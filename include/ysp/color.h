/* ysp/color.h - v0.2.0 - public domain single-header color library for
 *   calibrated displays
 *
 *   Color for psychophysics on a measured display: the display calibration
 *   (photometer readings in, a lookup table and the matrices into CIE 1931
 *   XYZ and cone space out, a canonical file), a conversion context, color
 *   values tagged with their space, conversions in both directions between
 *   linear device RGB and XYZ, xyY, CIELAB, CIELUV, Oklab, sRGB, Display P3,
 *   Rec. 2020, cone excitations, cone contrast, DKL and MacLeod-Boynton,
 *   gamut questions (in gamut by how much, the largest contrast along a
 *   direction, the gamut boundary in a plane), gamut mapping by a stated
 *   method, and a participant's luminance from flicker photometry.
 *
 *   Three rules hold for every call. A conversion goes through a
 *   calibration: measured, or nominal and flagged, never an assumed sRGB
 *   display. A color outside the gamut comes back unclipped, with its
 *   distance. A conversion that the calibration cannot support is refused,
 *   with a message that names what is missing.
 *
 *   Written in the single-header style of the stb / sokol libraries. Pure
 *   computation: no OS calls, no threads, no heap, no I/O. Needs nothing
 *   but libm. Not a transport header, so it does not include ysp/rt.h.
 *   C99 is the floor: it builds as C99, C11 and C++17, and in the C dialect
 *   MSVC compiles by default. The YCOL_<SPACE>(...) value macros are
 *   designated initializers: C99, or C++20.
 *
 *   ---------------------------------------------------------------------
 *   USAGE
 *   ---------------------------------------------------------------------
 *   Do this:
 *
 *       #define YSP_COLOR_IMPLEMENTATION
 *
 *   in *one* C or C++ file before including this header to create the
 *   implementation. Every other file just includes the header normally.
 *
 *       #define YSP_COLOR_IMPLEMENTATION
 *       #include "ysp/color.h"
 *
 *       static ycol_cal cal;          // 62528 bytes: static or heap
 *       char err[200];
 *       if (ycol_cal_load(&cal, bytes, n, err, sizeof err) < 0) fail(err);
 *
 *       static ycol_ctx cx;           // about 2 KB, read-only after open
 *       ycol_ctx_desc d = { 0 };
 *       d.cal = &cal;
 *       d.background[0] = d.background[1] = d.background[2] = 0.5;
 *       if (!ycol_ctx_open(&cx, &d)) fail(ycol_ctx_error(&cx));
 *
 *       ycol_gamut g;
 *       ycol_rgb dir = ycol_to_dir(&cx, ycol_make(YCOL_SPACE_DKL, 0, 90, 0.05), &g);
 *       if (!g.in) printf("out by %.3g; %.3g of it fits\n", g.distance, g.scale);
 *       double kmax = ycol_max_scale(&cx, ycol_make(YCOL_SPACE_DKL, 0, 0, 1),
 *                                    YCOL_SYMMETRIC);
 *       ycol_rgb orange = ycol_to_rgb(&cx, ycol_hex("#ff8800"), &g);
 *       ycol_rgb soft   = ycol_map(&cx, ycol_make(YCOL_SPACE_OKLCH, 0.75, 0.2, 30),
 *                                  YCOL_MAP_CHROMA_OKLCH, &g);
 *       ycol_color lab  = ycol_from_rgb(&cx, orange, YCOL_SPACE_CIELAB, NULL);
 *       ycol_store3f(stim.dir, dir);  // ysp/gfx.h takes float[3]
 *       ycol_ctx_close(&cx);
 *
 *   In C99 and C++20 the value macros name the fields:
 *   YCOL_DKL(.azim = 90, .contrast = 0.05) is ycol_make(YCOL_SPACE_DKL, 0,
 *   90, 0.05). A value macro needs at least one field: YCOL_DKL() expands
 *   to empty braces, which C99 refuses.
 *
 *   ---------------------------------------------------------------------
 *   MODEL
 *   ---------------------------------------------------------------------
 *   LINEAR DEVICE RGB
 *     Per gun 0..1, proportional to that gun's light above the display's
 *     black: ysp/gfx.h's scene value. The gamut is the cube [0, 1]^3.
 *   THE CALIBRATION HAS THREE LEVELS
 *     readings (Y per gun and level, a black)   RGB, DEVICE
 *     + chromaticities x, y at level 1 per gun, and for a black with
 *       light (YCOL_CAL_HAS_XY)               XYZ and every space made
 *                                               from it (CIE 1931 2 degree)
 *     + the primaries' spectra
 *       (YCOL_CAL_HAS_SPECTRA)                LMS and every cone space
 *     A conversion that needs a level the calibration lacks is refused
 *     with YCOL_ERR_REFUSED and a message. There is no CIE 1931 to cone
 *     approximation: one measured 7 to 16 percent S-cone leak on a DKL
 *     L-M axis (docs/color.md).
 *   THE CONTEXT
 *     ycol_ctx holds one calibration, one cone observer, one luminance
 *     definition, one background and one reference white, and the
 *     matrices derived from them. A calibration alone cannot answer a DKL
 *     question (it needs a background and a luminance) or a CIELAB one (it
 *     needs a white). The context is the unit to log:
 *     ycol_ctx_describe() gives one line, ctx.id a 32-bit CRC of all of
 *     it. ycol_ctx_open() fills it, ycol_ctx_close() empties it (there is
 *     nothing to release).
 *   ERRORS AND THREADS
 *     ycol_ctx_open() and ycol_ctx_set_background() write the context:
 *     on failure, the message (ycol_ctx_error()) and the code
 *     (cx->status). Every call that takes a const ycol_ctx* only reads
 *     it, so several threads can share one context while nothing calls
 *     those two. The readers do not write the message: a conversion
 *     reports in its ycol_gamut (status, why) or its status argument, and
 *     the others return the code (ycol_strerror()). The calibration and
 *     lum calls that take char* err, size_t cap write a caller buffer,
 *     which may be NULL.
 *   ABSOLUTE AND RELATIVE
 *     XYZ, xyY and LMS are absolute: the light at the eye, the display's
 *     black included (measure the black in the room light of the
 *     experiment). CIELAB, CIELUV and Oklab take that absolute XYZ against
 *     the reference white: desc.white, or by default the XYZ of device
 *     (1, 1, 1), black included. Oklab divides by the white's Y and uses
 *     Ottosson's M1 with no chromatic adaptation, unless desc.adapt is
 *     BRADFORD: then the white maps to D65 first.
 *   D65 SOURCES
 *     sRGB, Display P3 and Rec. 2020 values are encoded values of a D65
 *     space: decoded by the space's transfer function (sign kept), taken
 *     to XYZ by the matrix made from the space's primaries and D65, then
 *     shown on this display. desc.src_white_Y = 0 (the default): relative,
 *     the source's white has the display's white luminance and the
 *     source's black is the display's black (ysp/gfx.h's video choice).
 *     desc.src_white_Y > 0: absolute, the source's white has that many
 *     cd/m2, and its black is XYZ 0, below the display's black. desc.adapt
 *     BRADFORD maps D65 onto the display's white; NONE (the default)
 *     keeps D65's chromaticity. ycol_ctx_describe() prints the choice.
 *   CONTRAST SPACES
 *     Cone contrast, DKL (spherical and Cartesian) are about the context's
 *     background. DKL is Brainard's (1996) ComputeDKL_M as in Psychtoolbox:
 *     luminance V = w . LMS, the L-M axis keeps luminance and S, the S axis
 *     keeps L and M, and each isolating direction at a pooled cone contrast
 *     of 1 is a unit vector. Spherical: elevation 90 is +luminance, azimuth
 *     0 is +(L-M), azimuth 90 is +S, all in degrees; contrast is the radius.
 *     This is PsychoPy's convention for the angles, not for the units:
 *     PsychoPy's radius 1 is the edge of its monitor's gamut on each axis,
 *     which changes with the display. Use ycol_max_scale() for the edge.
 *   ANGLES
 *     Hue and azimuth in degrees, 0 to 360. Below a chroma of 1e-12 (or a
 *     DKL contrast of 0, or an elevation of +-90 degrees) the hue or azimuth
 *     is 0 and YCOL_F_ACHROMATIC is set on the result.
 *
 *   ---------------------------------------------------------------------
 *   SPACES
 *   ---------------------------------------------------------------------
 *   space         values                     needs     notes
 *   RGB           r, g, b                    -         linear device RGB
 *   DEVICE        r, g, b in 0..1            -         drive values (an 8-bit
 *                                                      code is k / 255), through
 *                                                      the inverse of the stored
 *                                                      CLUT; refused outside 0..1
 *   XYZ           X, Y, Z (cd/m2)            XY        CIE 1931 2 degree
 *   XYY           x, y, Y                    XY        Y = 0 gives the white's x, y
 *   CIELAB        L, a, b                    XY        CIE 1976 about the white
 *   CIELCH        L, C, h                    XY
 *   CIELUV        L, u, v                    XY
 *   CIELCHUV      L, C, h                    XY
 *   OKLAB         L, a, b                    XY        Ottosson 2020, L 1 at the white
 *   OKLCH         L, C, h                    XY
 *   SRGB          r, g, b encoded            XY        IEC 61966-2-1
 *   DISPLAY_P3    r, g, b encoded            XY        P3 primaries, D65, sRGB's
 *                                                      transfer
 *   REC2020       r, g, b encoded            XY        BT.2020 primaries, D65, the
 *                                                      inverse of BT.2020's OETF
 *   LMS           l, m, s                    SPECTRA   cone excitations, the
 *                                                      calibration's spectral units
 *                                                      on fundamentals of peak 1
 *   CONE          l, m, s contrast           SPECTRA   (LMS - LMS_bg) / LMS_bg
 *   DKL           elev, azim, contrast       SPECTRA   spherical, degrees
 *   DKL_CART      lum, lm, s                 SPECTRA   Cartesian
 *   MB            l, s, lum                  SPECTRA   MacLeod-Boynton: l = wL L / V,
 *                                                      s = k S / V, lum = V = w . LMS;
 *                                                      k puts the spectrum locus's
 *                                                      largest s at 1
 *   The contrast spaces also need a background whose L, M and S
 *   excitations are above 0. ycol_convert() between the XYZ family and
 *   the cone family goes through this display's RGB and sets
 *   YCOL_F_VIA_DEVICE: XYZ does not fix LMS; a display does.
 *   String names (ycol_spaces(), ycol_format()) are "rgb", "device",
 *   "xyz", "xyy", "cielab", "cielch", "cieluv", "cielchuv", "oklab",
 *   "oklch", "srgb", "display-p3", "rec2020", "lms", "cone", "dkl",
 *   "dkl-cart", "mb". "cielab", not "lab": CSS's lab() has a D50 white.
 *
 *   ---------------------------------------------------------------------
 *   GAMUT
 *   ---------------------------------------------------------------------
 *   ycol_to_rgb() never clips and never maps. ycol_gamut tells:
 *     in        every channel in [-eps, 1 + eps], eps = YCOL_GAMUT_EPS
 *               = 2^-20 = 9.54e-7, below 1/1000 of a 10-bit code. Why:
 *               the calibration stores its readings as float, each to a
 *               relative 2^-24 (u). The first-order error that storage
 *               puts in a device value, the sum over every stored float f
 *               of |d rgb / d f| f u, is at most 7.2 u for a nominal
 *               display with sRGB's primaries and an sRGB source at the
 *               corners of its cube (5.8 u for P3, 4.7 u for BT.2020;
 *               the test measures at most 0.9 u at those corners and 1.4
 *               u for #ff8800). eps is 16 u, over twice the largest bound,
 *               so a color the source puts on its gamut's boundary is in
 *               gamut on such a display.
 *     margin[k] min(v, 1 - v): the room to the nearer bound, negative
 *               outside; exact, no tolerance
 *     distance  max(0, -min margin): how far outside, in linear RGB
 *     scale     the largest k with bg + k (rgb - bg) inside; 1 or more is
 *               in gamut. ycol_to_dir() checks bg + dir and bg - dir.
 *   ycol_max_scale(c, sides) is the largest k that keeps k times the
 *   color's offset from the background inside: for DKL it multiplies the
 *   contrast, for CONE and DKL_CART the vector, for other spaces the line
 *   in linear RGB from the background. YCOL_SYMMETRIC checks both signs,
 *   as a grating swings. It is Psychtoolbox's MaximizeGamutContrast.
 *   ycol_max_ring() gives the boundary in a plane as n radii: DKL at an
 *   elevation, cone contrast at an elevation toward S, OkLCh or CIE LCh at
 *   a lightness (by bisection on the chroma; -1 where a gray of that
 *   lightness is outside).
 *   ycol_map() brings a color inside by a stated method: SCALE toward
 *   the background (keeps the direction in every linear space),
 *   CHROMA_OKLCH or CHROMA_CIELCH (less chroma at the same lightness and
 *   hue), CHROMA_DKL (less of the isoluminant part, the luminance part
 *   kept), CLIP (per channel). A color in gamut comes back bit for bit.
 *   The bisections assume that the chroma in gamut at one lightness and
 *   hue is one interval from 0, true for every display tested.
 *
 *   ---------------------------------------------------------------------
 *   CALIBRATION
 *   ---------------------------------------------------------------------
 *   ycol_cal is the canonical form, in memory and on disk (62528 bytes,
 *   fixed offsets, little-endian, CRC-32). Fill it:
 *       ycol_cal_init(&c);
 *       ycol_cal_add(&c, YCOL_GUN_BLACK, 0, Y, x, y);     // all guns 0
 *       ycol_cal_add(&c, gun, level, Y, x, y);              // per gun, to 1
 *       ycol_cal_add(&c, YCOL_GUN_WHITE, 1, Y, x, y);     // additivity
 *       ycol_cal_set_spectra(&c, 380, 4, n, r, g, b, black); // optional
 *       ycol_cal_derive(&c, err, sizeof err);
 *   derive() linearizes each gun, Y' = (Y - Yblack) / (Y(1) - Yblack), by a
 *   monotone cubic (Fritsch and Carlson) inverted into the CLUT; refuses
 *   readings whose luminance does not rise, naming the level; gives RGB
 *   to XYZ (CIE 1931) from the full-level chromaticities, and RGB to LMS
 *   from the spectra with the Stockman and Sharpe (2000) 2-degree cone
 *   fundamentals (CVRL linss2_10e_1, embedded), the spectra interpolated
 *   linearly onto 1 nm. Black is removed from the matrices and kept apart
 *   (black_xyz, black_lms). derive() also seals the CRC. Any change after
 *   it (add(), set_spectra(), a field written by hand) leaves the CRC
 *   stale: call derive() again, or save(), which seals what is there. A
 *   context (and ysp/gfx.h's open) refuses a calibration that is not
 *   sealed.
 *   save() sets the CRC and copies the bytes; load() refuses another size,
 *   magic or version, a bad CRC, and a file whose stored matrices or CLUT
 *   differ from what its readings give: a calibration can always be
 *   rebuilt from its readings. ycol_cal_nominal() makes one from stated
 *   primaries and a gamma (an EDID's, sRGB's), flagged NOMINAL: not a
 *   measurement. ycol_cal_set_spectra_nominal() takes spectra from a
 *   datasheet and flags them SPECTRA_NOMINAL. Both flags appear in every
 *   describe line.
 *   The raw calls ycol_cal_lms(), _dir_cone(), _dkl_matrix(), _dir_dkl(),
 *   ycol_dkl_from_sph() and ycol_max_contrast() are ysp/gfx.h v0.3's,
 *   kept bit for bit for it. New code uses the context.
 *
 *   ---------------------------------------------------------------------
 *   OBSERVERS
 *   ---------------------------------------------------------------------
 *   XYZ is always CIE 1931 2 degree, from the readings' chromaticities.
 *   Cones: desc.cones is SS2 (Stockman and Sharpe 2000, 2 degree, the CIE
 *   170-1:2006 fundamentals, CVRL linss2_10e_1; the default) or SS10 (the
 *   10 degree set, CVRL linss10e_1). The file stores the SS2 matrix; the
 *   context integrates SS10 from the stored spectra by the same loop.
 *   Standard luminance in cone units: 0.68990272 L + 0.34832189 M (2
 *   degree), 0.69283932 L + 0.34967567 M (10 degree). MacLeod-Boynton's s
 *   scale k puts the largest S / V on the spectrum locus at 1 (at 418 nm
 *   for both tables).
 *
 *   ---------------------------------------------------------------------
 *   ISOLUMINANCE
 *   ---------------------------------------------------------------------
 *   At the null of heterochromatic flicker photometry, minimum motion or
 *   a minimally distinct border, two lights a and b look equally luminous
 *   to the participant: w . (LMS(a) - LMS(b)) = 0, where w is the
 *   participant's luminance in cone units. ycol_lum keeps the raw
 *   settings (the two lights in linear device RGB, the calibration's CRC,
 *   the surround, repeats, SD, field size, eccentricity and frequency) and
 *   the weights that derive() fits from them:
 *       ycol_lum_init(&l, YCOL_CONES_SS2, YCOL_LUM_HFP);
 *       ycol_lum_pair(&cx, 0, elev, 0.1, a, b);   // a probe at an elevation
 *       ycol_lum_add(&l, a, b, bg, sd, n);        // the null the participant set
 *       ycol_lum_derive(&l, &cal, err, sizeof err);
 *   One setting fixes wL / wM with wS = 0; with YCOL_LUM_FIT_S, two
 *   independent settings fix wS too. The weights are scaled so that an
 *   equal-energy spectrum has the standard luminance. derive() refuses
 *   another calibration than the settings', no spectra, a = b, too few
 *   settings and a negative weight; resid[] is each setting's luminance
 *   contrast left. ycol_lum_stated() takes published weights (STATED).
 *   Give the record to the context (desc.lum). It sets the DKL luminance
 *   row, so the isoluminant plane is the participant's, and
 *   MacLeod-Boynton's l and luminance. It does not touch cone contrast,
 *   XYZ, CIELAB, CIELUV, Oklab or the D65 sources: those are CIE
 *   colorimetry. The file (.ysplum, 1688 bytes, CRC-32) keeps its settings,
 *   so ycol_lum_check() can fit the weights again.
 *
 *   ---------------------------------------------------------------------
 *   EXACTNESS
 *   ---------------------------------------------------------------------
 *   The math is double. The calibration stores float readings, spectra
 *   and CLUT; the matrices are double. Round trips through each space are
 *   within 1.4e-13 of the input (DKL spherical; 8.4e-15 for the rest,
 *   docs/color.md). Oklab's M2 is inverted exactly: Ottosson's printed
 *   10-digit inverse, which ysp/gfx.h's shader uses, is 2.4e-7 from it in
 *   linear RGB, inside the paint tolerance. C libraries differ by a few
 *   ulp in cbrt, sin, cos and atan2, so two platforms can differ in the
 *   last bits; the calibration itself uses none of them and gave the same
 *   bits on the three x64 compilers tested.
 *   ycol_output_code() repeats ysp/gfx.h's output stage in float (the
 *   CLUT interpolated, rounded at floor(v * max + 0.5), dither NONE) for
 *   a value the scene holds; an RGBA16F scene rounds a value first.
 *
 *   ---------------------------------------------------------------------
 *   COST
 *   ---------------------------------------------------------------------
 *   ycol_ctx_open() checks the calibration's CRC (62 KB) and, for SS10,
 *   integrates 441 nm of spectra: a setup call. A conversion is a few
 *   3 x 3 products and, for the uniform spaces, cube roots or
 *   trigonometry: 5 to 75 ns a call (STATUS; examples/color/bench.c
 *   measures them on your machine).
 *
 *   ---------------------------------------------------------------------
 *   LEFT OUT
 *   ---------------------------------------------------------------------
 *   ICC profiles, color appearance models (CIECAM02, CAM16), color
 *   difference formulas, reflectances and spectral rendering, named
 *   colors. The CIE 2006 age and field-size model (it needs lens and
 *   macular templates and a set of parameters per participant) and the
 *   Smith-Pokorny and Judd-Vos fundamentals (older work only): out of
 *   v0.1. The CIE 1964 10 degree XYZ (it needs spectra and its color
 *   matching functions). A cone space from XYZ alone (see MODEL).
 *
 *   ---------------------------------------------------------------------
 *   BUILDING
 *   ---------------------------------------------------------------------
 *   Link libm on Linux. Define YCOL_API to override the default `extern`
 *   linkage; -DYCOL_API=static needs a translation unit that calls
 *   every function, or -Wno-unused-function.
 *
 *   ---------------------------------------------------------------------
 *   CHANGELOG
 *   ---------------------------------------------------------------------
 *   v0.2.0 - The ysp conventions (README.md, Conventions); breaks the API.
 *          ycol_ctx_init(cx, d, err, cap) is bool ycol_ctx_open(cx, d):
 *          the message is in the context (ycol_ctx_error()) and the code
 *          in cx->status. ycol_ctx_close() is new; it has nothing to
 *          release. ycol_ctx_set_background() writes the message and
 *          cx->status when it fails. The calls that take a const context
 *          (the conversions, the gamut calls, ycol_ctx_describe()) read
 *          it only and keep their error reports, so threads can share a
 *          context. The calibration and lum records keep init, load,
 *          save, derive and check with a caller buffer. USAGE builds as
 *          C99 and C++17. No conversion changed.
 *   v0.1.0 - first version. The calibration (ycol_cal, the .yspcal
 *          file) is ysp/gfx.h v0.4's ygfx_cal, field for field and byte
 *          for byte, with its code; ysp/gfx.h v0.5 uses this header
 *          (docs/color.md). The gamut tolerance is 2^-20, the
 *          precision of the calibration's float storage (GAMUT). New: the
 *          context, the color
 *          values, the conversions, the gamut calls, the 10 degree cone
 *          fundamentals, the participant luminance record (ycol_lum,
 *          the .ysplum file) and nominal spectra.
 *
 *   STATUS: v0.2.0: the v0.1.0 checks below pass unchanged, and the test
 *   checks the context's message, code and close. v0.1.0: built and run with MSVC 19.44 (/W4 /WX, the default
 *   C dialect and C++17), MinGW-w64 gcc 14.3 (C99, C11, C++17) and gcc
 *   11.4 on Linux under ASan and UBSan; the value macros as C++20 on MSVC
 *   and g++. tests/adapt/color_test.c checks:
 *   - the calibration against published values, as ysp/gfx.h v0.3 did:
 *     IEC 61966-2-1's sRGB matrix within 3.9e-5, the CLUT of an sRGB
 *     transfer within 1.3e-3 (17 levels) and 3.6e-5 (64), RGB to LMS, the
 *     DKL matrix and its axes within 7e-8 (relative) of Psychtoolbox's
 *     ComputeDKL_M in MATLAB R2023a; the 10 degree table against CVRL's
 *     rows through colour-science;
 *   - the conversions against colour-science 0.4.7
 *     (tests/compare/color_refs.py): XYZ, xyY, Oklab, Display P3 and Rec.
 *     2020 within 4e-16, CIELAB and CIELUV within 9e-14, the polar forms
 *     within 1.4e-12 (3.3e-10 for the hue of a near-gray), Bradford 1e-16;
 *   - round trips through every space on three contexts, 20000 colors
 *     each: within 1.4e-13 (DKL spherical) and 8.4e-15 for the rest;
 *   - gamut edges, max_scale against a bisection (1.1e-15), the rings,
 *     every map method on 2000 colors (results in gamut, L and h kept
 *     within 3.3e-16 and 2.3e-12 degrees), device codes through the CLUT
 *     and back (0 of 3840 differ), rounding at 0.002 of a code;
 *   - the luminance fit on a synthetic observer (the weights' direction
 *     within 1e-15), every refusal, the parameter tables.
 *   Before ysp/gfx.h v0.5 took its calibration from this header, a build
 *   of the test with ysp/gfx.h v0.4 beside it compared the two codes bit
 *   for bit: 52 calibrations (struct, file bytes, check), 2100 raw calls,
 *   PAINT's matrices and forward conversions, the video primaries; equal.
 *   After the move, ysp_gfx's test gives the same readback hashes.
 *   A calibration from float literals hashes to the same bits on all three
 *   compilers. 32 deliberate faults in the header, 32 caught.
 *   Against Psychtoolbox in MATLAB R2023a (tests/compare/color_ptb.m,
 *   through the MEX binding): ComputeDKL_M within 6.2e-16 with the
 *   standard and a participant's luminance; LMSToMacBoyn within 1.2e-7
 *   (its fitted luminance weights against the published ones);
 *   SensorToPrimary within 1.5e-8 and SensorToSettings's out-of-gamut
 *   flags equal on 500 colors; MaximizeGamutContrast within 7.3e-16.
 *   Cost (examples/color/bench.c, MSVC 19.44 /O2, the Iris Xe laptop,
 *   power source not recorded): a
 *   DKL direction 22 to 27 ns a call, CIELAB and Oklab from RGB 40 to 44
 *   ns, an OkLCh gamut ring of 360 hues 0.8 to 1.0 ms, a context 0.6 to
 *   0.8 ms; MinGW's pow and cbrt are 2 to 5 times slower.
 *   What is NOT done here: no number is a measurement of light.
 *   docs/color.md has the tables.
 */
#ifndef YSP_COLOR_H_INCLUDED
#define YSP_COLOR_H_INCLUDED

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef YCOL_API
#define YCOL_API extern
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define YCOL_VERSION_MAJOR  0
#define YCOL_VERSION_MINOR  2
#define YCOL_VERSION_PATCH  0
#define YCOL_VERSION_STRING "0.2.0"

/* YCOL_VERSION_STRING, as compiled into the implementation. */
YCOL_API const char* ycol_version(void);

/* Return codes. The values equal ysp/gfx.h's for the same meaning. */
#define YCOL_OK            0
#define YCOL_ERR_ARG     (-1)   /* NULL, not finite, a bad enum            */
#define YCOL_ERR_FULL    (-4)   /* a fixed table or the output is full     */
#define YCOL_ERR_REFUSED (-7)   /* the calibration or context cannot do it */
#define YCOL_ERR_FORMAT  (-8)   /* not the canonical form                  */
#define YCOL_ERR_RANGE   (-9)   /* readings or values out of range         */

YCOL_API const char* ycol_strerror(int code);

/* --- calibration ------------------------------------------------------------ */

#define YCOL_CAL_MAX_READINGS 256
#define YCOL_CAL_MAX_WL       471   /* 360 to 830 nm at 1 nm                  */
#define YCOL_CAL_MAX_LUT      4096

#define YCOL_CAL_NOMINAL         0x1u   /* made from stated primaries and a
                                           * gamma, not from a measurement       */
#define YCOL_CAL_HAS_XY          0x2u   /* the readings carry chromaticities   */
#define YCOL_CAL_HAS_SPECTRA     0x4u   /* spd[] holds the primaries' spectra  */
#define YCOL_CAL_SPECTRA_NOMINAL 0x8u   /* the spectra are a datasheet's, not
                                           * measured                            */

#define YCOL_GUN_BLACK (-1)
#define YCOL_GUN_WHITE 3

typedef struct ycol_cal_reading {
    int32_t gun;       /* 0, 1, 2; YCOL_GUN_BLACK (all 0); YCOL_GUN_WHITE    */
    float   level;     /* device value of that gun, 0..1                          */
    float   Y;         /* cd/m2                                                   */
    float   x, y;      /* CIE 1931 chromaticity; 0, 0 = not measured             */
} ycol_cal_reading;

/* A display calibration: the canonical in-memory and on-disk form (62528
 * bytes, little-endian, every offset fixed). Fill the identity, the readings
 * and the spectra, then ycol_cal_derive() fills the rest. */
typedef struct ycol_cal {
    char     magic[8];             /* "YSPCAL\0\1"                               */
    uint32_t version;              /* 1                                          */
    uint32_t bytes;                /* sizeof(ycol_cal)                          */
    uint32_t flags;                /* YCOL_CAL_*                                */
    int32_t  mode_w, mode_h, refresh_num, refresh_den;
    int32_t  n_readings;
    int64_t  date;                 /* seconds since 1970, UTC                     */
    char     display[64];
    char     serial[64];
    char     instrument[64];
    char     screen[256];          /* yscr_describe() at measurement            */
    ycol_cal_reading readings[YCOL_CAL_MAX_READINGS];
    int32_t  n_wl;                 /* spectra samples                             */
    float    wl_start, wl_step;    /* nm                                          */
    int32_t  lut_n;                /* CLUT entries, 2..4096; 0 = 4096             */
    float    spd[4][YCOL_CAL_MAX_WL];  /* R, G, B at full output, and black;
                                    * W sr-1 m-2 nm-1                             */
    /* derived by ycol_cal_derive() */
    double   rgb_to_xyz[9];        /* CIE 1931, row-major, black removed          */
    double   rgb_to_lms[9];        /* Stockman-Sharpe 2 deg, black removed        */
    double   black_xyz[3];
    double   black_lms[3];
    double   white_err;            /* white minus the sum of the guns, percent    */
    float    lut[3][YCOL_CAL_MAX_LUT];   /* linear to device value, per gun     */
    uint32_t reserved;
    uint32_t crc;                  /* CRC-32 of every byte before it              */
} ycol_cal;

/* Zero, magic, version, size. */
YCOL_API void ycol_cal_init(ycol_cal* c);
/* One reading. Unseals the CRC (derive() seals it again). */
YCOL_API int  ycol_cal_add(ycol_cal* c, int gun, float level, float Y, float x, float y);
/* n samples from wl_start every wl_step nm, each gun at full output and the
 * black (NULL = 0). */
YCOL_API int  ycol_cal_set_spectra(ycol_cal* c, float wl_start, float wl_step, int n,
                                       const float* r, const float* g, const float* b, const float* black);
/* The same, flagged YCOL_CAL_SPECTRA_NOMINAL: a datasheet's spectra. */
YCOL_API int  ycol_cal_set_spectra_nominal(ycol_cal* c, float wl_start, float wl_step, int n,
                                               const float* r, const float* g, const float* b,
                                               const float* black);
