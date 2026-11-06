#! /bin/bash
#
# rcdaq setup for the room-417 test bench: run types, begin-run readback packets, and one or two
# CAEN V1742 (DRS4) boards, all configured the same from the selected DAQ profile (testbench.sh use daq PROFILE).
#
#   setup_rcdaq.sh              configure rcdaq and the V1742s (starts rcdaq_server if needed)
#   setup_rcdaq.sh --dry-run    print every rcdaq_client / caen_client command instead
#
# The boards read are the packets of the active detector's cabling.txt (2000 + 10*link + node: 2000 =
# node 0, s/n 49, the original board; 2001 = node 1, s/n 51240, added 5 Nov 2026, daisy-chained on
# optical link 0); a cabling.txt without packets reads board 0 alone. TB_V1742_BOARDS="0:0 0:1"
# (LINK:NODE) in the environment overrides it, e.g. to look at a board nothing is mapped to.
# The first board gives the rcdaq trigger; the others are read after it and wait until they have their
# event, so EVERY board read needs the TR0 NIM trigger: if one gets none, the DAQ hangs.
# Start chain (V1742_START_CHAIN in testbench.conf; LEMO fitted 6 Nov 2026): node 0 TRG-OUT -> node 1
# S-IN. Node 0's TRG-OUT gives its RUN level and node 1 starts on it, so both start on the same edge and
# no laser pulse can reach only one board (software start: ~5 % of runs one pulse apart).
#
# Assumes PATH and LD_LIBRARY_PATH already point at rcdaq (as for hpDIRC_setup.sh).
#
# Begin-run packets (event type 9):
#   900        this script
#   901, 902   V1742 factory calibration, one per board in packet order (device_CAENcalibration)
#   910 / 911  test-bench db: detector, HV table, cabling, DAQ profile, scan point (get_testbench_info.sh)
#   920 / 921  V1742 identity / serial, every board     (get_v1742_info.sh)
#   930 / 931  stage position text / 2, X, Y [um]         (get_motor_xy_info.sh)
#   940 / 941  CAEN N1470 HV readback                   (get_hv_info.sh)
#   950 / 951  PiLas laser readback                     (get_laser_info.sh)
# Data: 2000 + 10*link + node = V1742 waveforms, every event: 2000 node 0, 2001 node 1.

. "$(dirname "$(readlink -f "$0")")/../lib/tb.sh"
MYSELF=$(readlink -f "$0")
HERE=$(dirname "$MYSELF")

case $1 in
    "")        RC=rcdaq_client CC=caen_client ;;
    --dry-run) RC="echo rcdaq_client" CC="echo caen_client" ;;
    *)         sed -n '3,29p' "$0" | sed 's/^# \{0,1\}//' ; exit 2 ;;
esac

tb_site
tb_active
tb_conf "$(tb_daq_file)"
[ "$READOUT" == v1742 ] || tb_die "DAQ profile $TB_DAQ is READOUT=$READOUT: setup_rcdaq.sh only drives the V1742"

# the boards, as LINK:NODE in packet order: from the cabling (see the top); the first one gives the trigger
CABLING=$(tb_detector_dir)/cabling.txt
BOARDS=$(tb_table "$CABLING" 2> /dev/null | awk '{ print $1 }' | sort -nu |
         awk '$1 >= 2000 && $1 < 2100 && $1 % 10 < 8 { printf "%s%d:%d", n++ ? " " : "", int(($1 - 2000) / 10), $1 % 10 }')
OTHER=$(tb_table "$CABLING" 2> /dev/null | awk '!($1 >= 2000 && $1 < 2100 && $1 % 10 < 8) { print $1 }' | sort -nu | tr '\n' ' ')
[ -n "$OTHER" ] && tb_warn "cabling.txt of $TB_DETECTOR has packets that are no V1742 (2000 + 10*link + node): $OTHER- left out"
BOARDS=${TB_V1742_BOARDS:-${BOARDS:-0:0}}
NB=$(wc -w <<< "$BOARDS")
# 1 = the boards are chained TRG-OUT -> S-IN in packet order (the first starts the others); 0 = no
# cable, every board started by software (without the cable a chained board never starts)
START_CHAIN=${V1742_START_CHAIN:-1}

export ANC_PACKETS
mkdir -p "$ANC_PACKETS"
echo "setting up rcdaq for detector $TB_DETECTOR, DAQ profile $TB_DAQ ($DESCRIPTION), $NB V1742 (LINK:NODE $BOARDS)"

