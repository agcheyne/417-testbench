// Gain vs HV of a finished HV scan in batch; fit_hv_scan.sh runs it:
//   root -b -q 'fit_hv_scan.C("<scan>_runs.txt", "<scan dir>/<scan>", 21, 30, "20 12 19 21 22 23")'
// q_from >= q_to: the window is found from the data (find_window). list: one "file.evt hv_volts" per line; writes <prefix>_fits.csv/.root, <prefix>_gain.png and
// <prefix>_spectra_<ch>.png. channels: the ones to plot ("" = all with a curve, except exclude).
#include "gain_hv.h"
R__LOAD_LIBRARY(libgain_hv.so)

void fit_hv_scan(const char *list, const char *prefix, double q_from, double q_to,
                 const char *spot = "", int events = 20000, const char *channels = "", const char *exclude = "")
{
  setvbuf(stdout, nullptr, _IOLBF, 0);   // the log fills line by line, not only at the end
  if (q_from < q_to) set_charge_window(q_from, q_to);
  else if (!find_window(list)) return;
  set_events(events);
  set_spot(spot);
  add_runs(list);
  fit_all();
  plot_scan(prefix, channels, exclude);
  save(Form("%s_fits", prefix));   // not <prefix>.csv: that is the scan's own log
}
