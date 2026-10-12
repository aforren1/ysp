# Rig specification: display, audio, video, pack, player, designer

Status: proposal of 2026-09-24, revised 2026-10-06. It records the
decisions made while planning the move from the transport and adaptive
headers into graphics, audio, video and the tooling above them. Steps 1 to
6 of section 11 have started; section 11 gives the state of each step.
Each header's design note in `docs/` records what was built and measured.
When a note and this document disagree, the note is newer.

The existing headers ([README.md](../README.md)) and their conventions ([conventions.md](conventions.md))
stay as they are. This document adds to them.

## 1. Principles

These extend the conventions in conventions.md. Every new header, tool and
format follows them.

1. **One clock.** Every timestamp in the system is on the `ysp/rt.h`
   clock. A backend that reports time on another clock converts it at
   the boundary and records the correlation.
2. **Present at a time, not at an interval.** The display, audio and
   video APIs take a target time and report the actual time. The
   difference is data and goes in the log.
3. **A measurement or nothing.** A latency number in a header is a
   measurement from a loopback test, or a bound, or a pointer to the
   test. The STATUS block says which.
4. **One canonical format per resource.** The runtime accepts exactly
   one in-memory and one on-disk form of each resource type. It rejects
   everything else at open with a message that names the tool that
   produces the canonical form. There is no on-the-fly fallback.
5. **No implicit conversion.** No resampling at play, no resizing at
   upload, no color format coercion, no frame rate conversion. A
   mismatch is a refusal.
6. **Data over code.** Everything an experiment changes often is data in
   the pack: conditions, timelines, parameters, resources. Code is the
   player, or an eject path for the few who need it.
7. **One external dependency per device header, at most.** Pure
   computation headers depend on nothing. Device headers may depend on
   one library each, named in the "Needs" line of its entry in status.md.
8. **Introspectable descriptors.** Every parameter a designer can set is
   a plain value in a `desc` struct, and every new header exports a
   table that enumerates its parameters with type, range, unit and
   default.

## 2. Architecture

```
designer (web app, static, offline PWA)
    |  runner protocol: JSON over WebSocket, localhost, token
    v
player (prebuilt per platform, and wasm)   <--  pack (zip, store-only)
    |
    +-- ysp/screen.h   window, display modes, flip at time      [SDL3, ANGLE]
    +-- ysp/gfx.h      stimuli on GL ES 3.0, output stage       [ysp/screen.h]
    +-- ysp/audio.h    device, scheduled playback, synthesis    [miniaudio]
    +-- ysp/video.h    decode-ahead, scheduler, capture         [ysp/screen.h, SDL3]
    +-- ysp/timeline.h events and tracks on the clock           [nothing]
    +-- ysp/trials.h, ysp/stair.h, ysp/quest.h, ysp/aep.h        [as today]
    +-- ysp/rt.h       clock, waits, worker, pump, event ring   [OS]
    +-- ysp/parallel.h, ysp/serial.h                            [ysp/rt.h]
```

The headers stay usable without the player, the pack or the designer.
The player is a program that uses them. The designer is a program that
produces packs and talks to the player.

## 3. Dependency decisions

| Component | Decision | Reason |
|---|---|---|
| SDL3 | Accepted, for windowing, input, display modes, camera capture | Windowing is the hairiest OS surface. Stable ABI, packaged everywhere, Emscripten support, exposes native handles so present-timing code can go under it. Camera API since 3.2 covers Media Foundation, AVFoundation, V4L2 and getUserMedia. |
| ANGLE | Accepted, for GL ES 3.0 on Windows and macOS | Three browser vendors maintain it. One shader language end to end. Same code path as WebGL2 in the browser. Reliable on Intel integrated GPUs where vendor GL is not. Exposes vblank timestamps on its D3D11 backend. Binaries, not a source build (decided 2026-10-06): CI takes ANGLE from an Electron release zip, checked by checksum; development machines use whatever Electron copy is present. A source build at a pinned commit (depot_tools, about 10 GB) waits until an ANGLE bug or a missing feature needs a patch. Each measurement names the ANGLE version it ran on, and program binaries are keyed per ANGLE build. |
| miniaudio | Accepted, for audio devices | Single file, public domain, WASAPI shared and exclusive, CoreAudio, ALSA, PulseAudio, Web Audio. Same implementation-macro mechanic as `ysp/rt.h`. |
| Emscripten | Accepted, for the web player | Required by SDL3's web backend. `ysp/rt.h` already has the branch. |
| zig cc | Accepted, for the eject path and custom modules | One directory, cross-compiles to Windows and Linux with a pinned glibc. Driven as a compiler, version pinned, shipped with the designer. |
| glslang, SPIRV-Cross | Accepted, in the tool only | Compile the user shader contract offline. Both have WebAssembly builds, so the designer runs them in the browser. Never in the runtime. |
| FFmpeg | Accepted, opt-in, as a video backend on Linux and as the tool's transcoder | The tool shells out to the command line. The Linux hardware-decode backend links the libraries, off by default. |
| pl_mpeg | Accepted, as the zero-install compressed video backend | Single header, MPEG-1 and MP2. |
| QOI | Accepted, for lossless frame and texture compression | Single header, fast, exact. |
| bgfx | Rejected | Hides the swap chain, runs a render thread, precompiled shaders per backend for every user, heavy build. |
| sokol_gfx | Rejected | Good design, single author. The ES 3.0 plan needs no second graphics abstraction. Reference for the backend interface shape. |
| SDL_GPU | Rejected for now | No GL, no browser backend shipped, SDL owns the swap chain. Revisit if ANGLE fails on a platform. |
| WebGPU via Dawn | Deferred | The consistent choice if compute becomes a must. Larger binary, WGSL, no present timing. |
| GStreamer | Rejected | Hundreds of megabytes, runtime plugin discovery, platform install failures. |
| Magnum | Rejected | C++ engine, single author, GL-centric, nothing ES 3.0 does not do for this surface. |
| Dear ImGui | Accepted for the operator window in examples, C++ only. Rejected for stimuli | UI batcher, packed 8-bit color, no gamma or bit depth control. |
| alwan | Not a dependency | Color science library, explicitly no device characterization. The rig needs measured primaries, not standard ones. |
| Basis Universal | Opt-in only | Lossy in exactly the values a calibrated stimulus is made of. |
| Luau | Proposed, in the player only, for scripted logic | Sandboxed by default, fast interpreter, incremental collector, a type checker and language server that build to WebAssembly for the designer's editor. C++, so never in a header. Plain Lua 5.4 is the fallback if C matters more than tooling. |
| QuickJS | Rejected for scripting | Familiar language, but larger, a less predictable collector, and the web player would have to choose between QuickJS in WebAssembly for parity and the browser engine for speed. |
| nanovg | Rejected | Vector UI drawing with a fixed anti-aliasing fringe and its own font atlas. A stimulus edge is a parameter; see the signed-distance shapes in `ysp/gfx.h`. Unmaintained since 2019. |
| wasm3 | Deferred | Hosts modules from any language in the native player if someone needs Rust or C++ logic. |
| minicoro, tina | Deferred | Native stackful coroutines. Luau's VM-level coroutines cover the scripting model with no native stack switch, so they run in the web player and under the sanitizers. Only the eject path could want native coroutines for sequential C. If that demand appears, minicoro, player only, never in a header, and not in the web player. |
| Tracy | Accepted, opt-in, through a macro layer in `ysp/rt.h` | The headers see only C macros that compile to nothing by default. Tracy's C++ client is compiled into the player or the user's program. On-demand mode in a deployed player. |

## 4. Headers

### 4.1 ysp/rt.h additions

- **Event ring.** A wait-free fixed-record ring that any thread pushes
  into without a lock or an allocation, drained by a normal thread. The
  record is a timestamp, a source, a kind and a small payload. The
  screen, audio and video headers push their flip, onset and frame
  records into a ring the caller supplies. Export format is the
  caller's business; examples show a CSV drain.
- **Clock correlation.** A call that correlates the `ysp/rt.h` clock
  with another clock read back to back, for SDL's tick clock, the
  Windows interrupt-time clock and audio device positions.
- **Instrumentation macros.** Zone, frame mark, plot and message
  macros. By default they compile to nothing. With `YRT_TRACY` they
  map to Tracy's C API. With `YRT_TRACE_RING` they write zones to
  the event ring, so a rig without Tracy still records them. Every
  header instruments its hot paths through these macros: the frame
  loop phases, the pump callbacks, the audio callback, the decode-ahead,
  the serial reader. Frame marks go at the flip. Plots carry the flip
  residual, the audio fill level and the decode-ahead depth. Messages
  mirror trial and event boundaries. A message with the `ysp/rt.h` time
  at each frame mark correlates Tracy's clock with the rig's. A thread
  that will run a real-time callback emits one zone at open to
  allocate its queue before it matters.

### 4.2 ysp/screen.h (`yscr_`, `YSCR_`)

Needs SDL3. Owns the window, the GL ES 3.0 context, the display mode,
the swap path and the input restamping.

- **Context.** EGL through ANGLE on Windows and macOS, the native driver
  on Linux, WebGL2 under Emscripten. Confirm SDL3's EGL path on Cocoa
  with ANGLE's Metal backend before the first macOS build. On Windows,
  SDL3 owns only the window: the header makes the D3D11 device and the
  DXGI swapchain and gives ANGLE a client-buffer pbuffer on the back
  buffer, because SDL's EGL path gets ANGLE's blt-model window surface,
  which has no vblank statistics and no frame latency control
  ([screen.md](screen.md) has the probe numbers).
- **Display modes.** Enumerate the chosen display's modes. Offer "the
  mode whose refresh is an integer multiple of this rate" for video and
  report what was obtained.
- **Flip at a time.** `yscr_flip_at(t)` presents the drawn frame at
  target time `t` on the `ysp/rt.h` clock. It does not wait for the flip:
  the record of the flip, with the estimated onset, the residual and a
  dropped-frame count, arrives with the next frame's begin call and in the
  event ring, and `yscr_wait_flip()` blocks for it as Psychtoolbox's
  Flip does. The capability query reports one of three kinds:
  - fixed grid, with the period, where `t` snaps to the nearest vblank by
    the timeline's rule (the first vblank at or after `t` minus half a
    period), so a target with a little prediction noise does not go a
    frame late; a never-early lead is an option;
  - continuous, with a minimum and maximum interval, for variable
    refresh;
  - callback-driven, with the grid the compositor gives, for the
    browser and macOS display links.
- **Queue depth.** At most one frame in flight. Where the platform gives
  no control, the header issues a trivial draw and waits on a fence
  after the swap.
- **Swap path backends**, behind one interface:
  - Windows 11: the composition swapchain API, with present-at-time,
    displayed time and duration, and skipped-present statistics. ANGLE
    renders into the D3D11 presentation buffer through a client-buffer
    surface.
  - Windows 10: DXGI flip model, the swapchain the header's own, ANGLE
    rendering into its back buffer through a client-buffer surface, vblank
    timestamps from DXGI's frame statistics. ANGLE's sync-control extension
    failed on every frame of its own window surface (blt model).
  - Linux X11: GLX sync control on the native driver.
  - Linux Wayland: presentation-time through SDL's native handles.
  - macOS: whatever ANGLE's Metal backend exposes, measured before it
    is claimed.
- **Variable refresh.** A capability, never a default. Enabled only
  after a photodiode interval sweep and a luminance-versus-interval
  sweep on that panel. The header re-presents on purpose to stay inside
  the panel's range and logs each re-present, so the driver's
  low-framerate compensation never fires silently.
- **Input.** SDL events restamped onto the `ysp/rt.h` clock. The manual
  states that keyboard and mouse timestamps are at best
  millisecond-granular. On the Windows message path they count in the
  system tick: posted key events were stamped up to 15.4 ms early
  (docs/screen.md). The manual points at `ysp/serial.h` response
  boxes for reaction times.
- **Photodiode patch.** A corner patch the frame loop can toggle for the
  loopback test.
- **Multiple displays.** One handle per display, each with its own
  context, swap path, capability kind and flip record. A group present
  submits to every handle and then waits on every handle, on one
  thread per display where the swap blocks, so waits never serialize
  into a dropped frame. The log records each display's onset. Two
  displays flipping together is a hardware claim: only genlock on a
  workstation GPU guarantees it, and two photodiodes measure it.
  - Windows: one fullscreen window per display in independent flip,
    each composed at its own refresh. The composition swapchain
    presents to more than one display.
  - Linux X11: one window per output with its own sync-control
    counters, or one window spanning two outputs in the same mode with
    a single swap, which works only when the CRTCs are in step and is
    the one path to a synchronous dual display without genlock.
    Separate X screens (`:0.0`, `:0.1`, one per display) are a
    supported configuration too, and the recommended one for a stimulus
    display beside an operator display, as in Psychtoolbox: each screen
    has its own page flips and vblank counter, and the operator
    screen's compositor cannot touch the stimulus screen. The GLX
    backend takes the X screen by name (`desc.x_display`). Whether
    SDL3 creates a window and context on a screen other than the
    default must be checked on a real two-screen setup (reports from
    the SDL 2.0 era say it did not). If it does not, the backend opens
    its own X connection to that screen and owns the window and the
    GLX context there, as the Windows backends own their swapchain.
    Input on that screen then comes from XInput2 on the same
    connection, or only from the operator screen; reaction times use
    response boxes either way. Wayland has no equivalent: a display is
    chosen by going fullscreen on a `wl_output`, with presentation-time
    per output.
  - macOS: one window per display on its own display link, nothing
    synchronizes them.
  - Web: one window. A second display is out of scope.
- **Untimed screens** (proposal, 2026-10-06), for an operator view, a
  mirror of what the participant sees, or an eye-tracker overlay. An
  untimed screen is a `ysp/screen.h` handle opened with a kind that
  makes no timing claim:
  - it never blocks the frame thread: it presents without waiting
    (DXGI: `DXGI_PRESENT_DO_NOT_WAIT`; GLX: swap interval 0 on its own
    X screen) and drops frames freely, counting them;
  - it never joins a group's wait, and a group refuses it;
  - its records carry no onset and no tier;
  - it shows a scaled copy of a timed screen's scene (a texture shared
    on the same device, sampled after that screen's draw) or its own
    content, such as a UI.

  A second swapchain on the same GPU can disturb the timed display:
  on Windows the operator window adds composition work for DWM on a
  shared GPU. So before it is built, a measurement on two physical
  displays decides it: the timed display fullscreen in independent
  flip, the untimed window updating at the desktop rate on the other
  display, and the timed display's flip records (drops, residuals,
  path) with and without it. If the untimed screen disturbs the timed
  one, the manual names the condition, and the browser operator
  console (section 6) stays the recommended operator display.
- **Per-flip record** into the event ring: target, estimate, residual,
  dropped count, mode, and the phase durations of the frame: timeline
  evaluate, script callback, draw submission, texture upload, swap
  wait, and GPU time where the disjoint timer query extension exists.
  A dropped frame names the phase that ate the budget without any
  tool attached.

### 4.3 ysp/gfx.h (`ygfx_`, `YGFX_`)

Includes `ysp/screen.h`. GL ES 3.0 only. No compute stage.

Coordinates (decided 2026-10-05; the script API of section 6.1 uses the
same): screen pixels from the top-left corner, x right, y down; positive
angles turn +x toward +y, clockwise on the screen; degrees are one scale
factor with the same origin and axes. A stimulus has a `place` (the screen
point x and y are measured from, the center by default) and an anchor (the
point of its own box at x, y that it turns about, the center by default).

- **Backend interface** of about fifteen calls: create a pipeline from
  precompiled shaders, create and update a texture, set a uniform
  block, draw a quad, draw an instanced set, begin and end a pass to a
  float render target, present. Small enough that a second
  implementation is possible if ANGLE fails somewhere.
- **Stimulus set.** Rect, grating and gabor by shader, dot fields by
  instancing, image textures, text from glyph outlines (Slug, 5.2), noise
  and filtered noise by fragment passes on float render targets.
- **Shapes by signed distance.** Circle, annulus, rect, line, polygon,
  cross, aperture and mask as signed-distance functions in one
  fragment shader, with the edge profile as a parameter: hard,
  raised-cosine or Gaussian, with its width in pixels. The edge
  profile sets the spatial-frequency content, so the caller states it.
  Complex vector artwork is rasterized by the pack tool at the display
  resolution and drawn as a texture. No vector drawing library.
  The boundary (v0.3, 2026-10-05): closed-form distance functions
  (rounded and partial shapes, regular polygons and stars, ellipses), a
  bounded fold of at most 56 primitives, and paths of at most 224 points
  are inside it, with effects, dashes and gradients drawn from the same
  field. SVG, fill rules and a general path rasterizer are outside it.
  Artwork goes through the pack tool as Slug paths, the same form as
  glyph outlines (5.2). Changed 2026-10-06: "as a distance texture
  (MASK_TEX, MSDF or MTSDF)".
- **Output stage.** Gamma, CLUT, dithering for 10-bit and Bits#-style
  modes, in the final shader. SDL3 has no gamma ramps, so this is the
  only place gamma lives. Stereo on one display is an output mode
  here: side by side, interlaced, or frame-sequential with shutter
  glasses at 120 Hz. Quad-buffer stereo is a workstation GL feature
  that ES 3.0 does not have.
- **Calibration.** Measured primaries and gamma from a photometer become
  a 3x3 matrix into cone space (Stockman-Sharpe fundamentals), DKL and
  cone contrast, and a table from linear to device values. Changed
  2026-10-06: the calibration and the color math are `ysp/color.h`'s
  (`ycol_`, pure computation), which `ysp/gfx.h` includes from v0.5. It
  is a color header for calibrated displays, not a general color library:
  every conversion goes through a calibration, a color outside the gamut
  comes back unclipped with its distance, and gamut questions are calls of
  their own. No ICC profiles, appearance models or color-difference
  formulas (docs/color.md). Was: "No general color library".
- **Shader contract.** A user writes a fragment function body in GLSL ES
  against a fixed interface: a uniform block with time, resolution, the
  frame's predicted onset and a parameter array, plus fixed texture
  slots. The tool wraps it and precompiles it. ANGLE's own translator
  compiles it at runtime in dev mode. A user shader is the user's
  timing problem, and the flip log is where they find out.
- **Determinism.** GPU float arithmetic is not identical across vendors.
  A stimulus whose pixel values must be reproducible is generated on
  the CPU on the pump, or in the pack, seeded, and uploaded.
- **Parameter table** for the designer.
- **Added in v0.4.0** (2026-10-06): a program binary cache in storage the
  caller owns; NV12 and I420 textures converted to linear light by a
  stated encoding, and texture import for `ysp/video.h`'s GPU paths;
  instanced stimuli (a template and up to 16384 elements a frame, with a
  per-element hit test); draws reordered across kinds where they do not
  overlap, with identical pixels.
- **Added in v0.6.0** (2026-10-06): curve sets (the Slug format, version 1,
  that the pack tool's outline builder writes) and curve runs (glyphs and
  artwork layers as instanced items, exact area on resolved glyphs, rays
  elsewhere); a separable Gaussian blur pass for soft shadows, glow and
  blurred-letter stimuli (R16F, RGBA16F, RGBA32F, opt-in 2x). The pack no
  longer produces MSDF atlases; the MSDF path stays.
- **Added in v0.7.0** (2026-10-06): a rays-only option for pages of small
  text; static text cached in a target with the drawn run's coverage, bit
  for bit; Gaussian NOISE from a quantile table with an exact CPU twin;
  specialized vector programs for large rounded rects, dashed circles and
  circle compounds.
- **Added in v0.8.0** (2026-10-07): setup passes (targets rendered outside a
  screen frame, at setup); the rays program picked by itself for turned
  curve runs.
- **Changed in v0.9.0** (2026-10-07): the noise hash (triple32, one hash
  of a point's index under seed keys); every noise and dither pattern
  changed.
- **Added in v0.10.0** (2026-10-07): gradient noise (3D simplex fBm) with
  an exact CPU twin; not band-limited (filtered noise or gratings give a
  stated band).

### 4.4 ysp/audio.h (`yau_`, `YAU_`)

Needs miniaudio. The implementation block defines
`MINIAUDIO_IMPLEMENTATION` unless the translation unit already did.

Two halves behind one API. The scheduler and the synthesis render
into any callback that supplies a frame clock. The device backend is
miniaudio by default, and an audio device extension (section 12) can
replace it, for hardware miniaudio cannot open, such as a 64-channel
ASIO array. The scheduler then runs as a source inside the extension's
callback.

- **Project format.** The caller declares one rate, one channel count,
  one sample format and a channel map. 48 kHz is the default. 44.1, 96
  and 192 kHz and 24-bit are equal choices. The device opens at the
  declared format or the open fails.
- **Strict open.** Disable miniaudio's automatic sample rate conversion,
  read back the device's internal rate after init, and fail on a
  mismatch with a message that names the OS rate. Exclusive mode is a
  `desc` field.
  Changed 2026-10-05 (from the miniaudio 0.11.25 source): with a nonzero
  rate, miniaudio asks WASAPI shared mode to resample and then reports
  the requested rate, so a read-back cannot see it; the header asks for
  no format, rate or channel count and refuses what the OS gives when it
  differs. Exclusive mode does not fix a rate mismatch: miniaudio opens
  it at the endpoint's Default Format, the same rate as shared mode.
  Another exclusive rate needs a WASAPI Audio device extension.
- **Schedule.** `yau_play_at(buffer, t)` on the `ysp/rt.h` clock. The
  callback counts frames from an anchor correlated to the clock and
  starts the buffer on the right frame.
- **Master clock.** The `ysp/rt.h` clock is the master. The header fits
  the device clock against it and plans every onset in device frames from
  the fit. It never resamples and never inserts frames. A long buffer's
  end follows the device clock and its record says where it ended.
  Changed 2026-10-05: the first plan nudged the stream by resampling or
  frame insertion, against principle 5. Locking a movie's soundtrack to
  its video goes to `ysp/video.h`, which follows the audio fit with the
  movie base.
- **Synthesis.** Tones, noise, envelopes, at the project rate.
- **Latency.** Reported only as measured by the line-out to line-in
  loopback test.
- **Per-onset record** into the event ring.
- **Streams.** Added 2026-10-06 (v0.2.0): a ring-fed voice that is
  played, planned, recorded and confirmed as a buffer is. Its sample s
  plays on stream frame origin + s. A late start or a ring underrun skips
  samples and does not move origin. A movie's soundtrack is a stream, so
  movie sound takes the same path as every other sound, and `ysp/video.h`
  follows origin. WAV files (RIFF, RF64, BW64) stream through it.

### 4.5 ysp/timeline.h (`ytl_`, `YTL_`)

Includes nothing. Caller-sized fixed arrays, no heap.

- **Events.** Onset, offset, trigger, at a time on a chosen time base:
  the `ysp/rt.h` clock, a trial clock, or a movie clock.
- **Tracks.** Keyframes with easing on any float channel: position,
  contrast, opacity, spatial frequency.
- **Evaluate at the predicted onset.** One call takes the predicted
  onset of the frame being drawn, fills a state struct and reports
  which events fired. It never samples the current time.
- **Quantization.** An event lands on a frame, by default the nearest
  frame to its time. The evaluate call reports the frame and the
  residual.
- **Replay.** Deterministic from the same inputs and the same frame
  times, with a record of where every event landed.
- Composes with `ysp/trials.h`: the trial handler picks the condition,
  the timeline runs it. At movie scale the arrays hold thousands of
  annotations.

#### 4.5.1 C authoring API

Status: built in `ysp/timeline.h` v0.4.0 (2026-10-06), revised from the
2026-10-04 draft after a review (docs/timeline.md, "Sequences"). The
draft lowered each `ytl_to` to `ytl_tween()`. A channel holds one
waiting tween, so the draft trial below lost its first ramp, and a
re-anchor did not replay tweens. The goal is for C code to be about as
short as a JS animation library, GSAP or anime.js, with the timeline's
timing unchanged. The builder is in the same header. A C trial and a Luau
trial read alike (section 6.1).

The trial from the `ysp/timeline.h` USAGE section, in GSAP:

```js
gsap.timeline()
  .set(fix, {visible: 1})
  .set(fix, {visible: 0}, "+=0.5")
  .set(grating, {visible: 1}, "<")
  .call(trigger, [12], "<")
  .to(grating, {contrast: 0.5, duration: 0.1, ease: "sine.inOut"}, "<")
  .to(grating, {contrast: 0, duration: 0.1, ease: "sine.inOut"}, "+=0.5")
  .set(grating, {visible: 0});
```

The same trial with the builder (C99):

```c
ytl_seq q = ytl_seq_on(&tl, TRIAL);          /* cursor at base time 0 */
ytl_on(&q, FIX);
ytl_wait(&q, YRT_MS(500));
ytl_off(&q, FIX);
ytl_on(&q, GRATING);
ytl_trigger(&q, 12);
ytl_to(&q, CONTRAST, &(ytl_tween_desc){
    .to = 0.5f, .duration_ns = YRT_MS(100), .ease = YTL_EASE_COSINE });
ytl_wait(&q, YRT_MS(600));
ytl_then(&q, CONTRAST, &(ytl_tween_desc){
    .to = 0.0f, .duration_ns = YRT_MS(100), .ease = YTL_EASE_COSINE });
ytl_off(&q, GRATING);
if (q.err < 0) report(ytl_strerror(q.err), q.err_call);
```

The rules:
- **A cursor.** `ytl_seq` holds a base and a cursor in base time.
  `ytl_seq_on()` puts the cursor at 0. `ytl_seq_now()` puts it at the
  base's "now", the time a tween desc uses when `start_set` is false.
  `ytl_wait(&q, dt)` moves the cursor by `dt`, and `ytl_at(&q, t)`
  sets it. `q.t` is the cursor: store it in a variable to use it as a
  label.
- **Events and tweens at the cursor.** `ytl_on`, `ytl_off`,
  `ytl_set_value`, `ytl_trigger` and `ytl_mark(&q, code, user)` add
  an event at the cursor. `ytl_to` starts a tween at the cursor plus the
  desc's `delay`, and the cursor does not move. That is the script's
  non-blocking `tween()`. A desc with `start_set` is refused: the cursor
  is the start.
- **One keyed track per channel.** A sequence's tweens on a channel
  become one keyed track in the key arena, which grows with each call.
  The track waits for its first start and then takes over from the old
  driver, as `ytl_tween()` does. After that it is a function of base
  time, so re-anchoring a trial replays it. A tween that starts inside
  the channel's previous one is refused, and so is `YTL_FOREVER`.
- **Sequencing.** `ytl_then` is `ytl_to` and then a wait to the
  tween's end, which is the default placement of GSAP's `.to()`.
  `ytl_to` returns the tween's end time, so
  `ytl_at(&q, ytl_to(...))` does the same thing explicitly.
- **A sticky error, as in a stream.** The first failing call stores its
  code in `q.err` and its index in `q.err_call`. Every call after that
  does nothing. So a sequence needs no check after each line, only one
  at the end. That is the C analog of a chained JS call. The builder is
  not atomic: the calls before the error stay. `ytl_seq_cancel(&q)`
  removes what the sequence added.
- **Keyframes and stagger:** `ytl_keyframes(&q, ch, values, offsets, n,
  ease)` joins the channel's track in the arena, and
  `ytl_on_n(&q, chans, n, stagger)` adds onsets all or none.
- **Nothing here changes timing.** Every call lowers to events and keys at
  absolute base times. Frame placement, sampling at the onset and replay
  are the core's.

##### The same sequence as data

There is no method chaining. In C, a call that returns its receiver
chains only by nesting, which reads from the inside out. Function
pointers in the struct still repeat the receiver, prevent inlining and
are hard to follow in a debugger. Chaining in JS gives three things, and
C gets each one in another way:
- One error path for the sequence: the sticky `q.err` above.
- No repeated receiver: `&q,` on each line is the cost, and that is
  ordinary C (sokol, raylib).
- A whole timeline as one readable unit: an op array, below.

A sequence that is known when the code is written is a table of ops.
Thin macros over designated initializers build the ops. `ytl_run()`
gives each op to the same builder calls, in order, with a sticky error:

```c
static const ytl_op trial[] = {
    YTL_ON(FIX),
    YTL_WAIT(YRT_MS(500)),
    YTL_OFF(FIX), YTL_ON(GRATING), YTL_TRIGGER(12),
    YTL_TO(CONTRAST, .to = 0.5f, .duration_ns = YRT_MS(100), .ease = YTL_EASE_COSINE),
    YTL_WAIT(YRT_MS(600)),
    YTL_THEN(CONTRAST, .to = 0.0f, .duration_ns = YRT_MS(100), .ease = YTL_EASE_COSINE),
    YTL_OFF(GRATING),
};

ytl_seq q = ytl_seq_on(&tl, TRIAL);
ytl_run(&q, trial, YTL_COUNT(trial));
if (q.err < 0) report(ytl_strerror(q.err), q.err_call);   /* the op's index */
```

An op is a tagged struct:

```c
typedef struct ytl_op {
    int              op;      /* YTL_OP_ON, _OFF, _SET, _TRIGGER, _MARK,
                               * _WAIT, _AT, _TO, _THEN                      */
    int              ch;
    int64_t          t;       /* WAIT: dt; AT: base time                    */
    int32_t          code;    /* TRIGGER, MARK                              */
    float            value;   /* SET_VALUE                                  */
    uint64_t         user;    /* MARK                                       */
    ytl_tween_desc tween;   /* TO, THEN                                   */
} ytl_op;                   /* 96 bytes                                   */

#define YTL_ON(c)      { .op = YTL_OP_ON, .ch = (c) }
#define YTL_WAIT(dt)     { .op = YTL_OP_WAIT, .t = (dt) }
#define YTL_TO(c, ...)   { .op = YTL_OP_TO, .ch = (c), .tween = { __VA_ARGS__ } }
```

Why this form, and not only the builder:
- **It is data.** The table can be `static const`, so it can live in
  read-only memory. It can also be logged and compared with a diff. It
  has the shape of the pack's experiment definition (principle 6), so
  the eject path emits it directly, and a table from the pack can be
  given to `ytl_run()` without a compile step.
- **No new semantics.** `ytl_run()` is a loop over the builder calls,
  so the two forms mix freely in one seq. A static table can set up a
  trial, and builder calls can then add the computed parts, such as a
  staircase level.
- **Precedent.** Modern C single-header libraries use this style: sokol
  passes descs as compound literals, and the Clay layout library builds
  whole UIs from macros over designated initializers.

Measured (a stand-in for `ytl_op` with the real `ytl_tween_desc`,
in a scratch program, 2026-10-05):

| Build | Result |
|---|---|
| gcc 11.4, C99, `-Wall -Wextra -Wpedantic -Werror` | Clean; the table is a constant initializer |
| MSVC 19.44, default C dialect, `/W4 /WX` | Clean |
| MSVC 19.44, `/std:c++20 /W4 /WX` | Clean |
| g++ 11.4, `-std=c++20 -Wall -Wextra -Werror` | 44 `-Wmissing-field-initializers` errors: g++ 11 warns about omitted fields even with designated initializers. Clean with `-Wno-missing-field-initializers` |
| g++ 11.4, `-std=c++17` | Fails: no designated initializers before C++20 |

The costs:
- A mistake inside a macro's `__VA_ARGS__` gives a worse compiler
  message than the same mistake in a plain call. One thin macro for
  each op and no logic in the macros keep the message short.
- In C++20, designated initializers must follow declaration order:
  `.to` before `.duration_ns` before `.ease`, the order of
  `ytl_tween_desc`. C does not require this.
- C++17 users use the builder.

The header's own compile checks are `tests/compile/timeline.c` (the
table as C) and `tests/compile/timeline_ops.cpp` (as C++20, with
`-Wno-missing-field-initializers` on g++; g++ 16.1 still warns there).

