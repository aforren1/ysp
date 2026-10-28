# ysp.aep Python binding

A CPython extension that wraps the
[ysp/aep.h](../../../include/ysp/aep.h) single-header library: Gaussian-process adaptive
psychophysics, the subset of AEPsych that fits in one C file. It also compiles
the header's optional async layer (`YAEP_ASYNC`), which runs the inference on
a C thread from the shared [ysp/rt.h](../../../include/ysp/rt.h).

The binding uses the plain CPython C API and has no dependencies: no NumPy, no
PyTorch, no pybind. It targets the **Limited API / stable ABI**
(`Py_LIMITED_API = 0x030A0000`), so one `ysp/aep.abi3.so` (`aep.pyd` on
Windows) works on CPython 3.10 and later.

The distribution is `ysp-aep` and the module is `ysp.aep`. `ysp` is a
[PEP 420](https://peps.python.org/pep-0420/) implicit namespace package (no
`__init__.py`), so `ysp-aep` installs next to `ysp-serial`, `ysp-parallel` and
the other `ysp-*` bindings, or alone.

This page has three parts: how to install and use the binding, the API
reference, and an explanation of the threading rules and of what to log.

## How to install

From this directory:

```sh
pip install .                       # build and install an abi3 wheel
# or, in place for development:
python setup.py build_ext --inplace
```

With uv, from the repository root:

```sh
uv pip install ./bindings/python/ysp_aep
```

The in-place build puts the extension into `ysp/`. That directory is otherwise
empty and stays in git for this purpose.

setup.py finds the headers in the repository's `include/` directory in a
development tree. A
build outside the tree, such as an sdist or a cibuildwheel run, sees only this
directory. Copy `ysp/aep.h` and `ysp/rt.h` into `include/ysp/` next to
`setup.py` first:

```sh
mkdir -p include/ysp && cp ../../../include/ysp/aep.h ../../../include/ysp/rt.h include/ysp/
uv build --sdist                    # or: python -m build --sdist
```

The two copies are gitignored. When the repository root is present, its
headers win, so a stale copy cannot shadow a newer header.

### How to allow more than 512 trials

The trial ceiling, `ysp.aep.MAX_TRIALS`, is fixed when the extension is
compiled. Set `YSP_AEP_MAX_TRIALS` at build time to raise it:

```sh
YSP_AEP_MAX_TRIALS=1024 uv pip install ./bindings/python/ysp_aep
python -c "import ysp.aep; print(ysp.aep.MAX_TRIALS)"     # 1024
```

The ceiling itself costs nothing in a `GP` object: the history and the
matrices are allocated per handle from `max_trials` (the history is 144
bytes a trial), so they cost only when a run asks for that many trials. At
`max_trials=1024`, `memory_size()` measured about 26 MB for the GP model with
LSE, 33 MB with EAVC, 35 MB for the psychometric model, and 52 MB for a
three-class categorical model (two dimensions, 512 candidates). The matrices
grow as the square of `max_trials`. The time per exact refit grows as its
cube, so a long run needs `refit_every` or `fit_step()`. Use the same value
wherever the wheel is built; `pip` does not see the variable in a cached
wheel.

On Linux and macOS the build links `-pthread` (for the async thread) and
`libm`. Windows needs no extra libraries.

## How to run an adaptive session

```python
import ysp.aep as yaep

gp = yaep.GP(
    lo=[0.0, -3.0], hi=[1.5, 0.0],     # log10 spatial frequency, log10 contrast
    intensity_dim=1,                   # thresholds are along contrast
    acq=yaep.ACQ_EAVC,                   # or "eavc"
    target_p=0.75,
    grid=[15, 25],                     # candidate grid, 375 points
    n_init=10,                         # Halton trials first
    fit=True, fit_every=10,            # fit the hyperparameters every 10 trials
    stop_trials=150,
)

while not gp.done:
    index, x = gp.next()               # x is a list of n_dims floats
    seen = run_trial(x)                # your code: 1 = yes, 0 = no
    gp.update(x, seen)                 # the stimulus you SHOWED, and the outcome

thr, lo, hi = gp.threshold([0.7])      # contrast threshold at log10 SF 0.7
```

Give `update()` the stimulus that you showed, not the one that was proposed.
If the display quantized it, the history records that. A proposal without a
response is proposed again by the next `next()`.

The other likelihoods:

```python
# a four-point confidence rating; the level set is "rated 2 or above"
yaep.GP(..., lik="ordinal", n_outcomes=4, target_outcome=2, target_p=0.5)
# which of three intervals (softmax over three latent GPs)
yaep.GP(..., lik="categorical", n_outcomes=3, target_outcome=1, target_p=0.5)
# a continuous setting; use update_real(x, y)
yaep.GP(..., lik="gaussian", acq="balv", target_value=0.5)
```

