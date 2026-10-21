#!/usr/bin/env python3
"""Run ysp.trials and PsychoPy's TrialHandler / TrialHandlerExt on the same
condition lists and check the properties each order promises, on both.

Not part of CI: PsychoPy is a heavy install. This is the comparison that
docs/trials.md and the ysp/trials.h STATUS block report.

    uv pip install ./bindings/python/ysp_trials numpy psychopy
    python tests/compare/compare_trials_psychopy.py

On Windows `uv pip install psychopy` can fail building pywinhook; psychopy.data
only needs `uv pip install --no-deps psychopy` plus the few packages it
imports (see compare_stair_psychopy.py).

The two draw from different generators (PsychoPy: numpy default_rng(seed);
ysp.trials: splitmix64 from the same seed), so random orders are compared by
property, not trial by trial:

  sequential   identical order to PsychoPy's, every seed
  random       each repetition is one block holding every condition once,
               blocks in repetition order (both sides)
  fullRandom   exact counts per condition (both sides); the first-slot
               frequencies are printed, not tested
  weights      TrialHandlerExt weight w_c with nReps r against ysp.trials
               cond_reps = w_c * r: exact counts under fullRandom (both
               sides). The weighted SEQUENTIAL orders differ by design
               (PsychoPy runs a row's w_c copies back to back each
               repetition; ysp.trials cycles the rows), which is printed as
               information.

Exits 1 when any property fails on either side.
"""
import collections
import sys

import ysp.trials as ytr

try:
    from psychopy import logging as pp_logging
    from psychopy.data import TrialHandler, TrialHandlerExt
except ImportError as exc:  # pragma: no cover - depends on the environment
    print(f"psychopy is not importable here: {exc}", file=sys.stderr)
    sys.exit(2)

pp_logging.console.setLevel(pp_logging.CRITICAL)

SEEDS = range(100)
DESIGNS = [  # (name, n_conditions, n_reps)
    ("3 x 5", 3, 5),
    ("8 x 4", 8, 4),
    ("12 x 10", 12, 10),
]
WEIGHTS = [(3, [3, 1, 2]), (4, [1, 5, 2, 2])]   # (nReps, weight per condition)


def pp_order(n, reps, method, seed, weights=None):
    rows = [{"c": i} for i in range(n)]
    if weights is not None:
        for r, w in zip(rows, weights):
            r["weight"] = w
        th = TrialHandlerExt(rows, reps, method=method, seed=seed, autoLog=False)
    else:
        th = TrialHandler(rows, reps, method=method, seed=seed, autoLog=False)
    return [int(trial["c"]) for trial in th]


def our_order(n, reps, order, seed, cond_reps=None):
    kw = dict(n_conditions=n, order=order, rng=seed)
    if cond_reps is None:
        kw["reps"] = reps
    else:
        kw["cond_reps"] = cond_reps
    t = ytr.Trials(**kw)
    seq, blocks = [], []
    while (ti := t.next()) is not None:
        seq.append(ti.condition)
        blocks.append(ti.rep)
        t.update(1)
    return seq, blocks


def blocks_ok(seq, n, reps):
    return len(seq) == n * reps and all(
        sorted(seq[k * n:(k + 1) * n]) == list(range(n)) for k in range(reps))


def main():
    rows = []   # (check, design, ours ok/total, psychopy ok/total, note)
    bad = 0

    def add(check, design, ours, theirs, note=""):
        nonlocal bad
        n_seeds = len(SEEDS)
        ok = ours == n_seeds and (theirs is None or theirs == n_seeds)
        bad += not ok
        rows.append((check, design, f"{ours}/{n_seeds}",
                     "-" if theirs is None else f"{theirs}/{n_seeds}", "ok" if ok else "FAIL", note))

    for name, n, reps in DESIGNS:
        same = sum(our_order(n, reps, ytr.ORDER_SEQUENTIAL, s)[0] == pp_order(n, reps, "sequential", s)
                   for s in SEEDS)
        add("sequential: identical order", name, same, same)

        ours = theirs = 0
        for s in SEEDS:
            seq, rep = our_order(n, reps, ytr.ORDER_RANDOM, s)
            # The rep of every trial in block k is k: blocks in repetition order.
            ours += blocks_ok(seq, n, reps) and rep == [k for k in range(reps) for _ in range(n)]
            theirs += blocks_ok(pp_order(n, reps, "random", s), n, reps)
        add("random: one of each per rep block", name, ours, theirs)

        ours = theirs = 0
        first_ours, first_pp = collections.Counter(), collections.Counter()
        want = {c: reps for c in range(n)}
        for s in SEEDS:
            seq = our_order(n, reps, ytr.ORDER_FULL_RANDOM, s)[0]
            pp = pp_order(n, reps, "fullRandom", s)
            ours += collections.Counter(seq) == want
            theirs += collections.Counter(pp) == want
            first_ours[seq[0]] += 1
            first_pp[pp[0]] += 1
        exp = len(SEEDS) / n
        note = (f"first slot max|dev| from {exp:.1f}: ours "
                f"{max(abs(first_ours[c] - exp) for c in range(n)):.1f}, psychopy "
                f"{max(abs(first_pp[c] - exp) for c in range(n)):.1f}")
        add("fullRandom: exact counts", name, ours, theirs, note)

    for reps, w in WEIGHTS:
        name = f"w={w} x {reps}"
        cond_reps = [x * reps for x in w]
        want = {c: cond_reps[c] for c in range(len(w))}
        ours = theirs = 0
        for s in SEEDS:
            ours += collections.Counter(our_order(len(w), 0, ytr.ORDER_FULL_RANDOM, s, cond_reps)[0]) == want
            theirs += collections.Counter(pp_order(len(w), reps, "fullRandom", s, w)) == want
        add("weights: exact counts (fullRandom)", name, ours, theirs)
        seq = our_order(len(w), 0, ytr.ORDER_SEQUENTIAL, 0, cond_reps)[0]
        pp = pp_order(len(w), reps, "sequential", 0, w)
        same_counts = collections.Counter(seq) == collections.Counter(pp) == want
        add("weights: sequential counts", name, len(SEEDS) if same_counts else 0, None,
            "order " + ("identical" if seq == pp else "differs by design") +
            f": ours {''.join(map(str, seq[:12]))}..., psychopy {''.join(map(str, pp[:12]))}...")

    print(f"ysp.trials {ytr.__version__} vs psychopy.data.TrialHandler / TrialHandlerExt, "
          f"{len(SEEDS)} seeds per row")
    print()
    hdr = f"{'property':38s} {'design':18s} {'ysp.trials':>10s} {'psychopy':>9s} {'result':>6s}  note"
    print(hdr)
    print("-" * len(hdr))
    for check, design, o, t, res, note in rows:
        print(f"{check:38s} {design:18s} {o:>10s} {t:>9s} {res:>6s}  {note}")
    print()
    print("PASS" if bad == 0 else f"FAIL: {bad} row(s)")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
