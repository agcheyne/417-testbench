#! /bin/bash
#
# Gain map of a finished xy scan (bin/scan_xy.sh): single-photoelectron fit at every point of the
# charge summed over all good channels ("all"), as photoelectrons share their charge between pads
# (a single pad gets ~0.8 of it even at a pixel centre), with the common mode subtracted per event
# and noisy channels left out (gain_hv set_auto_spot); also of the spot (the pads holding 95 % of
# the mean charge) and of every channel alone, and the gain from the mean charge without a fit.
# Several ROOT jobs at once; then maps of the spot's mu and gain. Only reads the data; the results
# go into the scan's folder. Needs enough events per point for a photoelectron peak (a few
# thousand pulses: 20000 events at mu ~0.05). Method and tests: 30 Oct 2026, on the Photonis
# 1 mm scan (Q1 of the 90 %, 95 %, 99 % and all-channel sums agreed within 1-3 % at pixel centres).
#
#   analysis/gain_hv/fit_gain_map.sh SCAN [options]
#
#   SCAN               the scan's folder (logs/scans/<scan>/), its .csv, or just <scan>
#   --window FROM:TO   charge window [ns] after the group trigger, for inputs 16-31; each input's
#                      window is shifted by its db/v1742 time offset (ch0-15 about -2.3 ns).
#                      Default 24.1:27.3, from the Photonis HV scan at 2100 V (29 Oct 2026)
#   --syst "FROM:TO .."  other windows fitted alongside for the systematic error of the window:
#                      gain_sys_lo/hi = the largest drop/rise of Q1 in them, as gain. Default
#                      "24.1:27.0 24.1:33.0": ending at the pulse's zero crossing, and taking its slow
#                      tail (+6-12 % at pixel centres, window survey 30 Oct 2026); "" = none
#   --contain F        the spot at each point: the smallest set of good channels holding F of the
#                      mean charge per event (default 0.95; 0 = channels alone only, no sums)
#   --events N         events read per point (default 20000)
#   --jobs N           ROOT jobs at once (default: the cores less one); each reads ~360 events/s
#   --points FROM:TO   only these points (the scan's point numbers, both included)
#   --zmax G           top of the gain colour scale (default: the largest gain)
#   --resume           keep the chunks already fitted (same window and events) and do the rest
#   --merge-only       no fitting: merge and plot the chunks fitted so far
#
# The points are fitted in chunks of 10, each chunk's table kept as soon as it is done. Ctrl-C,
# or kill $(cat <scan>_gain_map_parts/driver.pid) when it runs in the background, stops the jobs,
# then merges and plots the finished chunks; --resume goes on from there.
#
# Writes, in the scan's folder:
#   <scan>_gain_map.csv     every fit with a photoelectron peak above the pedestal: point, x_mm,
#                           y_mm, then gain_hv's columns (run, ch = channel or "spot", gain, mu,
#                           q1, s0, s1, ...), peak = 1 when the fit is usable and the peak clearly
#                           resolved (s1 < 0.8 q1 and q1 > 5 s0; the maps use only those),
#                           gain_sys_lo/hi (both positive, empty without --syst fits; for "all" the
#                           spot's), spot_channels and contained (spot rows: its share of the mean
#                           charge), gain_meanq (all rows: (mean of the sum - fitted pedestal) /
#                           (mu e), no peak shape assumed), and each --syst window's Q1
#   <scan>_gain_map.png     mu and gain of the spot per point, and the sum of all against it; per
#                           pad (the brightest channel) the gain of the spot with the window
#                           systematic, of the sum of all, from the mean charge, and of the pad alone
#   <scan>_gain_map.log     the settings, the per-channel table and the ROOT output of the plot
#   <scan>_gain_map_parts/  each chunk's run list, fit table, spectra (.root) and log
# Running it again without --resume or --merge-only starts again and overwrites these.

. "$(dirname "$(readlink -f "$0")")/../../lib/tb.sh"
HERE=$(dirname "$(readlink -f "$0")")
tb_site

usage() { sed -n '3,48p' "$0" | sed 's/^# \{0,1\}//' ; exit 2 ; }

