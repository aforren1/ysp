# ysp/rdk.h design notes

This note explains why `ysp/rdk.h` has its current form and records what
was measured. The manual in the header tells you how to use it.

All numbers come from one machine: a Windows 11 laptop with an Intel Iris
Xe that drives a 1920 x 1200 panel at 60 Hz, MSVC 19.44 (Visual Studio
2022) and MinGW-w64 gcc 16.1 unless a row says otherwise. No photodiode
was used, so no number here is a measurement of light.

## Status

v0.1.0, 2026-10-06. The design was reviewed and approved by the
coordinator on 2026-10-06; "Decisions" records the answers.

| Item | State |
|---|---|
| Header: algorithms, presets, edges, clocks, interleaved sets, lifetimes, outputs, digest, replay, snapshots | Done. |
| Integer state, exact reading of float inputs, Philox4x32-10 | Done. Golden digests equal on MSVC 19.44 (C, C11, C++17), MinGW gcc 16.1 (C99, C11, C++17), WSL gcc 11.4 (-O2, -O3 -march=native, -Ofast -march=native, ASan and UBSan) and the Python model. CI adds Linux gcc and clang and macOS arm64 clang. |
| `tests/adapt/rdk_test.c` | Done: see "Tests". |
| `tests/compare/rdk_ref.py` | Done: an independent Python model; the 8 golden digests are its output. |
| Mutations (`tests/mutate/rdk.toml`) | Done: 29 mutants, 26 caught, 2 equivalent, 1 control; 29 of 29 as expected on the final header. See "Mutations". |
| CI | Not run yet: the Linux and macOS arm64 runs of the golden digests are the first on those platforms. |
| `examples/rdk/gfx_rdk.c` | Done: 4 pages; `--sim` in CI replays two trials from their logs. |
| `examples/rdk/bench.c`, `examples/rdk/gfx_rdk_bench.c` | Done: see "Cost". |
| Python binding (`bindings/python/ysp_rdk`, `ysp.rdk`) | Done 2026-10-07: see "Bindings". 25 tests pass on Windows (CPython 3.14 and 3.9, MSVC) and on Linux (WSL2, CPython 3.10, gcc 11.4, an isolated build with the header staged as CI does). CI builds and tests it on Linux, macOS arm64 and Windows. |

## Decisions

Answers from the review of 2026-10-06.

