#! /bin/bash
#
# Command-line access to the Keithley 6487 picoammeter / voltage source over RS-232
# (Prolific USB adapter; 9600 8N1, CR terminator, XON/XOFF).
#
# Readback:
#   keithley6487.sh status        identity, voltage source state, zero check/correct, one current reading
#   keithley6487.sh read [N]      current reading; with N > 1 prints mean and std. dev. of N readings
#   keithley6487.sh query CMD     raw SCPI query, e.g. "query SOUR:VOLT?", "query SYST:ERR?"
#   keithley6487.sh clear         serial break = device clear, if it stops answering (settings are kept)
# Ammeter:
#   keithley6487.sh zero          zero correct (2 nA range, zero check on), then auto range; zero check stays ON
#   keithley6487.sh zcheck on|off zero check (input shunted) - turn on before changing connections
#   keithley6487.sh set irange R  ammeter range: auto, or 2e-9 2e-8 2e-7 2e-6 2e-5 2e-4 2e-3 2e-2 [A]
# Voltage source (setpoint is kept while off; changes while on are ramped at RAMP_STEP / RAMP_DELAY):
#   keithley6487.sh set volt V    output voltage [V], |V| <= VMAX and <= range
#   keithley6487.sh set range R   source range 10, 50 or 500 [V] (only while off)
#   keithley6487.sh set ilim L    current limit 25u, 250u, 2.5m or 25m [A] (25m only on the 10 V range)
#   keithley6487.sh off [--now]   ramp to 0 V and turn the source off (--now: no ramp)
#   keithley6487.sh on --box-safe
#                                 turn the source on at 0 V and ramp to the setpoint; refuses without the flag
#
# The 50 V and 500 V ranges need the INTERLOCK connector closed. Ctrl-C during a ramp stops at the present
# voltage. Remote commands lock the front panel except OPER, which still turns the source off; LOCAL unlocks.

PORT=${K6487_PORT:-/dev/serial/by-id/usb-Prolific_Technology_Inc._USB-Serial_Controller_DGBCb2A9709-if00-port0}   # -> /dev/ttyUSB1
BAUD=9600          # must match CONFIG -> COMM on the 6487 front panel
EOL=$'\r'          # terminator, also set under CONFIG -> COMM (CR; use $'\n' for LF)
VMAX=250           # refuse setpoints above |VMAX| V - raise deliberately for other devices
RAMP_STEP=5        # V per step
RAMP_DELAY=0.5     # s between steps (5 V / 0.5 s = 10 V/s)

usage() { sed -n '3,24p' "$0" | sed 's/^# \{0,1\}//' ; exit 2 ; }
# die also works inside $(...): it signals the main script, so a failed query never shows up as a value
die()   { echo "keithley6487: $*" >&2 ; [ "$BASHPID" != "$MAINPID" ] && kill -USR1 $MAINPID ; exit 1 ; }
MAINPID=$$
trap 'exit 1' USR1

k_write() { printf '%s%s' "$1" "$EOL" >&3 ; }

