# ysp.quest Python binding

A CPython extension for the [ysp/quest.h](../../../include/ysp/quest.h)
single-header library: QUEST+ (Watson 2017) on a grid, which includes QUEST,
Psi and Psi-marginal as configurations. It also wraps the header's `YQST_ASYNC`
layer, a background C thread that runs the inference between trials. It uses
the plain CPython C API and has no dependencies; NumPy is optional. It is
built against the **Limited API / stable ABI** (`Py_LIMITED_API =
0x030A0000`), so one `ysp/quest.abi3.so` works on CPython 3.10 and later.

The distribution is `ysp-quest` and the module is `ysp.quest`. `ysp` is a
[PEP 420](https://peps.python.org/pep-0420/) implicit namespace package (no
`__init__.py`), so `ysp-quest` installs alone or next to the other `ysp-*`
distributions.

## How to install

From this directory:

```sh
uv pip install .                    # build and install an abi3 wheel
# or, in place for development:
python setup.py build_ext --inplace
```

The extension defines `YQST_ASYNC`, so it compiles ysp/quest.h together with
the [ysp/rt.h](../../../include/ysp/rt.h) that it includes. In a development tree
setup.py finds both headers in the repository's `include/` directory. An
isolated build (an sdist or a cibuildwheel run) sees only this directory. Copy
both headers into `include/ysp/` next to `setup.py` first:

```sh
mkdir -p include/ysp && cp ../../../include/ysp/quest.h ../../../include/ysp/rt.h include/ysp/
uv build --sdist
```

The copies are gitignored. The repository root wins when it is present, so a
stale copy cannot shadow a newer header. On Linux and macOS the build links
`-pthread` for the async thread. Windows needs no extra library.

## How to run a Psi experiment

```python
import ysp.quest as yqst

q = yqst.Quest(
    stim=[(-3.0, 0.0, 31)],                    # log10 contrast, 31 points
    params=[(-3.0, 0.0, 61),                   # threshold
            (0.5, 6.0, 12),                    # slope
            0.5,                               # guess: 2AFC, fixed
            0.02],                             # lapse, fixed
    stop_trials=60)

while not q.done:
    i = q.next()                               # stimulus grid index
    correct = run_trial(10 ** q.stim_value(i))
    q.update(i, correct)

print(q.estimate(yqst.EST_MEAN), q.sd(0))
```

An axis is one of these:

- a number: a fixed parameter (`yqst_fixed`).
- a `(lo, hi, n)` tuple: a linspace. The third item must be an `int`. A tuple
  is always read this way; use a list for explicit values.
- a list, a tuple of another length, or a float buffer such as a NumPy array:
  explicit values.
- a dict from `linspace(lo, hi, n, prior=..., nuisance=...)`,
  `values(seq, prior=..., nuisance=...)` or `fixed(value)`. Use this form for
  a prior or a nuisance flag.

For Psi-marginal, give the guess and lapse axes a few points and flag them:

```python
params = [(-3.0, 0.0, 61), (0.5, 6.0, 12),
          yqst.values([0.45, 0.475, 0.5, 0.525, 0.55], nuisance=True),
          yqst.values([0.0, 0.015, 0.03, 0.045, 0.06], nuisance=True)]
```

For a QUEST prior on the threshold:

```python
thr = yqst.linspace(-3.0, 0.0, 61)
params = [yqst.linspace(-3.0, 0.0, 61, prior=yqst.prior_normal(thr, -1.5, 0.5)),
          3.5, 0.5, 0.01]
q = yqst.Quest([(-3.0, 0.0, 31)], params, select=yqst.SELECT_QUANTILE, stop_trials=40)
```

## How to use a custom model

Use `pf_batch`. The header calls it once per stimulus when it builds the
table, and once per off-grid evaluation:

```python
import numpy as np

def weibull_batch(stim, params):
    P = np.asarray(params)                  # (P, n_param) float64; no copy
    x = stim[0]
    p1 = P[:, 2] + (1 - P[:, 2] - P[:, 3]) * (1 - np.exp(-10.0 ** (P[:, 1] * (x - P[:, 0]))))
    return np.stack([1 - p1, p1], axis=1)   # P*K values, row i outcome k at i*K + k

q = yqst.Quest([(-3.0, 0.0, 31)], [(-3.0, 0.0, 61), (0.5, 6.0, 12), 0.5, 0.02],
             pf_batch=weibull_batch, stop_trials=60)
```

