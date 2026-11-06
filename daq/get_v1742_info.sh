#! /bin/bash
#
# begin-run packets 920 / 921: identity and serial of each V1742 read out
#   get_v1742_info.sh [LINK:NODE ...]     default 0:0 (one board, link 0 node 0)
# one block per board in 920, one serial per line in 921, in the order given

. "$(dirname "$(readlink -f "$0")")/../lib/tb.sh"
tb_site
export ANC_PACKETS
mkdir -p "$ANC_PACKETS"

> $ANC_PACKETS/v1742info.txt
for B in ${@:-0:0} ; do
    LINK=${B%:*}
    NODE=${B#*:}
    echo -n "$LINK  $NODE " >> $ANC_PACKETS/v1742info.txt
    caen_client -d $LINK -n $NODE identify >> $ANC_PACKETS/v1742info.txt
done

grep 'Serial Number' $ANC_PACKETS/v1742info.txt | awk '{print $NF}' > $ANC_PACKETS/v1742info_numeric.txt