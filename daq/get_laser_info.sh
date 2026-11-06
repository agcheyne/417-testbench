#! /bin/bash

. "$(dirname "$(readlink -f "$0")")/../lib/tb.sh"
tb_site
export ANC_PACKETS
mkdir -p "$ANC_PACKETS"

BIN=$TB_ROOT/bin

# PiLas laser readback (read-only), through its IOC. Fails harmlessly (FAILED) if the IOC is stopped.
> $ANC_PACKETS/laserinfo.txt
> $ANC_PACKETS/laserinfo_numeric.txt
date >> $ANC_PACKETS/laserinfo.txt
if ! timeout 20 "$BIN/pilas.sh" status >> $ANC_PACKETS/laserinfo.txt 2>&1 ; then
    echo "laser readback FAILED" >> $ANC_PACKETS/laserinfo.txt
    exit 0
fi

# numeric packet (integers only so decimals need conversion, e.g. 50.00% -> 5000), one value per line:
#   controller serial (trailing digits), wavelength [nm], pulsed emission (1 on), CW emission (1 on),
#   interlock (1 enabled), trigger source (0 int, 1 ext. adj., 2 ext. TTL), trigger edge (1 rising),
#   trigger level [mV], tune mode (1 auto), tune value [0.01 %], int. frequency [Hz],
#   ext. frequency [Hz], laser head temp [0.01 C], LD TEC (1 on), LD temp (1 good)
awk -F': ' '
    function num(s)  { return s + 0 }
    function rnd(x)  { return x < 0 ? int(x - 0.5) : int(x + 0.5) }
    { v[$1] = $2 }
    END {
        s = v["controller serial"] ; sub(/^.*[^0-9]/, "", s)
        printf "%d\n", s
        printf "%d\n", num(v["center wavelength"])
        printf "%d\n", v["laser emission"] == "on"
        printf "%d\n", v["CW emission"] == "on"
        printf "%d\n", v["interlock"] == "enabled"
        ts = v["trigger source"]
        printf "%d\n", ts ~ /TTL/ ? 2 : (ts ~ /ext/ ? 1 : 0)
        printf "%d\n", v["trigger edge"] == "rising"
        printf "%d\n", rnd(num(v["trigger level"]) * 1000)
        printf "%d\n", v["tune mode"] == "auto"
        printf "%d\n", rnd(num(v["tune value"]) * 100)
        printf "%d\n", num(v["int. frequency"])
        printf "%d\n", num(v["ext. frequency"])
        printf "%d\n", rnd(num(v["laser head temp."]) * 100)
        printf "%d\n", v["LD TEC state"] == "on"
        printf "%d\n", v["LD temp."] == "good"
    }' $ANC_PACKETS/laserinfo.txt > $ANC_PACKETS/laserinfo_numeric.txt
