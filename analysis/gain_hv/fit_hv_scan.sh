#! /bin/bash
#
# Gain vs HV of a finished HV scan (bin/scan_hv.sh): single-photoelectron fit of every run,
# then gain and mu vs HV. Only reads the data; the results go into the scan's folder.
#
#   analysis/gain_hv/fit_hv_scan.sh SCAN [options]
#
#   SCAN               the scan's folder (logs/scans/<scan>/), its .csv, or just <scan>
#   --window FROM:TO   charge window [ns] after the group trigger, for inputs 16-31; each input's
#                      window is shifted by its db/v1742 time offset (ch0-15 about -2.3 ns).
#                      Default "auto": found from the scan's own pulses - the average pulse of each
#                      run, where it is deeper than 5 % of its minimum +- 0.2 ns, united over all
#                      HV points (the pulse moves with HV) - and printed with each run's pulse time
#   --spot "CH CH .."  also fit the charge summed over these channels (photoelectrons shared
#                      between pads), e.g. "20 12 19 21 22 23" for the Photonis spot on ch20
#   --ch "CH .."       channels to plot (default: every channel with gain points at 3 or more HV
#                      values, at most 8, except MASKED_CHANNELS of db/testbench.conf; 32 = spot)
#   --events N         events read per run (default 20000)
#
# Reads <scan>_runs.txt (one "file.evt VMON" per point, written by scan_hv.sh) and writes, in the
# scan's folder:
#   <scan>_gain.png         gain vs HV (log) and mu vs HV next to it, points with their fit errors
#   <scan>_spectra_<ch>.png every run's charge spectrum with its fit
#   <scan>_fits.csv/.root   the fit table, the spectra and the gain curves
#   <scan>_fits.log         the full ROOT output
# Running it again (e.g. with another --window or --spot) overwrites these.
# Interactive instead: ./gain_hv.sh, then add_runs("<scan>_runs.txt") (see gain_hv.cc).

. "$(dirname "$(readlink -f "$0")")/../../lib/tb.sh"
HERE=$(dirname "$(readlink -f "$0")")
MASKED_CHANNELS=
tb_site

usage() { sed -n '3,27p' "$0" | sed 's/^# \{0,1\}//' ; exit 2 ; }

SCAN= WINDOW=auto SPOT= PLOT_CH= EVENTS=20000
while [ $# -gt 0 ] ; do
    case $1 in
        --window) WINDOW=$2 ; shift ;;
        --spot) SPOT=$2 ; shift ;;
        --ch) PLOT_CH=$2 ; shift ;;
        --events) EVENTS=$2 ; shift ;;
        -h|--help) usage ;;
        -*) tb_die "unknown option $1 (fit_hv_scan.sh -h)" ;;
        *) [ -z "$SCAN" ] || tb_die "one scan at a time" ; SCAN=$1 ;;
    esac
    shift
done
[ -n "$SCAN" ] || usage
if [ "$WINDOW" == auto ] ; then
    Q_FROM=0 Q_TO=0     # fit_hv_scan.C: from >= to = find it
else
    [[ $WINDOW =~ ^([0-9.]+):([0-9.]+)$ ]] && tb_fcmp ${BASH_REMATCH[1]} '<' ${BASH_REMATCH[2]} || tb_die "--window FROM:TO in ns, or auto"
    Q_FROM=${BASH_REMATCH[1]} Q_TO=${BASH_REMATCH[2]}
fi
[[ $SPOT =~ ^[0-9\ ]*$ ]] || tb_die "--spot: channel numbers separated by spaces"
[[ $PLOT_CH =~ ^[0-9\ ]*$ ]] || tb_die "--ch: channel numbers separated by spaces"
[[ $EVENTS =~ ^[1-9][0-9]*$ ]] || tb_die "--events needs a positive integer"

# the scan's folder and name, from a folder, its .csv or the bare name
if [ -d "$SCAN" ] ; then DIR=$SCAN
elif [ -f "$SCAN" ] ; then DIR=$(dirname "$SCAN")
elif [ -d "$TB_LOGS/scans/$SCAN" ] ; then DIR=$TB_LOGS/scans/$SCAN
else tb_die "no scan $SCAN (give its folder in logs/scans/)"
fi
DIR=$(readlink -f "$DIR")
NAME=$(basename "$DIR")
PREFIX=$DIR/$NAME
LIST=${PREFIX}_runs.txt
LOG=${PREFIX}_fits.log
[ -r "$LIST" ] || tb_die "no $LIST: is $DIR an HV scan (bin/scan_hv.sh)?"
NRUNS=$(grep -c . "$LIST")
[ "$NRUNS" -ge 1 ] || tb_die "$LIST is empty: the scan took no runs"
while read -r f v ; do
    [ -r "$f" ] || tb_die "$f (in $LIST) does not exist: was rcdaq logging on?"
done < <(grep . "$LIST")
command -v root > /dev/null || tb_die "no root in PATH (source the ROOT setup first)"
make -s -C "$HERE" > /dev/null 2>&1 || tb_die "could not build $HERE (run make there to see why)"

echo "scan $NAME: $NRUNS runs, charge window $( [ "$WINDOW" == auto ] && echo "from the data" || echo "$Q_FROM-$Q_TO ns")${SPOT:+, spot $SPOT}, $EVENTS events per run (about $(( NRUNS * EVENTS / 150 / 60 + 2 )) min)"
echo "  log $LOG"
( cd "$HERE" && root -b -q -l "fit_hv_scan.C(\"$LIST\", \"$PREFIX\", $Q_FROM, $Q_TO, \"$SPOT\", $EVENTS, \"$PLOT_CH\", \"$MASKED_CHANNELS\")" ) > "$LOG" 2>&1 ||
    tb_warn "root ended with an error: see $LOG"
# the window, the fit table and the curves; the rest stays in the log
sed -n '/finding the charge window/,/^gain_hv: charge window [0-9]/p; /shifted per input\|the same for every input/p' "$LOG"
sed -n '/^  run      HV \[V\]  ch/,/^  (/p; /: gain = exp/p; /left out/p' "$LOG" | grep -v '^Info in'
for p in "${PREFIX}_gain.png" "${PREFIX}_fits.csv" ; do [ -e "$p" ] && echo "  $p" ; done
ls "${PREFIX}"_spectra_*.png 2> /dev/null | sed 's/^/  /'
[ -e "${PREFIX}_gain.png" ] || { tb_warn "no gain plot: see $LOG" ; exit 1 ; }
