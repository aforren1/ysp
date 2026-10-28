# ysp.dataset Python binding

A CPython extension for the [ysp/dataset.h](../../../include/ysp/dataset.h)
single-header library: a session's data set, the folder that the ysp player
writes (`session.json`, `trials.csv`, `frames.csv`, `events.csv`,
`clock.csv` and `streams/<name>.csv`), and a zip of it for the web. Use it
from a PsychoPy script, or from any Python experiment, to write the session
in the ysp data set format, so that the same analysis reads it. It uses the
plain CPython C API and has no dependencies. It is built against the
**Limited API / stable ABI** (`Py_LIMITED_API = 0x030A0000`), so one
`ysp/dataset.abi3.so` works on CPython 3.10 and later.

The distribution is `ysp-dataset` and the module is `ysp.dataset`. `ysp` is
a [PEP 420](https://peps.python.org/pep-0420/) implicit namespace package
(no `__init__.py`), so `ysp-dataset` installs alone or next to the other
`ysp-*` distributions.

The header's manual (THE FOLDER, SESSION.JSON, THE TABLES, THE ZIP) and
[docs/dataset.md](../../../docs/dataset.md) are the reference for the
files. This page tells you how to write them from Python.

## How to install

From this directory:

```sh
uv pip install .                    # build and install an abi3 wheel
# or, in place for development:
python setup.py build_ext --inplace
```

In a development tree, setup.py finds `ysp/dataset.h` in the repository's
`include/` directory. An isolated build (an sdist or a cibuildwheel run)
sees only this directory. Copy the header and the three headers that it
includes into `include/ysp/` next to `setup.py` first:

```sh
mkdir -p include/ysp
cp ../../../include/ysp/{dataset,trials,table,json}.h include/ysp/
uv build --sdist
```

## How to write a session from PsychoPy

```python
import ysp.dataset as yds

cols = [("participant", "const", "P07"),     # the same text on every row
        ("trial_index", "int"),
        ("contrast", "double"),
        ("response", "string"),
        ("correct", "bool"),
        ("rt_s", "time")]                     # seconds; the name ends in _s
ds = yds.Dataset("data/P07_20261021T140312Z", cols,
                 session={"experiment": "gabor-2afc", "seed": 20261021,
                          "fields": {"participant": "P07", "age": 31}})
frame = 0
for trial, contrast in enumerate(contrasts):
    for k in range(n_frames):
        grating.draw()
        t = win.flip()                        # the flip's time stamp, in seconds
        if k == 0:
            t_onset = t
        ds.write_frame(frame=frame, trial_index=trial, onset_s=t)
        frame += 1
    key, t_key = wait_key()                   # yours: the key and its time, on the same clock
    ds.write_event(t_s=t_key, trial_index=trial, kind="keyboard", text=key)
    ds.write_trial(trial_index=trial, contrast=contrast, response=key,
                   correct=key == answer, rt_s=t_key - t_onset)
ds.finish()
```

1. The folder must not exist; its parent must. The open makes it and
   writes `session.json` with `"closed": false`.
2. A row is a dict, or keywords, of column names and values. A column
   that the row does not name is an empty cell. `None` is an empty cell
   in every column, and `NaN` in a number or time column.
3. `finish()` closes every table and writes `session.json` again with
   `"closed": true` and the row counts. `close()` and the normal end of a
   `with` block also finish a data set that is open. A session that fails
   does not finish: see the next section.
4. Times are seconds as floats, on one clock of yours (the clock of
   `win.flip()`'s time stamps, for example). The binding rounds each
   time to the nearest nanosecond, and the file holds it as seconds with
   9 decimals. A time may be negative (a residual, an SOA).

`example.py` is a complete simulated session.

## How to end a session that failed

`"closed": true` means that the session ended normally. When it does not,
the data set is abandoned: every table file closes, every row written stays,
and `session.json` keeps `"closed": false` and no row counts, as after a
crash. This happens when:

- an exception leaves a `with` block (the exception goes on);
- you call `abandon()`, for example in your own `except` block;
- a `Dataset` that was never finished or closed is deleted (at the end of
  the script, or by the garbage collector). Python then also emits a
  `ResourceWarning`, as for a file that was never closed.

```python
with yds.Dataset(folder, cols, session=info) as ds:
    run_experiment(ds)          # an exception here: "closed": false
                                # the normal end of the block: "closed": true
```

After `abandon()` the writes, `finish()` and `zip()` refuse the data set;
`close()` and a second `abandon()` do nothing.

## How to add trial rows from ysp.trials

`trials.csv` has no part that the sequencer writes itself (the header
opens it with `desc.trials` NULL; a tree has several `ytr_trials`), and
the two extensions share no C state. Write the sequencer's columns as your
own. They then equal `ysp.trials`' own data line (`format_row()`), text for
text; the tests check this:

```python
import ysp.trials as ytr

t = ytr.Trials(n_conditions=4, reps=10, order=ytr.ORDER_FULL_RANDOM, rng=1, seed=7,
               vars=[ytr.var_choice("side", ["left", "right"])])
cols = [("index", "int"), ("block", "int"), ("condition", "int"), ("outcome", "int"),
        ("side", "string"), ("trial_seed", "string"), ("rt_s", "time")]
ds = yds.Dataset(folder, cols)
while (ti := t.next()) is not None:
    rt, correct = run_trial(ti)
    t.update(int(correct))
    h = t.history()[ti.index]
    ds.write_trial(index=h.index, block=h.block, condition=h.condition, outcome=h.outcome,
                   side=t.var(ti.index, "side"),
                   trial_seed=yds.seed_text(t.trial_seed(ti.index)), rt_s=rt)
```

`seed_text()` writes a 64-bit seed as `x` and 16 hex digits, as the trials
writer does, so pandas and R read all 64 bits as text. A jitter's
`Jitter.s` goes into a `time` column exactly: the binding gives back the
same nanosecond.

## How to add a stream

```python
dots = ds.stream("dots", [("t_s", "time"), ("coherence", "double", 3)])
ds.write_stream(dots, t_s=t, coherence=0.5)       # or ds.write_stream("dots", ...)
```

A stream's name follows the column rule (`[A-Za-z_][A-Za-z0-9_]*`, at most
63 bytes) and differs from every other stream's in more than case. A data
set has at most 16 streams (`MAX_STREAMS`).

## How to log a live change

```python
ds.session_push("changes", {"trial_index": 40, "key": "contrast_step", "value": 0.5})
ds.session_set("calibration", {"file": "rig.yspcal", "hash": h})
```

Each call rewrites `session.json` atomically, which costs milliseconds (a
sync to the disk and a rename). Call it between trials, never in a trial's
frames. A value is `None`, `bool`, `int`, `float`, `str`, `list`, `tuple`
or `dict` with `str` keys; NumPy scalars also work. JSON has no NaN.

## How to make the zip for the web

```python
ds.finish()
blob = ds.zip()                  # bytes: post it or save it
ds.zip("P07.zip")                # or to a file
```

The zip is stored (no compression), with the folder's name as its top
folder, and its bytes depend on the files only.

## How to read a data set

Use pandas (`pd.read_csv("trials.csv")`) or the `csv` module. The binding
has no reader: the header has none. Check `session.json` first.
`"closed": false` means that the session did not end normally; the rows in
the files are the rows written before the end.

## Reference

### `Dataset(dir, trial_cols, *, session=None, sync=False)`

| Argument | Meaning |
|---|---|
| `dir` | the folder: a `str` or path, UTF-8 on every OS; it must not exist |
| `trial_cols` | `trials.csv`'s columns: a list of `(name, type)`, `(name, type, decimals)` or `(name, "const", text)` |
| `session` | a `dict` of your keys for `session.json`; not `format`, `version`, `closed`, `rows` or `write_errors`, which the writer sets. `start_utc` is the writer's unless you give it |
| `sync` | `True`: every row of every table also waits for the disk (`YTR_FLUSH_SYNC`, milliseconds a row). `False` (the default): each row reaches the OS when the call returns, so a crash keeps it |

| Column type | Python value | Missing |
|---|---|---|
| `"int"`, `"int64"` | an `int` (or a float with no fraction) | `None`, `NaN` |
| `"double"` (`"float"`) | a number; `decimals` 0 (the shortest exact) or 1..17 | `None`, `NaN` |
| `"bool"` | `True`, `False`, 0 or 1 | `None`, `NaN` |
| `"string"` (`"str"`) | a `str`, or UTF-8 `bytes` | `None`, `""` |
| `"time"` | seconds; `decimals` 0 (9, exact) or 1..9; the name ends in `_s` | `None`, `NaN` |
| `"const"` | none: the text is the third item | |

| Method or property | Does |
|---|---|
| `write_trial(row=None, **values)` | one row of `trials.csv` |
| `write_frame(row=None, **values)` | one row of `frames.csv`; the names are `FRAME_COLUMNS` |
| `write_event(row=None, **values)` | one row of `events.csv`; the names are `EVENT_COLUMNS` |
| `write_clock(row=None, **values)` | one row of `clock.csv`; the names are `CLOCK_COLUMNS` |
| `stream(name, cols)` | makes `streams/<name>.csv`; returns its id |
| `write_stream(stream, row=None, **values)` | one row of a stream, by id or name |
| `session_set(key, value)`, `session_push(key, value)` | `session.json`'s key, or an item appended to its list; one atomic rewrite each |
| `rows()` | `{"trials": n, "frames": n, "events": n, "clock": n, "streams": {name: n}}` |
| `finish()` | finishes the data set and returns `rows()` |
| `close()` | finishes a data set that is open; safe twice |
| `abandon()` | an abnormal end: closes the files, leaves `session.json` `"closed": false`; safe twice |
| `zip(path=None)` | the finished folder as one zip: `bytes`, or written to `path` |
| `dir`, `is_open`, `finished`, `streams`, `trial_columns` | state |

The columns of the fixed tables (the header's THE TABLES):

- `FRAME_COLUMNS`: `frame, trial_index, target_s, planned_s, onset_s,
  residual_s, dropped, path, tier, flags, code_risk, evaluate_s, script_s,
  draw_s, upload_s, swap_s, gpu_s`.
- `EVENT_COLUMNS`: `t_s, trial_index, kind, role, stimulus, frame,
  planned_s, residual_s, code, control, type, value, x, y, deadline_s,
  end_s, tier, flags, ticks, text`. `kind` says what the row is: a
  timeline landing (`"onset"`, `"mark"`, ...), an input (`"keyboard"`,
  `"mouse"`, `"box"`, ...) or the script's (`"response"`, `"out"`).
- `CLOCK_COLUMNS`: `role, kind, t_s, ticks, width_s, ns_per_tick, ppm,
  spread_s, span_s, points, epoch`. `kind` is `"pair"` or `"fit"`.

Module: `seed_text(seed)`, `version()`, `__version__`, `FORMAT`
(`"ysp-session"`), `FORMAT_VERSION` (1), `MAX_STREAMS` (16), `MAX_COLUMNS`
(256).

Errors: `Error`, and its subclasses `ArgumentError` (also `ValueError`),
`ExistsError` (also `FileExistsError`: the folder is there), `WriteError`
(also `OSError`: a file could not be made, written or renamed),
`ClosedError`, `OrderError` (`zip()` before the finish), `FullError` (16
streams), `RangeError` (a zip of 4 GiB or more). A refused value writes
nothing: the binding checks every value of a row before it sets one.

## Costs and threads

A row costs one write call, about 12 us in C (docs/dataset.md has the
numbers), and the binding allocates nothing per row once its text buffer
has grown to the longest row. The interpreter still makes the call's
keyword `dict`. `sync=True` and `session_set()` cost milliseconds: keep
them out of a trial's frames.

The calls that sync the disk (the open, `session_set()`,
`session_push()`, `finish()`, `close()`, `abandon()`, `zip()`) release the
GIL. Row
writes keep it: a row takes microseconds, and getting the GIL back after a
release can wait for another thread's switch interval. One thread writes
one data set; a second call while the GIL is released raises `Error`.

## What is not bound

| Item | Why |
|---|---|
| The adapters (`yds_frame_from_screen()`, `yds_event_from_input()`, `yds_event_from_timeline()`, `yds_clock_from_fit()`) | Their input structs come from ysp/screen.h, input.h, timeline.h and rt.h, which no Python binding has. The keywords of `write_frame()`, `write_event()` and `write_clock()` are their fields |
| `yds_trials_writer()`, `yds_stream_writer()` | The binding uses them for every row; a row is a `dict` |
| Bound (struct field) columns, `CHARS` columns | Python has no row struct; every column is set per row, which also gives a missing `bool` |
| A reader | The header has none; pandas, R and the `csv` module read every file |
| `yds_session()` | Read `session.json`: it is the same value, rewritten at each change |
