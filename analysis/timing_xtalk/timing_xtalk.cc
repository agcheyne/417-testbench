// timing_xtalk - timing resolution and crosstalk of every channel in one run.
//
//   ./timing_xtalk.sh /scratch2/data/hpDIRC/test/test-00000070-0000.evt
//   root [1] set_signal_window(17, 35)          // from general's suggest_windows()
//   root [2] prun(20000)
//   root [3] timing_summary() ; draw_timing(20)
//   root [4] xtalk_summary() ; draw_xtalk(20) ; draw_xtalk_matrix() ; save()
//
// Timing, per channel (times relative to the channel's own DRS4 group trigger):
//  - CFD time: core sigma (Gaussian over +-2 sigma of the peak) and the late-tail fraction
//    (hits more than 3 core sigmas after the peak: backscattered photoelectrons etc.)
//  - leading-edge time at the hit threshold, which walks with amplitude: fitted with
//    t = a + b / amplitude (TProfile of the hits), then corrected and refitted.
// Crosstalk: events where one channel (the source) has a pulse above the source threshold and
// all others are below it. For every other channel without a hit of its own, its trace is
// averaged in a window around the source's CFD time (moved into that channel's DRS4 group by
// the difference of the group triggers). Noise averages away; what is left is the induced
// pulse. Its positive and negative peaks and its net charge are quoted relative to the
// average source pulse. Per-event peak finding would be biased by noise at this size (~2 mV).

#include <iostream>
#include <pmonitor/pmonitor.h>
#include "timing_xtalk.h"

#include "tb_db.h"
#include "tb_fit.h"
#include "tb_pulse.h"
#include "tb_v1742.h"

#include <TCanvas.h>
#include <TF1.h>
#include <TFile.h>
#include <TH1.h>
#include <TH2.h>
#include <TLegend.h>
#include <TProfile.h>
#include <TStyle.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace std;
using tb::NSIG;

int init_done = 0;

static const int PACKET = 2000;

// --- settings -------------------------------------------------------------------------------
static tb::Cuts cuts;
static double source_mv = 20.0, xt_before = 3.0, xt_after = 8.0;

void set_signal_window(double a, double b) { cuts.sig_from = a ; cuts.sig_to = b ; }
void set_threshold(double mv) { cuts.threshold = mv ; cuts.le_threshold = mv ; }
void set_cfd_fraction(double f) { cuts.cfd_fraction = f ; }
void set_source_threshold(double mv) { source_mv = mv ; }
void set_xtalk_window(double before, double after) { xt_before = before ; xt_after = after ; }

static tb::V1742Event ev;
static tb::RunInfo info;
static tb::Db db;
static bool db_ok = false;

// --- timing histograms ------------------------------------------------------------------------------
static bool booked = false;
static TH1F *h_cfd[NSIG], *h_le[NSIG], *h_le_corr[NSIG];
static TH2F *h_le_amp[NSIG], *h_cfd_amp[NSIG];
static TProfile *p_le_amp[NSIG];
static vector<pair<float, float>> le_hits[NSIG];   // (amplitude, LE time) for the walk correction
static double walk_a[NSIG], walk_b[NSIG];
static bool walk_done[NSIG];

// --- crosstalk ---------------------------------------------------------------------------------------
static TProfile *p_xt[NSIG][NSIG];   // [source][receiver]: average trace around the source CFD time
static long n_source[NSIG];

static void book()
{
  booked = true;
  double tlo = max(cuts.sig_from, -ev.trace_ns()), thi = min(cuts.sig_to, ev.trace_ns());
  int nt = max(10, int((thi - tlo) / 0.01));
  for (int ch = 0; ch < NSIG; ch++) {
    h_cfd[ch] = new TH1F(Form("cfd_%02d", ch), Form("ch %d CFD %.0f%% time - group trigger;t [ns];hits", ch, 100 * cuts.cfd_fraction), nt, tlo, thi);
    h_le[ch] = new TH1F(Form("le_%02d", ch), Form("ch %d leading-edge time at -%.0f mV;t [ns];hits", ch, cuts.le_threshold), nt, tlo, thi);
    h_le_corr[ch] = new TH1F(Form("le_corr_%02d", ch), Form("ch %d leading-edge time, walk corrected;t [ns];hits", ch), nt, tlo, thi);
    h_le_amp[ch] = new TH2F(Form("le_amp_%02d", ch), Form("ch %d leading-edge time vs amplitude;amplitude [mV];t [ns]", ch),
                            120, 0, 240, max(10, int((thi - tlo) / 0.02)), tlo, thi);
    h_cfd_amp[ch] = new TH2F(Form("cfd_amp_%02d", ch), Form("ch %d CFD time vs amplitude;amplitude [mV];t [ns]", ch),
                             120, 0, 240, max(10, int((thi - tlo) / 0.02)), tlo, thi);
    p_le_amp[ch] = new TProfile(Form("le_amp_prof_%02d", ch), Form("ch %d leading-edge time vs amplitude;amplitude [mV];t [ns]", ch), 120, 0, 240);
  }
}

