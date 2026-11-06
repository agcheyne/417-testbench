#! /bin/bash
#
# Begin-run readback of the CAEN N1470 HV, through its IOC (n1470.sh -u all status, ~0.2 s):
#   940 (text)    the status of every unit; a unit that cannot be read is marked FAILED
#   941 (numbers) per unit read out completely, in "-u all" order: serial number, then per channel
#                 0-3: VSET [0.1 V], VMON [0.1 V], ISET [0.01 uA], IMON [0.01 uA], STAT bits

. "$(dirname "$(readlink -f "$0")")/../lib/tb.sh"
tb_site
mkdir -p "$ANC_PACKETS"
TXT=$ANC_PACKETS/hvinfo.txt
NUM=$ANC_PACKETS/hvinfo_numeric.txt

{ date ; timeout 10 "$TB_ROOT/bin/n1470.sh" -u all status 2>&1 || echo "HV readback FAILED for at least one unit (see above)" ; } > "$TXT"

awk 'function flush() { if (sn != "" && n == 4) printf "%s", blk ; sn = "" }
     $1 == "BDSNUM" { flush() ; sn = $2 ; n = 0 ; blk = sprintf("%d\n", $2) ; next }
     sn != "" && $1 ~ /^[0-9]+$/ && NF >= 10 {
         blk = blk sprintf("%d\n%d\n%d\n%d\n%d\n", $2*10+0.5, $3*10+0.5, $4*100+0.5, $5*100+0.5, $10) ; n++ }
     END { flush() }' "$TXT" > "$NUM"
exit 0
