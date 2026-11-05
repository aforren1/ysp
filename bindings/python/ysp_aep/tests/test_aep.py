"""Tests for the ysp.aep binding. Run with pytest from anywhere once ysp-aep is
installed. NumPy is optional: the tests that need it skip without it.

The numerical tolerances are the ones tests/adapt/aep_test.c enforces on
the same observers, so a pass here says the binding did not lose anything on
the way through, not that the model got better.
"""
import array
import math
import os
import random
import re

import pytest

import ysp.aep as yaep

try:
    import numpy as np
except ImportError:  # the binding has no NumPy dependency
    np = None

SQRT2 = math.sqrt(2.0)


def phi(z):
    return 0.5 * math.erfc(-z / SQRT2)


# --- the 1-D probit observer of the C test ----------------------------------

SLOPE, X50 = 8.0, 0.40
THR75 = X50 + 0.67448975019608171 / SLOPE


def run_1d(acq, stream, trials=100):
    """The C test's run_observer(): same desc, same observer, a different
    uniform stream (Python's Mersenne Twister, not splitmix64)."""
    u = random.Random(1000 + stream)
    g = yaep.GP(lo=[0.0], hi=[1.0], acq=acq, target_p=0.75, grid=[65],
              n_init=8, fit=True, fit_every=10, stop_trials=trials)
    while not g.done:
        idx, x = g.next()
        assert idx >= -1
        p = phi(SLOPE * (x[0] - X50))
        g.update(x, yaep.simulate_outcome([1.0 - p, p], u.random()))
    assert g.n_trials == trials
    thr, lo, hi = g.threshold()
    assert lo <= thr + 1e-9 and thr <= hi + 1e-9
    return abs(thr - THR75)


@pytest.mark.parametrize("acq", [yaep.ACQ_LSE, yaep.ACQ_EAVC])
def test_1d_threshold_recovery(acq):
    errs = [run_1d(acq, s) for s in range(8)]
    mean = sum(errs) / len(errs)
    print(f"acq {acq}: mean abs threshold error {mean:.4f} over 8 streams")
    assert mean < 0.08


def test_string_enum_names_match_constants():
    a = yaep.GP(lo=[0], hi=[1], acq="EAVC", kernel="rbf", link="probit",
              target_p=0.75, stop_trials=5)
    b = yaep.GP(lo=[0], hi=[1], acq=yaep.ACQ_EAVC, target_p=0.75, stop_trials=5)
    assert a.next() == b.next()


# --- the other likelihoods ----------------------------------------------------

def test_ordinal_40_trials():
    u = random.Random(55)
    cut = [0.0, 1.0, 2.0]
    g = yaep.GP(lo=[0.0], hi=[1.0], lik=yaep.LIK_ORDINAL, n_outcomes=4,
              target_outcome=2, target_p=0.5, acq=yaep.ACQ_BALD, grid=[33],
              n_init=8, fit=True, fit_every=20, hyper={"cutpoint": cut},
              stop_trials=40)
    while not g.done:
        _, x = g.next()
        f = 4.0 * (x[0] - 0.5)
        c = [phi(cj - f) for cj in cut]
        p = [c[0], c[1] - c[0], c[2] - c[1], 1.0 - c[2]]
        g.update(x, yaep.simulate_outcome(p, u.random()))
    lo, hi = g.predict_p(0.05), g.predict_p(0.95)   # P(y >= 2)
    assert 0.0 <= lo < hi <= 1.0
    assert hi - lo > 0.3
    probs = g.predict_outcomes(0.5)
    assert len(probs) == 4 and abs(sum(probs) - 1.0) < 1e-9
    # cutpoint[0] = 0 reads as "fit", so the vector moves; it stays ordered.
    c = g.hyper()["cutpoint"]
    assert len(c) == 3 and c[0] < c[1] < c[2]


