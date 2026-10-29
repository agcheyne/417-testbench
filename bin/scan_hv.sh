#! /bin/bash
#
# HV (gain) scan: step one CAEN N1470 channel through a list of voltages and take one rcdaq run
# at each. Acquisition only: fit and plot the gain afterwards with
#   analysis/gain_hv/fit_hv_scan.sh logs/scans/<scan> [--spot "CH .."] [--window FROM:TO]
#
#   scan_hv.sh -v SPEC (-n EVENTS | -t SECONDS) [options]
#   scan_hv.sh --quit                  ask a running scan to stop after its current point
#
#   -v SPEC            volts (magnitudes, as the N1470 takes them): START:STOP:STEP (STOP included
#                      when it falls on the grid; STOP < START steps down) or a list 1900,2000,2050
#   -e ELECTRODE       the hv.conf electrode to step (default: the detector's only cabled one)
#   -u UNIT -c CH      or a CAEN unit (A, B or board serial) and channel directly
#   -n EVENTS          events per point (run ends by itself)
#   -t SECONDS         seconds per point
#   -r, --runtype T    rcdaq run type (default: RUNTYPE of the selected detector)
#   --name TEXT        label for the scan (log file name, packet 910)
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
BIN=$TB_ROOT/bin
ANA=$TB_ROOT/analysis/gain_hv
QUIT=$TB_STATE/scan.quit
POINT_FILE=$TB_STATE/scan_point.conf
HV_TOL=2        # V: |VMON - VSET| when the ramp counts as done

usage() { sed -n '3,42p' "$0" | sed 's/^# \{0,1\}//' ; exit 2 ; }

ORIG_ARGS=("$@")
tb_site
MAXT=3600 VSPEC= ELECTRODE= HVU= HVCH= EVENTS= SECONDS_PER= RUNTYPE_OPT= LABEL= SETTLE=60 MAXV_OPT= END_V=
DRY= YES= HVCHECK=1 NOTIFY=$NOTIFY_EMAIL LASER_OFF=1 DARK=
while [ $# -gt 0 ] ; do
    case $1 in
        -v) VSPEC=$2 ; shift ;;
        -e) ELECTRODE=$2 ; shift ;;
        -u) HVU=$2 ; shift ;;
        -c) HVCH=$2 ; shift ;;
        -n) EVENTS=$2 ; shift ;;
        -t) SECONDS_PER=$2 ; shift ;;
        -r|--runtype) RUNTYPE_OPT=$2 ; shift ;;
        --name) LABEL=$2 ; shift ;;
        --settle) SETTLE=$2 ; shift ;;
        --max-v) MAXV_OPT=$2 ; shift ;;
        --end-v) END_V=$2 ; shift ;;
        --max-point-time) MAXT=$2 ; shift ;;
        --notify) NOTIFY=$2 ; shift ;;
        --skip-hv-check) HVCHECK= ;;
        --keep-laser) LASER_OFF= ;;
        --dark) DARK=1 ;;
        --dry-run) DRY=1 ;;
        --yes) YES=1 ;;
        --quit) mkdir -p "$TB_STATE" ; touch "$QUIT" ; echo "asked the scan to stop after its current point" ; exit 0 ;;
        -h|--help) usage ;;
        *) tb_die "unknown option $1 (scan_hv.sh -h)" ;;
    esac
    shift
done

NUM='^[0-9]+(\.[0-9]+)?$'

