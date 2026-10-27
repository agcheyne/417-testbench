#! /bin/bash
#
# Thorlabs APT XY stage through EPICS (THOR_X / THOR_Y, mm). Needs the IOC and thor_epics
# running (see the cheat sheet, "XY stage").
#
#   motor.sh pos                       X and Y position [mm]
#   motor.sh status                    position + decoded status bits of both axes
#   motor.sh move X Y                  move X, then Y, and wait until both have arrived;
#                                      "-" leaves an axis where it is. Exit 1 on timeout/limit.
#   motor.sh stop                      stop both axes
#   motor.sh home x|y|both --yes       drive to the home switch (position 0)
#
# Limits, tolerance and timeout are X_MIN.. / MOVE_TOL / MOVE_TIMEOUT in db/testbench.conf.
# Targets are rounded to 0.01 mm (what thor_epics passes on). thor_epics approaches every
# position from below: a move to a smaller value overshoots by 0.5 mm and comes back.

. "$(dirname "$(readlink -f "$0")")/../lib/tb.sh"
tb_site

usage() { sed -n '3,16p' "$0" | sed 's/^# \{0,1\}//' ; exit 2 ; }

MOVING_BITS=$((0x10 | 0x20 | 0x40 | 0x80 | 0x200))   # moving fwd/rev, jogging fwd/rev, homing
LIMIT_BITS=$((0x1 | 0x2))                             # hardware limit switch fwd/rev

pv_get() {   # PV -> value, or die
    local v
    v=$(caget -t -w 3 -g 10 "$1" 2> /dev/null) && [ -n "$v" ] ||
        tb_die "no answer from EPICS PV $1 (IOC running? see the cheat sheet, XY stage)"
    echo "$v"
}
pv_put() { caput -t -w 3 "$1" "$2" > /dev/null 2>&1 || tb_die "caput $1 $2 failed" ; }

decode() {   # STATUS -> words
    local s=$1 out=""
    (( s & 0x1 ))   && out+=" LIMIT-fwd"
    (( s & 0x2 ))   && out+=" LIMIT-rev"
    (( s & 0x30 ))  && out+=" moving"
    (( s & 0xC0 ))  && out+=" jogging"
    (( s & 0x200 )) && out+=" homing"
    (( s & 0x400 )) && out+=" homed"
    (( s & 0x4000 )) && out+=" motion-error"
    (( s & 0x80000000 )) && out+=" enabled"
    echo "${out# }"
}

check_target() {   # AXIS VALUE
    local lo hi
    [ $1 == X ] && lo=$X_MIN hi=$X_MAX || { lo=$Y_MIN ; hi=$Y_MAX ; }
    [[ $2 =~ ^[0-9]+(\.[0-9]*)?$ ]] || tb_die "$1 target '$2' is not a number"
    tb_fcmp "$2" '>=' $lo && tb_fcmp "$2" '<=' $hi || tb_die "$1 = $2 mm is outside $lo-$hi mm (db/testbench.conf)"
}

move_axis() {   # AXIS TARGET
    local ax=$1 target pos stat start last moved=0 ok=0 cmd
    target=$(printf '%.2f' "$2")
    check_target $ax "$target"
    pos=$(pv_get THOR_$ax:read_pos)
    tb_fcmp "$(tb_calc "($pos) - ($target)")" '<=' $MOVE_TOL && tb_fcmp "$(tb_calc "($target) - ($pos)")" '<=' $MOVE_TOL && return 0
    # thor_epics only acts when move_abs changes: if it already holds this target (the
    # stage was moved some other way since), first "move" to where the stage is now
    cmd=$(pv_get THOR_$ax:move_abs)
    if tb_fcmp "$cmd" == "$target" ; then
        pv_put THOR_$ax:move_abs "$(printf '%.2f' "$pos")"
        sleep 1
    fi
    pv_put THOR_$ax:move_abs "$target"
    start=$(date +%s) last=$pos
    while sleep 0.25 ; do
        pos=$(pv_get THOR_$ax:read_pos) stat=$(pv_get THOR_$ax:read_stat)
        tb_fcmp "$pos" != "$last" && moved=1
        last=$pos
        if (( stat & LIMIT_BITS )) ; then
            tb_die "$ax hit a limit switch at $pos mm ($(decode $stat))"
        fi
        if (( !(stat & MOVING_BITS) )) && tb_fcmp "$(tb_calc "($pos) - ($target)")" '<=' $MOVE_TOL &&
                tb_fcmp "$(tb_calc "($target) - ($pos)")" '<=' $MOVE_TOL ; then
            ok=$((ok + 1))
            [ $ok -ge 2 ] && return 0          # two readings in a row: settled
        else
            ok=0
        fi
        local t=$(( $(date +%s) - start ))
        if [ $moved == 0 ] && [ $t -ge 10 ] ; then
            pgrep -x thor_epics > /dev/null || tb_die "$ax did not start moving: thor_epics is not running (cd $THOR_DIR && ./thor_epics -e)"
            if [ -e "$THOR_DIR/busy" ] && [ -e "$THOR_DIR/todo" ] ; then
                tb_die "$ax did not start moving: thor_epics is stuck on '$(cat "$THOR_DIR/todo")' since $(date -r "$THOR_DIR/busy" +%T) (the motor never got there). Stop it (Ctrl-C or touch $THOR_DIR/stop), rm $THOR_DIR/todo $THOR_DIR/busy, restart it, and check the APT controller"
            fi
            tb_die "$ax did not start moving towards $target mm (still at $pos): check the thor_epics terminal and the APT controller"
        fi
        [ $t -ge $MOVE_TIMEOUT ] && tb_die "$ax did not reach $target mm within $MOVE_TIMEOUT s (at $pos mm, $(decode $stat))"
    done
}

case $1 in
    pos)
        printf '%.3f %.3f\n' "$(pv_get THOR_X:read_pos)" "$(pv_get THOR_Y:read_pos)" ;;
    status)
        for ax in X Y ; do
            s=$(pv_get THOR_$ax:read_stat)
            printf '%s  %9.3f mm   status 0x%08x  %s\n' $ax "$(pv_get THOR_$ax:read_pos)" $(( s & 0xFFFFFFFF )) "$(decode $s)"
        done ;;
    move)
        [ $# -eq 3 ] || usage
        [ "$2" == - ] || check_target X "$2"
        [ "$3" == - ] || check_target Y "$3"
        [ "$2" == - ] || move_axis X "$2"
        [ "$3" == - ] || move_axis Y "$3"
        printf 'at %.3f %.3f\n' "$(pv_get THOR_X:read_pos)" "$(pv_get THOR_Y:read_pos)" ;;
    stop)
        pv_put THOR_X:stop 1 ; pv_put THOR_Y:stop 1 ; echo "stop sent to X and Y" ;;
    home)
        [ "$3" == --yes ] || tb_die "homing drives the axis to its switch at 0 mm: add --yes"
        case $2 in x|X) axes=X ;; y|Y) axes=Y ;; both) axes="X Y" ;; *) usage ;; esac
        for ax in $axes ; do pv_put THOR_$ax:move_home 1 ; done
        tb_log "home $axes"
        echo "homing $axes (motor.sh status shows when done)" ;;
    *)
        usage ;;
esac