int pinit()
{
  if (init_done) return 1;
  init_done = 1;
  return 0;
}

int process_event(Event *e)
{
  if (e->getEvtType() == 9) {
    info.parse(e);
    ev.begin_run(e);   // serial and calibration of this run's digitiser
    if (!db_ok || (!info.detector.empty() && info.detector != db.detector)) db_ok = db.load(info.detector);
    return 0;
  }
  if (!ev.fill(e)) return 0;
  if (!booked) book();

  tb::ChannelHit h[NSIG];
  int n_big = 0, src = -1;
  for (int ch = 0; ch < NSIG; ch++) {
    h[ch] = tb::measure(ev, ch, cuts);
    if (!h[ch].hit) continue;
    if (!std::isnan(h[ch].t_cfd)) {
      h_cfd[ch]->Fill(h[ch].t_cfd);
      h_cfd_amp[ch]->Fill(h[ch].amp, h[ch].t_cfd);
    }
    if (!std::isnan(h[ch].t_le)) {
      h_le[ch]->Fill(h[ch].t_le);
      h_le_amp[ch]->Fill(h[ch].amp, h[ch].t_le);
      p_le_amp[ch]->Fill(h[ch].amp, h[ch].t_le);
      le_hits[ch].emplace_back(h[ch].amp, h[ch].t_le);
    }
    if (h[ch].amp > source_mv) { n_big++ ; src = ch ; }
  }

  // crosstalk: exactly one big pulse in the event
  if (n_big != 1 || std::isnan(h[src].t_abs)) return 0;
  n_source[src]++;
  double t0_src = h[src].t_abs, trig_src = ev.trigger_of(src);
  for (int d = 0; d < NSIG; d++) {
    if (d != src && (!h[d].valid || h[d].hit)) continue;   // receivers: no hit of their own
    double trig_d = ev.trigger_of(d);
    if (std::isnan(trig_d)) continue;
    double t0 = t0_src - trig_src + trig_d;               // source time on receiver d's DRS4 group
    TProfile *&p = p_xt[src][d];
    if (!p) p = new TProfile(Form("xt_%02d_%02d", src, d), Form("ch %d average trace around ch %d pulses;t - t_{source} [ns];mV", d, src),
                             int((xt_before + xt_after) / 0.1), -xt_before, xt_after);
    int lo, hi;
    tb::window_around(ev.t[d], ev.ns, t0, -xt_before, xt_after, lo, hi);
    for (int s = lo; s < hi; s++) p->Fill(ev.t[d][s] - t0, ev.v[d][s]);
  }
  return 0;
}

// --- timing results ------------------------------------------------------------------------------------
static bool ready() { if (!booked) printf("timing_xtalk: no events yet - prun() first\n") ; return booked ; }

// t_LE = a + b / amplitude, fitted to the profile above twice the threshold, then applied hit by hit
static void walk_correct(int ch)
{
  if (walk_done[ch] || p_le_amp[ch]->GetEntries() < 200) return;
  walk_done[ch] = true;
  TF1 f("walk", "[0] + [1] / x", 2 * cuts.le_threshold, 240);
  f.SetParameters(p_le_amp[ch]->GetMean(2), 0);
  if (p_le_amp[ch]->Fit(&f, "RQ0N") != 0) return;
  walk_a[ch] = f.GetParameter(0);
  walk_b[ch] = f.GetParameter(1);
  h_le_corr[ch]->Reset();
  for (auto &ht : le_hits[ch]) h_le_corr[ch]->Fill(ht.second - walk_b[ch] / ht.first + walk_b[ch] / 50.0);   // referred to 50 mV
}