/* The CLUT, the matrices, the black and the white error from the readings;
 * seals the CRC. YCOL_ERR_RANGE with the reading named for bad readings. */
YCOL_API int  ycol_cal_derive(ycol_cal* c, char* err, size_t cap);
/* The CRC (when set) and the derived fields against the readings. */
YCOL_API int  ycol_cal_check(const ycol_cal* c, char* err, size_t cap);
/* Seals the CRC and copies the bytes to out (NULL: only the size).
 * Returns the size, or YCOL_ERR_FULL when cap is short. */
YCOL_API int  ycol_cal_save(ycol_cal* c, void* out, size_t cap);
/* The bytes of a saved calibration; checked as ycol_cal_check() does. */
YCOL_API int  ycol_cal_load(ycol_cal* c, const void* bytes, size_t n, char* err, size_t cap);
/* From stated chromaticities (R, G, B, white) and a gamma, flagged NOMINAL. */
YCOL_API int  ycol_cal_nominal(ycol_cal* c, const float xy[4][2], float white_Y, double gamma);
/* One line for the log: flags, readings, white error, CRC. */
YCOL_API int  ycol_cal_describe(const ycol_cal* c, char* buf, size_t cap);

/* The raw layer: ysp/gfx.h v0.3's calls, bit for bit. bg, cc, dkl and dir
 * in float as ysp/gfx.h's stimuli hold them. */
YCOL_API int   ycol_cal_lms(const ycol_cal* c, const float rgb[3], double lms[3]);
YCOL_API int   ycol_cal_dir_cone(const ycol_cal* c, const float bg[3], const float cc[3], float dir[3]);
YCOL_API int   ycol_cal_dkl_matrix(const ycol_cal* c, const float bg[3], double m[9]);
YCOL_API int   ycol_cal_dir_dkl(const ycol_cal* c, const float bg[3], const float dkl[3], float dir[3]);
YCOL_API void  ycol_dkl_from_sph(float elevation_deg, float azimuth_deg, float radius, float dkl[3]);
YCOL_API float ycol_max_contrast(const float bg[3], const float dir[3]);

/* --- observers -------------------------------------------------------------- */

typedef enum ycol_cones {
    YCOL_CONES_SS2 = 0,   /* Stockman and Sharpe (2000) 2 degree, CVRL
                             * linss2_10e_1 = CIE 170-1:2006 2 degree          */
    YCOL_CONES_SS10       /* the 10 degree set, CVRL linss10e_1              */
} ycol_cones;

/* Linear energy, peak 1, 390 to 830 nm at 1 nm, interpolated linearly, 0
 * outside; S is 0 above 615 nm, as CVRL gives it. */
YCOL_API void ycol_cone_fundamentals(int cones, double nm, double lms[3]);
/* The standard luminance in cone units: V = w[0] L + w[1] M + w[2] S. */
YCOL_API void ycol_cone_luminance(int cones, double w[3]);

/* --- participant luminance (ISOLUMINANCE) ----------------------------------- */

#define YCOL_LUM_MAX_SETTINGS 16
#define YCOL_LUM_STATED 0x1u   /* weights given, not fitted                   */
#define YCOL_LUM_FIT_S  0x2u   /* fit the S weight too; else it is 0          */

typedef enum ycol_lum_method {
    YCOL_LUM_HFP = 1,          /* heterochromatic flicker photometry          */
    YCOL_LUM_MIN_MOTION,       /* minimum motion (Anstis and Cavanagh)        */
    YCOL_LUM_MIN_BORDER,       /* minimally distinct border                   */
    YCOL_LUM_OTHER
} ycol_lum_method;

typedef struct ycol_lum_setting {
    double  a[3], b[3];          /* the two lights at the null, linear device RGB */
    double  bg[3];               /* the surround while setting                    */
    double  sd;                  /* across repeats, in the adjusted unit; 0 = none */
    int32_t n;                   /* repeats averaged into a and b                 */
    int32_t reserved;
} ycol_lum_setting;            /* 88 bytes                                      */

/* A participant's luminance: the canonical in-memory and on-disk form
 * (1688 bytes, little-endian, every offset fixed, CRC-32). */
typedef struct ycol_lum {
    char     magic[8];           /* "YSPLUM\0\1"                                  */
    uint32_t version;            /* 1                                             */
    uint32_t bytes;              /* sizeof(ycol_lum)                            */
    uint32_t flags;              /* YCOL_LUM_STATED, _FIT_S                     */
    int32_t  cones;              /* the fundamentals the weights are in           */
    int32_t  method;             /* ycol_lum_method                             */
    int32_t  n_settings;
    int64_t  date;               /* seconds since 1970, UTC                       */
    char     participant[64];
    uint32_t cal_crc;            /* the calibration of the settings; derive()
                                  * sets it when 0                                */
    float    field_deg, ecc_deg, freq_hz;   /* the conditions of the settings     */
    ycol_lum_setting settings[YCOL_LUM_MAX_SETTINGS];
    /* derived by ycol_lum_derive() */
    double   w[3];               /* luminance = w . LMS                           */
    double   resid[YCOL_LUM_MAX_SETTINGS];   /* w.(La - Lb) / w.(La + Lb)       */
    uint32_t reserved;
    uint32_t crc;                /* CRC-32 of every byte before it                */
} ycol_lum;

YCOL_API void ycol_lum_init(ycol_lum* l, int cones, int method);
/* One setting: the two lights a and b at the null, the surround bg (may be
 * NULL), the SD and the number of repeats. Unseals the CRC. */
YCOL_API int  ycol_lum_add(ycol_lum* l, const double a[3], const double b[3],
                               const double bg[3], double sd, int n);
/* Fits w from the settings on cal (which must have spectra, and be the
 * settings' calibration); fills resid; seals the CRC. */
YCOL_API int  ycol_lum_derive(ycol_lum* l, const ycol_cal* cal, char* err, size_t cap);
/* Weights from elsewhere, flagged STATED; seals the CRC. */
YCOL_API int  ycol_lum_stated(ycol_lum* l, int cones, const double w[3]);
/* The header and CRC; with cal, also the fit again from the settings. */
YCOL_API int  ycol_lum_check(const ycol_lum* l, const ycol_cal* cal, char* err, size_t cap);
YCOL_API int  ycol_lum_save(ycol_lum* l, void* out, size_t cap);
YCOL_API int  ycol_lum_load(ycol_lum* l, const void* bytes, size_t n, char* err, size_t cap);
YCOL_API int  ycol_lum_describe(const ycol_lum* l, char* buf, size_t cap);

/* --- context ---------------------------------------------------------------- */

typedef enum ycol_adapt {
    YCOL_ADAPT_NONE = 0,       /* colorimetric: D65 stays D65                   */
    YCOL_ADAPT_BRADFORD        /* D65 maps to the display's white (Bradford)    */
} ycol_adapt;

typedef struct ycol_ctx_desc {
    const ycol_cal* cal;       /* required; keep it alive while the ctx is used */
    double  background[3];       /* linear device RGB, 0..1: the zero of the
                                  * contrast spaces and of ycol_gamut.scale     */
    int32_t cones;               /* ycol_cones; 0 = SS2                         */
    int32_t adapt;               /* ycol_adapt; 0 = NONE                        */
    const ycol_lum* lum;       /* NULL = the standard luminance of `cones`      */
    double  white[3];            /* XYZ cd/m2 for CIELAB, CIELUV, Oklab; 0 = the
                                  * display's white (device 1, 1, 1, black in)    */
    double  src_white_Y;         /* cd/m2 of a D65 source's white; 0 = relative to
                                  * the display (D65 SOURCES)                     */
} ycol_ctx_desc;

#define YCOL_CAN_RGB 0x1u      /* always                                        */
#define YCOL_CAN_XYZ 0x2u      /* the calibration has chromaticities            */
#define YCOL_CAN_LMS 0x4u      /* the calibration has spectra                   */
#define YCOL_CAN_BG  0x8u      /* the background's cone excitations are > 0     */

/* Public, read-only after open. ysp/gfx.h reads the matrices for its
 * shaders; every matrix is row-major. */
typedef struct ycol_ctx {
    uint32_t can;                /* YCOL_CAN_*                                  */
    uint32_t id;                 /* CRC-32 of the calibration's and the lum's CRC
                                  * and every desc value                          */
    ycol_ctx_desc desc;        /* as given                                      */
    double bg[3], bg_lms[3];
    double w[3];                 /* the luminance in cone units                   */
    double white_xyz[3];         /* the reference white, absolute                 */
    double rgb_to_xyz[9], xyz_to_rgb[9], black_xyz[3];
    double rgb_to_lms[9], lms_to_rgb[9], black_lms[3];
    double dkl[9], dkl_inv[9];   /* LMS increment to DKL, and back                */
    double rgb_to_dkl[9], dkl_to_rgb[9];   /* RGB increment to DKL, and back     */
    double ok_rgb_to_lms[9], ok_lms_to_rgb[9], ok_black[3];   /* Oklab's linear
                                  * cone response = ok_rgb_to_lms rgb + ok_black  */
    double src_to_rgb[3][9], rgb_to_src[3][9], src_off[3][3];   /* SRGB,
                                  * DISPLAY_P3, REC2020: rgb = M lin + off        */
    double mb_k;                 /* MacLeod-Boynton's s scale                     */
    const char* why_no_xyz;      /* why a level is missing, for the refusals      */
    const char* why_no_lms;
    const char* why_no_bg;
    int32_t status;              /* YCOL_OK, or the code of the last failure of
                                  * ycol_ctx_open() or _set_background()         */
    char err[256];               /* the message: ycol_ctx_error()                 */
} ycol_ctx;

/* Fills cx from d. False only for a bad desc (no calibration, a calibration
 * that is not sealed or fails its CRC, a lum record that fails its CRC or
 * has other cones, a value that is not finite or a background outside
 * 0..1), with the reason in ycol_ctx_error(cx) and the code in cx->status.
 * A calibration without chromaticities or spectra gives a context without
 * those levels (cx->can); the conversions that need them refuse, each call,
 * with the reason. */
YCOL_API bool ycol_ctx_open(ycol_ctx* cx, const ycol_ctx_desc* d);
/* Empties cx. Nothing to release; safe on a zeroed context, after a failed
 * open and twice. */
YCOL_API void ycol_ctx_close(ycol_ctx* cx);
/* The message of the last failure of ycol_ctx_open() or
 * ycol_ctx_set_background(); "" if none. */
YCOL_API const char* ycol_ctx_error(const ycol_ctx* cx);
/* A new background: the contrast spaces and the id follow. Not while
 * another thread reads cx. */
YCOL_API int ycol_ctx_set_background(ycol_ctx* cx, const double bg[3]);
/* One line for the log: calibration, cones, luminance, background, white,
 * D65 sources, levels, id. A reader: returns the code only. */
YCOL_API int ycol_ctx_describe(const ycol_ctx* cx, char* buf, size_t cap);

/* --- color values ----------------------------------------------------------- */

typedef enum ycol_space {
    YCOL_SPACE_NONE = 0,       /* a zeroed color: refused, never black          */
    YCOL_SPACE_RGB,
    YCOL_SPACE_DEVICE,
    YCOL_SPACE_XYZ,
    YCOL_SPACE_XYY,
    YCOL_SPACE_CIELAB,
    YCOL_SPACE_CIELCH,
    YCOL_SPACE_CIELUV,
    YCOL_SPACE_CIELCHUV,
    YCOL_SPACE_OKLAB,
    YCOL_SPACE_OKLCH,
    YCOL_SPACE_SRGB,
    YCOL_SPACE_DISPLAY_P3,
    YCOL_SPACE_REC2020,
    YCOL_SPACE_LMS,
    YCOL_SPACE_CONE,
    YCOL_SPACE_DKL,
    YCOL_SPACE_DKL_CART,
    YCOL_SPACE_MB,
    YCOL_SPACE_COUNT
} ycol_space;

typedef struct ycol_rgb  { double r, g, b; } ycol_rgb;
typedef struct ycol_xyz  { double X, Y, Z; } ycol_xyz;
typedef struct ycol_xyy  { double x, y, Y; } ycol_xyy;
typedef struct ycol_lab  { double L, a, b; } ycol_lab;      /* CIELAB, OKLAB */
typedef struct ycol_luv  { double L, u, v; } ycol_luv;
typedef struct ycol_lch  { double L, C, h; } ycol_lch;      /* h in degrees  */
typedef struct ycol_lms  { double l, m, s; } ycol_lms;      /* LMS, CONE     */
typedef struct ycol_dkl  { double elev, azim, contrast; } ycol_dkl;   /* degrees */
typedef struct ycol_dklc { double lum, lm, s; } ycol_dklc;
typedef struct ycol_mb   { double l, s, lum; } ycol_mb;

#define YCOL_F_ACHROMATIC 0x1u   /* hue or azimuth undefined, given as 0          */
#define YCOL_F_VIA_DEVICE 0x2u   /* from the XYZ family to the cone family, or
                                    * back, through this display's RGB            */

typedef struct ycol_color {
    int32_t  space;              /* ycol_space                                  */
    uint32_t flags;              /* YCOL_F_*, set on results                    */
    union {
        double      v[3];
        ycol_rgb  rgb;         /* RGB, DEVICE, SRGB, DISPLAY_P3, REC2020        */
        ycol_xyz  xyz;
        ycol_xyy  xyy;
        ycol_lab  lab;         /* CIELAB, OKLAB                                 */
        ycol_luv  luv;
        ycol_lch  lch;         /* CIELCH, CIELCHUV, OKLCH                       */
        ycol_lms  lms;         /* LMS, CONE                                     */
        ycol_dkl  dkl;
        ycol_dklc dkl_cart;
        ycol_mb   mb;
    } u;
} ycol_color;                  /* 32 bytes */

/* Values: fields by name or by position, unset fields 0, at least one
 * field. C99 compound literals; in C++20 a functional cast. */
#ifdef __cplusplus
#define YCOL__LIT(T) T
#else
#define YCOL__LIT(T) (T)
#endif
#define YCOL__C(sp, m, ...) YCOL__LIT(ycol_color){ .space = (sp), .u = { .m = { __VA_ARGS__ } } }
#define YCOL_RGB(...)        YCOL__C(YCOL_SPACE_RGB, rgb, __VA_ARGS__)
#define YCOL_DEVICE(...)     YCOL__C(YCOL_SPACE_DEVICE, rgb, __VA_ARGS__)
#define YCOL_XYZ(...)        YCOL__C(YCOL_SPACE_XYZ, xyz, __VA_ARGS__)
#define YCOL_XYY(...)        YCOL__C(YCOL_SPACE_XYY, xyy, __VA_ARGS__)
#define YCOL_CIELAB(...)     YCOL__C(YCOL_SPACE_CIELAB, lab, __VA_ARGS__)
#define YCOL_CIELCH(...)     YCOL__C(YCOL_SPACE_CIELCH, lch, __VA_ARGS__)
#define YCOL_CIELUV(...)     YCOL__C(YCOL_SPACE_CIELUV, luv, __VA_ARGS__)
#define YCOL_CIELCHUV(...)   YCOL__C(YCOL_SPACE_CIELCHUV, lch, __VA_ARGS__)
#define YCOL_OKLAB(...)      YCOL__C(YCOL_SPACE_OKLAB, lab, __VA_ARGS__)
#define YCOL_OKLCH(...)      YCOL__C(YCOL_SPACE_OKLCH, lch, __VA_ARGS__)
#define YCOL_SRGB(...)       YCOL__C(YCOL_SPACE_SRGB, rgb, __VA_ARGS__)
#define YCOL_DISPLAY_P3(...) YCOL__C(YCOL_SPACE_DISPLAY_P3, rgb, __VA_ARGS__)
#define YCOL_REC2020(...)    YCOL__C(YCOL_SPACE_REC2020, rgb, __VA_ARGS__)
#define YCOL_LMS(...)        YCOL__C(YCOL_SPACE_LMS, lms, __VA_ARGS__)
#define YCOL_CONE(...)       YCOL__C(YCOL_SPACE_CONE, lms, __VA_ARGS__)
#define YCOL_DKL(...)        YCOL__C(YCOL_SPACE_DKL, dkl, __VA_ARGS__)
#define YCOL_DKL_CART(...)   YCOL__C(YCOL_SPACE_DKL_CART, dkl_cart, __VA_ARGS__)
#define YCOL_MB(...)         YCOL__C(YCOL_SPACE_MB, mb, __VA_ARGS__)

/* The same value from a space and three numbers (C++17, the bindings). */
YCOL_API ycol_color ycol_make(int space, double a, double b, double c);
/* "#rgb", "#rrggbb", with or without '#': an SRGB color, k / 255 (k / 15
 * for one digit). A string that is not one gives space NONE, which every
 * conversion refuses. */
YCOL_API ycol_color ycol_hex(const char* s);
/* An SRGB color as "#rrggbb" (8 bytes with the 0); YCOL_ERR_RANGE when a
 * value rounds outside 0..255, YCOL_ERR_ARG for another space. */
YCOL_API int ycol_hex_format(ycol_color c, char out[8]);
/* "dkl(0 90 0.1)": the space's name and its values at 17 digits, for logs.
 * Returns the length, or YCOL_ERR_FULL when cap is short. */
YCOL_API int ycol_format(ycol_color c, char* buf, size_t cap);
/* An RGB value into a float[3], as ysp/gfx.h's stimuli hold colors. */
YCOL_API void ycol_store3f(float dst[3], ycol_rgb c);

/* --- conversion ------------------------------------------------------------- */

/* 16 times the float storage's unit round-off 2^-24 (GAMUT): below 1/1000
 * of a 10-bit code. */
#define YCOL_GAMUT_EPS (1.0 / 1048576.0)

typedef struct ycol_gamut {
    int32_t     status;          /* YCOL_OK, or the refusal: then rgb is NaN    */
    int32_t     in;              /* every channel in [-eps, 1 + eps]              */
    uint32_t    below, above;    /* bit k: channel k under 0, over 1              */
    double      margin[3];       /* min(v, 1 - v): room inside, negative outside  */
    double      distance;        /* max(0, -min margin)                           */
    double      scale;           /* largest k with bg + k (rgb - bg) inside;
                                  * HUGE_VAL for rgb = bg                         */
    double      kept;            /* ycol_map(): the fraction of the contrast,
                                  * chroma or isoluminant part kept; NaN for CLIP */
    const char* why;             /* static text when status < 0                   */
} ycol_gamut;

/* A color as linear device RGB, unclipped. g may be NULL. Outside the gamut
 * is not an error: g->status is YCOL_OK and g->in 0. A refusal gives NaN
 * and the reason in g. */
YCOL_API ycol_rgb ycol_to_rgb(const ycol_ctx* cx, ycol_color c, ycol_gamut* g);
/* A modulation direction: the color minus the background, for ysp/gfx.h's
 * stim.dir. For CONE, DKL and DKL_CART it is computed as an increment, not
 * as a difference. g checks bg + dir and bg - dir. */
YCOL_API ycol_rgb ycol_to_dir(const ycol_ctx* cx, ycol_color c, ycol_gamut* g);
/* Linear device RGB as a color in `space`. status (may be NULL) gets the
 * code; a refusal gives space NONE. */
YCOL_API ycol_color ycol_from_rgb(const ycol_ctx* cx, ycol_rgb rgb, int space, int* status);
/* Any space to any space through linear device RGB, which is linear and
 * invertible, so only rounding differs from a direct path. g gets the gamut
 * of the color. */
YCOL_API int ycol_convert(const ycol_ctx* cx, ycol_color in, int space, ycol_color* out,
                              ycol_gamut* g);
/* n triples from space `from` to `to`; strides in doubles (0 = 3). flags,
 * when not NULL, gets a byte per triple: bit 0 in gamut, bit 1 refused (the
 * output triple is then NaN). Returns n, or a code for a bad argument. */
YCOL_API long ycol_convert_n(const ycol_ctx* cx, int from, const double* in, size_t in_stride, int to,
                                 double* out, size_t out_stride, size_t n, uint8_t* flags);

/* --- gamut questions -------------------------------------------------------- */

#define YCOL_ONE_SIDED 0
#define YCOL_SYMMETRIC 1       /* bg + k d and bg - k d both inside             */

/* The largest k >= 0 that keeps k times the color's offset from the
 * background inside the gamut (GAMUT). HUGE_VAL for no offset; the
 * negative code for a refusal. */
YCOL_API double ycol_max_scale(const ycol_ctx* cx, ycol_color c, int sides);

typedef enum ycol_plane {
    YCOL_PLANE_DKL = 1,        /* fixed = elevation; out[i] = the largest DKL
                                  * contrast at azimuth 360 i / n                 */
    YCOL_PLANE_CONE,           /* fixed = elevation toward S; angle from +L
                                  * toward +M; the largest cone contrast           */
    YCOL_PLANE_OKLCH,          /* fixed = L; the largest C at hue 360 i / n      */
    YCOL_PLANE_CIELCH          /* fixed = L*; the largest C*ab                   */
} ycol_plane;

/* The gamut boundary in a plane as n radii. sides applies to DKL and CONE.
 * -1 where a gray of that lightness is outside (OKLCH, CIELCH). */
YCOL_API int ycol_max_ring(const ycol_ctx* cx, int plane, double fixed, int sides, int n, double* out);

typedef enum ycol_map_method {
    YCOL_MAP_SCALE = 1,        /* toward the background in linear RGB           */
    YCOL_MAP_CHROMA_OKLCH,     /* less C at the same L and h in OkLCh           */
    YCOL_MAP_CHROMA_CIELCH,    /* the same in CIE LCh(ab)                        */
    YCOL_MAP_CHROMA_DKL,       /* less of the (L-M, S) part about the
                                  * background, the luminance part kept           */
    YCOL_MAP_CLIP              /* each channel clamped to 0..1                   */
} ycol_map_method;

/* The color brought inside by `method`. g describes the color as given and
 * g->kept the fraction kept. A color in gamut comes back bit for bit.
 * YCOL_ERR_RANGE (NaN) when the method cannot reach the gamut: a gray of
 * that lightness, or the luminance part alone, is outside. */
YCOL_API ycol_rgb ycol_map(const ycol_ctx* cx, ycol_color c, int method, ycol_gamut* g);

/* --- device codes ----------------------------------------------------------- */

/* The code ysp/gfx.h's output stage writes for a scene value with dither
 * NONE: the stored CLUT interpolated in float, floor(v * max + 0.5), max =
 * 2^bits - 1, bits 8 to 16. */
YCOL_API int ycol_output_code(const ycol_cal* cal, ycol_rgb rgb, int bits, uint32_t code[3]);

/* --- standard primaries ----------------------------------------------------- */

/* The values equal ysp/gfx.h's YGFX_PRIM_* and ysp/video.h's YVID_PRIM_*. */
#define YCOL_PRIM_BT709      2   /* = sRGB                                     */
#define YCOL_PRIM_BT601_525  3   /* SMPTE C                                    */
#define YCOL_PRIM_BT601_625  4   /* EBU                                        */
#define YCOL_PRIM_BT2020     5
#define YCOL_PRIM_P3         6   /* Display P3 (D65)                           */

/* The matrix from a D65 source's linear RGB to this display's linear RGB,
 * by the context's src_white_Y and adapt (rgb = m lin + off; off may be
 * NULL in relative mode, where it is 0). */
YCOL_API int ycol_prim_to_rgb(const ycol_ctx* cx, int prim, double m[9], double off[3]);

/* --- isoluminance helper ---------------------------------------------------- */

/* The two lights of a flicker or motion probe: a = bg + d and b = bg - d,
 * d the DKL increment at (elev, azim, contrast). */
YCOL_API int ycol_lum_pair(const ycol_ctx* cx, double azim, double elev, double contrast,
                               double a[3], double b[3]);

/* --- parameter tables ------------------------------------------------------- */

typedef struct ycol_space_info {
    int32_t     space;           /* ycol_space                                  */
    uint32_t    needs;           /* YCOL_CAN_* it needs                         */
    const char* name;            /* "dkl"                                         */
    const char* comp[3];         /* "elev", "azim", "contrast"                    */
    const char* unit[3];         /* "deg", "deg", ""                              */
    double      lo[3], hi[3];    /* the usual range, for a picker                 */
} ycol_space_info;

/* Every space but NONE, in enum order; *n gets YCOL_SPACE_COUNT - 1. */
YCOL_API const ycol_space_info* ycol_spaces(int* n);
/* The space for a name, or YCOL_SPACE_NONE. */
YCOL_API int ycol_space_from_name(const char* name);

typedef struct ycol_param {
    const char* name;            /* the desc field, with an index for arrays      */
    const char* type;            /* "f64", "enum"                                 */
    double      min, max;        /* inclusive                                     */
    double      def;             /* the value a zero field means                  */
    const char* unit;
    const char* doc;
    uint32_t    offset;          /* in ycol_ctx_desc                            */
} ycol_param;

/* The desc fields a designer sets (the pointers are not in it). */
YCOL_API const ycol_param* ycol_desc_params(int* n);

#ifdef __cplusplus
}
#endif

#endif /* YSP_COLOR_H_INCLUDED */

/* ======================================================================= *
 *                            IMPLEMENTATION                               *
 * ======================================================================= */
#ifdef YSP_COLOR_IMPLEMENTATION
#ifndef YSP_COLOR_IMPLEMENTATION_GUARD
#define YSP_COLOR_IMPLEMENTATION_GUARD

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4996)   /* snprintf and vsnprintf are fine here */
#endif

static void ycol__set_error(char* buf, size_t cap, const char* fmt, ...) {
    va_list ap;
    if (!buf || cap == 0) return;
    va_start(ap, fmt);
    vsnprintf(buf, cap, fmt, ap);
    va_end(ap);
}

