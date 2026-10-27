#! /bin/bash
#
# Command-line access to the CAEN N1470 HV supplies over USB (FTDI serial, 9600 8N1 XON/XOFF).
# Commands that talk to a unit need -u UNIT: a board serial number (236, 1593), an alias from
# UNITS (A, B), or "all" (status, check, mon and off only).
#
# Readback (read-only):
#   n1470.sh units                  list the units found and their ports
#   n1470.sh -u A status            board info + table of all channels
#   n1470.sh -u all status          same, for every unit
#   n1470.sh -u all check [--color] [--quiet]
#                                   status + ALARM/NOTE lines; exit 3 on alarm, else 0 (also when a unit
#                                   is off or absent while its HV was off: only a NOTE). An alarm also
#                                   puts a banner on top (red with --color), a desktop notification, a
#                                   pop-up in each VNC display, and ALARM_SOUND in the room (+ remote
#                                   pulse) every time (not with --quiet). To watch:
#                                   watch -c -n 5 n1470.sh -u all check --color
#                                   A systemd user timer also runs it every minute in the background.
#                                   Every check logs, from the readings it takes anyway: alarm START/
#                                   CHANGE/CLEAR, HV-ON/HV-OFF and unit not present/busy/back (UNIT) to
#                                   logs/hv_alarms.log and journalctl -t n1470, and while a unit's HV is
#                                   in use, all its channels every HISTORY_EVERY s to
#                                   logs/hv_history_YYYYMMDD.csv (no file on days the HV stays off).
#                                   A unit that cannot be read is an alarm only if its HV was in use
#                                   (port busy: after BUSY_GRACE s); several checks at once log once.
#   n1470.sh mute | unmute          silence the alarm sound until the alarm clears; monitoring goes on
#   n1470.sh test-alarm [--quiet] [--color]
#                                   are the alarms working? fires every alarm output once with a fake
#                                   alarm (SOUNDS IN THE ROOM unless --quiet), then "cleared", and says
#                                   what each output did. Only this command: no HV access, and a running
#                                   "watch ... check" shows nothing and keeps its own state and mute.
#   n1470.sh test-alarm --remote-only
#                                   sound + spoken test message only on ALARM_REMOTE_PULSE (your laptop
#                                   through the tunnel), nothing in the room, no pop-ups
#   n1470.sh -u A mon PAR [CH]      raw monitor query, e.g. "mon VMON", "mon VSET 2", "mon BDILK"
#                                   (CH 0-3, or omit for all channels; board params start with BD)
# Set values:
#   n1470.sh -u A set PAR CH VAL    PAR one of VSET ISET MAXV RUP RDW TRIP PDWN
#                                   e.g. "set VSET 0 2250", "set ISET 2 10", "set RUP 1 20"
#   n1470.sh -u A off CH|all        turn channel(s) off; "-u all off all" = all 8 channels
#   n1470.sh -u A on CH|all --box-safe
#                                   turn channel(s) on; refuses without the flag
#
# Both units have identical FTDI FT232B adapters without a USB serial number, so the by-id link
# points at whichever was plugged in last. Units are found by asking every such by-path port for
# its board serial (MON BDSNUM, read-only) instead.
# Only one program can use a port at a time: close GECO2020 first.

set -o pipefail         # a failed hv_cmd fails hv_mon too, not just the trailing tr

UNITS=(A:236 B:1593)   # alias:board serial; "-u all" goes through them in this order
BOARD=00
NCH=4
SETTABLE=" VSET ISET MAXV RUP RDW TRIP PDWN "
STAT_BITS=(ON RUP RDW OVC OVV UNV MAXV TRIP OVP OVT DIS KILL ILK NOCAL)

# "check" raises an ALARM (exit 3) for the board's own BDALARM, these STAT flags on any channel,
# and the limits below on channels that are ON and not ramping. Empty value = that test is off.
ALARM_BITS=" OVC OVV UNV MAXV TRIP OVP OVT ILK "
NOTREADY_BITS=" DIS KILL "   # only a NOTE: the channel cannot be switched on until cleared
VTOL=10                      # V: |VMON - VSET| above this
IFRAC=0.9                    # IMON above this fraction of ISET (the trip level)
# per-channel IMON limits [uA], as [SERIAL:CH]=limit; none set yet
declare -A IMAX=(
#   [SN:CH]=100
)
# what an alarm (or a unit that cannot be read) does besides exit 3; to try it, use test-alarm
# (N1470_FAKE_ALARM="text" in the environment also adds a fake alarm to every unit of that check)
# ALARM_SOUND=/usr/share/sounds/freedesktop/stereo/alarm-clock-elapsed.oga   # every check; empty = none
ALARM_SOUND=/usr/share/sounds/freedesktop/stereo/dialog-warning.oga
ALARM_REMOTE_PULSE=tcp:127.0.0.1:24713   # also play it on this PulseAudio server (e.g. your laptop through
                                         # an ssh -R tunnel), if it answers within 1 s; empty = room only
