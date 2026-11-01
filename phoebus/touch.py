# Touch script of the bench panel (embedded by make_bench_panel.py; Jython): any change of its
# setpoint PVs (1...) puts the time into PV 0, loc://last_write$(DID), for relock.py.
import time
pvs[0].write(time.time())
