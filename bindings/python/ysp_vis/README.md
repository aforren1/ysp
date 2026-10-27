# ysp-vis Python binding

One CPython extension, `ysp._vis`, for three headers:
[ysp/screen.h](../../../include/ysp/screen.h) (the display, the frame
loop, flip at a time, the record of every flip),
[ysp/gfx.h](../../../include/ysp/gfx.h) (stimuli) and
[ysp/video.h](../../../include/ysp/video.h) (movies). Three thin modules
give the imports that match the headers: `ysp.screen`, `ysp.gfx` and
`ysp.video`. One module holds the three because their handles point at
each other and ysp/screen.h keeps state per process (EGL, the abort log,
the window thread). The plan and its reasons are in
[docs/rig_spec.md](../../../docs/rig_spec.md#host-language-bindings-python-matlab-octave),
section 12.

Status: phase A. `ysp.screen` binds the frame loop in full. `ysp.gfx` has
the draw path that a frame loop needs to show something (a background and
filled rectangles). Phase B adds the stimuli
of ysp/gfx.h and the movies of ysp/video.h.

Phase B, movies (0.10.1.1): `ysp.video` binds ysp/video.h's movies (frame
sequences, MPEG-1 through pl_mpeg, MP4 through Media Foundation), their
soundtrack on a `ysp.audio.Audio`, the frame sequence writer and the index
maker (How to play a movie, below).

**Windows only.** The other screen backends of ysp/screen.h are stubs.
The wheel is `cp38-abi3-win_amd64`: one build for CPython 3.8 and later.

## What the wheel holds

| File | What it is |
|---|---|
| `ysp/_vis.pyd` | the extension; SDL3 3.4.0 is linked in statically |
| `ysp/screen.py`, `ysp/gfx.py`, `ysp/video.py` | the thin modules |
| `ysp/libEGL.dll`, `ysp/libGLESv2.dll` | ANGLE 2.1.25848 (git 85cc0e0b9cc3), from the Electron v38.8.6 release for win32-x64 |
| `ysp/ysp_vis-LICENSE.SDL3.txt`, `ysp/ysp_vis-LICENSE.ANGLE.txt` | the license notices of SDL3 (zlib) and ANGLE (BSD) |

