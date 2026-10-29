#! /bin/bash
#
# Plot a lamp scan (bin/lamp_scan.sh). Only reads the data; the plot goes into the scan's folder.
#
#   analysis/lamp_scan/plot_lamp_scan.sh [SCAN]
#
#   SCAN    the scan's folder (logs/scans/<scan>/), its .csv, or just <scan>;
#           default: the newest lamp scan in logs/scans/
#
# Writes <scan>_spectrum.png in the scan's folder (light - dark against wavelength, with the light
# and dark as read; the dark against time) and prints the table in pW. Running it again, e.g. on a
# scan that was stopped and is now complete, overwrites the plot.

. "$(dirname "$(readlink -f "$0")")/../../lib/tb.sh"
HERE=$(dirname "$(readlink -f "$0")")

usage() { sed -n '3,13p' "$0" | sed 's/^# \{0,1\}//' ; exit 2 ; }

SCAN=
while [ $# -gt 0 ] ; do
    case $1 in
        -h|--help) usage ;;
        -*) tb_die "unknown option $1 (plot_lamp_scan.sh -h)" ;;
        *) [ -z "$SCAN" ] || tb_die "one scan at a time" ; SCAN=$1 ;;
    esac
    shift
done
HEADER=time,nm,light_W
if [ -z "$SCAN" ] ; then
    CSV=$(ls -t "$TB_LOGS"/scans/*/*.csv 2> /dev/null | while read -r f ; do
              head -1 "$f" | grep -q "^$HEADER" && { echo "$f" ; break ; } ; done)
    [ -n "$CSV" ] || tb_die "no lamp scan in $TB_LOGS/scans/"
    DIR=$(dirname "$CSV")
elif [ -d "$SCAN" ] ; then DIR=$SCAN
elif [ -f "$SCAN" ] ; then DIR=$(dirname "$SCAN")
elif [ -d "$TB_LOGS/scans/$SCAN" ] ; then DIR=$TB_LOGS/scans/$SCAN
else tb_die "no scan $SCAN (give its folder in logs/scans/)"
fi
DIR=$(readlink -f "$DIR")
NAME=$(basename "$DIR")
PREFIX=$DIR/$NAME
CSV=$PREFIX.csv
[ -r "$CSV" ] || tb_die "no $CSV"
head -1 "$CSV" | grep -q "^$HEADER" || tb_die "$CSV is not a lamp scan (bin/lamp_scan.sh)"
command -v root > /dev/null || tb_die "no root in PATH (source the ROOT setup first)"

echo "scan $NAME: $(( $(wc -l < "$CSV") - 1 )) points"
sed -n 's/^note *//p' "$PREFIX.txt" 2> /dev/null | sed 's/^/  note: /'
( cd "$HERE" && root -b -q -l "plot_lamp_scan.C(\"$CSV\", \"$PREFIX\")" ) 2>&1 | grep -v '^Info in\|^Processing\|^$'
[ -e "${PREFIX}_spectrum.png" ] || tb_die "no plot was written"
echo "  ${PREFIX}_spectrum.png"
