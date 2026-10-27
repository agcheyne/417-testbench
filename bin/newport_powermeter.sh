#! /bin/bash
# Query the Newport 1830-R power meter (USB 104d:cec7).
#
# Usage: ./newport_powermeter.sh                     -> prints one power reading (W)
#        ./newport_powermeter.sh CMD [CMD ...]       -> sends each command in order; prints the reply
#                                                       of every query (commands ending in ?)
# Useful commands (quote anything with * or ?):
#   PM:Power?                  power reading in the current units (W with PM:UNITS 2)
#   'PM:Lambda 405' PM:Lambda? set / read the wavelength correction [nm], 200-1100 with this head
#   PM:AUTO? PM:RANGE?         autorange on/off, present range
#   PM:ATT?                    attenuator (OD3 cap) in use: set PM:ATT 1 when it is fitted
#   PM:DIGITALFILTER? PM:ZERO? filter mode, zero (offset) subtraction
#   '*IDN?' ERRSTR? 'Q?'       identity, oldest error, status byte
# The display only refreshes every ~0.3-2 s: space repeated readings at least 0.5 s apart.

case $1 in -h|--help) sed -n '2,15p' "$0" | sed 's/^# \{0,1\}//' ; exit 0 ;; esac

[ $# -eq 0 ] && set -- "PM:Power?"

if ! lsusb -d 104d:cec7 > /dev/null; then
    echo "ERROR: Newport power meter not connected" >&2
    exit 1
fi

python3 - "$@" <<'EOF'
import sys, ctypes as C

VID, PID, EP_OUT, EP_IN, TMO = 0x104D, 0xCEC7, 0x02, 0x81, 1000
LIBUSB_ERROR_TIMEOUT = -7

lib = C.CDLL("libusb-1.0.so.0")
lib.libusb_open_device_with_vid_pid.restype = C.c_void_p
lib.libusb_open_device_with_vid_pid.argtypes = [C.c_void_p, C.c_uint16, C.c_uint16]
for f in ("libusb_set_auto_detach_kernel_driver", "libusb_claim_interface", "libusb_release_interface"):
    getattr(lib, f).argtypes = [C.c_void_p, C.c_int]
lib.libusb_bulk_transfer.argtypes = [C.c_void_p, C.c_ubyte, C.c_char_p, C.c_int, C.POINTER(C.c_int), C.c_uint]
lib.libusb_close.argtypes = [C.c_void_p]

ctx = C.c_void_p()
lib.libusb_init(C.byref(ctx))
h = lib.libusb_open_device_with_vid_pid(ctx, VID, PID)
if not h:
    sys.exit("ERROR: power meter is connected but cannot be opened: no permission (install the udev rule, see header)")
lib.libusb_set_auto_detach_kernel_driver(h, 1)
if lib.libusb_claim_interface(h, 0) != 0:
    sys.exit("ERROR: cannot claim USB interface (in use by another program?)")

def bulk(ep, buf, n, tmo):
    got = C.c_int(0)
    rc = lib.libusb_bulk_transfer(h, ep, buf, n, C.byref(got), tmo)
    return rc, got.value

# drain stale data
buf = C.create_string_buffer(64)
while bulk(EP_IN, buf, 64, 50)[1] > 0:
    pass

for cmd in sys.argv[1:]:
    out = (cmd + "\r").encode()
    rc, _ = bulk(EP_OUT, out, len(out), TMO)
    if rc != 0:
        sys.exit(f"ERROR: write failed for {cmd} (libusb rc={rc})")
    if "?" not in cmd:
        continue
    reply = b""
    while not reply.endswith((b"\r", b"\n")):
        rc, n = bulk(EP_IN, buf, 64, TMO)
        reply += buf.raw[:n]
        if rc == LIBUSB_ERROR_TIMEOUT or (rc != 0 and n == 0):
            break
    print(reply.decode(errors="ignore").strip())

lib.libusb_release_interface(h, 0)
lib.libusb_close(h)
lib.libusb_exit(ctx)
EOF
