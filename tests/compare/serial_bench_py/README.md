# Serial bench in Python: pyserial, Psychtoolbox IOPort and ysp.serial

This folder is the Python counterpart of [../serial_bench/](../serial_bench/)
(MATLAB). It compares three ways to use a serial port from Python:

- `pyserial`: the common pure-Python package (PyPI `pyserial`, release 3.5).
- `ioport` and `ioport_tuned`: the `IOPort` of the Psychtoolbox wheel (PyPI
  `psychtoolbox`, release 3.0.22.2), with its defaults and with the latency
  options of the MATLAB audit.
- `ysp`: `ysp.serial`, the binding of [ysp/serial.h](../../../include/ysp/serial.h)
  in [bindings/python/ysp_serial/](../../../bindings/python/ysp_serial/),
  release 0.5.0.2 or later (it needs the async reader).

The parts, the parameters, the metrics and the board-stamp fit are those of
the MATLAB bench, so the numbers compare. The MATLAB
[README](../serial_bench/README.md) explains each part, each metric and the
fit. The results and their explanation are in
[docs/serial.md](../../../docs/serial.md#results-in-python-pyserial-psychtoolbox-yspserial).

## Before you start

1. Install [uv](https://docs.astral.sh/uv/).
2. Build a `ysp-serial` wheel. From `bindings/python/ysp_serial/`, run
   `uv build --wheel -o <dir> .` (on Windows this needs the MSVC build
   tools).
3. Flash the echo board and find its port, as the MATLAB
   [README](../serial_bench/README.md#flash-the-echo-board) says.
4. Connect the computer to AC power. Close other programs.

## Run it

The script itself needs only the Python standard library. It runs each API
in a new process, in a new uv environment that holds only that API's
package (`uv run --isolated --with ...`), so third-party wheels never share
an interpreter with each other. Run it twice, the second time with the API
order reversed:

```
uv run --no-project python serial_bench.py --port COM3 --ysp <dir>/ysp_serial-0.5.0.2-cp38-abi3-win_amd64.whl --apis pyserial,ioport,ioport_tuned,ysp --out run1 --label "Teensy 4.0 echo, run 1"
uv run --no-project python serial_bench.py --port COM3 --ysp <dir>/ysp_serial-0.5.0.2-cp38-abi3-win_amd64.whl --apis ysp,ioport_tuned,ioport,pyserial --out run2 --label "Teensy 4.0 echo, run 2"
```

One run of all four APIs took 297 and 315 s on the test laptop (2026-10-12).
Most of the variation is IOPort: each of its writes beside its background
reader takes seconds. Without `--port`, only the `clock`
and `noport` parts run. `--parts rt,stream` runs fewer parts.
`--set n_rt=100` changes a count (the names are those of `serial_bench.m`).
`--python 3.13` selects the interpreter (the default).

The median of a port changes from day to day. Run `raw_win32.exe`
([../serial_bench/](../serial_bench/README.md#the-tuned-ioport-and-the-raw-win32-floor)),
or `raw_posix` on Linux, in the same session, twice, the second time with `rev`, and compare each
API with that floor, not with the floor of another day.

## Read the results

The output folder has the files of the MATLAB bench: `summary.txt`,
`summary.csv`, `<api>_samples.csv`, `<api>_rt_<block or poll>.csv`,
`<api>_stream_<mode>.csv` and `conditions.txt`, plus `<api>_rows.json`, the
raw rows of each worker. Times are in microseconds.

Each API is timed with its own clock: `time.perf_counter()` for pyserial
(which has no clock), `GetSecs()` for IOPort and `ysp.serial.now()` for ysp.
All three read QueryPerformanceCounter on Windows.

What differs from the MATLAB bench:

| Item | Python bench |
|---|---|
| Stream modes of pyserial | `line` (`read(26)`), `readline` (`readline()`, the common idiom), `poll` (`in_waiting`, then `read`), `async` (`serial.threaded.ReaderThread`) |
| Echo probes around each stream | 2 to 3 ms apart instead of 0 to 1 ms: Python sends them faster, and 200 probes 0 to 1 ms apart fall in one 200 ms bucket of the fit |
| Writes beside an async reader | Timed with a reader of their own, which stops before the stream's echo probes start. IOPort's writes take seconds, and in a trial run a fit that spanned them had no slope and put the stamps 1.3 ms early |
| `noport` | adds `py_call_floor` (an empty Python call) and `py_error_baseline` (a raise and catch) |
| `cpu_pct` | `time.process_time()` of the Python process, all its threads |
| pyserial `read_empty` | `read(64)` with `timeout=0`. pyserial then calls `ClearCommError` and no `ReadFile`. Setting `timeout` reconfigures the port, so the part sets it once, outside the timed loop |
| IOPort data | `Write` takes a `numpy.uint8` array and `Read` returns a 1-by-n `float64` array. The bench converts outside the timed calls |

The limits of the MATLAB bench apply: the one-way split is good to the fit
bound (`fit_unc`), and the median of one open moves by 100 to 150 us from
one open to the next on this port.
