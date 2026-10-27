#! /bin/bash
#
# XY scan: move the stage to each point and take one rcdaq run there.
#
#   scan_xy.sh -x START:STOP:STEP -y START:STOP:STEP (-n EVENTS | -t SECONDS) [options]
#   scan_xy.sh --points FILE (-n EVENTS | -t SECONDS) [options]     FILE: "x y" per line [mm]
#   scan_xy.sh --quit                  ask a running scan to stop after its current point
#
#   -x, -y SPEC        mm; a single value, or START:STOP:STEP (STOP included when it falls on the grid)
#   -n EVENTS          events per point (run ends by itself)
#   -t SECONDS         seconds per point
#   -r, --runtype T    rcdaq run type (default: RUNTYPE of the selected detector)
#   --name TEXT        label for the scan (log file name, packet 910)
#   --order snake|raster   snake (default) runs Y back and forth; raster always runs Y upwards
#   --settle S         wait after each move before starting the run (default 1 s)
#   --max-point-time S give up on a point after S seconds (default 3600; -n with no triggers)
#   --rest X,Y         where to leave the stage at the end (default REST_X,REST_Y in db/testbench.conf)
#   --no-rest          leave the stage at the last point
#   --notify "ADDR.."  mail these when the scan ends (default NOTIFY_EMAIL in db/testbench.conf)
#   --keep-laser       leave the PiLas on at the end (default: switched off, however the scan ends)
#   --skip-hv-check    start even if the CAEN HV check reports an alarm
#   --dark             scan on purpose with the laser off or the detector HV off (dark counts,
#                      noise); without it the scan refuses to start in that case
#   --dry-run          print the plan and check it; nothing moves, no run is taken
#   --yes              do not ask before starting
#
# Each point: move X then Y (bin/motor.sh, waits for arrival), write state/scan_point.conf
# (picked up into begin-run packets 910/911 when rcdaq was set up with daq/setup_rcdaq.sh),
# daq_begin, wait for N events or T seconds, daq_end. One CSV line per point goes to
# logs/scans/<scan>.csv, with the run number and the position read back.
#
# Stopping: "scan_xy.sh --quit" (or touch state/scan.quit) finishes the current point, then
# goes to the rest position. Ctrl-C ends the current run at once, stops the motors and
# leaves the stage where it is. In every case the laser is then switched off (not with
# --keep-laser); the HV is never touched.

. "$(dirname "$(readlink -f "$0")")/../lib/tb.sh"
BIN=$TB_ROOT/bin
QUIT=$TB_STATE/scan.quit
POINT_FILE=$TB_STATE/scan_point.conf

usage() { sed -n '3,35p' "$0" | sed 's/^# \{0,1\}//' ; exit 2 ; }

ORIG_ARGS=("$@")
tb_site
MAXT=3600 XSPEC= YSPEC= POINTS= EVENTS= SECONDS_PER= RUNTYPE_OPT= LABEL= ORDER=snake SETTLE=1
REST=$REST_X,$REST_Y DRY= YES= HVCHECK=1 NOTIFY=$NOTIFY_EMAIL LASER_OFF=1 DARK=
while [ $# -gt 0 ] ; do
    case $1 in
        -x) XSPEC=$2 ; shift ;;
        -y) YSPEC=$2 ; shift ;;
        --points) POINTS=$2 ; shift ;;
        -n) EVENTS=$2 ; shift ;;
        -t) SECONDS_PER=$2 ; shift ;;
        -r|--runtype) RUNTYPE_OPT=$2 ; shift ;;
        --name) LABEL=$2 ; shift ;;
        --order) ORDER=$2 ; shift ;;
        --settle) SETTLE=$2 ; shift ;;
        --max-point-time) MAXT=$2 ; shift ;;
        --rest) REST=$2 ; shift ;;
        --no-rest) REST= ;;
        --notify) NOTIFY=$2 ; shift ;;
        --skip-hv-check) HVCHECK= ;;
        --keep-laser) LASER_OFF= ;;
        --dark) DARK=1 ;;
        --dry-run) DRY=1 ;;
        --yes) YES=1 ;;
        --quit) mkdir -p "$TB_STATE" ; touch "$QUIT" ; echo "asked the scan to stop after its current point" ; exit 0 ;;
        -h|--help) usage ;;
        *) tb_die "unknown option $1 (scan_xy.sh -h)" ;;
    esac
    shift
