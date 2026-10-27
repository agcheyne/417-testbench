// gain_hv - gain vs HV from single-photoelectron fits, one run per HV point.
//
//   ./gain_hv.sh
//   root [1] set_charge_window(21, 30)                // from general's suggest_windows()
//   root [2] set_hv_electrode("")                     // or set_hv_channel(236, 0)
//   root [3] add_run("/scratch2/data/hpDIRC/photek/photek-00000101-0000.evt")   // one per HV
//   root [4] add_run(...) ...                         // or add_runs("series.txt")
//   root [5] fit_all() ; gain_curve(20) ; working_point(20, 1e6) ; save()
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
#include <TF1.h>
#include <TFile.h>
#include <TGraphErrors.h>
#include <TH1.h>
#include <TMath.h>
#include <TSystem.h>

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
  TH1F *q[NSIG] = {};
  tb::SpeFit fit[NSIG];
};
static vector<RunData> runs;
static RunData *cur = nullptr;
static tb::V1742Event ev;
static tb::RunInfo info;
static tb::Db db;

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
  if (!cur) return 0;
  if (e->getEvtType() == 9) {
    info.parse(e);
    ev.serial = info.serial;
    cur->run = info.run;
    if (!cur->hv_given) cur->hv = run_hv();
    return 0;
  }
  if (!ev.fill(e)) return 0;
  if (!cur->q[0])
    for (int ch = 0; ch < NSIG; ch++)
      cur->q[ch] = new TH1F(Form("q_run%d_ch%02d", cur->run, ch),
                            Form("run %d ch %d charge (%.1f to %.1f ns after the trigger);charge [pC];events",
                                 cur->run, ch, cuts.q_from, cuts.q_to), q_bins, q_lo, q_hi);
  cur->events++;
  for (int ch = 0; ch < NSIG; ch++) {
    tb::ChannelHit h = tb::measure(ev, ch, cuts);
    if (h.valid) cur->q[ch]->Fill(h.q);
  }
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
  cur = nullptr;
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
  printf("  run      HV [V]  ch   gain             mu [p.e.]        chi2/ndf  note\n");
  for (auto &r : runs)
    for (int ch = 0; ch < NSIG; ch++) {
      TH1F *h = r.q[ch];
      if (!h || h->GetEntries() < min_entries) continue;
      delete r.fit[ch].f;
      r.fit[ch] = tb::fit_spe(h);
      const tb::SpeFit &f = r.fit[ch];
      if (!f.f || f.mu <= 0.02 || !f.resolved) { hidden++ ; continue ; }   // no light, or no peak above the pedestal noise
      const char *note = !f.ok ? "fit did not converge" : f.mu > 3 ? "mu > 3: too much light, not used"
                       : !r.hv_on ? "HV off" : "";
      printf("  %-6d %8.1f  %2d   %.3e +- %.1e  %6.3f +- %.3f  %6.2f    %s\n",
             r.run, r.hv, ch, f.gain, f.egain, f.mu, f.emu, f.chi2ndf, note);
    }
  if (hidden) printf("  (%d run x channel spectra without a photoelectron peak 3 pedestal sigmas clear of the pedestal not shown)\n", hidden);
}

static map<int, TF1 *> curve_fit;   // per channel: gain = exp(p0 + p1 * V)

static TGraphErrors *curve(int ch)
{
  auto *g = new TGraphErrors();
  g->SetName(Form("gain_ch%02d", ch));
  g->SetTitle(Form("ch %d gain;HV [V];gain", ch));
  for (auto &r : runs)
    if (!std::isnan(r.hv) && r.hv_on && usable(r.fit[ch])) {
      int n = g->GetN();
      g->SetPoint(n, r.hv, r.fit[ch].gain);
      g->SetPointError(n, 0, r.fit[ch].egain);
    }
  return g;
}

