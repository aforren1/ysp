# filly as a Renderer extension: probe, 2026-11-18

This note records a time-boxed probe. It asked whether filly
(github.com/aforren1/filly, an MIT renderer for glTF models on Google
Filament) can be a "Renderer" extension of ysp as
[rig_spec.md](rig_spec.md) section 12 defines it: the renderer draws into a
texture that the host owns, the host composites and presents, and the
renderer's frame time is a phase in the flip record. The extension loader
was not built. The probe built the smallest programs that answer six
questions (shared context, cost in a frame loop, GL state, the web, the
interface, licenses and sizes), measured them on the Linux desktop, and
records the results here.

The source is in `tests/probe/filly/`: `share_test.c` (question 1 at the
driver level, no Filament), `filly_ysp.cpp` and `ysp_impl.c` (questions 1
to 3 with filly and a ysp screen), `gles_share.cpp` (route (b), Filament
without filly), `build.sh` and `run.sh`, and `web/` (question 4: the C
program, the page, a server and a browser driver). filly, Filament and
their build trees are not in the repo; they were fetched and built in a
scratch folder (see "How to rebuild").

## Recommendation

1. **Route (a), a shared context, works on Mesa with no copy, and it is the
   route to use on Linux.** Filament's OpenGL 4.1 core GLX context shares
   objects with ysp's GL ES 3.0 GLX context. filly renders into a texture
   that ysp made, and `ygfx_texture_import()` wraps that texture. Mesa
   refused nothing: GLX made a 4.1 core context that shares an ES 3.0 context in
   both orders, and EGL in the one tried (section 1.1). In the ysp frame loop
   (GLX_OML, a 1024 x 768 window, 3,600 frames for each run), filly's CPU
   time on the frame thread was 0.100 / 0.156 / 0.317 ms (p50 / p99 / max)
   at 512 x 512 and 0.131 / 0.176 / 0.264 ms at 1920 x 1080. Filament's
   GPU frame was 0.074 ms and 0.334 ms (p50). No flip record was dropped
   or late, and the frame-interval p99 was 16.668 ms, the same as without
   filly (section 2).
2. **The synchronization of route (a) is GPU-side and needs no glFinish.**
   Filament's driver thread inserts a GL fence after each render.
   `ImportedTarget::acquire()` waits on the CPU only until that fence
   exists, then queues `glWaitSync()` in ysp's context. `release()`, called
   after `ygfx_end()`, puts a fence in ysp's context that Filament waits on
   before it writes the next frame. The CPU wait in `acquire()` was 0.062 /
   0.116 / 0.194 ms at 512 x 512.
3. **The CPU readback route (d) works everywhere but costs much more.**
   `RenderTarget::read()` and `ygfx_texture_update()` took 1.25 / 1.41 /
   2.65 ms a frame at 512 x 512, 2.62 / 2.87 / 3.09 ms at 1024 x 1024 and
   4.94 / 5.21 / 8.05 ms at 1920 x 1080. Pipelined by one frame (read frame
   N - 1 after submitting frame N), they took 0.27 / 0.45 / 0.98 ms, 0.53 /
   1.09 / 3.34 ms and 1.61 / 2.25 / 5.84 ms, with one frame of latency. At
   1920 x 1080 the upload alone was 0.88 ms p50. Use it only where no
   shared route exists (section 1.4). It does not fit the 2 ms frame of the
   500 Hz target at 1920 x 1080.
4. **Route (b), Filament's GL ES backend on a context shared with ysp's,
   works with Filament, not with filly as it is.** Filament 1.80.1 built
   with `FILAMENT_SUPPORTS_EGL_ON_LINUX` and `FILAMENT_LINUX_IS_MOBILE`
   (and a two-line patch, section 1.2) made an ES context that shares
   ysp's EGL ES 3.0 context, and rendered filly's sample horse through
   gltfio into a texture that ysp drew. filly cannot use this build: its
   interop code is desktop GL only (WGL, GLX, CGL, EGL with GL 4.1) and its
   material archive is compiled for desktop GLSL. This is the route that
   Windows needs, because ysp's context there is ANGLE (GL ES only).
5. **Route (c), dma-buf and EGLImage, works at the driver level.** A
   texture of a 4.1 core context reached an ES 3.0 context that shares
   nothing with it, through an EGLImage on one EGLDisplay and through an
   exported and imported dma-buf (`AB24`, one plane, a DCC modifier),
   ordered by an EGL fence and `eglWaitSyncKHR()`. Both read back exactly.
   It was not wired to filly: filly has no call that gives its own target's
   GL name and context (section 1.3).
6. **ysp's GL state was not broken by filly, natively or on the web.** In
   12 poses for each case, the output outside the model was equal, byte
   for byte, to the output before filly existed, and the model's pixels
   were equal to a CPU copy of the same frame (max difference 0): natively
   on the shared route, and in Chromium and Firefox on one WebGL2 context,
   with and without `ygfx_reset_state()` after filly (section 3). Keep the
   reset in the contract: it cost nothing measurable (gfx CPU time 0.015
   against 0.020 ms p50 in Chromium), and the web result depends on what
   filly's hand-back restores.
7. **The web works on one WebGL2 context, with two Emscripten modules.**
   ysp's module gives its `WebGL2RenderingContext` and its texture object
   to filly's `createRenderer()` and `importTarget()` (four `EM_JS`
   functions). filly's render and hand-back took 0.135 / 0.340 / 0.750 ms
   in Chromium 159 and 0.240 / 0.540 / 1.820 ms in Firefox 128 ESR at
   512 x 512. No flip record was dropped in 3,600 frames in either
   (section 4). Firefox took 7.6 to 8.3 s to compile filly's 3.66 MB module
   (Chromium 26 ms); load it before the session, not between trials.
