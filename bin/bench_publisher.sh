#! /bin/bash
#
# The bench panel's link to everything that is not EPICS (service bench-publisher; docs/epics.md).
#   bench_publisher.sh            the loop
#   bench_publisher.sh once       one pass of the status, then exit
#
# Every 2 s it fills the panel's records in the N1470 IOC: DAQ:* from rcdaq_client, SCAN:* from
# state/scan_point.conf and the scan's CSV; every 30 s the HV channels' NAME / USED / ILIM / RLOAD from
# the detector's hv.conf. Every 0.5 s it carries out what the panel asked for:
#   DAQ:CMD    begin / end / open / close / apply (testbench.sh use ..., then daq/setup_rcdaq.sh in the
#              background), and "cabling PACKET SLOT" (which readout a V1742 reads, in cabling.txt)
#   DAQ:RUNTYPE_SP   the run type
#   STAGE:CMD  move / home / jog, through bin/motor.sh in the background; STAGE:STOP stops it
# Nothing runs during a scan (it drives the DAQ itself); run type, open / close and apply only while
# stopped. The result goes to DAQ:CMD_MSG / STAGE:MSG and one line to logs/daq_commands.log.

. "$(dirname "$(readlink -f "$0")")/../lib/tb.sh"

POINT_FILE=$TB_STATE/scan_point.conf
UNITS=(A B)
ILIM_DEFAULT=360       # uA: IMON limit of a channel without imax_uA in hv.conf (user, 1 Nov 2026)
PERIOD=2
CONFIG_EVERY=30
POLL=0.5               # s: how often the panel's DAQ commands are looked at
RC=rcdaq_client
CMD_LOG=$TB_LOGS/daq_commands.log
APPLY_OUT=$TB_STATE/.daq_apply.out           # the background apply's output; .rc when finished
SETUP_LAST=$TB_LOGS/setup_rcdaq_last.log     # the last good setup's full output (overwritten)
MOTOR=$TB_ROOT/bin/motor.sh
STAGE_OUT=$TB_STATE/.stage_move.out          # the background move's output; .rc when finished
MBBO_STATES=(ZRST ONST TWST THST FRST FVST SXST SVST EIST NIST TEST ELST TVST TTST FTST FFST)

put()  { tb_ca caput -t -w 2 "$1" "$2" > /dev/null 2>&1 ; }
puts() { tb_ca caput -t -S -w 2 "$1" "$2" > /dev/null 2>&1 ; }    # CHAR waveform from a string

