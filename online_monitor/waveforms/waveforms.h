#ifndef __WAVEFORMS_H__
#define __WAVEFORMS_H__

#include <pmonitor/pmonitor.h>
#include <Event/Event.h>
#include <Event/EventTypes.h>

int process_event (Event *e); //++CINT

void online();                  // rcdaqopen() + pstart() + draw(): the live view, refreshed every 2 s
void draw();                    // the canvas: one pad per DRS4 chip of every V1742 board
void next(int n = 1);           // from a file: process n more events and show the last
void save_plot(const char *name = "waveforms");   // <name>.png (and <name>_persistence.png)
// persistence of one input on its own canvas: every sample of every event from now on (with
// set_pulse_only, only events where this input has a pulse), on the waveforms' time axis
void persistence(int ch, int board = 0);
void reset_persistence();

// time axis: true = relative to each chip's trigger (TR edge), minus the input's db time offset when
// db/v1742 has one for the board, so the laser pulses line up; false = the trace's own time axis
void set_laser_aligned(bool on = true);
// only events with a pulse below -mv in some input (most are empty at low light); 0 = every event
void set_pulse_only(double mv = 5);
void set_range(double min_mv = -100, double max_mv = 20);
void set_time(double from_ns = 0, double to_ns = 0);   // (0, 0) = the whole trace

#endif /* __WAVEFORMS_H__ */
