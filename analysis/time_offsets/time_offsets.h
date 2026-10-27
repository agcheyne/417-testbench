#ifndef __TIME_OFFSETS_H__
#define __TIME_OFFSETS_H__

#include <pmonitor/pmonitor.h>
#include <Event/Event.h>
#include <Event/EventTypes.h>

int process_event (Event *e); //++CINT

// --- settings (before the data) -------------------------------------------------------------
void set_signal_window(double from_ns = 17, double to_ns = 35);   // relative to the group trigger
void set_min_amplitude(double mv = 4.0);     // pulses used: at least this high (well clear of the noise)

// --- data ------------------------------------------------------------------------------------
void add_scan(const char *csv);              // every run of a scan_xy.sh log (logs/scans/*.csv)
void add_run(const char *filename);          // one run (all its parts); or pfileopen() + prun()
void save_times(const char *filename);       // the pulse times, to refit later without the data
void load_times(const char *filename);

// --- results ---------------------------------------------------------------------------------
// each input's laser peak (core Gaussian of its CFD times), drift-corrected per run, relative to
// the mean of the measured inputs of groups 2 and 3; inputs with fewer than min_hits in the peak,
// or no clear peak (pickup, dead), are left unmeasured
void fit_offsets(int min_hits = 50);
void draw_offsets();                         // offsets, core widths, hits per input; drift per run; peaks
void write_db(const char *note = "");        // db/v1742/time_offsets_<serial>_<speed>.conf
void save_plots(const char *prefix = "time_offsets");   // <prefix>_offsets.png

#endif /* __TIME_OFFSETS_H__ */
