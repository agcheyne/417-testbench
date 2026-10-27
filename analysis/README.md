# Analyses

Each folder is a pmonitor project (started with `writePmonProject.sh <name>`), built with `make`
and started with `./<name>.sh [file.evt]`. They all use the same event preparation and helpers
from `../lib/`, so a channel is measured the same way in every study.

| Project | What it gives | Typical use |
| --- | --- | --- |
| `general/` | Per channel: amplitude, charge, CFD time, late pulses, occupancy (per channel and per pixel), SPE gain fit; `suggest_windows()` finds the signal window | First look at any run; choose the windows for the others |
| `gain_hv/` | SPE gain and mean photoelectrons per run and channel, gain vs HV (exponential fit), working point for a target gain | An HV series at low light (mu ~ 0.1-1) |
| `timing_xtalk/` | CFD core sigma and late-tail fraction; leading-edge time-walk fit and corrected sigma; crosstalk: average induced pulse in every channel around clean single-channel pulses, source x receiver matrix | Any laser run; crosstalk needs light mostly in one pixel |
| `scan_map/` | From an XY scan: occupancy maps per channel, best-channel map, channel centroids, fit of the stage position of pixel (0,0), pitch, mirror / row-column swap, and channels that are not where the map says | Checking a detector's mapping and position after a `scan_xy.sh` scan |

## Shared code (`../lib/`)

| File | Contents |
| --- | --- |
| `tb_v1742.h` | `V1742Event`: DRS4 calibration (picked from the data), baseline, despike, group trigger times. `RunInfo`: begin-run packets (detector 910, scan point 911, serial 921, stage 932/931, HV 941). `measure()`: amplitude, charge, CFD and leading-edge time of one channel with `Cuts`. |
| `tb_pulse.h` | Waveform helpers: median, rms, CFD, crossing time, charge, pulse finding, despike. No ROOT. |
| `tb_spe.h` | Single-photoelectron fit (pedestal + Poisson x Gauss), gain = Q1 / e, `resolved` = Q1 > 3 pedestal sigmas. |
| `tb_fit.h` | Timing core sigma (Gaussian over +-2 sigma) and tail fraction. |
| `tb_db.h` | The test-bench db: detector settings, slot/channel -> pixel through mapping + cabling, electrode -> CAEN channel. |

## Notes that apply to all of them

- **Times are relative to the channel's own DRS4 group trigger** (TR trace of its 8-channel
  group). The two DRS4 boards (groups 0/1 and 2/3) are not aligned (3.4 ns rms), so never
  compare raw times across them.
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