done

# --- the plan --------------------------------------------------------------------------
[ -n "$EVENTS" ] && [ -n "$SECONDS_PER" ] && tb_die "give -n or -t, not both"
[ -n "$EVENTS$SECONDS_PER" ] || tb_die "give -n EVENTS or -t SECONDS per point"
[ -z "$EVENTS" ] || [[ $EVENTS =~ ^[1-9][0-9]*$ ]] || tb_die "-n needs a positive integer"
[ -z "$SECONDS_PER" ] || [[ $SECONDS_PER =~ ^[1-9][0-9]*$ ]] || tb_die "-t needs a positive integer (s)"
[[ $SETTLE =~ ^[0-9]+(\.[0-9]+)?$ ]] || tb_die "--settle needs seconds"
[[ $MAXT =~ ^[1-9][0-9]*$ ]] || tb_die "--max-point-time needs seconds"
[[ $ORDER =~ ^(snake|raster)$ ]] || tb_die "--order snake or raster"
if [ -n "$REST" ] ; then
    [[ $REST =~ ^([0-9.]+),([0-9.]+)$ ]] || tb_die "--rest X,Y"
    RX=${BASH_REMATCH[1]} RY=${BASH_REMATCH[2]}
fi

axis_values() {   # SPEC -> one value per line, %.2f
    awk -v s="$1" 'BEGIN {
        n = split(s, p, ":")
        if (n == 1 && p[1] ~ /^[0-9]+(\.[0-9]+)?$/) { printf "%.2f\n", p[1] ; exit }
        if (n != 3 || p[3] + 0 == 0) { print "bad" ; exit }
        k = int((p[2] - p[1]) / p[3] + 1e-9)
        if (k < 0) { print "bad" ; exit }
        for (i = 0; i <= k; i++) printf "%.2f\n", p[1] + i * p[3]
    }'
}

if [ -n "$POINTS" ] ; then
    [ -z "$XSPEC$YSPEC" ] || tb_die "give --points or -x/-y, not both"
    [ -r "$POINTS" ] || tb_die "cannot read $POINTS"
    PLAN=$(tb_table "$POINTS" | awk 'NF != 2 || $1 !~ /^[0-9.]+$/ || $2 !~ /^[0-9.]+$/ { print "bad: " $0 ; next } { printf "%.2f %.2f\n", $1, $2 }')
else
    [ -n "$XSPEC" ] && [ -n "$YSPEC" ] || tb_die "give -x and -y (or --points)"
    XS=$(axis_values "$XSPEC") ; YS=$(axis_values "$YSPEC")
    [[ $XS == *bad* ]] && tb_die "-x $XSPEC: use a value or START:STOP:STEP"
    [[ $YS == *bad* ]] && tb_die "-y $YSPEC: use a value or START:STOP:STEP"
    PLAN=$(i=0 ; for x in $XS ; do
               if [ $ORDER == snake ] && (( i % 2 )) ; then ys=$(echo "$YS" | tac) ; else ys=$YS ; fi
               for y in $ys ; do echo "$x $y" ; done
               i=$((i + 1))
           done)
fi
[[ $PLAN == *bad* ]] && tb_die "$POINTS: lines must be \"x y\" in mm ($(echo "$PLAN" | grep bad | head -1))"
NPOINTS=$(echo "$PLAN" | grep -c .)
[ $NPOINTS -gt 0 ] || tb_die "no points"
OUT=$(echo "$PLAN" | awk -v x0=$X_MIN -v x1=$X_MAX -v y0=$Y_MIN -v y1=$Y_MAX \
      '$1 < x0 || $1 > x1 || $2 < y0 || $2 > y1 { print "  " $1 ", " $2 }')
