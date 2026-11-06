// Gain map of an xy scan from fit_gain_map.sh's merged table (<scan>_gain_map.csv: point, x_mm, y_mm,
// then gain_hv's fit columns with ch = a channel, "spot" or "all", peak = a photoelectron peak
// clearly resolved, the window systematic gain_sys_lo/hi, spot_channels, contained, gain_meanq,
// then each systematic window's Q1). Per point: mu and gain of the spot (the pads holding 95 % of
// the mean charge: the gain map), and the gain of the sum of all good channels relative to it, as
// maps; per pad (the channel brightest at the point) the median gain of the spot with the window
// systematic, of the sum of all, from the mean charge (no fit), and of the pad alone. Without sums
// (--contain 0) the brightest channel stands in for the spot. Writes <prefix>.png (maps with stage y down,
// as the bench face) and prints the per-pad table.
//   root -b -q 'plot_gain_map.C("<scan>_gain_map.csv", "<scan>_gain_map")'
#include <fstream>
#include <sstream>
#include <map>
#include <set>
#include <vector>
#include <algorithm>
#include "TCanvas.h"
#include "TH2F.h"
#include "TGraphErrors.h"
#include "TGraphAsymmErrors.h"
#include "TLegend.h"
#include "TLatex.h"
#include "TStyle.h"
#include "TColor.h"
#include "../../lib/tb_ydown.h"

