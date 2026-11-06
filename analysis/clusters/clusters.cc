// clusters - pixel clusters from laser runs: cluster sizes, charge sharing between pixels, which
// pixel pairs fire together and how often by chance, and where to put the thresholds.
//
// Stage 1, once per run (a 100k-event run takes ~10 min):
//   ./clusters.sh /scratch2/data/hpDIRC/photonis/photonis-00000252-0000.evt
//   root [1] prun() ; save_events("run252_events.root")
// Stage 2, from the saved events, as often as wanted (seconds):
//   ./clusters.sh
//   root [1] load_events("run252_events.root")
//   root [2] set_cuts(8, 3) ; make_clusters() ; draw_clusters() ; draw_examples() ; save_plots("run252")
//
// Stage 1 keeps, for every event and every channel, the deepest point in the signal window (pulse
// height, also when there is no pulse: the noise), the charge around it, and the CFD time relative
// to the group trigger when the pulse is above the time threshold (2 mV). It also keeps the
// waveforms of the first few hundred events with two or more channels above 3 mV.
// Stage 2 builds clusters as the online monitor does: the groups are first lined up on their
// hit-time peaks (the two DRS4 boards differ by ~2 ns); the seed is the highest pulse >= the seed
// threshold within the seed window of the laser peak; neighbours are pixels within the range,
// above the neighbour threshold and within dt of the seed. Masked inputs (MASKED_CHANNELS in
// db/testbench.conf, or mask_channels()) take no part.
// Pairs: 2-pixel clusters per pixel pair, against the number expected by chance from each pixel's
// own in-time rate. Side-by-side and above/below pairs share charge; diagonal pairs (a corner)
// hardly do, so they are the control: lowering the neighbour threshold is safe as long as the
// diagonal neighbours stay at their accidental level (scan_thresholds).
// Time offsets: with db/v1742/time_offsets_<serial>_<speed>.conf (analysis/time_offsets), stage 2
// takes each input's offset off its times before lining up the groups (what is left should be ~0),
// and stage 1 shifts the fixed charge window by it, so one window fits every group. The file keeps
// the raw times.
// Charge (draw_charge): stage 1 with set_charge_window() also keeps the charge of every channel in
// a fixed window, pulse or not, so the pedestal is in the spectrum. The charge of the spot pixels
// summed per event (photoelectrons shared between pixels counted once, whole) is then fitted with
// the pedestal + Poisson-weighted Gaussians of lib/tb_spe.h: Q1 = the charge of one photoelectron.
//   root [1] set_charge_window(24, 27.2) ; prun() ; save_events("run252_events.root")
//   root [2] suggest_charge_window()      // from the waveforms of an earlier stage 1

#include <iostream>
#include <pmonitor/pmonitor.h>
#include "clusters.h"

#include "tb_canvas.h"
#include "tb_db.h"
#include "tb_pulse.h"
#include "tb_spe.h"
#include "tb_v1742.h"

#include <TCanvas.h>
#include <TF1.h>
#include <TFile.h>
#include <TGraph.h>
#include <TH1.h>
#include <TH2.h>
#include <TLatex.h>
#include <TLegend.h>
#include <TLine.h>
#include <TNamed.h>
#include <TPaveText.h>
#include <TProfile.h>
#include <TROOT.h>
#include <TStyle.h>
#include <TTree.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace std;
using tb::NSIG;
using tb::NTRIG;

int init_done = 0;

static const int PACKET = 2000;

// --- stage 1 ----------------------------------------------------------------------------------
static tb::Cuts cuts = [] { tb::Cuts c ; c.sig_from = 17 ; c.sig_to = 35 ; c.threshold = 2 ; c.le_threshold = 2 ; return c ; }();
static int max_trace_events = 300;
void set_signal_window(double a, double b) { cuts.sig_from = a ; cuts.sig_to = b ; }
void set_time_threshold(double mv) { cuts.threshold = mv ; cuts.le_threshold = mv ; }
void set_trace_events(int n) { max_trace_events = n ; }
static double qw_from = NAN, qw_to = NAN;   // fixed charge window [ns] relative to the group trigger
void set_charge_window(double a, double b) { qw_from = a ; qw_to = b ; }

static tb::V1742Event ev;
static tb::RunInfo info;
static string detector_name;
static tb::TimeOffsets toff;             // stage 1: shifts the fixed charge window; stage 2: lines up the times
static int v1742_serial = -1, v1742_freq = -1;
static bool qw_shifted = false;          // the fixed-window charge was taken with the db offsets

struct Row { int run, event ; float x, y, amp[NSIG], q[NSIG], t[NSIG], qw[NSIG] ; };
struct Trace { int event, ch ; vector<float> t, v ; };
static vector<Row> rows;
static vector<Trace> traces;
static int trace_events = 0;

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
    if (!info.detector.empty()) detector_name = info.detector;
    printf("clusters: run %d, detector %s\n", info.run, info.detector.empty() ? "? (not in the data)" : info.detector.c_str());
    if (std::isnan(qw_from)) printf("clusters: no set_charge_window(): no fixed-window charge, so no pedestal for draw_charge()'s fit\n");
    return 0;
  }
  if (!ev.fill(e)) return 0;
  if (ev.serial != v1742_serial || ev.freq != v1742_freq) {
    v1742_serial = ev.serial ; v1742_freq = ev.freq;
    qw_shifted = toff.load(ev.serial, ev.freq);
    if (!std::isnan(qw_from))
      printf("clusters: fixed charge window %s\n", qw_shifted ? ("shifted per input by " + toff.file).c_str() : "the same for every input (no db/v1742 time offsets)");
  }
  Row r;
  r.run = info.run ; r.event = e->getEvtSequence() ; r.x = info.x ; r.y = info.y;
  static set<int> masked = tb::masked_channels();
  int above3 = 0;
  for (int ch = 0; ch < NSIG; ch++) {
    tb::ChannelHit h = tb::measure(ev, ch, cuts);
    r.amp[ch] = h.valid ? h.amp : NAN;
    r.q[ch] = h.valid ? h.q : NAN;
    r.t[ch] = h.hit ? h.t_cfd : NAN;
    r.qw[ch] = NAN;
    if (h.valid && !std::isnan(qw_from)) {
      int lo, hi;
      tb::window_around(ev.t[ch], ev.ns, ev.trigger_of(ch), qw_from + toff.ns[ch], qw_to + toff.ns[ch], lo, hi);
      r.qw[ch] = tb::charge_pC(ev.t[ch], ev.v[ch], lo, hi);
    }
    if (h.valid && h.amp >= 3 && !masked.count(ch)) above3++;
  }
  rows.push_back(r);
  if (above3 >= 2 && trace_events < max_trace_events) {   // waveforms around the signal window
    trace_events++;
    for (int ch = 0; ch < NSIG; ch++) {
      if (masked.count(ch) || !(r.amp[ch] >= 1.0)) continue;
      double tg = ev.trigger_of(ch);
      Trace tr{r.event, ch, {}, {}};
      for (int s = 0; s < ev.ns; s++) {
        double tt = ev.t[ch][s] - tg;
        if (tt >= cuts.sig_from - 5 && tt <= cuts.sig_to + 10) { tr.t.push_back(tt) ; tr.v.push_back(ev.v[ch][s]) ; }
      }
      traces.push_back(tr);
    }
  }
  return 0;
}

