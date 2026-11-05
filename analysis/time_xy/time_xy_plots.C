// Hit time vs laser position for an x-y scan (any step) or an x line, from the time_xy_point parts. Usually run by
// time_xy.sh; by hand:
//   root -b -q 'time_xy_plots.C+("<scan>.csv", "<parts>", "<out>", "pad:20", "p-0.5:p+1.5:0.05", true, "<geo>", "1 4 6")'
// channels: "20,19,23" or "pad:20" (20 and its neighbours); slices: see txy::slices (p = the first channel's laser
// peak); align: line each channel's peak up with the first's; geo: see txy::Geo; masked: inputs left out of "pad:".
// Writes
//   <out>_spectrum.png             hits per event per point vs time, all points, slices marked
//   <out>_time_vs_x.png (_zoom)    time vs laser x for the points on the first channel's pad row (all, for an x line);
//   <out>_time_vs_y.png (_zoom)    time vs laser y on its pad column (x-y scans); zoom = peak -2 to +4 ns
//   <out>_xy_slices.png            x-y scans: hits per event per point in each slice; more than 6 slices in a grid of
//                                  small maps, each on its own log scale (2 decades), sparse ones in merged bins
//   <out>_x_slices.png             x lines: hits per event vs x per slice (one plot, or one panel per slice for > 6)
//   <out>_x_slices_scaled.png      x lines, > 6 slices: x vs slice time, each slice scaled to its own maximum
//   <out>.root                     the histograms
#include "time_xy_common.h"
#include <TCanvas.h>
#include <TColor.h>
#include <TError.h>
#include <TH2D.h>
#include <TLegend.h>
#include <TStyle.h>
#include <algorithm>