8. **filly writes rows bottom first, and ysp has no way to say so.** The
   imported texture shows upside down, because `ygfx_texture_import()`
   samples texel row 0 as the top and filly writes GL's row order. The
   Renderer contract needs a row-order flag, or `ygfx_import_desc` needs
   one (section 5).
9. **Filament starts 22 threads; create the renderer before the frame
   thread is elevated.** In this process: 18 `JobSystem::loop` threads (20
   logical CPUs), `FEngine::loop` (the driver thread), `ServiceThread`,
   `CompilerThreadPool` and `FrameInfoGpuCom`, plus Mesa's threads for
   Filament's context. All were SCHED_OTHER. Filament's thread priority
   call does nothing on Linux, and a new thread inherits its creator's
   policy, so a renderer made after `yrt_thread_elevate()` gives SCHED_FIFO
   to all of them (CLAUDE.md: inherited spin threads froze the rig). The
   driver thread used 0.04 to 0.06 ms of CPU a frame on route (a). filly
   does not expose Filament's `jobSystemThreadCount`.
10. **Licenses and size do not block it.** filly is MIT; Filament and the
    parts it links are Apache 2.0, BSD, MIT, zlib and public domain
    (section 6). filly and Filament add 5.6 MB to a stripped program
    (6.13 MB, against 0.52 MB for `gfx_hello`). The web module is 3.66 MB
    raw and 1.14 MB with brotli.

## Conditions

- Linux desktop: Intel Core i5-14500 (20 logical CPUs), AMD Radeon RX 6600,
  Mesa 22.3.6 (radeonsi, LLVM 15.0.6), Debian 12, kernel
  6.11.5-1-liquorix-amd64, X.Org 1.21.1.7, XFCE with Xfwm4's compositor
  off, one output (DisplayPort-4) at 1920 x 1080 and 60 Hz.
- Background: `radeontop` (root) used about 7 % of one core throughout. No
  other measuring job ran. Load averages are in the results: 0.00 to 0.35
  (1 minute) at the start of each native run.
- ysp at commit 7d55dcc: `ysp/screen.h` 0.19.0 (GLX_OML and the web
  backend), `ysp/gfx.h` 0.20.1. SDL 3.4.18 (`/home/bwadmin/sdl3-3.4.18`).
  The ysp headers compiled as C11 with GCC 12.2.0, `-O2`.
- filly v0.5.0 (commit 023e681480cac9c6c303866077d8cc6e05edd057, the tag).
  Filament 1.80.1, the version filly pins (`FILAMENT_VERSION`). Source
  archive sha256 `89525d8e74a5bc4884ad8879ba5a740196c614b9115a41c4e8d3cb692d7cff83`;
  release archive for the headers `c2f46496ba5707605f0459d8714d34c73296fbfd7fcfdbe4c1a00b76607da193`.
- Filament's prebuilt Linux release could not be used: filly's Linux build
  builds Filament from source (`tools/build_filament_linux.py`), because
  the release libraries are built for libc++ and the release tools need
  glibc 2.38 (Debian 12 has 2.36). The SDK and filly's core were built
  with filly's tool and its patches, with emsdk 7.0.0's Clang
  (24.0.0git, llvm-project 84ba7b37) used as a native x86-64 compiler with
  libstdc++ 12, because the machine has no Clang. A wrapper adds
  `-Wno-deprecated-declarations`: libstdc++ 12's `get_temporary_buffer`
  is deprecated for Clang 24, and Filament builds with `-Werror`.
- The native probe ran windowed, 1024 x 768, managed by the window
  manager. Every present was a copy (`path=composed`, tier 2). The frame
  thread ran at normal priority; no thread was elevated.
- Web: ysp's module with emscripten 7.0.0
  (777e6e5db7b4990bcaeb1b9998ed642cc84e6629, the emsdk of the session;
  `examples/web/build.sh` pins 6.0.12), SDL3 port, `-O2`, ASYNCIFY,
  WebGL2, no threads. filly's module from the release
  `filly-web-0.5.0.zip` (sha256
  `0aaba93e8ba7925c7f9dd7fddba2f9134d8e8aa614965a1bd2761c664d112ce8`),
  which filly builds with emsdk 5.0.4. Browsers: Chromium 159.0.8154.6
  from Playwright 1.65.2 (`chromium-1255`), headed on the X display, a new
  browser for each run; system Firefox 128.3.1esr, a new profile for each
  run. The server sent COOP and COEP, and every page reported
  `crossOriginIsolated` true. Chromium's WebGL renderer was "ANGLE (AMD,
  AMD Radeon RX 6600 (navi23 LLVM 15.0.6), OpenGL 4.6)"; Firefox reported
  "Radeon R9 200 Series, or similar" (its masked string).

## Method

- **Model and pose.** filly's sample `samples/horse.glb` (Quaternius,
  CC0; one mesh with 15 morph targets and a 1.5 s gallop), framed with
  `Camera::frame()` (fill 0.8, sphere), one directional light at 100,000
  lux, background (0.2, 0.2, 0.25). Each frame turns it about +Y at 90
  degrees a second and applies the gallop at the time of the frame's
  predicted onset (`yscr_frame.onset`). Two runs used the README's model
  (`examples/assets/wooden_playground_horse.glb`, textured PBR, 14 nodes,
  CC BY 4.0), which fills more of the target. The sample horse covers a
  small part of its target (figure: about 100 x 100 px of 512 x 512);
  its bounds include the morph targets' extremes.