def test_categorical_40_trials():
    u = random.Random(66)
    g = yaep.GP(lo=[0.0], hi=[1.0], lik="categorical", n_outcomes=3,
              target_outcome=1, target_p=0.5, acq=yaep.ACQ_BALV, grid=[25],
              n_init=10, stop_trials=40)
    while not g.done:
        _, x = g.next()
        fs = [2.0 - 4.0 * x[0], 0.0, -2.0 + 4.0 * x[0]]
        e = [math.exp(v) for v in fs]
        g.update(x, yaep.simulate_outcome([v / sum(e) for v in e], u.random()))
    left, right = g.predict_outcomes(0.0), g.predict_outcomes(1.0)
    assert abs(sum(left) - 1.0) < 1e-9 and abs(sum(right) - 1.0) < 1e-9
    assert max(range(3), key=lambda k: left[k]) == 0
    assert max(range(3), key=lambda k: right[k]) == 2
    mu0, _ = g.predict_f(0.0, k=0)
    mu2, _ = g.predict_f(0.0, k=2)
    assert mu0 > mu2


def test_gaussian_40_trials():
    u = random.Random(44)
    g = yaep.GP(lo=[0.0], hi=[1.0], lik=yaep.LIK_GAUSSIAN, acq=yaep.ACQ_BALV,
              target_value=0.5, grid=[33], n_init=6, fit=True, fit_every=20,
              stop_trials=40)
    while not g.done:
        _, x = g.next()
        g.update_real(x, math.sin(4.0 * x[0]) + 0.05 * (u.random() - 0.5))
    xs = [i / 20 for i in range(21)]
    rms = math.sqrt(sum((g.predict_p(x) - math.sin(4 * x)) ** 2 for x in xs) / len(xs))
    assert rms < 0.05
    with pytest.raises(yaep.Error):
        g.update([0.5], 1)          # the discrete update under GAUSSIAN
    with pytest.raises(yaep.Error):
        g.predict_outcomes(0.5)     # no outcome probabilities under GAUSSIAN


# --- batch prediction ---------------------------------------------------------

def fitted_2d(trials=40, seed=3, **extra):
    u = random.Random(seed)
    desc = dict(lo=[0.0, 0.0], hi=[1.0, 1.0], intensity_dim=1, target_p=0.75,
                grid=[9, 17], n_init=6, fit=True, fit_every=10,
                stop_trials=trials)
    desc.update(extra)
    g = yaep.GP(**desc)
    while not g.done:
        _, x = g.next()
        p = phi(8.0 * (x[1] - (0.3 + 0.3 * x[0])))
        g.update(x, yaep.simulate_outcome([1 - p, p], u.random()))
    return g


def grid_points(n=12):
    return [[i / (n - 1), j / (n - 1)] for i in range(n) for j in range(n)]


def test_predict_p_many_equals_predict_p():
    g = fitted_2d()
    pts = grid_points()
    one = [g.predict_p(x) for x in pts]
    flat = array.array("d", [v for x in pts for v in x])
    for xs in (pts, tuple(tuple(x) for x in pts), flat, memoryview(flat)):
        many = g.predict_p_many(xs)
        assert isinstance(many, memoryview) and many.format == "d"
        assert many.tolist() == one            # to the last bit, as in C
    mu, sd = g.predict_f_many(pts)
    for i, x in enumerate(pts):
        assert (mu[i], sd[i]) == g.predict_f(x)
    assert g.predict_p_many([]).tolist() == []


@pytest.mark.skipif(np is None, reason="NumPy not installed")
def test_predict_many_numpy():
    g = fitted_2d()
    pts = np.array(grid_points())
    ref = [g.predict_p(list(x)) for x in pts]
    out = np.frombuffer(g.predict_p_many(pts))
    assert out.tolist() == ref
    # Fortran order and float32 take the copying paths and still agree.
    assert np.frombuffer(g.predict_p_many(np.asfortranarray(pts))).tolist() == ref
    f32 = np.frombuffer(g.predict_p_many(pts.astype(np.float32)))
    assert np.allclose(f32, ref, atol=1e-5)
    with pytest.raises(ValueError):
        g.predict_p_many(np.zeros((4, 3)))


# --- the async layer ----------------------------------------------------------