ALARM_VNC_POPUP=1            # also a pop-up window in every running Xvnc display; empty = off
ALARM_SPEAK=1                # after the sound, say what it is ("HV alarm. Unit A, channel 2, over-current.")
ALARM_VOICE=en               #   espeak-ng voice and speed (words per minute); ALARM_SPEAK empty = no speech
ALARM_SPEED=150
NOTIFY_REPEAT=60             # s: repeat the notification (and reopen a closed pop-up) while the alarm stays
# desktop session of this user, so notifications and sound also work from ssh / cron
[ -d "$XDG_RUNTIME_DIR" ] || XDG_RUNTIME_DIR=/run/user/$(id -u)
[ -d "$XDG_RUNTIME_DIR" ] || XDG_RUNTIME_DIR=/tmp
export XDG_RUNTIME_DIR DBUS_SESSION_BUS_ADDRESS=${DBUS_SESSION_BUS_ADDRESS:-unix:path=$XDG_RUNTIME_DIR/bus}
ALARM_MUTE=$XDG_RUNTIME_DIR/n1470-mute   # "n1470.sh mute" creates it, a cleared alarm removes it
# logs written by check (several monitors at once never write a line twice)
LOG_DIR=$(dirname "$(dirname "$(readlink -f "$0")")")/logs
ALARM_LOG=$LOG_DIR/hv_alarms.log             # START / CHANGE / CLEAR per unit, also to journalctl -t n1470
HISTORY_CSV=$LOG_DIR/hv_history_%Y%m%d.csv   # date format: one file per day
HISTORY_EVERY=60                             # s: all channels once per interval; empty = no history
HV_ON_VMON=10       # V: a unit's HV is "in use" while any channel is ON or ramping or has VMON above this
BUSY_GRACE=100      # s: a busy port (GECO2020?) on a unit whose HV is in use becomes an alarm after this

# Which detector electrode each channel feeds, as [SERIAL:CH]=name; "status" shows it.
# Filled from the hv.conf of the detector selected with "testbench.sh use detector NAME"
# (rows with a unit and channel); no detector selected = no names, everything else works.
declare -A ELECTRODE=()
load_electrodes() {
    . "$(dirname "$(readlink -f "$0")")/../lib/tb.sh" || return
    ( tb_active ) > /dev/null 2>&1 || return
    tb_active
    local name unit ch rest u
    while read -r name unit ch rest ; do
        for u in "${UNITS[@]}" ; do [ "$unit" == "${u%%:*}" ] && unit=${u#*:} ; done
        [[ $unit =~ ^[0-9]+$ && $ch =~ ^[0-3]$ ]] && ELECTRODE[$unit:$ch]=$name
    done < <(tb_table "$(tb_detector_dir)/hv.conf")
}
load_electrodes

ME=n1470
usage() { sed -n '3,43p' "$0" | sed 's/^# \{0,1\}//' ; exit 2 ; }
die()   { echo "$ME: $*" >&2 ; exit 1 ; }

# send one command, print the VAL field (or the whole reply if none); fail on ERR / no reply
hv_cmd() {   # CMD PAR [CH] [VAL]
    local msg="\$BD:$BOARD,CMD:$1" reply junk
    [ -n "$3" ] && msg+=",CH:$3"
    msg+=",PAR:$2"
    [ -n "$4" ] && msg+=",VAL:$4"
    while read -r -t 0.05 -u 3 junk ; do : ; done     # drop stale input
    printf '%s\r\n' "$msg" >&3
    read -r -t 2 -u 3 reply || die "no reply to $msg"
    reply=${reply%$'\r'}
    [[ $reply == *ERR* ]] && die "$msg -> $reply"
    if [[ $reply == *VAL:* ]] ; then echo "${reply#*VAL:}" ; else echo "$reply" ; fi
}

# monitor query; channel params default to all channels (CH:4), output space-separated
hv_mon() {   # PAR [CH]
    local ch=$2
    [ -z "$ch" ] && [[ $1 != BD* ]] && ch=$NCH
    hv_cmd MON "$1" "$ch" | tr ';' ' '
}

decode_stat() {
    local s=$((10#$1)) i out=""
    for i in "${!STAT_BITS[@]}" ; do
        (( s >> i & 1 )) && out+="${out:+|}${STAT_BITS[$i]}"
    done
    echo "${out:-OFF}"
}

parse_ch() {
    case $1 in
        all|ALL) echo $NCH ;;
        [0-3])   echo $1 ;;
        *)       die "channel must be 0-$((NCH-1)) or all" ;;
    esac
}

status() {
    local p c i
    printf '%-7s %s, %s (%s)\n' UNIT "$(unit_label $SN)" "$PORT" "$(basename "$(readlink -f "$PORT")")"
    for p in BDNAME BDSNUM BDFREL BDCTR BDILK ; do
        printf '%-7s %s\n' $p "$(hv_mon $p)"
    done
    local cols=(VSET VMON ISET IMON RUP RDW MAXV POL STAT)
    declare -gA d=()   # global: check() uses it too
    for c in "${cols[@]}" ; do d[$c]=$(hv_mon $c) || exit 1 ; done
    printf 'CH' ; printf '%9s' "${cols[@]}" ; printf '  flags\n'
    for (( i=0 ; i<NCH ; i++ )) ; do
        printf '%2d' $i
        for c in "${cols[@]}" ; do
            local v=(${d[$c]})
            printf '%9s' "${v[$i]}"
        done
        local st=(${d[STAT]})
        printf '  %s%s\n' "$(decode_stat ${st[$i]})" "${ELECTRODE[$SN:$i]:+   = ${ELECTRODE[$SN:$i]}}"
    done
    echo "units: V, uA, V/s"
}

fgt() { awk -v a="$1" -v b="$2" 'BEGIN { exit !(a+0 > b+0) }' ; }   # float A > B

