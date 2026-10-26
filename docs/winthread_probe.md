# Window-thread probe, 2026-10-26

This page is reference and explanation. It records a probe that asked
whether the window of `ysp/screen.h` can belong to a thread of the
header's own, and what that costs. The question comes from the ysp-av
host probe (`docs/av_probe.md` on branch `probe-vis`): when a host idles
between trials (Python `time.sleep()` or `input()`, Octave `pause`, C
`Sleep()`), the frame thread pumps no window messages. After 4.1 to
4.6 s Windows marks the window hung, a ghost window takes the
foreground, and the first 2 to 16 frames after the gap go wrong.

The prototype is in [tests/probe/winthread/](../tests/probe/winthread/)
(build and run steps in its README). It is a copy of `ysp/screen.h`
0.9.0 with a window thread, switched on by `YSCR_WINDOW_THREAD=1` in the
environment, so one build runs both designs and every comparison below
is the same binary. `include/ysp/screen.h` is not changed on this
branch. The raw logs and CSV files are in the worker's scratch folder;
the numbers below are the record.

@@SUMMARY@@

## 1. Design

### What the window thread owns

One thread per process, started by the first `yscr_open()` that makes a
window and stopped by the last `yscr_close()`. SDL has one "main
thread", the thread that starts its video subsystem (SDL 3.4.0
`src/SDL.c`, `SDL_InitSubSystem`: "We initialize video on the main
thread. On Apple platforms this is a requirement. On other platforms,
this is the definition."). SDL's video calls are documented "should
only be called on the main thread", and `SDL_PumpEvents()` asserts it in
debug builds (`src/events/SDL_events.c`, `SDL_PumpEventsInternal`). So
the window thread starts and stops SDL's video subsystem, and every SDL
video call goes to it. Several screens share it.