Decisions (2026-10-05):
- **`ytl_on` and `ytl_off`, not show and hide.** The header does not
  know what a channel means. The script layer keeps `show` and `hide`
  for stimuli, and they lower to on and off. The ops are `YTL_ON` and
  `YTL_OFF`, as in the examples above.
- **Audio is not frame-quantized.** An on or off on a channel that the
  player maps to a sound goes to `yau_play_at()` at the event's own
  time, sample-accurate. It must also be handed over before that time,
  because the audio path needs lead time. An evaluate fires an event on
  the frame that shows it. That is too late and too coarse for sound. So
  the timeline needs a look-ahead query: give the pending events of a
  base whose RT time falls in a window, without firing them:
  `ytl_peek(tl, base, from_rt, to_rt, out, cap)`, built in
  ysp/timeline.h v0.3.0 (2026-10-05). The
  player's audio path schedules from it, and the video path keeps
  evaluate. The event's landing record stays the video frame's. The
  audio onset record comes from `ysp/audio.h` (section 4.4). Which base
  and channel kinds are audio is the player's choice, not the header's.
- **The key arena is in the timeline handle.** `desc.keys` and
  `desc.key_capacity` give caller-owned storage, as `desc.events` does,
  with no default (NULL: a sequence tween is `YTL_ERR_FULL`). Each
  tweened channel has one block, and the channel stores an offset, not a
  pointer. A replaced block, or one whose base `ytl_clear()` cleared, is
  garbage until a call needs its room; then one pass compacts the arena.
  A call that does not fit is refused and adds nothing. The arena is not
  in the seq: a seq is a short-lived cursor, often on the stack, and the
  keys must live until the track is replaced or its base is cleared,
  which only the timeline knows.
- **No C++ sugar for now.** C++17 users use the builder. A chained C++
  wrapper is not planned.
- **g++ and `-Wmissing-field-initializers`.** The C++20 compile check of
  the op macros builds with `-Wno-missing-field-initializers`. The
  macros keep zero means default.
- **`ytl_run()` checks every table again.** A table that the designer
  checked at import can still be wrong in the player: the player can be
  a different version from the designer, an extension that an op needs
  may not be installed, the parameter tables and channel counts may have
  changed, or the rig's display or audio rate may make an op invalid,
  for example a flicker rate. The check runs once per run, not per frame.
  Binding is checked against the first earlier op naming the channel,
  O(n^2) compares with no scratch table. It is all or nothing, as
  `ytl_add_n()` is: the whole table is checked before the first op is
  applied, so a bad op at index 37 does not leave half a trial.
  `ytl_check_ops(tl, base, ops, n, &bad)` does the same check without
  applying, so the player can reject a pack when it loads, before the
  session starts, not at trial 190.

Open questions:
- How far ahead must `ytl_peek()` look for audio? That is the audio
  path's measured scheduling lead plus one frame. ysp/audio.h v0.1.0
  measured the shortest lead with no late onset at 43.5 to 54.5 ms
  (WASAPI shared, 10 ms periods, this laptop; docs/audio.md). The
  line-in loopback test is still needed for the output latency itself.
- Answered (v0.4.0): the arena has no default. An inline arena would grow
  every handle, and the handle is already 64 KB.

### 4.6 ysp/video.h (`yvid_`, `YVID_`)

Includes `ysp/screen.h`. Uses SDL3 for capture.

- **Scheduler.** At each flip, pick the frame whose presentation
  timestamp is due against the `ysp/rt.h` clock. Drop or repeat as
  needed. Never count frames. Log every decision.
- **Decode-ahead** on the `ysp/rt.h` pump, N frames ahead, texture
  upload on the frame thread.
- **Backend interface:** open, decode the next frame with its
  timestamp into caller-owned planes, seek.
- **Backends**, in order of implementation:
  1. Frame sequence: a flat container of raw or QOI frames with an
     index. Read and write. The value-exact path.
  2. pl_mpeg: MPEG-1 and MP2, software decode.
  3. Media Foundation source reader (Windows) and AVFoundation asset
     reader (macOS): hardware H.264 and HEVC to NV12 with timestamps.
     Required by feature-length naturalistic paradigms.
  4. FFmpeg: opt-in, the Linux route for the same.
- **Seek and resume.** Seek to the previous keyframe and decode forward
  silently. Frame-accurate, checked against the log.
- **YUV to RGB** in the output shader, with the manual stating that
  codecs are lossy in luminance and chroma and that value-exact
  stimuli use the frame sequence.
- **Capture.** SDL3 camera frames restamped onto the `ysp/rt.h` clock,
  with the current trial and event attached, written to the frame
  sequence container or to an opt-in encoder (minih264 with minimp4,
  Media Foundation sink writer, AVFoundation asset writer). Encoding
  runs on the pump. The manual states that the camera timestamp is
  delivery time, not exposure time, and that the LED loopback measures
  the offset.
- **Per-frame record** into the event ring: movie timestamp, flip time,
  decision.

### 4.7 ysp/net.h (`ynet_`, `YNET_`), later

UDP triggers and clock-offset estimation between two machines. Lab
Streaming Layer markers through liblsl as an opt-in backend.

### 4.8 Not planned

- HMD virtual reality. An OpenXR runtime owns the compositor: the
  application asks for a predicted display time, renders for it and
  hands the frame back, and the runtime reprojects, drops or repeats
  as it decides. Present-at-time does not exist there and no timing
  claim can be the header's. If a `ysp/xr.h` ever appears, OpenXR is
  the route and `ysp/timeline.h` carries over unchanged, because
  evaluate-at-predicted-onset is the OpenXR frame loop. `ysp/screen.h`,
  the swap paths, stereo rendering, tracking and controllers do not
  carry over and are a separate project.
- Offline psychometric fitting. R, Python and MATLAB do it better and
  analysis happens there.
- A logging library. The event ring in `ysp/rt.h` is the primitive.
- A general random-number header. `ysp/trials.h` owns the only draw.

Removed from this list 2026-10-06: "a standalone color header". It is
`ysp/color.h` (4.3, Calibration).

## 5. The pack

One zip file in store-only mode, entries aligned to 4 KB, so anyone can
open it with anything, and a browser can fetch the central directory
from the tail and then only the entries it needs by HTTP range. A
two-hour film streams by byte range. Compression is per resource, never
on the container. The pack can be appended to the player executable for
a single-file deploy on Windows and Linux.

### 5.1 Manifest

A format version, and per entry: a checksum, the source file's hash,
the tool version, and the exact derivation including any ffmpeg command
line. The runtime refuses a format version it does not know. A methods
section can cite the manifest and a reviewer can rebuild the pack.

### 5.2 Canonical resource forms