- **The ysp frame.** `yscr_begin()`, then filly's work, `yscr_mark(SCRIPT)`,
  then `ygfx_begin()`, a gabor, a red rectangle and the model as an
  `ygfx_image()` (shown at 512 px wide whatever the target size),
  `ygfx_end()`, then `release()` on route (a), then `yscr_flip()`. The
  texture is imported with an sRGB encoding (`YGFX_TRC_SRGB`,
  `YGFX_PRIM_DEVICE`), because filly writes sRGB-encoded RGBA8.
- **filly's time** is `CLOCK_MONOTONIC` around filly's calls on the frame
  thread: `render()` and `acquire()` (route a), `render()`, `read()` and
  `ygfx_texture_update()` (route d). It is charged to
  `YSCR_PHASE_SCRIPT` in the flip record, because section 12's extension
  phase does not exist yet. The GPU time is filly's `Stats.gpu_frame_ms`,
  Filament's timer query, which reports a frame two or more frames late.
- **Flip records.** Dropped (`dropped > 0`), late (`YSCR_FLIP_LATE_TARGET`)
  and skipped records, and the interval between the onsets of consecutive
  records. The first 10 frames are left out.
- **Threads.** `/proc/self/task/*/comm`, `stat` (CPU time) and
  `sched_getscheduler()` before filly exists and after the run.
- **Runs.** `run.sh`: 14 runs of 3,600 frames, one at a time, 3 s apart,
  with a run without filly first and last.
- **Web.** The page loads filly's ES module, then ysp's module. ysp opens
  its screen (SDL3 makes the WebGL2 context), makes an RGBA8 texture with
  `glTexStorage2D`, and calls an `EM_ASYNC_JS` function that passes
  `GL.currentContext.GLctx` and `GL.textures[name]` to the page, which
  calls `createRenderer(gl)` and `importTarget(texture, w, h)` and waits for
  `prepare()`. Each frame calls an `EM_JS` function that sets the pose,
  calls `render()` and `handBack()`, and returns its time from
  `performance.now()`. ysp then calls `ygfx_reset_state()` (or not, with
  `--no-reset`) and draws. The frame interval is the browser's
  `requestAnimationFrame` time (`Module.yscrRafT`), as in
  `examples/web/selftest.c`.

## 1. The shared context

### 1.1 Driver level (`share_test.c`)

Each test makes a 64 x 64 RGBA8 texture, clears it to (0.25, 0.5, 0.75) in
the producer context, orders it with the route's sync, and reads texel
(5, 5) in the consumer context.

| Test | Result |
|---|---|
| GLX: ES 3.0 context (`GLX_EXT_create_context_es2_profile`), then a 4.1 core forward-compatible context that shares it | Made, no X error. Mesa gave "OpenGL ES 3.2" and "4.6 (Core Profile)". `glIsTexture()` of the ES name is true in the core context |
| GLX: the reverse order (ES sharing a core context) | Made, no X error |
| GLX: core writes, `glFenceSync` + `glFlush`, ES `glWaitSync`, read | 64 128 191 255: pass |
| EGL (surfaceless): ES 3.0, then a 4.1 core context that shares it | Made (`EGL_SUCCESS`); read 64 128 191: pass |
| EGLImage on one EGLDisplay (`EGL_KHR_gl_texture_2D_image`): core texture to an ES context with no sharing, EGL fence and `eglWaitSyncKHR()` | Pass |
| dma-buf: `eglExportDMABUFImageMESA` (fourcc `AB24`, 1 plane, modifier `0x100000000000002`, stride 512), `EGL_LINUX_DMA_BUF_EXT` import, `glEGLImageTargetTexture2DOES`, EGL fence | Pass |

So the error the brief expected from Mesa did not occur: Mesa shares
objects between ES and desktop contexts of one screen. Other drivers are
not tested (see "Not tested").

### 1.2 The routes with Filament

| Route | Result |
|---|---|
| (a) Filament shares ysp's GLX ES 3.0 context (`filly::Renderer(current_gl_context())`, filly's patched `PlatformGLX` on ysp's X connection) | Works. Renderer made in 9.5 to 10.3 ms. Pixel-exact (section 3). Timing in section 2 |
| (b) Filament's GL ES backend on a context shared with ysp's | Works with Filament and gltfio (`gles_share.cpp`); not with filly. Details below |
| (c) dma-buf or EGLImage between Filament's context and ysp's | Works at the driver level (1.1). Not wired to filly |
| (d) filly's offscreen renderer (its own EGL context), `read()`, `ygfx_texture_update()` | Works. Timing in 1.4 |

Route (b) in detail:

- `FILAMENT_USE_EXTERNAL_GLES3` is not the switch: it removes Filament's
  OpenGL backend from the build. `FILAMENT_SUPPORTS_EGL_ON_LINUX`
  compiles the backend against `GLES3/gl31.h` (so it is the GL ES backend)
  with `PlatformEGLHeadless`, and `FILAMENT_LINUX_IS_MOBILE` compiles the
  materials for GLSL ES (`matc -p mobile`).
