// Timing over an xy scan (timing_map.sh's <scan>_timing.csv): at each point the brightest channel
// (most hits, fit ok, >= MINHITS hits; the MASK inputs left out): its main-peak core sigma, and its mean
// time less that channel's median over the map (each channel's cable / trigger offset out), as maps drawn
// as the bench face (stage y down); per pad the median core sigma and its 16-84 % spread.
//   root -b -q 'plot_timing_map.C+("<scan>_timing.csv", "<out>", 200, "0 3 4 5 6")'   -> <out>_timing_map.png
// MASK "-" = MASKED_CHANNELS in db/testbench.conf.
#include <fstream>
#include <sstream>
#include <map>
#include <set>
#include <vector>
#include <algorithm>
#include "TCanvas.h"
#include "TH2F.h"
#include "TH1F.h"
#include "TGraphAsymmErrors.h"
#include "TStyle.h"
#include "TSystem.h"
#include "TLatex.h"
#include "../../lib/tb_db.h"
#include "../../lib/tb_ydown.h"

void plot_timing_map(const char *csv, const char *out, int minhits = 200, const char *mask = "-")
{
  if (!gSystem->Getenv("TB_ROOT")) {   // the repository: two folders up from this macro
    TString f = __FILE__;
    if (!f.BeginsWith("/")) f = TString(gSystem->WorkingDirectory()) + "/" + f;
    gSystem->Setenv("TB_ROOT", gSystem->DirName(gSystem->DirName(gSystem->DirName(f))));
  }
  std::set<int> masked = tb::masked_channels(mask);
  struct R { int pt, ch; double x, y, hits, sig, mean; };
  std::map<int, R> best;
  std::set<double> xs, ys;
  std::ifstream in(csv);
  std::string l;
  std::getline(in, l);
  while (std::getline(in, l)) {
    std::vector<std::string> c;
    std::stringstream ss(l);
    for (std::string x; std::getline(ss, x, ',');) c.push_back(x);
    if (c.size() < 14) continue;
    double x = atof(c[1].c_str()), y = atof(c[2].c_str());
    xs.insert(x) ; ys.insert(y);
    double sig = atof(c[9].c_str()), hits = atof(c[5].c_str());
    int ch = atoi(c[3].c_str());
    if (masked.count(ch)) continue;
    if (c[13] != "1" || hits < minhits || !(sig > 0 && sig < 0.3)) continue;
    int pt = atoi(c[0].c_str());
    R r{pt, atoi(c[3].c_str()), x, y, hits, sig * 1000, atof(c[7].c_str()) * 1000};
    if (!best.count(pt) || hits > best[pt].hits) best[pt] = r;
  }
  std::map<int, std::vector<double>> sig, tm;
  for (auto &[p, r] : best) { sig[r.ch].push_back(r.sig) ; tm[r.ch].push_back(r.mean) ; }
  auto med = [](std::vector<double> v, double q = 0.5) { std::sort(v.begin(), v.end()) ; return v.empty() ? NAN : v[std::min(v.size() - 1, (size_t) (q * v.size()))] ; };
  std::map<int, double> tmed;
  for (auto &[ch, v] : tm) tmed[ch] = med(v);
  double dx = *std::next(xs.begin()) - *xs.begin(), dy = *std::next(ys.begin()) - *ys.begin();
  int nx = xs.size(), ny = ys.size();
  double x0 = *xs.begin() - dx / 2, y0 = *ys.begin() - dy / 2;
  gStyle->SetOptStat(0) ; gStyle->SetPalette(kBird) ; gStyle->SetTitleFontSize(0.045);
  auto *hs = new TH2F("hs", "core #sigma of the brightest channel (clamped to 50-150 ps);stage x [mm];stage y [mm];#sigma [ps]", nx, x0, x0 + nx * dx, ny, y0, y0 + ny * dy);
  auto *ht = new TH2F("ht", "mean time - that channel's median [ps];stage x [mm];stage y [mm];#Deltat [ps]", nx, x0, x0 + nx * dx, ny, y0, y0 + ny * dy);
  auto *h1 = new TH1F("h1", "core #sigma, all points;#sigma [ps];points", 60, 30, 150);
  for (auto &[p, r] : best) {
    hs->Fill(r.x, r.y, r.sig);
    ht->Fill(r.x, r.y, std::max(-99.0, std::min(99.0, r.mean - tmed[r.ch])));
    h1->Fill(r.sig);
  }
  std::vector<double> all;
  for (auto &[p, r] : best) all.push_back(r.sig);
  hs->SetMinimum(50) ; hs->SetMaximum(150);
  ht->SetMinimum(-100) ; ht->SetMaximum(100);
  // points without a fit: below the scale, so not drawn (white); fits outside it are clamped in
  std::set<int> filled;
  for (auto &[p, r] : best) filled.insert(hs->FindBin(r.x, r.y));
  for (int b = 0; b < (nx + 2) * (ny + 2); b++)
    if (!filled.count(b)) { hs->SetBinContent(b, -1e9) ; ht->SetBinContent(b, -1e9) ; }
    else hs->SetBinContent(b, std::max(50.5, std::min(149.5, hs->GetBinContent(b))));
  auto *g = new TGraphAsymmErrors();
  for (auto &[ch, v] : sig) {
    if (v.size() < 5) continue;
    double m = med(v);
    int n = g->GetN();
    g->SetPoint(n, ch, m) ; g->SetPointError(n, 0, 0, m - med(v, 0.16), med(v, 0.84) - m);
    printf("ch%2d: %3zu points, core sigma median %.0f ps (16-84 %%: %.0f-%.0f)\n", ch, v.size(), m, med(v, 0.16), med(v, 0.84));
  }
  auto *c = new TCanvas("c", "", 1800, 1200);
  c->Divide(2, 2, 0.002, 0.002);
  for (int k : {1, 2}) { c->cd(k) ; gPad->SetRightMargin(0.15) ; gPad->SetLeftMargin(0.1) ; }
  c->cd(1) ; hs->GetZaxis()->SetTitleOffset(1.2) ; tb::draw_y_down(hs, "COLZ");
  c->cd(2) ; ht->GetZaxis()->SetTitleOffset(1.2) ; tb::draw_y_down(ht, "COLZ");
  c->cd(3) ; gPad->SetGridy();
  auto *fr = gPad->DrawFrame(-0.5, 30, 31.5, 140);
  fr->SetTitle("core #sigma per pad, points where it is brightest, bar 16-84 %;V1742 input;#sigma [ps]");
  g->SetMarkerStyle(20) ; g->Draw("P");
  c->cd(4) ; h1->Draw();
  TLatex t;
  t.SetNDC() ; t.SetTextSize(0.035);
  t.DrawLatex(0.5, 0.8, Form("%zu points, median %.0f ps", best.size(), med(all)));
  t.DrawLatex(0.5, 0.74, Form("brightest channel, >= %d hits >= 5 mV", minhits));
  t.DrawLatex(0.5, 0.68, "tb::timing_fit core (left side + top)");
  c->SaveAs(Form("%s_timing_map.png", out));
}
