"""ysp.rt: frames on the ysp clock, another clock mapped to it, and a device
clock fit.

    python example.py

The device in the last part is simulated, so its numbers describe the fit,
not a device.
"""
import random
import statistics
import time

import ysp.rt as rt

print(rt.describe())   # log this line beside timing data

# 200 frames of 5 ms against a running deadline, so lateness cannot add up.
late = []
t = rt.now()
for i in range(200):
    t += 0.005
    late.append(rt.sleep_until(t))
late.sort()
print(f"lateness over 200 waits: median {statistics.median(late) * 1e6:.1f} us, p99 {late[197] * 1e6:.1f} us, "
      f"max {late[-1] * 1e6:.1f} us")

# Another clock (here Python's perf_counter; PsychoPy's core.getTime works
# the same way) as one pair of readings.
c = rt.correlate(time.perf_counter)
print(f"perf_counter {c.other:.9f} s was ysp time {c.t:.9f} s, +- {c.width / 2 * 1e9:.0f} ns")
print(f"so a perf_counter stamp s maps to s + {c.t - c.other:.9f}")

# A board's micros() clock, read over a USB link that delivers each report 0
# to 1 ms late (simulated). It runs 50 ppm fast, so its tick is 50 ppm short:
# info["ppm"] is -50.
fit = rt.Fit("late", tick=1e-6, bits=32)
rng = random.Random(3)
host = 100.0
for i in range(2000):
    true_us = int((host - 100.0) * 1e6 * (1 + 50e-6)) + 4_000_000_000
    fit.add(true_us % (1 << 32), host + rng.random() * 0.001)
    host += 0.01
info = fit.info()
print(f"fit: {info['points']:d} points over {info['span']:.1f} s, {info['ppm']:+.1f} ppm, spread {info['spread'] * 1e6:.0f} us")
print(f"an event at micros() = 4010000500 happened at ysp time {fit.map(4010000500):.6f} s (truth 110 s)")