# --- the plan --------------------------------------------------------------------------
[ -n "$EVENTS" ] && [ -n "$SECONDS_PER" ] && tb_die "give -n or -t, not both"
[ -n "$EVENTS$SECONDS_PER" ] || tb_die "give -n EVENTS or -t SECONDS per point"
[ -z "$EVENTS" ] || [[ $EVENTS =~ ^[1-9][0-9]*$ ]] || tb_die "-n needs a positive integer"
[ -z "$SECONDS_PER" ] || [[ $SECONDS_PER =~ ^[1-9][0-9]*$ ]] || tb_die "-t needs a positive integer (s)"
[[ $SETTLE =~ $NUM ]] || tb_die "--settle needs seconds"
[[ $MAXT =~ ^[1-9][0-9]*$ ]] || tb_die "--max-point-time needs seconds"
[ -z "$MAXV_OPT" ] || [[ $MAXV_OPT =~ $NUM ]] || tb_die "--max-v needs volts"
[ -z "$END_V" ] || [[ $END_V =~ $NUM ]] || tb_die "--end-v needs volts"
[ -n "$VSPEC" ] || tb_die "give the voltages: -v START:STOP:STEP or -v V1,V2,..."
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

tb_active
RUNTYPE= HV_SUPPLY=
tb_conf "$(tb_detector_dir)/detector.conf"
RUNTYPE=${RUNTYPE_OPT:-$RUNTYPE}

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
        1) read -r ELECTRODE HVU HVCH _ _ _ _ HVCONF_MAXV <<< "$ROWS" ;;
        0) tb_die "no cabled electrode ${ELECTRODE:+'$ELECTRODE' }in $(tb_detector_dir)/hv.conf: give -u UNIT -c CH" ;;
        *) tb_die "$TB_DETECTOR has several cabled electrodes: choose one with -e ($(echo "$ROWS" | awk '{ print $1 }' | paste -sd' '))" ;;
    esac
    WHAT="$ELECTRODE (unit $HVU ch$HVCH)"
fi
[[ $HVCH =~ ^[0-3]$ ]] || tb_die "CAEN channel $HVCH: must be 0-3"
HVCONF_MAXV=${HVCONF_MAXV:--}

SCAN_ID=$(date +%s)
STAMP=$(date -d @$SCAN_ID +%Y%m%d_%H%M%S)
SCAN_NAME=${LABEL:-hvscan}
SCAN_NAME=${SCAN_NAME//[^A-Za-z0-9_.-]/_}
SCAN_DIR=$TB_LOGS/scans/${STAMP}_$SCAN_NAME
CSV=$SCAN_DIR/${STAMP}_$SCAN_NAME.csv
RUNLIST=${CSV%.csv}_runs.txt

# --- the channel as it is now --------------------------------------------------------------
hv_read() {   # PAR -> its value on the scan channel; retries while the port is busy (minute check timer)
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

echo "HV scan '$SCAN_NAME': $WHAT, $NPOINTS points, $( [ -n "$EVENTS" ] && echo "$EVENTS events" || echo "$SECONDS_PER s") per point, run type $RUNTYPE"
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
STATUS=$(rcdaq_client daq_status -s 2> /dev/null) || tb_die "no rcdaq_server (daq/setup_rcdaq.sh)"
[ "$(echo $STATUS | awk '{ print $1 }')" == -1 ] || tb_die "rcdaq is taking run $(echo $STATUS | awk '{ print $1 }'): end it first"
rcdaq_client daq_list_runtypes -s 2> /dev/null | grep -qw -- "$RUNTYPE" || tb_die "rcdaq has no run type '$RUNTYPE' (daq/setup_rcdaq.sh defines them)"
RULE=$(rcdaq_client daq_list_runtypes 2> /dev/null | awk -v t="$RUNTYPE" '$1 == t && $2 == "-" { print $3 }')
[ -d "$(dirname "$RULE")" ] && [ -w "$(dirname "$RULE")" ] ||
    tb_die "rcdaq would write $RUNTYPE runs to $(dirname "$RULE"), which does not exist or is not writable (daq/setup_rcdaq.sh creates it)"
rcdaq_client daq_status -l 2> /dev/null | grep -q 'Logging enabled' ||
    tb_die "rcdaq file logging is disabled: the runs would not be written (rcdaq_client daq_open)"
READLIST=$(rcdaq_client daq_list_readlist)
echo "$READLIST" | grep -q 'Subevent id: 2000' || tb_die "rcdaq reads no V1742 (packet 2000): run daq/setup_rcdaq.sh"
echo "$READLIST" | grep -q 'Subevent id: 910' ||
    tb_warn "rcdaq was not set up by daq/setup_rcdaq.sh: the scan point is NOT stored in the data, only in $CSV"
if [ -n "$HVCHECK" ] ; then
    "$BIN/n1470.sh" -u all check --quiet > /dev/null 2>&1
    case $? in
        0) ;;
        3) tb_die "the CAEN HV check reports an alarm (n1470.sh -u all check); --skip-hv-check to start anyway" ;;
        *) tb_warn "the CAEN HV check could not read every unit" ;;
    esac