- That combination does not build as released: `PlatformEGLHeadless.cpp`
  includes and calls BlueGL, which the mobile configuration does not
  build. The probe guards the two places with a macro
  (`FILAMENT_LINUX_IS_MOBILE_PROBE`); with a GL ES backend that links
  `libGLESv2` directly, BlueGL is not needed. The mobile configuration
  also builds no host tools; the probe imported `matc`, `resgen`, `uberz`
  and the others from the desktop build tree
  (`FILAMENT_IMPORT_PREBUILT_EXECUTABLES_DIR`).
- Filament's `PlatformEGL` takes `eglGetDisplay(EGL_DEFAULT_DISPLAY)`, and
  EGL shares objects only within one display, so ysp's headless context
  in `gles_share.cpp` uses that display too, not the surfaceless one of
  `tests/adapt/gfx_headless.h`. ysp's on-screen context on Linux is GLX,
  which cannot share with EGL; a ysp screen would need an EGL backend, or
  Filament a GLX ES platform, for route (b) on the screen.
- Result: "Filament engine with a shared ES context: 4.9 to 7.3 ms",
  feature level 1. gltfio's ubershader archive (mobile) loaded the horse;
  ysp drew the texture, and 23,804 pixels of the model's square differ
  from Filament's clear color. Filament has no public GPU-side wait for
  another context, so the probe waited on the CPU
  (`Fence::waitAndDestroy`): 0.015 / 0.153 / 0.565 ms, unpaced, 590
  frames. filly adds the GPU-side wait through its own platform subclass
  (`Engine::createSync()`), which a GL ES port would keep.

### 1.3 What route (c) needs from filly

filly creates its own targets' color textures in Filament's context, on
the driver thread, and runs its own tasks there (`GlInterop::run`), but
neither the texture's name nor the context is public. For route (c) filly
would need either a call that exports a target as an EGLImage or a dma-buf
(the producer side of 1.1), or an import of a host EGLImage as a target.
On Linux route (a) makes this unnecessary; the same shape (a native handle
and a cross-API fence) is what Windows and macOS need (see "Not tested").

### 1.4 Route (d): readback cost

`filly_ysp --mode readback` and `--mode readback-async`; 3,590 frames each
(ms, p50 / p99 / max). "Wait" is `read()`; the pipelined mode reads the
previous frame after it submits this one.

| Target | Mode | filly phase | `read()` wait | `ygfx_texture_update` | GPU frame p50 | Driver thread CPU a frame |
|---|---|---|---|---|---|---|
| 512 x 512 | read | 1.252 / 1.407 / 2.650 | 1.079 / 1.211 / 1.694 | 0.119 / 0.154 / 0.272 | 0.219 | 0.22 |
| 512 x 512 | pipelined | 0.267 / 0.446 / 0.983 | 0.185 / 0.315 / 0.665 | 0.043 / 0.065 / 0.228 | 0.217 | 0.19 |
| 1024 x 1024 | read | 2.620 / 2.872 / 3.088 | 2.327 / 2.442 / 2.644 | 0.184 / 0.470 / 0.720 | 0.567 | 0.46 |
| 1024 x 1024 | pipelined | 0.528 / 1.087 / 3.343 | 0.282 / 0.609 / 1.952 | 0.179 / 0.495 / 1.284 | 0.566 | 0.34 |
| 1920 x 1080 | read | 4.944 / 5.209 / 8.053 | 4.035 / 4.289 / 7.085 | 0.875 / 0.930 / 0.991 | 0.971 | 0.88 |
| 1920 x 1080 | pipelined | 1.614 / 2.249 / 5.841 | 0.685 / 1.219 / 3.510 | 0.907 / 1.045 / 2.148 | 0.968 | 0.76 |
| 1920 x 1080, wooden horse | pipelined | 2.048 / 2.260 / 5.215 | 1.028 / 1.222 / 3.269 | 0.934 / 1.088 / 1.731 | 1.536 | 1.09 |

- The synchronous `read()` waits for the GPU, then filly's driver thread
  copies the rows, flipped, from a mapped pixel buffer; the caller waits
  for both. Sync: filly's GL fence and a CPU wait on its driver thread.
- No frame dropped in any readback run: at 60 Hz, 5 to 8 ms fits in the
  frame.
- Filament's GPU frame was 0.97 ms here at 1920 x 1080 against 0.33 ms
  on route (a). The two runs differ in context (EGL offscreen, GLX
  shared) and in how long the GPU idles each frame; the cause was not
  found.

## 2. Cost in the ysp frame loop

`run.sh`, 3,600 frames for each run, 3,590 measured (ms, p50 / p99 /
max). filly phase = `render()` + `acquire()` on route (a).

| Run | filly phase | `render()` | `acquire()` | gfx begin..end | GPU frame p50 / p99 | Records: dropped, late | Interval p99 / max | Intervals over 25 ms |
|---|---|---|---|---|---|---|---|---|
| No filly (first) | | | | 0.026 / 0.046 / 0.133 | | 0, 0 | 16.668 / 33.334 | 1 |
| Shared, 512 x 512 | 0.100 / 0.156 / 0.317 | 0.038 / 0.068 / 0.123 | 0.062 / 0.116 / 0.194 | 0.014 / 0.031 / 0.058 | 0.074 / 0.104 | 0, 0 | 16.668 / 16.674 | 0 |
| Shared, 1024 x 1024 | 0.149 / 0.194 / 0.359 | 0.042 / 0.055 / 0.114 | 0.106 / 0.148 / 0.245 | 0.024 / 0.031 / 0.061 | 0.220 / 0.256 | 0, 0 | 16.668 / 16.682 | 0 |
| Shared, 1920 x 1080 | 0.131 / 0.176 / 0.264 | 0.039 / 0.065 / 0.113 | 0.097 / 0.138 / 0.180 | 0.011 / 0.025 / 0.053 | 0.334 / 0.344 | 0, 0 | 16.668 / 16.675 | 0 |
| Shared, 1920 x 1080, wooden horse | 0.126 / 0.162 / 1.288 | 0.042 / 0.055 / 0.107 | 0.084 / 0.115 / 1.248 | 0.024 / 0.034 / 0.058 | 0.401 / 0.475 | 0, 0 | 16.668 / 16.675 | 0 |
| No filly (last) | | | | 0.018 / 0.032 / 0.103 | | 0, 0 | 16.668 / 16.675 | 0 |

