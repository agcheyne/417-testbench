// time_offsets - CFD time offset of every V1742 input, from a scan that put the laser on every
// pixel, written to the db for the online monitor and the analyses (lib/tb_v1742.h TimeOffsets).
//
//   ./time_offsets.sh
//   root [1] add_scan("../../logs/scans/20261027_115115_photonis_readout0_map.csv")   // ~15 min
//   root [2] fit_offsets() ; draw_offsets() ; save_plots("../../logs/scans/time_offsets_0049_5G")
//   root [3] write_db("Photonis readout-0 scan; inputs 0-15 cable one pin off")
//   root [4] save_times("readout0_times.root")    // later: load_times(...) ; fit_offsets() in seconds
//
// Every pulse of at least 4 mV in the signal window gives a CFD time relative to its group
// trigger. Per input, the laser peak is the core Gaussian of those times (tb::core_sigma). The
// laser timing may drift over a long scan, and each input is lit in only a few of its runs, so
// the drift is measured per run (mean of every measured input's pulses within 0.3 ns of its
// peak) and taken out before the final fit. A peak counts with >= 50 pulses within +-2 sigma and
// at least 3 times the input's flat background there (pickup and dark counts). The offsets are given relative to the mean of the
// measured inputs of groups 2 and 3, so those keep their times and groups 0/1 move by ~2 ns.
// Inputs without a clear peak (dead, no light, pickup) stay unmeasured: users take the mean of
// their DRS4 board (groups 0/1 and 2/3 differ by the cabling of their TR inputs, ~2.3 ns). Measure again after any change of cables, trigger fan-out or sampling speed.

#include <iostream>
#include <pmonitor/pmonitor.h>
#include "time_offsets.h"

#include "tb_canvas.h"
#include "tb_db.h"
#include "tb_fit.h"
#include "tb_pulse.h"
#include "tb_v1742.h"

#include <TCanvas.h>
#include <TFile.h>
#include <TNamed.h>
#include <TTree.h>
#include <TGraphErrors.h>
#include <TH1.h>
#include <TH2.h>
#include <TLatex.h>
#include <TLegend.h>
#include <TLine.h>
#include <TStyle.h>
#include <TSystem.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

using namespace std;
using tb::NSIG;
using tb::NTRIG;

int init_done = 0;
int pinit()
{
  if (init_done) return 1;
  init_done = 1;
  return 0;
}

static tb::Cuts cuts = [] { tb::Cuts c ; c.sig_from = 17 ; c.sig_to = 35 ; c.threshold = 4 ; c.le_threshold = 4 ; return c ; }();
void set_signal_window(double a, double b) { cuts.sig_from = a ; cuts.sig_to = b ; }
void set_min_amplitude(double mv) { cuts.threshold = mv ; cuts.le_threshold = mv ; }

static tb::V1742Event ev;
static tb::RunInfo info;
static int serial = -1, freq = -1;
static string runtype;
static map<int, vector<float>[NSIG]> times;   // run -> per input: CFD times of its pulses
static long n_events = 0;

int process_event(Event *e)
{
  if (e->getEvtType() == 9) {
    info.parse(e);
    ev.serial = info.serial;
    if (info.serial >= 0) serial = info.serial;
    return 0;
  }
  if (!ev.fill(e)) return 0;
  freq = ev.freq;
  n_events++;
  auto &t = times[e->getRunNumber()];
  for (int ch = 0; ch < NSIG; ch++) {
    tb::ChannelHit h = tb::measure(ev, ch, cuts);
    if (h.hit && !std::isnan(h.t_cfd)) t[ch].push_back(h.t_cfd);
  }
  return 0;
}

void add_run(const char *filename)
{
  string f = filename;
  size_t k = f.rfind("-0000.evt");
  for (int part = 0;; part++) {
    string p = k == string::npos ? f : f.substr(0, k) + Form("-%04d.evt", part);
    if (part > 0 && (k == string::npos || gSystem->AccessPathName(p.c_str()))) break;
    if (pfileopen(p.c_str()) != 0) { printf("time_offsets: cannot open %s\n", p.c_str()) ; break ; }
    prun();
    pclose();
  }
}

