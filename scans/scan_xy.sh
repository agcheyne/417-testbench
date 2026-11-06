#! /bin/bash
#
# XY scan: move the stage to each point and take one rcdaq run there.
#
#   scan_xy.sh WHERE (-n EVENTS | -t SECONDS) [options]
#   scan_xy.sh --quit                  ask a running scan to stop after its current point
#
# Examples (add --dry-run first: it prints the points and checks them; nothing moves):
#   scan_xy.sh --pad 5,6 --step 0.4 -n 20000          pad row 5, column 6 on a 0.4 mm grid
#   scan_xy.sh --channel 20 --step 0.4 -n 20000       the pad that V1742 input 20 reads
#   scan_xy.sh --rows 2-5 --step 1 -n 20000           rows 2 to 5, all columns, 1 mm grid
#   scan_xy.sh --detector -n 2000                     the centre of every pad
#   scan_xy.sh -x 58.6:71.6:0.1 -y 83.9 -n 20000      a line, in stage mm
#
# WHERE, on the selected detector (testbench.sh show), pads numbered as on the panel's laser face:
#   --pad R,C          one pad, by row and column
#   --channel N        the pad that V1742 input N reads (mapping.txt and cabling.txt of the detector)
#   --rows A[-B]       a block of pads: these rows (default: all) ...
#   --cols A[-B]       ... and these columns (default: all)
#   --detector         every pad
#   --step S           mm between points: a grid over the pads, edge to edge and centred on them;
#                      without --step, one point at the centre of each pad
# or in stage mm:
#   -x, -y SPEC        a single value, or START:STOP:STEP (STOP included when it falls on the grid)
#   --points FILE      "x y" per line
#
# Options:
#   -n EVENTS          events per point (run ends by itself)
#   -t SECONDS         seconds per point
#   -r, --runtype T    rcdaq run type (default: RUNTYPE of the selected detector)
#   --name TEXT        label for the scan (folder name, packet 910; default from WHERE, e.g. Photonis_pad5-6_0.4mm)
#   --order snake|raster   snake (default) runs Y back and forth; raster always runs Y upwards
#   --settle S         wait after each move before starting the run (default 1 s)
#   --max-point-time S give up on a point after S seconds (default 3600; -n with no triggers)
#   --rest X,Y         where to leave the stage at the end (default REST_X,REST_Y in db/testbench.conf)
#   --no-rest          leave the stage at the last point
#   --notify "ADDR.."  mail these when the scan ends (default NOTIFY_EMAIL in db/testbench.conf)
#   --keep-laser       leave the PiLas on at the end (default: switched off, however the scan ends)
#   --hv-off           switch the detector HV off at the end, however the scan ends
#                      (testbench.sh hv-off: every channel of both CAEN units)
#   --skip-hv-check    start even if the CAEN HV check reports an alarm
#   --dark             scan on purpose with the laser off or the detector HV off (dark counts,
#                      noise); without it the scan refuses to start in that case
#   --dry-run          print the plan and check it; nothing moves, no run is taken
#   --yes              do not ask before starting
#
# Each point: move X then Y (bin/motor.sh, waits for arrival), write state/scan_point.conf
# (picked up into begin-run packets 910/911 when rcdaq was set up with daq/setup_rcdaq.sh),
# daq_begin, wait for N events or T seconds, daq_end. Each scan gets its own folder,
# logs/scans/<scan>/ (<scan> = <date>_<time>_<name>): one CSV line per point goes to <scan>.csv
# there, with the run number and the position read back; <scan>.txt has the parameters. Put the
# scan's analysis output (maps, plots) in the same folder.
#
# Stopping: "scan_xy.sh --quit" (or touch state/scan.quit) finishes the current point, then
# goes to the rest position. Ctrl-C ends the current run at once, stops the motors and
# leaves the stage where it is. In every case the laser is then switched off (not with
# --keep-laser); the HV is not touched unless --hv-off.

. "$(dirname "$(readlink -f "$0")")/../lib/tb.sh"
tb_site
SCAN_PROG=scan_xy.sh
. "$TB_ROOT/lib/tb_scan.sh"

ORIG_ARGS=("$@")
[ $# -gt 0 ] || scan_usage
XSPEC= YSPEC= POINTS= PAD= CHANNEL= ROWS= COLS= WHOLE= STEP= ORDER=snake SETTLE=1
REST=$REST_X,$REST_Y HV_OFF=
while [ $# -gt 0 ] ; do
    case $1 in
        -x) XSPEC=$2 ; shift ;;
        -y) YSPEC=$2 ; shift ;;
        --points) POINTS=$2 ; shift ;;
        --pad) PAD=$2 ; shift ;;
        --channel) CHANNEL=$2 ; shift ;;
        --rows) ROWS=$2 ; shift ;;
        --cols) COLS=$2 ; shift ;;
        --detector) WHOLE=1 ;;
        --step) STEP=$2 ; shift ;;
        --order) ORDER=$2 ; shift ;;
        --rest) REST=$2 ; shift ;;
        --no-rest) REST= ;;
        --hv-off) HV_OFF=1 ;;
        -h|--help) scan_usage ;;
        *) scan_common_opt "$@" || tb_die "unknown option $1 ($SCAN_PROG -h)" ; shift $((SCAN_SHIFT - 1)) ;;
    esac
    shift
