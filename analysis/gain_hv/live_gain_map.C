// Live view of a gain map while fit_gain_map.sh --follow works through a scan.
// Left: the spot gain (or mu) of every point fitted so far, on the scan's grid, stage y down as the bench
// face (grey: taken, not yet fitted; red ring: the latest fit). Right: the latest point's spot charge spectrum with its
// single-photoelectron fit (tb::fit_spe, as gain_hv, redone on the stored spectrum: the chunk files
// keep only the parameters). Points with no spot fit stay blank. Redraws every `every` s and writes
// <scan>_live.png in the scan's folder each time, so it can be looked at remotely. See also live_hv_curve.C.
//   root -l 'analysis/gain_hv/live_gain_map.C("<scan>")'               window, refreshed by a timer
//   root -l 'analysis/gain_hv/live_gain_map.C("<scan>", "mu", 20)'     the spot mu, every 20 s
//   root -b -q 'analysis/gain_hv/live_gain_map.C("<scan>", "gain", 20)'  no window: the png only, until the
//                                                                        scan has ended and every point is fitted
//   root -b -q 'analysis/gain_hv/live_gain_map.C("<scan>", "gain", 0)'   the png once
// <scan>: the scan's folder (logs/scans/<scan>/) or just <scan>. Reads only.
#include <set>
#include "TH2D.h"
#include "TBox.h"
#include "TMarker.h"
#include "TStyle.h"
#include "TTimer.h"
#include "TROOT.h"
#include "live_common.h"
#include "../../lib/tb_ydown.h"

