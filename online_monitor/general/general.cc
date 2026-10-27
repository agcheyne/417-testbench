// online_monitor/general - live view of the V1742 data on the 417 test bench.
//
// Online, while rcdaq takes data:
//   ./general.sh
//   root [1] online()                  // rcdaqopen() + pstart() + draw(): the main canvas, refreshed every 2 s
//   root [2] draw_details() ; draw_noise()     // the other canvases, when wanted
// From a file (testing, or a look at a finished run):
//   ./general.sh /scratch2/data/hpDIRC/photonis/photonis-00000111-0000.evt
//   root [1] pstart() ; draw()         // runs through the file in the background, canvases refresh
//   root [1] prun(5000) ; draw()       // or a fixed number of events first
// Then any time: watch_channel(20) (-1: follow the laser again), set_waveform_mode("average"),
// set_waveform_time(-50, 150), set_cluster(10, 5, 1, 0.5), reset(), status(), save_plots("run111").
//
// Canvases:
//   main       (draw)          hit rate per pixel; for the watched channel: persistence (every
//                              event, scope style), average waveform with its neighbours and TR,
//                              charge spectrum (every event: pedestal + photoelectrons), hit time
//                              with its core sigma; trends over the last 30 min (10 s steps of
//                              data time, kept across runs): event rate, hits per event, mean
//                              pulse height, mean hit time
//   face       (draw_details)  mean pulse height of the hits per pixel, hit rate per pixel
//   clusters   (draw_details)  the latest cluster, cluster centroids, cluster size, summed charge,
//                              seed time, and the neighbour - seed time of every candidate
//   waveforms  (draw_details)  one pad per DRS4 group: its 8 channels and its trigger trace (TR / 10)
//   noise      (draw_noise)    baseline rms per channel (TR0-3 as 32-35), mean of every event per
//                              pixel (pedestal face), trigger found per group, hit rate per channel
//                              (also the unmapped ones): for checks with the laser off
//
// The watched channel follows the laser: the channel with the most in-time hits (within 1 ns
// of the hit-time peak, so pickup and dark counts do not win), re-chosen every 10 s.
// The face uses the detector's mapping + cabling from the db (the detector in the begin-run
// packet 910, else the one selected with testbench.sh use); without them, channels are laid
// out 8 per row, one row per DRS4 group.
// Clusters: the seed is the highest pulse (>= seed threshold); neighbours are hits within the
// range of the seed (pixels, square) and in time with it. Times are relative to each channel's
// own group trigger, and the two DRS4 boards still differ by ~2 ns there, so each group is first
// shifted to put its hit-time peak at 0 (from the data: every 200 events at first, then every 1000).
// The event loop (pstart) only fills; the canvases are redrawn by a timer in ROOT's main thread.

#include <iostream>
#include <pmonitor/pmonitor.h>
#include "general.h"

#include "tb_db.h"
#include "tb_fit.h"
#include "tb_pulse.h"
#include "tb_v1742.h"

#include <TCanvas.h>
#include <TGraph.h>
#include <TH1.h>
#include <TH2.h>
#include <TLatex.h>
#include <TLegend.h>
#include <TProfile.h>
#include <TProfile2D.h>
#include <TROOT.h>
#include <TStyle.h>
#include <TSystem.h>
#include <TTimer.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

using namespace std;
using tb::NCH;
using tb::NSIG;
using tb::NTRIG;

int init_done = 0;

static const int PACKET = 2000;
static const double TR_SCALE = 0.1;      // trigger traces (NIM, ~ -800 mV) drawn / 10
static const int TREND_EVERY = 10, TREND_KEEP = 1800;            // s of data time
static const int TREND_N = TREND_KEEP / TREND_EVERY + 2;         // points per trend graph

// --- settings ---------------------------------------------------------------------------
static tb::Cuts cuts = [] { tb::Cuts c ; c.sig_from = 17 ; c.sig_to = 35 ; return c ; }();
static double seed_mv = 8, neighbour_mv = 5, cluster_dt = 1.0;
static int cluster_range = 1;
static bool wave_average = false;
static double wave_min = -100, wave_max = 20;
static double wave_from = NAN, wave_to = NAN;   // ns from the group trigger; NaN = -10 .. signal window + 20
static bool reset_each_run = true;
static int watch_fixed = -1;                    // channel on the main canvas; -1 = follow the laser

static tb::V1742Event ev;
static tb::RunInfo info;
static tb::Db db;
static bool db_ok = false;

// --- face layout: channel -> pixel ---------------------------------------------------------------
static int pix_row[NSIG], pix_col[NSIG];
static int nrows = 4, ncols = 8;
static bool by_pixel = false;