def responder(seed):
    u = random.Random(seed)

    def respond(x):
        p = phi(8.0 * (x[1] - (0.3 + 0.3 * x[0])))
        return yaep.simulate_outcome([1 - p, p], u.random())
    return respond


ASYNC_DESC = dict(lo=[0.0, 0.0], hi=[1.0, 1.0], intensity_dim=1,
                  target_p=0.75, acq="eavc", grid=[9, 17], n_init=6, fit=True,
                  fit_every=10, stop_trials=40)


def test_async_reproduces_sync():
    respond = responder(7)
    g1 = yaep.GP(**ASYNC_DESC)
    sync_x = []
    while not g1.done:
        _, x = g1.next()
        sync_x.append(x)
        g1.update(x, respond(x))

    respond = responder(7)
    g2 = yaep.GP(**ASYNC_DESC)
    async_x = []
    with yaep.Async(g2, context=[0.5]) as a:
        assert a.running
        assert a.policy in (yaep.POLICY_NORMAL, yaep.POLICY_BELOW_NORMAL)
        seq, snap = a.poll()
        assert seq == 0
        with pytest.raises(yaep.Busy):
            g2.predict_p([0.5, 0.5])   # the thread owns the handle
        while not snap.done:
            async_x.append(snap.x)
            s = a.submit(snap.x, respond(snap.x))
            got, snap = a.wait(s, timeout_s=30.0)
            assert got >= s and snap.update_rc == 0
        last = snap
    assert not a.running
    assert async_x == sync_x
    assert g2.history() == g1.history()
    assert g2.log_marginal == g1.log_marginal
    assert g2.hyper() == g1.hyper()
    thr = g2.threshold([0.5])
    assert thr == g1.threshold([0.5])
    assert (last.threshold, last.threshold_lo, last.threshold_hi) == thr
    assert last.n_trials == 40 and last.stop == yaep.STOP_TRIALS


def test_async_busy_timeout_and_rules():
    g = yaep.GP(lo=[0.0, 0.0], hi=[1.0, 1.0], intensity_dim=1, target_p=0.75,
              acq="eavc", grid=[33, 33], n_init=2, stop_trials=200)
    a = yaep.Async(g, context=0.5)
    a.start()
    try:
        with pytest.raises(yaep.Busy):
            a.start()
        busy = 0
        seqs = []
        for k in range(40):     # far faster than an EAVC update at M = 1089
            try:
                seqs.append(a.submit([0.5, (k % 10) / 10], k % 2))
            except yaep.Busy as e:
                assert e.code == yaep.ERR_BUSY
                busy += 1
        assert busy > 0
        assert a.pending <= yaep.ASYNC_QUEUE
        with pytest.raises(yaep.Timeout):
            a.wait(seqs[-1] + 100, timeout_s=0.01)
        with pytest.raises(yaep.Error):
            a.submit([2.0, 0.5], 1)     # outside the box
    finally:
        a.stop()
    # stop() drained every accepted response, and the GP is ours again.
    assert g.n_trials == len(seqs)
    g.predict_p([0.5, 0.5])


def test_async_refuses_python_rng():
    g = yaep.GP(lo=[0], hi=[1], target_p=0.75, stop_trials=5,
              rng=random.Random(1).random)
    with pytest.raises(ValueError):
        yaep.Async(g).start()
    with pytest.raises(ValueError):
        yaep.Async(g, context=[0.5])           # a 1-D GP has no context


# --- errors -------------------------------------------------------------------

def test_error_hierarchy():
    for e in (yaep.Numeric, yaep.NoCross, yaep.Full, yaep.Busy, yaep.Timeout):
        assert issubclass(e, yaep.Error)


def test_bad_desc():
    with pytest.raises(yaep.Error):
        yaep.GP(lo=[0], hi=[1], target_p=0.75)          # no stop criterion
    with pytest.raises(TypeError):
        yaep.GP(lo=[0], hi=[1], stop_trials=5, bogus=1)
    with pytest.raises(TypeError):
        yaep.GP([0], [1])
    with pytest.raises(ValueError):
        yaep.GP(lo=[0, 0], hi=[1], stop_trials=5)
    with pytest.raises(ValueError):
        yaep.GP(lo=[0], hi=[1], stop_trials=5, acq="nope")