| Resource | Canonical form | Notes |
|---|---|---|
| Texture | Uncompressed 8-bit, 16-bit or float planes, optionally QOI or a single-file deflate or LZ4 | Block compression opt-in, recorded in the manifest. |
| Video | Constant frame rate, fixed pixel format and resolution, closed GOPs, no B-frames, plus an index of frame to byte offset and timestamp | Produced by ffmpeg, command recorded. |
| Audio | The project rate and channel count; 16-bit, 24-bit or float samples | Resampled offline with a high-quality resampler, recorded. In memory always float, widened exactly at load (2026-10-05): one mixer path, and float holds 16- and 24-bit values exactly. |
| Font | The font file, plus the glyph outlines built from it in the Slug form (quadratic curves and bands; cubic outlines converted within a stated tolerance), and alpha coverage at the sizes the experiment uses for value-exact text. Static text is stored as laid-out glyph runs. | No platform font engine. One pinned Skribidi does layout, bidirectional text and shaping in the designer (WebAssembly, with editing), the pack tool and the player, so text is the same everywhere. Its sources are vendored at a commit with named local patches (a bidi run-merge fix, docs/layout_probe.md; rule L1 per line, docs/layout.md), built without FetchContent; HarfBuzz is pinned by tag separately; the pack manifest records the Skribidi commit, the patch hashes and the HarfBuzz version. In the pack tool HarfBuzz is also the outline source for variable-font instances and CFF2, which ysp/outline.h refuses; its curve sets equal ysp/outline.h's on static fonts. Compared and not taken (2026-10-07, docs/layout_probe.md): kb_text_shape (no paragraph layout or editing, unsafe on untrusted fonts), HarfBuzz's hb-gpu renderer (1 to 3 times the rays' edge error unturned, 24 to 51 times turned at 200 px), and Rust layout engines (no Rust dependency). Scalable text is drawn from the outlines by the Slug algorithm (Lengyel, JCGT 2017; patent dedicated to the public domain 2026-03-17; reference shaders need attribution). A whole font fits in the pack (about 65 MB for a CJK font, against 212 to 762 MB as MSDF atlases), and a glyph missing from it is built from the font's outline in 20 to 120 us, resolved (ysp/outline.h, docs/outline.md; the probe's 17 to 66 us did not remove overlaps), so the player rasterizes nothing at run time. Value-exact alpha coverage comes from an exact-area rasterizer in the pack tool, not Skribidi's (max error 0.24 to 0.32 of full scale), and is drawn on the pixel grid only. Effects: outlines and faux bold by stroke expansion of the curves, filled the same way; hard shadows by an offset copy; soft shadows, glow and blurred text by a Gaussian blur pass on a render target (numbers in docs/text_probe.md). Headers only draw glyph runs. Changed 2026-10-06: MSDF atlases dropped from the pack, after a probe on the Iris Xe (docs/text_probe.md; MSDF failed on dense CJK and overlapping contours, and a distance soft edge is a true blur only on straight edges). Changed 2026-10-05: "glyph atlas plus metrics; no font engine at runtime". |
| Artwork | Filled paths in the Slug form, one solid color per layer, with the fill rule; strokes expanded to fills in the tool | The tool reads a subset of SVG: paths, basic shapes, transforms, solid fills and strokes. Filters, text, CSS, masks and gradients are refused by name, not ignored. Decided 2026-10-06, replacing MSDF artwork; the subset grows only when an experiment needs it. |
| Shader | The user's fragment body wrapped in the contract (`ygfx_shader_wrap()`, the same function in the tool and the runtime), as GLSL ES 3.00 text, with reflection | glslang validates it in the tool. GL ES 3.0 has no portable binary, so ANGLE compiles the text on the rig when the pack loads; `ysp/gfx.h` caches each program's binary per ANGLE build, adapter and driver (`desc.cache`, v0.4.0), checked by its own hash before the driver sees it. SPIRV-Cross is for a second backend only. Changed 2026-10-05: "D3D bytecode is finished by the runner" (docs/gfx.md). |
| Table | Typed binary columns from condition files and from ELAN, Praat or BIDS events exports | Read directly by `ysp/trials.h` and `ysp/timeline.h`. The form is `ysp/table.h`'s PSTB block (v0.1.0, 2026-10-07; docs/table.md): position independent, hashed, int32, double or string columns with levels; the CSV parser in the header builds the same bytes the pack tool stores. |
| Calibration | The 3x3 matrix and the gamma table, with the photometer readings beside them | `ysp/color.h`'s `.yspcal` file (62528 bytes, CRC-32; the same bytes as `ysp/gfx.h` v0.4's), rebuilt from its readings on load. A participant's luminance from flicker photometry is session data, not pack data: `ysp/color.h`'s `.ysplum` file (changed 2026-10-06). The display calibration is the rig's, independent of every pack; a `.yspcal` in a pack is for preview and simulation only, and the player never applies it on a rig (decided 2026-10-09, docs/pack.md 4.2). |
| Experiment | The definition the player interprets | See section 6. |

The canonical in-memory forms are the same as the on-disk forms (audio
excepted: integer samples are widened exactly to float at load), so a
stimulus generated per trial on the pump enters through the same API
as one loaded from the pack.

## 6. The player

A prebuilt program per platform, and a WebAssembly build, that loads a
pack and interprets the experiment definition. Deploy is player plus
pack. Web deploy is `player.wasm` plus pack. No compile step in the
common case.

- **Runner protocol.** A localhost WebSocket with a session token
  carrying control, resource updates, live parameters, status and the
  log stream. The designer, a future desktop shell, the operator
  console and hot reload are all clients of it.
- **Hot reload.** A resource or parameter update applies immediately in
  preview, between trials in a live session, and never mid-trial. The
  log records that a change applied and what it was.
- **Dev mode.** Runtime shader compilation through ANGLE's translator.
  Off in a deployed player.
- **Profiling.** The player is built with Tracy in on-demand mode, so
  nothing is buffered and no port is opened until a session flag turns
  it on. The operator console plots the per-flip phase durations live
  over the runner protocol. Tracy's capture and CSV export tools give
  the text form, and the loopback tests save a capture beside their
  photodiode data.
- **Trace export.** The event ring's drain writes Chrome trace event
  JSON. One file opens in Chrome DevTools, in Perfetto and in Tracy
  through its Chrome import tool, so a browser session and a native
  rig produce the same artifact.
- **Web player instrumentation.** Under Emscripten the macro layer maps
  zones to the User Timing API and frame marks to console timestamps,
  so DevTools shows them beside the compositor, the GPU, long tasks
  and garbage collection. Builds carry DWARF debug info so a zone
  links to its C line. A session self-test at start records the
  measured refresh, the clock resolution, cross-origin isolation, the
  GPU timer query's availability and fullscreen state, and the log
  records every visibility change. The web player posts its trace and
  log to the designer over the runner protocol. Timing on a
  participant's machine is measured and excluded, never controlled,
  and the exclusion rule is written before data collection.
- **Web backend settings.** A desynchronized WebGL context, the
  high-performance power preference, and Web Audio's scheduled start on
  the audio clock with its output timestamp for the correlation.
- **Scripting.** Between the data definition and the eject path, an
  embedded script (Luau, proposed) carries the logic the designer's
  vocabulary cannot: response-contingent branching, computed stimulus
  parameters, custom trial flow. Rules:
  - Scripts live in the player. The headers stay C.
  - The script API is generated from the headers' parameter tables,
    the same source as the designer's panels.
  - Sandboxed: no file access outside the pack, no network, no wall
    clock. Time comes from the `ysp/rt.h` clock through the API and
    randomness from the trial handler's draw, so a replay reproduces.
    A pack from another lab is safe to run.
  - Compiled to bytecode at import, so syntax errors surface in the
    designer.
  - A per-frame callback has a stated budget, and the flip log shows
    when a script blew it. Collector pauses count against it.
  - A data-only experiment has no script. Nobody learns a language to
    run constant stimuli.
- **Script concurrency model.** Coroutines, no async syntax. A call
  that conceptually blocks yields the coroutine to the player's
  scheduler. Lua coroutines are stackful, so a wait works from any
  depth of ordinary function calls and helpers can wait inside
  themselves. The scheduler lives in the player, a few hundred lines,
  and follows these rules:
  - `wait(dt)` targets the previous scheduled point plus `dt`, then
    the frame whose predicted onset is nearest to it, which is the
    timeline's default half-frame lead. Waits chain from scheduled time,
    never from resume time, so sequences do not drift. "At or after" was
    the first rule. It was dropped because a predicted onset carries
    noise, and with 2 us of noise it put about 12% of frame-aligned
    targets a frame late (docs/timeline.md). A wait that must never
    resume early uses the timeline's never-early lead.
    `wait_until(t)` and `wait_frames(n)` exist beside it.
  - Resumes happen at one point in the frame, on the frame thread,
    before the timeline evaluates and before drawing, with the
    predicted onset available. Script time counts against the frame
    budget in the flip record.
  - Everything awaitable is a handle the C side completes: a presented
    flip, an audio onset, a response, a video seek, a streaming load.
    `await(h)` yields until it resolves and returns its measured
    values. `race`, `all`, `timeout` and `repeat` compose handles. A
    response window is `race(input.key(...), wait(2.0))`.
  - Cancellation is `coroutine.close`. An abandoned branch of a race
    releases its handle.
  - Coroutines resume ordered by target time, then creation order, so a
    replay with the same frame times reproduces the sequence.
  - An error aborts the trial with a traceback into the log. The
    experiment definition says whether the session continues.
  - A wait is a timeline event with a coroutine attached and a show is
    an onset event, so the declarative timeline and the script are one
    mechanism. A script can post timeline events directly, a tween on
    contrast for example.
  - Luau yields from C functions but has no continuations, so resume
    values return to the Lua caller and a C wait runs no code after
    the resume. Pack resources are preloaded, so loading needs no
    await except for streaming.
- **Eject path.** The designer can emit ordinary C that calls the
  headers, for users who need custom logic, built with the bundled zig.
  Custom native modules load into the player through a defined
  interface.
- **The player is not an engine.** It is a few headers around a frame
  loop whose flip times, onsets and trigger edges are measured and
  logged.
- **The runner gives the player the foreground** (decided 2026-10-10).
  `ysp/screen.h` v0.5.0 settles at open and fails a fullscreen open
  strictly when the OS gives no vblank statistics, which is what a
  covered or background window gets (5 of 5 DXGI_FLIP and 8 of 8
  COMPOSITION covered opens failed; v0.4.4 opened them and then gave
  seconds of off-grid or missing onsets). From v0.5.1 a fullscreen open
  forces the raise by default (`desc.foreground`), and the runner still
  grants the foreground before it starts the player
  (`AllowSetForegroundWindow`, or it runs in the interactive session),
  so a launch from the designer's Run view, a scheduler or a remote
  shell settles like a launch from a terminal. `YSP_SETTLE=warn` is the
  development escape, and it is recorded in the data file.

### 6.1 Script API, draft

Status: draft, 2026-10-04. Nothing here is implemented. It proposes the
API that scripts see. Section 6 above gives the concurrency model.

The goal is the brevity of GSAP and Phaser with the timing of
`ysp/timeline.h`. Every call lowers to timeline events, tracks and
tweens. The script does not have a second animation system.

#### Rules

1. **Time is the scheduled point, in seconds.** `now()` is the
   coroutine's scheduled time on the trial clock, not the time at which
   the coroutine resumed. `wait(dt)` moves it forward by `dt`. A call
   that takes no time argument acts at `now()`. So
   `show(a); wait(0.5); hide(a)` puts the offset exactly 0.5 s after the
   onset, and the timeline puts each one on its nearest frame.
2. **Trial time 0 is the first frame of the trial.** The player anchors
   the trial base there and clears it after the trial. A script never
   anchors a base. Session-level timing uses `session.now()` on the RT
   base.
3. **A tween does not block.** `tween()` returns a handle at once, as
   GSAP's `to()` does. `await(h)` blocks. If you do not await, things run
   in parallel. If you await, they run in sequence. No other sequencing
   syntax is needed.
4. **Properties come from the parameter tables.** `g.contrast`, `g.x`
   and `g.sf` are the names in `ysp/gfx.h`'s table for the stimulus type,
   with the table's units. The Luau type checker knows them, so a wrong
   name fails in the designer.
5. **One driver per channel is the player's business.** Each stimulus's
   `visible` is driven by events, so each onset and offset has a record
   of the frame it landed on. Every other property is driven by a track.
   Assigning to it (`g.contrast = 0.3`) is a tween of length 0 at
   `now()`. A script never sees `YTL_ERR_BOUND`.
6. **Reading a property gives the value of the last evaluated frame.**
   This is what the participant saw on that frame. It is deterministic
   under replay.
7. **The script has no per-tween callbacks.** `await` replaces
   `onComplete`. A track or a sampled table replaces `onUpdate`. The
   escape hatch is `every_frame(fn)`, which has a stated time budget
   (section 6).

#### Examples

The trial from the `ysp/timeline.h` USAGE section:

```lua
function trial(c)
  show(fix)
  wait(0.5)
  hide(fix); show(g); trigger(12)
  tween(g, {contrast = c.contrast}, 0.1, {ease = "cosine"})
  wait(0.6)
  await(tween(g, {contrast = 0}, 0.1, {ease = "cosine"}))
  hide(g)
end
```

A response window, with feedback that depends on the response:

```lua
function trial(c)
  show(g)
  local r = await(race(input.key{"f", "j"}, wait(2.0)))
  hide(g)
  if r.key == nil then mark("miss") return end
  local ok = (r.key == "f") == c.left
  show(ok and tick or cross); wait(0.3); hide(ok and tick or cross)
  return {key = r.key, rt = r.t - r.window_start, correct = ok}
end
```

A 10-minute tracking block with a 7.5 Hz flicker on the target:

```lua
function trial(c)
  show(target)
  follow(target, {x = pack.table("sos_x"), y = pack.table("sos_y")})
  flicker(target, {hz = 7.5, duty = 0.5, cycles = 4500})
  wait(600)
  hide(target)
end
```

A staggered onset of 50 dots, 20 ms apart, then all of them fade out
together:

```lua
show(dots, {stagger = 0.02})
wait(2.0)
await(tween(dots, {opacity = 0}, 0.25))
```

#### Reference

Time:

| Call | Effect |
|---|---|
| `now()` | Scheduled trial time, in seconds |
| `wait(dt)` | Advance `now()` by `dt` and yield until the frame nearest to it |
| `wait_until(t)` | Set `now()` to `t` and yield the same way; an error if `t` is before `now()` |
| `wait_frames(n)` | Yield for `n` frames; set `now()` to the trial time of the frame it resumes on |
| `frame()` | The current frame: `index`, `onset` (predicted, trial s) and `period` |

Stimulus events. Each call takes an optional `{at = t}`, which defaults
to `now()`. When `s` is a list, each call also takes `{stagger = dt}`.
Each call returns a handle. `await` on the handle returns the record:
`frame`, `onset` and `residual`.

| Call | Effect |
|---|---|
| `show(s)`, `hide(s)` | Onset and offset of `s.visible` |
| `trigger(code)` | A trigger code at that time; the trigger output sends it at the flip |
| `mark(name, value)` | An annotation in the log; `value` is optional |

Tweens and tracks. A target is a stimulus or a list of stimuli. `props`
maps property names to end values. Each call returns one handle for all
the channels it drives.

| Call | Effect |
|---|---|
| `tween(target, props, duration, opts)` | From the current value to each end value |
| `props` value `{from = a, to = b}` | From a given start (GSAP `fromTo`) |
| `props` value `by(d)` | To the current value plus `d` |
| `opts.ease` | `"linear"` (default), `"step"`, `"cosine"`, `"quad_in"`, `"quad_out"`, `"log"`, or `bezier(x1, y1, x2, y2)` |
| `opts.delay` | Start at `now() + delay` |
| `opts.repeat`, `opts.yoyo` | Repeat the tween; `yoyo` also plays it back to the start |
| `opts.stagger` | With a list: start each one `stagger` later than the one before. A table `{each = dt, from = "first" \| "last" \| "center" \| i, grid = {w, h}, ease = name}` orders the delays by distance from `from`, on a grid when `grid` is given, and spreads them with `ease` (anime.js's stagger) |
| `props` value `{a, b, c}` | Keyframes: a keyed track through the values, evenly spaced over `duration`, or at `opts.times` (fractions of `duration`, rising, 0 to 1) |
| `props` value `function(i, s)` | A value for each target of a list, from its index and stimulus |
| `opts.keep_velocity` | Start from the current value and the current velocity, and come to rest at the end value, with no step in velocity. For a moving target that is retargeted in mid-flight (smooth pursuit) |
| `follow(target, props, opts)` | Drive each property from a pack table (a sampled track), starting at `now()`. `opts.interp` is `"cubic"` (default), `"linear"` or `"step"` |
| `flicker(target, opts)` | A square wave on `target.gate`, a 0-or-1 multiplier in the stimulus shader. `opts.hz`, `opts.duty` (0.5 by default), and `opts.cycles` (forever by default) |
| `target.prop = v` | A tween of length 0 at `now()` |
| `stop(h)` | End the tracks of handle `h` at their current values |

Composition, as section 6 gives it: `await`, `race`, `all`, `timeout`,
`repeat`. Responses, as section 6 gives them: `input.key{...}`,
`input.button{...}` and serial lines through `ysp/serial.h`.

#### Lowering

Each call is one or more `ysp/timeline.h` calls on the trial base. Times
go to nanoseconds by `ytl_ns()`, rounded to nearest.

| Script | Timeline |
|---|---|
| `show(s)` / `hide(s)` | `ytl_add` ONSET / OFFSET on `s.visible` |
| `trigger(code)` | `ytl_add` TRIGGER, `code` |
| `mark(name, v)` | `ytl_add` MARK, `code` = name id, `user` = `v` |
| `wait(dt)` | A scheduler entry at the target time, resumed by the window rule of the timeline; logged as a MARK |
| `tween(...)` | `ytl_tween` per channel, with a `ytl_tween_desc` |
| keyframes `{a, b, c}` | `ytl_keyframes` on a sequence, with the keys in the timeline's key arena (`desc.keys`, section 4.5.1) |
| `stagger` | The player computes each target's `start`; the timeline sees ordinary tweens and events |
| `opts.repeat`, `opts.yoyo` | `ytl_set_track` keyed, with `period` and `repeats`; `yoyo` adds a key back to the start |
| `follow(...)` | `ytl_set_track` sampled, with the table's rate from the manifest |
| `flicker(...)` | `ytl_set_track` with two STEP keys and a repeat |
| `s.prop = v` | `ytl_tween` with length 0 |
| reading `s.prop` | `ytl_value` |

#### C additions this needs

- Done in v0.2.0: `ytl_tween()` takes a `ytl_tween_desc` with
  `from`, `start` or `delay`, `cycles`, `yoyo` and `keep_velocity`, and
  takes over from the old track's value at its start.
- Done in v0.4.0: `ytl_window(tl, base, &frame, &end)` gives the base
  time that a frame's window reaches, from the same function the evaluate
  uses. The scheduler resumes a wait on the frame where `target <= end`,
  so the player does not copy the rule. `ytl_rt_time()` gives the RT
  time of a base time, to lower `now()` to an RT change point.
- Dropped (v0.4.0): constructors `ytl_onset()`, `ytl_trigger()`,
  `ytl_keys()`, `ytl_samples()`. The sequence builder of section 4.5.1
  and designated initializers serve the same users, and the builder now
  owns the name `ytl_trigger`.

#### Left out, and why

- **Easings that overshoot** (back, elastic, bounce): an overshoot of a
  stimulus value is a confound. `bezier()` with y outside [0, 1] stays
  possible on purpose.
- **Time scale, reverse, seek inside a trial:** a rate on the trial base
  scales every wait, tween and duration of the trial, which is a
  confound. `ytl_rate()` (v0.4.0) exists for a movie base: a script sets
  it through the movie (`m.rate`), never on the trial base. Reverse is
  refused at every level (docs/timeline.md). A movie seek is the video
  header's job.
- **Position parameters and labels** (`"<"`, `"+=0.5"`): `now()` is the
  label, a local variable keeps it, and `wait` is the relative offset.
- **Per-tween `onUpdate`:** it costs script time on every frame, and its
  values would not be in the timeline's record. Use a track.
- **Color tweens in device RGB:** an RGB tween is not a perceptual or
  cone-space path. See the open questions.

#### Open questions

- **Recording the flicker edges.** A `flicker` on `gate` is a track. A
  track has no landing record for each edge, and the flip log and the
  photodiode are the record. Should `flicker` also be able to expand to
  events, for an edge-by-edge log? At 7.5 Hz for 10 minutes, that is
  9000 events, 576 KB of storage.
- **Flicker rates the display cannot show evenly.** 7.5 Hz at 144 Hz is
  19.2 frames per cycle, so the edges alternate between 19 and 20
  frames. Should the designer warn about this, refuse it, or offer the
  nearest rate that divides evenly?
- **Color.** Should a tween on color run in DKL or cone space through
  the `ysp/gfx.h` calibration, with the channels as the coordinates of
  that space? Partly answered 2026-10-06: `ysp/color.h` converts a DKL or
  cone-contrast color to a stimulus direction (`ycol_to_dir()`) through
  the context `ysp/gfx.h` uses (`ygfx_color()`), at 22 to 27 ns a call
  (MSVC; 79 ns MinGW), so three channels bound to a color in a space cost
  about 0.3 ms of CPU for 10000 elements a frame. Open: the binding form
  (three channels to one color field) and whether it belongs in
  `ygfx_bind`.
- **Units.** The parameter table gives each property's unit. Should
  `tween(g, {x = 2})` take a unit suffix (`deg(2)`, `px(2)`), or only the
  table's unit?
- **Aliases.** Should `"sine.inOut"` map to `"cosine"` for users who come
  from GSAP? The current answer is no: one name for each easing.

## 7. The designer

A static web app, offline as a PWA, with the project in local storage.
There is no server. The only job that wants one is the build job.

- **Import is normalization.** Dropping a file runs the pack tool in the
  browser, shows what it did and what it chose, exposes the choices
  that matter (lossy or lossless, project rate, exclusive audio), and
  writes the manifest. A resource that cannot be normalized fails in
  the designer with the file selected, never at run time.
- **Preview** runs the WebAssembly player in the page. It is faithful in
  logic and not in timing, and the page says so.
- **Run** talks to the native player on the rig through the runner
  protocol. A browser is never the rig.
- **Shaders** are validated in the browser with the WebAssembly build of
  glslang, through the same wrapper function the runtime uses. ANGLE
  compiles them on the rig when the pack loads (5.2).
- **Text** is edited in the designer with Skribidi compiled to
  WebAssembly: bidirectional editing, shaping and line breaking for
  complex scripts, laid out by the same pinned version the pack tool
  and the player use.
- **Operator console** is a browser tab on the runner protocol: live
  parameters, the staircase plot, the dropped-frame counter, the log.
- **Build jobs** go to a template repository with a GitHub Actions
  workflow, hosted or self-hosted runners, submitted through the
  GitHub API. Only generated code and the manifest are sent, never the
  pack. macOS builds run on a Mac. The toolchain versions from the
  build go into the manifest.
- **Parameter panels** are built from each header's parameter table.

## 8. Timing model

- The `ysp/rt.h` clock is the master for display, audio, video,
  triggers and capture.
- Every present and every onset is a target time and an actual time.
  The residual is logged.
- Under variable refresh the same API gets smaller residuals. Nothing in
  the experiment definition knows which refresh model it got.
- Software timestamps are estimates. The photodiode, the line-in and the
  LED are the measurements. Each header's STATUS block reports the gap
  between the two per platform.
- On Windows, the loopback test prints whether the window is in
  independent flip, from the composition swapchain statistics or from
  PresentMon.
- Variable refresh must be off unless the panel passed both sweeps.
- **Timing tiers** (proposal, 2026-10-05). Each platform and each
  presentation path is in one tier:
  1. Timing good. The onset is an OS vblank time on a path the header
     can verify, such as independent flip or direct scanout.
  2. Timing good only under stated conditions, which the header checks
     at run time where it can and names in the describe line.
  3. Timing bad. The path runs for task development and dry runs only.

  Measurements assign a tier, never an expectation. Each per-flip record
  carries the tier of that flip, because the path can change between
  frames. The screen reports the worst tier since open. An estimated or
  planned onset is never tier 1. A run can require a minimum tier and
  then refuses or flags the frames below it. The analysis, not the
  experiment definition, decides what to do with flagged frames.

## 9. Platform matrix

| Platform | Context | Swap path | Audio | Video decode | Deploy from |
|---|---|---|---|---|---|
| Windows 11 | ANGLE, D3D11 | Composition swapchain | WASAPI, exclusive available | Media Foundation | any host, zig |
| Windows 10 | ANGLE, D3D11 | DXGI flip model | WASAPI | Media Foundation | any host, zig |
| Linux X11 | native GL | GLX sync control | ALSA or PulseAudio | FFmpeg, opt-in | any host, zig, pinned glibc |
| Linux Wayland | native GL | presentation-time | ALSA or PulseAudio | FFmpeg, opt-in | any host, zig |
| macOS | ANGLE, Metal | measured, then claimed | CoreAudio | AVFoundation | a Mac |
| Web | WebGL2 | requestAnimationFrame, fixed grid | Web Audio | frame sequence, pl_mpeg | any host, Emscripten |

Frame sequence and pl_mpeg run everywhere.

Windows 11 is the Windows target. Windows 10 is end of life: it stays
supported where that costs little, for example through run-time lookup
of newer APIs, but no design decision or measurement rests on it.

### 9.1 Android and iOS, not planned, not precluded

The stack ports: SDL3 on both, native GL ES 3.0 on Android, ANGLE's
Metal backend on iOS, AAudio and iOS CoreAudio in miniaudio, MediaCodec
and AVFoundation for video. Android exposes scheduled presentation
times and display-present timestamps through EGL extensions. iOS gives
a presented-time handler. Touch input latency runs tens of milliseconds
and varies, which caps reaction-time work.

Two cases with different answers:

- **Lab-owned tablets.** Controlled hardware, a handful of devices,
  ad-hoc or TestFlight builds, a photodiode. The case to do first if
  mobile comes.
- **Participants' own devices.** The installable web player is the
  answer. A native route needs one player app in each store that
  downloads packs. Apple's guideline 2.5.2 forbids downloaded code
  outside JavaScriptCore, so downloaded Luau bytecode is a gray area.
  If iOS to participants ever matters, the scripting language becomes
  JavaScript. Decide that before the first script binding is written.

## 10. Verification

- `tests/loopback/` gains a photodiode test for `ysp/screen.h`, a
  line-out to line-in test for `ysp/audio.h`, an LED test for capture,
  a variable-refresh interval sweep and a luminance-versus-interval
  sweep. State on 2026-10-06: the photodiode test
  (`screen_loopback`) and the line-in mode of the audio loopback
  (`--line`) are built and have never run. Every onset measured so far
  is a software timestamp.
- `tests/compile/` gains the C and C++ pairs for each header, and a
  no-ANGLE build for Linux.
- CI on Windows takes ANGLE from the Electron release zip named in
  `ci.yml` (Electron v38.8.6, ANGLE 2.1.25848), checked by checksum and
  cached. Linux uses Mesa's EGL. macOS CI has no GL, so the ysp_gfx
  pixel checks skip there; where macOS gets ANGLE is not decided.
- No number outside a STATUS block is a measurement, as today.

## 11. Order of work

Revised 2026-10-06. The first order put Linux X11 with the photodiode
test first and Windows 10 second. The work went to Windows 11 first, on
the one development machine, and Windows 10 became best effort
(section 9).

### State on 2026-10-07

| Step | State |
|---|---|
| 1. `ysp/rt.h`: event ring, clock correlation | Done, v0.5.0, with the instrumentation macros. |
| 2. `ysp/timeline.h` | v0.4.0: events, tracks, tweens, `ytl_skip`, `ytl_lead`, `ytl_peek`, exact rational base rates (`ytl_rate`), `ytl_window`, `ytl_rt_time`, the `ytl_seq` builder with op tables, `ytl_run`, `ytl_check_ops` and the key arena (4.5.1). |
| 3. `ysp/screen.h` | v0.3.2 on Windows: DXGI_FLIP and COMPOSITION, flip hooks (codes, triggers, after-flip callbacks), the OS gamma ramp, the Shift+Esc abort and the triple-press panic, the window icon, text input for a text box with a ring record and a flip flag while keys are off the raw path, D3D11 video support on the device. Wayland and macOS are stubs. v0.7.0: the web backend (WebGL2, requestAnimationFrame, the lowest tier; docs/web.md) and the demos on GitHub Pages. v0.8.0 (2026-10-12): Linux X11, GLX with OML target vblanks and Present's per-flip events, X11 key times from the kernel's evdev time, else the X server's (docs/screen.md). No photodiode run. |
| 4. `ysp/audio.h` | v0.2.0 on WASAPI shared mode: onsets from a fit of device positions, tier 2. Streams (ring-fed voices, the soundtrack path) and WAV streaming. The other backends compile and have never run. No line-in run. |
| 5. `ysp/gfx.h` | v0.10.0: stimuli, the signed-distance vector program and kind-specialized programs, strokes, sprites, groups, render targets and setup passes, curve runs from Slug-form curve sets (exact area on resolved unturned glyphs, rays elsewhere and chosen automatically for turned runs), the blur pass, NOISE (uniform, Gaussian, integer simplex fBm, each with a bit-exact CPU twin; v0.9 replaced the nested noise hash, which repeated rows), the output stage; a program binary cache, NV12 and I420 video in linear light, texture import (GL, D3D11 through ANGLE), instanced stimuli, and draws reordered where they do not overlap. It requires `ysp/color.h` (v0.1.0). Text layout (Skribidi) belongs to the pack tool and the player (5.2), not to the header. Missed bars: full text pages (docs/gfx.md; a page drawn once into a target costs 0.5 ms a frame), the text program's cold compile, and some of v0.3's GPU bars. |
| 6. `ysp/video.h` | v0.2.0: scheduler, frame sequence, pl_mpeg, decode-ahead; Media Foundation (DXVA by default), planar upload to the gfx shader by default, the shared D3D11 GPU path as an option, the soundtrack on `ysp/audio.h` streams, base rates. Not built: AVFoundation, capture. |
| Also built | `ysp/outline.h` v0.2.1 (item 3), `ysp/rdk.h` v0.1.0 (random dot kinematograms, integer state, Python binding), `ysp/table.h` v0.1.0 and `ysp/trials.h` v0.2.0 (CSV and pack tables, order lists, sampling, units, transition balance, rules text). |
| 7 to 10 | Not started. |

### Next, in order

1. **Hardware verification on Windows 11.** Run the photodiode test on
   DXGI_FLIP and COMPOSITION, and the line-in test for `ysp/audio.h`.
   The "one vblank early" flips of the windowed DXGI runs were measured
   again on 2026-10-09 with the scanout as reference: they remain, and
   come from the depth rule when a window leaves the composed path, not
   from the grid (section 13). Every later
   step builds on onset claims that are software timestamps until this
   step is done. It needs the photodiode and a line-out to line-in
   cable on the development machine.
2. **`ysp/gfx.h`**: done for now (v0.10.0). Slug curve runs, the blur
   pass, setup passes, kind-specialized programs and gradient noise are
   built; sampler-filtered chroma was measured and deleted. The open
   items are in docs/gfx.md "Next".
3. **The outline builder**: curves and bands in the Slug form from font
   outlines and paths, stroke expansion, the exact-area alpha
   rasterizer. It is the core of the pack tool's text and artwork
   (item 6) and runs in parallel with item 2. State on 2026-10-07:
   `ysp/outline.h` v0.2.1 (docs/outline.md): a bounded TrueType and
   CFF reader that places glyphs as HarfBuzz does, overlap removal (curve sets carry the RESOLVED flag),
   strokes without the probe's fold, exact alpha, curve sets in format
   v1, and the SVG subset reader of 5.2 (Artwork). Missed bars: a Latin
   stroke (289 to 303 us a glyph against 200; CJK meets its 1 ms) and the
   sets of the heaviest fonts (Source Han Sans 94 to 103 us a glyph
   against 66).
4. **`ysp/timeline.h`**: done for now (v0.4.0: base rates,
   `ytl_window()`, sequences). `ysp/video.h` applies the rates for any
   rate other than 1.
5. **Other platforms.** `ysp/screen.h` on Linux X11 (GLX sync control;
   v0.8.0 on one X screen; separate X screens, the panic watchdog and
   the CRTC gamma ramp are next) and Wayland (presentation-time), then
   macOS through ANGLE's Metal backend. `ysp/audio.h` on ALSA, PulseAudio and CoreAudio. A platform
   gets its tier from a photodiode or line-in run on that platform, not
   from CI.
6. The pack tool as a library, and its CLI, with Skribidi, the outline
   builder (item 3) and the SVG subset reader (5.2, Artwork). State on
   2026-10-07: the first piece of its text half is built
   (docs/layout.md). Skribidi `dee63d6` with two local bidi patches (run merge; rule L1 per line, 2026-10-08), HarfBuzz
   14.6.0, SheenBidi, libunibreak 6.1 and budouxc are vendored in
   `third_party/` (7.7 MB, verified byte for byte by
   `tools/vendor_layout.py`); `pack/layout` v0.1.0 lays text out into
   curve-run items over on-demand curve sets and stamps each block with
   the versions and font hashes; it gives Chromium's glyph ids on all 15
   corpus lines (`tests/layout/`), on MSVC, MinGW and emcc.
   `examples/layout/gfx_layout.c` shows paragraphs in seven scripts, the bidi bug
   strings, BudouX breaking and Skribidi's editor (on `ysp/screen.h`
   v0.3.2's `yscr_text_input()`). Not started: the pack format, the
   CLI, serialized glyph runs.
7. The player, native and WebAssembly, and the runner protocol.
8. The designer.
9. Build jobs, `ysp/net.h`.

Items 2 and 3 run in parallel. Measurements on the development
machine run one at a time: on 2026-10-05 a second measuring job's load
showed up as a false regression in the first.

## 12. Extensions

The host owns what a timing claim depends on: the clock, the present,
the device callback's timing, the pack, the frame loop and the log.
Everything inside those boundaries is replaceable behind a small C
interface. The backends already named in this document, the swap path,
the gfx backend and the video decoder, are the same mechanism. An
extension implements one of these kinds, each a versioned C vtable:

| Kind | Contract | Example |
|---|---|---|
| Renderer | Draws into a texture the host owns. The host composites and presents. Its frame time is a phase in the flip record. | Filament, sharing the GL context and rendering to a render target, fenced on that context. |
| Presenter | Takes the window and implements the swap-path interface, flip record included. The host's timing claim ends at this boundary and the manual says so. | Code that must own the swapchain. |
| Audio source | Renders into the buffer the host's device callback hands it, under the real-time rules. | cave-audio's offline sink. |
| Audio device | Owns the device and calls the host's scheduler as a source in its callback. | cave-audio's ASIO array output. |
| Input source | Posts timestamped samples or events into the event ring and to scripts. | Eye trackers, motion capture, cave-audio's NatNet head tracking. |
| Video decoder | The interface in `ysp/video.h`. | A codec the built-in backends lack. |
| Script module | Exports a parameter table and typed functions. The host generates the Luau binding from the table. | Any of the above exposing parameters. |

- **Mechanics.** A shared module with one exported symbol that returns
  the vtable and its interface version, compiled by its author against
  the ysp C headers with any toolchain. The ABI is plain C structs. On
  desktop the player loads the modules the pack manifest lists, by
  hash. The web player and iOS have no dynamic loading, so an
  extension there is a custom player build through the eject path,
  from the same source and the same vtable. The designer shows a
  placeholder for what an extension renders unless the extension has
  a browser build.
- **Host API.** A second vtable passed in at load: the clock, the
  event ring push, the instrumentation macros as function pointers so
  extension zones land in the same trace, frame hooks with the
  predicted onset and the flip record, pump submission for off-thread
  work, pack resource access by name, and parameter table
  registration so the designer's panels and the script binding include
  the extension's parameters.
- **Threading contract**, stated and measured, not enforced. The host
  owns the frame thread and the audio callback. An extension may run
  its own threads. It must not block the frame thread, must not
  allocate or do I/O in the audio callback, and must report its own
  timing through the hooks. The flip record's extension phase shows
  when one does not.
- **Licenses.** The boundary does not change them. The manifest records
  each module's license beside its hash. A lab that distributes a
  player with a GPL module distributes under the GPL.

## 13. Open questions, to be measured or verified, not decided

- Does SDL3 create an EGL context on Cocoa against ANGLE's Metal
  backend, and what present timing does that backend expose?
- ANGLE's 10-bit surface configurations on D3D11 and Metal. Answered
  for D3D11 (2026-10-05, docs/gfx.md): RGBA 10/10/10/2 and RGBA16F
  configurations exist, and a client-buffer pbuffer on an
  `R10G10B10A2` texture works. Metal: not run.
- Composition swapchain: how a target time between two vblanks is
  quantized, how far ahead the queue accepts presents, and whether a
  borderless fullscreen composition window gets independent flip on the
  lab GPU and driver. Answered on the Iris Xe (2026-10-05,
  docs/screen.md): the frame goes to the nearest vblank, a tie to
  the earlier one; the queue accepts at least 7 presents ahead; a
  fullscreen COMPOSITION window got independent flip. The lab GPU and
  driver: not run.
- Whether an untimed screen on a second display disturbs a timed
  display in independent flip on the same GPU (4.2, Untimed screens).
  One-display result (docs/imgui_probe.md, 2026-10-07): an ImGui console
  window on its own device and swapchain, presented with DO_NOT_WAIT,
  left the timed window's flip records unchanged in windowed
  COMPOSITION; but a busy console under a fullscreen timed window gave
  25 presentation-path changes and 107 composed frames in 2 minutes
  (none late). The two-display run must count path changes and
  composed frames, not only drops.
- Whether SDL3 opens a window and a GL context on an X screen other
  than the default (`:0.1`). Answered 2026-10-12 (SDL 3.4.18, Xorg,
  docs/screen.md "Two X screens"): with `DISPLAY=:0.1` yes; from a
  process on `:0.0`, no, the window lands on screen 0. So
  `ysp/screen.h` v0.8.0's `desc.x_display` picks the X screen per
  process.
- Whether the composition swapchain's present-at-time removes the need
  for the spin-wait under variable refresh.
- miniaudio's exclusive-mode behavior per backend and the exact flag
  set that disables its silent rate conversion. Answered for WASAPI
  (2026-10-05, docs/audio.md): ask for no format, rate or channel
  count, so the client format is the device's; exclusive mode opens at
  the endpoint's Default Format. Other backends: not run.
- Whether SDL_GPU gains a browser backend, which would make it a
  candidate second implementation behind the `ysp/gfx.h` interface.
- The first stimulus that needs compute, which would move the plan to
  WebGPU through Dawn.
- Early flips of `ysp/video.h` in a window on DXGI_FLIP (found
  2026-10-06; measured again 2026-10-09 on `ysp/screen.h` v0.4.3,
  docs/video.md, "Rerun: the early flips"). Not the grid. When the
  system moves a foreground window from the composed path to an overlay
  plane (1.7 to 1.9 s after the start here), presents show at depth 1
  but the path report says composed for 5 more flips. v0.4.3's learner
  then shows 3 flips a vblank early (4 of 4 runs); the in-progress rule
  where the path sets the depth shows 6, and about 105 when open() learned
  depth 3 on the composed path (3 of 9 runs). COMPOSITION showed none in
  8 runs. To decide in `ysp/screen.h` (docs/screen.md, "Not measured").
- Whether a driver setting that forces vsync off (AMD Software "Wait for
  Vertical Refresh: Always off", NVIDIA "Vertical sync: Off", Intel
  "Vertical Sync: Speed") overrides a flip-model `Present(1)`, and on
  Linux Mesa's `vblank_mode=0` overrides swap interval 1. Prior art: the
  SDL frame pacing sample (TylerGlaiel/SDL-Frame-Pacing-Sample) detects
  "not actually vsynced" from the drift between measured and snapped
  frame times (requested 2026-10-09). The guard is in `ysp/screen.h`
  v0.4.2 (SYNC GUARD; docs/screen.md, "Driver-forced vsync off"): 32 of
  the last 64 flips with evidence (two in one refresh, no vblank wait,
  or a flip a quarter period before its vblank), or more presents than
  refreshes plus 8 over 64 refreshes, marks the screen untimed (tier 3,
  `YSCR_FLIP_UNSYNCED`, a ring record) with a message that names the
  settings. A scripted forced-off driver fires it four ways; legitimate
  paths give no evidence in the core test, and on the Iris Xe at most 3
  of 64 flips under load, misses and held frames (DXGI_FLIP; none on
  COMPOSITION). Still open: whether any panel overrides `Present(1)`. No
  panel setting was changed on 2026-10-09; the Intel setting is a hand
  test with `screen_sync_check` (docs/screen.md, "Hand test: the Intel
  setting"). AMD, NVIDIA: not run. Mesa on X11, `vblank_mode=0` in the
  environment (2026-10-12, Mesa 22.3.6): the swap interval reads back 0
  after asking for 1, and the GLX backend (screen.h v0.8.0) refuses to
  open, so the guard never sees that case; a driconf file setting was
  not tried.
- Exclusive-mode audio on hardware other than the development laptop
  (added 2026-10-09). There, the Realtek codec sits behind Intel Smart
  Sound Technology (the SST bus and its offload engine driver): shared
  periods are 10 ms only, and exclusive mode bursts three callbacks per
  event and holds 30 to 110 ms (shortest safe lead 125 ms) with raw
  WASAPI as well as miniaudio, so the DSP path is the likely cause, not
  exclusive mode (docs/audio.md, v0.3.0). To do: run
  `audio_wasapi_periods --init --exclusive` and
  `audio_schedule --exclusive` on each rig's audio device and under ALSA
  on the Linux machine; have `ysp/audio.h` report the driver stack (an
  endpoint on an offload DSP such as SST, the bus and driver names) in
  the describe line and the data file, so the cause is visible; and,
  once audio capture exists, measure the acoustic latency with a
  loopback cable on every path (all latencies so far come from device
  positions, not sound). Until then the rig recommendation is a
  dedicated audio interface (USB class compliant, PCIe, or ASIO opt-in,
  section 12).
  Deferred by the user (2026-10-09): on the development laptop, switch
  the controller to Microsoft's inbox HD Audio driver (128 to 480
  samples per Microsoft's low-latency audio page) or turn SST off in the
  firmware, then rerun both programs. Public reports agree that Realtek
  drivers offer 480 frames only (miniaudio discussion 1084, issue 949;
  JUCE issue 560); none found measures SST in exclusive mode.
- The first sound after a long silence (added 2026-10-10). `yau_open()`
  runs the stream silent until the fit is ready and keeps it running
  until `yau_close()`, so the OS, driver and DMA path stays awake between
  sounds (PsychPortAudio does this only in run mode 1). Zeros do not keep
  awake a sink that gates on digital silence: HDMI and DisplayPort
  monitors, some USB DACs, and speakers or amplifiers with auto-standby.
  Such a sink can clip or delay the first sound after a gap. To do, once
  audio capture exists: on each output path, record the first sound after
  1, 10 and 60 s of silence against a repeat, by line-in at the jack or a
  microphone at the speaker, and compare onset time and level. If a path
  gates, add an opt-in noise floor (dither near -90 dBFS) so that the sink
  never gets pure zeros; off by default, because it changes the signal.

## 14. Coverage of jsPsych

Status: reference, 2026-10-07. This section maps each jsPsych plugin and
extension, and the jsPsych timeline model, to the ysp headers and to the
player of section 6. It lists what is missing and ranks the gaps.

Sources, read on 2026-10-07:
- jsPsych `main` at `3e24c16` (2026-09-16), core 8.3.0: `packages/plugin-*`,
  `packages/extension-*`, `docs/overview/` (timeline, plugins, data, events,
  experiment options, timing accuracy), `docs/plugins/`, `docs/extensions/`,
  `docs/reference/` (core, randomization, pluginAPI).
- jspsych-contrib `main` at `f5f3d15` (2026-09-16): package names and
  `package.json` descriptions. Parameters read for `plugin-rdk` only.
- jspsych-psychophysics (Kuroki, third party, MIT): parameter and object
  property names only.
- ysp: README.md, this document (sections 4 to 8, 11, 12), and the
  manuals of `ysp/trials.h`, `ysp/screen.h` (INPUT, ABORT), `ysp/gfx.h`
  (API list, glyph and curve runs, hit tests, units), `ysp/audio.h` and
  `ysp/video.h` (API lists), `ysp/rdk.h` (desc), `ysp/outline.h` (font
  metrics).

### 14.1 What ysp gives a jsPsych-style trial today

| Need | ysp today | Limit |
|---|---|---|
| Show a stimulus at a time, hide it after a duration | `ysp/timeline.h` onset and offset events (`ytl_seq`, op tables), bound to `ysp/gfx.h` stimuli (`ygfx_bind`); each event has a landing record (frame, residual) | None for shapes, gratings, gabors, dots, noise, images, video |
| Key response | `yscr_poll()`: SDL events restamped on the `ysp/rt.h` clock; raw keyboard path on Windows. `ysp/response.h` v0.1.0 (2026-10-07; v0.2.0 trimmed 2026-10-09): choices, a window bound to the flip onset, minimum RT, release, held keys, RT with the onset's tier; double-report removal moved to `ysp/screen.h` v0.4.1's input bridge (`examples/response/trial_keyboard.c`) | Keyboards are ms-grade at best |
| RT-grade response | `ysp/serial.h` response boxes | None |
| Mouse click on a stimulus | SDL mouse events through `yscr_poll()`; `ygfx_hit()`, `ygfx_hit_index()` (instances, curve runs, artwork layers) | No drag helper, no pointer trace recorder |
| Sound at a time | `yau_play_at()`, tones, noise, clicks, WAV load and stream; onset record (fit time, tier 2 at best) | No microphone capture |
| Movie | `ysp/video.h`: frame sequence, MPEG-1, Media Foundation; seek, loop, pause; rate on the movie base | No camera capture (planned, 4.6) |
| Text | `ysp/gfx.h` v0.7 curve runs and cached static text; `ysp/outline.h` glyph outlines, `yol_font_glyph_index()`, `yol_font_hmetrics()` | Layout (Skribidi: wrap, shaping, kerning, bidi) is the pack tool's and the player's, not built. A one-line Latin label placed by advances works now (`examples/outline/font.c`) |
| Image file | `ygfx_image()` from planes; QOI decode in `ysp/video.h` (`yvid_qoi_decode()`) | No PNG or JPEG decoder in a header. The pack tool converts (5.2), not built |
| Degrees of visual angle | `ygfx_view {distance_mm, width_mm}`, `YGFX_DEG` | The participant procedure that measures them (card, blind spot) does not exist |
| Conditions, order, repetitions | `ysp/trials.h`: factorial, flat or table rows (CSV conditions files via `ysp/table.h`), SEQUENTIAL, RANDOM, FULL_RANDOM, CONSTRAINED, LIST, WITH_REPLACEMENT, subsets, blocked and alternating groups, units, transition balance, Latin squares, rules text, weighted reps, blocks, practice, warmup, re-queue, tracks (v0.2.0) | One level of nesting (groups); deeper trees belong to the player. See 14.5 |
| Adaptive levels | `ysp/stair.h`, `ysp/quest.h`, `ysp/aep.h` as tracks of `ysp/trials.h` | None |
| Triggers | `ysp/parallel.h`, `ysp/serial.h`, flip hooks of `ysp/screen.h` | None |
| Experiment flow, data file | Nothing above `ysp/trials.h`. The player (section 6) is not built | See 14.4 |

### 14.2 Classes

| Class | Meaning |
|---|---|
| Now | The headers cover it. A C program can do it today with caller code for the response loop. |
| Text | Needs multi-line or shaped text. Glyph drawing exists (`ysp/gfx.h` v0.7 curve runs). Layout is Skribidi in the pack tool and the player, not built. |
| Helper | Needs a small helper (tens to a few hundred lines) above the headers, in the player or an example. |
| Widget | Needs a widget layer: buttons, sliders, text entry, layout of controls, focus. |
| Device | Needs a device feature that is planned but not built: microphone capture, camera capture, an eye-tracker input source. |
| Out | Out of scope, with the reason given. |

"C ex." says whether the plugin makes a good C example in `examples/`.

### 14.3 Official plugins and extensions

53 plugins and 4 extensions in `packages/`. Totals: Now 14, Text 8,
Widget 11, Helper 4 (`sketchpad` counted here), Device 8, Out 8.

#### Keyboard-response trials

| Plugin | What it does | ysp parts | Missing | Class | C ex. |
|---|---|---|---|---|---|
| `html-keyboard-response` | Shows HTML, records a key | gfx shapes, curve runs; timeline on and off; `yscr_poll()` | Layout for general HTML text | Text (Now for shapes and one-line Latin labels) | Done: `examples/response/trial_keyboard.c` (fixation, stimulus, response window, RT from the flip onset) |
| `image-keyboard-response` | Shows an image, records a key | `ygfx_image()`; timeline | Image file decode for C users | Now | Yes, with QOI or procedural images |
| `canvas-keyboard-response` | Draws through a user function, records a key | Any `ysp/gfx.h` drawing, user shader contract | None (`ysp/response.h` v0.1.0) | Now | Covered by the base trial |
| `audio-keyboard-response` | Plays a sound, records a key; `trial_ends_after_audio`, `response_allowed_while_playing` | `yau_play_at()`, onset record, buffer length | None: `ysp/response.h` v0.1.0 measures RT from the audio onset fit (`yrsp_onset_audio()`, tier 2) | Now | Done: `examples/response/trial_audio_keyboard.c` |
| `video-keyboard-response` | Plays a video, records a key; `start`, `stop`, `rate`, `trial_ends_after_video` | `ysp/video.h` play, seek, end; rate on the movie base (`ytl_rate`) | None (`ysp/response.h` v0.1.0) | Now | Yes: extend `examples/video/play.c` |
| `animation` | Image sequence at `frame_time`, `frame_isi`, `sequence_reps`; keys during it | Images as timeline onsets; one landing record per frame | Image decode for C users; prompt text | Now | Yes: shows per-frame landing records that jsPsych cannot give |
| `categorize-image` | Image, key, feedback text by `key_answer` | Images, timeline | Feedback text (`correct_text`, `incorrect_text`) | Text (Now with symbol feedback) | After text |
| `categorize-html` | HTML, key, feedback text | Shapes | Layout | Text | No |
| `categorize-animation` | Animation, key, feedback text | Images, timeline | Feedback text | Text | No |
| `same-different-image` | Two images in sequence: `first_stim_duration`, `gap_duration`, `second_stim_duration`; same or different key | Timeline sequence, images | None (`ysp/response.h` v0.1.0) | Now | Done: `examples/response/trial_same_different.c` (bars; SOA from flip records) |
| `same-different-html` | The same with HTML | Shapes | Layout | Text | No |
| `iat-image` | IAT: image, category labels left and right, error feedback | Images; one-line labels by advances | Layout for labels in general; `html_when_wrong` | Text | No |
| `iat-html` | IAT with an HTML stimulus | As above | Layout | Text | No |
| `serial-reaction-time` | Grid of squares, a target lights, key per position; `pre_target_duration`, `fade_duration` | `ygfx_instances()`, `ygfx_inst_grid()`, tween on opacity | None (`ysp/response.h` v0.1.0) | Now | Done: `examples/response/trial_srt.c` (keys; sequence and random blocks) |
| `serial-reaction-time-mouse` | The same, click the target | `ygfx_hit_index()` on the grid; mouse events | Mouse times are ms-grade (state it) | Now | Yes, with the SRT above |
| `visual-search-circle` | Target and foils on a circle; present or absent key | Shapes or images at computed places; instances | None (`ysp/response.h` v0.1.0) | Now | Yes |
| `reconstruction` | Method of adjustment: keys change a parameter of `stim_function` | Any gfx parameter driven by a key; `ysp/timeline.h` tween of length 0 | Finish key instead of `button_label` | Now | Done: `examples/response/trial_adjustment.c` |

#### Button and slider trials

| Plugin | What it does | ysp parts | Missing | Class | C ex. |
|---|---|---|---|---|---|
| `html-button-response` | HTML, buttons; `button_layout`, `grid_rows`, `enable_button_after` | Shapes and hit tests | Button widget, control layout, layout for labels | Widget | No |
| `image-button-response` | Image, buttons | Images, hit tests | Button widget | Widget | No |
| `canvas-button-response` | Canvas, buttons | gfx, hit tests | Button widget | Widget | No |
| `audio-button-response` | Sound, buttons | `yau_play_at()`, hit tests | Button widget | Widget | No |
| `video-button-response` | Video, buttons | `ysp/video.h`, hit tests | Button widget | Widget | No |
| `html-slider-response` | HTML, slider; `min`, `max`, `step`, `slider_start`, `labels`, `require_movement` | Shapes, hit tests | Slider widget (drag, keys, tick labels) | Widget | No |
| `image-slider-response` | Image, slider | Images | Slider widget | Widget | No |
| `canvas-slider-response` | Canvas, slider | gfx | Slider widget | Widget | No |
| `audio-slider-response` | Sound, slider | Audio | Slider widget | Widget | No |
| `video-slider-response` | Video, slider | Video | Slider widget | Widget | No |
| `cloze` | Text with blanks the participant types into | None | Text entry widget, layout. `ysp/screen.h` stops SDL text input on purpose (keys stay on the raw path), so a text-entry trial turns it on and is untimed | Widget | No |

#### Pointer, drawing and size procedures

| Plugin | What it does | ysp parts | Missing | Class | C ex. |
|---|---|---|---|---|---|
| `free-sort` | Drag images into an area; logs every move | Images, `ygfx_hit()`, mouse events | Drag helper (pointer capture, z-order, drop area test, move log); finish button (a key will do) | Helper | Later |
| `sketchpad` | Freehand drawing, undo, redo; saves strokes and PNG | Polyline paths (224 points each), capsules, render targets, `ygfx_read_target()` | Stroke accumulation into a target; buttons; PNG writer | Helper + Widget | No |
| `resize` | Participant scales a card image to measure pixels per unit | Images, key or drag adjust, `ygfx_view` | Procedure that turns the card width into `width_mm` | Helper | No; web player only |
| `virtual-chinrest` | Card resize, then blind-spot task, gives viewing distance | Shapes, tween of a moving dot, keys, `ygfx_view` | Procedure and the distance formula. A lab uses a chin rest and a tape | Helper | No; web player only |

#### Media capture and eye tracking

| Plugin | What it does | ysp parts | Missing | Class | C ex. |
|---|---|---|---|---|---|
| `html-audio-response` | Records the microphone after a stimulus | `ysp/audio.h` output only | Microphone capture on the `ysp/rt.h` clock | Device | After capture |
| `initialize-microphone` | Asks permission, picks the microphone | None | Microphone capture; device choice | Device | No |
| `html-video-response` | Records the camera after a stimulus | None | Camera capture (4.6, planned) | Device | After capture |
| `initialize-camera` | Asks permission, picks the camera | None | Camera capture | Device | No |
| `mirror-camera` | Shows the live camera | `ygfx_texture_update()` can show frames | Camera capture; the display is untimed | Device | No |
| `webgazer-init-camera` | Starts WebGazer, positions the face | None | Eye-tracker input source (12) | Device | No |
| `webgazer-calibrate` | Calibration dots | Dots and timeline are Now | Input source; calibration fit | Device | No |
| `webgazer-validate` | Validation dots, gaze offset, percent in ROI | Dots are Now | Input source; validation statistics | Device | No |

#### Flow, setup and forms

| Plugin | What it does | ysp parts | Missing | Class | C ex. |
|---|---|---|---|---|---|
| `instructions` | Pages of HTML; keys or clickable navigation | Static text in a target (v0.7), keys | Layout; buttons optional | Text | After text |
| `fullscreen` | Enters browser fullscreen | `ysp/screen.h` opens fullscreen; the web player's self-test records the state (6) | Nothing native. Web player: not built | Now | No |
| `preload` | Loads media before trials | The pack loads at open; long media streams (5, 4.4) | Nothing | Now (by design) | No |
| `browser-check` | Checks features, window size, measures refresh | `yscr_describe()`, capabilities, flip records; the web self-test (6) | Web self-test not built | Now (native) | No; `screen_flipstats.c` exists |
| `call-function` | Runs a function in the timeline | C, or a script call in the player | Nothing | Now | No |
| `external-html` | Loads an external page (often consent) | None | Out: consent and information pages are documents for a browser or the recruitment platform, before the player starts. No timing claim, and a document engine is not a stimulus engine | Out | No |
| `survey` | SurveyJS forms | None | Out: questionnaires need a full form engine with validation and accessibility. SurveyJS, REDCap and Qualtrics do this; the player links to them. No timing claim | Out | No |
| `survey-html-form` | Free HTML form | None | Out, as `survey` | Out | No |
| `survey-likert` | Likert items | None | Out, as `survey`. A single rating by key is a keyboard trial | Out | No |
| `survey-multi-choice` | Radio questions | None | Out, as `survey` | Out | No |
| `survey-multi-select` | Checkbox questions | None | Out, as `survey` | Out | No |
| `survey-text` | Text questions | None | Out, as `survey` | Out | No |
| `maxdiff` | Best and worst choice in a table of alternatives | None | Out: an untimed form for preference scaling; a survey tool does it | Out | No |

#### Extensions

| Extension | What it does | ysp parts | Missing | Class | C ex. |
|---|---|---|---|---|---|
| `extension-mouse-tracking` | Records pointer samples and target boxes per trial; `minimum_sample_time`, `targets`, `events` | Restamped mouse events, `ygfx_bounds()`, the event ring | Pointer trace recorder into the ring, with stimulus boxes at each flip | Helper | Partly: `examples/response/trial_mouse_tracking.c` (the collector's trace, fixed boxes in the file header; not in the ring) |
| `extension-webgazer` | Gaze samples per trial, ROI targets | None | Input source (12) | Device | No |
| `extension-record-video` | Camera recording per trial | None | Camera capture (4.6) | Device | No |
| `extension-pipe` | Sends data to DataPipe (OSF) | None | Out for the native player: data stays on the rig. The web player has no upload path; decide that with the web deploy, not here | Out | No |

### 14.4 jspsych-contrib, paradigm-relevant

Descriptions from `package.json`. Not verified beyond that, except
`plugin-rdk`. Skipped as not paradigm-relevant: URL capture and redirect,
Nextcloud and DataPipe storage, device orientation, slide-to-continue,
result charts, countdown, the survey variants, number and numpad entry,
tangram and copying games.

| Plugin | What it does | ysp parts | Missing | Class | C ex. |
|---|---|---|---|---|---|
| `plugin-rdk` (contrib) | Random dot kinematogram, key report | `ysp/rdk.h`: `count`, `coherence`, `direction`, `lifetime`, `sets`, aperture CIRCLE and RECT, edge WRAP and REPLOT; transparent motion as two fields | `number_of_apertures` is several fields; `opposite_coherence` has no direct field. Units differ: contrib `move_distance` is px per frame, ysp `speed` is units per second (contrib angle and `dot_life` units not verified) | Now | Yes: `gfx_rdk.c` plus a response window |
| `plugin-rok` (contrib) | Random object kinematogram: oriented objects | `ysp/rdk.h` into instance records (gabor arrays) | None (`ysp/response.h`) | Now | No (RDK covers it) |
| `plugin-flanker` (contrib) | Flanker array with SOA | Arrows as polygons or paths; letters as one-line labels; timeline | None (`ysp/response.h`) | Now | Yes |
| `plugin-stop-signal` (contrib) | Stop-signal task: an animation, button responses (its description; the paradigm details not verified) | Timeline; `ysp/stair.h` for the stop-signal delay; `yau_play_at()` for an auditory signal | Covered by `ysp/response.h` (ysp uses keys or a response box, not buttons) | Now | Done: `examples/response/trial_stop_signal.c` |
| `plugin-libet-intentional-binding` (contrib) | Libet clock; participant sets the hand to report a time | Repeating rotation track, line shape, tone at a delay, key or mouse adjust | None (`ysp/response.h`) | Now | Yes: timing-critical |
| `plugin-corsi-blocks` (contrib) | Blocks flash in order; participant clicks the order | Timeline sequence, `ygfx_hit_index()` | Click log helper | Now | Yes |
| `plugin-spatial-nback` (contrib) | Grid cell lights; n-back responses | Instances grid, timeline | None (`ysp/response.h`) | Now | No |
| `plugin-visual-search-click-target` (contrib) | Search display, click the target | Instances, hit tests | None | Now | No |
| `plugin-image-array-keyboard-response` (contrib) | Many images, key response | Images | Image decode for C | Now | No |
| `plugin-image-click-response`, `plugin-image-hotspots` (contrib) | Click points or regions on an image | Image, hit tests, mouse events | Region list helper | Now | No |
| `plugin-video-several-keyboard-responses` (contrib) | Several keys during a video, with times | `ysp/video.h`, `yvid_movie_time()` | None (`ysp/response.h` keep_open) | Now | No |
| `plugin-video-hotspots` (contrib) | Video freezes, click regions | `ysp/video.h` pause, hit tests | None | Now | No |
| `plugin-vsl-animate-occlusion`, `plugin-vsl-grid-scene` (contrib) | Statistical learning: shapes behind an occluder; image grids | Tweens, images, instances | None | Now | No |
| `plugin-pursuit-rotor` (contrib) | Track a moving target with the pointer | Sampled track or tween for the target; mouse events | Pointer trace recorder | Helper | Yes, with the mouse-tracking example |
| `plugin-trail-making` (contrib) | Connect numbered circles in order | Circles, one-character labels, lines, hit tests | Click sequence helper | Helper | No |
| `plugin-tower-of-london` (contrib) | Move balls between pegs to a goal | Shapes, hit tests | Click-to-move or drag helper | Helper | No |
| `plugin-self-paced-reading`, `plugin-spr` (contrib) | Words revealed one by one on key press; RT per word | One-line Latin by advances | Layout for real text and masks | Text | After text: common in psycholinguistics |
| `plugin-circle-click-response` (contrib) | Options on a circle around a stimulus, click one | Shapes, images, hit tests | Text labels | Text | No |
| `plugin-html-multi-response`, `plugin-image-multi-response`, `plugin-audio-multi-response` (contrib) | Button or key response | As the key trials | Button widget | Widget | No |
| `plugin-html-vas-response`, `plugin-html-keyboard-slider`, `plugin-survey-vas` (contrib) | Visual analog scale | Shapes | Slider widget | Widget | No |
| `plugin-bart`, `plugin-columbia-card-task` (contrib) | Risk tasks with buttons and money counters | Shapes, hit tests | Buttons, number text | Widget | No |
| `plugin-free-recall-response` (contrib) | Typed recall, word by word | None | Text entry | Widget | No |
| `plugin-gamepad` (contrib) | Gamepad responses | SDL events through `yscr_poll()` | `ysp/screen.h` starts only `SDL_INIT_VIDEO`; gamepad events need `SDL_INIT_GAMEPAD` from the caller (not verified); gamepad timing not measured | Helper | No |
| `*-swipe-response`, `extension-touchscreen-buttons`, `extension-device-motion` (contrib) | Touch and motion input on phones | None | Out: mobile is not planned (9.1) | Out | No |
| `extension-chiasm`, `plugin-chiasm-*`, `extension-mediapipe-face-mesh` (contrib) | Webcam eye and face tracking | None | Input source (12) | Device | No |
| `plugin-html-keyboard-response-raf` (contrib) | Key trial timed by `requestAnimationFrame` | The frame loop does this by design | None (`ysp/response.h`) | Now | No |

Third party, not in contrib: jspsych-psychophysics (Kuroki 2021, MIT) is
the jsPsych route that jsPsych's own timing page recommends. Its stimulus
objects (`obj_type` rect, circle, line, cross, text, image, sound, gabor,
manual) with `show_start_time`, `show_end_time`, `motion_start_time`,
`horiz_pix_sec` and frame variants (`show_start_frame`) map directly to
`ysp/gfx.h` stimuli and timeline on, off and tween events. It is the
closest jsPsych analog of a ysp trial and the best source of names for a
designer's stimulus list.

### 14.5 Timeline model

jsPsych runs a tree of nodes. A leaf is a trial (a plugin `type` and its
parameters). A node has a `timeline` array and node parameters. ysp has a
flat session in `ysp/trials.h` and a within-trial sequence in
`ysp/timeline.h`. The tree lives in the player's experiment definition (6).

| jsPsych concept | jsPsych behavior | `ysp/trials.h` | `ysp/timeline.h` (`ytl_seq`) | Player (6, 6.1) |
|---|---|---|---|---|
| Trial object, `type` | One plugin run | One `ytr_next()` / `ytr_update()` pair | One trial base, anchored at the trial's first frame, cleared after | A trial function `trial(c)` or a data trial template from the designer's vocabulary |
| Nested `timeline` | Children inherit the node's parameters; any depth | One level: `desc.groups` (v0.2) orders the levels of one factor as blocks or a cycle; `block_size` counts otherwise | None | Needed: nodes with inherited parameters |
| `timeline_variables` | A table; each row runs the node's timeline once | Condition rows (flat or factorial); the caller maps row to values | None | Rows from a pack Table (5.2); `c.face` in a script for `jsPsych.timelineVariable('face')` |
| `randomize_order` | Shuffle rows; reshuffle each repetition | `YTR_ORDER_RANDOM` (each repetition a shuffled block): same semantics | None | Map directly |
| `repetitions` | Repeat the node | `desc.reps`; `desc.cond_reps` per row | None | Map directly |
| `sample: fixed-repetitions` | Each row `size` times, one shuffle | `YTR_ORDER_FULL_RANDOM` with `reps = size` | None | Map directly |
| `sample: with-replacement` (`size`, `weights`) | Independent draws | `YTR_ORDER_WITH_REPLACEMENT`, `desc.draws`, `desc.weights` (v0.2) | None | Map directly |
| `sample: without-replacement` (`size`) | A random subset of rows | `desc.subset` with FULL_RANDOM and 1 repetition (v0.2) | None | Map directly |
| `sample: alternate-groups` (`groups`, `randomize_group_order`) | Strict cycle through groups | `desc.groups` ALTERNATE, any group order (v0.2); unequal sizes are refused, where jsPsych cuts to the smallest | None | Map directly |
| `sample: custom` (`fn`) | User function returns the order | `YTR_ORDER_LIST`, `desc.order_list` (v0.2); `ytr_latin()` for Latin and Williams rows by participant | None | The script's function writes the list |
| `jsPsych.randomization.shuffleNoRepeats` | Shuffle, no immediate repeats | `YTR_ORDER_CONSTRAINED` with `ytr_max_run(YTR_CONDITION, YTR_ANY_LEVEL, 1)`; more rules than jsPsych has | None | ysp is ahead |
| `jsPsych.randomization.factorial` | Crossed design | `desc.factors` | None | Map directly |
| `sampleExGaussian`, `sampleExponential`, `randomInt` | Jitter for foreperiods and ITIs | Only `desc.rng`; 4.8 bars a general random header | None | Gap: the script needs draws from the trial handler's generator (uniform, exponential, truncated) so a replay reproduces |
| `conditional_function` | Run the node or skip it, decided at the node's first trial | None | None | A script `if`; a data-only form needs a condition expression on a node |
| `loop_function(data)` | Repeat the node while it returns true | `ytr_requeue()` covers "repeat this trial" only | None | A script loop; a data-only form for "practice until criterion" (accuracy over the last iteration) |
| Runtime push and pop of nodes | Branches added in `on_finish` | None | None | Do not borrow: replay needs the tree fixed at load. Use explicit branches |
| `on_start(trial)` | Edit the trial's parameters before it runs | The caller computes before drawing | Builder calls after a static op table (4.5.1) | The trial function computes from `c` |
| `on_load` | After the first display | None | The first flip record of the trial | A handle that resolves at the first flip |
| `on_finish(data)` | Edit the trial's data | `ytr_update(outcome, rec)` | None | The trial function returns its record table |
| `on_timeline_start`, `on_timeline_finish` | Once per node | None | None | Node hooks |
| `data` (node or trial) | Constant columns, inherited | `desc.records` (fixed bytes); factor columns in `ytr_format_row()` | MARK events | Named columns, inherited from nodes |
| `jsPsych.data.addProperties` | Columns on every row (subject, group) | `ytr_format_meta()` heads the file | None | Session columns |
| `save_timeline_variables` | Copies row values into the data | Factor levels are in every row already | None | Default on: every row value goes in the record |
| `record_data: false` | No data row | None | None | A flag on a trial |
| `trial_index`, `trial_type`, `time_elapsed` | Columns on every row | `index`, `block`, `rep`, `condition` columns | None | Add `trial_type` (template name) and session time on the `ysp/rt.h` clock |
| `stimulus_duration` | Hides the stimulus by `setTimeout` | None | OFFSET at onset + duration, on the nearest frame, with a landing record | `wait(d); hide(s)` |
| `trial_duration` | Ends the trial if no response | None | A wait | `race(input.key{...}, wait(d))` (6) |
| `response_ends_trial` | End at the response, or run to `trial_duration` | None | None | After the race: return, or `wait_until(start + d)` |
| `post_trial_gap`, `default_iti` | Blank screen after the trial | None | A wait on the session base | A node or session parameter |
| `rt` | From the call that registers the key listener (`performance.now()`), not from a flip (pluginAPI docs) | Caller's record | Onset record of the stimulus event | RT from the stimulus's flip onset estimate (or audio onset fit), with its tier in the row |
| `choices`, `"ALL_KEYS"`, `"NO_KEYS"` | `KeyboardEvent.key` strings, lowercased by default | None | None | Accept jsPsych key names; map to SDL keycodes; log the scancode too |
| `minimum_valid_rt`, `wait_for_key_release`, `rt_key_duration`, `allow_held_key`, `persist` | Key listener options | `ysp/response.h` v0.1.0 | None | Options of `yrsp_desc` (`min_rt_ns` for `minimum_valid_rt`, in ns since response.h v0.3.0; `allow_held_key` and `persist` semantics: docs/response.md) |
| `name`, `abortTimelineByName`, `abortCurrentTimeline`, `abortExperiment`, `pauseExperiment` | Flow control | None | None | Node names; abort and pause over the runner protocol and `yscr_request_abort()` |
| `extensions: [{type, params}]` per trial | Extension hooks per trial | None | None | Input-source extensions (12) enabled per node or trial |
| Simulation mode (`data-only`, `visual`), `simulation_options` | Runs without a participant | The SIM display; simulated observers in examples | None | A player mode: data-only runs with a simulated observer on the SIM display |

### 14.6 What the player's experiment definition should borrow

Borrow the structure and these names, so a jsPsych user can read a ysp
definition:
- Nodes: `timeline`, `timeline_variables`, `randomize_order`,
  `repetitions`, `sample` with `type` (`with-replacement`,
  `without-replacement`, `fixed-repetitions`, `alternate-groups`,
  `custom`), `size`, `weights`, `groups`, `randomize_group_order`,
  `conditional_function`, `loop_function`, `on_timeline_start`,
  `on_timeline_finish`, `name`.
- Trials: `stimulus`, `choices` (with `ALL_KEYS` and `NO_KEYS`), `prompt`,
  `stimulus_duration`, `trial_duration`, `response_ends_trial`,
  `post_trial_gap`, `data`, `record_data`, `save_timeline_variables`,
  `on_start`, `on_finish`, `extensions`.
- Response options: `minimum_valid_rt`, `wait_for_key_release`,
  `allow_held_key`, `persist`.
- Paradigm parameters, for the built-in templates: `key_answer`,
  `correct_text`, `incorrect_text`, `feedback_duration`,
  `show_stim_with_feedback`, `first_stim_duration`, `gap_duration`,
  `second_stim_duration`, `same_key`, `different_key`, `answer`,
  `trial_ends_after_audio`, `trial_ends_after_video`,
  `response_allowed_while_playing`, `frame_time`, `frame_isi`,
  `sequence_reps`, `target_present`, `set_size`.
- Data columns: `rt`, `response`, `stimulus`, `correct`, `trial_index`,
  `trial_type`, `time_elapsed`, `rt_key_duration`.
- From jspsych-psychophysics, for timed stimulus objects:
  `show_start_time`, `show_end_time`, `motion_start_time`,
  `motion_end_time`, `response_start_time`.

Keep the ysp semantics where they differ, and say so in the designer:
- `rt` starts at the flip onset estimate of the stimulus event (or the
  audio onset fit), not at listener registration. The row carries the
  onset tier and residual.
- `stimulus_duration` and every other duration land on a frame. The
  landing record is in the data.
- Rows are a fixed tree at load. No runtime push or pop.
- Keys: log scancode and keycode. Case folding does not apply.
- Randomness comes from the trial handler's generator with a logged seed.

Units, decided 2026-10-07: seconds, in the experiment definition, the
script API and the data file. Milliseconds were rejected: they add a
conversion for users who come from Psychtoolbox and PsychoPy, which use
seconds. jsPsych gives durations in ms, so a jsPsych name with a value in
seconds is a 1000x trap for a user who switches. Proposed safeguards, not
yet decided:
- A duration value may carry its unit as text (`"250 ms"`, `"0.25 s"`);
  the loader converts it. A bare number is seconds.
- The designer shows the unit beside every duration field.
- The loader warns when a bare duration is above 60 s, with the
  millisecond reading in the message.
- An importer for jsPsych timelines converts ms to s.

### 14.7 Gaps ranked by the paradigms they block

Counts are of the official plugins in 14.3, then of the contrib plugins in
14.4. "Blocks" means a C user cannot do it with the headers; "burdens"
means a C user writes the code each time.

| Rank | Gap | Where it goes | Blocks or burdens | Count (official + contrib) | Size |
|---|---|---|---|---|---|
| 1 | Experiment flow: nested nodes, timeline variables bound to a pack table, conditional and loop, named data rows | The player (11, item 7) | Blocks every paradigm for users who do not write C | All | Large; planned |
| 2 | Response helper: key set, double-report removal (now in `ysp/screen.h` v0.4.1's input bridge), RT from the onset record, window, `response_ends_trial`, key release, minimum RT | Done for C: `ysp/response.h` v0.1.0 and `examples/response/trial_keyboard.c` (2026-10-07). Left: `input.key` in the player | Burdened every key response trial | 17 + about 10 | Small |
| 3 | Text layout (Skribidi): prompts, instructions, word stimuli, feedback text | Pack tool and player (11, items 6 and 7) | Blocks text-heavy paradigms; `prompt` appears on most plugins | 8 + 3, and every `prompt` | Large; planned |
| 4 | Widget layer: button, slider, text entry, control layout | Player, above `ysp/gfx.h` | Blocks button and slider responses | 11 + 9 | Medium |
| 5 | Image file decode for C users | Pack tool; meanwhile QOI or an outside decoder in examples | Burdens image trials | 10 + 3 | Small (in the pack tool) |
| 6 | Eye-tracker input source and calibration, validation | Extension (12), Input source kind | Blocks gaze tasks and fixation control. jsPsych counts understate it: lab psychophysics uses fixation control widely | 4 + 4 | Medium per tracker |
| 7 | Capture: microphone, camera | `ysp/audio.h` input; `ysp/video.h` capture (4.6) | Blocks voice and video responses | 6 + 0 | Medium |
| 8 | Pointer helpers: drag, pointer trace, click sequence | Helper in the player; examples | Blocks drag sorting and mouse tracking | 2 + 4 | Small |
| 9 | Done in the headers: order and draws (`ysp/trials.h` v0.2.0) and jitter draws (v0.2.1, 2026-10-07: uniform, choice and truncated exponential durations per trial, snapped to frames on request, logged and replayed, per condition from table columns). Left: exposing them in the script | Player | Burdens designs with jittered foreperiods | Node-level; not counted per plugin | Small |
| 10 | Size procedures: card resize, blind spot | Web player | Blocks only web deploys without a measured display | 2 + 0 | Small |
| 11 | Freehand canvas, gamepad | Helper; caller's SDL init | Blocks sketch tasks and gamepad responses | 1 + 1 | Small |

Recommended C examples, in order: the base keyboard trial (rank 2's
helper in its first form, with RT from the flip onset and the tier),
`same-different-image` (SOA on the frame grid), `serial-reaction-time`
with its mouse variant, `audio-keyboard-response` (RT from a sound
onset), `reconstruction` (method of adjustment), the contrib
`plugin-stop-signal` (staircase, sound and display on one clock), and
`extension-mouse-tracking` (pointer trace in the event ring).

### 14.8 License

jsPsych and every jspsych-contrib package are MIT (jsPsych: "Copyright (c)
2014-2022 Joshua R. de Leeuw"; contrib: the `license` field of all 61
`package.json` files, no root LICENSE file; each contrib package has its
own authors). jspsych-psychophysics is MIT, Copyright (c) 2019-2026
Daiichiro Kuroki. Behavior, parameter names and data column names are not
code: ysp reimplements them freely under MIT-0. Code copied from any of
them keeps the MIT notice and its copyright line, as the SDF functions in
`ysp/gfx.h` do. No code is copied in this section.

### 14.9 Not verified

- jspsych-contrib plugins other than `plugin-rdk`: only their
  `package.json` descriptions were read.
- The angle convention and `dot_life` unit of contrib `plugin-rdk`.
- That SDL gamepad and touch events reach `yscr_poll()` when the caller
  starts the gamepad subsystem; their timing is not measured.
- jsPsych's internal behavior beyond its documentation (no source read).

## 15. Coverage of PsychoPy's demos

Status: reference, 2026-10-09. This section maps each demo that PsychoPy
ships, Coder and Builder, to the ysp headers. It gives the status of
each demo, ranks the gaps, merges them with the jsPsych gaps of 14.7, and
proposes new C examples.

Sources, read on 2026-10-09:
- PsychoPy `dev` at `9535622` (2026-10-07), `psychopy/demos/`: 97 Coder
  scripts (the docstring and the first 45 lines of each; two helper
  modules, `iohub/serial/_parseserial.py` and
  `iohub/wintab/_wintabgraphics.py`, are counted with their demos), and
  43 Builder demos (each `README.md` or `readme.md`, and the component
  and loop types in each `.psyexp`; parameters read for `noise`,
  `gratings`, `counterbalance`, the EEG, LSL and fMRI demos).
- ysp: README.md, this document (sections 4, 11, 12, 14),
  `examples/README.md`, docs/response.md "What the examples show",
  docs/devices_spec.md 10.1, docs/pack.md (kinds), and the top-of-file
  manuals of `ysp/gfx.h` (STIMULI, SHADER CONTRACT, STATUS),
  `ysp/screen.h` (MULTIPLE DISPLAYS, gamepads), `ysp/rdk.h`,
  `ysp/color.h`, `ysp/video.h`, `ysp/audio.h`, `ysp/device.h`,
  `ysp/box.h`, `ysp/input.h`, `ysp/pack.h`, `ysp/table.h`,
  `ysp/stair.h` and `pack/ysp/layout.h`.

License: PsychoPy is GPL-3.0. 70 of the 99 Coder files say that their
contents are in the public domain; the rest, and the Builder demos, fall
under the GPL. This section was written from reading only. No code is
copied, and the proposed examples must not copy code either.

### 15.1 Status values

| Status | Meaning |
|---|---|
| Example | An example in `examples/` shows the same thing. The cell names it. |
| Possible | Every piece is built. A C program can do it today; no example shows it. |
| Gap | A piece is missing. The cell names it. |
| Out | Out of scope, with the reason. |

A Builder demo is a C program for a ysp user today. For a user who does
not write C, every Builder demo needs the player (rank 1 in 15.4). The
status columns below give the C status.

Totals: 140 demos (97 Coder, 43 Builder). Example 50 (36 + 14), Possible
44 (29 + 15), Gap 24 (12 + 12), Out 22 (20 + 2). Updated 2026-10-09 for
`gfx_gratings`, `trial_2afc_adaptive`, `trial_rdk`, `trial_stroop`,
`trial_sternberg` and `trial_images` (15 demos from Possible to Example).

### 15.2 Coder demos

Paths are from `psychopy/demos/coder/`.

#### Stimuli

| Demo | What it shows | ysp headers | Status |
|---|---|---|---|
| `basic/hello_world.py` | Two lines of text, one with an accented capital | gfx, outline | Example: `gfx_text` |
| `stimuli/gabor.py` | A gabor with drifting phase | gfx | Example: `gfx_hello` |
| `stimuli/counterphase.py` | A grating whose contrast follows a sine (counterphase flicker) | gfx, timeline | Example: `gfx_gratings` tile 1 (a contrast track; the achieved frequency from the flip records) |
| `stimuli/plaid.py` | Two gratings summed into a drifting plaid | gfx | Example: `gfx_gallery` page 1 (plaid tile) |
| `stimuli/secondOrderGratings.py` | Contrast-modulated gratings and beats on sine and noise carriers | gfx (USER shader, `ysp_hash2()` for a noise carrier) | Example: `gfx_gratings` tiles 2 and 3 (USER shaders; no built-in envelope kind) |
| `stimuli/rotatingFlashingWedge.py` | A rotating radial checkerboard wedge that flashes | gfx (USER shader), timeline | Example: `gfx_gratings` tile 4 (angle and polarity tracks) |
| `stimuli/aperture.py` | A gabor in an irregular aperture | gfx (MASK_TEX, POLYGON) | Example: `gfx_gallery` page 5 (grating in a mask aperture) |
| `stimuli/customTextures.py` | A radial texture from an array, and a sub-region of it | gfx (IMAGE from planes, USER) | Example: `gfx_gallery` pages 1 and 5 (radial USER shader, R32F image as a modulation) |
| `stimuli/visual_noise.py` | An array of noise as a texture, drifted by phase | gfx (NOISE, IMAGE) | Example: `gfx_gallery` pages 1 and 8 |
| `stimuli/dots.py` | A dot kinematogram: signal and noise dot rules (Scase et al.) | rdk, gfx | Example: `gfx_rdk` page 1 (`ysp/rdk.h` names PsychoPy's defaults); `trial_rdk` (as a trial with a response) |
| `stimuli/dot_gabors.py` | Gabors as the dots of a kinematogram | rdk, gfx (instances) | Example: `gfx_rdk` page 2 |
| `stimuli/elementArrays.py` | A global-form array: 500 gabors with orientation coherence | gfx (instances) | Example: `gfx_gallery` page 6 (400 gabors, one draw) |
| `stimuli/starField.py` | 500 dots moving out from the center | gfx (DOTS or instances) | Possible |
| `stimuli/maskReveal.py` | An image revealed by element opacities | gfx (instances) | Possible (per-element opacity not verified, 15.6) |
| `stimuli/shapes.py` | Polygons, a self-crossing shape, a shape with a hole, lines | gfx (POLYGON, COMPOUND, POLYLINE) | Example: `gfx_gallery` pages 2 and 3 |
| `stimuli/shapeContains.py` | Click inside a polygon; a circle that follows the mouse | gfx (`ygfx_hit()`), screen | Example: `gfx_gallery` page 6 (hit test under the mouse). `overlaps()` has no ysp analog |
| `stimuli/kanizsa.py` | Four pies make illusory contours | gfx (PIE) | Possible |
| `stimuli/clockface.py` | Clock hands from polygons, turned by the wall clock | gfx | Possible |
| `stimuli/face_jpg.py` | A JPEG image, and the image as a grating's mask | gfx (IMAGE), pack (TEXTURE) | Example: `trial_images` (a PNG through the pack tool, no JPEG; a graded mask by a target and MULTIPLY) and `gfx_gratings` tile 6 (an image as a USER shader's amplitude) |
| `stimuli/imagesAndPatches.py` | Images at pixel size, mirrored; an image as a mask; frame intervals | gfx, pack, screen | Possible, with the same mask question |
| `stimuli/bufferImageStim.py` | Many static stimuli captured into one image, and the speed gain | gfx (targets, setup passes) | Example: `gfx_gallery` page 5 (target drawn once), page 7 (static text rendered at setup) |
| `stimuli/variousVisualStims.py` | A gabor, a movie, text and an image turned by the mouse | gfx, video, screen | Possible |
| `stimuli/MovieStim.py` | A movie with sound; play, pause, stop by keys | video, audio | Example: `video_play` (keys for pause not shown) |
| `stimuli/soundStimuli.py` | Tones by note name and by frequency | audio | Example: `audio_tone` |
| `stimuli/ratingScale.py` | Rating scales: choices, a line, an accept button | None | Gap: widget layer (14.7 rank 4) |
| `stimuli/screensAndWindows.py` | Two windows, on one screen or on two | screen (one `yscr_screen` per display) | Possible on two displays. Two windows on one display: gap, untimed screens (4.2 proposal) |
| `stimuli/embeddedOpenGL.py` | Raw OpenGL calls among stimuli | gfx (USER pipeline) | Possible through the shader contract; raw GL in the frame is not supported. A Renderer extension (12) draws into a target |
| `stimuli/stim3d.py` | Lit, textured 3D boxes and spheres | None | Out: 3D is a Renderer extension (12), not a header |
| `stimuli/colors/colors.py` | Type a value in a color space, see the color | color, gfx | Possible: a key cycles the space in place of the slider. `color_convert` shows the conversions without a display |
| `stimuli/colors/hsvColorPalette.py` | An HSV color picker | color | Out: HSV is device RGB with no calibration, and every `ysp/color.h` conversion goes through one. A lightness and hue picker in CIELAB or Oklab is possible |

#### Text

| Demo | What it shows | ysp headers | Status |
|---|---|---|---|
| `stimuli/text/textStimuli.py` | Fonts, Unicode, turned and mirrored text, bidi Arabic | pack/layout, outline, gfx | Example: `gfx_layout` (seven scripts, bidi), `gfx_gallery` page 8 (turning text) |
| `stimuli/text/textbox_simple.py` | Two text boxes with options | pack/layout, gfx | Example: `gfx_text`, `gfx_layout` |
| `stimuli/text/textbox_editable.py` | An editable text box | pack/layout, screen (`yscr_text_input()`) | Example: `gfx_layout` (Skribidi's editor) |
| `stimuli/text/textbox_glyph_placement.py` | The box of the glyph at a string index, checked by the mouse | pack/layout, gfx (`ygfx_hit_index()` on a curve run) | Possible |
| `stimuli/text/fontLayout.py` | A diagram of the font metrics that place lines | outline (`yol_font_hmetrics()`, font metrics), gfx | Possible |
| `stimuli/compare_text_timing.py` | The cost to make, change and draw three text classes | gfx, outline | Example: `gfx_bench`; docs/gfx.md has the page costs |

#### Input

| Demo | What it shows | ysp headers | Status |
|---|---|---|---|
| `input/keyNameFinder.py` | The name of each key pressed | screen | Example: `screen_input --hand` (scancode names) |
| `input/mouse.py` | Mouse position, relative motion, wheel, buttons | screen, input | Possible; `screen_input --mice` for raw mice |
| `input/customMouse.py` | A drawn pointer with movement limits, click on release | screen (`show_cursor`), gfx | Possible |
| `input/joystick_universal.py` | Joystick axes, buttons and hats | screen (`desc.gamepads`), input | Possible for gamepads. Generic joysticks: not verified (15.6) |
| `input/GUI.py` | A dialog for session fields | None | Out: a desktop dialog. A C example takes arguments; the player's runner gives session fields (6) |

#### Timing

| Demo | What it shows | ysp headers | Status |
|---|---|---|---|
| `timing/timeByFrames.py` | Frame intervals: histogram, drops | screen | Example: `screen_flipstats` |
| `timing/timeByFramesEx.py` | The same with process priority and no garbage collection | screen, rt | Example: `screen_flipstats --load N` (no GC in C) |
| `timing/callOnFlip.py` | A function called at the flip, for a trigger | screen (flip hooks), device | Example: `device_trigger_flip` |
| `timing/clocksAndTimers.py` | Clocks, count-down timers | rt | Possible; `rt_jitter` shows what the waits are worth |
| `timing/millikeyKeyboardTimingTest.py` | Keyboard stamp error against key presses a MilliKey generates on command | serial (the command), screen (the key) | Possible: a MilliKey is a keyboard-mode box (docs/devices_spec.md 10.1). `screen_input` measures injected keys in software |

#### Experiment control

| Demo | What it shows | ysp headers | Status |
|---|---|---|---|
| `experiment control/trialHandler.py` | A trial loop over a factorial list | trials | Example: `trials_mocs` |
| `experiment control/trialHandler2.py` | The same, with upcoming trials computed on the fly | trials | Example: `trials_mocs`, `trials_interleave` |
| `experiment control/stairHandler.py` | A 1-up 1-down staircase with step sizes per reversal | stair | Example: `stair_sim` |
| `experiment control/JND_staircase_exp.py` | An orientation JND by staircase, with a dialog | gfx, stair, trials, response | Example: `trial_2afc_adaptive` (contrast detection in place of an orientation JND; no dialog) |
| `experiment control/JND_staircase_analysis.py` | A psychometric fit over staircase files | None | Out: offline fitting (4.8) |
| `experiment control/experimentHandler.py` | One data file over loops, with session fields | trials (`ytr_format_meta()`, rows) | Example: `trial_keyboard --out` |
| `experiment control/logFiles.py` | Log levels to files and console | rt (the ring) | Example: `rt_ring_csv` |
| `experiment control/autoDraw_autoLog.py` | Stimuli drawn and logged each frame without a call | timeline, gfx | Out: a Python convenience. Timeline on and off events with their records do this (`gfx_trial`) |
| `experiment control/piloting.py` | A pilot flag: windowed, short, marked | screen | Possible: a flag; `--sim` in the examples is the nearest form |
| `experiment control/runtimeInfo.py` | System, Python and window facts at run time | screen (`yscr_describe()`), gfx, audio | Possible |
| `sysInfo.py` | Paths, OS, library versions | screen, gfx | Possible (the describe lines) |
| `csvFromPsydat.py` | A `.psydat` file to CSV | None | Out: PsychoPy's own file format |

#### Hardware

| Demo | What it shows | ysp headers | Status |
|---|---|---|---|
| `hardware/parallelPortOutput.py` | A pin held high for N frames with a stimulus | parallel, device, screen | Example: `parallel_trigger`, `device_trigger_flip` |
| `hardware/cedrusRB730.py` | A Cedrus RB-730: info, round trip, keys | device (XID), box | Possible (no box on hand; docs/devices_spec.md) |
| `hardware/fMRI_launchScan.py` | Wait for scanner sync pulses, or emulate them | screen (a pulse as a key), device (`YIN_KIND_SYNC` from a board), timeline | Possible; the pulse emulator is caller code |
| `hardware/monitorDemo.py` | Monitor profiles: distance, calibration by name | color (`ycol_cal` load), gfx (`ygfx_view`) | Possible |
| `hardware/gammaMotionNull.py` | Display gamma by motion nulling (Ledgeway and Smith 1994) | gfx, stair, response | Possible (proposed: `gfx_gamma_null`, 15.5) |
| `hardware/gammaMotionAnalysis.py` | Plots of the nulling staircases | None | Out: offline analysis (4.8) |
| `hardware/testSoundLatency.py` | Sound onset latency measured by a LabJack | audio | Possible by line-in: `tests/loopback/audio_loopback.c` (a test, not an example) |
| `hardware/VSHD_Distortion.py` | Barrel distortion for an in-scanner display | gfx (target, USER shader) | Example: `gfx_gratings` tile 5 (a target through a USER shader, ysp/gfx.h v0.10.4) |
| `hardware/CRS_BitsBox.py`, `CRS_BitsPlusPlus.py`, `crsBitsAdvancedDemo.py` (3) | Bits++ and Bits# modes, CLUTs, digital I/O | None | Gap: high bit depth output (Bits++, Mono++, Color++). `ysp/gfx.h` refuses it until a device verifies it |
| `hardware/cameraLiveView.py`, `cameraSideBySide.py` (2) | Live camera feeds in a window | gfx (`ygfx_texture_update()`) | Gap: camera capture (4.6; 14.7 rank 7) |
| `hardware/labjack_u3.py` | LabJack DAC and digital output | None | Gap: `ysp/labjack.h` (docs/devices_spec.md, later) |
| `hardware/egi_netstation.py` | EGI NetStation: session, events over TCP | None | Gap: `ysp/net.h` (4.7) and the NetStation protocol (not in docs/devices_spec.md) |
| `hardware/RiftMinimal.py`, `RiftHeadTrackingExample.py` (2) | Oculus Rift rendering and head tracking | None | Out: HMD VR (4.8) |
| `hardware/ioLab_bbox.py` | An ioLabs button box | None | Out: not a device ysp targets (not in docs/devices_spec.md) |
| `hardware/qmixPump.py` | A Cetoni syringe pump | None | Out: not a device ysp targets |
| `hardware/hdf5_extract.py` | Gaze from an ioHub HDF5 file, animated | None | Out: ioHub's file format; offline |

#### ioHub

| Demo | What it shows | ysp headers | Status |
|---|---|---|---|
| `iohub/keyboard.py` | ioHub key events against `getKeys()`: press, release, modifiers | screen, input | Example: `screen_input --hand` |
| `iohub/keyboardreactiontime.py` | Key RT in a line-length match | gfx, response | Example: `trial_keyboard` |
| `iohub/mouse.py` | ioHub mouse events, position set | screen, input | Possible |
| `iohub/mouse_multi_window.py` | Mouse positions over windows on two monitors | screen | Possible (not verified across two screens, 15.6) |
| `iohub/serial/customparser.py` (with `_parseserial.py`) | A serial device's bytes parsed into events | serial, box | Possible: a decoder in `ysp/box.h` or caller code on `ysp/serial.h`; `serial_trigger` shows the echo |
| `iohub/serial/pstbox.py` | A PST Serial Response Box (the file says it does not work in Python 3) | None | Gap: a bit-state box decoder in `ysp/box.h` (docs/devices_spec.md 10.1) |
| `iohub/wintab/pen_demo.py` (with `_wintabgraphics.py`) | Pen position, pressure and tilt from a Wintab tablet | screen, input (`YIN_KIND_PEN`) | Possible through SDL's pen events (not verified with a tablet, 15.6) |
| `iohub/eyetracking/simple.py` | An eye tracker: calibrate, record, gaze | None | Gap: eye-tracker input source (12; 14.7 rank 6) |
| `iohub/eyetracking/validation.py` | Calibrate, validate, gaze cursor | None | Gap: as above |
| `iohub/eyetracking/gcCursor/run.py` | A gaze cursor over images | None | Gap: as above |
| `iohub/eyetracking/gcCursor/readTrialEventsByConditionVariables.py`, `readTrialEventsByMessages.py` (2) | Samples from HDF5 split into trials | None | Out: ioHub's file format; offline |
| `iohub/iodatastore/saveEventReport.py` | Events from HDF5 to text | None | Out: as above |
| `iohub/launchHub.py` | ioHub server start options | None | Out: ysp has no input server process; events are restamped in the frame thread |
| `iohub/delaytest.py` | The round trip to the ioHub process | None | Out: as above. `screen_input` measures the stamp error that matters |

#### Misc

| Demo | What it shows | ysp headers | Status |
|---|---|---|---|
| `misc/makeMovie.py` | Frames captured into a movie file | gfx (`ygfx_read_target()`), video (frame sequence writer) | Possible |
| `misc/encrypt_data.py` | Encrypt a data file | None | Out: a separate tool's job |
| `misc/hdf5_2_csv` | Eye-tracker HDF5 to CSV | None | Out: ioHub's file format |
| `misc/rigidBodyTransform.py` | Poses of 3D objects | None | Out: 3D (12) |

### 15.3 Builder demos

Paths are from `psychopy/demos/builder/`. `Experiments/GoNoGo/` and
`Experiments/goNoGo/` (a README only) are one demo. The three
`lab_streaming_layer*` variants are one demo.

#### Design templates and experiments

| Demo | What it shows | ysp headers | Status |
|---|---|---|---|
| `Design Templates/branchedExperiment/` | A loop ended early by setting `finished` | trials | Possible in C. Player: conditional and loop nodes (14.5) |
| `Design Templates/randomisedBlocks/` | Image blocks in random order, images random within a block | trials (groups), gfx, pack | Example: `trial_images` |
| `Design Templates/psychophysicsStaircase/` | Gabor detection, yes or no, 3-down 1-up on contrast | gfx, stair, response | Example: `trial_2afc_adaptive` (3-down 1-up on contrast, as 2AFC in place of yes or no) |
| `Design Templates/psychophysicsStairsInterleaved/` | 2AFC detection, four interleaved staircases over two spatial frequencies, in deg | gfx, trials (tracks), stair, response | Example: `trial_2afc_adaptive` (a staircase and a QUEST+ track interleaved, one spatial frequency, in deg) |
| `Design Templates/dualWindow/` | A Stroop task with an experimenter window that shows progress | screen | Gap: untimed screens (4.2 proposal) |
| `Experiments/stroop/` | Colored color words, key per ink color | gfx, pack/layout or outline, trials, table, response | Example: `trial_stroop` |
| `Experiments/stroopExtended/` | Stroop with practice and feedback; reverse Stroop | as above | Example: `trial_stroop` (practice with feedback; no reverse Stroop) |
| `Experiments/stroopVoice/` | Spoken responses, transcribed by Whisper | None | Gap: microphone capture (14.7 rank 7). Transcription: out (a speech model is not a rig header) |
| `Experiments/GoNoGo/` | Go and no-go images, 25 % no-go | gfx, pack, trials, response | Possible (`trial_stop_signal` has the no-response outcome) |
| `Experiments/sternberg/` | A memory set, then a probe: present or absent | gfx, timeline, trials, response | Example: `trial_sternberg` |
| `Experiments/navon/` | Global and local letters, congruent or not | gfx, pack, trials, response | Example: `trial_images` |
| `Experiments/mentalRotation/` | Rotated letter pairs, same or different; a plot at the end | gfx (IMAGE `ori`), trials, response | Example: `trial_images` (one turned letter, normal or mirrored, in place of a pair); the plot is out (4.8) |
| `Experiments/BART/` | Balloon risk task: pump by key, burst sound, earnings | gfx, audio, pack/layout, response | Possible (MP3 converted to WAV) |
| `Experiments/dragAndDrop/` | Drag shapes into place; positions saved | gfx, screen | Gap: drag helper (14.7 rank 8) |
| `Experiments/BigFiveInventory/` | Personality questionnaires as forms | None | Out: a survey form (as 14.3 `survey`) |

#### Feature demos

| Demo | What it shows | ysp headers | Status |
|---|---|---|---|
| `Feature Demos/gratings/` | Contrast-modulated sine and noise, beats | gfx (USER shader) | Example: `gfx_gratings` |
| `Feature Demos/noise/` | Binary, filtered (band-pass), Gabor and image noise; new samples per repeat | gfx (NOISE) | Gap: filtered noise. Binary, uniform, Gaussian and simplex exist; `ysp/gfx.h` lists filtered noise on the GPU as not done |
| `Feature Demos/movies/` | A movie with play, pause and seek by a button and a slider | video, gfx | Possible with keys for the controls (the widgets: 14.7 rank 4) |
| `Feature Demos/panorama/` | A 360 degree image, looked around by mouse or keys | gfx (USER shader, IMAGE) | Possible (texture size and target sampling not verified, 15.6) |
| `Feature Demos/sliders/` | Slider styles, vertical and horizontal | None | Gap: widget layer (14.7 rank 4) |
| `Feature Demos/progress/` | A progress bar | gfx | Possible |
| `Feature Demos/pilotMode/` | `PILOTING` skips routines and makes dummy responses | trials, simulated observers | Possible |
| `Feature Demos/counterbalance/` | A group from a slot counter that persists across runs | trials (`ytr_latin()`) | Possible for the assignment. A slot quota shared across sessions is the runner's (6) |
| `Feature Demos/buttonBox/` | A button box, or the keyboard in its place, lights circles | device (XID or line board), screen | Possible |
| `Feature Demos/visualValidator/` | A light sensor checks each stimulus's on and off times | device, screen | Example: `photodiode_check` (edge minus flip onset) |

#### Hardware

| Demo | What it shows | ysp headers | Status |
|---|---|---|---|
| `Hardware/EEG_parallel_component/` | Parallel port codes at a stimulus, a key, a click | device (parallel), screen, response | Example: `device_trigger_flip`, `parallel_trigger`. Response-locked codes are caller code (proposed: `trial_eeg_triggers`) |
| `Hardware/EEG_serial_component/` | The same through a serial trigger device | device, screen | Example: `device_trigger_flip --out` |
| `Hardware/EEG_serial_code/` | Serial triggers written from code | serial, device | Example: `serial_trigger`, `device_trigger_flip` |
| `Hardware/fMRI/` | Wait for the scanner's key, then non-slip timing | screen, input, timeline | Possible (proposed: `trial_scanner_sync`) |
| `Hardware/EGI_netstation/` | Stroop with NetStation tags | None | Gap: `ysp/net.h` and the NetStation protocol |
| `Hardware/lab_streaming_layer/` (and `lab_streaming_layer_legacy/`) | LSL markers at a stimulus and a key | None | Gap: `ysp/net.h` LSL outlet (4.7) |
| `Hardware/eyetracking/` | Calibration, recording, an ROI and a gaze cursor | None | Gap: eye-tracker input source (14.7 rank 6) |
| `Hardware/eyetracking_custom_cal/` | The same with a custom calibration | None | Gap: as above |
| `Hardware/Eyetracking_visual_search/` | Gaze-driven instructions, fixation check and search | None | Gap: as above |
| `Hardware/camera/` | Record a webcam, show it live, replay it | None | Gap: camera capture (14.7 rank 7) |
| `Hardware/microphone/` | Record and transcribe read phrases | None | Gap: microphone capture; transcription out |
| `Hardware/pump/` | Fill Cetoni syringe pumps | None | Out: not a device ysp targets |

#### Helper tools

| Demo | What it shows | ysp headers | Status |
|---|---|---|---|
| `Helper Tools/keyNameFinder/` | Key names | screen | Example: `screen_input --hand` |
| `Helper Tools/spatialUnits/` | One square in several units | gfx (`ygfx_view`, `YGFX_DEG`) | Possible |
| `Helper Tools/clockFace/` | Clock hands turned by routine time | gfx, timeline | Possible |
| `Helper Tools/colors/` | Type a color in a space, see it | color, gfx | Possible with keys; `color_convert` |
| `Helper Tools/drawPolygon/` | Click vertices, then change size, position and angle by sliders | gfx (POLYGON to 16 vertices, POLYLINE to 224 points), screen | Possible with keys in place of the sliders |
| `Helper Tools/achorVSalignment/` | Text anchor against text alignment, set by sliders | gfx (`anchor`), pack/layout | Possible with keys |

### 15.4 Gaps ranked by the demos they block

"Blocks" counts the demos with status Gap. "Burdens" counts demos a C
user can do today, but with work each time or with a conversion. The
14.7 column gives the jsPsych rank where the gap is the same.

| Rank | Gap | 14.7 | Where it goes | Blocks | Burdens | Size |
|---|---|---|---|---|---|---|
| 1 | Experiment flow: loops, routines, conditions files, branching, session fields | 1 | The player (11, item 7) | All 43 Builder demos for users who do not write C | None in C | Large; planned |
| 2 | Eye-tracker input source, calibration, validation, ROI and fixation checks | 6 | Extension (12); `ysp/eyelink.h`, `ysp/tobii.h` (docs/devices_spec.md) | 6: `iohub/eyetracking/simple.py`, `validation.py`, `gcCursor/run.py`, `Hardware/eyetracking/`, `eyetracking_custom_cal/`, `Eyetracking_visual_search/` | None | Medium per tracker |
| 3 | Capture: camera and microphone | 7 | `ysp/video.h` capture (4.6); `ysp/audio.h` input | 5: camera 3 (`cameraLiveView.py`, `cameraSideBySide.py`, `Hardware/camera/`), microphone 2 (`Hardware/microphone/`, `stroopVoice/`) | None | Medium |
| 4 | Network markers: LSL outlets, EGI NetStation | None (new) | `ysp/net.h` (4.7) | 3: `Hardware/lab_streaming_layer/`, `Hardware/EGI_netstation/`, `hardware/egi_netstation.py` | None | Small for LSL (liblsl at run time); NetStation not specified |
| 5 | High bit depth output: Bits++, Bits#, Mono++, Color++ | None (new) | `ysp/gfx.h` output stage | 3: the CRS demos | None | Small in code; needs a device to verify |
| 6 | Widget layer: slider, rating scale, button, text entry | 4 | Player, above `ysp/gfx.h` | 2: `ratingScale.py`, `Feature Demos/sliders/` | 6: `movies/`, `drawPolygon/`, `achorVSalignment/`, both `colors` demos, `BigFiveInventory/` if forms come in scope | Medium |
| 7 | Drag helper | 8 | Player; an example | 1: `dragAndDrop/` | None | Small |
| 8 | Filtered noise: band-pass, 1/f, Gabor noise, phase-scrambled images | None (new) | `ysp/gfx.h` (GPU) or a CPU filter into an IMAGE | 1: `Feature Demos/noise/` | None | Small to medium |
| 9 | Untimed screens: an operator window beside the stimulus | None (new) | `ysp/screen.h` (4.2 proposal) | 1: `dualWindow/` | 1: `screensAndWindows.py` (two windows on one display) | Small |
| 10 | Bit-state box decoder (PST SRBox) | None | `ysp/box.h` (docs/devices_spec.md 10.1) | 1: `pstbox.py` | None | Small |
| 11 | LabJack glue | None | `ysp/labjack.h` (docs/devices_spec.md) | 1: `labjack_u3.py` | None | Small |
| Burden | Source formats: `.xlsx` conditions, JPEG, MP3 | 5 (images) | Pack tool importers (docs/pack.md takes CSV, PNG, WAV) | None | 16 demos load `.xlsx` conditions; 11 use JPEG; 1 uses MP3 | Small each |
| Burden | Text in C | 3 | Done for C: `pack/layout` v0.1.0 (Skribidi, `YSP_BUILD_LAYOUT`); the player's text is not built | None | Every Builder demo with instructions or word stimuli | None left in C; a build option and `third_party/` |

Notes on the merge with 14.7:
- 14.7 ranks 1, 4, 6, 7 and 8 are the same gaps here. PsychoPy's demos
  rank the eye tracker and capture higher than jsPsych's plugins do,
  because PsychoPy is a lab tool. This agrees with the note in 14.7 rank
  6.
- 14.7 rank 3 (text) is closed for C programs since `pack/layout`
  v0.1.0. It stays open for the player.
- 14.7 rank 5 (image decode) is closed for PNG by the pack tool
  (lodepng). JPEG is not taken; PsychoPy's demos use it for photographs.
- Ranks 4, 5, 8, 9, 10 and 11 have no jsPsych counterpart. Ranks 4,
  10 and 11 are already in docs/devices_spec.md.

### 15.5 Proposed C examples

Each one uses headers that no windowed example combines today: `ysp/quest.h`
and `ysp/color.h` in a display, `ysp/rdk.h` and `ysp/video.h` with a
response, `ysp/pack.h` into `ysp/gfx.h`, `pack/layout` in a timed trial,
`ysp/input.h` SYNC events, and `ysp/device.h` outputs with a stimulus and
a response. Frame-interval measurement is not proposed:
`screen_flipstats` covers `timeByFrames.py`. A rating scale or slider is
not proposed: it needs the widget layer (15.4 rank 6), and an example
would build a private one. Sizes are estimates, from
the response examples (330 to 500 lines).

| Order | Program (file) | Mirrors | Headers | What it shows that no example shows | Lines | Hardware |
|---|---|---|---|---|---|---|
| 1 | `trial_2afc_adaptive` (`examples/response/`), built | `psychophysicsStairsInterleaved/`, `psychophysicsStaircase/`, `JND_staircase_exp.py` | gfx, color, trials, stair, quest, response, timeline | A spatial 2AFC gabor detection on a display: two staircases and one QUEST+ track interleaved in `ysp/trials.h`; contrast set through the calibration with its gamut check; sizes in deg. Today the adaptive headers run only against simulated observers | 450 | None (a nominal calibration unless a `.yspcal` is given) |
| 2 | `trial_rdk` (`examples/rdk/`), built | `dots.py`, `dot_gabors.py`; contrib `plugin-rdk` (14.4) | rdk, gfx, timeline, trials, response | An RDK direction discrimination with RT from the motion onset's flip record, coherence per trial from a factor, and the field's replay digest in each data row | 350 | None |
| 3 | `trial_stroop` (`examples/response/`), built | `stroop/`, `stroopExtended/`, `EGI_netstation/` (task part) | pack/layout, outline, gfx, table, trials, response | Words as text in a timed trial: color words in an ink color, conditions from a CSV through `ysp/table.h`, a practice block with feedback text, congruency in the row | 400 | None; needs `YSP_BUILD_LAYOUT` (or one-line words by advances, as `outline_font`) |
| 4 | `trial_movie` (`examples/video/`) | `Feature Demos/movies/`, `MovieStim.py`; jsPsych `video-keyboard-response` (14.3) | video, audio, timeline, gfx, response | RT from a video frame's flip record (the frame named in the trial row); pause and seek by key; each trial's shown, repeated and dropped counts | 350 | A sound device for the soundtrack (none with `--sim`) |
| 5 | `gfx_gratings` (`examples/gfx/`), built | `counterphase.py`, `secondOrderGratings.py`, `rotatingFlashingWedge.py`, `Feature Demos/gratings/` | gfx (GRATING, GABOR, USER), timeline | Phase drift on a timeline track; counterphase flicker as a contrast track, with the achieved frequency from the flip records; contrast-modulated gratings and a noise-carrier envelope in USER shaders; a flashing checkerboard wedge | 400 | None |
| 6 | `trial_sternberg` (`examples/timeline/`), built | `sternberg/`; jsPsych `animation` (14.3) | timeline (`ytl_seq` op tables), gfx, outline, trials, response | A timed sequence: a memory set of 1 to 6 digits at a fixed SOA, a probe, and the landing record of each item; the SOAs measured from the flip records; set size as a factor | 350 | None |
| 7 | `color_patch` (`examples/color/`) | `monitorDemo.py`, `colors/colors.py`, `Helper Tools/colors/`, `hsvColorPalette.py` (as a CIELAB picker) | color, gfx, screen | A calibrated patch: a `.yspcal` loaded; a color given in CIELAB, xyY or DKL; keys walk hue and chroma; the device RGB, the gamut distance and refusals printed; the OS gamma ramp left alone | 250 | A calibration of the display for a claim about light; a nominal one otherwise (flagged) |
| 8 | `trial_eeg_triggers` (`examples/device/`) | `EEG_parallel_component/`, `EEG_serial_component/`, `EEG_serial_code/`, `parallelPortOutput.py`, `callOnFlip.py` | device, screen, gfx, timeline, trials, response | A stimulus code at the flip through the trigger channel, a response code at the key event, and an end code; each write's record beside the flip and response records in the row. `device_trigger_flip` has no stimulus or response | 350 | A trigger output for real lines; the in-process box without one |
| 9 | `trial_images` (`examples/pack/`), built | `randomisedBlocks/`, `GoNoGo/`, `navon/`, `mentalRotation/`, `face_jpg.py` | pack, gfx (IMAGE), table, trials (groups), response | The pack path end to end: textures and the conditions table read from a pack built by `ypak`; blocks in random order; images turned for mental rotation | 350 | None; needs a pack (`ypak build`, `YSP_BUILD_PACK`) |
| 10 | `trial_instructions` (`examples/layout/`) | `hello_world.py`, `textbox_simple.py`; jsPsych `instructions` (14.3) | pack/layout, outline, gfx (setup passes, targets), screen | Paragraph pages laid out once and rendered into targets at setup, turned by keys; the time each page was shown, from the flip records | 250 | None; needs `YSP_BUILD_LAYOUT` |
| 11 | `trial_scanner_sync` (`examples/timeline/`) | `Hardware/fMRI/`, `fMRI_launchScan.py` | input (`YIN_KIND_SYNC`), device or screen, timeline, trials | Trials on a base anchored at a scanner pulse (non-slip timing): pulses from a key or a line board, each onset as an offset from the volume pulse; an emulator thread for runs without a scanner | 300 | A scanner or a line board for real pulses; the emulator without |
| 12 | `gfx_gamma_null` (`examples/gfx/`) | `gammaMotionNull.py` | gfx (square-wave gratings, output stage), stair, response, timeline | A psychophysical check of the display's linearization by motion nulling, through the output stage's lookup table, with no photometer | 300 | None |

Examples 1 to 4 close the first-order holes: adaptive methods, RDK and
video are built and tested but no example runs them with a participant.
Examples 8 and 11 need hardware for their claims and run without it.
Built by 2026-10-09: 1, 2, 3, 5, 6 and 9 ("built" in the Program
column); 15.2 and 15.3 name them.

### 15.6 Not verified

- Coder demos past their first 45 lines, including the 780-line
  `crsBitsAdvancedDemo.py` and the Wintab and eye-tracking demos.
- Builder demo parameters other than the ones named in the sources
  above. The status rests on README text and component types.
- PsychoPy's library behavior: no library source was read.
- The largest texture a panorama can use (`caps.max_texture`). Answered
  on 2026-10-09: a USER shader samples a render target or an image as
  `ysp_tex0` from `ysp/gfx.h` v0.10.4 (`stim.tex`); v0.10.3 bound no
  texture for a USER draw (docs/gfx.md, v0.10.4). `gfx_gratings` tile 5
  shows a target through a lens shader (`VSHD_Distortion.py`).
- Answered on 2026-10-09: a graded image as a grating's mask
  (`face_jpg.py`, `imagesAndPatches.py`) works by a USER shader that
  multiplies a carrier by the image (`gfx_gratings` tile 6, v0.10.4).
  The target and MULTIPLY route is `trial_images`'s.
- Per-element opacity in instance records (`maskReveal.py`).
- That SDL3's pen events from a Wintab tablet reach `yscr_poll()` with
  pressure and tilt; that generic joysticks (not mapped as gamepads)
  give events with `desc.gamepads`; mouse positions over two
  `yscr_screen` windows.
- The MilliKey's serial command syntax, the NetStation protocol, and a
  real Cedrus box with `ysp/device.h`.
- The asset counts in 15.4 (`.xlsx`, JPEG, MP3) come from the file names
  in the demo folders and the names in each `.psyexp` and script, not
  from a run.

## 16. Coverage of Psychtoolbox-3's demos

Status: reference, 2026-10-09. This section maps each demo that
Psychtoolbox-3 (PTB) ships in `Psychtoolbox/PsychDemos/` to the ysp
headers, and the timing and precision tests in `Psychtoolbox/PsychTests/`
that have a ysp counterpart. It compares ysp with PTB on the features PTB
is known for, ranks the gaps, merges them with 14.7 and 15.4, and
proposes new C examples.

Sources, read on 2026-10-09:
- Psychtoolbox-3 `master` at `8f952b4` (2026-08-14, "Merge pull request
  #298"), through `gh api`: the file tree; the help text (the first
  comment block, up to 12 lines) of the 154 demo files in
  `Psychtoolbox/PsychDemos/` and its subfolders (`Contents.m` files and
  data files not counted); a few demo bodies where the help text was
  empty or copied from another demo (`FloatTextureDemo.m`,
  `ImageWarpingDemo.m`, `MinExpEntStairDemo.m`,
  `PsychTutorials/ImagingVideoCaptureDemo.m`,
  `VideoTextureExtractionDemo.m`, the output-device list of
  `PsychTutorials/AdditiveBlendingForLinearSuperpositionTutorial.m`); the
  help text of 35 tests in `Psychtoolbox/PsychTests/` (VBLSyncTest,
  PerceptualVBLSyncTest and the others in 16.3); `Psychtoolbox/License.txt`
  and the help text of `Psychtoolbox/PsychLicenseHandling.m`. File names
  only: `PsychHardware/EyelinkToolbox/EyelinkDemos/SR-ResearchDemos/` (10
  demos) and `PsychHardware/BitsPlusToolbox/BitsPlusDemos/` (8 files).
- ysp: README.md, this document (sections 4, 8, 9, 11 to 15),
  `examples/README.md` (with the uncommitted `gfx_gratings`,
  `trial_images`, `trial_rdk`, `trial_2afc_adaptive`, `trial_stroop`,
  `trial_sternberg` and the `net` examples), docs/devices_spec.md (10.1,
  10.2 and the prior-art table), docs/gfx.md (the 10-bit rows),
  docs/rt.md (clock widths), docs/response.md (two keyboards), and the
  top-of-file manuals and STATUS blocks of `ysp/screen.h`, `ysp/gfx.h`,
  `ysp/color.h`, `ysp/audio.h`, `ysp/video.h`, `ysp/rt.h`,
  `ysp/input.h`, `ysp/device.h`, `ysp/box.h`, `ysp/net.h`,
  `ysp/serial.h`, `ysp/parallel.h` and `ysp/rdk.h`.

License: `Psychtoolbox/License.txt` puts material with no license of its
own under MIT (copyright the PTB core developers). No demo file has a
license header. Some demos name other terms for parts they use: the
OpenGL for Matlab toolbox (`SpinningCubeDemo.m`, `UtahTeapotDemo.m`) and
the earth image of kdeworldclock (`CylinderAnnulusOpenGLDemo.m`,
`MinimalisticOpenGLDemo.m`, `HDRMinimalisticOpenGLDemo.m`) are GPL; the
Mandelbrot shader is 3Dlabs'; `MultiTouchPinchDemo.m` holds a
BSD-2-Clause function. Of the tests, `CIEConeFundamentalsTest.m` and
`FitConeFundamentalsTest.m` mention GPL. The Datapixx toolbox is LGPL.
`PsychLicenseHandling.m` says that the prebuilt mex files on some
operating systems need a paid license or a time-limited trial, with
online activation, from the Medical Innovations Incubator GmbH (since
December 2024); which platforms, it does not list in the lines read.
This section was written from reading only. No code is copied, and the
proposed examples must not copy code either. Behavior, test procedures
and names are not code.

### 16.1 Status values

As in 15.1: Example, Possible, Gap, Out. PTB demos are scripts with no
experiment layer, so no status here depends on the player.

Totals: 154 demos. Example 45, Possible 53, Gap 20, Out 36. The 35
tests in 16.3 are counted apart: Example 13, Possible 5, Gap 14, Out 3.
Recounted 2026-10-09 after the 16.6 examples and `screen_sync_check`; at
the survey: Example 37, Possible 61 (demos), Example 8, Possible 10
(tests).

### 16.2 PsychDemos

Paths are from `Psychtoolbox/PsychDemos/`. The top-level folder holds 112
demos; its tables are split by topic here, but in PTB they are one
folder.

#### Top level: gratings, gabors, noise and procedural textures

| Demo | What it shows | ysp headers | Status |
|---|---|---|---|
| `AlphaRotateDemo.m` | A turning grating under a Gaussian transparency mask | gfx (GABOR, or GRATING with a Gaussian edge; `ori` on a timeline track) | Possible |
| `ContrastModulatedNoiseTheClumsyStyleDemo.m` | Noise with another contrast inside a disk the mouse drags; keys set the contrast | gfx (NOISE, USER), screen | Example: `gfx_gratings` (a contrast-modulated noise carrier); the mouse disk is not shown |
| `ContrastModulatedNoiseTheElegantStyleDemo.m` | The same in a float framebuffer | gfx (float scene) | Example: `gfx_gratings` |
| `DriftDemo.m` | A drifting grating, one texture per frame | gfx, timeline | Example: `gfx_hello` |
| `DriftDemo2.m` | A drifting masked grating from one texture shifted each frame | gfx | Example: `gfx_hello` (procedural, no texture) |
| `DriftDemo3.m` | The same in the least code | gfx | Example: `gfx_hello` |
| `DriftDemo4.m` | A drifting grating from a procedural shader | gfx (GRATING) | Example: `gfx_hello`, `gfx_gratings` |
| `DriftDemo5.m` | A drifting grating in a disk, another grating in an annulus around it | gfx (GRATING, ANNULUS aperture) | Possible |
| `DriftDemo6.m` | The same, combined by texture draw shaders | gfx | Possible |
| `DriftWaitDemo.m` | A grating that changes every n-th refresh (old WaitBlanking) | screen (`yscr_flip_at(f.onset + n * f.period)`), gfx | Possible |
| `ExpandingRingsDemo.m` | Expanding rings from a procedural shader | gfx (USER) | Possible: `gfx_gratings` has a radial USER shader (the wedge) |
| `FastFilteredNoiseDemo.m` | Noise filtered on the GPU each frame (Gaussian and other kernels), a benchmark | gfx | Gap: filtered noise on the GPU (15.4 rank 8). On the CPU into an IMAGE: Example: `gfx_filtered_noise` |
| `FastMaskedNoiseDemo.m` | Many noise patches a frame in circular apertures | gfx (NOISE, CIRCLE aperture) | Example: `gfx_gallery` pages 1 and 8; `gfx_load` for the load |
| `FastNoiseDemo.m` | Noise patches made each frame, a benchmark | gfx (NOISE) | Example: `gfx_gallery` page 1, `gfx_load` |
| `GarboriumDemo.m` | Hundreds of moving gabors as textures, summed by additive blending | gfx (instances, ADD) | Example: `gfx_gallery` page 6 (400 gabors, one draw), `gfx_load --gabors` (the cost); the motion is not shown |
| `GratingDemo.m` | A stationary grating | gfx (GRATING) | Example: `gfx_gallery` page 1 |
| `MandelbrotDemo.m` | The Mandelbrot set as a procedural texture | gfx (USER) | Possible |
| `ProceduralColorGratingDemo.m` | A grating between two colors, sine or square, in a round aperture | gfx (GRATING with `dir`, or PAINT) | Possible |
| `ProceduralGaborDemo.m` | Gabors from a procedural shader, a benchmark | gfx (GABOR) | Example: `gfx_hello`; `gfx_bench` (1000 gabors) |
| `ProceduralGarboriumDemo.m` | Procedural gabors in an "aquarium", summed | gfx (instances) | Example: `gfx_gallery` page 6, `gfx_bench` |
| `ProceduralNoiseDemo.m` | Procedural noise, a benchmark and a histogram check | gfx (NOISE UNIFORM, GAUSSIAN) | Example: `gfx_gallery` page 8 (the histograms) |
| `ProceduralSmoothedApertureSineGratingDemo.m` | Sine gratings in a smooth-edged aperture | gfx (GRATING, edge profile) | Possible |
| `ProceduralSmoothedDiscMaskDemo.m` | 700 smooth discs inside a smooth disc mask | gfx (instances, an aperture) | Possible |
| `ProceduralSmoothedDiscsDemo.m` | Smooth discs with an alpha per disc | gfx (instances) | Possible (per-element opacity not verified, 15.6) |
| `ProceduralSquareWaveDemo.m` | Square-wave gratings in an aperture | gfx (GRATING, wave 1) | Possible |

#### Top level: shapes, dots, images and the output stage

| Demo | What it shows | ysp headers | Status |
|---|---|---|---|
| `AlphaImageDemo/AlphaImageDemo.m` | An image under a Gaussian mask that follows the cursor | gfx (IMAGE, MASK_TEX), screen | Possible: `trial_images` has a graded mask (a target and MULTIPLY); the cursor is not shown |
| `ArcDemo.m` | Filled arcs as a pie chart | gfx (PIE, ARC) | Example: `gfx_gallery` pages 2 and 3 |
| `BubbleDemo.m` | Two images blended by a Gaussian mask at the gaze; the mouse stands in for the gaze | gfx (targets, MULTIPLY, USER), screen | Example with the mouse: `gfx_gaze_contingent` (a sharp image over its blurred copy). An eye tracker: gap (16.5 rank 6) |
| `ClutAnimDemo.m` | Animation by CLUT changes, at once or at the flip | gfx (`ygfx_set_lut()` between frames) | Example: `gfx_clut_sync` (the change lands on its own frame: read back) |
| `DotDemo.m` | A moving dot field; sprites; squares at subpixel places | rdk, gfx (DOTS, instances) | Example: `gfx_rdk` |
| `DotDemoStencil.m` | The same through a stencil aperture | rdk, gfx (aperture) | Example: `gfx_rdk` |
| `DotRotDemo.m` | Dots in a rotation pattern | gfx (DOTS; positions from the caller) | Possible |
| `GazeContingentDemo.m` | A gaze-contingent blend of two images; the mouse stands in | gfx, screen | Example with the mouse: `gfx_gaze_contingent`. An eye tracker: gap |
| `ImageUndistortionDemo.m` | Geometric undistortion of an image from a calibration file | gfx (a target sampled by a USER shader) | Possible (the calibration file formats are PTB's) |
| `ImageWarpingDemo.m` | An image warped by a map at the mouse | gfx (USER, `stim.tex`) | Possible |
| `LineStippleDemo.m` | Dashed and dotted lines | gfx (dashes) | Example: `gfx_gallery` page 3 |
| `LinesDemo.m` | Many moving lines | gfx (LINE, instances) | Possible |
| `MovingLineDemo.m` | A line that moves across the display, to show CRT and LCD motion artifacts | gfx, screen | Possible |
| `PanelFitterDemo.m` | A framebuffer for a display turned 90 degrees, or scaled | gfx (a target of the turned size, drawn turned) | Possible (not verified) |
| `SadowskiDemo.m` | A color adaptation image, then a gray image: a color afterimage | gfx (IMAGE), color | Possible (the adaptation image made on the CPU) |
| `SimpleImageMixingDemo.m` | Two images morphed through a Gaussian or ramp mask | gfx (targets, USER) | Possible |
| `SpriteDemo.m` | An image that follows the mouse | gfx (IMAGE), screen | Possible |
| `VignettingCorrectionDemo.m` | A gain per pixel to correct display vignetting | gfx (the scene in a target, a gain image by MULTIPLY in linear light) | Possible (not verified; no built-in gain stage) |

#### Top level: text

| Demo | What it shows | ysp headers | Status |
|---|---|---|---|
| `DrawFormattedTextDemo.m` | Wrapped and centered text, text bounds | pack/layout, gfx | Example: `gfx_layout` |
| `DrawFormattedText2Demo.m` | Alignment, line breaks, transforms, a box per word | pack/layout, gfx | Example: `gfx_layout` |
| `DrawHighQualityUnicodeTextDemo.m` | Anti-aliased Japanese text from Unicode | pack/layout, outline, gfx | Example: `gfx_layout` (seven scripts) |
| `DrawManuallyAntiAliasedTextDemo.m` | Large text drawn small and blurred, to anti-alias it | gfx (curve runs) | Example: `gfx_text`. Curve runs have box-filter coverage, so the workaround is not necessary |
| `DrawMirroredTextDemo.m` | Mirrored and upside-down text with its bounds | gfx (curve runs), outline | Possible (a negative scale on a run: not verified) |
| `DrawSomeTextDemo.m` | One text string | gfx, outline | Example: `gfx_text` |
| `FontDemo.m` | The first available font from a list | outline, pack/layout | Possible: a font is a file; there is no lookup by family name |

#### Top level: input

| Demo | What it shows | ysp headers | Status |
|---|---|---|---|
| `KbDemo.m` | KbCheck, KbWait, GetChar | screen | Example: `screen_input --hand` |
| `KbQueueDemo.m` | A key queue for a device: first and last press and release per key | screen (raw keyboard path, device ids), input, response | Example: `screen_input --devices`; `trial_two_keyboards` (a queue per keyboard, as docs/response.md "Run two keyboards or two participants") |
| `MouseMotionRecordingDemo.m` | Raw mouse movement, no pointer acceleration | screen (raw mice), input | Example: `screen_input --mice`, `trial_mouse_tracking --raw-mice` |
| `MouseTraceDemo.m` | Draw a curve with the mouse | gfx (POLYLINE), screen | Possible (224 points a path; a longer trace is several paths) |
| `MouseTraceDemo2.m` | The same; the flip does not clear the framebuffer | gfx (strokes kept in a target) | Possible |
| `MouseTraceDemo3.m` | Several people draw with several mice (Linux Multi-Pointer X) | screen (raw mice per device), gfx | Possible on Windows through raw mice (positions summed from counts). Multi-Pointer X is X11, a stub |
| `MultiTouchDemo.m`, `MultiTouchMinimalDemo.m`, `MultiTouchPinchDemo.m` (3) | Touch contacts as blobs; a two-finger pinch task | screen, input (`YIN_KIND_TOUCH`), gfx | Possible (no touchscreen tested; touch is tier 3, docs/devices_spec.md 10.1) |

#### Top level: sound

| Demo | What it shows | ysp headers | Status |
|---|---|---|---|
| `BasicSoundOutputDemo.m` | Play a sound file, repeated | audio (`yau_wav_load()`) | Example: `audio_tone` (a tone; WAV files have no example) |
| `BasicSoundScheduleDemo.m` | A playlist of preloaded buffers, added to while it plays | audio (`yau_play_at()` per buffer) | Example: `audio_schedule` |
| `SimpleSoundScheduleDemo.m` | Beeps 5, 10 and 15 s after a key press | audio, screen | Example: `audio_schedule` (the program's start is the trigger, not a key) |
| `BasicAMAndMixScheduleDemo.m` | Voices mixed, with a volume per voice and amplitude modulation | audio (voices, `yau_gain_at()`, `yau_ramp()`) | Example: `audio_schedule` for the mix and a volume change at a time. AM only as a precomputed buffer: there is no modulator voice |
| `BasicSoundChannelHoppingDemo.m` | A beep on each channel of a multichannel card in turn | audio (channel count, channel map) | Example: `audio_schedule` (left, right; more than 2 channels not run) |
| `BasicSoundPhaseShiftDemo.m` | A tone with a changing phase, mixed live from a sine and a cosine | audio (two voices, `yau_gain_at()`) | Possible |
| `BasicSoundInputDemo.m` | Record from the microphone, optionally after a voice trigger | audio | Gap: audio capture (14.7 rank 7) |
| `SimpleVoiceTriggerDemo.m` | Voice onset time from the microphone | audio | Gap: audio capture |
| `DelayedSoundFeedbackDemo.m` | Capture, then play back after a set delay | audio | Gap: audio capture (full duplex) |
| `TurnTableDemo.m` | A turntable: playback rate and direction from the mouse | audio | Out: `ysp/audio.h` never resamples (4.4) |
| `AudioTunnel3DDemo.m`, `AudioTunnel3DDemo2.m` (2) | 3D sound sources through OpenAL | None | Out: spatial audio rendering is an Audio source extension (12) |

#### Top level: movies, stereo, HDR, capture and VR

| Demo | What it shows | ysp headers | Status |
|---|---|---|---|
| `ImagingStereoMoviePlayer.m` | A side-by-side stereo movie in a stereo mode, anaglyph by default | video, gfx (IMAGE source rectangles, tint, ADD) | Possible for anaglyph, drawn by the caller. Other stereo modes: gap |
| `StereoViewer.m` | A stereo image pair as an anaglyph, offset by the mouse | gfx (two tinted IMAGEs, ADD) | Possible (anaglyph by the caller) |
| `StereoDemo.m` | Legacy stereo modes; its help marks it deprecated | None | Gap: stereo output modes (refused, `ysp/gfx.h` OUTPUT STAGE) |
| `ImagingStereoDemo.m` | Stereo modes through the imaging pipeline; crosstalk gain; DataPixx | None | Gap: stereo output modes |
| `HDRViewer.m`, `HDRDebugViewer.m`, `SimpleHDRDemo.m` (3) | HDR images on an HDR-10 display, values in nits | None | Gap: HDR output and HDR image files |
| `SimpleHDRLinuxStereoDemo.m` | HDR in stereo on Linux X11 | None | Gap: HDR output, stereo, the X11 backend |
| `HDRMinimalisticOpenGLDemo.m` | 3D rendering on an HDR display | None | Out: 3D (12) |
| `BlurredMipmapDemo.m` | Blur that grows with distance from the gaze, on live video or a movie, through a mipmap pyramid | video, gfx | Gap: camera capture; no mipmaps (blurred targets mixed by a USER shader, 3 texture slots: not verified) |
| `BlurredVideoCaptureDemo.m` | GPU convolution kernels on live video | None | Gap: camera capture; convolution beyond the blur pass |
| `VideoCaptureDemo.m` | Live camera in a window, with capture times | None | Gap: camera capture (4.6; 14.7 rank 7) |
| `VideoCaptureToMatlabDemo.m` | Camera frames into a matrix | None | Gap: camera capture |
| `VideoDelayLoopMiniDemo.m` | Delayed visual feedback from a camera | None | Gap: camera capture |
| `VideoMultiCameraCaptureDemo.m` | Several cameras, optional hardware sync, recording | None | Gap: camera capture |
| `VideoOfflineCaptureDemo.m` | Capture into memory, show it after | None | Gap: camera capture |
| `VideoRecordingDemo.m` | Capture video and sound, encode to a file | None | Gap: camera and audio capture; the encoder (4.6) |
| `VideoDVCamCaptureDemo.m` | Capture from a DV camera | None | Out: deprecated hardware (DV, FireWire) |
| `VideoIPWebcamCaptureDemo.m` | A network video stream from a phone app | None | Out: a network stream through GStreamer, not a rig camera |
| `VideoPluginCaptureDemo.m` | A marker-tracking plugin on several cameras | None | Out: its help says experimental and not for regular users |
| `VideoTextureExtractionDemo.m`, `ARToolkitDemo.m`, `ApriltagsDemo.m` (3) | Marker tracking on live video with 3D overlays | None | Out: computer vision and 3D |
| `KinectDemo.m`, `Kinect3DDemo.m` (2) | Kinect video and depth | None | Out: deprecated hardware (Kinect for Xbox 360) |
| `VRHMDDemo.m` | An HMD as a mono or stereo display in one line | None | Out: HMD VR (4.8) |

#### Top level: color, hardware and the rest

| Demo | What it shows | ysp headers | Status |
|---|---|---|---|
| `CalDemo.m` | The calibration structure: load, gamma, conversions | color (`ycol_cal`) | Example: `gfx_calib`, `color_convert` |
| `DKLDemo.m` | The DKL isoluminant plane as a picture | color | Example: `color_convert` (DKL directions, the largest contrast per azimuth; no picture) |
| `RenderDemo.m` | A patch at a CIE xyY through the calibration, the last way on the GPU | color, gfx | Possible (proposed `color_patch`, 15.5) |
| `FitGammaDemo.m` | Parametric fits to gamma readings | color | Out: offline fitting (4.8). `gfx_calib` makes the CLUT from the readings by a monotone cubic, not a parametric fit |
| `NomogramDemo.m`, `PhotopigmentNomogramDemo.m`, `IsomerizationsInDishDemo.m`, `IsomerizationsInEyeDemo.m`, `ValetonVanNorrenDemo.m` (5) | Photopigment nomograms, isomerization rates, a cone adaptation model | None | Out: colorimetric computation for analysis (4.8). `ysp/color.h` embeds the Stockman-Sharpe fundamentals only |
| `DatarecordingFromSerialPortDemo.m` | Packets from a serial device, stamped on a background reader | serial, device | Possible (a decoder in caller code; `serial_trigger` shows the echo) |
| `DatarecordingFromISCANDemo.m` | ISCAN eye-tracker samples over serial | serial | Possible as bytes. ISCAN is not in docs/devices_spec.md |
| `ReceivingTriggerFromSerialPortDemo.m` | Trigger bytes from an fMRI, TMS or EEG system | device (a byte-per-press family, `YIN_KIND_SYNC`), box | Possible |
| `PsychRTBoxDemo.m` | The USTC RTBox: buttons, its clock synchronized to the host | None | Gap: an RTBox decoder. docs/devices_spec.md reads PsychRTBox's clock sync but has no RTBox family in 10.1 |
| `RaspberryPiGPIODemo.m` | GPIO pins of a Raspberry Pi | None | Out: a host ysp does not target |
| `MinExpEntStairDemo.m` | A minimum expected entropy staircase against a model observer | quest (Psi) | Example: `quest_sim` |
| `ErrorCatchDemo.m` | try and catch to close the screen after an error | screen | Out: a MATLAB convenience. C returns codes; the panic watchdog puts the gamma ramp back (`ysp/screen.h` PANIC) |

#### ECVP2013

| Demo | What it shows | ysp headers | Status |
|---|---|---|---|
| `ECVP2013/HelloWorldDemo.m` | One text string | gfx, outline | Example: `gfx_text` |
| `ECVP2013/HelloShapesDemo.m` | Basic shapes in colors | gfx | Example: `gfx_gallery` page 2 |
| `ECVP2013/HelloAlphaDemo.m` | Alpha blending | gfx (opacity, OVER) | Example: `gfx_gallery` page 6 (premultiplied alpha) |
| `ECVP2013/HelloAnimationDemo.m` | A sinusoidal movement | gfx, timeline (a sampled track) | Possible (`timeline_tracking` makes the track without a display) |
| `ECVP2013/HelloGaborArrayDemo.m` | An array of gabors | gfx (instances) | Example: `gfx_gallery` page 6 |
| `ECVP2013/HelloSpiralTextureDemo.m` | A spiral texture | gfx (USER) | Possible |

#### GPGPUDemos

| Demo | What it shows | ysp headers | Status |
|---|---|---|---|
| `GPGPUDemos/GPUFFTDemo1.m`, `GPUFFTMoviePlaybackDemo.m`, `GPUFFTVideoCaptureDemo.m` (3) | 2-D FFT filtering in CUDA through the GPUmat toolbox, on an image, a movie and live video | None | Out: a MATLAB GPU toolbox; `ysp/gfx.h` has no compute stage (4.3) |

#### MovieDemos

| Demo | What it shows | ysp headers | Status |
|---|---|---|---|
| `MovieDemos/SimpleMovieDemo.m` | Play a movie once | video | Example: `video_play` |
| `MovieDemos/PlayMoviesDemo.m` | Movies by pattern; keys for pause, rate and seek | video, timeline (rate on a base) | Example: `video_play` (the keys are not shown) |
| `MovieDemos/PlayDualMoviesDemo.m` | Two movies at once, with sound | video, audio | Possible |
| `MovieDemos/PlayMoviesWithoutGapDemo1.m`, `PlayMoviesWithoutGapDemo2.m` (2) | Movies one after another, the next opened in the background | video | Possible (a cold open took 115 to 308 ms, so open ahead; gaps not measured) |
| `MovieDemos/LoadMovieIntoTexturesDemo.m` | Movie frames into textures for exact timing and order | video (frame sequence, value-exact), gfx | Possible |
| `MovieDemos/DetectionRTInVideoDemo.m` | RT to a time-locked event in a movie | video, timeline, response | Possible (proposed `trial_movie`, 15.5) |
| `MovieDemos/PlayInterlacedMovieDemo.m` | Deinterlacing by shader | None | Out: the canonical form is progressive; `ysp/video.h` refuses interlaced input |

#### OpenGL4MatlabDemos

| Demo | What it shows | ysp headers | Status |
|---|---|---|---|
| `OpenGL4MatlabDemos/FDFDemo.m` | A formless dot field: a turning sphere seen only from dot motion | gfx (DOTS; points projected by the caller) | Possible |
| `OpenGL4MatlabDemos/ShepardZoomDemo.m` | An endless zoom from scaled textures blended together | gfx (IMAGEs, ADD, timeline) | Possible |
| `OpenGL4MatlabDemos/FloatTextureDemo.m` | Float textures through MOGL | gfx (R32F, RGBA32F images) | Possible (`gfx_gallery` page 5 uses an R32F image) |
| `OpenGL4MatlabDemos/CylinderAnnulusOpenGLDemo.m`, `DrawDots3DDemo.m`, `GLSLDemo.m`, `MinimalisticOpenGLDemo.m`, `MorphDemo/MorphDemo.m`, `MorphDemo/MorphTextureDemo.m`, `SpinningCubeDemo.m`, `SpinningMovieCube.m`, `SuperShapeDemo.m`, `UtahTeapotDemo.m` (10) | 3D scenes by raw OpenGL from MATLAB | None | Out: 3D is a Renderer extension (12); raw GL in the frame is not supported |
| `OpenGL4MatlabDemos/VRHMDDemo1.m` | 3D stereo on an HMD with head tracking | None | Out: HMD VR (4.8) |

#### PsychExampleExperiments

| Demo | What it shows | ysp headers | Status |
|---|---|---|---|
| `PsychExampleExperiments/MinimumMotionExp.m` | Equiluminance by minimum motion (Anstis and Cavanagh), a windmill | gfx (USER), color, stair, response | Possible (close to `gfx_gamma_null`, 15.5) |
| `PsychExampleExperiments/MullerLyerIllusion.m` | A full Muller-Lyer experiment: same or different by key | gfx (POLYLINE), trials, response | Possible |
| `PsychExampleExperiments/OldNewRecognition/OldNewRecogExp.m` | A study phase and a test phase with images from lists | pack, gfx, table, trials, response | Possible (`trial_images`'s pack path; JPEG converted to PNG) |

#### PsychTutorials

| Demo | What it shows | ysp headers | Status |
|---|---|---|---|
| `PsychTutorials/AdditiveBlendingForLinearSuperpositionTutorial.m` | Two gratings summed in a float framebuffer, then sent to one of 15 output devices | gfx (float scene, ADD) | Example: `gfx_gallery` page 1 (plaid). Its output devices (Bits++ Mono++ and Color++, DataPixx M16 and C48, PseudoGray, VideoSwitcher, native 10, 11 and 16 bit): gap (16.5 rank 4); PseudoGray in caller code: `gfx_luminance_bits` |
| `PsychTutorials/AlphaImageTutorial.m` | As `AlphaImageDemo.m` | gfx, screen | Possible |
| `PsychTutorials/DriftingMaskedGratingTutorial.m` | A drifting masked grating | gfx | Example: `gfx_hello` |
| `PsychTutorials/GazeContingentTutorial.m` | As `GazeContingentDemo.m` | gfx, screen | Example: `gfx_gaze_contingent` |
| `PsychTutorials/ImageMixingTutorial.m` | Two images mixed through a mask drawn each frame | gfx (targets, MULTIPLY, USER) | Possible |
| `PsychTutorials/ImagingVideoCaptureDemo.m` | A camera through the imaging pipeline, mirrored | None | Gap: camera capture |
| `PsychTutorials/PlayDualMoviesTutorial.m` | As `PlayDualMoviesDemo.m` | video, audio | Possible |

### 16.3 PsychTests: timing and precision

Paths are from `Psychtoolbox/PsychTests/`. Only the tests that check
display, input, sound or clock timing, or output precision. The other 61
(colorimetry, fitting, text rendering bugs, unit tests) are not listed.

| Test | What it checks | ysp counterpart | Status |
|---|---|---|---|
| `VBLSyncTest.m` | Flip against the vblank under load and frame skips; IFI, onset and miss plots; optional DataPixx stamps | screen | Example: `screen_flipstats` (`--hold` for numifis, `--overrun` and `--load` for loadjitter, `--csv` for the plots) |
| `PerceptualVBLSyncTest.m`, `PerceptualVBLSyncTestFlipInfo.m`, `PerceptualVBLSyncTestFlipInfo2.m` (3) | Full-screen flicker: tearing shows a sync fault; two displays in step | screen | Example: `screen_sync_check` (flicker and a moving bar, `--no-flicker`; per second the flip records and the sync guard's evidence; the verdict). One display; two in step is `screen_flipstats --group` |
| `AsyncFlipTest.m` | Async flips under load | screen | Example: `screen_flipstats`. `yscr_flip_at()` never blocks; every flip is async |
| `MultiWindowLockStepTest.m` | Parallel flips on several windows at different rates | screen (groups) | Example: `screen_flipstats --group` (one rate) |
| `CheckFrameTiming.m` | Missed frames under a noise load | screen, gfx | Example: `gfx_load` |
| `LoadGenerator.m` | A CPU load to run beside a test | screen | Example: `screen_flipstats --load N` |
| `OSSchedulingAccuracyTest.m` | OS wake-up accuracy for script code | rt | Example: `rt_jitter` |
| `FlipTimingWithRTBoxPhotoDiodeTest.m` | Onset stamps against a photodiode on an RTBox, DataPixx or similar | screen, device, net | Example: `photodiode_check`, `net_labstreamer_flip` (both never run with the device) |
| `HighColorPrecisionDrawingTest.m` | Drawing precision read back against a double reference | gfx | Example (a test): `tests/adapt/gfx_test.c` |
| `BeampositionTest.m` | Scanout position queries | None | Out: ysp takes onsets from DXGI and DComp statistics; `D3DKMTGetScanLine` was polled once to check what they are (`ysp/screen.h` STATUS) |
| `OMLBasicTest.m` | OML_sync_control flip stamps against the vblank and the clock (Linux) | screen (GLX, a stub) | Gap: the X11 backend. `tests/probe/screen_x11/` measures the same stamps |
| `GraphicsDisplaySyncAcrossDualHeadsTest.m`, `GraphicsDisplaySyncAcrossDualHeadsTestLinux.m` (2) | The scanout phase of two display heads | screen | Gap: two physical displays not run; no scanout query |
| `FrameSequentialStereoTest.m` | Frame-sequential stereo order and onset | None | Gap: stereo output modes |
| `VRRTest.m`, `VRRFixedRateSwitchingTest.m` (2) | Variable refresh: flips at varying intervals; fast rate switching | screen | Gap: variable refresh (refused until the two sweeps, 4.2) |
| `GetSecsTest.m` | The Windows timer against another clock | rt | Possible: `yrt_correlate()` against QueryInterruptTimePrecise (docs/rt.md B6); no example |
| `KeyboardLatencyTest.m` | Key and mouse latency against a microphone that hears the key | audio, screen | Gap: audio capture. `screen_input` measures injected keys only |
| `HIDIntervalTest.m` | The sampling interval of keyboards and mice | screen, input | Possible (`screen_input --hand` stamps keys pressed by hand) |
| `PsychPortAudioTimingTest.m` | Sound onset against a flip, by photodiode, microphone and oscilloscope | audio, screen | Example: `audio_av_sync` (the records; its board mode never run); `tests/loopback/audio_loopback.c` (never run with a cable) |
| `PsychPortAudioDataPixxTimingTest.m` | The same, stamped by a DataPixx | None | Gap: `ysp/dpx.h` |
| `AudioFeedbackLatencyTest.m` | Sound onset by capture of its own output | audio | Gap: audio capture |
| `VideoCaptureLatencyTest.m` | Camera to display latency | None | Gap: camera capture |
| `HighPrecisionLuminanceOutputDriversImagingPipelineTest.m` | Each output formatter against a reference | None | Gap: high-precision output formatters. `gfx_luminance_bits` checks 8-bit, both dithers and PseudoGray code by code |
| `DatapixxGPUDitherpatternTest.m` | GPU dithering, by DataPixx scanline readback | None | Gap: `ysp/dpx.h`. The header reads codes back through D3D (1800 of 1800 equal), not through a device |
| `SyncedCLUTUpdateTest.m` | A CLUT change on the same flip as the image (a perceptual cancel test) | gfx | Example: `gfx_clut_sync` (gfx CLUT read back; the OS ramp's flip needs a photodiode) |
| `DriftTexturePrecisionTest.m` | The smallest subpixel texture step the GPU resolves | gfx | Possible |
| `FloatTexturePrecisionTest.m` | Float texture precision | gfx | Possible |
| `Color3DLUTTest.m` | 3D CLUT color correction and its precision | None | Gap: a 3D color LUT (the output stage has a CLUT per channel) |
| `HDRTest.m` | HDR-10 luminance and primaries by colorimeter | None | Gap: HDR output |
| `CedrusResponseBoxTest.m` | A Cedrus box | device (XID), box | Possible (no box on hand) |
| `OSXCompositorIdiocyTest.m` | macOS compositor interference with flips | None | Out: macOS swap path (a stub) |
| `MultiWindowVulkanTest.m` | Several fullscreen windows under Linux Vulkan | None | Out: a Vulkan display backend |

### 16.4 What PTB is known for

"Match", "Exceeds" and "Lacks" compare what each side ships today on
the platforms it runs. A number on the ysp side is from the header's
STATUS block or doc, on the one Windows 11 laptop (Iris Xe, 60 Hz panel)
unless stated. No ysp number is a measurement of light or sound at the
display or the jack: no photodiode or line-in run has been made
(11, item 1). PTB numbers were measured on one machine only: the Linux
desktop, 2026-10-12, PTB 3.0.19.14 under Octave against ysp/screen.h
v0.8.0, both against the kernel's vblank and evdev times
(`tests/compare/ptb_linux/README.md`).

| Topic | PTB | ysp today | Verdict |
|---|---|---|---|
| `Screen('Flip')` timing and its tests | Flip blocks to the vblank and returns the vblank time, the onset estimate and a miss flag. Stamps from beamposition queries, from OML_sync_control, and from the kernel's vblank and flip events on Linux; VBLSyncTest, PerceptualVBLSyncTest, BeampositionTest, OMLBasicTest; FlipTimingWithRTBoxPhotoDiodeTest against a photodiode. Linux, Windows, macOS | A record per flip: planned and shown vblank, residual, drops, path, tier, phases (`ysp/screen.h`). DXGI_FLIP fullscreen: onset within 2.3 us of the prediction at p99 over 7200 frames, no drop; COMPOSITION independent flip: DisplayedTime 13 us p50, 77 us p99 after the vblank entry polled with D3DKMTGetScanLine. `screen_flipstats` is the VBLSyncTest analog. Linux X11 (GLX_OML, v0.8.0): beside PTB on one machine, 9600 flips (next vblank and held 1 to 5 frames, idle and with 20 spinning processes), both on the vblank asked for every time, both stamps the kernel's vblank time (PTB within 2 us, ysp exactly). Open: PTB 1.5 s with its sync tests, ysp 0.55 s for the whole process with its settle check. Wayland and macOS are stubs | Match on Linux X11 timing; exceeds in what each flip records (path, tier, the phase that was late, with no tool attached). Lacks macOS and Wayland, a photodiode run, and any rate but 60 Hz |
| `when` scheduling | `Flip(win, when)`: the first vblank at or after `when`; the manual's idiom is `vbl + (n - 0.5) * ifi`. Async flips are separate calls | `yscr_flip_at(t)` never blocks. It snaps to the nearest vblank (lead 0.5), or never early (`YSCR_LEAD_NONE`); a late caller gets `LATE_TARGET`. Holds 0 to 4 vblanks ahead: 1800 of 1800 fullscreen and 2979 of 2998 windowed on the vblank asked for, 19 late, 0 early; overruns of 17 and 20 ms: 84 of 84 flagged | Match. The half-period lead is PTB's idiom made the default |
| High-precision luminance | PseudoGray (bit-stealing); native 10, 11 and 16 bit framebuffers; Bits++, Bits# Mono++ and Color++; DataPixx M16 and C48; VideoSwitcher and attenuators; checked by HighPrecisionLuminanceOutputDriversImagingPipelineTest, BitsPlusIdentityClutTest | A float scene (RGBA16F, 11-bit mantissa), a CLUT of 2 to 4096 entries per channel, then rounding, an 8 x 8 ordered dither or a noise dither to 8 bits. All 256 codes, and 10-bit codes written as k/1023, read back exact; an `R10G10B10A2` swapchain and pbuffer work on D3D11 (docs/gfx.md). `YGFX_OUT_10`, `MONO_PP` and `COLOR_PP` are refused: the panel link is 8 bits and no device was there to verify | Lacks a path past 8 bits on the link. In caller code (`gfx_luminance_bits`, 2026-10-09, Iris Xe, nominal calibration, not photometered): PseudoGray reaches 1696 levels (10.7 bits) with uneven sub-steps of 0.13 to 0.78 % and a chromaticity shift up to 0.017 in x, y; a 0.40 % grating shows as 0.63 % undithered, 0.38 % ordered, 0.40 % noise, 0.455 % PseudoGray; 3,532,030 read-back codes matched |
| Color and gamma calibration | PsychCal, PsychColorimetric, DKL, `LoadNormalizedGammaTable`; photometer drivers (MeasXYZ meters) | `ysp/color.h`: calibration from readings (monotone cubic CLUT), XYZ, LMS, DKL, cone contrast, gamut distance never clipped. Against PTB in MATLAB R2023a: `ComputeDKL_M` within 6.2e-16, `SensorToPrimary` within 1.5e-8, `MaximizeGamutContrast` within 7.3e-16, out-of-gamut flags equal on 500 colors. The CLUT is in the frame's shader; the OS ramp is set to identity | Match for the math (measured agreement). Lacks photometer drivers: `gfx_calib` takes readings typed in |
| Stereo | Frame-sequential, dual-display, anaglyph, interleaved, side-by-side and VPixx modes; crosstalk gains | Refused in the output stage. Anaglyph and side by side are possible in caller code; two displays through groups are not synchronized and not run | Lacks |
| HDR | PsychHDR: HDR-10 output in nits, HDR images and movies; HDRTest by colorimeter | None. The header detects HDR or auto color management (`ADVANCED_COLOR`) and flags display codes as at risk | Lacks |
| Imaging pipeline, procedural shaders | PsychImaging: float framebuffers, procedural gabors, gratings and noise, GLSL operators, convolution, geometry correction, panel fitter, gain correction | Linear-light float scene; GRATING, GABOR, NOISE, DOTS, instances (16384 a frame), targets, the blur pass, the USER shader contract. Gabor pixels within 1.2e-6 of a double CPU reference (Iris Xe); 10000 instanced gabors 1.26 to 1.33 ms of GPU and 177 to 191 us of CPU; 1000 gabors of 256 x 256 9.3 to 11 ms of GPU | Match for procedural stimuli; exceeds in measured pixel accuracy on four renderers. Lacks GPU filtered noise, convolution operators, built-in geometry and gain correction |
| Movies | GStreamer: most formats, rate and direction, gapless playback, HDR movies, frame stamps; movie sound goes through GStreamer, outside PsychPortAudio (docs/audio.md) | Canonical form through Media Foundation (DXVA), pl_mpeg or the frame sequence; a record per frame decision; the soundtrack on `ysp/audio.h`'s clock: each start within 14 us of its target on the fit, flips within -0.42 to +0.08 ms (p1 to p99) of each frame's time | Exceeds in records and in sound and picture on one clock (measured in a composed window). Lacks formats outside the canonical form, Linux and macOS decode, reverse play |
| Audio | PsychPortAudio: low-latency classes (WASAPI exclusive, ASIO, Core Audio, ALSA), scheduled starts, schedules, slave voices, AM modulators, capture and full duplex; its help says sub-ms onsets and below 10 ms latency are possible; PsychPortAudioTimingTest by oscilloscope | `yau_play_at()` with onsets from a fit of device positions, tier 2: placement 67.3 us p99 from the median idle through WASAPI's loopback tap (bar 20.8 us, missed), 19.8 us under load. Shared mode only (10 ms period); the shortest lead with no late onset was 43.5 to 54.5 ms; exclusive mode delivered callbacks in bursts. No capture | Lacks low latency, capture and modulators. Scheduling matches in kind; its accuracy at the jack is not measured |
| Keyboard | KbCheck, KbQueue per device with press and release times; KeyboardLatencyTest by microphone; HIDIntervalTest | SDL's raw keyboard path on Windows, a device id per keyboard, double reports dropped. Injected keys stamped 0.18 to 0.67 ms after `SendInput()` (the message path: 6.2 ms before to 12.1 ms after). Raw mice per device. Linux X11 (v0.8.0): the kernel's evdev time when the keyboard's node is readable (uinput keys: 0 error), else the X server's (1 to 2 ms early). PTB's KbQueue on the same uinput keys: 0.03 to 0.40 ms after the kernel's time idle, up to 2.0 ms under load | Match on Windows; exceeds on Linux X11 with the udev rule (the kernel's time, independent of load), lacks without it (1 to 2 ms early against KbQueue's 0.1 ms late). The physical key latency is not measured (no capture) |
| `GetSecs` clock | QPC on Windows; GetSecsTest checks it against another timer | `yrt_now_ns()` on QPC (10 MHz here, 100 ns steps); SDL's clock correlated within 0.1 us; device clocks fitted | Match |
| IOPort serial | A reader thread per port, one stamp per read; device protocols in M-files (PsychRTBox clock sync, CedrusResponseBox) | `ysp/serial.h`; `ysp/device.h` reader threads with clock fits (FIT, BRACKET); `ysp/box.h` decoders (XID, the line protocol, photodiode frame, trigger boxes). On simulated devices: a wrapping us counter mapped within -12 to +46 us | Exceeds in design (device clocks fitted, not stamped on read). Lacks a run on any real device and an RTBox decoder |
| Datapixx and VPixx | PsychDataPixx: clock sync, M16 and C48 output, scanline readback, audio, Pixel Mode triggers | `ysp/screen.h` draws Pixel Mode codes and PTB's 8-pixel pixel sync (1800 read-backs through D3D, 0 differences); `ysp/dpx.h` is planned (docs/devices_spec.md) | Lacks |
| Eye tracking | EyelinkToolbox (10 SR Research demos), calibration on the PTB screen, gaze-contingent displays | None; `ysp/eyelink.h` and `ysp/tobii.h` planned; the input bridge takes their events (`yscr_push_input()`) | Lacks (15.4 rank 2) |
| Multi-display | Several onscreen windows; dual-display stereo; MultiWindowLockStepTest; scanout phase test | One `yscr_screen` per display, `yscr_begin_group()`, `yscr_flip_group_at()`; untimed screens proposed (4.2) | Lacks verification: two physical displays not run |
| Variable refresh | VRRTest, fine-grained flip intervals on supported Linux systems | `desc.vrr` refused until the two sweeps (4.2) | Lacks |
| OpenGL access (MOGL) | All of OpenGL from MATLAB; 3D demos | A GLSL ES fragment body (USER shader contract); raw GL in the frame is not supported; 3D is a Renderer extension (12) | Lacks by design |

### 16.5 Gaps ranked by the demos they block

"Blocks" counts the demos (16.2) and tests (16.3) with status Gap for
that piece. A demo with two missing pieces counts for each. The 14.7 and
15.4 columns give the rank there where the gap is the same.

| Rank | Gap | 14.7 | 15.4 | Where it goes | Blocks | Size |
|---|---|---|---|---|---|---|
| 1 | Capture: camera and microphone | 7 | 3 | `ysp/video.h` capture (4.6); `ysp/audio.h` input | 12 demos: 9 need a camera (`VideoRecordingDemo.m` also the microphone), 3 the microphone only and 3 tests (`KeyboardLatencyTest.m`, `AudioFeedbackLatencyTest.m`, `VideoCaptureLatencyTest.m`) | Medium |
| 2 | HDR output and HDR image files | None | None (new) | `ysp/screen.h` (an HDR swap chain), `ysp/gfx.h` output stage | 4 demos, `HDRTest.m` | Medium; needs a colorimeter and an HDR panel |
| 3 | Stereo output modes: frame-sequential, side by side, interleaved, dual display | None | None (new) | `ysp/gfx.h` output stage (4.3) | 3 demos (`StereoDemo.m`, `ImagingStereoDemo.m`, `SimpleHDRLinuxStereoDemo.m`), `FrameSequentialStereoTest.m`; outside PsychDemos, 4 BitsPlusDemos | Small for anaglyph and side by side; frame-sequential needs a 120 Hz panel and glasses |
| 4 | High-precision luminance output: native 10 bit, PseudoGray, Bits# Mono++ and Color++, VPixx M16 and C48 | None | 5 | `ysp/gfx.h` output stage; `ysp/screen.h` 10-bit back buffer | 0 demos outright (the output half of `AdditiveBlendingForLinearSuperpositionTutorial.m`), 1 test; outside PsychDemos, all 8 BitsPlusDemos files. Ranked by count; by reputation it is PTB's first feature, and 16.6 puts it second | Small in code; needs a device, a 10-bit panel or a photometer to verify |
| 5 | Linux X11 backend with OML and kernel stamps | None | None | `ysp/screen.h` GLX (11, item 5) | `OMLBasicTest.m`, `GraphicsDisplaySyncAcrossDualHeadsTestLinux.m`, `SimpleHDRLinuxStereoDemo.m` | Large; the probe exists |
| 6 | Eye-tracker input source and calibration | 6 | 2 | `ysp/eyelink.h`, `ysp/tobii.h` | 0 in PsychDemos (the mouse stands in for gaze); the 10 SR Research demos outside | Medium per tracker |
| 7 | GPU filters: filtered noise, convolution, mipmaps | None | 8 | `ysp/gfx.h` (fragment passes on targets) | `FastFilteredNoiseDemo.m`, `BlurredVideoCaptureDemo.m`, `BlurredMipmapDemo.m` (the last two also need capture) | Small to medium |
| 8 | Two physical displays: scanout phase, groups verified | None | 9 (untimed screens) | `ysp/screen.h` | 2 tests | A second display and two photodiodes |
| 9 | Variable refresh | None | None | `ysp/screen.h` (4.2) | `VRRTest.m`, `VRRFixedRateSwitchingTest.m` | Medium; a VRR panel and two sweeps |
| 10 | VPixx glue | None | None | `ysp/dpx.h` (docs/devices_spec.md) | 2 tests; the DataPixx option of `ImagingStereoDemo.m` and `VBLSyncTest.m` | Small; a device |
| 11 | RTBox decoder | None | None (15.4 rank 10 is the same kind: a decoder) | `ysp/box.h` | `PsychRTBoxDemo.m`; the RTBox path of `FlipTimingWithRTBoxPhotoDiodeTest.m` | Small |
| 12 | 3D color LUT | None | None | `ysp/gfx.h` output stage | `Color3DLUTTest.m` | Small |

Notes on the merge with 14.7 and 15.4:
- Capture is the top gap in all three. PTB's demos weight the camera
  most: 9 of the 12 capture demos are camera demos.
- 14.7 rank 1 (experiment flow) and 15.4 rank 1 do not show here: PTB
  ships no experiment layer, so its demos need none.
- 15.4 rank 4 (network markers) is closed for LSL by `ysp/net.h`
  v0.1.0; no PTB demo uses LSL or NetStation.
- 15.4 rank 5 (high bit depth) is ranked low by count in both lists
  because PsychoPy and PTB put those demos in hardware folders. It is the
  gap a PTB user notices first. A 10-bit panel and a photometer would
  verify `YGFX_OUT_10` without any vendor device.
- New against 14.7 and 15.4: HDR, stereo, the Linux backend, variable
  refresh, VPixx, RTBox and the 3D LUT. The Linux backend and variable
  refresh were in the plan (11, item 5; 4.2); PTB's tests make them
  concrete.
- PTB demos that need the widget layer (14.7 rank 4, 15.4 rank 6): none.

### 16.6 Proposed C examples

None of these repeats an example in `examples/`. `screen_flipstats`
already does what VBLSyncTest does, `gfx_load` and `gfx_bench` what the
Garborium and procedural gabor benchmarks do, `screen_input` what KbDemo
and KbQueueDemo do, and 15.5's `trial_movie` (not built) covers
`DetectionRTInVideoDemo.m` and `LoadMovieIntoTexturesDemo.m`, so none of
those is proposed again. Timing and luminance precision come first,
because PTB is the reference there. Sizes are estimates from the
existing examples.

| Order | Program (file) | Mirrors | Headers | What it shows that no example shows | Lines | Hardware |
|---|---|---|---|---|---|---|
| 1 | `screen_sync_check` (`examples/screen/`) | `PerceptualVBLSyncTest.m`, `OSXCompositorIdiocyTest.m`'s flicker pattern | screen | Full-screen black and white flicker with stripes, where a torn or doubled frame shows; beside it, per second, presents completed per refresh and flips off the grid, which is the guard of 13 for a driver that forces vsync off. Runs with a driver panel set to vsync off and on | 200 | None (the eyes); a photodiode board optional |
| 2 | `gfx_luminance_bits` (`examples/gfx/`) | `AdditiveBlendingForLinearSuperpositionTutorial.m` (PseudoGray, 10-bit), `HighPrecisionLuminanceOutputDriversImagingPipelineTest.m`, `BitsPlusCSFDemo.m` | gfx, color, screen | A near-threshold grating and a shallow ramp through four paths: 8-bit rounding, the ordered dither, the noise dither (one gfx each), and PseudoGray in a USER shader on an identity CLUT, with the luminance step of each path from the calibration. `gfx_gallery` page 8 shows the three roundings, not their luminance or PseudoGray | 350 | A photometer's calibration for any claim; a nominal one otherwise (flagged) |
| 3 | `audio_av_sync` (`examples/audio/`) | `PsychPortAudioTimingTest.m`, `AudioFeedbackLatencyTest.m` (without capture) | screen, gfx, audio, device, timeline | A tone at each black-to-white flip, planned for the flip's predicted onset; the line board reads the photodiode and the sound on its own clock, so sound minus light comes from one device with no oscilloscope. Without the board: planned minus fit times only | 350 | The `firmware/ysp_line/` board with a photodiode and an audio input; a sound device |
| 4 | `audio_schedule` (`examples/audio/`) | `SimpleSoundScheduleDemo.m`, `BasicSoundScheduleDemo.m`, `BasicAMAndMixScheduleDemo.m`, `BasicSoundChannelHoppingDemo.m` | audio | Preloaded buffers on a schedule at a trigger plus 5, 10 and 15 s; gain changes at times (`yau_gain_at()`); a WAV stream with a fade (`yau_stop_at()`); a beep on each channel in turn; each onset record with its tier. `--null` for CI | 250 | A sound device (none with `--null`) |
| 5 | `gfx_clut_sync` (`examples/gfx/`) | `ClutAnimDemo.m`, `SyncedCLUTUpdateTest.m` | gfx, screen | CLUT animation by `ygfx_set_lut()` each frame, and the cancel test: a half-range ramp under a full CLUT, then a full ramp under a half CLUT, alternating; a steady image means the CLUT and the image changed on the same flip; a read-back of the codes checks it without eyes | 200 | None |
| 6 | `trial_two_keyboards` (`examples/response/`) | `KbQueueDemo.m` (`deviceIndex`), `MouseTraceDemo3.m` | screen, input, response, trials, gfx | Two keyboards as two roles: two participants race to a target, the first press per role, ties resolved by the stamps, presses from a third keyboard ignored. docs/response.md describes it; no program runs it | 300 | Two USB keyboards (one keyboard plays both roles without them) |
| 7 | `gfx_filtered_noise` (`examples/gfx/`) | `FastFilteredNoiseDemo.m`; PsychoPy's `Feature Demos/noise/` | gfx, rt (pump) | Band-pass and 1/f noise filtered on the CPU on the pump into an IMAGE each frame; the filter's cost against the 16 ms frame, and the GPU time from the flip phases. It measures whether 15.4 rank 8 needs a GPU pass | 300 | None |
| 8 | `gfx_gaze_contingent` (`examples/gfx/`) | `GazeContingentDemo.m`, `BubbleDemo.m`, `ImageMixingTutorial.m` | gfx, screen, input | An image sharp in a Gaussian window at the gaze and blurred outside; the mouse stands in for the gaze. Per frame: the newest input stamp, the flip onset, and their difference, the software part of a gaze-contingent latency. An eye-tracker source replaces the mouse when `ysp/eyelink.h` exists | 300 | None |

Examples 1 to 3 need hardware for their claims and run without it.
Example 2 is the base for `YGFX_OUT_10` once a 10-bit panel is on hand.

### 16.7 Not verified

- PTB demos past their first comment block, except the bodies named in
  the sources. EyelinkDemos and BitsPlusDemos: file names only.
- PTB's timestamping methods (beamposition, OML, the kernel's flip
  events) and its claims (sub-ms sound onsets, below 10 ms latency) are
  from help text, not from source or a run. No PTB number was measured
  here.
- PTB's `Flip` semantics in 16.4 (`when`, the half-IFI idiom, the
  return values) are from PTB's documentation, not from the files read.
- Whether PTB's KbQueue separates keyboards on Windows.
- Which platforms' mex files need a paid license
  (`PsychLicenseHandling.m` was read only in its first 60 lines).
- On the ysp side, not built and not run: anaglyph by tinted IMAGEs with
  source rectangles; a negative scale for mirrored text; a turned target
  for the panel fitter; a gain image by MULTIPLY for vignetting
  (PseudoGray: built 2026-10-09, `gfx_luminance_bits`); touch input through `yscr_poll()` from a
  touchscreen; more than 2 audio channels; gapless movie changes.
- That the Kinect for Xbox 360 counts as deprecated hardware (its
  production ended; PTB's support status was not read).
