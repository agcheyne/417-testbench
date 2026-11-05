#! /bin/bash
#
# Do the V1742's two DRS4 boards (inputs 0-15 and 16-31) record the same laser pulse? Above ~2 kHz they
# can take different triggers (31 Oct 2026, 5 kHz: groups 2/3 one pulse late in ~70 % of events), and then
# sums or coincidences across the two halves are wrong. Reads the group trigger time tags only: fast.
#
#   analysis/check_align/check_align.sh [--events N] [--min PCT] RUN|FILE.evt ...
#
#   RUN            a run number: its first part, <DATA_ROOT>/*/<runtype>-<run>-0000.evt
#   --events N     read only the first N events of each run (default all)
#   --min PCT      exit status 1 if a run has fewer than PCT % aligned events (for a scan's background check)
#
# One line per run: events, trigger rate, % aligned, and of the rest how many have groups 2/3 on the next
# (or previous) event's pulse.

. "$(dirname "$(readlink -f "$0")")/../../lib/tb.sh"
HERE=$(dirname "$(readlink -f "$0")")
tb_site

usage() { sed -n '3,14p' "$0" | sed 's/^# \{0,1\}//' ; exit 2 ; }

OPTS=() FILES=()
while [ $# -gt 0 ] ; do
    case $1 in
        --events|--min) [ $# -ge 2 ] || usage ; OPTS+=("$1" "$2") ; shift 2 ;;
        -h|--help) usage ;;
        -*) usage ;;
        *.evt) FILES+=("$1") ; shift ;;
        *[!0-9]*) tb_die "not a run number or .evt file: $1" ;;
        *)  f=$(ls "$DATA_ROOT"/*/*-$(printf %08d "$((10#$1))")-0000.evt 2>/dev/null | head -1)
            [ -n "$f" ] || tb_die "run $1: no file in $DATA_ROOT/*/"
            FILES+=("$f") ; shift ;;
    esac
done
[ ${#FILES[@]} -gt 0 ] || usage

if [ ! -x "$HERE/check_align" ] || [ "$HERE/check_align.cc" -nt "$HERE/check_align" ] ; then
    make -s -C "$HERE" check_align >&2 || tb_die "check_align does not build"
fi
exec "$HERE/check_align" "${OPTS[@]}" "${FILES[@]}"