def test_nocross_and_full():
    g = yaep.GP(lo=[0.0], hi=[1.0], target_p=0.75, stop_trials=3, max_trials=3)
    with pytest.raises(yaep.NoCross) as ei:
        g.threshold()                  # the prior is flat at p = 0.5
    assert ei.value.code == yaep.ERR_NOCROSS
    for k in range(3):
        g.update([0.5], k % 2)
    assert g.done and g.stop_reason in (yaep.STOP_TRIALS, yaep.STOP_FULL)
    with pytest.raises(yaep.Full):
        g.update([0.5], 1)
    with pytest.raises(yaep.Full):
        g.next()


def test_numeric_keeps_the_trial():
    # A noiseless Gaussian model with duplicate stimuli: the kernel matrix is
    # singular, which is the one reliable way to make a Cholesky fail.
    g = yaep.GP(lo=[0.0], hi=[1.0], lik="gaussian", acq="balv", stop_trials=10,
              hyper={"noise_sd": 1e-200, "outputscale": 1.0,
                     "lengthscale": [0.5], "mean": 1e-12}, jitter=1e-300)
    g.update_real([0.5], 0.0)
    with pytest.raises(yaep.Numeric) as ei:
        g.update_real([0.5], 1.0)
    assert ei.value.code == yaep.ERR_NUMERIC
    assert g.n_trials == 2
    assert math.isfinite(g.predict_p(0.5))


def test_argument_errors_and_close():
    g = yaep.GP(lo=[0.0], hi=[1.0], target_p=0.75, stop_trials=5)
    with pytest.raises(yaep.Error) as ei:
        g.update([1.5], 1)
    assert ei.value.code == yaep.ERR_ARG
    with pytest.raises(ValueError):
        g.update([0.1, 0.2], 1)
    with pytest.raises(yaep.Error):
        g.predict_p(2.0)
    g.close()
    with pytest.raises(yaep.Error) as ei:
        g.next()
    assert ei.value.code == yaep.ERR_CLOSED


# --- odds and ends --------------------------------------------------------------

def test_memory_size_is_the_layout():
    base = dict(lo=[0, 0], hi=[1, 1], n_candidates=512, max_trials=512,
                target_p=0.75, stop_trials=5)
    lse = yaep.GP.memory_size(**base)
    eavc = yaep.memory_size(acq="eavc", **base)
    # EAVC adds exactly the M x M look-ahead covariance and its M x N block.
    assert eavc - lse == 512 * 512 * 8 + 512 * 512 * 8
    with pytest.raises(yaep.Error):
        yaep.GP.memory_size(lo=[0], hi=[1])


def test_hyper_object_and_candidates():
    h = yaep.Hyper(lengthscale=[0.2], outputscale=2.0, mean=0.5)
    cand = [[i / 20] for i in range(21)]
    g = yaep.GP(lo=[0.0], hi=[1.0], hyper=h, candidates=cand, target_p=0.75,
              n_init=2, stop_trials=10)
    assert g.hyper()["lengthscale"] == [0.2]
    assert g.hyper()["outputscale"] == 2.0
    assert g.n_candidates == 21
    assert g.candidate(20) == [1.0]
    del cand                  # the binding keeps its own copy
    for k in range(4):
        i, x = g.next()
        g.update(x, k % 2)
    i, x = g.next()
    assert 0 <= i < 21 and x == g.candidate(i)
    assert math.isfinite(g.acq_score(i))
    g.update(x, 1)            # an unanswered proposal would be handed out again
    j, y = g.next_subset([3, 4, 5])
    assert j in (3, 4, 5) and y == g.candidate(j)


def test_rng_callable():
    r = random.Random(5)
    g = yaep.GP(lo=[0.0], hi=[1.0], acq="random", stop_trials=10, rng=r.random)
    idx, x = g.next()
    assert idx == -1 and 0.0 <= x[0] <= 1.0

    def broken():
        raise RuntimeError("generator failed")
    g = yaep.GP(lo=[0.0], hi=[1.0], acq="random", stop_trials=10, rng=broken,
              n_init=0)
    for k in range(4):
        g.update([k / 4], k % 2)
    with pytest.raises(RuntimeError):
        g.next()


