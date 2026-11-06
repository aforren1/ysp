# ysp/input.h

Status: v0.6.0, 2026-11-06. The header's manual (its comment block) is the
reference for each field and constant. This page records why the header
exists and how it was checked.

## Why a header of its own

`ysp/response.h` v0.1.0 to v0.1.2 defined the input event. A producer, such
as a serial box reader, `ysp/screen.h`'s raw mice or a later eye-tracker
reader, then had to include the response collector to make events. A data
logger or the player's `input.key` had to include it to read them. The
coordinator split the event out on 2026-10-08, as the user approved:

| Part | Header |
|---|---|
| The 48-byte event, its kinds, types, flags, controls and axes | `ysp/input.h` |
| The source entry and the tier scale | `ysp/input.h` |
| The SDL3 adapter (`yin_from_sdl`, inline, when SDL3 came first) | `ysp/input.h` |
| The raw mouse adapter (`yin_from_mouse`, inline, when `ysp/screen.h` v0.3.5 came first) | `ysp/input.h` |
| The collector, the window, the result, key names, the cursor | `ysp/response.h` |

`ysp/response.h` requires `ysp/input.h` beside it and includes it, in the
same way that `ysp/gfx.h` requires `ysp/color.h`. Every `yrsp_` and
`YRSP_` name of the event stays as an alias (`yrsp_input` is
`yin_event`), so code written for `ysp/response.h` v0.1.2 builds
unchanged.

## v0.2.0

- The raw mouse report (`yin_mouse_report`, `YIN_MOUSE_*`) moved here
  from `ysp/screen.h`, so `yin_from_mouse()` needs no other header.
  `ysp/screen.h`'s `yscr_mouse_event` and `YSCR_MOUSE_*` are aliases.
- The SDL3 adapter sits outside the include guard, behind its own guard:
  a file that includes SDL3 after `ysp/input.h` includes `ysp/input.h`
  again and gets the adapter. `ysp/screen.h`'s implementation does this
  for `yscr_event_input()`.
- `ysp/screen.h` v0.4.0 requires `ysp/input.h`: its input bridge stores
  and delivers `yin_event`s.

## v0.3.0

For specialized devices (`docs/devices_spec.md`, section 7; the user's
defaults of 2026-10-08, section 16):

| Change | Where | Why |
|---|---|---|
| `YIN_KIND_SYNC` (7) | the kind | A scanner pulse, a TTL input, a photodiode or sound-key edge is a timing event, not a participant's response. Kind 7 was free. `ysp/response.h` v0.1.4 keeps it out of ALL mode |
| `uint16_t unc_us` | offset 26, the 2 bytes that were `reserved_` | This event's own uncertainty: a device clock fit's spread at the time, a bracket's width, a known quantizer. ioHub (`confidence_interval`) and Presentation (`unc_dms`) carry one per event; a source entry has one bound for a whole session. 0 = the source entry's; `YIN_UNC_MAX` (65535) = 65.5 ms or more |
| `uint32_t ticks` | offset 44, the 4 bytes of tail padding | The device clock's raw count, low 32 bits, valid with `YIN_DEVTICKS` (0x10). The analysis can map the event again with an offline fit of the clock pairs in the log (`ysp/rt.h` v0.6.0). 32 bits suffice because the pairs carry the whole count |
| `YIN_EYE_GAZE` (0, as `YIN_AXIS_POSITION`), `_FIXATION` (1), `_SACCADE` (2), `_BLINK` (3); `YIN_EYE_LEFT` (0), `_RIGHT` (1), `_BOTH` (2) in `code` | constants | The existing types carry eye events: a gaze SAMPLE, a blink as a press and release of the BLINK control, PROXIMITY for track lost and found |

The size stays 48 bytes and every older offset and value stays. A producer
built against v0.2.0 writes zeros in both places (the adapters `memset`
the event), which reads as "the source entry's bounds" and "no device
ticks".

## Decisions