A candidate grid quantizes every proposal: on an 11 x 21 grid over 140 dB,
that is 7 dB along intensity. `refine_steps=2` moves the grid winner off the
grid by a short golden-section search between its neighbors, for about a
fifth of the cost of the candidate sweep. On the audiometric benchmark of the
header's manual it takes the LSE threshold error at 150 trials from 1.98 to
1.50 dB.

When the threshold is what you want and the context varies smoothly, try
`model="psychometric"`. Every slice along the intensity is then a
psychometric function by construction, and `threshold()` returns the
posterior mean of m + f* exp(-g) in closed form, with a band of +-1.96
posterior sd. A stationary GP needs one lengthscale for the whole intensity
axis, so it cannot hold a rise that is narrow against the box.

## How to find the best stimulus instead of a threshold

```python
gp = yaep.GP(lo=[0, 0], hi=[1, 1], lik="gaussian", acq="ucb", grid=[21, 21],
           n_init=8, fit=True, fit_every=10, stop_trials=40)
while not gp.done:
    _, x = gp.next()
    gp.update_real(x, rating(x))
x_best, value = gp.argmax()
```

`acq="thompson"` needs `rng=` (for example `numpy.random.default_rng(1).random`).
For "which of the two did you prefer" trials, use `lik="pairwise"` with
`acq="bald"` or `"thompson"`, `x1, x2 = gp.next_pair()` and
`gp.update_pair(x1, x2, 1 if first_preferred else 0)`. `argmax()` then gives
the favorite stimulus.

## How to resume a session

```python
data = gp.save()                     # bytes; store it with your data
gp = yaep.GP.load(data, **desc)        # the same desc; the session continues
```

The resumed session proposes, updates and estimates exactly as the
uninterrupted one would. Save and restore your generator's state yourself if
you set `rng`.

## How to plot the field

`predict_p_many()` evaluates the model at many points in one blocked pass.
It takes any buffer (a NumPy array, `array.array('d')`) or a sequence of
points, and it returns a `memoryview` of doubles:

```python
import numpy as np
xs = np.column_stack([cc.ravel(), ii.ravel()])     # n x n_dims
p = np.frombuffer(gp.predict_p_many(xs)).reshape(cc.shape)
mu, sd = (np.frombuffer(v) for v in gp.predict_f_many(xs))
```

The result is a copy, not a view into the handle. The Limited API before
Python 3.11 has no buffer slot, so a zero-copy result waits for a later
version. A float64 C-order or Fortran-order array goes in through one copy.
Other dtypes go through `tolist()`, which is slower.

## How to keep the frame loop free (Async)

An EAVC selection on a few hundred candidates takes 5 to 30 ms, and a
hyperparameter fit takes hundreds of ms. That does not fit between two frames
at 60 Hz. `ysp.aep.Async` runs `update`, `next` and `threshold` on a C thread,
so a PsychoPy frame loop only polls:

```python
gp = yaep.GP(**desc)
with yaep.Async(gp, context=[0.5], fit_in_idle=True, below_normal=True) as a:
    seq, snap = a.poll()                  # seq 0: the first stimulus is ready
    while not snap.done:
        k = run_trial_frames(snap.x)      # your win.flip() loop
        seq = a.submit(snap.x, k)         # returns at once
        while True:                       # the inter-trial interval
            win.flip()
            got, snap = a.poll()          # an atomic load and a struct copy
            if got >= seq:
                break                     # snap.x is the next stimulus
# stop() has run: every queued response is applied, and gp is yours again
print(gp.threshold([0.5]), gp.hyper(), gp.log_marginal)
```

This is the way to run a 30 ms EAVC selection under a PsychoPy frame loop.
The loop keeps drawing while the thread computes. When the proposal is late,
the loop shows one more frame of the same screen; it does not block.

`Async` refuses a `"pairwise"` GP, because it submits single stimuli.

`submit()` raises `ysp.aep.Busy` when the queue (`ASYNC_QUEUE`, 8 responses)
is full. Nothing was copied, so keep the response and try again on the next
frame. `wait(seq, timeout_s)` is the blocking form for the end of an interval;
it releases the GIL.

See [example.py](example.py) for both loops on a simulated observer.

## API reference

### Module

