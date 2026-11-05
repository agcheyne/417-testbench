// live_common.h - shared by the live monitors (live_gain_map.C, live_hv_curve.C): reading the CSVs a
// scan and its live fits write while they grow, telling whether the scan has ended, and drawing a
// run's spot spectrum with its single-photoelectron fit.
#ifndef LIVE_COMMON_H
#define LIVE_COMMON_H
#include <fstream>
#include <sstream>
#include <map>
#include <vector>
#include <string>
#include <algorithm>
#include <ctime>
#include "TCanvas.h"
#include "TH1.h"
#include "TFile.h"
#include "TPaveText.h"
#include "TPad.h"
#include "TGraph.h"
#include "TLine.h"
#include "TSystem.h"
#include "../../lib/tb_spe.h"

namespace live {

using Row = std::map<std::string, std::string>;

// rows of a CSV with a header; a file being written may end in a partial line, which is skipped
inline std::vector<Row> read_csv(const std::string &path)
{
  std::vector<Row> rows;
  std::ifstream in(path);
  std::string line;
  if (!std::getline(in, line)) return rows;
  std::vector<std::string> head;
  { std::stringstream ss(line) ; std::string c ; while (std::getline(ss, c, ',')) head.push_back(c) ; }
  while (std::getline(in, line)) {
    if (std::count(line.begin(), line.end(), ',') != (long) head.size() - 1) continue;
    Row r;
    std::stringstream ss(line);
    std::string c;
    for (size_t i = 0; i < head.size() && std::getline(ss, c, ','); i++) r[head[i]] = c;
    rows.push_back(r);
  }
  return rows;
}

inline double num(const Row &r, const char *k)
{
  auto it = r.find(k);
  if (it == r.end() || it->second.empty()) return NAN;
  try { return std::stod(it->second) ; } catch (...) { return NAN ; }
}

// as fit_gain_map.sh's peak flag, less the scan-median outlier test
inline bool usable(const Row &r)
{
  double g = num(r, "gain"), eg = num(r, "gain_err"), q1 = num(r, "q1_pC"), s0 = num(r, "s0_pC"), s1 = num(r, "s1_pC");
  return r.count("converged") && r.at("converged") == "1" && s1 < 0.8 * q1 && q1 > 5 * s0 && eg < 0.5 * g;
}

// the scan's folder from a folder or a bare <scan> name (logs/scans/ of the repository); "" if none
inline std::string scan_dir(const char *scan, const char *macro_file)
{
  std::string d = scan;
  while (d.size() > 1 && d.back() == '/') d.pop_back();
  if (gSystem->AccessPathName(d.c_str())) {
    std::string tb = gSystem->DirName(gSystem->DirName(gSystem->DirName(macro_file)));
    d = tb + "/logs/scans/" + d;
  }
  return gSystem->AccessPathName(d.c_str()) ? "" : d;
}

// ended: "# ended" in <scan>.txt (scan_xy.sh / scan_hv.sh since 31 Oct 2026), or, once it has points,
// state/scan_point.conf no longer has this scan's id (an older scan, or one that was killed)
inline bool scan_ended(const std::string &dir, const std::string &name, bool has_points)
{
  std::ifstream t(dir + "/" + name + ".txt");
  std::string l, id;
  while (std::getline(t, l)) {
    if (l.rfind("# ended ", 0) == 0) return true;
    size_t a = l.find(", id ");
    if (l.rfind("# ", 0) == 0 && l.find("scan ") != std::string::npos && a != std::string::npos && id.empty())
      id = l.substr(a + 5, l.find(',', a + 5) - a - 5);
  }
  if (!has_points || id.empty()) return false;
  std::string tb = gSystem->DirName(gSystem->DirName(gSystem->DirName(dir.c_str())));
  std::ifstream pf(tb + "/state/scan_point.conf");
  while (std::getline(pf, l)) if (l == "SCAN_ID=" + id) return false;
  return true;
}

inline std::string now_hms()
{
  time_t now = time(nullptr);
  char b[16];
  strftime(b, sizeof b, "%H:%M:%S", localtime(&now));
  return b;
}

// a run's spot spectrum (q_run<N>_spot in rootfile) in the current pad, log y, with tb::fit_spe redone on
// it (the fit tables keep only the parameters), its pedestal and 1, 2, 3 photoelectron terms, and a box
// of the fit table's row; below it the residuals (n - f) / sqrt(f), bins merged until the fit expects
// >= 5 events in each, and their Pearson chi2/ndf in the box (the fit's own, a likelihood ratio, sits near 0.5 for
// these sparse spectra however good the fit: ~1 is good for Pearson's). False if not there yet.
// xlo/xhi/ymax fix the axes (e.g. the same for every frame of a GIF); NaN = from this spectrum. The two
// sub-pads are deleted by the next Clear() of the pad.
inline bool draw_spectrum(const std::string &rootfile, long run, const Row &r, const std::string &title,
                          TH1 *&hspec, TF1 *&ffit, std::vector<TObject *> &trash,
                          double xlo = NAN, double xhi = NAN, double ymax = NAN)
{
  TFile *f = TFile::Open(rootfile.c_str());
  TH1 *h = f && !f->IsZombie() ? (TH1 *) f->Get(Form("q_run%ld_spot", run)) : nullptr;
  if (h) {
    delete hspec;
    hspec = (TH1 *) h->Clone("live_spec");
    hspec->SetDirectory(nullptr);
  }
  if (f) f->Close();
  delete f;
  if (!h) return false;
  delete ffit;
  tb::SpeFit fit = tb::fit_spe(hspec);
  ffit = fit.f;
  hspec->SetTitle((title + ";charge [pC];events").c_str());
  hspec->SetStats(false);
  hspec->SetLineColor(kGray + 2);
  double q0 = num(r, "q0_pC"), q1 = num(r, "q1_pC");
  int lo = hspec->FindFirstBinAbove(0), hi = hspec->FindLastBinAbove(0);
  if (std::isnan(xlo)) xlo = hspec->GetBinCenter(lo) - 0.05;
  if (std::isnan(xhi)) xhi = std::min(hspec->GetBinCenter(hi), q0 + 8 * std::max(q1, 0.05));
  hspec->GetXaxis()->SetRangeUser(xlo, xhi);
  hspec->SetMinimum(0.05);            // low enough for the 2 and 3 p.e. terms at mu ~ 0.1
  if (!std::isnan(ymax)) hspec->SetMaximum(ymax);
  // the spectrum in the top 70 % of the pad, the residuals in the bottom 30 %
  TVirtualPad *parent = gPad;
  auto *top = new TPad("spec_top", "", 0, 0.3, 1, 1), *bot = new TPad("spec_res", "", 0, 0, 1, 0.3);
  for (TPad *p : {top, bot}) {
    p->SetBit(kCanDelete);
    p->SetLeftMargin(parent->GetLeftMargin()) ; p->SetRightMargin(parent->GetRightMargin());
    parent->cd() ; p->Draw();
  }
  top->SetBottomMargin(0.02) ; top->SetTopMargin(parent->GetTopMargin() / 0.7);
  bot->SetTopMargin(0.03) ; bot->SetBottomMargin(0.32) ; bot->SetGridy();
  top->cd();
  gPad->SetLogy();
  hspec->GetXaxis()->SetLabelSize(0) ; hspec->GetXaxis()->SetTitleSize(0);
  hspec->GetYaxis()->SetLabelSize(0.05) ; hspec->GetYaxis()->SetTitleSize(0.05) ; hspec->GetYaxis()->SetTitleOffset(0.9);
  hspec->Draw("HIST");
  if (ffit) {
    // the sum (red), then on top of it the terms (dashed): pedestal grey, 1 p.e. blue, 2 green, 3 orange
    const int ccol[] = {kGray + 1, kAzure + 1, kGreen + 2, kOrange + 1};
    ffit->SetLineColor(kRed) ; ffit->SetNpx(600) ; ffit->Draw("same");
    for (int n = 0; n <= 3; n++) {
      TF1 *fn = tb::spe_function(Form("live_term%d", n), xlo, xhi, fit.model, n);
      fn->SetParameters(fit.par);
      fn->SetLineColor(ccol[n]) ; fn->SetLineStyle(2) ; fn->SetLineWidth(2) ; fn->SetNpx(600);
      fn->Draw("same");
      trash.push_back(fn);
    }
  }
  // residuals over the fit range, neighbouring bins merged until the fit (bin-centre prediction)
  // expects >= 5 events in each group, so the sparse 1 p.e. tail of a high gain counts too; a last group
  // short of 5 is left out. Clipped at +-5 (open markers).
  double pearson = 0, nacc = 0, facc = 0, xacc = 0;
  int npear = 0, nb = 0;
  auto *gres = new TGraph(), *gclip = new TGraph();
  if (ffit)
    for (int b = hspec->FindBin(ffit->GetXmin()); b <= hspec->FindBin(ffit->GetXmax()); b++) {
      double x = hspec->GetBinCenter(b);
      nacc += hspec->GetBinContent(b) ; facc += ffit->Eval(x) ; xacc += x ; nb++;
      if (!(facc >= 5)) continue;
      double res = (nacc - facc) / std::sqrt(facc), xm = xacc / nb;
      pearson += res * res;
      npear++;
      if (xm >= xlo && xm <= xhi) {
        if (std::abs(res) < 5) gres->SetPoint(gres->GetN(), xm, res);
        else gclip->SetPoint(gclip->GetN(), xm, res > 0 ? 4.8 : -4.8);
      }
      nacc = facc = xacc = 0 ; nb = 0;
    }
  int ndf = ffit ? npear - ffit->GetNumberFreeParameters() : 0;
  bot->cd();
  TH1F *fr = bot->DrawFrame(xlo, -5, xhi, 5);
  fr->GetXaxis()->SetTitle("charge [pC]") ; fr->GetYaxis()->SetTitle("(n-f)/#sqrt{f}");
  fr->GetXaxis()->SetLabelSize(0.12) ; fr->GetXaxis()->SetTitleSize(0.13) ; fr->GetXaxis()->SetTitleOffset(1.0);
  fr->GetYaxis()->SetLabelSize(0.10) ; fr->GetYaxis()->SetTitleSize(0.11) ; fr->GetYaxis()->SetTitleOffset(0.4);
  fr->GetYaxis()->SetNdivisions(505);
  auto *l0 = new TLine(xlo, 0, xhi, 0);
  l0->SetLineColor(kRed) ; l0->Draw();
  gres->SetMarkerStyle(20) ; gres->SetMarkerSize(0.5);
  gclip->SetMarkerStyle(24) ; gclip->SetMarkerSize(0.6);
  if (gres->GetN()) gres->Draw("P");
  if (gclip->GetN()) gclip->Draw("P");
  for (TObject *o : {(TObject *) gres, (TObject *) gclip, (TObject *) l0}) trash.push_back(o);
  top->cd();
  auto *t = new TPaveText(0.55, 0.50, 0.93, 0.92, "NDC");
  t->SetFillColor(kWhite) ; t->SetTextAlign(12) ; t->SetTextSize(0.04);
  t->AddText(Form("gain %.3g #pm %.2g", num(r, "gain"), num(r, "gain_err")));
  t->AddText(Form("#mu %.3f", num(r, "mu")));
  t->AddText(Form("Q_{1} %.3f pC, #sigma_{1} %.3f", q1, num(r, "s1_pC")));
  t->AddText(ndf > 0 ? Form("Pearson #chi^{2}/ndf %.2f, %d groups", pearson / ndf, npear) : "Pearson #chi^{2}/ndf -");
  t->AddText(Form("likelihood #chi^{2}/ndf %.2f", num(r, "chi2ndf")));
  t->AddText(usable(r) ? "usable" : "NOT usable");
  t->Draw();
  trash.push_back(t);
  parent->cd();
  return true;
}

// the canvas to <scan>_live.png, written aside and renamed so a reader never sees half a file
inline void save_png(TCanvas *c, const std::string &dir, const std::string &name)
{
  std::string png = dir + "/" + name + "_live.png", tmp = dir + "/" + name + "_live.tmp.png";
  c->SaveAs(tmp.c_str());
  gSystem->Rename(tmp.c_str(), png.c_str());
}

}  // namespace live
#endif
