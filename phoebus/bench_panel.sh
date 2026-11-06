#! /bin/bash
#
# Open the bench panel (bench.bob) in Phoebus, on this PC's screen or a VNC display.
#   phoebus/bench_panel.sh
#
#   This script opens the bench panel in Phoebus.
#   The IOCs listen on 127.0.0.1 only: run it on np-bbox1
HERE=$(dirname "$(readlink -f "$0")")
exec "$HOME/epics/product-4.7.3/phoebus.sh" -settings "$HERE/bench.ini" -logging "$HERE/logging.properties" -resource "$HERE/bench.bob" "$@"
