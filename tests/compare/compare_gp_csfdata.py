#!/usr/bin/env python3
"""ysp.aep and AEPsych fitted to real human data: cross-validated model quality.

No acquisition is involved. The data are the contrast-sensitivity dataset of
Letham et al. 2022, "Look-ahead acquisition functions for Bernoulli level set
estimation" (AISTATS; github.com/facebookresearch/bernoulli_lse, code MIT,
data license not stated): 1001 detection trials over six stimulus
dimensions. The file is downloaded at run time into a cache directory and is
not part of this repository.

Every model is fitted to the same training trials and scored on the same
held-out trials of a 5-fold split (fixed seed):

- ysp.aep, GP model: RBF ARD, probit, weak priors on (the default). One
  yaep_fit() is capped at 40 objective evaluations, so the script repeats
  whole fits until one moves the log marginal likelihood by < 1e-3 nats (at
  most 30), and reports the rounds and the fraction of folds that converged.
- ysp.aep, GP model, no_hyper_prior (type-II maximum likelihood).
- ysp.aep, psychometric model: intensity_dim = 0 (contrast), a threshold GP and
  a log-slope GP over the other five dimensions, fitted the same way.
- AEPsych GPClassificationModel at its defaults (fixed output scale 1,
  GreedyVarianceReduction inducing points, 100 of them), on the inputs
  scaled to the unit cube by the bounds, which is what AEPsych's config path
  does with its normalizing parameter transform.
- The same with k-means++ inducing points (KMeansAllocator).
- The same with a fitted output scale (fixed_kernel_amplitude = False).
- Logistic regression on the raw inputs (standardized for the solve), fitted
  by Newton's method with a tiny ridge, so the GP gains have a floor.

ysp.aep is compiled with YAEP_MAX_TRIALS = 512, so a training fold of about
800 trials does not fit in one handle. Every model therefore trains on the
same random 500-trial subset of its training fold (one subset per fold, fixed
seed), and is tested on the whole held-out fold of about 200.

Metrics on the held-out fold: mean log loss (p clipped to [1e-6, 1 - 1e-6]),
Brier score, and accuracy with the prediction thresholded at 0.5. The
interval is the t interval over the 5 folds (t = 2.776 at 4 degrees of
freedom). Fit time is wall time for the fit alone, one thread, on this
machine.

Usage (from the repository root, in a venv with ysp-aep, numpy and aepsych):

    python tests/compare/compare_gp_csfdata.py
    python tests/compare/compare_gp_csfdata.py --models ysp-aep,logreg --workers 1

Exit code: 0 when every fit succeeded, 1 when any raised, 2 on a bad argument.
"""
import argparse
import math
import os
import sys
import time
import urllib.request
from concurrent.futures import ProcessPoolExecutor, as_completed

import numpy as np

DATA_URL = ("https://raw.githubusercontent.com/facebookresearch/bernoulli_lse/"
            "refs/heads/main/data/csf_dataset.csv")
COLUMNS = ("contrast", "pedestal", "temporal_frequency", "spatial_frequency",
           "size", "eccentricity")
LO = np.array([-1.5, -1.5, 0.0, 0.5, 1.0, 0.0])
HI = np.array([0.0, 0.0, 20.0, 7.0, 10.0, 10.0])
MODELS = ("ysp-aep", "ysp-aep-noprior", "ysp-psychometric", "aepsych",
          "aepsych-kmeans", "aepsych-scale", "logreg")
T_975_4 = 2.7764451051977987
N_TRAIN = 500
MAX_TRIALS = 512
MAX_FIT_ROUNDS = 30


def cache_dir():
    base = (os.environ.get("LOCALAPPDATA") or os.environ.get("XDG_CACHE_HOME")
            or os.path.join(os.path.expanduser("~"), ".cache"))
    return os.path.join(base, "ysp", "compare")


def load_data(path=None):
    """(X n x 6 in the bounds' units, y in {0, 1}); downloads once."""
    if path is None:
        path = os.path.join(cache_dir(), "csf_dataset.csv")
        if not os.path.exists(path):
            os.makedirs(os.path.dirname(path), exist_ok=True)
            print(f"downloading {DATA_URL}\n  -> {path}", flush=True)
            tmp = path + ".part"
            urllib.request.urlretrieve(DATA_URL, tmp)
            os.replace(tmp, path)
    with open(path, encoding="utf-8") as fh:
        header = fh.readline().strip().split(",")
        rows = [list(map(float, line.split(","))) for line in fh if line.strip()]
    data = np.array(rows)
    col = {name: i for i, name in enumerate(header)}
    X = data[:, [col[c] for c in COLUMNS]]
    y = data[:, col["response"]].astype(int)
    return X, y, path


