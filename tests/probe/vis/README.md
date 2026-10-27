# ysp-vis measurements

The harness of the phase A measurements in
[docs/screen.md](../../../docs/screen.md#bindings): a 10-minute loop of
the minimal frame through the real binding (Python `ysp.screen` and
`ysp.gfx`, MEX `ysp_screen` and `ysp_gfx`), to compare with the probe of
[docs/av_probe.md](../../../docs/av_probe.md), and an idle gap of 30 s
without a call into the module. Windows only.

| File | What it is |
|---|---|
| `loop.py` | The Python loop: each frame `begin()`, `Gfx.begin()`, one `rect()`, `Gfx.end()`, `flip()`; the records of each frame kept, as a script keeps its data; `gc.freeze()` after setup; `--idle S` sleeps S seconds after frame 120 |
| `loop_mex.m` | The same in MATLAB and Octave (`pause` for the gap) |
| `watch_hung.py` | Polls the window by title every 50 ms: `IsHungAppWindow()`, and a "Ghost" window in front |
| `run.sh` | One run: the host and, for the gap, the watcher |
| `table.py` | The table of the CSVs, with av_probe.md's columns; `--after 120 CSV` gives the records after the gap |
| `movie.py` | A movie (phase B, `ysp.video`): each frame `begin()`, `Movie.update()` (timed), `Gfx.begin()`, `Movie.draw()`, `Gfx.end()`, `flip()`; `--wav` adds the soundtrack on the default output. Writes PREFIX.frames.csv (FRAME records), .flips.csv (flip records with the DRAW, SWAP and UPLOAD phases), .update.csv and .txt |
| `movie_mex.m` | The same in MATLAB and Octave |
| `movie_table.py` | The table of movie runs: frame decisions, drops, missed flips, work, `update()`, the A/V error the records report, the sound's start ([video.md](../../../docs/video.md#bindings)) |

"Work" is a record's DRAW phase: the time from the return of
`yscr_begin()` to the call of `yscr_flip()`, so it holds everything the
host does in a frame through the binding (the frame's conversion, the
draw calls, the GIL), as av_probe.md's work did.

## Run

Build first: the wheel into a venv (bindings/python/ysp_vis/README.md) and
the MEX files (bindings/mex/build.m). Every run is a timing measurement:
one at a time, through the timing guard, on a quiet machine.

```sh
export PY=<venv>/Scripts/python.exe OCTAVE="C:/Program Files/GNU Octave/Octave-10.1.0/mingw64/bin/octave-cli.exe"
G="bash /c/tmp/psy-guard.sh time"
$G vis_py_full     bash tests/probe/vis/run.sh py full OUT/py_full          # 600 s fullscreen
$G vis_matlab_full bash tests/probe/vis/run.sh matlab full OUT/matlab_full
$G vis_py_idle     bash tests/probe/vis/run.sh py idle OUT/py_idle          # 30 s gap, watched
$G vis_octave_idle bash tests/probe/vis/run.sh octave idle OUT/octave_idle
$PY tests/probe/vis/table.py OUT/*_full.csv
$PY tests/probe/vis/table.py --after 120 OUT/py_idle.csv
cat OUT/py_idle.watch
```

The movies (the 60 s clips of `tests/media/make_video_clips.sh --long`,
indexed with `ysp.video.make_index()`, and a 60 s WAV at the device's
rate):

```sh
$G vis_movie_py_30 $PY tests/probe/vis/movie.py OUT/m_1080p30.mp4 --wav OUT/m_60s.wav --out OUT/py30
$G vis_movie_matlab_30 matlab -batch "addpath('bindings/mex'); addpath('tests/probe/vis'); movie_mex('OUT/m_1080p30.mp4', 'wav', 'OUT/m_60s.wav', 'out', 'OUT/ml30')"
$PY tests/probe/vis/movie_table.py OUT/py30 OUT/ml30
```

`window` in place of `full` runs a 1280 x 720 window. Shift+Esc ends a run.