# status, then one line per alarm; exit 3 if there is any
check() {
    local i s b name vs vm is im lim alarms=() notes=()
    status || exit 1
    local st=(${d[STAT]}) vset=(${d[VSET]}) vmon=(${d[VMON]}) iset=(${d[ISET]}) imon=(${d[IMON]})
    local ba ; ba=$(hv_mon BDALARM) || exit 1
    [[ $ba =~ ^\ *0+\ *$ ]] || alarms+=("board: BDALARM=$ba")
    for (( i=0 ; i<NCH ; i++ )) ; do
        s=$((10#${st[$i]}))
        name="ch$i${ELECTRODE[$SN:$i]:+ (${ELECTRODE[$SN:$i]})}"
        for b in "${!STAT_BITS[@]}" ; do
            (( s >> b & 1 )) || continue
            [[ $ALARM_BITS == *" ${STAT_BITS[$b]} "* ]] && alarms+=("$name: ${STAT_BITS[$b]}")
            [[ $NOTREADY_BITS == *" ${STAT_BITS[$b]} "* ]] && notes+=("$name ${STAT_BITS[$b]}")
        done
        (( (s & 1) && !(s & 6) )) || continue        # ON, not RUP/RDW
        vs=${vset[$i]} vm=${vmon[$i]} is=${iset[$i]} im=${imon[$i]} lim=${IMAX[$SN:$i]}
        [ -n "$VTOL" ] && awk -v s=$vs -v m=$vm -v t=$VTOL 'BEGIN { d = m - s ; exit !(d > t || -d > t) }' &&
            alarms+=("$name: VMON off -- VMON $vm V vs VSET $vs V (more than $VTOL V)")
        [ -n "$IFRAC" ] && fgt $im $(awk -v i=$is -v f=$IFRAC 'BEGIN { print i*f }') &&
            alarms+=("$name: IMON near trip -- IMON $im uA above $IFRAC x ISET $is uA")
        [ -n "$lim" ] && fgt $im $lim && alarms+=("$name: IMON over limit -- IMON $im uA above $lim uA")
    done
    [ ${#notes[@]} -gt 0 ] && printf 'NOTE    not ready: %s\n' "$(IFS=, ; echo "${notes[*]}" | sed 's/,/, /g')"
    [ -n "$N1470_FAKE_ALARM" ] && alarms+=("TEST $N1470_FAKE_ALARM")
    [ ${#alarms[@]} -gt 0 ] && printf "ALARM   $(unit_label $SN) %s\n" "${alarms[@]}"
    [ ${#alarms[@]} -eq 0 ] && { echo "OK      no alarms" ; return 0 ; }
    return 3
}

# banner for the top of the check output: red under "watch -c" with --color, "!!!" frame without
banner() {   # COLOR LINE...
    local on="" off=$'\e[0m' l bar=$(printf '%78s' '' | tr ' ' '!')
    case $1 in red) on=$'\e[1;97;41m' ;; green) on=$'\e[1;97;42m' ;; *) off="" ;; esac
    shift
    echo "$on$bar$off"
    for l in "$@" ; do printf '%s!!!  %-70s!!!%s\n' "$on" "$l" "$off" ; done
    echo "$on$bar$off"
}

notify() {   # URGENCY TITLE BODY   (desktop on the physical screen)
    command -v notify-send > /dev/null && notify-send -u $1 -a n1470 "$2" "$3" 2> /dev/null
}

is_zenity() { [ "$(cat /proc/$(cat "$1" 2> /dev/null)/comm 2> /dev/null)" == zenity ] ; }   # PIDFILE

vnc_displays() {   # "DISPLAY AUTHFILE" for each running Xvnc of this user
    pgrep -u $(id -u) -a Xvnc | awk '{ d = a = "" ; for (i = 3 ; i <= NF ; i++) {
        if ($i ~ /^:[0-9]+$/) d = $i ; if ($(i-1) == "-auth") a = $i } ; if (d != "") print d, a }'
}

# a pop-up in every running Xvnc display (VNC has no sound, and notify-send only reaches the
# physical screen); replaces this script's previous pop-up there, or with KEEP leaves an open one.
# POPTAG keeps test-alarm's pop-ups apart from the monitor's.
popup() {   # KEEP|"" TYPE TITLE TEXT
    local d a pid
    [ -n "$ALARM_VNC_POPUP" ] && command -v zenity > /dev/null || return
    while read -r d a ; do
        pid=$XDG_RUNTIME_DIR/n1470-popup$POPTAG$d.pid
        if is_zenity "$pid" ; then
            [ -n "$1" ] && continue
            kill $(cat "$pid")
        fi
        # GDK_BACKEND=x11: GTK otherwise prefers $XDG_RUNTIME_DIR/wayland-0 and opens on the physical screen
        DISPLAY=$d XAUTHORITY=$a GDK_BACKEND=x11 zenity --$2 --no-markup --width 500 --title "$3" --text "$4" > /dev/null 2>&1 &
        echo $! > "$pid"
    done < <(vnc_displays)
}

play_room() {   # FILE...: play them one after the other on the default sink, in the foreground
    local f rc=0
    for f in "$@" ; do
        if command -v pw-play > /dev/null ; then
            pw-play "$f" || rc=1
        elif command -v canberra-gtk-play > /dev/null ; then
            canberra-gtk-play -f "$f" || rc=1
        else
            printf '\a' > /dev/tty
        fi
    done
    return $rc
}

remote_up() {   # ALARM_REMOTE_PULSE is set and something answers on it within 1 s
    local h p
    [[ $ALARM_REMOTE_PULSE == tcp:*:* ]] || return 1
    h=${ALARM_REMOTE_PULSE#tcp:} p=${h##*:} h=${h%:*}
    timeout 1 bash -c "exec 3<>/dev/tcp/$h/$p" 2> /dev/null
}

play_remote() {   # FILE...: play them one after the other on ALARM_REMOTE_PULSE, in the foreground
    local f rc=0
    for f in "$@" ; do
        timeout 30 gst-launch-1.0 -q filesrc location="$f" ! decodebin ! audioconvert \
            ! audioresample ! pulsesink server="$ALARM_REMOTE_PULSE" || rc=1
    done
    return $rc
}

# FILE... in the room and, if it answers, on ALARM_REMOTE_PULSE; returns when both are done
play_alarm() {
    play_room "$@" &
    remote_up && play_remote "$@" &
    wait
}

# play FILE... in the background; skipped while the previous one still plays, unless QUEUE: then after it
sound_bg() {   # QUEUE|"" FILE...
    local pid=$XDG_RUNTIME_DIR/n1470-sound.pid prev
    prev=$(cat "$pid" 2> /dev/null)
    kill -0 "$prev" 2> /dev/null && [ -z "$1" ] && return
    shift
    [ $# -gt 0 ] || return
    ( while kill -0 "$prev" 2> /dev/null ; do sleep 0.5 ; done ; play_alarm "$@" ) > /dev/null 2>&1 &
    echo $! > "$pid"
}

# short spoken form of alarm lines: "HV alarm. Unit A, channel 2, over-current. ... And 3 more."
speech_text() {   # ALARM...
    local l unit where what tst n=0 out="HV alarm."
    for l in "$@" ; do
        n=$((n+1))
        [ $n -gt 2 ] && continue
        unit="" tst=""
        if [[ $l =~ ^([A-Za-z0-9]+)\ s/n\ [0-9]+\ (.*)$ ]] ; then
            unit="unit ${BASH_REMATCH[1]}" l=${BASH_REMATCH[2]}
        elif [[ $l =~ ^s/n\ ([0-9]+)\ \(not\ in\ UNITS\)\ (.*)$ ]] ; then
            unit="serial ${BASH_REMATCH[1]}" l=${BASH_REMATCH[2]}
        fi
        [[ $l == "TEST "* ]] && l=${l#TEST } tst="test, "
        where=$unit
        if [[ $l =~ ^ch([0-3])(\ \(([^\)]*)\))?:\ (.*)$ ]] ; then
            where="$unit, channel ${BASH_REMATCH[1]}"
            [ -n "${BASH_REMATCH[3]}" ] && where="${BASH_REMATCH[3]//[-_]/ }, $where"
            l=${BASH_REMATCH[4]}
        fi
        case $l in
            OVC)  what=over-current ;;
            OVV)  what=over-voltage ;;
            UNV)  what=under-voltage ;;
            TRIP) what=trip ;;
            MAXV) what="at max voltage" ;;
            OVT)  what=over-temperature ;;
            OVP)  what=over-power ;;
            ILK)  what=interlock ;;
            "VMON off -- "*)
                [[ $l =~ VMON\ ([0-9.]+)\ V\ vs\ VSET\ ([0-9.]+) ]]
                what="voltage off by $(awk -v a=${BASH_REMATCH[1]:-0} -v b=${BASH_REMATCH[2]:-0} 'BEGIN { d = a - b ; printf "%.0f", d < 0 ? -d : d }') volts" ;;
            "IMON near trip"*)  what="current near trip" ;;
            "IMON over limit"*) what="current over limit" ;;
            "NO READBACK"*)     what="no readback from $unit" where="" ;;
            board:*)            what="board alarm" ;;
            *)                  what=${l%% -- *} ;;
        esac
        where="$tst$where"
        [ -z "$where" ] && what=${what^}
        out+=" ${where:+${where^}, }$what."
    done
    [ $n -gt 2 ] && out+=" And $((n-2)) more."
    echo "$out"
}

