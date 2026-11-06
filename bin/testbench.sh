#! /bin/bash
#
# The test-bench database (db/): which detector and DAQ profile are in use, and their settings.
#
#   testbench.sh list                     detectors and DAQ profiles in the db, * = in use
#   testbench.sh use detector NAME        select the detector (sets run type, HV names, mapping)
#   testbench.sh use daq PROFILE          select the readout profile (setup_rcdaq.sh applies it)
#   testbench.sh show                     the selection and its settings
#   testbench.sh check                    sanity-check every file in the db
#   testbench.sh new NAME                 start a detector from db/detectors/_template
#   testbench.sh hv-show                  hv.conf of the detector next to what the CAEN units hold now
#   testbench.sh hv-load [--yes]          set VSET/ISET/RUP/RDW/MAXV on the CAEN units from hv.conf.
#                                         Without --yes it only prints what it would set. It never
#                                         switches a channel on and refuses to touch one that is on.
#                                         Also sets PDWN = RAMP, and TRIP where hv.conf gives it.
#   testbench.sh hv-on --box-safe         switch on every channel of hv.conf (both units, back to back)
#   testbench.sh hv-off                   switch off every channel of both units
#   testbench.sh hv-mcp [MCP_V PC_GAP_V [--inter V] [--anode V] [--allow-700] [--wait] [--yes]]
#                                         two-MCP detector (HRPPD): set the five electrodes from the
#                                         voltage per MCP and the PC gap (-10 = no photoelectrons),
#                                         in an order that keeps every gap within its old/new value;
#                                         no arguments = the gaps now. MCP_V <= 675 (--allow-700: 700).
#
# The selection lives in state/active.conf (not in git); TB_DETECTOR=... / TB_DAQ=... in the
# environment override it for one command.

. "$(dirname "$(readlink -f "$0")")/../lib/tb.sh"

usage() { sed -n '3,26p' "$0" | sed 's/^# \{0,1\}//' ; exit 2 ; }
N1470=$TB_ROOT/bin/n1470.sh

set_active() {   # KEY VALUE
    local DETECTOR= DAQ=
    mkdir -p "$TB_STATE"
    [ -r "$TB_STATE/active.conf" ] && tb_conf "$TB_STATE/active.conf"
    printf -v "$1" '%s' "$2"
    printf '# written by testbench.sh use - %s\nDETECTOR=%s\nDAQ=%s\n' "$(date '+%F %T')" "$DETECTOR" "$DAQ" \
        > "$TB_STATE/active.conf"
    tb_log "use ${1,,} $2"
}

cmd_list() {
    local DETECTOR= DAQ= n
    [ -r "$TB_STATE/active.conf" ] && tb_conf "$TB_STATE/active.conf"
    echo "detectors (db/detectors):"
    for n in $(tb_detector_names) ; do
        local DESCRIPTION= RUNTYPE=
        tb_conf "$(tb_detector_dir $n)/detector.conf"
        printf '  %1s %-20s %-8s %s\n' "$([ "$n" == "$DETECTOR" ] && echo '*')" "$n" "$RUNTYPE" "$DESCRIPTION"
    done
    echo "DAQ profiles (db/daq):"
    for n in $(tb_daq_names) ; do
        local DESCRIPTION=
        tb_conf "$(tb_daq_file $n)"
        printf '  %1s %-20s %s\n' "$([ "$n" == "$DAQ" ] && echo '*')" "$n" "$DESCRIPTION"
    done
}

cmd_use() {
    case $1 in
        detector) [ -d "$(tb_detector_dir "$2")" ] && [[ $2 != _* ]] || tb_die "no detector '$2' (testbench.sh list)"
                  set_active DETECTOR "$2" ;;
        daq)      [ -r "$(tb_daq_file "$2")" ] || tb_die "no DAQ profile '$2' (testbench.sh list)"
                  set_active DAQ "$2"
                  echo "run daq/setup_rcdaq.sh to apply it to the digitiser" ;;
        *)        usage ;;
    esac
    if ( tb_active ) > /dev/null 2>&1 ; then cmd_show ; else ( tb_active ) 2>&1 | sed 's/^ERROR: /next: /' ; fi
}