- `stim` is a tuple of `n_stim` floats. The header always calls with one
  stimulus (`S = 1`).
- `params` is a read-only memoryview of format `'d'` and shape `(P, n_param)`.
  Its rows are the header's walk of the parameter grid. That walk is permuted
  when a nuisance axis is not last, so treat the rows as a list of points and
  answer row for row. The binding makes the view once at open and reuses it.
- Return `P * K` probabilities as any float32 (`'f'`) or float64 (`'d'`)
  buffer, or as a sequence of numbers. Each row must sum to 1 within 1e-6.
  The binding converts to the header's float table.

`pf_fn=callable(stim, params) -> K floats` is the per-cell form. It is
called `S * P` times at open, a Python call for every table cell, which is
seconds for a Psi-marginal grid. Use it only for small grids or to check a
`pf_batch`. A callable implies `pf=PF_CUSTOM`, and `n_outcomes` defaults to 2.

`no_table=True` with a Python model calls the model on every `next()`, which
moves the whole table across the language boundary every trial. Do not do
that.

An exception in a callback is raised from the call that ran it. The header
cannot stop a callback loop early, so the remaining callbacks in that call
return a neutral value and the handle stays usable.

## How to run the inference on a thread

```python
q = yqst.Quest([(-3.0, 0.0, 31)], params, stop_trials=60)

with yqst.Async(q) as a:                    # start(); stop() on exit
    seq, snap = a.poll()                  # seq 0: the first proposal
    while not snap.done:
        k = run_trial(snap.stim[0])       # your frames
        seq = a.submit(snap.proposed, k)
        while a.poll()[0] < seq:          # the inter-trial interval
            draw_frame()
        _, snap = a.poll()
print(q.estimate())                        # the Quest is yours again
```

`poll()` is cheap enough for every frame. `wait(seq, timeout_s)` is the
blocking form for the end of an interval. `submit` raises `Busy` when the
queue (`ASYNC_QUEUE` responses) is full: nothing was queued, so retry on the
next frame. See [example.py](example.py).

## How to save and resume a session

```python
import random

gen = random.Random(seed)
q = yqst.Quest(stim, params, rng=gen.random, subset_size=8, stop_trials=100)
...
snap, gen_state = q.save(), gen.getstate()     # write both to disk
...
gen = random.Random()
gen.setstate(gen_state)
q = yqst.Quest.load(snap, stim, params, rng=gen.random, subset_size=8, stop_trials=100)
```

The resumed session makes the same proposals, and has the same posterior
bytes and estimates, as the session that was not interrupted. That is also
true when a proposal was pending at the save. `load` takes the same desc as
the saved session. It must match on every number except the priors (the
saved posterior replaces them). It also re-supplies what a snapshot cannot
carry: the model callable and the `rng`. A mismatch raises `Error` with the
header's message, which names the first field that differs. The generator is
yours: save its state beside the snapshot and restore it before the next
`next()`. With an `Async`, save after `stop()`.

## Reference

### `Quest(stim, params, **desc)`

`stim` and `params` are lists of axes (see above). The other arguments are
keyword-only `yqst_desc` fields; a zero means the library default.

| Argument | Meaning |
|---|---|
| `pf` | `PF_GUMBEL` (default), `PF_WEIBULL`, `PF_LOGISTIC`, `PF_NORMAL`, `PF_HYPSEC`, `PF_CUSTOM` |
| `pf_fn`, `pf_batch` | a Python model; set one at most |
| `n_outcomes` | K for a custom model; 0 means 2 |
| `joint_prior` | P weights in C order, for a prior that does not factor |
| `select`, `select_param`, `select_quantile` | `SELECT_ENTROPY` (default), `SELECT_QUANTILE`, `SELECT_MEAN`, `SELECT_MODE` |
| `tiebreak`, `tie_tolerance` | `TIE_LOWEST` (default), `TIE_NEAREST`, `TIE_ALTERNATE`, `TIE_RANDOM` |
| `rng` | a callable that returns a float in [0, 1); needed for `TIE_RANDOM` and `subset_size` |
| `subset_size` | score this many random stimuli per `next()` |
| `stop_trials`, `stop_entropy`, `stop_sd`, `stop_sd_param` | stop criteria; one is required |
| `no_table` | evaluate the model per trial instead of storing the table |

