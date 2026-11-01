#! /bin/bash
#
# Fill the bench panel's soft records in the N1470 IOC (docs/phoebus_panel_plan.md), as thor_epics does
# for the stage, and carry out the panel's DAQ commands.
#   bench_publisher.sh            loop: every 2 s DAQ:* and SCAN:* , every 30 s the HV channel config,
#                                 every 0.5 s the panel's DAQ commands
#   bench_publisher.sh once       one pass of the status, then exit
#
# DAQ commands (the panel writes DAQ:CMD = begin / end / open / close / apply, DAQ:RUNTYPE_SP, the
# setup pickers DAQ:SETUP_DET / DAQ:SETUP_DAQ): $RC daq_begin / daq_end / daq_open /
# daq_close / daq_set_runtype, and for apply "testbench.sh use detector", "testbench.sh use daq" and
# daq/setup_rcdaq.sh (in the background; it reconfigures the V1742). Refused while a scan runs (it
# drives the DAQ itself); run type, open / close and apply only while stopped; apply only with an
# rcdaq_server running (one started from this service would die with it). Result in DAQ:CMD_MSG,
# every command and its output in logs/daq_commands.log. (The panel's lock is the panel's: every
# command but "end" needs it unlocked there.)
# Stage (STAGE:CMD = move / home / x+ x- y+ y- / stop, targets STAGE:X_SP / Y_SP, jog STAGE:STEP): bin/motor.sh
# move / home both --yes in the background (its limits and arrival check); refused during a scan, while a run
# is going, or while a move is still running. STAGE:STOP (its own record) ends the running motor.sh (the
# panel's STOP has already written THOR_X:stop / THOR_Y:stop). Result in STAGE:MSG and logs/daq_commands.log.
# Test hooks: RCDAQ_CLIENT (another rcdaq_client), BENCH_MOTOR (another motor.sh), BENCH_DRY=echo (apply only prints its commands),
# BENCH_TEST_DIR (command log and apply output there, not in logs/ and state/).
#
# DAQ:*     from "rcdaq_client daq_status -s" and "daq_get_runtype"
# SCAN:*    from state/scan_point.conf (written by scan_xy.sh / scan_hv.sh at every point, removed at
#           the end) and the scan's CSV in logs/scans/ (ETA from the points done so far)
# N1470_U:CHn:NAME / USED / ILIM / RLOAD   from the hv.conf of the selected detector: rows with a unit
#           and channel; columns imax_uA and r_MOhm (9, 10) if given, else ILIM_DEFAULT / no resistance
# BENCH:PUB_ALIVE   written every pass (it drops to "down" 10 s after the last one)
#
# Runs as the systemd user service bench-publisher.service. Without the IOC it waits for it.

. "$(dirname "$(readlink -f "$0")")/../lib/tb.sh"

EPICS_BIN=${EPICS_BIN:-$HOME/epics/base/bin/linux-x86_64}
IOC_CA=${N1470_IOC_CA:-127.0.0.1:5070}
POINT_FILE=${BENCH_POINT_FILE:-$TB_STATE/scan_point.conf}   # (BENCH_POINT_FILE: for a test)
UNITS=(A B)
ILIM_DEFAULT=360       # uA: IMON limit of a channel without imax_uA in hv.conf (user, 1 Nov 2026)
PERIOD=2
CONFIG_EVERY=30
POLL=0.5               # s: how often the panel's DAQ commands are looked at
RC=${RCDAQ_CLIENT:-rcdaq_client}
DRY=${BENCH_DRY:-}
CMD_LOG=${BENCH_TEST_DIR:-$TB_LOGS}/daq_commands.log
APPLY_OUT=${BENCH_TEST_DIR:-$TB_STATE}/.daq_apply.out   # the background apply's output; .rc when finished
MOTOR=${BENCH_MOTOR:-$TB_ROOT/bin/motor.sh}
STAGE_OUT=${BENCH_TEST_DIR:-$TB_STATE}/.stage_move.out   # the background move's output; .rc when finished
THOR_CA=${BENCH_THOR_CA:-127.0.0.1}                      # the motor IOC (CA port 5064)
MBBO_STATES=(ZRST ONST TWST THST FRST FVST SXST SVST EIST NIST TEST ELST TVST TTST FTST FFST)

