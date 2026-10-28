# The queue-ahead probe

A prototype, not a change to `ysp/screen.h`. It answers whether a host
can submit K frames ahead on COMPOSITION, each with its own target
vblank, and then sleep. The results are in
[docs/queue_probe.md](../../../docs/queue_probe.md). Windows 11, x64,
MSVC only.

| File | What it is |
|---|---|
| `include/ysp/screen.h` | A copy of `ysp/screen.h` 0.10.3 with a queue mode on COMPOSITION. The build puts this directory before the repository's `include/`. |
| `probe_queue.c` | A DLL with the probe's calls (`pq_*`), for `ctypes`. |
| `run_queue.py` | The Python driver: one mode per run. The host that sleeps is the Python interpreter. |
| `build.bat` | Builds `probe_queue.dll` with MSVC against SDL3's DLL and copies `SDL3.dll` beside it. |

## What the copy adds

- `yscr_queue_reserve(s, n)`: up to n presents in flight (1 to 28; 1
  is 0.10.3), with n + 2 buffers and one black buffer. Call it between
  frames on the frame thread. It refuses n > 28, because Microsoft
  documents at most 31 presentation buffers per presentation manager,
  and two runs that registered 62 to 122 buffers stopped this laptop
  with bugcheck 0x133 (DPC_WATCHDOG_VIOLATION). `begin()` then waits only while n presents are in flight, so
  n pairs of `begin()` and `flip_at()` return without a wait.
- `yscr_queue_cancel(s, blank)`: from any thread, cancel every present
  not yet shown (`IPresentationManager::CancelPresentsFrom` from the
  oldest present in flight). `blank` 1 then presents a black buffer for
  the next vblank.
- `yscr_queue_cancel_on(bits)`: 1 cancels at each abort, on the thread
  that saw it (the window thread for SDL's key, the watchdog's hook
  thread, the caller of `yscr_request_abort()`); 2 cancels at the
  panic's release, on the window thread; 4 adds the black buffer.
- The core's limits for one frame in flight become limits for a queue:
  `YSCR__MAX_PEND` 160, the presenter's id map and done list 256, and
  `yscr__drain()` reads every completion, not 4. `wait_flip()` waits
  until the last planned vblank in flight.
- `YSCR_QUEUE_DEBUG=1` in the environment prints each present and each
  statistic to stderr.

Everything else is 0.10.3. With no `yscr_queue_reserve()` call the copy
runs as 0.10.3 does.

## Build

```sh
cmd //c "tests\probe\queue\build.bat" C:\sdk\SDL3-3.4.0 BIN
```

`SDL3_ROOT` is the SDL3 3.4.0 VC development package. ANGLE's
`libEGL.dll` and `libGLESv2.dll` come from `--angle DIR` (default: the
Electron build in Docker Desktop's `frontend` folder, ANGLE
2.1.23876).

## Run

Each run goes through the timing guard:

```sh
bash /c/tmp/psy-guard.sh time NAME uv run --no-project python tests/probe/queue/run_queue.py MODE BIN [options]
```

| Mode | What it does |
|---|---|
| `smoke` | 4 frames queued, records printed. |
| `land` | One held frame (0 to 4 vblanks ahead, one in flight) against queues of K = 2 to 28 frames, `--reps` each. `--prime N` presents N frames one in flight after each reserve. |
| `far` | K = 4 frames whose first target is `--leads` vblanks ahead. |
| `cancel` | 28 frames queued; Shift+Esc from a helper thread after `--delay-ms`; the abort cancels the queue. `--blank` adds the black buffer. `--request` uses `yscr_request_abort()` when the window is not in front. |
| `panic` | 28 frames queued, 3 presses of Shift+Esc 120 ms apart, panic armed with a 1 ms grace; `--bits` picks where the cancel runs. |
| `sleep` | An RSVP of 28 queued frames, then `time.sleep(--sleeps)`, a watcher for a hung window, then 120 frames one in flight. |
| `cost` | Memory of 8, 16 and 31 displayable buffers in RGBA8 and RGBA16F, CPU per frame one in flight and queued. |

Common options: `--windowed` (1280 x 800), `--json OUT` (every record).
Before each reserve the driver prints the buffer memory it adds and the
DXGI budget, and refuses more than a quarter of the budget. Run Python
with `-u` and send the output to a file, so a crash leaves a partial log.
