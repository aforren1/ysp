"""ysp.rdk: a trial run step by step, logged, and regenerated from the log.

    python example.py

The rig side would run ysp/yrdk.h in C; here the same header runs through
the binding, so the example needs nothing but NumPy.
"""
import numpy as np

import ysp.rdk as yrdk

P = 16666667                     # ns per frame at 60 Hz (the rig logs ns)
T0 = 1_000_000_000
SEED = 2026

# The rig: a trial of 1 s at 25.6 % coherence that turns 90 degrees.
f = yrdk.Field(algorithm="MN", w=10, count=100, coherence=0.256, speed=5)
f.start(SEED, T0 / 1e9)          # seconds
log = []
for k in range(1, 61):
    f.direction = 0.0 if k < 40 else 90.0 * (k - 40) / 20.0
    f.update((T0 + k * P) / 1e9)  # seconds; f.last.ns is the exact ns
    log.append(f.last)
digest = f.digest()

# The analysis: the same desc, the seed and the log give the same dots.
g = yrdk.Field(algorithm="MN", w=10, count=100, coherence=0.256, speed=5)
xy, direction, signal = g.trajectory(SEED, T0 / 1e9, log)
print(f"updates {xy.shape[0]:d}, dots shown {xy.shape[1]:d}, digest {g.digest():016x}, the same: {g.digest() == digest}")
print(f"signal dots per update: mean {signal.sum(axis=1).mean():.1f} (the coherence times {g.n:d})")
print(f"first update's first dot: x {xy[0, 0, 0]:.4f}, y {xy[0, 0, 1]:.4f}, direction {direction[0, 0]:.2f} deg")