static uint32_t ycol__crc32(const void* data, size_t n) {
    const unsigned char* p = (const unsigned char*)data;
    uint32_t c = 0xFFFFFFFFu;
    size_t i;
    int k;
    for (i = 0; i < n; i++) {
        c ^= p[i];
        for (k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return c ^ 0xFFFFFFFFu;
}

YCOL_API const char* ycol_version(void) { return YCOL_VERSION_STRING; }

YCOL_API const char* ycol_strerror(int code) {
    switch (code) {
    case YCOL_OK:          return "ok";
    case YCOL_ERR_ARG:     return "bad argument";
    case YCOL_ERR_FULL:    return "full";
    case YCOL_ERR_REFUSED: return "refused: the calibration or context cannot support it";
    case YCOL_ERR_FORMAT:  return "not the canonical form";
    case YCOL_ERR_RANGE:   return "out of range";
    default:                 return "unknown code";
    }
}

/* Stockman and Sharpe (2000) 2-degree cone fundamentals, linear energy, normalized
 * to peak 1: CVRL linss2_10e_1 (cvrl.org), 390 to 830 nm at 1 nm, L, M, S. CVRL
 * leaves S blank above 615 nm; it is 0 here. */
static const float ycol__ss2[441][3] = {
    /* 390 */ {4.15003E-04f, 3.68349E-04f, 9.54729E-03f}, {5.02650E-04f, 4.48015E-04f, 1.14794E-02f}, {6.07367E-04f, 5.43965E-04f, 1.37986E-02f},
    /* 393 */ {7.31850E-04f, 6.58983E-04f, 1.65746E-02f}, {8.79012E-04f, 7.96121E-04f, 1.98869E-02f}, {1.05192E-03f, 9.58658E-04f, 2.38250E-02f},
    /* 396 */ {1.25373E-03f, 1.15002E-03f, 2.84877E-02f}, {1.48756E-03f, 1.37367E-03f, 3.39832E-02f}, {1.75633E-03f, 1.63296E-03f, 4.04274E-02f},
    /* 399 */ {2.06261E-03f, 1.93089E-03f, 4.79417E-02f}, {2.40836E-03f, 2.26991E-03f, 5.66498E-02f}, {2.79522E-03f, 2.65210E-03f, 6.66757E-02f},
    /* 402 */ {3.22640E-03f, 3.08110E-03f, 7.81479E-02f}, {3.70617E-03f, 3.56156E-03f, 9.11925E-02f}, {4.23972E-03f, 4.09900E-03f, 1.05926E-01f},
    /* 405 */ {4.83339E-03f, 4.70010E-03f, 1.22451E-01f}, {5.49335E-03f, 5.37186E-03f, 1.40844E-01f}, {6.21933E-03f, 6.11757E-03f, 1.61140E-01f},
    /* 408 */ {7.00631E-03f, 6.93795E-03f, 1.83325E-01f}, {7.84503E-03f, 7.83144E-03f, 2.07327E-01f}, {8.72127E-03f, 8.79369E-03f, 2.33008E-01f},
    /* 411 */ {9.61879E-03f, 9.81865E-03f, 2.60183E-01f}, {1.05324E-02f, 1.09044E-02f, 2.88723E-01f}, {1.14620E-02f, 1.20509E-02f, 3.18512E-01f},
    /* 414 */ {1.24105E-02f, 1.32582E-02f, 3.49431E-01f}, {1.33837E-02f, 1.45277E-02f, 3.81363E-01f}, {1.43870E-02f, 1.58600E-02f, 4.14141E-01f},
    /* 417 */ {1.54116E-02f, 1.72496E-02f, 4.47350E-01f}, {1.64424E-02f, 1.86878E-02f, 4.80439E-01f}, {1.74614E-02f, 2.01638E-02f, 5.12767E-01f},
    /* 420 */ {1.84480E-02f, 2.16649E-02f, 5.43618E-01f}, {1.93852E-02f, 2.31801E-02f, 5.72399E-01f}, {2.02811E-02f, 2.47139E-02f, 5.99284E-01f},
    /* 423 */ {2.11545E-02f, 2.62785E-02f, 6.24786E-01f}, {2.20286E-02f, 2.78905E-02f, 6.49576E-01f}, {2.29317E-02f, 2.95714E-02f, 6.74474E-01f},
    /* 426 */ {2.38896E-02f, 3.13438E-02f, 7.00186E-01f}, {2.49026E-02f, 3.32151E-02f, 7.26460E-01f}, {2.59631E-02f, 3.51889E-02f, 7.52726E-01f},
    /* 429 */ {2.70619E-02f, 3.72683E-02f, 7.78333E-01f}, {2.81877E-02f, 3.94566E-02f, 8.02555E-01f}, {2.93303E-02f, 4.17546E-02f, 8.24818E-01f},
    /* 432 */ {3.04898E-02f, 4.41544E-02f, 8.45422E-01f}, {3.16694E-02f, 4.66432E-02f, 8.64961E-01f}, {3.28731E-02f, 4.92050E-02f, 8.84100E-01f},
    /* 435 */ {3.41054E-02f, 5.18199E-02f, 9.03573E-01f}, {3.53670E-02f, 5.44645E-02f, 9.23844E-01f}, {3.66400E-02f, 5.71131E-02f, 9.44055E-01f},
    /* 438 */ {3.78988E-02f, 5.97369E-02f, 9.62920E-01f}, {3.91148E-02f, 6.23038E-02f, 9.79057E-01f}, {4.02563E-02f, 6.47782E-02f, 9.91020E-01f},
    /* 441 */ {4.12989E-02f, 6.71329E-02f, 9.97765E-01f}, {4.22582E-02f, 6.93844E-02f, 9.99982E-01f}, {4.31627E-02f, 7.15658E-02f, 9.98861E-01f},
    /* 444 */ {4.40444E-02f, 7.37164E-02f, 9.95628E-01f}, {4.49380E-02f, 7.58812E-02f, 9.91515E-01f}, {4.58729E-02f, 7.80980E-02f, 9.87377E-01f},
    /* 447 */ {4.68459E-02f, 8.03538E-02f, 9.82619E-01f}, {4.78446E-02f, 8.26198E-02f, 9.76301E-01f}, {4.88555E-02f, 8.48644E-02f, 9.67513E-01f},
    /* 450 */ {4.98639E-02f, 8.70524E-02f, 9.55393E-01f}, {5.08618E-02f, 8.91640E-02f, 9.39499E-01f}, {5.18731E-02f, 9.12523E-02f, 9.20807E-01f},
    /* 453 */ {5.29317E-02f, 9.33950E-02f, 9.00592E-01f}, {5.40746E-02f, 9.56775E-02f, 8.80040E-01f}, {5.53418E-02f, 9.81934E-02f, 8.60240E-01f},
    /* 456 */ {5.67734E-02f, 1.01031E-01f, 8.42031E-01f}, {5.83973E-02f, 1.04229E-01f, 8.25572E-01f}, {6.02409E-02f, 1.07814E-01f, 8.10860E-01f},
    /* 459 */ {6.23353E-02f, 1.11817E-01f, 7.97902E-01f}, {6.47164E-02f, 1.16272E-01f, 7.86704E-01f}, {6.74131E-02f, 1.21204E-01f, 7.77119E-01f},
    /* 462 */ {7.04040E-02f, 1.26573E-01f, 7.68365E-01f}, {7.36489E-02f, 1.32311E-01f, 7.59538E-01f}, {7.70978E-02f, 1.38334E-01f, 7.49777E-01f},
    /* 465 */ {8.06894E-02f, 1.44541E-01f, 7.38268E-01f}, {8.43613E-02f, 1.50828E-01f, 7.24389E-01f}, {8.80904E-02f, 1.57146E-01f, 7.08113E-01f},
    /* 468 */ {9.18630E-02f, 1.63457E-01f, 6.89572E-01f}, {9.56637E-02f, 1.69721E-01f, 6.68927E-01f}, {9.94755E-02f, 1.75893E-01f, 6.46359E-01f},
    /* 471 */ {1.03286E-01f, 1.81938E-01f, 6.22112E-01f}, {1.07103E-01f, 1.87872E-01f, 5.96591E-01f}, {1.10947E-01f, 1.93731E-01f, 5.70216E-01f},
    /* 474 */ {1.14838E-01f, 1.99557E-01f, 5.43373E-01f}, {1.18802E-01f, 2.05398E-01f, 5.16411E-01f}, {1.22863E-01f, 2.11302E-01f, 4.89647E-01f},
    /* 477 */ {1.27026E-01f, 2.17283E-01f, 4.63406E-01f}, {1.31293E-01f, 2.23347E-01f, 4.37965E-01f}, {1.35666E-01f, 2.29501E-01f, 4.13549E-01f},
    /* 480 */ {1.40145E-01f, 2.35754E-01f, 3.90333E-01f}, {1.44731E-01f, 2.42108E-01f, 3.68394E-01f}, {1.49415E-01f, 2.48545E-01f, 3.47582E-01f},
    /* 483 */ {1.54189E-01f, 2.55037E-01f, 3.27724E-01f}, {1.59038E-01f, 2.61554E-01f, 3.08676E-01f}, {1.63952E-01f, 2.68063E-01f, 2.90322E-01f},
    /* 486 */ {1.68932E-01f, 2.74561E-01f, 2.72626E-01f}, {1.74063E-01f, 2.81180E-01f, 2.55772E-01f}, {1.79457E-01f, 2.88097E-01f, 2.39945E-01f},
    /* 489 */ {1.85241E-01f, 2.95508E-01f, 2.25280E-01f}, {1.91556E-01f, 3.03630E-01f, 2.11867E-01f}, {1.98532E-01f, 3.12649E-01f, 1.99719E-01f},
    /* 492 */ {2.06182E-01f, 3.22565E-01f, 1.88674E-01f}, {2.14485E-01f, 3.33319E-01f, 1.78557E-01f}, {2.23411E-01f, 3.44844E-01f, 1.69217E-01f},
    /* 495 */ {2.32926E-01f, 3.57061E-01f, 1.60526E-01f}, {2.42992E-01f, 3.69895E-01f, 1.52368E-01f}, {2.53616E-01f, 3.83355E-01f, 1.44620E-01f},
    /* 498 */ {2.64810E-01f, 3.97467E-01f, 1.37173E-01f}, {2.76587E-01f, 4.12260E-01f, 1.29937E-01f}, {2.88959E-01f, 4.27764E-01f, 1.22839E-01f},
    /* 501 */ {3.01934E-01f, 4.44001E-01f, 1.15834E-01f}, {3.15508E-01f, 4.60947E-01f, 1.08922E-01f}, {3.29673E-01f, 4.78562E-01f, 1.02118E-01f},
    /* 504 */ {3.44416E-01f, 4.96795E-01f, 9.54374E-02f}, {3.59716E-01f, 5.15587E-01f, 8.88965E-02f}, {3.75550E-01f, 5.34868E-01f, 8.25343E-02f},
    /* 507 */ {3.91895E-01f, 5.54575E-01f, 7.64613E-02f}, {4.08722E-01f, 5.74640E-01f, 7.07769E-02f}, {4.25998E-01f, 5.94984E-01f, 6.55491E-02f},
    /* 510 */ {4.43683E-01f, 6.15520E-01f, 6.08210E-02f}, {4.61731E-01f, 6.36162E-01f, 5.65919E-02f}, {4.80094E-01f, 6.56872E-01f, 5.27659E-02f},
    /* 513 */ {4.98717E-01f, 6.77624E-01f, 4.92440E-02f}, {5.17539E-01f, 6.98393E-01f, 4.59470E-02f}, {5.36494E-01f, 7.19154E-01f, 4.28123E-02f},
    /* 516 */ {5.55493E-01f, 7.39844E-01f, 3.98014E-02f}, {5.74386E-01f, 7.60235E-01f, 3.69227E-02f}, {5.92995E-01f, 7.80039E-01f, 3.41909E-02f},
    /* 519 */ {6.11124E-01f, 7.98941E-01f, 3.16158E-02f}, {6.28561E-01f, 8.16610E-01f, 2.92033E-02f}, {6.45139E-01f, 8.32783E-01f, 2.69536E-02f},
    /* 522 */ {6.60907E-01f, 8.47550E-01f, 2.48575E-02f}, {6.75993E-01f, 8.61113E-01f, 2.29047E-02f}, {6.90542E-01f, 8.73698E-01f, 2.10855E-02f},
    /* 525 */ {7.04720E-01f, 8.85550E-01f, 1.93912E-02f}, {7.18662E-01f, 8.96873E-01f, 1.78145E-02f}, {7.32323E-01f, 9.07638E-01f, 1.63514E-02f},
    /* 528 */ {7.45606E-01f, 9.17757E-01f, 1.49980E-02f}, {7.58410E-01f, 9.27137E-01f, 1.37497E-02f}, {7.70630E-01f, 9.35687E-01f, 1.26013E-02f},
    /* 531 */ {7.82208E-01f, 9.43362E-01f, 1.15466E-02f}, {7.93290E-01f, 9.50309E-01f, 1.05766E-02f}, {8.04084E-01f, 9.56733E-01f, 9.68268E-03f},
    /* 534 */ {8.14813E-01f, 9.62844E-01f, 8.85746E-03f}, {8.25711E-01f, 9.68858E-01f, 8.09453E-03f}, {8.36943E-01f, 9.74919E-01f, 7.38890E-03f},
    /* 537 */ {8.48349E-01f, 9.80850E-01f, 6.73799E-03f}, {8.59676E-01f, 9.86388E-01f, 6.13947E-03f}, {8.70656E-01f, 9.91267E-01f, 5.59073E-03f},
    /* 540 */ {8.81011E-01f, 9.95217E-01f, 5.08900E-03f}, {8.90496E-01f, 9.98007E-01f, 4.63120E-03f}, {8.99052E-01f, 9.99595E-01f, 4.21366E-03f},
    /* 543 */ {9.06669E-01f, 9.99982E-01f, 3.83286E-03f}, {9.13341E-01f, 9.99177E-01f, 3.48559E-03f}, {9.19067E-01f, 9.97193E-01f, 3.16893E-03f},
    /* 546 */ {9.23898E-01f, 9.94102E-01f, 2.88021E-03f}, {9.28099E-01f, 9.90204E-01f, 2.61698E-03f}, {9.31995E-01f, 9.85855E-01f, 2.37701E-03f},
    /* 549 */ {9.35916E-01f, 9.81404E-01f, 2.15829E-03f}, {9.40198E-01f, 9.77193E-01f, 1.95896E-03f}, {9.45076E-01f, 9.73441E-01f, 1.77737E-03f},
    /* 552 */ {9.50367E-01f, 9.69881E-01f, 1.61218E-03f}, {9.55775E-01f, 9.66133E-01f, 1.46215E-03f}, {9.61000E-01f, 9.61823E-01f, 1.32606E-03f},
    /* 555 */ {9.65733E-01f, 9.56583E-01f, 1.20277E-03f}, {9.69744E-01f, 9.50167E-01f, 1.09118E-03f}, {9.73133E-01f, 9.42773E-01f, 9.90135E-04f},
    /* 558 */ {9.76086E-01f, 9.34709E-01f, 8.98564E-04f}, {9.78793E-01f, 9.26271E-01f, 8.15524E-04f}, {9.81445E-01f, 9.17750E-01f, 7.40174E-04f},
    /* 561 */ {9.84186E-01f, 9.09339E-01f, 6.71772E-04f}, {9.86965E-01f, 9.00895E-01f, 6.09693E-04f}, {9.89678E-01f, 8.92197E-01f, 5.53372E-04f},
    /* 564 */ {9.92220E-01f, 8.83035E-01f, 5.02292E-04f}, {9.94486E-01f, 8.73205E-01f, 4.55979E-04f}, {9.96386E-01f, 8.62565E-01f, 4.13997E-04f},
    /* 567 */ {9.97895E-01f, 8.51173E-01f, 3.75940E-04f}, {9.99004E-01f, 8.39132E-01f, 3.41439E-04f}, {9.99706E-01f, 8.26545E-01f, 3.10160E-04f},
    /* 570 */ {9.99993E-01f, 8.13509E-01f, 2.81800E-04f}, {9.99837E-01f, 8.00082E-01f, 2.56084E-04f}, {9.99123E-01f, 7.86166E-01f, 2.32764E-04f},
    /* 573 */ {9.97719E-01f, 7.71635E-01f, 2.11616E-04f}, {9.95491E-01f, 7.56376E-01f, 1.92436E-04f}, {9.92310E-01f, 7.40291E-01f, 1.75039E-04f},
    /* 576 */ {9.88146E-01f, 7.23369E-01f, 1.59257E-04f}, {9.83345E-01f, 7.05890E-01f, 1.44940E-04f}, {9.78342E-01f, 6.88184E-01f, 1.31948E-04f},
    /* 579 */ {9.73564E-01f, 6.70554E-01f, 1.20157E-04f}, {9.69429E-01f, 6.53274E-01f, 1.09454E-04f}, {9.66211E-01f, 6.36524E-01f, 9.97367E-05f},
    /* 582 */ {9.63630E-01f, 6.20217E-01f, 9.09125E-05f}, {9.61273E-01f, 6.04211E-01f, 8.28976E-05f}, {9.58732E-01f, 5.88375E-01f, 7.56160E-05f},
    /* 585 */ {9.55602E-01f, 5.72597E-01f, 6.89991E-05f}, {9.51569E-01f, 5.56783E-01f, 6.29846E-05f}, {9.46654E-01f, 5.40896E-01f, 5.75163E-05f},
    /* 588 */ {9.40962E-01f, 5.24912E-01f, 5.25432E-05f}, {9.34600E-01f, 5.08817E-01f, 4.80192E-05f}, {9.27673E-01f, 4.92599E-01f, 4.39024E-05f},
    /* 591 */ {9.20264E-01f, 4.76268E-01f, 4.01551E-05f}, {9.12391E-01f, 4.59893E-01f, 3.67431E-05f}, {9.04050E-01f, 4.43551E-01f, 3.36353E-05f},
    /* 594 */ {8.95243E-01f, 4.27314E-01f, 3.08037E-05f}, {8.85969E-01f, 4.11246E-01f, 2.82228E-05f}, {8.76242E-01f, 3.95396E-01f, 2.58697E-05f},
    /* 597 */ {8.66117E-01f, 3.79782E-01f, 2.37235E-05f}, {8.55658E-01f, 3.64410E-01f, 2.17653E-05f}, {8.44926E-01f, 3.49289E-01f, 1.99779E-05f},
    /* 600 */ {8.33982E-01f, 3.34429E-01f, 1.83459E-05f}, {8.22859E-01f, 3.19843E-01f, 1.68551E-05f}, {8.11491E-01f, 3.05564E-01f, 1.54929E-05f},
    /* 603 */ {7.99794E-01f, 2.91625E-01f, 1.42476E-05f}, {7.87689E-01f, 2.78053E-01f, 1.31087E-05f}, {7.75103E-01f, 2.64872E-01f, 1.20667E-05f},
    /* 606 */ {7.61996E-01f, 2.52099E-01f, 1.11131E-05f}, {7.48425E-01f, 2.39747E-01f, 1.02399E-05f}, {7.34470E-01f, 2.27822E-01f, 9.43999E-06f},
    /* 609 */ {7.20208E-01f, 2.16330E-01f, 8.70695E-06f}, {7.05713E-01f, 2.05273E-01f, 8.03488E-06f}, {6.91044E-01f, 1.94650E-01f, 7.41844E-06f},
    /* 612 */ {6.76212E-01f, 1.84448E-01f, 6.85279E-06f}, {6.61220E-01f, 1.74654E-01f, 6.33352E-06f}, {6.46072E-01f, 1.65256E-01f, 5.85662E-06f},
    /* 615 */ {6.30773E-01f, 1.56243E-01f, 5.41843E-06f}, {6.15349E-01f, 1.47602E-01f, 0.0f}, {5.99888E-01f, 1.39329E-01f, 0.0f},
    /* 618 */ {5.84489E-01f, 1.31416E-01f, 0.0f}, {5.69240E-01f, 1.23856E-01f, 0.0f}, {5.54224E-01f, 1.16641E-01f, 0.0f},
    /* 621 */ {5.39469E-01f, 1.09766E-01f, 0.0f}, {5.24827E-01f, 1.03226E-01f, 0.0f}, {5.10124E-01f, 9.70205E-02f, 0.0f},
    /* 624 */ {4.95206E-01f, 9.11430E-02f, 0.0f}, {4.79941E-01f, 8.55872E-02f, 0.0f}, {4.64270E-01f, 8.03428E-02f, 0.0f},
    /* 627 */ {4.48338E-01f, 7.53913E-02f, 0.0f}, {4.32329E-01f, 7.07136E-02f, 0.0f}, {4.16406E-01f, 6.62924E-02f, 0.0f},
    /* 630 */ {4.00711E-01f, 6.21120E-02f, 0.0f}, {3.85355E-01f, 5.81595E-02f, 0.0f}, {3.70377E-01f, 5.44276E-02f, 0.0f},
    /* 633 */ {3.55793E-01f, 5.09098E-02f, 0.0f}, {3.41618E-01f, 4.75991E-02f, 0.0f}, {3.27864E-01f, 4.44879E-02f, 0.0f},
    /* 636 */ {3.14541E-01f, 4.15659E-02f, 0.0f}, {3.01662E-01f, 3.88152E-02f, 0.0f}, {2.89239E-01f, 3.62181E-02f, 0.0f},
    /* 639 */ {2.77278E-01f, 3.37599E-02f, 0.0f}, {2.65784E-01f, 3.14282E-02f, 0.0f}, {2.54740E-01f, 2.92176E-02f, 0.0f},
    /* 642 */ {2.44054E-01f, 2.71403E-02f, 0.0f}, {2.33634E-01f, 2.52084E-02f, 0.0f}, {2.23399E-01f, 2.34286E-02f, 0.0f},
    /* 645 */ {2.13284E-01f, 2.18037E-02f, 0.0f}, {2.03257E-01f, 2.03285E-02f, 0.0f}, {1.93370E-01f, 1.89779E-02f, 0.0f},
    /* 648 */ {1.83688E-01f, 1.77272E-02f, 0.0f}, {1.74263E-01f, 1.65560E-02f, 0.0f}, {1.65141E-01f, 1.54480E-02f, 0.0f},
    /* 651 */ {1.56354E-01f, 1.43924E-02f, 0.0f}, {1.47916E-01f, 1.33896E-02f, 0.0f}, {1.39834E-01f, 1.24414E-02f, 0.0f},
    /* 654 */ {1.32111E-01f, 1.15488E-02f, 0.0f}, {1.24749E-01f, 1.07120E-02f, 0.0f}, {1.17744E-01f, 9.92994E-03f, 0.0f},
    /* 657 */ {1.11081E-01f, 9.20057E-03f, 0.0f}, {1.04747E-01f, 8.52126E-03f, 0.0f}, {9.87277E-02f, 7.88945E-03f, 0.0f},
    /* 660 */ {9.30085E-02f, 7.30255E-03f, 0.0f}, {8.75769E-02f, 6.75820E-03f, 0.0f}, {8.24219E-02f, 6.25474E-03f, 0.0f},
    /* 663 */ {7.75328E-02f, 5.79045E-03f, 0.0f}, {7.28989E-02f, 5.36347E-03f, 0.0f}, {6.85100E-02f, 4.97179E-03f, 0.0f},
    /* 666 */ {6.43554E-02f, 4.61300E-03f, 0.0f}, {6.04242E-02f, 4.28339E-03f, 0.0f}, {5.67057E-02f, 3.97940E-03f, 0.0f},
    /* 669 */ {5.31896E-02f, 3.69803E-03f, 0.0f}, {4.98661E-02f, 3.43667E-03f, 0.0f}, {4.67258E-02f, 3.19326E-03f, 0.0f},
    /* 672 */ {4.37598E-02f, 2.96653E-03f, 0.0f}, {4.09594E-02f, 2.75543E-03f, 0.0f}, {3.83165E-02f, 2.55896E-03f, 0.0f},
    /* 675 */ {3.58233E-02f, 2.37617E-03f, 0.0f}, {3.34725E-02f, 2.20618E-03f, 0.0f}, {3.12583E-02f, 2.04809E-03f, 0.0f},
    /* 678 */ {2.91751E-02f, 1.90110E-03f, 0.0f}, {2.72173E-02f, 1.76442E-03f, 0.0f}, {2.53790E-02f, 1.63734E-03f, 0.0f},
    /* 681 */ {2.36541E-02f, 1.51916E-03f, 0.0f}, {2.20336E-02f, 1.40913E-03f, 0.0f}, {2.05092E-02f, 1.30655E-03f, 0.0f},
    /* 684 */ {1.90735E-02f, 1.21078E-03f, 0.0f}, {1.77201E-02f, 1.12128E-03f, 0.0f}, {1.64451E-02f, 1.03766E-03f, 0.0f},
    /* 687 */ {1.52510E-02f, 9.59892E-04f, 0.0f}, {1.41404E-02f, 8.87951E-04f, 0.0f}, {1.31137E-02f, 8.21729E-04f, 0.0f},
    /* 690 */ {1.21701E-02f, 7.61051E-04f, 0.0f}, {1.13063E-02f, 7.05623E-04f, 0.0f}, {1.05131E-02f, 6.54874E-04f, 0.0f},
    /* 693 */ {9.78127E-03f, 6.08240E-04f, 0.0f}, {9.10300E-03f, 5.65240E-04f, 0.0f}, {8.47170E-03f, 5.25457E-04f, 0.0f},
    /* 696 */ {7.88227E-03f, 4.88550E-04f, 0.0f}, {7.33213E-03f, 4.54277E-04f, 0.0f}, {6.81928E-03f, 4.22436E-04f, 0.0f},
    /* 699 */ {6.34173E-03f, 3.92839E-04f, 0.0f}, {5.89749E-03f, 3.65317E-04f, 0.0f}, {5.48444E-03f, 3.39710E-04f, 0.0f},
    /* 702 */ {5.09978E-03f, 3.15856E-04f, 0.0f}, {4.74086E-03f, 2.93607E-04f, 0.0f}, {4.40538E-03f, 2.72834E-04f, 0.0f},
    /* 705 */ {4.09129E-03f, 2.53417E-04f, 0.0f}, {3.79703E-03f, 2.35265E-04f, 0.0f}, {3.52187E-03f, 2.18331E-04f, 0.0f},
    /* 708 */ {3.26520E-03f, 2.02574E-04f, 0.0f}, {3.02632E-03f, 1.87948E-04f, 0.0f}, {2.80447E-03f, 1.74402E-04f, 0.0f},
    /* 711 */ {2.59882E-03f, 1.61878E-04f, 0.0f}, {2.40849E-03f, 1.50305E-04f, 0.0f}, {2.23259E-03f, 1.39612E-04f, 0.0f},
    /* 714 */ {2.07024E-03f, 1.29734E-04f, 0.0f}, {1.92058E-03f, 1.20608E-04f, 0.0f}, {1.78269E-03f, 1.12176E-04f, 0.0f},
    /* 717 */ {1.65540E-03f, 1.04373E-04f, 0.0f}, {1.53762E-03f, 9.71383E-05f, 0.0f}, {1.42839E-03f, 9.04202E-05f, 0.0f},
    /* 720 */ {1.32687E-03f, 8.41716E-05f, 0.0f}, {1.23238E-03f, 7.83538E-05f, 0.0f}, {1.14456E-03f, 7.29425E-05f, 0.0f},
    /* 723 */ {1.06308E-03f, 6.79164E-05f, 0.0f}, {9.87592E-04f, 6.32542E-05f, 0.0f}, {9.17777E-04f, 5.89349E-05f, 0.0f},
    /* 726 */ {8.53264E-04f, 5.49360E-05f, 0.0f}, {7.93589E-04f, 5.12291E-05f, 0.0f}, {7.38306E-04f, 4.77872E-05f, 0.0f},
    /* 729 */ {6.87018E-04f, 4.45862E-05f, 0.0f}, {6.39373E-04f, 4.16049E-05f, 0.0f}, {5.95057E-04f, 3.88245E-05f, 0.0f},
    /* 732 */ {5.53797E-04f, 3.62302E-05f, 0.0f}, {5.15350E-04f, 3.38086E-05f, 0.0f}, {4.79496E-04f, 3.15474E-05f, 0.0f},
    /* 735 */ {4.46035E-04f, 2.94354E-05f, 0.0f}, {4.14809E-04f, 2.74634E-05f, 0.0f}, {3.85749E-04f, 2.56268E-05f, 0.0f},
    /* 738 */ {3.58792E-04f, 2.39217E-05f, 0.0f}, {3.33861E-04f, 2.23432E-05f, 0.0f}, {3.10869E-04f, 2.08860E-05f, 0.0f},
    /* 741 */ {2.89699E-04f, 1.95424E-05f, 0.0f}, {2.70145E-04f, 1.82990E-05f, 0.0f}, {2.52007E-04f, 1.71423E-05f, 0.0f},
    /* 744 */ {2.35117E-04f, 1.60611E-05f, 0.0f}, {2.19329E-04f, 1.50458E-05f, 0.0f}, {2.04535E-04f, 1.40893E-05f, 0.0f},
    /* 747 */ {1.90692E-04f, 1.31899E-05f, 0.0f}, {1.77771E-04f, 1.23462E-05f, 0.0f}, {1.65736E-04f, 1.15569E-05f, 0.0f},
    /* 750 */ {1.54549E-04f, 1.08200E-05f, 0.0f}, {1.44167E-04f, 1.01334E-05f, 0.0f}, {1.34528E-04f, 9.49367E-06f, 0.0f},
    /* 753 */ {1.25574E-04f, 8.89736E-06f, 0.0f}, {1.17251E-04f, 8.34135E-06f, 0.0f}, {1.09508E-04f, 7.82271E-06f, 0.0f},
    /* 756 */ {1.02300E-04f, 7.33865E-06f, 0.0f}, {9.55828E-05f, 6.88612E-06f, 0.0f}, {8.93161E-05f, 6.46228E-06f, 0.0f},
    /* 759 */ {8.34631E-05f, 6.06462E-06f, 0.0f}, {7.79912E-05f, 5.69093E-06f, 0.0f}, {7.28730E-05f, 5.33942E-06f, 0.0f},
    /* 762 */ {6.80921E-05f, 5.00929E-06f, 0.0f}, {6.36342E-05f, 4.69984E-06f, 0.0f}, {5.94841E-05f, 4.41034E-06f, 0.0f},
    /* 765 */ {5.56264E-05f, 4.13998E-06f, 0.0f}, {5.20430E-05f, 3.88772E-06f, 0.0f}, {4.87063E-05f, 3.65186E-06f, 0.0f},
    /* 768 */ {4.55900E-05f, 3.43070E-06f, 0.0f}, {4.26710E-05f, 3.22278E-06f, 0.0f}, {3.99295E-05f, 3.02683E-06f, 0.0f},
    /* 771 */ {3.73509E-05f, 2.84192E-06f, 0.0f}, {3.49326E-05f, 2.66795E-06f, 0.0f}, {3.26730E-05f, 2.50491E-06f, 0.0f},
    /* 774 */ {3.05690E-05f, 2.35267E-06f, 0.0f}, {2.86163E-05f, 2.21100E-06f, 0.0f}, {2.68077E-05f, 2.07946E-06f, 0.0f},
    /* 777 */ {2.51286E-05f, 1.95700E-06f, 0.0f}, {2.35645E-05f, 1.84258E-06f, 0.0f}, {2.21026E-05f, 1.73528E-06f, 0.0f},
    /* 780 */ {2.07321E-05f, 1.63433E-06f, 0.0f}, {1.94444E-05f, 1.53910E-06f, 0.0f}, {1.82351E-05f, 1.44932E-06f, 0.0f},
    /* 783 */ {1.71008E-05f, 1.36478E-06f, 0.0f}, {1.60380E-05f, 1.28526E-06f, 0.0f}, {1.50432E-05f, 1.21054E-06f, 0.0f},
    /* 786 */ {1.41127E-05f, 1.14038E-06f, 0.0f}, {1.32420E-05f, 1.07446E-06f, 0.0f}, {1.24263E-05f, 1.01247E-06f, 0.0f},
    /* 789 */ {1.16618E-05f, 9.54130E-07f, 0.0f}, {1.09446E-05f, 8.99170E-07f, 0.0f}, {1.02716E-05f, 8.47379E-07f, 0.0f},
    /* 792 */ {9.64036E-06f, 7.98635E-07f, 0.0f}, {9.04897E-06f, 7.52832E-07f, 0.0f}, {8.49535E-06f, 7.09857E-07f, 0.0f},
    /* 795 */ {7.97750E-06f, 6.69594E-07f, 0.0f}, {7.49341E-06f, 6.31907E-07f, 0.0f}, {7.04079E-06f, 5.96604E-07f, 0.0f},
    /* 798 */ {6.61744E-06f, 5.63495E-07f, 0.0f}, {6.22133E-06f, 5.32408E-07f, 0.0f}, {5.85057E-06f, 5.03187E-07f, 0.0f},
    /* 801 */ {5.50333E-06f, 4.75686E-07f, 0.0f}, {5.17756E-06f, 4.49753E-07f, 0.0f}, {4.87135E-06f, 4.25249E-07f, 0.0f},
    /* 804 */ {4.58300E-06f, 4.02050E-07f, 0.0f}, {4.31102E-06f, 3.80046E-07f, 0.0f}, {4.05422E-06f, 3.59156E-07f, 0.0f},
    /* 807 */ {3.81213E-06f, 3.39356E-07f, 0.0f}, {3.58438E-06f, 3.20633E-07f, 0.0f}, {3.37053E-06f, 3.02966E-07f, 0.0f},
    /* 810 */ {3.17009E-06f, 2.86329E-07f, 0.0f}, {2.98248E-06f, 2.70687E-07f, 0.0f}, {2.80691E-06f, 2.55980E-07f, 0.0f},
    /* 813 */ {2.64257E-06f, 2.42147E-07f, 0.0f}, {2.48873E-06f, 2.29130E-07f, 0.0f}, {2.34468E-06f, 2.16878E-07f, 0.0f},
    /* 816 */ {2.20974E-06f, 2.05338E-07f, 0.0f}, {2.08315E-06f, 1.94449E-07f, 0.0f}, {1.96419E-06f, 1.84155E-07f, 0.0f},
    /* 819 */ {1.85222E-06f, 1.74407E-07f, 0.0f}, {1.74666E-06f, 1.65158E-07f, 0.0f}, {1.64705E-06f, 1.56373E-07f, 0.0f},
    /* 822 */ {1.55307E-06f, 1.48031E-07f, 0.0f}, {1.46448E-06f, 1.40117E-07f, 0.0f}, {1.38100E-06f, 1.32615E-07f, 0.0f},
    /* 825 */ {1.30241E-06f, 1.25508E-07f, 0.0f}, {1.22844E-06f, 1.18781E-07f, 0.0f}, {1.15888E-06f, 1.12416E-07f, 0.0f},
    /* 828 */ {1.09348E-06f, 1.06398E-07f, 0.0f}, {1.03203E-06f, 1.00711E-07f, 0.0f}, {9.74306E-07f, 9.53411E-08f, 0.0f},
};

/* The 10-degree set: CVRL linss10e_1 (cvrl.org) as colour-science 0.4.7 gives it,
 * 390 to 830 nm at 1 nm, L, M, S; S below 1e-15 is 0, as for the 2-degree set. */
static const float ycol__ss10[441][3] = {
    /* 390 */ {4.07619E-04f, 3.58227E-04f, 6.14265E-03f}, {4.97068E-04f, 4.38660E-04f, 7.44280E-03f}, {6.04713E-04f, 5.36230E-04f, 9.01661E-03f},
    /* 393 */ {7.33640E-04f, 6.54061E-04f, 1.09170E-02f}, {8.87247E-04f, 7.95649E-04f, 1.32053E-02f}, {1.06921E-03f, 9.64828E-04f, 1.59515E-02f},
    /* 396 */ {1.28340E-03f, 1.16572E-03f, 1.92347E-02f}, {1.53382E-03f, 1.40263E-03f, 2.31436E-02f}, {1.82443E-03f, 1.67992E-03f, 2.77750E-02f},
    /* 399 */ {2.15896E-03f, 2.00180E-03f, 3.32339E-02f}, {2.54073E-03f, 2.37208E-03f, 3.96308E-02f}, {2.97282E-03f, 2.79433E-03f, 4.70801E-02f},
    /* 402 */ {3.45993E-03f, 3.27374E-03f, 5.57012E-02f}, {4.00793E-03f, 3.81660E-03f, 6.56137E-02f}, {4.62370E-03f, 4.43021E-03f, 7.69323E-02f},
    /* 405 */ {5.31546E-03f, 5.12316E-03f, 8.97612E-02f}, {6.09138E-03f, 5.90458E-03f, 1.04188E-01f}, {6.95291E-03f, 6.78005E-03f, 1.20273E-01f},
    /* 408 */ {7.89634E-03f, 7.75260E-03f, 1.38044E-01f}, {8.91300E-03f, 8.82286E-03f, 1.57485E-01f}, {9.98835E-03f, 9.98841E-03f, 1.78530E-01f},
    /* 411 */ {1.11054E-02f, 1.12452E-02f, 2.01077E-01f}, {1.22607E-02f, 1.25949E-02f, 2.25091E-01f}, {1.34578E-02f, 1.40425E-02f, 2.50566E-01f},
    /* 414 */ {1.47044E-02f, 1.55944E-02f, 2.77507E-01f}, {1.60130E-02f, 1.72596E-02f, 3.05941E-01f}, {1.73958E-02f, 1.90467E-02f, 3.35858E-01f},
    /* 417 */ {1.88452E-02f, 2.09545E-02f, 3.66981E-01f}, {2.03440E-02f, 2.29763E-02f, 3.98876E-01f}, {2.18700E-02f, 2.51017E-02f, 4.30998E-01f},
    /* 420 */ {2.33957E-02f, 2.73163E-02f, 4.62692E-01f}, {2.48961E-02f, 2.96062E-02f, 4.93357E-01f}, {2.63761E-02f, 3.19746E-02f, 5.23006E-01f},
    /* 423 */ {2.78541E-02f, 3.44330E-02f, 5.51939E-01f}, {2.93551E-02f, 3.69983E-02f, 5.80599E-01f}, {3.09104E-02f, 3.96928E-02f, 6.09570E-01f},
    /* 426 */ {3.25498E-02f, 4.25402E-02f, 6.39359E-01f}, {3.42714E-02f, 4.55474E-02f, 6.69651E-01f}, {3.60620E-02f, 4.87161E-02f, 6.99829E-01f},
    /* 429 */ {3.79052E-02f, 5.20467E-02f, 7.29177E-01f}, {3.97810E-02f, 5.55384E-02f, 7.56885E-01f}, {4.16705E-02f, 5.91876E-02f, 7.82292E-01f},
    /* 432 */ {4.35728E-02f, 6.29819E-02f, 8.05666E-01f}, {4.54932E-02f, 6.69031E-02f, 8.27599E-01f}, {4.74385E-02f, 7.09285E-02f, 8.48780E-01f},
    /* 435 */ {4.94172E-02f, 7.50299E-02f, 8.69984E-01f}, {5.14343E-02f, 7.91769E-02f, 8.91761E-01f}, {5.34735E-02f, 8.33463E-02f, 9.13444E-01f},
    /* 438 */ {5.55102E-02f, 8.75160E-02f, 9.33977E-01f}, {5.75166E-02f, 9.16625E-02f, 9.52218E-01f}, {5.94619E-02f, 9.57612E-02f, 9.66960E-01f},
    /* 441 */ {6.13240E-02f, 9.97977E-02f, 9.77340E-01f}, {6.31288E-02f, 1.03804E-01f, 9.84028E-01f}, {6.49189E-02f, 1.07834E-01f, 9.88144E-01f},
    /* 444 */ {6.67425E-02f, 1.11948E-01f, 9.90851E-01f}, {6.86538E-02f, 1.16220E-01f, 9.93336E-01f}, {7.06963E-02f, 1.20706E-01f, 9.96373E-01f},
    /* 447 */ {7.28508E-02f, 1.25363E-01f, 9.99038E-01f}, {7.50778E-02f, 1.30111E-01f, 9.99978E-01f}, {7.73325E-02f, 1.34856E-01f, 9.97844E-01f},
    /* 450 */ {7.95647E-02f, 1.39493E-01f, 9.91329E-01f}, {8.17368E-02f, 1.43943E-01f, 9.79657E-01f}, {8.38826E-02f, 1.48281E-01f, 9.63905E-01f},
    /* 453 */ {8.60598E-02f, 1.52637E-01f, 9.45573E-01f}, {8.83322E-02f, 1.57157E-01f, 9.26078E-01f}, {9.07704E-02f, 1.62006E-01f, 9.06735E-01f},
    /* 456 */ {9.34397E-02f, 1.67331E-01f, 8.88509E-01f}, {9.63577E-02f, 1.73144E-01f, 8.71353E-01f}, {9.95304E-02f, 1.79417E-01f, 8.54998E-01f},
    /* 459 */ {1.02964E-01f, 1.86117E-01f, 8.39198E-01f}, {1.06663E-01f, 1.93202E-01f, 8.23726E-01f}, {1.10629E-01f, 2.00621E-01f, 8.08307E-01f},
    /* 462 */ {1.14828E-01f, 2.08315E-01f, 7.92431E-01f}, {1.19217E-01f, 2.16212E-01f, 7.75569E-01f}, {1.23740E-01f, 2.24231E-01f, 7.57244E-01f},
    /* 465 */ {1.28336E-01f, 2.32275E-01f, 7.37043E-01f}, {1.32947E-01f, 2.40257E-01f, 7.14730E-01f}, {1.37570E-01f, 2.48160E-01f, 6.90558E-01f},
    /* 468 */ {1.42218E-01f, 2.55987E-01f, 6.64890E-01f}, {1.46905E-01f, 2.63744E-01f, 6.38078E-01f}, {1.51651E-01f, 2.71441E-01f, 6.10456E-01f},
    /* 471 */ {1.56475E-01f, 2.79097E-01f, 5.82346E-01f}, {1.61404E-01f, 2.86755E-01f, 5.54065E-01f}, {1.66464E-01f, 2.94476E-01f, 5.25903E-01f},
    /* 474 */ {1.71690E-01f, 3.02323E-01f, 4.98108E-01f}, {1.77116E-01f, 3.10372E-01f, 4.70894E-01f}, {1.82777E-01f, 3.18692E-01f, 4.44450E-01f},
    /* 477 */ {1.88685E-01f, 3.27307E-01f, 4.18992E-01f}, {1.94845E-01f, 3.36232E-01f, 3.94699E-01f}, {2.01261E-01f, 3.45480E-01f, 3.71707E-01f},
    /* 480 */ {2.07940E-01f, 3.55066E-01f, 3.50108E-01f}, {2.14875E-01f, 3.64985E-01f, 3.29904E-01f}, {2.22022E-01f, 3.75142E-01f, 3.10864E-01f},
    /* 483 */ {2.29316E-01f, 3.85410E-01f, 2.92741E-01f}, {2.36684E-01f, 3.95646E-01f, 2.75338E-01f}, {2.44046E-01f, 4.05688E-01f, 2.58497E-01f},
    /* 486 */ {2.51347E-01f, 4.15435E-01f, 2.42158E-01f}, {2.58695E-01f, 4.25064E-01f, 2.26504E-01f}, {2.66252E-01f, 4.34851E-01f, 2.11726E-01f},
    /* 489 */ {2.74202E-01f, 4.45097E-01f, 1.97960E-01f}, {2.82752E-01f, 4.56137E-01f, 1.85297E-01f}, {2.92071E-01f, 4.68236E-01f, 1.73751E-01f},
    /* 492 */ {3.02094E-01f, 4.81255E-01f, 1.63149E-01f}, {3.12673E-01f, 4.94927E-01f, 1.53311E-01f}, {3.23637E-01f, 5.08947E-01f, 1.44086E-01f},
    /* 495 */ {3.34786E-01f, 5.22970E-01f, 1.35351E-01f}, {3.45943E-01f, 5.36701E-01f, 1.27011E-01f}, {3.57126E-01f, 5.50195E-01f, 1.19017E-01f},
    /* 498 */ {3.68416E-01f, 5.63616E-01f, 1.11333E-01f}, {3.79907E-01f, 5.77150E-01f, 1.03934E-01f}, {3.91705E-01f, 5.91003E-01f, 9.67990E-02f},
    /* 501 */ {4.03905E-01f, 6.05350E-01f, 8.99169E-02f}, {4.16500E-01f, 6.20160E-01f, 8.32878E-02f}, {4.29453E-01f, 6.35343E-01f, 7.69157E-02f},
    /* 504 */ {4.42720E-01f, 6.50796E-01f, 7.08052E-02f}, {4.56252E-01f, 6.66404E-01f, 6.49614E-02f}, {4.69997E-01f, 6.82055E-01f, 5.94049E-02f},
    /* 507 */ {4.83926E-01f, 6.97672E-01f, 5.42076E-02f}, {4.98012E-01f, 7.13186E-01f, 4.94281E-02f}, {5.12227E-01f, 7.28526E-01f, 4.50993E-02f},
    /* 510 */ {5.26538E-01f, 7.43612E-01f, 4.12337E-02f}, {5.40923E-01f, 7.58396E-01f, 3.78142E-02f}, {5.55406E-01f, 7.72966E-01f, 3.47627E-02f},
    /* 513 */ {5.70025E-01f, 7.87457E-01f, 3.20029E-02f}, {5.84828E-01f, 8.02017E-01f, 2.94746E-02f}, {5.99867E-01f, 8.16808E-01f, 2.71300E-02f},
    /* 516 */ {6.15162E-01f, 8.31919E-01f, 2.49376E-02f}, {6.30569E-01f, 8.47096E-01f, 2.28931E-02f}, {6.45883E-01f, 8.61972E-01f, 2.09956E-02f},
    /* 519 */ {6.60880E-01f, 8.76152E-01f, 1.92427E-02f}, {6.75313E-01f, 8.89214E-01f, 1.76298E-02f}, {6.88975E-01f, 9.00812E-01f, 1.61501E-02f},
    /* 522 */ {7.01886E-01f, 9.11006E-01f, 1.47911E-02f}, {7.14140E-01f, 9.19968E-01f, 1.35407E-02f}, {7.25841E-01f, 9.27890E-01f, 1.23884E-02f},
    /* 525 */ {7.37108E-01f, 9.34977E-01f, 1.13252E-02f}, {7.48046E-01f, 9.41414E-01f, 1.03437E-02f}, {7.58684E-01f, 9.47279E-01f, 9.44092E-03f},
    /* 528 */ {7.69032E-01f, 9.52622E-01f, 8.61365E-03f}, {7.79099E-01f, 9.57498E-01f, 7.85831E-03f}, {7.88900E-01f, 9.61962E-01f, 7.17089E-03f},
    /* 531 */ {7.98472E-01f, 9.66080E-01f, 6.54648E-03f}, {8.07945E-01f, 9.69966E-01f, 5.97777E-03f}, {8.17478E-01f, 9.73743E-01f, 5.45794E-03f},
    /* 534 */ {8.27239E-01f, 9.77539E-01f, 4.98125E-03f}, {8.37403E-01f, 9.81481E-01f, 4.54287E-03f}, {8.48078E-01f, 9.85629E-01f, 4.13908E-03f},
    /* 537 */ {8.59064E-01f, 9.89754E-01f, 3.76794E-03f}, {8.70068E-01f, 9.93551E-01f, 3.42783E-03f}, {8.80779E-01f, 9.96714E-01f, 3.11696E-03f},
    /* 540 */ {8.90871E-01f, 9.98931E-01f, 2.83352E-03f}, {9.00057E-01f, 9.99942E-01f, 2.57556E-03f}, {9.08253E-01f, 9.99692E-01f, 2.34084E-03f},
    /* 543 */ {9.15433E-01f, 9.98178E-01f, 2.12722E-03f}, {9.21575E-01f, 9.95405E-01f, 1.93276E-03f}, {9.26660E-01f, 9.91383E-01f, 1.75573E-03f},
    /* 546 */ {9.30744E-01f, 9.86199E-01f, 1.59455E-03f}, {9.34160E-01f, 9.80229E-01f, 1.44783E-03f}, {9.37318E-01f, 9.73906E-01f, 1.31430E-03f},
    /* 549 */ {9.40634E-01f, 9.67653E-01f, 1.19281E-03f}, {9.44527E-01f, 9.61876E-01f, 1.08230E-03f}, {9.49291E-01f, 9.56820E-01f, 9.81819E-04f},
    /* 552 */ {9.54680E-01f, 9.52151E-01f, 8.90533E-04f}, {9.60309E-01f, 9.47398E-01f, 8.07687E-04f}, {9.65785E-01f, 9.42105E-01f, 7.32568E-04f},
    /* 555 */ {9.70703E-01f, 9.35829E-01f, 6.64512E-04f}, {9.74756E-01f, 9.28274E-01f, 6.02887E-04f}, {9.78055E-01f, 9.19667E-01f, 5.47062E-04f},
    /* 558 */ {9.80819E-01f, 9.10356E-01f, 4.96461E-04f}, {9.83271E-01f, 9.00677E-01f, 4.50571E-04f}, {9.85636E-01f, 8.90949E-01f, 4.08931E-04f},
    /* 561 */ {9.88085E-01f, 8.81386E-01f, 3.71136E-04f}, {9.90560E-01f, 8.71834E-01f, 3.36838E-04f}, {9.92945E-01f, 8.62059E-01f, 3.05723E-04f},
    /* 564 */ {9.95124E-01f, 8.51840E-01f, 2.77504E-04f}, {9.96979E-01f, 8.40969E-01f, 2.51918E-04f}, {9.98411E-01f, 8.29303E-01f, 2.28725E-04f},
    /* 567 */ {9.99391E-01f, 8.16911E-01f, 2.07699E-04f}, {9.99912E-01f, 8.03910E-01f, 1.88638E-04f}, {9.99965E-01f, 7.90413E-01f, 1.71357E-04f},
    /* 570 */ {9.99543E-01f, 7.76526E-01f, 1.55688E-04f}, {9.98615E-01f, 7.62311E-01f, 1.41480E-04f}, {9.97051E-01f, 7.47669E-01f, 1.28596E-04f},
    /* 573 */ {9.94701E-01f, 7.32476E-01f, 1.16912E-04f}, {9.91416E-01f, 7.16622E-01f, 1.06316E-04f}, {9.87057E-01f, 7.00013E-01f, 9.67045E-05f},
    /* 576 */ {9.81599E-01f, 6.82647E-01f, 8.79858E-05f}, {9.75451E-01f, 6.64817E-01f, 8.00756E-05f}, {9.69120E-01f, 6.46858E-01f, 7.28979E-05f},
    /* 579 */ {9.63093E-01f, 6.29072E-01f, 6.63837E-05f}, {9.57841E-01f, 6.11728E-01f, 6.04705E-05f}, {9.53664E-01f, 5.94998E-01f, 5.51020E-05f},
    /* 582 */ {9.50236E-01f, 5.78783E-01f, 5.02269E-05f}, {9.47086E-01f, 5.62935E-01f, 4.57988E-05f}, {9.43752E-01f, 5.47321E-01f, 4.17759E-05f},
    /* 585 */ {9.39781E-01f, 5.31825E-01f, 3.81202E-05f}, {9.34826E-01f, 5.16354E-01f, 3.47974E-05f}, {9.28917E-01f, 5.00870E-01f, 3.17763E-05f},
    /* 588 */ {9.22177E-01f, 4.85350E-01f, 2.90288E-05f}, {9.14729E-01f, 4.69777E-01f, 2.65294E-05f}, {9.06693E-01f, 4.54142E-01f, 2.42549E-05f},
    /* 591 */ {8.98170E-01f, 4.38454E-01f, 2.21847E-05f}, {8.89188E-01f, 4.22778E-01f, 2.02996E-05f}, {8.79759E-01f, 4.07188E-01f, 1.85826E-05f},
    /* 594 */ {8.69894E-01f, 3.91752E-01f, 1.70182E-05f}, {8.59605E-01f, 3.76527E-01f, 1.55924E-05f}, {8.48912E-01f, 3.61559E-01f, 1.42924E-05f},
    /* 597 */ {8.37865E-01f, 3.46856E-01f, 1.31066E-05f}, {8.26522E-01f, 3.32422E-01f, 1.20248E-05f}, {8.14940E-01f, 3.18261E-01f, 1.10373E-05f},
    /* 600 */ {8.03173E-01f, 3.04378E-01f, 1.01356E-05f}, {7.91253E-01f, 2.90784E-01f, 9.31202E-06f}, {7.79118E-01f, 2.77506E-01f, 8.55941E-06f},
    /* 603 */ {7.66691E-01f, 2.64575E-01f, 7.87141E-06f}, {7.53900E-01f, 2.52012E-01f, 7.24221E-06f}, {7.40680E-01f, 2.39837E-01f, 6.66657E-06f},
    /* 606 */ {7.26995E-01f, 2.28065E-01f, 6.13970E-06f}, {7.12905E-01f, 2.16703E-01f, 5.65727E-06f}, {6.98491E-01f, 2.05754E-01f, 5.21535E-06f},
    /* 609 */ {6.83829E-01f, 1.95221E-01f, 4.81036E-06f}, {6.68991E-01f, 1.85104E-01f, 4.43906E-06f}, {6.54037E-01f, 1.75398E-01f, 4.09850E-06f},
    /* 612 */ {6.38979E-01f, 1.66091E-01f, 3.78599E-06f}, {6.23825E-01f, 1.57169E-01f, 3.49911E-06f}, {6.08579E-01f, 1.48620E-01f, 3.23563E-06f},
    /* 615 */ {5.93248E-01f, 1.40431E-01f, 2.99354E-06f}, {5.77857E-01f, 1.32591E-01f, 0.0f}, {5.62493E-01f, 1.25092E-01f, 0.0f},
    /* 618 */ {5.47248E-01f, 1.17928E-01f, 0.0f}, {5.32209E-01f, 1.11091E-01f, 0.0f}, {5.17449E-01f, 1.04573E-01f, 0.0f},
    /* 621 */ {5.02993E-01f, 9.83663E-02f, 0.0f}, {4.88692E-01f, 9.24685E-02f, 0.0f}, {4.74376E-01f, 8.68759E-02f, 0.0f},
    /* 624 */ {4.59896E-01f, 8.15834E-02f, 0.0f}, {4.45125E-01f, 7.65841E-02f, 0.0f}, {4.30007E-01f, 7.18683E-02f, 0.0f},
    /* 627 */ {4.14687E-01f, 6.74186E-02f, 0.0f}, {3.99340E-01f, 6.32176E-02f, 0.0f}, {3.84122E-01f, 5.92492E-02f, 0.0f},
    /* 630 */ {3.69168E-01f, 5.54990E-02f, 0.0f}, {3.54580E-01f, 5.19550E-02f, 0.0f}, {3.40389E-01f, 4.86103E-02f, 0.0f},
    /* 633 */ {3.26609E-01f, 4.54591E-02f, 0.0f}, {3.13249E-01f, 4.24945E-02f, 0.0f}, {3.00316E-01f, 3.97097E-02f, 0.0f},
    /* 636 */ {2.87817E-01f, 3.70952E-02f, 0.0f}, {2.75762E-01f, 3.46347E-02f, 0.0f}, {2.64158E-01f, 3.23125E-02f, 0.0f},
    /* 639 */ {2.53009E-01f, 3.01151E-02f, 0.0f}, {2.42316E-01f, 2.80314E-02f, 0.0f}, {2.32061E-01f, 2.60564E-02f, 0.0f},
    /* 642 */ {2.22158E-01f, 2.42011E-02f, 0.0f}, {2.12516E-01f, 2.24760E-02f, 0.0f}, {2.03060E-01f, 2.08870E-02f, 0.0f},
    /* 645 */ {1.93730E-01f, 1.94366E-02f, 0.0f}, {1.84495E-01f, 1.81200E-02f, 0.0f}, {1.75402E-01f, 1.69149E-02f, 0.0f},
    /* 648 */ {1.66509E-01f, 1.57991E-02f, 0.0f}, {1.57865E-01f, 1.47543E-02f, 0.0f}, {1.49509E-01f, 1.37660E-02f, 0.0f},
    /* 651 */ {1.41470E-01f, 1.28246E-02f, 0.0f}, {1.33760E-01f, 1.19304E-02f, 0.0f}, {1.26383E-01f, 1.10850E-02f, 0.0f},
    /* 654 */ {1.19343E-01f, 1.02892E-02f, 0.0f}, {1.12638E-01f, 9.54315E-03f, 0.0f}, {1.06264E-01f, 8.84609E-03f, 0.0f},
    /* 657 */ {1.00208E-01f, 8.19600E-03f, 0.0f}, {9.44558E-02f, 7.59059E-03f, 0.0f}, {8.89934E-02f, 7.02754E-03f, 0.0f},
    /* 660 */ {8.38077E-02f, 6.50455E-03f, 0.0f}, {7.88865E-02f, 6.01952E-03f, 0.0f}, {7.42191E-02f, 5.57093E-03f, 0.0f},
    /* 663 */ {6.97952E-02f, 5.15728E-03f, 0.0f}, {6.56050E-02f, 4.77687E-03f, 0.0f}, {6.16384E-02f, 4.42794E-03f, 0.0f},
    /* 666 */ {5.78857E-02f, 4.10832E-03f, 0.0f}, {5.43366E-02f, 3.81470E-03f, 0.0f}, {5.09811E-02f, 3.54392E-03f, 0.0f},
    /* 669 */ {4.78096E-02f, 3.29329E-03f, 0.0f}, {4.48132E-02f, 3.06050E-03f, 0.0f}, {4.19831E-02f, 2.84369E-03f, 0.0f},
    /* 672 */ {3.93111E-02f, 2.64175E-03f, 0.0f}, {3.67892E-02f, 2.45374E-03f, 0.0f}, {3.44098E-02f, 2.27875E-03f, 0.0f},
    /* 675 */ {3.21660E-02f, 2.11596E-03f, 0.0f}, {3.00509E-02f, 1.96456E-03f, 0.0f}, {2.80594E-02f, 1.82378E-03f, 0.0f},
    /* 678 */ {2.61861E-02f, 1.69287E-03f, 0.0f}, {2.44260E-02f, 1.57115E-03f, 0.0f}, {2.27738E-02f, 1.45798E-03f, 0.0f},
    /* 681 */ {2.12238E-02f, 1.35274E-03f, 0.0f}, {1.97679E-02f, 1.25476E-03f, 0.0f}, {1.83986E-02f, 1.16340E-03f, 0.0f},
    /* 684 */ {1.71092E-02f, 1.07812E-03f, 0.0f}, {1.58939E-02f, 9.98424E-04f, 0.0f}, {1.47492E-02f, 9.23962E-04f, 0.0f},
    /* 687 */ {1.36773E-02f, 8.54713E-04f, 0.0f}, {1.26804E-02f, 7.90652E-04f, 0.0f}, {1.17590E-02f, 7.31684E-04f, 0.0f},
    /* 690 */ {1.09123E-02f, 6.77653E-04f, 0.0f}, {1.01373E-02f, 6.28297E-04f, 0.0f}, {9.42568E-03f, 5.83108E-04f, 0.0f},
    /* 693 */ {8.76917E-03f, 5.41584E-04f, 0.0f}, {8.16076E-03f, 5.03294E-04f, 0.0f}, {7.59453E-03f, 4.67870E-04f, 0.0f},
    /* 696 */ {7.06588E-03f, 4.35007E-04f, 0.0f}, {6.57252E-03f, 4.04490E-04f, 0.0f}, {6.11262E-03f, 3.76138E-04f, 0.0f},
    /* 699 */ {5.68440E-03f, 3.49784E-04f, 0.0f}, {5.28607E-03f, 3.25278E-04f, 0.0f}, {4.91573E-03f, 3.02477E-04f, 0.0f},
    /* 702 */ {4.57086E-03f, 2.81237E-04f, 0.0f}, {4.24908E-03f, 2.61427E-04f, 0.0f}, {3.94832E-03f, 2.42930E-04f, 0.0f},
    /* 705 */ {3.66675E-03f, 2.25641E-04f, 0.0f}, {3.40297E-03f, 2.09478E-04f, 0.0f}, {3.15632E-03f, 1.94400E-04f, 0.0f},
    /* 708 */ {2.92624E-03f, 1.80370E-04f, 0.0f}, {2.71213E-03f, 1.67347E-04f, 0.0f}, {2.51327E-03f, 1.55286E-04f, 0.0f},
    /* 711 */ {2.32895E-03f, 1.44135E-04f, 0.0f}, {2.15836E-03f, 1.33830E-04f, 0.0f}, {2.00071E-03f, 1.24309E-04f, 0.0f},
    /* 714 */ {1.85521E-03f, 1.15513E-04f, 0.0f}, {1.72108E-03f, 1.07388E-04f, 0.0f}, {1.59750E-03f, 9.98800E-05f, 0.0f},
    /* 717 */ {1.48342E-03f, 9.29320E-05f, 0.0f}, {1.37787E-03f, 8.64907E-05f, 0.0f}, {1.27998E-03f, 8.05090E-05f, 0.0f},
    /* 720 */ {1.18900E-03f, 7.49453E-05f, 0.0f}, {1.10433E-03f, 6.97652E-05f, 0.0f}, {1.02563E-03f, 6.49470E-05f, 0.0f},
    /* 723 */ {9.52602E-04f, 6.04718E-05f, 0.0f}, {8.84959E-04f, 5.63207E-05f, 0.0f}, {8.22396E-04f, 5.24748E-05f, 0.0f},
    /* 726 */ {7.64585E-04f, 4.89142E-05f, 0.0f}, {7.11110E-04f, 4.56137E-05f, 0.0f}, {6.61570E-04f, 4.25490E-05f, 0.0f},
    /* 729 */ {6.15612E-04f, 3.96989E-05f, 0.0f}, {5.72917E-04f, 3.70443E-05f, 0.0f}, {5.33206E-04f, 3.45688E-05f, 0.0f},
    /* 732 */ {4.96234E-04f, 3.22588E-05f, 0.0f}, {4.61782E-04f, 3.01026E-05f, 0.0f}, {4.29654E-04f, 2.80893E-05f, 0.0f},
    /* 735 */ {3.99670E-04f, 2.62088E-05f, 0.0f}, {3.71690E-04f, 2.44529E-05f, 0.0f}, {3.45650E-04f, 2.28177E-05f, 0.0f},
    /* 738 */ {3.21494E-04f, 2.12995E-05f, 0.0f}, {2.99155E-04f, 1.98941E-05f, 0.0f}, {2.78553E-04f, 1.85965E-05f, 0.0f},
    /* 741 */ {2.59583E-04f, 1.74003E-05f, 0.0f}, {2.42061E-04f, 1.62931E-05f, 0.0f}, {2.25809E-04f, 1.52632E-05f, 0.0f},
    /* 744 */ {2.10674E-04f, 1.43005E-05f, 0.0f}, {1.96528E-04f, 1.33965E-05f, 0.0f}, {1.83271E-04f, 1.25449E-05f, 0.0f},
    /* 747 */ {1.70868E-04f, 1.17441E-05f, 0.0f}, {1.59290E-04f, 1.09929E-05f, 0.0f}, {1.48506E-04f, 1.02900E-05f, 0.0f},
    /* 750 */ {1.38482E-04f, 9.63397E-06f, 0.0f}, {1.29179E-04f, 9.02263E-06f, 0.0f}, {1.20542E-04f, 8.45301E-06f, 0.0f},
    /* 753 */ {1.12519E-04f, 7.92207E-06f, 0.0f}, {1.05061E-04f, 7.42700E-06f, 0.0f}, {9.81226E-05f, 6.96522E-06f, 0.0f},
    /* 756 */ {9.16641E-05f, 6.53422E-06f, 0.0f}, {8.56455E-05f, 6.13129E-06f, 0.0f}, {8.00302E-05f, 5.75391E-06f, 0.0f},
    /* 759 */ {7.47858E-05f, 5.39984E-06f, 0.0f}, {6.98827E-05f, 5.06711E-06f, 0.0f}, {6.52966E-05f, 4.75413E-06f, 0.0f},
    /* 762 */ {6.10128E-05f, 4.46019E-06f, 0.0f}, {5.70183E-05f, 4.18466E-06f, 0.0f}, {5.32996E-05f, 3.92690E-06f, 0.0f},
    /* 765 */ {4.98430E-05f, 3.68617E-06f, 0.0f}, {4.66322E-05f, 3.46157E-06f, 0.0f}, {4.36424E-05f, 3.25155E-06f, 0.0f},
    /* 768 */ {4.08501E-05f, 3.05464E-06f, 0.0f}, {3.82346E-05f, 2.86951E-06f, 0.0f}, {3.57781E-05f, 2.69504E-06f, 0.0f},
    /* 771 */ {3.34676E-05f, 2.53040E-06f, 0.0f}, {3.13007E-05f, 2.37550E-06f, 0.0f}, {2.92761E-05f, 2.23033E-06f, 0.0f},
    /* 774 */ {2.73908E-05f, 2.09478E-06f, 0.0f}, {2.56411E-05f, 1.96864E-06f, 0.0f}, {2.40205E-05f, 1.85152E-06f, 0.0f},
    /* 777 */ {2.25160E-05f, 1.74248E-06f, 0.0f}, {2.11145E-05f, 1.64060E-06f, 0.0f}, {1.98046E-05f, 1.54507E-06f, 0.0f},
    /* 780 */ {1.85766E-05f, 1.45518E-06f, 0.0f}, {1.74228E-05f, 1.37039E-06f, 0.0f}, {1.63392E-05f, 1.29045E-06f, 0.0f},
    /* 783 */ {1.53228E-05f, 1.21518E-06f, 0.0f}, {1.43705E-05f, 1.14437E-06f, 0.0f}, {1.34792E-05f, 1.07784E-06f, 0.0f},
    /* 786 */ {1.26454E-05f, 1.01538E-06f, 0.0f}, {1.18652E-05f, 9.56684E-07f, 0.0f}, {1.11344E-05f, 9.01491E-07f, 0.0f},
    /* 789 */ {1.04493E-05f, 8.49542E-07f, 0.0f}, {9.80671E-06f, 8.00606E-07f, 0.0f}, {9.20363E-06f, 7.54492E-07f, 0.0f},
    /* 792 */ {8.63806E-06f, 7.11091E-07f, 0.0f}, {8.10815E-06f, 6.70309E-07f, 0.0f}, {7.61209E-06f, 6.32045E-07f, 0.0f},
    /* 795 */ {7.14808E-06f, 5.96195E-07f, 0.0f}, {6.71432E-06f, 5.62639E-07f, 0.0f}, {6.30876E-06f, 5.31206E-07f, 0.0f},
    /* 798 */ {5.92942E-06f, 5.01726E-07f, 0.0f}, {5.57450E-06f, 4.74047E-07f, 0.0f}, {5.24229E-06f, 4.48030E-07f, 0.0f},
    /* 801 */ {4.93115E-06f, 4.23543E-07f, 0.0f}, {4.63925E-06f, 4.00453E-07f, 0.0f}, {4.36488E-06f, 3.78634E-07f, 0.0f},
    /* 804 */ {4.10651E-06f, 3.57978E-07f, 0.0f}, {3.86280E-06f, 3.38387E-07f, 0.0f}, {3.63270E-06f, 3.19787E-07f, 0.0f},
    /* 807 */ {3.41579E-06f, 3.02157E-07f, 0.0f}, {3.21172E-06f, 2.85486E-07f, 0.0f}, {3.02010E-06f, 2.69756E-07f, 0.0f},
    /* 810 */ {2.84049E-06f, 2.54942E-07f, 0.0f}, {2.67239E-06f, 2.41015E-07f, 0.0f}, {2.51507E-06f, 2.27920E-07f, 0.0f},
    /* 813 */ {2.36783E-06f, 2.15603E-07f, 0.0f}, {2.22998E-06f, 2.04014E-07f, 0.0f}, {2.10091E-06f, 1.93105E-07f, 0.0f},
    /* 816 */ {1.98000E-06f, 1.82829E-07f, 0.0f}, {1.86657E-06f, 1.73134E-07f, 0.0f}, {1.75997E-06f, 1.63969E-07f, 0.0f},
    /* 819 */ {1.65964E-06f, 1.55289E-07f, 0.0f}, {1.56506E-06f, 1.47054E-07f, 0.0f}, {1.47580E-06f, 1.39231E-07f, 0.0f},
    /* 822 */ {1.39160E-06f, 1.31804E-07f, 0.0f}, {1.31221E-06f, 1.24758E-07f, 0.0f}, {1.23742E-06f, 1.18078E-07f, 0.0f},
    /* 825 */ {1.16700E-06f, 1.11751E-07f, 0.0f}, {1.10072E-06f, 1.05760E-07f, 0.0f}, {1.03839E-06f, 1.00093E-07f, 0.0f},
    /* 828 */ {9.79788E-07f, 9.47349E-08f, 0.0f}, {9.24725E-07f, 8.96718E-08f, 0.0f}, {8.73008E-07f, 8.48902E-08f, 0.0f},
};

static const char ycol__cal_magic[8] = { 'Y', 'S', 'P', 'C', 'A', 'L', '\0', '\1' };

/* The canonical layout is fixed: every offset, on every compiler. */
#define YCOL__AT(f, off) typedef char ycol__cal_at_##f[offsetof(ycol_cal, f) == (off) ? 1 : -1]
YCOL__AT(version, 8);
YCOL__AT(n_readings, 36);
YCOL__AT(date, 40);
YCOL__AT(display, 48);
YCOL__AT(screen, 240);
YCOL__AT(readings, 496);
YCOL__AT(n_wl, 5616);
YCOL__AT(lut_n, 5628);
YCOL__AT(spd, 5632);
YCOL__AT(rgb_to_xyz, 13168);
YCOL__AT(white_err, 13360);
YCOL__AT(lut, 13368);
YCOL__AT(crc, 62524);
typedef char ycol__cal_size[sizeof(ycol_cal) == 62528 ? 1 : -1];
typedef char ycol__reading_size[sizeof(ycol_cal_reading) == 20 ? 1 : -1];
#undef YCOL__AT

YCOL_API void ycol_cal_init(ycol_cal* c) {
    if (!c) return;
    memset(c, 0, sizeof *c);
    memcpy(c->magic, ycol__cal_magic, 8);
    c->version = 1;
    c->bytes = (uint32_t)sizeof *c;
}

YCOL_API int ycol_cal_add(ycol_cal* c, int gun, float level, float Y, float x, float y) {
    ycol_cal_reading* r;
    if (!c) return YCOL_ERR_ARG;
    if (c->n_readings < 0 || c->n_readings >= YCOL_CAL_MAX_READINGS) return YCOL_ERR_FULL;
    if (gun < YCOL_GUN_BLACK || gun > YCOL_GUN_WHITE || !(level >= 0.0f && level <= 1.0f) || !(Y >= 0.0f))
        return YCOL_ERR_ARG;
    r = &c->readings[c->n_readings++];
    r->gun = gun; r->level = level; r->Y = Y; r->x = x; r->y = y;
    if (x > 0.0f && y > 0.0f) c->flags |= YCOL_CAL_HAS_XY;
    c->crc = 0;   /* unsealed: derive() seals it again */
    return YCOL_OK;
}

YCOL_API int ycol_cal_set_spectra(ycol_cal* c, float wl_start, float wl_step, int n,
                                      const float* r, const float* g, const float* b, const float* black) {
    int i;
    if (!c || !r || !g || !b || n < 2 || n > YCOL_CAL_MAX_WL || !(wl_step > 0.0f)) return YCOL_ERR_ARG;
    memset(c->spd, 0, sizeof c->spd);
    for (i = 0; i < n; i++) {
        c->spd[0][i] = r[i]; c->spd[1][i] = g[i]; c->spd[2][i] = b[i];
        c->spd[3][i] = black ? black[i] : 0.0f;
    }
    c->n_wl = n; c->wl_start = wl_start; c->wl_step = wl_step;
    c->flags |= YCOL_CAL_HAS_SPECTRA;
    c->crc = 0;
    return YCOL_OK;
}

static int ycol__inv3(const double* m, double* out) {
    double a = m[0], b = m[1], c = m[2], d = m[3], e = m[4], f = m[5], g = m[6], h = m[7], i = m[8];
    double A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
    double det = a * A + b * B + c * C;
    if (!(fabs(det) > 1e-300)) return 0;
    out[0] = A / det; out[1] = -(b * i - c * h) / det; out[2] = (b * f - c * e) / det;
    out[3] = B / det; out[4] = (a * i - c * g) / det;  out[5] = -(a * f - c * d) / det;
    out[6] = C / det; out[7] = -(a * h - b * g) / det; out[8] = (a * e - b * d) / det;
    return 1;
}

static void ycol__mul3(const double* m, const double* v, double* out) {
    double t0 = m[0] * v[0] + m[1] * v[1] + m[2] * v[2];
    double t1 = m[3] * v[0] + m[4] * v[1] + m[5] * v[2];
    double t2 = m[6] * v[0] + m[7] * v[1] + m[8] * v[2];
    out[0] = t0; out[1] = t1; out[2] = t2;
}

/* Monotone cubic Hermite (Fritsch and Carlson 1980) through (x[i], y[i]),
 * x and y strictly increasing. */
static double ycol__hermite(const double* x, const double* y, const double* m, int n, double t) {
    int lo = 0, hi = n - 1;
    double h, s, s2, s3;
    if (t <= x[0]) return y[0];
    if (t >= x[n - 1]) return y[n - 1];
    while (hi - lo > 1) { int mid = (lo + hi) / 2; if (x[mid] <= t) lo = mid; else hi = mid; }
    h = x[hi] - x[lo];
    s = (t - x[lo]) / h; s2 = s * s; s3 = s2 * s;
    return (2 * s3 - 3 * s2 + 1) * y[lo] + (s3 - 2 * s2 + s) * h * m[lo] + (-2 * s3 + 3 * s2) * y[hi] + (s3 - s2) * h * m[hi];
}

/* Everything ycol_cal_derive() computes. verify != 0: compare with what is
 * stored instead of writing, and fail when they differ beyond rounding. */
static int ycol__cal_derive(ycol_cal* c, int verify, char* err, size_t cap) {
    double lx[3][YCOL_CAL_MAX_READINGS + 1], ly[3][YCOL_CAL_MAX_READINGS + 1], lm[YCOL_CAL_MAX_READINGS + 1];
    int np[3] = { 0, 0, 0 }, i, k, gun, lut_n, nblack = 0, full[3] = { -1, -1, -1 }, white = -1;
    double Ybk = 0.0, Y1[3], xyz[9], lms[9], bxyz[3] = { 0, 0, 0 }, blms[3] = { 0, 0, 0 }, werr = 0.0;
    const ycol_cal_reading* black = NULL;
    double maxd = 0.0;
    if (memcmp(c->magic, ycol__cal_magic, 8) != 0 || c->version != 1 || c->bytes != (uint32_t)sizeof *c) {
        ycol__set_error(err, cap, "not a canonical calibration (magic, version 1, %u bytes); the pack tool writes one",
                          (unsigned)sizeof *c);
        return YCOL_ERR_FORMAT;
    }
    if (c->n_readings < 1 || c->n_readings > YCOL_CAL_MAX_READINGS) {
        ycol__set_error(err, cap, "%d readings; a calibration is rebuilt from its readings", c->n_readings);
        return YCOL_ERR_RANGE;
    }
    lut_n = c->lut_n ? c->lut_n : YCOL_CAL_MAX_LUT;
    if (lut_n < 2 || lut_n > YCOL_CAL_MAX_LUT) { ycol__set_error(err, cap, "lut_n %d outside 2..4096", c->lut_n); return YCOL_ERR_RANGE; }
    for (i = 0; i < c->n_readings; i++) {
        const ycol_cal_reading* r = &c->readings[i];
        if (r->gun == YCOL_GUN_BLACK) { black = r; nblack++; }
        else if (r->gun == YCOL_GUN_WHITE) white = i;
        else if (r->gun < 0 || r->gun > 2) { ycol__set_error(err, cap, "reading %d: gun %d", i, r->gun); return YCOL_ERR_RANGE; }
    }
    if (nblack != 1) { ycol__set_error(err, cap, "%d black readings; one is needed", nblack); return YCOL_ERR_RANGE; }
    Ybk = black->Y;
    for (gun = 0; gun < 3; gun++) {
        lx[gun][0] = 0.0; ly[gun][0] = Ybk; np[gun] = 1;
        for (i = 0; i < c->n_readings; i++) {
            const ycol_cal_reading* r = &c->readings[i];
            int j;
            if (r->gun != gun) continue;
            /* insertion by level */
            for (j = np[gun]; j > 1 && lx[gun][j - 1] > r->level; j--) { lx[gun][j] = lx[gun][j - 1]; ly[gun][j] = ly[gun][j - 1]; }
            lx[gun][j] = r->level; ly[gun][j] = r->Y;
            np[gun]++;
            if (r->level == 1.0f) full[gun] = i;
        }
        if (full[gun] < 0) { ycol__set_error(err, cap, "gun %d has no reading at level 1", gun); return YCOL_ERR_RANGE; }
        for (k = 1; k < np[gun]; k++) {
            if (!(lx[gun][k] > lx[gun][k - 1]) || !(ly[gun][k] > ly[gun][k - 1])) {
                ycol__set_error(err, cap, "gun %d: luminance does not rise at level %.6g (%.6g cd/m2 after %.6g); "
                                  "non-monotone readings are refused", gun, lx[gun][k], ly[gun][k], ly[gun][k - 1]);
                return YCOL_ERR_RANGE;
            }
        }
        Y1[gun] = ly[gun][np[gun] - 1];
        for (k = 0; k < np[gun]; k++) ly[gun][k] = (ly[gun][k] - Ybk) / (Y1[gun] - Ybk);
    }
    if (white >= 0) {
        double sum = Ybk + (Y1[0] - Ybk) + (Y1[1] - Ybk) + (Y1[2] - Ybk);
        werr = (c->readings[white].Y - sum) / c->readings[white].Y * 100.0;
    }
    /* XYZ (1931) from the chromaticities of the full-level readings */
    memset(xyz, 0, sizeof xyz);
    {
        int have = 1;
        for (gun = 0; gun < 3; gun++) {
            const ycol_cal_reading* r = &c->readings[full[gun]];
            if (!(r->x > 0.0f && r->y > 0.0f)) have = 0;
        }
        if (have && Ybk > 0.0) {
            if (black->x > 0.0f && black->y > 0.0f) {
                bxyz[0] = black->x / black->y * Ybk; bxyz[1] = Ybk; bxyz[2] = (1.0 - black->x - black->y) / black->y * Ybk;
            } else {
                have = 0;   /* a black with light needs its chromaticity */
            }
        }
        if (have) {
            for (gun = 0; gun < 3; gun++) {
                const ycol_cal_reading* r = &c->readings[full[gun]];
                double Y = r->Y, x = r->x, y = r->y;
                xyz[0 * 3 + gun] = x / y * Y - bxyz[0];
                xyz[1 * 3 + gun] = Y - bxyz[1];
                xyz[2 * 3 + gun] = (1.0 - x - y) / y * Y - bxyz[2];
            }
        }
    }
    /* LMS from the spectra: the 1-nm cone table against each spectrum
     * interpolated linearly onto it, 0 outside the measured range. */
    memset(lms, 0, sizeof lms);
    if ((c->flags & YCOL_CAL_HAS_SPECTRA) && c->n_wl >= 2 && c->n_wl <= YCOL_CAL_MAX_WL && c->wl_step > 0) {
        double acc[4][3];
        int j, nm;
        memset(acc, 0, sizeof acc);
        for (nm = 390; nm <= 830; nm++) {
            double t = ((double)nm - c->wl_start) / c->wl_step, f;
            int i0 = (int)floor(t);
            if (t < 0.0 || t > (double)(c->n_wl - 1)) continue;
            if (i0 >= c->n_wl - 1) i0 = c->n_wl - 2;
            f = t - i0;
            for (j = 0; j < 4; j++) {
                double v = c->spd[j][i0] + (c->spd[j][i0 + 1] - c->spd[j][i0]) * f;
                acc[j][0] += ycol__ss2[nm - 390][0] * v;
                acc[j][1] += ycol__ss2[nm - 390][1] * v;
                acc[j][2] += ycol__ss2[nm - 390][2] * v;
            }
        }
        for (k = 0; k < 3; k++) {
            blms[k] = acc[3][k];
            for (j = 0; j < 3; j++) lms[k * 3 + j] = acc[j][k] - acc[3][k];
        }
    }
    if (!verify) {
        memcpy(c->rgb_to_xyz, xyz, sizeof xyz);
        memcpy(c->rgb_to_lms, lms, sizeof lms);
        memcpy(c->black_xyz, bxyz, sizeof bxyz);
        memcpy(c->black_lms, blms, sizeof blms);
        c->white_err = werr;
        c->lut_n = lut_n;
    } else {
        for (k = 0; k < 9; k++) {
            double s1 = fabs(xyz[k] - c->rgb_to_xyz[k]) / (fabs(xyz[k]) + 1e-30);
            double s2 = fabs(lms[k] - c->rgb_to_lms[k]) / (fabs(lms[k]) + 1e-30);
            if ((xyz[k] != c->rgb_to_xyz[k] && s1 > 1e-9) || (lms[k] != c->rgb_to_lms[k] && s2 > 1e-9)) {
                ycol__set_error(err, cap, "the stored matrices differ from the ones its readings give");
                return YCOL_ERR_FORMAT;
            }
        }
        if (c->lut_n != lut_n) { ycol__set_error(err, cap, "lut_n differs"); return YCOL_ERR_FORMAT; }
    }
    /* The CLUT: entry k is the level whose normalized luminance is k/(n-1). */
    for (gun = 0; gun < 3; gun++) {
        int n = np[gun];
        const double* x = lx[gun];
        const double* y = ly[gun];
        double dk[YCOL_CAL_MAX_READINGS + 1];
        dk[0] = 1.0;   /* n >= 2 always: black and the full level */
        for (k = 0; k < n - 1; k++) dk[k] = (y[k + 1] - y[k]) / (x[k + 1] - x[k]);
        lm[0] = dk[0];
        lm[n - 1] = dk[n - 2];
        for (k = 1; k < n - 1; k++) lm[k] = 0.5 * (dk[k - 1] + dk[k]);
        for (k = 0; k < n - 1; k++) {
            double a = lm[k] / dk[k], b = lm[k + 1] / dk[k], s = a * a + b * b;
            if (s > 9.0) { double t = 3.0 / sqrt(s); lm[k] = t * a * dk[k]; lm[k + 1] = t * b * dk[k]; }
        }
        for (k = 0; k < lut_n; k++) {
            double target = (double)k / (double)(lut_n - 1), lo = 0.0, hi = 1.0, v;
            int it;
            if (k == 0) v = 0.0;
            else if (k == lut_n - 1) v = 1.0;
            else {
                for (it = 0; it < 64; it++) {
                    double mid = 0.5 * (lo + hi);
                    if (ycol__hermite(x, y, lm, n, mid) < target) lo = mid; else hi = mid;
                }
                v = 0.5 * (lo + hi);
            }
            if (!verify) c->lut[gun][k] = (float)v;
            else if (fabs((double)c->lut[gun][k] - v) > maxd) maxd = fabs((double)c->lut[gun][k] - v);
        }
    }
    if (verify && maxd > 1e-6) {
        ycol__set_error(err, cap, "the stored CLUT differs from the one its readings give by %.3g", maxd);
        return YCOL_ERR_FORMAT;
    }
    return YCOL_OK;
}

YCOL_API int ycol_cal_derive(ycol_cal* c, char* err, size_t cap) {
    if (!c) return YCOL_ERR_ARG;
    if (err && cap) err[0] = '\0';
    {
        /* sealed here, so a calibration made by the manual's recipe passes
         * ycol_cal_check() and open as it is: the CRC always covers what
         * derive() wrote */
        int rc = ycol__cal_derive(c, 0, err, cap);
        if (rc >= 0) c->crc = ycol__crc32(c, offsetof(ycol_cal, crc));
        return rc;
    }
}

YCOL_API int ycol_cal_check(const ycol_cal* c, char* err, size_t cap) {
    if (!c) return YCOL_ERR_ARG;
    if (err && cap) err[0] = '\0';
    if (c->crc && c->crc != ycol__crc32(c, offsetof(ycol_cal, crc))) {
        ycol__set_error(err, cap, "CRC mismatch");
        return YCOL_ERR_FORMAT;
    }
    /* verify mode reads only; the cast does not write */
    return ycol__cal_derive((ycol_cal*)c, 1, err, cap);
}

YCOL_API int ycol_cal_save(ycol_cal* c, void* out, size_t cap) {
    if (!c) return YCOL_ERR_ARG;
    c->crc = ycol__crc32(c, offsetof(ycol_cal, crc));
    if (!out) return (int)sizeof *c;
    if (cap < sizeof *c) return YCOL_ERR_FULL;
    memcpy(out, c, sizeof *c);
    return (int)sizeof *c;
}

YCOL_API int ycol_cal_load(ycol_cal* c, const void* bytes, size_t n, char* err, size_t cap) {
    int rc;
    if (err && cap) err[0] = '\0';
    if (!c || !bytes) return YCOL_ERR_ARG;
    if (n != sizeof *c || memcmp(bytes, ycol__cal_magic, 8) != 0) {
        ycol__set_error(err, cap, "not a canonical calibration (%lu bytes, want %u with magic YSPCAL); "
                          "the pack tool writes one", (unsigned long)n, (unsigned)sizeof *c);
        return YCOL_ERR_FORMAT;
    }
    memcpy(c, bytes, sizeof *c);
    if (c->crc != ycol__crc32(c, offsetof(ycol_cal, crc))) {
        memset(c, 0, sizeof *c);
        ycol__set_error(err, cap, "CRC mismatch: the file was changed or truncated");
        return YCOL_ERR_FORMAT;
    }
    rc = ycol__cal_derive(c, 1, err, cap);
    if (rc < 0) memset(c, 0, sizeof *c);
    return rc;
}

YCOL_API int ycol_cal_nominal(ycol_cal* c, const float xy[4][2], float white_Y, double gamma) {
    double m[9], inv[9], w[3], Yp[3];
    int gun, k, rc;
    if (!c || !xy || !(white_Y > 0.0f) || !(gamma > 0.0)) return YCOL_ERR_ARG;
    for (k = 0; k < 4; k++) if (!(xy[k][1] > 0.0f)) return YCOL_ERR_ARG;
    ycol_cal_init(c);
    /* luminance of each primary so that the three sum to the white point */
    for (gun = 0; gun < 3; gun++) {
        m[0 * 3 + gun] = xy[gun][0] / xy[gun][1];
        m[1 * 3 + gun] = 1.0;
        m[2 * 3 + gun] = (1.0 - xy[gun][0] - xy[gun][1]) / xy[gun][1];
    }
    w[0] = xy[3][0] / xy[3][1] * white_Y; w[1] = white_Y; w[2] = (1.0 - xy[3][0] - xy[3][1]) / xy[3][1] * white_Y;
    if (!ycol__inv3(m, inv)) return YCOL_ERR_RANGE;
    ycol__mul3(inv, w, Yp);
    ycol_cal_add(c, YCOL_GUN_BLACK, 0.0f, 0.0f, 0.0f, 0.0f);
    for (gun = 0; gun < 3; gun++) {
        if (!(Yp[gun] > 0.0)) return YCOL_ERR_RANGE;
        for (k = 1; k <= 32; k++) {
            double l = k / 32.0;
            ycol_cal_add(c, gun, (float)l, (float)(Yp[gun] * pow(l, gamma)), xy[gun][0], xy[gun][1]);
        }
    }
    ycol_cal_add(c, YCOL_GUN_WHITE, 1.0f, white_Y, xy[3][0], xy[3][1]);
    c->flags |= YCOL_CAL_NOMINAL;
    snprintf(c->instrument, sizeof c->instrument, "nominal: stated primaries, gamma %.4g", gamma);
    rc = ycol_cal_derive(c, NULL, 0);
    if (rc >= 0) ycol_cal_save(c, NULL, 0);
    return rc;
}

static int ycol__has_lms(const ycol_cal* c) {
    return c && (c->flags & YCOL_CAL_HAS_SPECTRA) && (c->rgb_to_lms[0] != 0.0 || c->rgb_to_lms[4] != 0.0);
}

YCOL_API int ycol_cal_lms(const ycol_cal* c, const float rgb[3], double out[3]) {
    double v[3];
    if (!ycol__has_lms(c) || !rgb || !out) return YCOL_ERR_REFUSED;
    v[0] = rgb[0]; v[1] = rgb[1]; v[2] = rgb[2];
    ycol__mul3(c->rgb_to_lms, v, out);
    out[0] += c->black_lms[0]; out[1] += c->black_lms[1]; out[2] += c->black_lms[2];
    return YCOL_OK;
}

YCOL_API int ycol_cal_dir_cone(const ycol_cal* c, const float bg[3], const float cc[3], float dir[3]) {
    double lb[3], d[3], inv[9], r[3];
    int k;
    if (!bg || !cc || !dir) return YCOL_ERR_ARG;
    if (ycol_cal_lms(c, bg, lb) < 0) return YCOL_ERR_REFUSED;
    if (!ycol__inv3(c->rgb_to_lms, inv)) return YCOL_ERR_RANGE;
    for (k = 0; k < 3; k++) d[k] = (double)cc[k] * lb[k];
    ycol__mul3(inv, d, r);
    for (k = 0; k < 3; k++) dir[k] = (float)r[k];
    return YCOL_OK;
}

/* Psychtoolbox's ComputeDKL_M (Brainard 1996, the appendix in Kaiser and
 * Boynton), with luminance V = 0.68990272 L + 0.34832189 M. */
YCOL_API int ycol_cal_dkl_matrix(const ycol_cal* c, const float bg[3], double m[9]) {
    static const double w0 = 0.68990272, w1 = 0.34832189;
    double b[3], raw[9], inv[9], col[3], pooled[3], resp[3];
    int k, j;
    if (!bg || !m) return YCOL_ERR_ARG;
    if (ycol_cal_lms(c, bg, b) < 0) return YCOL_ERR_REFUSED;
    if (!(b[0] > 0 && b[1] > 0 && b[2] > 0)) return YCOL_ERR_RANGE;
    raw[0] = w0;  raw[1] = w1;            raw[2] = 0.0;
    raw[3] = 1.0; raw[4] = -b[0] / b[1];  raw[5] = 0.0;
    raw[6] = -w0; raw[7] = -w1;           raw[8] = (w0 * b[0] + w1 * b[1]) / b[2];
    if (!ycol__inv3(raw, inv)) return YCOL_ERR_RANGE;
    /* each isolating stimulus, scaled to unit pooled cone contrast, must
     * give a unit response: rescale the rows */
    for (j = 0; j < 3; j++) {
        double s = 0.0;
        for (k = 0; k < 3; k++) { col[k] = inv[k * 3 + j]; s += (col[k] / b[k]) * (col[k] / b[k]); }
        pooled[j] = sqrt(s);
        for (k = 0; k < 3; k++) col[k] /= pooled[j];
        ycol__mul3(raw, col, resp);
        for (k = 0; k < 3; k++) m[j * 3 + k] = raw[j * 3 + k] / resp[j];
    }
    return YCOL_OK;
}

YCOL_API int ycol_cal_dir_dkl(const ycol_cal* c, const float bg[3], const float dkl[3], float dir[3]) {
    double m[9], inv[9], v[3], cone[3], linv[9], r[3];
    int k, rc;
    if (!dkl || !dir) return YCOL_ERR_ARG;
    rc = ycol_cal_dkl_matrix(c, bg, m);
    if (rc < 0) return rc;
    if (!ycol__inv3(m, inv) || !ycol__inv3(c->rgb_to_lms, linv)) return YCOL_ERR_RANGE;
    v[0] = dkl[0]; v[1] = dkl[1]; v[2] = dkl[2];
    ycol__mul3(inv, v, cone);
    ycol__mul3(linv, cone, r);
    for (k = 0; k < 3; k++) dir[k] = (float)r[k];
    return YCOL_OK;
}

YCOL_API void ycol_dkl_from_sph(float elevation_deg, float azimuth_deg, float radius, float dkl[3]) {
    const double e = (double)elevation_deg * 3.14159265358979323846 / 180.0;
    const double a = (double)azimuth_deg * 3.14159265358979323846 / 180.0;
    if (!dkl) return;
    dkl[0] = (float)(radius * sin(e));
    dkl[1] = (float)(radius * cos(e) * cos(a));
    dkl[2] = (float)(radius * cos(e) * sin(a));
}

YCOL_API float ycol_max_contrast(const float bg[3], const float dir[3]) {
    float best = 3.4e38f;
    int k;
    if (!bg || !dir) return 0.0f;
    for (k = 0; k < 3; k++) {
        float a = fabsf(dir[k]), room = bg[k] < 1.0f - bg[k] ? bg[k] : 1.0f - bg[k];
        if (a > 0.0f && room / a < best) best = room / a;
    }
    return best;
}

/* --- v0.1: what ysp/gfx.h v0.3 did not have ------------------------------ */

#define YCOL__PI 3.14159265358979323846
#define YCOL__D2R (YCOL__PI / 180.0)

static double ycol__nan(void) {
    volatile double z = 0.0;
    return z / z;
}

static int ycol__finite(double v) { return v == v && v - v == 0.0; }

static int ycol__finite3(const double* v) {
    return ycol__finite(v[0]) && ycol__finite(v[1]) && ycol__finite(v[2]);
}

static const float (*ycol__table(int cones))[3] {
    return cones == YCOL_CONES_SS10 ? ycol__ss10 : ycol__ss2;
}

YCOL_API void ycol_cone_fundamentals(int cones, double nm, double lms[3]) {
    const float (*t)[3] = ycol__table(cones);
    int i0;
    double f;
    if (!lms) return;
    lms[0] = lms[1] = lms[2] = 0.0;
    if (!(nm >= 390.0 && nm <= 830.0)) return;
    i0 = (int)(nm - 390.0);
    if (i0 >= 440) i0 = 439;
    f = nm - 390.0 - i0;
    lms[0] = t[i0][0] + (t[i0 + 1][0] - t[i0][0]) * f;
    lms[1] = t[i0][1] + (t[i0 + 1][1] - t[i0][1]) * f;
    lms[2] = t[i0][2] + (t[i0 + 1][2] - t[i0][2]) * f;
}

YCOL_API void ycol_cone_luminance(int cones, double w[3]) {
    if (!w) return;
    /* The CIE 2008 physiologically relevant luminous efficiency functions as
     * sums of the fundamentals (CVRL). */
    if (cones == YCOL_CONES_SS10) { w[0] = 0.69283932; w[1] = 0.34967567; }
    else                            { w[0] = 0.68990272; w[1] = 0.34832189; }
    w[2] = 0.0;
}

/* RGB to LMS for `cones` from the spectra: ycol__cal_derive()'s loop on
 * the given table. For SS2 it equals the stored matrix bit for bit. */
static int ycol__integrate(const ycol_cal* c, int cones, double lms[9], double blms[3]) {
    const float (*t)[3] = ycol__table(cones);
    double acc[4][3];
    int j, k, nm;
    if (!((c->flags & YCOL_CAL_HAS_SPECTRA) && c->n_wl >= 2 && c->n_wl <= YCOL_CAL_MAX_WL && c->wl_step > 0)) return 0;
    memset(acc, 0, sizeof acc);
    for (nm = 390; nm <= 830; nm++) {
        double tt = ((double)nm - c->wl_start) / c->wl_step, f;
        int i0 = (int)floor(tt);
        if (tt < 0.0 || tt > (double)(c->n_wl - 1)) continue;
        if (i0 >= c->n_wl - 1) i0 = c->n_wl - 2;
        f = tt - i0;
        for (j = 0; j < 4; j++) {
            double v = c->spd[j][i0] + (c->spd[j][i0 + 1] - c->spd[j][i0]) * f;
            acc[j][0] += t[nm - 390][0] * v;
            acc[j][1] += t[nm - 390][1] * v;
            acc[j][2] += t[nm - 390][2] * v;
        }
    }
    for (k = 0; k < 3; k++) {
        blms[k] = acc[3][k];
        for (j = 0; j < 3; j++) lms[k * 3 + j] = acc[j][k] - acc[3][k];
    }
    return 1;
}

static void ycol__mm3(const double* a, const double* b, double* out) {
    double t[9];
    int i, j, k;
    for (i = 0; i < 3; i++)
        for (j = 0; j < 3; j++) {
            t[i * 3 + j] = 0.0;
            for (k = 0; k < 3; k++) t[i * 3 + j] += a[i * 3 + k] * b[k * 3 + j];
        }
    memcpy(out, t, sizeof t);
}

static void ycol__xy_to_xyz(double x, double y, double Y, double out[3]) {
    out[0] = x / y * Y; out[1] = Y; out[2] = (1.0 - x - y) / y * Y;
}

YCOL_API int ycol_cal_set_spectra_nominal(ycol_cal* c, float wl_start, float wl_step, int n,
                                              const float* r, const float* g, const float* b, const float* black) {
    int rc = ycol_cal_set_spectra(c, wl_start, wl_step, n, r, g, b, black);
    if (rc >= 0) c->flags |= YCOL_CAL_SPECTRA_NOMINAL;
    return rc;
}

YCOL_API int ycol_cal_describe(const ycol_cal* c, char* buf, size_t cap) {
    int n;
    if (!c || !buf || cap == 0) return YCOL_ERR_ARG;
    n = snprintf(buf, cap, "calibration %08x: %d readings%s%s%s%s, white minus the guns %+.3f%%, CLUT %d",
                 (unsigned)c->crc, c->n_readings, c->flags & YCOL_CAL_NOMINAL ? ", NOMINAL (not measured)" : "",
                 c->flags & YCOL_CAL_HAS_XY ? ", xy" : ", no xy",
                 c->flags & YCOL_CAL_HAS_SPECTRA ? ", spectra" : ", no spectra",
                 c->flags & YCOL_CAL_SPECTRA_NOMINAL ? " (NOMINAL: a datasheet's)" : "",
                 c->white_err, c->lut_n ? c->lut_n : YCOL_CAL_MAX_LUT);
    if (n >= 0 && c->display[0] && (size_t)n < cap)
        n += snprintf(buf + n, cap - (size_t)n, ", display \"%.64s\"", c->display);
    return n < 0 ? YCOL_ERR_ARG : ((size_t)n >= cap ? YCOL_ERR_FULL : n);
}

/* --- the D65 sources and standard primaries ------------------------------- */

static const double ycol__prims[5][8] = {   /* R, G, B, white xy */
    { 0.640, 0.330, 0.300, 0.600, 0.150, 0.060, 0.3127, 0.3290 },   /* BT.709, sRGB */
    { 0.630, 0.340, 0.310, 0.595, 0.155, 0.070, 0.3127, 0.3290 },   /* BT.601 525 (SMPTE C) */
    { 0.640, 0.330, 0.290, 0.600, 0.150, 0.060, 0.3127, 0.3290 },   /* BT.601 625 (EBU) */
    { 0.708, 0.292, 0.170, 0.797, 0.131, 0.046, 0.3127, 0.3290 },   /* BT.2020 */
    { 0.680, 0.320, 0.265, 0.690, 0.150, 0.060, 0.3127, 0.3290 } }; /* Display P3 */

/* Linear RGB to XYZ (white Y = 1) from primaries, in ysp/gfx.h's order of
 * operations, so the video path can move here without changing a bit. */
static int ycol__prim_matrix(const double* p, double src[9]) {
    double P[9], Pi[9], w[3], s[3];
    int k;
    for (k = 0; k < 3; k++) {
        P[0 * 3 + k] = p[2 * k] / p[2 * k + 1];
        P[1 * 3 + k] = 1.0;
        P[2 * 3 + k] = (1.0 - p[2 * k] - p[2 * k + 1]) / p[2 * k + 1];
    }
    w[0] = p[6] / p[7]; w[1] = 1.0; w[2] = (1.0 - p[6] - p[7]) / p[7];
    if (!ycol__inv3(P, Pi)) return 0;
    ycol__mul3(Pi, w, s);
    for (k = 0; k < 9; k++) src[k] = P[k] * s[k % 3];
    return 1;
}

/* Bradford: XYZ under white ws to XYZ under white wd (both at Y = 1). */
static void ycol__bradford(const double ws[3], const double wd[3], double out[9]) {
    static const double mb[9] = { 0.8951, 0.2664, -0.1614, -0.7502, 1.7135, 0.0367, 0.0389, -0.0685, 1.0296 };
    double mi[9], cs[3], cd[3], d[9], t[9];
    ycol__inv3(mb, mi);
    ycol__mul3(mb, ws, cs);
    ycol__mul3(mb, wd, cd);
    memset(d, 0, sizeof d);
    d[0] = cd[0] / cs[0]; d[4] = cd[1] / cs[1]; d[8] = cd[2] / cs[2];
    ycol__mm3(d, mb, t);
    ycol__mm3(mi, t, out);
}

static const double ycol__d65[3] = { 0.3127 / 0.3290, 1.0, (1.0 - 0.3127 - 0.3290) / 0.3290 };

static int ycol__prim_index(int prim) {
    switch (prim) {
    case YCOL_PRIM_BT709: return 0;
    case YCOL_PRIM_BT601_525: return 1;
    case YCOL_PRIM_BT601_625: return 2;
    case YCOL_PRIM_BT2020: return 3;
    case YCOL_PRIM_P3: return 4;
    default: return -1;
    }
}

/* The display's white, black removed, at Y = 1. */
static void ycol__display_white_rel(const ycol_ctx* cx, double w[3]) {
    const double* m = cx->rgb_to_xyz;
    double yw = m[3] + m[4] + m[5];
    w[0] = (m[0] + m[1] + m[2]) / yw; w[1] = 1.0; w[2] = (m[6] + m[7] + m[8]) / yw;
}

static int ycol__src_matrix(const ycol_ctx* cx, int prim, double m[9], double off[3]) {
    double src[9], a[9], as[9];
    int i = ycol__prim_index(prim), k;
    if (i < 0) return YCOL_ERR_ARG;
    if (!(cx->can & YCOL_CAN_XYZ)) return YCOL_ERR_REFUSED;
    if (!ycol__prim_matrix(ycol__prims[i], src)) return YCOL_ERR_RANGE;
    off[0] = off[1] = off[2] = 0.0;
    if (cx->desc.src_white_Y > 0.0) {   /* absolute */
        if (cx->desc.adapt == YCOL_ADAPT_BRADFORD) {
            double wd[3] = { cx->white_xyz[0] / cx->white_xyz[1], 1.0, cx->white_xyz[2] / cx->white_xyz[1] };
            ycol__bradford(ycol__d65, wd, a);
            ycol__mm3(a, src, as);
        } else {
            memcpy(as, src, sizeof as);
        }
        ycol__mm3(cx->xyz_to_rgb, as, m);
        for (k = 0; k < 9; k++) m[k] *= cx->desc.src_white_Y;
        ycol__mul3(cx->xyz_to_rgb, cx->black_xyz, off);
        for (k = 0; k < 3; k++) off[k] = -off[k];
    } else {                            /* relative, as ysp/gfx.h's video path */
        double yw = cx->rgb_to_xyz[3] + cx->rgb_to_xyz[4] + cx->rgb_to_xyz[5], xn[9], x2d[9];
        if (!(yw > 0.0)) return YCOL_ERR_RANGE;
        for (k = 0; k < 9; k++) xn[k] = cx->rgb_to_xyz[k] / yw;
        if (!ycol__inv3(xn, x2d)) return YCOL_ERR_RANGE;
        if (cx->desc.adapt == YCOL_ADAPT_BRADFORD) {
            double wd[3];
            ycol__display_white_rel(cx, wd);
            ycol__bradford(ycol__d65, wd, a);
            ycol__mm3(a, src, as);
            ycol__mm3(x2d, as, m);
        } else {
            ycol__mm3(x2d, src, m);
        }
    }
    return YCOL_OK;
}

YCOL_API int ycol_prim_to_rgb(const ycol_ctx* cx, int prim, double m[9], double off[3]) {
    double o[3];
    int rc;
    if (!cx || !m) return YCOL_ERR_ARG;
    rc = ycol__src_matrix(cx, prim, m, o);
    if (rc >= 0 && off) memcpy(off, o, sizeof o);
    return rc;
}

/* Transfer functions, sign kept (an encoded value below 0 or above 1 is a
 * color outside the source's gamut, not an error). */
static double ycol__srgb_decode(double v) {
    double a = fabs(v), r = a <= 0.04045 ? a / 12.92 : pow((a + 0.055) / 1.055, 2.4);
    return v < 0 ? -r : r;
}
static double ycol__srgb_encode(double v) {
    double a = fabs(v), r = a <= 0.0031308 ? 12.92 * a : 1.055 * pow(a, 1.0 / 2.4) - 0.055;
    return v < 0 ? -r : r;
}
/* BT.2020's OETF, inverted, as CSS Color 4 and colour-science decode it. */
#define YCOL__B2020_A 1.09929682680944
#define YCOL__B2020_B 0.018053968510807
static double ycol__b2020_decode(double v) {
    double a = fabs(v), r = a < YCOL__B2020_B * 4.5 ? a / 4.5 : pow((a + YCOL__B2020_A - 1.0) / YCOL__B2020_A, 1.0 / 0.45);
    return v < 0 ? -r : r;
}
static double ycol__b2020_encode(double v) {
    double a = fabs(v), r = a < YCOL__B2020_B ? 4.5 * a : YCOL__B2020_A * pow(a, 0.45) - (YCOL__B2020_A - 1.0);
    return v < 0 ? -r : r;
}

static int ycol__src_slot(int space) {
    return space == YCOL_SPACE_SRGB ? 0 : space == YCOL_SPACE_DISPLAY_P3 ? 1 : space == YCOL_SPACE_REC2020 ? 2 : -1;
}

/* --- context ---------------------------------------------------------------- */

static int ycol__lum_header(const ycol_lum* l);

/* The background's part: cone excitations, DKL, the levels and the id. */
static void ycol__ctx_bg(ycol_ctx* cx) {
    static const char* no_bg = "needs a background whose L, M and S excitations are all above 0";
    double raw[9], inv[9], col[3], pooled, resp[3], b[3];
    const double* w = cx->w;
    int j, k;
    cx->can &= ~YCOL_CAN_BG;
    cx->why_no_bg = (cx->can & YCOL_CAN_LMS) ? no_bg : cx->why_no_lms;
    memset(cx->bg_lms, 0, sizeof cx->bg_lms);
    memset(cx->dkl, 0, sizeof cx->dkl); memset(cx->dkl_inv, 0, sizeof cx->dkl_inv);
    memset(cx->rgb_to_dkl, 0, sizeof cx->rgb_to_dkl); memset(cx->dkl_to_rgb, 0, sizeof cx->dkl_to_rgb);
    if (!(cx->can & YCOL_CAN_LMS)) return;
    ycol__mul3(cx->rgb_to_lms, cx->bg, b);
    b[0] += cx->black_lms[0]; b[1] += cx->black_lms[1]; b[2] += cx->black_lms[2];
    memcpy(cx->bg_lms, b, sizeof b);
    if (!(b[0] > 0 && b[1] > 0 && b[2] > 0)) return;
    /* ComputeDKL_M as ycol_cal_dkl_matrix(), with the context's luminance;
     * with S's weight 0 every operation gives the same bits as that call. */
    raw[0] = w[0];  raw[1] = w[1];            raw[2] = w[2];
    raw[3] = 1.0;   raw[4] = -b[0] / b[1];    raw[5] = 0.0;
    raw[6] = -w[0]; raw[7] = -w[1];           raw[8] = (w[0] * b[0] + w[1] * b[1] + w[2] * b[2]) / b[2] - w[2];
    if (!ycol__inv3(raw, inv)) return;
    for (j = 0; j < 3; j++) {
        double s = 0.0;
        for (k = 0; k < 3; k++) { col[k] = inv[k * 3 + j]; s += (col[k] / b[k]) * (col[k] / b[k]); }
        pooled = sqrt(s);
        for (k = 0; k < 3; k++) col[k] /= pooled;
        ycol__mul3(raw, col, resp);
        for (k = 0; k < 3; k++) cx->dkl[j * 3 + k] = raw[j * 3 + k] / resp[j];
    }
    if (!ycol__inv3(cx->dkl, cx->dkl_inv)) return;
    ycol__mm3(cx->dkl, cx->rgb_to_lms, cx->rgb_to_dkl);
    if (!ycol__inv3(cx->rgb_to_dkl, cx->dkl_to_rgb)) return;
    cx->can |= YCOL_CAN_BG;
    cx->why_no_bg = NULL;
}

static void ycol__ctx_id(ycol_ctx* cx) {
    unsigned char buf[96];
    uint32_t lc = cx->desc.lum ? cx->desc.lum->crc : 0u, cc = cx->desc.cal ? cx->desc.cal->crc : 0u;
    size_t o = 0;
    memset(buf, 0, sizeof buf);
    memcpy(buf + o, &cc, 4); o += 4;
    memcpy(buf + o, &lc, 4); o += 4;
    memcpy(buf + o, &cx->desc.cones, 4); o += 4;
    memcpy(buf + o, &cx->desc.adapt, 4); o += 4;
    memcpy(buf + o, cx->bg, 24); o += 24;
    memcpy(buf + o, cx->desc.white, 24); o += 24;
    memcpy(buf + o, &cx->desc.src_white_Y, 8); o += 8;
    cx->id = ycol__crc32(buf, o);
}

/* The ctx's code and message; the message first, so a NULL-safe caller can
 * return the result. */
#define YCOL__CTX_FAIL(cx, rc, ...) \
    do { ycol__set_error((cx)->err, sizeof (cx)->err, __VA_ARGS__); (cx)->status = (rc); return false; } while (0)

YCOL_API bool ycol_ctx_open(ycol_ctx* cx, const ycol_ctx_desc* d) {
    static const double m1[9] = { 0.8189330101, 0.3618667424, -0.1288597137, 0.0329845436, 0.9293118715, 0.0361456387,
                                  0.0482003018, 0.2643662691, 0.6338517070 };
    const ycol_cal* c;
    int k, s;
    if (!cx) return false;
    memset(cx, 0, sizeof *cx);
    if (!d) YCOL__CTX_FAIL(cx, YCOL_ERR_ARG, "ysp_color: NULL desc");
    c = d->cal;
    if (!c) YCOL__CTX_FAIL(cx, YCOL_ERR_ARG, "ysp_color: desc.cal is required (ycol_cal_nominal() states a nominal one)");
    if (memcmp(c->magic, ycol__cal_magic, 8) != 0 || c->version != 1 || c->bytes != (uint32_t)sizeof *c)
        YCOL__CTX_FAIL(cx, YCOL_ERR_FORMAT, "ysp_color: not a canonical calibration (magic, version 1, %u bytes)",
                       (unsigned)sizeof *c);
    if (c->crc == 0 || c->crc != ycol__crc32(c, offsetof(ycol_cal, crc)))
        YCOL__CTX_FAIL(cx, YCOL_ERR_FORMAT, "ysp_color: the calibration is not sealed or its CRC fails: seal it with "
                       "ycol_cal_derive() or ycol_cal_save() after the last change, or load it again");
    if (!(d->cones == YCOL_CONES_SS2 || d->cones == YCOL_CONES_SS10) ||
        !(d->adapt == YCOL_ADAPT_NONE || d->adapt == YCOL_ADAPT_BRADFORD))
        YCOL__CTX_FAIL(cx, YCOL_ERR_ARG, "ysp_color: desc.cones or desc.adapt is not a known value");
    if (!ycol__finite3(d->background) || !ycol__finite3(d->white) || !ycol__finite(d->src_white_Y) || d->src_white_Y < 0.0)
        YCOL__CTX_FAIL(cx, YCOL_ERR_ARG, "ysp_color: a desc value is not finite, or src_white_Y is below 0");
    for (k = 0; k < 3; k++)
        if (!(d->background[k] >= 0.0 && d->background[k] <= 1.0))
            YCOL__CTX_FAIL(cx, YCOL_ERR_RANGE, "ysp_color: desc.background[%d] = %g is outside 0..1", k, d->background[k]);
    if ((d->white[0] != 0.0 || d->white[1] != 0.0 || d->white[2] != 0.0) && !(d->white[1] > 0.0))
        YCOL__CTX_FAIL(cx, YCOL_ERR_RANGE, "ysp_color: desc.white needs Y above 0 (or all 0 for the display's white)");
    if (d->lum) {
        int rc = ycol__lum_header(d->lum);
        if (rc < 0) YCOL__CTX_FAIL(cx, rc, "ysp_color: desc.lum is not a sealed participant luminance record");
        if (d->lum->cones != d->cones)
            YCOL__CTX_FAIL(cx, YCOL_ERR_REFUSED, "ysp_color: desc.lum is in %s cone units, the context in %s",
                           d->lum->cones == YCOL_CONES_SS10 ? "SS10" : "SS2", d->cones == YCOL_CONES_SS10 ? "SS10" : "SS2");
    }
    cx->desc = *d;
    cx->can = YCOL_CAN_RGB;
    for (k = 0; k < 3; k++) cx->bg[k] = d->background[k];
    if (d->lum) memcpy(cx->w, d->lum->w, sizeof cx->w);
    else ycol_cone_luminance(d->cones, cx->w);

    /* XYZ */
    cx->why_no_xyz = "needs the readings' chromaticities (x, y) at level 1 for each gun, and for black when black has light; "
                     "this calibration has none";
    memcpy(cx->rgb_to_xyz, c->rgb_to_xyz, sizeof cx->rgb_to_xyz);
    memcpy(cx->black_xyz, c->black_xyz, sizeof cx->black_xyz);
    if ((c->flags & YCOL_CAL_HAS_XY) && c->rgb_to_xyz[4] != 0.0) {
        if (ycol__inv3(cx->rgb_to_xyz, cx->xyz_to_rgb)) { cx->can |= YCOL_CAN_XYZ; cx->why_no_xyz = NULL; }
        else cx->why_no_xyz = "the calibration's RGB to XYZ matrix is singular";
    }
    if (cx->can & YCOL_CAN_XYZ) {
        const double* m = cx->rgb_to_xyz;
        double xn[9], yw, bk[3];
        if (d->white[1] > 0.0) memcpy(cx->white_xyz, d->white, sizeof cx->white_xyz);
        else for (k = 0; k < 3; k++) cx->white_xyz[k] = m[k * 3] + m[k * 3 + 1] + m[k * 3 + 2] + cx->black_xyz[k];
        /* Oklab: ysp/gfx.h v0.3's matrices when the black is 0 and the white
         * the display's; the black and an explicit white are new. */
        yw = cx->white_xyz[1];
        for (k = 0; k < 9; k++) xn[k] = m[k] / yw;
        for (k = 0; k < 3; k++) bk[k] = cx->black_xyz[k] / yw;
        if (d->adapt == YCOL_ADAPT_BRADFORD) {
            double ws[3] = { cx->white_xyz[0] / yw, 1.0, cx->white_xyz[2] / yw }, a[9], am[9], ab[3];
            ycol__bradford(ws, ycol__d65, a);
            ycol__mm3(a, xn, am); memcpy(xn, am, sizeof xn);
            ycol__mul3(a, bk, ab); memcpy(bk, ab, sizeof bk);
        }
        ycol__mm3(m1, xn, cx->ok_rgb_to_lms);
        ycol__mul3(m1, bk, cx->ok_black);
        if (!ycol__inv3(cx->ok_rgb_to_lms, cx->ok_lms_to_rgb)) {
            cx->can &= ~YCOL_CAN_XYZ;
            cx->why_no_xyz = "the calibration's Oklab matrix is singular";
        }
        for (s = 0; s < 3 && (cx->can & YCOL_CAN_XYZ); s++) {
            static const int prim[3] = { YCOL_PRIM_BT709, YCOL_PRIM_P3, YCOL_PRIM_BT2020 };
            if (ycol__src_matrix(cx, prim[s], cx->src_to_rgb[s], cx->src_off[s]) < 0 ||
                !ycol__inv3(cx->src_to_rgb[s], cx->rgb_to_src[s])) {
                cx->can &= ~YCOL_CAN_XYZ;
                cx->why_no_xyz = "the matrix from a D65 source to this display is singular";
            }
        }
    }

    /* LMS */
    cx->why_no_lms = "needs the primaries' spectra (ycol_cal_set_spectra); without them there is no cone space, "
                     "and no CIE 1931 to cone approximation is made";
    if (c->flags & YCOL_CAL_HAS_SPECTRA) {
        int ok;
        if (d->cones == YCOL_CONES_SS2) {
            memcpy(cx->rgb_to_lms, c->rgb_to_lms, sizeof cx->rgb_to_lms);
            memcpy(cx->black_lms, c->black_lms, sizeof cx->black_lms);
            ok = c->rgb_to_lms[0] != 0.0 || c->rgb_to_lms[4] != 0.0;
        } else {
            ok = ycol__integrate(c, d->cones, cx->rgb_to_lms, cx->black_lms);
        }
        if (ok && ycol__inv3(cx->rgb_to_lms, cx->lms_to_rgb)) { cx->can |= YCOL_CAN_LMS; cx->why_no_lms = NULL; }
        else if (ok) cx->why_no_lms = "the calibration's RGB to LMS matrix is singular";
    }
    {   /* MacLeod-Boynton: the largest S / V on the spectrum locus becomes 1 */
        const float (*t)[3] = ycol__table(d->cones);
        double ws[3], best = 0.0;
        int i;
        ycol_cone_luminance(d->cones, ws);
        for (i = 0; i < 441; i++) {
            double v = ws[0] * t[i][0] + ws[1] * t[i][1];
            if (v > 0.0 && t[i][2] / v > best) best = t[i][2] / v;
        }
        cx->mb_k = best > 0.0 ? 1.0 / best : 0.0;
    }
    ycol__ctx_bg(cx);
    ycol__ctx_id(cx);
    return true;
}

YCOL_API void ycol_ctx_close(ycol_ctx* cx) {
    if (cx) memset(cx, 0, sizeof *cx);
}

YCOL_API const char* ycol_ctx_error(const ycol_ctx* cx) {
    return cx ? cx->err : "";
}

/* The message, then the code; returns the code. */
static int ycol__ctx_set_fail(ycol_ctx* cx, int rc, const char* what) {
    ycol__set_error(cx->err, sizeof cx->err, "ysp_color: %s: %s", what, ycol_strerror(rc));
    cx->status = rc;
    return rc;
}

YCOL_API int ycol_ctx_set_background(ycol_ctx* cx, const double bg[3]) {
    int k;
    if (!cx) return YCOL_ERR_ARG;
    if (!cx->desc.cal) return ycol__ctx_set_fail(cx, YCOL_ERR_ARG, "set_background on a context that is not open");
    if (!bg || !ycol__finite3(bg)) return ycol__ctx_set_fail(cx, YCOL_ERR_ARG, "set_background: NULL or not finite");
    for (k = 0; k < 3; k++)
        if (!(bg[k] >= 0.0 && bg[k] <= 1.0)) return ycol__ctx_set_fail(cx, YCOL_ERR_RANGE, "set_background: outside 0..1");
    for (k = 0; k < 3; k++) cx->bg[k] = cx->desc.background[k] = bg[k];
    ycol__ctx_bg(cx);
    ycol__ctx_id(cx);
    return YCOL_OK;
}

YCOL_API int ycol_ctx_describe(const ycol_ctx* cx, char* buf, size_t cap) {
    const ycol_cal* c;
    int n;
    char src[192], wh[192], lum[320];
    if (!cx || !buf || cap == 0 || !cx->desc.cal) return YCOL_ERR_ARG;
    c = cx->desc.cal;
    if (cx->desc.src_white_Y > 0.0) snprintf(src, sizeof src, "D65 sources absolute (white %.6g cd/m2)", cx->desc.src_white_Y);
    else snprintf(src, sizeof src, "D65 sources relative to the display (white to its white Y, black to its black)");
    if (cx->desc.white[1] > 0.0) snprintf(wh, sizeof wh, "white XYZ %.6g %.6g %.6g (stated)", cx->white_xyz[0], cx->white_xyz[1], cx->white_xyz[2]);
    else if (cx->can & YCOL_CAN_XYZ) snprintf(wh, sizeof wh, "white XYZ %.6g %.6g %.6g (the display's)", cx->white_xyz[0], cx->white_xyz[1], cx->white_xyz[2]);
    else snprintf(wh, sizeof wh, "no white (no xy)");
    if (cx->desc.lum)
        snprintf(lum, sizeof lum, "luminance of \"%.40s\" (%s%s, lum %08x): %.8g L + %.8g M + %.8g S", cx->desc.lum->participant,
                 cx->desc.lum->flags & YCOL_LUM_STATED ? "STATED" : "fitted",
                 cx->desc.lum->method == YCOL_LUM_HFP ? ", HFP" : cx->desc.lum->method == YCOL_LUM_MIN_MOTION ? ", minimum motion" :
                 cx->desc.lum->method == YCOL_LUM_MIN_BORDER ? ", minimally distinct border" : "",
                 (unsigned)cx->desc.lum->crc, cx->w[0], cx->w[1], cx->w[2]);
    else snprintf(lum, sizeof lum, "standard luminance %.8g L + %.8g M", cx->w[0], cx->w[1]);
    n = snprintf(buf, cap, "ysp_color %s: calibration %08x%s%s, cones %s, %s, background %.6g %.6g %.6g, %s, %s, %s, "
                 "can%s%s%s%s, id %08x",
                 YCOL_VERSION_STRING, (unsigned)c->crc, c->flags & YCOL_CAL_NOMINAL ? " NOMINAL" : "",
                 c->flags & YCOL_CAL_SPECTRA_NOMINAL ? " SPECTRA_NOMINAL" : "",
                 cx->desc.cones == YCOL_CONES_SS10 ? "SS10" : "SS2", lum, cx->bg[0], cx->bg[1], cx->bg[2], wh, src,
                 cx->desc.adapt == YCOL_ADAPT_BRADFORD ? "Bradford to the display's white" : "no chromatic adaptation",
                 " RGB", cx->can & YCOL_CAN_XYZ ? " XYZ" : "", cx->can & YCOL_CAN_LMS ? " LMS" : "",
                 cx->can & YCOL_CAN_BG ? " BG" : "", (unsigned)cx->id);
    return n < 0 ? YCOL_ERR_ARG : ((size_t)n >= cap ? YCOL_ERR_FULL : n);
}

/* --- conversions ------------------------------------------------------------ */

static int ycol__family(int space) {   /* 0 device, 1 XYZ, 2 cones */
    if (space >= YCOL_SPACE_XYZ && space <= YCOL_SPACE_REC2020) return 1;
    if (space >= YCOL_SPACE_LMS && space <= YCOL_SPACE_MB) return 2;
    return 0;
}

static int ycol__need(const ycol_ctx* cx, int space, const char** why) {
    if (space == YCOL_SPACE_NONE) { *why = "no space: a zeroed color, or a string ycol_hex() could not read"; return YCOL_ERR_REFUSED; }
    if (space < 0 || space >= YCOL_SPACE_COUNT) { *why = "not a known space"; return YCOL_ERR_ARG; }
    if (ycol__family(space) == 1 && !(cx->can & YCOL_CAN_XYZ)) { *why = cx->why_no_xyz; return YCOL_ERR_REFUSED; }
    if (ycol__family(space) == 2 && !(cx->can & YCOL_CAN_LMS)) { *why = cx->why_no_lms; return YCOL_ERR_REFUSED; }
    if ((space == YCOL_SPACE_CONE || space == YCOL_SPACE_DKL || space == YCOL_SPACE_DKL_CART) && !(cx->can & YCOL_CAN_BG)) {
        *why = cx->why_no_bg; return YCOL_ERR_REFUSED;
    }
    return YCOL_OK;
}

/* Ottosson's M2: the cube roots of the cone response to Lab. */
static const double ycol__ok_m2[9] = { 0.2104542553, 0.7936177850, -0.0040720468, 1.9779984951, -2.4285922050, 0.4505937099,
                                         0.0259040371, 0.7827717662, -0.8086757660 };

/* CIE 1976 lightness function and its inverse, with the CIE's exact
 * constants (epsilon 216/24389, kappa 24389/27), as colour-science. */
#define YCOL__CIE_E (216.0 / 24389.0)
#define YCOL__CIE_K (24389.0 / 27.0)
static double ycol__lab_f(double t) { return t > YCOL__CIE_E ? cbrt(t) : (YCOL__CIE_K * t + 16.0) / 116.0; }
static double ycol__lab_finv(double f) { double f3 = f * f * f; return f3 > YCOL__CIE_E ? f3 : (116.0 * f - 16.0) / YCOL__CIE_K; }

static void ycol__to_polar(const double* in, double* out, uint32_t* flags) {
    double C = sqrt(in[1] * in[1] + in[2] * in[2]), h;
    out[0] = in[0];
    out[1] = C;
    if (C < 1e-12) { out[2] = 0.0; *flags |= YCOL_F_ACHROMATIC; return; }
    h = atan2(in[2], in[1]) / YCOL__D2R;
    if (h < 0.0) h += 360.0;
    if (h >= 360.0) h -= 360.0;
    out[2] = h;
}

static void ycol__from_polar(const double* in, double* out) {
    double h = in[2] * YCOL__D2R;
    out[0] = in[0];
    out[1] = in[1] * cos(h);
    out[2] = in[1] * sin(h);
}

static void ycol__dkl_cart(const double* sph, double* cart) {
    const double e = sph[0] * YCOL__D2R, a = sph[1] * YCOL__D2R, r = sph[2];
    cart[0] = r * sin(e);
    cart[1] = r * cos(e) * cos(a);
    cart[2] = r * cos(e) * sin(a);
}

/* An increment in linear RGB for the contrast spaces, computed as
 * ycol_cal_dir_cone() and ycol_cal_dir_dkl() do. */
static void ycol__incr(const ycol_ctx* cx, int space, const double* v, double* d) {
    double t[3], u[3];
    if (space == YCOL_SPACE_CONE) {
        t[0] = v[0] * cx->bg_lms[0]; t[1] = v[1] * cx->bg_lms[1]; t[2] = v[2] * cx->bg_lms[2];
        ycol__mul3(cx->lms_to_rgb, t, d);
        return;
    }
    if (space == YCOL_SPACE_DKL) { ycol__dkl_cart(v, u); v = u; }
    ycol__mul3(cx->dkl_inv, v, t);
    ycol__mul3(cx->lms_to_rgb, t, d);
}

static int ycol__is_incr(int space) {
    return space == YCOL_SPACE_CONE || space == YCOL_SPACE_DKL || space == YCOL_SPACE_DKL_CART;
}

/* The device value of a linear value through the stored CLUT, extrapolated
 * past 0..1 by the end segments. */
static double ycol__lut_fwd(const ycol_cal* c, int gun, double v) {
    int n = c->lut_n ? c->lut_n : YCOL_CAL_MAX_LUT, i0;
    const float* l = c->lut[gun];
    double x = v * (n - 1);
    i0 = x <= 0.0 ? 0 : (x >= n - 2 ? n - 2 : (int)x);
    return l[i0] + ((double)l[i0 + 1] - l[i0]) * (x - i0);
}

/* The linear value whose CLUT entry is device value y: the CLUT's inverse
 * by linear interpolation, so the output stage writes y's code back. */
static double ycol__lut_inv(const ycol_cal* c, int gun, double y) {
    int n = c->lut_n ? c->lut_n : YCOL_CAL_MAX_LUT, lo = 0, hi;
    const float* l = c->lut[gun];
    if (y <= l[0]) return 0.0;
    if (y >= l[n - 1]) return 1.0;
    hi = n - 1;
    while (hi - lo > 1) { int mid = (lo + hi) / 2; if (l[mid] <= y) lo = mid; else hi = mid; }
    if (!(l[hi] > l[lo])) return (double)lo / (n - 1);
    return (lo + (y - l[lo]) / ((double)l[hi] - l[lo])) / (n - 1);
}

static int ycol__to_rgb3(const ycol_ctx* cx, const ycol_color* c, double* rgb, const char** why) {
    const double* v = c->u.v;
    double xyz[3], t[3], u[3];
    int rc = ycol__need(cx, c->space, why), k, s;
    if (rc < 0) return rc;
    if (!ycol__finite3(v)) { *why = "a value is not finite"; return YCOL_ERR_ARG; }
    switch (c->space) {
    case YCOL_SPACE_RGB:
        memcpy(rgb, v, 3 * sizeof(double));
        return YCOL_OK;
    case YCOL_SPACE_DEVICE:
        for (k = 0; k < 3; k++) {
            if (!(v[k] >= 0.0 && v[k] <= 1.0)) { *why = "a device value is outside 0..1"; return YCOL_ERR_RANGE; }
            rgb[k] = ycol__lut_inv(cx->desc.cal, k, v[k]);
        }
        return YCOL_OK;
    case YCOL_SPACE_XYZ:
        memcpy(xyz, v, sizeof xyz);
        break;
    case YCOL_SPACE_XYY:
        if (v[1] == 0.0) {
            if (v[2] != 0.0) { *why = "xyY with y = 0 and Y above 0"; return YCOL_ERR_RANGE; }
            xyz[0] = xyz[1] = xyz[2] = 0.0;
        } else {
            ycol__xy_to_xyz(v[0], v[1], v[2], xyz);
        }
        break;
    case YCOL_SPACE_CIELAB:
    case YCOL_SPACE_CIELCH: {
        double lab[3], fy;
        if (c->space == YCOL_SPACE_CIELCH) ycol__from_polar(v, lab); else memcpy(lab, v, sizeof lab);
        fy = (lab[0] + 16.0) / 116.0;
        xyz[0] = cx->white_xyz[0] * ycol__lab_finv(fy + lab[1] / 500.0);
        xyz[1] = cx->white_xyz[1] * (lab[0] > YCOL__CIE_K * YCOL__CIE_E ? fy * fy * fy : lab[0] / YCOL__CIE_K);
        xyz[2] = cx->white_xyz[2] * ycol__lab_finv(fy - lab[2] / 200.0);
        break;
    }
    case YCOL_SPACE_CIELUV:
    case YCOL_SPACE_CIELCHUV: {
        const double* w = cx->white_xyz;
        double luv[3], dn = w[0] + 15.0 * w[1] + 3.0 * w[2], un = 4.0 * w[0] / dn, vn = 9.0 * w[1] / dn, Y, up, vp;
        if (c->space == YCOL_SPACE_CIELCHUV) ycol__from_polar(v, luv); else memcpy(luv, v, sizeof luv);
        if (luv[0] == 0.0) { xyz[0] = xyz[1] = xyz[2] = 0.0; break; }
        Y = w[1] * (luv[0] > YCOL__CIE_K * YCOL__CIE_E ? pow((luv[0] + 16.0) / 116.0, 3.0) : luv[0] / YCOL__CIE_K);
        up = luv[1] / (13.0 * luv[0]) + un;
        vp = luv[2] / (13.0 * luv[0]) + vn;
        if (vp == 0.0) { *why = "CIELUV with v' = 0"; return YCOL_ERR_RANGE; }
        xyz[1] = Y;
        xyz[0] = Y * 9.0 * up / (4.0 * vp);
        xyz[2] = Y * (12.0 - 3.0 * up - 20.0 * vp) / (4.0 * vp);
        break;
    }
    case YCOL_SPACE_OKLAB:
    case YCOL_SPACE_OKLCH: {
        double lab[3], l[3];
        if (c->space == YCOL_SPACE_OKLCH) ycol__from_polar(v, lab); else memcpy(lab, v, sizeof lab);
        double mi[9];
        /* M2's exact inverse: Ottosson's printed one (ysp/gfx.h's shader)
         * is 10 digits and leaves 2.6e-7 in a round trip */
        ycol__inv3(ycol__ok_m2, mi);
        ycol__mul3(mi, lab, l);
        for (k = 0; k < 3; k++) l[k] = l[k] * l[k] * l[k] - cx->ok_black[k];
        ycol__mul3(cx->ok_lms_to_rgb, l, rgb);
        return YCOL_OK;
    }
    case YCOL_SPACE_SRGB:
    case YCOL_SPACE_DISPLAY_P3:
    case YCOL_SPACE_REC2020:
        s = ycol__src_slot(c->space);
        for (k = 0; k < 3; k++) t[k] = c->space == YCOL_SPACE_REC2020 ? ycol__b2020_decode(v[k]) : ycol__srgb_decode(v[k]);
        ycol__mul3(cx->src_to_rgb[s], t, rgb);
        for (k = 0; k < 3; k++) rgb[k] += cx->src_off[s][k];
        return YCOL_OK;
    case YCOL_SPACE_LMS:
        for (k = 0; k < 3; k++) t[k] = v[k] - cx->black_lms[k];
        ycol__mul3(cx->lms_to_rgb, t, rgb);
        return YCOL_OK;
    case YCOL_SPACE_CONE:
    case YCOL_SPACE_DKL:
    case YCOL_SPACE_DKL_CART:
        ycol__incr(cx, c->space, v, u);
        for (k = 0; k < 3; k++) rgb[k] = cx->bg[k] + u[k];
        return YCOL_OK;
    case YCOL_SPACE_MB: {
        const double* w = cx->w;
        double V = v[2], L, S;
        if (V < 0.0) { *why = "MacLeod-Boynton luminance below 0"; return YCOL_ERR_RANGE; }
        if (!(w[0] != 0.0) || !(w[1] != 0.0) || !(cx->mb_k > 0.0)) { *why = "a luminance without an M weight"; return YCOL_ERR_RANGE; }
        L = v[0] * V / w[0];
        S = v[1] * V / cx->mb_k;
        t[0] = L; t[2] = S; t[1] = (V - w[0] * L - w[2] * S) / w[1];
        for (k = 0; k < 3; k++) t[k] -= cx->black_lms[k];
        ycol__mul3(cx->lms_to_rgb, t, rgb);
        return YCOL_OK;
    }
    default:
        *why = "not a known space";
        return YCOL_ERR_ARG;
    }
    for (k = 0; k < 3; k++) t[k] = xyz[k] - cx->black_xyz[k];
    ycol__mul3(cx->xyz_to_rgb, t, rgb);
    return YCOL_OK;
}

static int ycol__from_rgb3(const ycol_ctx* cx, const double* rgb, int space, ycol_color* out, const char** why) {
    double xyz[3], t[3], d[3];
    double* o = out->u.v;
    int rc = ycol__need(cx, space, why), k, s;
    memset(out, 0, sizeof *out);
    if (rc < 0) return rc;
    if (!ycol__finite3(rgb)) { *why = "a value is not finite"; return YCOL_ERR_ARG; }
    out->space = space;
    if (ycol__family(space) == 1 && space != YCOL_SPACE_OKLAB && space != YCOL_SPACE_OKLCH && ycol__src_slot(space) < 0) {
        ycol__mul3(cx->rgb_to_xyz, rgb, xyz);
        for (k = 0; k < 3; k++) xyz[k] += cx->black_xyz[k];
    }
    switch (space) {
    case YCOL_SPACE_RGB:
        memcpy(o, rgb, 3 * sizeof(double));
        break;
    case YCOL_SPACE_DEVICE:
        for (k = 0; k < 3; k++) o[k] = ycol__lut_fwd(cx->desc.cal, k, rgb[k]);
        break;
    case YCOL_SPACE_XYZ:
        memcpy(o, xyz, sizeof xyz);
        break;
    case YCOL_SPACE_XYY: {
        double sum = xyz[0] + xyz[1] + xyz[2];
        if (sum == 0.0) {
            const double* w = cx->white_xyz;
            double ws = w[0] + w[1] + w[2];
            o[0] = w[0] / ws; o[1] = w[1] / ws; o[2] = 0.0;
            out->flags |= YCOL_F_ACHROMATIC;
        } else {
            o[0] = xyz[0] / sum; o[1] = xyz[1] / sum; o[2] = xyz[1];
        }
        break;
    }
    case YCOL_SPACE_CIELAB:
    case YCOL_SPACE_CIELCH: {
        double fx = ycol__lab_f(xyz[0] / cx->white_xyz[0]), fy = ycol__lab_f(xyz[1] / cx->white_xyz[1]),
               fz = ycol__lab_f(xyz[2] / cx->white_xyz[2]), lab[3];
        lab[0] = 116.0 * fy - 16.0; lab[1] = 500.0 * (fx - fy); lab[2] = 200.0 * (fy - fz);
        if (space == YCOL_SPACE_CIELCH) ycol__to_polar(lab, o, &out->flags); else memcpy(o, lab, sizeof lab);
        break;
    }
    case YCOL_SPACE_CIELUV:
    case YCOL_SPACE_CIELCHUV: {
        const double* w = cx->white_xyz;
        double dn = w[0] + 15.0 * w[1] + 3.0 * w[2], un = 4.0 * w[0] / dn, vn = 9.0 * w[1] / dn;
        double den = xyz[0] + 15.0 * xyz[1] + 3.0 * xyz[2], up = un, vp = vn, luv[3];
        if (den != 0.0) { up = 4.0 * xyz[0] / den; vp = 9.0 * xyz[1] / den; }
        luv[0] = 116.0 * ycol__lab_f(xyz[1] / w[1]) - 16.0;
        luv[1] = 13.0 * luv[0] * (up - un);
        luv[2] = 13.0 * luv[0] * (vp - vn);
        if (space == YCOL_SPACE_CIELCHUV) ycol__to_polar(luv, o, &out->flags); else memcpy(o, luv, sizeof luv);
        break;
    }
    case YCOL_SPACE_OKLAB:
    case YCOL_SPACE_OKLCH: {
        double l[3], lab[3];
        ycol__mul3(cx->ok_rgb_to_lms, rgb, l);
        for (k = 0; k < 3; k++) l[k] = cbrt(l[k] + cx->ok_black[k]);
        ycol__mul3(ycol__ok_m2, l, lab);
        if (space == YCOL_SPACE_OKLCH) ycol__to_polar(lab, o, &out->flags); else memcpy(o, lab, sizeof lab);
        break;
    }
    case YCOL_SPACE_SRGB:
    case YCOL_SPACE_DISPLAY_P3:
    case YCOL_SPACE_REC2020:
        s = ycol__src_slot(space);
        for (k = 0; k < 3; k++) t[k] = rgb[k] - cx->src_off[s][k];
        ycol__mul3(cx->rgb_to_src[s], t, d);
        for (k = 0; k < 3; k++) o[k] = space == YCOL_SPACE_REC2020 ? ycol__b2020_encode(d[k]) : ycol__srgb_encode(d[k]);
        break;
    case YCOL_SPACE_LMS:
        ycol__mul3(cx->rgb_to_lms, rgb, o);
        for (k = 0; k < 3; k++) o[k] += cx->black_lms[k];
        break;
    case YCOL_SPACE_CONE:
    case YCOL_SPACE_DKL:
    case YCOL_SPACE_DKL_CART:
        for (k = 0; k < 3; k++) t[k] = rgb[k] - cx->bg[k];
        ycol__mul3(cx->rgb_to_lms, t, d);
        if (space == YCOL_SPACE_CONE) {
            for (k = 0; k < 3; k++) o[k] = d[k] / cx->bg_lms[k];
        } else {
            double cart[3], r;
            ycol__mul3(cx->dkl, d, cart);
            if (space == YCOL_SPACE_DKL_CART) { memcpy(o, cart, sizeof cart); break; }
            r = sqrt(cart[0] * cart[0] + cart[1] * cart[1] + cart[2] * cart[2]);
            o[2] = r;
            if (r == 0.0) { o[0] = o[1] = 0.0; out->flags |= YCOL_F_ACHROMATIC; break; }
            o[0] = asin(cart[0] / r < -1.0 ? -1.0 : (cart[0] / r > 1.0 ? 1.0 : cart[0] / r)) / YCOL__D2R;
            if (sqrt(cart[1] * cart[1] + cart[2] * cart[2]) <= 1e-12 * r) { o[1] = 0.0; out->flags |= YCOL_F_ACHROMATIC; break; }
            o[1] = atan2(cart[2], cart[1]) / YCOL__D2R;
            if (o[1] < 0.0) o[1] += 360.0;
            if (o[1] >= 360.0) o[1] -= 360.0;
        }
        break;
    case YCOL_SPACE_MB: {
        const double* w = cx->w;
        double lms[3], V;
        ycol__mul3(cx->rgb_to_lms, rgb, lms);
        for (k = 0; k < 3; k++) lms[k] += cx->black_lms[k];
        V = w[0] * lms[0] + w[1] * lms[1] + w[2] * lms[2];
        if (!(V > 0.0)) { memset(out, 0, sizeof *out); *why = "MacLeod-Boynton needs a luminance above 0"; return YCOL_ERR_RANGE; }
        o[0] = w[0] * lms[0] / V; o[1] = cx->mb_k * lms[2] / V; o[2] = V;
        break;
    }
    default:
        memset(out, 0, sizeof *out);
        *why = "not a known space";
        return YCOL_ERR_ARG;
    }
    return YCOL_OK;
}

/* The gamut of rgb about the background; sym: of bg + d and bg - d. */
static void ycol__gamut(const ycol_ctx* cx, const double* rgb, const double* d, int sym, ycol_gamut* g) {
    double mn = HUGE_VAL, sc = HUGE_VAL;
    int k;
    g->status = YCOL_OK;
    g->why = NULL;
    g->below = g->above = 0;
    g->in = 1;
    for (k = 0; k < 3; k++) {
        double bg = cx->bg[k], a, m, lo, hi;
        if (sym) {
            a = fabs(d[k]);
            lo = bg - a; hi = bg + a;
            m = (bg < 1.0 - bg ? bg : 1.0 - bg) - a;
            if (a > 0.0) { double r = (bg < 1.0 - bg ? bg : 1.0 - bg) / a; if (r < sc) sc = r; }
        } else {
            lo = hi = rgb[k];
            m = rgb[k] < 1.0 - rgb[k] ? rgb[k] : 1.0 - rgb[k];
            if (d[k] > 0.0) { double r = (1.0 - bg) / d[k]; if (r < sc) sc = r; }
            else if (d[k] < 0.0) { double r = bg / -d[k]; if (r < sc) sc = r; }
        }
        g->margin[k] = m;
        if (m < mn) mn = m;
        if (lo < 0.0) g->below |= 1u << k;
        if (hi > 1.0) g->above |= 1u << k;
        if (lo < -YCOL_GAMUT_EPS || hi > 1.0 + YCOL_GAMUT_EPS) g->in = 0;
    }
    g->distance = mn < 0.0 ? -mn : 0.0;
    g->scale = sc;
    g->kept = 1.0;
}

static void ycol__refuse(ycol_gamut* g, int rc, const char* why, double* rgb) {
    rgb[0] = rgb[1] = rgb[2] = ycol__nan();
    if (!g) return;
    memset(g, 0, sizeof *g);
    g->status = rc;
    g->why = why;
    g->margin[0] = g->margin[1] = g->margin[2] = g->distance = g->scale = g->kept = ycol__nan();
}

static ycol_rgb ycol__rgb(const double* v) {
    ycol_rgb r;
    r.r = v[0]; r.g = v[1]; r.b = v[2];
    return r;
}

YCOL_API ycol_rgb ycol_to_rgb(const ycol_ctx* cx, ycol_color c, ycol_gamut* g) {
    double rgb[3], d[3];
    const char* why = "NULL context";
    int rc = cx ? ycol__to_rgb3(cx, &c, rgb, &why) : YCOL_ERR_ARG, k;
    if (rc < 0) { ycol__refuse(g, rc, why, rgb); return ycol__rgb(rgb); }
    if (g) {
        for (k = 0; k < 3; k++) d[k] = rgb[k] - cx->bg[k];
        ycol__gamut(cx, rgb, d, 0, g);
    }
    return ycol__rgb(rgb);
}

YCOL_API ycol_rgb ycol_to_dir(const ycol_ctx* cx, ycol_color c, ycol_gamut* g) {
    double rgb[3], d[3];
    const char* why = "NULL context";
    int rc, k;
    if (!cx) { ycol__refuse(g, YCOL_ERR_ARG, why, d); return ycol__rgb(d); }
    if (ycol__is_incr(c.space)) {
        rc = ycol__need(cx, c.space, &why);
        if (rc >= 0 && !ycol__finite3(c.u.v)) { rc = YCOL_ERR_ARG; why = "a value is not finite"; }
        if (rc < 0) { ycol__refuse(g, rc, why, d); return ycol__rgb(d); }
        ycol__incr(cx, c.space, c.u.v, d);
    } else {
        rc = ycol__to_rgb3(cx, &c, rgb, &why);
        if (rc < 0) { ycol__refuse(g, rc, why, d); return ycol__rgb(d); }
        for (k = 0; k < 3; k++) d[k] = rgb[k] - cx->bg[k];
    }
    if (g) ycol__gamut(cx, d, d, 1, g);
    return ycol__rgb(d);
}

YCOL_API ycol_color ycol_from_rgb(const ycol_ctx* cx, ycol_rgb rgb, int space, int* status) {
    ycol_color out;
    double v[3];
    const char* why = NULL;
    int rc;
    v[0] = rgb.r; v[1] = rgb.g; v[2] = rgb.b;
    if (!cx) { memset(&out, 0, sizeof out); rc = YCOL_ERR_ARG; }
    else rc = ycol__from_rgb3(cx, v, space, &out, &why);
    if (status) *status = rc;
    return out;
}

YCOL_API int ycol_convert(const ycol_ctx* cx, ycol_color in, int space, ycol_color* out, ycol_gamut* g) {
    double rgb[3], d[3];
    const char* why = "NULL argument";
    int rc, k, fi, fo;
    if (!cx || !out) { if (out) memset(out, 0, sizeof *out); ycol__refuse(g, YCOL_ERR_ARG, why, rgb); return YCOL_ERR_ARG; }
    rc = ycol__to_rgb3(cx, &in, rgb, &why);
    if (rc >= 0) rc = ycol__from_rgb3(cx, rgb, space, out, &why);
    if (rc < 0) { memset(out, 0, sizeof *out); ycol__refuse(g, rc, why, rgb); return rc; }
    fi = ycol__family(in.space); fo = ycol__family(space);
    if (fi && fo && fi != fo) out->flags |= YCOL_F_VIA_DEVICE;
    if (g) {
        for (k = 0; k < 3; k++) d[k] = rgb[k] - cx->bg[k];
        ycol__gamut(cx, rgb, d, 0, g);
    }
    return YCOL_OK;
}

YCOL_API long ycol_convert_n(const ycol_ctx* cx, int from, const double* in, size_t in_stride, int to,
                                 double* out, size_t out_stride, size_t n, uint8_t* flags) {
    size_t i;
    if (!cx || (n && (!in || !out))) return YCOL_ERR_ARG;
    if (from <= YCOL_SPACE_NONE || from >= YCOL_SPACE_COUNT || to <= YCOL_SPACE_NONE || to >= YCOL_SPACE_COUNT)
        return YCOL_ERR_ARG;
    if (in_stride == 0) in_stride = 3;
    if (out_stride == 0) out_stride = 3;
    if (in_stride < 3 || out_stride < 3) return YCOL_ERR_ARG;
    for (i = 0; i < n; i++) {
        const double* a = in + i * in_stride;
        double* b = out + i * out_stride;
        ycol_color c, r;
        ycol_gamut g;
        int rc;
        c = ycol_make(from, a[0], a[1], a[2]);
        rc = ycol_convert(cx, c, to, &r, &g);
        if (rc < 0) { b[0] = b[1] = b[2] = ycol__nan(); if (flags) flags[i] = 2u; continue; }
        b[0] = r.u.v[0]; b[1] = r.u.v[1]; b[2] = r.u.v[2];
        if (flags) flags[i] = (uint8_t)(g.in ? 1u : 0u);
    }
    return (long)n;
}

/* --- gamut questions -------------------------------------------------------- */

static double ycol__k(const ycol_ctx* cx, const double* d, int sides) {
    double best = HUGE_VAL;
    int k;
    for (k = 0; k < 3; k++) {
        double bg = cx->bg[k], r;
        if (d[k] == 0.0) continue;
        if (sides == YCOL_SYMMETRIC) r = (bg < 1.0 - bg ? bg : 1.0 - bg) / fabs(d[k]);
        else r = d[k] > 0.0 ? (1.0 - bg) / d[k] : bg / -d[k];
        if (r < best) best = r;
    }
    return best;
}

YCOL_API double ycol_max_scale(const ycol_ctx* cx, ycol_color c, int sides) {
    double d[3], rgb[3];
    const char* why;
    int rc, k;
    if (!cx || (sides != YCOL_ONE_SIDED && sides != YCOL_SYMMETRIC)) return YCOL_ERR_ARG;
    if (ycol__is_incr(c.space)) {
        rc = ycol__need(cx, c.space, &why);
        if (rc < 0) return rc;
        if (!ycol__finite3(c.u.v)) return YCOL_ERR_ARG;
        ycol__incr(cx, c.space, c.u.v, d);
    } else {
        rc = ycol__to_rgb3(cx, &c, rgb, &why);
        if (rc < 0) return rc;
        for (k = 0; k < 3; k++) d[k] = rgb[k] - cx->bg[k];
    }
    return ycol__k(cx, d, sides);
}

/* Inside to double round-off only: the bisections and the maps return
 * colors in the cube, not merely within YCOL_GAMUT_EPS of it. */
static int ycol__inside(const double* rgb) {
    int k;
    for (k = 0; k < 3; k++) if (!(rgb[k] >= -1e-12 && rgb[k] <= 1.0 + 1e-12)) return 0;
    return 1;
}

/* The largest chroma in gamut at lightness L and hue h, from [0, cap]
 * (cap <= 0: no cap, the bracket grows). < 0 when chroma 0 is outside. */
static double ycol__max_chroma(const ycol_ctx* cx, int space, double L, double h, double cap) {
    ycol_color c;
    double rgb[3], lo = 0.0, hi;
    const char* why;
    int i;
    c = ycol_make(space, L, 0.0, h);
    if (ycol__to_rgb3(cx, &c, rgb, &why) < 0 || !ycol__inside(rgb)) return -1.0;
    if (cap > 0.0) {
        hi = cap;
        c.u.v[1] = hi;
        if (ycol__to_rgb3(cx, &c, rgb, &why) >= 0 && ycol__inside(rgb)) return hi;
    } else {
        hi = space == YCOL_SPACE_OKLCH ? 0.5 : 150.0;
        for (i = 0; i < 64; i++) {
            c.u.v[1] = hi;
            if (ycol__to_rgb3(cx, &c, rgb, &why) < 0 || !ycol__inside(rgb)) break;
            lo = hi;
            hi *= 2.0;
        }
    }
    for (i = 0; i < 60; i++) {
        double mid = 0.5 * (lo + hi);
        c.u.v[1] = mid;
        if (ycol__to_rgb3(cx, &c, rgb, &why) >= 0 && ycol__inside(rgb)) lo = mid; else hi = mid;
    }
    return lo;
}

YCOL_API int ycol_max_ring(const ycol_ctx* cx, int plane, double fixed, int sides, int n, double* out) {
    const char* why;
    int i, rc;
    if (!cx || !out || n < 1 || !ycol__finite(fixed) || (sides != YCOL_ONE_SIDED && sides != YCOL_SYMMETRIC))
        return YCOL_ERR_ARG;
    switch (plane) {
    case YCOL_PLANE_DKL:
    case YCOL_PLANE_CONE:
        rc = ycol__need(cx, plane == YCOL_PLANE_DKL ? YCOL_SPACE_DKL : YCOL_SPACE_CONE, &why);
        if (rc < 0) return rc;
        for (i = 0; i < n; i++) {
            double a = 360.0 * i / n, v[3], d[3];
            if (plane == YCOL_PLANE_DKL) {
                v[0] = fixed; v[1] = a; v[2] = 1.0;
                ycol__incr(cx, YCOL_SPACE_DKL, v, d);
            } else {
                double e = fixed * YCOL__D2R, t = a * YCOL__D2R;
                v[0] = cos(e) * cos(t); v[1] = cos(e) * sin(t); v[2] = sin(e);
                ycol__incr(cx, YCOL_SPACE_CONE, v, d);
            }
            out[i] = ycol__k(cx, d, sides);
        }
        return YCOL_OK;
    case YCOL_PLANE_OKLCH:
    case YCOL_PLANE_CIELCH:
        rc = ycol__need(cx, YCOL_SPACE_OKLCH, &why);
        if (rc < 0) return rc;
        for (i = 0; i < n; i++)
            out[i] = ycol__max_chroma(cx, plane == YCOL_PLANE_OKLCH ? YCOL_SPACE_OKLCH : YCOL_SPACE_CIELCH, fixed,
                                        360.0 * i / n, 0.0);
        return YCOL_OK;
    default:
        return YCOL_ERR_ARG;
    }
}

YCOL_API ycol_rgb ycol_map(const ycol_ctx* cx, ycol_color c, int method, ycol_gamut* g) {
    ycol_gamut gg;
    double rgb[3], d[3], res[3];
    const char* why = "NULL context";
    int rc, k;
    if (!g) g = &gg;
    if (!cx) { ycol__refuse(g, YCOL_ERR_ARG, why, rgb); return ycol__rgb(rgb); }
    if (method < YCOL_MAP_SCALE || method > YCOL_MAP_CLIP) {
        ycol__refuse(g, YCOL_ERR_ARG, "not a known map method", rgb);
        return ycol__rgb(rgb);
    }
    rc = ycol__to_rgb3(cx, &c, rgb, &why);
    if (rc < 0) { ycol__refuse(g, rc, why, rgb); return ycol__rgb(rgb); }
    for (k = 0; k < 3; k++) d[k] = rgb[k] - cx->bg[k];
    ycol__gamut(cx, rgb, d, 0, g);
    if (g->in) return ycol__rgb(rgb);
    switch (method) {
    case YCOL_MAP_SCALE:
        for (k = 0; k < 3; k++) res[k] = cx->bg[k] + g->scale * d[k];
        g->kept = g->scale;
        break;
    case YCOL_MAP_CLIP:
        for (k = 0; k < 3; k++) res[k] = rgb[k] < 0.0 ? 0.0 : (rgb[k] > 1.0 ? 1.0 : rgb[k]);
        g->kept = ycol__nan();
        break;
    case YCOL_MAP_CHROMA_OKLCH:
    case YCOL_MAP_CHROMA_CIELCH: {
        int sp = method == YCOL_MAP_CHROMA_OKLCH ? YCOL_SPACE_OKLCH : YCOL_SPACE_CIELCH;
        ycol_color lch, m;
        double cmax;
        rc = ycol__from_rgb3(cx, rgb, sp, &lch, &why);
        if (rc < 0) { ycol__refuse(g, rc, why, res); return ycol__rgb(res); }
        cmax = ycol__max_chroma(cx, sp, lch.u.v[0], lch.u.v[2], lch.u.v[1]);
        if (cmax < 0.0) {
            ycol__refuse(g, YCOL_ERR_RANGE, "a gray of this lightness is outside the gamut: less chroma cannot reach it", res);
            return ycol__rgb(res);
        }
        m = ycol_make(sp, lch.u.v[0], cmax, lch.u.v[2]);
        ycol__to_rgb3(cx, &m, res, &why);
        g->kept = lch.u.v[1] > 0.0 ? cmax / lch.u.v[1] : 1.0;
        break;
    }
    case YCOL_MAP_CHROMA_DKL: {
        double cart[3], lmsd[3], p0[3], q[3], v[3], t = 1.0;
        rc = ycol__need(cx, YCOL_SPACE_DKL_CART, &why);
        if (rc < 0) { ycol__refuse(g, rc, why, res); return ycol__rgb(res); }
        ycol__mul3(cx->rgb_to_lms, d, lmsd);
        ycol__mul3(cx->dkl, lmsd, cart);
        v[0] = cart[0]; v[1] = 0.0; v[2] = 0.0;
        ycol__incr(cx, YCOL_SPACE_DKL_CART, v, p0);
        v[0] = 0.0; v[1] = cart[1]; v[2] = cart[2];
        ycol__incr(cx, YCOL_SPACE_DKL_CART, v, q);
        for (k = 0; k < 3; k++) p0[k] += cx->bg[k];
        if (!ycol__inside(p0)) {
            ycol__refuse(g, YCOL_ERR_RANGE, "the luminance part alone is outside the gamut", res);
            return ycol__rgb(res);
        }
        for (k = 0; k < 3; k++) {
            double r = HUGE_VAL;
            if (q[k] > 0.0) r = (1.0 - p0[k]) / q[k];
            else if (q[k] < 0.0) r = p0[k] / -q[k];
            if (r < 0.0) r = 0.0;
            if (r < t) t = r;
        }
        for (k = 0; k < 3; k++) res[k] = p0[k] + t * q[k];
        g->kept = t;
        break;
    }
    default:
        memcpy(res, rgb, sizeof res);
        break;
    }
    return ycol__rgb(res);
}

/* --- device codes ----------------------------------------------------------- */

YCOL_API int ycol_output_code(const ycol_cal* cal, ycol_rgb rgb, int bits, uint32_t code[3]) {
    const double in[3] = { rgb.r, rgb.g, rgb.b };
    int n, c;
    float mx;
    if (!cal || !code || bits < 8 || bits > 16) return YCOL_ERR_ARG;
    n = cal->lut_n ? cal->lut_n : YCOL_CAL_MAX_LUT;
    if (n < 2 || n > YCOL_CAL_MAX_LUT) return YCOL_ERR_FORMAT;
    mx = (float)((1 << bits) - 1);
    for (c = 0; c < 3; c++) {
        /* the output stage's float arithmetic, step for step */
        float v = (float)in[c], x, a, b, d, k;
        int i0;
        if (!(v == v)) return YCOL_ERR_ARG;
        v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
        x = v * (float)(n - 1);
        i0 = (int)x;
        if (i0 > n - 2) i0 = n - 2;
        a = cal->lut[c][i0];
        b = cal->lut[c][i0 + 1];
        d = a + (b - a) * (x - (float)i0);
        k = (float)floor(d * mx + 0.5f);
        k = k < 0.0f ? 0.0f : (k > mx ? mx : k);
        code[c] = (uint32_t)k;
    }
    return YCOL_OK;
}

/* --- values ----------------------------------------------------------------- */

YCOL_API ycol_color ycol_make(int space, double a, double b, double c) {
    ycol_color r;
    memset(&r, 0, sizeof r);
    r.space = space;
    r.u.v[0] = a; r.u.v[1] = b; r.u.v[2] = c;
    return r;
}

static int ycol__hexval(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

YCOL_API ycol_color ycol_hex(const char* s) {
    ycol_color r;
    int v[6], n = 0, k;
    memset(&r, 0, sizeof r);
    if (!s) return r;
    if (*s == '#') s++;
    while (s[n] && n < 7) n++;
    if (n != 3 && n != 6) return r;
    for (k = 0; k < n; k++) if ((v[k] = ycol__hexval(s[k])) < 0) return r;
    r.space = YCOL_SPACE_SRGB;
    for (k = 0; k < 3; k++) r.u.v[k] = n == 3 ? v[k] / 15.0 : (v[2 * k] * 16 + v[2 * k + 1]) / 255.0;
    return r;
}

YCOL_API int ycol_hex_format(ycol_color c, char out[8]) {
    static const char dig[] = "0123456789abcdef";
    int k, q[3];
    if (!out || c.space != YCOL_SPACE_SRGB) return YCOL_ERR_ARG;
    for (k = 0; k < 3; k++) {
        double t = floor(c.u.v[k] * 255.0 + 0.5);
        if (!(t >= 0.0 && t <= 255.0)) return YCOL_ERR_RANGE;
        q[k] = (int)t;
    }
    out[0] = '#';
    for (k = 0; k < 3; k++) { out[1 + 2 * k] = dig[q[k] >> 4]; out[2 + 2 * k] = dig[q[k] & 15]; }
    out[7] = '\0';
    return YCOL_OK;
}

YCOL_API void ycol_store3f(float dst[3], ycol_rgb c) {
    if (!dst) return;
    dst[0] = (float)c.r; dst[1] = (float)c.g; dst[2] = (float)c.b;
}

/* --- parameter tables ------------------------------------------------------- */

static const ycol_space_info ycol__spaces[YCOL_SPACE_COUNT - 1] = {
    { YCOL_SPACE_RGB, 0, "rgb", { "r", "g", "b" }, { "", "", "" }, { 0, 0, 0 }, { 1, 1, 1 } },
    { YCOL_SPACE_DEVICE, 0, "device", { "r", "g", "b" }, { "", "", "" }, { 0, 0, 0 }, { 1, 1, 1 } },
    { YCOL_SPACE_XYZ, YCOL_CAN_XYZ, "xyz", { "X", "Y", "Z" }, { "cd/m2", "cd/m2", "cd/m2" }, { 0, 0, 0 }, { 200, 200, 200 } },
    { YCOL_SPACE_XYY, YCOL_CAN_XYZ, "xyy", { "x", "y", "Y" }, { "", "", "cd/m2" }, { 0, 0, 0 }, { 0.8, 0.9, 200 } },
    { YCOL_SPACE_CIELAB, YCOL_CAN_XYZ, "cielab", { "L", "a", "b" }, { "", "", "" }, { 0, -128, -128 }, { 100, 128, 128 } },
    { YCOL_SPACE_CIELCH, YCOL_CAN_XYZ, "cielch", { "L", "C", "h" }, { "", "", "deg" }, { 0, 0, 0 }, { 100, 150, 360 } },
    { YCOL_SPACE_CIELUV, YCOL_CAN_XYZ, "cieluv", { "L", "u", "v" }, { "", "", "" }, { 0, -100, -140 }, { 100, 180, 110 } },
    { YCOL_SPACE_CIELCHUV, YCOL_CAN_XYZ, "cielchuv", { "L", "C", "h" }, { "", "", "deg" }, { 0, 0, 0 }, { 100, 180, 360 } },
    { YCOL_SPACE_OKLAB, YCOL_CAN_XYZ, "oklab", { "L", "a", "b" }, { "", "", "" }, { 0, -0.4, -0.4 }, { 1, 0.4, 0.4 } },
    { YCOL_SPACE_OKLCH, YCOL_CAN_XYZ, "oklch", { "L", "C", "h" }, { "", "", "deg" }, { 0, 0, 0 }, { 1, 0.4, 360 } },
    { YCOL_SPACE_SRGB, YCOL_CAN_XYZ, "srgb", { "r", "g", "b" }, { "", "", "" }, { 0, 0, 0 }, { 1, 1, 1 } },
    { YCOL_SPACE_DISPLAY_P3, YCOL_CAN_XYZ, "display-p3", { "r", "g", "b" }, { "", "", "" }, { 0, 0, 0 }, { 1, 1, 1 } },
    { YCOL_SPACE_REC2020, YCOL_CAN_XYZ, "rec2020", { "r", "g", "b" }, { "", "", "" }, { 0, 0, 0 }, { 1, 1, 1 } },
    { YCOL_SPACE_LMS, YCOL_CAN_LMS, "lms", { "l", "m", "s" }, { "", "", "" }, { 0, 0, 0 }, { 1, 1, 1 } },
    { YCOL_SPACE_CONE, YCOL_CAN_LMS | YCOL_CAN_BG, "cone", { "l", "m", "s" }, { "", "", "" }, { -1, -1, -1 }, { 1, 1, 1 } },
    { YCOL_SPACE_DKL, YCOL_CAN_LMS | YCOL_CAN_BG, "dkl", { "elev", "azim", "contrast" }, { "deg", "deg", "" }, { -90, 0, 0 }, { 90, 360, 1 } },
    { YCOL_SPACE_DKL_CART, YCOL_CAN_LMS | YCOL_CAN_BG, "dkl-cart", { "lum", "lm", "s" }, { "", "", "" }, { -1, -1, -1 }, { 1, 1, 1 } },
    { YCOL_SPACE_MB, YCOL_CAN_LMS, "mb", { "l", "s", "lum" }, { "", "", "" }, { 0.5, 0, 0 }, { 1, 1, 1 } },
};

YCOL_API const ycol_space_info* ycol_spaces(int* n) {
    if (n) *n = YCOL_SPACE_COUNT - 1;
    return ycol__spaces;
}

YCOL_API int ycol_space_from_name(const char* name) {
    int i;
    if (!name) return YCOL_SPACE_NONE;
    for (i = 0; i < YCOL_SPACE_COUNT - 1; i++) if (strcmp(name, ycol__spaces[i].name) == 0) return ycol__spaces[i].space;
    return YCOL_SPACE_NONE;
}

YCOL_API int ycol_format(ycol_color c, char* buf, size_t cap) {
    const char* name = c.space > YCOL_SPACE_NONE && c.space < YCOL_SPACE_COUNT ? ycol__spaces[c.space - 1].name : "none";
    int n;
    if (!buf || cap == 0) return YCOL_ERR_ARG;
    n = snprintf(buf, cap, "%s(%.17g %.17g %.17g)", name, c.u.v[0], c.u.v[1], c.u.v[2]);
    return n < 0 ? YCOL_ERR_ARG : ((size_t)n >= cap ? YCOL_ERR_FULL : n);
}

#define YCOL__P(f, i) (uint32_t)(offsetof(ycol_ctx_desc, f) + (i) * sizeof(double))
static const ycol_param ycol__desc_params[] = {
    { "background[0]", "f64", 0, 1, 0, "", "background, red: the zero of the contrast spaces", YCOL__P(background, 0) },
    { "background[1]", "f64", 0, 1, 0, "", "background, green", YCOL__P(background, 1) },
    { "background[2]", "f64", 0, 1, 0, "", "background, blue", YCOL__P(background, 2) },
    { "cones", "enum", 0, 1, 0, "", "cone fundamentals: 0 SS2 (2 degree), 1 SS10 (10 degree)", (uint32_t)offsetof(ycol_ctx_desc, cones) },
    { "adapt", "enum", 0, 1, 0, "", "D65 sources: 0 no adaptation, 1 Bradford to the display's white", (uint32_t)offsetof(ycol_ctx_desc, adapt) },
    { "white[0]", "f64", 0, 1e6, 0, "cd/m2", "reference white X; all 0 = the display's white", YCOL__P(white, 0) },
    { "white[1]", "f64", 0, 1e6, 0, "cd/m2", "reference white Y", YCOL__P(white, 1) },
    { "white[2]", "f64", 0, 1e6, 0, "cd/m2", "reference white Z", YCOL__P(white, 2) },
    { "src_white_Y", "f64", 0, 1e6, 0, "cd/m2", "a D65 source's white; 0 = relative to the display", (uint32_t)offsetof(ycol_ctx_desc, src_white_Y) },
};
#undef YCOL__P

YCOL_API const ycol_param* ycol_desc_params(int* n) {
    if (n) *n = (int)(sizeof ycol__desc_params / sizeof ycol__desc_params[0]);
    return ycol__desc_params;
}

/* --- participant luminance -------------------------------------------------- */

static const char ycol__lum_magic[8] = { 'Y', 'S', 'P', 'L', 'U', 'M', '\0', '\1' };

#define YCOL__LAT(f, off) typedef char ycol__lum_at_##f[offsetof(ycol_lum, f) == (off) ? 1 : -1]
YCOL__LAT(version, 8);
YCOL__LAT(date, 32);
YCOL__LAT(participant, 40);
YCOL__LAT(cal_crc, 104);
YCOL__LAT(settings, 120);
YCOL__LAT(w, 1528);
YCOL__LAT(resid, 1552);
YCOL__LAT(crc, 1684);
typedef char ycol__lum_size[sizeof(ycol_lum) == 1688 ? 1 : -1];
typedef char ycol__setting_size[sizeof(ycol_lum_setting) == 88 ? 1 : -1];
#undef YCOL__LAT

static int ycol__lum_header(const ycol_lum* l) {
    if (memcmp(l->magic, ycol__lum_magic, 8) != 0 || l->version != 1 || l->bytes != (uint32_t)sizeof *l) return YCOL_ERR_FORMAT;
    if (l->crc == 0 || l->crc != ycol__crc32(l, offsetof(ycol_lum, crc))) return YCOL_ERR_FORMAT;
    if (!(l->cones == YCOL_CONES_SS2 || l->cones == YCOL_CONES_SS10)) return YCOL_ERR_FORMAT;
    if (l->n_settings < 0 || l->n_settings > YCOL_LUM_MAX_SETTINGS) return YCOL_ERR_FORMAT;
    return YCOL_OK;
}

YCOL_API void ycol_lum_init(ycol_lum* l, int cones, int method) {
    if (!l) return;
    memset(l, 0, sizeof *l);
    memcpy(l->magic, ycol__lum_magic, 8);
    l->version = 1;
    l->bytes = (uint32_t)sizeof *l;
    l->cones = cones;
    l->method = method;
}

YCOL_API int ycol_lum_add(ycol_lum* l, const double a[3], const double b[3], const double bg[3], double sd, int n) {
    ycol_lum_setting* s;
    if (!l || !a || !b || !ycol__finite3(a) || !ycol__finite3(b) || (bg && !ycol__finite3(bg)) || !ycol__finite(sd) || n < 0)
        return YCOL_ERR_ARG;
    if (l->n_settings < 0 || l->n_settings >= YCOL_LUM_MAX_SETTINGS) return YCOL_ERR_FULL;
    s = &l->settings[l->n_settings++];
    memset(s, 0, sizeof *s);
    memcpy(s->a, a, sizeof s->a);
    memcpy(s->b, b, sizeof s->b);
    if (bg) memcpy(s->bg, bg, sizeof s->bg);
    s->sd = sd;
    s->n = n;
    l->crc = 0;
    return YCOL_OK;
}

/* Eigenvalues (ascending) and vectors (columns) of a symmetric n x n
 * matrix, n <= 3, by cyclic Jacobi rotations. */
static void ycol__jacobi(double* a, int n, double* val, double* vec) {
    int i, j, k, sweep;
    for (i = 0; i < n * n; i++) vec[i] = 0.0;
    for (i = 0; i < n; i++) vec[i * n + i] = 1.0;
    for (sweep = 0; sweep < 60; sweep++) {
        double off = 0.0;
        for (i = 0; i < n; i++) for (j = i + 1; j < n; j++) off += a[i * n + j] * a[i * n + j];
        if (off < 1e-300) break;
        for (i = 0; i < n; i++)
            for (j = i + 1; j < n; j++) {
                double apq = a[i * n + j], th, t, cs, sn;
                if (apq == 0.0) continue;
                th = (a[j * n + j] - a[i * n + i]) / (2.0 * apq);
                t = (th >= 0 ? 1.0 : -1.0) / (fabs(th) + sqrt(th * th + 1.0));
                cs = 1.0 / sqrt(t * t + 1.0); sn = t * cs;
                for (k = 0; k < n; k++) {
                    double x = a[k * n + i], y = a[k * n + j];
                    a[k * n + i] = cs * x - sn * y; a[k * n + j] = sn * x + cs * y;
                }
                for (k = 0; k < n; k++) {
                    double x = a[i * n + k], y = a[j * n + k];
                    a[i * n + k] = cs * x - sn * y; a[j * n + k] = sn * x + cs * y;
                }
                for (k = 0; k < n; k++) {
                    double x = vec[k * n + i], y = vec[k * n + j];
                    vec[k * n + i] = cs * x - sn * y; vec[k * n + j] = sn * x + cs * y;
                }
            }
    }
    for (i = 0; i < n; i++) val[i] = a[i * n + i];
    for (i = 0; i < n; i++)   /* ascending, vectors along */
        for (j = i + 1; j < n; j++)
            if (val[j] < val[i]) {
                double t = val[i]; val[i] = val[j]; val[j] = t;
                for (k = 0; k < n; k++) { t = vec[k * n + i]; vec[k * n + i] = vec[k * n + j]; vec[k * n + j] = t; }
            }
}

static int ycol__lum_fit(ycol_lum* l, const ycol_cal* cal, char* err, size_t cap) {
    double m[9], blms[3], sc[9], val[3], vec[9], w[3], ws[3], sum[3] = { 0, 0, 0 }, scale;
    const float (*t)[3];
    int fit_s = (l->flags & YCOL_LUM_FIT_S) != 0, n = fit_s ? 3 : 2, i, j, k;
    if (!cal || memcmp(cal->magic, ycol__cal_magic, 8) != 0 || cal->crc == 0 ||
        cal->crc != ycol__crc32(cal, offsetof(ycol_cal, crc))) {
        ycol__set_error(err, cap, "ysp_color: the calibration is missing, not sealed or fails its CRC");
        return YCOL_ERR_FORMAT;
    }
    if (!(l->cones == YCOL_CONES_SS2 || l->cones == YCOL_CONES_SS10)) {
        ycol__set_error(err, cap, "ysp_color: lum.cones is not a known value"); return YCOL_ERR_ARG;
    }
    if (l->cal_crc && l->cal_crc != cal->crc) {
        ycol__set_error(err, cap, "ysp_color: the settings were made on calibration %08x, not %08x", (unsigned)l->cal_crc,
                          (unsigned)cal->crc);
        return YCOL_ERR_REFUSED;
    }
    if (l->cones == YCOL_CONES_SS2) {
        if (!ycol__has_lms(cal)) { ycol__set_error(err, cap, "ysp_color: the calibration has no spectra, so no cone space"); return YCOL_ERR_REFUSED; }
        memcpy(m, cal->rgb_to_lms, sizeof m); memcpy(blms, cal->black_lms, sizeof blms);
    } else if (!ycol__integrate(cal, l->cones, m, blms)) {
        ycol__set_error(err, cap, "ysp_color: the calibration has no spectra, so no cone space"); return YCOL_ERR_REFUSED;
    }
    if (l->n_settings < (fit_s ? 2 : 1) || l->n_settings > YCOL_LUM_MAX_SETTINGS) {
        ycol__set_error(err, cap, "ysp_color: %d settings; %s needs at least %d", l->n_settings, fit_s ? "FIT_S" : "a fit", fit_s ? 2 : 1);
        return YCOL_ERR_RANGE;
    }
    memset(sc, 0, sizeof sc);
    for (i = 0; i < l->n_settings; i++) {
        const ycol_lum_setting* s = &l->settings[i];
        double d[3], dl[3], nn;
        for (k = 0; k < 3; k++) d[k] = s->a[k] - s->b[k];
        ycol__mul3(m, d, dl);
        nn = sqrt(dl[0] * dl[0] + dl[1] * dl[1] + dl[2] * dl[2]);
        if (!(nn > 0.0)) { ycol__set_error(err, cap, "ysp_color: setting %d: the two lights are equal", i); return YCOL_ERR_RANGE; }
        if (!fit_s && sqrt(dl[0] * dl[0] + dl[1] * dl[1]) <= 1e-9 * nn) {
            ycol__set_error(err, cap, "ysp_color: setting %d differs only in S; without YCOL_LUM_FIT_S it says nothing", i);
            return YCOL_ERR_RANGE;
        }
        for (k = 0; k < 3; k++) dl[k] /= nn;
        for (j = 0; j < n; j++) for (k = 0; k < n; k++) sc[j * n + k] += dl[j] * dl[k];
    }
    ycol__jacobi(sc, n, val, vec);
    if (!(val[1] > 1e-12 * val[n - 1])) {
        ycol__set_error(err, cap, "ysp_color: the settings do not fix the weights (FIT_S needs two independent directions)");
        return YCOL_ERR_RANGE;
    }
    w[0] = vec[0 * n]; w[1] = vec[1 * n]; w[2] = fit_s ? vec[2 * n] : 0.0;
    if (w[0] + w[1] < 0.0) { w[0] = -w[0]; w[1] = -w[1]; w[2] = -w[2]; }
    if (!(w[0] > 0.0 && w[1] > 0.0)) {
        ycol__set_error(err, cap, "ysp_color: the fit gives L %.4g, M %.4g: not a luminance", w[0], w[1]);
        return YCOL_ERR_RANGE;
    }
    /* an equal-energy spectrum keeps the standard luminance */
    t = ycol__table(l->cones);
    for (i = 0; i < 441; i++) { sum[0] += t[i][0]; sum[1] += t[i][1]; sum[2] += t[i][2]; }
    ycol_cone_luminance(l->cones, ws);
    scale = (ws[0] * sum[0] + ws[1] * sum[1] + ws[2] * sum[2]) / (w[0] * sum[0] + w[1] * sum[1] + w[2] * sum[2]);
    for (k = 0; k < 3; k++) l->w[k] = w[k] * scale;
    for (i = 0; i < YCOL_LUM_MAX_SETTINGS; i++) l->resid[i] = 0.0;
    for (i = 0; i < l->n_settings; i++) {
        const ycol_lum_setting* s = &l->settings[i];
        double la[3], lb[3], va, vb;
        ycol__mul3(m, s->a, la);
        ycol__mul3(m, s->b, lb);
        for (k = 0; k < 3; k++) { la[k] += blms[k]; lb[k] += blms[k]; }
        va = l->w[0] * la[0] + l->w[1] * la[1] + l->w[2] * la[2];
        vb = l->w[0] * lb[0] + l->w[1] * lb[1] + l->w[2] * lb[2];
        l->resid[i] = va + vb != 0.0 ? (va - vb) / (va + vb) : 0.0;
    }
    l->cal_crc = cal->crc;
    return YCOL_OK;
}

YCOL_API int ycol_lum_derive(ycol_lum* l, const ycol_cal* cal, char* err, size_t cap) {
    int rc;
    if (err && cap) err[0] = '\0';
    if (!l) return YCOL_ERR_ARG;
    if (memcmp(l->magic, ycol__lum_magic, 8) != 0 || l->version != 1 || l->bytes != (uint32_t)sizeof *l) {
        ycol__set_error(err, cap, "ysp_color: not a participant luminance record (ycol_lum_init)");
        return YCOL_ERR_FORMAT;
    }
    if (l->flags & YCOL_LUM_STATED) { ycol__set_error(err, cap, "ysp_color: a STATED record has no fit"); return YCOL_ERR_ARG; }
    rc = ycol__lum_fit(l, cal, err, cap);
    if (rc >= 0) l->crc = ycol__crc32(l, offsetof(ycol_lum, crc));
    return rc;
}

YCOL_API int ycol_lum_stated(ycol_lum* l, int cones, const double w[3]) {
    if (!l || !w || !ycol__finite3(w) || !(w[0] > 0.0 && w[1] > 0.0)) return YCOL_ERR_ARG;
    if (!(cones == YCOL_CONES_SS2 || cones == YCOL_CONES_SS10)) return YCOL_ERR_ARG;
    if (memcmp(l->magic, ycol__lum_magic, 8) != 0) ycol_lum_init(l, cones, YCOL_LUM_OTHER);
    l->cones = cones;
    l->flags |= YCOL_LUM_STATED;
    memcpy(l->w, w, sizeof l->w);
    l->crc = ycol__crc32(l, offsetof(ycol_lum, crc));
    return YCOL_OK;
}

YCOL_API int ycol_lum_check(const ycol_lum* l, const ycol_cal* cal, char* err, size_t cap) {
    ycol_lum copy;
    int rc, k;
    if (err && cap) err[0] = '\0';
    if (!l) return YCOL_ERR_ARG;
    rc = ycol__lum_header(l);
    if (rc < 0) { ycol__set_error(err, cap, "ysp_color: not a sealed participant luminance record, or its CRC fails"); return rc; }
    if (!cal || (l->flags & YCOL_LUM_STATED)) return YCOL_OK;
    memcpy(&copy, l, sizeof copy);
    rc = ycol__lum_fit(&copy, cal, err, cap);
    if (rc < 0) return rc;
    for (k = 0; k < 3; k++)
        if (fabs(copy.w[k] - l->w[k]) > 1e-12 * (fabs(l->w[0]) + fabs(l->w[1]))) {
            ycol__set_error(err, cap, "ysp_color: the stored weights differ from the ones its settings give");
            return YCOL_ERR_FORMAT;
        }
    return YCOL_OK;
}

YCOL_API int ycol_lum_save(ycol_lum* l, void* out, size_t cap) {
    if (!l) return YCOL_ERR_ARG;
    l->crc = ycol__crc32(l, offsetof(ycol_lum, crc));
    if (!out) return (int)sizeof *l;
    if (cap < sizeof *l) return YCOL_ERR_FULL;
    memcpy(out, l, sizeof *l);
    return (int)sizeof *l;
}

YCOL_API int ycol_lum_load(ycol_lum* l, const void* bytes, size_t n, char* err, size_t cap) {
    if (err && cap) err[0] = '\0';
    if (!l || !bytes) return YCOL_ERR_ARG;
    if (n != sizeof *l || memcmp(bytes, ycol__lum_magic, 8) != 0) {
        ycol__set_error(err, cap, "ysp_color: not a participant luminance record (%lu bytes, want %u with magic YSPLUM)",
                          (unsigned long)n, (unsigned)sizeof *l);
        return YCOL_ERR_FORMAT;
    }
    memcpy(l, bytes, sizeof *l);
    if (ycol__lum_header(l) < 0) {
        memset(l, 0, sizeof *l);
        ycol__set_error(err, cap, "ysp_color: CRC mismatch or a bad header: the file was changed or truncated");
        return YCOL_ERR_FORMAT;
    }
    return YCOL_OK;
}

YCOL_API int ycol_lum_describe(const ycol_lum* l, char* buf, size_t cap) {
    static const char* const meth[] = { "?", "HFP", "minimum motion", "minimally distinct border", "other" };
    double rmax = 0.0;
    int n, i;
    if (!l || !buf || cap == 0) return YCOL_ERR_ARG;
    for (i = 0; i < l->n_settings && i < YCOL_LUM_MAX_SETTINGS; i++) if (fabs(l->resid[i]) > rmax) rmax = fabs(l->resid[i]);
    n = snprintf(buf, cap, "lum %08x \"%.40s\" %s%s, %s, w %.8g L + %.8g M + %.8g S, %d settings, |resid| <= %.3g, calibration %08x, "
                 "field %.3g deg, eccentricity %.3g deg, %.4g Hz",
                 (unsigned)l->crc, l->participant, meth[l->method >= 1 && l->method <= 4 ? l->method : 0],
                 l->flags & YCOL_LUM_STATED ? " STATED" : "", l->cones == YCOL_CONES_SS10 ? "SS10" : "SS2", l->w[0], l->w[1],
                 l->w[2], l->n_settings, rmax, (unsigned)l->cal_crc, (double)l->field_deg, (double)l->ecc_deg, (double)l->freq_hz);
    return n < 0 ? YCOL_ERR_ARG : ((size_t)n >= cap ? YCOL_ERR_FULL : n);
}

YCOL_API int ycol_lum_pair(const ycol_ctx* cx, double azim, double elev, double contrast, double a[3], double b[3]) {
    double v[3], d[3];
    const char* why;
    int rc, k;
    if (!cx || !a || !b || !ycol__finite(azim) || !ycol__finite(elev) || !ycol__finite(contrast)) return YCOL_ERR_ARG;
    rc = ycol__need(cx, YCOL_SPACE_DKL, &why);
    if (rc < 0) return rc;
    v[0] = elev; v[1] = azim; v[2] = contrast;
    ycol__incr(cx, YCOL_SPACE_DKL, v, d);
    for (k = 0; k < 3; k++) { a[k] = cx->bg[k] + d[k]; b[k] = cx->bg[k] - d[k]; }
    return YCOL_OK;
}

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#endif /* YSP_COLOR_IMPLEMENTATION_GUARD */
#endif /* YSP_COLOR_IMPLEMENTATION */

/* ------------------------------------------------------------------------
 * This software is available under the MIT-0 (MIT No Attribution) license.
 *
 * Copyright (c) 2026 ysp contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY.
 * ------------------------------------------------------------------------ */
