# Shared parts of the rcdaq scans in scans/ (scan_xy.sh, scan_hv.sh): the options both take, the
# checks before a scan, one rcdaq run per point, and the end of a scan. Source it after lib/tb.sh
# and tb_site; set SCAN_PROG (the script's name, for messages and the scan's .txt) first.
#
# Common options (each scan's -h lists them):
#   -n EVENTS  -t SECONDS  -r/--runtype T  --name TEXT  --settle S  --max-point-time S  --notify "ADDR.."
#   --keep-laser  --skip-hv-check  --dark  --dry-run  --yes  --quit

BIN=$TB_ROOT/bin
QUIT=$TB_STATE/scan.quit
POINT_FILE=$TB_STATE/scan_point.conf

MAXT=3600 EVENTS= SECONDS_PER= RUNTYPE_OPT= LABEL= DRY= YES= HVCHECK=1 NOTIFY=$NOTIFY_EMAIL LASER_OFF=1 DARK=
IN_RUN= DONE=0 FIRST_RUN= LAST_RUN=

# the comment block at the top of the scan, without the "#"
scan_usage() { awk 'NR > 2 && !/^#/ { exit } NR > 2 { sub(/^# ?/, "") ; print }' "$0" ; exit 2 ; }

# a common option in $1 (its value in $2): sets it and SCAN_SHIFT (how many arguments it took);
# returns 1 if $1 is not a common option
scan_common_opt() {
    SCAN_SHIFT=2
    case $1 in
        -n|-t|-r|--runtype|--name|--settle|--max-point-time|--notify)
            [ $# -ge 2 ] || tb_die "$1 needs a value ($SCAN_PROG -h)" ;;
    esac
    case $1 in
        -n) EVENTS=$2 ;;
        -t) SECONDS_PER=$2 ;;
        -r|--runtype) RUNTYPE_OPT=$2 ;;
        --name) LABEL=$2 ;;
        --settle) SETTLE=$2 ;;
        --max-point-time) MAXT=$2 ;;
        --notify) NOTIFY=$2 ;;
        *)  SCAN_SHIFT=1
            case $1 in
                --skip-hv-check) HVCHECK= ;;
                --keep-laser) LASER_OFF= ;;
                --dark) DARK=1 ;;
                --dry-run) DRY=1 ;;
                --yes) YES=1 ;;
                --quit) mkdir -p "$TB_STATE" ; touch "$QUIT" ; echo "asked the scan to stop after its current point" ; exit 0 ;;
                *) return 1 ;;
            esac ;;
    esac
}

scan_check_common() {
    [ -n "$EVENTS" ] && [ -n "$SECONDS_PER" ] && tb_die "give -n or -t, not both"
    [ -n "$EVENTS$SECONDS_PER" ] || tb_die "give -n EVENTS or -t SECONDS per point ($SCAN_PROG -h for examples)"
    [ -z "$EVENTS" ] || [[ $EVENTS =~ ^[1-9][0-9]*$ ]] || tb_die "-n needs a positive integer"
    [ -z "$SECONDS_PER" ] || [[ $SECONDS_PER =~ ^[1-9][0-9]*$ ]] || tb_die "-t needs a positive integer (s)"
    [[ $SETTLE =~ ^[0-9]+(\.[0-9]+)?$ ]] || tb_die "--settle needs seconds"
    [[ $MAXT =~ ^[1-9][0-9]*$ ]] || tb_die "--max-point-time needs seconds"
}

# the selected detector's run type (or -r) and the scan's folder and files
scan_setup() {   # DEFAULT_NAME
    tb_active
    RUNTYPE= HV_SUPPLY=
    tb_conf "$(tb_detector_dir)/detector.conf"
    RUNTYPE=${RUNTYPE_OPT:-$RUNTYPE}
    SCAN_ID=$(date +%s)
    STAMP=$(date -d @$SCAN_ID +%Y%m%d_%H%M%S)
    SCAN_NAME=${LABEL:-$1}
    SCAN_NAME=${SCAN_NAME//[^A-Za-z0-9_.-]/_}
    SCAN_DIR=$TB_LOGS/scans/${STAMP}_$SCAN_NAME
    CSV=$SCAN_DIR/${STAMP}_$SCAN_NAME.csv
}

scan_per_point() { [ -n "$EVENTS" ] && echo "$EVENTS events" || echo "$SECONDS_PER s" ; }

# rcdaq ready for the scan's runs (sets RULE, rcdaq's file name rule for the run type)
scan_check_daq() {
    local status
    status=$(rcdaq_client daq_status -s 2> /dev/null) || tb_die "no rcdaq_server (systemctl --user start rcdaq-server)"
    [ "$(echo $status | awk '{ print $1 }')" == -1 ] || tb_die "rcdaq is taking run $(echo $status | awk '{ print $1 }'): end it first"
    rcdaq_client daq_list_runtypes -s 2> /dev/null | grep -qw -- "$RUNTYPE" || tb_die "rcdaq has no run type '$RUNTYPE' (daq/setup_rcdaq.sh defines them)"
    RULE=$(rcdaq_client daq_list_runtypes 2> /dev/null | awk -v t="$RUNTYPE" '$1 == t && $2 == "-" { print $3 }')
    [ -d "$(dirname "$RULE")" ] && [ -w "$(dirname "$RULE")" ] ||
        tb_die "rcdaq would write $RUNTYPE runs to $(dirname "$RULE"), which does not exist or is not writable (daq/setup_rcdaq.sh creates it)"
    rcdaq_client daq_status -l 2> /dev/null | grep -q 'Logging enabled' ||
        tb_die "rcdaq file logging is disabled: the runs would not be written (rcdaq_client daq_open)"
    local readlist=$(rcdaq_client daq_list_readlist)
    echo "$readlist" | grep -q 'Subevent id: 2000' || tb_die "rcdaq reads no V1742 (packet 2000): run daq/setup_rcdaq.sh"
    echo "$readlist" | grep -q 'Subevent id: 910' ||
        tb_warn "rcdaq was not set up by daq/setup_rcdaq.sh: the scan point is NOT stored in the data, only in $CSV"
}

scan_check_hv_alarm() {
    [ -n "$HVCHECK" ] || return 0
    "$BIN/n1470.sh" -u all check > /dev/null 2>&1
    case $? in
        0) ;;
        3) tb_die "the CAEN HV check reports an alarm (n1470.sh -u all check); --skip-hv-check to start anyway" ;;
        *) tb_warn "the CAEN HV check could not read every unit" ;;
    esac
}

