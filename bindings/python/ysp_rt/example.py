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
print("lateness over 200 waits: median %.1f us, p99 %.1f us, max %.1f us"
      % (statistics.median(late) * 1e6, late[197] * 1e6, late[-1] * 1e6))

# Another clock (here Python's perf_counter; PsychoPy's core.getTime works
# the same way) as one pair of readings.
c = rt.correlate(time.perf_counter)
print("perf_counter %.9f s was ysp time %.9f s, +- %.0f ns" % (c.other, c.t, c.width / 2 * 1e9))
print("so a perf_counter stamp s maps to s + %.9f" % (c.t - c.other))

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
print("fit: %d points over %.1f s, %+.1f ppm, spread %.0f us"
      % (info["points"], info["span"], info["ppm"], info["spread"] * 1e6))
print("an event at micros() = 4010000500 happened at ysp time %.6f s (truth 110 s)" % fit.map(4010000500))
