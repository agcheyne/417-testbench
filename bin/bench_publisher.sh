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
    # changed outside the panel: the combo follows rcdaq, unless a pick is waiting for runtype_poll (the count
    # moved: syncing first would undo the pick)
    local n ; n=$(ca caget -t -w 1 DAQ:RUNTYPE_N 2> /dev/null) ; n=${n%.*}
    [ -n "$RUNTYPES" ] && [ "$n" == "$LAST_N" ] &&
        [ "$(ca caget -n -t -w 1 DAQ:RUNTYPE_SP 2> /dev/null)" != "$(idx "$rt" "${RUNTYPES[@]}")" ] && rt_sync
    if [ "$r" == -1 ] ; then                     # stopped: the last run and its file
        put DAQ:RUNNING 0
        file=$(timeout 5 $RC daq_get_lastfilename 2> /dev/null | head -n 1)
        [[ $file =~ -0*([0-9]+)-[0-9]+\.evt$ ]] && put DAQ:RUN "${BASH_REMATCH[1]}"
        puts DAQ:FILE "${file:+last: }$file"
        put DAQ:EVENTS 0 ; put DAQ:RATE 0 ; put DAQ:RATE_AVG 0 ; put DAQ:DURATION 0
        LAST_RUN= LAST_EV= LAST_T=
        return
    fi
    put DAQ:RUNNING 1
    put DAQ:RUN "$r"
    (( r > SEEN_RUN )) && SEEN_RUN=$r                     # for cabling_set: runs without a file count too
    put DAQ:EVENTS "$n"
    put DAQ:DURATION "$dur"
    # instantaneous: events since the last look ~2 s ago; average: events / run time
    local now ; now=$(date +%s.%N)
    if [ "$r" == "$LAST_RUN" ] && [ -n "$LAST_T" ] ; then
        put DAQ:RATE "$(awk -v a="$n" -v b="$LAST_EV" -v t="$now" -v u="$LAST_T" 'BEGIN { print (t > u ? (a - b) / (t - u) : 0) }')"
    else
        put DAQ:RATE "$(awk -v n="$n" -v t="$dur" 'BEGIN { print (t > 0 ? n / t : 0) }')"      # first look at this run
    fi
    LAST_RUN=$r LAST_EV=$n LAST_T=$now
    put DAQ:RATE_AVG "$(awk -v n="$n" -v t="$dur" 'BEGIN { print (t > 0 ? n / t : 0) }')"
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

# an mbbo's labels, written only when they differ from the IOC's (its start-up labels, bench_choices.sh, are
# normally the same): a label write processes the record, and for DAQ:RUNTYPE_SP counts up DAQ:RUNTYPE_N, so
# the count is noted again after it (a label write is not a pick)
labels() {   # RECORD LABEL...
    local rec=$1 i have want ; shift
    have=$(ca caget -t -w 1 $(for i in "${MBBO_STATES[@]}" ; do echo $rec.$i ; done) 2> /dev/null | tr '\n' '|')
    want=$(for i in "${!MBBO_STATES[@]}" ; do echo "${@:$((i + 1)):1}" ; done | tr '\n' '|')
    [ "$have" == "$want" ] && return
    for i in "${!MBBO_STATES[@]}" ; do put $rec.${MBBO_STATES[$i]} "${@:$((i + 1)):1}" ; done
    [ "$rec" == DAQ:RUNTYPE_SP ] && [ -n "$LAST_N" ] && { LAST_N=$(ca caget -t -w 1 DAQ:RUNTYPE_N 2> /dev/null) ; LAST_N=${LAST_N%.*} ; }
}

