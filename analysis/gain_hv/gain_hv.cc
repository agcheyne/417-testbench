// gain_hv - gain vs HV from single-photoelectron fits, one run per HV point.
//
//   ./gain_hv.sh
//   root [1] set_charge_window(21, 30)                // from general's suggest_windows()
//   root [2] set_hv_electrode("")                     // or set_hv_channel(236, 0)
//   root [3] add_run("/scratch2/data/hpDIRC/photek/photek-00000101-0000.evt")   // one per HV
//   root [4] add_run(...) ...                         // or add_runs("series.txt")
//   root [5] fit_all() ; gain_curve(20) ; working_point(20, 1e6) ; save()
//
// After an HV scan (bin/scan_hv.sh), the same in batch with the plots saved in the scan's folder:
//   ./fit_hv_scan.sh ../../logs/scans/<scan> --spot "20 12 19 21 22 23"     (runs fit_hv_scan.C)
//
// set_spot() adds one more spectrum per run, the charge summed over a group of pixels, for
// photoelectrons that share their charge between pads (on the Photonis, ch20 alone gives Q1
// about 0.21 pC, the spot 0.31 pC). It is index 32 (SPOT) in the functions below.
//
// Per run and channel: the charge in a FIXED window after the trigger, so the pedestal is in
// the spectrum, fitted with the Poisson x Gauss single-photoelectron model (lib/tb_spe.h):
// gain = Q1 / e. The HV of each run is the CAEN readback (VMON) in begin-run packet 941 of the
// chosen channel, unless given to add_run(). The light must be low for the fit to work:
// mu (photoelectrons per pulse) ~ 0.1-1; fits with mu > 3 are flagged and left out of the curve.

#include <iostream>
#include <pmonitor/pmonitor.h>
#include "gain_hv.h"

#include "tb_db.h"
#include "tb_spe.h"
#include "tb_v1742.h"

#include <TCanvas.h>
#include <TColor.h>
#include <TF1.h>
#include <TFile.h>
#include <TGraphErrors.h>
#include <TH1.h>
#include <TLegend.h>
#include <TMultiGraph.h>
#include <TProfile.h>
#include <TStyle.h>
#include <TMath.h>
#include <TSystem.h>

#include <algorithm>
#include <set>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace std;
using tb::NSIG;

int init_done = 0;

// --- settings -------------------------------------------------------------------------------
static tb::Cuts cuts;
static int n_events_per_run = 20000;
static string hv_electrode;
static int hv_unit = -1, hv_ch = -1;
static double q_lo = -0.2, q_hi = 2.0;
static int q_bins = 1100;
static vector<int> spot_chs;              // channels summed into the extra spectrum SPOT
const int SPOT = NSIG;                    // index of the spot sum in q[] / fit[] and the functions below
static int nq() { return spot_chs.empty() ? NSIG : NSIG + 1 ; }
static string chname(int ch) { return ch == SPOT ? string("spot") : Form("ch %d", ch) ; }
static string chtag(int ch) { return ch == SPOT ? string("spot") : Form("ch%02d", ch) ; }

void set_charge_window(double a, double b) { cuts.q_from = a ; cuts.q_to = b ; }
void set_events(int n) { n_events_per_run = n ; }
void set_hv_electrode(const char *e) { hv_electrode = e ? e : "" ; hv_unit = hv_ch = -1 ; }
void set_hv_channel(int unit, int ch) { hv_unit = unit ; hv_ch = ch ; }
void set_charge_range(double lo, double hi, int n) { q_lo = lo ; q_hi = hi ; q_bins = n ; }

// --- runs ---------------------------------------------------------------------------------------
struct RunData {
  string file;
  int run = -1;
  double hv = NAN;
  bool hv_given = false;
  bool hv_on = true;                       // from the 941 status bit; given HV = assumed on
  long events = 0;
  TH1F *q[NSIG + 1] = {};                  // + the spot sum
  tb::SpeFit fit[NSIG + 1];
};
static vector<RunData> runs;
static RunData *cur = nullptr;
static TProfile *find_prof = nullptr;     // find_window(): average pulse of the run being read
static long find_pulses = 0;
static set<int> masked;                   // MASKED_CHANNELS of db/testbench.conf: left out of find_window()
static bool masked_read = false;

