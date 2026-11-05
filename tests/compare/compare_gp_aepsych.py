#!/usr/bin/env python3
"""ysp.aep beside AEPsych on the audiometric benchmark of Owen et al. 2021.

The protocol of examples/aep/audiometric.c, run through the Python binding and
through AEPsych's server-free API in the same process tree, on one response
stream:

- The observer: the metabolic phenotype of Dubno et al. 2013 (thresholds at 8
  audiometric frequencies), a not-a-knot cubic spline in kHz between them and
  the end tangent beyond them, and p = Phi((xi - theta(f)) / beta) with
  beta = 2 dB. The box is log2(f / kHz) in [-3, 4] by intensity in
  [-20, 120] dB HL.
- The session: 5 Sobol trials, then 145 adaptive ones, target p = 0.75.
  Both libraries get the SAME 5 init points (AEPsych's SobolGenerator with the
  replication's seed, which ysp.aep is fed through update()) and the SAME
  uniform variate on every trial, drawn once per replication from a seeded
  numpy Generator: the response is 1 when u >= 1 - p. So the two runs differ
  only in where the adaptive trials go and in what the model makes of them.
- ysp.aep: RBF kernel, probit Bernoulli, candidates on an 11 x 21 grid
  (M = 231, the C benchmark's), hyperparameters fitted every 20 trials.
- AEPsych: GPClassificationModel with its defaults (variational GP, an RBF
  kernel with a lognormal lengthscale prior and its output scale fixed at 1
  unless --aepsych-scale, 100 inducing points), refit from scratch every trial (the
  Strategy default), OptimizeAcqfGenerator with its defaults, on the unit
  square, which is what AEPsych's own config path does with its normalizing
  parameter transform. The acquisitions are MCLevelSetEstimation (LSE), EAVC
  and MCPosteriorVariance (BALV), each with the probit objective and target
  0.75.
- The metrics, as the paper defines them, on a 30 x 30 grid: the mean absolute
  error of E[p] against the true p, and the mean absolute error of the 0.75
  threshold found by bracketing along intensity per frequency column (the
  first bracketing pair, linearly interpolated, identically for truth and
  model), over the columns where both cross.
- Wall time per trial: next + update for ysp.aep; gen (which includes the
  model fit) + add_data for AEPsych. The metric grid is not charged.

Replications run in worker processes, one job per (library, method,
replication), each pinned to one torch thread so the per-trial times are
single-core numbers and the jobs do not fight over cores.

Usage (from the repository root, in a venv with ysp-aep, numpy, scipy and
aepsych installed):

    python tests/compare/compare_gp_aepsych.py                 # 10 reps, both
    python tests/compare/compare_gp_aepsych.py --reps 3 --libs ysp
    python tests/compare/compare_gp_aepsych.py --csv curves.csv --workers 8
    python tests/compare/compare_gp_aepsych.py --same-data sobol,ysp-lse

--same-data takes the acquisition out of the comparison: each replication is
one fixed trial sequence (150 Sobol points, or the trials ysp.aep LSE with
refine_steps = 2 chose), both models are fitted from scratch to its first 25,
50, 100 and 150 trials, and the table compares them with the truth and with
each other, with MAE(p) split into the transition band (true p in 0.05..0.95)
and outside it, and the fitted hyperparameters of both.

Not a CI test: AEPsych pulls PyTorch. The output table is what goes into
ysp/aep.h's STATUS block.
"""
import argparse
import math
import os
import sys
import time
from concurrent.futures import ProcessPoolExecutor, as_completed

import numpy as np
from scipy.interpolate import CubicSpline
from scipy.special import ndtr

# --- the test field (examples/aep/audiometric.c) -----------------------------

AUDIO_F = np.array([0.25, 0.5, 1.0, 2.0, 3.0, 4.0, 6.0, 8.0])
PHENO = {
    "older-normal":      [6.82, 5.49, 3.51, 5.91, 6.70, 10.09, 13.47, 12.97],
    "sensory":           [5.52, 4.19, 5.62, 19.84, 42.00, 53.33, 62.05, 66.09],
    "metabolic":         [21.23, 22.01, 24.24, 33.93, 41.36, 47.17, 54.12, 58.31],
    "metabolic+sensory": [20.26, 20.71, 21.97, 37.49, 53.18, 64.02, 75.01, 76.61],
}
XC_LO, XC_HI = -3.0, 4.0
XI_LO, XI_HI = -20.0, 120.0
LO = np.array([XC_LO, XI_LO])
HI = np.array([XC_HI, XI_HI])
NGRID = 30
N_INIT = 5
TARGET_P = 0.75
MARKS = (25, 50, 100, 150)
METHODS = ("lse", "eavc", "balv")


