#ifndef __GENERAL_H__
#define __GENERAL_H__

#include <pmonitor/pmonitor.h>
#include <Event/Event.h>
#include <Event/EventTypes.h>

int process_event (Event *e); //++CINT

// --- settings (call before prun) -----------------------------------------------------
// signal search window [ns] relative to each group's trigger time (TR trace, 50 % CFD)
void set_signal_window(double from_ns, double to_ns);
// a channel has a hit when its pulse goes below -threshold_mv inside the window
void set_threshold(double threshold_mv = 5.0);
void set_cfd_fraction(double fraction = 0.5);
// charge integration [ns] relative to the trigger time, the same window in every event
// (pedestal events included), so the spectrum can be fitted with fit_gain()
void set_charge_window(double from_ns, double to_ns);
// late pulses: searched from from_ns after the main pulse to the end of the trace
void set_late_pulse_search(double from_ns = 5.0, double threshold_mv = 5.0);
// samples used for the signal baseline (median), before any pulse
void set_baseline_samples(int first = 10, int last = 110);
void set_despike(bool on = true);
// one TTree entry per hit (run, event, channel, pixel, stage position, amplitude, charge, time)
void set_tree(bool on = true);

// --- results ---------------------------------------------------------------------------
void summary();                 // per-channel occupancy, amplitude, timing, late pulses
void suggest_windows();         // where the pulses sit relative to the trigger, per channel
void fit_gain(int ch);          // single-photoelectron fit of the charge spectrum of one channel
void draw_channel(int ch);      // amplitude, charge, time and late-pulse plots of one channel
void draw_maps();               // occupancy per pixel (needs mapping + cabling) and per channel
void save(const char *filename = "");   // histograms (+ tree) to a ROOT file; "" = general_run<run>.root

#endif /* __GENERAL_H__ */