static void layout()
{
  by_pixel = false;
  int maxr = 0, maxc = 0;
  for (int ch = 0; ch < NSIG; ch++) {
    pix_row[ch] = pix_col[ch] = -1;
    int r, c;
    if (db_ok && db.pixel(PACKET, ch, r, c)) { pix_row[ch] = r ; pix_col[ch] = c ; by_pixel = true ; maxr = max(maxr, r) ; maxc = max(maxc, c) ; }
  }
  if (by_pixel) {
    nrows = max(int(db.num("NROWS", 0)), maxr + 1);
    ncols = max(int(db.num("NCOLS", 0)), maxc + 1);
    return;
  }
  for (int ch = 0; ch < NSIG; ch++) { pix_row[ch] = ch / 8 ; pix_col[ch] = ch % 8 ; }
  nrows = 4 ; ncols = 8;
}

// --- histograms -------------------------------------------------------------------------------
static bool booked = false;
static TProfile2D *face_amp = nullptr, *face_occ = nullptr, *face_all = nullptr;
static TH2F *clu_last = nullptr, *clu_centroid = nullptr;
static TH1F *clu_size = nullptr, *clu_q = nullptr, *clu_t = nullptr, *clu_dt = nullptr;
static TH1F *h_tgroup[NTRIG];            // hit times per group, to line the groups up
static double grp_off[NTRIG], t_peak = NAN;   // group offsets; hit-time peak after lining up
static TGraph *g_wave[NCH];              // latest event
static TProfile *p_wave[NCH];            // average of the hit events
static TH1F *wave_frame[NTRIG];
static TH1F *h_qch[NSIG], *h_tch[NSIG];  // per channel: charge of every event, hit time
static long n_events = 0, n_clusters = 0;
static chrono::steady_clock::time_point last_wave, last_cluster;

// main canvas: the watched channel (copies of its histograms, refreshed from the event loop)
static int watched = -1;
static bool watched_changed = false;
static TH2F *persist = nullptr;
static TH1F *w_q = nullptr, *w_t = nullptr, *w_frame = nullptr;
static const int NW = 10;                // watched + up to 8 neighbours + its TR
static TProfile *w_avg[NW];
static int w_avg_ch[NW];

// trends: one point every TREND_EVERY s of data time
static TGraph *trend[4];
static TH1F *trend_frame[4];
static vector<pair<double, array<double, 4>>> trend_pts;
static struct Interval { time_t start = 0 ; long events = 0, hits = 0, timed = 0, intime[NSIG] = {} ; double amp = 0, time = 0 ; } iv;
static double last_rate = NAN;

// noise canvas
static TProfile *noise_rms = nullptr, *noise_hits = nullptr, *trig_found = nullptr;

static double trig_of(int ch) { return ch < NSIG ? ev.trigger_of(ch) : ev.trig[ch - NSIG]; }
static void wave_xrange();

static const char *xtitle() { return by_pixel ? "col" : "channel % 8"; }
static const char *ytitle() { return by_pixel ? "row" : "DRS4 group"; }

static void face_titles()
{
  face_amp->SetTitle(Form("%s: mean pulse height of the hits (> %.1f mV);%s;%s;mV", db_ok ? db.detector.c_str() : "?", cuts.threshold,
                          xtitle(), ytitle()));
  face_occ->SetTitle(Form("hit rate (> %.1f mV, %.0f to %.0f ns);%s;%s;hits / event", cuts.threshold, cuts.sig_from, cuts.sig_to,
                          xtitle(), ytitle()));
  noise_hits->SetTitle(Form("hit rate per channel (> %.1f mV, %.0f to %.0f ns);channel;hits / event", cuts.threshold, cuts.sig_from, cuts.sig_to));
}

static void cluster_titles()
{
  clu_size->SetTitle(Form("pixels per cluster (seed > %.0f mV, neighbours > %.0f mV within %d px and %.1f ns);pixels;clusters",
                          seed_mv, max(neighbour_mv, cuts.threshold), cluster_range, cluster_dt));
}

