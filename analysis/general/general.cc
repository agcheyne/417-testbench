// general - first-look analysis of 417 test-bench runs (CAEN V1742, packet 2000).
//
//   ./general.sh /scratch2/data/hpDIRC/HRPPD/HRPPD-00000123-0000.evt
//   root [1] suggest_windows()          (after a first prun(5000): where are the pulses?)
//   root [2] set_signal_window(20, 40) ; set_charge_window(25, 32)
//   root [3] prun(20000)
//   root [4] summary() ; draw_channel(20) ; fit_gain(20) ; draw_maps() ; save()
//
// Per event: DRS4 calibration (picked from the board's own FREQUENCY code and the V1742 serial
// in begin-run packet 921), baseline from the median of early samples, DRS4 spike removal,
// then for each group the trigger time (50 % CFD on its TR trace) and for each channel:
// amplitude and CFD time inside the signal window, charge in the charge window, and late
// pulses after the main pulse. All times are relative to the channel's OWN group trigger:
// groups 0/1 and 2/3 sit on different DRS4 chips whose clocks are not aligned (3.4 ns rms).
// The begin-run packets give the detector (910 -> db mapping, for the pixel map), the scan
// point (911) and the stage position (932, else 931).
// Helpers: lib/tb_pulse.h (waveform), lib/tb_spe.h (gain fit), lib/tb_db.h (database).

#include <iostream>
#include <pmonitor/pmonitor.h>
#include "general.h"

#include "tb_db.h"
#include "tb_fit.h"
#include "tb_pulse.h"
#include "tb_spe.h"
#include "tb_v1742.h"

#include <TCanvas.h>
#include <TF1.h>
#include <TFile.h>
#include <TH1.h>
#include <TH2.h>
#include <TStyle.h>
#include <TSystem.h>
#include <TTree.h>

#include <cmath>
#include <cstdio>
#include <string>

using namespace std;
using tb::NSIG;
using tb::NTRIG;

int init_done = 0;

static const int PACKET = 2000;

// --- settings ---------------------------------------------------------------------------
static tb::Cuts cuts;
static double late_from = 5.0, late_thr = 5.0;
static bool want_tree = false;
static tb::V1742Event ev;
static tb::RunInfo info;

void set_signal_window(double a, double b) { cuts.sig_from = a ; cuts.sig_to = b ; }
void set_threshold(double mv) { cuts.threshold = mv ; cuts.le_threshold = mv ; }
void set_cfd_fraction(double f) { cuts.cfd_fraction = f ; }
void set_charge_window(double a, double b) { cuts.q_from = a ; cuts.q_to = b ; }
void set_late_pulse_search(double from_ns, double mv) { late_from = from_ns ; late_thr = mv ; }
void set_baseline_samples(int a, int b) { ev.bl_first = a ; ev.bl_last = b ; }
void set_despike(bool on) { ev.despike = on ; }
void set_tree(bool on) { want_tree = on ; }

// --- detector from the begin-run event ------------------------------------------------------
static tb::Db db;
static bool db_ok = false;

// --- histograms ---------------------------------------------------------------------------
static bool booked = false;
static TH1F *h_amp[NSIG], *h_q[NSIG], *h_t[NSIG], *h_late[NSIG], *h_where[NSIG];
static TH1F *h_occ = nullptr;
static TH2F *h_map = nullptr;
static long n_valid[NSIG], n_hit[NSIG], n_late[NSIG];
static double sum_amp[NSIG];
static long n_badtrig[NTRIG];
static TTree *tree = nullptr;
static struct { int run, event, ch, row, col, point, nlate ; float x_mm, y_mm, amp, q, t, trig ; } tr;

