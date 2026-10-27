#! /bin/bash

. "$(dirname "$(readlink -f "$0")")/../lib/tb.sh"
tb_site
export ANC_PACKETS
mkdir -p "$ANC_PACKETS"

BIN=$TB_ROOT/bin

# CAEN N1470 HV readback of all units (read-only). A unit that cannot be read (e.g. GECO2020
# has its port open) is marked FAILED in the text packet and left out of the numeric one.
> $ANC_PACKETS/hvinfo.txt
> $ANC_PACKETS/hvinfo_numeric.txt
date >> $ANC_PACKETS/hvinfo.txt
if ! timeout 40 "$BIN/n1470.sh" -u all status >> $ANC_PACKETS/hvinfo.txt 2>&1 ; then
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