# TEXT -> a wav of it spoken (espeak-ng), made once and kept in LOG_DIR/.state/speech; fails if off
speech_wav() {   # TEXT
    local d=$LOG_DIR/.state/speech f
    [ -n "$ALARM_SPEAK" ] && command -v espeak-ng > /dev/null || return 1
    mkdir -p "$d" 2> /dev/null || return 1
    f=$d/$(echo "$ALARM_VOICE $ALARM_SPEED $1" | md5sum | cut -c 1-16).wav
    if [ ! -s "$f" ] ; then
        find "$d" -name '*.wav' -mtime +7 -delete 2> /dev/null
        espeak-ng -v "$ALARM_VOICE" -s "$ALARM_SPEED" -w "$f.$$" "$1" 2> /dev/null && mv "$f.$$" "$f" || return 1
    fi
    echo "$f"
}

# test-alarm --remote-only: the alarm sound and a spoken test message only on ALARM_REMOTE_PULSE
test_remote() {
    local files=() f
    [ -n "$ALARM_REMOTE_PULSE" ] || { echo "remote sound:  off (ALARM_REMOTE_PULSE is empty)" ; return 1 ; }
    echo "test-alarm --remote-only: alarm sound + spoken test message on $ALARM_REMOTE_PULSE only, nothing in the room"
    [ -r "$ALARM_SOUND" ] && files+=("$ALARM_SOUND")
    if f=$(speech_wav "This is a test of the HV alarm. Not a real alarm.") ; then
        files+=("$f")
    else
        echo "speech:        off (ALARM_SPEAK) or espeak-ng failed"
    fi
    if ! remote_up ; then
        echo "remote sound:  $ALARM_REMOTE_PULSE not reachable (tunnel down?)" ; return 1
    elif play_remote "${files[@]}" > /dev/null 2>&1 ; then
        echo "remote sound:  played on $ALARM_REMOTE_PULSE (tunnel up)" ; return 0
    else
        echo "remote sound:  FAILED on $ALARM_REMOTE_PULSE (tunnel up, but no PulseAudio accepting at the far end?)" ; return 1
    fi
}

log_alarm() {   # EVENT TEXT   -> ALARM_LOG and the system journal
    mkdir -p "$LOG_DIR" 2> /dev/null
    echo "$(date -Iseconds) $1 $2" 2> /dev/null >> "$ALARM_LOG"
    logger -t n1470 -- "$1 $2" 2> /dev/null
}

with_values() {   # alarm lines on stdin -> "line [readings of its channel]; ..." (uses unit_log's row[])
    local l
    while IFS= read -r l ; do
        if [[ $l =~ ^(TEST )?ch([0-3]) ]] && [ -n "${row[${BASH_REMATCH[2]}]}" ] ; then
            set -- ${row[${BASH_REMATCH[2]}]}
            l+=" [VSET $1 VMON $2 ISET $3 IMON $4 STAT $9 ${10}]"
        fi
        printf '%s; ' "$l"
    done | sed 's/; $//'
}

