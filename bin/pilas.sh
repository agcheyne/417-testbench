#! /bin/bash
#
# Command-line access to the PiLas picosecond laser controller over USB (FTDI serial, 19200 8N1).
#
# Readback (read-only):
#   pilas.sh status               serial numbers, wavelength + full system state
#   pilas.sh query CMD            raw query, e.g. "query lht?", "query mState?", "query help?"

# Set values (each prints the readback afterwards):
#   pilas.sh set PAR VAL          PAR one of
#                                   f     internal oscillator frequency [Hz]  25 ... 125000000
#                                   ts    trigger source  0 internal, 1 ext. adjustable, 2 ext. TTL
#                                   te    trigger edge    0 falling, 1 rising
#                                   tl    trigger level [mV]  -4800 ... 4800
#                                   tm    tune mode       0 manual, 1 auto
#                                   tune  tune value [%]  0 ... 100  (only accepted in manual tune mode)
#   pilas.sh off                  pulsed laser emission off
#   pilas.sh on --box-safe
#                                 pulsed laser emission on; refuses without the flag
#   pilas.sh store | save N | load N [--box-safe]
#                                 store settings as boot default / save to / load from memory N=1,2,3
#
# Only one program can use the port at a time: close the PiLas GUI first.
# While the EPICS IOC of the laser runs (~/epics/app/iocBoot/iocPiLas, docs/epics_plan.md) it has
# the port, and every command goes through its PVs instead, with the same output; there "load"
# needs --box-safe (a stored memory might hold emission on), and "query" knows the commands of
# "status" plus ld? f? ts? te? tl? tm? tune? lht?. PILAS_BACKEND=serial forces the port (stop the
# IOC first), PILAS_BACKEND=ioc never falls back to it.

PORT=/dev/serial/by-id/usb-FTDI_FT232R_USB_UART_BG01VUCO-if00-port0   # -> /dev/ttyUSB2

usage() { sed -n '3,22p' "$0" | sed 's/^# \{0,1\}//' ; exit 2 ; }
die()   { echo "pilas: $*" >&2 ; exit 1 ; }

# send one command, print the reply lines (tabs/padding squeezed to "key: value"); fail on no reply.
# The reply ends at a line matching LAST (a grep -E pattern), or else after 0.3 s without a new line
# (that wait was most of the time of a status: give LAST where the reply is known).
pl_cmd() {   # CMD [LAST]
    [ -n "$IOC" ] && { ioc_query "$1" | pl_fmt ; return ${PIPESTATUS[0]} ; }
    local line reply="" junk
    while read -r -t 0.01 -u 3 junk ; do : ; done     # drop stale input
    printf '%s\r' "$1" >&3
    read -r -t 2 -u 3 line || die "no reply to $1"
    while : ; do
        line=${line%$'\r'}
        [ -n "$line" ] && reply+="$line"$'\n'
        [ -n "$2" ] && [ -n "$line" ] && grep -Eq "$2" <<< "$line" && break
        read -r -t 0.3 -u 3 line || break             # multi-line replies (state?, version?)
    done
    printf '%s' "$reply" | pl_fmt
}

pl_fmt() { LC_ALL=C sed -e $'s/\xb0/deg /' -e 's/:[[:space:]]*/: /' -e 's/[[:space:]]*$//' ; }

# set command: device answers "done" on success; anything else (e.g. "invalid command!") is an error
pl_set() {   # CMD
    [ -n "$IOC" ] && { ioc_set "$1" ; return ; }
    local reply
    reply=$(pl_cmd "$1") || exit 1
    [ "$reply" == "done" ] || die "$1 -> $reply"
}

# ---- through the EPICS IOC ----
# The IOC keeps the replies of state? version? ldtec? ldtemp? as text and the single values as
# numbers; ioc_query gives them back as the controller's own lines (a reading INVALID, or older
# than three of its scans, is none). Emission on, and load, need the PILAS:BOX_SAFE arm, set here
# just before.
EPICS_BIN=${EPICS_BIN:-$HOME/epics/base/bin/linux-x86_64}
IOC_CA=${PILAS_IOC_CA:-127.0.0.1:5072}   # its CA server, loopback only
IOC_FAST_MAX=6                           # s: PILAS:LD, scanned every 2 s
IOC_SLOW_MAX=30                          # s: everything else, every 10 s (VERSION_TXT: read at start)
IOC=""                                   # 1 = through the IOC

ca() {   # caget|caput|cainfo ARGS..., asking only the IOC
    EPICS_CA_AUTO_ADDR_LIST=NO EPICS_CA_ADDR_LIST=$IOC_CA "$EPICS_BIN/$1" "${@:2}"
}

