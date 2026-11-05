#!/usr/bin/env python3
"""Cross-method comparison of ysp.stair, ysp.quest and ysp.aep on the test
problems of Watson 2017 (QUEST+) and Owen et al. 2021 (AEPsych).

See docs/adapt_comparison.md for what is compared, why, and the results.

Every method sees the same response stream: one seeded numpy Generator per
(problem, replication) draws one uniform u per trial, and trial t of every
method gets u[t]. A binary response is 1 when u >= 1 - p (as
compare_gp_aepsych.py); a K-outcome response is the smallest k whose
cumulative probability exceeds u. The GP methods on a problem share their
init points (scrambled Sobol, seeded per replication).

Usage (from the repository root, in a venv with ysp-stair, ysp-quest, ysp-aep,
numpy and scipy):

    python tests/compare/methods_compare.py --out DIR             # everything
    python tests/compare/methods_compare.py --out DIR --only w1,novel-det
    python tests/compare/methods_compare.py --out DIR --quick     # 3 reps
    python tests/compare/methods_compare.py --replay QuestPlus.nb

Writes DIR/raw_<problem>.csv (one row per scored trial) and DIR/tables.txt,
and prints one table per problem. The replications run in worker processes;
ms/trial comes from a serial pass afterward (--timing-reps). --replay needs
Watson's QUEST+ Mathematica notebook, which is not in this repository.

Problem keys: w1 to w4 (Watson's 1-D examples), audio-<phenotype>-b<beta>,
novel-det, novel-dis, csf, circ, and csf6 (Letham et al. 2022's real-data
CSF), which runs only when --only names it. csf6 downloads its data into
--cache and builds its truth there (--truth aepsych|yaep, --build-truth).
Not a CI test.
"""
import argparse
import math
import os
import subprocess
import sys
import time
from concurrent.futures import ProcessPoolExecutor, as_completed

import numpy as np
from scipy.special import ndtr, ndtri

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
# The audiometric field, its spline and the paper's two metrics.
import compare_gp_aepsych as aud  # noqa: E402

SEED = 20170310

# --- shared pieces ------------------------------------------------------------

def stream(prob_id, rep, n):
    """One uniform per trial, the same for every method on a problem."""
    return np.random.default_rng([SEED, prob_id, rep]).random(n)


def respond(p, u):
    return 0 if u < 1.0 - p else 1


def respond_k(probs, u):
    c = np.cumsum(probs)
    k = int(np.searchsorted(c, u, side="right"))
    return min(k, len(probs) - 1)


def sobol_init(prob_id, rep, d, n):
    from scipy.stats import qmc
    import warnings
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        return qmc.Sobol(d, scramble=True,
                         seed=np.random.default_rng([SEED, prob_id, rep, 1])).random(n)


def column_threshold(p, xi, target):
    """The paper's rule: the first bracketing pair along intensity, linearly
    interpolated."""
    for j in range(len(p) - 1):
        a, b = p[j] - target, p[j + 1] - target
        if (a <= 0.0 <= b) or (a >= 0.0 >= b):
            if b == a:
                continue
            return xi[j] + (xi[j + 1] - xi[j]) * (-a) / (b - a)
    return None


def mean_ci(v):
    v = np.array([x for x in v if x is not None and x == x], dtype=float)
    if len(v) == 0:
        return float("nan"), float("nan"), 0
    ci = 1.96 * v.std(ddof=1) / math.sqrt(len(v)) if len(v) > 1 else 0.0
    return float(v.mean()), ci, len(v)


def machine_load():
    """Percent CPU load across all cores, from Windows, or the 1-min load
    average elsewhere. For the report only."""
    try:
        if os.name == "nt":
            out = subprocess.run(["powershell", "-NoProfile", "-Command",
                                  "(Get-CimInstance Win32_Processor | Measure-Object "
                                  "-Property LoadPercentage -Average).Average"],
                                 capture_output=True, text=True, timeout=30).stdout.strip()
            return f"{out}% CPU"
        return "loadavg %.2f" % os.getloadavg()[0]
    except Exception as e:  # pragma: no cover
        return f"unknown ({e})"


# --- 2-D problems -------------------------------------------------------------

NGRID = 30          # the paper's metric grid per dimension
NFINE = 400         # intensity points per column for the transition band
N2D = 150
N_INIT = 5
MARKS_2D = (25, 50, 100, 150)


class Field2D:
    """A 2-D observer on a box (context, intensity): the truth on the paper's
    30 x 30 grid, its threshold per column, and the transition band on a fine
    intensity grid."""

    def __init__(self, name, lo, hi, p_fn, thr_fn, target_p, units, criterion,
                 stair_ctx, stair_start, stair_steps, prob_id, note="", guess=0.0,
                 lapse=0.0, marks=MARKS_2D):
        self.name, self.lo, self.hi = name, np.array(lo, float), np.array(hi, float)
        self.guess, self.lapse, self.marks = guess, lapse, tuple(marks)
        self.p_fn, self.thr_fn = p_fn, thr_fn
        self.target_p, self.units, self.criterion = target_p, units, criterion
        self.stair_ctx, self.stair_start, self.stair_steps = stair_ctx, stair_start, stair_steps
        self.prob_id, self.note = prob_id, note
        self.xc = np.linspace(lo[0], hi[0], NGRID)
        self.xi = np.linspace(lo[1], hi[1], NGRID)
        cc, ii = np.meshgrid(self.xc, self.xi, indexing="ij")
        self.xs = np.column_stack([cc.ravel(), ii.ravel()])
        self.truth_p = p_fn(self.xs[:, 0], self.xs[:, 1]).reshape(NGRID, NGRID)
        self.truth_thr = [column_threshold(self.truth_p[i], self.xi, target_p)
                          for i in range(NGRID)]
        self.xi_f = np.linspace(lo[1], hi[1], NFINE)
        cc, ii = np.meshgrid(self.xc, self.xi_f, indexing="ij")
        self.xs_f = np.column_stack([cc.ravel(), ii.ravel()])
        tf = p_fn(self.xs_f[:, 0], self.xs_f[:, 1])
        self.band = (tf >= 0.05) & (tf <= 0.95)
        self.truth_f = tf

    def p(self, x):
        return float(self.p_fn(np.array([x[0]]), np.array([x[1]]))[0])

    def score(self, model_p):
        model_p = np.asarray(model_p).reshape(NGRID, NGRID)
        mae_p = float(np.mean(np.abs(model_p - self.truth_p)))
        errs, miss = [], 0
        for i in range(NGRID):
            t = column_threshold(model_p[i], self.xi, self.target_p)
            if t is None or self.truth_thr[i] is None:
                miss += 1
            else:
                errs.append(abs(t - self.truth_thr[i]))
        return mae_p, (float(np.mean(errs)) if errs else float("nan")), miss

    def band_mae(self, model_pf):
        return float(np.mean(np.abs(np.asarray(model_pf) - self.truth_f)[self.band]))

    def shifted(self, xs, thr_est):
        """The informed model of a staircase: the TRUE psychometric function at
        each context, moved along intensity so its target crossing sits at the
        estimate. This gives the staircase the slope for free."""
        c = xs[:, 0]
        return self.p_fn(c, xs[:, 1] - (thr_est(c) - self.thr_fn(c)))


def audiogram_field(pheno, beta, prob_id):
    f = aud.Field(pheno, beta)
    z = ndtri(0.75)
    return Field2D(
        f"audio-{pheno}-b{beta:g}", [aud.XC_LO, aud.XI_LO], [aud.XC_HI, aud.XI_HI],
        lambda c, i: ndtr((np.asarray(i) - f.theta(c)) / beta),
        lambda c: f.theta(c) + beta * z, 0.75, "dB", 2.0,
        list(np.log2(aud.AUDIO_F)), 40.0, [10.0, 5.0], prob_id,
        note=f"Dubno et al. 2013 {pheno} phenotype, beta = {beta:g} dB")


def novel_theta(f):
    return 2.0 * (0.05 + 0.4 * (-1.0 + 0.2 * f) ** 2 * f ** 2)


def novel_field(kind, prob_id):
    z = ndtri(0.75)
    if kind == "det":
        p = lambda c, a: ndtr(4.0 * (np.asarray(a) + 1.0) / novel_theta(c) - 4.0)
        t = lambda c: novel_theta(c) * (z + 4.0) / 4.0 - 1.0
    else:
        p = lambda c, a: ndtr(2.0 * (np.asarray(a) + 1.0) / novel_theta(c))
        t = lambda c: novel_theta(c) * z / 2.0 - 1.0
    return Field2D(f"novel-{kind}", [-1.0, -1.0], [1.0, 1.0], p, t, 0.75,
                   "intensity units", 0.1, list(np.linspace(-1.0, 1.0, 6)), 0.0,
                   [0.28, 0.14, 0.07], prob_id,
                   note=f"Owen et al. 2021 novel {'detection' if kind == 'det' else 'discrimination'}")


def piecewise_curve(ctx, thr):
    """Thresholds at a few contexts read as a curve: linear between them and
    the end segments extended, as aep_audiometric.c's stair_curve."""
    ctx, thr = np.asarray(ctx, float), np.asarray(thr, float)

    def curve(c):
        c = np.asarray(c, float)
        j = np.clip(np.searchsorted(ctx, c) - 1, 0, len(ctx) - 2)
        w = (c - ctx[j]) / (ctx[j + 1] - ctx[j])
        return thr[j] + w * (thr[j + 1] - thr[j])
    return curve


def stair_estimate(s):
    import ysp.stair as yst
    v = s.estimate(yst.EST_REVERSALS)
    return v if v == v else s.estimate(yst.EST_LAST)


