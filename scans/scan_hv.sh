#! /bin/bash
#
# HV (gain) scan: step one CAEN N1470 channel through a list of voltages and take one rcdaq run
# at each. Acquisition only: fit and plot the gain afterwards with
#   analysis/gain_hv/fit_hv_scan.sh logs/scans/<scan> [--spot "CH .."] [--window FROM:TO]
#
#   scan_hv.sh -v SPEC (-n EVENTS | -t SECONDS) [options]
#   scan_hv.sh --quit                  ask a running scan to stop after its current point
#
# Examples (add --dry-run first: it reads the channel and prints the plan; the HV is not touched):
#   scan_hv.sh -v 1950:2250:50 -n 20000              the detector's electrode, 1950 V to 2250 V
#   scan_hv.sh -v 2250:1950:50 -n 20000              the same, stepping down
#   scan_hv.sh -v 2100,2150,2200 -t 60 -e MCP1       three voltages, 60 s each, on electrode MCP1
#
#   -v SPEC            volts (magnitudes, as the N1470 takes them): START:STOP:STEP (STOP included
#                      when it falls on the grid; STOP < START steps down) or a list 1900,2000,2050
#   -e ELECTRODE       the hv.conf electrode to step (default: the detector's only cabled one)
#   -u UNIT -c CH      or a CAEN unit (A, B or board serial) and channel directly
#   -n EVENTS          events per point (run ends by itself)
#   -t SECONDS         seconds per point
#   -r, --runtype T    rcdaq run type (default: RUNTYPE of the selected detector)
#   --name TEXT        label for the scan (folder name, packet 910; default hvscan)
#   --settle S         wait after the ramp before each run, for the gain to settle (default 60 s)
#   --max-v V          refuse points above V (default: maxv_V of the electrode in hv.conf, else
#                      the channel's MAXV on the supply)
#   --end-v V          leave the channel at V at the end (default: its VSET before the scan)
#   --max-point-time S give up on a point after S seconds (default 3600; -n with no triggers)
#   --notify "ADDR.."  mail these when the scan ends (default NOTIFY_EMAIL in db/testbench.conf)
#   --keep-laser       leave the PiLas on at the end (default: switched off, however the scan ends)
#   --skip-hv-check    start even if the CAEN HV check reports an alarm
#   --dark             scan with the laser off on purpose (dark-count spectra)
#   --dry-run          print the plan and check it; no HV is set, no run is taken
#   --yes              do not ask before starting
#
# The channel must already be ON and at its setpoint: the scan only changes VSET, it never
# switches the HV on or off. Low light is needed (mu ~ 0.1-1 photoelectrons per pulse) so the
# pedestal and the single-photoelectron peak are both in the spectrum.
#
# Each point: set VSET, wait until the channel stops ramping and VMON is within 2 V, wait --settle,
# read VMON/IMON/STAT, write state/scan_point.conf (begin-run packets 910/911), take the run.
# Each scan gets its own folder, logs/scans/<scan>/ (<scan> = <date>_<time>_<name>), with
# <scan>.csv (one line per point), <scan>.txt (parameters, command) and <scan>_runs.txt (one
# "file VMON" line per run, what fit_hv_scan.sh reads). The fits and plots go in the same folder.
#
# Stopping: "scan_hv.sh --quit" (or touch state/scan.quit) finishes the current point. Ctrl-C ends
# the current run at once. A trip or alarm stops the scan. In every case the channel is then set
# back to --end-v (its VSET before the scan) and the laser switched off (not with --keep-laser).

. "$(dirname "$(readlink -f "$0")")/../lib/tb.sh"
tb_site
SCAN_PROG=scan_hv.sh
. "$TB_ROOT/lib/tb_scan.sh"
ANA=$TB_ROOT/analysis/gain_hv
HV_TOL=2        # V: |VMON - VSET| when the ramp counts as done