SCAN= WINDOW=24.1:27.3 SYST="24.1:27.0 24.1:33.0" CONTAIN=0.95 EVENTS=20000 JOBS=$(( $(nproc) - 1 )) ZMAX=0 POINTS= MODE=fresh
CHUNK=10
while [ $# -gt 0 ] ; do
    case $1 in
        --window) WINDOW=$2 ; shift ;;
        --syst) SYST=$2 ; shift ;;
        --contain) CONTAIN=$2 ; shift ;;
        --events) EVENTS=$2 ; shift ;;
        --jobs) JOBS=$2 ; shift ;;
        --points) POINTS=$2 ; shift ;;
        --zmax) ZMAX=$2 ; shift ;;
        --resume) MODE=resume ;;
        --merge-only) MODE=merge ;;
        -h|--help) usage ;;
        -*) tb_die "unknown option $1 (fit_gain_map.sh -h)" ;;
        *) [ -z "$SCAN" ] || tb_die "one scan at a time" ; SCAN=$1 ;;
    esac
    shift
done
[ -n "$SCAN" ] || usage
[[ $WINDOW =~ ^([0-9.]+):([0-9.]+)$ ]] && tb_fcmp ${BASH_REMATCH[1]} '<' ${BASH_REMATCH[2]} || tb_die "--window FROM:TO in ns"
Q_FROM=${BASH_REMATCH[1]} Q_TO=${BASH_REMATCH[2]}
for w in $SYST ; do
    [[ $w =~ ^([0-9.]+):([0-9.]+)$ ]] && tb_fcmp ${BASH_REMATCH[1]} '<' ${BASH_REMATCH[2]} || tb_die "--syst: $w is not FROM:TO in ns"
done
[[ $CONTAIN =~ ^[0-9.]+$ ]] && tb_fcmp "$CONTAIN" '<=' 1 || tb_die "--contain needs a fraction, at most 1"
[[ $EVENTS =~ ^[1-9][0-9]*$ ]] || tb_die "--events needs a positive integer"
[[ $JOBS =~ ^[1-9][0-9]*$ ]] || tb_die "--jobs needs a positive integer"
[[ $ZMAX =~ ^[0-9.eE+]+$ ]] || tb_die "--zmax needs a number"
P_FROM=0 P_TO=999999
if [ -n "$POINTS" ] ; then
    [[ $POINTS =~ ^([0-9]+):([0-9]+)$ ]] && [ "${BASH_REMATCH[1]}" -le "${BASH_REMATCH[2]}" ] || tb_die "--points FROM:TO, point numbers"
    P_FROM=${BASH_REMATCH[1]} P_TO=${BASH_REMATCH[2]}
fi

if [ -d "$SCAN" ] ; then DIR=$SCAN
elif [ -f "$SCAN" ] ; then DIR=$(dirname "$SCAN")
elif [ -d "$TB_LOGS/scans/$SCAN" ] ; then DIR=$TB_LOGS/scans/$SCAN
else tb_die "no scan $SCAN (give its folder in logs/scans/)"
fi
DIR=$(readlink -f "$DIR")
NAME=$(basename "$DIR")
PREFIX=$DIR/$NAME
CSV=$PREFIX.csv
[ -r "$CSV" ] && head -1 "$CSV" | grep -q '^point,target_x_mm,target_y_mm,x_mm,y_mm,run,' ||
    tb_die "no $CSV from an xy scan (bin/scan_xy.sh)"
RUNTYPE=$(sed -n 's/^# detector .*, run type \([^ ,]*\).*/\1/p' "$PREFIX.txt" 2> /dev/null)
[ -n "$RUNTYPE" ] || tb_die "no run type in $PREFIX.txt"
command -v root > /dev/null || tb_die "no root in PATH (source the ROOT setup first)"
make -s -C "$HERE" > /dev/null 2>&1 || tb_die "could not build $HERE (run make there to see why)"

PARTS=${PREFIX}_gain_map_parts
OUT=${PREFIX}_gain_map.csv
LOG=${PREFIX}_gain_map.log
SETTINGS="window $Q_FROM-$Q_TO ns${SYST:+, systematic windows $SYST}, spot $CONTAIN of the charge, $EVENTS events per point"

