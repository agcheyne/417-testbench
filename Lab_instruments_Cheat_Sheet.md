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
| JY H20 UV monochromator | not yet | NI GPIB-USB-HS `3923:709b`, GPIB address 1 (or spare RS-232) | GPIB (linux-gpib not installed yet) | No |



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

A background timer (`n1470-check.timer`) runs `n1470.sh -u all check` every minute, whether or not anyone is watching: it raises the alarm and writes the logs below. To look at the live values, run this in a terminal on the lab PC or in VNC (refreshes every 5 s; keep just one):

```bash
watch -c -n 5 "$PWD/bin/n1470.sh -u all check --color"
```

| On an alarm | Where it shows |
| --- | --- |
| Red ALARM banner naming unit, channel and fault | The `watch` terminal |
| Desktop notification (stays until dismissed, repeats every 60 s) | Lab PC's own screen |
| Warning pop-up window | Every running VNC desktop (`:2`, `:3`) |
| Alarm sound, then a spoken message ("HV alarm. Unit A, channel 2, over-current."), every check; "HV alarm cleared" once when it clears | Room speaker, and your remote desktop if the audio tunnel is up |

- **Alarms:** OVC, OVV, UNV, MAXV, TRIP, OVP, OVT, ILK on any channel, or VMON more than 10 V from VSET (`VTOL`). `KILL` and `DIS` show as a NOTE (not ready), not an alarm.
- **Silence the sound:** `bin/n1470.sh mute` silences sound and speech (monitoring continues; it unmutes itself when the alarm clears, or `bin/n1470.sh unmute`). `check --quiet` never plays either.
- **Spoken alarm:** after the sound, `espeak-ng` says what is wrong (unit, channel or electrode, fault; the first two problems, then "and N more"). Turn it off with `ALARM_SPEAK=` (empty); voice and speed are `ALARM_VOICE` / `ALARM_SPEED`.
- **Test the alarms:** `bin/n1470.sh test-alarm` fires every output once (warn the room first; `--quiet` skips the sound) and reports which outputs worked. It affects only that one command: no HV access, and a running `watch` monitor is untouched.
- **Is a monitor running?** `pgrep -af 'watch .*n1470.sh'` should show exactly one; two monitors give every alarm twice.
- **Units switched off or unplugged** while their HV is off are not an alarm (one log line). A unit that disappears, or can't be read, while its HV is on IS an alarm; a port held by GECO2020 only becomes one after 5 min (`BUSY_GRACE`).
- **Logs** (in `logs/`; until the switch-over in the README, the timer still runs `~/hpDIRC/n1470.sh` and logs to `~/hpDIRC/logs/`): `hv_alarms.log` gets one line per alarm start/change/clear, HV on/off, and unit present/absent/busy; `hv_history_YYYYMMDD.csv` gets every channel once a minute, only while the HV is in use (any channel on, ramping, or VMON > 10 V). Idle days write nothing. Read with `tail -f logs/hv_alarms.log`.
- **Pause / resume the background check:** `systemctl --user stop n1470-check.timer` / `start`; `systemctl --user list-timers` shows when it runs next. It runs while the lab account is logged in (the lab desktop session normally is).
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

**Start them** (each in its own terminal, as nptest):

```bash
cd $EPICS/app/iocBoot/iocThorLabs && ./st.cmd
```