ca() { EPICS_CA_AUTO_ADDR_LIST=NO EPICS_CA_ADDR_LIST=$IOC_CA "$EPICS_BIN/$1" "${@:2}" ; }
put()  { ca caput -t -w 2 "$1" "$2" > /dev/null 2>&1 ; }
puts() { ca caput -t -S -w 2 "$1" "$2" > /dev/null 2>&1 ; }    # CHAR waveform from a string

daq() {
    local st r n vol open srv dur file rt
    st=$(timeout 5 $RC daq_status -s 2> /dev/null) || { put DAQ:RUNNING 0 ; puts DAQ:FILE "no rcdaq_server" ; return ; }
    read -r r n vol open srv dur file _ <<< "$st"
    rt=$(timeout 5 $RC daq_get_runtype 2> /dev/null | head -n 1)
    put DAQ:LOGGING "$open"
    put DAQ:RUNTYPE "${rt:--}"
    if [ -n "$RUNTYPES" ] && [ "$LAST_SP" != -2 ] && [ "$(idx "$rt" "${RUNTYPES[@]}")" != "$LAST_SP" ] ; then
        LAST_SP=$(idx "$rt" "${RUNTYPES[@]}") ; put DAQ:RUNTYPE_SP "$LAST_SP"      # changed outside the panel
    fi
    if [ "$r" == -1 ] ; then                     # stopped: the last run and its file
        put DAQ:RUNNING 0
        file=$(timeout 5 $RC daq_get_lastfilename 2> /dev/null | head -n 1)
        [[ $file =~ -0*([0-9]+)-[0-9]+\.evt$ ]] && put DAQ:RUN "${BASH_REMATCH[1]}"
        puts DAQ:FILE "${file:+last: }$file"
        put DAQ:EVENTS 0 ; put DAQ:RATE 0 ; put DAQ:DURATION 0
        return
    fi
    put DAQ:RUNNING 1
    put DAQ:RUN "$r"
    put DAQ:EVENTS "$n"
    put DAQ:DURATION "$dur"
    put DAQ:RATE "$(awk -v n="$n" -v t="$dur" 'BEGIN { print (t > 0 ? n / t : 0) }')"
    puts DAQ:FILE "$file"
}