done

# --- the plan --------------------------------------------------------------------------
scan_check_common
[[ $ORDER =~ ^(snake|raster)$ ]] || tb_die "--order snake or raster"
if [ -n "$REST" ] ; then
    [[ $REST =~ ^([0-9.]+),([0-9.]+)$ ]] || tb_die "--rest X,Y"
    RX=${BASH_REMATCH[1]} RY=${BASH_REMATCH[2]}
fi
[ -z "$STEP" ] || { [[ $STEP =~ ^[0-9]*\.?[0-9]+$ ]] && tb_fcmp "$STEP" '>' 0 ; } || tb_die "--step needs mm (> 0)"

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

range() {   # VAR A[-B] MAX OPTION: VAR="A B", within 0..MAX-1 (not in $(...): tb_die must end the scan)
    [[ $2 =~ ^([0-9]+)(-([0-9]+))?$ ]] || tb_die "$4 $2: give A or A-B"
    local a=$((10#${BASH_REMATCH[1]})) b=$((10#${BASH_REMATCH[3]:-${BASH_REMATCH[1]}}))
    [ $a -le $b ] && [ $b -lt $3 ] || tb_die "$4 $2: give A-B with 0 <= A <= B <= $(($3 - 1)) ($TB_DETECTOR has $3)"
    printf -v "$1" '%s %s' $a $b
}

# a block of pads (rows R0-R1, columns C0-C1) -> XS, YS: pad centres, or a grid of STEP over them
pad_axis() {   # FIRST LAST ORIGIN DIR PITCH
    awk -v a=$1 -v b=$2 -v o=$3 -v d=$4 -v p=$5 -v s="$STEP" 'BEGIN {
        u = o + d * a * p ; v = o + d * b * p
        lo = (u < v ? u : v) ; hi = (u < v ? v : u)
        if (s == "") { for (i = 0; i <= b - a; i++) printf "%.2f\n", lo + i * p ; exit }
        lo -= p / 2 ; hi += p / 2
        n = int((hi - lo) / s + 1e-9) ; if (n < 1) n = 1
        off = (hi - lo - (n - 1) * s) / 2
        for (i = 0; i < n; i++) printf "%.2f\n", lo + off + i * s
    }'
}

NAREA=0 NMM=0   # how many WHERE options of each kind
for v in "$PAD" "$CHANNEL" "$WHOLE" "$ROWS$COLS" ; do [ -n "$v" ] && NAREA=$((NAREA + 1)) ; done
for v in "$XSPEC$YSPEC" "$POINTS" ; do [ -n "$v" ] && NMM=$((NMM + 1)) ; done
[ $((NAREA + NMM)) -le 1 ] || tb_die "give one WHERE: --pad, --channel, --rows/--cols, --detector, -x/-y or --points"
[ $((NAREA + NMM)) -eq 1 ] || tb_die "where to scan? --pad R,C, --channel N, --rows/--cols, --detector, -x/-y or --points ($SCAN_PROG -h)"
[ -z "$STEP" ] || [ $NAREA -eq 1 ] || tb_die "--step goes with the pad options; with -x/-y give START:STOP:STEP"

scan_setup scan
if [ $NAREA -eq 1 ] ; then
    for k in NROWS NCOLS PITCH_X_MM PITCH_Y_MM STAGE_X0_MM STAGE_Y0_MM ; do
        [ -n "${!k}" ] || tb_die "$TB_DETECTOR has no $k in $(tb_detector_dir)/detector.conf: scan it with -x/-y in stage mm"
    done
    tb_fcmp "$PITCH_X_MM" '>' 0 && tb_fcmp "$PITCH_Y_MM" '>' 0 || tb_die "PITCH_X_MM / PITCH_Y_MM of $TB_DETECTOR must be > 0"
    if [ -n "$PAD" ] ; then
        [[ $PAD =~ ^([0-9]+),([0-9]+)$ ]] || tb_die "--pad R,C (row, column)"
        pr=${BASH_REMATCH[1]} pc=${BASH_REMATCH[2]}
        range R $pr $NROWS --pad ; range C $pc $NCOLS --pad
        AREA="pad row ${R% *}, column ${C% *}" AREA_NAME=pad${R% *}-${C% *}
    elif [ -n "$CHANNEL" ] ; then
        [[ $CHANNEL =~ ^[0-9]+$ ]] || tb_die "--channel N (a V1742 input)"
        SLOTS=" $(tb_table "$(tb_detector_dir)/cabling.txt" | awk '{ print $2 }' | paste -sd' ') "
        RC=$(tb_table "$(tb_detector_dir)/mapping.txt" | awk -v s="$SLOTS" -v ch=$CHANNEL \
             'index(s, " " $1 " ") && $2 == ch { print $3, $4 }' | sort -u)
        case $(echo "$RC" | grep -c .) in
            1) read -r r c <<< "$RC" ; R="$r $r" C="$c $c" ;;
            0) tb_die "no pad of $TB_DETECTOR is read by input $CHANNEL with the cabling now (cabled slots:${SLOTS% })" ;;
            *) tb_die "input $CHANNEL reads several pads (rows, columns: $(echo "$RC" | paste -sd';')): give --pad R,C" ;;
        esac
        AREA="the pad V1742 input $CHANNEL reads (row ${R% *}, column ${C% *})" AREA_NAME=ch$CHANNEL
    else
        range R "${ROWS:-0-$((NROWS - 1))}" $NROWS --rows ; range C "${COLS:-0-$((NCOLS - 1))}" $NCOLS --cols
        span() { [ $1 == $2 ] && echo $1 || echo $1-$2 ; }
        AREA="pads in rows $(span $R), columns $(span $C)" AREA_NAME=rows$(span $R)_cols$(span $C)
        [ -n "$WHOLE" ] && AREA="every pad" AREA_NAME=detector
    fi
    XS=$(pad_axis $C $STAGE_X0_MM ${STAGE_X_DIR:-1} $PITCH_X_MM)
    YS=$(pad_axis $R $STAGE_Y0_MM ${STAGE_Y_DIR:-1} $PITCH_Y_MM)
    AREA+=" of $TB_DETECTOR: $( [ -n "$STEP" ] && echo "a $STEP mm grid" || echo "pad centres"), $(echo "$XS" | grep -c .) x $(echo "$YS" | grep -c .) points"
    [ -n "$LABEL" ] || scan_setup "${TB_DETECTOR}_${AREA_NAME}_$( [ -n "$STEP" ] && echo "${STEP}mm" || echo centres)"
