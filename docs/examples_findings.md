# Findings from four task examples (2026-11-06)

Status: open list. Each item names a header problem that one of the four
new examples showed: `trial_posner`, `trial_visuomotor`,
`trial_timed_response` and `trial_copying` (examples/response/), or that
the fixes of response.h 0.8.0, input.h 0.6.0, screen.h 0.18.0, gfx.h
0.18.0, table.h 0.3.1 and audio.h 0.9.1 showed. Those versions fixed the
first list's bugs B1 to B6, features F1, F2, F4, F5, F12 to F15 and the
first part of F11, and docs D1 to D9. Each change below is a proposal
and needs the usual header work (CLAUDE.md, "Changing a header"). Remove
an item when it is fixed or rejected.

Machine for every run: the Linux desktop (Debian 12, GCC 12.2, RX 6600,
X11 GLX_OML, 59.9994 Hz). No tablet, no photodiode, no line-in; window
runs used scripted input.

## Bugs (wrong data or a hang)

| # | Header | Problem | Proposed change | From |
|---|---|---|---|---|
| B7 | screen.h, input.h | On Linux `yscr_event_input()` gives every key tier 3, because `caps.raw_keyboard` is false there, also when the key carries the kernel's evdev time (checked 2026-11-06 with a uinput keyboard: the key time fell 59 to 65 us after the write) | A Linux key source entry by stamp kind: kernel, X server, SDL | input-screen fix |
| B8 | screen.h (SDL) | 20 clicks from a uinput mouse gave SDL's own event stream 1 press (which 0) and no release; XTEST clicks gave 20 of 20. Cause not known (docs/screen.md) | Find the cause before a raw-mouse claim about buttons through SDL | input-screen fix |

## Missing features

| # | Header | Need | Proposed change | From |
|---|---|---|---|---|
| F3 | response.h | No way to end a window on an early press; the program scans `yrsp_entries()` each frame. No non-final way to learn why a window ended | `desc.end_on_early` with a `YRSP_R_EARLY_END` flag; `yrsp_ended_by(c)` | posner |
| F6 | timeline.h:599-609 | Scheduling audio from the timeline (`ytl_peek` window, MARK filter, dedup, late path, lead clamp) is about 40 lines in the caller; this is its first use outside the tests | A helper when both headers are included; `yrsp_onset_plan(t)` for a plan onset not tied to a frame | timed |
| F7 | trials.h:186-246 | "Each stimulus 5 times per 20, at most 2 in a row" needs a synthetic factor, BLOCKED groups and `constraints_span_blocks` (26 of 60 seeds break the run rule without it). An open-ended criterion block needs a fixed cap at open | Constraints under RANDOM, repaired per repetition; extend the schedule at run time | timed |
| F8 | trials.h | Weights are per row; per-factor weights are multiplied by hand. A `weight` column becomes a factor | Factor weights in `ytr_factor_desc`, or a weight column kept out of the factors | posner |
| F9 | trials.h:2267 | The data writer is bound to one session; blocks of different designs need a new session or a session-less writer | `ytr_data_rebind()`, or a note in DATA FILE | timed |
| F10 | outline.h | A short word without pack/layout takes about 25 lines (also repeated in `trial_sternberg`) | `yol_text_path(ctx, font, utf8, size, &path)` and a shared bitmap fallback | posner |
| F12b | gfx.h, ysp-vis | `ygfx_open()` on an open SIM screen cannot see the window size, so it stays `desc.width`/`height` or 800 x 600; ysp-vis opens its gfx this way | Report the SIM size in `yscr_caps.mode` (screen.h), or set it in the binding | gfx fix |
| F16 | response.h, trials.h | `yrsp_data_set()` skips a caller's column by reading the data writer's private column record (`offset == YTR_UNBOUND`): two headers coupled through a private field | A public query in trials.h (is this column set per row?) | response fix |
| F17 | gfx.h:541 | Trim (0, 0) means "whole" (documented in 0.18.0); an animated trim from 0 needs a workaround | A flag that marks an explicit trim, so (0, 0) with it is empty (changes the struct layout) | gfx fix |
| F18 | examples | The examples write the screen's describe line at open, so its counters (key_time, raw_mice) are always 0 in the .meta file | Write the describe line again at close | user run, 2026-11-06 |
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
