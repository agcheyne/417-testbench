#! /bin/bash
#
# JY H20 UV monochromator through its ISA controller on GPIB (NI GPIB-USB-HS, linux-gpib).
# Wavelengths in nm; the controller counts motor steps (MONO_STEPS_PER_NM in db/testbench.conf).
#
#   monochromator.sh pos               wavelength [nm]
#   monochromator.sh status            controller state, busy, wavelength and steps
#   monochromator.sh start             start the controller's main program if it is in boot mode
#                                      (it is after every power-on, and has then forgotten the
#                                      position: use set next)
#   monochromator.sh set NM            tell the controller where the grating is now (read it off the
#                                      monochromator's own counter); nothing moves
#   monochromator.sh move NM           move there and wait. Downward moves overshoot by MONO_BACKLASH
#                                      and come back up. Ctrl-C stops the motor
#   monochromator.sh dark              park at MONO_DARK (below MONO_MIN), where no light gets through:
#                                      a dark without capping the detector. move NM comes back up
#   monochromator.sh stop              stop the motor (also from another terminal)
#   monochromator.sh speed             motor speed: start and top speed [steps/s], ramp [ms]
#   monochromator.sh speed MIN MAX MS  set it until the next move or dark, which put MONO_SPEED back
#   monochromator.sh raw CMD [CMD ..]  send ISA commands as they are and print each reply; \r = CR,
#                                      \x00 = NUL, e.g. raw ' ' E 'H0\r'. No limits are checked
#
# Limits, overshoot, dark position, speed and timeout: MONO_MIN / MONO_MAX / MONO_BACKLASH /
# MONO_DARK / MONO_SPEED / MONO_MOVE_TIMEOUT in db/testbench.conf. Moves are refused while the
# position reads 0 (not set since power-on).

. "$(dirname "$(readlink -f "$0")")/../lib/tb.sh"
tb_site

usage() { sed -n '3,25p' "$0" | sed 's/^# \{0,1\}//' ; exit 2 ; }

# send each command, print each reply on its own line without the CR; exit 1 on a GPIB error
mono_io() {
    python3 -I - "$MONO_GPIB_BOARD" "$MONO_GPIB_ADDR" "$@" <<'EOF'
import ctypes, sys

board, pad = int(sys.argv[1]), int(sys.argv[2])
lib = ctypes.CDLL("libgpib.so.0")
for f in ("ThreadIbsta", "ThreadIberr", "ThreadIbcnt"):
    getattr(lib, f).restype = ctypes.c_int
T3S, REOS, ERR = 12, 0x400, 0x8000
HINT = {2: "nothing listening at that address: controller switched off?", 6: "timed out"}

def fail(what):
    e = lib.ThreadIberr()
    sys.exit(f"ERROR: {what} (GPIB board {board}, address {pad}, iberr {e}: {HINT.get(e, 'see linux-gpib ibvers/iberr')})")

ud = lib.ibdev(board, pad, 0, T3S, 1, REOS | 0x0D)    # replies end in CR
if ud < 0:
    fail("cannot open the controller")
try:
    for arg in sys.argv[3:]:
        cmd = arg.encode().decode("unicode_escape").encode("latin-1")
        lib.ibwrt(ud, cmd, len(cmd))
        if lib.ThreadIbsta() & ERR:
            fail(f"sending {arg!r} failed")
        buf = ctypes.create_string_buffer(256)
        lib.ibrd(ud, buf, 255)
        if lib.ThreadIbsta() & ERR:
            fail(f"no reply to {arg!r}")
        print(buf.raw[:lib.ThreadIbcnt()].decode("latin-1").rstrip("\r\n"))
finally:
    lib.ibonl(ud, 0)
EOF
}

to_steps() { awk -v n="$1" -v s=$MONO_STEPS_PER_NM 'BEGIN { printf "%.0f", n * s }' ; }
to_nm()    { awk -v n="$1" -v s=$MONO_STEPS_PER_NM 'BEGIN { printf "%.2f", n / s }' ; }

state() {   # -> F/f main program, B boot program, * autobauded (RS-232)
    mono_io ' '
}