daq() {
    local st r n vol open srv dur file rt
    st=$(timeout 5 $RC daq_status -s 2> /dev/null) || { put DAQ:RUNNING 0 ; puts DAQ:FILE "no rcdaq_server" ; return ; }
    read -r r n vol open srv dur file _ <<< "$st"
    rt=$(timeout 5 $RC daq_get_runtype 2> /dev/null | head -n 1)
    put DAQ:LOGGING "$open"
    put DAQ:RUNTYPE "${rt:--}"
    # changed outside the panel: the combo follows rcdaq, unless a pick is waiting for runtype_poll (the count
    # moved: syncing first would undo the pick)
    local n ; n=$(tb_ca caget -t -w 1 DAQ:RUNTYPE_N 2> /dev/null) ; n=${n%.*}
    [ -n "$RUNTYPES" ] && [ "$n" == "$LAST_N" ] &&
        [ "$(tb_ca caget -n -t -w 1 DAQ:RUNTYPE_SP 2> /dev/null)" != "$(idx "$rt" "${RUNTYPES[@]}")" ] && rt_sync
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

ioc_up() { tb_ca caget -t -w 2 BENCH:PUB_ALIVE > /dev/null 2>&1 ; }

# ---- the panel's DAQ commands ----

idx() { local w=$1 i=0 x ; shift ; for x ; do [ "$x" == "$w" ] && { echo $i ; return ; } ; i=$((i + 1)) ; done ; echo -1 ; }

# an mbbo's labels, written only when they differ from the IOC's (its start-up labels, bench_choices.sh, are
# normally the same): a label write processes the record, and for DAQ:RUNTYPE_SP counts up DAQ:RUNTYPE_N, so
# the count is noted again after it (a label write is not a pick)
labels() {   # RECORD LABEL...
    local rec=$1 i have want ; shift
    have=$(tb_ca caget -t -w 1 $(for i in "${MBBO_STATES[@]}" ; do echo $rec.$i ; done) 2> /dev/null | tr '\n' '|')
    want=$(for i in "${!MBBO_STATES[@]}" ; do echo "${@:$((i + 1)):1}" ; done | tr '\n' '|')
    [ "$have" == "$want" ] && return
    for i in "${!MBBO_STATES[@]}" ; do put $rec.${MBBO_STATES[$i]} "${@:$((i + 1)):1}" ; done
    [ "$rec" == DAQ:RUNTYPE_SP ] && [ -n "$LAST_N" ] && { LAST_N=$(tb_ca caget -t -w 1 DAQ:RUNTYPE_N 2> /dev/null) ; LAST_N=${LAST_N%.*} ; }
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
    local DETECTOR= DAQ= pk
    [ -r "$TB_STATE/active.conf" ] && tb_conf "$TB_STATE/active.conf"
    ACTIVE_DET=$DETECTOR ACTIVE_DAQ=$DAQ
    put DAQ:ACTIVE_DET "${DETECTOR:--}"
    put DAQ:ACTIVE_DAQ "${DAQ:--}"
    for pk in "${!CABLED_PV[@]}" ; do put ${CABLED_PV[$pk]} "$(cabled_name $pk)" ; done
}

# ---- which readout (mapping.txt slot) each V1742 reads: the active detector's cabling.txt, a row per
# board's packet (2000 board 0, 2001 board 1). daq/setup_rcdaq.sh reads the boards that have a row (board 0
# alone if none has), so no row for 2001 = one board ----

declare -A CABLED_PV=([2000]=DAQ:CABLED [2001]=DAQ:CABLED1)

cabling_file() { echo "$TB_DB/detectors/$ACTIVE_DET/cabling.txt" ; }

cabled_slot() {   # PACKET: the slot(s) it reads now
    tb_table "$(cabling_file)" 2> /dev/null | awk -v p="$1" '$1 == p { printf "%s%s", n++ ? " " : "", $2 }'
}

cabled_name() {   # PACKET: the SLOT_NAMES name of the slot it reads now, "none" for none
    local s
    s=$(cabled_slot $1)
    [ -n "$s" ] || { echo none ; return ; }
    slot_names "$ACTIVE_DET" | awk -v s="$s" '$1 == s { $1 = "" ; print substr($0, 2) ; f = 1 } END { if (!f) print "slot " s }'
}

slot_names() {   # DET: "SLOT NAME" for every slot of its mapping.txt (names: SLOT_NAMES in detector.conf)
    local d=$TB_DB/detectors/$1 SLOT_NAMES= s n
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

# the panel's "cabled" buttons (do_cmd "cabling PACKET SLOT|-", so rcdaq is stopped): board PACKET reads SLOT
# (- = none: that board is not read, while the other one reads) from the next run on. Its old line stays as a comment with a dated note;
# a change with no run since (its note says the same "from run") is undone first, not stacked on. Then
# testbench.sh check and the faces, and when a board comes or goes, daq/setup_rcdaq.sh (in the background)
cabling_set() {   # PACKET SLOT|-
    local pk=$1 s=$2 b=$(($1 - 2000)) f name cur o ov boards new next note out
    [ -n "$ACTIVE_DET" ] || { msg "cabling refused: no active detector" ; return ; }
    f=$(cabling_file)
    if [ "$s" == - ] ; then
        # one board at least: with no packet at all setup_rcdaq.sh would read board 0 anyway, unmapped
        [ -n "$(tb_table "$f" 2> /dev/null | awk -v p=$pk '$1 != p')" ] ||
            { msg "cabling refused: the other board reads nothing, one of them has to read" ; return ; }
        name=none s=
    else
        name=$(slot_names "$ACTIVE_DET" | awk -v s="$s" '$1 == s { $1 = "" ; print substr($0, 2) }')
        [ -n "$name" ] || { msg "cabling refused: $ACTIVE_DET has no slot $s in mapping.txt" ; return ; }
    fi
    cur=$(cabled_slot $pk)
    [ "$cur" == "$s" ] && { msg "cabling: board $b already reads $name" ; put ${CABLED_PV[$pk]} "$name" ; return ; }
    # not a slot another board reads, nor one that shares a pixel with it (testbench.sh check refuses both)
    for o in $( [ -n "$s" ] && tb_table "$f" 2> /dev/null | awk -v p=$pk '$1 != p { print $2 }') ; do
        [ "$o" == "$s" ] && { msg "cabling refused: another board reads $name already" ; return ; }
        ov=$(tb_table "$(dirname "$f")/mapping.txt" | awk -v a=$s -v b=$o '$1 == a { pa[$3 "," $4] } $1 == b { pb[$3 "," $4] }
                                                          END { for (k in pa) if (k in pb) { print k ; exit } }')
        [ -n "$ov" ] && { msg "cabling refused: $name and $(slot_names "$ACTIVE_DET" | awk -v s=$o '$1 == s { $1 = "" ; print substr($0, 2) }'), which another board reads, share pixel $ov" ; return ; }
    done
    boards=$(tb_table "$f" 2> /dev/null | awk '{ print $1 }' | sort -nu | tr '\n' ' ') ; boards=${boards% }
    next=$(next_run)
    note="# $(date '+%Y-%m-%d %H:%M') (bench panel): board $b (packet $pk) $( [ -n "$s" ] && echo "reads $name, slot $s" || echo off), from run ${next:-?} on"
    awk -v pk=$pk -v s="$s" -v nx="$next" -v note="$note" '
        function is_note(l) { return l ~ /^# .*\(bench panel\):/ && (index(l, "(packet " pk ")") || (pk == 2000 && l !~ /\(packet [0-9]+\)/)) }
        function rb(l,  g) { if (l !~ /\(replaced below\)[[:space:]]*$/) return 0 ; split(substr(l, 2), g) ; return g[1] == pk }
        function act(l,  g) { sub(/#.*/, "", l) ; return split(l, g) == 2 && g[1] == pk ? g[2] : "" }
        { L[++n] = $0 }
        END {
            # the last panel note of this board; with no run since, undo its change: its line goes, the lines it
            # replaced (after the note before it) come back
            for (i = n; i >= 1 && !is_note(L[i]); i--) ;
            if (i >= 1 && nx != "" && index(L[i], "from run " nx " on")) {
                for (j = i - 1; j >= 1 && !is_note(L[j]); j--) ;
                for (k = j + 1; k < i; k++)
                    if (rb(L[k])) { sub(/^# /, "", L[k]) ; sub(/[[:space:]]*\(replaced below\)[[:space:]]*$/, "", L[k]) }
                for (k = i + 1; k <= n; k++) if (act(L[k]) != "") del[k]
                del[i]
            }
            cur = ""
            for (k = 1; k <= n; k++) if (!(k in del) && (c = act(L[k])) != "") cur = cur (cur == "" ? "" : " ") c
            for (k = 1; k <= n; k++) {
                if (k in del) continue
                if (cur != s && act(L[k]) != "") print "# " L[k] "    (replaced below)"
                else print L[k]
            }
            if (cur != s) {
                print note (cur != "" ? " (was slot " cur ")" : "")
                if (s != "") printf "%s  %s\n", pk, s
            }
        }' "$f" > "$f.new" 2> /dev/null && mv "$f.new" "$f" ||
        { msg "cabling FAILED: cannot write $f" ; rm -f "$f.new" ; return ; }
    new=$(tb_table "$f" | awk '{ print $1 }' | sort -nu | tr '\n' ' ') ; new=${new% }
    out=$( { "$TB_ROOT/bin/testbench.sh" check
             python3 "$TB_ROOT/phoebus/make_laser_face.py" ; } 2>&1 )
    put ${CABLED_PV[$pk]} "$name"
    put DAQ:ACTIVE_DET - ; sleep 1 ; put DAQ:ACTIVE_DET "$ACTIVE_DET"   # open panels load the new face (file changes)
    msg "cabling: $ACTIVE_DET board $b (packet $pk) $( [ -n "$s" ] && echo "reads $name" || echo "not read") from run ${next:-?} on; cabling.txt and the faces updated" "$out"
    [ "$new" == "$boards" ] && return
    # a board came or went: rcdaq's readout list changes
    msg "cabling: board $b $( [ -n "$s" ] && echo "added (reads $name)" || echo "left out"), packets now $new: setting up rcdaq ..."
    setup_bg cabling
}

setup_bg() {   # WHAT [DETECTOR PROFILE]: (testbench.sh use detector / use daq, then) daq/setup_rcdaq.sh in the
    SETUP_WHAT=$1                                     # background; apply_done reports it as WHAT
    put DAQ:BUSY 1
    : > "$APPLY_OUT"
    ( { [ -z "$2" ] || { "$TB_ROOT/bin/testbench.sh" use detector "$2" && "$TB_ROOT/bin/testbench.sh" use daq "$3" ; } &&
        timeout 300 "$TB_ROOT/daq/setup_rcdaq.sh" ; } >> "$APPLY_OUT" 2>&1
      echo $? > "$APPLY_OUT.rc" ) &
}

msg() {   # TEXT [FULL OUTPUT]: to the panel and the command log
    puts DAQ:CMD_MSG "$(date +%T)  $1"
    { echo "$(date -Iseconds) $1" ; [ -n "$2" ] && sed 's/^/    /' <<< "$2" ; } >> "$CMD_LOG" 2> /dev/null
}

scan_busy() { [ -r "$POINT_FILE" ] || pgrep -f 'scans/scan_(xy|hv)\.sh' > /dev/null ; }

do_cmd() {   # begin|end|open|close|apply|cabling [PACKET] SLOT (PACKET 2000 or 2001, default 2000; SLOT - = none)
    local c=$1 st run open out rc det prof
    [[ $c =~ ^(begin|end|open|close|apply|cabling( 200[01])? [0-9]{1,2}|cabling 200[01] -)$ ]] || { msg "unknown command '$c'" ; return ; }
    [ -e "$APPLY_OUT" ] && { msg "$c refused: the setup is still being applied" ; return ; }
    scan_busy && { msg "$c refused: a scan is running (scan_xy.sh / scan_hv.sh drive the DAQ themselves)" ; return ; }
    st=$(timeout 5 $RC daq_status -s 2> /dev/null) || { msg "$c refused: no rcdaq_server (start it with daq/setup_rcdaq.sh)" ; return ; }
    read -r run _ _ open _ <<< "$st"
    case $c in
        begin) [ "$run" == -1 ] || { msg "begin refused: run $run is going" ; return ; } ;;
        end)   [ "$run" != -1 ] || { msg "end: no run is going" ; return ; } ;;
        *)     [ "$run" == -1 ] || { msg "$c refused: run $run is going (end it first)" ; return ; } ;;
    esac
    if [[ $c == cabling\ * ]] ; then
        set -- ${c#cabling } ; [ $# == 1 ] && set -- 2000 $1
        cabling_set $1 $2 ; return
    fi
    if [ $c == apply ] ; then
        det=${DETS[$(tb_ca caget -n -t -w 2 DAQ:SETUP_DET)]} prof=${PROFS[$(tb_ca caget -n -t -w 2 DAQ:SETUP_DAQ)]}
        [ -n "$det" ] && [ -n "$prof" ] || { msg "apply refused: pick a detector and a DAQ profile" ; return ; }
        msg "apply: detector $det, DAQ profile $prof: setting up rcdaq and the V1742s ..."
        setup_bg apply "$det" "$prof"
        return
    fi
    put DAQ:BUSY 1
    out=$(timeout 30 $RC daq_$c 2>&1) ; rc=$?
    put DAQ:BUSY 0
    if [ $rc -eq 0 ] ; then msg "$c: done${out:+ ($(tail -n 1 <<< "$out"))}" "$out"
    else msg "$c FAILED (rc $rc): $(tail -n 1 <<< "$out")" "$out" ; fi
    daq
}

apply_done() {   # the background apply (or a cabling's setup) has finished: report it
    local rc out
    [ -e "$APPLY_OUT.rc" ] || return
    rc=$(cat "$APPLY_OUT.rc") out=$(cat "$APPLY_OUT")
    rm -f "$APPLY_OUT" "$APPLY_OUT.rc"
    put DAQ:BUSY 0
    # the full output in the command log only when it failed; a good one's in SETUP_LAST
    if [ "$rc" == 0 ] ; then printf '%s\n' "$out" > "$SETUP_LAST" 2> /dev/null
                             msg "${SETUP_WHAT:-apply}: done ($(grep -m 1 '^setting up rcdaq' <<< "$out" || tail -n 1 <<< "$out"); output in $(basename "$SETUP_LAST"))"
    else msg "${SETUP_WHAT:-apply} FAILED (rc $rc): $(grep -v '^ *$' <<< "$out" | tail -n 1)" "$out" ; fi
    active ; choices_init ; daq
}

# The run type picked in the panel. A pick is a write to DAQ:RUNTYPE_SP, which counts up DAQ:RUNTYPE_N; a
# restarted IOC starts the count at 0. So only a count above the one noted after our own last write is a
# pick; a lower one is a restart (1 Nov 2026: judged by the combo's value, two restarts set rcdaq to HRPPD).

rt_sync() {   # the combo on rcdaq's actual run type; note the count after this (our own) write
    local rt i
    rt=$(timeout 5 $RC daq_get_runtype 2> /dev/null | head -n 1)
    i=$(idx "$rt" "${RUNTYPES[@]}")
    [ "$i" -ge 0 ] && [ "$(tb_ca caget -n -t -w 1 DAQ:RUNTYPE_SP 2> /dev/null)" != "$i" ] &&
        tb_ca caput -c -t -w 2 DAQ:RUNTYPE_SP "$i" > /dev/null 2>&1
    put DAQ:RUNTYPE "${rt:--}"
    LAST_N=$(tb_ca caget -t -w 1 DAQ:RUNTYPE_N 2> /dev/null) ; LAST_N=${LAST_N%.*}
}

runtype_poll() {
    local n sp want st run out
    n=$(tb_ca caget -t -w 1 DAQ:RUNTYPE_N 2> /dev/null) || return
    n=${n%.*}
    [ "$n" == "$LAST_N" ] && return
    if [ -z "$LAST_N" ] || (( n < LAST_N )) ; then rt_sync ; return ; fi      # an IOC restart, not a pick
    sp=$(tb_ca caget -n -t -w 1 DAQ:RUNTYPE_SP 2> /dev/null) want=${RUNTYPES[$sp]}
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

thor() { tb_ca caget -t -w 2 "$@" 2> /dev/null ; }

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
        move) x=$(tb_ca caget -t -w 2 STAGE:X_SP) y=$(tb_ca caget -t -w 2 STAGE:Y_SP) ;;
        home) ;;
        *)    step=$(tb_ca caget -t -w 2 STAGE:STEP) x=- y=-
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
    [ "$(tb_ca caget -n -t -w 1 BENCH:PUB_INIT 2> /dev/null)" == 1 ] || return
    [ "$(tb_ca caget -n -t -w 1 STAGE:STOP 2> /dev/null)" == 1 ] && { put STAGE:STOP 0 ; stage_cmd stop ; }
    c=$(tb_ca caget -t -w 1 STAGE:CMD 2> /dev/null) && [ -n "$c" ] && { put STAGE:CMD "" ; stage_cmd "$c" ; }
    apply_done
    c=$(tb_ca caget -t -w 1 DAQ:CMD 2> /dev/null) || return
    if [ -n "$c" ] ; then put DAQ:CMD "" ; do_cmd "$c" ; fi
    runtype_poll
}

# the scan records start empty, not UDF (they keep the last scan's values while idle)
scan_init() { puts SCAN:NAME "" ; put SCAN:TYPE - ; put SCAN:POINT 0 ; put SCAN:NPOINTS 0 ; put SCAN:X 0 ; put SCAN:Y 0 ; put SCAN:HV 0 ; }

if [ "$1" == once ] ; then
    ioc_up || tb_die "the N1470 IOC does not answer"
    daq ; scan_init ; scan ; hvconfig ; put BENCH:PUB_ALIVE 1
    exit
fi
[ $# -eq 0 ] || { sed -n '3,5p' "$0" | sed 's/^# \{0,1\}//' ; exit 2 ; }

next_config=0
rm -f "$APPLY_OUT" "$APPLY_OUT.rc" "$STAGE_OUT" "$STAGE_OUT.rc" "$STAGE_OUT.stopped"   # cut off by a restart
while : ; do
    if ioc_up ; then
        # set up a (re)started IOC: BENCH:PUB_INIT is 0 after every IOC start, not only after one this loop
        # happened to see as an outage (1 Nov 2026: a missed restart set rcdaq's run type to HRPPD); and on our
        # own first pass, as a restarted publisher has none of the choices yet (an IOC already set up says 1)
        if [ -z "$SET_UP" ] || [ "$(tb_ca caget -n -t -w 2 BENCH:PUB_INIT 2> /dev/null)" != 1 ] ; then
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