static void book()
{
  layout();
  const char *xt = xtitle(), *yt = ytitle();
  double xl = -0.5, xh = ncols - 0.5, yl = -0.5, yh = nrows - 0.5;
  face_amp = new TProfile2D("face_amp", "", ncols, xl, xh, nrows, yl, yh);
  face_occ = new TProfile2D("face_occ", "", ncols, xl, xh, nrows, yl, yh);
  face_all = new TProfile2D("face_all", Form("deepest point in the signal window, mean of every event (pedestal);%s;%s;mV", xt, yt),
                            ncols, xl, xh, nrows, yl, yh);
  noise_hits = new TProfile("noise_hits", "", NSIG, -0.5, NSIG - 0.5);
  face_titles();
  clu_last = new TH2F("cluster_last", Form("latest cluster;%s;%s;mV", xt, yt), ncols, xl, xh, nrows, yl, yh);
  clu_last->SetMarkerSize(2.5);   // size of the mV numbers
  clu_centroid = new TH2F("cluster_centroid", Form("cluster centroids (pulse-height weighted);%s;%s;clusters", xt, yt),
                          4 * ncols, xl, xh, 4 * nrows, yl, yh);
  clu_size = new TH1F("cluster_size", "", 9, 0.5, 9.5);
  cluster_titles();
  clu_q = new TH1F("cluster_charge", "summed charge of the cluster;charge [pC];clusters", 400, -0.2, 6);
  double tr = ev.trace_ns(), tlo = max(cuts.sig_from, -tr), thi = min(cuts.sig_to, tr);
  clu_t = new TH1F("cluster_time", "seed time - group trigger (groups lined up on group 0);t [ns];clusters",
                   max(10, int((thi - tlo) / 0.02)), tlo, thi);
  clu_dt = new TH1F("cluster_dt", "neighbour - seed time, before the time cut;#Deltat [ns];neighbours", 200, -5, 5);
  for (int g = 0; g < NTRIG; g++) {
    h_tgroup[g] = new TH1F(Form("tgroup_%d", g), "", max(10, int((thi - tlo) / 0.05)), tlo, thi);
    h_tgroup[g]->SetDirectory(nullptr);
    grp_off[g] = 0;
  }
  for (int ch = 0; ch < NSIG; ch++) {
    h_qch[ch] = new TH1F(Form("q_%02d", ch), Form("ch %d charge, every event;charge [pC];events", ch), 400, -0.2, 3.0);
    h_tch[ch] = new TH1F(Form("t_%02d", ch), Form("ch %d hit time - group trigger;t [ns];hits", ch), max(10, int((thi - tlo) / 0.01)), tlo, thi);
  }

  // waveforms: time relative to the group trigger, range from this first event
  double tg = std::isnan(ev.trig[0]) ? 0 : ev.trig[0];
  double wl = ev.t[0][0] - tg, wh = ev.t[0][ev.ns - 1] - tg;
  int nb = max(10, int((wh - wl) / ev.ns_per_sample));
  static const int colors[8] = {kRed + 1, kOrange + 7, kYellow + 2, kGreen + 2, kCyan + 2, kBlue + 1, kViolet + 1, kMagenta + 1};
  for (int ch = 0; ch < NCH; ch++) {
    int col = ch < NSIG ? colors[ch % 8] : kBlack;
    g_wave[ch] = new TGraph(ev.ns);
    g_wave[ch]->SetLineColor(col);
    g_wave[ch]->SetLineWidth(2);
    p_wave[ch] = new TProfile(Form("wave_avg_%02d", ch), "", nb, wl, wh);
    p_wave[ch]->SetLineColor(col);
    p_wave[ch]->SetLineWidth(2);
  }
  for (int g = 0; g < NTRIG; g++) {
    wave_frame[g] = new TH1F(Form("wave_frame_%d", g), "", 100, wl, wh);
    wave_frame[g]->SetMinimum(wave_min);
    wave_frame[g]->SetMaximum(wave_max);
  }

  // main canvas
  persist = new TH2F("persistence", "", nb, wl, wh, 150, wave_min, wave_max);
  w_q = new TH1F("watch_q", "", 400, -0.2, 3.0);
  w_t = new TH1F("watch_t", "", max(10, int((thi - tlo) / 0.01)), tlo, thi);
  w_frame = new TH1F("watch_frame", "", 100, wl, wh);
  w_frame->SetMinimum(wave_min);
  w_frame->SetMaximum(wave_max);
  for (int k = 0; k < NW; k++) { w_avg[k] = new TProfile(Form("watch_avg_%d", k), "", nb, wl, wh) ; w_avg_ch[k] = -1 ; }
  for (int i = 0; i < 4; i++) {
    trend_frame[i] = new TH1F(Form("trend_frame_%d", i), "", 1, 0, 1);
    trend_frame[i]->SetDirectory(nullptr);
    TAxis *a = trend_frame[i]->GetXaxis();
    a->SetTimeDisplay(1) ; a->SetTimeFormat("%H:%M") ; a->SetTimeOffset(0, "local");
    a->SetLabelSize(0.13) ; trend_frame[i]->GetYaxis()->SetLabelSize(0.13) ; trend_frame[i]->GetYaxis()->SetNdivisions(504);
    trend[i] = new TGraph(TREND_N);       // fixed size, never reallocated while drawn
    trend[i]->SetLineColor(kBlue + 1) ; trend[i]->SetMarkerColor(kBlue + 1) ; trend[i]->SetMarkerStyle(20) ; trend[i]->SetMarkerSize(0.4);
  }

  // noise canvas
  noise_rms = new TProfile("noise_rms", Form("baseline rms (samples %d-%d), TR0-3 = 32-35;channel;mV", ev.bl_first, ev.bl_last), NCH, -0.5, NCH - 0.5);
  trig_found = new TProfile("trigger_found", "events with the group trigger found;DRS4 group;fraction", NTRIG, -0.5, NTRIG - 0.5);
  booked = true;
  wave_xrange();
}