def test_fit_step_and_refit():
    g = fitted_2d(trials=30, fit=False, refit_every=5)
    before = g.log_marginal
    steps = 0
    while g.fit_step():
        steps += 1
        assert steps < 100
    assert g.log_marginal != before
    g.refit()
    g.fit()


def test_simulate_outcome():
    assert yaep.simulate_outcome([0.25, 0.75], 0.2) == 0
    assert yaep.simulate_outcome([0.25, 0.75], 0.3) == 1
    assert yaep.GP.simulate_outcome([0.1, 0.2, 0.7], 0.25) == 1


def test_second_async_cannot_take_or_release_the_gp():
    g = yaep.GP(lo=[0.0], hi=[1.0], target_p=0.75, stop_trials=20)
    a, b = yaep.Async(g), yaep.Async(g)
    a.start()
    try:
        with pytest.raises(yaep.Busy):
            b.start()
        b.stop()                        # never started: must not free the GP
        with pytest.raises(yaep.Busy):
            g.next()
        seq = a.submit([0.5], 1)
        got, snap = a.wait(seq, timeout_s=10.0)
        assert got >= seq and snap.n_trials == 1
    finally:
        a.stop()
    assert g.n_trials == 1


def test_version_matches_pyproject():
    # pyproject.toml's version is a copy; __version__ comes from the header
    # compiled into the extension. Fail when the two drift.
    path = os.path.join(os.path.dirname(__file__), "..", "pyproject.toml")
    if not os.path.exists(path):
        pytest.skip("pyproject.toml is not beside the tests")
    with open(path, encoding="utf-8") as f:
        m = re.search(r'^version\s*=\s*"([^"]+)"', f.read(), re.M)
    assert m, "no version in pyproject.toml"
    assert yaep.__version__ == m.group(1)


def test_refine_steps():
    with pytest.raises(yaep.Error):
        yaep.GP(lo=[0], hi=[1], target_p=0.75, stop_trials=5, refine_steps=33)
    u = random.Random(9)
    g = yaep.GP(lo=[0.0], hi=[1.0], target_p=0.75, grid=[9], n_init=4,
              stop_trials=30, refine_steps=2)
    off_grid = 0
    while not g.done:
        idx, x = g.next()
        if g.n_trials >= 4 and idx == -1:
            off_grid += 1
        p = phi(SLOPE * (x[0] - X50))
        g.update(x, yaep.simulate_outcome([1 - p, p], u.random()))
    # The refined proposals leave the 9-point grid (0.125 apart).
    assert off_grid > 0
    assert any(abs(h["x"][0] * 8 - round(h["x"][0] * 8)) > 1e-9 for h in g.history())