# the fit tables of the finished chunks -> one table, then the plot
merge_and_plot() {
    local n
    n=$(ls "$PARTS"/chunk*.csv 2> /dev/null | grep -vc _spots)
    tr ' ' ',' < "$PARTS/points.txt" > "$PARTS/points_csv"
    [ "$n" -ge 1 ] || { tb_warn "no chunk finished: nothing to plot" ; return 1 ; }
    # the point and its position in front of gain_hv's columns (hv_V and hv_on left out: one HV
    # for the whole scan), peak, the window systematic, then the other windows' Q1 (columns 17-)
    local alt
    alt=$(ls "$PARTS"/chunk*.csv | grep -v _spots | head -1)
    alt=$(head -1 "$alt" | cut -d, -f17- -s)
    echo "point,x_mm,y_mm,run,ch,gain,gain_err,mu,mu_err,q1_pC,q1_err,q0_pC,s0_pC,s1_pC,chi2ndf,converged,used,peak,gain_sys_lo,gain_sys_hi,spot_channels,contained,gain_meanq${alt:+,$alt}" > "$OUT"
    awk -F, 'FILENAME ~ /points_csv$/ { pt[$4] = $1 "," $2 "," $3 ; next }
             FILENAME ~ /_spots\.csv$/ { if (FNR > 1) { sp[$1] = $2 ; ct[$1] = $5 ; mall[$1] = $8 } ; next }
             FNR > 1 && ($1 in pt) { peak = ($16 == 1 && $13 < 0.8 * $9 && $9 > 5 * $12) ? 1 : 0
                                     lo = hi = 0 ; n = 0
                                     for (i = 17; i < NF; i += 2) if ($i != "" && $9 > 0) {
                                         r = $i / $9 - 1 ; n++
                                         if (r < lo) lo = r
                                         if (r > hi) hi = r }
                                     # the sum of all channels: its pedestal is too wide for a fit in
                                     # the long window, so it takes the spot'"'"'s window ratios (the spot
                                     # comes first in each table)
                                     if ($4 == "spot") { slo[$1] = lo ; shi[$1] = hi ; sn[$1] = n }
                                     if ($4 == "all") { lo = slo[$1] ; hi = shi[$1] ; n = sn[$1] }
                                     printf "%s,%s,%s", pt[$1], $1, $4
                                     for (i = 5; i <= 16; i++) printf ",%s", $i
                                     printf ",%d", peak
                                     if (n) printf ",%.4g,%.4g", lo < 0 ? -lo * $5 : 0, hi * $5 ; else printf ",,"
                                     printf ",%s,%s", $4 == "spot" ? sp[$1] : "", $4 == "spot" ? ct[$1] : ""
                                     if ($4 == "all" && $7 > 0 && mall[$1] != "") printf ",%.4g", (mall[$1] - $11) / ($7 * 1.602177e-7) ; else printf ","
                                     for (i = 17; i <= NF; i++) printf ",%s", $i
                                     printf "\n" }' \
        "$PARTS/points_csv" $(ls "$PARTS"/chunk*_spots.csv 2> /dev/null) $(ls "$PARTS"/chunk*.csv | grep -v _spots) |
        sort -t, -k1,1n -k5,5n >> "$OUT"
    local npts
    npts=$(cat $(ls "$PARTS"/chunk*.csv | grep -v _spots | sed 's/\.csv$/.txt/') | grep -c .)
    echo "  $npts of $(grep -c . "$PARTS/points.txt") points fitted ($n chunks, $(cat "$PARTS/settings.txt")):" \
         "$(( $(wc -l < "$OUT") - 1 )) fits, $(awk -F, '$18 == 1' "$OUT" | wc -l) with a resolved peak, in $OUT"
    { echo "# $NAME: $npts points, $(cat "$PARTS/settings.txt")"
      ( cd "$HERE" && root -b -q -l "plot_gain_map.C(\"$OUT\", \"${PREFIX}_gain_map\", $ZMAX)" ) 2>&1 ; } > "$LOG" ||
        tb_warn "the plot ended with an error: see $LOG"
    sed -n '/^  pad  points/,/^[^ ]/p' "$LOG" | grep '^ '
    [ -e "${PREFIX}_gain_map.png" ] && echo "  ${PREFIX}_gain_map.png" || { tb_warn "no map: see $LOG" ; return 1 ; }
}