static void wave_xrange()
{
  double lo = std::isnan(wave_from) ? -10 : wave_from, hi = std::isnan(wave_to) ? cuts.sig_to + 20 : wave_to;
  for (int g = 0; g < NTRIG; g++) wave_frame[g]->GetXaxis()->SetRangeUser(lo, hi);
  w_frame->GetXaxis()->SetRangeUser(lo, hi);
  persist->GetXaxis()->SetRangeUser(lo, hi);
}

static void wave_titles()
{
  for (int g = 0; g < NTRIG; g++)
    wave_frame[g]->SetTitle(Form("group %d, ch %d-%d + TR%d / 10: %s;t - group trigger [ns];mV", g, 8 * g, 8 * g + 7, g,
                                 wave_average ? "average of the hit events" : "latest event"));
}

void reset()
{
  if (!booked) return;
  face_amp->Reset() ; face_occ->Reset() ; face_all->Reset();
  clu_last->Reset() ; clu_centroid->Reset() ; clu_size->Reset() ; clu_q->Reset() ; clu_t->Reset() ; clu_dt->Reset();
  for (int g = 0; g < NTRIG; g++) h_tgroup[g]->Reset();
  for (int ch = 0; ch < NCH; ch++) p_wave[ch]->Reset();
  for (int ch = 0; ch < NSIG; ch++) { h_qch[ch]->Reset() ; h_tch[ch]->Reset() ; }
  persist->Reset() ; w_q->Reset() ; w_t->Reset();
  for (int k = 0; k < NW; k++) w_avg[k]->Reset();
  noise_rms->Reset() ; noise_hits->Reset() ; trig_found->Reset();
  n_events = n_clusters = 0;                // the trends go on
}

// --- the watched channel ---------------------------------------------------------------------
static void refresh_watched()
{
  if (watched < 0) return;
  w_q->Reset() ; w_q->Add(h_qch[watched]);
  w_t->Reset() ; w_t->Add(h_tch[watched]);
  for (int k = 0; k < NW; k++) if (w_avg_ch[k] >= 0) { w_avg[k]->Reset() ; w_avg[k]->Add(p_wave[w_avg_ch[k]]) ; }
  double m, s = tb::core_sigma(w_t, m);
  if (std::isnan(s)) { w_t->SetTitle(Form("ch %d hit time - group trigger;t [ns];hits", watched)) ; return ; }
  w_t->SetTitle(Form("ch %d hit time - group trigger: peak %.3f ns, #sigma %.0f ps;t [ns];hits", watched, m, 1000 * s));
  w_t->GetXaxis()->SetRangeUser(m - 2, m + 4);
}

static void set_watched(int ch)
{
  if (ch == watched) return;
  watched = ch;
  persist->Reset();
  int k = 0;
  w_avg_ch[k++] = ch;
  for (int n = 0; n < NSIG && k < NW - 1; n++)   // neighbours on the face
    if (n != ch && pix_row[n] >= 0 && pix_row[ch] >= 0 && abs(pix_row[n] - pix_row[ch]) <= 1 && abs(pix_col[n] - pix_col[ch]) <= 1)
      w_avg_ch[k++] = n;
  w_avg_ch[k++] = NSIG + ch / 8;                // its group's trigger trace
  for (; k < NW; k++) w_avg_ch[k] = -1;
  static const int colors[8] = {kOrange + 7, kYellow + 2, kGreen + 2, kCyan + 2, kBlue + 1, kViolet + 1, kMagenta + 1, kGray + 1};
  for (k = 0; k < NW; k++) {
    int c = w_avg_ch[k];
    w_avg[k]->SetLineColor(k == 0 ? kRed + 1 : c >= NSIG ? kBlack : colors[(k - 1) % 8]);
    w_avg[k]->SetLineWidth(k == 0 ? 3 : 2);
  }
  string px = pix_row[ch] >= 0 && by_pixel ? Form(" (pixel %d,%d)", pix_row[ch], pix_col[ch]) : "";
  persist->SetTitle(Form("ch %d%s, every event;t - group trigger [ns];mV;events", ch, px.c_str()));
  w_frame->SetTitle(Form("ch %d (thick) and its neighbours, average of the hit events;t - group trigger [ns];mV", ch));
  w_q->SetTitle(Form("ch %d charge, every event (pedestal + photoelectrons);charge [pC];events", ch));
  watched_changed = true;                   // the timer redraws the waveform pad and its legend
  refresh_watched();
}

static void choose_watched()
{
  if (watch_fixed >= 0) { set_watched(watch_fixed) ; return ; }
  int best = -1;
  for (int ch = 0; ch < NSIG; ch++)
    if (pix_row[ch] >= 0 && (best < 0 || iv.intime[ch] > iv.intime[best])) best = ch;
  if (best < 0 || iv.intime[best] < 5) return;
  if (watched < 0 || iv.intime[best] > 1.2 * iv.intime[watched]) set_watched(best);
}