- Every record of every run was a copy (path composed, tier 2); none was
  skipped.
- The one long interval is in the first run without filly: frame 2353
  came 33.334 ms after frame 2352, with `dropped` 0 and no flag, so the
  frame was planned for the later vblank (the frame thread was late on
  the host, not on the GPU). No run with filly had one.
- **Threads.** Before filly: the main thread and Mesa's threads for ysp's
  context (`cs0`, `sh0` to `sh3`, `shlo0`, `disk$0`, `gdrv0`; `shlo0` and
  `disk$0` are SCHED_BATCH). filly's renderer added 18
  `JobSystem::loop`, `FEngine::loop`, `ServiceThread`,
  `CompilerThreadPool`, `FrameInfoGpuCom` and Mesa's threads for
  Filament's context (`gdrv0`, `sh4`). The job threads used 0.00 s each
  over 60 s; `FEngine::loop` used 0.15 to 0.22 s on route (a) (0.04 to
  0.06 ms a frame) and 0.67 to 3.9 s on route (d). All new threads were
  SCHED_OTHER. Filament's `JobSystem::setThreadPriority()` has code only
  for Android and Apple; on Linux it does nothing, so the threads take the
  creator's policy (pthread's default `PTHREAD_INHERIT_SCHED`).
- **The frame thread and the driver thread.** On route (a) the frame
  thread waits in `acquire()` for the driver thread to make the fence,
  0.06 to 0.11 ms p50. That is the only coupling on the CPU; it did not
  make a frame late. With an elevated frame thread and a SCHED_OTHER
  driver thread, a busy machine could stretch that wait; this was not
  tested (no thread was elevated).
- filly logs "FrameInfo's circular queue is full" on each render of an
  unpaced loop (here the `prepare()` loop before the timed frames, and
  `gles_share`); a vblank-paced loop logs none, as filly's design notes
  say.

## 3. GL state

`filly_ysp --mode shared --check` and the web page with `--check`. Five
frames without any filly call give the reference output. Then 120 frames
with filly, and every 10th pose twice: once through the shared texture
and once through a CPU copy of the same pose (natively a Filament-owned
target in the same renderer and `read()`; on the web `ImportedTarget.read()`),
uploaded in GL's row order to a second ysp texture. `ygfx_read_output()`
reads the whole window each time.

| Case | Poses | Values outside the model that differ from the output before filly | Values in the model that differ, shared against CPU copy |
|---|---|---|---|
| Native, route (a) | 12 | 0 | 0 (max 0) |
| Chromium, `ygfx_reset_state()` after filly | 12 | 0 | 0 |
| Chromium, no reset | 12 | 0 | 0 |
| Firefox, reset | 12 | 0 | 0 |
| Firefox, no reset | 12 | 0 | 0 |

- Natively filly's GL work runs in Filament's own context. ysp's context
  sees only `import_gl_target()` (binds the texture to check it and
  restores the binding), `glWaitSync()` and `glFenceSync()`. The host's
  own GL calls (making the texture) need `ygfx_reset_state()`, as the gfx
  manual says.
- On the web both share one context. filly's `handBack()` unbinds what the
  core bound, restores GL defaults (scissor, stencil, masks) and the page's
  pixel-store values. With these stimuli (a gabor, a rectangle, images)
  gfx drew correctly without a reset. That is a result for this set, not a
  guarantee: gfx's cache can hold state that `handBack()` resets to a
  default, so the contract keeps the reset after the renderer's calls.
  Its cost was not measurable: gfx begin..end 0.015 / 0.035 ms with the
  reset, 0.020 / 0.040 ms without (Chromium, p50 / p99).

## 4. The web

Two Emscripten modules share the canvas's WebGL2 context. filly's
`createRenderer(gl)` registers the context object with filly's own
Emscripten GL layer; ysp's module keeps its own. The glue in `ysp`'s C
code is four `EM_JS` functions (`web/filly_web.c`); the page holds
filly's objects (`web/index.html`).

3,600 frames a run, 3,610 measured (ms, p50 / p99 / max). filly = the
pose, `render()` and `handBack()`, from `performance.now()` in the page.