def test_psychometric_model():
    # The header rejects what the model cannot carry, at open.
    base = dict(lo=[0.0, 0.0], hi=[1.0, 1.0], intensity_dim=1, target_p=0.75,
                stop_trials=60, model="psychometric")
    for bad in (dict(kernel="semip"), dict(lik="categorical", n_outcomes=3),
                dict(lik="gaussian")):
        with pytest.raises(yaep.Error):
            yaep.GP(**dict(base, **bad))
    # The look-ahead acquisitions are accepted since ysp/aep.h 0.4.0.
    for ok in ("eavc", "localmi"):
        la = yaep.GP(**dict(base, acq=ok, grid=[5, 9], n_init=4, stop_trials=8))
        uu = random.Random(3)
        while not la.done:
            _, x = la.next()
            p = phi(8.0 * (x[1] - 0.5))
            la.update(x, yaep.simulate_outcome([1 - p, p], uu.random()))
        assert la.n_trials == 8
    assert yaep.MODEL_PSYCHOMETRIC != yaep.MODEL_GP

    u = random.Random(21)
    g = yaep.GP(grid=[9, 17], n_init=8, fit=True, fit_every=20, acq="lse",
              **dict(base, model=yaep.MODEL_PSYCHOMETRIC, stop_trials=100))
    while not g.done:
        _, x = g.next()
        p = phi(8.0 * (x[1] - (0.3 + 0.3 * x[0])))
        g.update(x, yaep.simulate_outcome([1 - p, p], u.random()))
    h = g.hyper()
    assert {"lengthscale_g", "outputscale_g", "mean_g"} <= set(h)
    assert len(h["lengthscale_g"]) == 2 and h["outputscale_g"] > 0.0

    z75 = 0.67448975019608171
    errs = []
    for ctx in (0.2, 0.5, 0.8):
        thr, lo, hi = g.threshold([ctx])
        # m and log-slope g at this context: k = 0 and k = 1; intensity ignored
        mm, sm = g.predict_f([ctx, 0.5], k=0)
        mg, sg = g.predict_f([ctx, 0.5], k=1)
        # E[m + f_t exp(-g)] for (m, g) jointly Gaussian needs no covariance
        closed = mm + z75 * math.exp(-mg + sg * sg / 2.0)
        assert abs(thr - closed) < 1e-9 * max(1.0, abs(closed))
        assert lo <= thr <= hi
        if 0.0 < lo and hi < 1.0:          # unclamped: E -+ 1.96 sd
            assert abs((thr - lo) - (hi - thr)) < 1e-9
        errs.append(abs(thr - (0.3 + 0.3 * ctx + z75 / 8.0)))
    # One stream: a mean over three contexts at 100 trials, with the 1-D
    # test's tolerance, rather than a per-context bound one seed can break.
    assert sum(errs) / len(errs) < 0.08
    assert not g.threshold_multi_cross


def test_max_trials_is_the_compiled_ceiling():
    # MAX_TRIALS is YAEP_MAX_TRIALS as compiled (YSP_AEP_MAX_TRIALS at build
    # time). max_trials = 0 means that ceiling, and one more is refused.
    base = dict(lo=[0, 0], hi=[1, 1], n_candidates=64, target_p=0.75,
                stop_trials=5)
    assert yaep.MAX_TRIALS >= 16
    assert (yaep.memory_size(max_trials=0, **base)
            == yaep.memory_size(max_trials=yaep.MAX_TRIALS, **base))
    assert (yaep.memory_size(max_trials=yaep.MAX_TRIALS, **base)
            > yaep.memory_size(max_trials=yaep.MAX_TRIALS - 1, **base))
    with pytest.raises(yaep.Error):
        yaep.memory_size(max_trials=yaep.MAX_TRIALS + 1, **base)
    g = yaep.GP(max_trials=yaep.MAX_TRIALS, **base)
    assert g.is_open


# --- v0.5.0 .. v0.13.1: optimization, pairwise, kinds, priors, snapshots ---

def bump(x, cx=0.3, cy=0.7, w=0.12):
    return math.exp(-((x[0] - cx) ** 2 + (x[1] - cy) ** 2) / (2.0 * w * w))


@pytest.mark.parametrize("minimize", [False, True])
def test_ucb_finds_the_maximum_of_a_bump(minimize):
    u = random.Random(31)
    sign = -1.0 if minimize else 1.0
    g = yaep.GP(lo=[0.0, 0.0], hi=[1.0, 1.0], lik="gaussian", acq="ucb",
              minimize=minimize, grid=[21, 21], n_init=8, fit=True,
              fit_every=10, stop_trials=40)
    while not g.done:
        _, x = g.next()
        g.update_real(x, sign * bump(x) + 0.02 * (u.random() - 0.5))
    x, v = g.argmax()
    assert math.hypot(x[0] - 0.3, x[1] - 0.7) < 0.05
    assert abs(v - sign * 1.0) < 0.15


