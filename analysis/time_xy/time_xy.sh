#! /bin/bash
#
# Hit time vs laser position of a finished xy scan or x line (scans/scan_xy.sh): where in x-y each part of a
# channel's time spectrum comes from (the direct laser peak, its halo, the later shoulder and bumps), and where
# the peak arrives first. Only reads the data; plots go into <scan>_time_xy/ in the scan's folder.
# First done on the Photonis 0.4 mm scan of 31 Oct 2026 (pad centre 30-60 ps earlier than its edges).
#
#   analysis/time_xy/time_xy.sh SCAN (--pad CH | --ch "CH,CH,..") [options]
#
#   SCAN             the scan's folder (logs/scans/<scan>/), its .csv, or just <scan>
#   --pad CH         input CH alone ("ch<CH>_*") and with the inputs on its 8 neighbouring pixels
#                    ("ch<CH>_neighbours_*", from the mapping and the run's own cabling; MASKED_CHANNELS left out)
#   --ch "A,B,.."    these inputs summed instead ("<name>_*", --name, default ch<A>_<B>..)
#   --plots "LIST"   which sets (default all): times in slices relative to the first input's laser peak P
#                      overview  P-7 to P-1, P+-0.6, to P+3, to P+15, to P+20 ns: maps (or x curves), time vs x / y
#                      many      1 ns from P-7, 0.25 ns over P-2 to P+4, then 1 ns to P+20 (45 slices)
#                      peak      0.1 ns over P-0.5 to P+1.5
#                      peak50    50 ps over P-0.5 to P+1.5 (the parts' bin)
#                      map       median hit time and its spread per point in P-0.4 to P+0.6 (x-y scans)
#   --x0 MM --y0 MM  stage position of pixel (0,0) for the pad grid lines (default: the db's STAGE_X0/Y0_MM now;
#                    an older scan may need its own, e.g. Photonis before run 6902: --x0 32.6)
#   --no-align       keep each input's db time offset (default: line each input's peak up with the first's)
#   --events N       events read per point (default 20000)
#   --jobs N         points read at once (default: the cores less one)
#   --redo           read the points again (default: keep the parts already read)
#
# The parts (one TH2F of channel x time per point, 50 ps bins, and each input's pixel) go to
# <DATA_ROOT>/analysis/time_xy/<scan>/ (home quota); time_xy_point.cc reads them, time_xy_plots.C and
# peak_time_map.C draw them (both can be run by hand, see their headers).

. "$(dirname "$(readlink -f "$0")")/../../lib/tb.sh"
HERE=$(dirname "$(readlink -f "$0")")
MASKED_CHANNELS=
tb_site

usage() { sed -n '3,30p' "$0" | sed 's/^# \{0,1\}//' ; exit 2 ; }

SCAN= PAD= CHS= NAME= PLOTS="overview many peak peak50 map" X0= Y0= ALIGN=true EVENTS=20000 JOBS=$(( $(nproc) - 1 )) REDO=
while [ $# -gt 0 ] ; do
    case $1 in
        --pad) PAD=$2 ; shift ;;
        --ch) CHS=$2 ; shift ;;
        --name) NAME=$2 ; shift ;;
        --plots) PLOTS=$2 ; shift ;;
        --x0) X0=$2 ; shift ;;
        --y0) Y0=$2 ; shift ;;
        --no-align) ALIGN=false ;;
        --events) EVENTS=$2 ; shift ;;
        --jobs) JOBS=$2 ; shift ;;
        --redo) REDO=1 ;;
        -h|--help) usage ;;
        -*) tb_die "unknown option $1 (time_xy.sh -h)" ;;
        *) [ -z "$SCAN" ] || tb_die "one scan at a time" ; SCAN=$1 ;;
    esac
    shift
done
[ -n "$SCAN" ] || usage
[ -n "$PAD$CHS" ] || tb_die "give --pad CH or --ch \"A,B,..\""
[ -z "$PAD" ] || [ -z "$CHS" ] || tb_die "--pad or --ch, not both"

if [ -d "$SCAN" ] ; then DIR=$SCAN
elif [ -f "$SCAN" ] ; then DIR=$(dirname "$SCAN")
elif [ -d "$TB_LOGS/scans/$SCAN" ] ; then DIR=$TB_LOGS/scans/$SCAN
else tb_die "no scan $SCAN (give its folder in logs/scans/)"
fi
DIR=$(readlink -f "$DIR")
N=$(basename "$DIR")
PREFIX=$DIR/$N
CSV=$PREFIX.csv
[ -f "$CSV" ] || tb_die "no $CSV"
RUNTYPE=$(sed -n 's/^# detector .*, run type \([^ ,]*\).*/\1/p' "$PREFIX.txt" 2> /dev/null)
DET=$(sed -n 's/^# detector \([^ ,]*\),.*/\1/p' "$PREFIX.txt" 2> /dev/null)
[ -n "$RUNTYPE" ] && [ -n "$DET" ] || tb_die "no detector / run type in $PREFIX.txt"