void timing_summary()
{
  if (!ready()) return;
  printf("run %d: times relative to each channel's DRS4 group trigger; sigma = Gaussian core\n", info.run);
  printf("  ch   hits     CFD t0 ns  sigma ps  tail>3s   LE sigma ps  LE walk-corr ps  walk b [ns mV]\n");
  for (int ch = 0; ch < NSIG; ch++) {
    if (h_cfd[ch]->GetEntries() < 200) continue;
    double m, s = tb::core_sigma(h_cfd[ch], m);
    double ml, sl = tb::core_sigma(h_le[ch], ml);
    walk_correct(ch);
    double mc, sc = tb::core_sigma(h_le_corr[ch], mc);
    printf("  %2d  %7.0f   %8.3f   %7.0f   %7.4f   %9.0f   %13.0f   %10.2f\n", ch, h_cfd[ch]->GetEntries(), m, 1000 * s,
           tb::tail_fraction(h_cfd[ch], m, s), 1000 * sl, 1000 * sc, walk_b[ch]);
  }
}

void draw_timing(int ch)
{
  if (!ready() || ch < 0 || ch >= NSIG) return;
  walk_correct(ch);
  auto *c = new TCanvas(Form("c_timing_%02d", ch), Form("ch %d timing", ch), 1200, 900);
  c->Divide(2, 2);
  double m, s = tb::core_sigma(h_cfd[ch], m);
  auto zoom = [&](TH1 *h) { if (!std::isnan(m)) h->GetXaxis()->SetRangeUser(m - 1.5, m + 4) ; };
  c->cd(1) ; gPad->SetLogy() ; zoom(h_cfd[ch]) ; h_cfd[ch]->Draw();
  c->cd(2) ; gPad->SetLogz() ; zoom(h_le_amp[ch]) ; h_le_amp[ch]->GetYaxis()->SetRangeUser(m - 2, m + 3) ; h_le_amp[ch]->Draw("colz");
  if (walk_done[ch]) {
    auto *f = new TF1(Form("walk_%02d", ch), "[0] + [1] / x", 2 * cuts.le_threshold, 240);
    f->SetParameters(walk_a[ch], walk_b[ch]);
    f->SetLineColor(kRed);
    f->Draw("same");
  }
  c->cd(3) ; gPad->SetLogy() ; zoom(h_le_corr[ch]) ; h_le_corr[ch]->Draw();
  c->cd(4) ; gPad->SetLogz() ; h_cfd_amp[ch]->GetYaxis()->SetRangeUser(m - 1, m + 2) ; h_cfd_amp[ch]->Draw("colz");
}

// --- crosstalk results ---------------------------------------------------------------------------------
struct Induced { int d ; double pos, neg, q ; };   // peaks in % of the source amplitude, net charge in % of the source charge

// Peaks and net charge are taken from -1 to +4 ns around the source pulse (crosstalk is induced
// at the same time; real light in a receiver, e.g. on the other DRS4 board's cabling, arrives
// earlier or later), above the trace's level at +6 to +8 ns, where everything has settled.
static const double XT_FROM = -1.0, XT_TO = 4.0, BASE_FROM = 6.0, BASE_TO = 8.0;

struct Shape { double base, max, min, integral ; };
static Shape shape(TProfile *p)
{
  Shape s{0, -1e9, 1e9, 0};
  int nb = 0;
  for (int b = 1; b <= p->GetNbinsX(); b++) {
    double c = p->GetBinCenter(b);
    if (c >= BASE_FROM && c < BASE_TO && p->GetBinEntries(b) > 0) { s.base += p->GetBinContent(b) ; nb++ ; }
  }
  s.base = nb ? s.base / nb : 0;
  for (int b = 1; b <= p->GetNbinsX(); b++) {
    double c = p->GetBinCenter(b);
    if (c < XT_FROM || c >= XT_TO || p->GetBinEntries(b) == 0) continue;
    double v = p->GetBinContent(b) - s.base;
    s.max = max(s.max, v) ; s.min = min(s.min, v) ; s.integral += v * p->GetBinWidth(b);
  }
  return s;
}

static vector<Induced> induced(int src)
{
  vector<Induced> out;
  TProfile *ps = p_xt[src][src];
  if (!ps || n_source[src] < 100) return out;
  Shape so = shape(ps);
  double amp = -so.min, qs = so.integral;
  for (int d = 0; d < NSIG; d++) {
    TProfile *p = p_xt[src][d];
    if (d == src || !p || p->GetEntries() < 1000) continue;
    Shape sd = shape(p);
    out.push_back({d, 100 * sd.max / amp, 100 * -sd.min / amp, 100 * sd.integral / qs});
  }
  sort(out.begin(), out.end(), [](const Induced &a, const Induced &b) { return max(a.pos, a.neg) > max(b.pos, b.neg) ; });
  return out;
}