namespace live {

std::string dir, name, zvar = "gain";
TCanvas *c = nullptr;
TH2D *hmap = nullptr;
TH1 *hspec = nullptr;
TF1 *ffit = nullptr;
std::vector<TObject *> trash;     // boxes, markers, texts of the last drawing
bool finished = false;            // the scan has ended and every point taken is fitted

void update()
{
  for (auto *o : trash) delete o;
  trash.clear();
  // every point taken: run -> row of the scan csv
  auto pts = read_csv(dir + "/" + name + ".csv");
  std::map<long, Row> run2pt;
  std::set<double> xs, ys;
  for (auto &p : pts) {
    xs.insert(std::round(num(p, "target_x_mm") * 1000) / 1000);
    ys.insert(std::round(num(p, "target_y_mm") * 1000) / 1000);
    if (p["status"] == "ok") run2pt[std::stol(p["run"])] = p;
  }
  // the spot fits of the chunks done so far, oldest chunk first
  std::string parts = dir + "/" + name + "_gain_map_parts";
  std::vector<std::pair<long, std::string>> chunks;
  if (void *d = gSystem->OpenDirectory(parts.c_str())) {
    while (const char *f = gSystem->GetDirEntry(d)) {
      std::string s = f;
      if (s.size() == 13 && s.compare(0, 5, "chunk") == 0 && s.compare(9, 4, ".csv") == 0) {
        FileStat_t st;
        gSystem->GetPathInfo((parts + "/" + s).c_str(), st);
        chunks.push_back({st.fMtime, parts + "/" + s});
      }
    }
    gSystem->FreeDirectory(d);
  }
  std::sort(chunks.begin(), chunks.end());
  std::map<long, Row> fits;
  std::set<long> done;            // runs fitted (any row): without a spot row they had no spot fit
  std::string newest_chunk;
  long newest_run = -1;
  for (auto &ch : chunks)
    for (auto &r : read_csv(ch.second)) {
      long run = std::stol(r["run"]);
      done.insert(run);
      if (r["ch"] == "spot" && run2pt.count(run)) { fits[run] = r ; newest_chunk = ch.second ; newest_run = run ; }
    }
  bool ended = scan_ended(dir, name, !run2pt.empty());

  c->cd(1);
  gPad->Clear();
  delete hmap ; hmap = nullptr;
  if (xs.empty()) { c->Update() ; return ; }
  std::vector<double> vx(xs.begin(), xs.end()), vy(ys.begin(), ys.end());
  double dx = vx.size() > 1 ? vx[1] - vx[0] : 1, dy = vy.size() > 1 ? vy[1] - vy[0] : 1;
  hmap = new TH2D("live_map", "", vx.size(), vx.front() - dx / 2, vx.back() + dx / 2, vy.size(), vy.front() - dy / 2, vy.back() + dy / 2);
  hmap->SetDirectory(nullptr);
  std::vector<double> good;
  std::vector<std::pair<double, double>> waiting;
  for (auto &rp : run2pt) {
    double x = num(rp.second, "target_x_mm"), y = num(rp.second, "target_y_mm");
    if (!done.count(rp.first)) { waiting.push_back({x, y}) ; continue ; }
    auto f = fits.find(rp.first);
    if (f == fits.end()) continue;
    if (!usable(f->second)) continue;
    double z = num(f->second, zvar.c_str());
    hmap->Fill(x, y, z);
    good.push_back(z);
  }
  int nusable = good.size();
  finished = ended && waiting.empty();
  if (!good.empty()) {
    std::sort(good.begin(), good.end());
    hmap->SetMinimum(good[size_t(0.02 * (good.size() - 1))]);
    hmap->SetMaximum(good[size_t(0.98 * (good.size() - 1))]);
  }
  hmap->SetTitle(Form("spot %s: %zu of %zu points fitted (%d usable), scan %s, %s;stage x [mm];stage y [mm]",
                      zvar.c_str(), done.size(), run2pt.size(), nusable, ended ? "ended" : "running", now_hms().c_str()));
  hmap->SetStats(false);
  tb::draw_y_down(hmap, "COLZ");
  for (auto &w : waiting) {
    auto *b = new TBox(w.first - dx / 2, tb::y_down(w.second) - dy / 2, w.first + dx / 2, tb::y_down(w.second) + dy / 2);
    b->SetFillColor(kGray);
    b->Draw();
    trash.push_back(b);
  }
  if (newest_run >= 0) {
    auto &p = run2pt[newest_run];
    auto *m = new TMarker(num(p, "x_mm"), tb::y_down(num(p, "y_mm")), 24);
    m->SetMarkerColor(kRed) ; m->SetMarkerSize(2.5);
    m->Draw();
    trash.push_back(m);
  }

  c->cd(2);
  gPad->Clear();
  if (newest_run < 0) { c->Update() ; return ; }
  auto &p = run2pt[newest_run];
  auto &r = fits[newest_run];
  std::string rootf = newest_chunk.substr(0, newest_chunk.size() - 4) + ".root";
  draw_spectrum(rootf, newest_run, r, Form("%s, latest: point %s at %.2f, %.2f mm, run %ld", name.c_str(),
                                           p["point"].c_str(), num(p, "x_mm"), num(p, "y_mm"), newest_run),
                hspec, ffit, trash);
  c->Update();
  save_png(c, dir, name);
}

}  // namespace live

void live_update() { live::update() ; }

void live_gain_map(const char *scan, const char *z = "gain", double every = 10)
{
  std::string d = live::scan_dir(scan, __FILE__);
  if (d.empty()) { printf("no scan folder %s\n", scan) ; return ; }
  live::dir = d;
  live::name = gSystem->BaseName(d.c_str());
  live::zvar = z;
  gStyle->SetPalette(kViridis);
  gStyle->SetTitleFontSize(0.045);
  gErrorIgnoreLevel = kWarning;
  live::c = new TCanvas("live_gain_map", (live::name + " (live)").c_str(), 1500, 640);
  live::c->Divide(2, 1);
  live::c->cd(1) ; gPad->SetRightMargin(0.14);
  live::update();
  if (every <= 0) return;
  if (gROOT->IsBatch()) {
    while (!live::finished) { gSystem->Sleep(int(every * 1000)) ; live::update() ; }
    return;
  }
  auto *t = new TTimer(int(every * 1000));
  t->SetCommand("live_update()");
  t->TurnOn();
}