| Browser | Run | filly | gfx begin..end | rAF interval | Over 1.5 periods | GPU frame p50 / p99 | Dropped, late |
|---|---|---|---|---|---|---|---|
| Chromium 159 | No filly | | 0.020 / 0.055 / 0.315 | 16.665 / 16.670 / 16.675 | 0 | | 0, 0 |
| Chromium 159 | 512 x 512 | 0.135 / 0.340 / 0.750 | 0.015 / 0.035 / 0.160 | 16.665 / 16.675 / 16.680 | 0 | 0.100 / 0.104 | 0, 0 |
| Chromium 159 | 512 x 512, no reset | 0.210 / 0.365 / 0.845 | 0.020 / 0.040 / 0.235 | 16.665 / 16.670 / 16.675 | 0 | 0.100 / 0.104 | 0, 0 |
| Chromium 159 | 1920 x 1080 | 0.185 / 0.345 / 0.655 | 0.015 / 0.035 / 0.170 | 16.665 / 16.675 / 16.675 | 0 | 0.305 / 0.338 | 0, 0 |
| Firefox 128 ESR | No filly | | 0.040 / 0.080 / 0.160 | 17.06 / 17.12 / 83.4 | 1 | | 0, 0 |
| Firefox 128 ESR | 512 x 512 | 0.240 / 0.540 / 1.820 | 0.020 / 0.060 / 0.180 | 17.06 / 17.12 / 17.20 | 0 | none | 0, 0 |
| Firefox 128 ESR | 1920 x 1080 | 0.220 / 0.560 / 1.840 | 0.020 / 0.060 / 0.180 | 17.06 / 17.12 / 33.14 | 1 | none | 0, 0 |

- Firefox's `requestAnimationFrame` times advanced 17.06 ms p50 a
  callback in the loop, with and without filly, while ysp's settle at open
  measured a period of 16.64 to 16.69 ms (the describe line's
  `raf_period`). The cause was not looked into; it is the same in the
  run without filly. Firefox's clock step in an isolated page was 20 us
  (`clock_step=20000ns`), Chromium's 5 us. Firefox gives no GPU timer to
  WebGL, so filly has no GPU time there.
- Setup: Chromium loaded filly's module in 26 ms, made the renderer in
  25 ms, loaded the model in 29 ms, `prepare()` took 83 to 133 ms and the
  first render 40 to 62 ms. Firefox took 7,581 to 8,316 ms to load the
  module (3.66 MB of wasm with wasm exceptions), 14 ms for the renderer,
  8 to 9 ms for the model; `prepare()` returned at once (no
  `KHR_parallel_shader_compile`) and the first render took 32 to 90 ms.
- **How a C host calls filly's web build.** filly's web build is an ES
  module (`filly.js`) over an Emscripten factory (`filly-core.js`,
  `filly-core.wasm`), released as `filly-web-<version>.zip` and as the npm
  package `@aforren1/filly` (the npm registry returned 404 for it on
  2026-11-18; the GitHub release has the zip). A C/Emscripten program
  cannot link it; it calls it through the page: C, `EM_JS`, the page's
  JavaScript, `filly.js`, embind, filly's wasm. The measured filly time
  above includes those crossings. A single-module build (the eject path)
  would link filly's core and Filament's WebAssembly libraries into ysp's
  module. That needs one emscripten version for both (filly pins 5.0.4,
  ysp 6.0.12) and filly's hand-back (`filly.js` and `gl_tracking.js`) as
  a JavaScript library of that module; it was not built.
- **A property collision.** filly's `textureName()` stores its own GL
  name in the `name` property of the `WebGLTexture`, the property
  Emscripten's GL layer in ysp's module also sets in `glGenTextures()`.
  ysp's module finds its texture by its own table, so drawing worked, but
  a `glGet` of a texture binding in ysp's module would return filly's
  number, and `glDeleteTextures()` in ysp's module sets it to 0. The
  host should not delete an imported texture while filly holds it.

## 5. The interface

What worked fixes the shape: the host makes the texture and owns it; the
renderer queues its frame from the frame thread and does not wait for the
GPU; a GPU-side wait in the host's context orders the read after the
write, and a host fence orders the next write after the read; on one
context (web, ANGLE) the renderer hands the state back and the host
drops its cache.