void set_spot(const char *channels)
{
  if (!runs.empty()) printf("gain_hv: set_spot only applies to runs added after it\n");
  spot_chs.clear();
  istringstream is(channels ? channels : "");
  int ch;
  while (is >> ch)
    if (ch >= 0 && ch < NSIG) spot_chs.push_back(ch);
    else printf("gain_hv: set_spot: no channel %d\n", ch);
}
static tb::V1742Event ev;
static tb::RunInfo info;
static tb::Db db;
static tb::TimeOffsets toff;              // db/v1742 offsets: the charge window is shifted per input
static bool use_offsets = true, toff_tried = false;
static string toff_file = "?";

void set_time_offsets(bool on) { use_offsets = on ; toff_tried = false ; }

// the offsets for this V1742 and speed (once per run: the serial comes with the begin-run event)
static void load_offsets()
{
  if (toff_tried) return;
  toff_tried = true;
  if (!use_offsets) { toff = tb::TimeOffsets() ; return ; }
  toff.load(ev.serial, ev.freq);
  string f = toff.ok ? toff.file : "";
  if (f != toff_file)
    printf("gain_hv: charge window %s\n", toff.ok ? ("shifted per input by " + f).c_str()
                                                  : "the same for every input (no db/v1742 time offsets for this V1742 and speed)");
  toff_file = f;
}
static double offset(int ch) { return toff.ok ? toff.ns[ch] : 0 ; }

int pinit()
{
  if (init_done) return 1;
  init_done = 1;
  return 0;
}

// the HV of this run from packet 941: the chosen CAEN channel, else the detector's electrode
static double run_hv()
{
  int unit = hv_unit, ch = hv_ch;
  if (unit < 0) {
    if (!db.load(info.detector) || !db.hv_channel(hv_electrode, unit, ch)) {
      printf("gain_hv: run %d: no HV channel - set_hv_channel(serial, ch), or fill in hv.conf of detector %s%s\n",
             info.run, db.detector.empty() ? "(none)" : db.detector.c_str(),
             hv_electrode.empty() ? " (with exactly one cabled electrode, or set_hv_electrode)" : "");
      return NAN;
    }
  }
  double v = info.hv(unit, ch, "VMON");
  if (std::isnan(v)) printf("gain_hv: run %d: unit %d was not read out in packet 941\n", info.run, unit);
  else if (!(info.hv_status(unit, ch) & 1)) {
    printf("gain_hv: run %d: unit %d ch %d is OFF (VMON %.1f V): run left out of the gain curve\n", info.run, unit, ch, v);
    if (cur) cur->hv_on = false;
  }
  return v;
}

int process_event(Event *e)
{
  if (!cur && !find_prof) return 0;
  if (e->getEvtType() == 9) {
    info.parse(e);
    ev.serial = info.serial;
    toff_tried = false;
    if (cur) {
      cur->run = info.run;
      if (!cur->hv_given) cur->hv = run_hv();
    }
    return 0;
  }
  if (!ev.fill(e)) return 0;
  load_offsets();
  if (find_prof) {   // find_window(): average every pulse >= 5 mV, on the offset-corrected time axis
    tb::Cuts c;
    c.sig_from = find_prof->GetXaxis()->GetXmin();
    c.sig_to = find_prof->GetXaxis()->GetXmax();
    for (int ch = 0; ch < NSIG; ch++) {
      if (masked.count(ch)) continue;
      double tg = ev.trigger_of(ch);
      tb::ChannelHit h = tb::measure(ev, ch, c);
      if (!h.hit || std::isnan(tg)) continue;
      find_pulses++;
      for (int k = 0; k < ev.ns; k++) find_prof->Fill(ev.t[ch][k] - tg - offset(ch), ev.v[ch][k]);
    }
    return 0;
  }
  if (!cur->q[0])
    for (int ch = 0; ch < nq(); ch++)
      cur->q[ch] = new TH1F(Form("q_run%d_%s", cur->run, chtag(ch).c_str()),
                            Form("run %d %s charge (%.1f to %.1f ns after the trigger);charge [pC];events",
                                 cur->run, chname(ch).c_str(), cuts.q_from, cuts.q_to), q_bins, q_lo, q_hi);
  cur->events++;
  double qspot = 0;
  size_t nspot = 0;
  tb::Cuts c = cuts;
  for (int ch = 0; ch < NSIG; ch++) {
    c.q_from = cuts.q_from + offset(ch);
    c.q_to = cuts.q_to + offset(ch);
    tb::ChannelHit h = tb::measure(ev, ch, c);
    if (!h.valid) continue;
    cur->q[ch]->Fill(h.q);
    if (std::find(spot_chs.begin(), spot_chs.end(), ch) != spot_chs.end()) { qspot += h.q ; nspot++ ; }
  }
  if (cur->q[SPOT] && nspot == spot_chs.size()) cur->q[SPOT]->Fill(qspot);
  return 0;
}