`Quest.memory_size(stim, params, **desc)` is a static method. It returns the
arena bytes for that desc, or 0 for a desc the constructor would refuse, and
calls no callback.

| Method or property | Returns |
|---|---|
| `next()`, `next_subset(indices)` | a stimulus grid index |
| `expected_entropy(i)` | expected posterior entropy in bits after stimulus `i` |
| `update(i, k)`, `update_values(stim, k)` | None; record outcome `k` for a grid index or a value |
| `done`, `stop_reason` | a bool; a `Stop` member |
| `estimate(how=EST_MEAN)` | a list, one value per parameter axis |
| `quantile(axis, p)`, `sd(axis)` | a float |
| `marginal(axis)` | a memoryview `'d'` (a copy) |
| `posterior(shaped=False)` | a memoryview `'d'` of P values in C order (a copy); `shaped=True` casts it to `param_shape` |
| `entropy()` | posterior entropy in bits, over the non-nuisance axes |
| `p(i, params)`, `p_values(stim, params)` | a tuple of K probabilities |
| `simulate(i, params, u)` | an outcome drawn with your `u` in [0, 1) |
| `stim_value(i, axis=0)`, `stim_values(i)`, `stim_index(sub)`, `stim_nearest(values)` | grid conversions |
| `param_value(t, axis=0)`, `param_values(t)`, `param_index(sub)` | grid conversions |
| `n_stim`, `n_param`, `n_outcomes` | S, P, K (joint grid sizes, as in C) |
| `stim_shape`, `param_shape` | points per axis |
| `n_trials`, `history()` | a list of `Trial(stim, stim_index, proposed_index, outcome)` named tuples |
| `releases_gil` | a dict: which calls release the GIL for this configuration |
| `save()` | a snapshot as bytes: the posterior, the history, the pending proposal, the tie and stop state. The table is not in it. |
| `Quest.load(data, stim, params, **desc)` | a classmethod: a new Quest resumed from `save()`. The table is rebuilt, so this costs one construction. |
| `close()`, `is_open` | free the arena; `Quest` is a context manager |

`__version__` is `yqst_version()` from the compiled header, and the tests
check that `pyproject.toml` carries the same string.

`posterior()` and `marginal()` return a copy in a read-only memoryview over
a bytes object. `numpy.frombuffer(q.posterior())` and `numpy.asarray(...)`
view that copy without a second copy. The Limited API before 3.11 has no
buffer slot for a type, so a zero-copy view of the live posterior is a later
item.

### `Async(quest, estimator=EST_MEAN, below_normal=False, pin_cpu=0, queue_depth=0)`

`queue_depth` is how many responses may wait for the thread: 0 means
`ASYNC_QUEUE`, and 1 keeps the trial loop in lockstep with the inference.
When the queue is at that depth, `submit` raises `Busy`.

| Method or property | Returns |
|---|---|
| `start()`, `stop()` | take over the Quest and start the thread; drain the queue, join, hand the Quest back. `Async` is a context manager. |
| `submit(i, k)`, `submit_values(stim, k)` | the seq of the queued response |
| `poll()` | `(seq, Snapshot)`: the newest snapshot and the seq it accounts for |
| `wait(seq, timeout_s=None)` | `(seq, Snapshot)` after the thread finishes `seq`; `Timeout` if the time runs out |
| `pending` | responses queued and not yet applied |
| `policy` | `ASYNC_NORMAL` or `ASYNC_BELOW_NORMAL` while running, `ASYNC_NONE` otherwise |
| `running`, `quest` | state |

`Snapshot` is a named tuple: `seq, proposed, stim, update_rc, n_trials, done,
stop, estimate, entropy, sd`. `estimate` follows the `estimator` argument and
`sd` is for parameter axis 0. A snapshot stays readable after `stop()`.

