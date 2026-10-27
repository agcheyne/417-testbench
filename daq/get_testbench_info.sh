#! /bin/bash
#
# Begin-run readback of the test-bench database: what was selected for this run.
#   910 (text)    repository version, the active detector's detector.conf, hv.conf and
#                 cabling.txt, the DAQ profile, and the scan point if scan_xy.sh is running
#   911 (numbers) one integer per line, in this order (format version 1):
#                   0 format version (1)
#                   1 DAQ FREQ_CODE (0 = 5 GS/s, 1 = 2.5, 2 = 1; -1 = not a V1742 profile)
#                   2 RECORD_LENGTH [samples]
#                   3 POST_TRIGGER [%]
#                   4 scan id (scan start, Unix time; 0 = not part of a scan)
#                   5 scan point index (0-based; -1 = not part of a scan)
#                   6 number of points in the scan (0 = not part of a scan)
#                   7 scan target X [um]
#                   8 scan target Y [um]
# The real stage position is in 931/932 (get_motor_xy_info.sh).

. "$(dirname "$(readlink -f "$0")")/../lib/tb.sh"
tb_site
mkdir -p "$ANC_PACKETS"
TXT=$ANC_PACKETS/tbinfo.txt
NUM=$ANC_PACKETS/tbinfo_numeric.txt

{
    echo "417-testbench begin-run info, $(date '+%F %T')"
    echo "repository $TB_ROOT at $(git -C "$TB_ROOT" describe --always --dirty 2> /dev/null || echo 'unknown version')"
    if ! ( tb_active ) 2> /dev/null ; then
        echo "NO DETECTOR / DAQ PROFILE SELECTED: $( ( tb_active ) 2>&1 )"
    else
        tb_active
        echo "detector $TB_DETECTOR, DAQ profile $TB_DAQ"
        for f in detector.conf hv.conf cabling.txt ; do
            echo "--- detectors/$TB_DETECTOR/$f"
            tb_table "$(tb_detector_dir)/$f"
        done
        echo "--- daq/$TB_DAQ.conf"
        tb_table "$(tb_daq_file)"
    fi
    if [ -r "$TB_STATE/scan_point.conf" ] ; then
        echo "--- scan point (scan_xy.sh)"
        tb_table "$TB_STATE/scan_point.conf"
    fi
} > "$TXT" 2>&1

FREQ_CODE=-1 RECORD_LENGTH=0 POST_TRIGGER=0
SCAN_ID=0 POINT=-1 NPOINTS=0 TARGET_X=0 TARGET_Y=0
( tb_active ) 2> /dev/null && { tb_active ; tb_conf "$(tb_daq_file)" ; }
[ "$READOUT" == v1742 ] || FREQ_CODE=-1
[ -r "$TB_STATE/scan_point.conf" ] && tb_conf "$TB_STATE/scan_point.conf"
um() { awk -v v="$1" 'BEGIN { printf "%d", v * 1000 + (v < 0 ? -0.5 : 0.5) }' ; }
printf '%d\n' 1 "$FREQ_CODE" "$RECORD_LENGTH" "$POST_TRIGGER" "$SCAN_ID" "$POINT" "$NPOINTS" \
    "$(um "$TARGET_X")" "$(um "$TARGET_Y")" > "$NUM"
exit 0