void add_scan(const char *csv)
{
  ifstream in(csv);
  if (!in) { printf("time_offsets: cannot read %s\n", csv) ; return ; }
  string txt = string(csv).substr(0, string(csv).rfind(".csv")) + ".txt", line;
  ifstream t(txt);
  smatch m;
  static const regex re("run type ([A-Za-z0-9_-]+)");
  while (getline(t, line)) if (regex_search(line, m, re)) runtype = m[1];
  if (runtype.empty()) { printf("time_offsets: no run type in %s\n", txt.c_str()) ; return ; }
  string data_root = tb::read_conf(tb::repo_root() + "/db/testbench.conf")["DATA_ROOT"];
  getline(in, line);   // header: point,target_x_mm,target_y_mm,x_mm,y_mm,run,events,start,end,status
  int n = 0;
  while (getline(in, line)) {
    vector<string> f;
    stringstream ss(line);
    for (string c; getline(ss, c, ',');) f.push_back(c);
    if (f.size() < 10 || f[9] != "ok") continue;
    add_run((data_root + "/" + runtype + "/" + runtype + Form("-%08d-0000.evt", atoi(f[5].c_str()))).c_str());
    n++;
  }
  printf("time_offsets: %d runs, %ld events so far\n", n, n_events);
}

// the pulse times, so fit_offsets() can be redone without the data
void save_times(const char *filename)
{
  TFile f(filename, "RECREATE");
  int run, ch;
  float t;
  TTree tr("times", "CFD time - group trigger [ns] of every pulse used");
  tr.Branch("run", &run) ; tr.Branch("ch", &ch) ; tr.Branch("t", &t);
  for (auto &r : times) { run = r.first ; for (ch = 0; ch < NSIG; ch++) for (float x : r.second[ch]) { t = x ; tr.Fill() ; } }
  tr.Write();
  TNamed("info", Form("%d %d %ld %.3f %.3f %.3f %s", serial, freq, n_events, cuts.threshold, cuts.sig_from, cuts.sig_to, runtype.c_str())).Write();
  f.Close();
  printf("time_offsets: wrote the pulse times of %zu runs to %s\n", times.size(), filename);
}

void load_times(const char *filename)
{
  TFile f(filename);
  TTree *tr = f.IsOpen() ? (TTree *)f.Get("times") : nullptr;
  if (!tr) { printf("time_offsets: no times tree in %s\n", filename) ; return ; }
  times.clear();
  int run, ch;
  float t;
  tr->SetBranchAddress("run", &run) ; tr->SetBranchAddress("ch", &ch) ; tr->SetBranchAddress("t", &t);
  for (Long64_t i = 0; i < tr->GetEntries(); i++) { tr->GetEntry(i) ; if (ch >= 0 && ch < NSIG) times[run][ch].push_back(t) ; }
  if (auto *n = (TNamed *)f.Get("info")) {
    char rt[64] = "";
    double thr, a, b;
    sscanf(n->GetTitle(), "%d %d %ld %lf %lf %lf %63s", &serial, &freq, &n_events, &thr, &a, &b, rt);
    cuts.threshold = cuts.le_threshold = thr ; cuts.sig_from = a ; cuts.sig_to = b ; runtype = rt;
  }
  printf("time_offsets: %zu runs of pulse times from %s\n", times.size(), filename);
}

// --- results ------------------------------------------------------------------------------------
static double peak[NSIG], epeak[NSIG], sigma[NSIG], offset[NSIG], ref_peak = NAN;
static long in_peak[NSIG];
static bool measured[NSIG];
static map<int, double> drift;   // run -> laser time shift [ns]
static TH2F *h_all = nullptr;    // input vs drift-corrected time
static TGraphErrors *g_off = nullptr, *g_sig = nullptr, *g_drift = nullptr;
static TGraph *g_unmeas = nullptr;
static double drift_rms = NAN;