void add_run(const char *filename, double hv_volts)
{
  if (std::isnan(cuts.q_from)) { printf("gain_hv: set_charge_window(from_ns, to_ns) first\n") ; return ; }
  // all parts of a run: name-0000.evt, name-0001.evt, ...
  vector<string> parts{filename};
  string f = filename;
  size_t k = f.rfind("-0000.evt");
  if (k != string::npos && k + 9 == f.size())
    for (int i = 1; i < 10000; i++) {
      string p = f.substr(0, k) + Form("-%04d.evt", i);
      if (gSystem->AccessPathName(p.c_str())) break;   // true = does not exist
      parts.push_back(p);
    }
  runs.emplace_back();
  cur = &runs.back();
  cur->file = filename;
  if (hv_volts >= 0) { cur->hv = hv_volts ; cur->hv_given = true ; }
  for (auto &p : parts) {
    if (pfileopen(p.c_str()) != 0) { printf("gain_hv: cannot open %s\n", p.c_str()) ; break ; }
    long left = n_events_per_run > 0 ? n_events_per_run - cur->events : 0;
    if (n_events_per_run > 0 && left <= 0) { pclose() ; break ; }
    prun(left);
    pclose();
  }
  printf("gain_hv: run %d, %ld events, HV %.1f V%s\n", cur->run, cur->events, cur->hv, cur->hv_given ? " (given)" : "");
  if (cur->events == 0) { printf("gain_hv: %s left out: no events\n", filename) ; runs.pop_back() ; }
  cur = nullptr;
}