| Moves to the window thread | Stays on the frame thread (the host's) | Any thread, unchanged |
|---|---|---|
| `SDL_InitSubSystem(VIDEO)` and its quit; gamepad init, open, close and quit | D3D11 device and context, DXGI factory | `SDL_PushEvent` (`yscr_push_input`), `SDL_PeepEvents` |
| `SDL_CreateWindowWithProperties`, `SDL_SetWindowFullscreenMode`, `SDL_SyncWindow`, `SDL_DestroyWindow` | DXGI_FLIP: `CreateSwapChainForHwnd` on the window thread's HWND, `MakeWindowAssociation`, `Present1`, frame statistics, the latency waitable | `SDL_AddEventWatch` and the abort watch (it runs on the thread that queues the event) |
| `SDL_SetWindowIcon` and `WM_SETICON`, `SDL_RaiseWindow` with the force hint (FOREGROUND) | COMPOSITION: the DComp device, `CreateTargetForHwnd`, the visual, `IPresentationManager`, its textures and statistics | `SDL_GetWindowProperties`, `SDL_GetWindowSizeInPixels`, `SDL_GetTicksNS` (reads) |
| `SDL_HideCursor`, `SDL_ShowCursor`, `SDL_StopTextInput`, `SDL_StartTextInput`, `SDL_SetTextInputArea` | ANGLE: EGL display, context, client-buffer pbuffers, all GL | the panic watchdog (its own thread and hook), the raw-mice reader (its own message-only window), the idle-wake waiter, the trigger worker |
| the device lists at open (`SDL_GetKeyboards`, `SDL_GetMice`, ...) | `begin()`, `flip_at()`, `wait_flip()`, settling, records, the grid | SDL's raw keyboard thread (keys, stamps) |
| the message loop: `SDL_PumpEvents()` | `yscr_poll()` takes events with `SDL_PeepEvents` (no pump) | the gamma ramp (GDI, by display name) |

The window thread's loop waits in `MsgWaitForMultipleObjectsEx` on its
command event and on any message for its windows (`QS_ALLINPUT`), at most
100 ms (4 ms while gamepads are open, because SDL polls XInput in its
pump), then runs a waiting command and calls `SDL_PumpEvents()`. The
100 ms wake is a heartbeat: it tells a live thread from a hung one
(PANIC, section 4).

### Can DXGI and DirectComposition present from a thread that does not own the window?

Yes, on every path the header uses, measured (section 2): DXGI_FLIP on
the overlay and independent-flip paths, COMPOSITION on independent
flip, in a window and fullscreen. What the documentation says:

- DXGI ("DXGI overview", Multithread considerations): "Be careful that
  you never have the message-pump thread wait on the render thread when
  you use full-screen swap chains. [...] calling Present1 (from the
  render thread) may cause the render thread to wait on the
  message-pump thread. When a mode change occurs, this scenario is
  possible if Present1 calls ::SetWindowPos() or ::SetWindowStyle()".
  The header never makes an exclusive full-screen swapchain (fullscreen
  is a borderless window), and in the design the window thread never
  waits on the frame thread. The frame thread waits on the window thread
  only for a command (open, close, text input, gamepads), never in a
  frame.
- DirectComposition (`IDCompositionDevice::CreateTargetForHwnd`): "the
  window must belong to the calling process". Process, not thread.
- ANGLE renders to pbuffers (client buffers) only; it never touches the
  HWND.

### Input, abort and close

- **Keys** come from SDL's raw-input thread, which stamps them and queues
  them whatever any other thread does (SDL 3.4.0
  `src/video/windows/SDL_windowsrawinput.c`). That does not change.
- **Mouse, focus, close and window events** come from the window
  thread's pump, as soon as Windows posts them, not at the next
  `begin()`.
- **The frame thread** takes events with `SDL_PeepEvents(GETEVENT)`,
  which does not pump (`SDL_PollEvent` pumps, and pumping belongs to
  SDL's main thread). `begin()` no longer pumps.
- **Abort and close requests** need no change. The abort state is one
  lock-free log per process, written by SDL's event watch on the
  thread that queues the event (the raw-input thread for Shift+Esc, the
  window thread for a close request or Alt+F4) and by the watchdog's
  hook; `begin()` reads it.
- **`yscr_push_input()`** already runs on any thread.

### Open and close, in order

`yscr_open()` on the frame thread:

1. Check the desc (unchanged).
2. Start the window thread if this is the first window; wait until its
   message queue exists.
3. Window thread (one synchronous command): SDL hints, video init, the
   doorbell event type, gamepads, the display mode, the window, the mode
   switch, the icon, `SDL_SyncWindow`, `SDL_RaiseWindow`,
   `SDL_StopTextInput`, `SDL_HideCursor`, the mode obtained, the slot,
   the abort watch. A failure leaves the message in `s->error`.
4. Frame thread: the presenter's open (device, swapchain or composition
   target on the HWND, ANGLE).
5. Window thread: the foreground check and the forced raise; the device
   list.
6. Frame thread: raw mice, the display state and gamma, the panic
   watchdog, the context hook, the warm-up and settling. The window
   thread pumps the whole time.

`yscr_close()` on the frame thread: the last flip, the trigger worker,
the watchdog, the gamma, the waiter, the foreground given back, the abort
watch, then the presenter's close (the swapchain or composition target
goes before the window), then one command to the window thread (cursor,
`SDL_DestroyWindow`, icons, gamepads, video quit), then the window
thread stops if this was the last window.

Failure paths: every step of open that fails returns with the message
and calls `yscr_close()`, which undoes what exists, as in 0.9.0. A
command that fails on the window thread does not call `yscr_close()`
there (the close would wait on itself). `CreateThread` failing fails the
open with "window thread: CreateThread failed".

### The change as it would land

State: a process-wide struct (thread, id, command and done events, a
lock for one command at a time, the heartbeat, the release flags, the
user count) and one `int32_t` per screen. Functions:
`yscr__wt_acquire()`/`_release()`, `yscr__wt_run(fn, arg)` (synchronous
command), the loop, and two extracted functions,
`yscr__window_open()` and `yscr__window_close()`, which hold the window
code of 0.9.0's open and close unchanged. Calls rerouted: the
foreground, the device list, text input, gamepad hot-plug, the pump,
`yscr_poll()`.

Size, measured on the prototype: the copy differs from 0.9.0 by 437
added and 189 removed lines (`git diff -w`), of which about 185 are the
window code moved into the two functions, so about 250 new lines, 133
of them the thread itself. A version to merge needs also: a desc field
(`desc.window_thread`: AUTO, ON, OFF) in place of the environment
variable; `YRT_NO_THREADS` and the web build kept on the single-thread
path; `yscr_displays()` and `yscr_modes()` routed to the window thread
while it runs; a public `yscr_window_call(s, fn, ctx)` for code that
calls SDL on `yscr_window()` (section 5); a timeout on the command wait;
the manual (MEMORY AND THREADS, INPUT, ABORT, PANIC, FOREGROUND,
BACKENDS); `tests/adapt` and `tests/compile` updates and a mutation list
entry. Estimate: 400 to 500 lines of code and 150 lines of manual, one
header version (0.10.0).

@@MEASUREMENTS@@
