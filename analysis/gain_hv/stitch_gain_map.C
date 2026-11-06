// One gain map of the whole Photonis from the maps of its two readout halves (fit_gain_map.sh's
// <scan>_gain_map.csv, one per half): the spot gain of every point with a resolved photoelectron peak
// (peak = 1, outliers already cut), on one stage-coordinate grid (stage y down, as the bench face), one
// colour scale: <out>.png, and
// <out>_pads.png with the pads drawn over it; <out>_log.png and <out>_pads_log.png the same with a log
// colour scale from zmin_log. The pad grid, at the given pitch, is placed by the pad
// boundaries in the data: every pair of neighbouring scan points (1 mm apart) whose brightest pads (the
// first of spot_channels, at every fitted point) are neighbouring pads puts a boundary at their midpoint;
// x0 and y0 are the least-squares offsets over all of them (1 Nov 2026: 136 x and 138 y pairs, the two
// halves agree within 0.1 mm, a free pitch gives 6.54 / 6.49 mm). Pads are labelled with the readout and
// V1742 input (grey: a pad without points, a dead input or no light).
//   root -b -q 'stitch_gain_map.C("r0_gain_map.csv:0 r1_gain_map.csv:2", "out", 2.2e6)'
// The halves in readout order (the pad labels say R<index> ch<input>). Each csv is followed by the db/detectors/Photonis/mapping.txt slot of its half (readout 0: slot 0;
// readout 1: slot 2, the 180-degree-turned map) and optionally by a label (":2:readout 1, 1500 Hz", "_" for
// a space; "label@dx,dy" shifts that scan by dx, dy mm, e.g. "join@0.92,0": 2 Nov the detector sat 0.92 mm lower
// in x than on 30 Oct-1 Nov, from the join's pad boundaries and a 0.1 mm x line across pad (2,5)). A slot written "0s" applies the readout-0 slip of inputs 0-15 (27 Oct 2026, not in the db
// until the cable is checked): input N sits on the pad the map gives input N-1.
// More than two scans work too (e.g. the join scan of rows 2-5 as slot 3): where scans overlap, the later one wins.
// grid_inputs > 0 places the pad grid from the first grid_inputs scans only (e.g. to leave out a scan whose
// shift is not known yet: unshifted, the 2 Nov join pulled the grid 0.9 mm in x).
#include <fstream>
#include <sstream>
#include <map>
#include <set>
#include <vector>
#include "TCanvas.h"
#include "TH2F.h"
#include "TLine.h"
#include "TLatex.h"
#include "TStyle.h"
#include "TSystem.h"
#include "TMath.h"
#include "../../lib/tb_ydown.h"

