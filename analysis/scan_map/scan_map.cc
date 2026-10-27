// scan_map - detector response vs laser position from an XY scan (bin/scan_xy.sh).
//
// Stage 1, one run per scan point:
//   ./scan_map.sh
//   root [1] set_signal_window(17, 35)
//   root [2] add_scan("../../logs/scans/20261030_101500_tile66_map.csv")   // every point of the scan
//   root [3] save_points("tile66_points.root")
// Stage 2, maps and geometry (also from a saved points file, without the data):
//   root [4] make_maps()                    // or make_maps("tile66_points.root")
//   root [5] draw_channel_map(20) ; save_maps()
//   root [6] save_plots("tile66")           // tile66_overview.png, tile66_channels.png
//   root [7] save_plots("tile66_try", 32.4, 72.0)   // map grid drawn with pixel (0,0) at a trial position
//
// Stage 1 reduces every (point, channel) to one row of the "points" tree: position, events,
// occupancy (hits / events with a trigger), mean amplitude, charge and CFD time of the hits.
// The position is the stage readback stored in the run (packet 932), else the scan log's readback,
// else the scan target (911), else packet 931 (whole mm). Pixels come from the mapping + cabling in the db.
//
// Stage 2: per-channel occupancy maps over the scan grid, the best channel at every point,
// and each channel's centroid: the mean position weighted by occupancy above the channel's own
// background (its 25 % quantile over the scan: dark counts, pickup), over the points above half
// its peak. Channels that do not rise clearly above their background (flat pickup, no light) are left out.
// The centroids are then fitted with centroid = offset + pitch * (col, row), and with rows and
// columns swapped; the better one gives the stage position of pixel (0,0), the pitch and its
// sign (mirror), compared with STAGE_X0_MM / STAGE_Y0_MM / PITCH / STAGE_*_DIR in the db.
// Channels whose centroid is more than 0.4 pitch from the fit are listed: wrong mapping or cabling.

#include <iostream>
#include <pmonitor/pmonitor.h>
#include "scan_map.h"

#include "tb_db.h"
#include "tb_v1742.h"

#include <TBox.h>
#include <TCanvas.h>
#include <TFile.h>
#include <TH2.h>
#include <TLatex.h>
#include <TLine.h>
#include <TMarker.h>
#include <TMath.h>
#include <TROOT.h>
#include <TNamed.h>
#include <TStyle.h>
#include <TSystem.h>
#include <TTree.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace std;
using tb::NSIG;

int init_done = 0;

static const int PACKET = 2000;

// --- stage 1 ----------------------------------------------------------------------------------
static tb::Cuts cuts;
static int n_events_per_point = 5000;
void set_signal_window(double a, double b) { cuts.sig_from = a ; cuts.sig_to = b ; }
void set_threshold(double mv) { cuts.threshold = mv ; cuts.le_threshold = mv ; }
void set_charge_window(double a, double b) { cuts.q_from = a ; cuts.q_to = b ; }
void set_events(int n) { n_events_per_point = n ; }

static tb::V1742Event ev;
static tb::RunInfo info;
static tb::Db db;
static bool db_ok = false;
static string detector_name;

struct Point { int ch, row, col ; float x, y, occ, amp, q, t ; int run, point ; long events ; };
static vector<Point> points;

static struct Acc {
  bool active = false;
  long events = 0, valid[NSIG] = {}, hits[NSIG] = {}, timed[NSIG] = {};
  double amp[NSIG] = {}, q[NSIG] = {}, t[NSIG] = {};
} acc;

int pinit()
{
  if (init_done) return 1;
  init_done = 1;
  return 0;
}

