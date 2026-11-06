#! /bin/bash
# ExecStartPost of rcdaq-server.service: wait until the new rcdaq_server answers, then configure it
# for the active detector and DAQ profile (daq/setup_rcdaq.sh: run types, readlist, the V1742), and
# select the detector's run type.
# Only on a server that is not taking a run (it never is, just after its start). One line to the
# command log the panel's Apply uses, with the full output only if it failed (a good setup's goes to
# logs/setup_rcdaq_last.log, overwritten). A failure leaves the server running, unconfigured.
TB=$(dirname "$(dirname "$(readlink -f "$0")")")
LOG=$TB/logs/daq_commands.log
for i in $(seq 40) ; do rcdaq_client daq_status > /dev/null 2>&1 && break ; sleep 0.5 ; done
run=$(rcdaq_client daq_status -s 2> /dev/null | awk '{ print $1 }')
if [ "$run" != -1 ] ; then
    echo "$(date -Iseconds) rcdaq-server start: not configured (rcdaq_server ${run:+run $run }not ready)" >> "$LOG"
    exit 1
fi
out=$(timeout 300 "$TB/daq/setup_rcdaq.sh" 2>&1) ; rc=$?
# a new server stands on the last run type setup_rcdaq.sh defined ("test"): select the detector's own
if [ $rc -eq 0 ] ; then
    RUNTYPE=$(. "$TB/lib/tb.sh" && tb_active && tb_conf "$(tb_detector_dir)/detector.conf" && echo "$RUNTYPE")
    [ -n "$RUNTYPE" ] && rcdaq_client daq_set_runtype "$RUNTYPE" > /dev/null 2>&1 && out+=$'\n'"run type: $RUNTYPE"
fi
echo "$(date -Iseconds) rcdaq-server start: setup_rcdaq.sh rc $rc ($(grep -m 1 '^setting up rcdaq' <<< "$out"))" >> "$LOG"
if [ $rc -eq 0 ] ; then printf '%s\n' "$out" > "$TB/logs/setup_rcdaq_last.log"
else sed 's/^/    /' <<< "$out" >> "$LOG" ; fi
exit $rc