ioc_up() {   # a refused TCP connect fails at once, so this costs nothing while the IOC is stopped
    local h=${IOC_CA%:*} p=${IOC_CA##*:}
    [ -x "$EPICS_BIN/caget" ] && timeout 1 bash -c "exec 3<>/dev/tcp/$h/$p" 2> /dev/null &&
        ca caget -w 2 -t PILAS:LD > /dev/null 2>&1
}

ioc_get() {   # PV...: their values, one per line; dies if one is INVALID or too old
    local out
    out=$(ca caget -a -S -F '|' -w 2 "$@" 2> /dev/null) || die "no answer from the PiLas IOC ($IOC_CA)"
    awk -F'|' -v now=$(date +%s) -v fast=$IOC_FAST_MAX -v slow=$IOC_SLOW_MAX '
        { split($2, d, /[-: ]/) ; age = now - mktime(d[1] " " d[2] " " d[3] " " d[4] " " d[5] " " int(d[6]))
          if ($5 == "INVALID") { print "pilas: no reading: " $1 " INVALID (" $4 ")" > "/dev/stderr" ; bad = 1 }
          else if (($1 == "PILAS:LD" && age > fast) || ($1 != "PILAS:VERSION_TXT" && age > slow)) {
              print "pilas: no reading: " $1 " is " age " s old" > "/dev/stderr" ; bad = 1 }
          print $3 }
        END { exit bad }' <<< "$out" || exit 1
}

ioc_query() {   # CMD: the reply lines as the controller sends them (before pl_fmt)
    local v
    case $1 in
        'state?')   v=STATE_TXT ;;
        'version?') v=VERSION_TXT ;;
        'ldtec?')   v=LDTEC_TXT ;;
        'ldtemp?')  v=LDTEMP_TXT ;;
    esac
    if [ -n "$v" ] ; then
        v=$(ioc_get PILAS:$v) || exit 1
        printf '%b\n' "$v" | tr -d '\r' | grep -av '^$'    # caget -S escapes CR, LF, tab and the degree byte
        return
    fi
    case $1 in
        'ld?'|'f?'|'ts?'|'te?'|'tl?'|'tm?'|'tune?'|'lht?') v=$(ioc_get "PILAS:$(tr a-z A-Z <<< "${1%\?}")") || exit 1 ;;
        *) die "the IOC does not keep the reply to '$1' (it has state? version? ldtec? ldtemp? ld? f? ts? te? tl? tm? tune? lht?); for others stop it and use PILAS_BACKEND=serial" ;;
    esac
    case $1 in
        'ld?')   echo "pulsed laser emission: $v" ;;
        'f?')    echo "int. frequency: $v Hz" ;;
        'ts?')   echo "trigger source: $v" ;;
        'te?')   echo "trigger edge: $v" ;;
        'tm?')   echo "tune mode: $v" ;;
        'tl?')   printf 'trigger level: %+.2f V\n' "$v" ;;
        'tune?') printf 'tune value: %.2f %%\n' "$v" ;;
        'lht?')  printf 'laser head temp.: %.2f \xb0C\n' "$v" ;;
    esac
}

# caput -c (returns once the IOC has written and read everything back), failing if the IOC refused
# it or the controller did not answer "done" (the record is then INVALID and the reply in LAST_ERR)
ioc_put() {   # PV VALUE [WHAT]
    local out
    out=$(ca caput -c -w 15 "$1" "$2" 2>&1) && [[ $out != *[Ee]rror* && $out != *denied* ]] ||
        die "caput $1 $2: $(grep -m 1 -i 'error\|denied\|timed out' <<< "$out" || tail -n 1 <<< "$out")"
    [ "$(ca caget -t -w 2 "$1.SEVR" 2> /dev/null)" != INVALID ] ||
        die "${3:-$1 $2} -> $(ca caget -t -w 2 PILAS:LAST_ERR 2> /dev/null | sed "s/ *$//")"
}

ioc_armed_put() {   # PV VALUE: with PILAS:BOX_SAFE armed for it (the IOC's access security)
    local i rc
    ca caput -t -w 2 PILAS:BOX_SAFE 1 > /dev/null 2>&1 || die "cannot arm PILAS:BOX_SAFE"
    for (( i=0 ; i<30 ; i++ )) ; do
        ca cainfo -w 2 "$1" 2> /dev/null | grep -q 'Access: *read, write' && break
        sleep 0.1
    done
    ( ioc_put "$1" "$2" ) ; rc=$?
    ca caput -t -w 2 PILAS:BOX_SAFE 0 > /dev/null 2>&1      # it also drops back after 10 s by itself
    [ $rc -eq 0 ] || exit 1
}

ioc_set() {   # "par=value", as pl_set
    local par=${1%%=*} val=${1#*=}
    case $par in
        ld) if [ "$val" == 1 ] ; then ioc_armed_put PILAS:LD_ON 1 ; else ioc_put PILAS:LD_OFF 1 "$1" ; fi ;;
        tune) ioc_put PILAS:TUNE_SP "$(awk -v v="$val" 'BEGIN { print v / 10 }')" "$1" ;;   # PV in %
        *)  ioc_put "PILAS:${par^^}_SP" "$val" "$1" ;;
    esac
}