// core peak of times shifted by -shift(run); returns sigma, NaN without a clear peak
static long bg_in_peak[NSIG];
static double fit_one(int ch, const map<int, double> &shift, double &m, double &em, long &n)
{
  TH1F h("tb_to", "", int((cuts.sig_to - cuts.sig_from) / 0.01), cuts.sig_from, cuts.sig_to);
  h.SetDirectory(nullptr);
  for (auto &r : times) {
    auto it = shift.find(r.first);
    double d = it == shift.end() ? 0 : it->second;
    for (float x : r.second[ch]) h.Fill(x - d);
  }
  double s = tb::core_sigma(&h, m);
  n = std::isnan(s) ? 0 : long(h.Integral(h.FindBin(m - 2 * s), h.FindBin(m + 2 * s)));
  // flat background: the pulses more than 1 ns from the peak, per ns, times the 4 sigma window
  double out = std::isnan(s) ? 0 : h.Integral() - h.Integral(h.FindBin(m - 1), h.FindBin(m + 1));
  bg_in_peak[ch] = std::isnan(s) ? 0 : long(out / (cuts.sig_to - cuts.sig_from - 2) * 4 * s + 0.5);
  em = n > 0 ? s / sqrt(double(n)) : NAN;
  return s;
}

static int min_pulses = 50;
static string why_not(int ch)
{
  if (std::isnan(sigma[ch])) return "no peak";
  if (sigma[ch] >= 0.4) return Form("no clear peak: sigma %.0f ps", 1000 * sigma[ch]);
  if (in_peak[ch] < min_pulses) return Form("%ld pulses in the peak", in_peak[ch]);
  return Form("peak %ld pulses on a background of %ld: pickup?", in_peak[ch], bg_in_peak[ch]);
}