static void book(double trace_ns)
{
  booked = true;
  double tlo = max(cuts.sig_from, -trace_ns), thi = min(cuts.sig_to, trace_ns);
  for (int ch = 0; ch < NSIG; ch++) {
    h_amp[ch] = new TH1F(Form("amp_%02d", ch), Form("ch %d amplitude in the signal window;amplitude [mV];events", ch), 500, -10, 240);
    h_q[ch] = new TH1F(Form("q_%02d", ch), Form("ch %d charge;charge [pC];events", ch), 1600, -0.2, 3.0);
    h_t[ch] = new TH1F(Form("t_%02d", ch), Form("ch %d hit time (CFD %.0f%%) - group trigger;t [ns];hits", ch, 100 * cuts.cfd_fraction),
                       max(10, int((thi - tlo) / 0.01)), tlo, thi);
    h_late[ch] = new TH1F(Form("late_%02d", ch), Form("ch %d late pulses after the main pulse;#Deltat [ns];pulses", ch),
                          int(trace_ns / 0.5), 0, trace_ns);
    h_where[ch] = new TH1F(Form("where_%02d", ch), Form("ch %d deepest point below threshold, whole trace - trigger;t [ns];events", ch),
                           int(2 * trace_ns / 0.5), -trace_ns, trace_ns);
  }
  h_occ = new TH1F("occupancy", "hits per event with a valid trigger;channel;occupancy", NSIG, -0.5, NSIG - 0.5);
  int nr = int(db.num("NROWS", 0)), nc = int(db.num("NCOLS", 0));
  if (db_ok && nr > 0 && nc > 0 && !db.by_packet.empty())
    h_map = new TH2F("pixel_map", Form("%s hits per pixel;col;row", db.detector.c_str()), nc, -0.5, nc - 0.5, nr, -0.5, nr - 0.5);
  else
    printf("general: no pixel map (%s)\n", db_ok ? "NROWS/NCOLS or cabling.txt not filled in" : "detector not in the db");
  if (want_tree) {
    tree = new TTree("hits", "one entry per hit");
    tree->Branch("run", &tr.run) ; tree->Branch("event", &tr.event) ; tree->Branch("ch", &tr.ch);
    tree->Branch("row", &tr.row) ; tree->Branch("col", &tr.col) ; tree->Branch("point", &tr.point);
    tree->Branch("x_mm", &tr.x_mm) ; tree->Branch("y_mm", &tr.y_mm);
    tree->Branch("amp", &tr.amp) ; tree->Branch("q", &tr.q) ; tree->Branch("t", &tr.t);
    tree->Branch("trig", &tr.trig) ; tree->Branch("nlate", &tr.nlate);
  }
}

int pinit()
{
  if (init_done) return 1;
  init_done = 1;
  return 0;
}

static void begin_run(Event *e)
{
  info.parse(e);
  ev.serial = info.serial;   // calibration file of this run's digitiser
  if (!db_ok || (!info.detector.empty() && info.detector != db.detector)) {
    db_ok = db.load(info.detector);
    printf("general: run %d, detector %s%s\n", info.run, db_ok ? db.detector.c_str() : "unknown",
           info.detector.empty() ? " (from testbench.sh use; not in the data)" : "");
  }
}