| Question | Decision | Why |
|---|---|---|
| Prefix | `yin_`, `YIN_` | One prefix per header. The old names stay in `ysp/response.h` as aliases. |
| Implementation section | None | Types and inline functions only: nothing to link. |
| Flag names | `YIN_REPEAT`, `YIN_SYNTHETIC`, `YIN_ERASER`, `YIN_UNLISTED` | `YRSP_IN_REPEAT` read as "input repeat" in the collector; in the input header the `IN_` is redundant. |
| Values | Unchanged | Kinds, types, axes and tiers are in data files: their numbers are pinned by the test. |
| Key names (`yrsp_scancode`, `yrsp_key_name`) | Stay in `ysp/response.h` | They resolve choices; a producer does not need them. |
| The cursor (`yrsp_cursor`) | Stays in `ysp/response.h` | A consumer's integration of events, with the design's gain. |

## Bounds in ns, and why `unc_us` stays (v0.4.0)

v0.4.0 applies the repository conventions (README.md, Conventions): every
time in a C API is an `int64_t` count of nanoseconds, and a name says its
unit.

- **`yin_source`'s bounds are `int64_t lo_ns` and `hi_ns`.** They were
  `int32_t lo_us` and `hi_us`. The entry grows from 24 to 32 bytes. It is
  written once per source, so the size does not matter.
  `ysp/response.h`'s result copies them as `stamp_lo_ns` and
  `stamp_hi_ns`. `ysp/rigfile.h` now copies its bounds in ns, with no
  rounding to us.
- **`yin_event.unc_us` stays a `uint16_t` in us.** This is the one time
  in ysp that is not `int64_t` ns. The event is 48 bytes, and the only
  free bytes were the 2 bytes at offset 26 where `unc_us` sits. An
  `int64_t unc_ns` must go at the end. Measured with MinGW-w64 gcc 16.1
  (x64): the event becomes 56 bytes, and `ysp/screen.h`'s input slot (16
  bytes of state and the event) becomes 72 bytes, where it is 64 now,
  one cache line. The field's name says its unit, so code that reads it
  cannot take it for ns. The coordinator decides whether this exception
  stays.

The event's `t` keeps its name: it is `int64_t` ns already, and every
producer header writes it.

## macOS: NSEvent stamps (v0.5.0)

SDL 3.4 stamps a Cocoa key or mouse event with `NSEvent.timestamp` plus
a static offset (`Cocoa_GetEventTimestamp`,
`src/video/cocoa/SDL_cocoaevents.m`). The process's first such event sets
the offset to its own read delay; later events only lower it, when it
would put a stamp after the read. So every stamp is late by the shortest
read delay seen so far in the process: 3773 us on the first key, then
501, 316 and down to 207 us in the probe's run
([screen_macos_probe.md](screen_macos_probe.md), 6.9). The same function
stamps mouse buttons, motion and the wheel, touch, pinches and pens. A
modifier key (shift, control, option, command) has no stamp: SDL stamps
it when it reads it. `NSEvent.timestamp` has no offset: its clock is mach
time, the ysp_rt clock on macOS.

`ysp/screen.h` v0.15.0 reads `NSEvent.timestamp` for keys, modifier keys
and mouse buttons and gives it to this header's adapter (docs/screen.md,
"Keys and mouse buttons: NSEvent.timestamp"). The adapter takes it
through the context, as it takes `raw_keyboard`:

- `ctx.t_os`, when not 0, is the event's own OS time on the ysp_rt
  clock, and replaces `t_rt`.
