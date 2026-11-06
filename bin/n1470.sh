#! /bin/bash
#
# The CAEN N1470 HV supplies, through their EPICS IOC (service ioc-n1470: both units, CA port 5070;
# docs/epics.md). -u UNIT is A (s/n 236), B (s/n 1593), or all (status, check and off only).
#
#   n1470.sh units                  the port the IOC found each unit on
#   n1470.sh -u A status            board info and a table of the four channels
#   n1470.sh -u all check           status, then the alarms the IOC's records see; exit 3 if any
#                                   (to watch: watch -n 5 n1470.sh -u all check)
#   n1470.sh -u A mon PAR [CH]      one parameter, e.g. "mon VMON", "mon VSET 2", "mon BDILK"
#   n1470.sh -u A set PAR CH VAL    PAR one of VSET ISET MAXV RUP RDW TRIP PDWN; CH 0-3 or all
#   n1470.sh -u A off CH|all        switch off; "-u all off all" = all 8 channels
#   n1470.sh -u A on CH|all --box-safe
#                                   switch on: only the detector's channels (a row in its hv.conf),
#                                   and only with --box-safe (the dark box is closed, the detector safe)
#   n1470.sh mute | unmute          the alarm sound, as the panel's mute (it ends when the alarm clears)
#
# The IOC owns the serial ports; with it stopped (e.g. for GECO2020) nothing here works: use the
# front panel. The IOC also works out the alarms (n1470_alarm_*.db); bin/hv_annunciator.sh sounds,
# logs and mails them. The IOC refuses an ON unless N1470:BOX_SAFE is armed, which "on --box-safe"
# does just before it.

UNITS=(A:236 B:1593)   # alias:board serial; "-u all" goes through them in this order (testbench.sh check
                       # compares this line with HV_UNITS in db/testbench.conf)
NCH=4
SETTABLE=" VSET ISET MAXV RUP RDW TRIP PDWN "
STAT_BITS=(ON RUP RDW OVC OVV UNV MAXV TRIP OVP OVT DIS KILL ILK NOCAL)
ALARM_BITS=" OVC OVV UNV MAXV TRIP OVP OVT ILK "    # as A_BITS in the IOC
BOARD_PARS=(BDNAME BDSNUM BDFREL BDCTR BDILK BDALARM)
CH_PARS=(VSET VMON ISET IMON RUP RDW MAXV TRIP POL PDWN STAT NAME USED ILIM A_BITS A_VDEV A_IMON A_NOTE TEST)
COLS=(VSET VMON ISET IMON RUP RDW MAXV POL STAT)     # the status table
declare -A FMT=([VSET]=%06.1f [VMON]=%06.1f [TRIP]=%06.1f [ISET]=%07.2f [IMON]=%07.2f
                [RUP]=%03d [RDW]=%03d [MAXV]=%04d [STAT]=%05d)   # as the supply sends them
FAST_MAX=6      # s: VMON IMON STAT are read every 2 s, the rest every 10 s; older than three reads = stale
SLOW_MAX=30

die() { echo "n1470${U:+ [$U]}: $*" >&2 ; exit 1 ; }
usage() { sed -n '3,19p' "$0" | sed 's/^# \{0,1\}//' ; exit 2 ; }

. "$(dirname "$(readlink -f "$0")")/../lib/tb.sh"

unit_alias() {   # A, a, 236 -> A
    local u
    for u in "${UNITS[@]}" ; do
        [ "${1^^}" == "${u%%:*}" ] || [ "$1" == "${u#*:}" ] && { echo "${u%%:*}" ; return ; }
    done
    die "unknown unit '$1' (A, B, or all)"
}
serial_of() { local u ; for u in "${UNITS[@]}" ; do [ "$1" == "${u%%:*}" ] && echo "${u#*:}" ; done ; }

channels() {   # 0-3 or all -> the channel numbers
    case $1 in
        all|ALL) echo 0 1 2 3 ;;
        [0-3])   echo $1 ;;
        *)       die "channel must be 0-3 or all" ;;
    esac
}