```c
/* What the host gives a renderer at create. */
typedef struct ysp_ext_gpu {
    uint32_t kind;          /* YSP_EXT_GPU_GLX, _EGL, _WEBGL, _ANGLE_D3D11, _ANGLE_METAL */
    void*    display;       /* Display* (GLX) or EGLDisplay                          */
    void*    context;       /* the host's GLXContext or EGLContext, current on the
                             * calling thread during create, destroy, acquire and
                             * release                                               */
    void*    device;        /* ID3D11Device* (yscr_native), or NULL                  */
    uint32_t luid[2];       /* the adapter, to open a second device on it            */
    int32_t  gles;          /* 1: the host context is GL ES (always, today)          */
} ysp_ext_gpu;

/* The texture the host owns and the renderer writes. */
typedef struct ysp_ext_target {
    uint32_t kind;          /* _GL_SHARED: a GL name the renderer's context shares;
                             * _GL_SAME: the host's own context (web, ANGLE);
                             * _D3D11_HANDLE, _IOSURFACE, _DMABUF: a native handle;
                             * _CPU: rows the host uploads (the readback fallback)   */
    uint32_t gl_name;
    intptr_t native;        /* HANDLE, IOSurfaceRef or dma-buf fd                    */
    int32_t  w, h;
    uint32_t format;        /* RGBA8 in version 1                                    */
    uint32_t encoding;      /* what the renderer writes: SRGB or LINEAR              */
    uint32_t flags;         /* PREMULTIPLIED, ROWS_BOTTOM_UP                         */
} ysp_ext_target;

typedef struct ysp_ext_frame {
    int64_t onset_ns;       /* yscr_frame.onset: pose the scene for this time        */
    int64_t period_ns;
    int64_t index;
} ysp_ext_frame;

typedef struct ysp_ext_timing {
    uint32_t cpu_ns;        /* render() and acquire() on the frame thread            */
    uint32_t gpu_ns;        /* of frame gpu_index; YSCR_PHASE_UNKNOWN when none      */
    int64_t  gpu_index;     /* arrives two or more frames late                       */
} ysp_ext_timing;

typedef struct ysp_renderer_vt {
    uint32_t version, size;
    const char* name;
    const char* license;    /* the manifest records it beside the module's hash      */
    uint32_t target_kinds;  /* bits: what it can write                               */
    int  (*create)(const ysp_host* host, const ysp_ext_gpu* gpu, void** self);
    void (*destroy)(void* self);
    /* The parameter table (rows as ygfx_param) and a setter, for the designer's
     * panels and the script binding. Between frames. */
    const ygfx_param* (*params)(void* self, int* n);
    int  (*set)(void* self, int param, const double* v, int n);
    int  (*load)(void* self, const char* resource);   /* through host->resource()   */
    int  (*target)(void* self, const ysp_ext_target* t);  /* returns a target id    */
    void (*target_free)(void* self, int target);
    /* Compiles what the next frames need; 1 when ready. Between trials. */
    int  (*prepare)(void* self);
    /* Frame thread, before ygfx_begin(): queue the frame for f->onset_ns. Must
     * not wait for the GPU. */
    int  (*render)(void* self, const ysp_ext_frame* f, int target);
    /* Host context current. acquire before the host's first draw that samples
     * the target (a GPU wait: glWaitSync, a keyed mutex, nothing on one
     * context); release after ygfx_end() (a host fence that the next write
     * waits on). */
    int  (*acquire)(void* self, int target);
    int  (*release)(void* self, int target);
    /* One context only: restore what the host expects; the host then calls
     * ygfx_reset_state(). */
    void (*hand_back)(void* self);
    void (*timing)(void* self, ysp_ext_timing* out);
} ysp_renderer_vt;
```

The host functions it needs, beyond section 12's list:

- the native context and device: on Linux the GLX display and context
  (or EGL), on Windows `yscr_native()`'s D3D11 device and ANGLE's
  EGLDisplay, on the web the `WebGL2RenderingContext` object;
- a texture of the renderer's format with immutable storage, and its
  native name (today the probe makes it with raw GL and imports it; a
  `ygfx_target_native()` or a gfx call that makes an importable texture
  would remove the raw GL and the reset after it);
- a row-order flag in `ygfx_import_desc` (or a source rectangle with a
  negative height): without it filly's frame shows upside down;
- `ygfx_reset_state()` after `hand_back()`, and an extension phase in
  `yscr_record.phase_ns` for `cpu_ns` (the probe used `SCRIPT`);
- a rule for threads: the host calls `create()` before
  `yrt_thread_elevate()`, or from a SCHED_OTHER thread.

What filly's API gives today: a renderer that shares the current host
context (`Renderer(shared_context)`), import of a host texture as a
target (`import_gl_target`), a `render()` that does not wait, the fence
pair (`ImportedTarget::acquire()` and `release()`), `prepare()`,
`Stats` with CPU submit and wait times and a late GPU time, the web
build's `handBack()` and `onHandBack`, readback (`begin_read()`), and a
complete scene API (models, cameras, lights, animation, materials,
picking).

What it lacks for the vtable: a C ABI (the core is C++20 with exceptions;
an adapter of a few hundred lines would wrap it); a parameter table (its
parameters are object properties); a GL ES desktop path (route b) and
native-handle targets (D3D11, dma-buf; IOSurface exists only as
`CopyTarget` on macOS); a row-order choice; control of Filament's thread
count (`Engine::Config::jobSystemThreadCount`). Time is already the
caller's: filly has no clock, so the predicted onset goes in directly.

## 6. Licenses and sizes

| Component | License | Linked by |
|---|---|---|
| filly | MIT | the extension |
| Filament (filament, backend, gltfio, utils, ibl, geometry, filabridge, filaflat, uberz, ktxreader, image, bluegl) | Apache 2.0 | filly |
| abseil, Draco, Basis Universal | Apache 2.0 | Filament |
| zstd | BSD 3-clause | Filament |
| meshoptimizer, cgltf, robin-map, smol-v | MIT | Filament, filly |
| mikktspace | zlib-style | Filament |
| stb | public domain or MIT | Filament |
| libwebp (decoder) | BSD 3-clause and a patent grant | filly |

Apache 2.0 asks that a binary distribution carries the license and the
NOTICE files of the parts it contains; filly's packages already ship the
license files. Nothing here is copyleft, so a player with filly stays
under ysp's terms for ysp's code.

Sizes (Linux, release, no debug information):

| Item | Size |
|---|---|
| Filament's static libraries that filly links (22 archives, PIC) | 17.4 MB |
| filly's core and material archives | 1.99 MB |
| `filly_ysp` stripped (ysp, SDL3 shared, filly, Filament) | 6.13 MB |
| `gfx_hello` stripped (ysp and SDL3 shared only) | 0.52 MB |
| `filly-core.wasm` | 3,664,049 bytes; 1,139,308 with brotli 11; 1,469,931 with gzip 9 |
| `filly-core.js` and `filly.js` | 142,865 and 47,514 bytes; 31,795 and 12,328 with brotli |
| ysp's probe module, `filly_web.wasm` (for comparison) | 1,279,624 bytes; 322,732 with brotli |

## Not tested