# the choices of the combos, and the pickers set to what is active now
choices_init() {
    local i
    # the run types daq/setup_rcdaq.sh defines (junk, test, every detector's RUNTYPE), in rcdaq's order: from the
    # db, so a stopped rcdaq leaves no empty choices; the same lists as the IOC's start-up labels (bench_choices.sh)
    RUNTYPES=($( { for i in $(tb_detector_names) ; do RUNTYPE= ; tb_conf "$(tb_detector_dir $i)/detector.conf" ; echo $RUNTYPE ; done
                   echo junk ; echo test ; } | LC_ALL=C sort -u ))
    DETS=($(tb_detector_names))
    PROFS=() ; for i in $(tb_daq_names) ; do grep -q '^READOUT=v1742' "$(tb_daq_file $i)" && PROFS+=($i) ; done
    labels DAQ:RUNTYPE_SP "${RUNTYPES[@]}"
    labels DAQ:SETUP_DET "${DETS[@]}"
    labels DAQ:SETUP_DAQ "${PROFS[@]}"
    active
    put DAQ:SETUP_DET "$(idx "$ACTIVE_DET" "${DETS[@]}")"
    put DAQ:SETUP_DAQ "$(idx "$ACTIVE_DAQ" "${PROFS[@]}")"
}

active() {
    local DETECTOR= DAQ= s
    [ -r "$TB_STATE/active.conf" ] && tb_conf "$TB_STATE/active.conf"
    ACTIVE_DET=$DETECTOR ACTIVE_DAQ=$DAQ
    put DAQ:ACTIVE_DET "${DETECTOR:--}"
    put DAQ:ACTIVE_DAQ "${DAQ:--}"
    s=$(tb_table "$(cabling_file)" 2> /dev/null | awk 'NR == 1 { print $2 }')
    put DAQ:CABLED "$( [ -n "$s" ] && slot_names "$ACTIVE_DET" | awk -v s="$s" '$1 == s { $1 = "" ; print substr($0, 2) }')"
}

# ---- which readout (mapping.txt slot) the V1742 reads: the active detector's cabling.txt ----

cabling_file() { echo "${BENCH_TEST_DB:-$TB_DB}/detectors/$ACTIVE_DET/cabling.txt" ; }   # (BENCH_TEST_DB: a test copy)

slot_names() {   # DET: "SLOT NAME" for every slot of its mapping.txt (names: SLOT_NAMES in detector.conf)
    local d=${BENCH_TEST_DB:-$TB_DB}/detectors/$1 SLOT_NAMES= s n
    [ -n "$1" ] && [ -r "$d/mapping.txt" ] || return
    tb_conf "$d/detector.conf"
    for s in $(tb_table "$d/mapping.txt" | awk '{ print $1 }' | sort -nu) ; do
        n=$(tr ';' '\n' <<< "$SLOT_NAMES" | sed -n "s/^ *$s *= *//p" | head -n 1)
        echo "$s ${n:-slot $s}"
    done
}

# the next run's number: the highest of rcdaq's last file, a run seen going, the panel's begin/end lines in the
# command log, the run-number file and the newest file on disk (a run without logging leaves no file; after an
# rcdaq restart there is no last file)
next_run() {
    local RUNNUMBER_FILE= DATA_ROOT= f n=0 m
    tb_site 2> /dev/null
    f=$(timeout 5 $RC daq_get_lastfilename 2> /dev/null | head -n 1)
    for m in "$( [[ $f =~ -0*([0-9]+)-[0-9]+\.evt$ ]] && echo "${BASH_REMATCH[1]}")" "$SEEN_RUN" \
             "$(tail -n 2000 "$CMD_LOG" 2> /dev/null | sed -n 's/.* Run \([0-9][0-9]*\) \(started\|ended\).*/\1/p' | sort -n | tail -n 1)" \
             "$(cat "$RUNNUMBER_FILE" 2> /dev/null)" \
             "$( [ -n "$DATA_ROOT" ] && find "$DATA_ROOT" -maxdepth 2 -name '*.evt' -mtime -14 -printf '%f\n' 2> /dev/null |
                 sed -n 's/.*-0*\([0-9][0-9]*\)-[0-9]*\.evt$/\1/p' | sort -n | tail -n 1)" ; do
        [[ $m =~ ^[0-9]+$ ]] && (( m > n )) && n=$m
    done
    [ $n -gt 0 ] && echo $((n + 1))
}

