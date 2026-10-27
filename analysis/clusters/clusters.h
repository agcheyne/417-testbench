#ifndef __CLUSTERS_H__
#define __CLUSTERS_H__

#include <pmonitor/pmonitor.h>
#include <Event/Event.h>
#include <Event/EventTypes.h>

int process_event (Event *e); //++CINT

// --- stage 1: one pass over the data -> pulse height, charge and time of every channel in every event
void set_signal_window(double from_ns = 17, double to_ns = 35);   // relative to the group trigger
void set_time_threshold(double mv = 2.0);    // CFD time only for pulses above this (a time below it is noise)
void set_trace_events(int n = 300);          // waveforms kept of the first n events with >= 2 channels above 3 mV
// charge of every channel in a fixed window [ns] relative to the group trigger, pulse or not (the
// pedestal), for draw_charge(); shifted per input by the db time offsets (db/v1742); not set = none kept
void set_charge_window(double from_ns, double to_ns);
void save_events(const char *filename = "cluster_events.root");

// --- stage 2: clusters from the saved events (seconds: re-run with other cuts as often as wanted)
void load_events(const char *filename = "");     // "" = the events in memory
// seed: the highest pulse, at least seed_mv, within seed_window_ns of the laser's timing peak;
// neighbours: at least neighbour_mv within range_pixels of the seed (square) and dt_ns of its time
void set_cuts(double seed_mv = 8, double neighbour_mv = 5, int range_pixels = 1, double dt_ns = 1.0, double seed_window_ns = 2.0);
void mask_channels(const char *list = "-");      // V1742 inputs to leave out, e.g. "1 4 6"; "" = none; "-" = db/testbench.conf
void make_clusters();                            // with the current cuts: sizes, pair table, histograms
void scan_thresholds(const char *mv_list = "2 2.5 3 3.5 4 5 6 8");   // neighbours per cluster vs neighbour threshold (>= the time threshold)
void draw_clusters();                            // the cluster canvas
void draw_examples(int n = 12, int min_size = 2);   // waveforms of example clusters (from the saved traces)
// every other pixel around each seed, by distance from the seed: how often it has a pulse >= the
// neighbour threshold at the seed's time (against chance), and when (charge sharing: touching and
// prompt; backscattered photoelectrons: further and later; ion feedback: much later)
void draw_distance();
// charge of one photoelectron: the fixed-window charge of the spot pixels summed in every event,
// fitted with pedestal + Poisson-weighted Gaussians (lib/tb_spe.h); also each pixel alone and the
// cluster charge with the fitted photoelectron peaks
void suggest_charge_window();                    // from the saved waveforms' average pulse
void set_spot(const char *channels = "");        // pixels to sum, e.g. "20 21 22 23"; "" = pixels with >= 10 % of the busiest one's laser pulses
void draw_charge();
void save_plots(const char *prefix = "clusters");    // <prefix>_clusters.png, _examples.png, _distance.png, _charge.png

#endif /* __CLUSTERS_H__ */
