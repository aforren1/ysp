#!/usr/bin/env python3
"""Run ysp.quest and the questplus package side by side on the same grids and
the same simulated observer, and report per-trial stimulus agreement and the
largest posterior difference.

Not part of CI. This is the acceptance test for ysp/quest.h that
docs/adapt.md ("Verification") reports.

    uv pip install ./bindings/python/ysp_quest numpy questplus
    python tests/compare/compare_quest_questplus.py

One seeded numpy Generator draws one uniform u per trial and both sides get
that u: ysp.quest through Quest.simulate(), questplus through the same rule
written in numpy (outcome 1 when u >= p(incorrect)). Both then see the stimulus
ysp.quest proposed, so a disagreement at one trial does not fork the two
posteriors and the posterior comparison stays meaningful to the end.

Three runs:
  psi            Psi grid, joint entropy: questplus's min_entropy.
  psi-marg joint the Psi-marginal grid with no axis flagged: still joint
                 entropy, so questplus selects the same way.
  psi-marg marg  the same grid with guess and lapse flagged nuisance. questplus
                 has no marginalized selection, so the reference selection
                 is the marginal expected entropy computed in numpy from
                 questplus's own likelihood table and posterior (Prins 2013).

A selection that differs is classed as a TIE when the reference expected
entropies of the two stimuli differ by less than TIE_BITS; the float table
moves a score by up to about 5e-8 bits (ysp/quest.h, STATUS), so a closer
pair is not separable. Exits 1 on a non-tie disagreement, an outcome mismatch,
or a posterior difference above POST_TOL.
"""
import sys
import time
from importlib.metadata import version

import numpy as np
import questplus as qp

import ysp.quest as yqst

SEED = 20260922
N_TRIALS = 60
POST_TOL = 1e-5
TIE_BITS = 1e-6

STIM = np.linspace(-3.0, 0.0, 31)
THRESH = np.linspace(-3.0, 0.0, 61)
SLOPE = np.linspace(0.5, 6.0, 12)
GUESS_FIXED = np.array([0.5])
LAPSE_FIXED = np.array([0.02])
GUESS_MARG = np.linspace(0.45, 0.55, 5)
LAPSE_MARG = np.linspace(0.0, 0.06, 5)
TRUTH = (-1.5, 3.5, 0.5, 0.02)   # threshold, slope, guess, lapse


def make_qp(guess, lapse):
    return qp.QuestPlus(
        stim_domain=dict(intensity=STIM),
        param_domain=dict(threshold=THRESH, slope=SLOPE, lower_asymptote=guess, lapse_rate=lapse),
        outcome_domain=dict(response=["Correct", "Incorrect"]),
        func="weibull", stim_scale="log10", stim_selection_method="min_entropy")


def make_pq(guess, lapse, nuisance):
    return yqst.Quest([STIM], [THRESH, SLOPE, yqst.values(guess, nuisance=nuisance),
                             yqst.values(lapse, nuisance=nuisance)], stop_trials=10000)


def p_correct(x, a, b, g, lam):
    return 1.0 - lam - (1.0 - g - lam) * np.exp(-(10.0 ** (b * (x - a))))


def reference_eh(q_plus, marginal):
    """Expected posterior entropy in bits for every stimulus, from questplus's
    likelihood table and posterior, joint or marginalized over the last two
    parameter axes."""
    lik = q_plus.likelihoods.transpose("response", "intensity", "threshold", "slope",
                                       "lower_asymptote", "lapse_rate").values
    post = q_plus.posterior.transpose("threshold", "slope", "lower_asymptote",
                                      "lapse_rate").values
    joint = lik * post[None, None]
    pk = joint.sum(axis=(2, 3, 4, 5))
    if marginal:
        joint = joint.sum(axis=(4, 5))
    cond = joint / pk.reshape(pk.shape + (1,) * (joint.ndim - 2))
    with np.errstate(divide="ignore", invalid="ignore"):
        h = -np.where(cond > 0, cond * np.log2(cond), 0.0)
    h = h.sum(axis=tuple(range(2, joint.ndim)))
    return (pk * h).sum(axis=0)