- `ctx.os_stamps` says that the caller gives such times where it has
  them (`ysp/screen.h`'s `caps.os_input_stamps`). A key or mouse button
  without one then carries SDL's stamp, and gets tier 3, so a consumer
  sees which events kept the offset.

`yin_sdl_source()` on macOS:

| Kind | `ctx.os_stamps` | Tier | Bounds (stamp minus post) | From |
|---|---|---|---|---|
| keyboard, mouse | true | unknown (host part only) | 49.3 us to 46.591 ms | the run below; the maximum of three runs |
| keyboard, mouse | false | 3 | 262 us to 67.962 ms | SDL's own stamp: the probe's minimum, the maximum of the three runs below |
| touch, pen | either | 3 | none | SDL's source, not measured |

The bounds are the host part for synthetic events: they leave out the
keyboard's scan, debounce and USB poll, and a real key's path into the
window server.

Conditions: 2026-11-05 11:58, Apple M2 Mac mini, macOS 27.0.1, the M27Q
on HDMI at "60 Hz", AC, the session unlocked, no Screen Sharing, nothing
else running; Accessibility granted to the terminal's app. An 800 x 600
METAL window ran a frame loop that read its events once a frame with
`yscr_poll()` and `yscr_event_input()`, the path a rig uses. A thread
posted F19, the left button at the window's center, and (to the process
only) the left shift key, 40 to 50 ms apart at a random phase to the
frames, 200 of each per pass. "post" is the mach time just before the
post call. The test's own local NSEvent monitor gave the reference
NSEvent.timestamp. Times in us.

| Pass, kind | n | SDL's stamp kept | \|stamp - NSEvent\| max | stamp - post: min / p50 / p99 / max | SDL's stamp - post: min / p50 / p99 / max |
|---|---|---|---|---|---|
| `CGEventPostToPid`, key down | 200 | 0 | 0 | 68.8 / 113.7 / 3781 / 44567 | 585.5 / 782.4 / 6598 / 52827 |
| `CGEventPostToPid`, key up | 200 | 0 | 0 | 49.3 / 115.3 / 3139 / 15397 | 566.0 / 772.1 / 4311 / 23657 |
| `CGEventPostToPid`, left shift down | 200 | 0 | 0 | 68.1 / 113.6 / 2578 / 2803 | 546.0 / 8411 / 16940 / 17150 |
| `CGEventPostToPid`, left shift up | 200 | 0 | 0 | 50.7 / 112.3 / 2788 / 3211 | 499.5 / 8996 / 16837 / 17049 |
| `CGEventPostToPid`, left button down and up | 200 each | SDL delivered none | | | |
| `CGEventPost(kCGHIDEventTap)`, key down | 200 | 0 | 0 | 75.8 / 135.7 / 3724 / 3743 | 592.4 / 652.4 / 4240 / 4259 |
| `CGEventPost(kCGHIDEventTap)`, key up | 200 | 0 | 0 | 73.2 / 125.8 / 2933 / 3531 | 589.9 / 642.4 / 3450 / 4048 |
| `CGEventPost(kCGHIDEventTap)`, left button down | 200 | 0 | 0 | 74.4 / 136.0 / 2917 / 3582 | 591.0 / 652.6 / 3433 / 4099 |
| `CGEventPost(kCGHIDEventTap)`, left button up | 200 | 0 | 0 | 64.2 / 122.5 / 3194 / 3725 | 580.9 / 639.1 / 3711 / 4242 |

- The pairs are exact: the corrected stamp equals the test monitor's
  `NSEvent.timestamp` to the nanosecond (1600 of 1600; `os_stamps=1600/1600`
  in the describe line). The header and the test round the double to ns
  in different ways; no difference reached 1 ns.
- The corrected stamp came 49 to 136 us after the post at the median, and
  up to 3.8 ms at p99 in both passes. Two process-posted keys came 15 and
  44.6 ms after the post (the two runs before this one: 46.6 and 45.4 ms
  at most). The probe saw such a delay once on the HID path (40 ms, during
  a 26 ms post call). It is the event's own time, so the stamp keeps it.
- SDL's own stamps came 0.5 to 0.8 ms later at the median: its offset in
  this process (516 us at the end of the run). Its modifier keys came 8.4
  to 9.0 ms after the post at the median, up to 17 ms: the time of the
  frame's read, which is a frame long.
- SDL 3.4 delivered no mouse button posted with `CGEventPostToPid` (0 of
  200 down and up; both monitors saw all 200). These are events the
  pairing must pass over; the keys that followed them kept their exact
  stamps.

## Raw mice on macOS (v0.5.1)

