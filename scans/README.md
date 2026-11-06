# Scans

Each scan steps one thing and takes one rcdaq run at every step:

| Script | Steps | Typical use |
| --- | --- | --- |
| `scan_xy.sh` | the laser spot over the detector (XY stage) | gain and timing maps, a line across a pad boundary |
| `scan_hv.sh` | the voltage of one CAEN HV channel | gain against HV |

The lamp spectrum (`bin/lamp_scan.sh`) is not an rcdaq scan. See `bin/`.

Every script prints its full help with `-h`.

## Before a scan

1. **Detector and DAQ profile** selected: `bin/testbench.sh show` (change with `bin/testbench.sh use detector NAME`).
2. **DAQ** set up and idle: the `rcdaq-server` service does this when it starts. If in doubt,
   `systemctl --user restart rcdaq-server`.
3. **HV on** and at its setpoint (bench panel, or `bin/testbench.sh hv-on --box-safe`).
4. **Laser on** (`bin/pilas.sh on --box-safe`).

The scan checks all four and refuses to start if one is missing. For dark-count runs on purpose, add `--dark`.

## XY scans

Say *where* on the detector, and how many events per point (`-n`) or seconds per point (`-t`).
Pads are numbered by row and column, as on the panel's laser face (hover over a pad to see its
row, column and V1742 input):

```bash
scans/scan_xy.sh --pad 5,6 --step 0.4 -n 20000 --dry-run       # one pad, 0.4 mm grid
scans/scan_xy.sh --channel 20 --step 0.4 -n 20000 --dry-run    # the pad V1742 input 20 reads
scans/scan_xy.sh --rows 2-5 --step 1 -n 20000 --dry-run        # a block of rows, 1 mm grid
scans/scan_xy.sh --detector -n 2000 --dry-run                  # the centre of every pad
```

Without `--step` the scan visits the centre of each pad. With `--step` it covers the pads edge to
edge with a grid centred on them. Positions in stage millimetres still work (`-x 58.6:71.6:0.1 -y 83.9`,
or `--points FILE`).

`--dry-run` prints every point and checks them against the stage limits; nothing moves. Drop it
to start: the scan shows the plan once more and asks before it begins (`--yes` skips the question).

## HV scans

The channel must already be on and at its setpoint; the scan only changes the voltage and puts it
back at the end.

```bash
scans/scan_hv.sh -v 1950:2250:50 -n 20000 --dry-run            # 1950 V to 2250 V in 50 V steps
```

Use low light (about 0.1-1 photoelectrons per pulse), so the single-photoelectron peak shows.

## While it runs

- **Progress:** the bench panel's scan box, or `tail -f logs/scans/<scan>/<scan>.csv`.
- **Stop after the current point:** `scans/scan_xy.sh --quit` (either script's `--quit` does it).
- **Stop now:** Ctrl-C in the scan's terminal. The run ends and the motors stop where they are.

However the scan ends, the laser is switched off (unless `--keep-laser`), and the HV scan puts the
channel back to its starting voltage.

## Afterwards

Each scan gets its own folder, `logs/scans/<date>_<time>_<name>/`, with one CSV line per point
(run number, position or voltage read back) and a `.txt` with the parameters and the command. The
scan prints the next analysis step when it ends:

- XY: `analysis/gain_hv/fit_gain_map.sh logs/scans/<scan>` (gain map),
  `analysis/time_xy/time_xy.sh logs/scans/<scan>` (timing)
- HV: `analysis/gain_hv/fit_hv_scan.sh logs/scans/<scan>`

Analysis output goes into the same folder.

Shared code of both scripts: `lib/tb_scan.sh`.