void save_events(const char *filename)
{
  TFile f(filename, "RECREATE");
  Row r;
  TTree te("events", "pulse height [mV], charge [pC], CFD time - group trigger [ns] of every channel");
  te.Branch("run", &r.run) ; te.Branch("event", &r.event) ; te.Branch("x_mm", &r.x) ; te.Branch("y_mm", &r.y);
  te.Branch("amp", r.amp, Form("amp[%d]/F", NSIG)) ; te.Branch("q", r.q, Form("q[%d]/F", NSIG)) ; te.Branch("t", r.t, Form("t[%d]/F", NSIG));
  te.Branch("qw", r.qw, Form("qw[%d]/F", NSIG));
  for (auto &x : rows) { r = x ; te.Fill() ; }
  te.Write();
  int event, ch, n;
  static float tt[tb::MAXS], vv[tb::MAXS];
  TTree tw("traces", "waveforms of events with >= 2 channels above 3 mV: t - group trigger [ns], mV");
  tw.Branch("event", &event) ; tw.Branch("ch", &ch) ; tw.Branch("n", &n) ; tw.Branch("t", tt, "t[n]/F") ; tw.Branch("v", vv, "v[n]/F");
  for (auto &x : traces) {
    event = x.event ; ch = x.ch ; n = x.t.size();
    copy(x.t.begin(), x.t.end(), tt) ; copy(x.v.begin(), x.v.end(), vv);
    tw.Fill();
  }
  tw.Write();
  TNamed("detector", detector_name.c_str()).Write();
  TNamed("v1742", Form("%d %d %d", v1742_serial, v1742_freq, qw_shifted ? 1 : 0)).Write();   // serial, FREQUENCY code, charge window shifted
  TNamed("settings", Form("signal window %.1f to %.1f ns, time threshold %.1f mV, charge window %.1f to %.1f ns", cuts.sig_from, cuts.sig_to, cuts.threshold, qw_from, qw_to)).Write();
  f.Close();
  printf("clusters: wrote %zu events (%d with waveforms) to %s\n", rows.size(), trace_events, filename);
}

// --- stage 2 ----------------------------------------------------------------------------------
static double seed_mv = 8, nb_mv = 5, dt_cut = 1.0, seed_win = 2.0;
static int range_px = 1;
static string masked_list = "-";
void set_cuts(double s, double n, int r, double dt, double w) { seed_mv = s ; nb_mv = n ; range_px = r ; dt_cut = dt ; seed_win = w ; }
void mask_channels(const char *list) { masked_list = list ; }

void load_events(const char *filename)
{
  if (!filename || !*filename) { printf("clusters: %zu events in memory\n", rows.size()) ; return ; }
  TFile f(filename);
  TTree *te = f.IsOpen() ? (TTree *)f.Get("events") : nullptr;
  if (!te) { printf("clusters: no events tree in %s\n", filename) ; return ; }
  if (auto *d = (TNamed *)f.Get("detector")) detector_name = d->GetTitle();
  v1742_serial = -1 ; v1742_freq = 0 ; qw_shifted = false;   // files from before: 5 GS/s, window not shifted
  if (auto *d = (TNamed *)f.Get("v1742")) { int sh = 0 ; sscanf(d->GetTitle(), "%d %d %d", &v1742_serial, &v1742_freq, &sh) ; qw_shifted = sh ; }
  rows.clear() ; traces.clear();
  Row r;
  te->SetBranchAddress("run", &r.run) ; te->SetBranchAddress("event", &r.event) ; te->SetBranchAddress("x_mm", &r.x) ; te->SetBranchAddress("y_mm", &r.y);
  te->SetBranchAddress("amp", r.amp) ; te->SetBranchAddress("q", r.q) ; te->SetBranchAddress("t", r.t);
  fill(r.qw, r.qw + NSIG, NAN);
  if (te->GetBranch("qw")) te->SetBranchAddress("qw", r.qw);   // files from before the fixed window have none
  for (Long64_t i = 0; i < te->GetEntries(); i++) { te->GetEntry(i) ; rows.push_back(r) ; }
  if (auto *tw = (TTree *)f.Get("traces")) {
    int event, ch, n;
    static float tt[tb::MAXS], vv[tb::MAXS];
    tw->SetBranchAddress("event", &event) ; tw->SetBranchAddress("ch", &ch) ; tw->SetBranchAddress("n", &n);
    tw->SetBranchAddress("t", tt) ; tw->SetBranchAddress("v", vv);
    for (Long64_t i = 0; i < tw->GetEntries(); i++) { tw->GetEntry(i) ; traces.push_back({event, ch, vector<float>(tt, tt + n), vector<float>(vv, vv + n)}) ; }
  }
  printf("clusters: %zu events, %zu waveforms, detector %s, from %s\n", rows.size(), traces.size(), detector_name.c_str(), filename);
}

static int pix_row[NSIG], pix_col[NSIG], nrows = 0, ncols = 0;
static double grp_off[NTRIG], t_peak = NAN;

static bool prepare()   // pixel map, masked inputs, time offsets, group alignment
{
  if (rows.empty()) { printf("clusters: no events (stage 1, or load_events)\n") ; return false ; }
  static string last_file = "?";
  bool sh = qw_shifted;
  toff.load(v1742_serial, v1742_freq);
  qw_shifted = sh;
  if (toff.file != last_file) {
    last_file = toff.file;
    printf("clusters: time offsets %s\n", toff.ok ? toff.file.c_str() : "none in db/v1742: groups lined up from the data only");
  }
  tb::Db db;
  bool ok = db.load(detector_name);
  set<int> masked = tb::masked_channels(masked_list);
  for (int ch = 0; ch < NSIG; ch++) {
    pix_row[ch] = pix_col[ch] = -1;
    int r, c;
    if (ok && db.pixel(PACKET, ch, r, c) && !masked.count(ch)) { pix_row[ch] = r ; pix_col[ch] = c ; }
  }
  nrows = ok ? int(db.num("NROWS", 8)) : 8 ; ncols = ok ? int(db.num("NCOLS", 8)) : 8;
  if (!ok) printf("clusters: detector '%s' not in the db: no pixel map\n", detector_name.c_str());
  // hit-time peak per group (best 0.25 ns window of pulses above the seed threshold)
  double pk[NTRIG];
  long n[NTRIG] = {};
  int ref = -1;
  for (int g = 0; g < NTRIG; g++) {
    TH1F h("tg", "", 1800, cuts.sig_from, cuts.sig_to);
    h.SetDirectory(nullptr);
    for (auto &r : rows)
      for (int ch = 8 * g; ch < 8 * g + 8; ch++)
        if (pix_row[ch] >= 0 && r.amp[ch] >= seed_mv && !std::isnan(r.t[ch])) { h.Fill(r.t[ch] - toff.ns[ch]) ; n[g]++ ; }
    double best = 0;
    pk[g] = NAN;
    for (int b = 1; b + 24 <= h.GetNbinsX(); b++) {
      double s = h.Integral(b, b + 24);
      if (s > best) { best = s ; pk[g] = h.GetXaxis()->GetBinLowEdge(b) + 0.125 ; }
    }
    if (best < 30 || best < 0.2 * n[g]) pk[g] = NAN;
    if (!std::isnan(pk[g]) && (ref < 0 || n[g] > n[ref])) ref = g;
  }
  if (ref < 0) { printf("clusters: no laser timing peak in any group\n") ; return false ; }
  for (int g = 0; g < NTRIG; g++) grp_off[g] = std::isnan(pk[g]) ? 0 : pk[g] - pk[ref];
  t_peak = pk[ref];
  return true;
}