void time_xy_plots(const char *scan_csv, const char *parts, const char *out, const char *chan_spec = "pad:20",
                   const char *slice_spec = "p-7:p-1,p-0.6:p+0.6,p+0.6:p+3,p+3:p+15,p+15:p+20", bool align = true,
                   const char *geo_spec = "", const char *masked = "")
{
  gStyle->SetOptStat(0);
  gStyle->SetPalette(kBird);
  gStyle->SetNumberContours(64);
  txy::Scan s;
  if (!s.read(scan_csv)) { printf("no ok points in %s\n", scan_csv) ; return ; }
  txy::Geo geo(geo_spec);
  std::vector<int> chs = txy::channels(chan_spec, parts, s, txy::int_set(masked));
  std::set<int> use(chs.begin(), chs.end());
  std::string chlist;
  for (int c : chs) chlist += (chlist.empty() ? "" : ",") + std::to_string(c);
  txy::Peaks pk;
  pk.find(parts, s, chs, align);
  { std::vector<int> k; for (int c : chs) if (!pk.drop.count(c)) k.push_back(c); chs = k; }
  use = std::set<int>(chs.begin(), chs.end());
  chlist.clear();
  for (int c : chs) chlist += (chlist.empty() ? "" : ",") + std::to_string(c);
  if (std::isnan(pk.ref)) { printf("no parts in %s\n", parts) ; return ; }
  auto win = txy::slices(slice_spec, pk.ref);
  printf("channels %s, laser peak %.2f ns; %zu slices %.2f-%.2f ns\n", chlist.c_str(), pk.ref, win.size(), win.front().first, win.back().second);

  // the bands for time vs x / y: the first channel's pad row / column (all points without a grid or pixel)
  double bylo = -1e9, byhi = 1e9, bxlo = -1e9, bxhi = 1e9;
  int r0, c0;
  if (geo.ok() && s.ny > 1 && txy::pixel_of(parts, s, chs[0], r0, c0)) {
    bylo = geo.y(r0) - geo.py / 2 ; byhi = geo.y(r0) + geo.py / 2 ; bxlo = geo.x(c0) - geo.px / 2 ; bxhi = geo.x(c0) + geo.px / 2;
    printf("input %d on pixel (%d,%d): pad x %.2f-%.2f, y %.2f-%.2f mm\n", chs[0], r0, c0, bxlo, bxhi, bylo, byhi);
  }
  bool yband = bylo > -1e8, xband = bxlo > -1e8;

  const int NT = 700; const double T0 = 10, T1 = 45;          // the parts' 50 ps bins
  TH1D *spec = new TH1D("spec", Form("channels %s, all points;time - trigger [ns];hits per event per point", chlist.c_str()), NT, T0, T1);
  TH2D *tx = new TH2D("tx", yband ? Form("channels %s, laser y %.1f-%.1f mm;laser x [mm];time - trigger [ns];hits per event per point", chlist.c_str(), bylo, byhi)
                                  : Form("channels %s;laser x [mm];time - trigger [ns];hits per event per point", chlist.c_str()), s.nx, s.xlo, s.xhi, NT, T0, T1);
  TH2D *ty = new TH2D("ty", xband ? Form("channels %s, laser x %.1f-%.1f mm;laser y [mm];time - trigger [ns];hits per event per point", chlist.c_str(), bxlo, bxhi)
                                  : Form("channels %s;laser y [mm];time - trigger [ns];hits per event per point", chlist.c_str()), s.ny, s.ylo, s.yhi, NT, T0, T1);
  std::vector<TH2D *> xy;
  for (size_t k = 0; k < win.size(); k++)
    xy.push_back(new TH2D(Form("xy%zu", k), Form("%.2f-%.2f ns;laser x [mm];laser y [mm];hits per event", win[k].first, win[k].second),
                          s.nx, s.xlo, s.xhi, s.ny, s.ylo, s.yhi));
  std::map<double, int> ncol, nrow;   // points per x column / y row inside the bands
  int npts = 0; double nsum = 0;
  for (auto &p : s.pos) {
    TFile *f = txy::open_part(parts, p.first);
    if (!f) continue;
    TH2F *tc = (TH2F *)f->Get("tc"); TH1D *nev = (TH1D *)f->Get("nev");
    if (!tc || !nev || nev->GetBinContent(1) < 1) { f->Close() ; delete f ; continue ; }
    double n = nev->GetBinContent(1), x = p.second.first, y = p.second.second;
    bool in_y = y >= bylo && y <= byhi, in_x = x >= bxlo && x <= bxhi;
    if (in_y) ncol[x]++;
    if (in_x) nrow[y]++;
    npts++; nsum += n;
    std::vector<double> sum(win.size(), 0);
    for (int ch : use)
      for (int b = 1; b <= tc->GetNbinsY(); b++) {
        double c = tc->GetBinContent(ch + 1, b);
        if (c == 0) continue;
        double t = tc->GetYaxis()->GetBinCenter(b + pk.shift[ch]);
        spec->Fill(t, c / n);
        if (in_y) tx->Fill(x, t, c / n);
        if (in_x) ty->Fill(y, t, c / n);
        for (size_t k = 0; k < win.size(); k++) if (t >= win[k].first && t < win[k].second) sum[k] += c / n;
      }
    for (size_t k = 0; k < win.size(); k++) xy[k]->Fill(x, y, sum[k]);
    f->Close() ; delete f;
  }
  spec->Scale(1.0 / npts);
  auto per_point = [&](TH2D *h, std::map<double, int> &cnt, double step) {
    for (int i = 1; i <= h->GetNbinsX(); i++) {
      double v = h->GetXaxis()->GetBinCenter(i); int m = 0;
      for (auto &c : cnt) if (std::fabs(c.first - v) < step / 2) m = c.second;
      for (int j = 1; j <= NT; j++) if (m) h->SetBinContent(i, j, h->GetBinContent(i, j) / m);
    }
  };
  per_point(tx, ncol, s.dx);
  per_point(ty, nrow, s.dy);
  double nev_pt = nsum / std::max(npts, 1);
  printf("%d points, %zu x columns, %zu y rows, %.0f events per point\n", npts, ncol.size(), nrow.size(), nev_pt);

  TCanvas c1("c1", "", 900, 550); c1.SetLogy(); c1.SetGridx();
  spec->SetLineColor(kBlack); spec->Draw("hist");
  c1.Update();
  TLine l; l.SetLineColor(kRed + 1); l.SetLineStyle(2);
  double ylo_f = std::pow(10, gPad->GetUymin()), yhi_f = std::pow(10, gPad->GetUymax());
  if (win.size() <= 12) for (auto &w : win) { l.DrawLine(w.first, ylo_f, w.first, yhi_f) ; l.DrawLine(w.second, ylo_f, w.second, yhi_f) ; }
  else { l.DrawLine(win.front().first, ylo_f, win.front().first, yhi_f) ; l.DrawLine(win.back().second, ylo_f, win.back().second, yhi_f) ; }
  c1.SaveAs(Form("%s_spectrum.png", out));

  for (TH2D *h : {tx, ty}) {
    if (h == ty && s.ny == 1) continue;   // an x line has no y
    TCanvas c("c", "", 1000, 650); c.SetLogz(); c.SetRightMargin(0.14);
    h->SetMinimum(1e-6); h->Draw("colz");
    h->GetYaxis()->SetRangeUser(T0, T1);
    c.SaveAs(Form("%s_%s.png", out, h == tx ? "time_vs_x" : "time_vs_y"));
    h->GetYaxis()->SetRangeUser(pk.ref - 2, pk.ref + 4);   // the laser peak and its shoulder
    c.SaveAs(Form("%s_%s_zoom.png", out, h == tx ? "time_vs_x" : "time_vs_y"));
  }

  // merged bins for a sparse slice: about 10 hits per bin on average
  auto merge_factor = [&](double integral, int nbins, std::initializer_list<int> fs, int dim) {
    double mean = integral / nbins * nev_pt;
    int f = 1;
    for (int g : fs) if (mean * std::pow(f, dim) < 10) f = g;
    return f;
  };
  int mc = std::ceil(std::sqrt(win.size() * 1.6)), mr = (win.size() + mc - 1) / mc;

  if (s.ny == 1) {   // an x line
    if (win.size() <= 6) {
      TCanvas c4("c4", "", 1000, 600); c4.SetLogy();
      TLegend lg(0.80, 0.70, 0.98, 0.95);
      const int col[] = {kBlack, kRed + 1, kBlue + 1, kGreen + 2, kMagenta + 1, kOrange + 7};
      for (size_t k = 0; k < win.size(); k++) {
        TH1D *px = xy[k]->ProjectionX(Form("px%zu", k));
        px->SetTitle(Form("channels %s, laser y %.2f mm;laser x [mm];hits per event", chlist.c_str(), *s.ys.begin()));
        px->SetLineColor(col[k % 6]); px->SetLineWidth(2);
        px->SetMinimum(1e-5) ; px->SetMaximum(1);
        px->Draw(k ? "hist same" : "hist");
        lg.AddEntry(px, xy[k]->GetTitle(), "l");
      }
      lg.Draw();
      geo.draw(s.xlo, s.xhi, 1e-5, 1, true);
      c4.SaveAs(Form("%s_x_slices.png", out));
    } else {
      TCanvas c5("c5", "", 330 * mc, 260 * mr); c5.Divide(mc, mr, 0.002, 0.002);
      for (size_t k = 0; k < win.size(); k++) {
        c5.cd(k + 1); gPad->SetLogy(); gPad->SetTopMargin(0.12); gPad->SetBottomMargin(0.14); gPad->SetLeftMargin(0.16); gPad->SetRightMargin(0.03);
        TH1D *px = xy[k]->ProjectionX(Form("pm%zu", k));
        int f = merge_factor(px->Integral(), px->GetNbinsX(), {2, 5, 10}, 1);
        if (f > 1) { gErrorIgnoreLevel = kError ; px->Rebin(f) ; px->Scale(1.0 / f) ; gErrorIgnoreLevel = kInfo ; }   // a part group at the top is dropped
        px->SetTitle(Form("%.2f-%.2f ns (%.1f mm bins);x [mm];hits/event", win[k].first, win[k].second, f * s.dx));
        px->SetLineColor(kBlack); px->SetLineWidth(1);
        double mx = px->GetBinContent(px->GetMaximumBin()), mn = mx;
        for (int b = 1; b <= px->GetNbinsX(); b++) if (px->GetBinContent(b) > 0) mn = std::min(mn, px->GetBinContent(b));
        px->SetMaximum(mx * 3); px->SetMinimum(std::max(mn / 3, mx * 1e-3));
        for (auto *a : {px->GetXaxis(), px->GetYaxis()}) { a->SetLabelSize(0.06); a->SetTitleSize(0.06); }
        px->GetYaxis()->SetTitleOffset(1.3);
        gStyle->SetTitleFontSize(0.08);
        px->Draw("hist");
        gPad->Update();
        geo.draw(s.xlo, s.xhi, std::pow(10, gPad->GetUymin()), std::pow(10, gPad->GetUymax()), true);
      }
      c5.SaveAs(Form("%s_x_slices.png", out));
      // x vs slice time, each slice (row) scaled to its own maximum: which times follow the pad and which are flat
      std::vector<double> te;
      for (auto &w : win) te.push_back(w.first);
      te.push_back(win.back().second);
      bool contiguous = true;
      for (size_t k = 1; k < win.size(); k++) contiguous &= std::fabs(win[k].first - win[k - 1].second) < 1e-6;
      if (contiguous) {
        int fx = std::max(1, (int)std::lround(0.5 / s.dx));   // 0.5 mm columns
        TH2D *sx = new TH2D("x_vs_slice", Form("channels %s, laser y %.2f mm, each time slice scaled to its maximum;laser x [mm];time - trigger [ns];hits per event / slice maximum",
                                               chlist.c_str(), *s.ys.begin()), s.nx / fx, s.xlo, s.xlo + s.nx / fx * fx * s.dx, te.size() - 1, te.data());
        for (size_t k = 0; k < win.size(); k++) {
          TH1D *px = xy[k]->ProjectionX(Form("ps%zu", k));
          gErrorIgnoreLevel = kError; px->Rebin(fx); gErrorIgnoreLevel = kInfo;
          double mx = px->GetBinContent(px->GetMaximumBin());
          if (mx > 0) for (int b = 1; b <= px->GetNbinsX(); b++) sx->SetBinContent(b, k + 1, px->GetBinContent(b) / mx);
        }
        TCanvas c6("c6", "", 1100, 750); c6.SetRightMargin(0.16);
        sx->SetMinimum(0); sx->SetMaximum(1);
        sx->Draw("colz");
        geo.draw(s.xlo, s.xhi, te.front(), te.back(), true);
        c6.SaveAs(Form("%s_x_slices_scaled.png", out));
      }
    }
  } else if (win.size() <= 6) {
    int nc = win.size() <= 2 ? win.size() : 2, nr = (win.size() + nc - 1) / nc;
    TCanvas c3("c3", "", 900 * nc, 520 * nr); c3.Divide(nc, nr);
    for (size_t k = 0; k < win.size(); k++) {
      c3.cd(k + 1); gPad->SetRightMargin(0.15); gPad->SetLogz();
      double mx = xy[k]->GetMaximum();
      xy[k]->SetMinimum(std::max(mx * 1e-3, 1e-6)); xy[k]->Draw("colz");
      geo.draw(s.xlo, s.xhi, s.ylo, s.yhi);
    }
    c3.SaveAs(Form("%s_xy_slices.png", out));
  } else {
    TCanvas c7("c7", "", 340 * mc, 320 * mr); c7.Divide(mc, mr, 0.002, 0.002);
    for (size_t k = 0; k < win.size(); k++) {
      c7.cd(k + 1); gPad->SetLogz(); gPad->SetTopMargin(0.1); gPad->SetBottomMargin(0.1); gPad->SetLeftMargin(0.1); gPad->SetRightMargin(0.16);
      TH2D *h = (TH2D *)xy[k]->Clone(Form("xym%zu", k));
      int f = merge_factor(h->Integral(), s.nx * s.ny, {2, 3, 4, 6}, 2);
      if (f > 1) { gErrorIgnoreLevel = kError ; h->Rebin2D(f, f) ; h->Scale(1.0 / (f * f)) ; gErrorIgnoreLevel = kInfo ; }
      h->SetTitle(Form("%.2f-%.2f ns (%.1f mm bins);;", win[k].first, win[k].second, f * s.dx));
      h->SetMinimum(h->GetMaximum() * 1e-2);   // two decades per panel
      for (auto *a : {h->GetXaxis(), h->GetYaxis(), h->GetZaxis()}) a->SetLabelSize(0.05);
      gStyle->SetTitleFontSize(0.07);
      h->Draw("colz");
      geo.draw(s.xlo, s.xhi, s.ylo, s.yhi);
    }
    c7.SaveAs(Form("%s_xy_slices.png", out));
  }

  TFile fo(Form("%s.root", out), "RECREATE");
  spec->Write(); tx->Write(); if (s.ny > 1) ty->Write(); for (auto *h : xy) h->Write();
  fo.Close();
}
