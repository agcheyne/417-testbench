#! /bin/bash
#
# Timing map of a finished xy scan (scans/scan_xy.sh): timing_point on every point in parallel jobs
# (the core sigma and mean time of every channel's main timing peak), then plot_timing_map.C: the
# brightest channel's core sigma and time offset over the scan, and the core sigma per pad. Only reads
# the data; the results go into the scan's folder. First done on the Photonis 1 mm scans (31 Oct 2026).
#
#   analysis/timing_map/timing_map.sh SCAN [options]
#
#   SCAN             the scan's folder (logs/scans/<scan>/), its .csv, or just <scan>
#   --events N       events read per point (default 20000)
#   --jobs N         points read at once (default: the cores less one)
#   --minhits N      a channel needs N hits at a point to count there (default 200)
#   --mask "CH .."   noisy inputs left out (default: MASKED_CHANNELS in db/testbench.conf; the Photonis
#                    readout-1 scans of 31 Oct - 2 Nov: "0 3 4 5 6")
#
# Writes <scan>_timing.csv (one line per point and channel) and <scan>_timing_map.png. While it runs,
# each point's lines are in <scan>_timing_parts/, removed once they are merged.

. "$(dirname "$(readlink -f "$0")")/../../lib/tb.sh"
HERE=$(dirname "$(readlink -f "$0")")
tb_site

usage() { sed -n '3,19p' "$0" | sed 's/^# \{0,1\}//' ; exit 2 ; }

SCAN= EVENTS=20000 JOBS=$(( $(nproc) - 1 )) MINHITS=200 MASK=-
while [ $# -gt 0 ] ; do
    case $1 in
        --events)  EVENTS=$2 ; shift ;;
        --jobs)    JOBS=$2 ; shift ;;
        --minhits) MINHITS=$2 ; shift ;;
        --mask)    MASK=$2 ; shift ;;
        -h|--help) usage ;;
        -*)        tb_die "unknown option $1 (timing_map.sh -h)" ;;
        *)         [ -z "$SCAN" ] || tb_die "one scan at a time" ; SCAN=$1 ;;
    esac
    shift
done
[ -n "$SCAN" ] || usage
if [ -d "$SCAN" ] ; then DIR=$SCAN
elif [ -f "$SCAN" ] ; then DIR=$(dirname "$SCAN")
elif [ -d "$TB_LOGS/scans/$SCAN" ] ; then DIR=$TB_LOGS/scans/$SCAN
else tb_die "no scan $SCAN (give its folder in logs/scans/)"
fi
DIR=$(readlink -f "$DIR")
NAME=$(basename "$DIR")
PREFIX=$DIR/$NAME
head -1 "$PREFIX.csv" 2> /dev/null | grep -q '^point,target_x_mm,target_y_mm,x_mm,y_mm,run,' ||
    tb_die "no $PREFIX.csv from an xy scan (scans/scan_xy.sh)"
RUNTYPE=$(sed -n 's/^# detector .*, run type \([^ ,]*\).*/\1/p' "$PREFIX.txt" 2> /dev/null)
[ -n "$RUNTYPE" ] || tb_die "no run type in $PREFIX.txt"
command -v root > /dev/null || tb_die "no root in PATH (source the ROOT setup first)"
make -s -C "$HERE" || tb_die "could not build $HERE"

# every point taken: "point run", one timing_point each
export TP=$HERE/timing_point PARTS=${PREFIX}_timing_parts EVENTS DATA_ROOT RUNTYPE
rm -rf "$PARTS" ; mkdir -p "$PARTS"
awk -F, 'NR > 1 && $10 == "ok" { print $1, $6 }' "$PREFIX.csv" > "$PARTS/points.txt"
NPTS=$(grep -c . "$PARTS/points.txt")
[ "$NPTS" -gt 0 ] || tb_die "$PREFIX.csv has no points with status ok"
echo "scan $NAME: $NPTS points, $EVENTS events each, $JOBS at once"
xargs -P "$JOBS" -L 1 bash -c 'f=$(printf "%s/%s/%s-%08d-0000.evt" "$DATA_ROOT" "$RUNTYPE" "$RUNTYPE" "$1")
                                "$TP" "$f" "$0" "$EVENTS" > "$PARTS/p$0.csv" 2> /dev/null' < "$PARTS/points.txt"

# merge, with each point's position (the stage readback of the scan's CSV)
OUT=${PREFIX}_timing.csv
{ echo "point,x_mm,y_mm,ch,events,hits,amp_mV,t_mean_ns,t_mean_err,sigma_ns,sigma_err,rest_frac,late_frac,ok"
  awk -F, 'FILENAME ~ /_timing_parts\// { if (NF == 12) print $1 "," x[$1] "," y[$1] "," substr($0, length($1) + 2) ; next }
           FNR > 1 { x[$1] = $4 ; y[$1] = $5 }' "$PREFIX.csv" "$PARTS"/p*.csv
} > "$OUT"
NFIT=$(( $(wc -l < "$OUT") - 1 ))
[ "$NFIT" -gt 0 ] || tb_die "no channel fits: see $PARTS"
rm -rf "$PARTS"
echo "  $NFIT channel fits in $OUT"
( cd "$HERE" && root -b -q -l "plot_timing_map.C+(\"$OUT\", \"$PREFIX\", $MINHITS, \"$MASK\")" ) 2>&1 |
    grep '^ch\|Info in <TCanvas::Print>' | sed 's/^/  /'