# the panel's "cabled" buttons (do_cmd "cabling SLOT", so rcdaq is stopped): cabling.txt reads SLOT from the
# next run on. The old line stays as a comment with a dated note; then testbench.sh check and the faces
cabling_set() {   # SLOT
    local s=$1 f name cur pk last next out
    [ -n "$ACTIVE_DET" ] || { msg "cabling refused: no active detector" ; return ; }
    f=$(cabling_file)
    name=$(slot_names "$ACTIVE_DET" | awk -v s="$s" '$1 == s { $1 = "" ; print substr($0, 2) }')
    [ -n "$name" ] || { msg "cabling refused: $ACTIVE_DET has no slot $s in mapping.txt" ; return ; }
    cur=$(tb_table "$f" 2> /dev/null | awk 'NR == 1 { print $2 }')
    [ "$cur" == "$s" ] && { msg "cabling: $ACTIVE_DET already reads $name (slot $s)" ; put DAQ:CABLED "$name" ; return ; }
    pk=$(tb_table "$f" 2> /dev/null | awk 'NR == 1 { print $1 }') ; pk=${pk:-2000}
    next=$(next_run)
    # a panel change with no run since (its note says the same "from run"): replace it, not stack on it; back to
    # what was there before it: drop it
    local prev
    prev=$(grep -v '^[[:space:]]*$' "$f" | tail -n 3 | head -n 2 | tr '\n' '|')
    if [ -n "$next" ] && [[ $prev =~ ^#\ +[0-9]+\ +([0-9]+)\ +\(replaced\ below\)\|#.*\(bench\ panel\):.*from\ run\ $next\ on ]] ; then
        cur=${BASH_REMATCH[1]}
        awk -v n="$(grep -c . "$f")" 'NF { k++ } !(NF && k > n - 3)' "$f" > "$f.new" && mv "$f.new" "$f"
        printf '%s  %s\n' "$pk" "$cur" >> "$f"
    fi
    if [ "$cur" != "$s" ] ; then
        { awk '/^[[:space:]]*(#|$)/ { print ; next } { print "# " $0 "    (replaced below)" }' "$f" 2> /dev/null
          echo "# $(date '+%Y-%m-%d %H:%M') (bench panel): $name, slot $s, from run ${next:-?} on${cur:+ (was slot $cur)}"
          printf '%s  %s\n' "$pk" "$s" ; } > "$f.new" && mv "$f.new" "$f" ||
            { msg "cabling FAILED: cannot write $f" ; rm -f "$f.new" ; return ; }
    fi
    out=$( { [ -z "$BENCH_TEST_DB" ] && "$TB_ROOT/bin/testbench.sh" check
             $DRY python3 "$TB_ROOT/phoebus/make_laser_face.py" ; } 2>&1 )
    put DAQ:CABLED "$name"
    put DAQ:ACTIVE_DET - ; sleep 1 ; put DAQ:ACTIVE_DET "$ACTIVE_DET"   # open panels load the new face (file changes)
    msg "cabling: $ACTIVE_DET reads $name (slot $s, packet $pk) from run ${next:-?} on; cabling.txt and the faces updated" "$out"
}

msg() {   # TEXT [FULL OUTPUT]: to the panel and the command log
    puts DAQ:CMD_MSG "$(date +%T)  $1"
    { echo "$(date -Iseconds) $1" ; [ -n "$2" ] && sed 's/^/    /' <<< "$2" ; } >> "$CMD_LOG" 2> /dev/null
}

scan_busy() { [ -r "$POINT_FILE" ] || pgrep -f 'bin/scan_(xy|hv)\.sh' > /dev/null ; }

do_cmd() {   # begin|end|open|close|apply|cabling SLOT
    local c=$1 st run open out rc det prof
    case $c in begin|end|open|close|apply|cabling\ [0-9]|cabling\ [0-9][0-9]) ;; *) msg "unknown command '$c'" ; return ;; esac
    [ -e "$APPLY_OUT" ] && { msg "$c refused: the setup is still being applied" ; return ; }
    scan_busy && { msg "$c refused: a scan is running (scan_xy.sh / scan_hv.sh drive the DAQ themselves)" ; return ; }
    st=$(timeout 5 $RC daq_status -s 2> /dev/null) || { msg "$c refused: no rcdaq_server (start it with daq/setup_rcdaq.sh)" ; return ; }
    read -r run _ _ open _ <<< "$st"
    case $c in
        begin) [ "$run" == -1 ] || { msg "begin refused: run $run is going" ; return ; } ;;
        end)   [ "$run" != -1 ] || { msg "end: no run is going" ; return ; } ;;
        *)     [ "$run" == -1 ] || { msg "$c refused: run $run is going (end it first)" ; return ; } ;;
    esac
    [[ $c == cabling\ * ]] && { cabling_set "${c#cabling }" ; return ; }
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