static double t_al(const Row &r, int ch) { return r.t[ch] - toff.ns[ch] - grp_off[ch / 8]; }
static bool in_time(const Row &r, int ch, double win) { return !std::isnan(r.t[ch]) && fabs(t_al(r, ch) - t_peak) <= win; }
static int seed_of(const Row &r)
{
  int s = -1;
  for (int ch = 0; ch < NSIG; ch++)
    if (pix_row[ch] >= 0 && r.amp[ch] >= seed_mv && in_time(r, ch, seed_win) && (s < 0 || r.amp[ch] > r.amp[s])) s = ch;
  return s;
}
static bool have_qw()   // stage 1 ran with a fixed charge window
{
  for (size_t i = 0; i < rows.size() && i < 100; i++) for (int ch = 0; ch < NSIG; ch++) if (!std::isnan(rows[i].qw[ch])) return true;
  return false;
}
// the fixed window was set on the laser pulses of the busiest group and, with db time offsets,
// shifted per input in stage 1; an input whose pulses still sit elsewhere (groups 0/1 are ~2 ns
// earlier than 2/3 without offsets) falls back to the charge around each pulse
static bool qw_fits(int ch) { return fabs(grp_off[ch / 8] + (qw_shifted ? 0 : toff.ns[ch])) < 0.5 ; }
static int dist(int a, int b) { return max(abs(pix_row[a] - pix_row[b]), abs(pix_col[a] - pix_col[b])); }
static bool edge(int a, int b) { return dist(a, b) == 1 && (pix_row[a] == pix_row[b] || pix_col[a] == pix_col[b]); }

// neighbours per cluster at one neighbour threshold, and what chance alone would give
struct Count { long clusters = 0, edge = 0, diag = 0 ; double edge_acc = 0, diag_acc = 0 ; };
static Count count_at(double thr)
{
  double rate[NSIG] = {};   // in-time pulses above thr per event, per channel (as if independent of the seed)
  for (auto &r : rows) for (int ch = 0; ch < NSIG; ch++) if (pix_row[ch] >= 0 && r.amp[ch] >= thr && in_time(r, ch, dt_cut)) rate[ch]++;
  for (double &x : rate) x /= rows.size();
  Count c;
  for (auto &r : rows) {
    int s = seed_of(r);
    if (s < 0) continue;
    c.clusters++;
    double ts = t_al(r, s);
    for (int ch = 0; ch < NSIG; ch++) {
      if (ch == s || pix_row[ch] < 0 || dist(ch, s) != 1) continue;
      bool e = edge(ch, s);
      (e ? c.edge_acc : c.diag_acc) += rate[ch];
      if (r.amp[ch] >= thr && !std::isnan(r.t[ch]) && fabs(t_al(r, ch) - ts) <= dt_cut) (e ? c.edge : c.diag)++;
    }
  }
  return c;
}

// --- histograms ----------------------------------------------------------------------------------
static TH1F *h_size = nullptr, *h_nb_edge = nullptr, *h_nb_diag = nullptr, *h_share = nullptr, *h_q[4] = {};
static TH2F *h_centroid = nullptr, *h_dt_amp = nullptr, *h_nb_seed = nullptr, *h_pairs = nullptr;
static TGraph *g_scan[4] = {};
struct Example { int event ; vector<int> members ; };
static vector<Example> examples;
static map<int, size_t> trace_index;     // event -> first trace

static void book()
{
  static bool done = false;
  auto zap = [](TH1 *&h) { delete h ; h = nullptr ; };
  if (done) {
    zap((TH1 *&)h_size) ; zap((TH1 *&)h_nb_edge) ; zap((TH1 *&)h_nb_diag) ; zap((TH1 *&)h_share);
    for (auto &h : h_q) zap((TH1 *&)h);
    zap((TH1 *&)h_centroid) ; zap((TH1 *&)h_dt_amp) ; zap((TH1 *&)h_nb_seed) ; zap((TH1 *&)h_pairs);
  }
  done = true;
  h_size = new TH1F("cluster_size", "pixels per cluster;pixels;clusters", 9, 0.5, 9.5);
  h_nb_edge = new TH1F("nb_amp_edge", "pulse height of the 8 pixels around the seed (no cut);pulse height [mV];pixels", 300, 0, 30);
  h_nb_diag = new TH1F("nb_amp_diag", "", 300, 0, 30);
  h_share = new TH1F("share", "2-pixel clusters, side by side or above/below: neighbour / (seed + neighbour);fraction;clusters", 50, 0, 0.5);
  const char *qn[4] = {"1 pixel", "2 pixels", "3+ pixels", "all"};
  for (int i = 0; i < 4; i++) h_q[i] = new TH1F(Form("q_size%d", i + 1), Form("summed charge, %s;charge [pC];clusters", qn[i]), 125, -0.05, 1.2);
  h_centroid = new TH2F("centroid", "cluster centroids (pulse-height weighted);col;row;clusters", 10 * ncols, -0.5, ncols - 0.5, 10 * nrows, -0.5, nrows - 0.5);
  h_dt_amp = new TH2F("dt_amp", "pixels around the seed with a time: time - seed time vs pulse height;pulse height [mV];#Deltat [ns]", 60, 0, 30, 100, -5, 5);
  h_nb_seed = new TH2F("nb_seed", "cluster members: neighbour vs seed pulse height (side by side, above/below);seed [mV];neighbour [mV]", 60, 0, 60, 60, 0, 60);
  h_pairs = new TH2F("pairs", "2-pixel clusters per pair of channels;channel;channel", NSIG, -0.5, NSIG - 0.5, NSIG, -0.5, NSIG - 0.5);
  for (TH1 *h : initializer_list<TH1 *>{h_size, h_nb_edge, h_nb_diag, h_share, h_q[0], h_q[1], h_q[2], h_q[3], h_centroid, h_dt_amp, h_nb_seed, h_pairs})
    h->SetDirectory(nullptr);
}