int process_event(Event *e)
{
  if (e->getEvtType() == 9) { begin_run(e) ; return 0 ; }
  if (!ev.fill(e)) return 0;
  if (!booked) book(ev.trace_ns());
  for (int g = 0; g < NTRIG; g++) if (std::isnan(ev.trig[g])) n_badtrig[g]++;

  for (int ch = 0; ch < NSIG; ch++) {
    tb::ChannelHit h = tb::measure(ev, ch, cuts);
    if (!h.valid) continue;
    const double *tc = ev.t[ch], *vc = ev.v[ch];
    double tg = ev.trigger_of(ch);
    n_valid[ch]++;
    h_amp[ch]->Fill(h.amp);
    h_q[ch]->Fill(h.q);
    int iw = tb::min_index(vc, 0, ev.ns);
    if (vc[iw] < -cuts.threshold) h_where[ch]->Fill(tc[iw] - tg);
    if (!h.hit) continue;

    n_hit[ch]++;
    sum_amp[ch] += h.amp;
    if (!std::isnan(h.t_cfd)) h_t[ch]->Fill(h.t_cfd);
    double tmain = std::isnan(h.t_abs) ? tc[h.imin] : h.t_abs;
    int llo, lhi;
    tb::window_around(tc, ev.ns, tmain, late_from, 1e9, llo, lhi);
    auto late = tb::find_pulses(tc, vc, llo, lhi, -late_thr);
    for (auto &lp : late) h_late[ch]->Fill(lp.time - tmain);
    if (!late.empty()) n_late[ch]++;

    int row = -1, col = -1;
    bool mapped = db_ok && db.pixel(PACKET, ch, row, col);
    if (mapped && h_map) h_map->Fill(col, row);
    if (tree) {
      tr.run = info.run ; tr.event = e->getEvtSequence() ; tr.ch = ch ; tr.row = row ; tr.col = col;
      tr.point = info.point ; tr.x_mm = info.x ; tr.y_mm = info.y;
      tr.amp = h.amp ; tr.q = h.q ; tr.t = h.t_cfd ; tr.trig = tg ; tr.nlate = late.size();
      tree->Fill();
    }
  }
  return 0;
}

// --- results ------------------------------------------------------------------------------
static bool ready() { if (!booked) printf("general: no events yet - prun() first\n") ; return booked ; }

static double core_sigma(TH1 *h, double &mean) { return tb::core_sigma(h, mean) ; }

void summary()
{
  if (!ready()) return;
  printf("run %d, %ld events, detector %s, stage %.3f %.3f mm, scan point %d\n", info.run, ev.n_events,
         db_ok ? db.detector.c_str() : "unknown", info.x, info.y, info.point);
  for (int g = 0; g < NTRIG; g++)
    if (n_badtrig[g]) printf("  group %d: no trigger found in %ld events\n", g, n_badtrig[g]);
  printf("  ch  pixel    occupancy  <amp> mV   t0 ns   sigma ps   late-pulse frac\n");
  for (int ch = 0; ch < NSIG; ch++) {
    if (!n_valid[ch]) continue;
    h_occ->SetBinContent(ch + 1, double(n_hit[ch]) / n_valid[ch]);
    if (!n_hit[ch]) continue;
    int row, col;
    char px[16] = "-";
    if (db_ok && db.pixel(PACKET, ch, row, col)) snprintf(px, sizeof px, "%d,%d", row, col);
    double m, s = core_sigma(h_t[ch], m);
    printf("  %2d  %-7s  %8.4f  %8.1f  %8.3f  %8.0f   %8.4f\n", ch, px, double(n_hit[ch]) / n_valid[ch],
           sum_amp[ch] / n_hit[ch], m, 1000 * s, double(n_late[ch]) / n_hit[ch]);
  }
}

void suggest_windows()
{
  if (!ready()) return;
  // the window follows the busiest channel; peaks more than 15 ns from it are listed but
  // ignored (unconnected channels can show baseline dips near the end of the trace)
  int best = -1;
  for (int ch = 0; ch < NSIG; ch++)
    if (n_valid[ch] && (best < 0 || h_where[ch]->GetEntries() > h_where[best]->GetEntries())) best = ch;
  if (best < 0 || h_where[best]->GetEntries() < 0.01 * n_valid[best]) {
    printf("no channel has pulses below -%.1f mV: lower set_threshold(), or no light?\n", cuts.threshold);
    return;
  }
  double ref = h_where[best]->GetBinCenter(h_where[best]->GetMaximumBin()), lo = ref, hi = ref;
  printf("deepest point below -%.1f mV, relative to the group trigger (channels with > 1 %% of events):\n", cuts.threshold);
  for (int ch = 0; ch < NSIG; ch++) {
    TH1 *h = h_where[ch];
    if (!n_valid[ch] || h->GetEntries() < 0.01 * n_valid[ch]) continue;
    double pk = h->GetBinCenter(h->GetMaximumBin());
    bool far = fabs(pk - ref) > 15;
    printf("  ch %2d: peak at %7.2f ns (%5.1f %% of events)%s\n", ch, pk, 100.0 * h->GetEntries() / n_valid[ch],
           far ? "   far from the others: ignored" : "");
    if (!far) { lo = min(lo, pk) ; hi = max(hi, pk) ; }
  }
  printf("suggestion: set_signal_window(%.0f, %.0f) ; set_charge_window(%.1f, %.1f)\n",
         floor(lo - 5), ceil(hi + 10), lo - 2.0, hi + 4.0);
}

