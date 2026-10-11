# The web backend and the web demos

`ysp/screen.h` v0.7.0 has a backend for the browser, `YSCR_BACKEND_WEB`.
A program written as a blocking frame loop compiles with Emscripten and
runs in a page: SDL3's Emscripten port gives the canvas and the input,
the header makes a WebGL2 context, and `requestAnimationFrame` paces the
frames. `examples/web/` has four demos and a static site that GitHub Pages
serves.

Browser timing is not rig timing. A browser does not report when the
display showed a frame, gives a page no control of the display, and runs
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
   at a pinned commit and checks its SHA-256, and compiles each demo to
   `web_site/<name>.js` and `web_site/<name>.wasm`. The first build also
   compiles SDL3's port (about 40 s).

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
   and call `Module.callMain(args)` from a button. The button's click is
   also the user gesture that fullscreen and Web Audio need.
5. Hand the data to the page from C with an `EM_JS` function, as
   `examples/web/tutorial.c` does: `ytr_data` writes the rows to a memory
   buffer through `desc.write`, and the page offers them as a download.

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
| Fullscreen | never requested by the header; `desc.windowed = false` sizes the canvas to the viewport at open |
| Hidden page | `yscr_begin()` waits until the page is shown; the frame in flight completes `OCCLUDED`; `YSCR_EV_VISIBILITY` records |
| Input | SDL's events, stamped when the page's handler ran, not with the DOM event's `timeStamp` |
| Threads | the demos build with `YRT_NO_THREADS`; triggers need `-pthread` and were not tested |
| Describe line | `raf_period`, `raf_settle`, `coi`, `clock_step`, `corr`, `desync`, `skipped`, `late`, `hidden`, `vis_changes`, `canvas` |

### The demos

| Page | Program | What it does |
|---|---|---|
| `tutorial.html` | `examples/web/tutorial.c` | A Gabor tilted left or right after a fixation cross; F or J; 10 trials; RT from the flip record; results and a CSV from `ytr_data` |
| `rdk.html` | `examples/web/rdk_staircase.c` | Random dot motion, left or right (`ysp/rdk.h`); a 2-down 1-up staircase on coherence (`ysp/stair.h`); the threshold at the end |
| `gallery.html` | `examples/gfx/gallery.c` | The `ysp/gfx.h` gallery, eight pages, compiled without a change |
| `selftest.html` | `examples/web/selftest.c` | The measured period, a histogram of callback intervals, the clock step, isolation, drops, and key latency against the DOM's `timeStamp` |

Each page shows, and each CSV or JSON result records, whether the page was
cross-origin isolated (`coi`), the clock step, and the audio path (`none`:
no demo plays sound). The three programs with `--sim` also build natively
(`web_tutorial`, `web_rdk_staircase`, `web_selftest`) and CI runs them on
the simulated display.

### Sizes

emsdk 6.0.12, `-O2`, ASYNCIFY, no threads. gzip -9 is what a server that
compresses sends.

| Demo | wasm | wasm, gzip | js | js, gzip |
|---|---|---|---|---|
| tutorial | 1,408,705 | 463,878 | 215,489 | 48,571 |
| rdk_staircase | 1,364,301 | 448,836 | 215,509 | 48,605 |
| gallery | 1,514,551 | 492,453 | 215,641 | 48,517 |
| selftest | 1,071,474 | 321,673 | 214,466 | 48,422 |

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

### No threads

The demos build with `YRT_NO_THREADS`. They use no worker: no trigger, no
pump, no sound. A threaded build needs `SharedArrayBuffer`, so it runs only
on an isolated page and needs a second build as the fallback; the isolation
gives the finer clock to the no-threads build too (measured below). A
program that needs threads (the pump for QUEST+ in the background, or
miniaudio's AudioWorklet path, which also needs `SharedArrayBuffer`) builds
with `-pthread`, runs only when `crossOriginIsolated` is true, and needs a
fallback build. No demo does that yet.

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

### Not measured

- When light appears. No photodiode has watched a web demo; the
  compositor's delay after the onset in the record is unknown on every
  browser here.
- A desynchronized context, Safari, mobile browsers, a display above 60 Hz,
  a second display.
- Key latency from a physical key. The self-test reports the program's
  stamp against the DOM event's `timeStamp`; the headless runs pressed keys
  by script only.