int process_event(Event *e)
{
  if (!acc.active) return 0;
  if (e->getEvtType() == 9) {
    info.parse(e);
    ev.serial = info.serial;
    if (!db_ok || (!info.detector.empty() && info.detector != db.detector)) {
      db_ok = db.load(info.detector);
      detector_name = db_ok ? db.detector : "";
    }
    return 0;
  }
  if (!ev.fill(e)) return 0;
  acc.events++;
  for (int ch = 0; ch < NSIG; ch++) {
    tb::ChannelHit h = tb::measure(ev, ch, cuts);
    if (!h.valid) continue;
    acc.valid[ch]++;
    if (!h.hit) continue;
    acc.hits[ch]++;
    acc.amp[ch] += h.amp;
    acc.q[ch] += h.q;
    if (!std::isnan(h.t_cfd)) { acc.t[ch] += h.t_cfd ; acc.timed[ch]++ ; }
  }
  return 0;
}

static vector<string> run_parts(const string &f)
{
  vector<string> parts{f};
  size_t k = f.rfind("-0000.evt");
  if (k != string::npos && k + 9 == f.size())
    for (int i = 1; i < 10000; i++) {
      string p = f.substr(0, k) + Form("-%04d.evt", i);
      if (gSystem->AccessPathName(p.c_str())) break;
      parts.push_back(p);
    }
  return parts;
}

void add_run(const char *filename, int point, double x_mm, double y_mm)
{
  acc = Acc();
  acc.active = true;
  info = tb::RunInfo();
  for (auto &p : run_parts(filename)) {
    if (pfileopen(p.c_str()) != 0) { printf("scan_map: cannot open %s\n", p.c_str()) ; break ; }
    long left = n_events_per_point > 0 ? n_events_per_point - acc.events : 0;
    if (n_events_per_point > 0 && left <= 0) { pclose() ; break ; }
    prun(left);
    pclose();
  }
  acc.active = false;
  // position: stage readback in the run (932), else the scan log's readback, else the scan
  // target (911), else 931 (whole mm only: older runs)
  double x = info.x_whole_mm ? NAN : info.x, y = info.x_whole_mm ? NAN : info.y;
  if (std::isnan(x) && x_mm >= 0) { x = x_mm ; y = y_mm ; }
  if (std::isnan(x)) { x = info.target_x ; y = info.target_y ; }
  if (std::isnan(x)) { x = info.x ; y = info.y ; }
  if (info.point >= 0) point = info.point;
  if (std::isnan(x) || std::isnan(y)) { printf("scan_map: %s has no position - skipped\n", filename) ; return ; }
  for (int ch = 0; ch < NSIG; ch++) {
    if (!acc.valid[ch]) continue;
    Point pt;
    pt.ch = ch ; pt.row = pt.col = -1;
    if (db_ok) db.pixel(PACKET, ch, pt.row, pt.col);
    pt.x = x ; pt.y = y ; pt.run = info.run ; pt.point = point ; pt.events = acc.valid[ch];
    pt.occ = double(acc.hits[ch]) / acc.valid[ch];
    pt.amp = acc.hits[ch] ? acc.amp[ch] / acc.hits[ch] : 0;
    pt.q = acc.hits[ch] ? acc.q[ch] / acc.hits[ch] : 0;
    pt.t = acc.timed[ch] ? acc.t[ch] / acc.timed[ch] : NAN;
    points.push_back(pt);
  }
  printf("scan_map: run %d point %d at %.3f, %.3f mm, %ld events\n", info.run, point, x, y, acc.events);
}

void add_scan(const char *csv)
{
  ifstream in(csv);
  if (!in) { printf("scan_map: cannot read %s\n", csv) ; return ; }
  // run type: from the .txt next to the log ("# detector ..., DAQ profile ..., run type T")
  string txt = string(csv).substr(0, string(csv).rfind(".csv")) + ".txt", line, runtype;
  ifstream t(txt);
  smatch m;
  static const regex re("run type ([A-Za-z0-9_-]+)");
  while (getline(t, line)) if (regex_search(line, m, re)) runtype = m[1];
  if (runtype.empty()) { printf("scan_map: no run type in %s\n", txt.c_str()) ; return ; }
  string data_root = tb::read_conf(tb::repo_root() + "/db/testbench.conf")["DATA_ROOT"];
  getline(in, line);   // header: point,target_x_mm,target_y_mm,x_mm,y_mm,run,events,start,end,status
  while (getline(in, line)) {
    vector<string> f;
    stringstream ss(line);
    for (string c; getline(ss, c, ',');) f.push_back(c);
    if (f.size() < 10 || f[9] != "ok") continue;
    int run = atoi(f[5].c_str());
    string file = data_root + "/" + runtype + "/" + runtype + Form("-%08d-0000.evt", run);
    add_run(file.c_str(), atoi(f[0].c_str()), atof(f[3].c_str()), atof(f[4].c_str()));
  }
}