cmd_show() {
    tb_active
    local d=$(tb_detector_dir)
    echo "detector  $TB_DETECTOR   ($d)"
    tb_table "$d/detector.conf" | sed 's/^/    /'
    echo "  hv.conf:"
    tb_table "$d/hv.conf" | awk '{ printf "    %-12s unit %-4s ch %-2s vset %-6s iset %-5s rup %-4s rdw %-4s maxv %s\n", $1, $2, $3, $4, $5, $6, $7, $8 }'
    echo "  mapping.txt: $(tb_table "$d/mapping.txt" | wc -l) pixels mapped"
    echo "  cabling.txt: $(tb_table "$d/cabling.txt" | awk '{ printf "%spacket %s = slot %s", n++ ? ", " : "", $1, $2 }' | grep . || echo 'not filled in')"
    echo "DAQ       $TB_DAQ   ($(tb_daq_file))"
    tb_table "$(tb_daq_file)" | sed 's/^/    /'
}

cmd_new() {
    [[ $1 =~ ^[A-Za-z0-9][A-Za-z0-9_.-]*$ ]] || tb_die "name: letters, digits, _ . - only"
    local d=$(tb_detector_dir "$1")
    [ -e "$d" ] && tb_die "$d exists already"
    cp -r "$TB_DB/detectors/_template" "$d"
    sed -i "s/^NAME=\$/NAME=$1/" "$d/detector.conf"
    tb_log "new detector $1"
    echo "created $d: fill in detector.conf, hv.conf and mapping.txt, then testbench.sh check"
}

