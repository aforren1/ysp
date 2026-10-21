#!/usr/bin/env python3
"""Replay fixed response sequences through ysp.stair and PsychoPy's
StairHandler and report the first trial at which their proposals differ.

Not part of CI: PsychoPy is a heavy install. This is the acceptance test for
ysp/stair.h that docs/adapt.md ("Verification") reports.

    uv pip install ./bindings/python/ysp_stair numpy psychopy
    python tests/compare/compare_stair_psychopy.py

On Windows `uv pip install psychopy` can fail building pywinhook; the
StairHandler only needs `uv pip install --no-deps psychopy` plus the few
packages psychopy.data imports (i18next, pyyaml, pandas, msgpack,
msgpack-numpy, packaging, psutil, ujson, requests, openpyxl, matplotlib,
pillow, astunparse, esprima, numpy, scipy, questplus).

Both sides see the same responses at every trial and are asked for the next
level before each one. PsychoPy stops on reversals AND trials; ysp_stair
stops on reversals OR trials. So each case runs a fixed number of trials,
with PsychoPy's nTrials set to that number and its nReversals left at the
schedule length, which it cannot finish before the last trial. Exits 1 on any
disagreement beyond the tolerance.
"""
import math
import sys

import numpy as np

import ysp.stair as yst

try:
    from psychopy import logging as pp_logging
    from psychopy.data import StairHandler
except ImportError as exc:  # pragma: no cover - depends on the environment
    print(f"psychopy is not importable here: {exc}", file=sys.stderr)
    sys.exit(2)

pp_logging.console.setLevel(pp_logging.CRITICAL)

# LIN steps are exact on both sides. LOG and DB are not: PsychoPy multiplies
# the level by 10^step each time, ysp_stair adds step to log10(level) and takes
# the antilog, so the two agree to a few ulp per step, not bit for bit.
REL_TOL = 1e-9

STEP = {"lin": yst.STEP_LIN, "log": yst.STEP_LOG, "db": yst.STEP_DB}

CASES = [
    # name, start, n_up, n_down, step_type, steps, initial_rule, min, max, p_correct
    ("1u2d lin schedule, initial off", 20.0, 1, 2, "lin", [4.0, 2.0, 1.0], False, None, None, 0.70),
    ("1u2d lin schedule, initial on", 20.0, 1, 2, "lin", [4.0, 2.0, 1.0], True, None, None, 0.70),
    ("1u3d lin fixed step, initial on", 40.0, 1, 3, "lin", [4.0], True, None, None, 0.80),
    ("2u1d lin schedule, initial off", 10.0, 2, 1, "lin", [2.0, 1.0], False, None, None, 0.30),
    ("1u2d lin with limits, initial on", 5.0, 1, 2, "lin", [2.0, 1.0], True, 1.0, 8.0, 0.55),
    ("1u2d dB schedule, initial on", 0.5, 1, 2, "db", [4.0, 2.0, 1.0], True, None, None, 0.70),
    ("1u3d log schedule, initial off", 0.5, 1, 3, "log", [0.3, 0.15, 0.075], False, None, None, 0.80),
]
N_TRIALS = 80
SEED = 20260922
EST_WINDOW = 6


def run_case(case, responses):
    name, start, n_up, n_down, step_type, steps, initial, lo, hi, _ = case
    kw = dict(start=start, n_up=n_up, n_down=n_down, step_type=STEP[step_type], steps=steps,
              initial_rule=initial, stop_trials=len(responses), est_reversals=EST_WINDOW)
    if lo is not None:
        kw.update(min=lo, max=hi)
    ours = yst.Staircase(**kw)
    theirs = StairHandler(startVal=start, nReversals=None, stepSizes=list(steps),
                          nTrials=len(responses), nUp=n_up, nDown=n_down,
                          applyInitialRule=initial, stepType=step_type,
                          minVal=lo, maxVal=hi, autoLog=False)
    first_diff = None
    worst = 0.0
    rows = []
    for t, r in enumerate(responses):
        a = ours.next()
        b = theirs.next()
        rel = abs(a - b) / max(abs(b), 1e-300)
        worst = max(worst, rel)
        if rel > REL_TOL and first_diff is None:
            first_diff = t
        rows.append((t, a, b, r))
        ours.update(a, r)
        theirs.addResponse(r)
    our_rev = [t for t, _ in ours.reversals()]
    their_rev = list(theirs.reversalPoints)
    our_rev_lev = [lv for _, lv in ours.reversals()]
    their_rev_lev = list(theirs.reversalIntensities)
    lev_ok = len(our_rev_lev) == len(their_rev_lev) and all(
        abs(x - y) <= REL_TOL * max(abs(y), 1e-300) for x, y in zip(our_rev_lev, their_rev_lev))
    # The customary PsychoPy estimate is the arithmetic mean of the last few
    # reversal intensities. ysp_stair's EST_REVERSALS is that mean in the
    # step's units, so it is comparable directly for LIN only.
    est_ours = ours.estimate(yst.EST_REVERSALS)
    est_theirs = float(np.mean(their_rev_lev[-EST_WINDOW:])) if their_rev_lev else math.nan
    if step_type != "lin":
        est_theirs = float(10 ** np.mean(np.log10(their_rev_lev[-EST_WINDOW:])))
    return dict(name=name, first_diff=first_diff, worst=worst, rev_ok=our_rev == their_rev,
                lev_ok=lev_ok, n_rev=(len(our_rev), len(their_rev)),
                est=(est_ours, est_theirs), rows=rows)


def main():
    rng = np.random.default_rng(SEED)
    results = []
    for case in CASES:
        responses = (rng.random(N_TRIALS) < case[-1]).astype(int).tolist()
        results.append(run_case(case, responses))

    print(f"ysp.stair vs psychopy.data.StairHandler, {N_TRIALS} trials per case, seed {SEED}")
    print(f"proposal tolerance: relative {REL_TOL:g}")
    print()
    hdr = f"{'case':36s} {'first diff':>10s} {'max rel diff':>13s} {'reversals':>11s} {'rev trials':>10s} {'rev levels':>10s} {'est ours':>10s} {'est pp':>10s}"
    print(hdr)
    print("-" * len(hdr))
    bad = 0
    for r in results:
        ok = r["first_diff"] is None and r["rev_ok"] and r["lev_ok"]
        est_ok = abs(r["est"][0] - r["est"][1]) <= 1e-9 * max(1.0, abs(r["est"][1])) or (
            math.isnan(r["est"][0]) and math.isnan(r["est"][1]))
        ok = ok and est_ok
        bad += not ok
        fd = "none" if r["first_diff"] is None else str(r["first_diff"])
        print(f"{r['name']:36s} {fd:>10s} {r['worst']:13.3g} {r['n_rev'][0]:>5d}/{r['n_rev'][1]:<5d} "
              f"{'same' if r['rev_ok'] else 'DIFF':>10s} {'same' if r['lev_ok'] else 'DIFF':>10s} "
              f"{r['est'][0]:10.5g} {r['est'][1]:10.5g}")
    for r in results:
        if r["first_diff"] is not None:
            t0 = r["first_diff"]
            print(f"\n{r['name']}: trials {max(0, t0 - 3)}..{t0 + 2}")
            print(f"{'trial':>5s} {'ysp.stair':>12s} {'psychopy':>12s} {'resp':>4s}")
            for t, a, b, resp in r["rows"][max(0, t0 - 3):t0 + 3]:
                print(f"{t:5d} {a:12.6g} {b:12.6g} {resp:4d}")
    print()
    print("PASS" if bad == 0 else f"FAIL: {bad} case(s) disagree")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
