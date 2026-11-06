// timing_point - timing of every channel in one run (one scan point; standalone, no pmonitor; used
// by timing_map.sh). The CFD time of each hit (>= 5 mV, 21-31 ns after its DRS4 group trigger) less the
// input's time offset (db/v1742), then tb::timing_fit on 5 ps bins: main peak mean and core sigma,
// and the share of the shoulder and late peak. One CSV line per channel with >= 50 hits:
//   point,ch,events,hits,amp_mV,t_mean_ns,t_mean_err,sigma_ns,sigma_err,rest_frac,late_frac,ok
//   timing_point FILE POINT [EVENTS]          (EVENTS default 20000)
#include "tb_v1742.h"
#include "tb_fit.h"
#include <Event/fileEventiterator.h>
#include <TH1D.h>
#include <cstdio>
#include <cstdlib>
int main(int argc, char **argv)
{
  if (argc < 3) { fprintf(stderr, "usage: timing_point FILE POINT [EVENTS]\n") ; return 2 ; }
  long nmax = argc > 3 ? atol(argv[3]) : 20000, n = 0;
  static tb::V1742Event ev;
  static tb::RunInfo info;
  tb::TimeOffsets toff;
  bool loaded = false;
  TH1D *h[32];
  for (int ch = 0; ch < 32; ch++) { h[ch] = new TH1D(Form("t%d", ch), "", 2400, 20, 32) ; h[ch]->SetDirectory(nullptr) ; }
  long nhit[32] = {};
  double asum[32] = {};
  int status;
  fileEventiterator it(argv[1], status);
  if (status) { fprintf(stderr, "cannot open %s\n", argv[1]) ; return 1 ; }
  Event *e;
  while (n < nmax && (e = it.getNextEvent())) {
    if (e->getEvtType() == 9) { info.parse(e) ; ev.begin_run(e) ; delete e ; continue ; }
    if (!ev.fill(e)) { delete e ; continue ; }
    delete e;
    if (!loaded) { toff.load(ev.serial, ev.freq) ; loaded = true ; }
    n++;
    for (int ch = 0; ch < 32; ch++) {
      tb::Cuts c;
      c.sig_from = 21 + toff.ns[ch] ; c.sig_to = 31 + toff.ns[ch];
      tb::ChannelHit x = tb::measure(ev, ch, c);
      if (!x.valid || !x.hit || std::isnan(x.t_cfd)) continue;
      h[ch]->Fill(x.t_cfd - toff.ns[ch]);
      nhit[ch]++ ; asum[ch] += x.amp;
    }
  }
  for (int ch = 0; ch < 32; ch++) {
    if (nhit[ch] < 50) continue;
    tb::TimingFit f = tb::timing_fit(h[ch]);
    printf("%s,%d,%ld,%ld,%.1f,%.5f,%.5f,%.5f,%.5f,%.4f,%.4f,%d\n", argv[2], ch, n, nhit[ch], asum[ch] / nhit[ch],
           f.mean, f.emean, f.sigma, f.esigma, f.rest_frac, f.late_frac, f.ok);
  }
  return 0;
}
