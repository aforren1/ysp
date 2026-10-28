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
`ysp.video` plays movies with a soundtrack (phase B). `ysp.gfx` lays out
and draws text (phase C, 0.10.2.1; [Text](#text-yspgfx), below).

Phase B, movies (0.10.1.1): `ysp.video` binds ysp/video.h's movies (frame
sequences, MPEG-1 through pl_mpeg, MP4 through Media Foundation), their
soundtrack on a `ysp.audio.Audio`, the frame sequence writer and the index
maker (How to play a movie, below).

**Windows only.** The other screen backends of ysp/screen.h are stubs.
The wheel is `cp310-abi3-win_amd64`: one build for CPython 3.10 and later.

## What the wheel holds

| File | What it is |
|---|---|
| `ysp/_vis.pyd` | the extension; SDL3 3.4.0 is linked in statically |
| `ysp/screen.py`, `ysp/gfx.py`, `ysp/video.py` | the thin modules |
| `ysp/libEGL.dll`, `ysp/libGLESv2.dll` | ANGLE 2.1.25848 (git 85cc0e0b9cc3), from the Electron v38.8.6 release for win32-x64 |
| `ysp/ysp_vis-LICENSE.SDL3.txt`, `ysp/ysp_vis-LICENSE.ANGLE.txt` | the license notices of SDL3 (zlib) and ANGLE (BSD) |
| `ysp/ysp_vis-LICENSE.pl_mpeg.txt` | pl_mpeg's notice (MIT): its MPEG-1 decoder is compiled into the extension |
| `ysp/ysp_vis-LICENSE.Skribidi.txt`, `.HarfBuzz.txt`, `.SheenBidi.txt`, `.libunibreak.txt`, `.budouxc.txt` | the license files of the text layout linked into the extension: Skribidi (MIT), HarfBuzz (Old MIT; its USE table MIT), SheenBidi (Apache-2.0), libunibreak (zlib), budouxc (MIT; BudouX's models Apache-2.0) |

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
uv run --no-project python tools/vendor_layout.py --write
uv run --no-project python tools/vendor_vis.py --write --angle --sdl msvc --pl-mpeg --layout msvc
uv pip install ./bindings/python/ysp_audio ./bindings/python/ysp_timeline ./bindings/python/ysp_vis
```

`tools/vendor_vis.py` downloads the SDL3 3.4.0 release tarball and the
Electron zip, checks both by SHA-256, builds the static SDL3 with MSVC and
takes the two ANGLE DLLs (also checked by SHA-256) into
`third_party/vis/`; `--pl-mpeg` fetches pl_mpeg.h (commit c871f2b, checked
by SHA-256), the MPEG-1 decoder compiled into the extension. `--layout
msvc` builds the text layout as static libraries (pack/layout.c with the
Skribidi, HarfBuzz, SheenBidi, libunibreak and budouxc trees that
`tools/vendor_layout.py` fetches by commit and checks file by file against
`third_party/MANIFEST.sha256`), with CMake and MSVC, about a minute. A
download that breaks off resumes where it stopped. setup.py takes the headers, the shared tables and the
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
`IOError` (an `OSError` too), by the header's code: a file that is not
there raises `NotFoundError` from `Movie()`, `probe()`, `SeqWriter()` and
`soundtrack()`. A path (`str`, `bytes` or `os.PathLike`) goes to the
header as UTF-8 (PEP 529 on Windows), so any folder name works
(ysp/video.h 0.6.0).

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

## Text (ysp.gfx)

`ysp.gfx` lays text out with pack/layout: Skribidi with HarfBuzz, pinned
and patched ([docs/layout.md](../../../docs/layout.md)), the layout of
the pack tool and the player. So a text is shaped for its script (Arabic,
Hebrew, Devanagari, Thai, CJK: whatever its fonts hold), put in order by
the Unicode bidirectional algorithm, kerned and ligated by HarfBuzz's
default features, and broken into lines by UAX 14 (by BudouX phrases for
Japanese, Chinese and Thai with `lang`). ysp/gfx.h draws the glyphs as
curve runs from ysp/outline.h curve sets built from the same font bytes:
the pixel's box-filter coverage at any size, turned or not.

```python
import ysp.screen as yscr, ysp.gfx as ygfx

with yscr.Screen(gfx={"background": 0.5}) as s:
    g = ygfx.Gfx(s)
    fonts = g.font([r"C:\Windows\Fonts\segoeui.ttf", r"C:\Windows\Fonts\msyh.ttc"])   # a fallback list
    word = g.text("RED", font=fonts, size=64, color=(0.6, 0.05, 0.05))
    page = g.text(open("instructions.txt", encoding="utf-8").read(), font=fonts, size=24,
                  width=900, align="start", line_height=1.3, y=-100)
    word.set(text="GREEN", color=(0.05, 0.45, 0.05))   # a new layout: between frames
    f = s.begin()
    g.frame(word, page)
    s.flip()
    for w, start, end, line, x0, y0, x1, y1 in page.words():   # interest areas, screen px
        ...
```

**Fonts.** `g.font(source, face=0, name=None)` makes a `Font`: a path, the
bytes of a font file, or a list of them (or of `(source, face)` pairs) as a
fallback list. For each run of one script, the files that support it are
tried in order, and the first that has the character draws it. A text's
`font=` is a `Font`, a path or a list of paths (made once per `Gfx` and
kept), or none: the system's sans font, Segoe UI, else Arial. No font file
is in the wheel: Segoe UI is on every Windows machine, the only platform,
and a bundled file would be another pin and another license to carry; give
a file to have the same glyphs on every machine. `f.info` gives each
file's name, face, SHA-256 and metrics, and `t.stamp` (and `f.stamp()`)
one line with the layout's versions and the hashes of the fonts a text
used, for a data file. TrueType and CFF fonts; CFF2 and fonts without
outlines are refused (ysp/outline.h reads the outlines; docs/layout.md).

**Parameters.** `g.text(text="", **p)` returns a `Text`, a `Stim` with its
own parameters:

| Name | Value | Default |
|---|---|---|
| `text` | a str | `""` |
| `font` | a `Font`, a path, a list of paths | the system's sans font |
| `size` | units per em (px, or degrees with `units="deg"`) | 32 px, 1 deg |
| `width` | the wrap width in units; 0: no wrapping | 0 |
| `align` | `"start"`, `"center"` or `"end"` (start and end follow the direction) | `"center"` |
| `line_height` | times the fonts' own line; 0: 1 | 0 |
| `dir` | `"auto"` (the first strong character), `"ltr"` or `"rtl"` | `"auto"` |
| `lang` | a BCP 47 tag; `"ja"`, `"zh-Hans"`, `"zh-Hant"` and `"th"` break at BudouX phrases | `""` |
| `spans` | `[(start, end, color=None, size=0), ...]`: str indices, end excluded, sorted, not overlapping; at most 15 colors | none |

A change of one of these lays the text out again, between frames
(`OrderError` between `g.begin()` and `g.end()`); a refused change leaves
the text as it was. A word or a sentence took 3 to 16 us a call, a page of 520
characters 0.06 to 0.13 ms, in the three hosts ([docs/gfx.md](../../../docs/gfx.md#text-measured),
measured). A glyph that no text of the font used before is built from the
font's outline and uploaded in the same call (tens of us a glyph): lay out
every word of an experiment once at setup and the trials build none.
The rest are a stimulus's parameters, as for every `Stim`: `place`,
`anchor`, `x`, `y`, `ori`, `color` (with `space=` or a hex color through
a calibration), `opacity`, `visible`, `gate`, `group`, `blend`, `rays` and
`aliased` (ysp/gfx.h CURVE RUNS). Edges, strokes, effects and paint are
refused on text, as ysp/gfx.h refuses them on a run.

**The box.** A text's box is its laid-out block: as wide as its widest
line, or as `width` when it wraps, and from the first line's top to the
last line's bottom. `place` and `anchor` put that box on the screen as
they put any stimulus's: by default its center is the screen's center. So
a word is centered by its line, ascender to descender, not by its ink.
`t.box` is `(w, h)` in units; `t.bounds()` the screen px.

**Reading the layout.** Screen px, axis-aligned (a turned text gives the
bounds of the turned boxes):

| Call | Gives |
|---|---|
| `t.glyphs()` | `[(index, line, x0, y0, x1, y1)]`: each glyph's character (str index), line, and ink box; a glyph with no ink (a space) gives its pen point |
| `t.words()` | `[(word, start, end, line, x0, y0, x1, y1)]`: the runs of characters between white space (no-break spaces join), with their glyphs' advances across and their line's box down: an eye tracker's interest areas |
| `t.lines()` | `[(start, end, x0, y0, x1, y1)]`: each line's characters and box; a line's trailing space is in its box and may hang past `width` |
| `t.hit(x, y)`, `t.hit_index(x, y)` | whether a glyph's box holds the point, and the str index of the topmost one (-1: none); `L.hit_element(x, y)` gives `(item, index)` for a text in a list |
| `t.n_lines`, `n_glyphs`, `n_runs`, `n_chars`, `missing`, `resolved_dir` | the layout's counts; `missing`: glyphs no font of the list has (drawn as the first font's .notdef box). A character no font supports at all can give no glyph: an emoji without an emoji font |
| `t.layout_time`, `build_time`, `upload_time`, `new_glyphs` | the last layout's parts, in seconds, and the glyphs it built |

**Lists and bindings.** A `Text` goes in a `DrawList` with any stimulus,
and `L.set("color", ...)`, `L.set("y", ...)` work across texts. A timeline
channel binds to a text's `x`, `y`, `ori`, `opacity`, `color`, `visible`,
... as to any stimulus. `size`, `text` and the other own parameters do
not bind (each change is a layout): animate the size with a group's
`scale`. A text takes 3 of the gfx's `max_draws` blocks a frame per run
(per font and size in it): past about 340 texts a frame, open the `Gfx`
with a larger `max_draws`. Many words that do not move are cheaper as one
text: 100 one-word texts cost 1.0 to 1.3 ms of host work a frame, a page
of 520 characters as one text 0.4 to 0.6 ms (docs/gfx.md, Text: measured).

**Memory.** Making a font or a text, and each layout, allocate (Skribidi
allocates inside every layout). Drawing a text allocates nothing.

`example_stroop.py` runs a Stroop block (one `Text`, its word and ink
changed between trials); `example_reading.py` an RSVP sentence (a `Text`
per word, shown by a timeline) and the sentence's word boxes.

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

`test_text_sim.py` checks text on the simulated display: fonts by path,
bytes and fallback list, bidi, wrapping, spans, a change of text between
trials and its refusals, the glyph, word and line boxes and hit tests,
texts in lists and bindings, degrees, and the lifetime rules. It uses
Windows's Segoe UI, Arial and Microsoft YaHei, and skips what needs a
missing one. `YSP_VIS_WINDOW=1 pytest .../test_text_window.py`: two texts
made through `ysp.gfx` (Latin; and Latin, CJK and Arabic from a fallback
list, wrapped, with a colored span, turned 20 degrees) draw the same scene,
bit for bit, as the same texts made in C with pack/layout and
`ygfx_crun()`; the ink drawn is inside the boxes `glyphs()` and `words()`
give; a text changed and changed back draws its first scene again.

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
- `desc.on_context` as a callback: `Screen(gfx={...})` uses it to start
  the gfx while the screen settles; no Python function runs there.
- In ysp.gfx: compounds, effects (`fx`), paint (gradients), MSDF glyph
  runs, curve sets and runs as objects of their own, user shaders, render
  targets, blur and video textures (ysp.video's). Each needs memory or GL
  objects with a lifetime of its own; text uses curve sets and runs
  inside a `Text`, and none of the rest was needed by the experiments of
  phases B and C. Text without blur has no glow or soft shadow; a hard
  shadow is a second `Text` offset and recolored.
- In text: OpenType feature switches (HarfBuzz's defaults apply: kerning,
  ligatures, marks), a language per span, vertical text, justification,
  outlines and faux bold (ysp/outline.h's strokes), color glyphs, and the
  editor (pack/layout has Skribidi's: caret, selection, IME). pack/layout's
  style takes none of the first two; the rest need a design of their own. File loading: ysp/gfx.h reads no image file; make the
  pixels with NumPy or an image library and give them to `texture()`.
- ysp/video.h: the GPU path (`gpu_path`; it needs the screen's
  `d3d11_video` option, not bound, and UPLOAD has the shorter tail by
  measurement), a decoder extension (`desc.decoder`: callbacks on the
  decode thread), a byte source by range (`desc.reader`: the same; `data=`
  takes the whole file), `yvid_follow()` with a host clock (a callback),
  the pure functions (`yvid_due`, `yvid_frame_time`), the QOI and YUV
  tools, and a movie's own event ring (records() and events() read it).