// --- the charge window from the data ---------------------------------------------------------
// The average pulse (every unmasked input, >= 5 mV, times corrected by the db offsets) of each
// run: its window is where it is deeper than 5 % of its minimum, +- 0.2 ns (as clusters'
// suggest_charge_window()). The union over all runs, so no HV point is clipped, is set.
bool find_window(const char *listfile, int events)
{
  if (!masked_read) { masked = tb::masked_channels() ; masked_read = true ; }
  ifstream in(listfile);
  if (!in) { printf("gain_hv: cannot read %s\n", listfile) ; return false ; }
  printf("gain_hv: finding the charge window: average pulse of %d events per run, inputs %s left out\n", events,
         masked.empty() ? "none" : [] { string m ; for (int c : masked) m += (m.empty() ? "" : " ") + to_string(c) ; return m ; }().c_str());
  printf("  run      HV [V]   pulses   deepest [mV] at [ns]   window [ns]\n");
  double lo_all = 1e9, hi_all = -1e9;
  string line;
  while (getline(in, line)) {
    line = line.substr(0, line.find('#'));
    istringstream is(line);
    string file;
    double hv = NAN;
    if (!(is >> file)) continue;
    is >> hv;
    TProfile prof("find_prof", "", 600, 5, 65);   // 0.1 ns bins
    prof.SetDirectory(nullptr);
    find_prof = &prof;
    find_pulses = 0;
    info = tb::RunInfo();
    if (pfileopen(file.c_str()) != 0) { printf("gain_hv: cannot open %s\n", file.c_str()) ; find_prof = nullptr ; continue ; }
    prun(events);
    pclose();
    find_prof = nullptr;
    if (find_pulses < 50) { printf("  %-6d %8.1f   %6ld   too few pulses\n", info.run, hv, find_pulses) ; continue ; }
    int bmin = prof.GetMinimumBin(), a = bmin, b = bmin;
    double depth = prof.GetBinContent(bmin);
    while (a > 1 && prof.GetBinContent(a - 1) < 0.05 * depth) a--;
    while (b < prof.GetNbinsX() && prof.GetBinContent(b + 1) < 0.05 * depth) b++;
    double lo = floor(10 * (prof.GetXaxis()->GetBinLowEdge(a) - 0.2)) / 10;
    double hi = ceil(10 * (prof.GetXaxis()->GetBinUpEdge(b) + 0.2)) / 10;
    printf("  %-6d %8.1f   %6ld   %6.1f at %6.2f      %5.1f to %5.1f\n", info.run, hv, find_pulses, -depth, prof.GetBinCenter(bmin), lo, hi);
    lo_all = std::min(lo_all, lo);
    hi_all = std::max(hi_all, hi);
  }
  if (lo_all > hi_all) { printf("gain_hv: no run had enough pulses to find a window (no light?)\n") ; return false ; }
  set_charge_window(lo_all, hi_all);
  printf("gain_hv: charge window %.1f to %.1f ns after the group trigger (+ each input's db offset)\n", lo_all, hi_all);
  return true;
}

void add_runs(const char *listfile)
{
  ifstream in(listfile);
  if (!in) { printf("gain_hv: cannot read %s\n", listfile) ; return ; }
  string line;
  while (getline(in, line)) {
    line = line.substr(0, line.find('#'));
    istringstream is(line);
    string file;
    double hv = -1;
    if (!(is >> file)) continue;
    is >> hv;
    add_run(file.c_str(), hv);
  }
}

// --- results ------------------------------------------------------------------------------------
// a gain point: converged, a resolved photoelectron peak, light in the useful range
static bool usable(const tb::SpeFit &f) { return f.ok && f.resolved && f.mu > 0.02 && f.mu <= 3 ; }

void fit_all(int min_entries)
{
  int hidden = 0;
  printf("  run      HV [V]  ch    gain             mu [p.e.]        chi2/ndf  note\n");
  for (auto &r : runs)
    for (int ch = 0; ch < nq(); ch++) {
      TH1F *h = r.q[ch];
      if (!h || h->GetEntries() < min_entries) continue;
      delete r.fit[ch].f;
      r.fit[ch] = tb::fit_spe(h);
      const tb::SpeFit &f = r.fit[ch];
      if (!f.f || f.mu <= 0.02 || !f.resolved) { hidden++ ; continue ; }   // no light, or no peak above the pedestal noise
      const char *note = !f.ok ? "fit did not converge" : f.mu > 3 ? "mu > 3: too much light, not used"
                       : !r.hv_on ? "HV off" : "";
      printf("  %-6d %8.1f  %-4s  %.3e +- %.1e  %6.3f +- %.3f  %6.2f    %s\n",
             r.run, r.hv, ch == SPOT ? "spot" : Form("%d", ch), f.gain, f.egain, f.mu, f.emu, f.chi2ndf, note);
    }
  if (hidden) printf("  (%d run x channel spectra without a photoelectron peak 3 pedestal sigmas clear of the pedestal not shown)\n", hidden);
}

static map<int, TF1 *> curve_fit;   // per channel: gain = exp(p0 + p1 * V)

static TGraphErrors *curve(int ch)
{
  auto *g = new TGraphErrors();
  g->SetName(Form("gain_%s", chtag(ch).c_str()));
  g->SetTitle(Form("%s gain;HV [V];gain", chname(ch).c_str()));
  for (auto &r : runs)
    if (!std::isnan(r.hv) && r.hv_on && usable(r.fit[ch])) {
      int n = g->GetN();
      g->SetPoint(n, r.hv, r.fit[ch].gain);
      g->SetPointError(n, 0, r.fit[ch].egain);
    }
  return g;
}