static void write_points(const char *filename)
{
  TFile f(filename, "RECREATE");
  TTree tr("points", "one entry per scan point and channel");
  Point p;
  tr.Branch("run", &p.run) ; tr.Branch("point", &p.point) ; tr.Branch("x", &p.x) ; tr.Branch("y", &p.y);
  tr.Branch("ch", &p.ch) ; tr.Branch("row", &p.row) ; tr.Branch("col", &p.col) ; tr.Branch("events", &p.events);
  tr.Branch("occ", &p.occ) ; tr.Branch("amp", &p.amp) ; tr.Branch("q", &p.q) ; tr.Branch("t", &p.t);
  for (auto &q : points) { p = q ; tr.Fill() ; }
  tr.Write();
  TNamed("detector", detector_name.c_str()).Write();
  f.Close();
}

void save_points(const char *filename)
{
  write_points(filename);
  printf("scan_map: wrote %zu rows to %s\n", points.size(), filename);
}

// --- stage 2 ----------------------------------------------------------------------------------
static map<int, TH2F *> occ_map;
static TH2F *h_best = nullptr, *h_best_occ = nullptr, *h_sum = nullptr;
struct Cen { int ch, row, col ; double x, y, peak, bkg ; int n ; };
static vector<Cen> cens;
static map<int, pair<int, int>> ch_pixel;                    // channel -> (row, col) from the db
static struct { bool ok = false ; double x0, y0, pitch, dx, dy ; string name ; } geo;   // db placement

static vector<double> edges(set<double> v)   // bin edges halfway between the scan positions
{
  vector<double> c(v.begin(), v.end()), e;
  if (c.size() == 1) return {c[0] - 0.5, c[0] + 0.5};
  e.push_back(c[0] - 0.5 * (c[1] - c[0]));
  for (size_t i = 0; i + 1 < c.size(); i++) e.push_back(0.5 * (c[i] + c[i + 1]));
  e.push_back(c.back() + 0.5 * (c.back() - c[c.size() - 2]));
  return e;
}

struct LinFit { double a = NAN, b = NAN, rms = NAN ; };
static LinFit linfit(const vector<double> &u, const vector<double> &v)   // v = a + b u
{
  LinFit r;
  size_t n = u.size();
  if (n < 2) return r;
  double su = 0, sv = 0, suu = 0, suv = 0;
  for (size_t i = 0; i < n; i++) { su += u[i] ; sv += v[i] ; suu += u[i] * u[i] ; suv += u[i] * v[i] ; }
  double d = n * suu - su * su;
  if (fabs(d) < 1e-12) return r;
  r.b = (n * suv - su * sv) / d;
  r.a = (sv - r.b * su) / n;
  double s2 = 0;
  for (size_t i = 0; i < n; i++) s2 += pow(v[i] - r.a - r.b * u[i], 2);
  r.rms = sqrt(s2 / n);
  return r;
}

static bool load_points(const char *file)
{
  if (!file || !*file) return !points.empty();
  TFile f(file);
  TTree *tr = f.IsOpen() ? (TTree *)f.Get("points") : nullptr;
  if (!tr) { printf("scan_map: no points tree in %s\n", file) ; return false ; }
  if (auto *d = (TNamed *)f.Get("detector")) detector_name = d->GetTitle();
  points.clear();
  Point p;
  tr->SetBranchAddress("run", &p.run) ; tr->SetBranchAddress("point", &p.point) ; tr->SetBranchAddress("x", &p.x);
  tr->SetBranchAddress("y", &p.y) ; tr->SetBranchAddress("ch", &p.ch) ; tr->SetBranchAddress("row", &p.row);
  tr->SetBranchAddress("col", &p.col) ; tr->SetBranchAddress("events", &p.events) ; tr->SetBranchAddress("occ", &p.occ);
  tr->SetBranchAddress("amp", &p.amp) ; tr->SetBranchAddress("q", &p.q) ; tr->SetBranchAddress("t", &p.t);
  for (Long64_t i = 0; i < tr->GetEntries(); i++) { tr->GetEntry(i) ; points.push_back(p) ; }
  return true;
}

