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
#   pilas.sh store | save N | load N
#                                 store settings as boot default / save to / load from memory N=1,2,3
#
# Only one program can use the port at a time: close the PiLas GUI first.

PORT=/dev/serial/by-id/usb-FTDI_FT232R_USB_UART_BG01VUCO-if00-port0   # -> /dev/ttyUSB2

usage() { sed -n '3,22p' "$0" | sed 's/^# \{0,1\}//' ; exit 2 ; }
die()   { echo "pilas: $*" >&2 ; exit 1 ; }

# send one command, print the reply lines (tabs/padding squeezed to "key: value"); fail on no reply
pl_cmd() {   # CMD
    local line reply="" junk
    while read -r -t 0.05 -u 3 junk ; do : ; done     # drop stale input
    printf '%s\r' "$1" >&3
    read -r -t 2 -u 3 line || die "no reply to $1"
    while : ; do
        line=${line%$'\r'}
        [ -n "$line" ] && reply+="$line"$'\n'
        read -r -t 0.3 -u 3 line || break             # multi-line replies (state?, version?)
    done
    printf '%s' "$reply" | LC_ALL=C sed -e $'s/\xb0/deg /' -e 's/:[[:space:]]*/: /' -e 's/[[:space:]]*$//'
}

# set command: device answers "done" on success; anything else (e.g. "invalid command!") is an error
pl_set() {   # CMD
    local reply
    reply=$(pl_cmd "$1") || exit 1
    [ "$reply" == "done" ] || die "$1 -> $reply"
}

status() {
    pl_cmd 'version?' | grep -Ev '^(software|controller hardware|laser head hardware) version' || exit 1
    pl_cmd 'state?'   | grep -Ev '^(PiLas system state|-+)$'
    pl_cmd 'ldtec?'
    pl_cmd 'ldtemp?'
}

is_int() { [[ $1 =~ ^-?[0-9]+$ ]] ; }
in_range() {   # VAL MIN MAX
    is_int "$1" && (( $1 >= $2 && $1 <= $3 )) || die "value must be an integer $2 ... $3"
}

[ $# -lt 1 ] && usage
case $1 in -h|--help) usage ;; esac

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
        pl_cmd 'store' ;;
    save|load)
        [ $# -eq 1 ] && [[ $1 == [123] ]] || die "memory position must be 1, 2 or 3"
        pl_cmd "$OP$1"
        [ $OP == load ] && status ;;
    *)
        usage ;;
esac
