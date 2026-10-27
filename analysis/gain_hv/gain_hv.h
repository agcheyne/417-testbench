#ifndef __GAIN_HV_H__
#define __GAIN_HV_H__

#include <pmonitor/pmonitor.h>
#include <Event/Event.h>
#include <Event/EventTypes.h>

int process_event (Event *e); //++CINT

// --- settings (before add_run) ---------------------------------------------------------
// charge window [ns] relative to the group trigger: the same in every event, so the pedestal
// is in the spectrum (find it with general's suggest_windows())
void set_charge_window(double from_ns, double to_ns);
void set_events(int n = 20000);                 // events read per run (0 = all)
// which HV the gain is plotted against: an electrode of the detector's hv.conf ("" = the
// only cabled one), or a CAEN unit (board serial) and channel directly
void set_hv_electrode(const char *electrode = "");
void set_hv_channel(int unit_serial, int ch);
void set_charge_range(double lo_pC = -0.2, double hi_pC = 2.0, int nbins = 1100);

// --- runs ------------------------------------------------------------------------------------
// read one run (all its parts if you give part -0000); hv_volts overrides the 941 readback
void add_run(const char *filename, double hv_volts = -1);
// a text file with one "filename [hv_volts]" per line
void add_runs(const char *listfile);

// --- results ---------------------------------------------------------------------------------
void fit_all(int min_entries = 2000);           // SPE fit of every run x channel, table printed
void gain_curve(int ch);                        // gain vs HV of one channel, exponential fit, drawn
double working_point(int ch, double target_gain = 1e6);   // HV for a gain, from the fit
void draw_spectrum(int run, int ch);            // one charge spectrum with its fit
void save(const char *prefix = "gain_hv");      // <prefix>.csv (table) + <prefix>.root

#endif /* __GAIN_HV_H__ */