void make_clusters()
{
  if (!prepare()) return;
  book();
  const bool fixed_q = have_qw();
  examples.clear();
  trace_index.clear();
  for (size_t i = 0; i < traces.size(); i++) trace_index.emplace(traces[i].event, i);

  double rate[NSIG] = {};   // in-time pulses above the neighbour threshold, per event
  for (auto &r : rows) for (int ch = 0; ch < NSIG; ch++) if (pix_row[ch] >= 0 && r.amp[ch] >= nb_mv && in_time(r, ch, dt_cut)) rate[ch]++;
  for (double &x : rate) x /= rows.size();

  struct Pair { long n = 0 ; vector<double> ratio ; double dt = 0, dt2 = 0 ; };
  map<pair<int, int>, Pair> pairs;
  map<pair<int, int>, double> pair_acc;   // expected by chance: sum over clusters of the neighbour's rate
  long clusters = 0, dropped_dt = 0;
  map<int, long> sizes;
  for (auto &r : rows) {
    int s = seed_of(r);
    if (s < 0) continue;
    clusters++;
    double ts = t_al(r, s);
    vector<int> m{s};
    for (int ch = 0; ch < NSIG; ch++) {
      if (ch == s || pix_row[ch] < 0 || dist(ch, s) > range_px) continue;
      if (dist(ch, s) == 1) (edge(ch, s) ? h_nb_edge : h_nb_diag)->Fill(r.amp[ch]);
      pair_acc[{min(s, ch), max(s, ch)}] += rate[ch];
      bool timed = !std::isnan(r.t[ch]);
      double dt = timed ? t_al(r, ch) - ts : NAN;
      if (timed) h_dt_amp->Fill(r.amp[ch], dt);
      if (r.amp[ch] < nb_mv || !timed) continue;
      if (fabs(dt) <= dt_cut) m.push_back(ch) ; else dropped_dt++;
    }
    sizes[m.size()]++;
    h_size->Fill(m.size());
    double sa = 0, sr = 0, sc = 0, sq = 0;
    for (int ch : m) { double a = r.amp[ch] ; sa += a ; sr += a * pix_row[ch] ; sc += a * pix_col[ch] ; sq += fixed_q && qw_fits(ch) ? r.qw[ch] : r.q[ch] ; }
    h_centroid->Fill(sc / sa, sr / sa);
    h_q[min<int>(m.size(), 3) - 1]->Fill(sq);
    h_q[3]->Fill(sq);
    for (size_t k = 1; k < m.size(); k++) if (edge(m[k], s)) h_nb_seed->Fill(r.amp[s], r.amp[m[k]]);
    if (m.size() == 2) {
      int b = m[1];
      if (edge(s, b)) h_share->Fill(r.amp[b] / (r.amp[s] + r.amp[b]));
      Pair &p = pairs[{min(s, b), max(s, b)}];
      double d = t_al(r, b) - ts;
      p.n++ ; p.ratio.push_back(r.amp[b] / r.amp[s]) ; p.dt += d ; p.dt2 += d * d;
      h_pairs->Fill(min(s, b), max(s, b));
    }
    if (m.size() >= 2 && trace_index.count(r.event)) examples.push_back({r.event, m});
  }

  printf("clusters: %zu events, detector %s; seed >= %.1f mV within %.1f ns of the laser peak (%.3f ns), neighbours >= %.1f mV within %d px and %.2f ns\n",
         rows.size(), detector_name.c_str(), seed_mv, seed_win, t_peak, nb_mv, range_px, dt_cut);
  printf("  group offsets %.2f %.2f %.2f %.2f ns; masked inputs:", grp_off[0], grp_off[1], grp_off[2], grp_off[3]);
  for (int ch : tb::masked_channels(masked_list)) printf(" %d", ch);
  printf("\n  %ld clusters (%.2f %% of events); sizes:", clusters, 100.0 * clusters / rows.size());
  for (auto &s : sizes) printf("  %d px %ld (%.1f %%)", s.first, s.second, 100.0 * s.second / clusters);
  printf("\n  neighbours above threshold but outside the time cut: %ld\n", dropped_dt);
  printf("\n  pair    pixels (r,c)    kind          2-px clusters  by chance   neighbour/seed median (10-90 %%)   dt mean / rms [ps]\n");
  vector<pair<long, pair<int, int>>> order;
  for (auto &p : pairs) order.push_back({p.second.n, p.first});
  sort(order.rbegin(), order.rend());
  for (size_t i = 0; i < order.size() && i < 15; i++) {
    int a = order[i].second.first, b = order[i].second.second;
    Pair &p = pairs[order[i].second];
    sort(p.ratio.begin(), p.ratio.end());
    auto qu = [&](double f) { return p.ratio[min(p.ratio.size() - 1, size_t(f * p.ratio.size()))] ; };
    double mu = p.dt / p.n, rms = sqrt(max(0.0, p.dt2 / p.n - mu * mu));
    const char *kind = dist(a, b) > 1 ? "further" : pix_row[a] == pix_row[b] ? "side by side" : pix_col[a] == pix_col[b] ? "above/below" : "diagonal";
    printf("  %2d+%-2d   (%d,%d)+(%d,%d)   %-12s  %8ld       %7.1f     %.2f (%.2f-%.2f)                   %5.0f / %4.0f\n", a, b,
           pix_row[a], pix_col[a], pix_row[b], pix_col[b], kind, p.n, pair_acc[order[i].second], qu(0.5), qu(0.1), qu(0.9), 1000 * mu, 1000 * rms);
  }
  printf("  (by chance: each cluster's seed with an independent in-time pulse in the other pixel, from that pixel's own rate)\n");
  for (auto &g : g_scan) { delete g ; g = nullptr ; }
}

void scan_thresholds(const char *mv_list)
{
  if (!prepare()) return;
  vector<double> thr;
  istringstream is(mv_list);
  for (double x; is >> x;) thr.push_back(x);
  for (auto &g : g_scan) { delete g ; g = new TGraph(thr.size()) ; }
  printf("clusters: neighbours per cluster vs neighbour threshold (seed >= %.1f mV, |dt| <= %.2f ns)\n", seed_mv, dt_cut);
  printf("  threshold    side/above-below   by chance      diagonal   by chance\n");
  for (size_t i = 0; i < thr.size(); i++) {
    Count c = count_at(thr[i]);
    double n = max<long>(c.clusters, 1);
    printf("  %6.1f mV      %8.4f          %8.4f      %8.4f   %8.4f\n", thr[i], c.edge / n, c.edge_acc / n, c.diag / n, c.diag_acc / n);
    g_scan[0]->SetPoint(i, thr[i], c.edge / n) ; g_scan[1]->SetPoint(i, thr[i], c.edge_acc / n);
    g_scan[2]->SetPoint(i, thr[i], c.diag / n) ; g_scan[3]->SetPoint(i, thr[i], c.diag_acc / n);
  }
}

// --- display ------------------------------------------------------------------------------------
static TCanvas *c_clu = nullptr, *c_ex = nullptr;

static void zoom_to_content(TH2 *h, double margin)
{
  int x0 = h->GetNbinsX(), x1 = 0, y0 = h->GetNbinsY(), y1 = 0;
  for (int i = 1; i <= h->GetNbinsX(); i++)
    for (int j = 1; j <= h->GetNbinsY(); j++)
      if (h->GetBinContent(i, j) > 0) { x0 = min(x0, i) ; x1 = max(x1, i) ; y0 = min(y0, j) ; y1 = max(y1, j) ; }
  if (x1 < x0) return;
  h->GetXaxis()->SetRangeUser(h->GetXaxis()->GetBinLowEdge(x0) - margin, h->GetXaxis()->GetBinUpEdge(x1) + margin);
  h->GetYaxis()->SetRangeUser(h->GetYaxis()->GetBinLowEdge(y0) - margin, h->GetYaxis()->GetBinUpEdge(y1) + margin);
}