// exponential fit of a gain curve, kept in curve_fit[ch] for working_point(); null with too few points
static TF1 *fit_curve(int ch, TGraphErrors *g)
{
  delete curve_fit[ch];
  curve_fit[ch] = nullptr;
  double lo = TMath::MinElement(g->GetN(), g->GetX()), hi = TMath::MaxElement(g->GetN(), g->GetX());
  if (g->GetN() < 2 || hi - lo < 10) {
    printf("gain_hv: %s: %d point(s) within %.1f V (gain %.3g at %.0f V) - need HV points at least 10 V apart for a curve\n",
           chname(ch).c_str(), g->GetN(), hi - lo, g->GetY()[0], g->GetX()[0]);
    return nullptr;
  }
  TF1 *f = new TF1(Form("gain_fit_%s", chtag(ch).c_str()), "expo", lo - 1, hi + 1);
  g->Fit(f, "Q0");
  curve_fit[ch] = f;
  double b = f->GetParameter(1);
  printf("%s: gain = exp(%.3f + %.5f V): x2 every %.1f V (%d points, chi2/ndf %.2f)\n", chname(ch).c_str(), f->GetParameter(0), b,
         log(2.0) / b, g->GetN(), f->GetNDF() > 0 ? f->GetChisquare() / f->GetNDF() : 0.0);
  return f;
}

void gain_curve(int ch)
{
  if (ch < 0 || ch >= nq()) return;
  TGraphErrors *g = curve(ch);
  if (g->GetN() == 0) {
    delete curve_fit[ch];
    curve_fit[ch] = nullptr;
    printf("gain_hv: %s has no usable fit (fit_all() first; mu must be 0.02-3)\n", chname(ch).c_str());
    return;
  }
  auto *c = new TCanvas(Form("c_gain_%s", chtag(ch).c_str()), Form("%s gain vs HV", chname(ch).c_str()), 800, 600);
  c->SetLogy();
  g->SetMarkerStyle(20);
  g->Draw("AP");
  if (TF1 *f = fit_curve(ch, g)) f->Draw("same");
}

double working_point(int ch, double target)
{
  if (!curve_fit.count(ch)) gain_curve(ch);
  if (!curve_fit[ch]) { printf("gain_hv: no gain curve for %s\n", chname(ch).c_str()) ; return NAN ; }
  TF1 *f = curve_fit[ch];
  double v = (log(target) - f->GetParameter(0)) / f->GetParameter(1);
  printf("%s: gain %.3g at %.1f V%s\n", chname(ch).c_str(), target, v,
         (v < f->GetXmin() || v > f->GetXmax()) ? " (extrapolated outside the measured range)" : "");
  return v;
}

void draw_spectrum(int run, int ch)
{
  for (auto &r : runs)
    if (r.run == run && ch >= 0 && ch < nq() && r.q[ch]) {
      auto *c = new TCanvas(Form("c_q_run%d_%s", run, chtag(ch).c_str()), Form("run %d %s", run, chname(ch).c_str()), 800, 600);
      c->SetLogy();
      r.q[ch]->Draw();
      if (r.fit[ch].f) { r.fit[ch].f->SetLineColor(kRed) ; r.fit[ch].f->Draw("same") ; }
      return;
    }
  printf("gain_hv: no run %d / channel %d (the spot sum is %d)\n", run, ch, SPOT);
}

