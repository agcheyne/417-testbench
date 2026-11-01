# Relock script of the bench panel (embedded into bench.bob by make_bench_panel.py; Jython).
# PVs: 0 loc://unlocked$(DID), 1 a 1 s ticker, 2 loc://relock_left$(DID), 3 loc://last_write$(DID).
# The window locks again 5 min after unlocking or after the last write, whichever is later. The
# writes are seen by the touch scripts (touch.py), one per IOC, which put the time of any setpoint
# change into loc://last_write$(DID): Phoebus runs a script only once all its PVs are connected, so
# one IOC being down must not stop the relock. Any window's write counts: the setpoints are shared.
from org.csstudio.display.builder.runtime.script import PVUtil
import time

RELOCK_S = 300
now = time.time()
try:
    unlocked = PVUtil.getLong(pvs[0]) == 1
except Exception:
    unlocked = False
try:
    last_write = PVUtil.getDouble(pvs[3])
except Exception:
    last_write = 0.0
was = widget.getUserData("relock_was")
since = widget.getUserData("relock_since") or 0.0
if unlocked and not was:
    since = now
widget.setUserData("relock_since", since)
widget.setUserData("relock_was", unlocked)
left = 0
if unlocked:
    left = int(RELOCK_S - (now - max(since, last_write)))
    if left <= 0:
        left = 0
        pvs[0].write(0)
pvs[2].write(left)