void watch_channel(int ch)
{
  if (ch >= NSIG) { printf("general: channels are 0-%d\n", NSIG - 1) ; return ; }
  watch_fixed = ch;
  if (booked && ch >= 0) set_watched(ch);
  printf("general: main canvas %s\n", ch >= 0 ? Form("on ch %d", ch) : "follows the laser (most in-time hits)");
}

// --- trends ---------------------------------------------------------------------------------
static void trend_point(time_t now)
{
  double dt = now - iv.start;
  last_rate = iv.events / dt;
  array<double, 4> v = {last_rate, iv.events ? double(iv.hits) / iv.events : NAN, iv.hits ? iv.amp / iv.hits : NAN,
                        iv.timed ? iv.time / iv.timed : NAN};
  trend_pts.push_back({double(now), v});
  while (trend_pts.size() > TREND_N || trend_pts.front().first < now - TREND_KEEP) trend_pts.erase(trend_pts.begin());
  double x0 = trend_pts.front().first, x1 = max(double(now), x0 + 60);
  for (int i = 0; i < 4; i++) {
    int k = 0;
    double lo = 1e300, hi = -1e300, lx = 0, ly = 0;
    for (auto &p : trend_pts) {
      if (std::isnan(p.second[i])) continue;
      lx = p.first ; ly = p.second[i];
      trend[i]->SetPoint(k++, lx, ly);
      lo = min(lo, ly) ; hi = max(hi, ly);
    }
    if (!k) continue;
    for (; k < TREND_N; k++) trend[i]->SetPoint(k, lx, ly);   // unused points sit on the last one
    double m = hi > lo ? 0.15 * (hi - lo) : max(0.05 * fabs(hi), 1e-3);
    trend_frame[i]->SetMinimum(lo - m);
    trend_frame[i]->SetMaximum(hi + m);
    trend_frame[i]->GetXaxis()->SetLimits(x0 - 5, x1 + 5);
  }
}

// --- settings ----------------------------------------------------------------------------
void set_signal_window(double a, double b) { cuts.sig_from = a ; cuts.sig_to = b ; if (booked) { face_titles() ; printf("general: the time histograms keep their range: restart for a new one\n") ; } }
void set_threshold(double mv) { cuts.threshold = mv ; cuts.le_threshold = mv ; if (booked) { face_titles() ; cluster_titles() ; } }
void set_charge_window(double a, double b) { cuts.q_from = a ; cuts.q_to = b ; }
void set_cluster(double seed, double neighbour, int range, double dt)
{
  seed_mv = seed ; neighbour_mv = neighbour ; cluster_range = range ; cluster_dt = dt;
  if (neighbour < cuts.threshold) printf("general: neighbours also need the hit threshold, %.1f mV (set_threshold)\n", cuts.threshold);
  if (booked) cluster_titles();
}
void set_reset_each_run(bool on) { reset_each_run = on ; }

// --- per event ----------------------------------------------------------------------------
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
    tb::Db d;
    bool ok = d.load(info.detector);
    if (booked && ok && d.detector != db.detector)
      printf("general: run %d is detector %s, the face was made for %s: restart the monitor\n", info.run, d.detector.c_str(), db.detector.c_str());
    else { db = d ; db_ok = ok ; }
  }
  if (booked && reset_each_run) reset();
  printf("general: run %d, detector %s\n", info.run, db_ok ? db.detector.c_str() : "unknown");
}

// shift each group so that its hit-time peak sits where the first group's with hits does
static void align_groups()
{
  double peak[NTRIG];
  for (int g = 0; g < NTRIG; g++)
    peak[g] = h_tgroup[g]->GetEntries() >= 200 ? h_tgroup[g]->GetBinCenter(h_tgroup[g]->GetMaximumBin()) : NAN;
  int ref = -1;
  for (int g = 0; g < NTRIG && ref < 0; g++) if (!std::isnan(peak[g])) ref = g;
  if (ref < 0) return;
  for (int g = 0; g < NTRIG; g++) grp_off[g] = std::isnan(peak[g]) ? 0 : peak[g] - peak[ref];
  t_peak = peak[ref];
}