void fit_offsets(int min_hits)
{
  min_pulses = min_hits;
  if (times.empty()) { printf("time_offsets: no data (add_scan or add_run first)\n") ; return ; }
  map<int, double> none;
  // 1. peaks from all runs as they are
  for (int ch = 0; ch < NSIG; ch++) {
    long n;
    sigma[ch] = fit_one(ch, none, peak[ch], epeak[ch], n);
    measured[ch] = !std::isnan(sigma[ch]) && sigma[ch] < 0.4 && n >= min_hits && n >= 3 * bg_in_peak[ch];
  }
  // 2. laser drift per run: every measured input's pulses near its peak
  drift.clear();
  double s1 = 0, s2 = 0;
  int nr = 0;
  for (auto &r : times) {
    double sum = 0;
    long n = 0;
    for (int ch = 0; ch < NSIG; ch++) {
      if (!measured[ch]) continue;
      for (float x : r.second[ch]) if (fabs(x - peak[ch]) < 0.3) { sum += x - peak[ch] ; n++ ; }
    }
    if (n < 10) continue;
    drift[r.first] = sum / n;
    s1 += sum / n ; s2 += (sum / n) * (sum / n) ; nr++;
  }
  drift_rms = nr ? sqrt(max(0.0, s2 / nr - (s1 / nr) * (s1 / nr))) : NAN;
  // 3. again, drift taken out
  for (int ch = 0; ch < NSIG; ch++) {
    sigma[ch] = fit_one(ch, drift, peak[ch], epeak[ch], in_peak[ch]);
    measured[ch] = !std::isnan(sigma[ch]) && sigma[ch] < 0.4 && in_peak[ch] >= min_hits && in_peak[ch] >= 3 * bg_in_peak[ch];
  }
  double sum = 0;
  int n = 0;
  for (int ch = 2 * 8; ch < NSIG; ch++) if (measured[ch]) { sum += peak[ch] ; n++ ; }
  if (!n) for (int ch = 0; ch < NSIG; ch++) if (measured[ch]) { sum += peak[ch] ; n++ ; }
  if (!n) { printf("time_offsets: no input has a clear laser peak\n") ; return ; }
  ref_peak = sum / n;
  for (int ch = 0; ch < NSIG; ch++) offset[ch] = measured[ch] ? peak[ch] - ref_peak : NAN;

  printf("time_offsets: %zu runs, %ld events, V1742 serial %d, pulses >= %.1f mV in %.0f..%.0f ns\n", times.size(), n_events, serial,
         cuts.threshold, cuts.sig_from, cuts.sig_to);
  printf("  laser drift over the runs: rms %.0f ps (%d runs with enough pulses), taken out\n", 1000 * drift_rms, nr);
  printf("  reference: mean peak of the measured inputs of groups 2 and 3 = %.4f ns after the group trigger\n", ref_peak);
  printf("  input  group   offset [ns]        core sigma [ps]   pulses in the peak (flat background)\n");
  for (int ch = 0; ch < NSIG; ch++) {
    if (measured[ch]) printf("  %4d    %d    %+8.4f +- %.4f      %5.0f             %7ld (%ld)\n", ch, ch / 8, offset[ch], epeak[ch], 1000 * sigma[ch], in_peak[ch], bg_in_peak[ch]);
    else printf("  %4d    %d    not measured (%s)\n", ch, ch / 8, why_not(ch).c_str());
  }
  for (int g = 0; g < NTRIG; g++) {
    double a = 0, b = 0;
    int k = 0;
    for (int ch = 8 * g; ch < 8 * g + 8; ch++) if (measured[ch]) { a += offset[ch] ; b += offset[ch] * offset[ch] ; k++ ; }
    if (k) printf("  group %d: mean %+.3f ns, spread (rms) %.0f ps over %d inputs\n", g, a / k, 1000 * sqrt(max(0.0, b / k - a * a / k / k)), k);
    else printf("  group %d: no input measured\n", g);
  }

  // plots
  delete h_all ; delete g_off ; delete g_sig ; delete g_drift ; delete g_unmeas;
  h_all = new TH2F("to_all", "CFD time - group trigger, laser drift taken out;input;t [ns];pulses", NSIG, -0.5, NSIG - 0.5,
                   int((cuts.sig_to - cuts.sig_from) / 0.02), cuts.sig_from, cuts.sig_to);
  h_all->SetDirectory(nullptr);
  for (auto &r : times) {
    double d = drift.count(r.first) ? drift[r.first] : 0;
    for (int ch = 0; ch < NSIG; ch++) for (float x : r.second[ch]) h_all->Fill(ch, x - d);
  }
  g_off = new TGraphErrors ; g_sig = new TGraphErrors ; g_unmeas = new TGraph ; g_drift = new TGraphErrors;
  double gm[NTRIG / 2] = {};               // per DRS4 board
  int gn[NTRIG / 2] = {};
  for (int ch = 0; ch < NSIG; ch++) if (measured[ch]) { gm[ch / 16] += offset[ch] ; gn[ch / 16]++ ; }
  for (int ch = 0; ch < NSIG; ch++) {
    if (measured[ch]) {
      int i = g_off->GetN();
      g_off->SetPoint(i, ch, offset[ch]) ; g_off->SetPointError(i, 0, epeak[ch]);
      g_sig->SetPoint(i, ch, 1000 * sigma[ch]);
    } else g_unmeas->SetPoint(g_unmeas->GetN(), ch, gn[ch / 16] ? gm[ch / 16] / gn[ch / 16] : 0);
  }
  for (auto &d : drift) g_drift->SetPoint(g_drift->GetN(), d.first, 1000 * d.second);
}

