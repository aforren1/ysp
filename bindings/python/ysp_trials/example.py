#!/usr/bin/env python3
"""Minimal ysp.trials demo, with a simulated observer.

1. The method of constant stimuli: 2 orientations x 5 contrasts, 20
   repetitions, constrained random order (never the same orientation four
   times running), and the proportion correct per condition.
2. Three staircases interleaved with catch trials: a catch condition on about
   one trial in ten, never two in a row, and each staircase's level stored as
   the trial record.

Install first (from this directory; part 2 also needs ysp-stair):
    uv pip install . ../ysp_stair
or build in place:
    python setup.py build_ext --inplace

Run:
    python example.py [seed]
"""
import math
import random
import struct
import sys

import ysp.trials as ytr

CONTRASTS = [0.01, 0.02, 0.04, 0.08, 0.16]


def p_correct(contrast, threshold=0.04, slope=3.0):
    """2AFC Weibull."""
    return 0.5 + 0.49 * (1.0 - math.exp(-((contrast / threshold) ** slope)))


def constant_stimuli(seed, rng):
    t = ytr.Trials(factors=[("orientation", 2), ("contrast", len(CONTRASTS))], reps=20,
                  order=ytr.ORDER_CONSTRAINED,
                  constraints=[ytr.max_run("orientation", ytr.ANY_LEVEL, 3)],
                  rng=seed)               # the header's splitmix64; log the seed
    print(t.format_meta(), end="")
    while (ti := t.next()) is not None:
        con = CONTRASTS[t.level(ti.condition, 1)]
        t.update(1 if rng.random() < p_correct(con) else 0)
    print("orientation contrast  n  p(correct)")
    for c in range(t.n_conditions):
        ori, con = t.levels(c)
        print(f"{ori:11d} {CONTRASTS[con]:8.2f} {t.n_valid(c):2d}  {t.proportion(c):.2f}")


def interleaved(seed, rng):
    try:
        import ysp.stair as yst
    except ImportError:
        print("ysp-stair is not installed; skipping the interleaved example")
        return
    stairs = [yst.Staircase(start=0.3, n_down=n_down, step_type=yst.STEP_LOG, steps=[0.2, 0.1],
                           min=0.001, max=1.0, stop_reversals=8)
              for n_down in (2, 3, 4)]   # 70.7%, 79.4%, 84.1% correct
    t = ytr.Trials(n_conditions=1, reps=10,            # the catch trial
                  order=ytr.ORDER_CONSTRAINED,
                  constraints=[ytr.min_gap(ytr.CONDITION, 0, 1)],
                  tracks=stairs,                      # a Staircase is a track as is
                  track_rate=0.9, rng=seed, record_size=8)
    print()
    print(t.format_header(), end="")
    while (ti := t.next()) is not None:
        if ti.track >= 0:
            s = stairs[ti.track]
            level = s.next()
            r = 1 if rng.random() < p_correct(level) else 0
            s.update(level, r)
            t.update(r, struct.pack("<d", level))    # the level is the record
        else:
            t.update(1 if rng.random() < 0.02 else 0)   # a false alarm on a blank
        if ti.index < 6:
            print(t.format_row(ti.index), end="")
    print("...")
    for i, s in enumerate(stairs):
        print(f"staircase {i}: {s.n_trials} trials, estimate {s.estimate():.4f}")
    violations = sum(h.violation for h in t.history())
    print(f"catch trials: {t.n_valid(0)}, false alarms {t.count(0, 1)}; "
          f"{violations} ran against the gap rule (only possible once the staircases end)")


def main():
    seed = int(sys.argv[1]) if len(sys.argv) > 1 else 20260923
    rng = random.Random(seed)      # the simulated observer's generator
    constant_stimuli(seed, rng)
    interleaved(seed, rng)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except ytr.Error as e:
        print(f"ysp.trials error: {e}", file=sys.stderr)
        sys.exit(1)
