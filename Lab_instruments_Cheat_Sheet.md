# Room 417 Test Bench: Instruments Cheat Sheet

Oct 23, 2026 · @Andrew George Cheyne · repository `~/417-testbench` (was `~/hpDIRC`)

## Devices at a glance

The instrument scripts live in `bin/` of this repository; run them from the repository folder (e.g. `bin/n1470.sh -u all status`). The rcdaq setup and its readback scripts are in `daq/`, the database of detectors and DAQ settings in `db/` (see below). Every script prints its full usage with `-h`. Ports are found by fixed `/dev/serial/by-id` or `by-path` names, so the `ttyUSBn` numbers can change freely.

| Device | Script | Port (USB adapter) | Serial settings | In rcdaq? |
| --- | --- | --- | --- | --- |
| CAEN N1470 HV, unit A (s/n 236) | `n1470.sh -u A` | FTDI FT232B, found by board serial | 9600 8N1, XON/XOFF | Yes, packets 940/941 |
| CAEN N1470 HV, unit B (s/n 1593) | `n1470.sh -u B` | FTDI FT232B, found by board serial | 9600 8N1, XON/XOFF | Yes, packets 940/941 |
| Keithley 6487 picoammeter + V-source (s/n 4686018) | `keithley6487.sh` | Prolific PL2303 `DGBCb2A9709` | 9600 8N1, CR, XON/XOFF (instrument must be set to RS-232) | No, by choice |
| PiLas 405 nm laser (PLC0405CW0F00001279) | `pilas.sh` | FTDI FT232R `BG01VUCO` | 19200 8N1, CR, no flow control | Yes, packets 950/951 |
| Newport 1830-R power meter | `newport_powermeter.sh` | Native USB `104d:cec7` | USB (libusb) | No |
| Thorlabs APT stepper (X/Y motors) | `motor.sh` (EPICS `THOR_X` / `THOR_Y`) | FTDI `0403:faf0` | via EPICS IOC + `thor_epics` | Yes, packets 930/931/932 |
| Tektronix MSO64B oscilloscope | none yet (by hand) | LAN 192.168.0.125 | socket server port 4000 | No |
| JY H20 UV monochromator | `monochromator.sh` | NI GPIB-USB-HS `3923:709b` → `/dev/gpib0`, GPIB address 1 | GPIB, linux-gpib 4.3.7 | No |



## Starting the bench (systemd user services)

