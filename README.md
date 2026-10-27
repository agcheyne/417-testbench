# 417-testbench

Control, DAQ and first-look analysis for the photodetector test bench in room 417 (Glasgow NHP):
MCP-PMTs, HRPPDs and similar detectors, a PiLas laser on a Thorlabs XY stage, CAEN N1470 HV,
a CAEN V1742 digitiser read out with rcdaq, and the QE bench (Keithley 6487, Newport power meter).

This is intended to be a more general improvement to the `~/hpDIRC` (which stays in use until the switch, see below) and the old `~/MCP_Scan` Wavedump based analysis.

## Layout

| Folder | What is there |
| --- | --- |
| `bin/` | Instrument and bench commands. `-h` for help on any script. |
| `daq/` | rcdaq setup (`setup_rcdaq.sh`) and the begin-run readback scripts. |
| `db/` | The database, plain text in git: site settings, one folder per detector, DAQ profiles. |
| `lib/` | Shared code for use in analysis: `tb.sh` for the scripts; `tb_pulse.h`, `tb_spe.h`, `tb_db.h` for methods. |
| `analysis/` | pmonitor analysis projects, one folder each (`general/` to start). |
| `online_monitor/` | pmonitor analysis projects, focused on live-monitoring. |
| `logs/`, `state/` | Written at run time, not in git: HV logs, scan logs, the current selection. |

See `Lab_instruments_Cheat_Sheet.md` for day-to-day use of every instrument.

## The database (`db/`)

- `testbench.conf`: data and calibration paths, stage limits and rest position, scan notification.
- `detectors/<name>/`: one folder per detector (`testbench.sh new NAME` copies `_template`).
  - `detector.conf`: type, serial, rcdaq run type, anode geometry, where it sits under the stage, HV supply.
  - `hv.conf`: one row per electrode: CAEN unit/channel and VSET/ISET/RUP/RDW/MAXV.
    `n1470.sh` shows these names next to the channels; `testbench.sh hv-load` sets the values.
  - `mapping.txt`: digitiser slot + channel to anode pixel (the detector's own map).
  - `cabling.txt`: which rcdaq packet reads which slot on this bench.
- `daq/<profile>.conf`: readout settings: V1742 sampling rate, record length, trigger; or the scope.
- `v1742/time_offsets_<serial>_<5G|2G|1G>.conf`: CFD time offset of every V1742 input (the two
  DRS4 boards differ by ~2.3 ns, from their TR cabling; unmeasured inputs take their board's mean), measured with `analysis/time_offsets` from a scan that lights every
  pixel; the online monitor and the analyses take it off every time. Measure again after re-cabling.

```bash
bin/testbench.sh list
```

```bash
bin/testbench.sh use detector HRPPD_tile66
```

```bash
bin/testbench.sh use daq v1742_5G
```

```bash
bin/testbench.sh check
```

The selection is stored in `state/active.conf`. `daq/setup_rcdaq.sh` applies the DAQ profile, and
every run records the detector, profile and scan point in begin-run packets 910/911, so the
analysis knows what the data is without a run sheet.

## Taking data

- `daq/setup_rcdaq.sh` (try `--dry-run` first) configures rcdaq and the V1742 from the selected profile.
- `bin/scan_xy.sh` steps the stage over a grid and takes one rcdaq run per point
  (`--dry-run` prints the plan; `scan_xy.sh --quit` stops after the current point).
- `bin/motor.sh` moves and reads the XY stage.

## Analysis

pmonitor projects in `analysis/`, one folder per study, all on the shared event preparation in
`lib/` (see `analysis/README.md`):

- `general`: first look at a run (amplitude, charge, timing, late pulses, occupancy, gain), and `suggest_windows()`
- `gain_hv`: gain vs HV from single-photoelectron fits, working point
- `timing_xtalk`: timing resolution, time walk, crosstalk matrix
- `scan_map`: response maps from an XY scan, and a check of the detector's mapping and position
- `clusters`: cluster sizes, charge sharing between pixels, and where to put the cluster thresholds, and the single-photoelectron charge of the summed spot pixels
- `time_offsets`: the time offset of every V1742 input from an XY scan, written to `db/v1742/`

```bash
cd analysis/general && make && ./general.sh /scratch2/data/hpDIRC/HRPPD/HRPPD-00000123-0000.evt
```
## Online monitor

`online_monitor/general` shows the V1742 data live: mean pulse height and hit rate per pixel on
the detector face, clusters (seed + in-time neighbours: centroid, size, summed charge, time) and
the waveforms of each DRS4 group. Settings and commands are at the top of `general.cc`.

```bash
cd online_monitor/general && make && ./general.sh
```

then `online()` at the ROOT prompt (or `./general.sh FILE.evt`, then `pstart() ; draw()`).
