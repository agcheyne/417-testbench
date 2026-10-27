#! /bin/bash

. "$(dirname "$(readlink -f "$0")")/../lib/tb.sh"
tb_site
export ANC_PACKETS
mkdir -p "$ANC_PACKETS"

> $ANC_PACKETS/thorinfo.txt
caget THOR_X:read_pos >> $ANC_PACKETS/thorinfo.txt
caget THOR_Y:read_pos >> $ANC_PACKETS/thorinfo.txt
caget THOR_X:read_stat >> $ANC_PACKETS/thorinfo.txt
caget THOR_Y:read_stat >> $ANC_PACKETS/thorinfo.txt

# 931 (kept as hpDIRC had it): Y then X as read; rcdaq reads integers, so 57.5 mm is stored as 57
grep 'THOR_Y:read_pos' $ANC_PACKETS/thorinfo.txt | awk '{print $NF}' > $ANC_PACKETS/thorinfo_numeric.txt
grep 'THOR_X:read_pos' $ANC_PACKETS/thorinfo.txt | awk '{print $NF}' >> $ANC_PACKETS/thorinfo_numeric.txt

# 932: X then Y in micrometres (full precision)
awk '$1 == "THOR_X:read_pos" { x = $NF } $1 == "THOR_Y:read_pos" { y = $NF }
     END { if (x != "" && y != "") printf "%d\n%d\n", x * 1000 + (x < 0 ? -0.5 : 0.5), y * 1000 + (y < 0 ? -0.5 : 0.5) }' \
    $ANC_PACKETS/thorinfo.txt > $ANC_PACKETS/thorinfo_um.txt
exit 0
