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
void draw_fits();               // the two results: timing fit and single-photoelectron fit of the watched channel
void draw_noise();              // noise and trigger canvas (laser-off checks), frequency spectrum of the noise
void reset();                   // clear the histograms (also at each new run); trends and group alignment go on
void status();                  // events, rate, clusters, settings, group time offsets
void save_plots(const char *prefix = "monitor");   // <prefix>_main / _face / _clusters / _waveforms / _noise (/ _fits) .png

// --- settings (any time) -------------------------------------------------------------------
void watch_channel(int ch = -1);                                  // channel on the main canvas; -1 = follow the laser
void set_signal_window(double from_ns = 17, double to_ns = 35);   // relative to the group trigger
void set_threshold(double mv = 5.0);                              // a hit
void set_charge_window(double from_ns, double to_ns);             // default: -2 .. +4 ns around each pulse
// seed: the highest pulse, at least seed_mv, within seed_window_ns of the laser's timing peak;
// neighbours: pulses of at least neighbour_mv (timed down to it, below the hit threshold too) within range_pixels of the seed
// (a square: 1 = the 8 around it) and within dt_ns of its time
void set_cluster(double seed_mv = 5, double neighbour_mv = 2, int range_pixels = 1, double dt_ns = 1.0, double seed_window_ns = 2.0);
void mask_channels(const char *list = "-");   // V1742 inputs to leave out, e.g. "1 4 6"; "" = none; "-" = db/testbench.conf
void set_waveform_mode(const char *mode = "latest");   // "latest" event, or the "average" of each channel's hits
void set_waveform_range(double min_mv = -100, double max_mv = 20);
void set_waveform_time(double from_ns = -10, double to_ns = 55);   // default: -10 ns .. signal window + 20 ns
void set_reset_each_run(bool on = true);
// pulse heights for the timing pads and fit; default every hit above the hit threshold. Bigger
// pulses come later (CFD walk), so a narrow window gives the sharper peak for a closer look
// (Photonis at 2100 V: 5-12.5 mV); (0, 0) = every hit again
void set_timing_amplitude(double min_mv = 5, double max_mv = 12.5);
void fft_channel(int ch = -1);
// spot charge (clusters canvas): the watched channel and its neighbours summed in every event, in a
// window around each group's laser peak, with the single-photoelectron fit (lib/tb_spe.h)
void set_spot_window(double before_ns = 0.8, double after_ns = 2.0);                 // channel in the noise canvas's frequency spectrum; -1 = the noisiest

#endif /* __GENERAL_H__ */