The extension always loads ANGLE from its own directory
(`desc.angle_dir`), so another ANGLE in the process (MATLAB's, a Qt
program's) is never the one used. SDL3 is static, so an `SDL3.dll` of
another package in the process cannot take its place
([docs/av_probe.md](../../../docs/av_probe.md), sections 2 and 5).
`d3dcompiler_47.dll` is not needed on Windows 10 and 11.

## Dependencies

`ysp-audio` and `ysp-timeline`, of the same major and minor version as the
headers that ysp-vis was built against. ysp/video.h calls ysp/audio.h and
ysp/timeline.h through their tables, so a movie's soundtrack plays on the
experiment's `ysp.audio.Audio` and a `ysp.timeline.Timeline` drives gfx
([docs/bindings.md](../../../docs/bindings.md#sharing-one-implementation-across-modules)).
An import with another major.minor fails with the reason.

## How to install

From the repository root, make the natives once, then install the three
packages:

```sh
uv run --no-project python tools/vendor_vis.py --write --angle --sdl msvc --pl-mpeg
uv pip install ./bindings/python/ysp_audio ./bindings/python/ysp_timeline ./bindings/python/ysp_vis
```

`tools/vendor_vis.py` downloads the SDL3 3.4.0 release tarball and the
Electron zip, checks both by SHA-256, builds the static SDL3 with MSVC and
takes the two ANGLE DLLs (also checked by SHA-256) into
`third_party/vis/`; `--pl-mpeg` fetches pl_mpeg.h (commit c871f2b, checked
by SHA-256), the MPEG-1 decoder compiled into the extension. A download
that breaks off resumes where it stopped. setup.py takes the headers, the shared tables and the
natives from the repository. An isolated build (an sdist, cibuildwheel)
sees only this directory: stage the files first (MANIFEST.in lists them).

## How to run a frame loop

```python
import gc
import ysp.screen as yscr
import ysp.gfx as ygfx

with yscr.Screen() as s, ygfx.Gfx(s, background=0.5) as g:   # fullscreen, primary display
    print(s.describe())
    gc.freeze()                      # after setup: no full collection of the setup's heap in a frame
    try:
        for i in range(600):
            f = s.begin()            # waits for the swap path (GIL released); f.onset: when this frame shows
            g.begin()
            g.rect(0, 0, 200, 200, (1.0, 1.0, 1.0))   # center from the screen's center, pixels
            g.end()
            s.flip()                 # presents at f.onset (GIL released)
    except yscr.Abort as a:          # Shift+Esc, the close button, Alt+F4, request_abort()
        print("aborted: 0x%x" % a.reasons)
    s.wait_flip()
    for r in s.records():            # every flip since the last call, oldest first
        print(r.index, r.onset, r.residual_ns, r.dropped, r.path, r.tier)
```

Draw each frame for `f.onset`, the predicted onset of its own flip. To show
a frame later, give the time: `s.flip_at(f.onset + 3 * f.period)`. The
record of a flip arrives one frame later: in `f.done` of the next
`begin()`, from `wait_flip()`, and from `records()`.

`example.py` is a complete loop: `python example.py` (a window),
`--full`, or `--sim` (no window).

## How to play a movie

```python
import ysp.audio as yau, ysp.gfx as ygfx, ysp.screen as yscr, ysp.video as yvid

with yau.Audio() as au, yscr.Screen() as s, ygfx.Gfx(s, background=0.45) as g, \
        yvid.Movie(g, "clip.mp4") as m:          # clip.mp4.yspvi beside it
    print(m.info["duration"], m.describe())
    m.soundtrack(au, "clip.wav")                 # optional; the movie clock follows the sound
    m.play()                                     # as soon as the frames and the sound are ready
    while True:
        f = s.begin()
        playing = m.update()                     # the frame due at f.onset; never waits
        g.begin()
        m.draw()                                 # 1:1 in the center; draw(x, y, w, h, ori, opacity, ...)
        g.end()
        s.flip()
        if not playing:                          # the first frame past the movie's end
            break
    s.wait_flip()                                # the last flips' records
    for r in m.records():                        # one per display frame
        print(r.display, r.frame, yvid.DECISION_NAMES[r.decision], yvid.WHY_NAMES[r.why], r.residual)
```

`update()` goes between the screen's `begin()` and the gfx's `begin()`,
once a display frame: it picks the frame due at the frame's predicted
onset by ysp/video.h's rule (never a count of frames) and puts it in the
texture. `draw()` goes between the gfx's `begin()` and `end()`. A file must
be in the canonical form ([docs/video.md](../../../docs/video.md)): a frame
sequence (`SeqWriter` writes one), MPEG-1 (pl_mpeg, built in) or an MP4
with H.264 or HEVC (Windows Media Foundation), each MPEG-1 and MP4 file
with its index beside it: `ysp.video.make_index("clip.mp4", matrix="bt709",
range="limited", transfer="bt1886", primaries="bt709", siting="left")` (an
MPEG-1 file needs no color arguments). A WAV soundtrack must be at the
device's rate and channel count and last as long as the movie, to the
sample. `example_video.py` plays a movie it makes, with sound: `python
example_video.py` (a window and the default output), `--full`, or `--sim
--null` (no window, no sound card).

### ysp.video reference

`Movie(gfx, path=None, *, data=None, index=None, timeline=None,
records=36000, **options)`: `data` is the file in memory (bytes; from
`ysp.pack.Pack.read()`, or `ysp.video.from_pack(gfx, pack, name)`), `index`
a path or bytes (None: `path + ".yspvi"`), `timeline` a
`ysp.timeline.Timeline` whose `base=` (1 to 7) is the movie clock. The
options are the header's desc (`ysp.video.options()` lists them): `loop`,
`ahead`, `preroll`, `lead`, `light` (`'auto'`, `'codes'`, `'eotf'`),
`chroma`, `backend` (`'auto'`, `'seq'`, `'plmpeg'`, `'mf'`), `hw_decode`
(`'auto'`, `'off'`, `'dxva'`), `strict_cadence`, `inline_decode`,
`inline_budget`, `refresh_num`, `refresh_den`, `movie_index`, `min_tier`,
`pin_cpu`. Opening releases the GIL.

| Call | Result |
|---|---|
| `m.update()` | `True` while playing, `False` from the first frame past the end; `OrderError` out of turn |
| `m.draw(x=0, y=0, w=0, h=0, ori=0, opacity=1, linear=False, place='center', anchor='center')` | the movie as a ysp/gfx.h IMAGE; `w`, `h` 0 = the frame's pixels |
| `m.play(at=None)`, `m.pause(at=None)` | an id; `at` a time on the ysp_rt clock, None as soon as possible |
| `m.seek(t, resume=None)`, `m.seek_frame(i, resume=None)` | an id; `t` a movie time; `resume` None, `'paused'` or a time |
| `m.show(i)` | this display frame shows frame `i` (manual mode); before `update()` |
| `m.result(id)` | the PLAY, PAUSE or SEEK record as a dict, or None before it happened |
| `m.last()` | the `Record` of the newest `update()` (its onset predicted, `F_PENDING`), or None |
| `m.records()` | `list[Record]`: the FRAME records completed since the last call (`records_lost`) |
| `m.events()` | `list[dict]`: the other records (`kind` open, play, pause, seek, loop, end, drop, clock, decode, sound, rate) with their times in seconds and `_ns` |
| `m.soundtrack(audio, path=None, *, data=None, db=0, channels=0, ramp=None, ring=None, follow=True)` | the WAV as a ysp/audio.h stream on a `ysp.audio.Audio`; before the first play. `follow`: the movie clock follows the sound |
| `m.follow_audio(audio)` | each `update()` takes the movie time from the audio clock; None stops it |
| `m.movie_time(t)`, `m.info`, `m.describe()`, `m.close()` | the movie time at `t`; size, rate, frames, duration (s and ns), form, cadence, counts; the log line |

A `Record` has `display`, `frame`, `onset`, `due`, `residual` (`onset -
due`; with the audio clock the frame's A/V error), `movie_t` (each with
`_ns`), `decision` (`SHOWN`, `REPEATED`; a dropped frame has a `drop`
event), `why` (`WHY_DUE`, `WHY_CADENCE`, `WHY_DISPLAY_LATE`,
`WHY_DECODE_LATE`, `WHY_DRIFT`, `WHY_SEEK`, `WHY_LOOP`, `WHY_MANUAL`;
`WHY_NAMES` gives the names), `tier`, `path` (`PATH_UPLOAD`), `flags`
(`F_*`), `ahead` and `shows`. The screen's flip records complete them, so
call `s.wait_flip()` before the last `records()`. A closed gfx or screen
closes its movies first; their records stay readable.

`SeqWriter(path, w, h, fps, format='rgba8', compression='raw', matrix,
range, transfer, primaries, siting, max_frames=0)` writes a frame
sequence, value-exact: `write(bytes)` (the planes tight, in order;
`frame_bytes` says how many), `finish()`, `close()`, a context manager.
`probe(path=None, *, data=None, index=None)` reads a movie's description
without opening it. `make_index(media, index=None, *, matrix, range,
transfer, primaries, siting, backend='auto', hw='off', note=None,
no_seek_check=False)` decodes every frame and writes the index (GIL
released). Errors: those of `ysp.screen`, and `ysp.video.FormatError` (an
`ArgumentError`), `NotFoundError` (a `FileNotFoundError` too) and
`IOError` (an `OSError` too). A path goes to the header's `fopen()` in the
ANSI code page; a path outside it is refused (pass `data=`).

The soundtrack's `Audio` is held through ysp.audio's table: its `close()`
raises `BusyError` until the movie closes. A `Timeline` given to `Movie()`
is held the same way.

## Between trials

The window belongs to a thread of ysp/screen.h's own, so `time.sleep()`,
`input()` or any other wait between trials does not hang it, and the first
frame after the wait is on time. No pumping call is needed.

## Reference

`Screen(**options)` opens the display and settles it: about 0.5 to 0.8 s
fullscreen, 2.5 s in a window. Keyword options only;
`ysp.screen.options()` gives each with a line of text. Times are seconds.

| Option | What it does |
|---|---|
| `windowed`, `size` | a window of `(w, h)` pixels in place of the full screen |
| `display` | a display's `id` from `displays()`; 0 = the primary |
| `mode` | a `Mode` from `modes()` to switch to; none = the desktop mode |
| `backend` | `'auto'` (COMPOSITION where it runs, else DXGI_FLIP), `'dxgi_flip'`, `'composition'`, `'sim'` (no window: tests) |
| `window_thread` | `'auto'` (on), `'on'`, `'off'` |
| `lead` | `flip_at()`'s snap: 0.5 (the nearest vblank) by default; -1 never early |
| `onset_offset` | seconds added to every onset (a photodiode's measured delay) |
| `depth`, `depth_learn` | the depth pin (1 to 8) and the opt-in learner |
| `settle`, `settle_flips`, `settle_min`, `settle_max` | open's settling: `'auto'`, `'strict'`, `'warn'`, `'off'` |
| `foreground` | `'auto'`, `'honor'`, `'force'` |
| `panic`, `panic_presses`, `panic_window`, `panic_grace` | the panic watchdog (off by default) |
| `patch`, `patch_size`, `patch_corner` | the photodiode patch; `set_patch(v)` sets its gray |
| `show_cursor`, `keep_os_gamma`, `key_dedup`, `gamepads`, `min_tier`, `sim_period` | as the header's desc |

| Call | Result |
|---|---|
| `s.begin()` | `Frame`: `onset`, `period`, `index`, `vblank`, `done` (the `Record`s completed since the last `begin()`), `last`, `done_lost`; times in seconds with `_ns` exact. Raises `Abort` once for each abort |
| `s.flip()`, `s.flip_at(t)` | present at the predicted onset, or at the vblank `t` snaps to |
| `s.wait_flip()` | `Record` of the newest flip, after every flip has its record |
| `s.records()` | `list[Record]`: every record completed since the last call (`records_lost` counts the overflow of 4096) |
| `s.poll(max=0)` | `list[Event]`: the input events since the last poll, in the shared layout of `ysp.device` and `ysp.response` |
| `s.mark(phase)` | charge the time since `begin()` or the last mark to `PHASE_EVALUATE`, `_SCRIPT`, `_DRAW` or `_UPLOAD` |
| `s.title(text)`, `s.cursor(show)`, `s.grab(on)` | through `yscr_window_call()`: the window thread does the SDL call |
| `s.describe()`, `s.caps()`, `s.settle()`, `s.sync()` | the log line, the capabilities, open's settling, the sync guard |
| `s.close()` | lets the last flip complete; closes a `Gfx` on the screen first |
| `displays()`, `modes(display=0)`, `mode_multiple(num, den, ...)` | the displays and their modes |
| `request_abort()`, `input_stats()`, `now()`, `now_ns()` | |

A `Record` has `index`, `target`, `planned`, `onset`, `residual` (each with
`_ns`), `dropped`, `missed` (`dropped > 0`), `late` (`FLIP_LATE_TARGET`:
the caller asked for a vblank it could no longer make), `path`, `tier`,
`flags`, `phases` (seconds per phase; `None` for the GPU phase, which the
header does not measure) and `code_risk`.

`Abort` is not an `Error`: the operator asked to stop. It has `reasons`
(`ABORT_*` bits), `presses`, `t`, `t_ns` and `panic`. After `begin()` raised
it, the next `begin()` starts a frame: the program decides how to stop. With
`panic` true, the panic released the display and only `close()` is left.

Errors: `ysp.screen.Error` and its subclasses `ArgumentError`
(`ValueError` too), `ClosedError`, `OrderError` (begin and flip out of
turn), `TimeoutError`, `LostError`, `RefusedError`, `NotImplementedError`
(a window call on the `'sim'` backend) and `BusyError` (a second thread's
call while one runs without the GIL). A message is the header's own.

`ysp.gfx.Gfx(screen, background=0.0)`: `begin()` after the screen's
`begin()`, `rect(x, y, w, h, color=1.0)` (center from the screen's center,
y down; color a gray level or `(r, g, b)`, linear device RGB 0..1),
`end()` before the screen's `flip()`, `describe()`, `close()`. One per
screen in phase A. A closed screen closes its gfx.

## Threads

The window thread and ysp/screen.h's other threads never take the GIL.
`Screen()`, `close()`, `begin()`, `flip()`, `flip_at()`, `wait_flip()`,
the display lists and the window calls release it. No Python function
runs on a C thread: triggers, the panic function and the after-flip hook
are not bound. Call `gc.freeze()` after setup (docs/av_probe.md: a full
collection of a large heap costs a frame in Python 3.14).

## Tests

```sh
pytest bindings/python/ysp_vis/tests                       # the simulated display: no window
YSP_VIS_WINDOW=1 pytest bindings/python/ysp_vis/tests/test_window.py   # a real window, about 15 s
```

`test_window.py` opens a 1280 x 720 window with the bundled ANGLE, draws
two seconds, idles 6 s without a call and draws again. CI runs the
simulated tests only.

`test_video_sim.py` makes its clips (frame sequences; MPEG-1 and an MP4
with ffmpeg when it is on PATH) and checks each display frame of a run on
the simulated display and the virtual clock against the due rule; with
`YSP_VIDEO_PLAY` set to an `examples/video/play.c` executable, against the
C path line by line. `YSP_VIS_AUDIO=1` adds one soundtrack on the default
output. `YSP_VIS_WINDOW=1 pytest .../test_video_window.py` plays a frame
sequence, MPEG-1 and an MP4 (DXVA and the software decoder) in a window.

## What is not bound

- Triggers at onset and the after-flip and before-present hooks: each
  runs a callback on a C thread or inside the header's frame, and no
  binding runs a host function there (docs/rig_spec.md, section 12).
- The panic function (`desc.panic_fn`): the same reason. The panic itself
  (`panic=True`) is bound.
- Codes (VPixx pixel mode and the like), text input, raw mice, groups of
  screens (`begin_group`) and the native handles: not in phase A.
- `desc.abort_keys`: the combination stays Shift+Esc.
- `desc.on_context`: `Gfx()` opens after the screen settles, so its
  programs compile after the settling, not during it (phase B can open
  both in one call).
- ysp/video.h: the GPU path (`gpu_path`; it needs the screen's
  `d3d11_video` option, not bound, and UPLOAD has the shorter tail by
  measurement), a decoder extension (`desc.decoder`: callbacks on the
  decode thread), a byte source by range (`desc.reader`: the same; `data=`
  takes the whole file), `yvid_follow()` with a host clock (a callback),
  the pure functions (`yvid_due`, `yvid_frame_time`), the QOI and YUV
  tools, and a movie's own event ring (records() and events() read it).