# Everything of unit U in one caget: V[BDSNUM], V[CH2:VMON], ...; STALE[...] says why a reading
# is unusable (INVALID, or older than three of the IOC's reads). Dies if the unit cannot be read.
read_unit() {
    local P=N1470_$U p v why c
    local pvs=("$P:SN_OK" "$P:A_LOST" "$P:A_BOARD")
    for p in "${BOARD_PARS[@]}" ; do pvs+=("$P:$p") ; done
    for (( c=0 ; c<NCH ; c++ )) ; do for p in "${CH_PARS[@]}" ; do pvs+=("$P:CH$c:$p") ; done ; done
    declare -gA V=() STALE=()
    while IFS='|' read -r p v why ; do V[$p]=$v STALE[$p]=$why ; done < <(
        tb_ca caget -a -F '|' -w 2 "${pvs[@]}" 2> /dev/null |
        awk -F'|' -v pre="$P:" -v now=$(date +%s) -v fast=$FAST_MAX -v slow=$SLOW_MAX '
            NF >= 3 && index($1, pre) == 1 {
                n = substr($1, length(pre) + 1) ; par = n ; sub(/^CH[0-9]:/, "", par)
                split($2, d, /[-: ]/) ; age = now - mktime(d[1] " " d[2] " " d[3] " " d[4] " " d[5] " " int(d[6]))
                why = ""
                if ($5 == "INVALID" && par !~ /^(NAME|USED)$/) why = "INVALID"
                else if (par ~ /^(VMON|IMON|STAT)$/ && age > fast) why = age " s old"
                else if (par ~ /^(VSET|ISET|RUP|RDW|MAXV|TRIP|POL|PDWN|BDSNUM|BDCTR|BDILK|BDALARM)$/ && age > slow) why = age " s old"
                print n "|" $3 "|" why
            }')
    [ ${#V[@]} -gt 0 ] || die "no answer from the N1470 IOC (systemctl --user status ioc-n1470)"
    [ -z "${STALE[BDSNUM]}" ] || die "cannot be read: BDSNUM ${STALE[BDSNUM]} (unit off or USB out?)"
    [ "${V[SN_OK]}" == 1 ] || die "its port answers s/n '${V[BDSNUM]}', not $(serial_of $U): refusing"
}

value() {   # PAR [CH]: one reading, formatted as the supply sends it; dies if stale
    local k=${2:+CH$2:}$1 v
    [ -z "${STALE[$k]}" ] || die "no reading: $k ${STALE[$k]}"
    v=${V[$k]}
    if [[ ${FMT[$1]} == *d ]] ; then printf "${FMT[$1]}" "${v%.*}"
    elif [ -n "${FMT[$1]}" ] ; then printf "${FMT[$1]}" "$v"
    else printf '%s' "$v"
    fi
}

flags() {   # STAT -> ON|RUP ..., OFF for none
    local s=$((10#$1)) i out=""
    for i in "${!STAT_BITS[@]}" ; do (( s >> i & 1 )) && out+="${out:+|}${STAT_BITS[$i]}" ; done
    echo "${out:-OFF}"
}

status() {
    local p c
    printf '%-7s %s s/n %s, %s\n' UNIT $U "$(serial_of $U)" "$(tb_ca caget -t -S -w 2 "N1470_$U:DEV.VAL\$" 2> /dev/null)"
    for p in BDNAME BDSNUM BDFREL BDCTR BDILK ; do printf '%-7s %s\n' $p "$(value $p)" ; done
    printf 'CH' ; printf '%9s' "${COLS[@]}" ; printf '  flags\n'
    for (( c=0 ; c<NCH ; c++ )) ; do
        printf '%2d' $c
        for p in "${COLS[@]}" ; do printf '%9s' "$(value $p $c)" || exit 1 ; done
        printf '  %s%s\n' "$(flags ${V[CH$c:STAT]})" "${V[CH$c:NAME]:+   = ${V[CH$c:NAME]}}"
    done
    echo "units: V, uA, V/s"
}

# One line per alarm the IOC's records hold for unit U (its A_* records; the readings say why).
# A used channel in DIS / KILL is a NOTE only (it cannot be switched on), as on the panel.
alarm_lines() {
    local c b s name
    for (( c=0 ; c<NCH ; c++ )) ; do
        name="ch$c${V[CH$c:NAME]:+ (${V[CH$c:NAME]})}"
        s=$((10#${V[CH$c:STAT]:-0}))
        if [ "${V[CH$c:A_BITS]}" == 1 ] ; then
            for b in "${!STAT_BITS[@]}" ; do
                (( s >> b & 1 )) && [[ $ALARM_BITS == *" ${STAT_BITS[$b]} "* ]] && echo "ALARM   $U $name: ${STAT_BITS[$b]}"
            done
        fi
        [ "${V[CH$c:A_VDEV]}" == 1 ] && echo "ALARM   $U $name: VMON off -- VMON ${V[CH$c:VMON]} V vs VSET ${V[CH$c:VSET]} V"
        [ "${V[CH$c:A_IMON]}" == 1 ] && echo "ALARM   $U $name: IMON high -- IMON ${V[CH$c:IMON]} uA (ISET ${V[CH$c:ISET]}, limit ${V[CH$c:ILIM]})"
        [ "${V[CH$c:TEST]}" == TEST ] && echo "ALARM   $U $name: TEST alarm (from the panel)"
        [ "${V[CH$c:A_NOTE]}" == 1 ] && echo "NOTE    $U $name: used channel not ready ($(flags $s))"
    done
    [ "${V[A_BOARD]}" == 1 ] && echo "ALARM   $U board: BDALARM ${V[BDALARM]}, interlock ${V[BDILK]}"
    return 0
}

# caput -c (returns once the IOC has written and read back); fails if the IOC refused it or the
# supply did not answer OK (the record is then INVALID, the reply in LAST_ERR)
put() {   # PV VALUE
    local out
    out=$(tb_ca caput -c -w 10 "$1" "$2" 2>&1) && [[ $out != *[Ee]rror* && $out != *denied* ]] ||
        die "caput $1 $2: $(grep -m 1 -i 'error\|denied\|timed out' <<< "$out" || tail -n 1 <<< "$out")"
    [ "$(tb_ca caget -t -w 2 "$1.SEVR" 2> /dev/null)" != INVALID ] ||
        die "$1 $2 -> $(tb_ca caget -t -w 2 "N1470_$U:LAST_ERR" 2> /dev/null) (the supply did not accept it)"
}

switch_on() {   # CH|all
    local c pv=N1470_$U:CH$1:ON i rc
    for c in $(channels $1) ; do
        [ "${V[CH$c:USED]}" == used ] ||
            die "ch$c is not in the detector's hv.conf: not switched on (add it there to use it)"
    done
    [ "$1" == all ] && pv=N1470_$U:ALL:ON
    tb_ca caput -t -w 2 N1470:BOX_SAFE 1 > /dev/null 2>&1 || die "cannot arm N1470:BOX_SAFE"
    for i in $(seq 30) ; do     # the IOC's access security takes a moment to open the ON record
        tb_ca cainfo -w 2 "$pv" 2> /dev/null | grep -q 'Access: *read, write' && break
        sleep 0.1
    done
    ( put "$pv" 1 ) ; rc=$?
    tb_ca caput -t -w 2 N1470:BOX_SAFE 0 > /dev/null 2>&1    # it also drops back after 10 s by itself
    [ $rc -eq 0 ] && echo "$U ch$1 on"
    return $rc
}

# one command on unit U (in a subshell: die ends only that unit)
run() {   # OP ARGS...
    local c
    read_unit
    case $1 in
        status) status ;;
        check)  status && alarm_lines ;;
        mon)
            if [[ " ${BOARD_PARS[*]} " == *" ${2^^} "* ]] ; then value ${2^^} ; echo
            else
                local out=()
                for c in $(channels ${3:-all}) ; do out+=("$(value ${2^^} $c)") || exit 1 ; done
                echo "${out[*]}"
            fi ;;
        set)
            for c in $(channels $3) ; do put "N1470_$U:CH$c:${2^^}_SP" "${4^^}" ; done
            read_unit
            for c in $(channels $3) ; do echo "$U ch$c ${2^^} $(value ${2^^} $c)" ; done ;;
        off)
            if [ "$2" == all ] ; then put "N1470_$U:ALL:OFF" 1 ; else put "N1470_$U:CH$(channels $2):OFF" 1 ; fi
            echo "$U ch$2 off" ;;
        on) switch_on $2 ;;
    esac
}

# ---- main ----

UNIT=""
if [ "$1" == -u ] ; then [ $# -ge 2 ] || usage ; UNIT=$2 ; shift 2 ; fi
OP=$1 ; [ -n "$OP" ] || usage ; shift

# the arguments first, before anything is written
case $OP in
    units|status|check|mute|unmute) [ $# -eq 0 ] || usage ;;
    mon) [ $# -ge 1 ] && [ $# -le 2 ] || usage ; [ -z "$2" ] || channels $2 > /dev/null ;;
    set)
        [ $# -eq 3 ] || usage
        [[ $SETTABLE == *" ${1^^} "* ]] || die "can only set:$SETTABLE(on / off for power)"
        channels $2 > /dev/null
        if [ "${1^^}" == PDWN ] ; then [[ ${3^^} == RAMP || ${3^^} == KILL ]] || die "PDWN must be RAMP or KILL"
        else [[ $3 =~ ^[0-9]+(\.[0-9]+)?$ ]] || die "value must be a number"
        fi ;;
    off) [ $# -eq 1 ] || usage ; channels $1 > /dev/null ;;
    on)
        [ $# -eq 2 ] || usage
        [ "$2" == --box-safe ] || die "refusing to switch on without --box-safe (the dark box is closed and the detector safe to power)"
        channels $1 > /dev/null ;;
    *) usage ;;
esac

case $OP in
    units)
        for u in "${UNITS[@]}" ; do
            printf '%-12s %s\n' "${u%%:*} s/n ${u#*:}" "$(tb_ca caget -t -S -w 2 "N1470_${u%%:*}:DEV.VAL\$" 2> /dev/null || echo 'no answer from the IOC')"
        done
        echo "(the ports ioc-n1470 found when it started; /nonexistent/ = unit off then: restart the IOC)"
        exit 0 ;;
    mute|unmute)
        tb_ca caput -t -w 2 N1470:MUTE $([ $OP == mute ] && echo 1 || echo 0) > /dev/null || die "no answer from the N1470 IOC"
        echo "alarm sound $([ $OP == mute ] && echo "off until the alarm clears" || echo on)"
        exit 0 ;;
