#! /bin/bash
#
# The HV alarm annunciator (service hv-annunciator; docs/epics.md). The N1470 IOC works out the
# alarms; every 2 s this reads them (n1470.sh -u all check, and the laser's PILAS:LDTEMP / LDTEC) and
#   - plays the alarm sound in the room, again and again while an alarm is neither muted
#     (N1470:MUTE, the panel or n1470.sh mute) nor acknowledged (the panel's ack, N1470_U:CHn:ACK);
#     a mute and an ack end when their alarm clears;
#   - writes one line to logs/hv_alarms.log when alarms start, change or clear, when a unit's HV goes
#     on or off, and when the IOC stops or comes back;
#   - mails NOTIFY_EMAIL (db/testbench.conf) when alarms start, change or clear;
#   - fills the panel's N1470:ALARM_TXT (now), N1470:ALARM_HIST (the log, newest first) and ANN_ALIVE.
# The sound is the one picked on the panel (N1470:SOUND, from bin/alarm_sounds.list; kept in
# state/alarm_sound). To test the whole chain: the panel's "test" button on a channel (30 s).

. "$(dirname "$(readlink -f "$0")")/../lib/tb.sh"
tb_site
N1470=$TB_ROOT/bin/n1470.sh
SOUNDS=$TB_ROOT/bin/alarm_sounds.list
SOUND_STATE=$TB_STATE/alarm_sound
ALARM_LOG=$TB_LOGS/hv_alarms.log
PERIOD=2

log() { echo "$(date -Iseconds) $*" >> "$ALARM_LOG" ; }
mail_out() {   # SUBJECT TEXT
    [ -n "$NOTIFY_EMAIL" ] || return 0
    ( echo "$2" | timeout 30 mail -s "$1" $NOTIFY_EMAIL ) > /dev/null 2>&1 &
}

sound_labels() { awk '!/^#/ && NF { print $1 }' "$SOUNDS" | head -n 16 ; }
sound_file()   { awk -v l="$1" '!/^#/ && $1 == l { print $2 ; exit }' "$SOUNDS" ; }
SOUND_LABEL=$(cat "$SOUND_STATE" 2> /dev/null)
SOUND=$(sound_file "$SOUND_LABEL") ; [ -r "$SOUND" ] || SOUND=/usr/share/sounds/freedesktop/stereo/dialog-warning.oga
PLAYER=""
play() {   # play SOUND in the background, unless the last one is still playing
    kill -0 "$PLAYER" 2> /dev/null && return
    pw-play "$SOUND" > /dev/null 2>&1 &
    PLAYER=$!
}

# the panel's sound picker: a new pick is kept and played once; "test" plays it once
sound_pick() {
    local i tst label f
    read -r i tst < <(tb_ca caget -n -t -w 1 N1470:SOUND N1470:SOUND_TEST 2> /dev/null | paste -sd' ')
    [ -n "$tst" ] || return
    label=$(sound_labels | sed -n "$((i + 1))p")
    if [ -n "$label" ] && [ "$label" != "$SOUND_LABEL" ] && f=$(sound_file "$label") && [ -r "$f" ] ; then
        SOUND_LABEL=$label SOUND=$f
        echo "$label" > "$SOUND_STATE"
        log "UNIT alarm sound now $label, picked on the panel"
        tst=1
    fi
    [ "$tst" == 1 ] && { tb_ca caput -t -w 1 N1470:SOUND_TEST 0 > /dev/null 2>&1 ; PLAYER="" ; play ; }
}