ORIG_ARGS=("$@")
[ $# -gt 0 ] || scan_usage
VSPEC= ELECTRODE= HVU= HVCH= SETTLE=60 MAXV_OPT= END_V=
while [ $# -gt 0 ] ; do
    case $1 in
        -v) VSPEC=$2 ; shift ;;
        -e) ELECTRODE=$2 ; shift ;;
        -u) HVU=$2 ; shift ;;
        -c) HVCH=$2 ; shift ;;
        --max-v) MAXV_OPT=$2 ; shift ;;
        --end-v) END_V=$2 ; shift ;;
        -h|--help) scan_usage ;;
        *) scan_common_opt "$@" || tb_die "unknown option $1 ($SCAN_PROG -h)" ; shift $((SCAN_SHIFT - 1)) ;;
    esac
    shift
done

NUM='^[0-9]+(\.[0-9]+)?$'

# --- the plan --------------------------------------------------------------------------
scan_check_common
[ -z "$MAXV_OPT" ] || [[ $MAXV_OPT =~ $NUM ]] || tb_die "--max-v needs volts"
[ -z "$END_V" ] || [[ $END_V =~ $NUM ]] || tb_die "--end-v needs volts"
[ -n "$VSPEC" ] || tb_die "give the voltages: -v START:STOP:STEP or -v V1,V2,... ($SCAN_PROG -h for examples)"
if [[ $VSPEC == *:* ]] ; then
    PLAN=$(awk -v s="$VSPEC" 'BEGIN {
        n = split(s, p, ":")
        if (n != 3 || p[3] + 0 <= 0) { print "bad" ; exit }
        d = (p[2] + 0 >= p[1] + 0) ? 1 : -1
        k = int(d * (p[2] - p[1]) / p[3] + 1e-9)
        for (i = 0; i <= k; i++) printf "%.1f\n", p[1] + d * i * p[3]
    }')
else
    PLAN=$(echo "$VSPEC" | tr ',' '\n' | awk '$1 !~ /^[0-9]+(\.[0-9]+)?$/ { print "bad" ; next } { printf "%.1f\n", $1 }')
fi
[[ $PLAN == *bad* ]] || [ -z "$PLAN" ] && tb_die "-v $VSPEC: use START:STOP:STEP (STEP > 0) or V1,V2,... in volts"
NPOINTS=$(echo "$PLAN" | grep -c .)

scan_setup hvscan
RUNLIST=${CSV%.csv}_runs.txt

# which channel: -u/-c, else the electrode's row in hv.conf
HVCONF_MAXV=-
if [ -n "$HVU$HVCH" ] ; then
    [ -n "$HVU" ] && [ -n "$HVCH" ] || tb_die "give both -u UNIT and -c CH"
    [ -z "$ELECTRODE" ] || tb_die "give -e or -u/-c, not both"
    WHAT="unit $HVU ch$HVCH"
else
    [ "$HV_SUPPLY" == n1470 ] || tb_die "detector $TB_DETECTOR has HV_SUPPLY=$HV_SUPPLY: give the CAEN channel with -u UNIT -c CH"
    ROWS=$(tb_table "$(tb_detector_dir)/hv.conf" | awk -v e="$ELECTRODE" '$2 != "-" && $3 != "-" && (e == "" || $1 == e)')
    case $(echo "$ROWS" | grep -c .) in
        1) read -r ELECTRODE HVU HVCH _ _ _ _ HVCONF_MAXV _ <<< "$ROWS" ;;
        0) tb_die "no cabled electrode ${ELECTRODE:+'$ELECTRODE' }in $(tb_detector_dir)/hv.conf: give -u UNIT -c CH" ;;
        *) tb_die "$TB_DETECTOR has several cabled electrodes: choose one with -e ($(echo "$ROWS" | awk '{ print $1 }' | paste -sd' '))" ;;
    esac
    WHAT="$ELECTRODE (unit $HVU ch$HVCH)"
fi
[[ $HVCH =~ ^[0-3]$ ]] || tb_die "CAEN channel $HVCH: must be 0-3"
HVCONF_MAXV=${HVCONF_MAXV:--}

