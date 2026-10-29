// Plot of a lamp scan (bin/lamp_scan.sh) in batch; plot_lamp_scan.sh runs it:
//   root -b -q 'plot_lamp_scan.C("<scan>.csv", "<scan dir>/<scan>", "<scan>_darks.csv")'
// Writes <prefix>_spectrum.png: left, light - dark against wavelength with its standard error, and
// the light and dark, points with a flag (bin/lamp_scan.sh -h) circled; right, the darks taken
// before and after the scan against wavelength (the meter's drift in between). Scans from before
// the darks file (a dark at every point, darks = ""): right, the dark against time.
// Prints the table in pW.
#include <cmath>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "TCanvas.h"
#include "TColor.h"
#include "TDatime.h"
#include "TGraph.h"
#include "TGraphErrors.h"
#include "TLegend.h"
#include "TLine.h"
#include "TMultiGraph.h"
#include "TStyle.h"

void plot_lamp_scan(const char *csv, const char *prefix, const char *darks = "")
{
  struct Point { double t, nm, light, dark, net, err; std::string flags; };
  std::vector<Point> pts;
  std::ifstream in(csv);
  std::string line;
  std::getline(in, line);                      // header
  while (std::getline(in, line)) {
    std::vector<std::string> f;
    std::stringstream ss(line);
    for (std::string c; std::getline(ss, c, ',');) f.push_back(c);
    if (f.size() < 11) continue;
    TDatime d(f[0].c_str());                   // "YYYY-MM-DD HH:MM:SS"
    pts.push_back({double(d.Convert()), std::stod(f[1]), std::stod(f[2]) * 1e12, std::stod(f[4]) * 1e12,
                   std::stod(f[6]) * 1e12, std::stod(f[7]) * 1e12, f[10]});
  }
  if (pts.empty()) { printf("plot_lamp_scan: no points in %s\n", csv); return; }

  // darks before ("start") and after ("end") the scan: time,nm,when,dark_W,dark_sd_W,n,range,flags
  auto *dk_start = new TGraphErrors(), *dk_end = new TGraphErrors();
  std::ifstream din(darks);
  if (din) {
    std::getline(din, line);
    while (std::getline(din, line)) {
      std::vector<std::string> f;
      std::stringstream ss(line);
      for (std::string c; std::getline(ss, c, ',');) f.push_back(c);
      if (f.size() < 8) continue;
      auto *g = f[2] == "start" ? dk_start : dk_end;
      int i = g->GetN();
      g->SetPoint(i, std::stod(f[1]), std::stod(f[3]) * 1e12);
      g->SetPointError(i, 0, std::stod(f[4]) * 1e12 / std::sqrt(std::stod(f[5])));
    }
  }
  const bool two_darks = dk_start->GetN() > 0;

  printf("%8s %10s %10s %10s %8s  %s\n", "nm", "light pW", "dark pW", "net pW", "+- pW", "flags");
  for (auto &p : pts)
    printf("%8g %10.2f %10.2f %10.2f %8.2f  %s\n", p.nm, p.light, p.dark, p.net, p.err, p.flags == "-" ? "" : p.flags.c_str());

  gStyle->SetOptTitle(0);
  int c_net = TColor::GetColor("#2a78d6"), c_light = TColor::GetColor("#898781"), c_dark = TColor::GetColor("#eb6834");
  auto *net = new TGraphErrors(), *light = new TGraphErrors(), *dark = new TGraphErrors();
  auto *drift = new TGraph(), *flagged = new TGraph();
  for (auto &p : pts) {
    int i = net->GetN();
    net->SetPoint(i, p.nm, p.net);
    net->SetPointError(i, 0, p.err);
    light->SetPoint(i, p.nm, p.light);
    dark->SetPoint(i, p.nm, p.dark);
    drift->SetPoint(i, (p.t - pts.front().t) / 60, p.dark);
    if (p.flags != "-") flagged->SetPoint(flagged->GetN(), p.nm, p.net);
  }
  flagged->SetMarkerStyle(24); flagged->SetMarkerSize(2.2); flagged->SetMarkerColor(kBlack);
  net->SetLineColor(c_net); net->SetMarkerColor(c_net); net->SetMarkerStyle(20); net->SetLineWidth(2);
  light->SetLineColor(c_light); light->SetLineStyle(2); light->SetLineWidth(2);
  dark->SetLineColor(c_dark); dark->SetLineStyle(3); dark->SetLineWidth(2);
  drift->SetLineColor(c_dark); drift->SetMarkerColor(c_dark); drift->SetMarkerStyle(20); drift->SetLineWidth(2);

  auto *c = new TCanvas("c", "lamp scan", 1400, 550);
  c->Divide(2, 1);
  c->cd(1)->SetGrid();
  auto *mg = new TMultiGraph();
  mg->Add(light, "L");
  mg->Add(dark, "L");
  mg->Add(net, "PL");
  if (flagged->GetN()) mg->Add(flagged, "P");
  mg->SetTitle(";monochromator wavelength [nm];power [pW]");
  mg->Draw("A");
  auto *zero = new TLine(mg->GetXaxis()->GetXmin(), 0, mg->GetXaxis()->GetXmax(), 0);
  zero->SetLineColor(kGray + 1);
  zero->Draw();
  auto *leg = new TLegend(0.55, 0.72, 0.89, 0.89);
  leg->AddEntry(net, "light - dark (#pm std. error)", "PL");
  leg->AddEntry(light, "light, as read", "L");
  leg->AddEntry(dark, two_darks ? "dark, interpolated in time" : "dark (grating parked), as read", "L");
  if (flagged->GetN()) leg->AddEntry(flagged, "flagged: spike / retaken / noisy", "P");
  leg->Draw();

  c->cd(2)->SetGrid();
  if (two_darks) {
    dk_start->SetLineColor(c_net); dk_start->SetMarkerColor(c_net); dk_start->SetMarkerStyle(20); dk_start->SetLineWidth(2);
    dk_end->SetLineColor(c_dark); dk_end->SetMarkerColor(c_dark); dk_end->SetMarkerStyle(21); dk_end->SetLineWidth(2);
    auto *mgd = new TMultiGraph();
    mgd->Add(dk_start, "PL");
    if (dk_end->GetN()) mgd->Add(dk_end, "PL");
    mgd->SetTitle(";meter wavelength [nm];dark [pW] (grating parked)");
    mgd->Draw("A");
    auto *dleg = new TLegend(0.15, 0.78, 0.50, 0.89);
    dleg->AddEntry(dk_start, "before the scan", "PL");
    dleg->AddEntry(dk_end, dk_end->GetN() ? "after the scan" : "after the scan: not taken", "PL");
    dleg->Draw();
  } else {
    drift->SetTitle(";minutes since the first point;dark [pW] (meter at each point's wavelength)");
    drift->Draw("APL");
  }

  c->SaveAs(Form("%s_spectrum.png", prefix));
}