void draw_clusters()
{
  if (!h_size) make_clusters();
  if (!h_size) return;
  if (!g_scan[0]) scan_thresholds();
  gStyle->SetOptStat(0);
  gStyle->SetNumberContours(64);
  gStyle->SetPaintTextFormat(".0f");
  if (!tb::canvas_open(c_clu)) { c_clu = new TCanvas("c_clusters", "clusters", 1600, 1050) ; c_clu->Divide(3, 3) ; }
  string cutx = Form("seed #geq %.0f mV, neighbours #geq %.1f mV, |#Deltat| #leq %.1f ns", seed_mv, nb_mv, dt_cut);

  c_clu->cd(1) ; gPad->SetLogy() ; h_size->SetTitle(Form("pixels per cluster (%s);pixels;clusters", cutx.c_str())) ; h_size->Draw("hist text0");
  c_clu->cd(2) ; gPad->SetRightMargin(0.15) ; zoom_to_content(h_centroid, 0.3) ; h_centroid->Draw("colz");
  {                                              // pixel edges and channel numbers in the zoomed range
    TAxis *ax = h_centroid->GetXaxis(), *ay = h_centroid->GetYaxis();
    double x0 = ax->GetBinLowEdge(ax->GetFirst()), x1 = ax->GetBinUpEdge(ax->GetLast());
    double y0 = ay->GetBinLowEdge(ay->GetFirst()), y1 = ay->GetBinUpEdge(ay->GetLast());
    for (int ch = 0; ch < NSIG; ch++) {
      if (pix_row[ch] < 0 || pix_col[ch] - 0.45 < x0 || pix_col[ch] - 0.45 > x1 || pix_row[ch] + 0.38 < y0 || pix_row[ch] + 0.38 > y1) continue;
      auto *t = new TLatex(pix_col[ch] - 0.45, pix_row[ch] + 0.38, Form("%d", ch));
      t->SetTextSize(0.035) ; t->SetTextColor(kGray + 2) ; t->Draw();
    }
    for (int k = 0; k <= max(nrows, ncols); k++) {
      double e = k - 0.5;
      if (e > x0 && e < x1) { auto *l = new TLine(e, y0, e, y1) ; l->SetLineColor(kGray) ; l->SetLineStyle(2) ; l->Draw() ; }
      if (e > y0 && e < y1) { auto *l = new TLine(x0, e, x1, e) ; l->SetLineColor(kGray) ; l->SetLineStyle(2) ; l->Draw() ; }
    }
  }

  c_clu->cd(3) ; gPad->SetLogy();
  h_nb_edge->SetLineColor(kBlue + 1) ; h_nb_diag->SetLineColor(kRed + 1);
  h_nb_edge->Draw("hist") ; h_nb_diag->Draw("hist same");
  {
    auto *l = new TLine(nb_mv, 0.5, nb_mv, h_nb_edge->GetMaximum()) ; l->SetLineStyle(2) ; l->Draw();
    auto *leg = new TLegend(0.45, 0.7, 0.88, 0.88);
    leg->AddEntry(h_nb_edge, "side by side, above/below", "l") ; leg->AddEntry(h_nb_diag, "diagonal", "l") ; leg->Draw();
  }
  c_clu->cd(4) ; gPad->SetRightMargin(0.15) ; gPad->SetLogz() ; h_dt_amp->Draw("colz");
  for (double y : {-dt_cut, dt_cut}) { auto *l = new TLine(0, y, 30, y) ; l->SetLineStyle(2) ; l->Draw() ; }
  c_clu->cd(5) ; gPad->SetRightMargin(0.15) ; h_nb_seed->Draw("colz");
  c_clu->cd(6) ; h_share->Draw("hist");

  c_clu->cd(7) ; gPad->SetRightMargin(0.12);
  zoom_to_content(h_pairs, 1) ; h_pairs->SetMarkerSize(1.6) ; h_pairs->Draw("colz text");
  c_clu->cd(8);
  int colors[4] = {kGreen + 2, kBlue + 1, kRed + 1, kBlack};
  auto *leg = new TLegend(0.55, 0.66, 0.88, 0.88);
  for (int i : {3, 0, 1, 2}) {
    h_q[i]->SetLineColor(colors[i]);
    h_q[i]->SetTitle(Form("summed charge of the cluster (%s);charge [pC];clusters", have_qw() ? "fixed window" : "around each pulse"));
    h_q[i]->Draw(i == 3 ? "hist" : "hist same");
    leg->AddEntry(h_q[i], i == 0 ? "1 pixel" : i == 1 ? "2 pixels" : i == 2 ? "3+ pixels" : "all", "l");
  }
  leg->Draw();

  c_clu->cd(9) ; gPad->SetLogy() ; gPad->SetGridx() ; gPad->SetGridy();
  auto *fr = gPad->DrawFrame(0, 1e-4, 10, 1, "neighbours per cluster vs neighbour threshold;neighbour threshold [mV];neighbours per cluster");
  (void)fr;
  int sc[4] = {kBlue + 1, kBlue + 1, kRed + 1, kRed + 1};
  auto *l9 = new TLegend(0.45, 0.68, 0.88, 0.88);
  for (int i = 0; i < 4; i++) {
    g_scan[i]->SetLineColor(sc[i]) ; g_scan[i]->SetMarkerColor(sc[i]) ; g_scan[i]->SetLineStyle(i % 2 ? 2 : 1) ; g_scan[i]->SetMarkerStyle(i % 2 ? 24 : 20);
    g_scan[i]->Draw("LP");
  }
  l9->AddEntry(g_scan[0], "side by side, above/below", "lp") ; l9->AddEntry(g_scan[2], "diagonal", "lp");
  l9->AddEntry(g_scan[1], "dashed: by chance", "l") ; l9->Draw();
  auto *l = new TLine(nb_mv, 1e-4, nb_mv, 1) ; l->SetLineStyle(2) ; l->Draw();
  c_clu->Update();
}

