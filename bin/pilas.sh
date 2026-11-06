#! /bin/bash
#
# The PiLas picosecond laser, through its EPICS IOC (service ioc-pilas, CA port 5072; docs/epics.md).
#
#   pilas.sh status               serial numbers, wavelength and the full state (as the controller words it)
#   pilas.sh set PAR VAL          PAR one of
#                                   f     internal oscillator frequency [Hz]  25 ... 125000000
#                                   ts    trigger source  0 internal, 1 ext. adjustable, 2 ext. TTL
#                                   te    trigger edge    0 falling, 1 rising
#                                   tl    trigger level [mV]  -4800 ... 4800
#                                   tm    tune mode       0 manual, 1 auto
#                                   tune  tune value [%]  0 ... 100 (only in manual tune mode)
#   pilas.sh off                  pulsed laser emission off
#   pilas.sh on --box-safe        pulsed laser emission on; refuses without the flag
#   pilas.sh store                store the settings as the boot default
#   pilas.sh save N | load N --box-safe
#                                 save to / load from memory N = 1, 2, 3 (load needs the flag: a stored
#                                 memory might hold emission on)


FAST_MAX=6      # s: PILAS:LD is read every 2 s, the rest every 10 s; older than three reads = stale
SLOW_MAX=30

die() { echo "pilas: $*" >&2 ; exit 1 ; }
usage() { sed -n '3,18p' "$0" | sed 's/^# \{0,1\}//' ; exit 2 ; }

. "$(dirname "$(readlink -f "$0")")/../lib/tb.sh"

get() {   # PV...: their values, one per line; dies if one is INVALID or stale
    local out
    out=$(tb_ca caget -a -S -F '|' -w 2 "$@" 2> /dev/null) || die "no answer from the PiLas IOC (systemctl --user status ioc-pilas)"
    awk -F'|' -v now=$(date +%s) -v fast=$FAST_MAX -v slow=$SLOW_MAX '
        { split($2, d, /[-: ]/) ; age = now - mktime(d[1] " " d[2] " " d[3] " " d[4] " " d[5] " " int(d[6]))
          if ($5 == "INVALID") { print "pilas: no reading: " $1 " INVALID" > "/dev/stderr" ; bad = 1 }
          else if (($1 == "PILAS:LD" && age > fast) || ($1 != "PILAS:VERSION_TXT" && age > slow)) {
              print "pilas: no reading: " $1 " is " age " s old" > "/dev/stderr" ; bad = 1 }
          print $3 }
        END { exit bad }' <<< "$out" || exit 1
}

# the controller's own reply lines, kept as text by the IOC: "key: value", the degree sign as "deg"
# (caget -S escapes CR, LF, tab and the degree byte)
status() {
    get PILAS:VERSION_TXT PILAS:STATE_TXT PILAS:LDTEC_TXT PILAS:LDTEMP_TXT | while IFS= read -r t ; do printf '%b\n' "$t" ; done |
        tr -d '\r' | LC_ALL=C sed -e $'s/\xb0/deg /' -e 's/:[[:space:]]*/: /' -e 's/[[:space:]]*$//' |
        grep -av -e '^$' -e '^PiLas system state' -e '^-*$' -e '^\(software\|controller hardware\|laser head hardware\) version'
}

# caput -c (returns once the IOC has written and read back); fails if the IOC refused it or the
# controller did not answer "done" (the record is then INVALID, its reply in LAST_ERR)
put() {   # PV VALUE
    local out
    out=$(tb_ca caput -c -w 15 "$1" "$2" 2>&1) && [[ $out != *[Ee]rror* && $out != *denied* ]] ||
        die "caput $1 $2: $(grep -m 1 -i 'error\|denied\|timed out' <<< "$out" || tail -n 1 <<< "$out")"
    [ "$(tb_ca caget -t -w 2 "$1.SEVR" 2> /dev/null)" != INVALID ] ||
        die "$1 $2 -> $(tb_ca caget -t -w 2 PILAS:LAST_ERR 2> /dev/null)"
}

armed_put() {   # PV VALUE, with PILAS:BOX_SAFE armed for it
    local i rc
    tb_ca caput -t -w 2 PILAS:BOX_SAFE 1 > /dev/null 2>&1 || die "cannot arm PILAS:BOX_SAFE"
    for i in $(seq 30) ; do     # the IOC's access security takes a moment to open the record
        tb_ca cainfo -w 2 "$1" 2> /dev/null | grep -q 'Access: *read, write' && break
        sleep 0.1
    done
    ( put "$1" "$2" ) ; rc=$?
    tb_ca caput -t -w 2 PILAS:BOX_SAFE 0 > /dev/null 2>&1    # it also drops back after 10 s by itself
    return $rc
}

in_range() {   # VAL MIN MAX
    [[ $1 =~ ^-?[0-9]+$ ]] && (( $1 >= $2 && $1 <= $3 )) || die "value must be an integer $2 ... $3"
}

OP=$1 ; [ -n "$OP" ] || usage ; shift
case $OP in
    status)
        [ $# -eq 0 ] || usage
        status ;;
    set)
        [ $# -eq 2 ] || usage
        PAR=${1,,} VAL=$2
        case $PAR in
            f)     in_range "$VAL" 25 125000000 ;;
            ts)    in_range "$VAL" 0 2 ;;
            te|tm) in_range "$VAL" 0 1 ;;
            tl)    in_range "$VAL" -4800 4800 ;;
            tune)  [[ $VAL =~ ^[0-9]+(\.[0-9])?$ ]] && awk -v v="$VAL" 'BEGIN { exit !(v <= 100) }' ||
                       die "tune value must be 0 ... 100 % (0.1 % steps)" ;;
            *)     die "can only set: f ts te tl tm tune (on / off for emission)" ;;
        esac
        put "PILAS:${PAR^^}_SP" "$VAL"
        echo "$PAR $(get PILAS:${PAR^^})" ;;
    off)
        [ $# -eq 0 ] || usage
        put PILAS:LD_OFF 1
        echo "laser emission: $(get PILAS:LD)" ;;
    on)
        [ "$1" == --box-safe ] || die "refusing to switch emission on without --box-safe (the dark box is closed and the detector safe to light)"
        armed_put PILAS:LD_ON 1 || exit 1
        echo "laser emission: $(get PILAS:LD)" ;;
    store)
        [ $# -eq 0 ] || usage
        put PILAS:STORE 1 && echo "settings stored as the boot default" ;;
    save)
        [ $# -eq 1 ] && [[ $1 == [123] ]] || die "memory position must be 1, 2 or 3"
        put PILAS:SAVE $1 && echo "settings saved to memory $1" ;;
    load)
        [ $# -ge 1 ] && [[ $1 == [123] ]] || die "memory position must be 1, 2 or 3"
        [ "$2" == --box-safe ] || die "load needs --box-safe (a stored memory might hold emission on)"
        armed_put PILAS:LOAD $1 && status ;;
    -h|--help|*)
        usage ;;
esac