class Field:
    """theta(f) and the truth on the metric grid for one phenotype and beta."""

    def __init__(self, pheno="metabolic", beta=2.0):
        thr = np.array(PHENO[pheno])
        # not-a-knot in kHz, as the C file; its end extrapolation is the end
        # TANGENT, not the end cubic, so SciPy's own extrapolation is off.
        self.cs = CubicSpline(AUDIO_F, thr, bc_type="not-a-knot", extrapolate=False)
        self.f0, self.f1 = AUDIO_F[0], AUDIO_F[-1]
        self.v0, self.v1 = self.cs(self.f0), self.cs(self.f1)
        self.d0, self.d1 = self.cs(self.f0, 1), self.cs(self.f1, 1)
        self.beta = beta
        self.xc = np.linspace(XC_LO, XC_HI, NGRID)
        self.xi = np.linspace(XI_LO, XI_HI, NGRID)
        # context-major, intensity-minor, as truth_p in the C file
        cc, ii = np.meshgrid(self.xc, self.xi, indexing="ij")
        self.xs = np.column_stack([cc.ravel(), ii.ravel()])
        self.truth_p = self.p(self.xs[:, 0], self.xs[:, 1]).reshape(NGRID, NGRID)
        self.truth_thr = [column_threshold(self.truth_p[i], self.xi) for i in range(NGRID)]

    def theta(self, xc):
        f = np.exp2(np.asarray(xc, dtype=float))
        out = np.where(f <= self.f0, self.v0 + self.d0 * (f - self.f0),
                       np.where(f >= self.f1, self.v1 + self.d1 * (f - self.f1),
                                self.cs(np.clip(f, self.f0, self.f1))))
        return out

    def p(self, xc, xi):
        return ndtr((np.asarray(xi) - self.theta(xc)) / self.beta)

    def band_mae(self, model_p):
        """MAE of p over the transition band, the cells with true p in
        0.05..0.95, where a stationary kernel's width shows."""
        band = (self.truth_p >= 0.05) & (self.truth_p <= 0.95)
        return float(np.mean(np.abs(np.asarray(model_p).reshape(NGRID, NGRID) - self.truth_p)[band]))

    def score(self, model_p):
        """(MAE of p, MAE of the 0.75 threshold in dB, columns not scored)."""
        model_p = np.asarray(model_p).reshape(NGRID, NGRID)
        mae_p = float(np.mean(np.abs(model_p - self.truth_p)))
        errs, miss = [], 0
        for i in range(NGRID):
            t = column_threshold(model_p[i], self.xi)
            if t is None or self.truth_thr[i] is None:
                miss += 1
            else:
                errs.append(abs(t - self.truth_thr[i]))
        return mae_p, (float(np.mean(errs)) if errs else float("nan")), miss


def column_threshold(p, xi, target=TARGET_P):
    for j in range(len(p) - 1):
        a, b = p[j] - target, p[j + 1] - target
        if (a <= 0.0 <= b) or (a >= 0.0 >= b):
            if b == a:
                continue
            return xi[j] + (xi[j + 1] - xi[j]) * (-a) / (b - a)
    return None


def spline_selfcheck(field):
    """The knots, and agreement with the C file's end-tangent rule."""
    thr = np.array(PHENO["metabolic"])
    assert np.allclose(field.theta(np.log2(AUDIO_F)), thr, atol=1e-9)
    # just outside each end, the tangent line, continuous with the spline
    eps = 1e-6
    assert abs(field.theta(np.log2(field.f0) - eps) - field.v0) < 1e-3
    assert abs(field.theta(np.log2(field.f1) + eps) - field.v1) < 1e-3


def respond(p, u):
    """yaep_simulate_outcome([1 - p, p], u), shared by both libraries."""
    return 0 if u < 1.0 - p else 1


# --- the stream ---------------------------------------------------------------

def stream(seed, rep, n_trials):
    """One uniform per trial, the same for both libraries."""
    return np.random.default_rng([seed, rep]).random(n_trials)


def sobol_init(rep):
    """AEPsych's own Sobol init points on the unit square, or SciPy's when
    AEPsych is not installed (then the ysp.aep half cannot be paired anyway)."""
    try:
        import torch
        from aepsych.generators import SobolGenerator
        gen = SobolGenerator(lb=torch.zeros(2, dtype=torch.float64),
                             ub=torch.ones(2, dtype=torch.float64), seed=rep)
        return gen.gen(N_INIT).numpy().astype(float), "aepsych"
    except ImportError:
        from scipy.stats import qmc
        import warnings
        with warnings.catch_warnings():
            warnings.simplefilter("ignore")
            return qmc.Sobol(2, scramble=True, seed=rep).random(N_INIT), "scipy"


