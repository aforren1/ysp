#!/usr/bin/env python3
"""Minimal ysp.stair demo: a 1-up-3-down staircase on log10 contrast against a
simulated Weibull observer.

Install first (from this directory):
    uv pip install .
or build in place:
    python setup.py build_ext --inplace

Run:
    python example.py [seed]
"""
import math
import random
import sys

import ysp.stair as yst

THRESHOLD = 0.05   # the observer's 2AFC threshold, contrast
SLOPE = 3.5


def p_correct(contrast):
    """2AFC Weibull: 0.5 at zero contrast, 0.99 at the top."""
    return 0.5 + 0.49 * (1.0 - math.exp(-((contrast / THRESHOLD) ** SLOPE)))


def main():
    seed = int(sys.argv[1]) if len(sys.argv) > 1 else 1
    rng = random.Random(seed)   # the header has no generator; this is ours

    s = yst.Staircase(start=0.5, n_down=3, step_type=yst.STEP_LOG,
                     steps=[0.3, 0.15, 0.075], min=0.001, max=1.0,
                     stop_reversals=12)
    while not s.done:
        level = s.next()
        response = yst.simulate_response(p_correct(level), rng.random())
        events = s.update(level, response)
        mark = " reversal" if events & yst.Event.REVERSAL else ""
        print(f"trial {s.n_trials:3d}  level {level:8.5f}  response {response}{mark}")

    est = s.estimate(yst.EST_REVERSALS)
    n = s.estimate_count(yst.EST_REVERSALS)
    p = yst.convergence_p(1, 3)
    print(f"stopped: {s.stop_reason.name} after {s.n_trials} trials")
    print(f"estimate: {est:.5f} (geometric mean of {n} reversals); the rule "
          f"converges on p = {p:.3f}, where the observer's level is "
          f"{THRESHOLD * (-math.log(1 - (p - 0.5) / 0.49)) ** (1 / SLOPE):.5f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