[ -n "$OUT" ] && tb_die "points outside the stage limits X $X_MIN-$X_MAX, Y $Y_MIN-$Y_MAX mm (db/testbench.conf):
$OUT"
[ -n "$REST" ] && { tb_fcmp $RX '>=' $X_MIN && tb_fcmp $RX '<=' $X_MAX && tb_fcmp $RY '>=' $Y_MIN && tb_fcmp $RY '<=' $Y_MAX ||
                    tb_die "rest position $REST is outside the stage limits" ; }

tb_active
RUNTYPE=
tb_conf "$(tb_detector_dir)/detector.conf"
RUNTYPE=${RUNTYPE_OPT:-$RUNTYPE}
SCAN_ID=$(date +%s)
STAMP=$(date -d @$SCAN_ID +%Y%m%d_%H%M%S)
SCAN_NAME=${LABEL:-scan}
SCAN_NAME=${SCAN_NAME//[^A-Za-z0-9_.-]/_}
CSV=$TB_LOGS/scans/${STAMP}_$SCAN_NAME.csv

echo "scan '$SCAN_NAME': $NPOINTS points, $( [ -n "$EVENTS" ] && echo "$EVENTS events" || echo "$SECONDS_PER s") per point, run type $RUNTYPE"
echo "  detector $TB_DETECTOR, DAQ profile $TB_DAQ"
echo "  X $(echo "$PLAN" | awk '{ print $1 }' | sort -n | sed -n '1p;$p' | paste -sd' ' | sed 's/ / to /') mm," \
     "Y $(echo "$PLAN" | awk '{ print $2 }' | sort -n | sed -n '1p;$p' | paste -sd' ' | sed 's/ / to /') mm, order $ORDER"
[ -n "$SECONDS_PER" ] && echo "  data-taking time $(( NPOINTS * SECONDS_PER / 60 )) min plus moves"
echo "  then $( [ -n "$REST" ] && echo "rest at $RX, $RY mm" || echo "stay at the last point"), laser $( [ -n "$LASER_OFF" ] && echo "switched off" || echo "left on")"
echo "  log $CSV"

if [ -n "$DRY" ] ; then
    echo "points (dry run, nothing moves):"
    echo "$PLAN" | awk '{ printf "  %4d  %8s %8s\n", NR - 1, $1, $2 }'
    exit 0
fi

# --- checks before anything moves ---------------------------------------------------------
STATUS=$(rcdaq_client daq_status -s 2> /dev/null) || tb_die "no rcdaq_server (daq/setup_rcdaq.sh)"
[ "$(echo $STATUS | awk '{ print $1 }')" == -1 ] || tb_die "rcdaq is taking run $(echo $STATUS | awk '{ print $1 }'): end it first"
rcdaq_client daq_list_runtypes -s 2> /dev/null | grep -qw -- "$RUNTYPE" || tb_die "rcdaq has no run type '$RUNTYPE' (daq/setup_rcdaq.sh defines them)"
RULE=$(rcdaq_client daq_list_runtypes 2> /dev/null | awk -v t="$RUNTYPE" '$1 == t && $2 == "-" { print $3 }')
[ -d "$(dirname "$RULE")" ] && [ -w "$(dirname "$RULE")" ] ||
    tb_die "rcdaq would write $RUNTYPE runs to $(dirname "$RULE"), which does not exist or is not writable (daq/setup_rcdaq.sh creates it)"
READLIST=$(rcdaq_client daq_list_readlist)
echo "$READLIST" | grep -q 'Subevent id: 2000' || tb_die "rcdaq reads no V1742 (packet 2000): run daq/setup_rcdaq.sh"
echo "$READLIST" | grep -q 'Subevent id: 910' ||
    tb_warn "rcdaq was not set up by daq/setup_rcdaq.sh: the scan point is NOT stored in the data, only in $CSV"
"$BIN/motor.sh" pos > /dev/null || exit 1
if [ -n "$HVCHECK" ] ; then
    "$BIN/n1470.sh" -u all check --quiet > /dev/null 2>&1
    case $? in
        0) ;;
        3) tb_die "the CAEN HV check reports an alarm (n1470.sh -u all check); --skip-hv-check to start anyway" ;;
        *) tb_warn "the CAEN HV check could not read every unit" ;;
    esac