| # | Question | Decision |
|---|---|---|
| 1 | Name | `ysp/rdk.h`, prefix `yrdk_`. |
| 2 | LEAST_RECENT and the LL preset | Kept: a published algorithm (Law and Gold 2008; Pilly and Seitz 2009's LL) for 2 bytes a dot. |
| 3 | A geometric lifetime (Psychtoolbox's `f_kill`) | Not in v0.1. It is defined per update, not per second, so a dropped frame or another refresh rate changes the lifetime. The manual says how to approximate DotDemo (a fixed lifetime of 1 / f_kill frames under CLOCK_FRAMES has the same mean, not the same distribution). |
| 4 | The generator | Counter-based: Philox4x32-10. Frozen noise across coherence levels is an experimental need (reverse correlation and motion energy analyses reuse a noise sequence). See "The generator". |
| 5 | A float-state variant | Not built. The integer core is decided on portability. |

## Why the state is integers

A trial must regenerate bit for bit on the rig (MSVC, x64) and in the
analysis (a Python C extension, built by setuptools with gcc on Linux or
clang on macOS arm64). Float state cannot promise that:

- clang contracts `a * b + c` into a fused multiply-add by default
  (`-ffp-contract=on`), and arm64 always has the instruction;
- gcc contracts in its GNU dialects on any target with FMA, and ignores
  `#pragma STDC FP_CONTRACT`;
- `sin` and `cos` differ in the last bit between C libraries.

One different bit in a position changes the update on which a dot leaves
the aperture, and the sequences diverge from there. So:

- A position is two int32 on a lattice where the larger half-size of the
  aperture is 2^30 steps (9.3e-10 of the radius, finer than a float at the
  edge).
- A direction is a uint32, 2^32 steps a turn.
- Sine and cosine come from two literal tables of 257 and 256 Q30 entries
  by angle addition, plus a first-order term for the last 14 bits. The
  tables are generated at 50 digits by `tests/compare/rdk_tables.py`, so
  no platform computes them.
- A float input (coherence, direction, speed, sizes) is read as the exact
  rational it is, with one stated rounding. The step uses a 64 x 64 to
  128-bit product and a 128 / 64 division (Hacker's Delight, divlu).
- Floats appear only in the outputs, which nothing reads back.

The finite checks on inputs read the float's bits, so `-ffast-math`
cannot remove them. The test passes built with `-Ofast -march=native`.

## The generator

Every random word is `W(dot, update, slot)`: Philox4x32-10 (Salmon,
Moraes, Dror and Shaw 2011, Random123's constants) with the 64-bit seed as
key and the counter (dot, update, slot, stream). The layout of slots is in
the manual (REPRODUCIBILITY):

| Slot | Words | Use |
|---|---|---|
| 0 at update 0 | w0, w1, w2, w3 | SAME's key, the start direction, the first try of the start position |
| 0 at update u | w0, w1, w2, w3 | the selection key, a direction (WALK, a birth under DIRECTION), the first try of a placement |
| 1 to 16 | two tries each | placement retries (a disc: 33 tries in all) |
| 17 at update 0 | w0, w1 | the first lifetime |

A placement in a disc rejects points of the square. All 33 tries fail
with probability (1 - pi/4)^33 = 9.7e-23; the dot then goes to the center
and `stats.exhausted` counts it. At 10,000 placements a frame at 60 Hz
that is once in about 5e9 years.

What counter-based buys, and what it does not:

- Frozen noise: a noise dot's words depend on (seed, stream, dot, update,
  slot) only. With one seed at two coherence levels, a dot that is noise
  on an update in both runs lands on the same point (POSITION) or takes
  the same direction (WALK, DIRECTION). The selection key is one word, so
  the signal dots at a higher coherence include those at a lower one, for
  EXACT and BERNOULLI alike. The test checks both.
- Random access to draws: any word can be computed without running the
  generator. Positions still integrate their history (a signal dot is the
  sum of its steps), so a replay from update k needs the state at k:
  `yrdk_snapshot()` gives it, or the replay runs from the start.

Considered: PCG32 (sequential, about 1 ns a word, no frozen noise) and
Widynski's squares32 (counter-based, one word a call; its keys must be
drawn by a stated procedure, so a 64-bit seed is not a key as it is, and
its 64-bit counter does not hold the tuple with a stream). Philox's
128-bit counter holds the tuple and any 64-bit seed is a key.
Measured per 32-bit word (`rdk_bench`, one session, AC):

| Generator | MSVC 19.44 /O2 | MinGW gcc 16.1 -O2 |
|---|---|---|
| Philox4x32-10, one call (4 words) | 28.2 ns (7.1 ns a word) | 8.8 ns (2.2 ns a word) |
| squares32, one word | 3.6 ns | 2.1 ns |

Two ways to make Philox faster were measured and removed (10,000 calls,
median of 400 runs, AC, the lock held):

| Form | MSVC | gcc |
|---|---|---|
| one call at a time | 83 us | 101 us |
| four chains interleaved in one loop | 132 us | not measured |
| SSE2, four lanes | 80 us | 106 us |

Neither is faster by more than the run-to-run spread, so the header calls
Philox one counter at a time.

## Algorithms and their sources

| Field | Value | Source |
|---|---|---|
| noise | DIRECTION: its own direction, drawn at birth, at the signal speed | Scase, Braddick and Raymond 1996 ("random direction"); PsychoPy's default |
| noise | POSITION: a random position each update | Scase et al. 1996 ("random position"); Britten et al. 1992 ("white noise") |
| noise | WALK: a new direction each update, at the signal speed | Scase et al. 1996 ("random walk"); Pilly and Seitz 2009 ("Brownian") |
| signal | SAME: the same dots all trial | Williams and Sekuler 1984; Scase et al. 1996 |
| signal | DIFFERENT: afresh each update | Williams and Sekuler 1984; Britten et al. 1992 |
| signal | LEAST_RECENT: the longest-running signal dots become noise first | Law and Gold 2008; Snowden and Braddick 1989 |
| select | EXACT: round(c n) | (PsychoPy truncates: `int(coherence * nDots)`) |
| select | BERNOULLI: each dot with probability c | the Shadlen lab's `dotsX.m` |
| sets | 3 interleaved sets | Shadlen and Newsome 2001; Roitman and Shadlen 2002 |

The Shadlen lab's code, read in phase 2 (github.com/shadlenlab/dotsX,
`dotsX.m`, commit 63fc105): the selection is Bernoulli per dot
(`L = rand(ndots(df),1) < coh(df)`), the noise dots are replotted at random
(`this_s{df}(~L,:) = rand(sum(~L),2)`), and three sets are interleaved.
A dot that leaves the square is not wrapped: it is put on a random point
of one edge, and one weighted coin per frame picks the edge for all such
dots (`rand < abs(xdir)/(abs(xdir) + abs(ydir))`). Pilly and Seitz 2009
describe this code's edge as a wrap. The MN preset here keeps the
algorithm (Bernoulli, POSITION noise, 3 sets) and uses WRAP, which keeps
the density uniform.

## Edges and density

A uniform density stays uniform when each step is a measure-preserving
map of the aperture onto itself.

- RECT, WRAP: a torus. Each axis is a translation modulo a length.
- CIRCLE, WRAP: a dot moves modulo the length of its own chord. The line
  of motion through the dot cuts the disc in a chord of length
  2 sqrt(R^2 - s^2), s the line's distance from the center. On each chord
  the step is a rotation of a circle of that length, which preserves
  length; the chords along one direction tile the disc, so area is
  preserved (Fubini). This holds for each dot and each update separately,
  so it holds for any mix of signal and noise directions.

This also makes the density test exact: a dot's map does not depend on
its position (the keys and directions are other words), so the positions
stay independent and uniform, and one snapshot of each of many
independent fields is a multinomial sample.

Wraps that are not density-preserving, each caught by the density test
as a mutation (see "Mutations"):

- p -> -p of the overshot point: the dot is outside the disc for one
  update, and the dot jumps to another chord.
- re-entry at a uniform point of the circumference: entries are not
  weighted by the flux.
- half the chord: the dot re-enters in the middle of the disc.

REPLOT is kept to replicate PsychoPy and Psychtoolbox. It is not
density-preserving: signal dots enter across the trailing edge only by
landing there at random. The measured profile is in "Tests".

## Time

The onset clock moves a set by the time since that set's last update,
so a dropped frame moves dots by two periods. The frame clock moves by
`frame_ns` each update, so the displacement per shown frame is fixed and
a dropped frame delays the rest of the trajectory. Under the frame clock
the field's clock for lifetimes is `start + u frame_ns`, so a lifetime
counts updates; the first version compared deaths with `t`, and the test
of dropped frames under the frame clock found it.

## Reproducibility

What to log is in the manual (REPRODUCIBILITY). `examples/rdk/gfx_rdk.c`
logs `rdk.last` on each update of page 4 and replays each trial into a
second field with `yrdk_replay()`; CI runs it on the simulated display,
so the recipe is checked end to end on every platform.

## Tests

`tests/adapt/rdk_test.c`, self-checking, about 7 to 15 s. Every
statistic uses fixed seeds, so a verdict is the same on every run, and
each limit is a chi-square quantile at p = 1e-7 (Wilson and Hilferty) or
a stated bound. The values are from MinGW gcc 16.1 -O2
(`YRDK_TEST_VERBOSE=1`); MSVC gives the same digests and verdicts.

| Check | Measured | Limit |
|---|---|---|
| Philox4x32-10 against Random123's 3 known answers | equal | equal |
| `yrdk_angle()`: exact cases, both rounding paths at a tie, 200,000 random angles against long double | equal | equal |
| `yrdk_sincos()` against libm in double, 2^20 angles | 2.64e-9 | 4e-9 |
| its norm, \|(s, c)\| - 1 | 2.16e-9 | 4e-9 |
| the step D against double | 0.4994 lattice steps | 0.5 + 1e-3 |
| the 128 / 64 division against bit-by-bit, 200,000 cases | equal | equal |
| 8 golden digests against `tests/compare/rdk_ref.py` | equal | equal |
| EXACT: signal dots counted from their displacements, c = 0 to 1, n = 1, 7, 100, 1000 | round(c n) on every update | exact |
| SAME: the count follows a coherence ramp; the set only grows | exact | exact |
| BERNOULLI: the count's mean and variance, 4000 updates of 400 | z = 1.00 and 0.02 | 5 |
| density, 36 configurations (shape x signal x noise x lifetime), WRAP | chi-square / critical 0.42 to 0.79 | 1 |
| density, REPLOT | 21.2 (circle), 18.6 (rect) | must be above 10 |
| lifetime, frame clock: every life exactly L | 0 wrong of more than 200,000 | 0 |
| lifetime: remaining lives at start | chi-square 11.6 | 45.4 (6 df) |
| lifetime: deaths on one update (no pulse) | at most 162 | 202.6 |
| lifetime, onset clock, L = 5.5 frames: lives of 5 and 6 frames, mean | 1.4e-5 from L | 0.002 |
| displacement of signal dots, jittered onsets and drops (float output) | 4.7e-7 units on a 5-unit radius | 1.2e-6 |
| the same across a torus wrap | 4.7e-7 | 2.4e-6 |
| WALK: directions uniform; next direction independent | chi-square 101.6 and 72.7 | 151.6 (71 df) |
| WALK: noise step length against D | 0.64 lattice steps | 3 |
| DIRECTION: directions uniform; never changed while noise | chi-square 69.6; 0 changes | 151.6; 0 |
| POSITION: new position independent of the old (8 x 8 sectors) | chi-square 32.3 | 119.4 (49 df) |
| frozen noise: noise in both runs, coherence 0.2 and 0.55 | same position (POSITION) or direction (WALK, DIRECTION) on every such dot; signal sets nested (EXACT and BERNOULLI) | exact |
| dropped frame, onset clock: positions against the full run | 9.5e-7 | 1.2e-6 |
| dropped frame, frame clock: the run one update behind | equal | equal |
| interleaved sets: each set's step against its own elapsed time; other sets still | 3.8e-7 | 1.2e-6 |
| LEAST_RECENT: no dot signal twice running below c = 0.5; kept runs at most the dropped ones | 0 violations | 0 |
| edges: every position inside, stalls of 0.1 s to 1000 s | 0 outside; 0 chord wraps stuck of 17,578 | 0 |
| chord wrap keeps the dot on its line | 0 off by more than 2 lattice steps | 0 |
| replay of a logged trial, snapshot and restore, two fields interleaved, caller memory | equal digests | equal |
| refusals: 21 bad descs, each with a message naming the field | 21 | 21 |
| allocations after open | 0 | 0 |

The REPLOT profile along the motion (100 seeds x 500 dots, coherence 0.5,
no lifetime, 2 s, counts in 10 strips of equal width, trailing edge
first): circle 1340, 2796, 3975, 5028, 5889, 6628, 7081, 7127, 6262, 3874;
rect (10 x 6, along a direction of 20 degrees) 1676, 3322, 4061, 4693,
5186, 5789, 6269, 6438, 6508, 4680. Divided by each circle strip's
uniform share (its area over pi, times 50,000), the circle's density runs
from 0.52 of the mean at the trailing strip through 0.62, 0.72, 0.83,
0.93, 1.05, 1.17, 1.30, 1.39 to 1.49 at the leading strip: a gradient of
almost 3 to 1 along the motion, at coherence 0.5.

Builds, warnings as errors: MSVC 19.44 /W4 /WX (the default C dialect,
C11 and C++17, and the test as C++17), MinGW-w64 gcc 16.1 -Wall -Wextra
-Wpedantic -Wshadow (C99, C11, C++17, the test as C++17), WSL gcc 11.4
(C99, C11, C++17; the test under ASan and UBSan, at -O3 -march=native and
at -Ofast -march=native: the same digests).

## Mutations

`tests/mutate/rdk.toml`, 29 mutants, run with `uv run
tests/mutate/mutate.py rdk.toml`. Most run with `YRDK_TEST_NO_GOLDEN=1`,
so the kill comes from a property check, not from the digests.

| Result | Count | Mutants |
|---|---|---|
| killed by a property check | 25 | the wraps (point reflection, uniform re-entry, half chord, a step past the rect's edge kept), placement without rejection, unstaggered lives, the death schedule, the frame clock's lifetimes, EXACT truncated, the Bernoulli threshold, the selection count, LEAST_RECENT's runs, WALK drawn once, DIRECTION redrawn, the noise speed, both clocks' dt, a set's dt, the sine quadrant and residual, the direction's sense, Philox's round, the output stride, SAME ignoring a coherence change, noise draws that depend on the coherence, the angle's tie |
| killed by the golden digests only | 1 | placement retries from the wrong slots (the retries stay uniform) |
| survived, equivalent | 2 | rejection `<` for `<=` (only points exactly on the circle, about 1e-9 a try); `>` for `>=` at the radix boundary bucket (any boundary between the m-th and the (m + 1)-th key gives the same set) |
| survived, control | 1 | no change |

Two mutants of the first list survived and were changed: `<=` for `<` on
the rect's edge moves only points exactly on the edge (equivalent in
practice), so it became a step past the edge kept; and the angle's tie
was tested on one rounding path only, so the test got a tie on the other.

## Cost

All on the Iris Xe laptop, AC, the measurement lock held, CPU load at 2
to 15 % when the lock was taken. Two sessions:

- A: 2026-10-06 21:44, the whole table (`rdk_bench`), MSVC 19.44 /O2
  (CMake Release) and MinGW gcc 16.1 -O2, rounds A and B.
- B: 2026-10-06 21:47, after the selection change below: the LL rows
  again, and `gfx_rdk_bench`. The load was 70 % when the run ended:
  another program started during its last rows.

An earlier session at 21:27 is void: another agent's test ran through it
(load 54 to 68 %) and rows differed by up to 2 times between rounds.

### The update (`yrdk_update()`), mean per call, us

The range covers rounds A and B and both apertures. 50 % coherence, 5
deg/s on a 10 deg field, 2000 updates at 60 Hz.

| Rule | 100 dots, MSVC / gcc | 1000 dots, MSVC / gcc | 10,000 dots, MSVC / gcc |
|---|---|---|---|
| SAME, DIRECTION noise (no draws) | 1.3 to 1.4 / 0.7 to 0.8 | 8.9 to 16.0 / 8.9 to 9.1 | 110 to 172 / 102 to 104 |
| the same, lifetime 0.2 s | 1.9 to 2.4 / 1.1 | 13.6 to 17.6 / 11.8 to 12.5 | 154 to 178 / 125 to 130 |
| the same, REPLOT | 0.9 to 1.4 / 0.8 | 10.6 to 14.4 / 9.3 to 9.9 | 101 to 199 / 107 to 142 |
| WN | 2.5 to 4.5 / 1.9 to 2.4 | 25.3 to 32.7 / 18.2 to 21.3 | 224 to 288 / 183 to 227 |
| MN (3 sets) | 2.3 to 2.9 / 1.9 to 2.2 | 22.1 to 35.8 / 18.2 to 21.5 | 285 to 341 / 183 to 213 |
| LL (3 sets), session B | 2.0 to 3.1 / 2.1 to 3.1 | 25.8 to 34.1 / 26.6 to 32.6 | 275 to 314 / 277 to 351 |
| BM | 3.1 to 4.7 / 2.8 to 2.9 | 32.4 to 43.1 / 25.1 to 29.2 | 281 to 318 / 252 to 254 |
| DIFFERENT, WALK | 5.0 to 5.4 / 2.8 to 2.9 | 28.4 to 42.2 / 25.1 to 26.1 | 233 to 443 / 254 to 303 |

The p99 is 1.0 to 2.1 times the mean (the full table is the output of
`rdk_bench`). The output: `yrdk_xy()` for 10,000 dots 6.5 to 14.5 us;
a strided write of x, y and direction into 32-byte records 28 to 90 us.

Bars: 1000 dots at most 10 us: met only by SAME with DIRECTION noise on
gcc (8.9 to 9.1 us); missed by MSVC on it (8.9 to 16.0) and by every rule
that draws. 10,000 dots at most 100 us mean: missed by every rule (101 us
the best). The cost is not the generator's alone: SAME with DIRECTION
noise draws nothing and still costs 10 to 17 ns a dot (the step, the edge
test and the flags); a rule that draws adds 8 to 12 ns a dot: Philox
(10,000 calls: 80 to 106 us) and the placement or the selection. PCG32
would not meet the bars either: the update without draws misses them.
10,000 dots at 300 us is 1.8 % of a 16.7 ms frame.

What was changed by measurement: the sine and cosine of a DIRECTION
noise dot are kept (8 bytes a dot), not computed on each update: 10,000
dots of SAME DIRECTION, 176 and 189 us before, 136 us after (MSVC,
interleaved runs). A radix digit that every candidate shares is not
copied: LL at 10,000 dots, 479 to 591 us before (MSVC, session A), 275
to 314 us after (session B).

### End to end with ysp/gfx.h (`gfx_rdk_bench`), session B

1920 x 1200, ysp/gfx.h as it was in the tree on 2026-10-06 (0.6.0 in
progress), 240 frames a row; CPU is the field's update and output, the
upload and the draw calls, per frame; GPU from frames between two
glFinish calls.

| Workload | CPU mean, us (A / B) | CPU p99, us | field, us | GPU ms (A / B) |
|---|---|---|---|---|
| empty frame | 61 / 34 | 117 / 46 | 0 | 0.464 / 0.391 |
| 1000 dots, WN | 74 / 81 | 95 / 102 | 31 / 34 | 0.429 / 0.430 |
| 10,000 dots, WN | 307 / 304 | 460 / 430 | 254 / 258 | 0.784 / 0.770 |
| 10,000 dots, SAME DIRECTION | 198 / 197 | 246 / 257 | 150 / 149 | 0.681 / 0.678 |
| 2 x 5000 dots, transparent, WN | 295 / 299 | 415 / 439 | 247 / 252 | 0.766 / 0.778 |
| 1000 gabors (32 px) riding the dots, WN | 56 / 96 | 66 / 173 | 21 / 39 | 0.484 / 0.562 |
| 10,000 gabors riding the dots, WN | 421 / 405 | 587 / 634 | 289 / 300 | 1.819 / 1.873 |

Bars: 10,000 dots at most 0.5 ms of CPU (304 to 307 us: met) and 0.5 ms
of GPU over the empty frame (0.32 to 0.38 ms: met); 10,000 gabors at
most 0.6 ms of CPU (405 to 421 us: met) and 2 ms of GPU over the empty
frame (1.36 to 1.48 ms: met).

### What fits a 16 ms frame

From these rows only: 10,000 dots cost 0.2 to 0.5 ms of CPU (by rule)
and about 0.8 ms of GPU in all; 10,000 gabors riding the dots 0.4 ms of
CPU and 1.9 ms of GPU in all. Either fits a 16.7 ms frame many times
over; two fields of 10,000 (transparent motion) cost about twice one.
No row was measured in a window with the swap path, so nothing here
says how often a frame is late.

## Lifetime in ns, open and messages (v0.2.0)

v0.2.0 applies the repository conventions (README.md, Conventions).

- **`desc.lifetime_ns`.** The desc had the lifetime as float seconds, and
  open turned it into ns with `llround((double)lifetime * 1e9)`. A float
  0.3 is 0.30000001192..., so it gave L = 300,000,012 ns; a C caller now
  writes `YRT_MS(300)`, which is 300,000,000 ns. The rules do not change
  for the same L, and neither do the golden digests: the test gives the
  old L values (`life_s()`, the v0.1 rounding). Seconds stay at the
  script layer. The Python binding keeps `lifetime=` in seconds and
  converts it as v0.1 did (float32, then `llround(x 1e9)`), so its fields,
  its test and `tests/c_trial.bin` are unchanged, and so is
  `tests/compare/rdk_ref.py`, which models seconds. A Python field and a C
  field are the same when the C caller passes that L.
- **`yrdk_out.age_s` stays float seconds.** It is a per-dot attribute
  that goes to the GPU next to x, y and the direction (a gabor's phase
  restarts with it). An `int64_t` per dot would double that stream, and a
  shader has no 64-bit integers. It is the one time of this header that
  is not `int64_t` ns, and its name says the unit.
- **`yrdk_open()` returns `bool`.** The message was already in
  `yrdk_error()`. Every other call that returns a code writes the
  message when it fails, so `yrdk_xy()`, `yrdk_write()` and
  `yrdk_snapshot()` take a non-const field. `yrdk_close()` was already
  safe on a zeroed field and twice.

Checked on 2026-10-10 (Windows 11, MinGW-w64 gcc 16.1 and MSVC 19.44):
`tests/adapt/rdk_test.c` passes with its golden digests, with a new test
of close and the messages. `tests/make_c_trial.c` writes the same bytes
as the committed `tests/c_trial.bin`. The USAGE blocks compile as C99 and
C++17. Mutants `conv-01` (no message from a failing write) and `conv-02`
(a negative `lifetime_ns` accepted) are caught; control `00` survives.

## Bindings

The API is made for a binding: plain structs, no callbacks, caller memory
(`yrdk_bytes()`, `desc.mem`), strided outputs, and `yrdk_replay()`
runs a logged trial in one call.

The Python binding (`bindings/python/ysp_rdk`, the module `ysp.rdk`) is a
C extension on the Limited API, so one abi3 wheel serves CPython 3.8 and
later. An analysis opens a field with the rig's desc as keyword
arguments and calls `trajectory(seed, t0, steps)`: one C loop replays the
trial and gives the shown dots after each update as NumPy arrays
(positions, directions, signal flags), with no Python work per dot. The
steps are the rig's log, `rdk.last` of each update, as tuples, `Step`
records or rows of a NumPy structured array. The binding also has start,
update, replay, the outputs of the last update, snapshots and the exact
pieces (`philox`, `angle`, `sincos`).

Its test (`bindings/python/ysp_rdk/tests/test_rdk.py`) checks:

| Check | Result (Windows, Linux) |
|---|---|
| the 8 golden digests, by updates in Python and by `replay()` and `trajectory()` | equal to the C test's and `rdk_ref.py`'s |
| a trial logged by the C header (`tests/c_trial.bin`, written by `tests/make_c_trial.c`; LL, a rect, a lifetime, changing coherence, direction and negative speed, jittered onsets and a drop; MSVC and MinGW write the same bytes) replayed by `trajectory()` | x, y, direction and signal equal bit for bit on all 150 updates; the digest equal |
| a live run's `Field.last` log replayed | the same digest |
| frozen noise, coherence 0.2 and 0.55, three noise rules | the signal sets nested; a dot that is noise in both lands on the same point (POSITION) or takes the same direction (WALK, DIRECTION) |
| snapshot and restore, outputs, the names of the enums, the refusals | as the header |

The wheel's NumPy dependency is the one cost: the outputs are
`numpy.frombuffer` views of bytes that the C code fills.