def run_stair2d(fd, rep, n_trials=N2D, every=5):
    """Interleaved weighted 1-up-1-down staircases at fixed contexts, round
    robin, one per context, as aep_audiometric.c. Scored through the informed
    model (Field2D.shifted)."""
    import ysp.stair as yst
    u = stream(fd.prob_id, rep, n_trials)
    nc = len(fd.stair_ctx)
    ss = [yst.Staircase(start=fd.stair_start, n_up=1, n_down=1, step_type=yst.STEP_LIN,
                       steps=fd.stair_steps, step_down_scale=yst.weighted_scale(fd.target_p),
                       min=fd.lo[1], max=fd.hi[1], use_limits=True,
                       stop_trials=n_trials // nc + 2) for _ in range(nc)]
    rows, spent = [], 0.0
    for t in range(n_trials):
        k = t % nc
        t0 = time.perf_counter()
        lv = ss[k].next()
        spent += time.perf_counter() - t0
        y = respond(fd.p([fd.stair_ctx[k], lv]), u[t])
        t0 = time.perf_counter()
        ss[k].update(lv, y)
        spent += time.perf_counter() - t0
        n = t + 1
        if n % every == 0 or n in fd.marks:
            curve = piecewise_curve(fd.stair_ctx, [stair_estimate(s) for s in ss])
            mp = fd.shifted(fd.xs, curve)
            band = fd.band_mae(fd.shifted(fd.xs_f, curve)) if n in fd.marks else float("nan")
            rows.append((n,) + fd.score(mp) + (1000.0 * spent / n, band))
    return rows, {}


def run_gp2d(fd, rep, model, acq, n_trials=N2D, every=5, grid=(11, 21), refine=2,
             fit_every=20):
    import ysp.aep as yaep
    u = stream(fd.prob_id, rep, n_trials)
    init = fd.lo + (fd.hi - fd.lo) * sobol_init(fd.prob_id, rep, 2, N_INIT)
    g = yaep.GP(lo=list(fd.lo), hi=list(fd.hi), intensity_dim=1, acq=acq,
              target_p=fd.target_p, grid=list(grid), n_init=N_INIT, fit=True,
              fit_every=fit_every, stop_trials=n_trials, max_trials=n_trials,
              refine_steps=refine, model=model, guess=fd.guess, lapse=fd.lapse)
    rows, spent, numeric = [], 0.0, 0
    for t in range(n_trials):
        t0 = time.perf_counter()
        x = list(init[t]) if t < N_INIT else g.next()[1]
        spent += time.perf_counter() - t0
        y = respond(fd.p(x), u[t])
        t0 = time.perf_counter()
        try:
            g.update(x, y)
        except yaep.Numeric:
            numeric += 1
        spent += time.perf_counter() - t0
        n = t + 1
        if n % every == 0 or n in fd.marks:
            mp = np.frombuffer(g.predict_p_many(fd.xs))
            band = (fd.band_mae(np.frombuffer(g.predict_p_many(fd.xs_f)))
                    if n in fd.marks else float("nan"))
            rows.append((n,) + fd.score(mp) + (1000.0 * spent / n, band))
    return rows, {"numeric": numeric, "hyper": g.hyper(), "log_marginal": g.log_marginal}


# --- 1-D problems: Watson 2017 ------------------------------------------------
#
# Specifications from Watson's QUEST+ notebook (QuestPlus.nb, the reference
# implementation behind the paper): the Weibull in dB,
#   P(correct) = 1 - lapse - (1 - guess - lapse) exp(-10^(slope (x - thr) / 20)),
# which is ysp/quest.h's PF_GUMBEL with its slope axis scaled by 1/20; and the
# cumulative normal with a symmetric lapse,
#   P(right) = lapse + (1 - 2 lapse) Phi((x - mean) / sd).
# Watson's estimate is the joint posterior mode on the grid (the first
# maximum), and his selection takes the first minimum of expected entropy.

def weibull_db(x, thr, slope, guess, lapse):
    return 1.0 - lapse - (1.0 - guess - lapse) * np.exp(-10.0 ** (slope * (np.asarray(x) - thr) / 20.0))


def normal_lapse(x, mean, sd, lapse):
    return lapse + (1.0 - 2.0 * lapse) * ndtr((np.asarray(x) - mean) / sd)


def normal_batch(stim, params):
    P = np.asarray(params)
    p1 = normal_lapse(stim[0], P[:, 0], P[:, 1], P[:, 2])
    return np.stack([1.0 - p1, p1], axis=1)


D40 = [float(v) for v in range(-40, 1)]
SLOPES = [2.0, 3.0, 4.0, 5.0]
LAPSES = [0.0, 0.01, 0.02, 0.03, 0.04]


class Problem1D:
    """A Watson example: the stimulus axis, the truth, the configurations of
    ysp.quest that apply, and what the other methods need."""

    def __init__(self, key, prob_id, title, stim, truth, n_paper, p_fn, thr_name,
                 thr_param, quest_cfgs, paper_cfg, guess, lapse, criterion, units,
                 slope_name=None, stair_start=0.0, stair_steps=(8.0, 4.0, 2.0)):
        self.key, self.prob_id, self.title = key, prob_id, title
        self.stim, self.truth, self.n_paper = list(stim), tuple(truth), n_paper
        self.p_fn, self.thr_name = p_fn, thr_name
        self.thr_true = truth[thr_param]
        self.quest_cfgs, self.paper_cfg = quest_cfgs, paper_cfg
        self.guess, self.lapse = guess, lapse
        self.criterion, self.units = criterion, units
        self.slope_name = slope_name
        self.stair_start, self.stair_steps = stair_start, list(stair_steps)
        self.lo, self.hi = min(stim), max(stim)
        self.n_trials = max(128, n_paper)
        # The threshold every method is scored against: the paper's threshold
        # parameter, and the p the true function has there.
        self.target_p = float(p_fn(self.thr_true))
        self.slope_true = truth[1] if slope_name else None

    def x_at(self, p):
        """The true stimulus at which P(correct) = p, by bisection."""
        a, b = self.lo - 200.0, self.hi + 200.0
        for _ in range(200):
            m = 0.5 * (a + b)
            if self.p_fn(m) < p:
                a = m
            else:
                b = m
        return 0.5 * (a + b)


def watson_problems():
    probs = {}
    t1 = (-20.0, 3.5, 0.5, 0.02)
    probs["w1"] = Problem1D(
        "w1", 101, "Weibull, threshold only (Watson 2017 example 1)", D40, t1, 32,
        lambda x: weibull_db(x, *t1), "threshold (dB)", 0,
        {"QUEST (QUEST+, threshold only)": dict(params=[D40, 3.5 / 20, 0.5, 0.02], slope=None),
         "Psi (threshold, slope)": dict(params=[D40, [s / 20 for s in SLOPES], 0.5, 0.02], slope=1),
         "Psi-marginal (lapse nuisance)": dict(
             params=[D40, [s / 20 for s in SLOPES], 0.5, "nuis"], slope=1)},
        "QUEST (QUEST+, threshold only)", 0.5, 0.02, 2.0, "dB")
    t2 = (-20.0, 3.0, 0.5, 0.02)
    probs["w2"] = Problem1D(
        "w2", 102, "Weibull, threshold and slope (Watson 2017 example 2)", D40, t2, 64,
        lambda x: weibull_db(x, *t2), "threshold (dB)", 0,
        {"QUEST (QUEST+, threshold only)": dict(params=[D40, 3.0 / 20, 0.5, 0.02], slope=None),
         "Psi (threshold, slope)": dict(params=[D40, [s / 20 for s in SLOPES], 0.5, 0.02], slope=1),
         "Psi-marginal (lapse nuisance)": dict(
             params=[D40, [s / 20 for s in SLOPES], 0.5, "nuis"], slope=1)},
        "Psi (threshold, slope)", 0.5, 0.02, 2.0, "dB", slope_name="slope")
    t3 = (-20.0, 3.0, 0.5, 0.03)
    probs["w3"] = Problem1D(
        "w3", 103, "Weibull, threshold, slope and lapse (Watson 2017 example 3)", D40, t3, 128,
        lambda x: weibull_db(x, *t3), "threshold (dB)", 0,
        {"QUEST (QUEST+, threshold only)": dict(params=[D40, 3.0 / 20, 0.5, 0.03], slope=None),
         "Psi (threshold, slope; lapse fixed at 0.02)": dict(
             params=[D40, [s / 20 for s in SLOPES], 0.5, 0.02], slope=1),
         "QUEST+ (threshold, slope, lapse joint)": dict(
             params=[D40, [s / 20 for s in SLOPES], 0.5, LAPSES], slope=1),
         "Psi-marginal (lapse nuisance)": dict(
             params=[D40, [s / 20 for s in SLOPES], 0.5, "nuis"], slope=1)},
        "QUEST+ (threshold, slope, lapse joint)", 0.5, 0.02, 2.0, "dB", slope_name="slope")
    t4 = (1.0, 3.0, 0.02)
    x4 = [float(v) for v in range(-10, 11)]
    probs["w4"] = Problem1D(
        "w4", 104, "Cumulative normal, mean, sd and lapse (Watson 2017 example 4)", x4, t4, 128,
        lambda x: normal_lapse(x, *t4), "mean (deg)", 0,
        {"QUEST (QUEST+, mean only)": dict(params=[[float(v) for v in range(-5, 6)], 3.0, 0.02],
                                           slope=None, custom=True),
         "QUEST+ (mean, sd, lapse joint)": dict(
             params=[[float(v) for v in range(-5, 6)], [float(v) for v in range(1, 11)], LAPSES],
             slope=1, custom=True),
         "Psi-marginal (lapse nuisance)": dict(
             params=[[float(v) for v in range(-5, 6)], [float(v) for v in range(1, 11)], "nuis"],
             slope=1, custom=True)},
        "QUEST+ (mean, sd, lapse joint)", 0.02, 0.02, 0.5, "deg", slope_name="sd",
        stair_start=-10.0, stair_steps=(4.0, 2.0, 1.0))
    return probs


def quest_axes(prob, cfg):
    import ysp.quest as yqst
    out = []
    for a in cfg["params"]:
        if isinstance(a, str) and a == "nuis":
            out.append(yqst.values(LAPSES, nuisance=True))
        elif isinstance(a, list):
            out.append(yqst.values(a))
        else:
            out.append(float(a))
    return out


def make_quest1d(prob, cfg, stop):
    import ysp.quest as yqst
    kw = dict(stop_trials=stop)
    if cfg.get("custom"):
        kw.update(pf_batch=normal_batch)
    else:
        kw.update(pf=yqst.PF_GUMBEL)
    return yqst.Quest([prob.stim], quest_axes(prob, cfg), **kw)


def slope_scale(prob, cfg):
    return 1.0 if cfg.get("custom") else 20.0


def run_quest1d(prob, rep, name):
    import ysp.quest as yqst
    cfg = prob.quest_cfgs[name]
    q = make_quest1d(prob, cfg, prob.n_trials)
    u = stream(prob.prob_id, rep, prob.n_trials)
    rows, spent = [], 0.0
    for t in range(prob.n_trials):
        t0 = time.perf_counter()
        i = q.next()
        spent += time.perf_counter() - t0
        k = respond(float(prob.p_fn(prob.stim[i])), u[t])
        t0 = time.perf_counter()
        q.update(i, k)
        spent += time.perf_counter() - t0
        mean = q.estimate(yqst.EST_MEAN)
        mode = q.estimate(yqst.EST_MODE)
        s = cfg["slope"]
        sc = slope_scale(prob, cfg)
        rows.append((t + 1, mean[0], mean[s] * sc if s else float("nan"),
                     1000.0 * spent / (t + 1), mode[0], mode[s] * sc if s else float("nan")))
    return rows, {}


STAIR_RULES = {
    "1-up-2-down": dict(n_up=1, n_down=2),
    "1-up-3-down": dict(n_up=1, n_down=3),
    "weighted 1-up-1-down": dict(n_up=1, n_down=1, weighted=True),
}


def run_stair1d(prob, rep, name):
    """Scored against the true level at the rule's own convergence p (for the
    weighted rule that is the threshold)."""
    import ysp.stair as yst
    r = STAIR_RULES[name]
    kw = dict(start=prob.stair_start, n_up=r["n_up"], n_down=r["n_down"],
              step_type=yst.STEP_LIN, steps=prob.stair_steps, min=prob.lo, max=prob.hi,
              use_limits=True, stop_trials=prob.n_trials)
    if r.get("weighted"):
        kw["step_down_scale"] = yst.weighted_scale(prob.target_p)
    s = yst.Staircase(**kw)
    u = stream(prob.prob_id, rep, prob.n_trials)
    rows, spent = [], 0.0
    for t in range(prob.n_trials):
        t0 = time.perf_counter()
        lv = s.next()
        spent += time.perf_counter() - t0
        y = respond(float(prob.p_fn(lv)), u[t])
        t0 = time.perf_counter()
        s.update(lv, y)
        spent += time.perf_counter() - t0
        rows.append((t + 1, stair_estimate(s), float("nan"), 1000.0 * spent / (t + 1),
                     float("nan"), float("nan")))
    return rows, {}


def stair_truth(prob, name):
    r = STAIR_RULES[name]
    if r.get("weighted"):
        return prob.thr_true
    import ysp.stair as yst
    return prob.x_at(yst.convergence_p(r["n_up"], r["n_down"]))


# The slope the psychometric model's probit implies, in the paper's units:
# the one whose 25-to-75 percent spread of F matches. For the dB Weibull,
# F = 1 - exp(-10^(b (x - t) / 20)) spreads over 20 D / b dB with
# D = log10(ln 4) - log10(ln 4/3); a probit Phi(k (x - m)) spreads over
# 2 z75 / k. For the normal, sd = 1 / k exactly.
D_WEIB = math.log10(math.log(4.0)) - math.log10(math.log(4.0 / 3.0))
Z75 = float(ndtri(0.75))


def equiv_slope(prob, k):
    if prob.key == "w4":
        return 1.0 / k
    return 20.0 * D_WEIB * k / (2.0 * Z75)


def run_gp1d(prob, rep, model, acq, fit_every=10, grid=81):
    import ysp.aep as yaep
    u = stream(prob.prob_id, rep, prob.n_trials)
    init = prob.lo + (prob.hi - prob.lo) * sobol_init(prob.prob_id, rep, 1, N_INIT)
    g = yaep.GP(lo=[prob.lo], hi=[prob.hi], acq=acq, target_p=prob.target_p,
              guess=prob.guess, lapse=prob.lapse, grid=[grid], n_init=N_INIT, fit=True,
              fit_every=fit_every, stop_trials=prob.n_trials, max_trials=prob.n_trials,
              model=model)
    rows, spent, numeric, nocross = [], 0.0, 0, 0
    for t in range(prob.n_trials):
        t0 = time.perf_counter()
        x = [float(init[t][0])] if t < N_INIT else g.next()[1]
        spent += time.perf_counter() - t0
        y = respond(float(prob.p_fn(x[0])), u[t])
        t0 = time.perf_counter()
        try:
            g.update(x, y)
        except yaep.Numeric:
            numeric += 1
        spent += time.perf_counter() - t0
        try:
            thr = g.threshold()[0]
        except yaep.NoCross:
            thr = float("nan")
            nocross += 1
        slope = float("nan")
        if model == "psychometric" and prob.slope_name:
            mu_g, _ = g.predict_f([x[0]], 1)
            slope = equiv_slope(prob, math.exp(mu_g))
        rows.append((t + 1, thr, slope, 1000.0 * spent / (t + 1), float("nan"), float("nan")))
    return rows, {"numeric": numeric, "nocross": nocross}


# --- the reference check: Watson's own runs, replayed ------------------------
#
# Watson's QUEST+ notebook (QuestPlus.nb, (c) 2017 A. B. Watson, the code
# behind the paper) saves the stimulus and outcome of every trial of its
# example runs. --replay reads the notebook, rebuilds each example's grids and
# psychometric function (written here from the formulas, not copied), and
# replays each run through ysp.quest: at every trial it asks ysp.quest for its
# selection before it feeds Watson's recorded stimulus and outcome. The paper
# reports single runs, not means and sds over replications, so this trial by
# trial agreement is the check that is available. A selection that differs is
# a TIE when ysp.quest's own expected entropies of the two stimuli differ by
# less than TIE_BITS (the float table moves a score by about 5e-8 bits).

TIE_BITS = 1e-6


def notebook_cells(path):
    """(style, text) for every cell of a text-format Mathematica notebook, with
    the box structure flattened to linear text. Enough for Input and Output
    cells that hold numbers; graphics come out as <graphics>."""
    import re
    sys.setrecursionlimit(100000)
    tok = re.compile(r'"(?:[^"\\]|\\.)*"|[A-Za-z$][A-Za-z0-9$`]*|\d+\.?\d*(?:`[\d.]*)?'
                     r'(?:\*\^-?\d+)?|[\[\]{},]|->|:>|\S', re.S)
    toks = tok.findall(open(path, encoding="utf-8", errors="replace").read())
    pos = [0]

    def parse():
        t = toks[pos[0]]
        pos[0] += 1
        if t == "{":
            items = []
            while toks[pos[0]] != "}":
                items.append(expr())
                if toks[pos[0]] == ",":
                    pos[0] += 1
            pos[0] += 1
            node = ("list", items)
        else:
            node = ("atom", t)
        while pos[0] < len(toks) and toks[pos[0]] == "[":
            pos[0] += 1
            args = []
            while toks[pos[0]] != "]":
                args.append(expr())
                if toks[pos[0]] == ",":
                    pos[0] += 1
            pos[0] += 1
            node = ("call", node, args)
        return node

    def expr():
        parts = [parse()]
        while pos[0] < len(toks) and toks[pos[0]] not in (",", "]", "}"):
            parts.append(parse())
        return parts[0] if len(parts) == 1 else ("seq", parts)

    def head(n):
        return n[1][1] if n[0] == "call" and n[1][0] == "atom" else None

    def unq(s):
        if s.startswith('"'):
            s = s[1:-1].replace('\\"', '"')
            s = re.sub(r"\\\[(\w+)\]", r"<\1>", s)
        return s

    def render(n):
        if n[0] == "atom":
            return unq(n[1])
        if n[0] == "list":
            return "{" + ",".join(render(i) for i in n[1]) + "}"
        if n[0] == "seq":
            return " ".join(render(i) for i in n[1])
        h, a = head(n), n[2]
        if h == "RowBox":
            return "".join(render(i) for i in a[0][1]) if a and a[0][0] == "list" else ""
        if h in ("GraphicsBox", "Graphics3DBox", "DynamicModuleBox", "RasterBox"):
            return "<graphics>"
        if h in ("StyleBox", "TagBox", "InterpretationBox", "FormBox", "BoxData", "Cell"):
            return render(a[0]) if a else ""
        return render(n[1]) + "[" + ",".join(render(i) for i in a) + "]"

    while not (toks[pos[0]] == "Notebook" and toks[pos[0] + 1] == "["):
        pos[0] += 1
    root = parse()
    out = []

    def walk(n):
        if head(n) == "Cell":
            a = n[2]
            if head(a[0]) == "CellGroupData":
                for c in a[0][2][0][1]:
                    walk(c)
                return
            style = (unq(a[1][1]) if len(a) > 1 and a[1][0] == "atom"
                     and a[1][1].startswith('"') else "")
            out.append((style, render(a[0])))
        elif n[0] == "list":
            for c in n[1]:
                walk(c)
    walk(root[2][0])
    return out


def mma_list(text):
    """A Mathematica numeric list ({{-21,3.5`,...},{{{-18},2},...}}) as Python."""
    import json
    import re
    t = re.sub(r"`[\d.]*", "", text).replace("*^", "e").replace(" ", "")
    t = re.sub(r"(?<=\d)\.(?=[,}\]])", ".0", t)
    t = re.sub(r"(?<![\d])\.(?=\d)", "0.", t)
    return json.loads(t.replace("{", "[").replace("}", "]"))


def _vm_arc(x, kappa, a, b, n=96):
    """von Mises(x, kappa) mass on the arc [a, b], b > a, by Gauss-Legendre."""
    from scipy.special import i0e
    g, w = np.polynomial.legendre.leggauss(n)
    a, b = np.asarray(a, float), np.asarray(b, float)
    y = 0.5 * (b - a)[:, None] * g[None, :] + 0.5 * (b + a)[:, None]
    k = np.asarray(kappa, float)[:, None]
    dens = np.exp(k * (np.cos(y - x) - 1.0)) / (2.0 * math.pi * i0e(k))
    return 0.5 * (b - a) * (dens * w[None, :]).sum(axis=1)


def _weib(c, thr, slope, guess, lapse):
    return 1.0 - lapse - (1.0 - guess - lapse) * np.exp(-10.0 ** (slope * (c - thr) / 20.0))


def _two(p1):
    return np.stack([1.0 - p1, p1], axis=1)


def pf_watson(kind):
    """Watson's example psychometric functions as NumPy batches: stim is one
    stimulus, the rows of params are parameter points; returns P x K, with
    outcome k here being Watson's outcome k + 1."""
    def f(stim, params):
        P = np.asarray(params)
        if kind == "weibull":
            return _two(_weib(stim[0], P[:, 0], P[:, 1], P[:, 2], P[:, 3]))
        if kind == "normal":
            return _two(normal_lapse(stim[0], P[:, 0], P[:, 1], P[:, 2]))
        if kind == "scsf":
            return _two(_weib(stim[1], np.maximum(P[:, 0], P[:, 1] + P[:, 2] * stim[0]),
                              3.0, 0.5, 0.01))
        if kind == "stcsf":
            thr = np.maximum(P[:, 0], P[:, 1] + P[:, 2] * stim[1] + P[:, 3] * stim[2])
            return _two(_weib(stim[0], thr, 3.0, 0.5, 0.01))
        if kind == "scaling":
            def theta(x):
                return P[:, 0] * (np.maximum(0.0, x - P[:, 1]) / (1.0 - P[:, 1])) ** P[:, 2]
            return _two(ndtr((theta(stim[1]) - theta(stim[0])) / math.sqrt(2.0)))
        if kind == "blur":
            om, be = P[:, 0], P[:, 1]
            sig = 10.0 ** stim[0]
            inc = -sig + np.sqrt(be ** 2 * (om ** 2 - 1.0) + sig ** 2 * om ** 2)
            return _two(_weib(stim[1], np.log10(inc), 40.0, 0.5, 0.01))
        if kind == "rating":
            crit = np.cumsum(P[:, 1:], axis=1)
            F = ndtr((stim[0] - crit) / P[:, :1])
            F[F < 1e-10] = 0.0
            out = np.column_stack([F[:, :-1] - F[:, 1:], F[:, -1]])
            return np.column_stack([np.clip(1.0 - out.sum(axis=1), 0.0, 1.0), out])
        if kind == "circular":
            return circular_p(stim[0], P)
        raise ValueError(kind)
    return f


def circular_p(x, P):
    """Watson's circular categorization: a von Mises centered on the stimulus
    direction, and category i is its mass between successive boundaries
    first + cumsum(widths); the last category takes the rest."""
    P = np.atleast_2d(np.asarray(P, float))
    kap, first, widths = P[:, 0], P[:, 1], P[:, 2:]
    crit = first[:, None] + np.column_stack([np.zeros(len(P)), np.cumsum(widths, axis=1)])
    m = np.column_stack([_vm_arc(x, kap, crit[:, i], crit[:, i + 1])
                         for i in range(widths.shape[1])])
    return np.column_stack([m, np.clip(1.0 - m.sum(axis=1), 0.0, 1.0)])


def _rng(a, b, step=1.0):
    n = int(round((b - a) / step)) + 1
    return [round(a + i * step, 12) for i in range(n)]


PI9 = [k * math.pi / 9.0 for k in range(18)]
WATSON_CONFIGS = {
    # name: (pf kind, stimulus axes, parameter axes, K), all in the notebook's order
    "threshold": ("weibull", [_rng(-40, 0)], [_rng(-40, 0), [3.5], [0.5], [0.02]], 2),
    "threshold, slope": ("weibull", [_rng(-40, 0)], [_rng(-40, 0), _rng(2, 5), [0.5], [0.02]], 2),
    "threshold, slope, lapse": ("weibull", [_rng(-40, 0)],
                                [_rng(-40, 0), _rng(2, 5), [0.5], _rng(0, 0.04, 0.01)], 2),
    "mean, sd, lapse": ("normal", [_rng(-10, 10)],
                        [_rng(-5, 5), _rng(1, 10), _rng(0, 0.04, 0.01)], 2),
    "spatial CSF": ("scsf", [_rng(0, 40, 2), _rng(-50, 0, 2)],
                    [_rng(-30, -50, -2), _rng(-40, -60, -2), _rng(0.8, 1.6, 0.2)], 2),
    "spatiotemporal CSF": ("stcsf", [_rng(-40, 0, 5), _rng(0, 40, 5), _rng(0, 40, 5)],
                           [_rng(-30, -50, -5), _rng(-40, -60, -5), _rng(0.8, 1.6, 0.2),
                            _rng(0.8, 1.6, 0.2)], 2),
    "Thurstone scaling": ("scaling", [_rng(0, 1, 0.1), _rng(0, 1, 0.1)],
                          [_rng(1, 10), _rng(0, 0.9, 0.1), _rng(0.1, 1, 0.1)], 2),
    "increment threshold (blur)": ("blur", [_rng(-1, 1.5, 0.25), _rng(-1, 1, 0.25)],
                                   [_rng(1.05, 1.5, 0.05), _rng(0.5, 2, 0.1)], 2),
    "rating, 3 categories": ("rating", [_rng(0, 3, 0.25)],
                             [_rng(0.05, 0.5, 0.05), _rng(0.5, 2, 0.5), _rng(0.5, 2, 0.5)], 3),
    "rating, 4 categories": ("rating", [_rng(0, 8, 0.25)],
                             [_rng(0.05, 1, 0.05)] + [_rng(0.5, 4, 0.5)] * 3, 4),
    "circular categorization": ("circular", [PI9],
                                [_rng(1, 8), PI9, [2 * math.pi / 3], [2 * math.pi / 3]], 3),
}
# notebook subsection or subsubsection title -> configuration
SECTION_CONFIG = {
    "Example result": "threshold", "QuestPlus": "threshold", "QpRun": "threshold",
    "Threshold": "threshold", "Threshold, slope": "threshold, slope",
    "Threshold, slope, lapse": "threshold, slope, lapse",
    "Mean, standard deviation, lapse": "mean, sd, lapse",
    "QpPlot2D": "spatial CSF", "Spatial contrast sensitivity function": "spatial CSF",
    "QpPlot3D": "spatiotemporal CSF",
    "Spatiotemporal contrast sensitivity function": "spatiotemporal CSF",
    "Thurstone scaling": "Thurstone scaling", "Increment threshold": "increment threshold (blur)",
    "3 categories": "rating, 3 categories", "4 categories": "rating, 4 categories",
    "Circular categorization": "circular categorization",
}


def notebook_runs(path):
    """Every saved run in the notebook: (label, configuration, estimate, trials)."""
    cells = notebook_cells(path)
    runs, sub, subsub = [], "", ""
    for i, (style, text) in enumerate(cells):
        if style == "Subsection":
            sub, subsub = text.strip(), ""
            continue
        if style == "Subsubsection":
            subsub = text.strip()
            continue
        t = text.replace("<IndentingNewLine>", "").replace(" ", "").replace("\n", "")
        src = None
        if style == "Input" and (t.startswith("result={{") or t.startswith("ExampleResult={{")):
            src = t.split("=", 1)[1].rstrip(";")
        elif (style == "Input" and ("QuestPlus[" in t or "QpRun[" in t) and "result" in t
              and ":=" not in t and not t.startswith("Timing") and i + 1 < len(cells)
              and cells[i + 1][0] == "Output" and cells[i + 1][1].lstrip().startswith("{{")):
            src = cells[i + 1][1]
        if src is None:
            continue
        cfg = SECTION_CONFIG.get(subsub) or SECTION_CONFIG.get(sub)
        if cfg is None:
            continue
        try:
            est, trials = mma_list(src)
        except Exception as e:
            print(f"  could not parse a run in {sub}: {e}")
            continue
        runs.append((f"{sub}{' / ' + subsub if subsub else ''}", cfg, est, trials))
    return runs


def replay_run(cfg_name, est, trials):
    import ysp.quest as yqst
    kind, stim_axes, par_axes, K = WATSON_CONFIGS[cfg_name]
    q = yqst.Quest([yqst.values(a) for a in stim_axes], [yqst.values(a) for a in par_axes],
                 pf_batch=pf_watson(kind), n_outcomes=K, stop_trials=100000)
    same = tie = diff = 0
    first_diff = None
    tie_max = 0.0
    for t, (x, k) in enumerate(trials):
        i = q.next()
        sub = [int(np.argmin(np.abs(np.asarray(a) - v))) for a, v in zip(stim_axes, x)]
        j = q.stim_index(sub)
        if i == j:
            same += 1
        elif abs(q.expected_entropy(i) - q.expected_entropy(j)) < TIE_BITS:
            tie += 1
            tie_max = max(tie_max, abs(q.expected_entropy(i) - q.expected_entropy(j)))
        else:
            diff += 1
            if first_diff is None:
                first_diff = (t + 1, q.stim_values(i), list(x),
                              q.expected_entropy(i) - q.expected_entropy(j))
        q.update(j, int(k) - 1)
    mode = q.estimate(yqst.EST_MODE)
    ok_mode = bool(np.allclose(mode, est, atol=1e-6))
    return dict(n=len(trials), same=same, tie=tie, diff=diff, first_diff=first_diff,
                mode=mode, est=est, ok_mode=ok_mode, tie_max=tie_max)


def replay_main(path):
    runs = notebook_runs(path)
    print(f"replay of Watson's saved runs from {path}: {len(runs)} runs")
    print(f"{'run':46s} {'configuration':26s} {'trials':>6s} {'same':>5s} {'tie':>4s} "
          f"{'diff':>5s}  mode, ysp.quest / Watson")
    bad = 0
    rows = []

    def fmt(v):
        return "[" + ", ".join(f"{x:.4g}" for x in v) + "]"
    for label, cfg, est, trials in runs:
        r = replay_run(cfg, est, trials)
        rows.append((label, cfg, r))
        bad += (r["diff"] > 0) or not r["ok_mode"]
        ties = "  (ties within %.1e bits)" % r["tie_max"] if r["tie"] else ""
        print(f"{label[:46]:46s} {cfg[:26]:26s} {r['n']:6d} {r['same']:5d} {r['tie']:4d} "
              f"{r['diff']:5d}  {fmt(r['mode'])} / {fmt(r['est'])}"
              f"{'' if r['ok_mode'] else '  MODE DIFFERS'}{ties}")
        if r["first_diff"]:
            t, a, b, dh = r["first_diff"]
            print(f"    first difference at trial {t}: ysp.quest {fmt(a)}, Watson {fmt(b)}, "
                  f"dEH {dh:+.3g} bits")
    print("PASS" if bad == 0 else f"{bad} run(s) with a non-tie difference or a different mode")
    return rows


# --- Watson's spatial CSF as a 2-D problem ------------------------------------

CSF_TRUTH = (-35.0, -50.0, 1.2)
CSF_T = _rng(-30, -50, -2)
CSF_C0 = _rng(-40, -60, -2)
CSF_CF = _rng(0.8, 1.6, 0.2)


def csf_field(prob_id):
    t, c0, cf = CSF_TRUTH
    thr = lambda f: np.maximum(t, c0 + cf * np.asarray(f))
    p = lambda f, c: _weib(np.asarray(c), thr(f), 3.0, 0.5, 0.01)
    target = float(_weib(0.0, 0.0, 3.0, 0.5, 0.01))
    return Field2D("csf", [0.0, -50.0], [40.0, 0.0], p, thr, target, "dB", 2.0,
                   [0.0, 8.0, 16.0, 24.0, 32.0, 40.0], 0.0, [8.0, 4.0, 2.0], prob_id,
                   note="Watson 2017 spatial CSF: threshold max(t, c0 + cf f) dB, Weibull slope 3",
                   guess=0.5, lapse=0.01, marks=(25, 32, 50, 100, 150))


def run_quest_csf(fd, rep, n_trials=N2D, every=5):
    """Watson's spatial CSF example: his grids, his model, entropy selection."""
    import ysp.quest as yqst
    q = yqst.Quest([yqst.values(_rng(0, 40, 2)), yqst.values(_rng(-50, 0, 2))],
                 [yqst.values(CSF_T), yqst.values(CSF_C0), yqst.values(CSF_CF)],
                 pf_batch=pf_watson("scsf"), stop_trials=n_trials)
    u = stream(fd.prob_id, rep, n_trials)
    rows, spent, params = [], 0.0, {}
    for t in range(n_trials):
        t0 = time.perf_counter()
        i = q.next()
        spent += time.perf_counter() - t0
        f, c = q.stim_values(i)
        y = respond(fd.p([f, c]), u[t])
        t0 = time.perf_counter()
        q.update(i, y)
        spent += time.perf_counter() - t0
        n = t + 1
        if n % every == 0 or n in fd.marks:
            mean = q.estimate(yqst.EST_MEAN)
            mode = q.estimate(yqst.EST_MODE)
            est = lambda cc, e=mean: np.maximum(e[0], e[1] + e[2] * np.asarray(cc))
            mp = fd.shifted(fd.xs, est)
            band = fd.band_mae(fd.shifted(fd.xs_f, est)) if n in fd.marks else float("nan")
            rows.append((n,) + fd.score(mp) + (1000.0 * spent / n, band))
            if n in fd.marks:
                params[n] = (list(mean), list(mode))
    return rows, {"params": params}


# --- Watson's circular categorization, three outcomes -------------------------

CIRC_TRUTH = (4.0, math.pi / 3, 2 * math.pi / 3, 2 * math.pi / 3)
CIRC_N = 128
CIRC_MARKS = (16, 32, 64, 128)
CIRC_EVAL = np.linspace(0.0, 2 * math.pi, 72, endpoint=False)
CIRC_PROB_ID = 301


def circ_truth_table():
    return np.vstack([circular_p(x, np.array([CIRC_TRUTH]))[0] for x in CIRC_EVAL])


def run_quest_circ(rep):
    import ysp.quest as yqst
    kind, stim_axes, par_axes, K = WATSON_CONFIGS["circular categorization"]
    q = yqst.Quest([yqst.values(a) for a in stim_axes], [yqst.values(a) for a in par_axes],
                 pf_batch=pf_watson(kind), n_outcomes=K, stop_trials=CIRC_N)
    truth = circ_truth_table()
    u = stream(CIRC_PROB_ID, rep, CIRC_N)
    rows, spent, params = [], 0.0, {}
    for t in range(CIRC_N):
        t0 = time.perf_counter()
        i = q.next()
        spent += time.perf_counter() - t0
        y = respond_k(circular_p(q.stim_value(i), np.array([CIRC_TRUTH]))[0], u[t])
        t0 = time.perf_counter()
        q.update(i, y)
        spent += time.perf_counter() - t0
        n = t + 1
        if n % 4 == 0 or n in CIRC_MARKS:
            mode = q.estimate(yqst.EST_MODE)
            # c1 is circular: its marginal mean is not meaningful, the mode is.
            model = np.vstack([circular_p(x, np.array([mode]))[0] for x in CIRC_EVAL])
            dc1 = (mode[1] - CIRC_TRUTH[1] + math.pi) % (2 * math.pi) - math.pi
            rows.append((n, float(np.mean(np.abs(model - truth))), abs(math.degrees(dc1)),
                         mode[0] - CIRC_TRUTH[0], 1000.0 * spent / n))
    return rows, {}


def run_gp_circ(rep, acq):
    import ysp.aep as yaep
    truth = circ_truth_table()
    u = stream(CIRC_PROB_ID, rep, CIRC_N)
    init = 2 * math.pi * sobol_init(CIRC_PROB_ID, rep, 1, N_INIT)
    g = yaep.GP(lo=[0.0], hi=[2 * math.pi], lik="categorical", n_outcomes=3, acq=acq,
              target_outcome=0, target_p=0.5, candidates=[[x] for x in PI9], n_init=N_INIT,
              fit=True, fit_every=10, stop_trials=CIRC_N, max_trials=CIRC_N)
    rows, spent, numeric = [], 0.0, 0
    for t in range(CIRC_N):
        t0 = time.perf_counter()
        x = [float(init[t][0])] if t < N_INIT else g.next()[1]
        spent += time.perf_counter() - t0
        y = respond_k(circular_p(x[0], np.array([CIRC_TRUTH]))[0], u[t])
        t0 = time.perf_counter()
        try:
            g.update(x, y)
        except yaep.Numeric:
            numeric += 1
        spent += time.perf_counter() - t0
        n = t + 1
        if n % 4 == 0 or n in CIRC_MARKS:
            model = np.array([g.predict_outcomes([x]) for x in CIRC_EVAL])
            rows.append((n, float(np.mean(np.abs(model - truth))), float("nan"), float("nan"),
                         1000.0 * spent / n))
    return rows, {"numeric": numeric}


# --- Letham et al. 2022: the real-data 6-D contrast sensitivity function -------
#
# bernoulli_lse's ContrastSensitivity6d (problems.py, MIT): 1001 trials of a
# human contrast detection study, a GP classifier fit to all of them, and the
# truth p(x) = Phi(max(mu(x), 0)) with mu the posterior mean of the latent,
# clamped at 0 because p is floored at 0.5. Inputs, unnormalized: log10
# contrast, log10 pedestal, temporal frequency (Hz), spatial frequency
# (c/deg), size (deg), eccentricity (deg). Target p 0.75. The data license is
# not stated, so the CSV is downloaded at run time into a cache directory and
# never copied into this repository.
#
# A truth surface is stored as a kernel expansion that numpy can evaluate:
# mu(x) = c + sum_j a_j s exp(-|((x - Z_j) / l)|^2 / 2), in box units. For
# AEPsych, Z are its 100 k-means++ inducing points and a = K_ZZ^-1 (mu(Z) - c),
# which reproduces its predictive mean (checked at build time). For ysp.aep, Z
# are the training inputs and a = K^-1 (mu(X) - m), which reproduces its
# Laplace posterior mean the same way.

CSF6_URL = ("https://raw.githubusercontent.com/facebookresearch/bernoulli_lse/"
            "refs/heads/main/data/csf_dataset.csv")
CSF6_CITE = ("Letham, B., Guan, P., Tymms, C., Bakshy, E., Shvartsman, M. (2022). "
             "Look-ahead acquisition functions for Bernoulli level set estimation. "
             "AISTATS. Data: github.com/facebookresearch/bernoulli_lse, data/csf_dataset.csv")
CSF6_LO = np.array([-1.5, -1.5, 0.0, 0.5, 1.0, 0.0])
CSF6_HI = np.array([0.0, 0.0, 20.0, 7.0, 10.0, 10.0])
CSF6_NAMES = ("log10 contrast", "log10 pedestal", "temporal frequency", "spatial frequency",
              "size", "eccentricity")
CSF6_TARGET = 0.75
CSF6_PROB_ID = 401
CSF6_N_INIT = 10
CSF6_EVERY = 10
CSF6_NCTX = 200        # random contexts for the threshold metric
CSF6_NLINE = 200       # contrast points per context
CSF6_NEVAL = 2000      # Sobol points for MAE(p)


def csf6_cache_dir(path=None):
    d = path or os.environ.get("YSP_COMPARE_CACHE") or os.path.join(
        os.path.expanduser("~"), ".cache", "ysp-compare")
    os.makedirs(d, exist_ok=True)
    return d


def csf6_data(cache):
    """(x, y): the 1001 trials, downloaded once into the cache."""
    path = os.path.join(cache, "csf_dataset.csv")
    if not os.path.exists(path):
        import urllib.request
        print(f"downloading {CSF6_URL}\n  into {path}\n  source: {CSF6_CITE}", flush=True)
        urllib.request.urlretrieve(CSF6_URL, path + ".part")
        os.replace(path + ".part", path)
    d = np.loadtxt(path, delimiter=",", skiprows=1)
    return d[:, 1:].astype(float), d[:, 0].astype(int)


def rbf_expansion_mu(xs, t):
    """The latent mean of a stored truth at the rows of xs (box units)."""
    xs = np.atleast_2d(np.asarray(xs, float))
    out = np.empty(len(xs))
    for i in range(0, len(xs), 512):
        d = (xs[i:i + 512, None, :] - t["Z"][None, :, :]) / t["ls"]
        out[i:i + 512] = t["c"] + t["s"] * np.exp(-0.5 * (d * d).sum(-1)) @ t["a"]
    return out


def csf6_p(xs, t):
    return ndtr(np.maximum(rbf_expansion_mu(xs, t), 0.0))


def _expansion(Z, mu_z, c, s, ls, jitter=1e-6):
    d = (Z[:, None, :] - Z[None, :, :]) / ls
    K = s * np.exp(-0.5 * (d * d).sum(-1))
    K[np.diag_indices_from(K)] += jitter * s
    a = np.linalg.solve(K, mu_z - c)
    return dict(Z=Z, a=a, c=float(c), s=float(s), ls=np.asarray(ls, float))


def build_truth_aepsych(x, y, seed=0):
    """AEPsych 0.8's GPClassificationModel at its defaults with 100 k-means++
    inducing points, on the unit cube (AEPsych's own config path normalizes
    the box; its lengthscale prior assumes it)."""
    import logging
    import warnings
    import torch
    warnings.filterwarnings("ignore")
    logging.getLogger().setLevel(logging.WARNING)
    from aepsych.models import GPClassificationModel
    from aepsych.models.inducing_points import KMeansAllocator
    torch.manual_seed(seed)
    span = CSF6_HI - CSF6_LO
    xu = torch.tensor((x - CSF6_LO) / span, dtype=torch.float64)
    m = GPClassificationModel(dim=6, inducing_size=100,
                              inducing_point_method=KMeansAllocator(dim=6))
    m.fit(xu, torch.tensor(y, dtype=torch.float64))
    Zu = m.variational_strategy.inducing_points.detach().numpy().astype(float)
    with torch.no_grad():
        mu_z = m.posterior(torch.tensor(Zu)).mean.numpy().reshape(-1).astype(float)
    k = m.covar_module
    s = float(k.outputscale) if hasattr(k, "outputscale") else 1.0
    base = getattr(k, "base_kernel", k)
    ls_u = base.lengthscale.detach().numpy().reshape(-1).astype(float)
    c = float(m.mean_module.constant)
    t = _expansion(Zu * span + CSF6_LO, mu_z, c, s, ls_u * span)
    # the check: the expansion against AEPsych's own mean at fresh points
    from scipy.stats import qmc
    chk = qmc.Sobol(6, scramble=True, seed=1).random(512)
    with torch.no_grad():
        ref = m.posterior(torch.tensor(chk)).mean.numpy().reshape(-1)
    t["check"] = float(np.max(np.abs(rbf_expansion_mu(chk * span + CSF6_LO, t) - ref)))
    import importlib.metadata as md
    t["source"] = (f"AEPsych {md.version('aepsych')} GPClassificationModel(dim=6, "
                   f"inducing_size=100, KMeansAllocator), unit cube, torch seed {seed}")
    return t


def build_truth_yaep(x, y, max_n=None):
    """ysp.aep's GP model (RBF ARD, probit), fitted to the trials until the fit
    stops. The binding holds at most MAX_TRIALS trials, so a longer data set is
    cut to its first MAX_TRIALS rows."""
    import ysp.aep as yaep
    n = min(len(y), max_n or yaep.MAX_TRIALS)
    g = yaep.GP(lo=list(CSF6_LO), hi=list(CSF6_HI), intensity_dim=0, target_p=CSF6_TARGET,
              n_candidates=64, n_init=0, fit=True, fit_every=0, stop_trials=n, max_trials=n)
    for xi, yi in zip(x[:n], y[:n]):
        try:
            g.update(list(xi), int(yi))
        except yaep.Numeric:
            pass
    for _ in range(20):
        before = g.log_marginal
        g.fit()
        if abs(g.log_marginal - before) < 1e-4:
            break
    h = g.hyper()
    X = np.asarray([r["x"] for r in g.history()], float)
    mu_x = np.frombuffer(g.predict_f_many(X)[0]).copy()
    t = _expansion(X, mu_x, h["mean"], h["outputscale"], h["lengthscale"][:6], jitter=1e-6)
    from scipy.stats import qmc
    chk = CSF6_LO + (CSF6_HI - CSF6_LO) * qmc.Sobol(6, scramble=True, seed=1).random(512)
    ref = np.frombuffer(g.predict_f_many(chk)[0])
    t["check"] = float(np.max(np.abs(rbf_expansion_mu(chk, t) - ref)))
    t["source"] = (f"ysp.aep {yaep.__version__} GP model, RBF ARD, probit, fitted to the first "
                   f"{n} of {len(y)} trials (MAX_TRIALS {yaep.MAX_TRIALS}); log marginal "
                   f"{g.log_marginal:.1f}; lengthscales {np.round(h['lengthscale'][:6], 3).tolist()}, "
                   f"outputscale {h['outputscale']:.3f}, mean {h['mean']:.3f}")
    return t


def csf6_truth(kind, cache):
    """The stored truth surface, built on first use."""
    path = os.path.join(cache, f"csf6_truth_{kind}.npz")
    if os.path.exists(path):
        z = np.load(path, allow_pickle=False)
        return {k: (z[k] if z[k].ndim else z[k].item()) for k in z.files}
    x, y = csf6_data(cache)
    if kind == "aepsych":
        try:
            t = build_truth_aepsych(x, y)
        except ImportError as e:
            raise SystemExit(f"--truth aepsych needs AEPsych in this interpreter ({e}). Build the "
                             f"truth once with an interpreter that has it: python "
                             f"tests/compare/methods_compare.py --build-truth aepsych --cache {cache}")
    else:
        t = build_truth_yaep(x, y)
    np.savez(path, **{k: np.asarray(v) for k, v in t.items()})
    print(f"truth {kind}: {t['source']}; expansion check {t['check']:.2e}", flush=True)
    return t


class CSF6:
    """The evaluation sets, fixed across methods and replications."""

    def __init__(self, t):
        from scipy.stats import qmc
        span = CSF6_HI - CSF6_LO
        self.t = t
        import warnings
        with warnings.catch_warnings():
            warnings.simplefilter("ignore")
            self.eval_x = CSF6_LO + span * qmc.Sobol(6, scramble=True, seed=11).random(CSF6_NEVAL)
        self.eval_p = csf6_p(self.eval_x, t)
        ctx = CSF6_LO[1:] + span[1:] * np.random.default_rng(12).random((CSF6_NCTX, 5))
        self.line = np.linspace(CSF6_LO[0], CSF6_HI[0], CSF6_NLINE)
        self.line_x = np.column_stack([np.repeat(self.line[None, :], CSF6_NCTX, 0).ravel(),
                                       np.repeat(ctx, CSF6_NLINE, 0)])
        tp = csf6_p(self.line_x, t).reshape(CSF6_NCTX, CSF6_NLINE)
        self.line_p = tp
        self.truth_thr = [column_threshold(tp[i], self.line, CSF6_TARGET) for i in range(CSF6_NCTX)]
        self.band = (tp >= 0.05) & (tp <= 0.95)
        self.all_x = np.vstack([self.eval_x, self.line_x])

    def score(self, model_p_all):
        mp = np.asarray(model_p_all)
        pe, pl = mp[:CSF6_NEVAL], mp[CSF6_NEVAL:].reshape(CSF6_NCTX, CSF6_NLINE)
        mae_p = float(np.mean(np.abs(pe - self.eval_p)))
        errs, miss = [], 0
        for i in range(CSF6_NCTX):
            tt = self.truth_thr[i]
            if tt is None:
                continue
            m = column_threshold(pl[i], self.line, CSF6_TARGET)
            if m is None:
                miss += 1
            else:
                errs.append(abs(m - tt))
        band = float(np.mean(np.abs(pl - self.line_p)[self.band])) if self.band.any() else float("nan")
        return mae_p, (float(np.mean(errs)) if errs else float("nan")), miss, band


def truth_difference(ta, tb, csf):
    """How far two truth surfaces are apart on the same evaluation sets."""
    other = CSF6(tb)
    pa, pb = csf.eval_p, other.eval_p
    d = [abs(a - b) for a, b in zip(csf.truth_thr, other.truth_thr) if a is not None and b is not None]
    only = sum((a is None) != (b is None) for a, b in zip(csf.truth_thr, other.truth_thr))
    return dict(mae_p=float(np.mean(np.abs(pa - pb))), thr_mae=float(np.mean(d)) if d else float("nan"),
                thr_max=float(np.max(d)) if d else float("nan"), n_both=len(d), n_one=only,
                cross_a=sum(t is not None for t in csf.truth_thr),
                cross_b=sum(t is not None for t in other.truth_thr))


CSF6_METHODS = [("Sobol (random)", "gp", "sobol"), ("GP, LSE", "gp", "lse"),
                ("GP, EAVC", "gp", "eavc"), ("GP, BALV", "gp", "balv"),
                ("psychometric, LSE", "psychometric", "lse"),
                ("psychometric, EAVC", "psychometric", "eavc"),
                ("psychometric, BALV", "psychometric", "balv"),
                ("psychometric, BALD", "psychometric", "bald")]
CSF6_STATE = {}


def _predict_chunked(g, xs, chunk=2000):
    """predict_p_many in pieces. The development machine ran near its commit
    limit (other work), and one 42000-point call raised MemoryError; pieces
    keep the transient allocation small and a failed piece is retried."""
    out = []
    for i in range(0, len(xs), chunk):
        for attempt in range(5):
            try:
                out.append(np.frombuffer(g.predict_p_many(xs[i:i + chunk])).copy())
                break
            except MemoryError:
                if attempt == 4:
                    raise
                time.sleep(5.0)
    return np.concatenate(out)


def run_csf6(rep, model, acq, n_trials, cache, truth_kind, n_cand=1000, fit_every=20):
    import ysp.aep as yaep
    key = (cache, truth_kind)
    if key not in CSF6_STATE:
        CSF6_STATE[key] = CSF6(csf6_truth(truth_kind, cache))
    csf = CSF6_STATE[key]
    u = stream(CSF6_PROB_ID, rep, n_trials)
    sob = CSF6_LO + (CSF6_HI - CSF6_LO) * sobol_init(CSF6_PROB_ID, rep, 6, 2 ** int(
        math.ceil(math.log2(max(n_trials, 2)))))[:n_trials]
    g = yaep.GP(lo=list(CSF6_LO), hi=list(CSF6_HI), intensity_dim=0, target_p=CSF6_TARGET,
              acq="lse" if acq == "sobol" else acq, n_candidates=n_cand, n_init=CSF6_N_INIT,
              fit=True, fit_every=fit_every, stop_trials=n_trials, max_trials=n_trials,
              refine_steps=2, model=model, guess=0.5)
    rows, spent, numeric = [], 0.0, 0
    for t in range(n_trials):
        t0 = time.perf_counter()
        x = list(sob[t]) if (t < CSF6_N_INIT or acq == "sobol") else g.next()[1]
        spent += time.perf_counter() - t0
        y = respond(float(csf6_p(np.array([x]), csf.t)[0]), u[t])
        t0 = time.perf_counter()
        try:
            g.update(x, y)
        except yaep.Numeric:
            numeric += 1
        spent += time.perf_counter() - t0
        n = t + 1
        if n % CSF6_EVERY == 0:
            rows.append((n,) + csf.score(_predict_chunked(g, csf.all_x)) + (1000.0 * spent / n,))
    return rows, {"numeric": numeric, "hyper": g.hyper()}


# --- the job table ------------------------------------------------------------

GP2D = [("GP, LSE", "gp", "lse"), ("GP, EAVC", "gp", "eavc"), ("GP, BALV", "gp", "balv"),
        ("psychometric, LSE", "psychometric", "lse"), ("psychometric, EAVC", "psychometric", "eavc"),
        ("psychometric, BALV", "psychometric", "balv"), ("psychometric, BALD", "psychometric", "bald")]
GP1D = [("GP, LSE", "gp", "lse"), ("GP, EAVC", "gp", "eavc"),
        ("psychometric, LSE", "psychometric", "lse"), ("psychometric, EAVC", "psychometric", "eavc")]
GPCIRC = [("GP categorical, BALD", "bald"), ("GP categorical, BALV", "balv")]


def problems_2d():
    out = {}
    pid = 200
    for ph in ("older-normal", "sensory", "metabolic", "metabolic+sensory"):
        for beta in (2.0, 0.5):
            pid += 1
            out[f"audio-{ph}-b{beta:g}"] = ("audio", ph, beta, pid)
    out["novel-det"] = ("novel", "det", None, 221)
    out["novel-dis"] = ("novel", "dis", None, 222)
    out["csf"] = ("csf", None, None, 231)
    return out


_FIELDS = {}


def field_for(key):
    if key not in _FIELDS:
        kind, a, b, pid = problems_2d()[key]
        if kind == "audio":
            _FIELDS[key] = audiogram_field(a, b, pid)
        elif kind == "novel":
            _FIELDS[key] = novel_field(a, pid)
        else:
            _FIELDS[key] = csf_field(pid)
    return _FIELDS[key]


def methods_for(key):
    """(label, cost class) for every method that applies to a problem."""
    if key in WATSON:
        pr = WATSON[key]
        return ([(n, "cheap") for n in STAIR_RULES] + [(n, "cheap") for n in pr.quest_cfgs]
                + [(n, "gp1") for n, _, _ in GP1D])
    if key == "circ":
        return [("QUEST+ (Watson's grid)", "cheap")] + [(n, "gpc") for n, _ in GPCIRC]
    if key == "csf6":
        return [(n, "csf6") for n, _, _ in CSF6_METHODS]
    out = [("interleaved staircases (informed)", "cheap")]
    if key == "csf":
        out.append(("QUEST+ (Watson's model and grid)", "cheap"))
    return out + [(n, "gp2") for n, _, _ in GP2D]


WATSON = watson_problems()


def job(key, method, rep, cfg=None):
    t0 = time.perf_counter()
    try:
        if key == "csf6":
            _, model, acq = next(m for m in CSF6_METHODS if m[0] == method)
            rows, extra = run_csf6(rep, model, acq, cfg["csf6_trials"], cfg["cache"], cfg["truth"])
        elif key in WATSON:
            pr = WATSON[key]
            if method in STAIR_RULES:
                rows, extra = run_stair1d(pr, rep, method)
            elif method in pr.quest_cfgs:
                rows, extra = run_quest1d(pr, rep, method)
            else:
                _, model, acq = next(m for m in GP1D if m[0] == method)
                rows, extra = run_gp1d(pr, rep, model, acq)
        elif key == "circ":
            if method.startswith("QUEST+"):
                rows, extra = run_quest_circ(rep)
            else:
                rows, extra = run_gp_circ(rep, next(a for n, a in GPCIRC if n == method))
        else:
            fd = field_for(key)
            if method.startswith("interleaved"):
                rows, extra = run_stair2d(fd, rep)
            elif method.startswith("QUEST+"):
                rows, extra = run_quest_csf(fd, rep)
            else:
                _, model, acq = next(m for m in GP2D if m[0] == method)
                rows, extra = run_gp2d(fd, rep, model, acq)
        err = None
    except Exception:
        import traceback
        rows, extra, err = [], {}, traceback.format_exc()
    return key, method, rep, rows, extra, err, time.perf_counter() - t0


# --- reporting ------------------------------------------------------------------

# ms per trial from the serial timing pass, (problem, method) -> ms; the
# tables fall back to the loaded parallel number where it is missing.
TIMING = {}
MS_INDEX = {"1d": 3, "2d": 4, "circ": 4}


def ms_of(key, method, fallback):
    v = TIMING.get((key, method))
    return v if v is not None else fallback

def fmt_ci(vals, f="%.2f"):
    m, c, n = mean_ci(vals)
    return (f + " +- " + f) % (m, c) if n else "n/a"


def first_below(curve, crit):
    """The first trial count from which the replication mean stays at or
    below crit to the end of the run. Sustained, not first touched: Watson's
    uniform priors put the posterior mean on the true threshold before the
    first trial, so a first touch would credit the grid, not the method."""
    ns = sorted(curve)
    best = None
    for n in reversed(ns):
        v = curve[n]
        if v == v and v <= crit:
            best = n
        else:
            break
    if best is None:
        return "> %d" % ns[-1] if ns else "n/a"
    return str(best)


def table_1d(key, res, reps_of, out):
    pr = WATSON[key]
    marks = (32, 64, 128)
    out.append(f"\n== {key}: {pr.title}; truth {pr.truth}; paper trials {pr.n_paper}; "
               f"criterion {pr.criterion:g} {pr.units}; target p {pr.target_p:.4f} ==")
    out.append("threshold error = estimate - truth, " + pr.units + "; bias +- 1.96 sd/sqrt(n), [sd]; "
               "slope at the paper's trial count, mean [sd] (truth "
               f"{pr.slope_true if pr.slope_true is not None else 'n/a'})")
    hdr = (f"{'method':44s} {'reps':>4s} " + " ".join(f"{'bias@' + str(m):>22s}" for m in marks)
           + f" {'MAE@' + str(pr.n_paper):>9s} {'slope@' + str(pr.n_paper):>14s} {'to crit':>8s} {'ms/trial':>8s}")
    out.append(hdr)
    for method, _ in methods_for(key):
        runs = res.get((key, method), [])
        if not runs:
            continue
        truth = stair_truth(pr, method) if method in STAIR_RULES else pr.thr_true
        per_n = {}
        for rows, _ in runs:
            for r in rows:
                per_n.setdefault(r[0], []).append(r)
        cells = []
        for m in marks:
            e = [r[1] - truth for r in per_n.get(m, []) if r[1] == r[1]]
            mm, c, n = mean_ci(e)
            sd = float(np.std(e, ddof=1)) if len(e) > 1 else float("nan")
            miss = len(per_n.get(m, [])) - len(e)
            cells.append(f"{mm:+6.2f} +-{c:5.2f} [{sd:5.2f}]" + (f"*{miss}" if miss else ""))
        e = [abs(r[1] - truth) for r in per_n.get(pr.n_paper, []) if r[1] == r[1]]
        mae = f"{np.mean(e):9.2f}" if e else "      n/a"
        sl = [r[2] for r in per_n.get(pr.n_paper, []) if r[2] == r[2]]
        slope = f"{np.mean(sl):6.2f} [{np.std(sl, ddof=1):5.2f}]" if len(sl) > 1 else "           n/a"
        curve = {n: float(np.mean([abs(r[1] - truth) for r in v if r[1] == r[1]]))
                 if any(r[1] == r[1] for r in v) else float("nan") for n, v in per_n.items()}
        ms = ms_of(key, method, float(np.mean([r[3] for r in per_n.get(pr.n_trials, [])])))
        out.append(f"{method[:44]:44s} {len(runs):4d} " + " ".join(f"{c:>22s}" for c in cells)
                   + f" {mae} {slope:>14s} {first_below(curve, pr.criterion):>8s} {ms:8.3f}")
        if method == pr.paper_cfg:
            mode = [r[4] - truth for r in per_n.get(pr.n_paper, [])]
            mslope = [r[5] for r in per_n.get(pr.n_paper, []) if r[5] == r[5]]
            out.append(f"{'  (the same runs, joint mode: Watson estimator)':44s}      "
                       f"threshold bias {np.mean(mode):+.2f} [sd {np.std(mode, ddof=1):.2f}] at {pr.n_paper}"
                       + (f"; slope {np.mean(mslope):.2f} [sd {np.std(mslope, ddof=1):.2f}]" if mslope else ""))


def table_2d(key, res, out):
    fd = field_for(key)
    out.append(f"\n== {key}: {fd.note}; target p {fd.target_p:.4f}; criterion {fd.criterion:g} "
               f"{fd.units} (threshold MAE); mean +- 1.96 sd/sqrt(n) ==")
    last = max(fd.marks)
    hdr = (f"{'method':36s} {'reps':>4s} {'MAE(p)@' + str(last):>18s} {'band MAE@' + str(last):>18s} "
           f"{'thr MAE@50':>16s} {'thr MAE@' + str(last):>16s} {'nocross':>7s} {'to crit':>8s} {'ms/trial':>8s}")
    out.append(hdr)
    for method, _ in methods_for(key):
        runs = res.get((key, method), [])
        if not runs:
            continue
        per_n = {}
        for rows, _ in runs:
            for r in rows:
                per_n.setdefault(r[0], []).append(r)
        at = lambda n, i: [r[i] for r in per_n.get(n, [])]
        curve = {n: float(np.nanmean([r[2] for r in v])) for n, v in per_n.items()}
        ms = ms_of(key, method, float(np.mean(at(last, 4))))
        out.append(f"{method[:36]:36s} {len(runs):4d} {fmt_ci(at(last, 1), '%.4f'):>18s} "
                   f"{fmt_ci(at(last, 5), '%.4f'):>18s} {fmt_ci(at(50, 2)):>16s} {fmt_ci(at(last, 2)):>16s} "
                   f"{np.mean(at(last, 3)):7.2f} {first_below(curve, fd.criterion):>8s} {ms:8.3f}")
    if key == "csf":
        runs = res.get((key, "QUEST+ (Watson's model and grid)"), [])
        if runs:
            out.append("QUEST+ parameter estimates (truth t -35, c0 -50, cf 1.2): mean [sd] over replications")
            for n in (32, 150):
                for j, lab in ((0, "marginal mean"), (1, "joint mode (Watson)")):
                    v = np.array([e["params"][n][j] for _, e in runs if n in e.get("params", {})])
                    if len(v):
                        out.append(f"  n = {n:3d} {lab:20s} t {v[:, 0].mean():7.2f} [{v[:, 0].std(ddof=1):.2f}]"
                                   f"  c0 {v[:, 1].mean():7.2f} [{v[:, 1].std(ddof=1):.2f}]"
                                   f"  cf {v[:, 2].mean():6.3f} [{v[:, 2].std(ddof=1):.3f}]")


def table_circ(res, out):
    out.append("\n== circ: Watson 2017 circular categorization, 3 categories, truth kappa 4, c1 60 deg, "
               "widths 120 deg; criterion MAE(p) 0.05 over 72 directions ==")
    out.append(f"{'method':32s} {'reps':>4s} {'MAE(p)@64':>18s} {'MAE(p)@128':>18s} "
               f"{'|c1 err|@64 deg':>16s} {'kappa bias@64':>16s} {'to crit':>8s} {'ms/trial':>8s}")
    for method, _ in methods_for("circ"):
        runs = res.get(("circ", method), [])
        if not runs:
            continue
        per_n = {}
        for rows, _ in runs:
            for r in rows:
                per_n.setdefault(r[0], []).append(r)
        at = lambda n, i: [r[i] for r in per_n.get(n, [])]
        curve = {n: float(np.mean([r[1] for r in v])) for n, v in per_n.items()}
        c1 = at(64, 2)
        out.append(f"{method[:32]:32s} {len(runs):4d} {fmt_ci(at(64, 1), '%.4f'):>18s} "
                   f"{fmt_ci(at(128, 1), '%.4f'):>18s} "
                   f"{(fmt_ci(c1, '%.1f') if all(v == v for v in c1) else 'n/a'):>16s} "
                   f"{(fmt_ci(at(64, 3)) if all(v == v for v in at(64, 3)) else 'n/a'):>16s} "
                   f"{first_below(curve, 0.05):>8s} {ms_of('circ', method, np.mean(at(128, 4))):8.2f}")


CSF6_DIFF = {}


def table_csf6(res, cfg, out):
    t = csf6_truth(cfg["truth"], cfg["cache"])
    out.append(f"\n== csf6: Letham et al. 2022 real-data CSF, 6-D; truth {t['source']}; "
               f"target p {CSF6_TARGET}; {CSF6_N_INIT} Sobol + {cfg['csf6_trials'] - CSF6_N_INIT} "
               f"trials; criterion 0.1 log10 contrast; ms/trial from the loaded parallel run ==")
    if CSF6_DIFF:
        d = CSF6_DIFF
        out.append(f"truth vs {d['other']} truth: MAE(p) {d['mae_p']:.4f} over {CSF6_NEVAL} Sobol points; "
                   f"0.75 threshold along contrast differs by {d['thr_mae']:.3f} mean, "
                   f"{d['thr_max']:.3f} max log10 units at the {d['n_both']} of {CSF6_NCTX} contexts where "
                   f"both cross ({d['cross_a']} and {d['cross_b']} cross; {d['n_one']} cross in one only)")
    last = cfg["csf6_trials"]
    mid = max(CSF6_EVERY, (last // 2) // CSF6_EVERY * CSF6_EVERY)
    out.append(f"{'method':22s} {'reps':>4s} {'MAE(p)@' + str(mid):>18s} {'MAE(p)@' + str(last):>18s} "
               f"{'band@' + str(last):>18s} {'thr MAE@' + str(mid):>16s} {'thr MAE@' + str(last):>16s} "
               f"{'nocross':>7s} {'to crit':>8s} {'ms/trial':>8s}")
    for method, _ in methods_for("csf6"):
        runs = res.get(("csf6", method), [])
        if not runs:
            continue
        per_n = {}
        for rows, _ in runs:
            for r in rows:
                per_n.setdefault(r[0], []).append(r)
        at = lambda n, i: [r[i] for r in per_n.get(n, [])]
        curve = {n: float(np.nanmean([r[2] for r in v])) for n, v in per_n.items()}
        out.append(f"{method:22s} {len(runs):4d} {fmt_ci(at(mid, 1), '%.4f'):>18s} "
                   f"{fmt_ci(at(last, 1), '%.4f'):>18s} {fmt_ci(at(last, 4), '%.4f'):>18s} "
                   f"{fmt_ci(at(mid, 2), '%.3f'):>16s} {fmt_ci(at(last, 2), '%.3f'):>16s} "
                   f"{np.mean(at(last, 3)):7.1f} {first_below(curve, 0.1):>8s} "
                   f"{np.mean(at(last, 5)):8.1f}")


def write_csv(path, key, res):
    with open(path, "w") as fh:
        if key == "csf6":
            fh.write("problem,method,rep,trial,mae_p,mae_thr,nocross_ctx,band_mae_p,ms_per_trial\n")
        elif key in WATSON:
            fh.write("problem,method,rep,trial,threshold_est,slope_est,ms_per_trial,threshold_mode,slope_mode\n")
        elif key == "circ":
            fh.write("problem,method,rep,trial,mae_p,c1_abs_err_deg,kappa_err,ms_per_trial\n")
        else:
            fh.write("problem,method,rep,trial,mae_p,mae_thr,nocross_cols,ms_per_trial,band_mae_p\n")
        for (k, m), runs in sorted(res.items()):
            if k != key:
                continue
            for rep, (rows, _) in enumerate(runs):
                for r in rows:
                    fh.write(f"{k},\"{m}\",{rep}," + ",".join(f"{v:.6g}" for v in r) + "\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--out", required=False, default=None, help="directory for the raw CSVs")
    ap.add_argument("--only", default=None, help="comma list of problem keys")
    ap.add_argument("--reps-cheap", type=int, default=100, dest="reps_cheap")
    ap.add_argument("--reps-gp1", type=int, default=50, dest="reps_gp1")
    ap.add_argument("--reps-gp2", type=int, default=20, dest="reps_gp2")
    ap.add_argument("--reps-gpc", type=int, default=20, dest="reps_gpc")
    ap.add_argument("--quick", action="store_true", help="3 replications of everything")
    ap.add_argument("--workers", type=int, default=8)
    ap.add_argument("--timing-reps", type=int, default=1, dest="timing_reps",
                    help="replications per method rerun serially in this process for ms/trial")
    ap.add_argument("--replay", default=None, metavar="QuestPlus.nb",
                    help="replay Watson's saved runs from his notebook through ysp.quest and exit")
    ap.add_argument("--cache", default=None,
                    help="download cache for the csf6 data and truth surfaces "
                         "(default $YSP_COMPARE_CACHE or ~/.cache/ysp-compare)")
    ap.add_argument("--truth", default=None, choices=("aepsych", "yaep"),
                    help="csf6 truth surface: AEPsych's GP classifier (as the paper) or ysp.aep's "
                         "GP model; default aepsych when it is importable or already cached")
    ap.add_argument("--build-truth", default=None, choices=("aepsych", "yaep"), dest="build_truth",
                    help="build one csf6 truth surface into the cache and exit")
    ap.add_argument("--csf6-trials", type=int, default=500, dest="csf6_trials",
                    help="csf6 session length: 10 Sobol trials, then adaptive ones")
    ap.add_argument("--reps-csf6", type=int, default=10, dest="reps_csf6")
    ap.add_argument("--classes", default=None,
                    help="comma list of method classes to run: cheap (staircases, QUEST+), gp1, gp2, "
                         "gpc, csf6; default all. For rerunning one library's rows after a change")
    args = ap.parse_args()
    args.cache = csf6_cache_dir(args.cache)
    if args.build_truth:
        csf6_truth(args.build_truth, args.cache)
        return 0
    if args.replay:
        rows = replay_main(args.replay)
        return 0 if all(r["diff"] == 0 and r["ok_mode"] for _, _, r in rows) else 1
    if args.quick:
        args.reps_cheap = args.reps_gp1 = args.reps_gp2 = args.reps_gpc = 3
    # csf6 runs only when --only names it: it takes hours, not minutes.
    keys = list(WATSON) + list(problems_2d()) + ["circ"]
    if args.only:
        keys = [k for k in keys + ["csf6"] if k in args.only.split(",")]
    if args.quick:
        args.reps_csf6 = min(args.reps_csf6, 3)
    cfg = {"cache": args.cache, "csf6_trials": args.csf6_trials, "truth": args.truth}
    if "csf6" in keys:
        if cfg["truth"] is None:
            have = os.path.exists(os.path.join(args.cache, "csf6_truth_aepsych.npz"))
            if not have:
                try:
                    import aepsych  # noqa: F401
                    have = True
                except ImportError:
                    pass
            cfg["truth"] = "aepsych" if have else "yaep"
        t = csf6_truth(cfg["truth"], args.cache)   # build before the workers need it
        print(f"csf6 truth: {t['source']}; expansion check {t['check']:.1e}")
        other = "yaep" if cfg["truth"] == "aepsych" else "aepsych"
        try:
            d = truth_difference(t, csf6_truth(other, args.cache), CSF6(t))
            CSF6_DIFF.update(d, other=other)
        except SystemExit:
            print(f"csf6: the {other} truth is not available, so the two truths are not compared")
    reps = {"cheap": args.reps_cheap, "gp1": args.reps_gp1, "gp2": args.reps_gp2,
            "gpc": args.reps_gpc, "csf6": args.reps_csf6}
    classes = set(args.classes.split(",")) if args.classes else set(reps)
    jobs = [(k, m, r) for k in keys for m, cls in methods_for(k) if cls in classes
            for r in range(reps[cls])]
    # The slow jobs first, so the pool does not end on one long straggler.
    order = {"csf6": -1, "gp2": 0, "gpc": 1, "gp1": 2, "cheap": 3}
    cls_of = {(k, m): c for k in keys for m, c in methods_for(k)}
    jobs.sort(key=lambda j: order[cls_of[(j[0], j[1])]])

    import ysp.aep
    import ysp.quest
    import ysp.stair
    print("methods_compare: ysp.stair %s, ysp.quest %s, ysp.aep %s (compiled headers); numpy %s; "
          "python %s" % (ysp.stair.__version__, ysp.quest.__version__, ysp.aep.__version__,
                         np.__version__, sys.version.split()[0]))
    print(f"{len(jobs)} jobs on {args.workers} worker processes (one thread each), "
          f"{os.cpu_count()} logical CPUs; replications {reps}; seed {SEED}")
    print(f"machine load at start: {machine_load()}", flush=True)
    res, fails = {}, []
    t_start = time.perf_counter()
    done = 0
    with ProcessPoolExecutor(max_workers=args.workers) as ex:
        futs = [ex.submit(job, k, m, r, cfg) for k, m, r in jobs]
        for f in as_completed(futs):
            k, m, r, rows, extra, err, secs = f.result()
            done += 1
            if err:
                fails.append((k, m, r, err))
                print(f"  FAILED {k} {m} rep {r}: {err.splitlines()[-1]}", flush=True)
            else:
                res.setdefault((k, m), []).append((r, rows, extra))
            if done % 200 == 0 or cls_of[(k, m)] == "csf6":
                print(f"  {done}/{len(jobs)} jobs, {time.perf_counter() - t_start:.0f} s, "
                      f"load {machine_load()}", flush=True)
    wall = time.perf_counter() - t_start
    print(f"machine load at end: {machine_load()}; wall time {wall:.0f} s")
    res = {km: [(rows, extra) for _, rows, extra in sorted(v, key=lambda x: x[0])]
           for km, v in res.items()}
    if args.timing_reps:
        # The pool above shares the machine among its workers, so its times are
        # loaded times. ms/trial in the tables comes from this serial pass.
        print(f"serial timing pass, {args.timing_reps} replication(s) per method; "
              f"load before: {machine_load()}", flush=True)
        for k in keys:
            if k == "csf6":
                continue   # a serial csf6 pass would take hours; its times are loaded ones
            kind = "1d" if k in WATSON else ("circ" if k == "circ" else "2d")
            for m, c in methods_for(k):
                if c not in classes:
                    continue
                ms = []
                for r in range(args.timing_reps):
                    _, _, _, rows, _, err, _ = job(k, m, r)
                    if rows and not err:
                        ms.append(rows[-1][MS_INDEX[kind]])
                if ms:
                    TIMING[(k, m)] = float(np.mean(ms))
        print(f"serial timing pass done; load after: {machine_load()}", flush=True)

    out = []
    for k in keys:
        if k in WATSON:
            table_1d(k, res, reps, out)
        elif k == "circ":
            table_circ(res, out)
        elif k == "csf6":
            table_csf6(res, cfg, out)
        else:
            table_2d(k, res, out)
    print("\n".join(out))
    if fails:
        print(f"\n{len(fails)} failed jobs; first: {fails[0][:3]}\n{fails[0][3]}")
    if args.out:
        os.makedirs(args.out, exist_ok=True)
        for k in keys:
            write_csv(os.path.join(args.out, f"raw_{k}.csv"), k, res)
        with open(os.path.join(args.out, "tables.txt"), "w") as fh:
            fh.write("\n".join(out) + "\n")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
