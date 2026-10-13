# The web backend and the web demos

`ysp/screen.h` v0.7.0 has a backend for the browser, `YSCR_BACKEND_WEB`.
A program written as a blocking frame loop compiles with Emscripten and
runs in a page: SDL3's Emscripten port gives the canvas and the input,
the header makes a WebGL2 context, and `requestAnimationFrame` paces the
frames. v0.7.1 stamps key, mouse, touch and pen presses with the DOM
event's own time; v0.7.2 keeps SDL from leaving the page's fullscreen. `ysp/audio.h` v0.4.1 plays sound through Web Audio, with
onset records from the browser's output timestamps. `examples/web/` has
five demos and a static site that GitHub Pages serves; each demo works
with a keyboard on a desktop and by touch alone on a phone or a tablet.

Browser timing is not rig timing. A browser does not report when the
display showed a frame or when the speaker played a sound, gives a page
no control of the display or the audio device, and runs
other work on the same thread. In a browser, timing is measured and
reported, never controlled (docs/rig_spec.md, section 6).

This page has how-to guides, a reference, and an explanation of what a
browser can and cannot claim.

## How to build and serve the demos

You need [emsdk](https://emscripten.org/docs/getting_started/downloads.html)
6.0.12, the version the Pages workflow pins.

1. Install and activate emsdk:

   ```sh
   git clone https://github.com/emscripten-core/emsdk
   cd emsdk
   ./emsdk install 6.0.12
   ./emsdk activate 6.0.12
   source ./emsdk_env.sh        # emsdk_env.bat on Windows
   ```

2. From the repository root, build the site:

   ```sh
   sh examples/web/build.sh web_site
   ```

   The script copies `examples/web/site/`, fetches `coi-serviceworker.js`
   at a pinned commit and miniaudio 0.11.25 and checks their SHA-256, and
   compiles each demo to `web_site/<name>.js` and `web_site/<name>.wasm`;
   the sound demo twice, `av_sync` and `av_sync_mt`. The first build also
   compiles SDL3's port, without and with threads (about 40 s each).

3. Serve the directory and open <http://localhost:8000/>:

   ```sh
   uv run --no-project python -m http.server -d web_site 8000
   ```

   `localhost` is a secure context, so the service worker registers and the
   page reloads once, isolated. A page opened from `file://` does not run.

To check isolation without the service worker, serve the site with the
headers yourself. This server sets `Cross-Origin-Opener-Policy:
same-origin` and `Cross-Origin-Embedder-Policy: require-corp`:

```python
# coi_server.py: uv run --no-project python coi_server.py web_site 8000
import functools, http.server, sys
class H(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        super().end_headers()
http.server.ThreadingHTTPServer(("127.0.0.1", int(sys.argv[2])),
    functools.partial(H, directory=sys.argv[1])).serve_forever()
```

## How to publish the demos on GitHub Pages

`.github/workflows/pages.yml` builds the site in the `emscripten/emsdk:6.0.12`
container on each push to `main` and on a manual run, and deploys it with
`actions/upload-pages-artifact` and `actions/deploy-pages`.

1. In the repository on GitHub, open **Settings > Pages**.
2. Under **Build and deployment > Source**, select **GitHub Actions**.
3. Push to `main`, or run the **Pages** workflow from the **Actions** tab.

The site is then at `https://<owner>.github.io/<repository>/`. GitHub Pages
sets no COOP or COEP headers, so `coi-serviceworker.js` isolates the pages
(see [Isolation](#isolation-and-the-service-worker)).

## How to build your own program for the browser

1. Write the program as for a rig: `yscr_open()`, then a loop of
   `yscr_begin()`, drawing, `yscr_flip()`. `YSCR_BACKEND_AUTO` is the web
   backend under Emscripten.
2. Open a window (`desc.windowed = true` and a size in CSS pixels), or set
   `desc.windowed = false` for a canvas the size of the page's viewport.
3. Compile with the flags of `examples/web/build.sh`:

   ```sh
   emcc -O2 -std=c11 -Iinclude -DYRT_NO_THREADS -sUSE_SDL=3 \
        -sMIN_WEBGL_VERSION=2 -sMAX_WEBGL_VERSION=2 \
        -sASYNCIFY -sASYNCIFY_STACK_SIZE=65536 -sALLOW_MEMORY_GROWTH=1 \
        -sSTACK_SIZE=1MB -sENVIRONMENT=web \
        -sEXPORTED_FUNCTIONS=_main,_yscr_request_abort \
        -sEXPORTED_RUNTIME_METHODS=callMain -o app.js app.c
   ```

4. Give the page a `<canvas id="canvas">`, set `Module.noInitialRun = true`,
   and call `Module.callMain(args)` from a button's click handler, in the
   handler itself, not from a timer: that click is the user gesture that
   fullscreen and Web Audio need, and iOS Safari takes no later one for an
   AudioContext.
5. Hand the data to the page from C with an `EM_JS` function, as
   `examples/web/tutorial.c` does: `ytr_data` writes the rows to a memory
   buffer through `desc.write`, and the page offers them as a download.
6. Make and prime the stimuli (`ygfx_prime()`) before the first trial, then
   tell the page, which shows the canvas then (`web_ready()` in
   `examples/web/web_common.h`; see [Priming](#priming-before-the-page-shows-the-canvas)).

## How to add sound

1. Open `ysp/audio.h` first in `main()`, before the screen, so the
   AudioContext is made inside the Start click (step 4 above). Leave
   `desc.backend` at AUTO.
2. Build twice and let the page pick one by `crossOriginIsolated`:

   ```sh
   # an isolated page: threads, miniaudio's AudioWorklet path
   emcc ... -std=gnu11 -I<miniaudio dir> -pthread -Wno-pthreads-mem-growth \
        -DMA_ENABLE_AUDIO_WORKLETS -sAUDIO_WORKLET=1 -sWASM_WORKERS=1 -o app_mt.js app.c
   # every other page: no threads, miniaudio's ScriptProcessorNode path
   emcc ... -std=gnu11 -I<miniaudio dir> -DYRT_NO_THREADS -o app.js app.c
   ```

   miniaudio's EM_ASM needs `-std=gnu11`. Both builds need ASYNCIFY or
   JSPI. In the page, set `yspPage.builds = { coi: 'app_mt.js', fallback:
   'app.js' }` before `ysp-web.js`; it loads the one the page can run.
3. On the ScriptProcessorNode path, ask for 2048 frames (`desc.period`;
   43 ms at 48 kHz): its callbacks run on the page's main thread, and 512
   frames gave onsets up to 5.6 ms off their plan. After the screen's
   setup, close the sound and open it again: a main-thread stall of
   seconds with a ScriptProcessorNode running left its timing wrong (see
   [Audio](#audio-what-the-records-say)).
4. Plan each sound at least `yau_lead_ns()` ahead (about 60 ms on the
   AudioWorklet path, 140 to 170 ms on the ScriptProcessorNode path in
   Chrome on the laptop below) with `yau_play_at()`, call `yau_update()`
   each frame, and read the record with `yau_result()`. Do not call
   `yau_wait()` in the frame loop.
5. Record the audio path and the record's tier (5) with the data.
   `examples/web/av_sync.c` and `av.html` do all of this.

## How to take touch responses

1. Open the window at the size of the stage the page gives (`--size WxH`
   from `ysp-web.js`), and size the stimuli in units of the canvas's
   shorter side (`ygfx_size()`): a phone's canvas has 2 to 3 device pixels
   per CSS pixel.
2. Read events with `yscr_poll()` and `yscr_event_input()`. Touch comes as
   `YIN_KIND_TOUCH` presses and releases, x and y 0..1 of the canvas, time
   the DOM event's `timeStamp`.
3. Turn a press into a choice by where it landed: `web_side()` in
   `examples/web/web_common.h` sets `code` to 1 (left half) or 2 (right
   half) for touch and for the primary mouse button. Give
   `ysp/response.h` the choices `{ kind TOUCH, match KEYCODE, code 1 }`
   and the like beside the keys, with the same names. Feed every event;
   the response, its RT and its kind come out of the one collector.
4. Write the response's kind to the data file (`rsp_kind`, from
   `yrsp_result.kind`).
5. Keep anything the participant must not tap off the canvas, or above it
   with `pointer-events: none` (the demos' target labels).

## How to check a demo on a phone or a tablet by hand

Chrome's device emulation checks the layout and the touch path, not a
real device. On each device, open the site over HTTPS (GitHub Pages) and:

1. On the index page, check that the status line says
   `cross-origin isolated: yes` after the one reload.
2. Run `tutorial.html` with touch only: the Start tap enters fullscreen
   (Android Chrome, iPad Safari; not iPhone Safari), the canvas fills the
   screen less the notch, the page does not scroll or zoom on a swipe, a
   pinch or a double tap, and the CSV has `rsp_kind` = `touch`.
3. Turn the device during a run: the canvas keeps its aspect and the
   session log has a `viewport ... during the run` line.
4. Run `av.html` with silent mode off: the status line names the audio
   path, the click is heard, and the summary has 20 trials with tier 5.
   On iOS Safari, if no sound plays, tap the page once more (miniaudio
   resumes its AudioContext on the next tap). Where the device has element
   fullscreen, the run stays fullscreen to the end. Switch to another app
   and back during a run: if the browser left fullscreen, the stage says
   so, and one tap brings it back without a response.
5. During each run with touch targets (tutorial, dot motion, gallery), in
   portrait and landscape, with the browser's toolbar shown and hidden:
   the two labels are whole, above the screen's bottom edge, the home
   indicator and the toolbar, with a gap about the height of a line of
   text.
6. Run `selftest.html` and tap the canvas a few times: the result says how
   many taps had the DOM's time.

## Reference

### The backend

The header's top comment, BACKENDS, YSCR_BACKEND_WEB, is the complete
reference. In short:

| Item | Behavior |
|---|---|
| Window | SDL3's Emscripten port; the canvas `#canvas` unless SDL's hint or window property names another |
| Context | WebGL2 (GL ES 3.0) made by the header: no alpha, depth, stencil or multisampling; drawing buffer not preserved; `powerPreference: high-performance`. `Module.yscrDesynchronized = true` asks for a desynchronized context (not measured) |
| Pacing | `yscr_begin()` awaits the next `requestAnimationFrame` callback (ASYNCIFY or JSPI). A callback that ran more than half a period ago is skipped |
| Record | onset: the first callback time after both the drawing callback and the present call; `YSCR_PATH_BROWSER`, `YSCR_TIER_WEB` (5, the lowest), `ONSET_PLANNED` |
| Grid | the callback times, correlated to the ysp_rt clock at open; off-grid times give the grid's onset (`GRID_UNSTABLE`) and no warning |
| Capability | `caps.kind = YSCR_CALLBACK`; `caps.period_ns` measured at open; `caps.mode` the drawing buffer in device pixels |
| Settling | at least 0.25 s, at most 2 s: 20 intervals, none over 1.5 times their median, SD at most 0.5 ms. Warns, never fails |
| Holds | none: a later target shows at the next frame and is recorded `EARLY` |
| Fullscreen | never requested by the header; `desc.windowed = false` sizes the canvas to the viewport at open. From v0.7.2 SDL cannot leave the page's fullscreen at open, at close or when the page is shown again (see [Fullscreen](#fullscreen)); SDL sends no `WINDOW_SHOWN` or `WINDOW_HIDDEN` for the page |
| Hidden page | `yscr_begin()` waits until the page is shown; the frame in flight completes `OCCLUDED`; `YSCR_EV_VISIBILITY` records |
| Input | SDL's events, stamped when SDL's handler ran. From v0.7.1 `yscr_event_input()` gives a key, mouse button, touch or pen press or release the DOM event's `timeStamp` where it finds its record; `yscr_poll()`'s time stays SDL's |
| Touch | SDL's finger events from pointer events on the canvas: x, y 0..1 of its CSS box, finger id the `pointerId` + 1; SDL's mouse events made from touch are dropped |
| Threads | the demos without sound build with `YRT_NO_THREADS`; `av_sync_mt` runs the screen with `-pthread` (checked in Chrome 154); triggers were not tested |
| Describe line | `raf_period`, `raf_settle`, `coi`, `clock_step`, `corr`, `desync`, `skipped`, `late`, `hidden`, `vis_changes`, `canvas`, `dpr`, `dom_stamps` (found/tried) |

### The audio path

`ysp/audio.h`'s top comment, BACKENDS, YAU_BACKEND_WEB, is the complete
reference. In short:

| Item | AudioWorklet (`av_sync_mt`) | ScriptProcessorNode (`av_sync`) |
|---|---|---|
| Page | cross-origin isolated (SharedArrayBuffer) | any |
| Build | `-pthread -sAUDIO_WORKLET=1 -sWASM_WORKERS=1 -DMA_ENABLE_AUDIO_WORKLETS` | `-DYRT_NO_THREADS` |
| Callback | audio thread, 128 frames (a render quantum) | main thread, `desc.period` rounded up to a power of 2 |
| Stream frame 0 in context frames | `currentFrame`, exact | `playbackTime`, late by up to a device chunk |
| Callback clock | `Date.now()`, 1 ms (the worklet has no `performance.now()`) | `performance.now()` |
| Position reports | `getOutputTimestamp()`, read every 10 ms on the main thread | the same, and in each callback |
| Tier | `YAU_TIER_WEB` (5), confirmed or not | the same |
| Describe line | `path=`, `reports=`, `base_latency=`, `output_latency=`, `block_moves=` | the same |
| Capture (`yau_capture`, v0.5.0) | compiled, never run; its callback times would read 0 | compiled, never run |

### The demos

| Page | Program | What it does |
|---|---|---|
| `tutorial.html` | `examples/web/tutorial.c` | A Gabor tilted left or right after a fixation cross; F or J, or a tap on a canvas half; 10 trials; RT from the flip record; results and a CSV from `ytr_data` with `rsp_kind` |
| `rdk.html` | `examples/web/rdk_staircase.c` | Random dot motion, left or right (`ysp/rdk.h`); the arrow keys or a tap on a canvas half; a 2-down 1-up staircase on coherence (`ysp/stair.h`); the threshold at the end |
| `av.html` | `examples/web/av_sync.c` | A flash and a click planned for one time, 20 trials; the two records, sound minus light, the browser's latency figures, every audio callback; two builds |
| `gallery.html` | `examples/gfx/gallery.c` | The `ysp/gfx.h` gallery, eight pages; a tap on the right half turns the page, on the left half turns back |
| `selftest.html` | `examples/web/selftest.c` | The measured period, a histogram of callback intervals, the clock step, isolation, drops, and the delay of keys and taps against the DOM's `timeStamp` |

Each page shows, and each CSV or JSON result records, whether the page was
cross-origin isolated (`coi`), the clock step, and the audio path (`none`
on every page but `av.html`). The four programs with `--sim` also build
natively (`web_tutorial`, `web_rdk_staircase`, `web_selftest`,
`web_av_sync`) and CI runs them on the simulated display (`web_av_sync` on
miniaudio's null device).

During a run, `ysp-web.js` and `style.css` give every page:

| Item | Behavior |
|---|---|
| Stage | covers the visual viewport (`visualViewport.height`, so not the part under a phone browser's toolbar; CSS falls back to `100dvh`, then `100vh`) less `env(safe-area-inset-*)`; the canvas opens at its size (`--size`), except the gallery's fixed 1200 x 760 |
| Rotation, fullscreen change | the drawing buffer keeps its size; the canvas's CSS box scales to fit, aspect kept; the session log counts each change (`viewport_changes`) |
| Scroll and zoom | `touch-action: none` and `overscroll-behavior: none` on the stage and the body, `gesturestart` stopped (iOS); the viewport meta has `viewport-fit=cover` |
| Fullscreen | asked from the Start tap when the box is checked; checked by default on a coarse pointer; iPhone Safari has no element fullscreen |
| Fullscreen lost | when the browser leaves fullscreen or refuses it during the run, the stage says so, and the next key or tap (not Esc, not the Stop button) asks again; that key or tap does not reach the program. The session log has a line for each, and `fullscreen_lost` counts them |
| Targets | two labels for the canvas halves with `pointer-events: none`, as wide as the canvas: under it where the stage has room, else inside its bottom edge. Margin 5 % and text 4.5 % (12 to 24 px) of the canvas's shorter side; placed again at each viewport change. The Stop button in the stage's corner |
| Ready | the canvas stays hidden ("Preparing...") until the program calls `web_ready()` after its prime |

### Sizes

emsdk 6.0.12, `-O2`, ASYNCIFY. gzip -9 is what a server that compresses
sends.

| Demo | wasm | wasm, gzip | js | js, gzip |
|---|---|---|---|---|
| tutorial | 1,413,407 | 465,295 | 216,720 | 48,955 |
| rdk_staircase | 1,369,055 | 450,111 | 216,740 | 48,983 |
| gallery | 1,521,496 | 493,965 | 216,553 | 48,774 |
| selftest | 1,075,629 | 323,835 | 215,729 | 48,804 |
| av_sync (no threads) | 1,392,633 | 453,186 | 225,205 | 50,751 |
| av_sync_mt (threads, AudioWorklet) | 1,458,322 | 470,922 | 260,100 | 57,603 |

Against the same demos built from main at 7316129 with the same emsdk, the
input stamps, the touch choices and the priming added 1.7 to 4.1 KB of
wasm a demo.

## Explanation

### What a browser can and cannot claim

A browser gives a page the time it began each frame (the
`requestAnimationFrame` timestamp), a clock, and input events with their own
times. It does not say when the display showed a frame, how long its
compositor took, whether the display scaled or converted the colors, or
what else ran on the page's thread. So a web record's onset is a lower
bound: the earliest vsync at which the frame can have shown. The tier is
`YSCR_TIER_WEB`, below tier 3 and below the simulated display, so
`desc.min_tier` flags every web flip and an analysis can never mistake one
for a rig's.

What a page can do is measure and report. The demos log the measured
period, isolation, the clock step, dropped frames as the callbacks show
them, and visibility changes. An experiment on participants' own machines
writes its exclusion rule (for example, more than 2 % of frames dropped, or
a hidden page during a trial) before data collection, and applies it to
those logs.

### Pacing a blocking loop

The examples are written as `for (;;) { yscr_begin(); draw(); yscr_flip(); }`.
A browser's main thread cannot block: a wait stops rendering and input. So
`yscr_begin()` suspends the program until the next frame callback
(`EM_ASYNC_JS` around a `requestAnimationFrame` promise). The program
resumes inside that callback's task, draws, and the browser composites the
drawing with that frame.

Two findings in headless Chrome 154 shaped the rule:

- A frame drawn in a task that ran past a vsync is composited after that
  vsync, but the next callback carries the time of the vsync it missed. The
  first records after a 17 ms setup showed onsets 1.3 ms before their own
  present call. The onset is now the first callback time after both the
  callback and the present call (`yscr__web_shown()`).
- A frame drawn in a callback that ran long after its time (the program set
  up for 17 ms after `yscr_open()` returned) was replaced by the next
  callback's draw before the browser composited it. `yscr_begin()` now
  skips a callback that ran more than half a period ago.

Before these two rules, 2 to 4 runs in 8 had `EARLY` records (1 to 599 of
600); once one frame was planned a vblank late, the core's rule "never the
vblank the last frame was planned for" kept every later frame a vblank
ahead. In a browser the next frame is now planned from `yscr_begin()`'s own
prediction. After the three changes, 0 of 8 runs had an `EARLY` record.

### The context: high-performance, not desynchronized

docs/rig_spec.md (section 6, web backend settings) asks for a
desynchronized WebGL context and the high-performance power preference.
The header takes the power preference. It leaves `desynchronized` off by
default: that hint lets the browser bypass its compositor and draw to the
screen directly, which can tear, and the onset rule above assumes a frame
the compositor shows whole at a vsync. Nothing here measured either case.
A page can ask for it with `Module.yscrDesynchronized = true`; the describe
line then says `desync=1`. A photodiode run decides it.

### ASYNCIFY, not JSPI

Both suspend the program at the await. Measured on the self-test (emsdk
6.0.12, Chrome 154 headed on the laptop of [status](status.md), 60 Hz panel,
isolated page, 3 interleaved rounds of 600 frames each, under the timing
guard):

| | ASYNCIFY | JSPI |
|---|---|---|
| wasm (selftest) | 1,071,703 B (gzip 321,882) | 779,439 B (gzip 245,472) |
| callback to program, p50 | 50 to 55 us | 35 us |
| callback to program, p99 | 600 to 615 us | 460 to 595 us |
| program time per frame, p50 | 35 us | 35 to 40 us |
| dropped frames | 0 of 1,800 | 0 of 1,800 |

JSPI saves 27 % of the wasm and about 15 to 20 us per frame. Chrome 154 and
Firefox 157 have it (`WebAssembly.Suspending` exists); Safari was not
checked. ASYNCIFY runs in every WebGL2 browser, and 20 us is 0.1 % of a
16.7 ms frame, so the demos use ASYNCIFY. A site that serves only browsers
with JSPI can build with `-sJSPI` in place of `-sASYNCIFY`.

### Threads only where sound needs them

The demos without sound build with `YRT_NO_THREADS`: they use no worker,
and one build runs isolated (with the finer clock) and not. A threaded
build needs `SharedArrayBuffer`, so it runs only on an isolated page and
needs a second build as the fallback. The sound demo has both:
miniaudio's AudioWorklet path needs `SharedArrayBuffer`, and its
ScriptProcessorNode path runs anywhere. `ysp-web.js` picks the build by
`crossOriginIsolated` before it loads one. The page's status line says
which.

### Isolation and the service worker

A browser coarsens `performance.now()` on a page that is not cross-origin
isolated. Measured on this laptop with the self-test, the smallest step two
reads show:

| Browser | Not isolated | Isolated |
|---|---|---|
| Chrome 154 (headless and headed) | 100 us | 5 us |
| Edge 155 (headless) | not run | 5 us |
| Firefox 154 to 157 (headless) | 1 ms | 20 us |

Every frame time, onset and key stamp has that resolution. In Firefox, not
isolated, the callback intervals ran from 3.0 to 33.3 ms (p1 12.7, p99
21.7) against 15.0 to 18.1 ms isolated (headless, 600 frames each).

GitHub Pages sets no headers, so the site loads `coi-serviceworker.js`
(gzuidhof/coi-serviceworker, MIT, fetched at a pinned commit and checked by
SHA-256; its license is beside it). The service worker adds COOP and COEP
to every response from the site. Measured: with a server that sends no
headers, Chrome 154, Edge 155 and Firefox 154 pages were isolated after the
service worker's reload, with the same clock step as with real headers.
Limits:

- The first visit reloads the page once, when the service worker takes
  control.
- A browser without service workers (Firefox private windows, some
  managed profiles) runs the pages without isolation; the status line and
  the data say `coi=0`.
- Every resource must come from the site's own origin or send CORP
  headers. The demos load nothing from elsewhere.

### The page log stays off the frame path

Each `printf` of a demo reaches the page through `Module.print`. The first
log rewrote the whole box and read `scrollHeight` on every line: a forced
layout inside the frame, longer as the log grew. Measured with the four
demos in Chrome 154 (headless and headed, isolated, keys pressed by
script, the page's own `requestAnimationFrame` times during the run):

| | One print, max | All prints of a run | Intervals over 1.5 periods |
|---|---|---|---|
| Before | 3.4 to 7.6 ms | 5 to 390 ms (gallery: 167 lines) | 1 to 6 per run |
| After | 0.04 to 0.19 ms | 0.06 to 1.2 ms | 1 to 3 per run |

Now lines wait in a buffer and reach the box at most every 300 ms from
an idle callback, as one appended text node; the box keeps the newest 400
lines, shows the newest by `flex-direction: column-reverse` with no scroll
position read, and has `contain: strict`. The intervals over 1.5 periods
that remain come with the programs' own work, not with the log (next
section). No demo prints once per frame: the gallery prints a page's tile
list when the page changes, the trials one line per trial.

### Priming before the page shows the canvas

The intervals over 1.5 periods left after the log fix were named "a
trial's first frame, a gallery page's setup". The page's own
`requestAnimationFrame` times with each printed line beside them (the
same runs as above: 3 runs a demo, keys by script, isolated, Chrome 154)
place them: in headless Chrome 10 of 11 (9 runs), and in headed Chrome 9
of 14, came 50 to 400 ms after the screen's describe line, so during
setup after `yscr_open()` (making the stimuli, `ygfx_prime()`'s wait for
the GPU) and before the first frame of the loop, while the canvas showed
nothing. The gallery called no `ygfx_prime()`: in 1 of 3 headless runs
the page change that first drew new kinds took a 33 ms frame. The rest,
headed only, came during trials (tutorial: 3 in one run; dot motion: 2 in
one run).

Now each program makes and primes its stimuli (the gallery all eight
pages', the dot motion demo also the dots' first buffer upload), then calls
`web_ready()`; the page keeps the canvas hidden behind "Preparing..." until
then. Counted from the frame the page got the signal (the stage shown):

| Intervals over 1.5 periods, per run | Tutorial | Dot motion | Gallery |
|---|---|---|---|
| Before, headless, from the click | 1, 1, 2 (all setup) | 1, 1, 1 (all setup) | 2, 1, 1 (setup, one page change) |
| After, headless, from ready | 0, 0, 0 | 0, 0, 0 | 0, 0, 0 |
| Before, headed, from the click | 1, 4, 1 | 1, 3, 1 | 1, 1, 1 |
| After, headed, from ready | 0, 0, 1 | 1, 0, 0 | 0, 0, 2 |

The headed "after" ones: one 33 ms interval on the tutorial's first frame
after the signal, one 67 ms frame in a dot motion trial, and two of 32 and
29 ms in the gallery 26 s into a run with no page change; nothing in the
program's own work ran then, so they are not setup. The "after" runs drew
a larger canvas (the stage: 1280 x 860 CSS pixels) than the "before" runs
(720 x 480). Setup still blocks the page as long as before; it now happens
before the participant sees the canvas.

### Audio: what the records say

A browser reports three things about its audio output: the context's
clock (`currentTime`, frames rendered), `getOutputTimestamp()` (the
context time of the frame it says is at the output now, and the
`performance.now()` time of that), and `outputLatency` and `baseLatency`.
None of them is an observation of sound. `getOutputTimestamp()` is the
render position less the browser's own figure for the delay to the
speaker, which comes from the operating system's audio API and covers
neither a Bluetooth link's delay nor the device's own.

`ysp/audio.h` treats each `getOutputTimestamp()` as a WASAPI position
report: the stream frame at the output at a time, fitted against the
ysp_rt clock, which `performance.now()` maps to by a correlation at open.
A sound's onset is the fit's time for its first frame. On the AudioWorklet
path the stream's frame 0 is a known context frame (`currentFrame`); on
the ScriptProcessorNode path it is estimated from `playbackTime`, which
Chrome 154 gives as `currentTime` plus the buffer when the event runs, so
it is late by up to the audio device's chunk (10 ms here) or by however
long the main thread made the event wait. The tier is `YAU_TIER_WEB` (5),
confirmed or not: the browser's estimate, as a web flip record is.

So "sound minus light" on `av.html` is what the two headers believe, both
from the browser: the flip's onset (a frame callback's vsync, before the
compositor) and the click's (the browser's output figure). Its spread
says how well the two plans agree, not when light and sound arrived.
A photodiode and a microphone on an oscilloscope measure that.

Measured on the laptop of [status](status.md) (Windows 11, Realtek
speakers, WASAPI shared at 48 kHz under the browser), Chrome 154 headed,
`av.html`, 20 trials a run, 3 runs a path interleaved, in one session
under the timing guard (load 0 to 19 % at the start):

| | AudioWorklet (isolated) | ScriptProcessorNode, 2048 frames (not isolated) |
|---|---|---|
| Sound minus light, mean per run | -0.01 to +0.07 ms | -0.03 to +0.12 ms |
| Sound minus light, SD per run | 0.06 to 0.10 ms | 0.08 to 0.10 ms |
| Sound minus light, all trials | -0.26 to +0.25 ms | -0.25 to +0.28 ms |
| Render to onset (the browser's figure), median | 52 to 55 ms | 111 to 127 ms |
| `outputLatency`, `baseLatency` | 48 ms, 10 ms | 40 ms, 10 ms |
| Lead at the end (`yau_lead_ns()`) | 56 to 60 ms | 137 to 168 ms |
| Callbacks per run | 11,772 to 11,937, 128 frames each | 711 to 713 |
| Callback rhythm | bursts every 10 ms: gaps of 2 ms or more p50 10, max 11 ms; within a burst 0 or 1 ms (the worklet's clock has 1 ms steps) | p50 40 ms, p99 50 ms, max 52 ms |
| Fit of the reports: residual RMS, drift | 32 and 38 us, +3.1 and +4.8 ppm (2 runs); one run restarted its fit 8 times (7 skipped blocks) | 49 to 323 us, +4.2 to +13.0 ppm |
| Skipped blocks found (`block_moves`) | 0, 7, 0 | 0, 0, 0 |
| Trials with a late or dropped flash, a flagged sound | 0, 1 | 0, 0 |

Earlier the same session's ScriptProcessorNode runs had one callback gap
of 2.2 to 2.7 s each and 51 to 63 skipped blocks: the program opens the
sound first (in the Start tap), then the screen's settling and the shader
compiles hold the main thread for 2 s. The events that waited then ran
back to back, and their `playbackTime` no longer said where their blocks
played. In Firefox 157 the same stall put every onset 7.7 s off. The
ScriptProcessorNode build of `av_sync.c` now closes the sound and opens it
again after its setup; the table is from that build. On the AudioWorklet
path the sound runs on its own thread and the stall did not touch it.

The ScriptProcessorNode's period, 2 runs each after the fix: 1024 frames
gave sound minus light -0.19 to +0.17 ms, no skipped block, a lead of 97
to 110 ms; 512 frames gave 184 and 185 callbacks over 1.5 periods late,
26 and 28 underruns found, and onsets up to 5.6 ms off their plan. The
page asks for 2048, the margin against the frame loop's own work on the
same thread; 1024 also held here.

Firefox 157 headed, the same pages started by script (its own profile,
autoplay allowed): one AudioWorklet run in the guarded session, sound
minus light -1.27 to +0.03 ms (SD 0.32 ms), the browser's render to onset
28 ms (`outputLatency` 32 ms, `baseLatency` 0), residual RMS 517 us; two
more runs of that session never showed the page in front, so its frame
callbacks did not run. Unguarded checks (not timing claims): an
AudioWorklet run with SD 0.41 ms, and a ScriptProcessorNode run with a
mean of -1.9 ms (SD 0.32 ms) and no skipped block.

`getOutputTimestamp()` itself, read every 2 ms for 6 s:

| | Chrome 154 headed | Firefox 157 headed |
|---|---|---|
| A new pair every | 10.0 ms (the device's chunk) | 10.7 ms (512 frames) |
| `performanceTime` against a line through `contextTime`, p1 to p99 | -0.15 to +0.14 ms | -3.0 to +3.1 ms |
| The read minus `performanceTime` | 2 ms | 39 ms: `outputLatency` |
| `contextTime` | the frame at the output | `currentTime` less `outputLatency` |

Headless Chrome's pairs came every 10 ms too, but one was 34 ms off and
the first was 1 s old; the reader drops pairs more than 100 ms old.
Firefox's `contextTime` steps with its callback while `performanceTime`
follows the read, hence the spread; the reader keeps the lower envelope (a
pair at most 0.5 ms later than the least late of the last 32). Firefox
also stamps the pair `outputLatency` before the read, though by its own
`contextTime` that frame is at the output at the read; the reader adds
`outputLatency` back where it sees that. Both are readings of one
browser's behavior, not of sound.

### Fullscreen

The page asks for fullscreen on the stage (the canvas's parent) in the
Start tap, then runs the program. Before v0.7.2 the sound demo left
fullscreen soon after it entered it. A listener on `fullscreenchange` and a
wrapper on `document.exitFullscreen` that logged the caller's stack, in
headed Chrome 154 and Firefox 157, showed the cause in every run (Chrome:
not isolated and isolated; Firefox: isolated):

```
_emscripten_exit_fullscreen
Emscripten_SetWindowFullscreen
SDL_UpdateFullscreenMode
SDL_SetWindowFullscreen
ApplyWindowFlags
SDL_SendWindowEvent
SDL_ShowWindow
```

SDL 3.4.2's Emscripten port applies the window's flags when it shows a
new window, and its driver calls `emscripten_exit_fullscreen()` when the
document is fullscreen and the window is not flagged fullscreen. In the
other demos, `yscr_open()` makes the window inside the Start tap, before
the browser enters fullscreen, so the driver saw no fullscreen. The sound
demo opens its sound first, and the sound's open waits for callbacks
(ASYNCIFY), which lets the browser finish entering fullscreen before the
window exists. Fullscreen ended 2.2 to 3.7 s after the tap, when the
screen opened. The same call came from SDL's `visibilitychange` handler in
every demo: with a hidden state faked by script during a fullscreen run of
the tutorial, the page left fullscreen when it was shown again.

`ysp/screen.h` v0.7.2 makes `document.exitFullscreen` a no-op while SDL
makes and destroys the window, and removes SDL's `visibilitychange`
handler (BACKENDS, WEB, in the header). Checked after the change, headed,
with Fullscreen checked, on this laptop:

| Browser, page | Isolated | Not isolated |
|---|---|---|
| Chrome 154, `av.html` (both builds) | fullscreen 33 s, to the end; 20 trials | fullscreen 35 to 36 s, to the end; 20 trials (2 runs) |
| Chrome 154, tutorial, dot motion, gallery, self-test | fullscreen for the whole run (14 to 16 s, or the self-test's end) | the same |
| Firefox 157, `av.html` | fullscreen 33 to 34 s, to the end (2 runs, one after the service worker's reload) | fullscreen to the end in 4 of 4 runs |
| Firefox 157, the other four | the same as Chrome | the same as Chrome |

The faked hidden state no longer ends fullscreen. In each run the only
exit was the page's own, in `ysp.finish()`.

The browser still leaves fullscreen by its own rules: Esc, a switch of tab,
and in Firefox a switch of window. A page may ask again only from a user
gesture, so `ysp-web.js` shows "Fullscreen ended. Press a key or tap to go
back to fullscreen." over the top of the stage and asks again on the next
key down, mouse press or touch release. That key or tap stops at the page:
the program does not see it as a response. Checked by an exit from script
in Chrome 154 (a key, a mouse press, a tap in touch emulation) and Firefox
157 (a key): fullscreen came back, the program's input log did not grow,
and the next key reached the program. An Esc sent by script does not end
fullscreen in Chrome, so the real Esc was not checked.

Some runs of the ScriptProcessorNode build opened no sound ("gave no
steady callbacks within 3 s"): in Firefox 157, 2 of 6 interleaved runs, one
with fullscreen and one without, and one more run in each browser.
Fullscreen is not the cause; the cause is not known.

### Input: touch, the mouse and the DOM's time

SDL's Emscripten port stamps an event in its own handler, which runs when
the page's main thread gets to it: after the frame the program is drawing,
or after a stall. The DOM event's `timeStamp` is when the browser made the
event, for input the time the operating system gave it to the browser,
on the `performance.now()` clock. `yscr_event_input()` now takes that
time. A capture listener on the window records each keydown, keyup and
pointer event with the time its listener ran; SDL's handler for the same
event runs in the same task right after it, so the newest record of the
event's class whose listener ran just before SDL's stamp is the match.

Touch on the canvas reaches SDL as pointer events, and `ysp/input.h`
turns them into `YIN_KIND_TOUCH` presses and releases; the demos make a
press on the left or right half of the canvas a choice, so a tap, a click
and a key go through one `ysp/response.h` collector and the data file
says which (`rsp_kind`). Stamp delays are in the
[measurements](#measurements). What is not known is the delay from the
finger to the browser's event: the touch panel's scan and the operating
system's processing, often 10 to 50 ms on phones by others' reports and
never measured here.

iOS specifics the pages rely on: an AudioContext starts only from a user
gesture (the Start tap, which runs the program's audio open inside it;
miniaudio resumes a suspended context on the next tap); iPhone Safari has
no element fullscreen (iPad Safari has `webkitRequestFullscreen`), so the
stage covers the viewport less the safe areas instead; `touch-action:
none` and the `gesturestart` handler stop pinch zoom during a run, and the
viewport meta has `viewport-fit=cover` for `env(safe-area-inset-*)`.
None of these was checked on an iOS device here.

### Measurements

All on the laptop of [status](status.md) (Windows 11, Intel Iris Xe,
internal panel at 60 Hz), on AC.

- Chrome 154, headed, isolated, the self-test, 5 runs of 600 frames in two
  sessions: callback intervals mean 16.666 ms, SD 47 to 99 us, p1 16.51
  to 16.55 ms, p99 16.78 to 16.84 ms, max 16.83 to 17.58 ms; no dropped
  frame; period measured at open 16.668 to 16.673 ms.
- Chrome 154, headed, not isolated, 2 runs of 600 frames interleaved with
  2 isolated ones: SD 90 to 100 us, p1 16.4 to 16.5 ms, p99 16.8 to 16.9 ms
  (the 100 us clock step shows in every number); no dropped frame. The
  isolated runs between them: SD 64 to 99 us.
- Chrome 154, headed, isolated, 20 ms of busy time per frame (more than a
  period): the callbacks came 16.7 or 33.4 ms apart (mean 23.2 ms, p50
  16.72, p99 33.44). The records show it as the program's lateness
  (`LATE_TARGET`: 202 of 600 in a headless run), not as frames the browser
  dropped (0). The self-test reports both.
- Headless Chrome 154 (no panel: its frame source is a timer), 600 frames:
  mean 16.667 ms, SD 63 to 70 us; settling took 0.36 to 0.92 s.
- Input stamps (v0.7.1), the self-test, headless Chrome 154, isolated,
  under the timing guard, events sent by script. 30 key taps (60 events)
  on the desktop page: every one got the DOM's time (`dom_stamps=60/60`);
  SDL's stamp was 0.53 ms after the DOM's at the median and 352 ms after
  it at most (a key pressed while setup held the main thread). 29 taps in
  Pixel 7 emulation: `dom_stamps=29/29`; SDL's stamp 11.2 ms after the
  DOM's at the median, 18.0 ms at most. That difference is what the DOM's
  time removes from each response; the delay before the DOM's time
  remains.
- Touch path in Pixel 7 emulation (412 x 839 CSS pixels, device pixel
  ratio 2.625): taps on the canvas halves gave `left` and `right`
  responses with `rsp_kind` = `touch` in the tutorial's CSV and turned the
  gallery's pages both ways; a turn to 839 x 412 during a run scaled the
  canvas's box to 202 x 412, aspect kept, and counted one viewport change.
- Target labels in Chrome 154's device emulation (headless, Fullscreen
  off, safe-area insets set by `Emulation.setSafeAreaInsetsOverride`:
  iPhone 14 Pro 59 top and 34 bottom in portrait, 59 left and right and 21
  bottom in landscape). Distance from the bottom of the label text to the
  visible bottom edge, in CSS px, tutorial and dot motion (the gallery in
  landscape is the same):

  | Device | Before | After |
  |---|---|---|
  | Pixel 7, portrait 412 x 839 | 8 (text 16 px) | 21 (text 19 px) |
  | Pixel 7, landscape 863 x 360 | 8 | 18 (16 px) |
  | iPhone SE, portrait 320 x 568 | 8 | 16 (14 px) |
  | iPhone SE, landscape 568 x 320 | 8 | 16 (14 px) |
  | iPhone 14 Pro, portrait 393 x 660 | 42: 8 above the safe area | 54: 20 above it (18 px) |
  | iPhone 14 Pro, landscape 734 x 343 | 29: 8 above the safe area | 37: 16 above it (14 px) |

  Before, the labels sat at the stage's bottom whatever the canvas did:
  in portrait they were 150 to 280 px under the gallery's canvas, and in
  landscape they spanned the stage, not the canvas. After, they sit 28 to
  31 px under the gallery's canvas in portrait, as wide as it. With the viewport made 56 px shorter during a run (a
  toolbar), the stage, the canvas and the labels followed: 13 to 52 px
  from the new edge. Emulation has no browser toolbar, so the visual
  viewport against `100vh` on a real phone is not measured.

### Not measured

- When light appears. No photodiode has watched a web demo; the
  compositor's delay after the onset in the record is unknown on every
  browser here.
- When sound appears. No microphone or line-in has heard `av.html`; the
  browser's `outputLatency` is in every audio onset and was not checked.
- Capture. `ysp/audio.h` 0.5.0's input compiles under emcc (alone and
  duplex, both builds of `av_sync`) and has `YAU_TIER_WEB`, but no page
  has opened one. On the AudioWorklet path its callback times would come
  from `yrt_now_ns()`, which reads 0 in that scope.
- A desynchronized context, Safari, a real phone or tablet (only Chrome's
  device emulation ran), a display above 60 Hz, a second display.
- Key and touch latency from a physical key or finger. The self-test
  reports SDL's stamp against the DOM event's `timeStamp`; the runs here
  pressed keys and tapped by script only.
