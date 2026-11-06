# Online monitor cheat sheet

Two monitors, each in its own folder. Both run in ROOT and redraw every 2 s.

Theses same scipts can also be ran on .evt files.

| Monitor | Shows |
| --- | --- |
| `general/` | Hit rate per pixel, the watched channel (persistence, charge, timing), clusters, noise |
| `waveforms/` | Every DRS4 chip of every V1742 (1 or 2 boards) in one canvas, one event at a time |

To make a new monitor project, make a new directory and call: \
`analysis/new_pmonitor_project.sh online_monitor/NAME` (letters, digits and _ only).

## Start

```bash
cd ~/417-testbench/online_monitor/waveforms     # or general
make                                            # recompile after any change
./waveforms.sh                                  # live
./waveforms.sh /scratch2/data/hpDIRC/photonis/photonis-00007703-0000.evt   # a file
```

Then in ROOT:

| Live | From a file |
| --- | --- |
| `online()` | `next()` one event, `next(10)` ten on, or `pstart() ; draw()` to run through |

After `make`, quit ROOT (`.q`) and start again: a running session keeps the old library.

## waveforms

| Command | Does |
| --- | --- |
| `set_laser_aligned(true)` | Time relative to each chip's trigger (default); `false` = the trace's own time |
| `set_pulse_only(5)` | Only events with a pulse below −5 mV; `0` = every event |
| `persistence(20,0)` | New canvas: every sample of input 20 (board 0); `persistence(5, 1)` = board 1 |
| `reset_persistence()` | Start the persistence again |
| `set_range(-200, 20)` | mV axis |
| `set_time(-10, 40)` | ns axis; `set_time(0, 0)` = whole trace |
| `save_plot("run7703")` | `run7703.png` (and `run7703_persistence.png`) |

## general

| Command | Does |
| --- | --- |
| `draw_details()` | Face, clusters and waveform canvases |
| `draw_noise()` / `draw_fits()` | Noise and trigger checks / timing and photoelectron fits |
| `watch_channel(20)` | Channel on the main canvas; `-1` = follow the laser |
| `set_signal_window(17, 35)` | ns after the group trigger |
| `set_threshold(5)` | mV for a hit |
| `set_waveform_mode("average")` | Or `"latest"` |
| `reset()` / `status()` | Clear the histograms / print rates and settings |
| `save_plots("run7703")` | Every canvas as PNG |

## When something looks wrong

| Symptom | Cause / fix |
| --- | --- |
| A board's traces are noisy, `s/n -1` in the titles | Old library: quit ROOT, `make`, start again |
| "no … to take the begin-run event from" | The run isn't logged to disk (rcdaq logging closed): the boards get no calibration |
| "no new rcdaq stream opened" | Already open in this session, or `rcdaq-server` is down (`systemctl --user status rcdaq-server`) |