void make_maps(const char *points_file, const char *detector)
{
  if (!load_points(points_file) || points.empty()) { printf("scan_map: no points (add_scan / add_run, or give a points file)\n") ; return ; }
  string det = (detector && *detector) ? detector : detector_name;
  tb::Db d;
  bool have_db = d.load(det);

  set<double> xs, ys;
  for (auto &p : points) { xs.insert(round(p.x * 1000) / 1000) ; ys.insert(round(p.y * 1000) / 1000) ; }
  vector<double> ex = edges(xs), ey = edges(ys);
  for (auto &m : occ_map) delete m.second;
  occ_map.clear();
  delete h_best ; delete h_best_occ ; delete h_sum;
  h_best = new TH2F("best_channel", "lit channel with the highest occupancy above its background;x [mm];y [mm];channel", ex.size() - 1, ex.data(), ey.size() - 1, ey.data());
  h_best_occ = new TH2F("best_occupancy", "highest occupancy above background of any lit channel;x [mm];y [mm];occupancy - background", ex.size() - 1, ex.data(), ey.size() - 1, ey.data());
  h_sum = new TH2F("sum_occupancy", "sum of the occupancies of all channels;x [mm];y [mm];sum", ex.size() - 1, ex.data(), ey.size() - 1, ey.data());
  for (auto &p : points) {
    TH2F *&h = occ_map[p.ch];
    if (!h) h = new TH2F(Form("occ_ch%02d", p.ch), Form("ch %d occupancy;x [mm];y [mm];occupancy", p.ch), ex.size() - 1, ex.data(), ey.size() - 1, ey.data());
    h->Fill(p.x, p.y, p.occ);
    h_sum->Fill(p.x, p.y, p.occ);
  }

  // centroids
  cens.clear();
  ch_pixel.clear();
  for (auto &p : points) if (p.row >= 0) ch_pixel[p.ch] = {p.row, p.col};
  geo.ok = have_db && !std::isnan(d.num("STAGE_X0_MM", NAN)) && !std::isnan(d.num("STAGE_Y0_MM", NAN)) && !std::isnan(d.num("PITCH_X_MM", NAN));
  if (geo.ok) geo = {true, d.num("STAGE_X0_MM", NAN), d.num("STAGE_Y0_MM", NAN), d.num("PITCH_X_MM", NAN),
                     d.num("STAGE_X_DIR", 1), d.num("STAGE_Y_DIR", 1), d.detector};
  vector<int> flat;
  for (auto &m : occ_map) {
    int ch = m.first;
    vector<double> occ;
    int row = -1, col = -1;
    for (auto &p : points) if (p.ch == ch) { occ.push_back(p.occ) ; row = p.row ; col = p.col ; }
    sort(occ.begin(), occ.end());
    double bkg = occ[occ.size() / 4], peak = occ.back(), sig = peak - bkg;
    if (peak < 3 * bkg) { flat.push_back(ch) ; continue ; }     // pickup / noise everywhere
    if (sig < 0.01) continue;                                   // no light seen
    double sw = 0, sx = 0, sy = 0;
    int n = 0;
    for (auto &p : points)
      if (p.ch == ch && p.occ - bkg >= 0.5 * sig) { double w = p.occ - bkg ; sw += w ; sx += w * p.x ; sy += w * p.y ; n++ ; }
    cens.push_back({ch, row, col, sx / sw, sy / sw, peak, bkg, n});
  }
  for (auto &c : cens)
    for (auto &p : points)
      if (p.ch == c.ch) {
        int b = h_best_occ->FindBin(p.x, p.y);
        if (p.occ - c.bkg > h_best_occ->GetBinContent(b)) { h_best_occ->SetBinContent(b, p.occ - c.bkg) ; h_best->SetBinContent(b, p.ch) ; }
      }
  printf("scan_map: %zu points rows, %zu x %zu positions, detector %s\n", points.size(), xs.size(), ys.size(), have_db ? d.detector.c_str() : "unknown");
  printf("  ch   pixel     centroid x, y [mm]   peak occ  background  points\n");
  for (auto &c : cens)
    printf("  %2d  %3d,%-3d   %8.3f %8.3f       %6.3f      %6.3f    %3d\n", c.ch, c.row, c.col, c.x, c.y, c.peak, c.bkg, c.n);
  if (!flat.empty()) {
    printf("  left out, hits everywhere at about the same rate (pickup or noise):");
    for (int ch : flat) printf(" %d", ch);
    printf("\n");
  }

  // geometry fit: centroid = a + b * (col, row), or rows and columns swapped
  vector<double> cx, cy, cr, cc;
  for (auto &c : cens) if (c.row >= 0 && c.n >= 2) { cx.push_back(c.x) ; cy.push_back(c.y) ; cr.push_back(c.row) ; cc.push_back(c.col) ; }
  set<double> ucols(cc.begin(), cc.end()), urows(cr.begin(), cr.end());
  if (cx.size() < 3 || ucols.size() < 2 || urows.size() < 2) {
    printf("geometry: needs >= 3 mapped channels spanning 2 rows and 2 columns (%zu mapped: fill in cabling.txt?)\n", cx.size());
    return;
  }
  LinFit fx = linfit(cc, cx), fy = linfit(cr, cy);     // normal
  LinFit sx = linfit(cr, cx), sy = linfit(cc, cy);     // rows <-> columns
  bool swapped = hypot(sx.rms, sy.rms) < hypot(fx.rms, fy.rms);
  if (swapped) { fx = sx ; fy = sy ; }
  // refit without channels more than 0.4 pitch off, so a few mis-cabled channels don't pull it
  {
    double st = 0.5 * (fabs(fx.b) + fabs(fy.b));
    vector<double> kx, ky, kr, kc;
    for (size_t i = 0; i < cx.size(); i++) {
      double ux = swapped ? cr[i] : cc[i], uy = swapped ? cc[i] : cr[i];
      if (hypot(cx[i] - fx.a - fx.b * ux, cy[i] - fy.a - fy.b * uy) <= 0.4 * st) { kx.push_back(cx[i]) ; ky.push_back(cy[i]) ; kr.push_back(cr[i]) ; kc.push_back(cc[i]) ; }
    }
    if (kx.size() >= 3 && kx.size() < cx.size()) {
      fx = swapped ? linfit(kr, kx) : linfit(kc, kx);
      fy = swapped ? linfit(kc, ky) : linfit(kr, ky);
      printf("(refitted without %zu channel(s) more than 0.4 pitch off)\n", cx.size() - kx.size());
    }
  }
  double pitch = have_db ? d.num("PITCH_X_MM", NAN) : NAN;
  printf("geometry fit (%zu channels)%s:\n", cx.size(), swapped ? " - ROWS AND COLUMNS SWAPPED relative to the map" : "");
  printf("  x = %.3f %+.4f * %s   (rms %.3f mm)\n", fx.a, fx.b, swapped ? "row" : "col", fx.rms);
  printf("  y = %.3f %+.4f * %s   (rms %.3f mm)\n", fy.a, fy.b, swapped ? "col" : "row", fy.rms);
  printf("  pixel (0,0) at %.3f, %.3f mm; pitch %.3f x %.3f mm\n", fx.a, fy.a, fabs(fx.b), fabs(fy.b));
  if (have_db) {
    double x0 = d.num("STAGE_X0_MM", NAN), y0 = d.num("STAGE_Y0_MM", NAN);
    double dx = d.num("STAGE_X_DIR", 1), dy = d.num("STAGE_Y_DIR", 1);
    printf("  db (%s): pixel (0,0) at %.3f, %.3f mm, pitch %.3f, directions %+.0f %+.0f\n", d.detector.c_str(), x0, y0, pitch, dx, dy);
    if (fx.b * dx < 0) printf("  -> x runs the other way: the map is MIRRORED left-right relative to the stage (or STAGE_X_DIR is wrong)\n");
    if (fy.b * dy < 0) printf("  -> y runs the other way: the map is MIRRORED top-bottom relative to the stage (or STAGE_Y_DIR is wrong)\n");
    if (!std::isnan(pitch) && (fabs(fabs(fx.b) - pitch) > 0.05 * pitch || fabs(fabs(fy.b) - pitch) > 0.05 * pitch))
      printf("  -> fitted pitch differs from PITCH_X_MM by more than 5 %%\n");
  }
  double step = 0.5 * (fabs(fx.b) + fabs(fy.b));
  int bad = 0;
  for (auto &c : cens) {
    if (c.row < 0) continue;
    double ux = swapped ? c.row : c.col, uy = swapped ? c.col : c.row;
    double r = hypot(c.x - fx.a - fx.b * ux, c.y - fy.a - fy.b * uy);
    if (r > 0.4 * step) { printf("  ch %d (pixel %d,%d) is %.2f mm from where the map puts it: wrong mapping or cabling?\n", c.ch, c.row, c.col, r) ; bad++ ; }
  }
  if (!bad) printf("  every mapped channel is within 0.4 pitch of the fit\n");

  auto *cv = new TCanvas("c_scan_maps", "scan maps", 1300, 600);
  cv->Divide(2, 1);
  gStyle->SetOptStat(0);
  cv->cd(1) ; h_best->Draw("colz text");
  cv->cd(2) ; h_sum->Draw("colz");
}