def to_box(unit):
    return LO + (HI - LO) * np.asarray(unit)


# --- the two runners ------------------------------------------------------------

def run_ysp(method, rep, args):
    import ysp.aep as yaep
    field = Field(args.pheno, args.beta)
    u = stream(args.seed, rep, args.trials)
    init, _ = sobol_init(rep)
    g = yaep.GP(lo=list(LO), hi=list(HI), intensity_dim=1, kernel=yaep.KERNEL_RBF,
              acq=method, target_p=TARGET_P, grid=list(args.grid), n_init=N_INIT,
              fit=True, fit_every=args.fit_every, stop_trials=args.trials,
              max_trials=args.trials, refine_steps=args.refine, model=args.model)
    rows, spent, numeric = [], 0.0, 0
    for t in range(args.trials):
        t0 = time.perf_counter()
        if t < N_INIT:
            x = list(to_box(init[t]))
        else:
            _, x = g.next()
        y = respond(float(field.p(x[0], x[1])), u[t])
        try:
            g.update(x, y)
        except yaep.Numeric:
            numeric += 1        # recorded; the previous posterior stays
        spent += time.perf_counter() - t0
        n = t + 1
        if n % args.every == 0 or n in MARKS:
            mp = np.frombuffer(g.predict_p_many(field.xs))
            rows.append((n,) + field.score(mp) + (1000.0 * spent / n, field.band_mae(mp)))
    extra = {"log_marginal": g.log_marginal, "numeric": numeric,
             "hyper": g.hyper()}
    return rows, extra


def run_aepsych(method, rep, args):
    import logging
    import warnings
    import torch
    warnings.filterwarnings("ignore")
    torch.set_num_threads(1)
    torch.manual_seed(rep)
    from aepsych.acquisition import EAVC, MCLevelSetEstimation, MCPosteriorVariance
    from aepsych.acquisition.objective import ProbitObjective
    from aepsych.generators import OptimizeAcqfGenerator, SobolGenerator
    from aepsych.models import GPClassificationModel
    from aepsych.strategy import SequentialStrategy, Strategy
    # AEPsych configures the ROOT logger at INFO on import and logs every fit
    # and gen; 4500 trials of that bury the table.
    logging.getLogger().setLevel(logging.WARNING)

    field = Field(args.pheno, args.beta)
    u = stream(args.seed, rep, args.trials)
    init, _ = sobol_init(rep)
    lb = torch.zeros(2, dtype=torch.float64)
    ub = torch.ones(2, dtype=torch.float64)
    if method == "lse":
        acqf, kw = MCLevelSetEstimation, {"target": TARGET_P, "beta": 3.84,
                                          "objective": ProbitObjective()}
    elif method == "eavc":
        acqf, kw = EAVC, {"lb": lb, "ub": ub, "target": TARGET_P}
    else:
        acqf, kw = MCPosteriorVariance, {"objective": ProbitObjective()}
    init_strat = Strategy(generator=SobolGenerator(lb=lb, ub=ub, seed=rep),
                          lb=lb, ub=ub, outcome_types=["binary"],
                          min_asks=N_INIT, min_total_outcome_occurrences=0,
                          name="init")
    opt_strat = Strategy(generator=OptimizeAcqfGenerator(lb=lb, ub=ub, acqf=acqf,
                                                         acqf_kwargs=kw),
                         lb=lb, ub=ub, outcome_types=["binary"],
                         model=make_aepsych_model(args),
                         min_asks=args.trials - N_INIT,
                         min_total_outcome_occurrences=0, name="opt")
    strat = SequentialStrategy([init_strat, opt_strat])
    grid = torch.tensor((field.xs - LO) / (HI - LO), dtype=torch.float64)
    rows, spent = [], 0.0
    init_match = True
    for t in range(args.trials):
        t0 = time.perf_counter()
        xu = strat.gen(1)
        xn = xu.detach().numpy().reshape(-1).astype(float)
        if t < N_INIT and not np.allclose(xn, init[t], atol=1e-12):
            init_match = False
        x = to_box(xn)
        y = respond(float(field.p(x[0], x[1])), u[t])
        strat.add_data(xu.reshape(1, -1), torch.tensor([float(y)], dtype=torch.float64))
        spent += time.perf_counter() - t0
        n = t + 1
        if (n % args.every == 0 or n in MARKS) and strat.model is not None and n > N_INIT:
            # The model is refit lazily at the next gen(); score what an
            # experiment would read now by running that fit here. It replaces
            # the fit gen() would have run, so it is charged to the trial.
            t0 = time.perf_counter()
            strat._strat.fit()
            strat._strat._model_is_fresh = True
            spent += time.perf_counter() - t0
            mp, _ = strat.model.predict(grid, probability_space=True)
            mp = mp.detach().numpy()
            rows.append((n,) + field.score(mp) + (1000.0 * spent / n, field.band_mae(mp)))
    return rows, {"init_match": init_match}


