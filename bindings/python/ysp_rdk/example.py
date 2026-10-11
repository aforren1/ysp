"""ysp.rdk: a trial run step by step, logged, and regenerated from the log.

    python example.py

The rig side would run ysp/rdk.h in C; here the same header runs through
the binding, so the example needs nothing but NumPy.
"""
import numpy as np

import ysp.rdk as rdk

P = 16666667                     # ns per frame at 60 Hz (the rig logs ns)
T0 = 1_000_000_000
SEED = 2026

# The rig: a trial of 1 s at 25.6 % coherence that turns 90 degrees.
f = rdk.Field(algorithm="MN", w=10, count=100, coherence=0.256, speed=5)
f.start(SEED, T0 / 1e9)          # seconds
log = []
for k in range(1, 61):
    f.direction = 0.0 if k < 40 else 90.0 * (k - 40) / 20.0
    f.update((T0 + k * P) / 1e9)  # seconds; f.last.ns is the exact ns
    log.append(f.last)
digest = f.digest()

# The analysis: the same desc, the seed and the log give the same dots.
g = rdk.Field(algorithm="MN", w=10, count=100, coherence=0.256, speed=5)
xy, direction, signal = g.trajectory(SEED, T0 / 1e9, log)
print("updates %d, dots shown %d, digest %016x, the same: %s" % (xy.shape[0], xy.shape[1], g.digest(), g.digest() == digest))
print("signal dots per update: mean %.1f (the coherence times %d)" % (signal.sum(axis=1).mean(), g.n))
print("first update's first dot: x %.4f, y %.4f, direction %.2f deg" % (xy[0, 0, 0], xy[0, 0, 1], direction[0, 0]))
