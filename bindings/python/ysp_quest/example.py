#!/usr/bin/env python3
"""Minimal ysp.quest demo: a Psi-marginal run against a simulated observer,
first on this thread, then with the inference on the Async thread behind a
16 ms frame loop.

Install first (from this directory):
    uv pip install .
or build in place:
    python setup.py build_ext --inplace

Run:
    python example.py [seed]
"""
import random
import sys
import time

import ysp.quest as yqst

TRUTH = [-1.72, 3.1, 0.5, 0.02]   # threshold (log10 contrast), slope, guess, lapse
FRAME_S = 1.0 / 60.0


def desc():
    return dict(
        stim=[(-3.0, 0.0, 31)],
        params=[(-3.0, 0.0, 61), (0.5, 6.0, 12),
                yqst.values([0.45, 0.475, 0.5, 0.525, 0.55], nuisance=True),
                yqst.values([0.0, 0.015, 0.03, 0.045, 0.06], nuisance=True)],
        stop_trials=40)


def synchronous(rng):
    q = yqst.Quest(**desc())
    t0 = time.perf_counter()
    while not q.done:
        i = q.next()
        q.update(i, q.simulate(i, TRUTH, rng.random()))
    dt = (time.perf_counter() - t0) / q.n_trials
    est = q.estimate(yqst.EST_MEAN)
    print(f"synchronous: threshold {est[0]:+.3f} (sd {q.sd(0):.3f}), slope {est[1]:.2f}; "
          f"{dt * 1e3:.2f} ms per next + update on this thread")


def on_a_thread(rng):
    q = yqst.Quest(**desc())
    frames_waited = 0
    with yqst.Async(q) as a:
        names = {yqst.ASYNC_NORMAL: "NORMAL", yqst.ASYNC_BELOW_NORMAL: "BELOW_NORMAL"}
        print(f"async thread policy: {names.get(a.policy, a.policy)}")   # log it
        seq, snap = a.poll()                 # seq 0: the proposal made at start
        while not snap.done:
            # The trial, simulated. The thread owns `q` until stop(), so the
            # observer's model is evaluated on a second Quest.
            k = observer.simulate(snap.proposed, TRUTH, rng.random())
            seq = a.submit(snap.proposed, k)
            # The inter-trial interval: draw frames until the proposal for the
            # next trial accounts for this response.
            while True:
                got, snap = a.poll()
                if got >= seq:
                    break
                frames_waited += 1
                time.sleep(FRAME_S)
    est = q.estimate(yqst.EST_MEAN)            # the Quest is ours again
    print(f"async:       threshold {est[0]:+.3f} (sd {q.sd(0):.3f}) after {q.n_trials} "
          f"trials; {frames_waited} frames waited for a proposal")


observer = yqst.Quest(**desc())


def main():
    seed = int(sys.argv[1]) if len(sys.argv) > 1 else 1
    synchronous(random.Random(seed))
    on_a_thread(random.Random(seed))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except yqst.Error as e:
        print(f"ysp.quest error: {e}", file=sys.stderr)
        sys.exit(1)