static void cluster(const tb::ChannelHit *hit)
{
  int seed = -1;
  for (int ch = 0; ch < NSIG; ch++)
    if (hit[ch].hit && pix_row[ch] >= 0 && hit[ch].amp >= seed_mv && (seed < 0 || hit[ch].amp > hit[seed].amp)) seed = ch;
  if (seed < 0) return;
  double ts = hit[seed].t_cfd - grp_off[seed / 8];
  vector<int> members{seed};
  for (int ch = 0; ch < NSIG; ch++) {
    if (ch == seed || !hit[ch].hit || pix_row[ch] < 0 || hit[ch].amp < neighbour_mv) continue;
    if (max(abs(pix_row[ch] - pix_row[seed]), abs(pix_col[ch] - pix_col[seed])) > cluster_range) continue;
    double dt = hit[ch].t_cfd - grp_off[ch / 8] - ts;
    clu_dt->Fill(dt);
    if (fabs(dt) <= cluster_dt) members.push_back(ch);
  }
  double sa = 0, sr = 0, sc = 0, sq = 0;
  for (int ch : members) { double a = hit[ch].amp ; sa += a ; sr += a * pix_row[ch] ; sc += a * pix_col[ch] ; sq += hit[ch].q ; }
  clu_size->Fill(members.size());
  clu_q->Fill(sq);
  clu_t->Fill(ts);
  clu_centroid->Fill(sc / sa, sr / sa);
  n_clusters++;
  auto now = chrono::steady_clock::now();
  if (now - last_cluster >= chrono::seconds(1)) {   // the display shows one cluster a second
    last_cluster = now;
    clu_last->Reset();
    for (int ch : members) clu_last->Fill(pix_col[ch], pix_row[ch], hit[ch].amp);
    clu_last->SetTitle(Form("latest cluster (run %d): seed ch %d, %zu pixel%s, %.2f pC;%s;%s;mV", info.run, seed, members.size(),
                            members.size() > 1 ? "s" : "", sq, xtitle(), ytitle()));
  }
}

static void waveforms(const tb::ChannelHit *hit)
{
  for (int ch = 0; ch < NCH; ch++) {              // average: every hit, and every trigger trace
    if (ch < NSIG && !hit[ch].hit) continue;
    double tg = trig_of(ch), sc = ch < NSIG ? 1 : TR_SCALE;
    if (std::isnan(tg)) continue;
    for (int s = 0; s < ev.ns; s++) p_wave[ch]->Fill(ev.t[ch][s] - tg, sc * ev.v[ch][s]);
  }
  if (watched >= 0 && !std::isnan(trig_of(watched)))   // persistence: every event
    for (int s = 0; s < ev.ns; s++) persist->Fill(ev.t[watched][s] - trig_of(watched), ev.v[watched][s]);
  auto now = chrono::steady_clock::now();
  if (now - last_wave < chrono::seconds(1)) return;   // latest: one event a second
  last_wave = now;
  for (int ch = 0; ch < NCH; ch++) {
    double tg = trig_of(ch), sc = ch < NSIG ? 1 : TR_SCALE;
    if (std::isnan(tg)) tg = 0;
    if (g_wave[ch]->GetN() != ev.ns) g_wave[ch]->Set(ev.ns);
    for (int s = 0; s < ev.ns; s++) g_wave[ch]->SetPoint(s, ev.t[ch][s] - tg, sc * ev.v[ch][s]);
  }
}

int process_event(Event *e)
{
  if (e->getEvtType() == 9) { begin_run(e) ; return 0 ; }
  if (!ev.fill(e)) return 0;
  if (!booked) {
    if (!db_ok) {
      db_ok = db.load("");
      printf("general: no begin-run event seen: detector %s (testbench.sh use)\n", db_ok ? db.detector.c_str() : "unknown");
    }
    book();
  }
  n_events++;
  time_t now = e->getTime();
  if (now <= 0) now = time(nullptr);
  if (!iv.start) iv.start = now;
  iv.events++;

  for (int g = 0; g < NTRIG; g++) trig_found->Fill(g, std::isnan(ev.trig[g]) ? 0 : 1);
  int bl_hi = min(ev.bl_last, ev.ns);
  for (int ch = 0; ch < NCH; ch++) noise_rms->Fill(ch, tb::rms(ev.v[ch], ev.bl_first, bl_hi));

  tb::ChannelHit hit[NSIG];
  for (int ch = 0; ch < NSIG; ch++) {
    hit[ch] = tb::measure(ev, ch, cuts);
    const tb::ChannelHit &h = hit[ch];
    if (!h.valid) continue;
    noise_hits->Fill(ch, h.hit ? 1 : 0);
    h_qch[ch]->Fill(h.q);
    if (h.hit) {
      h_tch[ch]->Fill(h.t_cfd);
      double ta = h.t_cfd - grp_off[ch / 8];
      if (std::isnan(t_peak) || fabs(ta - t_peak) < 1.0) iv.intime[ch]++;
    }
    if (pix_row[ch] < 0) continue;
    face_occ->Fill(pix_col[ch], pix_row[ch], h.hit ? 1 : 0);
    face_all->Fill(pix_col[ch], pix_row[ch], h.amp);
    if (!h.hit) continue;
    face_amp->Fill(pix_col[ch], pix_row[ch], h.amp);
    h_tgroup[ch / 8]->Fill(h.t_cfd);
    iv.hits++ ; iv.amp += h.amp;
    if (!std::isnan(h.t_cfd)) { iv.time += h.t_cfd - grp_off[ch / 8] ; iv.timed++ ; }
  }
  if (n_events % (n_events < 2000 ? 200 : 1000) == 0) align_groups();
  cluster(hit);
  waveforms(hit);

  if (watched < 0 && n_events % 200 == 0) choose_watched();   // a first choice soon after the start
  if (n_events % 300 == 0) refresh_watched();
  if (now - iv.start >= TREND_EVERY) {
    trend_point(now);
    choose_watched();
    iv = Interval();
    iv.start = now;
  }
  return 0;
}