void save(const char *prefix)
{
  string csv = string(prefix) + ".csv", root = string(prefix) + ".root";
  ofstream out(csv);
  out << "run,hv_V,hv_on,ch,gain,gain_err,mu,mu_err,q1_pC,q1_err,q0_pC,s0_pC,s1_pC,chi2ndf,converged,used\n";
  for (auto &r : runs)
    for (int ch = 0; ch < nq(); ch++) {
      const tb::SpeFit &f = r.fit[ch];
      if (!f.f || f.mu <= 0.02 || !f.resolved) continue;
      out << r.run << "," << r.hv << "," << r.hv_on << "," << (ch == SPOT ? string("spot") : to_string(ch)) << "," << f.gain << "," << f.egain << "," << f.mu << "," << f.emu << ","
          << f.q1 << "," << f.eq1 << "," << f.q0 << "," << f.s0 << "," << f.s1 << "," << f.chi2ndf << ","
          << f.ok << "," << (usable(f) && r.hv_on) << "\n";
    }
  TFile tf(root.c_str(), "RECREATE");
  for (auto &r : runs)
    for (int ch = 0; ch < nq(); ch++)
      if (r.q[ch]) r.q[ch]->Write();
  for (int ch = 0; ch < nq(); ch++) {
    TGraphErrors *g = curve(ch);
    if (g->GetN()) g->Write();
    delete g;
  }
  tf.Close();
  printf("gain_hv: wrote %s and %s\n", csv.c_str(), root.c_str());
}

// --- after an HV scan: gain and mu vs HV of chosen channels, and every run's spectrum -------------
// series colours in a fixed order (at most 8 channels in one plot)
static const char *series_hex[] = {"#2a78d6", "#eb6834", "#1baf7a", "#eda100", "#e87ba4", "#008300", "#6250d6", "#e34948"};
const int NSERIES = 8;

