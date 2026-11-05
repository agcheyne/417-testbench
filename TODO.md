# TODO

- [ ] Dead V1742 inputs 0, 2, 4, 6, 8, 10, 18, 25, 28 (+1 pickup): find on bench side (adapter/cable)
- [ ] Readout-0 rows 0-1 bundle one pin off (input N on pad N-1): in that bundle/connector, not V1742 inputs 0-15 (no slip there 2 Nov); fix cable or mapping
- [ ] Column 7 (x 75-81) dark whatever input reads it (inputs 14-17, 30, 31; 14-17 fine at col 0): shadow/mask or detector? check by eye
- [x] Join scan (slot 3) + pad (5,6) patch, 2 Nov: stitched in logs/scans/photonis_full_gain_map_20261102/
- [ ] Next maps: ND 4.7 or 5.0, 10k events/point (pick one)
- [ ] Light test at core, 2100 V: ND 5.0 vs 4.5, 200k events (under-amplified p.e. term)
- [ ] New student HV scan at 1 kHz (no board misalignment), clean pad; set in /scratch2/data/hpDIRC/students/
- [x] Detector (or stage zero) 0.92 mm lower in x since the 2 Nov recabling: STAGE_X0_MM 32.6 -> 31.7 (3 Nov, runs 6902 on); cause not checked
- [ ] Hit-time study (analysis/time_xy) on the other detectors: HRPPD, Photek
- [ ] HRPPD #26 first light (4 Nov): cable HV as db/detectors/HRPPD_26/hv.conf (A0-3 MCP electrodes, B0 PC), fill V1742 slot (cabling.txt) + STAGE_X0/Y0_MM
- [ ] Group HV off (IOC n1470_group.db): test a real trip once on a dummy load before trusting it
- [ ] Pearson chi2/ndf column in gain fit tables
- [ ] `testbench.sh check`: accept 11-column hv.conf (TRIP, 4 Nov); README example `HRPPD_tile66` no longer exists
- [ ] Photek remount: hv.conf unit/ch/vset, octant in cabling.txt, new STAGE_X0/Y0_MM
- [ ] fit_hv_scan.sh auto window picks 61 ns pickup on readout 1 (use `--window 24.1:27.3` until fixed)
- [ ] Dark-count tool into repo (draft: scratchpad dark_count.cc, runs 6887-6896)
- [ ] Readout-1 column 0 (ch14-17) dark 3-6x, big single-pad pulses (17-21 mV vs SPE 7.5), lowest gain: lights/cloth test, readout-0 dark run, rate vs HV
- [ ] Move fit/timing parts to /scratch2 (/home file quota)
- [ ] Commit analysis/gain_hv + bin changes (when asked)

## Macros

- HRPPD HV: `testbench.sh use detector HRPPD_26`, `hv-load --yes`, `hv-on --box-safe`, then `hv-mcp MCP_V PC_GAP --wait --yes` (gap -10 = MCP-only dark); `hv-off`
- Detector face + laser (Phoebus): `phoebus/laser_face.sh` (on the bench panel too); after a db change: `phoebus/make_laser_face.py` then `phoebus/make_bench_panel.py`
- Hit time vs position (any scan): `analysis/time_xy/time_xy.sh <scan> --pad CH` (Photonis before run 6902: `--x0 32.6`)
- Timing map (1 mm scan): `root -b -q 'plot_timing_map.C+("<scan>_timing.csv", "<out>")'` (scratchpad for now)
- Full map (lin + log, with/without pads; later scans win on overlap): `root -b -q 'analysis/gain_hv/stitch_gain_map.C+("<r0>_gain_map.csv:0s <r1>_gain_map.csv:2 <join>_gain_map.csv:3:join@0.92,0 <patch>_gain_map.csv:3:patch@0.92,0", "<out>")'` (2 Nov scans shifted +0.92 mm in x)