def folds(n, k, seed):
    perm = np.random.default_rng(seed).permutation(n)
    return [np.sort(perm[i::k]) for i in range(k)]


# --- the models -----------------------------------------------------------------
# Each returns (p_test, lengthscales in raw units or None, notes dict).

def fit_ysp(Xtr, ytr, Xte, variant):
    import ysp.aep as yaep
    desc = dict(lo=list(LO), hi=list(HI), intensity_dim=0, target_p=0.75,
                acq="lse", n_candidates=64, max_trials=MAX_TRIALS,
                stop_trials=MAX_TRIALS, fit=True, fit_every=0,
                # rank-one updates while the trials go in; the exact Newton
                # refit happens once below, before the fit
                refit_every=MAX_TRIALS + 1)
    if variant == "noprior":
        desc["no_hyper_prior"] = True
    if variant == "psychometric":
        desc["model"] = "psychometric"
        del desc["refit_every"]   # the psychometric model always refits
    g = yaep.GP(**desc)
    t0 = time.perf_counter()
    numeric = 0
    for x, y in zip(Xtr, ytr):
        try:
            g.update(list(x), int(y))
        except yaep.Numeric:
            numeric += 1
    g.refit()
    # One fit is at most 40 objective evaluations, and on 500 trials in six
    # dimensions the first one runs out of budget rather than converging. A
    # new fit starts from where the last one stopped, so fit again until a
    # whole fit moves the log marginal likelihood by less than 1e-3 nats.
    steps, rounds, converged = 0, 0, False
    prev = g.log_marginal
    while rounds < MAX_FIT_ROUNDS:
        rounds += 1
        steps += 1
        while g.fit_step():
            steps += 1
        if rounds > 1 and abs(g.log_marginal - prev) < 1e-3:
            converged = True
            break
        prev = g.log_marginal
    secs = time.perf_counter() - t0
    p = np.frombuffer(g.predict_p_many(np.ascontiguousarray(Xte))).copy()
    h = g.hyper()
    if variant == "psychometric":
        ls = {"threshold": h["lengthscale"], "log_slope": h["lengthscale_g"],
              "outputscale": h["outputscale"], "outputscale_g": h["outputscale_g"],
              "mean_g": h["mean_g"]}
    else:
        ls = {"lengthscale": h["lengthscale"], "outputscale": h["outputscale"],
              "mean": h["mean"]}
    return p, ls, {"fit_s": secs, "fit_steps": steps, "fit_rounds": rounds,
                   "converged": float(converged), "numeric": numeric,
                   "log_marginal": g.log_marginal}


def fit_aepsych(Xtr, ytr, Xte, variant, seed):
    import logging
    import warnings
    import torch
    warnings.filterwarnings("ignore")
    torch.set_num_threads(1)
    torch.manual_seed(seed)
    from aepsych.models import GPClassificationModel
    logging.getLogger().setLevel(logging.WARNING)
    kw = {}
    if variant == "kmeans":
        from aepsych.models.inducing_points import KMeansAllocator
        kw["inducing_point_method"] = KMeansAllocator(dim=6)
    if variant == "scale":
        from aepsych.config import Config
        from aepsych.factory.default import default_mean_covar_factory
        cfg = Config(config_dict={"default_mean_covar_factory": {
            "lb": str([0.0] * 6), "ub": str([1.0] * 6),
            "fixed_kernel_amplitude": "False"}})
        kw["mean_module"], kw["covar_module"] = default_mean_covar_factory(cfg)
    m = GPClassificationModel(dim=6, inducing_size=100, **kw)
    xu = torch.tensor((Xtr - LO) / (HI - LO), dtype=torch.float64)
    t0 = time.perf_counter()
    m.fit(xu, torch.tensor(ytr, dtype=torch.float64))
    secs = time.perf_counter() - t0
    p, _ = m.predict(torch.tensor((Xte - LO) / (HI - LO), dtype=torch.float64),
                     probability_space=True)
    ck = m.covar_module
    base = getattr(ck, "base_kernel", ck)
    ls = (base.lengthscale.detach().numpy().reshape(-1) * (HI - LO)).tolist()
    return p.detach().numpy().astype(float).reshape(-1), {
        "lengthscale": ls,
        "outputscale": float(ck.outputscale) if hasattr(ck, "outputscale") else 1.0,
        "mean": float(m.mean_module.constant)}, {
        "fit_s": secs,
        "inducing": int(m.variational_strategy.inducing_points.shape[0]),
        "allocator": type(m.inducing_point_method).__name__}