# --- same-data mode -------------------------------------------------------------
#
# The adaptive comparison lets each library choose its own trials after the
# shared init, so a difference in the table mixes the model with the
# acquisition. This mode takes the acquisition out: one fixed (x, y) sequence
# per replication, and both models fitted from scratch to exactly its first n
# trials at each mark.

SAME_SOURCES = ("sobol", "ysp-lse")


def same_sequence(source, rep, args):
    """The replication's fixed trials in box units, and their responses."""
    field = Field(args.pheno, args.beta)
    u = stream(args.seed, rep, args.trials)
    if source == "sobol":
        try:
            import torch
            from aepsych.generators import SobolGenerator
            gen = SobolGenerator(lb=torch.zeros(2, dtype=torch.float64),
                                 ub=torch.ones(2, dtype=torch.float64), seed=rep)
            unit = gen.gen(args.trials).numpy().astype(float)
        except ImportError:
            from scipy.stats import qmc
            unit = qmc.Sobol(2, scramble=True, seed=rep).random(args.trials)
        xs = to_box(unit)
        ys = [respond(float(field.p(x[0], x[1])), u[t]) for t, x in enumerate(xs)]
        return np.asarray(xs), np.asarray(ys, dtype=float)
    # ysp-lse: the trials ysp.aep LSE with refine_steps = 2 chose in the
    # adaptive protocol, replayed as they were answered.
    import ysp.aep as yaep
    init, _ = sobol_init(rep)
    g = yaep.GP(lo=list(LO), hi=list(HI), intensity_dim=1, acq="lse",
              target_p=TARGET_P, grid=list(args.grid), n_init=N_INIT, fit=True,
              fit_every=args.fit_every, stop_trials=args.trials,
              max_trials=args.trials, refine_steps=2)
    for t in range(args.trials):
        x = list(to_box(init[t])) if t < N_INIT else g.next()[1]
        y = respond(float(field.p(x[0], x[1])), u[t])
        try:
            g.update(x, y)
        except yaep.Numeric:
            pass
    h = g.history()
    return np.array([r["x"] for r in h]), np.array([r["y"] for r in h])


def fit_ysp(xs, ys, args):
    """ysp.aep fitted once from its defaults to these trials, as AEPsych is."""
    import ysp.aep as yaep
    n = len(ys)
    g = yaep.GP(lo=list(LO), hi=list(HI), intensity_dim=1, target_p=TARGET_P,
              grid=list(args.grid), n_init=N_INIT, fit=True, fit_every=0,
              stop_trials=n, max_trials=n)
    for x, y in zip(xs, ys):
        try:
            g.update(list(x), int(y))
        except yaep.Numeric:
            pass
    g.fit()
    field = Field(args.pheno, args.beta)
    p = np.frombuffer(g.predict_p_many(field.xs)).copy()
    h = g.hyper()
    return p, {"ls_freq": h["lengthscale"][0], "ls_int": h["lengthscale"][1],
               "outputscale": h["outputscale"], "mean": h["mean"]}


def describe_aepsych(m, ck, base, n):
    amp = "fitted" if hasattr(ck, "outputscale") else "fixed at 1"
    inner = "" if base is ck else "(" + type(base).__name__ + ")"
    return (type(m).__name__ + ": " + type(m.mean_module).__name__ + ", "
            + type(ck).__name__ + inner + " (output scale " + amp + "), "
            + type(m.likelihood).__name__ + ", "
            + type(m.variational_strategy).__name__ + ", inducing_size "
            + str(getattr(m, "inducing_size", "?")) + " (greedy variance "
            "reduction picks at most that many; the count used is in the table)")


