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

Status: `ysp.screen` binds the frame loop in full (phase A). `ysp.gfx`
binds the stimuli, draw lists, groups, instances and timeline bindings of
ysp/gfx.h (phase B, 2026-10-27; [Stimuli](#stimuli-yspgfx), below).
`ysp.video` is a placeholder. Text is phase C.

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
uv run --no-project python tools/vendor_vis.py --write --angle --sdl msvc
uv pip install ./bindings/python/ysp_audio ./bindings/python/ysp_timeline ./bindings/python/ysp_vis
```

`tools/vendor_vis.py` downloads the SDL3 3.4.0 release tarball and the
Electron zip, checks both by SHA-256, builds the static SDL3 with MSVC and
takes the two ANGLE DLLs (also checked by SHA-256) into
`third_party/vis/`. setup.py takes the headers, the shared tables and the
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

`ysp.gfx.Gfx(screen, **options)`: `background` (a gray level or linear
`(r, g, b)`), `units`, `view`, `dither`, `seed`, `cones`, `cal`, `lum`,
`max_draws`, `max_instances`, `cache` (True, the default: the per-user
program cache, so a second open compiles nothing; False; or a folder) and
`prime`. `Screen(gfx={...})` takes the same options and starts the gfx
while the screen settles; `Gfx(screen)` then finishes it. One per screen:
its output stage writes every pixel of the frame. A closed screen closes
its gfx. `begin()`, `end()`, `rect(x, y, w, h, color)` (phase A's
immediate rectangle) and the rest: [Stimuli](#stimuli-yspgfx).

## Stimuli (ysp.gfx)

`ysp.gfx` binds the stimuli of ysp/gfx.h (phase B, 2026-10-27). A `Gfx`
makes them and owns their memory: the C stimulus holds pointers (a path,
the elements of instances, a palette, a group) that must stay valid while
it is drawn, and here each one points into memory the binding owns.

```python
import ysp.screen as yscr, ysp.gfx as ygfx, ysp.timeline as ytl

with yscr.Screen(gfx={"background": 0.5}) as s:      # the programs compile while the screen settles
    g = ygfx.Gfx(s)
    left = g.grating(x=-200, w=200, sf=1 / 20, aperture="circle", edge="cosine", edge_width=12)
    right = left.copy()
    right.x = 200
    fix = g.shape("circle", w=10, color=0.0)
    scene = g.list([left, right, fix])                # drawn in order by one call
    tl = ytl.Timeline(["contrast"])
    g.bind(scene, "contrast", tl, "contrast")         # written in C at each frame
    g.timeline(tl)                                    # evaluated at each frame's predicted onset
    f = s.begin()
    tl.anchor(1, f.onset)
    tl.tween("contrast", 0.5, base=1, start=0.0, duration=0.5, from_=0.0)
    while True:
        g.frame(scene)                                # begin, the bindings, the draws, end
        s.flip()
        f = s.begin()
```

**Makers.** Each takes the parameters by ysp/gfx.h's names as keywords
and returns a `Stim`:

| Call | Kind |
|---|---|
| `g.shape(shape, **p)` | `'rect'` (`radius`), `'circle'`, `'annulus'` (`inner`), `'line'` (`width`), `'polygon'` (`vertices`: 3 to 16 `(x, y)`), `'cross'` (`width`), `'rrect'` (`radii`), `'arc'` (`sweep`, `thickness`, `start`), `'pie'` (`sweep`, `start`), `'capsule'` (`radii`), `'ngon'` (`sides`, `rounding`), `'star'` (`points`, `inner`, `rounding`), `'ellipse'`, `'qbezier'` and `'polyline'` (`path`, `width`; `join`, `cap`) |
| `g.grating(**p)`, `g.gabor(**p)`, `g.noise(**p)` | modulation stimuli: `sf`, `phase`, `sigma`, `size` (gabor: 0 = 8 sigma), `aperture`, `dist` (`'uniform'`, `'binary'`, `'gaussian'`, `'simplex'`), `check`, `seed`, `contrast`, `dir` |
| `g.texture(pixels, w=0, h=0, format=None, linear=False, srgb=False)`, `g.image(tex, **p)` | a `Texture` from a buffer (NumPy `(h, w)` or `(h, w, c)`: uint8 1 to 4 channels, uint16, float32 or float64; rows top first) and an image of it (`src` for a sprite of an atlas, `modulation`, `tint`); `tex.update(pixels, x=0, y=0)` between frames |
| `g.dots(n=, positions=, **p)` | a dot field; `d.positions = xy` (`(x, y)` rows, units from its center) between frames |
| `g.instances(template, n, fields=("xy",), palette=None)` | n elements of a template in one draw; `elements` is a writable float32 `(n, 8)` view: x, y, ori, phase, contrast, scale, gate, color |
| `g.list(items)`, `g.group(**p)` | a `DrawList`; a `Group` (`place`, `anchor`, `x`, `y`, `w`, `h`, `ori`, `scale`, `opacity`, `visible`) that a stimulus joins with `group=` |

Every parameter is an attribute: `st.x = 10`, `st.color = (1, 0, 0)`,
`st.edge = "gaussian"`, `st.anchor = "top_left"` or `(0, 0)`.
`st.set(**p)` sets several. `ysp.gfx.params(kind)` lists the names a kind
reads, with unit, range and default. Units: the gfx's (`units="px"`, or
`"deg"` with `view=(distance_mm, width_mm)`); `x`, `y` from the place point
(the screen's center by default), y down, `ori` clockwise in degrees.
Colors are linear device RGB, or, with a calibration (`Gfx(cal=...)`, a
`ysp.color.Calibration` or the bytes of a .yspcal file), any ysp/color.h
space: `space="dkl"` with `dir=(elevation, azimuth, contrast)`, a hex
sRGB color `color="#ff8000"`, `g.color(space, a, b, c)` and
`g.dir(space, a, b, c)`.

**A frame in a few calls.** `g.frame(*items)` is `begin()`, the bindings,
`draw(*items)` and `end()`. A `DrawList` draws its stimuli in order;
`L.set(name, values)` sets a parameter of every item in one call (n values,
n x k for a color, or one for all) and `L.get(name)` reads them. For many
copies of one stimulus, `instances()` is one draw: write the elements
through the view, with NumPy, at no call per element.

**Timeline bindings.** `g.bind(target, name, timeline, channel, element=None)`
binds a parameter to a channel of a `ysp.timeline.Timeline` (by index or
name): a stimulus (`color` takes three channels), a `DrawList` (one
channel per item, or one for all), a `Group`, or elements of instances.
`g.begin()` (and `g.frame()`) writes the channels' current values into the
stimuli in C. With `g.timeline(tl)`, the gfx also evaluates the timeline at
the frame's predicted onset first; without it, evaluate it yourself (the
fired events come back to you). The gfx pins each timeline it uses (its
`close()` raises `BusyError` meanwhile) and gives the pin back at
`unbind()`, `close()` and the gfx's own close.

**Hit tests.** `st.hit(x, y)` (screen px from the top-left, the space of
mouse events at pixel density 1), `inst.hit_index(x, y)`, `L.hit(x, y)`
(the topmost visible item) and `L.hit_element(x, y)`, by the same distance
field as the GPU's coverage of 0.5. `st.bounds()` and `st.resolve()` give
the box and the anchor on the screen.

**Capture and reads.** `g.capture_start(path=, every=, rect=, slots=, manual=)`,
`capture_mark()`, `capture_pump()`, `capture_stop()` write chosen frames
as PPM files off the frame thread. `g.read_scene(x, y, w, h)` and
`read_output(...)` wait for the GPU: for tests.

**Lifetime.** A stimulus, list, group or texture holds its `Gfx`. After the
`Gfx` (or its `Screen`) closes, a set, a draw or a hit test on one raises
`ClosedError`; reading a parameter gives its last value. The `elements`
view owns its memory: kept after the stimulus is gone, it reads the last
values ([docs/timeline.md](../../../docs/timeline.md), "A view owns what it
shows").

`example_2afc.py` runs a two-alternative forced-choice grating trial;
`example_rsvp.py` an RSVP stream of 20 atlas items at 10 per second (NumPy).

## Threads

Use a `Screen`, its `Gfx` and the gfx's objects from one thread: the GL
context is that thread's. The window thread and ysp/screen.h's other
threads never take the GIL.
`Screen()`, `close()`, `begin()`, `flip()`, `flip_at()`, `wait_flip()`,
the display lists and the window calls release it. No Python function
runs on a C thread: triggers, the panic function and the after-flip hook
are not bound. Call `gc.freeze()` after setup (docs/av_probe.md: a full
collection of a large heap costs a frame in Python 3.14).

## Tests

```sh
pytest bindings/python/ysp_vis/tests                       # the simulated display: no window
YSP_VIS_WINDOW=1 pytest bindings/python/ysp_vis/tests/test_window.py   # a real window, about 25 s
```

`test_screen_sim.py` and `test_gfx_sim.py` run on the simulated display,
where gfx's null backend draws nothing: the frame loop, and the stimuli,
lists, instances, bindings and lifetime rules of `ysp.gfx`.
`test_gfx_sim.py` needs NumPy, and `ysp.color` for its calibration test.
`test_window.py` opens a 1280 x 720 window with the bundled ANGLE, draws
two seconds, idles 6 s without a call and draws again; then five stimuli
made through `ysp.gfx` must draw the same scene, bit for bit, as the same
stimuli made in C, and instances must match single stimuli. CI runs the
simulated tests only.

## What is not bound

- Triggers at onset and the after-flip and before-present hooks: each
  runs a callback on a C thread or inside the header's frame, and no
  binding runs a host function there (docs/rig_spec.md, section 12).
- The panic function (`desc.panic_fn`): the same reason. The panic itself
  (`panic=True`) is bound.
- Codes (VPixx pixel mode and the like), text input, raw mice, groups of
  screens (`begin_group`) and the native handles: not in phase A.
- `desc.abort_keys`: the combination stays Shift+Esc.
- `desc.on_context` as a callback: `Screen(gfx={...})` uses it to start
  the gfx while the screen settles; no Python function runs there.
- In ysp.gfx: text (phase C), compounds, effects (`fx`), paint
  (gradients), MSDF glyph runs and curve sets, user shaders, render
  targets, blur and video textures (ysp.video's). Each needs memory or
  GL objects with a lifetime of its own; none was needed by the phase B
  experiments. File loading: ysp/gfx.h reads no image file; make the
  pixels with NumPy or an image library and give them to `texture()`.