# LASER: the PiLas emission, on / off, empty if it could not be read (a warning then)
scan_read_laser() {
    LASER=$(timeout 20 "$BIN/pilas.sh" status 2> /dev/null | sed -n 's/^laser emission: //p')
    case $LASER in
        on|off) ;;
        *) LASER= ; tb_warn "could not read the PiLas (port busy?): check the laser is on" ;;
    esac
}

scan_confirm() {   # QUESTION
    [ -n "$YES" ] && return
    read -r -p "$1 [y/N] " a
    [[ $a =~ ^[yY] ]] || { echo "not started" ; exit 0 ; }
}

# the scan's folder, its .txt (HEADER lines, then the command) and the run type set in rcdaq; the command
# also goes into every point's POINT_FILE (scan_point_command), so begin-run packet 910 has it
scan_point_command() { printf 'SCAN_COMMAND="%s"\n' "${SCAN_COMMAND//#/}" ; }   # (a # would start a comment)

scan_start() {   # HEADER_LINES
    mkdir -p "$SCAN_DIR" "$TB_STATE"
    rm -f "$QUIT"
    SCAN_COMMAND="$SCAN_PROG $(printf '%q ' "${ORIG_ARGS[@]}")"
    SCAN_COMMAND=${SCAN_COMMAND% }
    { echo "$1" ; echo "# command: $SCAN_COMMAND" ; } > "${CSV%.csv}.txt"
    rcdaq_client daq_set_runtype "$RUNTYPE" > /dev/null || tb_die "cannot set run type $RUNTYPE"
}

# one rcdaq run of EVENTS events or SECONDS_PER s, checking rcdaq every POLL s: RUN, NEV, T0 (start)
# and PST (ok, timeout); returns 1 if the run did not start
scan_take_run() {   # POINT POLL
    local r n rest el start
    # rcdaq counts the begin-run event too: N data events need a limit of N + 1
    rcdaq_client daq_set_maxevents $(( ${EVENTS:-0} > 0 ? EVENTS + 1 : 0 )) > /dev/null
    T0=$(date '+%F %T')
    rcdaq_client daq_begin > /dev/null || return 1
    IN_RUN=1
    RUN=$(rcdaq_client daq_status -s | awk '{ print $1 }')
    FIRST_RUN=${FIRST_RUN:-$RUN} LAST_RUN=$RUN
    NEV=0 PST=ok start=$(date +%s)
    while sleep "$2" ; do
        read -r r n rest <<< "$(rcdaq_client daq_status -s)"
        if [ "$r" == -1 ] ; then NEV=$EVENTS ; break ; fi   # ended itself at -n events
        NEV=$n
        el=$(( $(date +%s) - start ))
        [ -n "$SECONDS_PER" ] && [ $el -ge $SECONDS_PER ] && break
        [ $el -ge $MAXT ] && { PST=timeout ; tb_warn "point $1: only $NEV events after $MAXT s" ; break ; }
    done
    [ "$r" == -1 ] || rcdaq_client daq_end > /dev/null
    IN_RUN=
}

# the first part of every scan's finish: no run left going, no scan state, the end in the .txt
# (fit_gain_map.sh --follow and follow_hv_scan.sh wait for that line), the laser off unless --keep-laser
# (LASER_END says what happened to it)
scan_finish_daq() {   # REASON NPOINTS
    trap - INT TERM
    if [ -n "$IN_RUN" ] ; then rcdaq_client daq_end > /dev/null 2>&1 ; IN_RUN= ; fi
    rcdaq_client daq_set_maxevents 0 > /dev/null 2>&1
    rm -f "$POINT_FILE" "$QUIT"
    echo "# ended $(date '+%F %T'): $1, $DONE of $2 points" >> "${CSV%.csv}.txt"
    LASER_END="left on"
    if [ -n "$LASER_OFF" ] ; then
        if timeout 20 "$BIN/pilas.sh" off > /dev/null 2>&1 ; then
            LASER_END=off ; echo "laser switched off"
        else
            LASER_END="NOT switched off" ; tb_warn "could not switch the laser off: bin/pilas.sh off"
        fi
    fi
}

scan_report() {   # SUBJECT MESSAGE: to the bench log, the terminal and --notify
    tb_log "$2"
    echo "$2"
    [ -n "$NOTIFY" ] && echo "$2" | mail -s "417 test bench: $1" $NOTIFY
}
