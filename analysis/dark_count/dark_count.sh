#! /bin/bash
#
# Dark count rates from runs taken with the laser off (e.g. scans/scan_xy.sh --dark, or single runs):
# dark_count on each run, in parallel, then plot_dark.C over all of them.
#
#   analysis/dark_count/dark_count.sh RUN [RUN ...] [options]
#
#   --runtype T     the runs' rcdaq run type (default: the run type of the detector in use)
#   --detector D    the detector for the pixel map (default: the one in use)
#   --events N      events read per run (default: all)
#   --mask "CH .."  noisy inputs, left out of the pickup test and the average (default: MASKED_CHANNELS
#                   in db/testbench.conf; the 1 Nov Photonis readout-1 runs: "0 3 4 5 6")
#   --jobs N        runs read at once (default: the cores less one)
#
# Writes logs/runs/dark_<first>-<last>/: run<N>.csv and run<N>.root per run, dark.png and dark.txt
# (the rates per input).

. "$(dirname "$(readlink -f "$0")")/../../lib/tb.sh"
HERE=$(dirname "$(readlink -f "$0")")
tb_site

usage() { sed -n '3,16p' "$0" | sed 's/^# \{0,1\}//' ; exit 2 ; }

RUNS=() RUNTYPE= DETECTOR= EVENTS=0 MASK=- JOBS=$(( $(nproc) - 1 ))
while [ $# -gt 0 ] ; do
    case $1 in
        --runtype)  RUNTYPE=$2 ; shift ;;
        --detector) DETECTOR=$2 ; shift ;;
        --events)   EVENTS=$2 ; shift ;;
        --mask)     MASK=$2 ; shift ;;
        --jobs)     JOBS=$2 ; shift ;;
        -h|--help)  usage ;;
        -*)         tb_die "unknown option $1 (dark_count.sh -h)" ;;
        *)          [[ $1 =~ ^[0-9]+$ ]] || tb_die "$1 is not a run number" ; RUNS+=($1) ;;
    esac
    shift
done
[ ${#RUNS[@]} -gt 0 ] || usage
if [ -z "$RUNTYPE" ] || [ -z "$DETECTOR" ] ; then
    tb_active
    DETECTOR=${DETECTOR:-$TB_DETECTOR}
    [ -n "$RUNTYPE" ] || { tb_conf "$(tb_detector_dir "$DETECTOR")/detector.conf" ; }
fi
command -v root > /dev/null || tb_die "no root in PATH (source the ROOT setup first)"
make -s -C "$HERE" || tb_die "could not build $HERE"

FILES=()
for r in "${RUNS[@]}" ; do
    FILES+=("$(printf '%s/%s/%s-%08d-0000.evt' "$DATA_ROOT" "$RUNTYPE" "$RUNTYPE" "$r")")
    [ -r "${FILES[-1]}" ] || tb_die "no ${FILES[-1]}"
done
export DC=$HERE/dark_count OUT=$TB_LOGS/runs/dark_${RUNS[0]}-${RUNS[-1]} EVENTS MASK TB_ROOT
mkdir -p "$OUT"
echo "reading ${#RUNS[@]} runs, $JOBS at once, into $OUT"
for i in "${!RUNS[@]}" ; do echo "${FILES[$i]} ${RUNS[$i]}" ; done |
    xargs -P "$JOBS" -L 1 bash -c '"$DC" "$1" "$OUT/run$2.root" "$EVENTS" "$MASK" > "$OUT/run$2.csv" 2> "$OUT/run$2.log"' _ ||
    tb_die "dark_count failed for a run: see $OUT/run*.log"
( cd "$HERE" && root -b -q -l "plot_dark.C+(\"${RUNS[*]}\", \"$OUT/dark.png\", \"$OUT\", \"$DETECTOR\", \"$MASK\")" ) 2>&1 |
    grep -v -e '^Info in' -e '^Processing' -e '^(' -e 'CheckABICompatibility' -e '^  Possible C++' -e '^  Extraction of runtime' -e '^$' > "$OUT/dark.txt"
tail -n 1 "$OUT/dark.txt"
echo "$OUT/dark.png"