void plot_scan(const char *prefix, const char *channels, const char *exclude)
{
  vector<int> chs, skip;
  istringstream is(channels ? channels : ""), ix(exclude ? exclude : "");
  int c;
  while (is >> c)
    if (c >= 0 && c < nq()) chs.push_back(c);
    else printf("gain_hv: plot_scan: no channel %d\n", c);
  while (ix >> c) skip.push_back(c);
  if (chs.empty()) {   // every channel with gain points at 3 or more HV values, those with most points first
    vector<pair<int, int>> n;
    for (int ch = 0; ch < nq(); ch++) {
      if (std::find(skip.begin(), skip.end(), ch) != skip.end()) continue;
      TGraphErrors *g = curve(ch);
      if (g->GetN() >= 3) n.push_back({ch == SPOT ? -1000 : -g->GetN(), ch});
      delete g;
    }
    sort(n.begin(), n.end());
    for (auto &p : n) chs.push_back(p.second);
    if ((int)chs.size() > NSERIES) {
      printf("gain_hv: plot_scan: %zu channels have a curve, plotting the %d with most points (plot_scan(prefix, \"ch ...\") to choose)\n",
             chs.size(), NSERIES);
      chs.resize(NSERIES);
    }
    sort(chs.begin(), chs.end(), [](int a, int b) { return (a == SPOT) != (b == SPOT) ? a == SPOT : a < b ; });
  }
  if ((int)chs.size() > NSERIES) chs.resize(NSERIES);
  if (chs.empty()) { printf("gain_hv: plot_scan: no channel has gain points at 3 or more HV values (fit_all() first)\n") ; return ; }

  gStyle->SetOptStat(0);
  gStyle->SetOptTitle(0);
  gStyle->SetGridColor(kGray);
  gStyle->SetGridStyle(3);
  gStyle->SetEndErrorSize(4);

  // gain vs HV (log) and mu vs HV next to it: the light per pulse should not change with the HV.
  // Points with their (statistical) fit errors, joined by straight segments; no curve is fitted
  // into the plot, since log(gain) need not be linear in HV (fit_curve() still prints one)
  auto *cg = new TCanvas(Form("c_scan_gain_%s", gSystem->BaseName(prefix)), "gain vs HV", 1500, 650);
  cg->Divide(2, 1);
  auto *mg = new TMultiGraph("mg_gain", ";HV [V];gain (Q_{1} / e)");
  auto *mm = new TMultiGraph("mg_mu", ";HV [V];#mu [photoelectrons per pulse]");
  auto *lg = new TLegend(0.15, 0.88 - 0.06 * chs.size(), 0.40, 0.88);   // top left: gain rises with HV
  lg->SetBorderSize(0);
  lg->SetFillStyle(0);
  for (size_t i = 0; i < chs.size(); i++) {
    int ch = chs[i];
    int col = TColor::GetColor(series_hex[i]);
    TGraphErrors *g = curve(ch);
    auto *gm = new TGraphErrors();
    for (auto &r : runs)
      if (!std::isnan(r.hv) && r.hv_on && usable(r.fit[ch])) {
        int n = gm->GetN();
        gm->SetPoint(n, r.hv, r.fit[ch].mu);
        gm->SetPointError(n, 0, r.fit[ch].emu);
      }
    for (auto *x : {g, gm}) { x->SetMarkerStyle(20) ; x->SetMarkerSize(0.9) ; x->SetMarkerColor(col) ; x->SetLineColor(col) ; x->SetLineWidth(2) ; }
    if (g->GetN()) mg->Add(g, "PL");
    if (gm->GetN()) mm->Add(gm, "PL");
    if (g->GetN()) fit_curve(ch, g);   // printed and kept for working_point(), not drawn
    lg->AddEntry(g, chname(ch).c_str(), "lp");
  }
  cg->cd(1);
  gPad->SetLogy();
  gPad->SetGrid();
  gPad->SetLeftMargin(0.12);
  mg->Draw("A");
  lg->Draw();
  cg->cd(2);
  gPad->SetGrid();
  gPad->SetLeftMargin(0.12);
  mm->Draw("A");
  mm->SetMinimum(0);
  cg->Update();
  string png = string(prefix) + "_gain.png";
  cg->SaveAs(png.c_str());

  // each channel: one pad per run, ordered by HV, the spectrum with its fit
  vector<const RunData *> byhv;
  for (auto &r : runs) byhv.push_back(&r);
  stable_sort(byhv.begin(), byhv.end(), [](const RunData *a, const RunData *b) { return a->hv < b->hv ; });
  for (size_t i = 0; i < chs.size(); i++) {
    int ch = chs[i];
    int npad = 0;
    for (auto *r : byhv) npad += r->q[ch] && r->q[ch]->GetEntries() > 0;
    if (!npad) continue;
    int ncol = (int)ceil(sqrt((double)npad)), nrow = (npad + ncol - 1) / ncol;
    auto *cs = new TCanvas(Form("c_scan_spectra_%s", chtag(ch).c_str()), Form("%s spectra", chname(ch).c_str()),
                           380 * ncol, 300 * nrow);
    cs->Divide(ncol, nrow, 0.002, 0.002);
    int pad = 0;
    for (auto *r : byhv) {
      TH1F *h = r->q[ch];
      if (!h || h->GetEntries() == 0) continue;
      cs->cd(++pad);
      gPad->SetLogy();
      gPad->SetTopMargin(0.12);
      // cut the empty tail: up to where 99.95 % of the entries are
      double hi = q_hi, tot = h->Integral(), sum = 0;
      for (int b = 1; b <= h->GetNbinsX(); b++)
        if ((sum += h->GetBinContent(b)) >= 0.9995 * tot) { hi = h->GetXaxis()->GetBinUpEdge(b) ; break ; }
      h->GetXaxis()->SetRangeUser(q_lo, std::max(hi, 0.5));
      h->SetLineColor(kGray + 2);
      h->Draw("hist");
      const tb::SpeFit &f = r->fit[ch];
      string lab = Form("run %d, %.0f V", r->run, r->hv);
      if (f.f) {
        f.f->SetLineColor(TColor::GetColor(series_hex[i]));
        f.f->SetLineWidth(2);
        f.f->Draw("same");
        lab += usable(f) ? Form(": gain %.2e, #mu %.2f", f.gain, f.mu) : ": fit not used";
      }
      h->SetTitle(lab.c_str());
      gStyle->SetOptTitle(1);
    }
    string sp = string(prefix) + "_spectra_" + chtag(ch) + ".png";
    cs->SaveAs(sp.c_str());
    gStyle->SetOptTitle(0);
  }
}
