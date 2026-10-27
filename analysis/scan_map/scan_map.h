#ifndef __SCAN_MAP_H__
#define __SCAN_MAP_H__

#include <pmonitor/pmonitor.h>
#include <Event/Event.h>
#include <Event/EventTypes.h>

int process_event (Event *e); //++CINT

// --- stage 1: one row per (scan point, channel) --------------------------------------------
void set_signal_window(double from_ns, double to_ns);   // relative to the group trigger
void set_threshold(double mv = 5.0);
void set_charge_window(double from_ns, double to_ns);   // default: around each pulse
void set_events(int n = 5000);                          // events per point (0 = all)
// a scan_xy.sh log (logs/scans/<date>_<name>.csv): every point's run, found under DATA_ROOT
void add_scan(const char *csv);
// one run; x_mm / y_mm only used if the run has no position packets (932 / 911)
void add_run(const char *filename, int point = -1, double x_mm = -1, double y_mm = -1);
void save_points(const char *filename = "scan_points.root");

// --- stage 2: maps and geometry, from the points (in memory, or a saved file) ---------------
// per-channel response maps, best-channel map, channel centroids, and a fit of
// centroid = offset + pitch * (col, row) against the detector's mapping in the db:
// gives the stage position of pixel (0,0), the pitch, mirror / row-column swap, and
// channels that sit somewhere other than the map says. detector "" = the one in the data.
void make_maps(const char *points_file = "", const char *detector = "");
void draw_channel_map(int ch);
void save_maps(const char *filename = "scan_maps.root");
// PNGs: <prefix>_overview.png (best channel per point with the map's pixel grid; centroids against
// where the map puts each channel) and <prefix>_channels.png (each lit channel's map and its pixel).
// x0_mm / y0_mm: draw the grid with pixel (0,0) there instead of the db's STAGE_X0_MM / STAGE_Y0_MM
void save_plots(const char *prefix = "scan", double x0_mm = NAN, double y0_mm = NAN);

// --- testing without hardware -------------------------------------------------------------
// a synthetic scan of one readout slot: laser spot (sigma_mm) stepped over the slot's pixels,
// response = 1 - exp(-mu * spot fraction on the pixel); the detector placed with pixel (0,0)
// at (x0_mm, y0_mm) and mirrored in x if mirror_x. Written as a points file for make_maps().
void simulate_scan(const char *filename, const char *detector, int slot, double x0_mm, double y0_mm,
                   bool mirror_x = false, double step_mm = 0.5, double sigma_mm = 0.3, double mu = 2.0);

#endif /* __SCAN_MAP_H__ */