def fit_logreg(Xtr, ytr, Xte, ridge=1e-6):
    t0 = time.perf_counter()
    mu, sd = Xtr.mean(axis=0), Xtr.std(axis=0)
    sd[sd == 0.0] = 1.0
    A = np.column_stack([np.ones(len(Xtr)), (Xtr - mu) / sd])
    w = np.zeros(A.shape[1])
    for it in range(100):
        p = 1.0 / (1.0 + np.exp(-A @ w))
        grad = A.T @ (ytr - p) - ridge * w
        H = (A * (p * (1.0 - p))[:, None]).T @ A + ridge * np.eye(len(w))
        step = np.linalg.solve(H, grad)
        w += step
        if np.max(np.abs(step)) < 1e-10:
            break
    secs = time.perf_counter() - t0
    B = np.column_stack([np.ones(len(Xte)), (Xte - mu) / sd])
    p = 1.0 / (1.0 + np.exp(-B @ w))
    coef = (w[1:] / sd).tolist()     # per raw unit, for the record
    return p, {"coef_per_unit": coef, "intercept": float(w[0] - np.sum(w[1:] * mu / sd))}, {
        "fit_s": secs, "iterations": it + 1}


def scores(p, y):
    p = np.clip(np.asarray(p, dtype=float), 1e-6, 1.0 - 1e-6)
    return {"logloss": float(-np.mean(y * np.log(p) + (1 - y) * np.log(1.0 - p))),
            "brier": float(np.mean((p - y) ** 2)),
            "accuracy": float(np.mean((p >= 0.5) == (y == 1)))}


def job(model, fold, args):
    import traceback
    try:
        X, y, _ = load_data(args.data)
        parts = folds(len(y), args.folds, args.seed)
        test = parts[fold]
        train = np.setdiff1d(np.arange(len(y)), test)
        rng = np.random.default_rng([args.seed, fold])
        sub = np.sort(rng.choice(train, size=min(args.n_train, len(train)), replace=False))
        Xtr, ytr, Xte, yte = X[sub], y[sub], X[test], y[test]
        if model == "ysp-aep":
            p, hyp, notes = fit_ysp(Xtr, ytr, Xte, "prior")
        elif model == "ysp-aep-noprior":
            p, hyp, notes = fit_ysp(Xtr, ytr, Xte, "noprior")
        elif model == "ysp-psychometric":
            p, hyp, notes = fit_ysp(Xtr, ytr, Xte, "psychometric")
        elif model == "aepsych":
            p, hyp, notes = fit_aepsych(Xtr, ytr, Xte, "default", args.seed + fold)
        elif model == "aepsych-kmeans":
            p, hyp, notes = fit_aepsych(Xtr, ytr, Xte, "kmeans", args.seed + fold)
        elif model == "aepsych-scale":
            p, hyp, notes = fit_aepsych(Xtr, ytr, Xte, "scale", args.seed + fold)
        else:
            p, hyp, notes = fit_logreg(Xtr, ytr, Xte)
        if not np.all(np.isfinite(p)):
            raise RuntimeError("non-finite prediction")
        return model, fold, scores(p, yte), hyp, notes, None
    except Exception:
        return model, fold, None, None, None, traceback.format_exc()