put_text() {   # PV TEXT MAX: a CHAR waveform, cut to whole lines within MAX bytes (the record's size)
    local t=${2:0:$3}
    [ ${#2} -gt $3 ] && t=${t%$'\n'*}
    tb_ca caput -t -S -w 1 "$1" "$t" > /dev/null 2>&1
}

log "UNIT annunciator started (hv_annunciator.sh, pid $$)"
last_keys="" ioc=up
declare -A inuse=() ack=()
while : ; do
    out=$("$N1470" -u all check 2>&1)
    alarms=$(sed -n 's/^ALARM   //p' <<< "$out")

    # the IOC itself: its stopping is an alarm only while the HV was in use
    if tb_ca caget -t -w 1 N1470:ALARM > /dev/null 2>&1 ; then
        [ $ioc == down ] && log "UNIT N1470 IOC answering again"
        ioc=up
    else
        [ $ioc == up ] && log "UNIT N1470 IOC not answering"
        ioc=down
        [[ " ${inuse[*]} " == *" 1 "* ]] && alarms+="${alarms:+$'\n'}N1470 IOC not answering while the HV was in use"
    fi

    # the laser
    read -r temp tec temp_sevr tec_sevr < <(tb_ca caget -t -w 1 PILAS:LDTEMP PILAS:LDTEC PILAS:LDTEMP.SEVR PILAS:LDTEC.SEVR 2> /dev/null | paste -sd' ')
    [ "$temp" == bad ] && [ "$temp_sevr" != INVALID ] && alarms+="${alarms:+$'\n'}laser: LD temperature bad"
    [ "$tec" == off ] && [ "$tec_sevr" != INVALID ] && alarms+="${alarms:+$'\n'}laser: LD TEC off"

    if [ $ioc == up ] ; then
        # HV on / off per unit
        for u in A B ; do
            v=$(tb_ca caget -t -w 1 N1470_$u:INUSE 2> /dev/null) || continue
            v=${v%.*}
            [ -n "${inuse[$u]}" ] && [ "$v" != "${inuse[$u]}" ] &&
                log "$([ "$v" == 1 ] && echo HV-ON || echo HV-OFF) unit $u: $(awk -v u=$u '$1 == "UNIT" { cur = $2 }
                    cur == u && $1 ~ /^[0-3]$/ && NF >= 11 && $11 != "OFF" { printf "%sch%s %s VMON %s", s, $1, $11, $3 ; s = ", " }' <<< "$out")"
            inuse[$u]=$v
        done

        # acknowledged channels do not sound; an ack ends when its channel has no alarm any more
        ack=()
        while read -r pv v ; do ack[$pv]=$v ; done < <(tb_ca caget -w 1 N1470_{A,B}:CH{0,1,2,3}:ACK 2> /dev/null)
        for pv in "${!ack[@]}" ; do
            [[ $pv =~ ^N1470_([AB]):CH([0-3]) ]] && [ "${ack[$pv]}" == acknowledged ] || continue
            grep -q "^${BASH_REMATCH[1]} ch${BASH_REMATCH[2]}" <<< "$alarms" && continue
            tb_ca caput -t -w 1 $pv 0 > /dev/null 2>&1 ; ack[$pv]=""
        done
        unacked=0
        while IFS= read -r l ; do
            [ -n "$l" ] || continue
            [[ $l =~ ^([AB])\ ch([0-3]) ]] && [ "${ack[N1470_${BASH_REMATCH[1]}:CH${BASH_REMATCH[2]}:ACK]}" == acknowledged ] && continue
            unacked=$((unacked + 1))
        done <<< "$alarms"

        sound_pick
        [ -z "$alarms" ] && tb_ca caput -t -w 1 N1470:MUTE 0 > /dev/null 2>&1     # a mute ends with the alarm
        [ $unacked -gt 0 ] && [ "$(tb_ca caget -t -w 1 N1470:MUTE 2> /dev/null)" != muted ] && play

        if [ -n "$alarms" ] ; then put_text N1470:ALARM_TXT "$(date +%T)  alarms:"$'\n'"$alarms" 2000
        else put_text N1470:ALARM_TXT "$(date +%T)  no alarms"$'\n'"$(sed -n 's/^NOTE    //p' <<< "$out")" 2000
        fi
        put_text N1470:ALARM_HIST "$(tail -n 40 "$ALARM_LOG" | cut -c 1-220 | tac)" 4000
        tb_ca caput -t -w 1 N1470:ANN_ALIVE 1 > /dev/null 2>&1
    elif [ -n "$alarms" ] ; then
        play       # no IOC: no mute or ack to read
    fi

    # start / change / clear: a changing reading (after " -- ") is not a change
    keys=$(sed 's/ -- .*//' <<< "$alarms")
    if [ "$keys" != "$last_keys" ] ; then
        if [ -z "$keys" ] ; then
            log "CLEAR was: $(paste -sd';' <<< "$last_keys")"
            mail_out "417 test bench: HV alarm cleared" "Cleared at $(date '+%F %T'). It was:"$'\n'"$last_keys"
        else
            log "$([ -z "$last_keys" ] && echo START || echo CHANGE) $(paste -sd';' <<< "$alarms")"
            mail_out "417 test bench: HV ALARM" "$(date '+%F %T')"$'\n'"$alarms"$'\n\n'"$out"
        fi
        last_keys=$keys
    fi
    sleep $PERIOD
done
