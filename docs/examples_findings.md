# Findings from four task examples (2026-11-06)

Status: open list. Each item names a header problem that one of the four
new examples showed: `trial_posner`, `trial_visuomotor`,
`trial_timed_response` and `trial_copying` (examples/response/). No
header was changed. Each change below is a proposal and needs the usual
header work (CLAUDE.md, "Changing a header"). Remove an item when it is
fixed or rejected.

Machine for every run: the Linux desktop (Debian 12, GCC 12.2, RX 6600,
X11 GLX_OML, 59.9994 Hz). No tablet, no photodiode, no line-in; window
runs used scripted input.

## Bugs (wrong data or a hang)

| # | Header | Problem | Proposed change | From |
|---|---|---|---|---|
| B1 | response.h:1934 | `yrsp_finish()` sets `YRSP_R_TIMEOUT` when there is no response and `duration_ns > 0`, also when no onset came and no deadline ran (an early-aborted trial gets TIMEOUT \| NO_ONSET) | Set TIMEOUT only when the deadline ended the window | posner |
| B2 | response.h:703-715 | `yrsp_cursor` sums float positions, so the cursor drifts off the count grid (0.033 px in 160 trials at gain 0.1). The gain's value changes the data | Keep an int64 count sum per axis; position = origin + gain x count | visuomotor |
| B3 | response.h:1114-1134 | A caller column with the same name as a `yrsp_data_set()` column (`rt_s`) makes the call fail and the row is lost | Skip non-SET columns, or refuse the clash at `ytr_data_open` | timed |
| B4 | response.h, HELD KEYS | Held-key state is per collector. A key released while another collector was fed stays down in the first; its next press is HELD and the trial hangs | Document that every collector must see every event; with per-arm durations (F2) one collector is enough | timed |
| B5 | gfx.h:11537 | A repeated consecutive point makes the whole POLYLINE fail at draw time | Skip zero-length segments | copying |

## Missing features

| # | Header | Need | Proposed change | From |
|---|---|---|---|---|
| F1 | response.h:761 | A forced response can come before the stimulus (negative preparation time; seen at -33 ms in a window run). `min_rt_ns >= 0` makes it EARLY, never a response | Allow `min_rt_ns < 0` (or `desc.early_valid_ns`): valid presses before the onset with a negative rt | timed |
| F2 | response.h:763 | The deadline is fixed at open; blocks with different deadlines need two collectors (see B4) | `yrsp_arm_ex(c, t, duration_ns)` or `yrsp_set_duration()` | timed |
| F3 | response.h | No way to end a window on an early press; the program scans `yrsp_entries()` each frame. No non-final way to learn why a window ended | `desc.end_on_early` with a `YRSP_R_EARLY_END` flag; `yrsp_ended_by(c)` | posner |
| F4 | response.h:1945 | The onset cannot be read while armed: `yrsp_plan_frame()` returns -1 once it is final. A first version of `trial_visuomotor` hung on this | `yrsp_onset_now(c, &onset)` | visuomotor |
| F5 | response.h:1746 | A DISTANCE crossing measures from the last position fed before `yrsp_arm()`; the reference cannot be set | `yrsp_arm_at(c, t, x, y)` | copying |
| F6 | timeline.h:599-609 | Scheduling audio from the timeline (`ytl_peek` window, MARK filter, dedup, late path, lead clamp) is about 40 lines in the caller; this is its first use outside the tests | A helper when both headers are included; `yrsp_onset_plan(t)` for a plan onset not tied to a frame | timed |
| F7 | trials.h:186-246 | "Each stimulus 5 times per 20, at most 2 in a row" needs a synthetic factor, BLOCKED groups and `constraints_span_blocks` (26 of 60 seeds break the run rule without it). An open-ended criterion block needs a fixed cap at open | Constraints under RANDOM, repaired per repetition; extend the schedule at run time | timed |
| F8 | trials.h | Weights are per row; per-factor weights are multiplied by hand. A `weight` column becomes a factor | Factor weights in `ytr_factor_desc`, or a weight column kept out of the factors | posner |
| F9 | trials.h:2267 | The data writer is bound to one session; blocks of different designs need a new session or a session-less writer | `ytr_data_rebind()`, or a note in DATA FILE | timed |
| F10 | outline.h | A short word without pack/layout takes about 25 lines (also repeated in `trial_sternberg`) | `yol_text_path(ctx, font, utf8, size, &path)` and a shared bitmap fallback | posner |
| F11 | gfx.h:11529 | The POLYLINE quad is symmetric about the stimulus origin: a path away from the origin costs twice (10.6 vs 5.1 ms for a 224-point chunk). Every pixel evaluates every segment | Center the quad on the path's box; a segment-instanced stroke primitive | copying |
| F12 | gfx.h:6413 | The SIM backend ignores `yscr_desc.window_w/h` (800 x 600), so `--sim` geometry differs from a window run (also `trial_mouse_tracking`) | SIM takes the window size | visuomotor |

## Docs

| # | Where | Fix | From |
|---|---|---|---|
| D1 | response.h:333 | Still says raw mice are Windows and macOS only (screen.h's two places are fixed in its v0.18.0) | visuomotor |
| D3 | response.h, THE FRAME LOOP | An early-press check must run before `ytl_evaluate()` in the frame where the target lands | posner |
| D4 | response.h:695-700 | `yrsp_cursor` integrates the hand; a rotation or other mapping is the caller's, after the feed, and the clamp bounds the hand, not the drawn cursor | visuomotor |
| D5 | gfx.h:541 | Trim (0, 0) means "whole": an animated trim from 0 shows the full path on its first frame | copying |
| D7 | audio.h:339-360 | AUTO on Linux takes the card exclusively; through PulseAudio the format must match the sink (s16 here) | timed |
| D8 | table.h | The per-table arena overhead is not stated (an 8-row, 1-column CSV needs 4560 bytes) | visuomotor |

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
