# ysp.stair Python binding

A CPython extension for the [ysp/stair.h](../../../include/ysp/stair.h)
single-header library: transformed and weighted up/down staircases and
accelerated stochastic approximation. It uses the plain CPython C API and has
no dependencies. It is built against the **Limited API / stable ABI**
(`Py_LIMITED_API = 0x030A0000`), so one `ysp/stair.abi3.so` works on CPython
3.10 and later.

The distribution is `ysp-stair` and the module is `ysp.stair`. `ysp` is a
[PEP 420](https://peps.python.org/pep-0420/) implicit namespace package (no
`__init__.py`), so `ysp-stair` installs alone or next to the other `ysp-*`
distributions.

## How to install

From this directory:

```sh
uv pip install .                    # build and install an abi3 wheel
# or, in place for development:
python setup.py build_ext --inplace
```

The in-place build puts the extension in `ysp/`. That directory is otherwise
empty and is kept in git for this purpose.

In a development tree, setup.py finds `ysp/stair.h` in the repository's
`include/` directory.
An isolated build (an sdist or a cibuildwheel run) sees only this directory.
Copy the header into `include/ysp/` next to `setup.py` first:

```sh
mkdir -p include/ysp && cp ../../../include/ysp/stair.h include/ysp/
uv build --sdist
```

The copy is gitignored. The repository root wins when it is present, so a
stale copy cannot shadow a newer header. ysp/stair.h does not include
ysp/rt.h, so no other header is necessary.

## How to run a staircase

```python
import ysp.stair as yst

s = yst.Staircase(start=0.5, n_down=3, step_type=yst.STEP_LOG,
                 steps=[0.3, 0.15, 0.075], min=0.001, max=1.0,
                 stop_reversals=10)

while not s.done:
    level = s.next()                 # what the rule proposes
    shown = display.quantize(level)  # what you can show
    s.update(shown, run_trial(shown))

print(s.estimate(yst.EST_REVERSALS), s.estimate_count(yst.EST_REVERSALS))
```

Give `update()` the level that you showed. The rule steps from its own
proposal, and the history keeps both, so an analysis can see the
quantization.

To simulate an observer, draw `u` from your own generator:

```python
import random
rng = random.Random(1)
r = yst.simulate_response(p_correct, rng.random())
```

See [example.py](example.py) for a complete simulated run.

## Reference

### `Staircase(**desc)`

All arguments are keyword-only and are the `yst_desc` fields. A zero means
the library default, as in C.

| Argument | Meaning |
|---|---|
| `start` | first proposed level, in your units |
| `rule` | `RULE_UPDOWN` (default) or `RULE_ASA` |
| `n_up`, `n_down` | incorrect / correct responses per step; 0 means 1 |
| `step_type` | `STEP_LIN` (default), `STEP_LOG` or `STEP_DB` |
| `steps` | one number, or a sequence of up to `MAX_STEPS` numbers (the schedule; one entry per reversal) |
| `step_down_scale` | harder step = step times this; 0 means 1; not 1 makes a weighted staircase |
| `target_p` | `RULE_ASA` only: the proportion to converge on |
| `min`, `max`, `use_limits` | proposal clamp; both 0 means no limits unless `use_limits` |
| `harder_is_up` | a correct response raises the level |
| `initial_rule` | 1-up-1-down until the first reversal |
| `stop_reversals`, `stop_trials`, `stop_at_limit` | stop criteria; 0 disables; one of the first two is required |
| `est_reversals`, `est_trials` | estimate windows; 0 means everything after the schedule ran out |

An invalid desc raises `ArgumentError` with the library's message.

| Method or property | Returns |
|---|---|
| `next()` | the proposed level (float). Two calls without an update return the same value. |
| `update(level, response)` | an `Event` mask: `STEP`, `REVERSAL`, `DONE`, or 0 |
| `done` | True after a stop criterion fired; later updates are still recorded |
| `stop_reason` | a `Stop` member (`NONE`, `REVERSALS`, `TRIALS`, `LIMIT`, `FULL`) |
| `estimate(how=EST_REVERSALS)` | the threshold estimate; NaN when the window is empty |
| `estimate_count(how=EST_REVERSALS)` | the number of values the estimate averages |
| `n_trials`, `n_reversals`, `is_open` | counters and state |
| `history()` | a list of `Trial` named tuples: `(proposed, shown, response, reversal, direction, step_index)` |
| `reversal_level(i)`, `reversal_trial(i)` | reversal `i`; `IndexError` out of range |
| `reversals()` | a list of `(trial, level)` tuples |
| `save()` | the snapshot as `bytes` (`yst_save()`) |
| `Staircase.load(data, **desc)` | a new `Staircase` resumed from `save()`; the keywords are the constructor's and must open the same staircase. A mismatch or a bad snapshot raises `ArgumentError` with the library's message, which names the first field that differs. |

A resumed staircase makes the same proposals and has the same history and
estimates as one that was not interrupted, with no replay of the updates.

### Module functions

| Function | Returns |
|---|---|
| `weighted_scale(p)` | `(1 - p) / p`, the `step_down_scale` for a weighted 1-up-1-down at `p` |
| `convergence_p(n_up, n_down, scale=0.0)` | the proportion correct the rule converges on; NaN when both counts exceed 1 |
| `simulate_response(p, u)` | 1 when `u < p`, else 0 |
| `strerror(code)` | the library's text for an `ERR_*` code |

### Constants and enums

The enums are `enum.IntEnum` classes (`enum.IntFlag` for `Event`) from the
standard library: `Rule`, `StepType`, `Estimator`, `Stop`, `Event`. Each
member is also a module constant with the header's prefix: `RULE_UPDOWN`,
`STEP_LOG`, `EST_MEDIAN_REV`, `STOP_LIMIT`, `EVENT_REVERSAL`, and so on. The
other constants are `ERR_ARG`, `ERR_CLOSED`, `ERR_FULL`, `MAX_TRIALS` (1024),
`MAX_REVERSALS` (256) and `MAX_STEPS` (16).

`__version__` is `yst_version()` from the compiled header, and the tests
check that `pyproject.toml` carries the same string.

### Errors

| Exception | Raised when |
|---|---|
| `Error` | base class |
| `ArgumentError` | an invalid desc, a response that is not 0 or 1, a non-finite level, a level at or below 0 under `STEP_LOG` / `STEP_DB`. Also a `ValueError`. |
| `Closed` | the handle was never opened (its construction failed) |
| `Full` | `MAX_TRIALS` trials are recorded |

## Threads and the GIL

No call releases the GIL. Each call is a few arithmetic operations on a
handle inside the Python object, so a release would cost more than the call.
The GIL also keeps two Python threads from changing one handle at the same
time. To interleave staircases, make several `Staircase` objects.

The semantics of the rule (which step the reversal trial takes, what a
clamped step counts as, which reversals the default estimate uses) are in the
manual at the top of [ysp/stair.h](../../../include/ysp/stair.h).
[tests/compare/compare_stair_psychopy.py](../../../tests/compare/compare_stair_psychopy.py)
replays fixed response sequences through this binding and PsychoPy's
`StairHandler` and reports where they differ. On its seven configurations
no proposal differs on any trial, the reversals are the same, and the
estimates agree, the log and dB cases to 1e-15. The comparison with
Palamedes' `PAL_AMUD` runs through the MEX binding.