`ysp/screen.h` v0.16.0 reads raw mice on macOS too, with an
`IOHIDManager` on a thread of its own, into the same `yin_mouse_report`.
Two fields mean other things there: `t` is the HID driver's stamp
(`IOHIDValueGetTimeStamp`), not the time the reader read the report,
and `device` is the low 32 bits of the device's IORegistry entry id, not
a Raw Input handle. On one Dell MS116 at 125 Hz the reader's callback
came 55 us after the stamp at the least, 122 us at the median, 266 us at
p99 and 486 us at most (1159 values, ysp/screen.h v0.16.1)
([screen.md, "Raw mice"](screen.md#raw-mice)). v0.5.1 fixes the comments
that said raw mice were Windows only, and `yin_raw_mouse_source()`'s
note on macOS now gives those numbers ("the HID driver's stamp; host
part to the read 55 us min, 122 us median, 266 us p99, 486 us max (one
Dell MS116, 125 Hz); USB poll not in it"); elsewhere it keeps Windows' (stamped at read, 0.15 to
0.40 ms, injected). The bounds stay unstated: one mouse on one machine.

## Drawing tablets (pens)

Reference for pen input: what arrives, how it is stamped, and what is
not verified. Status, 2026-11-06: no tablet has been tested with ysp.
Everything below comes from this header's adapter and from SDL 3.4.18's
source (`src/events/SDL_pen.c` and the video backends). Nothing here is
measured.

### The path

SDL 3 sends pen events. `yscr_poll()` delivers them, and
`yscr_event_input()` converts them with `yin_from_sdl()` to `yin_event`s
of kind `YIN_KIND_PEN`. `device` is SDL's pen id (`which`). SDL keeps
the id while the pen stays connected, and gives a new id in a new
process.

| SDL event | `type` | `control` | `x`, `y` | `value` | `flags` |
|---|---|---|---|---|---|
| `PEN_PROXIMITY_IN`, `_OUT` | `PROXIMITY` | 0 | 0 (SDL gives no position) | 1 in, 0 out | `YIN_ERASER` (SDL 3.4.16 or later) |
| `PEN_DOWN`, `PEN_UP` | `PRESS`, `RELEASE` | 0, the tip | window px | 1, 0 | `YIN_ERASER` |
| `PEN_BUTTON_DOWN`, `_UP` | `PRESS`, `RELEASE` | the barrel button, 1 to 5 | window px | 1, 0 | `YIN_ERASER` |
| `PEN_MOTION` | `SAMPLE` | `YIN_AXIS_POSITION` (0) | window px | 0 | `YIN_ERASER` |
| `PEN_AXIS` | `SAMPLE` | `YIN_AXIS_PEN` + `SDL_PenAxis`: 16 pressure, 17 x tilt, 18 y tilt, 19 distance, 20 rotation, 21 slider, 22 tangential pressure | window px, the pen's position | the axis: pressure 0 to 1, tilt -90 to 90 deg, rotation -180 to 180 deg | `YIN_ERASER` |

From v0.6.0 every pen event has SDL's `pen_state` (`SDL_PenInputFlags`:
`SDL_PEN_INPUT_DOWN`, `_BUTTON_1` to `_5`, `_ERASER_TIP`,
`_IN_PROXIMITY`) in `code`, and `YIN_ERASER` when its eraser bit is set.
A proximity event has them from SDL 3.4.16, which added `pen_state` to
it; built against older headers its `code` stays 0. SDL sets the eraser
bit when the eraser end touches and clears it when the tip touches, so
hover before the first touch is not flagged, and hover after an eraser
stroke is.

Rules for a consumer:

- Pressure and tilt are separate `SAMPLE` events, not fields of the
  position sample: tilt is in `value`, and `x`, `y` are the pen's
  position. In SDL 3.4 every backend (Windows, X11, Wayland,
  Cocoa) sends the motion of one report first and its axes after it. So
  give an axis value to the position sample before it.
- SDL sends a motion or an axis event only when the value changes.
- A position sample can come while the pen is in range but does not
  touch. Use `PRESS` and `RELEASE` with control 0 to know when the pen
  draws.
- `code & SDL_PEN_INPUT_DOWN` says on every sample whether the pen
  touches (v0.6.0). The adapter does not copy `device_type` (direct or
  indirect, SDL 3.4.18): it is the device's, the same on every event, so
  ask SDL for it once (`SDL_GetPenDeviceType(which)`).
- By default SDL also makes mouse events (`which` `SDL_PEN_MOUSEID`) and
  touch events (`SDL_PEN_TOUCHID`) from pen input. `yin_from_sdl()`
  drops them unless `ctx.keep_synthetic`. `yscr_event_input()` drops
  them unless the screen's `desc.keep_synthetic` (`ysp/screen.h`
  v0.18.0). So a program that reads only `YIN_KIND_MOUSE` gets nothing
  from a tablet by default. Read `YIN_KIND_PEN`, or set the flag and read
  the events flagged `YIN_SYNTHETIC`.