def run(label, guess, lapse, nuisance, rng):
    q = make_pq(guess, lapse, nuisance)
    ref = make_qp(guess, lapse)
    rows = []
    t_ours = t_qp = 0.0
    for trial in range(N_TRIALS):
        t0 = time.perf_counter()
        i = q.next()
        t1 = time.perf_counter()
        if nuisance:
            eh = reference_eh(ref, marginal=True)
            j = int(np.argmin(eh))
        else:
            x_ref = ref.next_stim["intensity"]
            j = int(np.flatnonzero(STIM == x_ref)[0])
        t2 = time.perf_counter()
        if not nuisance:
            eh = reference_eh(ref, marginal=False)   # for the tie test only
        t_ours += t1 - t0
        t_qp += t2 - t1
        if i == j:
            kind = "same"
        elif abs(eh[i] - eh[j]) < TIE_BITS:
            kind = "tie"
        else:
            kind = "DIFF"
        u = rng.random()
        k_ours = q.simulate(i, TRUTH, u)
        k_ref = 1 if u >= 1.0 - p_correct(STIM[i], *TRUTH) else 0
        q.update(i, k_ours)
        ref.update(stim=dict(intensity=STIM[i]),
                   outcome=dict(response="Correct" if k_ref == 1 else "Incorrect"))
        post_ours = np.frombuffer(q.posterior(), dtype=np.float64)
        post_ref = ref.posterior.transpose("threshold", "slope", "lower_asymptote",
                                           "lapse_rate").values.ravel()
        dpost = float(np.max(np.abs(post_ours - post_ref)))
        rows.append((trial, i, j, kind, k_ours, k_ref, dpost, abs(eh[i] - eh[j])))
    return dict(label=label, rows=rows, P=q.n_param, t_ours=t_ours, t_qp=t_qp,
                est_ours=q.estimate(yqst.EST_MEAN),
                est_ref=[ref.param_estimate[k] for k in
                         ("threshold", "slope", "lower_asymptote", "lapse_rate")])


def main():
    runs = [("psi", GUESS_FIXED, LAPSE_FIXED, False),
            ("psi-marg joint", GUESS_MARG, LAPSE_MARG, False),
            ("psi-marg marg", GUESS_MARG, LAPSE_MARG, True)]
    results = []
    for label, g, lam, nuis in runs:
        rng = np.random.default_rng(SEED)
        results.append(run(label, g, lam, nuis, rng))

    print(f"ysp-quest {version('ysp-quest')} vs questplus {qp.__version__}; seed {SEED}, "
          f"{N_TRIALS} trials per run, truth {TRUTH}")
    print(f"posterior tolerance {POST_TOL:g}, tie band {TIE_BITS:g} bits")
    bad = 0
    for r in results:
        print()
        print(f"== {r['label']}  (S = {len(STIM)}, P = {r['P']})")
        print(f"{'trial':>5s} {'ysp.quest':>9s} {'ref':>5s} {'select':>6s} {'k':>3s} {'k ref':>5s} "
              f"{'max|dpost|':>11s} {'|dEH| bits':>11s}")
        for (t, i, j, kind, k1, k2, dp, deh) in r["rows"]:
            print(f"{t:5d} {i:9d} {j:5d} {kind:>6s} {k1:3d} {k2:5d} {dp:11.3e} {deh:11.3e}")
        same = sum(1 for x in r["rows"] if x[3] == "same")
        ties = sum(1 for x in r["rows"] if x[3] == "tie")
        diffs = sum(1 for x in r["rows"] if x[3] == "DIFF")
        kmis = sum(1 for x in r["rows"] if x[4] != x[5])
        worst = max(x[6] for x in r["rows"])
        ok = diffs == 0 and kmis == 0 and worst <= POST_TOL
        bad += not ok
        r.update(same=same, ties=ties, diffs=diffs, kmis=kmis, worst=worst, ok=ok)
        print(f"estimate (mean): ysp.quest {np.round(r['est_ours'], 6).tolist()}  "
              f"questplus {np.round(r['est_ref'], 6).tolist()}")

    print()
    print("SUMMARY")
    hdr = (f"{'run':16s} {'same':>5s} {'tie':>4s} {'diff':>5s} {'k mismatch':>10s} "
           f"{'max|dpost|':>11s} {'ysp.quest s':>11s} {'reference s':>11s} {'result':>6s}")
    # "reference s" is questplus's next_stim for the joint runs and the numpy
    # marginal expected entropy for the marginal run; selection time only.
    print(hdr)
    print("-" * len(hdr))
    for r in results:
        print(f"{r['label']:16s} {r['same']:5d} {r['ties']:4d} {r['diffs']:5d} {r['kmis']:10d} "
              f"{r['worst']:11.3e} {r['t_ours']:11.3f} {r['t_qp']:11.3f} "
              f"{'ok' if r['ok'] else 'FAIL':>6s}")
    print()
    print("PASS" if bad == 0 else f"FAIL: {bad} run(s) disagree")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