scan() {
    local SCAN_NAME SCAN_TYPE POINT NPOINTS TARGET_X TARGET_Y TARGET_HV csv eta
    if [ ! -r "$POINT_FILE" ] ; then
        put SCAN:STATE idle
        put SCAN:ETA -
        return
    fi
    . "$POINT_FILE" 2> /dev/null
    put SCAN:STATE running
    puts SCAN:NAME "$SCAN_NAME"
    put SCAN:TYPE "${SCAN_TYPE:-xy}"
    put SCAN:POINT $(( ${POINT:-0} + 1 ))
    put SCAN:NPOINTS "${NPOINTS:-0}"
    put SCAN:X "${TARGET_X:-0}"
    put SCAN:Y "${TARGET_Y:-0}"
    put SCAN:HV "${TARGET_HV:-0}"
    # ETA: the time per point so far, from the first start to the last end in the scan's CSV
    csv=$(ls -t "$TB_LOGS"/scans/*_"$SCAN_NAME"/*_"$SCAN_NAME".csv 2> /dev/null | head -n 1)
    eta=$(awk -F, -v n="${NPOINTS:-0}" -v now="$(date +%s)" '
        NR == 1 { for (i = 1 ; i <= NF ; i++) { if ($i == "start") s = i ; if ($i == "end") e = i } ; next }
        s && e { if (first == "") first = $s ; last = $e ; done++ }
        function t(x) { gsub(/[-:]/, " ", x) ; return mktime(x) }
        END { if (done < 1 || n <= done) { print "-" ; exit }
              per = (t(last) - t(first)) / done ; left = per * (n - done)
              printf "%s (%d min)\n", strftime("%H:%M", now + left), left / 60 + 0.5 }' "$csv" 2> /dev/null)
    put SCAN:ETA "${eta:--}"
}

# names, used flags and limits of the 8 channels, from the selected detector's hv.conf
hvconfig() {
    local -A name=() ilim=() rload=()
    local e u c rest f u2 ch det
    if ( tb_active ) > /dev/null 2>&1 ; then
        tb_active
        while read -r e u c f f f f f f9 f10 _ || [ -n "$e" ] ; do
            case $u in 236) u=A ;; 1593) u=B ;; esac      # hv.conf may give the board serial (UNITS of n1470.sh)
            [[ $u == [AB] && $c =~ ^[0-3]$ ]] || continue
            name[$u$c]=$e
            [[ $f9 =~ ^[0-9.]+$ ]] && ilim[$u$c]=$f9
            [[ $f10 =~ ^[0-9.]+$ ]] && rload[$u$c]=$f10
        done < <(tb_table "$(tb_detector_dir)/hv.conf")
    fi
    for u2 in "${UNITS[@]}" ; do
        for ch in 0 1 2 3 ; do
            put N1470_$u2:CH$ch:NAME "${name[$u2$ch]:-}"
            put N1470_$u2:CH$ch:USED "$([ -n "${name[$u2$ch]}" ] && echo 1 || echo 0)"
            put N1470_$u2:CH$ch:ILIM "${ilim[$u2$ch]:-$ILIM_DEFAULT}"
            put N1470_$u2:CH$ch:RLOAD "${rload[$u2$ch]:-0}"
        done
    done
}

ioc_up() { ca caget -t -w 2 BENCH:PUB_ALIVE > /dev/null 2>&1 ; }

# ---- the panel's DAQ commands ----

idx() { local w=$1 i=0 x ; shift ; for x ; do [ "$x" == "$w" ] && { echo $i ; return ; } ; i=$((i + 1)) ; done ; echo -1 ; }

# the choices of the combos, and the pickers set to what is active now
choices_init() {
    local i
    RUNTYPES=($(timeout 5 $RC daq_list_runtypes -s 2> /dev/null))
    DETS=($(tb_detector_names))
    PROFS=() ; for i in $(tb_daq_names) ; do grep -q '^READOUT=v1742' "$(tb_daq_file $i)" && PROFS+=($i) ; done
    for i in "${!MBBO_STATES[@]}" ; do
        put DAQ:RUNTYPE_SP.${MBBO_STATES[$i]} "${RUNTYPES[$i]}"
        put DAQ:SETUP_DET.${MBBO_STATES[$i]} "${DETS[$i]}"
        put DAQ:SETUP_DAQ.${MBBO_STATES[$i]} "${PROFS[$i]}"
    done
    active
    put DAQ:SETUP_DET "$(idx "$ACTIVE_DET" "${DETS[@]}")"
    put DAQ:SETUP_DAQ "$(idx "$ACTIVE_DAQ" "${PROFS[@]}")"
    LAST_SP=-2                             # sync DAQ:RUNTYPE_SP with the next status
}

active() {
    local DETECTOR= DAQ=
    [ -r "$TB_STATE/active.conf" ] && tb_conf "$TB_STATE/active.conf"
    ACTIVE_DET=$DETECTOR ACTIVE_DAQ=$DAQ
    put DAQ:ACTIVE_DET "${DETECTOR:--}"
    put DAQ:ACTIVE_DAQ "${DAQ:--}"
}

msg() {   # TEXT [FULL OUTPUT]: to the panel and the command log
    puts DAQ:CMD_MSG "$(date +%T)  $1"
    { echo "$(date -Iseconds) $1" ; [ -n "$2" ] && sed 's/^/    /' <<< "$2" ; } >> "$CMD_LOG" 2> /dev/null
}

scan_busy() { [ -r "$POINT_FILE" ] || pgrep -f 'bin/scan_(xy|hv)\.sh' > /dev/null ; }

do_cmd() {   # begin|end|open|close|apply
    local c=$1 st run open out rc det prof
    case $c in begin|end|open|close|apply) ;; *) msg "unknown command '$c'" ; return ;; esac
    [ -e "$APPLY_OUT" ] && { msg "$c refused: the setup is still being applied" ; return ; }
    scan_busy && { msg "$c refused: a scan is running (scan_xy.sh / scan_hv.sh drive the DAQ themselves)" ; return ; }
    st=$(timeout 5 $RC daq_status -s 2> /dev/null) || { msg "$c refused: no rcdaq_server (start it with daq/setup_rcdaq.sh)" ; return ; }
    read -r run _ _ open _ <<< "$st"
    case $c in
        begin) [ "$run" == -1 ] || { msg "begin refused: run $run is going" ; return ; } ;;
        end)   [ "$run" != -1 ] || { msg "end: no run is going" ; return ; } ;;
        *)     [ "$run" == -1 ] || { msg "$c refused: run $run is going (end it first)" ; return ; } ;;
    esac
    if [ $c == apply ] ; then
        det=${DETS[$(ca caget -n -t -w 2 DAQ:SETUP_DET)]} prof=${PROFS[$(ca caget -n -t -w 2 DAQ:SETUP_DAQ)]}
        [ -n "$det" ] && [ -n "$prof" ] || { msg "apply refused: pick a detector and a DAQ profile" ; return ; }
        put DAQ:BUSY 1
        msg "apply: detector $det, DAQ profile $prof: setting up rcdaq and the V1742 ..."
        : > "$APPLY_OUT"
        ( { $DRY "$TB_ROOT/bin/testbench.sh" use detector "$det" && $DRY "$TB_ROOT/bin/testbench.sh" use daq "$prof" &&
            $DRY timeout 300 "$TB_ROOT/daq/setup_rcdaq.sh" ; } >> "$APPLY_OUT" 2>&1
          echo $? > "$APPLY_OUT.rc" ) &
        return
    fi
    put DAQ:BUSY 1
    out=$(timeout 30 $RC daq_$c 2>&1) ; rc=$?
    put DAQ:BUSY 0
    if [ $rc -eq 0 ] ; then msg "$c: done${out:+ ($(tail -n 1 <<< "$out"))}" "$out"
    else msg "$c FAILED (rc $rc): $(tail -n 1 <<< "$out")" "$out" ; fi
    daq
}

apply_done() {   # the background apply has finished: report it
    local rc out
    [ -e "$APPLY_OUT.rc" ] || return
    rc=$(cat "$APPLY_OUT.rc") out=$(cat "$APPLY_OUT")
    rm -f "$APPLY_OUT" "$APPLY_OUT.rc"
    put DAQ:BUSY 0
    if [ "$rc" == 0 ] ; then msg "apply: done ($(grep -m 1 '^setting up rcdaq' <<< "$out" || tail -n 1 <<< "$out"))" "$out"
    else msg "apply FAILED (rc $rc): $(grep -v '^ *$' <<< "$out" | tail -n 1)" "$out" ; fi
    active ; choices_init ; daq
}

# the run type picked in the panel: set it when allowed, else put the combo back
runtype_poll() {
    local sp want st run rt
    sp=$(ca caget -n -t -w 1 DAQ:RUNTYPE_SP 2> /dev/null) || return
    [ "$sp" == "$LAST_SP" ] && return
    if [ "$LAST_SP" == -2 ] ; then LAST_SP=$sp ; return ; fi      # first look: no request yet
    want=${RUNTYPES[$sp]}
    LAST_SP=$sp
    if [ -e "$APPLY_OUT" ] ; then msg "run type refused: the setup is still being applied"
    elif scan_busy ; then msg "run type refused: a scan is running"
    elif ! st=$(timeout 5 $RC daq_status -s 2> /dev/null) ; then msg "run type refused: no rcdaq_server"
    else
        read -r run _ <<< "$st"
        if [ "$run" != -1 ] ; then msg "run type refused: run $run is going"
        elif out=$(timeout 10 $RC daq_set_runtype "$want" 2>&1) ; then msg "run type: $want" "$out"
        else msg "run type $want FAILED: $(tail -n 1 <<< "$out")" "$out" ; fi
    fi
    rt=$(timeout 5 $RC daq_get_runtype 2> /dev/null | head -n 1)
    LAST_SP=$(idx "$rt" "${RUNTYPES[@]}") ; put DAQ:RUNTYPE_SP "$LAST_SP" ; put DAQ:RUNTYPE "${rt:--}"
}

# ---- the panel's stage commands ----

thor() { EPICS_CA_AUTO_ADDR_LIST=NO EPICS_CA_ADDR_LIST=$THOR_CA "$EPICS_BIN/caget" -t -w 2 "$@" 2> /dev/null ; }

smsg() {   # TEXT [FULL OUTPUT]
    puts STAGE:MSG "$(date +%T)  $1"
    { echo "$(date -Iseconds) stage: $1" ; [ -n "$2" ] && sed 's/^/    /' <<< "$2" ; } >> "$CMD_LOG" 2> /dev/null
}

stage_init() {   # the target fields start at where the stage is
    local x y
    x=$(thor THOR_X:read_pos) && put STAGE:X_SP "$x"
    y=$(thor THOR_Y:read_pos) && put STAGE:Y_SP "$y"
    put STAGE:BUSY 0 ; put STAGE:CMD "" ; put STAGE:STOP 0
}

stage_cmd() {   # move | home | x+ x- y+ y- | stop
    local c=$1 x y step st run
    if [ "$c" == stop ] ; then
        [ -e "$STAGE_OUT" ] && { touch "$STAGE_OUT.stopped" ; pkill -f "^/bin/bash $MOTOR " 2> /dev/null ; }
        smsg "STOP (THOR_X:stop / THOR_Y:stop written by the panel)" ; return
    fi
    case $c in move|home|x+|x-|y+|y-) ;; *) smsg "unknown command '$c'" ; return ;; esac
    [ -e "$STAGE_OUT" ] && { smsg "$c refused: the stage is still moving" ; return ; }
    scan_busy && { smsg "$c refused: a scan is running (it moves the stage itself)" ; return ; }
    st=$(timeout 5 $RC daq_status -s 2> /dev/null) && read -r run _ <<< "$st" && [ "$run" != -1 ] &&
        { smsg "$c refused: run $run is going (its begin-run packet has the position: end it first)" ; return ; }
    case $c in
        move) x=$(ca caget -t -w 2 STAGE:X_SP) y=$(ca caget -t -w 2 STAGE:Y_SP) ;;
        home) ;;
        *)    step=$(ca caget -t -w 2 STAGE:STEP) x=- y=-
              local pos ; pos=$(thor THOR_${c:0:1}:read_pos | tr -d ' ') || { smsg "$c refused: no position from the motor IOC" ; return ; }
              pos=$(awk -v p="$pos" -v s="$step" -v d="${c:1:1}" 'BEGIN { printf "%.2f", d == "+" ? p + s : p - s }')
              if [ ${c:0:1} == x ] ; then x=$pos ; else y=$pos ; fi
              c="jog ${c} ${step} mm" ;;
    esac
    put STAGE:BUSY 1
    : > "$STAGE_OUT"
    if [ "$c" == home ] ; then
        smsg "home both axes ..."
        ( "$MOTOR" home both --yes >> "$STAGE_OUT" 2>&1 ; echo $? > "$STAGE_OUT.rc" ) &
    else
        smsg "${c/move/move to} x $x, y $y ..."
        ( "$MOTOR" move "$x" "$y" >> "$STAGE_OUT" 2>&1 ; echo $? > "$STAGE_OUT.rc" ) &
    fi
}

stage_done() {
    local rc out x y
    [ -e "$STAGE_OUT.rc" ] || return
    rc=$(cat "$STAGE_OUT.rc") out=$(cat "$STAGE_OUT")
    put STAGE:BUSY 0
    x=$(thor THOR_X:read_pos) y=$(thor THOR_Y:read_pos)
    if [ -e "$STAGE_OUT.stopped" ] ; then smsg "stopped at x $x, y $y mm" "$out"
    elif [ "$rc" == 0 ] ; then smsg "done: at x $x, y $y mm" "$out"
    else smsg "FAILED: $(grep -v '^ *$' <<< "$out" | tail -n 1 | sed 's/^[a-z_.]*: //') (at x $x, y $y)" "$out" ; fi
    rm -f "$STAGE_OUT" "$STAGE_OUT.rc" "$STAGE_OUT.stopped"
}

cmd_poll() {
    local c
    stage_done
    [ "$(ca caget -n -t -w 1 STAGE:STOP 2> /dev/null)" == 1 ] && { put STAGE:STOP 0 ; stage_cmd stop ; }
    c=$(ca caget -t -w 1 STAGE:CMD 2> /dev/null) && [ -n "$c" ] && { put STAGE:CMD "" ; stage_cmd "$c" ; }
    apply_done
    c=$(ca caget -t -w 1 DAQ:CMD 2> /dev/null) || return
    if [ -n "$c" ] ; then put DAQ:CMD "" ; do_cmd "$c" ; fi
    runtype_poll
}

# the scan records start empty, not UDF (they keep the last scan's values while idle)
scan_init() { puts SCAN:NAME "" ; put SCAN:TYPE - ; put SCAN:POINT 0 ; put SCAN:NPOINTS 0 ; put SCAN:X 0 ; put SCAN:Y 0 ; put SCAN:HV 0 ; }

if [ "$1" == once ] ; then
    ioc_up || tb_die "the N1470 IOC does not answer on $IOC_CA"
    daq ; scan_init ; scan ; hvconfig ; put BENCH:PUB_ALIVE 1
    exit
fi
[ $# -eq 0 ] || { sed -n '3,6p' "$0" | sed 's/^# \{0,1\}//' ; exit 2 ; }

next_config=0
rm -f "$APPLY_OUT" "$APPLY_OUT.rc" "$STAGE_OUT" "$STAGE_OUT.rc" "$STAGE_OUT.stopped"   # cut off by a restart
while : ; do
    if ioc_up ; then
        if [ $next_config -eq 0 ] ; then
            scan_init ; choices_init ; stage_init ; put DAQ:BUSY 0 ; put DAQ:CMD ""
            put DAQ:RUNTYPE_SP "$(idx "$(timeout 5 $RC daq_get_runtype 2> /dev/null | head -n 1)" "${RUNTYPES[@]}")"
            LAST_SP=$(ca caget -n -t -w 2 DAQ:RUNTYPE_SP 2> /dev/null)
        fi
        daq
        scan
        if [ $SECONDS -ge $next_config ] ; then
            hvconfig
            next_config=$(( SECONDS + CONFIG_EVERY ))
        fi
        put BENCH:PUB_ALIVE 1
        for (( i=0 ; i < 4 ; i++ )) ; do sleep $POLL ; cmd_poll ; done     # PERIOD = 4 x POLL
    else
        next_config=0                     # an IOC (re)start: load the config at once
        sleep $PERIOD
    fi
done