void draw_examples(int n, int min_size)
{
  if (!h_size) make_clusters();
  vector<const Example *> pick;
  for (auto &x : examples) if ((int)x.members.size() >= min_size && (int)pick.size() < n) pick.push_back(&x);
  if (pick.empty()) { printf("clusters: no saved waveforms of clusters with %d+ pixels (stage 1 keeps the first %d events with 2+ channels above 3 mV)\n", min_size, max_trace_events) ; return ; }
  int nx = (int)ceil(sqrt(pick.size() * 1.5)), ny = (pick.size() + nx - 1) / nx;
  if (tb::canvas_open(c_ex)) delete c_ex;   // not if its window was closed: already deleted
  c_ex = new TCanvas("c_examples", "example clusters", 380 * nx, 300 * ny);
  c_ex->Divide(nx, ny);
  static const int colors[9] = {kRed + 1, kBlue + 1, kGreen + 2, kMagenta + 1, kOrange + 7, kCyan + 2, kViolet + 1, kYellow + 2, kGray + 2};
  for (size_t i = 0; i < pick.size(); i++) {
    c_ex->cd(i + 1);
    const Example &x = *pick[i];
    double vmin = -5;                              // frame: down to the deepest member
    for (int ch : x.members)
      for (size_t j = trace_index[x.event]; j < traces.size() && traces[j].event == x.event; j++)
        if (traces[j].ch == ch) for (float v : traces[j].v) vmin = min<double>(vmin, v);
    auto *fr = gPad->DrawFrame(t_peak - 4, 1.25 * vmin, t_peak + 8, -0.3 * vmin);
    string title = Form("event %d:", x.event);
    for (int ch : x.members) title += Form(" ch%d", ch);
    fr->SetTitle((title + ";t - group trigger (lined up) [ns];mV").c_str());
    auto *leg = new TLegend(0.62, 0.15, 0.95, 0.15 + 0.07 * x.members.size());
    for (size_t k = 0; k < x.members.size(); k++) {
      int ch = x.members[k];
      for (size_t j = trace_index[x.event]; j < traces.size() && traces[j].event == x.event; j++) {
        if (traces[j].ch != ch) continue;
        auto *g = new TGraph(traces[j].t.size());
        for (size_t s = 0; s < traces[j].t.size(); s++) g->SetPoint(s, traces[j].t[s] - toff.ns[ch] - grp_off[ch / 8], traces[j].v[s]);
        g->SetLineColor(colors[k % 9]) ; g->SetLineWidth(2) ; g->Draw("L");
        leg->AddEntry(g, Form("ch %d (%d,%d)", ch, pix_row[ch], pix_col[ch]), "l");
      }
    }
    leg->Draw();
    auto *l = new TLine(t_peak - 4, -nb_mv, t_peak + 8, -nb_mv) ; l->SetLineStyle(2) ; l->SetLineColor(kGray + 1) ; l->Draw();
  }
  c_ex->Update();
}

static TCanvas *c_dist = nullptr;

void draw_distance()
{
  if (!prepare()) return;
  tb::Db db;
  double pitch = db.load(detector_name) ? db.num("PITCH_X_MM", 1) : 1;
  double rate[NSIG] = {};   // in-time pulses above the neighbour threshold per event, per channel
  for (auto &r : rows) for (int ch = 0; ch < NSIG; ch++) if (pix_row[ch] >= 0 && r.amp[ch] >= nb_mv && in_time(r, ch, dt_cut)) rate[ch]++;
  for (double &x : rate) x /= rows.size();
  double tmax = 0;
  for (auto &r : rows) for (int ch = 0; ch < NSIG; ch++) if (!std::isnan(r.t[ch])) tmax = max<double>(tmax, r.t[ch]);
  double dtmax = tmax - t_peak + 2;

  struct D { long chances = 0, prompt = 0, late = 0 ; double chance = 0, dt = 0, dt2 = 0 ; };
  map<int, D> by;           // key: distance in 1/100 pixel
  delete gROOT->FindObject("dist_dt");
  auto *h2 = new TH2F("dist_dt", Form("pulses >= %.1f mV around the seed: time - seed time vs distance;distance from the seed [mm];#Deltat [ns]", nb_mv),
                      int(8 * pitch / 0.25), 0, 8 * pitch, int((dtmax + 4) / 0.1), -4, dtmax);
  h2->SetDirectory(nullptr);
  const char *cls[3] = {"touching, side by side", "touching, diagonal", "further away"};
  TH1F *hdt[3];
  for (int i = 0; i < 3; i++) {
    delete gROOT->FindObject(Form("dt_class%d", i));
    hdt[i] = new TH1F(Form("dt_class%d", i), "time - seed time, per pixel looked at;#Deltat [ns];pulses per pixel and 0.1 ns", int((dtmax + 4) / 0.1), -4, dtmax);
    hdt[i]->SetDirectory(nullptr);
  }
  long n_cls[3] = {};
  for (auto &r : rows) {
    int s = seed_of(r);
    if (s < 0) continue;
    double ts = t_al(r, s);
    for (int ch = 0; ch < NSIG; ch++) {
      if (ch == s || pix_row[ch] < 0) continue;
      double dpx = hypot(pix_row[ch] - pix_row[s], pix_col[ch] - pix_col[s]);
      int k = dist(ch, s) > 1 ? 2 : edge(ch, s) ? 0 : 1;
      D &d = by[int(dpx * 100 + 0.5)];
      d.chances++ ; d.chance += rate[ch] ; n_cls[k]++;
      if (!(r.amp[ch] >= nb_mv) || std::isnan(r.t[ch])) continue;
      double dt = t_al(r, ch) - ts;
      h2->Fill(dpx * pitch, dt);
      hdt[k]->Fill(dt);
      if (fabs(dt) <= dt_cut) { d.prompt++ ; d.dt += dt ; d.dt2 += dt * dt ; }
      else if (dt > dt_cut) d.late++;
    }
  }
  for (int i = 0; i < 3; i++) if (n_cls[i]) hdt[i]->Scale(1.0 / n_cls[i]);

  printf("clusters: pixels around the seed by distance (pulse >= %.1f mV; prompt = within %.1f ns of the seed, late = after that, up to %.0f ns)\n",
         nb_mv, dt_cut, dtmax);
  printf("  distance        pixels looked at   prompt per pixel   by chance   ratio   prompt dt rms [ps]   late per pixel\n");
  auto *g_obs = new TGraph, *g_acc = new TGraph, *g_late = new TGraph;
  for (auto &x : by) {
    D &d = x.second;
    double dpx = x.first / 100.0, p = double(d.prompt) / d.chances, c = d.chance / d.chances, l = double(d.late) / d.chances;
    double mu = d.prompt ? d.dt / d.prompt : 0, rms = d.prompt ? sqrt(max(0.0, d.dt2 / d.prompt - mu * mu)) : 0;
    printf("  %4.2f px %5.1f mm   %10ld        %9.5f        %9.5f   %6.1f      %6.0f            %9.5f\n", dpx, dpx * pitch, d.chances, p, c,
           c > 0 ? p / c : 0, 1000 * rms, l);
    if (d.chances < 1000) continue;     // too few pixels that far for a point on the plot
    int i = g_obs->GetN();
    g_obs->SetPoint(i, dpx * pitch, max(p, 1e-6)) ; g_acc->SetPoint(i, dpx * pitch, max(c, 1e-6)) ; g_late->SetPoint(i, dpx * pitch, max(l, 1e-6));
  }

  gStyle->SetOptStat(0);
  if (tb::canvas_open(c_dist)) delete c_dist;   // not if its window was closed: already deleted
  c_dist = new TCanvas("c_distance", "distance from the seed", 1600, 520);
  c_dist->Divide(3, 1);
  c_dist->cd(1) ; gPad->SetLogy() ; gPad->SetGridy();
  auto *fr = gPad->DrawFrame(0, 1e-5, 8 * pitch, 1, Form("pixels around the seed: pulse #geq %.1f mV per pixel;distance from the seed [mm];pulses per pixel", nb_mv));
  (void)fr;
  g_obs->SetMarkerStyle(20) ; g_obs->SetMarkerColor(kBlue + 1) ; g_obs->SetLineColor(kBlue + 1) ; g_obs->Draw("LP");
  g_acc->SetMarkerStyle(24) ; g_acc->SetMarkerColor(kBlue + 1) ; g_acc->SetLineColor(kBlue + 1) ; g_acc->SetLineStyle(2) ; g_acc->Draw("LP");
  g_late->SetMarkerStyle(21) ; g_late->SetMarkerColor(kRed + 1) ; g_late->SetLineColor(kRed + 1) ; g_late->Draw("LP");
  auto *leg = new TLegend(0.40, 0.72, 0.88, 0.88);
  leg->AddEntry(g_obs, Form("prompt (|#Deltat| #leq %.1f ns)", dt_cut), "lp") ; leg->AddEntry(g_acc, "prompt, by chance", "lp");
  leg->AddEntry(g_late, Form("late (#Deltat > %.1f ns)", dt_cut), "lp") ; leg->Draw();
  c_dist->cd(2) ; gPad->SetLogz() ; gPad->SetRightMargin(0.14) ; h2->Draw("colz");
  c_dist->cd(3) ; gPad->SetLogy();
  int col[3] = {kBlue + 1, kGreen + 2, kRed + 1};
  auto *leg3 = new TLegend(0.45, 0.72, 0.88, 0.88);
  double ymax = 0, ymin = 1e9;
  for (auto *h : hdt) { ymax = max(ymax, h->GetMaximum()) ; for (int b = 1; b <= h->GetNbinsX(); b++) if (h->GetBinContent(b) > 0) ymin = min(ymin, h->GetBinContent(b)) ; }
  for (int i = 0; i < 3; i++) {
    hdt[i]->SetLineColor(col[i]) ; hdt[i]->SetMaximum(2 * ymax) ; hdt[i]->SetMinimum(0.5 * ymin);
    hdt[i]->Draw(i ? "hist same" : "hist");
    leg3->AddEntry(hdt[i], cls[i], "l");
  }
  leg3->Draw();
  c_dist->Update();
}