- `desc.raw_mice` does not read tablets on Linux (the evdev reader takes
  relative mice only). On Windows, Raw Input gives a tablet in mouse mode
  as `YIN_AXIS_ABSOLUTE` samples (0 to 1), with no pressure.
- With `desc.ring`, `ysp/screen.h` writes a `YSCR_EV_DEVICE` record
  (`YSCR_DEV_PEN`) when a pen comes into range for the first time.

### Stamps and tiers by platform

`yin_sdl_source(ctx, YIN_KIND_PEN)` gives the source entry. No pen
stamp is measured on any platform.

| Platform | SDL 3.4 backend | Stamp | Tier from `yin_sdl_source()` |
|---|---|---|---|
| Windows | Windows Ink pointer messages (`WM_POINTER`, `GetPointerPenInfo`); pressure is `pressure / 1024` | the window message's time, a ms tick | 3 |
| macOS | Cocoa tablet events | `NSEvent.timestamp` plus SDL's offset for the process (macOS: NSEvent stamps, above); `ysp/screen.h` does not correct pen events | 3 |
| Linux X11 | XInput 2; a device with an absolute pressure valuator is a pen | the time SDL queues the event (SDL passes 0, so `SDL_GetTicksNS()` at the queue), not the X server's time | 3 (v0.6.0; was 0): SDL's pump, as for its X11 buttons, which `ysp/screen.h` v0.18.0 measured with XTEST |
| Linux Wayland | `zwp_tablet_tool_v2` (tablet protocol, unstable v2) | the compositor's frame time, ms | 0; `ysp/screen.h`'s Wayland backend is a stub |

SDL 3 does not read Wintab. A tablet whose driver gives Wintab only, and
not Windows Ink, sends no pen events to SDL on Windows.

### Use it

- Pen down as a response: a `ysp/response.h` choice with `kind`
  `YRSP_KIND_PEN` and `control` 0 (a `PRESS`).
- Pressure onset: a `RISING` crossing on `YRSP_AXIS_PEN_PRESSURE`.
- A drawing trace: `desc.trace` with `trace_kinds`
  `YRSP_KIND_BIT(YRSP_KIND_PEN)`.

`examples/response/trial_copying.c` uses all three paths of input
(`--input mouse`, `raw`, `pen`). With `--input pen` the ink follows the
pen while it touches, and the sample file has the pressure of each
sample. `trial_copying --sim --input pen` sends SDL pen events made in
the program (in range, a hover, down, motion each followed by a pressure
axis, up, a hover) through `yin_from_sdl()`. It checks that hover input
makes no ink and that each sample has the pressure of the axis event
after it. It calls `yin_from_sdl()` itself, not `yscr_event_input()`:
that call takes its time from the screen's clock map, which a simulated
screen does not have (`yscr_restamp()` returns 0 there).
`ysp/screen.h` v0.18.0 adds `yscr_event_input_at()`, the same conversion
with the caller's time, for such events.

### Not verified

- Any tablet with ysp: events, positions, pressure range, tilt, the
  eraser, rates and stamps (docs/rig_spec.md, 15.6).
- The stamps in the table above. They are read from SDL's source.
- `pen_state` and `YIN_ERASER` from a real eraser: SDL's rule above is
  read in its source.

## v0.6.0: pens, Linux sources, raw mouse order

