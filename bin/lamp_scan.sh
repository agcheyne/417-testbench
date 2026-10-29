#! /bin/bash
#
# Lamp spectrum through the monochromator, read on the Newport power meter, with a dark taken
# at every wavelength by parking the grating at MONO_DARK: nothing is capped or touched, so the
# box stays closed and the dark follows the meter's drift (tens of pW per hour, 29 Oct 2026).
#
#   lamp_scan.sh FROM TO STEP [options]      wavelengths [nm], TO included when it falls on the grid
#
#   --samples N     readings per light and per dark measurement, 1 s apart (default 5)
#   --settle S      wait after each move before reading (default 6 s; filter 1 settles slowly)
#   --name TEXT     label for the scan (folder and file names)
#   --note TEXT     free text kept with the scan: lamp, cables, slits, what is on the sphere
#   --stay          leave the grating at the last wavelength (default: parked at the dark position)
#   --max-spread P  retake a measurement once when its readings spread over more than P pW, or 5 % of
#                   the value if that is more (default 2)
#
# Each point: monochromator.sh move NM, meter wavelength set to NM, read the light; then
# monochromator.sh dark, the meter still at NM, read the dark; net = light - dark. Every move up to
# NM starts from the dark position, so each wavelength is approached from below.
# Spikes (single readings tens of pW off) are kept out: a measurement is the median of its readings,
# its spread the standard deviation of the readings within 3 robust sigmas (+0.5 pW) of the median.
# Flags in the CSV: spike-light/-dark (a reading left out), retaken-light/-dark (spread too large,
# measured again), noisy-light/-dark (still too large after the retake: treat the point with care).
# Before the scan the meter is set to watts, running and autorange; filter, zero and attenuator are
# left as they are and recorded. Use zero on (PM:ZERO 1 with the detector dark): with zero off,
# readings below 0 show as 0 and the dark is lost.
# Output: logs/scans/<date>_<time>_<name>/, <scan>.csv (one line per point, powers in W) and
# <scan>.txt (parameters, meter settings). Ctrl-C stops; the points so far are kept.

. "$(dirname "$(readlink -f "$0")")/../lib/tb.sh"
BIN=$TB_ROOT/bin
PM=$BIN/newport_powermeter.sh
MONO=$BIN/monochromator.sh

usage() { sed -n '3,29p' "$0" | sed 's/^# \{0,1\}//' ; exit 2 ; }

ORIG_ARGS=("$@")
tb_site
SAMPLES=5 SETTLE=6 MAX_SPREAD=2 LABEL= NOTE= STAY= POS=()
while [ $# -gt 0 ] ; do
    case $1 in
        --samples) SAMPLES=$2 ; shift ;;
        --settle) SETTLE=$2 ; shift ;;
        --max-spread) MAX_SPREAD=$2 ; shift ;;
        --name) LABEL=$2 ; shift ;;
        --note) NOTE=$2 ; shift ;;
        --stay) STAY=1 ;;
        -h|--help) usage ;;
        -*) tb_die "unknown option $1 (lamp_scan.sh -h)" ;;
        *) POS+=("$1") ;;
    esac
    shift