# --- the channel as it is now --------------------------------------------------------------
hv_read() {   # PAR -> its value on the scan channel; retries while the port is busy
    local v i
    for i in 1 2 3 4 5 ; do
        v=$(timeout 30 "$BIN/n1470.sh" -u "$HVU" mon "$1" "$HVCH" 2> /dev/null) && [[ $v =~ ^[[:space:]]*([0-9.]+)[[:space:]]*$ ]] &&
            { awk -v x="${BASH_REMATCH[1]}" 'BEGIN { print x + 0 }' ; return 0 ; }
        sleep 2
    done
    return 1
}
hv_set() {    # VOLTS: set VSET and read it back
    local i
    for i in 1 2 3 4 5 ; do
        timeout 30 "$BIN/n1470.sh" -u "$HVU" set VSET "$HVCH" "$1" > /dev/null 2>&1 &&
            tb_fcmp "$(hv_read VSET)" '==' "$1" && return 0
        sleep 2
    done
    return 1
}
STAT_BITS=(ON RUP RDW OVC OVV UNV MAXV TRIP OVP OVT DIS KILL ILK NOCAL)
stat_names() { local s=$1 i out= ; for i in "${!STAT_BITS[@]}" ; do (( s >> i & 1 )) && out+="${out:+|}${STAT_BITS[$i]}" ; done ; echo "${out:-OFF}" ; }
ALARM_MASK=$(( 1<<3 | 1<<4 | 1<<5 | 1<<6 | 1<<7 | 1<<8 | 1<<9 | 1<<12 ))   # OVC OVV UNV MAXV TRIP OVP OVT ILK
RAMP_ALARM_MASK=$(( ALARM_MASK & ~(1<<4 | 1<<5) ))                         # OVV/UNV may show while ramping

