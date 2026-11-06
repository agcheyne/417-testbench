// Where the laser peak comes first: per scan point, the median and spread (half the 16-84 % width) of the hit times
// inside a window around the first channel's laser peak, summed over the listed channels (aligned on the first's peak).
// Usually run by time_xy.sh; by hand:
//   root -b -q 'peak_time_map.C+("<scan>.csv", "<parts>", "<out>", "20", -0.4, 0.6, 30, "<geo>", "1 4 6")'
// lo, hi: the window relative to the peak [ns]; min_hits: points with fewer hits in it get no median.
// Writes <out>_peak_time_map.png (hits, median time, spread) and <out>_peak_time_map.root.
#include "time_xy_common.h"
#include <TCanvas.h>
#include <TH2D.h>
#include <TStyle.h>
#include <algorithm>

void peak_time_map(const char *scan_csv, const char *parts, const char *out, const char *chan_spec = "pad:20",
                   double lo = -0.4, double hi = 0.6, int min_hits = 30, const char *geo_spec = "", const char *masked = "")
{
  gStyle->SetOptStat(0);
  gStyle->SetPalette(kBird);
  gStyle->SetNumberContours(64);
  txy::Scan s;
  if (!s.read(scan_csv)) { printf("no ok points in %s\n", scan_csv) ; return ; }
  if (s.ny == 1) { printf("an x line: no x-y map (use time_xy_plots.C)\n") ; return ; }
  txy::Geo geo(geo_spec);
  std::vector<int> chs = txy::channels(chan_spec, parts, s, txy::int_set(masked));
  std::string chlist;
  for (int c : chs) chlist += (chlist.empty() ? "" : ",") + std::to_string(c);
  txy::Peaks pk;
  pk.find(parts, s, chs, true);
  { std::vector<int> k; for (int c : chs) if (!pk.drop.count(c)) k.push_back(c); chs = k; }
  chlist.clear();
  for (int c : chs) chlist += (chlist.empty() ? "" : ",") + std::to_string(c);
  if (std::isnan(pk.ref)) { printf("no parts in %s\n", parts) ; return ; }
  double t0 = pk.ref + lo, t1 = pk.ref + hi;
  printf("channels %s, laser peak %.2f ns, window %.2f-%.2f ns\n", chlist.c_str(), pk.ref, t0, t1);
  TH2D *med = new TH2D("med", Form("ch %s: median hit time in %.2f-%.2f ns (points with >= %d hits);laser x [mm];laser y [mm];median time - trigger [ns]", chlist.c_str(), t0, t1, min_hits),
                       s.nx, s.xlo, s.xhi, s.ny, s.ylo, s.yhi);
  TH2D *wid = new TH2D("wid", Form("ch %s: half width (16-84 %%) of the hit times;laser x [mm];laser y [mm];half width [ns]", chlist.c_str()),
                       s.nx, s.xlo, s.xhi, s.ny, s.ylo, s.yhi);
  TH2D *hits = new TH2D("hits", Form("ch %s: hits per event in %.2f-%.2f ns;laser x [mm];laser y [mm];hits per event", chlist.c_str(), t0, t1),
                        s.nx, s.xlo, s.xhi, s.ny, s.ylo, s.yhi);
  std::vector<double> meds;
  for (auto &p : s.pos) {
    TFile *f = txy::open_part(parts, p.first);
    if (!f) continue;
    TH2F *tc = (TH2F *)f->Get("tc"); TH1D *nev = (TH1D *)f->Get("nev");
    if (!tc || !nev || nev->GetBinContent(1) < 1) { f->Close() ; delete f ; continue ; }
    TH1D t("t", "", 700, 10, 45);   // the listed channels, each shifted onto the first's peak
    for (int ch : chs)
      for (int b = 1; b <= 700; b++) {
        int bs = b + pk.shift[ch];
        if (bs >= 1 && bs <= 700) t.AddBinContent(bs, tc->GetBinContent(ch + 1, b));
      }
    int b0 = t.FindBin(t0 + 1e-6), b1 = t.FindBin(t1 - 1e-6);
    double n = 0;
    for (int b = b0; b <= b1; b++) n += t.GetBinContent(b);
    double x = p.second.first, y = p.second.second;
    hits->Fill(x, y, n / nev->GetBinContent(1));
    if (n >= min_hits) {
      auto quant = [&](double q) {   // linear inside the 50 ps bin
        double c = 0;
        for (int b = b0; b <= b1; b++) {
          double v = t.GetBinContent(b);
          if (v > 0 && c + v >= q * n) return t.GetBinLowEdge(b) + t.GetBinWidth(b) * (q * n - c) / v;
          c += v;
        }
        return t1;
      };
      double m = quant(0.5);
      med->Fill(x, y, m);
      wid->Fill(x, y, 0.5 * (quant(0.84) - quant(0.16)));
      meds.push_back(m);
    }
    f->Close() ; delete f;
  }
  // colour range: the earliest 5 % of the medians to 0.25 ns later
  std::sort(meds.begin(), meds.end());
  double mlo = meds.empty() ? t0 : std::floor(meds[meds.size() / 20] / 0.05) * 0.05;
  TCanvas c("c", "", 2100, 650); c.Divide(3, 1);
  c.cd(1); gPad->SetRightMargin(0.16); gPad->SetLogz(); tb::draw_y_down(hits, "colz"); geo.draw(s.xlo, s.xhi, s.ylo, s.yhi);
  c.cd(2); gPad->SetRightMargin(0.18);
  med->SetMinimum(mlo); med->SetMaximum(mlo + 0.25); med->GetZaxis()->SetTitleOffset(1.5); tb::draw_y_down(med, "colz"); geo.draw(s.xlo, s.xhi, s.ylo, s.yhi);
  c.cd(3); gPad->SetRightMargin(0.18);
  wid->SetMinimum(0.05); wid->SetMaximum(0.2); wid->GetZaxis()->SetTitleOffset(1.5); tb::draw_y_down(wid, "colz"); geo.draw(s.xlo, s.xhi, s.ylo, s.yhi);
  c.SaveAs(Form("%s_peak_time_map.png", out));
  TFile fo(Form("%s_peak_time_map.root", out), "RECREATE");
  hits->Write(); med->Write(); wid->Write();
  fo.Close();
}
