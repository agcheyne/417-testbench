// Shared by time_xy_plots.C and peak_time_map.C: the scan's points, the time_xy_point parts, the
// channel list ("pad:CH" = CH and its 8 neighbours, from the pixel of each input stored in the parts),
// the laser peak and per-channel alignment, time slices relative to the peak, and the pad grid.
#ifndef TIME_XY_COMMON_H
#define TIME_XY_COMMON_H
#include <TFile.h>
#include <TH1D.h>
#include <TH1I.h>
#include <TH2F.h>
#include <TLine.h>
#include <TSystem.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace txy {

inline std::vector<std::string> split(const std::string &s, char sep)
{
  std::vector<std::string> v; std::stringstream ss(s); std::string x;
  while (std::getline(ss, x, sep)) if (!x.empty()) v.push_back(x);
  return v;
}

// scan_xy.sh's <scan>.csv: point -> stage x, y (ok points only), and the scan's steps
struct Scan {
  std::map<int, std::pair<double, double>> pos;
  std::set<double> xs, ys;
  double dx = 1, dy = 1, xlo = 0, xhi = 0, ylo = 0, yhi = 0;
  int nx = 0, ny = 0;
  bool read(const char *csv)
  {
    std::ifstream in(csv); std::string line;
    if (!std::getline(in, line)) return false;
    while (std::getline(in, line)) {
      auto f = split(line, ',');
      if (f.size() < 10 || f[9] != "ok") continue;
      double px = std::round(atof(f[3].c_str()) * 100) / 100, py = std::round(atof(f[4].c_str()) * 100) / 100;
      pos[atoi(f[0].c_str())] = {px, py};
      xs.insert(px) ; ys.insert(py);
    }
    if (pos.empty()) return false;
    dx = xs.size() > 1 ? *std::next(xs.begin()) - *xs.begin() : 1;   // an x line: dy = 1 mm
    dy = ys.size() > 1 ? *std::next(ys.begin()) - *ys.begin() : 1;
    xlo = *xs.begin() - dx / 2 ; xhi = *xs.rbegin() + dx / 2 ; ylo = *ys.begin() - dy / 2 ; yhi = *ys.rbegin() + dy / 2;
    nx = std::lround((xhi - xlo) / dx) ; ny = std::lround((yhi - ylo) / dy);
    return true;
  }
};

// stage position of pixel (0,0)'s centre, pitch, size and directions:
// "x0,y0,pitch_x,pitch_y,ncols,nrows,x_dir,y_dir" (time_xy.sh, from the db's detector.conf); "" = no pad grid
struct Geo {
  double x0 = NAN, y0 = NAN, px = NAN, py = NAN, xdir = 1, ydir = 1;
  int ncols = 0, nrows = 0;
  Geo(const char *s = "")
  {
    auto f = split(s, ',');
    if (f.size() >= 6) { x0 = atof(f[0].c_str()) ; y0 = atof(f[1].c_str()) ; px = atof(f[2].c_str()) ; py = atof(f[3].c_str()) ;
                         ncols = atoi(f[4].c_str()) ; nrows = atoi(f[5].c_str()) ; }
    if (f.size() >= 8) { xdir = atof(f[6].c_str()) ; ydir = atof(f[7].c_str()) ; }
  }
  bool ok() const { return !std::isnan(x0) && px > 0 && ncols > 0; }
  double x(int col) const { return x0 + xdir * px * col; }   // stage position of a pixel's centre
  double y(int row) const { return y0 + ydir * py * row; }
  // dashed pad edges inside the frame (x only for an x line: y0 == y1)
  void draw(double xa, double xb, double ya, double yb, bool vertical_only = false) const
  {
    if (!ok()) return;
    TLine l; l.SetLineColor(kGray + 1); l.SetLineStyle(2); l.SetLineWidth(1);
    for (int k = -ncols - 1; k <= ncols; k++) { double x = x0 + px * (k + 0.5) ; if (x > xa && x < xb) l.DrawLine(x, ya, x, yb) ; }
    if (vertical_only) return;
    for (int k = -nrows - 1; k <= nrows; k++) { double y = y0 + py * (k + 0.5) ; if (y > ya && y < yb) l.DrawLine(xa, y, xb, y) ; }
  }
};

inline TFile *open_part(const char *parts, int point)
{
  TString fn = Form("%s/p%d.root", parts, point);
  if (gSystem->AccessPathName(fn)) return nullptr;   // not done (yet)
  TFile *f = TFile::Open(fn);
  if (!f || f->IsZombie()) { delete f ; return nullptr ; }
  return f;
}

// an input's pixel, from the first part that has it (false: none)
inline bool pixel_of(const char *parts, const Scan &s, int ch, int &row, int &col)
{
  for (auto &p : s.pos) {
    TFile *f = open_part(parts, p.first);
    if (!f) continue;
    TH1 *r = (TH1 *)f->Get("row"), *c = (TH1 *)f->Get("col");
    bool ok = r && c && r->GetBinContent(ch + 1) >= 0;
    if (ok) { row = r->GetBinContent(ch + 1) ; col = c->GetBinContent(ch + 1) ; }
    f->Close() ; delete f;
    return ok;
  }
  return false;
}