# send one query, print the reply; fail on no reply
k_query() {   # CMD
    local reply junk
    while read -r -t 0.05 -u 3 junk ; do : ; done     # drop stale input
    k_write "$1"
    read -r -t 10 -u 3 reply || die "no reply to $1 (RS-232 selected, baud $BAUD, terminator?)"
    reply=${reply//$'\r'/} ; reply=${reply//$'\n'/}
    echo "$reply"
}

# commands get no reply: check the error queue afterwards
k_cmd() {   # CMD
    local err
    k_write "$1"
    err=$(k_query 'SYST:ERR?') || exit 1
    [[ $err == 0,* || $err == +0,* ]] || die "$1 -> $err"
}

is_num() { [[ $1 =~ ^[-+]?[0-9]+(\.[0-9]*)?([eE][-+]?[0-9]+)?$ ]] ; }
fcmp()   { awk -v a="$1" -v b="$3" "BEGIN { exit !(a $2 b) }" ; }   # fcmp A OP B, e.g. fcmp -200 '<' 0
fabs()   { awk -v a="$1" 'BEGIN { print (a < 0 ? -a : a) }' ; }
fmt_v()  { awk -v a="$1" 'BEGIN { printf "%.3f", a }' ; }
on_off() { [[ $1 == 1 ]] && echo on || echo off ; }

# step the output from its present value to TARGET; Ctrl-C stops at the present step
ramp_to() {   # TARGET
    local v cur abort=0
    cur=$(k_query 'SOUR:VOLT?') || exit 1
    trap 'abort=1' INT TERM
    for v in $(awk -v a="$cur" -v b="$1" -v s=$RAMP_STEP 'BEGIN {
            d = (b > a) ? s : -s
            for (v = a + d ; (d > 0 && v < b) || (d < 0 && v > b) ; v += d) printf "%.3f\n", v
            printf "%.3f\n", b }') ; do
        (( abort )) && break
        k_cmd "SOUR:VOLT $v"
        printf '  %10s V\n' "$v"
        sleep $RAMP_DELAY
    done
    trap - INT TERM
    (( abort )) && die "ramp interrupted, output now at $(fmt_v $(k_query 'SOUR:VOLT?')) V"
}

check_setpoint() {   # VOLTS
    is_num "$1" || die "voltage must be a number"
    fcmp "$(fabs $1)" '<=' $VMAX || die "|$1| V > VMAX = $VMAX V (edit VMAX in $0 if intended)"
    local rng=$(k_query 'SOUR:VOLT:RANG?')
    fcmp "$(fabs $1)" '<=' "$rng" || die "|$1| V is outside the $(fmt_v $rng) V source range (set range, while off)"
}

check_interlock() {
    local rng=$(k_query 'SOUR:VOLT:RANG?')
    fcmp "$rng" '>' 10 && [[ $(k_query 'SOUR:VOLT:INT:FAIL?') == 1 ]] \
        && die "interlock open: the $(fmt_v $rng) V range needs the INTERLOCK connector closed"
    return 0
}

read_current() {   # N
    k_cmd 'FORM:ELEM READ'
    [[ $(k_query 'SYST:ZCH?') == 1 ]] && echo "warning: zero check is ON - reading is not the input current" >&2
    # after power-up the front panel measures continuously (ARM:COUN INF), where READ? is refused (error 830).
    # ABOR first: changing ARM:COUN while that runs locks up the parser. Restore INF afterwards, but no INIT
    # (INIT with INF arm count blocks the parser too); the front panel takes over again on SYST:LOC at exit.
    local i r all="" arm
    arm=$(k_query 'ARM:COUN?')
    fcmp "$arm" '>' 1e30 && { k_cmd 'ABOR' ; k_cmd 'ARM:COUN 1' ; }
    for (( i=0 ; i<$1 ; i++ )) ; do
        r=$(k_query 'READ?') || exit 1
        all+="$r "
    done
    fcmp "$arm" '>' 1e30 && k_cmd 'ARM:COUN INF'
    echo $all | awk '{
        for (i = 1 ; i <= NF ; i++) { if ($i + 0 > 9e37 || $i + 0 < -9e37) ovr = 1 ; s += $i ; ss += $i * $i }
        m = s / NF ; var = NF > 1 ? (ss - NF * m * m) / (NF - 1) : 0 ; sd = var > 0 ? sqrt(var) : 0
        if (ovr)          print "OVERFLOW (reading out of range)"
        else if (NF == 1) printf "I = %.6e A\n", m
        else              printf "I = %.6e A   std. dev. %.2e A   (N = %d)\n", m, sd, NF }'
}

status() {
    local id ; id=$(k_query '*IDN?')
    echo "id:              $id"
    echo "source:          $(on_off $(k_query 'SOUR:VOLT:STAT?'))"
    echo "source voltage:  $(fmt_v $(k_query 'SOUR:VOLT?')) V"
    echo "source range:    $(fmt_v $(k_query 'SOUR:VOLT:RANG?')) V"
    echo "current limit:   $(k_query 'SOUR:VOLT:ILIM?') A"
    echo "interlock open:  $(k_query 'SOUR:VOLT:INT:FAIL?')"
    echo "zero check:      $(on_off $(k_query 'SYST:ZCH?'))"
    echo "zero correct:    $(on_off $(k_query 'SYST:ZCOR?'))"
    echo "ammeter range:   $(k_query 'RANG?') A (auto $(on_off $(k_query 'RANG:AUTO?')))"
    read_current 1
}

[ $# -lt 1 ] && usage
case $1 in -h|--help) usage ;; esac

