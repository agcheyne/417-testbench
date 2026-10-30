// SPE fit of every run in a list, in batch: one part of a gain map, fit_gain_map.sh runs several at once:
//   root -b -q 'fit_gain_map.C("<part>_runs.txt", "<part>", 24.1, 27.3, 20000, "24.1:27.0 24.1:33.0", 0.95)'
// list: one file.evt per line; writes <prefix>.csv (the fit table, as gain_hv's save()), <prefix>.root and
// <prefix>_spots.csv. alt: other windows fitted alongside for the systematic error of the window
// (set_alt_windows); contain: the sums of all channels and of the spot holding this share of the
// mean charge, per point (set_auto_spot), 0 = none
#include "gain_hv.h"
R__LOAD_LIBRARY(libgain_hv.so)

void fit_gain_map(const char *list, const char *prefix, double q_from, double q_to, int events = 20000, const char *alt = "",
                  double contain = 0.95)
{
  setvbuf(stdout, nullptr, _IOLBF, 0);
  set_charge_window(q_from, q_to);
  set_alt_windows(alt);
  set_auto_spot(contain);
  set_events(events);
  add_runs(list);
  fit_all();
  save(prefix);
}