def interval(v):
    v = np.asarray(v, dtype=float)
    if len(v) < 2:
        return float(v.mean()), 0.0
    # the t quantile for the default 5 folds, the normal one otherwise
    q = T_975_4 if len(v) == 5 else 1.96
    return float(v.mean()), q * float(v.std(ddof=1)) / math.sqrt(len(v))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--models", default=",".join(MODELS))
    ap.add_argument("--folds", type=int, default=5)
    ap.add_argument("--seed", type=int, default=20220328)
    ap.add_argument("--n-train", type=int, default=N_TRAIN, dest="n_train")
    ap.add_argument("--workers", type=int, default=5)
    ap.add_argument("--data", default=None, help="a local copy of csf_dataset.csv")
    args = ap.parse_args()
    models = [m for m in args.models.split(",") if m]
    for m in models:
        if m not in MODELS:
            ap.error(f"unknown model {m}; choose from {MODELS}")
    if args.n_train > MAX_TRIALS:
        ap.error(f"--n-train must be at most {MAX_TRIALS} (ysp.aep's YAEP_MAX_TRIALS)")
    X, y, path = load_data(args.data)
    args.data = path
    # Importing aepsych writes ./logs/aepsych_server.log; keep it out of the
    # repository. Workers re-import this script by path, so make it absolute.
    import tempfile
    sys.modules["__main__"].__file__ = os.path.abspath(__file__)
    os.chdir(tempfile.mkdtemp(prefix="compare_gp_csfdata_"))

    print("compare_gp_csfdata: 5-fold CV on the Letham et al. 2022 CSF dataset")
    print(f"data: {path}, {len(y)} trials, {int(y.sum())} yes; source {DATA_URL}")
    print(f"folds: {args.folds}, seed {args.seed}; every model trains on the same "
          f"random {args.n_train}-trial subset of each training fold "
          f"(ysp.aep YAEP_MAX_TRIALS = {MAX_TRIALS}) and is tested on the whole "
          f"held-out fold")
    try:
        import importlib.metadata as md
        print("versions:", ", ".join(f"{k} {md.version(k)}" for k in
                                     ("ysp-aep", "aepsych", "torch", "gpytorch")))
    except Exception:
        pass
    sys.stdout.flush()

    results = {}
    t_all = time.perf_counter()
    with ProcessPoolExecutor(max_workers=args.workers) as ex:
        futs = [ex.submit(job, m, f, args) for m in models for f in range(args.folds)]
        for fu in as_completed(futs):
            m, f, sc, hyp, notes, err = fu.result()
            results[(m, f)] = (sc, hyp, notes, err)
            msg = ("FAILED\n" + err) if err else f"{notes['fit_s']:7.2f} s fit"
            print(f"  {m:17s} fold {f}  {msg}", flush=True)
    wall = time.perf_counter() - t_all

    print(f"\n== held-out scores, mean +- t interval over {args.folds} folds ==")
    print("model              log loss          Brier             accuracy          fit s")
    for m in models:
        ok = [results[(m, f)] for f in range(args.folds) if results[(m, f)][0]]
        if not ok:
            print(f"{m:17s}  failed")
            continue
        cells = []
        for k in ("logloss", "brier", "accuracy"):
            mu, ci = interval([r[0][k] for r in ok])
            cells.append(f"{mu:.4f} +- {ci:.4f}")
        fit_s = np.mean([r[2]["fit_s"] for r in ok])
        print(f"{m:17s}  " + "  ".join(cells) + f"   {fit_s:7.2f}")
    base = np.mean(y)
    print(f"{'constant p':17s}  {scores(np.full(len(y), base), y)['logloss']:.4f}"
          f"                              {max(base, 1 - base):.4f}"
          "             (the whole-data base rate, for scale)")

    print("\n== fitted hyperparameters, mean over folds; lengthscales in the "
          "inputs' own units ==")
    print("dims: " + ", ".join(COLUMNS))
    for m in models:
        ok = [results[(m, f)] for f in range(args.folds) if results[(m, f)][0]]
        if not ok:
            continue
        hs = [r[1] for r in ok]
        for key in ("lengthscale", "threshold", "log_slope", "coef_per_unit"):
            if key in hs[0]:
                v = np.mean([h[key][:6] for h in hs], axis=0)
                print(f"{m:17s} {key:13s} " + " ".join(f"{x:8.3g}" for x in v))
        extra = []
        for key in ("outputscale", "outputscale_g", "mean", "mean_g", "intercept"):
            if key in hs[0]:
                extra.append(f"{key} {np.mean([h[key] for h in hs]):.4g}")
        notes = [r[2] for r in ok]
        for key in ("fit_rounds", "fit_steps", "converged", "numeric", "log_marginal",
                    "inducing", "iterations"):
            if key in notes[0]:
                extra.append(f"{key} {np.mean([n[key] for n in notes]):.3g}")
        if "allocator" in notes[0]:
            extra.append(f"allocator {notes[0]['allocator']}")
        print(f"{'':17s} " + ", ".join(extra))
    print("   psychometric: lengthscale[0] (contrast, the intensity) is unused by "
          "both of its GPs")

    fails = [k for k, v in results.items() if v[3]]
    print(f"\ntotal wall time {wall:.0f} s; failed fits: {fails if fails else 'none'}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