need_main() {
    local s
    s=$(state) || exit 1
    case $s in
        F|f) ;;
        B)   tb_die "the controller is in boot mode (switched off and on?): monochromator.sh start, then set NM" ;;
        *)   tb_die "unexpected controller state '$s' (expected F or f)" ;;
    esac
}

read_steps() {
    local r
    r=$(mono_io 'H0\r') || exit 1
    [[ $r =~ ^o(-?[0-9]+)$ ]] || tb_die "unexpected reply to H0: '$r'"
    echo ${BASH_REMATCH[1]}
}

check_nm() {   # NM
    [[ $1 =~ ^[0-9]+(\.[0-9]*)?$ ]] || tb_die "'$1' is not a wavelength in nm"
    tb_fcmp "$1" '>=' $MONO_MIN && tb_fcmp "$1" '<=' $MONO_MAX ||
        tb_die "$1 nm is outside $MONO_MIN-$MONO_MAX nm (db/testbench.conf)"
}

wait_idle() {
    local r start=$SECONDS
    while : ; do
        r=$(mono_io E) || exit 1
        case $r in
            oz) return 0 ;;
            oq) ;;
            *)  tb_die "unexpected reply to E: '$r'" ;;
        esac
        if (( SECONDS - start > MONO_MOVE_TIMEOUT )) ; then
            mono_io L > /dev/null
            tb_die "still moving after $MONO_MOVE_TIMEOUT s: stop sent"
        fi
        sleep 0.25
    done
}

move_rel() {   # STEPS
    local r
    [ "$1" -eq 0 ] && return 0
    r=$(mono_io "F0,$1\\r") || exit 1
    [ "$r" == o ] || tb_die "move of $1 steps refused: reply '$r'"
    wait_idle
}

read_speed() {   # -> MIN,MAX,RAMP
    local r
    r=$(mono_io 'C0\r') || exit 1
    [[ $r =~ ^o([0-9]+,[0-9]+,[0-9]+)$ ]] || tb_die "unexpected reply to C0: '$r'"
    echo ${BASH_REMATCH[1]}
}

set_speed() {   # MIN,MAX,RAMP
    local r
    [[ $1 =~ ^([0-9]+),([0-9]+),([0-9]+)$ ]] && (( BASH_REMATCH[1] > 0 && BASH_REMATCH[1] <= BASH_REMATCH[2] )) ||
        tb_die "speed '$1' is not MIN,MAX,RAMP with 0 < MIN <= MAX [steps/s] and RAMP [ms]"
    r=$(mono_io "B0,$1\\r") || exit 1
    [ "$r" == o ] || tb_die "speed $1 refused: reply '$r'"
    [ "$(read_speed)" == "$1" ] || tb_die "the speed did not change to $1"
}

use_speed() {   # put MONO_SPEED back if it was changed (or lost at power-off)
    local cur
    cur=$(read_speed) || exit 1
    [ "$cur" == "$MONO_SPEED" ] && return 0
    set_speed "$MONO_SPEED"
    tb_log "speed set to $MONO_SPEED (MONO_SPEED; it was $cur)"
}

lock() {   # SECONDS to wait for another monochromator.sh
    mkdir -p "$TB_STATE"
    exec 9> "$TB_STATE/monochromator.lock"
    flock -w "$1" 9 || tb_die "monochromator.sh is running in another terminal (moving?)"
}

[ -e /dev/gpib$MONO_GPIB_BOARD ] ||
    tb_die "no /dev/gpib$MONO_GPIB_BOARD: linux-gpib not loaded, or the GPIB-USB-HS is not plugged in (lsusb -d 3923:709b)"