// --- charge of one photoelectron -----------------------------------------------------------
static string spot_list = "";
void set_spot(const char *channels) { spot_list = channels ? channels : "" ; }

static vector<int> spot_pixels()   // set_spot(), or the pixels with >= 10 % of the busiest one's in-time pulses
{
  vector<int> spot;
  if (!spot_list.empty()) {
    istringstream is(spot_list);
    for (int ch; is >> ch;) if (ch >= 0 && ch < NSIG && pix_row[ch] >= 0) spot.push_back(ch);
    return spot;
  }
  double n[NSIG] = {}, top = 0;
  for (auto &r : rows) for (int ch = 0; ch < NSIG; ch++) if (pix_row[ch] >= 0 && r.amp[ch] >= seed_mv && in_time(r, ch, seed_win)) n[ch]++;
  for (double x : n) top = max(top, x);
  for (int ch = 0; ch < NSIG; ch++) if (top > 0 && n[ch] >= 0.1 * top) spot.push_back(ch);
  return spot;
}

void suggest_charge_window()
{
  set<int> masked = tb::masked_channels(masked_list);
  TProfile *p[NTRIG];
  int n[NTRIG] = {};
  for (int g = 0; g < NTRIG; g++) { p[g] = new TProfile(Form("avg_pulse_g%d", g), "", 200, cuts.sig_from - 5, cuts.sig_to + 15) ; p[g]->SetDirectory(nullptr) ; }
  for (auto &tr : traces) {
    if (masked.count(tr.ch) || tr.v.empty() || *min_element(tr.v.begin(), tr.v.end()) > -5) continue;
    n[tr.ch / 8]++;
    for (size_t k = 0; k < tr.t.size(); k++) p[tr.ch / 8]->Fill(tr.t[k], tr.v[k]);
  }
  printf("clusters: average pulse (>= 5 mV) per group, relative to the group trigger; window = where it is below 5 %% of its depth, +-0.2 ns\n");
  int best = -1;
  double lo[NTRIG], hi[NTRIG];
  for (int g = 0; g < NTRIG; g++) {
    if (n[g] < 20) { printf("  group %d: %d pulses, too few\n", g, n[g]) ; continue ; }
    int bmin = p[g]->GetMinimumBin(), a = bmin, b = bmin;
    double depth = p[g]->GetBinContent(bmin);
    while (a > 1 && p[g]->GetBinContent(a - 1) < 0.05 * depth) a--;
    while (b < p[g]->GetNbinsX() && p[g]->GetBinContent(b + 1) < 0.05 * depth) b++;
    lo[g] = floor(10 * (p[g]->GetXaxis()->GetBinLowEdge(a) - 0.2)) / 10;
    hi[g] = ceil(10 * (p[g]->GetXaxis()->GetBinUpEdge(b) + 0.2)) / 10;
    printf("  group %d: %4d pulses, deepest %.1f mV at %.2f ns -> %.1f to %.1f ns\n", g, n[g], -depth, p[g]->GetBinCenter(bmin), lo[g], hi[g]);
    if (best < 0 || n[g] > n[best]) best = g;
  }
  for (auto *h : p) delete h;
  if (best < 0) { printf("  no waveforms with pulses (stage 1, or load_events of a file with traces)\n") ; return ; }
  printf("suggestion: set_charge_window(%.1f, %.1f)   (one window for all groups: groups 0/1 sit ~2 ns earlier than 2/3)\n", lo[best], hi[best]);
}

static TCanvas *c_q = nullptr;
static TH1F *h_spot = nullptr, *h_pix[NSIG] = {};
static void print_fit(const char *what, const tb::SpeFit &f)
{
  static const char *model[] = {"Gauss", "Polya", "Polya+under"};
  printf("  %-14s %s %s: Q1 = %.4f +- %.4f pC (gain %.3g), s1 = %.4f pC, under-amplified %.0f %% (mean %.4f pC per p.e.), mu = %.4f +- %.4f, "
         "pedestal %.4f, sigma %.4f pC, chi2/ndf %.2f%s\n", what, f.ok ? "" : "(NO CONVERGENCE)", model[f.model], f.q1, f.eq1, f.gain, f.s1,
         100 * f.w, f.mean_q, f.mu, f.emu, f.q0, f.s0, f.chi2ndf, f.resolved ? "" : "  not resolved");
}

