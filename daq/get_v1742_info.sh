#! /bin/bash

. "$(dirname "$(readlink -f "$0")")/../lib/tb.sh"
tb_site
export ANC_PACKETS
mkdir -p "$ANC_PACKETS"

> $ANC_PACKETS/v1742info.txt
NODE=0
for LINK in {0..0} ; do # add more links if you have more than one V1742
    echo -n "$LINK  $NODE " >> $ANC_PACKETS/v1742info.txt
    caen_client -d $LINK -n $NODE identify >> $ANC_PACKETS/v1742info.txt
done

grep 'Serial Number' $ANC_PACKETS/v1742info.txt | awk '{print $NF}' > $ANC_PACKETS/v1742info_numeric.txt