// --- display: drawn in ROOT's main thread --------------------------------------------------------
static TCanvas *c_main = nullptr, *c_face = nullptr, *c_clusters = nullptr, *c_waves = nullptr, *c_noise = nullptr;

static void style()
{
  gStyle->SetOptStat(0);
  gStyle->SetNumberContours(64);
  gStyle->SetPaintTextFormat(".0f");
}

static bool wait_booked()
{
  for (int i = 0; i < 100 && !booked; i++) { gSystem->ProcessEvents() ; gSystem->Sleep(100) ; }   // online: wait for a first event
  if (!booked) printf("general: no events yet (online(), pstart() or prun(N) first)\n");
  return booked;
}

static void draw_watch_pad()
{
  static TLegend *leg = nullptr;
  c_main->cd(3);
  gPad->Clear();
  w_frame->Draw();
  delete leg;
  leg = new TLegend(0.78, 0.12, 0.98, 0.55);
  for (int k = 0; k < NW; k++) {
    int ch = w_avg_ch[k];
    if (ch < 0) continue;
    w_avg[k]->Draw("hist same");
    leg->AddEntry(w_avg[k], ch < NSIG ? Form("ch %d", ch) : Form("TR%d / 10", ch - NSIG), "l");
  }
  leg->Draw();
}

static void draw_waveforms()
{
  static TLegend *leg[NTRIG] = {};
  wave_titles();
  for (int g = 0; g < NTRIG; g++) {
    c_waves->cd(g + 1);
    gPad->Clear();
    wave_frame[g]->Draw();
    if (!leg[g]) {
      leg[g] = new TLegend(0.80, 0.12, 0.98, 0.52);
      for (int k = 0; k < 9; k++) {
        int ch = k < 8 ? 8 * g + k : NSIG + g;
        leg[g]->AddEntry(g_wave[ch], k < 8 ? Form("ch %d", ch) : Form("TR%d / 10", g), "l");
      }
    }
    for (int k = 0; k < 9; k++) {
      int ch = k < 8 ? 8 * g + k : NSIG + g;
      if (wave_average) p_wave[ch]->Draw("hist same");
      else if (g_wave[ch]->GetN() > 0) g_wave[ch]->Draw("L same");
    }
    leg[g]->Draw();
  }
  c_waves->Modified();
  c_waves->Update();
}

static void modify_all(TVirtualPad *p)
{
  p->Modified();
  TIter next(p->GetListOfPrimitives());
  while (TObject *o = next()) if (o->InheritsFrom(TVirtualPad::Class())) modify_all((TVirtualPad *)o);
}

static void tick()   // redraw: from the timer, and before saving
{
  if (watched_changed && c_main) { watched_changed = false ; draw_watch_pad() ; }
  for (TCanvas *c : {c_main, c_face, c_clusters, c_waves, c_noise})
    if (c) { modify_all(c) ; c->Update() ; }
}

struct Ticker : public TTimer {
  Ticker() : TTimer(2000, kTRUE) {}
  Bool_t Notify() override { tick() ; Reset() ; return kTRUE ; }
};

static void start_ticker()
{
  static Ticker *t = nullptr;
  if (t || gROOT->IsBatch()) return;
  t = new Ticker;
  t->TurnOn();
}

void set_waveform_mode(const char *m)
{
  string s = m;
  if (s != "latest" && s != "average") { printf("general: waveform mode is \"latest\" or \"average\"\n") ; return ; }
  wave_average = s == "average";
  if (c_waves) draw_waveforms();
}

void set_waveform_range(double lo, double hi)
{
  wave_min = lo ; wave_max = hi;
  if (!booked) return;
  for (int g = 0; g < NTRIG; g++) { wave_frame[g]->SetMinimum(lo) ; wave_frame[g]->SetMaximum(hi) ; }
  w_frame->SetMinimum(lo) ; w_frame->SetMaximum(hi);
  if (c_waves) draw_waveforms();
}

void set_waveform_time(double from_ns, double to_ns)
{
  wave_from = from_ns ; wave_to = to_ns;
  if (!booked) return;
  wave_xrange();
  if (c_waves) draw_waveforms();
}