Everything that has to run all the time is a systemd user service of nptest. Lingering is on, so they
all start at boot, without anyone logging in. After a reboot, the only thing to do is open the panel
(`phoebus/bench_panel.sh`, in a VNC desktop or on the lab PC's screen).

| Service | What it runs | Log |
| --- | --- | --- |
| `ioc-n1470` | EPICS IOC of both CAEN N1470 units (CA port 5070) | journal |
| `ioc-pilas` | EPICS IOC of the PiLas laser (CA port 5072) | journal |
| `ioc-thorlabs` | EPICS IOC of the XY stage, `THOR_X` / `THOR_Y` (CA port 5064) | `logs/services/ioc-thorlabs.log` |
| `thor-epics` | `~/v1/thor_epics -e`: the APT controller over USB, moves the stage | `logs/services/thor-epics.log` |
| `rcdaq-server` | `rcdaq_server`, configured at its start by `daq/setup_rcdaq.sh` for the active detector and DAQ profile | `~/rcdaq.log`, setup output in `logs/daq_commands.log` |
| `bench-publisher` | DAQ and scan status for the panel, the panel's DAQ commands, HV channel names and limits | journal |
| `hv-annunciator` | the HV alarms (`n1470.sh annunciate`): logs, sound, speech, pop-ups | `logs/hv_alarms.log` |

```bash
systemctl --user list-units 'ioc-*' thor-epics rcdaq-server bench-publisher hv-annunciator
```

- **Restart one:** `systemctl --user restart NAME`; stop / start likewise. Stopping `rcdaq-server` ends a run
  that is going first. Stopping `thor-epics` stops it through its `stop` file.
- **GECO2020 or the PiLas GUI** need the port: `systemctl --user stop ioc-n1470` (or `ioc-pilas`) first, `start`
  it afterwards. The scripts (`n1470.sh`, `pilas.sh`) use the serial port by themselves while an IOC is stopped.
- **Not services:** the VNC desktops `:2` and `:3` (started by hand with `vncserver`; after a reboot they have to
  be started again to see the panel remotely), and `caRepeater` (CA clients start it themselves).
- **`thor-epics` at start** moves nothing and does not home (a start-up bug that could drive Y to 0 was fixed on 1 Nov
  2026). The controller keeps its position and "homed" while it stays powered; after it was switched off, home by hand
  with the box safe: `bin/motor.sh home both --yes`, then `bin/motor.sh status` says `homed enabled`.
- Details, and the IOCs' design: `docs/epics_plan.md`; the panel and the alarms: `docs/phoebus_panel_plan.md`.

## Detectors and DAQ profiles (`db/`, `testbench.sh`)

The bench can hold any MCP-PMT / HRPPD. What changes per detector, and per kind of measurement, is kept as plain-text files in `db/` (in git), and one detector and one DAQ profile are "in use":

| Command | What it does |
| --- | --- |
| `bin/testbench.sh list` | Detectors and DAQ profiles in the db; `*` marks the ones in use |
| `bin/testbench.sh use detector HRPPD_tile66` | Select the detector: rcdaq run type, HV electrode names, pixel map |
| `bin/testbench.sh use daq v1742_1G` | Select the readout profile; then run `daq/setup_rcdaq.sh` to apply it |
| `bin/testbench.sh show` | The selection and all its settings |
| `bin/testbench.sh check` | Check every db file (run after editing one) |
| `bin/testbench.sh new NAME` | New detector folder from `db/detectors/_template` |
| `bin/testbench.sh hv-show` | `hv.conf` next to what the CAEN units hold now |
| `bin/testbench.sh hv-load` | Print the VSET/ISET/RUP/RDW/MAXV changes needed to match `hv.conf`; `--yes` sets them. Never switches a channel on, refuses channels that are on |

Per detector (`db/detectors/<name>/`): `detector.conf` (type, serial, run type, rows × cols, pitch, stage position of pixel 0,0), `hv.conf` (electrode → CAEN unit/channel and setpoints), `mapping.txt` (digitiser slot + channel → pixel), `cabling.txt` (which rcdaq packet reads which slot here).

DAQ profiles (`db/daq/`):

| Profile | Window | Use |
| --- | --- | --- |
| `v1742_5G` | 205 ns at 5 GS/s | Laser timing (the standard) |
| `v1742_5G_520` | 104 ns at 5 GS/s, uncompressed | Fast scans: ~1500 events/s, 27 kB/event; late pulses beyond ~55 ns after the trigger are lost |
| `v1742_2G5` | 410 ns at 2.5 GS/s | Late pulses out to ~350 ns |
| `v1742_1G` | 1024 ns at 1 GS/s | Afterpulses / ion feedback |
| `mso64b` | – | The Tektronix scope, by hand for now (`setup_rcdaq.sh` refuses it) |

Each V1742 profile has a matching calibration in `/scratch2/data/v1742.db` (`calib_0049_5G/2G/1G.dat`); the analysis picks it from the data itself.

## rcdaq setup and begin-run packets (`daq/setup_rcdaq.sh`)

`daq/setup_rcdaq.sh` configures rcdaq and the V1742 from the selected DAQ profile (`--dry-run` prints the commands). It refuses while a run is going. Run types: `junk`, `test` and the run type of every detector in the db, written to `/scratch2/data/hpDIRC/<type>/`.

rcdaq stores runtime information in the .evt file as packets in the begin-run event (type 9). At every begin-run, the readback scripts write text and integer files to `~/anc_packets`, and rcdaq stores them. A device that can't be read writes FAILED into its text packet and never blocks the run.

| Packet | Content | Written by |
| --- | --- | --- |
| 900 | The setup script itself | `setup_rcdaq.sh` |
| 910 / 911 | Test-bench db: detector, HV table, cabling, DAQ profile, scan point / numbers | `get_testbench_info.sh` |
| 920 / 921 | V1742 digitiser info: text / numbers (s/n, firmware) | `get_v1742_info.sh` |
| 930 / 931 | Thorlabs motor positions: text / whole mm (Y, then X; fractions are cut off) | `get_motor_xy_info.sh` |
| 932 | Thorlabs motor positions, X then Y \[µm\] | `get_motor_xy_info.sh` |
| 940 / 941 | Both CAEN HV units: text / numbers | `get_hv_info.sh` |
| 950 / 951 | PiLas laser: text / numbers | `get_laser_info.sh` |
| 2000 | V1742 waveform data (every event) | `device_CAENdrs` |

Packet 911, 9 integers in order: format version (1), FREQ_CODE (0 = 5 GS/s, 1 = 2.5, 2 = 1; −1 = not a V1742 profile), record length \[samples\], post-trigger \[%\], scan id (Unix time of the scan start, 0 = no scan), scan point (0-based, −1 = no scan), number of scan points, scan target X and Y \[µm\].

Packet 941: one block of 21 integers per unit read out completely, unit A first, then B. Each block is the board serial, then for channels 0–3: VSET \[0.1 V\], VMON \[0.1 V\], ISET \[0.01 µA\], IMON \[0.01 µA\], status bits.

Packet 951, 15 integers in order: controller serial (trailing digits), wavelength \[nm\], pulsed emission, CW emission, interlock, trigger source, trigger edge (1 = rising), trigger level \[mV\], tune mode (1 = auto), tune \[0.01 %\], internal frequency \[Hz\], external frequency \[Hz\], head temperature \[0.01 °C\], LD TEC on, LD temperature good.

## CAEN N1470 HV (2 units, `n1470.sh`)

Every command needs `-u A`, `-u B`, a board serial (`236`, `1593`) or `-u all` (`status`, `check`, `mon` and `off` only). Both units share one by-id name, so the script finds each one by asking every port for its board serial.

| Command | What it does |
| --- | --- |
| `bin/n1470.sh units` | List the units found and their ports |
| `bin/n1470.sh -u all status` | Board info + table of all 8 channels (VSET, VMON, ISET, IMON, ramps, MAXV, flags) |
| `bin/n1470.sh -u all check --color` | Status + ALARM/NOTE lines; raises the alarm (see HV monitoring below), exit 3 on alarm |
| `bin/n1470.sh mute` / `unmute` | Silence / restore the alarm sound; monitoring continues |
| `bin/n1470.sh test-alarm` | Are the alarms working? Fires every output once with a fake alarm (sounds in the room), then "cleared", and reports what each output did; `--quiet` = no sound; `--remote-only` = sound + spoken message on your remote desktop only |
| `bin/n1470.sh -u A mon VMON` | Raw monitor query; add a channel number (0–3) for one channel, `BD…` names for board parameters |
| `bin/n1470.sh -u A set VSET 0 1650` | Set a parameter: VSET, ISET, MAXV, RUP, RDW, TRIP, PDWN (one unit at a time) |
| `bin/n1470.sh -u A off 2` | Turn channel 2 off (`off all` = the whole unit) |
| `bin/n1470.sh -u all off all` | Turn all 8 channels off |
| `bin/n1470.sh -u A on 0 --box-safe` | Turn a channel on; refuses without the flag |

Units: V, µA, V/s. The supply ramps by itself at RUP/RDW, so the script returns straight away. MAXV reads 2400 V on all 8 channels (22 Oct).

Before powering the detector:

- Check VSET on every channel: older setpoints may still be loaded.
- Clear any `KILL` or `DIS` flags on the units; channels can't turn on with them.
- Fill in unit and channel for each electrode in the detector's `db/detectors/<name>/hv.conf`; `status` then labels each channel, and `testbench.sh hv-show` compares the setpoints.
- Close GECO2020 first: only one program can use a port.

## HV monitoring and alarms

The `hv-annunciator` service (`n1470.sh annunciate`) checks every 2 s, through the N1470 IOC, whether or not anyone is watching: it raises the alarm and writes the logs below. The conditions are the IOC's alarm records, so the bench panel shows the same (red / orange rows, the banner, the log). The old minute timer `n1470-check.timer` is disabled. To look at the values in a terminal (on the lab PC or in VNC; it only shows, the annunciator does the alarms):

```bash
watch -c -n 5 "$PWD/bin/n1470.sh -u all check --color"
```

| On an alarm | Where it shows |
| --- | --- |
| Red ALARM banner naming unit, channel and fault | The `watch` terminal |
| Desktop notification (stays until dismissed, repeats every 60 s) | Lab PC's own screen |
| Warning pop-up window | Every running VNC desktop (`:2`, `:3`) |
| Alarm sound, again and again until it is muted (panel "mute alarm sound", `bin/n1470.sh mute`) or every sounding channel is acknowledged (the panel's `ack` on that row); no speech | Room speaker, and your remote desktop if the audio tunnel is up |
| Red / orange row, alarm banner, alarm log | The bench panel (`phoebus/bench_panel.sh`) |

- **Alarms:** OVC, OVV, UNV, MAXV, TRIP, OVP, OVT, ILK on any channel, the board alarm or interlock, and no readback while the HV is in use (MAJOR); VMON more than 10 V from VSET once the channel has been on without ramping for 30 s (MAJOR); IMON above 90 % of ISET, above the channel's limit (360 uA unless `hv.conf` says otherwise), or more than 10 % above VMON / R where `hv.conf` gives R (MINOR, still sounds). `KILL` and `DIS` on a channel the detector uses show as a NOTE.
- **Silence the sound:** the panel's "mute alarm sound" button, or `bin/n1470.sh mute` (all alarms; it ends when the alarms clear, or `bin/n1470.sh unmute`); or `ack` on one row (that channel only, until its alarm clears; other channels still sound).
- **Test a channel's alarm:** the panel's `test` on that row (unlocked): a MAJOR test alarm for 30 s through the whole chain (banner, log, sound, pop-ups). `bin/n1470.sh test-alarm` still tests the outputs without the HV.
- **Only the detector's channels can be switched on:** a channel needs a row with unit and channel in the active detector's `hv.conf` (`testbench.sh use detector`); `n1470.sh on`, the panel's ON / "ON selected" and the IOC refuse all others (OFF always works). To use another channel, add its row to `hv.conf` (the IOC picks it up within 30 s). To keep a channel off in hardware, set its front-panel switch to OFF (status DIS) or KILL: the supply then refuses ON whatever the software does; HV_EN to use it again.
- **Switch several channels at once:** tick their `sel` boxes in the panel, then "ON selected" (unlocked and HV box-safe armed) or "OFF selected"; "clear" un-ticks all. The ticks are shared by every window.
- **Spoken alarm:** after the sound, `espeak-ng` says what is wrong (unit, channel or electrode, fault; the first two problems, then "and N more"). Turn it off with `ALARM_SPEAK=` (empty); voice and speed are `ALARM_VOICE` / `ALARM_SPEED`.
- **Test the alarms:** `bin/n1470.sh test-alarm` fires every output once (warn the room first; `--quiet` skips the sound) and reports which outputs worked. It affects only that one command: no HV access, and a running `watch` monitor is untouched.
- **Is the annunciator running?** `systemctl --user status hv-annunciator`; the panel's "annunciator" lamp is green and its banner grey when it is down. Any number of `watch` windows is fine: they only show.
- **Units switched off or unplugged** while their HV is off are not an alarm (one log line). A unit that disappears, or can't be read, while its HV is on IS an alarm; a port held by GECO2020 only becomes one after 5 min (`BUSY_GRACE`).
- **Logs** (in `logs/`; until the switch-over in the README, the timer still runs `~/hpDIRC/n1470.sh` and logs to `~/hpDIRC/logs/`): `hv_alarms.log` gets one line per alarm start/change/clear, HV on/off, and unit present/absent/busy; `hv_history_YYYYMMDD.csv` gets every channel once a minute, only while the HV is in use (any channel on, ramping, or VMON > 10 V). Idle days write nothing. Read with `tail -f logs/hv_alarms.log`.
- **Pause / resume the alarms:** `systemctl --user stop hv-annunciator` / `start`. Without the IOCs (e.g. both stopped for GECO2020), the old timer still works: `systemctl --user enable --now n1470-check.timer`.
- **Settings** (sound file, remote audio address, pop-ups, repeat time, `HV_ON_VMON`, `BUSY_GRACE`, `HISTORY_EVERY`) are at the top of `bin/n1470.sh`.

### Alarm sound on your remote desktop (audio tunnel)

VNC carries no sound; the alarm sound rides on the `ssh <lab-pc>` connection instead. One-off setup on the Ubuntu or Alma 9 desktop:

1. Let PipeWire accept sound on a local port (persists across logins):

```bash
mkdir -p ~/.config/pipewire/pipewire-pulse.conf.d
printf 'pulse.properties = {\n    server.address = [ "unix:native" "tcp:127.0.0.1:4713" ]\n}\n' > ~/.config/pipewire/pipewire-pulse.conf.d/tcp.conf
systemctl --user restart pipewire-pulse
```

2. In `~/.ssh/config`, the block for the lab PC needs a **RemoteForward** (not LocalForward) for the audio:

```
Host  <lab-pc>
  HostName <lab-pc-address>
  User <lab-account>
  ProxyJump <jump-host>          # if the lab PC is only reachable through one
  LocalForward 5902 localhost:5902
  LocalForward 5903 localhost:5903
  RemoteForward 24713 127.0.0.1:4713
```

3. Connect with `ssh <lab-pc>` and keep it open: the remote sound works for as long as that connection is up, with or without the VNC viewer.

Checks:

- On the desktop: `ss -ltn | grep 4713` should show `127.0.0.1:4713`.
- On the lab PC: `ss -ltn | grep 24713` should show `127.0.0.1:24713`.
- Test from the lab PC (alarm sound + spoken test message on your desktop only, nothing in the room):

```bash
bin/n1470.sh test-alarm --remote-only
```

If the tunnel is down, the alarm just skips that output; the room speaker and pop-ups still work. A second `ssh <lab-pc>` prints `remote port forwarding failed for listen port 24713`: harmless, the first connection still carries the audio.

## XY stage (Thorlabs APT through EPICS, `motor.sh`)

The two stepper axes (APT controller s/n 70810396) are driven by `~/v1/thor_epics`, which talks to the controller over USB and serves the EPICS PVs of the `iocThorLabs` IOC. Both have to be running before anything can move or be read.

**From the bench panel:** click a pad on the detector face to move the laser to its centre (unlocked), or use the
"Move the laser (XY stage)" box under the face: x, y
and Move, jogs X−/X+/Y−/Y+ by the step, Home, and STOP (STOP works locked too; the rest needs the unlock). It runs
`motor.sh` (same limits), and refuses during a scan, while a run is going, or while a move is still running;
the box shows the result (all of them: `logs/daq_commands.log`).

**They are services** (`ioc-thorlabs`, `thor-epics`; see "Starting the bench"). By hand, only with the
services stopped: `cd $EPICS/app/iocBoot/iocThorLabs && ./st.cmd`, then `cd ~/v1 && ./thor_epics -e`.

Check with `bin/motor.sh status`: both axes should say `homed enabled`.

| Command | What it does |
| --- | --- |
| `bin/motor.sh pos` | X and Y position \[mm\] |
| `bin/motor.sh status` | Positions + decoded status bits (moving, homed, limit switch, enabled) |
| `bin/motor.sh move 65 82` | Move X, then Y, and wait until both have arrived (`-` leaves an axis where it is) |
| `bin/motor.sh stop` | Stop both axes |
| `bin/motor.sh home x --yes` | Drive an axis (`x`, `y`, `both`) to its home switch at 0 mm |

The raw EPICS PVs (`caget` to read, `caput` to write):

| PV | Meaning |
| --- | --- |
| `THOR_X:read_pos`, `THOR_Y:read_pos` | Position \[mm\] |
| `THOR_X:read_stat`, `THOR_Y:read_stat` | APT status bits: 0x01/0x02 limit switch, 0x10/0x20 moving, 0x200 homing, 0x400 homed, 0x80000000 enabled |
| `THOR_X:move_abs`, `THOR_Y:move_abs` | Absolute move \[mm\], e.g. `caput THOR_X:move_abs 69` |
| `THOR_X:move_home`, `THOR_Y:move_home` | 1 = home |
| `THOR_X:stop`, `THOR_Y:stop` | 1 = stop |

- **Range:** `thor_epics` silently ignores targets outside 0–150 mm; `motor.sh` checks the limits in `db/testbench.conf` first (narrow them to what is safe in the box) and fails if the stage does not arrive.
- **Precision:** targets go to the controller with 2 decimals (0.01 mm); 1 mm = 25600 steps.
- **Backlash:** every position is approached from below. A move to a smaller value overshoots by 0.5 mm and comes back up.
- **A raw `caput` of the value `move_abs` already holds does nothing** (the PV doesn't change, so `thor_epics` doesn't see it). `motor.sh` works around this; with plain `caput`, move somewhere else first.

## XY scans (`scan_xy.sh`)

One rcdaq run per point: the stage moves, the scan point is recorded (packets 910/911 and a CSV log), then N events or T seconds are taken.

```bash
bin/scan_xy.sh -x 60:80:2 -y 70:90:2 -n 10000 --name tile66_map --dry-run
```

| Option | Meaning |
| --- | --- |
| `-x`, `-y` | A value, or `START:STOP:STEP` in mm |
| `--points FILE` | Instead of a grid: `x y` per line |
| `-n EVENTS` / `-t SECONDS` | How much data per point |
| `-r TYPE` | rcdaq run type (default: the detector's) |
| `--order snake\|raster` | Y back and forth (default) or always upwards |
| `--rest X,Y` / `--no-rest` | Where the stage goes at the end (default `REST_X,REST_Y` from `db/testbench.conf`) |
| `--notify "a@b c@d"` | Mail when the scan ends (default `NOTIFY_EMAIL`) |
| `--keep-laser` | Leave the PiLas on at the end (default: switched off) |
| `--dark` | Scan on purpose with the laser or the detector HV off (dark counts); otherwise the scan refuses |
| `--dry-run` | Print and check the plan; nothing moves |

- **Before it starts** it checks: rcdaq running and idle, the run type exists, the V1742 is in the readlist, the motors answer, no CAEN HV alarm (`--skip-hv-check` to override), and that every electrode in the detector's `hv.conf` is on and at its VSET and the PiLas emission is on (`--dark` to scan without). Then it asks (`--yes` skips the question).
- **Stop after the current point:** `bin/scan_xy.sh --quit`. The stage then goes to the rest position.
- **Ctrl-C** ends the current run at once, stops the motors and leaves the stage where it is.
- **Laser:** however the scan ends (finished, `--quit`, Ctrl-C), the PiLas is switched off unless `--keep-laser` is given. The HV is never touched.
- **Log:** one folder per scan, `logs/scans/<date>_<time>_<name>/`. In it, `<date>_<time>_<name>.csv` has one line per point (target and read-back position, run number, events, times) and the `.txt` the parameters and the exact command. The scan's analysis output (maps, plots) goes in the same folder.
- **Set up rcdaq with `daq/setup_rcdaq.sh`**, or the scan point is only in the CSV, not in the data (the scan warns).

## Keithley 6487 picoammeter + voltage source (`keithley6487.sh`)

Used for the photocathode current (QE) test; it biases the photocathode through the Photek box and measures the current. Readback, `set`, `on` and `off` are verified on the instrument (1 V test into an open output, 22 Oct); longer ramps have only run on a simulator, so take the first real ramp to −200 V slowly.

| Command | What it does |
| --- | --- |
| `bin/keithley6487.sh status` | Identity, source on/off, voltage, range, current limit, interlock, zero check/correct, one reading |
| `bin/keithley6487.sh read 20` | Mean and std. dev. of 20 current readings (warns if zero check is on) |
| `bin/keithley6487.sh query 'SOUR:VOLT?'` | Any raw SCPI query |
| `bin/keithley6487.sh zero` | Zero correction (leaves zero check ON) |
| `bin/keithley6487.sh zcheck off` | Zero check off to measure; turn it on before changing connections |
| `bin/keithley6487.sh set irange auto` | Ammeter range: `auto` or 2e-9 … 2e-2 A |
| `bin/keithley6487.sh set ilim 25u` | Source current limit: 25u, 250u, 2.5m, 25m (25m only on the 10 V range) |
| `bin/keithley6487.sh set range 500` | Source range 10, 50 or 500 V (only while off) |
| `bin/keithley6487.sh set volt -200` | Setpoint; ramped if the source is on |
| `bin/keithley6487.sh on --box-safe` | Source on at 0 V, then ramp to the setpoint |
| `bin/keithley6487.sh off` | Ramp to 0 V and switch off (`off --now` = no ramp); keeps the setpoint |
| `bin/keithley6487.sh clear` | Serial break (device clear) if it stops answering; settings are kept |

Ramp: 5 V steps every 0.5 s (10 V/s). Setpoints above 250 V are refused (`VMAX` at the top of the script). Ctrl-C during a ramp stops at the present voltage.

Before biasing a device:

- **Interlock:** the 50 V and 500 V ranges (so anything around −200 V) need INTERLOCK pins 1–2 closed, or the source stays at high impedance.
- **Current limit:** after a reset it is 25 mA; run `set ilim 25u` first.
- **505 V limit:** never wire the Keithley into a circuit the CAEN supplies are biasing; its LO must stay within 505 V of chassis.
- The front panel locks while a command runs and is handed back afterwards; the OPER key always turns the source off.

## PiLas 405 nm laser (`pilas.sh`)

Picosecond pulsed laser; not used for the QE measurement. Every `set` prints the readback afterwards.

| Command | What it does |
| --- | --- |
| `bin/pilas.sh status` | Serials, wavelength, emission, trigger, tune, frequency, head temperature, TEC |
| `bin/pilas.sh query 'lht?'` | Any raw query (`help?` lists them all) |
| `bin/pilas.sh set f 1030` | Internal frequency \[Hz\], 25 … 125000000 |
| `bin/pilas.sh set ts 0` | Trigger source: 0 internal, 1 ext. adjustable, 2 ext. TTL |
| `bin/pilas.sh set te 1` | Trigger edge: 0 falling, 1 rising |
| `bin/pilas.sh set tl -500` | Trigger level \[mV\], −4800 … 4800 |
| `bin/pilas.sh set tm 0` | Tune mode: 0 manual, 1 auto |
| `bin/pilas.sh set tune 50` | Tune \[%\], 0 … 100; only accepted in manual tune mode |
| `bin/pilas.sh off` | Pulsed emission off |
| `bin/pilas.sh on --box-safe` | Pulsed emission on |
| `bin/pilas.sh store` / `save 2` / `load 2` | Store as boot default / save to / load from memory 1–3 |

The tune scale is confirmed on hardware: `set tune 45` reads back 45.00 % (22 Oct).

## Newport 1830-R power meter (`newport_powermeter.sh`)

Arguments are sent to the meter in order; every query (ending in `?`) prints its reply. Quote anything containing `*` or `?`. `bin/newport_powermeter.sh -h` lists the useful commands. The display only refreshes every ~0.3–2 s, so space repeated readings at least 0.5 s apart.

| Command | What it does | Seen 22 Oct |
| --- | --- | --- |
| `bin/newport_powermeter.sh` (= `PM:Power?`) | Power reading in the current units: watts, because `PM:UNITS?` is `2` | `+3.6420E-08` (36.4 nW) |
| `'PM:Lambda 405' 'PM:Lambda?'` | Set / read the wavelength correction \[nm\]. The detector measures a current; the meter divides by the detector's sensitivity at this wavelength, so a wrong setting gives a wrong power for the same light. In a monochromator scan, set it to the monochromator's wavelength at every point | Accepts 200–1100 nm with this head; outside that: `201,"VALUE OUT OF RANGE"`, old value kept |
| `PM:AUTO?` / `PM:RANGE?` | Autorange on/off; gain range in use (0 = most sensitive, higher = more light). Each range has its own small zero offset | `1` / `0`; dark: range 0 = 0 ± 20 pW, range 1 = steady +48 pW; the 21 Oct lamp scans ran on range 3 |
| `PM:ATT?` | OD3 attenuator cap (×1/1000) in use. Set `PM:ATT 1` whenever the cap is fitted, or readings are ~1000× too low. The meter cannot tell whether the cap is on: look at the front of the detector | `0` (no attenuator). 28–29 Oct: the cap was on with `PM:ATT 0`, so the lamp's 10–50 pW became 10–50 fW and every range read exactly 0 |
| `PM:DIGITALFILTER?` | Averaging (noise vs. how fast the reading settles): `1` = slow (most averaging), `2` = medium, `3` = fast; `0` is refused. Keep it the same between measurements you compare; with `1`, wait ~5 s after a change of light | 29 Oct, 27 pW at 400 nm, 1 s apart: filter 1 ±0.15 pW, 2 ±0.6 pW, 3 ±3 pW |
| `PM:ZERO 1` / `PM:ZERO 0` / `PM:ZERO?` | Zero on: the reading at that moment (sign included) is stored and subtracted from then on, so readings below it now show as negative instead of 0. Take it with the detector dark: any light present is subtracted too. Off: no subtraction, negatives clipped to 0. Front-panel ZERO does the same | 29 Oct: works remotely (`PM:ZERO?` → `1`); dark then 0 ± 1 pW on range 0 |
| `PM:UNITS 2` | Watts; the only value this meter accepts (0, 1, 3, 4 → `201,"VALUE OUT OF RANGE"`) | `2` |
| `'*IDN?'` | Model, firmware, serial | `NEWPORT 1830-R-GPIB v1.4 09/21/12 10062` |
| `ERRSTR?` | Oldest error in the queue, and removes it: an error can be from several commands ago. Repeat until `0,"NO ERROR"` | `116,"SYNTAX ERROR"` = unknown command; `201,"VALUE OUT OF RANGE"` = bad value |
| `'Q?'` | Status byte; add up the bits below | `128` = normal (fresh reading) |

`Q?` bits:

| Bit | Value | Meaning |
| --- | --- | --- |
| 0 | 1 | parameter error |
| 1 | 2 | command error |
| 2 | 4 | saturated (too much light): reading not valid |
| 3 | 8 | over-range: reading not valid |
| 5 | 32 | busy |
| 7 | 128 | a fresh reading is ready |

Example: `131` = 128 + 2 + 1 = fresh reading plus two old errors. For QE work, watch bits 2 and 3.

**Readings of exactly `+0.0000E+00`:** on 28–29 Oct every range read 0 because the OD3 attenuator cap was on the detector (see `PM:ATT?`): the light was 1000× below what the meter resolves. Check the front of the detector first. Separately, with zero off, negative readings are clipped to 0, so a dark that sits below 0 is lost: run with zero on.

**Dark level:** the dark drifts: on 29 Oct, lamp on and box closed, it went from −3.6 to +23 pW (at 400 nm) in an hour, as large as the monochromatic signal (10–50 pW into the sphere). So take darks around every measurement: `bin/lamp_scan.sh` reads the dark before and after the scan and interpolates it in time. The dark in W also depends on the meter's wavelength setting (it is a current divided by that wavelength's sensitivity), so it is read at every wavelength. Negligible only above ~2 nW.

**Zeroing the diode:** with zero off, the meter shows the detector's offset as part of the power, and anything below 0 is clipped to `+0.0000E+00`. Zeroing stores the reading at that moment and subtracts it from every later reading, so the dark sits at 0 and can go negative. Whatever light is on the detector while you zero is subtracted too, so it must be dark.

1. Check the OD3 attenuator cap is off (`PM:ATT?` → `0`). It is not a dark cap: it only cuts the light by 1000×.
2. Make the detector dark: a light-tight cap on it, or `bin/monochromator.sh dark` with the box closed.
3. Wait ~5 s for the reading to settle (filter 1).
4. `bin/newport_powermeter.sh 'PM:ZERO 1'`. The front-panel ZERO button does the same.
5. Check: `PM:ZERO?` → `1`, and the reading 0 ± 1 pW.

Zero once at the start of a session. The dark still drifts after that (see Dark level above), so a zero does not replace taking darks. `bin/lamp_scan.sh` warns when zero is off.

**Older 1830 commands** also work: `Z?` zero, `R?` range, `U?` units, `W?` wavelength, `A?` attenuator, `F?` filter, `D?` reading (clipped at 0 like `PM:Power?`).


## Monochromator and lamp (QE light source)

Reachable from this machine since 28 Oct 2026: the NI GPIB-USB-HS (`3923:709b`, moved here from the Windows laptop) is `/dev/gpib0` through linux-gpib 4.3.7 (built from source in `~/linux-gpib-4.3.7`, installed in `/usr/local`; rebuild the kernel part after a kernel update). The controller is at GPIB address 1 (`GPIB0::1::INSTR` in the laptop's LabVIEW). `/etc/gpib.conf` has no device entry, so open it with `ibdev(0, 1, 0, …)` and set EOS = CR with REOS yourself.

After switching the controller on: `monochromator.sh start`, read the wavelength off the monochromator, `monochromator.sh set NM`.

| Command | What it does | Seen 28 Oct |
| --- | --- | --- |
| `bin/monochromator.sh status` | Controller state, busy, wavelength and steps | `main program (f)`, `not busy`, `600.00 nm (12000 steps)` |
| `bin/monochromator.sh pos` | Wavelength [nm] | `600.00` |
| `bin/monochromator.sh start` | After power-on: start the controller's main program (`O2000`). Moves nothing | `already running` |
| `bin/monochromator.sh set NM` | Tell the controller where the grating is (read off the monochromator's counter). Moves nothing | – |
| `bin/monochromator.sh move NM` | Move and wait; downward moves overshoot by `MONO_BACKLASH` and come back up. Ctrl-C stops | `move 550` from 600: 3.7 s, the monochromator's counter showed 550; `move 600`: 3.0 s |
| `bin/monochromator.sh dark` | Park at `MONO_DARK` (100 nm, below `MONO_MIN`): 1st order 100 nm and 2nd order 50 nm are absorbed, so nothing reaches the sphere. A dark without capping anything; `move NM` comes back up | 29 Oct: read the same as the capped diode, within the drift |
| `bin/monochromator.sh stop` | Stop the motor, also from another terminal | – |
| `bin/monochromator.sh speed [MIN MAX MS]` | Read the motor speed (start and top speed in steps/s, ramp in ms), or set it until the next `move`/`dark`, which put `MONO_SPEED` back | 29 Oct: `speed` → start 150, top 225 steps/s (11.25 nm/s), ramp 2000 ms |
| `bin/monochromator.sh raw CMD…` | ISA commands as they are, one reply per line (`\r` = CR, `\x00` = NUL); no limits checked | `raw ' ' E 'H0\r' K` → `f`, `oz`, `o12000`, `o0` |

Limits (`MONO_MIN`/`MONO_MAX`, 200–750 nm: the H20 UV's specified range), steps per nm, backlash overshoot, motor speed and move timeout are in `db/testbench.conf`. `move` and `dark` first set the speed to `MONO_SPEED` if the controller holds anything else (it forgets it at power-off; the old LabVIEW program or a `raw 'B0,…'` may have changed it). Moves are refused while the position reads 0 (not set since power-on). Every `start`, `set` and `move` goes to `logs/testbench.log`.

- **Lamp:** Hamamatsu L7893, deuterium + tungsten-halogen, 200–1100 nm, into the monochromator by SMA fibre. Front-panel switches only. Warm up at least 30 min (stability is specified from then: 0.3 %/h drift). Don't switch it off for darks.
- **Monochromator:** Jobin Yvon H20 UV (200 mm), with an ISA/JY controller speaking the "ISA Command Set". The laptop's `H20 Control.vi` (Glasgow, 2005) scans through ISA's LabVIEW driver (`Isa_user.llb`, `Isa_comm.llb`; manual: `from_GLADD/monochromator/equipment/LabView_Manual.pdf`) and stops at a software limit of 870 nm. From the driver's labels and comments:
  - **Startup:** send a space (ASCII 32) to ask the controller's state: `F` = main program running (ready), `B` = boot program, `*` = autobauded (RS-232). In `B` or `*` (on GPIB also `=`), send `O2000` followed by a NUL byte to start the main program and read until `*` comes back; a space should then answer `F`. Then `A` initialises the motor. Seen 28 Oct: space → `B`; `O2000`+NUL → `*`; space → `f` (lower case: probably main program before `A`, not yet confirmed).
  - **Replies:** a confirmation character first (`o` expected; anything else is "Bad Confirmation"), then any data up to a CR.
  - **Motor commands** (motor 0 = grating): `H0`+CR read position (steps), `F0,<steps>`+CR move relative, `G0,<steps>`+CR set the current position without moving, `E` busy check (`q` = busy, `z` = not busy), `K` limit status (`o0` = no limit switch hit), `L` stop. Commands that take parameters end with CR; `A` and `E` are sent bare. Over GPIB each reply ends with EOI. Seen 28 Oct: `E` → `oz`, `H0`+CR → `o0`+CR; `G0,6980`+CR → `o` (grating found at 349 nm); `F0,5020`+CR → `o`, `E` → `oq` while moving, `oz` after a few seconds, then `H0` → `o12000`+CR (600 nm). `F` worked without `A` first.
  - **Shutter:** `W`/`X` open/close, one parameter (shutter number). 29 Oct: `X0`/`W0`/`X1`/`W1` answered `o` but the light did not change: no shutter is fitted in the beam.
  - **Position after power-on:** the controller forgets it (it came up in boot mode and `H0` read 0 on 28 Oct). Read the wavelength off the monochromator itself, then tell the controller with `G0,<20 × nm>` before any move. Don't trust the last value LabVIEW showed: on 28 Oct it said 600 nm but the drive was at 349 nm.
- **Darks:** `monochromator.sh dark` (grating at 100 nm), checked against the capped diode on 29 Oct. Never near 0 nm: that's zero order, white light (1.27 nW into the sphere on 29 Oct, ~50× a single wavelength). After a dark, check that the monochromator's counter agrees with `monochromator.sh pos`: the long move down and back is where steps were lost (see Troubleshooting).
- **Slits:** fixed-width inserts (0.5, 1 or 2 mm on the H20, not marked); the two sliders are the slit height limiters (1–8 mm). A scan across the deuterium 656.1 nm line (29 Oct, 0.5 nm steps) gave a flat-topped peak 7.4 nm wide (FWHM), i.e. the 2 mm inserts at ~4–5 nm per mm. With a 400 µm fibre the fibre, not the slit, sets how much light gets in.
- **Cables (29 Oct, into the sphere):** of the two lamp → monochromator cables, the black one (HPK s/n FB0110, STU100H) gives ~2× the light of the yellow one (HPK s/n 0094, 105002143-3). Unscrewing and refitting a cable changed the light by up to 30 %: don't touch the cables between a reference and a detector measurement.
- **Second order (TBD: check):** above about 400 nm the grating also passes λ/2 UV light; use an order-sorting long-pass filter there, or the QE comes out too high. Not yet checked on this bench.
- **Steps:** 20 motor steps per nm, position = 20 × wavelength (600 nm = 12000 steps).
- **Still needed from the laptop:** the other Spectrometer Setup panel values (backlash, limits, Auto/Manual calibration); they're not readable from the copied `.llb` files. `CommDevDLL.dll` belongs to ISA's "HW Config and Control" program and only moves bytes (serial or GPIB); it holds no commands.

## Troubleshooting

| Symptom | Cause | Fix |
| --- | --- | --- |
| `n1470: say which unit` | `-u` missing | Add `-u A`, `-u B` or `-u all` |
| `n1470: unit … not found` | Unit off, or USB not plugged in | Switch it on; check with `bin/n1470.sh units` |
| `… is in use by: PID` | Another program has the port (GECO2020, PiLas GUI) | Close that program |
| `keithley6487: no reply to *IDN?` | Instrument on GPIB, or wrong baud/terminator | Front panel COMM → RS-232; CONFIG → COMM: 9600, CR, XON/XOFF |
| Keithley stops answering mid-session | Its command parser is stuck | `bin/keithley6487.sh clear` |
| `keithley6487: interlock open` | 50 V / 500 V range without the interlock closed | Close INTERLOCK pins 1–2 (lid switch or jumper) |
| `warning: zero check is ON` | Input shunted | `bin/keithley6487.sh zcheck off` |
| `pilas: … not allowed!` on `set tune` | Tune mode is auto | `bin/pilas.sh set tm 0` first |
| Power readings look wrong | Power meter wavelength not set | `bin/newport_powermeter.sh 'PM:Lambda <nm>'` |
| Power meter reads exactly `+0.0000E+00` everywhere | OD3 attenuator cap on the detector (28–29 Oct); no light (lamp fibre unplugged); or zero off with the dark below 0 | Take the cap off (or `PM:ATT 1` if it should be on); check the fibre at the lamp and the sphere; dark the detector and `PM:ZERO 1` |
| Monochromator ends short of its target; its counter disagrees with `monochromator.sh pos` | Drive jamming (29 Oct: motor heard jamming, wheel sticking; long downward moves lost 1–30 % of their steps) | Unscrew the back plate, oil the drive thread and the counter mechanism; then `monochromator.sh set NM` from the counter. `MONO_SPEED` was halved meanwhile: once moves land again it can go back to `300,450,1000` |
| Power meter `cannot claim USB interface` | Another program has the meter (a `watch` in another terminal) | Stop it: only one program at a time |
| No alarm sound on the remote desktop | Audio tunnel not up | `ss -ltn \| grep 24713` on the lab PC; check `RemoteForward 24713 127.0.0.1:4713` in `~/.ssh/config` and reconnect `ssh <lab-pc>` |
| HV or laser packet says FAILED | Port busy or device off at begin-run | Close the GUI or switch the device on; the run itself is unaffected |
| `motor.sh: no answer from EPICS PV` | IOC not running | `systemctl --user restart ioc-thorlabs` |
| `motor.sh: X did not start moving` | `thor_epics` not running, or the controller is off | `systemctl --user restart thor-epics`; check the APT controller |
| Panel shows a service lamp grey / `no rcdaq_server` | A service stopped | `systemctl --user list-units 'ioc-*' thor-epics rcdaq-server bench-publisher hv-annunciator`; `restart` it |
| `scan_xy.sh: rcdaq has no run type` | rcdaq set up by another script | `daq/setup_rcdaq.sh` |
| `no detector selected` / `no DAQ profile selected` | Nothing chosen yet | `bin/testbench.sh use detector NAME` / `use daq PROFILE` |