fi
# the detector's electrodes (hv.conf rows with a unit and channel) must be on and ramped,
# and the laser on: otherwise every run is dark data
DARKWHY=()
if [ "$HV_SUPPLY" == n1470 ] ; then
    while read -r EL U C REST_OF_ROW ; do
        [ "$U" == - ] || [ "$C" == - ] && continue
        ROW=$(timeout 30 "$BIN/n1470.sh" -u "$U" status 2> /dev/null | awk -v c="$C" '$1 == c && NF >= 10 { print $2, $3, $10 ; exit }')
        if [ -z "$ROW" ] ; then
            tb_warn "could not read CAEN unit $U ch$C ($EL): check its HV is on"
            continue
        fi
        read -r VS VM ST <<< "$ROW"
        if (( !(10#$ST & 1) )) ; then
            DARKWHY+=("HV $EL (unit $U ch$C) is OFF")
        elif tb_fcmp "$(tb_calc "($VS) - ($VM)")" '>' 10 || tb_fcmp "$(tb_calc "($VM) - ($VS)")" '>' 10 ; then
            DARKWHY+=("HV $EL (unit $U ch$C) is at $VM V, not yet at its $VS V")
        fi
    done < <(tb_table "$(tb_detector_dir)/hv.conf")
fi
LASER=$(timeout 20 "$BIN/pilas.sh" status 2> /dev/null | sed -n 's/^laser emission: //p')
case $LASER in
    on) ;;
    off) DARKWHY+=("the PiLas emission is OFF (bin/pilas.sh on --box-safe)") ;;
    *) tb_warn "could not read the PiLas (port busy?): check the laser is on" ;;