# every board must answer, and has its own calibration (calib_<serial>_<speed>.dat)
if [ "$RC" == rcdaq_client ] ; then
    for B in $BOARDS ; do
        SERIAL=$(caen_client -d ${B%:*} -n ${B#*:} identify 2>&1 | awk '/Serial Number/ { print $NF }')
        [ -n "$SERIAL" ] || tb_die "V1742 link ${B%:*} node ${B#*:} does not answer: check the optical ring (LINK LEDs)"
        ls "$CALIB_DIR"/calib_$(printf %04d $SERIAL)_"$CALIB_SPEED".dat > /dev/null 2>&1 ||
            tb_warn "no calibration calib_$(printf %04d $SERIAL)_$CALIB_SPEED.dat in $CALIB_DIR: board s/n $SERIAL cannot be calibrated"
    done
fi

if [ "$RC" == rcdaq_client ] ; then
    if ! rcdaq_client daq_status > /dev/null 2>&1 ; then
        echo "No rcdaq_server running, starting... log goes to $HOME/rcdaq.log"
        rcdaq_server > $HOME/rcdaq.log 2>&1 &
        sleep 2
    fi
    S=$(rcdaq_client daq_status -s | awk '{ print $1 }')
    [ "$S" == -1 ] || tb_die "rcdaq is taking run $S: end it first (rcdaq_client daq_end)"
fi

# one shared run-number file, so run numbers never repeat
$RC daq_setrunnumberfile "$RUNNUMBER_FILE"

# file compression from the DAQ profile: 0 none, 1-3 LZO (saves ~2 % on V1742 data), 4 bzip2
$RC daq_set_compression ${COMPRESSION:-4}
$RC daq_set_nr_threads 10

# run types: junk, test, and the RUNTYPE of every detector in the db
for T in junk test $(for D in $(tb_detector_names) ; do RUNTYPE= ; tb_conf "$(tb_detector_dir $D)/detector.conf" ; echo $RUNTYPE ; done | sort -u) ; do
    $RC daq_define_runtype $T "$DATA_ROOT/$T/$T-%08d-%04d.evt"
    [ "$RC" == rcdaq_client ] && { mkdir -p "$DATA_ROOT/$T" || tb_die "cannot create $DATA_ROOT/$T" ; }
done

$RC daq_clear_readlist

# --- begin-run packets --------------------------------------------------------------
$RC create_device device_file 9 900 "$MYSELF"

$RC create_device device_command 9 0 "$HERE/get_testbench_info.sh"
$RC create_device device_file_delete 9 910 "$ANC_PACKETS/tbinfo.txt"
$RC create_device device_filenumbers_delete 9 911 "$ANC_PACKETS/tbinfo_numeric.txt"

$RC create_device device_command 9 0 "$HERE/get_v1742_info.sh $BOARDS"
$RC create_device device_file_delete 9 920 "$ANC_PACKETS/v1742info.txt"
$RC create_device device_filenumbers_delete 9 921 "$ANC_PACKETS/v1742info_numeric.txt"

$RC create_device device_command 9 0 "$HERE/get_motor_xy_info.sh"
$RC create_device device_file_delete 9 930 "$ANC_PACKETS/thorinfo.txt"
$RC create_device device_filenumbers_delete 9 931 "$ANC_PACKETS/thorinfo_numeric.txt"

# CAEN N1470 HV (read-only; a unit whose port GECO2020 holds is marked FAILED)
$RC create_device device_command 9 0 "$HERE/get_hv_info.sh"
$RC create_device device_file_delete 9 940 "$ANC_PACKETS/hvinfo.txt"
$RC create_device device_filenumbers_delete 9 941 "$ANC_PACKETS/hvinfo_numeric.txt"

# PiLas laser (read-only; FAILED in 950 if the PiLas GUI has the port open)
$RC create_device device_command 9 0 "$HERE/get_laser_info.sh"
$RC create_device device_file_delete 9 950 "$ANC_PACKETS/laserinfo.txt"
$RC create_device device_filenumbers_delete 9 951 "$ANC_PACKETS/laserinfo_numeric.txt"

# --- CAEN V1742s -----------------------------------------------------------------------
$RC load librcdaqplugin_CAENdrs.so

TRIGGER=1   # first board only
CALIB=901   # calibration packets 901, 902, ... in the board order
for B in $BOARDS ; do
    LINK=${B%:*}
    NODE=${B#*:}
    PACKET=$((2000 + 10 * LINK + NODE))
    echo "V1742 link $LINK node $NODE -> packet $PACKET, calibration $CALIB$( [ $TRIGGER == 1 ] && echo ', gives the trigger')"

    # the board's factory DRS4 calibration (all 3 speeds) into the begin-run event, tagged
    # with the serial and the data packet it is for (0 = optical link); read with caen_calibration
    $RC create_device device_CAENcalibration 9 $CALIB $LINK $NODE 0 $PACKET
    CALIB=$((CALIB + 1))

    # the device constructor does a full CAEN_DGTZ_Reset(), which also resets the TR0/TR1
    # comparator DACs (and the start mode and front-panel I/O): everything below has to come after it
    $RC create_device device_CAENdrs 1 $PACKET $LINK $NODE $TRIGGER
    TRIGGER=0
    $CC -d $LINK -n $NODE init
    $CC -d $LINK -n $NODE SetRecordLength $RECORD_LENGTH
    $CC -d $LINK -n $NODE SetPostTriggerSize $POST_TRIGGER
    $CC -d $LINK -n $NODE SetDRS4Correction $DRS4_CORRECTION

    # TR0/TR1 fast trigger (NIM on TRn): DC offset, then threshold DAC = 2^14 - threshold
    #   0x39B0 = 18k, 0x37BC = 18.5k, 0x35C8 = 19k, 0x33D4 = 19.5k, 0x31E0 = 20k,
    #   0x3D98 = 17k, 0x4180 = 16k, 0x4568 = 15k, 0x4A30 = 14k, 0x4D38 = 13k, 0x58F0 = 10k
    $CC -d $LINK -n $NODE WriteRegister 0x80DC $TRIG_DC_OFFSET
    $CC -d $LINK -n $NODE WriteRegister 0x80D4 $TRIG_THRESHOLD

    $CC -d $LINK -n $NODE SetDRS4SamplingFrequency $FREQ_CODE
    # the post-trigger register depends on the sampling frequency: set it again now. Set only
    # before (29 Oct 2026, 520 samples), POST_TRIGGER 100 read back 250 and the trigger fell
    # outside the record; set after, it reads back 106 and the trigger is 50 ns in
    $CC -d $LINK -n $NODE SetPostTriggerSize $POST_TRIGGER
    $CC -d $LINK -n $NODE SetFastTriggerDigitizing $FAST_TRIGGER_DIGITIZING
    $CC -d $LINK -n $NODE SetFastTriggerMode 1
    # NIM pulses are negative-going: 1 = negative polarity
    $CC -d $LINK -n $NODE SetTriggerPolarity $TRIG_POLARITY

    # start chain (two or more boards): every board puts its RUN level on TRG-OUT (front-panel I/O
    # 0x811C: bits 17:16 = 01 motherboard probe, 19:18 = 00 RUN; bit 0 = 0 NIM), and every board
    # after the first starts on S-IN. rcdaq arms the chained ones first (CAENdrs patch). One board
    # alone keeps the software start the reset left it on
    if [ $NB -gt 1 ] && [ $START_CHAIN == 1 ] ; then
        $CC -d $LINK -n $NODE WriteRegister 0x811C 0x10000
        [ "$B" != "${BOARDS%% *}" ] && $CC -d $LINK -n $NODE SetAcquisitionMode 1
    fi
done

# read back what each board actually took (register writes confirm nothing by themselves)
for B in $BOARDS ; do
    LINK=${B%:*}
    NODE=${B#*:}
    echo "=== CAEN V1742 (link $LINK node $NODE) after setup, profile $TB_DAQ ==="
    $CC -d $LINK -n $NODE status
    echo "--- TR comparator DACs per group (expect DC offset $TRIG_DC_OFFSET, threshold $TRIG_THRESHOLD) ---"
    for GRP in 0 1 2 3 ; do
        THRADDR=$(printf "0x1%dD4" $GRP)
        OFFADDR=$(printf "0x1%dDC" $GRP)
        echo -n "  group $GRP  threshold ($THRADDR): "
        $CC -d $LINK -n $NODE ReadRegister $THRADDR
        echo -n "  group $GRP  DC offset ($OFFADDR): "
        $CC -d $LINK -n $NODE ReadRegister $OFFADDR
    done
done
echo "======================================================================="

$RC daq_set_runtype test
$RC daq_set_maxevents 0     # 0 = no limit (scan_xy.sh sets its own per run)

$RC daq_open
[ "$RC" == rcdaq_client ] && tb_log "setup_rcdaq: detector $TB_DETECTOR, DAQ $TB_DAQ, V1742s $BOARDS"
exit 0