fi
LASER=$(timeout 20 "$BIN/pilas.sh" status 2> /dev/null | sed -n 's/^laser emission: //p')
case $LASER in
    on) ;;
    off) [ -n "$DARK" ] || tb_die "the PiLas emission is OFF (bin/pilas.sh on --box-safe), or add --dark for dark-count spectra"
         tb_warn "--dark: scanning with the laser off" ;;
    *) tb_warn "could not read the PiLas (port busy?): check the laser is on" ;;
esac
read -r STAGE_X STAGE_Y <<< "$("$BIN/motor.sh" pos 2> /dev/null)" || true
[[ $STAGE_X =~ $NUM && $STAGE_Y =~ $NUM ]] || { tb_warn "could not read the stage position: not stored with the scan" ; STAGE_X= STAGE_Y= ; }
if [ -z "$YES" ] ; then
    read -r -p "start the HV scan (first point: $VSET0 -> $FIRST V)? [y/N] " a
    [[ $a =~ ^[yY] ]] || { echo "not started" ; exit 0 ; }
fi

# --- the scan ----------------------------------------------------------------------------
mkdir -p "$SCAN_DIR" "$TB_STATE"
rm -f "$QUIT"
echo "point,target_V,vset_V,vmon_V,imon_uA,stat,run,events,start,end,status" > "$CSV"
: > "$RUNLIST"
tb_log "HV scan $SCAN_NAME start: $WHAT, $NPOINTS points $(echo "$PLAN" | paste -sd' ') V, detector $TB_DETECTOR, run type $RUNTYPE, log $CSV"
{
    echo "# HV scan $SCAN_NAME, id $SCAN_ID, started $(date '+%F %T') by $USER"
    echo "# detector $TB_DETECTOR, DAQ profile $TB_DAQ, run type $RUNTYPE"
    echo "# channel $WHAT, VSET before $VSET0 V, end $END_V V, ramps $RUP/$RDW V/s, limit $LIMIT V ($LIMIT_FROM)"
    echo "# $( [ -n "$EVENTS" ] && echo "$EVENTS events" || echo "$SECONDS_PER s") per point, settle $SETTLE s, stage at ${STAGE_X:-?}, ${STAGE_Y:-?} mm, laser ${LASER:-?}"
    echo "# command: scan_hv.sh $(printf '%q ' "${ORIG_ARGS[@]}")"
} > "${CSV%.csv}.txt"

rcdaq_client daq_set_runtype "$RUNTYPE" > /dev/null || tb_die "cannot set run type $RUNTYPE"
IN_RUN= DONE=0 RESULT=aborted FIRST_RUN= LAST_RUN=