esac
if [ ${#DARKWHY[@]} -gt 0 ] ; then
    for W in "${DARKWHY[@]}" ; do echo "  - $W" >&2 ; done
    [ -n "$DARK" ] || tb_die "this scan would only see dark counts: fix the above, or add --dark if that is the point"
    tb_warn "--dark: scanning anyway"
fi
if [ -z "$YES" ] ; then
    read -r -p "start the scan? [y/N] " a
    [[ $a =~ ^[yY] ]] || { echo "not started" ; exit 0 ; }
fi

# --- the scan ----------------------------------------------------------------------------
mkdir -p "$TB_LOGS/scans" "$TB_STATE"
rm -f "$QUIT"
echo "point,target_x_mm,target_y_mm,x_mm,y_mm,run,events,start,end,status" > "$CSV"
tb_log "scan $SCAN_NAME start: $NPOINTS points, detector $TB_DETECTOR, DAQ $TB_DAQ, run type $RUNTYPE, log $CSV"
{
    echo "# scan $SCAN_NAME, id $SCAN_ID, started $(date '+%F %T') by $USER"
    echo "# detector $TB_DETECTOR, DAQ profile $TB_DAQ, run type $RUNTYPE"
    echo "# $( [ -n "$EVENTS" ] && echo "$EVENTS events" || echo "$SECONDS_PER s") per point, order $ORDER, settle $SETTLE s"
    echo "# command: scan_xy.sh $(printf '%q ' "${ORIG_ARGS[@]}")"
} > "${CSV%.csv}.txt"

rcdaq_client daq_set_runtype "$RUNTYPE" > /dev/null || tb_die "cannot set run type $RUNTYPE"
IN_RUN= DONE=0 RESULT=aborted FIRST_RUN= LAST_RUN=

finish() {   # REASON
    trap - INT TERM
    if [ -n "$IN_RUN" ] ; then rcdaq_client daq_end > /dev/null 2>&1 ; IN_RUN= ; fi
    rcdaq_client daq_set_maxevents 0 > /dev/null 2>&1
    rm -f "$POINT_FILE" "$QUIT"
    local laser="left on"
    if [ -n "$LASER_OFF" ] ; then
        if timeout 20 "$BIN/pilas.sh" off > /dev/null 2>&1 ; then
            laser=off ; echo "laser switched off"
        else
            laser="NOT switched off" ; tb_warn "could not switch the laser off: bin/pilas.sh off"
        fi
    fi
    if [ "$1" == interrupted ] ; then
        "$BIN/motor.sh" stop > /dev/null 2>&1
        echo "interrupted: run ended, motors stopped, stage left where it is"
    elif [ -n "$REST" ] ; then
        echo "moving to the rest position $RX, $RY"
        "$BIN/motor.sh" move $RX $RY > /dev/null || tb_warn "could not reach the rest position"
    fi
    local msg="scan $SCAN_NAME $1: $DONE of $NPOINTS points, runs ${FIRST_RUN:-none}${LAST_RUN:+-$LAST_RUN}, laser $laser, log $CSV"
    tb_log "$msg"
    echo "$msg"
    [ -n "$NOTIFY" ] && echo "$msg" | mail -s "417 test bench: scan $SCAN_NAME $1" $NOTIFY
}
trap 'finish interrupted ; exit 130' INT TERM

i=0
while read -r x y <&3 ; do
    if [ -e "$QUIT" ] ; then RESULT=stopped ; break ; fi
    echo "point $i/$NPOINTS: $x, $y mm"
    if ! "$BIN/motor.sh" move "$x" "$y" > /dev/null ; then RESULT="failed (move to $x, $y)" ; break ; fi
    read -r rx ry <<< "$("$BIN/motor.sh" pos)"
    printf 'SCAN_ID=%s\nSCAN_NAME=%s\nPOINT=%d\nNPOINTS=%d\nTARGET_X=%s\nTARGET_Y=%s\n' \
        "$SCAN_ID" "$SCAN_NAME" $i $NPOINTS "$x" "$y" > "$POINT_FILE"
    sleep "$SETTLE"
    # rcdaq counts the begin-run event too: N data events need a limit of N + 1
    rcdaq_client daq_set_maxevents $(( ${EVENTS:-0} > 0 ? EVENTS + 1 : 0 )) > /dev/null
    t0=$(date '+%F %T')
    rcdaq_client daq_begin > /dev/null || { RESULT="failed (daq_begin at point $i)" ; break ; }
    IN_RUN=1
    run=$(rcdaq_client daq_status -s | awk '{ print $1 }')
    FIRST_RUN=${FIRST_RUN:-$run} LAST_RUN=$run
    nev=0 start=$(date +%s) st=ok
    while sleep 1 ; do
        read -r r n rest <<< "$(rcdaq_client daq_status -s)"
        if [ "$r" == -1 ] ; then nev=$EVENTS ; break ; fi   # ended itself at -n events
        nev=$n
        el=$(( $(date +%s) - start ))
        [ -n "$SECONDS_PER" ] && [ $el -ge $SECONDS_PER ] && break
        [ $el -ge $MAXT ] && { st=timeout ; tb_warn "point $i: only $nev events after $MAXT s" ; break ; }
    done
    [ "$r" == -1 ] || rcdaq_client daq_end > /dev/null
    IN_RUN=
    echo "$i,$x,$y,$rx,$ry,$run,$nev,$t0,$(date '+%F %T'),$st" >> "$CSV"
    echo "  run $run, $nev events${st/ok/}"
    DONE=$((i + 1))
    i=$((i + 1))
done 3<<< "$PLAN"
[ $DONE == $NPOINTS ] && RESULT=finished
finish "$RESULT"
[ "$RESULT" == finished ] || [ "$RESULT" == stopped ]
