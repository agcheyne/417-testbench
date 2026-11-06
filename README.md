# 417-testbench

Control, DAQ and first-look analysis for the photodetector test bench in room 417 (Glasgow NHP):
MCP-PMTs, HRPPDs and similar detectors, a PiLas laser on a Thorlabs XY stage, CAEN N1470 HV,
a CAEN V1742 digitiser read out with rcdaq, and the QE bench (Keithley 6487, Newport power meter).

It replaces `~/hpDIRC` (switched over 5 Nov 2026; kept only as an archive; its October HV alarms are at the start of `logs/hv_alarms.log`) and the old `~/MCP_Scan` Wavedump based analysis. The students' analysis kit is a separate repository in `~/student_kit`.

## Layout

| Folder | What is there |
| --- | --- |
| `bin/` | Instrument and bench commands. `-h` for help on any script. |
| `daq/` | rcdaq setup (`setup_rcdaq.sh`) and the begin-run readback scripts. |
| `scans/` | The rcdaq scans: `scan_xy.sh` (stage, by pad or in mm) and `scan_hv.sh` (one HV channel). Start with `scans/README.md`. |
| `db/` | The database, plain text in git: site settings, one folder per detector, DAQ profiles. |
| `lib/` | Shared code for use in analysis: `tb.sh` for the scripts, `tb_scan.sh` for the scans; `tb_pulse.h`, `tb_spe.h`, `tb_db.h` for methods. |
| `analysis/` | pmonitor analysis projects, one folder each (`general/` to start). |
| `online_monitor/` | Live monitors (`general/`, `waveforms/`). Start with `online_monitor/Online_Monitor_Cheat_Sheet.md`. |
| `logs/`, `state/` | Written at run time, not in git: HV logs, scan logs, the current selection. |

See `Lab_instruments_Cheat_Sheet.md` for day-to-day use of every instrument. Though most functionality can be called in the DAQ or done by a scan script.

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
bin/testbench.sh use detector HRPPD_26
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

- `daq/setup_rcdaq.sh` (try `--dry-run` first) configures rcdaq and the V1742s from the selected profile: board 0, and board 1 too when the detector's `cabling.txt` has a packet 2001 line.
- `scans/scan_xy.sh` steps the stage over pads or a grid and takes one rcdaq run per point
  (`--dry-run` prints the plan; `scan_xy.sh --quit` stops after the current point). With enough
  events per point (~20000 at low light), `analysis/gain_hv/fit_gain_map.sh logs/scans/<scan>` fits
  the gain of every channel at every point in parallel jobs and maps it.
- `scans/scan_hv.sh` steps one CAEN channel through a list of voltages (it must already be on) and
  takes one run per point. Acquisition only: fit and plot the gain afterwards with
  `analysis/gain_hv/fit_hv_scan.sh logs/scans/<scan>` (`--spot` sums a group of pixels).
- `bin/lamp_scan.sh FROM TO STEP` measures the lamp through the monochromator on the power meter,
  with darks before and after (grating parked, nothing touched), interpolated in time; plot it with
  `analysis/lamp_scan/plot_lamp_scan.sh`.
- Every scan gets its own folder, `logs/scans/<date>_<time>_<name>/`: the scan's CSV and `.txt`, and
  whatever analysis output belongs to it. Plots and files from single-run studies go in
  `logs/runs/run<N>/`. The HV logs and `testbench.log` stay at the top of `logs/`.
- What a run is lives in its begin-run packets (910/911: detector, HV table, cabling, DAQ profile,
  scan id, point, target and the scan's command line; 930/931 stage; 940/941 HV; 950/951 laser), so
  `logs/` keeps only what the data cannot: the scan's CSV as its run index (and the points that
  failed), HV alarms and trips (`hv_alarms.log`), bench actions
  (`testbench.log`, `daq_commands.log`: one line per command, the full output only when it
  failed; the last good rcdaq setup's output is in `setup_rcdaq_last.log`), and the analysis
  results. The fits' working files (`<scan>_gain_map_parts/`, `<scan>_live_parts/`) are removed
  once the fit is done (`--keep-parts` keeps them); everything in them can be fitted again from the `.evt` files.
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
- `lamp_scan`: not a pmonitor project; a ROOT macro that plots a `bin/lamp_scan.sh` scan

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
