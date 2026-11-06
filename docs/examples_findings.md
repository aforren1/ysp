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
| B5 | gfx.h:11537 | A repeated consecutive point makes the whole POLYLINE fail at draw time | Skip zero-length segments | copying |

## Missing features

| # | Header | Need | Proposed change | From |
|---|---|---|---|---|
| F3 | response.h | No way to end a window on an early press; the program scans `yrsp_entries()` each frame. No non-final way to learn why a window ended | `desc.end_on_early` with a `YRSP_R_EARLY_END` flag; `yrsp_ended_by(c)` | posner |
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