void stitch_gain_map(const char *inputs, const char *prefix, double zmax = 2.2e6, double pitch = 6.5, double zmin_log = 0.5e6,
                     int grid_inputs = 0)
{
  std::map<std::pair<int, int>, std::pair<int, int>> map_slot;   // (slot, ch) -> (row, col)
  {
    std::ifstream m(Form("%s/db/detectors/Photonis/mapping.txt", gSystem->Getenv("TB_ROOT") ? gSystem->Getenv("TB_ROOT")
                                                                                              : "/home/nptest/417-testbench"));
    std::string l;
    while (std::getline(m, l)) {
      std::istringstream is(l.substr(0, l.find('#')));
      int s, ch, r, c;
      if (is >> s >> ch >> r >> c) map_slot[{s, ch}] = {r, c};
    }
  }
  struct Pt { double x, y, gain, mu; int half, ch; };
  std::vector<Pt> pts;
  std::set<double> xs, ys, xs0, ys0;   // all scans (range) / the first scan (step)
  std::vector<int> slots;
  std::vector<bool> slips;
  std::vector<std::string> labels;
  std::map<std::pair<int, int>, std::pair<int, int>> bright;   // point (x, y in 10 um) -> brightest pad (row, col)
  std::istringstream in(inputs);
  for (std::string arg; in >> arg;) {
    std::string csv = arg.substr(0, arg.find(':')), rest = arg.find(':') == std::string::npos ? "" : arg.substr(arg.find(':') + 1);
    int slot = atoi(rest.c_str());
    slips.push_back(rest.size() > 1 && rest[1] == 's');
    std::string label = rest.find(':') == std::string::npos ? "" : rest.substr(rest.find(':') + 1);
    double shx = 0, shy = 0;   // "label@dx,dy": add dx, dy [mm] to this scan's stage positions
    if (label.find('@') != std::string::npos) {
      sscanf(label.substr(label.find('@') + 1).c_str(), "%lf,%lf", &shx, &shy);
      label = label.substr(0, label.find('@'));
    }
    for (auto &c : label) if (c == '_') c = ' ';
    int half = slots.size();
    slots.push_back(slot) ; labels.push_back(label);
    std::ifstream f(csv);
    if (!f) { printf("stitch_gain_map: cannot read %s\n", csv.c_str()) ; return ; }
    std::string line;
    std::getline(f, line);
    int n = 0;
    while (std::getline(f, line)) {
      std::vector<std::string> c;
      std::stringstream ss(line);
      for (std::string x; std::getline(ss, x, ',');) c.push_back(x);
      if (c.size() < 21) continue;
      double x = atof(c[1].c_str()) + shx, y = atof(c[2].c_str()) + shy;
      xs.insert(x) ; ys.insert(y);
      if (half == 0) { xs0.insert(x) ; ys0.insert(y) ; }
      if (c[4] != "spot") continue;
      int ch = atoi(c[20].c_str()), mch = slips.back() && ch < 16 ? ch - 1 : ch;
      if ((grid_inputs <= 0 || half < grid_inputs) && !c[20].empty() && map_slot.count({slot, mch})) bright[{TMath::Nint(x * 100), TMath::Nint(y * 100)}] = map_slot[{slot, mch}];
      if (c[17] != "1") continue;
      pts.push_back({x, y, atof(c[5].c_str()), atof(c[7].c_str()), half, atoi(c[20].c_str())});
      n++;
    }
    printf("%s: %d points with a resolved spot gain\n", csv.c_str(), n);
  }
  // one grid over both scans: the 1 mm steps of the first, extended over the union of the ranges
  double dx = *std::next(xs0.begin()) - *xs0.begin(), dy = *std::next(ys0.begin()) - *ys0.begin();
  {   // the grid sits on the first scan's points and covers all scans
    double xa = *xs.begin(), xb = *xs.rbegin(), ya = *ys.begin(), yb = *ys.rbegin();
    xs = {*xs0.begin() - dx * std::ceil((*xs0.begin() - xa) / dx - 0.01), *xs0.rbegin() + dx * std::ceil((xb - *xs0.rbegin()) / dx - 0.01)};
    ys = {*ys0.begin() - dy * std::ceil((*ys0.begin() - ya) / dy - 0.01), *ys0.rbegin() + dy * std::ceil((yb - *ys0.rbegin()) / dy - 0.01)};
  }
  int nx = TMath::Nint((*xs.rbegin() - *xs.begin()) / dx) + 1, ny = TMath::Nint((*ys.rbegin() - *ys.begin()) / dy) + 1;
  double x0 = *xs.begin() - dx / 2, y0 = *ys.begin() - dy / 2, x1 = x0 + nx * dx, y1 = y0 + ny * dy;

  gStyle->SetOptStat(0) ; gStyle->SetOptTitle(0) ; gStyle->SetPalette(kBird);
  auto *hg = new TH2F("hg", ";stage x [mm];stage y [mm];gain [10^{6}]", nx, x0, x1, ny, y0, y1);
  // where scans overlap, the later input wins (e.g. the join scan over the seam between the halves)
  std::map<std::pair<int, int>, Pt> best;   // grid bin -> point
  for (auto &p : pts) best[{hg->GetXaxis()->FindBin(p.x), hg->GetYaxis()->FindBin(p.y)}] = p;
  std::map<std::pair<int, int>, std::pair<int, int>> who;   // pad -> (half, ch), the latest half lighting it
  for (auto &[b, p] : best) {
    hg->SetBinContent(b.first, b.second, p.gain / 1e6);
    int mch = slips[p.half] && p.ch < 16 ? p.ch - 1 : p.ch;
    auto m = map_slot.find({slots[p.half], mch});
    if (m == map_slot.end()) continue;
    if (!who.count(m->second) || who[m->second].first <= p.half) who[m->second] = {p.half, p.ch};
  }
  hg->SetMinimum(0) ; hg->SetMaximum(zmax / 1e6);

  // pad grid: x = gx0 + pitch * col, y = gy0 + pitch * row, placed by the boundaries between brightest pads
  double bxs = 0, bys = 0, bx2 = 0, by2 = 0;
  int nbx = 0, nby = 0;
  for (auto &[k, rc] : bright) {
    auto q = bright.find({k.first + TMath::Nint(dx * 100), k.second});
    if (q != bright.end() && q->second.first == rc.first && q->second.second == rc.second + 1) {
      double r = (k.first / 100.0 + dx / 2) - pitch * (rc.second + 0.5);
      bxs += r ; bx2 += r * r ; nbx++;
    }
    auto u = bright.find({k.first, k.second + TMath::Nint(dy * 100)});
    if (u != bright.end() && u->second.second == rc.second && u->second.first == rc.first + 1) {
      double r = (k.second / 100.0 + dy / 2) - pitch * (rc.first + 0.5);
      bys += r ; by2 += r * r ; nby++;
    }
  }
  double gx0 = bxs / nbx, gy0 = bys / nby;
  printf("pad grid, pitch %.2f mm, from %d x and %d y boundary pairs: pad (0,0) centre at (%.2f, %.2f) mm, boundary rms %.2f / %.2f mm\n",
         pitch, nbx, nby, gx0, gy0, std::sqrt(bx2 / nbx - gx0 * gx0), std::sqrt(by2 / nby - gy0 * gy0));

  for (int k = 0; k < 4; k++) {   // without, then with the pads; linear, then log colour scale
    int g = k % 2, lz = k / 2;
    auto *c = new TCanvas(Form("c%d", k), "", 1150, 1100);
    c->SetLogz(lz);
    hg->SetMinimum(lz ? zmin_log / 1e6 : 0) ; hg->GetZaxis()->SetMoreLogLabels(lz) ; hg->GetZaxis()->SetNoExponent(lz);
    c->SetRightMargin(0.15) ; c->SetLeftMargin(0.10) ; c->SetTopMargin(0.105) ; c->SetBottomMargin(0.09);
    hg->GetZaxis()->SetTitleOffset(1.1) ; hg->GetYaxis()->SetTitleOffset(1.2);
    tb::draw_y_down(hg, "COLZ");
    TLatex t;
    t.SetNDC() ; t.SetTextSize(0.028);
    t.DrawLatex(0.10, 0.95, Form("Photonis gain at 2100 V, %zu scans (%zu points, 1 mm)", slots.size(), best.size()));
    t.SetTextSize(0.019) ; t.SetTextColor(kGray + 2);
    t.DrawLatex(0.10, 0.925, "SPE fit of the spot (pads with 95 % of the charge), window 24.1-27.3 ns; white: no resolved peak");
    std::string halves;
    for (size_t h = 0; h < labels.size(); h++) {
      if (labels[h].empty()) continue;
      double lo = 1e9, hi = -1e9;
      for (auto &[b, p] : best) if (p.half == (int) h) { lo = std::min(lo, p.y) ; hi = std::max(hi, p.y) ; }
      halves += Form("%sy %.0f-%.0f mm: %s", halves.empty() ? "" : ";  ", lo, hi, labels[h].c_str());
    }
    if (!halves.empty()) t.DrawLatex(0.10, 0.903, halves.c_str());
    if (g) {
      TLine ln;
      ln.SetLineColor(kRed + 1) ; ln.SetLineWidth(2);
      for (int k = 0; k <= 8; k++) {
        double gx = gx0 + pitch * (k - 0.5), gy = gy0 + pitch * (k - 0.5);
        double ylo = std::max(y0, gy0 - pitch / 2), yhi = std::min(y1, gy0 + 7.5 * pitch);
        double xlo = std::max(x0, gx0 - pitch / 2), xhi = std::min(x1, gx0 + 7.5 * pitch);
        if (gx >= x0 && gx <= x1) ln.DrawLine(gx, tb::y_down(ylo), gx, tb::y_down(yhi));
        if (gy >= y0 && gy <= y1) ln.DrawLine(xlo, tb::y_down(gy), xhi, tb::y_down(gy));
      }
      // every pad once: red with the half whose points fill it, else grey with the first half that reads it
      // (dead input or no light)
      std::set<std::pair<int, int>> labelled;
      TLatex l;
      l.SetTextSize(0.020) ; l.SetTextAlign(22);
      for (size_t h = 0; h < slots.size(); h++)
        for (auto &[sc, rc] : map_slot) {
          if (sc.first != slots[h]) continue;
          int m = sc.second, in = slips[h] && m < 16 ? (m < 15 ? m + 1 : -1) : m;   // the input on this pad
          double x = gx0 + pitch * rc.second, y = gy0 + pitch * rc.first;
          if (x < x0 || x > x1 || y < y0 || y > y1) continue;
          bool lit = who.count(rc) && who[rc].first == (int) h;
          if (who.count(rc) && !lit) continue;
          if (!labelled.insert(rc).second) continue;
          l.SetTextColor(lit ? kRed + 2 : kGray + 1);
          l.DrawLatex(x, tb::y_down(y) + 0.30 * pitch, in < 0 ? Form("R%zu ?", h) : Form("R%zu ch%d", h, in));
        }
    }
    c->SaveAs(Form("%s%s%s.png", prefix, g ? "_pads" : "", lz ? "_log" : ""));
  }
}