finish() {   # REASON
    trap - INT TERM
    if [ -n "$IN_RUN" ] ; then rcdaq_client daq_end > /dev/null 2>&1 ; IN_RUN= ; fi
    rcdaq_client daq_set_maxevents 0 > /dev/null 2>&1
    rm -f "$POINT_FILE" "$QUIT"
    local laser="left on" hv
    if [ -n "$LASER_OFF" ] ; then
        if timeout 20 "$BIN/pilas.sh" off > /dev/null 2>&1 ; then
            laser=off ; echo "laser switched off"
        else
            laser="NOT switched off" ; tb_warn "could not switch the laser off: bin/pilas.sh off"
        fi
    fi
    echo "setting $WHAT back to $END_V V"
    if ! hv_set "$END_V" ; then
        hv="NOT set back to $END_V V" ; tb_warn "could not set VSET: n1470.sh -u $HVU set VSET $HVCH $END_V"
    elif wait_hv "$END_V" "$(ramp_time "$(hv_read VMON || echo "$END_V")" "$END_V")" ; then
        hv="back at $END_V V"
    else
        hv="VSET $END_V V, but $HVERR" ; tb_warn "$HVERR"
    fi
    local msg="HV scan $SCAN_NAME $1: $DONE of $NPOINTS points, runs ${FIRST_RUN:-none}${LAST_RUN:+-$LAST_RUN}, HV $hv, laser $laser, log $CSV"
    tb_log "$msg"
    echo "$msg"
    [ -n "$NOTIFY" ] && echo "$msg" | mail -s "417 test bench: HV scan $SCAN_NAME $1" $NOTIFY
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
    # rcdaq counts the begin-run event too: N data events need a limit of N + 1
    rcdaq_client daq_set_maxevents $(( ${EVENTS:-0} > 0 ? EVENTS + 1 : 0 )) > /dev/null
    t0=$(date '+%F %T')
    rcdaq_client daq_begin > /dev/null || { RESULT="failed (daq_begin at point $i)" ; break ; }
    IN_RUN=1
    run=$(rcdaq_client daq_status -s | awk '{ print $1 }')
    FIRST_RUN=${FIRST_RUN:-$run} LAST_RUN=$run
    nev=0 start=$(date +%s) pst=ok
    while sleep 1 ; do
        read -r r n rest <<< "$(rcdaq_client daq_status -s)"
        if [ "$r" == -1 ] ; then nev=$EVENTS ; break ; fi   # ended itself at -n events
        nev=$n
        el=$(( $(date +%s) - start ))
        [ -n "$SECONDS_PER" ] && [ $el -ge $SECONDS_PER ] && break
        [ $el -ge $MAXT ] && { pst=timeout ; tb_warn "point $i: only $nev events after $MAXT s" ; break ; }
    done
    [ "$r" == -1 ] || rcdaq_client daq_end > /dev/null
    IN_RUN=
    # did the channel stay on and quiet during the run?
    st2=$(hv_read STAT) && st2=$((10#$st2)) || st2=-1
    if (( st2 < 0 )) ; then
        pst=hv-unread ; tb_warn "point $i: could not read $WHAT after the run"
    elif (( !(st2 & 1) || (st2 & ALARM_MASK) )) ; then
        pst="hv-$(stat_names $st2)" ; RESULT="failed ($WHAT $(stat_names $st2) after run $run)"
    fi
    echo "$i,$v,$(hv_read VSET),$vm,$im,$((10#$st)),$run,$nev,$t0,$(date '+%F %T'),$pst" >> "$CSV"
    [[ $pst == hv-[A-Z]* ]] || printf "$RULE %s\n" "$run" 0 "$vm" >> "$RUNLIST"
    echo "  run $run, $nev events, VMON $vm V, IMON $im uA${pst/ok/}"
    [[ $pst == hv-[A-Z]* ]] && { tb_warn "$RESULT: run $run left out of the gain fits" ; break ; }
    DONE=$((i + 1))
    i=$((i + 1))
done 3<<< "$PLAN"
[ $DONE == $NPOINTS ] && RESULT=finished
finish "$RESULT"
[ $DONE -gt 0 ] && echo "gain fits and plots: $TB_ROOT/analysis/gain_hv/fit_hv_scan.sh $SCAN_DIR [--spot \"CH ..\"]"
[ "$RESULT" == finished ] || [ "$RESULT" == stopped ]