done
[ ${#POS[@]} -eq 3 ] || usage
FROM=${POS[0]} TO=${POS[1]} STEP=${POS[2]}
for v in "$FROM" "$TO" "$STEP" "$SETTLE" "$MAX_SPREAD" ; do
    [[ $v =~ ^[0-9]+(\.[0-9]*)?$ ]] || tb_die "'$v' is not a positive number"
done
[[ $SAMPLES =~ ^[1-9][0-9]*$ ]] || tb_die "--samples: '$SAMPLES' is not a whole number"
tb_fcmp "$STEP" '>' 0 && tb_fcmp "$FROM" '<=' "$TO" || tb_die "need FROM <= TO and STEP > 0"
tb_fcmp "$FROM" '>=' $MONO_MIN && tb_fcmp "$TO" '<=' $MONO_MAX || tb_die "the scan must stay within $MONO_MIN-$MONO_MAX nm (db/testbench.conf)"
PLAN=$(awk -v a="$FROM" -v b="$TO" -v s="$STEP" 'BEGIN { for (i = 0; a + i * s <= b + 1e-9; i++) printf "%g\n", a + i * s }')
NPOINTS=$(echo "$PLAN" | wc -l)

# --- instruments ---------------------------------------------------------------------------
"$MONO" status > /dev/null || exit 1
"$MONO" pos > /dev/null || exit 1
pm() { "$PM" "$@" || tb_die "power meter: '$*' failed" ; }
for i in 1 2 3 4 5 6 7 8 ; do     # empty the error queue, so the status byte starts clean
    [[ $(pm 'ERRSTR?') == 0,* ]] && break
done
pm 'PM:UNITS 2' 'PM:RUN 1' 'PM:AUTO 1' > /dev/null
read -r UNITS FILTER ZERO ATT IDN <<< "$(pm 'PM:UNITS?' 'PM:DIGITALFILTER?' 'PM:ZERO?' 'PM:ATT?' '*IDN?' | paste -sd' ')"
[ "$UNITS" == 2 ] || tb_die "the power meter did not switch to watts (PM:UNITS? $UNITS)"
[ "$ZERO" == 1 ] || tb_warn "the power meter's zero is off: readings below 0 show as 0, so darks are clipped. Cap the detector and send PM:ZERO 1 first"
[ "$ATT" == 1 ] && tb_warn "PM:ATT is 1: readings are multiplied by 1000 for the OD3 attenuator. Is it fitted?"

# --- output --------------------------------------------------------------------------------
SCAN_ID=$(date +%s)
STAMP=$(date -d @$SCAN_ID +%Y%m%d_%H%M%S)
SCAN_NAME=${LABEL:-lamp}
SCAN_NAME=${SCAN_NAME//[^A-Za-z0-9_.-]/_}
SCAN_DIR=$TB_LOGS/scans/${STAMP}_$SCAN_NAME
CSV=$SCAN_DIR/${STAMP}_$SCAN_NAME.csv
mkdir -p "$SCAN_DIR" || tb_die "cannot create $SCAN_DIR"
{
    printf 'started   %s\ncommand   %s\nnote      %s\n' "$(date -d @$SCAN_ID '+%F %T')" "$0 ${ORIG_ARGS[*]}" "$NOTE"
    printf 'points    %s (%s-%s nm, step %s), dark at %s nm after each\n' "$NPOINTS" "$FROM" "$TO" "$STEP" "$MONO_DARK"
    printf 'samples   %s per measurement, 1 s apart, after %s s settling\n' "$SAMPLES" "$SETTLE"
    printf 'meter     %s; units %s, filter %s, zero %s, attenuator %s, autorange on\n' "$IDN" "$UNITS" "$FILTER" "$ZERO" "$ATT"
    printf 'retake    when the readings spread over more than %s pW (or 5 %% of the value)\n' "$MAX_SPREAD"
    printf 'csv       powers in W; light/dark = median of the readings, *_sd = their standard deviation\n'
    printf '          without spikes; net_err = standard error of light - dark; flags: see lamp_scan.sh -h\n'
} > "$SCAN_DIR/${STAMP}_$SCAN_NAME.txt"
echo "time,nm,light_W,light_sd_W,dark_W,dark_sd_W,net_W,net_err_W,range_light,range_dark,flags" > "$CSV"
tb_log "lamp scan $SCAN_NAME start: $NPOINTS points $FROM-$TO nm step $STEP, dark at $MONO_DARK nm, log $CSV"

# SAMPLES readings 1 s apart -> "readings;ranges;flags" (readings in W, space-separated)
readings() {
    local i out q p r vals= ranges= flags=
    sleep "$SETTLE"
    pm 'PM:Power?' > /dev/null      # drop the reading from before the settle
    for (( i = 0 ; i < SAMPLES ; i++ )) ; do
        sleep 1
        out=$(pm 'Q?' 'PM:Power?' 'PM:RANGE?')
        { read -r q ; read -r p ; read -r r ; } <<< "$out"
        [[ $q =~ ^[0-9]+$ ]] || tb_die "unexpected power meter status '$q'"
        (( q & 4 )) && flags+=" saturated"
        (( q & 8 )) && flags+=" over-range"
        vals+="$p " ranges+="$r "
    done
    echo "$vals;$ranges;$flags"
}

# readings -> "median sd n_kept spread_pW": sd and n over the readings within 3 robust sigmas
# (1.4826 x median absolute deviation) + 0.5 pW of the median; spread = max - min of all of them
robust() {
    awk -v vals="$1" 'function median(a, n,   i, j, t) {
            for (i = 2; i <= n; i++) for (j = i; j > 1 && a[j - 1] > a[j]; j--) { t = a[j] ; a[j] = a[j - 1] ; a[j - 1] = t }
            return n % 2 ? a[(n + 1) / 2] : (a[n / 2] + a[n / 2 + 1]) / 2 }
        BEGIN {
            n = split(vals, v, " ") ; for (i = 1; i <= n; i++) v[i] += 0
            for (i = 1; i <= n; i++) { s[i] = v[i] ; lo = i == 1 || v[i] < lo ? v[i] : lo ; hi = i == 1 || v[i] > hi ? v[i] : hi }
            m = median(s, n)
            for (i = 1; i <= n; i++) d[i] = v[i] > m ? v[i] - m : m - v[i]
            cut = 3 * 1.4826 * median(d, n) + 0.5e-12
            for (i = 1; i <= n; i++) if ((v[i] > m ? v[i] - m : m - v[i]) <= cut) { k++ ; sum += v[i] ; ss += v[i] * v[i] }
            var = k > 1 ? (ss - sum * sum / k) / (k - 1) : 0
            printf "%.6e %.3e %d %.3f\n", m, sqrt(var > 0 ? var : 0), k, (hi - lo) * 1e12 }'
}

# KIND (light|dark) -> "median sd n_kept highest_range flags"; retaken once when the spread is too large
measure() {
    local kind=$1 r m sd k spread flags= vals ranges qflags hi
    r=$(readings) || exit 1
    read -r m sd k spread <<< "$(robust "${r%%;*}")"
    if ! tb_fcmp "$spread" '<=' "$(awk -v m=$m -v p=$MAX_SPREAD 'BEGIN { a = m < 0 ? -m : m ; print (0.05 * a * 1e12 > p) ? 0.05 * a * 1e12 : p }')" ; then
        flags+=" retaken-$kind"
        r=$(readings) || exit 1
        read -r m sd k spread <<< "$(robust "${r%%;*}")"
        tb_fcmp "$spread" '<=' "$(awk -v m=$m -v p=$MAX_SPREAD 'BEGIN { a = m < 0 ? -m : m ; print (0.05 * a * 1e12 > p) ? 0.05 * a * 1e12 : p }')" ||
            flags+=" noisy-$kind"
    fi
    [ "$k" -lt "$SAMPLES" ] && flags+=" spike-$kind"
    IFS=';' read -r vals ranges qflags <<< "$r"
    hi=$(echo $ranges | tr ' ' '\n' | sort -n | tail -1)
    flags=$(echo $flags $qflags | tr ' ' '\n' | grep . | sort -u | paste -sd'|')
    echo "$m $sd $k $hi ${flags:--}"
}

trap 'echo ; tb_log "lamp scan $SCAN_NAME interrupted, $CSV" ; tb_die "interrupted: the points so far are in $CSV"' INT

printf '%8s %12s %12s %12s %10s\n' nm "light [pW]" "dark [pW]" "net [pW]" "± [pW]"
for nm in $PLAN ; do
    "$MONO" move "$nm" > /dev/null || exit 1
    pm "PM:Lambda $(printf '%.0f' "$nm")" > /dev/null
    read -r LM LS LN LR LF <<< "$(measure light)"
    [ -n "$LF" ] || exit 1
    "$MONO" dark > /dev/null || exit 1
    read -r DM DS DN DR DF <<< "$(measure dark)"
    [ -n "$DF" ] || exit 1
    flags=$(echo "$LF $DF" | tr ' |' '\n\n' | grep -v '^-$' | grep . | sort -u | paste -sd'|')
    line=$(awk -v l=$LM -v ls=$LS -v ln=$LN -v d=$DM -v ds=$DS -v dn=$DN 'BEGIN {
        printf "%.6e,%.3e", l - d, sqrt(ls * ls / ln + ds * ds / dn) }')
    echo "$(date '+%F %T'),$nm,$LM,$LS,$DM,$DS,$line,$LR,$DR,${flags:--}" >> "$CSV"
    awk -v nm=$nm -v l=$LM -v d=$DM -v nl="$line" -v f="$flags" 'BEGIN { split(nl, x, ",")
        printf "%8s %12.2f %12.2f %12.2f %10.2f  %s\n", nm, l * 1e12, d * 1e12, x[1] * 1e12, x[2] * 1e12, f }'
done
trap - INT

[ -n "$STAY" ] && "$MONO" move "$nm" > /dev/null
tb_log "lamp scan $SCAN_NAME done: $CSV"
echo "done: $CSV"
echo "grating $( [ -n "$STAY" ] && echo "at $nm nm" || echo "parked at the dark position ($MONO_DARK nm)" )"