status() {
    pl_cmd 'version?' '^laser head hardware version' | grep -Ev '^(software|controller hardware|laser head hardware) version' || exit 1
    pl_cmd 'state?'   '^trigger level' | grep -Ev '^(PiLas system state|-+)$'
    pl_cmd 'ldtec?'   .
    pl_cmd 'ldtemp?'  .
}

is_int() { [[ $1 =~ ^-?[0-9]+$ ]] ; }
in_range() {   # VAL MIN MAX
    is_int "$1" && (( $1 >= $2 && $1 <= $3 )) || die "value must be an integer $2 ... $3"
}

[ $# -lt 1 ] && usage
case $1 in -h|--help) usage ;; esac

case ${PILAS_BACKEND:-auto} in
    auto)   ioc_up && IOC=1 ;;
    ioc)    ioc_up || die "the PiLas IOC does not answer on $IOC_CA (PILAS_BACKEND=ioc)" ; IOC=1 ;;
    serial) ;;
    *)      die "PILAS_BACKEND must be auto, ioc or serial" ;;
esac
if [ -z "$IOC" ] ; then
[ -e "$PORT" ] || die "$PORT not found - is the PiLas controller plugged in?"
# other pilas.sh instances (e.g. under watch) are queued on the flock below; anything else is fatal
others=""
is_pilas() {   # PID, or any of its ancestors, is a pilas.sh (children like sed inherit the port fd)
    local p=$1
    while [ -n "$p" ] && [ "$p" -gt 1 ] ; do
        grep -qs pilas.sh /proc/$p/cmdline && return 0
        p=$(awk '/^PPid:/ {print $2}' /proc/$p/status 2>/dev/null)
    done
    return 1
}
for p in $(fuser "$PORT" 2>/dev/null) ; do
    p=${p//[^0-9]/}
    is_pilas $p || others+=" $p"
done
[ -n "$others" ] && die "$PORT is in use by:$others (PiLas GUI?)"
exec 3<>"$PORT" || die "cannot open $PORT"
flock -w 10 3 || die "$PORT still locked by another pilas.sh after 10 s"
stty -F "$PORT" 19200 cs8 -parenb -cstopb -ixon -ixoff -crtscts clocal cread raw -echo || die "cannot configure $PORT"
fi

OP=$1 ; shift
case $OP in
    status)
        status ;;
    query)
        [ $# -eq 1 ] || usage
        [[ $1 == *\? ]] || die "query commands end in '?' (use set/on/off/store/save/load to change things)"
        pl_cmd "$1" ;;
    set)
        [ $# -eq 2 ] || usage
        PAR=${1,,} ; VAL=$2
        case $PAR in
            f)    in_range "$VAL" 25 125000000 ;;
            ts)   in_range "$VAL" 0 2 ;;
            te|tm) in_range "$VAL" 0 1 ;;
            tl)   in_range "$VAL" -4800 4800 ;;
            tune) [[ $VAL =~ ^[0-9]+(\.[0-9])?$ ]] && awk -v v="$VAL" 'BEGIN { exit !(v <= 100) }' \
                      || die "tune value must be 0 ... 100 % (0.1 % steps)"
                  VAL=$(awk -v v="$VAL" 'BEGIN { printf "%d", v*10 + 0.5 }') ;;   # device takes 0 ... 1000
            *)    die "can only set: f ts te tl tm tune (use on/off for emission)" ;;
        esac
        pl_set "$PAR=$VAL"
        pl_cmd "$PAR?" ;;
    off)
        [ $# -eq 0 ] || usage
        pl_set 'ld=0'
        pl_cmd 'ld?' ;;
    on)
        [ "$1" == "--box-safe" ] || die "refusing to enable emission without --box-safe (the dark box is closed and the detector is safe to light)"
        pl_set 'ld=1'
        pl_cmd 'ld?' ;;
    store)
        [ $# -eq 0 ] || usage
        if [ -n "$IOC" ] ; then ioc_put PILAS:STORE 1 store && echo "store sent through the IOC (it does not keep the reply)"
        else pl_cmd 'store' ; fi ;;
    save|load)
        [ $# -ge 1 ] && [[ $1 == [123] ]] || die "memory position must be 1, 2 or 3"
        [ $# -eq 1 ] || { [ $# -eq 2 ] && [ $OP == load ] && [ "$2" == --box-safe ] ; } || usage
        if [ -z "$IOC" ] ; then
            pl_cmd "$OP$1"
        elif [ $OP == save ] ; then
            ioc_put PILAS:SAVE $1 save$1 && echo "save$1 sent through the IOC (it does not keep the reply)"
        else
            [ "$2" == --box-safe ] || die "load through the IOC needs --box-safe (a stored memory might hold emission on)"
            ioc_armed_put PILAS:LOAD $1
        fi
        [ $OP == load ] && status ;;
    *)
        usage ;;
esac