esac
[ -n "$UNIT" ] || die "say which unit: -u A, -u B or -u all"

if [ "${UNIT,,}" != all ] ; then
    U=$(unit_alias "$UNIT") || exit 1
    ( run $OP "$@" )
    exit
fi
case $OP in status|check|off) ;; *) die "-u all only works with status, check and off" ;; esac

# every unit; one that fails does not stop the others
rc=0 out=""
for u in "${UNITS[@]}" ; do
    U=${u%%:*}
    o=$( ( run $OP "$@" ) 2>&1 ) || { rc=1 ; [ $OP == check ] && o+=$'\n'"NOTE    $U: cannot be read (an alarm only while its HV is in use)" ; }
    out+="${out:+$'\n\n'}$o"
done
U=""
[ $OP == check ] || { echo "$out" ; exit $rc ; }

# check: the alarms on top; a unit that cannot be read is an alarm only while its HV is in use (A_LOST)
alarms=$(grep '^ALARM' <<< "$out")
for u in "${UNITS[@]}" ; do
    [ "$(tb_ca caget -t -w 2 N1470_${u%%:*}:A_LOST 2> /dev/null)" == 1 ] &&
        alarms+="${alarms:+$'\n'}ALARM   ${u%%:*}: no readback while its HV is in use"
done
if [ -n "$alarms" ] ; then echo "HV ALARM   $(date +%T)" ; echo "$alarms" ; else echo "HV OK   no alarms" ; fi
echo
grep -v '^ALARM' <<< "$out"
[ -n "$alarms" ] && exit 3
exit $rc