elif [ -n "$POINTS" ] ; then
    [ -r "$POINTS" ] || tb_die "cannot read $POINTS"
    PLAN=$(tb_table "$POINTS" | awk 'NF != 2 || $1 !~ /^[0-9.]+$/ || $2 !~ /^[0-9.]+$/ { print "bad: " $0 ; next } { printf "%.2f %.2f\n", $1, $2 }')
    [[ $PLAN == *bad* ]] && tb_die "$POINTS: lines must be \"x y\" in mm ($(echo "$PLAN" | grep bad | head -1))"
else
    [ -n "$XSPEC" ] && [ -n "$YSPEC" ] || tb_die "give both -x and -y"
    XS=$(axis_values "$XSPEC") ; YS=$(axis_values "$YSPEC")
    [[ $XS == *bad* ]] && tb_die "-x $XSPEC: use a value or START:STOP:STEP"
    [[ $YS == *bad* ]] && tb_die "-y $YSPEC: use a value or START:STOP:STEP"
fi
if [ -z "$POINTS" ] ; then
    PLAN=$(i=0 ; for x in $XS ; do
               if [ $ORDER == snake ] && (( i % 2 )) ; then ys=$(echo "$YS" | tac) ; else ys=$YS ; fi
               for y in $ys ; do echo "$x $y" ; done
               i=$((i + 1))
           done)
fi
NPOINTS=$(echo "$PLAN" | grep -c .)
[ $NPOINTS -gt 0 ] || tb_die "no points"
OUT=$(echo "$PLAN" | awk -v x0=$X_MIN -v x1=$X_MAX -v y0=$Y_MIN -v y1=$Y_MAX \
      '$1 < x0 || $1 > x1 || $2 < y0 || $2 > y1 { print "  " $1 ", " $2 }')
[ -n "$OUT" ] && tb_die "points outside the stage limits X $X_MIN-$X_MAX, Y $Y_MIN-$Y_MAX mm (db/testbench.conf):
$OUT"
[ -n "$REST" ] && { tb_fcmp $RX '>=' $X_MIN && tb_fcmp $RX '<=' $X_MAX && tb_fcmp $RY '>=' $Y_MIN && tb_fcmp $RY '<=' $Y_MAX ||
                    tb_die "rest position $REST is outside the stage limits" ; }