- **Windows.** ysp's context is ANGLE (GL ES 3.0 on D3D11), and filly's
  is WGL (desktop GL 4.1) on the vendor driver; WGL and ANGLE contexts
  cannot share objects. Two routes: (1) route (b): Filament's GL ES
  backend with an EGL platform on ANGLE's EGLDisplay, sharing ysp's
  context, which this probe showed on Mesa; filly would need an EGL and
  GL ES interop and a mobile material archive. (2) A D3D11 shared
  handle: a texture made on ANGLE's device (`yscr_native()`) with
  `D3D11_RESOURCE_MISC_SHARED_NTHANDLE` and a keyed mutex, opened by the
  vendor GL through `WGL_NV_DX_interop2` (or `GL_EXT_memory_object_win32`)
  on the same adapter (the LUID), and filly rendering into it as an
  imported target; ysp imports it with `ygfx_texture_import()`
  (`YGFX_IMPORT_D3D11`), and the keyed mutex orders the write before
  `ygfx_end()` as the gfx manual describes. Readback works anywhere.
- **macOS.** ysp is ANGLE on Metal; filly is CGL (GL 4.1 on Metal), and
  CGL contexts do not share with ANGLE's. filly's `CopyTarget` already
  copies each frame through an IOSurface; ysp would bind the same
  IOSurface through `EGL_ANGLE_iosurface_client_buffer`. No GL fence
  crosses the two; filly waits on the CPU for the copy (`acquire()`).
- Drivers other than Mesa radeonsi: NVIDIA's and Intel's sharing between
  ES and desktop contexts, and the dma-buf modifiers.
- Fullscreen (independent flip, tier 1), a photodiode, an elevated frame
  thread, and load: every run was windowed, idle, at normal priority.
- The 500 Hz target: only 60 Hz was measured.
- Route (b) with a ysp screen (ysp's Linux screen is GLX; route (b) ran
  headless on EGL), with filly, and its timing at vblank pace.
- The single-module web build (the eject path) and the npm package (the
  registry returned 404).
- Several renderers or targets at once, `prepare()` between trials, and
  model loads during a session.

## How to rebuild

From the repository root, with a scratch folder `$W` (about 6 GB during
the builds):

```sh
git clone https://github.com/aforren1/filly $W/filly          # v0.5.0
# Clang for the native builds: emsdk's, through a wrapper that appends
# -Wno-deprecated-declarations, as clang and clang++ in $W/clangbin.
PATH=$W/clangbin:$PATH uv run --no-project --python 3.12 --with cmake --with ninja \
    python -I $W/filly/tools/build_filament_linux.py --work $W/fil-work --output $W/fil-sdk --jobs 16
uv run --no-project --with cmake --with ninja sh -c "cmake -S $W/filly -B $W/filly-build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=$W/clangbin/clang -DCMAKE_CXX_COMPILER=$W/clangbin/clang++ \
    -DFILLY_PYTHON=OFF -DFILAMENT_ROOT=$W/fil-sdk && cmake --build $W/filly-build"
FILLY=$W/filly FILLY_BUILD=$W/filly-build FILAMENT=$W/fil-sdk SDL3=/home/bwadmin/sdl3-3.4.18 \
    CXX=$W/clangbin/clang++ sh tests/probe/filly/build.sh $W/bin
$W/bin/share_test
sh tests/probe/filly/run.sh $W/bin $W/filly $W/runs          # about 16 minutes, in the X session
$W/bin/filly_ysp --mode shared --check --model $W/filly/samples/horse.glb
```

Route (b): in `$W/fil-work/build`, also build `cmgen filamesh mipgen resgen
glslminifier`; copy the source root's `ImportExecutables-Release.cmake` to
`hosttools/ImportExecutables-Prebuilt.cmake` with `IMPORTED_LOCATION_RELEASE`
renamed to `IMPORTED_LOCATION`; guard the BlueGL include and
`bluegl::bind()` in `PlatformEGLHeadless.cpp` with
`#if !defined(FILAMENT_LINUX_IS_MOBILE_PROBE)`; configure a second tree with
filly's tool's flags plus `-DFILAMENT_SUPPORTS_EGL_ON_LINUX=ON
-DFILAMENT_LINUX_IS_MOBILE=ON -DFILAMENT_IMPORT_PREBUILT_EXECUTABLES_DIR=hosttools
-DFILAMENT_BUILD_TESTING=OFF` and `-DFILAMENT_LINUX_IS_MOBILE_PROBE` in
`CMAKE_CXX_FLAGS`; build `gltfio_core uberarchive filament backend` and
the other libraries; then `FILAMENT_GLES_BUILD=<tree> sh
tests/probe/filly/build.sh $W/bin` and `$W/bin/gles_share horse.glb out.ppm`.

Web, with emsdk's `emcc` on PATH and `filly-web-0.5.0.zip` unzipped to
`$W/filly-web`:

```sh
FILLY_WEB=$W/filly-web FILLY=$W/filly sh tests/probe/filly/web/build.sh $W/site
uv run --no-project python -I tests/probe/filly/web/serve.py $W/site $W/webruns 8766 &
PLAYWRIGHT_BROWSERS_PATH=$W/pw uv run --no-project --with playwright playwright install chromium
PLAYWRIGHT_BROWSERS_PATH=$W/pw uv run --no-project --with playwright python -I \
    tests/probe/filly/web/drive.py $W/webruns 8766 chromium "--mode filly --frames 3600"
uv run --no-project python -I tests/probe/filly/web/drive.py $W/webruns 8766 firefox-system "--mode filly --check"
```