void draw()
{
  if (!wait_booked()) return;
  style();
  if (watched < 0) choose_watched();
  if (watched < 0) set_watched(watch_fixed >= 0 ? watch_fixed : 0);
  if (!c_main) { c_main = new TCanvas("c_main", "417 test bench monitor", 1500, 860) ; c_main->Divide(3, 2) ; }
  c_main->cd(1) ; gPad->SetRightMargin(0.15) ; face_occ->Draw("colz");
  c_main->cd(2) ; gPad->SetRightMargin(0.15) ; gPad->SetLogz() ; persist->Draw("colz");
  draw_watch_pad();
  watched_changed = false;
  c_main->cd(4) ; gPad->SetLogy() ; w_q->Draw();
  c_main->cd(5) ; w_t->Draw();
  static const char *tt[4] = {"event rate [Hz]", "hits per event, all pixels", "mean pulse height of the hits [mV]",
                              "mean hit time - trigger [ns]"};
  c_main->cd(6);
  gPad->Divide(1, 4, 0.002, 0.002);
  for (int i = 0; i < 4; i++) {
    c_main->cd(6)->cd(i + 1);
    gPad->SetTopMargin(0.22) ; gPad->SetBottomMargin(0.2) ; gPad->SetLeftMargin(0.14) ; gPad->SetRightMargin(0.03) ; gPad->SetGridy();
    trend_frame[i]->Draw("axis");
    trend[i]->Draw("LP same");
    auto *t = new TLatex(0.14, 0.86, Form("%s, last %d min", tt[i], TREND_KEEP / 60));
    t->SetNDC() ; t->SetTextSize(0.14) ; t->Draw();
  }
  c_main->Update();
  start_ticker();
}

void draw_details()
{
  if (!wait_booked()) return;
  style();
  if (!c_face) { c_face = new TCanvas("c_face", "detector face", 1300, 560) ; c_face->Divide(2, 1) ; }
  for (int i = 1; i <= 2; i++) { c_face->cd(i) ; gPad->SetRightMargin(0.15) ; }
  c_face->cd(1) ; face_amp->Draw("colz");
  c_face->cd(2) ; face_occ->Draw("colz");
  c_face->Update();

  if (!c_clusters) { c_clusters = new TCanvas("c_clusters", "clusters", 1400, 820) ; c_clusters->Divide(3, 2) ; }
  c_clusters->cd(1) ; gPad->SetRightMargin(0.15) ; clu_last->Draw("colz text");
  c_clusters->cd(2) ; gPad->SetRightMargin(0.15) ; clu_centroid->Draw("colz");
  c_clusters->cd(3) ; clu_size->Draw();
  c_clusters->cd(4) ; clu_q->Draw();
  c_clusters->cd(5) ; clu_t->Draw();
  c_clusters->cd(6) ; clu_dt->Draw();
  c_clusters->Update();

  if (!c_waves) { c_waves = new TCanvas("c_waves", "waveforms", 1400, 820) ; c_waves->Divide(2, 2) ; }
  draw_waveforms();
  start_ticker();
}

void draw_noise()
{
  if (!wait_booked()) return;
  style();
  if (!c_noise) { c_noise = new TCanvas("c_noise", "noise and triggers", 1300, 820) ; c_noise->Divide(2, 2) ; }
  c_noise->cd(1) ; gPad->SetGridy() ; noise_rms->SetMinimum(0) ; noise_rms->Draw("hist");
  c_noise->cd(2) ; gPad->SetRightMargin(0.15) ; face_all->Draw("colz");
  c_noise->cd(3) ; gPad->SetGridy() ; trig_found->SetMinimum(0) ; trig_found->SetMaximum(1.05) ; trig_found->Draw("hist");
  c_noise->cd(4) ; gPad->SetGridy() ; gPad->SetLogy() ; noise_hits->Draw("hist");
  c_noise->Update();
  start_ticker();
}

void online()
{
  if (rcdaqopen()) { printf("general: could not open the rcdaq stream (rcdaq_server running?)\n") ; return ; }
  pstart();
  draw();
}

void status()
{
  printf("general: run %d, detector %s, %ld events, %ld clusters, %.0f Hz (last %d s)\n", info.run, db_ok ? db.detector.c_str() : "unknown",
         n_events, n_clusters, last_rate, TREND_EVERY);
  printf("  signal window %.1f to %.1f ns, hit threshold %.1f mV, waveforms %s\n", cuts.sig_from, cuts.sig_to, cuts.threshold,
         wave_average ? "average" : "latest");
  printf("  clusters: seed > %.1f mV, neighbours > %.1f mV within %d pixel(s) and %.2f ns\n", seed_mv, max(neighbour_mv, cuts.threshold),
         cluster_range, cluster_dt);
  printf("  main canvas: ch %d (%s)\n", watched, watch_fixed >= 0 ? "fixed" : "follows the laser");
  if (booked) printf("  group time offsets: %.2f %.2f %.2f %.2f ns\n", grp_off[0], grp_off[1], grp_off[2], grp_off[3]);
}

void save_plots(const char *prefix)
{
  if (!c_main) draw();
  if (!c_face) draw_details();
  if (!c_noise) draw_noise();
  if (!c_main) return;
  refresh_watched();
  tick();
  c_main->SaveAs(Form("%s_main.png", prefix));
  c_face->SaveAs(Form("%s_face.png", prefix));
  c_clusters->SaveAs(Form("%s_clusters.png", prefix));
  c_waves->SaveAs(Form("%s_waveforms.png", prefix));
  c_noise->SaveAs(Form("%s_noise.png", prefix));
}
