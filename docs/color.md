# ysp/color.h design notes

Status: v0.4.0, 2026-10-21 (`ycol_michelson()`, section "Michelson
contrast in light"); v0.1.0, 2026-10-06. ysp/gfx.h v0.5.0 takes its calibration,
PAINT's spaces and VIDEO's primaries from this header (stages A to C
below, each checked bit for bit). The Python and MATLAB/Octave bindings
exist, and a MATLAB run compares the header with Psychtoolbox. The design
was reviewed and approved by the coordinator on 2026-10-06; section
"Decisions" records the answers.

## Why a color header

The user asked for color utilities, "easy x_to_rgba functions, but maybe
that's insufficient". A plain `x_to_rgba()` is the wrong shape for three
reasons:

1. A stimulus color is linear device RGB on a measured display. Every
   conversion must go through a calibration: measured, or nominal and
   flagged. An assumed sRGB display is a guess, not a stimulus.
2. A color outside the display's gamut must come back unclipped, with its
   distance. A silent clip changes the stimulus without a record.
3. Psychophysics asks gamut questions as often as conversion questions:
   the largest contrast along a direction about a background, the
   boundary of the gamut in an isoluminant plane.

This reverses rig_spec 4.8 ("a standalone color header: not planned") and
the 4.3 sentence "no general color library". The boundary stays narrow:
no ICC profiles, no color appearance models (CIECAM02, CAM16), no color
difference formulas, no reflectances or spectral rendering, no named
colors. rig_spec changes when the extraction lands (stage D).

## Why a context, not a calibration argument

A calibration alone cannot answer most questions:

| Question | Needs beyond the calibration |
|---|---|
| DKL, cone contrast, MacLeod-Boynton | a background, a cone observer (2 or 10 degree), a luminance definition (standard or the participant's) |
| CIELAB, CIELUV, Oklab | a reference white |
| sRGB, hex, Display P3, Rec. 2020 | how a D65 source meets this display: luminance scale and chromatic adaptation |

`ycol_ctx` holds these once, with the derived matrices. It is the unit
to log: `ycol_ctx_describe()` gives one line, and `ctx.id` is a CRC of
every input. It is read-only after init, so threads can share one. The
calibration stays a separate object, because it has its own lifetime (one
per display and date) and its own file.

The call reads like a JavaScript color library:

| Task | Color.js, culori | ysp/color.h |
|---|---|---|
| a color in a space | `new Color("oklch", [0.75, 0.2, 30])` | `YCOL_OKLCH(.L = 0.75, .C = 0.2, .h = 30)` |
| to device | `c.to("srgb")` | `ycol_to_rgb(&cx, c, &g)` |
| in gamut | `c.inGamut("srgb")`, `displayable(c)` | `g.in`, `g.distance`, `g.margin[k]` |
| gamut map | `c.toGamut({ method: "oklch.c" })`, `clampChroma(c, "oklch")` | `ycol_map(&cx, c, YCOL_MAP_CHROMA_OKLCH, &g)` |
| hex | `new Color("#3366cc")` | `ycol_hex("#3366cc")` |
| largest contrast | none | `ycol_max_scale(&cx, YCOL_DKL(.azim = 90, .contrast = 1), YCOL_SYMMETRIC)` |

The value macros are designated initializers in a compound literal (C99)
or a functional cast (C++20), as ysp/timeline.h's op macros are. C++17
uses `ycol_make(space, a, b, c)`. A macro needs at least one field,
because empty braces are not C99.

## Decisions

Answers from the review of 2026-10-06. The user can still override them.

| # | Question | Decision |
|---|---|---|
| 1 | A standalone color header | Yes. Keep the boundary above. Update rig_spec 4.8 and 4.3 when the header lands in ysp/gfx.h. |
| 2 | Compatibility names in ysp/gfx.h | Aliases only between stages A and B, never in a release. |
| 3 | The black in CIELAB, CIELUV, Oklab | Absolute XYZ, black included. ysp/gfx.h's OKLAB paint gets the black term in stage C. |
| 4 | D65 sources by default | Relative luminance, no adaptation (ysp/gfx.h's video choice), printed by `ycol_ctx_describe()` so every session logs it. |
| 5 | Cone spaces without spectra | Refused. Datasheet spectra are allowed with the flag `YCOL_CAL_SPECTRA_NOMINAL`, set by `ycol_cal_set_spectra_nominal()` and shown in every describe line. |
| 6 | Participant luminance | As designed. Oklab never takes it. |
| 7, 8 | CIE 2006 age model; Smith-Pokorny and Judd-Vos | Out of v0.1, listed under LEFT OUT in the manual with the reason. |
| 9 | Display P3 and Rec. 2020 | Source spaces like sRGB, through the same code path (primaries, white, transfer). |
| 10 | Space names in strings | "cielab", "cielch", "cieluv", "cielchuv", to keep them apart from CSS `lab()`, which has a D50 white. |
| 11 | Python color type | A namedtuple. |
| 12 | Color tweens | `examples/color/bench.c` is written and built; it runs under the measurement lock later. |

## The model

### Three levels in the calibration

| Content | Flag | Gives |
|---|---|---|
| readings: Y per gun and level, a black | always | the CLUT; RGB and DEVICE |
| plus x, y at level 1 for each gun, and for a black with light | `YCOL_CAL_HAS_XY` | XYZ (CIE 1931 2 degree) and every space made from it |
| plus the primaries' spectra | `YCOL_CAL_HAS_SPECTRA` | LMS with SS2 or SS10, and every cone space |

A conversion that needs a missing level is refused with
`YCOL_ERR_REFUSED` and a message that names what is missing.

### Conventions

- Linear device RGB is ysp/gfx.h's scene value: per gun 0..1, in
  proportion to that gun's light above black.
- XYZ, xyY and LMS are absolute: the light at the eye, black included.
  This is how `ygfx_cal_lms()` already treated LMS.
- CIELAB, CIELUV and Oklab take absolute XYZ against the reference white:
  `desc.white`, or the XYZ of device (1, 1, 1), black included.
- Oklab divides XYZ by the white's Y and applies Ottosson's M1 with no
  adaptation, unless `desc.adapt` is BRADFORD. The inverse of M2 is
  computed exactly; Ottosson's printed 10-digit inverse, which ysp/gfx.h's
  shader uses, leaves 2.4e-7 in linear RGB (measured below).
- D65 sources (sRGB, Display P3, Rec. 2020) decode by their transfer
  function with the sign kept, go to XYZ by the matrix made from their
  primaries and D65, then to this display. Relative (the default,
  `src_white_Y` 0): the source's white has the display's white luminance
  and its black is the display's black. Absolute (`src_white_Y` > 0): the
  source's white has that luminance and its black is XYZ 0, below the
  display's black. Rec. 2020 uses the exact alpha and beta of BT.2020's
  OETF (1.09929682680944, 0.018053968510807), as CSS Color 4 does.
  colour-science rounds them to the 10-bit values by default.
- Hue and azimuth are in degrees, 0 to 360. Below a chroma of 1e-12, a
  DKL contrast of 0, or an elevation of +-90 degrees, the angle is 0 and
  `YCOL_F_ACHROMATIC` is set.

### Spaces and refusals

| Space | Values | Needs | Refused when | Reference in the test |
|---|---|---|---|---|
| RGB | linear device RGB | calibration | never | identity |
| DEVICE | drive values 0..1 through the inverse of the stored CLUT | calibration | a value outside 0..1 | `ycol_output_code()` gives the code back for all 256 codes |
| XYZ | cd/m2, absolute | XY | no xy | colour-science |
| XYY | x, y, Y | XY | y = 0 with Y > 0 | colour-science |
| CIELAB, CIELCH | CIE 1976 about the white | XY | as XYZ | colour-science |
| CIELUV, CIELCHUV | CIE 1976 about the white | XY | v' = 0 | colour-science |
| OKLAB, OKLCH | Ottosson 2020 | XY | as XYZ | colour-science |
| SRGB, DISPLAY_P3, REC2020 | encoded values | XY | as XYZ | colour-science, derived matrices |
| LMS | cone excitations, absolute | SPECTRA | no spectra | Psychtoolbox `T_cones_ss2 * B` |
| CONE | (LMS - LMS_bg) / LMS_bg | SPECTRA, BG | a background excitation <= 0 | algebra; the raw `dir_cone` bit for bit |
| DKL, DKL_CART | ComputeDKL_M about the background, luminance `w` | SPECTRA, BG | as CONE | `ComputeDKL_M.m`; the raw `dir_dkl` bit for bit |
| MB | l = wL L / V, s = k S / V, lum = V | SPECTRA | V <= 0 | the spectrum locus's s at most 1 |

`ycol_convert()` goes through linear device RGB, which is linear and
invertible. Between the XYZ family and the cone family it sets
`YCOL_F_VIA_DEVICE`: XYZ does not fix LMS; a display does.

### Gamut

- The gamut is the device cube.
- `in`: every channel in [-eps, 1 + eps], eps = `YCOL_GAMUT_EPS` =
  2^-20 = 9.54e-7, below 1/1000 of a 10-bit code (9.78e-7). Margins and
  the distance are exact, with no tolerance. The tolerance is the
  precision of the stored data, not double round-off: the calibration
  stores its readings as float, each to a relative u = 2^-24. Stage 0 had
  eps = 1e-9, and an sRGB primary on a nominal sRGB calibration came out
  "OUT by 8.3e-08": a false alarm on day one.
- The bound behind eps, to first order: the error that float storage puts
  in a device value is at most the sum over every stored float f of
  |d rgb / d f| f u. For a nominal display and a source with the same
  primaries, at the 8 corners of the source's cube, the bound is 7.2 u
  (sRGB), 5.8 u (Display P3) and 4.7 u (BT.2020), from the 11 stored
  numbers (8 chromaticities, 3 luminances; computed by finite differences
  in double). The test measures at most 0.9 u at those corners, and 1.4 u
  for #ff8800. eps = 16 u, over twice the largest bound. A measured
  calibration has more readings and the same form of bound; eps is fixed.
- The bisections (`ycol_max_ring()`, `ycol_map()`) stop inside the cube
  to double round-off (1e-12), not merely within eps.
- `margin[k] = min(v, 1 - v)`; `distance = max(0, -min margin)`;
  `scale`: the largest k with bg + k (rgb - bg) inside.
- `ycol_max_scale()` is closed form. `ycol_max_ring()` is closed form
  for DKL and cone planes, and bisects the chroma (60 steps) for OkLCh
  and CIE LCh. The bisection assumes that the chroma in gamut at one
  lightness and hue is one interval from 0.
- `ycol_map()` methods: SCALE, CHROMA_OKLCH, CHROMA_CIELCH, CHROMA_DKL,
  CLIP. None is a default. The CSS Color 4 algorithm is not offered: its
  last step is a clip within a JND.

### Observers

- XYZ is CIE 1931 2 degree, from the readings' chromaticities.
- Cones: SS2 (Stockman and Sharpe 2000, the CIE 170-1:2006 2 degree
  fundamentals, CVRL linss2_10e_1) or SS10 (CVRL linss10e_1, new). The
  SS10 table came from colour-science 0.4.7; the same script made the SS2
  table bit for bit equal to ysp/gfx.h's, CVRL's blank S above 615 nm as 0.
- Standard luminance: 0.68990272 L + 0.34832189 M (2 degree), 0.69283932 L
  + 0.34967567 M (10 degree). A least-squares fit of colour-science's CIE
  2008 physiological LEFs on the tables gives 0.68990262, 0.34832198 and
  0.69283932, 0.34967561, residual 8.3e-7 and 8.0e-7 of peak.
- MacLeod-Boynton's s scale puts the spectrum locus's largest S / V at 1,
  at 418 nm in both tables: 0.0371598 (SS2) and 0.0554793 (SS10).

### No cone space from XYZ

The rule stays: without spectra, the cone spaces refuse. The alternative
was a fixed 3 x 3 from CIE 1931 XYZ to SS2 LMS. The least-squares 3 x 3
over 390 to 780 nm leaves residuals of 0.027 (L), 0.030 (M) and 0.20 (S)
of peak. A DKL L-M axis built through it at mid-gray, then evaluated with
the true spectra (colour-science's display spectra):

| Display | luminance contrast per unit L contrast | S contrast per unit L contrast |
|---|---|---|
| Typical CRT, Brainard 1997 | +0.020 | -0.073 |
| Apple Studio Display | +0.044 | -0.160 |

An S leak of 7 to 16 percent of the L-M modulation is not a
cone-isolating stimulus.

### Participant luminance

At the null of heterochromatic flicker photometry, minimum motion or a
minimally distinct border, two lights a and b are equally luminous for the
participant: w . (LMS(a) - LMS(b)) = 0.

- **Stored** in `ycol_lum` (`.ysplum`, 1688 bytes, fixed offsets,
  CRC-32): the raw settings (both lights in linear device RGB, the
  calibration's CRC, the surround, repeats, SD, field size, eccentricity,
  frequency) and the derived weights. The weights are in cone units, so
  they do not depend on the display.
- **Fitted** by `ycol_lum_derive()`: the smallest eigenvector of the
  scatter of the normalized differences (2 x 2, or 3 x 3 with
  `YCOL_LUM_FIT_S`), sign so that wL + wM > 0, scaled so that an
  equal-energy spectrum keeps the standard luminance. It refuses another
  calibration than the settings', no spectra, a = b, too few independent
  settings, and a negative weight. `resid[]` gives the luminance contrast
  left per setting. `ycol_lum_stated()` takes published weights and
  sets STATED. `ycol_lum_check()` fits again from the settings.
- **Applied** through `desc.lum`: the DKL luminance row (so the
  isoluminant plane and its axes are the participant's) and
  MacLeod-Boynton's l and luminance. Not cone contrast, XYZ, CIELAB,
  CIELUV, Oklab or the D65 sources: those are CIE colorimetry. The context
  refuses a record in other cone units.
- **Logged** by `ycol_lum_describe()`, and by the context's describe
  line and id. Stage C adds a ysp/gfx.h ring record at open with the
  context id, the calibration CRC, the lum CRC and the cones.
- **Measured** with `ycol_lum_pair()`, which gives the two lights of a
  probe at a trial elevation; a method of adjustment or a ysp_stair track
  finds the null.

For DKL with a weight on S, the S row of ComputeDKL_M becomes
(-wL, -wM, V_bg / S_bg - wS). With wS = 0 every operation gives the same
bits as the raw `ycol_cal_dkl_matrix()`, which the test checks.

## Open, close and the context's message (v0.2.0)

v0.2.0 applies the repository conventions (README.md, Conventions) to this
header. Three decisions:

- **The context opens.** `ycol_ctx_open(cx, d)` replaces
  `ycol_ctx_init(cx, d, err, cap)`: it takes a desc and checks it, which
  is what open means in every header. It returns `bool`. The message is in
  the context (`ycol_ctx_error()`), and the code in `cx->status`, because
  the Python and MATLAB bindings map the code to an exception class
  (`FormatError`, `RefusedError`) and their tests check the class.
  `ycol_ctx_close()` zeroes the context; there is nothing to release, but
  every open has a close. A closed context refuses every conversion.
- **Readers keep their reports.** The calls that take a `const ycol_ctx*`
  (the conversions, the gamut calls, `ycol_ctx_describe()`) do not write
  the message: several threads can share one context, and a write would
  race. A conversion reports in its `ycol_gamut` (`status`, `why`) or its
  status argument, as in v0.1. `ycol_ctx_set_background()` writes the
  context, so it also writes the message when it fails.
- **Records stay records.** `ycol_cal` and `ycol_lum` are plain structs
  with a file form, so they keep init, load, save, derive and check with a
  caller buffer for the message.

The context grew by 260 bytes (the code and a 256-byte message). The
matrices keep their offsets. USAGE now builds as C99 and C++17: it uses
`ycol_make()`, and the value macros are shown as the C99 and C++20 form.

Checked on 2026-10-10 (Windows 11, MSVC 19.44 /W4 /WX, and MinGW-w64 gcc
-std=c99 and -std=c++17 -pedantic for USAGE): `tests/adapt/color_test.c`
passes with every v0.1 check unchanged, plus the close (zeroed, after a
failed open, twice), the message and code of a failed open and of a
refused background. `tests/compile/color.c` names the context's calls by
type. Mutants 33 (a refused background without a message) and 34 (a close
that keeps the matrices) are caught.

## Float and double

The math is double. The calibration stores float readings, spectra and
CLUT. ysp/gfx.h's GPU paint path keeps its measured tolerances against
the double reference (docs/gfx.md, v0.3 pixel tests): paint RGB 2e-6,
OKLAB 2e-5 (SwiftShader 1e-4), DKL_POLAR 1e-4 (SwiftShader 1e-3).
`ycol_output_code()` repeats the output stage's float arithmetic, so
the CPU code and the GPU code agree with dither NONE for a value the
scene holds.

## Extraction from ysp/gfx.h

### What moves

| ysp/gfx.h | ysp/color.h |
|---|---|
| `ygfx_cal`, `ygfx_cal_reading`, `YGFX_CAL_*`, `YGFX_GUN_*` | `ycol_cal` and the rest: the same layout and offset asserts |
| cal_init, add, set_spectra, derive, check, save, load, nominal | `ycol_cal_*`: the code moved verbatim, renamed |
| cal_lms, cal_dir_cone, cal_dkl_matrix, cal_dir_dkl, dkl_from_sph, max_contrast | the raw layer, verbatim |
| cone_fundamentals and its table | `ycol_cone_fundamentals(cones, ...)` |
| inv3, mul3, hermite, CRC-32 | private copies (gfx keeps its CRC for the program cache) |
| Oklab and DKL spherical CPU math, `ygfx__paint_spaces` | the context |
| the video primaries and their matrix | `ycol_prim_to_rgb()` |

### What stays

The CLUT and transfer textures and their upload, the output stage,
dithering, every GLSL string, the per-draw float gamut check and
`ygfx_clipped()` (a hot path), Y'CbCr matrices and ranges (encoding,
not light), `ygfx_calibrated()`.

### Stages

Each stage was a copy of ysp/gfx.h outside the tree (the copies were not
kept); the tree got only the last one. The baseline is v0.4.0 as the gfx worker left it. For every
stage, ysp_gfx's full test ran on the Iris Xe (ANGLE D3D11), WARP and
SwiftShader with a harness that folds every `ygfx_read_scene`,
`_read_output` and `_read_target` result into an FNV-1a hash, and the
compile checks, the video test and the color test were built with MSVC
19.44 (`/W4 /WX`) and MinGW gcc 14.3 (C99, C11, C++17).

| Stage | What changed | Result |
|---|---|---|
| 0 | New files: ysp/color.h, its test, compile checks, examples, the reference script; `test_color_gfx` compared v0.4's calibration code with ysp/color.h's | 0 differences (Verification) |
| A | ysp/gfx.h includes ysp/color.h and deletes its calibration code and cone table; aliases (`typedef ycol_cal ygfx_cal`, `#define ygfx_cal_derive ycol_cal_derive`, ...) keep callers unchanged | readback hashes equal to the baseline's on all three renderers; every printed number the same |
| B | callers renamed (the gfx test, gfx_calib, gfx_bench, gfx_gallery's five calls); aliases deleted; the CALIBRATION section of the manual points here | hashes equal again; the printed lines differ only in a renamed function's name |
| C | open() builds a `ycol_ctx`; PAINT's matrices and VIDEO's primaries come from it; `desc.cones`, `desc.lum`, `ygfx_color()`, `YGFX_EV_COLOR`; the OKLAB black term in the CPU form and the shader; v0.5.0 | see below |

Stage C's checks:
- Every part of v0.4's test (all but the new `v0.5 color` part, through
  `YGFX_TEST_PARTS`) gives the same readback hashes as stage B on all
  three renderers: the black term changes no pixel on a calibration whose
  black has no light, which is every calibration in v0.4's test.
- The new part draws an OKLAB gradient on a calibration whose black is
  0.5 cd/m2 (chromaticity 0.31, 0.33) under a white of about 80 cd/m2, and
  compares each pixel with a double reference written in the test from
  Ottosson's published matrices and its own 3 x 3 solve (nothing shared
  with either header). Measured: 6.98e-7 (Iris Xe), 7.28e-7 (WARP),
  7.28e-7 (SwiftShader); tolerance 2e-5 (SwiftShader 1e-4), as v0.3's
  OKLAB row. v0.4's black-free form would have moved that gradient by
  5.20e-3 of full scale: the size of the change, on that display.
- `ygfx_color()` equals a context built apart from the same desc (id
  and matrices); `desc.cones` without a calibration and a lum record in
  other cone units are refused at open; without a calibration
  `ygfx_color()` is NULL.

What ysp/video.h and its worker must change: nothing. ysp/video.h,
examples/video/*.c and tests/adapt/video_test.c use none of the
renamed names (only `ygfx_calibrated()`, `YGFX_PRIM_*` and
`YGFX_TRC_*`, which stay); ysp_video's test and compile checks pass
against stage C on MSVC and MinGW. A user who copies ysp/video.h and
ysp/gfx.h now also copies ysp/color.h.

Stage D (shared files, 2026-10-06): the README row for ysp/color.h, ysp/gfx.h's
row and Needs column (ysp/color.h), the Dependencies, Names, Layout and
Bindings paragraphs; rig_spec 4.3 (Calibration), 4.8 (the "standalone
color header" item removed, with a note), 5.2 (the Calibration row) and
the 6.1 open question "Color" (partly answered, with the measured cost);
the MEX README's table row.

## Verification

`tests/adapt/color_test.c`. Tolerances are about 3 times the largest
error measured, or the stated bound.

### Published values (moved from ysp/gfx.h's test)

| Check | Reference | Measured | Tolerance |
|---|---|---|---|
| sRGB primaries and D65 to XYZ | IEC 61966-2-1:1999, printed | 3.9e-5 | 1.2e-4 |
| CLUT of an sRGB transfer, 17 and 64 levels | sRGB's encoding | 1.3e-3, 3.6e-5 | 4e-3, 1.1e-4 |
| RGB to LMS of B_monitor | Psychtoolbox, MATLAB R2023a | 1.3e-8 relative | 1e-6 |
| DKL matrix at (0.4, 0.5, 0.3) | ComputeDKL_M | 3.1e-8 relative | 1e-6 |
| DKL unit axes as RGB | the same | 6.8e-8 relative | 1e-5 |
| 10 degree rows at 440 and 570 nm | CVRL through colour-science | equal to float rounding | 1e-6 relative |
| MacLeod-Boynton scale | colour-science's tables | 0.0371598, 0.0554793 | 1e-6 relative |

### Against colour-science 0.4.7

`tests/compare/color_refs.py` (uv). Twelve sRGB colors (black, white, the
primaries, near-gray, saturated), four Display P3 and Rec. 2020 values,
a Bradford adaptation and two dark grays on each side of CIELAB's
epsilon. The context has a D65 white at Y = 1 and absolute D65 sources
at Y = 1, so each result depends on the calibration only through
rounding.

| Space | Largest difference | Tolerance |
|---|---|---|
| XYZ from sRGB | 2.2e-16 | 1e-12 |
| xyY | 1.1e-16 | 1e-12 |
| CIELAB | 5.7e-14 | 1e-10 |
| CIELCh | 6.8e-13 | 1e-9 |
| CIELUV | 8.5e-14 | 1e-10 |
| CIELChuv | 1.4e-12 | 1e-9 |
| Oklab | 4.0e-16 | 1e-12 |
| OkLCh (the hue of near-grays) | 3.3e-10 | 1e-9 |
| Display P3, Rec. 2020 to XYZ | 2.2e-16 | 1e-12 |
| back to XYZ or the encoded values | 1.8e-15 | 1e-10 |
| Bradford | 1.1e-16 | 1e-12 |

### Round trips

20000 random device RGB values on each of three contexts (SS2 with the
standard luminance; SS10 with a stated luminance with an S weight,
Bradford and absolute sources; an explicit white with relative sources),
through each space and back. Largest difference in linear RGB:

| Space | Measured |
|---|---|
| RGB, DEVICE | 0 |
| XYZ, sRGB, Display P3, Rec. 2020, LMS, CONE, DKL_CART | 6.7e-16 to 2.8e-15 |
| xyY, CIELAB, CIELCh, CIELUV, CIELChuv | 1.4e-15 to 2.6e-15 |
| Oklab, OkLCh | 6.1e-15 (2.6e-7 with Ottosson's printed inverse of M2) |
| MacLeod-Boynton | 8.4e-15 |
| DKL spherical | 1.4e-13 |

Tolerance: 1e-12.

### Gamut, mapping, device, luminance

| Check | Measured | Tolerance |
|---|---|---|
| `ycol_max_scale()` against a 200-step bisection on the gamut, 400 DKL directions | 1.1e-15 relative | 1e-12 |
| the OkLCh ring: in gamut at the radius, outside the cube at 1 + 1e-6 times it | all 72 hues | all |
| sRGB, Display P3 and Rec. 2020 cube corners on nominal displays with those primaries: in gamut | 24 of 24; largest distance 5.2e-8 (0.9 u) | 24 of 24 |
| a channel 9.5e-7 outside: in; 9.6e-7 outside: out (eps 2^-20) | as stated | as stated |
| map, 2000 colors (1164 outside): result in gamut | all | all |
| CHROMA_OKLCH keeps L; keeps h | 3.3e-16; 2.3e-12 deg | 1e-12; 1e-8 deg |
| CHROMA_DKL keeps the luminance part; directions kept (SCALE, CHROMA_DKL) | 6.7e-16; 5.1e-16 | 1e-12 |
| DEVICE(k / 255) to RGB to `ycol_output_code()`, 256 codes, 3 guns, 5 calibrations | 0 of 3840 differ | 0 |
| a device value 0.502 of a code above k writes k + 1, 0.498 writes k | 0 of 510 differ | 0 |
| luminance fit, one setting: direction of w against the observer's | 9.4e-16 | 1e-9 |
| luminance fit, FIT_S, three settings | 6.7e-16 | 1e-9 |
| the L-M axis with the fitted w: luminance left | under 1e-12 relative | 1e-12 |
| the context's cone and DKL directions against the raw calls | bit for bit | bit for bit |

### Against ysp/gfx.h v0.4, bit for bit (stage 0's `test_color_gfx`)

| Check | Measured |
|---|---|
| calibrations: nominal, with spectra, 50 random reading sets (levels, chromaticities, a black with light, spectra, a short CLUT, refusals): struct, saved bytes, check() and its message | 0 of 52 differ |
| raw calls: lms, dkl_matrix, dir_cone, dir_dkl, max_contrast, dkl_from_sph, cone_fundamentals, 300 random inputs each | 0 of 2100 differ |
| PAINT's matrices (`ygfx__paint_spaces`) against the context, two calibrations | 0 differ |
| PAINT's OKLAB and DKL_POLAR forward conversions against the context, 400 colors | 0 differ |
| PAINT's OKLAB back to RGB (ysp/gfx.h's printed inverse of M2) | within 2.4e-7 |
| the video path's primaries (`ygfx__video_consts`) against `ycol_prim_to_rgb()` as float, four primaries | 0 of 36 differ |

The build read ysp/gfx.h's static functions. It ran against v0.4.0 as the
gfx worker left it (0 differences again) and was removed with stage C,
when ysp/gfx.h stopped having its own calibration code.

### A pinned calibration

A calibration derived from float literals (no libm in the inputs) has
FNV-1a 86f31b63 on MSVC 19.44 (56f8efa3 before the magic bytes changed from
PSYCAL to YSPCAL; MinGW gcc 14.3 and gcc 11.4 agreed on the old value).
The test pins it on x64 and WebAssembly, where the arithmetic is plain
IEEE double without contraction.

### Mutations

[tests/mutate/color.toml](../tests/mutate/color.toml), run with
`uv run tests/mutate/mutate.py color.toml` ([tests/mutate](../tests/mutate/README.md)):
each fault in a copy of the header, the test built with MinGW gcc against
it; a fault must fail the test. A control
with no change passes. Run again after the gamut tolerance became 2^-20:
32 of 32 again.

| # | Fault | Result |
|---|---|---|
| 1 | CIELAB epsilon 216/24000 | caught (after the test got grays on each side of it) |
| 2 | CIELUV u* against v'n | caught |
| 3 | CIELUV u' from Y | caught |
| 4 | an Oklab M1 row | caught |
| 5 | Oklab without the white scale | caught |
| 6 | Bradford ratio inverted | caught |
| 7 | cbrt as pow (negative cone responses) | caught (after the test got a cone response below 0) |
| 8 | hue in radians | caught |
| 9 | DKL azimuth sign | caught |
| 10 | DKL elevation sin as cos | caught |
| 11 | MacLeod-Boynton without its s scale | caught |
| 12 | the 10 degree table ignored | caught |
| 13 | the luminance fit not normalized | caught |
| 14 | the luminance fit's sign | caught |
| 15 | max_scale SYMMETRIC as one-sided | caught |
| 16 | margin sign | caught |
| 17 | below reported as above | caught |
| 18 | chroma bisection at 4 steps | caught |
| 19 | hex nibble order | caught |
| 20 | sRGB threshold 0.03928 | caught |
| 21 | src_white_Y ignored | caught |
| 22 | black not added to XYZ | caught (after the test got a black with light) |
| 23 | set_background without the contrast spaces | caught (after the test compared with a fresh context) |
| 24 | the context id without the lum CRC | caught (after the test compared ids) |
| 25 | output rounding at 0.49 | caught (after the test got values 0.002 of a code from a half) |
| 26 | cone contrast without the background | caught |
| 27 | CLUT inverse end | caught |
| 28 | DKL S row without w_S | caught (after the test checked the luminance axis with an S weight) |
| 29 | the gamut tolerance ignored | caught |
| 30 | map not returning in-gamut colors as they are | caught |
| 31 | BT.2020 alpha 1.099 | caught |
| 32 | the display's white without its black | caught |

32 of 32.

### Builds

Warnings as errors (MSVC `/W4 /WX`; gcc `-Wall -Wextra -Wpedantic
-Wshadow -Werror`).

| Toolchain | Builds | Runs |
|---|---|---|
| MSVC 19.44 (VS 2022), CMake and Ninja, the whole tree after stage C, SDL3 3.4.0 | every target | ctest: 45 of 45 pass (ysp_gfx's GL test excluded there; it ran on the three renderers above) |
| MSVC 19.44, `cl` | the test in the default C dialect, the gfx test | pass |
| MinGW-w64 gcc 14.3 | compile checks C99, C11, C++17, the macros as C++20, the test with `-DYCOL_API=static`, both examples; after stage C, ysp_gfx's and ysp_video's compile checks and tests and gfx_gallery | pass; color_convert exits 0 |
| gcc 11.4 (WSL2), ASan and UBSan | the test, the gfx test | pass, no report |
| gcc 13.3 (WSL2, Ubuntu 24.04), 2026-10-06 | the test at -O2 and under ASan and UBSan | pass, no report |
| gcc 13.3 and clang 18.1 (Ubuntu 24.04 container, the CI jobs' flags), 2026-10-06 | every target, ctest | pass |
| MinGW-w64 gcc 16.2 (MSYS2 MINGW64), 2026-10-06 | the MinGW CI job: every compile check and test | pass |
| emcc 6.0.10, node, 2026-10-06 | the CI wasm job: the compile check, the test, the gfx test | pass |

### Not done

- No measurement of light: every number is arithmetic.
- No Apple clang build here (CI's macOS jobs build it).
- The WebAssembly shim for the designer.

## Cost

`examples/color/bench.c`, under the measurement lock (owner "color", no
compiler or test running), on the Iris Xe laptop,
2026-10-06 14:41; power source not recorded. A nominal calibration with
Gaussian spectra; the median of 9 rounds of 100000 calls (10000 for the
per-space rows). Two runs of the MSVC 19.44 `/O2` build (CMake Release)
and one of MinGW gcc 14.3 `-O2`; the first MSVC run was 10 to 30 percent
slower than the second throughout.

Power source not recorded for any column; the next run records it.

| Case | Bar | MSVC run 1 | MSVC run 2 | MinGW |
|---|---|---|---|---|
| DKL spherical to a direction, one call (per element per frame) | 100 ns | 26.9 ns | 21.7 ns | 78.7 ns |
| OkLCh gamut ring, 360 hues (the picker) | 16 ms | 1.04 ms | 0.78 ms | 1.29 ms |
| DKL gamut ring, 360 azimuths | | 10 us | 6 us | 38 us |
| context init, SS2; SS10 | | 769 us; 743 us | 581 us; 676 us | 464 us; 539 us |

Per call, from RGB and to RGB, MSVC run 2 (ns): RGB 7.6, 6.8; DEVICE
10.7, 106.5 (a binary search in a 4096-entry CLUT); XYZ, xyY, LMS, CONE,
DKL_CART, MB 5 to 11; CIELAB, Oklab 40 to 42 and 19 to 20 (a cube root
one way); the polar forms 63 to 74 and 19 to 36; sRGB, Display P3 and
Rec. 2020 45 to 47 and 44 to 49 (pow); DKL spherical 26 and 28. MinGW's
libm is slower for pow and cbrt: sRGB 268 ns and 226 ns, CIELAB 130 ns.
Every bar is met on both compilers. A frame that converts 10000 DKL
colors costs 0.2 to 0.8 ms of CPU; a context per background is a setup
cost (0.5 to 0.8 ms: mostly the calibration's CRC over 62 KB), not a
per-frame one.

## Compared with other toolboxes

| Toolbox | Taken | Rejected |
|---|---|---|
| Psychtoolbox PsychColorimetric (Brainard) | settings, primary and sensor as levels; the calibration as the context of a conversion; out-of-gamut entries reported (`badIndex`); `MaximizeGamutContrast`; `ComputeDKL_M` exactly; a luminance vector as an argument (`LMSToMacBoyn`) | settings clipped by `PrimaryToGamut` beside the report; `SetSensorColorSpace` changing the calibration in place |
| BrainardLabToolbox | the calibration as an object with a stated gamma method; an exact inverse beside the table | a choice of gamma models: one monotone cubic, stated, rebuilt from the readings |
| PsychoPy | a color as a value with a space; DKL angles (elevation 90 luminance, azimuth 0 L-M, 90 S); a Cartesian DKL space | signed RGB in -1..1 as the default; DKL radius normalized to the monitor's gamut (as read in `makeDKL2RGB`); a default conversion matrix when there are no spectra |
| colour-science | `source_to_target` names; an explicit white; one `convert` entry; its data and functions as the test reference | path-finding through a conversion graph with hidden adaptation defaults; a global domain-range scale |
| Color.js, culori | gamut mapping named by space and channel; `inGamut` and `displayable` as plain queries; the hex constructor | CSS Color 4's final clip; CSS `lab()`'s D50 white |

## Enum values in the parameter table (v0.3.0)

Every ysp parameter table has one row shape: `name`, `type`, `min`, `max`, `def`, `unit`, `doc`, `values` ([conventions.md](conventions.md), "Parameter tables"; gap G6 of [experiment_format.md](experiment_format.md)). `ycol_param` gained `values` after `offset`: `cones` lists
`ss2=0,ss10=1` and `adapt` lists `none=0,bradford=1`; the other rows have
NULL. The first seven fields are the shared shape's, and a reader by
field name does not see that `offset` sits between `doc` and `values`.
Moving `offset` to the end instead would change it for every reader
that knows it, for no gain.

The test (`test_params()` in `tests/adapt/color_test.c`) checks every
row against the `ycol_ctx_desc` field at its offset, the names against
`switch`es on `ycol_cones` and `ycol_adapt` without a `default`, every
default (all 0: a value, or the display's white and the relative D65
scale the doc names) against a context opened with a zero desc, and that
a context opened with the defaults written in has the zero desc's id.
2 of 2 new mutants killed (`tests/mutate/color.toml`, pv-). No conversion changed.

## Michelson contrast in light (v0.4.0)

Gap G5 of [experiment_format.md](experiment_format.md): a contrast
threshold is a statement about light, ysp/gfx.h's contrast is about linear
device RGB, and the 8-bit codes show a third value. The 2AFC example
(`examples/response/trial_2afc_adaptive.c`) did the arithmetic in about 20
lines of its own; a definition's `{"michelson": ...}` needs it in the
header.

| Question | Decision | Why |
|---|---|---|
| One call or two | One, `ycol_michelson(cx, m, dir, bits, &r)`, with every number in `ycol_shown`: `gain`, `scene`, `max`, `shown`, the light of the background, peak and trough, the codes, the gamut | The caller needs all of them on every trial (the stimulus takes `scene`, the method takes `shown`, the row takes the codes) and they share the luminance row and the codes. A second call for gfx's contrast back to light is `scene * gain`, which the result already holds |
| Light from what | Y of the readings: each gun's level-1 reading less the black's, and the black's | A photometer gives Y only. The context's XYZ matrix needs chromaticities, so a contrast study on a luminance meter's calibration would be refused for no reason. With chromaticities, these equal row 1 of `rgb_to_xyz` and `black_xyz[1]`, so the call agrees with the xyY path (test: within 1.1e-16) |
| Which luminance | Photometric Y, not the context's `lum` record | Michelson contrast is CIE photometry. A participant's luminance (flicker photometry) sets the DKL isoluminant plane; it is not a contrast in cd/m2 |
| The direction | `dir` in linear RGB per unit contrast, all 0 meaning the background, as ysp/gfx.h's `dir` | The same field means the same thing in both headers. A colored grating's luminance contrast comes from the same call; a direction with no luminance is refused, not divided by 0 |
| The gamut | `max` is the largest Michelson contrast with both extremes in the cube; past it, `scene` is not clipped and `gamut` (`ycol_to_dir()`'s check of bg + scene dir and bg - scene dir) says by how much; `shown` is what the output stage's clamp gives | The header's rule: report, never clip. The display does clip, so the contrast shown is the clipped one; a method that hears `shown` sees the ceiling |
| The codes | `ycol_output_code()` of the nominal peak and trough, dither NONE, at `bits` (8 to 16) | The output stage's arithmetic, already checked against ysp/gfx.h. An RGBA16F scene rounds a value first, and a gabor's drawn extremes sit at or inside the nominal ones (its envelope, the pixel grid); the manual says so instead of modelling a pixel the caller did not name |
| The sign | A negative contrast is the opposite phase: the codes swap and `shown` is negated, exactly | ysp/gfx.h's contrast takes a sign; a method may step through 0 |

Results (2026-10-21; MinGW-w64 gcc 16.1 `-std=c99 -O2 -Wall -Wextra
-Wpedantic -Wshadow -Werror`, MSVC 19.44 through ctest; arithmetic only,
nothing measured with light). `test_michelson()` in
`tests/adapt/color_test.c`:

| Check | Result |
|---|---|
| The 2AFC example's arithmetic (xyY luminance, `Y_bg / (Y_bg - Y_black)`, codes of 0.5 (1 +- c), their light through DEVICE), 401 contrasts 0 to 0.2 on a display with chromaticities and 0.4 cd/m2 of black | 0 of 2406 codes differ; scene, shown, max and the three luminances within 1.1e-16; `shown` never decreases as the contrast rises |
| Quantization, a mid-gray background, gamma 2.2 | `shown` minus the contrast asked, in gamut: at most 0.00561 at 8 bits (one code step there is about 0.0067) and 2.08e-5 at 16 bits |
| Edges | 0 gives the background's code twice and 0; 0.0005 gives a nonzero scene and 0 shown; `max` gives codes 255 and 0 (1023 and 0 at 10 bits), in gamut, and shows itself within 1e-15; 1.25 max gives scene 1.25 (not clipped), `gamut.scale` 0.8, `distance` 0.125, below and above on every channel, and shows max; the opposite phase swaps the codes and negates `shown` |
| Directions and calibrations | the red gun alone: gain from its luminance, max from its room, the green and blue codes unchanged; a luminance-only calibration (no chromaticities) gives Y from the readings; at a background of 0.2 with no black, contrast 1 puts the trough at code 0 and shows 1 |
| Refusals | no context (also a closed one), bits 7 and 17, a contrast or a dir that is not finite, a background with no light (also along a colored dir), a dir with no luminance |
| Mutants | 10 of 10 killed (`tests/mutate/color.toml`, `mi-01` to `mi-10`) |

The 2AFC example now calls it; its `--sim` run gives the same rows as
before the change.

## Bindings

### Python: `ysp-color`, module `ysp.color`

`bindings/python/ysp_color/`, the Limited API (one abi3 wheel per
platform, CPython 3.8 and later), PEP 420 `ysp` namespace, added to the CI
wheel matrix. `Calibration`, `Lum` and `Context` are types; `Color` and
`Gamut` are namedtuples; each space has a constructor with the header's
field names (`pc.dkl(elev=0, azim=90, contrast=0.1)`); `convert_n` takes a
buffer of doubles or a sequence of triples and returns a `'d'` memoryview,
as ysp.aep does. `Context.michelson(m, dir=(0, 0, 0), bits=8)` (v0.4.0)
returns a `Shown` namedtuple. A Context keeps references to its Calibration and Lum, so
the pointers in the header's context stay valid. Errors: `Error`, and
`ArgumentError`, `RefusedError`, `FormatError`, `RangeError`, which are
also `ValueError`. `tests/test_color.py`: 11 tests (the file round trip,
refusals with their messages, conversions, gamut edges, sRGB primaries in
gamut on a nominal display, `convert_n` against single calls, mapping and
rings, the luminance fit, the tables, and since v0.4.0 `michelson()`: its
values against their definitions, the gamut's edge, 16 bits, the opposite
phase, a colored and an isoluminant dir); all pass on CPython 3.14
(Windows, MSVC).

### MATLAB and Octave: `ysp_color` MEX

`bindings/mex/ysp_color.c`, command dispatch as the other ysp MEX files.
Calibrations and luminance records travel as their canonical bytes.

| Command | What it does |
|---|---|
| `cal = ysp_color('cal_derive', readings, [spectra], [nominal_spectra])` | readings N x 5 (gun, level, Y, x, y); spectra M x 4 or 5 (nm, r, g, b, black), each gun at full output, the black's light included |
| `cal = ysp_color('cal_nominal', xy, white_Y, gamma)` | xy 4 x 2 (R, G, B, white) |
| `info = ysp_color('cal_info', cal)` | flags, CRC, matrices (3 x 3), black, CLUT (n x 3), describe |
| `h = ysp_color('open', struct('cal', cal, 'background', bg, 'cones', 'ss2', 'lum', lum, 'white', w, 'src_white_Y', y, 'adapt', 'none'))` | a context |
| `[rgb, g] = ysp_color('to_rgb' or 'to_dir', h, space, X)` | rows of colors; g a struct of columns (in, below, above, margin, distance, scale, kept) |
| `[Y, in, flags] = ysp_color('convert', h, src, dst, X)` | a row out of range is NaN (in 0, flags -1); any other refusal raises |
| `k = ysp_color('max_scale', h, space, X, 'one' or 'symmetric')` | per row |
| `r = ysp_color('ring', h, plane, fixed, n, sides)` | 'dkl', 'cone', 'oklch', 'cielch' |
| `[rgb, g] = ysp_color('map', h, space, X, method)` | 'scale', 'chroma_oklch', 'chroma_cielch', 'chroma_dkl', 'clip' |
| `lum = ysp_color('lum_derive', settings, cal, opts)`, `ysp_color('lum_stated', w, cones)`, `ysp_color('lum_info', lum)` | settings N x 6 (a, b) |
| `sh = ysp_color('michelson', h, m, [dir], [bits])` | a Michelson contrast in light (v0.4.0): a struct with `gain`, `scene` (ysp/gfx.h's contrast), `max`, `shown`, `Y_bg`, `Y_peak`, `Y_trough`, `peak` and `trough` (1 x 3 output codes) and `gamut` (as `to_dir`'s). `dir` defaults to `[0 0 0]` (the background), `bits` to 8. Past the gamut it is reported, not clipped; a `dir` with no luminance raises `ysp_color:range` |
| `ysp_color('lum_pair', h, azim, elev, contrast)`, `'info'`, `'describe'`, `'set_background'`, `'close'` | the rest |
| `ysp_color('output_code', cal, RGB, bits)`, `ysp_color('cone_fundamentals', nm, cones)`, `ysp_color('version')` | without a handle |

Errors carry `ysp_color:arg`, `:refused`, `:format`, `:range`, `:handle`.
`bindings/mex/test_mex_color.m` passes in MATLAB R2023a and Octave 10.1
(Windows); CI runs it on Octave 7.3 and MATLAB. Its `michelson` checks
(v0.4.0, the Python test's cases, and the codes against `output_code`;
70 checks in all, 53 before) passed in Octave 10.1 and MATLAB R2023a
(MSVC 2022) on Windows on 2026-10-22.

### Against Psychtoolbox

`tests/compare/color_ptb.m`, MATLAB R2023a Update 6, Psychtoolbox-3
from `source/matlab/psychtoolbox-3`:

| Check | Measured | Tolerance |
|---|---|---|
| ComputeDKL_M at three backgrounds of B_monitor, standard luminance: max relative difference | 3.7e-16 | 1e-9 |
| ComputeDKL_M with a participant's 0.75 L + 0.30 M (ysp_color's `lum_stated`) | 6.2e-16 | 1e-9 |
| LMSToMacBoyn with T_cones_ss2 and T_CIE_Y2, 500 colors: l (absolute) | 9.7e-8 | 1e-6 |
| the same: s, luminance (relative) | 1.2e-7, 7.9e-8 | 1e-6 |
| SensorToPrimary on PTB3TestCal at 1 nm (ambient as the black), 500 colors, 282 outside the gamut: primaries (absolute) | 1.5e-8 | 1e-7 |
| SensorToSettings's badIndex against ysp_color's `in` | 0 of 500 differ | 0 |
| MaximizeGamutContrast against `max_scale` symmetric, 500 directions and backgrounds | 7.3e-16 relative | 1e-12 |

LMSToMacBoyn fits its luminance weights to T_CIE_Y2 (0.68990262,
0.34832198); ysp_color takes the published 0.68990272, 0.34832189: that is
the 1e-7. Psychtoolbox's P_device is the light above the ambient;
ysp_color's spectra are each gun at full output, the black included, so
the comparison gives ysp_color P_device + P_ambient. The primaries differ
by the float storage of the spectra (the .yspcal file keeps float). The
settings themselves pass through each toolbox's own gamma inversion and
are not compared.

### WebAssembly, planned

The header and a flat C shim (`ycolw_*`) built with emcc, no struct
passed by value, for the designer's picker. The picker stores colors in
the pack as a space and three numbers, never as device RGB; the player
converts them at pack load with the rig's calibration and reports colors
outside that rig's gamut then.
