#! /bin/bash

. "$(dirname "$(readlink -f "$0")")/../lib/tb.sh"
tb_site
export ANC_PACKETS
mkdir -p "$ANC_PACKETS"

BIN=$TB_ROOT/bin

# CAEN N1470 HV readback of all units (read-only). A unit that cannot be read (e.g. GECO2020
# has its port open) is marked FAILED in the text packet and left out of the numeric one.
# Reading both units takes ~4 s, during which the V1742 is armed but not read out, so this takes
# the readings of the last n1470.sh check (the minute timer) when they are at most CACHE_MAX_AGE s
# old, and asks the units only when there are none (timer stopped, or the HV just switched).
CACHE=$TB_STATE/hv_status.txt
CACHE_MAX_AGE=150
> $ANC_PACKETS/hvinfo.txt
> $ANC_PACKETS/hvinfo_numeric.txt
date >> $ANC_PACKETS/hvinfo.txt
# age from the time the check wrote in its first line: the file's own time comes from the NFS server
# (npls0), whose clock was ~2 min behind this machine's on 31 Oct 2026
t=$(head -1 "$CACHE" 2> /dev/null | sed -n 's/^n1470.sh -u all check at //p')
age=$(( $(date +%s) - $(date -d "${t:-1970-01-01}" +%s 2> /dev/null || echo 0) ))
# ... and only if it read every unit (a check that found a port busy, e.g. while the IOC started,
# would otherwise give a packet without readings)
if [ $age -le $CACHE_MAX_AGE ] && ! grep -q '^n1470 \[' "$CACHE" ; then
    echo "readings of the last HV check, $age s before this run ($CACHE):" >> $ANC_PACKETS/hvinfo.txt
    cat "$CACHE" >> $ANC_PACKETS/hvinfo.txt
elif ! timeout 40 "$BIN/n1470.sh" -u all status >> $ANC_PACKETS/hvinfo.txt 2>&1 ; then
    echo "HV readback FAILED for at least one unit (see above)" >> $ANC_PACKETS/hvinfo.txt
fi

# numeric packet (integers only): one block of 21 per unit read out completely, in "-u all" order:
#   serial number, then per channel 0-3: VSET [0.1 V], VMON [0.1 V], ISET [0.01 uA], IMON [0.01 uA], STAT bits
awk 'function flush() { if (sn != "" && n == 4) printf "%s", blk ; sn = "" }
     $1 == "BDSNUM" { flush() ; sn = $2 ; n = 0 ; blk = sprintf("%d\n", $2) ; next }
     sn != "" && $1 ~ /^[0-9]+$/ && NF >= 10 {
         blk = blk sprintf("%d\n%d\n%d\n%d\n%d\n", $2*10+0.5, $3*10+0.5, $4*100+0.5, $5*100+0.5, $10) ; n++ }
     END { flush() }' $ANC_PACKETS/hvinfo.txt >> $ANC_PACKETS/hvinfo_numeric.txt
exit 0