echo "scan '$SCAN_NAME': $NPOINTS points, $(scan_per_point) per point, run type $RUNTYPE"
echo "  detector $TB_DETECTOR, DAQ profile $TB_DAQ"
[ -n "$AREA" ] && echo "  $AREA"
echo "  X $(echo "$PLAN" | awk '{ print $1 }' | sort -n | sed -n '1p;$p' | paste -sd' ' | sed 's/ / to /') mm," \
     "Y $(echo "$PLAN" | awk '{ print $2 }' | sort -n | sed -n '1p;$p' | paste -sd' ' | sed 's/ / to /') mm, order $ORDER"
[ -n "$SECONDS_PER" ] && echo "  data-taking time $(( NPOINTS * SECONDS_PER / 60 )) min plus moves"
echo "  then $( [ -n "$REST" ] && echo "rest at $RX, $RY mm" || echo "stay at the last point"), laser $( [ -n "$LASER_OFF" ] && echo "switched off" || echo "left on"), HV $( [ -n "$HV_OFF" ] && echo "switched off" || echo "left as it is")"
echo "  log $CSV"

if [ -n "$DRY" ] ; then
    echo "points (dry run, nothing moves):"
    echo "$PLAN" | awk '{ printf "  %4d  %8s %8s\n", NR - 1, $1, $2 }'
    exit 0
fi

# --- checks before anything moves ---------------------------------------------------------
scan_check_daq
"$BIN/motor.sh" pos > /dev/null || exit 1
scan_check_hv_alarm
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
scan_read_laser
[ "$LASER" == off ] && DARKWHY+=("the PiLas emission is OFF (bin/pilas.sh on --box-safe)")
if [ ${#DARKWHY[@]} -gt 0 ] ; then
    for W in "${DARKWHY[@]}" ; do echo "  - $W" >&2 ; done
    [ -n "$DARK" ] || tb_die "this scan would only see dark counts: fix the above, or add --dark if that is the point"
    tb_warn "--dark: scanning anyway"
fi
scan_confirm "start the scan?"

# --- the scan ----------------------------------------------------------------------------
scan_start "# scan $SCAN_NAME, id $SCAN_ID, started $(date '+%F %T') by $USER
# detector $TB_DETECTOR, DAQ profile $TB_DAQ, run type $RUNTYPE${AREA:+
# $AREA}
# $(scan_per_point) per point, order $ORDER, settle $SETTLE s"
echo "point,target_x_mm,target_y_mm,x_mm,y_mm,run,events,start,end,status" > "$CSV"
tb_log "scan $SCAN_NAME start: $NPOINTS points, detector $TB_DETECTOR, DAQ $TB_DAQ, run type $RUNTYPE, log $CSV"
RESULT=aborted

finish() {   # REASON
    scan_finish_daq "$1" $NPOINTS
    local hv="left as it is"
    if [ -n "$HV_OFF" ] ; then
        if timeout 60 "$BIN/testbench.sh" hv-off > /dev/null 2>&1 ; then
            hv="switched off" ; echo "HV switched off (ramping down)"
        else
            hv="NOT switched off" ; tb_warn "could not switch the HV off: bin/testbench.sh hv-off"
        fi
    fi
    if [ "$1" == interrupted ] ; then
        "$BIN/motor.sh" stop > /dev/null 2>&1
        echo "interrupted: run ended, motors stopped, stage left where it is"
    elif [ -n "$REST" ] ; then
        echo "moving to the rest position $RX, $RY"
        "$BIN/motor.sh" move $RX $RY > /dev/null || tb_warn "could not reach the rest position"
    fi
    scan_report "scan $SCAN_NAME $1" \
        "scan $SCAN_NAME $1: $DONE of $NPOINTS points, runs ${FIRST_RUN:-none}${LAST_RUN:+-$LAST_RUN}, laser $LASER_END, HV $hv, log $CSV"
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
    scan_point_command >> "$POINT_FILE"
    sleep "$SETTLE"
    scan_take_run $i 0.2 || { RESULT="failed (daq_begin at point $i)" ; break ; }   # notices the end of a -n run within 0.2 s
    echo "$i,$x,$y,$rx,$ry,$RUN,$NEV,$T0,$(date '+%F %T'),$PST" >> "$CSV"
    echo "  run $RUN, $NEV events${PST/ok/}"
    DONE=$((i + 1))
    i=$((i + 1))
done 3<<< "$PLAN"
[ $DONE == $NPOINTS ] && RESULT=finished
finish "$RESULT"
if [ $DONE -gt 0 ] ; then
    echo "next: gain map  $TB_ROOT/analysis/gain_hv/fit_gain_map.sh $SCAN_DIR"
    echo "      timing    $TB_ROOT/analysis/time_xy/time_xy.sh $SCAN_DIR"
fi
[ "$RESULT" == finished ] || [ "$RESULT" == stopped ]