void fit_gain(int ch)
{
  if (!ready() || ch < 0 || ch >= NSIG) return;
  if (std::isnan(cuts.q_from)) printf("note: no set_charge_window(): the charge is taken around the deepest point of each\n"
                                 "      event, which biases the pedestal - set a fixed window for a real gain fit\n");
  tb::SpeFit f = tb::fit_spe(h_q[ch]);
  if (!f.f) { printf("ch %d: too few entries\n", ch) ; return ; }
  printf("ch %d SPE fit %s: Q1 = %.4f +- %.4f pC, gain = %.3g +- %.2g, mu = %.3f +- %.3f p.e., "
         "pedestal %.4f +- %.4f pC (sigma %.4f), chi2/ndf %.2f\n", ch, f.ok ? "ok" : "DID NOT CONVERGE",
         f.q1, f.eq1, f.gain, f.egain, f.mu, f.emu, f.q0, f.eq0, f.s0, f.chi2ndf);
  if (!f.resolved)
    printf("  WARNING: Q1 is less than 3 pedestal sigmas: no real photoelectron peak (no light, or noise)\n");
  if (f.mu > 3)
    printf("  WARNING: mu > 3 leaves almost no pedestal events (P(0) = %.1g), so the pedestal and gain are\n"
           "  poorly constrained - for a gain measurement lower the light to mu ~ 0.1-1\n", exp(-f.mu));
  auto *c = new TCanvas(Form("c_gain_%02d", ch), Form("ch %d gain", ch), 800, 600);
  c->SetLogy();
  h_q[ch]->Draw();
  f.f->SetLineColor(kRed);
  f.f->Draw("same");
}

void draw_channel(int ch)
{
  if (!ready() || ch < 0 || ch >= NSIG) return;
  auto *c = new TCanvas(Form("c_ch_%02d", ch), Form("channel %d", ch), 1200, 900);
  c->Divide(2, 2);
  c->cd(1) ; gPad->SetLogy() ; h_amp[ch]->Draw();
  c->cd(2) ; gPad->SetLogy() ; h_q[ch]->Draw();
  c->cd(3) ; gPad->SetLogy() ; h_t[ch]->Draw();
  double m, s = core_sigma(h_t[ch], m);
  if (!std::isnan(s)) h_t[ch]->GetXaxis()->SetRangeUser(m - 2, m + 5);
  c->cd(4) ; gPad->SetLogy() ; h_late[ch]->Draw();
}

void draw_maps()
{
  if (!ready()) return;
  summary();
  auto *c = new TCanvas("c_maps", "occupancy", h_map ? 1200 : 700, 600);
  if (h_map) {
    c->Divide(2, 1);
    c->cd(1);
    gStyle->SetOptStat(0);
    h_map->Draw("colz");
    c->cd(2);
  }
  h_occ->Draw("hist");
}

void save(const char *filename)
{
  if (!ready()) return;
  summary();
  string name = (filename && *filename) ? filename : Form("general_run%d.root", info.run);
  TFile f(name.c_str(), "RECREATE");
  for (int ch = 0; ch < NSIG; ch++) { h_amp[ch]->Write() ; h_q[ch]->Write() ; h_t[ch]->Write() ; h_late[ch]->Write() ; h_where[ch]->Write() ; }
  h_occ->Write();
  if (h_map) h_map->Write();
  if (tree) tree->Write();
  f.Close();
  printf("general: wrote %s\n", name.c_str());
}
