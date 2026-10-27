#ifndef __TIMING_XTALK_H__
#define __TIMING_XTALK_H__

#include <pmonitor/pmonitor.h>
#include <Event/Event.h>
#include <Event/EventTypes.h>

int process_event (Event *e); //++CINT

// --- settings (before prun) -----------------------------------------------------------
void set_signal_window(double from_ns, double to_ns);   // relative to the group trigger
void set_threshold(double mv = 5.0);                    // hit threshold (and leading-edge level)
void set_cfd_fraction(double fraction = 0.5);
// crosstalk: a "source" is a channel with a pulse above source_mv while every other channel
// stays below it; its neighbours' traces are averaged around the source's CFD time
void set_source_threshold(double source_mv = 20.0);
void set_xtalk_window(double before_ns = 3.0, double after_ns = 8.0);

// --- timing -----------------------------------------------------------------------------
void timing_summary();          // per channel: CFD core sigma, late tail, leading-edge sigma raw / walk-corrected
void draw_timing(int ch);       // CFD time, LE time vs amplitude with the walk fit, corrected LE, CFD vs amplitude

// --- crosstalk --------------------------------------------------------------------------
void xtalk_summary(int source = -1);   // induced peaks in % of the source pulse; -1 = every source seen
void draw_xtalk(int source, int n_neighbours = 6);   // average source pulse and the largest induced ones
void draw_xtalk_matrix();       // source x receiver matrix of the negative induced peak, %

void save(const char *filename = "");  // "" = timing_xtalk_run<run>.root

#endif /* __TIMING_XTALK_H__ */