void xtalk_summary(int source)
{
  if (!ready()) return;
  for (int s = 0; s < NSIG; s++) {
    if ((source >= 0 && s != source) || n_source[s] < 100 || !p_xt[s][s]) continue;
    printf("source ch %d: %ld clean events, average pulse %.1f mV; peaks from %.0f to %+.0f ns around it\n", s, n_source[s],
           -shape(p_xt[s][s]).min, XT_FROM, XT_TO);
    printf("   ch   +peak %%  -peak %%  net charge %%\n");
    for (auto &i : induced(s)) printf("   %2d  %7.2f  %7.2f  %8.2f\n", i.d, i.pos, i.neg, i.q);
  }
  printf("(induced crosstalk is bipolar: net charge near 0. A clearly positive net charge is real light below the hit\n"
         " threshold. Unconnected channels show the noise floor.)\n");
}

void draw_xtalk(int src, int n)
{
  if (!ready() || src < 0 || src >= NSIG || !p_xt[src][src]) { printf("timing_xtalk: no clean events with source ch %d\n", src) ; return ; }
  auto *c = new TCanvas(Form("c_xt_%02d", src), Form("crosstalk from ch %d", src), 1000, 700);
  c->Divide(1, 2);
  c->cd(1);
  p_xt[src][src]->SetLineColor(kBlack);
  p_xt[src][src]->SetTitle(Form("ch %d: average source pulse;t - t_{source} [ns];mV", src));
  p_xt[src][src]->Draw("hist");
  c->cd(2);
  auto list = induced(src);
  auto *leg = new TLegend(0.75, 0.55, 0.95, 0.95);
  double lo = 0, hi = 0;
  for (int k = 0; k < n && k < (int)list.size(); k++) {
    TProfile *p = p_xt[src][list[k].d];
    lo = min(lo, p->GetMinimum()) ; hi = max(hi, p->GetMaximum());
  }
  for (int k = 0; k < n && k < (int)list.size(); k++) {
    TProfile *p = p_xt[src][list[k].d];
    p->SetLineColor(k + 2 == 5 ? 46 : k + 2);   // skip yellow
    p->SetMinimum(1.1 * lo) ; p->SetMaximum(1.1 * hi);
    p->SetTitle(Form("induced in the other channels (average);t - t_{source} [ns];mV"));
    p->Draw(k ? "hist same" : "hist");
    leg->AddEntry(p, Form("ch %d: %+.1f / -%.1f %%", list[k].d, list[k].pos, list[k].neg), "l");
  }
  leg->Draw();
}

static TH2F *xt_matrix()
{
  auto *m = new TH2F("xtalk_matrix", "negative induced peak, % of the source pulse;source channel;receiver channel",
                     NSIG, -0.5, NSIG - 0.5, NSIG, -0.5, NSIG - 0.5);
  for (int s = 0; s < NSIG; s++)
    for (auto &i : induced(s)) m->SetBinContent(s + 1, i.d + 1, i.neg);
  return m;
}

void draw_xtalk_matrix()
{
  if (!ready()) return;
  auto *c = new TCanvas("c_xt_matrix", "crosstalk matrix", 800, 700);
  c->SetRightMargin(0.15);
  gStyle->SetOptStat(0);
  TH2F *m = xt_matrix();
  m->Draw("colz");
  // pixel neighbourhood of a source, when the detector's mapping and cabling are known
  int row, col;
  if (db_ok && db.pixel(PACKET, 0, row, col)) printf("(mapping known: receivers can be placed on the pixel grid with tb::Db::pixel)\n");
}

void save(const char *filename)
{
  if (!ready()) return;
  string name = (filename && *filename) ? filename : Form("timing_xtalk_run%d.root", info.run);
  TFile f(name.c_str(), "RECREATE");
  for (int ch = 0; ch < NSIG; ch++) {
    walk_correct(ch);
    h_cfd[ch]->Write() ; h_le[ch]->Write() ; h_le_corr[ch]->Write() ; h_le_amp[ch]->Write() ; h_cfd_amp[ch]->Write() ; p_le_amp[ch]->Write();
  }
  for (int s = 0; s < NSIG; s++)
    for (int d = 0; d < NSIG; d++)
      if (p_xt[s][d] && n_source[s] >= 100) p_xt[s][d]->Write();
  xt_matrix()->Write();
  f.Close();
  printf("timing_xtalk: wrote %s\n", name.c_str());
}