void plot_gain_map(const char *csv, const char *prefix, double zmax_gain = 0)
{
  struct Fit { int ch = -1; double x = 0, y = 0, gain = 0, mu = 0, slo = NAN, shi = NAN, gmean = NAN; };
  std::map<int, Fit> all, spot, pad;       // per point: the sums, and the brightest single channel
  std::set<double> xs, ys;
  std::ifstream in(csv);
  if (!in) { printf("plot_gain_map: cannot read %s\n", csv) ; return ; }
  std::string line;
  std::getline(in, line);
  auto split = [](const std::string &l) { std::vector<std::string> c ; std::stringstream ss(l) ;
                                          for (std::string f; std::getline(ss, f, ',');) c.push_back(f) ; if (!l.empty() && l.back() == ',') c.push_back("") ; return c ; };
  while (std::getline(in, line)) {
    std::vector<std::string> c = split(line);
    if (c.size() < 18) continue;
    double x = atof(c[1].c_str()), y = atof(c[2].c_str());
    xs.insert(x) ; ys.insert(y);
    if (c[17] != "1") continue;
    Fit f;
    f.x = x ; f.y = y ; f.gain = atof(c[5].c_str()) ; f.mu = atof(c[7].c_str());
    if (c.size() >= 20 && !c[18].empty() && !c[19].empty()) { f.slo = atof(c[18].c_str()) ; f.shi = atof(c[19].c_str()) ; }
    if (c.size() >= 23 && !c[22].empty()) f.gmean = atof(c[22].c_str());
    int p = atoi(c[0].c_str());
    if (c[4] == "all") all[p] = f;
    else if (c[4] == "spot") spot[p] = f;
    else {
      f.ch = atoi(c[4].c_str());
      if (!pad.count(p) || f.mu > pad[p].mu) pad[p] = f;
    }
  }
  if (xs.size() < 2 || ys.size() < 2) { printf("plot_gain_map: not a 2-d scan\n") ; return ; }
  bool sums = !spot.empty();
  const std::map<int, Fit> &main_ = sums ? spot : pad;
  const char *what = sums ? "the spot (pads with 95 % of the charge)" : "the brightest channel";
  double dx = *std::next(xs.begin()) - *xs.begin(), dy = *std::next(ys.begin()) - *ys.begin();
  int nx = xs.size(), ny = ys.size();
  double x0 = *xs.begin() - dx / 2, y0 = *ys.begin() - dy / 2, x1 = x0 + nx * dx, y1 = y0 + ny * dy;

  gStyle->SetOptStat(0);
  gStyle->SetOptTitle(0);
  gStyle->SetPalette(kBird);
  auto *hmu = new TH2F("hmu", ";stage x [mm];stage y [mm];mu [p.e.]", nx, x0, x1, ny, y0, y1);
  auto *hg = new TH2F("hg", ";stage x [mm];stage y [mm];gain", nx, x0, x1, ny, y0, y1);
  auto *hr = new TH2F("hr", ";stage x [mm];stage y [mm];sum of all / spot - 1 [%]", nx, x0, x1, ny, y0, y1);
  std::map<int, std::vector<double>> g_mean;
  // per pad: the points where it is the brightest channel
  std::map<int, std::vector<double>> g_main, g_spot, g_pad, rlo, rhi;
  std::map<int, double> sx, sy, sw;        // mu-weighted centre of each pad's points
  for (auto &[p, f] : main_) {
    hmu->Fill(f.x, f.y, f.mu);
    hg->Fill(f.x, f.y, f.gain);
    if (sums && all.count(p)) hr->Fill(f.x, f.y, 100 * (all.at(p).gain / f.gain - 1));
    if (!pad.count(p)) continue;
    int ch = pad[p].ch;
    g_main[ch].push_back(f.gain);
    g_pad[ch].push_back(pad[p].gain);
    if (all.count(p)) g_spot[ch].push_back(all.at(p).gain);
    if (all.count(p) && !std::isnan(all.at(p).gmean)) g_mean[ch].push_back(all.at(p).gmean);
    if (!std::isnan(f.shi)) { rlo[ch].push_back(f.slo / f.gain) ; rhi[ch].push_back(f.shi / f.gain) ; }
    sx[ch] += f.mu * f.x ; sy[ch] += f.mu * f.y ; sw[ch] += f.mu;
  }
  if (zmax_gain > 0) hg->SetMaximum(zmax_gain);
  hg->SetMinimum(0);
  bool have_ratio = hr->GetEntries() > 0;
  if (have_ratio) {   // points without both fits stay blank, not 0 %
    hr->SetMinimum(-20) ; hr->SetMaximum(20);
    std::set<int> filled;
    for (auto &[p, f] : main_) if (all.count(p)) filled.insert(hr->FindBin(f.x, f.y));
    for (int bx = 1; bx <= nx; bx++) for (int by = 1; by <= ny; by++) if (!filled.count(hr->GetBin(bx, by))) hr->SetBinContent(bx, by, -1e9);
  }

  int npad = have_ratio ? 4 : 3;
  auto *c = new TCanvas("c", "", 1500, 400 * npad);
  c->Divide(1, npad, 0.002, 0.002);
  TLatex tx;
  tx.SetTextSize(0.05);
  tx.SetTextAlign(22);
  std::vector<TH2F *> maps = {hmu, hg};
  std::vector<std::string> titles = {Form("mu of %s (numbers: the brightest channel)", what),
                                     Form("gain of %s, main window (numbers: the brightest channel)", what)};
  if (have_ratio) { maps.push_back(hr) ; titles.push_back("gain of the sum of all good channels relative to the spot, capped at +-20 %") ; }
  for (size_t k = 0; k < maps.size(); k++) {
    auto *pk = c->cd(1 + k);
    pk->SetRightMargin(0.17) ; pk->SetLeftMargin(0.07) ; pk->SetBottomMargin(0.14);
    TH2F *h = maps[k];
    h->GetZaxis()->SetTitleOffset(0.75) ; h->GetYaxis()->SetTitleOffset(0.5);
    for (auto *a : {h->GetXaxis(), h->GetYaxis(), h->GetZaxis()}) { a->SetTitleSize(0.055) ; a->SetLabelSize(0.05) ; }
    tb::draw_y_down(h, "colz");
    for (auto &[ch, w] : sw)
      if (g_main[ch].size() >= 3) tx.DrawLatex(sx[ch] / w, tb::y_down(sy[ch] / w), Form("%d", ch));
    TLatex t2 ; t2.SetNDC() ; t2.SetTextSize(0.055);
    t2.DrawLatex(0.07, 0.93, titles[k].c_str());
  }

  // per pad: medians over its points; the spread of the main gain (16-84 %) as the bar
  auto quant = [](std::vector<double> v, double q) -> double { if (v.empty()) return NAN ; std::sort(v.begin(), v.end()) ; return v[int(q * (v.size() - 1) + 0.5)] ; };
  printf("  pad  points   gain of %s (16-84 %%)   sum of all  mean charge  pad alone   spot/pad  window systematic\n", sums ? "the spot" : "the pad");
  auto *gm = new TGraphErrors(), *gs = new TGraphErrors(), *gp = new TGraphErrors(), *gq = new TGraphErrors();
  auto *gsys = new TGraphAsymmErrors();
  for (auto &[ch, v] : g_main) {
    int n = v.size();
    double med = quant(v, 0.5), lo = quant(v, 0.16), hi = quant(v, 0.84);
    double ms = quant(g_spot[ch], 0.5), mp = quant(g_pad[ch], 0.5), slo = quant(rlo[ch], 0.5), shi = quant(rhi[ch], 0.5);
    double mm = quant(g_mean[ch], 0.5);
    printf("  %-4d %6d   %.3e (%.2e - %.2e)   %.3e   %.3e    %.3e   %.2f", ch, n, med, lo, hi, ms, mm, mp, med / mp);
    if (!std::isnan(shi)) printf("      -%.1f %% +%.1f %%", 100 * slo, 100 * shi);
    printf("\n");
    if (n < 3) continue;
    int i = gm->GetN();
    gm->SetPoint(i, ch, med) ; gm->SetPointError(i, 0, (hi - lo) / 2);
    if (sums) gp->SetPoint(gp->GetN(), ch - 0.25, mp);
    if (!std::isnan(ms)) gs->SetPoint(gs->GetN(), ch + 0.25, ms);
    if (!std::isnan(mm)) gq->SetPoint(gq->GetN(), ch + 0.38, mm);
    if (!std::isnan(shi)) { int j = gsys->GetN() ; gsys->SetPoint(j, ch + 0.12, med) ; gsys->SetPointError(j, 0, 0, slo * med, shi * med) ; }
  }
  auto *pd = c->cd(npad);
  pd->SetRightMargin(0.17) ; pd->SetLeftMargin(0.07) ; pd->SetBottomMargin(0.14) ; pd->SetGridy();
  double ymax = 1e6;
  for (auto *g : {gm, gs, gp, gq}) for (int k = 0; k < g->GetN(); k++) ymax = std::max(ymax, g->GetY()[k] + g->GetErrorY(k));
  auto *fr = pd->DrawFrame(-0.5, 0, 31.5, 1.15 * ymax);
  fr->GetXaxis()->SetTitle("pad (the brightest channel at the point)");
  fr->GetYaxis()->SetTitle("gain");
  fr->GetXaxis()->SetNdivisions(32);
  for (auto *a : {fr->GetXaxis(), fr->GetYaxis()}) { a->SetTitleSize(0.055) ; a->SetLabelSize(0.05) ; }
  fr->GetYaxis()->SetTitleOffset(0.5);
  int blue = TColor::GetColor("#2a78d6"), orange = TColor::GetColor("#eb6834"), green = TColor::GetColor("#1baf7a"), grey = TColor::GetColor("#898781");
  gp->SetMarkerStyle(24) ; gp->SetMarkerSize(1.1) ; gp->SetMarkerColor(grey);
  gs->SetMarkerStyle(21) ; gs->SetMarkerSize(1.0) ; gs->SetMarkerColor(green);
  gq->SetMarkerStyle(33) ; gq->SetMarkerSize(1.5) ; gq->SetMarkerColor(TColor::GetColor("#6250d6"));
  gm->SetMarkerStyle(20) ; gm->SetMarkerSize(1.2) ; gm->SetMarkerColor(blue) ; gm->SetLineColor(blue) ; gm->SetLineWidth(2);
  gsys->SetLineColor(orange) ; gsys->SetLineWidth(4) ; gsys->SetMarkerSize(0);
  auto *leg = new TLegend(0.835, 0.35, 0.995, 0.88);
  leg->SetTextSize(0.042);
  leg->AddEntry(gm, sums ? "spot (95 % of the charge)" : "pad alone", "lp");
  if (gsys->GetN()) { gsys->Draw("Z") ; leg->AddEntry(gsys, "window systematic", "l") ; }
  if (gs->GetN()) { gs->Draw("P") ; leg->AddEntry(gs, "sum of all good channels", "p") ; }
  if (gq->GetN()) { gq->Draw("P") ; leg->AddEntry(gq, "mean charge / mu (no fit)", "p") ; }
  if (gp->GetN()) { gp->Draw("P") ; leg->AddEntry(gp, "pad alone", "p") ; }
  gm->Draw("P");
  leg->Draw();
  TLatex t3 ; t3.SetNDC() ; t3.SetTextSize(0.055);
  t3.DrawLatex(0.07, 0.93, "per pad: median gain of the points where it is the brightest (bar: 16-84 % of points)");
  c->SaveAs(Form("%s.png", prefix));
}
