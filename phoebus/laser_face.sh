#! /bin/bash
#
# Open laser_face.bob (the detector face with the laser on it, live) in Phoebus, on this PC's screen or
# a VNC display. Regenerate it after a db change: phoebus/make_laser_face.py
HERE=$(dirname "$(readlink -f "$0")")
exec "$HOME/epics/product-4.7.3/phoebus.sh" -settings "$HERE/bench.ini" -logging "$HERE/logging.properties" -resource "$HERE/laser_face.bob" "$@"
