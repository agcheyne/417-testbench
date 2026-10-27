#! /bin/bash
#
# rcdaq setup for the room-417 test bench: run types, begin-run readback packets, and the
# CAEN V1742 configured from the selected DAQ profile (testbench.sh use daq PROFILE).
#
#   setup_rcdaq.sh              configure rcdaq and the V1742 (starts rcdaq_server if needed)
#   setup_rcdaq.sh --dry-run    print every rcdaq_client / caen_client command instead
#
# Assumes PATH and LD_LIBRARY_PATH already point at rcdaq (as for hpDIRC_setup.sh).
#
# Begin-run packets (event type 9):
#   900        this script
#   910 / 911  test-bench db: detector, HV table, cabling, DAQ profile, scan point (get_testbench_info.sh)
#   920 / 921  V1742 identity / serial                  (get_v1742_info.sh)
#   930 / 931  stage position text / integer mm (Y, X)  (get_motor_xy_info.sh)
#   932        stage position X, Y in micrometres       (get_motor_xy_info.sh)
#   940 / 941  CAEN N1470 HV readback                   (get_hv_info.sh)
#   950 / 951  PiLas laser readback                     (get_laser_info.sh)
# Data: 2000 + 10*link + node = V1742 waveforms, every event.

. "$(dirname "$(readlink -f "$0")")/../lib/tb.sh"
MYSELF=$(readlink -f "$0")
HERE=$(dirname "$MYSELF")

case $1 in
    "")        RC=rcdaq_client CC=caen_client ;;
    --dry-run) RC="echo rcdaq_client" CC="echo caen_client" ;;
    *)         sed -n '3,20p' "$0" | sed 's/^# \{0,1\}//' ; exit 2 ;;
esac

tb_site
tb_active
tb_conf "$(tb_daq_file)"
[ "$READOUT" == v1742 ] || tb_die "DAQ profile $TB_DAQ is READOUT=$READOUT: setup_rcdaq.sh only drives the V1742"
ls "$CALIB_DIR"/calib_*_"$CALIB_SPEED".dat > /dev/null 2>&1 ||
    tb_warn "no calibration calib_*_$CALIB_SPEED.dat in $CALIB_DIR: the data cannot be calibrated"
export ANC_PACKETS
mkdir -p "$ANC_PACKETS"
echo "setting up rcdaq for detector $TB_DETECTOR, DAQ profile $TB_DAQ ($DESCRIPTION)"

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

$RC daq_set_compression 4
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

$RC create_device device_command 9 0 "$HERE/get_v1742_info.sh"
$RC create_device device_file_delete 9 920 "$ANC_PACKETS/v1742info.txt"
$RC create_device device_filenumbers_delete 9 921 "$ANC_PACKETS/v1742info_numeric.txt"

$RC create_device device_command 9 0 "$HERE/get_motor_xy_info.sh"
$RC create_device device_file_delete 9 930 "$ANC_PACKETS/thorinfo.txt"
$RC create_device device_filenumbers_delete 9 931 "$ANC_PACKETS/thorinfo_numeric.txt"
$RC create_device device_filenumbers_delete 9 932 "$ANC_PACKETS/thorinfo_um.txt"

# CAEN N1470 HV (read-only; a unit whose port GECO2020 holds is marked FAILED)
$RC create_device device_command 9 0 "$HERE/get_hv_info.sh"
$RC create_device device_file_delete 9 940 "$ANC_PACKETS/hvinfo.txt"
$RC create_device device_filenumbers_delete 9 941 "$ANC_PACKETS/hvinfo_numeric.txt"

# PiLas laser (read-only; FAILED in 950 if the PiLas GUI has the port open)
$RC create_device device_command 9 0 "$HERE/get_laser_info.sh"
$RC create_device device_file_delete 9 950 "$ANC_PACKETS/laserinfo.txt"
$RC create_device device_filenumbers_delete 9 951 "$ANC_PACKETS/laserinfo_numeric.txt"

# --- CAEN V1742 ------------------------------------------------------------------------
$RC load librcdaqplugin_CAENdrs.so

TRIGGER=1
NODE=0
LINK=0
PACKET=$((2000 + 10 * LINK + NODE))
echo "V1742 link $LINK node $NODE -> packet $PACKET"

# the device constructor does a full CAEN_DGTZ_Reset(), which also resets the TR0/TR1
# comparator DACs: everything below has to come after it
$RC create_device device_CAENdrs 1 $PACKET $LINK $NODE $TRIGGER
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
$CC -d $LINK -n $NODE SetFastTriggerDigitizing $FAST_TRIGGER_DIGITIZING
$CC -d $LINK -n $NODE SetFastTriggerMode 1
# NIM pulses are negative-going: 1 = negative polarity
$CC -d $LINK -n $NODE SetTriggerPolarity $TRIG_POLARITY

# read back what the board actually took (register writes confirm nothing by themselves)
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
echo "======================================================================="

$RC daq_set_runtype test
$RC daq_set_maxevents 0     # 0 = no limit (scan_xy.sh sets its own per run)

$RC daq_open
[ "$RC" == rcdaq_client ] && tb_log "setup_rcdaq: detector $TB_DETECTOR, DAQ $TB_DAQ"
exit 0
