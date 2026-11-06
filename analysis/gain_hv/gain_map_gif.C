// Animated GIF of a finished gain map being taken: the spot gain (or mu) of the usable fits fills the
// scan's grid (stage y down, as the bench face) in the order the points were taken (red ring: the newest), one frame per `step` points,
// on the colour scale of the final map (2-98 % of its points), and the full map held at the end. Next to
// it (spectrum = true) the newest point's spot charge spectrum with its single-photoelectron fit, as
// live_gain_map.C shows it. Reads the scan's CSV, fit_gain_map.sh's <scan>_gain_map.csv and, for the
// spectra, its <scan>_gain_map_spectra.root (or <scan>_gain_map_parts/ while it is fitting); writes
// <scan>_gain_map.gif.
//   root -b -q 'analysis/gain_hv/gain_map_gif.C("<scan>")'                 ~60 frames, gain
//   root -b -q 'analysis/gain_hv/gain_map_gif.C("<scan>", "mu", 120, 8)'   120 frames of mu, 80 ms each
//   root -b -q 'analysis/gain_hv/gain_map_gif.C("<scan>", "gain", 60, 10, false)'   the map alone
// <scan>: the scan's folder (logs/scans/<scan>/) or just <scan>. Needs batch mode (-b). Loops for ever when the
// lab venv's python (PIL) is there to set it, else plays once.
#include <set>
#include <fstream>
#include "TH2D.h"
#include "TMarker.h"
#include "TStyle.h"
#include "TROOT.h"
#include "live_common.h"
#include "../../lib/tb_ydown.h"