if [ "$MODE" == merge ] ; then
    [ -r "$PARTS/points.txt" ] || tb_die "no $PARTS: nothing fitted yet"
    merge_and_plot ; exit
fi
if [ "$MODE" == resume ] ; then
    [ -r "$PARTS/settings.txt" ] || tb_die "nothing to resume in $PARTS"
    [ "$(cat "$PARTS/settings.txt")" == "$SETTINGS" ] ||
        tb_die "the chunks so far have $(cat "$PARTS/settings.txt"), not $SETTINGS: start again without --resume"
else
    rm -rf "$PARTS"
    mkdir -p "$PARTS"
    echo "$SETTINGS" > "$PARTS/settings.txt"
    # the points taken (point x y run), in chunks of CHUNK files
    awk -F, -v a=$P_FROM -v b=$P_TO 'NR > 1 && $10 == "ok" && $1 >= a && $1 <= b { print $1, $4, $5, $6 }' "$CSV" > "$PARTS/points.txt"
    [ -s "$PARTS/points.txt" ] || tb_die "$CSV has no points with status ok${POINTS:+ in $POINTS}"
    while read -r p x y run ; do
        f=$(printf '%s/%s/%s-%08d-0000.evt' "$DATA_ROOT" "$RUNTYPE" "$RUNTYPE" "$run")
        [ -r "$f" ] || tb_die "$f (point $p) does not exist"
        echo "$f"
    done < "$PARTS/points.txt" | split -l $CHUNK -d -a 4 --additional-suffix=.txt - "$PARTS/chunk"
fi

echo $$ > "$PARTS/driver.pid"

# the chunks still to do: no table yet (a stopped chunk left only its log)
TODO=()
for l in "$PARTS"/chunk*.txt ; do [ -e "${l%.txt}.csv" ] || TODO+=("${l%.txt}") ; done
NPTS=$(grep -c . "$PARTS/points.txt")
NTODO=$(cat "${TODO[@]/%/.txt}" 2> /dev/null | grep -c .)
[ "$JOBS" -le "${#TODO[@]}" ] || JOBS=${#TODO[@]}
if [ "${#TODO[@]}" -eq 0 ] ; then echo "all $NPTS points fitted already" ; merge_and_plot ; exit ; fi
echo "scan $NAME: $NTODO of $NPTS points to fit, $SETTINGS, $JOBS jobs (about $(( NTODO * EVENTS / 360 / JOBS / 60 + 1 )) min)"
echo "  chunks in $PARTS; Ctrl-C stops and plots the chunks done so far, --resume goes on"
t0=$(date +%s)
printf '%s\n' "${TODO[@]}" |
    xargs -P "$JOBS" -I{} bash -c 'cd "$1" && root -b -q -l "fit_gain_map.C(\"$2.txt\", \"$2\", $3, $4, $5, \"$6\", $7)" > "$2.log" 2>&1' _ "$HERE" {} $Q_FROM $Q_TO $EVENTS "$SYST" $CONTAIN &
XARGS=$!
kill_tree() { local c ; for c in $(pgrep -P "$1") ; do kill_tree "$c" ; done ; kill "$1" 2> /dev/null ; }
stopped=
trap 'stopped=1 ; kill_tree $XARGS' INT TERM
while kill -0 $XARGS 2> /dev/null ; do
    sleep 10 &
    wait $! 2> /dev/null
    [ -n "$stopped" ] && break
    printf '\r  %d of %d points read, %d min ' "$(cat "$PARTS"/chunk*.log 2> /dev/null | grep -c '^gain_hv: run ')" "$NPTS" $(( ($(date +%s) - t0) / 60 ))
done
wait $XARGS 2> /dev/null
trap - INT TERM
echo
if [ -n "$stopped" ] ; then
    sleep 1
    for l in "${TODO[@]}" ; do [ -e "$l.csv" ] || rm -f "$l.root" "${l}_spots.csv" ; done   # a stopped chunk's half-written files
    echo "stopped: plotting the chunks finished so far (--resume goes on)"
else
    for l in "${TODO[@]}" ; do [ -e "$l.csv" ] || tb_warn "chunk $(basename "$l") wrote no table: see $l.log" ; done
fi
merge_and_plot