From the four task examples ([examples_findings.md](examples_findings.md),
B6, F14, D2, D6):

| Change | Why |
|---|---|
| A pen event's `code` is SDL's `pen_state`; `YIN_ERASER` on every pen event while the eraser end touched last | `trial_copying` needs to know on every sample whether the pen draws and with which end. The adapter dropped `pen_state` and set the eraser on `PRESS` and `RELEASE` only |
| `yin_raw_mouse_source()` on Linux: "the kernel's evdev stamp of the report; inside the uinput write() in 20 of 20 (injected); USB poll not in it" | It gave Windows' note ("stamped at read, 0.15 to 0.40 ms"). Tier unknown, partial, no bounds: one machine, injected input only, as on Windows and macOS |
| `yin_sdl_source()` on Linux | It returned tier 0, "not measured on this platform", for every kind |
| THE EVENT: tilt is an axis sample's `value` | The comment said that `x`, `y` hold the tilt |
| RAW MICE: a report's press comes before its movement | Both have the report's time, but a cursor summed from the samples is still at the report's start when the press arrives |

`yin_sdl_source()` on Linux (X11, SDL 3.4.18; Wayland is not built):

| Kind | Tier | Bounds (stamp minus event) | From |
|---|---|---|---|
| mouse | 3, host part | 1.498 to 16.646 ms | SDL stamps X11 buttons and motion with `SDL_GetTicksNS()` at its pump (read in its source). `ysp/screen.h` v0.18.0's `test_screen_glx` clicked through XTEST, one poll per 60 Hz frame: 100 clicks in five runs ([screen.md](screen.md#input-from-the-task-examples-v0180)). The upper bound is a frame: a loop that polls less often stamps later |
| touch, pen | 3 | none | the same pump time (read in SDL's source); no tablet or touch screen here |
| keyboard | 3, host part | none | through `ysp/screen.h`, the X server's ms time or the kernel's evdev time ([screen.md, "Kernel key times"](screen.md#kernel-key-times)); without it, SDL's pump. Two stamps with different bounds, so the entry gives none. `yscr_event_input()` marks X11 keys tier 3 on the event (`caps.raw_keyboard` is false on Linux). v0.7.0 gives each stamp its own entry (below) |

Tier 3 is the tier rule's "a tick-quantized OS time, a poll": the pump
runs once a frame in a frame loop, and the measured stamps sit up to a
frame after the click. XTEST's buttons come with `which` 0, so the
adapter marks them tier 3 on the event (`yin_sdl_mouse_stamp()`); the
entry covers buttons that come with a device's id.

## v0.7.0: Linux keys by their stamp

[examples_findings.md](examples_findings.md) B7: on Linux,
`yin_sdl_source()` gave keys one entry, tier 3, and `yscr_event_input()`
marked every X11 key tier 3, also a key with the kernel's evdev time.
`ysp/screen.h` gives a key one of three times
([screen.md, "Linux key tiers by stamp"](screen.md#linux-key-tiers-by-stamp-v0190)).
The tier now follows the stamp, as on macOS (v0.5.0):

| Stamp | `ctx` from `ysp/screen.h` | Tier on the event | Entry: tier, bounds (stamp minus event) |
|---|---|---|---|
| the kernel's evdev time, us | `os_stamps` on, `t_os` the kernel's time | 0: the entry's | UNKNOWN, host part, 101 to 18183 ns (`YIN__X11_KERNEL_*`) |
| the X server's time, ms | `os_stamps` on, `t_os` 0; or `os_stamps` off | 3 | 3, host part, -1.99 to -0.96 ms (`YIN__X11_SERVER_*`) |
| SDL's pump time | as the X server's | 3 | as the X server's |

`os_stamps` follows `ysp/screen.h`'s `caps.os_input_stamps`, true while
its X11 hook reads at least one keyboard's evdev node. With it off,
`yin_sdl_source()` gives the X server's entry, the stamp most keys then
have. The kernel's stamp gets UNKNOWN, not tier 1 or 2: no device clock
is fit (tier 1), and no run from a key press to the stamp was made with a
photodiode or a line-in (tier 2). It covers the host part only, after
the keyboard's scan and USB poll. The bounds of the kernel's entry are
`ysp/screen.h`'s `test_screen_glx` part 11: the stamp minus the start of
the `write()` to uinput, 100 keys in five runs. The X server's are
`ysp/screen.h` v0.17.0's: its time minus the kernel's, with uinput and
with a wireless keyboard pressed by hand.

On Linux `ctx.os_stamps` and `t_os` apply to keys only. A mouse button
with `os_stamps` on gets tier 3, as without it: SDL stamps X11 buttons at
its pump.

## Verification

v0.7.0, on 2026-11-06 (the Linux desktop, GCC 12.2, SDL 3.4.18):
`test_input_sdl` checks both Linux key entries and a key's tier with and
without `t_os`. `ysp/screen.h`'s `test_screen_glx` gives 20 of 20 uinput
keys stamp 0 with the kernel's time and 20 of 20 XTEST keys tier 3.
Mutants `in-10` and `in-11` killed; `in-07` and `in-08` again (`in-08`'s
anchor moved). Not built: Windows, macOS, MSVC, clang.

v0.6.0, on 2026-11-06 (the Linux desktop: Debian 12, GCC 12.2, SDL
3.4.18's headers): `test_input_sdl` 178 checks pass (84 without SDL),
with every pen event converted (motion, x and y tilt and pressure axes,
down, up with the eraser and the tip, a barrel button, proximity in and
out) and the Linux entries. Built as C99 and C11 with `-Wall -Wextra
-Wpedantic -Wshadow -Werror` against SDL 3.4.0's headers too (no
proximity state there) and as C++17 against 3.4.18's. Mutants `in-05`
to `in-09` killed, and `in-01`, `in-02` again; `ms-01` and `ms-04` of
`ysp/response.h`'s list, which edit this header, killed. Not built:
Windows, macOS, MSVC, clang.

v0.5.0, on 2026-11-05 (Apple clang 21, macOS 27.0.1): `test_input_sdl`
115 checks pass, with the new context fields (`t_os` replaces the time;
`os_stamps` without it gives tier 3 to a key and a button) and the macOS
source entries; the C and C++ compile checks build. The measurement is
`ysp/screen.h`'s (above). 3 of 3 mutants in the new
`tests/mutate/input.toml` caught.

v0.4.0, on 2026-10-10: the test pins `yin_source`'s new offsets (`lo_ns`
at 8, `hi_ns` at 16, `note` at 24) and its size (32), and the event's
size (48) and offsets as before. The USAGE block builds and runs as C99
(gcc `-Wall -Wextra -Wpedantic -Werror`) and C++17.

v0.3.0, on 2026-10-08, with the same compilers and flags as below: the
test pins the new offsets and sizes (`unc_us` at 26, 2 bytes; `ticks` at
44, 4 bytes), the new constants, and that the raw mouse adapter writes
both new fields as 0 over a buffer filled with 0xA5. 77 checks pass
(gcc, MinGW, MSVC; with SDL3 through `test_input_sdl`).
`ysp/response.h`'s test and its 50 mutants pass on v0.3.0.

v0.2.0:

On 2026-10-08, with MinGW-w64 gcc 16.1 (C11, C99, C++17; `-Wall -Wextra
-Wpedantic -Wshadow -Werror`) and MSVC 19.44 (C11, C++17; `/W4 /WX`):

| Check | Result |
|---|---|
| `tests/adapt/input_test.c` | 58 checks pass; 67 with `YIN_TEST_SDL` (v0.1.0 and v0.2.0) (SDL 3.4.0 headers, nothing linked): the event's size and every field offset, every constant's value, the raw mouse adapter, the SDL adapter |
| `tests/compile/input.c`, `.cpp` | Build and run on both compilers |
| `ysp/response.h` through the aliases | Its test unchanged: 20,604 checks, 20,669 with the SDL adapter; 49 of 49 mutations caught (`ms-01` and `ms-04` now edit `ysp/input.h`) |
| `examples/response/trial_keyboard.c --sim` | As scripted |
