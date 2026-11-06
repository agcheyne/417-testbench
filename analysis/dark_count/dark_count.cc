// dark_count - dark pulses in one run taken with the laser off (standalone, no pmonitor; first used on
// the Photonis readout-1 dark runs of 1 Nov 2026, 6887-6896).
//
// Every pulse below -3 mV on the whole trace after the baseline samples (a pulse ends when the trace is
// back above -1.5 mV), per input, with its amplitude and its time relative to the group trigger. Events
// where a group's trigger is not found are left out for that group's inputs. Pulses on >= 6 inputs
// within 2 ns of each other are pickup, not photocathode: counted apart and left out. The masked
// inputs (MASK, e.g. "0 3 4 5 6"; default MASKED_CHANNELS in db/testbench.conf) are left out of that
// pickup test and of the coincidence plot.
//
//   dark_count FILE OUT.root [EVENTS [MASK]] > OUT.csv     (EVENTS 0 = all)
//
// OUT.root: per input the amplitude (amp<ch>) and time (time<ch>) of its pulses; the multiplicity of
// >= 5 mV pulses within 2 ns (mult); per input the other inputs with a >= 5 mV pulse within 2 ns
// (coinc). Standard output, one line per input:
//   ch,events,no_trigger,live_ns,n3,n5,n8,n12,pickup_events
// (n3 ... n12: pulses of at least 3, 5, 8, 12 mV). plot_dark.C sums and plots several runs.
#include "tb_v1742.h"
#include "tb_db.h"
#include <Event/fileEventiterator.h>
#include <TFile.h>
#include <TH1D.h>
#include <TH2D.h>
#include <cstdio>
#include <cstdlib>
#include <vector>

int main(int argc, char **argv)
{
  if (argc < 3) { fprintf(stderr, "usage: dark_count FILE OUT.root [EVENTS [MASK]] > OUT.csv\n") ; return 2 ; }
  long nmax = argc > 3 && atol(argv[3]) > 0 ? atol(argv[3]) : 1000000000L, n = 0, pickup = 0;
  static tb::V1742Event ev;
  static tb::RunInfo info;
  const double thr = 3.0;
  unsigned masked = 0;
  for (int ch : tb::masked_channels(argc > 4 ? argv[4] : "-")) masked |= 1u << ch;

  TFile out(argv[2], "RECREATE");
  TH1D *ha[32], *ht[32];
  for (int ch = 0; ch < 32; ch++) {
    ha[ch] = new TH1D(Form("amp%d", ch), Form("ch%d dark pulse amplitude;amplitude [mV];pulses", ch), 200, 0, 200);
    ht[ch] = new TH1D(Form("time%d", ch), Form("ch%d dark pulse time;time - group trigger [ns];pulses", ch), 440, -100, 120);
  }
  TH1D *hmult = new TH1D("mult", "inputs >= 5 mV within 2 ns;inputs;clusters", 33, -0.5, 32.5);
  TH2D *hco = new TH2D("coinc", "other inputs within 2 ns of a >= 5 mV pulse;input;other inputs", 32, -0.5, 31.5, 10, -0.5, 9.5);
  long cnt[32][4] = {};
  double live[32] = {};
  long notrig[32] = {};
  const double cut[4] = {3, 5, 8, 12};

  int status;
  fileEventiterator it(argv[1], status);
  if (status) { fprintf(stderr, "cannot open %s\n", argv[1]) ; return 1 ; }
  Event *e;
  struct Pulse { int ch; double t, a; };
  std::vector<Pulse> ps;
  while (n < nmax && (e = it.getNextEvent())) {
    if (e->getEvtType() == 9) { info.parse(e) ; ev.begin_run(e) ; delete e ; continue ; }
    if (!ev.fill(e)) { delete e ; continue ; }
    delete e;
    n++;
    ps.clear();
    int k0 = ev.bl_last + 5, k1 = ev.ns - 20;
    for (int ch = 0; ch < 32; ch++) {
      const double *v = ev.v[ch], *t = ev.t[ch];
      if (std::isnan(ev.trigger_of(ch))) { notrig[ch]++ ; continue ; }   // corrupt group readout
      live[ch] += t[k1] - t[k0];
      for (int k = k0; k < k1; k++) {
        if (v[k] >= -thr) continue;
        int m = k;
        while (k < k1 && v[k] < -thr / 2) { if (v[k] < v[m]) m = k ; k++ ; }
        ps.push_back({ch, t[m] - ev.trigger_of(ch), -v[m]});
      }
    }
    // pickup: >= 6 inputs with >= 5 mV pulses within 2 ns
    std::vector<bool> bad(ps.size(), false);
    bool any = false;
    for (size_t i = 0; i < ps.size(); i++) {
      if (ps[i].a < 5) continue;
      unsigned mask = 0;
      for (size_t j = 0; j < ps.size(); j++)
        if (ps[j].a >= 5 && std::fabs(ps[j].t - ps[i].t) < 2) mask |= 1u << ps[j].ch;
      int m = __builtin_popcount(mask & ~masked);
      hmult->Fill(m);
      if (m >= 6) {
        any = true;
        for (size_t j = 0; j < ps.size(); j++) if (std::fabs(ps[j].t - ps[i].t) < 2) bad[j] = true;
      }
    }
    pickup += any;
    for (size_t i = 0; i < ps.size(); i++) {
      if (ps[i].a < 5 || bad[i] || (masked >> ps[i].ch & 1)) continue;
      unsigned m = 0;
      for (size_t j = 0; j < ps.size(); j++)
        if (j != i && ps[j].a >= 5 && !(masked >> ps[j].ch & 1) && ps[j].ch != ps[i].ch && std::fabs(ps[j].t - ps[i].t) < 2) m |= 1u << ps[j].ch;
      hco->Fill(ps[i].ch, __builtin_popcount(m));
    }
    for (size_t i = 0; i < ps.size(); i++) {
      if (bad[i]) continue;
      ha[ps[i].ch]->Fill(ps[i].a);
      ht[ps[i].ch]->Fill(ps[i].t);
      for (int c = 0; c < 4; c++) cnt[ps[i].ch][c] += ps[i].a >= cut[c];
    }
  }
  for (int ch = 0; ch < 32; ch++)
    printf("%d,%ld,%ld,%.0f,%ld,%ld,%ld,%ld,%ld\n", ch, n, notrig[ch], live[ch], cnt[ch][0], cnt[ch][1], cnt[ch][2], cnt[ch][3], pickup);
  out.Write();
  return 0;
}