# until the channel is on, not ramping and VMON within HV_TOL of TARGET; HVERR says why not
wait_hv() {   # TARGET TIMEOUT_S
    local t0=$(date +%s) st vm
    HVERR=
    while : ; do
        st=$(hv_read STAT) && vm=$(hv_read VMON) || { HVERR="cannot read unit $HVU" ; return 1 ; }
        st=$((10#$st))
        (( st & 1 )) || { HVERR="$WHAT is OFF ($(stat_names $st), VMON $vm V)" ; return 1 ; }
        (( st & RAMP_ALARM_MASK )) && { HVERR="$WHAT alarm: $(stat_names $st), VMON $vm V" ; return 1 ; }
        if (( !(st & 6) )) && tb_fcmp "$(tb_calc "($vm) - ($1)")" '<=' $HV_TOL && tb_fcmp "$(tb_calc "($1) - ($vm)")" '<=' $HV_TOL ; then
            (( st & ALARM_MASK )) && { HVERR="$WHAT alarm: $(stat_names $st), VMON $vm V" ; return 1 ; }
            return 0
        fi
        [ $(( $(date +%s) - t0 )) -ge $2 ] && { HVERR="$WHAT did not reach $1 V in $2 s (VMON $vm V, $(stat_names $st))" ; return 1 ; }
        sleep 2
    done
}
ramp_time() {   # FROM TO -> s allowed for the ramp (from RUP/RDW, plus a minute)
    awk -v a="$1" -v b="$2" -v up="$RUP" -v dn="$RDW" 'BEGIN { r = (b > a) ? up : dn ; if (r <= 0) r = 1
        d = b - a ; if (d < 0) d = -d ; printf "%d\n", d / r + 60 }'
}

echo "reading $WHAT ..."
VSET0=$(hv_read VSET) && VMON0=$(hv_read VMON) && STAT0=$(hv_read STAT) && MAXV=$(hv_read MAXV) &&
    RUP=$(hv_read RUP) && RDW=$(hv_read RDW) || tb_die "cannot read $WHAT (n1470.sh -u $HVU status)"
STAT0=$((10#$STAT0))
END_V=${END_V:-$VSET0}
LIMIT=$MAXV LIMIT_FROM="the channel's MAXV"
[ "$HVCONF_MAXV" != - ] && tb_fcmp "$HVCONF_MAXV" '<' "$LIMIT" && LIMIT=$HVCONF_MAXV LIMIT_FROM="maxv_V in hv.conf"
[ -n "$MAXV_OPT" ] && tb_fcmp "$MAXV_OPT" '<' "$LIMIT" && LIMIT=$MAXV_OPT LIMIT_FROM="--max-v"
OUT=$( (echo "$PLAN" ; echo "$END_V") | awk -v m="$LIMIT" '$1 > m + 0' | paste -sd' ')
[ -n "$OUT" ] && tb_die "$OUT V above the limit $LIMIT V ($LIMIT_FROM)"
FIRST=$(echo "$PLAN" | head -1)

echo "HV scan '$SCAN_NAME': $WHAT, $NPOINTS points, $(scan_per_point) per point, run type $RUNTYPE"
echo "  detector $TB_DETECTOR, DAQ profile $TB_DAQ"
echo "  now: VSET $VSET0 V, VMON $VMON0 V, $(stat_names $STAT0); ramps $RUP V/s up, $RDW V/s down; limit $LIMIT V ($LIMIT_FROM)"
echo "  points: $(echo "$PLAN" | paste -sd' ') V, settle $SETTLE s after each ramp"
[ -n "$SECONDS_PER" ] && echo "  data-taking time $(( NPOINTS * SECONDS_PER / 60 )) min plus $(( NPOINTS * ${SETTLE%.*} / 60 )) min settling"
echo "  then back to $END_V V, laser $( [ -n "$LASER_OFF" ] && echo "switched off" || echo "left on")"
echo "  folder $SCAN_DIR"
(( STAT0 & 1 )) || tb_die "$WHAT is OFF: switch it on and let it reach its setpoint first (the scan never switches HV on)"
(( STAT0 & 6 )) && tb_die "$WHAT is still ramping: wait until it is at its setpoint"
[ -n "$DRY" ] && { echo "dry run: no HV set, no run taken" ; exit 0 ; }

# --- checks before the HV changes ----------------------------------------------------------
scan_check_daq
scan_check_hv_alarm
scan_read_laser
if [ "$LASER" == off ] ; then
    [ -n "$DARK" ] || tb_die "the PiLas emission is OFF (bin/pilas.sh on --box-safe), or add --dark for dark-count spectra"
    tb_warn "--dark: scanning with the laser off"
fi
read -r STAGE_X STAGE_Y <<< "$("$BIN/motor.sh" pos 2> /dev/null)" || true
[[ $STAGE_X =~ $NUM && $STAGE_Y =~ $NUM ]] || { tb_warn "could not read the stage position: not stored with the scan" ; STAGE_X= STAGE_Y= ; }
scan_confirm "start the HV scan (first point: $VSET0 -> $FIRST V)?"

# --- the scan ----------------------------------------------------------------------------
scan_start "# HV scan $SCAN_NAME, id $SCAN_ID, started $(date '+%F %T') by $USER
# detector $TB_DETECTOR, DAQ profile $TB_DAQ, run type $RUNTYPE
# channel $WHAT, VSET before $VSET0 V, end $END_V V, ramps $RUP/$RDW V/s, limit $LIMIT V ($LIMIT_FROM)
# $(scan_per_point) per point, settle $SETTLE s, stage at ${STAGE_X:-?}, ${STAGE_Y:-?} mm, laser ${LASER:-?}"
echo "point,target_V,vset_V,vmon_V,imon_uA,stat,run,events,start,end,status" > "$CSV"
: > "$RUNLIST"
tb_log "HV scan $SCAN_NAME start: $WHAT, $NPOINTS points $(echo "$PLAN" | paste -sd' ') V, detector $TB_DETECTOR, run type $RUNTYPE, log $CSV"
RESULT=aborted

finish() {   # REASON
    scan_finish_daq "$1" $NPOINTS
    local hv
    echo "setting $WHAT back to $END_V V"
    if ! hv_set "$END_V" ; then
        hv="NOT set back to $END_V V" ; tb_warn "could not set VSET: n1470.sh -u $HVU set VSET $HVCH $END_V"
    elif wait_hv "$END_V" "$(ramp_time "$(hv_read VMON || echo "$END_V")" "$END_V")" ; then
        hv="back at $END_V V"
    else
        hv="VSET $END_V V, but $HVERR" ; tb_warn "$HVERR"
    fi
    scan_report "HV scan $SCAN_NAME $1" \
        "HV scan $SCAN_NAME $1: $DONE of $NPOINTS points, runs ${FIRST_RUN:-none}${LAST_RUN:+-$LAST_RUN}, HV $hv, laser $LASER_END, log $CSV"
}
trap 'finish interrupted ; exit 130' INT TERM

i=0 prev=$VMON0
while read -r v <&3 ; do
    if [ -e "$QUIT" ] ; then RESULT=stopped ; break ; fi
    echo "point $i/$NPOINTS: $v V"
    hv_set "$v" || { RESULT="failed (could not set VSET $v V)" ; break ; }
    wait_hv "$v" "$(ramp_time "$prev" "$v")" || { RESULT="failed ($HVERR)" ; tb_warn "$HVERR" ; break ; }
    sleep "$SETTLE"
    wait_hv "$v" 30 || { RESULT="failed ($HVERR)" ; tb_warn "$HVERR" ; break ; }
    vm=$(hv_read VMON) ; im=$(hv_read IMON) ; st=$(hv_read STAT)
    prev=$vm
    printf 'SCAN_ID=%s\nSCAN_NAME=%s\nSCAN_TYPE=hv\nPOINT=%d\nNPOINTS=%d\nTARGET_HV=%s\nHV_CHANNEL="%s"\n' \
        "$SCAN_ID" "$SCAN_NAME" $i $NPOINTS "$v" "$WHAT" > "$POINT_FILE"
    [ -n "$STAGE_X" ] && printf 'TARGET_X=%s\nTARGET_Y=%s\n' "$STAGE_X" "$STAGE_Y" >> "$POINT_FILE"
    scan_point_command >> "$POINT_FILE"
    scan_take_run $i 1 || { RESULT="failed (daq_begin at point $i)" ; break ; }
    pst=$PST
    # did the channel stay on and quiet during the run?
    st2=$(hv_read STAT) && st2=$((10#$st2)) || st2=-1
    if (( st2 < 0 )) ; then
        pst=hv-unread ; tb_warn "point $i: could not read $WHAT after the run"
    elif (( !(st2 & 1) || (st2 & ALARM_MASK) )) ; then
        pst="hv-$(stat_names $st2)" ; RESULT="failed ($WHAT $(stat_names $st2) after run $RUN)"
    fi
    echo "$i,$v,$(hv_read VSET),$vm,$im,$((10#$st)),$RUN,$NEV,$T0,$(date '+%F %T'),$pst" >> "$CSV"
    [[ $pst == hv-[A-Z]* ]] || printf "$RULE %s\n" "$RUN" 0 "$vm" >> "$RUNLIST"
    echo "  run $RUN, $NEV events, VMON $vm V, IMON $im uA${pst/ok/}"
    [[ $pst == hv-[A-Z]* ]] && { tb_warn "$RESULT: run $RUN left out of the gain fits" ; break ; }
    DONE=$((i + 1))
    i=$((i + 1))
done 3<<< "$PLAN"
[ $DONE == $NPOINTS ] && RESULT=finished
finish "$RESULT"
[ $DONE -gt 0 ] && echo "next: gain fits and plots  $ANA/fit_hv_scan.sh $SCAN_DIR [--spot \"CH ..\"]"
[ "$RESULT" == finished ] || [ "$RESULT" == stopped ]