def test_ei_and_thompson_run():
    for acq, extra in (("ei", {}), ("thompson", {"rng": random.Random(4).random})):
        u = random.Random(32)
        g = yaep.GP(lo=[0.0, 0.0], hi=[1.0, 1.0], lik="gaussian", acq=acq,
                  grid=[15, 15], n_init=8, fit=True, fit_every=10,
                  stop_trials=30, **extra)
        while not g.done:
            _, x = g.next()
            g.update_real(x, bump(x) + 0.02 * (u.random() - 0.5))
        x, v = g.argmax()
        assert math.hypot(x[0] - 0.3, x[1] - 0.7) < 0.1, acq
    with pytest.raises(yaep.Error):     # THOMPSON needs a generator
        yaep.GP(lo=[0], hi=[1], lik="gaussian", acq="thompson", stop_trials=5)


def test_pairwise_session():
    u = random.Random(33)
    g = yaep.GP(lo=[0.0, 0.0], hi=[1.0, 1.0], lik="pairwise", acq="bald",
              grid=[15, 15], n_init=8, fit=True, fit_every=10, stop_trials=80)
    while not g.done:
        x1, x2 = g.next_pair()
        p = phi(2.0 * (3.0 * bump(x1) - 3.0 * bump(x2)))
        g.update_pair(x1, x2, 1 if u.random() < p else 0)
    x, _ = g.argmax()
    assert math.hypot(x[0] - 0.3, x[1] - 0.7) < 0.15
    h = g.history()
    assert len(h) == 80 and all(len(t["x2"]) == 2 for t in h)
    a, b = [0.3, 0.7], [0.9, 0.1]
    assert g.predict_pair(a, b) > 0.8
    assert abs(g.predict_pair(a, b) + g.predict_pair(b, a) - 1.0) < 1e-9
    with pytest.raises(yaep.Error):
        g.next()                     # a comparison needs next_pair()
    with pytest.raises(ValueError):
        yaep.Async(g).start()


def test_categorical_dimension():
    thr = {0: 0.3, 1: 0.6, 2: 0.45}   # an unordered context: level 1 is far
    u = random.Random(34)
    g = yaep.GP(lo=[0.0, 0.0], hi=[2.0, 1.0], intensity_dim=1,
              dim_kind=["categorical", yaep.DIM_CONTINUOUS], dim_levels=[3, 0],
              target_p=0.75, grid=[3, 21], n_init=8, fit=True, fit_every=10,
              refine_steps=2, stop_trials=90)
    while not g.done:
        _, x = g.next()
        assert x[0] in (0.0, 1.0, 2.0)
        p = phi(10.0 * (x[1] - thr[int(x[0])]))
        g.update(x, yaep.simulate_outcome([1 - p, p], u.random()))
    for level, t50 in thr.items():
        est, lo, hi = g.threshold([level])
        assert abs(est - (t50 + 0.67449 / 10.0)) < 0.1, level
    with pytest.raises(yaep.Error):     # the box of a categorical dimension is 0..levels-1
        yaep.GP(lo=[0.0, 0.0], hi=[3.0, 1.0], intensity_dim=1,
              dim_kind=["categorical", "continuous"], dim_levels=[3, 0],
              target_p=0.75, stop_trials=5)


def snapshot_desc():
    return dict(lo=[0.0, 0.0], hi=[1.0, 1.0], intensity_dim=1, acq="eavc",
                target_p=0.75, grid=[9, 17], n_init=6, fit=True, fit_every=10,
                refine_steps=2, stop_trials=40)


def snapshot_run(cut=None):
    u = random.Random(35)
    g = yaep.GP(**snapshot_desc())
    proposals = []
    while not g.done:
        _, x = g.next()
        if cut is not None and g.n_trials == cut:
            # cut with the proposal pending: the resumed GP hands it out again
            g = yaep.GP.load(g.save(), **snapshot_desc())
            _, again = g.next()
            assert again == x
        proposals.append(x)
        p = phi(8.0 * (x[1] - (0.3 + 0.3 * x[0])))
        g.update(x, yaep.simulate_outcome([1 - p, p], u.random()))
    return proposals, g.save(), g


