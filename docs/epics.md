# EPICS and the bench panel

The serial instruments are each owned by one EPICS IOC; the scripts (`n1470.sh`, `pilas.sh`,
`motor.sh`), the begin-run packets and the Phoebus panel all go through them. In service since
1 Nov 2026. The design and test record of that build are in git history (`docs/epics_plan.md` and
`docs/phoebus_panel_plan.md`, removed 6 Nov 2026).

## What runs

systemd user services, linger on (they start at boot, no login needed). Unit files in `systemd/`
(the N1470 / PiLas ones in `~/epics/app/iocBoot/ioc*/`), linked into `~/.config/systemd/user/`. They
have no login environment: `systemd/bench.env` gives PATH, LD_LIBRARY_PATH and the CA address list.

| Unit | Runs | CA port |
| --- | --- | --- |
| `ioc-n1470` | `streamApp` for both CAEN N1470 units (`~/epics/app/iocBoot/iocN1470`) | 5070 |
| `ioc-pilas` | `streamApp` for the PiLas controller (`iocPiLas`) | 5072 |
| `ioc-thorlabs` | `softIoc` with the stage's soft records (`iocThorLabs`) | 5064 |
| `thor-epics` | `~/v1/thor_epics -e -q`: drives the APT controller from those records (after `ioc-thorlabs`) | |
| `rcdaq-server` | `rcdaq_server` (log `~/rcdaq.log`), set up at start by `systemd/rcdaq_start_setup.sh` | |
| `bench-publisher` | `bin/bench_publisher.sh`: panel status, the panel's DAQ / stage commands, HV config | |
| `hv-annunciator` | `bin/hv_annunciator.sh`: HV alarms (room sound, `logs/hv_alarms.log`, mail, the panel's banner) | |

```bash
systemctl --user status ioc-n1470 ioc-pilas ioc-thorlabs thor-epics rcdaq-server bench-publisher hv-annunciator
```

- The N1470 and PiLas IOCs listen on 127.0.0.1 only: nothing off this PC can write to the HV or the
  laser. Clients need `EPICS_CA_AUTO_ADDR_LIST=NO` and
  `EPICS_CA_ADDR_LIST="localhost localhost:5070 localhost:5072"`; the scripts use `tb_ca` in `lib/tb.sh`, which sets it.
  Separate ports, because several CA servers on one UDP port each get only some of the unicast searches.
- `run_ioc.sh` (in each `iocBoot/ioc*`) finds the N1470 units by asking each port for its board serial (the
  FT232 adapters have no USB serial number), holds a `flock` on each port, and refuses a second copy.
  Run it without `--service` in a terminal for the IOC console (`dbpr`, `asynReport`, `var streamDebug 1`).
- Starting or restarting an IOC never changes an instrument: no output record has PINI, no autosave.

## PVs

Instrument and unit before the first colon, the instrument's own parameter names, settings end in `_SP`.

- **N1470** (`U` = `A`, s/n 236, or `B`, s/n 1593; `n` = 0-3): `N1470_U:CHn:VMON`, `IMON`, `STAT` (every
  2 s); `VSET ISET RUP RDW MAXV TRIP POL PDWN` (every 10 s, and after each write); `CHn:VSET_SP` etc. to
  write; `CHn:ON`, `CHn:OFF`, `ALL:ON`, `ALL:OFF`, `SEL` / `SEL:ON` / `SEL:OFF` (the ticked channels);
  `BDSNUM`, `SN_OK` (1 while the serial on that port is the expected one), `DEV` (`caget -S N1470_U:DEV.VAL$`),
  `BDCTR BDILK BDALARM`, `LAST_ERR`; the arm `N1470:BOX_SAFE`.
- **PiLas**: `PILAS:LD` (emission, every 2 s), `F TS TE TL TM TUNE LHT` (10 s), the raw texts `STATE_TXT`,
  `VERSION_TXT`, `LDTEC_TXT`, `LDTEMP_TXT` (what packet 950/951 is made from); `*_SP`, `LD_ON`, `LD_OFF`,
  `STORE`, `SAVE`, `LOAD`, `PILAS:BOX_SAFE`.
- **Stage**: `THOR_X:read_pos`, `THOR_X:move_abs`, `THOR_X:stop`, ..., `THOR_X:pixel` (pad under the laser).
- **Panel records** (`bench_status.db`, in the N1470 IOC, filled by `bench_publisher.sh`): `DAQ:*`
  (run, type, events, rate, the command records), `SCAN:*`, `STAGE:*`, `BENCH:PUB_INIT`.

**Stale values.** One query fills all four channels' records; when it fails only CH0 goes INVALID, CH1-3
keep their last value and its time stamp. Take a value as good only if it is not INVALID and younger
than three scan periods (6 s / 30 s); `caget -a` shows both.

## Safety rules (in the IOC, so they hold for the panel, the scripts and a typed `caput`)

- **ON needs the arm:** `...:ON`, `ALL:ON`, `SEL:ON`, `PILAS:LD_ON`, `PILAS:LOAD` are writable only while
  the IOC's `BOX_SAFE` is 1 ("the box is closed"); it drops to 0 after 10 s. `n1470.sh on` / `pilas.sh on`
  arm it only with `--box-safe`.
- **ON only for the active detector's channels** (a row in its `hv.conf`; `CHn:USED` from the publisher).
- **Every N1470 write needs `SN_OK`**: nothing goes to a port that answers with the other unit's serial.
- **OFF always works**, locked panel, no arm, any channel. A write of 0 to an ON/OFF record does nothing.
- Everything else (readbacks, `BDSNUM`, `SN_OK`) is read-only over CA. All clients run as nptest, so this
  stops accidents, not people.

## When something is down

| Situation | What happens |
| --- | --- |
| An IOC stopped | `n1470.sh` / `pilas.sh` do nothing (no serial fallback since 6 Nov 2026): use the front panel; the begin-run packet says FAILED. The annunciator logs it, and alarms if the HV was on |
| GECO2020 or the PiLas GUI wanted | `systemctl --user stop ioc-n1470` (or `ioc-pilas`) first; start it again after |
| A unit switched on after the IOC started | its `DEV` is `/nonexistent/...`: `systemctl --user restart ioc-n1470` (does not touch the HV) |
| USB pulled while running | CH0 INVALID, `check` says UNREADABLE (an alarm if its HV was in use); asyn reopens the port when it is back |
| USB sockets swapped | `SN_OK` 0, writes refused: restart `ioc-n1470` |
| IOC hung (port held, PVs dead) | restart the IOC |

## The bench panel (Phoebus)

`phoebus/bench_panel.sh` opens it (any number of windows: the PC's screen, VNC :2 and :3); generated
by `phoebus/make_bench_panel.py`, the detector faces by `make_laser_face.py`.

- **Lock**, per window: unlock to write; it relocks after 5 min without a write. ALL HV OFF, emission
  OFF, the stage STOP, End run, ack and mute work locked.
- **DAQ and stage controls** are executed by `bench_publisher.sh`: nothing during a scan; begin only
  stopped, end only running; run type, open/close and Apply only stopped. One line per command in
  `logs/daq_commands.log` (the full output only when it failed; the last good setup's in
  `logs/setup_rcdaq_last.log`).
- **Alarms** are record severities in the N1470 IOC: MAJOR for STAT alarm bits, BDALARM, BDILK, an
  unreadable or wrong unit while its HV is in use, and |VMON - VSET| > 10 V on a channel ON and not
  ramping; MINOR for IMON above 90 % of ISET or above its limit (360 uA, the Photonis divider's). The
  annunciator repeats the sound until the alarm is acknowledged (per channel, `ack`) or muted; the sound
  is picked in the panel (`bin/alarm_sounds.list`). The full Phoebus alarm server is not used (it needs Kafka).

## Open items

In `TODO.md`, under EPICS.