void gain_map_gif(const char *scan, const char *z = "gain", int frames = 60, int delay_cs = 10, bool spectrum = true)
{
  using namespace live;
  std::string dir = scan_dir(scan, __FILE__);
  if (dir.empty()) { printf("no scan folder %s\n", scan) ; return ; }
  std::string name = gSystem->BaseName(dir.c_str());
  auto pts = read_csv(dir + "/" + name + ".csv");
  auto fits = read_csv(dir + "/" + name + "_gain_map.csv");
  if (pts.empty() || fits.empty()) { printf("%s: no points or no %s_gain_map.csv\n", name.c_str(), name.c_str()) ; return ; }
  std::map<std::string, double> zval;     // point -> value of its usable spot fit
  std::map<std::string, Row> spotrow;     // point -> its spot fit, usable or not
  for (auto &r : fits) {
    if (r["ch"] != "spot") continue;
    spotrow[r["point"]] = r;
    if (r["peak"] == "1") zval[r["point"]] = num(r, z);
  }
  // run -> the chunk whose .root holds its spectra (each chunk's .txt lists its runs' files)
  std::map<long, std::string> chunk_of;
  std::string parts = dir + "/" + name + "_gain_map_parts";
  if (void *dd = gSystem->OpenDirectory(parts.c_str())) {
    while (const char *f = gSystem->GetDirEntry(dd)) {
      std::string fs = f;
      if (fs.size() != 13 || fs.compare(0, 5, "chunk") || fs.compare(9, 4, ".txt")) continue;
      std::ifstream in(parts + "/" + fs);
      std::string l;
      while (std::getline(in, l)) {     // .../<type>-<run>-0000.evt
        size_t e = l.rfind('-'), b = e == std::string::npos || e == 0 ? std::string::npos : l.rfind('-', e - 1);
        if (b == std::string::npos) continue;
        try { chunk_of[std::stol(l.substr(b + 1, e - b - 1))] = parts + "/" + fs.substr(0, 9) + ".root" ; } catch (...) {}
      }
    }
    gSystem->FreeDirectory(dd);
  }
  // a finished map: fit_gain_map.sh has packed the spectra of every run into one file
  std::string packed = dir + "/" + name + "_gain_map_spectra.root";
  if (chunk_of.empty() && !gSystem->AccessPathName(packed.c_str()))
    for (auto &p : pts) try { chunk_of[std::stol(p["run"])] = packed ; } catch (...) {}
  if (spectrum && chunk_of.empty()) { printf("no spectra in %s or %s: the map alone\n", packed.c_str(), parts.c_str()) ; spectrum = false ; }
  // one set of axes for every frame's spectrum: from the pedestal to 4.5x the scan's median Q1, and up to
  // twice the highest bin of the spectra shown
  double sxlo = NAN, sxhi = NAN, symax = NAN;
  if (spectrum) {
    std::vector<double> q1s, q0s;
    for (auto &kv : spotrow) if (kv.second["peak"] == "1") { q1s.push_back(num(kv.second, "q1_pC")) ; q0s.push_back(num(kv.second, "q0_pC") - 5 * num(kv.second, "s0_pC")) ; }
    if (!q1s.empty()) {
      std::sort(q1s.begin(), q1s.end());
      std::sort(q0s.begin(), q0s.end());
      sxlo = q0s[q0s.size() / 2] ; sxhi = 4.5 * q1s[q1s.size() / 2];
    }
    double hmax = 0;
    int stepv = std::max<int>(1, (pts.size() + frames - 1) / frames);
    for (size_t i = 0; i < pts.size(); i += stepv) {
      long run = std::stol(pts[i]["run"]);
      if (!chunk_of.count(run)) continue;
      TFile *f = TFile::Open(chunk_of[run].c_str());
      if (TH1 *hh = f && !f->IsZombie() ? (TH1 *) f->Get(Form("q_run%ld_spot", run)) : nullptr) hmax = std::max(hmax, hh->GetMaximum());
      delete f;
    }
    if (hmax > 0) symax = 2 * hmax;
  }
  std::set<double> xs, ys;
  for (auto &p : pts) {
    xs.insert(std::round(num(p, "target_x_mm") * 1000) / 1000);
    ys.insert(std::round(num(p, "target_y_mm") * 1000) / 1000);
  }
  std::vector<double> vx(xs.begin(), xs.end()), vy(ys.begin(), ys.end()), all;
  for (auto &kv : zval) all.push_back(kv.second);
  std::sort(all.begin(), all.end());
  double dx = vx.size() > 1 ? vx[1] - vx[0] : 1, dy = vy.size() > 1 ? vy[1] - vy[0] : 1;
  gStyle->SetPalette(kViridis);
  gStyle->SetTitleFontSize(0.04);
  gErrorIgnoreLevel = kWarning;
  auto *c = new TCanvas("gif", name.c_str(), spectrum ? 1500 : 900, spectrum ? 640 : 700);
  if (spectrum) c->Divide(2, 1);
  c->cd(1);
  gPad->SetRightMargin(0.15);
  TH1 *hspec = nullptr;
  TF1 *ffit = nullptr;
  std::vector<TObject *> trash;
  const Row *shown = nullptr;          // the newest point with a spot fit, for the spectrum
  auto *h = new TH2D("gif_map", "", vx.size(), vx.front() - dx / 2, vx.back() + dx / 2, vy.size(), vy.front() - dy / 2,
                     vy.back() + dy / 2);
  h->SetDirectory(nullptr);
  h->SetStats(false);
  h->SetMinimum(all[size_t(0.02 * (all.size() - 1))]);
  h->SetMaximum(all[size_t(0.98 * (all.size() - 1))]);
  h->SetContour(64);
  std::string gif = dir + "/" + name + "_gain_map.gif";
  gSystem->Unlink(gif.c_str());
  int step = std::max<int>(1, (pts.size() + frames - 1) / frames);
  auto *m = new TMarker(0, 0, 24);
  m->SetMarkerColor(kRed) ; m->SetMarkerSize(2);
  for (size_t i = 0; i < pts.size(); i++) {
    auto &p = pts[i];
    auto it = zval.find(p["point"]);
    if (it != zval.end()) h->Fill(num(p, "target_x_mm"), num(p, "target_y_mm"), it->second);
    if (spotrow.count(p["point"])) shown = &p;
    bool last = i + 1 == pts.size();
    if ((i + 1) % step && !last) continue;
    h->SetTitle(Form("%s: spot %s, point %zu of %zu;stage x [mm];stage y [mm]", name.c_str(), z, i + 1, pts.size()));
    c->cd(1);
    tb::draw_y_down(h, "COLZ");
    if (!last) { m->SetX(num(p, "target_x_mm")) ; m->SetY(tb::y_down(num(p, "target_y_mm"))) ; m->Draw() ; }
    if (spectrum) {
      for (auto *o : trash) delete o;
      trash.clear();
      c->cd(2);
      gPad->Clear();
      if (shown) {
        const Row &q = *shown;
        long run = std::stol(q.at("run"));
        if (chunk_of.count(run))
          draw_spectrum(chunk_of[run], run, spotrow[q.at("point")], Form("point %s at %.2f, %.2f mm, run %ld", q.at("point").c_str(),
                                                                         num(q, "x_mm"), num(q, "y_mm"), run), hspec, ffit, trash,
                        sxlo, sxhi, symax);
      }
    }
    c->Print((gif + "+" + std::to_string(delay_cs)).c_str());
  }
  for (int k = 0; k < 15; k++) c->Print((gif + "+" + std::to_string(delay_cs)).c_str());   // hold the full map ~1.5 s
  // ROOT's "file.gif++" (loop for ever) fails here (ROOT 6.32): set the loop with PIL from the lab venv
  std::string py = std::string(gSystem->HomeDirectory()) + "/venvs/lab/bin/python";
  if (!gSystem->AccessPathName(py.c_str()))
    gSystem->Exec(Form("%s -c \"import sys; from PIL import Image; im = Image.open(sys.argv[1]); "
                       "fr = []\nfor i in range(im.n_frames): im.seek(i); fr.append(im.copy())\n"
                       "fr[0].save(sys.argv[1], save_all=True, append_images=fr[1:], loop=0, duration=%d)\" %s",
                       py.c_str(), delay_cs * 10, gif.c_str()));
  printf("wrote %s (%zu points, a frame every %d)\n", gif.c_str(), pts.size(), step);
}