# The run type picked in the panel. A pick is a write to DAQ:RUNTYPE_SP, which counts up DAQ:RUNTYPE_N; a
# restarted IOC starts the count at 0. So only a count above the one noted after our own last write is a
# pick; a lower one is a restart (1 Nov 2026: judged by the combo's value, two restarts set rcdaq to HRPPD).

rt_sync() {   # the combo on rcdaq's actual run type; note the count after this (our own) write
    local rt i
    rt=$(timeout 5 $RC daq_get_runtype 2> /dev/null | head -n 1)
    i=$(idx "$rt" "${RUNTYPES[@]}")
    [ "$i" -ge 0 ] && [ "$(ca caget -n -t -w 1 DAQ:RUNTYPE_SP 2> /dev/null)" != "$i" ] &&
        ca caput -c -t -w 2 DAQ:RUNTYPE_SP "$i" > /dev/null 2>&1
    put DAQ:RUNTYPE "${rt:--}"
    LAST_N=$(ca caget -t -w 1 DAQ:RUNTYPE_N 2> /dev/null) ; LAST_N=${LAST_N%.*}
}

runtype_poll() {
    local n sp want st run out
    n=$(ca caget -t -w 1 DAQ:RUNTYPE_N 2> /dev/null) || return
    n=${n%.*}
    [ "$n" == "$LAST_N" ] && return
    if [ -z "$LAST_N" ] || (( n < LAST_N )) ; then rt_sync ; return ; fi      # an IOC restart, not a pick
    sp=$(ca caget -n -t -w 1 DAQ:RUNTYPE_SP 2> /dev/null) want=${RUNTYPES[$sp]}
    if [ -z "$want" ] ; then msg "run type: no such choice ($sp)"
    elif [ -e "$APPLY_OUT" ] ; then msg "run type refused: the setup is still being applied"
    elif scan_busy ; then msg "run type refused: a scan is running"
    elif ! st=$(timeout 5 $RC daq_status -s 2> /dev/null) ; then msg "run type refused: no rcdaq_server"
    else
        read -r run _ <<< "$st"
        if [ "$run" != -1 ] ; then msg "run type refused: run $run is going"
        elif out=$(timeout 10 $RC daq_set_runtype "$want" 2>&1) ; then msg "run type: $want" "$out"
        else msg "run type $want FAILED: $(tail -n 1 <<< "$out")" "$out" ; fi
    fi
    rt_sync
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
    # an IOC that has (re)started and not been set up yet holds start values, not requests: act on nothing
    # until the main loop has set it up (1 Nov 2026: the run-type combo's start value 0 set rcdaq to HRPPD)
    [ "$(ca caget -n -t -w 1 BENCH:PUB_INIT 2> /dev/null)" == 1 ] || return
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
        # set up a (re)started IOC: BENCH:PUB_INIT is 0 after every IOC start, not only after one this loop
        # happened to see as an outage (1 Nov 2026: a missed restart set rcdaq's run type to HRPPD); and on our
        # own first pass, as a restarted publisher has none of the choices yet (an IOC already set up says 1)
        if [ -z "$SET_UP" ] || [ "$(ca caget -n -t -w 2 BENCH:PUB_INIT 2> /dev/null)" != 1 ] ; then
            SET_UP=1
            scan_init ; choices_init ; stage_init ; put DAQ:BUSY 0 ; put DAQ:CMD ""
            rt_sync
            next_config=0
            put BENCH:PUB_INIT 1
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