# --- db sanity check ----------------------------------------------------------------
cmd_check() {
    local bad=0 n f
    err() { echo "  $*" ; bad=1 ; }
    problems() { local l ; while IFS= read -r l ; do echo "$l" ; bad=1 ; done ; }   # lines from awk checks
    shopt -s lastpipe   # so "... | problems" runs here and can set bad
    tb_site
    for n in DATA_ROOT CALIB_DIR ANC_PACKETS X_MIN X_MAX Y_MIN Y_MAX REST_X REST_Y MOVE_TOL MOVE_TIMEOUT \
             MONO_GPIB_BOARD MONO_GPIB_ADDR MONO_STEPS_PER_NM MONO_MIN MONO_MAX MONO_BACKLASH MONO_MOVE_TIMEOUT ; do
        [ -n "${!n}" ] || err "testbench.conf: $n is empty"
    done
    [ "$HV_UNITS" == "$(sed -n 's/^UNITS=(\([^)]*\)).*/\1/p' "$N1470")" ] ||
        err "testbench.conf: HV_UNITS=\"$HV_UNITS\" differs from UNITS in bin/n1470.sh"
    tb_fcmp $X_MIN '>=' 0 && tb_fcmp $X_MAX '<=' 150 && tb_fcmp $Y_MIN '>=' 0 && tb_fcmp $Y_MAX '<=' 150 ||
        err "testbench.conf: stage limits must lie inside 0-150 mm (thor_epics ignores anything else)"
    tb_fcmp $MONO_MIN '>=' 100 && tb_fcmp $MONO_MIN '<' $MONO_MAX && tb_fcmp $MONO_MAX '<=' 870 ||
        err "testbench.conf: monochromator limits must lie inside 100-870 nm (zero order below, LabVIEW's soft limit above)"
    for n in $(tb_detector_names) ; do
        echo "detector $n"
        f=$(tb_detector_dir $n)
        local NAME= RUNTYPE= HV_SUPPLY=
        tb_conf "$f/detector.conf"
        [ "$NAME" == "$n" ] || err "detector.conf: NAME=$NAME, folder is $n"
        [[ $RUNTYPE =~ ^[A-Za-z0-9_-]+$ ]] || err "detector.conf: RUNTYPE '$RUNTYPE' is not a plain word"
        tb_table "$f/hv.conf" | awk -v units=" A B $(sed -n 's/^UNITS=(\(.*\)).*/\1/p' "$N1470" | sed 's/[A-Z]*://g') " '
            NF != 8 && NF != 10 && NF != 11 { print "  hv.conf: line \"" $0 "\" needs 8 columns (10 with imax_uA r_MOhm, 11 with trip_s)" ; next }
            { for (i = 4; i <= NF; i++) if ($i != "-" && $i !~ /^[0-9]+(\.[0-9]+)?$/) print "  hv.conf: " $1 ": column " i " is not a number or -" }
            $2 != "-" && index(units, " " $2 " ") == 0 { print "  hv.conf: " $1 ": unknown unit " $2 }
            $3 != "-" && $3 !~ /^[0-3]$/ { print "  hv.conf: " $1 ": channel " $3 " not 0-3" }
            ($2 == "-") != ($3 == "-") { print "  hv.conf: " $1 ": give both unit and ch, or neither" }
            $4 != "-" && $8 != "-" && $4 + 0 > $8 + 0 { print "  hv.conf: " $1 ": vset above maxv" }
            $2 != "-" { k = $2 ":" $3 ; if (k in seen) print "  hv.conf: " $1 " and " seen[k] " both on " k ; seen[k] = $1 }
        ' | problems
        local NROWS= NCOLS=
        tb_conf "$f/detector.conf"
        tb_table "$f/mapping.txt" | awk -v nr="$NROWS" -v nc="$NCOLS" '
            NF != 4 || $1 !~ /^[0-9]+$/ || $2 !~ /^[0-9]+$/ || $3 !~ /^[0-9]+$/ || $4 !~ /^[0-9]+$/ { print "  mapping.txt: bad line \"" $0 "\"" ; next }
            $2 > 31 { print "  mapping.txt: channel " $2 " > 31" }
            nr != "" && $3 >= nr + 0 { print "  mapping.txt: row " $3 " but NROWS=" nr }
            nc != "" && $4 >= nc + 0 { print "  mapping.txt: col " $4 " but NCOLS=" nc }
            (($1 " " $2) in ch) { print "  mapping.txt: slot " $1 " channel " $2 " listed twice" }
            (($1 " " $3 " " $4) in px) { print "  mapping.txt: slot " $1 " pixel " $3 "," $4 " listed twice" }
            { ch[$1 " " $2] ; px[$1 " " $3 " " $4] }
        ' | problems
        [ -r "$f/cabling.txt" ] || err "cabling.txt missing"
        # first input: mapping.txt "slot channel row col", second: cabling.txt "packet slot". Slots may
        # share pixels (the same readout plugged in another way), but the slots read may not
        awk 'FNR == NR { chans[$1] = chans[$1] " " $2 ; pixs[$1] = pixs[$1] " " $3 "," $4 ; next }
            NF != 2 || $1 !~ /^[0-9]+$/ || $2 !~ /^[0-9]+$/ { print "  cabling.txt: bad line \"" $0 "\"" ; next }
            !($2 in chans) { print "  cabling.txt: slot " $2 " is not in mapping.txt" ; next }
            ($2 in s) { print "  cabling.txt: slot " $2 " listed twice" ; next }
            { s[$2] ; n = split(chans[$2], c, " ")
              for (i = 1; i <= n; i++) {
                  k = $1 " " c[i]
                  if (k in used) { print "  cabling.txt: packet " $1 " channel " c[i] " is in slot " used[k] " and slot " $2 ; break }
                  used[k] = $2
              }
              n = split(pixs[$2], p, " ")
              for (i = 1; i <= n; i++) {
                  if (p[i] in read) { print "  cabling.txt: pixel " p[i] " is in slot " read[p[i]] " and slot " $2 ; break }
                  read[p[i]] = $2
              } }
        ' <(tb_table "$f/mapping.txt") <(tb_table "$f/cabling.txt" 2> /dev/null) | problems
    done
    for n in $(tb_daq_names) ; do
        echo "DAQ profile $n"
        local READOUT= FREQ_CODE= CALIB_SPEED= RECORD_LENGTH= COMPRESSION=
        tb_conf "$(tb_daq_file $n)"
        case $READOUT in
            v1742) [[ $FREQ_CODE =~ ^[012]$ ]] || err "$n: FREQ_CODE must be 0, 1 or 2"
                   [ "$CALIB_SPEED" == "$(echo 5G 2G 1G | cut -d' ' -f$((FREQ_CODE + 1)))" ] ||
                       err "$n: CALIB_SPEED $CALIB_SPEED does not match FREQ_CODE $FREQ_CODE"
                   ls "$CALIB_DIR"/calib_*_"$CALIB_SPEED".dat > /dev/null 2>&1 ||
                       err "$n: no calibration calib_*_$CALIB_SPEED.dat in $CALIB_DIR"
                   [[ $RECORD_LENGTH =~ ^(1024|520|256|136)$ ]] || err "$n: RECORD_LENGTH $RECORD_LENGTH (V1742: 1024, 520, 256, 136)"
                   [[ ${COMPRESSION:-4} =~ ^[0-4]$ ]] || err "$n: COMPRESSION $COMPRESSION (rcdaq: 0 none .. 4 bzip2)" ;;
            scope) ;;
            *)     err "$n: READOUT must be v1742 or scope" ;;
        esac
    done
    [ $bad -eq 0 ] && echo "db OK" || { echo "db has problems (above)" ; exit 1 ; }
}

