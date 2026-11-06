# TODO

- [ ] Bad V1742 inputs, the same in all four maps (27, 29, 30 Oct, 2 Nov: different pads each time, so bench side). 6 Nov: cables left in the adapter (join position) for the technicians, purple bundle = inputs 0-15, yellow = 16-31, each labelled 1-16 (purple k = input k-1, yellow k = input 15+k; GSI's own labels differ). If the cables test fine: pulser straight into the V1742 inputs
  - Dead: 8, 10, 18, 25, 28 (purple 9, 11; yellow 3, 10, 13)
  - Noise/pickup: 4, 6 every map (purple 5, 7); 0, 2 dead 27-29 Oct, noisy since (purple 1, 3). 2 Nov waveforms: slow ±5-10 mV baseline wander, rms 0.6-1.4 mV (good 0.34); 0 and 4 still show laser pulses, 2 and 6 hardly any
- [ ] Next maps: ND 4.7 or 5.0, 10k events/point (pick one)
- [ ] Light test at core, 2100 V: ND 5.0 vs 4.5, 200k events (under-amplified p.e. term)
- [ ] New student HV scan at 1 kHz (no board misalignment), clean pad; set in /scratch2/data/hpDIRC/students/
- [ ] Hit-time study (analysis/time_xy) on the other detectors: HRPPD, Photek
- [ ] HRPPD #26 first light (4 Nov): cable HV as db/detectors/HRPPD_26/hv.conf (A0-3 MCP electrodes, B0 PC), fill V1742 slot (cabling.txt) + STAGE_X0/Y0_MM
- [ ] Group HV off (IOC n1470_group.db): test a real trip once on a dummy load before trusting it
- [ ] Pearson chi2/ndf column in gain fit tables
- [ ] `testbench.sh check`: accept 11-column hv.conf (TRIP, 4 Nov)
- [ ] Photek remount: hv.conf unit/ch/vset, octant in cabling.txt, new STAGE_X0/Y0_MM
- [ ] fit_hv_scan.sh auto window picks 61 ns pickup on readout 1 (use `--window 24.1:27.3` until fixed)
- [ ] Dark-count tool into repo (draft: scratchpad dark_count.cc, runs 6887-6896)
- [ ] Readout-1 column 0 (ch14-17) dark 3-6x, big single-pad pulses (17-21 mV vs SPE 7.5), lowest gain: lights/cloth test, readout-0 dark run, rate vs HV
- [ ] Analysis output to /scratch2 only if the /home file quota still bites (the fits' parts folders are removed after each fit since 6 Nov)
- [ ] 2nd V1742 (s/n 51240, node 1): analysis to decode 2001 and take the calibration from packets 901/902 (caen_calibration) instead of calib_*.dat (none for 51240)
- [ ] Sync cable between the two V1742s: clocks drift 0.7 ppm without one (TTT0-TTT1 grows ~85 counts/s; start chain node 0 TRG-OUT -> node 1 S-IN already fitted 6 Nov)
- [ ] 2-board analysis sync check: flag events where TTT0-TTT1 jumps by ~a period from the previous event (not a fixed half-period cut: 0.7 ppm drift reaches 0.5 ms after ~11 min)
- [ ] Old hpDIRC/rcdaq_scripts libs (glasgow, sigma_hits) built against the pre-5-Nov libEvent: rebuild before use (Packet vtable changed)
- [ ] Rework the student kit

## EPICS (open since the IOCs went in, 1 Nov; docs/epics.md)

- [ ] `LOC:ERR` check (at the bench): unit B's front panel in LOCAL, `caput N1470_B:CH3:VSET_SP 0`: the error lands in `N1470_B:LAST_ERR`, setting unchanged, record INVALID; panel back to REMOTE
- [ ] Unplug / replug one N1470 USB with the HV off (at the bench)
- [ ] The new annunciator (bin/hv_annunciator.sh, 6 Nov) through the whole chain once: the panel's `test` on a channel (room sound, log, mail, ack, mute)
- [ ] PiLas `store` / `save N` / `load N`: not tried (they overwrite stored settings); can a stored memory hold "emission on"? If so `load` needs `--box-safe`
- [ ] Optional: fast scan 1 s instead of 2 s; `mState?` for the PiLas batch
- [ ] Motor IOC listens on all interfaces (the N1470 / PiLas ones on 127.0.0.1 only)

## Macros

- One or two V1742s (packets 2000, 2001): `daq/setup_rcdaq.sh` reads the boards of the active detector's `cabling.txt` (a `2001 SLOT` row = both; panel: the "board 1" row); `TB_V1742_BOARDS="0:0 0:1"` overrides

- HRPPD HV: `testbench.sh use detector HRPPD_26`, `hv-load --yes`, `hv-on --box-safe`, then `hv-mcp MCP_V PC_GAP --wait --yes` (gap -10 = MCP-only dark); `hv-off`
- Detector face + laser (Phoebus): `phoebus/laser_face.sh` (on the bench panel too); after a db change: `phoebus/make_laser_face.py` then `phoebus/make_bench_panel.py`
- Hit time vs position (any scan): `analysis/time_xy/time_xy.sh <scan> --pad CH` (Photonis before run 6902: `--x0 32.6`)
- Timing map (1 mm scan): `root -b -q 'plot_timing_map.C+("<scan>_timing.csv", "<out>")'` (scratchpad for now)
- Full map (lin + log, with/without pads; later scans win on overlap): `root -b -q 'analysis/gain_hv/stitch_gain_map.C+("<r0>_gain_map.csv:0s <r1>_gain_map.csv:2 <join>_gain_map.csv:3:join@0.92,0 <patch>_gain_map.csv:3:patch@0.92,0", "<out>")'` (2 Nov scans shifted +0.92 mm in x)