def make_aepsych_model(args):
    """GPClassificationModel at its defaults, or with a fitted output scale.

    AEPsych 0.8.0's default_mean_covar_factory takes fixed_kernel_amplitude
    only from a Config, so --aepsych-scale builds that Config: the kernel is
    then ScaleKernel(RBF) with the factory's own SmoothedBoxPrior(1, 4) on the
    output scale, and everything else is unchanged."""
    from aepsych.models import GPClassificationModel
    if not getattr(args, "aepsych_scale", False):
        return GPClassificationModel(dim=2)
    from aepsych.config import Config
    from aepsych.factory.default import default_mean_covar_factory
    cfg = Config(config_dict={"default_mean_covar_factory": {
        "lb": "[0, 0]", "ub": "[1, 1]", "fixed_kernel_amplitude": "False"}})
    mean, covar = default_mean_covar_factory(cfg)
    return GPClassificationModel(dim=2, mean_module=mean, covar_module=covar)


def fit_aepsych(xs, ys, rep, args):
    import logging
    import warnings
    import torch
    warnings.filterwarnings("ignore")
    torch.set_num_threads(1)
    torch.manual_seed(rep)
    from aepsych.models import GPClassificationModel
    logging.getLogger().setLevel(logging.WARNING)
    field = Field(args.pheno, args.beta)
    m = make_aepsych_model(args)
    xu = torch.tensor((xs - LO) / (HI - LO), dtype=torch.float64)
    m.fit(xu, torch.tensor(ys, dtype=torch.float64))
    grid = torch.tensor((field.xs - LO) / (HI - LO), dtype=torch.float64)
    p, _ = m.predict(grid, probability_space=True)
    ck = m.covar_module
    base = getattr(ck, "base_kernel", ck)
    ls = base.lengthscale.detach().numpy().reshape(-1) * (HI - LO)  # box units
    info = {"ls_freq": float(ls[0]), "ls_int": float(ls[1]),
            "outputscale": float(ck.outputscale) if hasattr(ck, "outputscale") else 1.0,
            "mean": float(m.mean_module.constant) if hasattr(m.mean_module, "constant") else 0.0,
            "inducing": int(m.variational_strategy.inducing_points.shape[0]),
            "defaults": describe_aepsych(m, ck, base, len(ys))}
    return p.detach().numpy().astype(float), info


def compare_fields(field, pa, pb):
    """Per-model scores against the truth, and the two models against each other."""
    band = (field.truth_p >= 0.05) & (field.truth_p <= 0.95)
    out = {}
    curves = {}
    for tag, p in (("ysp", pa), ("aep", pb)):
        p = np.asarray(p).reshape(NGRID, NGRID)
        err = np.abs(p - field.truth_p)
        out[tag + "_p"] = float(err.mean())
        out[tag + "_p_band"] = float(err[band].mean())
        out[tag + "_p_out"] = float(err[~band].mean())
        out[tag + "_thr"] = field.score(p)[1]
        curves[tag] = [column_threshold(p[i], field.xi) for i in range(NGRID)]
    out["p_diff"] = float(np.mean(np.abs(np.asarray(pa) - np.asarray(pb))))
    d = [abs(a - b) for a, b in zip(curves["ysp"], curves["aep"])
         if a is not None and b is not None]
    out["thr_diff"] = float(np.mean(d)) if d else float("nan")
    return out


def same_job(source, rep, args):
    t0 = time.perf_counter()
    try:
        field = Field(args.pheno, args.beta)
        xs, ys = same_sequence(source, rep, args)
        rows = []
        for n in MARKS:
            if n > len(ys):
                continue
            pa, ha = fit_ysp(xs[:n], ys[:n], args)
            pb, hb = fit_aepsych(xs[:n], ys[:n], rep, args)
            rows.append((n, compare_fields(field, pa, pb), ha, hb))
        return source, rep, rows, None, time.perf_counter() - t0
    except Exception:
        import traceback
        return source, rep, [], traceback.format_exc(), time.perf_counter() - t0


