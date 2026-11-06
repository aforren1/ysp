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
| B6 | input.h:449-460, 693 | Linux raw mice carry Windows' source note ("stamped at read, 0.15 to 0.40 ms") and tier UNKNOWN; `yin_sdl_source()` has no Linux branch (pen and mouse tier 0) | Linux branches: evdev's kernel stamp for raw mice; SDL's queue time for X11 pointer and pen | visuomotor, copying |

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
| F11 | gfx.h | Every pixel of a POLYLINE chunk's quad evaluates every segment: 5841 points cost 51 ms of GPU a frame as chunks of 224, 1.8 ms as chunks of 16, and one LINE instance per segment 0.66 ms (Intel UHD 770 through Mesa 22.3.6, headless 1024 x 768: the ink benchmark ran on the iGPU surfaceless EGL picks, not the RX 6600; its 0.10 ms LINE row was an artifact of the row before it). The quad's centering on the path's box is done (gfx.h 0.18.0) | A segment-instanced stroke primitive | copying |
| F13 | screen.h:16507, 16241 | `yscr_event_input()` drops synthetic mouse events (a tablet's), with no switch; `yscr_restamp()` returns 0 on SIM, so synthetic SDL events cannot go through it in `--sim` | A desc flag; `yscr_event_input_at(s, ev, t, out)` | copying |
| F14 | input.h:570-614 | Pen `pen_state` and `device_type` are dropped; `YIN_ERASER` is only on down and up | Carry `pen_state` in `code` | copying |
| F15 | screen.h:15602 | The describe line has no raw-mouse state on Linux | `raw_mice=<nodes>(reports n, unfocused n, syn_dropped n)` | visuomotor |

## Docs

| # | Where | Fix | From |
|---|---|---|---|
| D1 | screen.h:1311, 16679; response.h:333 | Three places still say raw mice are Windows (or Windows and macOS) only | visuomotor |
| D2 | input.h:72 | Pen tilt is in `value` of AXIS samples, not in x, y | copying |
| D3 | response.h, THE FRAME LOOP | An early-press check must run before `ytl_evaluate()` in the frame where the target lands | posner |
| D4 | response.h:695-700 | `yrsp_cursor` integrates the hand; a rotation or other mapping is the caller's, after the feed, and the clamp bounds the hand, not the drawn cursor | visuomotor |
| D6 | input.h | `yin_from_mouse()` puts a report's press before its movement | copying |
| D9 | CLAUDE.md | GCC 12's AddressSanitizer also needs `setarch -R` on this kernel (16 of 40 `trial_posner` runs hung without it) | posner |

## Tests

- `tests/adapt/response_test.c` converts PEN_DOWN and a pressure axis
  only; no test converts PEN_MOTION, PEN_UP, tilt or proximity.

## Rejected

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
