# Findings from the task examples (2026-11-06)

Status: open list. Each item names a header problem that one of the five
new examples showed: `trial_posner`, `trial_visuomotor`,
`trial_timed_response`, `trial_copying` and `trial_bimanual`
(examples/response/), or that
the fixes of response.h 0.8.0, input.h 0.6.0, screen.h 0.18.0, gfx.h
0.18.0, table.h 0.3.1 and audio.h 0.9.1 showed. Those versions fixed the
first list's bugs B1 to B6, features F1, F2, F4, F5, F12 to F15 and the
first part of F11, and docs D1 to D9. input.h 0.7.0, screen.h 0.19.0,
gfx.h 0.19.0, trials.h 0.7.0, response.h 0.8.1 and ysp-vis 0.19.0 fixed
B7, B8 (the cause: Xorg's libinput debounce, docs/screen.md), F12b, F16,
F17 and F18. Each change below is a proposal
and needs the usual header work (CLAUDE.md, "Changing a header"). Remove
an item when it is fixed or rejected.

Machine for every run: the Linux desktop (Debian 12, GCC 12.2, RX 6600,
X11 GLX_OML, 59.9994 Hz). No tablet, no photodiode, no line-in; window
runs used scripted input.

## Bugs (wrong data, a hang or a failed build)

| # | Header | Problem | Proposed change | From |
|---|---|---|---|---|
| B9 | timeline.h:1805, 2701 | `ytl_base_time()` leaves `*out` unset when it returns false; GCC 12 at `-O2 -Werror` then stops a caller's build with maybe-uninitialized (seen at timeline.h:3766), so each caller writes `bt = 0` first | Set `*out = 0` on failure | bimanual |

## Missing features

| # | Header | Need | Proposed change | From |
|---|---|---|---|---|
| F3 | response.h | No way to end a window on an early press; the program scans `yrsp_entries()` each frame. No non-final way to learn why a window ended | `desc.end_on_early` with a `YRSP_R_EARLY_END` flag; `yrsp_ended_by(c)` | posner |
| F6 | timeline.h:599-609 | Scheduling audio from the timeline (`ytl_peek` window, MARK filter, dedup, late path, lead clamp) is about 40 lines in the caller; this is its first use outside the tests | A helper when both headers are included; `yrsp_onset_plan(t)` for a plan onset not tied to a frame | timed |
| F7 | trials.h:186-246 | "Each stimulus 5 times per 20, at most 2 in a row" needs a synthetic factor, BLOCKED groups and `constraints_span_blocks` (26 of 60 seeds break the run rule without it). An open-ended criterion block needs a fixed cap at open | Constraints under RANDOM, repaired per repetition; extend the schedule at run time | timed |
| F8 | trials.h | Weights are per row; per-factor weights are multiplied by hand. A `weight` column becomes a factor | Factor weights in `ytr_factor_desc`, or a weight column kept out of the factors | posner |
| F9 | trials.h:2267 | The data writer is bound to one session; blocks of different designs need a new session or a session-less writer | `ytr_data_rebind()`, or a note in DATA FILE | timed |
| F10 | outline.h | A short word without pack/layout takes about 25 lines (also repeated in `trial_sternberg`) | `yol_text_path(ctx, font, utf8, size, &path)` and a shared bitmap fallback | posner |
| F19 | response.h:1086-1092 | No way to read the trace while the window is open: `yrsp_finish()` stops it. The reach target's color needs the peak speed when the hold starts, and a hold can break and start again, so the program keeps its own position ring (trial_bimanual.c:375-389) | `yrsp_trace_now(c, &n)`, as `yrsp_onset_now()` | bimanual |
| F20 | response.h, input.h | A position made from two devices (one mouse per hand) is fed as one DISTANCE channel, so `result.device` and `stamp_tier` are those of the hand whose report crossed; the other hand's part is up to one report interval old. The program adds a `move_hand` column | A note in MICE, or a derived-sample flag in input.h whose tier is the worst of its sources | bimanual |
| F21 | response.h:774-781 | `yrsp_cursor` clamps to [0, w] x [0, h]; a hand kept as a displacement needs a false 2W x 2H area placed at (W, H) | Explicit clamp bounds, or w = h = 0 for no clamp | bimanual |
| F22 | timeline.h:104-112 | No channel whose value is a function of time: the sum-of-sines target is computed outside the timeline, which gives only the anchor, the ramp tween and the end mark. Part of the continuous-control design (a signal path from inputs and generators to stimulus properties), not a separate fix | `ytl_set_fn(tl, ch, base, fn, ctx)` evaluated at the frame's base time, or the signal-path header | bimanual |
| F23 | trials.h:1888, 4488 | `block_size` is one value for all blocks; a list of blocks of different sizes and kinds needs a one-row-per-block table under `ORDER_LIST`, which refuses `reps = 1`; `ti.block` stays 0, so the program finds blocks by row change. Values drawn per block (the phases) have no home: vars are per trial | `desc.block_sizes[]` or a block column; vars per block | bimanual |
| F24 | screen.h | No call for a device's name: `SDL_GetMouseNameForID()` names the listed mice, and an unlisted node's name is only in ring records, cut at 23 bytes. Click-to-assign with its refusals is about 25 lines, as in `trial_two_keyboards` | `yscr_device_name()` and a small assign helper | bimanual |
| D10 | response.md | A frame's flip record arrives in the next `yscr_begin()`, before that frame's input is polled; hand values read at a record's onset need the input polled first (the first `--auto` failed on this) | A note in THE FRAME LOOP | bimanual |
| F11 | gfx.h | Every pixel of a POLYLINE chunk's quad evaluates every segment: 5841 points cost 51 ms of GPU a frame as chunks of 224, 1.8 ms as chunks of 16, and one LINE instance per segment 0.66 ms (Intel UHD 770 through Mesa 22.3.6, headless 1024 x 768: the ink benchmark ran on the iGPU surfaceless EGL picks, not the RX 6600; its 0.10 ms LINE row was an artifact of the row before it). The quad's centering on the path's box is done (gfx.h 0.18.0) | A segment-instanced stroke primitive | copying |

## Rejected

- F14's `device_type` (input.h 0.6.0 carries `pen_state` only). It is
  the pen's, the same on every event, and SDL 3.4.18 added it: ask SDL
  once with `SDL_GetPenDeviceType(which)`, or log it with the device.

- Physical screen size from EDID in `yscr_caps` (posner). EDID sizes are
  often 0 or wrong, and extenders and network video present an EDID that
  is not the panel's. The measured `view` block of ysp/rigfile.h stays the
  only source of degrees; without it the examples mark the geometry
  NOMINAL.

## Toolchain

- GCC 12.2 at `-O2` drops a `(float)` rounding in a loop that stores
  into a `double` array; `-O0`, `-O1`, `-O3` and `-O2
  -fno-tree-vectorize` keep it (reproduced 2026-11-06 on the Linux
  desktop). `trial_copying` keeps its samples in `double` to avoid it.
  No header is known to hit it.
