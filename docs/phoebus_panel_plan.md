# Bench panel (Phoebus): plan

Draft 1 Nov 2026. One Phoebus display for whoever runs the DAQ: HV, laser, stage, DAQ and scan status,
alarms, and the controls behind a lock. It reads the IOCs of `epics_plan.md` (N1470 on CA port 5070,
PiLas on 5072, ThorLabs motors on 5064) and needs them running all the time (its step 6).

## Decisions (user, 1 Nov 2026)

- One panel with a lock (not separate monitor and control panels).
- All 8 HV channels shown, the ones the current detector uses marked (from `db/detectors/<DET>/hv.conf`).
- DAQ and scan status in the first version.
- The alarms move into this (IOC alarm severities + panel), replacing `n1470.sh check`'s pop-ups and
  sound. The old ones did their job, but all 14 real alarms in `logs/hv_alarms.log` (27 Oct-1 Nov) were
  "VMON off" during ramps, and they ran only once a minute.

## Where it runs

- Any number of windows at once, on the machine's own screen and in VNC :2 and :3. Every window updates
  live, and none of them touches a serial port: only the IOCs do, on their own schedule.
- The CA servers listen on 127.0.0.1 only, so Phoebus runs on np-bbox1 (a laptop goes through VNC, as
  now). A gateway for remote clients is a later security decision.
- Several people can write at once; the last write wins and every window shows it at once.

## Layout (one screen, 1920 x 1080)

1. **Status bar** (top, always visible): big lamps for any HV on, laser emission on, interlock (BDILK),
   board alarm, a tripped channel, box-safe armed (with its countdown); IOC and unit connected (SN_OK);
   the alarm banner (below).
2. **HV table**, 8 rows (A ch0-3, B ch0-3):
   - electrode name from the current detector's `hv.conf`, "not used" rows greyed (still readable);
   - VMON, IMON, VSET, ISET, RUP / RDW, TRIP, MAXV, PDWN;
   - STAT as lamps: ON, RUP, RDW, OVC, OVV, UNV, MAXV, TRIP, OVP, OVT, DIS, KILL, ILK.
3. **Strip chart**: VMON and IMON of the used channels, last hour (Phoebus keeps what it saw since the
   window opened; no archiver yet).
