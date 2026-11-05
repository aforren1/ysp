#!/usr/bin/env python3
"""Minimal ysp.aep Python demo: a simulated 2-D observer, run twice.

1. The plain loop: next(), show, update(). Fine when next + update fit
   between trials.
2. The same session through ysp.aep.Async inside a 60 Hz frame loop: the
   inference runs on a C thread, and the loop polls for the next stimulus
   once per frame instead of blocking for an EAVC selection.

Build/install first (from this directory):
    pip install .
or build in place:
    python setup.py build_ext --inplace

Run:
    python example.py
"""
import math
import random
import time

import ysp.aep as yaep

# The observer: a 75% threshold that rises linearly with the context
# dimension, and a probit psychometric function along the intensity.
def p_yes(x):
    thr50 = 0.3 + 0.3 * x[0]
    return 0.5 * math.erfc(-8.0 * (x[1] - thr50) / math.sqrt(2.0))


def observe(x, u):
    p = p_yes(x)
    return yaep.simulate_outcome([1.0 - p, p], u.random())


DESC = dict(
    lo=[0.0, 0.0], hi=[1.0, 1.0],   # context, intensity
    intensity_dim=1,
    acq=yaep.ACQ_EAVC,                # look-ahead level set
    target_p=0.75,
    grid=[15, 25],                  # 375 candidates
    n_init=8,                       # Halton trials first
    fit=True, fit_every=10,
    stop_trials=80,
)


def log_threshold(g, ctx):
    """What to keep with a threshold: the value, its band, the fit behind it."""
    try:
        thr, lo, hi = g.threshold([ctx])
    except yaep.NoCross:
        print(f"  ctx {ctx:.2f}: no crossing inside the box")
        return
    truth = 0.3 + 0.3 * ctx + 0.67449 / 8.0
    print(f"  ctx {ctx:.2f}: threshold {thr:.3f} [{lo:.3f}, {hi:.3f}] "
          f"(true {truth:.3f}){'  MULTI-CROSS' if g.threshold_multi_cross else ''}")


def plain_loop():
    u = random.Random(1)
    g = yaep.GP(**DESC)
    t0 = time.perf_counter()
    while not g.done:
        _, x = g.next()
        g.update(x, observe(x, u))
    print(f"plain loop: {g.n_trials} trials in {time.perf_counter() - t0:.2f} s")
    for ctx in (0.2, 0.5, 0.8):
        log_threshold(g, ctx)
    h = g.hyper()
    print(f"  lengthscales {h['lengthscale']}, outputscale {h['outputscale']:.3f}, "
          f"log marginal {g.log_marginal:.1f}")


def frame_loop():
    """What a PsychoPy loop looks like with the inference on a thread. The
    sleep stands in for win.flip(); the stimulus is 'shown' for 3 frames."""
    u = random.Random(1)
    g = yaep.GP(**DESC)
    frame = 1.0 / 60.0
    late = 0
    with yaep.Async(g, context=[0.5], fit_in_idle=True, below_normal=True) as a:
        print(f"async: thread policy {a.policy}")
        _, snap = a.poll()                      # seq 0: the first stimulus
        while not snap.done:
            x = snap.x
            for _ in range(3):                  # the trial's frames
                time.sleep(frame)
            while True:                         # a full queue is Busy: retry
                try:
                    seq = a.submit(x, observe(x, u))
                    break
                except yaep.Busy:
                    time.sleep(frame)
            time.sleep(frame)                   # the response frame
            got, snap = a.poll()
            while got < seq:                    # not ready: draw another frame
                late += 1
                time.sleep(frame)
                got, snap = a.poll()
        print(f"async: {snap.n_trials} trials, {late} late frames, "
              f"snapshot threshold at 0.5 = {snap.threshold:.3f} "
              f"[{snap.threshold_lo:.3f}, {snap.threshold_hi:.3f}]")
    # stop() has run: the handle is ours again.
    for ctx in (0.2, 0.5, 0.8):
        log_threshold(g, ctx)


if __name__ == "__main__":
    plain_loop()
    frame_loop()