void draw_charge()
{
  if (!prepare()) return;
  if (!have_qw()) { printf("clusters: no fixed-window charge in these events: stage 1 again with set_charge_window() (see suggest_charge_window())\n") ; return ; }
  if (!h_size) make_clusters();
  vector<int> spot;
  for (int ch : spot_pixels()) {
    if (qw_fits(ch)) spot.push_back(ch);
    else printf("clusters: ch %d left out of the spot: its group's pulses are %.1f ns off the fixed charge window\n", ch, grp_off[ch / 8]);
  }
  if (spot.empty()) { printf("clusters: no spot pixels\n") ; return ; }
  string names;
  for (int ch : spot) names += (names.empty() ? "" : " ") + to_string(ch);

  delete h_spot;
  for (auto &h : h_pix) { delete h ; h = nullptr ; }
  h_spot = new TH1F("q_spot", Form("charge of ch %s summed, every event;charge [pC];events", names.c_str()), 340, -0.2, 1.5);
  h_spot->SetDirectory(nullptr);
  for (int ch : spot) { h_pix[ch] = new TH1F(Form("q_pix%02d", ch), "", 340, -0.2, 1.5) ; h_pix[ch]->SetDirectory(nullptr) ; }
  for (auto &r : rows) {
    double sum = 0;
    bool ok = true;
    for (int ch : spot) { if (std::isnan(r.qw[ch])) ok = false ; else { sum += r.qw[ch] ; h_pix[ch]->Fill(r.qw[ch]) ; } }
    if (ok) h_spot->Fill(sum);
  }

  printf("clusters: single-photoelectron fits (pedestal + Poisson-weighted Gaussians), fixed-window charge, %zu events\n", rows.size());
  tb::SpeFit f = tb::fit_spe(h_spot);
  if (!f.f) { printf("  too few events\n") ; return ; }
  f.f->SetName("spe_spot");   // tb::fit_spe reuses its name: keep this one
  print_fit(("spot (" + to_string(spot.size()) + " px)").c_str(), f);
  vector<tb::SpeFit> fp;
  for (int ch : spot) {
    fp.push_back(tb::fit_spe(h_pix[ch]));
    if (fp.back().f) fp.back().f->SetName(Form("spe_pix%02d", ch));
    print_fit(Form("ch %d alone", ch), fp.back());
  }
  printf("  (one pixel alone loses the charge shared with its neighbours; the spot sum counts each photoelectron whole)\n");

  gStyle->SetOptStat(0);
  if (!tb::canvas_open(c_q)) { c_q = new TCanvas("c_charge", "charge of one photoelectron", 1500, 1000) ; c_q->Divide(2, 2) ; }
  vector<TF1 *> comp;
  int ccol[4] = {kGray + 2, kBlue + 1, kGreen + 2, kMagenta + 1};
  for (int n = 0; n < 4; n++) {
    delete gROOT->GetListOfFunctions()->FindObject(Form("spe_term%d", n));
    auto *c = tb::spe_function(Form("spe_term%d", n), -0.2, 1.5, f.model, n);
    c->SetParameters(f.par);
    c->SetNpx(1000) ; c->SetLineColor(ccol[n]) ; c->SetLineStyle(2) ; c->SetLineWidth(2);
    comp.push_back(c);
  }
  f.f->SetLineColor(kRed) ; f.f->SetLineWidth(2);

  c_q->cd(1) ; gPad->SetLogy();
  h_spot->GetXaxis()->SetRangeUser(-0.15, min(1.5, f.q0 + 5 * f.q1));
  h_spot->SetMinimum(0.5);
  h_spot->Draw("hist");
  f.f->Draw("same");
  for (auto *c : comp) c->Draw("same");
  auto *pt = new TPaveText(0.5, 0.52, 0.89, 0.89, "NDC");
  pt->SetFillColor(0) ; pt->SetBorderSize(1) ; pt->SetTextAlign(12);
  pt->AddText(Form("Q_{1} = %.4f #pm %.4f pC", f.q1, f.eq1));
  pt->AddText(Form("gain = %.3g", f.gain));
  pt->AddText(Form("#sigma_{1} = %.4f pC (%.0f %% of Q_{1})", f.s1, 100 * f.s1 / f.q1));
  pt->AddText(Form("#mu = %.4f #pm %.4f p.e. per event", f.mu, f.emu));
  pt->AddText(Form("pedestal %.4f pC, #sigma_{0} = %.4f pC", f.q0, f.s0));
  pt->AddText(Form("#chi^{2}/ndf = %.2f%s", f.chi2ndf, f.ok ? "" : "  (no convergence)"));
  pt->Draw();
  auto *lg = new TLegend(0.12, 0.12, 0.42, 0.34);
  lg->AddEntry(f.f, "fit", "l");
  for (int n = 0; n < 4; n++) lg->AddEntry(comp[n], n == 0 ? "pedestal" : Form("%d p.e.", n), "l");
  lg->Draw();

  c_q->cd(2);                                     // above the pedestal, linear
  TH1 *hz = h_spot->DrawCopy("hist");
  hz->SetTitle("above the pedestal;charge [pC];events");
  double x0 = f.q0 + 3 * f.s0, x1 = min(1.5, f.q0 + 4 * f.q1);
  hz->GetXaxis()->SetRangeUser(x0, x1);
  double ymax = 0;
  for (int b = hz->FindBin(x0); b <= hz->FindBin(x1); b++) ymax = max(ymax, hz->GetBinContent(b));
  hz->SetMinimum(0) ; hz->SetMaximum(1.3 * ymax);
  f.f->Draw("same");
  for (int n = 1; n < 4; n++) comp[n]->Draw("same");

  c_q->cd(3) ; gPad->SetLogy();                   // each spot pixel on its own
  auto *lp = new TLegend(0.45, 0.6, 0.89, 0.89);
  int pcol[6] = {kBlack, kBlue + 1, kRed + 1, kGreen + 2, kMagenta + 1, kOrange + 7};
  for (size_t i = 0; i < spot.size(); i++) {
    TH1F *h = h_pix[spot[i]];
    h->SetTitle("one pixel at a time (fixed window, every event);charge [pC];events");
    h->SetLineColor(pcol[i % 6]) ; h->SetMinimum(0.5) ; h->GetXaxis()->SetRangeUser(-0.15, min(1.5, f.q0 + 4 * f.q1));
    h->Draw(i ? "hist same" : "hist");
    lp->AddEntry(h, fp[i].f ? Form("ch %d: Q_{1} %.3f pC, #mu %.3f", spot[i], fp[i].q1, fp[i].mu) : Form("ch %d", spot[i]), "l");
  }
  lp->Draw();

  c_q->cd(4);                                     // clusters, with the photoelectron peaks of the fit
  int colors[4] = {kGreen + 2, kBlue + 1, kRed + 1, kBlack};
  auto *lc = new TLegend(0.55, 0.62, 0.88, 0.88);
  for (int i : {3, 0, 1, 2}) {
    h_q[i]->SetLineColor(colors[i]);
    h_q[i]->SetTitle(Form("summed charge of the cluster (fixed window), seed #geq %.0f mV, neighbours #geq %.1f mV;charge [pC];clusters", seed_mv, nb_mv));
    h_q[i]->Draw(i == 3 ? "hist" : "hist same");
    lc->AddEntry(h_q[i], i == 0 ? "1 pixel" : i == 1 ? "2 pixels" : i == 2 ? "3+ pixels" : "all", "l");
  }
  for (int n = 1; n <= 3; n++) {
    double x = f.q0 + n * f.q1;
    if (x > h_q[3]->GetXaxis()->GetXmax()) break;
    auto *l = new TLine(x, 0, x, h_q[3]->GetMaximum()) ; l->SetLineStyle(2) ; l->SetLineColor(kRed) ; l->Draw();
  }
  lc->AddEntry((TObject *)nullptr, "dashed: n #times Q_{1} of the spot fit", "");
  lc->Draw();
  c_q->Update();
}

void save_plots(const char *prefix)
{
  if (!tb::canvas_open(c_clu)) draw_clusters();
  if (tb::canvas_open(c_clu)) c_clu->SaveAs(Form("%s_clusters.png", prefix));
  if (!tb::canvas_open(c_ex)) draw_examples();
  if (tb::canvas_open(c_ex)) c_ex->SaveAs(Form("%s_examples.png", prefix));
  if (tb::canvas_open(c_dist)) c_dist->SaveAs(Form("%s_distance.png", prefix));
  if (tb::canvas_open(c_q)) c_q->SaveAs(Form("%s_charge.png", prefix));
}
