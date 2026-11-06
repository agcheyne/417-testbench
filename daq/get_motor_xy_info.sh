#! /bin/bash

. "$(dirname "$(readlink -f "$0")")/../lib/tb.sh"
tb_site
export ANC_PACKETS
mkdir -p "$ANC_PACKETS"

# 930: the caget output as text
> $ANC_PACKETS/thorinfo.txt
caget THOR_X:read_pos >> $ANC_PACKETS/thorinfo.txt
caget THOR_Y:read_pos >> $ANC_PACKETS/thorinfo.txt
caget THOR_X:read_stat >> $ANC_PACKETS/thorinfo.txt
caget THOR_Y:read_stat >> $ANC_PACKETS/thorinfo.txt

# 931 (integers, rcdaq reads no others): format version 2, then X and Y in micrometres.
# Runs before 6 Nov 2026 have X, Y [um] in 932 and 931 as hpDIRC wrote it: 2 values, Y then X in
# whole mm. The format word (and the length) tells the two 931s apart (RunInfo in lib/tb_v1742.h).
awk '$1 == "THOR_X:read_pos" { x = $NF } $1 == "THOR_Y:read_pos" { y = $NF }
     END { if (x != "" && y != "") printf "2\n%d\n%d\n", x * 1000 + (x < 0 ? -0.5 : 0.5), y * 1000 + (y < 0 ? -0.5 : 0.5) }' \
    $ANC_PACKETS/thorinfo.txt > $ANC_PACKETS/thorinfo_numeric.txt
exit 0
