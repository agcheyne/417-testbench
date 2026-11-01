# EPICS IOCs for the CAEN N1470 HV and the PiLas laser: plan

Status: **draft files, 31 Oct 2026; read-only test steps 1-3 done on hardware 1 Nov** (results
under [Test plan](#test-plan-on-hardware)). The N1470 IOC ran read-only for ~7 min; no PV was
written, no HV or laser state changed. The PiLas IOC has not been run. Nothing is installed as a
service, the scripts are unchanged.

## Why

Each rcdaq run start waits ~7 s: `get_hv_info.sh` runs `n1470.sh -u all status` (~4.3 s of
9600-baud round trips, ~35 queries at ~125 ms) and `get_laser_info.sh` runs `pilas.sh status`
(~2.2 s). The stage readback takes 0.04 s because the motor IOC holds its values and `caget`
only asks the IOC.

With an IOC for each of the two serial instruments:

- the begin-run readback becomes a few `caget`s (~0.1 s for all ~90 PVs instead of ~6.5 s);
- one program owns each port, so the timer, a `watch` monitor, a scan and the begin-run script
  never queue on the port (today they take turns on a `flock`, and a busy port shows up as a
  "busy" unit);
- the HV check could run every 10-15 s instead of every minute, at no port cost;
- Phoebus (installed, `~/epics/product-4.7.3`) and an archiver can read the same PVs later.

The short-term fix being installed separately (HV readings cached by the minute timer, faster
PiLas reply handling) is no longer needed once this is in, and can be taken out then.

Cost: two more processes to keep running, a CA setup for several IOCs on one PC (below), and
`n1470.sh` / `pilas.sh` get a second back end. The front panels, GECO2020 and the PiLas GUI can
only be used with the IOC stopped (as now with the scripts: one program per port).

## What is installed (checked 31 Oct, read-only)

| Item | State |
| --- | --- |
| EPICS base | `~/epics/base` -> `base-7.0.8.1`, built, `softIoc`, `caget`, `msi`, `ascheck` in `bin/linux-x86_64` |
| asyn | `~/epics/support/asyn`, git `R4-44-2-61-g9519475a`, **built**: `lib/linux-x86_64/libasyn.so`, serial driver (`drvAsynSerialPort.dbd`), `asynRecord` |
| StreamDevice | `~/epics/support/StreamDevice` 2.8.26, **built** against that asyn: `libstream.so` and the IOC binary `bin/linux-x86_64/streamApp` (its dbd registers `drvAsynSerialPortRegisterCommands`, `asynRecord`, all stream device types). Built without CALC and PCRE, so no `scalcout` and no regular expressions in protocols; the drafts need neither. |
| IOC binary | `streamApp` is used as is, like the motor IOC uses `softIoc`: nothing to compile. Its `main()` has no `-S` option and ends when stdin closes, so under systemd `st.service.cmd` ends with `epicsThreadSleep`. |
| Motor IOC | `softIoc ./st.cmd` in `~/epics/app/iocBoot/iocThorLabs`, started by hand in a terminal (pts/8) on 5 Oct; `./thor_epics -e` in `~/v1` (pts/16) since 27 Oct fills its soft records with `ezcaPut`. Not a systemd service; CA server on all interfaces, port 5064. |
| CA client setup | `~/.cshrc` and `~/.bashrc`: `EPICS_CA_AUTO_ADDR_LIST=NO`, `EPICS_CA_ADDR_LIST=localhost`. `caRepeater` running since 5 Oct. |
| procServ | not installed; EPEL has `procServ-2.8.0-1.el9` (console for IOCs under systemd, optional) |
| systemd user | `n1470-check.timer` (minute, at :30) runs `n1470.sh -u all check`. `Linger=no`: user services stop when nptest logs out (true of the timer today too). |

## Draft files

All new; nothing existing was changed. `~/epics/app` is not a git repository.

| File | What |
| --- | --- |
| `~/epics/app/BenchSerialApp/Db/n1470.proto` | N1470 protocol: `mon4_*` (one query reads a parameter of all 4 channels), board queries, settings, `power_on` / `power_off` |
| `.../Db/n1470_board.db` | per unit: `BDSNUM`, identity check `SN_OK`, `BDNAME BDFREL BDCTR BDILK BDALARM`, `ALL:ON` / `ALL:OFF` |
| `.../Db/n1470_readback.db`, `n1470_readback_ch.db` | per unit: CH0 readers (do the I/O) + soft records CH1-3 (filled by I/O redirection) |
| `.../Db/n1470_ch.db` | per channel: `*_SP` settings, `ON`, `OFF` |
| `.../Db/n1470.substitutions` | both units, 4 channels each (187 records) |
| `.../Db/box_safe.db` | the arm record of an IOC (`N1470:BOX_SAFE`, `PILAS:BOX_SAFE`) |
| `.../Db/pilas.proto`, `pilas.db` | PiLas: raw text of `state?` / `version?`, single values, settings, emission, memories (27 records) |
| `~/epics/app/iocBoot/iocN1470/` | `st.cmd`, `st.service.cmd`, `envPaths`, `n1470.acf`, `run_ioc.sh` (unit discovery + locks), `ioc-n1470.service`, no-op `Makefile` |
| `~/epics/app/iocBoot/iocPiLas/` | the same for the laser: `st.cmd`, `st.service.cmd`, `envPaths`, `pilas.acf`, `run_ioc.sh`, `ioc-pilas.service`, `Makefile` |

Checked offline: `msi` expands the substitutions and `dbExpand.pl` parses all records against
`streamApp.dbd` (record types, fields, menu values); every FLNK / SDIS / fanout / redirection
target exists; `ascheck` accepts both ACF files; `bash -n` on both `run_ioc.sh`. Since 1 Nov
also: both databases and protocol files load and parse in a `streamApp` on dummy ports
(`/nonexistent/...`, test prefixes, CA on 127.0.0.1:5099), which `dbExpand.pl` does not cover
(it missed the bad scan periods, the asyn record and the 40-character limit, below).

## PV names

Style as the motor IOC (`THOR_X:read_pos`): instrument and unit before the first colon. Parameter
names are the instrument's own (N1470 `VMON`, `ISET`, ...), so `n1470.sh mon VMON 2` maps 1:1
to `N1470_A:CH2:VMON`. Settings end in `_SP`; the readback has the bare name.

### CAEN N1470 IOC (both units in one IOC, CA port 5070)

`U` = `A` (s/n 236) or `B` (s/n 1593), the aliases of `UNITS` in `n1470.sh`; `n` = 0-3.

| PV | Type | Scan | What |
| --- | --- | --- | --- |
| `N1470_U:CHn:VMON`, `:IMON` | ai | 2 s | V, uA |
| `N1470_U:CHn:STAT` | longin | 2 s | status bits as `STAT_BITS` in `n1470.sh` (0 ON, 1 RUP, 2 RDW, 3 OVC, ... 11 KILL, 12 ILK) |
| `N1470_U:CHn:VSET`, `:ISET`, `:RUP`, `:RDW`, `:MAXV`, `:TRIP` | ai | 10 s, and after each write | settings as read back |
| `N1470_U:CHn:POL`, `:PDWN` | stringin | 10 s | `+`/`-`, `RAMP`/`KILL` |
| `N1470_U:CHn:VSET_SP`, `ISET_SP`, `TRIP_SP` | ao | - | write a setting |
| `N1470_U:CHn:RUP_SP`, `RDW_SP`, `MAXV_SP` | longout | - | |
| `N1470_U:CHn:PDWN_SP` | bo | - | `KILL` / `RAMP` |
| `N1470_U:CHn:ON` | bo | - | write 1: channel on. Needs `N1470:BOX_SAFE` (see below) |
| `N1470_U:CHn:OFF` | bo | - | write 1: channel off. Always allowed (if the unit's identity is OK) |
| `N1470_U:ALL:ON`, `:ALL:OFF` | bo | - | the whole unit (`CH:4`), as `on all` / `off all` |
| `N1470_U:BDSNUM` | longin | 10 s + start | board serial as read now (posts every read, so `SN_OK` is re-time-stamped) |
| `N1470_U:SN`, `:SN_OK` | longin, calc | - | expected serial; 1 while `BDSNUM` = `SN` |
| `N1470_U:DEV` | lsi | start | the by-path port the IOC opened (`/nonexistent/...` = unit not found at start), from `run_ioc.sh`'s `DEV_A`/`DEV_B`; 56 characters, so read it as `caget -S N1470_U:DEV.VAL$` (plain `caget` cuts it at 40) |
| `N1470_U:BDNAME`, `:BDFREL` | stringin | start | |
| `N1470_U:BDCTR`, `:BDILK`, `:BDALARM` | stringin, longin | 10 s | LOCAL/REMOTE, interlock, board alarm bits |
| `N1470_U:LAST_ERR` | stringin | - | last reply that did not parse (`#BD:00,CMD:ERR`, `LOC:ERR`, ...) |
| `N1470:BOX_SAFE` | bo | - | the arm for ON: 1 for 10 s, then back to 0 by itself |

### PiLas IOC (CA port 5072)

| PV | Type | Scan | What |
| --- | --- | --- | --- |
| `PILAS:STATE_TXT` | waveform CHAR[2048] | 10 s, and after each write | raw reply of `state?` |
| `PILAS:VERSION_TXT` | waveform CHAR[1024] | start | raw reply of `version?` (serials, wavelength) |
| `PILAS:LDTEC_TXT`, `:LDTEMP_TXT` | stringin | 10 s | reply lines of `ldtec?`, `ldtemp?` |
| `PILAS:LD` | bi | 2 s | pulsed emission off/on (`ld?`) |
| `PILAS:F` | longin | 10 s | frequency [Hz] |
| `PILAS:TS` | stringin | 10 s | trigger source as the controller says it (`internal`; the words for 1, 2 not seen yet) |
| `PILAS:TE`, `:TM` | bi | 10 s | trigger edge `falling`/`rising`, tune mode `manual`/`auto` (0/1 as `te=`, `tm=`) |
| `PILAS:TL` | ai | 10 s | trigger level [V] (`tl?` answers in V; `TL_SP` is in mV, as `tl=`) |
| `PILAS:TUNE`, `:LHT` | ai | 10 s | tune [%], laser head temperature [C] |
| `PILAS:F_SP`, `TS_SP`, `TE_SP`, `TL_SP`, `TM_SP` | longout | - | settings, ranges as `pilas.sh` |
| `PILAS:TUNE_SP` | longout | - | tune in the controller's 0.1 % units (`pilas.sh set tune 45` writes 450) |
| `PILAS:LD_ON` | bo | - | write 1: emission on. Needs `PILAS:BOX_SAFE` |
| `PILAS:LD_OFF` | bo | - | write 1: emission off. Always allowed |
| `PILAS:STORE`, `:SAVE`, `:LOAD` | bo, longout | - | boot default / memory 1-3. `LOAD` needs `PILAS:BOX_SAFE` (open question 3) |
| `PILAS:REFRESH` | fanout | - | processed after every write: re-reads `LD`, `STATE_TXT` and the settings |
| `PILAS:LAST_ERR`, `PILAS:BOX_SAFE` | | | as for the N1470 |

The single PiLas queries were read on 1 Nov (test step 1): each answers one `key: value` line,
`ts?` `te?` `tm?` in words and `tl?` in V (table in `pilas.proto`). Words not seen yet, because
they need a setting change: `ts` 1/2, `te` rising, `tm` manual, `ld` on; a different word puts the
reply in `PILAS:LAST_ERR` and the record INVALID. `mState?` (in `help?`) answers everything as
one line of numbers; it could replace the 10 s batch once its field order is known. **The
begin-run packets do not depend on any of this:** they use the raw text PVs.

## What each IOC polls, and how often

Each query costs ~125 ms on the N1470 (9600 baud: ~30 bytes out, ~50 back, plus the
supply's answer time); one `CH:4` query reads a parameter of all four channels.

| N1470, per unit (each unit has its own port) | Queries | Port time |
| --- | --- | --- |
| every 2 s: VMON, IMON, STAT | 3 | ~0.4 s per 2 s (20 %) |
| every 10 s: VSET ISET RUP RDW MAXV TRIP POL PDWN, BDCTR BDILK BDALARM | 11 | ~1.4 s per 10 s (14 %) |
| every 10 s: BDSNUM (identity) | 1 | 1 % |
| at start: BDNAME, BDFREL | 2 | |
| after a write: the readback of that parameter (4 channels) | 1 | |

| PiLas (19200 baud) | Queries | Port time |
| --- | --- | --- |
| every 2 s: `ld?` | 1 | small |
| every 10 s: `state?` (ends after 0.3 s of quiet), `ldtec?`, `ldtemp?`, `f? ts? te? tl? tm? tune? lht?` | 10 | ~0.6 s per 10 s |
| at start: `version?` | 1 | 0.3 s |
| after a write: `PILAS:REFRESH` (8 reads) | 8 | ~0.6 s |

A write queues behind the reads already waiting on that port: usually one query (~0.15 s), at
most the 10 s batch (~1.5 s). The `OFF` records have `PRIO HIGH`, so StreamDevice queues them
ahead of waiting reads and they wait for the query in progress only (checked in
`AsynDriverInterface.cc`: the queue request uses the record's PRIO).

Readbacks are up to 2 s old (VMON, IMON, STAT, laser emission) or 10 s old (settings), except
right after a write through the IOC, which re-reads the parameter. That is fresher than the
cache of the short-term fix (one minute).

**Stale values.** One query fills four records: the CH0 record does the I/O, CH1-3 are filled
by StreamDevice's redirection and processed with the same time stamp. When a read fails, only
the CH0 record goes INVALID; CH1-3 keep their last value **and its time stamp**. So a client
takes a value as good only if it is not INVALID **and** its time stamp is younger than three
scan periods (6 s for the 2 s PVs, 30 s for the 10 s ones). `caget -a` prints both.

## Channel Access with three IOCs on one PC

With `EPICS_CA_ADDR_LIST=localhost` a search goes to 127.0.0.1:5064 by unicast. When several
CA servers share UDP port 5064 on one host, the kernel delivers a unicast datagram to **one** of
them, so some PVs would not be found at random. (Base 7.0.8.1's rsrv does not forward unicast
searches to the others; checked in `caservertask.c`.) The drafts avoid it:

- each new IOC has its own server port: N1470 **5070**, PiLas **5072**
  (`EPICS_CAS_SERVER_PORT` in `st.cmd`); the motor IOC stays on 5064, untouched;
- they listen on 127.0.0.1 only (`EPICS_CAS_INTF_ADDR_LIST`): nothing off this PC can write to
  the HV or the laser. (The motor IOC listens on all interfaces; worth changing too, later.)
- clients list all three:

  ```
  EPICS_CA_AUTO_ADDR_LIST=NO
  EPICS_CA_ADDR_LIST="localhost localhost:5070 localhost:5072"
  ```

  in `~/.cshrc` and `~/.bashrc` (for people), and exported by `lib/tb.sh` (for the scripts:
  the systemd timer and rcdaq do not read the login files). `thor_epics` keeps working:
  `localhost` is still in the list.
- each IOC's own CA client (it resolves the access-security inputs, below) is pointed at its
  own server in `st.cmd`.

The other way, `EPICS_CA_ADDR_LIST=127.255.255.255` (loopback broadcast reaches every server),
needs no port numbers, but may not reach servers bound to 127.0.0.1 only (not tried); not chosen.

## How the scripts change

The plan keeps the command lines **and the output** of `n1470.sh` and `pilas.sh`, and changes
only what is behind them. Everything that calls them (`get_hv_info.sh`, `get_laser_info.sh`,
`scan_xy.sh`, `scan_hv.sh`, `testbench.sh hv-load`, the timer, a `watch`) then needs no change
and gets faster.

New in `lib/tb.sh`: the CA environment above, full paths to `caget` / `caput` / `cainfo`
(the timer has no EPICS `PATH`), and a helper `tb_caget_fresh MAXAGE PV...` that runs one
`caget -w 1 -a` and fails if a PV is missing, INVALID or older than MAXAGE.

### `n1470.sh`

| Command | With the IOC |
| --- | --- |
| `units` | `caget -S N1470_U:DEV.VAL$ N1470_U:BDSNUM N1470_U:SN_OK` (no port probing while the IOC runs) |
| `status` | one `caget -a` of the ~50 PVs of a unit; same table, values formatted as now (`%05d` for STAT); one more line: "read by IOC at HH:MM:SS" (oldest time stamp) |
| `check` | the same alarm logic (`ALARM_BITS`, `VTOL`, `IFRAC`, `IMAX`, `unit_assess`, logs, sound, pop-ups), on values from `status`. Unit states: `/nonexistent/` port = **ABSENT**; INVALID or stale = **UNREADABLE**; `SN_OK` 0 = **UNREADABLE** with "wrong unit on port"; IOC not answering = **IOC DOWN** (fallback below). `BUSY` (GECO2020) no longer happens while the IOC runs. |
| `mon PAR [CH]` | `caget` of the matching PV; a parameter the IOC does not have: "not in the IOC; stop it for raw queries" (a raw-query PV is left out on purpose: a raw command could send `PAR:ON`) |
| `set PAR CH VAL` | `caput -c -w 5 N1470_U:CHn:PAR_SP VAL` (`-c` waits until the readback has been re-read), then prints the readback; `CH all` = four writes |
| `off CH\|all` | `caput -c N1470_U:CHn:OFF 1` / `N1470_U:ALL:OFF 1` |
| `on CH\|all --box-safe` | refuses without the flag, as now; then `caput N1470:BOX_SAFE 1`, wait up to 1 s until `cainfo` shows write access to `...:ON`, `caput -c ...:ON 1`, `caput N1470:BOX_SAFE 0` |
| `test-alarm`, `mute`, `unmute` | unchanged (no HV access) |

The `n1470-check` timer: unchanged at first (same command, now ~0.3 s instead of ~4 s). Once
it has run well for a while, `OnCalendar=*:*:0/15` (every 15 s) is possible; the alarm
latency then drops from up to a minute to 15 s. `HISTORY_EVERY` stays 60 s.

`scan_hv.sh` polls `mon VMON` to wait for the ramp: it now sees a new value every 2 s (it
loops anyway). `scan_xy.sh` / `scan_hv.sh` preflight (`check --quiet`, `status`): no change.

### `pilas.sh`

| Command | With the IOC |
| --- | --- |
| `status` | `caget -S -t` of `PILAS:VERSION_TXT PILAS:STATE_TXT PILAS:LDTEC_TXT PILAS:LDTEMP_TXT`, through the same `grep -Ev` / `sed` as now, so the output stays the same (`scan_*.sh` read `laser emission:` from it; `get_laser_info.sh` builds packet 951 from it) |
| `query CMD` | the PV of a known query; others: "stop the IOC for raw queries" |
| `set PAR VAL` | same range checks, then `caput -c PILAS:PAR_SP VAL` (tune x10 as now), print the readback |
| `off` | `caput -c PILAS:LD_OFF 1` |
| `on --box-safe` | refuses without the flag; `caput PILAS:BOX_SAFE 1`, wait for write access, `caput -c PILAS:LD_ON 1`, `caput PILAS:BOX_SAFE 0` |
| `store`, `save N`, `load N` | `PILAS:STORE`, `PILAS:SAVE`, `PILAS:LOAD` |

To check on hardware: how `caget -S` prints the degree sign (byte `\xb0`) that the `sed`
turns into `deg`.

### rcdaq begin-run scripts (`daq/`)

`get_hv_info.sh` and `get_laser_info.sh` need no change: they call `n1470.sh -u all status`
and `pilas.sh status`, whose output stays the same; packets 940/941 and 950/951 keep their
format. The begin-run then takes ~0.3 s for both instead of ~6.5 s. The text packets gain the
"read by IOC at" line, so the age of the values is on record. Later, if wanted, they can call
`caget` directly and skip the bash start-up of the scripts (~0.1 s more saved).

## The `--box-safe` guard

Today the guard is the flag: `n1470.sh on` and `pilas.sh on` refuse without `--box-safe`. With
an IOC owning the port, every write goes through the IOC, and anyone on the PC could
`caput N1470_A:CH0:ON 1`. The drafts keep the step deliberate in two layers:

1. **Scripts:** unchanged rule, no flag, no ON.
2. **IOC access security** (`n1470.acf`, `pilas.acf`, loaded by `asSetFilename`):
   - `...:ON`, `ALL:ON`, `LD_ON` (and `PILAS:LOAD`) are writable only while the IOC's
     `BOX_SAFE` record is 1. Writing 1 to it means "the dark box is closed and the detector is
     safe"; it drops back to 0 after 10 s by itself (`HIGH`), and `on --box-safe` clears it right
     after the ON. A `caput` to an ON record without arming gets "Write access denied".
   - all N1470 writes, OFF included, also need `SN_OK` = 1 for that unit: nothing is written to a
     port that answers with the other unit's serial (USB plugged into a different socket).
     While a unit cannot be read, its writes are refused too: switch it off at the front panel.
   - OFF otherwise always works: `CHn:OFF`, `ALL:OFF`, `LD_OFF` are not behind the arm.
   - everything else is read-only over CA (`ASG(DEFAULT)` has no WRITE rule): readbacks,
     `BDSNUM`, `SN_OK`, `DEV`. Otherwise a `caput 236` to `N1470_B:BDSNUM` could fake the identity
     check. Writable: the `_SP` settings, OFF, the arm (`ASG ARM`), and the guarded ON records.
     There is no asyn record (raw strings, `PAR:ON` included): `streamApp` has no device support
     for it anyway.

   Every client on the PC runs as nptest, so this cannot tell `n1470.sh` from a typed `caput`:
   like the flag, it stops accidents, not people. Arming takes two `caput`s on purpose.

Also:

- **A write of 0 does nothing.** The ON/OFF records are disabled while their own value is 0
  (`SDIS` = own `VAL`, `DISV` 0) and fall back to 0 after 1 s, so a stray `caput ...:ON 0` or a
  GUI button reset sends nothing.
- **Starting or restarting an IOC never changes the instruments:** no output record has `PINI`,
  and there is no autosave. A supply that is on stays on through an IOC restart, and its values
  come back with the first scans.

## Running the IOCs: systemd user services

`ioc-n1470.service` and `ioc-pilas.service` (drafts next to the st.cmd files), `Type=simple`,
`Restart=on-failure` after 20 s, logs in `journalctl --user -u ioc-n1470`. `ExecStart` is
`run_ioc.sh --service`, which:

1. takes `$XDG_RUNTIME_DIR/ioc-n1470.lock`: a second copy of the IOC refuses to start (two
   copies would open the same port and garble both);
2. (N1470) runs `n1470.sh units` and takes unit A and B's by-path ports from its answer
   (`BDSNUM` probing as today: both FT232B adapters have no USB serial number, so the by-id
   link is useless); a unit that is off or answers on two ports gets a `/nonexistent/` port;
3. keeps a `flock` on each port it opens for as long as the IOC runs: the same lock
   `n1470.sh` / `pilas.sh` take, so their serial fallback waits instead of talking over the IOC
   (asyn itself does not lock the tty; checked in `drvAsynSerialPort.c`);
4. execs `streamApp st.service.cmd` (= `st.cmd`, then sleep: no console under systemd).

`./run_ioc.sh` without `--service`, in a terminal, gives the IOC console (`dbpr`, `asynReport`,
`var streamDebug 1`) for testing. Install, when the user says so:

```bash
ln -s ~/epics/app/iocBoot/iocN1470/ioc-n1470.service ~/epics/app/iocBoot/iocPiLas/ioc-pilas.service ~/.config/systemd/user/
```

```bash
systemctl --user daemon-reload
```

```bash
systemctl --user enable --now ioc-n1470 ioc-pilas
```

Points to decide:

- **Linger** is off: user services and the HV timer stop when nptest logs out of the desktop
  (the HV check has the same limit today). `loginctl enable-linger nptest` keeps them running
  without a session; the alarm pop-ups/sound still need the desktop.
- **procServ** (EPEL) would give the service a console you can attach to
  (`procServ -f -n ioc-n1470 unix:%t/ioc-n1470.sock ./run_ioc.sh`). Optional; the drafts work
  without it.
- **A unit switched on after the IOC started** is not picked up by itself (its port was not
  found at start): `systemctl --user restart ioc-n1470`. `n1470.sh check` says so when a unit is
  ABSENT. Restarting the IOC does not touch the HV.
- The motor IOC and `thor_epics` could get the same treatment (they run in terminals since 5 and
  27 Oct, and do not survive a reboot or a closed terminal). Separate job.

### The whole bench as services (1 Nov 2026, user: "Yes, make services")

Unit files in `417-testbench/systemd/` (the N1470 / PiLas IOC units stay in `~/epics/app/iocBoot/ioc*/`),
linked into `~/.config/systemd/user/`. Lingering is on (`loginctl show-user nptest -p Linger`), so all
start at boot without a login. They get no login environment: `systemd/bench.env` (PATH,
LD_LIBRARY_PATH for rcdaq and EPICS, the CA address list) is their `EnvironmentFile`.

| Unit | Runs | Order |
| --- | --- | --- |
| `ioc-n1470`, `ioc-pilas` | the serial IOCs (above) | first |
| `ioc-thorlabs` | `softIoc -S st.cmd` in `iocThorLabs` (CA 5064) | first |
| `thor-epics` | `~/v1/thor_epics -e` (APT controller, USB); stopped via its `stop` file | after (Requires) `ioc-thorlabs` |
| `rcdaq-server` | `rcdaq_server` (log `~/rcdaq.log`), then `systemd/rcdaq_start_setup.sh` runs `daq/setup_rcdaq.sh` for the active detector / profile (a failure leaves the server up); stop: `rcdaq_stop.sh` ends a run first, then `daq_shutdown` | after the three IOCs (begin-run packets read them) |
| `bench-publisher` | panel status, DAQ commands, HV config | after `ioc-n1470`, `rcdaq-server` |
| `hv-annunciator` | HV alarms | after `ioc-n1470`, `ioc-pilas` |

Not services: the VNC desktops `:2` / `:3` (started by hand), `caRepeater` (started by CA clients).
`n1470-check.timer` is disabled (the fallback while both HV IOCs are stopped).

**thor_epics at start (found 1 Nov):** `check_epics_commands()` is meant to swallow the first monitor value
of every command PV, but its loop `while(epicsCommands[n++]!=NULL){ ezcaNewMonitorValue(epicsCommands[n],…) }`
skips index 0, `THOR_Y:move_abs`. So on its second pass thor_epics may take the IOC's `THOR_Y:move_abs` as a
command and drive Y there: 0 after an IOC (re)start. No move when the stage is at rest (0, 0) or the
controller was power-cycled (it counts from 0); after a PC-only reboot with the stage elsewhere, Y goes to
0. **Fixed 1 Nov 2026 16:39** (user: "ok do it"): the loop is now `while(epicsCommands[n]!=NULL){ …[n]… ; n++ ; }`,
rebuilt with `make -f Makefile_epics thor_epics`; the old source and binary are `~/v1/thor_epics.c.orig_20261101`,
`~/v1/thor_epics.orig_20261101`. Not yet run on the controller (thor_epics was not running).

**Hand-over done 1 Nov 2026 16:42-16:46** (user closed the hand-started IOC and asked for the hand-over and a
test from scratch): the 5 Oct rcdaq_server shut down with `daq_shutdown` (no run); all bench services stopped,
the three new units enabled, then everything started at once with `systemctl --user start default.target`
(what boot does). All seven active within 2 s, none failed. Checked: the stage at 0.000 / 0.000 mm, `homed
enabled`, not moving, positions updating (so thor_epics connected and moved nothing); rcdaq configured by
its start script (setup rc 0, V1742 5 GS/s on the readlist, logging on) and on the detector's run type;
`rcdaq-server` restarts cleanly (`daq_shutdown`); junk run 6898 (3 s) from the panel's PVs with packets 930
(stage), 940 (HV), 950 (laser) read from the IOCs in the services' environment; scripts 0.15-0.30 s; every
panel lamp up. Fixed on the way: `thor_epics` runs with `-q` (its status screen wrote 27 kB/s, 2.3 GB a day,
into its log; the positions to EPICS are not part of that screen); `rcdaq_start_setup.sh` also selects the
detector's run type (a new server stands on "test", the last one defined).
Note: when everything starts at once, the annunciator logs "cannot be read" for ~3 s until the IOC's first
readings (HV off: a note only; with the HV on at a PC reboot that would be a short NO READBACK alarm).

## When an IOC, or an instrument, is down

| Situation | What clients see | What the scripts do |
| --- | --- | --- |
| IOC stopped (service inactive or failed) | PVs do not connect (`caget -w 1` times out) | **Fallback to the serial port**, exactly as today, if `systemctl --user is-active ioc-n1470` is not `active`/`activating` and no other program has the port open. `status`/`check` add "(serial: IOC down)". Writes too, `on` still only with `--box-safe`: `off` must always work. |
| IOC starting (`activating`) | PVs not there yet | wait up to 10 s, then report "IOC starting", no fallback (it would collide with the IOC opening the port) |
| IOC running but hung (port held, PVs dead) | PVs time out | no fallback (port in use); `check`: UNREADABLE, an alarm while HV is in use, text "systemctl --user restart ioc-n1470" |
| Unit off / USB out at IOC start | `N1470_U:DEV` = `/nonexistent/...`, PVs INVALID | `check`: ABSENT (alarm only if the HV was in use, as now) |
| Unit off / USB pulled while running | CH0 records INVALID (timeout), CH1-3 stale | `check`: UNREADABLE (alarm if in use). asyn reopens the port by itself when it comes back on the same socket. |
| Wrong unit on a port (USB swapped) | `SN_OK` = 0 (MAJOR), writes refused | `check`: alarm "unit identity" + "restart ioc-n1470" |
| `n1470-check` timer while IOC is down | | same as today via the fallback, plus a log line `IOC DOWN` / `IOC BACK` in `hv_alarms.log` |
| Begin-run while IOC is down | | `n1470.sh status` falls back to serial: the run starts ~4 s slower, the packet is complete. If even that fails: "HV readback FAILED", as today. |
| GECO2020 / PiLas GUI wanted | | stop the IOC first (`systemctl --user stop ioc-n1470`); the scripts then use the port directly |

The fallback keeps everything working during the switch-over and whenever an IOC is stopped on
purpose; it can stay in for good, it is the code that runs today.

## Test plan on hardware

For the user, in this order, at a time when no scan is running. Steps 1-3 are read-only on the
instruments.

1. **PiLas reply formats** (IOC not running): `pilas.sh query 'ld?'`, and the same for `f?`,
   `ts?`, `te?`, `tl?`, `tm?`, `tune?`, `lht?`, `ldtec?`, `ldtemp?`; fix the `get_*` protocols
   in `pilas.proto` to match. Also what `store`, `save1`, `load1` reply (manual or a test).
2. **CA setup:** set the client `EPICS_CA_ADDR_LIST` above in one terminal; check that
   `caget THOR_X:read_pos` still works.
3. **N1470 IOC, read only:** stop `n1470-check.timer`, run `iocBoot/iocN1470/run_ioc.sh` in a
   terminal. Check that it reports no protocol errors at `iocInit`, that `camonitor
   N1470_A:CH0:VMON N1470_A:CH1:VMON N1470_A:BDSNUM N1470_A:SN_OK` updates, and that the values
   match the last `n1470.sh -u all status`. `cainfo N1470_A:CH0:ON` must say **no** write
   access (the guard, checked without writing). Unplug/replug test with the HV off.
4. **N1470 writes, HV off, detector disconnected or unit B:** `VSET_SP` on an off channel and its
   readback; `caput` of 0 to `...:ON` does nothing; arm + ON + OFF on a channel with nothing
   connected; `LOC:ERR` with the front panel in LOCAL lands in `LAST_ERR`. Check that a
   setpoint written as `2250.0` is accepted (`set_f1`).
5. **PiLas IOC,** the same, with the laser head capped or the box closed.
6. Switch `n1470.sh` / `pilas.sh` to the PVs (separate change, with the fallback), compare
   `status` output and packets 940/941/950/951 of one run against a run taken without the IOC.
7. Install the services, then the faster check timer if wanted.

### Results of steps 1-3 (1 Nov 2026, 00:58-01:10, after the speed test ended)

Before: no scan or `--follow` running, A ch0 OFF, laser emission off. The user's `watch` windows
(`n1470.sh -u all check` every 3 s, `pilas.sh status` every 10 s) kept running.

1. **PiLas replies:** all one `key: value` line; `ts?`, `te?`, `tm?` answer in words, `tl?` in
   V (`+0.00 V`). `pilas.proto` / `pilas.db` fixed to match: `TS` stringin, `TE` / `TM` bi with
   the controller's words, `TL` ai in V. `help?` lists `mState?` (all state as numbers) and
   confirms `te=` 1 rising, `tm=` 1 auto. Not seen (writes or setting changes): replies of
   `store` / `saveN` / `loadN`, the words for `ts` 1/2, `te` rising, `tm` manual, `ld` on.
2. **CA:** with `EPICS_CA_ADDR_LIST="localhost localhost:5070 localhost:5072"`,
   `caget THOR_X:read_pos THOR_Y:read_pos` answers (0, 0) in 0.03 s; a missing PV times out in
   1 s as set.
3. **N1470 IOC:** `run_ioc.sh` found A s/n 236 on usb 6.1 and B s/n 1593 on usb 6.2 and took
   both locks. After the fixes below, `iocInit` had no errors. Every value of both units
   (board info, VSET VMON ISET IMON RUP RDW MAXV POL STAT, 4 channels) matched
   `n1470.sh -u all status` taken just before; PDWN and TRIP matched `n1470.sh mon` afterwards.
   VMON/IMON/STAT are re-read every 2 s (one unit's 3 queries take ~0.17 s), settings and
   `BDSNUM` every 10 s; no INVALID/MAJOR/MINOR, `LAST_ERR` empty. `caget -a` of all 102
   readback PVs: 0.03 s (`n1470.sh -u all status`: 4.2 s). `cainfo`: all `ON` / `ALL:ON` read,
   no write; `OFF`, `_SP`, `BOX_SAFE` read, write; readbacks no write.
   `camonitor` works (VMON posts on change only, as `MDEL` 0; `caget -a` shows the
   time stamp). Note: `timeout N camonitor | ...` shows nothing, the buffered output dies with
   it; use `timeout -s INT` to a file, or a terminal.
   While the IOC ran, the `watch` checks logged `UNIT ... port busy` (HV off: no alarm) and
   `readable again` after; that is what the plan's "IOC holds the port" case predicts until
   `n1470.sh` reads the PVs. Not done: unplug/replug (needs someone at the bench).

   Fixed during the test (first start, or the dummy-port check before it):
   - `st.cmd`: `EPICS_DB_INCLUDE_PATH`, without which `dbLoadTemplate` finds none of the
     `file "..."` db files.
   - `N1470_U:DEV` was a stringin (40 characters); the by-path names are 56, so the whole
     `n1470_board.db` failed to load. Now an `lsi` read from `DEV_A` / `DEV_B` (`getenv`).
   - `SCAN "30 second"` / `"60 second"` are not in base's `menuScan`: `BDSNUM` now 10 s,
     `VERSION_TXT` at start only.
   - the asyn records: `streamApp` has no `asynRecordDevice`; removed.
   - `ASG(DEFAULT)` allowed writes to every readback, `BDSNUM` too (a `caput` could fake
     `SN_OK`); now read-only, with `ARM` (the `BOX_SAFE` records) and `PILAS_SET` groups for what
     must be writable.
   - `BDSNUM` `MDEL -1`: `SN_OK` (a CP calc) was recomputed only when the serial changed, so its
     time stamp froze and the stale rule would have called it stale.

   Afterwards: IOC stopped, no process on either HV port, no CA server on 5070/5072/5099,
   `n1470-check.timer` started again; its first check exited 0, no new alarm.

### Step 4 so far (1 Nov 2026, 12:04-12:12)

**On a fake N1470** (`fake_n1470.py` on a pty, both units, the real db / proto / ACF, CA on
5099; no instrument involved), every write path was checked by the exact bytes it sent:
- unarmed `ON 1` and `ON 0`: "Write access denied", nothing sent; armed `ON 0`: nothing sent;
  armed `ON 1`: exactly one `$BD:00,CMD:SET,CH:3,PAR:ON`; the `HIGH` reset 1 s later sends
  nothing; write access comes 0.04 s after arming and goes when `BOX_SAFE` drops after 10 s.
  `ALL:ON` the same.
- `OFF 1` sends `PAR:OFF`, `OFF 0` nothing; `ALL:OFF` sends `CH:4,PAR:OFF`.
- setpoints: `VAL:2250.0` (`set_f1`), `VAL:310.00`, `VAL:50`, `VAL:2400`, `VAL:10.0`,
  `PAR:PDWN,VAL:RAMP` / `KILL`; each followed by its `MON` readback, fresh when `caput -c`
  returns. Out of range is clamped by DRVL/DRVH (`VSET_SP 9000` sent as 8000.0; the supply
  clamps to MAXV itself).
- unit unreadable (fake stopped): its OFF and setpoints lose write access within ~10 s, the
  other unit keeps it; unit answering the wrong serial: `SN_OK` 0 (MAJOR), all its writes
  refused. asyn reopened the port by itself.

**Bug found and fixed there:** the re-read after a write never happened. A forward link (and a
fanout) only processes a **Passive** record, and the CH0 readers are periodic, so the readback
stayed old for up to 10 s and `caput -c` did not wait for it. Now each write FLNKs to a Passive
helper `N1470_U:RB:<PAR>` that puts 1 to `CH0:<PAR>.PROC` (processes any record);
`PILAS:REFRESH` is a `seq` writing to the readers' `.PROC` fields instead of a fanout.

**On the real supply (12:30-12:32, user's go-ahead), unit B ch3, off with VSET 0:**
- unarmed `caput ON 1` and `ON 0`: "Write access denied"; STAT stayed 2048, VMON 0.
- `VSET_SP 1234.5`: accepted, readback 1234.6 (the supply's set step), `caput -c` 0.2 s
  including the readback; back to 0: readback 0. So `%.1f` values are accepted.
- ISET 310, RUP 50, RDW 50, MAXV 2400, TRIP 10 (present values) rewritten: each accepted,
  readback right, 0.2-0.4 s per `caput -c`.
- `PDWN_SP RAMP`, then `KILL`: both accepted and read back (`n1470.sh set PDWN` cannot do this).
- `OFF 1` on the off channel: accepted, STAT 2048 unchanged.
- `LAST_ERR` stayed empty throughout. Afterwards `n1470.sh -u all status` and `mon PDWN` /
  `mon TRIP` match the 12:04 readings exactly. IOC stopped, ports free, timer restarted (check
  exit 0; the `watch` windows logged port busy / readable again).

**Armed ON + OFF on the real supply (13:05-13:06, box-safe confirmed by the user), unit B ch3,
VSET 0, done twice; the second time with an asyn I/O trace of the port:**
- write access to the ON records appears right after arming (A's too: the arm is per IOC, not
  per unit) and goes when `BOX_SAFE` is written back to 0.
- one `caput -c ...:CH3:ON 1` sent exactly one `$BD:00,CMD:SET,CH:3,PAR:ON`, the supply answered
  `#BD:00,CMD:OK`, and the STAT re-read followed at once (`RB:STAT`); `OFF 1` the same with
  `PAR:OFF`. Nothing else was sent, and no `SET` at the 1 s `HIGH` reset.
- **the channel did not switch on:** STAT stayed 02048 (KILL), VMON and IMON 0. The supply
  accepts `PAR:ON` on a killed channel and ignores it; the KILL latch would have to be cleared
  first. That is the supply, not the IOC (`n1470.sh on` would see the same). A real switch-on
  through the IOC is therefore still to be seen, on a channel that is OFF, not KILL.

Not done in step 4: `LOC:ERR` with the front panel in LOCAL (needs someone at the bench).

### Step 5: PiLas IOC (1 Nov 2026, 13:00-13:04, emission off throughout)

- `run_ioc.sh` started cleanly (the `watch pilas.sh status` window was running; its pilas.sh
  is waited for, as designed). All single values matched `pilas.sh status`; `STATE_TXT` and
  `VERSION_TXT` hold the raw replies (`STATE_TXT` filled at its first 10 s scan).
- `caget -S` escapes the control bytes and the degree byte (`\r\n`, `\t`, `\xb0`):
  `printf '%b' "$(caget -S -t PILAS:STATE_TXT)" | tr -d '\r'` and then `pilas.sh`'s own `sed`
  gives output identical to `pilas.sh status`. That settles the degree-sign question.
- Access: `LD_ON`, `LOAD` no write; `LD_OFF`, settings, `STORE`, `SAVE`, `BOX_SAFE` write;
  readbacks no write. Unarmed `LD_ON 1` / `0`: "Write access denied".
- Writes (each ~1.2 s with `caput -c`, including the `REFRESH` re-read of all values):
  `LD_OFF 1` (already off); f 1031 and back to 1030 (`state?` showed 1031); te rising and back;
  tm manual, tune 50.0 % in manual, tm auto; ts 1 / 2 / 0; tl 0. All accepted and read back.
  New words: ts 1 `ext. adjustable`, ts 2 `ext. TTL`, te 1 `rising`, tm 0 `manual`.
- A refused setting (tune in auto mode): `caput` itself still reports success, but the
  setpoint record goes `CALC INVALID` and the reply lands in `LAST_ERR` (first 39 characters:
  "changing tune values in auto tune mode"). So step 6's `pilas.sh set` must check the
  setpoint's severity after the `caput`, not just its exit code. Same for the N1470 records.
- After stopping the IOC: port free, `pilas.sh status` identical to before.
- Not done: `LD_ON` armed (not needed, per the user), `STORE` / `SAVE` / `LOAD`.
- Changed afterwards (checked on dummy ports only): `LAST_ERR` records have `PINI` so they
  start empty instead of UDF INVALID; comments in `pilas.proto` / `pilas.db`.

### Step 6: the scripts through the IOCs (1 Nov 2026, 13:10-13:25; installed in `bin/`)

How: only the transport layer changed. `n1470.sh`: `hv_mon` reads from one `caget -a` per unit
(`ioc_open`), `set` / `on` / `off` write the `_SP` / `ON` / `OFF` records with `caput -c`
(`ioc_cmd`), `units` lists the ports the IOC opened. `pilas.sh`: `pl_cmd` gives the IOC's text
records back as the controller's lines (`ioc_query`), `pl_set` writes the setpoints. `status`,
`check`, the alarm logic, logs, history, cache, and every output line are the same code as before.
- backend: `auto` (default) uses the IOC when its CA port answers (a refused TCP connect costs
  nothing, so with the IOC stopped the scripts are as fast as before), else the serial ports;
  `N1470_BACKEND` / `PILAS_BACKEND` = `serial` or `ioc` to force one. `run_ioc.sh` finds the
  units with `N1470_BACKEND=serial`.
- a reading that is INVALID or older than three scans (6 s fast, 30 s slow) is no reading: the
  unit is UNREADABLE for `check`, as a unit that does not answer. Identity: `SN_OK` must be 1,
  else "now answers s/n ...; refusing", for reads and writes.
- writes: `caput -c`, then the record's severity: a refused write (record INVALID) fails with
  `LAST_ERR`, like the serial "-> reply" error. `on --box-safe` / `pilas.sh on --box-safe` arm
  `BOX_SAFE`, wait for write access, write, disarm. `pilas.sh load N` through the IOC needs
  `--box-safe` (open question 3); `store` / `save` say the IOC does not keep the reply.
  `query` through the IOC knows the commands of `status` and the single values; others: stop
  the IOC. `n1470.sh mon` likewise for the parameters the IOC reads.
- `n1470.sh set PDWN CH RAMP|KILL` now accepted (before: "value must be a number").

Tests:
- on the fake N1470 / fake PiLas (pty, CA 5099 / 5098): every command; the commands the fakes got
  were exactly the expected ones; arm cleared after ON; unreadable unit and wrong unit refused.
- serial fallback (no IOC) vs the installed scripts on the real units: `units`, `-u all status`,
  `mon`, `pilas.sh status`, `query`: same output and exit codes.
- through the real IOCs vs a serial snapshot taken just before: `-u all status` identical
  (VMON / IMON too), `mon VSET RUP PDWN TRIP` identical, `check` the same NOTE / OK lines,
  `pilas.sh status` identical. No-op writes through the real IOCs: B ch3 VSET 0 and PDWN KILL,
  laser f 1030, and tune in auto mode refused with the controller's message.
- begin-run: `get_hv_info.sh` / `get_laser_info.sh` packets (text and numeric) identical through
  the IOCs and over serial.
- the `watch` windows and the timer picked the IOC up by themselves (one "port busy" while the
  IOCs started under the old script, then "readable again"), and fell back to serial without a
  log line when the IOCs stopped.

| | serial | through the IOC |
| --- | --- | --- |
| `n1470.sh -u all status` | 4.2 s | 0.25 s |
| `n1470.sh -u all check` | 4.5 s (+10 s, see below) | 0.32 s |
| `pilas.sh status` | 0.8 s | 0.21 s |
| `get_hv_info.sh` without the cache | 4.4 s | 0.30 s |
| `n1470.sh set` / `pilas.sh set` | | 0.4 s / 1.3 s, readback included |

Fixed on the way:
- `n1470.sh` alarm lock: pop-ups and alarm sounds inherited the lock's fd, so an open "HV alarm
  cleared" pop-up held it, and every later check waited the full 10 s (`flock -w 10`) before
  going on: since 07:33 on 1 Nov every check took ~15 s. The lock is now fd 8, which `popup` and
  `sound_bg` close. The two pop-ups opened at 07:33 still hold the old lock until closed.
- `daq/get_hv_info.sh` used the minute cache even when that check could not read a unit
  (e.g. port busy): the packet then had no readings. It now reads live in that case.

## To do

Kept up to date as the tests go; done items move to the results above.

- [ ] **`LOC:ERR` check** (needs someone at the bench): put unit B's front panel in LOCAL, write a
  setting (`N1470_B:CH3:VSET_SP 0`), check the error reply lands in `N1470_B:LAST_ERR`, the
  setting is unchanged, and the record goes INVALID; panel back to REMOTE.
- [ ] Unplug / replug test of one N1470 USB with the HV off (needs someone at the bench).
- [ ] A real switch-on through the IOC: B ch3 is KILL-latched, so ON was accepted and ignored
  (13:05). Needs a channel that is OFF but not KILL, at 0 V or nothing connected, and the
  box-safe confirmation; or the first real `n1470.sh on` after step 6.
- [ ] PiLas `store` / `saveN` / `loadN` replies: not tried, they overwrite stored settings;
  decide whether and when (open question 3).
- [ ] Close the two "N1470 HV alarm cleared" pop-ups from 07:33 on 1 Nov (they hold the old
  alarm lock; until then every check waits 10 s).
- [ ] An alarm through the IOC with the real outputs (pop-up, sound) once, e.g. the next real
  one; `test-alarm` does not go through the HV path.
- [x] Services installed 1 Nov 14:39 (see docs/phoebus_panel_plan.md, "Live").
- [ ] Optional: fast scan 1 s instead of 2 s; `mState?` for the PiLas batch. (Phoebus panel: done.)

## Open questions for the user

1. Go ahead with EPICS for these two? (The other serial/GPIB instruments, Keithley 6487,
   Newport, monochromator, are not part of this plan.)
2. ~~Keep the serial fallback in the scripts for good?~~ **Answered 1 Nov: keep it** ("Yes keep the backups").
3. `pilas.sh load N` needs no flag today. Can a stored memory hold "emission on"? If it can,
   `load` should need `--box-safe` too (the draft puts `PILAS:LOAD` behind the arm).
4. Linger on, so the IOCs (and the HV check) run without a desktop session?
5. Check every 15 s instead of every minute once the IOC is in?

## Smaller notes found on the way

- `n1470.sh set PDWN ...` could not work: its value check only accepted numbers. Fixed in step 6
  (`set PDWN CH RAMP|KILL`); the supply accepts `PAR:PDWN,VAL:RAMP` / `KILL` (step 4, B ch3).
- The motor IOC's `db/thorlabs.db` defines `$(P)_$(AXIS):pixel` twice (harmless: the second
  is merged into the first).