| | |
|---|---|
| `GP(**desc)` | open a run; the keywords are the `yaep_desc` fields below |
| `Async(gp, context=None, target=0, fit_in_idle=False, below_normal=False, pin_cpu=0)` | the inference thread for `gp` |
| `Hyper(**fields)` | a namespace of hyperparameter fields for `hyper=`, `hyper_min=`, `hyper_max=`; a dict works too |
| `Priors(**fields)` | a namespace of the weak priors for `priors=`; a dict works too |
| `Snapshot` | the type of what `Async.poll()` and `Async.wait()` return |
| `simulate_outcome(p, u) -> int` | the smallest k with cumulative `p` > `u` (`u` in [0, 1) from your generator) |
| `memory_size(**desc) -> int` | bytes `GP(**desc)` allocates at open; raises `Error` with the validation message on a bad desc |
| `__version__` | the version of the compiled `ysp/aep.h` (`yaep_version()`), for example `"0.14.0"`; log it with your data |

### GP keywords

Every keyword is optional. An omitted, `None` or zero field takes the library
default. `n_dims` comes from `len(lo)` unless you give it.

| Keyword | Type | Meaning |
|---|---|---|
| `lo`, `hi` | sequence of floats | the box, one entry per dimension |
| `intensity_dim` | int | the dimension that thresholds run along (default 0) |
| `lik` | `LIK_*` or name | `"bernoulli"` (default), `"ordinal"`, `"categorical"`, `"gaussian"`, `"pairwise"` (a trial compares two stimuli; use `next_pair()` and `update_pair()`) |
| `n_outcomes` | int | K, for ordinal and categorical |
| `model` | `MODEL_*` or name | `"gp"` (default): one GP over the box. `"psychometric"`: a threshold GP m(c) and a log-slope GP g(c) over the context, f = exp(g) (x - m). Bernoulli or ordinal and the RBF kernel only; every acquisition but `"thompson"` works |
| `kernel` | `KERNEL_*` or name | `"rbf"` (default) or `"semip"` |
| `link` | `LINK_*` or name | `"probit"` (default) or `"logit"` |
| `guess`, `lapse` | float | floor and ceiling of p; fixed, never fitted |
| `hyper`, `hyper_min`, `hyper_max` | dict or `Hyper` | fixed values and fit bounds; zero means default or fit |
| `fit`, `fit_every` | bool, int | fit the free hyperparameters every `fit_every` trials after the init phase |
| `no_hyper_prior` | bool | fit type-II maximum likelihood instead of the weak priors |
| `priors` | dict or `Priors` | the weak priors, by name: `lengthscale`, `outputscale`, `outputscale_b`, `outputscale_g`, `mean`, `mean_g`, `noise_sd`. Each is `{center, sd, ceiling}` or a `(center, sd[, ceiling])` tuple. 0 or missing is the measured default; a negative `sd` turns that prior off. `get_priors()` shows what is in force |
| `fit_max_evals`, `fit_tol` | int, float | one fit's objective evaluations (0 = 40) and its gradient tolerance (0 = 1e-8) |
| `fit_pcg` | bool | conjugate-gradient Newton steps inside hyperparameter fits and the psychometric mode search, one factorization per search. Off by default because results differ from the default path at tolerance level; ignored under `"gaussian"` and `"categorical"`. Measured by the header on 6-D, 500 trials: a GP fit 6.9 to 3.4 s, a psychometric fit 168 to 43 s |
| `refit_every` | int | rank-one updates between exact Newton refits |
| `jitter` | float | kernel diagonal jitter (default 1e-6) |
| `acq` | `ACQ_*` or name | level set: `"lse"` (default), `"eavc"`, `"localmi"`; field: `"balv"`, `"bald"`; `"random"`; optimization: `"ucb"`, `"ei"`, `"thompson"` (needs `rng`; GP model only) |
| `minimize` | bool | the optimization acquisitions and `argmax()` minimize the target quantity |
| `target_p`, `target_value`, `target_outcome` | float, float, int | the level set; the level-set acquisitions need one |
| `acq_beta` | float | LSE straddle width (default 1.96) |
| `n_init` | int | Halton trials before the acquisition runs |
| `rng` | callable | returns a uniform in [0, 1); breaks ties and drives `"random"`, for example `numpy.random.default_rng(1).random` |
| `candidates` | buffer or sequence of points | an explicit M x n_dims candidate set; the binding keeps a copy |
| `n_candidates`, `grid` | int, sequence of ints | a Halton set of M points, or a product grid |
| `stop_trials`, `stop_threshold_sd`, `stop_context` | int, float, sequence | stop rules; one is required |
| `max_trials` | int | the hard ceiling and the size of the allocation (at most `MAX_TRIALS`, 512 unless the build raised it) |
| `dim_kind`, `dim_levels` | sequences | per dimension `DIM_CONTINUOUS` (default), `DIM_INTEGER` or `DIM_CATEGORICAL` (or the lower-case names), and a categorical dimension's level count. A categorical dimension's box is `[0, levels - 1]`; the intensity dimension stays continuous |
| `monotone_dims` | int mask or sequence of ints | project the posterior mean to increase along these dimensions (AEPsych's monotonic projection); GP model, one latent only |
| `pcg_threshold` | int | above this many trials an update solves by preconditioned conjugate gradients; 0 = 512, negative = never |
| `refine_steps` | int, 0 to 32 | rounds of golden-section refinement of the grid winner between its neighbors; 0 (default) keeps proposals on the grid. A refined proposal returns index -1. EAVC refines on the LSE straddle, because its score has no value off the grid |

A caller-owned memory buffer (`desc.memory`) is not exposed.

### GP methods

| | |
|---|---|
| `next() -> (index, x)` | the next stimulus; `index` is a candidate index, or -1 for a Halton or `rng` point |
| `next_subset(indices) -> (index, x)` | `next()` restricted to some candidates; skips the init phase |
| `acq_score(i) -> float` | the acquisition score of candidate `i` (NaN in the init phase) |
| `update(x, outcome)` | record an outcome in 0..K-1 at the stimulus shown, and refit |
| `update_real(x, y)` | the same for a continuous `y` under `"gaussian"` |
| `predict_f(x, k=0) -> (mu, sd)` | latent `k` at `x` (k > 0 only under categorical); under `model="psychometric"`, k = 0 is the threshold m and k = 1 the log slope g at `x`'s context |
| `predict_p(x) -> float` | the target quantity at `x`: E[P(y=1)], E[P(y>=k*)], E[P(y=k*)] or E[y] |
| `predict_p_var(x) -> float` | its posterior variance (the BALV score) |
| `predict_outcomes(x) -> list` | all K outcome probabilities |
| `predict_p_many(xs) -> memoryview` | `predict_p` at n points, format `'d'` |
| `predict_f_many(xs, k=0) -> (memoryview, memoryview)` | `predict_f` at n points |
| `threshold(ctx=None, target=0) -> (x, lo, hi)` | where `predict_p` crosses `target` along the intensity dimension at context `ctx` (the other `n_dims - 1` coordinates); raises `NoCross` |
| `fit() -> int` | fit the free hyperparameters: 1 when it converged, 0 when `fit_max_evals` or a guard stopped it (call again to continue); raises only on an error |
| `fit_delta() -> float` | the fit objective's change over the last whole fit; repeat `fit()` until it is small |
| `argmax() -> (x, value)` | where the posterior mean of the target quantity is highest (lowest under `minimize`), and that mean |
| `get_priors() -> dict` | the priors in force, defaults filled in; one that is off has `sd` -1 |
| `next_pair() -> (x1, x2)` | `"pairwise"`: the next comparison |
| `update_pair(x1, x2, outcome)` | `"pairwise"`: 1 if `x1` was preferred, 0 if `x2` |
| `predict_pair(x1, x2) -> float` | `"pairwise"`: P(`x1` is preferred to `x2`) |
| `save() -> bytes` | a snapshot of the whole session, pending proposal included (not the `rng` state) |
| `GP.load(data, **desc) -> GP` | resume from `save()`, bit for bit, without replay. The desc must agree with the snapshot on every number and re-supplies `rng` and `candidates` |
| `fit_step() -> bool` | one step of that fit; `True` while another step helps |
| `refit()` | the exact refit that `refit_every` skips |
| `hyper() -> dict` | `lengthscale`, `outputscale`, `mean`, `lengthscale_b`, `outputscale_b`, `cutpoint`, `noise_sd`, `lengthscale_g`, `outputscale_g`, `mean_g`; under `model="psychometric"` the first three belong to the threshold GP and the `_g` fields to the log-slope GP |
| `candidate(i) -> list` | candidate `i` |
| `history() -> list[dict]` | every trial as `{'x', 'y', 'proposed', 'init'}`, plus `'x2'` under `"pairwise"` |
| `close()` | free the handle; `GP` is also a context manager |

Read-only properties: `done`, `stop_reason` (`STOP_*`), `log_marginal`,
`n_candidates`, `n_trials`, `threshold_multi_cross`, `n_dims`, `is_open`.
`done` runs a threshold search when `stop_threshold_sd` is set, so read it
once per trial.

### Async

| | |
|---|---|
| `start()` / `stop()` | take the GP over and start the thread; drain the queue, join, and give the GP back. `with Async(gp) as a:` does both |
| `submit(x, outcome) -> seq` | queue one response and return at once |
| `submit_real(x, y) -> seq` | the same under `"gaussian"` |
| `poll() -> (seq, Snapshot)` | the newest snapshot; `seq >= submit's seq` means that response is applied |
| `wait(seq, timeout_s=None) -> (seq, Snapshot)` | block until then, with the GIL released; raises `Timeout` |
| `pending` | responses queued and not applied yet (takes the queue mutex; for logs) |
| `policy` | `POLICY_NORMAL` or `POLICY_BELOW_NORMAL` (what the OS granted), `POLICY_NONE` when stopped |
| `running`, `gp` | state, and the GP it drives |

A `Snapshot` has `seq`, `x`, `proposed`, `update_rc`, `n_trials`, `done`,
`stop`, `threshold`, `threshold_lo`, `threshold_hi`, `threshold_rc`,
`multi_cross`, `hyper` (a dict), `log_marginal`, `numeric` and `fitting`.
`threshold_rc` is 0, `ERR_NOCROSS` or another `ERR_*` code; `update_rc` is 0 or the code that
`update` returned on the thread for that response.

### Errors

| Exception | Raised when |
|---|---|
| `Error` | base class; also a bad desc, a closed handle (`.code == ERR_CLOSED`) or a bad argument (`ERR_ARG`) |
| `Numeric` | a Cholesky failed. The trial **is** recorded and the previous posterior stays in force; continue |
| `NoCross` | `threshold()` found no crossing inside the box |
| `Full` | `max_trials` are recorded |
| `Busy` | the Async queue is full, or the GP is owned by a running Async, or another thread is inside a call on it |
| `Timeout` | `Async.wait()` ran out of time |

Every library error carries `.code`, the `ERR_*` value. Type errors in the
arguments raise `TypeError` or `ValueError`.

Constants mirror the header: `LIK_*`, `KERNEL_*`, `LINK_*`, `ACQ_*`,
`STOP_*`, `OK`, `ERR_*`, `POLICY_*`, `MAX_DIMS`, `MAX_OUTCOMES`,
`MAX_TRIALS`, `QUAD_N`, `ASYNC_QUEUE`, `MODEL_*`, `DIM_*`.

## Explanation

### The GIL and the threads

A GP handle is not thread-safe. These calls release the GIL while the C code
runs: `next`, `next_subset`, `update`, `update_real`, `fit`, `fit_step`,
`refit`, `predict_p_many` and `predict_f_many`. Other Python threads keep
running during them. A flag on the object, read and written only while the
GIL is held, makes a second call on the same GP from another thread raise
`Busy` instead of corrupting the handle. The one-point predictions keep the
GIL, because they take microseconds.

When `rng` is set, `next()` and `next_subset()` keep the GIL: the library
calls the generator from inside the selection, and the generator is Python
code. If the generator raises, `next()` raises the same exception.

The Async thread is a C thread that never touches the interpreter. It calls
`yaep_update`, `yaep_next` and `yaep_threshold` and publishes a plain C
struct under a lock. The Python side copies that struct into a `Snapshot`
when you poll. For this reason, `Async` refuses a GP whose `rng` is a Python
callable. While an `Async` runs, it owns the GP, and every call on the GP
raises `Busy`, including the predictions: a prediction writes the handle's
scratch space. Read the snapshot instead, or stop first.

The same responses give the same posterior bit for bit through `Async` as
through the plain loop, with `fit_in_idle` off. With `fit_in_idle` on, the
hyperparameter fit runs in the gaps between responses, so its schedule
depends on timing, and a replay needs the plain loop.

### What to log with a threshold

A threshold number alone does not tell you whether to trust it. Log these
with it:

- `lo` and `hi`. The band is the crossing of the mu +- 1.96 sd latent
  curves: it is wide when the fit is loose. It is a credible band from a
  Laplace posterior, which underestimates the variance early and at the edges
  of the box.
- `threshold_multi_cross`. When the posterior mean crosses the target more
  than once along the intensity axis, the value is the lowest crossing.
- `hyper()`. A lengthscale or an output scale at its bound is a warning. A
  lengthscale at its upper bound on a dimension that does not matter is
  normal.
- `log_marginal`. A value much closer to 0 than the responses allow means the
  fit interpolated its own trials. One hundred binary trials at a 75% level
  cannot honestly do better than about -56 nats.
- `history()`, which has the stimuli that you showed, and whether each one
  was the proposal.
- For an Async session, `policy`, and how many frames each proposal was late.

The header's manual, [ysp/aep.h](../../../include/ysp/aep.h), explains the model, the
kernels, the priors, the acquisitions and their costs.
