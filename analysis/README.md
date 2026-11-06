# Analyses

Each folder is a pmonitor project (started with `writePmonProject.sh <name>`), built with `make`
and started with `./<name>.sh [file.evt]`. They all use the same event preparation and helpers
from `../lib/`, so a channel is measured the same way in every study.

| Project | What it gives | Typical use |
| --- | --- | --- |
| `general/` | Per channel: amplitude, charge, CFD time, late pulses, occupancy (per channel and per pixel), SPE gain fit; `suggest_windows()` finds the signal window | First look at any run; choose the windows for the others |
| `gain_hv/` | SPE gain and mean photoelectrons per run and channel, gain vs HV (exponential fit), working point for a target gain | An HV series at low light (mu ~ 0.1-1); `bin/scan_hv.sh` takes one; `fit_hv_scan.sh <scan folder>` fits and plots it. An xy scan with ~20000 events per point: `fit_gain_map.sh <scan folder>` maps the gain (parallel jobs) |
| `timing_xtalk/` | CFD core sigma and late-tail fraction; leading-edge time-walk fit and corrected sigma; crosstalk: average induced pulse in every channel around clean single-channel pulses, source x receiver matrix | Any laser run; crosstalk needs light mostly in one pixel |
| `scan_map/` | From an XY scan: occupancy maps per channel, best-channel map, channel centroids, fit of the stage position of pixel (0,0), pitch, mirror / row-column swap, and channels that are not where the map says | Checking a detector's mapping and position after a `scan_xy.sh` scan |
| `clusters/` | Two stages: one pass saves every channel's pulse height, charge and time per event (plus some waveforms); then clusters with any cuts in seconds: sizes, centroids, charge sharing, 2-pixel pairs against chance, neighbour-threshold scan (diagonals as the control), example waveforms, distance/time of other pixels' pulses; with a fixed charge window, the single-photoelectron fit of the spot pixels' summed charge (shared photoelectrons counted whole) | Charge sharing and cluster cuts; SPE charge and gain with the laser on a pixel edge or corner |
| `lamp_scan/` | Not a pmonitor project (no `.evt` data): `plot_lamp_scan.sh [scan]` plots a `bin/lamp_scan.sh` scan with ROOT, light − dark against wavelength and the darks before and after the scan | After every lamp scan; the newest one by default |
| `time_xy/` | Not a pmonitor project: `time_xy.sh <scan> --pad CH` reads every point's hit times (parts on /scratch2) and plots, for the input alone and with its 8 neighbours (from the mapping and the run's own cabling): x-y maps or x curves in time slices relative to the laser peak (coarse, 0.25 ns, 0.1 ns, 50 ps), time vs x / y along the pad, and a map of where the peak arrives first (median hit time and spread per point) | Which part of the time spectrum follows the light spot (direct peak, halo, shoulder, late bumps); arrival time across a pad. Pad grid from the db now (`--x0/--y0` for an older scan) |
| `check_align/` | Not a pmonitor project: `check_align.sh RUN ...` reads only the group trigger time tags and gives, per run, the % of events where both DRS4 boards (inputs 0-15 and 16-31) took the same trigger, and how many of the rest have groups 2/3 on the next event's pulse; `--min PCT` sets the exit status | Any run above ~1.5 kHz before summing across the two halves; a scan's background check |
| `time_offsets/` | From an XY scan that lights every pixel: each V1742 input's laser peak (CFD time - group trigger), the laser drift per run taken out, offsets relative to groups 2/3, written to `db/v1742/time_offsets_<serial>_<speed>.conf` | After any change of cables, trigger fan-out or sampling speed |

## Shared code (`../lib/`)

| File | Contents |
| --- | --- |
| `tb_v1742.h` | `V1742Event`: DRS4 calibration (picked from the data), baseline, despike, group trigger times (line fit over the TR edge). `TimeOffsets`: the inputs' time offsets from `db/v1742/` (same serial/speed lookup as the calibration). `RunInfo`: begin-run packets (detector 910, scan point 911, serial 921, stage 932/931, HV 941). `measure()`: amplitude, charge, CFD and leading-edge time of one channel with `Cuts`. |
| `tb_pulse.h` | Waveform helpers: median, rms, CFD, edge-fit time, crossing time, charge, pulse finding, despike. No ROOT. |
| `tb_spe.h` | Single-photoelectron fit (pedestal + Poisson x Gauss), gain = Q1 / e, `resolved` = Q1 > 3 pedestal sigmas. |
| `tb_fit.h` | Timing core sigma (Gaussian over +-2 sigma) and tail fraction. |
| `tb_db.h` | The test-bench db: detector settings, slot/channel -> pixel through mapping + cabling, electrode -> CAEN channel. |
| `tb_ydown.h` | x-y maps drawn as the bench face, stage y down, (0,0) at the top left: `draw_y_down(h, opt)` draws a copy at -y with the labels as +y, `y_down(y)` places what goes over it. Used by `scan_map`, the gain maps and the `time_xy` maps. |

## Notes that apply to all of them

- **Times are relative to the channel's own DRS4 group trigger** (TR trace of its 8-channel
  group). The two DRS4 boards (groups 0/1 and 2/3) are not aligned (3.4 ns rms), so never
  compare raw times across them. Subtract the db time offsets (`tb::TimeOffsets`, from
  `time_offsets/`) to put every input on one axis, as `clusters` and the online monitor do.
- **Windows** are in ns after the group trigger. Run `general` with `suggest_windows()` first;
  for run 70 (5 GS/s) they were `set_signal_window(17, 35)` and `set_charge_window(21, 30)`.
- **Gain fits need low light**: the pedestal must be in the spectrum. Fits with mu > 3 or
  without a resolved photoelectron peak are flagged and not used.
- **Pixels** need the detector's `mapping.txt` and `cabling.txt` in the db; without the
  cabling, results are per channel only.

## A new study

```bash
mkdir analysis/mystudy && cd analysis/mystudy && writePmonProject.sh mystudy
```

Then, in `mystudy.Makefile`, replace the `CXXFLAGS = ...` line as in the other projects:

```make
TB_ROOT_DIR = $(abspath ../..)
CXXFLAGS = -I. -I$(TB_ROOT_DIR)/lib -DTB_ROOT_DIR=\"$(TB_ROOT_DIR)\" $(ROOTFLAGS) -I$(ONLINE_MAIN)/include -I$(OFFLINE_MAIN)/include
```

and start `process_event` from the pattern in `../lib/tb_v1742.h`.