def same_data_main(args):
    sources = [s for s in args.same_data.split(",") if s]
    for src in sources:
        if src not in SAME_SOURCES:
            sys.exit("unknown --same-data source %s; choose from %s" % (src, SAME_SOURCES))
    print("same-data mode: sources %s, %d replications, fits at %s; ysp.aep fitted "
          "once from its defaults (fit(), grid %dx%d), AEPsych "
          "GPClassificationModel(dim=2) on the unit square, output scale %s"
          % (sources, args.reps, [m for m in MARKS if m <= args.trials],
             args.grid[0], args.grid[1],
             "FITTED (--aepsych-scale)" if args.aepsych_scale else "fixed at 1 (default)"))
    sys.stdout.flush()
    results = {}
    t_start = time.perf_counter()
    with ProcessPoolExecutor(max_workers=args.workers) as ex:
        futs = [ex.submit(same_job, src, r, args) for src in sources for r in range(args.reps)]
        for f in as_completed(futs):
            src, r, rows, err, secs = f.result()
            results[(src, r)] = rows
            status = ("FAILED: " + err) if err else ("%6.1f s" % secs)
            print("  %-8s rep %2d  %s" % (src, r, status), flush=True)
    wall = time.perf_counter() - t_start

    def cell(vals, fmt="%.4f+-%.4f"):
        m, c, _ = mean_ci(vals)
        return fmt % (m, c)

    for src in sources:
        print("\n== same data: %s, %s, beta = %g, %d replications; mean +- 1.96 sd / sqrt(n) =="
              % (src, args.pheno, args.beta, args.reps))
        by_n = {}
        for r in range(args.reps):
            for row in results.get((src, r), []):
                by_n.setdefault(row[0], []).append(row)
        print("   n  MAE(p) ysp      MAE(p) aep      |p ysp-aep|     "
              "band ysp        band aep        outside ysp     outside aep")
        for n in sorted(by_n):
            c = [row[1] for row in by_n[n]]
            print("%4d  " % n + "  ".join(cell([x[k] for x in c]) for k in
                  ("ysp_p", "aep_p", "p_diff", "ysp_p_band", "aep_p_band",
                   "ysp_p_out", "aep_p_out")))
        print("   n  thr ysp (dB)   thr aep (dB)   |thr ysp-aep| (dB)")
        for n in sorted(by_n):
            c = [row[1] for row in by_n[n]]
            print("%4d  " % n + "   ".join(cell([x[k] for x in c], "%5.2f+-%4.2f") for k in
                  ("ysp_thr", "aep_thr", "thr_diff")))
        print("   n  ls freq (log2 kHz) ysp/aep   ls intensity (dB) ysp/aep   "
              "outputscale ysp/aep   mean ysp/aep   aep inducing points")
        for n in sorted(by_n):
            rs = by_n[n]
            f = lambda i, k: float(np.mean([row[i][k] for row in rs]))
            print("%4d  %6.2f / %6.2f              %6.1f / %6.1f              "
                  "%5.2f / %5.2f          %5.2f / %5.2f    %5.1f"
                  % (n, f(2, "ls_freq"), f(3, "ls_freq"), f(2, "ls_int"), f(3, "ls_int"),
                     f(2, "outputscale"), f(3, "outputscale"), f(2, "mean"), f(3, "mean"),
                     f(3, "inducing")))
    defaults = sorted({row[3]["defaults"] for rows in results.values() for row in rows})
    print("\nAEPsych model at defaults, as fitted:")
    for d in defaults:
        print("  " + d)
    fails = [k for k, v in results.items() if not v]
    print("total wall time %.0f s; failed jobs %s" % (wall, fails if fails else "none"))
    return 1 if fails else 0


def job(lib, method, rep, args):
    t0 = time.perf_counter()
    try:
        rows, extra = (run_ysp if lib == "ysp" else run_aepsych)(method, rep, args)
        err = None
    except Exception as e:  # report and keep the other jobs
        import traceback
        rows, extra, err = [], {}, "".join(traceback.format_exception_only(type(e), e)).strip()
    return lib, method, rep, rows, extra, err, time.perf_counter() - t0


# --- reporting ------------------------------------------------------------------

