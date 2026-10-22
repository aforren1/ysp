# ysp/gfx.h design notes

This note explains why `ysp/gfx.h` has its current form and records what
was measured. The manual in the header tells you how to use it. The plan
that asked for the header is section 4.3 of [rig_spec.md](rig_spec.md).

All numbers come from one machine: a Windows 11 25H2 laptop with an Intel
Iris Xe (driver 32.0.101.7088) that drives a 1920 x 1200 panel at 60 Hz,
and an NVIDIA RTX A500 with no outputs. ANGLE is the copy in Docker
Desktop's Electron folder, `2.1.23876 git hash: fffbc739779a`, unless a
row says otherwise. No photometer, spectroradiometer, photodiode or Bits#
was used, so no number here is a measurement of light.

## v0.2 status

This section is the running record of v0.2. Update it when an item
changes, so that a restart loses nothing.

| Item | State |
|---|---|
| 1. Strokes, MITER, MASK_TEX, `ygfx_sdf_from_mask()` | Done, tested on 4 renderers. Fixed 2026-10-05: ring dots lost their band when the field had no aperture; RECT and CROSS now bevel below a limit of 1.414 (they never did). Stroke GPU time 1.05 to 1.17 times the fill (bar 1.3). |
| 2. Sprites (`src`), nearest and linear | Done, tested. Linear is a hand bilinear from four clamped `texelFetch`, not the sampler (departure, see Sprites and tint). |
| 3. Tint | Done, tested (color, modulation, ADD). |
| 4. Groups | Done, tested. |
| 5. Render targets, `YGFX_STIM_ADD`, `YGFX_ADD` | Done, tested, with the OVER alpha check and the 16-pass limit. ADD kept: 0.43 ms against 9.1 to 9.2 ms. Composite 0.43 ms GPU (bar 1 ms); pass switch 4.7 us CPU (bar 50 us). |
| 6. GL epoch | Done, tested (`ygfx__sync()`). |
| 7. Contract | Done: `ysp_look.w` is the band's center offset, parameters 61 to 70 appended, 336-byte stimulus on 64-bit targets, 8 built-in programs plus the output stage. Open time 1.3 to 1.4 s after the SDF got one call site (3.2 to 5.0 s before). |
| Mutations | 18 of 18 caught (after one test addition). |
| Build matrix | MSVC 19.44 (CMake) and 19.51 C11 and C++17; MinGW gcc 16.1 C99, C11, C++17; WSL gcc 11.4 C99, C11, C++17, ASan and UBSan, llvmpipe pixel test; emcc 6.0.10 C11 run in node, C++17 compiled. All pass. |
| Left | v0.2 frame-loop runs in a window and an allocation count with target passes; timing on renderers other than the Iris Xe. |

## v0.3 status

The running record of v0.3 (SDF shapes, compounds, effects, dashes,
gradients, MSDF). The design is in the session notes of 2026-10-05;
decisions taken from it: the vector program compiles at every open (no
flag); smooth operations are gradient-normalized only; no CSG stack; the
ellipse's iteration count follows its aspect ratio; one shadow offset; no
batching of stimuli with extension blocks; the Gaussian reach is 5 SD; ADD's
gamut check is v0.2's.

