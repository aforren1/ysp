# ysp/input.h

Status: v0.3.0, 2026-10-08. The header's manual (its comment block) is the
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

## Verification

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