case $1 in
    pos)
        lock 5
        need_main
        steps=$(read_steps) || exit 1
        to_nm $steps ; echo ;;
    status)
        lock 5
        s=$(state) || exit 1
        case $s in
            F|f) printf 'controller  main program (%s)\n' "$s"
                 b=$(mono_io E) || exit 1
                 case $b in oz) b="not busy" ;; oq) b=moving ;; esac
                 printf 'motor       %s\n' "$b"
                 steps=$(read_steps) || exit 1
                 printf 'position    %s nm (%s steps)%s\n' "$(to_nm $steps)" $steps \
                     "$( [ $steps -eq 0 ] && echo ', not set since power-on: monochromator.sh set NM')" ;;
            B)   echo "controller  boot mode (after power-on): monochromator.sh start, then set NM" ;;
            *)   echo "controller  state '$s'" ;;
        esac ;;
    start)
        lock 5
        s=$(state) || exit 1
        case $s in
            F|f) steps=$(read_steps) || exit 1
                 echo "the main program is already running (position $(to_nm $steps) nm)" ; exit 0 ;;
            B|'*'|=) ;;
            *)   tb_die "unexpected controller state '$s'" ;;
        esac
        r=$(mono_io 'O2000\x00') || exit 1
        [ "$r" == '*' ] || tb_die "O2000 was answered with '$r' instead of '*'"
        need_main
        tb_log "started the controller's main program"
        echo "main program started. The position is not known: read the wavelength off the monochromator, then monochromator.sh set NM" ;;
    set)
        [ $# -eq 2 ] || usage
        [[ $2 =~ ^[0-9]+(\.[0-9]*)?$ ]] && tb_fcmp "$2" '>' 0 || tb_die "'$2' is not a wavelength in nm"
        lock 5
        need_main
        old=$(read_steps) || exit 1
        new=$(to_steps "$2")
        r=$(mono_io "G0,$new\\r") || exit 1
        [ "$r" == o ] || tb_die "set position refused: reply '$r'"
        [ "$(read_steps)" == "$new" ] || tb_die "the position did not change to $new steps"
        tb_log "position set to $(to_nm $new) nm ($new steps; it read $old)"
        echo "position set to $(to_nm $new) nm" ;;
    move)
        [ $# -eq 2 ] || usage
        check_nm "$2"
        lock 5
        need_main
        cur=$(read_steps) || exit 1
        [ "$cur" -ne 0 ] || tb_die "the position reads 0, i.e. it was not set since power-on: monochromator.sh set NM"
        use_speed
        tgt=$(to_steps "$2")
        trap 'mono_io L > /dev/null ; tb_die "interrupted: motor stopped at $(to_nm "$(read_steps)") nm"' INT
        if [ $tgt -lt $cur ] ; then
            low=$(( tgt - $(to_steps $MONO_BACKLASH) ))
            min=$(to_steps $MONO_MIN)
            [ $low -lt $min ] && low=$min
            move_rel $(( low - cur ))
            cur=$(read_steps) || exit 1
        fi
        move_rel $(( tgt - cur ))
        trap - INT
        end=$(read_steps) || exit 1
        [ "$end" == "$tgt" ] || tb_die "ended at $(to_nm $end) nm instead of $(to_nm $tgt) nm"
        tb_log "moved to $(to_nm $end) nm"
        echo "at $(to_nm $end) nm" ;;
    dark)
        [ $# -eq 1 ] || usage
        lock 5
        need_main
        cur=$(read_steps) || exit 1
        [ "$cur" -ne 0 ] || tb_die "the position reads 0, i.e. it was not set since power-on: monochromator.sh set NM"
        use_speed
        tgt=$(to_steps $MONO_DARK)
        trap 'mono_io L > /dev/null ; tb_die "interrupted: motor stopped at $(to_nm "$(read_steps)") nm"' INT
        move_rel $(( tgt - cur ))     # no backlash: the dark position need not be exact
        trap - INT
        end=$(read_steps) || exit 1
        [ "$end" == "$tgt" ] || tb_die "ended at $(to_nm $end) nm instead of $(to_nm $tgt) nm"
        echo "at $(to_nm $end) nm (dark)" ;;
    speed)
        [ $# -eq 1 ] || [ $# -eq 4 ] || usage
        lock 5
        need_main
        if [ $# -eq 4 ] ; then
            set_speed "$2,$3,$4"
            tb_log "speed set to $2,$3,$4 by hand"
        fi
        IFS=, read -r lo hi ramp <<< "$(read_speed)"
        echo "start $lo steps/s, top $hi steps/s ($(to_nm $hi) nm/s), ramp $ramp ms (MONO_SPEED $MONO_SPEED)" ;;
    stop)
        r=$(mono_io L) || exit 1
        echo "stop sent (reply '$r')" ;;
    raw)
        [ $# -ge 2 ] || usage
        shift
        lock 5
        mono_io "$@" ;;
    *)
        usage ;;
esac