void draw_channel_map(int ch)
{
  if (!occ_map.count(ch)) { printf("scan_map: no map for ch %d (make_maps first)\n", ch) ; return ; }
  auto *c = new TCanvas(Form("c_occ_ch%02d", ch), Form("ch %d", ch), 700, 600);
  c->SetRightMargin(0.15);
  occ_map[ch]->Draw("colz");
}

// the pixels of the db map that overlap the plotted range, as outlines labelled with their channel;
// only == -1: all of them, else only that channel's pixel
static void draw_pixel_grid(TH2 *h, double x0, double y0, int only, int color)
{
  double xl = h->GetXaxis()->GetXmin(), xh = h->GetXaxis()->GetXmax();
  double yl = h->GetYaxis()->GetXmin(), yh = h->GetYaxis()->GetXmax();
  double hp = 0.5 * geo.pitch;
  for (auto &e : ch_pixel) {
    if (only >= 0 && e.first != only) continue;
    double cx = x0 + geo.dx * e.second.second * geo.pitch, cy = y0 + geo.dy * e.second.first * geo.pitch;
    double bx0 = max(xl, cx - hp), bx1 = min(xh, cx + hp), by0 = max(yl, cy - hp), by1 = min(yh, cy + hp);
    if (bx0 >= bx1 || by0 >= by1) continue;
    auto *b = new TBox(bx0, by0, bx1, by1);
    b->SetFillStyle(0) ; b->SetLineColor(color) ; b->SetLineStyle(2) ; b->SetLineWidth(2) ; b->Draw();
    if (only < 0) {
      auto *t = new TLatex(bx0 + 0.04 * geo.pitch, by1 - 0.04 * geo.pitch, Form("map %d", e.first));
      t->SetTextAlign(13) ; t->SetTextSize(0.028) ; t->SetTextColor(color) ; t->Draw();
    }
  }
}

