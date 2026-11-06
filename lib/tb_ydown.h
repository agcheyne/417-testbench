// tb_ydown.h - 

// helper for x-y maps drawn as the bench face: (0,0) at the top left (ROOT).
//
// ROOT has no reversed axis, so a map is drawn as a copy at -y with its y labels changed back to +y;
// anything drawn over it (boxes, markers, lines, text) goes at tb::y_down(y). The histogram itself keeps
// the stage y (fits, FindBin, files):
//   TH2D *hd = tb::draw_y_down(h, "colz");      // the copy drawn
//   marker->DrawMarker(x, tb::y_down(y));

// A histogram built at -y by the caller just needs tb::y_down_axis(h) after its Draw.

#ifndef TB_YDOWN_H
#define TB_YDOWN_H

#include <TAxis.h>
#include <TH2.h>
#include <TH2D.h>
#include <THLimitsFinder.h>
#include <TString.h>

#include <map>
#include <string>
#include <vector>

namespace tb {

inline double y_down(double y) { return -y; }

// labels of h's y axis (drawn at -y) as the stage y
inline void y_down_axis(TH1 *h)
{
  TAxis *a = h->GetYaxis();
  double lo, hi, bw;
  int n;
  THLimitsFinder::Optimize(a->GetXmin(), a->GetXmax(), a->GetNdivisions() % 100, lo, hi, n, bw, "");   // the labelled ticks
  for (int i = 0; i <= n; i++) a->ChangeLabelByValue(lo + i * bw, -1, -1, -1, -1, -1, Form("%g", y_down(lo + i * bw) + 0.0));
}

// draws h with y down: a copy at -y (bins, contents, minimum / maximum, contours, axis titles and styles),
// replaced at the next call for the same h name; returns the copy
inline TH2D *draw_y_down(TH2 *h, const char *opt)
{
  static std::map<std::string, TH2D *> copies;
  std::string name = std::string(h->GetName()) + "_ydown";
  delete copies[name];
  int nx = h->GetNbinsX(), ny = h->GetNbinsY();
  std::vector<double> ex, ey;
  for (int i = 1; i <= nx + 1; i++) ex.push_back(h->GetXaxis()->GetBinLowEdge(i));
  for (int j = ny + 1; j >= 1; j--) ey.push_back(y_down(h->GetYaxis()->GetBinLowEdge(j)));
  auto *f = new TH2D(name.c_str(), h->GetTitle(), nx, ex.data(), ny, ey.data());
  f->SetDirectory(nullptr);
  copies[name] = f;
  for (int i = 1; i <= nx; i++)
    for (int j = 1; j <= ny; j++) f->SetBinContent(i, ny + 1 - j, h->GetBinContent(i, j));
  f->SetEntries(h->GetEntries());
  for (int k = 0; k < 3; k++) {
    TAxis *a = k == 0 ? h->GetXaxis() : k == 1 ? h->GetYaxis() : h->GetZaxis();
    TAxis *b = k == 0 ? f->GetXaxis() : k == 1 ? f->GetYaxis() : f->GetZaxis();
    a->TAttAxis::Copy(*b);
    b->SetTitle(a->GetTitle()) ; b->SetMoreLogLabels(a->GetMoreLogLabels()) ; b->SetNoExponent(a->GetNoExponent());
    b->CenterTitle(a->GetCenterTitle());
  }
  if (h->GetXaxis()->TestBit(TAxis::kAxisRange)) f->GetXaxis()->SetRange(h->GetXaxis()->GetFirst(), h->GetXaxis()->GetLast());
  if (h->GetMinimumStored() != -1111) f->SetMinimum(h->GetMinimumStored());
  if (h->GetMaximumStored() != -1111) f->SetMaximum(h->GetMaximumStored());
  if (h->TestBit(TH1::kUserContour) && h->GetContour() > 0) {
    std::vector<double> lv(h->GetContour());
    h->GetContour(lv.data());
    f->SetContour(lv.size(), lv.data());
  } else if (h->GetContour() > 0) f->SetContour(h->GetContour());
  f->SetStats(!h->TestBit(TH1::kNoStats));
  f->Draw(opt);
  y_down_axis(f);
  return f;
}

} // namespace tb

#endif // TB_YDOWN_H
