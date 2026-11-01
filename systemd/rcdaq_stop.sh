#! /bin/bash
# ExecStop of rcdaq-server.service: end a run that is going (so its file is closed properly), then
# ask the server to shut down; systemd sends SIGTERM only if it is still there afterwards.
LOG=$(dirname "$(dirname "$(readlink -f "$0")")")/logs/daq_commands.log
run=$(rcdaq_client daq_status -s 2> /dev/null | awk '{ print $1 }')
if [ -n "$run" ] && [ "$run" != -1 ] ; then
    echo "$(date -Iseconds) rcdaq-server stop: ending run $run first" >> "$LOG"
    timeout 30 rcdaq_client daq_end > /dev/null 2>&1
fi
timeout 10 rcdaq_client daq_shutdown > /dev/null 2>&1
exit 0