def mean_ci(v):
    v = np.array([x for x in v if x == x], dtype=float)
    if len(v) == 0:
        return float("nan"), float("nan"), 0
    ci = 1.96 * v.std(ddof=1) / math.sqrt(len(v)) if len(v) > 1 else 0.0
    return float(v.mean()), ci, len(v)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--reps", type=int, default=10)
    ap.add_argument("--methods", default=",".join(METHODS))
    ap.add_argument("--libs", default="ysp,aepsych")
    ap.add_argument("--trials", type=int, default=150)
    ap.add_argument("--every", type=int, default=5, help="score every this many trials")
    ap.add_argument("--fit-every", type=int, default=20, dest="fit_every")
    ap.add_argument("--refine", type=int, default=0,
                    help="ysp.aep refine_steps: golden-section rounds off the grid (0 = off)")
    ap.add_argument("--model", default="gp", choices=("gp", "psychometric"),
                    help="ysp.aep desc.model: one GP over the box, or the threshold "
                         "and log-slope GPs over frequency (ysp/aep.h >= 0.3.0)")
    ap.add_argument("--grid", default="11x21", type=parse_grid,
                    help="ysp.aep candidate grid, frequency x intensity (default 11x21, M = 231)")
    ap.add_argument("--pheno", default="metabolic", choices=sorted(PHENO))
    ap.add_argument("--beta", type=float, default=2.0)
    ap.add_argument("--seed", type=int, default=20210419)
    ap.add_argument("--workers", type=int, default=max(1, (os.cpu_count() or 2) // 2))
    ap.add_argument("--csv", default=None, help="per-trial curves")
    ap.add_argument("--aepsych-scale", action="store_true", dest="aepsych_scale",
                    help="AEPsych model with a ScaleKernel and a fitted output scale "
                         "(fixed_kernel_amplitude = False) instead of its default "
                         "fixed amplitude of 1")
    ap.add_argument("--same-data", default=None, dest="same_data",
                    help="fit both models to one fixed trial sequence instead of "
                         "running the adaptive protocol: a comma list of "
                         + ", ".join(SAME_SOURCES))
    args = ap.parse_args()
    if args.csv:
        args.csv = os.path.abspath(args.csv)
    # Importing aepsych creates ./logs/aepsych_server.log in the working
    # directory. Keep that out of the repository; worker processes inherit the
    # directory at spawn.
    import tempfile
    # Spawned workers re-import this script by the path it was started with,
    # so a relative path has to be made absolute before the directory moves.
    sys.modules["__main__"].__file__ = os.path.abspath(__file__)
    os.chdir(tempfile.mkdtemp(prefix="compare_gp_aepsych_"))

    field = Field(args.pheno, args.beta)
    spline_selfcheck(field)
    methods = [m for m in args.methods.split(",") if m]
    libs = [l for l in args.libs.split(",") if l]
    for m in methods:
        if m not in METHODS:
            ap.error(f"unknown method {m}")
    if "aepsych" in libs:
        try:
            import aepsych  # noqa: F401
        except Exception as e:
            print(f"AEPsych did not import ({e!r}); running ysp.aep only", file=sys.stderr)
            libs = [l for l in libs if l != "aepsych"]
    if args.same_data:
        return same_data_main(args)
    _, init_src = sobol_init(0)

    print("compare_gp_aepsych: Owen et al. 2021 audiometric benchmark")
    print(f"observer: {args.pheno}, beta = {args.beta} dB; {args.trials} trials "
          f"({N_INIT} Sobol from {init_src}, shared); target p = {TARGET_P}")
    print(f"ysp.aep candidates: {args.grid[0]} x {args.grid[1]} grid "
          f"(M = {args.grid[0] * args.grid[1]}), refine_steps {args.refine}, "
          f"model {args.model}; "
          f"hyperparameters fitted every "
          f"{args.fit_every} trials")
    print(f"{args.reps} replications x {len(methods)} methods x {libs}; "
          f"{args.workers} worker processes, one thread each; seed {args.seed}")
    try:
        import importlib.metadata as md
        vers = {k: md.version(k) for k in ("ysp-aep", "aepsych", "torch", "botorch", "gpytorch")
                if _has_dist(md, k)}
        print("versions:", ", ".join(f"{k} {v}" for k, v in vers.items()))
    except Exception:
        pass
    sys.stdout.flush()

    jobs = [(lib, m, r) for r in range(args.reps) for m in methods for lib in libs]
    results = {}
    t_start = time.perf_counter()
    with ProcessPoolExecutor(max_workers=args.workers) as ex:
        futs = [ex.submit(job, lib, m, r, args) for lib, m, r in jobs]
        for f in as_completed(futs):
            lib, m, r, rows, extra, err, secs = f.result()
            results[(lib, m, r)] = (rows, extra, err)
            status = "FAILED: " + err if err else f"{secs:6.1f} s"
            print(f"  {lib:8s} {m:5s} rep {r:2d}  {status}", flush=True)
    wall = time.perf_counter() - t_start

    if args.csv:
        with open(args.csv, "w") as fh:
            fh.write("lib,method,rep,trial,mae_p,mae_thr_db,nocross_cols,ms_per_trial,band_mae_p\n")
            for (lib, m, r), (rows, _, _) in sorted(results.items()):
                for row in rows:
                    fh.write(f"{lib},{m},{r},{row[0]},{row[1]:.6f},{row[2]:.4f},{row[3]},{row[4]:.3f},{row[5]:.6f}\n")

    print(f"\n== {args.pheno}, beta = {args.beta}, {args.reps} replications; "
          f"mean +- 1.96 sd / sqrt(n) over replications ==")
    print("method lib      trial   MAE(p)              band MAE(p)         MAE(thr, dB)       nocross  ms/trial")
    for m in methods:
        for lib in libs:
            first = True
            for mark in MARKS:
                if mark > args.trials:
                    continue
                vals = [row for (l, mm, r), (rows, _, _) in results.items()
                        if l == lib and mm == m for row in rows if row[0] == mark]
                if not vals:
                    continue
                p, pc, n = mean_ci([v[1] for v in vals])
                t, tc, nt = mean_ci([v[2] for v in vals])
                miss = np.mean([v[3] for v in vals])
                ms = np.mean([v[4] for v in vals])
                b, bc, _ = mean_ci([v[5] for v in vals])
                print(f"{m if first else '':6s} {lib if first else '':8s} {mark:5d}   "
                      f"{p:.4f} +- {pc:.4f}   {b:.4f} +- {bc:.4f}   {t:6.2f} +- {tc:5.2f}"
                      f"{'' if nt == n else f' (n={nt})':8s} {miss:5.2f}   {ms:8.2f}")
                first = False
        print()
    fails = [(k, e) for k, (_, _, e) in results.items() if e]
    for k, e in fails:
        print(f"failed: {k}: {e}")
    if "ysp" in libs:
        nums = sum(results[k][1].get("numeric", 0) for k in results if k[0] == "ysp")
        lms = [results[k][1]["log_marginal"] for k in results if k[0] == "ysp" and results[k][1]]
        print(f"ysp.aep: YAEP_ERR_NUMERIC {nums} times; mean final log marginal "
              f"{np.mean(lms):.1f}")
        # The fitted hyperparameters at the end of each run, in the box's own
        # units (dB for intensity, log2 kHz for frequency), so they can be put
        # beside another model's without guessing the scaling.
        for m in methods:
            hs = [results[k][1]["hyper"] for k in sorted(results)
                  if k[0] == "ysp" and k[1] == m and results[k][1]]
            if not hs:
                continue
            ls = np.array([h["lengthscale"][:2] for h in hs])
            os_ = np.array([h["outputscale"] for h in hs])
            mn = np.array([h["mean"] for h in hs])
            print(f"ysp.aep {m}: final lengthscale freq {ls[:, 0].mean():.3f} "
                  f"[{ls[:, 0].min():.3f}, {ls[:, 0].max():.3f}] log2 kHz, "
                  f"intensity {ls[:, 1].mean():.2f} [{ls[:, 1].min():.2f}, "
                  f"{ls[:, 1].max():.2f}] dB, outputscale {os_.mean():.3f} "
                  f"[{os_.min():.3f}, {os_.max():.3f}], mean {mn.mean():.3f}")
            if args.model == "psychometric":
                # Under this model the fields above are the threshold GP's
                # (intensity lengthscale unused); the log-slope GP is the _g set.
                lg = np.array([h["lengthscale_g"][0] for h in hs])
                og = np.array([h["outputscale_g"] for h in hs])
                mg = np.array([h["mean_g"] for h in hs])
                print(f"ysp.aep {m}: log-slope GP lengthscale_g freq {lg.mean():.3f} "
                      f"[{lg.min():.3f}, {lg.max():.3f}] log2 kHz, outputscale_g "
                      f"{og.mean():.3f} [{og.min():.3f}, {og.max():.3f}], mean_g "
                      f"{mg.mean():.3f} [{mg.min():.3f}, {mg.max():.3f}] "
                      f"(slope exp(mean_g) = {np.exp(mg.mean()):.3f} per dB)")
    if "aepsych" in libs:
        bad = [k for k in results if k[0] == "aepsych" and results[k][1]
               and not results[k][1].get("init_match", True)]
        print(f"aepsych: init points matched the shared Sobol set in "
              f"{sum(1 for k in results if k[0] == 'aepsych') - len(bad)} of "
              f"{sum(1 for k in results if k[0] == 'aepsych')} runs")
    print(f"total wall time {wall:.0f} s")
    return 1 if fails else 0


def parse_grid(text):
    try:
        c, i = (int(v) for v in text.lower().split("x"))
    except ValueError:
        raise argparse.ArgumentTypeError(f"grid must look like 11x21, not {text!r}")
    if c < 2 or i < 2:
        raise argparse.ArgumentTypeError("each grid count must be at least 2")
    return (c, i)


def _has_dist(md, name):
    try:
        md.version(name)
        return True
    except md.PackageNotFoundError:
        return False


if __name__ == "__main__":
    sys.exit(main())