void save_plots(const char *prefix, double x0, double y0)
{
  if (!h_best) { printf("scan_map: make_maps first\n") ; return ; }
  bool grid = geo.ok;
  if (std::isnan(x0)) x0 = geo.x0;
  if (std::isnan(y0)) y0 = geo.y0;
  string where = grid ? Form("pixel (0,0) at %.2f, %.2f mm%s", x0, y0, (x0 == geo.x0 && y0 == geo.y0) ? " (db)" : "") : "";
  gStyle->SetOptStat(0);
  gStyle->SetPaintTextFormat(".0f");

  // overview: best channel over the scan, and centroids against the map
  auto *cv = new TCanvas("c_scan_overview", "scan overview", 1500, 680);
  cv->Divide(2, 1);
  cv->cd(1);
  gPad->SetRightMargin(0.16);
  h_best_occ->SetTitle("brightest channel (number), occupancy above background (colour);x [mm];y [mm];occupancy - background");
  h_best_occ->SetMinimum(-1e-9);   // zero bins painted too
  h_best_occ->GetZaxis()->SetTitleOffset(1.5);
  h_best_occ->Draw("colz");
  for (int i = 1; i <= h_best->GetNbinsX(); i++)
    for (int j = 1; j <= h_best->GetNbinsY(); j++) {
      int ch = (int)h_best->GetBinContent(i, j);
      bool lit = false;
      for (auto &c : cens) if (c.ch == ch && h_best_occ->GetBinContent(i, j) >= 0.5 * (c.peak - c.bkg)) lit = true;
      if (!lit) continue;
      auto *t = new TLatex(h_best->GetXaxis()->GetBinCenter(i), h_best->GetYaxis()->GetBinCenter(j), Form("%d", ch));
      t->SetTextAlign(22) ; t->SetTextSize(0.04) ; t->SetTextColor(kWhite) ; t->Draw();
    }
  if (grid) draw_pixel_grid(h_best_occ, x0, y0, -1, kRed);

  cv->cd(2);
  double fx0 = h_best_occ->GetXaxis()->GetXmin(), fx1 = h_best_occ->GetXaxis()->GetXmax();
  double fy0 = h_best_occ->GetYaxis()->GetXmin(), fy1 = h_best_occ->GetYaxis()->GetXmax();
  for (auto &c : cens)
    if (grid) {
      double ex = x0 + geo.dx * c.col * geo.pitch, ey = y0 + geo.dy * c.row * geo.pitch;
      fx0 = min(fx0, ex - 1) ; fx1 = max(fx1, ex + 1) ; fy0 = min(fy0, ey - 1) ; fy1 = max(fy1, ey + 1);
    }
  delete gROOT->FindObject("frame");
  auto *fr = new TH2F("frame", "", 10, fx0, fx1, 10, fy0, fy1);
  auto *scanned = new TBox(h_best_occ->GetXaxis()->GetXmin(), h_best_occ->GetYaxis()->GetXmin(),
                           h_best_occ->GetXaxis()->GetXmax(), h_best_occ->GetYaxis()->GetXmax());
  fr->SetTitle(Form("centroid (filled) and where the map puts the channel (open), hatched = scanned%s;x [mm];y [mm]", grid ? "" : ", no db position"));
  fr->Draw("axis");
  scanned->SetFillColor(kGray) ; scanned->SetFillStyle(3003) ; scanned->Draw();
  if (grid) draw_pixel_grid(fr, x0, y0, -1, kGray + 1);
  for (auto &c : cens) {
    auto *m = new TMarker(c.x, c.y, 20) ; m->SetMarkerColor(kBlue + 1) ; m->SetMarkerSize(1.4) ; m->Draw();
    auto *t = new TLatex(c.x, c.y + 0.35, Form("%d", c.ch)) ; t->SetTextAlign(21) ; t->SetTextSize(0.035) ; t->SetTextColor(kBlue + 1) ; t->Draw();
    if (grid && ch_pixel.count(c.ch)) {
      double ex = x0 + geo.dx * c.col * geo.pitch, ey = y0 + geo.dy * c.row * geo.pitch;
      auto *l = new TLine(c.x, c.y, ex, ey) ; l->SetLineColor(kRed) ; l->Draw();
      auto *o = new TMarker(ex, ey, 24) ; o->SetMarkerColor(kRed) ; o->SetMarkerSize(1.4) ; o->Draw();
    }
  }
  if (grid) {
    auto *t = new TLatex(0.12, 0.92, where.c_str()) ; t->SetNDC() ; t->SetTextSize(0.03) ; t->Draw();
  }
  cv->SaveAs(Form("%s_overview.png", prefix));

  // one occupancy map per lit channel, with the pixel the map gives it
  if (!cens.empty()) {
    int n = cens.size(), nx = (int)ceil(sqrt(n)), ny = (n + nx - 1) / nx;
    auto *cc = new TCanvas("c_scan_channels", "channels", 420 * nx, 380 * ny);
    cc->Divide(nx, ny);
    for (int i = 0; i < n; i++) {
      cc->cd(i + 1);
      gPad->SetRightMargin(0.18);
      TH2F *h = occ_map[cens[i].ch];
      h->SetTitle(Form("ch %d (map pixel %d,%d);x [mm];y [mm];occupancy", cens[i].ch, cens[i].row, cens[i].col));
      h->SetMinimum(-1e-9);
      h->GetZaxis()->SetTitleOffset(1.6);
      h->Draw("colz");
      if (grid) draw_pixel_grid(h, x0, y0, cens[i].ch, kRed);
    }
    cc->SaveAs(Form("%s_channels.png", prefix));
  }
  printf("scan_map: plots %s_overview.png, %s_channels.png%s%s\n", prefix, prefix, grid ? ", map grid with " : "", where.c_str());
}