[ -e "$PORT" ] || die "$PORT not found - is the 6487 RS-232 adapter plugged in?"
# other keithley6487.sh instances (e.g. under watch) are queued on the flock below; anything else is fatal
others=""
is_k6487() {   # PID, or any of its ancestors, is a keithley6487.sh (children like awk inherit the port fd)
    local p=$1
    while [ -n "$p" ] && [ "$p" -gt 1 ] ; do
        grep -qs keithley6487.sh /proc/$p/cmdline && return 0
        p=$(awk '/^PPid:/ {print $2}' /proc/$p/status 2>/dev/null)
    done
    return 1
}
for p in $(fuser "$PORT" 2>/dev/null) ; do
    p=${p//[^0-9]/}
    is_k6487 $p || others+=" $p"
done
[ -n "$others" ] && die "$PORT is in use by:$others"
exec 3<>"$PORT" || die "cannot open $PORT"
flock -w 10 3 || die "$PORT still locked by another keithley6487.sh after 10 s"
# icrnl: the driver turns the CR terminator into LF, so plain "read" works (read -d $'\r' alters tty settings)
stty -F "$PORT" $BAUD cs8 -parenb -cstopb -crtscts clocal cread raw icrnl ixon ixoff -echo || die "cannot configure $PORT"
k_write '*CLS'      # clear stale errors so k_cmd only reports its own
trap "k_write 'SYST:LOC'" EXIT    # hand the front panel back after every command

OP=$1 ; shift
case $OP in
    clear)
        trap - EXIT
        python3 -I -c 'import os, sys, termios; termios.tcsendbreak(os.open(sys.argv[1], os.O_RDWR | os.O_NOCTTY), 0)' "$PORT"
        sleep 1
        echo "sent break (device clear); id: $(k_query '*IDN?')"
        k_write 'SYST:LOC' ;;
    status)
        status ;;
    read)
        N=${1:-1}
        [[ $N =~ ^[1-9][0-9]*$ ]] || die "N must be a positive integer"
        read_current $N ;;
    query)
        [ $# -eq 1 ] || usage
        [[ $1 == *\? ]] || die "query commands end in '?' (use set/on/off/zero/zcheck to change things)"
        k_query "$1" ;;
    zero)
        [ $# -eq 0 ] || usage
        for c in 'SYST:ZCH ON' 'RANG 2e-9' 'INIT' 'SYST:ZCOR:ACQ' 'SYST:ZCOR ON' 'RANG:AUTO ON' ; do k_cmd "$c" ; done
        echo "zero correct acquired; zero check is still ON ('zcheck off' to measure)" ;;
    zcheck)
        case ${1,,} in
            on|off) k_cmd "SYST:ZCH ${1^^}" ; echo "zero check: $(on_off $(k_query 'SYST:ZCH?'))" ;;
            *)      usage ;;
        esac ;;
    set)
        [ $# -eq 2 ] || usage
        PAR=${1,,} ; VAL=$2
        case $PAR in
            irange)
                if [[ ${VAL,,} == auto ]] ; then k_cmd 'RANG:AUTO ON'
                else
                    [[ " 2e-9 2e-8 2e-7 2e-6 2e-5 2e-4 2e-3 2e-2 " == *" $VAL "* ]] || die "irange: auto or 2e-9 ... 2e-2"
                    k_cmd "RANG $VAL"
                fi
                echo "ammeter range: $(k_query 'RANG?') A (auto $(on_off $(k_query 'RANG:AUTO?')))" ;;
            volt)
                check_setpoint "$VAL"
                if [[ $(k_query 'SOUR:VOLT:STAT?') == 1 ]] ; then ramp_to "$VAL" ; else k_cmd "SOUR:VOLT $VAL" ; fi
                echo "source voltage: $(fmt_v $(k_query 'SOUR:VOLT?')) V ($(on_off $(k_query 'SOUR:VOLT:STAT?')))" ;;
            range)
                [[ " 10 50 500 " == *" $VAL "* ]] || die "range must be 10, 50 or 500"
                [[ $(k_query 'SOUR:VOLT:STAT?') == 1 ]] && die "turn the source off before changing range"
                k_cmd "SOUR:VOLT:RANG $VAL"
                echo "source range: $(fmt_v $(k_query 'SOUR:VOLT:RANG?')) V" ;;
            ilim)
                case ${VAL,,} in
                    25u|2.5e-5)   VAL=2.5e-5 ;;
                    250u|2.5e-4)  VAL=2.5e-4 ;;
                    2.5m|2.5e-3)  VAL=2.5e-3 ;;
                    25m|2.5e-2)   VAL=2.5e-2 ;;
                    *)            die "ilim must be 25u, 250u, 2.5m or 25m" ;;
                esac
                k_cmd "SOUR:VOLT:ILIM $VAL"
                echo "current limit: $(k_query 'SOUR:VOLT:ILIM?') A" ;;
            *)
                die "can only set: volt range ilim irange (use on/off for the source)" ;;
        esac ;;
    off)
        if [[ $(k_query 'SOUR:VOLT:STAT?') == 1 ]] ; then
            SETPOINT=$(k_query 'SOUR:VOLT?')
            [ "$1" == "--now" ] || ramp_to 0
            k_cmd 'SOUR:VOLT:STAT OFF'
            k_cmd "SOUR:VOLT $SETPOINT"      # keep the setpoint for the next "on"
        fi
        echo "source: off (setpoint $(fmt_v $(k_query 'SOUR:VOLT?')) V)" ;;
    on)
        [ "$1" == "--box-safe" ] || die "refusing to turn the source on without --box-safe (the dark box is closed and the detector is safe to power)"
        [[ $(k_query 'SOUR:VOLT:STAT?') == 1 ]] && die "source is already on (use set volt to change it)"
        SETPOINT=$(k_query 'SOUR:VOLT?')
        check_setpoint "$SETPOINT"
        check_interlock
        k_cmd 'SOUR:VOLT 0'
        k_cmd 'SOUR:VOLT:STAT ON'
        ramp_to "$SETPOINT"
        echo "source: on at $(fmt_v $(k_query 'SOUR:VOLT?')) V" ;;
    *)
        usage ;;
esac