4. **Laser**: emission (LD), frequency, tune, trigger source, temperature, last error.
5. **Stage**: x, y, moving, and the pad under the laser (the motor IOC's `pixel` records).
6. **DAQ**: running / stopped, run number, run type, events, rate, open file; scan name, point i of N,
   x, y, ETA. rcdaq and the scan scripts are not EPICS: a small publisher writes soft records (below).
7. **Controls** (in place in the table and the laser box, greyed out while locked).

## The lock

- A local PV per window (`loc://unlocked`), so unlocking one screen does not unlock the others.
- Unlock button; the panel relocks itself after 5 min without a write (a small display script).
- Locked: every write widget is disabled. Unlocked: setpoints, OFF, ALL OFF, PDWN, laser frequency and tune.
- HV ON and laser emission ON also need the box-safe arm, as now (the IOC refuses them otherwise): the ON
  buttons stay grey until BOX_SAFE is armed, and the arm button shows its 10 s countdown.
- ALL OFF and emission OFF work locked too (stopping must never need a step).

## Alarms

The logic goes into the IOC records, so every window, and the annunciator, see the same thing.

- **Severities on the records:**
  - MAJOR for a STAT alarm bit (OVC OVV UNV MAXV TRIP OVP OVT ILK), BDALARM, BDILK, and a unit that is
    unreadable or has the wrong serial while its HV is in use;
  - MAJOR for |VMON - VSET| > 10 V **only on a channel that is ON and has not been ramping for 30 s**
    (fixes the ramp false alarms);
  - MINOR for IMON > 90 % of ISET, and for the per-channel IMON limits below;
  - MINOR (note) for DIS or KILL on a used channel.
- **IMON limits** (user, 1 Nov 2026: set the Photonis one, the same for the others until known):
  - Photonis divider, A ch0: MINOR above **360 uA** (it draws 322 uA at 2100 V and 345 uA at 2250 V;
    ISET 400 uA trips it), and MINOR when IMON is more than 10 % above VMON / 6.5 MOhm (its resistance
    from the 1 Nov readings: 6.52 MOhm at 2100 V, 6.51 at 2250 V), which catches extra current at any HV;
  - every other channel: the same 360 uA until its own value is known; the resistance check only where
    a resistance is given.
- **Summary records**, one per unit and one for the bench (the highest severity, and a text saying what).
- **Panel:** red / orange outlines on the affected widgets, the alarm banner in the status bar with the
  text, and an alarm list (time, what, cleared).
- **Annunciator:** a small service (systemd user unit) that watches the bench summary and keeps the
  outputs that worked: sound in the room and on the laptop through the tunnel, speech ("HV alarm. Unit
  A, channel 2, over-current"), the VNC pop-up. It repeats every 60 s while active.
- **Acknowledge / mute**: a button on the panel (a PV, so it works from any window); mute ends when the
  alarm clears.
- **Log:** START / CHANGE / CLEAR to `logs/hv_alarms.log` and the journal as now, and the HV history CSV
  every 60 s while HV is in use (the begin-run packet keeps reading through the IOC).
- **Later, optional:** the full Phoebus alarm server (acknowledge per alarm, shelving, alarm history in
  the UI). The UI jars are in `product-4.7.3`, but the server needs Kafka, which is not installed.

## DAQ and scan publisher

- A soft-record IOC (or the database of an existing one) with: DAQ:RUNNING, DAQ:RUN, DAQ:RUNTYPE,
  DAQ:EVENTS, DAQ:RATE, DAQ:FILE; SCAN:NAME, SCAN:POINT, SCAN:NPOINTS, SCAN:X, SCAN:Y, SCAN:ETA, SCAN:STATE.
- Filled every 2 s by a script from `rcdaq_client daq_status` and the running scan's CSV/log in
  `logs/scans/` (read-only on both), as `thor_epics` does for the stage.

## Build order

1. Alarm severities and summary records in the N1470 and PiLas databases; test them on the fake N1470.
2. The panel (`.bob`), read-only parts first, against the fake N1470 and the real PiLas and stage IOCs.
3. The lock and the controls; test writes on the fake, then B ch3 as in step 4.
4. The DAQ / scan publisher and its panel box.
5. The annunciator service; then switch off the old `n1470-check.timer` alarms (its history and the
   begin-run cache move to the IOC at the same time, `epics_plan.md` step 6).

## Settled (user, 1 Nov 2026)

- IMON limits as above.
- ALL OFF is one click, no confirmation.
- Relock after 5 min without a write.
- Build: go ahead, in the build order above.

## Progress (1 Nov 2026, 13:30-14:05): build steps 1-5 done and tested; not yet running for real

Nothing below runs now: the IOCs are not services yet (`epics_plan.md` step 7), so the panel, the
annunciator and the publisher have nothing to talk to between tests. `n1470-check.timer` still does
the alarms. Nothing committed.

**1. Alarm records** (`~/epics/app/BenchSerialApp/Db/n1470_alarm_ch.db`, `n1470_alarm_unit.db`,
`bench_status.db`; laser: `pilas.db` `LDTEMP`, `LDTEC`, `ALARM`). Per channel `A_BITS` (MAJOR, STAT
alarm bits), `A_VDEV` (MAJOR, |VMON-VSET| > 10 V after 30 s ON and not ramping, from `STEADY`),
`A_IMON` (MINOR: > 0.9 ISET, > `ILIM`, or > 1.1 VMON/`RLOAD`, after 5 s steady), `A_NOTE` (MINOR: DIS /
KILL on a used channel), summed with MS links into `CHn:ALARM`, `N1470_U:ALARM` (+ `A_LOST`: no readback
while in use, `A_BOARD`: BDALARM or interlock) and `N1470:ALARM`. Config per channel `NAME USED ILIM
RLOAD`, from hv.conf. Tested on the fake N1470 (pty) in all seven cases: a 21 s ramp gives no
alarm; VMON +20 V is MAJOR exactly at 30 s steady; IMON 363 uA MINOR, 348 uA not; OVC MAJOR;
interlock MAJOR and back; used KILL channel MINOR; unit lost while in use MAJOR.
Also: `BDILK` is now a bi (`NO`/`YES`); the setpoint records read the present value at IOC start
(StreamDevice `@init`, a MON, nothing written), so panels show the real settings, also checked
on the real units (every `_SP` = its readback); a failed `@init` (unit off) leaves the setpoint usable.
`PILAS:TUNE_SP` is now in % (the controller's 0.1 % steps via ESLO; `pilas.sh` adapted). The slow
readers have PINI, so a restarted IOC has all readings within a second or two.

**2.-3. Panel** `phoebus/bench.bob`, generated by `phoebus/make_bench_panel.py` (edit that, then run it),
relock script `phoebus/relock.py`, opened by `phoebus/bench_panel.sh` with `phoebus/bench.ini` (the three
IOCs, no archive). 1900 x 900 (fits a 1080p screen under Phoebus' menus). Status bar, 8-row HV table
(electrode, readbacks, setpoints, PDWN, ON lamp, STAT lamps, OFF / ON, alarm lamp; unused rows grey,
alarm rows red / orange), 1 h strip charts, laser, stage, DAQ / scan, alarm log, controls. Checked in
Xvfb screenshots against the fake N1470 and, read-only, the real IOCs. Clicked through with XTest on
the fake: locked, channel OFF does nothing, ALL HV OFF sends `CH:4 OFF` to both units, mute works;
unlocked, OFF works, ON stays dead until armed, then sends `CH:1 ON`; a typed VSET goes out as
`VAL:150.0`; locked again, OFF dead. The window relocks itself (tested with 10 s instead of 300 s).
Not done: a panel write on the real B ch3 (the same PVs as the step-4 caputs, which worked).

**4. Publisher** `bin/bench_publisher.sh` (`once` for one pass): DAQ from `rcdaq_client daq_status -s`,
`daq_get_runtype`, `daq_get_lastfilename`; scan from `state/scan_point.conf` and the scan CSV (ETA from
the points done); every 30 s the channel config from the detector's hv.conf, which takes two optional
columns `imax_uA r_MOhm` (Photonis divider: 360, 6.5; `testbench.sh check` accepts 8 or 10 columns).

**5. Annunciator** `n1470.sh annunciate`: the `-u all check` loop every 2 s through the IOC, with the
alarm lines taken from the IOC's alarm records (the same as the panel), the logs, HV history CSV and
begin-run cache as the check did, sound + speech when an alarm starts or changes and every 60 s while it
lasts, notification and VNC pop-ups as before; writes `N1470:ALARM_TXT` (banner), `ALARM_HIST` (last 40
log lines), `ANN_ALIVE`; follows `N1470:MUTE` (panel button) both ways, and a mute ends with the alarm.
IOC down while HV in use: NO READBACK alarm within ~8 s. While it runs, `n1470.sh check` (the watch
windows, the old timer) only shows: no logs, sound or pop-ups (installed; with the IOC stopped it
behaves exactly as before). `N1470_SILENT=1` turns the outputs off, for tests.
Service files, not installed: `systemd/hv-annunciator.service`, `systemd/bench-publisher.service`.

Found on the way: the "HV alarm cleared" pop-ups inherited the alarm lock (every check waited 10 s);
fixed in `n1470.sh` (fd 8). The two pop-ups from 07:33 still hold the old lock until closed.

To make it live (needs the user): install the IOC services (`epics_plan.md` step 7), then
`hv-annunciator` and `bench-publisher`; then `n1470-check.timer` can be disabled (its checks only show
once the annunciator runs). Close the two old pop-ups.

## Live (1 Nov 2026, 14:39-14:42, user: "Install and go live")

- Installed and enabled as systemd user services (symlinks in `~/.config/systemd/user/`):
  `ioc-n1470`, `ioc-pilas` (unit files in `~/epics/app/iocBoot/ioc*/`), `hv-annunciator`,
  `bench-publisher` (in `systemd/`). `n1470-check.timer` stopped and disabled (its unit files stay;
  `systemctl --user enable --now n1470-check.timer` brings it back, e.g. with the IOCs stopped).
  Linger is still off: the services stop if nptest's last session ends.
- Checked live: the IOCs up in 6 s, both units identified, setpoints initialised; annunciator and
  publisher running ("no alarms", divider config loaded); `n1470.sh -u all status` 0.25 s, `check`
  0.30 s (display-only), `pilas.sh status` 0.21 s, `get_hv_info.sh` 0.05 s (annunciator cache, 42
  numeric lines), `get_laser_info.sh` 0.24 s; only the IOCs hold the three serial ports; the panel
  (`phoebus/bench_panel.sh`) shows the live values with a green banner.
- Fixed while going live: `PILAS:ALARM` read 3 for a few seconds after an IOC start (LD temp / TEC
  not read yet: UDF looked like "bad" / "off"); both now read at start (PINI), and the annunciator
  ignores INVALID laser readings.
- Not done: the CA address list in `~/.cshrc` / `~/.bashrc` (blocked by a permission check; for
  caget in a terminal only, nothing else needs it).
- Fixed 1 Nov 15:43: `N1470:ALARM_HIST` stayed empty: 40 log lines are ~4.9-8.8 kB, the record
  holds 4096, and the caput was refused ("Invalid element count") with its error sent to /dev/null.
  `n1470.sh` `put_text` now keeps the newest whole lines within 4000 (ALARM_TXT: 2000) bytes and
  reports a failed caput to the service's journal. Annunciator restarted; the log shows in the panel.
- 1 Nov 15:54: `N1470:ALARM_HIST` is newest line first (a Phoebus text update shows the top of a long
  text), cut to its first whole lines within 4000 bytes. Annunciator restarted.

## RCDAQ control in the panel (1 Nov 2026, 15:50-16:06; user: "incorporate the RCDAQ control panel")

- Records (`bench_status.db`): `DAQ:CMD` (begin / end / open / close / apply), `DAQ:RUNTYPE_SP`,
  `DAQ:SETUP_DET`, `DAQ:SETUP_DAQ` (mbbo, choices filled by the publisher from `daq_list_runtypes`,
  `db/detectors`, the V1742 profiles of `db/daq`), `DAQ:ACTIVE_DET` / `ACTIVE_DAQ` (state/active.conf),
  `DAQ:CMD_MSG`, `DAQ:BUSY`. The panel only writes; `bench_publisher.sh` looks every 0.5 s and executes.
- Rules, in the publisher: nothing during a scan (`state/scan_point.conf` or a scan_xy / scan_hv process);
  begin only stopped, end only running; run type, open / close and apply only stopped; apply only with
  an rcdaq_server running, in the background (testbench.sh use detector, use daq, daq/setup_rcdaq.sh,
  timeout 300 s). A run type set elsewhere moves the combo. Every command and its output:
  `logs/daq_commands.log`. In the panel: everything but End needs the unlock; Begin/End and Open/Close
  are paired buttons that swap with the state; the combos and Apply grey out while running or scanning.
- Tested on the fake N1470 IOC with a fake rcdaq_client (every rule; clicks in the panel: locked Begin
  dead, unlocked Begin / End / Close / Open / Apply, locked End works), then live: run type junk and
  back, close / open; with the user's OK a re-apply of Photonis / v1742_5G_520 (4 s, the V1742 TR
  registers read back as expected) and junk run 6897 (12 s, ~1050 Hz on the laser sync with emission
  off, 355 MB; packets 940 / 941 / 950 in it). rcdaq left stopped, logging on, run type photonis.
- The DAQ box is 252 high; the HV alarm log under it shows ~5 lines (newest first).
- rcdaq_control.py is no longer needed for this, but still works alongside (both use rcdaq_client).

## Moving the laser from the panel; alarm log into Controls (1 Nov 2026, 16:50-17:32)

User: "we need laser movement buttons or control on the detector face panel" and "the HV alarm is pretty small
too -- can we move it down into the control and make some of those smaller?"
- Records (`bench_status.db`): `STAGE:CMD` (move / home / x+ x- y+ y-), `STAGE:X_SP`, `STAGE:Y_SP` (0-150 mm),
  `STAGE:STEP` (jog, default 1 mm), `STAGE:STOP` (its own record, so a STOP cannot be cleared together with a
  CMD), `STAGE:BUSY`, `STAGE:MSG`. The publisher runs `bin/motor.sh move X Y` / `home both --yes` in the
  background (motor.sh's limits and arrival check), refuses during a scan, while a run is going or while a move
  runs; STAGE:STOP kills the running motor.sh. The panel's STOP also writes `THOR_X:stop` / `THOR_Y:stop` itself.
  The targets start at the stage's position when the publisher starts.
- Panel: a "Move the laser (XY stage)" box under the DAQ box (the face keeps the whole right column, the
  position is shown there): x, y, Move, Home, the message; step, X-/X+/Y-/Y+, busy lamp, STOP (works locked).
  The Controls box is compact (lock bar, ALL HV OFF, arm and mute) with the HV alarm log taking its right half.
- Relock fixed: Phoebus runs a script only while all its PVs are connected, so with the PiLas IOC down the
  relock never ran. Now `relock.py` has only local PVs and a ticker; three `touch.py` scripts (HV, laser,
  bench setpoints) stamp `loc://last_write$(DID)`. Tested with the PiLas IOC down: countdown 25 s -> 58 s
  after a typed value, and the window relocks.
- Tested on the fake N1470 IOC with a fake motor.sh (every rule; clicks: locked Move dead, Move to a typed x,
  STOP during a move, jog, locked STOP), then live with the user's OK: jog X +1 mm and Move back to 0, 0
  (4 s each); the stage is at 0.000 / 0.000, homed enabled.

## Face, stage, alarms, picked channels (1 Nov 2026, 17:40-18:02)

User: move the laser controls under the face; click a pixel to move to its centre; remove the cable slip; does
the face follow Photek / HRPPD; test and acknowledge buttons per channel; the sound should repeat until muted,
no speech; switching several channels at once; disabling channels (asked back, below).
- `make_laser_face.py`: one face per detector (`laser_face[_small]_DET.bob`, plus `laser_face[_small].bob` =
  the active detector's); SLIP_SLOTS emptied; the small face 418 x 392 (scale fitted to width and height, no
  labels on pads under 26 px); HRPPD_tile66 gets a placeholder (no pitch / stage offsets in its detector.conf);
  `laser_face_pads_DET.json` with each pad's rectangle and centre.
- `bench.bob`: the face shows the active detector's (`DAQ:ACTIVE_DET`, so it follows the setup picker); click
  buttons over the pads (transparent, the active detector's only) write STAGE:X_SP / Y_SP = the pad centre and
  STAGE:CMD = move, enabled only unlocked and with no scan, run or move. They live in bench.bob, not in the
  face: an embedded display cannot be given the window's lock (`$(DID)` is not expanded in a macro passed
  down). The stage box is under the face; the DAQ box takes the whole middle column.
- Per channel (`n1470_alarm_ch.db`): `CHn:TEST` (bo, 30 s, MAJOR, in CHn:ALARM bit 4) and `CHn:ACK`; both start
  at 0 (PINI, UDFS NO_ALARM: undefined, they made every row INVALID). Panel: `test` (unlocked), `ack` (locked too).
- Annunciator: no speech (ALARM_SPEAK empty); the sound repeats every pass (a new one when the last has played)
  while any alarm line is neither muted nor acknowledged; an ACK ends when its channel has no alarm line (a NOTE,
  e.g. a used channel in KILL, does not hold it); notifications and pop-ups only on a change and once a
  NOTIFY_REPEAT (calling them every pass waited 10 s for the lock that an old pop-up still holds).
- Picked channels (`n1470_board.db`): `N1470_U:SEL` (bit n = ch n; the panel's check boxes), `SEL:ON` (seq,
  Mask, ASG POWER: needs the arm, as each ON) and `SEL:OFF`; `SHFT 0` (the default -1 maps bit 0 to LNK1: the
  first try switched ch n+1). Panel: "ON selected" (unlocked + armed), "OFF selected", "clear".
- Tested on the fakes (clicks: locked pad dead, unlocked pad -> move 32.6 116.4 = row 7 col 0's centre; detector
  switch to Photek and a Photek pad; A ch1 + B ch2 ticked: not armed nothing, armed ON on exactly those two, OFF;
  test / ack; the sound counted: ~1 per 2.7 s, none after ack or mute, ack cleared with the test's end); then
  the IOC, publisher and annunciator restarted live (18:00).
- 18:23 (user: "Yes, block them"): ON only for the active detector's channels (a row in its hv.conf, CHn:USED from
  bench_publisher.sh). In the IOC: CHn:ON's SDIS reads `CHn:ON_OK` = ON && USED, processed on the spot, so a
  caput and SEL:ON (database links, past the access security) are both blocked; `ALL:ON` needs all four used.
  `n1470.sh on` refuses with "chN is not in the detector's hv.conf" (serial too). Panel: ON and the tick box
  grey on unused rows. OFF is never blocked. Tested on the fake (used ch0 ON; unused ch1 nothing; SEL ch0+ch1
  -> only ch0; ALL:ON nothing; n1470.sh on 1 refused, on 0 works), then live: only A ch0 (divider) usable.
  DIS / KILL are the front-panel HV_EN / OFF / KILL switch (not remote): the hardware way to keep a channel off.

## Restart-proof panel, y-down face, rate, alarm sound picker (1 Nov 2026, 19:00-20:05)

User: "its still not right -- the readout is type "3" setup "0,0" -- lets get that reliable / lots of pink things,
laser clicking doesnt work / the event rate, is that just events/time? / the laser map also needs inverted in Y /
is there also a place we can select the alarm sound?"

- Cause of the numbers, the pink and the dead clicks: after the 18:23 IOC restart the (old) publisher never set
  the IOC up again, so the combos had no labels, many records were UDF INVALID and DAQ:ACTIVE_DET was empty (no
  face, no pads). Now:
  - `BENCH:PUB_INIT` (bo, 0 at every IOC start): the publisher sets up any IOC that shows 0, then writes 1; its
    command handling waits for 1.
  - Run-type picks are counted: every write to `DAQ:RUNTYPE_SP` counts up `DAQ:RUNTYPE_N` (FLNK calc); a
    restarted IOC starts at 0, so only a count above the publisher's own is a pick (judging by the combo's value,
    restarts twice set rcdaq to HRPPD). The publisher writes combo labels only when they differ (a label write
    processes the mbbo and so counted as a pick).
  - Start values from `iocBoot/iocN1470/bench_choices.sh` (run_ioc.sh): the labels and rcdaq's run type now
    (`RTSP`), the active detector and profile (`DETSP`, `DAQSP`, `ACTDET`, `ACTDAQ`), the saved sound (`SNDSP`).
    One EPICS macro value holds at most 256 characters (MAC_SIZE): the list is split into `BENCH_C1..8`.
  - `UDFS NO_ALARM` on every record the publisher / annunciator fill: a restarted IOC shows them empty, not pink.
  - Also restored: the stage section of bench_publisher.sh, lost in an earlier edit today (stage_done etc.).
- Face: y down, (0, 0) at the top left (`make_laser_face.py`: sy(), pad tops, click centres, laser marker).
- Rate: `DAQ:RATE` is now instantaneous (events since the last look, ~2 s), `DAQ:RATE_AVG` = events / run time.
- Alarm sound: `bin/alarm_sounds.list` (16 label + file; duplicates by symlink left out), `N1470:SOUND` (mbbo,
  labels SND0..15) with its own write counter `N1470:SOUND_N`, `N1470:SOUND_TEST`. The annunciator: a pick
  becomes ALARM_SOUND, is saved in `state/alarm_sound` and plays once; otherwise the IOC must show ours (put back
  after a restart). Panel: "sound" drop-down and `test` above the HV alarm log (locked too).
- `bench.ini` hides Phoebus' own alarm apps (Annunciator, Alarm Tree/Table/Area/Log): an open Annunciator tab
  retried Kafka at localhost:9092 every second in the console.
- Tested on the fakes: 3+2 IOC restarts (rcdaq type unchanged, no run-type command, combo right from the first
  0.5 s, labels complete); sound pick / same pick / test / restart with right and wrong start value; an alarm
  repeats the picked sound (4 x glass in 10 s), none after ack; Phoebus on Xvfb 2 s after a restart: no pink;
  unlocked click on R1 ch17 -> `move 32.6 96.9`. Then live 20:02 (HV off, rcdaq stopped on photonis): ioc-n1470
  restarted with the publisher stopped, then bench-publisher and hv-annunciator; rcdaq still photonis, nothing
  INVALID, labels complete.

## Face view picker, cabled readout, two publisher fixes (1 Nov 2026, 20:40-21:00)

User: "how do I get the detector face to change?" -> "Yes, add it" (a view-only picker); "can we have an option
to select which readout is cabled now? and it changes in the db or something?"

- Face picker: a drop-down in the face box's title row, `loc://face_view$(DID)` (this window only): "active"
  (follows DAQ:ACTIVE_DET) or any detector. The face file rule and the pad buttons' visibility use both PVs:
  pads only while the active detector's face is shown; a red "view only" otherwise.
- Cabled readout: `SLOT_NAMES` in detector.conf (Photonis: 0=readout 0;1=readout 1;2=readout 1 turned; in the
  template too). DAQ box row "cabled": `DAQ:CABLED` (new stringout, start value CABNOW from bench_choices.sh) and
  one button per slot for detectors with 2-4 slots (DAQ:CMD "cabling SLOT"; unlocked, stopped; the slot read now
  in blue). bench_publisher.sh `cabling_set`: same refusals as the other DAQ commands, then cabling.txt (old line
  commented "(replaced below)", a dated "(bench panel): NAME, slot S, from run N on (was slot C)" note, the new
  line), `testbench.sh check`, `make_laser_face.py`, then DAQ:ACTIVE_DET "-" and back for 1 s so open panels
  reload the face file (tested: they do). Face legend: "cabled now: <SLOT_NAMES name>".
- Fixes found on the way: a publisher restarted while the IOC stayed up skipped its set-up (the IOC already said
  PUB_INIT 1) and had no detector or choices: it now sets up on its own first pass too. A run-type pick could be
  undone by the 2-s status sync before runtype_poll saw it: the sync now waits while the write count has moved.
- Tested on the fakes (db copy via BENCH_TEST_DB): view Photek -> pads off, back -> click moves; cabling 2 (same:
  no-op), 1, 7 (refused), during a run (refused), clicks on the buttons, face reload; publisher restart then 8 quick
  picks, none lost. Live 20:58 (HV off, stopped): publisher stopped, ioc-n1470 restarted, publisher started.
- 21:00: the first live clicks (4 within 20 s) stacked 8 comment lines, all "from run ?" (no last file after the
  rcdaq restart; run 6901 had no file). Now: the next run is the highest of rcdaq's last file, a run the publisher
  saw going, the panel's begin/end lines in daq_commands.log, the run-number file and the newest file on disk,
  plus 1; a change with no run since the previous panel change replaces that note (back to the old slot: no note).
  cabling.txt restored to its committed content (net no change).
