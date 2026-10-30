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
#
# The selection lives in state/active.conf (not in git); TB_DETECTOR=... / TB_DAQ=... in the
# environment override it for one command.

. "$(dirname "$(readlink -f "$0")")/../lib/tb.sh"

usage() { sed -n '3,18p' "$0" | sed 's/^# \{0,1\}//' ; exit 2 ; }
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
            NF != 8 { print "  hv.conf: line \"" $0 "\" needs 8 columns" ; next }
            { for (i = 4; i <= 8; i++) if ($i != "-" && $i !~ /^[0-9]+(\.[0-9]+)?$/) print "  hv.conf: " $1 ": column " i " is not a number or -" }
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
            (($3 " " $4) in px) { print "  mapping.txt: pixel " $3 "," $4 " listed twice" }
            { ch[$1 " " $2] ; px[$3 " " $4] }
        ' | problems
        [ -r "$f/cabling.txt" ] || err "cabling.txt missing"
        # first input: mapping.txt "slot channel", second: cabling.txt "packet slot"
        awk 'FNR == NR { chans[$1] = chans[$1] " " $2 ; next }
            NF != 2 || $1 !~ /^[0-9]+$/ || $2 !~ /^[0-9]+$/ { print "  cabling.txt: bad line \"" $0 "\"" ; next }
            !($2 in chans) { print "  cabling.txt: slot " $2 " is not in mapping.txt" ; next }
            ($2 in s) { print "  cabling.txt: slot " $2 " listed twice" ; next }
            { s[$2] ; n = split(chans[$2], c, " ")
              for (i = 1; i <= n; i++) {
                  k = $1 " " c[i]
                  if (k in used) { print "  cabling.txt: packet " $1 " channel " c[i] " is in slot " used[k] " and slot " $2 ; break }
                  used[k] = $2
              } }
        ' <(tb_table "$f/mapping.txt" | awk '{ print $1, $2 }') <(tb_table "$f/cabling.txt" 2> /dev/null) | problems
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
    local e u c vset iset rup rdw maxv
    while read -r e u c vset iset rup rdw maxv ; do
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

case $1 in
    list)    cmd_list ;;
    use)     shift ; cmd_use "$@" ;;
    show)    cmd_show ;;
    check)   cmd_check ;;
    new)     [ -n "$2" ] || usage ; cmd_new "$2" ;;
    hv-show) cmd_hv show ;;
    hv-load) cmd_hv load "$2" ;;
    *)       usage ;;
esac