static TCanvas *c_off = nullptr;
void draw_offsets()
{
  if (!g_off) fit_offsets();
  if (!g_off) return;
  gStyle->SetOptStat(0);
  if (!tb::canvas_open(c_off)) { c_off = new TCanvas("c_time_offsets", "V1742 time offsets", 1500, 900) ; c_off->Divide(2, 2) ; }
  c_off->cd(1) ; gPad->SetGridy();
  auto *fr = gPad->DrawFrame(-0.5, -3, NSIG - 0.5, 1, "offset per input (gray: not measured, its board's mean is used);input;offset [ns]");
  (void)fr;
  g_off->SetMarkerStyle(20) ; g_off->SetMarkerColor(kBlue + 1) ; g_off->SetLineColor(kBlue + 1) ; g_off->Draw("P");
  g_unmeas->SetMarkerStyle(24) ; g_unmeas->SetMarkerColor(kGray + 1) ; if (g_unmeas->GetN()) g_unmeas->Draw("P");
  c_off->cd(2) ; gPad->SetGridy();
  gPad->DrawFrame(-0.5, 0, NSIG - 0.5, 250, "core width of the laser peak per input;input;#sigma [ps]");
  g_sig->SetMarkerStyle(20) ; g_sig->SetMarkerColor(kRed + 1) ; g_sig->Draw("P");
  c_off->cd(3) ; gPad->SetRightMargin(0.15) ; gPad->SetLogz();
  if (!std::isnan(ref_peak)) h_all->GetYaxis()->SetRangeUser(ref_peak - 4, ref_peak + 3);
  h_all->Draw("colz");
  c_off->cd(4) ; gPad->SetGridy();
  g_drift->SetTitle(Form("laser time shift per run (rms %.0f ps), taken out;run;shift [ps]", 1000 * drift_rms));
  g_drift->SetMarkerStyle(20) ; g_drift->SetMarkerSize(0.6) ; g_drift->Draw("AP");
  c_off->Update();
}

void write_db(const char *note)
{
  if (std::isnan(ref_peak)) { printf("time_offsets: fit_offsets() first\n") ; return ; }
  static const char *speed[] = {"5G", "2G", "1G"}, *gsps[] = {"5", "2.5", "1"};
  if (freq < 0 || freq > 2) { printf("time_offsets: sampling speed unknown\n") ; return ; }
  if (serial < 0) { printf("time_offsets: V1742 serial unknown (no begin-run event): not written\n") ; return ; }
  string dir = tb::repo_root() + "/db/v1742";
  gSystem->mkdir(dir.c_str(), kTRUE);
  string file = dir + Form("/time_offsets_%04d_%s.conf", serial, speed[freq]);
  bool existed = !gSystem->AccessPathName(file.c_str());
  ofstream o(file);
  time_t now = time(nullptr);
  char date[32];
  strftime(date, sizeof date, "%d %b %Y", localtime(&now));
  o << "# V1742 serial " << serial << " at " << gsps[freq] << " GS/s: CFD time offset of each input [ns], read by lib/tb_v1742.h (TimeOffsets).\n"
    << "# offset = the input's laser peak (CFD time - group trigger) - the mean of the measured inputs of groups 2 and 3;\n"
    << "# subtract it from a CFD time. Inputs marked - were not measured: the mean of their DRS4 board (groups 0/1, 2/3) is used.\n"
    << "# Measured " << date << " by analysis/time_offsets: runs " << times.begin()->first << "-" << times.rbegin()->first << " (" << times.size()
    << " runs, " << n_events << " events" << (runtype.empty() ? "" : ", run type " + runtype) << "), pulses >= " << cuts.threshold << " mV in "
    << cuts.sig_from << ".." << cuts.sig_to << " ns,\n"
    << Form("# laser drift over the runs rms %.0f ps (taken out); reference peak %.4f ns after the group trigger.\n", 1000 * drift_rms, ref_peak);
  if (note && *note) o << "# " << note << "\n";
  o << "# Measure again after any change of cables, trigger fan-out or sampling speed.\n\n";
  for (int ch = 0; ch < NSIG; ch++) {
    if (ch % 8 == 0) o << "# --- group " << ch / 8 << " ---\n";
    if (measured[ch]) o << Form("OFFSET_%02d=%+.4f   # +- %.4f ns, core sigma %.0f ps, %ld pulses\n", ch, offset[ch], epeak[ch], 1000 * sigma[ch], in_peak[ch]);
    else o << Form("OFFSET_%02d=-         # not measured (%s)\n", ch, why_not(ch).c_str());
  }
  o.close();
  printf("time_offsets: %s %s\n", existed ? "replaced" : "wrote", file.c_str());
}

void save_plots(const char *prefix)
{
  if (!tb::canvas_open(c_off)) draw_offsets();
  if (tb::canvas_open(c_off)) c_off->SaveAs(Form("%s_offsets.png", prefix));
}
