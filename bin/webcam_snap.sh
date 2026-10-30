#! /bin/bash
#
# One webcam picture, kept in logs/webcam/<date>/<date>_<time>.jpg. Meant for cron (hourly), so the
# camera does not have to stream all the time.
#
#   webcam_snap.sh                    take one picture now
#   webcam_snap.sh --install [MIN]    add an hourly crontab line (at MIN past the hour, default 0)
#   webcam_snap.sh --uninstall        remove that crontab line
#
# If bin/webcam.py is serving (it holds the camera), the picture is its latest frame from
# http://localhost:$WEBCAM_PORT/snapshot.jpg; otherwise the camera is opened for about 3 s
# (45 frames, so the auto exposure settles) and closed again.
# Environment: WEBCAM_PORT (default 8080), WEBCAM_DEVICE (default /dev/video0).

. "$(dirname "$(readlink -f "$0")")/../lib/tb.sh"
PORT=${WEBCAM_PORT:-8080}
DEVICE=${WEBCAM_DEVICE:-/dev/video0}
SELF=$(readlink -f "$0")
TAG="# 417-testbench webcam_snap"

case $1 in
    --install)
        min=${2:-0}
        [[ $min =~ ^[0-9]+$ ]] && (( min < 60 )) || tb_die "minute must be 0-59"
        { crontab -l 2> /dev/null | grep -v -F "$TAG" ; echo "$min * * * * $SELF >> $TB_LOGS/webcam/webcam_snap.log 2>&1 $TAG" ; } | crontab -
        crontab -l | grep -F "$TAG"
        exit ;;
    --uninstall)
        crontab -l 2> /dev/null | grep -v -F "$TAG" | crontab -
        echo "removed" ; exit ;;
    -h|--help) sed -n '3,13p' "$0" | sed 's/^# \{0,1\}//' ; exit ;;
    "") ;;
    *) tb_die "unknown option $1 (-h for help)" ;;
esac

now=$(date '+%Y%m%d_%H%M%S')
dir=$TB_LOGS/webcam/${now%_*}
mkdir -p "$dir" || tb_die "cannot make $dir"
out=$dir/$now.jpg

if curl -s -f -m 5 -o "$out" "http://localhost:$PORT/snapshot.jpg" && [ -s "$out" ] ; then
    how="from webcam.py"
else
    rm -f "$out"
    tmp=$(mktemp -d) || exit 1
    timeout 20 gst-launch-1.0 -q v4l2src device="$DEVICE" num-buffers=45 ! image/jpeg,width=1280,height=720 \
        ! multifilesink location="$tmp/f%02d.jpg" > /dev/null 2>&1
    last=$(ls "$tmp"/f*.jpg 2> /dev/null | tail -1)
    [ -n "$last" ] && mv "$last" "$out"
    rm -rf "$tmp"
    [ -s "$out" ] || tb_die "$now: no picture (camera $DEVICE busy or absent?)"
    how="from $DEVICE"
fi
echo "$now: $out ($(stat -c %s "$out") bytes, $how)"