def test_snapshot_resume_matches_uninterrupted_run():
    ref_props, ref_bytes, ref = snapshot_run()
    for cut in (7, 20, 33):
        props, final, g = snapshot_run(cut)
        assert props == ref_props, cut
        assert final == ref_bytes, cut          # byte for byte
        assert g.threshold([0.5]) == ref.threshold([0.5])
    snap = ref.save()
    with pytest.raises(yaep.Error):               # the desc must agree
        yaep.GP.load(snap, **dict(snapshot_desc(), target_p=0.7))
    with pytest.raises(yaep.Error):               # a truncated image is refused
        yaep.GP.load(snap[:-3], **snapshot_desc())


def test_fit_return_values_and_delta():
    g = yaep.GP(lo=[0.0, 0.0], hi=[1.0, 1.0], target_p=0.75, n_candidates=64,
              stop_trials=60, fit_max_evals=2)
    assert g.fit() == 1                         # nothing to fit yet
    u = random.Random(36)
    for _ in range(60):
        x = [u.random(), u.random()]
        p = phi(8.0 * (x[1] - 0.5))
        g.update(x, yaep.simulate_outcome([1 - p, p], u.random()))
    assert g.fit() == 0                         # two evaluations do not converge
    assert g.fit_delta() > 0.0
    rounds, rc = 0, 0
    while rc != 1 and rounds < 200:
        rc = g.fit()
        rounds += 1
    assert rc == 1 and rounds > 1
    assert abs(g.fit_delta()) < 1e-3


def test_priors_and_other_desc_fields():
    base = dict(lo=[0.0, 0.0], hi=[1.0, 1.0], target_p=0.75, stop_trials=5)
    d = yaep.GP(**base).get_priors()
    assert set(d) == {"lengthscale", "outputscale", "outputscale_b",
                      "outputscale_g", "mean", "mean_g", "noise_sd"}
    assert d["outputscale"]["center"] == 1.0 and d["outputscale"]["sd"] > 0.0
    got = yaep.GP(priors={"outputscale": (2.0, 0.5)}, **base).get_priors()
    assert got["outputscale"]["center"] == 2.0 and got["outputscale"]["sd"] == 0.5
    obj = yaep.Priors(mean={"center": 0.5, "sd": 0.3})
    assert yaep.GP(priors=obj, **base).get_priors()["mean"]["center"] == 0.5
    off = yaep.GP(no_hyper_prior=True, **base).get_priors()
    assert off["lengthscale"]["sd"] == -1.0
    with pytest.raises(TypeError):
        yaep.GP(priors={"bogus": (1, 1)}, **base)

    # monotone_dims: a mask or a list of dimensions, the same thing
    a = yaep.GP(monotone_dims=[1], intensity_dim=1, grid=[5, 9], **base)
    b = yaep.GP(monotone_dims=2, intensity_dim=1, grid=[5, 9], **base)
    for x, y in (([0.2, 0.3], 0), ([0.8, 0.9], 1), ([0.5, 0.5], 1)):
        a.update(x, y)
        b.update(x, y)
    assert a.predict_p_many(grid_points(5)).tolist() == b.predict_p_many(grid_points(5)).tolist()
    with pytest.raises(yaep.Error):     # the psychometric model is monotone already
        yaep.GP(monotone_dims=[1], intensity_dim=1, model="psychometric", **base)
    yaep.GP(pcg_threshold=-1, fit_tol=1e-6, **base)
    yaep.GP(pcg_threshold=64, **base)


@pytest.mark.parametrize("model", ["gp", "psychometric"])
def test_fit_pcg_reaches_the_same_fit(model):
    def session(pcg):
        u = random.Random(37)
        g = yaep.GP(lo=[0.0, 0.0], hi=[1.0, 1.0], intensity_dim=1, target_p=0.75,
                  model=model, grid=[9, 17], n_init=8, fit=True, fit_every=0,
                  stop_trials=60, fit_pcg=pcg)
        for _ in range(60):
            x = [u.random(), u.random()]
            p = phi(8.0 * (x[1] - (0.3 + 0.3 * x[0])))
            g.update(x, yaep.simulate_outcome([1 - p, p], u.random()))
        for _ in range(50):          # converge through the evaluation budget
            if g.fit() == 1:
                break
        return g.log_marginal
    assert abs(session(True) - session(False)) < 1e-6
