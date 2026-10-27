// tb_canvas.h - canvases kept in a static pointer by the analyses and the online monitor (ROOT).
//
// Closing a canvas window deletes the TCanvas; a pointer kept to it then points at freed memory,
// and drawing into it (the monitor's 2 s timer, or calling draw_...() again) corrupts the other
// canvases and ROOT. Check it before every use:
//   if (!tb::canvas_open(c_main)) c_main = new TCanvas(...);
#ifndef TB_CANVAS_H
#define TB_CANVAS_H

#include <TCanvas.h>
#include <TROOT.h>

namespace tb {

// true if c is still an open canvas; if it was closed, c is set to nullptr
inline bool canvas_open(TCanvas *&c)
{
  if (c && !gROOT->GetListOfCanvases()->FindObject(c)) c = nullptr;
  return c != nullptr;
}

} // namespace tb

#endif // TB_CANVAS_H