// "20,19,23" as listed, or "pad:20": 20 first, then the inputs on its 8 neighbouring pixels (from the first part's
// row/col, i.e. the run's own cabling); masked inputs left out
inline std::vector<int> channels(const std::string &spec, const char *parts, const Scan &s, const std::set<int> &masked)
{
  std::vector<int> v;
  if (spec.rfind("pad:", 0) != 0) {
    for (auto &c : split(spec, ',')) v.push_back(atoi(c.c_str()));
    return v;
  }
  int c0 = atoi(spec.c_str() + 4);
  v.push_back(c0);
  for (auto &p : s.pos) {
    TFile *f = open_part(parts, p.first);
    if (!f) continue;
    TH1 *r = (TH1 *)f->Get("row"), *c = (TH1 *)f->Get("col");
    if (r && c && r->GetBinContent(c0 + 1) >= 0) {
      int r0 = r->GetBinContent(c0 + 1), k0 = c->GetBinContent(c0 + 1);
      for (int ch = 0; ch < 32; ch++) {
        int dr = r->GetBinContent(ch + 1) - r0, dk = c->GetBinContent(ch + 1) - k0;
        if (ch != c0 && r->GetBinContent(ch + 1) >= 0 && std::abs(dr) <= 1 && std::abs(dk) <= 1 && !masked.count(ch)) v.push_back(ch);
      }
    } else fprintf(stderr, "no pixel for input %d in %s/p%d.root (no cabling in the run?): channel %d alone\n", c0, parts, p.first, c0);
    f->Close() ; delete f;
    break;
  }
  return v;
}

// laser peak of each channel (hits summed over all points, 50 ps bins) and the shift [bins] that puts it on the first
// channel's: the db offsets leave ~0.1 ns between inputs (Photonis, 3 Nov 2026), more after a cable change until
// time_offsets/ is run again
struct Peaks {
  double ref = NAN;                     // the first channel's peak [ns], on the 50 ps grid
  std::map<int, int> shift;
  std::set<int> drop;                   // inputs without a laser peak (dead, unlit)
  void find(const char *parts, const Scan &s, const std::vector<int> &chs, bool align)
  {
    std::map<int, TH1D *> sum;
    for (auto &p : s.pos) {
      TFile *f = open_part(parts, p.first);
      if (!f) continue;
      if (TH2F *tc = (TH2F *)f->Get("tc"))
        for (int ch : chs) {
          TH1D *py = tc->ProjectionY("py", ch + 1, ch + 1);
          if (!sum.count(ch)) { sum[ch] = (TH1D *)py->Clone(Form("peak_sum%d", ch)) ; sum[ch]->SetDirectory(nullptr) ; }
          else sum[ch]->Add(py);
          delete py;
        }
      f->Close() ; delete f;
    }
    if (!sum.count(chs[0])) return;
    TH1D *r = sum[chs[0]];
    int rb = r->GetMaximumBin();
    ref = r->GetBinLowEdge(rb);
    for (auto &[ch, h] : sum) {
      // a dead or unlit input has no laser peak: its highest bin under 20x its mean (lit pads: > 100x)
      double ratio = h->GetBinContent(h->GetMaximumBin()) / std::max(h->Integral() / h->GetNbinsX(), 1e-9);
      int sh = rb - h->GetMaximumBin();
      if (ch != chs[0] && (ratio < 20 || std::abs(sh) * h->GetBinWidth(1) > 5)) {
        printf("ch %2d: no laser peak (highest bin %.0fx the mean, at %.2f ns): left out\n", ch, ratio, h->GetBinCenter(h->GetMaximumBin()));
        drop.insert(ch);
        continue;
      }
      shift[ch] = align ? sh : 0;
      printf("ch %2d: laser peak %.2f ns%s\n", ch, h->GetBinCenter(h->GetMaximumBin()),
             align ? Form(", shifted by %+.2f ns", shift[ch] * h->GetBinWidth(1)) : "");
    }
    for (auto &[ch, h] : sum) delete h;
  }
};

// time slices: "A:B" a window, "A:B:STEP" A to B in steps; a value "p+X" / "p-X" / "p" is relative to the laser peak.
// Clipped to the 10-45 ns of the parts.
inline std::vector<std::pair<double, double>> slices(const std::string &spec, double peak)
{
  auto val = [&](std::string t) { return t[0] == 'p' ? peak + (t.size() > 1 ? atof(t.c_str() + 1) : 0) : atof(t.c_str()); };
  std::vector<std::pair<double, double>> w;
  for (auto &tok : split(spec, ',')) {
    auto f = split(tok, ':');
    if (f.size() < 2) continue;
    double a = std::max(10.0, val(f[0])), b = std::min(45.0, val(f[1]));
    if (f.size() == 2) { if (b > a) w.push_back({a, b}) ; continue ; }
    double st = atof(f[2].c_str());
    int n = std::lround((b - a) / st);
    for (int i = 0; i < n; i++) w.push_back({a + i * st, a + (i + 1) * st});
  }
  return w;
}

inline std::set<int> int_set(const char *s)
{
  std::set<int> m;
  for (auto &c : split(s, ' ')) m.insert(atoi(c.c_str()));
  return m;
}

} // namespace txy
#endif