# pad grid from the db (now), unless given
NROWS= NCOLS= PITCH_X_MM= PITCH_Y_MM= STAGE_X0_MM= STAGE_Y0_MM= STAGE_X_DIR=1 STAGE_Y_DIR=1
tb_conf "$(tb_detector_dir "$DET")/detector.conf"
RUNTYPE=$(sed -n 's/^# detector .*, run type \([^ ,]*\).*/\1/p' "$PREFIX.txt")   # the scan's, not the db's now
X0=${X0:-$STAGE_X0_MM} Y0=${Y0:-$STAGE_Y0_MM}
GEO=
if [ -n "$X0" ] && [ -n "$Y0" ] && [ -n "$PITCH_X_MM" ] && [ -n "$NCOLS" ] ; then
    GEO="$X0,$Y0,$PITCH_X_MM,${PITCH_Y_MM:-$PITCH_X_MM},$NCOLS,${NROWS:-$NCOLS},$STAGE_X_DIR,$STAGE_Y_DIR"
    tb_info "$DET pad grid: pixel (0,0) at $X0, $Y0 mm, pitch $PITCH_X_MM mm${STAGE_X0_MM:+ (db: $STAGE_X0_MM, $STAGE_Y0_MM)}"
else
    tb_warn "$DET: no stage geometry in its detector.conf, plots without the pad grid"
fi

# --- read the points ---------------------------------------------------------------------------------
make -s -C "$HERE" time_xy_point || tb_die "could not build time_xy_point"
export LD_LIBRARY_PATH=$ONLINE_MAIN/lib:$LD_LIBRARY_PATH
P=$DATA_ROOT/analysis/time_xy/$N
mkdir -p "$P"
[ -z "$REDO" ] || rm -f "${P:?}"/p*.root
read_points() {   # $1 = events (0 = pixel info only, into existing parts)
    awk -F, 'NR > 1 && $10 == "ok" { print $1, $6 }' "$CSV" | while read -r pt run ; do
        f=$DATA_ROOT/$RUNTYPE/$RUNTYPE-$(printf %08d "$run")-0000.evt
        if [ "$1" == 0 ] ; then [ -e "$P/p$pt.root" ] && echo "$pt $f"
        elif [ ! -e "$P/p$pt.root" ] ; then echo "$pt $f"
        fi
    done | xargs -P "$JOBS" -L 1 bash -c '
        if [ "'"$1"'" == 0 ] ; then "'"$HERE"'/time_xy_point" "$1" "'"$P"'/p$0.root" 0 > /dev/null 2>&1
        else "'"$HERE"'/time_xy_point" "$1" "'"$P"'/p$0.tmp.root" '"$1"' > /dev/null 2>&1 && mv "'"$P"'/p$0.tmp.root" "'"$P"'/p$0.root"
        fi'
}
NOK=$(awk -F, 'NR > 1 && $10 == "ok"' "$CSV" | wc -l)
if [ ! -e "$P/.pixels" ] && ls "$P"/p*.root > /dev/null 2>&1 ; then
    tb_info "adding each input's pixel to the parts read before it was stored"
    read_points 0
fi
todo=$(( NOK - $(ls "$P" | grep -c '^p[0-9]*\.root$') ))
if [ "$todo" -gt 0 ] ; then
    tb_info "reading $todo of $NOK points, $EVENTS events each, $JOBS at a time, into $P"
    read_points "$EVENTS"
fi
touch "$P/.pixels"
tb_info "$(ls "$P" | grep -c '^p[0-9]*\.root$') of $NOK points read"

# --- plots -----------------------------------------------------------------------------------------
OUT=${PREFIX}_time_xy
mkdir -p "$OUT"
if [ -n "$PAD" ] ; then SETS="ch$PAD:$PAD ch${PAD}_neighbours:pad:$PAD"
else SETS="${NAME:-ch${CHS//,/_}}:$CHS"
fi
declare -A SL=(
    [overview]="p-7:p-1,p-0.6:p+0.6,p+0.6:p+3,p+3:p+15,p+15:p+20"
    [many]="p-7:p-2:1,p-2:p+4:0.25,p+4:p+20:1"
    [peak]="p-0.5:p+1.5:0.1"
    [peak50]="p-0.5:p+1.5:0.05" )
cd "$HERE" || exit 1
for set in $SETS ; do
    name=${set%%:*} spec=${set#*:}
    for pl in $PLOTS ; do
        case $pl in
            overview) o=$OUT/$name ;;
            many|peak|peak50) o=$OUT/${name}_$pl ;;
            map) root -l -b -q "peak_time_map.C+(\"$CSV\", \"$P\", \"$OUT/$name\", \"$spec\", -0.4, 0.6, 30, \"$GEO\", \"$MASKED_CHANNELS\")" 2>&1 |
                     grep -v -e '^Info in' -e '^Processing' -e 'CheckABICompatibility' -e 'standard library' ; continue ;;
            *) tb_die "unknown plot set $pl" ;;
        esac
        root -l -b -q "time_xy_plots.C+(\"$CSV\", \"$P\", \"$o\", \"$spec\", \"${SL[$pl]}\", $ALIGN, \"$GEO\", \"$MASKED_CHANNELS\")" 2>&1 |
            grep -v -e '^Info in' -e '^Processing' -e 'CheckABICompatibility' -e 'standard library'
    done
done
tb_info "plots in $OUT"
