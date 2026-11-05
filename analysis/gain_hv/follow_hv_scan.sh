#! /bin/bash
#
# Fit an HV scan (bin/scan_hv.sh) while it is being taken, for the live gain curve
# (live_hv_curve.C): each point's run, as soon as scan_hv.sh has written its CSV line, gets the
# single-photoelectron fits of fit_gain_map.C (every channel, and the spot holding --contain of
# the mean charge) in a fixed window. Once the scan has ended ("# ended" in <scan>.txt, or
# state/scan_point.conf no longer its), the full fit_hv_scan.sh runs with its own window (found
# from all the runs) unless --no-final. Only reads the data; writes into the scan's folder.
#
#   analysis/gain_hv/follow_hv_scan.sh SCAN [options]
#
#   SCAN               the scan's folder (logs/scans/<scan>/), or just <scan>
#   --window FROM:TO   charge window [ns] for the live fits (default 24.1:27.3, the 2100 V window;
#                      the pulse moves a little with HV: the final fit finds its own)
#   --contain F        the spot: the channels holding F of the mean charge (default 0.95)
#   --events N         events read per run (default 20000)
#   --jobs N           runs fitted at once (default 2)
#   --no-final         skip fit_hv_scan.sh at the end
#
# Writes <scan>_live_parts/run<N>.csv / .root / _spots.csv / .log (what live_hv_curve.C reads).
. "$(dirname "$(readlink -f "$0")")/../../lib/tb.sh"
HERE=$(dirname "$(readlink -f "$0")")
tb_site

usage() { sed -n '3,21p' "$0" | sed 's/^# \{0,1\}//' ; exit 2 ; }

SCAN= WINDOW=24.1:27.3 CONTAIN=0.95 EVENTS=20000 JOBS=2 FINAL=1
while [ $# -gt 0 ] ; do
    case $1 in
        --window) WINDOW=$2 ; shift ;;
        --contain) CONTAIN=$2 ; shift ;;
        --events) EVENTS=$2 ; shift ;;
        --jobs) JOBS=$2 ; shift ;;
        --no-final) FINAL= ;;
        -h|--help) usage ;;
        -*) tb_die "unknown option $1 (follow_hv_scan.sh -h)" ;;
        *) [ -z "$SCAN" ] || tb_die "one scan at a time" ; SCAN=$1 ;;
    esac
    shift
done
[ -n "$SCAN" ] || usage
[[ $WINDOW =~ ^([0-9.]+):([0-9.]+)$ ]] || tb_die "--window FROM:TO in ns"
Q_FROM=${BASH_REMATCH[1]} Q_TO=${BASH_REMATCH[2]}
[[ $JOBS =~ ^[1-9][0-9]*$ ]] || tb_die "--jobs needs a positive integer"
if [ -d "$SCAN" ] ; then DIR=$SCAN
elif [ -d "$TB_LOGS/scans/$SCAN" ] ; then DIR=$TB_LOGS/scans/$SCAN
else tb_die "no scan $SCAN (give its folder in logs/scans/)"
fi
DIR=$(readlink -f "$DIR")
NAME=$(basename "$DIR")
CSV=$DIR/$NAME.csv
head -1 "$CSV" 2> /dev/null | grep -q '^point,target_V,.*,run,' || tb_die "no $CSV from an HV scan (bin/scan_hv.sh)"
RUNTYPE=$(sed -n 's/^# detector .*, run type \([^ ,]*\).*/\1/p' "$DIR/$NAME.txt")
SCAN_ID=$(sed -n 's/^# HV scan .*, id \([0-9]*\),.*/\1/p' "$DIR/$NAME.txt")
[ -n "$RUNTYPE" ] && [ -n "$SCAN_ID" ] || tb_die "no run type or scan id in $DIR/$NAME.txt"
make -s -C "$HERE" > /dev/null 2>&1 || tb_die "could not build $HERE (run make there to see why)"
PARTS=$DIR/${NAME}_live_parts
mkdir -p "$PARTS"
echo "HV scan $NAME: following it, window $Q_FROM-$Q_TO ns, spot $CONTAIN of the charge, $EVENTS events per run, up to $JOBS at once"

declare -A RUNNING=()
stopped=
trap 'stopped=1' INT TERM
while [ -z "$stopped" ] ; do
    ended=
    if grep -q '^# ended ' "$DIR/$NAME.txt" ; then ended=1
    elif [ "$(wc -l < "$CSV")" -gt 1 ] && ! grep -qx "SCAN_ID=$SCAN_ID" "$TB_STATE/scan_point.conf" 2> /dev/null ; then ended=1
    fi
    for pid in "${!RUNNING[@]}" ; do
        kill -0 $pid 2> /dev/null && continue
        echo "$(date +%T) run ${RUNNING[$pid]} fitted"
        unset "RUNNING[$pid]"
    done
    # runs whose CSV line is written and not started yet (column 7 = run)
    todo=$(awk -F, 'NR > 1 && $11 == "ok" { print $7 }' "$CSV" | while read -r run ; do
               [ -e "$PARTS/run$run.txt" ] || echo $run ; done)
    for run in $todo ; do
        [ ${#RUNNING[@]} -lt $JOBS ] || break
        printf '%s/%s/%s-%08d-0000.evt\n' "$DATA_ROOT" "$RUNTYPE" "$RUNTYPE" "$run" > "$PARTS/run$run.txt"
        ( cd "$HERE" && exec root -b -q -l "fit_gain_map.C(\"$PARTS/run$run.txt\", \"$PARTS/run$run\", $Q_FROM, $Q_TO, $EVENTS, \"\", $CONTAIN)" > "$PARTS/run$run.log" 2>&1 ) &
        RUNNING[$!]=$run
    done
    [ -n "$ended" ] && [ -z "$todo" ] && [ ${#RUNNING[@]} -eq 0 ] && break
    sleep 5 &
    wait $! 2> /dev/null
done
trap - INT TERM
if [ -n "$stopped" ] ; then
    for pid in "${!RUNNING[@]}" ; do pkill -P $pid 2> /dev/null ; kill $pid 2> /dev/null ; done
    echo "stopped" ; exit 1
fi
echo "$(date +%T) all runs fitted"
[ -n "$FINAL" ] && "$HERE/fit_hv_scan.sh" "$DIR" --events $EVENTS && echo "$(date +%T) final fit done"