```bash
cd ~/v1 && ./thor_epics -e
```

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
- **Log:** `logs/scans/<date>_<name>.csv`, one line per point: target and read-back position, run number, events, times. The `.txt` next to it has the parameters and the exact command.
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
| `PM:ATT?` | OD3 attenuator cap (×1/1000) in use. Set `PM:ATT 1` whenever the cap is fitted, or readings are ~1000× too low | `0` (no attenuator) |
| `PM:DIGITALFILTER?` | Averaging (noise vs. how fast the reading settles). Keep it the same between measurements you compare; meaning of each number not yet confirmed (manual / front-panel FILTER) | `2` now; every point of the 21 Oct scans logged `1` |
| `PM:ZERO?` | Offset subtraction on/off. To zero, cap the detector and press ZERO on the front panel (the remote zero command didn't work) | `0` (off) |
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

**Dark level:** with the detector capped the reading is zero within about ±20 pW (negative readings show as 0), so the background is negligible above ~2 nW.


## Monochromator and lamp (QE light source)

Not controllable from this machine yet: the monochromator still runs from LabVIEW on the Windows laptop, through the NI GPIB-USB-HS (`3923:709b`), at GPIB address 1 (`GPIB0::1::INSTR` in LabVIEW). Two ways here: move the GPIB-USB-HS to this machine (needs linux-gpib 4.3.7; its kernel modules build cleanly against this kernel, install needs root; `/etc/gpib.conf` `board_type = "ni_usb_b"`, then the instrument is `pad = 1`), or connect the controller's spare RS-232 port through a USB-serial adapter on the keyboard/mouse hub.

- **Lamp:** Hamamatsu L7893, deuterium + tungsten-halogen, 200–1100 nm, into the monochromator by SMA fibre. Front-panel switches only. Warm up at least 30 min (stability is specified from then: 0.3 %/h drift). Don't switch it off for darks.
- **Monochromator:** Jobin Yvon H20 UV (200 mm), with an ISA/JY controller speaking the "ISA Command Set". The laptop's `H20 Control.vi` (Glasgow, 2005) scans through ISA's LabVIEW driver (`Isa_user.llb`, `Isa_comm.llb`; manual: `from_GLADD/monochromator/equipment/LabView_Manual.pdf`) and stops at a software limit of 870 nm. From the driver's labels and comments:
  - **Startup:** send a space (ASCII 32) to ask the controller's state: `F` = main program running (ready), `B` = boot program, `*` = autobauded (RS-232). In `B` or `*` (on GPIB also `=`), send `O2000` followed by a NUL byte to start the main program and read until `*` comes back; a space should then answer `F`. Then `A` initialises the motor.
  - **Replies:** a confirmation character first (`o` expected; anything else is "Bad Confirmation"), then any data up to a CR.
  - **Motor commands** (motor 0 = grating): `H0`+CR read position (steps), `F0,<steps>`+CR move relative, `G0,<steps>`+CR set the current position without moving, `E` busy check (`q` = busy, `z` = not busy), `K` limit status, `L` stop. Commands that take parameters end with CR; `A` and `E` are sent bare. These framings come from the driver, not from a trace, so check them with harmless commands (space, `E`, `H0`) first.
  - **Shutter:** `W`/`X` open/close, one parameter (shutter number).
  - **Reference:** left at 600 nm on 26 Oct 2026, so the first `H0` read should return about 12000.
- **Darks:** cap the SMA fibre at the monochromator input, or park the monochromator at about 180 nm, if its drive goes that low; check with the power meter that it really reads dark there. Never near 0 nm: that's zero order, white light.
- **Second order:** above about 400 nm the grating also passes λ/2 UV light; use an order-sorting long-pass filter there, or the QE comes out too high.
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
| No alarm sound on the remote desktop | Audio tunnel not up | `ss -ltn \| grep 24713` on the lab PC; check `RemoteForward 24713 127.0.0.1:4713` in `~/.ssh/config` and reconnect `ssh <lab-pc>` |
| HV or laser packet says FAILED | Port busy or device off at begin-run | Close the GUI or switch the device on; the run itself is unaffected |
| `motor.sh: no answer from EPICS PV` | IOC not running | Start `st.cmd` (see XY stage) |
| `motor.sh: X did not start moving` | `thor_epics` not running, or the controller is off | Start `~/v1/thor_epics -e`; check the APT controller |
| `scan_xy.sh: rcdaq has no run type` | rcdaq set up by another script | `daq/setup_rcdaq.sh` |
| `no detector selected` / `no DAQ profile selected` | Nothing chosen yet | `bin/testbench.sh use detector NAME` / `use daq PROFILE` |


