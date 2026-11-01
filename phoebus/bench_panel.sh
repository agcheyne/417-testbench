#! /bin/bash
#
# Open the bench panel (bench.bob) in Phoebus, on this PC's screen or a VNC display.
#   phoebus/bench_panel.sh
# A Phoebus already running in this display gets the panel as a new tab, with its own settings
# (bench.ini only applies to a Phoebus started here). The IOCs listen on 127.0.0.1 only: run it on
# np-bbox1 (from a laptop: through VNC).
# logging.properties keeps the terminal to warnings and errors (the known harmless start-up messages off).
HERE=$(dirname "$(readlink -f "$0")")
exec "$HOME/epics/product-4.7.3/phoebus.sh" -settings "$HERE/bench.ini" -logging "$HERE/logging.properties" -resource "$HERE/bench.bob" "$@"
