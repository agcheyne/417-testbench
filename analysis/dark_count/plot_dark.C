// Dark pulses of several dark runs (dark_count's run<N>.csv / run<N>.root in DIR, summed): the rate per
// pad at the 5 mV threshold on the anode map, the rate per input against the threshold, and the
// amplitude and time (relative to the group trigger) of the pulses. The masked inputs (MASK, e.g.
// "0 3 4 5 6"; "-" = MASKED_CHANNELS in db/testbench.conf) are drawn apart: white on the map, red in
// the amplitude plot, and left out of the average.
//   root -b -q 'plot_dark.C+("6887 6888 ...", "out.png", "DIR", "Photonis", "0 3 4 5 6")'
// The detector (for the pixel map and the pad area) defaults to the one in use. The pixel map uses the
// detector's cabling.txt as it is now: for older runs, check it matches the cabling of their time.
#include <fstream>
#include <sstream>
#include "TFile.h"
#include "TH1D.h"
#include "TH2D.h"
#include "TGraphErrors.h"
#include "TCanvas.h"
#include "TLegend.h"
#include "TLatex.h"
#include "TStyle.h"
#include "TSystem.h"
#include "../../lib/tb_db.h"

void plot_dark(const char *runs, const char *png, const char *dir = ".", const char *detector = "", const char *mask = "-")
{
  gStyle->SetOptStat(0) ; gStyle->SetPaintTextFormat(".0f");
  double live[32] = {}, cnt[32][4] = {};
  long events = 0, notrig = 0, pickup = 0;
  TH1D *amp = nullptr, *tim = nullptr, *ampm = nullptr;
  if (!gSystem->Getenv("TB_ROOT")) {   // the repository: two folders up from this macro
    TString f = __FILE__;
    if (!f.BeginsWith("/")) f = TString(gSystem->WorkingDirectory()) + "/" + f;
    gSystem->Setenv("TB_ROOT", gSystem->DirName(gSystem->DirName(gSystem->DirName(f))));
  }
  std::set<int> masked = tb::masked_channels(mask);
  std::istringstream rs(runs);
  for (int run; rs >> run;) {
    std::ifstream in(Form("%s/run%d.csv", dir, run));
    std::string l;
    while (std::getline(in, l)) {
      int ch; long ev, nt; double lv, a, b, c, d; long pk;
      if (sscanf(l.c_str(), "%d,%ld,%ld,%lf,%lf,%lf,%lf,%lf,%ld", &ch, &ev, &nt, &lv, &a, &b, &c, &d, &pk) != 9) continue;
      live[ch] += lv ; cnt[ch][0] += a ; cnt[ch][1] += b ; cnt[ch][2] += c ; cnt[ch][3] += d;
      if (ch == 0) { events += ev ; notrig += nt ; pickup += pk ; }
    }
    TFile *f = TFile::Open(Form("%s/run%d.root", dir, run));
    if (!f || f->IsZombie()) { printf("no %s/run%d.root\n", dir, run) ; continue ; }
    for (int ch = 0; ch < 32; ch++) {
      TH1D *ha = (TH1D *) f->Get(Form("amp%d", ch)), *ht = (TH1D *) f->Get(Form("time%d", ch));
      if (masked.count(ch)) {
        if (!ampm) { ampm = (TH1D *) ha->Clone("ampm") ; ampm->SetDirectory(nullptr) ; } else ampm->Add(ha);
        continue;
      }
      if (!amp) { amp = (TH1D *) ha->Clone("amps") ; amp->SetDirectory(nullptr) ; tim = (TH1D *) ht->Clone("tims") ; tim->SetDirectory(nullptr) ; }
      else { amp->Add(ha) ; tim->Add(ht) ; }
    }
    f->Close();
  }
  if (!amp || events == 0) { printf("plot_dark: nothing read from %s\n", dir) ; return ; }

  tb::Db db;
  if (!(*detector ? db.load(detector) : db.load())) printf("plot_dark: cannot load the detector from the db: no pixel map\n");
  const double area = db.num("PITCH_X_MM", 6.5) * db.num("PITCH_Y_MM", 6.5) / 100;   // cm^2 per pad
  int rmin = 99, rmax = -1, cmin = 99, cmax = -1, row, col;
  for (int ch = 0; ch < 32; ch++)
    if (db.pixel(2000, ch, row, col)) { rmin = std::min(rmin, row) ; rmax = std::max(rmax, row) ; cmin = std::min(cmin, col) ; cmax = std::max(cmax, col) ; }
  if (rmax < 0) { rmin = cmin = 0 ; rmax = cmax = 0 ; }
  double live_s = live[20] * 1e-9;

  auto *c = new TCanvas("c", "", 1800, 1200);
  c->Divide(2, 2, 0.003, 0.003);
  c->cd(1) ; gPad->SetRightMargin(0.14) ; gPad->SetLogz();
  auto *m = new TH2D("m", Form("%s: dark rate per pad, >= 5 mV [Hz] (%ld events, %.3g s live per input);column;row",
                               db.detector.c_str(), events, live_s),
                     cmax - cmin + 1, cmin - 0.5, cmax + 0.5, rmax - rmin + 1, rmin - 0.5, rmax + 0.5);
  for (int ch = 0; ch < 32; ch++) {
    if (!db.pixel(2000, ch, row, col) || masked.count(ch) || live[ch] <= 0) continue;   // masked: white
    m->Fill(col, row, std::max(0.5, cnt[ch][1] / live[ch] * 1e9));
  }
  m->SetMinimum(0.5) ; m->SetMaximum(500) ; m->SetMarkerSize(1.6) ; m->Draw("COLZ TEXT");
  for (int ch = 0; ch < 32; ch++) {
    if (!db.pixel(2000, ch, row, col)) continue;
    auto *t = new TLatex(col, row - 0.3, Form("ch%d%s", ch, masked.count(ch) ? " masked" : ""));
    t->SetTextAlign(22) ; t->SetTextSize(0.03) ; t->SetTextColor(kGray + 2) ; t->Draw();
  }

  c->cd(2) ; gPad->SetLogy() ; gPad->SetGridy();
  TH1F *fr = gPad->DrawFrame(-0.5, 0.3, 31.5, 3e6);
  fr->SetTitle("dark rate per input against threshold;V1742 input;rate [Hz]");
  const int colr[4] = {kGray + 1, kRed, kAzure + 1, kGreen + 2};
  const char *nm[4] = {">= 3 mV", ">= 5 mV", ">= 8 mV", ">= 12 mV"};
  auto *lg = new TLegend(0.55, 0.70, 0.88, 0.88);
  lg->SetBorderSize(0) ; lg->SetFillStyle(0);
  for (int k = 0; k < 4; k++) {
    auto *g = new TGraphErrors();
    for (int ch = 0; ch < 32; ch++) {
      if (live[ch] <= 0) continue;
      double r = cnt[ch][k] / live[ch] * 1e9, e = std::sqrt(std::max(1.0, cnt[ch][k])) / live[ch] * 1e9;
      g->SetPoint(g->GetN(), ch + 0.15 * (k - 1.5), std::max(r, 0.4)) ; g->SetPointError(g->GetN() - 1, 0, cnt[ch][k] ? e : 0);
    }
    g->SetMarkerStyle(20 + k) ; g->SetMarkerColor(colr[k]) ; g->SetLineColor(colr[k]) ; g->SetMarkerSize(0.9) ; g->Draw("P");
    lg->AddEntry(g, nm[k], "p");
  }
  lg->Draw();
  auto *note = new TLatex(0.14, 0.84, "0 counts drawn at 0.4 Hz");
  note->SetNDC() ; note->SetTextSize(0.035) ; note->Draw();

  c->cd(3) ; gPad->SetLogy();
  amp->SetTitle("dark pulse amplitude, inputs summed (red: masked inputs);amplitude [mV];pulses");
  amp->SetLineColor(kBlack) ; amp->GetXaxis()->SetRangeUser(0, 120) ; amp->SetMinimum(0.5);
  amp->SetMaximum(2 * std::max(amp->GetMaximum(), ampm ? ampm->GetMaximum() : 0.0));
  amp->Draw("HIST");
  if (ampm) { ampm->SetLineColor(kRed) ; ampm->Draw("HIST SAME") ; }

  c->cd(4);
  tim->Rebin(4);
  tim->SetTitle("dark pulse time, inputs summed (not masked);time - group trigger [ns];pulses / 2 ns");
  tim->GetXaxis()->SetRangeUser(-35, 58) ; tim->SetLineColor(kBlack) ; tim->Draw("HIST");
  c->SaveAs(png);

  printf("%ld events, %ld without a trigger in a group, %ld with pickup; live per input %.3f s\n", events, notrig, pickup, live_s);
  double tot = 0, tl = 0;
  for (int ch = 0; ch < 32; ch++) {
    if (live[ch] <= 0) continue;
    printf("ch%2d: %8.0f %8.0f %8.0f %8.0f pulses -> %9.1f Hz (>= 5 mV), %8.1f Hz/cm^2%s\n", ch, cnt[ch][0], cnt[ch][1], cnt[ch][2], cnt[ch][3],
           cnt[ch][1] / live[ch] * 1e9, cnt[ch][1] / live[ch] * 1e9 / area, masked.count(ch) ? "  (masked)" : "");
    if (!masked.count(ch)) { tot += cnt[ch][1] ; tl += live[ch] ; }
  }
  printf("inputs not masked: %.0f pulses >= 5 mV, %.1f Hz per pad, %.1f Hz/cm^2\n", tot, tot / tl * 1e9, tot / tl * 1e9 / area);
}