# --- HV --------------------------------------------------------------------------------
# rows of hv.conf that name a unit and channel: "electrode unit ch vset iset rup rdw maxv"
hv_rows() { tb_table "$(tb_detector_dir)/hv.conf" | awk '$2 != "-" && $3 != "-"' ; }

# the status table of one unit as "ch VSET VMON ISET IMON RUP RDW MAXV POL STAT"
unit_table() {
    "$N1470" -u "$1" status | awk '$1 ~ /^[0-3]$/ && NF >= 10 { print $1, $2, $3, $4, $5, $6, $7, $8, $9, $10 }'
}

cmd_hv() {   # show | load [--yes]
    tb_active
    local mode=$1 yes=$2 rows unit
    local HV_SUPPLY=
    tb_conf "$(tb_detector_dir)/detector.conf"
    [ "$HV_SUPPLY" == n1470 ] || tb_die "$TB_DETECTOR: HV_SUPPLY=$HV_SUPPLY, not n1470"
    rows=$(hv_rows)
    [ -n "$rows" ] || tb_die "$TB_DETECTOR: no row in hv.conf has a unit and channel yet"
    declare -A now=()
    for unit in $(echo "$rows" | awk '{ print $2 }' | sort -u) ; do
        local t
        t=$(unit_table "$unit") || tb_die "cannot read unit $unit"
        while read -r ch rest ; do now[$unit:$ch]=$rest ; done <<< "$t"
    done
    local -a cmds=()
    printf '%-12s %-5s %-3s %-22s %s\n' electrode unit ch "db vset/iset/rup/rdw/maxv" "supply VSET/ISET/RUP/RDW/MAXV  VMON  state"
    local e u c vset iset rup rdw maxv imax rmohm trip
    while read -r e u c vset iset rup rdw maxv imax rmohm trip _ ; do
        local cur=(${now[$u:$c]})   # VSET VMON ISET IMON RUP RDW MAXV POL STAT
        [ ${#cur[@]} -ge 9 ] || { printf '%-12s %-5s %-3s no readback\n' "$e" "$u" "$c" ; continue ; }
        local st=$((10#${cur[8]})) state=off
        (( st & 1 )) && state=ON
        (( st & 6 )) && state=ramping
        printf '%-12s %-5s %-3s %-22s %-30s %-5s %s\n' "$e" "$u" "$c" "$vset/$iset/$rup/$rdw/$maxv" \
            "${cur[0]}/${cur[2]}/${cur[4]}/${cur[5]}/${cur[6]}" "${cur[1]}" "$state"
        [ "$mode" == load ] || continue
        # order: MAXV first so a higher VSET is accepted, VSET last
        local p v i=0 want=("$maxv" "$iset" "$rup" "$rdw" "$vset") par=(MAXV ISET RUP RDW VSET) idx=(6 2 4 5 0)
        for p in "${par[@]}" ; do
            v=${want[$i]}
            if [ "$v" != - ] && ! tb_fcmp "$v" == "${cur[${idx[$i]}]}" ; then
                [ $state == off ] || tb_die "$e ($u ch $c) is $state: switch it off before changing $p"
                cmds+=("$u set $p $c $v")
            fi
            i=$((i + 1))
        done
        # PDWN RAMP on every row (4 Nov 2026): with KILL a tripped channel drops at once, and on a
        # detector with one channel per electrode (HRPPD) the gaps next to it then go far above
        # their values; TRIP (the supply's trip time) when hv.conf gives it
        local pdwn tr
        pdwn=$("$N1470" -u "$u" mon PDWN "$c" 2> /dev/null)
        if [ "$pdwn" != RAMP ] ; then
            [ $state == off ] || tb_die "$e ($u ch $c) is $state with PDWN ${pdwn:-unread}: switch it off before changing PDWN"
            cmds+=("$u set PDWN $c RAMP")
        fi
        if [ -n "$trip" ] && [ "$trip" != - ] ; then
            tr=$("$N1470" -u "$u" mon TRIP "$c" 2> /dev/null)
            if ! tb_fcmp "$trip" == "${tr:-x}" ; then
                [ $state == off ] || tb_die "$e ($u ch $c) is $state: switch it off before changing TRIP"
                cmds+=("$u set TRIP $c $trip")
            fi
        fi
    done <<< "$rows"
    [ "$mode" == load ] || return 0
    [ ${#cmds[@]} -gt 0 ] || { echo "supply already matches hv.conf" ; return 0 ; }
    echo "to set:"
    printf '  n1470.sh -u %s\n' "${cmds[@]}"
    [ "$yes" == --yes ] || { echo "(nothing done: add --yes to set these)" ; return 0 ; }
    local cmd
    for cmd in "${cmds[@]}" ; do
        "$N1470" -u $cmd || tb_die "failed: n1470.sh -u $cmd"
    done
    tb_log "hv-load $TB_DETECTOR: ${cmds[*]}"
    echo "done (channels are still off; switch on with n1470.sh ... on CH --box-safe)"
}

# --- the detector's channels as one group (4 Nov 2026, for the HRPPD's five electrodes) -----------
# NOW[unit:ch] = "VSET VMON ISET IMON RUP RDW MAXV POL STAT" of the units in hv.conf
declare -A NOW=()
hv_now() {   # ROWS
    local unit t ch rest
    NOW=()
    for unit in $(echo "$1" | awk '{ print $2 }' | sort -u) ; do
        t=$(unit_table "$unit") || tb_die "cannot read unit $unit"
        while read -r ch rest ; do NOW[$unit:$ch]=$rest ; done <<< "$t"
    done
}
hv_state() {   # UNIT CH -> off | ON | ramping | unread
    local cur=(${NOW[$1:$2]}) st
    [ ${#cur[@]} -ge 9 ] || { echo unread ; return ; }
    st=$((10#${cur[8]}))
    if (( st & 6 )) ; then echo ramping ; elif (( st & 1 )) ; then echo ON ; else echo off ; fi
}

# all channels of hv.conf on, both units back to back (one command per unit). The IOC switches
# them all off again if they are not all on within 10 s (n1470_group.db).
cmd_hv_on() {   # --box-safe
    tb_active
    [ "$1" == --box-safe ] || tb_die "refusing to power on without --box-safe (the dark box is closed and the detector is safe to power)"
    local rows e u c _ n s rates
    rows=$(hv_rows)
    [ -n "$rows" ] || tb_die "$TB_DETECTOR: no row in hv.conf has a unit and channel yet"
    hv_now "$rows"
    n=$(echo "$rows" | wc -l)
    while read -r e u c _ ; do
        s=$(hv_state $u $c)
        [ $s == off ] || tb_die "$e ($u ch $c) is $s: hv-on switches on a detector that is all off"
        local cur=(${NOW[$u:$c]})
        rates+="${cur[4]}/${cur[5]} "
        if [ $n -gt 1 ] ; then
            [ "$("$N1470" -u $u mon PDWN $c)" == RAMP ] || tb_die "$e ($u ch $c): PDWN is not RAMP (testbench.sh hv-load --yes sets it)"
        fi
    done <<< "$rows"
    [ $n -gt 1 ] && [ $(tr ' ' '\n' <<< "$rates" | sed '/^$/d' | sort -u | wc -l) -ne 1 ] &&
        tb_die "the channels' RUP/RDW differ ($rates): they must ramp together (hv.conf, then hv-load --yes)"
    for u in $(echo "$rows" | awk '{ print $2 }' | sort -u) ; do
        local chs=$(echo "$rows" | awk -v u=$u '$2 == u { print $3 }')
        if [ $(echo "$chs" | wc -l) -eq 4 ] ; then
            "$N1470" -u $u on all --box-safe || tb_die "unit $u did not switch on: testbench.sh hv-off"
        else
            for c in $chs ; do "$N1470" -u $u on $c --box-safe || tb_die "$u ch $c did not switch on: testbench.sh hv-off" ; done
        fi
    done
    tb_log "hv-on $TB_DETECTOR"
    echo "switched on: $(echo "$rows" | awk '{ printf "%s%s", (NR > 1 ? ", " : ""), $1 }') (testbench.sh hv-mcp to watch the ramp)"
}

cmd_hv_off() {
    "$N1470" -u all off all && tb_log "hv-off (both units, all channels)"
}

# electrode voltages of a two-MCP detector from MCP_V (per MCP) and PC_GAP_V (PC to MCP1 entry;
# negative = PC less negative than MCP1 entry, no photoelectrons reach the MCPs). VSETs are
# written so that no gap passes its old or new value on the way (equal ramp rates): the ones
# that go down first, from the PC down, then the ones that go up, from the anode up.
MCP_ELECTRODES="PC MCP1-entry MCP1-exit MCP2-entry MCP2-exit"
cmd_hv_mcp() {   # [MCP_V PC_GAP_V [--inter V] [--anode V] [--allow-700] [--wait] [--yes]]
    tb_active
    local mcp= gap= inter=200 anode=200 allow= wait= yes= rows e u c vset _x maxv
    while [ $# -gt 0 ] ; do
        case $1 in
            --inter) inter=$2 ; shift ;;
            --anode) anode=$2 ; shift ;;
            --allow-700) allow=1 ;;
            --wait) wait=1 ;;
            --yes) yes=1 ;;
            -[0-9]*) ;&
            [0-9]*) if [ -z "$mcp" ] ; then mcp=$1 ; elif [ -z "$gap" ] ; then gap=$1 ; else usage ; fi ;;
            *) usage ;;
        esac
        shift
    done
    rows=$(hv_rows)
    declare -A U=() C=() MAXV=() WANT=()
    while read -r e u c vset _x _x _x maxv _ ; do U[$e]=$u C[$e]=$c MAXV[$e]=$maxv ; done <<< "$rows"
    for e in $MCP_ELECTRODES ; do
        [ -n "${U[$e]}" ] || tb_die "$TB_DETECTOR: hv.conf has no row $e with a unit and channel (hv-mcp needs $MCP_ELECTRODES)"
    done
    hv_now "$rows"
    if [ -z "$mcp" ] ; then hv_gaps ; return ; fi
    [ -n "$gap" ] || usage
    local n
    for n in "$mcp" "$gap" "$inter" "$anode" ; do [[ $n =~ ^-?[0-9]+(\.[0-9]+)?$ ]] || tb_die "not a number: $n" ; done
    local mmax=675 why=" (above 675: afterpulsing > 4 %, the Incom lifetime limit; --allow-700 to go to 700)"
    [ -n "$allow" ] && mmax=700 why=
    tb_fcmp "$mcp" '>=' 0 && tb_fcmp "$mcp" '<=' $mmax || tb_die "MCP_V $mcp outside 0-$mmax V per MCP$why"
    tb_fcmp "$gap" '>=' -10 && tb_fcmp "$gap" '<=' 400 || tb_die "PC_GAP_V $gap outside -10 to 400 V (Incom's test range)"
    tb_fcmp "$inter" '>=' 0 && tb_fcmp "$inter" '<=' 250 || tb_die "--inter $inter outside 0-250 V"
    tb_fcmp "$anode" '>=' 0 && tb_fcmp "$anode" '<=' 250 || tb_die "--anode $anode outside 0-250 V"
    WANT[MCP2-exit]=$anode
    WANT[MCP2-entry]=$(tb_calc "$anode + $mcp")
    WANT[MCP1-exit]=$(tb_calc "$anode + $mcp + $inter")
    WANT[MCP1-entry]=$(tb_calc "$anode + 2 * $mcp + $inter")
    WANT[PC]=$(tb_calc "$anode + 2 * $mcp + $inter + $gap")
    local states="" s down=() up=()
    printf '%-12s %-5s %-3s %8s -> %8s  %s\n' electrode unit ch VSET new state
    for e in $MCP_ELECTRODES ; do
        s=$(hv_state ${U[$e]} ${C[$e]}) ; states+=" $s"
        local cur=(${NOW[${U[$e]}:${C[$e]}]})
        printf '%-12s %-5s %-3s %8s -> %8s  %s\n' "$e" "${U[$e]}" "${C[$e]}" "${cur[0]}" "${WANT[$e]}" "$s"
        [ "${MAXV[$e]}" == - ] || tb_fcmp "${WANT[$e]}" '<=' "${MAXV[$e]}" || tb_die "$e: ${WANT[$e]} V above its maxv ${MAXV[$e]} in hv.conf"
        if tb_fcmp "${WANT[$e]}" '<' "${cur[0]}" ; then down+=("$e")
        elif tb_fcmp "${WANT[$e]}" '>' "${cur[0]}" ; then up=("$e" "${up[@]}")
        fi
    done
    [[ $states == *unread* ]] && tb_die "a channel could not be read: nothing set"
    [[ $states == *ramping* ]] && tb_die "a channel is still ramping: wait for it (testbench.sh hv-mcp shows the gaps)"
    [[ $states == *ON* && $states == *off* ]] && tb_die "some channels are on and some off: testbench.sh hv-off, or switch the rest on"
    [ ${#down[@]} -gt 0 ] || [ ${#up[@]} -gt 0 ] || { echo "already set" ; return 0 ; }
    echo "order: ${down[*]:+down ${down[*]}}${down[*]:+${up[*]:+, then }}${up[*]:+up ${up[*]}}"
    [ -n "$yes" ] || { echo "(nothing done: add --yes to set these)" ; return 0 ; }
    for e in "${down[@]}" "${up[@]}" ; do
        "$N1470" -u ${U[$e]} set VSET ${C[$e]} ${WANT[$e]} > /dev/null || tb_die "failed to set $e VSET; testbench.sh hv-off if in doubt"
    done
    tb_log "hv-mcp $TB_DETECTOR: $mcp V per MCP, PC gap $gap V (inter $inter, anode $anode)"
    echo "set: $mcp V per MCP, PC gap $gap V"
    [ -n "$wait" ] || return 0
    local t0=$SECONDS
    while : ; do
        sleep 4
        hv_now "$rows"
        hv_gaps
        local done=1
        for e in $MCP_ELECTRODES ; do
            local cur=(${NOW[${U[$e]}:${C[$e]}]})
            [ "$(hv_state ${U[$e]} ${C[$e]})" == off ] && continue
            tb_fcmp "$(tb_calc "${cur[1]} - ${WANT[$e]}")" '<=' 2 && tb_fcmp "$(tb_calc "${WANT[$e]} - ${cur[1]}")" '<=' 2 || done=
        done
        [ -n "$done" ] && { echo "reached" ; return 0 ; }
        [ $((SECONDS - t0)) -lt 900 ] || tb_die "not reached after 15 min"
    done
}
# one line: the gaps from VMON, and each electrode's VMON / IMON
hv_gaps() {
    local e v=() i=()
    for e in $MCP_ELECTRODES ; do
        local cur=(${NOW[${U[$e]}:${C[$e]}]})
        v+=("$(tb_calc "${cur[1]:-nan} + 0")") i+=("$(tb_calc "${cur[3]:-nan} + 0")")
    done
    printf '%s  PC gap %s  MCP1 %s  inter %s  MCP2 %s  anode %s V |' "$(date +%T)" \
        "$(tb_calc "${v[0]} - ${v[1]}")" "$(tb_calc "${v[1]} - ${v[2]}")" "$(tb_calc "${v[2]} - ${v[3]}")" \
        "$(tb_calc "${v[3]} - ${v[4]}")" "${v[4]}"
    local k=0
    for e in $MCP_ELECTRODES ; do printf ' %s %s V %s uA' "$e" "${v[$k]}" "${i[$k]}" ; k=$((k + 1)) ; done
    echo
}

case $1 in
    list)    cmd_list ;;
    use)     shift ; cmd_use "$@" ;;
    show)    cmd_show ;;
    check)   cmd_check ;;
    new)     [ -n "$2" ] || usage ; cmd_new "$2" ;;
    hv-show) cmd_hv show ;;
    hv-load) cmd_hv load "$2" ;;
    hv-on)   cmd_hv_on "$2" ;;
    hv-off)  cmd_hv_off ;;
    hv-mcp)  shift ; cmd_hv_mcp "$@" ;;
    *)       usage ;;
esac