| Item | State |
|---|---|
| 0. v0.2 edge model against truth (manual EDGES, this file, test `gl_edge_truth`) | Done on 4 renderers: vertex 0.2500, disc 0.0406 / 0.0100, MITER 0.0800 on each. |
| 1. Types, API, parameter tables, `ygfx_bind.field` | Done. `ygfx_prim_params()` 11 rows, `ygfx_fx_params()` 32, `ygfx_paint_params()` 28; P_OFFSET to P_TRIM_END appended to the stimulus table. |
| 2. Vector program: kinds, compounds, effects, dashes, trim, paint | Done, tested on 4 renderers. Departures: QBEZIER is chords within 0.005 px, not exact to 4e-5 px; dashes on CROSS refused; MITER and BEVEL in the vector program on POLYLINE only. |
| 3. Blend modes (MULTIPLY; ADD for shapes), backend version 2 | Done, tested (MULTIPLY within 4.8e-8 of the product). Backend v2 adds deferred pipelines and `pipeline_finish`. |
| 4. MSDF and MTSDF, mask source rectangles, range rule, glyph runs | Done, tested (MSDF 3.5e-6; glyph runs equal single draws bit for bit). |
| 5. Tests T1 to T12, mutations | Done. 26 mutations: 24 caught on llvmpipe, 1 caught only on D3D11 (the star's reflection guard), 1 equivalent (normalizing a non-smooth fold). Two test additions were needed (an Oklab white check; the MSDF and glyph checks). |
| 6. Bench (under the lock), open time | Done: open 1.96 to 2.14 s (bar 2.0 s, met on 1 of 3 runs). Four bars missed: see "v0.3 cost". |
| 7. Manual, README, rig_spec 4.3, license block | Done. The year of each Inigo Quilez notice was copied from the Shadertoy pages by hand (2015, 2018, 2019, 2019, 2019), because the pages refused automated reads. |
| 8. Build matrix | See "v0.3 builds". |

## v0.4 status

The running record of v0.4 (program cache, video, instances, the
gallery's fixes). The design is in the session notes of 2026-10-06; the
coordinator's conditions are listed with the item each belongs to.

| Item | State |
|---|---|
| 1. Program cache | Done, tested (11 GL cases on 3 renderers, a CPU half with a fake backend). Warm open 11.4 to 35.0 ms on the Iris Xe against 2.3 to 5.3 s without a cache (bar 100 ms: met); a cold open adds 32 to 120 ms of stores (bar 5 %: met). |
| 1b. Kind-specialized vector programs | Done in v0.7 (see "v0.7: kind-specialized vector programs"): RRECT, dashed CIRCLE and CIRCLE compounds; OKLAB paint not built (it does not close its gap). |
| 2. Gallery fixes (calibration CRC, straight-alpha sprites, DIST range rule, first, shape_p); the dots' zero-default bug and the no-pixel audit | Done, tested. |
| 3. Planar YUV, encodings, import, rebind | Done, tested on 3 renderers; D3D11 NV12 import (array slice, shared handles) on the Iris Xe and WARP. Bars: update CPU met (30 to 38 % of RGBA8); GPU 1.0 ms met except DEVICE (1.12 ms); GPU 1.5x RGBA8 missed (1.63 to 1.91). |
| 4. Instances | Done, tested on 3 renderers. Every bar met: 10000 gabors 177 to 191 us of CPU, 1.26 to 1.33 ms of GPU (5.5 ms one stimulus each). |
| 5. Overlap-aware reordering in end() | Done, tested bit for bit against call order on 3 renderers. 1000 interleaved stimuli of 4 kinds: 7.2 to 1.1 ms of GPU, 3.3 to 0.47 ms of CPU. |
| 6. Bench on AC | Done ("v0.4 cost"); a build ran during parts of it, named there. |
| Mutations | 35 of 35 caught ("v0.4 mutations"). |

## What the GPU and ANGLE report

Queries made before the design, offscreen:

| Question | Answer |
|---|---|
| The panel's link | `IDXGIOutput6::GetDesc1`: 8 bits per color, SDR (G22, P709), 270 cd/m2. EDID primaries R (0.600, 0.370), G (0.356, 0.550), B (0.155, 0.110), white (0.3135, 0.3291): a narrow gamut. |
| 10-bit swapchain | `R10G10B10A2_UNORM`: a flip-model swapchain is made; the sRGB color space can present and use an overlay. |
| ANGLE's 10-bit and float surfaces (spec 13) | On the D3D11 device: config 101 is RGBA 10/10/10/2, config 76 is RGBA16F. A client-buffer pbuffer on an `R10G10B10A2` texture works: a clear of 513/1023 reads back as 513 through D3D. |
| So, 10-bit output here | The GPU side works. The link is 8 bits per color, so the driver truncates or dithers. Which one cannot be found without a photometer. v0.1 refuses `YGFX_OUT_10`. |
| Float targets | `EXT_color_buffer_float`, `EXT_float_blend`, `OES_texture_float_linear` on Iris Xe (D3D11), WARP, SwiftShader, NVIDIA (Vulkan) and Mesa 23.2 llvmpipe. A float32 clear of 0.123456789 reads back as 0.123456791. |
| Float to 8-bit and 10-bit codes | Every code k written as k/255 or k/1023 reads back as k, on all four ANGLE renderers. |
| Program binaries | One format, ANGLE's own (0x93A6), valid for one ANGLE build on one adapter and driver. There is no portable binary for GL ES 3.0. |
| Software GL for CI | ANGLE on D3D11 WARP and on Vulkan SwiftShader; Mesa llvmpipe through EGL's surfaceless platform. No window and no Khronos header. |

## The row order of the back buffer

ANGLE's pbuffer on a D3D11 texture (`EGL_ANGLE_d3d_texture_client_buffer`)
puts GL's row 0 at D3D's row 0, which is the top of the screen. A clear of
GL's upper half read back through D3D as the bottom rows. So on DXGI_FLIP
and COMPOSITION the output stage writes the rows the other way, and a
headless context keeps GL's own order. A visible check in an 800 x 600
window confirmed it: a rect placed at the top-left showed at the window's
top-left through GDI, with the same codes as `ygfx_read_output()`
(140, 115, 102 for 0.55, 0.45, 0.40).

## Coordinates

The convention lives in one function, `ygfx__place()`, and its helpers.
Screen pixels have the origin at the top-left corner, x right, y down;
positive angles turn +x toward +y, which is clockwise on the screen. A
stimulus has a `place`, the screen point x, y are measured from (nine
choices, the center by default), and an anchor, the point of its own box
that sits at x, y and that it turns about (`ax`, `ay`, the center by
default, as in Phaser). The pattern is defined about the box's own center,
so the anchor moves a pattern without changing it. The test checks that a
gabor placed by its center and by its top-left corner gives the same bits.

SDL3 reported a pixel density of 1.000 and a display content scale of
1.000 for a window on this panel, so mouse coordinates are pixels here.

## Local coordinates come from gl_FragCoord

The first version interpolated each fragment's point in the stimulus frame
from the quad's vertices. Against a CPU reference in double, the errors
were large on the software renderers and grew with spatial frequency:

| Renderer | Grating max error, interpolated | Grating max error, from gl_FragCoord |
|---|---|---|
| Iris Xe (D3D11) | 1.7e-5 (gabor 1.6e-4) | 1.7e-5 (gabor 1.2e-6) |
| WARP | 1.1e-3 | 1.2e-5 |
| SwiftShader | 2.1e-2 | 1.4e-4 |
| llvmpipe | 3.2e-5 | 1.2e-5 |

A rasterizer snaps vertex positions to its subpixel grid, and the
interpolated coordinates move with the snapped vertices. The vertex shader
now hands the fragment shader the stimulus's exact center (flat), and the
fragment shader computes its point from `gl_FragCoord`, which is the exact
pixel center. The quad only has to cover the stimulus. Shapes with no
aperture are clipped to the box in the shader, so snapping cannot move an
edge either.

## Compile time and the batch length

`ygfx_open()` first took 7.4 to 8.7 s through ANGLE on D3D11 (12 to 20 s
on WARP). Per program, measured:

| Stim block in the shader | Compile and link per program |
|---|---|
| `vec4[16]` (one stimulus) | 27 to 48 ms |
| `vec4[256]` (16 stimuli) | 69 to 88 ms |
| `vec4[1024]` (64 stimuli) | 226 to 265 ms |
| `vec4[4096]` (256 stimuli) | 940 to 1280 ms |
| an array of 256 structs | not timed per program; the open of eight took 7.4 to 8.1 s, as with `vec4[4096]` (7.6 to 8.7 s) |

The polygon loop made no difference (835 to 1126 ms without it). The time
grows with the length of the dynamically indexed uniform array. Batch
length against open time and CPU time per frame (bench, 1000 gabors,
CPU in `begin()` to `end()`):

| Batch | `ygfx_open()` | 1000 gabors, CPU mean |
|---|---|---|
| 16 | 0.8 s | 387 us |
| 64 | 2.2 s | 508 us |
| 256 | 18.8 s | 132 us |

16 is kept: a whole frame of 1000 stimuli stays under 0.5 ms of CPU, and
the open stays under 1.5 s.

## Measured, then deleted

| Choice | Variants | Measured | Kept |
|---|---|---|---|
| Draw calls | batched (one instanced draw per run of 16 like stimuli) against one draw call each | 1000 gabors: 0.20 to 0.50 ms against 2.1 to 5.0 ms of CPU; GPU the same | batched. One draw each remains only as a test seam (`max_batch`), which checks that both give the same bits |
| Uniforms per stimulus | `glUniform4fv` per draw against one buffer upload per frame and a bound range per draw (raw GL, 16 vec4 each) | 1000 draws: 1.36 to 1.74 ms against 0.82 to 2.15 ms of CPU, no winner; batched ranges beat both | one upload per frame and batched ranges; per-draw uniforms were never put in the header |
| Scene format | RGBA16F against RGBA32F | empty frames: no difference (0.46 to 1.26 ms both); 1000 gabors 9.9 to 11.2 ms against 16.1 to 17.4 ms of GPU; full-screen noise 0.9 to 1.0 against 1.7 to 2.3 ms | RGBA16F. RGBA32F remains only as a test seam, because the pixel test needs float32 to measure shader error below half precision |
| Full-frame texture upload | `glTexSubImage2D` from memory against a pixel unpack buffer | 1.1 to 2.6 ms against 9.4 to 19.2 ms of CPU | `glTexSubImage2D`; no PBO path was added |
| A direct path into the back buffer, without the scene | an empty frame against a bare clear of framebuffer 0 | 0.42 to 1.2 ms against 0.12 to 0.38 ms of GPU | the scene path only: its cost is under the 1.5 ms planned, so a second path is not worth keeping |

## Frame cost

`examples/gfx/bench.c` on the Iris Xe at 1920 x 1200, RGBA16F scene,
batch 16. "GPU" is the time per frame of 120 to 300 frames run back to back
between two `glFinish()` calls; it includes the empty frame's cost. Five
runs, Balanced plan: the batch study's (power state not recorded), the two
scene-format runs (the empty frames of that session on AC), and two final
runs on battery; some rows ran in fewer of them. The bars are the ones the
design proposed against the 16.7 ms frame.

| Workload | CPU mean, us | GPU, ms | Bar | Met |
|---|---|---|---|---|
| empty frame: scene clear + output stage, identity CLUT | 113 to 259 | 0.46 to 1.15 | GPU 1.0 ms, CPU 10 us | GPU in 5 of 7 runs (the two on battery missed: 0.89 and 1.15); CPU no |
| empty frame, CLUT 4096 + ordered dither | 56 to 171 | 0.42 to 1.34 | GPU 1.0 ms | in 4 of 7 runs |
| gabors 256 x 256, x 1 | 182 to 248 | 0.49 to 1.13 | | |
| gabors x 10 | 78 to 189 | 0.57 to 0.62 | | |
| gabors x 100 | 75 to 213 | 1.33 to 1.72 | CPU 100 us | in 1 of 5 runs |
| gabors x 1000 | 196 to 502 | 9.3 to 11.2 | CPU 1 ms, GPU 10 ms | CPU yes; GPU in 4 of 5 runs |
| dots x 1000, 4 px, upload each frame | 66 to 288 | 0.37 to 0.61 | CPU 0.1 ms | void: drew nothing (v0.4, "the gallery's findings") |
| dots x 10000 | 87 to 243 | 0.45 to 0.59 | CPU 0.3 ms, GPU 0.5 ms over the empty frame | void: drew nothing |
| dots x 100000 | 654 to 1271 | 1.45 to 1.99 | CPU 2 ms, GPU 3 ms | void: drew nothing; re-measured in "v0.4 cost" |
| noise, full screen, GPU hash | 63 to 285 | 0.83 to 1.01 | GPU 1 ms over the empty frame | yes |
| `ygfx_noise_fill()` 1920 x 1200 on the CPU | 8 500 to 26 600, once | | off the frame thread | on the pump |
| noise, CPU R32F upload + draw | 1 708 to 3 259 | 2.99 to 4.79 | CPU 3 ms | in 2 of 3 runs |
| image RGBA8 full frame, upload + draw | 1 450 to 2 782 | 3.49 to 4.21 | CPU 3 ms | mean yes, p99 (4.4 to 5.8 ms) no |

The one bar missed in every run is the CPU time of `end()` for an empty
frame: about 15 GL calls take 56 to 260 us through ANGLE. It was not
broken down further. ysp/screen.h's present call through ANGLE costs 380 to
1230 us on the same machine, so this is the same order. A frame with up to
1000 stimuli still stays under 0.5 ms of CPU for `begin()` to `end()`.

The variation between runs is the machine's: the same binary moved up to
2x between runs, with the power state and another agent's work on the
machine as the likely causes. The runs were not repeated enough to
separate them. In the runs at batch 64 and 256 (not kept) the GPU rows
varied as much.

### In a real frame loop

`examples/gfx/load.c`, an 800 x 600 window on the panel, the composition
swapchain, the window kept on top, 1 minute each, on battery. Phases from
ysp/screen.h's flip records.

| Load | Flips | Dropped vblanks | Late targets | DRAW p50 / p99, us | UPLOAD p99, us |
|---|---|---|---|---|---|
| nothing (empty frames) | 3554 | 43 | 0 | 188 / 476 | 0.7 |
| 1 gabor | 2565 (1) | 21 | 2 | 286 / 654 | 0.7 |
| 1 gabor, again | 3600 | 1 | 0 | 255 / 590 | 0.6 |
| 100 gabors | 3595 | 5 | 1 | 332 / 788 | 0.8 |
| 10000 dots, uploaded each frame (void: they drew nothing; not re-measured) | 3567 | 3 | 1 | 268 / 643 | 215 |

(1) The run ended before its minute; the window was most likely covered or
closed. The rerun below it is complete.

The drops were the system's: no late target came with a DRAW or UPLOAD
phase near the 16.7 ms budget (DRAW p99 under 0.8 ms in every run), and the
empty-frame run had the most drops. screen.md describes the drops of
windowed runs.

### Allocations

MSVC debug build, `gfx_load --allocs` with 100 gabors and 10000 dots,
10 s: 0 C runtime heap calls in the 566 frames after frame 30. The control
(`--allocs-control`, one `malloc` per frame) counted 569. Allocations
inside ANGLE's DLLs are not seen by this hook.

## Pixel tests: measured error and tolerance

`tests/adapt/gfx_test.c` renders each stimulus into an RGBA32F scene
(the test seam), reads it back, and compares it with a CPU reference in
double at each pixel center, found through `ygfx_local()`, the inverse
of the coordinate transform. Errors are in scene units (full scale 1).

| Check | Iris Xe (D3D11) | WARP | SwiftShader | llvmpipe | Tolerance |
|---|---|---|---|---|---|
| soft edges (cosine, Gaussian) | 1.69e-5 | 2.93e-6 | 9.49e-5 | 2.40e-6 | 5e-5; SwiftShader 3e-4 |
| hard edges, wrong pixels | 0 | 0 | 0 | 0 | 0 (pixels within 1e-3 px of an edge skipped) |
| `ygfx_hit()` against GPU coverage >= 0.5 | 0 | 0 | 0 | 0 | 0 (same skip) |
| grating, sine | 1.72e-5 | 1.20e-5 | 1.41e-4 | 1.20e-5 | 5e-5; SwiftShader 4e-4 |
| grating, square | 7.15e-6 | 1.22e-6 | 4.73e-5 | 1.16e-6 | 2e-5; SwiftShader 1.5e-4 |
| gabor | 1.23e-6 | 1.11e-6 | 8.77e-5 | 4.85e-7 | 4e-6; SwiftShader 3e-4 |
| dots | 1.47e-5 | 7.70e-6 | 9.49e-5 | 7.70e-6 | 5e-5; SwiftShader 3e-4 |
| images, all eight formats, 1:1 | 2.97e-8 | 2.97e-8 | 7.43e-8 | 7.43e-8 | 2.2e-7 |
| noise against `ygfx_noise_value()` | 0 | 0 | 0 | 0 | 0 (bit for bit) |
| output codes: identity, rounding, CLUT | 0 | 0 | 0 | 0 | 0 |
| ordered dither against the 8 x 8 pattern | 0 | 0 | 0 | 0 | 0 |
| noise dither, mean minus target, in codes | +0.0008 | +0.0008 | +0.0008 | +0.0008 | 0.005 (v0.1: -0.0015; the frame index, and so the pattern, moved when v0.2 added frames before this check) |
| two gabors against the sum of each alone | 1.19e-7 | 1.19e-7 | 1.19e-7 | 1.19e-7 | 4e-7 |
| user shader against the built-in grating | 0 | 0 | 0 | 0 | 1e-6 |
| anchor moved with the position | 0 | 0 | 0 | 0 | 1e-6 |
| batched against one draw each | 0 | 0 | 0 | 0 | 0 |

Each tolerance is about 3x the largest error measured on a renderer of
its kind. SwiftShader's transcendentals are about ten times coarser than
the others' and get their own column. The design's probe had put SwiftShader's
`cos` error at 1.9e-4; with the point from `gl_FragCoord`, the largest
grating error is 1.4e-4.

Calibration checks, against published values:

| Check | Reference | Measured | Tolerance |
|---|---|---|---|
| sRGB primaries and D65 to XYZ | IEC 61966-2-1:1999, the printed matrix | max 3.9e-5 | 1.2e-4 |
| CLUT of a display with sRGB's transfer function | sRGB's encoding | 1.3e-3 from 17 levels, 3.6e-5 from 64 | 4e-3, 1.1e-4 |
| cone fundamentals, embedded | CVRL `linss2_10e_1.csv`: all 441 rows compared once; spot rows in the test | equal to float rounding | 1e-7 relative |
| RGB to LMS of Psychtoolbox's `B_monitor` | `T_cones_ss2 * B` in MATLAB R2023a | 1.3e-8 relative | 1e-6 |
| DKL matrix | Psychtoolbox's `ComputeDKL_M.m`, background RGB (0.4, 0.5, 0.3) | 3.1e-8 relative | 1e-6 |
| DKL unit axes as RGB increments | the same | 6.8e-8 relative | 1e-5 |
| axis isolation | algebra: L-M keeps S and luminance, S keeps L and M | under 1e-6 relative | 1e-5 |

Psychtoolbox's `T_cones_ss2.mat` equals CVRL's table to 1e-13. The MATLAB
script resampled `B_monitor` (380 to 780 nm at 5 nm) linearly onto 1 nm,
as the header does. Its numbers are constants in the test.

Mutations: 14 deliberate faults in a copy of the header, each built and run
against the test on llvmpipe. All 14 failed the test: the local y axis
sign, the rotation direction, an off-center cosine edge, the erfc constant,
output rounding at 0.49, the Bayer bit order, the GPU hash constant, an
uninterpolated CLUT, batches that ignore textures, image rows upside down,
the anchor's sign, cone integration one nm off, the DKL luminance weights
swapped, and a hit test that uses the margin. The first run let three
through (rounding, the Bayer bit, textures in batches), and the test grew
a check for each.

v0.2 mutations: 18 more, by a script that applies each fault to a copy of
the header and runs the test on llvmpipe. All 18 failed the test: a stroke
as F(|d - c| - s/2), INSIDE and OUTSIDE swapped, no bevel past a polygon's
limit, no bevel on a right angle below 1.414, bilinear clamped to the
texture instead of the source rectangle, tint alpha not on coverage, ADD
without tint alpha, a group's scale on the edge width, group opacity
ignored, group ori not added, target rows bottom first, no feedback
refusal, the cache kept over an epoch change, the distance transform
without its half texel, OVER keeping alpha (v0.1's blend), ring dots
without their band, a non-uniform mask fit drawn, and a target's clear
ignored. The first run let one through (ADD without tint alpha); the tint
check grew an ADD image.

## Shaders on GL ES 3.0

GL ES 3.0 has no portable binary, so the runtime takes text: the user's
body wrapped by `ygfx_shader_wrap()`, which the pack tool calls too, so
the two cannot disagree. ANGLE compiles it when the pack loads. The
compiler's log names the body's own lines: the wrapper puts `#line 1`
before the body, and the test checks that an error on the body's line 3
is reported as `0:3` (ANGLE `0:3:`, Mesa `0:3(`). A program binary cache
would cut the open from 0.8 to 1.4 s to a few ms; v0.4 has one ("v0.4: the
program cache").

## v0.2: what was added and why

### One uniform block, four free floats

The per-stimulus uniform block stays at 256 bytes (16 vec4). ANGLE's
compile time on D3D11 grows with the length of the dynamically indexed
array (the batch table above), so a longer block would make
`ygfx_open()` slower for every user. v0.2 puts its new data in the four
floats that v0.1 did not use, or derives it in the shader, or keeps it on
the CPU:

| Float | v0.2 use |
|---|---|
| `look.w` | the stroke band's center offset, px (was `visible`, which the CPU already tests) |
| `shape.w` | the band's half width, px; 0 is a fill |
| `dir.a` | the miter limit; its sign is the polygon's orientation; 0 is ROUND |
| `color.a` | IMAGE: the tint's alpha |

The tint's rgb goes into `color.rgb` (color and ADD images) or into `dir`
on the CPU (modulation images). The source rectangle goes into `p[0..3]`,
which an IMAGE did not use. A group never reaches the shader:
`ygfx__place()` composes it into the stimulus's center, axes and pixels
per unit.

### Strokes

A stroke is the band between the offsets a1 < a2 of the boundary. Its
coverage is F(d - a2) - F(d - a1), with F the fill coverage under the
stimulus's edge profile. This is the hard band convolved with the
profile's kernel. The shortcut F(|d - c| - s/2) gives a band that reaches
1 at every width; the true band does not when s is narrower than the edge.
The test checks this: a 1 px band under a Gaussian edge of SD 2 px peaks at
0.197 (2 erf(0.25 / sqrt 2)), not 1.

A round join is the SDF's own offset, so one SDF value serves both
offsets. A mitered RECT or CROSS uses the box's Chebyshev distance, whose
offsets keep sharp corners. A mitered convex POLYGON uses the max of its
edges' line distances. Past the miter limit, a bevel plane per corner cuts
the corner; the bevel depends on the offset level, so a band needs the SDF
at both levels. For a polygon, one loop gives both levels, because only the
bevel terms depend on the level. A right angle's miter is sqrt 2 times the
width, so RECT and CROSS bevel only below a limit of 1.414. MITER on a
curve, a rounded rect or a concave polygon is refused
(`YGFX_ERR_ARG`): a concave corner's miter is not the max of line
distances.

### MASK_TEX

`ygfx_sdf_from_mask()` runs Felzenszwalb and Huttenlocher's exact
squared Euclidean distance transform twice (to the nearest inside texel,
to the nearest outside texel) and puts the boundary halfway between the
two texel centers. The test compares it with a brute-force search on 20
random masks: equal in every texel. Against an analytic disc of radius 24
texels, the largest difference is 0.49 texel; a binary mask places its
boundary only to within half a texel's diagonal (0.71). The shader samples
the texture by the same four-texelFetch bilinear as images, so an R32F
mask needs no `OES_texture_float_linear`. A box of another aspect ratio
than the texture's is refused: a non-uniform fit would scale the distance
differently along each axis. `ygfx_hit()` returns false for a mask,
because the distances are on the GPU only.

### Sprites and tint

Nearest sampling reads one texel by `texelFetch` at an integer address
clamped to the source rectangle, so no neighbor can contribute. Linear
sampling interpolates by hand from four `texelFetch` whose addresses are
clamped to the rectangle. The design had the sampler's own linear filter
with the sample point clamped half a texel inside. The hand form was kept
because it is exact for any rectangle, it needs no padding, and its
weights are float on every renderer: the test measured the hand form
within 8.4e-7 of a double reference on all four renderers. There are no
mipmaps: draw sprites at 1:1 or larger.

The tint multiplies linear light. For a modulation image it multiplies
`dir` and the amplitude, which `dir` and `contrast` already do; the manual
says so.

### Groups

A group is one level of transform, composed on the CPU in
`ygfx__frame_px()`: S = group place point + (x, y) + R(group ori) *
scale * (P - group anchor point), with P the member's anchor in the group's
box. Scale multiplies what is in units (positions, sizes, sigma, shape
parameters, dot positions; sf is divided by it). It does not multiply what
is in px: `edge_width`, `stroke`, `dot_size` and `check`. A px value sets
the stimulus's spatial-frequency content at the panel, so a zoom must not
change it. Pixi's `container.scale` zooms everything. The test draws each
member in a group and the same stimulus placed alone from the composed
values: the largest difference is 6.0e-8 of full scale on every renderer.

### Render targets

A target pass splits the scene pass: the frame's commands are recorded in
segments, played in call order, each with its own frame block (17 fixed
slots: the scene and 16 passes). Nothing is allocated per pass. A target
holds scene-space linear values; the CLUT and dither run once, in the
output stage, which the test checks code for code against direct drawing.
A target is drawn with its rows top first, so it reads back and samples
with no flip; its local y axis keeps its sign in GL's axes.

v0.1's OVER blend kept the destination's alpha. A target drawn as a
premultiplied image needs the coverage it accumulated, so v0.2's OVER
accumulates alpha: (ONE, ONE_MINUS_SRC_ALPHA) for color and for alpha. The
scene's alpha is never read. The test draws the same frame through the
built-in backend and through a copy that puts v0.1's blend back after
each apply: the scene's rgb and the output codes are bit-identical. The
control shows that the copy took effect: in a target cleared to alpha 0,
the two blends give a different alpha at every texel drawn.

### The GL epoch

`ygfx__sync()` runs on entry to every function that issues GL, when
ysp/screen.h defines `YSCR_HAS_GL_EPOCH`. When `yscr_gl_epoch()` has
moved (a present callback ran), it drops the GL state cache. When
`yscr_gl_generation()` has moved, the context is new: the call returns
`YGFX_ERR_LOST` and the handle closes without deleting anything,
because the old names could name the new context's objects. The test
installs a present callback that unbinds the program, the vertex array,
the uniform ranges and the textures and disables blending: the next frame
is the reference, code for code. The control makes the same changes with
no epoch: 901 output values differ.

### v0.2 pixel tests: measured error and tolerance

The same method as v0.1's table above: an RGBA32F scene read back against
a CPU reference in double at each pixel center. ANGLE rows: the Docker
copy and Electron v38.8.6's copy gave the same numbers. llvmpipe: Mesa
23.2.1 in WSL2.

| Check | Iris Xe (D3D11) | WARP | SwiftShader | llvmpipe | Tolerance |
|---|---|---|---|---|---|
| strokes, soft edges: 6 shapes x 3 alignments x 2 edges x 2 angles x both joins, ring dots, a 1 px band | 1.62e-5 | 9.12e-6 | 9.53e-5 | 8.59e-6 | 5e-5; SwiftShader 3e-4 |
| strokes, hard edges, wrong pixels | 0 | 0 | 0 | 0 | 0 (pixels within 1e-3 px of a band edge skipped) |
| `ygfx_hit()` on strokes against GPU coverage >= 0.5 | 0 | 0 | 0 | 0 | 0 (same skip) |
| alignments by hand, MITER corner pixels, bevels (polygon past its limit, RECT and CROSS below 1.414) against the CPU form | 0 | 0 | 0 | 0 | 0 |
| MASK_TEX (fill and stroke, scale 1 and 2, turned) against the CPU's bilinear of the same texture | 1.42e-5 | 1.16e-5 | 9.53e-5 | 1.10e-5 | 5e-5; SwiftShader 3e-4 |
| sprites, nearest: wrong texels; bleed from a neighbor (nearest and linear) | 0; 0 | 0; 0 | 0; 0 | 0; 0 | 0 |
| sprites, linear, against the CPU's clamped bilinear | 7.77e-7 | 7.77e-7 | 8.36e-7 | 8.36e-7 | 2.5e-6 |
| tint: color, modulation and ADD images against exact products | 4.18e-8 | 4.18e-8 | 6.26e-8 | 6.26e-8 | 2e-7 |
| group members against the same stimuli placed alone | 5.96e-8 | 5.96e-8 | 5.96e-8 | 5.96e-8 | 2e-7 |
| `ygfx_hit()` through a group | 0 | 0 | 0 | 0 | 0 |
| shapes through a target against direct drawing | 0 | 0 | 0 | 0 | 0 (bit for bit) |
| target row order, clear, persistence, RGBA8 codes | 0 | 0 | 0 | 0 | 0 |
| 30 gabors through an RGBA16F target and ADD against direct drawing | 2.78e-4 | 2.78e-4 | 1.46e-4 | 2.78e-4 | 8e-4 (half precision of the increments) |
| output codes through a target with a CLUT against direct drawing | 0 | 0 | 0 | 0 | 0 |
| scene rgb and codes, OVER with alpha against v0.1's OVER | 0 | 0 | 0 | 0 | 0 (bit for bit) |
| output codes after a present callback that changes GL state | 0 (control: 901) | 0 (901) | 0 (901) | 0 (901) | 0 |
| refusals (MITER on a curve and a concave polygon, a non-uniform mask fit, a target sampled in its own pass), the pass limit, a new context | all | all | all | all | all |

CPU only: `ygfx_sdf_from_mask()` equals brute force at every texel of
20 random masks (exact: both are square roots of the same integers), and
is within 0.49 texel of an analytic disc (bound 0.75, from the mask's half
diagonal of 0.71).

### The edge model against truth

The checks above compare the GPU with the CPU form of the same function.
They cannot see where that function is not the Euclidean distance, or where
a profile of the distance is not a blur of the shape. Both happen, by
design, and v0.2's manual did not say so. Coverage is F(d): the profile of
the signed distance to the nearest boundary point.

- **F(d) is a blur only on a straight edge.** At a vertex of interior angle
  a, the Gaussian blur of the hard shape is a / 360 of the step and F(0) is
  0.5: 0.25 off at a right angle, 0.333 at 60 degrees, 0.083 at 150
  degrees. Above 1/255 of the step within 3.75 SD of a right-angled vertex.
  On a disc of radius R, F(d) is about 0.2 SD / R above the blur at the
  boundary: 0.448 at R = SD, 0.115 (2), 0.041 (5), 0.020 (10), 0.010 (20),
  0.004 (50).
- **MITER's field is a bound outside corners.** It is the largest distance
  to the edges' lines: Euclidean inside a convex shape, below the Euclidean
  distance outside a corner. A soft outer edge there is up to 0.083 of the
  step too high for a right angle (at 1.17 SD along the bisector), more at
  sharper corners. This does not depend on the edge width: it scales with
  it.

The analytic numbers came from double-precision integrals (the disc's
exact blur is the noncentral chi-square CDF with 2 degrees of freedom).
The test `gl_edge_truth` measures them on the GPU against references that
share none of the header's arithmetic: the exact blur of a rect (a product
of erf), the blur of a disc by a 2000-panel Simpson integral, and the
Euclidean distance by brute force over a rect's four edge segments. Each
measured deviation must be at most the analytic value plus the renderer's
tolerance and at least 0.9 (vertex, disc) or 0.5 (MITER corner, sampled
off its worst point by the pixel grid) of it.

| Check (GAUSSIAN, SD 2 px) | Analytic | Iris Xe | WARP | SwiftShader | llvmpipe |
|---|---|---|---|---|---|
| 90 deg vertex on a pixel center, F(d) minus the blur | 0.2500 | 0.2500 | 0.2500 | 0.2500 | 0.2500 |
| disc, R = 5 SD, F(d) minus the blur | 0.0406 | 0.0406 | 0.0406 | 0.0406 | 0.0406 |
| disc, R = 20 SD | 0.0100 | 0.0100 | 0.0100 | 0.0100 | 0.0100 |
| MITER rect fill, F(field) minus F(Euclidean d) | 0.0830 | 0.0800 | 0.0800 | 0.0800 | 0.0800 |

A user who needs a soft edge that is exactly a blur uses a straight edge,
or for a sharp rect the exact separable blur (v0.3, section v0.3 below).

### v0.2 cost

`examples/gfx/bench.c --only v0.2`, Iris Xe at 1920 x 1200, RGBA16F
scene, on AC (Balanced plan), measurement lock held. The rows of one table
take turns in blocks of 12 frames, 25 rounds; the GPU time is per frame
between two `glFinish()` calls, as the median over the rounds; "over
empty" is the median of the per-round difference from an empty frame. Two
runs of the final shaders. Three earlier runs, before the SDF got one call
site (below), gave stroke ratios of 0.95 to 1.17 and the same target and
sprite rows within 10 %.

| Workload | GPU over empty, ms | Ratio to the fill | Bar | Met |
|---|---|---|---|---|
| circle, 1160 px, cosine edge: fill | 0.227 to 0.242 | | | |
| the same circle, 8 px stroke | 0.262 to 0.282 | 1.15 to 1.17 | 1.3 | yes |
| pentagon, 1114 px: fill | 0.693 to 0.738 | | | |
| the same, 8 px stroke, MITER | 0.738 to 0.773 | 1.05 to 1.06 | 1.3 | yes |
| the same, 8 px stroke, ROUND | 0.739 to 0.775 | 1.05 to 1.07 | 1.3 | yes |
| rect 1880 x 1160: fill | 0.401 to 0.402 | | | |
| the same, 8 px stroke, MITER, limit 1.2 (a bevel) | 0.431 to 0.436 | 1.07 to 1.08 | 1.3 | yes |
| MASK_TEX circle, 288 x 288 R16F texels fitted to 1160 px | 0.307 to 0.320 | 1.32 to 1.35 of the analytic circle | | |

A stroke's quad covers the shape's box and the band, not a ring, so the
shader runs on about the same pixels as the fill's.

| Workload | GPU over empty, ms | CPU over empty, us | Bar | Met |
|---|---|---|---|---|
| 1000 sprites of 32 x 32 from one atlas | 0.490 to 0.493 | 153 to 155 | | |
| 100 gabors of 256 x 256 | 0.959 to 0.979 | 24 to 25 | | |
| the same 100 gabors in a group, its x bound each frame | 0.955 to 0.970 | 26 to 29 | | |
| an empty target pass, cleared (1920 x 1200 RGBA16F) | 0.000 to 0.001 | -6 (noise) | | |
| full-screen target composite, COLOR | 0.430 | 9 | 1 ms GPU | yes |
| full-screen target composite, ADD | 0.427 to 0.430 | 5 to 8 | 1 ms GPU | yes |
| 1000 gabors drawn each frame | 9.13 to 9.20 | 130 to 141 | | |
| pass switch, per target pass | 9.0 to 9.1 us of frame time | 4.7 | 50 us CPU | yes |

The pass switch: 16 passes a frame, each one small shape into a target
between two scene draws, against the same 32 shapes with no pass. The
difference includes the batching that a pass breaks (32 draw calls
against 2). The first form of this bench (16 empty passes on one target)
gave 0.1 us, because consecutive passes on one target bind nothing; it did
not measure a switch and was replaced.

ADD is kept: 1000 gabors rendered once into a target and added each frame
cost 0.43 ms of GPU, against 9.1 to 9.2 ms drawn each frame (9.3 to 11.2 ms
in v0.1's runs). The target holds RGBA16F, so the increments are rounded
to half precision (2.8e-4 of full scale measured for 30 gabors at contrast
0.3; under a quarter of an 8-bit code).

### Open time and the SDF's call sites

The first v0.2 shaders called the SDF from three places in the aperture
(a fill, a band at one level, a band at two levels). `ygfx_open()` then
took 3.2 to 5.0 s on the Iris Xe (five runs) and 3.9 s on WARP, against
0.8 to 1.4 s in v0.1: ANGLE's D3D11 path inlines a function at every call
site, and this one holds two polygon loops. Now one function gives both
levels and has one call site: 1.31 to 1.43 s on the Iris Xe (five runs)
and 1.40 s on WARP, for eight built-in programs and the output stage.

## v0.3: what was added and why

### One vector program, always compiled

The v0.3 kinds, compounds, effects, dashes, trim and paint share one
built-in program. A stimulus's uniform block is 16 vec4 (256 bytes), and
the bound range is 4096 bytes. So the blocks after a stimulus's own block
can hold its data (polygon vertices, path points, primitives, effects,
paint stops and matrices, up to 247 vec4) at no compile cost: the shader
reads them with the same index arithmetic. A stimulus that uses such
"extension blocks" draws alone, as one instance; one that fits its block
batches as before. `desc.max_draws` now counts blocks. There is no
`desc.vector` flag: the program compiles at every open, which the open
time below pays for.

The block: vec4 0 the placement; 1 (half extents, px per unit, quad
margin); 2 (opacity, gate, offset, band center); 3 (rgb, band half width);
4 (dash, gap, dash offset, length); 5 (kind, profile, edge width, flags);
6 (trim start, trim end, cap, primitive count); 7 (second point's offset,
compound onion, point count); 8 (effect and paint offsets, path half
width); from 9 the data. A primitive is 4 vec4. Flags: DASH, TRIM, SMOOTH,
S (arc length needed), PAINT, FX, RBLUR.

Each function returns the distance, its gradient and the arc length s
together. The gradient serves the smooth operations' normalization and
the effects' offsets; s serves dashes and trim. The CPU has the same
functions in double (`ygfx__vfield()`), which the test compares and
`ygfx_hit()` uses.

### Compile time and GPU time on ANGLE's D3D11 path

The first vector program took 6 s to open and 4.5 ms of GPU for a
full-screen RRECT. What was found, by compiling variants in a harness
(`vharness.c`: compile time and GPU time of one GLSL file):

- **Loops that the compiler cannot bound are kept; loops it can bound are
  unrolled.** A field loop over a constant 56 primitives became 56 copies.
  The loop bounds now come from the frame block (`ysp_axes.y` = 16,
  `.z` = 3), which the compiler cannot see as constants.
- **A nested loop makes FXC flatten.** An inner loop over a path's two
  points made both compile and GPU time about 8 times worse. The field is
  one flat loop (`for t < n * npts`).
- **The quadratic Bezier's closed form** (a cubic solve and Newton steps)
  was the largest single cost. QBEZIER is now drawn as a polyline of
  chords from the CPU, within 0.005 px of the curve. This is a departure:
  the design had it exact to 4e-5 px.
- **ANGLE compiles programs in parallel** when the program statuses are
  read only after every link has started. A probe with four programs went
  from 7.3 s to 3.0 s. Backend version 2 has deferred `pipeline_make` and
  a `pipeline_finish` that reads the status; `ygfx_open()` starts all
  eleven programs, then finishes each.
- A trick that turned branches into 0-or-1 loops (`YSP_IF`) made the
  compile slower and was removed.

Result: `ygfx_open()` 2143, 1957 and 2096 ms on the Iris Xe (bar 2.0 s;
v0.2: 1.3 to 1.4 s). There is no program binary cache (as decided).

### Trigonometry precision

D3D's `sin` and `cos` are coarse, and `atan` on D3D and SwiftShader is not
exact. So:

- the CPU computes every angle's sine and cosine (arc ends, pie edges, the
  star's sector) and puts them in the block;
- the star and the regular polygon fold a point into their sector by a
  power of a unit complex number, not by a `sin` and `cos` per pixel. A
  point that a coarse `atan` puts in the next sector is reflected back
  across the tip line. Removing that reflection passes on llvmpipe and
  SwiftShader but fails on both D3D11 renderers (a star's soft error
  7.3e-5 against a 5e-5 tolerance; 3.5e-5 against brute force where the
  guarded form gives 4.3e-6);
- an arc's s is measured from the middle of its sweep
  (`clamp(sw / 2 + atan(pv, pu), 0, sw)`), so the `atan` branch cut is
  outside the arc. SwiftShader put s off by the full sweep at angle 0
  before.

### Polyline interiors

A polyline as a union of boxes and join polygons is the Euclidean distance
outside, but inside it is only a bound near each inner corner. For the
Bezier's chords, that bound was 1.1 px off. With ROUND joins the field is
now the distance to the center line, minus the half width, which is
exact; a BUTT or SQUARE end adds the half strip past the last segment. The
chords then measure 0.0050 px from the curve. MITER and BEVEL keep the
box form: a bound inside near each turn.

### The quadratic smooth minimum, normalized

A raw smooth union has a gradient below 1 in the blend, so its soft edge
is up to 2.2 times wider there (0.19 to 0.20 of the step at a settled
neck, from the design's CPU numbers). After a smooth operation the field
is divided by its gradient's length: 0.015 at a settled neck. Where two
parts first connect, the field has a saddle with a zero gradient and no
first-order fix: 0.122 measured. A non-smooth fold of exact fields has a
unit gradient already; the test cannot see the division there (mutation
1 below is equivalent).

### v0.3 pixel tests: measured error and tolerance

`tests/adapt/gfx_test.c`, the same method as v0.2: an RGBA32F scene,
read back, against a CPU reference in double at pixel centers. The parts
`v0.3 kinds` (243 stimuli: every kind, fill and the three strokes, soft
and hard, two angles), `v0.3 truth`, `v0.3 fx`, `v0.3 paint` and `v0.3
msdf`. Largest |error| of full scale; the tolerance is about 3 times the
worst renderer's error, with SwiftShader in its own column where it needs
one.

| Check | Tolerance (SwiftShader) | Iris Xe | WARP | SwiftShader | llvmpipe |
|---|---|---|---|---|---|
| kinds, soft, against the CPU form | 5e-5 (3e-4) | 1.7e-5 | 8.2e-6 | 9.6e-5 | 8.2e-6 |
| the same with dashes or trim | 1.5e-3 (1e-2) | 4.0e-4 | 4.0e-4 | 2.5e-3 | 5.8e-5 |
| hard pixels wrong; `ygfx_hit()` wrong | 0 | 0 | 0 | 0 | 0 |
| exact kinds against F(brute-force Euclidean d) | 6e-5 (3e-4) | 4.3e-6 | 4.9e-6 | 4.9e-6 | 4.9e-6 |
| the CPU form's distance against brute force | 1e-3 px | 9.4e-6 px | (CPU) | | |
| QBEZIER chords: coverage; distance | 2e-3; 0.0055 px | 1.3e-3; 0.0050 px | same | same | same |
| a dash pattern moved by one period against unmoved | 2e-3 (2e-2) | 3.1e-6 | 1.6e-5 | 1.6e-5 | 1.6e-5 |
| dash count on a circle; trim 0..1 against none | 0 wrong | 0 | 0 | 0 | 0 |
| effects in one pass against the layers drawn by the CPU | 5e-5 (3e-4) | 1.9e-5 | 5.7e-6 | 1.2e-4 | 5.7e-6 |
| EXACT_BLUR shadow against the erf product | 5e-5 (3e-4) | 1.5e-7 | 1.7e-7 | 1.7e-7 | 1.7e-7 |
| MULTIPLY against the product | 2e-7 | 4.8e-8 | 4.8e-8 | 4.8e-8 | 4.8e-8 |
| extension blocks batched with v0.2 stimuli, against single draws | 0 | 0 | 0 | 0 | 0 |
| paint RGB | 2e-6 | 1.2e-6 | 1.2e-6 | 6.4e-8 | 3.0e-7 |
| paint OKLAB, against Ottosson's matrices through the calibration | 2e-5 (1e-4) | 1.8e-6 | 1.8e-6 | 9.2e-7 | 9.5e-7 |
| paint DKL_POLAR, against `ygfx_cal_dir_dkl()` | 1e-4 (1e-3) | 1.5e-5 | 6.3e-6 | 1.6e-4 | 1.3e-6 |
| paint VERTEX, against barycentric | 2e-5 | 1.2e-7 | 1.1e-7 | 1.1e-7 | 1.1e-7 |
| MSDF against the CPU's bilinear median | 5e-5 (3e-4) | 3.5e-6 | 3.5e-6 | 3.5e-6 | 3.6e-6 |
| a glyph run against the same glyphs drawn alone | 1e-5 | 0 | 0 | 0 | 0 |
| refusals (range rule, MSDF with ROUND joins, blend on a modulation, ...) | all | all | all | all | all |

The dashes' error is the arc length's float error at a dash end, where
the coverage across s is steep; it is largest on SwiftShader. The MSDF
test atlas has different values per channel (R = v, G = v + 0.2, B = v -
0.15) so a mean in place of the median fails.

The CPU half also checks the parameter tables, `ygfx_bind.field` (all
or nothing), and Oklab's white (L = 1, a = b = 0 within 1e-3 through a
nominal sRGB calibration). Two development seams: `YGFX_TEST_PARTS`
runs named parts, `YGFX_TEST_VERBOSE` prints the worst pixels.

### Exactness as measured

Against references that share none of the header's arithmetic (brute-force
distance to a dense boundary sample, a supersampled blur, the erf
product). Deviation in units of the edge's step; Gaussian edge.

| Shape or operation | Distance | Measured | Predicted |
|---|---|---|---|
| RRECT, ARC, PIE, CAPSULE, NGON, STAR, POLYGON fillets, ELLIPSE 2:1 and 9:1 | exact | 4.9e-6 (float) | float only |
| QBEZIER | chords, 0.0050 px | 1.3e-3 | |
| UNION, reflex 90 degree crease (an L of two rects) | a bound inside | 0.0822 | 0.0830 |
| MITER corner, outside (v0.2) | a bound | 0.0800 | 0.0830 |
| SMOOTH_UNION normalized, settled neck | first order | 0.0150 | 0.012 to 0.015 |
| SMOOTH_UNION normalized, at the moment of connection | first order | 0.1220 | 0.12 to 0.15 |
| F(d) against the blur, 90 degree vertex | | 0.2500 | 0.25 |
| F(d) against the blur, disc of R = 5 SD, 20 SD | | 0.0406, 0.0100 | 0.0406, 0.0100 |
| drop shadow (F of the moved field) against the blur, at a corner | | 0.2484 | 0.25 |
| EXACT_BLUR | exact | 1.7e-7 | 0 |

Not measured separately: INTERSECT and SUBTRACT outside a convex crease,
XOR, POLYLINE MITER and BEVEL inside a turn (each a bound, about 0.083 at
90 degrees by the same geometry as the union's crease).

### v0.3 cost

`examples/gfx/bench.c --only v0.3`, Iris Xe, 1920 x 1200, RGBA16F scene,
AC, the measurement lock held, rows interleaved (25 rounds of 12 frames).
GPU over an empty frame, ms.

| Workload | GPU, ms | CPU over empty, us | Bar | Met |
|---|---|---|---|---|
| `ygfx_open()`, three runs | 2143, 1957, 2096 ms | | 2.0 s | 1 of 3 |
| v0.2 rounded rect, full screen, fill | 0.396 | 8 | | |
| RRECT (vector program), fill | 0.929 | 17 | 1.3x v0.2 | no: 2.3x |
| RRECT, LINEAR paint, RGB | 1.383 | 19 | | |
| RRECT, LINEAR paint, OKLAB | 1.536 | 20 | 1.1x solid | no: 1.65x |
| STAR 5, rounded | 0.860 | 20 | | |
| ELLIPSE | 1.315 | 27 | | |
| v0.2 circle, 8 px stroke | 0.271 | 8 | | |
| the same, dashed | 0.903 | 7 | 1.2x the stroke | no: 3.3x |
| compound of 1, 2, 8, 32 circles, full screen | 0.782, 1.219, 3.528, 12.433 | 9 to 21 | 0.15 ms per primitive per Mpx | no: 0.163 |
| compound of 8, 600 x 400 box | 0.617 | 15 | | |
| the same with every effect | 1.622 | 16 | 1.0 ms | no |
| 100 compounds of 4, 60 x 20, one draw each | 0.474 | 120 | 600 us CPU | yes |
| a run of 200 MSDF glyphs, 38 px | 0.121 | 18 | 0.2 ms, 20 us | yes |

The vector program's per-pixel cost is the price of one general program:
every pixel runs the fold loop, the dash and paint branches, and the
effects' tests, where v0.2's shape program has one switch. The v0.2 kinds
still draw with v0.2's program, so nothing v0.2 drew became slower. A
full-screen compound of 32 primitives (12.4 ms) does not fit a frame with
anything else; compounds are for stimuli of a few hundred pixels.

### Mutations

`mutate03.py`, a script that was not kept: each fault in a copy of the
header, the test built against it and run on llvmpipe; a fault must fail
the test. This table is the record; tests/mutate/ does not hold these
mutants.

| # | Fault | Result |
|---|---|---|
| 0 | no gradient normalization after a smooth op | caught |
| 1 | normalized without a smooth op | equivalent: a hard fold of exact fields has a unit gradient |
| 2 | the second point without its offset | caught |
| 3 | SUBTRACT operands swapped | caught |
| 4 | onion sign | caught |
| 5 | circle arc length off its start | caught |
| 6 | trim end as a length, not a fraction | caught |
| 7 | OKLAB without the white scale | caught after the white check was added (a round trip cannot see it) |
| 8 | DKL azimuth turned the other way | caught |
| 9 | MULTIPLY without coverage | caught |
| 10 | the quad not grown for a moved primitive | caught |
| 11 | MSDF mean, not median | caught (after the test atlas got distinct channels) |
| 12 | range rule off | caught |
| 13 | extension data read at the next stimulus | caught |
| 14 | Wachspress colors shifted | caught |
| 15 | ellipse at 2 iterations | caught |
| 16 | Bezier chords at 0.5 px | caught |
| 17 | fillet convexity swapped | caught |
| 18 | exact blur with the wrong axis | caught |
| 19 | glyphs ignore the run scale | caught |
| 20 | ADD blend drawn as OVER | caught |
| 21 | one dash, not its neighbors | caught |
| 22 | deferred programs never finished | caught |
| 23 | star fold without the reflection | not on llvmpipe; caught on D3D11 hardware and WARP |
| 24 | ROUND polyline as boxes | caught |
| 25 | paint stops past the first ignored | caught |

### Departures from the design

- QBEZIER is chords within 0.005 px, not exact to 4e-5 px (compile time).
- The vector program misses four cost bars (above); open time is at the
  2.0 s bar, not under it.
- Dash ends with ROUND caps use the capsule distance across s and the
  boundary; on curves this is the design's "(1 + a / R) stretch".
- Dashes and trim on CROSS, masks and compounds are refused (no length).
- MITER and BEVEL in the vector program are on POLYLINE only; v0.2's
  MITER on RECT, CROSS and POLYGON is unchanged.
- CAPSULE's w is the full length, ends included (it fits its box).
- POLYLINE and QBEZIER take their width in shape_p[0], in units.
- A glyph record has no w, h: the run's scale and the atlas rectangle
  give the box.
- Trim and dash offset compare within tolerance, not bit for bit as the
  design asked (s is a float that the offset changes).
- A USER stimulus with a MASK_TEX aperture loses p[28..31] to the mask's
  rectangle.

### v0.3 builds

Warnings as errors everywhere (MSVC `/W4 /WX`; gcc `-Wall -Wextra
-Wpedantic -Wshadow -Werror`).

| Toolchain | Builds | Runs |
|---|---|---|
| MSVC 19.44, CMake (VS 2022), SDL3 | every target | ctest 40 of 40 pass; the gfx test on D3D11 hardware and WARP (CMake copies only `libEGL.dll` and `libGLESv2.dll`, so SwiftShader does not start there) |
| MSVC 19.51 (`cl`, VS 18) | the test as C11 and as C++17 | C++17 on SwiftShader through the Docker ANGLE folder: pass |
| MinGW-w64 gcc 16.1 | compile check and test, C99, C11, C++17 | not run |
| gcc 11.4, WSL2 | compile check and test, C99, C11, C++17 | C99 and C++17 on llvmpipe: pass |
| gcc 11.4, ASan and UBSan | the test | CPU half: pass; with llvmpipe (`detect_leaks=0`, Mesa's known 112 bytes): pass |
| WSL suite (rt, screen, audio, gfx, timeline, video) | `verify_all.sh` | all pass |
| emcc 6.0.10 (Docker) | compile check C11 and C++17, the test C11 | node: the CPU half passes |

### rig_spec 4.3

v0.3 stays inside "no vector drawing library": closed-form distance
functions, a bounded fold (56 primitives), bounded paths (224 points). No
SVG, no fill rules, no general path rasterizer. Artwork still goes
through the pack tool, now also as MSDF.

## v0.4: the program cache

### What ANGLE does with a binary

A probe on the Docker ANGLE (2.1.23876), 2026-10-06, under the lock:

| Fact | Iris Xe (D3D11) | WARP | SwiftShader |
|---|---|---|---|
| Binary formats | 1, 0x93A6 | 1, 0x93A6 | 1, 0x93A6 |
| The eleven programs' binaries | 456 KB (17 to 39 KB each, the vector program 112 KB) | 456 KB | 425 KB |
| `glProgramBinary` and the link status, all eleven | 2.8 ms | 9.3 ms | 0.4 ms |
| One byte flipped in the middle of a binary | linked: 10 of 11 programs | linked: 10 of 11 | linked: 11 of 11 |
| Half a binary under a valid header | the process ended (fast fail) | | |

GL_RENDERER carries the adapter and the driver version ("Direct3D11
vs_5_0 ps_5_0, D3D11-32.0.101.7088"), GL_VERSION the ANGLE build and git
hash. ANGLE does not check what it is given, so an entry carries its own
check, and the driver sees no byte that has not matched it.

EGL_ANDROID_blob_cache exists too. It was not used: ysp/screen.h owns the
EGL display, the blob cache is set once per display, and its corruption
handling is ANGLE's. `glGetProgramBinary` is core GL ES 3.0 and keeps the
key, the check and the fallback in this header.

### The entry and the key

The key is FNV-1a 64 of the vendor, renderer, version and GLSL strings,
the binary format, and the vertex and fragment text. The 48-byte header
holds the key, a second hash of the same material (a multiply-xorshift
hash), the material's length, the binary's format and size, and FNV-1a 64
of the binary. A load is used only when every field matches. A forced
collision (a test seam sets every key to 0) shows the second hash at work:
each program finds another program's entry, rejects it, compiles, and the
frame is bit-identical.

### Tests

`v0.4 cache` (GL, each renderer): a reference frame with no cache (a
gabor, a soft circle, an RRECT, a linear image, noise, a grating in a
circle, ordered dither), then eight opens with an in-memory cache, each
followed by the same frame, compared bit for bit, and the counters:

| Case | Loaded | Compiled | Rejected | Frame |
|---|---|---|---|---|
| cold | 0 | 11 | 0 | equal |
| warm | 11 | 0 | 0 | equal |
| a byte flipped in each payload | 0 | 11 | 11 | equal |
| warm again (the entries were rewritten) | 11 (and a user pipeline) | 0 | 0 | equal |
| every key 0, cold | 0 | 11 | | equal |
| every key 0, warm | 1 | 10 | 10 | equal |
| another driver identity | 0 | 11 | 0 | equal |
| a binary of another ANGLE build under a valid header | 0 | 11 | 11 | equal |
| the file cache: cold, warm, a folder that cannot be made | as expected | | | equal |

The CPU half runs everywhere with a backend whose binary is a hash of the
program's text: a changed vendor, renderer or version is a miss; a
flipped byte, a flipped magic and a truncation are rejects; a binary the
driver refuses is compiled; with every key 0 nothing is loaded; a user
pipeline is cached. The whole GL suite runs with an in-memory cache, so
every other pixel check also runs on loaded binaries.

### Open time

The probe's 2.8 ms for eleven loads became 11 to 31 ms for the whole open
(scene, CLUT, buffers, eleven loads). The table: fresh processes, five
rounds interleaved (none, cold, warm), the lock held. Run 1 on AC; the
WARP and SwiftShader runs on AC.

| Renderer | No cache | Cold (empty cache, stores) | Warm |
|---|---|---|---|
| Iris Xe | 2410 to 4569 ms | 2289 to 4678 ms | 11.4 to 22.3 ms |
| WARP | 1808 to 2666 ms | 1829 to 2541 ms | 17.8 to 30.7 ms |
| SwiftShader | 42 to 80 ms | 155 to 176 ms | 4.0 to 4.4 ms |

What a cold open adds, timed inside open (the reads of the binaries and
the stores), three runs each: an in-memory cache 1.3 to 2.6 ms on the Iris
Xe; the file cache 39 to 121 ms (eleven files). Bar: a cold open at most
5 % slower than none: met (at most 121 ms of 2.3 s or more). SwiftShader
pays more (133 to 347 ms): reading a binary back makes ANGLE's Vulkan
backend build what it would otherwise build at the first draw.

v0.4 against v0.3.0 with no cache, to check that the version-3 backend
and the cache's code cost nothing when no cache is given: fresh
processes, eight rounds interleaved, the lock held, 2026-10-06 09:56 to
09:58. **A loaded machine:** on battery, and another worker's build ran
during rounds 2 to 4, 7 and 8.

| Header | Open, ms (8 rounds) | Median |
|---|---|---|
| v0.3.0 (git HEAD) | 7489, 4603, 3384, 4122, 4010, 4743, 3524, 3231 | 4066 |
| v0.4 | 4751, 3305, 4008, 3743, 4355, 3646, 3370, 3475 | 3694 |

No regression. Both headers moved together from v0.3's 1.96 to 2.14 s
(measured on 2026-10-05); the machine moved. PROGRAM_BINARY_RETRIEVABLE_HINT
is set only when a cache is given.

## v0.4: video

### What ANGLE imports

A probe on the Iris Xe (2026-10-06): an NV12 texture on ANGLE's D3D11
device, imported plane by plane with `eglCreateImageKHR(EGL_D3D11_TEXTURE_ANGLE,
{EGL_D3D11_TEXTURE_PLANE_ANGLE, p})` and `glEGLImageTargetTexture2DOES`:
plane 0 an R8 texture, plane 1 RG8, read back exactly; slice 1 of a
2-slice array made with D3D11_BIND_DECODER through
EGL_D3D11_TEXTURE_ARRAY_SLICE_ANGLE, exactly. RGBA8 and BGRA8 textures
import with no attributes. The imported textures cannot be rendered to
(the framebuffer is incomplete); ysp_gfx only samples them.

### The conversion

Y' at the image point; chroma at the stated siting in its own plane
(texel-edge coordinates c = L / 2, plus 0.25 across for LEFT, both ways
for TOP_LEFT), bilinear by four texelFetch clamped to the plane, or
replicated; the matrix and range from constants the CPU puts in the
block; R'G'B' clamped; the transfer; the primaries' 3x3. At 1:1 the
weights are ysp/video.h's sixteenths: CENTER takes 9/16, 3/16, 3/16, 1/16;
LEFT takes the co-sited sample whole and the mean of two between them.

TRC_DEVICE: at open (and at each `ygfx_set_lut()`) a 256 x 3 table holds,
per gun, the linear value the CLUT maps to each code k / 255, inverted on
the CPU. The output stage then writes code k back: all 256 codes through
a nominal calibration's CLUT came out as they went in.

The video program is compiled the first time an encoded texture is made
or imported, so an open without video compiles nothing more and the
plain IMAGE program is the same text as v0.3's (plus the straight-alpha
fix).

### Pixel tests

`v0.4 video`, an RGBA32F scene against a double reference built from the
definitions (H.273's matrices and ranges, the transfers' formulas, the
luma equation for green, chroma weights from the siting), none of the
header's arithmetic. Frames of 36 or 37 x 23 or 24 texels (odd and even),
random codes with some 0 and 255 (out of range for LIMITED, so the clamp
is exercised).

| Check | Iris Xe | WARP | SwiftShader | Tolerance |
|---|---|---|---|---|
| NV12 and I420 x 3 matrices x 2 ranges x 3 sitings x 2 chroma filters, 1:1 (72) | 2.7e-7 | 3.1e-7 | 3.0e-7 | 1e-6 |
| each transfer (DEVICE, BT1886, SRGB, LINEAR, GAMMA22) | 4.4e-7 | 6.6e-7 | 7.5e-7 | 2e-6 |
| linear sampling at 2.5x | 6.2e-6 | 5.8e-6 | 5.5e-6 | 2e-5 |
| one plane's rectangle, then all planes, updated | 5.1e-7 | 6.6e-7 | 6.3e-7 | 2e-6 |
| BT.709 and BT.2020 primaries through a calibration (EDID chromaticities, nominal) | 1.1e-6 | 1.3e-6 | 9.8e-7 | 4e-6 |
| TRC_DEVICE: the 256 gray codes through a non-identity CLUT | 0 wrong | 0 wrong | 0 wrong | 0 |
| rebind: the other's texels, its own, its own after the other is freed | 5.1e-7 | 5.0e-7 | 6.3e-7 | 2e-6 |
| GL import of a texture's planes (the source survives the import's free) | 5.1e-7 | 5.0e-7 | 6.3e-7 | 2e-6 |
| D3D11 NV12 import, slice 1 of an array on ANGLE's device | 5.1e-7 | 5.0e-7 | (no D3D11) | 2e-6 |
| NV12 from a second D3D11 device by an NT handle and a keyed mutex | 5.1e-7 | 5.2e-7 | | 2e-6 |
| the same by a legacy shared handle, after the producer's flush and an event query | 5.1e-7 | 5.2e-7 | | 2e-6 |
| refusals (no encoding; primaries without a calibration; siting NONE on 4:2:0; texture_update on a planar texture; a video as a modulation; rebind across formats) | 6 of 6 | 6 of 6 | 6 of 6 | all |

The shared-handle cases are ysp/video.h's SHARED path: the producer
writes on its own device's immediate context; ANGLE's device opens the
texture by its handle; the consumer acquires key 1 before `ygfx_end()`
and releases key 0 after the frame. The test cannot show what happens
without the keyed mutex: the producer's write finishes long before ANGLE
samples. The order above is the documented contract, not a measured need.

### Cost, from a loaded machine

`gfx_bench --only "v0.4 video"`, 1920 x 1080 frames in a 1920 x 1200
scene, the lock held, 2026-10-06 09:58. **On battery, with another
worker's build running at the end.** GPU over an empty frame, the median
of 25 interleaved rounds.

| Workload | GPU, ms | CPU over empty, us |
|---|---|---|
| RGBA8, draw | 0.890 | 32 |
| NV12, BT.709, BT1886, draw | 1.227 | 48 |
| I420, the same, draw | 1.329 | 51 |
| RGBA8, upload + draw | 3.403 | 2117 |
| NV12, upload planes + draw | 2.290 | 755 |
| (second table) RGBA8, draw | 0.719 | 23 |
| NV12, transfer LINEAR | 1.125 | 32 |
| NV12, transfer BT1886 | 1.111 | 25 |
| NV12, transfer DEVICE | 1.275 | 21 |

The update call alone, 400 calls each, A/B/A/B: RGBA8 (8.3 MB) 1107 to
1253 us mean, 1848 to 2192 us p99; NV12 (3.1 MB) 360 to 362 us mean, 535
to 556 us p99; I420 367 to 402 us mean. `ygfx_texture_rebind()`: 0.001
us a call.

The knob for the GPU time. Chroma costs 8 texelFetch a pixel for NV12
and 16 for I420 (the hand bilinear, kept for exactness), and DEVICE 3
more. The sampler's own bilinear (one `texture()` a plane) would remove
most of them. Its weights: at 1:1 with the supported sitings the chroma
sample point falls on fractions 0, 1/4, 1/2 or 3/4 of a texel (CENTER: 1/4
and 3/4 both ways; LEFT: 0 and 1/2 across, 1/4 and 3/4 down; TOP_LEFT: 0
and 1/2), which a sampler's 8-bit subtexel weights hold exactly, so the
fast path would give the same weights at 1:1. Scaled, its weights are
quantized to 1/256: an error of up to 1/512 of the step between two
chroma samples, under 1/255 of a code. The filter's own arithmetic on R8
texels was not checked. Not built (the coordinator's decision, 2026-10-06).

## v0.4: instances

### Design

A template stimulus (one uniform block) and an array of 32-byte
`ygfx_inst` records, all float so `ygfx_bind.field` binds any of them,
drawn in one instanced draw. draw() copies the records into the frame's
element staging; end() uploads them in one call into one of three element
buffers (the ring of the uniform blocks) and draws with two vec4
attributes, the glyph runs' layout. The template's block is followed by
two more: the anchor in the pass's GL px and the center's offset from it,
px per unit, the field mask, and the palette.

The vertex shader computes each element's center and axes and hands the
fragment shader flat replacements for the placement, look, color or dir
and misc. A built-in body sees the template's frame at p / k (k the
element's scale) and every distance is multiplied by k: a uniform scale is
exact for any SDF, and what is in px (edge width, stroke, offset) is added
after, so it does not scale. A user body sees its own px and ysp_size
scaled, as a group of that scale gives it.

D3D's `sin` and `cos` are coarse (v0.3), so an element's angle comes from
a polynomial in degrees: the turn reduced to a quarter, then degree-9 and
degree-8 Taylor terms. Run in float on the CPU, as the shader runs it,
from -720 to 720 degrees in steps of 0.001 degree: within 4.3e-7 of
double.

### Timeline bindings and the hit test

A template field moves every element (a `ygfx_bind` on the instanced
stimulus, which holds the template's fields); `ygfx_bind.field =
&items[k].gate` moves one element. No new binding kind was needed.
`ygfx_hit_index()` builds element i as its own stimulus (the template in
a one-point group at the element's anchor, scaled and turned there) and
runs the existing hit test on it, from the highest index down, skipping
gate 0: the topmost element under a pixel, by the same field the GPU
draws.

### Tests

`v0.4 inst`: each element against the same stimulus drawn alone, which
the test composes itself (a one-point group at the element's anchor with
its scale and turn, the fields applied); an RGBA32F scene, largest
difference of full scale:

| Template | Iris Xe | WARP | SwiftShader | Tolerance |
|---|---|---|---|---|
| gabor, every field (dir from the palette); two arrays in one frame | 8.3e-7 | 8.3e-7 | 8.3e-7 | 3e-5 |
| grating in a cosine-edged circle; noise | 2.9e-6 | 3.4e-6 | 3.3e-6 | 3e-5 |
| stroked CROSS (v0.2 program); vector RRECT with an offset | 9.5e-7 | 1.0e-6 | 1.0e-6 | 3e-5 |
| linear image tinted by the palette | 9.5e-7 | 9.5e-7 | 9.5e-7 | 3e-5 |
| USER at scale 0.5 to 1.5, turned, about an anchor at (0.3, 0.6) | 9.1e-6 | 9.2e-6 | 9.1e-6 | 3e-5 |
| palette: integer entries (bit for bit); fractions against double | 0 wrong; 1.1e-7 | 0; 1.1e-7 | 0; 1.1e-7 | 0; 2e-7 |
| `ygfx_hit_index()` against the GPU: 16 overlapping turned and scaled rects, one with gate 0 | 0 of 24712 wrong (4 ties) | same | same | 0 |
| bindings: a template field, one element's field | 0 wrong | | | |
| refusals: a DOTS template, PHASE on a shape, COLOR without a palette, SCALE on NOISE, a template with extension blocks, more than max_instances, 9000 + 9000 elements in one frame | 8 of 8 | 8 of 8 | 8 of 8 | all |

The USER case is the coordinator's condition: the user's shader sees the
same local coordinates whether drawn as an element or alone.

## v0.4: the draw order

### What interleaving cost

1000 stimuli, four kinds in turn (gabor, soft circle, grating in a circle,
RRECT), on a grid, no overlap, against the same stimuli grouped by kind by
the caller (2026-10-06 10:36, AC, the lock held, no build running):

| Workload | GPU over empty, ms | CPU over empty, us |
|---|---|---|
| interleaved, call order | 5.520 | 2239 |
| grouped by kind | 0.470 | 124 |
| interleaved, 10 % overlapping their neighbor | 5.517 | 2217 |

So batching by call order gave interleaved kinds about one draw each, and
about 5 us of GPU a draw through ANGLE's D3D11 backend. Built.

### The rule

In end(), within each pass, a draw joins the latest earlier batch of its
kind (the same program, textures and blend, room for one more) when its
bounds meet none of the batches between. The commands and their blocks
are then written in the new order to a second staging, the members of a
batch next to each other, and the two are swapped; nothing is allocated
in the frame. The bounds: the draw's own quad, center +- (half size +
margin) along its turned axes, plus 1 px; every fragment a draw writes is
in its quad (soft edges, strokes, offsets and effects grow the margin).
Instanced draws, dot fields and glyph runs place their pieces anywhere:
their bounds are the whole pass, so nothing passes them. A draw looks
back at most 64 batches.

Two draws with disjoint bounds write no common pixel, and blending reads
only the pixel it writes, so their order cannot change a value, for any
blend: ADD (the gabors) moves too, and the result is bit-identical (the
coordinator first excluded ADD; the argument was accepted). Target passes
keep their order: each is its own segment. A group needs no barrier: it
is composed into each draw's block on the CPU and sets no GL state.

### Tests

`v0.4 order`: each frame drawn in call order (the seam `no_reorder`) and
reordered; the scene (RGBA32F) and the output codes (ordered dither)
compared bit for bit.

| Scene | Frames | Differed |
|---|---|---|
| sparse grids of 160 stimuli of five kinds (gabor, soft circle, RRECT, grating, linear image), turned | 3 | 0 |
| dense: 400 at random places, most overlapping | 3 | 0 |
| a draw that moves past two batches to join an earlier run, and one that may not pass a gabor three draws back | 1 | 0 |
| a Gaussian-edged circle next to an RRECT, the quads from overlapping by 6.5 px to apart by 2.5 px | 9 | 0 |
| an RRECT 40 x 40 at 45 degrees whose corner meets a circle's fringe, a copy of it far away | 4 | 0 |

On all three renderers; 520 to 559 draw calls saved over the 20 frames.
The two-batch move is checked by count: 7 draws become 5, not 4.

### Cost

`gfx_bench --only "v0.4 mixed"`, 2026-10-06 11:24, AC, the lock held, no
build running during the part:

| Workload | GPU over empty, ms | CPU over empty, us |
|---|---|---|
| 1000, 4 kinds interleaved, call order | 7.197 | 3335 |
| the same, reordered | 1.133 | 467 |
| the same, grouped by kind by the caller | 0.574 | 170 |
| 10 % overlapping, call order | 7.305 | 3336 |
| 10 % overlapping, reordered | 1.158 | 450 |
| 1000 at random places (dense), call order | 6.937 | 3294 |
| the same, reordered | 1.624 | 642 |

The reorder saves 6.1 ms of GPU and 2.9 ms of CPU a frame here; its own
cost is inside the 467 us (the grouped row shows what is left to gain: a
caller who groups by kind still draws fewer, larger batches, because a
batch's union bounds grow and stop later draws). Kept.

## v0.4: the gallery's findings, and an audit

Found by the gallery worker (examples/gfx/gallery.c), fixed, each with a
test and a mutation:

| Finding | Fix |
|---|---|
| `ygfx_cal_nominal()`, `cal_set_spectra()`, `cal_derive()`, then open: "CRC mismatch" until a `cal_save()` | derive() seals the CRC; `cal_add()` and `cal_set_spectra()` unseal it. The test runs the manual's recipe as written, and refuses a reading changed after derive |
| A straight-alpha sprite drawn linear at 16x got dark fringes | the color image program filters straight-alpha RGBA and RG texels as premultiplied, then divides back; `image_desc.premultiplied` states a premultiplied texture. 1.0e-7 (Iris Xe), 6.3e-8 (WARP), 1.3e-7 (SwiftShader) from the premultiplied bilinear in double; v0.3's way was 0.196 off |
| Glyph runs and masks on a DIST atlas clipped silently | the range rule holds on DIST atlases with `texture_desc.sdf_range` (twice the padding); only the outer reach is checked (inside, a DIST atlas holds true distances). A glyph run or a src rectangle on a DIST atlas with no range is refused. A whole-texture DIST mask without a range is v0.2's, unchecked |
| A glyph run needed its own buffer | `first` on glyph and dot descs: a run from any record of a shared buffer; bit-equal to its own buffer |
| An aperture's shape_p had to be set after construction | `shape_p[4]` on the grating, noise and user descs; bit-equal to setting it by hand |

Found here, by the dots' mutation passing: **`ygfx_dots_desc.aperture`
0 is YGFX_RECT, so a dot field with no aperture set had a 0 x 0 RECT
field and every dot was culled**; the manual said the default was no
aperture. Since v0.1. `gfx_bench`'s dots rows (above, "Frame cost") and
`gfx_load`'s "10000 dots" run drew nothing; their numbers are void. The
gallery set its apertures and was not affected.

The audit of every constructor for the same trap (a nonempty stimulus
that draws nothing by a zero default or a zero field):

| Constructor | Zero default or field | Now |
|---|---|---|
| dots | aperture 0 = RECT, w 0 | no aperture (the manual's default); an explicit RECT or CIRCLE of no area, or dot_size 0 with dots, refused |
| dots, glyph runs | count 0 | legal, draws nothing, returns OK (v0.3: ERR_ARG with no message) |
| shape POLYGON | w, h 0 with vertices | the box is the vertices' own (they are from its center) |
| shape CIRCLE, RECT, CROSS, LINE, ANNULUS; grating, noise, user | w (or LINE's width, CROSS's arm, ANNULUS's ring) 0 | refused with a message naming the field (v0.3 drew nothing) |
| image | w, h 0 with no gfx to take the texels' size from | refused |
| gabor | sigma 0 | refused with a message (v0.3: ERR_ARG with none) |
| vector kinds, compounds, paths | w, h, widths 0 | refused already (v0.3) |
| instances | a refused `ygfx_instances()` | draws nothing (v0.3 would have drawn the template once) |

Every `return 0` in the packing now names its reason in `ygfx_error()`
(a missing texture, buffer or pipeline; a src rectangle outside its
texture; a mask's format or aspect; MITER where it does not apply). Test:
14 cases, each refused with a message or drawn as the manual says; a
polygon with no box drew 660 px, dots with no aperture 80 px.

## v0.4 cost

All on the Iris Xe at 1920 x 1200, RGBA16F scene, the lock held,
`gfx_bench`. Three sessions, each named by its conditions:

- A: 2026-10-06 10:36, AC, no build running (instances, mixed kinds).
- B: 2026-10-06 11:19 to 11:26, AC, the re-run of every bar; a build
  ran during open rounds 2 and 4 (the run paused while one was seen) and
  at the end of the instances part.
- C: 2026-10-06 11:27, AC, no build seen, but every row about 2.3 times
  B's, the empty frame included: an unknown load. Given for the ratios.

### Open

| Session | No cache | Cold | Warm |
|---|---|---|---|
| B, five rounds | 2346 to 5287 ms | 2218 to 7590 ms | 11.5 to 35.0 ms |
| earlier, AC (see "Open time") | 2410 to 4569 ms | 2289 to 4678 ms | 11.4 to 22.3 ms |

Bars: warm at most 100 ms: met. Cold at most 5 % over none: met by the
store's own time (32 to 120 ms of a 2.2 s or longer open).

### Video, 1920 x 1080 (session B)

| Workload | GPU over empty, ms | Ratio to RGBA8 | CPU over empty, us |
|---|---|---|---|
| RGBA8, draw | 0.555 | | 6 |
| NV12, BT.709, BT1886, draw | 0.908 | 1.64 | 11 |
| I420, the same | 0.935 | 1.68 | 10 |
| RGBA8, upload + draw | 1.898 | | 955 |
| NV12, upload planes + draw | 1.417 | 0.75 | 355 |
| (transfers table) RGBA8, draw | 0.585 | | 12 |
| NV12, LINEAR | 0.954 | 1.63 | 14 |
| NV12, BT1886 | 0.971 | 1.66 | 18 |
| NV12, DEVICE | 1.119 | 1.91 | 17 |

The update call alone, A/B/A/B: RGBA8 906 and 1026 us mean (1631, 2351 p99);
NV12 304 and 389 us (424, 799); I420 316 and 300 us (524, 476): 30 to 38 %
of RGBA8's. Bars: update at most 50 % of RGBA8's: met. GPU at most 1.0 ms:
met by NV12 and I420 with BT1886 and LINEAR (0.91 to 0.97 ms), missed by
DEVICE (1.12 ms). GPU at most 1.5 times RGBA8: missed (1.63 to 1.91). The
knob is the chroma fetches (above, "Cost, from a loaded machine"). A
decoded 1080p NV12 frame uploaded and drawn takes 1.4 ms of GPU and 0.36
ms of CPU: it fits a 16 ms frame with room for the rest.

### Instances

| Workload | GPU over empty, ms: A / B / C | CPU over empty, us: A / B / C |
|---|---|---|
| 10000 gabors 32 x 32, one stimulus each | 5.54 / 5.53 / 14.3 | 2804 / 3089 / 8321 |
| the same, instanced (ring of 3 element buffers) | 1.26 / 1.33 / 3.13 | 177 / 191 / 527 |
| the same, one element buffer orphaned each frame | 1.28 / 1.44 / 3.21 | 191 / 211 / 384 |
| instanced, every element's ori set each frame | 1.38 / 1.52 / 3.25 | 167 / 244 / 425 |
| 10000 LINE 16 px, instanced | 0.54 / 0.62 / 1.37 | 258 / 218 / 498 |
| 10000 RRECT 14 x 9 (vector), instanced | 1.62 / 1.70 / 4.10 | 152 / 212 / 466 |
| 1000 gabors 256 x 256, one stimulus each | 8.61 / 10.7 / 24.1 | 263 / 306 / 646 |
| the same, instanced | 4.59 / 5.87 / 11.9 | 6 / 10 / 83 |
| `ygfx_hit_index()` over 10000 RECT elements | 546 us (A), 1381 us (B, a build ran), 702 us (C) | |

Bars (A and B): CPU at most 0.2 ms for 10000: met (177, 191 us); GPU at
most 2 ms and at most one stimulus each: met (1.26 to 1.33 against 5.5);
1000 large gabors within 5 % of one stimulus each: better, 0.53 to 0.55 of
it; hit test at most 1 ms: met in A and C. The 1000 large gabors cost
about half instanced: a likely cause is that the batched path indexes
the uniform array per fragment while an element's values arrive as flat
inputs; not confirmed. The ring and orphaning were equal within the
noise in all three sessions (the ring 2 to 8 % faster on the GPU): the
ring was kept and the orphaning seam deleted. 10000 elements of any of
these kinds fit a 16 ms frame.

### Dots, re-measured (session B)

The rows of "Frame cost" drew nothing (above). Now drawn, `gfx_bench
--only dots`, 120 frames back to back, GPU per frame including the empty
frame (1.0 to 1.1 ms in the same session):

| Workload | CPU mean, us | CPU p99, us | GPU, ms |
|---|---|---|---|
| dots x 1000, 4 px, upload every frame | 198 | 370 | 0.93 |
| dots x 10000 | 171 | 393 | 1.30 |
| dots x 100000 | 1301 | 1876 | 4.98 |

Bars (v0.1's): 10000, GPU at most 0.5 ms over empty: met (about 0.2);
100000, CPU at most 2 ms: met; GPU at most 3 ms: missed (about 3.9 over
empty). `gfx_load`'s window run was not re-measured (it needs the panel,
which the photodiode setup is using).

### No regression (session B)

| Row | v0.3 (2026-10-05) | v0.4 |
|---|---|---|
| empty frame, identity CLUT (GPU per frame) | 0.46 to 1.15 ms | 1.08 to 1.11 ms |
| 1000 gabors 256 x 256, batched (GPU per frame) | 9.3 to 11.2 ms | 9.20, 9.45 ms (25.3 ms once, in a session where everything was slow) |
| v0.2 rounded rect, RRECT, RRECT with OKLAB paint, dashed stroke, compound of 8 (GPU over empty) | 0.396, 0.929, 1.536, 0.903, 3.528 ms | 0.440, 0.967, 1.629, 0.983, 3.648 ms |
| a run of 200 MSDF glyphs | 0.121 ms | 0.122 ms |

The v0.3 rows moved by 1 to 11 % together, the v0.2 rect (whose program
did not change) as much as the others: the machine, not v0.4.

## v0.4 mutations

[tests/mutate/gfx.toml](../tests/mutate/gfx.toml), run with
`uv run tests/mutate/mutate.py gfx.toml` ([tests/mutate](../tests/mutate/README.md)):
each fault in a copy of the header, the test built against it (MSVC) and
run on the Iris Xe (the CPU half alone for the CPU faults); a fault must
fail the test. Faults 14 and 24 now edit ysp/color.h, where v0.5 moved
that code.

| # | Fault | Result |
|---|---|---|
| 0 | the payload hash not checked | caught |
| 1 | the key without GL_RENDERER | caught (after the fake backend's renderer string was varied) |
| 2 | the key without GL_VERSION | caught (same) |
| 3 | block and sampler bindings not set after a binary load | caught |
| 4 | a binary the driver refuses not compiled | caught |
| 5 | the second hash and the length not checked | caught |
| 6 | Cb and Cr swapped | caught |
| 7 | limited range without its 16 / 219 offset | caught |
| 8 | LEFT siting as CENTER | caught |
| 9 | chroma replicated when sited | caught |
| 10 | no clamp before the transfer | caught |
| 11 | BT.709 weights for BT.601 | caught |
| 12 | the display's transfer half a code off | caught |
| 13 | rebind ignored | caught |
| 14 | the primaries' matrix in the wrong order | caught |
| 15 | an element's ori turned the other way | caught |
| 16 | an element's phase not added | caught |
| 17 | the field not rescaled at an element's scale | caught |
| 18 | a palette fraction ignored | caught |
| 19 | element offsets along the screen's axes, not the template's | caught |
| 20 | an element's gate not multiplied | caught |
| 21 | the vector program's look taken as the others' | caught |
| 22 | two element arrays read at one offset | caught (after a two-array check was added) |
| 23 | `ygfx_hit_index()` from the lowest index | caught |
| 24 | the CRC not sealed by derive | caught (after the check compared the CRC itself) |
| 25 | straight alpha filtered as v0.3 did | caught |
| 26 | the DIST range rule off | caught |
| 27 | dot fields ignore first | caught (after the dots bug was found and fixed: its test drew nothing) |
| 28 | the reorder ignores overlap | caught |
| 29 | the reorder checks only its neighbor batch | caught |
| 30 | bounds without the quad's margin | caught |
| 31 | bounds without the turn | caught (after the turned-box case was added) |
| 32 | a stimulus of no area drawn as nothing | caught |
| 33 | a dot field's zero default as a 0 x 0 RECT | caught |
| 34 | a refused element array draws its template | caught (after the scene was checked) |

35 of 35.

## v0.4 builds

Warnings as errors. MSVC 19.44 (CMake, build-gfx; and `cl` alone): the
test as C and as C++17, the compile checks, `gfx_bench`, the examples and
`test_video`; the full test on the Iris Xe, WARP and SwiftShader.
MinGW-w64 gcc 16.1 (winlibs): the test as C99, C11 and C++17, the
ysp_gfx compile checks (C11, C++17) against SDL3, ysp_video's C11 compile
check, `gfx_bench`; the C99 and C++17 tests run on WARP and SwiftShader.
Run on 2026-10-06 against v0.5.0: gcc 13.3 in WSL2 (Ubuntu 24.04), the
test at -O2 and under ASan and UBSan (`detect_leaks=0`,
`YGFX_TEST_DEVICES=mesa`), on Mesa 25.2 llvmpipe (LLVM 20.1.2): pass.
The cache's GL half passes on Mesa's binary format (11 cases, wrong 0),
but a warm open was not faster there: 80 ms against 54 ms cold at -O2.
The same tree in an Ubuntu 24.04 container with gcc 13.3 and clang 18.1
(the CI jobs' flags, SDL3 3.4.0): every target builds and ctest passes.
emcc 6.0.10 under node: the compile check and the test's CPU half pass.

## v0.4 departures from the design

- Element bindings: no new binding kind was needed (the template's fields and
  `ygfx_bind.field`).
- Templates: USER without extension blocks, as approved; the design had
  "any kind except USER with extension blocks".
- The palette holds 16 entries, as approved.
- The reorder, the audit and the gallery's fixes were not in the design;
  they were asked for during the work.
- No kind-specialized vector programs (below).

## v0.5: the calibration moved to ysp/color.h

The calibration (`ycol_cal`, the same .yspcal bytes), its calls, the
cone table and PAINT's and VIDEO's color math are ysp/color.h's now;
docs/color.md has the design and the stages. ysp/gfx.h keeps the CLUT
and transfer textures, the output stage, dithering, every shader, the
per-draw gamut check and `ygfx_clipped()`.

| Item | State |
|---|---|
| Stage A: the code moved, aliases for callers | Readback hashes of the whole test equal v0.4.0's on the Iris Xe, WARP and SwiftShader. |
| Stage B: callers renamed, aliases deleted | Hashes equal again. ysp/video.h needed no change. |
| Stage C: a `ycol_ctx` at open; `desc.cones`, `desc.lum`, `ygfx_color()`, `YGFX_EV_COLOR` | v0.4's parts: hashes equal to stage B's on all three. |
| OKLAB on absolute XYZ, the black included | The shader adds the row's w for OKLAB as it did for DKL_POLAR. A black without light changes no pixel. On a calibration with 0.5 cd/m2 of black under 80 cd/m2 of white, the test's gradient is within 6.98e-7 / 7.28e-7 / 7.28e-7 (Iris Xe / WARP / SwiftShader) of an independent double reference; v0.4's form would have moved it by 5.20e-3. |
| A calibration that is not sealed | Refused at open (ysp/color.h's context checks the CRC); derive() and save() seal it, as since v0.4. |

The CPU form of OKLAB back to RGB (the clip sampling of a gradient) keeps
Ottosson's printed 10-digit inverse of M2, as the shader does; ysp/color.h
inverts M2 exactly, 2.4e-7 away in linear RGB.

## v0.6 status

The running record of v0.6: curve runs (Slug), the blur pass, the video
program's chroma. The design is in the session notes of 2026-10-06; the
coordinator approved it with changes (the names `ygfx_crun`,
`ygfx_citem` and `ygfx_blur_apply`, int codes from `ygfx_blur_make`,
format flag bit 2).

| Item | State |
|---|---|
| 1. The format, `ygfx_cset_check()`, `ygfx_cset_winding()`, the test vector | Done, in the tree first so the outline builder could test against it. Flag bit 2 (resolved) added on the outline worker's request. |
| 2. Curve runs | Done, tested on 3 renderers. Rays with the half-plane coverage (V1); the exact area (V3) on resolved glyphs that are not turned. Not built: V2 (a corner rule) and screen-aligned rays (below). |
| 3. The D3D11 difference of the probe | Cause found: interpolated em coordinates (below). |
| 4. Blur pass | Done, tested on 3 renderers, 1x and 2x, three formats. The pyramid above sigma 8: not built (Next). |
| 5. Sampler-filtered chroma | Measured, then deleted: no faster than the hand bilinear (slower). The video bars are met by passing the program's constants as flat inputs. |
| 6. Bench on AC | Done ("v0.6 cost"). |
| Mutations | 29 of 29 caught ("v0.6 mutations"). Two survived the first run and showed a bug: a curve that turns back before a ray counted as a crossing (fixed, below). |
| Build matrix | See "v0.6 builds". |
| Left | gfx_bench on the outline builder's header (its stopgap `glyf` reader then deleted); Next items 4 to 8. |

## v0.6: curve sets

A curve set is one font or one artwork set in the form of Lengyel's Slug
algorithm (JCGT 6(2), 2017): quadratic Bezier curves and bands. ysp/gfx.h
draws it; the pack tool's outline builder writes it. This header implements
the algorithm from the paper. No code comes from the reference shaders.

### Curve sets: the format

Version 1, frozen on 2026-10-06. This section is the contract between the
outline builder and ysp/gfx.h. `ygfx_cset_check()` checks a set against
it on the CPU, without GL.

**Two arrays.** A set is two flat arrays:

- `texels`: `n_texels` texels of four little-endian IEEE f32 values each.
- `words`: `n_words` little-endian uint32 values. `n_words` is a multiple
  of 4.

ysp_gfx uploads them as one RGBA32F and one RGBA32UI texture. The texture
layout is not part of the format. The pack stores the two arrays as they
are.

**Coordinates.** Glyph coordinates are in em units (font units divided by
unitsPerEm). Artwork coordinates are in the artwork's own units. y points
down. The origin is the glyph's origin, the pen position on the baseline,
so an ascender has negative y. A curve run's `size` converts set units to gfx
units.

**Words 0 to 7, the set header.**

| Word | Value |
|---|---|
| 0 | `0x43505359` ("YSPC" as little-endian bytes) |
| 1 | the format version, 1 |
| 2 | G, the number of glyph table entries, below 2^24 |
| 3 to 7 | 0 (reserved) |

**Words 8 to 8 + G - 1, the glyph table.** `words[8 + g]` is the word offset
of glyph g's record, or 0 when the set does not hold glyph g. A curve run item
that names an absent glyph draws nothing.

**A glyph record** starts at a word offset `o` that is a multiple of 4 and
is past the table:

| Word | Value |
|---|---|
| o to o + 3 | the bbox x0, y0, x1, y1 as f32 bits; it contains every control point of the glyph's listed curves |
| o + 4 | `nh | (nv << 16)`: the horizontal and vertical band counts; both 0 means an empty glyph |
| o + 5 | flags: bit 0 even-odd fill (else nonzero), bit 1 backward lists, bit 2 resolved (below); the other bits 0 |
| o + 6 | first: the glyph's first curve texel |
| o + 7 | n: the number of the glyph's texels, `first` to `first + n - 1` |
| o + 8 on | band descriptors, 4 words each: the nh horizontal bands, then the nv vertical bands |

A non-empty glyph has `x1 > x0`, `y1 > y0`, `nh >= 1` and `nv >= 1`. A band
descriptor is `(fwd_offset, fwd_count, bwd_offset, bwd_count)`. Each offset
is the absolute word index of a list of `count` words. Without flag bit 1,
`bwd_offset` and `bwd_count` are 0. A list can start at any word, and two
bands can share a list.

**A list entry** is a curve reference: the index `r` of the curve's first
texel, with `first <= r` and `r + 2 <= first + n`.

**Curves.** Curve r has the control points `p0 = (T[r].x, T[r].y)`,
`p1 = (T[r].z, T[r].w)` and `p2 = (T[r + 1].x, T[r + 1].y)`. A contour of k
curves takes k + 1 consecutive texels, because curve j + 1 starts at the
texel that holds curve j's p2. The last texel of a contour holds the
contour's start point again (bit for bit) in x, y, and 0 in z, w. Contours
must be closed. There is no contour table: bands list curves.

- A line is a quadratic with `p1 = p0`, bit for bit. This is exact in f32,
  and the shader needs no special case for it.
- All values are f32. An f16 curve texture failed for Arabic, Devanagari
  and CFF glyphs (docs/text_probe.md).

**Bands.** Horizontal band k of a glyph, k = 0 to nh - 1, covers y from
`lo_k = y0 + k h` to `hi_k = lo_k + h`, with `h = (y1 - y0) / nh`, in double.
A curve is in band k when its y range meets `[lo_k - h / 256, hi_k + h / 256]`.
Use the range of the three control points, or the curve's exact y extent.
The margin of h / 256 covers the shader's f32 rounding when it selects a
band. Vertical bands are the same with x and y interchanged.

**Sort order.** The shader stops at the first curve that cannot cross its
ray, so the order is part of the format:

- a horizontal band's forward list: descending `max(p0.x, p1.x, p2.x)`;
- a vertical band's forward list: descending `max(p0.y, p1.y, p2.y)`;
- backward lists: ascending `min` of the same coordinates;
- ties: any order.

**Fill rules.** Nonzero or even-odd, per glyph (flag bit 0). Contour
direction matters for nonzero only, as in SVG. ygfx_cset_winding()
counts +1 inside a contour that turns clockwise on the screen.

**Resolved glyphs.** Flag bit 2 states that the glyph's contours do not
overlap and that its winding is 0 or 1 everywhere (+1 inside a contour
that turns clockwise on the screen). The outline builder sets it after it
resolves overlaps. The shader can then use a method that is exact only on
such glyphs. `ygfx_cset_check()` accepts the bit and does not verify it,
because that needs a sweep of every glyph. A bit set on a glyph that is
not resolved gives wrong coverage near the overlaps. It cannot make the
shader read outside the textures or stop.

**Glyphs added at run time.** The builder appends the glyph's texels and
words, then sets its table entry. `ygfx_cset_add()` uploads what is new.
The textures get their size at `ygfx_cset_make()` from `cap_texels` and
`cap_words`.

**What the check refuses.** `ygfx_cset_check()` returns
`YGFX_ERR_FORMAT`, with a message that names the glyph and the word, when:

- the magic, the version or a reserved word is wrong;
- the table does not fit in the words, or G is 2^24 or more;
- a texel is not finite;
- a record offset is not a multiple of 4, is not past the table, or does
  not fit in the words;
- the bbox is not finite or not ordered, or the curves pass it;
- reserved flag bits are set, or only one band count is 0;
- the texel range does not fit in the texels;
- a descriptor or a list does not fit in the words, or a backward list has
  no flag;
- a reference is outside its glyph's texels;
- a list is not sorted.

The check does not find contours that are not closed. It takes one pass over
both arrays.

**Test vector.** A square from 0 to 1 em with a square hole from 0.25 to
0.75 em, nonzero, 2 x 2 bands. The outer contour turns clockwise on the
screen, the hole the other way. Texels (x, y, z, w):

```
0: 0 0 0 0           1: 1 0 1 0           2: 1 1 1 1
3: 0 1 0 1           4: 0 0 0 0           5: 0.25 0.25 0.25 0.25
6: 0.25 0.75 0.25 0.75                    7: 0.75 0.75 0.75 0.75
8: 0.75 0.25 0.75 0.25                    9: 0.25 0.25 0 0
```

Words (60):

```
0x43505359 1 1 0 0 0 0 0 12 0 0 0
0 0 0x3f800000 0x3f800000 0x00020002 0 0 10
36 6 0 0  42 6 0 0  48 6 0 0  54 6 0 0
0 1 7 8 5 3   1 2 6 7 5 3   2 3 5 6 8 0   1 2 6 7 8 0
```

`tests/adapt/gfx_test.c` builds this set with the test builder
(`tests/adapt/gfx_cset_build.h`) and compares every word.

### Curve runs: what the shader does

One program, made at the first curve set. The vertex shader reads the
glyph's record from the word texture, builds the item's map from em to
the pass's GL px (the run's placement, the item's ori and scale about the
glyph box's center), and emits the turned glyph box grown by 0.75 px (half
a pixel's diagonal, rounded up). The fragment shader computes the pixel's
em point from `gl_FragCoord` through that map's inverse, relative to the
glyph box's center (so a glyph far from the origin keeps every bit near
the pixel), and then one of two methods:

- **Exact area (V3).** When the glyph is resolved (flag bit 2) and the map
  is axis-aligned (no turn, or quarter turns), the pixel is a rectangle in
  em. Its coverage is the area of glyph intersect pixel over the pixel's
  area: by Green's theorem the sum over curves of the integral of
  clamp(x, 0, W) d clamp(y, 0, H) in pixel-corner coordinates. Only curves
  in the bands the pixel's rows meet count, each band integrating its own
  part of the rows. A curve right of the pixel adds W times its clamped
  rise (no root); one left of it nothing (the early exit); one over the
  pixel is cut where it turns in x or y, and on each monotone piece the
  part inside the rows splits into left (nothing), middle (two Gauss
  points, exact for the cubic integrand) and right (W times the rise).
  With backward lists the integral of clamp(x) - W counts the curves left
  of the pixel instead: a closed contour's rise sums to 0.
- **Rays (V1, after Lengyel).** Two rays from the pixel center along em +x
  and +y through their bands. The paper's rule finds the crossings: the
  signs of the three control values give which roots lie on the curve
  (above: v > 0, so an endpoint on the ray counts once), and the roots come
  from the form without cancellation (q / a and c / q). A row where both
  endpoints are on one side and the control point on the other has two
  roots, or none when the discriminant is negative (see "A bug the
  mutations found"). A crossing within 1 px adds the exact box coverage of the
  half-plane bounded by the curve's tangent there (the CDF of the pixel
  square's projection on the normal, a trapezoid), so a straight edge at
  any angle is exact; farther ones add a step. Each ray's weight is the
  largest `cos x (1 - |d| / half-support)` of its crossings, where cos is
  between the ray and the edge's normal: a ray counts most where it
  crosses an edge steeply, and a weight falls to 0 where the coverage
  saturates. The fill rule maps each ray's fractional winding, and the two
  mix by weight; below a total weight of 1/64 the mix fades to their mean,
  where both rays see the same integer winding.

The two rays run as one function called twice. A first form ran them in
one loop (so that ANGLE's D3D11 compiler would inline the body once, the
v0.2 lesson): 33 % slower on the Iris Xe (10.2 against 6.9 ms on a 12 px
page), deleted.

### Curve runs: measured, then deleted or not built

| Choice | Measured | Kept |
|---|---|---|
| V0, Lengyel's linear ramp over the window | mean edge error 0.031 to 0.074 at 8 px (worst at 45 degrees), 0.013 to 0.081 at 48 px; 1.0 ms less than V1 on a 12 px page (5.9 against 6.9 ms, one-loop rays) | V1: the error at 45 degrees falls 3 to 24 times |
| V1, the half-plane per crossing | mean 0.024 to 0.029 at 8 px, 0.0034 to 0.0037 at 48 px, at every rotation | yes |
| A fade from the half-plane to the ramp within 1 px of a curve's end (to remove the corner's jump) | the jump stayed (0.49): it comes from a ray passing a corner, not from the tangent; mean error up 30 % at 45 degrees; the largest error off the edges 0.25 to 0.078 | deleted |
| V2, a corner rule | not built: V3 makes corners exact where it applies, and the rotated corners' error is the rays' | Next |
| V3, exact area | within 1.8e-5 on resolved glyphs; 1.4 times the rays on a 12 px page, faster than them at 200 px | yes, where it applies |
| Screen-aligned rays | not built (the design's analysis: a 45 degree edge gets a ramp 0.71 px wide against the true 1.41; V1 is exact there for either ray direction; they would need every curve of the glyph) | no |
| The combination with a 1/65536 floor (the suspected D3D11 cause) | no difference across renderers from the continuous one when em comes from gl_FragCoord | the continuous mix |
| Band count (8 or 16 against the builder's default, the curve count's square root up to 16) | 12 px page with V3: default 8.4 ms, 8 bands 10.4, 16 bands 24 (the pixel's rows meet more bands); rays: 16 bands 2 % faster | the default |
| Backward lists | rays 18 to 27 % faster on pages (12 px Latin 6.6 to 5.4 ms, 16 px CJK 9.8 to 7.1); V3 7 % | yes: the builder should write them |
| Quad margin 1 px against 0.75 | equal within the noise | 0.75 |

### The probe's 0.49 difference between renderers

The probe saw Slug differ between the Iris Xe and the software renderers
by up to 0.49 on 20 to 50 pixels per turned condition. This implementation
does not: the Iris Xe, WARP and SwiftShader agree within 6.5e-5 on all
1.5 million compared pixels but one. To find the cause, the test ran the
paper's ramp in three forms:

| em point | window | Iris Xe against WARP | Iris Xe against SwiftShader |
|---|---|---|---|
| interpolated from the vertices | `fwidth` | max 3.4e-3, 99 pixels above 1e-3 | max 0.478, 26156 pixels above 1e-3 |
| from `gl_FragCoord` | `fwidth` | max 1.2e-4 | max 5.9e-3, 1731 pixels |
| interpolated | exact (from the map) | max 3.4e-3, 99 pixels | max 0.478, 25959 pixels |
| from `gl_FragCoord` | exact | max 1.6e-5 | max 1.6e-5 |

The cause is the interpolated em coordinate: each rasterizer snaps the
vertices and interpolates differently (v0.1 found the same for local
coordinates). The weights' floor of 1/65536 was not the cause: the
continuous combination gave the same differences. ysp_gfx computes em from
`gl_FragCoord` through the item's exact map and takes the window from the
map.

The one pixel left (0.087 on SwiftShader, both methods' rays): its ray
passes a glyph's corner within rounding, so it takes the corner's jump on
one renderer and not on the other. The test allows 3 such pixels.

### Curve runs: pixel tests

`v0.6 text`: the test-local builder (tests/adapt/gfx_cset_build.h)
makes 12 glyphs from contours: a rect, the square with a hole, a circle of
8 quadratics with its extrema inside curves (so that a ray near an extremum
meets one curve twice), two overlapping rects (nonzero and even-odd), a pentagram
(nonzero and even-odd), thin stems of 0.3 and 0.8 px at 12 px per em, a
flat ellipse with a slanted bar, a CJK-like grid of overlapping strokes, a
ring far from the origin (60 em, where f16 holds 0.03 em), and a rect with
a degenerate curve. The reference is the exact box coverage of each pixel
in double: scanlines in y by adaptive Gauss-Kronrod 7-15, split at every
curve end, y extremum and crossing of the pixel's sides; each scanline's
covered length from the exact roots with the fill rule on its integer
winding. Conditions: 8, 12, 24 and 48 px per em, rotations 0, 15 and 45
degrees (each item's ori), two subpixel offsets.

| Check | Iris Xe | WARP | SwiftShader | Tolerance |
|---|---|---|---|---|
| exact area, resolved glyphs, rotation 0, largest error | 1.78e-5 | 1.78e-5 | 1.78e-5 | 5e-5 |
| rays, mean edge error, worst rotation, 8 / 12 / 24 / 48 px | 0.0285 / 0.0192 / 0.0091 / 0.0037 | same | same | 0.035 / 0.024 / 0.012 / 0.005 |
| with exact area | 0.0275 / 0.0180 / 0.0089 / 0.0036 | same | same | the same |
| largest edge error (corners, self-intersections) | 0.557 | 0.557 | 0.557 | 0.6 |
| largest error off the edges (a half-plane past an acute corner) | 0.254 | 0.254 | 0.254 | 0.3 |
| against the Iris Xe, pixel by pixel | | 6.5e-5 | 1 pixel 0.087, the rest under 1e-3 | 3 pixels above 1e-3 |
| the rays against their CPU form in double, every glyph at 24 px | 5.0e-5 | 9.1e-6 | 9.1e-6 | 1e-3 |
| items alone against one run; copied items against a buffer | 0 | 0 | 0 | 0 |
| a glyph added at run time against one made at make | 0 | 0 | 0 | 0 |
| a run through an RGBA16F target against the scene | 4.9e-4 | 4.9e-4 | 2.5e-4 | 1e-3 |
| palette: integer entries (aliased); fractions | 0 wrong; 0 | 0; 0 | 0; 0 | 0; 2e-7 |
| `ygfx_hit()` against aliased coverage; the topmost item | 0 of 64000; 0 | 0; 0 | 0; 0 | 0 |
| a 0.005 px move: exact area; rays on a circle (turned 15 and 45) | 0.0079; 0.0375 | same | same | 0.02; 0.05 |
| a 0.005 px move: rays past a corner (reported) | 0.4385 | 0.4385 | 0.4385 | none |
| refusals (no set, size 0, an edge profile, a stroke, a glyph id not an integer or past the table, COLOR without a palette, make and add inside a frame, a run as a template, a wrong version, an add past the room) | 12 of 12 | 12 of 12 | 12 of 12 | all |

The CPU half: the test vector bit for bit; 17 faults, each refused with the
glyph or word named; the CPU winding against a brute-force winding (the
quadratic formula with half-open intervals, none of the header's
arithmetic) at 195296 points over 12 glyphs, 4 band counts, with and
without backward lists: 0 wrong. Of these, 3296 lie on the row of a curve's
endpoint, where the header's rule (on the ray is not above) must equal the
winding just past the row.

### A bug the mutations found

The first mutation run left two mutants alive: the shader's root rule
without its two-root rows (v06-02), and the CPU winding with an endpoint on
the ray counted as above (v06-03). No test glyph had a curve with an
extremum inside it, and no test point lay on an endpoint's row. With both
added (the circle turned by 22.5 degrees; points on every endpoint row),
the CPU winding was wrong at 69 points: in a row where both endpoints lie
on one side of the ray and the control point on the other, the curve can
turn back before it reaches the ray. Then the discriminant is negative.
Clamping it to 0 gives one double root in the paper's form, (b +- sqrt D) /
a, but two different roots in the form without cancellation (q / a and
c / q), and their crossings did not cancel. The shader had the same fault.
Now a two-root row with a negative discriminant has no crossing, in the
shader, the CPU winding and the test's CPU form; mutants v06-27 and v06-28
cover it. Fonts whose curves have their extrema at on-curve points
(TrueType's recommendation) rarely meet this; CFF conversions and artwork
do. Its cost, A/B under the lock (rays alone): a 12 px page 5.28 to 5.92
ms before, 5.54 to 5.80 after; the 16 px CJK page 6.94 to 7.71 before,
7.20 to 7.60 after: within the spread.

## v0.6: blur

### Pixel tests

`v0.6 blur`: a rect on whole pixels (its point samples are its box
coverage) drawn into the layer, blurred, read back from the result,
against the exact blurred rect at pixel centers (a product of erf
differences).

| Format, source | sigma 0.5 | 1 | 2 | 4 | 8 | 16 |
|---|---|---|---|---|---|---|
| RGBA32F, 1x | 0.204 | 0.0313 | 0.0087 | 0.0022 | 5.4e-4 | 1.5e-4 |
| RGBA32F, 2x | 0.038 | 0.0076 | 0.0021 | 5.4e-4 | 1.4e-4 | 3.8e-5 |
| RGBA16F and R16F, 1x | 0.203 | 0.0310 | 0.0082 to 0.0087 | 0.0019 to 0.0024 | 8.7e-4 to 9.1e-4 | 5.9e-4 to 7.8e-4 |
| RGBA16F and R16F, 2x | 0.037 to 0.038 | 0.0071 to 0.0081 | 0.0019 to 0.0022 | 8.1e-4 to 9.8e-4 | 5.3e-4 to 9.6e-4 | 4.5e-4 to 8.6e-4 |

The same on the three renderers to 2 digits (the 16-bit rows vary by
renderer within the ranges given). They reproduce the probe's table
(docs/text_probe.md): 1x f32 0.20, 0.040, 0.0089, 0.0017, 6.1e-4, 1.4e-4;
2x 0.023, 0.010, 0.0034, 5.7e-4, 1.4e-4, 3.1e-5. The tolerances are about
1.25 times these rows. Also: an R16F result drawn as tinted coverage
against its own values, 2.4e-8; a rect at the layer's corner, sigma 4,
against the exact blur on an empty plane, within the interior's error; 9
refusals of 9 (format RGBA8, supersample 3, apply outside a frame, sigma
below the box's SD, sigma 40, make inside a frame, apply inside a target
pass, the 16 passes).

## v0.6: video chroma


The video program's NV12 draw cost 1.64 times an RGBA8 image (v0.4). Two
changes were timed (AC, the lock held, A/B/A/B, GPU over an empty frame at
1080p):

| Video program | RGBA8 | NV12 | I420 | NV12 / RGBA8 |
|---|---|---|---|---|
| v0.5: hand bilinear, constants read per fragment | 0.58 | 0.92 to 0.94 | 0.97 to 0.99 | 1.59 to 1.62 |
| sampler chroma (one `textureLod` a plane at 1:1), constants per fragment | 0.58 | 0.93 to 0.95 | 0.96 to 1.01 | 1.58 to 1.60 |
| sampler chroma, constants as flat inputs | 0.57 to 0.61 | 0.69 to 0.75 | 0.77 to 0.83 | 1.22 |
| hand bilinear, constants as flat inputs | 0.57 to 0.59 | 0.63 to 0.67 | 0.78 to 0.81 | 1.10 to 1.14 |

The sampler gave nothing: the chroma fetches were not the cost. Seven vec4
constants read per fragment from the dynamically indexed uniform array
were (v0.4 suspected the same for instances). They now come from the
vertex shader as flat inputs. The sampler path was deleted; the hand
bilinear stays the only path, with its float weights at every scale.
ysp/video.h needs no change. Bars: GPU 1.0 ms: met (NV12 0.63 to 0.67, I420
0.78 to 0.81, TRC_DEVICE 1.00 to 1.03: at the bar); 1.5x RGBA8: met (1.10
to 1.14).

## v0.6 cost

`gfx_bench --only "v0.6 ..."` on the Iris Xe at 1920 x 1200, RGBA16F
scene, 2026-10-06 20:11 to 20:20, AC, the lock held, no build or test
running (checked before the lock was taken). GPU over an empty frame, the
median of 25 interleaved rounds; each configuration in its own process,
A/B/A/B. The fonts: Segoe UI (94 printable ASCII glyphs) and Microsoft
YaHei (7000 glyphs from U+4E00), read from C:/Windows/Fonts by a stopgap
`glyf` reader in examples/gfx/bench.c (no outline is committed), built by the test
builder with its default bands and backward lists. The pages are larger
than the probe's: 26205 against 21763 glyphs, and 7140 against 6783.

| Workload | Exact area where it applies, ms | Rays only, ms | Probe (Slug f32), ms |
|---|---|---|---|
| 12 px Latin page, 26205 glyphs | 7.93, 8.16 | 5.82, 5.55 | 3.95 (21763 glyphs) |
| 48 px Latin, 1612 glyphs | 1.69, 1.75 | 1.86, 1.76 | 1.55 (1439) |
| 16 px CJK page, 7140 glyphs | 10.00, 10.19 | 7.69, 7.23 | 6.08 (6783) |
| 200 px CJK, 45 glyphs | 1.13, 1.15 | 1.68, 1.59 | 2.52 |
| 12 px Latin page, turned 15 degrees (rays) | 6.16, 6.20 | 5.65, 5.43 | |
| 16 px CJK page, turned 15 degrees (rays) | 6.63, 6.72 | 6.51, 6.20 | |
| a 40-glyph line at 48 px, items copied each frame | 0.087 ms GPU, 16.8 us CPU | 0.088, 18.1 us | |

Bars: the probe's numbers, scaled to these pages (4.76 ms and 6.38 ms):
missed by the 12 px page (5.5 to 5.8 rays, 7.9 to 8.2 exact) and the 16
px CJK page (7.2 to 7.7 rays, 10.0 to 10.2 exact); met at 48 and 200 px.
The probe's shader is lost, so the gap cannot be broken down further than
this: the half-plane coverage costs 1.0 ms of a 12 px page (V0 against V1,
above), and the exact area 2.4 ms more than the rays at 12 px and 0.4 ms
less at 200 px. The 40-glyph line meets its bar (0.1 ms, 10 us of CPU in
draw(): the 16.8 us are the whole frame's). A page of 12 px text with
the exact area fits a 16.7 ms frame with 8 ms left; the knobs for a page
are the rays alone (no exact area: a run option, not built) and fewer
glyphs a frame.

`ygfx_cset_make()`: the Latin set with the program's compile 451 ms
(155 ms when the exact area is compiled out: the larger program is slower
to compile on ANGLE's D3D11, bar 60 ms missed; the program cache makes a
warm open load it); 7000 CJK glyphs, 432297 texels and 3.3 million words
(about 59 MB), 26 ms (bar 250 ms for 30000 glyphs: met).

### Blur

| Workload | sigma 0.5 | 1 | 2 | 4 | 8 | 16 | 32 |
|---|---|---|---|---|---|---|---|
| full screen R16F | 1.42 | 1.96 | 3.34 | 5.96 | | | |
| probe, full screen R16F | 1.21 | 1.86 | 3.37 | 6.26 | | | |
| 400 x 200 R16F, 1x: the rect drawn, blurred, the image drawn | 0.10 | 0.12 | 0.18 | 0.27 | 0.45 | 0.76 | 1.34 |
| the same, 2x | 0.17 | 0.24 | 0.38 | 0.66 | 1.18 | 2.21 | 3.92 |

Full screen at sigma 2: R16F 3.32 ms, RGBA16F 3.43, RGBA32F 3.57 (bar:
RGBA32F at most 2 times RGBA16F, met). The full-screen rows equal the
probe's within 0.2 ms; a full-screen blur at sigma 2 or less fits a 16.7
ms frame with 13 ms left, as the user accepted. The word rows include
drawing the layer and the result (the probe timed the passes alone): 0.27
ms at sigma 4 misses the 0.2 ms bar; at sigma 2 and below it is met. 2x
costs 1.7 to 2.4 times 1x on a word; the 0.3 ms bar is met at sigma 1 and
below (0.38 ms at sigma 2). Weights from the CPU in a uniform array were
not timed: each tap costs a fetch beside its exp. The knob for a large
sigma is the pyramid (Next).

### Video, and no regression

| Row | v0.5.0 (git HEAD) | v0.6 |
|---|---|---|
| RGBA8 1080p, draw | 0.571, 0.581 | 0.557, 0.588 |
| NV12, BT1886 | 0.964, 0.975 | 0.642, 0.679 |
| I420 | 0.990, 1.000 | 0.782, 0.824 |
| NV12, TRC_DEVICE | 1.098, 1.108 | 1.003, 1.041 |
| 10000 gabors, instanced | 1.228, 1.154 | 1.290, 1.199 |
| 1000 stimuli of 4 kinds, reordered | 1.046, 1.297 | 1.136, 1.038 |
| v0.2 rounded rect; RRECT; a run of 200 MSDF glyphs | 0.413, 0.994, 0.128 | 0.412, 0.992, 0.134 |
| `ygfx_open()`, no cache, 3 rounds | 2136, 2104, 2108 ms | 2192, 2130, 2095 ms |

Video bars: 1.0 ms met (TRC_DEVICE at it: 1.00 to 1.04); 1.5 times RGBA8
met (1.15). The other rows moved within their run-to-run spread.

## v0.6 mutations

[tests/mutate/gfx.toml](../tests/mutate/gfx.toml), mutants v06-00 to
v06-28, run with `uv run tests/mutate/mutate.py gfx.toml --only ...` on
the Iris Xe (the CPU half alone for the CPU faults). The text part's exact
references were read from disk (`YGFX_TEST_REFDIR`, a development seam
of the test), so a text mutant takes 7 s, not 4 minutes. v04-18 and v04-22
were anchored again (their anchors now occur twice: the curve runs' palette
and offsets copy the instances' code); both still caught.

| # | Fault | Result |
|---|---|---|
| 0 | a ray's band one off | caught |
| 1 | the loop's early exit without the window | caught |
| 2 | the root rule without the two-root rows | caught (after the test circle was turned, below) |
| 3 | the CPU winding counts an endpoint on the ray as above | caught (after points on endpoint rows were added) |
| 4 | even-odd drawn as nonzero | caught |
| 5 | a backward ray's winding not negated | caught |
| 6 | the glyph table read one word off | caught |
| 7 | an item turned the other way | caught |
| 8 | an item's scale ignored | caught |
| 9 | a palette fraction ignored | caught |
| 10 | a run's local y axis without the pass's sign | caught |
| 11 | the half-plane coverage's corners as a ramp | caught |
| 12 | the rays' mix without its fade | caught |
| 13 | aliased coverage with the window | caught |
| 14 | a glyph added at run time: its table entry not uploaded | caught |
| 15 | the check skips the range of a band's curve references | caught |
| 16 | the exact area's backward term dropped | caught |
| 17 | the exact area's band part from the pixel's bottom | caught |
| 18 | the blur without the box filter's variance | caught |
| 19 | taps to 3 SD | caught |
| 20 | weights not normalized | caught |
| 21 | both passes horizontal | caught |
| 22 | the output center half a texel off | caught |
| 23 | taps past the layer clamped to its edge | caught |
| 24 | coverage drawn without the tint | caught |
| 25 | a 2x layer at 1x px per unit | caught |
| 26 | the video constants read one block off | caught |
| 27 | a two-root row that misses the ray counted (the fix) | caught |
| 28 | the same in the CPU winding | caught |

29 of 29. The first run had 25 of 27: 2 and 3 survived, which found the
bug in "A bug the mutations found".

## v0.6 builds

Warnings as errors. MSVC 19.44 (`cl` alone, /W4 /WX): the test as C and
as C++17, the full test on the Iris Xe, WARP and SwiftShader. MinGW-w64
gcc 16.1 (winlibs, -Wall -Wextra -Wpedantic -Wshadow -Werror): the test as
C99, C11 and C++17, their CPU halves run. CMake (build-slug, MSVC, SDL3
3.4.0): `test_gfx`, `gfx_bench`, every other gfx and video example,
the ysp_gfx and ysp_video compile checks and `test_video` build clean;
the compile checks, the CPU half and `test_video` pass. Not run for v0.6: WSL (gcc 13.3, ASan, UBSan, llvmpipe) and
emcc; CI runs the Linux and wasm jobs.

## v0.6 departures from the design

- Names, as the coordinator asked: `ygfx_crun`, `ygfx_crun_desc`,
  `ygfx_citem`, `ygfx_blur_apply`; `ygfx_blur_make` returns an int
  code.
- Format flag bit 2 (resolved) was added after the format froze, on the
  outline worker's request; the version stays 1 (a set without the bit
  draws with the rays, as before).
- V2 and screen-aligned rays were not built (above).
- Sampler chroma was deleted; the video bars are met by flat inputs, which
  the design did not have.
- The text program compiles in 430 to 450 ms, not under 60 ms: the exact
  area doubles the program. The program cache covers warm opens.
- examples/gfx/bench.c reads TrueType `glyf` tables itself, a stopgap until the
  outline builder's header is in the tree; it then moves to that header
  and the reader is deleted.

## v0.7 status

The running record of v0.7: the coordinator's review of v0.6 (2026-10-06,
late), seven items in order.

| Item | State |
|---|---|
| 1. Test time | Done. The default run takes 42 to 54 s on this laptop (one renderer, 5 of the text part's 24 conditions); `YGFX_TEST_FULL=1` runs every renderer and condition in 174 to 264 s (was about 17 minutes). Every measured value equal to the 17-minute run's. All 75 mutants of gfx.toml caught by the default run. WSL ASan + UBSan and emcc runs: below. |
| 2. Rays-only option | Done: `crun_desc.rays` on a second program; a 12 px page 5.28 to 5.31 ms against 7.43 to 7.50. Static text cached in a target: bit for bit after a change of the text program's coordinates; 0.50 ms. |
| 3. Gallery page 7 | Done: a staggered word entrance, a glow, artwork layers with the hit test, cached text. |
| 4. The kinds patch | Applied on v0.6 by hand where needed; `vspec` part 0 values differ on all three renderers; re-timed quiet on AC: all three programs close at least half their gaps; built. |
| 5. ELLIPSE CPU | Done: the arithmetic-geometric mean; 1000 packs 0.06 ms, were 36 ms; the same floats. |
| 6. Gaussian NOISE | Done: a quantile table, bit for bit on the GPU and the CPU; 8 % over UNIFORM at 1 px checks. |
| 7. Gradient noise | Designed, not built (the report; "Next"). |
| ysp/outline.h | gfx_bench builds its sets with it (the stopgap `glyf` reader deleted); the test cross-checks its set of the test glyphs. |

## v0.7: test time

The default `test_gfx` took about 17 minutes on this laptop: three
renderers of 81 to 103 s each, and 206 s of exact text references. Where
the time went, per part on the Iris Xe (a timing copy of the test; the
machine was busy, so the parts ran slower than in the table of results):

| Part | s | Cause |
|---|---|---|
| v0.3 kinds | 32 | `ygfx_hit()` at every pixel, which packs the block each call; ELLIPSE's pack ran a 1024-point quadrature (23 to 50 us) |
| v0.3 truth | 37 | the brute-force distance over 8192 pieces and the inside test over every boundary point at every pixel; the smooth union's 59000 zero-set points at every pixel near it |
| v0.4 cache | 29 | 8 opens without a cache, 3.7 s each on ANGLE's D3D11 |
| v0.3 paint, alpha+passes | 10, 7 | opens without the test's program cache |
| v0.6 text | 206 | 24 exact references, single-threaded |

What changed, none of it in a check's arithmetic:

- ELLIPSE's quarter perimeter by the arithmetic-geometric mean (header,
  "v0.7: ELLIPSE's perimeter").
- The hit test runs only where it is checked (hard edges).
- The CPU references run on threads (`YGFX_TEST_THREADS`, default the
  hardware threads up to 16): a row or a pixel per call, results reduced
  afterwards in row order, so the results do not depend on the count.
- The brute-force distance skips boxes of 32 pieces that cannot be nearer
  than the best so far, and boxes of 32 boundary edges that cannot cross
  the row; the inside test runs only within reach. The smooth union's zero
  set sits in 2 px cells: exact within 13 px, the check uses 12.
- Paint and alpha+passes open with the test's program cache.
- The cache part's default takes its reference frame from case 0
  (compiled while storing) instead of a separate open without a cache, and
  skips the unwritable folder's case.
- The text part's default runs 5 of its 24 conditions: (8 px, 15 deg,
  offset 0), (12, 0, 1), (12, 45, 0), (24, 15, 1), (48, 0, 0). The rest is
  `YGFX_TEST_FULL=1`.
- With no `YGFX_TEST_DEVICES`, the default runs the first renderer that
  opens (hardware, else WARP, else SwiftShader; Mesa on Linux). CI names
  its renderers, so CI runs as before.

The text references scale poorly with threads (1 thread 35 s, 16 threads
10 s for the part): items run in turn, and a box of an 8 px glyph has few
pixels. The Gauss-Kronrod tolerance is not the cost (1e-9, 1e-8, 1e-7: 13,
14, 12 s).

| Run | Wall time |
|---|---|
| default (Iris Xe, 5 text conditions) | 42, 45 s; 54 s with the specialized programs (14 at each cold open) |
| `YGFX_TEST_FULL=1` (Iris Xe, WARP, SwiftShader, 24 conditions) | 174, 178 s; 255, 264 s with the specialized programs |
| WSL Ubuntu 22.04, gcc 11.4, 8 threads: build -O2 / ASan + UBSan | 7.7 / 20.5 s |
| the CPU half, -O2 / ASan + UBSan | 0.83 / 0.84 s |
| llvmpipe GL, default / full / ASan + UBSan default | 24.0 / 58.5 / 38.4 s |
| emcc 6.0.10 in node 24.19: the compile check built and run; the test built; its CPU half | 2.7 s; 5.4 s; 0.77 s |

All of these pass. The full run's measured values, line for line, equal
the 17-minute run's. The mutants of tests/mutate/gfx.toml run against the
default test: 64 of 64 before the v0.7 features, 75 of 75 after.

## v0.7: curve runs

### The rays alone, on a program of their own

`crun_desc.rays` draws a run by the rays even where the exact area applies.
A flag in the one program saved too little (12 px page, Iris Xe, AC, the
guard; GPU over an empty frame, three interleaved rounds):

| Program | 12 px Latin page, ms |
|---|---|
| exact area where it applies (the default) | 7.48, 7.50, 7.58 |
| .rays as a flag in that program | 6.01, 6.01, 6.05 |
| a program compiled without the exact area | 5.30, 5.34, 5.30 |

The exact area's registers stay reserved in a program that has it, so
`.rays` takes a second program, made at the first `ygfx_crun()` with
`.rays` (about 150 ms on ANGLE's D3D11; the program cache keeps it). It
draws the same bits as the program the test compiles without the exact
area.

Which to pick: the exact area wherever text is read or measured. `.rays`
saves 2.2 ms (29 %) on a 12 px page, at the rays' error everywhere: a mean
edge error of 0.029, 0.019, 0.009 and 0.004 at 8, 12, 24 and 48 px per em,
and up to 0.56 at corners, against 1.8e-5.

### Static text in a target

A reading page is static: draw it once into a target, then composite it
at 1:1 on whole px. The composite is exact (`texelFetch`, times 1). The
run in the target was not: a target pass has rows top first, so its
pixels' em points came from a mirrored `gl_FragCoord`, which rounds
differently in f32 (8.6e-6 on the Iris Xe, 7.9e-6 on WARP and SwiftShader
in an RGBA32F target). The text program now works in px with y down: the
pack writes the run's placement in y-down px (block slots 24 to 27), the
vertex shader builds the glyph in them and flips only the scene's quad,
and the fragment shader turns `gl_FragCoord` into y-down px exactly (H - y
at a pixel center). A target pass and the scene then compute every bit
alike.

| Cached against drawn, every value | Iris Xe | WARP | SwiftShader |
|---|---|---|---|
| RGBA16F target, RGBA16F scene, over black | 0 | 0 | 0 |
| RGBA32F target, RGBA32F scene | 0 | 0 | 0 |
| RGBA32F target, RGBA16F scene, over gray | 0 | 0 | 0 |
| RGBA16F target, RGBA16F scene, over gray | 4.88e-4 (one f16 step) | 0 | 4.88e-4 |
| before the change: RGBA16F over black; RGBA32F | 4.88e-4; 8.6e-6 | 7.6e-6; 7.9e-6 | 3.1e-5; 7.9e-6 |

Over gray an RGBA16F target has rounded alpha before the blend; the scene's
own draw blends with f32 alpha. Use an RGBA32F target (36.9 MB at 1920 x
1200, against 18.4 MB) when the page lies over a non-black field and the
values must match the drawn run.

| Workload (Iris Xe, AC, the guard) | GPU ms |
|---|---|
| 12 px Latin page, 26205 glyphs, exact area | 7.43, 7.50, 7.46 |
| the same with .rays | 5.29, 5.31, 5.28 |
| the same drawn once into an RGBA16F target, composited each frame | 0.50, 0.50, 0.51 |
| 16 px CJK page, 7140 glyphs, exact area | 9.17, 9.24, 9.23 |
| 12 px and 16 px pages turned 15 deg (the rays, in the exact-area program) | 5.81 to 5.88; 6.33 to 6.40 |
| 48 px Latin, 1612 glyphs; 200 px CJK, 45 glyphs | 1.55 to 1.58; 1.02 to 1.06 |
| a 40-glyph line at 48 px, items copied | 0.074 to 0.084 (CPU 14 to 16 us) |

The pass that fills the target must be inside a frame: `begin_target` is
refused outside `ygfx_begin()` and `ygfx_end()`, so a page goes in at
an ITI frame. A setup-time render would be cheap to allow: `gfx_bench`
already calls `ygfx_begin()` with a zeroed `yscr_frame` and
`ygfx_end()` outside ysp_screen's frames. That works, but `end()` clears
the scene, runs the output pass into the back buffer and counts a frame. A
flag that skips those three for a frame with no scene draws is about 15
lines. Not changed.

### ysp/outline.h

gfx_bench builds its sets with `yol_cset_add_font()` (resolved, backward
lists); the stopgap `glyf` reader is deleted. Its sets cost the same as
the test builder's (one session, interleaved: 12 px page 7.44 to 7.95
against 7.42 to 7.95 ms; 16 px CJK 9.28 to 9.42 against 9.26 to 9.91). The
7000 CJK glyphs build in 330 to 424 ms (437032 texels, 3.32 million words,
0.5 % more than the test builder's: the resolve splits some curves).
`ygfx_cset_make()` with the program 420 to 509 ms; the CJK set 23 to 27
ms.

The test builds its 12 glyphs with ysp/outline.h as well:

| Check | Iris Xe | WARP | SwiftShader |
|---|---|---|---|
| `ygfx_cset_check()` on its set | OK | | |
| its winding 0 or 1, and its fill equal to the brute force's, 24000 points | 0 wrong | | |
| every glyph (overlaps resolved, so exact area) against the exact coverage, 12 px | 1.15e-5 | 1.14e-5 | 1.14e-5 |
| against the test builder's set where both are resolved | 5.4e-7 | 4.8e-7 | 4.8e-7 |

## v0.7: kind-specialized vector programs


### The idea

The vector program is one program for every kind, compound, dash, paint
and effect. FXC inlines every kind's function at the call site in the
fold loop, so each pixel pays for code that its stimulus cannot reach. A
specialized program is the same text with defines that remove that code.
It computes the same values for its case, so its pixels are equal to the
generic program's pixels.

The defines:

| Define | Effect | Default (generic) |
|---|---|---|
| `YSP_VK` | the kinds in `ysp_prim_()`, bit k for kind k | all |
| `YSP_ONE` | one kind: no kind test | 0 |
| `YSP_V1` | one primitive: no fold loop and no operations | not set |
| `YSP_WS` | the arc length as a constant (0 or 1), not from the flags | not set |
| `YSP_ALONG` | dashes and trim | 1 |
| `YSP_PAINT` | paint | 1 |
| `YSP_FX` | effects | 1 |

A tier is a set of these defines. PLAIN is one primitive with no dashes,
trim, paint or effects. ALONG is one primitive with dashes or trim, and
no paint or effects. FOLD is a compound of one kind with no paint.

The kinds worker's design and measurements, sessions A to C, as it wrote
them; session D re-times them for v0.7.

### Method

A harness (`kinds_bench.c`, scratchpad) builds each variant from the
vector program's text with its defines, beside the generic program in one
process. GPU time is `gfx_bench`'s method: blocks of 12 frames between two
`glFinish()` calls, 25 rounds, the rows of a table in turn. The cost of a
row is the median of its difference from an empty frame in the same
round. The workloads are the workloads of the four bars that v0.3
missed, at 1920 x 1200. The Iris Xe through ANGLE's D3D11 path, the
measurement lock held for every run.

| Session | Time (2026-10-06) | Power | Load |
|---|---|---|---|
| A | 16:44 | AC | another worker's `find` over the home folder ran (CPU load 7 % at the start) |
| B | 17:13 | battery | none seen (4 %) |
| C | 17:15 to 17:18 | battery | none seen (4 %); `gfx_bench` built against v0.5.0 and against the patch, fresh processes, A/B/A/B |

Sessions A and B put every row of the four bars in one interleaved
table. Session C runs the real patch: `gfx_bench --only v0.3`, the
selection rule in the header, not the harness.

### Result per bar

"Gap closed" is (generic minus specialized) divided by (generic minus
bar), on the bar's own measure: a ratio for RRECT, dashes and OKLAB, ms
for the compound. A value of 1 meets the bar.

| Bar | Session | Reference, ms | Generic, ms | Specialized, ms | Generic | Specialized | Gap closed |
|---|---|---|---|---|---|---|---|
| RRECT fill, 1.3x the v0.2 rounded rect | A | 0.455 | 0.920 | 0.607 (PLAIN, RRECT) | 2.02x | 1.33x | 0.95 |
| | B | 0.595 | 1.246 | 0.804 | 2.09x | 1.35x | 0.94 |
| | C, round 1 | 0.666 / 0.469 | 1.621 | 0.654 | 2.43x | 1.39x | 0.92 |
| | C, round 2 | 0.480 / 0.619 | 1.147 | 0.901 | 2.39x | 1.46x | 0.86 |
| dashed circle, 1.2x the v0.2 stroke | A | 0.262 | 0.869 | 0.552 (ALONG, CIRCLE) | 3.32x | 2.11x | 0.57 |
| | B | 0.371 | 1.272 | 0.815 | 3.43x | 2.20x | 0.55 |
| | C, round 1 | 0.475 / 0.330 | 1.661 | 0.641 | 3.50x | 1.94x | 0.68 |
| | C, round 2 | 0.352 / 0.433 | 1.190 | 0.863 | 3.38x | 1.99x | 0.64 |
| compound of 8 with every effect, 1.0 ms | A | | 1.595 | 1.031 (FOLD, CIRCLE) | | | 0.95 |
| | B | | 2.187 | 1.286 | | | 0.76 |
| | C, round 1 | | 1.844 | 1.104 | | | 0.88 |
| | C, round 2 | | 1.754 | 1.074 | | | 0.90 |
| OKLAB paint, 1.1x the solid fill | A | 0.920 (solid, generic) | 1.495 | 1.368 (PLAIN + LINEAR OKLAB, RRECT) | 1.63x | 2.25x of the specialized solid; 1.49x of the generic solid | below 0 (-1.20); 0.26 |
| | B | 1.246 | 2.111 | 1.803 | 1.69x | 2.24x; 1.45x | below 0 (-0.92); 0.42 |
| | C (OKLAB stays generic in the patch) | 1.621 / 0.654 | 2.673 / 1.856 | | 1.65x | 2.84x | below 0 |

In session C the reference and the generic column come from different
processes: "0.666 / 0.469" is v0.5.0's reference, then the patch's. The
ratios use each process's own reference.

The PLAIN RRECT, the ALONG CIRCLE and the FOLD CIRCLE programs close at
least half of their gaps in every session. They are built.

The OKLAB paint does not. A specialized paint program makes the OKLAB
RRECT 8 to 17 % faster, but most of the paint's cost is outside the code
that the defines remove, and the solid fill gets faster too. With the
RRECT program in place, OKLAB paint costs 2.2 to 2.8 times the solid
fill, not 1.65 times. No pixel is slower than in v0.5.0: the bar moves
because its reference moves. The paint bar needs another approach.

The compound bar is an absolute time. In session A (AC) the specialized
compound took 1.031 ms; on battery 1.07 to 1.29 ms. The generic program
took 1.59 to 2.19 ms in the same sessions. The bar is met within the
machine's variation, not with margin.

The same programs also change two rows that have no bar of their own in
this list (session C, the patch):

| Workload | v0.5.0, ms | Patch, ms |
|---|---|---|
| compound of 8 circles, full screen | 3.943, 3.786 | 2.186, 2.097 |
| compound of 32 circles, full screen | 14.007, 13.389 | 7.691, 7.327 |
| compound cost per primitive per Mpx: (32 circles minus 1 circle) / 31 / 2.30 Mpx (bar 0.15) | 0.184, 0.176 | 0.099, 0.094 |
| 100 compounds of 4, 60 x 20 (below the area rule: generic) | 0.591, 0.589 | 0.631, 0.517 |

### Re-timed for v0.7 (session D)

The sessions above ran on battery or beside other work. Session D: the
patch applied by hand on v0.7 (above the changes below), 2026-10-06 23:02,
AC, the shared guard (load 8 % at the start, no compiler or test running),
`gfx_bench --only v0.3` and `--open-only` without the patch (v0.7's other
changes) and with it, fresh processes, A/B/A/B, three rounds (open: five).

| Bar | Reference, ms | Generic, ms | Specialized, ms | Generic | Specialized | Gap closed |
|---|---|---|---|---|---|---|
| RRECT fill, 1.3x the v0.2 rounded rect | 0.397 to 0.414 | 0.951 to 0.958 | 0.557 to 0.589 | 2.40x | 1.40 to 1.43x | 0.88 |
| dashed circle, 1.2x the v0.2 8 px stroke | 0.258 to 0.273 | 0.915 to 0.946 | 0.498 to 0.530 | 3.4 to 3.6x | 1.9 to 2.0x | 0.70 |
| compound of 8 with every effect, 1.0 ms | | 1.596 to 1.616 | 0.939 to 0.975 | | | met |
| OKLAB paint, 1.1x the solid fill | | 1.548 to 1.572 | 1.532 to 1.575 (generic: paint) | | | none |

| Workload | Without, ms | With, ms |
|---|---|---|
| compound of 8 circles, full screen | 3.48 to 3.51 | 1.85 to 1.87 |
| compound of 32 circles, full screen | 12.32 to 12.42 | 6.54 to 6.64 |
| 100 compounds of 4, 60 x 20 (below the area rule) | 0.457 to 0.495 | 0.461 to 0.484 |
| `ygfx_open()`, no cache, five rounds | 2113, 2042, 2116, 2107, 2045 | 2203, 2304, 2485, 2237, 2215 |

The three programs close at least half their gaps on a quiet machine:
built. The compound bar is met with margin now (0.94 to 0.98 ms). Open
costs 100 to 370 ms more for the three programs (session C could not see
it through its variation); a warm open from the cache is unchanged.

Applying the patch to v0.6: two hunks by hand (the backend's pipeline
count, now `+ YGFX__N_VSPEC` on v0.6's `2 * YGFX__N_BUILTIN + 8`, and
the test's part list), the rest with offsets. The patch's preprocessor
lines took three pieces of the vector program past C99's 4095-character
literal (gcc -Wpedantic): `ygfx__glsl_vec0`, `_vec3` and `_vec4` are now
split in two each (`_vec0b` and so on); the program's text is unchanged.
The test's `kinds` stats were renamed (`statsk`, `SK`: v0.6 had a
`stats8`), and the part is named `vspec`, so that a `YGFX_TEST_PARTS`
filter of "v0.3 kinds" does not run it. The worker's five mutants are in
gfx.toml as v07-00 to v07-04, run on the Iris Xe: all caught.

### Measured, then deleted

Exploration sessions on 2026-10-06 between 16:11 and 16:16 (AC; another
worker's `find` ran during the second). GPU time over an empty frame, ms;
each row against the generic program in the same table.

| Variant | Workload | Generic | Variant | Kept |
|---|---|---|---|---|
| PLAIN, every kind | RRECT | 1.174 | 1.099 | no: the kinds, not the features, make the cost |
| ALONG, every kind | dashed circle | 0.987 | 1.184 (slower) | no |
| FOLD + effects, every kind and operation | compound + effects | 1.911 | 1.627 | no |
| FOLD, every kind, SMOOTH_UNION only | compound + effects | 2.576 | 1.784 | no |
| RRECT only, every feature kept | RRECT | 1.410 | 1.117 | no: PLAIN RRECT 0.871 in the same table |
| CIRCLE only, every feature kept | dashed circle | 1.409 | 0.856 | no: ALONG CIRCLE 0.742 |
| FOLD CIRCLE, SMOOTH_UNION only | compound + effects | 2.576 | 1.308 | no: 6 % over FOLD CIRCLE with every operation (1.397), and one program per set of operations |
| the edge profile as a constant (COSINE) | RRECT; dashed; compound | 0.715; 0.620; 1.012 | 0.679; 0.548; 0.997 | no: 1 to 12 %, three times the programs |
| the dash loop of 3 as a constant | dashed circle | 0.620 (ALONG CIRCLE) | 0.557 | not yet: one session only; free (no new program) |
| no exact rect blur | compound + effects | 1.012 | 0.967 | no: 4 % |
| PLAIN + every paint, every kind | OKLAB RRECT | 1.871 | 1.835 | no |
| PLAIN STAR | STAR fill | 1.314 | 0.874 | no bar; a candidate |
| PLAIN ELLIPSE | ELLIPSE fill | 1.854 | 1.017 | no bar; a candidate |

### Program switches and the area rule

A program switch through ANGLE costs CPU time. When kinds split across
programs, the batches split too. Measured with the harness's pick seam
(each kind to its own program), 16:36 and 16:42, AC, CPU load 12 and
16 % at the start; a build was seen at the end of the first run:

| Workload | One program: wall ms, CPU us, draws | One program per kind: wall ms, CPU us, draws |
|---|---|---|
| 1000 small stimuli (RRECT, dashed circle, STAR in turn), grid, reordered | 2.642, 1413, 63 | 3.595, 2198, 71 (71 switches) |
| the same, call order | 2.250, 1180, 63 | 23.474, 16694, 1000 |
| the same at random places (overlapping), reordered | 2.459, 1300, 63 | 8.645, 6568, 326 |
| RRECT and STAR in turn, overlapping, 256 of 32 px | 0.451, 189, 16 | 1.010, 623, 51 |
| 256 of 64 px | 0.895, 208, 16 | 1.161, 559, 50 |
| 256 of 128 px | 2.468, 257, 16 | 2.460, 952, 42 |
| 128 of 256 px | 4.785, 253, 8 | 3.614, 824, 29 |
| 64 of 512 px | 9.140, 224, 4 | 5.827, 499, 18 |

The 1000-stimulus rows are CPU-bound, so their wall time is CPU time. An
extra program switch, with the draw that it splits off a batch, costs 7
to 24 us of CPU. GPU time breaks even at 128 x 128 px stimuli and wins
at 256 x 256 px.

So the rule: a draw takes a specialized program only when its quad
covers 65536 px or more (256 x 256). A smaller stimulus keeps the
generic program and batches as in v0.5.0. Thus the 1000-stimulus scenes
above do not change. An element array (instances) always keeps the
generic program: its instanced program comes from the generic one.

Per frame, the extra switches are at most one into each specialized
program per run of large stimuli of that case. A typical trial screen
with a few large shapes adds 0 to 3 switches.

### Compile time and open time

Each program alone, compiled and linked with a comment that defeats
ANGLE's memory cache (16:19, AC, the `find` running), three times; then
loaded from its binary three times:

| Program | Compile and link, ms | Binary, KB | Load, ms |
|---|---|---|---|
| generic vector program | 11658, 15112, 15084 | 110 | 2.1, 1.0, 0.9 |
| PLAIN RRECT | 294, 268, 299 | 23 | 0.5, 0.4, 0.4 |
| ALONG CIRCLE | 445, 424, 452 | 28 | 0.6, 0.6, 0.5 |
| FOLD CIRCLE | 713, 683, 768 | 32 | 0.4, 0.4, 0.3 |

Started together, as `ygfx_open()` starts its programs (16:39, AC):

| Programs | Wall time, ms (three runs) |
|---|---|
| generic alone | 10524, 5856, 6983 |
| the three specialized | 1305, 1119, 1470 |
| generic and the three specialized | 7506, 7455, 6632 |

The generic program is the long pole. The three others compile beside
it. `ygfx_open()` in fresh processes, five rounds, v0.5.0 and the
patch in turn (17:08 to 17:10, AC, no other load seen):

| Header | No cache, ms | Cold file cache, ms | Warm file cache, ms | Stores, ms |
|---|---|---|---|---|
| v0.5.0 (11 programs) | 3858, 5731, 4352, 4681, 8681 | 3907, 6283, 3618, 3955, 6912 | 23.9, 15.0, 22.1, 27.6, 43.6 | 70 to 105 |
| patch (14 programs) | 3807, 5050, 3705, 5099, 7364 | 3937, 4455, 3619, 9278, 6460 | 28.8, 32.8, 28.7, 48.9, 19.7 | 88 to 155 |

No change in the open without a cache can be seen through the
variation between rounds. A warm open stays under the 100 ms bar. The
stores stay under 5 % of a cold open.

The program cache needs no change: its key hashes the full program text,
and the defines are in the text. Each specialized program has its own
entry.

### Tests

`kinds` (GL, each renderer; named `vspec` in v0.7): 117 stimuli, each drawn alone twice. The
first draw keeps the generic program (the area rule set out of reach),
and the second takes the program that the rule picks (the rule set to
0). The RGBA32F scenes are compared bit for bit, and the program that
each draw took is compared with the program that its case expects. The
stimuli, for each of three edges (COSINE, GAUSSIAN, HARD): RRECT fill,
the three stroke alignments, offset, opacity and gate, in a turned and
scaled group, zero radii; a dashed or trimmed CIRCLE with snap, offset,
the three caps, in a group; compounds of CIRCLEs with every operation,
an onion primitive, a desc onion, an offset, a stroke and every effect;
the cases that must stay generic (paint, effects on one primitive, a
dashed RRECT, a compound with an RRECT in it, every other vector kind
filled and dashed). Then the area rule (a 40 x 30 RRECT stays generic, a
140 x 90 one does not, with the rule at 10000 px) and an element array.

| Check | Iris Xe | WARP | SwiftShader | Tolerance |
|---|---|---|---|---|
| values that differ between the generic and the picked program | 0 | 0 | 0 | 0 |
| draws on the wrong program | 0 | 0 | 0 | 0 |
| draws on each specialized program (RRECT, dashed CIRCLE, compound) | 15, 15, 12 | 15, 15, 12 | 15, 15, 12 | each above 0 |
| area rule, element array | 0, 0 wrong | 0, 0 | 0, 0 | 0 |

The whole test passes on the three renderers with the patch (MSVC
19.44, C). The C++17 build passes the `kinds` part on WARP. The CPU
cache test counts 14 programs.

Mutations, each in a copy of the patched header, the `kinds` part run on
WARP:

| # | Fault | Result |
|---|---|---|
| 0 | the dashed CIRCLE program without the arc length | caught |
| 1 | the pick does not check paint | caught |
| 2 | the pick does not check a compound's kinds | caught |
| 3 | the area rule inverted | caught |
| 4 | the pick does not check effects on one primitive | caught |

### Found on the way

An ELLIPSE costs 23 us of CPU per draw: 1000 ELLIPSEs took 23.3 ms of
CPU per frame against 0.35 to 0.59 ms for 1000 RRECTs, dashed circles
or STARs (16:31, AC; the `find` ran). `ygfx__vkind()` calls
`ygfx__ell_quarter()`, a quadrature of 1024 points with a `sin`, a
`cos` and a `sqrt` each, at every pack. The quarter perimeter depends
only on w, h and the scale, so a cache in the stimulus or a shorter
quadrature would remove it. Not changed here. (Fixed in v0.7 by the
arithmetic-geometric mean: "v0.7: ELLIPSE's perimeter".)

## v0.7: ELLIPSE's perimeter

ELLIPSE's pack computed its quarter perimeter (for dashes, trim and snap)
by a 1024-point Gauss-Legendre quadrature with a `sin`, a `cos` and a
`sqrt` per point: 23 us a draw (MSVC; the kinds worker's harness) to 36 us
(gcc -O2). It is now a E(e) by the arithmetic-geometric mean, pi / (2
M(a, b)) (a^2 - sum 2^(n-1) c_n^2), which converges quadratically: five
steps reach double precision up to 20:1.

| Check | Result |
|---|---|
| against a 2-million-panel Simpson rule in long double, aspects 1 to 20, scales 0.5 to 1000 | within 8.8e-16 (relative); the quadrature 6.0e-15 |
| the float the pack stores, against the quadrature's, the same 40 cases | equal in all 40 |
| the test's check, against a 20000-panel Simpson rule in double | 7.6e-15 (tolerance 1e-12) |
| every value of the full test (kinds, truth, dashes on ELLIPSE) | equal to v0.6.0's |
| 1000 ELLIPSE packs (gcc -O2, the guard, three rounds) | 35.6 to 38.1 ms before; 0.053 to 0.071 ms after |

The value no longer needs a cache: 0.02 us a call.

## v0.7: Gaussian noise

NOISE takes `dist = YGFX_GAUSSIAN`. The GPU computes no transcendental:
the check's hash (the same as UNIFORM's) picks, by its top 16 bits, one of
65536 equally likely values, a table made on the CPU at open:

- the standard normal's quantiles at the centers of 65536 equal-probability
  bins, Phi^-1((i + 0.5) / 65536), by Acklam's rational approximation
  (relative error under 1.15e-9);
- scaled in double so that the 65536 values have variance 1, then rounded
  to float; the upper half is the lower half negated, bit for bit, so the
  mean is 0;
- a 256 x 256 R32F texture (256 KB); 0.3 ms to build (gcc -O2).

`ygfx_noise_value(..., YGFX_GAUSSIAN)` reads the same table: the CPU
twin is exact. What is not exact is the table's relation to the true
normal: the quantiles are within 1.15e-9 of Phi^-1 before rounding, and
the distribution is discrete (65536 levels) and ends at 4.33 SD (P(|z| >
4.33) = 1.5e-5 for the true normal). A C library whose `log` differs in
the last bit could change a table entry; the test pins the table's hash
(MinGW gcc C99 and C11, g++, MSVC C and C++, glibc in WSL and emcc all give
`19ac94b1`).

| Check | Result |
|---|---|
| the table against quantiles found by bisection on `erfc` (the test's own, scaled the same way) | 6.0e-8 (relative; float rounding) |
| order, symmetry, variance | strictly increasing, symmetric bit for bit, 1.000000000 |
| 10^6 checks: mean, SD, beyond 3 SD | -0.0002, 0.9987, 0.00262 (normal: 0.00270) |
| Kolmogorov-Smirnov against Phi, 10^6 checks | D = 0.00095 (1 % critical value 0.00163) |
| GPU against `ygfx_noise_value()`, every check, Iris Xe, WARP, SwiftShader | 0 wrong |

| Full screen 1920 x 1200, contrast 0.2 (Iris Xe, AC, the guard, three rounds) | GPU ms |
|---|---|
| UNIFORM, 1 px checks | 0.372, 0.382, 0.392 |
| BINARY, 1 px checks | 0.383, 0.384, 0.381 |
| GAUSSIAN, 1 px checks | 0.414, 0.419, 0.421 |
| UNIFORM, BINARY, GAUSSIAN, 4 px checks | 0.391 to 0.420, all three |

contrast is the noise's SD. A Gaussian field leaves the gamut where
contrast times the value passes the background's room; the per-draw
gamut check counts those draws. `ygfx_noise_gauss()` (CPU only,
Box-Muller) stays, and is not what NOISE draws.

## v0.7 mutations

[tests/mutate/gfx.toml](../tests/mutate/gfx.toml) against the default test
(one renderer, the 5 text conditions), the Iris Xe:

| # | Fault | Result |
|---|---|---|
| v06-29 | a .rays run on the program with the exact area | caught |
| v06-30 | the scene's y down by another rounding path (`H * 1.0000002 - y`) | caught. A first form, `(H + 0.37) - (y + 0.37)`, survived: the shader compiler folds it to `H - y` |
| v06-31 | the ellipse's perimeter series off by half a term | caught (by the CPU check against Simpson; the GPU and its CPU form share the value) |
| v07-00 | the dashed CIRCLE program without the arc length | caught |
| v07-01 | paint not checked by the pick | caught |
| v07-02 | a compound's kinds not checked by the pick | caught |
| v07-03 | the area threshold inverted | caught |
| v07-04 | fx not checked for one primitive | caught |
| v07-05 | the Gaussian table's row and column swapped | caught |
| v07-06 | the Gaussian table not scaled to unit variance | caught |
| v07-07 | GAUSSIAN drawn as UNIFORM | caught |

v06-10 was anchored again: the text vertex shader no longer reads the
pass's axes; the fault now flips the quad in the wrong pass. The whole
list: 75 of 75 (767 s of mutants, 1009 s with the waits for other workers' timing).

## v0.7 builds

Warnings as errors. MSVC 19.44 (`cl`, /W4 /WX): the test as C and C++17,
the full test on the Iris Xe, WARP and SwiftShader. MinGW-w64 gcc 16.1
(-Wall -Wextra -Wpedantic -Wshadow -Werror): the test as C99, C11 and
C++17, their CPU halves run. CMake (build-slug, MSVC, SDL3 3.4.0):
`test_gfx`, `gfx_bench`, `gfx_gallery` (and its `--sim` run), the
compile checks. WSL (Ubuntu 22.04, gcc 11.4): -O2 and ASan + UBSan, the CPU
half and llvmpipe. emcc 6.0.10: the compile check and the test's CPU half
in node. Times in "v0.7: test time".

## v0.7 departures

- The default test runs one renderer; `YGFX_TEST_FULL=1` runs them all.
  Asked for: a default under about a minute.
- The text program's coordinates changed (y down) to make the cached page
  exact; not asked for, found while checking the coordinator's claim
  that the composite would be bit for bit (it was not, by up to one f16
  step, before).
- `.rays` takes a second program, not a flag: the flag saved 1.5 ms of
  the 2.2 a program saves.
- Gaussian noise is a table, not a formula: the only way found to a CPU
  twin with the same bits.
- The gallery's page 7 builds its font as a curve set with ysp/outline.h
  from the 5 x 7 bitmap (each pixel a square, merged), so it needs no font
  file on any platform.

## v0.8: setup passes and the automatic rays program

Two items the user approved after v0.7 (Next 9 and 5).

### Setup passes

`ygfx_begin_setup()` and `ygfx_end_setup()` run target passes outside a
screen frame, so a page of text, artwork or a blur layer is rendered at
setup instead of in an ITI frame. Of the two designs (a flag on
`ygfx_begin()`, or a pair of calls) the pair was the cleaner API: it
needs no `yscr_frame` and cannot be confused with a frame's `end()`.

- `begin_setup()` makes the GL context current (as `yscr_begin()` would),
  then runs `begin()`'s bookkeeping with a frame of time 0 and index 0;
  the frame block is written as in a frame, so a pass sees the same
  uniforms layout.
- Inside: target passes (16, not nested: the frame's rules and code),
  draws into them, `ygfx_blur_apply()`.
- Refused (`YGFX_ERR_ORDER`): a draw into the scene, `ygfx_end()`,
  `ygfx_begin()`, a second `begin_setup()`, `begin_setup()` inside a
  frame, `end_setup()` without one.
- `end_setup()` is `end()` without the scene's segments, the output stage,
  the present, the frame count and the clipped event (the clipped count
  still adds to `ygfx_clipped()`). It uploads the uniforms into the next
  ring slot, as a frame does; a ring slot is rewritten in full by every
  frame, so the next frame cannot see it. Nothing is allocated.

| Check (each renderer) | Iris Xe | WARP | SwiftShader |
|---|---|---|---|
| a page (12 test glyphs at 12 px) in a setup pass against in a frame: the RGBA32F targets | equal | equal | equal |
| the two targets composited 1:1 into the scene | equal | equal | equal |
| a blur (RGBA16F, sigma 2.5) applied in a setup pass against in a frame: the results | equal | equal | equal |
| the frame after a setup pass against the same frame before it: scene; output codes | 0; equal | 0; equal | 0; equal |
| frames counted by setup passes | 0 | 0 | 0 |
| refusals | 9 of 9 | 9 of 9 | 9 of 9 |

The gallery's page 7 renders its cached page and its glow layer in setup
passes.

### The rays program for turned runs

v0.7's `.rays` program draws the same bits as the exact-area program
wherever the exact area does not apply. v0.8 picks it by itself for such
runs. The rule, at draw:

- the run is aliased; or
- the run has no `YGFX_I_ORI`, and its turn (with its group's) is 0.01
  degrees or more from a quarter turn; or
- the run has `YGFX_I_ORI`, its items on the CPU, and every item's turn
  plus the run's is 0.01 degrees or more from a quarter turn.

The shader takes the exact area only within about 6e-5 degrees of a quarter
turn (its test is 1e-6, relative), so the margin of 0.01 degrees keeps
every item that could take it on the exact-area program. Items in a buffer
are on the GPU only, so a buffer run with `YGFX_I_ORI` stays there. The
rays program is made at `ygfx_crun()` for runs that are turned, aliased,
grouped, have `YGFX_I_ORI` or ask for `.rays`; a run made before it
exists stays on the exact-area program, which draws the same bits. The
check costs one `fmod` per item, in the loop that already validates each
item's glyph.

| Against the exact-area program (test seam `ygfx__text_auto = 0`) | Iris Xe | WARP | SwiftShader |
|---|---|---|---|
| run turned 15 degrees | 0 (rays program) | 0 | 0 |
| items turned 7 to 128 degrees (`YGFX_I_ORI`) | 0 (rays program) | 0 | 0 |
| one item on 90 degrees, the others turned | 0 (exact-area program) | 0 | 0 |
| aliased, not turned | 0 (rays program) | 0 | 0 |
| turned items in a buffer | 0 (exact-area program) | 0 | 0 |
| draws on the program the rule does not name | 0 | 0 | 0 |

| Turned pages (Iris Xe, AC, the guard, interleaved; rounds 2 and 3 of 3) | v0.7 (seam off), ms | v0.8, ms |
|---|---|---|
| 12 px Latin, 26205 glyphs, turned 15 degrees | 5.83, 6.03 (5.86 in round 1) | 5.09, 5.10 |
| 16 px CJK, 7140 glyphs, turned 15 degrees | 6.39, 6.61 (6.39) | 5.94, 5.92 |
| not turned: 12 px Latin; 16 px CJK (no change expected) | 7.48, 7.58; 9.30, 9.31 | 7.45, 7.43; 9.23, 9.19 |

The saving holds: 0.7 to 0.9 ms (13 %) on the Latin page, 0.45 to 0.7 ms
on the CJK page. The turned Latin page is now a little faster than the `.rays` page not
turned (5.24 to 5.28 ms in the same rounds).

### v0.8 mutations and builds

| # | Fault | Result |
|---|---|---|
| v08-00 | a setup pass draws into the scene | caught |
| v08-01 | end_setup runs the output stage and counts a frame | caught |
| v08-02 | end() accepted in a setup pass | caught |
| v08-03 | the automatic rays ignore the items' own turns | caught |
| v08-04 | the automatic rays for items in a buffer | caught |

v06-29 was anchored again (the program choice is now a condition). The
whole list: 80 of 80 against the default test (1062 s).

A setup pass that clears the scene (the scene's segment run with its
clear) was not made a mutant: it changes nothing a frame can see, since
every frame clears the scene first.

Builds: MSVC 19.44 C and C++17, MinGW gcc 16.1 C99, C11 and C++17, -Werror
and /W4 /WX; the CMake targets and `gfx_gallery --sim`. The full test on
the three renderers passes in 199 s; the default in 42 s.

## v0.9: the noise hash

NOISE, the noise dither and the Gaussian table's index all draw from one
integer hash. Through v0.8 that was `pcg_hash` (Jarzynski and Olano 2020),
nested for 2D: `hash(x ^ hash(y ^ hash(seed)))`. The user pointed at XQO
(skeeto/hash-prospector issue 23, Unlicense) and the coordinator at the
best-known `lowbias32` constants (issue 19). The candidates were measured
on a single hash, on the 2D constructions the stimuli use, and on the GPU;
v0.9 changes the construction, which was the weak part, and the hash.

### Candidates

| Name | Source | Operations |
|---|---|---|
| pcg_hash | Jarzynski and Olano 2020 (v0.8's) | 2 multiplies, a data-dependent shift |
| XQO | hash-prospector issue 23 | 2 multiplies (one a square), 3 shifts |
| triple32 | hash-prospector README (Chris Wellons, public domain) | 3 multiplies, 4 shifts |
| lowbias32 | hash-prospector README, the first constants `[16 7feb352d 15 846ca68b 16]` | 2 multiplies, 3 shifts |
| lowbias32, best | issue 19, comment 1120105785 (TheIronBorn, 2022-05-07): `[16 21f0aaad 15 735a2d97 15]`, bias 0.10704, the best in the thread (the `d35a2d97` and `f35a2d97` variants score 0.1133 and 0.1073) | 2 multiplies, 3 shifts |

Excluded: the CRC32C-based functions of January 2026 in issue 19 (bias
0.021 to 0.051). They need a hardware CRC32 instruction; GLSL ES 3.0 has
none, and a software CRC would cost more than these hashes and complicate
the CPU twin.

### A single hash: avalanche

Reynolds' method over all 2^32 inputs: for each input bit and output bit,
the count of inputs where flipping the input bit flips the output bit,
against 2^31 (WSL, gcc -O3, 8 threads, 2 minutes a hash).

| Hash | Linf | RMS |
|---|---|---|
| pcg_hash | 331871348 | 16645540.6 |
| XQO | 836260 | 121867.6 |
| triple32 | 167788 | 44857.9 |
| lowbias32 (first constants) | 2023972 | 372660.5 |
| lowbias32 (best constants) | 1211488 | 229873.3 |

The pcg_hash, XQO and triple32 rows reproduce the numbers quoted in issue
23 exactly. pcg_hash's worst bit pair flips 15 % away from even. Over all
2^32 inputs the sampling floor of the RMS is about 2^15 (32768): triple32
sits near it. Issue 19's scores (0.107 for the best lowbias32) are
hash-prospector's own bias estimate, which was not recomputed here; by the
exhaustive counts the best lowbias32 lies between XQO and the first
lowbias32.

### The constructions, as streams

PractRand 0.96 (CC0; built and run locally in WSL, not in the
repository), `RNG_test stdin32 -tlfail`, to 16 GB or the first FAIL. The
streams are what a stimulus draws:

- frames: the 32-bit key of every check of a 1920 x 1200 field, row by
  row, frame after frame with the seed 0, 1, 2, ... (a new noise frame
  each trial);
- seeds: for each check in turn, its key under 256 consecutive seeds (one
  check's values across trials);
- dither: the dither's key of every pixel, frame after frame (the frame
  index changes, the seed is 99);
- counter: the hash of 0, 1, 2, ... (the hash alone).

The constructions: the nest, v0.8's `hash(cx ^ hash(cy ^ hash(seed)))`;
"lin1" and "lin2", `hash(cx A + cy B + seed C)` with odd constants, one or
two rounds; "xor-mul", `hash(cx A ^ cy B ^ hash(seed))`; and "index",
v0.9's: `k = hash(seed)`, `m = hash(k ^ 0x9E3779B9)`,
`hash((cx + (cy << 16)) (k | 1) ^ m)`.

| Hash | Counter | Nest: frames, seeds, dither | lin1 | lin2 | xor-mul: frames, seeds, dither | Index: frames, seeds, dither |
|---|---|---|---|---|---|---|
| pcg_hash | 128 MB (Gap-16) | 256 MB (BDayS), 16 GB, 512 MB (BDayS) | 128 MB (Gap-16) | 1 GB (FPF, too even) | 16 GB, 1 GB (BDayS), 16 GB | 16 GB, 16 GB, 16 GB: no failure |
| XQO | 1 GB (FPF, too even) | 256 MB, 16 GB, 512 MB | 1 GB (FPF) | 1 GB (FPF) | 8 GB, 1 GB, 8 GB | 16 GB, 16 GB, 16 GB: no failure |
| triple32 | 1 GB (FPF, too even) | 256 MB, 16 GB, 512 MB | 1 GB (FPF) | 1 GB (FPF) | 16 GB, 1 GB, 8 GB | 16 GB, 16 GB, 16 GB: no failure |
| lowbias32, first | 256 MB (Gap-16, BRank) | 64 MB, 1 GB, 256 MB | 1 GB (FPF) | 1 GB (FPF) | not run | not run |
| lowbias32, best | 256 MB (Gap-16, BRank) | 256 MB, 256 MB, 256 MB | 1 GB (FPF) | 1 GB (FPF) | 8 GB, 1 GB, 8 GB | not run (out: the counter) |

Lengths are where the first FAIL came (16 GB: none). The "FPF, too even"
failures at 1 GB are a bijection's: distinct inputs through a permutation
never repeat, and at 2^28 words the missing birthday repeats show. They
say nothing about the hash; the constructions whose inputs can repeat do
not show them. Both lowbias32 variants fail a plain counter at 256 MB
(Gap-16, BRank), despite their low avalanche bias: bryc's point in issue
19, that the best single score need not combine best, holds here.

### The weak part was the nesting

Every hash failed the nest at 256 MB in the frames stream (BDayS, too many
repeats). The cause is the construction: row cy of a frame is
`hash(cx ^ K)` with `K = hash(cy ^ hash(seed))`. Two rows whose K agree
above bit 10 hold the same 2048 values, in another order (cx XOR the two
keys' difference). With 1200 rows that happens in a frame with
probability 1 - exp(-1200^2 / 2 / 2^21) = 29 %. Counted over 10000 seeds:

| Hash | Frames with two rows that are copies | The same, a frame or the pair of consecutive frames |
|---|---|---|
| pcg_hash | 28.3 % | 73.7 % |
| triple32 | 29.2 % | 74.5 % |

A copy XORs the column index by a number below 2048: by 1, it swaps
neighbors. The dither's nest has the same flaw.

xor-mul removes the row copies, but `cx A ^ cy B` is not injective: 896
pixel pairs of a 1920 x 1200 field share it and so are equal in every
frame (a random 32-bit key gives 618 equal pairs per frame, each pair
different every frame), and two frames whose seed keys differ by `cx A ^
cx' A` share a whole column. The seeds stream fails at 1 GB for every hash.

The index construction is injective over any 65536 x 65536 window (the
index cx + 65536 cy), and the seed enters as an odd multiplier and an
offset, so no two seeds' frames are shifts or copies of each other. It
passes every stream to 16 GB, with each of the three hashes, pcg_hash
included.

### Spatial checks

Over 64 frames of 1920 x 1200 (uniform values; expected 0 within 1 /
sqrt(n) = 8.2e-5 for the lags, 1.4e-3 across seeds):

| Hash, construction | x lag 1 | y lag 1 | diagonal | consecutive seeds |
|---|---|---|---|---|
| pcg_hash, nest (v0.8) | -7.8e-5 | -7.3e-5 | +1.0e-4 | -1.2e-4 |
| pcg_hash, index | -1.8e-5 | +2.4e-4 | -1.0e-5 | -2.5e-3 |
| XQO, index | +2.2e-5 | +2.6e-5 | -5.7e-6 | +2.2e-3 |
| triple32, index (v0.9) | -6.5e-5 | +9.4e-5 | +8.2e-5 | -1.4e-5 |

The 2D power spectrum of 1024 x 1024 fields, 8 seeds averaged, white
normalized to 1: the largest deviation of a radial bin (32 bins; the
expected scatter of one bin is 0.0126), the mean of the axis rows, the
largest single frequency (of 2^20; for an average of 8 periodograms about
3.5 is expected):

| Hash, construction | largest bin deviation | axes | largest frequency |
|---|---|---|---|
| pcg_hash, nest (v0.8) | 0.010 | 0.971 | 3.85 |
| pcg_hash, lin1 | 0.043 | 1.005 | 16.9 (a spike: the lattice of the linear form) |
| pcg_hash, index | 0.007 | 0.976 | 3.62 |
| XQO, index | 0.015 | 1.011 | 3.72 |
| triple32, index (v0.9) | 0.010 | 0.991 | 3.48 |

All flat but pcg_hash under lin1. Only pcg_hash under the index shows a
lag above 3 SD (y, +2.4e-4); triple32 shows none.

### GPU cost

Full-screen NOISE at 1 px checks and the noise dither (an empty frame
with the CLUT), the Iris Xe, AC, the guard, three interleaved rounds, GPU
ms over an empty frame (dither: the whole frame):

| Hash, construction | UNIFORM | GAUSSIAN | noise dither frame |
|---|---|---|---|
| pcg_hash, nest (v0.8) | 0.418 to 0.453 | 0.470 to 0.495 | 0.660 to 0.680 |
| XQO, nest | 0.454 to 0.489 | 0.506 to 0.554 | 0.638 to 0.670 |
| triple32, nest | 0.393 to 0.437 | 0.445 to 0.486 | 0.658 to 0.701 |
| pcg_hash, index | 0.448 to 0.467 | 0.500 to 0.515 | 0.666 to 0.704 |
| XQO, index | 0.455 to 0.538 | 0.494 to 0.575 | 0.655 to 0.714 |
| triple32, index | 0.444 to 0.464 | 0.482 to 0.512 | 0.644 to 0.828 |

No hash or construction is distinguishable from another through the
spread: the fragment cost is not the hash's arithmetic on this GPU. The
index form computes its two seed keys per fragment (they are uniform; a
compiler may hoist them).

### The decision

The construction changes to the index form: it is the only one that
passes every stream, and it costs the same. The hash changes to triple32:
with the index form all three pass, so the single hash decides, and
triple32 has the lowest avalanche bias (Linf 5 times XQO's, 2000 times
pcg_hash's) and passes a counter to the bijection limit, where pcg_hash
fails at 128 MB. XQO was as good on every stimulus test; triple32 wins on
the single hash alone.

What changed for a user: every NOISE pattern and the noise dither's
pattern (v0.9 is a version step for that), `ygfx_noise_gauss()`'s values
(the same construction), and a user shader's `ysp_hash()`. New:
`ygfx_hash2()` and the shaders' `ysp_hash2()`. `ygfx_noise_value()`
and the GPU still agree at every check on the three renderers. The test's
pinned digests changed (uniform `5f8bf9f7`, Gaussian draws `30886f1e`; the
Gaussian table `19ac94b1` does not depend on the hash); all five builds give
them. The noise dither's mean check had a tolerance of 2 SD (0.005 of a
code over 32768 draws); with the new pattern it read -0.0029, so it is 4
SD (0.01) now.

### v0.9 mutations and builds

| # | Fault | Result |
|---|---|---|
| v09-00 | the shaders' key without the odd multiplier | caught (GPU against the CPU twin) |
| v09-01 | the CPU twin's key nested as before v0.9 | caught (pinned digests) |
| v09-02 | the hash a step short | caught (pinned digests) |

The list before them: 80 of 80 against the default v0.9 test (1180 s).
Builds: MSVC 19.44 C and C++17, MinGW gcc 16.1 C99, C11 and C++17: the
pinned digests equal in all five; the full test on the Iris Xe, WARP and
SwiftShader passes (229 s).

## v0.10: gradient noise (SIMPLEX)

NOISE takes `dist = YGFX_SIMPLEX`: 3D simplex noise summed over octaves
(fBm), with the feature scale in units (px or deg), octaves, lacunarity,
gain, a seed, and a third coordinate z to bind for evolution. Simplex
noise's patent (US 6867776) expired in 2022.

### Design

- The lattice: Perlin's simplex in 3D (skew 1/3, unskew 1/6, four
  corners, radial falloff (0.6 - d^2)^4, the sum times 32).
- Gradients: the 12 cube edges in 16 slots, picked by a corner's key's top
  4 bits.
- Keys: v0.9's construction. A z layer gets `k = hash(cz ^ S)`,
  `m = hash(k ^ 0x9E3779B9)`; a corner's key is
  `hash((cx + cy 2^16) (k | 1) ^ m)`; S is the seed's hash plus the octave.
  Two hashes per z layer (two layers a pixel) and one per corner.
- fBm: octave k has cells of scale / lacunarity^k and weight gain^k; the
  weights sum to 1.

### Integer or float: the harness first

The same simplex as two user shaders on the Iris Xe (AC, the guard, three
interleaved rounds; GPU ms over an empty frame, 1920 x 1200, cell 32 px):

| Octaves | f32 simplex | integer simplex | integer / f32 |
|---|---|---|---|
| 1 | 1.475, 1.480, 1.481 | 1.968, 1.969, 1.974 | 1.33 |
| 2 | 2.672, 2.675, 2.681 | 3.573, 3.582, 3.583 | 1.34 |
| 4 | 4.954, 4.975 | 6.819, 6.843 | 1.37 |
| 8 | 9.713, 9.755 | 13.331, 13.400 | 1.37 |

The integer form costs at most 1.37 times f32 per octave, under the 1.5
the user set, so it ships, with an exact CPU twin.

### The integer form

Lattice coordinates are Q14 (1/16384 of a cell), from the pixel's index:
`((2 i + 1) r) >> 1` with r the octave's Q14 cells per px, an integer the
CPU computes (round(16384 lacunarity^k / scale px)). Every quantity is
unsigned with a bias of 2^28 (so floor division and shifts are those of
non-negative numbers), and every product stays below 2^31. The falloff
runs in Q16 (t >> 12, then two squarings >> 16), the sum in Q30. An
octave's value in Q12 (`sum >> 13`, a floor) times its Q12 weight is
summed; the field is that sum over 2^24, exact in a float. z comes from
the CPU in Q14 (modulo 2^30) in two 16-bit halves, so it is exact in the
block's floats. The block: p[0..7] each octave's r, p[8..23] its z, p[24..31]
its weight, misc.w the octave count.

`ygfx_simplex_value(g, s, ix, iy)` runs the same integers on the CPU.

| Check | Iris Xe | WARP | SwiftShader |
|---|---|---|---|
| GPU against `ygfx_simplex_value()`: 6 fields, 227000 pixels (1 to 8 octaves; lacunarity 1, 1.7, 2, 3; gain 0.4 to 1; z 0, 3.37, -2.25; cells 2 to 200 px) | 0 wrong | 0 wrong | 0 wrong |
| a turned field (30 degrees, 3 octaves): largest value; mean | 0.853; 0.0061 | the same | the same |
| refusals (scale 0, 9 octaves, lacunarity 9, gain 1.5, a POLYGON aperture, a cell below 1/32 px) | 6 of 6 | 6 of 6 | 6 of 6 |

The integer form against the same noise in double (the same lattice,
keys and gradients, real arithmetic) at 262144 lattice points, one
octave: within 1.4e-3 of full scale. That is the CPU twin's distance from
an ideal f32/f64 simplex; the twin itself is exact. Q12 coordinates and a
Q12 falloff gave 2.2e-2 (the falloff's fourth power lost the most), Q12
coordinates with a Q16 falloff 2.4e-3.

### What the field is

From `ygfx_simplex_value()`, 1024 x 1024 fields, cell 32 px, lacunarity
2, gain 0.5:

| Octaves | SD | min, max |
|---|---|---|
| 1 | 0.425 | -0.978, 0.978 |
| 2 | 0.317 | -0.947, 0.946 |
| 3 | 0.278 | -0.929, 0.913 |
| 4 | 0.261 | -0.868, 0.909 |
| 6 | 0.249 | -0.839, 0.862 |
| 8 | 0.246 | -0.826, 0.855 |

The mean is 0 within 1e-4. contrast scales the field: for an SD of c, set
contrast to c / SD.

Radial power spectrum, power per octave band of frequency (4 seeds; the
peak band = 1):

| Band, cycles/px (lattice 1/32 = 0.031) | 1 octave | 4 octaves |
|---|---|---|
| 0.002 to 0.004 | 0.013 | 0.013 |
| 0.004 to 0.008 | 0.054 | 0.052 |
| 0.008 to 0.016 | 0.276 | 0.267 |
| 0.016 to 0.031 | 1 | 1 |
| 0.031 to 0.063 | 0.732 | 0.929 |
| 0.063 to 0.125 | 0.0077 | 0.237 |
| 0.125 to 0.25 | 0.0022 | 0.059 |
| 0.25 to 0.5 | 0.0015 | 0.012 |

One octave peaks at 0.020 cycles/px (0.64 of the lattice frequency) and
spreads over about four octaves of frequency: gradient noise is not
band-limited, and the fBm sum widens it. A stimulus that needs a stated
band takes filtered noise or gratings.

Isotropy: the power of 12 angle sectors of the ring 0.5/32 to 2/32
cycles/px is 0.926 to 1.105 of their mean (4 octaves: 0.935 to 1.091);
the axes 1.05, the diagonals 1.02. The cube-edge gradients make the field
slightly anisotropic, as 3D simplex noise with these gradients is.

Correlation with distance (one octave, cell 32 px): 0.57 at 8 px, 0.02 at
16 px, 0 beyond, in x and y alike. With z (evolution): 0.98 at 0.05
cells, 0.92 at 0.1, 0.60 at 0.25, 0.07 at 0.5, 0 at 1.

### Cost

`gfx_bench --only "v0.10 simplex"`, Iris Xe, AC, the guard, three rounds,
GPU ms over an empty frame, 1920 x 1200:

| Workload | ms |
|---|---|
| SIMPLEX, 1 octave, cell 32 px | 1.834, 1.829, 1.847 |
| 2 octaves | 3.396, 3.419, 3.440 |
| 3 octaves | 5.014, 4.956, 4.991 |
| 4 octaves | 6.554, 6.593, 6.535 |
| 8 octaves | 13.07, 12.98, 12.91 |
| UNIFORM, 1 px checks (reference) | 0.408, 0.390, 0.412 |

About 1.6 ms per octave full screen; a smaller box costs in proportion
to its pixels (not measured). A static field belongs in a target,
rendered once in a setup pass (v0.8); an evolving one costs this every
frame. Of a pixel's eight hashes per octave, four make the two z layers'
keys; the layer depends on the skewed coordinate, so they cannot move to
the CPU.

### Mutations and builds

| # | Fault | Result |
|---|---|---|
| v10-00 | the GPU's second simplex corner a unit off | caught (GPU against the CPU twin) |
| v10-01 | the GPU's z layer ignored | caught |
| v10-02 | the octave weights not normalized | caught (the SD by octaves, CPU) |
| v10-03 | the integer falloff at lower precision | caught (against double, CPU) |
| v10-04 | a POLYGON aperture accepted under SIMPLEX | caught (refusals) |

The whole list: 88 of 88 against the default test (1293 s). Builds: MSVC 19.44 C and C++17, MinGW gcc 16.1
C99, C11 and C++17 (warnings as errors); the full test on the three
renderers passes (278 s); the default run takes 61 s.

The WSL and emcc runs on v0.10 (with other work running, so slower than
v0.7's): gcc 11.4 -O2 and ASan + UBSan, the CPU half and llvmpipe (SIMPLEX
bit for bit there as well), default 41 s, full 86 s, sanitized 46 s; the
pinned digests equal; emcc 6.0.10, the CPU half 1.1 s. All pass.

## Next

1. Done 2026-10-06 for v0.7.0: kind-specialized vector programs (three
   built). Left from them: **paint cost** (OKLAB and RGB paint add 0.5 to
   0.9 ms to a full-screen RRECT, outside what specialization removes:
   measure the paint's uniform reads and the stop loop first); **PLAIN
   STAR and PLAIN ELLIPSE** programs (measured 33 and 45 % faster; no
   bar); **the dash loop as a constant** (measured once).
2. Done 2026-10-06 for v0.6.0: sampler-filtered chroma, measured and
   deleted (see "v0.6: video chroma").
3. Done 2026-10-06 for v0.5.0: WSL, llvmpipe, sanitizer and emcc runs
   (see the v0.4 build notes).
4. **Blur pyramid** above sigma 8, opt-in: the probe measured under 1 ms
   full screen at every sigma with an error of about 0.012
   ([text_probe.md](text_probe.md)), against 5.96 ms at sigma 4 for the
   direct taps. Build it when a stimulus needs a large sigma on a large
   layer.
5. Done for v0.7.0 and v0.8.0: `crun_desc.rays`, and the rays program
   picked by itself for turned and aliased runs ("v0.8").
6. **V2, a corner rule** for turned glyphs, where the rays' largest error
   (0.56) and the corner jump (0.44) are; the harness first.
7. **Blur weights from the CPU** (a uniform array instead of an `exp` per
   tap), and bilinear tap pairing re-timed on the Iris Xe: not timed in
   v0.6.
8. Done for v0.7.0: gfx_bench on ysp/outline.h; the test cross-checks the
   two builders' sets by pixels (they differ in words where the resolve
   splits curves).
9. Done for v0.8.0: setup passes (`ygfx_begin_setup()`,
   `ygfx_end_setup()`).
10. Done for v0.10.0: gradient noise (NOISE SIMPLEX), integer, with an
   exact CPU twin. Left: its cost (1.6 ms per octave full screen).
11. Done for v0.12.0: stimulus capture during a trial (see "v0.12:
   stimulus capture"). Left: the map's 2.6 ms on the frame thread. A
   D3D11 staging texture of our own, copied by the immediate context and
   mapped on the writer thread (the device has multithread protection with
   `desc.d3d11_video`), would take it off the frame thread on Windows;
   neither built nor measured.
12. **Lower input-to-photon latency for closed-loop trials** (requested
   2026-10-09; cursor feedback, mouse tracking), opt-in per trial, with
   ysp/screen.h:
   - Late frame start: wake at the planned vblank minus a budget, then
     poll input, draw and present. The budget is a high percentile of the
     measured frame-start-to-present-return time plus a margin, raised
     after a miss and lowered only on evidence (as the depth learner's
     slack histogram does). Misses are logged. Expected gain: most of the
     idle time after the previous flip, about 10 to 12 ms at 60 Hz.
   - Late layer: the static scene drawn into a render target early, then
     after the late input poll only the input-dependent items and the
     target's composite. This shrinks the budget to the late draws.
   - Late latching (GPU reads the newest input from a persistently mapped
     buffer, as in nlguillemot/LateLatching): GL ES 3.0 cannot do it;
     probe only whether ANGLE's D3D11 backend exposes
     GL_EXT_buffer_storage. Its gain over the late layer is the late draws'
     GPU time, so build it only if a measurement shows that is large.
   - Measure first: input-to-photon with a Teensy as a USB HID mouse
     (1 kHz polling) that also reads the photodiode on its own clock, late
     start off and on, on DXGI_FLIP and COMPOSITION, with the photodiode's
     screen position recorded (scanout runs top to bottom).

## CI

The `screen` job builds ysp/gfx.h with SDL3 on Windows, Linux and macOS and
runs the pixel test where software GL exists:

| Runner | Renderers | Source |
|---|---|---|
| Linux | Mesa llvmpipe | `libegl1 libegl-mesa0 libgl1-mesa-dri` from apt |
| Windows | ANGLE on WARP and on SwiftShader | Electron v38.8.6's zip, SHA-256 `366ae2b4aa9e6bc89b98c8b5831d46303e45cd49b2970c2b3921de5f2fcfc2e1`, cached; its ANGLE reports `2.1.25848 git hash: 85cc0e0b9cc3` |
| macOS | none: the CPU checks only | |

Electron v44's zip no longer has `libEGL.dll` and `libGLESv2.dll`. v38.8.6
is the newest release line that was checked and still has them. The test
gave the same errors with it as with the Docker copy. The wasm job runs the
compile check and the CPU half of the test in node. The sanitizer job runs
the CPU half: under ASan, Mesa leaves 112 bytes in 2 allocations from a
module it unloads, which a suppression cannot name, so the test runs GL
under a sanitizer only when `YGFX_TEST_DEVICES` asks for it. Locally,
with GL on, everything passed under ASan and UBSan apart from that leak.

## v0.15: the first trial's calls

v0.15.0 adds three calls that every trial example had written out, for
the first-trial work of 2026-10-10 (docs/response.md, "The first trial
(v0.4.0)", has the decisions and the line counts):

- `ygfx_open_screen()`: OPEN START's hook, `ygfx_open()` and
  `ygfx_prime()` in one call. The trial examples in `examples/response/`
  (not `trial_two_keyboards.c`) now open this way, so they get the
  overlapped compile, which only `gfx_trial.c` had.
- `ygfx_show_until_key()`: an instruction or break screen. It is a frame
  loop in the header because an instruction screen is not timed and every
  program wrote the same loop; a timed screen stays in the program's loop.
- `ygfx_text_make()`, with `ysp/outline.h` included first: text from a
  font file by its advances, wrapped at spaces and centered. Not shaping:
  `pack/layout` (Skribidi) does that, and it is not a header. The call is
  in this header, behind the include of `ysp/outline.h`, because
  `ysp/outline.h` is pure computation and this header builds on
  `ysp/screen.h` and `ysp/color.h` only.

### What the text's first frame costs

The text program is made at the first `ygfx_cset_make()`, and ANGLE makes
its D3D shader objects at its first draw (see "v0.11: priming"). So
`ygfx_text_make()` calls `ygfx_prime()` when its set made a program. It
compares the program counts of `ygfx_program_stats()` before and after,
so a second text, which makes no program, does not prime.

Measured with a scratch program on 2026-10-11 (Windows 11, the Iris Xe,
a 1920 x 1200 window in the foreground, AC, under the timing guard, three
runs each): `trial_keyboard.c`'s instructions in Segoe UI at 32 px, made
by `ygfx_text_make()` (prime) or by the same calls without the prime
(no prime), then 180 frames that draw the text.

| | Make the text | First frame's CPU | ygfx_end() of it | Frames 3 to 179, CPU p50 |
|---|---|---|---|---|
| no prime | 786 to 1695 ms | 10.1 to 11.0 ms | 10.1 to 10.9 ms | 0.18 to 0.21 ms |
| prime | 1499 to 2193 ms | 0.76 to 1.30 ms | 0.74 to 1.25 ms | 0.16 to 0.29 ms |

The make time is the text program's compile: no program cache was given
(`desc.cache`). With the test's warm cache, the make took 17 ms (MSVC
build) and 43 ms (MinGW build). The prime adds the draw of every program
made so far (15 here). In these runs no first frame was late; the
10 ms is a frame's budget at 100 Hz and two thirds of one at 60 Hz, on a
frame that a program also uses for other work. The window runs had 0 to 6
late frames among frames 3 to 179 in both modes, so a window in a
desktop session is not a quiet display; the first frames were not among
them.

### Tests and mutations

`tests/adapt/gfx_test.c`'s part "v0.15 first" (ANGLE on the Iris Xe,
MSVC 19.44 and MinGW-w64 gcc 16.1): the open in one call and its refusal
of a taken `on_context`; the loop's arguments and its end at `max_ns`
(the last frame is the first planned at `max_ns` or later); a font that
does not open, named in the message; a text's items, lines, line height,
centering, wrap, and ink inside its bounds only (495 pixels inside, 0
outside); the prime of the first text alone. Mutants `ft-01` to `ft-05`,
5 of 5 caught. The key path needs SDL input, which the headless test does
not have. A scratch program checked it in a window on 2026-10-11, with
SDL key events pushed from a second thread: a space pushed before the
first frame and a J (not in the list) did not end the loop; a space 1.5 s
later did, and `*key` held its press (scancode 44, its time). OS input
did not reach the window: the session gave no foreground to a program
started from the background, so `SendInput()` went to another window.

## v0.14.0: PPM only

The user decided on 2026-10-10 that capture writes PPM only: the stored
PNG measured slower (14.7 to 18.5 ms a 1920 x 1200 frame against 8.0 to
11.4 ms), and every tool the files go to opens PPM. v0.14.0 removes
`YGFX_IMAGE_PNG`, the PNG writer and its CRC-32 and Adler-32 (about 130
lines of the header, 4 KB of tables on the writer's stack), and the PNG
reader and checksums of the test.

| Question | Decision | Why |
|---|---|---|
| `ygfx_capture_desc.format`, `YGFX_IMAGE_PPM` and the `format` argument of `ygfx_image_write()` | Removed; the version is 0.14.0, not 0.13.1 | With one value, the field is always 0 and the argument always `YGFX_IMAGE_PPM`: an argument every call passes for nothing. Capture shipped in v0.13.0 the same week; every caller in the repository is changed (6 examples, the test, the compile check). A caller that set the field or passed the argument now fails to compile, which says what changed; a field kept with one value would accept `YGFX_IMAGE_PNG`'s old value 2 and fail only at run time |
| The comment | One `#` line for each line of the comment | As before; the test now writes a two-line comment and reads both lines back, and mutant `cp-17` (the second line without its `#`) checks it |

## v0.13.1: render targets under WebGL

The gallery compiled for the browser ([web.md](web.md)) drew page 1 with
133 draws refused in Chrome 154: `GL_INVALID_OPERATION: Feedback loop
formed between Framebuffer and active Texture`. The scene target's texture
stayed bound to a texture unit after the output stage sampled it, and the
next frame's draws into the scene had programs with a sampler on that
unit. WebGL refuses such a draw whether or not the shader reads the
sampler; GL ES calls it undefined only when the texture is sampled, so
ANGLE and Mesa drew. A pass into a target now unbinds that target's
texture from every unit first, through the cached bindings, so it costs
nothing when none matches. After it: no refused draw on any of the 8
pages. The native tests cannot see the fault (no GL ES driver refuses the
draw), so it has no mutant.

## v0.13: stimulus capture

"Next" item 11. A trial can keep the frames it showed: every Nth frame, or
the frames the caller marks, as files or through a callback, each with its
frame index and flip record. The manual's CAPTURE section is the
reference; this section says why it has this form and what it costs.

### Design

- **The copy is the output stage's codes.** `ygfx_end()` reads the
  rectangle of framebuffer 0 after the output stage and before the
  present, so the copy is what the CLUT and dither made, and the same
  bytes as `ygfx_read_output()` (tested bit for bit). The screen's patch
  and codes, which `yscr_flip()` draws later, are not in it.
- **No wait on the frame thread.** `glReadPixels` into a bound pixel pack
  buffer returns before the copy, and a fence (`glFenceSync`) marks its
  end. A later `ygfx_end()` polls the fence with a zero timeout and no
  flush (the present flushes), then maps the buffer
  (`glMapBufferRange`). Both are GL ES 3.0 core; WebGL 2 has the fence but
  not the map, so the five calls load apart from the required table and
  capture answers `YGFX_ERR_NOT_IMPLEMENTED` without them.
- **The map is the cost.** On ANGLE's D3D11 backend the map is where the
  pixels come to the CPU: 2.6 ms mean, 5.1 ms p99 for 1920 x 1200 on the
  frame thread (below). The copy's call and the fence cost nothing
  measurable. So `ygfx_end()` maps at most one buffer per frame, the
  oldest; after a stall the backlog spreads over the next frames instead
  of one frame paying for all. `desc.manual` maps nothing in a frame:
  the copies wait in their buffers until `ygfx_capture_pump()` between
  trials.
- **The writer reads the mapped buffer.** The buffer stays mapped until
  the writer is done with it, so the frame thread copies no pixels
  itself. The writer is ysp/rt.h's pump at below normal priority: it
  must never take a core from the frame loop, and a pump already gives a
  ring that copies nothing, a lock-free done counter for the frame thread
  to poll, and a drain at stop. Its ring holds every slot, so a submit
  cannot fail.
- **Drop, never block.** A buffer is busy from its copy until the writer
  is done. A chosen frame with no free buffer is not copied: it counts in
  `stats.dropped` and pushes `YGFX_EV_CAPTURE_DROP` (the frame index), so
  the data file says which frames are missing.
- **The flip record.** A record comes with the `yscr_begin()` after its
  flip, in `yscr_frame.done`; `ygfx_begin()` matches it to a buffer by
  frame index. A copy waits for its record up to 30 frames before it goes
  to the writer, so the file's comment carries the onset. A frame filled
  by hand (`done` is NULL) and a headless gfx get no record and do not
  wait; `ygfx_capture_stop()` writes what is left without one
  (`stats.no_record`).
- **Rows.** GL reads bottom first unless the swap path turned the rows
  (DXGI_FLIP and COMPOSITION, "The row order of the back buffer"). The
  writer and the callback get the top row and a signed stride, so nothing
  turns the rows in memory.
- **No allocation in the frame.** `ygfx_capture_start()` allocates the
  state and makes the buffers; the frame work moves pointers and calls GL.
  The writer opens files on its own thread.
- **Threads.** With `YRT_NO_THREADS` there is no pump, so capture is
  always manual there, as ysp/device.h's manual mode is its threadless
  form. A wasm build without `-pthread` has the pump's code but cannot
  start it: start then fails and says so, and `desc.manual` works.
- **The backend.** Version 4 adds `capture_make`, `_read`, `_poll`,
  `_unmap` and `_free`. `ygfx_open()` takes version 3 tables too and
  reads no capture call from them. The null backend keeps its buffers in
  memory (black frames), so capture runs on SIM, in CI and in the tests.

### The file format

One writer, `ygfx_image_write()`, for capture and for the examples'
screenshots (gfx_gallery, gfx_text, gfx_rdk, gfx_layout had four copies
of one PPM writer). No compression: include/ysp takes no new dependency,
and a deflate of its own was not built or timed. Two formats were built
and timed in v0.13, 10 writes of 1920 x 1200
from memory into C:\tmp on the laptop's NVMe SSD (Samsung
MZVL21T0HDLU), three guard sessions on AC:

| Format | ms per frame | MB/s of pixels |
|---|---|---|
| PPM (P6) | 7.95 to 11.43 | 605 to 870 |
| PNG, stored blocks (removed v0.14.0: measured slower, PPM opens everywhere needed) | 14.71 to 18.47 | 374 to 470 |

The PNG's extra time was its two checksums (CRC-32 by slicing by 4, and
Adler-32) over 6.9 MB. PPM keeps up with every frame at 60 Hz on one
writer; PNG held every second frame. v0.14.0 writes PPM only (see
"v0.14.0: PPM only" above). The comment is the frame index, the predicted
onset, the rectangle and the record (onset, planned, target, residual,
dropped, flags, path, tier), one PPM comment line for each line of it.

### Measured: the cost of a captured frame

examples/gfx/bench.c, `--only "v0.12 capture"`, headless at 1920 x 1200
on the Iris Xe (ANGLE 2.1.23876), the guard held, AC, 2026-10-10. Paced
as a 60 Hz loop: after each frame `glFlush` (the present's) and
`glFinish`, so the GPU is idle when the next frame starts. cpu: begin to
end; gpu: end's return to glFinish's. Medians of 25 rounds of 12 frames,
the rows interleaved; the "- off" columns are the median of the
per-round differences:

| Workload | gpu ms | ms - off | cpu us | us - off | cpu p99 us |
|---|---|---|---|---|---|
| empty frame | 1.133 | 0 | 78 | 0 | 169 |
| empty frame, capture every frame | 1.883 | 0.773 | 2375 | 2245 | 5157 |
| 100 gabors | 3.510 | 0 | 388 | 0 | 4077 |
| 100 gabors, capture every frame | 4.074 | 0.564 | 2530 | 2123 | 5170 |
| 100 gabors, every 4th frame | 3.498 | 0.129 | 972 | 611 | 4708 |
| 100 gabors, every frame, the map in `ygfx_capture_pump()` | 4.195 | 0.736 | 207 | -126 | 370 |

`ygfx_capture_pump()` after each frame of the last row (the previous
copy's fence, map and hand-off): 2593 us mean, 5060 us p99. So the copy
costs the GPU 0.56 to 0.77 ms and the frame thread nothing measurable,
and the map costs the frame thread about 2.6 ms. Every 4th frame costs
0.6 ms a frame on average, the map's 2.6 ms in one frame of four.

Two runs before this one timed frames back to back (GPU bound, no flush
per frame). There ANGLE blocked in `ygfx_end()` on a full queue, and the
fence polls, which flush, fed the GPU earlier, so frames with capture
measured cheaper than frames without (-571 us). That method was dropped.

### Measured: no flip moved

examples/gfx/capture.c: 100 drifting gabors and a moving bar on gray,
ysp/screen.h's AUTO path (COMPOSITION), the guard held, AC, 2026-10-10,
one session, the rows in this order. Fullscreen is the panel's 1920 x 1200;
the window is 800 x 600. Files went to the NVMe SSD and were deleted
between rows. "Dropped" counts records with `dropped` > 0; "long" counts
onset intervals over 1.5 periods; end() is `ygfx_end()`'s CPU time.

| Path | Capture | Frames | Late targets | Dropped | Early | Interval max ms | Long | end() p50 / p99 / max us | Captured | Capture drops |
|---|---|---|---|---|---|---|---|---|---|---|
| fullscreen | off | 1800 | 0 | 0 | 0 | 16.672 | 0 | 129 / 305 / 642 | | |
| fullscreen | every frame, no files | 1800 | 0 | 1 | 0 | 16.675 | 0 | 1277 / 3918 / 5609 | 1802 | 0 |
| fullscreen | every 4th, no files | 1800 | 0 | 0 | 0 | 16.673 | 0 | 223 / 2773 / 4414 | 451 | 0 |
| fullscreen | off | 1800 | 0 | 0 | 0 | 16.675 | 0 | 139 / 452 / 842 | | |
| fullscreen | every frame, PPM | 600 | 0 | 0 | 0 | 16.675 | 0 | 1583 / 4189 / 5431 | 602 | 0 |
| fullscreen | every 4th, PPM | 1800 | 0 | 1 | 0 | 16.688 | 0 | 227 / 2675 / 4126 | 451 | 0 |
| fullscreen | every 2nd, PNG | 600 | 0 | 0 | 0 | 16.672 | 0 | 982 / 2640 / 3842 | 301 | 0 |
| fullscreen | 1 in 60 marked, manual, 32 slots | 1800 | 0 | 1 | 0 | 16.674 | 0 | 148 / 494 / 1082 | 30 | 0 |
| window | off | 1800 | 0 | 1 | 0 | 33.331 | 1 | 203 / 518 / 1106 | | |
| window | every frame, no files | 1800 | 0 | 0 | 0 | 16.670 | 0 | 598 / 1482 / 2782 | 1802 | 0 |
| window | every 4th, no files | 1800 | 0 | 0 | 0 | 16.669 | 0 | 240 / 1000 / 1631 | 451 | 0 |
| window | off | 1800 | 0 | 2 | 0 | 33.331 | 2 | 207 / 557 / 8906 | | |
| window | every frame, PPM | 600 | 0 | 0 | 0 | 16.669 | 0 | 635 / 1356 / 1853 | 602 | 0 |
| window | every 4th, PPM | 1800 | 0 | 0 | 0 | 16.672 | 0 | 243 / 1119 / 1962 | 451 | 0 |
| window | every 2nd, PNG | 600 | 0 | 0 | 0 | 16.669 | 0 | 425 / 1435 / 1823 | 301 | 0 |
| window | 1 in 60 marked, manual, 32 slots | 1800 | 0 | 1 | 0 | 16.669 | 0 | 203 / 538 / 1230 | 30 | 0 |

The PNG rows are v0.13's; v0.14.0 has no PNG. No row with capture had a
late target, an early flip or an onset interval
over 16.69 ms. The four records with `dropped` set in capture rows have no
long interval next to them: the drop is not a gap between consecutive
frames (not looked into further). The control rows in the window had the
only real gaps (33.3 ms), so nothing here ties a gap to capture. Every
chosen frame was written; the writer kept up with every frame at 1920 x
1200 in PPM (at most 5 of 6 buffers busy) and with every 2nd in PNG.

So every frame at 1920 x 1200 holds the 16.7 ms budget on this machine
with this load: `ygfx_end()` took 3.9 to 4.2 ms at p99 and 5.6 ms at most
with capture, against 0.3 to 0.5 ms without. A frame loop with more than
about 10 ms of its own CPU work leaves no room for the map: then capture
every 4th frame (the map in one frame of four), a smaller rectangle
(`desc.x`, `y`, `w`, `h`), or `desc.manual` (no map in a trial; size
`desc.slots` for the trial's frames). The knobs are `desc.every`, the
rectangle and `desc.manual`.

One session only; the AMD Linux rig and other GPUs are not measured.

### Tests

`test_v12_capture_cpu` (the null backend on SIM, and the writer): a PPM
of a 1500 x 20 image, top first and bottom first (a row wider than the
1024-pixel conversion chunk), read back by the test's own reader, with a
two-line comment (v0.14.0; v0.13 also wrote and read back PNG); refusals; manual mode with every 3rd frame, a mark and 4
slots: chosen, read, dropped (2) and written counts, the drop records,
oldest first, the pump's max, the flip record matched by index in each
callback and file, the last frame written without its record at stop, a
frame filled by hand written at once; the writer thread at every frame
with copies done after 2 polls: written + dropped = chosen, order, records;
a version 3 backend refused with `YGFX_ERR_NOT_IMPLEMENTED`. GL part
`v0.12 capture`: 6 whole frames through the writer thread and a 45 x 17
rectangle in manual mode, each equal to `ygfx_read_output()` of the same
frame, bit for bit (which also shows the pack buffer is unbound). The
compile check captures 3 frames on SIM (every 2nd and a mark). CI runs
`gfx_capture --sim` with files, threaded and manual.

### Mutations

`cp-01` to `cp-16` in tests/mutate/gfx.toml, 16 of 16 caught (MSVC, the
Iris Xe for the two GL ones; v0.14.0 removed the PNG writer's three,
`cp-07` to `cp-09`, and added `cp-17`; on 2026-10-10 the 14 capture mutants of v0.14.0 ran: 14 of 14 caught, the list 125 mutants); `cp-15` and `cp-16` came with the port to
the conventions (a capture call's message, `ygfx_image_write()`'s `err`).
After the merge the whole list ran again on 2026-10-10: 127 of 127
caught. `cp-04` (a record attached without its
index) survived the first run: SIM's records came before a newer copy
waited, so no record could go astray. A frame with another frame's record
in its `done` list now checks it.

### Builds

MSVC 19.44 (msvc-full: the test, the compile checks as C11 and C++17, the
examples), MinGW-w64 gcc 16.1 (the compile check as C99, C11, C++17 and
C++17 with `YRT_NO_THREADS`; the test with and without `YRT_NO_THREADS`).
The test passed on ANGLE D3D11 hardware, WARP and SwiftShader. Not built
here: Linux, emcc, the sanitizers (CI runs them).

### Decisions

- The repository conventions (README.md, Conventions; v0.12.0) hold for
  capture, which was built beside v0.12.0 and merged after it as
  v0.13.0. `ygfx_capture_start()` and `ygfx_capture_stop()` begin and
  end an activity on an open gfx, so they are start and stop, not open
  and close. Each capture call goes through the same message wrapper as
  the frame calls, so a failure always leaves `ygfx_error()`;
  `ygfx_image_write()` has no gfx and writes its message into the
  caller's `err` buffer. The frame's predicted onset is
  `ygfx_capture_frame.predicted_ns`. The writer thread is ysp/rt.h's
  pump, opened with `yrt_pump_open()` and closed (drained) with
  `yrt_pump_close()`.
- PNG was kept beside PPM in v0.13 although it measured slower (see
  "The file format"). The user removed it on 2026-10-10 (v0.14.0).
- examples/outline/font.c keeps its own PGM writer: it includes only
  ysp/outline.h, and `ygfx_image_write()` would pull in ysp/gfx.h and SDL.

## v0.12.0: a message for every failure

v0.12.0 applies the repository conventions (README.md, Conventions) to
this header. The sweep found two things to do and one not to do:

- **Every failing call on a gfx writes `ygfx_error()`.** About half of
  the frame and upload calls returned a bare code: `ygfx_draw()` outside
  `begin..end`, a full draw queue, `ygfx_begin_target()` out of turn, a
  bad texture id in an update. The message then belonged to an older
  failure, and a caller that printed it after a failed call read the
  wrong reason. Each public call now runs inside a wrapper, as in
  ysp/screen.h v0.6.0: it hides the first byte of the message, runs the
  call, and keeps the message the call wrote, or writes "ysp_gfx:
  <call>: <code's text>", or puts the byte back after a success. The
  makers of textures, targets, buffers, pipelines and curve sets do the
  same when they return a 0 id. The cost is one more call and two byte
  writes per call, also per `ygfx_draw()`; `snprintf()` runs only on
  failure. Not measured: the spec for this change asked for no timing
  runs, so `examples/gfx/bench.c` is the check if a frame's CPU time is
  in doubt.
- **The const calls keep their codes.** `ygfx_describe()`,
  `ygfx_hit_index()` and `ygfx_simplex_value()` take a const gfx: they
  read it and return the code.
- **No time field changed.** The desc and the stimuli hold no time, by
  design (NOTHING READS A CLOCK): a frame gets its time from
  `yscr_frame`, already `int64_t` ns. `ygfx_programs.open_ns` and
  `store_ns` are already `int64_t` ns. Rates (`sf`, the drift `tf_hz` in
  USAGE) are not times.

`ygfx_open_start()` stays: it is an open variant (README, Lifecycle
verbs).

Checked on 2026-10-10 (Windows 11, MSVC 19.44 /W4 /WX, and MinGW-w64 gcc
-std=c99 and -std=c++17 -pedantic for USAGE, then with memset; the merge
made the USAGE descs `= { 0 }`, C only, as README.md, Conventions says):
`tests/adapt/gfx_test.c`
checks the message after a failing end, draw, begin, texture and
texture_update, and that a success keeps it. Mutants cv-01 (a failure
that writes nothing) and cv-02 (a success that drops the message) are
caught.

**What the message costs a draw.** Every public call on a gfx hides the
old message's first byte on entry and puts it back on success, so each
call stores one byte of the cold message buffer. Measured on 2026-10-10
(Iris Xe laptop, Windows 11, AC power, MSVC 19.44 /O2, the measurement
lock held, base `0a16f22` and v0.12.0 alternated): 256 `ygfx_draw()`
calls a frame for 400 frames on the simulated display's null backend, so
the queue is the whole cost. Median ns a draw over 6 runs each: v0.11.1
75.6 (runs 73.0 to 85.5), v0.12.0 75.8 (70.7 to 84.8). The difference
is under the run-to-run spread. `gfx_bench --only gabors` (1 to 1000
draws a frame, ANGLE on D3D11) and `gfx_rdk_bench` over 5 alternated
rounds also showed no difference above their noise, which is 10 to 30 %
a workload on this laptop. So the hot calls keep the same wrapper as the
others; no direct path.

## v0.11.1: the open in two calls

`ygfx_open_start()` does the first half of `ygfx_open()`: the desc's
checks, the backend, the buffers and textures, and every built-in program
submitted with `pipeline_src.deferred`. `ygfx_open()` on the started
handle does the rest: `pipeline_finish` on each program (the link status),
the cache's stores and the open record. Called from ysp/screen.h v0.5.2's
`desc.on_context`, the 14 programs compile on ANGLE's threads while
`yscr_open()` settles (docs/screen.md, "Compiles during settling", for
the hook and its measurements).

### Design

- The start copies the desc. `ygfx_open()` on a started handle uses the
  copy and may get NULL, so a hook may pass a desc on its stack. What the
  desc points to (the cache, the calibration, the ring) must live until
  `ygfx_open()` returns, as before.
- Between the two calls the handle is not open: `ygfx_is_open()` is false
  and every call but `ygfx_open()` and `ygfx_close()` returns
  `YGFX_ERR_CLOSED`. No program has a link status yet, so a draw then
  would use programs nobody checked.
- The start submits only on a backend that reports
  `caps.parallel_compile`; the GL backend reports it when the context
  lists `GL_KHR_parallel_shader_compile` (ANGLE on D3D11 hardware and
  WARP, Mesa llvmpipe: all have it). Without it, `glLinkProgram()` may
  compile at once, and a 2 s compile in the hook would hold the warm-up
  and settling; the start then makes nothing and `ygfx_open()` does the
  whole open after settling, as v0.11.0 did. The describe line ends with
  `open_start=parallel` or
  `open_start=serial(no GL_KHR_parallel_shader_compile)`; a blocking open
  adds nothing.
- Program binaries from the cache load in the start too (16 to 40 ms for
  14 on the Iris Xe; ANGLE reports the link status of a binary at once),
  so with a warm cache `ygfx_open()` has nothing left to do.
- A start whose screen then fails to open (strict settling) leaves GL
  objects in a context that is gone. `ygfx_close()` now checks the
  screen: when it is closed or its GL generation moved, it frees the
  memory and deletes nothing. `ygfx_open()` on such a handle fails with
  "the screen closed after ygfx_open_start()".
- `ygfx_program_stats().open_ns` is the two calls' time together, not the
  settling between them.
- `caps.parallel_compile` is a new field at the end of
  `ygfx_backend_caps`; the backend version stays 3. A backend that does
  not set it gets the v0.11.0 open.

### Measured

This laptop (Iris Xe, ANGLE 2.1.23876), on battery, the guard held, the
session unlocked and the display on, 2026-10-10; 10 opens per row,
interleaved; the probe and the conditions are in docs/screen.md. ms from
the `yscr_open()` call until `ygfx_open()` returned, median (range):

| Configuration | Cache | One after the other | Start in the hook | `ygfx_open()` after `yscr_open()`, hook |
|---|---|---|---|---|
| COMPOSITION window | cold | 5374 (5184 to 5614) | 3405 (3266 to 3588) | 886 (740 to 1068) |
| DXGI_FLIP window | cold | 5396 (5263 to 5609) | 3407 (3301 to 3685) | 880 (776 to 1156) |
| COMPOSITION fullscreen | cold | 3852 (3794 to 3971) | 3468 (3335 to 3636) | 2668 (2522 to 2830) |
| DXGI_FLIP fullscreen | cold | 3887 (3629 to 4145) | 3459 (3241 to 3626) | 2794 (2579 to 2972) |
| COMPOSITION window | warm | 2546 (2540 to 2554) | 2520 (2517 to 2530) | 0 |
| DXGI_FLIP window | warm | 2553 (2549 to 2560) | 2524 (2517 to 2532) | 0 |
| COMPOSITION fullscreen | warm | 805 (759 to 905) | 781 (762 to 828) | 0 |
| DXGI_FLIP fullscreen | warm | 717 (671 to 853) | 666 (648 to 741) | 0 |

The same on AC (the laptop was plugged in later the same morning), cold
cache, 5 opens per row, interleaved:

| Configuration | One after the other | Start in the hook | `ygfx_open()` after `yscr_open()`, hook |
|---|---|---|---|
| COMPOSITION window | 4832 (4712 to 5027) | 2567 (2561 to 2572) | 38 (36 to 52) |
| DXGI_FLIP window | 4787 (4733 to 4815) | 2567 (2561 to 2572) | 41 (34 to 44) |
| COMPOSITION fullscreen | 2908 (2901 to 3257) | 2466 (2447 to 2638) | 1780 (1762 to 1970) |
| DXGI_FLIP fullscreen | 2865 (2774 to 2989) | 2459 (2383 to 2619) | 1890 (1815 to 2035) |

The start took 9 to 48 ms in the hook. The compiles took 2.2 to 2.3 s on
AC and 2.8 to 3.2 s on battery: on AC a window's end within 0.04 s of its
2.5 s minimum, on battery 0.9 s after it. Without the extension (a test seam), the hook
gave the times of one after the other (docs/screen.md, "Without parallel
compile").

### Tests

`test_v11_start_cpu` (a fake backend, with and without
`caps.parallel_compile`): the start submits 14 programs and finishes none,
or submits nothing; `ygfx_open()` finishes either with the start's desc;
the handle is closed between the calls; a second start is refused; a
failed start leaves a closed handle; a started handle closed deletes its
objects; after the screen closed, `ygfx_open()` fails and names it and
`ygfx_close()` deletes nothing; the describe tokens. The GL part
`v0.11.1 start`: the start and the finish draw the frame of a blocking
open bit for bit, with the extension, with the seam (serial), and with
the start in a screen's hook (ANGLE D3D11 hardware and WARP, MinGW and
MSVC; Mesa llvmpipe on WSL2), and the hook's open ends with the default
framebuffer bound. Mutants `os-01` to `os-09`, all caught; `v10-06`'s
anchor moved with the describe line.

## v0.11: priming

`ygfx_prime()` draws once with every program the gfx has made so far: the
10 built-in programs, the 3 specialized vector programs, the instanced,
video, text and blur programs made so far, the caller's pipelines and the
output stage. Each draw is a quad of zero size (the stimulus block is
zeros), so no fragment runs and nothing is shown. Then it reads one pixel,
which waits for the GPU.

### What the first use costs

All runs: this laptop (Iris Xe, Docker's ANGLE 2.1.23876), AC, the guard
held, fullscreen on the composition swapchain, the window in front,
2026-10-09. The probe (`C:\tmp\psy-work\settle\gfxwarm.c`, not in the
repository) opens a screen and a gfx, makes 19 stimuli of every kind, then
draws each kind alone for 1 frame (cold) and 3 more frames (warm), with 2
empty frames between kinds. Each process is fresh. "No cache" passes no
program cache; "cold cache" an empty folder (the run stores every
program); "warm cache" the folder a cold run filled. 5 rounds.

Setup calls, ms (minimum to maximum):

| Call | No cache | Cold cache | Warm cache |
|---|---|---|---|
| `ygfx_open()` (14 programs) | 2216 to 2409 | 2237 to 2370 | 11.2 to 18.1 |
| first `ygfx_cset_make()` (the text program) | 451 to 486 | 446 to 491 | 1.1 to 1.9 |
| a second `ygfx_cset_make()` | 0.17 to 0.25 | 0.18 to 0.26 | 0.20 to 0.34 |
| first `ygfx_crun()` with `.rays` (its program) | 160 to 168 | 162 to 188 | 0.7 to 1.2 |
| `ygfx_instances()`, gabor and shape (each a program) | 121 to 150 | 124 to 151 | 0.8 to 1.6 |
| `ygfx_pipeline()` | 137 to 162 | 144 to 156 | 0.6 to 1.0 |
| first `ygfx_blur_make()` | 13.5 to 14.6 | 15.3 to 17.3 | 1.5 to 2.5 |

Texture uploads, every condition (15 runs), ms including a `glFinish()`:
512 x 512 RGBA8, the first 1.2 to 2.1, the second 0.8 to 1.2; 1920 x 1200
RGBA8 2.4 to 4.3; `ygfx_texture_update()` of 1920 x 1200, the first 2.1 to
3.0, then 2.0 to 2.4.

The first frame of each program costs CPU in `ygfx_end()`: ANGLE makes the
program's D3D shader objects at its first draw, from a cache entry too.
`ygfx_end()` of the cold frame, ms; a warm frame's whole draw, end and
`glFinish()` is 1.2 to 4.3 ms:

| Kind | No cache | Warm cache | Warm cache, after `ygfx_prime()` |
|---|---|---|---|
| SHAPE circle (the first kind drawn) | 2.7 to 3.3 | 5.8 to 8.6 | 0.10 to 0.17 |
| GRATING | 2.5 to 3.3 | 2.7 to 3.2 | 0.11 to 0.17 |
| instanced gabor, 400 | 4.7 to 5.9 | 5.7 to 7.1 | 0.12 to 0.14 |
| instanced shape, 400 | 5.1 to 5.6 | 5.9 to 7.5 | 0.10 to 0.17 |
| USER pipeline | 2.5 to 3.9 | 3.1 to 3.4 | 0.09 to 0.11 |
| curve run, exact area | 4.5 to 5.4 | 5.5 to 10.5 | 0.15 to 0.16 |
| curve run, `.rays` | 4.3 to 5.1 | 5.4 to 12.9 | 0.10 to 0.42 |

A kind whose program was already drawn costs nothing more (NOISE SIMPLEX
after NOISE UNIFORM, a second image, the blur's image draw: 0.1 to 0.3
ms). With no cache, the first process of the session also had first draws
of 14 to 27 ms in `glFinish()` (the first shape, both instanced kinds, the
exact-area text), and dropped 4 frames; later processes did not.

The prime itself, 12 runs (no cache, cold and warm cache): 20 programs in
63.4 to 73.4 ms. Frames drawn after it are bit-identical to frames drawn
before it (`gl_v11_prime` in the test). Kept: it moves 2.5 to 12.9 ms per
program out of the first trials, the frame budget is 16.7 ms, and the
first frame of a trial is the one a task cannot afford to lose.

### The first frame after a long setup

The first frame after the setup (frame 0) was late or dropped in 21 of 22
runs whose setup took 2.2 s or more (no cache, cold cache) and in 0 of 13
runs with a warm cache, with and without the prime. `ygfx_end()` and the
GPU were then 0.1 and 1.7 ms: the swap path, not the program, was slow
after 2 s without a present. ysp/screen.h v0.5.0 settles the display in
`yscr_open()`; a long setup after it leaves the swap path idle again.

Found and fixed in ysp/screen.h v0.5.1 (docs/screen.md, "The first frame
after a gap"): on the composition swapchain the first present after 67 ms
or more without one showed a vblank late, whatever the CPU and GPU did; a
thread that waits on the vblank event removes it. Also fixed there:
`ygfx_prime()` right after `ygfx_open()` hung on the composition swapchain
with ysp/screen.h v0.5.0, whose `yscr_open()` ended with the presented
back buffer bound. With v0.5.1, a cold cache, with and without the prime:
frame 0 late or dropped in 0 of 8 runs of the probe above.

### Priming in the trial examples (2026-10-10)

The question: does `ygfx_prime()` change the first frames of the trial
examples? None of the 13 `trial_*.c` called it; `gfx_trial` did.
Measured, not assumed.

Conditions: this laptop (i7-1360P, Iris Xe, ANGLE D3D11-32.0.101.7088,
the internal panel at 60 Hz), ysp/gfx.h 0.11.1, ysp/screen.h 0.5.2, MSVC
Release, AC, each session under the measurement lock (load 5 to 11 % at
the start). Three programs, built from instrumented copies (the
repository's sources unchanged; the patch script is
`C:\tmp\psy-work\docsfix\prime\patch.py`, not in the repository):
an environment variable adds the `ygfx_prime()` call after the stimuli
are made (in `gfx_trial` it makes its own call optional), a file program cache in a
given folder, the CPU time of each `ygfx_end()` and every flip record.
`trial_keyboard` and `trial_stroop`: `--reps 1`, no responses (every trial
times out), the first 480 frames (8 s), fullscreen and windowed.
`gfx_trial`: windowed (it has no fullscreen option), its whole 1.3 s run.
Cache: none (the trial examples' default), cold (an empty folder each
run) or warm (a folder filled before the session). Each round runs every
condition once, interleaved; 3 rounds (`gfx_trial`: 5). A first session
was discarded: its later runs had flip records flagged OCCLUDED (the
window was hidden) and ran at about 16 frames a second; no run below has
an OCCLUDED record.

What each program draws first: `trial_keyboard` the fixation cross
(SHAPE) at frame 0, then the circle or square and the feedback ring, all
SHAPE. `trial_stroop` the block's banner (curve runs) at frame 0, and the
first fixation cross (SHAPE) at frame 88 to 90, the first trial's first
frame. `gfx_trial` the cross at frame 0 and the grating at frame 29 or 30.

`ygfx_end()` of a kind's first frame, ms, minimum to maximum over every
cache condition; "late" counts runs whose flip record of that frame has
`dropped` above 0:

| Program, frame | Runs each | Without the prime | After `ygfx_prime()` |
|---|---|---|---|
| `trial_keyboard`, frame 0 | 18 | 4.3 to 8.7; late in 0 | 0.08 to 0.23; late in 0 |
| `trial_stroop`, frame 0 | 18 | 6.3 to 29.3; late in 5 | 0.15 to 0.38; late in 1 |
| `trial_stroop`, the first fixation | 18 | 2.9 to 6.8; late in 1 | 0.10 to 0.44; late in 0 |
| `gfx_trial`, frame 0 | 15 | 4.5 to 11.2; late in 1 | 0.08 to 0.18; late in 1 |
| `gfx_trial`, the grating's onset | 15 | 3.1 to 6.9; late in 0 | 0.10 to 0.22; late in 0 |

`trial_keyboard` has no first use after frame 0: every shape it draws
uses frame 0's program. Other frames over 2 ms: 1 without the prime (a
later `trial_stroop` frame, 6.3 ms, in one run) and 2 with it (2.05 and
2.13 ms). Late frames elsewhere: 7 without the prime and 5 with it, over
the 51 runs of each, with no pattern. The prime itself took 40.6 to 98.9
ms at setup.

The cache condition made no difference that these runs show: a warm
cache gave the same first-frame cost as no cache. ANGLE makes a program's
D3D shader objects at its first draw, from a cache entry too (above).

Result: without the prime every run paid 2.9 to 29 ms of extra CPU on
frame 0 and on each later first use; with it no run did. Late frames on
those frames: 7 of 102 frames without the prime, 2 of 102 with it. The
counts are small because a drop needs the rest of the frame's work too;
the cost is in every run, against a 16.7 ms frame, and it falls on frame
0 and on the first trial's onset. So the trial examples call
`ygfx_prime()` after they make their stimuli (from 2026-10-10).

## v0.10.5: a pack's textures load as stored; shader contract 2

Found by `examples/pack/trial_images.c` (docs/rig_spec.md 15.5): the
documented load path of a TEXTURE entry, the view's encoding bytes into
`ygfx_texture()`, failed in the common case. The pack tool (v0.1.1) wrote
matrix RGB, full range and the transfer on every texture with primaries 0,
which `ygfx_texture()` refuses; it also refused any encoding on R8 and
RG8, and the tool's default for an 8-bit gray PNG is sRGB. The example
mapped the bytes by hand.

The decision (the tool's half is in docs/pack.md 4.9): the stored bytes
are `texture_desc.enc`, unchanged, and every encoding that `ygfx_texture()`
would refuse is refused at build.

- Linear values carry no encoding (all 0), the only form that every use
  takes: modulation, ADD, coverage and a shader's `ysp_tex0` read raw
  values, and an encoded texture draws as a color image only.
- Gray (R8, RG8) may state a transfer, matrix RGB and primaries DEVICE.
  Gray has no chromaticity of its own; its light is the display's white at
  each level. Other primaries on gray are refused. The alternative, the
  tool linearizing 8-bit sRGB to 8-bit linear, merges the dark codes: the
  256 sRGB codes become 183 linear codes (codes 0 to 6 all 0, 7 to 17 all
  1). To R16F it would keep them, at twice the GPU memory and four times
  the pack size (R16F is stored as f32).
- sRGB codes in RGB need their primaries named in the source description
  (`bt709`: converted through the calibration's chromaticities; `device`:
  the transfer decoded, the RGB shown as the display's). An untagged PNG
  does not say which, so the tool stops rather than guess.

The change here: `ygfx__enc_check()` takes R8 and RG8 with primaries
DEVICE, and the video program's RGB branch spreads a gray texture's level
to rgb and takes alpha from g, by the channel count that every IMAGE block
already carries (`ysp_misc.x`). 10 lines of code. The refusals of an
encoded texture as a modulation, ADD, coverage or shader texture now say
why: those uses read linear values.

| Check (`v0.10.5 gray`, RGBA32F scene, black background) | Iris Xe (D3D11) | WARP | SwiftShader | Tolerance |
|---|---|---|---|---|
| R8 and RG8 with the SRGB and GAMMA22 transfers, 128 texels each (every code once across the two R8 cases), against the formula; RG8 alpha over black | 2.2e-7 | 1.6e-7 | 5.1e-7 | 2e-6 |
| Refusals: BT.709 primaries on R8 (by its own message); an encoding on R16UI; an encoded R8 as a modulation; as coverage. Control: as a color image | 5 of 5 | 5 of 5 | 5 of 5 | all |

Mesa llvmpipe (WSL2, gcc 11.4 `-O2 -Wall -Wextra -Werror`): 5.1e-7 and
5 of 5.

The pack tool's test (`tests/pack/tool_test.c`) puts each corpus texture
into `ygfx_texture()` as stored on the null backend, without and with a
nominal calibration: 9 of 9 made, and the BT.709 texture refused without
the calibration by the calibration message. `trial_images` now copies the
bytes.

`YGFX_SHADER_CONTRACT` is 2 (the user, 2026-10-09; see "v0.10.4"): a body
that samples `ysp_tex0` needs v0.10.4's binding, and a contract-1 player
would run it and read garbage. The wrapper's text did not change, so its
pinned hashes did not; the test now requires 2. The corpus pack's SHADER
entry records the contract, so its SHA-256 changed (docs/pack.md).

Mutants v10-10 to v10-13 (`gfx.toml`: gray refused, other primaries taken
on gray, the level not spread to rgb, RG8's alpha dropped): 4 of 4 killed,
each by the new part (5, 1, 1 and 1 new failing checks).

## v0.10.4: a user shader samples stim.tex

docs/rig_spec.md 15.6 asked whether a USER shader can sample a render
target as `ysp_tex0` (PsychoPy's `VSHD_Distortion.py` and the panorama
demo need it). `examples/gfx/gratings.c` tried it, with a target and with
an uploaded image, on 2026-10-09 (Iris Xe, ANGLE on D3D11).

What v0.10.3 did: the contract named `ysp_tex0`, but the pack step of a
USER draw set no texture, and the backend binds a unit only when a draw
names a texture for it. So `ysp_tex0` read whatever the previous draw left
on unit 0. The new test part (`v0.10 user tex`) shows it on v0.10.3: the
body read the other image of the frame before (0.25 of full scale from the
reference), the target's value was off by 0.075, a draw into the target it
samples was not refused, and a texture id of no gfx was not refused.

The change is in the pack step: `stim.tex` of a USER stimulus (and
`ygfx_user_desc.tex`, appended) goes to unit 0, or to `ysp_utex0` for an
R16UI texture, as an IMAGE's does. Because the texture is now in the draw's
command, the existing feedback check refuses a draw into the target the
body samples (`YGFX_ERR_ORDER`), and runs split where the texture changes.
A texture id that is not this gfx's is refused (`YGFX_ERR_ARG`), and so is
a video texture (its planes need the video program). 14 lines of code.

| Check (`v0.10 user tex`, ANGLE D3D11 hardware) | v0.10.3 | v0.10.4 |
|---|---|---|
| Body `texelFetch(ysp_tex0, ...)` against the same R8 texture as a modulation IMAGE, after a frame that drew another texture | 0.25 | 0 |
| Body against a target cleared to 0.3 in a setup pass (expected 0.5 (1 + 0.5 v), v the texel read back) | 0.075 | 0 |
| Draw into the target it samples | drawn | `YGFX_ERR_ORDER` |
| A texture id above `YGFX_MAX_TEXTURES` | drawn | `YGFX_ERR_ARG` |

v0.10.4 gives 0 in both rows on ANGLE's D3D11 hardware, WARP and
SwiftShader renderers and on Mesa llvmpipe. The target's texel is read
back for the expected value, because the renderers store the clear value
0.3 differently in RGBA16F: D3D11 (hardware and WARP) 0.29980, rounded
down, and SwiftShader 0.30005, the nearest half float. A fixed expected
value failed on SwiftShader by 6.1e-5.

Without `stim.tex`, `ysp_tex0` stays unbound for a USER draw, and the
manual says its value is undefined. Binding a 1 x 1 texture there would
cost a bind per run for a body that has no use for it.

`YGFX_SHADER_CONTRACT` stays 1: the wrapper's text and the names did not
change, and no body could have used `ysp_tex0` before. A pack whose body
samples it on a v0.10.3 player reads garbage; a contract 2 would make such
a player refuse it, and would make v0.10.4 refuse every pack made so far.
Not decided here. Decided on 2026-10-09 by the user: the contract is 2
from v0.10.5 (see "v0.10.5"), because no pack existed outside the
repository.

The graded mask (rig_spec 15.6, `face_jpg.py`): with this change, a body
multiplies a carrier by `ysp_bilinear(ysp_tex0, ...)` of any image, which
is tile 6 of `gfx_gratings`. MASK_TEX stays a distance shape.

Mutants v10-08 (the bind removed) and v10-09 (`ygfx_user()` drops
`desc.tex`): 2 of 2 caught (3 and 2 new failing checks). The part gives
0 on Mesa llvmpipe too (WSL2, gcc 11.4 `-O2 -Werror`). The compile check
draws a user stimulus that samples its target on the null backend:
`YGFX_ERR_ORDER` inside the target's pass, `YGFX_OK` after it.

## v0.10.3: the shader contract version

`YGFX_SHADER_CONTRACT` (1; 2 from v0.10.5) names the SHADER CONTRACT. The pack stores it
with each shader entry (docs/pack.md, SHADER) and the player refuses a
shader built for another version, so a pack never runs a body under a
wrapper that means something else. The number changes when what the
wrapper does to a body changes: the names it gives the body, their
meaning, the blend. A change to the wrapper's text that keeps its meaning
(a comment, a reordered declaration) keeps the number; the player then
logs that the wrap hash differs.

The test pins the FNV-1a 64 of the wrapper's text for one body per mode.
A change to the text fails the test with the new hashes printed, which
forces the decision: raise the contract or not, then update the pins.
Mutant v10-07 (`#line 1` with a space) checks that the pin catches a
one-byte change. The constant is compared through a volatile copy, because
MSVC's `/W4 /WX` refuses a constant condition (C4127).

## v0.10.1: the default cache folder

From v0.10.2 the function is a one-line wrapper of `ysp/rt.h` v0.7.0's
`yrt_user_dir(YRT_DIR_CACHE, "progcache", ...)`, which took over the
rules below unchanged, so a rig profile (`YRT_DIR_CONFIG`) and the program
cache follow one set of rules. The test of this section passes unchanged.

Before v0.10.1 the program cache was used only when a caller passed
`--cache DIR`, so most runs compiled every program (2.3 to 5.3 s on the
Iris Xe, "v0.4: the program cache"). The header still reads and writes no
file unless the caller gives a cache. `ygfx_default_cache_dir()` gives
the folder to use, and the examples use it.

### The folder

| Platform | Folder | Read with |
|---|---|---|
| Windows | `%LOCALAPPDATA%\ysp\progcache` | `GetEnvironmentVariableW`, converted to UTF-8 |
| Linux | `$XDG_CACHE_HOME/ysp/progcache`, else `$HOME/.cache/ysp/progcache` | `getenv` |
| macOS | `$HOME/Library/Caches/ysp/progcache` | `getenv` |

The function makes no folder: the file cache makes it at the first store.
`GetEnvironmentVariableW` and not `SHGetKnownFolderPath`, because the
second needs shell32 and ole32 at link time; the variable is set for each
interactive user. A relative value is refused (the XDG specification says
to ignore one).

### Why there is no fallback

The function fails, and the caller uses no cache, when the variable is
missing, the path does not fit, on the web, and on POSIX when a part of
the path that exists can be written by all users or belongs to another
user (root excepted). It never falls back to `/tmp` or another shared
folder. The entry hash finds damage, not attack: it is not keyed, so
anyone who can write the folder can put an entry there with a correct
hash, and the GL driver parses that binary inside the experiment's
process. The owner check also refuses a folder that another user made
under `/tmp` before the first run.

### The describe line

`ygfx_describe()` now names the folder of a `ygfx_file_cache` and
gives hits and misses:

```
... origin top, program cache C:\Users\me\AppData\Local\psy\progcache: 14 hit / 0 miss (0 rejected)
```

It says `(caller)` for a cache of the caller's own and `off` for none. When
the backend has no program binaries (the null backend of `--sim`, WebGL),
`(unused: no binary format)` follows the folder. A rejected entry is also
a miss. The counts are those of the open; `ygfx_program_stats()` gives
the later builds too.

### The examples

`gfx_bench`, `gfx_gallery`, `gfx_text`, `gfx_layout`, `gfx_hello`,
`gfx_trial`, `gfx_load`, `gfx_rdk` and `gfx_rdk_bench` use the default
folder when `--cache` is not given. `--no-cache` turns the cache off.
Each prints the describe line.

| Run (Windows, Iris Xe, ANGLE D3D11; empty folder first) | First | Second |
|---|---|---|
| `gfx_hello --frames 3` | 0 hit / 14 miss | 14 hit / 0 miss |
| `gfx_trial`, `gfx_load`, `gfx_rdk`, `gfx_gallery`, `gfx_text`, `gfx_rdk_bench` (after the run above) | 14 hit | 14 hit |
| any example with `--sim` | unused | unused |
| `gfx_hello --frames 3 --no-cache` | off, 14 miss | off, 14 miss |

On WSL (Ubuntu, gcc 11.4, Mesa llvmpipe), with `XDG_CACHE_HOME` set to a
private folder, `gfx_bench --device llvmpipe --open-only` opened in 244 ms
(14 compiled and stored) and then in 42 ms (14 loaded). With the real
environment the folder was `/home/adf44/.cache/psy/progcache` (the
cache folder before the rename to ysp; it is now `ysp/progcache`);
`XDG_CACHE_HOME=/tmp` and `HOME=/tmp` gave none.

### Tests and mutations

The CPU half sets the environment and puts it back: a non-ASCII
`LOCALAPPDATA` comes out as UTF-8 without its trailing slash, a UNC path is
kept, a relative or missing value and a short buffer fail with the output
empty; on Linux `XDG_CACHE_HOME` first, a relative one ignored, `/tmp` as
either variable refused, and a missing `HOME` refused. The describe line is
checked for `(caller)` and `off` with the fake backend.

| # | Fault | Result |
|---|---|---|
| v10-05 | a relative `LOCALAPPDATA` accepted | caught |
| v10-06 | the describe line's hits and misses swapped | caught |

Builds: MSVC 19.44 C11 and C++17, MinGW gcc C11 and C++17 (warnings as
errors); `examples/layout/gfx_layout.c` is checked by MinGW gcc only, because this build
leaves `YSP_BUILD_LAYOUT` off. The default test passes on Windows and on
WSL (CPU half and llvmpipe).

## Gallery page 8: v0.7 to v0.10

`gfx_gallery` page 8 (2026-10-08) shows the features of v0.7 to v0.10 in
five tiles. The rounding tile spans two grid cells; the simplex and
additive-layer tiles span two by two (`gallery_page.cells`):

| Tile | What it shows |
|---|---|
| Uniform and Gaussian noise | `UNIFORM` and `GAUSSIAN` at one seed and one SD (0.15), 2 px checks, each with the histogram of the 44 x 44 checks drawn, in half-SD bins, from `ygfx_noise_fill()` (the same values, bit for bit). The bars are drawn once into a target in a setup pass. |
| Simplex fBm | 1, 2, 4 and 8 octaves in 190 px squares on a 24 px lattice, `noise_z` on a track (0.1 cells a second). contrast is 0.15 / SD with this manual's SD per octave count (0.425, 0.317, 0.261, 0.246), so the four have one SD. |
| Turning text | One word by quarter turns that stop for 2.5 s, drawn by default (left) and with `.rays` (right). The label under the left word says which program draws it by the rule in RUNS: EXACT within 0.01 degrees of a quarter turn, else RAYS. The `.rays` run is made first, because a run made before the rays program exists stays on the exact-area program. |
| Rounding | A ramp from code 40 to code 44 rounded three ways: no dither, the Bayer 8 x 8 threshold, and one frame of the noise dither (left). The same codes 8 times as far from code 42, the mean (codes 26 to 58), for viewing (right). |
| Additive layer | A gabor drawn once into an RGBA16F target in a setup pass, added (`YGFX_STIM_ADD`) over a grating, next to the grating alone. |

The dither is one per gfx (`desc.dither`), so the rounding tile cannot
show three live output stages side by side. It computes the three
roundings on the CPU with the output shader's formulas (`floor(d * 255 +
t)`; the Bayer bit interleave; `ygfx_hash2()` with frame 0's key). Each
strip is then an RGBA32F image of the scene values that the output stage,
with no dither, rounds to those codes. The value for a code is the middle
of its interval, by bisection on `ycol_output_code()`. A check of the
screenshot read back from the back buffer (`--shots`) against a separate
model of the three roundings, written in Python, found 0 different pixels
of 8640 in each of the six strips. The tile's label says that it shows codes, not
light. The codes are dark (40 to 44), because there a code step is about
5 % of the light. At mid-gray a step is about 1.7 %. At normal contrast the
real strips still look the same; the x8 copies, labeled "X8 CONTRAST, TO
SEE", show the bands and the two dither patterns.


- Light. A CLUT, a dither, a calibration: all checked as arithmetic, none
  against a photometer.
- 10-bit output, Mono++, Color++, stereo, text layout, GPU filtered noise.
- Fullscreen. gfx changes what is drawn, not how it is presented; the
  swap path's numbers are screen.md's.
- Any GPU but the Iris Xe for timing; any OS but Windows 11 for timing.
- The GPU time of the output stage inside a real frame loop (the GPU timer
  query costs too much per frame; the bench measures it offline).
- v0.2 in a real frame loop: the window runs and the allocation count
  above are v0.1's. v0.2 allocates nothing new after open by design (fixed
  frame-block slots, segments in the handle), but no frame loop with
  target passes was counted.
- Stroke, group and target costs on WARP, SwiftShader and llvmpipe: their
  pixels are tested, their speed is not.
- v0.3 in a real frame loop: no `gfx_load` window run and no allocation
  count with vector stimuli. The vector path allocates nothing after open
  by design (the staging has 16 blocks of slack for extension blocks).
- v0.3 costs on renderers other than the Iris Xe, and on battery.
- ELLIPSE's distance error above an aspect of 9:1 (the test stops there;
  20:1 is allowed).
- Why a warm open from the program cache is slower than a cold one on
  llvmpipe (80 ms against 54 ms at -O2, 101 against 92 ms under ASan; one
  run each).
- `gfx_load` with dots that draw (the v0.1 window run drew none).
- D3D11 import on another GPU or driver; what leaving out the keyed
  mutex does (the test's producer finishes long before ANGLE samples).
- v0.4 in a real frame loop: no window run and no allocation count with
  instances, video or the reorder (none allocates in the frame by design).
- v0.4 costs on renderers other than the Iris Xe, and on battery (the
  battery rows above are named).
- v0.6 text and blur: costs on any renderer but the Iris Xe; pixels on
  Mesa llvmpipe (CI runs it); a window run and an allocation count with
  curve runs and blurs (neither allocates in the frame by design).
- v0.7: the Gaussian table's hash on macOS (clang, Apple's libm) and MSVC
  ARM64; CI's macOS job runs the pinned check. The specialized vector
  programs' GPU time on renderers other than the Iris Xe.
- Curve runs on real fonts against an exact reference: the pixel tests use
  the test builder's glyphs; the probe's font tables are not repeated.

## v0.15.1: costs on the real clock

On ysp/rt.h's virtual clock (rt.h v0.9.0) `yrt_now_ns()` stands still
between waits, so the program cache's open and store times and a text's
build time read 0 in a `--sim` run. They are costs, so they read
`yrt_real_ns()`, the real counter on either clock. `gfx_text --sim`
then prints 5 ms for Segoe UI's 95 glyphs and 171 ms for Microsoft YaHei's
1728 (Windows 11 test laptop, MSVC 19.44 Release, 2026-10-14). Shader time
stays on `yrt_now_ns()`: it is a stimulus time.

## v0.16.0: every stimulus field in a table

The experiment loader checks a stimulus's keys and the designer builds
its panels from the parameter tables ([rig_spec.md](rig_spec.md),
principle 8; gap G6 of [experiment_format.md](experiment_format.md)).
`ygfx_params()` listed the bindable floats only. A definition also sets
`shape`, `edge`, `aperture`, `place` and the other fields that are not
floats, and nothing listed them or their values.

### Decision: the `values` field on `ygfx_param`, and a second table

| Option | For | Against |
|---|---|---|
| A new struct in the shared shape (`name` to `doc`, then `values`) for an enum table | The exact shape of the new tables | No `offset` and no `kinds`. The loader needs `kinds` to know that `aperture` is a key of a grating and not of a gabor; the float table has it and the new one would not |
| Enum rows appended to `ygfx_params()` | One table | `YGFX_P_COUNT`, the `YGFX_P_*` indices and `ygfx_apply()` assume every row is a float. A binding to an enum row would write a float into a `uint16_t` |
| **`values` added as the last field of `ygfx_param`, and a new table `ygfx_enum_params()` of that type** (chosen) | One row type for every gfx table, with `offset` and `kinds`. `values` is last, as in the shared shape, so its first seven fields are the shared shape's. Code that reads the tables compiles unchanged. The float tables keep their indices and their meaning | `ygfx_param` grew 8 bytes (an ABI change, so a minor version). A positional initializer of a `ygfx_param` outside the header gets `values` NULL, and g++ `-Wextra` warns about it; none exists in the repository |

`ygfx_desc_params()` (the gfx's own desc: `units`, `dither`, `output`,
`stereo`, `cones`) returns `yscr_param`, ysp/screen.h's struct, which had
no `values` in this version, so its enum values stayed in the doc text.
v0.17.0 gives them `values` (below).

### The rows

21 rows, indexed by `YGFX_E_*`:

| Rows | Type | Field |
|---|---|---|
| `place`, `anchor` | enum, `center=0` to `bottom_right=8` | `place`; `anchor` sets `ax` and `ay` |
| `shape` | enum, all 18 `ygfx_shape_kind` values | `shape`, SHAPE |
| `aperture` | enum, `rect=0` to `mask_tex=7` (the apertures of v0.1) | `shape`, GRATING, NOISE, USER |
| `aperture` | enum, `rect=0,circle=1,no_aperture=6`, default `no_aperture` | `shape`, DOTS |
| `edge`, `stroke_align` | enum | `edge`, `stroke_align` |
| `join`, `cap`, `blend` | enum | SHAPE; `blend` also TEXT |
| `square`, `dist` | bool, enum | both `wave`: GRATING, NOISE |
| `octaves` | i32, 1 to 8 | `gn_octaves` |
| `dash_snap` | bool | `dash_snap` |
| `modulation`, `linear`, `add`, `premultiplied`, `coverage`, `aliased`, `rays` | bool | a `YGFX_STIM_*` flag: IMAGE, and TEXT for the last two |

Names are the desc fields, so `aperture` and `square` are the keys a
definition writes, not the stimulus fields (`shape`, `wave`) that hold
them. `aperture` has two rows because a dot field takes three apertures
and has another default. No kind reads two rows of one name; a loader
looks a key up with the stimulus's kind.

`ygfx_enum_get()` and `ygfx_enum_set()` read and write a row on a
stimulus. The loader then needs no knowledge of the field widths, the
flag bits or the nine anchor points: it finds the row by name and kind,
the value by name in `values`, and calls `ygfx_enum_set()`, which refuses
a kind that the row does not take and a value that it does not list. The
`offset` of a flag row is that of `flags` and of `anchor` that of `ax`;
the doc names the flag.

Not in a table: the desc pointers (`fx`, `paint`, `prims`, `path`,
`palette`, `items`, `vertices`) and the enums of the structs they point to
(`ygfx_paint.kind` and `.space`, `ygfx_prim.shape` and `.op`). The
definition format (version 1) has no key for them.

### Tests

`test_enum_params()` in the CPU half of `tests/adapt/gfx_test.c`:

- Each row's `offset` and `kinds` equal the test's own list, taken from
  the descs that have the field.
- For each kind that reads a row, the stimulus from a zero desc
  (`ygfx_shape()` to `ygfx_user()`, `ygfx_crun()` without a gfx) holds the
  row's default, read by `ygfx_enum_get()` and by the test from the field.
- Every value from min - 1 to max + 1: a listed value lands in the field
  and is read back, and setting the default again gives the zero desc's
  stimulus byte for byte; any other value, and every kind the row does not
  take, is refused and changes nothing.
- An enum row lists every value of its C enum (the aperture rows: their
  stated sets), in order, with the enum's own names, and its `max` is the
  last value. The names come from a `switch` without a `default` in the
  test, so gcc and clang (`-Wall -Werror`, as CI builds) stop when an enum
  gains a value, and the check fails until the row lists it.
- The float tables: `values` NULL and type `f32` in every row.

Checked on 2026-10-21: 21 rows, 46 row and kind pairs, with MinGW-w64
gcc 16.1 (`-Werror`), MSVC 19.44 (`msvc-full`, ctest 89 of 89) and gcc
11.4 on WSL2; the CI `--sim` list passes. Mutants `en-01` to `en-08`
(CPU half only, `YGFX_TEST_PARTS=none`) caught, 8 of 8.

## v0.17.0: values in the desc table

Every ysp parameter table has one row shape: `name`, `type`, `min`, `max`, `def`, `unit`, `doc`, `values` ([conventions.md](conventions.md), "Parameter tables"; gap G6 of [experiment_format.md](experiment_format.md)). ysp/screen.h v0.9.0 gave `yscr_param` its `values`, so
`ygfx_desc_params()` fills it:

| Row | values |
|---|---|
| `units` | `px=0,deg=1` |
| `dither` | `none=0,ordered=1,noise=2` |
| `output` | `out_8=0,out_10=1,out_mono_pp=2,out_color_pp=3` |
| `stereo` | `mono=0,side_by_side=1,row_interleaved=2,frame_sequential=3` |
| `cones` | `ss2=0,ss10=1` (ysp/color.h's `ycol_cones`) |

`output` and `stereo` listed 0 only, with `max` 0, because `open()`
takes `OUT_8` and `MONO` only. Now they list every value of the C enum
and `max` is the last one; the doc says what opens. The list follows the
C enum, as `backend`'s in ysp/screen.h does.
`output`'s names keep `out_`: the enum's suffix after `OUT_` is `8` or
`10`, and a name never starts with a digit, so that a loader can tell a
name from a number.

`test_desc_params()` in the CPU half of `tests/adapt/gfx_test.c` checks
each row against its `ygfx_desc` field and the names against `switch`es
without a `default`, and opens a gfx on a SIM screen with the null
backend and a zero desc: `max_draws` is 1024 there, the other defaults
are 0. 2 of 2 new mutants killed (`tests/mutate/gfx.toml`, pv-).

## v0.17.1: font paths that do not fit

gcc refused `ygfx_text_make()` under `-Werror` at -O1, -Os and -Og:

    include/ysp/gfx.h:12373:54: error: '%s' directive output may be
    truncated writing up to 511 bytes into a region of size between 398
    and 400 [-Werror=format-truncation=]
        snprintf(tried + u, sizeof tried - u, "%s%s", u ? ", " : "", path);

| Compiler | -O0 | -O1 | -O2 | -O3 | -Os | -Og |
|---|---|---|---|---|---|---|
| gcc 11.4 (WSL2, Ubuntu 22.04), C99, C11, C++17 | clean | warns | clean | clean | warns | warns |
| MinGW-w64 gcc 16.1, C11 | | warns | clean | clean | warns | warns |
| MinGW-w64 gcc 16.1, C11, `-D_FORTIFY_SOURCE=2` | | clean | clean | clean | clean | clean |
| clang 11 (WSL2), C99, C11, C++17 | clean | clean | clean | clean | clean | |

The warning is right: the error message's list of the paths tried
(`tried`, 400 bytes) cut a path of 398 bytes or more without a mark.
Next to it was a real defect that no compiler named. The path itself
was cut to 511 bytes by `snprintf()` and then opened, so a path of 512
bytes or more opened another file, its first 511 bytes.

The fix:

- A path that does not fit its 511 bytes is not opened (YGFX_ERR_IO,
  "no font file opened").
- `ygfx__tried_add()` appends to the list by length and `memcpy()`. A
  path that does not fit is cut and ends in "...". gcc sees the lengths,
  so it has nothing to warn about. No pragma.

A path that fits gives the same bytes as before, error messages
included.

`test_v17_text_paths_cpu()` in the CPU half of `tests/adapt/gfx_test.c`
runs on a SIM screen with the null backend. A missing font of 450 bytes
gives YGFX_ERR_IO, and the message has its start and "...". It makes a
file (not a font) whose path is exactly 511 bytes, in folders of 200
bytes under the temporary folder. On Windows the path is extended-length
(`\\?\`), past MAX_PATH. That path gives YGFX_ERR_FORMAT, so it opens.
The same path with 1 more byte gives YGFX_ERR_IO, where v0.17.0 opened
the 511-byte file and gave YGFX_ERR_FORMAT. Then the test removes the file
and the folders. 2 of 2 new mutants killed on MSVC (`tests/mutate/gfx.toml`,
tp-). CI's new `gcc-O1` entry (`linux-extra`) builds the tree at -O1, where
the warning showed.