# Per unit, from the output of its check (no extra HV queries), with state in LOG_DIR/.state under
# a lock so that several checks (watch, timer) never log twice: decides whether a unit that cannot
# be read is an alarm, logs alarm / HV on-off / unit transitions, and writes history while the HV
# is in use. Sets UA_ALARMS (banner lines) and UA_NOTE.
unit_assess() {   # SERIAL RC OUTPUT
    local sn=$1 rc=$2 o=$3 st=$LOG_DIR/.state label=$(unit_label $1) now=$(date +%s)
    local lf l ch keys prev avail detail since inuse=0 was_inuse on=() full=()
    UA_ALARMS=() UA_NOTE=""
    mkdir -p "$st" 2> /dev/null
    exec {lf}>> "$st/lock" && flock -w 10 $lf
    local -A row=()
    while read -r ch l ; do row[$ch]=$l ; done < <(awk '$1 ~ /^[0-3]$/ && NF >= 11' <<< "$o")
    was_inuse=$(cat "$st/inuse-$sn" 2> /dev/null)

    # can it be read? log changes once
    if [ $rc -ne 1 ] ; then
        avail=OK
    else
        detail=$(grep -E '^(n1470|  )' <<< "$o" | sed -e 's/^n1470 \[[^]]*\]: //' -e "s|^$label ||" \
                 -e 's|/dev/serial/by-path/pci-[^ ]*-usb-[0-9]*:\([0-9.]*\):1\.0-port0|usb \1|g' | tr -s ' \n' ' ' | sed 's/ $//')
        case $detail in *"in use by"*) avail=BUSY ;; *"not found"*) avail=ABSENT ;; *) avail=UNREADABLE ;; esac
    fi
    prev=$(cat "$st/avail-$sn" 2> /dev/null)
    if [ "$avail" != "$prev" ] ; then
        case $avail in
            OK)         [ -n "$prev" ] && log_alarm UNIT "$label: readable again" ;;
            ABSENT)     log_alarm UNIT "$label: not present ($detail)" ;;
            BUSY)       log_alarm UNIT "$label: port busy ($detail)" ; echo $now > "$st/busy-$sn" ;;
            UNREADABLE) log_alarm UNIT "$label: cannot be read ($detail)" ;;
        esac
        echo $avail > "$st/avail-$sn"
    fi

    if [ $avail == OK ] && [ ${#row[@]} -eq $NCH ] ; then
        # HV in use: any channel ON or ramping, or still above HV_ON_VMON
        for (( ch=0 ; ch<NCH ; ch++ )) ; do
            set -- ${row[$ch]}
            if (( 10#$9 & 7 )) || { [ -n "$HV_ON_VMON" ] && fgt $2 $HV_ON_VMON ; } ; then
                on+=("ch$ch${ELECTRODE[$sn:$ch]:+ (${ELECTRODE[$sn:$ch]})} ${10} VSET $1 VMON $2")
            fi
        done
        [ ${#on[@]} -gt 0 ] && inuse=1
        if [ $inuse != "${was_inuse:-0}" ] ; then
            if [ $inuse == 1 ] ; then
                log_alarm HV-ON "$label: $(printf '%s, ' "${on[@]}" | sed 's/, $//')"
            else
                log_alarm HV-OFF "$label: all channels off and below $HV_ON_VMON V"
            fi
            echo $inuse > "$st/inuse-$sn"
        fi
        while IFS= read -r l ; do full+=("${l#ALARM   $label }") ; done < <(grep '^ALARM   ' <<< "$o")
    else
        [ $rc -ne 1 ] && full+=("cannot read the channel table")
        inuse=${was_inuse:-0}                  # last known
        since=$(cat "$st/busy-$sn" 2> /dev/null) ; since=$(( now - ${since:-$now} ))
        if [ $inuse != 1 ] ; then
            UA_NOTE="NOTE    $label: ${avail,,} (HV was off: not an alarm)"
        elif [ $avail != BUSY ] ; then
            full+=("NO READBACK while HV in use: ${avail,,} -- $detail")
        elif (( since > BUSY_GRACE )) ; then
            full+=("NO READBACK while HV in use: port busy -- for $((since/60)) min: $detail")
        else
            UA_NOTE="NOTE    $label: port busy for $since s while HV in use; alarm after $BUSY_GRACE s"
        fi
    fi

    # alarm START / CHANGE / CLEAR (a changing reading after " -- " is not a change)
    keys=$(printf '%s\n' "${full[@]}" | sed 's/ -- .*//')
    prev=$(cat "$st/alarms-$sn" 2> /dev/null)
    if [ "$keys" != "$prev" ] ; then
        if [ -z "$prev" ] ; then
            log_alarm START "$label: $(printf '%s\n' "${full[@]}" | with_values)"
        elif [ -z "$keys" ] ; then
            log_alarm CLEAR "$label: cleared, was: $(with_values <<< "$prev")"
        else
            log_alarm CHANGE "$label: $(printf '%s\n' "${full[@]}" | with_values)"
        fi
        echo "$keys" > "$st/alarms-$sn"
    fi
    [ ${#full[@]} -gt 0 ] && UA_ALARMS=("${full[@]/#/$label }")

    # history, only while the HV is in use
    local slot=$(( now / ${HISTORY_EVERY:-1} ))
    if [ -n "$HISTORY_EVERY" ] && [ $avail == OK ] && [ $inuse == 1 ] && [ ${#row[@]} -eq $NCH ] &&
       [ "$slot" != "$(cat "$st/history-$sn" 2> /dev/null)" ] ; then
        local csv=$(date +"$HISTORY_CSV") ts=$(date -Iseconds) alias=""
        for l in "${UNITS[@]}" ; do [ "${l#*:}" == "$sn" ] && alias=${l%%:*} ; done
        [ -s "$csv" ] || echo "timestamp,unit,serial,ch,VSET,VMON,ISET,IMON,STAT,flags,electrode" > "$csv"
        for (( ch=0 ; ch<NCH ; ch++ )) ; do
            set -- ${row[$ch]}
            echo "$ts,$alias,$sn,$ch,$(awk -v a=$1 -v b=$2 -v c=$3 -v d=$4 'BEGIN { printf "%g,%g,%g,%g", a, b, c, d }'),$((10#$9)),${10},${ELECTRODE[$sn:$ch]}"
        done >> "$csv"
        echo $slot > "$st/history-$sn"
    fi
    exec {lf}>&-
}

# test-alarm: every output once, then the cleared step, with a report. Uses none of the monitor's
# state (rate limit, mute, sound and pop-up pid files) and does not talk to the HV.
test_alarm() {
    local text="TEST from n1470.sh test-alarm - not a real alarm" d a rp gp rc=0 n=0
    POPTAG=-test
    log_alarm TEST-START "test-alarm run by $USER, not a real alarm, no HV involved"
    if [ -z "$QUIET" ] ; then
        echo "test-alarm: this will sound in the room${ALARM_REMOTE_PULSE:+ (and on $ALARM_REMOTE_PULSE if it answers)}"
        [ -r "$ALARM_SOUND" ] || { echo "sound file $ALARM_SOUND not readable" ; QUIET=1 ; rc=1 ; }
    fi
    banner "${COLOR:+red}" "HV ALARM   $(date +%T)   1 problem(s)" "$text"
    if notify critical "N1470 HV ALARM" "$text" ; then
        echo "notification:  sent to the desktop on the physical screen"
    else
        echo "notification:  FAILED (notify-send / $DBUS_SESSION_BUS_ADDRESS)" ; rc=1
    fi
    popup "" error "N1470 HV ALARM" "$(date +%T)"$'\n'"$text"
    if [ -z "$QUIET" ] ; then
        local files=("$ALARM_SOUND") f
        if f=$(speech_wav "This is a test of the HV alarm. Not a real alarm.") ; then
            files+=("$f") ; echo "speech:        test message made with espeak-ng"
        else
            echo "speech:        off (ALARM_SPEAK) or espeak-ng failed"
        fi
        play_room "${files[@]}" > /dev/null 2>&1 & rp=$!
        if remote_up ; then
            play_remote "${files[@]}" > /dev/null 2>&1 & gp=$!
        fi
    fi
    sleep 1                                   # a zenity that cannot open its display is gone by now
    if [ -z "$ALARM_VNC_POPUP" ] ; then
        echo "VNC pop-ups:   off (ALARM_VNC_POPUP)"
    else
        while read -r d a ; do
            n=$((n+1))
            if is_zenity "$XDG_RUNTIME_DIR/n1470-popup$POPTAG$d.pid" ; then
                echo "VNC pop-up:    open on $d"
            else
                echo "VNC pop-up:    FAILED on $d (auth $a)" ; rc=1
            fi
        done < <(vnc_displays)
        [ $n -eq 0 ] && echo "VNC pop-ups:   none (no Xvnc running)"
    fi
    if [ -n "$QUIET" ] ; then
        echo "sound:         skipped (--quiet)"
        sleep 3                               # leave the alarm pop-ups up for a moment
    else
        local sink=$(wpctl inspect @DEFAULT_AUDIO_SINK@ 2> /dev/null | sed -n 's/.*node.description = "\(.*\)"/\1/p')
        local vol=$(wpctl get-volume @DEFAULT_AUDIO_SINK@ 2> /dev/null)
        if wait $rp ; then
            echo "room sound:    played on ${sink:-default sink}${vol:+, ${vol,,}}"
        else
            echo "room sound:    FAILED (pw-play)" ; rc=1
        fi
        if [ -z "$ALARM_REMOTE_PULSE" ] ; then
            echo "remote sound:  off (ALARM_REMOTE_PULSE)"
        elif [ -z "$gp" ] ; then
            echo "remote sound:  $ALARM_REMOTE_PULSE not reachable (tunnel down?)"
        elif wait $gp ; then
            echo "remote sound:  played on $ALARM_REMOTE_PULSE"
        else
            echo "remote sound:  FAILED on $ALARM_REMOTE_PULSE (tunnel up, but no PulseAudio accepting at the far end?)" ; rc=1
        fi
        [ -e "$ALARM_MUTE" ] && echo "note:          the monitor's alarm sound is muted right now (n1470.sh unmute)"
    fi
    notify normal "N1470 HV alarm cleared" "$text"
    popup "" info "N1470 HV alarm cleared" "Cleared at $(date +%T). It was:"$'\n'"$text"
    echo "cleared:       notification and pop-ups sent"
    log_alarm TEST-CLEAR "test-alarm finished, not a real alarm"
    if tail -n 1 "$ALARM_LOG" 2> /dev/null | grep -q TEST-CLEAR ; then
        echo "alarm log:     TEST-START / TEST-CLEAR written to $ALARM_LOG and journalctl -t n1470"
    else
        echo "alarm log:     FAILED to write $ALARM_LOG" ; rc=1
    fi
    if [ $rc -eq 0 ] ; then echo "test-alarm:    all outputs OK" ; else echo "test-alarm:    some outputs FAILED, see above" ; fi
    return $rc
}

# notification + pop-ups when the alarms appear, change, or every NOTIFY_REPEAT s; sound on every
# call; one "cleared" notification + pop-up when they are gone
alarm_actions() {   # ALARM...   (under a lock: a watch and the timer may check at the same time)
    local lf
    exec {lf}>> "$XDG_RUNTIME_DIR/n1470-alarm.lock" && flock -w 10 $lf
    do_alarm_actions "$@"
    exec {lf}>&-
}

do_alarm_actions() {   # ALARM...
    local state=$XDG_RUNTIME_DIR/n1470-alarms-${UNIT,,} text keys keep f files=()
    if [ $# -eq 0 ] ; then
        if [ -s "$state" ] ; then
            notify normal "N1470 HV alarm cleared" "$(cat "$state")"
            popup "" info "N1470 HV alarm cleared" "Cleared at $(date +%T). It was:"$'\n'"$(cat "$state")"
            [ -z "$QUIET" ] && [ ! -e "$ALARM_MUTE" ] && f=$(speech_wav "HV alarm cleared.") && sound_bg queue "$f"
        fi
        rm -f "$state" "$ALARM_MUTE"
        return
    fi
    text=$(printf '%s\n' "$@")
    keys=$(sed 's/ -- .*//' <<< "$text")                 # a changing reading is not a new alarm
    [ "$keys" == "$(cat "$state" 2> /dev/null)" ] && keep=1
    if [ -z "$keep" ] || (( $(date +%s) - $(stat -c %Y "$state" 2> /dev/null || echo 0) >= NOTIFY_REPEAT )) ; then
        notify critical "N1470 HV ALARM" "$text"
        popup "$keep" error "N1470 HV ALARM" "$(date +%T)"$'\n'"$text"
        echo "$keys" > "$state"
    fi
    [ -n "$QUIET" ] || [ -e "$ALARM_MUTE" ] && return
    [ -r "$ALARM_SOUND" ] && files+=("$ALARM_SOUND")
    f=$(speech_wav "$(speech_text "$@")") && files+=("$f")
    sound_bg "" "${files[@]}"                                     # not while the last one still plays
}

# ---- finding the units ----

unit_sn() {   # alias or serial -> serial
    local u
    for u in "${UNITS[@]}" ; do
        [ "${1^^}" == "${u%%:*}" ] && { echo ${u#*:} ; return ; }
    done
    [[ $1 =~ ^[0-9]+$ ]] || die "unknown unit '$1' (use a serial number, all, or one of: ${UNITS[*]})"
    echo $((10#$1))
}

unit_label() {   # serial -> "A s/n 236"
    local u
    for u in "${UNITS[@]}" ; do
        [ "$1" == "${u#*:}" ] && { echo "${u%%:*} s/n $1" ; return ; }
    done
    echo "s/n $1 (not in UNITS)"
}

is_n1470() {   # PID, or any of its ancestors, is an n1470.sh (children like tr inherit the port fd)
    local p=$1
    while [ -n "$p" ] && [ "$p" -gt 1 ] ; do
        grep -qs n1470.sh /proc/$p/cmdline && return 0
        p=$(awk '/^PPid:/ {print $2}' /proc/$p/status 2>/dev/null)
    done
    return 1
}

# open PORT as fd 3; other n1470.sh instances (e.g. under watch) are queued on the flock,
# anything else holding the port is fatal
open_port() {   # PORT
    local p others=""
    for p in $(fuser "$1" 2>/dev/null) ; do
        p=${p//[^0-9]/}
        is_n1470 $p || [ ! -e /proc/$p ] || others+=" $p"   # gone by now: not holding it
    done
    [ -n "$others" ] && die "$1 is in use by:$others (GECO2020?)"
    exec 3<>"$1" || die "cannot open $1"
    flock -w 10 3 || die "$1 still locked by another n1470.sh after 10 s"
    stty -F "$1" 9600 cs8 -parenb -cstopb ixon ixoff clocal cread raw -echo || die "cannot configure $1"
}

# FTDI 0403:6001 rev 0400 (FT232B) with no USB serial number: the N1470 adapters
# (the PiLas FT232R has a serial number and rev 0600, so it is never probed)
candidates() {
    local l
    for l in /dev/serial/by-path/* ; do
        [ -e "$l" ] || continue
        udevadm info -q property -n "$l" 2>/dev/null | awk -F= '{ p[$1] = $2 }
            END { exit !(p["ID_VENDOR_ID"] == "0403" && p["ID_MODEL_ID"] == "6001" &&
                         p["ID_REVISION"] == "0400" && p["ID_SERIAL_SHORT"] == "") }' && echo "$l"
    done
}

# ask every candidate port for its serial: FOUND[serial]="port ...", PROBLEMS = ports that did not answer
scan() {
    local l out
    declare -gA FOUND=()
    PROBLEMS=""
    for l in $(candidates) ; do
        if out=$( (open_port "$l" && hv_mon BDSNUM) 2>&1 ) && [[ $out =~ ^\ *([0-9]+)\ *$ ]] ; then
            out=$((10#${BASH_REMATCH[1]}))
            FOUND[$out]+="${FOUND[$out]:+ }$l"
        else
            out=${out#$ME: }
            PROBLEMS+=$'\n'"  $l: ${out#"$l "}"
        fi
    done
}

unit_port() {   # serial -> its port; dies unless exactly one port answers with that serial
    local p=(${FOUND[$1]})
    [ ${#p[@]} -eq 0 ] && die "$(unit_label $1) not found${PROBLEMS:+; ports that did not answer:$PROBLEMS}"
    [ ${#p[@]} -gt 1 ] && die "$(unit_label $1) answers on ${#p[@]} ports (${p[*]}); refusing"
    echo $p
}

# run OP on one unit; call in a subshell (it opens fd 3, and die only ends that unit)
run_unit() {   # SERIAL
    SN=$1
    ME="n1470 [$(unit_label $SN)]"
    PORT=$(unit_port $SN) || exit 1
    open_port "$PORT"
    local got=$(hv_mon BDSNUM)
    [[ $got =~ ^\ *0*$SN\ *$ ]] || die "$PORT now answers s/n '$got'; refusing"
    case $OP in
        status)
            status ;;
        check)
            check ;;
        mon)
            local ch="" ; [ -n "${ARGS[1]}" ] && ch=$(parse_ch ${ARGS[1]})
            hv_mon "${ARGS[0]^^}" $ch ;;
        set)
            hv_cmd SET ${ARGS[0]^^} $(parse_ch ${ARGS[1]}) ${ARGS[2]} ;;
        off)
            hv_cmd SET OFF $(parse_ch ${ARGS[0]}) ;;
        on)
            hv_cmd SET ON $(parse_ch ${ARGS[0]}) ;;
    esac
}

# ---- main ----

UNIT=""
if [ "$1" == "-u" ] ; then
    [ $# -ge 2 ] || usage
    UNIT=$2 ; shift 2
fi
[ $# -lt 1 ] && usage
OP=$1 ; shift
ARGS=("$@")

# check the arguments before touching any port
case $OP in
    units|status|mute|unmute)
        [ $# -eq 0 ] || usage ;;
    test-alarm)
        for a in "$@" ; do
            case $a in --color) COLOR=1 ;; --quiet) QUIET=1 ;; --remote-only) REMOTE_ONLY=1 ;; *) usage ;; esac
        done ;;
    check)
        for a in "$@" ; do
            case $a in --color) COLOR=1 ;; --quiet) QUIET=1 ;; *) usage ;; esac
        done ;;
    mon)
        [ $# -ge 1 ] || usage
        [ -n "$2" ] && parse_ch $2 > /dev/null ;;
    set)
        [ $# -eq 3 ] || usage
        [[ $SETTABLE == *" ${1^^} "* ]] || die "can only set:$SETTABLE(use on/off for power)"
        parse_ch $2 > /dev/null
        [[ $3 =~ ^[0-9]+(\.[0-9]+)?$ ]] || die "value must be a number" ;;
    off)
        [ $# -eq 1 ] || usage
        parse_ch $1 > /dev/null ;;
    on)
        [ $# -ge 1 ] || usage
        [ "$2" == "--box-safe" ] || die "refusing to power on without --box-safe (the dark box is closed and the detector is safe to power)"
        parse_ch $1 > /dev/null ;;
    *)
        usage ;;
esac
if [ "$OP" == test-alarm ] ; then
    if [ -n "$REMOTE_ONLY" ] ; then test_remote ; else test_alarm ; fi   # -u is ignored: no HV access
    exit
elif [ "$OP" == mute ] || [ "$OP" == unmute ] ; then
    [ -z "$UNIT" ] || usage
    if [ $OP == mute ] ; then
        touch "$ALARM_MUTE" && echo "alarm sound off until the alarm clears (n1470.sh unmute to undo)"
    else
        rm -f "$ALARM_MUTE" && echo "alarm sound on"
    fi
    exit
elif [ "$OP" == units ] ; then
    [ -z "$UNIT" ] || usage
elif [ -z "$UNIT" ] ; then
    die "say which unit: -u $(printf '%s|' "${UNITS[@]%%:*}")SERIAL|all"
elif [ "${UNIT,,}" != all ] ; then
    SN=$(unit_sn "$UNIT") || exit 1
else
    case $OP in status|check|mon|off) ;; *) die "-u all only works with status, check, mon and off" ;; esac
fi

scan

if [ "$OP" == units ] ; then
    for sn in $(printf '%s\n' "${!FOUND[@]}" | sort -n) ; do
        for p in ${FOUND[$sn]} ; do
            printf '%-22s %s (%s)\n' "$(unit_label $sn)" "$p" "$(basename "$(readlink -f "$p")")"
        done
    done
    for u in "${UNITS[@]}" ; do
        [ -n "${FOUND[${u#*:}]}" ] || echo "$(unit_label ${u#*:}) not found"
    done
    [ -n "$PROBLEMS" ] && echo "ports that did not answer:$PROBLEMS"
    exit 0
fi

# with -u all: the units in UNITS, then any other one found; keep going when one fails
if [ "${UNIT,,}" != all ] ; then
    list=$SN
else
    list="${UNITS[*]#*:}"
    for sn in $(printf '%s\n' "${!FOUND[@]}" | sort -n) ; do
        [[ " $list " == *" $sn "* ]] || list+=" $sn"
    done
fi

# check: collect every unit's output first, so the alarm banner can go on top
if [ $OP == check ] ; then
    rc=0 ; out="" ; alarms=()
    for sn in $list ; do
        o=$( ( run_unit $sn ) 2>&1 ) ; r=$?
        unit_assess $sn $r "$o"                # an unreadable unit is an alarm only if its HV was in use
        alarms+=("${UA_ALARMS[@]}")
        out+="${out:+$'\n\n'}$o${UA_NOTE:+$'\n'$UA_NOTE}"
    done
    [ ${#alarms[@]} -gt 0 ] && rc=3            # else 0, also for units off / absent
    alarm_actions "${alarms[@]}"
    if [ ${#alarms[@]} -gt 0 ] ; then
        mute=""
        [ -z "$QUIET" ] && [ -e "$ALARM_MUTE" ] && mute="(sound muted until cleared: n1470.sh unmute)"
        banner "${COLOR:+red}" "HV ALARM   $(date +%T)   ${#alarms[@]} problem(s)" "${alarms[@]}" ${mute:+"$mute"}
    elif [ -n "$COLOR" ] ; then
        printf '\e[1;97;42m%-78s\e[0m\n' "HV OK   no alarms"
    else
        echo "HV OK   no alarms"
    fi
    echo "$out"
    exit $rc
fi

if [ "${UNIT,,}" != all ] ; then
    ( run_unit $SN )
    exit
fi

rc=0 ; first=1
for sn in $list ; do
    if [ $OP == status ] ; then
        [ $first ] || echo ; first=
        ( run_unit $sn ) || rc=1
    else
        ( run_unit $sn ) | sed "s|^|$(unit_label $sn): |"
        [ ${PIPESTATUS[0]} -eq 0 ] || rc=1
    fi
done
exit $rc
