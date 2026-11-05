// Live gain curve of an HV scan (bin/scan_hv.sh) while follow_hv_scan.sh fits its runs.
// Left: gain vs HV (log), the spot (black, with an exponential fit once there are 3 points: the
// voltage that doubles the gain, and the gain at 2100 V) and the three channels with the most light
// at the latest point; the points taken and not fitted yet are counted in the title. Right: the latest
// run's spot charge spectrum with its single-photoelectron fit. Redraws every `every` s and writes
// <scan>_live.png in the scan's folder each time. Same use as live_gain_map.C:
//   root -l 'analysis/gain_hv/live_hv_curve.C("<scan>")'                window, refreshed by a timer
//   root -b -q 'analysis/gain_hv/live_hv_curve.C("<scan>", 20)'         png only, until the scan has
//                                                                      ended and every run is fitted
//   root -b -q 'analysis/gain_hv/live_hv_curve.C("<scan>", 0)'          png once
// The live fits use one fixed window (follow_hv_scan.sh --window); fit_hv_scan.sh at the end finds its
// own from all runs, so its curve can differ by a few %. Reads only.
#include <set>
#include "TGraphErrors.h"
#include "TMultiGraph.h"
#include "TLegend.h"
#include "TLatex.h"
#include "TF1.h"
#include "TStyle.h"
#include "TTimer.h"
#include "TROOT.h"
#include "live_common.h"

namespace live {

std::string hdir, hname;
TCanvas *hc = nullptr;
TMultiGraph *mg = nullptr;
TH1 *hspec_hv = nullptr;
TF1 *ffit_hv = nullptr, *fexp = nullptr;
std::vector<TObject *> htrash;
bool hfinished = false;

void update_hv()
{
  for (auto *o : htrash) delete o;
  htrash.clear();
  auto pts = read_csv(hdir + "/" + hname + ".csv");
  std::map<long, Row> run2pt;
  for (auto &p : pts) if (p["status"] == "ok") run2pt[std::stol(p["run"])] = p;
  std::string parts = hdir + "/" + hname + "_live_parts";
  // each run's fit table: run<N>.csv, written when its fit is done
  std::map<long, std::vector<Row>> fits;
  long newest = -1;
  long newest_mtime = -1;
  for (auto &rp : run2pt) {
    std::string f = parts + "/run" + std::to_string(rp.first) + ".csv";
    FileStat_t st;
    if (gSystem->GetPathInfo(f.c_str(), st) != 0) continue;
    fits[rp.first] = read_csv(f);
    if (st.fMtime >= newest_mtime) { newest_mtime = st.fMtime ; newest = rp.first ; }
  }
  bool ended = scan_ended(hdir, hname, !run2pt.empty());
  hfinished = ended && fits.size() == run2pt.size();

  // the spot and the three brightest channels at the latest run
  std::vector<std::string> chs = {"spot"};
  if (newest >= 0) {
    std::vector<std::pair<double, std::string>> bright;
    for (auto &r : fits[newest])
      if (r["ch"] != "spot" && r["ch"] != "all" && usable(r)) bright.push_back({num(r, "mu"), r["ch"]});
    std::sort(bright.rbegin(), bright.rend());
    for (size_t i = 0; i < bright.size() && i < 3; i++) chs.push_back(bright[i].second);
  }
  const int col[] = {kBlack, kAzure + 1, kOrange + 7, kGreen + 2};

  hc->cd(1);
  gPad->Clear();
  gPad->SetLogy();
  gPad->SetGrid();
  delete mg ; mg = new TMultiGraph();
  auto *leg = new TLegend(0.14, 0.70, 0.45, 0.88);
  leg->SetBorderSize(0) ; leg->SetFillStyle(0);
  htrash.push_back(leg);
  TGraphErrors *gspot = nullptr;
  for (size_t k = 0; k < chs.size(); k++) {
    auto *g = new TGraphErrors();
    for (auto &fr : fits)
      for (auto &r : fr.second)
        if (r["ch"] == chs[k] && usable(r)) {
          int n = g->GetN();
          g->SetPoint(n, num(r, "hv_V"), num(r, "gain"));
          g->SetPointError(n, 0, num(r, "gain_err"));
        }
    if (g->GetN() == 0) { delete g ; continue ; }
    g->SetMarkerStyle(k ? 24 : 20) ; g->SetMarkerSize(k ? 1.0 : 1.3);
    g->SetMarkerColor(col[k]) ; g->SetLineColor(col[k]);
    mg->Add(g, "P");
    leg->AddEntry(g, k ? Form("ch%s", chs[k].c_str()) : "spot (95 % of the charge)", "P");
    if (!k) gspot = g;
  }
  mg->SetTitle(Form("%s: %zu of %zu runs fitted, scan %s, %s;HV (VMON) [V];gain", hname.c_str(), fits.size(),
                    run2pt.size(), ended ? "ended" : "running", now_hms().c_str()));
  if (mg->GetListOfGraphs()) {
    mg->Draw("A");
    leg->Draw();
    if (gspot && gspot->GetN() >= 3) {
      delete fexp;
      fexp = new TF1("live_expo", "expo", gspot->GetX()[0] - 10, gspot->GetX()[gspot->GetN() - 1] + 10);
      fexp->SetLineColor(kGray + 1) ; fexp->SetLineStyle(2);
      if (gspot->Fit(fexp, "Q0R") == 0 && fexp->GetParameter(1) > 0) {
        fexp->Draw("same");
        auto *t = new TLatex(0.40, 0.15, Form("spot, exponential fit: #times2 every %.0f V, %.3g at 2100 V", std::log(2) / fexp->GetParameter(1),
                                             fexp->Eval(2100)));
        t->SetNDC() ; t->SetTextSize(0.03);
        t->Draw();
        htrash.push_back(t);
      }
    }
  }

  hc->cd(2);
  gPad->Clear();
  if (newest >= 0) {
    Row spot;
    for (auto &r : fits[newest]) if (r["ch"] == "spot") spot = r;
    auto &p = run2pt[newest];
    if (!spot.empty())
      draw_spectrum(parts + "/run" + std::to_string(newest) + ".root", newest, spot,
                    Form("latest: point %s, %s V (VMON), run %ld", p["point"].c_str(), p["vmon_V"].c_str(), newest),
                    hspec_hv, ffit_hv, htrash);
  }
  hc->Update();
  save_png(hc, hdir, hname);
}

}  // namespace live

void live_hv_update() { live::update_hv() ; }

void live_hv_curve(const char *scan, double every = 10)
{
  std::string d = live::scan_dir(scan, __FILE__);
  if (d.empty()) { printf("no scan folder %s\n", scan) ; return ; }
  live::hdir = d;
  live::hname = gSystem->BaseName(d.c_str());
  gStyle->SetTitleFontSize(0.04);
  gErrorIgnoreLevel = kWarning;
  live::hc = new TCanvas("live_hv_curve", (live::hname + " (live)").c_str(), 1500, 640);
  live::hc->Divide(2, 1);
  live::update_hv();
  if (every <= 0) return;
  if (gROOT->IsBatch()) {
    while (!live::hfinished) { gSystem->Sleep(int(every * 1000)) ; live::update_hv() ; }
    return;
  }
  auto *t = new TTimer(int(every * 1000));
  t->SetCommand("live_hv_update()");
  t->TurnOn();
}
