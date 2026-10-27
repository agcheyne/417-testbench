#ifndef __GENERAL_H__
#define __GENERAL_H__

#include <pmonitor/pmonitor.h>
#include <Event/Event.h>
#include <Event/EventTypes.h>

int process_event (Event *e); //++CINT

// --- running ---------------------------------------------------------------------------------
void online();                  // rcdaqopen() + pstart() + draw(): the live monitor
void draw();                    // the main canvas (refreshed every 2 s)
void draw_details();            // face, clusters and waveforms canvases
void draw_noise();              // noise and trigger canvas (laser-off checks)
void reset();                   // clear every histogram (also done at each new run); the trends go on
void status();                  // events, rate, clusters, settings, group time offsets
void save_plots(const char *prefix = "monitor");   // <prefix>_main / _face / _clusters / _waveforms / _noise .png

// --- settings (any time) -------------------------------------------------------------------
void watch_channel(int ch = -1);                                  // channel on the main canvas; -1 = follow the laser
void set_signal_window(double from_ns = 17, double to_ns = 35);   // relative to the group trigger
void set_threshold(double mv = 5.0);                              // a hit
void set_charge_window(double from_ns, double to_ns);             // default: -2 .. +4 ns around each pulse
// seed: the highest pulse, at least seed_mv; neighbours: hits of at least neighbour_mv (and the hit
// threshold) within range_pixels of the seed (a square: 1 = the 8 around it) and within dt_ns of its time
void set_cluster(double seed_mv = 8, double neighbour_mv = 5, int range_pixels = 1, double dt_ns = 1.0);
void set_waveform_mode(const char *mode = "latest");   // "latest" event, or the "average" of each channel's hits
void set_waveform_range(double min_mv = -100, double max_mv = 20);
void set_waveform_time(double from_ns = -10, double to_ns = 55);   // default: -10 ns .. signal window + 20 ns
void set_reset_each_run(bool on = true);

#endif /* __GENERAL_H__ */