### Constants and errors

The enums are standard-library `enum.IntEnum` classes: `PF`, `Select`,
`Estimator`, `Tiebreak`, `Stop`. Each member is also a module constant with
the header's prefix (`PF_GUMBEL`, `SELECT_QUANTILE`, `EST_MODE`,
`TIE_RANDOM`, `STOP_SD`). Other constants: `ERR_*`, `MAX_STIM_DIMS` (4),
`MAX_PARAMS` (8), `MAX_OUTCOMES` (8), `MAX_TRIALS` (2048), `ASYNC_QUEUE` (8),
`ASYNC_NONE`, `ASYNC_NORMAL`, `ASYNC_BELOW_NORMAL`.

| Exception | Raised when |
|---|---|
| `Error` | base class; also a Quest that an Async owns, or a Quest already in a call |
| `ArgumentError` | an invalid desc, an index or outcome out of range, an outcome impossible under every parameter point. Also a `ValueError`. |
| `Closed` | the Quest is closed, or the Async is not running |
| `Full` | `MAX_TRIALS` trials are recorded |
| `OutOfMemory` | the arena allocation failed. Also a `MemoryError`. |
| `Busy` | the Async queue is full |
| `Timeout` | `wait()` ran out of time. Also a `TimeoutError`. |

## Explanation: the GIL, callbacks and the thread

A call releases the GIL only when no Python callback can run inside it. The
header cannot give a callback the GIL back in the middle of a table sweep, so
the binding decides per call:

| Call | Releases the GIL unless |
|---|---|
| construction (the table build) | the model is a Python callable |
| `next()`, `next_subset()` | there is an `rng`, or a Python model under `no_table` |
| `update()` | a Python model under `no_table` |
| `update_values()` | the model is a Python callable |
| everything else | never releases: estimates, grids and `expected_entropy()` are cheap, and `p()`, `p_values()`, `simulate()` can call a Python model |

`releases_gil` shows the answer for one Quest. While one thread is inside a
call with the GIL released, a second thread that calls the same Quest gets
`Error`, not a data race: a Quest handle is not thread-safe, and the binding
does not pretend that it is. The same flag refuses a callback that calls back
into the Quest it runs under.

The `Async` thread runs `yqst_update` and `yqst_next` in C and never touches
the interpreter, so it needs no GIL and does not slow Python threads. For
that reason `Async.start()` refuses a Quest with an `rng` or with a Python
model under `no_table`, and `submit_values()` refuses a Python model: in each
case the thread would have to call Python. A Python `pf_batch` with a table
is allowed, because the table is built before the thread starts. From
`start()` to `stop()` the Async owns the Quest, and every call on the Quest
raises `Error`; read the snapshot instead. `wait()` releases the GIL while it
blocks, in one call to the header. ysp/rt.h 0.3.1 makes that wait safe
against a `stop()` on another thread at any point. A wait that races a
draining stop waits for the drain and returns for a seq that was already
queued; a wait on a seq the thread never reaches (never submitted, or dropped)
raises `Closed`. Ctrl-C is handled only when the wait returns, so give a
timeout to a wait you may need to interrupt.

`below_normal=True` asks the OS for a lower priority. On a machine where
other processes use every core, a below-normal thread can wait seconds for a
turn. Log `policy` with your timing data.

## Verification

[tests/compare/compare_quest_questplus.py](../../../tests/compare/compare_quest_questplus.py)
runs this binding and the `questplus` package on the same Psi and
Psi-marginal grids with one simulated observer, and reports per-trial
stimulus agreement and the largest posterior difference: 180 of 180
selections are identical, and the largest posterior difference is 6.8e-8,
which is the float likelihood table.
[tests/compare/methods_compare.py](../../../tests/compare/methods_compare.py)
`--replay` replays the 17 runs saved in Watson's QUEST+ notebook through
this binding: 1452 of 1504 selections identical, 52 ties within 2e-8 bits,
and no difference. The comparisons with mQUESTPlus and Palamedes run
through the MEX binding. The numbers, the cost model and the frame budget
are in the manual at the top of [ysp/quest.h](../../../include/ysp/quest.h).