void save_maps(const char *filename)
{
  if (!h_best) { printf("scan_map: make_maps first\n") ; return ; }
  TFile f(filename, "RECREATE");
  h_best->Write() ; h_best_occ->Write() ; h_sum->Write();
  for (auto &m : occ_map) m.second->Write();
  f.Close();
  printf("scan_map: wrote %s\n", filename);
}

// --- synthetic scan -------------------------------------------------------------------------------
void simulate_scan(const char *filename, const char *detector, int slot, double x0, double y0,
                   bool mirror_x, double step, double sigma, double mu)
{
  tb::Db d;
  if (!d.load(detector)) { printf("scan_map: no detector %s in the db\n", detector) ; return ; }
  double pitch = d.num("PITCH_X_MM", NAN);
  if (std::isnan(pitch)) { printf("scan_map: %s has no PITCH_X_MM\n", detector) ; return ; }
  struct Px { int ch, row, col ; };
  vector<Px> px;
  for (auto &e : d.map_slot) if (e.first.first == slot) px.push_back({e.first.second, e.second.first, e.second.second});
  if (px.empty()) { printf("scan_map: no slot %d in %s's mapping\n", slot, detector) ; return ; }
  double sx = mirror_x ? -1 : 1;
  double xlo = 1e9, xhi = -1e9, ylo = 1e9, yhi = -1e9;
  for (auto &p : px) {
    double x = x0 + sx * p.col * pitch, y = y0 + p.row * pitch;
    xlo = min(xlo, x) ; xhi = max(xhi, x) ; ylo = min(ylo, y) ; yhi = max(yhi, y);
  }
  auto frac = [&](double c, double l) {   // spot fraction within [l - pitch/2, l + pitch/2]
    return 0.5 * (TMath::Erf((l + pitch / 2 - c) / (sqrt(2.0) * sigma)) - TMath::Erf((l - pitch / 2 - c) / (sqrt(2.0) * sigma)));
  };
  points.clear();
  detector_name = detector;
  int point = 0;
  for (double x = xlo - pitch; x <= xhi + pitch + 1e-9; x += step)
    for (double y = ylo - pitch; y <= yhi + pitch + 1e-9; y += step, point++)
      for (auto &p : px) {
        double f = frac(x, x0 + sx * p.col * pitch) * frac(y, y0 + p.row * pitch);
        points.push_back({p.ch, p.row, p.col, (float)x, (float)y, (float)(1 - exp(-mu * f)), 20.f, 0.2f, 25.f, 1000 + point, point, 5000});
      }
  write_points(filename);
  printf("scan_map: simulated %s slot %d (%zu pixels), pixel (0,0) at %.3f, %.3f mm%s, %d points -> %s\n", detector, slot, px.size(),
         x0, y0, mirror_x ? ", mirrored in x" : "", point, filename);
}
