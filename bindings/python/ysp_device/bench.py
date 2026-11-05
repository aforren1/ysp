"""ysp.device: the binding's own costs on simulated boards (docs/device.md,
Bindings): poll() per event, an event's t to wait() returning it, and
pulse() per call. Run it under the timing guard on a quiet machine:

    bash /c/tmp/psy-guard.sh time NAME python bench.py
"""
import sys
import time

import ysp.device as yd


def pct(v, p):
    v = sorted(v)
    return v[min(len(v) - 1, int(p / 100 * len(v)))]


def show(name, v, unit=1e6):
    print("%-28s n %5d  p50 %8.2f  p95 %8.2f  p99 %8.2f  max %8.2f us"
          % (name, len(v), pct(v, 50) * unit, pct(v, 95) * unit, pct(v, 99) * unit, max(v) * unit))


print(sys.version.split()[0], sys.platform)
# poll() building Events: 2000 events queued, then taken in one call
sim = yd.Sim("line", frame=0.0, tail=1e-6)
box = yd.Device("m_box", "line", sim.key, sim=sim, queue=4096)
assert box.wait_running(3.0)
costs = []
for rep in range(10):
    t0 = yd.now() + 0.02
    for i in range(1000):
        sim.press(t0 + i * 0.0002, 0.0001)
    while box.stats()["queue_events"] < (rep + 1) * 2000:
        time.sleep(0.01)
    a = time.perf_counter()
    ev = box.poll()
    b = time.perf_counter()
    assert len(ev) == 2000
    costs.append((b - a) / 2000)
show("poll() per event", costs)

# wait() wake: an event's t to the moment wait() returned it
lat = []
for i in range(300):
    t = yd.now() + 0.003
    sim.press(t, 0.001)
    ev = box.wait(1.0, max=1)
    got = yd.now()
    lat.append(got - ev[0].t)
    box.wait(1.0)
    time.sleep(0.002)
show("press t to wait() return", lat)
box.close()

# pulse(): the call on a simulated TriggerBox (host-timed end on the worker)
ts = yd.Sim("triggerbox")
trig = yd.Device("m_trig", "triggerbox", ts.key, sim=ts)
assert trig.wait_running(2.0)
calls = []
for i in range(600):
    a = time.perf_counter()
    trig.pulse(i & 0x7f, 0.001)
    calls.append(time.perf_counter() - a)
    time.sleep(0.003)
show("pulse() call", calls)
trig.close()