void gain_curve(int ch)
{
  if (ch < 0 || ch >= NSIG) return;
  delete curve_fit[ch];
  curve_fit[ch] = nullptr;   // stays null unless a curve is fitted below
  TGraphErrors *g = curve(ch);
  if (g->GetN() == 0) { printf("gain_hv: ch %d has no usable fit (fit_all() first; mu must be 0.02-3)\n", ch) ; return ; }
  auto *c = new TCanvas(Form("c_gain_ch%02d", ch), Form("ch %d gain vs HV", ch), 800, 600);
  c->SetLogy();
  g->SetMarkerStyle(20);
  g->Draw("AP");
  double span = TMath::MaxElement(g->GetN(), g->GetX()) - TMath::MinElement(g->GetN(), g->GetX());
  if (g->GetN() < 2 || span < 10) {
    printf("gain_hv: ch %d: %d point(s) within %.1f V (gain %.3g at %.0f V) - need HV points at least 10 V apart for a curve\n",
           ch, g->GetN(), span, g->GetY()[0], g->GetX()[0]);
    return;
  }
  TF1 *f = new TF1(Form("gain_fit_ch%02d", ch), "expo", g->GetX()[0] - 1, g->GetX()[g->GetN() - 1] + 1);
  g->Fit(f, "Q");
  curve_fit[ch] = f;
  double b = f->GetParameter(1);
  printf("ch %d: gain = exp(%.3f + %.5f V): x2 every %.1f V (%d points, chi2/ndf %.2f)\n", ch, f->GetParameter(0), b,
         log(2.0) / b, g->GetN(), f->GetNDF() > 0 ? f->GetChisquare() / f->GetNDF() : 0.0);
}

double working_point(int ch, double target)
{
  if (!curve_fit.count(ch)) gain_curve(ch);
  if (!curve_fit[ch]) { printf("gain_hv: no gain curve for ch %d\n", ch) ; return NAN ; }
  TF1 *f = curve_fit[ch];
  double v = (log(target) - f->GetParameter(0)) / f->GetParameter(1);
  printf("ch %d: gain %.3g at %.1f V%s\n", ch, target, v,
         (v < f->GetXmin() || v > f->GetXmax()) ? " (extrapolated outside the measured range)" : "");
  return v;
}

void draw_spectrum(int run, int ch)
{
  for (auto &r : runs)
    if (r.run == run && ch >= 0 && ch < NSIG && r.q[ch]) {
      auto *c = new TCanvas(Form("c_q_run%d_ch%02d", run, ch), Form("run %d ch %d", run, ch), 800, 600);
      c->SetLogy();
      r.q[ch]->Draw();
      if (r.fit[ch].f) { r.fit[ch].f->SetLineColor(kRed) ; r.fit[ch].f->Draw("same") ; }
      return;
    }
  printf("gain_hv: no run %d / channel %d\n", run, ch);
}

void save(const char *prefix)
{
  string csv = string(prefix) + ".csv", root = string(prefix) + ".root";
  ofstream out(csv);
  out << "run,hv_V,hv_on,ch,gain,gain_err,mu,mu_err,q1_pC,q1_err,q0_pC,s0_pC,s1_pC,chi2ndf,converged,used\n";
  for (auto &r : runs)
    for (int ch = 0; ch < NSIG; ch++) {
      const tb::SpeFit &f = r.fit[ch];
      if (!f.f || f.mu <= 0.02 || !f.resolved) continue;
      out << r.run << "," << r.hv << "," << r.hv_on << "," << ch << "," << f.gain << "," << f.egain << "," << f.mu << "," << f.emu << ","
          << f.q1 << "," << f.eq1 << "," << f.q0 << "," << f.s0 << "," << f.s1 << "," << f.chi2ndf << ","
          << f.ok << "," << (usable(f) && r.hv_on) << "\n";
    }
  TFile tf(root.c_str(), "RECREATE");
  for (auto &r : runs)
    for (int ch = 0; ch < NSIG; ch++)
      if (r.q[ch]) r.q[ch]->Write();
  for (int ch = 0; ch < NSIG; ch++) {
    TGraphErrors *g = curve(ch);
    if (g->GetN()) g->Write();
    delete g;
  }
  tf.Close();
  printf("gain_hv: wrote %s and %s\n", csv.c_str(), root.c_str());
}
