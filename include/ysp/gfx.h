/* ysp/gfx.h - v0.12.0 - public domain single-header stimulus graphics library
 *   (with MIT-licensed parts: see below)
 *
 *   Stimuli on GL ES 3.0, on top of ysp/screen.h: signed-distance shapes
 *   with a stated edge profile, gratings, gabors, dot fields, image
 *   textures and hash noise, drawn in linear light into a float scene,
 *   then one output stage (a lookup table per channel from a calibration,
 *   dithering, 8-bit quantization) into the screen's back buffer. Also the
 *   calibration math (cone fundamentals, cone contrast, DKL), a canonical
 *   calibration file, a user shader contract, timeline bindings and a
 *   parameter table. Vector shapes from closed-form distance functions
 *   (v0.3): rounded and partial shapes, paths of up to 224 points,
 *   compounds, effects, dashes, gradients, blend modes, MSDF atlases and
 *   glyph runs. Text and artwork from glyph outlines (v0.6): curve sets of
 *   quadratic curves and bands (Lengyel's Slug algorithm), drawn as curve
 *   runs with box-filter coverage; a separable Gaussian blur pass. No
 *   compute stage, no SVG: the pack tool's outline builder makes the curve
 *   sets (ysp/outline.h).
 *
 *   REQUIRES ysp/screen.h, ysp/rt.h and ysp/color.h beside it, and what
 *   ysp/screen.h needs (SDL3 to link; ANGLE's DLLs at run time on
 *   Windows). Nothing ANGLE or Khronos is needed to compile: GL is loaded
 *   at run time.
 *
 *   Written in the single-header style of the stb / sokol libraries. C99 is
 *   the floor: it builds as C99, C11 and C++17, and in the C dialect MSVC
 *   compiles by default.
 *
 *   MIT-LICENSED PARTS: the vector program's distance functions follow
 *   Inigo Quilez's (box, rounded box, uneven capsule, arc, pie, star,
 *   smooth minimum) and 0xfaded's ellipse. They are in ygfx__glsl_vec0,
 *   _vec0b, _vec1 and their CPU forms (ygfx__vp_*, ygfx__vsmin).
 *   Each such function names its source in a comment; the MIT notices are
 *   in the license block at the end of the file. The rest is public domain
 *   / MIT-0.
 *
 *   ---------------------------------------------------------------------
 *   CHANGELOG
 *   ---------------------------------------------------------------------
 *   v0.12.0 - CAPTURE: the output of chosen frames (every Nth, or marked
 *          with ygfx_capture_mark()) copied in ygfx_end() into pixel
 *          buffers with a fence, collected a frame or more later without a
 *          wait, tagged with the frame index and its flip record, and
 *          written off the frame thread (ysp/rt.h's pump at below normal
 *          priority) as PPM or stored PNG files, or handed to a callback. A
 *          full ring drops the frame and counts it (YGFX_EV_CAPTURE_DROP).
 *          desc.manual, and YRT_NO_THREADS, leave the maps and the writes
 *          to ygfx_capture_pump() between trials. ygfx_image_write(), the
 *          image writer the examples now share. Backend version 4 (five
 *          capture calls; open() still takes version 3, without capture).
 *          YGFX_ERR_IO. Measured on the Iris Xe at 1920 x 1200 (docs/gfx.md,
 *          "v0.12"): a captured frame costs 0.56 to 0.77 ms of GPU and the
 *          frame thread 2.1 to 2.2 ms of CPU, almost all in ANGLE's map
 *          (2.6 ms mean, 5.1 ms p99); without the map in the frame
 *          (manual) the copy's call costs nothing measurable. A PPM file
 *          takes the writer 8.0 to 11.4 ms, a stored PNG 14.7 to 18.5 ms.
 *          In 600 to 1800-frame runs fullscreen (1920 x 1200) and in a
 *          window, capture of every frame, every 4th, to PPM, to PNG and
 *          manual moved no flip: no late target, no early flip, no onset
 *          interval over 16.69 ms; ygfx_end() p99 3.9 to 4.2 ms with every
 *          frame captured fullscreen, 0.3 to 0.5 ms without.
 *          Fixed: ygfx_strerror() named YGFX_ERR_LOST "unknown error".
 *   v0.11.1 - ygfx_open_start() (OPEN START): the open in two calls, so
 *          the programs compile while ysp/screen.h v0.5.2's open()
 *          settles. Called from desc.on_context, it submits every built-in
 *          program and returns; ygfx_open() then waits for the link
 *          statuses. Without GL_KHR_parallel_shader_compile it submits
 *          nothing and ygfx_open() does the whole open, as before; the
 *          describe line says which (open_start=parallel or
 *          open_start=serial(...)). caps.parallel_compile in the backend
 *          interface (version unchanged). ygfx_close() deletes nothing
 *          when the screen's context is gone. Measured on the Iris Xe
 *          (docs/gfx.md, "v0.11.1"), cold program cache, on AC: gfx
 *          ready 2.57 s after yscr_open() was called in a window, was
 *          4.79 to 4.83 s; 2.46 to 2.47 s fullscreen, was 2.87 to 2.91 s
 *          (on battery 3.41 against 5.37 to 5.40 s, and 3.46 to 3.47
 *          against 3.85 to 3.89 s); a warm cache 24 to 51 ms earlier.
 *   v0.11.0 - ygfx_prime() (PRIME): draws every program made so far once,
 *          as zero-size quads, and waits for the GPU, so a kind's first
 *          trial frame does not pay ANGLE's first draw. Measured on the
 *          Iris Xe (docs/gfx.md, "Priming"): each kind's first frame cost
 *          2.5 to 12.9 ms more CPU in ygfx_end() without it, from a warm
 *          program cache too, and 0.09 to 0.42 ms with it; the prime of 20
 *          programs took 63 to 73 ms. With ysp/screen.h v0.5.0 (open()
 *          settles the display).
 *   v0.10.5 - VIDEO: an R8 or RG8 texture may state an encoding (matrix
 *          RGB, primaries DEVICE: gray is the display's white), drawn as a
 *          color image through the video program with the level in r and
 *          alpha in g. A pack's TEXTURE entry (docs/pack.md 4.9) now goes
 *          into ygfx_texture() as stored: an 8-bit gray PNG is sRGB codes,
 *          which the tool could linearize to 8 bits only by merging the
 *          dark codes. Other primaries on a gray texture are refused. An
 *          encoded texture used as a modulation, add, coverage or shader
 *          texture is refused as before, with a message that says why.
 *          YGFX_SHADER_CONTRACT is 2 (decided 2026-10-09 by the user): a
 *          body that samples ysp_tex0 needs v0.10.4's binding, and a
 *          contract-1 player would run it and read garbage.
 *   v0.10.4 - SHADER CONTRACT: stim.tex (user_desc.tex) is the body's
 *          ysp_tex0, ysp_utex0 for an R16UI texture: an uploaded texture
 *          or a render target. Before, a USER draw bound no texture, so
 *          ysp_tex0 read what the previous draw had left on its unit, and
 *          the feedback check could not see a body that sampled its own
 *          target. A tex that is not this gfx's, or a video texture, is
 *          refused (YGFX_ERR_ARG); a draw into the target it samples is
 *          YGFX_ERR_ORDER, as for an image. Found by
 *          examples/gfx/gratings.c (docs/rig_spec.md 15.6). The contract's
 *          version stays 1: the wrapper's text did not change.
 *   v0.10.3 - YGFX_SHADER_CONTRACT, the shader contract's version (1). A
 *          pack records it with each shader (docs/pack.md, SHADER) and the
 *          player refuses a shader built for another contract. It changes
 *          only when the wrapper's meaning changes; the test pins the
 *          wrapper's text, so a change to the text forces that decision.
 *   v0.10.2 - ygfx_default_cache_dir() is a wrapper of ysp/rt.h v0.7.0's
 *          yrt_user_dir(YRT_DIR_CACHE, "progcache"), which took over its
 *          rules unchanged. Needs ysp/rt.h v0.7.0.
 *   v0.10.1 - PROGRAM CACHE: ygfx_default_cache_dir() gives the per-user
 *          cache folder (it makes no file or folder). The describe line names
 *          the cache folder and gives hits and misses.
 *   v0.10.0 - NOISE: dist YGFX_SIMPLEX, 3D simplex gradient noise as fBm
 *          (noise_desc.scale, z, octaves, lacunarity, gain; stimulus fields
 *          gn_*; parameters YGFX_P_NOISE_SCALE, _Z, _LACUNARITY, _GAIN,
 *          appended), in 32-bit integers on the GPU, so that
 *          ygfx_simplex_value() gives the same bits on the CPU. Not
 *          band-limited (NOISE). Breaking: ygfx_stim grew; the noise
 *          program's cache key changed.
 *   v0.9.0 - NOISE, the noise dither and ygfx_noise_gauss() draw new
 *          patterns. The hash is triple32 (Chris Wellons, hash-prospector,
 *          public domain), not pcg_hash; the key of a point is its index
 *          times an odd key of the seed, XOR a second key, through one hash
 *          (ygfx_hash2(), ysp_hash2()), not three nested hashes. The
 *          nested form, hash(x ^ hash(y ^ hash(seed))), made 29 % of
 *          full-screen frames hold two rows that were copies of each other
 *          (each 2048-value block permuted), whatever the hash; PractRand
 *          failed it at 256 MB. The new form passes 16 GB, frame by frame
 *          and seed by seed (docs/gfx.md, v0.9). The same GPU cost.
 *          Breaking: every noise pattern, the dither pattern, and a user
 *          shader's ysp_hash() values; ygfx_noise_value() and the GPU
 *          still agree bit for bit.
 *   v0.8.0 - SETUP PASSES: ygfx_begin_setup() and ygfx_end_setup()
 *          run target passes outside a screen frame: a page of text, a
 *          blur layer, artwork rendered at setup. CURVE RUNS: a run whose
 *          items are all turned off the quarter turns, or an aliased run,
 *          takes the rays program by itself (the same bits, 0.5 to 0.8 ms
 *          less on a turned page); the rays program is made at
 *          ygfx_crun() for turned, aliased, grouped and YGFX_I_ORI
 *          runs. Breaking: none (a turned run's cache key set gains the
 *          rays program).
 *   v0.7.0 - CURVE RUNS: crun_desc.rays (YGFX_STIM_RAYS, appended) draws
 *          a run by the rays even where the exact area applies, on a second
 *          text program made at the first such run: a 12 px page 5.3 ms
 *          against 7.5. The text program works in px with y down, so a run
 *          drawn once into a target and composited at 1:1 on whole px has
 *          the run's coverage bit for bit. NOISE: dist YGFX_GAUSSIAN, a
 *          table of 65536 normal quantiles (256 KB, made at open), the same
 *          bits on the GPU and from ygfx_noise_value(). VECTOR: three
 *          specialized copies of the vector program for large RRECTs,
 *          dashed CIRCLEs and CIRCLE compounds (14 programs at open; the
 *          kinds worker's design, docs/gfx.md). ELLIPSE's quarter
 *          perimeter by the arithmetic-geometric mean: a pack 0.06 us, was
 *          36 us (1000 ELLIPSEs 0.06 ms of CPU, was 36 ms); the same floats.
 *          Fixed: none. Breaking: ygfx_crun_desc grew (rays); the text and
 *          noise programs' cache keys changed.
 *   v0.6.0 - CURVE SETS: ygfx_cset_make(), ygfx_cset_add(),
 *          ygfx_cset_free(), and with no gfx ygfx_cset_check() and
 *          ygfx_cset_winding(): one font or artwork set as two arrays
 *          (texels, words), the format of docs/gfx.md "Curve sets: the
 *          format" (version 1), uploaded as one RGBA32F and one RGBA32UI
 *          texture. CURVE RUNS: ygfx_crun() and ygfx_crun_desc, items
 *          ygfx_citem (ygfx_inst's layout with glyph in phase's place),
 *          kind YGFX_TEXT (until now not implemented), YGFX_P_SIZE,
 *          YGFX_STIM_ALIASED; ygfx_hit() and ygfx_hit_index() on a
 *          run. BLUR: ygfx_blur, ygfx_blur_desc, ygfx_blur_make(),
 *          ygfx_blur_apply(), ygfx_blur_free(); targets in R16F and
 *          RGBA32F; image_desc.coverage (YGFX_STIM_COVERAGE). The video
 *          program reads its constants as flat inputs: NV12 at 1080p costs
 *          1.10 to 1.16 times an RGBA8 image (was 1.64). Backend: the
 *          format YGFX_RGBA32UI, caps.max_texture (appended; 0 = 2048),
 *          a sampler in the vertex shader; version 3 still. Breaking:
 *          ygfx_stim grew (set, items, size); every program's cache key
 *          changed (the vertex text), so a cache compiles once again.
 *   v0.5.0 - COLOR: the calibration is ysp/color.h's. ygfx_cal and its
 *          calls are ycol_cal and ycol_cal_* (init, add, set_spectra,
 *          derive, check, save, load, nominal, lms, dir_cone, dkl_matrix,
 *          dir_dkl), YGFX_CAL_* and YGFX_GUN_* are YCOL_CAL_* and
 *          YCOL_GUN_*, ygfx_dkl_from_sph and ygfx_max_contrast are
 *          ycol_dkl_from_sph and ycol_max_contrast, and
 *          ygfx_cone_fundamentals(nm, lms) is
 *          ycol_cone_fundamentals(YCOL_CONES_SS2, nm, lms): the same
 *          code, the same bits, the same .yspcal bytes. desc.cal is a
 *          const ycol_cal*. ysp/color.h must be beside ysp/gfx.h.
 *          open() makes a ysp/color.h context (ycol_ctx) from desc.cal,
 *          desc.background and the new desc.cones and desc.lum, and takes
 *          PAINT's matrices and VIDEO's primaries from it;
 *          ygfx_color(g) returns it, so a caller converts colors and
 *          DKL directions with the background and luminance PAINT uses. A
 *          YGFX_EV_COLOR ring record at open logs it. desc.lum (a
 *          participant's luminance from flicker photometry) sets the DKL
 *          luminance axis for DKL_POLAR paint; desc.cones picks the 2 or
 *          10 degree cones. OKLAB paint now works on absolute XYZ, the
 *          display's black included, against the display's white:
 *          ysp/color.h's Oklab. A calibration whose black has no light
 *          gives the same pixels as v0.4 (every calibration in v0.4's
 *          test); one with 0.5 cd/m2 of black under a white of 80 moves
 *          the test's OKLAB gradient by up to 5.2e-3 of full scale. A calibration that
 *          is not sealed (no CRC after the last change) is refused at
 *          open: ycol_cal_derive() and ycol_cal_save() seal it.
 *   v0.4.0 - PROGRAM CACHE: desc.cache (a load and store pair the caller
 *          owns), ygfx_file_cache_init(), ygfx_program_stats(); an
 *          entry is checked by a second hash of its key and a hash of its
 *          binary before the driver sees it. VIDEO: YGFX_NV12 and
 *          YGFX_I420 textures with a stated encoding (matrix, range,
 *          transfer, primaries, siting) converted to linear light in the
 *          image program; encoded RGB textures; texture_update_planes(),
 *          texture_update_plane(), texture_import() (GL names; D3D11 RGBA
 *          and NV12 through ANGLE), texture_rebind(). INSTANCES:
 *          ygfx_instances(), ygfx_inst_grid(), ygfx_hit_index(),
 *          ygfx_inst_resolve(), desc.max_instances. ygfx_screen(),
 *          ygfx_calibrated(), ygfx_features(). Fixed (found by the
 *          gallery): ygfx_cal_derive() seals the CRC (the manual's recipe
 *          failed open with "CRC mismatch" until a save; cal_add() and
 *          cal_set_spectra() unseal it); a straight-alpha image drawn with
 *          linear filtering is filtered as premultiplied texels (clear
 *          texels darkened its edges; image_desc.premultiplied states a
 *          premultiplied texture); the range rule holds on DIST atlases
 *          with texture_desc.sdf_range (glyph runs and src rectangles on
 *          one without it are refused); a glyph run with no buffer read
 *          past the pool. Added: first (a run or dot field from any record
 *          of a shared buffer) on glyph and dot descs; shape_p on the
 *          grating, noise and user descs. Breaking: backend version 3
 *          (BACKENDS says what an out-of-tree backend changes);
 *          ygfx_texture_update() on a planar texture is refused; a
 *          straight-alpha image drawn linear looks different at its
 *          transparent edges (correct now). Also: end() reorders draws
 *          that do not overlap, so runs form across interleaved kinds
 *          (DRAW ORDER; identical pixels); a stimulus that can cover no
 *          pixel is refused with a message (v0.3 drew nothing); a dot
 *          field with no aperture set draws (v0.1 to v0.3 culled every
 *          dot); a POLYGON with no box takes its vertices' own; an empty
 *          dot field or glyph run returns OK; every refusal in draw()
 *          names its reason.
 *   v0.3.0 - the vector program (VECTOR SHAPES): SHAPE kinds RRECT, ARC,
 *          PIE, CAPSULE, NGON, STAR, ELLIPSE, QBEZIER, POLYLINE and
 *          COMPOUND (ygfx_compound(), up to 56 primitives folded by
 *          UNION, INTERSECT, SUBTRACT, XOR and smooth variants, onion);
 *          POLYGON fillets; offset (dilation) on every SHAPE; dashes,
 *          dash_offset, dash_snap, trim and caps; effects in one pass
 *          (drop shadow, glow, inner shadow, two bands, an exact blur for a
 *          sharp RECT's shadow); paint (LINEAR, RADIAL, ANGULAR, VERTEX;
 *          mixed in RGB, OKLAB or DKL_POLAR); blend modes OVER, ADD and
 *          MULTIPLY on color stimuli; ygfx_length(); the parameter tables
 *          ygfx_prim_params(), ygfx_fx_params(), ygfx_paint_params()
 *          and ygfx_bind.field. DISTANCE TEXTURES: MSDF and MTSDF masks
 *          with sdf_range and the range rule; a mask's src rectangle;
 *          ygfx_glyphs(), an instanced glyph run. The edge model's limits
 *          are stated and measured (EDGES, EXACTNESS). Backend version 2:
 *          deferred pipelines and pipeline_finish (ANGLE compiles the
 *          programs in parallel), bindings.blend, YGFX_BLEND_MULTIPLY,
 *          two vec4 instance attributes. Breaking: a backend must set
 *          version 2 and may leave pipeline_finish NULL; max_draws counts
 *          uniform blocks (a vector stimulus with data takes more than
 *          one); a USER stimulus with a MASK_TEX aperture loses p[28..31]
 *          to the mask's rectangle.
 *   v0.2.0 - strokes on every SDF shape and aperture (CENTER, INSIDE or
 *          OUTSIDE, the same edge profile on both sides of the band; ROUND
 *          or MITER joins, MITER on RECT, CROSS and convex POLYGON with a
 *          miter limit and a bevel beyond it); YGFX_MASK_TEX, a shape from
 *          a signed-distance texture, and ygfx_sdf_from_mask(); sprites:
 *          an IMAGE's source rectangle, nearest or linear, with no bleed;
 *          tint; flat groups (ygfx_group: place, anchor, box, x, y, ori,
 *          scale, opacity) with bindings; render targets
 *          (ygfx_target(), ygfx_begin_target(), ygfx_end_target(),
 *          ygfx_read_target()) and YGFX_STIM_ADD images; the YGFX_ADD
 *          shader mode; the GL state cache follows yscr_gl_epoch().
 *          Breaking: ysp_look.w in the shader contract is the stroke band's
 *          center offset, not visible; ygfx_stim and ygfx_bind grew
 *          (zero a ygfx_bind you fill field by field; an initializer list
 *          gets a NULL group for gcc -Wextra); the OVER blend now
 *          accumulates alpha (the scene's output is unchanged); IMAGE's
 *          linear sampling is by hand from four texelFetch, clamped to the
 *          source rectangle, not the sampler's.
 *   v0.1.0 - first release.
 *
 *   STATUS: v0.10.0 (docs/gfx.md "v0.10"): SIMPLEX on the GPU equals
 *   ygfx_simplex_value() at every pixel of 6 fields (227000 pixels; 1 to
 *   8 octaves, lacunarity 1 to 3, z negative and fractional) on the Iris
 *   Xe, WARP and SwiftShader. The integer form is within 1.4e-3 of the same
 *   noise in double. Full screen on the Iris Xe: 1.83 ms for one octave,
 *   6.56 ms for four, 12.96 ms for eight (UNIFORM 0.40).
 *   v0.9.0 (docs/gfx.md "v0.9" has the hash's evaluation):
 *   NOISE on the GPU equals ygfx_noise_value() at every check, for
 *   UNIFORM, BINARY and GAUSSIAN, on the Iris Xe, WARP and SwiftShader.
 *   v0.8.0 (docs/gfx.md "v0.8"). A page rendered in a setup
 *   pass equals the page rendered in a frame, target and composite, bit for
 *   bit; a blur applied in a setup pass equals one applied in a frame; the
 *   frame after a setup pass equals one with none before it, scene and
 *   output codes (Iris Xe, WARP, SwiftShader). Turned, aliased and
 *   item-turned runs on the rays program equal the exact-area program's
 *   pixels, bit for bit, on all three; a run with one item on a quarter
 *   turn, or with its items in a buffer, stays on the exact-area program.
 *   A 12 px page turned 15 degrees: 5.09 to 5.10 ms, was 5.83 to 6.03; a
 *   16 px CJK page turned: 5.92 to 5.94 ms, was 6.39 to 6.61.
 *   v0.7.0 (docs/gfx.md "v0.7" has the tables). A run with
 *   .rays draws what the program without the exact area draws, bit for bit;
 *   a run cached in an RGBA16F or RGBA32F target and composited at 1:1
 *   equals the run drawn, bit for bit, over black (Iris Xe, WARP,
 *   SwiftShader); over a gray background an RGBA16F target differs by one
 *   f16 step (its alpha was rounded before the blend), an RGBA32F one not
 *   at all. ysp/outline.h's set of the test glyphs, overlaps resolved:
 *   every glyph within 1.15e-5 of the exact coverage. NOISE GAUSSIAN: the
 *   table within 6e-8 (relative) of quantiles found by bisection on erfc,
 *   variance 1, symmetric bit for bit; 10^6 checks pass Kolmogorov-Smirnov
 *   at 1 % (D 0.00095); the GPU equals ygfx_noise_value() at every check
 *   on all three renderers. The specialized vector programs: 117 stimuli
 *   drawn by each and by the generic program, 0 values differ. Cost (Iris
 *   Xe, AC, the shared guard): a 12 px page (ysp/outline.h's set) 7.43 to
 *   7.50 ms exact, 5.28 to 5.31 ms with .rays, 0.50 ms composited from a
 *   target; GAUSSIAN noise full screen 0.41 to 0.42 ms against UNIFORM's
 *   0.37 to 0.39; RRECT fill 0.56 to 0.59 ms (was 0.95), a dashed circle
 *   0.50 to 0.53 (was 0.92 to 0.95), a compound of 8 with every effect 0.94
 *   to 0.98 (was 1.60 to 1.62); ygfx_open() 2.20 to 2.49 s, was 2.04 to
 *   2.12 (three more programs).
 *   v0.6.0 (docs/gfx.md "v0.6" has the tables). Curve runs
 *   against the exact box coverage of each pixel in double (scanlines,
 *   Gauss-Kronrod in y, the fill rule per scanline): on resolved glyphs
 *   not turned, the exact area is within 1.8e-5 of it; elsewhere the rays'
 *   mean error on edge pixels is 0.029, 0.019, 0.009 and 0.004 at 8, 12, 24
 *   and 48 px per em, the same at 0, 15 and 45 degrees (Lengyel's ramp
 *   alone: 0.031 to 0.074 at 8 px, the turned ones worst), and the largest
 *   error 0.56, at corners and self-intersections. Iris Xe, WARP and
 *   SwiftShader agree pixel by pixel within 6.5e-5 but for one SwiftShader
 *   pixel (0.087, where a ray passes a corner). The probe's 0.49
 *   difference between renderers came from interpolated em coordinates:
 *   with them, SwiftShader moved 26156 pixels by more than 1e-3 (up to
 *   0.48) from the Iris Xe; from gl_FragCoord, none. A curve run equals
 *   its items drawn alone and a buffer copied items, bit for bit; a glyph
 *   added at run time draws as one made at open; ygfx_hit() agrees with
 *   aliased coverage at all 64000 pixels. The blur against the exact blur
 *   of a rect (erf products): 0.20, 0.031, 0.0087, 0.0022, 5.4e-4 and
 *   1.5e-4 at sigma 0.5, 1, 2, 4, 8 and 16 (RGBA32F); a 2x source 0.038,
 *   0.0076, 0.0021, 5.4e-4, 1.4e-4 and 3.8e-5; 16-bit targets level off at
 *   about 8e-4. 29 deliberate faults in v0.6, each caught (docs/gfx.md);
 *   two first survived and led to a fix (a curve that turns back before a
 *   ray counted as crossing it).
 *   Cost on the Iris Xe at 1920 x 1200 (AC, the lock held, interleaved; GPU
 *   over an empty frame; backward lists): a 12 px Latin page of 26205
 *   glyphs 7.9 to 8.2 ms with the exact area, 5.5 to 5.8 ms by the rays
 *   alone; a 16 px CJK page of 7140 glyphs 10.0 to 10.2 and 7.2 to 7.7 ms;
 *   1612 glyphs at 48 px 1.7 ms; 45 glyphs at 200 px 1.1 ms (rays 1.6); a
 *   40-glyph line with its items copied each frame 0.09 ms of GPU and 17 us
 *   of CPU. The text program compiles in 430 ms (140 ms without the exact
 *   area) on ANGLE's D3D11; the program cache loads it after. The blur, full
 *   screen R16F: 1.42, 1.96, 3.34 and 5.96 ms at sigma 0.5, 1, 2 and 4;
 *   RGBA32F at sigma 2 3.57 ms; a 400 x 200 layer drawn, blurred and drawn
 *   0.10 to 0.27 ms at sigma 0.5 to 4 (1x), 0.17 to 0.66 ms (2x). Video
 *   1080p: NV12 0.64 to 0.68 ms, RGBA8 0.56 to 0.59 ms (v0.5: NV12 0.96).
 *   ygfx_open() and every v0.3 and v0.4 row: unchanged (interleaved
 *   against v0.5.0).
 *   v0.5.0: the calibration moved to ysp/color.h with the same
 *   bits: every scene and output readback of the test, hashed, is the same
 *   as v0.4's on the Iris Xe, WARP and SwiftShader, and v0.4's calibration
 *   code and ysp/color.h's agreed bit for bit before the move (52
 *   calibrations, 2100 raw calls, PAINT's matrices and the video
 *   primaries; docs/color.md). The new OKLAB black term: within 7.3e-7
 *   of an independent double reference on all three, on a calibration
 *   whose black has 0.5 cd/m2 under a white of 80, where v0.4's black-free
 *   form would have moved the gradient by 5.2e-3. What follows is v0.4's
 *   status, which v0.5 does not change.
 *   v0.4.0: built and run on one Windows 11 laptop (Intel Iris Xe,
 *   a 1920 x 1200 panel at 60 Hz) with the ANGLE in Docker Desktop's
 *   Electron folder (2.1.23876, git fffbc739779a) and with CI's, from
 *   Electron v38.8.6 (2.1.25848, git 85cc0e0b9cc3): the same numbers. No
 *   photometer, spectroradiometer, photodiode or Bits# has been used: no
 *   number here is a measurement of light.
 *   Pixels (tests/adapt/gfx_test.c, offscreen, every stimulus read back
 *   from a float32 scene against a CPU reference in double at pixel
 *   centers, positions through the inverse of the one coordinate transform)
 *   on four GL ES 3.0 renderers: ANGLE on D3D11 (Iris Xe), on D3D11 WARP,
 *   on Vulkan SwiftShader, and Mesa 23.2 llvmpipe (WSL2). Largest errors
 *   (Iris Xe, WARP, llvmpipe / SwiftShader): soft edges 1.7e-5 / 9.5e-5,
 *   grating 1.7e-5 / 1.4e-4, gabor 1.2e-6 / 8.8e-5, dots 1.5e-5 / 9.5e-5,
 *   strokes 1.6e-5 / 9.5e-5, MASK_TEX 1.4e-5 / 9.5e-5, linear sprites
 *   8.4e-7, tint 6.3e-8, group members against the same stimuli placed
 *   alone 6.0e-8 of full scale; images, noise, hard edges and hard bands,
 *   MITER and bevels against their CPU form, sprite borders (no bleed),
 *   shapes through a target against direct drawing, the output stage's
 *   codes (all 256, a CLUT, rounding at 0.005 above each halfway point, a
 *   CLUT through a target), the 8 x 8 ordered dither, the scene with v0.2's
 *   OVER alpha against v0.1's, the anchor's invariance and batched against
 *   single draws: exact. ygfx_hit() agrees with the GPU's coverage >= 0.5
 *   on every pixel not within 1e-3 px of an edge, strokes and groups
 *   included. ygfx_sdf_from_mask() equals a brute-force search. A present
 *   callback that changes GL state leaves the next frame exact (without
 *   the epoch, 901 values differ). docs/gfx.md has the tables and the
 *   tolerances. 14 deliberate faults in v0.1 and 18 in v0.2 were each
 *   caught.
 *   Calibration math against published values: sRGB primaries to XYZ
 *   within 3.9e-5 of IEC 61966-2-1's printed matrix; the CLUT of a display
 *   with sRGB's transfer function within 1.3e-3 (17 levels) and 3.6e-5 (64
 *   levels) of sRGB's encoding; the embedded cone fundamentals equal CVRL's
 *   linss2_10e_1; RGB to LMS, the DKL matrix and its unit axes within 7e-8
 *   (relative) of Psychtoolbox's ComputeDKL_M run in MATLAB R2023a on its
 *   own B_monitor spectra.
 *   Cost on the Iris Xe at 1920 x 1200 (examples/gfx/bench.c, GPU time from
 *   frames between two glFinish calls, AC and battery runs): an empty frame
 *   (scene clear and the output stage) 0.42 to 1.34 ms of GPU; 1000 gabors
 *   of 256 x 256 about 9.3 to 11 ms of GPU and 0.2 to 0.5 ms of CPU
 *   (batched; 2.1 to 5.0 ms one draw each); 100000 dots uploaded each frame
 *   1.5 to 2.0 ms of GPU and 0.65 to 1.3 ms of CPU; a full-frame RGBA8
 *   upload 1.1 to 2.6 ms of CPU. end()'s own CPU time for an empty frame is
 *   56 to 260 us mean, which misses the 10 us planned; the time is in
 *   ANGLE's calls, not broken down further. ygfx_open() 1.96 to 2.14 s
 *   in v0.3 (three runs; eleven programs, the vector program always
 *   included; ANGLE compiles them in parallel when every link starts
 *   before any status is read: a probe of four programs went from 7.3 s
 *   to 3.0 s. v0.2's nine took 1.3 to 1.4 s).
 *   v0.2 (AC, two runs): a stroke costs 1.05 to 1.17 times its fill's GPU
 *   time; a full-screen target drawn as an image 0.43 ms of GPU, so 1000
 *   gabors rendered once and added each frame cost 0.43 ms against 9.1 to
 *   9.2 ms drawn each frame; a target pass 4.7 us of CPU; 1000 sprites
 *   from one atlas 0.49 ms of GPU and 155 us of CPU.
 *   v0.3 pixels (the same method; docs/gfx.md has the tables, Iris Xe
 *   / WARP / SwiftShader / llvmpipe): the new kinds, filled and stroked,
 *   soft, against their CPU form 1.7e-5 / 8.2e-6 / 9.6e-5 / 8.2e-6; with
 *   dashes or trim 4.0e-4 / 4.0e-4 / 2.5e-3 / 5.8e-5 (the arc length's
 *   float error at a dash end); the exact kinds against F(brute-force
 *   Euclidean distance) 4.3e-6 / 4.9e-6 / 4.9e-6 / 4.9e-6; QBEZIER as
 *   chords 0.0050 px from the curve; effects in one pass against the same
 *   layers drawn apart 1.9e-5 / 5.7e-6 / 1.2e-4 / 5.7e-6; EXACT_BLUR
 *   against the erf product 1.5e-7 to 1.7e-7; paint RGB 1.2e-6, OKLAB
 *   against Ottosson's matrices through the calibration 1.8e-6, DKL_POLAR
 *   against ycol_cal_dir_dkl() 1.5e-5 / 6.3e-6 / 1.6e-4 / 1.3e-6,
 *   VERTEX against barycentric 1.1e-7; MSDF against the CPU's bilinear
 *   median 3.5e-6; glyph runs, batching and MULTIPLY: exact or 4.8e-8. The
 *   edge model against a supersampled Gaussian blur: 0.2500 at a right
 *   angle, 0.0406 and 0.0100 on discs of 5 and 20 SD, a union's crease
 *   0.0822 (0.0830 predicted), a MITER corner 0.0800, on all four. 26
 *   deliberate faults in v0.3: 24 caught on llvmpipe, one (the star's
 *   reflection) only on D3D11, one equivalent (EXACTNESS).
 *   v0.3 cost (Iris Xe, AC, GPU over an empty frame, full screen): v0.2
 *   rounded rect 0.40 ms, RRECT 0.93 ms (2.3x, the bar was 1.3x), with RGB
 *   paint 1.38 ms, OKLAB 1.54 ms (1.65x solid, bar 1.1x), STAR 0.86 ms,
 *   ELLIPSE 1.32 ms; an 8 px stroke 0.27 ms, dashed 0.90 ms; a compound of
 *   1, 2, 8 and 32 circles 0.78, 1.22, 3.53 and 12.4 ms (0.163 ms per
 *   primitive per Mpx, bar 0.15); a compound of 8 in a 600 x 400 box 0.62
 *   ms, with every effect 1.62 ms (bar 1.0); 100 compounds of 4, 120 us of
 *   CPU (bar 600); a run of 200 MSDF glyphs 0.12 ms of GPU and 18 us of CPU
 *   (bars 0.2 ms, 20 us).
 *   In an 800 x 600 window on the composition swapchain, 1 minute each
 *   (v0.1): DRAW p99 0.48 to 0.79 ms with up to 100 gabors or 10000 dots, 0
 *   to 2 late targets per minute. No C runtime heap call in 566 frames of
 *   an MSVC debug build (v0.1; a control with one malloc per frame counted
 *   569).
 *   Builds, warnings as errors: MSVC 19.44 (CMake) and 19.51 (C11 and
 *   C++17), MinGW-w64 gcc 16.1 (C99, C11, C++17), gcc 11.4 on WSL2 (C99,
 *   C11, C++17, ASan and UBSan), emcc 6.0.10 (C11 run in node; C++17
 *   compiled); v0.3 again on each. Under ASan the
 *   test leaves GL out unless YGFX_TEST_DEVICES names it: Mesa leaks 112
 *   bytes from a module it unloads.
 *   NOT done: 10-bit, Mono++ and Color++ output (refused: ysp/screen.h makes
 *   RGBA8 back buffers, this panel's link is 8 bits per color, and no
 *   device was there to verify); stereo (refused); text layout (a glyph
 *   run draws placed glyphs; there is no atlas tool yet); cubic Beziers;
 *   filtered noise on the GPU; fullscreen runs; specialized vector programs
 *   past the three that met their bars (docs/gfx.md); any other GPU,
 *   macOS, X11 or Wayland (ysp/screen.h's stubs), WebGL; v0.4 on WSL,
 *   llvmpipe and emcc.
 *   v0.4 (docs/gfx.md has the tables; Iris Xe, WARP, SwiftShader):
 *   the program cache: every frame drawn from cached programs equals the
 *   same frame compiled, bit for bit, and a damaged, truncated, colliding
 *   or foreign entry is compiled instead (11 cases); ygfx_open() 11.4 to
 *   35 ms warm against 2.3 to 5.3 s without a cache (AC). Video: NV12 and
 *   I420 through 3 matrices, 2 ranges, 3 sitings and 2 chroma filters
 *   within 3.1e-7 of a double reference; the transfers 7.5e-7; primaries
 *   through a calibration 1.3e-6; the 256 codes through TRC_DEVICE and a
 *   CLUT exact; D3D11 NV12 import (an array slice, an NT handle with a
 *   keyed mutex, a legacy shared handle) 5.2e-7. 1080p: NV12 0.91 ms of
 *   GPU and 0.30 to 0.39 ms to update, RGBA8 0.56 ms and 0.91 to 1.03 ms.
 *   Instances: each element within 9.2e-6 of the same stimulus drawn
 *   alone, the palette's integer entries exact, ygfx_hit_index() equal
 *   to the GPU's coverage; 10000 gabors 177 to 191 us of CPU and 1.26 to
 *   1.33 ms of GPU (5.5 ms one stimulus each). The draw order: 20 frames
 *   reordered equal call order bit for bit; 1000 interleaved stimuli of 4
 *   kinds 1.1 ms of GPU and 0.47 ms of CPU (7.2 and 3.3 in call order).
 *   35 deliberate faults in v0.4, each caught. Fixed: v0.1's dot fields
 *   with no aperture set drew nothing (aperture 0 was a 0 x 0 RECT): the
 *   dot rows measured before v0.4 are void.
 *   Outside this block and docs/gfx.md, a number in this header is a
 *   measurement only where the text says "measured".
 *
 *   ---------------------------------------------------------------------
 *   USAGE
 *   ---------------------------------------------------------------------
 *   Do this:
 *
 *       #define YSP_GFX_IMPLEMENTATION
 *
 *   in *one* C or C++ file before including this header. ysp/screen.h's and
 *   ysp/rt.h's implementations come with it, once.
 *
 *   A gabor in the middle of a mid-gray window (examples/gfx/hello.c):
 *
 *       #define YSP_GFX_IMPLEMENTATION
 *       #include "ysp/gfx.h"
 *
 *       static yscr_screen scr;
 *       static ygfx_gfx gfx;
 *
 *       // yscr_open() calls it before it settles: the programs compile
 *       // meanwhile (OPEN START)
 *       static void start_gfx(void* gd, yscr_screen* s) {
 *           ((ygfx_desc*)gd)->screen = s;
 *           ygfx_open_start(&gfx, (ygfx_desc*)gd);
 *       }
 *
 *       int main(void) {
 *           ygfx_desc gd = { .background = { 0.5f, 0.5f, 0.5f } };
 *           if (!yscr_open(&scr, &(yscr_desc){ .windowed = true,
 *                              .on_context = start_gfx, .on_context_ctx = &gd })) return 1;
 *           if (!ygfx_open(&gfx, &gd)) return 1;
 *           ygfx_stim g = ygfx_gabor(&(ygfx_gabor_desc){
 *               .sf = 1 / 32.0f, .sigma = 32, .contrast = 0.5f });
 *           yscr_frame f;
 *           while (yscr_begin(&scr, &f) == YSCR_OK) {   // Shift+Esc ends it
 *               ygfx_begin(&gfx, &f);
 *               ygfx_draw(&gfx, &g);
 *               ygfx_end(&gfx);
 *               yscr_flip(&scr);
 *           }
 *           ygfx_close(&gfx);
 *           yscr_close(&scr);
 *       }
 *
 *   PsychoPy and Psychtoolbox, for comparison:
 *
 *       win = visual.Window(units='pix', color=0)          # PsychoPy
 *       g = visual.GratingStim(win, tex='sin', mask='gauss', sf=1/32,
 *                              size=256, contrast=0.5)
 *       while not event.getKeys(['escape']):
 *           g.draw(); win.flip()
 *
 *       PsychImaging('PrepareConfiguration');               % Psychtoolbox
 *       PsychImaging('AddTask', 'General', 'FloatingPoint32BitIfPossible');
 *       win = PsychImaging('OpenWindow', 0, 0.5);
 *       tex = CreateProceduralGabor(win, 256, 256, 0, [0.5 0.5 0.5 0]);
 *       while ~KbCheck
 *           Screen('DrawTexture', win, tex, [], [], 0, [], [], [], [], ...
 *                  kPsychDontDoRotation, [180, 1/32, 32, 50, 1, 0, 0, 0]);
 *           Screen('Flip', win);
 *       end
 *
 *   The differences: PsychoPy's 'gauss' mask has SD = size / 6 (42.7 px
 *   here); here sigma is stated. PTB scales its procedural gabor's contrast
 *   by its normalization options. Coordinates: here the origin is the
 *   top-left corner, y down, and ori turns clockwise on the screen;
 *   PsychoPy's origin is the center with y up, and its ori is clockwise
 *   from vertical; PTB's origin is top-left, y down, angles clockwise. Both
 *   draw for "the next flip"; here a frame is drawn for f.onset, the
 *   predicted onset of its own flip.
 *
 *   A trial with timeline bindings (examples/timeline/gfx_trial.c runs it):
 *
 *       static ygfx_stim grating;
 *       static const ygfx_bind binds[] = {
 *           { .stim = &grating, .param = YGFX_P_VISIBLE,  .channel = GRATING_ON }, // onset, offset
 *           { .stim = &grating, .param = YGFX_P_CONTRAST, .channel = CONTRAST   }, // a tween or keys
 *       };
 *       ...
 *       yscr_begin(&scr, &f);
 *       ytl_evaluate(&tl, &(ytl_frame){ f.onset, f.period, f.index }, fired, 8);
 *       ygfx_apply(binds, 2, ytl_values(&tl));
 *       yscr_mark(&scr, YSCR_PHASE_EVALUATE);
 *       ygfx_texture_update(&gfx, noise, 0, 0, w, h, next_noise, 0);  // if any
 *       yscr_mark(&scr, YSCR_PHASE_UPLOAD);
 *       ygfx_begin(&gfx, &f);
 *       ygfx_draw(&gfx, &grating);
 *       ygfx_end(&gfx);
 *       yscr_flip(&scr);                    // unmarked time goes to DRAW
 *
 *   A drifting carrier takes its phase from the predicted onset, computed
 *   in double on the CPU, so no shader needs absolute time:
 *
 *       g.phase = (float)fmod(tf_hz * (f.onset - t0) * 1e-9, 1.0);
 *
 *   ---------------------------------------------------------------------
 *   MODEL
 *   ---------------------------------------------------------------------
 *   TWO STAGES
 *     Stimuli draw into a float scene target (RGBA16F: 11 bits of mantissa,
 *     a quarter of an 8-bit code at worst; an RGBA32F scene measured 1.4 to
 *     1.7 times the GPU time on blend-heavy frames) in linear device RGB:
 *     per gun, 0..1, proportional to that gun's light after linearization.
 *     The output stage then reads the scene once per pixel, looks each
 *     channel up in the CLUT, dithers if asked, quantizes to 8 bits and
 *     writes framebuffer 0, the screen's back buffer. Gamma lives
 *     only there: SDL3 has no gamma ramps.
 *   TWO BLENDS
 *     A modulation stimulus (GRATING, GABOR, NOISE, an IMAGE with
 *     modulation, a MODULATION user shader) adds
 *         contrast * gate * g(p) * aperture(p) * dir
 *     to the scene, g in [-1, 1]. Overlapping gabors add in linear light.
 *     dir all 0 means the background, so contrast is Michelson luminance
 *     contrast around it: L = Lbg (1 + contrast g). The calibration gives
 *     dir for a cone-contrast or DKL direction. A color stimulus (SHAPE,
 *     DOTS, a color IMAGE, a COLOR user shader) is blended over the scene
 *     with coverage * opacity * gate.
 *   DRAW ORDER
 *     Call order wherever stimuli overlap; draws that do not overlap may be
 *     reordered, with identical pixels. No depth buffer. visible < 0.5
 *     skips a draw. (v0.4) end() lets a draw join an earlier run of its kind
 *     when its bounds meet none of the draws it moves past: then no pixel
 *     gets both, and blending reads only its own pixel, so no value can
 *     change, whatever the blend. The bounds are the draw's quad (half size
 *     plus the margin of its edge, stroke, offset and effects, turned) plus
 *     1 px; instanced draws, dot fields and glyph runs have the whole pass
 *     as bounds, so nothing passes them. Target passes keep their order.
 *     Tested bit for bit against call order (docs/gfx.md).
 *   NOTHING READS A CLOCK
 *     What a frame shows is a function of the yscr_frame given to begin():
 *     f.onset, f.index, f.vblank. No call in the frame waits for the GPU
 *     (no glFinish, no glGet, no readback that waits), so drawing cannot
 *     move a flip; a GPU that cannot keep up shows as SWAP time and drops in
 *     the flip record. The reads (ygfx_read_scene, _output) block, for
 *     tests. CAPTURE's copies are polled, never waited for; its map costs
 *     CPU time in end() (2.6 ms at 1920 x 1200 here) unless desc.manual.
 *   COST
 *     draw() packs the stimulus into a 256-byte uniform block and queues
 *     it. end() uploads the frame's blocks in one call, then draws each run
 *     of queued stimuli that share a program and textures as one instanced
 *     draw, up to 16 per draw, in order (measured: 1000 gabors cost 0.2 to
 *     0.5 ms of CPU this way, 2.1 to 5.0 ms one draw each; a longer batch
 *     makes ANGLE's compile slower: 64 took 2.2 s to open, 256 took 19 s).
 *     A vector stimulus whose data fills more than its block (VECTOR
 *     SHAPES) takes the blocks after it and draws alone. Runs form across
 *     interleaved kinds where they do not overlap (DRAW ORDER); a large
 *     array of one stimulus is cheaper as INSTANCES.
 *
 *   ---------------------------------------------------------------------
 *   COORDINATES
 *   ---------------------------------------------------------------------
 *   Screen pixels: the origin is the top-left corner of the screen (or of
 *   the headless target), x right, y down, pixel centers at + 0.5. Units
 *   are pixels, or degrees (desc.units = YGFX_DEG): one scale factor,
 *   the pixels per degree at the screen center from desc.view, with the
 *   same origin and axes.
 *   ori is in degrees and turns +x toward +y: clockwise on the screen. ori
 *   0 puts a carrier along x (vertical bars).
 *   Two fields place a stimulus:
 *     place     the screen point x, y are measured from: one of nine
 *               (YGFX_CENTER, the default, YGFX_TOP_LEFT, ... ,
 *               YGFX_BOTTOM_RIGHT). x, y are along the screen axes, so +y
 *               is down from it. .x = 100 is 100 px right of the center;
 *               .place = YGFX_TOP_LEFT, .x = 10, .y = 10 is 10 px in from
 *               that corner. Not bindable.
 *     anchor    the point of the stimulus's own box that sits at x, y, and
 *               the point it turns about: ax, ay as fractions of the box,
 *               0, 0 its top-left corner and 1, 1 its bottom-right (y
 *               down). The same idea as Phaser's setOrigin and Pixi's
 *               sprite.anchor; the default here is the center, 0.5, 0.5, as
 *               in Phaser (Pixi's default is the top-left). A desc takes it
 *               as one of the nine names; the stimulus holds ax, ay as
 *               bindable floats.
 *   The box is w x h for shapes, gratings, noise, images and user shaders,
 *   the 8-sigma quad for a gabor unless w, h are set, and the field's
 *   aperture box for DOTS (the anchor moves the field, not each dot).
 *   Shapes, apertures and carriers are defined about the box's own
 *   center, so moving the anchor moves and turns a stimulus but does not
 *   change its pattern.
 *   ygfx_resolve() gives the anchor's screen position, ygfx_bounds()
 *   the turned box's axis-aligned bounds, ygfx_hit() whether a screen
 *   pixel is inside (where the GPU's coverage is 0.5 or more, by the same
 *   SDF on the CPU), ygfx_center() and ygfx_size() the screen's.
 *   SDL's mouse events are in window coordinates. At a pixel density of 1
 *   they are these pixels; at another density, multiply them by
 *   SDL_GetWindowPixelDensity() first (STATUS gives what this machine
 *   reported).
 *   The convention lives in one function of the implementation,
 *   ygfx__place() and its helpers; the shaders take what it computes.
 *
 *   ---------------------------------------------------------------------
 *   GROUPS
 *   ---------------------------------------------------------------------
 *   A ygfx_group is a plain value, like a stimulus: place, anchor (ax, ay
 *   of its box), box w x h (units; 0 x 0 is a point), x, y, ori, scale,
 *   opacity and visible. A stimulus joins one with .group = &group (NULL is
 *   none). For a member:
 *     its place is a point of the group's box, not of the screen, and its
 *       x, y and ori are in the group's frame;
 *     the group's frame is its anchor at its own place point plus (x, y),
 *       turned by its ori and scaled by its scale;
 *     scale applies to everything in units (positions, sizes, sigma, shape
 *       parameters, dot positions; sf is divided by it), never to what is
 *       in px (edge_width, stroke, dot_size, check): a px value sets the
 *       stimulus's spatial-frequency content at the panel. Pixi's
 *       container.scale zooms everything; this one does not;
 *     opacity multiplies each member's gate (a color member's alpha and a
 *       modulation member's contrast). It is not a flattened layer: two
 *       overlapping members at 0.5 do not look like one layer at 0.5. A
 *       flattened layer is a target (TARGETS) drawn with opacity.
 *   One level: a group has no group. A tree would need a walk per draw and a
 *   rule for cycles, and a bound channel on a group would then move the
 *   screen by an amount that depends on its ancestors. Deeper composition
 *   goes through a target. A ygfx_bind with .group set binds a group
 *   field (YGFX_G_*), so one channel moves every member on the next draw.
 *   ygfx_resolve(), _bounds(), _local() and _hit() go through the group.
 *
 *   ---------------------------------------------------------------------
 *   TARGETS
 *   ---------------------------------------------------------------------
 *   ygfx_target() makes a render target at setup (RGBA16F, RGBA8, R16F,
 *   or RGBA32F where the renderer has EXT_float_blend),
 *   cleared to transparent black. Between ygfx_begin() and ygfx_end()
 *   (or in a setup pass, below),
 *   ygfx_begin_target(t, clear) and ygfx_end_target() draw into it with
 *   the normal calls (clear NULL keeps what it holds, so "draw once, sample
 *   every frame" works); then ygfx_image() draws it like any texture. The
 *   rules:
 *     coordinates in a target are its own: origin its top-left corner, y
 *       down, place points of its box, the same px per unit;
 *     its rows are top first, as an uploaded image's, so it is sampled and
 *       read (ygfx_read_target) with no flip;
 *     it holds scene-space linear values: no CLUT, dither or quantization
 *       runs in it; the CLUT applies once, in the output stage. RGBA8 keeps
 *       linear values in 1/255 steps, coarse in the dark: use it for masks
 *       and flat colors; RGBA16F holds negative increments too;
 *     color is premultiplied: draws accumulate alpha, and ygfx_image() on
 *       a target sets YGFX_STIM_PREMULTIPLIED;
 *     a target filled with modulation stimuli is a layer of increments:
 *       draw it with .add (YGFX_STIM_ADD), which adds texel.rgb scaled by
 *       contrast, gate, tint and aperture;
 *     a draw that samples the target being drawn is refused
 *       (YGFX_ERR_ORDER): GL calls that a feedback loop. A target sampled
 *       before the pass that writes it in the same frame shows the previous
 *       frame: call order decides;
 *     at most YGFX_MAX_PASSES (16) target passes per frame, not nested;
 *       nothing is allocated for them in the frame loop.
 *   SETUP PASSES (v0.8): ygfx_begin_setup() .. ygfx_end_setup() fills
 *   targets outside a screen frame, at setup or between trials:
 *       ygfx_begin_setup(&gfx);
 *       ygfx_begin_target(&gfx, page, (const float[4]){ 0, 0, 0, 0 });
 *       ygfx_draw(&gfx, &text);
 *       ygfx_end_target(&gfx);
 *       ygfx_end_setup(&gfx);
 *   Inside one: target passes (16, not nested, as in a frame), draws into
 *   them and ygfx_blur_apply(). Refused (YGFX_ERR_ORDER): a draw into
 *   the scene, ygfx_end(), ygfx_begin(), a second begin_setup, and
 *   begin_setup inside a frame. end_setup() runs the passes and touches
 *   neither the scene, the output stage, the present nor the frame count;
 *   the next frame is the same, bit for bit, as one with no setup pass
 *   before it (tested). The passes see a frame of time 0 and index 0. It
 *   makes the GL context current and allocates nothing.
 *
 *   ---------------------------------------------------------------------
 *   STIMULI
 *   ---------------------------------------------------------------------
 *   One plain struct, ygfx_stim, for every kind; set fields directly.
 *   Constructors take a desc whose zero fields mean defaults, and return a
 *   stimulus with real values in every field (visible, gate, contrast,
 *   opacity, aspect 1; anchor 0.5, 0.5). After construction a 0 is a 0.
 *   SHAPE    an SDF shape in a solid color: RECT (corner radius shape_p[1]),
 *            CIRCLE (diameter w), ANNULUS (inner radius shape_p[0]), LINE
 *            (length w, width shape_p[0], round caps), POLYGON (shape_p[0]
 *            vertices, 3 to 16, x, y pairs in p[] from the box center), CROSS
 *            (arm width shape_p[0]). The v0.3 kinds, compounds, offsets,
 *            dashes, effects, paint and blend modes: VECTOR SHAPES.
 *   GRATING  sine, or square (wave 1), cos(2 pi (sf x + phase)) along the
 *            box's x from its center, in the shape as an aperture.
 *   GABOR    cos(2 pi (sf x + phase)) exp(-(x^2 + (aspect y)^2) / 2 sigma^2);
 *            the quad is 8 sigma unless w, h are set: the envelope is
 *            3.4e-4 at the cut, 0.04 of an 8-bit code at contrast 1.
 *   DOTS     count dots, each an SDF circle of dot_size px with the edge
 *            profile, at x, y float pairs (units, from the field's center)
 *            in a ygfx_buffer; drawn when the dot's center is in the
 *            field's aperture (RECT, CIRCLE or none). One instanced draw.
 *            The aperture is none unless set with a w (aperture 0 is
 *            YGFX_RECT; before v0.4 that zero default culled every dot).
 *            first picks the first x, y pair of buf; count 0 draws nothing.
 *   No area  a stimulus whose box, width or ring can cover no pixel is
 *            refused by draw() with a message naming the field (v0.4): it
 *            is never drawn as nothing. A POLYGON with no w, h takes its
 *            vertices' extent.
 *   IMAGE    a texture, or its source rectangle src (texels from the
 *            top-left; a sprite from an atlas), 1:1 by texelFetch unless
 *            linear; color, the red channel as g (modulation), or rgb as an
 *            increment (add). Nearest reads one texel by its integer
 *            address, clamped to the rectangle; linear interpolates by hand
 *            from four texelFetch whose addresses are clamped to the
 *            rectangle: neither can read a neighbor, so an atlas needs no
 *            padding (the outer half texel repeats the edge texel, as
 *            clamping to the edge does). No mipmaps: draw sprites at 1:1 or
 *            larger. tint multiplies linear light: color rgb by tint.rgb,
 *            coverage by tint.a; for a modulation, dir by tint.rgb and the
 *            amplitude by tint.a (the same as dir and contrast); for add,
 *            the increment by tint.rgb and tint.a. 16-bit planes are R16UI,
 *            read as k / 65535. Values are linear, unless texture_desc.enc
 *            states a transfer (an 8-bit sRGB photograph: VIDEO).
 *   NOISE    per check of `check` px from the box's top-left corner (its
 *            key ygfx_hash2(cx, cy, seed): independent checks, no row or
 *            frame a copy of another; v0.9):
 *            uniform in (-1, 1), binary +-1, or (v0.7) GAUSSIAN, N(0, 1)
 *            as one of 65536 equally likely values (the normal quantiles
 *            at the bins' centers, scaled to variance 1, largest 4.33),
 *            from an integer hash of the check and seed, bit for bit what
 *            ygfx_noise_value() gives. contrast is the SD for GAUSSIAN;
 *            values past 1 / contrast leave the gamut, which the per-draw
 *            check counts. GAUSSIAN costs 8 % more than UNIFORM at 1 px
 *            checks (a texelFetch).
 *            SIMPLEX (v0.10): 3D simplex gradient noise, summed over
 *            octaves (fBm): octave k has lattice cells of scale / lacunarity^k
 *            units and weight gain^k, the weights summing to 1. scale is
 *            the lattice cell (deg or px, as other units); z the third
 *            coordinate in cells: bind it (noise_z) to make the field
 *            evolve; the field decorrelates over half a cell of z (r 0.92
 *            at 0.1 cells, 0.60 at 0.25, 0.07 at 0.5). The seed changes
 *            every octave's lattice keys. Values in [-1, 1]; the SD is
 *            0.425 for one octave and 0.317, 0.278, 0.261, 0.249, 0.246 for
 *            2, 3, 4, 6, 8 octaves (lacunarity 2, gain 0.5): contrast scales
 *            it. NOT BAND-LIMITED: one octave's power spreads from about
 *            1/8 to 2 times the lattice frequency (peak at 0.64 of it) with
 *            tails, and the cube-edge gradients make it anisotropic (the
 *            power of 12 angle sectors 0.93 to 1.10 of their mean,
 *            measured). For a stated band,
 *            use filtered noise (an IMAGE of noise filtered on the CPU) or
 *            gratings. Exact: ygfx_simplex_value(g, s, ix, iy) gives the
 *            GPU's value, bit for bit, at pixel (ix, iy) of a box not
 *            turned and on whole px (any ori: the same noise, sampled at
 *            turned points). The lattice repeats every 65536 cells in x and
 *            y, and z is used modulo 65536 cells. Refused: scale <= 0, a
 *            cell finer than 1/32 px at the last octave or above 32768 px,
 *            octaves outside 1..8, lacunarity outside 1..8, gain outside
 *            (0, 1], a POLYGON aperture (its vertices and the octaves share
 *            the block; MASK_TEX does that shape). Cost per octave about
 *            1.6 ms full screen at 1920 x 1200 (Iris Xe; STATUS): size the
 *            box, and render a static field once in a setup pass.
 *   USER     a pipeline from ygfx_pipeline() (SHADER CONTRACT).
 *   TEXT     a curve run (CURVE RUNS): glyphs or paths of a curve set at
 *            the caller's pen positions; layout is the pack tool's or
 *            Skribidi's. The MSDF glyph run (GLYPH RUNS) stays.
 *   MASK_TEX (any kind's shape) the signed-distance texture `mask` (R16F
 *            or R32F, distance in texels, negative inside, with padding),
 *            fitted to the box: px per texel = box width / texture width.
 *            A box of another aspect ratio than the texture's is refused.
 *            Nothing outside the box is drawn: the padding holds what an
 *            outer stroke or a soft edge reaches. ygfx_sdf_from_mask()
 *            makes one from a mask; ygfx_hit() does not see masks. MSDF
 *            and MTSDF atlases and a mask's src rectangle: DISTANCE
 *            TEXTURES.
 *   EDGES (edge, edge_width in px), centered on the nominal boundary, so
 *   the coverage is 0.5 at the stated size:
 *     HARD      1 where the pixel center is inside, else 0. No smoothing.
 *     COSINE    raised cosine from 1 to 0 over edge_width.
 *     GAUSSIAN  the hard edge blurred by a Gaussian of SD edge_width:
 *               0.5 erfc(d / (edge_width sqrt 2)).
 *   The edge sets the stimulus's spatial-frequency content, so state it.
 *   What the profile is, exactly: coverage = F(d), F the profile and d the
 *   signed distance to the boundary. Two limits (measured in the test
 *   against references that share none of this arithmetic; docs/gfx.md):
 *     F(d) is the Gaussian blur of the shape only on a straight edge. At a
 *       vertex of interior angle a the blur is a / 360 and F(0) is 0.5: 0.25
 *       of the step at a right angle, 0.333 at 60 degrees, above 1/255 of
 *       the step within 3.75 SD of a right-angled vertex. On a disc of
 *       radius R the blur is about 0.2 SD / R below F(d) at the boundary
 *       (0.041 at R = 5 SD, 0.010 at 20 SD). The same holds for COSINE.
 *     d is the Euclidean distance for every v0.2 shape, with one
 *       exception (the v0.3 kinds and operations: EXACTNESS): MITER's
 *       field (RECT, CROSS, POLYGON) is the largest distance to the
 *       edges' lines, Euclidean inside, but outside a corner a lower bound,
 *       so the outer edge is up to 0.083 of the step too high there (at 1.2
 *       SD from a right-angled corner along its bisector; above 1/255
 *       within 3.75 SD), whatever the stroke or edge width.
 *   A HARD edge needs only the sign of d: it is exact for every shape.
 *   STROKES (stroke px wide; stroke_align): the band between two offsets
 *   a1 < a2 of the boundary: CENTER -s/2..s/2, INSIDE -s..0, OUTSIDE 0..s.
 *   Its coverage is the fill's at a2 minus the fill's at a1: the hard band
 *   blurred by the same profile, so both sides get the same edge, and a
 *   band narrower than the edge never reaches 1 (as a blurred thin line
 *   does not). A stroke replaces the aperture of every kind: an outline, a
 *   grating or noise in a ring, ring dots. Joins: the SDF's own (ROUND)
 *   gives a round corner on the outside of a turn and a sharp one inside
 *   (a 4 px OUTSIDE stroke on a square has round outer corners of radius
 *   4 px). MITER keeps outer corners sharp on RECT (no corner radius), CROSS
 *   and a convex POLYGON, with a bevel where the miter would pass
 *   miter_limit times the stroke width (0 means 4, SVG's default). A right
 *   angle's miter is sqrt 2 times the width, so RECT and CROSS bevel only
 *   below a limit of 1.414.
 *   MITER on a curve, a rounded rect or a concave polygon is refused.
 *   ygfx_hit() tests the hard band.
 *
 *   ---------------------------------------------------------------------
 *   VECTOR SHAPES (v0.3)
 *   ---------------------------------------------------------------------
 *   One more built-in program, the vector program, draws a SHAPE from
 *   closed-form distance functions with their gradients and arc lengths.
 *   It is always compiled. A SHAPE goes to it when it has a v0.3 kind, a
 *   POLYGON with fillets, fx, a paint other than SOLID, dashes or trim;
 *   other SHAPEs stay on v0.2's program and draw as before. Sizes are in
 *   units unless the text says px.
 *     RRECT     w x h; shape_p = the corner radii top-left, top-right,
 *               bottom-right, bottom-left.
 *     ARC       a ring segment with round ends: outer diameter w;
 *               shape_p[0] sweep (degrees), [1] thickness, [2] start
 *               (degrees, +x toward +y).
 *     PIE       a disc sector: diameter w; [0] sweep, [2] start.
 *     CAPSULE   length w, end to end; [0], [1] the radii of the left and
 *               right ends (an uneven capsule fits its box).
 *     NGON      a regular polygon: circumdiameter w; [0] sides (3 to 64),
 *               [1] corner rounding radius.
 *     STAR      outer diameter w; [0] points (3 to 64), [1] the inner
 *               radius over the outer, [2] corner rounding radius.
 *     ELLIPSE   w x h, aspect up to 20. An iteration (0xfaded's): 3, 6 or 8
 *               steps for aspects up to 2, 4.5 and 20 (distance error under
 *               1e-5 px, measured at 2:1 and 9:1).
 *     QBEZIER   path = 3 points (x, y pairs from the box center); [0] the
 *               stroke width. Drawn as chords within 0.005 px of the curve
 *               (at most 223; a curve that needs more is refused).
 *     POLYLINE  path = 2 to 224 points; [0] the width; join ROUND, MITER
 *               (with miter_limit) or BEVEL; cap BUTT, ROUND or SQUARE.
 *     POLYGON   shape_p[1] > 0 rounds every corner with a fillet of that
 *               radius, convex or concave.
 *   offset (px, every SHAPE, v0.2 kinds and glyph runs included) moves the
 *   boundary out (+) or in (-) before the stroke and the edge: a dilated
 *   or eroded shape. The quad grows with it.
 *   The block: a stimulus packs into its 256-byte block and, when its data
 *   needs it, the blocks after it (up to 247 vec4 of data: polygon
 *   vertices, path points, primitives, effects, paint). Such a stimulus is
 *   one instanced draw of its own; a vector stimulus with one block batches
 *   with its neighbors. desc.max_draws counts blocks, not draws.
 *   ygfx_hit(), _bounds() and _local() use the same field on the CPU.
 *   Three specialized copies of the vector program draw three large cases
 *   in less GPU time: an RRECT with no dashes, trim, paint or fx; a CIRCLE
 *   with dashes or trim and no paint or fx; a compound of CIRCLEs only, with
 *   no paint. Each is the same text with what its case cannot reach left
 *   out, so its pixels equal the generic program's (tested bit for bit). A
 *   draw takes one only when its quad covers 65536 px or more (256 x 256):
 *   below that, the program switch costs more CPU than the stimulus saves.
 *   They compile at open with the others (14 programs); desc.cache keeps
 *   them. An element array (INSTANCES) keeps the generic program.
 *
 *   COMPOUNDS
 *   ygfx_compound() makes a SHAPE of kind COMPOUND from an array of
 *   ygfx_prim (48 bytes each, 1 to YGFX_MAX_PRIMS = 56). Each primitive
 *   has a kind (CIRCLE, ANNULUS, RECT, RRECT, LINE, CAPSULE, CROSS, ARC,
 *   PIE, NGON, STAR, ELLIPSE), a center, w, h, ori and p[4] as shape_p, in
 *   the compound's box frame (units from the box center). The fold runs
 *   left to right: primitive i meets everything before it by its op,
 *   UNION, INTERSECT, SUBTRACT (removes primitive i), XOR, or a SMOOTH_
 *   op with blend radius k (Quilez's quadratic smooth minimum). After a
 *   smooth op the field is divided by its gradient's length, so the edge
 *   keeps its stated width through the blend (EXACTNESS). onion > 0 makes
 *   a primitive a shell |d| - onion; desc.onion does it to the result. The
 *   box places and anchors the compound; a primitive outside the box is
 *   still drawn (the quad grows to it). Keep the array alive: the stimulus
 *   points at it. Bind a primitive's float with ygfx_bind.field and the
 *   offsets in ygfx_prim_params(). There is no CSG stack: a fold is
 *   linear. A compound has no length, so no dashes or trim.
 *
 *   EFFECTS
 *   stim.fx points at a ygfx_fx (px; NULL = none). From the same field,
 *   in one pass, back to front: drop (a shadow at dx, dy screen px, behind
 *   the shape), glow (the same, at no offset), the fill or stroke, inner
 *   (a shadow inside the fill, from dx, dy), then band[0] and band[1]
 *   (outlines between offsets a1 < a2 of the boundary, on top). A shadow
 *   is the silhouette moved out by spread and blurred by sigma (0 = a
 *   hard silhouette): coverage F at offset spread, with a Gaussian F of SD
 *   sigma. It is a true blur only on straight edges (EXACTNESS).
 *   YGFX_FX_EXACT_BLUR draws a sharp RECT's (or zero-radius RRECT's) drop
 *   shadow as the exact Gaussian blur of the rect, the product of two erf.
 *   An effect with opacity 0 is off. Bind its floats with ygfx_bind.field
 *   and ygfx_fx_params(). The quad grows by each effect's reach (5 SD).
 *
 *   DASHES AND TRIM
 *   Dashes and trim run along the arc length s of a closed boundary (a
 *   stroke is then required) or along the center line of a LINE, CAPSULE,
 *   ARC, QBEZIER or POLYLINE. s runs from a start point fixed per kind,
 *   +x toward +y (clockwise on the screen); ygfx_length() gives the
 *   total in px. dash[0] and dash[1] are the dash and gap in px;
 *   dash_offset (px) moves the pattern along s, so a bound channel marches
 *   it; dash_snap fits a whole number of periods to a closed boundary.
 *   trim[0], trim[1] keep the part from start to end, as fractions of the
 *   length (0, 0 or 0, 1 = all). cap shapes the ends of dashes, of a
 *   trimmed outline and of an open path: BUTT (flat, at the end), ROUND
 *   or SQUARE (half the band width past it). A dash's soft edge across s
 *   uses the same profile as the boundary. CROSS, masks and compounds have
 *   no length: dashes or trim on them are refused.
 *
 *   PAINT
 *   stim.paint points at a ygfx_paint (NULL or SOLID = color). LINEAR
 *   runs t from x0, y0 to x1, y1; RADIAL from the center x0, y0 to the
 *   radius x1; ANGULAR around x0, y0 from x1 degrees, clockwise; all in
 *   units of the box frame. 2 to 6 stops, t rising; repeat wraps t, else
 *   it is clamped. VERTEX gives each vertex of a convex POLYGON a color,
 *   mixed by Wachspress coordinates (barycentric on a triangle). The
 *   space is where colors mix: RGB (linear device RGB; XYZ, LMS and
 *   Cartesian DKL are maps of it and mix the same), OKLAB (ysp/color.h's
 *   Oklab: the calibration's absolute XYZ, black included, against the
 *   display's white; needs a calibration with chromaticities) or
 *   DKL_POLAR (elevation, azimuth and radius about the background, with
 *   desc.cones and desc.lum; needs a calibration with spectra; the azimuth
 *   takes the shorter turn between stops). Stops convert on the
 *   CPU; the shader mixes in the space and converts back per pixel. A
 *   path between in-gamut stops can leave the gamut in a nonlinear space:
 *   the CPU samples 64 points between neighbors and counts it as clipped.
 *
 *   BLEND
 *   stim.blend sets how a color stimulus meets the scene, in linear light:
 *   OVER (the default, premultiplied: an occluder), ADD (rgb += color x
 *   coverage x opacity: superposed light; alpha unchanged) or MULTIPLY
 *   (rgb = dst (1 - a + a color), a = coverage x opacity: a filter).
 *   Modulation stimuli add already: blend on them, and on an ADD image, is
 *   refused. A stimulus with another blend does not batch with its
 *   neighbors. The gamut check is v0.2's: per draw, overlaps not counted.
 *
 *   DISTANCE TEXTURES
 *   texture_desc.sdf marks a texture as a distance atlas for MASK_TEX:
 *   DIST (R16F or R32F, distance in texels, as v0.2), MSDF (msdfgen's
 *   multi-channel field: the median of the bilinear r, g, b; 0.5 on the
 *   edge, above it inside) or MTSDF (MSDF in rgb and the true distance in
 *   alpha, which offsets and round strokes read). MSDF and MTSDF need RGBA8,
 *   RGBA16F or RGBA32F and sdf_range, the atlas's full distance range in
 *   texels (msdf-atlas-gen's pxrange). An MSDF's offsets are mitered: a
 *   stroke with ROUND joins on it is refused (use JOIN_MITER, or MTSDF).
 *   The range rule: the atlas holds distances to range / 2 texels. What
 *   the edge (5 SD of a Gaussian, half the width of a cosine), the outer
 *   or inner part of the stroke and the offset reach must lie within
 *   (range / 2 - 1.5) texels, 1.5 texels kept for the bilinear neighbors.
 *   A draw that breaks it is refused with YGFX_ERR_RANGE and a message
 *   that gives the range it needs. src (x, y, w, h, texels from the
 *   top-left) picks one rectangle of the atlas for a mask; addresses are
 *   clamped to it, so no neighbor bleeds. The box must have the
 *   rectangle's aspect ratio: px per texel = box width / rectangle width.
 *
 *   GLYPH RUNS
 *   (The pack no longer makes MSDF atlases: its text is curve sets, CURVE
 *   RUNS. This path stays for atlases made elsewhere.)
 *   ygfx_glyphs() draws count glyphs from one atlas in one instanced
 *   draw. The buffer holds ygfx_glyph records (32 bytes): x, y, the
 *   glyph box's top-left in units from the run's box top-left, and sx, sy,
 *   sw, sh, its atlas rectangle in texels, padding included. scale is the
 *   units per atlas texel for every glyph. Edge, stroke, offset, color,
 *   opacity and blend apply to each glyph; the run places, turns and
 *   anchors as one box. The range rule holds per run. A run equals the
 *   same glyphs drawn one by one (bit for bit, measured). Layout,
 *   shaping and kerning are the caller's; bounds and hits come from the
 *   layout's metrics, not from the atlas. Coverage is an area fraction
 *   blended in linear light, so text looks thinner than text blended in
 *   sRGB; nothing corrects it.
 *
 *   EXACTNESS
 *   Two questions. Distance: is d the Euclidean signed distance? Then the
 *   coverage is exactly F(d), the stated profile. Blur: is the coverage
 *   the hard shape blurred by the kernel? For any shape, only on straight
 *   edges. The rule:
 *     1. A HARD edge needs only the sign of d: it is exact for every shape
 *        and operation (ELLIPSE, chords and textures to their stated
 *        error).
 *     2. A soft edge is exactly F(Euclidean d) for one exact primitive,
 *        filled, stroked with ROUND joins, offset or onioned; for a union
 *        of parts that stay apart by more than the edge's reach; and for
 *        dashes on straight runs.
 *     3. A soft edge is the Gaussian blur of the shape only on straight
 *        edges, and for a sharp RECT's shadow with EXACT_BLUR.
 *     4. Smooth merges never have an exact soft edge; the deviation is
 *        largest where two parts connect.
 *   Measured (llvmpipe, Iris Xe, WARP and SwiftShader agree), as the
 *   largest |coverage - reference| in units of the edge's step:
 *     exact kinds (RRECT, ARC, PIE, CAPSULE, NGON, STAR, POLYGON
 *       fillets, ELLIPSE) against F(brute-force Euclidean d)    4.9e-6
 *     QBEZIER (chords) against the curve                0.0050 px; 1.3e-3
 *     a UNION's reflex 90 degree crease (inside, a bound)     0.082
 *     a MITER corner, outside (a bound; v0.2)                 0.080
 *     SMOOTH_UNION, normalized, settled neck                  0.015
 *     SMOOTH_UNION at the moment two parts connect            0.122
 *     F(d) against the true blur: a 90 degree vertex          0.250
 *       a disc of radius 5 SD, 20 SD                          0.041, 0.010
 *       a drop shadow (F of the moved field) at a corner      0.248
 *     EXACT_BLUR against the erf product                      1.7e-7
 *   INTERSECT and SUBTRACT are bounds outside near a convex crease, XOR at
 *   every crease, and POLYLINE MITER and BEVEL near each turn, each about
 *   0.083 at 90 degrees (not measured separately). A non-smooth fold of
 *   exact fields already has a unit gradient, so dividing by it changes
 *   nothing there.
 *
 *   ---------------------------------------------------------------------
 *   CURVE SETS (v0.6)
 *   ---------------------------------------------------------------------
 *   A curve set is one font or one artwork set as quadratic Bezier curves
 *   and bands, the form of Lengyel's Slug algorithm (JCGT 6(2), 2017),
 *   implemented here from the paper. The pack tool's outline builder writes
 *   it; docs/gfx.md "Curve sets: the format" is the contract (version
 *   1). It is two arrays: texels (4 floats each: the curves) and words
 *   (uint32: a header, a glyph table, per glyph a record, band descriptors
 *   and sorted curve lists). ygfx_cset_make() checks them, uploads them as
 *   one RGBA32F and one RGBA32UI texture and, the first time, makes the text
 *   program (an open without text compiles nothing more):
 *       ygfx_cset font = ygfx_cset_make(&gfx, &(ygfx_cset_desc){
 *           .texels = pk.texels, .n_texels = pk.n_texels,
 *           .words = pk.words, .n_words = pk.n_words });
 *   Coordinates are em units (or the artwork's), y down, the origin at the
 *   glyph's origin (the pen position on the baseline). Glyph ids are the
 *   set's table indices: font glyph ids, or path indices for artwork.
 *   ygfx_cset_add() uploads glyphs the builder appended at run time,
 *   within cap_texels and cap_words given at make, between frames.
 *   ygfx_cset_check() and ygfx_cset_winding() need no gfx: the builder's
 *   own tests use them. Nonzero and even-odd fills are per glyph. The format
 *   holds no metrics: advances and line spacing are the layout's. Backward
 *   lists (flag bit 1) make the rays 18 to 27 % faster on a page (measured):
 *   the builder should write them.
 *
 *   ---------------------------------------------------------------------
 *   CURVE RUNS (v0.6)
 *   ---------------------------------------------------------------------
 *   ygfx_crun() makes a TEXT stimulus: n ygfx_citem records, each a
 *   glyph or path of a set at a pen position, drawn in one instanced draw.
 *   A word laid out by the pack tool or Skribidi:
 *       static ygfx_citem hello[5];          // .x, .glyph from the layout
 *       ygfx_stim word = ygfx_crun(&gfx, &(ygfx_crun_desc){
 *           .set = font, .items = hello, .n = 5, .size = 48,
 *           .color = { 1, 1, 1 }, .fields = YGFX_I_GATE });
 *   size is units per set unit: the font size per em, in px or deg. The
 *   run's box is w x h, or, when both are 0, the items' extent at make
 *   (from the glyph boxes the set keeps on the CPU; shape_p[0..1] then hold
 *   its top-left in item units), so CENTER centers the ink. ygfx_citem has
 *   ygfx_inst's layout with glyph in phase's place (INSTANCES): x, y and
 *   glyph are always read; ori and scale (about the glyph box's center, as
 *   GSAP's transformOrigin "50% 50%"), contrast (opacity), gate and color
 *   (a palette position) when their YGFX_I_* bit is in fields. Every
 *   field is a float, so a letter's y or gate binds to a timeline channel
 *   with ygfx_bind.field: a staggered entrance, GSAP's gsap.from(chars,
 *   { y: 40, opacity: 0, stagger: 0.05 }), is one channel per letter field.
 *   Items given by .items are copied at each draw into INSTANCES' element
 *   buffers (desc.max_instances a frame); a page of text goes in a
 *   ygfx_buffer once (.buf, .first) and costs no copy. Artwork is the same
 *   call, one item per layer at (0, 0) with a palette color: layers stack
 *   in item order, as GL blends one draw in order.
 *   Coverage is the pixel's box filter, in linear light (text looks thinner
 *   than text blended in sRGB; nothing corrects it):
 *     exact     where the glyph is resolved (format flag bit 2: no contours
 *               overlap) and the item is not turned, or turned by quarter
 *               turns: the area of glyph intersect pixel by Green's theorem
 *               over the bands the pixel's rows meet; within 1.8e-5 of the
 *               exact coverage (measured). A false resolved bit gives wrong
 *               coverage near the overlaps, nothing worse
 *     otherwise two rays from the pixel center through the glyph's bands
 *               (Lengyel). A crossing near the pixel adds the exact box
 *               coverage of the half-plane bounded by the curve's tangent
 *               there, so a straight edge at any angle is exact; corners,
 *               curvature tighter than a pixel and self-intersections are
 *               not. The rays mix by how steeply and how near each one
 *               crosses. Measured mean edge error 0.029, 0.019, 0.009 and
 *               0.004 at 8, 12, 24 and 48 px per em, the same at every
 *               rotation; the largest 0.56, at corners. Where a ray passes a
 *               corner, a pixel's value jumps (up to 0.44 between positions
 *               0.005 px apart, measured); on a smooth outline by at most
 *               0.038
 *   The exact area costs 1.4 times the rays on a page of 12 px text and
 *   less than them at 200 px (STATUS). Which to pick: the exact area (the
 *   default) wherever text is read or measured; .rays (crun_desc.rays) for
 *   a page whose glyphs are 8 px per em or more and whose frame needs the
 *   2 ms: it costs the rays' error everywhere, a mean of 0.029, 0.019,
 *   0.009 and 0.004 on edge pixels at 8, 12, 24 and 48 px per em and up
 *   to 0.56 at corners, against 1.8e-5. The first .rays run compiles the
 *   rays program (about 150 ms): make it at setup. Turned runs take the
 *   rays program by themselves (v0.8): a run draws on it, with the same
 *   bits as on the exact-area program, when no item can take the exact
 *   area there: the run is aliased, or its turn (with a group's) is 0.01
 *   degrees or more from a quarter turn and it has no YGFX_I_ORI, or it
 *   has YGFX_I_ORI, its items on the CPU, and every item's turn plus the
 *   run's is 0.01 degrees or more from one. Items in a buffer with
 *   YGFX_I_ORI stay on the exact-area program (their turns are on the
 *   GPU only), as does a run made before the rays program existed. Static
 *   text (a reading page) belongs in a target, drawn once, in a setup pass
 *   (SETUP PASSES): an RGBA16F target of the
 *   screen's size (18.4 MB at 1920 x 1200; RGBA32F 36.9 MB) drawn as an
 *   image at 1:1 on whole px costs 0.5 ms against 7.5 ms and has the run's
 *   coverage bit for bit (the text program computes each pixel's em point
 *   in px with y down, the same in a target as in the scene). Draw a run
 *   directly only when it moves, turns, scales or animates per letter: a
 *   cached texture drawn off the pixel grid or scaled is interpolated and
 *   no longer the exact coverage. A page rendered in a setup pass equals
 *   one rendered in a frame, bit for bit. The fill rule maps each ray's
 *   fractional winding to coverage: exact while a pixel sees one
 *   transition. .aliased gives 0 or 1 from the winding at the pixel center.
 *   Edge profiles, strokes, offsets, dashes, fx and paint are refused on a
 *   run: the outline builder expands outlines and bold into fills; soft
 *   edges, glow and shadow come from BLUR. A hard shadow is a second draw
 *   of the same run, offset and recolored. Blend OVER, ADD or MULTIPLY, as
 *   color stimuli. ygfx_hit() on a run is the winding at the pixel center
 *   from the set's arrays when the set was made with .keep (it agrees with
 *   aliased coverage), else the glyph boxes; ygfx_hit_index() gives the
 *   topmost item (artwork: the layer); items in a buffer are on the GPU
 *   only, so neither sees them. In the reorder a run's bounds are the whole
 *   pass.
 *
 *   ---------------------------------------------------------------------
 *   BLUR (v0.6)
 *   ---------------------------------------------------------------------
 *   A separable Gaussian blur of a layer, a target you draw into, into a
 *   result you draw as an image: soft shadows, glow, blurred-letter
 *   stimuli. CSS's text-shadow: 0 0 3px gold:
 *       static ygfx_blur glow;                              // setup
 *       ygfx_blur_make(&gfx, &glow, &(ygfx_blur_desc){ .w = 800, .h = 240,
 *                        .format = YGFX_R16F, .sigma = 3 });
 *       ygfx_begin_target(&gfx, glow.layer, NULL);          // in a frame
 *       ygfx_draw(&gfx, &word_white);                       // at the layer's center
 *       ygfx_end_target(&gfx);
 *       ygfx_blur_apply(&gfx, &glow);
 *       glow.image.tint[0] = 1; glow.image.tint[1] = 0.8f; glow.image.tint[2] = 0.2f;
 *       ygfx_draw(&gfx, &glow.image);                       // the glow
 *       ygfx_draw(&gfx, &word);                             // the sharp word on top
 *   The layer keeps the sharp content: draw it once and apply the blur each
 *   frame with a sigma bound to a channel (ygfx_bind.field = &glow.sigma),
 *   and only the two passes cost. Place the word at the layer's center and
 *   the image where the word is. The rules:
 *     two passes, horizontal into a scratch target, vertical into the
 *       result, in call order; they count as 2 of the 16 passes;
 *     direct taps within 5 SD, weights exp(-x^2 / 2 s^2) normalized in f32;
 *       a BOX source (the default: curve runs, antialiased edges) has the
 *       box filter's variance, 1/12 px^2, taken out of sigma; POINT sources
 *       (gratings) do not;
 *     outside the layer is empty: content within 5 sigma of its edge loses
 *       the mass that falls outside, as on an empty screen;
 *     formats: R16F (a coverage mask: draw white into it; the image is
 *       tint.rgb with coverage times tint.a), RGBA16F (premultiplied
 *       color), RGBA32F (needs EXT_float_blend for the draws into the
 *       layer; ANGLE on D3D11 hardware, WARP and SwiftShader and Mesa
 *       llvmpipe have it, and EXT_color_buffer_float, which open requires);
 *     supersample 2 (opt-in): the layer is 2w x 2h and draws into it see
 *       twice the px per unit (fields in px are layer px); the passes take
 *       taps on the 2x grid at each result pixel's center. The error at
 *       sigma 0.5 to 2 falls 4 to 5 times (measured), at a cost given in
 *       STATUS;
 *     sigma above the box's own SD (0.29 px; POINT: above 0) and at most
 *       32 px of the result. The error is the 1x sampling's: 0.20 at sigma
 *       0.5, 0.031 at 1, 0.0087 at 2, 0.0022 at 4 (RGBA32F, against the
 *       exact blur of a rect; docs/gfx.md);
 *     the cost grows with the layer's pixels and sigma: size the layer to
 *       the stimulus, not the screen.
 *   A blur is of linear light: the physical blur of the displayed pattern.
 *
 *   ---------------------------------------------------------------------
 *   PRIME (v0.11)
 *   ---------------------------------------------------------------------
 *   A cache entry or a compile gives a program; ANGLE still makes its D3D
 *   shader objects at the program's first draw. On the Iris Xe that cost
 *   2.5 to 12.9 ms of CPU in the first ygfx_end() of each kind, from a
 *   warm cache too (docs/gfx.md, "v0.11: priming"). ygfx_prime() draws
 *   every program made so far once, as zero-size quads, and waits for the
 *   GPU (63 to 73 ms for 20 programs). Call it after making the stimuli,
 *   before the first trial:
 *       ... ygfx_open(), textures, curve sets, ygfx_instances(), pipelines
 *       ygfx_prime(&g);
 *   The first frame after a setup of 2 s or more was still late or
 *   dropped (21 of 22 runs, prime or not): show a few frames before the
 *   first timed trial.
 *
 *   PROGRAM CACHE (v0.4)
 *   ---------------------------------------------------------------------
 *   Each built-in program costs ANGLE 0.15 to 0.75 s to compile at open.
 *   desc.cache keeps the compiled programs: a pair of calls that load and
 *   store bytes under a 64-bit key, in storage the caller owns. ysp_gfx
 *   reads or writes no file unless the caller gives a cache that does:
 *       static ygfx_file_cache pc;
 *       char dir[512];
 *       if (ygfx_default_cache_dir(dir, sizeof dir) == YGFX_OK)
 *           desc.cache = ygfx_file_cache_init(&pc, dir);
 *   ygfx_default_cache_dir() gives the per-user cache folder (v0.10.1;
 *   from v0.10.2 it is yrt_user_dir(YRT_DIR_CACHE, "progcache"), ysp/rt.h
 *   PER-USER FOLDERS, with the same rules). It makes no folder; on POSIX it reads the owner and mode of the parts
 *   of the path that exist.
 *     Windows  %LOCALAPPDATA%\ysp\progcache (read with the wide API, given
 *              as UTF-8)
 *     Linux    $XDG_CACHE_HOME/ysp/progcache when that is an absolute path,
 *              else $HOME/.cache/ysp/progcache
 *     macOS    $HOME/Library/Caches/ysp/progcache
 *   It fails (YGFX_ERR_ARG, out empty) when the variable is missing or
 *   not absolute, when the path does not fit, on the web, and on POSIX
 *   when a part of the path that exists can be written by all users or
 *   belongs to another user (not root). Then use no cache. Never fall
 *   back to a shared folder such as /tmp: the entry hash finds damage, not
 *   attack. Anyone who can write the folder can put an entry with a
 *   correct hash there, and the GL driver parses that binary in the
 *   experiment's process. Give ygfx_file_cache_init() only a folder that
 *   the user alone can write.
 *   The key covers the GL vendor, renderer (ANGLE puts the adapter and the
 *   driver version in it), version (the ANGLE build) and GLSL strings, the
 *   binary format and the program's whole text. An entry is a 48-byte
 *   header (the key, a second hash of the key's material and its length,
 *   a hash of the binary) and the driver's binary. An entry is used only
 *   when all of it matches; anything else is compiled from source and
 *   stored again: a miss, a damaged or truncated entry, a key collision,
 *   another driver's entry, a binary the driver refuses. ANGLE links a
 *   binary with a byte changed and crashed on half a binary (measured), so
 *   the hash is what keeps a damaged entry from the driver. The file cache
 *   writes each entry whole under a temporary name and renames it; it
 *   never deletes, so each ANGLE build and driver pair adds about 0.5 MB
 *   until the folder is emptied by hand. ygfx_program_stats() and the
 *   describe line give what the open did. The describe line names the
 *   folder of a ygfx_file_cache ("program cache <folder>: N hit / M
 *   miss (R rejected)"; a rejected entry is also a miss), or says
 *   "(caller)" for another cache and "off" for none; "(unused: no binary
 *   format)" follows when the backend has no program binaries (the null
 *   backend, WebGL). User pipelines, the video program and
 *   the instanced programs use the same cache. WebGL has no
 *   binaries: the cache is then unused (YGFX_FEAT_PROGRAM_CACHE clear).
 *
 *   OPEN START (v0.11.1)
 *   ---------------------------------------------------------------------
 *   With a cold cache the built-in programs take 2.2 to 3.2 s to compile
 *   on the Iris Xe (AC to battery), and ysp/screen.h's open() settles for 0.5 to 0.8 s
 *   fullscreen and 2.5 s in a window. ygfx_open_start() lets the two
 *   overlap: called from the screen's desc.on_context (ysp/screen.h
 *   CONTEXT HOOK), it checks the desc, makes the buffers and textures,
 *   submits every built-in program and returns; ANGLE compiles them on
 *   its own threads while open() settles. ygfx_open() on the started
 *   handle waits for the link statuses, stores the cache entries and
 *   opens it. It uses the start's desc (copied at the start; the desc it
 *   gets may be NULL), so the cache, the calibration and the ring the
 *   desc points to must stay valid until it returns:
 *       static ygfx_desc gd;                        // .background, .cache
 *       static void start(void* ctx, yscr_screen* s) {
 *           ((ygfx_desc*)ctx)->screen = s;
 *           ygfx_open_start(&gfx, (ygfx_desc*)ctx);  // returns in 14 to 48 ms
 *       }
 *       yscr_open(&scr, &(yscr_desc){ .on_context = start, .on_context_ctx = &gd });
 *       ygfx_open(&gfx, &gd);                       // waits for the rest
 *       ... stimuli, ygfx_prime(&gfx)
 *   A start that fails leaves the handle closed with ygfx_error() set;
 *   ygfx_open() then tries the whole open again and reports it. Between
 *   the start and ygfx_open() the handle is not open: every other call
 *   returns YGFX_ERR_CLOSED; ygfx_close() deletes what the start made.
 *   If the screen closed in between (its open() failed after the hook),
 *   ygfx_open() fails and names it, and ygfx_close() frees the memory
 *   and deletes nothing: the objects went with the context.
 *   Only a backend with caps.parallel_compile (the GL backend when the
 *   context has GL_KHR_parallel_shader_compile; ANGLE's D3D11 and Mesa's
 *   llvmpipe have it) submits at the start; on another the start makes
 *   nothing and ygfx_open() does the whole open after settling, as
 *   before. The describe line ends with open_start=parallel or
 *   open_start=serial(no GL_KHR_parallel_shader_compile).
 *   ygfx_program_stats().open_ns is the time of the two calls together,
 *   not the settling between them.
 *   Measured on the Iris Xe (docs/gfx.md, "v0.11.1"), ms from the
 *   yscr_open() call until ygfx_open() returned, median of the two
 *   backends, before (one after the other) and with the start in the hook:
 *     cold cache, window, AC           4787 to 4832 -> 2567
 *     cold cache, fullscreen, AC       2865 to 2908 -> 2459 to 2466
 *     cold cache, window, battery      5374 to 5396 -> 3405 to 3407
 *     cold cache, fullscreen, battery  3852 to 3887 -> 3459 to 3468
 *     warm cache, window, battery      2546 to 2553 -> 2520 to 2524
 *     warm cache, fullscreen, battery   717 to  805 ->  666 to  781
 *   On AC a window's compiles end within 0.04 s of its 2.5 s minimum;
 *   fullscreen 0.4 s of 2.3 s is hidden. Settling took the same time with
 *   the compiles running.
 *
 *   ---------------------------------------------------------------------
 *   VIDEO (v0.4)
 *   ---------------------------------------------------------------------
 *   YGFX_NV12 and YGFX_I420 textures hold 8-bit 4:2:0 video as planes
 *   (Y w x h; chroma ceil(w / 2) x ceil(h / 2)). ygfx_image() draws them
 *   as color images, converted per pixel to linear light by texture_desc.
 *   enc, which a planar texture must state in full (rig_spec principles 4
 *   and 5: nothing is assumed):
 *     matrix     BT601, BT709 or BT2020 (non-constant luminance)
 *     range      LIMITED (Y' 16..235, C 16..240) or FULL (C = (code - 128)
 *                / 255)
 *     transfer   DEVICE: the R'G'B' codes are device values; they go
 *                through the display's own transfer (the inverse of the
 *                CLUT, a 256-entry table made at open), so the output stage
 *                writes each code back, calibration or not (all 256 codes
 *                tested). BT1886 (V^2.4, black at the display's black),
 *                SRGB, LINEAR (values are linear already) or GAMMA22.
 *     primaries  DEVICE passes the source's RGB through as the display's,
 *                stated. BT709, BT601_525, BT601_625 or BT2020 convert
 *                through the calibration's chromaticities (relative: the
 *                source's D65 white at Y = 1 goes to the display's white,
 *                no chromatic adaptation); without a calibration that has
 *                them, refused. Light out of gamut is clamped by the scene,
 *                not counted.
 *     siting     LEFT (MPEG-2, H.264, HEVC: co-sited across, between rows),
 *                CENTER (MPEG-1, JPEG) or TOP_LEFT. Chroma is interpolated
 *                at that point, bilinear and clamped to its plane (at 1:1,
 *                the same weights as ysp/video.h's sixteenths), or
 *                replicated with chroma_nearest.
 *   R'G'B' is clamped to 0..1 before the transfer. An RGBA8, RGBA16F or
 *   RGBA32F texture may state an encoding too (matrix RGB), to show a frame
 *   sequence or an sRGB image in linear light; all zero keeps the old
 *   meaning (the values are linear). So may an R8 or RG8 texture (v0.10.5):
 *   gray, its level in r and alpha in g, primaries DEVICE (gray is the
 *   display's white at each level; others refused). An encoded texture
 *   draws as a color image only. A pack's TEXTURE entry stores these bytes
 *   as ygfx_texture() takes them (docs/pack.md 4.9). The values of the YGFX_MATRIX_, _RANGE_, _TRC_, _PRIM_
 *   and _SITING_ names are ysp/video.h's. A planar texture updates by
 *   ygfx_texture_update_planes() (each plane, one call) or
 *   ygfx_texture_update_plane() (a rectangle of one plane).
 *   ygfx_texture_import() wraps a texture ysp_gfx did not make: a GL
 *   texture of this context, or on Windows a D3D11 texture on ANGLE's
 *   device (yscr_native()), RGBA or NV12, a slice of an array included
 *   (EGL_ANGLE_image_d3d11_texture; its filters are set to NEAREST). A
 *   texture shared from another D3D11 device is opened on ANGLE's device by
 *   its handle first; the caller orders the producer's write before the
 *   frame: a keyed mutex acquired before ygfx_end() and released after
 *   the flip, or the producer's flush and a query it waits on (both read
 *   back exactly). ygfx_texture_rebind(t, src) makes t show src's texels
 *   with no GL call, so a movie's stimulus keeps its handle while the
 *   decoder's surfaces rotate. The video program compiles at the first
 *   encoded texture: an open without video costs nothing more.
 *
 *   ---------------------------------------------------------------------
 *   INSTANCES (v0.4)
 *   ---------------------------------------------------------------------
 *   One template stimulus and an array of ygfx_inst elements draw in one
 *   call (a gabor array, an element field, a search display):
 *       static ygfx_inst items[400];
 *       ygfx_inst_grid(items, 20, 20, 1.5f, 1.5f);          // deg
 *       for (i = 0; i < 400; i++) items[i].ori = random_ori();
 *       ygfx_stim gab = ygfx_gabor(&(ygfx_gabor_desc){ .sf = 4, .sigma = 0.15f });
 *       ygfx_stim field = ygfx_instances(&gfx, &gab, &(ygfx_instances_desc){
 *           .inst = items, .n = 400, .fields = YGFX_I_XY | YGFX_I_ORI });
 *       ygfx_draw(&gfx, &field);
 *       int k = ygfx_hit_index(&gfx, &field, mouse_x, mouse_y);   // which one
 *   Element i is the template with: its anchor moved by (x, y) units along
 *   the template's turned axes; its box turned by ori and scaled by scale
 *   about that anchor (as a group's scale: what is in px, edge_width,
 *   stroke, offset, does not scale); phase added; contrast multiplying
 *   contrast and opacity; gate multiplying gate; color a position in the
 *   palette (up to 16 rgb entries: color for a color stimulus, the tint for
 *   an image, dir for a modulation, used as given), where an integer gives
 *   that entry bit for bit and a fraction mixes two neighbors in linear
 *   light. Only the fields in .fields are read. Templates: SHAPE (vector
 *   kinds whose data fits their block), GRATING, GABOR, NOISE (no scale:
 *   its check is px), IMAGE and USER (the body sees its own px and ysp_size
 *   scaled, as in a group); not DOTS or glyph runs (instanced already) or
 *   a stimulus with extension blocks. A template field moves every element
 *   (a ygfx_bind on the stimulus); ygfx_bind.field on items[k] moves
 *   one. draw() copies the elements into the frame's buffer: up to
 *   desc.max_instances (16384 by default) a frame; past it the draw is
 *   refused with YGFX_ERR_FULL, and nothing of it is drawn.
 *   ygfx_hit_index() gives the topmost element (the highest index) under
 *   a pixel by the CPU's field, skipping gate 0; ygfx_inst_resolve() an
 *   element's anchor. The gamut check covers the template, not each
 *   element. Element angles come from a polynomial in degrees (D3D's sin
 *   and cos are coarse): within 4.4e-7 of double.
 *
 *   ---------------------------------------------------------------------
 *   OUTPUT STAGE
 *   ---------------------------------------------------------------------
 *   CLUT     n entries per channel from linear to device value (2 to
 *            4096), interpolated linearly by hand from two texel fetches,
 *            so no filtering extension is involved. Identity (n = 2) without
 *            a calibration: then a scene value is a device value.
 *            ygfx_set_lut() replaces it between frames.
 *   DITHER   code = floor(v * 255 + t), t = 0.5 (NONE, rounding), a Bayer
 *            8 x 8 threshold (ORDERED), or a hash of pixel, frame index and
 *            seed (NOISE, a new pattern each frame). The pattern is the
 *            scene's pixel grid from its bottom-left corner. Its spectrum
 *            is part of the stimulus; the default is NONE.
 *   GAMUT    the scene is clamped to 0..1. Each draw is checked on the CPU
 *            (bg +- contrast * gate * |dir| for modulations, the color for
 *            color stimuli); a draw that may leave 0..1 is counted
 *            (ygfx_clipped) and a frame with one pushes a CLIPPED record.
 *            Overlaps are not counted.
 *   10 BIT, MONO++, COLOR++, STEREO
 *            declared and refused in v0.1 (STATUS).
 *
 *   ---------------------------------------------------------------------
 *   CAPTURE
 *   ---------------------------------------------------------------------
 *   Opt-in (v0.12): the output of chosen frames, to files or to a
 *   callback, during a trial. ygfx_capture_start() at setup chooses the
 *   frames: every Nth (desc.every, by frame index, frame 0 first), and each
 *   frame that ygfx_capture_mark() marks between begin() and end(). It
 *   makes desc.slots pixel buffers of the rectangle (w x h x 4 bytes each,
 *   9.2 MB at 1920 x 1200) and starts the writer thread:
 *       ygfx_capture_desc cd = { .every = 4, .path = "data/s01/t" };
 *       ygfx_capture_start(&gfx, &cd);
 *       ... the trial's frames ...
 *       ygfx_capture_stop(&gfx, &stats);   // waits, writes the rest
 *   A chosen frame: end() copies the rectangle of the output stage's codes
 *   into a free buffer (glReadPixels into a pixel buffer object) and sets a
 *   fence, before the present. The screen's patch and codes, which flip()
 *   draws, are not in the copy. Nothing waits. A later end() (or
 *   ygfx_capture_pump()) sees that the fence passed and maps the buffer;
 *   begin() takes the frame's flip record from yscr_frame.done. Then the
 *   writer writes <path><frame index, 8 digits>.ppm (or .png) with the
 *   index, the predicted onset and the record in the file's comment, and
 *   calls desc.on_frame. A frame waits up to 30 frames for its record; a
 *   frame you fill yourself (done NULL) and a headless gfx do not wait;
 *   stop() writes what is left without records (stats.no_record).
 *   DROPS    a chosen frame with no free buffer is not copied: it counts in
 *            stats.dropped and pushes a YGFX_EV_CAPTURE_DROP record. The
 *            frame thread never waits for a buffer, the GPU or the disk. A
 *            buffer is busy from its copy until the writer is done: about 3
 *            frames plus the file's write.
 *   COST     docs/gfx.md, "v0.12": the frame thread pays the copy's call and,
 *            a frame later, the map (ANGLE copies the pixels there), and the
 *            writer the file. The knobs are desc.every, the rectangle and
 *            desc.format.
 *   ROWS     top first, in the files and for on_frame (fr.stride < 0 when
 *            the copy lies bottom first in memory). The bytes are
 *            ygfx_read_output()'s for the same frame (tested bit for bit).
 *   THREADS  the writer is ysp/rt.h's pump at below normal priority. With
 *            YRT_NO_THREADS or desc.manual there is no writer:
 *            ygfx_capture_pump() writes on the calling thread, outside a
 *            frame (between trials). In a trial the buffers then fill and
 *            later chosen frames drop: size desc.slots for the frames a
 *            trial captures.
 *   ygfx_image_write() is the file writer, for any RGBA8 rows: PPM (P6) or
 *   PNG with stored (not compressed) blocks, 8-bit RGB, no allocation.
 *
 *   ---------------------------------------------------------------------
 *   CALIBRATION
 *   ---------------------------------------------------------------------
 *   The calibration is ysp/color.h's ycol_cal (the .yspcal file, the same
 *   bytes as v0.4's ygfx_cal): readings, spectra, derive(), the CLUT and
 *   the matrices into XYZ and cone space; its manual has the recipe.
 *   desc.cal gives it to open(), which checks it (ycol_cal_check()),
 *   uploads its CLUT and makes PAINT's spaces and the video primaries from
 *   it. Without it a scene value is a device value. For colors, cone
 *   contrast and DKL directions (stim.dir), gamut questions and gamut
 *   mapping, use ygfx_color(g): the ysp/color.h context (ycol_ctx)
 *   open() made with desc.background, desc.cones and desc.lum, the one
 *   PAINT uses. v0.4's calls are
 *   ysp/color.h's raw layer under new names, the same bits:
 *   ycol_cal_dir_cone(), ycol_cal_dkl_matrix(), ycol_cal_dir_dkl(),
 *   ycol_dkl_from_sph(), ycol_max_contrast(),
 *   ycol_cone_fundamentals(YCOL_CONES_SS2, nm, lms).
 *
 *   ---------------------------------------------------------------------
 *   SHADER CONTRACT
 *   ---------------------------------------------------------------------
 *   A user shader is GLSL ES 3.00: one function and its helpers.
 *       float ysp_main(vec2 p)   MODULATION: g(p) in [-1, 1]
 *       vec4  ysp_main(vec2 p)   COLOR: linear rgb and coverage
 *       vec3  ysp_main(vec2 p)   ADD: an rgb increment
 *   p is the fragment's point in the stimulus's own frame, pixels from its
 *   box center, along its axes (x along ori, y a quarter turn clockwise
 *   from it on the screen), computed from gl_FragCoord and the exact
 *   center: interpolated coordinates moved with the rasterizer's subpixel
 *   snapping, up to 2e-2 of full scale on SwiftShader (docs/gfx.md).
 *   The wrapper applies the aperture and edge, contrast, gate, dir or
 *   opacity, and the blend. Available to the body:
 *       ysp_param(k)    the stimulus's p[k], k 0..31
 *       ysp_size        half box w, h (px), pixels per unit, quad margin
 *       ysp_look        contrast, opacity, gate, the stroke band's center
 *                       offset px (ysp_shape.w is its half width)
 *       ysp_color, ysp_dir, ysp_xf, ysp_edge, ysp_shape, ysp_misc
 *       ysp_target      target w, h, 1/w, 1/h
 *       ysp_time        onset s since open (float: 0.24 ms steps at 1 h),
 *                       period s, frame index, 0
 *       ysp_seed        uvec4: frame index low, high, desc.seed, vblank
 *       ysp_hash(uint)  the integer hash of NOISE (triple32 from v0.9)
 *       ysp_hash2(uvec2, uint)  NOISE's key of a point under a seed (v0.9)
 *       ysp_tex0..2, ysp_utex0 (unsigned), ysp_PI, ysp_sdf, ysp_sdf_at,
 *       ysp_coverage, ysp_aperture, ysp_bilinear (four texelFetch,
 *       clamped to a rectangle)
 *   stim.tex (v0.10.4) is ysp_tex0, or ysp_utex0 for an R16UI texture:
 *   an image, or a target (its rows top first, premultiplied, scene-space
 *   values). texelFetch reads it 1:1; ysp_bilinear interpolates. ysp_tex1
 *   is the mask of MASK_TEX. Without stim.tex, ysp_tex0 is unbound: what
 *   it reads is undefined.
 *   Identifiers with two underscores in a row are reserved in GLSL (ANGLE
 *   warns): the contract has none.
 *   The wrapper puts "#line 1" before the body, so the compiler's log
 *   names the body's lines. ygfx_shader_wrap() gives the fragment
 *   shader text without GL: the pack tool validates exactly that.
 *   YGFX_SHADER_CONTRACT (v0.10.3) is the contract's version: it changes
 *   when what the wrapper does to a body changes (the names above, their
 *   meaning, the blend), not when its text changes otherwise. A pack
 *   stores it with each shader; a player refuses another version. It is 2
 *   from v0.10.5: a body that samples ysp_tex0 (stim.tex, v0.10.4) read
 *   garbage under contract 1.
 *   A time-dependent pattern takes its phase in ysp_param() from the CPU,
 *   not from ysp_time. A slow shader is its writer's timing problem, and
 *   the flip record's phases are where it shows.
 *
 *   ---------------------------------------------------------------------
 *   BACKENDS
 *   ---------------------------------------------------------------------
 *   ygfx_backend (version 4) is a table of 24 calls (pipelines from
 *   shader text, textures, buffers, passes, apply, draw, present, read,
 *   reset, pipeline_finish, pipeline_binary, and five capture calls): the
 *   gfx backend of the rig's
 *   extensions. Version 3 (v0.4) asks an out-of-tree backend for: the caps
 *   strings vendor, version and glsl and binary_format (0: no binaries);
 *   pipeline_src.binary (try it, compile vs and fs when the driver refuses
 *   it) with pipeline_make returning 1 when the binary was used; a
 *   pipeline_binary call, or NULL; texture_src.gl_name and .d3d11, .plane,
 *   .slice (wrap, not make; NOT_IMPLEMENTED is a fine answer);
 *   bindings.tex[6] (units 4 and 5: the video program's Cr plane and the
 *   display's transfer table); bindings.instances_off (the first record);
 *   and caps.features (YGFX_FEAT_IMPORT_*). A backend that copies the
 *   built-in table and changes some calls, as the tests do, needs none of
 *   this. Version 4 (v0.12) adds capture_make, _read, _poll, _unmap and
 *   _free (CAPTURE), each NULL when the backend has no reads that do not
 *   wait; open() still takes a version 3 table, and capture then returns
 *   YGFX_ERR_NOT_IMPLEMENTED.
 *   open() starts every built-in program with pipeline_src.deferred set,
 *   then calls pipeline_finish on each: a driver that compiles in parallel
 *   (ANGLE does) then overlaps them. caps.parallel_compile (v0.11.1) says
 *   pipeline_make returns before the compile ends; only then does
 *   ygfx_open_start() submit (OPEN START). A backend that leaves it false
 *   gets today's open. A backend without it sets
 *   pipeline_finish NULL and completes each pipeline_make. bindings.blend
 *   overrides a pipeline's blend for one draw (BLEND). The built-in one is GL ES 3.0 through
 *   yscr_gl_proc() (or desc.gl_proc, headless); on a screen with no GL
 *   (SIM) the null backend runs everything and draws nothing. A backend
 *   gets and returns rows bottom-up, GL's order.
 *   On DXGI_FLIP and COMPOSITION, ANGLE's client-buffer pbuffer puts GL's
 *   row 0 at the top of the screen (measured); the output stage writes
 *   the rows in that order. desc.rows overrides it for a custom presenter.
 *   GL state is cached and set only where it changes. Another program's GL
 *   calls between frames make the cache wrong: call ygfx_reset_state()
 *   after them. A ysp/screen.h present callback is followed for you: the
 *   cache is dropped whenever yscr_gl_epoch() has moved (checked on entry
 *   to every call that issues GL). When yscr_gl_generation() has moved,
 *   the context is new and every GL object is gone: that call returns
 *   YGFX_ERR_LOST and the handle closes without deleting anything (the
 *   old names could name the new context's objects). Call ygfx_close(),
 *   then ygfx_open() and make the resources again.
 *
 *   ---------------------------------------------------------------------
 *   THE RING
 *   ---------------------------------------------------------------------
 *   With desc.ring, source YRT_SRC_GFX:
 *     YGFX_EV_OPEN     at open: aux scene format; u.u32[0..6] CLUT n,
 *                        dither, output, calibration CRC, its flags, w, h;
 *                        [7] programs from desc.cache, [8] compiled, [9]
 *                        open() in ms
 *     YGFX_EV_LUT      at ygfx_set_lut(): u.u32[0] n, [1] CRC-32 of it
 *     YGFX_EV_CLIPPED  a frame with clipped draws only: t_ns its onset,
 *                        aux the count, u.i64[0] the frame index
 *     YGFX_EV_COLOR    at open, with a calibration: u.u32[0] the color
 *                        context's id, [1] the calibration's CRC, [2] the
 *                        lum record's CRC (0 = the standard luminance),
 *                        [3] cones, [4] the context's YCOL_CAN_* levels
 *     YGFX_EV_CAPTURE_DROP  a chosen frame with no free capture buffer
 *                        (v0.12): t_ns its predicted onset, aux the drops so
 *                        far, u.i64[0] the frame index
 *
 *   ---------------------------------------------------------------------
 *   MEMORY AND THREADS
 *   ---------------------------------------------------------------------
 *   open() allocates once (the frame's uniform staging and queue,
 *   max_draws x 296 bytes plus 4 KB of slack for one stimulus's extension
 *   blocks and 4.25 KB of frame blocks, one per target pass, 304 KB by
 *   default, and the shader text while it compiles); nothing after it
 *   (measured in v0.1: no C runtime heap call in the frame loop).
 *   v0.4: the first ygfx_instances() allocates the element staging
 *   (desc.max_instances x 32 bytes, 512 KB by default) and three element
 *   buffers of that size on the GPU; the first encoded texture compiles the
 *   video program; a user pipeline keeps a copy of its body for its
 *   instanced program. The frame loop still allocates nothing.
 *   v0.6: ygfx_cset_make() allocates the set's box table (16 bytes a
 *   glyph) and makes its two textures, and the first one compiles the text
 *   program; ygfx_crun() with items allocates INSTANCES' element staging
 *   when it is not there; ygfx_blur_make() makes three targets (layer,
 *   scratch, result) and the first one compiles the blur program. A frame
 *   with runs and blurs allocates nothing.
 *   v0.7: open makes NOISE GAUSSIAN's table texture (256 x 256 R32F, 256
 *   KB) from a static table of the same size, built once per process with
 *   256 KB of static scratch; a run made with .rays compiles the rays
 *   program once.
 *   v0.12: ygfx_capture_start() allocates its state (about 10 KB with the
 *   writer's pump) and makes desc.slots pixel buffers on the GPU (w x h x
 *   4 bytes each). The capture's frame work allocates nothing in this
 *   header (what the driver does at a map is the driver's); the
 *   writer opens files (the C runtime allocates a FILE) on its own thread.
 *   The handle is about 23 KB on 64-bit targets. A target pass takes no memory of its own;
 *   ygfx_target() makes its texture and framebuffer at setup. One thread
 *   calls everything, the thread that owns the screen's context. Textures,
 *   buffers and pipelines are made and updated between frames, never
 *   between begin() and end().
 *
 *   ---------------------------------------------------------------------
 *   BUILDING
 *   ---------------------------------------------------------------------
 *   As ysp/screen.h: link SDL3, put ANGLE's libEGL.dll and libGLESv2.dll
 *   beside the program on Windows. With YSCR_NO_SDL it builds without SDL
 *   and runs headless on a GL ES 3.0 context you make (desc.gl_proc) or on
 *   a custom presenter (tests/adapt/gfx_headless.h makes one on EGL:
 *   ANGLE's D3D11, WARP or SwiftShader on Windows, Mesa on Linux). Link
 *   libm on Linux. Define YGFX_API to change the linkage.
 *   Zones: ygfx.begin, ygfx.draw, ygfx.end, ygfx.output,
 *   ygfx.capture, ygfx.upload.
 *
 *   LICENSE: public domain / MIT-0, with the MIT-licensed parts named at
 *   the top; all notices are at the end of the file.
 */
#ifndef YSP_GFX_H_INCLUDED
#define YSP_GFX_H_INCLUDED

#define YGFX_VERSION_MAJOR 0
#define YGFX_VERSION_MINOR 12
#define YGFX_VERSION_PATCH 0
#define YGFX_VERSION_STRING "0.12.0"

#include "ysp/screen.h"
#include "ysp/color.h"

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef YGFX_API
#define YGFX_API extern
#endif

/* --- codes -------------------------------------------------------------- */

#define YGFX_OK                    0
#define YGFX_ERR_ARG             (-1)
#define YGFX_ERR_CLOSED          (-2)  /* not open                          */
#define YGFX_ERR_ORDER           (-3)  /* begin, draw, end or an update out
                                          * of turn                          */
#define YGFX_ERR_FULL            (-4)  /* a pool or the frame's draws       */
#define YGFX_ERR_GL              (-5)  /* the backend failed                */
#define YGFX_ERR_NOT_IMPLEMENTED (-6)
#define YGFX_ERR_REFUSED         (-7)  /* a request done only on purpose    */
#define YGFX_ERR_FORMAT          (-8)  /* not the canonical form            */
#define YGFX_ERR_RANGE           (-9)  /* readings or values out of range   */
#define YGFX_ERR_LOST            (-10) /* the GL context is new: every GL
                                          * object is gone; the handle is now
                                          * closed (ygfx_close, then open) */
#define YGFX_ERR_IO              (-11) /* a file could not be written     */

/* Ring record kinds under YRT_SRC_GFX. */
#define YGFX_EV_OPEN    1u
#define YGFX_EV_LUT     2u
#define YGFX_EV_CLIPPED 3u
#define YGFX_EV_COLOR   4u
#define YGFX_EV_CAPTURE_DROP 5u   /* v0.12: a chosen frame with no free slot */

/* ygfx_features() */
#define YGFX_FEAT_PROGRAM_CACHE 0x01u  /* the renderer gives program binaries      */
#define YGFX_FEAT_PLANAR        0x02u  /* NV12 and I420 textures                   */
#define YGFX_FEAT_IMPORT_GL     0x04u  /* YGFX_IMPORT_GL                         */
#define YGFX_FEAT_IMPORT_D3D11  0x08u  /* YGFX_IMPORT_D3D11 of RGBA textures      */
#define YGFX_FEAT_IMPORT_NV12   0x10u  /* YGFX_IMPORT_D3D11 of NV12 textures      */
#define YGFX_FEAT_INSTANCES     0x20u  /* ygfx_instances()                       */

/* --- resources ------------------------------------------------------------ */

typedef struct ygfx_tex  { uint32_t id; } ygfx_tex;    /* 0 = none          */
typedef struct ygfx_buf  { uint32_t id; } ygfx_buf;    /* 0 = none          */
typedef struct ygfx_pipe { uint32_t id; } ygfx_pipe;   /* 0 = none          */
typedef struct ygfx_cset { uint32_t id; } ygfx_cset;   /* 0 = none (v0.6)   */

typedef enum ygfx_format {
    YGFX_FORMAT_NONE = 0,
    YGFX_R8,       /* uint8_t planes, 0..255 read as 0..1                     */
    YGFX_RG8,
    YGFX_RGBA8,
    YGFX_R16F,     /* float input, stored as half                             */
    YGFX_RGBA16F,
    YGFX_R32F,     /* float                                                   */
    YGFX_RGBA32F,
    YGFX_R16UI,    /* uint16_t planes, read as k / 65535 (16-bit images)      */
    /* v0.4: planar 8-bit 4:2:0 video, chroma ceil(w / 2) x ceil(h / 2)
     * (VIDEO); one texture, two or three planes */
    YGFX_NV12,     /* Y R8, then CbCr interleaved RG8                         */
    YGFX_I420,     /* Y, Cb, Cr: three R8 planes                              */
    YGFX_RGBA32UI, /* v0.6: uint32_t x 4 (a curve set's words)                */
    YGFX_FORMAT_COUNT
} ygfx_format;

/* How a texture's values encode light (VIDEO). The values are ysp/video.h's
 * YVID_* ones, so its fields pass through; 0 = unspecified, which a planar
 * texture refuses. */
#define YGFX_MATRIX_RGB      1   /* no matrix: R'G'B' texels                  */
#define YGFX_MATRIX_BT601    2
#define YGFX_MATRIX_BT709    3
#define YGFX_MATRIX_BT2020   4   /* non-constant luminance                    */
#define YGFX_RANGE_LIMITED   1   /* Y' 16..235, C 16..240 of 255              */
#define YGFX_RANGE_FULL      2   /* 0..255; C = (code - 128) / 255            */
#define YGFX_TRC_DEVICE      1   /* R'G'B' are device values: through the
                                    * display's own transfer (the inverse of the
                                    * CLUT), so the output stage writes the code
                                    * back                                       */
#define YGFX_TRC_BT1886      2   /* V^2.4 (black at the display's black)      */
#define YGFX_TRC_SRGB        3   /* IEC 61966-2-1                             */
#define YGFX_TRC_LINEAR      4   /* the values are linear already             */
#define YGFX_TRC_GAMMA22     5   /* V^2.2                                     */
#define YGFX_PRIM_DEVICE     1   /* the source's RGB is the display's: pass
                                    * through, stated                            */
#define YGFX_PRIM_BT709      2   /* the rest need a calibration with xy       */
#define YGFX_PRIM_BT601_525  3
#define YGFX_PRIM_BT601_625  4
#define YGFX_PRIM_BT2020     5
#define YGFX_SITING_NONE     1   /* not subsampled (an RGB texture)           */
#define YGFX_SITING_LEFT     2   /* MPEG-2, H.264, HEVC: co-sited across,
                                    * between rows                              */
#define YGFX_SITING_CENTER   3   /* MPEG-1, JPEG: between both ways           */
#define YGFX_SITING_TOP_LEFT 4   /* co-sited both ways                        */

typedef struct ygfx_encoding {
    uint8_t matrix, range, transfer, primaries, siting;
    uint8_t chroma_nearest;      /* 1: chroma replicated, not interpolated      */
    uint8_t reserved_[2];
} ygfx_encoding;

/* The planes of a planar texture: Y, then CbCr (NV12) or Cb and Cr (I420).
 * stride 0 = tight. */
typedef struct ygfx_planes { const void* data[3]; size_t stride[3]; } ygfx_planes;

/* A render target (TARGETS): drawn with the normal API between
 * ygfx_begin_target() and ygfx_end_target(), then drawn as an IMAGE. */
typedef struct ygfx_target_desc {
    int32_t       w, h;          /* required, pixels                             */
    ygfx_format format;        /* 0 = YGFX_RGBA16F; or YGFX_RGBA8          */
    bool          linear;        /* linear filtering when drawn scaled           */
} ygfx_target_desc;

/* What a texture used as a YGFX_MASK_TEX holds (DISTANCE TEXTURES). */
typedef enum ygfx_sdf_kind {
    YGFX_SDF_NONE = 0,         /* not a distance texture; as a mask, DIST      */
    YGFX_SDF_DIST,             /* R16F or R32F: distance in texels, negative
                                  * inside (ygfx_sdf_from_mask)                */
    YGFX_SDF_MSDF,             /* msdfgen multi-channel: the median of rgb,
                                  * 0.5 on the edge, above 0.5 inside           */
    YGFX_SDF_MTSDF             /* MSDF in rgb, the true distance in alpha      */
} ygfx_sdf_kind;

typedef struct ygfx_texture_desc {
    int32_t       w, h;          /* required                                     */
    ygfx_format format;        /* required                                     */
    bool          linear;        /* linear filtering; default nearest (1:1)      */
    const void*   data;          /* NULL = zeros; row 0 is the top row           */
    size_t        stride;        /* bytes per row; 0 = tight                     */
    /* v0.3 */
    ygfx_sdf_kind sdf;         /* MSDF, MTSDF: RGBA8, RGBA16F or RGBA32F       */
    float         sdf_range;     /* MSDF, MTSDF: the atlas's distance range, the
                                  * full width in texels (msdf-atlas-gen's
                                  * pxrange); required for them.
                                  * DIST (v0.4): twice its padding; rectangles
                                  * and glyph runs on a DIST atlas need it      */
    /* v0.4 */
    ygfx_encoding enc;         /* VIDEO: required for NV12 and I420; R8, RG8
                                  * (v0.10.5), RGBA8, RGBA16F, RGBA32F: all 0 =
                                  * values are linear                           */
    const ygfx_planes* planes; /* NV12, I420: the first frame; NULL = black   */
} ygfx_texture_desc;

/* A texture ysp_gfx did not make (VIDEO): a GL texture of this context, or
 * on Windows a D3D11 texture on ANGLE's device (yscr_native()). */
typedef enum ygfx_import_kind { YGFX_IMPORT_GL = 1, YGFX_IMPORT_D3D11 = 2 } ygfx_import_kind;
typedef struct ygfx_import_desc {
    ygfx_import_kind kind;     /* required                                     */
    int32_t       w, h;          /* required: texels (the Y plane's for NV12)    */
    ygfx_format format;        /* R8, RG8, RGBA8 (D3D11 BGRA8 too), RGBA16F,
                                  * R32F, RGBA32F; NV12                          */
    ygfx_encoding enc;         /* as ygfx_texture_desc                      */
    uint32_t      gl_tex[2];     /* GL: names; NV12: Y (R8), CbCr (RG8)          */
    void*         d3d11_tex;     /* D3D11: an ID3D11Texture2D* with
                                  * D3D11_BIND_SHADER_RESOURCE                   */
    uint32_t      d3d11_slice;   /* D3D11: the array slice (a decoder's output) */
} ygfx_import_desc;

/* --- open ------------------------------------------------------------------ */

typedef enum ygfx_units  { YGFX_PX = 0, YGFX_DEG = 1 } ygfx_units;
typedef enum ygfx_dither {
    YGFX_DITHER_NONE = 0,      /* round to nearest                             */
    YGFX_DITHER_ORDERED = 1,   /* Bayer 8 x 8, the same every frame            */
    YGFX_DITHER_NOISE = 2      /* white, from a hash of pixel, frame and seed  */
} ygfx_dither;
typedef enum ygfx_stereo {
    YGFX_MONO = 0, YGFX_SIDE_BY_SIDE, YGFX_ROW_INTERLEAVED, YGFX_FRAME_SEQUENTIAL
} ygfx_stereo;
typedef enum ygfx_output {
    YGFX_OUT_8 = 0,            /* 8 bits per channel                           */
    YGFX_OUT_10,               /* refused in v0.1                              */
    YGFX_OUT_MONO_PP,          /* Bits# Mono++, refused in v0.1                */
    YGFX_OUT_COLOR_PP          /* Bits# Color++, refused in v0.1               */
} ygfx_output;
/* The row order of framebuffer 0, which the output stage writes. Not the
 * coordinate convention: that is COORDINATES in the manual. */
typedef enum ygfx_rows {
    YGFX_ROWS_AUTO = 0,        /* from the screen's backend                    */
    YGFX_ROWS_BOTTOM_UP,       /* row 0 is the bottom (GL's own)               */
    YGFX_ROWS_TOP_DOWN         /* row 0 is the top (ANGLE's client buffers)    */
} ygfx_rows;

/* Nine points of a box: of the screen for `place`, of the stimulus's own box
 * for `anchor`. CENTER is the zero default for both. */
typedef enum ygfx_align {
    YGFX_CENTER = 0, YGFX_TOP_LEFT, YGFX_TOP, YGFX_TOP_RIGHT, YGFX_LEFT,
    YGFX_RIGHT, YGFX_BOTTOM_LEFT, YGFX_BOTTOM, YGFX_BOTTOM_RIGHT
} ygfx_align;

/* Viewing geometry for YGFX_DEG: one scale factor, the pixels per degree at
 * the screen center. */
typedef struct ygfx_view { float distance_mm, width_mm; } ygfx_view;

struct ygfx_backend;

/* Storage for compiled programs (PROGRAM CACHE): a map from a 64-bit key to
 * bytes that the caller owns. ysp_gfx frames and checks the bytes itself, so
 * the two calls only move them. */
typedef struct ygfx_cache {
    /* Copies the entry for key into dst when it fits in cap; returns its
     * size either way, 0 when there is none. */
    size_t (*load)(void* user, uint64_t key, void* dst, size_t cap);
    /* Stores n bytes under key, replacing any entry; < 0 = failed. */
    int    (*store)(void* user, uint64_t key, const void* data, size_t n);
    void*  user;
} ygfx_cache;

/* A cache in a folder: one file per program, <dir>/<key>.yspprog. */
typedef struct ygfx_file_cache {
    ygfx_cache cache;
    char         dir[512];                  /* UTF-8                         */
} ygfx_file_cache;

typedef struct ygfx_desc {
    yscr_screen*  screen;         /* required, unless gl_proc is set          */
    yscr_proc   (*gl_proc)(void* ctx, const char* name);  /* headless: a GL ES
                                     * 3.0 context current on this thread       */
    void*           gl_ctx;
    int32_t         width, height;  /* headless only: framebuffer 0's size      */
    float           background[3];  /* linear device RGB, 0..1; 0 = black       */
    ygfx_units    units;          /* 0 = PX                                   */
    ygfx_view     view;           /* DEG only                                 */
    const ycol_cal* cal;          /* NULL = identity: scene value = device value */
    ygfx_dither   dither;         /* 0 = NONE                                 */
    uint32_t        seed;           /* NOISE dither                             */
    ygfx_output   output;         /* only OUT_8 opens in v0.1                 */
    ygfx_stereo   stereo;         /* only MONO opens in v0.1                  */
    ygfx_rows     rows;           /* 0 = AUTO                                 */
    int32_t         max_draws;      /* per frame; 0 = 1024                      */
    yrt_ring*     ring;           /* YRT_SRC_GFX records; NULL = none       */
    const struct ygfx_backend* backend;   /* NULL = the built-in GL ES 3.0
                                     * backend, or the null backend on a screen
                                     * without GL                               */
    void*           backend_ctx;
    /* v0.4 */
    const ygfx_cache* cache;      /* compiled programs; NULL = compile each
                                     * open. Keep it alive while the gfx is open */
    int32_t         max_instances;  /* elements per frame (INSTANCES); 0 = 16384 */
    /* v0.5 (COLOR) */
    int32_t         cones;          /* ycol_cones for DKL_POLAR and
                                     * ygfx_color(); 0 = the 2 degree cones   */
    const ycol_lum* lum;          /* a participant's luminance; NULL = the
                                     * standard one of `cones`                  */
} ygfx_desc;

/* What ygfx_open() and later program builds did with desc.cache. */
typedef struct ygfx_programs {
    uint32_t loaded;                /* from a cache entry                       */
    uint32_t compiled;              /* from source                              */
    uint32_t rejected;              /* an entry was there and was not used      */
    uint32_t stored, store_failed;
    int64_t  store_ns;              /* of open_ns: reading binaries back, storing */
    int64_t  open_ns;               /* ygfx_open()'s wall time; with
                                     * ygfx_open_start(), both calls' time  */
} ygfx_programs;

/* --- stimuli -------------------------------------------------------------- */

typedef enum ygfx_kind {
    YGFX_KIND_NONE = 0,
    YGFX_SHAPE,        /* SDF shape, solid color                              */
    YGFX_GRATING,      /* sine or square carrier in an SDF aperture           */
    YGFX_GABOR,        /* cosine carrier x Gaussian envelope                  */
    YGFX_DOTS,         /* SDF dots at positions from a buffer                 */
    YGFX_IMAGE,        /* a texture, color or modulation                      */
    YGFX_NOISE,        /* hash noise per check, modulation                    */
    YGFX_TEXT,         /* v0.6: a run of glyphs or paths from a curve set     */
    YGFX_USER,         /* a pipeline from ygfx_pipeline()                   */
    YGFX_KIND_COUNT
} ygfx_kind;

typedef enum ygfx_shape_kind {
    YGFX_RECT = 0,     /* w x h; shape_p[1] = corner radius                   */
    YGFX_CIRCLE,       /* diameter w                                          */
    YGFX_ANNULUS,      /* outer diameter w; shape_p[0] = inner radius         */
    YGFX_LINE,         /* length w; shape_p[0] = width; round caps            */
    YGFX_POLYGON,      /* shape_p[0] = n (3..16); vertices x, y in p[]        */
    YGFX_CROSS,        /* arms w and h long; shape_p[0] = arm width           */
    YGFX_NO_APERTURE,  /* the quad itself (gabor default)                     */
    YGFX_MASK_TEX,     /* the signed-distance texture `mask`, fitted to the box */
    /* v0.3, SHAPE only (the vector program; SHAPES in the manual) */
    YGFX_RRECT,        /* w x h; shape_p = corner radii TL, TR, BR, BL        */
    YGFX_ARC,          /* outer diameter w; [0] sweep deg, [1] thickness,
                          * [2] start deg (clockwise from +x); round ends       */
    YGFX_PIE,          /* diameter w; [0] sweep deg, [2] start deg            */
    YGFX_CAPSULE,      /* length w; [0], [1] radii of the left, right end     */
    YGFX_NGON,         /* circumdiameter w; [0] sides n (3..64), [1] rounding */
    YGFX_STAR,         /* outer diameter w; [0] points n (3..64), [1] inner
                          * radius / outer, [2] rounding                         */
    YGFX_ELLIPSE,      /* w x h, aspect up to 20                               */
    YGFX_QBEZIER,      /* a quadratic Bezier, path = 3 points; [0] width       */
    YGFX_POLYLINE,     /* an open path of 2..224 points; [0] width; join, cap  */
    YGFX_COMPOUND      /* prims, folded by their ops (COMPOUNDS)               */
} ygfx_shape_kind;

/* A stroke: the band between two offsets of the boundary, `stroke` px wide. */
typedef enum ygfx_stroke_align {
    YGFX_STROKE_CENTER = 0,   /* the band straddles the boundary             */
    YGFX_STROKE_INSIDE,       /* the band lies inside it                      */
    YGFX_STROKE_OUTSIDE       /* the band lies outside it                     */
} ygfx_stroke_align;
typedef enum ygfx_join {
    YGFX_JOIN_ROUND = 0,      /* the SDF's own: round outside a turn          */
    YGFX_JOIN_MITER,          /* sharp; RECT (no corner radius), CROSS and
                                 * convex POLYGON only; POLYLINE                */
    YGFX_JOIN_BEVEL           /* POLYLINE only                                */
} ygfx_join;

/* The ends of a dash, of a trimmed outline and of an open path (POLYLINE,
 * QBEZIER). LINE, CAPSULE and ARC have round ends of their own. */
typedef enum ygfx_cap {
    YGFX_CAP_BUTT = 0, YGFX_CAP_ROUND, YGFX_CAP_SQUARE
} ygfx_cap;

/* How a color stimulus meets the scene, in linear light (BLEND). */
typedef enum ygfx_blend_mode {
    YGFX_BLEND_MODE_OVER = 0,  /* premultiplied over: an occluder             */
    YGFX_BLEND_MODE_ADD,       /* rgb += color x coverage: superposed light   */
    YGFX_BLEND_MODE_MULTIPLY   /* rgb *= 1 - a + a color: a filter            */
} ygfx_blend_mode;

/* A compound's operations: each primitive meets everything before it. */
typedef enum ygfx_op {
    YGFX_OP_UNION = 0, YGFX_OP_INTERSECT, YGFX_OP_SUBTRACT, YGFX_OP_XOR,
    YGFX_OP_SMOOTH_UNION, YGFX_OP_SMOOTH_INTERSECT, YGFX_OP_SMOOTH_SUBTRACT
} ygfx_op;

#ifndef YGFX_MAX_PRIMS
#define YGFX_MAX_PRIMS 56            /* primitives per compound               */
#endif
#define YGFX_MAX_PATH  224           /* points per POLYLINE                   */

/* One primitive of a compound: a plain value, 48 bytes. Its fields are in
 * the gfx's units, in the compound's box frame (pixels from the box center
 * along its axes). */
typedef struct ygfx_prim {
    uint8_t shape;          /* CIRCLE, ANNULUS, RECT, RRECT, LINE, CAPSULE,
                             * CROSS, ARC, PIE, NGON, STAR, ELLIPSE              */
    uint8_t op;             /* ygfx_op with everything before it; the first
                             * primitive's is ignored                           */
    uint8_t reserved_[2];
    float   x, y;           /* center                                           */
    float   w, h;           /* its box, as the shape's own w, h                 */
    float   ori;            /* degrees, +x toward +y                            */
    float   k;              /* SMOOTH_*: blend radius                           */
    float   onion;          /* > 0: the shell |d| - onion of this primitive     */
    float   p[4];           /* as shape_p                                       */
} ygfx_prim;

/* Effects drawn from the same distance field, in one pass (EFFECTS). px. */
typedef struct ygfx_shadow {
    float sigma;            /* Gaussian SD; 0 = a hard silhouette                */
    float spread;           /* the silhouette moved out (+) or in (-)            */
    float color[3];         /* linear device RGB                                 */
    float opacity;          /* 0 = off                                           */
} ygfx_shadow;

typedef struct ygfx_band {
    float a1, a2;           /* offsets of the boundary, a1 < a2; -2, 0 = a 2 px
                             * inner outline                                     */
    float color[3];
    float opacity;          /* 0 = off                                           */
} ygfx_band;

#define YGFX_FX_EXACT_BLUR 0x1u  /* drop shadow of a sharp RECT: the exact
                                    * Gaussian blur of the rect (separable erf) */

typedef struct ygfx_fx {
    float         dx, dy;   /* screen px: the offset of drop and inner shadow    */
    ygfx_shadow drop;     /* behind the shape, at the offset                   */
    ygfx_shadow glow;     /* behind the shape, at no offset                    */
    ygfx_shadow inner;    /* inside the fill, from the offset                  */
    ygfx_band   band[2];  /* outlines on top                                   */
    uint32_t      flags;    /* YGFX_FX_*                                       */
} ygfx_fx;

/* Fill colors that vary over the shape (PAINT). */
typedef enum ygfx_paint_kind {
    YGFX_PAINT_SOLID = 0,
    YGFX_PAINT_LINEAR,    /* along x0, y0 -> x1, y1                            */
    YGFX_PAINT_RADIAL,    /* center x0, y0; radius x1                          */
    YGFX_PAINT_ANGULAR,   /* center x0, y0; start x1 degrees, clockwise        */
    YGFX_PAINT_VERTEX     /* a color per vertex of a convex POLYGON            */
} ygfx_paint_kind;

typedef enum ygfx_space {
    YGFX_SPACE_RGB = 0,   /* linear device RGB (also XYZ, LMS, Cartesian DKL:
                             * linear maps of it give the same mix)             */
    YGFX_SPACE_OKLAB,     /* ysp/color.h's Oklab: absolute XYZ, display white */
    YGFX_SPACE_DKL_POLAR  /* elevation, azimuth, radius about the background  */
} ygfx_space;

typedef struct ygfx_stop { float t; float color[3]; } ygfx_stop;

typedef struct ygfx_paint {
    uint8_t kind, space;    /* ygfx_paint_kind, ygfx_space                   */
    uint8_t repeat;         /* t past 0..1 wraps; else it is clamped             */
    uint8_t n;              /* stops, 2..6                                       */
    float   x0, y0, x1, y1; /* units, the box frame (from its center)            */
    ygfx_stop stops[6];   /* t rising 0..1; colors in linear device RGB        */
    const float* vertex_colors;   /* VERTEX: rgb per POLYGON vertex             */
} ygfx_paint;

/* One glyph of a run: an instance (TEXT, GLYPH RUNS). 32 bytes. */
typedef struct ygfx_glyph {
    float x, y;             /* the glyph box's top-left, units from the run's
                             * box top-left                                      */
    float sx, sy, sw, sh;   /* its atlas rectangle, texels from the top-left,
                             * padding included                                  */
    float reserved[2];
} ygfx_glyph;

/* v0.6: one glyph or path placed in a run (RUNS): ygfx_inst's layout with
 * glyph in phase's place, all float, so ygfx_bind.field binds any of them.
 * x, y and glyph are always read; the rest when its bit is in the run's
 * fields. */
typedef struct ygfx_citem {
    float x, y;             /* the glyph's origin (pen position), units from the
                             * run box's top-left (YGFX_I_XY: always)        */
    float ori;              /* degrees about the glyph box's center (YGFX_I_ORI) */
    float glyph;            /* the glyph or path id in the set: an integer      */
    float contrast;         /* multiplies the run's opacity (YGFX_I_CONTRAST) */
    float scale;            /* about the glyph box's center (YGFX_I_SCALE)    */
    float gate;             /* multiplies gate (YGFX_I_GATE)                   */
    float color;            /* a palette position, as INSTANCES (YGFX_I_COLOR) */
} ygfx_citem;

/* One element of an instanced stimulus (INSTANCES): 32 bytes, all float, so
 * ygfx_bind.field binds any of them. Only the fields in the stimulus's
 * inst_fields mask are read. */
typedef struct ygfx_inst {
    float x, y;             /* units along the template's turned axes, from its
                             * anchor (YGFX_I_XY)                              */
    float ori;              /* degrees, added to the template's (YGFX_I_ORI)   */
    float phase;            /* cycles, added; GRATING, GABOR (YGFX_I_PHASE)    */
    float contrast;         /* multiplies contrast and opacity (YGFX_I_CONTRAST) */
    float scale;            /* multiplies what is in units, about the anchor, as a
                             * group's scale; not NOISE (YGFX_I_SCALE)          */
    float gate;             /* multiplies gate; 0 also hides it from
                             * ygfx_hit_index() (YGFX_I_GATE)                */
    float color;            /* a palette position: an integer is that entry, a
                             * fraction mixes two in linear light (YGFX_I_COLOR) */
} ygfx_inst;

#define YGFX_I_XY       0x01u
#define YGFX_I_ORI      0x02u
#define YGFX_I_PHASE    0x04u
#define YGFX_I_CONTRAST 0x08u
#define YGFX_I_SCALE    0x10u
#define YGFX_I_GATE     0x20u
#define YGFX_I_COLOR    0x40u
#define YGFX_MAX_PALETTE 16

struct ygfx_group;

typedef enum ygfx_edge {
    YGFX_EDGE_HARD = 0,    /* 1 where the pixel center is inside, else 0      */
    YGFX_EDGE_COSINE,      /* raised cosine, edge_width = full 0-to-1 width   */
    YGFX_EDGE_GAUSSIAN     /* hard edge blurred by a Gaussian, SD edge_width  */
} ygfx_edge;

typedef enum ygfx_noise_dist { YGFX_UNIFORM = 0, YGFX_BINARY = 1, YGFX_GAUSSIAN = 2,
                                 YGFX_SIMPLEX = 3   /* v0.10: gradient noise, fBm */ } ygfx_noise_dist;

/* One stimulus. A plain value: copy it, keep arrays of it, set fields
 * directly. The float fields are the parameter table's (ygfx_params) and
 * can be bound to timeline channels (ygfx_apply). Spatial fields are in
 * the units of the gfx (pixels, or degrees as a scale factor), edges and dot
 * sizes in pixels. Screen axes: origin top-left, x right, y down; ori turns
 * +x toward +y, clockwise on the screen (COORDINATES). */
typedef struct ygfx_stim {
    uint16_t    kind, shape, edge, wave;   /* ygfx_kind, _shape_kind, _edge;
                                            * wave 0 sine, 1 square           */
    uint32_t    flags;                     /* YGFX_STIM_*                     */
    uint8_t     place;                     /* ygfx_align: the screen point x,
                                            * y are measured from             */
    uint8_t     reserved_[3];
    float       visible;      /* < 0.5: not drawn                               */
    float       x, y;         /* the anchor's offset from the place point       */
    float       ax, ay;       /* anchor: the point of the box at x, y and the
                               * center of rotation, as fractions of the box
                               * (0, 0 top-left; 0.5, 0.5 the center)          */
    float       w, h;         /* size; GABOR: 0 = 8 sigma                       */
    float       ori;          /* degrees, +x toward +y (clockwise on screen)    */
    float       contrast;     /* modulation: amplitude along dir                */
    float       opacity;      /* color stimuli                                  */
    float       gate;         /* multiplier, for flicker tracks                 */
    float       color[3];     /* color stimuli: linear device RGB               */
    float       dir[3];       /* modulation per unit contrast, linear RGB;
                               * 0, 0, 0 = the background (luminance contrast)  */
    float       sf;           /* cycles per unit                                */
    float       phase;        /* cycles                                         */
    float       sigma;        /* GABOR envelope SD                              */
    float       aspect;       /* GABOR envelope: y extent = sigma / aspect      */
    float       edge_width;   /* px                                             */
    float       shape_p[4];   /* see ygfx_shape_kind                          */
    float       dot_size;     /* DOTS diameter, px                              */
    float       seed;         /* NOISE: an integer below 2^24                   */
    float       check;        /* NOISE check size, px; 0 = 1                    */
    float       p[32];        /* USER parameters; POLYGON vertices              */
    ygfx_tex  tex;          /* IMAGE; USER: ysp_tex0 (v0.10.4)                */
    ygfx_buf  buf;          /* DOTS: x, y float pairs, units, from x, y       */
    uint32_t    count;        /* DOTS                                           */
    ygfx_pipe pipe;         /* USER                                           */
    /* v0.2 */
    float       stroke;       /* band width, px; 0 = fill                       */
    float       miter_limit;  /* MITER: miter length / stroke width; 0 = 4       */
    float       tint[4];      /* IMAGE: linear rgb and alpha factors; 1 each      */
    float       src[4];       /* IMAGE: texel rect x, y, w, h from the top-left;
                               * w or h 0 = the whole texture                    */
    uint8_t     stroke_align, join;   /* ygfx_stroke_align, ygfx_join          */
    uint8_t     reserved2_[2];
    ygfx_tex  mask;         /* YGFX_MASK_TEX: signed distance in texels      */
    const struct ygfx_group* group;   /* NULL = none (GROUPS)                  */
    /* v0.3 */
    float       offset;       /* px: the boundary moved out (+) or in (-); SHAPE */
    float       dash[2];      /* px: dash and gap along the boundary; 0 = none   */
    float       dash_offset;  /* px along the boundary: march it                 */
    float       trim[2];      /* start, end as fractions of the length; 0, 0 =
                               * the whole                                       */
    uint8_t     cap;          /* ygfx_cap                                      */
    uint8_t     blend;        /* ygfx_blend_mode, color stimuli                */
    uint8_t     dash_snap;    /* closed boundaries: a whole number of periods    */
    uint8_t     reserved3_;
    uint32_t    n_path;       /* POLYLINE, QBEZIER points                        */
    uint32_t    n_prims;      /* COMPOUND                                        */
    const float*        path; /* x, y pairs, units, from the box center          */
    const ygfx_prim*  prims;
    const ygfx_fx*    fx;   /* NULL = none                                     */
    const ygfx_paint* paint;   /* NULL = color                                 */
    /* v0.4 */
    uint32_t    first;        /* DOTS, glyph runs: the first record of buf drawn */
    const ygfx_inst* inst;  /* INSTANCES: n_inst elements; NULL = one stimulus */
    uint32_t    n_inst;
    uint32_t    inst_fields;  /* YGFX_I_*                                      */
    const float* palette;     /* rgb triples: color (color stimuli) or dir
                               * (modulation) by YGFX_I_COLOR                  */
    uint32_t    n_palette;    /* 1..YGFX_MAX_PALETTE                           */
    /* v0.6 */
    ygfx_cset set;          /* TEXT: the curve set                             */
    const ygfx_citem* items; /* TEXT: count items copied at each draw; NULL = count
                               * items in buf from first                         */
    float       size;         /* TEXT: units per set unit (the font size per em)  */
    /* v0.10: NOISE SIMPLEX */
    float       gn_scale;     /* units per lattice cell (the feature size)       */
    float       gn_z;         /* the third coordinate, cells: bind it to evolve  */
    float       gn_lacunarity, gn_gain;   /* per octave: frequency, amplitude  */
    int32_t     gn_octaves;   /* 1..8                                            */
} ygfx_stim;

/* v0.6, BLUR: a separable Gaussian blur of a target (the layer) into
 * another (the result). */
typedef enum ygfx_blur_source {
    YGFX_BLUR_BOX = 0,      /* the layer holds box-filtered coverage (curve runs,
                               * antialiased edges): the box's variance comes
                               * out of sigma                                 */
    YGFX_BLUR_POINT         /* the layer holds point samples (gratings)        */
} ygfx_blur_source;

typedef struct ygfx_blur_desc {
    int32_t w, h;             /* the result, px; required                        */
    ygfx_format format;     /* 0 = RGBA16F; R16F (a coverage mask), RGBA32F    */
    int32_t supersample;      /* 0, 1: none; 2: the layer is 2w x 2h (opt-in)    */
    ygfx_blur_source source;
    float   sigma;            /* the first sigma, px of the result              */
} ygfx_blur_desc;

/* A plain value the caller keeps. layer is a target to draw into; image
 * draws result (premultiplied; an R16F result is coverage tinted by
 * image.tint); sigma (px of the result) may be bound with ygfx_bind.field. */
typedef struct ygfx_blur {
    ygfx_tex  layer, result;
    ygfx_stim image;
    float       sigma;
    ygfx_tex  scratch_;     /* private from here                               */
    int32_t     w_, h_, ss_, src_;
} ygfx_blur;

/* ygfx_instances(): the elements and what they set. fields 0 = YGFX_I_XY. */
typedef struct ygfx_instances_desc {
    const ygfx_inst* inst;
    int                n;
    uint32_t           fields;
    const float*       palette;
    int                n_palette;
} ygfx_instances_desc;

#define YGFX_STIM_MODULATION    0x1u  /* IMAGE: the red channel modulates (g) */
#define YGFX_STIM_LINEAR        0x2u  /* IMAGE: sample with the texture filter */
#define YGFX_STIM_ADD           0x4u  /* IMAGE: add texel.rgb (a layer of
                                         * increments, from a target)           */
#define YGFX_STIM_PREMULTIPLIED 0x8u  /* IMAGE: rgb already times alpha (set
                                         * for a target by ygfx_image)        */
#define YGFX_STIM_ALIASED      0x10u  /* TEXT: coverage 0 or 1 at the pixel
                                         * center (v0.6)                        */
#define YGFX_STIM_COVERAGE     0x20u  /* IMAGE: red is coverage, tint the color
                                         * (v0.6)                               */
#define YGFX_STIM_RAYS         0x40u  /* TEXT: the rays everywhere (v0.7)   */

/* A flat group (GROUPS): place, anchor and box on the screen; its members'
 * place, x, y and ori are in its frame. One level: a group has no group. */
typedef struct ygfx_group {
    uint8_t place;            /* ygfx_align, on the screen or target          */
    uint8_t reserved_[3];
    float   visible;          /* < 0.5: no member is drawn                       */
    float   x, y;             /* units, along the screen axes, from place        */
    float   ax, ay;           /* anchor: fractions of the group's box            */
    float   w, h;             /* the group's box, units; 0 x 0 = a point         */
    float   ori;              /* degrees, +x toward +y, about the anchor         */
    float   scale;            /* multiplies members' sizes in units, not in px   */
    float   opacity;          /* multiplies every member's gate                  */
} ygfx_group;

typedef struct ygfx_group_desc {
    ygfx_align place, anchor;                                /* 0 = CENTER */
    float x, y, w, h, ori;
    float scale, opacity;                                      /* 0 = 1 */
} ygfx_group_desc;

enum {
    YGFX_G_VISIBLE, YGFX_G_X, YGFX_G_Y, YGFX_G_AX, YGFX_G_AY, YGFX_G_W, YGFX_G_H,
    YGFX_G_ORI, YGFX_G_SCALE, YGFX_G_OPACITY, YGFX_G_COUNT
};

typedef struct ygfx_shape_desc {
    ygfx_align place, anchor;                                /* 0 = CENTER */
    ygfx_shape_kind shape; ygfx_edge edge; float edge_width;
    float x, y, w, h, ori; float color[3]; float opacity;     /* 0 = 1 */
    float shape_p[4]; const float* vertices;                   /* POLYGON: n x, y */
    float stroke; ygfx_stroke_align stroke_align; ygfx_join join; float miter_limit;
    ygfx_tex mask; const ygfx_group* group;
    /* v0.3 */
    float offset; float dash[2]; float dash_offset; float trim[2];
    ygfx_cap cap; ygfx_blend_mode blend; bool dash_snap;
    const float* path; int n_path;                             /* POLYLINE, QBEZIER */
    const ygfx_fx* fx; const ygfx_paint* paint;
} ygfx_shape_desc;

/* A compound (COMPOUNDS): its box places and anchors it, its prims draw it. */
typedef struct ygfx_compound_desc {
    ygfx_align place, anchor;                                /* 0 = CENTER */
    const ygfx_prim* prims; int n;                           /* 1..YGFX_MAX_PRIMS */
    float x, y, w, h, ori;
    float onion;                                               /* units, on the result */
    ygfx_edge edge; float edge_width;
    float color[3]; float opacity;                             /* 0 = 1 */
    float stroke; ygfx_stroke_align stroke_align; float offset;
    ygfx_blend_mode blend;
    const ygfx_fx* fx; const ygfx_paint* paint;
    const ygfx_group* group;
} ygfx_compound_desc;

/* A glyph run (GLYPH RUNS): one draw of count glyphs from an atlas. */
typedef struct ygfx_glyphs_desc {
    ygfx_align place, anchor;                                /* 0 = CENTER */
    ygfx_tex atlas;                                          /* MSDF, MTSDF or DIST */
    ygfx_buf buf; uint32_t count;                            /* ygfx_glyph records */
    float scale;                                               /* units per atlas texel */
    float x, y, w, h, ori;                                     /* the run's box */
    ygfx_edge edge; float edge_width;
    float color[3]; float opacity;                             /* 0 = 1 */
    float stroke; ygfx_stroke_align stroke_align; ygfx_join join; float offset;
    ygfx_blend_mode blend;
    const ygfx_group* group;
    uint32_t first;                                            /* v0.4: the first record in buf */
} ygfx_glyphs_desc;

/* v0.6, CURVE SETS: one font or artwork set as two flat arrays, the format
 * of docs/gfx.md "Curve sets: the format" (version 1). */
#define YGFX_CSET_MAGIC    0x43505359u   /* "YSPC" in little-endian bytes   */
#define YGFX_CSET_VERSION  1u
#define YGFX_CSET_EVENODD  0x1u          /* glyph flags: even-odd fill      */
#define YGFX_CSET_BACKWARD 0x2u          /* glyph flags: backward lists     */
#define YGFX_CSET_RESOLVED 0x4u          /* glyph flags: no contours overlap,
                                            * the winding is 0 or 1 everywhere   */

typedef struct ygfx_cset_desc {
    const float*    texels;   uint32_t n_texels;   /* 4 floats each               */
    const uint32_t* words;    uint32_t n_words;    /* a multiple of 4             */
    uint32_t        cap_texels, cap_words;         /* growth room; 0 = no growth  */
    bool            keep;     /* the arrays outlive the set and ysp_gfx may read
                               * them: exact hit tests, and ygfx_cset_add()
                               * reads the new glyphs from them              */
} ygfx_cset_desc;

/* A run (RUNS): glyphs or paths of a curve set, one instanced draw. */
typedef struct ygfx_crun_desc {
    ygfx_align place, anchor;                                /* 0 = CENTER */
    ygfx_cset set;                                           /* required */
    const ygfx_citem* items; int n;                           /* copied at each draw */
    ygfx_buf buf; uint32_t first;                            /* or n items in buf from
                                                                * first, uploaded once */
    uint32_t fields;                                           /* YGFX_I_*; 0 = x, y, glyph */
    const float* palette; int n_palette;                       /* YGFX_I_COLOR */
    float size;                                                /* units per set unit; required */
    float x, y, w, h, ori;                                     /* the run's box; w, h 0 =
                                                                * the items' extent */
    float color[3]; float opacity;                             /* 0 = 1 */
    ygfx_blend_mode blend;
    bool aliased;                                              /* coverage 0 or 1 at the
                                                                * pixel center */
    bool rays;                                                 /* v0.7: the rays even
                                                                * where the exact area
                                                                * applies (RUNS) */
    const ygfx_group* group;
} ygfx_crun_desc;

typedef struct ygfx_grating_desc {
    ygfx_align place, anchor;                                /* 0 = CENTER */
    float x, y, w, h, ori, sf, phase; float contrast;          /* 0 = 1 */
    int square;                                                /* square wave */
    ygfx_shape_kind aperture;                                /* 0 = RECT */
    ygfx_edge edge; float edge_width; float dir[3];
    float stroke; ygfx_stroke_align stroke_align; ygfx_tex mask; const ygfx_group* group;
    float shape_p[4];                                          /* v0.4: the aperture's */
} ygfx_grating_desc;

typedef struct ygfx_gabor_desc {
    ygfx_align place, anchor;                                /* 0 = CENTER */
    float x, y, ori, sf, phase, sigma; float contrast;         /* 0 = 1 */
    float aspect;                                              /* 0 = 1 */
    float size;                                                /* 0 = 8 sigma */
    float dir[3];
    const ygfx_group* group;
} ygfx_gabor_desc;

typedef struct ygfx_dots_desc {
    ygfx_align place, anchor;                                /* 0 = CENTER */
    ygfx_buf buf; uint32_t count; float x, y, ori;
    float dot_size; ygfx_edge edge; float edge_width;
    float color[3]; float opacity;                             /* 0 = 1 */
    ygfx_shape_kind aperture; float w, h;                    /* field; RECT,
                                                * CIRCLE or NO_APERTURE (default) */
    float stroke; ygfx_stroke_align stroke_align;                 /* rings */
    const ygfx_group* group;
    uint32_t first;                                            /* v0.4: the first x, y pair in buf */
} ygfx_dots_desc;

typedef struct ygfx_image_desc {
    ygfx_align place, anchor;                                /* 0 = CENTER */
    ygfx_tex tex; float x, y, w, h, ori;                     /* w, h 0 = texels */
    bool modulation; bool linear; float contrast; float opacity; /* 0 = 1 */
    float dir[3];
    float src[4];                                              /* w 0 = all */
    float tint[4];                                             /* all 0 = 1 */
    bool add;                                                  /* YGFX_STIM_ADD */
    const ygfx_group* group;
    /* v0.4 */
    bool premultiplied;            /* the texture holds rgb x alpha (YGFX_STIM_PREMULTIPLIED);
                                    * default straight alpha. A target sets it */
    /* v0.6 */
    bool coverage;                 /* a 1-channel texture's red is coverage: rgb =
                                    * tint.rgb, alpha = red x tint.a            */
} ygfx_image_desc;

typedef struct ygfx_noise_desc {
    ygfx_align place, anchor;                                /* 0 = CENTER */
    float x, y, w, h, ori; float check; uint32_t seed;
    ygfx_noise_dist dist; float contrast;                    /* 0 = 1 */
    ygfx_shape_kind aperture; ygfx_edge edge; float edge_width; float dir[3];
    float stroke; ygfx_stroke_align stroke_align; ygfx_tex mask; const ygfx_group* group;
    float shape_p[4];                                          /* v0.4: the aperture's */
    /* v0.10, SIMPLEX: gradient noise (NOISE) */
    float scale;                   /* units per lattice cell; required          */
    float z;                       /* cells                                     */
    int   octaves;                 /* 0 = 1; at most 8                          */
    float lacunarity, gain;        /* 0 = 2 and 0.5                             */
} ygfx_noise_desc;

typedef struct ygfx_user_desc {
    ygfx_align place, anchor;                                /* 0 = CENTER */
    ygfx_pipe pipe; float x, y, w, h, ori; float contrast, opacity; /* 0 = 1 */
    ygfx_shape_kind aperture; ygfx_edge edge; float edge_width;
    float color[3]; float dir[3]; const float* p; int n_p;
    float stroke; ygfx_stroke_align stroke_align; ygfx_tex mask; const ygfx_group* group;
    float shape_p[4];                                          /* v0.4: the aperture's */
    ygfx_tex tex;                                              /* v0.10.4: ysp_tex0 */
} ygfx_user_desc;

/* --- user shaders ---------------------------------------------------------- */

typedef enum ygfx_shader_mode {
    YGFX_MODULATION = 0,  /* float ysp_main(vec2 p): g in [-1, 1], added as
                             * contrast * gate * g * aperture * dir           */
    YGFX_COLOR = 1,       /* vec4 ysp_main(vec2 p): linear rgb and coverage,
                             * blended over with opacity * gate * aperture    */
    YGFX_ADD = 2          /* vec3 ysp_main(vec2 p): an rgb increment, added
                             * as contrast * gate * aperture * it             */
} ygfx_shader_mode;

typedef struct ygfx_pipeline_desc {
    const char*        body;   /* GLSL ES 3.00: ysp_main and its helpers        */
    ygfx_shader_mode mode;
    const char*        name;   /* for messages                                  */
} ygfx_pipeline_desc;

/* --- timeline binding ------------------------------------------------------ */

/* Indices into the parameter table: the bindable float fields of
 * ygfx_stim, in order. */
enum {
    YGFX_P_VISIBLE, YGFX_P_X, YGFX_P_Y, YGFX_P_AX, YGFX_P_AY, YGFX_P_W, YGFX_P_H, YGFX_P_ORI,
    YGFX_P_CONTRAST, YGFX_P_OPACITY, YGFX_P_GATE,
    YGFX_P_COLOR_R, YGFX_P_COLOR_G, YGFX_P_COLOR_B,
    YGFX_P_DIR_R, YGFX_P_DIR_G, YGFX_P_DIR_B,
    YGFX_P_SF, YGFX_P_PHASE, YGFX_P_SIGMA, YGFX_P_ASPECT, YGFX_P_EDGE_WIDTH,
    YGFX_P_SHAPE_P0, YGFX_P_SHAPE_P1, YGFX_P_SHAPE_P2, YGFX_P_SHAPE_P3,
    YGFX_P_DOT_SIZE, YGFX_P_SEED, YGFX_P_CHECK,
    YGFX_P_P0,
    /* v0.2, appended so v0.1's indices stay */
    YGFX_P_STROKE = YGFX_P_P0 + 32, YGFX_P_MITER_LIMIT,
    YGFX_P_TINT_R, YGFX_P_TINT_G, YGFX_P_TINT_B, YGFX_P_TINT_A,
    YGFX_P_SRC_X, YGFX_P_SRC_Y, YGFX_P_SRC_W, YGFX_P_SRC_H,
    /* v0.3 */
    YGFX_P_OFFSET, YGFX_P_DASH, YGFX_P_GAP, YGFX_P_DASH_OFFSET,
    YGFX_P_TRIM_START, YGFX_P_TRIM_END,
    /* v0.6 */
    YGFX_P_SIZE,
    /* v0.10 */
    YGFX_P_NOISE_SCALE, YGFX_P_NOISE_Z, YGFX_P_NOISE_LACUNARITY, YGFX_P_NOISE_GAIN,
    YGFX_P_COUNT
};

typedef struct ygfx_bind {
    ygfx_stim*  stim;
    uint16_t      param;    /* YGFX_P_*, or YGFX_G_* with group              */
    uint16_t      channel;  /* index into the values (ytl_values())           */
    ygfx_group* group;    /* non-NULL: param is a field of this group          */
    float*        field;    /* v0.3, non-NULL: values[channel] goes here; stim,
                             * param and group are not read. Any float: a
                             * primitive's x, a stop's t, a shadow's sigma       */
} ygfx_bind;

/* One field of ygfx_stim a designer or a script sets. */
typedef struct ygfx_param {
    const char* name;     /* "contrast", "p7"                                    */
    const char* type;     /* "f32"                                               */
    double      min, max;
    double      def;      /* the value a constructor gives when its desc is 0   */
    const char* unit;     /* "", "u" (the gfx unit), "px", "deg", "cyc/u", "cyc" */
    const char* doc;
    uint32_t    offset;   /* in ygfx_stim                                      */
    uint32_t    kinds;    /* bit (1 << ygfx_kind) for each kind that reads it  */
} ygfx_param;

/* --- backend (the gfx backend interface) ------------------------------------ */

#define YGFX_BACKEND_VERSION 4   /* 3: v0.4, program binaries (caps strings,
                                    * pipeline_src.binary, pipeline_binary);
                                    * 4: v0.12, the capture calls. open()
                                    * takes 3 and 4 */

typedef struct ygfx_backend_open {
    yscr_proc (*gl_proc)(void* ctx, const char* name);
    void*       gl_ctx;
    int32_t     w, h;              /* framebuffer 0                              */
} ygfx_backend_open;

typedef struct ygfx_backend_caps {
    bool     color_buffer_float;   /* float render targets                       */
    bool     float_blend;          /* blending into 32-bit float targets          */
    bool     float_linear;         /* linear filtering of 32-bit float textures   */
    int32_t  ubo_align;
    int32_t  max_ubo;
    char     renderer[160];
    /* version 3: what a program binary is valid for (the cache key), and
     * the binary format; 0 = no binaries */
    char     vendor[64];
    char     version[128];
    char     glsl[128];
    uint32_t binary_format;
    uint32_t features;             /* YGFX_FEAT_IMPORT_*                       */
    int32_t  max_texture;          /* v0.6: GL_MAX_TEXTURE_SIZE; 0 = 2048         */
    bool     parallel_compile;     /* v0.11.1: pipeline_make returns while the
                                    * driver compiles (KHR_parallel_shader_compile) */
} ygfx_backend_caps;

#define YGFX_BLEND_NONE  0
#define YGFX_BLEND_ADD   1       /* rgb += src, alpha kept                      */
#define YGFX_BLEND_OVER  2       /* premultiplied over, alpha accumulates       */
#define YGFX_BLEND_MULTIPLY 3    /* rgb = dst (1 - a) + dst src.rgb, alpha kept  */

typedef struct ygfx_pipeline_src {
    const char* vs;
    const char* fs;
    int32_t     blend;             /* YGFX_BLEND_*                              */
    int32_t     instance_attr;     /* 1: attribute 0 is a vec2 per instance; 2:
                                    * attributes 0 and 1 are vec4s, 32 bytes   */
    int32_t     deferred;          /* 1: return once the link is under way; the
                                    * status comes from pipeline_finish (when
                                    * the backend has it), so the driver can
                                    * compile several programs at once        */
    /* version 3: a binary to try first; vs and fs stay valid, and the
     * backend compiles them when the driver refuses it */
    const void* binary;
    uint32_t    binary_size, binary_format;
    int32_t     retrievable;       /* the core will ask for the binary            */
} ygfx_pipeline_src;

typedef struct ygfx_texture_src {
    int32_t       w, h;
    ygfx_format format;
    bool          linear;
    bool          target;          /* render target                               */
    const void*   data;
    size_t        stride;
    /* version 3: wrap a texture instead of making one */
    uint32_t      gl_name;         /* a GL name of this context; not deleted      */
    void*         d3d11;           /* an ID3D11Texture2D* through EGL             */
    int32_t       plane, slice;    /* d3d11: the plane (NV12 0 Y, 1 CbCr), slice   */
} ygfx_texture_src;

#define YGFX_BUFFER_UNIFORM  1
#define YGFX_BUFFER_INSTANCE 2

typedef struct ygfx_bindings {
    uint32_t pipeline;
    uint32_t ubo;                  /* frame block at frame_off, stims at stim_off */
    uint32_t frame_off, frame_size, stim_off, stim_size;
    uint32_t tex[6];               /* units 0..5; 0 = none (version 3: 6)        */
    uint32_t instances;            /* buffer for attribute 0; 0 = none            */
    int32_t  blend;                /* YGFX_BLEND_*; 0 = the pipeline's own      */
    uint32_t instances_off;        /* version 3: bytes into it                     */
} ygfx_bindings;

#define YGFX_READ_FLOAT 1        /* RGBA float                                  */
#define YGFX_READ_UNORM 2        /* RGBA uint8_t                                */

/* Every call runs on the thread that owns the context. Ids are the
 * backend's, never 0. Rows are given and read bottom-up, GL's order. */
typedef struct ygfx_backend {
    uint32_t    version;           /* YGFX_BACKEND_VERSION                      */
    const char* name;
    int  (*open)(void* c, const ygfx_backend_open* in, ygfx_backend_caps* caps, char* err, size_t cap);
    void (*close)(void* c);
    int  (*pipeline_make)(void* c, const ygfx_pipeline_src* d, uint32_t* id, char* err, size_t cap);
    void (*pipeline_free)(void* c, uint32_t id);
    int  (*texture_make)(void* c, const ygfx_texture_src* d, uint32_t* id);
    int  (*texture_update)(void* c, uint32_t id, int x, int y, int w, int h, const void* data, size_t stride);
    void (*texture_free)(void* c, uint32_t id);
    int  (*buffer_make)(void* c, int kind, size_t bytes, uint32_t* id);
    int  (*buffer_update)(void* c, uint32_t id, size_t off, const void* data, size_t n);
    void (*buffer_free)(void* c, uint32_t id);
    void (*pass_begin)(void* c, uint32_t target, int w, int h, const float* clear);  /* target 0 = framebuffer 0 */
    void (*apply)(void* c, const ygfx_bindings* b);
    void (*draw)(void* c, int vertices, int instances);
    void (*pass_end)(void* c);
    void (*present)(void* c);
    int  (*read)(void* c, uint32_t target, int x, int y, int w, int h, int format, void* out);
    void (*reset)(void* c);        /* forget cached state                          */
    /* version 2: waits for a deferred pipeline and checks it; NULL = every
     * pipeline_make is complete when it returns */
    int  (*pipeline_finish)(void* c, uint32_t id, char* err, size_t cap);
    /* version 3. pipeline_make returns 1 when it linked pipeline_src.binary,
     * 0 when it compiled. pipeline_binary copies a finished pipeline's binary
     * into out when it fits in cap and returns its size (< 0: none); NULL =
     * the backend has no binaries */
    int  (*pipeline_binary)(void* c, uint32_t id, void* out, size_t cap, uint32_t* format);
    /* version 4 (CAPTURE): reads of framebuffer 0 that do not wait. NULL =
     * no capture. capture_make makes n buffers of bytes each; capture_read
     * copies a rectangle (GL's rows) into buffer slot and returns at once;
     * capture_poll returns 1 with *data the bytes when that copy is done
     * (the buffer stays readable, from any thread, until capture_unmap), 0
     * when it is not yet, < 0 on an error; wait_ns > 0 lets it wait that
     * long. capture_free unmaps and frees them all. */
    int  (*capture_make)(void* c, int n, size_t bytes);
    int  (*capture_read)(void* c, int slot, int x, int y, int w, int h);
    int  (*capture_poll)(void* c, int slot, int64_t wait_ns, const void** data);
    void (*capture_unmap)(void* c, int slot);
    void (*capture_free)(void* c);
} ygfx_backend;

/* --- the handle ------------------------------------------------------------ */

#ifndef YGFX_MAX_TEXTURES
#define YGFX_MAX_TEXTURES  64
#endif
#ifndef YGFX_MAX_BUFFERS
#define YGFX_MAX_BUFFERS   32
#endif
#ifndef YGFX_MAX_PIPELINES
#define YGFX_MAX_PIPELINES 32
#endif
#define YGFX__N_BUILTIN     10
#define YGFX__N_VSPEC       3           /* specialized vector programs (KINDS) */
#define YGFX__BACKEND_WORDS 1280
#define YGFX__UBO_RING      3
#ifndef YGFX_MAX_PASSES
#define YGFX_MAX_PASSES    16          /* target passes per frame              */
#endif

typedef struct ygfx__seg {
    uint32_t target;               /* backend id; 0 = the scene                   */
    int32_t  w, h, topdown;
    int32_t  first, end;           /* commands                                    */
    int32_t  frame_slot;           /* its frame block                             */
    int32_t  has_clear;
    float    clear[4];
} ygfx__seg;

typedef struct ygfx__res {
    uint32_t used, bid;
    int32_t  w, h, format, flags;
    int32_t  sdf;                  /* ygfx_sdf_kind                             */
    float    sdf_range;            /* texels                                      */
    uint32_t plane_bid[2];         /* NV12: CbCr; I420: Cb, Cr                    */
    ygfx_encoding enc;
    int32_t  view;                 /* the slot whose texels it shows + 1; 0 = own  */
    int32_t  ss;                   /* a 2x blur layer: 2                           */
} ygfx__res;

typedef struct ygfx__cmd {
    uint32_t pipe;                 /* backend id                                  */
    uint32_t tex[6];
    uint32_t inst;                 /* DOTS buffer                                 */
    uint32_t count;                /* DOTS instances                              */
    uint32_t inst_off;             /* bytes into inst                             */
    uint32_t block;                /* index of its stim block                      */
    uint32_t nblk;                 /* blocks it uses: 1, or 1 + its extension      */
    int32_t  blend;                /* YGFX_BLEND_*, 0 = the pipeline's          */
} ygfx__cmd;

#ifndef YGFX_MAX_CSETS
#define YGFX_MAX_CSETS     8           /* curve sets (v0.6)                      */
#endif

/* A curve set's texture pair and what the CPU keeps of it. */
typedef struct ygfx__cs {
    uint32_t used, tex_t, tex_w;   /* backend ids: texels RGBA32F, words RGBA32UI */
    uint32_t n_texels, n_words, cap_t, cap_w, G;
    int32_t  lt;                   /* log2 of both textures' width in texels      */
    float*   bbox;                 /* G x 4, each glyph's box: runs' extents       */
    ygfx_cset_desc keep;         /* the caller's arrays, when desc.keep          */
} ygfx__cs;

/* A batch being gathered by the reorder (DRAW ORDER). */
typedef struct ygfx__rbatch { int32_t head, tail, count, pad_; float box[4]; } ygfx__rbatch;

/* A program's cache entry, kept from its make to its store. */
typedef struct ygfx__prec { uint64_t key, key2; uint32_t material; int store; } ygfx__prec;

/* The gfx. Caller-allocated and zeroed; every field is private. */
typedef struct ygfx_gfx {
    int                    open;
    int                    in_frame;
    int                    in_setup;                /* begin_setup .. end_setup  */
    const ygfx_backend*  be;
    void*                  bctx;
    yscr_screen*         screen;
    ygfx_backend_caps    caps;
    int32_t                w, h;
    int                    origin_top;
    float                  bg[3];
    float                  ppu;
    ygfx_format          scene_format;
    ygfx_dither          dither;
    uint32_t               seed;
    int32_t                lut_n;
    uint32_t               cal_crc;
    uint32_t               scene, lut;              /* backend texture ids       */
    uint32_t               builtin[YGFX__N_BUILTIN];
    uint32_t               vspec[YGFX__N_VSPEC];  /* 0 when not built          */
    uint32_t               output_pipe;
    uint32_t               ubo[YGFX__UBO_RING];
    int                    ubo_i;
    uint32_t               ubo_bytes;
    unsigned char*         staging;                 /* frame block, stim blocks  */
    ygfx__cmd*           cmds;
    int32_t                max_draws, n_cmds, n_blocks;
    int32_t                max_batch;               /* a test seam               */
    int32_t                no_reorder;              /* a test seam: call order   */
    unsigned char*         staging2;                /* the reorder's copies       */
    ygfx__cmd*           cmds2;
    int32_t*               ro_next;
    ygfx__rbatch*        ro_batch;
    yscr_frame           frame;
    ygfx__seg            segs[2 * YGFX_MAX_PASSES + 1];
    int32_t                n_segs, n_targets, in_target;
    uint32_t               cur_target_tex;          /* the pool id being drawn  */
    int32_t                cur_w, cur_h, cur_top;   /* the pass being recorded  */
    uint32_t               epoch;
    uint32_t               generation;              /* yscr_gl_generation at open */
    int64_t                t_open;
    uint32_t               clipped;                 /* this frame                */
    uint64_t               clipped_total, frames, draws;
    yrt_ring*            ring;
    /* from the calibration at open, for PAINT's spaces: Oklab's cone
     * response (cubed) to device RGB and back; DKL about the background */
    int32_t                has_oklab, has_dkl;
    double                 ok_lms2rgb[9], ok_rgb2lms[9], dkl2rgb[9], rgb2dkl[9];
    ygfx__res            tex[YGFX_MAX_TEXTURES];
    ygfx__res            buf[YGFX_MAX_BUFFERS];
    ygfx__res            pipe[YGFX_MAX_PIPELINES];
    /* v0.4 */
    const ygfx_cache*    cache;
    ygfx_programs        progs;
    int32_t                calibrated;
    uint32_t               features;                /* YGFX_FEAT_*             */
    uint32_t               video_pipe;              /* backend id; made at the first
                                                     * encoded texture           */
    uint32_t               eotf;                    /* the display's transfer: 256 x 3
                                                     * R32F, the inverse of the CLUT */
    uint32_t               gauss;                   /* NOISE GAUSSIAN's quantiles:
                                                     * 256 x 256 R32F (v0.7)      */
    int32_t                has_xyz;                 /* the calibration has xy     */
    /* v0.5: ysp/color.h's context from the calibration (COLOR) */
    int32_t                has_color;
    ycol_ctx             color;
    double                 ok_black[3], ok_off[3];  /* Oklab's black term, as the
                                                     * cone response and in rgb   */
    /* INSTANCES: the frame's element records and their buffers, made at the
     * first ygfx_instances(); the instanced programs */
    unsigned char*         inst_staging;
    int32_t                inst_cap, inst_used;
    uint32_t               inst_buf[YGFX__UBO_RING];
    uint32_t               inst_pipe[YGFX__N_BUILTIN + 1];   /* + the video program */
    uint32_t               pipe_inst[YGFX_MAX_PIPELINES];   /* users' */
    char*                  pipe_body[YGFX_MAX_PIPELINES];   /* kept for them */
    /* v0.6: curve sets and the text program, made at the first set */
    ygfx__cs             cset[YGFX_MAX_CSETS];
    uint32_t               text_pipe;
    uint32_t               text_rays_pipe;          /* without the exact area, at
                                                     * the first .rays run      */
    uint32_t               blur_pipe;               /* BLUR, at the first blur   */
    float                  ppu_keep;                /* px per unit outside a 2x layer */
    /* v0.11.1: ygfx_open_start() to ygfx_open() (OPEN START) */
    int32_t                started;                 /* YGFX__START_*; open is 0 */
    int32_t                start_mode;              /* what the start did, for describe */
    ygfx_desc            sdesc;                   /* the start's desc         */
    ygfx__prec           prec[YGFX__N_BUILTIN + 1], vprec[YGFX__N_VSPEC];
    struct ygfx__cap*      cap;                     /* v0.12: CAPTURE, from start */
    char                   error[512];
    uint64_t               backend_mem[YGFX__BACKEND_WORDS];
} ygfx_gfx;

/* --- API ------------------------------------------------------------------- */

YGFX_API const char* ygfx_version(void);
YGFX_API const char* ygfx_strerror(int code);

/* Opens on desc->screen (or the headless context of desc->gl_proc): loads GL,
 * compiles the built-in programs, makes the scene target, the CLUT and the
 * uniform buffers, and allocates the per-frame staging once (max_draws x 256
 * bytes). Nothing is allocated after it. False with ygfx_error() set:
 * no EXT_color_buffer_float, an output, stereo or scene format v0.1 does not
 * have, a calibration that fails ycol_cal_check() or is not sealed (seal it
 * with ycol_cal_derive() or ycol_cal_save() after the last change). On a
 * screen without GL (SIM) it opens the null backend: everything runs, nothing
 * is drawn. */
YGFX_API bool        ygfx_open(ygfx_gfx* g, const ygfx_desc* d);
/* The open's first half (OPEN START): the desc's checks, the buffers and
 * textures, and every built-in program submitted, without waiting for the
 * compiles. Call it from ysp/screen.h's desc.on_context; ygfx_open() on the
 * handle finishes it with the desc given here (it may get NULL). Without
 * parallel compile in the backend it submits nothing. False with
 * ygfx_error() set, as ygfx_open(); the handle is then closed. */
YGFX_API bool        ygfx_open_start(ygfx_gfx* g, const ygfx_desc* d);
YGFX_API void        ygfx_close(ygfx_gfx* g);
YGFX_API const char* ygfx_error(const ygfx_gfx* g);
YGFX_API bool        ygfx_is_open(const ygfx_gfx* g);
/* One line for the log: backend, renderer, size, scene format, CLUT, dither,
 * units. Returns snprintf's count. */
YGFX_API int         ygfx_describe(const ygfx_gfx* g, char* buf, size_t cap);
/* v0.4. The screen it opened on (NULL headless or closed); whether it opened
 * with a calibration; what desc.cache did. */
YGFX_API yscr_screen* ygfx_screen(const ygfx_gfx* g);
YGFX_API bool           ygfx_calibrated(const ygfx_gfx* g);
/* v0.5: the ysp/color.h context open() made from desc.cal, desc.background,
 * desc.cones and desc.lum (COLOR): colors, cone contrast and DKL directions
 * with the background and luminance PAINT uses. NULL without a calibration. */
YGFX_API const ycol_ctx* ygfx_color(const ygfx_gfx* g);
YGFX_API void           ygfx_program_stats(const ygfx_gfx* g, ygfx_programs* out);
/* What this gfx's renderer can do (YGFX_FEAT_*), after open. */
YGFX_API uint32_t       ygfx_features(const ygfx_gfx* g);
/* A program cache in the folder dir (UTF-8; made when missing), for
 * desc.cache: files named by key, written whole under a temporary name and
 * renamed. Nothing is ever deleted. NULL when dir is NULL, empty or too
 * long. fc must outlive the gfx. */
YGFX_API const ygfx_cache* ygfx_file_cache_init(ygfx_file_cache* fc, const char* dir);
/* v0.10.1: the per-user program cache folder (UTF-8, no trailing slash)
 * into out (PROGRAM CACHE): YGFX_OK, or YGFX_ERR_ARG with out empty
 * when there is none that only this user can write. Makes no folder. */
YGFX_API int ygfx_default_cache_dir(char* out, size_t cap);

/* Stimuli. Zero desc fields take defaults; the result has real values in
 * every field (visible 1, gate 1, contrast and opacity 1 unless set). */
YGFX_API ygfx_stim ygfx_shape(const ygfx_shape_desc* d);
YGFX_API ygfx_stim ygfx_grating(const ygfx_grating_desc* d);
YGFX_API ygfx_stim ygfx_gabor(const ygfx_gabor_desc* d);
YGFX_API ygfx_stim ygfx_dots(const ygfx_dots_desc* d);
YGFX_API ygfx_stim ygfx_image(const ygfx_gfx* g, const ygfx_image_desc* d);
YGFX_API ygfx_stim ygfx_noise(const ygfx_noise_desc* d);
YGFX_API ygfx_stim ygfx_user(const ygfx_user_desc* d);
/* v0.3. A compound: a SHAPE of kind YGFX_COMPOUND whose prims point at
 * the desc's array (keep it alive; bind its fields with ygfx_bind.field).
 * A glyph run: a SHAPE with MASK_TEX on the atlas and buf, count glyphs. */
YGFX_API ygfx_stim ygfx_compound(const ygfx_compound_desc* d);
YGFX_API ygfx_stim ygfx_glyphs(const ygfx_glyphs_desc* d);
/* v0.6, CURVE SETS. A set from its two arrays (setup: validates them,
 * uploads them, and makes the text program the first time). On failure id
 * 0 and ygfx_error() says why. */
YGFX_API ygfx_cset ygfx_cset_make(ygfx_gfx* g, const ygfx_cset_desc* d);
/* now: the arrays after the builder appended the glyphs named in glyphs[0 ..
 * n - 1] and set their table entries; uploads what is new. Between frames. */
YGFX_API int  ygfx_cset_add(ygfx_gfx* g, ygfx_cset s, const ygfx_cset_desc* now, const uint32_t* glyphs, int n);
YGFX_API void ygfx_cset_free(ygfx_gfx* g, ygfx_cset s);
/* v0.6, RUNS: a TEXT stimulus of a curve set's glyphs or paths. */
YGFX_API ygfx_stim ygfx_crun(ygfx_gfx* g, const ygfx_crun_desc* d);
/* v0.6, BLUR. ygfx_blur_make() at setup: the layer, a scratch target and
 * the result, and the blur program the first time; YGFX_OK or a code with
 * ygfx_error() saying why. ygfx_blur_apply() in a frame, outside a target
 * pass: two passes (of the 16) that blur the layer into the result by
 * b->sigma. ygfx_blur_free() frees the three targets. */
YGFX_API int  ygfx_blur_make(ygfx_gfx* g, ygfx_blur* b, const ygfx_blur_desc* d);
YGFX_API int  ygfx_blur_apply(ygfx_gfx* g, ygfx_blur* b);
YGFX_API void ygfx_blur_free(ygfx_gfx* g, ygfx_blur* b);
/* v0.4, INSTANCES. A copy of tmpl that draws d->n elements in one call. It
 * makes the instanced program now (setup; through desc.cache), so the first
 * draw compiles nothing. Keep d->inst and d->palette alive: the stimulus
 * points at them, and draw() reads them. On failure the copy has n_inst 0
 * and ygfx_error() says why. */
YGFX_API ygfx_stim ygfx_instances(ygfx_gfx* g, const ygfx_stim* tmpl, const ygfx_instances_desc* d);
/* x, y on an nx x ny grid of dx x dy units about 0, 0 (row by row from the
 * top-left); contrast, scale and gate 1, the rest 0, in every element. */
YGFX_API void ygfx_inst_grid(ygfx_inst* a, int nx, int ny, float dx, float dy);
/* The topmost element (the highest index) whose hard shape holds the
 * screen pixel, by the same field as the GPU's coverage >= 0.5; -1 for
 * none. Elements with gate 0 are skipped. */
YGFX_API int  ygfx_hit_index(const ygfx_gfx* g, const ygfx_stim* s, float px, float py);
/* Element i's anchor on the screen. */
YGFX_API void ygfx_inst_resolve(const ygfx_gfx* g, const ygfx_stim* s, int i, float* x, float* y);
/* The length in px of what dashes and trim run along: the boundary, or the
 * center line of a LINE, CAPSULE, ARC, QBEZIER or POLYLINE. Negative: a
 * code (no length: a CROSS, a mask, a compound, a stimulus that is not a
 * shape). */
YGFX_API double ygfx_length(const ygfx_gfx* g, const ygfx_stim* s);

/* The frame. begin() takes the frame yscr_begin() filled (or one you fill,
 * headless) and records nothing on the GPU; draw() packs the stimulus and
 * queues it; end() uploads the frame's uniforms once, draws the queue into the
 * scene target in order, runs the output stage into framebuffer 0, and
 * returns. Nothing waits for the GPU. Call yscr_flip() after end(). */
YGFX_API int  ygfx_begin(ygfx_gfx* g, const yscr_frame* f);
YGFX_API int  ygfx_draw(ygfx_gfx* g, const ygfx_stim* s);
YGFX_API int  ygfx_draw_n(ygfx_gfx* g, const ygfx_stim* s, int n);
YGFX_API int  ygfx_end(ygfx_gfx* g);
/* v0.8, SETUP PASSES: target passes outside a screen frame (static text,
 * artwork, a blur layer), at setup or between frames. Between the two,
 * begin_target/end_target, draws into the target and ygfx_blur_apply();
 * a draw into the scene is refused. end_setup() runs the passes and touches
 * neither the scene, the output stage nor the frame count. */
YGFX_API int  ygfx_begin_setup(ygfx_gfx* g);
YGFX_API int  ygfx_end_setup(ygfx_gfx* g);
/* v0.11, PRIME: draws once with every program made so far (the built-in
 * kinds, the specialized vector programs, the instanced, video, text and
 * blur programs made so far, the caller's pipelines, the output stage) as
 * zero-size quads into the scene, then waits for the GPU. Nothing is
 * shown. ANGLE makes a program's D3D shader objects at its first draw:
 * on the Iris Xe each kind's first frame cost 2.5 to 12.9 ms more CPU in
 * ygfx_end(), from a warm program cache too; after a prime, 0.09 to 0.42
 * ms. The prime of 20 programs took 63 to 73 ms. Call it outside a frame,
 * after making the stimuli and before the first trial; again after making
 * a new kind. Returns the programs drawn, or < 0. */
YGFX_API int  ygfx_prime(ygfx_gfx* g);
/* After your own GL calls between frames: forget the cached GL state. */
YGFX_API void ygfx_reset_state(ygfx_gfx* g);
/* Draws that may have left 0..1 since open (a CPU bound per draw). */
YGFX_API uint64_t ygfx_clipped(const ygfx_gfx* g);

/* Groups (GROUPS). Zero desc fields take defaults (scale, opacity 1; anchor
 * the center); the parameter table of YGFX_G_* fields. */
YGFX_API ygfx_group ygfx_group_make(const ygfx_group_desc* d);
YGFX_API const ygfx_param* ygfx_group_params(int* n);

/* Render targets (TARGETS). ygfx_target() at setup only. begin_target()
 * and end_target() inside begin..end, not nested; clear NULL keeps what the
 * target holds. read_target() reads RGBA float, rows top first. */
YGFX_API ygfx_tex ygfx_target(ygfx_gfx* g, const ygfx_target_desc* d);
YGFX_API int ygfx_begin_target(ygfx_gfx* g, ygfx_tex t, const float* clear);
YGFX_API int ygfx_end_target(ygfx_gfx* g);
YGFX_API int ygfx_read_target(ygfx_gfx* g, ygfx_tex t, int x, int y, int w, int h, float* rgba);

/* A signed-distance texture for YGFX_MASK_TEX from a mask (texels >= 128
 * inside): out is (w + 2 pad) x (h + 2 pad) floats, rows top first, the
 * distance in texels to the boundary halfway between inside and outside
 * texel centers, negative inside. An exact Euclidean distance transform
 * (Felzenszwalb and Huttenlocher 2012). At setup: allocates its scratch. */
YGFX_API int ygfx_sdf_from_mask(float* out, const uint8_t* mask, int w, int h, int pad);

/* Resources. Outside begin..end only (YGFX_ERR_ORDER otherwise): a queued
 * draw is submitted at end() and must see the data it was queued with. */
YGFX_API ygfx_tex ygfx_texture(ygfx_gfx* g, const ygfx_texture_desc* d);
YGFX_API int        ygfx_texture_update(ygfx_gfx* g, ygfx_tex t, int x, int y, int w, int h,
                                            const void* data, size_t stride);
YGFX_API void       ygfx_texture_free(ygfx_gfx* g, ygfx_tex t);
/* v0.4, VIDEO. Every plane of an NV12 or I420 texture (two or three uploads),
 * or one plane's rectangle in that plane's texels (0 Y, 1 CbCr or Cb, 2 Cr).
 * Between frames. */
YGFX_API int        ygfx_texture_update_planes(ygfx_gfx* g, ygfx_tex t, const ygfx_planes* p);
YGFX_API int        ygfx_texture_update_plane(ygfx_gfx* g, ygfx_tex t, int plane, int x, int y, int w, int h,
                                                  const void* data, size_t stride);
/* Wraps a texture ysp_gfx does not own; ygfx_texture_free() releases what
 * the import made (a GL texture and an EGLImage per plane for D3D11), never
 * the source. Setup only. YGFX_ERR_NOT_IMPLEMENTED without the feature. */
YGFX_API ygfx_tex ygfx_texture_import(ygfx_gfx* g, const ygfx_import_desc* d);
/* t shows src's texels from now on (the same format, size and encoding);
 * src = t gives t its own back. No GL call, nothing allocated: per frame,
 * between frames. Freeing src gives every texture that shows it its own. */
YGFX_API int        ygfx_texture_rebind(ygfx_gfx* g, ygfx_tex t, ygfx_tex src);
YGFX_API ygfx_buf ygfx_buffer(ygfx_gfx* g, size_t bytes);
YGFX_API int        ygfx_buffer_update(ygfx_gfx* g, ygfx_buf b, size_t off, const void* data, size_t n);
YGFX_API void       ygfx_buffer_free(ygfx_gfx* g, ygfx_buf b);
/* A user shader: the body wrapped in the contract (SHADER CONTRACT), compiled
 * now. On failure id 0 and ygfx_error() holds the compiler's log, whose
 * line numbers are the body's. */
YGFX_API ygfx_pipe ygfx_pipeline(ygfx_gfx* g, const ygfx_pipeline_desc* d);
YGFX_API void        ygfx_pipeline_free(ygfx_gfx* g, ygfx_pipe p);

/* The fragment shader ygfx_pipeline() compiles for a body, without GL: the
 * pack tool validates the same text. Returns the length, or a negative code
 * when cap is too small (the length needed is then -return - 1000). */
YGFX_API int ygfx_shader_wrap(const char* body, ygfx_shader_mode mode, char* out, size_t cap);
/* The SHADER CONTRACT's version (v0.10.3); 2 from v0.10.5 (stim.tex is the
 * body's ysp_tex0, v0.10.4). */
#define YGFX_SHADER_CONTRACT 2

/* The output stage's table: lut holds n values per channel, red first, linear
 * to device value 0..1, 2 <= n <= 4096. Between frames. */
YGFX_API int ygfx_set_lut(ygfx_gfx* g, const float* lut, int n);

/* Read back for tests and checks; blocks on the GPU, never in a trial. x, y
 * from the top-left, rows top first. scene: RGBA float in linear device RGB;
 * output: RGBA8 codes of framebuffer 0 after end(). */
YGFX_API int ygfx_read_scene(ygfx_gfx* g, int x, int y, int w, int h, float* rgba);
YGFX_API int ygfx_read_output(ygfx_gfx* g, int x, int y, int w, int h, uint8_t* rgba);

/* v0.12, CAPTURE: the output of chosen frames, copied without a stall and
 * written off the frame thread. */
#define YGFX_IMAGE_PPM 1         /* binary PPM (P6), 8-bit RGB                   */
#define YGFX_IMAGE_PNG 2         /* PNG, 8-bit RGB, stored (not compressed)      */

/* One captured frame, as on_frame gets it on the writer thread. rgba is the
 * top row of the rectangle; row r is at rgba + r * stride (stride < 0 when
 * the rows lie bottom first in memory). Valid until on_frame returns. */
typedef struct ygfx_capture_frame {
    int64_t        index;          /* yscr_frame.index of the frame            */
    int64_t        predicted;      /* yscr_frame.onset: the predicted onset    */
    int32_t        has_record;     /* 0: no flip record came (see CAPTURE)     */
    int32_t        x, y, w, h;     /* screen px from the top-left              */
    int32_t        reserved_;
    ptrdiff_t      stride;         /* bytes from one row to the one below      */
    const uint8_t* rgba;
    const char*    path;           /* the file written, or NULL                */
    int32_t        file_rc;        /* YGFX_OK, or why the file failed          */
    int32_t        reserved2_;
    yscr_record    record;         /* the frame's flip record, when has_record */
} ygfx_capture_frame;

typedef void (*ygfx_capture_fn)(void* ctx, const ygfx_capture_frame* fr);

/* Zero fields take defaults. */
typedef struct ygfx_capture_desc {
    int32_t         every;         /* frames whose index % every == 0; 0 = only
                                    * the frames ygfx_capture_mark() marks     */
    int32_t         x, y, w, h;    /* the rectangle, screen px from the top-left;
                                    * w or h 0 = the whole output             */
    int32_t         slots;         /* pixel buffers: frames being copied, waiting
                                    * for their flip record or being written;
                                    * 0 = 6, at most YGFX_CAPTURE_MAX_SLOTS   */
    const char*     path;          /* files <path><index, 8 digits>.ppm (or .png);
                                    * NULL = no files (on_frame only). Its folder
                                    * is made when missing. UTF-8.            */
    int32_t         format;        /* YGFX_IMAGE_PPM (0 = it) or _PNG         */
    bool            manual;        /* no writer thread, and no map in a frame:
                                    * ygfx_capture_pump() maps and writes.
                                    * Always so with YRT_NO_THREADS          */
    ygfx_capture_fn on_frame;      /* after the file, on the writer; may be NULL */
    void*           ctx;
} ygfx_capture_desc;

typedef struct ygfx_capture_stats {
    uint64_t chosen;               /* frames chosen (every Nth, marked)         */
    uint64_t dropped;              /* chosen while every slot was busy: not read */
    uint64_t read;                 /* copies started                            */
    uint64_t written;              /* frames finished (file and on_frame)       */
    uint64_t failed;               /* copies or files that failed               */
    uint64_t no_record;            /* finished without a flip record            */
    int32_t  busy, busy_max;       /* slots in use now, and the most at once    */
    int32_t  slots;
    int32_t  manual;               /* 1: no writer thread                       */
} ygfx_capture_stats;

#ifndef YGFX_CAPTURE_MAX_SLOTS
#define YGFX_CAPTURE_MAX_SLOTS 32
#endif

/* Setup, outside a frame: makes the pixel buffers (w x h x 4 bytes each) and
 * starts the writer. YGFX_ERR_NOT_IMPLEMENTED when the backend cannot read
 * back without a stall; YGFX_ERR_ORDER when capture is on already. */
YGFX_API int  ygfx_capture_start(ygfx_gfx* g, const ygfx_capture_desc* d);
/* Between begin() and end(): capture this frame too. */
YGFX_API int  ygfx_capture_mark(ygfx_gfx* g);
/* Outside a frame, on the frame thread: collects finished copies and, in
 * manual mode, writes up to max of them here (max <= 0: all that are ready).
 * Returns the frames written by this call, or < 0. */
YGFX_API int  ygfx_capture_pump(ygfx_gfx* g, int max);
/* Waits for every copy, writes every frame, stops the writer and frees the
 * buffers. Frames whose flip record has not come are written without one.
 * *out (may be NULL) gets the final counts. Returns the frames written since
 * start, or < 0. ygfx_close() calls it. */
YGFX_API int  ygfx_capture_stop(ygfx_gfx* g, ygfx_capture_stats* out);
YGFX_API void ygfx_capture_get_stats(const ygfx_gfx* g, ygfx_capture_stats* out);

/* Writes 8-bit RGBA rows as an RGB image file (alpha is dropped): rgba is
 * the top row, row r at rgba + r * stride. comment (may be NULL) goes in the
 * file: a PPM comment line, a PNG tEXt chunk with the keyword "Comment".
 * No allocation. YGFX_OK, YGFX_ERR_ARG, or YGFX_ERR_IO. */
YGFX_API int ygfx_image_write(const char* path, int format, const uint8_t* rgba, int w, int h,
                              ptrdiff_t stride, const char* comment);

/* Screen pixels: from the top-left corner, x right, y down, pixel centers at
 * + 0.5; the space of ygfx_read_output()'s rows and of SDL's mouse events
 * at pixel density 1 (COORDINATES). The screen's center and size; a
 * stimulus's anchor point; the axis-aligned bounds of its turned box; the
 * point of its own frame (pixels from its box center, along its axes) under
 * a screen pixel; and whether a screen pixel is inside it, where the GPU's
 * coverage is 0.5 or more (SHAPE, and the aperture of GRATING, GABOR, NOISE,
 * IMAGE and USER; false for the rest). */
YGFX_API void ygfx_center(const ygfx_gfx* g, float* x, float* y);
YGFX_API void ygfx_size(const ygfx_gfx* g, float* w, float* h);
YGFX_API void ygfx_resolve(const ygfx_gfx* g, const ygfx_stim* s, float* x, float* y);
YGFX_API void ygfx_bounds(const ygfx_gfx* g, const ygfx_stim* s, float* x0, float* y0, float* x1, float* y1);
YGFX_API void ygfx_local(const ygfx_gfx* g, const ygfx_stim* s, float px, float py, float* lx, float* ly);
YGFX_API bool ygfx_hit(const ygfx_gfx* g, const ygfx_stim* s, float px, float py);

/* Timeline binding: values[b[i].channel] goes to field b[i].param of
 * *b[i].stim, for each i. A loop of stores. */
YGFX_API int ygfx_apply(const ygfx_bind* b, int n, const float* values);

/* The parameter table; *n gets YGFX_P_COUNT. */
YGFX_API const ygfx_param* ygfx_params(int* n);
/* v0.3: the float fields of ygfx_prim, ygfx_fx and ygfx_paint, for a
 * designer; offset is within that struct, kinds is 0. Bind them with
 * ygfx_bind.field. */
YGFX_API const ygfx_param* ygfx_prim_params(int* n);
YGFX_API const ygfx_param* ygfx_fx_params(int* n);
YGFX_API const ygfx_param* ygfx_paint_params(int* n);
/* The desc fields a designer sets (yscr_param's shape). */
YGFX_API const yscr_param* ygfx_desc_params(int* n);

/* Noise, bit for bit what the NOISE stimulus draws for check (cx, cy) from
 * its top-left corner. ygfx_noise_fill() writes w x h checks, rows top
 * first. GAUSSIAN reads a 65536-entry table built at the first ygfx_open()
 * or GAUSSIAN call (0.3 ms; call one before using it from threads).
 * ygfx_noise_gauss() is CPU only and not what NOISE draws: N(0, 1) by
 * Box-Muller from the same hash; reproducible for one compiler and C
 * library. */
YGFX_API float ygfx_noise_value(int32_t cx, int32_t cy, uint32_t seed, ygfx_noise_dist dist);
YGFX_API void  ygfx_noise_fill(float* out, int w, int h, uint32_t seed, ygfx_noise_dist dist);
YGFX_API void  ygfx_noise_gauss(float* out, int w, int h, uint32_t seed);
YGFX_API uint32_t ygfx_hash(uint32_t v);   /* triple32 (v0.9; pcg_hash before) */
/* NOISE's and the dither's key of point (cx, cy) under seed: the shaders'
 * ysp_hash2(), bit for bit (v0.9). */
YGFX_API uint32_t ygfx_hash2(int32_t cx, int32_t cy, uint32_t seed);
/* NOISE SIMPLEX at pixel (ix, iy) of the stimulus's box, from its top-left
 * corner: what the GPU draws there, bit for bit, when the box is not turned
 * and sits on whole px (before contrast and aperture; v0.10). */
YGFX_API float ygfx_simplex_value(const ygfx_gfx* g, const ygfx_stim* s, int32_t ix, int32_t iy);

/* v0.6, CURVE SETS, CPU only (no gfx): the format's check (YGFX_OK, or
 * YGFX_ERR_FORMAT with msg naming the glyph and the word), and the winding
 * number of glyph `glyph` at the point x, y in set units, by the band rays the
 * shader casts, in double: +1 inside a contour that turns clockwise on the
 * screen. For a set that passed the check; it reads nothing outside the
 * arrays whatever they hold, and gives 0 for an absent glyph. */
YGFX_API int ygfx_cset_check(const ygfx_cset_desc* d, char* msg, size_t cap);
YGFX_API int ygfx_cset_winding(const ygfx_cset_desc* d, uint32_t glyph, double x, double y);


#ifdef __cplusplus
}
#endif

#endif /* YSP_GFX_H_INCLUDED */

/* ======================================================================= *
 *                            IMPLEMENTATION                               *
 * ======================================================================= */
#ifdef YSP_GFX_IMPLEMENTATION
#ifndef YSP_GFX_IMPLEMENTATION_GUARD
#define YSP_GFX_IMPLEMENTATION_GUARD

#ifndef YSP_SCREEN_IMPLEMENTATION_GUARD
    #define YSP_SCREEN_IMPLEMENTATION
    #include "ysp/screen.h"
#endif
#ifndef YSP_COLOR_IMPLEMENTATION_GUARD
    #define YSP_COLOR_IMPLEMENTATION
    #include "ysp/color.h"
#endif

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <math.h>
#if !defined(_WIN32)
    #include <sys/stat.h>   /* mkdir: the file cache */
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* --- small helpers ------------------------------------------------------- */

static void ygfx__set_error(char* buf, size_t cap, const char* fmt, ...) {
    va_list ap;
    if (!buf || cap == 0) return;
    va_start(ap, fmt);
    vsnprintf(buf, cap, fmt, ap);
    va_end(ap);
}


/* v0.3: the vector program's packing and CPU form, further down */
static int ygfx__is_vector(const ygfx_stim* s);
static const float* ygfx__gauss_table(void);   /* NOISE GAUSSIAN, further down */
/* NOISE SIMPLEX's per-octave integers (further down) */
typedef struct ygfx__gn { uint32_t r[8], z[8]; int32_t w[8]; int n; } ygfx__gn;
static const char* ygfx__gn_setup(const ygfx_gfx* g, const ygfx_stim* s, ygfx__gn* out);
static uint32_t ygfx__vspec_pick(const ygfx_gfx* g, const float* b, uint32_t pipe);
static int ygfx__vpack(const ygfx_gfx* g, const ygfx_stim* s, float* b, double ext[2], int* clipped, const char** err);
static int ygfx__vhard(const float* b, double px, double py);
static double ygfx__edge_reach(const ygfx_stim* s);
/* v0.6: a run's packing, with the curve sets further down */
struct ygfx__cmd;
static uint32_t ygfx__text_pack(ygfx_gfx* g, const ygfx_stim* s, float* b, struct ygfx__cmd* cmd, int* why);

YGFX_API const char* ygfx_version(void) { return YGFX_VERSION_STRING; }

YGFX_API const char* ygfx_strerror(int code) {
    switch (code) {
    case YGFX_OK:                  return "ok";
    case YGFX_ERR_ARG:             return "bad argument";
    case YGFX_ERR_CLOSED:          return "gfx not open";
    case YGFX_ERR_ORDER:           return "begin, draw, end or an update out of turn";
    case YGFX_ERR_FULL:            return "pool or frame full";
    case YGFX_ERR_GL:              return "the backend failed";
    case YGFX_ERR_NOT_IMPLEMENTED: return "not implemented";
    case YGFX_ERR_REFUSED:         return "refused";
    case YGFX_ERR_FORMAT:          return "not the canonical form";
    case YGFX_ERR_RANGE:           return "out of range";
    case YGFX_ERR_LOST:            return "the GL context is new";
    case YGFX_ERR_IO:              return "a file could not be written";
    default:                         return code > 0 ? "ok" : "unknown error";
    }
}

/* Bytes per texel of the canonical in-memory form. */
static int ygfx__texel_bytes(ygfx_format f) {
    switch (f) {
    case YGFX_R8:      return 1;
    case YGFX_RG8:     return 2;
    case YGFX_RGBA8:   return 4;
    case YGFX_R16F:    return 4;     /* float in, half stored */
    case YGFX_RGBA16F: return 16;
    case YGFX_R32F:    return 4;
    case YGFX_RGBA32F: return 16;
    case YGFX_R16UI:   return 2;
    case YGFX_NV12:    return 1;     /* the Y plane */
    case YGFX_I420:    return 1;
    case YGFX_RGBA32UI: return 16;
    default:             return 0;
    }
}

/* --- GL ES 3.0 backend ----------------------------------------------------- */

#if defined(_WIN32)
    #define YGFX__APIENTRY __stdcall
#else
    #define YGFX__APIENTRY
#endif

typedef unsigned int  ygfx__GLenum;
typedef unsigned int  ygfx__GLuint;
typedef int           ygfx__GLint;
typedef int           ygfx__GLsizei;
typedef unsigned char ygfx__GLboolean;
typedef ptrdiff_t     ygfx__GLintptr;
typedef ptrdiff_t     ygfx__GLsizeiptr;
typedef char          ygfx__GLchar;

/* The entry points the backend uses, loaded by name at open. */
#define YGFX__GL_FUNCS(X) \
    X(const unsigned char*, GetString, (ygfx__GLenum)) \
    X(const unsigned char*, GetStringi, (ygfx__GLenum, ygfx__GLuint)) \
    X(void, GetIntegerv, (ygfx__GLenum, ygfx__GLint*)) \
    X(ygfx__GLenum, GetError, (void)) \
    X(void, Enable, (ygfx__GLenum)) \
    X(void, Disable, (ygfx__GLenum)) \
    X(void, BlendFuncSeparate, (ygfx__GLenum, ygfx__GLenum, ygfx__GLenum, ygfx__GLenum)) \
    X(void, BlendEquation, (ygfx__GLenum)) \
    X(void, Viewport, (ygfx__GLint, ygfx__GLint, ygfx__GLsizei, ygfx__GLsizei)) \
    X(void, ClearColor, (float, float, float, float)) \
    X(void, Clear, (unsigned int)) \
    X(void, ColorMask, (ygfx__GLboolean, ygfx__GLboolean, ygfx__GLboolean, ygfx__GLboolean)) \
    X(ygfx__GLuint, CreateShader, (ygfx__GLenum)) \
    X(void, ShaderSource, (ygfx__GLuint, ygfx__GLsizei, const ygfx__GLchar* const*, const ygfx__GLint*)) \
    X(void, CompileShader, (ygfx__GLuint)) \
    X(void, GetShaderiv, (ygfx__GLuint, ygfx__GLenum, ygfx__GLint*)) \
    X(void, GetShaderInfoLog, (ygfx__GLuint, ygfx__GLsizei, ygfx__GLsizei*, ygfx__GLchar*)) \
    X(void, DeleteShader, (ygfx__GLuint)) \
    X(ygfx__GLuint, CreateProgram, (void)) \
    X(void, AttachShader, (ygfx__GLuint, ygfx__GLuint)) \
    X(void, LinkProgram, (ygfx__GLuint)) \
    X(void, GetProgramiv, (ygfx__GLuint, ygfx__GLenum, ygfx__GLint*)) \
    X(void, GetProgramInfoLog, (ygfx__GLuint, ygfx__GLsizei, ygfx__GLsizei*, ygfx__GLchar*)) \
    X(void, DeleteProgram, (ygfx__GLuint)) \
    X(void, UseProgram, (ygfx__GLuint)) \
    X(ygfx__GLuint, GetUniformBlockIndex, (ygfx__GLuint, const ygfx__GLchar*)) \
    X(void, UniformBlockBinding, (ygfx__GLuint, ygfx__GLuint, ygfx__GLuint)) \
    X(ygfx__GLint, GetUniformLocation, (ygfx__GLuint, const ygfx__GLchar*)) \
    X(void, Uniform1i, (ygfx__GLint, ygfx__GLint)) \
    X(void, GenTextures, (ygfx__GLsizei, ygfx__GLuint*)) \
    X(void, DeleteTextures, (ygfx__GLsizei, const ygfx__GLuint*)) \
    X(void, BindTexture, (ygfx__GLenum, ygfx__GLuint)) \
    X(void, ActiveTexture, (ygfx__GLenum)) \
    X(void, TexStorage2D, (ygfx__GLenum, ygfx__GLsizei, ygfx__GLenum, ygfx__GLsizei, ygfx__GLsizei)) \
    X(void, TexSubImage2D, (ygfx__GLenum, ygfx__GLint, ygfx__GLint, ygfx__GLint, ygfx__GLsizei, ygfx__GLsizei, ygfx__GLenum, ygfx__GLenum, const void*)) \
    X(void, TexParameteri, (ygfx__GLenum, ygfx__GLenum, ygfx__GLint)) \
    X(void, PixelStorei, (ygfx__GLenum, ygfx__GLint)) \
    X(void, GenFramebuffers, (ygfx__GLsizei, ygfx__GLuint*)) \
    X(void, DeleteFramebuffers, (ygfx__GLsizei, const ygfx__GLuint*)) \
    X(void, BindFramebuffer, (ygfx__GLenum, ygfx__GLuint)) \
    X(void, FramebufferTexture2D, (ygfx__GLenum, ygfx__GLenum, ygfx__GLenum, ygfx__GLuint, ygfx__GLint)) \
    X(ygfx__GLenum, CheckFramebufferStatus, (ygfx__GLenum)) \
    X(void, ReadPixels, (ygfx__GLint, ygfx__GLint, ygfx__GLsizei, ygfx__GLsizei, ygfx__GLenum, ygfx__GLenum, void*)) \
    X(void, GenBuffers, (ygfx__GLsizei, ygfx__GLuint*)) \
    X(void, DeleteBuffers, (ygfx__GLsizei, const ygfx__GLuint*)) \
    X(void, BindBuffer, (ygfx__GLenum, ygfx__GLuint)) \
    X(void, BufferData, (ygfx__GLenum, ygfx__GLsizeiptr, const void*, ygfx__GLenum)) \
    X(void, BufferSubData, (ygfx__GLenum, ygfx__GLintptr, ygfx__GLsizeiptr, const void*)) \
    X(void, BindBufferRange, (ygfx__GLenum, ygfx__GLuint, ygfx__GLuint, ygfx__GLintptr, ygfx__GLsizeiptr)) \
    X(void, GenVertexArrays, (ygfx__GLsizei, ygfx__GLuint*)) \
    X(void, DeleteVertexArrays, (ygfx__GLsizei, const ygfx__GLuint*)) \
    X(void, BindVertexArray, (ygfx__GLuint)) \
    X(void, EnableVertexAttribArray, (ygfx__GLuint)) \
    X(void, VertexAttribPointer, (ygfx__GLuint, ygfx__GLint, ygfx__GLenum, ygfx__GLboolean, ygfx__GLsizei, const void*)) \
    X(void, VertexAttribDivisor, (ygfx__GLuint, ygfx__GLuint)) \
    X(void, DrawArraysInstanced, (ygfx__GLenum, ygfx__GLint, ygfx__GLsizei, ygfx__GLsizei)) \
    X(void, Flush, (void)) \
    X(void, Finish, (void))

#define YGFX__GL_FIELD(ret, name, args) ret (YGFX__APIENTRY *name) args;
typedef struct ygfx__glf { YGFX__GL_FUNCS(YGFX__GL_FIELD) } ygfx__glf;
#undef YGFX__GL_FIELD

#define YGFX__GL_NO_ERROR             0
#define YGFX__GL_EXTENSIONS           0x1F03
#define YGFX__GL_RENDERER             0x1F01
#define YGFX__GL_VERSION              0x1F02
#define YGFX__GL_VENDOR               0x1F00
#define YGFX__GL_SHADING_LANGUAGE_VERSION 0x8B8C
#define YGFX__GL_NUM_PROGRAM_BINARY_FORMATS 0x87FE
#define YGFX__GL_PROGRAM_BINARY_FORMATS 0x87FF
#define YGFX__GL_PROGRAM_BINARY_LENGTH 0x8741
#define YGFX__GL_PROGRAM_BINARY_RETRIEVABLE_HINT 0x8257
#define YGFX__GL_NUM_EXTENSIONS       0x821D
#define YGFX__GL_UBO_ALIGN            0x8A34
#define YGFX__GL_MAX_UBO_SIZE         0x8A30
#define YGFX__GL_BLEND                0x0BE2
#define YGFX__GL_SCISSOR_TEST         0x0C11
#define YGFX__GL_DEPTH_TEST           0x0B71
#define YGFX__GL_CULL_FACE            0x0B44
#define YGFX__GL_STENCIL_TEST         0x0B90
#define YGFX__GL_DITHER               0x0BD0
#define YGFX__GL_RASTERIZER_DISCARD   0x8C89
#define YGFX__GL_ZERO                 0
#define YGFX__GL_ONE                  1
#define YGFX__GL_ONE_MINUS_SRC_ALPHA  0x0303
#define YGFX__GL_DST_COLOR            0x0306
#define YGFX__GL_FUNC_ADD             0x8006
#define YGFX__GL_COLOR_BUFFER_BIT     0x4000u
#define YGFX__GL_VERTEX_SHADER        0x8B31
#define YGFX__GL_FRAGMENT_SHADER      0x8B30
#define YGFX__GL_COMPILE_STATUS       0x8B81
#define YGFX__GL_LINK_STATUS          0x8B82
#define YGFX__GL_INFO_LOG_LENGTH      0x8B84
#define YGFX__GL_INVALID_INDEX        0xFFFFFFFFu
#define YGFX__GL_TEXTURE_2D           0x0DE1
#define YGFX__GL_TEXTURE0             0x84C0
#define YGFX__GL_TEXTURE_MIN_FILTER   0x2801
#define YGFX__GL_TEXTURE_MAG_FILTER   0x2800
#define YGFX__GL_TEXTURE_WRAP_S       0x2802
#define YGFX__GL_TEXTURE_WRAP_T       0x2803
#define YGFX__GL_NEAREST              0x2600
#define YGFX__GL_LINEAR               0x2601
#define YGFX__GL_CLAMP_TO_EDGE        0x812F
#define YGFX__GL_UNPACK_ALIGNMENT     0x0CF5
#define YGFX__GL_UNPACK_ROW_LENGTH    0x0CF2
#define YGFX__GL_PACK_ALIGNMENT       0x0D05
#define YGFX__GL_FRAMEBUFFER          0x8D40
#define YGFX__GL_READ_FRAMEBUFFER     0x8CA8
#define YGFX__GL_COLOR_ATTACHMENT0    0x8CE0
#define YGFX__GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define YGFX__GL_UNSIGNED_BYTE        0x1401
#define YGFX__GL_UNSIGNED_SHORT       0x1403
#define YGFX__GL_FLOAT                0x1406
#define YGFX__GL_RED                  0x1903
#define YGFX__GL_RG                   0x8227
#define YGFX__GL_RGBA                 0x1908
#define YGFX__GL_RED_INTEGER          0x8D94
#define YGFX__GL_R8                   0x8229
#define YGFX__GL_RG8                  0x822B
#define YGFX__GL_RGBA8                0x8058
#define YGFX__GL_R16F                 0x822D
#define YGFX__GL_RGBA16F              0x881A
#define YGFX__GL_R32F                 0x822E
#define YGFX__GL_RGBA32F              0x8814
#define YGFX__GL_R16UI                0x8234
#define YGFX__GL_RGBA32UI             0x8D70
#define YGFX__GL_RGBA_INTEGER         0x8D99
#define YGFX__GL_UNSIGNED_INT         0x1405
#define YGFX__GL_MAX_TEXTURE_SIZE     0x0D33
#define YGFX__GL_ARRAY_BUFFER         0x8892
#define YGFX__GL_UNIFORM_BUFFER       0x8A11
#define YGFX__GL_DYNAMIC_DRAW         0x88E8
#define YGFX__GL_TRIANGLE_STRIP       0x0005
#define YGFX__GL_TRIANGLES            0x0004

typedef struct ygfx__gltex {
    ygfx__GLuint tex, fbo;
    int32_t w, h, format;
    int32_t borrowed;              /* an imported GL name: not ours to delete     */
    void*   image;                 /* an EGLImage over a D3D11 texture            */
} ygfx__gltex;
typedef struct ygfx__glbuf { ygfx__GLuint buf; ygfx__GLenum target; uint32_t bytes; } ygfx__glbuf;
typedef struct ygfx__glpipe { ygfx__GLuint prog, vao, vs, fs; int32_t blend, inst, pending; } ygfx__glpipe;

#define YGFX__GL_TEX  (3 * YGFX_MAX_TEXTURES + 9)   /* three planes a texture */
#define YGFX__GL_BUF  (YGFX_MAX_BUFFERS + YGFX__UBO_RING + 1)
#define YGFX__GL_PIPE (YGFX_MAX_PIPELINES + 2 * YGFX__N_BUILTIN + YGFX__N_VSPEC + 8)

/* Program binaries: ES 3.0 has them, WebGL 2 does not, so they are loaded
 * apart from the required table and may be NULL. */
typedef void (YGFX__APIENTRY *ygfx__gl_getbin_fn)(ygfx__GLuint, ygfx__GLsizei, ygfx__GLsizei*, ygfx__GLenum*, void*);
typedef void (YGFX__APIENTRY *ygfx__gl_bin_fn)(ygfx__GLuint, ygfx__GLenum, const void*, ygfx__GLsizei);
typedef void (YGFX__APIENTRY *ygfx__gl_param_fn)(ygfx__GLuint, ygfx__GLenum, ygfx__GLint);
/* Import of D3D11 textures through EGL images (ANGLE), also optional. */
typedef void* (YGFX__APIENTRY *ygfx__egl_mkimg_fn)(void*, void*, unsigned, void*, const int32_t*);
typedef unsigned (YGFX__APIENTRY *ygfx__egl_rmimg_fn)(void*, void*);
typedef void* (YGFX__APIENTRY *ygfx__egl_dpy_fn)(void);
typedef const char* (YGFX__APIENTRY *ygfx__egl_qs_fn)(void*, int32_t);
typedef void (YGFX__APIENTRY *ygfx__gl_imgtgt_fn)(ygfx__GLenum, void*);
/* CAPTURE: fences and mapped pack buffers, ES 3.0 but not WebGL 2, also
 * optional. */
typedef void* (YGFX__APIENTRY *ygfx__gl_fence_fn)(ygfx__GLenum, unsigned int);
typedef ygfx__GLenum (YGFX__APIENTRY *ygfx__gl_cwait_fn)(void*, unsigned int, uint64_t);
typedef void (YGFX__APIENTRY *ygfx__gl_dsync_fn)(void*);
typedef void* (YGFX__APIENTRY *ygfx__gl_map_fn)(ygfx__GLenum, ygfx__GLintptr, ygfx__GLsizeiptr, unsigned int);
typedef ygfx__GLboolean (YGFX__APIENTRY *ygfx__gl_unmap_fn)(ygfx__GLenum);
#define YGFX__GL_PIXEL_PACK_BUFFER    0x88EB
#define YGFX__GL_STREAM_READ          0x88E1
#define YGFX__GL_SYNC_GPU_COMMANDS_COMPLETE 0x9117
#define YGFX__GL_TIMEOUT_EXPIRED      0x911B
#define YGFX__GL_WAIT_FAILED          0x911D
#define YGFX__GL_SYNC_FLUSH_COMMANDS_BIT 0x00000001u
#define YGFX__GL_MAP_READ_BIT         0x0001u

typedef struct ygfx__gl {
    ygfx__glf    f;
    ygfx__gl_getbin_fn GetProgramBinary;
    ygfx__gl_bin_fn    ProgramBinary;
    ygfx__gl_param_fn  ProgramParameteri;
    ygfx__egl_mkimg_fn  CreateImage;
    ygfx__egl_rmimg_fn  DestroyImage;
    ygfx__gl_imgtgt_fn  ImageTarget;
    void*                 dpy;     /* the EGL display of the context            */
    ygfx__gl_fence_fn   FenceSync;
    ygfx__gl_cwait_fn   ClientWaitSync;
    ygfx__gl_dsync_fn   DeleteSync;
    ygfx__gl_map_fn     MapBufferRange;
    ygfx__gl_unmap_fn   UnmapBuffer;
    int32_t        cap_n;          /* CAPTURE's pack buffers                    */
    size_t         cap_bytes;
    ygfx__GLuint   cap_buf[YGFX_CAPTURE_MAX_SLOTS];
    void*          cap_sync[YGFX_CAPTURE_MAX_SLOTS];
    const void*    cap_map[YGFX_CAPTURE_MAX_SLOTS];
    size_t         cap_len[YGFX_CAPTURE_MAX_SLOTS];   /* the bytes read */
    int32_t        w, h;
    ygfx__gltex  tex[YGFX__GL_TEX];
    ygfx__glbuf  buf[YGFX__GL_BUF];
    ygfx__glpipe pipe[YGFX__GL_PIPE];
    /* the state cache: what GL has bound, so apply() skips a call that
     * would not change anything; -1 = unknown */
    int64_t        c_prog, c_vao, c_blend, c_fbo, c_inst;
    int64_t        c_tex[6];
    int64_t        c_range_buf[2], c_range_off[2];
} ygfx__gl;

static int ygfx__gl_ext(const ygfx__gl* gl, const char* name) {
    ygfx__GLint n = 0, i;
    gl->f.GetIntegerv(YGFX__GL_NUM_EXTENSIONS, &n);
    for (i = 0; i < n; i++) {
        const char* e = (const char*)gl->f.GetStringi(YGFX__GL_EXTENSIONS, (ygfx__GLuint)i);
        if (e && strcmp(e, name) == 0) return 1;
    }
    return 0;
}

static void ygfx__gl_reset(void* c) {
    ygfx__gl* gl = (ygfx__gl*)c;
    int i;
    gl->c_prog = gl->c_vao = gl->c_blend = gl->c_fbo = gl->c_inst = -1;
    for (i = 0; i < 6; i++) gl->c_tex[i] = -1;
    for (i = 0; i < 2; i++) gl->c_range_buf[i] = gl->c_range_off[i] = -1;
}

/* A test seam: the GL backend reports no parallel compile (OPEN START). */
static int ygfx__test_serial = 0;

static int ygfx__gl_open(void* c, const ygfx_backend_open* in, ygfx_backend_caps* caps, char* err, size_t cap) {
    ygfx__gl* gl = (ygfx__gl*)c;
    const char* missing = NULL;
    const unsigned char* r;
    memset(gl, 0, sizeof *gl);
    if (!in->gl_proc) { ygfx__set_error(err, cap, "ysp_gfx: no GL loader"); return YGFX_ERR_ARG; }
#define YGFX__GL_LOAD(ret, name, args) \
    gl->f.name = (ret (YGFX__APIENTRY *) args)in->gl_proc(in->gl_ctx, "gl" #name); \
    if (!gl->f.name && !missing) missing = "gl" #name;
    YGFX__GL_FUNCS(YGFX__GL_LOAD)
#undef YGFX__GL_LOAD
    if (missing) {
        ygfx__set_error(err, cap, "ysp_gfx: the GL ES 3.0 context has no %s", missing);
        return YGFX_ERR_GL;
    }
    gl->w = in->w; gl->h = in->h;
    caps->color_buffer_float = ygfx__gl_ext(gl, "GL_EXT_color_buffer_float") != 0;
    caps->float_blend = ygfx__gl_ext(gl, "GL_EXT_float_blend") != 0;
    caps->float_linear = ygfx__gl_ext(gl, "GL_OES_texture_float_linear") != 0;
    caps->parallel_compile = ygfx__gl_ext(gl, "GL_KHR_parallel_shader_compile") != 0 && !ygfx__test_serial;
    gl->f.GetIntegerv(YGFX__GL_UBO_ALIGN, &caps->ubo_align);
    gl->f.GetIntegerv(YGFX__GL_MAX_UBO_SIZE, &caps->max_ubo);
    gl->f.GetIntegerv(YGFX__GL_MAX_TEXTURE_SIZE, &caps->max_texture);
    r = gl->f.GetString(YGFX__GL_RENDERER);
    snprintf(caps->renderer, sizeof caps->renderer, "%s", r ? (const char*)r : "?");
    r = gl->f.GetString(YGFX__GL_VENDOR);
    snprintf(caps->vendor, sizeof caps->vendor, "%s", r ? (const char*)r : "?");
    r = gl->f.GetString(YGFX__GL_VERSION);
    snprintf(caps->version, sizeof caps->version, "%s", r ? (const char*)r : "?");
    r = gl->f.GetString(YGFX__GL_SHADING_LANGUAGE_VERSION);
    snprintf(caps->glsl, sizeof caps->glsl, "%s", r ? (const char*)r : "?");
    gl->GetProgramBinary = (ygfx__gl_getbin_fn)in->gl_proc(in->gl_ctx, "glGetProgramBinary");
    gl->ProgramBinary = (ygfx__gl_bin_fn)in->gl_proc(in->gl_ctx, "glProgramBinary");
    gl->ProgramParameteri = (ygfx__gl_param_fn)in->gl_proc(in->gl_ctx, "glProgramParameteri");
    caps->binary_format = 0;
    if (gl->GetProgramBinary && gl->ProgramBinary) {
        ygfx__GLint n = 0, fmts[16];
        gl->f.GetIntegerv(YGFX__GL_NUM_PROGRAM_BINARY_FORMATS, &n);
        /* the format a binary comes back in is the driver's; the first one
         * listed is only the key's, and every format so far lists one */
        if (n >= 1 && n <= 16) {
            gl->f.GetIntegerv(YGFX__GL_PROGRAM_BINARY_FORMATS, fmts);
            caps->binary_format = (uint32_t)fmts[0];
        }
    }
    gl->FenceSync = (ygfx__gl_fence_fn)in->gl_proc(in->gl_ctx, "glFenceSync");
    gl->ClientWaitSync = (ygfx__gl_cwait_fn)in->gl_proc(in->gl_ctx, "glClientWaitSync");
    gl->DeleteSync = (ygfx__gl_dsync_fn)in->gl_proc(in->gl_ctx, "glDeleteSync");
    gl->MapBufferRange = (ygfx__gl_map_fn)in->gl_proc(in->gl_ctx, "glMapBufferRange");
    gl->UnmapBuffer = (ygfx__gl_unmap_fn)in->gl_proc(in->gl_ctx, "glUnmapBuffer");
    caps->features = YGFX_FEAT_IMPORT_GL;
#if defined(_WIN32)
    {
        ygfx__egl_dpy_fn cur = (ygfx__egl_dpy_fn)in->gl_proc(in->gl_ctx, "eglGetCurrentDisplay");
        ygfx__egl_qs_fn qs = (ygfx__egl_qs_fn)in->gl_proc(in->gl_ctx, "eglQueryString");
        const char* ext;
        gl->CreateImage = (ygfx__egl_mkimg_fn)in->gl_proc(in->gl_ctx, "eglCreateImageKHR");
        gl->DestroyImage = (ygfx__egl_rmimg_fn)in->gl_proc(in->gl_ctx, "eglDestroyImageKHR");
        gl->ImageTarget = (ygfx__gl_imgtgt_fn)in->gl_proc(in->gl_ctx, "glEGLImageTargetTexture2DOES");
        gl->dpy = cur ? cur() : NULL;
        ext = gl->dpy && qs ? qs(gl->dpy, 0x3055) : NULL;   /* EGL_EXTENSIONS */
        /* the NV12 plane and array slice attributes came with this extension
         * in the ANGLE probed (2.1.23876, docs/gfx.md) */
        if (ext && strstr(ext, "EGL_ANGLE_image_d3d11_texture") && gl->CreateImage && gl->DestroyImage && gl->ImageTarget &&
            ygfx__gl_ext(gl, "GL_OES_EGL_image"))
            caps->features |= YGFX_FEAT_IMPORT_D3D11 | YGFX_FEAT_IMPORT_NV12;
    }
#endif
    ygfx__gl_reset(gl);
    while (gl->f.GetError() != YGFX__GL_NO_ERROR) {}
    return YGFX_OK;
}

static void ygfx__gl_capture_free(void* c);

static void ygfx__gl_close(void* c) {
    ygfx__gl* gl = (ygfx__gl*)c;
    int i;
    if (!gl->f.DeleteTextures) return;
    ygfx__gl_capture_free(gl);
    for (i = 0; i < YGFX__GL_TEX; i++) {
        if (gl->tex[i].fbo) gl->f.DeleteFramebuffers(1, &gl->tex[i].fbo);
        if (gl->tex[i].tex && !gl->tex[i].borrowed) gl->f.DeleteTextures(1, &gl->tex[i].tex);
        if (gl->tex[i].image && gl->DestroyImage) gl->DestroyImage(gl->dpy, gl->tex[i].image);
    }
    for (i = 0; i < YGFX__GL_BUF; i++) if (gl->buf[i].buf) gl->f.DeleteBuffers(1, &gl->buf[i].buf);
    for (i = 0; i < YGFX__GL_PIPE; i++) {
        if (gl->pipe[i].vs) gl->f.DeleteShader(gl->pipe[i].vs);
        if (gl->pipe[i].fs) gl->f.DeleteShader(gl->pipe[i].fs);
        if (gl->pipe[i].prog) gl->f.DeleteProgram(gl->pipe[i].prog);
        if (gl->pipe[i].vao) gl->f.DeleteVertexArrays(1, &gl->pipe[i].vao);
    }
    memset(gl, 0, sizeof *gl);
}

/* ES 3.0 has no layout(binding): the blocks' and samplers' bindings are set
 * after every link, and after a binary load, which resets them. */
static void ygfx__gl_slots(ygfx__gl* gl, ygfx__glpipe* p) {
    static const char* const samplers[] = { "ysp_tex0", "ysp_tex1", "ysp_tex2", "ysp_utex0", "ysp_tex3", "ysp_tex4" };
    ygfx__GLuint idx;
    ygfx__GLint loc;
    int i;
    idx = gl->f.GetUniformBlockIndex(p->prog, "ysp_frame_block");
    if (idx != YGFX__GL_INVALID_INDEX) gl->f.UniformBlockBinding(p->prog, idx, 0);
    idx = gl->f.GetUniformBlockIndex(p->prog, "ysp_stim_block");
    if (idx != YGFX__GL_INVALID_INDEX) gl->f.UniformBlockBinding(p->prog, idx, 1);
    gl->f.UseProgram(p->prog);
    for (i = 0; i < 6; i++) {
        loc = gl->f.GetUniformLocation(p->prog, samplers[i]);
        if (loc >= 0) gl->f.Uniform1i(loc, i);
    }
    gl->c_prog = (int64_t)p->prog;
}

/* The link's status and the bindings; then the shaders go. */
static int ygfx__gl_pipeline_finish(void* c, uint32_t id, char* err, size_t cap) {
    ygfx__gl* gl = (ygfx__gl*)c;
    ygfx__glpipe* p;
    ygfx__GLint ok = 0;
    if (id < 1 || id > YGFX__GL_PIPE) return YGFX_ERR_ARG;
    p = &gl->pipe[id - 1];
    if (!p->pending) return YGFX_OK;
    p->pending = 0;
    gl->f.GetProgramiv(p->prog, YGFX__GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[400];
        ygfx__GLsizei n = 0;
        ygfx__GLint sok = 1;
        log[0] = '\0';
        gl->f.GetShaderiv(p->fs, YGFX__GL_COMPILE_STATUS, &sok);
        if (!sok) {
            gl->f.GetShaderInfoLog(p->fs, (ygfx__GLsizei)sizeof log, &n, log);
            ygfx__set_error(err, cap, "ysp_gfx: fragment shader: %s", log);
        } else {
            gl->f.GetShaderiv(p->vs, YGFX__GL_COMPILE_STATUS, &sok);
            if (!sok) {
                gl->f.GetShaderInfoLog(p->vs, (ygfx__GLsizei)sizeof log, &n, log);
                ygfx__set_error(err, cap, "ysp_gfx: vertex shader: %s", log);
            } else {
                gl->f.GetProgramInfoLog(p->prog, (ygfx__GLsizei)sizeof log, &n, log);
                ygfx__set_error(err, cap, "ysp_gfx: link: %s", log);
            }
        }
        gl->f.DeleteShader(p->vs);
        gl->f.DeleteShader(p->fs);
        gl->f.DeleteProgram(p->prog);
        if (p->vao) gl->f.DeleteVertexArrays(1, &p->vao);
        memset(p, 0, sizeof *p);
        return YGFX_ERR_GL;
    }
    gl->f.DeleteShader(p->vs);
    gl->f.DeleteShader(p->fs);
    p->vs = p->fs = 0;
    ygfx__gl_slots(gl, p);
    return YGFX_OK;
}

/* Submits the compile and link. ANGLE compiles on worker threads, so the
 * open submits every program before it asks for any status (measured: 4
 * programs 3.0 s, against 7.3 s one after another). */
static int ygfx__gl_pipeline_make(void* c, const ygfx_pipeline_src* d, uint32_t* id, char* err, size_t cap) {
    ygfx__gl* gl = (ygfx__gl*)c;
    ygfx__glpipe* p;
    int slot;
    for (slot = 0; slot < YGFX__GL_PIPE && gl->pipe[slot].prog; slot++) {}
    if (slot == YGFX__GL_PIPE) return YGFX_ERR_FULL;
    p = &gl->pipe[slot];
    p->blend = d->blend;
    p->inst = d->instance_attr;
    if (d->binary && d->binary_size && gl->ProgramBinary) {
        ygfx__GLint ok = 0;
        p->prog = gl->f.CreateProgram();
        gl->ProgramBinary(p->prog, (ygfx__GLenum)d->binary_format, d->binary, (ygfx__GLsizei)d->binary_size);
        gl->f.GetProgramiv(p->prog, YGFX__GL_LINK_STATUS, &ok);
        if (ok) {
            gl->f.GenVertexArrays(1, &p->vao);
            ygfx__gl_slots(gl, p);
            *id = (uint32_t)slot + 1;
            return 1;
        }
        /* another driver's, or damaged past what the header checks: the
         * source is here, so compile it */
        gl->f.DeleteProgram(p->prog);
        p->prog = 0;
        while (gl->f.GetError() != YGFX__GL_NO_ERROR) {}
    }
    p->vs = gl->f.CreateShader(YGFX__GL_VERTEX_SHADER);
    gl->f.ShaderSource(p->vs, 1, &d->vs, NULL);
    gl->f.CompileShader(p->vs);
    p->fs = gl->f.CreateShader(YGFX__GL_FRAGMENT_SHADER);
    gl->f.ShaderSource(p->fs, 1, &d->fs, NULL);
    gl->f.CompileShader(p->fs);
    p->prog = gl->f.CreateProgram();
    gl->f.AttachShader(p->prog, p->vs);
    gl->f.AttachShader(p->prog, p->fs);
    if (d->retrievable && gl->ProgramParameteri) gl->ProgramParameteri(p->prog, YGFX__GL_PROGRAM_BINARY_RETRIEVABLE_HINT, 1);
    gl->f.LinkProgram(p->prog);
    p->pending = 1;
    gl->f.GenVertexArrays(1, &p->vao);
    *id = (uint32_t)slot + 1;
    if (d->deferred) return YGFX_OK;
    {
        int rc = ygfx__gl_pipeline_finish(c, *id, err, cap);
        if (rc < 0) *id = 0;
        return rc;
    }
}

static int ygfx__gl_pipeline_binary(void* c, uint32_t id, void* out, size_t cap, uint32_t* format) {
    ygfx__gl* gl = (ygfx__gl*)c;
    ygfx__GLint len = 0;
    ygfx__GLsizei got = 0;
    ygfx__GLenum fmt = 0;
    if (id < 1 || id > YGFX__GL_PIPE || !gl->pipe[id - 1].prog || gl->pipe[id - 1].pending || !gl->GetProgramBinary) return -1;
    gl->f.GetProgramiv(gl->pipe[id - 1].prog, YGFX__GL_PROGRAM_BINARY_LENGTH, &len);
    if (len <= 0) return -1;
    if (!out || cap < (size_t)len) return len;
    gl->GetProgramBinary(gl->pipe[id - 1].prog, (ygfx__GLsizei)cap, &got, &fmt, out);
    if (gl->f.GetError() != YGFX__GL_NO_ERROR || got <= 0) return -1;
    if (format) *format = (uint32_t)fmt;
    return got;
}

static void ygfx__gl_pipeline_free(void* c, uint32_t id) {
    ygfx__gl* gl = (ygfx__gl*)c;
    ygfx__glpipe* p;
    if (id < 1 || id > YGFX__GL_PIPE) return;
    p = &gl->pipe[id - 1];
    if (p->vs) gl->f.DeleteShader(p->vs);
    if (p->fs) gl->f.DeleteShader(p->fs);
    if (p->prog) gl->f.DeleteProgram(p->prog);
    if (p->vao) gl->f.DeleteVertexArrays(1, &p->vao);
    memset(p, 0, sizeof *p);
    ygfx__gl_reset(gl);
}

/* Internal format, format, type for each ygfx_format. */
static void ygfx__gl_fmt(ygfx_format f, ygfx__GLenum* ifmt, ygfx__GLenum* fmt, ygfx__GLenum* type) {
    switch (f) {
    case YGFX_R8:      *ifmt = YGFX__GL_R8;      *fmt = YGFX__GL_RED;  *type = YGFX__GL_UNSIGNED_BYTE; break;
    case YGFX_RG8:     *ifmt = YGFX__GL_RG8;     *fmt = YGFX__GL_RG;   *type = YGFX__GL_UNSIGNED_BYTE; break;
    case YGFX_RGBA8:   *ifmt = YGFX__GL_RGBA8;   *fmt = YGFX__GL_RGBA; *type = YGFX__GL_UNSIGNED_BYTE; break;
    case YGFX_R16F:    *ifmt = YGFX__GL_R16F;    *fmt = YGFX__GL_RED;  *type = YGFX__GL_FLOAT; break;
    case YGFX_RGBA16F: *ifmt = YGFX__GL_RGBA16F; *fmt = YGFX__GL_RGBA; *type = YGFX__GL_FLOAT; break;
    case YGFX_R32F:    *ifmt = YGFX__GL_R32F;    *fmt = YGFX__GL_RED;  *type = YGFX__GL_FLOAT; break;
    case YGFX_RGBA32F: *ifmt = YGFX__GL_RGBA32F; *fmt = YGFX__GL_RGBA; *type = YGFX__GL_FLOAT; break;
    case YGFX_R16UI:   *ifmt = YGFX__GL_R16UI;   *fmt = YGFX__GL_RED_INTEGER; *type = YGFX__GL_UNSIGNED_SHORT; break;
    case YGFX_RGBA32UI: *ifmt = YGFX__GL_RGBA32UI; *fmt = YGFX__GL_RGBA_INTEGER; *type = YGFX__GL_UNSIGNED_INT; break;
    default:             *ifmt = *fmt = *type = 0; break;
    }
}

static void ygfx__gl_bind_tex(ygfx__gl* gl, int unit, ygfx__GLuint tex) {
    gl->f.ActiveTexture(YGFX__GL_TEXTURE0 + (ygfx__GLenum)unit);
    gl->f.BindTexture(YGFX__GL_TEXTURE_2D, tex);
    gl->c_tex[unit] = (int64_t)tex;
}

static int ygfx__gl_upload(ygfx__gl* gl, const ygfx__gltex* t, int x, int y, int w, int h,
                             const void* data, size_t stride) {
    ygfx__GLenum ifmt, fmt, type;
    int bpp = ygfx__texel_bytes((ygfx_format)t->format);
    ygfx__gl_fmt((ygfx_format)t->format, &ifmt, &fmt, &type);
    ygfx__gl_bind_tex(gl, 0, t->tex);
    gl->f.PixelStorei(YGFX__GL_UNPACK_ALIGNMENT, 1);
    gl->f.PixelStorei(YGFX__GL_UNPACK_ROW_LENGTH, stride ? (ygfx__GLint)(stride / (size_t)bpp) : 0);
    gl->f.TexSubImage2D(YGFX__GL_TEXTURE_2D, 0, x, y, w, h, fmt, type, data);
    gl->f.PixelStorei(YGFX__GL_UNPACK_ROW_LENGTH, 0);
    return YGFX_OK;
}

static int ygfx__gl_texture_make(void* c, const ygfx_texture_src* d, uint32_t* id) {
    ygfx__gl* gl = (ygfx__gl*)c;
    ygfx__GLenum ifmt, fmt, type;
    ygfx__gltex* t;
    int slot;
    ygfx__GLint filt;
    for (slot = 0; slot < YGFX__GL_TEX && gl->tex[slot].tex; slot++) {}
    if (slot == YGFX__GL_TEX) return YGFX_ERR_FULL;
    ygfx__gl_fmt(d->format, &ifmt, &fmt, &type);
    if (!ifmt) return YGFX_ERR_ARG;
    t = &gl->tex[slot];
    if (d->gl_name || d->d3d11) {
        /* an import: texelFetch needs a complete texture, so its filters
         * become NEAREST (the caller's texture, changed) */
        if (d->gl_name) {
            t->tex = d->gl_name;
            t->borrowed = 1;
        } else {
            int32_t at[6];
            int k = 0;
            /* the plane only for an NV12 texture's planes, the slice only in
             * an array */
            if (d->format == YGFX_R8 || d->format == YGFX_RG8) { at[k++] = 0x3492; at[k++] = d->plane; }   /* EGL_D3D11_TEXTURE_PLANE_ANGLE */
            if (d->slice > 0) { at[k++] = 0x3493; at[k++] = d->slice; }   /* EGL_D3D11_TEXTURE_ARRAY_SLICE_ANGLE */
            at[k] = 0x3038;                                                /* EGL_NONE */
            if (!gl->CreateImage || !gl->dpy) return YGFX_ERR_NOT_IMPLEMENTED;
            t->image = gl->CreateImage(gl->dpy, NULL, 0x3484, d->d3d11, at);   /* EGL_D3D11_TEXTURE_ANGLE */
            if (!t->image) { memset(t, 0, sizeof *t); return YGFX_ERR_GL; }
            gl->f.GenTextures(1, &t->tex);
            ygfx__gl_bind_tex(gl, 0, t->tex);
            gl->ImageTarget(YGFX__GL_TEXTURE_2D, t->image);
        }
        ygfx__gl_bind_tex(gl, 0, t->tex);
        gl->f.TexParameteri(YGFX__GL_TEXTURE_2D, YGFX__GL_TEXTURE_MIN_FILTER, YGFX__GL_NEAREST);
        gl->f.TexParameteri(YGFX__GL_TEXTURE_2D, YGFX__GL_TEXTURE_MAG_FILTER, YGFX__GL_NEAREST);
        gl->f.TexParameteri(YGFX__GL_TEXTURE_2D, YGFX__GL_TEXTURE_WRAP_S, YGFX__GL_CLAMP_TO_EDGE);
        gl->f.TexParameteri(YGFX__GL_TEXTURE_2D, YGFX__GL_TEXTURE_WRAP_T, YGFX__GL_CLAMP_TO_EDGE);
        t->w = d->w; t->h = d->h; t->format = d->format;
        if (gl->f.GetError() != YGFX__GL_NO_ERROR) {
            if (!t->borrowed) gl->f.DeleteTextures(1, &t->tex);
            if (t->image) gl->DestroyImage(gl->dpy, t->image);
            memset(t, 0, sizeof *t);
            ygfx__gl_reset(gl);
            return YGFX_ERR_GL;
        }
        *id = (uint32_t)slot + 1;
        return YGFX_OK;
    }
    gl->f.GenTextures(1, &t->tex);
    ygfx__gl_bind_tex(gl, 0, t->tex);
    gl->f.TexStorage2D(YGFX__GL_TEXTURE_2D, 1, ifmt, d->w, d->h);
    filt = (d->linear && d->format != YGFX_R16UI) ? YGFX__GL_LINEAR : YGFX__GL_NEAREST;
    gl->f.TexParameteri(YGFX__GL_TEXTURE_2D, YGFX__GL_TEXTURE_MIN_FILTER, filt);
    gl->f.TexParameteri(YGFX__GL_TEXTURE_2D, YGFX__GL_TEXTURE_MAG_FILTER, filt);
    gl->f.TexParameteri(YGFX__GL_TEXTURE_2D, YGFX__GL_TEXTURE_WRAP_S, YGFX__GL_CLAMP_TO_EDGE);
    gl->f.TexParameteri(YGFX__GL_TEXTURE_2D, YGFX__GL_TEXTURE_WRAP_T, YGFX__GL_CLAMP_TO_EDGE);
    t->w = d->w; t->h = d->h; t->format = d->format;
    if (d->data) ygfx__gl_upload(gl, t, 0, 0, d->w, d->h, d->data, d->stride);
    if (d->target) {
        gl->f.GenFramebuffers(1, &t->fbo);
        gl->f.BindFramebuffer(YGFX__GL_FRAMEBUFFER, t->fbo);
        gl->f.FramebufferTexture2D(YGFX__GL_FRAMEBUFFER, YGFX__GL_COLOR_ATTACHMENT0, YGFX__GL_TEXTURE_2D, t->tex, 0);
        gl->c_fbo = (int64_t)t->fbo;
        if (gl->f.CheckFramebufferStatus(YGFX__GL_FRAMEBUFFER) != YGFX__GL_FRAMEBUFFER_COMPLETE) {
            gl->f.DeleteFramebuffers(1, &t->fbo);
            gl->f.DeleteTextures(1, &t->tex);
            memset(t, 0, sizeof *t);
            ygfx__gl_reset(gl);
            return YGFX_ERR_GL;
        }
    }
    if (gl->f.GetError() != YGFX__GL_NO_ERROR) {
        if (t->fbo) gl->f.DeleteFramebuffers(1, &t->fbo);
        gl->f.DeleteTextures(1, &t->tex);
        memset(t, 0, sizeof *t);
        ygfx__gl_reset(gl);
        return YGFX_ERR_GL;
    }
    *id = (uint32_t)slot + 1;
    return YGFX_OK;
}

static int ygfx__gl_texture_update(void* c, uint32_t id, int x, int y, int w, int h, const void* data, size_t stride) {
    ygfx__gl* gl = (ygfx__gl*)c;
    if (id < 1 || id > YGFX__GL_TEX || !gl->tex[id - 1].tex) return YGFX_ERR_ARG;
    return ygfx__gl_upload(gl, &gl->tex[id - 1], x, y, w, h, data, stride);
}

static void ygfx__gl_texture_free(void* c, uint32_t id) {
    ygfx__gl* gl = (ygfx__gl*)c;
    ygfx__gltex* t;
    if (id < 1 || id > YGFX__GL_TEX) return;
    t = &gl->tex[id - 1];
    if (t->fbo) gl->f.DeleteFramebuffers(1, &t->fbo);
    if (t->tex && !t->borrowed) gl->f.DeleteTextures(1, &t->tex);
    if (t->image && gl->DestroyImage) gl->DestroyImage(gl->dpy, t->image);
    memset(t, 0, sizeof *t);
    ygfx__gl_reset(gl);
}

static int ygfx__gl_buffer_make(void* c, int kind, size_t bytes, uint32_t* id) {
    ygfx__gl* gl = (ygfx__gl*)c;
    ygfx__glbuf* b;
    int slot;
    for (slot = 0; slot < YGFX__GL_BUF && gl->buf[slot].buf; slot++) {}
    if (slot == YGFX__GL_BUF) return YGFX_ERR_FULL;
    b = &gl->buf[slot];
    b->target = kind == YGFX_BUFFER_UNIFORM ? YGFX__GL_UNIFORM_BUFFER : YGFX__GL_ARRAY_BUFFER;
    b->bytes = (uint32_t)bytes;
    gl->f.GenBuffers(1, &b->buf);
    gl->f.BindBuffer(b->target, b->buf);
    gl->f.BufferData(b->target, (ygfx__GLsizeiptr)bytes, NULL, YGFX__GL_DYNAMIC_DRAW);
    if (gl->f.GetError() != YGFX__GL_NO_ERROR) {
        gl->f.DeleteBuffers(1, &b->buf);
        memset(b, 0, sizeof *b);
        return YGFX_ERR_GL;
    }
    *id = (uint32_t)slot + 1;
    return YGFX_OK;
}

static int ygfx__gl_buffer_update(void* c, uint32_t id, size_t off, const void* data, size_t n) {
    ygfx__gl* gl = (ygfx__gl*)c;
    ygfx__glbuf* b;
    if (id < 1 || id > YGFX__GL_BUF || !gl->buf[id - 1].buf) return YGFX_ERR_ARG;
    b = &gl->buf[id - 1];
    if (off + n > b->bytes) return YGFX_ERR_ARG;
    gl->f.BindBuffer(b->target, b->buf);
    gl->f.BufferSubData(b->target, (ygfx__GLintptr)off, (ygfx__GLsizeiptr)n, data);
    return YGFX_OK;
}

static void ygfx__gl_buffer_free(void* c, uint32_t id) {
    ygfx__gl* gl = (ygfx__gl*)c;
    if (id < 1 || id > YGFX__GL_BUF || !gl->buf[id - 1].buf) return;
    gl->f.DeleteBuffers(1, &gl->buf[id - 1].buf);
    memset(&gl->buf[id - 1], 0, sizeof gl->buf[0]);
    ygfx__gl_reset(gl);
}

static void ygfx__gl_pass_begin(void* c, uint32_t target, int w, int h, const float* clear) {
    ygfx__gl* gl = (ygfx__gl*)c;
    ygfx__GLuint fbo = (target >= 1 && target <= YGFX__GL_TEX) ? gl->tex[target - 1].fbo : 0;
    if (gl->c_fbo != (int64_t)fbo) {
        gl->f.BindFramebuffer(YGFX__GL_FRAMEBUFFER, fbo);
        gl->c_fbo = (int64_t)fbo;
    }
    gl->f.Viewport(0, 0, w, h);
    if (clear) {
        gl->f.ColorMask(1, 1, 1, 1);
        gl->f.ClearColor(clear[0], clear[1], clear[2], clear[3]);
        gl->f.Clear(YGFX__GL_COLOR_BUFFER_BIT);
    }
}

static void ygfx__gl_apply(void* c, const ygfx_bindings* b) {
    ygfx__gl* gl = (ygfx__gl*)c;
    ygfx__glpipe* p;
    int i;
    if (b->pipeline < 1 || b->pipeline > YGFX__GL_PIPE) return;
    p = &gl->pipe[b->pipeline - 1];
    if (gl->c_prog != (int64_t)p->prog) { gl->f.UseProgram(p->prog); gl->c_prog = (int64_t)p->prog; }
    if (gl->c_vao != (int64_t)p->vao) { gl->f.BindVertexArray(p->vao); gl->c_vao = (int64_t)p->vao; gl->c_inst = -1; }
    {
        int32_t bl = b->blend ? b->blend : p->blend;
        if (gl->c_blend != bl) {
            if (bl == YGFX_BLEND_NONE) {
                gl->f.Disable(YGFX__GL_BLEND);
            } else {
                gl->f.Enable(YGFX__GL_BLEND);
                gl->f.BlendEquation(YGFX__GL_FUNC_ADD);
                /* OVER accumulates premultiplied alpha, which a target drawn
                 * as an image needs; ADD and MULTIPLY leave alpha alone: an
                 * increment and a filter cover nothing. The scene's alpha is
                 * never read. */
                if (bl == YGFX_BLEND_ADD)
                    gl->f.BlendFuncSeparate(YGFX__GL_ONE, YGFX__GL_ONE, YGFX__GL_ZERO, YGFX__GL_ONE);
                else if (bl == YGFX_BLEND_MULTIPLY)
                    gl->f.BlendFuncSeparate(YGFX__GL_DST_COLOR, YGFX__GL_ONE_MINUS_SRC_ALPHA,
                                            YGFX__GL_ZERO, YGFX__GL_ONE);
                else
                    gl->f.BlendFuncSeparate(YGFX__GL_ONE, YGFX__GL_ONE_MINUS_SRC_ALPHA,
                                            YGFX__GL_ONE, YGFX__GL_ONE_MINUS_SRC_ALPHA);
            }
            gl->c_blend = bl;
        }
    }
    if (b->ubo >= 1 && b->ubo <= YGFX__GL_BUF) {
        ygfx__GLuint ub = gl->buf[b->ubo - 1].buf;
        if (b->frame_size && (gl->c_range_buf[0] != (int64_t)ub || gl->c_range_off[0] != (int64_t)b->frame_off)) {
            gl->f.BindBufferRange(YGFX__GL_UNIFORM_BUFFER, 0, ub, (ygfx__GLintptr)b->frame_off, (ygfx__GLsizeiptr)b->frame_size);
            gl->c_range_buf[0] = (int64_t)ub; gl->c_range_off[0] = (int64_t)b->frame_off;
        }
        if (b->stim_size && (gl->c_range_buf[1] != (int64_t)ub || gl->c_range_off[1] != (int64_t)b->stim_off)) {
            gl->f.BindBufferRange(YGFX__GL_UNIFORM_BUFFER, 1, ub, (ygfx__GLintptr)b->stim_off, (ygfx__GLsizeiptr)b->stim_size);
            gl->c_range_buf[1] = (int64_t)ub; gl->c_range_off[1] = (int64_t)b->stim_off;
        }
    }
    for (i = 0; i < 6; i++) {
        ygfx__GLuint t = (b->tex[i] >= 1 && b->tex[i] <= YGFX__GL_TEX) ? gl->tex[b->tex[i] - 1].tex : 0;
        if (t && gl->c_tex[i] != (int64_t)t) ygfx__gl_bind_tex(gl, i, t);
    }
    /* the cache key is the buffer and the offset: a run from its first record */
    if (p->inst && b->instances >= 1 && b->instances <= YGFX__GL_BUF &&
        gl->c_inst != ((int64_t)b->instances | ((int64_t)b->instances_off << 16))) {
        size_t o = b->instances_off;
        gl->f.BindBuffer(YGFX__GL_ARRAY_BUFFER, gl->buf[b->instances - 1].buf);
        gl->f.EnableVertexAttribArray(0);
        if (p->inst == 2) {   /* glyphs: two vec4 per instance */
            gl->f.VertexAttribPointer(0, 4, YGFX__GL_FLOAT, 0, 32, (const void*)o);
            gl->f.EnableVertexAttribArray(1);
            gl->f.VertexAttribPointer(1, 4, YGFX__GL_FLOAT, 0, 32, (const void*)(o + 16));
            gl->f.VertexAttribDivisor(1, 1);
        } else {
            gl->f.VertexAttribPointer(0, 2, YGFX__GL_FLOAT, 0, 8, (const void*)o);
        }
        gl->f.VertexAttribDivisor(0, 1);
        gl->c_inst = (int64_t)b->instances | ((int64_t)b->instances_off << 16);
    }
}

static void ygfx__gl_draw(void* c, int vertices, int instances) {
    ygfx__gl* gl = (ygfx__gl*)c;
    gl->f.DrawArraysInstanced(vertices == 3 ? YGFX__GL_TRIANGLES : YGFX__GL_TRIANGLE_STRIP, 0, vertices, instances);
}

static void ygfx__gl_pass_end(void* c) { (void)c; }

/* ysp_screen's presenter flushes inside the present; a headless caller reads
 * back, which waits anyway. */
static void ygfx__gl_present(void* c) { (void)c; }

static int ygfx__gl_read(void* c, uint32_t target, int x, int y, int w, int h, int format, void* out) {
    ygfx__gl* gl = (ygfx__gl*)c;
    ygfx__GLuint fbo = (target >= 1 && target <= YGFX__GL_TEX) ? gl->tex[target - 1].fbo : 0;
    gl->f.BindFramebuffer(YGFX__GL_FRAMEBUFFER, fbo);
    gl->c_fbo = (int64_t)fbo;
    gl->f.PixelStorei(YGFX__GL_PACK_ALIGNMENT, 1);
    gl->f.ReadPixels(x, y, w, h, YGFX__GL_RGBA, format == YGFX_READ_FLOAT ? YGFX__GL_FLOAT : YGFX__GL_UNSIGNED_BYTE, out);
    return gl->f.GetError() == YGFX__GL_NO_ERROR ? YGFX_OK : YGFX_ERR_GL;
}

/* CAPTURE. glReadPixels into a bound pack buffer returns before the copy
 * is done; the fence says when it is. The buffer stays mapped while the
 * writer reads it, so the frame thread copies nothing itself. */
static int ygfx__gl_capture_make(void* c, int n, size_t bytes) {
    ygfx__gl* gl = (ygfx__gl*)c;
    int i;
    if (!gl->FenceSync || !gl->ClientWaitSync || !gl->DeleteSync || !gl->MapBufferRange || !gl->UnmapBuffer)
        return YGFX_ERR_NOT_IMPLEMENTED;
    if (n < 1 || n > YGFX_CAPTURE_MAX_SLOTS || gl->cap_n) return YGFX_ERR_ARG;
    while (gl->f.GetError() != YGFX__GL_NO_ERROR) {}
    gl->f.GenBuffers(n, gl->cap_buf);
    for (i = 0; i < n; i++) {
        gl->f.BindBuffer(YGFX__GL_PIXEL_PACK_BUFFER, gl->cap_buf[i]);
        gl->f.BufferData(YGFX__GL_PIXEL_PACK_BUFFER, (ygfx__GLsizeiptr)bytes, NULL, YGFX__GL_STREAM_READ);
    }
    gl->f.BindBuffer(YGFX__GL_PIXEL_PACK_BUFFER, 0);
    gl->cap_n = n;
    gl->cap_bytes = bytes;
    if (gl->f.GetError() != YGFX__GL_NO_ERROR) { ygfx__gl_capture_free(gl); return YGFX_ERR_GL; }
    return YGFX_OK;
}

static int ygfx__gl_capture_read(void* c, int slot, int x, int y, int w, int h) {
    ygfx__gl* gl = (ygfx__gl*)c;
    size_t len = (size_t)w * (size_t)h * 4;
    if (slot < 0 || slot >= gl->cap_n || gl->cap_sync[slot] || gl->cap_map[slot] || len > gl->cap_bytes) return YGFX_ERR_ARG;
    if (gl->c_fbo != 0) { gl->f.BindFramebuffer(YGFX__GL_FRAMEBUFFER, 0); gl->c_fbo = 0; }
    gl->f.BindBuffer(YGFX__GL_PIXEL_PACK_BUFFER, gl->cap_buf[slot]);
    gl->f.PixelStorei(YGFX__GL_PACK_ALIGNMENT, 4);
    gl->f.ReadPixels(x, y, w, h, YGFX__GL_RGBA, YGFX__GL_UNSIGNED_BYTE, NULL);
    /* unbound at once: a later ygfx_read_output() reads into client memory */
    gl->f.BindBuffer(YGFX__GL_PIXEL_PACK_BUFFER, 0);
    gl->cap_sync[slot] = gl->FenceSync(YGFX__GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    gl->cap_len[slot] = len;
    return gl->cap_sync[slot] ? YGFX_OK : YGFX_ERR_GL;
}

static int ygfx__gl_capture_poll(void* c, int slot, int64_t wait_ns, const void** data) {
    ygfx__gl* gl = (ygfx__gl*)c;
    ygfx__GLenum r;
    const void* p;
    if (slot < 0 || slot >= gl->cap_n) return YGFX_ERR_ARG;
    if (gl->cap_map[slot]) { *data = gl->cap_map[slot]; return 1; }
    if (!gl->cap_sync[slot]) return YGFX_ERR_ARG;
    /* a wait flushes first, or the fence may never reach the GPU; a poll
     * does not, so a frame pays no flush (the present flushes) */
    r = gl->ClientWaitSync(gl->cap_sync[slot], wait_ns > 0 ? YGFX__GL_SYNC_FLUSH_COMMANDS_BIT : 0u,
                           wait_ns > 0 ? (uint64_t)wait_ns : 0u);
    if (r == YGFX__GL_TIMEOUT_EXPIRED) return 0;
    gl->DeleteSync(gl->cap_sync[slot]);
    gl->cap_sync[slot] = NULL;
    if (r == YGFX__GL_WAIT_FAILED) return YGFX_ERR_GL;
    gl->f.BindBuffer(YGFX__GL_PIXEL_PACK_BUFFER, gl->cap_buf[slot]);
    p = gl->MapBufferRange(YGFX__GL_PIXEL_PACK_BUFFER, 0, (ygfx__GLsizeiptr)gl->cap_len[slot], YGFX__GL_MAP_READ_BIT);
    gl->f.BindBuffer(YGFX__GL_PIXEL_PACK_BUFFER, 0);
    if (!p) return YGFX_ERR_GL;
    gl->cap_map[slot] = p;
    *data = p;
    return 1;
}

static void ygfx__gl_capture_unmap(void* c, int slot) {
    ygfx__gl* gl = (ygfx__gl*)c;
    if (slot < 0 || slot >= gl->cap_n || !gl->cap_map[slot]) return;
    gl->f.BindBuffer(YGFX__GL_PIXEL_PACK_BUFFER, gl->cap_buf[slot]);
    gl->UnmapBuffer(YGFX__GL_PIXEL_PACK_BUFFER);
    gl->f.BindBuffer(YGFX__GL_PIXEL_PACK_BUFFER, 0);
    gl->cap_map[slot] = NULL;
}

static void ygfx__gl_capture_free(void* c) {
    ygfx__gl* gl = (ygfx__gl*)c;
    int i;
    for (i = 0; i < gl->cap_n; i++) {
        ygfx__gl_capture_unmap(gl, i);
        if (gl->cap_sync[i]) { gl->DeleteSync(gl->cap_sync[i]); gl->cap_sync[i] = NULL; }
    }
    if (gl->cap_n) gl->f.DeleteBuffers(gl->cap_n, gl->cap_buf);
    memset(gl->cap_buf, 0, sizeof gl->cap_buf);
    gl->cap_n = 0;
    gl->cap_bytes = 0;
}

static const ygfx_backend ygfx__gl_backend = {
    YGFX_BACKEND_VERSION, "gles3",
    ygfx__gl_open, ygfx__gl_close, ygfx__gl_pipeline_make, ygfx__gl_pipeline_free,
    ygfx__gl_texture_make, ygfx__gl_texture_update, ygfx__gl_texture_free,
    ygfx__gl_buffer_make, ygfx__gl_buffer_update, ygfx__gl_buffer_free,
    ygfx__gl_pass_begin, ygfx__gl_apply, ygfx__gl_draw, ygfx__gl_pass_end,
    ygfx__gl_present, ygfx__gl_read, ygfx__gl_reset, ygfx__gl_pipeline_finish,
    ygfx__gl_pipeline_binary,
    ygfx__gl_capture_make, ygfx__gl_capture_read, ygfx__gl_capture_poll,
    ygfx__gl_capture_unmap, ygfx__gl_capture_free
};

/* --- null backend: a screen without GL (SIM), and the interface's second
 *     implementation ---------------------------------------------------------- */

typedef struct ygfx__null {
    uint32_t       next;
    /* CAPTURE: buffers in memory, so the core's capture runs on SIM */
    int32_t        cap_n;
    size_t         cap_bytes;
    unsigned char* cap_mem;
    int32_t        cap_wait[YGFX_CAPTURE_MAX_SLOTS];   /* polls left; -1 = no copy */
    int32_t        cap_mapped[YGFX_CAPTURE_MAX_SLOTS];
    uint32_t       cap_reads;
} ygfx__null;

/* Test seams: polls before a copy is done, and a fill (the read's count in
 * every byte) in place of black. */
static int ygfx__test_cap_delay = 0;
static int ygfx__test_cap_fill = 0;

static int ygfx__null_open(void* c, const ygfx_backend_open* in, ygfx_backend_caps* caps, char* err, size_t cap) {
    (void)in; (void)err; (void)cap;
    ((ygfx__null*)c)->next = 1;
    caps->color_buffer_float = caps->float_blend = caps->float_linear = true;
    caps->ubo_align = 256;
    caps->max_ubo = 65536;
    caps->max_texture = 16384;
    snprintf(caps->renderer, sizeof caps->renderer, "null (nothing is drawn)");
    return YGFX_OK;
}
static void ygfx__null_close(void* c) { (void)c; }
static int ygfx__null_pipeline_make(void* c, const ygfx_pipeline_src* d, uint32_t* id, char* err, size_t cap) {
    (void)d; (void)err; (void)cap; *id = ((ygfx__null*)c)->next++; return YGFX_OK;
}
static void ygfx__null_free(void* c, uint32_t id) { (void)c; (void)id; }
static int ygfx__null_texture_make(void* c, const ygfx_texture_src* d, uint32_t* id) {
    (void)d; *id = ((ygfx__null*)c)->next++; return YGFX_OK;
}
static int ygfx__null_texture_update(void* c, uint32_t id, int x, int y, int w, int h, const void* data, size_t stride) {
    (void)c; (void)id; (void)x; (void)y; (void)w; (void)h; (void)data; (void)stride; return YGFX_OK;
}
static int ygfx__null_buffer_make(void* c, int kind, size_t bytes, uint32_t* id) {
    (void)kind; (void)bytes; *id = ((ygfx__null*)c)->next++; return YGFX_OK;
}
static int ygfx__null_buffer_update(void* c, uint32_t id, size_t off, const void* data, size_t n) {
    (void)c; (void)id; (void)off; (void)data; (void)n; return YGFX_OK;
}
static void ygfx__null_pass_begin(void* c, uint32_t t, int w, int h, const float* clear) {
    (void)c; (void)t; (void)w; (void)h; (void)clear;
}
static void ygfx__null_apply(void* c, const ygfx_bindings* b) { (void)c; (void)b; }
static void ygfx__null_draw(void* c, int v, int n) { (void)c; (void)v; (void)n; }
static void ygfx__null_void(void* c) { (void)c; }
static int ygfx__null_read(void* c, uint32_t t, int x, int y, int w, int h, int f, void* out) {
    (void)c; (void)t; (void)x; (void)y; (void)w; (void)h; (void)f; (void)out; return YGFX_ERR_NOT_IMPLEMENTED;
}

/* Nothing is drawn, so a copy is black (or the test's fill). */
static void ygfx__null_capture_free(void* c) {
    ygfx__null* n = (ygfx__null*)c;
    free(n->cap_mem);
    n->cap_mem = NULL;
    n->cap_n = 0;
}
static int ygfx__null_capture_make(void* c, int k, size_t bytes) {
    ygfx__null* n = (ygfx__null*)c;
    int i;
    if (k < 1 || k > YGFX_CAPTURE_MAX_SLOTS || n->cap_n) return YGFX_ERR_ARG;
    n->cap_mem = (unsigned char*)calloc((size_t)k, bytes);
    if (!n->cap_mem) return YGFX_ERR_FULL;
    n->cap_n = k;
    n->cap_bytes = bytes;
    for (i = 0; i < k; i++) { n->cap_wait[i] = -1; n->cap_mapped[i] = 0; }
    return YGFX_OK;
}
static int ygfx__null_capture_read(void* c, int slot, int x, int y, int w, int h) {
    ygfx__null* n = (ygfx__null*)c;
    size_t len = (size_t)w * (size_t)h * 4;
    (void)x; (void)y;
    if (slot < 0 || slot >= n->cap_n || n->cap_wait[slot] >= 0 || n->cap_mapped[slot] || len > n->cap_bytes)
        return YGFX_ERR_ARG;
    n->cap_reads++;
    /* calloc's zeros are the black; a memset per copy cost the frame thread
     * 0.1 to 0.5 ms at 800 x 600 on SIM */
    if (ygfx__test_cap_fill) memset(n->cap_mem + (size_t)slot * n->cap_bytes, (int)(n->cap_reads & 0xFFu), len);
    n->cap_wait[slot] = ygfx__test_cap_delay;
    return YGFX_OK;
}
static int ygfx__null_capture_poll(void* c, int slot, int64_t wait_ns, const void** data) {
    ygfx__null* n = (ygfx__null*)c;
    if (slot < 0 || slot >= n->cap_n) return YGFX_ERR_ARG;
    if (!n->cap_mapped[slot]) {
        if (n->cap_wait[slot] < 0) return YGFX_ERR_ARG;
        if (n->cap_wait[slot] > 0 && wait_ns <= 0) { n->cap_wait[slot]--; return 0; }
        n->cap_wait[slot] = -1;
        n->cap_mapped[slot] = 1;
    }
    *data = n->cap_mem + (size_t)slot * n->cap_bytes;
    return 1;
}
static void ygfx__null_capture_unmap(void* c, int slot) {
    ygfx__null* n = (ygfx__null*)c;
    if (slot >= 0 && slot < n->cap_n) n->cap_mapped[slot] = 0;
}

static const ygfx_backend ygfx__null_backend = {
    YGFX_BACKEND_VERSION, "null",
    ygfx__null_open, ygfx__null_close, ygfx__null_pipeline_make, ygfx__null_free,
    ygfx__null_texture_make, ygfx__null_texture_update, ygfx__null_free,
    ygfx__null_buffer_make, ygfx__null_buffer_update, ygfx__null_free,
    ygfx__null_pass_begin, ygfx__null_apply, ygfx__null_draw, ygfx__null_void,
    ygfx__null_void, ygfx__null_read, ygfx__null_void, NULL, NULL,
    ygfx__null_capture_make, ygfx__null_capture_read, ygfx__null_capture_poll,
    ygfx__null_capture_unmap, ygfx__null_capture_free
};

typedef char ygfx__gl_fits[sizeof(ygfx__gl) <= sizeof(((ygfx_gfx*)0)->backend_mem) ? 1 : -1];

/* --- shaders --------------------------------------------------------------- */

/* Stimuli per instanced draw, as vec4s of the stim block (16 each). Private:
 * the bench overrides it to measure. */
#ifndef YGFX__BATCH_VEC4
#define YGFX__BATCH_VEC4 256
#endif
#define YGFX__BATCH (YGFX__BATCH_VEC4 / 16)
#define YGFX__STR2(x) #x
#define YGFX__STR(x) YGFX__STR2(x)

/* The contract every stimulus program is built on: the frame block, the
 * stim blocks, the texture slots and the accessors. Built-in stimuli are
 * bodies against it, the same as a user's. */
static const char ygfx__glsl_common[] =
    "#version 300 es\n"
    "precision highp float;\n"
    "precision highp int;\n"
    "precision highp sampler2D;\n"
    "precision highp usampler2D;\n"
    "layout(std140) uniform ysp_frame_block {\n"
    "    vec4  ysp_target;\n"     /* w, h, 1/w, 1/h                         */
    "    vec4  ysp_time;\n"       /* onset s since open, period s, index, eye */
    "    uvec4 ysp_seed;\n"       /* index low, index high, seed, vblank low */
    "    ivec4 ysp_outp;\n"       /* output stage: lut n, max code, dither, flip h */
    "    vec4  ysp_axes;\n"       /* x: the local y axis sign in the pass's GL
                                 * axes (ygfx__place)                     */
    "};\n"
    /* YGFX__BATCH stimuli of 16 vec4: xf, size, look, color, dir, edge,
     * shape, misc, p[8]. ANGLE's compile time through D3D11 grows with this
     * array's length (docs/gfx.md), so the batch is short. */
    "layout(std140) uniform ysp_stim_block { vec4 ysp_v[" YGFX__STR(YGFX__BATCH_VEC4) "]; };\n";

static const char ygfx__glsl_access[] =
    /* An element of an instanced stimulus (INSTANCES) replaces its
     * template's placement, look, color or dir and misc from the vertex
     * shader. A built-in body sees the template's frame (p / ysp_k); a user
     * body its own px, with ysp_size scaled, as in a group of that scale. */
    "bool ysp_umain_ = false;\n"
    "#ifdef YSP_INST\n"
    "flat in vec4 ysp_ixf, ysp_ilook, ysp_icolor, ysp_idir, ysp_imisc, ysp_isize;\n"
    "flat in float ysp_k;\n"
    "#define ysp_xf    ysp_ixf\n"
    "#define ysp_look  ysp_ilook\n"
    "#define ysp_color ysp_icolor\n"
    "#define ysp_dir   ysp_idir\n"
    "#define ysp_misc  ysp_imisc\n"
    "#ifdef YSP_INST_USER\n"
    "#define ysp_size  (ysp_umain_ ? ysp_isize : ysp_v[ysp_i * 16 + 1])\n"
    "#define ysp_mainp_(p) (p)\n"
    "#else\n"
    "#define ysp_size  ysp_v[ysp_i * 16 + 1]\n"
    "#define ysp_mainp_(p) ((p) / ysp_k)\n"
    "#endif\n"
    "#else\n"
    "#define ysp_xf    ysp_v[ysp_i * 16]\n"
    "#define ysp_size  ysp_v[ysp_i * 16 + 1]\n"
    "#define ysp_look  ysp_v[ysp_i * 16 + 2]\n"
    "#define ysp_color ysp_v[ysp_i * 16 + 3]\n"
    "#define ysp_dir   ysp_v[ysp_i * 16 + 4]\n"
    "#define ysp_mainp_(p) (p)\n"
    "#endif\n"
    "#define ysp_edge  ysp_v[ysp_i * 16 + 5]\n"
    "#define ysp_shape ysp_v[ysp_i * 16 + 6]\n"
    "#ifndef YSP_INST\n"
    "#define ysp_misc  ysp_v[ysp_i * 16 + 7]\n"
    "#endif\n"
    "#define ysp_param(k) (ysp_v[ysp_i * 16 + 8 + (k) / 4][(k) % 4])\n";

/* Cosine and sine of degrees: reduced to a quarter turn's eighth, then
 * Taylor polynomials, as D3D's sin and cos are coarse (VECTOR SHAPES).
 * Instances and runs both turn their elements by it. */
#define YGFX__GLSL_CSD \
    "vec2 ysp_csd_(float deg) {\n" \
    "    float t = deg / 360.0;\n" \
    "    t -= floor(t + 0.5);\n" \
    "    float q = floor(t * 4.0 + 0.5), r = (t - 0.25 * q) * 6.28318530717959, r2 = r * r;\n" \
    "    float s = r * (1.0 + r2 * (-0.166666666666667 + r2 * (0.00833333333333333 + r2 * (-0.000198412698412698 + r2 * 2.75573192239859e-6))));\n" \
    "    float c = 1.0 + r2 * (-0.5 + r2 * (0.0416666666666667 + r2 * (-0.00138888888888889 + r2 * 2.48015873015873e-5)));\n" \
    "    int qi = int(q) & 3;\n" \
    "    return qi == 0 ? vec2(c, s) : (qi == 1 ? vec2(-s, c) : (qi == 2 ? vec2(-c, -s) : vec2(s, -c)));\n" \
    "}\n"

/* Quads: one stimulus per instance, from the range bound at its first block.
 * Dots: one dot per instance, the stimulus is block 0 of the range. The
 * fragment shader gets the center, not interpolated coordinates: a
 * rasterizer snaps vertices to its subpixel grid, and interpolated local
 * coordinates then ran up to 2e-2 px off on SwiftShader (docs/gfx.md). */
static const char ygfx__glsl_vs_body[] =
    "flat out vec2 ysp_c;\n"
    "flat out int ysp_i;\n"
    "#ifdef YSP_VIDEO\n"
    "flat out vec4 ysp_vk[7];\n"
    "#endif\n"
    "#ifdef YSP_GLYPHS\n"
    "layout(location = 0) in vec4 ysp_g0;\n"     /* x, y units; atlas x, y */
    "layout(location = 1) in vec4 ysp_g1;\n"     /* atlas w, h            */
    "flat out vec2 ysp_gh;\n"
    "flat out vec4 ysp_gr;\n"
    "#endif\n"
    "#ifdef YSP_INST\n"
    "layout(location = 0) in vec4 ysp_a0;\n"     /* x, y units; ori deg; phase cyc */
    "layout(location = 1) in vec4 ysp_a1;\n"     /* contrast, scale, gate, color */
    "flat out vec4 ysp_ixf, ysp_ilook, ysp_icolor, ysp_idir, ysp_imisc, ysp_isize;\n"
    "flat out float ysp_k;\n"
    YGFX__GLSL_CSD
    "#endif\n"
    "#ifdef YSP_DOTS\n"
    "layout(location = 0) in vec2 ysp_dot;\n"
    "float ysp_field_(vec2 q, vec4 f) {\n"     /* hx, hy, shape */
    "    if (f.z == 0.0) { vec2 d = abs(q) - f.xy; return max(d.x, d.y); }\n"
    "    if (f.z == 1.0) return length(q) - f.x;\n"
    "    return -1.0;\n"
    "}\n"
    "#endif\n"
    "void main() {\n"
    "#if defined(YSP_DOTS) || defined(YSP_GLYPHS) || defined(YSP_INST)\n"
    "    int i = 0;\n"
    "#else\n"
    "    int i = gl_InstanceID;\n"
    "#endif\n"
    "    vec4 xf = ysp_v[i * 16];\n"
    "    vec4 sz = ysp_v[i * 16 + 1];\n"
    /* an element: the template's anchor moved along its turned axes, the
     * box turned by ori and scaled by k about that anchor. Block 1: the
     * anchor in the pass's GL px, the center from the anchor in box px;
     * px per unit, the field mask, the palette's length. Block 2: the
     * palette. */
    "#ifdef YSP_INST\n"
    "    vec4 H = ysp_v[16], Q = ysp_v[17];\n"
    "    int m = int(Q.y);\n"
    "    float sg = ysp_axes.x, k = (m & 16) != 0 ? max(ysp_a1.y, 0.0) : 1.0;\n"
    "    vec2 cs = (m & 2) != 0 ? ysp_csd_(ysp_a0.z) : vec2(1.0, 0.0);\n"
    "    float c0 = xf.z, s0 = sg * xf.w, ce = c0 * cs.x - s0 * cs.y, se = s0 * cs.x + c0 * cs.y;\n"
    "    vec2 tx = xf.zw, ty = sg * vec2(-xf.w, xf.z), ex = vec2(ce, sg * se), ey = sg * vec2(-ex.y, ex.x);\n"
    "    vec2 A = H.xy + ((m & 1) != 0 ? Q.x * (ysp_a0.x * tx + ysp_a0.y * ty) : vec2(0.0));\n"
    "    xf = vec4(A + k * (H.z * ex + H.w * ey), ex);\n"
    "    sz = vec4(sz.xyz * k, sz.w);\n"
    "    ysp_k = k;\n"
    "    ysp_ixf = xf;\n"
    "    ysp_isize = sz;\n"
    "    float cf = (m & 8) != 0 ? ysp_a1.x : 1.0, gf = (m & 32) != 0 ? ysp_a1.z : 1.0;\n"
    "#ifdef YSP_VECTOR\n"   /* its block 2 is (opacity, gate, offset, band center) */
    "    ysp_ilook = ysp_v[2] * vec4(cf, gf, 1.0, 1.0);\n"
    "#else\n"
    "    ysp_ilook = ysp_v[2] * vec4(cf, cf, gf, 1.0);\n"
    "#endif\n"
    /* the palette: a fraction mixes two neighbors in linear light; an
     * integer gives its entry, bit for bit */
    "#ifdef YSP_INST_MOD\n"
    "    vec3 pc = ysp_v[4].rgb;\n"
    "#else\n"
    "    vec3 pc = ysp_v[3].rgb;\n"
    "#endif\n"
    "    if ((m & 64) != 0) {\n"
    "        int n = int(Q.z);\n"
    "        float u = clamp(ysp_a1.w, 0.0, float(n - 1));\n"
    "        int i0 = int(floor(u));\n"
    "        float f = u - float(i0);\n"
    "        pc = f > 0.0 ? mix(ysp_v[32 + i0].rgb, ysp_v[32 + min(i0 + 1, n - 1)].rgb, f) : ysp_v[32 + i0].rgb;\n"
    "    }\n"
    "#ifdef YSP_INST_MOD\n"
    "    ysp_icolor = ysp_v[3];\n"
    "    ysp_idir = vec4(pc, ysp_v[4].a);\n"
    "#else\n"
    "    ysp_icolor = vec4(pc, ysp_v[3].a);\n"
    "    ysp_idir = ysp_v[4];\n"
    "#endif\n"
    "    ysp_imisc = ysp_v[7] + vec4(0.0, (m & 4) != 0 ? ysp_a0.w : 0.0, 0.0, 0.0);\n"
    "#endif\n"
    "    vec2 c = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1)) * 2.0 - 1.0;\n"
    "    vec2 ax = xf.zw, ay = ysp_axes.x * vec2(-xf.w, xf.z);\n"
    "    vec2 local = c * (sz.xy + sz.ww);\n"
    "    vec2 o = vec2(0.0);\n"
    "#ifdef YSP_DOTS\n"
    "    o = ysp_dot * sz.z;\n"
    "    if (ysp_field_(o, ysp_v[i * 16 + 6]) > 0.0) local = vec2(1e9);\n"
    "#endif\n"
    /* a glyph's box from its atlas rectangle at the run's px per texel
     * (misc.y), from the run box's top-left */
    "#ifdef YSP_GLYPHS\n"
    "    vec2 gh = 0.5 * ysp_g1.xy * ysp_v[7].y;\n"
    "    o = ysp_g0.xy * sz.z + gh - sz.xy;\n"
    "    local = c * (gh + sz.ww);\n"
    "    ysp_gh = gh;\n"
    "    ysp_gr = vec4(ysp_g0.zw, ysp_g1.xy);\n"
    "#endif\n"
    "    ysp_c = xf.xy + o.x * ax + o.y * ay;\n"
    "    vec2 w = ysp_c + local.x * ax + local.y * ay;\n"
    "    gl_Position = vec4(w * ysp_target.zw * 2.0 - 1.0, 0.0, 1.0);\n"
    "    ysp_i = i;\n"
    /* the video program's constants once a vertex: indexed per fragment from
     * the uniform array, they cost 0.25 ms of 0.94 at 1080p (measured) */
    "#ifdef YSP_VIDEO\n"
    "    for (int k = 0; k < 7; k++) ysp_vk[k] = ysp_v[i * 16 + 8 + k];\n"
    "#endif\n"
    "}\n";

/* The fragment library: hash, SDFs, strokes, edge profiles, the aperture. */
static const char ygfx__glsl_fs_lib[] =
    "uniform sampler2D ysp_tex0;\n"
    "uniform sampler2D ysp_tex1;\n"
    "uniform sampler2D ysp_tex2;\n"
    "uniform usampler2D ysp_utex0;\n"
    "flat in vec2 ysp_c;\n"
    "flat in int ysp_i;\n"
    "#ifdef YSP_VIDEO\n"
    "flat in vec4 ysp_vk[7];\n"
    "#endif\n"
    "#ifdef YSP_GLYPHS\n"
    "flat in vec2 ysp_gh;\n"
    "flat in vec4 ysp_gr;\n"
    "#endif\n"
    "out vec4 ysp_out;\n"
    "vec2 ysp_p;\n"
    "const float ysp_PI = 3.14159265358979;\n"
    /* triple32 (Chris Wellons, hash-prospector, public domain): the
     * ygfx_hash() of the CPU, step for step (docs/gfx.md, v0.9) */
    "uint ysp_hash(uint x) {\n"
    "    x ^= x >> 17u; x *= 0xed5ad4bbu;\n"
    "    x ^= x >> 11u; x *= 0xac4c1b51u;\n"
    "    x ^= x >> 15u; x *= 0x31848babu;\n"
    "    x ^= x >> 14u;\n"
    "    return x;\n"
    "}\n"
    /* A 2D key: the point's index times an odd key of the seed, XOR a second
     * key, through one hash. Injective over any 65536 x 65536 window, and a
     * point's value is unrelated across seeds (docs/gfx.md, v0.9: the
     * nested form gave rows that were copies of each other) */
    "uint ysp_hash2(uvec2 c, uint seed) {\n"
    "    uint k = ysp_hash(seed), m = ysp_hash(k ^ 0x9E3779B9u);\n"
    "    return ysp_hash((c.x + (c.y << 16u)) * (k | 1u) ^ m);\n"
    "}\n"
    /* Bilinear by hand from four texelFetch, the indices clamped to the
     * rectangle sr (texels): no texel outside it can contribute, and the
     * weights are float on every renderer, not the sampler's 8-bit ones. */
    "vec4 ysp_bilinear(sampler2D t, vec2 x, vec4 sr) {\n"
    "    vec2 f = x - 0.5, i0 = floor(f), w = f - i0;\n"
    "    ivec2 lo = ivec2(sr.xy), hi = ivec2(sr.xy + sr.zw) - 1;\n"
    "    ivec2 a = clamp(ivec2(i0), lo, hi), b = clamp(ivec2(i0) + 1, lo, hi);\n"
    "    vec4 c00 = texelFetch(t, a, 0), c10 = texelFetch(t, ivec2(b.x, a.y), 0);\n"
    "    vec4 c01 = texelFetch(t, ivec2(a.x, b.y), 0), c11 = texelFetch(t, b, 0);\n"
    "    return mix(mix(c00, c10, w.x), mix(c01, c11, w.x), w.y);\n"
    "}\n"
    "#ifndef YSP_VECTOR\n"
    "float ysp_sd_box(vec2 p, vec2 b) {\n"
    "    vec2 d = abs(p) - b;\n"
    "    return length(max(d, 0.0)) + min(max(d.x, d.y), 0.0);\n"
    "}\n"
    /* A box with sharp corners: the max of its axis distances. Its corners
     * turn 90 degrees, so their miter ratio is sqrt 2: below that limit a
     * bevel plane per corner at the level a cuts them (as on a polygon). */
    "float ysp_mbox_(vec2 p, vec2 b, float L, float a) {\n"
    "    vec2 d = abs(p) - b;\n"
    "    float m = max(d.x, d.y);\n"
    "    return L < 1.41421356 ? max(m, (d.x + d.y) * 0.70710678 + a * 0.29289322) : m;\n"
    "}\n"
    "vec2 ysp_vtx_(int k) { vec4 v = ysp_v[ysp_i * 16 + 8 + k / 2]; return (k % 2 == 0) ? v.xy : v.zw; }\n"
    "float ysp_sd_polygon(vec2 p, int n) {\n"
    "    vec2 v0 = ysp_vtx_(0);\n"
    "    float d = dot(p - v0, p - v0), s = 1.0;\n"
    "    for (int k = 0, j = n - 1; k < n; j = k, k++) {\n"
    "        vec2 vi = ysp_vtx_(k), vj = ysp_vtx_(j), e = vj - vi, w = p - vi;\n"
    "        vec2 b = w - e * clamp(dot(w, e) / dot(e, e), 0.0, 1.0);\n"
    "        d = min(d, dot(b, b));\n"
    "        bvec3 c = bvec3(p.y >= vi.y, p.y < vj.y, e.x * w.y > e.y * w.x);\n"
    "        if (all(c) || all(not(c))) s = -s;\n"
    "    }\n"
    "    return s * sqrt(d);\n"
    "}\n"
    /* A convex polygon with sharp corners: the max of the edges' line
     * distances (unit gradients, so the edge profile keeps its width), and
     * past the miter limit L a bevel plane per corner whose level is the
     * bevel of the offset a. Only the bevels depend on the level, so one
     * loop over the edges gives both of a stroke's levels (a.x, a.y), not
     * two. o: the orientation's sign. */
    "vec2 ysp_sd_polygon_miter(vec2 p, int n, float o, float L, vec2 a) {\n"
    "    vec2 v = ysp_vtx_(0), e = v - ysp_vtx_(n - 1);\n"
    "    vec2 n1 = o * normalize(vec2(e.y, -e.x));\n"
    "    float d = -1e30;\n"
    "    vec2 bv = vec2(-1e30);\n"
    "    for (int k = 0; k < n; k++) {\n"
    "        vec2 vn = ysp_vtx_(k + 1 < n ? k + 1 : 0), e2 = vn - v;\n"
    "        vec2 n2 = o * normalize(vec2(e2.y, -e2.x)), w = p - v;\n"
    "        d = max(d, dot(w, n2));\n"
    "        vec2 b = normalize(n1 + n2);\n"
    "        float c = dot(b, n1);\n"
    "        if (c * L < 1.0) bv = max(bv, dot(w, b) + a * (1.0 - c));\n"
    "        v = vn; n1 = n2;\n"
    "    }\n"
    "    return max(vec2(d), bv);\n"
    "}\n"
    "#endif\n";

/* The library's second half (ISO C caps a string literal at 4095 bytes). */
static const char ygfx__glsl_fs_lib2[] =
    /* edge.x = shape: 0 rect, 1 circle, 2 annulus, 3 line, 4 polygon,
     * 5 cross, 6 none, 7 mask; shape = (p0, p1, n, stroke half width) in px;
     * dir.a = the miter limit (0: round), its sign the polygon's orientation.
     * a: the two offset levels asked for, which only a bevel needs. Both
     * levels come from one call, and the library has one call site: ANGLE's
     * D3D11 compiler inlines an SDF this size whole at every call site. */
    "#ifndef YSP_VECTOR\n"
    "vec2 ysp_sdf2_(vec2 p, vec2 a) {\n"
    "    int k = int(ysp_edge.x);\n"
    "    vec2 hs = ysp_size.xy;\n"
    "    vec4 sp = ysp_shape;\n"
    "    float ml = ysp_dir.a;\n"
    "    if (k == 0) {\n"
    "        if (ml != 0.0) return vec2(ysp_mbox_(p, hs, ml, a.x), ysp_mbox_(p, hs, ml, a.y));\n"
    "        float r = min(sp.y, min(hs.x, hs.y));\n"
    "        return vec2(ysp_sd_box(p, hs - r) - r);\n"
    "    }\n"
    "    if (k == 1) return vec2(length(p) - hs.x);\n"
    "    if (k == 2) return vec2(abs(length(p) - 0.5 * (hs.x + sp.x)) - 0.5 * (hs.x - sp.x));\n"
    "    if (k == 3) return vec2(length(vec2(max(abs(p.x) - (hs.x - hs.y), 0.0), p.y)) - hs.y);\n"
    "    if (k == 4) return ml != 0.0 ? ysp_sd_polygon_miter(p, int(sp.z), sign(ml), abs(ml), a)\n"
    "                                 : vec2(ysp_sd_polygon(p, int(sp.z)));\n"
    "    if (k == 5) {\n"
    "        vec2 b1 = vec2(hs.x, 0.5 * sp.x), b2 = vec2(0.5 * sp.x, hs.y);\n"
    "        if (ml != 0.0) return vec2(min(ysp_mbox_(p, b1, ml, a.x), ysp_mbox_(p, b2, ml, a.x)),\n"
    "                                   min(ysp_mbox_(p, b1, ml, a.y), ysp_mbox_(p, b2, ml, a.y)));\n"
    "        return vec2(min(ysp_sd_box(p, b1), ysp_sd_box(p, b2)));\n"
    "    }\n"
    "    if (k == 7) {\n"
    "#ifdef YSP_GLYPHS\n"
    "        hs = ysp_gh;\n"
    "        vec4 mr = ysp_gr;\n"
    "#else\n"
    "        vec4 mr = ysp_v[ysp_i * 16 + 15];\n"
    "#endif\n"
    "        vec2 r = vec2(p.x + hs.x, hs.y + p.y) / (2.0 * hs);\n"
    "        vec4 t = ysp_bilinear(ysp_tex1, mr.xy + r * mr.zw, mr);\n"
    "        if (sp.x < 1.5) return vec2(t.r * (2.0 * hs.x / mr.z));\n"
    "        float m = sp.z > 0.5 ? t.a : max(min(t.r, t.g), min(max(t.r, t.g), t.b));\n"
    "        return vec2((0.5 - m) * sp.y * (2.0 * hs.x / mr.z));\n"
    "    }\n"
    "    return vec2(-1e30);\n"
    "}\n"
    "float ysp_sdf_at(vec2 p, float a) { return ysp_sdf2_(p, vec2(a)).x; }\n"
    "float ysp_sdf(vec2 p) { return ysp_sdf2_(p, vec2(0.0)).x; }\n"
    "#endif\n"
    /* the fragment's point in the stimulus frame, from its exact center */
    "vec2 ysp_local_() {\n"
    "    vec2 d = gl_FragCoord.xy - ysp_c;\n"
    "    vec2 ax = ysp_xf.zw, ay = ysp_axes.x * vec2(-ysp_xf.w, ysp_xf.z);\n"
    "    return vec2(dot(d, ax), dot(d, ay));\n"
    "}\n"
    /* Abramowitz and Stegun 7.1.26, abs error below 1.5e-7 */
    "float ysp_erfc(float x) {\n"
    "    float z = abs(x);\n"
    "    float t = 1.0 / (1.0 + 0.3275911 * z);\n"
    "    float y = t * (0.254829592 + t * (-0.284496736 + t * (1.421413741 + t * (-1.453152027 + t * 1.061405429)))) * exp(-z * z);\n"
    "    return x >= 0.0 ? y : 2.0 - y;\n"
    "}\n"
    "float ysp_coverage(float d) {\n"
    "    int prof = int(ysp_edge.y);\n"
    "    float w = ysp_edge.z;\n"
    "    if (prof == 0 || w <= 0.0) return d <= 0.0 ? 1.0 : 0.0;\n"
    "    if (prof == 1) {\n"
    "        if (d <= -0.5 * w) return 1.0;\n"
    "        if (d >= 0.5 * w) return 0.0;\n"
    "        return 0.5 + 0.5 * cos(ysp_PI * (d + 0.5 * w) / w);\n"
    "    }\n"
    "    return 0.5 * ysp_erfc(d / (w * 1.4142135623731));\n"
    "}\n"
    /* A stroke is the band between the offsets a1 and a2 of the boundary:
     * the fill's coverage at a2 minus at a1, the hard band convolved with
     * the profile's kernel. look.w: the band's center offset. */
    "#ifndef YSP_VECTOR\n"
    "float ysp_aperture(vec2 p) {\n"
    "    int k = int(ysp_edge.x);\n"
    /* an element at scale ki: the template's field at p / ki, times ki;
     * the band and the offset are px, so they do not scale */
    "#ifdef YSP_INST\n"
    "    float ki = ysp_k;\n"
    "#else\n"
    "    const float ki = 1.0;\n"
    "#endif\n"
    "    if (k == 6) return all(lessThanEqual(abs(p), ki * ysp_v[ysp_i * 16 + 1].xy)) ? 1.0 : 0.0;\n"
    "    float hw = ysp_shape.w;\n"
    "    vec2 a = hw > 0.0 ? ysp_look.w + vec2(-hw, hw) : vec2(0.0);\n"
    /* SHAPE's offset (misc.x): the field at the level a + offset, so a
     * bevel is the offset outline's */
    "#ifdef YSP_OFFSET\n"
    "    vec2 d = ki * ysp_sdf2_(p / ki, (a + ysp_misc.x) / ki) - ysp_misc.x;\n"
    "#else\n"
    "    vec2 d = ki * ysp_sdf2_(p / ki, a / ki);\n"
    "#endif\n"
    "    if (hw <= 0.0) return ysp_coverage(d.x);\n"
    "    return ysp_coverage(d.y - a.y) - ysp_coverage(d.x - a.x);\n"
    "}\n"
    "#endif\n";

static const char ygfx__glsl_main_mod[] =
    "void main() {\n"
    "    ysp_p = ysp_local_();\n"
    "    ysp_umain_ = true;\n"
    "    float g = ysp_main(ysp_mainp_(ysp_p));\n"
    "    ysp_umain_ = false;\n"
    "    float a = ysp_aperture(ysp_p);\n"
    "    ysp_out = vec4(ysp_dir.rgb * (ysp_look.x * ysp_look.z * g * a), 0.0);\n"
    "}\n";

/* misc.z > 0.5: the body returns premultiplied color (a target). */
static const char ygfx__glsl_main_color[] =
    "void main() {\n"
    "    ysp_p = ysp_local_();\n"
    "    ysp_umain_ = true;\n"
    "    vec4 c = ysp_main(ysp_mainp_(ysp_p));\n"
    "    ysp_umain_ = false;\n"
    "    float k = ysp_aperture(ysp_p) * ysp_look.y * ysp_look.z;\n"
    "    ysp_out = ysp_misc.z > 0.5 ? c * k : vec4(c.rgb * (c.a * k), c.a * k);\n"
    "}\n";

static const char ygfx__glsl_main_add[] =
    "void main() {\n"
    "    ysp_p = ysp_local_();\n"
    "    ysp_umain_ = true;\n"
    "    vec3 c = ysp_main(ysp_mainp_(ysp_p));\n"
    "    ysp_umain_ = false;\n"
    "    ysp_out = vec4(c * (ysp_look.x * ysp_look.z * ysp_aperture(ysp_p)), 0.0);\n"
    "}\n";

/* Built-in bodies. misc = (sf cyc/px, phase, sigma px, aspect) for gratings
 * and gabors; (check px, seed, dist, 0) for noise; (channels, mode,
 * premultiplied, 0) for images, whose source rectangle is p[0..3] and tint
 * color. Local y points down the box, so a row from the top is hs.y + p.y. */
static const char ygfx__body_shape[] =
    "vec4 ysp_main(vec2 p) { return vec4(ysp_color.rgb, 1.0); }\n";
static const char ygfx__body_grating[] =
    "float ysp_main(vec2 p) {\n"
    "    float x = fract(ysp_misc.x * p.x + ysp_misc.y);\n"
    "    if (ysp_edge.w > 0.5) return (x < 0.25 || x >= 0.75) ? 1.0 : -1.0;\n"
    "    return cos(2.0 * ysp_PI * x);\n"
    "}\n";
static const char ygfx__body_gabor[] =
    "float ysp_main(vec2 p) {\n"
    "    float x = fract(ysp_misc.x * p.x + ysp_misc.y);\n"
    "    float ay = ysp_misc.w * p.y;\n"
    "    return cos(2.0 * ysp_PI * x) * exp(-(p.x * p.x + ay * ay) / (2.0 * ysp_misc.z * ysp_misc.z));\n"
    "}\n";
/* SIMPLEX (v0.10): 3D simplex noise in integers, so that the CPU's
 * ygfx_simplex_value() gives the same bits. Lattice coordinates are Q14
 * (1/16384 of a cell), biased by 2^28 so that every quantity is unsigned,
 * and every product stays below 2^31; the integer form costs 1.33 to 1.37
 * times f32 simplex per octave (docs/gfx.md, v0.10). Gradients: the 12
 * cube edges in 16 slots, by the corner's key's top 4 bits. */
static const char ygfx__body_noise[] =
    "const ivec3 ysp_gn_g_[16] = ivec3[16](ivec3(1,1,0),ivec3(-1,1,0),ivec3(1,-1,0),ivec3(-1,-1,0),ivec3(1,0,1),\n"
    "    ivec3(-1,0,1),ivec3(1,0,-1),ivec3(-1,0,-1),ivec3(0,1,1),ivec3(0,-1,1),ivec3(0,1,-1),ivec3(0,-1,-1),\n"
    "    ivec3(1,1,0),ivec3(-1,1,0),ivec3(0,-1,1),ivec3(0,-1,-1));\n"
    "uvec2 ysp_gn_layer_(uint z, uint S) { uint k = ysp_hash(z ^ S); return uvec2(k | 1u, ysp_hash(k ^ 0x9E3779B9u)); }\n"
    "int ysp_gn_(uvec3 v, uint S) {\n"
    "    uint sq = (v.x + v.y + v.z) / 3u;\n"
    "    uvec3 cell = (v + sq) >> 14u;\n"
    "    uint g3 = ((cell.x + cell.y + cell.z) << 14u) / 6u;\n"
    "    ivec3 x0 = ivec3(v - (cell << 14u) + g3);\n"
    "    ivec3 e = ivec3(greaterThanEqual(x0, x0.yzx));\n"
    "    ivec3 i1 = e * (1 - e.zxy), i2 = 1 - e.zxy * (1 - e);\n"
    "    uvec2 L0 = ysp_gn_layer_(cell.z, S), L1 = ysp_gn_layer_(cell.z + 1u, S);\n"
    "    ivec3 x[4], o[4];\n"
    "    x[0] = x0; x[1] = x0 - i1 * 16384 + 2731; x[2] = x0 - i2 * 16384 + 5461; x[3] = x0 - 8192;\n"
    "    o[0] = ivec3(0); o[1] = i1; o[2] = i2; o[3] = ivec3(1);\n"
    "    int sum = 0;\n"
    "    for (int k = 0; k < 4; k++) {\n"
    "        int t = 161061274 - (x[k].x * x[k].x + x[k].y * x[k].y + x[k].z * x[k].z);\n"
    "        if (t > 0) {\n"
    "            uvec3 cc = cell + uvec3(o[k]);\n"
    "            uvec2 L = o[k].z != 0 ? L1 : L0;\n"
    "            ivec3 gr = ysp_gn_g_[ysp_hash((cc.x + (cc.y << 16u)) * L.x ^ L.y) >> 28u];\n"
    "            t >>= 12; t = (t * t) >> 16; t = (t * t) >> 16;\n"
    "            sum += t * (gr.x * x[k].x + gr.y * x[k].y + gr.z * x[k].z);\n"
    "        }\n"
    "    }\n"
    "    return sum;\n"
    "}\n"
    /* block: p[0..7] each octave's Q14 lattice units per px, p[8..15] and
     * p[16..23] its z (Q14, mod 2^30) in two 16-bit halves, p[24..31] its
     * weight (sum 4096); misc.w the octaves. ysp_gn_() is the octave's
     * value times 2^25 (Q30 over 32) */
    "float ysp_gnoise_(vec2 p) {\n"
    "    uvec2 pi = uvec2(ivec2(floor(p + ysp_size.xy)));\n"
    "    uint S = ysp_hash(uint(ysp_misc.y));\n"
    "    int oc = int(ysp_misc.w), n = 0;\n"
    "    for (int o = 0; o < 8; o++) {\n"
    "        if (o >= oc) break;\n"
    "        uint r = uint(ysp_param(o));\n"
    "        uint z = (uint(ysp_param(8 + o)) << 16u) + uint(ysp_param(16 + o));\n"
    "        uvec3 v = uvec3(((2u * pi.x + 1u) * r) >> 1u, ((2u * pi.y + 1u) * r) >> 1u, z) + 0x10000000u;\n"
    "        n += (ysp_gn_(v, ysp_hash(S + uint(o))) >> 13) * int(ysp_param(24 + o));\n"
    "    }\n"
    "    return clamp(float(clamp(n, -16777215, 16777215)) * (1.0 / 16777216.0), -1.0, 1.0);\n"
    "}\n"
    "float ysp_main(vec2 p) {\n"
    "    if (ysp_misc.z > 2.5) return ysp_gnoise_(p);\n"
    "    vec2 q = vec2(p.x + ysp_size.x, ysp_size.y + p.y) / ysp_misc.x;\n"
    "    uvec2 c = uvec2(ivec2(floor(q)));\n"
    "    uint h = ysp_hash2(c, uint(ysp_misc.y));\n"
    /* GAUSSIAN: the hash's top 16 bits pick one of 65536 equally likely
     * quantiles, a table the CPU computed: no transcendental on the GPU, so
     * ygfx_noise_value() gives the same bits */
    "    if (ysp_misc.z > 1.5) return texelFetch(ysp_tex0, ivec2(int((h >> 16u) & 255u), int(h >> 24u)), 0).r;\n"
    "    if (ysp_misc.z > 0.5) return (h & 0x80000000u) != 0u ? 1.0 : -1.0;\n"
    "    return float(int(((h >> 9u) << 1u) + 1u) - 8388608) * (1.0 / 8388608.0);\n"
    "}\n";
/* The source rectangle's texel under p: nearest by texelFetch, clamped to
 * the rectangle; linear by hand, clamped the same way. */
static const char ygfx__body_image_fetch[] =
    /* A straight-alpha color image is filtered as premultiplied texels, then
     * divided back: a clear texel's rgb has no weight, so it cannot darken
     * its neighbors at the edges of a magnified sprite. */
    "#ifdef YSP_STRAIGHT\n"
    "vec4 ysp_bilinear_s_(vec2 x, vec4 sr, int ach) {\n"
    "    vec2 f = x - 0.5, i0 = floor(f), w = f - i0;\n"
    "    ivec2 lo = ivec2(sr.xy), hi = ivec2(sr.xy + sr.zw) - 1;\n"
    "    ivec2 a = clamp(ivec2(i0), lo, hi), b = clamp(ivec2(i0) + 1, lo, hi);\n"
    "    vec4 c00 = texelFetch(ysp_tex0, a, 0), c10 = texelFetch(ysp_tex0, ivec2(b.x, a.y), 0);\n"
    "    vec4 c01 = texelFetch(ysp_tex0, ivec2(a.x, b.y), 0), c11 = texelFetch(ysp_tex0, b, 0);\n"
    "    vec4 q = mix(mix(vec4(c00.rgb * c00[ach], c00[ach]), vec4(c10.rgb * c10[ach], c10[ach]), w.x),\n"
    "                 mix(vec4(c01.rgb * c01[ach], c01[ach]), vec4(c11.rgb * c11[ach], c11[ach]), w.x), w.y);\n"
    "    return vec4(q.a > 0.0 ? q.rgb / q.a : vec3(0.0), q.a);\n"
    "}\n"
    "#endif\n"
    "vec4 ysp_texel_(vec2 p) {\n"
    "    int mode = int(ysp_misc.y);\n"
    "    vec2 r = vec2(p.x + ysp_size.x, ysp_size.y + p.y) / (2.0 * ysp_size.xy);\n"
    "    vec4 sr = ysp_v[ysp_i * 16 + 8];\n"
    "    ivec2 t = ivec2(sr.xy) + clamp(ivec2(floor(r * sr.zw)), ivec2(0), ivec2(sr.zw) - 1);\n"
    "    int ch = int(ysp_misc.x);\n"
    "    if (mode == 2) return vec4(vec3(float(texelFetch(ysp_utex0, t, 0).r) * (1.0 / 65535.0)), 1.0);\n"
    "#ifdef YSP_STRAIGHT\n"
    "    if (mode == 1 && ch != 1 && ysp_misc.z < 0.5) {\n"
    "        vec4 s = ysp_bilinear_s_(sr.xy + r * sr.zw, sr, ch == 2 ? 1 : 3);\n"
    "        return ch == 2 ? vec4(s.rrr, s.a) : s;\n"
    "    }\n"
    "#endif\n"
    "    vec4 v = mode == 1 ? ysp_bilinear(ysp_tex0, sr.xy + r * sr.zw, sr) : texelFetch(ysp_tex0, t, 0);\n"
    "    if (ch == 1) return vec4(v.rrr, 1.0);\n"
    "    if (ch == 2) return vec4(v.rrr, v.g);\n"
    "    return v;\n"
    "}\n";
/* Tint: linear rgb and alpha factors, in color. */
static const char ygfx__body_image_color[] =
    "vec4 ysp_main(vec2 p) {\n"
    "    vec4 c = ysp_texel_(p);\n"
    "#ifndef YSP_VIDEO\n"
    "    if (ysp_misc.w > 0.5) { float a = c.r * ysp_color.a; return ysp_misc.z > 0.5 ? vec4(ysp_color.rgb * a, a) : vec4(ysp_color.rgb, a); }\n"
    "#endif\n"
    "    return ysp_misc.z > 0.5 ? c * vec4(ysp_color.rgb * ysp_color.a, ysp_color.a)\n"
    "                            : vec4(c.rgb * ysp_color.rgb, c.a * ysp_color.a);\n"
    "}\n";
/* The tint is folded into dir on the CPU. */
static const char ygfx__body_image_mod[] =
    "float ysp_main(vec2 p) { return ysp_texel_(p).r; }\n";
static const char ygfx__body_image_add[] =
    "vec3 ysp_main(vec2 p) { return ysp_texel_(p).rgb * ysp_color.rgb * ysp_color.a; }\n";
/* A dot: a circle of radius size.x with the stimulus's edge. */
static const char ygfx__body_dots[] =
    "vec4 ysp_main(vec2 p) { return vec4(ysp_color.rgb, 1.0); }\n";

/* The vector program (v0.3): SDF primitives with gradients and arc
 * lengths, the compound fold, dashes, trim, paint and effects. Its own
 * main; the CPU form is ygfx__vfield() and the functions it calls.
 * Primitives from Inigo Quilez's 2D distance functions (MIT) and
 * 0xfaded's ellipse (MIT): the notices are at the end of the file. */

static const char ygfx__glsl_vec0[] =
    /* A specialized program's switches (ygfx__vspecs); none set is the
     * generic program. YSP_VK: the kinds, bit k for kind k; YSP_ONE: one
     * kind, so no test; YSP_V1: one primitive, no fold. */
    "#ifndef YSP_VK\n#define YSP_VK 0x1FFFF\n#endif\n"
    "#ifndef YSP_ONE\n#define YSP_ONE 0\n#endif\n"
    "#ifndef YSP_ALONG\n#define YSP_ALONG 1\n#endif\n"
    "#ifndef YSP_PAINT\n#define YSP_PAINT 1\n#endif\n"
    "#ifndef YSP_FX\n#define YSP_FX 1\n#endif\n"
    "vec4 ysp_vd_(int k) { return ysp_v[ysp_i * 16 + 9 + k]; }\n"
    "const float ysp_TAU = 6.28318530717959;\n"
    "float ysp_ang_(vec2 q) { if (q.x == 0.0 && q.y == 0.0) return 0.0; float a = atan(q.y, q.x); return a < 0.0 ? a + ysp_TAU : a; }\n"
    "float ysp_wrap_(float a) { return a - ysp_TAU * floor(a / ysp_TAU); }\n"
    "vec2 ysp_nrm_(vec2 v, vec2 f) { float l = length(v); return l > 1e-20 ? v / l : f; }\n"
    "float ysp_sgn_(float x) { return x >= 0.0 ? 1.0 : -1.0; }\n"
    "vec4 ysp_p_circle_(vec2 q, float R, bool ws) {\n"
    "    return vec4(length(q) - R, ysp_nrm_(q, vec2(1.0, 0.0)), ws ? R * ysp_ang_(q) : 0.0);\n"
    "}\n"
    "vec4 ysp_p_annulus_(vec2 q, float R, float ri, bool ws) {\n"
    "    float l = length(q), sg = ysp_sgn_(l - 0.5 * (R + ri));\n"
    "    float s = !ws ? 0.0 : (sg > 0.0 ? R * ysp_ang_(q) : ysp_TAU * R + ri * ysp_ang_(q));\n"
    "    return vec4(abs(l - 0.5 * (R + ri)) - 0.5 * (R - ri), sg * ysp_nrm_(q, vec2(1.0, 0.0)), s);\n"
    "}\n"
    /* Inigo Quilez, sdBox with its gradient (iquilezles.org, "2D distance
     * functions"; MIT) */
    "vec4 ysp_box_(vec2 q, vec2 b) {\n"
    "    vec2 w = abs(q) - b, sg = vec2(ysp_sgn_(q.x), ysp_sgn_(q.y));\n"
    "    if (w.x > 0.0 && w.y > 0.0) { float l = length(w); return vec4(l, sg * w / l, 0.0); }\n"
    "    return w.x >= w.y ? vec4(w.x, sg.x, 0.0, 0.0) : vec4(w.y, 0.0, sg.y, 0.0);\n"
    "}\n"
    /* after Inigo Quilez, sdRoundBox (Shadertoy 4llXD7; MIT): a radius per
     * corner; the gradient and arc length are ours */
    "vec4 ysp_p_rrect_(vec2 q, vec2 b, vec4 r, bool ws) {\n"
    "    bool rt = q.x > 0.0, bt = q.y > 0.0;\n"
    "    float rr = rt ? (bt ? r.z : r.y) : (bt ? r.w : r.x);\n"
    "    vec2 sg = vec2(rt ? 1.0 : -1.0, bt ? 1.0 : -1.0), w = abs(q) - b + rr;\n"
    "    bool cn = w.x > 0.0 && w.y > 0.0;\n"
    "    vec4 e;\n"
    "    if (cn) { float l = length(w); e = vec4(l - rr, sg * w / l, 0.0); }\n"
    "    else if (w.x >= w.y) e = vec4(w.x - rr, sg.x, 0.0, 0.0);\n"
    "    else e = vec4(w.y - rr, 0.0, sg.y, 0.0);\n"
    "    if (ws) {\n"
    "        float h = 1.5707963267949, lt = 2.0 * b.x - r.x - r.y, lr = 2.0 * b.y - r.y - r.z;\n"
    "        float lb = 2.0 * b.x - r.z - r.w, ll = 2.0 * b.y - r.w - r.x;\n"
    "        float sR = lt + h * r.y, sB = sR + lr + h * r.z, sL = sB + lb + h * r.w;\n";
static const char ygfx__glsl_vec0b[] =   /* C99 caps a literal at 4095 characters */
    "        if (cn) {\n"
    "            if (rt && !bt) e.w = lt + rr * atan(w.x, w.y);\n"
    "            else if (rt) e.w = sR + lr + rr * atan(w.y, w.x);\n"
    "            else if (bt) e.w = sB + lb + rr * atan(w.x, w.y);\n"
    "            else e.w = sL + ll + rr * atan(w.y, w.x);\n"
    "        } else if (w.x >= w.y) {\n"
    "            e.w = rt ? sR + clamp(q.y + b.y - r.y, 0.0, lr) : sL + clamp(b.y - r.w - q.y, 0.0, ll);\n"
    "        } else {\n"
    "            e.w = bt ? sB + clamp(b.x - r.z - q.x, 0.0, lb) : clamp(q.x + b.x - r.x, 0.0, lt);\n"
    "        }\n"
    "    }\n"
    "    return e;\n"
    "}\n"
    /* after Inigo Quilez, sdUnevenCapsule (Shadertoy 4lcBWn; MIT) */
    "vec4 ysp_p_capsule_(vec2 q, float hx, float r0, float r1, bool ws) {\n"
    "    float x1 = r0 - hx, D = 2.0 * hx - r0 - r1, sn = (r0 - r1) / D, cs = sqrt(max(1.0 - sn * sn, 0.0));\n"
    "    float al = q.x - x1, k = -sn * abs(q.y) + cs * al;\n"
    "    vec4 e;\n"
    "    if (k < 0.0) { vec2 v = vec2(al, q.y); e = vec4(length(v) - r0, ysp_nrm_(v, vec2(-1.0, 0.0)), 0.0); }\n"
    "    else if (k > cs * D) { vec2 v = vec2(al - D, q.y); e = vec4(length(v) - r1, ysp_nrm_(v, vec2(1.0, 0.0)), 0.0); }\n"
    "    else e = vec4(cs * abs(q.y) + sn * al - r0, sn, ysp_sgn_(q.y) * cs, 0.0);\n"
    "    if (ws) e.w = clamp(al, 0.0, D);\n"
    "    return e;\n"
    "}\n"
    "float ysp_sweep_(float pv, float pu, float sw) {\n"
    "    return clamp(0.5 * sw + ((pv == 0.0 && pu == 0.0) ? 0.0 : atan(pv, pu)), 0.0, sw);\n"
    "}\n"
    /* after Inigo Quilez, sdArc (Shadertoy wl23RK; MIT); the trig comes from
     * the CPU, as D3D's sin and cos are coarse */
    "vec4 ysp_p_arc_(vec2 q, vec2 u, float ra, float rb, float sw, vec2 sc, bool ws) {\n"
    "    vec2 v = vec2(-u.y, u.x);\n"
    "    float pv = dot(q, v), d;\n"
    "    vec2 p = vec2(abs(pv), dot(q, u)), gi;\n"
    "    if (sc.y * p.x > sc.x * p.y) { vec2 w = p - sc * ra; d = length(w) - rb; gi = ysp_nrm_(w, sc); }\n"
    "    else { float l = length(p); d = abs(l - ra) - rb; gi = ysp_sgn_(l - ra) * ysp_nrm_(p, vec2(0.0, 1.0)); }\n"
    "    float s = ws ? ra * ysp_sweep_(pv, p.y, sw) : 0.0;\n"
    "    return vec4(d, gi.x * ysp_sgn_(pv) * v + gi.y * u, s);\n"
    "}\n"
    /* after Inigo Quilez, sdPie (Shadertoy 3l23RK; MIT) */
    "vec4 ysp_p_pie_(vec2 q, float R, vec2 u, float sw, vec2 c, bool ws) {\n"
    "    vec2 v = vec2(-u.y, u.x);\n"
    "    float pv = dot(q, v);\n"
    "    vec2 p = vec2(abs(pv), dot(q, u));\n"
    "    float l = length(p) - R, pr = clamp(dot(p, c), 0.0, R);\n"
    "    vec2 mv = p - c * pr, gi;\n";

static const char ygfx__glsl_vec1[] =
    "    float sg = ysp_sgn_(c.y * p.x - c.x * p.y), mm = length(mv) * sg, d;\n"
    "    bool arc = l > mm;\n"
    "    if (arc) { d = l; gi = ysp_nrm_(p, vec2(0.0, 1.0)); } else { d = mm; gi = sg * ysp_nrm_(mv, vec2(c.y, -c.x)); }\n"
    "    float s = 0.0;\n"
    "    if (ws) s = arc ? R + R * ysp_sweep_(pv, p.y, sw) : (pv < 0.0 ? pr : 2.0 * R + R * sw - pr);\n"
    "    return vec4(d, gi.x * ysp_sgn_(pv) * v + gi.y * u, s);\n"
    "}\n"
    /* after Inigo Quilez, sdStar (Shadertoy 3tSGDy; MIT): the sector fold;
     * the fold by a complex power and the reflection guard against D3D's
     * coarse atan, and the fillets, are ours */
    "vec4 ysp_p_star_(vec2 q, vec4 C, vec4 D, bool ws) {\n"
    "    float R = C.x, n = C.y, rho = C.z, rf = C.w, an = ysp_PI / n;\n"
    "    float al = (q.x == 0.0 && q.y == 0.0) ? 0.0 : atan(q.x, q.y), kf = floor((al + an) / (2.0 * an));\n"
    "    vec2 Th = D.xy, z = vec2(Th.x * Th.x - Th.y * Th.y, 2.0 * Th.x * Th.y), w2 = vec2(1.0, 0.0);\n"
    "    for (int kk = int(abs(kf)); kk > 0; kk /= 2) {\n"
    "        if ((kk & 1) != 0) w2 = vec2(w2.x * z.x - w2.y * z.y, w2.x * z.y + w2.y * z.x);\n"
    "        z = vec2(z.x * z.x - z.y * z.y, 2.0 * z.x * z.y);\n"
    "    }\n"
    "    if (kf < 0.0) w2.y = -w2.y;\n"
    "    float fys = dot(q, vec2(w2.x, -w2.y)), sb = ysp_sgn_(fys);\n"
    "    vec2 f = vec2(dot(q, w2.yx), abs(fys));\n"
    "    bool rfl = f.x * Th.y - f.y * Th.x < 0.0;\n"
    "    if (rfl) f = 2.0 * dot(f, Th) * Th - f;\n"
    "    vec2 I = vec2(R * rho, 0.0), T = R * Th, e = normalize(T - I), ne = vec2(e.y, -e.x);\n"
    "    vec2 PI = I, PT = T, gf = ne;\n"
    "    float best = 1e30, dv = 0.0, sh = 0.0, lI = D.w, H = D.z;\n"
    "    if (rf > 0.0) {\n"
    "        float cb = dot(e, Th), sbe = abs(e.x * Th.y - e.y * Th.x);\n"
    "        vec2 CT = T - Th * (rf / sbe);\n"
    "        PT = T - e * (rf * cb / sbe);\n"
    "        if (abs(e.x) > 1e-5) {\n"
    "            float cv = e.x > 0.0 ? -1.0 : 1.0;\n"
    "            vec2 CI = I - vec2(cv * rf / abs(e.y), 0.0);\n"
    "            PI = I + e * (rf * abs(e.x) / abs(e.y));\n"
    "            vec2 w = f - CI, a1 = vec2(cv, 0.0), a2 = (PI - CI) / rf;\n"
    "            float cr = a1.x * a2.y - a1.y * a2.x, lw = length(w);\n"
    "            if ((a1.x * w.y - a1.y * w.x) * cr >= -1e-5 * lw && (w.x * a2.y - w.y * a2.x) * cr >= -1e-5 * lw && lw > 0.0) {\n"
    "                dv = cv * (lw - rf); best = abs(dv); gf = cv * w / lw;\n"
    "                sh = rf * abs(atan(a1.x * w.y - a1.y * w.x, dot(a1, w)));\n"
    "            }\n"
    "        }\n"
    "        vec2 w = f - CT;\n"
    "        float lw = length(w), cr = ne.x * Th.y - ne.y * Th.x;\n"
    "        if ((ne.x * w.y - ne.y * w.x) * cr >= -1e-5 * lw && (w.x * Th.y - w.y * Th.x) * cr >= -1e-5 * lw && lw > 0.0 && abs(lw - rf) < best) {\n"
    "            dv = lw - rf; best = abs(dv); gf = w / lw;\n"
    "            sh = lI + length(PT - PI) + rf * abs(atan(ne.x * w.y - ne.y * w.x, dot(ne, w)));\n"
    "        }\n"
    "    }\n"
    "    float LP = length(PT - PI), t = clamp(dot(f - PI, e), 0.0, LP);\n"
    "    vec2 w = f - PI - e * t;\n"
    "    float l = length(w);\n"
    "    if (l < best) { float sg = dot(w, ne) >= 0.0 ? 1.0 : -1.0; dv = sg * l; gf = l > 0.0 ? sg * w / l : ne; sh = lI + t; }\n"
    "    if (rfl) { gf = 2.0 * dot(gf, Th) * Th - gf; sh = 2.0 * H - sh; }\n"
    "    vec2 g = gf.x * w2.yx + gf.y * sb * vec2(w2.x, -w2.y);\n"
    "    float s = 0.0;\n"
    "    if (ws) { float P = 2.0 * H; s = mod(n - kf, n) * P + (sb < 0.0 ? sh : -sh); s -= n * P * floor(s / (n * P)); }\n"
    "    return vec4(dv, g, s);\n"
    "}\n"
    "const float ysp_GLX[8] = float[8](0.0950125098376374, 0.2816035507792589, 0.4580167776572274, 0.6178762444026438,\n"
    "                                  0.7554044083550030, 0.8656312023878318, 0.9445750230732326, 0.9894009349916499);\n"
    "const float ysp_GLW[8] = float[8](0.1894506104550685, 0.1826034150449236, 0.1691565193950025, 0.1495959888165767,\n"
    "                                  0.1246289712555339, 0.0951585116824928, 0.0622535239386479, 0.0271524594117541);\n"
    /* 0xfaded (Carl Chatfield), the trig-free closest point on an ellipse
     * (github.com/0xfaded/ellipse_demo; MIT) */
    "vec4 ysp_p_ellipse_(vec2 q, vec2 ab, float it, float Lq, bool ws) {\n"
    "    vec2 p = abs(q), t = vec2(0.70710678118654752);\n"
    "    vec2 k2 = vec2(ab.x * ab.x - ab.y * ab.y, ab.y * ab.y - ab.x * ab.x) / ab;\n"
    "    for (int i = 0; i < int(it); i++) {\n"
    "        vec2 ev = k2 * t * t * t, r = ab * t - ev, qq = p - ev;\n"
    "        t = clamp((qq * (length(r) / max(length(qq), 1e-20)) + ev) / ab, 0.0, 1.0);\n";

static const char ygfx__glsl_vec2[] =
    "        t /= max(length(t), 1e-20);\n"
    "    }\n"
    "    vec2 w = p - ab * t;\n"
    "    float sg = dot(p / ab, p / ab) < 1.0 ? -1.0 : 1.0;\n"
    "    vec2 g = sg * ysp_nrm_(w, sg * normalize(t / ab)) * vec2(ysp_sgn_(q.x), ysp_sgn_(q.y));\n"
    "    float s = 0.0;\n"
    "    if (ws) {\n"
    "        float t1 = atan(t.y, t.x), h = 0.5 * t1, S = 0.0;\n"
    "        for (int i = 0; i < int(ysp_axes.y); i++) {\n"
    "            float x = ysp_GLX[i / 2] * (i % 2 == 0 ? 1.0 : -1.0), u = h * (1.0 + x);\n"
    "            S += ysp_GLW[i / 2] * length(ab * vec2(sin(u), cos(u)));\n"
    "        }\n"
    "        S *= h;\n"
    "        s = q.x >= 0.0 ? (q.y >= 0.0 ? S : 4.0 * Lq - S) : (q.y >= 0.0 ? 2.0 * Lq - S : 2.0 * Lq + S);\n"
    "    }\n"
    "    return vec4(sg * length(w), g, s);\n"
    "}\n"
    "vec4 ysp_p_polygon_(vec2 q, int g0, int n, bool ws) {\n"
    "    float best = 1e30, dv = 0.0, s = 0.0, par = 1.0, o = ysp_vd_(g0 + 2).y;\n"
    "    bool sharp = abs(o) > 1.5;\n"
    "    o = ysp_sgn_(o);\n"
    "    vec2 gb = vec2(1.0, 0.0);\n"
    "    for (int i = 0; i < n; i++) {\n"
    "        int j = i + 1 < n ? i + 1 : 0;\n"
    "        vec4 A = ysp_vd_(g0 + 3 * i), B = ysp_vd_(g0 + 3 * i + 1), C = ysp_vd_(g0 + 3 * i + 2), An = ysp_vd_(g0 + 3 * j);\n"
    "        if (B.z != 0.0) {\n"
    "            float r = abs(B.z), cv = ysp_sgn_(B.z);\n"
    "            vec2 w = q - B.xy, a1 = (A.xy - B.xy) / r, a2 = (A.zw - B.xy) / r;\n"
    "            float cr = a1.x * a2.y - a1.y * a2.x, lw = length(w);\n"
    "            if ((a1.x * w.y - a1.y * w.x) * cr >= -1e-5 * lw && (w.x * a2.y - w.y * a2.x) * cr >= -1e-5 * lw && lw > 0.0 && abs(lw - r) < best) {\n"
    "                dv = cv * (lw - r); best = abs(dv); gb = ysp_sgn_(lw - r) * w / lw;\n"
    "                s = B.w + r * abs(atan(a1.x * w.y - a1.y * w.x, dot(a1, w)));\n"
    "            }\n"
    "        }\n"
    "        vec2 ev = An.xy - A.zw;\n"
    "        float le = length(ev);\n"
    "        vec2 e = ev / max(le, 1e-20), ne = o * vec2(e.y, -e.x);\n"
    "        float t = clamp(dot(q - A.zw, e), 0.0, le);\n"
    "        vec2 w = q - A.zw - e * t;\n"
    "        float l = length(w);\n"
    "        if (l < best) { float sg = dot(w, ne) >= 0.0 ? 1.0 : -1.0; best = l; dv = sg * l; gb = l > 0.0 ? w / l : ne; s = C.x + t; }\n"
    "        if (sharp) {\n"
    "            vec2 vi = C.zw, vj = ysp_vd_(g0 + 3 * j + 2).zw, ee = vj - vi, ww = q - vi;\n"
    "            bvec3 c = bvec3(q.y >= vi.y, q.y < vj.y, ee.x * ww.y > ee.y * ww.x);\n"
    "            if (all(c) || all(not(c))) par = -par;\n"
    "        }\n"
    "    }\n"
    "    if (sharp) { dv = par * best; gb *= par; }\n"
    "    else gb *= ysp_sgn_(dv);\n"
    "    return vec4(dv, gb, ws ? s : 0.0);\n"
    "}\n"
    "vec3 ysp_seg2_(vec2 p, vec2 a, vec2 b, vec3 best) {\n"
    "    vec2 e = b - a, q = p - a, r = q - e * clamp(dot(q, e) / max(dot(e, e), 1e-20), 0.0, 1.0);\n"
    "    return dot(r, r) < best.x ? vec3(dot(r, r), r) : best;\n"
    "}\n"
    "vec4 ysp_cpoly_(vec2 p, vec2 a, vec2 b, vec2 c, vec2 d) {\n"
    "    vec3 best = ysp_seg2_(p, a, b, vec3(1e30, 0.0, 0.0));\n"
    "    best = ysp_seg2_(p, b, c, best);\n"
    "    best = ysp_seg2_(p, c, d, best);\n"
    "    best = ysp_seg2_(p, d, a, best);\n"
    "    float ar = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x) + (c.x - a.x) * (d.y - a.y) - (c.y - a.y) * (d.x - a.x);\n"
    "    bool ins = ar * ((b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x)) >= 0.0 && ar * ((c.x - b.x) * (p.y - b.y) - (c.y - b.y) * (p.x - b.x)) >= 0.0 &&\n"
    "               ar * ((d.x - c.x) * (p.y - c.y) - (d.y - c.y) * (p.x - c.x)) >= 0.0 && ar * ((a.x - d.x) * (p.y - d.y) - (a.y - d.y) * (p.x - d.x)) >= 0.0;\n"
    "    float l = sqrt(best.x), sg = ins ? -1.0 : 1.0;\n"
    "    return vec4(sg * l, l > 0.0 ? sg * best.yz / l : vec2(0.0), 0.0);\n"
    "}\n"
    "vec4 ysp_p_polyline_(vec2 p, int g0, int n, float hw, int join, float lim, int cap, bool ws) {\n"
    "    vec4 best = vec4(1e30, 1.0, 0.0, 0.0);\n"
    "    float bh = 0.5;\n"
    "    int bi = 0;\n"
    "    for (int i = 0; i < n - 1; i++) {\n"
    "        vec4 A = ysp_vd_(g0 + i), B = ysp_vd_(g0 + i + 1);\n"
    "        vec2 tv = B.xy - A.xy;\n"
    "        float len = length(tv);\n"
    "        vec2 t = tv / max(len, 1e-20), nv = vec2(-t.y, t.x), r = p - A.xy;\n"
    "        float al = dot(r, t);\n";

static const char ygfx__glsl_vec3[] =
    "        if (join == 0) {   /* round joins: the distance to the center line, exact */\n"
    "            float h = clamp(al, 0.0, len);\n"
    "            vec2 w = r - t * h;\n"
    "            float l = length(w);\n"
    "            if (l - hw < best.x) { best = vec4(l - hw, ysp_nrm_(w, nv), A.z + h); bh = al / max(len, 1e-20); bi = i; }\n"
    "            continue;\n"
    "        }\n"
    "        float e0 = (i == 0 && cap == 2) ? hw : 0.0, e1 = (i == n - 2 && cap == 2) ? hw : 0.0;\n"
    "        vec4 bx = ysp_box_(vec2(al - 0.5 * (len + e1 - e0), dot(r, nv)), vec2(0.5 * (len + e0 + e1), hw));\n"
    "        if (bx.x < best.x) best = vec4(bx.x, bx.y * t + bx.z * nv, A.z + clamp(al, 0.0, len));\n"
    "        if (i < n - 2) {\n"
    "            vec2 t2 = normalize(ysp_vd_(g0 + i + 2).xy - B.xy);\n"
    "            float cr = t.x * t2.y - t.y * t2.x;\n"
    "            if (abs(cr) > 1e-6 || dot(t, t2) < 0.0) {\n"
    "                float sd = cr > 0.0 ? -1.0 : 1.0;\n"
    "                vec2 na = sd * nv, nb = sd * vec2(-t2.y, t2.x), m = na + nb, Pa = B.xy + na * hw, Pb = B.xy + nb * hw;\n"
    "                float mm = dot(m, m);\n"
    "                bool mit = join == 1 && mm > 1e-12 && 2.0 / sqrt(mm) <= lim;\n"
    "                vec4 jp = ysp_cpoly_(p, B.xy, Pa, mit ? B.xy + m * (2.0 * hw / mm) : Pb, Pb);\n"
    "                if (jp.x < best.x) best = vec4(jp.xyz, B.z);\n"
    "            }\n"
    "        }\n"
    "    }\n"
    "    if (join == 0 && cap != 1 && ((bi == 0 && bh <= 0.0) || (bi == n - 2 && bh >= 1.0))) {\n"
    "        /* a butt or square end: the half strip past the end segment */\n"
    "        bool st = bi == 0 && bh <= 0.0;\n"
    "        vec4 E = ysp_vd_(g0 + (st ? 0 : n - 1)), F = ysp_vd_(g0 + (st ? 1 : n - 2));\n"
    "        vec2 to = normalize(E.xy - F.xy), nn = vec2(-to.y, to.x), r = p - E.xy;\n"
    "        float ac = dot(r, nn);\n"
    "        vec2 qd = vec2(dot(r, to) - (cap == 2 ? hw : 0.0), abs(ac) - hw);\n"
    "        vec2 gg = (qd.x > 0.0 && qd.y > 0.0) ? normalize(qd) : (qd.x >= qd.y ? vec2(1.0, 0.0) : vec2(0.0, 1.0));\n"
    "        best.x = length(max(qd, 0.0)) + min(max(qd.x, qd.y), 0.0);\n"
    "        best.yz = gg.x * to + gg.y * ysp_sgn_(ac) * nn;\n"
    "    }\n"
    "    if (join != 0 && cap == 1)\n"
    "        for (int k = 0; k < 2; k++) {\n"
    "            vec4 E = ysp_vd_(g0 + (k == 0 ? 0 : n - 1));\n"
    "            vec2 v = p - E.xy;\n"
    "            float l = length(v);\n"
    "            if (l - hw < best.x) best = vec4(l - hw, ysp_nrm_(v, vec2(1.0, 0.0)), E.z);\n";
static const char ygfx__glsl_vec3b[] =   /* C99 caps a literal at 4095 characters */
    "        }\n"
    "    if (!ws) best.w = 0.0;\n"
    "    return best;\n"
    "}\n"
    "vec4 ysp_prim_(vec2 q, vec4 B, vec4 C, vec4 D, bool ws) {\n"
    "    int k = int(B.x);\n"
    "#if (YSP_VK & 0x2) != 0\n"
    "    if (YSP_ONE != 0 || k == 1) return ysp_p_circle_(q, C.x, ws);\n"
    "#endif\n"
    "#if (YSP_VK & 0x4) != 0\n"
    "    if (YSP_ONE != 0 || k == 2) return ysp_p_annulus_(q, C.x, C.z, ws);\n"
    "#endif\n"
    "#if (YSP_VK & 0x100) != 0\n"
    "    if (YSP_ONE != 0 || k == 8) return ysp_p_rrect_(q, C.xy, vec4(C.zw, D.xy), ws);\n"
    "#endif\n"
    "#if (YSP_VK & 0x800) != 0\n"
    "    if (YSP_ONE != 0 || k == 11) return ysp_p_capsule_(q, C.x, C.z, C.w, ws);\n"
    "#endif\n"
    "#if (YSP_VK & 0x200) != 0\n"
    "    if (YSP_ONE != 0 || k == 9) return ysp_p_arc_(q, C.xy, C.z, C.w, D.x, D.zw, ws);\n"
    "#endif\n"
    "#if (YSP_VK & 0x400) != 0\n"
    "    if (YSP_ONE != 0 || k == 10) return ysp_p_pie_(q, C.x, C.yz, D.x, D.zw, ws);\n"
    "#endif\n"
    "#if (YSP_VK & 0x2000) != 0\n"
    "    if (YSP_ONE != 0 || k == 13) return ysp_p_star_(q, C, D, ws);\n"
    "#endif\n"
    "#if (YSP_VK & 0x4000) != 0\n"
    "    if (YSP_ONE != 0 || k == 14) return ysp_p_ellipse_(q, C.xy, C.z, C.w, ws);\n"
    "#endif\n"
    "#if (YSP_VK & 0x20) != 0\n"
    "    if (YSP_ONE != 0 || k == 5) { vec4 a = ysp_box_(q, vec2(C.x, 0.5 * C.z)), b = ysp_box_(q, vec2(0.5 * C.z, C.y)); return a.x < b.x ? a : b; }\n"
    "#endif\n"
    "#if (YSP_VK & 0x10) != 0\n"
    "    if (YSP_ONE != 0 || k == 4) return ysp_p_polygon_(q, int(D.z), int(D.w), ws);\n"
    "#endif\n"
    "#if (YSP_VK & 0x10000) != 0\n"
    "    if (YSP_ONE != 0 || k == 16) return ysp_p_polyline_(q, int(D.z), int(D.w), C.z, int(C.w), D.x, int(D.y), ws);\n"
    "#endif\n"
    "    return vec4(1e30, 1.0, 0.0, 0.0);\n"
    "}\n"
    /* Inigo Quilez, the quadratic polynomial smooth minimum
     * (iquilezles.org, "smooth minimum"; MIT), with its gradient */
    "vec4 ysp_smin_(vec4 a, vec4 b, float k) {\n"
    "    float h = clamp(0.5 + 0.5 * (b.x - a.x) / k, 0.0, 1.0);\n"
    "    return vec4(mix(b.x, a.x, h) - k * h * (1.0 - h), mix(b.yz, a.yz, h), h > 0.5 ? a.w : b.w);\n"
    "}\n"
    "vec4 ysp_op_(vec4 a, vec4 e, vec4 B) {\n"
    "    int op = int(B.y);\n"
    "    float k = max(B.z, 1e-6);\n"
    "    vec4 ne = vec4(-e.xyz, e.w), na = vec4(-a.xyz, a.w);\n"
    "    if (op == 0) return e.x < a.x ? e : a;\n"
    "    if (op == 1) return e.x > a.x ? e : a;\n"
    "    if (op == 2) return ne.x > a.x ? ne : a;\n"
    "    if (op == 3) { vec4 u = e.x < a.x ? e : a, v = e.x < a.x ? a : e; return u.x > -v.x ? u : vec4(-v.xyz, v.w); }\n"
    "    if (op == 4) return ysp_smin_(a, e, k);\n";

static const char ygfx__glsl_vec4[] =
    "    vec4 r = ysp_smin_(na, op == 5 ? ne : e, k);\n"
    "    return vec4(-r.xyz, r.w);\n"
    "}\n"
    "void ysp_field_(vec2 p, vec2 p1, int npts, bool ws, out vec4 f0, out vec4 f1) {\n"
    "    int n = int(ysp_v[ysp_i * 16 + 6].w);\n"
    "    vec4 acc = vec4(1e30, 1.0, 0.0, 0.0);\n"
    "    f0 = acc;\n"
    "    f1 = acc;\n"
    "#ifdef YSP_V1\n"   /* one primitive, block 0 (no onion): what the fold gives for n = 1 */
    "    vec4 A = ysp_vd_(0), B = ysp_vd_(1), C = ysp_vd_(2), D = ysp_vd_(3);\n"
    "    vec2 ax = A.zw, ay = vec2(-A.w, A.z);\n"
    "    for (int t = 0; t < npts; t++) {\n"
    "        vec2 r = (t == 0 ? p : p1) - A.xy;\n"
    "        vec4 e = ysp_prim_(vec2(dot(r, ax), dot(r, ay)), B, C, D, ws);\n"
    "        e.yz = e.y * ax + e.z * ay;\n"
    "        if (t == 0) f0 = e; else f1 = e;\n"
    "    }\n"
    "#else\n"
    "    for (int t = 0; t < n * npts; t++) {\n"
    "        int i = t < n ? t : t - n;\n"
    "        vec4 A = ysp_vd_(4 * i), B = ysp_vd_(4 * i + 1), C = ysp_vd_(4 * i + 2), D = ysp_vd_(4 * i + 3);\n"
    "        vec2 ax = A.zw, ay = vec2(-A.w, A.z), r = (t < n ? p : p1) - A.xy;\n"
    "        vec4 e = ysp_prim_(vec2(dot(r, ax), dot(r, ay)), B, C, D, ws);\n"
    "        e.yz = e.y * ax + e.z * ay;\n"
    "        if (B.w > 0.0) e = vec4(abs(e.x) - B.w, ysp_sgn_(e.x) * e.yz, e.w);\n"
    "        acc = i == 0 ? e : ysp_op_(acc, e, B);\n"
    "        if (t == n - 1) f0 = acc;\n"
    "        if (t == 2 * n - 1) f1 = acc;\n"
    "    }\n"
    "#endif\n"
    "    if (npts < 2) f1 = f0;\n"
    "}\n"
    "/* Coverage and Gaussians four at a time: D3D inlines each call site, so\n"
    " * the vector program calls these from a few sites, not many. */\n"
    "vec4 ysp_erfc4_(vec4 x) {\n"
    "    vec4 z = abs(x), t = 1.0 / (1.0 + 0.3275911 * z);\n"
    "    vec4 y = t * (0.254829592 + t * (-0.284496736 + t * (1.421413741 + t * (-1.453152027 + t * 1.061405429)))) * exp(-z * z);\n"
    "    return mix(2.0 - y, y, greaterThanEqual(x, vec4(0.0)));\n"
    "}\n"
    "vec4 ysp_cov4_(vec4 d) {\n"
    "    int prof = int(ysp_edge.y);\n"
    "    float w = ysp_edge.z;\n"
    "    vec4 hard = mix(vec4(0.0), vec4(1.0), lessThanEqual(d, vec4(0.0)));\n"
    "    if (prof == 0 || w <= 0.0) return hard;\n"
    "    if (prof == 1) {\n"
    "        vec4 c = 0.5 + 0.5 * cos(ysp_PI * (d + 0.5 * w) / w);\n"
    "        return mix(mix(c, vec4(0.0), greaterThanEqual(d, vec4(0.5 * w))), vec4(1.0), lessThanEqual(d, vec4(-0.5 * w)));\n"
    "    }\n"
    "    return 0.5 * ysp_erfc4_(d / (w * 1.4142135623731));\n"
    "}\n"
    "vec4 ysp_gauss4_(vec4 x, vec4 sg) {\n";
static const char ygfx__glsl_vec4b[] =   /* C99 caps a literal at 4095 characters */
    "    vec4 g = 0.5 * ysp_erfc4_(x / (max(sg, vec4(1e-30)) * 1.4142135623731));\n"
    "    return mix(g, mix(vec4(0.0), vec4(1.0), lessThanEqual(x, vec4(0.0))), lessThanEqual(sg, vec4(0.0)));\n"
    "}\n"
    "float ysp_along_(float cm, float nn, float hh, float s) {\n"
    "    vec4 V4 = ysp_v[ysp_i * 16 + 4], V6 = ysp_v[ysp_i * 16 + 6];\n"
    "    int fl = int(ysp_v[ysp_i * 16 + 5].w), cap = int(V6.z);\n"
    "    bool dsh = (fl & 1) != 0, trm = (fl & 2) != 0;\n"
    "    float P = V4.x + V4.y, u = dsh ? mod(s - V4.z, P) : 0.0, ex = cap == 2 ? hh : 0.0, a = 1.0, ga = 0.0;\n"
    "    if (dsh) {\n"
    "        a = 0.0;\n"
    "        ga = 1e30;\n"
    "        for (int k = 0; k < int(ysp_axes.z); k++) {\n"
    "            float uk = u + float(k - 1) * P;\n"
    "            vec4 c = ysp_cov4_(vec4(uk - V4.x - ex, uk + ex, 0.0, 0.0));\n"
    "            a += c.x - c.y;\n"
    "            ga = min(ga, max(abs(uk - 0.5 * V4.x) - 0.5 * V4.x, 0.0));\n"
    "        }\n"
    "    }\n"
    "    if (trm) ga = max(ga, max(abs(s - 0.5 * (V6.x + V6.y)) - 0.5 * (V6.y - V6.x), 0.0));\n"
    "    vec4 t = ysp_cov4_(vec4(s - V6.y - ex, s - V6.x + ex, length(vec2(ga, nn)) - hh, 0.0));\n"
    "    if (cap == 1) return t.z;\n"
    "    return cm * a * (trm ? t.x - t.y : 1.0);\n"
    "}\n"
    "vec3 ysp_paint_(vec2 p, vec4 f0, float d) {\n"
    "    int o = int(ysp_v[ysp_i * 16 + 8].y);\n"
    "    vec4 H = ysp_vd_(o), G = ysp_vd_(o + 1);\n"
    "    int kind = int(H.x), sp = int(H.y), n = int(H.w);\n"
    "    vec3 c = ysp_vd_(o + 2).xyz;\n"
    "    if (kind == 4) {\n"
    "        int g0 = int(G.x);\n"
    "        vec2 q = (p - max(d, 0.0) * f0.yz) * G.y;\n"
    "        float ws = 0.0;\n"
    "        vec3 acc = vec3(0.0);\n"
    "        for (int i = 0; i < n; i++) {\n"
    "            int ip = i == 0 ? n - 1 : i - 1, i2 = i + 1 < n ? i + 1 : 0;\n"
    "            vec2 vp = ysp_vd_(g0 + 3 * ip + 2).zw * G.y, vi = ysp_vd_(g0 + 3 * i + 2).zw * G.y, vn = ysp_vd_(g0 + 3 * i2 + 2).zw * G.y;\n"
    "            float w = (vi.x - vp.x) * (vn.y - vp.y) - (vi.y - vp.y) * (vn.x - vp.x);\n"
    "            for (int j = 0; j < n; j++) {\n"
    "                if (j == ip || j == i) continue;\n"
    "                vec2 a = ysp_vd_(g0 + 3 * j + 2).zw * G.y - q, b = ysp_vd_(g0 + 3 * (j + 1 < n ? j + 1 : 0) + 2).zw * G.y - q;\n"
    "                w *= a.x * b.y - a.y * b.x;\n"
    "            }\n"
    "            ws += w;\n";

static const char ygfx__glsl_vec5[] =
    "            acc += w * ysp_vd_(o + 2 + i).xyz;\n"
    "        }\n"
    "        c = acc / ws;\n"
    "    } else {\n"
    "        float t;\n"
    "        if (kind == 1) { vec2 ab = G.zw - G.xy; t = dot(p - G.xy, ab) / dot(ab, ab); }\n"
    "        else if (kind == 2) t = length(p - G.xy) / G.z;\n"
    "        else t = ysp_wrap_(ysp_ang_(p - G.xy) - G.z) / ysp_TAU;\n"
    "        t = H.z > 0.5 ? fract(t) : clamp(t, 0.0, 1.0);\n"
    "        for (int k = 0; k < n - 1; k++) {\n"
    "            vec4 a = ysp_vd_(o + 2 + k), b = ysp_vd_(o + 3 + k);\n"
    "            if (t >= a.w) c = b.w > a.w ? mix(a.xyz, b.xyz, clamp((t - a.w) / (b.w - a.w), 0.0, 1.0)) : b.xyz;\n"
    "        }\n"
    "    }\n"
    "    if (sp == 1) {\n"
    "        vec3 l = vec3(c.x + 0.3963377774 * c.y + 0.2158037573 * c.z, c.x - 0.1055613458 * c.y - 0.0638541728 * c.z,\n"
    "                      c.x - 0.0894841775 * c.y - 1.2914855480 * c.z);\n"
    "        l = l * l * l;\n"
    "        vec4 q0 = ysp_vd_(o + 18), q1 = ysp_vd_(o + 19), q2 = ysp_vd_(o + 20);\n"
    "        c = vec3(q0.w + dot(q0.xyz, l), q1.w + dot(q1.xyz, l), q2.w + dot(q2.xyz, l));\n"
    "    } else if (sp == 2) {\n"
    "        vec3 k3 = c.z * vec3(sin(c.x), cos(c.x) * cos(c.y), cos(c.x) * sin(c.y));\n"
    "        vec4 r0 = ysp_vd_(o + 18), r1 = ysp_vd_(o + 19), r2 = ysp_vd_(o + 20);\n"
    "        c = vec3(r0.w + dot(r0.xyz, k3), r1.w + dot(r1.xyz, k3), r2.w + dot(r2.xyz, k3));\n"
    "    }\n"
    "    return c;\n"
    "}\n"
    "vec4 ysp_over_(vec4 t, vec4 b) { return t + b * (1.0 - t.a); }\n"
    "void main() {\n"
    "    ysp_p = ysp_local_();\n"
    "    vec4 V2 = ysp_look, V3 = ysp_color, V5 = ysp_v[ysp_i * 16 + 5];\n"
    "    vec4 V7 = ysp_v[ysp_i * 16 + 7], V8 = ysp_v[ysp_i * 16 + 8];\n"
    "    int fl = int(V5.w);\n"
    "    vec4 f0, f1;\n"
    "#ifdef YSP_INST\n"   /* the template's field at p / k, times k */
    "    float ki = ysp_k;\n"
    "#else\n"
    "    const float ki = 1.0;\n"
    "#endif\n"
    "#ifdef YSP_WS\n"   /* the arc length: a constant in a specialized program */
    "    ysp_field_(ysp_p / ki, (ysp_p - V7.xy) / ki, int(V7.w), YSP_WS != 0, f0, f1);\n"
    "#else\n"
    "    ysp_field_(ysp_p / ki, (ysp_p - V7.xy) / ki, int(V7.w), (fl & 8) != 0, f0, f1);\n"
    "#endif\n"
    "    f0.x *= ki;\n"
    "    f1.x *= ki;\n"
    "    if ((fl & 4) != 0) { f0.x /= max(length(f0.yz), 1e-3); f1.x /= max(length(f1.yz), 1e-3); }\n"
    "    if (V7.z > 0.0) { f0.x = abs(f0.x) - V7.z; f1.x = abs(f1.x) - V7.z; }\n"
    "    float d = f0.x - V2.z, d1 = f1.x - V2.z, hw = V3.w;\n"
    "    vec4 c0 = ysp_cov4_(vec4(d - V2.w - hw, d - V2.w + hw, d, 0.0));\n"
    "    float cm = hw > 0.0 ? c0.x - c0.y : c0.z;\n"
    "#if YSP_ALONG != 0\n"
    "    if ((fl & 3) != 0) cm = ysp_along_(cm, hw > 0.0 ? d - V2.w : d + V8.w, hw > 0.0 ? hw : V8.w, f0.w);\n"
    "#endif\n"
    "#if YSP_PAINT != 0\n"
    "    vec4 acc = vec4((fl & 16) != 0 ? ysp_paint_(ysp_p / ki, f0, d / ki) : V3.rgb, 1.0) * cm;\n"
    "#else\n"
    "    vec4 acc = vec4(V3.rgb, 1.0) * cm;\n"
    "#endif\n"
    "#if YSP_FX != 0\n"
    "    if ((fl & 32) != 0) {\n"
    "        int o = int(V8.x);\n"
    "        vec4 Dp = ysp_vd_(o), Gp = ysp_vd_(o + 2), Ip = ysp_vd_(o + 4), B0 = ysp_vd_(o + 6), B1 = ysp_vd_(o + 8);\n"
    "        vec4 g = ysp_gauss4_(vec4(d1 - Dp.y, d - Gp.y, -(d1 + Ip.y), 0.0), vec4(Dp.x, Gp.x, Ip.x, 1.0));\n"
    "        vec4 bd = ysp_cov4_(vec4(d - B0.y, d - B0.x, d - B1.y, d - B1.x));\n"
    "        if ((fl & 64) != 0) {   /* the exact blur of a sharp rect: four erf in one */\n"
    "            vec2 q = ysp_p - V7.xy, hb = ysp_vd_(2).xy + Dp.y;\n"
    "            vec4 e = 0.5 * ysp_erfc4_(-vec4(hb - q, -hb - q) / (Dp.x * 1.4142135623731));\n"
    "            g.x = (e.x - e.z) * (e.y - e.w);\n"
    "        }\n"
    "        vec4 r = vec4(ysp_vd_(o + 1).rgb, 1.0) * (g.x * Dp.z);\n"
    "        r = ysp_over_(vec4(ysp_vd_(o + 3).rgb, 1.0) * (g.y * Gp.z), r);\n"
    "        r = ysp_over_(acc, r);\n"
    "        r = ysp_over_(vec4(ysp_vd_(o + 5).rgb, 1.0) * (c0.z * g.z * Ip.z), r);\n"
    "        r = ysp_over_(vec4(ysp_vd_(o + 7).rgb, 1.0) * ((bd.x - bd.y) * B0.z), r);\n"
    "        r = ysp_over_(vec4(ysp_vd_(o + 9).rgb, 1.0) * ((bd.z - bd.w) * B1.z), r);\n"
    "        acc = r;\n"
    "    }\n"
    "#endif\n"
    "    ysp_out = acc * (V2.x * V2.y);\n"
    "}\n";

static const char* const ygfx__glsl_vec[] = { ygfx__glsl_vec0, ygfx__glsl_vec0b, ygfx__glsl_vec1, ygfx__glsl_vec2, ygfx__glsl_vec3, ygfx__glsl_vec3b, ygfx__glsl_vec4, ygfx__glsl_vec4b, ygfx__glsl_vec5 };

/* The text program (v0.6, RUNS): one glyph or path per instance, from a
 * curve set's two textures (CURVE SETS). The vertex shader finds the glyph's
 * record, builds the item's map from em to the pass's GL px and emits the
 * turned glyph box grown by 1 px. The fragment shader casts two rays from
 * the pixel center through the glyph's bands (Lengyel 2017). Block 0:
 * placement; (half box px, px per unit, px per em); (opacity, gate, fields,
 * aliased); (rgb, palette length); (log2 texture width, quad margin px, G, rays); (cap
 * px, e, the box's top-left in item units); the palette from vec4 32. */
static const char ygfx__glsl_text_vs[] =
    "uniform usampler2D ysp_utex0;\n"
    "layout(location = 0) in vec4 ysp_a0;\n"     /* x, y units; ori deg; glyph */
    "layout(location = 1) in vec4 ysp_a1;\n"     /* contrast, scale, gate, palette position */
    "flat out vec4 ysp_tq;\n"                    /* the glyph box's center: px, y down; em */
    "flat out vec4 ysp_tj;\n"                    /* the em axes in px, y down, over px per em */
    "flat out vec4 ysp_tb;\n"                    /* the bbox, em */
    "flat out uvec4 ysp_th;\n"                   /* record word, band counts, flags */
    "flat out vec4 ysp_tc;\n"                    /* rgb, alpha */
    YGFX__GLSL_CSD
    "uvec4 ysp_w4_(int t, int L) { return texelFetch(ysp_utex0, ivec2(t & ((1 << L) - 1), t >> L), 0); }\n"
    "void main() {\n"
    "    vec4 B0 = ysp_v[0], B1 = ysp_v[1], B2 = ysp_v[2], B3 = ysp_v[3], B4 = ysp_v[4];\n"
    "    int m = int(B2.z), L = int(B4.x);\n"
    "    vec2 c = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1)) * 2.0 - 1.0;\n"
    "    gl_Position = vec4(2.0, 2.0, 2.0, 1.0);\n"
    "    ysp_tq = vec4(0.0); ysp_tj = vec4(0.0); ysp_tb = vec4(0.0); ysp_th = uvec4(0u); ysp_tc = vec4(0.0);\n"
    "    float gid = ysp_a0.w, k = (m & 16) != 0 ? ysp_a1.y : 1.0;\n"
    "    float a = B2.x * B2.y * ((m & 8) != 0 ? ysp_a1.x : 1.0) * ((m & 32) != 0 ? ysp_a1.z : 1.0);\n"
    "    if (!(gid >= 0.0 && gid < B4.z && gid == floor(gid)) || !(k > 0.0) || a == 0.0) return;\n"
    "    int ti = 8 + int(gid);\n"
    "    uint o = ysp_w4_(ti >> 2, L)[ti & 3];\n"
    "    if (o == 0u) return;\n"
    "    uvec4 h1 = ysp_w4_(int(o >> 2u) + 1, L);\n"
    "    if ((h1.x & 0xFFFFu) == 0u) return;\n"
    "    vec4 bb = uintBitsToFloat(ysp_w4_(int(o >> 2u), L));\n"
    /* the item's em axes: its ori about the glyph box's center, from the
     * run's turned axes; px per em times its scale. In px with y down (the
     * pack's, ysp_v[6]), not the pass's GL px: a target pass (rows top
     * first) and the scene then compute a pixel's em point from the same
     * bits, so a run cached in a target at 1:1 has the scene's coverage */
    "    vec4 B6 = ysp_v[6];\n"
    "    vec2 ax = B6.zw, ay = vec2(-B6.w, B6.z);\n"
    "    vec2 cs = (m & 2) != 0 ? ysp_csd_(ysp_a0.z) : vec2(1.0, 0.0);\n"
    "    vec2 ex = cs.x * ax + cs.y * ay, ey = cs.x * ay - cs.y * ax;\n"
    "    float sp = B1.w * k;\n"
    "    vec2 ce = 0.5 * (bb.xy + bb.zw), hb = 0.5 * (bb.zw - bb.xy);\n"
    "    vec2 lc = (ysp_a0.xy - ysp_v[5].zw) * B1.z - B1.xy + B1.w * ce;\n"
    "    vec2 Q = B6.xy + lc.x * ax + lc.y * ay;\n"
    /* grown by what a pixel can reach past the outline: half its diagonal */
    "    vec2 w = Q + (c.x * (sp * hb.x + B4.y)) * ex + (c.y * (sp * hb.y + B4.y)) * ey;\n"
    "    if (ysp_axes.x < 0.0) w.y = ysp_target.y - w.y;\n"
    "    gl_Position = vec4(w * ysp_target.zw * 2.0 - 1.0, 0.0, 1.0);\n"
    "    vec3 pc = B3.rgb;\n"
    "    if ((m & 64) != 0) {\n"
    "        int n = int(B3.w);\n"
    "        float u = clamp(ysp_a1.w, 0.0, float(n - 1));\n"
    "        int i0 = int(floor(u));\n"
    "        float f = u - float(i0);\n"
    "        pc = f > 0.0 ? mix(ysp_v[32 + i0].rgb, ysp_v[32 + min(i0 + 1, n - 1)].rgb, f) : ysp_v[32 + i0].rgb;\n"
    "    }\n"
    "    ysp_tq = vec4(Q, ce); ysp_tj = vec4(ex, ey) / sp; ysp_tb = bb;\n"
    "    ysp_th = uvec4(o, h1.x, h1.y, 0u); ysp_tc = vec4(pc, a);\n"

    "}\n";

static const char ygfx__glsl_text_fs0[] =
    "uniform sampler2D ysp_tex0;\n"
    "uniform usampler2D ysp_utex0;\n"
    "flat in vec4 ysp_tq, ysp_tj, ysp_tb;\n"
    "flat in uvec4 ysp_th;\n"
    "flat in vec4 ysp_tc;\n"
    "out vec4 ysp_out;\n"
    "int ysp_L;\n"
    "float ysp_sp, ysp_cap;\n"
    "bool ysp_alias;\n"
    "vec4 ysp_t4_(int i) { return texelFetch(ysp_tex0, ivec2(i & ((1 << ysp_L) - 1), i >> ysp_L), 0); }\n"
    "uvec4 ysp_w4_(int t) { return texelFetch(ysp_utex0, ivec2(t & ((1 << ysp_L) - 1), t >> ysp_L), 0); }\n"
    /* The fraction of the unit pixel square where n . q < z, for a unit
     * normal n with |n.x| = a, |n.y| = b: the CDF of the square's projection
     * on n, a trapezoid of support a + b and plateau |a - b|. */
    "float ysp_hp_(float z, float a, float b) {\n"
    "    float w0 = min(a, b), w1 = max(a, b), u = z + 0.5 * (a + b);\n"
    "    if (u <= 0.0) return 0.0;\n"
    "    if (u >= a + b) return 1.0;\n"
    "    if (w0 < 1e-6) return clamp(u / w1, 0.0, 1.0);\n"
    "    if (u < w0) return u * u / (2.0 * w0 * w1);\n"
    "    if (u <= w1) return (u - 0.5 * w0) / w1;\n"
    "    float r = a + b - u;\n"
    "    return 1.0 - r * r / (2.0 * w0 * w1);\n"
    "}\n"
    /* the fill rule on a fractional winding: exact while the window sees one
     * transition */
    "float ysp_fill_(float w, bool eo) { float x = abs(w); return eo ? 1.0 - abs(1.0 - mod(x, 2.0)) : min(x, 1.0); }\n"
    /* One crossing: s its direction, up its distance along the ray (px), T
     * the curve's tangent there (GL px), es the ray's direction (GL px). A
     * crossing near the pixel adds the exact box coverage of the half-plane
     * bounded by the tangent line (exact on any straight edge); its weight is
     * how steeply and how near the ray crosses, 0 where the coverage
     * saturates, so the mix of the two rays is continuous. */
    "void ysp_root_(float s, float up, vec2 Ts, vec2 es, inout float c, inout float wg) {\n"
    "    float tl = dot(Ts, Ts);\n"
    "    if (ysp_alias || abs(up) > ysp_cap || !(tl > 0.0)) { c += up > 0.0 ? s : 0.0; return; }\n"
    "    vec2 n = vec2(-Ts.y, Ts.x) * inversesqrt(tl);\n"
    "    float ce = abs(dot(n, es)), a = abs(n.x), b = abs(n.y), z = up * ce;\n"
    "    c += s * ysp_hp_(z, a, b);\n"
    "    wg = max(wg, ce * clamp(1.0 - abs(z) / (0.5 * (a + b)), 0.0, 1.0));\n"
    "}\n";

/* V3: exact area where the pixel's footprint in em is an axis-aligned
 * rectangle (no turn but quarter turns) and the glyph is resolved. The
 * area of shape intersect pixel is the sum over curves of the integral of
 * clamp(x, 0, W) d clamp(y, 0, H) in pixel-corner coordinates (Green's
 * theorem): a curve right of the pixel adds W times its clamped rise, one
 * left of it nothing, and one over it is cut into pieces monotone in x and
 * y, whose middle part is a cubic in t (two Gauss points are exact). */
static const char ygfx__glsl_text_slab[] =
    "float ysp_qv_(vec3 k, float t) { return (k.x * t + k.y) * t + k.z; }\n"
    /* the t in [s, e] where the monotone k(t) takes the value v */
    "float ysp_tat_(vec3 k, float v, float s, float e) {\n"
    "    float C = k.z - v, t;\n"
    "    if (abs(k.x) * (e - s) <= 1e-6 * abs(k.y)) t = -C / k.y;\n"
    "    else {\n"
    "        float D = sqrt(max(k.y * k.y - 4.0 * k.x * C, 0.0)), q = -0.5 * (k.y + (k.y >= 0.0 ? D : -D));\n"
    "        float t1 = q / k.x, t2 = q != 0.0 ? C / q : t1, m = 0.5 * (s + e);\n"
    "        t = abs(t1 - m) < abs(t2 - m) ? t1 : t2;\n"
    "    }\n"
    "    return clamp(t, s, e);\n"
    "}\n"
    "float ysp_mid_(vec3 kx, vec3 ky, float a, float b) {\n"
    "    float h = 0.5 * (b - a), m = 0.5 * (a + b), g = 0.577350269189626 * h;\n"
    "    float t0 = m - g, t1 = m + g;\n"
    "    return h * (ysp_qv_(kx, t0) * (2.0 * ky.x * t0 + ky.y) + ysp_qv_(kx, t1) * (2.0 * ky.x * t1 + ky.y));\n"
    "}\n"
    "float ysp_slab_(vec2 q0, vec2 q1, vec2 q2, float W, float H) {\n"
    "    float y0 = min(min(q0.y, q1.y), q2.y), y1 = max(max(q0.y, q1.y), q2.y);\n"
    "    if (y1 <= 0.0 || y0 >= H) return 0.0;\n"
    "    if (min(min(q0.x, q1.x), q2.x) >= W) return W * (clamp(q2.y, 0.0, H) - clamp(q0.y, 0.0, H));\n"
    "    vec3 kx = vec3(q0.x - 2.0 * q1.x + q2.x, 2.0 * (q1.x - q0.x), q0.x);\n"
    "    vec3 ky = vec3(q0.y - 2.0 * q1.y + q2.y, 2.0 * (q1.y - q0.y), q0.y);\n"
    "    float ex = kx.x != 0.0 ? clamp(-kx.y / (2.0 * kx.x), 0.0, 1.0) : 1.0;\n"
    "    float ey = ky.x != 0.0 ? clamp(-ky.y / (2.0 * ky.x), 0.0, 1.0) : 1.0;\n"
    "    vec4 sp = vec4(0.0, min(ex, ey), max(ex, ey), 1.0);\n"
    "    float acc = 0.0;\n"
    "    for (int i = 0; i < 3; i++) {\n"
    "        float s = sp[i], e = sp[i + 1];\n"
    "        if (!(e > s)) continue;\n"
    "        float ys = ysp_qv_(ky, s), ye = ysp_qv_(ky, e);\n"
    "        if (max(ys, ye) <= 0.0 || min(ys, ye) >= H) continue;\n"
    "        float va = clamp(ys, 0.0, H), vb = clamp(ye, 0.0, H);\n"
    "        float ta = va == ys ? s : ysp_tat_(ky, va, s, e), tb = vb == ye ? e : ysp_tat_(ky, vb, s, e);\n"
    "        float xs = ysp_qv_(kx, ta), xe = ysp_qv_(kx, tb);\n"
    "        if (max(xs, xe) <= 0.0) continue;\n"
    "        if (min(xs, xe) >= W) { acc += W * (vb - va); continue; }\n"
    "        bool up = xs <= xe;\n"
    "        float u0 = (xs < 0.0) != (xe < 0.0) ? ysp_tat_(kx, 0.0, ta, tb) : (up ? ta : tb);\n"
    "        float uw = (xs > W) != (xe > W) ? ysp_tat_(kx, W, ta, tb) : (up ? tb : ta);\n"
    "        if (up) {\n"   /* x rises: left, middle, right */
    "            acc += ysp_mid_(kx, ky, u0, max(uw, u0));\n"
    "            if (uw < tb) acc += W * (vb - ysp_qv_(ky, uw));\n"
    "        } else {\n"          /* x falls: right, middle, left */
    "            if (uw > ta) acc += W * (ysp_qv_(ky, uw) - va);\n"
    "            acc += ysp_mid_(kx, ky, uw, max(u0, uw));\n"
    "        }\n"
    "    }\n"
    "    return acc;\n"
    "}\n"
    ;

/* One ray along em +x (hz) or +y through its band: the fractional winding
 * and the weight. The band's list is sorted by the largest coordinate along
 * the ray, so the loop ends at the first curve wholly behind the window. */
static const char ygfx__glsl_text_ray[] =
    "vec2 ysp_ray_(vec2 p, vec2 pr, int rec, int nh, int nv, uint fl, vec2 ex, vec2 ey, bool hz) {\n"
    "    int nb = hz ? nh : nv;\n"
    "    float ac = hz ? p.y : p.x, lo = hz ? ysp_tb.y : ysp_tb.x, hi = hz ? ysp_tb.w : ysp_tb.z;\n"
    "    float al = hz ? p.x : p.y, alo = hz ? ysp_tb.x : ysp_tb.y, ahi = hz ? ysp_tb.z : ysp_tb.w;\n"
    "    int kb = clamp(int(floor((ac - lo) * float(nb) / (hi - lo))), 0, nb - 1);\n"
    "    uvec4 D = ysp_w4_(rec + 2 + (hz ? kb : nh + kb));\n"
    /* a backward list casts the ray the other way, from the nearer side */
    "    bool bw = (fl & 2u) != 0u && al < 0.5 * (alo + ahi);\n"
    "    int j = int(bw ? D.z : D.x), end = j + int(bw ? D.w : D.y);\n"
    "    vec2 E = hz ? vec2(1.0, 0.0) : vec2(0.0, 1.0), F = hz ? vec2(0.0, 1.0) : vec2(-1.0, 0.0);\n"
    "    float sg = bw ? -1.0 : 1.0;\n"
    "    E *= sg;\n"
    "    vec2 es = normalize(E.x * ex + E.y * ey);\n"
    "    float cut = ysp_alias ? 0.0 : -ysp_cap / ysp_sp, c = 0.0, wg = 0.0;\n"
    "    uvec4 R4 = ysp_w4_(j >> 2);\n"
    "    for (; j < end; j++) {\n"
    "        if ((j & 3) == 0) R4 = ysp_w4_(j >> 2);\n"
    "        int r = int(R4[j & 3]);\n"
    "        vec4 T0 = ysp_t4_(r) - ysp_tq.zwzw, T1 = ysp_t4_(r + 1) - ysp_tq.zwzw;\n"
    "        vec2 q0 = T0.xy - pr, q1 = T0.zw - pr, q2 = T1.xy - pr;\n"
    "        vec3 u = vec3(dot(q0, E), dot(q1, E), dot(q2, E));\n"
    "        if (max(max(u.x, u.y), u.z) < cut) break;\n"
    "        vec3 v = vec3(dot(q0, F), dot(q1, F), dot(q2, F));\n"
    /* the root rule (CURVE SETS): the signs of the three control values
     * give which roots lie on the curve; the root formula gives each one's
     * direction */
    "        bool s1 = v.x > 0.0, s2 = v.y > 0.0, s3 = v.z > 0.0;\n"
    "        bool two = s1 == s3 && s2 != s1;\n"
    "        bool fall = (s1 && !s3) || two, rise = (!s1 && s3) || two;\n"
    "        if (!fall && !rise) continue;\n"
    "        float a = v.x - 2.0 * v.y + v.z, b = v.x - v.y, D = b * b - a * v.x;\n"
    /* in a two-root row the curve may turn back before the ray: D < 0
     * then means no crossing, where clamping D would give two roots that
     * do not cancel. Elsewhere one root is certain and D < 0 is rounding */
    "        if (two && D < 0.0) continue;\n"
    "        float sD = sqrt(max(D, 0.0));\n"
    "        float q = b + (b >= 0.0 ? sD : -sD);\n"
    "        float tr = 0.0, tf = 0.0;\n"
    "        if (q != 0.0) { if (b >= 0.0) { tr = q / a; tf = v.x / q; } else { tf = q / a; tr = v.x / q; } }\n"
    "        float au = u.x - 2.0 * u.y + u.z, bu = u.x - u.y;\n"
    "        vec2 A2 = q0 - 2.0 * q1 + q2, B2 = q0 - q1;\n"
    /* a line has p1 = p0: no tangent at its start, so its direction */
    "        bool ln = dot(B2, B2) == 0.0;\n"
    "        vec2 Tr = ln ? A2 : A2 * tr - B2, Tf = ln ? A2 : A2 * tf - B2;\n"
    "        if (rise) ysp_root_(1.0, ((au * tr - 2.0 * bu) * tr + u.x) * ysp_sp, Tr.x * ex + Tr.y * ey, es, c, wg);\n"
    "        if (fall) ysp_root_(-1.0, ((au * tf - 2.0 * bu) * tf + u.x) * ysp_sp, Tf.x * ex + Tf.y * ey, es, c, wg);\n"
    "    }\n"
    "    return vec2(sg * c, wg);\n"
    "}\n";

/* V3: the pixel's rows are a slab [p.y - hw.y, p.y + hw.y] in em; each band
 * they meet integrates its own part of them. */
static const char ygfx__glsl_text_main[] =
    "float ysp_area_(vec2 p, vec2 pr, vec2 hw, int rec, int nh, uint fl) {\n"
    "    float bh = (ysp_tb.w - ysp_tb.y) / float(nh), area = 0.0, W = 2.0 * hw.x;\n"
    /* from the nearer side: a closed contour's rise sums to 0, so the
     * integral of clamp(x) - W counts the curves left of the pixel instead */
    "    bool bw = (fl & 2u) != 0u && p.x < 0.5 * (ysp_tb.x + ysp_tb.z);\n"
    "    int k0 = clamp(int(floor((p.y - hw.y - ysp_tb.y) / bh)), 0, nh - 1);\n"
    "    int k1 = clamp(int(floor((p.y + hw.y - ysp_tb.y) / bh)), 0, nh - 1);\n"
    "    for (int k = k0; k <= k1; k++) {\n"
    "        uvec4 D = ysp_w4_(rec + 2 + k);\n"
    "        float ya = k == 0 ? 0.0 : clamp(ysp_tb.y + float(k) * bh - (p.y - hw.y), 0.0, 2.0 * hw.y);\n"
    "        float yb = k == nh - 1 ? 2.0 * hw.y : clamp(ysp_tb.y + float(k + 1) * bh - (p.y - hw.y), 0.0, 2.0 * hw.y);\n"
    "        vec2 o = hw - pr - vec2(0.0, ya);\n"
    "        int j = int(bw ? D.z : D.x), end = j + int(bw ? D.w : D.y);\n"
    "        uvec4 R4 = ysp_w4_(j >> 2);\n"
    "        for (; j < end; j++) {\n"
    "            if ((j & 3) == 0) R4 = ysp_w4_(j >> 2);\n"
    "            int r = int(R4[j & 3]);\n"
    "            vec4 T0 = ysp_t4_(r) - ysp_tq.zwzw, T1 = ysp_t4_(r + 1) - ysp_tq.zwzw;\n"
    "            vec2 q0 = T0.xy + o, q1 = T0.zw + o, q2 = T1.xy + o;\n"
    /* sorted by the largest (smallest) x: nothing further reaches the pixel */
    "            if (bw ? min(min(q0.x, q1.x), q2.x) >= W : max(max(q0.x, q1.x), q2.x) <= 0.0) break;\n"
    "            area += ysp_slab_(q0, q1, q2, W, yb - ya) - (bw ? W * (clamp(q2.y, 0.0, yb - ya) - clamp(q0.y, 0.0, yb - ya)) : 0.0);\n"
    "        }\n"
    "    }\n"
    "    return area;\n"
    "}\n"
    "void main() {\n"
    "    ysp_L = int(ysp_v[4].x);\n"
    /* y down: H - y is exact at pixel centers */
    "    vec2 d = vec2(gl_FragCoord.x, ysp_axes.x < 0.0 ? ysp_target.y - gl_FragCoord.y : gl_FragCoord.y) - ysp_tq.xy;\n"
    /* curves from the glyph box's center: the two values are near each
     * other, so their difference is exact, and the pixel's offset small: a
     * glyph far from the origin (CFF units) keeps every bit near the pixel */
    "    vec2 pr = vec2(dot(ysp_tj.xy, d), dot(ysp_tj.zw, d)), p = ysp_tq.zw + pr;\n"
    "    vec2 ex = ysp_tj.xy, ey = ysp_tj.zw;\n"
    "    ysp_sp = inversesqrt(dot(ex, ex));\n"
    "    vec2 hw = 0.5 * vec2(abs(ex.x) + abs(ex.y), abs(ey.x) + abs(ey.y));\n"
    "    ysp_cap = ysp_v[5].x;\n"
    "    ysp_alias = ysp_v[2].w > 0.5;\n"
    "    uint fl = ysp_th.z;\n"
    "    bool eo = (fl & 1u) != 0u;\n"
    "    int rec = int(ysp_th.x >> 2u), nh = int(ysp_th.y & 0xFFFFu), nv = int(ysp_th.y >> 16u);\n"
    "    float cov;\n"
    /* exact area where the pixel is an axis-aligned rectangle in em (no turn
     * but quarter turns) and the glyph is resolved (CURVE SETS, flag bit 2) */
    "    if (YSP_TEXT_V3 != 0 && (fl & 4u) != 0u && !ysp_alias && (abs(ex.y) + abs(ey.x) <= 1e-6 * (abs(ex.x) + abs(ey.y)) ||\n"
    "                                          abs(ex.x) + abs(ey.y) <= 1e-6 * (abs(ex.y) + abs(ey.x)))) {\n"
    "        cov = clamp(abs(ysp_area_(p, pr, hw, rec, nh, fl)) / (4.0 * hw.x * hw.y), 0.0, 1.0);\n"
    "    } else {\n"
    "        vec2 rx = ysp_ray_(p, pr, rec, nh, nv, fl, ex, ey, true);\n"
    "        float fx = ysp_fill_(rx.x, eo);\n"
    "        if (ysp_alias) cov = fx;\n"
    "        else {\n"
    /* the weighted mean is a convex mix of the two rays, bounded whatever
     * the weights; below e of total weight it fades to their mean, where
     * both rays see the same integer winding */
    "            vec2 ry = ysp_ray_(p, pr, rec, nh, nv, fl, ex, ey, false);\n"
    "            float fy = ysp_fill_(ry.x, eo), sw = rx.y + ry.y, t = clamp(sw / ysp_v[5].y, 0.0, 1.0);\n"
    "            cov = mix(0.5 * (fx + fy), sw > 0.0 ? (fx * rx.y + fy * ry.y) / sw : 0.0, t);\n"
    "        }\n"
    "    }\n"
    "    ysp_out = vec4(ysp_tc.rgb * (cov * ysp_tc.a), cov * ysp_tc.a);\n"
    "}\n";

static const char ygfx__glsl_output[] =
    "uniform sampler2D ysp_tex0;\n"   /* the scene                          */
    "uniform sampler2D ysp_tex1;\n"   /* the CLUT, n x 3                    */
    "out vec4 ysp_out;\n"
    /* triple32 (Chris Wellons, hash-prospector, public domain): the
     * ygfx_hash() of the CPU, step for step (docs/gfx.md, v0.9) */
    "uint ysp_hash(uint x) {\n"
    "    x ^= x >> 17u; x *= 0xed5ad4bbu;\n"
    "    x ^= x >> 11u; x *= 0xac4c1b51u;\n"
    "    x ^= x >> 15u; x *= 0x31848babu;\n"
    "    x ^= x >> 14u;\n"
    "    return x;\n"
    "}\n"
    /* A 2D key: the point's index times an odd key of the seed, XOR a second
     * key, through one hash. Injective over any 65536 x 65536 window, and a
     * point's value is unrelated across seeds (docs/gfx.md, v0.9: the
     * nested form gave rows that were copies of each other) */
    "uint ysp_hash2(uvec2 c, uint seed) {\n"
    "    uint k = ysp_hash(seed), m = ysp_hash(k ^ 0x9E3779B9u);\n"
    "    return ysp_hash((c.x + (c.y << 16u)) * (k | 1u) ^ m);\n"
    "}\n"
    "void main() {\n"
    "    ivec2 q = ivec2(gl_FragCoord.xy);\n"
    "    if (ysp_outp.w > 0) q.y = ysp_outp.w - 1 - q.y;\n"
    "    vec3 v = clamp(texelFetch(ysp_tex0, q, 0).rgb, 0.0, 1.0);\n"
    "    int n = ysp_outp.x;\n"
    "    float mx = float(ysp_outp.y);\n"
    "    vec3 d;\n"
    "    for (int c = 0; c < 3; c++) {\n"
    "        float x = v[c] * float(n - 1);\n"
    "        int i0 = min(int(x), n - 2);\n"
    "        float a = texelFetch(ysp_tex1, ivec2(i0, c), 0).r;\n"
    "        float b = texelFetch(ysp_tex1, ivec2(i0 + 1, c), 0).r;\n"
    "        d[c] = a + (b - a) * (x - float(i0));\n"
    "    }\n"
    "    float t = 0.5;\n"
    "    if (ysp_outp.z == 1) {\n"   /* Bayer 8 x 8 by bit interleaving */
    "        int x = q.x & 7, y = q.y & 7, z = x ^ y;\n"
    "        int b = ((z & 1) << 5) | ((x & 1) << 4) | ((z & 2) << 2) | ((x & 2) << 1) | ((z & 4) >> 1) | ((x & 4) >> 2);\n"
    "        t = (float(b) + 0.5) / 64.0;\n"
    "    } else if (ysp_outp.z == 2) {\n"
    "        uint h = ysp_hash2(uvec2(q), ysp_seed.x ^ ysp_hash(ysp_seed.z));\n"
    "        t = (float(h >> 8u) + 0.5) * (1.0 / 16777216.0);\n"
    "    }\n"
    "    vec3 code = clamp(floor(d * mx + t), 0.0, mx);\n"
    "    ysp_out = vec4(code / mx, 1.0);\n"
    "}\n";

static const char ygfx__glsl_vs_output[] =
    "void main() {\n"
    "    vec2 c = vec2(float((gl_VertexID & 1) << 2), float((gl_VertexID & 2) << 1)) - 1.0;\n"
    "    gl_Position = vec4(c, 0.0, 1.0);\n"
    "}\n";

/* Appends to a buffer; the total length is counted even when it overflows. */
typedef struct ygfx__sb { char* p; size_t cap, n; } ygfx__sb;
static void ygfx__sb_add(ygfx__sb* b, const char* s) {
    size_t k = strlen(s);
    if (b->p && b->n + k < b->cap) memcpy(b->p + b->n, s, k + 1);
    b->n += k;
}

/* defs go after the common header (#version must come first); a mode
 * below 0: the body has its own main (the vector program). */
static size_t ygfx__fs_text2(const char* defs, const char* body, int mode, char* out, size_t cap) {
    ygfx__sb b;
    b.p = out; b.cap = cap; b.n = 0;
    if (out && cap) out[0] = '\0';
    ygfx__sb_add(&b, ygfx__glsl_common);
    ygfx__sb_add(&b, defs);
    ygfx__sb_add(&b, ygfx__glsl_access);
    ygfx__sb_add(&b, ygfx__glsl_fs_lib);
    ygfx__sb_add(&b, ygfx__glsl_fs_lib2);
    /* The compiler's log then gives the body's own line numbers. */
    ygfx__sb_add(&b, "#line 1\n");
    ygfx__sb_add(&b, body);
    ygfx__sb_add(&b, "\n");
    if (mode >= 0)
        ygfx__sb_add(&b, mode == YGFX_COLOR ? ygfx__glsl_main_color
                           : (mode == YGFX_ADD ? ygfx__glsl_main_add : ygfx__glsl_main_mod));
    return b.n;
}

static size_t ygfx__fs_text(const char* body, ygfx_shader_mode mode, char* out, size_t cap) {
    return ygfx__fs_text2("", body, (int)mode, out, cap);
}

YGFX_API int ygfx_shader_wrap(const char* body, ygfx_shader_mode mode, char* out, size_t cap) {
    size_t n;
    if (!body) return YGFX_ERR_ARG;
    n = ygfx__fs_text(body, mode, out, cap);
    if (!out || n >= cap) return -1000 - (int)n;
    return (int)n;
}

/* --- core ------------------------------------------------------------------ */

enum {
    YGFX__B_SHAPE, YGFX__B_GRATING, YGFX__B_GABOR, YGFX__B_NOISE,
    YGFX__B_IMAGE_COLOR, YGFX__B_IMAGE_MOD, YGFX__B_DOTS, YGFX__B_IMAGE_ADD,
    YGFX__B_VECTOR, YGFX__B_GLYPHS
};

/* Test seam, not API: the pixel test sets it before open to read the scene
 * in float32. */
static int ygfx__test_scene32 = 0;

#define YGFX__BLOCK 256u
#define YGFX__RES_TARGET 0x1     /* ygfx__res.flags: a render target */
/* The local y axis sign the coordinate convention gives (ygfx__place). */
#define YGFX__Y_SIGN (-1.0f)
/* Frame blocks at the start of the staging: the scene's, then one per
 * target pass; the stim blocks follow. */
#define YGFX__FRAME_SLOTS (1 + YGFX_MAX_PASSES)
#define YGFX__FRAME_BYTES ((uint32_t)YGFX__FRAME_SLOTS * YGFX__BLOCK)
#define YGFX__STIM_RANGE ((uint32_t)YGFX__BATCH_VEC4 * 16u)   /* the block as declared */

/* --- the program cache (PROGRAM CACHE) ---------------------------------------- */

/* Test seams, not API: the test forces key collisions (mask 0) and a new
 * identity (another driver) with them. */
static uint64_t ygfx__test_key_mask = ~(uint64_t)0;
static const char* ygfx__test_ident = "";

/* An entry: this header, then the driver's binary. ANGLE links a binary
 * with a byte changed (measured: 10 of 11 programs), so nothing reaches the
 * driver that does not match its hash under the same key material. */
#define YGFX__PROG_MAGIC "YSPPROG1"
#define YGFX__PROG_HEAD  48u
typedef struct ygfx__prog_head {
    char     magic[8];
    uint32_t head_bytes, format;
    uint64_t key;                  /* FNV-1a 64 of the key material            */
    uint64_t key2;                 /* a second hash of it: a key collision is
                                    * caught, not trusted                      */
    uint64_t payload_hash;         /* FNV-1a 64 of the binary                  */
    uint32_t material_bytes, payload_bytes;
} ygfx__prog_head;
typedef char ygfx__prog_head_size[sizeof(ygfx__prog_head) == YGFX__PROG_HEAD ? 1 : -1];

/* Two unrelated byte hashes over the same bytes. */
typedef struct ygfx__h2 { uint64_t a, b; uint32_t n; } ygfx__h2;
static void ygfx__h2_add(ygfx__h2* h, const void* data, size_t n) {
    const unsigned char* p = (const unsigned char*)data;
    uint64_t a = h->a, b = h->b;
    size_t i;
    for (i = 0; i < n; i++) {
        a = (a ^ p[i]) * 0x100000001B3ull;
        b = (b ^ p[i]) * 0x9E3779B97F4A7C15ull;
        b ^= b >> 29;
    }
    h->a = a; h->b = b; h->n += (uint32_t)n;
}
static void ygfx__h2_str(ygfx__h2* h, const char* s) { ygfx__h2_add(h, s, strlen(s) + 1); }
static uint64_t ygfx__fnv64(const void* data, size_t n) {
    ygfx__h2 h;
    h.a = 0xCBF29CE484222325ull; h.b = 0; h.n = 0;
    ygfx__h2_add(&h, data, n);
    return h.a;
}

/* What a build left to store once its program is finished. */
static void ygfx__prog_store2(ygfx_gfx* g, uint32_t id, const ygfx__prec* r) {
    ygfx__prog_head h;
    unsigned char* buf;
    uint32_t fmt = 0;
    int n, m;
    if (!r->store || !id) return;
    n = g->be->pipeline_binary(g->bctx, id, NULL, 0, &fmt);
    if (n <= 0) { g->progs.store_failed++; return; }
    buf = (unsigned char*)malloc(YGFX__PROG_HEAD + (size_t)n);
    if (!buf) { g->progs.store_failed++; return; }
    m = g->be->pipeline_binary(g->bctx, id, buf + YGFX__PROG_HEAD, (size_t)n, &fmt);
    if (m <= 0 || m > n) { free(buf); g->progs.store_failed++; return; }
    memset(&h, 0, sizeof h);
    memcpy(h.magic, YGFX__PROG_MAGIC, 8);
    h.head_bytes = YGFX__PROG_HEAD; h.format = fmt;
    h.key = r->key; h.key2 = r->key2; h.material_bytes = r->material;
    h.payload_bytes = (uint32_t)m;
    h.payload_hash = ygfx__fnv64(buf + YGFX__PROG_HEAD, (size_t)m);
    memcpy(buf, &h, sizeof h);
    if (g->cache->store(g->cache->user, r->key, buf, YGFX__PROG_HEAD + (size_t)m) < 0) g->progs.store_failed++;
    else g->progs.stored++;
    free(buf);
}

static void ygfx__prog_store(ygfx_gfx* g, uint32_t id, const ygfx__prec* r) {
    int64_t t0;
    if (!r->store || !id) return;
    t0 = (int64_t)yrt_now_ns();
    ygfx__prog_store2(g, id, r);
    g->progs.store_ns += (int64_t)yrt_now_ns() - t0;
}

/* Builds one program from its text, from desc.cache when an entry there is
 * whole and for this driver; else from source, leaving r to store it. inst:
 * 0 none, 1 dots, 2 glyphs. */
static int ygfx__build(ygfx_gfx* g, const char* vs, const char* fs, int blend, int inst, int deferred,
                         uint32_t* id, ygfx__prec* r) {
    ygfx_pipeline_src src;
    unsigned char* buf = NULL;
    int rc, had = 0;
    memset(&src, 0, sizeof src);
    memset(r, 0, sizeof *r);
    src.vs = vs;
    src.fs = fs;
    src.blend = blend;
    src.instance_attr = inst;
    src.deferred = deferred && g->be->pipeline_finish ? 1 : 0;
    if (g->cache && g->cache->load && g->cache->store && g->caps.binary_format && g->be->pipeline_binary) {
        ygfx__h2 h;
        size_t n;
        uint32_t f = g->caps.binary_format;
        h.a = 0xCBF29CE484222325ull; h.b = 0x2545F4914F6CDD1Dull; h.n = 0;
        ygfx__h2_str(&h, g->caps.vendor);
        ygfx__h2_str(&h, g->caps.renderer);
        ygfx__h2_str(&h, g->caps.version);
        ygfx__h2_str(&h, g->caps.glsl);
        ygfx__h2_str(&h, ygfx__test_ident);
        ygfx__h2_add(&h, &f, sizeof f);
        ygfx__h2_str(&h, vs);
        ygfx__h2_str(&h, fs);
        r->key = h.a & ygfx__test_key_mask;
        r->key2 = h.b;
        r->material = h.n;
        r->store = 1;
        src.retrievable = 1;
        n = g->cache->load(g->cache->user, r->key, NULL, 0);
        if (n > 0) {
            ygfx__prog_head hd;
            had = 1;
            buf = n >= YGFX__PROG_HEAD && n < ((size_t)1 << 28) ? (unsigned char*)malloc(n) : NULL;
            if (buf && g->cache->load(g->cache->user, r->key, buf, n) == n) {
                memcpy(&hd, buf, sizeof hd);
                if (memcmp(hd.magic, YGFX__PROG_MAGIC, 8) == 0 && hd.head_bytes == YGFX__PROG_HEAD &&
                    hd.key == r->key && hd.key2 == r->key2 && hd.material_bytes == r->material &&
                    (size_t)hd.payload_bytes == n - YGFX__PROG_HEAD &&
                    hd.payload_hash == ygfx__fnv64(buf + YGFX__PROG_HEAD, hd.payload_bytes)) {
                    src.binary = buf + YGFX__PROG_HEAD;
                    src.binary_size = hd.payload_bytes;
                    src.binary_format = hd.format;
                }
            }
        }
    }
    rc = g->be->pipeline_make(g->bctx, &src, id, g->error, sizeof g->error);
    free(buf);
    if (rc == 1) { g->progs.loaded++; r->store = 0; return YGFX_OK; }
    if (rc < 0) { r->store = 0; return rc; }
    g->progs.compiled++;
    if (had) g->progs.rejected++;
    if (!src.deferred) ygfx__prog_store(g, *id, r);   /* finished already */
    if (!src.deferred) r->store = 0;
    return rc;
}

/* mode below 0: the body has its own main and blends OVER. */
static int ygfx__make_pipe(ygfx_gfx* g, const char* defs, const char* body, int mode, int inst, int deferred,
                             uint32_t* id, const char* name, ygfx__prec* r) {
    size_t nv, nf;
    char* vs;
    char* fs;
    int rc;
    nv = strlen(ygfx__glsl_common) + strlen(defs) + strlen(ygfx__glsl_vs_body) + 1;
    nf = ygfx__fs_text2(defs, body, mode, NULL, 0) + 1;
    vs = (char*)malloc(nv + nf);
    if (!vs) { ygfx__set_error(g->error, sizeof g->error, "ysp_gfx: out of memory"); return YGFX_ERR_FULL; }
    fs = vs + nv;
    /* #version must come first, so the defines go after the common header. */
    snprintf(vs, nv, "%s%s%s", ygfx__glsl_common, defs, ygfx__glsl_vs_body);
    ygfx__fs_text2(defs, body, mode, fs, nf);
    /* ADD and MODULATION add */
    rc = ygfx__build(g, vs, fs, (mode == YGFX_COLOR || mode < 0) ? YGFX_BLEND_OVER : YGFX_BLEND_ADD,
                       inst, deferred, id, r);
    free(vs);
    if (rc < 0 && name) {
        char tmp[sizeof g->error];
        memcpy(tmp, g->error, sizeof tmp);
        ygfx__set_error(g->error, sizeof g->error, "%s (%s)", tmp, name);
    }
    return rc;
}

static int ygfx__make_output(ygfx_gfx* g, ygfx__prec* r) {
    size_t nv = strlen(ygfx__glsl_common) + strlen(ygfx__glsl_vs_output) + 1;
    size_t nf = strlen(ygfx__glsl_common) + strlen(ygfx__glsl_output) + 1;
    char* vs = (char*)malloc(nv + nf);
    int rc;
    if (!vs) return YGFX_ERR_FULL;
    snprintf(vs, nv, "%s%s", ygfx__glsl_common, ygfx__glsl_vs_output);
    snprintf(vs + nv, nf, "%s%s", ygfx__glsl_common, ygfx__glsl_output);
    rc = ygfx__build(g, vs, vs + nv, YGFX_BLEND_NONE, 0, 1, &g->output_pipe, r);
    free(vs);
    return rc;
}

static void ygfx__push(ygfx_gfx* g, uint16_t kind, uint64_t t, uint32_t aux, const uint32_t* u, int n, int64_t i64) {
    yrt_event ev;
    int i;
    if (!g->ring) return;
    memset(&ev, 0, sizeof ev);
    ev.source = (uint16_t)YRT_SRC_GFX;
    ev.kind = kind;
    ev.t_ns = t;
    ev.aux = aux;
    if (u) for (i = 0; i < n && i < (int)(sizeof ev.u.u32 / sizeof ev.u.u32[0]); i++) ev.u.u32[i] = u[i];
    else ev.u.i64[0] = i64;
    yrt_ring_push(g->ring, &ev);
}

static uint32_t ygfx__crc32(const void* data, size_t n) {
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

static int ygfx__eotf_upload(ygfx_gfx* g, const float* lut, int n);

static int ygfx__lut_upload(ygfx_gfx* g, const float* lut, int n) {
    return g->be->texture_update(g->bctx, g->lut, 0, 0, n, 3, lut, (size_t)n * sizeof(float));
}

static void ygfx__mul3(const double* m, const double* v, double* out);

/* PAINT's spaces, from ysp/color.h's context: Oklab's cone response
 * (before the cube root) from device RGB, on absolute XYZ against the
 * display's white (black included: ok_black), and DKL about desc.background
 * with desc.cones and desc.lum. ok_off is the black term in rgb, which the
 * shader adds after its matrix. */
static void ygfx__paint_spaces(ygfx_gfx* g) {
    const ycol_ctx* cx = &g->color;
    int k;
    if (cx->can & YCOL_CAN_XYZ) {
        memcpy(g->ok_rgb2lms, cx->ok_rgb_to_lms, sizeof g->ok_rgb2lms);
        memcpy(g->ok_lms2rgb, cx->ok_lms_to_rgb, sizeof g->ok_lms2rgb);
        memcpy(g->ok_black, cx->ok_black, sizeof g->ok_black);
        ygfx__mul3(g->ok_lms2rgb, g->ok_black, g->ok_off);
        for (k = 0; k < 3; k++) g->ok_off[k] = -g->ok_off[k];
        g->has_oklab = 1;
    }
    if (cx->can & YCOL_CAN_BG) {
        memcpy(g->rgb2dkl, cx->rgb_to_dkl, sizeof g->rgb2dkl);
        memcpy(g->dkl2rgb, cx->dkl_to_rgb, sizeof g->dkl2rgb);
        g->has_dkl = 1;
    }
}

/* The built-in programs. The defines change what a program holds, not what
 * it computes: SHAPE's offset is 0 for a v0.2 stimulus, and d - 0 is d. */
static const struct ygfx__builtin_def {
    const char* defs; const char* body; int mode; int inst; const char* name;
} ygfx__builtins[YGFX__N_BUILTIN] = {
    { "#define YSP_OFFSET 1\n", ygfx__body_shape, YGFX_COLOR, 0, "shape" },
    { "", ygfx__body_grating, YGFX_MODULATION, 0, "grating" },
    { "", ygfx__body_gabor,   YGFX_MODULATION, 0, "gabor" },
    { "", ygfx__body_noise,   YGFX_MODULATION, 0, "noise" },
    { "#define YSP_STRAIGHT 1\n", NULL, YGFX_COLOR, 0, "image" },
    { "", NULL,                 YGFX_MODULATION, 0, "image modulation" },
    { "#define YSP_DOTS 1\n", ygfx__body_dots, YGFX_COLOR, 1, "dots" },
    { "", NULL,                 YGFX_ADD,        0, "image add" },
    { "#define YSP_VECTOR 1\n", NULL, -1, 0, "vector" },
    { "#define YSP_GLYPHS 1\n#define YSP_OFFSET 1\n", ygfx__body_shape, YGFX_COLOR, 2, "glyphs" },
};

/* Specialized vector programs (KINDS): the vector program's text with
 * defines that remove what one case cannot reach, so each computes what the
 * generic program computes for that case (the test compares them bit for
 * bit). Built only where one closed at least half of a cost bar's gap
 * (docs/gfx.md, "Kind-specialized vector programs"). kind is the
 * shader's primitive kind (ygfx__vkind()); tier: PLAIN one primitive, no
 * dash, trim, paint or fx; ALONG one primitive with dashes or trim, no paint
 * or fx; FOLD a compound of that kind only, no paint. */
enum { YGFX__VS_PLAIN, YGFX__VS_ALONG, YGFX__VS_FOLD };
#define YGFX__VS_ONE "#define YSP_VECTOR 1\n#define YSP_ONE 1\n#define YSP_PAINT 0\n"
static const struct ygfx__vspec_def {
    int kind, tier; const char* defs; const char* name;
} ygfx__vspecs[YGFX__N_VSPEC] = {
    { 8, YGFX__VS_PLAIN, YGFX__VS_ONE "#define YSP_VK 0x100\n#define YSP_V1 1\n#define YSP_WS 0\n#define YSP_ALONG 0\n#define YSP_FX 0\n",
      "vector RRECT" },
    { 1, YGFX__VS_ALONG, YGFX__VS_ONE "#define YSP_VK 0x2\n#define YSP_V1 1\n#define YSP_WS 1\n#define YSP_FX 0\n",
      "vector CIRCLE, dashes" },
    { 1, YGFX__VS_FOLD,  YGFX__VS_ONE "#define YSP_VK 0x2\n#define YSP_WS 0\n#define YSP_ALONG 0\n",
      "vector CIRCLE compound" },
};
/* A draw takes a specialized program only when its quad covers this many
 * px. A program switch, with the draw it splits off a batch, cost 7 to 24
 * us of CPU through ANGLE's D3D11 path; GPU time broke even at 128 x 128 px
 * stimuli and won at 256 x 256 (measured, docs/gfx.md). A test seam,
 * not API: the pixel test sets 0 or a huge value. */
static double ygfx__vspec_area = 65536.0;

/* A built-in program's body, allocated (the vector program is in pieces, the
 * image ones a fetch and a main); NULL when out of memory. */
static char* ygfx__builtin_body(int i) {
    const char* parts[12];
    int np = 0, k;
    size_t n = 1;
    char* out;
    if (ygfx__builtins[i].body) parts[np++] = ygfx__builtins[i].body;
    else if (ygfx__builtins[i].mode < 0)
        for (k = 0; k < (int)(sizeof ygfx__glsl_vec / sizeof ygfx__glsl_vec[0]); k++) parts[np++] = ygfx__glsl_vec[k];
    else {
        parts[np++] = ygfx__body_image_fetch;
        parts[np++] = ygfx__builtins[i].mode == YGFX_COLOR ? ygfx__body_image_color
                    : (ygfx__builtins[i].mode == YGFX_ADD ? ygfx__body_image_add : ygfx__body_image_mod);
    }
    for (k = 0; k < np; k++) n += strlen(parts[k]);
    out = (char*)malloc(n);
    if (!out) return NULL;
    n = 0;
    for (k = 0; k < np; k++) { size_t m = strlen(parts[k]); memcpy(out + n, parts[k], m); n += m; }
    out[n] = '\0';
    return out;
}

#define YGFX__START_PARALLEL 1   /* every program submitted               */
#define YGFX__START_SERIAL   2   /* nothing submitted: no parallel compile */

/* The open's first part: the desc's checks and the backend. False with the
 * error set; nothing to undo then. */
static bool ygfx__open_a(ygfx_gfx* g, const ygfx_desc* d) {
    ygfx_backend_open in;
    int rc, i;
    if (g->open || g->started) { ygfx__set_error(g->error, sizeof g->error, "ysp_gfx: already open"); return false; }
    memset(g, 0, sizeof *g);
    if (!d) { ygfx__set_error(g->error, sizeof g->error, "ysp_gfx: no desc"); return false; }
    g->cache = d->cache;
    g->calibrated = d->cal != NULL;
    if (d->output != YGFX_OUT_8) {
        ygfx__set_error(g->error, sizeof g->error, "ysp_gfx: only 8-bit output in v0.1: 10-bit needs an "
                          "RGB10A2 back buffer from ysp_screen, Mono++ and Color++ need a device to verify");
        return false;
    }
    if (d->stereo != YGFX_MONO) {
        ygfx__set_error(g->error, sizeof g->error, "ysp_gfx: only MONO in v0.1");
        return false;
    }
    if (d->units == YGFX_DEG && (d->view.distance_mm <= 0 || d->view.width_mm <= 0)) {
        ygfx__set_error(g->error, sizeof g->error, "ysp_gfx: YGFX_DEG needs view.distance_mm and view.width_mm");
        return false;
    }
    for (i = 0; i < 3; i++) {
        if (!(d->background[i] >= 0.0f && d->background[i] <= 1.0f)) {
            ygfx__set_error(g->error, sizeof g->error, "ysp_gfx: background outside 0..1");
            return false;
        }
    }
    if (d->cal) {
        char e[200];
        if (ycol_cal_check(d->cal, e, sizeof e) < 0) {
            ygfx__set_error(g->error, sizeof g->error, "ysp_gfx: calibration: %s", e);
            return false;
        }
    }
    memset(&in, 0, sizeof in);
    g->screen = d->screen;
    if (d->screen) {
        yscr_caps sc;
        if (!yscr_is_open(d->screen)) { ygfx__set_error(g->error, sizeof g->error, "ysp_gfx: the screen is not open"); return false; }
        yscr_get_caps(d->screen, &sc);
        g->w = sc.mode.w; g->h = sc.mode.h;
        /* The simulated display has no size: desc.width, height, or 800 x 600. */
        if (g->w <= 0 || g->h <= 0) { g->w = d->width > 0 ? d->width : 800; g->h = d->height > 0 ? d->height : 600; }
        /* ANGLE's client-buffer pbuffer puts GL's row 0 at the top of the
         * D3D texture (probe, docs/gfx.md). */
        g->origin_top = sc.backend == YSCR_BACKEND_DXGI_FLIP || sc.backend == YSCR_BACKEND_COMPOSITION;
        if (yscr_gl_proc(d->screen, "glGetString")) {
            in.gl_proc = (yscr_proc (*)(void*, const char*))yscr_gl_proc;
            in.gl_ctx = d->screen;
        }
    } else if (d->gl_proc) {
        in.gl_proc = d->gl_proc;
        in.gl_ctx = d->gl_ctx;
        g->w = d->width; g->h = d->height;
    } else {
        ygfx__set_error(g->error, sizeof g->error, "ysp_gfx: desc.screen or desc.gl_proc is required");
        return false;
    }
    if (d->rows == YGFX_ROWS_BOTTOM_UP) g->origin_top = 0;
    if (d->rows == YGFX_ROWS_TOP_DOWN) g->origin_top = 1;
    if (g->w <= 0 || g->h <= 0) { ygfx__set_error(g->error, sizeof g->error, "ysp_gfx: no framebuffer size"); return false; }
    in.w = g->w; in.h = g->h;
    if (d->backend) { g->be = d->backend; g->bctx = d->backend_ctx; }
    else if (in.gl_proc) { g->be = &ygfx__gl_backend; g->bctx = g->backend_mem; }
    else { g->be = &ygfx__null_backend; g->bctx = g->backend_mem; }
    /* a version 3 table ends before the capture calls: CAPTURE checks */
    if (g->be->version != YGFX_BACKEND_VERSION && g->be->version != 3) {
        ygfx__set_error(g->error, sizeof g->error, "ysp_gfx: backend version %u, want 3 or %u", g->be->version, YGFX_BACKEND_VERSION);
        return false;
    }
    rc = g->be->open(g->bctx, &in, &g->caps, g->error, sizeof g->error);
    if (rc < 0) { if (!g->error[0]) ygfx__set_error(g->error, sizeof g->error, "ysp_gfx: backend open failed"); return false; }
    g->open = 1;   /* from here ygfx_close() undoes what was made */
    g->features = (g->caps.features & (YGFX_FEAT_IMPORT_GL | YGFX_FEAT_IMPORT_D3D11 | YGFX_FEAT_IMPORT_NV12)) | YGFX_FEAT_PLANAR | YGFX_FEAT_INSTANCES |
                  (g->caps.binary_format && g->be->pipeline_binary ? YGFX_FEAT_PROGRAM_CACHE : 0u);
#if defined(YSCR_HAS_GL_EPOCH)
    if (g->screen) { g->epoch = yscr_gl_epoch(g->screen); g->generation = yscr_gl_generation(g->screen); }
#endif
    return true;
}

/* The resources, and every program submitted; ygfx_close() on a failure. */
static bool ygfx__open_b(ygfx_gfx* g, const ygfx_desc* d) {
    ygfx_texture_src ts;
    int rc, i;
    static const float identity[6] = { 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f };
    if (!g->caps.color_buffer_float) {
        ygfx__set_error(g->error, sizeof g->error, "ysp_gfx: GL_EXT_color_buffer_float is missing (%s)", g->caps.renderer);
        goto fail;
    }
    /* RGBA16F: an RGBA32F scene took 1.4 to 1.7 times the GPU time on
     * blend-heavy frames and gives nothing at 8-bit output (docs/gfx.md).
     * The test reads RGBA32F to measure shader error below half precision. */
    g->scene_format = ygfx__test_scene32 ? YGFX_RGBA32F : YGFX_RGBA16F;
    if (g->scene_format == YGFX_RGBA32F && !g->caps.float_blend) {
        ygfx__set_error(g->error, sizeof g->error, "ysp_gfx: an RGBA32F scene needs GL_EXT_float_blend (%s)", g->caps.renderer);
        goto fail;
    }
    if (g->caps.ubo_align > (int32_t)YGFX__BLOCK || g->caps.max_ubo < (int32_t)YGFX__STIM_RANGE) {
        ygfx__set_error(g->error, sizeof g->error, "ysp_gfx: uniform buffer alignment %d or size %d not supported",
                          g->caps.ubo_align, g->caps.max_ubo);
        goto fail;
    }
    for (i = 0; i < 3; i++) g->bg[i] = d->background[i];
    g->ppu = 1.0f;
    if (d->units == YGFX_DEG)
        g->ppu = (float)((double)g->w / d->view.width_mm * d->view.distance_mm * tan(3.14159265358979323846 / 180.0));
    g->dither = d->dither;
    g->seed = d->seed;
    g->ring = d->ring;
    g->max_draws = d->max_draws > 0 ? d->max_draws : 1024;
    g->max_batch = YGFX__BATCH;
    g->inst_cap = d->max_instances > 0 ? d->max_instances : 16384;
    g->t_open = (int64_t)yrt_now_ns();
    /* The one allocation: the frame's uniform staging and its draw queue. */
    g->staging = (unsigned char*)calloc((size_t)YGFX__FRAME_BYTES + ((size_t)g->max_draws + 16) * YGFX__BLOCK, 1);
    g->cur_w = g->w; g->cur_h = g->h; g->cur_top = 0;
    g->cmds = (ygfx__cmd*)calloc((size_t)g->max_draws, sizeof(ygfx__cmd));
    g->staging2 = (unsigned char*)calloc((size_t)YGFX__FRAME_BYTES + ((size_t)g->max_draws + 16) * YGFX__BLOCK, 1);
    g->cmds2 = (ygfx__cmd*)calloc((size_t)g->max_draws, sizeof(ygfx__cmd));
    g->ro_next = (int32_t*)calloc((size_t)g->max_draws, sizeof(int32_t));
    g->ro_batch = (ygfx__rbatch*)calloc((size_t)g->max_draws, sizeof(ygfx__rbatch));
    if (!g->staging || !g->cmds || !g->staging2 || !g->cmds2 || !g->ro_next || !g->ro_batch) {
        ygfx__set_error(g->error, sizeof g->error, "ysp_gfx: out of memory");
        goto fail;
    }
    /* A range starts at any block and is as long as the declared array. */
    g->ubo_bytes = YGFX__FRAME_BYTES + ((uint32_t)g->max_draws + 16) * YGFX__BLOCK + YGFX__STIM_RANGE;
    for (i = 0; i < YGFX__UBO_RING; i++) {
        if (g->be->buffer_make(g->bctx, YGFX_BUFFER_UNIFORM, g->ubo_bytes, &g->ubo[i]) < 0) {
            ygfx__set_error(g->error, sizeof g->error, "ysp_gfx: uniform buffer of %u bytes", g->ubo_bytes);
            goto fail;
        }
    }
    memset(&ts, 0, sizeof ts);
    ts.w = g->w; ts.h = g->h; ts.format = g->scene_format; ts.target = true;
    if (g->be->texture_make(g->bctx, &ts, &g->scene) < 0) {
        ygfx__set_error(g->error, sizeof g->error, "ysp_gfx: scene target %dx%d", g->w, g->h);
        goto fail;
    }
    memset(&ts, 0, sizeof ts);
    ts.w = YCOL_CAL_MAX_LUT; ts.h = 3; ts.format = YGFX_R32F;
    if (g->be->texture_make(g->bctx, &ts, &g->lut) < 0) {
        ygfx__set_error(g->error, sizeof g->error, "ysp_gfx: CLUT texture");
        goto fail;
    }
    memset(&ts, 0, sizeof ts);
    ts.w = 256; ts.h = 3; ts.format = YGFX_R32F;
    if (g->be->texture_make(g->bctx, &ts, &g->eotf) < 0) {
        ygfx__set_error(g->error, sizeof g->error, "ysp_gfx: transfer table texture");
        goto fail;
    }
    memset(&ts, 0, sizeof ts);
    ts.w = 256; ts.h = 256; ts.format = YGFX_R32F;
    if (g->be->texture_make(g->bctx, &ts, &g->gauss) < 0 ||
        g->be->texture_update(g->bctx, g->gauss, 0, 0, 256, 256, ygfx__gauss_table(), 256 * sizeof(float)) < 0) {
        ygfx__set_error(g->error, sizeof g->error, "ysp_gfx: Gaussian noise table");
        goto fail;
    }
    if (d->cal) {
        int n = d->cal->lut_n ? d->cal->lut_n : YCOL_CAL_MAX_LUT, c;
        float* tmp = (float*)malloc((size_t)n * 3 * sizeof(float));
        if (!tmp) { ygfx__set_error(g->error, sizeof g->error, "ysp_gfx: out of memory"); goto fail; }
        for (c = 0; c < 3; c++) memcpy(tmp + c * n, d->cal->lut[c], (size_t)n * sizeof(float));
        rc = ygfx__lut_upload(g, tmp, n);
        if (rc >= 0) rc = ygfx__eotf_upload(g, tmp, n);
        free(tmp);
        g->lut_n = n;
        g->cal_crc = d->cal->crc;
    } else {
        rc = ygfx__lut_upload(g, identity, 2);
        if (rc >= 0) rc = ygfx__eotf_upload(g, identity, 2);
        g->lut_n = 2;
    }
    if (rc < 0) { ygfx__set_error(g->error, sizeof g->error, "ysp_gfx: CLUT upload"); goto fail; }
    if (d->cal) {   /* PAINT's spaces and VIDEO's primaries: ysp/color.h's context */
        ycol_ctx_desc cd;
        char cerr[200];
        int k;
        memset(&cd, 0, sizeof cd);
        cd.cal = d->cal;
        cd.cones = d->cones;
        cd.lum = d->lum;
        for (k = 0; k < 3; k++) cd.background[k] = g->bg[k];
        if (ycol_ctx_init(&g->color, &cd, cerr, sizeof cerr) < 0) {
            ygfx__set_error(g->error, sizeof g->error, "ysp_gfx: %s", cerr);
            goto fail;
        }
        g->has_color = 1;
        ygfx__paint_spaces(g);
        g->has_xyz = (g->color.can & YCOL_CAN_XYZ) != 0;
    } else if (d->lum || d->cones) {
        ygfx__set_error(g->error, sizeof g->error, "ysp_gfx: desc.lum and desc.cones need desc.cal");
        goto fail;
    }
    for (i = 0; i < YGFX__N_BUILTIN; i++) {
        char* src = ygfx__builtin_body(i);
        if (!src) { ygfx__set_error(g->error, sizeof g->error, "ysp_gfx: out of memory"); goto fail; }
        rc = ygfx__make_pipe(g, ygfx__builtins[i].defs, src, ygfx__builtins[i].mode, ygfx__builtins[i].inst, 1,
                               &g->builtin[i], ygfx__builtins[i].name, &g->prec[i]);
        free(src);
        if (rc < 0) goto fail;
    }
    {   /* the specialized vector programs, compiled beside the others */
        char* src = ygfx__builtin_body(YGFX__B_VECTOR);
        if (!src) { ygfx__set_error(g->error, sizeof g->error, "ysp_gfx: out of memory"); goto fail; }
        for (i = 0; i < YGFX__N_VSPEC && rc >= 0; i++)
            rc = ygfx__make_pipe(g, ygfx__vspecs[i].defs, src, -1, 0, 1, &g->vspec[i], ygfx__vspecs[i].name, &g->vprec[i]);
        free(src);
        if (rc < 0) goto fail;
    }
    if (ygfx__make_output(g, &g->prec[YGFX__N_BUILTIN]) < 0) goto fail;
    return true;
fail:
    {
        char keep[sizeof g->error];
        memcpy(keep, g->error, sizeof keep);
        ygfx_close(g);
        memcpy(g->error, keep, sizeof keep);
    }
    return false;
}

/* The statuses, the cache's stores and the open record; t_start is the
 * call that finishes. */
static bool ygfx__open_c(ygfx_gfx* g, const ygfx_desc* d, int64_t t_start) {
    int i;
    if (g->be->pipeline_finish) {   /* the statuses, once every program is under way */
        for (i = 0; i < YGFX__N_BUILTIN; i++) {
            if (g->be->pipeline_finish(g->bctx, g->builtin[i], g->error, sizeof g->error) < 0) {
                char tmp[sizeof g->error];
                g->builtin[i] = 0;
                memcpy(tmp, g->error, sizeof tmp);
                ygfx__set_error(g->error, sizeof g->error, "%s (%s)", tmp, ygfx__builtins[i].name);
                goto fail;
            }
        }
        for (i = 0; i < YGFX__N_VSPEC; i++) {
            if (g->be->pipeline_finish(g->bctx, g->vspec[i], g->error, sizeof g->error) < 0) {
                char tmp[sizeof g->error];
                g->vspec[i] = 0;
                memcpy(tmp, g->error, sizeof tmp);
                ygfx__set_error(g->error, sizeof g->error, "%s (%s)", tmp, ygfx__vspecs[i].name);
                goto fail;
            }
        }
        if (g->be->pipeline_finish(g->bctx, g->output_pipe, g->error, sizeof g->error) < 0) { g->output_pipe = 0; goto fail; }
    }
    for (i = 0; i < YGFX__N_VSPEC; i++) ygfx__prog_store(g, g->vspec[i], &g->vprec[i]);
    for (i = 0; i < YGFX__N_BUILTIN; i++) ygfx__prog_store(g, g->builtin[i], &g->prec[i]);
    ygfx__prog_store(g, g->output_pipe, &g->prec[YGFX__N_BUILTIN]);
    g->be->reset(g->bctx);
    g->error[0] = '\0';
    {
        uint32_t u[10];
        u[0] = (uint32_t)g->lut_n; u[1] = (uint32_t)g->dither; u[2] = (uint32_t)d->output;
        u[3] = g->cal_crc; u[4] = d->cal ? d->cal->flags : 0u; u[5] = (uint32_t)g->w; u[6] = (uint32_t)g->h;
        g->progs.open_ns += (int64_t)yrt_now_ns() - t_start;
        u[7] = g->progs.loaded; u[8] = g->progs.compiled; u[9] = (uint32_t)(g->progs.open_ns / 1000000);
        ygfx__push(g, (uint16_t)YGFX_EV_OPEN, 0, (uint32_t)g->scene_format, u, 10, 0);
        if (g->has_color) {
            u[0] = g->color.id; u[1] = d->cal->crc; u[2] = d->lum ? d->lum->crc : 0u;
            u[3] = (uint32_t)d->cones; u[4] = g->color.can;
            ygfx__push(g, (uint16_t)YGFX_EV_COLOR, 0, 0, u, 5, 0);
        }
    }
    return true;
fail:
    {
        char keep[sizeof g->error];
        memcpy(keep, g->error, sizeof keep);
        ygfx_close(g);
        memcpy(g->error, keep, sizeof keep);
    }
    return false;
}

YGFX_API bool ygfx_open(ygfx_gfx* g, const ygfx_desc* d) {
    int64_t t_start = (int64_t)yrt_now_ns();
    if (!g) return false;
    if (g->started) {   /* after ygfx_open_start(): its desc, not this one */
        int st = g->started;
        g->started = 0;
        g->open = 1;
#if defined(YSCR_HAS_GL_EPOCH)
        if (g->screen && (!yscr_is_open(g->screen) || yscr_gl_generation(g->screen) != g->generation)) {
            g->open = 0;   /* the context and every object are gone */
            ygfx_close(g);
            ygfx__set_error(g->error, sizeof g->error, "ysp_gfx: the screen closed after ygfx_open_start()");
            return false;
        }
#endif
        if (st == YGFX__START_SERIAL && !ygfx__open_b(g, &g->sdesc)) return false;
        return ygfx__open_c(g, &g->sdesc, t_start);
    }
    if (!ygfx__open_a(g, d)) return false;
    if (!ygfx__open_b(g, d)) return false;
    return ygfx__open_c(g, d, t_start);
}

YGFX_API bool ygfx_open_start(ygfx_gfx* g, const ygfx_desc* d) {
    int64_t t_start = (int64_t)yrt_now_ns();
    if (!g) return false;
    if (!ygfx__open_a(g, d)) return false;
    g->sdesc = *d;
    if (g->caps.parallel_compile) {
        if (!ygfx__open_b(g, d)) return false;
        g->started = YGFX__START_PARALLEL;
    } else {
        g->started = YGFX__START_SERIAL;
    }
    g->start_mode = g->started;
    g->open = 0;   /* nothing but ygfx_open() and ygfx_close() until it finishes */
    g->progs.open_ns = (int64_t)yrt_now_ns() - t_start;
    return true;
}

static void ygfx__cap_stop(ygfx_gfx* g, int gl_ok, ygfx_capture_stats* out);
static void ygfx__cap_records(struct ygfx__cap* c, const yscr_frame* f);
static void ygfx__cap_frame(ygfx_gfx* g);

YGFX_API void ygfx_close(ygfx_gfx* g) {
    if (!g) return;
    if ((g->open || g->started) && g->be) {
        int gone = 0;
#if defined(YSCR_HAS_GL_EPOCH)
        /* a screen that closed (yscr_open() failed after the hook) took
         * the context and every object with it */
        gone = g->screen && (!yscr_is_open(g->screen) || yscr_gl_generation(g->screen) != g->generation);
#endif
        if (g->cap) ygfx__cap_stop(g, !gone && g->open, NULL);
        if (!gone) {
            if (g->screen) yscr_bind(g->screen);
            g->be->close(g->bctx);
        }
    }
    if (g->cap) ygfx__cap_stop(g, 0, NULL);   /* a lost context closed the handle */
    free(g->staging);
    free(g->cmds);
    free(g->staging2);
    free(g->cmds2);
    free(g->ro_next);
    free(g->ro_batch);
    free(g->inst_staging);
    {
        int i;
        for (i = 0; i < YGFX_MAX_PIPELINES; i++) free(g->pipe_body[i]);
        for (i = 0; i < YGFX_MAX_CSETS; i++) free(g->cset[i].bbox);
    }
    {
        char keep[sizeof g->error];
        memcpy(keep, g->error, sizeof keep);
        memset(g, 0, sizeof *g);
        memcpy(g->error, keep, sizeof keep);
    }
}

YGFX_API const char* ygfx_error(const ygfx_gfx* g) { return g ? g->error : "ysp_gfx: no handle"; }
YGFX_API bool ygfx_is_open(const ygfx_gfx* g) { return g && g->open; }
YGFX_API uint64_t ygfx_clipped(const ygfx_gfx* g) { return g ? g->clipped_total : 0; }

static size_t ygfx__fc_load(void* user, uint64_t key, void* dst, size_t cap);

/* Truncation is the contract here, as for snprintf: the return value is the
 * length needed. gcc 16 at -O2 inlines a caller's fixed buffer and warns that
 * the cache folder (up to 511 bytes) may not fit. */
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ >= 7
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
#endif
YGFX_API int ygfx_describe(const ygfx_gfx* g, char* buf, size_t cap) {
    static const char* const dith[] = { "none", "ordered", "noise" };
    const char* where = "off";
    const char* unused = "";
    if (!g || !g->open) return snprintf(buf, cap, "ysp_gfx: closed");
    if (g->cache && g->cache->load == ygfx__fc_load) where = ((const ygfx_file_cache*)g->cache->user)->dir;
    else if (g->cache) where = "(caller)";
    if (g->cache && (!g->caps.binary_format || !g->be->pipeline_binary)) unused = " (unused: no binary format)";
    return snprintf(buf, cap, "ysp_gfx %s: %s, %s, %dx%d, scene %s, CLUT %d%s, dither %s, %.4g px/unit, origin %s, program cache %s%s: %u hit / %u miss (%u rejected)%s",
                    YGFX_VERSION_STRING, g->be->name, g->caps.renderer, g->w, g->h,
                    g->scene_format == YGFX_RGBA32F ? "RGBA32F" : "RGBA16F", g->lut_n,
                    g->cal_crc ? " (calibration)" : " (identity)", dith[g->dither <= 2 ? g->dither : 0],
                    (double)g->ppu, g->origin_top ? "top" : "bottom", where, unused,
                    g->progs.loaded, g->progs.compiled, g->progs.rejected,
                    g->start_mode == YGFX__START_PARALLEL ? ", open_start=parallel"
                    : g->start_mode == YGFX__START_SERIAL ? ", open_start=serial(no GL_KHR_parallel_shader_compile)" : "");
}
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ >= 7
#pragma GCC diagnostic pop
#endif

YGFX_API yscr_screen* ygfx_screen(const ygfx_gfx* g) { return g && g->open ? g->screen : NULL; }
YGFX_API bool ygfx_calibrated(const ygfx_gfx* g) { return g && g->open && g->calibrated; }
YGFX_API const ycol_ctx* ygfx_color(const ygfx_gfx* g) { return g && g->open && g->has_color ? &g->color : NULL; }
YGFX_API uint32_t ygfx_features(const ygfx_gfx* g) { return g && g->open ? g->features : 0u; }
YGFX_API void ygfx_program_stats(const ygfx_gfx* g, ygfx_programs* out) {
    if (!out) return;
    if (g) *out = g->progs; else memset(out, 0, sizeof *out);
}

/* --- the file cache ------------------------------------------------------------ */

#if defined(_WIN32)
static FILE* ygfx__wopen(const char* path, const wchar_t* mode) {
    wchar_t w[600];
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, w, 600) <= 0) return NULL;
#if defined(_MSC_VER)
    {
        FILE* f = NULL;
        return _wfopen_s(&f, w, mode) == 0 ? f : NULL;
    }
#else
    return _wfopen(w, mode);
#endif
}
#endif

static void ygfx__fc_path(const ygfx_file_cache* fc, uint64_t key, const char* ext, char* out, size_t cap) {
    snprintf(out, cap, "%s/%08x%08x.%s", fc->dir, (unsigned)(key >> 32), (unsigned)(key & 0xFFFFFFFFu), ext);
}

static size_t ygfx__fc_load(void* user, uint64_t key, void* dst, size_t cap) {
    const ygfx_file_cache* fc = (const ygfx_file_cache*)user;
    char path[600];
    FILE* f;
    long n;
    ygfx__fc_path(fc, key, "yspprog", path, sizeof path);
#if defined(_WIN32)
    f = ygfx__wopen(path, L"rb");
#else
    f = fopen(path, "rb");
#endif
    if (!f) return 0;
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) <= 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return 0; }
    if (dst && cap >= (size_t)n && fread(dst, 1, (size_t)n, f) != (size_t)n) n = 0;
    fclose(f);
    return (size_t)n;
}

/* Makes dir and its parents; the ones that exist are left alone. */
static void ygfx__fc_mkdirs(const char* dir) {
    char p[512];
    size_t i, n = strlen(dir);
    if (n >= sizeof p) return;
    memcpy(p, dir, n + 1);
    for (i = 1; i <= n; i++) {
        if (p[i] == '/' || p[i] == '\\' || p[i] == '\0') {
            char c = p[i];
            p[i] = '\0';
#if defined(_WIN32)
            {
                wchar_t w[520];
                if (MultiByteToWideChar(CP_UTF8, 0, p, -1, w, 520) > 0) CreateDirectoryW(w, NULL);
            }
#else
            mkdir(p, 0777);
#endif
            p[i] = c;
        }
    }
}

/* Whole or not at all: written under a name no reader looks for, then
 * renamed over the entry, so a reader never sees half a file. */
static int ygfx__fc_store(void* user, uint64_t key, const void* data, size_t n) {
    const ygfx_file_cache* fc = (const ygfx_file_cache*)user;
    char tmp[620], path[600], ext[40];
    FILE* f;
    int ok;
    snprintf(ext, sizeof ext, "tmp%08x", (unsigned)(yrt_now_ns() & 0xFFFFFFFFu));
    ygfx__fc_path(fc, key, ext, tmp, sizeof tmp);
    ygfx__fc_path(fc, key, "yspprog", path, sizeof path);
#if defined(_WIN32)
    f = ygfx__wopen(tmp, L"wb");
    if (!f) { ygfx__fc_mkdirs(fc->dir); f = ygfx__wopen(tmp, L"wb"); }
#else
    f = fopen(tmp, "wb");
    if (!f) { ygfx__fc_mkdirs(fc->dir); f = fopen(tmp, "wb"); }
#endif
    if (!f) return YGFX_ERR_ARG;
    ok = fwrite(data, 1, n, f) == n;
    ok = fclose(f) == 0 && ok;
#if defined(_WIN32)
    if (ok) {
        wchar_t wa[600], wb[600];
        ok = MultiByteToWideChar(CP_UTF8, 0, tmp, -1, wa, 600) > 0 && MultiByteToWideChar(CP_UTF8, 0, path, -1, wb, 600) > 0 &&
             MoveFileExW(wa, wb, MOVEFILE_REPLACE_EXISTING) != 0;
    }
#else
    ok = ok && rename(tmp, path) == 0;
#endif
    if (!ok) remove(tmp);
    return ok ? YGFX_OK : YGFX_ERR_ARG;
}

YGFX_API const ygfx_cache* ygfx_file_cache_init(ygfx_file_cache* fc, const char* dir) {
    size_t n;
    if (!fc || !dir || !dir[0]) return NULL;
    n = strlen(dir);
    if (n >= sizeof fc->dir) return NULL;
    memset(fc, 0, sizeof *fc);
    memcpy(fc->dir, dir, n + 1);
    while (n > 1 && (fc->dir[n - 1] == '/' || fc->dir[n - 1] == '\\')) fc->dir[--n] = '\0';
    fc->cache.load = ygfx__fc_load;
    fc->cache.store = ygfx__fc_store;
    fc->cache.user = fc;
    return &fc->cache;
}

/* yrt_user_dir()'s rules (ysp/rt.h, PER-USER FOLDERS): only a folder that
 * this user alone can write, because anyone who can write it can plant an
 * entry with a correct hash, and the driver parses the binary. */
YGFX_API int ygfx_default_cache_dir(char* out, size_t cap) {
    return yrt_user_dir(YRT_DIR_CACHE, "progcache", out, cap) == YRT_OK ? YGFX_OK : YGFX_ERR_ARG;
}

YGFX_API void ygfx_reset_state(ygfx_gfx* g) {
    if (g && g->open) g->be->reset(g->bctx);
}

/* --- stimuli ---------------------------------------------------------------- */

static void ygfx__stim_defaults(ygfx_stim* s, int kind) {
    memset(s, 0, sizeof *s);
    s->kind = (uint16_t)kind;
    s->ax = 0.5f;
    s->ay = 0.5f;
    s->visible = 1.0f;
    s->contrast = 1.0f;
    s->opacity = 1.0f;
    s->gate = 1.0f;
    s->aspect = 1.0f;
    s->check = 1.0f;
    s->tint[0] = s->tint[1] = s->tint[2] = s->tint[3] = 1.0f;
}

static void ygfx__copy3(float* dst, const float* src) { dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; }

/* A desc's place and anchor into the stimulus. */
static void ygfx__align(ygfx_stim* s, ygfx_align place, ygfx_align anchor) {
    static const float fx[9] = { 0.5f, 0.0f, 0.5f, 1.0f, 0.0f, 1.0f, 0.0f, 0.5f, 1.0f };
    static const float fy[9] = { 0.5f, 0.0f, 0.0f, 0.0f, 0.5f, 0.5f, 1.0f, 1.0f, 1.0f };
    int a = (int)anchor >= 0 && (int)anchor < 9 ? (int)anchor : 0;
    s->place = (uint8_t)((int)place >= 0 && (int)place < 9 ? (int)place : 0);
    s->ax = fx[a];
    s->ay = fy[a];
}

YGFX_API ygfx_stim ygfx_shape(const ygfx_shape_desc* d) {
    ygfx_stim s;
    int i;
    ygfx__stim_defaults(&s, YGFX_SHAPE);
    if (!d) return s;
    ygfx__align(&s, d->place, d->anchor);
    s.shape = (uint16_t)d->shape; s.edge = (uint16_t)d->edge; s.edge_width = d->edge_width;
    s.x = d->x; s.y = d->y; s.w = d->w; s.h = d->h ? d->h : d->w; s.ori = d->ori;
    ygfx__copy3(s.color, d->color);
    if (d->opacity != 0.0f) s.opacity = d->opacity;
    for (i = 0; i < 4; i++) s.shape_p[i] = d->shape_p[i];
    if (d->shape == YGFX_POLYGON && d->vertices) {
        int n = (int)d->shape_p[0];
        float mx = 0.0f, my = 0.0f;
        if (n > 16) n = 16;
        for (i = 0; i < 2 * n; i++) {
            s.p[i] = d->vertices[i];
            if (i % 2 == 0 && fabsf(s.p[i]) > mx) mx = fabsf(s.p[i]);
            if (i % 2 == 1 && fabsf(s.p[i]) > my) my = fabsf(s.p[i]);
        }
        /* no box: the vertices' own (they are from its center), so the zero
         * default draws the polygon given */
        if (d->w == 0.0f && d->h == 0.0f) { s.w = 2.0f * mx; s.h = 2.0f * my; }
    }
    s.stroke = d->stroke; s.stroke_align = (uint8_t)d->stroke_align; s.join = (uint8_t)d->join;
    s.miter_limit = d->miter_limit; s.mask = d->mask; s.group = d->group;
    s.offset = d->offset; s.dash[0] = d->dash[0]; s.dash[1] = d->dash[1]; s.dash_offset = d->dash_offset;
    s.trim[0] = d->trim[0]; s.trim[1] = d->trim[1];
    s.cap = (uint8_t)d->cap; s.blend = (uint8_t)d->blend; s.dash_snap = (uint8_t)(d->dash_snap ? 1 : 0);
    s.path = d->path; s.n_path = d->n_path > 0 ? (uint32_t)d->n_path : 0u;
    s.fx = d->fx; s.paint = d->paint;
    if (d->shape == YGFX_STAR && d->shape_p[1] == 0.0f) s.shape_p[1] = 0.5f;
    if (d->shape == YGFX_CAPSULE && d->h == 0.0f) s.h = 2.0f * (d->shape_p[0] > d->shape_p[1] ? d->shape_p[0] : d->shape_p[1]);
    return s;
}

YGFX_API ygfx_stim ygfx_compound(const ygfx_compound_desc* d) {
    ygfx_stim s;
    ygfx__stim_defaults(&s, YGFX_SHAPE);
    s.shape = YGFX_COMPOUND;
    if (!d) return s;
    ygfx__align(&s, d->place, d->anchor);
    s.prims = d->prims; s.n_prims = d->n > 0 ? (uint32_t)d->n : 0u;
    s.x = d->x; s.y = d->y; s.w = d->w; s.h = d->h ? d->h : d->w; s.ori = d->ori;
    s.shape_p[3] = d->onion;   /* a compound's onion on the result */
    s.edge = (uint16_t)d->edge; s.edge_width = d->edge_width;
    ygfx__copy3(s.color, d->color);
    if (d->opacity != 0.0f) s.opacity = d->opacity;
    s.stroke = d->stroke; s.stroke_align = (uint8_t)d->stroke_align; s.offset = d->offset;
    s.blend = (uint8_t)d->blend; s.fx = d->fx; s.paint = d->paint; s.group = d->group;
    return s;
}

YGFX_API ygfx_stim ygfx_glyphs(const ygfx_glyphs_desc* d) {
    ygfx_stim s;
    ygfx__stim_defaults(&s, YGFX_SHAPE);
    s.shape = YGFX_MASK_TEX;
    if (!d) return s;
    ygfx__align(&s, d->place, d->anchor);
    s.mask = d->atlas; s.buf = d->buf; s.count = d->count; s.first = d->first;
    s.shape_p[0] = d->scale;   /* units per atlas texel */
    s.x = d->x; s.y = d->y; s.w = d->w; s.h = d->h; s.ori = d->ori;
    s.edge = (uint16_t)d->edge; s.edge_width = d->edge_width;
    ygfx__copy3(s.color, d->color);
    if (d->opacity != 0.0f) s.opacity = d->opacity;
    s.stroke = d->stroke; s.stroke_align = (uint8_t)d->stroke_align; s.join = (uint8_t)d->join; s.offset = d->offset;
    s.blend = (uint8_t)d->blend; s.group = d->group;
    return s;
}

YGFX_API ygfx_stim ygfx_grating(const ygfx_grating_desc* d) {
    ygfx_stim s;
    ygfx__stim_defaults(&s, YGFX_GRATING);
    if (!d) return s;
    ygfx__align(&s, d->place, d->anchor);
    s.x = d->x; s.y = d->y; s.w = d->w; s.h = d->h ? d->h : d->w; s.ori = d->ori;
    s.sf = d->sf; s.phase = d->phase;
    if (d->contrast != 0.0f) s.contrast = d->contrast;
    s.wave = (uint16_t)(d->square ? 1 : 0);
    s.shape = (uint16_t)d->aperture; s.edge = (uint16_t)d->edge; s.edge_width = d->edge_width;
    ygfx__copy3(s.dir, d->dir);
    s.stroke = d->stroke; s.stroke_align = (uint8_t)d->stroke_align; s.mask = d->mask; s.group = d->group;
    memcpy(s.shape_p, d->shape_p, sizeof s.shape_p);
    return s;
}

YGFX_API ygfx_stim ygfx_gabor(const ygfx_gabor_desc* d) {
    ygfx_stim s;
    ygfx__stim_defaults(&s, YGFX_GABOR);
    s.shape = YGFX_NO_APERTURE;
    if (!d) return s;
    ygfx__align(&s, d->place, d->anchor);
    s.x = d->x; s.y = d->y; s.ori = d->ori; s.sf = d->sf; s.phase = d->phase; s.sigma = d->sigma;
    if (d->contrast != 0.0f) s.contrast = d->contrast;
    if (d->aspect != 0.0f) s.aspect = d->aspect;
    s.w = d->size;
    s.h = d->size;
    ygfx__copy3(s.dir, d->dir);
    s.group = d->group;
    return s;
}

YGFX_API ygfx_stim ygfx_dots(const ygfx_dots_desc* d) {
    ygfx_stim s;
    ygfx__stim_defaults(&s, YGFX_DOTS);
    s.shape = YGFX_NO_APERTURE;
    if (!d) return s;
    ygfx__align(&s, d->place, d->anchor);
    s.buf = d->buf; s.count = d->count; s.x = d->x; s.y = d->y; s.ori = d->ori;
    s.first = d->first;
    s.dot_size = d->dot_size; s.edge = (uint16_t)d->edge; s.edge_width = d->edge_width;
    ygfx__copy3(s.color, d->color);
    if (d->opacity != 0.0f) s.opacity = d->opacity;
    /* RECT is aperture 0: with no box it is the zero default, no aperture
     * (v0.3 and before drew nothing then) */
    s.shape = (uint16_t)((d->aperture == YGFX_RECT && d->w > 0.0f) || d->aperture == YGFX_CIRCLE ? d->aperture : YGFX_NO_APERTURE);
    s.w = d->w; s.h = d->h ? d->h : d->w;
    s.stroke = d->stroke; s.stroke_align = (uint8_t)d->stroke_align; s.group = d->group;
    return s;
}

YGFX_API ygfx_stim ygfx_image(const ygfx_gfx* g, const ygfx_image_desc* d) {
    ygfx_stim s;
    int i;
    ygfx__stim_defaults(&s, YGFX_IMAGE);
    s.shape = YGFX_NO_APERTURE;
    if (!d) return s;
    ygfx__align(&s, d->place, d->anchor);
    s.tex = d->tex; s.x = d->x; s.y = d->y; s.ori = d->ori;
    s.w = d->w; s.h = d->h;
    for (i = 0; i < 4; i++) s.src[i] = d->src[i];
    if (d->tint[0] != 0.0f || d->tint[1] != 0.0f || d->tint[2] != 0.0f || d->tint[3] != 0.0f)
        for (i = 0; i < 4; i++) s.tint[i] = d->tint[i];
    if (g && d->tex.id >= 1 && d->tex.id <= YGFX_MAX_TEXTURES) {
        const ygfx__res* r = &g->tex[d->tex.id - 1];
        float sw = s.src[2] > 0 && s.src[3] > 0 ? s.src[2] : (float)r->w;
        float sh = s.src[2] > 0 && s.src[3] > 0 ? s.src[3] : (float)r->h;
        if (s.w == 0) s.w = sw / g->ppu;
        if (s.h == 0) s.h = sh / g->ppu;
        if (r->flags & YGFX__RES_TARGET) s.flags |= YGFX_STIM_PREMULTIPLIED;
    }
    if (d->modulation) s.flags |= YGFX_STIM_MODULATION;
    if (d->add) s.flags |= YGFX_STIM_ADD;
    if (d->premultiplied) s.flags |= YGFX_STIM_PREMULTIPLIED;
    if (d->linear) s.flags |= YGFX_STIM_LINEAR;
    if (d->coverage) s.flags |= YGFX_STIM_COVERAGE;
    if (d->contrast != 0.0f) s.contrast = d->contrast;
    if (d->opacity != 0.0f) s.opacity = d->opacity;
    ygfx__copy3(s.dir, d->dir);
    s.group = d->group;
    return s;
}

YGFX_API ygfx_stim ygfx_noise(const ygfx_noise_desc* d) {
    ygfx_stim s;
    ygfx__stim_defaults(&s, YGFX_NOISE);
    if (!d) return s;
    ygfx__align(&s, d->place, d->anchor);
    s.x = d->x; s.y = d->y; s.w = d->w; s.h = d->h ? d->h : d->w; s.ori = d->ori;
    s.check = d->check > 0 ? d->check : 1.0f;
    s.seed = (float)(d->seed & 0xFFFFFFu);
    s.wave = (uint16_t)d->dist;
    s.gn_scale = d->scale; s.gn_z = d->z;
    s.gn_octaves = d->octaves > 0 ? d->octaves : 1;
    s.gn_lacunarity = d->lacunarity != 0.0f ? d->lacunarity : 2.0f;
    s.gn_gain = d->gain != 0.0f ? d->gain : 0.5f;
    if (d->contrast != 0.0f) s.contrast = d->contrast;
    s.shape = (uint16_t)d->aperture; s.edge = (uint16_t)d->edge; s.edge_width = d->edge_width;
    ygfx__copy3(s.dir, d->dir);
    s.stroke = d->stroke; s.stroke_align = (uint8_t)d->stroke_align; s.mask = d->mask; s.group = d->group;
    memcpy(s.shape_p, d->shape_p, sizeof s.shape_p);
    return s;
}

YGFX_API ygfx_stim ygfx_user(const ygfx_user_desc* d) {
    ygfx_stim s;
    int i;
    ygfx__stim_defaults(&s, YGFX_USER);
    if (!d) return s;
    ygfx__align(&s, d->place, d->anchor);
    s.pipe = d->pipe; s.x = d->x; s.y = d->y; s.w = d->w; s.h = d->h ? d->h : d->w; s.ori = d->ori;
    if (d->contrast != 0.0f) s.contrast = d->contrast;
    if (d->opacity != 0.0f) s.opacity = d->opacity;
    s.shape = (uint16_t)d->aperture; s.edge = (uint16_t)d->edge; s.edge_width = d->edge_width;
    ygfx__copy3(s.color, d->color);
    ygfx__copy3(s.dir, d->dir);
    if (d->p) for (i = 0; i < d->n_p && i < 32; i++) s.p[i] = d->p[i];
    s.stroke = d->stroke; s.stroke_align = (uint8_t)d->stroke_align; s.mask = d->mask; s.group = d->group;
    memcpy(s.shape_p, d->shape_p, sizeof s.shape_p);
    s.tex = d->tex;
    return s;
}

YGFX_API ygfx_group ygfx_group_make(const ygfx_group_desc* d) {
    static const float fx[9] = { 0.5f, 0.0f, 0.5f, 1.0f, 0.0f, 1.0f, 0.0f, 0.5f, 1.0f };
    static const float fy[9] = { 0.5f, 0.0f, 0.0f, 0.0f, 0.5f, 0.5f, 1.0f, 1.0f, 1.0f };
    ygfx_group gr;
    memset(&gr, 0, sizeof gr);
    gr.visible = 1.0f; gr.ax = 0.5f; gr.ay = 0.5f; gr.scale = 1.0f; gr.opacity = 1.0f;
    if (!d) return gr;
    gr.place = (uint8_t)((int)d->place >= 0 && (int)d->place < 9 ? (int)d->place : 0);
    if ((int)d->anchor > 0 && (int)d->anchor < 9) { gr.ax = fx[d->anchor]; gr.ay = fy[d->anchor]; }
    gr.x = d->x; gr.y = d->y; gr.w = d->w; gr.h = d->h; gr.ori = d->ori;
    if (d->scale != 0.0f) gr.scale = d->scale;
    if (d->opacity != 0.0f) gr.opacity = d->opacity;
    return gr;
}

/* --- frame ------------------------------------------------------------------ */

/* A pass's std140 frame block, slot 0 for the scene: 5 x 16 bytes. A target
 * is drawn with its rows top first (GL's row 0 its top row), so its local y
 * axis keeps its sign in GL's axes; the scene's flips (YGFX__Y_SIGN). */
static void ygfx__frame_block(ygfx_gfx* g, int slot, int w, int h, int topdown) {
    unsigned char* base = g->staging + (size_t)slot * YGFX__BLOCK;
    const yscr_frame* f = &g->frame;
    float* fb;
    uint32_t* fu;
    int32_t* fi;
    memset(base, 0, YGFX__BLOCK);
    fb = (float*)base;
    fb[0] = (float)w; fb[1] = (float)h; fb[2] = 1.0f / (float)w; fb[3] = 1.0f / (float)h;
    fb[4] = (float)((double)(f->onset - g->t_open) * 1e-9);
    fb[5] = (float)((double)f->period * 1e-9);
    fb[6] = (float)(f->index & 0xFFFFFF);
    fu = (uint32_t*)(base + 32);
    fu[0] = (uint32_t)((uint64_t)f->index & 0xFFFFFFFFu);
    fu[1] = (uint32_t)((uint64_t)f->index >> 32);
    fu[2] = g->seed;
    fu[3] = (uint32_t)((uint64_t)f->vblank & 0xFFFFFFFFu);
    fi = (int32_t*)(base + 48);
    fi[0] = g->lut_n;
    fi[1] = 255;
    fi[2] = (int32_t)g->dither;
    fi[3] = g->origin_top ? g->h : 0;
    fb = (float*)(base + 64);
    fb[0] = topdown ? 1.0f : YGFX__Y_SIGN;
    /* the vector program's loop bounds: data, so D3D's compiler cannot
     * unroll them (unrolled, they doubled its compile time) */
    fb[1] = 16.0f;   /* Gauss-Legendre nodes */
    fb[2] = 3.0f;    /* dashes about a pixel  */
}

/* GL state another program, or a ysp/screen.h hook, may have changed: the
 * cached state goes when the screen's GL epoch moves. A new context means
 * every name the handle holds is gone or, worse, names another object:
 * the handle closes without deleting anything, since deleting those names
 * would delete the new context's objects. Called on entry to every
 * function that issues GL; returns YGFX_ERR_LOST then. */
#if defined(YSCR_HAS_GL_EPOCH)
    #define YGFX__EPOCH 1
#else
    #define YGFX__EPOCH 0
#endif
static int ygfx__sync(ygfx_gfx* g) {
#if YGFX__EPOCH
    if (g->screen) {
        uint32_t e = yscr_gl_epoch(g->screen);
        if (yscr_gl_generation(g->screen) != g->generation) {
            g->open = 0;
            g->in_frame = 0;
            ygfx__set_error(g->error, sizeof g->error,
                              "ysp_gfx: the GL context is new; every GL object is gone (close and open again)");
            return YGFX_ERR_LOST;
        }
        if (e != g->epoch) { g->be->reset(g->bctx); g->epoch = e; }
    }
#else
    (void)g;
#endif
    return YGFX_OK;
}

YGFX_API int ygfx_begin(ygfx_gfx* g, const yscr_frame* f) {
    float* fb;
    uint32_t* fu;
    int32_t* fi;
    YRT_ZONE(z, "ygfx.begin");
    if (!g || !g->open) { YRT_ZONE_END(z); return YGFX_ERR_CLOSED; }
    if (g->in_frame) { YRT_ZONE_END(z); return YGFX_ERR_ORDER; }
    if (!f) { YRT_ZONE_END(z); return YGFX_ERR_ARG; }
    g->frame = *f;
    g->in_frame = 1;
    g->n_cmds = 0;
    g->n_blocks = 0;
    g->clipped = 0;
    g->n_targets = 0;
    g->inst_used = 0;
    g->in_target = 0;
    g->cur_target_tex = 0;
    g->cur_w = g->w; g->cur_h = g->h; g->cur_top = 0;
    g->n_segs = 1;
    memset(&g->segs[0], 0, sizeof g->segs[0]);
    g->segs[0].w = g->w; g->segs[0].h = g->h;
    ygfx__frame_block(g, 0, g->w, g->h, 0);
    if (g->cap) ygfx__cap_records(g->cap, f);
    (void)fb; (void)fu; (void)fi;
    YRT_ZONE_END(z);
    return YGFX_OK;
}

/* --- coordinates ------------------------------------------------------------- */

/* A group's scale applies to what is in units, never to what is in px. */
static float ygfx__scale(const ygfx_stim* s) {
    return s->group && s->group->scale > 0.0f ? s->group->scale : 1.0f;
}

/* The quad's half size in px, the shape the shader evaluates, and its
 * parameters in px. DOTS: the field's box and aperture. */
static int ygfx__geom(const ygfx_gfx* g, const ygfx_stim* s, float* hx, float* hy, float sp[3]) {
    const float ppu = g->ppu * ygfx__scale(s);
    int shape = s->shape;
    *hx = 0.5f * s->w * ppu;
    *hy = 0.5f * s->h * ppu;
    if (s->kind == YGFX_SHAPE && shape == YGFX_NO_APERTURE) shape = YGFX_RECT;
    if (s->kind == YGFX_GABOR) {
        float sg = s->sigma > 0 ? s->sigma : 0.0f;
        if (s->w <= 0) *hx = 4.0f * sg * ppu;
        if (s->h <= 0) *hy = 4.0f * sg * ppu / (s->aspect > 0 ? s->aspect : 1.0f);
    }
    if (shape == YGFX_LINE) *hy = 0.5f * s->shape_p[0] * ppu;
    sp[0] = s->shape_p[0] * ppu;
    sp[1] = s->shape_p[1] * ppu;
    sp[2] = shape == YGFX_POLYGON ? s->shape_p[0] : 0.0f;
    return shape;
}

/* The coordinate convention lives here and nowhere else.
 * Pixels: origin at the top-left corner of the screen (or of the target
 * being drawn), x right, y down. A stimulus's anchor point is its `place`
 * point plus (x, y) units along the axes. Its anchor, (ax, ay) as fractions
 * of its own box with (0, 0) the top-left corner, sits there, and the box
 * turns about it by ori degrees, +x toward +y (clockwise on the screen).
 * Everything a stimulus draws (SDF, aperture, carrier) is defined about the
 * box's own center, so the anchor moves and turns a pattern but never
 * changes it.
 * In a group, the member's place point is one of its group's box and its x,
 * y, ori are in the group's frame; the group's frame is its anchor at its
 * own place point plus (x, y), turned by its ori and scaled by its scale.
 * ygfx__place() hands the shaders the box center in the pass's GL pixels
 * and the local x axis in GL's axes; the local y axis is that axis turned a
 * quarter turn counterclockwise in GL's axes, times the frame block's sign. */

static void ygfx__place_point(int place, double w, double h, double* px, double* py) {
    switch (place) {
    case YGFX_TOP_LEFT:     *px = 0.0;     *py = 0.0;     break;
    case YGFX_TOP:          *px = 0.5 * w; *py = 0.0;     break;
    case YGFX_TOP_RIGHT:    *px = w;       *py = 0.0;     break;
    case YGFX_LEFT:         *px = 0.0;     *py = 0.5 * h; break;
    case YGFX_RIGHT:        *px = w;       *py = 0.5 * h; break;
    case YGFX_BOTTOM_LEFT:  *px = 0.0;     *py = h;       break;
    case YGFX_BOTTOM:       *px = 0.5 * w; *py = h;       break;
    case YGFX_BOTTOM_RIGHT: *px = w;       *py = h;       break;
    default:                  *px = 0.5 * w; *py = 0.5 * h; break;
    }
}

/* The anchor's position and the box center in the pixels of a W x H pass,
 * and the box's turn. */
static void ygfx__frame_px(const ygfx_gfx* g, const ygfx_stim* s, double W, double H,
                             double anchor[2], double center[2], double* c, double* sn) {
    const double deg = 3.14159265358979323846 / 180.0;
    const ygfx_group* gr = s->group;
    double ppu = g->ppu, rad = (double)s->ori * deg, px, py, ox, oy;
    float hx, hy, sp[3];
    if (!gr) {
        ygfx__place_point(s->place, W, H, &px, &py);
        anchor[0] = px + (double)s->x * ppu;
        anchor[1] = py + (double)s->y * ppu;
    } else {
        double gw = (double)gr->w * ppu, gh = (double)gr->h * ppu, k = gr->scale > 0 ? gr->scale : 1.0;
        double gc = cos((double)gr->ori * deg), gs = sin((double)gr->ori * deg), qx, qy, gx, gy;
        /* the member's anchor in the group's frame, from the box's pivot */
        ygfx__place_point(s->place, gw, gh, &px, &py);
        qx = (px + (double)s->x * ppu - (double)gr->ax * gw) * k;
        qy = (py + (double)s->y * ppu - (double)gr->ay * gh) * k;
        ygfx__place_point(gr->place, W, H, &gx, &gy);
        anchor[0] = gx + (double)gr->x * ppu + gc * qx - gs * qy;
        anchor[1] = gy + (double)gr->y * ppu + gs * qx + gc * qy;
        rad += (double)gr->ori * deg;
    }
    ygfx__geom(g, s, &hx, &hy, sp);
    *c = cos(rad);
    *sn = sin(rad);
    /* the center relative to the anchor, in the box's own axes, then
     * turned: local x is (cos, sin) and local y is (-sin, cos) on screen */
    ox = (0.5 - (double)s->ax) * 2.0 * hx;
    oy = (0.5 - (double)s->ay) * 2.0 * hy;
    center[0] = anchor[0] + *c * ox - *sn * oy;
    center[1] = anchor[1] + *sn * ox + *c * oy;
}

/* For the pass being recorded: the scene is GL's bottom-up rows, a target's
 * rows are top first. */
static void ygfx__place(const ygfx_gfx* g, const ygfx_stim* s, float out[4]) {
    double a[2], m[2], c, sn;
    ygfx__frame_px(g, s, (double)g->cur_w, (double)g->cur_h, a, m, &c, &sn);
    out[0] = (float)m[0];
    out[1] = g->cur_top ? (float)m[1] : (float)((double)g->cur_h - m[1]);
    out[2] = (float)c;
    out[3] = g->cur_top ? (float)sn : (float)-sn;
}

YGFX_API void ygfx_center(const ygfx_gfx* g, float* x, float* y) {
    if (x) *x = g ? 0.5f * (float)g->w : 0.0f;
    if (y) *y = g ? 0.5f * (float)g->h : 0.0f;
}

YGFX_API void ygfx_size(const ygfx_gfx* g, float* w, float* h) {
    if (w) *w = g ? (float)g->w : 0.0f;
    if (h) *h = g ? (float)g->h : 0.0f;
}

YGFX_API void ygfx_resolve(const ygfx_gfx* g, const ygfx_stim* s, float* x, float* y) {
    double a[2] = { 0, 0 }, m[2], c, sn;
    if (g && s) ygfx__frame_px(g, s, g->w, g->h, a, m, &c, &sn);
    if (x) *x = (float)a[0];
    if (y) *y = (float)a[1];
}

/* How far a level `a` px out of the boundary reaches from the box, px: a
 * mitered corner pushes it out by up to the miter limit (a polygon) or
 * sqrt 2 (a rect or cross). */
static float ygfx__reach(const ygfx_stim* s, int shape, float a) {
    float lim = s->miter_limit > 0.0f ? s->miter_limit : 4.0f;
    if (lim < 1.0f) lim = 1.0f;
    if (s->join == YGFX_JOIN_MITER && shape == YGFX_POLYGON) return a * lim;
    if (s->join == YGFX_JOIN_MITER && (shape == YGFX_RECT || shape == YGFX_CROSS)) return a * 1.4142136f;
    return a;
}

/* The outer offset of a stroke, px; 0 for a fill. */
static float ygfx__outer(const ygfx_stim* s, int shape) {
    float a2;
    if (s->stroke <= 0.0f) return 0.0f;
    a2 = s->stroke_align == YGFX_STROKE_INSIDE ? 0.0f
       : (s->stroke_align == YGFX_STROKE_OUTSIDE ? s->stroke : 0.5f * s->stroke);
    return ygfx__reach(s, shape, a2);
}

YGFX_API void ygfx_bounds(const ygfx_gfx* g, const ygfx_stim* s, float* x0, float* y0, float* x1, float* y1) {
    double a[2] = { 0, 0 }, m[2] = { 0, 0 }, c = 1, sn = 0, ex = 0, ey = 0;
    float hx = 0, hy = 0, sp[3];
    if (g && s) {
        int shape;
        ygfx__frame_px(g, s, g->w, g->h, a, m, &c, &sn);
        shape = ygfx__geom(g, s, &hx, &hy, sp);
        if (ygfx__is_vector(s)) {
            float blk[16 * 64];
            double e2[2];
            const char* err;
            int clip;
            if (ygfx__vpack(g, s, blk, e2, &clip, &err) >= 0) { hx = (float)e2[0]; hy = (float)e2[1]; }
        } else if (s->kind == YGFX_DOTS) {
            float r = 0.5f * s->dot_size + ygfx__outer(s, YGFX_CIRCLE);
            hx += r; hy += r;
        } else if (shape != YGFX_NO_APERTURE && shape != YGFX_MASK_TEX) {
            float o = ygfx__outer(s, shape) + (s->kind == YGFX_SHAPE && s->offset > 0.0f ? s->offset : 0.0f);
            hx += o; hy += o;
        }
        ex = fabs(c) * hx + fabs(sn) * hy;
        ey = fabs(sn) * hx + fabs(c) * hy;
    }
    if (x0) *x0 = (float)(m[0] - ex);
    if (y0) *y0 = (float)(m[1] - ey);
    if (x1) *x1 = (float)(m[0] + ex);
    if (y1) *y1 = (float)(m[1] + ey);
}

YGFX_API void ygfx_local(const ygfx_gfx* g, const ygfx_stim* s, float px, float py, float* lx, float* ly) {
    double a[2] = { 0, 0 }, m[2] = { 0, 0 }, c = 1, sn = 0, dx, dy;
    if (g && s) ygfx__frame_px(g, s, g->w, g->h, a, m, &c, &sn);
    dx = (double)px - m[0];
    dy = (double)py - m[1];
    if (lx) *lx = (float)(dx * c + dy * sn);
    if (ly) *ly = (float)(-dx * sn + dy * c);
}

/* The shader's ysp_sdf_at() on the CPU, in double. */
static double ygfx__sd_box(double px, double py, double bx, double by) {
    double dx = fabs(px) - bx, dy = fabs(py) - by;
    double ox = dx > 0 ? dx : 0, oy = dy > 0 ? dy : 0;
    double in = dx > dy ? dx : dy;
    return sqrt(ox * ox + oy * oy) + (in < 0 ? in : 0);
}

static double ygfx__mbox(double px, double py, double bx, double by, double L, double a) {
    double dx = fabs(px) - bx, dy = fabs(py) - by, m = dx > dy ? dx : dy, b;
    if (!(L < 1.41421356)) return m;
    b = (dx + dy) * 0.70710678118654752 + a * (1.0 - 0.70710678118654752);
    return b > m ? b : m;
}

/* A polygon's orientation in the local frame: +1 counterclockwise in the
 * math sense (y up), -1 the other way; 0 when not strictly convex. */
static int ygfx__convex(const float* v, int n, double scale) {
    int k, sgn = 0;
    for (k = 0; k < n; k++) {
        const float* a = v + 2 * k;
        const float* b = v + 2 * ((k + 1) % n);
        const float* c = v + 2 * ((k + 2) % n);
        double cr = ((double)b[0] - a[0]) * ((double)c[1] - b[1]) - ((double)b[1] - a[1]) * ((double)c[0] - b[0]);
        int sk = cr > 0 ? 1 : (cr < 0 ? -1 : 0);
        if (sk == 0) return 0;
        if (sgn == 0) sgn = sk;
        else if (sk != sgn) return 0;
    }
    (void)scale;
    return sgn;
}

static double ygfx__sd_polygon_miter(double px, double py, const float* verts, int n, double ppu,
                                       double o, double L, double a) {
    double d = -1e30;
    int k;
    for (k = 0; k < n; k++) {
        int kp = (k + n - 1) % n, kn = (k + 1) % n;
        double vx = verts[2 * k] * ppu, vy = verts[2 * k + 1] * ppu;
        double e1x = vx - verts[2 * kp] * ppu, e1y = vy - verts[2 * kp + 1] * ppu;
        double e2x = verts[2 * kn] * ppu - vx, e2y = verts[2 * kn + 1] * ppu - vy;
        double l1 = sqrt(e1x * e1x + e1y * e1y), l2 = sqrt(e2x * e2x + e2y * e2y);
        double n1x = o * e1y / l1, n1y = -o * e1x / l1, n2x = o * e2y / l2, n2y = -o * e2x / l2;
        double bx = n1x + n2x, by = n1y + n2y, bl = sqrt(bx * bx + by * by), c, t;
        t = (px - vx) * n2x + (py - vy) * n2y;
        if (t > d) d = t;
        bx /= bl; by /= bl;
        c = bx * n1x + by * n1y;
        if (c * L < 1.0) {
            t = (px - vx) * bx + (py - vy) * by + a * (1.0 - c);
            if (t > d) d = t;
        }
    }
    return d;
}

static double ygfx__sdf(int shape, double px, double py, double hx, double hy, const float sp[3],
                          const float* verts, double ppu, double ml, double a) {
    switch (shape) {
    case YGFX_RECT: {
        double r = sp[1];
        if (ml != 0.0) return ygfx__mbox(px, py, hx, hy, ml, a);
        if (r > hx) r = hx;
        if (r > hy) r = hy;
        return ygfx__sd_box(px, py, hx - r, hy - r) - r;
    }
    case YGFX_CIRCLE:  return sqrt(px * px + py * py) - hx;
    case YGFX_ANNULUS: return fabs(sqrt(px * px + py * py) - 0.5 * (hx + sp[0])) - 0.5 * (hx - sp[0]);
    case YGFX_LINE: {
        double ex = fabs(px) - (hx - hy);
        if (ex < 0) ex = 0;
        return sqrt(ex * ex + py * py) - hy;
    }
    case YGFX_POLYGON: {
        int n = (int)sp[2], k, j;
        double d, sg = 1.0;
        if (n < 3 || n > 16) return 1e30;
        if (ml != 0.0) return ygfx__sd_polygon_miter(px, py, verts, n, ppu, ml > 0 ? 1.0 : -1.0, fabs(ml), a);
        d = (px - verts[0] * ppu) * (px - verts[0] * ppu) + (py - verts[1] * ppu) * (py - verts[1] * ppu);
        for (k = 0, j = n - 1; k < n; j = k, k++) {
            double vix = verts[2 * k] * ppu, viy = verts[2 * k + 1] * ppu;
            double vjx = verts[2 * j] * ppu, vjy = verts[2 * j + 1] * ppu;
            double ex = vjx - vix, ey = vjy - viy, wx = px - vix, wy = py - viy;
            double t = (wx * ex + wy * ey) / (ex * ex + ey * ey);
            double bx, by;
            int c1 = py >= viy, c2 = py < vjy, c3 = ex * wy > ey * wx;
            if (t < 0) t = 0;
            if (t > 1) t = 1;
            bx = wx - ex * t;
            by = wy - ey * t;
            if (bx * bx + by * by < d) d = bx * bx + by * by;
            if ((c1 && c2 && c3) || (!c1 && !c2 && !c3)) sg = -sg;
        }
        return sg * sqrt(d);
    }
    case YGFX_CROSS: {
        double a1, b1;
        if (ml != 0.0) {
            a1 = ygfx__mbox(px, py, hx, 0.5 * sp[0], ml, a);
            b1 = ygfx__mbox(px, py, 0.5 * sp[0], hy, ml, a);
        } else {
            a1 = ygfx__sd_box(px, py, hx, 0.5 * sp[0]);
            b1 = ygfx__sd_box(px, py, 0.5 * sp[0], hy);
        }
        return a1 < b1 ? a1 : b1;
    }
    default:
        return -1e30;   /* no aperture: the quad alone clips */
    }
}

/* The band's offsets a1 < a2, px, of a stroke; a fill has none. */
static void ygfx__band(const ygfx_stim* s, float* a1, float* a2) {
    float w = s->stroke;
    if (s->stroke_align == YGFX_STROKE_INSIDE) { *a1 = -w; *a2 = 0.0f; }
    else if (s->stroke_align == YGFX_STROKE_OUTSIDE) { *a1 = 0.0f; *a2 = w; }
    else { *a1 = -0.5f * w; *a2 = 0.5f * w; }
}

/* The miter limit the shader gets in *ml: 0 round; else the limit, its sign
 * the polygon's orientation (the sign of its cross products in the local
 * frame, as the shader sees them). Returns -1 when MITER is asked where it
 * cannot be: a curve, a rounded rect, a concave polygon. */
static int ygfx__miter(const ygfx_stim* s, int shape, float* ml) {
    float lim = s->miter_limit > 0.0f ? s->miter_limit : 4.0f;
    *ml = 0.0f;
    if (s->join != YGFX_JOIN_MITER) return 0;
    if (lim < 1.0f) lim = 1.0f;
    if ((shape == YGFX_RECT && s->shape_p[1] <= 0.0f) || shape == YGFX_CROSS) { *ml = lim; return 0; }
    if (shape == YGFX_POLYGON) {
        int n = (int)s->shape_p[0], o;
        if (n < 3 || n > 16) return -1;
        o = ygfx__convex(s->p, n, 1.0);
        if (o == 0) return -1;
        *ml = (float)o * lim;
        return 0;
    }
    return -1;
}


static int ygfx__trimmed(const ygfx_stim* s) {
    return !(s->trim[0] == 0.0f && s->trim[1] == 0.0f) && !(s->trim[0] <= 0.0f && s->trim[1] >= 1.0f);
}

/* A glyph run: a SHAPE on a mask with glyph instances. */
static int ygfx__is_glyphs(const ygfx_stim* s) {
    return s->kind == YGFX_SHAPE && s->shape == YGFX_MASK_TEX && s->buf.id != 0 && s->count > 0;
}

static int ygfx__crun_hit(const ygfx_gfx* g, const ygfx_stim* s, float px, float py);

YGFX_API bool ygfx_hit(const ygfx_gfx* g, const ygfx_stim* s, float px, float py) {
    float lx, ly, hx, hy, sp[3], ml;
    int shape;
    if (!g || !g->open || !s || !(s->visible >= 0.5f)) return false;
    if (s->group && !(s->group->visible >= 0.5f)) return false;
    if (s->kind == YGFX_TEXT) return ygfx__crun_hit(g, s, px, py) >= 0;
    if (s->kind != YGFX_SHAPE && s->kind != YGFX_GRATING && s->kind != YGFX_GABOR &&
        s->kind != YGFX_IMAGE && s->kind != YGFX_NOISE && s->kind != YGFX_USER) return false;
    ygfx_local(g, s, px, py, &lx, &ly);
    if (ygfx__is_vector(s)) {   /* the vector program's field, on the CPU */
        float blk[16 * 64];
        double ext[2];
        const char* err;
        int clip;
        if (ygfx__vpack(g, s, blk, ext, &clip, &err) < 0) return false;
        return ygfx__vhard(blk, lx, ly) != 0;
    }
    shape = ygfx__geom(g, s, &hx, &hy, sp);
    if (shape == YGFX_MASK_TEX) return false;   /* its distances live on the GPU */
    if (ygfx__miter(s, shape, &ml) < 0) return false;
    /* Coverage >= 0.5 is d <= 0 for every edge profile, which are centered
     * on the boundary; with no aperture, the quad is the boundary. A stroke
     * is its hard band. */
    if (shape == YGFX_NO_APERTURE) return fabsf(lx) <= hx && fabsf(ly) <= hy;
    if (s->stroke > 0.0f) {
        float a1, a2;
        double d1, d2;
        const double k = g->ppu * ygfx__scale(s);
        ygfx__band(s, &a1, &a2);
        d2 = ygfx__sdf(shape, lx, ly, hx, hy, sp, s->p, k, ml, a2 + s->offset) - s->offset;
        d1 = ygfx__sdf(shape, lx, ly, hx, hy, sp, s->p, k, ml, a1 + s->offset) - s->offset;
        return d2 <= a2 && d1 > a1;
    }
    return ygfx__sdf(shape, lx, ly, hx, hy, sp, s->p, g->ppu * ygfx__scale(s), ml, s->offset) - s->offset <= 0.0;
}

/* Quad margin in px outside the nominal boundary that a stroke and an edge
 * profile reach: 1 px for the pixel centers on it, and their extent. A
 * mask is clipped to its box: its padding holds what reaches out. */
static float ygfx__margin(const ygfx_stim* s, int shape, float ew) {
    float m;
    if ((shape == YGFX_NO_APERTURE && s->kind != YGFX_DOTS) || shape == YGFX_MASK_TEX) return 1.0f;
    m = s->stroke_align == YGFX_STROKE_INSIDE || s->stroke <= 0.0f ? 0.0f
      : (s->stroke_align == YGFX_STROKE_OUTSIDE ? s->stroke : 0.5f * s->stroke);
    if (s->kind == YGFX_SHAPE && s->offset > 0.0f) m += s->offset;
    if (s->edge == YGFX_EDGE_COSINE && ew > 0) m += 0.5f * ew;
    if (s->edge == YGFX_EDGE_GAUSSIAN && ew > 0) m += 5.0f * ew;
    return ygfx__reach(s, shape, m) + 1.0f;
}

/* Packs one stimulus into its 256-byte std140 block (the 16 vec4 of the
 * contract). Returns the pipeline, or 0 when the stimulus cannot be drawn,
 * with *why the code. */
/* A refusal names its reason: draw() runs on the owning thread only. */
static uint32_t ygfx__refuse(ygfx_gfx* g, int* why, int code, const char* msg) {
    *why = code;
    ygfx__set_error(g->error, sizeof g->error, "ysp_gfx: %s", msg);
    return 0;
}

/* The backend blend of a color stimulus; -1 when s->blend is not one. */
static int32_t ygfx__blend_of(const ygfx_stim* s) {
    switch (s->blend) {
    case YGFX_BLEND_MODE_OVER:     return 0;
    case YGFX_BLEND_MODE_ADD:      return YGFX_BLEND_ADD;
    case YGFX_BLEND_MODE_MULTIPLY: return YGFX_BLEND_MULTIPLY;
    default:                         return -1;
    }
}

/* The video program's constants (VIDEO), b[36..59] of an IMAGE's block:
 * range, matrix, kind, transfer, siting, chroma filter, and the primaries'
 * matrix into device RGB (relative: the source's D65 white at Y = 1 goes to
 * the display's white, no chromatic adaptation: ycol_prim_to_rgb()). */
static void ygfx__video_consts(const ygfx_gfx* g, const ygfx__res* r, float* b) {
    const ygfx_encoding* e = &r->enc;
    double kr = 0.299, kb = 0.114, kg, m[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
    int lim = e->range == YGFX_RANGE_LIMITED;
    if (e->matrix == YGFX_MATRIX_BT709) { kr = 0.2126; kb = 0.0722; }
    else if (e->matrix == YGFX_MATRIX_BT2020) { kr = 0.2627; kb = 0.0593; }
    kg = 1.0 - kr - kb;
    b[36] = (float)(lim ? 255.0 / 219.0 : 1.0);  b[37] = (float)(lim ? -16.0 / 219.0 : 0.0);
    b[38] = (float)(lim ? 255.0 / 224.0 : 1.0);  b[39] = (float)(lim ? -128.0 / 224.0 : -128.0 / 255.0);
    b[40] = (float)(2.0 * (1.0 - kr));           b[41] = (float)(2.0 * kb * (1.0 - kb) / kg);
    b[42] = (float)(2.0 * kr * (1.0 - kr) / kg); b[43] = (float)(2.0 * (1.0 - kb));
    b[44] = (float)(r->format == YGFX_NV12 ? 1 : (r->format == YGFX_I420 ? 2 : 0));
    b[45] = (float)e->transfer;
    b[46] = e->siting == YGFX_SITING_LEFT || e->siting == YGFX_SITING_TOP_LEFT ? 0.25f : 0.0f;
    b[47] = e->siting == YGFX_SITING_TOP_LEFT ? 0.25f : 0.0f;
    b[48] = (float)e->chroma_nearest;
    if (e->primaries > YGFX_PRIM_DEVICE && e->primaries <= YGFX_PRIM_BT2020 && g->has_xyz) {
        double mp[9];   /* relative, no adaptation: the source's D65 white to the display's white */
        if (ycol_prim_to_rgb(&g->color, e->primaries, mp, NULL) == YCOL_OK) memcpy(m, mp, sizeof mp);
    }
    b[49] = (float)m[0]; b[50] = (float)m[1]; b[51] = (float)m[2];
    b[52] = (float)m[3]; b[53] = (float)m[4]; b[54] = (float)m[5]; b[55] = (float)m[6];
    b[56] = (float)m[7]; b[57] = (float)m[8];
}

static uint32_t ygfx__pack(ygfx_gfx* g, const ygfx_stim* s, float* b, ygfx__cmd* cmd, int* why) {
    const float k = ygfx__scale(s), ppu = g->ppu * k;
    float hx, hy, sp[3], ew = s->edge_width, ml = 0.0f, mrect[4] = { 0, 0, 0, 0 };
    float dir[3], gate = s->gate * (s->group ? s->group->opacity : 1.0f);
    int i, mod = 0, shape, mask_kind = 0;
    uint32_t pipe = 0;
    memset(b, 0, YGFX__BLOCK);
    memset(cmd, 0, sizeof *cmd);
    cmd->nblk = 1;
    *why = YGFX_ERR_ARG;
    if (s->edge == YGFX_EDGE_HARD || ew < 0) ew = 0;
    cmd->blend = ygfx__blend_of(s);
    if (cmd->blend < 0) return ygfx__refuse(g, why, YGFX_ERR_ARG, "blend is not a ygfx_blend_mode");
    if (s->kind == YGFX_TEXT) return ygfx__text_pack(g, s, b, cmd, why);
    if (s->kind != YGFX_SHAPE && (s->offset != 0.0f || s->dash[0] > 0.0f || ygfx__trimmed(s) || s->fx || s->paint || s->prims))
        return ygfx__refuse(g, why, YGFX_ERR_ARG, "offset, dashes, trim, fx, paint and prims are SHAPE fields");
    if (ygfx__is_vector(s)) {   /* the vector program: its own block layout */
        double ext[2];
        const char* err = "";
        int clip = 0, n;
        ygfx__place(g, s, b);
        n = ygfx__vpack(g, s, b, ext, &clip, &err);
        if (n < 0) return ygfx__refuse(g, why, n, err);
        cmd->nblk = (uint32_t)n;
        g->clipped += (uint32_t)clip;
        if (!s->paint || s->paint->kind == YGFX_PAINT_SOLID)
            for (i = 0; i < 3; i++)
                if (s->color[i] < 0.0f || s->color[i] > 1.0f) { g->clipped++; break; }
        *why = YGFX_OK;
        return g->builtin[YGFX__B_VECTOR];
    }
    if (s->shape > YGFX_MASK_TEX)
        return ygfx__refuse(g, why, YGFX_ERR_ARG, "the v0.3 kinds are SHAPE kinds, not apertures");
    shape = ygfx__geom(g, s, &hx, &hy, sp);
    switch (s->kind) {
    case YGFX_SHAPE:
        if (ygfx__is_glyphs(s)) {
            if (s->buf.id < 1 || s->buf.id > YGFX_MAX_BUFFERS || !g->buf[s->buf.id - 1].used ||
                ((size_t)s->first + s->count) * sizeof(ygfx_glyph) > (size_t)g->buf[s->buf.id - 1].w)
                return ygfx__refuse(g, why, YGFX_ERR_ARG, "a glyph run's buffer holds fewer than count glyphs");
            pipe = g->builtin[YGFX__B_GLYPHS];
            cmd->inst = g->buf[s->buf.id - 1].bid;
            cmd->count = s->count;
            cmd->inst_off = s->first * (uint32_t)sizeof(ygfx_glyph);
        } else {
            pipe = g->builtin[YGFX__B_SHAPE];
        }
        break;
    case YGFX_GRATING: pipe = g->builtin[YGFX__B_GRATING]; mod = 1; break;
    case YGFX_GABOR:
        if (!(s->sigma > 0)) return ygfx__refuse(g, why, YGFX_ERR_ARG, "a gabor needs sigma > 0");
        pipe = g->builtin[YGFX__B_GABOR];
        mod = 1;
        break;
    case YGFX_NOISE:
        if (s->wave > YGFX_SIMPLEX) return ygfx__refuse(g, why, YGFX_ERR_ARG, "noise dist is UNIFORM, BINARY, GAUSSIAN or SIMPLEX");
        if (s->wave == YGFX_SIMPLEX) {
            const char* e = ygfx__gn_setup(g, s, NULL);
            if (e) return ygfx__refuse(g, why, YGFX_ERR_ARG, e);
        }
        pipe = g->builtin[YGFX__B_NOISE];
        mod = 1;
        if (s->wave == YGFX_GAUSSIAN) cmd->tex[0] = g->gauss;
        break;
    case YGFX_IMAGE: {
        const ygfx__res* r;
        float sx, sy, sw, sh;
        if (s->tex.id < 1 || s->tex.id > YGFX_MAX_TEXTURES || !g->tex[s->tex.id - 1].used)
            return ygfx__refuse(g, why, YGFX_ERR_ARG, "an image needs a texture of this gfx (tex)");
        r = &g->tex[s->tex.id - 1];
        if (r->view) r = &g->tex[r->view - 1];   /* rebound (VIDEO) */
        mod = (s->flags & YGFX_STIM_MODULATION) != 0;
        if (r->enc.matrix) {
            if (mod || (s->flags & YGFX_STIM_ADD))
                return ygfx__refuse(g, why, YGFX_ERR_ARG, "an encoded texture (video, or a stated transfer) draws as a color image only: "
                                                             "a modulation, add, coverage or shader texture holds linear values (enc all zero)");
            pipe = g->video_pipe;
            cmd->tex[0] = r->bid; cmd->tex[2] = r->plane_bid[0]; cmd->tex[4] = r->plane_bid[1]; cmd->tex[5] = g->eotf;
            ygfx__video_consts(g, r, b);
        } else {
            pipe = g->builtin[(s->flags & YGFX_STIM_ADD) ? YGFX__B_IMAGE_ADD
                              : (mod ? YGFX__B_IMAGE_MOD : YGFX__B_IMAGE_COLOR)];
            if (r->format == YGFX_R16UI) cmd->tex[3] = r->bid; else cmd->tex[0] = r->bid;
        }
        b[28] = (r->format == YGFX_R8 || r->format == YGFX_R16F || r->format == YGFX_R32F ||
                 r->format == YGFX_R16UI) ? 1.0f : (r->format == YGFX_RG8 ? 2.0f : 4.0f);
        b[29] = r->format == YGFX_R16UI ? 2.0f : ((s->flags & YGFX_STIM_LINEAR) ? 1.0f : 0.0f);
        b[30] = (s->flags & YGFX_STIM_PREMULTIPLIED) ? 1.0f : 0.0f;
        if (s->flags & YGFX_STIM_COVERAGE) {
            if (b[28] != 1.0f || mod || (s->flags & YGFX_STIM_ADD) || r->enc.matrix)
                return ygfx__refuse(g, why, YGFX_ERR_ARG, "coverage is the red channel of a 1-channel texture of linear values (enc all zero) drawn as a color image");
            b[31] = 1.0f;
        }
        /* the source rectangle, whole texels inside the texture */
        sx = s->src[0]; sy = s->src[1]; sw = s->src[2]; sh = s->src[3];
        if (!(sw > 0.0f && sh > 0.0f)) { sx = 0; sy = 0; sw = (float)r->w; sh = (float)r->h; }
        sx = floorf(sx); sy = floorf(sy); sw = floorf(sw); sh = floorf(sh);
        if (sx < 0 || sy < 0 || sw < 1 || sh < 1 || sx + sw > (float)r->w || sy + sh > (float)r->h)
            return ygfx__refuse(g, why, YGFX_ERR_ARG, "the image's src rectangle is not inside its texture");
        b[32] = sx; b[33] = sy; b[34] = sw; b[35] = sh;
        break;
    }
    case YGFX_DOTS:
        if (s->buf.id < 1 || s->buf.id > YGFX_MAX_BUFFERS || !g->buf[s->buf.id - 1].used)
            return ygfx__refuse(g, why, YGFX_ERR_ARG, "a dot field needs a buffer of this gfx (buf)");
        if (((size_t)s->first + s->count) * 8u > (size_t)g->buf[s->buf.id - 1].w)
            return ygfx__refuse(g, why, YGFX_ERR_ARG, "a dot field's buffer holds fewer than first + count x, y pairs");
        if (!(s->dot_size > 0.0f)) return ygfx__refuse(g, why, YGFX_ERR_ARG, "a dot field needs dot_size > 0 (px)");
        if ((shape == YGFX_RECT || shape == YGFX_CIRCLE) && !(hx > 0.0f && hy > 0.0f))
            return ygfx__refuse(g, why, YGFX_ERR_ARG, "a dot field's RECT or CIRCLE aperture has no area: set w (and h), or no aperture");
        pipe = g->builtin[YGFX__B_DOTS];
        cmd->inst = g->buf[s->buf.id - 1].bid;
        cmd->count = s->count;
        cmd->inst_off = s->first * 8u;
        break;
    case YGFX_USER:
        if (s->pipe.id < 1 || s->pipe.id > YGFX_MAX_PIPELINES || !g->pipe[s->pipe.id - 1].used)
            return ygfx__refuse(g, why, YGFX_ERR_ARG, "a user stimulus needs a pipeline of this gfx (pipe)");
        pipe = g->pipe[s->pipe.id - 1].bid;
        mod = g->pipe[s->pipe.id - 1].flags != YGFX_COLOR;
        /* stim.tex is the body's ysp_tex0: bound here, so the feedback
         * check below sees a body that samples its own target */
        if (s->tex.id) {
            const ygfx__res* r;
            if (s->tex.id > YGFX_MAX_TEXTURES || !g->tex[s->tex.id - 1].used)
                return ygfx__refuse(g, why, YGFX_ERR_ARG, "a user stimulus's tex is not a texture of this gfx");
            r = &g->tex[s->tex.id - 1];
            if (r->view) r = &g->tex[r->view - 1];
            if (r->enc.matrix)
                return ygfx__refuse(g, why, YGFX_ERR_ARG, "an encoded texture (video, or a stated transfer) draws as a color image only: "
                                                             "a modulation, add, coverage or shader texture holds linear values (enc all zero)");
            if (r->format == YGFX_R16UI) cmd->tex[3] = r->bid; else cmd->tex[0] = r->bid;
        }
        break;
    default:
        return 0;
    }
    /* a stimulus that can cover no pixel is refused, not drawn as nothing
     * (v0.3's dot fields drew nothing by their zero default) */
    if (s->kind != YGFX_DOTS && !ygfx__is_glyphs(s)) {
        char msg[200];
        const char* no = NULL;
        if (!(hx > 0.0f && hy > 0.0f)) no = shape == YGFX_LINE ? "a LINE needs a length w and a width shape_p[0] > 0" : "its box has no area: set w and h";
        else if (shape == YGFX_ANNULUS && !(sp[0] < hx)) no = "an ANNULUS's inner radius shape_p[0] reaches its outer radius";
        else if (shape == YGFX_CROSS && !(sp[0] > 0.0f)) no = "a CROSS needs an arm width shape_p[0] > 0";
        else if (shape == YGFX_POLYGON && !(sp[2] >= 3.0f && sp[2] <= 16.0f)) no = "a POLYGON has 3 to 16 vertices (shape_p[0])";
        if (no) {
            snprintf(msg, sizeof msg, "%s; it would cover no pixel (box %.4g x %.4g px)", no, 2.0 * hx, 2.0 * hy);
            return ygfx__refuse(g, why, YGFX_ERR_ARG, msg);
        }
    }
    if (mod && s->blend != YGFX_BLEND_MODE_OVER)
        return ygfx__refuse(g, why, YGFX_ERR_ARG, "a modulation adds: blend is for color stimuli");
    if (s->kind == YGFX_IMAGE && (s->flags & YGFX_STIM_ADD) && s->blend != YGFX_BLEND_MODE_OVER)
        return ygfx__refuse(g, why, YGFX_ERR_ARG, "an ADD image adds already");
    if (shape == YGFX_MASK_TEX) {
        const ygfx__res* m;
        double pxpt, reach, out_r, in_r, er = ygfx__edge_reach(s);
        if (s->kind == YGFX_DOTS || s->mask.id < 1 || s->mask.id > YGFX_MAX_TEXTURES || !g->tex[s->mask.id - 1].used)
            return ygfx__refuse(g, why, YGFX_ERR_ARG, "MASK_TEX needs a texture of this gfx (mask); dots have no mask");
        m = &g->tex[s->mask.id - 1];
        mask_kind = m->sdf ? m->sdf : YGFX_SDF_DIST;
        if (mask_kind == YGFX_SDF_DIST && m->format != YGFX_R16F && m->format != YGFX_R32F)
            return ygfx__refuse(g, why, YGFX_ERR_ARG, "a distance mask is R16F or R32F");
        if (mask_kind != YGFX_SDF_DIST && !(m->sdf_range > 0.0f && (m->format == YGFX_RGBA8 || m->format == YGFX_RGBA16F ||
                                                                    m->format == YGFX_RGBA32F)))
            return ygfx__refuse(g, why, YGFX_ERR_ARG, "an MSDF or MTSDF mask is RGBA with an sdf_range");
        if (mask_kind == YGFX_SDF_MSDF && s->stroke > 0.0f && s->join == YGFX_JOIN_ROUND)
            return ygfx__refuse(g, why, YGFX_ERR_ARG, "an MSDF's offsets are mitered: a stroke on it needs JOIN_MITER, "
                                                         "or an MTSDF atlas for round joins");
        if (ygfx__is_glyphs(s)) {
            pxpt = (double)s->shape_p[0] * ppu;
            if (!(pxpt > 0.0)) return ygfx__refuse(g, why, YGFX_ERR_ARG, "a glyph run needs a scale (units per texel)");
            b[29] = (float)pxpt;
        } else {
            /* the mask's rectangle: src on every kind but IMAGE (whose src is
             * its texture's); clamped addresses, so no neighbor bleeds */
            mrect[2] = (float)m->w; mrect[3] = (float)m->h;
            if (s->kind != YGFX_IMAGE && s->src[2] > 0.0f && s->src[3] > 0.0f) {
                mrect[0] = floorf(s->src[0]); mrect[1] = floorf(s->src[1]); mrect[2] = floorf(s->src[2]); mrect[3] = floorf(s->src[3]);
                if (mrect[0] < 0 || mrect[1] < 0 || mrect[2] < 1 || mrect[3] < 1 || mrect[0] + mrect[2] > (float)m->w ||
                    mrect[1] + mrect[3] > (float)m->h)
                    return ygfx__refuse(g, why, YGFX_ERR_ARG, "the mask's src rectangle is not inside its texture");
            }
            /* one scale on both axes, or the distance would be wrong */
            if (!(hy > 0.0f) || fabs((double)hx / hy - (double)mrect[2] / mrect[3]) > 1e-3 * ((double)mrect[2] / mrect[3]))
                return ygfx__refuse(g, why, YGFX_ERR_ARG, "a mask's box must have its texture's (or src rectangle's) aspect ratio");
            pxpt = 2.0 * hx / mrect[2];
        }
        /* The range rule: the atlas encodes distances to range / 2 texels;
         * what the edge, stroke and offset reach must lie inside it, 1.5
         * texels in for the bilinear neighbors. A Gaussian reaches 5 SD. */
        out_r = (s->stroke > 0.0f && s->stroke_align != YGFX_STROKE_INSIDE ?
                 (s->stroke_align == YGFX_STROKE_OUTSIDE ? s->stroke : 0.5 * s->stroke) : 0.0) + s->offset + er;
        in_r = (s->stroke > 0.0f && s->stroke_align != YGFX_STROKE_OUTSIDE ?
                (s->stroke_align == YGFX_STROKE_INSIDE ? s->stroke : 0.5 * s->stroke) : 0.0) - s->offset + er;
        reach = out_r > in_r ? out_r : in_r;
        /* A DIST atlas holds true distances inside, so only the outside reach
         * meets its padding (sdf_range = twice the padding). A DIST mask with
         * no sdf_range is v0.2's whole-texture mask, unchecked; rectangles and
         * glyph runs on one must state it. */
        if (mask_kind == YGFX_SDF_DIST) {
            if (!(m->sdf_range > 0.0f) && (ygfx__is_glyphs(s) || (s->kind != YGFX_IMAGE && s->src[2] > 0.0f && s->src[3] > 0.0f)))
                return ygfx__refuse(g, why, YGFX_ERR_RANGE, "a DIST atlas drawn by rectangles or as a glyph run needs "
                                      "texture_desc.sdf_range (twice its padding in texels), so the range rule can be checked");
            reach = m->sdf_range > 0.0f ? out_r : 0.0;
        }
        if (m->sdf_range > 0.0f && reach > (0.5 * m->sdf_range - 1.5) * pxpt) {
            char msg[240];
            snprintf(msg, sizeof msg, "the atlas range %.3g texels is too small: the edge, stroke and offset reach %.3g px "
                     "at %.4g px per texel, which needs a range of %.3g", (double)m->sdf_range, reach, pxpt, 2.0 * (reach / pxpt + 1.5));
            return ygfx__refuse(g, why, YGFX_ERR_RANGE, msg);
        }
        cmd->tex[1] = m->bid;
    }
    if (s->kind != YGFX_DOTS && shape != YGFX_NO_APERTURE && shape != YGFX_MASK_TEX) {
        if (ygfx__miter(s, shape, &ml) < 0)
            return ygfx__refuse(g, why, YGFX_ERR_ARG, "MITER is for RECT without a corner radius, CROSS and a convex POLYGON");
    }
    /* a target pass must not sample its own target */
    if (g->in_target)
        for (i = 0; i < 6; i++)
            if (cmd->tex[i] && cmd->tex[i] == g->tex[g->cur_target_tex - 1].bid) { *why = YGFX_ERR_ORDER; return 0; }
    ygfx__place(g, s, b);                 /* xf */
    if (s->kind == YGFX_DOTS) {
        float r = 0.5f * s->dot_size;
        b[4] = r; b[5] = r; b[6] = ppu; b[7] = ygfx__margin(s, YGFX_CIRCLE, ew);
        b[20] = (float)YGFX_CIRCLE;
        /* the field's aperture, which the vertex shader tests each dot
         * center against: half box, 0 rect, 1 circle, 2 none */
        b[24] = hx; b[25] = hy;
        b[26] = (float)(shape == YGFX_RECT ? 0 : (shape == YGFX_CIRCLE ? 1 : 2));
    } else {
        b[4] = hx; b[5] = hy; b[6] = ppu; b[7] = ygfx__margin(s, shape, ew);
        b[20] = (float)shape;
        b[24] = sp[0]; b[25] = sp[1]; b[26] = sp[2];
    }
    /* look: contrast, opacity, gate, the stroke band's center offset;
     * shape.w: the band's half width (0: a fill) */
    b[8] = s->contrast; b[9] = s->opacity; b[10] = gate;
    /* DOTS: the band is each dot's ring, whatever the field's aperture */
    if (s->stroke > 0.0f && (s->kind == YGFX_DOTS || shape != YGFX_NO_APERTURE)) {
        float a1, a2;
        ygfx__band(s, &a1, &a2);
        b[11] = 0.5f * (a1 + a2);
        b[27] = 0.5f * (a2 - a1);
    }
    ygfx__copy3(b + 12, s->color);
    dir[0] = s->dir[0]; dir[1] = s->dir[1]; dir[2] = s->dir[2];
    if (dir[0] == 0.0f && dir[1] == 0.0f && dir[2] == 0.0f) ygfx__copy3(dir, g->bg);
    if (s->kind == YGFX_IMAGE) {
        /* tint: in color for the color and ADD paths, folded into dir for
         * a modulation */
        for (i = 0; i < 4; i++) b[12 + i] = s->tint[i];
        for (i = 0; i < 3; i++) dir[i] *= s->tint[i] * s->tint[3];
    }
    ygfx__copy3(b + 16, dir);
    b[19] = ml;
    b[21] = (float)s->edge; b[22] = ew; b[23] = (float)s->wave;
    if (s->kind == YGFX_GRATING || s->kind == YGFX_GABOR) {
        b[28] = s->sf / ppu; b[29] = s->phase; b[30] = s->sigma * ppu; b[31] = s->aspect;
    } else if (s->kind == YGFX_NOISE) {
        b[28] = s->check > 0 ? s->check : 1.0f; b[29] = s->seed; b[30] = (float)s->wave;
    }
    if (s->kind == YGFX_NOISE && s->wave == YGFX_SIMPLEX) {
        ygfx__gn g8;
        ygfx__gn_setup(g, s, &g8);
        for (i = 0; i < 8; i++) {
            b[32 + i] = (float)g8.r[i];
            b[40 + i] = (float)(g8.z[i] >> 16); b[48 + i] = (float)(g8.z[i] & 0xFFFFu);
            b[56 + i] = (float)g8.w[i];
        }
        b[31] = (float)g8.n;
    } else if (s->kind != YGFX_IMAGE) {
        if (shape == YGFX_POLYGON && s->kind != YGFX_USER) {
            for (i = 0; i < 32; i++) b[32 + i] = s->p[i] * ppu;
        } else {
            for (i = 0; i < 32; i++) b[32 + i] = s->p[i];
        }
    }
    if (s->kind == YGFX_SHAPE) b[28] = s->offset;   /* misc.x, the shape program's YSP_OFFSET */
    if (shape == YGFX_MASK_TEX) {
        /* shape.x the kind (1 distance, 2 MSDF, 3 MTSDF), y the range, z 1
         * to read MTSDF's true distance (round offsets); p[28..31] the
         * rectangle (a USER body loses those four parameters) */
        b[24] = (float)mask_kind; b[25] = mask_kind == YGFX_SDF_DIST ? 0.0f : g->tex[s->mask.id - 1].sdf_range;
        b[26] = mask_kind == YGFX_SDF_MTSDF && ((s->stroke > 0.0f && s->join == YGFX_JOIN_ROUND) || s->offset != 0.0f) ? 1.0f : 0.0f;
        for (i = 0; i < 4; i++) b[60 + i] = mrect[i];
    }
    /* A CPU bound on leaving 0..1, so a clip is counted without a readback. */
    if (s->kind == YGFX_IMAGE && (s->flags & YGFX_STIM_ADD)) {
        /* an increment layer's range is the texture's: not bounded here */
    } else if (mod) {
        float kk = fabsf(s->contrast * gate);
        for (i = 0; i < 3; i++) {
            float lo = g->bg[i] - kk * fabsf(dir[i]), hi = g->bg[i] + kk * fabsf(dir[i]);
            if (lo < -1e-6f || hi > 1.0f + 1e-6f) { g->clipped++; break; }
        }
    } else if (s->kind != YGFX_IMAGE) {
        for (i = 0; i < 3; i++)
            if (s->color[i] < 0.0f || s->color[i] > 1.0f) { g->clipped++; break; }
    }
    *why = YGFX_OK;
    return pipe;
}

static const char ygfx__body_image_video[] =
    "uniform sampler2D ysp_tex3;\n"   /* Cr (I420), unit 4 */
    "uniform sampler2D ysp_tex4;\n"   /* the display's transfer, 256 x 3, unit 5 */
    "float ysp_eotf_(float v, int tr, int ch) {\n"
    "    if (tr == 1) {\n"
    "        float x = v * 255.0;\n"
    "        int i0 = min(int(x), 254);\n"
    "        float a = texelFetch(ysp_tex4, ivec2(i0, ch), 0).r, b = texelFetch(ysp_tex4, ivec2(i0 + 1, ch), 0).r;\n"
    "        return a + (b - a) * (x - float(i0));\n"
    "    }\n"
    "    if (tr == 2) return pow(v, 2.4);\n"
    "    if (tr == 3) return v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4);\n"
    "    if (tr == 5) return pow(v, 2.2);\n"
    "    return v;\n"
    "}\n"
    /* Y' at the image point; chroma at the stated siting in its own plane
     * (texel-edge coordinates c = L / 2 + offset), bilinear and clamped
     * to the plane, or replicated; the matrix and range; R'G'B' clamped;
     * the transfer; the primaries */
    "vec4 ysp_texel_(vec2 p) {\n"
    "    vec2 r = vec2(p.x + ysp_size.x, ysp_size.y + p.y) / (2.0 * ysp_size.xy);\n"
    "    vec4 sr = ysp_vk[0], K = ysp_vk[1], C = ysp_vk[2], F = ysp_vk[3], M0 = ysp_vk[4], M1 = ysp_vk[5], M2 = ysp_vk[6];\n"
    "    bool lin = ysp_misc.y > 0.5;\n"
    "    vec2 L = sr.xy + r * sr.zw;\n"
    "    ivec2 t = ivec2(sr.xy) + clamp(ivec2(floor(r * sr.zw)), ivec2(0), ivec2(sr.zw) - 1);\n"
    "    vec4 y = lin ? ysp_bilinear(ysp_tex0, L, sr) : texelFetch(ysp_tex0, t, 0);\n"
    "    int kind = int(F.x);\n"
    "    vec3 e;\n"
    "    float a = 1.0;\n"
    /* a gray texture's one or two channels: the level in r, alpha in g */
    "    if (kind == 0) {\n"
    "        int ch = int(ysp_misc.x);\n"
    "        vec4 v = ch == 1 ? vec4(y.rrr, 1.0) : (ch == 2 ? vec4(y.rrr, y.g) : y);\n"
    "        e = v.rgb * K.x + K.y; a = v.a;\n"
    "    }\n"
    "    else {\n"
    "        vec2 Lc = lin ? L : vec2(t) + 0.5;\n"
    "        vec2 c0 = floor(sr.xy * 0.5);\n"
    "        vec4 cr = vec4(c0, ceil((sr.xy + sr.zw) * 0.5) - c0);\n"
    "        vec2 q;\n"
    "        if (M0.x > 0.5) {\n"
    "            ivec2 n = clamp(ivec2(floor(Lc)) / 2, ivec2(cr.xy), ivec2(cr.xy + cr.zw) - 1);\n"
    "            q = kind == 1 ? texelFetch(ysp_tex2, n, 0).rg : vec2(texelFetch(ysp_tex2, n, 0).r, texelFetch(ysp_tex3, n, 0).r);\n"
    "        } else {\n"
    "            vec2 c = Lc * 0.5 + F.zw;\n"
    "            q = kind == 1 ? ysp_bilinear(ysp_tex2, c, cr).rg : vec2(ysp_bilinear(ysp_tex2, c, cr).r, ysp_bilinear(ysp_tex3, c, cr).r);\n"
    "        }\n"
    "        float Y = y.r * K.x + K.y;\n"
    "        q = q * K.z + K.w;\n"   /* Cb, Cr */
    "        e = vec3(Y + C.x * q.y, Y - C.y * q.x - C.z * q.y, Y + C.w * q.x);\n"
    "    }\n"
    "    e = clamp(e, 0.0, 1.0);\n"
    "    int tr = int(F.y);\n"
    "    vec3 l = vec3(ysp_eotf_(e.r, tr, 0), ysp_eotf_(e.g, tr, 1), ysp_eotf_(e.b, tr, 2));\n"
    "    return vec4(dot(M0.yzw, l), dot(M1.xyz, l), dot(vec3(M1.w, M2.xy), l), a);\n"
    "}\n";

/* --- instances (INSTANCES) ------------------------------------------------------ */

#define YGFX__I_ALL 0x7Fu

/* The instanced program of a base program (backend id), 0 when none was
 * made: built-in i, the video program, or a user's. */
static uint32_t* ygfx__inst_slot(ygfx_gfx* g, uint32_t pipe) {
    int i;
    for (i = 0; i < YGFX__N_BUILTIN; i++) if (g->builtin[i] == pipe) return &g->inst_pipe[i];
    if (g->video_pipe && g->video_pipe == pipe) return &g->inst_pipe[YGFX__N_BUILTIN];
    for (i = 0; i < YGFX_MAX_PIPELINES; i++) if (g->pipe[i].used && g->pipe[i].bid == pipe) return &g->pipe_inst[i];
    return NULL;
}

/* Makes the instanced program of base program pipe: its text with YSP_INST
 * (and the palette going to dir for a modulation, a user body's own px). */
static int ygfx__inst_program(ygfx_gfx* g, uint32_t pipe) {
    uint32_t* slot = ygfx__inst_slot(g, pipe);
    ygfx__prec r;
    char defs[160];
    char* body = NULL;
    int mode, rc, i;
    if (!slot) return YGFX_ERR_ARG;
    if (*slot) return YGFX_OK;
    for (i = 0; i < YGFX__N_BUILTIN && g->builtin[i] != pipe; i++) {}
    if (i < YGFX__N_BUILTIN) {
        mode = ygfx__builtins[i].mode;
        body = ygfx__builtin_body(i);
        snprintf(defs, sizeof defs, "%s#define YSP_INST 1\n%s", ygfx__builtins[i].defs,
                 mode == YGFX_MODULATION ? "#define YSP_INST_MOD 1\n" : "");
    } else if (pipe == g->video_pipe) {
        size_t n = sizeof ygfx__body_image_video + sizeof ygfx__body_image_color;
        mode = YGFX_COLOR;
        body = (char*)malloc(n);
        if (body) snprintf(body, n, "%s%s", ygfx__body_image_video, ygfx__body_image_color);
        snprintf(defs, sizeof defs, "#define YSP_VIDEO 1\n#define YSP_INST 1\n");
    } else {
        for (i = 0; i < YGFX_MAX_PIPELINES && !(g->pipe[i].used && g->pipe[i].bid == pipe); i++) {}
        if (i == YGFX_MAX_PIPELINES || !g->pipe_body[i]) return YGFX_ERR_FULL;
        mode = g->pipe[i].flags;
        body = (char*)malloc(strlen(g->pipe_body[i]) + 1);
        if (body) memcpy(body, g->pipe_body[i], strlen(g->pipe_body[i]) + 1);
        snprintf(defs, sizeof defs, "#define YSP_INST 1\n#define YSP_INST_USER 1\n%s",
                 mode == YGFX_MODULATION ? "#define YSP_INST_MOD 1\n" : "");
    }
    if (!body) return YGFX_ERR_FULL;
    rc = ygfx__make_pipe(g, defs, body, mode, 2, 0, slot, "instanced", &r);
    free(body);
    g->be->reset(g->bctx);
    return rc < 0 ? rc : YGFX_OK;
}

/* The element records' staging and their buffers, at the first instanced
 * stimulus: an open without instances allocates nothing for them. */
static int ygfx__inst_memory(ygfx_gfx* g) {
    int i;
    if (g->inst_staging) return YGFX_OK;
    g->inst_staging = (unsigned char*)malloc((size_t)g->inst_cap * sizeof(ygfx_inst));
    if (!g->inst_staging) return YGFX_ERR_FULL;
    for (i = 0; i < YGFX__UBO_RING; i++)
        if (g->be->buffer_make(g->bctx, YGFX_BUFFER_INSTANCE, (size_t)g->inst_cap * sizeof(ygfx_inst), &g->inst_buf[i]) < 0) {
            free(g->inst_staging);
            g->inst_staging = NULL;
            return YGFX_ERR_GL;
        }
    return YGFX_OK;
}

/* What an element may set, for this template: NULL when it can be drawn. */
static const char* ygfx__inst_refusal(const ygfx_gfx* g, const ygfx_stim* s, uint32_t f) {
    if (s->kind == YGFX_DOTS) return "DOTS are instanced already: a dot field cannot be a template";
    if (ygfx__is_glyphs(s)) return "a glyph run is instanced already: it cannot be a template";
    if (s->kind != YGFX_SHAPE && s->kind != YGFX_GRATING && s->kind != YGFX_GABOR && s->kind != YGFX_NOISE &&
        s->kind != YGFX_IMAGE && s->kind != YGFX_USER) return "the template's kind cannot be instanced";
    if (f & ~YGFX__I_ALL) return "inst_fields holds bits that are not YGFX_I_*";
    if ((f & YGFX_I_PHASE) && s->kind != YGFX_GRATING && s->kind != YGFX_GABOR) return "YGFX_I_PHASE is for GRATING and GABOR";
    if ((f & YGFX_I_SCALE) && s->kind == YGFX_NOISE) return "YGFX_I_SCALE is not for NOISE: its check is px";
    if ((f & YGFX_I_SCALE) && s->dash[0] > 0.0f) return "YGFX_I_SCALE with dashes: the dash and gap are px";
    if ((f & YGFX_I_SCALE) && s->fx && (s->fx->flags & YGFX_FX_EXACT_BLUR)) return "YGFX_I_SCALE with EXACT_BLUR";
    if ((f & YGFX_I_COLOR) && (!s->palette || s->n_palette < 1 || s->n_palette > YGFX_MAX_PALETTE))
        return "YGFX_I_COLOR needs a palette of 1 to YGFX_MAX_PALETTE (16) rgb triples";
    if (!s->inst || s->n_inst < 1) return "no elements";
    if ((int32_t)s->n_inst > g->inst_cap) return "more elements than desc.max_instances";
    return NULL;
}

/* What a refused instanced stimulus points at: no elements, so it draws
 * nothing, never its template alone. */
static const ygfx_inst ygfx__no_inst = { 0, 0, 0, 0, 0, 0, 0, 0 };

YGFX_API ygfx_stim ygfx_instances(ygfx_gfx* g, const ygfx_stim* tmpl, const ygfx_instances_desc* d) {
    ygfx_stim s;
    ygfx__cmd cmd;
    float blk[16 * 64];
    const char* why;
    uint32_t pipe;
    int rc;
    memset(&s, 0, sizeof s);
    s.inst = &ygfx__no_inst;
    if (!g || !g->open || !tmpl || !d) return s;
    s = *tmpl;
    s.inst = d->inst ? d->inst : &ygfx__no_inst;
    s.inst_fields = d->fields ? d->fields : YGFX_I_XY;
    s.palette = d->palette;
    s.n_palette = d->n_palette > 0 ? (uint32_t)d->n_palette : 0u;
    s.n_inst = d->n > 0 ? (uint32_t)d->n : 0u;
    g->error[0] = '\0';
    if (g->in_frame) { ygfx__set_error(g->error, sizeof g->error, "ysp_gfx: ygfx_instances() inside begin..end"); goto fail; }
    if ((why = ygfx__inst_refusal(g, &s, s.inst_fields)) != NULL) {
        ygfx__set_error(g->error, sizeof g->error, "ysp_gfx: %s", why);
        goto fail;
    }
    /* the template packed as it will draw: its program, and one block */
    pipe = ygfx__pack(g, tmpl, blk, &cmd, &rc);
    if (!pipe) { if (!g->error[0]) ygfx__set_error(g->error, sizeof g->error, "ysp_gfx: the template cannot be drawn"); goto fail; }
    if (cmd.nblk != 1) {
        ygfx__set_error(g->error, sizeof g->error, "ysp_gfx: a vector stimulus whose data fills more than its block cannot be a template");
        goto fail;
    }
    if (ygfx__sync(g) < 0 || ygfx__inst_memory(g) < 0 || ygfx__inst_program(g, pipe) < 0) {
        if (!g->error[0]) ygfx__set_error(g->error, sizeof g->error, "ysp_gfx: the instanced program or its buffers");
        goto fail;
    }
    return s;
fail:
    s.n_inst = 0;
    return s;
}

YGFX_API void ygfx_inst_grid(ygfx_inst* a, int nx, int ny, float dx, float dy) {
    int i, j;
    if (!a || nx < 1 || ny < 1) return;
    for (j = 0; j < ny; j++)
        for (i = 0; i < nx; i++) {
            ygfx_inst* e = &a[j * nx + i];
            memset(e, 0, sizeof *e);
            e->x = ((float)i - 0.5f * (float)(nx - 1)) * dx;
            e->y = ((float)j - 0.5f * (float)(ny - 1)) * dy;
            e->contrast = e->scale = e->gate = 1.0f;
        }
}

/* Whether an element's palette entry is a dir (a modulation) or a color. */
static int ygfx__inst_mod(const ygfx_gfx* g, const ygfx_stim* s) {
    if (s->kind == YGFX_GRATING || s->kind == YGFX_GABOR || s->kind == YGFX_NOISE) return 1;
    if (s->kind == YGFX_IMAGE) return (s->flags & YGFX_STIM_MODULATION) && !(s->flags & YGFX_STIM_ADD);
    if (s->kind == YGFX_USER) return s->pipe.id >= 1 && s->pipe.id <= YGFX_MAX_PIPELINES && g->pipe[s->pipe.id - 1].flags == YGFX_MODULATION;
    return 0;
}

/* Element i as a stimulus of its own (hit tests, the test's references):
 * the template in a one-point group at the element's anchor, turned and
 * scaled there, with its fields applied. */
static void ygfx__inst_elem(const ygfx_gfx* g, const ygfx_stim* s, int i, ygfx_stim* e, ygfx_group* gr) {
    const ygfx_inst* it = &s->inst[i];
    const uint32_t f = s->inst_fields;
    const double ppu = (double)g->ppu * ygfx__scale(s);
    double a[2], m[2], c, sn, ax, ay;
    ygfx__frame_px(g, s, (double)g->w, (double)g->h, a, m, &c, &sn);
    ax = a[0]; ay = a[1];
    if (f & YGFX_I_XY) { ax += ppu * ((double)it->x * c - (double)it->y * sn); ay += ppu * ((double)it->x * sn + (double)it->y * c); }
    *e = *s;
    e->inst = NULL; e->n_inst = 0;
    memset(gr, 0, sizeof *gr);
    gr->place = YGFX_TOP_LEFT;
    gr->visible = 1.0f;
    gr->x = (float)(ax / g->ppu); gr->y = (float)(ay / g->ppu);
    gr->scale = (float)(ygfx__scale(s) * ((f & YGFX_I_SCALE) ? (it->scale > 0.0f ? it->scale : 0.0f) : 1.0f));
    gr->opacity = s->group ? s->group->opacity : 1.0f;
    e->group = gr;
    e->place = YGFX_CENTER;
    e->x = e->y = 0.0f;
    e->ori = (float)(atan2(sn, c) * (180.0 / 3.14159265358979323846) + ((f & YGFX_I_ORI) ? it->ori : 0.0f));
    if (f & YGFX_I_PHASE) e->phase += it->phase;
    if (f & YGFX_I_CONTRAST) { e->contrast *= it->contrast; e->opacity *= it->contrast; }
    if (f & YGFX_I_GATE) e->gate *= it->gate;
    if ((f & YGFX_I_COLOR) && s->palette && s->n_palette) {
        double u = it->color < 0.0f ? 0.0 : (it->color > (float)(s->n_palette - 1) ? (double)(s->n_palette - 1) : it->color);
        int i0 = (int)floor(u), i1 = i0 + 1 < (int)s->n_palette ? i0 + 1 : i0, k;
        double fr = u - i0;
        float* dst = ygfx__inst_mod(g, s) ? e->dir : (s->kind == YGFX_IMAGE ? e->tint : e->color);
        for (k = 0; k < 3; k++) dst[k] = (float)((1.0 - fr) * s->palette[3 * i0 + k] + fr * s->palette[3 * i1 + k]);
    }
}

YGFX_API int ygfx_hit_index(const ygfx_gfx* g, const ygfx_stim* s, float px, float py) {
    int i;
    if (g && g->open && s && s->kind == YGFX_TEXT)
        return s->visible >= 0.5f && (!s->group || s->group->visible >= 0.5f) ? ygfx__crun_hit(g, s, px, py) : -1;
    if (!g || !g->open || !s || !s->inst || !(s->visible >= 0.5f)) return -1;
    for (i = (int)s->n_inst - 1; i >= 0; i--) {
        ygfx_stim e;
        ygfx_group gr;
        ygfx__inst_elem(g, s, i, &e, &gr);
        if (!(e.gate != 0.0f)) continue;
        if (ygfx_hit(g, &e, px, py)) return i;
    }
    return -1;
}

YGFX_API void ygfx_inst_resolve(const ygfx_gfx* g, const ygfx_stim* s, int i, float* x, float* y) {
    ygfx_stim e;
    ygfx_group gr;
    if (!g || !s || !s->inst || i < 0 || i >= (int)s->n_inst) { if (x) *x = 0.0f; if (y) *y = 0.0f; return; }
    ygfx__inst_elem(g, s, i, &e, &gr);
    ygfx_resolve(g, &e, x, y);
}

/* draw()'s part: the instanced program, block 1 (the anchor and the
 * fields), block 2 (the palette), and the elements copied for end(). */
static uint32_t ygfx__inst_draw(ygfx_gfx* g, const ygfx_stim* s, float* b, ygfx__cmd* cmd, uint32_t pipe, int* why) {
    const uint32_t f = s->inst_fields;
    uint32_t* slot = ygfx__inst_slot(g, pipe);
    const char* no = ygfx__inst_refusal(g, s, f);
    double a[2], m[2], c, sn;
    float hx, hy, sp[3];
    size_t bytes;
    uint32_t k;
    if (no) return ygfx__refuse(g, why, YGFX_ERR_ARG, no);
    if (!slot || !*slot || !g->inst_staging)
        return ygfx__refuse(g, why, YGFX_ERR_ORDER, "ygfx_instances() makes the instanced program: make the stimulus "
                                                        "with it again after changing the template's kind or image");
    if (cmd->nblk != 1)
        return ygfx__refuse(g, why, YGFX_ERR_ARG, "a vector stimulus whose data fills more than its block cannot be a template");
    bytes = (size_t)s->n_inst * sizeof(ygfx_inst);
    if ((size_t)g->inst_used + bytes > (size_t)g->inst_cap * sizeof(ygfx_inst))
        return ygfx__refuse(g, why, YGFX_ERR_FULL, "the frame's elements pass desc.max_instances: none of this draw is drawn");
    ygfx__frame_px(g, s, (double)g->cur_w, (double)g->cur_h, a, m, &c, &sn);
    ygfx__geom(g, s, &hx, &hy, sp);
    memset(b + 64, 0, 2 * YGFX__BLOCK);
    b[64] = (float)a[0];
    b[65] = g->cur_top ? (float)a[1] : (float)((double)g->cur_h - a[1]);
    b[66] = (float)((0.5 - (double)s->ax) * 2.0 * hx);
    b[67] = (float)((0.5 - (double)s->ay) * 2.0 * hy);
    b[68] = g->ppu * ygfx__scale(s);
    b[69] = (float)f;
    b[70] = (float)s->n_palette;
    if ((f & YGFX_I_COLOR) && s->palette)
        for (k = 0; k < s->n_palette; k++) { b[128 + 4 * k] = s->palette[3 * k]; b[129 + 4 * k] = s->palette[3 * k + 1]; b[130 + 4 * k] = s->palette[3 * k + 2]; }
    memcpy(g->inst_staging + g->inst_used, s->inst, bytes);
    cmd->inst = g->inst_buf[g->ubo_i];
    cmd->inst_off = (uint32_t)g->inst_used;
    cmd->count = s->n_inst;
    cmd->nblk = 3;
    g->inst_used += (int32_t)bytes;
    *why = YGFX_OK;
    return *slot;
}

YGFX_API int ygfx_draw(ygfx_gfx* g, const ygfx_stim* s) {
    ygfx__cmd* cmd;
    uint32_t pipe;
    int why;
    YRT_ZONE(z, "ygfx.draw");
    if (!g || !g->open) { YRT_ZONE_END(z); return YGFX_ERR_CLOSED; }
    if (!g->in_frame) { YRT_ZONE_END(z); return YGFX_ERR_ORDER; }
    if (!s) { YRT_ZONE_END(z); return YGFX_ERR_ARG; }
    if (g->in_setup && !g->in_target) {
        ygfx__set_error(g->error, sizeof g->error, "ysp_gfx: a setup pass draws into targets only (the scene is a frame's)");
        YRT_ZONE_END(z);
        return YGFX_ERR_ORDER;
    }
    if (!(s->visible >= 0.5f) || (s->group && !(s->group->visible >= 0.5f))) { YRT_ZONE_END(z); return YGFX_OK; }
    /* an empty dot field, glyph run or element array is legal and draws
     * nothing (a refused ygfx_instances() gives one) */
    if (((s->kind == YGFX_DOTS || s->kind == YGFX_TEXT || ygfx__is_glyphs(s)) && s->count == 0) || (s->inst && s->n_inst == 0)) {
        YRT_ZONE_END(z);
        return YGFX_OK;
    }
    if (g->n_cmds >= g->max_draws || g->n_blocks >= g->max_draws) { YRT_ZONE_END(z); return YGFX_ERR_FULL; }
    cmd = &g->cmds[g->n_cmds];
    /* the staging has 16 blocks of slack, so a vector stimulus packs
     * before its blocks are counted */
    pipe = ygfx__pack(g, s, (float*)(g->staging + YGFX__FRAME_BYTES + (size_t)g->n_blocks * YGFX__BLOCK), cmd, &why);
    if (pipe && s->inst && s->n_inst)
        pipe = ygfx__inst_draw(g, s, (float*)(g->staging + YGFX__FRAME_BYTES + (size_t)g->n_blocks * YGFX__BLOCK), cmd, pipe, &why);
    else if (pipe && pipe == g->builtin[YGFX__B_VECTOR])   /* an element array keeps the generic program */
        pipe = ygfx__vspec_pick(g, (const float*)(g->staging + YGFX__FRAME_BYTES + (size_t)g->n_blocks * YGFX__BLOCK), pipe);
    if (!pipe) {
        YRT_ZONE_END(z);
        return why;
    }
    if (g->n_blocks + (int32_t)cmd->nblk > g->max_draws) { YRT_ZONE_END(z); return YGFX_ERR_FULL; }
    cmd->pipe = pipe;
    cmd->block = (uint32_t)g->n_blocks;
    g->n_blocks += (int32_t)cmd->nblk;
    g->n_cmds++;
    YRT_ZONE_END(z);
    return YGFX_OK;
}

YGFX_API int ygfx_draw_n(ygfx_gfx* g, const ygfx_stim* s, int n) {
    int i, rc, first = YGFX_OK;
    if (!s || n < 0) return YGFX_ERR_ARG;
    for (i = 0; i < n; i++) {
        rc = ygfx_draw(g, &s[i]);
        if (rc < 0 && first == YGFX_OK) first = rc;
        if (rc == YGFX_ERR_CLOSED || rc == YGFX_ERR_FULL) return rc;
    }
    return first;
}

static int ygfx__same_batch(const ygfx__cmd* a, const ygfx__cmd* b) {
    return a->pipe == b->pipe && a->inst == 0 && b->inst == 0 && a->nblk == 1 && b->nblk == 1 && a->blend == b->blend &&
           memcmp(a->tex, b->tex, sizeof a->tex) == 0;
}

/* Closes the segment being recorded and opens the next. */
static void ygfx__seg_next(ygfx_gfx* g, uint32_t target, int w, int h, int topdown, int slot, const float* clear) {
    ygfx__seg* sg;
    g->segs[g->n_segs - 1].end = g->n_cmds;
    sg = &g->segs[g->n_segs++];
    memset(sg, 0, sizeof *sg);
    sg->target = target; sg->w = w; sg->h = h; sg->topdown = topdown;
    sg->first = g->n_cmds; sg->frame_slot = slot;
    if (clear) { sg->has_clear = 1; memcpy(sg->clear, clear, sizeof sg->clear); }
}

YGFX_API int ygfx_begin_target(ygfx_gfx* g, ygfx_tex t, const float* clear) {
    ygfx__res* r;
    if (!g || !g->open) return YGFX_ERR_CLOSED;
    if (!g->in_frame || g->in_target || g->n_targets >= YGFX_MAX_PASSES) return YGFX_ERR_ORDER;
    if (t.id < 1 || t.id > YGFX_MAX_TEXTURES || !g->tex[t.id - 1].used || !(g->tex[t.id - 1].flags & YGFX__RES_TARGET))
        return YGFX_ERR_ARG;
    r = &g->tex[t.id - 1];
    g->n_targets++;
    g->in_target = 1;
    g->cur_target_tex = t.id;
    g->cur_w = r->w; g->cur_h = r->h; g->cur_top = 1;
    /* a 2x blur layer: what is in units comes out twice the px */
    if (r->ss > 1) { g->ppu_keep = g->ppu; g->ppu *= (float)r->ss; }
    ygfx__frame_block(g, g->n_targets, r->w, r->h, 1);
    ygfx__seg_next(g, r->bid, r->w, r->h, 1, g->n_targets, clear);
    return YGFX_OK;
}

YGFX_API int ygfx_end_target(ygfx_gfx* g) {
    if (!g || !g->open) return YGFX_ERR_CLOSED;
    if (!g->in_frame || !g->in_target) return YGFX_ERR_ORDER;
    g->in_target = 0;
    g->cur_target_tex = 0;
    g->cur_w = g->w; g->cur_h = g->h; g->cur_top = 0;
    if (g->ppu_keep > 0.0f) { g->ppu = g->ppu_keep; g->ppu_keep = 0.0f; }
    ygfx__seg_next(g, 0, g->w, g->h, 0, 0, NULL);
    return YGFX_OK;
}

/* --- the draw order (DRAW ORDER) ------------------------------------------------- */

/* A command's bounds in the pass's GL px, from its packed block: every
 * fragment it can write lies in its quad, center +- (half size + margin)
 * along its axes, effects, strokes and soft edges included; plus 1 px for
 * the pixel centers. Instanced draws, dots and glyph runs place their
 * pieces anywhere: the whole pass. */
static void ygfx__cmd_box(const ygfx_gfx* g, const ygfx__cmd* c, float box[4]) {
    const float* b = (const float*)(g->staging + YGFX__FRAME_BYTES + (size_t)c->block * YGFX__BLOCK);
    float hx, hy, ex, ey;
    if (c->inst) { box[0] = box[1] = -1e30f; box[2] = box[3] = 1e30f; return; }
    hx = b[4] + b[7]; hy = b[5] + b[7];
    ex = fabsf(b[2]) * hx + fabsf(b[3]) * hy + 1.0f;
    ey = fabsf(b[3]) * hx + fabsf(b[2]) * hy + 1.0f;
    box[0] = b[0] - ex; box[1] = b[1] - ey; box[2] = b[0] + ex; box[3] = b[1] + ey;
}

static int ygfx__box_meet(const float* a, const float* b) {
    return a[0] <= b[2] && b[0] <= a[2] && a[1] <= b[3] && b[1] <= a[3];
}

#ifndef YGFX__REORDER_LOOK
#define YGFX__REORDER_LOOK 64    /* batches a draw may move back past */
#endif

/* Within each pass, a draw joins an earlier batch of its kind when its
 * bounds meet none of the batches it moves past (then no pixel gets both,
 * so the order cannot change a value, whatever the blend). The commands
 * and their blocks are written in the new order to the second staging,
 * blocks of a batch next to each other, and the two are swapped. */
static void ygfx__reorder(ygfx_gfx* g) {
    ygfx__rbatch* B = g->ro_batch;
    int32_t* next = g->ro_next;
    int s, out = 0, blk = 0;
    memcpy(g->staging2, g->staging, YGFX__FRAME_BYTES);
    for (s = 0; s < g->n_segs; s++) {
        ygfx__seg* sg = &g->segs[s];
        int i, nb = 0, k;
        for (i = sg->first; i < sg->end; i++) {
            const ygfx__cmd* c = &g->cmds[i];
            float box[4];
            int j, at = -1;
            ygfx__cmd_box(g, c, box);
            next[i] = -1;
            for (j = nb - 1; j >= 0 && j >= nb - YGFX__REORDER_LOOK; j--) {
                if (B[j].count < g->max_batch && ygfx__same_batch(&g->cmds[B[j].head], c)) { at = j; break; }
                if (ygfx__box_meet(B[j].box, box)) break;
            }
            if (at < 0) {
                at = nb++;
                B[at].head = B[at].tail = i;
                B[at].count = 0;
                memcpy(B[at].box, box, sizeof box);
            } else {
                next[B[at].tail] = i;
                B[at].tail = i;
                if (box[0] < B[at].box[0]) B[at].box[0] = box[0];
                if (box[1] < B[at].box[1]) B[at].box[1] = box[1];
                if (box[2] > B[at].box[2]) B[at].box[2] = box[2];
                if (box[3] > B[at].box[3]) B[at].box[3] = box[3];
            }
            B[at].count++;
        }
        /* the segment's commands, batch by batch, in their new places */
        sg->first = out;
        for (k = 0; k < nb; k++)
            for (i = B[k].head; i >= 0; i = next[i]) {
                ygfx__cmd* d = &g->cmds2[out++];
                *d = g->cmds[i];
                memcpy(g->staging2 + YGFX__FRAME_BYTES + (size_t)blk * YGFX__BLOCK,
                       g->staging + YGFX__FRAME_BYTES + (size_t)d->block * YGFX__BLOCK, (size_t)d->nblk * YGFX__BLOCK);
                d->block = (uint32_t)blk;
                blk += (int)d->nblk;
            }
        sg->end = out;
    }
    {
        unsigned char* t = g->staging;
        ygfx__cmd* u = g->cmds;
        g->staging = g->staging2; g->staging2 = t;
        g->cmds = g->cmds2; g->cmds2 = u;
    }
}

/* A setup pass: the frame's bookkeeping with a frame of time 0 (frame
 * block 0 is written, as begin() does, so the target passes see the same
 * layout), and the GL context made current, as yscr_begin() would. */
YGFX_API int ygfx_begin_setup(ygfx_gfx* g) {
    yscr_frame f;
    int rc;
    if (!g || !g->open) return YGFX_ERR_CLOSED;
    if (g->in_frame) {
        ygfx__set_error(g->error, sizeof g->error, "ysp_gfx: begin_setup inside a frame or a setup pass");
        return YGFX_ERR_ORDER;
    }
    if (g->screen) yscr_bind(g->screen);
    memset(&f, 0, sizeof f);
    f.onset = g->t_open;   /* the shaders' time 0 */
    rc = ygfx_begin(g, &f);
    if (rc == YGFX_OK) g->in_setup = 1;
    return rc;
}

static int ygfx__end(ygfx_gfx* g, int setup);
YGFX_API int ygfx_end_setup(ygfx_gfx* g) {
    if (!g || !g->open) return YGFX_ERR_CLOSED;
    if (!g->in_frame || !g->in_setup) {
        ygfx__set_error(g->error, sizeof g->error, "ysp_gfx: end_setup without begin_setup");
        return YGFX_ERR_ORDER;
    }
    return ygfx__end(g, 1);
}

YGFX_API int ygfx_end(ygfx_gfx* g) {
    if (g && g->open && g->in_setup) {
        ygfx__set_error(g->error, sizeof g->error, "ysp_gfx: a setup pass ends with end_setup");
        return YGFX_ERR_ORDER;
    }
    return ygfx__end(g, 0);
}

/* setup 1: the target passes only; the scene's segments (empty: draws
 * there are refused), the output stage, the present, the frame count and
 * the clipped event belong to frames. */
static int ygfx__end(ygfx_gfx* g, int setup) {
    ygfx_bindings bd;
    uint32_t ubo;
    float clear[4];
    int i, j, k, rc, scene_cleared = 0;
    YRT_ZONE(z, "ygfx.end");
    if (!g || !g->open) { YRT_ZONE_END(z); return YGFX_ERR_CLOSED; }
    if (!g->in_frame) { YRT_ZONE_END(z); return YGFX_ERR_ORDER; }
    if (g->in_target) ygfx_end_target(g);
    g->in_setup = 0;
    g->in_frame = 0;
    g->segs[g->n_segs - 1].end = g->n_cmds;
    if (ygfx__sync(g) < 0) { YRT_ZONE_END(z); return YGFX_ERR_LOST; }
    if (!g->no_reorder && g->n_cmds > 1) ygfx__reorder(g);
    ubo = g->ubo[g->ubo_i];
    g->ubo_i = (g->ubo_i + 1) % YGFX__UBO_RING;
    /* One upload for the frame: the frame blocks and every stim block. */
    rc = g->be->buffer_update(g->bctx, ubo, 0, g->staging, YGFX__FRAME_BYTES + (size_t)g->n_blocks * YGFX__BLOCK);
    if (rc >= 0 && g->inst_used)   /* the elements, in the same ring slot */
        rc = g->be->buffer_update(g->bctx, g->inst_buf[(g->ubo_i + YGFX__UBO_RING - 1) % YGFX__UBO_RING], 0, g->inst_staging,
                                  (size_t)g->inst_used);
    if (rc < 0) { YRT_ZONE_END(z); return YGFX_ERR_GL; }
    clear[0] = g->bg[0]; clear[1] = g->bg[1]; clear[2] = g->bg[2]; clear[3] = 1.0f;
    memset(&bd, 0, sizeof bd);
    bd.ubo = ubo;
    bd.frame_size = YGFX__BLOCK;
    bd.stim_size = YGFX__STIM_RANGE;
    /* The segments in call order: the scene is cleared at its first, a
     * target only when its begin asked. An empty scene segment is skipped
     * unless the scene still needs its clear. */
    for (k = 0; k < g->n_segs; k++) {
        const ygfx__seg* sg = &g->segs[k];
        if (sg->target == 0) {
            if (setup) continue;
            if (sg->first == sg->end && scene_cleared) continue;
            g->be->pass_begin(g->bctx, g->scene, g->w, g->h, scene_cleared ? NULL : clear);
            scene_cleared = 1;
        } else {
            g->be->pass_begin(g->bctx, sg->target, sg->w, sg->h, sg->has_clear ? sg->clear : NULL);
        }
        bd.frame_off = (uint32_t)sg->frame_slot * YGFX__BLOCK;
        for (i = sg->first; i < sg->end; i = j) {
            const ygfx__cmd* c = &g->cmds[i];
            for (j = i + 1; j < sg->end && j - i < g->max_batch && ygfx__same_batch(c, &g->cmds[j]); j++) {}
            bd.pipeline = c->pipe;
            bd.stim_off = YGFX__FRAME_BYTES + c->block * YGFX__BLOCK;
            memcpy(bd.tex, c->tex, sizeof bd.tex);
            bd.instances = c->inst;
            bd.instances_off = c->inst_off;
            bd.blend = c->blend;
            g->be->apply(g->bctx, &bd);
            g->be->draw(g->bctx, 4, c->inst ? (int)c->count : j - i);
            g->draws++;
        }
        g->be->pass_end(g->bctx);
    }
    if (setup) {
        g->clipped_total += g->clipped;
        YRT_ZONE_END(z);
        return YGFX_OK;
    }
    {
        YRT_ZONE(zo, "ygfx.output");
        g->be->pass_begin(g->bctx, 0, g->w, g->h, NULL);
        memset(&bd, 0, sizeof bd);
        bd.pipeline = g->output_pipe;
        bd.ubo = ubo;
        bd.frame_size = YGFX__BLOCK;
        bd.tex[0] = g->scene;
        bd.tex[1] = g->lut;
        g->be->apply(g->bctx, &bd);
        g->be->draw(g->bctx, 3, 1);
        g->be->pass_end(g->bctx);
        YRT_ZONE_END(zo);
    }
    if (g->cap) {   /* before the present: the screen's patch and codes are not in it */
        YRT_ZONE(zc, "ygfx.capture");
        ygfx__cap_frame(g);
        YRT_ZONE_END(zc);
    }
    g->be->present(g->bctx);
    g->frames++;
    if (g->clipped) {
        g->clipped_total += g->clipped;
        ygfx__push(g, (uint16_t)YGFX_EV_CLIPPED, (uint64_t)g->frame.onset, g->clipped, NULL, 0, g->frame.index);
    }
    YRT_ZONE_END(z);
    return YGFX_OK;
}

/* PRIME. Zero-size quads: the stim block is zeros, so no fragment runs and
 * no shader loops over data, but each program is bound and drawn once. */
YGFX_API int ygfx_prime(ygfx_gfx* g) {
    ygfx_bindings bd;
    uint32_t ids[YGFX__N_BUILTIN * 2 + YGFX__N_VSPEC + 8 + 2 * YGFX_MAX_PIPELINES];
    unsigned char inst[sizeof ids / sizeof ids[0]];
    int n = 0, i, rc;
    float px[4];
    if (!g || !g->open) return YGFX_ERR_CLOSED;
    if (g->in_frame) {
        ygfx__set_error(g->error, sizeof g->error, "ysp_gfx: prime inside a frame or a setup pass");
        return YGFX_ERR_ORDER;
    }
    if (g->screen) yscr_bind(g->screen);
    if (ygfx__sync(g) < 0) return YGFX_ERR_LOST;
    for (i = 0; i < YGFX__N_BUILTIN; i++) if (g->builtin[i]) { inst[n] = 0; ids[n++] = g->builtin[i]; }
    for (i = 0; i < YGFX__N_VSPEC; i++) if (g->vspec[i]) { inst[n] = 0; ids[n++] = g->vspec[i]; }
    for (i = 0; i <= YGFX__N_BUILTIN; i++) if (g->inst_pipe[i]) { inst[n] = 1; ids[n++] = g->inst_pipe[i]; }
    if (g->video_pipe) { inst[n] = 0; ids[n++] = g->video_pipe; }
    if (g->text_pipe) { inst[n] = 0; ids[n++] = g->text_pipe; }
    if (g->text_rays_pipe) { inst[n] = 0; ids[n++] = g->text_rays_pipe; }
    if (g->blur_pipe) { inst[n] = 0; ids[n++] = g->blur_pipe; }
    for (i = 0; i < YGFX_MAX_PIPELINES; i++) {
        if (g->pipe[i].used && g->pipe[i].bid) { inst[n] = 0; ids[n++] = g->pipe[i].bid; }
        if (g->pipe_inst[i]) { inst[n] = 1; ids[n++] = g->pipe_inst[i]; }
    }
    memset(g->staging, 0, YGFX__FRAME_BYTES + YGFX__STIM_RANGE);
    rc = g->be->buffer_update(g->bctx, g->ubo[g->ubo_i], 0, g->staging, YGFX__FRAME_BYTES + YGFX__STIM_RANGE);
    if (rc < 0) return YGFX_ERR_GL;
    memset(&bd, 0, sizeof bd);
    bd.ubo = g->ubo[g->ubo_i];
    bd.frame_size = YGFX__BLOCK;
    bd.stim_off = YGFX__FRAME_BYTES;
    bd.stim_size = YGFX__STIM_RANGE;
    g->be->pass_begin(g->bctx, g->scene, g->w, g->h, NULL);
    for (i = 0; i < n; i++) {
        bd.pipeline = ids[i];
        bd.instances = inst[i] ? g->inst_buf[0] : 0;
        g->be->apply(g->bctx, &bd);
        g->be->draw(g->bctx, 4, 1);
    }
    g->be->pass_end(g->bctx);
    /* the output stage, into the back buffer: the next frame redraws every pixel */
    memset(&bd, 0, sizeof bd);
    bd.pipeline = g->output_pipe;
    bd.ubo = g->ubo[g->ubo_i];
    bd.frame_size = YGFX__BLOCK;
    bd.tex[0] = g->scene;
    bd.tex[1] = g->lut;
    g->be->pass_begin(g->bctx, 0, g->w, g->h, NULL);
    g->be->apply(g->bctx, &bd);
    g->be->draw(g->bctx, 3, 1);
    g->be->pass_end(g->bctx);
    /* a read waits for the GPU, so the cost lands here, not in a trial */
    if (g->be->read) g->be->read(g->bctx, g->scene, 0, 0, 1, 1, YGFX_READ_FLOAT, px);
    return n + 1;
}

/* --- resources ---------------------------------------------------------------- */

static int ygfx__slot(ygfx__res* pool, int n) {
    int i;
    for (i = 0; i < n; i++) if (!pool[i].used) return i;
    return -1;
}

static int ygfx__planar(int format) { return format == YGFX_NV12 || format == YGFX_I420; }
static int ygfx__nplanes(int format) { return format == YGFX_NV12 ? 2 : (format == YGFX_I420 ? 3 : 1); }

/* A plane's size and its texture format. */
static void ygfx__plane_dims(int format, int w, int h, int plane, int* pw, int* ph, ygfx_format* pf) {
    *pw = plane ? (w + 1) / 2 : w;
    *ph = plane ? (h + 1) / 2 : h;
    *pf = !ygfx__planar(format) ? (ygfx_format)format : (plane == 1 && format == YGFX_NV12 ? YGFX_RG8 : YGFX_R8);
}

/* An encoding is all or nothing, stated (rig_spec principles 4 and 5):
 * every field of a planar one; an RGB or gray texture's all zero (linear
 * values) or all set with matrix RGB. Primaries other than the display's
 * own need the calibration's chromaticities. A gray texture (R8, RG8) has
 * no chromaticity of its own: its light is the display's white, so its
 * primaries are DEVICE; a pack's 8-bit gray PNG arrives as sRGB codes, and
 * linearizing it to 8 bits in the tool would merge the dark codes. */
static int ygfx__enc_check(ygfx_gfx* g, int format, const ygfx_encoding* e) {
    const char* msg = NULL;
    int any = e->matrix || e->range || e->transfer || e->primaries || e->siting || e->chroma_nearest;
    int gray = format == YGFX_R8 || format == YGFX_RG8;
    if (!ygfx__planar(format) && !any) return YGFX_OK;
    if (!ygfx__planar(format) && !gray && format != YGFX_RGBA8 && format != YGFX_RGBA16F && format != YGFX_RGBA32F)
        msg = "an encoding is for NV12, I420, R8, RG8, RGBA8, RGBA16F and RGBA32F textures";
    else if (ygfx__planar(format) && !(e->matrix >= YGFX_MATRIX_BT601 && e->matrix <= YGFX_MATRIX_BT2020))
        msg = "a planar texture needs enc.matrix (BT601, BT709 or BT2020)";
    else if (!ygfx__planar(format) && e->matrix != YGFX_MATRIX_RGB)
        msg = "an RGB or gray texture's enc.matrix is YGFX_MATRIX_RGB";
    else if (!(e->range >= YGFX_RANGE_LIMITED && e->range <= YGFX_RANGE_FULL))
        msg = "enc.range is unspecified (LIMITED or FULL)";
    else if (!(e->transfer >= YGFX_TRC_DEVICE && e->transfer <= YGFX_TRC_GAMMA22))
        msg = "enc.transfer is unspecified (DEVICE, BT1886, SRGB, LINEAR or GAMMA22)";
    else if (!(e->primaries >= YGFX_PRIM_DEVICE && e->primaries <= YGFX_PRIM_BT2020))
        msg = "enc.primaries is unspecified: YGFX_PRIM_DEVICE passes the source's RGB through as the display's";
    else if (gray && e->primaries != YGFX_PRIM_DEVICE)
        msg = "a gray texture (R8, RG8) is the display's white at each level: enc.primaries is YGFX_PRIM_DEVICE";
    else if (ygfx__planar(format) && !(e->siting >= YGFX_SITING_LEFT && e->siting <= YGFX_SITING_TOP_LEFT))
        msg = "a 4:2:0 texture needs enc.siting (LEFT, CENTER or TOP_LEFT)";
    else if (!ygfx__planar(format) && e->siting > YGFX_SITING_NONE)
        msg = "an RGB or gray texture is not subsampled: enc.siting 0 or NONE";
    else if (e->chroma_nearest > 1)
        msg = "enc.chroma_nearest is 0 or 1";
    else if (e->primaries != YGFX_PRIM_DEVICE && !g->has_xyz)
        msg = "enc.primaries other than DEVICE convert through the calibration's chromaticities: open with a calibration "
              "that has them, or state YGFX_PRIM_DEVICE to pass the source's RGB through";
    if (!msg) return YGFX_OK;
    ygfx__set_error(g->error, sizeof g->error, "ysp_gfx: %s", msg);
    return YGFX_ERR_ARG;
}


/* The video program, made at the first encoded texture: an open without
 * video compiles nothing more. */
static int ygfx__video_pipe(ygfx_gfx* g) {
    char* body;
    size_t n;
    int rc;
    ygfx__prec r;
    if (g->video_pipe) return YGFX_OK;
    n = sizeof ygfx__body_image_video + sizeof ygfx__body_image_color;
    body = (char*)malloc(n);
    if (!body) return YGFX_ERR_FULL;
    snprintf(body, n, "%s%s", ygfx__body_image_video, ygfx__body_image_color);
    rc = ygfx__make_pipe(g, "#define YSP_VIDEO 1\n", body, YGFX_COLOR, 0, 0, &g->video_pipe, "image video", &r);
    free(body);
    g->be->reset(g->bctx);
    return rc < 0 ? rc : YGFX_OK;
}

/* The display's transfer: per gun, the linear value the CLUT maps to each
 * code, so the output stage writes a code that went through it back. */
static int ygfx__eotf_upload(ygfx_gfx* g, const float* lut, int n) {
    float tab[3 * 256];
    int c, k;
    if (!g->eotf) return YGFX_OK;
    for (c = 0; c < 3; c++) {
        const float* l = lut + (size_t)c * (size_t)n;
        int i = 0;
        for (k = 0; k < 256; k++) {
            double y = k / 255.0, L;
            if (y <= l[0]) L = 0.0;
            else if (y >= l[n - 1]) L = 1.0;
            else {
                while (i < n - 2 && l[i + 1] < y) i++;
                while (i > 0 && l[i] > y) i--;
                L = l[i + 1] > l[i] ? (i + (y - l[i]) / (l[i + 1] - l[i])) / (n - 1) : (double)i / (n - 1);
            }
            tab[c * 256 + k] = (float)L;
        }
    }
    return g->be->texture_update(g->bctx, g->eotf, 0, 0, 256, 3, tab, 256 * sizeof(float));
}

/* The planes' backend textures: made, or imported (src_kind 1 GL names, 2
 * a D3D11 texture). */
static int ygfx__make_planes(ygfx_gfx* g, ygfx__res* r, int w, int h, int format, int linear, const ygfx_planes* pl,
                               const ygfx_import_desc* imp) {
    int k, n = ygfx__nplanes(format), rc = YGFX_OK;
    unsigned char* black = NULL;
    uint32_t* ids[3];
    ids[0] = &r->bid; ids[1] = &r->plane_bid[0]; ids[2] = &r->plane_bid[1];
    for (k = 0; k < n && rc >= 0; k++) {
        ygfx_texture_src src;
        int pw, ph;
        ygfx_format pf;
        ygfx__plane_dims(format, w, h, k, &pw, &ph, &pf);
        memset(&src, 0, sizeof src);
        src.w = pw; src.h = ph; src.format = pf; src.linear = linear != 0;
        if (imp && imp->kind == YGFX_IMPORT_GL) src.gl_name = imp->gl_tex[k];
        else if (imp) { src.d3d11 = imp->d3d11_tex; src.plane = k; src.slice = (int32_t)imp->d3d11_slice; }
        else if (pl && pl->data[k]) { src.data = pl->data[k]; src.stride = pl->stride[k]; }
        else if (ygfx__planar(format)) {
            /* black in the texture's range: Y 16 or 0, chroma 128 */
            if (!black && !(black = (unsigned char*)malloc(((size_t)w + 1) * ((size_t)h + 1)))) { rc = YGFX_ERR_FULL; break; }
            memset(black, k ? 128 : (r->enc.range == YGFX_RANGE_LIMITED ? 16 : 0), (size_t)pw * (size_t)ph * (pf == YGFX_RG8 ? 2u : 1u));
            src.data = black;
        }
        if (imp && imp->kind == YGFX_IMPORT_GL && !src.gl_name) rc = YGFX_ERR_ARG;
        else rc = g->be->texture_make(g->bctx, &src, ids[k]);
    }
    free(black);
    if (rc < 0)
        for (k = 0; k < n; k++)
            if (*ids[k]) { g->be->texture_free(g->bctx, *ids[k]); *ids[k] = 0; }
    return rc;
}

YGFX_API ygfx_tex ygfx_texture(ygfx_gfx* g, const ygfx_texture_desc* d) {
    ygfx_tex t;
    ygfx__res* r;
    ygfx_planes one;
    int slot, rc;
    t.id = 0;
    if (!g || !g->open || !d) return t;
    if (g->in_frame) { ygfx__set_error(g->error, sizeof g->error, "ysp_gfx: texture made inside begin..end"); return t; }
    if (d->w <= 0 || d->h <= 0 || ygfx__texel_bytes(d->format) == 0) {
        ygfx__set_error(g->error, sizeof g->error, "ysp_gfx: texture size or format");
        return t;
    }
    if ((d->sdf == YGFX_SDF_MSDF || d->sdf == YGFX_SDF_MTSDF) &&
        (!(d->sdf_range > 0.0f) || (d->format != YGFX_RGBA8 && d->format != YGFX_RGBA16F && d->format != YGFX_RGBA32F))) {
        ygfx__set_error(g->error, sizeof g->error, "ysp_gfx: an MSDF or MTSDF texture is RGBA8, RGBA16F or RGBA32F with sdf_range > 0");
        return t;
    }
    if (d->sdf > YGFX_SDF_MTSDF) { ygfx__set_error(g->error, sizeof g->error, "ysp_gfx: sdf kind"); return t; }
    if (d->linear && (d->format == YGFX_R32F || d->format == YGFX_RGBA32F) && !g->caps.float_linear) {
        ygfx__set_error(g->error, sizeof g->error, "ysp_gfx: linear filtering of a 32-bit float texture needs GL_OES_texture_float_linear");
        return t;
    }
    if (ygfx__enc_check(g, d->format, &d->enc) < 0) return t;
    slot = ygfx__slot(g->tex, YGFX_MAX_TEXTURES);
    if (slot < 0) { ygfx__set_error(g->error, sizeof g->error, "ysp_gfx: %d textures in use", YGFX_MAX_TEXTURES); return t; }
    if (ygfx__sync(g) < 0) return t;
    if (d->enc.matrix && ygfx__video_pipe(g) < 0) return t;
    r = &g->tex[slot];
    memset(r, 0, sizeof *r);
    r->enc = d->enc;
    memset(&one, 0, sizeof one);
    one.data[0] = d->data; one.stride[0] = d->stride;
    rc = ygfx__make_planes(g, r, d->w, d->h, d->format, d->linear, ygfx__planar(d->format) ? d->planes : &one, NULL);
    if (rc < 0) {
        memset(r, 0, sizeof *r);
        ygfx__set_error(g->error, sizeof g->error, "ysp_gfx: texture %dx%d", d->w, d->h);
        return t;
    }
    r->used = 1;
    r->w = d->w; r->h = d->h; r->format = d->format;
    r->sdf = (int32_t)d->sdf; r->sdf_range = d->sdf_range;
    t.id = (uint32_t)slot + 1;
    return t;
}

YGFX_API ygfx_tex ygfx_texture_import(ygfx_gfx* g, const ygfx_import_desc* d) {
    ygfx_tex t;
    ygfx__res* r;
    uint32_t need;
    int slot, rc;
    t.id = 0;
    if (!g || !g->open || !d) return t;
    if (g->in_frame) { ygfx__set_error(g->error, sizeof g->error, "ysp_gfx: texture imported inside begin..end"); return t; }
    if (d->w <= 0 || d->h <= 0 || ygfx__texel_bytes(d->format) == 0 || d->format == YGFX_I420 || d->format == YGFX_R16UI ||
        (d->kind != YGFX_IMPORT_GL && d->kind != YGFX_IMPORT_D3D11) || (d->kind == YGFX_IMPORT_D3D11 && !d->d3d11_tex)) {
        ygfx__set_error(g->error, sizeof g->error, "ysp_gfx: import kind, size, format (not I420 or R16UI) or source");
        return t;
    }
    need = d->kind == YGFX_IMPORT_GL ? YGFX_FEAT_IMPORT_GL : (d->format == YGFX_NV12 ? YGFX_FEAT_IMPORT_NV12 : YGFX_FEAT_IMPORT_D3D11);
    if (!(g->features & need)) {
        ygfx__set_error(g->error, sizeof g->error, "ysp_gfx: this renderer cannot import %s (%s): EGL_ANGLE_image_d3d11_texture "
                          "and GL_OES_EGL_image are needed", d->kind == YGFX_IMPORT_GL ? "GL textures" : "D3D11 textures", g->caps.renderer);
        return t;
    }
    if (ygfx__enc_check(g, d->format, &d->enc) < 0) return t;
    slot = ygfx__slot(g->tex, YGFX_MAX_TEXTURES);
    if (slot < 0) { ygfx__set_error(g->error, sizeof g->error, "ysp_gfx: %d textures in use", YGFX_MAX_TEXTURES); return t; }
    if (ygfx__sync(g) < 0) return t;
    if (d->enc.matrix && ygfx__video_pipe(g) < 0) return t;
    r = &g->tex[slot];
    memset(r, 0, sizeof *r);
    r->enc = d->enc;
    rc = ygfx__make_planes(g, r, d->w, d->h, d->format, 0, NULL, d);
    if (rc < 0) {
        memset(r, 0, sizeof *r);
        ygfx__set_error(g->error, sizeof g->error, "ysp_gfx: the import failed (%s): a D3D11 texture must be on ANGLE's "
                          "device with D3D11_BIND_SHADER_RESOURCE, of the format and size given", rc == YGFX_ERR_ARG ? "no GL name" : "EGL");
        return t;
    }
    r->used = 1;
    r->w = d->w; r->h = d->h; r->format = d->format;
    t.id = (uint32_t)slot + 1;
    return t;
}

YGFX_API int ygfx_texture_rebind(ygfx_gfx* g, ygfx_tex t, ygfx_tex src) {
    ygfx__res *a, *b;
    if (!g || !g->open) return YGFX_ERR_CLOSED;
    if (g->in_frame) return YGFX_ERR_ORDER;
    if (t.id < 1 || t.id > YGFX_MAX_TEXTURES || src.id < 1 || src.id > YGFX_MAX_TEXTURES) return YGFX_ERR_ARG;
    a = &g->tex[t.id - 1];
    b = &g->tex[src.id - 1];
    if (!a->used || !b->used || a->w != b->w || a->h != b->h || a->format != b->format ||
        memcmp(&a->enc, &b->enc, sizeof a->enc) != 0 || ((a->flags ^ b->flags) & YGFX__RES_TARGET)) {
        ygfx__set_error(g->error, sizeof g->error, "ysp_gfx: rebind needs two textures of one format, size and encoding");
        return YGFX_ERR_ARG;
    }
    a->view = src.id == t.id ? 0 : (b->view ? b->view : (int32_t)src.id);
    return YGFX_OK;
}

static int ygfx__update(ygfx_gfx* g, ygfx_tex t, int plane, int x, int y, int w, int h, const void* data, size_t stride) {
    const ygfx__res* r;
    int pw, ph;
    ygfx_format pf;
    if (!g || !g->open) return YGFX_ERR_CLOSED;
    if (g->in_frame) return YGFX_ERR_ORDER;
    if (t.id < 1 || t.id > YGFX_MAX_TEXTURES || !g->tex[t.id - 1].used || !data) return YGFX_ERR_ARG;
    r = &g->tex[t.id - 1];
    if (plane < 0 || plane >= ygfx__nplanes(r->format)) return YGFX_ERR_ARG;
    ygfx__plane_dims(r->format, r->w, r->h, plane, &pw, &ph, &pf);
    if (x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > pw || y + h > ph) return YGFX_ERR_ARG;
    if (ygfx__sync(g) < 0) return YGFX_ERR_LOST;
    return g->be->texture_update(g->bctx, plane ? r->plane_bid[plane - 1] : r->bid, x, y, w, h, data, stride);
}

YGFX_API int ygfx_texture_update(ygfx_gfx* g, ygfx_tex t, int x, int y, int w, int h,
                                     const void* data, size_t stride) {
    int rc;
    YRT_ZONE(z, "ygfx.upload");
    if (g && g->open && t.id >= 1 && t.id <= YGFX_MAX_TEXTURES && ygfx__planar(g->tex[t.id - 1].format)) {
        ygfx__set_error(g->error, sizeof g->error, "ysp_gfx: a planar texture updates by ygfx_texture_update_planes()");
        YRT_ZONE_END(z);
        return YGFX_ERR_ARG;
    }
    rc = ygfx__update(g, t, 0, x, y, w, h, data, stride);
    YRT_ZONE_END(z);
    return rc;
}

YGFX_API int ygfx_texture_update_plane(ygfx_gfx* g, ygfx_tex t, int plane, int x, int y, int w, int h,
                                           const void* data, size_t stride) {
    int rc;
    YRT_ZONE(z, "ygfx.upload");
    rc = ygfx__update(g, t, plane, x, y, w, h, data, stride);
    YRT_ZONE_END(z);
    return rc;
}

YGFX_API int ygfx_texture_update_planes(ygfx_gfx* g, ygfx_tex t, const ygfx_planes* p) {
    int k, n, rc = YGFX_OK;
    YRT_ZONE(z, "ygfx.upload");
    if (!g || !g->open || !p || t.id < 1 || t.id > YGFX_MAX_TEXTURES || !g->tex[t.id - 1].used) {
        YRT_ZONE_END(z);
        return !g || !g->open ? YGFX_ERR_CLOSED : YGFX_ERR_ARG;
    }
    n = ygfx__nplanes(g->tex[t.id - 1].format);
    for (k = 0; k < n && rc >= 0; k++) {
        int pw, ph;
        ygfx_format pf;
        ygfx__plane_dims(g->tex[t.id - 1].format, g->tex[t.id - 1].w, g->tex[t.id - 1].h, k, &pw, &ph, &pf);
        rc = ygfx__update(g, t, k, 0, 0, pw, ph, p->data[k], p->stride[k]);
    }
    YRT_ZONE_END(z);
    return rc;
}

YGFX_API void ygfx_texture_free(ygfx_gfx* g, ygfx_tex t) {
    ygfx__res* r;
    int i;
    if (!g || !g->open || g->in_frame || t.id < 1 || t.id > YGFX_MAX_TEXTURES || !g->tex[t.id - 1].used) return;
    if (ygfx__sync(g) < 0) return;
    r = &g->tex[t.id - 1];
    g->be->texture_free(g->bctx, r->bid);
    for (i = 0; i < 2; i++) if (r->plane_bid[i]) g->be->texture_free(g->bctx, r->plane_bid[i]);
    memset(r, 0, sizeof *r);
    /* whatever showed it shows its own again */
    for (i = 0; i < YGFX_MAX_TEXTURES; i++) if (g->tex[i].view == (int32_t)t.id) g->tex[i].view = 0;
}

YGFX_API ygfx_buf ygfx_buffer(ygfx_gfx* g, size_t bytes) {
    ygfx_buf b;
    int slot;
    b.id = 0;
    if (!g || !g->open || bytes == 0 || bytes > 0x7FFFFFFFu) return b;
    if (g->in_frame) { ygfx__set_error(g->error, sizeof g->error, "ysp_gfx: buffer made inside begin..end"); return b; }
    slot = ygfx__slot(g->buf, YGFX_MAX_BUFFERS);
    if (slot < 0) { ygfx__set_error(g->error, sizeof g->error, "ysp_gfx: %d buffers in use", YGFX_MAX_BUFFERS); return b; }
    if (ygfx__sync(g) < 0) return b;
    if (g->be->buffer_make(g->bctx, YGFX_BUFFER_INSTANCE, bytes, &g->buf[slot].bid) < 0) {
        ygfx__set_error(g->error, sizeof g->error, "ysp_gfx: buffer of %lu bytes", (unsigned long)bytes);
        return b;
    }
    g->buf[slot].used = 1;
    g->buf[slot].w = (int32_t)bytes;
    b.id = (uint32_t)slot + 1;
    return b;
}

YGFX_API int ygfx_buffer_update(ygfx_gfx* g, ygfx_buf b, size_t off, const void* data, size_t n) {
    int rc;
    YRT_ZONE(z, "ygfx.upload");
    if (!g || !g->open) { YRT_ZONE_END(z); return YGFX_ERR_CLOSED; }
    if (g->in_frame) { YRT_ZONE_END(z); return YGFX_ERR_ORDER; }
    if (b.id < 1 || b.id > YGFX_MAX_BUFFERS || !g->buf[b.id - 1].used || !data || off + n > (size_t)g->buf[b.id - 1].w) {
        YRT_ZONE_END(z);
        return YGFX_ERR_ARG;
    }
    if (ygfx__sync(g) < 0) { YRT_ZONE_END(z); return YGFX_ERR_LOST; }
    rc = g->be->buffer_update(g->bctx, g->buf[b.id - 1].bid, off, data, n);
    YRT_ZONE_END(z);
    return rc;
}

YGFX_API void ygfx_buffer_free(ygfx_gfx* g, ygfx_buf b) {
    if (!g || !g->open || g->in_frame || b.id < 1 || b.id > YGFX_MAX_BUFFERS || !g->buf[b.id - 1].used) return;
    if (ygfx__sync(g) < 0) return;
    g->be->buffer_free(g->bctx, g->buf[b.id - 1].bid);
    memset(&g->buf[b.id - 1], 0, sizeof g->buf[0]);
}

YGFX_API ygfx_pipe ygfx_pipeline(ygfx_gfx* g, const ygfx_pipeline_desc* d) {
    ygfx_pipe p;
    int slot;
    p.id = 0;
    if (!g || !g->open || !d || !d->body) return p;
    if (g->in_frame) { ygfx__set_error(g->error, sizeof g->error, "ysp_gfx: pipeline made inside begin..end"); return p; }
    slot = ygfx__slot(g->pipe, YGFX_MAX_PIPELINES);
    if (slot < 0) { ygfx__set_error(g->error, sizeof g->error, "ysp_gfx: %d pipelines in use", YGFX_MAX_PIPELINES); return p; }
    if (ygfx__sync(g) < 0) return p;
    {
        ygfx__prec r;
        if (ygfx__make_pipe(g, "", d->body, (int)d->mode, 0, 0, &g->pipe[slot].bid, d->name ? d->name : "user", &r) < 0) return p;
    }
    g->be->reset(g->bctx);
    g->pipe[slot].used = 1;
    g->pipe[slot].flags = (int32_t)d->mode;
    {   /* kept for its instanced program (INSTANCES), made only when asked */
        size_t n = strlen(d->body) + 1;
        g->pipe_body[slot] = (char*)malloc(n);
        if (g->pipe_body[slot]) memcpy(g->pipe_body[slot], d->body, n);
    }
    g->error[0] = '\0';
    p.id = (uint32_t)slot + 1;
    return p;
}

YGFX_API void ygfx_pipeline_free(ygfx_gfx* g, ygfx_pipe p) {
    if (!g || !g->open || g->in_frame || p.id < 1 || p.id > YGFX_MAX_PIPELINES || !g->pipe[p.id - 1].used) return;
    if (ygfx__sync(g) < 0) return;
    g->be->pipeline_free(g->bctx, g->pipe[p.id - 1].bid);
    if (g->pipe_inst[p.id - 1]) g->be->pipeline_free(g->bctx, g->pipe_inst[p.id - 1]);
    free(g->pipe_body[p.id - 1]);
    g->pipe_body[p.id - 1] = NULL;
    g->pipe_inst[p.id - 1] = 0;
    memset(&g->pipe[p.id - 1], 0, sizeof g->pipe[0]);
}

YGFX_API int ygfx_set_lut(ygfx_gfx* g, const float* lut, int n) {
    int rc;
    YRT_ZONE(z, "ygfx.upload");
    if (!g || !g->open) { YRT_ZONE_END(z); return YGFX_ERR_CLOSED; }
    if (g->in_frame) { YRT_ZONE_END(z); return YGFX_ERR_ORDER; }
    if (!lut || n < 2 || n > YCOL_CAL_MAX_LUT) { YRT_ZONE_END(z); return YGFX_ERR_ARG; }
    if (ygfx__sync(g) < 0) { YRT_ZONE_END(z); return YGFX_ERR_LOST; }
    rc = ygfx__lut_upload(g, lut, n);
    if (rc >= 0) rc = ygfx__eotf_upload(g, lut, n);
    if (rc >= 0) {
        uint32_t u[2];
        g->lut_n = n;
        u[0] = (uint32_t)n;
        u[1] = ygfx__crc32(lut, (size_t)n * 3 * sizeof(float));
        ygfx__push(g, (uint16_t)YGFX_EV_LUT, 0, 0, u, 2, 0);
    }
    YRT_ZONE_END(z);
    return rc;
}

/* Reads bottom-up from GL, then turns the rows over in place. */
static void ygfx__flip_rows(unsigned char* p, int w, int h, size_t bpp) {
    size_t row = (size_t)w * bpp;
    int y;
    for (y = 0; y < h / 2; y++) {
        unsigned char* a = p + (size_t)y * row;
        unsigned char* b = p + (size_t)(h - 1 - y) * row;
        size_t k;
        for (k = 0; k < row; k++) { unsigned char t = a[k]; a[k] = b[k]; b[k] = t; }
    }
}

YGFX_API int ygfx_read_scene(ygfx_gfx* g, int x, int y, int w, int h, float* rgba) {
    int rc;
    if (!g || !g->open) return YGFX_ERR_CLOSED;
    if (g->in_frame) return YGFX_ERR_ORDER;
    if (!rgba || x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > g->w || y + h > g->h) return YGFX_ERR_ARG;
    if (ygfx__sync(g) < 0) return YGFX_ERR_LOST;
    rc = g->be->read(g->bctx, g->scene, x, g->h - y - h, w, h, YGFX_READ_FLOAT, rgba);
    g->be->reset(g->bctx);
    if (rc >= 0) ygfx__flip_rows((unsigned char*)rgba, w, h, 4 * sizeof(float));
    return rc;
}

YGFX_API int ygfx_read_output(ygfx_gfx* g, int x, int y, int w, int h, uint8_t* rgba) {
    int rc;
    if (!g || !g->open) return YGFX_ERR_CLOSED;
    if (g->in_frame) return YGFX_ERR_ORDER;
    if (!rgba || x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > g->w || y + h > g->h) return YGFX_ERR_ARG;
    if (ygfx__sync(g) < 0) return YGFX_ERR_LOST;
    rc = g->be->read(g->bctx, 0, x, g->origin_top ? y : g->h - y - h, w, h, YGFX_READ_UNORM, rgba);
    g->be->reset(g->bctx);
    if (rc >= 0 && !g->origin_top) ygfx__flip_rows(rgba, w, h, 4);
    return rc;
}

YGFX_API ygfx_tex ygfx_target(ygfx_gfx* g, const ygfx_target_desc* d) {
    ygfx_tex t;
    ygfx_texture_src src;
    ygfx_format fmt;
    int slot;
    t.id = 0;
    if (!g || !g->open || !d) return t;
    if (g->in_frame) { ygfx__set_error(g->error, sizeof g->error, "ysp_gfx: a target made inside begin..end"); return t; }
    fmt = d->format ? d->format : YGFX_RGBA16F;
    if (d->w <= 0 || d->h <= 0 || (fmt != YGFX_RGBA16F && fmt != YGFX_RGBA8 && fmt != YGFX_R16F && fmt != YGFX_RGBA32F)) {
        ygfx__set_error(g->error, sizeof g->error, "ysp_gfx: a target is RGBA16F, RGBA8, R16F or RGBA32F, with a size");
        return t;
    }
    /* draws into a target blend; blending into 32-bit float needs EXT_float_blend */
    if (fmt == YGFX_RGBA32F && !g->caps.float_blend) {
        ygfx__set_error(g->error, sizeof g->error, "ysp_gfx: an RGBA32F target needs EXT_float_blend, which this renderer lacks");
        return t;
    }
    slot = ygfx__slot(g->tex, YGFX_MAX_TEXTURES);
    if (slot < 0) { ygfx__set_error(g->error, sizeof g->error, "ysp_gfx: %d textures in use", YGFX_MAX_TEXTURES); return t; }
    if (ygfx__sync(g) < 0) return t;
    memset(&src, 0, sizeof src);
    src.w = d->w; src.h = d->h; src.format = fmt; src.linear = d->linear; src.target = true;
    if (g->be->texture_make(g->bctx, &src, &g->tex[slot].bid) < 0) {
        ygfx__set_error(g->error, sizeof g->error, "ysp_gfx: target %dx%d", d->w, d->h);
        return t;
    }
    {   /* a new target starts transparent black */
        static const float zero[4] = { 0, 0, 0, 0 };
        g->be->pass_begin(g->bctx, g->tex[slot].bid, d->w, d->h, zero);
        g->be->pass_end(g->bctx);
        g->be->reset(g->bctx);
    }
    g->tex[slot].used = 1;
    g->tex[slot].w = d->w; g->tex[slot].h = d->h; g->tex[slot].format = fmt;
    g->tex[slot].flags = YGFX__RES_TARGET;
    t.id = (uint32_t)slot + 1;
    return t;
}

/* A target's rows are top first already: no turn. */
YGFX_API int ygfx_read_target(ygfx_gfx* g, ygfx_tex t, int x, int y, int w, int h, float* rgba) {
    const ygfx__res* r;
    int rc;
    if (!g || !g->open) return YGFX_ERR_CLOSED;
    if (g->in_frame) return YGFX_ERR_ORDER;
    if (t.id < 1 || t.id > YGFX_MAX_TEXTURES || !g->tex[t.id - 1].used || !(g->tex[t.id - 1].flags & YGFX__RES_TARGET))
        return YGFX_ERR_ARG;
    r = &g->tex[t.id - 1];
    if (!rgba || x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > r->w || y + h > r->h) return YGFX_ERR_ARG;
    if (ygfx__sync(g) < 0) return YGFX_ERR_LOST;
    if (r->format == YGFX_RGBA8) {
        /* GL ES reads a fixed-point target as bytes only: read them into the
         * front of the caller's floats, then widen from the back. */
        unsigned char* b8 = (unsigned char*)rgba;
        size_t n = (size_t)w * (size_t)h * 4, i;
        rc = g->be->read(g->bctx, r->bid, x, y, w, h, YGFX_READ_UNORM, b8);
        if (rc >= 0) for (i = n; i-- > 0;) rgba[i] = (float)b8[i] * (1.0f / 255.0f);
    } else {
        rc = g->be->read(g->bctx, r->bid, x, y, w, h, YGFX_READ_FLOAT, rgba);
    }
    g->be->reset(g->bctx);
    return rc;
}

/* --- image files (CAPTURE) ---------------------------------------------------- */

/* PNG's CRC-32, four bytes a step (slicing by 4): the tables are made per
 * file on the stack, about 10 us, so nothing is shared between threads. */
static void ygfx__crc_tables(uint32_t t[4][256]) {
    uint32_t i, c;
    int k;
    for (i = 0; i < 256; i++) {
        c = i;
        for (k = 0; k < 8; k++) c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        t[0][i] = c;
    }
    for (i = 0; i < 256; i++) {
        c = t[0][i];
        for (k = 1; k < 4; k++) { c = t[0][c & 0xFFu] ^ (c >> 8); t[k][i] = c; }
    }
}

static uint32_t ygfx__crc_run(const uint32_t t[4][256], uint32_t c, const unsigned char* p, size_t n) {
    while (n >= 4) {
        c ^= (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
        c = t[3][c & 0xFFu] ^ t[2][(c >> 8) & 0xFFu] ^ t[1][(c >> 16) & 0xFFu] ^ t[0][c >> 24];
        p += 4;
        n -= 4;
    }
    while (n--) c = t[0][(c ^ *p++) & 0xFFu] ^ (c >> 8);
    return c;
}

typedef struct ygfx__isink {
    FILE*    f;
    int      png, ok;
    uint32_t crc, a1, a2;          /* the chunk's CRC; zlib's Adler-32 sums     */
    size_t   block_left, raw_left; /* PNG: bytes left in the stored block, and
                                    * in the image                              */
    uint32_t t[4][256];
} ygfx__isink;

static void ygfx__img_put(ygfx__isink* s, const void* p, size_t n) {
    if (s->ok && fwrite(p, 1, n, s->f) != n) s->ok = 0;
}

static void ygfx__img_out(ygfx__isink* s, const void* p, size_t n) {
    if (s->png) s->crc = ygfx__crc_run((const uint32_t(*)[256])s->t, s->crc, (const unsigned char*)p, n);
    ygfx__img_put(s, p, n);
}

static void ygfx__img_be32(unsigned char* b, uint32_t v) {
    b[0] = (unsigned char)(v >> 24); b[1] = (unsigned char)(v >> 16);
    b[2] = (unsigned char)(v >> 8);  b[3] = (unsigned char)v;
}

/* A chunk's length goes outside its CRC; its type and data inside. */
static void ygfx__img_chunk(ygfx__isink* s, uint32_t len, const char* type) {
    unsigned char b[4];
    ygfx__img_be32(b, len);
    ygfx__img_put(s, b, 4);
    s->crc = 0xFFFFFFFFu;
    ygfx__img_out(s, type, 4);
}

static void ygfx__img_chunk_end(ygfx__isink* s) {
    unsigned char b[4];
    ygfx__img_be32(b, s->crc ^ 0xFFFFFFFFu);
    ygfx__img_put(s, b, 4);
}

/* The image's bytes. A PNG gets them as zlib's stored blocks (at most 65535
 * bytes each, a 5-byte head) under one Adler-32: no compression, so the
 * writer's time is the disk's and two checksums. */
static void ygfx__img_raw(ygfx__isink* s, const unsigned char* p, size_t n) {
    uint32_t a = s->a1, b = s->a2;
    size_t m = n;
    const unsigned char* q = p;
    if (!s->png) { ygfx__img_put(s, p, n); return; }
    while (m) {   /* 5552 bytes is the most the sums take before a modulo */
        size_t k = m < 5552 ? m : 5552;
        m -= k;
        while (k--) { a += *q++; b += a; }
        a %= 65521u;
        b %= 65521u;
    }
    s->a1 = a; s->a2 = b;
    while (n) {
        size_t k;
        if (!s->block_left) {
            size_t len = s->raw_left < 65535 ? s->raw_left : 65535;
            unsigned char hd[5];
            hd[0] = (unsigned char)(len == s->raw_left);   /* BFINAL, BTYPE 00 */
            hd[1] = (unsigned char)(len & 0xFFu);
            hd[2] = (unsigned char)(len >> 8);
            hd[3] = (unsigned char)(~len & 0xFFu);
            hd[4] = (unsigned char)((~len >> 8) & 0xFFu);
            ygfx__img_out(s, hd, 5);
            s->block_left = len;
        }
        k = n < s->block_left ? n : s->block_left;
        ygfx__img_out(s, p, k);
        p += k;
        n -= k;
        s->block_left -= k;
        s->raw_left -= k;
    }
}

YGFX_API int ygfx_image_write(const char* path, int format, const uint8_t* rgba, int w, int h,
                              ptrdiff_t stride, const char* comment) {
    static const unsigned char sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    char iobuf[32768];             /* stdio's buffer: no allocation in fopen's */
    unsigned char row[3 * 1024];
    ygfx__isink s;
    FILE* f;
    int y, x0, i;
    if (!path || !path[0] || !rgba || w <= 0 || h <= 0 || (format != YGFX_IMAGE_PPM && format != YGFX_IMAGE_PNG))
        return YGFX_ERR_ARG;
    if (stride == 0) stride = (ptrdiff_t)w * 4;
    /* PNG keeps one IDAT chunk, whose length is 31 bits */
    if (format == YGFX_IMAGE_PNG && (double)h * (1.0 + 3.0 * w) * (1.0 + 5.0 / 65535.0) + 64.0 > 2147483647.0)
        return YGFX_ERR_ARG;
#if defined(_WIN32)
    f = ygfx__wopen(path, L"wb");
#else
    f = fopen(path, "wb");
#endif
    if (!f) return YGFX_ERR_IO;
    setvbuf(f, iobuf, _IOFBF, sizeof iobuf);
    memset(&s, 0, offsetof(ygfx__isink, t));
    s.f = f;
    s.ok = 1;
    s.png = format == YGFX_IMAGE_PNG;
    if (!s.png) {
        char head[64];
        int n;
        ygfx__img_put(&s, "P6\n", 3);
        if (comment && comment[0]) {
            const char* c = comment;
            ygfx__img_put(&s, "# ", 2);
            for (; *c; c++) {   /* each line of the comment is a comment line */
                if (*c == '\n') { ygfx__img_put(&s, "\n# ", 3); continue; }
                ygfx__img_put(&s, c, 1);
            }
            ygfx__img_put(&s, "\n", 1);
        }
        n = snprintf(head, sizeof head, "%d %d\n255\n", w, h);
        ygfx__img_put(&s, head, (size_t)n);
    } else {
        unsigned char b[13];
        size_t raw = (size_t)h * (1 + 3 * (size_t)w), blocks = (raw + 65534) / 65535;
        ygfx__crc_tables(s.t);
        ygfx__img_put(&s, sig, 8);
        ygfx__img_chunk(&s, 13, "IHDR");
        ygfx__img_be32(b, (uint32_t)w);
        ygfx__img_be32(b + 4, (uint32_t)h);
        b[8] = 8; b[9] = 2; b[10] = 0; b[11] = 0; b[12] = 0;   /* 8 bits, RGB, no interlace */
        ygfx__img_out(&s, b, 13);
        ygfx__img_chunk_end(&s);
        if (comment && comment[0]) {
            size_t n = strlen(comment);
            ygfx__img_chunk(&s, (uint32_t)(8 + n), "tEXt");
            ygfx__img_out(&s, "Comment", 8);   /* with its NUL separator */
            ygfx__img_out(&s, comment, n);
            ygfx__img_chunk_end(&s);
        }
        ygfx__img_chunk(&s, (uint32_t)(2 + raw + 5 * blocks + 4), "IDAT");
        b[0] = 0x78; b[1] = 0x01;   /* deflate, 32 KB window, no dictionary */
        ygfx__img_out(&s, b, 2);
        s.a1 = 1; s.a2 = 0;
        s.raw_left = raw;
    }
    for (y = 0; y < h && s.ok; y++) {
        const uint8_t* src = rgba + (ptrdiff_t)y * stride;
        if (s.png) { unsigned char none = 0; ygfx__img_raw(&s, &none, 1); }   /* filter type 0 */
        for (x0 = 0; x0 < w; x0 += 1024) {
            int n = w - x0 < 1024 ? w - x0 : 1024;
            const uint8_t* p = src + 4 * (size_t)x0;
            for (i = 0; i < n; i++) { row[3 * i] = p[4 * i]; row[3 * i + 1] = p[4 * i + 1]; row[3 * i + 2] = p[4 * i + 2]; }
            ygfx__img_raw(&s, row, 3 * (size_t)n);
        }
    }
    if (s.png) {
        unsigned char b[4];
        ygfx__img_be32(b, (s.a2 << 16) | s.a1);
        ygfx__img_out(&s, b, 4);
        ygfx__img_chunk_end(&s);
        ygfx__img_chunk(&s, 0, "IEND");
        ygfx__img_chunk_end(&s);
    }
    if (fclose(f) != 0) s.ok = 0;
    if (!s.ok) { remove(path); return YGFX_ERR_IO; }
    return YGFX_OK;
}

/* --- capture (CAPTURE) ---------------------------------------------------------- */

#define YGFX__CS_FREE    0
#define YGFX__CS_READING 1         /* the copy is on the GPU                    */
#define YGFX__CS_MAPPED  2         /* readable; waits for its flip record       */
#define YGFX__CS_QUEUED  3         /* the writer has it                         */
/* Frames a copy waits for its flip record. A record comes with the begin()
 * after its flip, 2 to 3 frames after the copy at a depth of 2. */
#define YGFX__CAP_RECORD_FRAMES 30

typedef struct ygfx__cslot {
    int32_t        state, has_record;
    int32_t        no_wait;            /* a frame not from yscr_begin(): no record */
    int64_t        index, predicted;
    uint64_t       frame_no;           /* g->frames at the copy: its age          */
    const uint8_t* data;
    uint32_t       seq;                /* the writer's message                    */
    int32_t        result;             /* the writer's: YGFX_OK or < 0            */
    yscr_record    record;
} ygfx__cslot;

typedef struct ygfx__cap {
    int32_t         x, y, w, h, gl_y, top_first, format, n, manual, every, mark, has_screen;
    volatile int    abandon;           /* the context is gone: the writer skips   */
    char            path[512];         /* the prefix; "" = no files               */
    ygfx_capture_fn on_frame;
    void*           ctx;
    ygfx_capture_stats st;
    ygfx__cslot     slot[YGFX_CAPTURE_MAX_SLOTS];
#ifndef YRT_NO_THREADS
    int32_t         ring[YGFX_CAPTURE_MAX_SLOTS];
    yrt_pump        pump;
#endif
} ygfx__cap;

/* On the writer (or the pump's caller): the file, then on_frame. */
static int ygfx__cap_write(ygfx__cap* c, int i) {
    const ygfx__cslot* s = &c->slot[i];
    ygfx_capture_frame fr;
    char path[560], note[400];
    memset(&fr, 0, sizeof fr);
    fr.index = s->index;
    fr.predicted = s->predicted;
    fr.has_record = s->has_record;
    fr.x = c->x; fr.y = c->y; fr.w = c->w; fr.h = c->h;
    /* GL's rows are bottom first unless the swap path turned them */
    fr.stride = (ptrdiff_t)c->w * 4;
    fr.rgba = s->data;
    if (!c->top_first) { fr.rgba = s->data + (size_t)(c->h - 1) * (size_t)c->w * 4; fr.stride = -fr.stride; }
    if (s->has_record) fr.record = s->record;
    fr.file_rc = YGFX_OK;
    if (c->abandon) return YGFX_ERR_LOST;
    if (c->path[0]) {
        snprintf(path, sizeof path, "%s%08lld.%s", c->path, (long long)s->index,
                 c->format == YGFX_IMAGE_PNG ? "png" : "ppm");
        if (s->has_record)
            snprintf(note, sizeof note,
                     "ysp_gfx capture: frame %lld, predicted %lld ns, rect %d %d %d %d; "
                     "record: onset %lld ns, planned %lld, target %lld, residual %lld, dropped %u, flags 0x%04x, path %u, tier %u",
                     (long long)s->index, (long long)s->predicted, c->x, c->y, c->w, c->h,
                     (long long)s->record.onset, (long long)s->record.planned, (long long)s->record.target,
                     (long long)s->record.residual, (unsigned)s->record.dropped, (unsigned)s->record.flags,
                     (unsigned)s->record.path, (unsigned)s->record.tier);
        else
            snprintf(note, sizeof note, "ysp_gfx capture: frame %lld, predicted %lld ns, rect %d %d %d %d; record: none",
                     (long long)s->index, (long long)s->predicted, c->x, c->y, c->w, c->h);
        fr.file_rc = ygfx_image_write(path, c->format, fr.rgba, c->w, c->h, fr.stride, note);
        fr.path = path;
    }
    if (c->on_frame) c->on_frame(c->ctx, &fr);
    return fr.file_rc;
}

#ifndef YRT_NO_THREADS
static void ygfx__cap_msg(void* ctx, const void* msg, uint32_t seq) {
    ygfx__cap* c = (ygfx__cap*)ctx;
    int32_t i;
    (void)seq;
    memcpy(&i, msg, sizeof i);
    c->slot[i].result = ygfx__cap_write(c, (int)i);
}
#endif

static void ygfx__cap_retire(ygfx_gfx* g, ygfx__cap* c, int i, int rc, int gl_ok) {
    ygfx__cslot* s = &c->slot[i];
    if (gl_ok) g->be->capture_unmap(g->bctx, i);
    if (rc >= 0) {
        c->st.written++;
        if (!s->has_record) c->st.no_record++;
    } else {
        c->st.failed++;
    }
    s->state = YGFX__CS_FREE;
    s->data = NULL;
    c->st.busy--;
}

/* Frees what the writer has finished. */
static void ygfx__cap_reclaim(ygfx_gfx* g, ygfx__cap* c, int gl_ok) {
#ifndef YRT_NO_THREADS
    uint32_t done;
    int i;
    if (c->manual) return;
    done = yrt_pump_done_seq(&c->pump);
    for (i = 0; i < c->n; i++) {
        ygfx__cslot* s = &c->slot[i];
        if (s->state == YGFX__CS_QUEUED && done >= s->seq) ygfx__cap_retire(g, c, i, s->result, gl_ok);
    }
#else
    (void)g; (void)c; (void)gl_ok;
#endif
}

/* Copies the GPU has finished become readable, oldest first, at most max of
 * them (<= 0: all); wait_ns > 0 waits for each. A map costs the frame
 * thread 2.6 ms mean at 1920 x 1200 on ANGLE (docs/gfx.md, "v0.12"), so a
 * frame maps one: after a stall the backlog spreads over the next frames.
 * The GPU finishes copies in order, so the first one not done ends it. */
static void ygfx__cap_poll(ygfx_gfx* g, ygfx__cap* c, int64_t wait_ns, int max) {
    int n = 0;
    while (max <= 0 || n < max) {
        int i, best = -1, rc;
        const void* p = NULL;
        for (i = 0; i < c->n; i++)
            if (c->slot[i].state == YGFX__CS_READING && (best < 0 || c->slot[i].index < c->slot[best].index)) best = i;
        if (best < 0) break;
        rc = g->be->capture_poll(g->bctx, best, wait_ns, &p);
        if (rc == 0 && wait_ns <= 0) break;
        if (rc == 1) {
            c->slot[best].state = YGFX__CS_MAPPED;
            c->slot[best].data = (const uint8_t*)p;
            n++;
        } else {   /* failed, or not done in the wait */
            c->slot[best].state = YGFX__CS_FREE;
            c->st.failed++;
            c->st.busy--;
        }
    }
}

/* The oldest readable copy whose record is in (or will not come). */
static int ygfx__cap_next(const ygfx_gfx* g, const ygfx__cap* c, int final) {
    int i, best = -1;
    for (i = 0; i < c->n; i++) {
        const ygfx__cslot* s = &c->slot[i];
        if (s->state != YGFX__CS_MAPPED) continue;
        if (!(s->has_record || s->no_wait || final || g->frames - s->frame_no > YGFX__CAP_RECORD_FRAMES)) continue;
        if (best < 0 || s->index < c->slot[best].index) best = i;
    }
    return best;
}

/* Threads: to the writer, in frame order. Manual: written here, up to max. */
static int ygfx__cap_hand(ygfx_gfx* g, ygfx__cap* c, int final, int max) {
    int i, n = 0;
    while ((i = ygfx__cap_next(g, c, final)) >= 0) {
        if (c->manual) {
            if (max <= 0 && !final) break;
            ygfx__cap_retire(g, c, i, ygfx__cap_write(c, i), 1);
            n++;
            if (max > 0 && n >= max) break;
        } else {
#ifndef YRT_NO_THREADS
            int32_t m = (int32_t)i;
            int seq = yrt_pump_submit(&c->pump, &m);
            if (seq < 0) break;   /* the ring holds every slot: not reached */
            c->slot[i].seq = (uint32_t)seq;
            c->slot[i].state = YGFX__CS_QUEUED;
#endif
        }
    }
    return n;
}

/* begin(): the flip records that came with this frame. */
static void ygfx__cap_records(ygfx__cap* c, const yscr_frame* f) {
    int k, i;
    if (!f->done) return;
    for (k = 0; k < f->n_done; k++) {
        const yscr_record* r = &f->done[k];
        for (i = 0; i < c->n; i++) {
            ygfx__cslot* s = &c->slot[i];
            if ((s->state == YGFX__CS_READING || s->state == YGFX__CS_MAPPED) && !s->has_record && s->index == r->index) {
                s->record = *r;
                s->has_record = 1;
                break;
            }
        }
    }
}

/* end(), after the output stage and before the present: collect, hand on,
 * and copy this frame when it is chosen. No wait, no allocation. */
static void ygfx__cap_frame(ygfx_gfx* g) {
    ygfx__cap* c = g->cap;
    int chosen = c->mark || (c->every > 0 && g->frame.index % c->every == 0);
    int i, rc;
    c->mark = 0;
    ygfx__cap_reclaim(g, c, 1);
    if (!c->manual) ygfx__cap_poll(g, c, 0, 1);   /* manual: the maps wait for the pump */
    ygfx__cap_hand(g, c, 0, 0);
    if (!chosen) return;
    c->st.chosen++;
    for (i = 0; i < c->n && c->slot[i].state != YGFX__CS_FREE; i++) {}
    if (i == c->n) {
        c->st.dropped++;
        ygfx__push(g, (uint16_t)YGFX_EV_CAPTURE_DROP, (uint64_t)g->frame.onset, (uint32_t)c->st.dropped, NULL, 0, g->frame.index);
        return;
    }
    rc = g->be->capture_read(g->bctx, i, c->x, c->gl_y, c->w, c->h);
    if (rc < 0) { c->st.failed++; return; }
    c->slot[i].state = YGFX__CS_READING;
    c->slot[i].has_record = 0;
    /* yscr_begin() always sets done; a frame filled by hand gets no record */
    c->slot[i].no_wait = !c->has_screen || !g->frame.done;
    c->slot[i].index = g->frame.index;
    c->slot[i].predicted = g->frame.onset;
    c->slot[i].frame_no = g->frames;
    c->st.read++;
    if (++c->st.busy > c->st.busy_max) c->st.busy_max = c->st.busy;
}

static void ygfx__cap_stats(const ygfx__cap* c, ygfx_capture_stats* out) {
    *out = c->st;
    out->slots = c->n;
    out->manual = c->manual;
}

/* Every copy waited for and written, the writer stopped, the buffers freed.
 * gl_ok 0: the context is gone, so nothing is read or unmapped. */
static void ygfx__cap_stop(ygfx_gfx* g, int gl_ok, ygfx_capture_stats* out) {
    ygfx__cap* c = g->cap;
    int i;
    if (!c) return;
    if (gl_ok) {
        if (g->screen) yscr_bind(g->screen);
        ygfx__cap_reclaim(g, c, 1);
        ygfx__cap_poll(g, c, INT64_C(1000000000), 0);
        ygfx__cap_hand(g, c, 1, 0);
    } else {
        c->abandon = 1;
    }
#ifndef YRT_NO_THREADS
    if (!c->manual) {
        yrt_pump_stop(&c->pump);   /* drains: every queued frame is written */
        ygfx__cap_reclaim(g, c, gl_ok);
    }
#endif
    for (i = 0; i < c->n; i++) if (c->slot[i].state != YGFX__CS_FREE) ygfx__cap_retire(g, c, i, YGFX_ERR_LOST, gl_ok);
    if (gl_ok) g->be->capture_free(g->bctx);
    if (out) ygfx__cap_stats(c, out);
    free(c);
    g->cap = NULL;
}

YGFX_API int ygfx_capture_start(ygfx_gfx* g, const ygfx_capture_desc* d) {
    ygfx__cap* c;
    int x, y, w, h, n, rc;
    if (!g || !g->open) return YGFX_ERR_CLOSED;
    if (g->in_frame || g->cap) {
        ygfx__set_error(g->error, sizeof g->error, "ysp_gfx: capture starts outside a frame, once");
        return YGFX_ERR_ORDER;
    }
    if (!d) return YGFX_ERR_ARG;
    x = d->x; y = d->y; w = d->w; h = d->h;
    if (w <= 0 || h <= 0) { x = 0; y = 0; w = g->w; h = g->h; }
    n = d->slots ? d->slots : 6;
    if (d->every < 0 || x < 0 || y < 0 || x + w > g->w || y + h > g->h || n < 1 || n > YGFX_CAPTURE_MAX_SLOTS ||
        (d->format != 0 && d->format != YGFX_IMAGE_PPM && d->format != YGFX_IMAGE_PNG) ||
        (d->path && strlen(d->path) >= sizeof(((ygfx__cap*)0)->path))) {
        ygfx__set_error(g->error, sizeof g->error,
                          "ysp_gfx: capture: every >= 0, a rectangle inside the output, 1 to %d slots, a format, a path under 512 bytes",
                          YGFX_CAPTURE_MAX_SLOTS);
        return YGFX_ERR_ARG;
    }
    if (g->be->version < 4 || !g->be->capture_make) {
        ygfx__set_error(g->error, sizeof g->error, "ysp_gfx: capture: the backend has no reads that do not wait");
        return YGFX_ERR_NOT_IMPLEMENTED;
    }
    c = (ygfx__cap*)calloc(1, sizeof *c);
    if (!c) return YGFX_ERR_FULL;
    c->x = x; c->y = y; c->w = w; c->h = h;
    c->top_first = g->origin_top;
    c->gl_y = g->origin_top ? y : g->h - y - h;
    c->format = d->format ? d->format : YGFX_IMAGE_PPM;
    c->n = n;
    c->every = d->every;
    c->has_screen = g->screen != NULL;
    c->on_frame = d->on_frame;
    c->ctx = d->ctx;
    c->manual = d->manual;
#if defined(YRT_NO_THREADS)
    c->manual = 1;
#endif
    if (d->path) {
        size_t k = strlen(d->path);
        memcpy(c->path, d->path, k + 1);
        while (k > 0 && c->path[k - 1] != '/' && c->path[k - 1] != '\\') k--;
        if (k > 1) {   /* the folder of the prefix */
            char dir[512];
            memcpy(dir, c->path, k - 1);
            dir[k - 1] = '\0';
            ygfx__fc_mkdirs(dir);
        }
    }
    if (g->screen) yscr_bind(g->screen);
    if (ygfx__sync(g) < 0) { free(c); return YGFX_ERR_LOST; }
    rc = g->be->capture_make(g->bctx, n, (size_t)w * (size_t)h * 4);
    if (rc < 0) {
        free(c);
        ygfx__set_error(g->error, sizeof g->error, "ysp_gfx: capture: %d buffers of %dx%d: %s", n, w, h, ygfx_strerror(rc));
        return rc;
    }
    g->be->reset(g->bctx);
#ifndef YRT_NO_THREADS
    if (!c->manual) {
        yrt_pump_desc pd;
        memset(&pd, 0, sizeof pd);
        pd.msg_size = sizeof(int32_t);
        pd.capacity = (uint32_t)n;
        pd.ring = c->ring;
        pd.on_msg = ygfx__cap_msg;
        pd.ctx = c;
        pd.below_normal = true;   /* it must never take a core from the frame loop */
        if (!yrt_pump_start(&c->pump, &pd)) {
            ygfx__set_error(g->error, sizeof g->error, "ysp_gfx: capture: the writer did not start: %s (desc.manual works without it)",
                              yrt_pump_error(&c->pump));
            g->be->capture_free(g->bctx);
            free(c);
            return YGFX_ERR_REFUSED;
        }
    }
#endif
    g->cap = c;
    return YGFX_OK;
}

YGFX_API int ygfx_capture_mark(ygfx_gfx* g) {
    if (!g || !g->open) return YGFX_ERR_CLOSED;
    if (!g->cap || !g->in_frame || g->in_setup) return YGFX_ERR_ORDER;
    g->cap->mark = 1;
    return YGFX_OK;
}

YGFX_API int ygfx_capture_pump(ygfx_gfx* g, int max) {
    ygfx__cap* c;
    if (!g || !g->open) return YGFX_ERR_CLOSED;
    c = g->cap;
    if (!c || g->in_frame) return YGFX_ERR_ORDER;
    if (g->screen) yscr_bind(g->screen);
    if (ygfx__sync(g) < 0) return YGFX_ERR_LOST;
    ygfx__cap_reclaim(g, c, 1);
    ygfx__cap_poll(g, c, 0, 0);
    return ygfx__cap_hand(g, c, 0, max <= 0 ? c->n : max);
}

YGFX_API int ygfx_capture_stop(ygfx_gfx* g, ygfx_capture_stats* out) {
    int gl_ok;
    if (out) memset(out, 0, sizeof *out);
    if (!g) return YGFX_ERR_ARG;
    if (!g->cap) return YGFX_ERR_ORDER;
    if (g->in_frame) return YGFX_ERR_ORDER;
    gl_ok = g->open && ygfx__sync(g) >= 0;
    {
        ygfx_capture_stats st;
        ygfx__cap_stop(g, gl_ok, &st);
        if (out) *out = st;
        return gl_ok ? (int)st.written : YGFX_ERR_LOST;
    }
}

YGFX_API void ygfx_capture_get_stats(const ygfx_gfx* g, ygfx_capture_stats* out) {
    if (!out) return;
    if (g && g->cap) ygfx__cap_stats(g->cap, out);
    else memset(out, 0, sizeof *out);
}

/* --- curve sets: the format (CURVE SETS) ------------------------------------- */

static float ygfx__u2f(uint32_t u) { float f; memcpy(&f, &u, sizeof f); return f; }

static int ygfx__cset_fail(char* msg, size_t cap, const char* fmt, ...) {
    va_list ap;
    if (msg && cap) {
        int n = snprintf(msg, cap, "ysp_gfx: curve set: ");
        if (n > 0 && (size_t)n < cap) {
            va_start(ap, fmt);
            vsnprintf(msg + n, cap - (size_t)n, fmt, ap);
            va_end(ap);
        }
    }
    return YGFX_ERR_FORMAT;
}

/* A list's sort key: the largest (forward) or smallest (backward) control
 * coordinate along the ray's axis; the shader's early exit depends on it. */
static float ygfx__cset_key(const float* t, int axis, int bwd) {
    float a = t[axis], b = t[2 + axis], c = t[4 + axis];
    if (bwd) return a < b ? (a < c ? a : c) : (b < c ? b : c);
    return a > b ? (a > c ? a : c) : (b > c ? b : c);
}

/* The set header: the words, the magic, the version, the table. */
static int ygfx__cset_head(const ygfx_cset_desc* d, char* msg, size_t cap) {
    const uint32_t* w;
    uint32_t nw, i;
    if (!d || !d->words) return ygfx__cset_fail(msg, cap, "no words");
    w = d->words;
    nw = d->n_words;
    if (nw < 8 || nw % 4) return ygfx__cset_fail(msg, cap, "n_words %u: at least 8 and a multiple of 4", nw);
    if (d->n_texels && !d->texels) return ygfx__cset_fail(msg, cap, "n_texels %u with no texels", d->n_texels);
    if (w[0] != YGFX_CSET_MAGIC) return ygfx__cset_fail(msg, cap, "word 0 is 0x%08x, not the magic 0x43505359", w[0]);
    if (w[1] != YGFX_CSET_VERSION) return ygfx__cset_fail(msg, cap, "format version %u; this ysp_gfx reads version 1", w[1]);
    for (i = 3; i < 8; i++)
        if (w[i]) return ygfx__cset_fail(msg, cap, "word %u is reserved and must be 0", i);
    if (w[2] >= (1u << 24) || (uint64_t)8 + w[2] > nw)
        return ygfx__cset_fail(msg, cap, "the glyph table (G = %u, word 2) passes the words or 2^24", w[2]);
    return YGFX_OK;
}

static int ygfx__cset_texels(const ygfx_cset_desc* d, uint32_t a, uint32_t b, char* msg, size_t cap) {
    uint32_t i;
    for (i = 4 * a; i < 4 * b; i++)
        if (!isfinite(d->texels[i])) return ygfx__cset_fail(msg, cap, "texel %u holds a value that is not finite", i / 4);
    return YGFX_OK;
}

/* One glyph's record, descriptors and lists, after the header's check. */
static int ygfx__cset_glyph(const ygfx_cset_desc* d, uint32_t g, char* msg, size_t cap) {
    const uint32_t* w = d->words;
    uint32_t nw = d->n_words, G = w[2], o = w[8 + g], nh, nv, fl, first, n, k;
    float bb[4];
    if (!o) return YGFX_OK;
    if (o % 4 || o < 8 + G || (uint64_t)o + 8 > nw)
        return ygfx__cset_fail(msg, cap, "glyph %u: its record offset %u (word %u) is not a multiple of 4 past the table "
                                 "and inside the words", g, o, 8 + g);
    for (k = 0; k < 4; k++) {
        bb[k] = ygfx__u2f(w[o + k]);
        if (!isfinite(bb[k])) return ygfx__cset_fail(msg, cap, "glyph %u: bbox word %u is not finite", g, o + k);
    }
    nh = w[o + 4] & 0xFFFFu; nv = w[o + 4] >> 16; fl = w[o + 5]; first = w[o + 6]; n = w[o + 7];
    if (fl & ~(YGFX_CSET_EVENODD | YGFX_CSET_BACKWARD | YGFX_CSET_RESOLVED))
        return ygfx__cset_fail(msg, cap, "glyph %u: flags 0x%x (word %u) set reserved bits", g, fl, o + 5);
    if (nh == 0 && nv == 0) return YGFX_OK;   /* an empty glyph */
    if (nh == 0 || nv == 0)
        return ygfx__cset_fail(msg, cap, "glyph %u: one band count is 0 (word %u): both are, or neither", g, o + 4);
    if (!(bb[2] > bb[0] && bb[3] > bb[1]))
        return ygfx__cset_fail(msg, cap, "glyph %u: bbox %g, %g, %g, %g is not x1 > x0, y1 > y0", g, bb[0], bb[1], bb[2], bb[3]);
    if (n < 2 || (uint64_t)first + n > d->n_texels)
        return ygfx__cset_fail(msg, cap, "glyph %u: texels %u + %u (words %u, %u) are not inside the %u texels",
                                 g, first, n, o + 6, o + 7, d->n_texels);
    if ((uint64_t)o + 8 + 4 * ((uint64_t)nh + nv) > nw)
        return ygfx__cset_fail(msg, cap, "glyph %u: %u + %u band descriptors pass the words", g, nh, nv);
    for (k = 0; k < nh + nv; k++) {
        uint32_t dw = o + 8 + 4 * k, half, j;
        int axis = k < nh ? 0 : 1;
        for (half = 0; half < 2; half++) {
            uint32_t off = w[dw + 2 * half], cnt = w[dw + 2 * half + 1];
            float prev = 0.0f;
            if (half && !(fl & YGFX_CSET_BACKWARD) && (off || cnt))
                return ygfx__cset_fail(msg, cap, "glyph %u: band %u has a backward list (word %u) without the flag", g, k, dw + 2);
            if ((uint64_t)off + cnt > nw)
                return ygfx__cset_fail(msg, cap, "glyph %u: band %u's list (word %u) passes the words", g, k, dw + 2 * half);
            for (j = 0; j < cnt; j++) {
                uint32_t r = w[off + j], m;
                const float* t;
                float key;
                if (r < first || (uint64_t)r + 2 > (uint64_t)first + n)
                    return ygfx__cset_fail(msg, cap, "glyph %u: band %u: reference %u (word %u) is not a curve in its texels %u..%u",
                                             g, k, r, off + j, first, first + n - 1);
                t = d->texels + 4 * (size_t)r;
                for (m = 0; m < 3; m++)
                    if (t[2 * m] < bb[0] || t[2 * m] > bb[2] || t[2 * m + 1] < bb[1] || t[2 * m + 1] > bb[3])
                        return ygfx__cset_fail(msg, cap, "glyph %u: curve %u (word %u) passes its bbox", g, r, off + j);
                key = ygfx__cset_key(t, axis, (int)half);
                if (j && (half ? key < prev : key > prev))
                    return ygfx__cset_fail(msg, cap, "glyph %u: band %u's %s list is not sorted at word %u", g, k,
                                             half ? "backward" : "forward", off + j);
                prev = key;
            }
        }
    }
    return YGFX_OK;
}

YGFX_API int ygfx_cset_check(const ygfx_cset_desc* d, char* msg, size_t cap) {
    uint32_t g;
    int rc;
    if (msg && cap) msg[0] = '\0';
    rc = ygfx__cset_head(d, msg, cap);
    if (rc == YGFX_OK) rc = ygfx__cset_texels(d, 0, d->n_texels, msg, cap);
    for (g = 0; rc == YGFX_OK && g < d->words[2]; g++) rc = ygfx__cset_glyph(d, g, msg, cap);
    return rc;
}

/* The crossings of one curve with the ray from the origin along +u, the
 * curve given across the ray (v) and along it (u), relative to the ray's
 * origin. y(t) = a t^2 - 2 b t + c has its falling root at (b - sqrt D) / a
 * and its rising root at (b + sqrt D) / a whatever the sign of a, and which
 * of them lie in [0, 1] follows from the signs of the three control values
 * alone (above: v > 0). So no t is compared with 0 or 1, and an endpoint on
 * the ray, shared by two curves, counts once. Rising counts +1. */
static int ygfx__cset_cross(const double v[3], const double u[3]) {
    int s1 = v[0] > 0.0, s2 = v[1] > 0.0, s3 = v[2] > 0.0, n = 0;
    int fall = (s1 && !s3) || (s1 && !s2 && s3) || (!s1 && s2 && !s3);
    int rise = (!s1 && s3) || (s1 && !s2 && s3) || (!s1 && s2 && !s3);
    double a, b, c, D, q, tr, tf, au, bu;
    if (!fall && !rise) return 0;
    a = v[0] - 2.0 * v[1] + v[2]; b = v[0] - v[1]; c = v[0];
    D = b * b - a * c;
    if (D < 0.0) {   /* no crossing in a two-root row (the shader's rule); else rounding */
        if (fall && rise) return 0;
        D = 0.0;
    }
    q = b + (b >= 0.0 ? sqrt(D) : -sqrt(D));
    /* the root that cancels nothing is q / a; its partner c / q */
    if (q == 0.0) { tr = tf = 0.0; }
    else if (b >= 0.0) { tr = q / a; tf = c / q; }
    else { tf = q / a; tr = c / q; }
    au = u[0] - 2.0 * u[1] + u[2]; bu = u[0] - u[1];
    if (rise && (au * tr - 2.0 * bu) * tr + u[0] > 0.0) n++;
    if (fall && (au * tf - 2.0 * bu) * tf + u[0] > 0.0) n--;
    return n;
}

YGFX_API int ygfx_cset_winding(const ygfx_cset_desc* d, uint32_t glyph, double x, double y) {
    const uint32_t* w;
    uint32_t o, nh, off, cnt, j;
    double y0, y1;
    int k, wsum = 0;
    if (!d || !d->words || !d->texels || d->n_words < 8) return 0;
    w = d->words;
    if (glyph >= w[2] || (uint64_t)8 + glyph >= d->n_words) return 0;
    o = w[8 + glyph];
    if (!o || (uint64_t)o + 8 > d->n_words) return 0;
    nh = w[o + 4] & 0xFFFFu;
    if (nh == 0) return 0;
    y0 = ygfx__u2f(w[o + 1]); y1 = ygfx__u2f(w[o + 3]);
    if (!(y >= y0 && y <= y1 && x <= ygfx__u2f(w[o + 2]))) return 0;
    k = (int)floor((y - y0) * (double)nh / (y1 - y0));
    if (k < 0) k = 0;
    if (k > (int)nh - 1) k = (int)nh - 1;
    if ((uint64_t)o + 8 + 4 * (uint64_t)k + 2 > d->n_words) return 0;
    off = w[o + 8 + 4 * (uint32_t)k]; cnt = w[o + 9 + 4 * (uint32_t)k];
    for (j = 0; j < cnt && (uint64_t)off + j < d->n_words; j++) {
        uint32_t r = w[off + j];
        const float* t;
        double u[3], v[3];
        if ((uint64_t)r + 2 > d->n_texels) break;
        t = d->texels + 4 * (size_t)r;
        u[0] = t[0] - x; v[0] = t[1] - y; u[1] = t[2] - x; v[1] = t[3] - y; u[2] = t[4] - x; v[2] = t[5] - y;
        /* sorted by descending largest x: nothing further holds a crossing to the right */
        if (u[0] <= 0.0 && u[1] <= 0.0 && u[2] <= 0.0) break;
        wsum += ygfx__cset_cross(v, u);
    }
    return wsum;
}

/* --- curve sets on the GPU, runs (CURVE SETS, RUNS) ------------------------------- */

/* Test seams, not API: 0 takes the rays where the exact area applies (to
 * measure the rays on resolved glyphs); the cap (px) of the half-plane
 * coverage along a ray; the rays' mix's e (docs/gfx.md, v0.6). */
static int   ygfx__text_v3 = 1;
/* Test seam: 0 keeps turned and aliased runs on the exact-area program */
static int   ygfx__text_auto = 1;
static float ygfx__text_cap = 1.0f, ygfx__text_e = 1.0f / 64.0f;

/* rays 1: the program without the exact area, for .rays runs. A flag in the
 * one program cost 0.7 ms more on a 12 px page than a program without the
 * code (6.0 against 5.3 ms, Iris Xe): the exact area's registers stay
 * reserved (docs/gfx.md, v0.7). */
static int ygfx__text_program(ygfx_gfx* g, int rays) {
    char defs[80];
    size_t nv, nf;
    char* vs;
    ygfx__prec r;
    int rc;
    uint32_t* pipe = rays ? &g->text_rays_pipe : &g->text_pipe;
    if (*pipe) return YGFX_OK;
    snprintf(defs, sizeof defs, "#define YSP_TEXT 1\n#define YSP_TEXT_V3 %d\n", rays ? 0 : ygfx__text_v3);
    nv = strlen(ygfx__glsl_common) + strlen(defs) + strlen(ygfx__glsl_text_vs) + 1;
    nf = strlen(ygfx__glsl_common) + strlen(defs) + strlen(ygfx__glsl_text_fs0) + strlen(ygfx__glsl_text_slab) +
         strlen(ygfx__glsl_text_ray) + strlen(ygfx__glsl_text_main) + 1;
    vs = (char*)malloc(nv + nf);
    if (!vs) { ygfx__set_error(g->error, sizeof g->error, "ysp_gfx: out of memory"); return YGFX_ERR_FULL; }
    snprintf(vs, nv, "%s%s%s", ygfx__glsl_common, defs, ygfx__glsl_text_vs);
    snprintf(vs + nv, nf, "%s%s%s%s%s%s", ygfx__glsl_common, defs, ygfx__glsl_text_fs0, ygfx__glsl_text_slab,
             ygfx__glsl_text_ray, ygfx__glsl_text_main);
    rc = ygfx__build(g, vs, vs + nv, YGFX_BLEND_OVER, 2, 0, pipe, &r);
    free(vs);
    g->be->reset(g->bctx);
    if (rc < 0) {
        char tmp[sizeof g->error];
        memcpy(tmp, g->error, sizeof tmp);
        ygfx__set_error(g->error, sizeof g->error, "%s (text%s)", tmp, rays ? ", rays" : "");
        *pipe = 0;
        return rc;
    }
    return YGFX_OK;
}

/* Texels a .. b - 1 of a linear array into a texture 2^L texels wide: a
 * partial first row, whole rows in one call, a partial last row. */
static int ygfx__cs_upload(ygfx_gfx* g, uint32_t id, int L, const void* data, uint32_t a, uint32_t b) {
    const uint32_t tw = 1u << L;
    const unsigned char* p = (const unsigned char*)data;
    int rc = YGFX_OK;
    while (a < b && rc >= 0) {
        uint32_t x = a & (tw - 1), n;
        if (x == 0 && b - a >= tw) {
            n = ((b - a) >> L) << L;
            rc = g->be->texture_update(g->bctx, id, 0, (int)(a >> L), (int)tw, (int)(n >> L), p + 16 * (size_t)a, 0);
        } else {
            n = tw - x < b - a ? tw - x : b - a;
            rc = g->be->texture_update(g->bctx, id, (int)x, (int)(a >> L), (int)n, 1, p + 16 * (size_t)a, 0);
        }
        a += n;
    }
    return rc;
}

/* Glyph g's box (set units) into the CPU's table; 0s for an absent or empty glyph. */
static void ygfx__cs_box(ygfx__cs* c, const uint32_t* w, uint32_t g) {
    float* b = c->bbox + 4 * (size_t)g;
    uint32_t o = w[8 + g], k;
    for (k = 0; k < 4; k++) b[k] = o && (w[o + 4] & 0xFFFFu) ? ygfx__u2f(w[o + k]) : 0.0f;
}

YGFX_API ygfx_cset ygfx_cset_make(ygfx_gfx* g, const ygfx_cset_desc* d) {
    ygfx_cset out;
    ygfx_texture_src ts;
    ygfx__cs* c;
    char msg[300];
    uint32_t ct, cw, rows_t, rows_w, k;
    int slot, L, maxt;
    out.id = 0;
    if (!g || !g->open) return out;
    if (!d) { ygfx__set_error(g->error, sizeof g->error, "ysp_gfx: no curve set desc"); return out; }
    if (g->in_frame) { ygfx__set_error(g->error, sizeof g->error, "ysp_gfx: a curve set made inside begin..end"); return out; }
    if (ygfx_cset_check(d, msg, sizeof msg) != YGFX_OK) { ygfx__set_error(g->error, sizeof g->error, "%s", msg); return out; }
    for (slot = 0; slot < YGFX_MAX_CSETS && g->cset[slot].used; slot++) {}
    if (slot == YGFX_MAX_CSETS) {
        ygfx__set_error(g->error, sizeof g->error, "ysp_gfx: %d curve sets in use (YGFX_MAX_CSETS)", YGFX_MAX_CSETS);
        return out;
    }
    ct = d->cap_texels > d->n_texels ? d->cap_texels : d->n_texels;
    if (ct < 2) ct = 2;
    cw = ((d->cap_words > d->n_words ? d->cap_words : d->n_words) + 3) / 4;
    maxt = g->caps.max_texture > 0 ? g->caps.max_texture : 2048;
    for (L = 12; L > 0 && (1 << L) > maxt; L--) {}
    rows_t = (ct + (1u << L) - 1) >> L;
    rows_w = (cw + (1u << L) - 1) >> L;
    if (rows_t > (uint32_t)maxt || rows_w > (uint32_t)maxt) {
        ygfx__set_error(g->error, sizeof g->error, "ysp_gfx: a curve set of %u texels and %u words passes this renderer's "
                          "%d x %d textures", ct, 4 * cw, maxt, maxt);
        return out;
    }
    if (ygfx__sync(g) < 0) return out;
    if (ygfx__text_program(g, 0) < 0) return out;
    c = &g->cset[slot];
    memset(c, 0, sizeof *c);
    c->G = d->words[2];
    c->bbox = (float*)malloc(16u * (size_t)(c->G ? c->G : 1));
    if (!c->bbox) { ygfx__set_error(g->error, sizeof g->error, "ysp_gfx: out of memory"); return out; }
    memset(&ts, 0, sizeof ts);
    ts.w = 1 << L; ts.h = (int32_t)rows_t; ts.format = YGFX_RGBA32F;
    if (g->be->texture_make(g->bctx, &ts, &c->tex_t) < 0) {
        free(c->bbox); memset(c, 0, sizeof *c);
        ygfx__set_error(g->error, sizeof g->error, "ysp_gfx: the curve texture (RGBA32F %d x %u)", 1 << L, rows_t);
        return out;
    }
    ts.h = (int32_t)rows_w; ts.format = YGFX_RGBA32UI;
    if (g->be->texture_make(g->bctx, &ts, &c->tex_w) < 0) {
        g->be->texture_free(g->bctx, c->tex_t);
        free(c->bbox); memset(c, 0, sizeof *c);
        ygfx__set_error(g->error, sizeof g->error, "ysp_gfx: the word texture (RGBA32UI %d x %u): the backend has no RGBA32UI",
                          1 << L, rows_w);
        return out;
    }
    if (ygfx__cs_upload(g, c->tex_t, L, d->texels, 0, d->n_texels) < 0 ||
        ygfx__cs_upload(g, c->tex_w, L, d->words, 0, d->n_words / 4) < 0) {
        g->be->texture_free(g->bctx, c->tex_t); g->be->texture_free(g->bctx, c->tex_w);
        free(c->bbox); memset(c, 0, sizeof *c);
        ygfx__set_error(g->error, sizeof g->error, "ysp_gfx: a curve set's upload failed");
        g->be->reset(g->bctx);
        return out;
    }
    g->be->reset(g->bctx);
    for (k = 0; k < c->G; k++) ygfx__cs_box(c, d->words, k);
    c->used = 1;
    c->n_texels = d->n_texels; c->n_words = d->n_words;
    c->cap_t = rows_t << L; c->cap_w = (rows_w << L) * 4u;
    c->lt = L;
    if (d->keep) c->keep = *d;
    out.id = (uint32_t)slot + 1;
    return out;
}

YGFX_API int ygfx_cset_add(ygfx_gfx* g, ygfx_cset s, const ygfx_cset_desc* now, const uint32_t* glyphs, int n) {
    ygfx__cs* c;
    char msg[300];
    int i, rc;
    if (!g || !g->open) return YGFX_ERR_CLOSED;
    if (g->in_frame) return YGFX_ERR_ORDER;
    if (s.id < 1 || s.id > YGFX_MAX_CSETS || !g->cset[s.id - 1].used || !now || n < 0 || (n > 0 && !glyphs)) return YGFX_ERR_ARG;
    c = &g->cset[s.id - 1];
    rc = ygfx__cset_head(now, msg, sizeof msg);
    if (rc == YGFX_OK && (now->words[2] != c->G || now->n_texels < c->n_texels || now->n_words < c->n_words))
        rc = ygfx__cset_fail(msg, sizeof msg, "the arrays may only grow, with the same glyph table");
    if (rc == YGFX_OK && (now->n_texels > c->cap_t || now->n_words > c->cap_w)) {
        ygfx__set_error(g->error, sizeof g->error, "ysp_gfx: curve set: %u texels and %u words pass its room (%u, %u: cap_texels, "
                          "cap_words)", now->n_texels, now->n_words, c->cap_t, c->cap_w);
        return YGFX_ERR_FULL;
    }
    if (rc == YGFX_OK) rc = ygfx__cset_texels(now, c->n_texels, now->n_texels, msg, sizeof msg);
    for (i = 0; rc == YGFX_OK && i < n; i++) {
        if (glyphs[i] >= c->G) rc = ygfx__cset_fail(msg, sizeof msg, "glyph %u is past the table (G = %u)", glyphs[i], c->G);
        else rc = ygfx__cset_glyph(now, glyphs[i], msg, sizeof msg);
    }
    if (rc != YGFX_OK) { ygfx__set_error(g->error, sizeof g->error, "%s", msg); return rc; }
    if (ygfx__sync(g) < 0) return YGFX_ERR_LOST;
    rc = ygfx__cs_upload(g, c->tex_t, c->lt, now->texels, c->n_texels, now->n_texels);
    if (rc >= 0) rc = ygfx__cs_upload(g, c->tex_w, c->lt, now->words, c->n_words / 4, now->n_words / 4);
    for (i = 0; rc >= 0 && i < n; i++) {
        uint32_t t = (8 + glyphs[i]) / 4;
        rc = ygfx__cs_upload(g, c->tex_w, c->lt, now->words, t, t + 1);
        ygfx__cs_box(c, now->words, glyphs[i]);
    }
    g->be->reset(g->bctx);
    if (rc < 0) { ygfx__set_error(g->error, sizeof g->error, "ysp_gfx: a curve set's upload failed"); return YGFX_ERR_GL; }
    c->n_texels = now->n_texels; c->n_words = now->n_words;
    if (c->keep.words) { c->keep = *now; c->keep.keep = true; }
    return YGFX_OK;
}

YGFX_API void ygfx_cset_free(ygfx_gfx* g, ygfx_cset s) {
    ygfx__cs* c;
    if (!g || !g->open || s.id < 1 || s.id > YGFX_MAX_CSETS || !g->cset[s.id - 1].used) return;
    c = &g->cset[s.id - 1];
    if (ygfx__sync(g) >= 0) {
        g->be->texture_free(g->bctx, c->tex_t);
        g->be->texture_free(g->bctx, c->tex_w);
    }
    free(c->bbox);
    memset(c, 0, sizeof *c);
}

YGFX_API ygfx_stim ygfx_crun(ygfx_gfx* g, const ygfx_crun_desc* d) {
    ygfx_stim s;
    ygfx__stim_defaults(&s, YGFX_TEXT);
    if (!d) return s;
    ygfx__align(&s, d->place, d->anchor);
    s.set = d->set; s.items = d->items; s.count = d->n > 0 ? (uint32_t)d->n : 0u;
    s.buf = d->buf; s.first = d->first;
    s.inst_fields = d->fields | YGFX_I_XY;
    s.palette = d->palette; s.n_palette = d->n_palette > 0 ? (uint32_t)d->n_palette : 0u;
    s.size = d->size;
    s.x = d->x; s.y = d->y; s.w = d->w; s.h = d->h; s.ori = d->ori;
    ygfx__copy3(s.color, d->color);
    if (d->opacity != 0.0f) s.opacity = d->opacity;
    s.blend = (uint8_t)d->blend; s.group = d->group;
    if (d->aliased) s.flags |= YGFX_STIM_ALIASED;
    if (d->rays) s.flags |= YGFX_STIM_RAYS;
    if (!g || !g->open) return s;
    if (d->items && s.count) ygfx__inst_memory(g);   /* setup: the items' frame buffers */
    /* setup: the rays program, for .rays and for the runs that may take it
     * automatically (turned, aliased, grouped); without it a turned run
     * stays on the exact-area program, which draws the same bits */
    if (d->rays || d->aliased || d->group || (d->fields & YGFX_I_ORI) || fmodf(fabsf(d->ori), 90.0f) != 0.0f)
        ygfx__text_program(g, 1);
    /* no box: the items' extent, its top-left kept in shape_p[0..1] (item units) */
    if (d->w == 0.0f && d->h == 0.0f && d->items && s.count && d->set.id >= 1 && d->set.id <= YGFX_MAX_CSETS &&
        g->cset[d->set.id - 1].used) {
        const ygfx__cs* c = &g->cset[d->set.id - 1];
        double lo[2] = { 1e300, 1e300 }, hi[2] = { -1e300, -1e300 };
        uint32_t i;
        for (i = 0; i < s.count; i++) {
            const ygfx_citem* it = &d->items[i];
            const float* b;
            int k;
            if (!(it->glyph >= 0.0f && it->glyph < (float)c->G && it->glyph == floorf(it->glyph))) continue;
            b = c->bbox + 4 * (size_t)it->glyph;
            if (!(b[2] > b[0])) continue;
            for (k = 0; k < 2; k++) {
                double p0 = (double)(k ? it->y : it->x) + (double)d->size * b[k], p1 = (double)(k ? it->y : it->x) + (double)d->size * b[2 + k];
                if (p0 < lo[k]) lo[k] = p0;
                if (p1 > hi[k]) hi[k] = p1;
            }
        }
        if (hi[0] > lo[0] && hi[1] > lo[1]) {
            s.w = (float)(hi[0] - lo[0]); s.h = (float)(hi[1] - lo[1]);
            s.shape_p[0] = (float)lo[0]; s.shape_p[1] = (float)lo[1];
        }
    }
    return s;
}

/* The topmost item of a curve run that holds the screen pixel: by the
 * winding at the pixel center from the set's own arrays when the set keeps
 * them, else by the glyph boxes; -1 for none. Items in a buffer are on the
 * GPU only: -1. */
static int ygfx__crun_hit(const ygfx_gfx* g, const ygfx_stim* s, float px, float py) {
    const ygfx__cs* c;
    const uint32_t f = s->inst_fields | YGFX_I_XY;
    float lx, ly, hx, hy, sp3[3];
    double X, Y, ppu;
    int i;
    if (!s->items || s->set.id < 1 || s->set.id > YGFX_MAX_CSETS || !g->cset[s->set.id - 1].used || !(s->size > 0.0f)) return -1;
    c = &g->cset[s->set.id - 1];
    ygfx_local(g, s, px, py, &lx, &ly);
    ygfx__geom(g, s, &hx, &hy, sp3);
    ppu = (double)g->ppu * ygfx__scale(s);
    X = (double)lx + hx; Y = (double)ly + hy;   /* box px from its top-left */
    for (i = (int)s->count - 1; i >= 0; i--) {
        const ygfx_citem* it = &s->items[i];
        const float* b;
        double k = (f & YGFX_I_SCALE) ? it->scale : 1.0, sp, cx, cy, dx, dy, cs = 1.0, sn = 0.0, ex, ey;
        uint32_t gid;
        if ((f & YGFX_I_GATE) && !(it->gate != 0.0f)) continue;
        if (!(it->glyph >= 0.0f && it->glyph < (float)c->G && it->glyph == floorf(it->glyph)) || !(k > 0.0)) continue;
        gid = (uint32_t)it->glyph;
        b = c->bbox + 4 * (size_t)gid;
        if (!(b[2] > b[0])) continue;
        sp = (double)s->size * ppu;
        cx = 0.5 * ((double)b[0] + b[2]); cy = 0.5 * ((double)b[1] + b[3]);
        /* the glyph box's center in box px, then the pixel in em about it */
        dx = X - (((double)it->x - s->shape_p[0]) * ppu + sp * cx);
        dy = Y - (((double)it->y - s->shape_p[1]) * ppu + sp * cy);
        if (f & YGFX_I_ORI) { double r = (double)it->ori * 0.017453292519943295; cs = cos(r); sn = sin(r); }
        ex = cx + (dx * cs + dy * sn) / (sp * k);
        ey = cy + (-dx * sn + dy * cs) / (sp * k);
        if (c->keep.words) {
            int w = ygfx_cset_winding(&c->keep, gid, ex, ey);
            const uint32_t o = c->keep.words[8 + gid];
            if ((o && (c->keep.words[o + 5] & YGFX_CSET_EVENODD)) ? (w & 1) : w != 0) return i;
        } else if (ex >= b[0] && ex <= b[2] && ey >= b[1] && ey <= b[3]) {
            return i;
        }
    }
    return -1;
}

/* A run's block (RUNS) and its items into the frame's element buffer. */
static uint32_t ygfx__text_pack(ygfx_gfx* g, const ygfx_stim* s, float* b, ygfx__cmd* cmd, int* why) {
    const ygfx__cs* c;
    const uint32_t f = s->inst_fields | YGFX_I_XY;
    float hx, hy, sp[3], ppu;
    uint32_t k;
    double run_deg;
    int turned;
    if (s->set.id < 1 || s->set.id > YGFX_MAX_CSETS || !g->cset[s->set.id - 1].used)
        return ygfx__refuse(g, why, YGFX_ERR_ARG, "a run needs a curve set of this gfx (set)");
    c = &g->cset[s->set.id - 1];
    if (!(s->size > 0.0f)) return ygfx__refuse(g, why, YGFX_ERR_ARG, "a run needs size > 0 (units per set unit); it would cover no pixel");
    if (s->edge != YGFX_EDGE_HARD || s->edge_width != 0.0f || s->stroke > 0.0f || s->offset != 0.0f || s->dash[0] > 0.0f ||
        ygfx__trimmed(s) || s->fx || s->paint || s->prims)
        return ygfx__refuse(g, why, YGFX_ERR_ARG, "a run's edge is the pixel's box filter: edge, stroke, offset, dashes, trim, fx "
                              "and paint are refused (the outline builder expands outlines; the blur pass makes soft edges)");
    if (s->inst) return ygfx__refuse(g, why, YGFX_ERR_ARG, "a run is instanced already: it cannot be an instances template");
    if ((f & YGFX_I_COLOR) && (!s->palette || s->n_palette < 1 || s->n_palette > YGFX_MAX_PALETTE))
        return ygfx__refuse(g, why, YGFX_ERR_ARG, "YGFX_I_COLOR needs a palette of 1 to 16 rgb entries");
    ygfx__place(g, s, b);
    ygfx__geom(g, s, &hx, &hy, sp);
    ppu = g->ppu * ygfx__scale(s);
    b[4] = hx; b[5] = hy; b[6] = ppu; b[7] = s->size * ppu;
    b[8] = s->opacity; b[9] = s->gate * (s->group ? s->group->opacity : 1.0f); b[10] = (float)f;
    b[11] = (s->flags & YGFX_STIM_ALIASED) ? 1.0f : 0.0f;
    ygfx__copy3(b + 12, s->color);
    b[15] = (float)s->n_palette;
    b[16] = (float)c->lt; b[17] = 0.75f; b[18] = (float)c->G; b[19] = 0.0f;
    b[20] = ygfx__text_cap; b[21] = ygfx__text_e;
    b[22] = s->shape_p[0]; b[23] = s->shape_p[1];   /* the box's top-left in item units */
    {   /* the placement in px with y down, the same in every pass (the text program) */
        double a[2], m[2], cs, sn;
        ygfx__frame_px(g, s, (double)g->cur_w, (double)g->cur_h, a, m, &cs, &sn);
        b[24] = (float)m[0]; b[25] = (float)m[1]; b[26] = (float)cs; b[27] = (float)sn;
    }
    memset(b + 64, 0, 2 * YGFX__BLOCK);
    for (k = 0; k < 3; k++)
        if (s->color[k] < 0.0f || s->color[k] > 1.0f) { g->clipped++; break; }
    if ((f & YGFX_I_COLOR) && s->palette)
        for (k = 0; k < s->n_palette; k++) {
            int q;
            for (q = 0; q < 3; q++) b[128 + 4 * k + (uint32_t)q] = s->palette[3 * k + (uint32_t)q];
        }
    /* Rays automatically (RUNS): where no item can take the exact area
     * (every item turned off the quarter turns, or the run aliased), the
     * rays program draws the same bits faster. turned: the run's own turn
     * is 0.01 degrees or more from a quarter turn, or with YGFX_I_ORI
     * every item's turn plus the run's is; items in a buffer are on the GPU
     * only, so such a run is turned only without YGFX_I_ORI. */
    run_deg = atan2((double)b[27], (double)b[26]) * 57.29577951308232;
    {
        double t = fmod(fabs(run_deg), 90.0);
        turned = (f & YGFX_I_ORI) ? (s->items != NULL) : (t > 0.01 && t < 89.99);
    }
    if (s->items) {
        size_t bytes = (size_t)s->count * sizeof(ygfx_citem);
        if (!g->inst_staging)
            return ygfx__refuse(g, why, YGFX_ERR_ORDER, "ygfx_crun() makes the items' buffers: make the run with it, with its items");
        if ((size_t)g->inst_used + bytes > (size_t)g->inst_cap * sizeof(ygfx_inst))
            return ygfx__refuse(g, why, YGFX_ERR_FULL, "the frame's items and elements pass desc.max_instances: none of this run is drawn");
        for (k = 0; k < s->count; k++) {
            float gid = s->items[k].glyph;
            if (!(gid >= 0.0f && gid < (float)c->G && gid == floorf(gid))) {
                char msg[160];
                snprintf(msg, sizeof msg, "item %u's glyph %g is not an integer in 0 .. %u (the set's glyph table)", k, (double)gid, c->G - 1);
                return ygfx__refuse(g, why, YGFX_ERR_ARG, msg);
            }
            if (turned && (f & YGFX_I_ORI)) {   /* the item's turn adds to the run's */
                double t = fmod(fabs(run_deg + (double)s->items[k].ori), 90.0);
                if (!(t > 0.01 && t < 89.99)) turned = 0;
            }
        }
        memcpy(g->inst_staging + g->inst_used, s->items, bytes);
        cmd->inst = g->inst_buf[g->ubo_i];
        cmd->inst_off = (uint32_t)g->inst_used;
        g->inst_used += (int32_t)bytes;
    } else {
        if (s->buf.id < 1 || s->buf.id > YGFX_MAX_BUFFERS || !g->buf[s->buf.id - 1].used ||
            ((size_t)s->first + s->count) * sizeof(ygfx_citem) > (size_t)g->buf[s->buf.id - 1].w)
            return ygfx__refuse(g, why, YGFX_ERR_ARG, "a run without items needs a buffer that holds first + n items (buf)");
        cmd->inst = g->buf[s->buf.id - 1].bid;
        cmd->inst_off = s->first * (uint32_t)sizeof(ygfx_citem);
    }
    cmd->count = s->count;
    cmd->nblk = 3;
    cmd->tex[0] = c->tex_t;
    cmd->tex[3] = c->tex_w;
    if ((s->flags & YGFX_STIM_RAYS) && !g->text_rays_pipe)
        return ygfx__refuse(g, why, YGFX_ERR_GL, "the rays program was not built: make the run with ygfx_crun(.rays)");
    *why = YGFX_OK;
    if ((s->flags & YGFX_STIM_RAYS) ||
        (ygfx__text_auto && g->text_rays_pipe && (turned || (s->flags & YGFX_STIM_ALIASED))))
        return g->text_rays_pipe;
    return g->text_pipe;
}

/* --- the blur pass (BLUR) --------------------------------------------------------- */

/* Two passes of direct taps: the output texel's center in input texels is
 * r (i + 0.5) along the pass's axis (r = 2 from a 2x layer), the taps are the
 * input texel centers within 5 s', weighted exp(-x^2 / 2 s'^2) and
 * normalized in f32; taps outside the input count 0. Block vec4 8: axis,
 * r, s' (input texels); 9: input w, h, reach (input texels). */
static const char ygfx__glsl_blur_vs[] =
    "void main() {\n"
    "    vec2 c = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1)) * 2.0 - 1.0;\n"
    "    gl_Position = vec4(c, 0.0, 1.0);\n"
    "}\n";
static const char ygfx__glsl_blur_fs[] =
    "uniform sampler2D ysp_tex0;\n"
    "out vec4 ysp_out;\n"
    "void main() {\n"
    "    vec4 P = ysp_v[8], Q = ysp_v[9];\n"
    "    bool hz = P.x > 0.5;\n"
    "    ivec2 o = ivec2(gl_FragCoord.xy);\n"
    "    float c = P.z * (hz ? gl_FragCoord.x : gl_FragCoord.y);\n"
    "    int n = int(hz ? Q.x : Q.y), j0 = max(int(ceil(c - 0.5 - Q.z)), 0), j1 = min(int(floor(c - 0.5 + Q.z)), n - 1);\n"
    "    int jl = int(ceil(c - 0.5 - Q.z)), jh = int(floor(c - 0.5 + Q.z));\n"
    "    float k = -0.5 / (P.w * P.w), ws = 0.0;\n"
    "    vec4 acc = vec4(0.0);\n"
    /* the weights' sum over every tap, inside or not: outside is 0, not missing */
    "    for (int j = jl; j <= jh; j++) {\n"
    "        float x = float(j) + 0.5 - c, w = exp(k * x * x);\n"
    "        ws += w;\n"
    "        if (j >= j0 && j <= j1) acc += w * texelFetch(ysp_tex0, hz ? ivec2(j, o.y) : ivec2(o.x, j), 0);\n"
    "    }\n"
    "    ysp_out = acc / ws;\n"
    "}\n";

static int ygfx__blur_program(ygfx_gfx* g) {
    size_t nv, nf;
    char* vs;
    ygfx__prec r;
    int rc;
    if (g->blur_pipe) return YGFX_OK;
    nv = strlen(ygfx__glsl_common) + strlen(ygfx__glsl_blur_vs) + 1;
    nf = strlen(ygfx__glsl_common) + strlen(ygfx__glsl_blur_fs) + 1;
    vs = (char*)malloc(nv + nf);
    if (!vs) { ygfx__set_error(g->error, sizeof g->error, "ysp_gfx: out of memory"); return YGFX_ERR_FULL; }
    snprintf(vs, nv, "%s%s", ygfx__glsl_common, ygfx__glsl_blur_vs);
    snprintf(vs + nv, nf, "%s%s", ygfx__glsl_common, ygfx__glsl_blur_fs);
    rc = ygfx__build(g, vs, vs + nv, YGFX_BLEND_NONE, 0, 0, &g->blur_pipe, &r);
    free(vs);
    g->be->reset(g->bctx);
    if (rc < 0) { g->blur_pipe = 0; return rc; }
    return YGFX_OK;
}

YGFX_API int ygfx_blur_make(ygfx_gfx* g, ygfx_blur* b, const ygfx_blur_desc* d) {
    ygfx_target_desc td;
    ygfx_image_desc id;
    ygfx_format f;
    int ss, rc;
    if (!g || !g->open) return YGFX_ERR_CLOSED;
    if (!b || !d) { ygfx__set_error(g->error, sizeof g->error, "ysp_gfx: ygfx_blur_make needs a blur and a desc"); return YGFX_ERR_ARG; }
    memset(b, 0, sizeof *b);
    f = d->format ? d->format : YGFX_RGBA16F;
    ss = d->supersample > 1 ? d->supersample : 1;
    if (d->w <= 0 || d->h <= 0 || (d->supersample != 0 && d->supersample != 1 && d->supersample != 2) ||
        (f != YGFX_R16F && f != YGFX_RGBA16F && f != YGFX_RGBA32F) ||
        (d->source != YGFX_BLUR_BOX && d->source != YGFX_BLUR_POINT)) {
        ygfx__set_error(g->error, sizeof g->error, "ysp_gfx: a blur needs w, h > 0, format R16F, RGBA16F or RGBA32F, supersample 0, 1 "
                          "or 2 and a ygfx_blur_source");
        return YGFX_ERR_ARG;
    }
    if (g->in_frame) { ygfx__set_error(g->error, sizeof g->error, "ysp_gfx: a blur made inside begin..end"); return YGFX_ERR_ORDER; }
    rc = ygfx__blur_program(g);
    if (rc < 0) return rc;
    memset(&td, 0, sizeof td);
    td.format = f;
    td.w = d->w * ss; td.h = d->h * ss;
    b->layer = ygfx_target(g, &td);
    td.w = d->w; td.h = d->h * ss;
    if (b->layer.id) b->scratch_ = ygfx_target(g, &td);
    td.h = d->h;
    if (b->scratch_.id) b->result = ygfx_target(g, &td);
    if (!b->result.id) {
        char tmp[sizeof g->error];
        memcpy(tmp, g->error, sizeof tmp);
        ygfx_texture_free(g, b->layer); ygfx_texture_free(g, b->scratch_);
        memset(b, 0, sizeof *b);
        ygfx__set_error(g->error, sizeof g->error, "%s (the blur's targets)", tmp);
        return f == YGFX_RGBA32F && !g->caps.float_blend ? YGFX_ERR_REFUSED : YGFX_ERR_GL;
    }
    g->tex[b->layer.id - 1].ss = ss;
    memset(&id, 0, sizeof id);
    id.tex = b->result;
    id.coverage = f == YGFX_R16F;
    b->image = ygfx_image(g, &id);
    b->sigma = d->sigma;
    b->w_ = d->w; b->h_ = d->h; b->ss_ = ss; b->src_ = (int32_t)d->source;
    return YGFX_OK;
}

YGFX_API int ygfx_blur_apply(ygfx_gfx* g, ygfx_blur* b) {
    double r, var, sp;
    int pass;
    if (!g || !g->open) return YGFX_ERR_CLOSED;
    if (!g->in_frame || g->in_target) { ygfx__set_error(g->error, sizeof g->error, "ysp_gfx: a blur runs in a frame, outside a target pass"); return YGFX_ERR_ORDER; }
    if (!b || !b->w_ || !b->result.id) return YGFX_ERR_ARG;
    if (g->n_targets + 2 > YGFX_MAX_PASSES) {
        ygfx__set_error(g->error, sizeof g->error, "ysp_gfx: a blur takes 2 of the frame's %d passes", YGFX_MAX_PASSES);
        return YGFX_ERR_ORDER;
    }
    r = (double)b->ss_;
    /* a box-filtered source has the box's variance already: 1/12 of its
     * texel squared, a texel being 1 / r result px */
    var = (double)b->sigma * b->sigma - (b->src_ == YGFX_BLUR_BOX ? 1.0 / (12.0 * r * r) : 0.0);
    if (!(b->sigma > 0.0f && b->sigma <= 32.0f && var > 1e-6)) {
        ygfx__set_error(g->error, sizeof g->error, "ysp_gfx: blur sigma %g px: it is 32 or less and, for a BOX source, above %.3f "
                          "(the box's own SD)", (double)b->sigma, sqrt(1.0 / (12.0 * r * r)));
        return YGFX_ERR_ARG;
    }
    sp = r * sqrt(var);
    if (g->n_cmds + 2 > g->max_draws || g->n_blocks + 2 > g->max_draws) return YGFX_ERR_FULL;
    for (pass = 0; pass < 2; pass++) {
        const ygfx__res* in = &g->tex[(pass ? b->scratch_.id : b->layer.id) - 1];
        const ygfx__res* out = &g->tex[(pass ? b->result.id : b->scratch_.id) - 1];
        ygfx__cmd* cmd = &g->cmds[g->n_cmds];
        float* k = (float*)(g->staging + YGFX__FRAME_BYTES + (size_t)g->n_blocks * YGFX__BLOCK);
        g->n_targets++;
        ygfx__frame_block(g, g->n_targets, out->w, out->h, 1);
        ygfx__seg_next(g, out->bid, out->w, out->h, 1, g->n_targets, NULL);
        memset(cmd, 0, sizeof *cmd);
        memset(k, 0, YGFX__BLOCK);
        /* the whole pass, for the reorder's bounds */
        k[0] = 0.5f * (float)out->w; k[1] = 0.5f * (float)out->h; k[2] = 1.0f;
        k[4] = 0.5f * (float)out->w; k[5] = 0.5f * (float)out->h;
        k[32] = pass ? 0.0f : 1.0f; k[33] = pass ? 1.0f : 0.0f; k[34] = (float)r; k[35] = (float)sp;
        k[36] = (float)in->w; k[37] = (float)in->h; k[38] = (float)(5.0 * sp);
        cmd->pipe = g->blur_pipe;
        cmd->tex[0] = in->bid;
        cmd->block = (uint32_t)g->n_blocks;
        cmd->nblk = 1;
        g->n_blocks++;
        g->n_cmds++;
    }
    ygfx__seg_next(g, 0, g->w, g->h, 0, 0, NULL);
    return YGFX_OK;
}

YGFX_API void ygfx_blur_free(ygfx_gfx* g, ygfx_blur* b) {
    if (!g || !b) return;
    ygfx_texture_free(g, b->layer);
    ygfx_texture_free(g, b->scratch_);
    ygfx_texture_free(g, b->result);
    memset(b, 0, sizeof *b);
}

/* --- noise ------------------------------------------------------------------- */

/* triple32, from Chris Wellons's hash-prospector (public domain): the best
 * measured avalanche of the 32-bit candidates (docs/gfx.md, v0.9). */
YGFX_API uint32_t ygfx_hash(uint32_t x) {
    x ^= x >> 17; x *= 0xed5ad4bbu;
    x ^= x >> 11; x *= 0xac4c1b51u;
    x ^= x >> 15; x *= 0x31848babu;
    x ^= x >> 14;
    return x;
}

YGFX_API uint32_t ygfx_hash2(int32_t cx, int32_t cy, uint32_t seed) {
    uint32_t k = ygfx_hash(seed), m = ygfx_hash(k ^ 0x9E3779B9u);
    return ygfx_hash(((uint32_t)cx + ((uint32_t)cy << 16)) * (k | 1u) ^ m);
}

/* NOISE SIMPLEX on the CPU: ysp_gn_() and ysp_gnoise_() of the shader,
 * operation for operation in 32-bit integers (unsigned wraps as GLSL's
 * uint; a signed right shift is a floor, as GLSL's). */
static int32_t ygfx__asr13(int32_t x) { return x >= 0 ? x >> 13 : -(int32_t)(((uint32_t)-x + 8191u) >> 13); }

static int32_t ygfx__gn_i(uint32_t vx, uint32_t vy, uint32_t vz, uint32_t S) {
    static const int8_t G[16][3] = { { 1, 1, 0 }, { -1, 1, 0 }, { 1, -1, 0 }, { -1, -1, 0 }, { 1, 0, 1 }, { -1, 0, 1 }, { 1, 0, -1 },
                                     { -1, 0, -1 }, { 0, 1, 1 }, { 0, -1, 1 }, { 0, 1, -1 }, { 0, -1, -1 }, { 1, 1, 0 }, { -1, 1, 0 },
                                     { 0, -1, 1 }, { 0, -1, -1 } };
    uint32_t sq = (vx + vy + vz) / 3u, cx = (vx + sq) >> 14, cy = (vy + sq) >> 14, cz = (vz + sq) >> 14;
    uint32_t g3 = ((cx + cy + cz) << 14) / 6u, L[2][2];
    int32_t x0[3], e[3], i1[3], i2[3], x[4][3], o[4][3], sum = 0;
    int k, j;
    x0[0] = (int32_t)(vx - (cx << 14) + g3); x0[1] = (int32_t)(vy - (cy << 14) + g3); x0[2] = (int32_t)(vz - (cz << 14) + g3);
    for (j = 0; j < 3; j++) e[j] = x0[j] >= x0[(j + 1) % 3];
    for (j = 0; j < 3; j++) {   /* e.zxy is e[(j + 2) % 3] */
        i1[j] = e[j] * (1 - e[(j + 2) % 3]);
        i2[j] = 1 - e[(j + 2) % 3] * (1 - e[j]);
    }
    for (k = 0; k < 2; k++) {
        uint32_t kk = ygfx_hash((cz + (uint32_t)k) ^ S);
        L[k][0] = kk | 1u; L[k][1] = ygfx_hash(kk ^ 0x9E3779B9u);
    }
    for (j = 0; j < 3; j++) {
        x[0][j] = x0[j]; x[1][j] = x0[j] - i1[j] * 16384 + 2731; x[2][j] = x0[j] - i2[j] * 16384 + 5461; x[3][j] = x0[j] - 8192;
        o[0][j] = 0; o[1][j] = i1[j]; o[2][j] = i2[j]; o[3][j] = 1;
    }
    for (k = 0; k < 4; k++) {
        int32_t t = 161061274 - (x[k][0] * x[k][0] + x[k][1] * x[k][1] + x[k][2] * x[k][2]);
        if (t > 0) {
            uint32_t ccx = cx + (uint32_t)o[k][0], ccy = cy + (uint32_t)o[k][1];
            const uint32_t* Lk = L[o[k][2] != 0];
            const int8_t* gr = G[ygfx_hash((ccx + (ccy << 16)) * Lk[0] ^ Lk[1]) >> 28];
            t >>= 12; t = (t * t) >> 16; t = (t * t) >> 16;
            sum += t * (gr[0] * x[k][0] + gr[1] * x[k][1] + gr[2] * x[k][2]);
        }
    }
    return sum;
}

/* Each octave's Q14 lattice units per px (r), its z in Q14 mod 2^30 (z),
 * and its weight (w, the weights summing to about 4096: an octave's value
 * in Q12 times its weight, summed, is the field in Q24, exact in a float);
 * NULL out: only the checks. */
static const char* ygfx__gn_setup(const ygfx_gfx* g, const ygfx_stim* s, ygfx__gn* out) {
    const double px = (double)s->gn_scale * g->ppu * ygfx__scale(s), lac = s->gn_lacunarity, gain = s->gn_gain;
    const double ext = (s->w > s->h ? s->w : s->h) * g->ppu * ygfx__scale(s) + 4.0;
    double tot = 0.0, a = 1.0, f = 1.0;
    int n = s->gn_octaves, o;
    if (!(px > 0.0)) return "SIMPLEX needs scale > 0 (units per lattice cell)";
    if (n < 1 || n > 8) return "SIMPLEX takes 1 to 8 octaves";
    if (!(lac >= 1.0 && lac <= 8.0)) return "SIMPLEX takes a lacunarity of 1 to 8";
    if (!(gain > 0.0 && gain <= 1.0)) return "SIMPLEX takes a gain above 0 and at most 1";
    if (s->shape == YGFX_POLYGON) return "SIMPLEX keeps its octaves where a POLYGON aperture keeps its vertices: use MASK_TEX";
    if (!(s->gn_z > -1e6f && s->gn_z < 1e6f)) return "SIMPLEX z is a number of cells below 1e6 (used modulo 65536)";
    for (o = 0; o < n; o++) { tot += a; a *= gain; }
    for (o = 0, a = 1.0, f = 1.0; o < n; o++, a *= gain, f *= lac) {
        double r = floor(16384.0 * f / px + 0.5), z = floor((double)s->gn_z * 16384.0 * f + 0.5);
        if (r < 1.0) return "SIMPLEX scale: a lattice cell is at most 32768 px";
        if (r * (2.0 * ext + 1.0) >= 1073741824.0) return "SIMPLEX scale: a cell at the finest octave is at least 1/32 px across this box";
        if (out) {
            out->r[o] = (uint32_t)r;
            out->z[o] = (uint32_t)((int64_t)z & 0x3FFFFFFF);
            out->w[o] = (int32_t)floor(4096.0 * a / tot + 0.5);
        }
    }
    if (out) {
        for (; o < 8; o++) { out->r[o] = 0; out->z[o] = 0; out->w[o] = 0; }
        out->n = n;
    }
    return NULL;
}

YGFX_API float ygfx_simplex_value(const ygfx_gfx* g, const ygfx_stim* s, int32_t ix, int32_t iy) {
    ygfx__gn q;
    uint32_t S;
    int32_t n = 0;
    int o;
    if (!g || !s || ygfx__gn_setup(g, s, &q)) return 0.0f;
    S = ygfx_hash((uint32_t)s->seed);
    for (o = 0; o < q.n; o++) {
        uint32_t vx = ((2u * (uint32_t)ix + 1u) * q.r[o]) >> 1, vy = ((2u * (uint32_t)iy + 1u) * q.r[o]) >> 1;
        n += ygfx__asr13(ygfx__gn_i(vx + 0x10000000u, vy + 0x10000000u, q.z[o] + 0x10000000u, ygfx_hash(S + (uint32_t)o))) * q.w[o];
    }
    {
        float v;
        n = n < -16777215 ? -16777215 : (n > 16777215 ? 16777215 : n);
        v = (float)n * (1.0f / 16777216.0f);
        return v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v);
    }
}

/* NOISE GAUSSIAN's table: the standard normal's quantiles at the centers of
 * 65536 equally likely bins, Phi^-1((i + 0.5) / 65536), by Acklam's
 * rational approximation (relative error below 1.15e-9), scaled so that
 * the 65536 values have variance 1 exactly in double, then rounded to
 * float. Symmetric bit for bit (the upper half is the lower half negated),
 * so the mean is 0. The largest value is 4.33. Built once, at the first
 * ygfx_open() or GAUSSIAN ygfx_noise_value(); not thread-safe before
 * that first call. */
static float ygfx__gauss_tab[65536];
static int   ygfx__gauss_ready;

static double ygfx__probit(double p) {
    static const double a[6] = { -3.969683028665376e+01, 2.209460984245205e+02, -2.759285104469687e+02,
                                 1.383577518672690e+02, -3.066479806614716e+01, 2.506628277459239e+00 };
    static const double b[5] = { -5.447609879822406e+01, 1.615858368580409e+02, -1.556989798598866e+02,
                                 6.680131188771972e+01, -1.328068155288572e+01 };
    static const double c[6] = { -7.784894002430293e-03, -3.223964580411365e-01, -2.400758277161838e+00,
                                 -2.549732539343734e+00, 4.374664141464968e+00, 2.938163982698783e+00 };
    static const double d[4] = { 7.784695709041462e-03, 3.224671290700398e-01, 2.445134137142996e+00,
                                 3.754408661907416e+00 };
    double q, r;
    if (p < 0.02425) {
        q = sqrt(-2.0 * log(p));
        return (((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) /
               ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1.0);
    }
    q = p - 0.5; r = q * q;
    return (((((a[0] * r + a[1]) * r + a[2]) * r + a[3]) * r + a[4]) * r + a[5]) * q /
           (((((b[0] * r + b[1]) * r + b[2]) * r + b[3]) * r + b[4]) * r + 1.0);
}

static const float* ygfx__gauss_table(void) {
    static double x[32768];
    double ss = 0.0, k;
    int i;
    if (ygfx__gauss_ready) return ygfx__gauss_tab;
    for (i = 0; i < 32768; i++) {   /* the lower half, every p below 0.5 */
        x[i] = ygfx__probit(((double)i + 0.5) / 65536.0);
        ss += x[i] * x[i];
    }
    k = 1.0 / sqrt(ss / 32768.0);   /* the upper half has the same squares */
    for (i = 0; i < 32768; i++) {
        ygfx__gauss_tab[i] = (float)(x[i] * k);
        ygfx__gauss_tab[65535 - i] = -ygfx__gauss_tab[i];
    }
    ygfx__gauss_ready = 1;
    return ygfx__gauss_tab;
}

/* The NOISE body's arithmetic, step for step: an odd 24-bit integer over
 * 2^23 is exact in a float, so the GPU and the CPU agree to the bit. */
YGFX_API float ygfx_noise_value(int32_t cx, int32_t cy, uint32_t seed, ygfx_noise_dist dist) {
    uint32_t h = ygfx_hash2(cx, cy, seed & 0xFFFFFFu);
    if (dist == YGFX_GAUSSIAN) return ygfx__gauss_table()[h >> 16];
    if (dist == YGFX_BINARY) return (h & 0x80000000u) ? 1.0f : -1.0f;
    return (float)((int32_t)(((h >> 9) << 1) + 1u) - 8388608) * (1.0f / 8388608.0f);
}

YGFX_API void ygfx_noise_fill(float* out, int w, int h, uint32_t seed, ygfx_noise_dist dist) {
    int x, y;
    if (!out) return;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) out[(size_t)y * (size_t)w + (size_t)x] = ygfx_noise_value(x, y, seed, dist);
}

YGFX_API void ygfx_noise_gauss(float* out, int w, int h, uint32_t seed) {
    size_t n = (size_t)w * (size_t)h, i;
    uint32_t s1, s2;
    if (!out || w <= 0 || h <= 0) return;
    s1 = ygfx_hash(seed) | 1u;   /* the index times an odd key, as ygfx_hash2() */
    s2 = ygfx_hash(s1 ^ 0x9E3779B9u);
    for (i = 0; i < n; i += 2) {
        uint32_t a = ygfx_hash((uint32_t)i * s1 ^ s2), b = ygfx_hash((uint32_t)(i + 1) * s1 ^ s2);
        double u1 = ((double)(a >> 8) + 0.5) / 16777216.0, u2 = ((double)(b >> 8) + 0.5) / 16777216.0;
        double r = sqrt(-2.0 * log(u1)), t = 6.283185307179586 * u2;
        out[i] = (float)(r * cos(t));
        if (i + 1 < n) out[i + 1] = (float)(r * sin(t));
    }
}

/* --- signed distance from a mask ----------------------------------------------- */

/* One row of the squared Euclidean distance transform (Felzenszwalb and
 * Huttenlocher 2012): f in, d out, n samples; v and z are scratch. */
static void ygfx__edt1(const double* f, double* d, int n, int* v, double* z) {
    int k = 0, q;
    v[0] = 0; z[0] = -1e300; z[1] = 1e300;
    for (q = 1; q < n; q++) {
        double sq;
        for (;;) {
            int r = v[k];
            sq = ((f[q] + (double)q * q) - (f[r] + (double)r * r)) / (2.0 * q - 2.0 * r);
            if (sq <= z[k] && k > 0) { k--; continue; }
            if (sq <= z[k]) { /* k == 0 and z[0] is -inf: never */ }
            break;
        }
        k++;
        v[k] = q; z[k] = sq; z[k + 1] = 1e300;
    }
    k = 0;
    for (q = 0; q < n; q++) {
        while (z[k + 1] < q) k++;
        d[q] = ((double)q - v[k]) * ((double)q - v[k]) + f[v[k]];
    }
}

/* Squared distance from every texel to the nearest texel where in[] is set. */
static void ygfx__edt2(const unsigned char* in, double* out, int w, int h, double* f, double* d, int* v, double* z) {
    int x, y, n = w > h ? w : h;
    (void)n;
    for (x = 0; x < w; x++) {
        for (y = 0; y < h; y++) f[y] = in[(size_t)y * w + x] ? 0.0 : 1e20;
        ygfx__edt1(f, d, h, v, z);
        for (y = 0; y < h; y++) out[(size_t)y * w + x] = d[y];
    }
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) f[x] = out[(size_t)y * w + x];
        ygfx__edt1(f, d, w, v, z);
        for (x = 0; x < w; x++) out[(size_t)y * w + x] = d[x];
    }
}

YGFX_API int ygfx_sdf_from_mask(float* out, const uint8_t* mask, int w, int h, int pad) {
    int W = w + 2 * pad, H = h + 2 * pad, n, x, y;
    size_t N;
    unsigned char* in;
    double *din, *dout, *f, *d, *z;
    int* v;
    if (!out || !mask || w <= 0 || h <= 0 || pad < 0 || W > 16384 || H > 16384) return YGFX_ERR_ARG;
    N = (size_t)W * (size_t)H;
    n = W > H ? W : H;
    in = (unsigned char*)malloc(N);
    din = (double*)malloc(2 * N * sizeof(double));
    f = (double*)malloc(((size_t)n * 3 + 2) * sizeof(double));
    v = (int*)malloc((size_t)n * sizeof(int));
    if (!in || !din || !f || !v) { free(in); free(din); free(f); free(v); return YGFX_ERR_FULL; }
    dout = din + N;
    d = f + n;
    z = d + n;
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++) {
            int mx = x - pad, my = y - pad;
            in[(size_t)y * W + x] = (unsigned char)(mx >= 0 && my >= 0 && mx < w && my < h && mask[(size_t)my * w + mx] >= 128);
        }
    ygfx__edt2(in, din, W, H, f, d, v, z);          /* to the nearest inside texel */
    for (x = 0; x < (int)N; x++) in[x] = (unsigned char)!in[x];
    ygfx__edt2(in, dout, W, H, f, d, v, z);         /* to the nearest outside texel */
    for (x = 0; x < (int)N; x++) {
        /* the boundary lies halfway between an inside and an outside texel
         * center; a texel with none of one kind gets the other side's 0.5 */
        double a = din[x] < 1e19 ? sqrt(din[x]) : 0.0, b = dout[x] < 1e19 ? sqrt(dout[x]) : 0.0;
        out[x] = in[x] ? (float)(a - 0.5) : (float)(0.5 - b);
    }
    free(in); free(din); free(f); free(v);
    return YGFX_OK;
}

/* --- 3 x 3 (the calibration's math is ysp/color.h's) ------------------------ */

static void ygfx__mul3(const double* m, const double* v, double* out) {
    double t0 = m[0] * v[0] + m[1] * v[1] + m[2] * v[2];
    double t1 = m[3] * v[0] + m[4] * v[1] + m[5] * v[2];
    double t2 = m[6] * v[0] + m[7] * v[1] + m[8] * v[2];
    out[0] = t0; out[1] = t1; out[2] = t2;
}

/* --- v0.3: the vector program's data and its CPU form ----------------------- */

/* The vector program's block, past the vertex shader's two vec4:
 *   2  opacity, gate, offset px, the band's center offset px
 *   3  color rgb, the band's half width px (0: a fill)
 *   4  dash px, gap px, dash offset px, the boundary's length L px
 *   5  kind, edge profile, edge width px, flags (YGFX__VF_*)
 *   6  trim start px, trim end px, cap, primitives n
 *   7  the second point's offset px (local), onion px, points 1 or 2
 *   8  fx offset, paint offset (vec4 into the data region), -, path half width
 *   9  the data region: 4 vec4 per primitive, then the path, fx and paint;
 *      it runs on into the extension blocks (16 vec4 each, at most 15)
 * A primitive: (center px, cos, sin) (kind, op, k px, onion px)
 * (C: 4 parameters px) (D: 2 parameters, geometry offset, count). */
#define YGFX__VDATA 247
#define YGFX__VF_DASH   1
#define YGFX__VF_TRIM   2
#define YGFX__VF_SMOOTH 4
#define YGFX__VF_S      8
#define YGFX__VF_PAINT  16
#define YGFX__VF_FX     32
#define YGFX__VF_RBLUR  64
#define YGFX__PI  3.14159265358979323846
#define YGFX__TAU 6.28318530717958647692

/* The specialized program for a vector stimulus packed at b, or pipe. */
static uint32_t ygfx__vspec_pick(const ygfx_gfx* g, const float* b, uint32_t pipe) {
    const float* vd = b + 36;
    int fl = (int)b[23], np = (int)b[27], kind = (int)vd[4], tier, i;
    double ex = (double)b[4] + b[7], ey = (double)b[5] + b[7];
    if (4.0 * ex * ey < ygfx__vspec_area || (fl & YGFX__VF_PAINT)) return pipe;
    if ((int)b[20] == YGFX_COMPOUND) {
        for (i = 1; i < np; i++) if ((int)vd[16 * i + 4] != kind) return pipe;
        tier = YGFX__VS_FOLD;
    } else {
        if (fl & YGFX__VF_FX) return pipe;
        tier = (fl & (YGFX__VF_DASH | YGFX__VF_TRIM)) ? YGFX__VS_ALONG : YGFX__VS_PLAIN;
    }
    for (i = 0; i < YGFX__N_VSPEC; i++)
        if (ygfx__vspecs[i].kind == kind && ygfx__vspecs[i].tier == tier && g->vspec[i]) return g->vspec[i];
    return pipe;
}

/* The quadrature of the shader's arc lengths: 16-point Gauss-Legendre. */
static const double ygfx__glx[8] = { 0.0950125098376374, 0.2816035507792589, 0.4580167776572274, 0.6178762444026438,
                                       0.7554044083550030, 0.8656312023878318, 0.9445750230732326, 0.9894009349916499 };
static const double ygfx__glw[8] = { 0.1894506104550685, 0.1826034150449236, 0.1691565193950025, 0.1495959888165767,
                                       0.1246289712555339, 0.0951585116824928, 0.0622535239386479, 0.0271524594117541 };

static double ygfx__sgn(double x) { return x >= 0.0 ? 1.0 : -1.0; }
static double ygfx__clampd(double x, double a, double b) { return x < a ? a : (x > b ? b : x); }
static double ygfx__ang(double x, double y) {
    double a;
    if (x == 0.0 && y == 0.0) return 0.0;
    a = atan2(y, x);
    return a < 0.0 ? a + YGFX__TAU : a;
}
static void ygfx__nrm(double vx, double vy, double fx, double fy, double* ox, double* oy) {
    double l = sqrt(vx * vx + vy * vy);
    if (l > 1e-300) { *ox = vx / l; *oy = vy / l; } else { *ox = fx; *oy = fy; }
}
static void ygfx__set4(double e[4], double a, double b, double c, double d) { e[0] = a; e[1] = b; e[2] = c; e[3] = d; }

/* Each primitive in its own frame: e = distance, gradient x, y, arc length.
 * The CPU forms of the shader's ysp_p_*_: the sources are named there
 * (Inigo Quilez, MIT: box, rounded box, uneven capsule, arc, pie, star,
 * smooth minimum; 0xfaded, MIT: ellipse). */
static void ygfx__vp_box(double qx, double qy, double bx, double by, double e[4]) {
    double wx = fabs(qx) - bx, wy = fabs(qy) - by, sx = ygfx__sgn(qx), sy = ygfx__sgn(qy);
    if (wx > 0.0 && wy > 0.0) { double l = sqrt(wx * wx + wy * wy); ygfx__set4(e, l, sx * wx / l, sy * wy / l, 0.0); return; }
    if (wx >= wy) ygfx__set4(e, wx, sx, 0.0, 0.0); else ygfx__set4(e, wy, 0.0, sy, 0.0);
}

/* after Inigo Quilez, sdRoundBox (MIT) */
static void ygfx__vp_rrect(double qx, double qy, double bx, double by, const double r[4], int ws, double e[4]) {
    int rt = qx > 0.0, bt = qy > 0.0, cn;
    double rr = rt ? (bt ? r[2] : r[1]) : (bt ? r[3] : r[0]);
    double sx = rt ? 1.0 : -1.0, sy = bt ? 1.0 : -1.0, wx = fabs(qx) - bx + rr, wy = fabs(qy) - by + rr;
    cn = wx > 0.0 && wy > 0.0;
    if (cn) { double l = sqrt(wx * wx + wy * wy); ygfx__set4(e, l - rr, sx * wx / l, sy * wy / l, 0.0); }
    else if (wx >= wy) ygfx__set4(e, wx - rr, sx, 0.0, 0.0);
    else ygfx__set4(e, wy - rr, 0.0, sy, 0.0);
    if (ws) {
        double h = 0.5 * YGFX__PI, lt = 2.0 * bx - r[0] - r[1], lr = 2.0 * by - r[1] - r[2];
        double lb = 2.0 * bx - r[2] - r[3], ll = 2.0 * by - r[3] - r[0];
        double sR = lt + h * r[1], sB = sR + lr + h * r[2], sL = sB + lb + h * r[3];
        if (cn) {
            if (rt && !bt) e[3] = lt + rr * atan2(wx, wy);
            else if (rt) e[3] = sR + lr + rr * atan2(wy, wx);
            else if (bt) e[3] = sB + lb + rr * atan2(wx, wy);
            else e[3] = sL + ll + rr * atan2(wy, wx);
        } else if (wx >= wy) {
            e[3] = rt ? sR + ygfx__clampd(qy + by - r[1], 0.0, lr) : sL + ygfx__clampd(by - r[3] - qy, 0.0, ll);
        } else {
            e[3] = bt ? sB + ygfx__clampd(bx - r[2] - qx, 0.0, lb) : ygfx__clampd(qx + bx - r[0], 0.0, lt);
        }
    }
}

/* after Inigo Quilez, sdUnevenCapsule (MIT) */
static void ygfx__vp_capsule(double qx, double qy, double hx, double r0, double r1, int ws, double e[4]) {
    double x1 = r0 - hx, D = 2.0 * hx - r0 - r1, sn = (r0 - r1) / D, cs = sqrt(fmax(1.0 - sn * sn, 0.0));
    double al = qx - x1, k = -sn * fabs(qy) + cs * al, gx, gy;
    if (k < 0.0) { ygfx__nrm(al, qy, -1.0, 0.0, &gx, &gy); ygfx__set4(e, sqrt(al * al + qy * qy) - r0, gx, gy, 0.0); }
    else if (k > cs * D) { ygfx__nrm(al - D, qy, 1.0, 0.0, &gx, &gy); ygfx__set4(e, sqrt((al - D) * (al - D) + qy * qy) - r1, gx, gy, 0.0); }
    else ygfx__set4(e, cs * fabs(qy) + sn * al - r0, sn, ygfx__sgn(qy) * cs, 0.0);
    if (ws) e[3] = ygfx__clampd(al, 0.0, D);
}

/* The arc and the pie are folded about their middle direction u; their
 * trig comes from the CPU (u, and sin, cos of the half sweep), because a
 * D3D shader's sin and cos may be coarse (measured: 0.01 px off at radius
 * 75 on WARP). IQ's sdArc and sdPie in that frame. */
/* The clockwise angle from the start, measured from the middle direction
 * so that no angle wraps through 2 pi; past either end, that end. */
static double ygfx__sweep(double pv, double pu, double sw) {
    return ygfx__clampd(0.5 * sw + ((pv == 0.0 && pu == 0.0) ? 0.0 : atan2(pv, pu)), 0.0, sw);
}

/* after Inigo Quilez, sdArc (MIT) */
static void ygfx__vp_arc(double qx, double qy, double ux, double uy, double ra, double rb, double sw, double scx, double scy,
                           int ws, double e[4]) {
    double vx = -uy, vy = ux, pv = qx * vx + qy * vy, px = fabs(pv), py = qx * ux + qy * uy, d, gx, gy;
    if (scy * px > scx * py) { double wx = px - scx * ra, wy = py - scy * ra; d = sqrt(wx * wx + wy * wy) - rb; ygfx__nrm(wx, wy, scx, scy, &gx, &gy); }
    else { double l = sqrt(px * px + py * py), sg = ygfx__sgn(l - ra); d = fabs(l - ra) - rb; ygfx__nrm(px, py, 0.0, 1.0, &gx, &gy); gx *= sg; gy *= sg; }
    gx *= ygfx__sgn(pv);
    ygfx__set4(e, d, gx * vx + gy * ux, gx * vy + gy * uy,
                 ws ? ra * ygfx__sweep(pv, py, sw) : 0.0);
}

/* after Inigo Quilez, sdPie (MIT) */
static void ygfx__vp_pie(double qx, double qy, double R, double ux, double uy, double sw, double cx, double cy, int ws, double e[4]) {
    double vx = -uy, vy = ux, pv = qx * vx + qy * vy, px = fabs(pv), py = qx * ux + qy * uy;
    double l = sqrt(px * px + py * py) - R, pr = ygfx__clampd(px * cx + py * cy, 0.0, R);
    double mx = px - cx * pr, my = py - cy * pr, sg = ygfx__sgn(cy * px - cx * py), mm = sqrt(mx * mx + my * my) * sg, d, gx, gy;
    int arc = l > mm;
    if (arc) { d = l; ygfx__nrm(px, py, 0.0, 1.0, &gx, &gy); }
    else { d = mm; ygfx__nrm(mx, my, cy, -cx, &gx, &gy); gx *= sg; gy *= sg; }
    gx *= ygfx__sgn(pv);
    ygfx__set4(e, d, gx * vx + gy * ux, gx * vy + gy * uy, 0.0);
    if (ws) e[3] = arc ? R + R * ygfx__sweep(pv, py, sw) : (pv < 0.0 ? pr : 2.0 * R + R * sw - pr);
}

/* A star's constants in its folded half sector, in double: half a
 * sector's boundary length H and the inner fillet's arc length lI. */
static void ygfx__star_consts(double R, double n, double rho, double rf, double* H, double* lI) {
    double an = YGFX__PI / n, Tx = R * cos(an), Ty = R * sin(an), Ix = R * rho;
    double LE = sqrt((Tx - Ix) * (Tx - Ix) + Ty * Ty), ex = (Tx - Ix) / LE, ey = Ty / LE;
    double cb = ex * cos(an) + ey * sin(an), sbe = fabs(ex * sin(an) - ey * cos(an)), tI = 0.0;
    *lI = 0.0;
    if (rf <= 0.0) { *H = LE; return; }
    if (fabs(ex) > 1e-5) { tI = rf * fabs(ex) / fabs(ey); *lI = rf * atan2(fabs(ex), fabs(ey)); }
    *H = *lI + (LE - tI - rf * cb / sbe) + rf * atan2(cb, sbe);
}

/* A star or a regular polygon (rho = cos(pi / n)) with fillets of radius rf
 * at every corner, folded into half a sector: the inner corner I on the
 * folded x axis, the tip T at angle pi / n (cos, sin in D[0], D[1]). The
 * sector's rotation is a power of a unit complex number, not a sin and cos
 * per pixel; a point the rounded sector index puts past the tip line is
 * reflected back across it. The fold is after Inigo Quilez's sdStar (MIT). */
static void ygfx__vp_star(double qx, double qy, const float* C, const float* D, int ws, double e[4]) {
    double R = C[0], n = C[1], rho = C[2], rf = C[3], an = YGFX__PI / n, Thx = D[0], Thy = D[1];
    double al = (qx == 0.0 && qy == 0.0) ? 0.0 : atan2(qx, qy), kf = floor((al + an) / (2.0 * an));
    double zx = Thx * Thx - Thy * Thy, zy = 2.0 * Thx * Thy, wx2 = 1.0, wy2 = 0.0, fys, sb, fx, fy;
    double Ix, Tx, Ty, ex, ey, nex, ney, LE, PIx, PIy, PTx, PTy, gfx, gfy, best = 1e300, dv = 0.0, sh = 0.0, lI = D[3], H = D[2];
    double LP, t, wx, wy, l;
    int kk = (int)fabs(kf), b, rfl;
    for (b = 0; b < 7; b++) {
        if (kk & 1) { double tx = wx2 * zx - wy2 * zy; wy2 = wx2 * zy + wy2 * zx; wx2 = tx; }
        { double tz = zx * zx - zy * zy; zy = 2.0 * zx * zy; zx = tz; }
        kk /= 2;
    }
    if (kf < 0.0) wy2 = -wy2;
    fys = qx * wx2 - qy * wy2; sb = ygfx__sgn(fys);
    fx = qx * wy2 + qy * wx2; fy = fabs(fys);
    rfl = fx * Thy - fy * Thx < 0.0;
    if (rfl) { double k2 = 2.0 * (fx * Thx + fy * Thy); fx = k2 * Thx - fx; fy = k2 * Thy - fy; }
    Ix = R * rho; Tx = R * Thx; Ty = R * Thy;
    LE = sqrt((Tx - Ix) * (Tx - Ix) + Ty * Ty); ex = (Tx - Ix) / LE; ey = Ty / LE; nex = ey; ney = -ex;
    PIx = Ix; PIy = 0.0; PTx = Tx; PTy = Ty; gfx = nex; gfy = ney;
    if (rf > 0.0) {
        double cb = ex * Thx + ey * Thy, sbe = fabs(ex * Thy - ey * Thx), CTx, CTy, lw, cr;
        CTx = Tx - Thx * rf / sbe; CTy = Ty - Thy * rf / sbe;
        PTx = Tx - ex * rf * cb / sbe; PTy = Ty - ey * rf * cb / sbe;
        if (fabs(ex) > 1e-5) {
            double cv = ex > 0.0 ? -1.0 : 1.0, CIx = Ix - cv * rf / fabs(ey), CIy = 0.0, a1x = cv, a1y = 0.0, a2x, a2y;
            PIx = Ix + ex * rf * fabs(ex) / fabs(ey); PIy = ey * rf * fabs(ex) / fabs(ey);
            a2x = (PIx - CIx) / rf; a2y = (PIy - CIy) / rf;
            wx = fx - CIx; wy = fy - CIy; lw = sqrt(wx * wx + wy * wy);
            cr = a1x * a2y - a1y * a2x;
            if ((a1x * wy - a1y * wx) * cr >= -1e-5 * lw && (wx * a2y - wy * a2x) * cr >= -1e-5 * lw && lw > 0.0) {
                dv = cv * (lw - rf); best = fabs(dv); gfx = cv * wx / lw; gfy = cv * wy / lw;
                sh = rf * fabs(atan2(a1x * wy - a1y * wx, a1x * wx + a1y * wy));
            }
        }
        wx = fx - CTx; wy = fy - CTy; lw = sqrt(wx * wx + wy * wy);
        cr = nex * Thy - ney * Thx;
        if ((nex * wy - ney * wx) * cr >= -1e-5 * lw && (wx * Thy - wy * Thx) * cr >= -1e-5 * lw && lw > 0.0 && fabs(lw - rf) < best) {
            dv = lw - rf; best = fabs(dv); gfx = wx / lw; gfy = wy / lw;
            sh = lI + sqrt((PTx - PIx) * (PTx - PIx) + (PTy - PIy) * (PTy - PIy)) + rf * fabs(atan2(nex * wy - ney * wx, nex * wx + ney * wy));
        }
    }
    LP = sqrt((PTx - PIx) * (PTx - PIx) + (PTy - PIy) * (PTy - PIy));
    t = ygfx__clampd((fx - PIx) * ex + (fy - PIy) * ey, 0.0, LP);
    wx = fx - PIx - ex * t; wy = fy - PIy - ey * t; l = sqrt(wx * wx + wy * wy);
    if (l < best) {
        double sg = wx * nex + wy * ney >= 0.0 ? 1.0 : -1.0;
        dv = sg * l; sh = lI + t;
        if (l > 0.0) { gfx = sg * wx / l; gfy = sg * wy / l; } else { gfx = nex; gfy = ney; }
    }
    if (rfl) { double k2 = 2.0 * (gfx * Thx + gfy * Thy); gfx = k2 * Thx - gfx; gfy = k2 * Thy - gfy; sh = 2.0 * H - sh; }
    e[0] = dv;
    e[1] = gfx * wy2 + gfy * sb * wx2;
    e[2] = gfx * wx2 - gfy * sb * wy2;
    e[3] = 0.0;
    if (ws) {
        double P = 2.0 * H, s2 = fmod(fmod(n - kf, n) + n, n) * P + (sb < 0.0 ? sh : -sh);
        e[3] = s2 - n * P * floor(s2 / (n * P));
    }
}

/* 0xfaded's trig-free closest point on an ellipse (MIT, see the end of the
 * file), in the first quadrant; the arc length by the shader's quadrature. */
static void ygfx__vp_ellipse(double qx, double qy, double a, double b, int it, double Lq, int ws, double e[4]) {
    double px = fabs(qx), py = fabs(qy), tx = 0.70710678118654752, ty = 0.70710678118654752;
    double k2x = (a * a - b * b) / a, k2y = (b * b - a * a) / b, wx, wy, sg, nx, ny, gx, gy;
    int i;
    for (i = 0; i < it && i < 8; i++) {
        double evx = k2x * tx * tx * tx, evy = k2y * ty * ty * ty, rx = a * tx - evx, ry = b * ty - evy;
        double qqx = px - evx, qqy = py - evy, rl = sqrt(rx * rx + ry * ry), ql = fmax(sqrt(qqx * qqx + qqy * qqy), 1e-300), tl;
        tx = ygfx__clampd((qqx * rl / ql + evx) / a, 0.0, 1.0);
        ty = ygfx__clampd((qqy * rl / ql + evy) / b, 0.0, 1.0);
        tl = fmax(sqrt(tx * tx + ty * ty), 1e-300);
        tx /= tl; ty /= tl;
    }
    wx = px - a * tx; wy = py - b * ty;
    sg = (px / a) * (px / a) + (py / b) * (py / b) < 1.0 ? -1.0 : 1.0;
    ygfx__nrm(tx / a, ty / b, 1.0, 0.0, &nx, &ny);
    ygfx__nrm(wx, wy, sg * nx, sg * ny, &gx, &gy);
    ygfx__set4(e, sg * sqrt(wx * wx + wy * wy), sg * gx * ygfx__sgn(qx), sg * gy * ygfx__sgn(qy), 0.0);
    if (ws) {
        double t1 = atan2(ty, tx), h = 0.5 * t1, S = 0.0;
        int j;
        for (i = 0; i < 8; i++)
            for (j = 0; j < 2; j++) {
                double u = h * (1.0 + (j == 0 ? ygfx__glx[i] : -ygfx__glx[i]));
                S += ygfx__glw[i] * sqrt(a * a * sin(u) * sin(u) + b * b * cos(u) * cos(u));
            }
        S *= h;
        e[3] = qx >= 0.0 ? (qy >= 0.0 ? S : 4.0 * Lq - S) : (qy >= 0.0 ? 2.0 * Lq - S : 2.0 * Lq + S);
    }
}

/* The quarter of an ellipse's perimeter, in double: a E(e) by the
 * arithmetic-geometric mean, pi / (2 M(a, b)) (a^2 - sum 2^(n-1) c_n^2).
 * It converges quadratically: 5 steps reach double precision up to 20:1.
 * Within 9e-16 (relative) of a long-double Simpson reference, and the same
 * float as v0.6.0's 1024-point quadrature at every aspect tested, at 0.02
 * us a call against 23 to 50 us (docs/gfx.md, v0.7). */
static double ygfx__ell_quarter(double a, double b) {
    double big = a > b ? a : b, an = big, gn = a > b ? b : a, p2 = 0.5, sum;
    int i;
    sum = 0.5 * (an * an - gn * gn);
    for (i = 0; i < 8; i++) {
        double c = 0.5 * (an - gn), g = sqrt(an * gn);
        an = 0.5 * (an + gn); gn = g;
        p2 *= 2.0;
        sum += p2 * c * c;
        if (c == 0.0) break;
    }
    return YGFX__PI / (2.0 * an) * (big * big - sum);
}

/* The data region as floats: vec4 k at vd + 4 k. */
#define YGFX__VD(vd, k, c) ((double)(vd)[4 * (k) + (c)])

static void ygfx__vp_polygon(double qx, double qy, const float* vd, int g0, int n, int ws, double e[4]) {
    double best = 1e300, dv = 0.0, s = 0.0, par = 1.0, o = YGFX__VD(vd, g0 + 2, 1), gbx = 1.0, gby = 0.0;
    int sharp = fabs(o) > 1.5, i;
    o = ygfx__sgn(o);
    for (i = 0; i < n && i < 16; i++) {
        int j = i + 1 < n ? i + 1 : 0;
        const float* A = vd + 4 * (g0 + 3 * i);
        const float* B = A + 4;
        const float* C = A + 8;
        const float* An = vd + 4 * (g0 + 3 * j);
        double evx, evy, le, ex, ey, nex, ney, t, wx, wy, l;
        if (B[2] != 0.0f) {
            double r = fabs((double)B[2]), cv = ygfx__sgn(B[2]);
            double ww_x = qx - B[0], ww_y = qy - B[1], a1x = (A[0] - B[0]) / r, a1y = (A[1] - B[1]) / r;
            double a2x = (A[2] - B[0]) / r, a2y = (A[3] - B[1]) / r, cr = a1x * a2y - a1y * a2x, lw = sqrt(ww_x * ww_x + ww_y * ww_y);
            if ((a1x * ww_y - a1y * ww_x) * cr >= -1e-5 * lw && (ww_x * a2y - ww_y * a2x) * cr >= -1e-5 * lw && lw > 0.0 && fabs(lw - r) < best) {
                double sg2 = ygfx__sgn(lw - r);
                dv = cv * (lw - r); best = fabs(dv); gbx = sg2 * ww_x / lw; gby = sg2 * ww_y / lw;
                s = B[3] + r * fabs(atan2(a1x * ww_y - a1y * ww_x, a1x * ww_x + a1y * ww_y));
            }
        }
        evx = (double)An[0] - A[2]; evy = (double)An[1] - A[3];
        le = sqrt(evx * evx + evy * evy);
        ex = evx / fmax(le, 1e-300); ey = evy / fmax(le, 1e-300); nex = o * ey; ney = -o * ex;
        t = ygfx__clampd((qx - A[2]) * ex + (qy - A[3]) * ey, 0.0, le);
        wx = qx - A[2] - ex * t; wy = qy - A[3] - ey * t; l = sqrt(wx * wx + wy * wy);
        if (l < best) {
            double sg = wx * nex + wy * ney >= 0.0 ? 1.0 : -1.0;
            best = l; dv = sg * l; s = C[0] + t;
            if (l > 0.0) { gbx = wx / l; gby = wy / l; } else { gbx = nex; gby = ney; }
        }
        if (sharp) {
            double vix = C[2], viy = C[3], vjx = vd[4 * (g0 + 3 * j + 2) + 2], vjy = vd[4 * (g0 + 3 * j + 2) + 3];
            double eex = vjx - vix, eey = vjy - viy, wwx = qx - vix, wwy = qy - viy;
            int c1 = qy >= viy, c2 = qy < vjy, c3 = eex * wwy > eey * wwx;
            if ((c1 && c2 && c3) || (!c1 && !c2 && !c3)) par = -par;
        }
    }
    if (sharp) { dv = par * best; gbx *= par; gby *= par; }
    else { gbx *= ygfx__sgn(dv); gby *= ygfx__sgn(dv); }
    ygfx__set4(e, dv, gbx, gby, ws ? s : 0.0);
}

/* A convex polygon of n = 3 or 4 vertices, exact. */
static void ygfx__vp_cpoly(double px, double py, const double* v, int n, double e[4]) {
    double ar = 0.0, best = 1e300, bx = 0.0, by = 0.0, l, sg;
    int i, ins = 1;
    for (i = 0; i < n; i++) { int j = i + 1 < n ? i + 1 : 0; ar += v[2 * i] * v[2 * j + 1] - v[2 * i + 1] * v[2 * j]; }
    for (i = 0; i < n; i++) {
        int j = i + 1 < n ? i + 1 : 0;
        double ux = v[2 * i], uy = v[2 * i + 1], ex = v[2 * j] - ux, ey = v[2 * j + 1] - uy, qx = px - ux, qy = py - uy;
        double h = ygfx__clampd((qx * ex + qy * ey) / fmax(ex * ex + ey * ey, 1e-300), 0.0, 1.0), rx = qx - ex * h, ry = qy - ey * h;
        if (rx * rx + ry * ry < best) { best = rx * rx + ry * ry; bx = rx; by = ry; }
        if (ar * (ex * qy - ey * qx) < 0.0) ins = 0;
    }
    l = sqrt(best); sg = ins ? -1.0 : 1.0;
    ygfx__set4(e, sg * l, l > 0.0 ? sg * bx / l : 0.0, l > 0.0 ? sg * by / l : 0.0, 0.0);
}

/* Round joins: the distance to the center line, exact (a union of short
 * boxes is only a bound inside, and a chain of Bezier chords is short
 * boxes). Miter and bevel joins: boxes and join polygons, a bound inside
 * near each turn's inner corner. */
static void ygfx__vp_polyline(double px, double py, const float* vd, int g0, int n, double hw, int join, double lim,
                                int cap, int ws, double best[4]) {
    int i, bi = 0;
    double bh = 0.5;
    ygfx__set4(best, 1e300, 1.0, 0.0, 0.0);
    for (i = 0; i < n - 1 && i < YGFX_MAX_PATH - 1; i++) {
        const float* A = vd + 4 * (g0 + i);
        const float* B = A + 4;
        double tvx = (double)B[0] - A[0], tvy = (double)B[1] - A[1], len = sqrt(tvx * tvx + tvy * tvy);
        double tx = tvx / fmax(len, 1e-300), ty = tvy / fmax(len, 1e-300), nx = -ty, ny = tx, rx = px - A[0], ry = py - A[1];
        double al = rx * tx + ry * ty, e0, e1, bx[4];
        if (join == 0) {
            double h = ygfx__clampd(al, 0.0, len), wx = rx - tx * h, wy = ry - ty * h, l = sqrt(wx * wx + wy * wy), gx, gy;
            if (l - hw < best[0]) {
                ygfx__nrm(wx, wy, nx, ny, &gx, &gy);
                ygfx__set4(best, l - hw, gx, gy, A[2] + h);
                bh = al / fmax(len, 1e-300); bi = i;
            }
            continue;
        }
        e0 = (i == 0 && cap == 2) ? hw : 0.0; e1 = (i == n - 2 && cap == 2) ? hw : 0.0;
        ygfx__vp_box(al - 0.5 * (len + e1 - e0), rx * nx + ry * ny, 0.5 * (len + e0 + e1), hw, bx);
        if (bx[0] < best[0]) ygfx__set4(best, bx[0], bx[1] * tx + bx[2] * nx, bx[1] * ty + bx[2] * ny, A[2] + ygfx__clampd(al, 0.0, len));
        if (i < n - 2) {
            const float* Cn = A + 8;
            double t2x = (double)Cn[0] - B[0], t2y = (double)Cn[1] - B[1], tl = sqrt(t2x * t2x + t2y * t2y), cr;
            t2x /= tl; t2y /= tl;
            cr = tx * t2y - ty * t2x;
            if (fabs(cr) > 1e-6 || tx * t2x + ty * t2y < 0.0) {
                double sd = cr > 0.0 ? -1.0 : 1.0, nax = sd * nx, nay = sd * ny, nbx = -sd * t2y, nby = sd * t2x;
                double mx = nax + nbx, my = nay + nby, mm = mx * mx + my * my, v[8], jp[4];
                int mit = join == 1 && mm > 1e-12 && 2.0 / sqrt(mm) <= lim;
                v[0] = B[0]; v[1] = B[1]; v[2] = B[0] + nax * hw; v[3] = B[1] + nay * hw;
                v[6] = B[0] + nbx * hw; v[7] = B[1] + nby * hw;
                if (mit) { v[4] = B[0] + mx * (2.0 * hw / mm); v[5] = B[1] + my * (2.0 * hw / mm); }
                else { v[4] = v[6]; v[5] = v[7]; }
                ygfx__vp_cpoly(px, py, v, 4, jp);
                if (jp[0] < best[0]) ygfx__set4(best, jp[0], jp[1], jp[2], B[2]);
            }
        }
    }
    if (join == 0 && cap != 1 && ((bi == 0 && bh <= 0.0) || (bi == n - 2 && bh >= 1.0))) {
        int st = bi == 0 && bh <= 0.0;
        const float* E = vd + 4 * (g0 + (st ? 0 : n - 1));
        const float* F = vd + 4 * (g0 + (st ? 1 : n - 2));
        double tox, toy, nx, ny, rx = px - E[0], ry = py - E[1], ac, qdx, qdy, ggx, ggy;
        ygfx__nrm((double)E[0] - F[0], (double)E[1] - F[1], 1.0, 0.0, &tox, &toy);
        nx = -toy; ny = tox;
        ac = rx * nx + ry * ny;
        qdx = rx * tox + ry * toy - (cap == 2 ? hw : 0.0); qdy = fabs(ac) - hw;
        if (qdx > 0.0 && qdy > 0.0) { double q = sqrt(qdx * qdx + qdy * qdy); ggx = qdx / q; ggy = qdy / q; }
        else if (qdx >= qdy) { ggx = 1.0; ggy = 0.0; } else { ggx = 0.0; ggy = 1.0; }
        best[0] = sqrt(fmax(qdx, 0.0) * fmax(qdx, 0.0) + fmax(qdy, 0.0) * fmax(qdy, 0.0)) + fmin(fmax(qdx, qdy), 0.0);
        best[1] = ggx * tox + ggy * ygfx__sgn(ac) * nx;
        best[2] = ggx * toy + ggy * ygfx__sgn(ac) * ny;
    }
    if (join != 0 && cap == 1) {
        int k;
        for (k = 0; k < 2; k++) {
            const float* E = vd + 4 * (g0 + (k == 0 ? 0 : n - 1));
            double vx = px - E[0], vy = py - E[1], l = sqrt(vx * vx + vy * vy), gx, gy;
            if (l - hw < best[0]) { ygfx__nrm(vx, vy, 1.0, 0.0, &gx, &gy); ygfx__set4(best, l - hw, gx, gy, E[2]); }
        }
    }
    if (!ws) best[3] = 0.0;
}

/* The shader's ysp_prim_, in double. */
static void ygfx__vprim(double qx, double qy, const float* B, const float* C, const float* D, const float* vd, int ws, double e[4]) {
    int k = (int)B[0];
    switch (k) {
    case 1: {
        double R = C[0], l = sqrt(qx * qx + qy * qy), gx, gy;
        ygfx__nrm(qx, qy, 1.0, 0.0, &gx, &gy);
        ygfx__set4(e, l - R, gx, gy, ws ? R * ygfx__ang(qx, qy) : 0.0);
        return;
    }
    case 2: {
        double R = C[0], ri = C[2], l = sqrt(qx * qx + qy * qy), sg = ygfx__sgn(l - 0.5 * (R + ri)), gx, gy;
        ygfx__nrm(qx, qy, 1.0, 0.0, &gx, &gy);
        ygfx__set4(e, fabs(l - 0.5 * (R + ri)) - 0.5 * (R - ri), sg * gx, sg * gy,
                     !ws ? 0.0 : (sg > 0.0 ? R * ygfx__ang(qx, qy) : YGFX__TAU * R + ri * ygfx__ang(qx, qy)));
        return;
    }
    case 8: {
        double r[4];
        r[0] = C[2]; r[1] = C[3]; r[2] = D[0]; r[3] = D[1];
        ygfx__vp_rrect(qx, qy, C[0], C[1], r, ws, e);
        return;
    }
    case 11: ygfx__vp_capsule(qx, qy, C[0], C[2], C[3], ws, e); return;
    case 9:  ygfx__vp_arc(qx, qy, C[0], C[1], C[2], C[3], D[0], D[2], D[3], ws, e); return;
    case 10: ygfx__vp_pie(qx, qy, C[0], C[1], C[2], D[0], D[2], D[3], ws, e); return;
    case 13: ygfx__vp_star(qx, qy, C, D, ws, e); return;
    case 14: ygfx__vp_ellipse(qx, qy, C[0], C[1], (int)C[2], C[3], ws, e); return;
    case 5: {
        double a[4], b[4];
        ygfx__vp_box(qx, qy, C[0], 0.5 * C[2], a);
        ygfx__vp_box(qx, qy, 0.5 * C[2], C[1], b);
        if (a[0] < b[0]) memcpy(e, a, sizeof a); else memcpy(e, b, sizeof b);
        return;
    }
    case 4:  ygfx__vp_polygon(qx, qy, vd, (int)D[2], (int)D[3], ws, e); return;
    case 16: ygfx__vp_polyline(qx, qy, vd, (int)D[2], (int)D[3], C[2], (int)C[3], D[0], (int)D[1], ws, e); return;
    default: ygfx__set4(e, 1e300, 1.0, 0.0, 0.0); return;
    }
}

/* Inigo Quilez's quadratic smooth minimum (MIT), with its gradient */
static void ygfx__vsmin(const double a[4], const double b[4], double k, double r[4]) {
    double h = ygfx__clampd(0.5 + 0.5 * (b[0] - a[0]) / k, 0.0, 1.0);
    r[0] = b[0] + (a[0] - b[0]) * h - k * h * (1.0 - h);
    r[1] = b[1] + (a[1] - b[1]) * h;
    r[2] = b[2] + (a[2] - b[2]) * h;
    r[3] = h > 0.5 ? a[3] : b[3];
}

static void ygfx__vop(double a[4], const double e[4], const float* B) {
    int op = (int)B[1], i;
    double k = B[2] > 1e-6f ? (double)B[2] : 1e-6, ne[4], na[4], r[4];
    ne[0] = -e[0]; ne[1] = -e[1]; ne[2] = -e[2]; ne[3] = e[3];
    na[0] = -a[0]; na[1] = -a[1]; na[2] = -a[2]; na[3] = a[3];
    switch (op) {
    case 0: if (e[0] < a[0]) memcpy(a, e, 4 * sizeof(double)); return;
    case 1: if (e[0] > a[0]) memcpy(a, e, 4 * sizeof(double)); return;
    case 2: if (ne[0] > a[0]) memcpy(a, ne, sizeof ne); return;
    case 3: {
        double u[4], v[4];
        memcpy(u, e[0] < a[0] ? e : a, sizeof u);
        memcpy(v, e[0] < a[0] ? a : e, sizeof v);
        if (u[0] > -v[0]) memcpy(a, u, sizeof u);
        else { a[0] = -v[0]; a[1] = -v[1]; a[2] = -v[2]; a[3] = v[3]; }
        return;
    }
    case 4: ygfx__vsmin(a, e, k, r); memcpy(a, r, sizeof r); return;
    default:
        ygfx__vsmin(na, op == 5 ? ne : e, k, r);
        for (i = 0; i < 3; i++) a[i] = -r[i];
        a[3] = r[3];
        return;
    }
}

/* The shader's field at local point p (and p minus the second point's
 * offset), folded, normalized, onioned: f[0] is what the edge sees before
 * the offset. b is the stimulus's block, its data region following. */
static void ygfx__vfield(const float* b, double px, double py, double f0[4], double f1[4]) {
    const float* vd = b + 36;
    int n = (int)b[27], np = (int)b[31], ws = ((int)b[23] & YGFX__VF_S) != 0, i, j;
    ygfx__set4(f0, 1e300, 1.0, 0.0, 0.0);
    ygfx__set4(f1, 1e300, 1.0, 0.0, 0.0);
    for (i = 0; i < n && i < YGFX_MAX_PRIMS; i++) {
        const float* A = vd + 16 * i;
        double ax = A[2], ay = A[3];
        for (j = 0; j < np && j < 2; j++) {
            double rx = (j == 0 ? px : px - b[28]) - A[0], ry = (j == 0 ? py : py - b[29]) - A[1], e[4], gx, gy;
            ygfx__vprim(rx * ax + ry * ay, -rx * ay + ry * ax, A + 4, A + 8, A + 12, vd, ws, e);
            gx = e[1] * ax - e[2] * ay;
            gy = e[1] * ay + e[2] * ax;
            e[1] = gx; e[2] = gy;
            if (A[7] > 0.0f) { double sg = ygfx__sgn(e[0]); e[0] = fabs(e[0]) - A[7]; e[1] *= sg; e[2] *= sg; }
            if (i == 0) memcpy(j == 0 ? f0 : f1, e, sizeof e);
            else ygfx__vop(j == 0 ? f0 : f1, e, A + 4);
        }
    }
    if (np < 2) memcpy(f1, f0, 4 * sizeof(double));
    if ((int)b[23] & YGFX__VF_SMOOTH) {
        f0[0] /= fmax(sqrt(f0[1] * f0[1] + f0[2] * f0[2]), 1e-3);
        f1[0] /= fmax(sqrt(f1[1] * f1[1] + f1[2] * f1[2]), 1e-3);
    }
    if (b[30] > 0.0f) { f0[0] = fabs(f0[0]) - b[30]; f1[0] = fabs(f1[0]) - b[30]; }
}

/* The main layer's hard coverage (where the GPU's is 0.5 or more), on the
 * CPU: the fill or the band, times the dashes and the trim. */
static int ygfx__vhard(const float* b, double px, double py) {
    double f0[4], f1[4], d, hw = b[15], c = b[11], nn, hh, s;
    int fl;
    ygfx__vfield(b, px, py, f0, f1);
    d = f0[0] - b[10];
    fl = (int)b[23];
    if (hw > 0.0 ? !(d - c - hw <= 0.0 && d - c + hw > 0.0) : !(d <= 0.0)) return 0;
    if (!(fl & (YGFX__VF_DASH | YGFX__VF_TRIM))) return 1;
    s = f0[3];
    nn = hw > 0.0 ? d - c : d + b[35];
    hh = hw > 0.0 ? hw : b[35];
    {
        double P = (double)b[16] + b[17], u = (fl & YGFX__VF_DASH) ? s - b[18] - P * floor((s - b[18]) / P) : 0.0;
        int cap = (int)b[26];
        double ex = cap == 2 ? hh : 0.0;
        if (cap != 1) {
            if (fl & YGFX__VF_DASH) {
                int k, in = 0;
                for (k = -1; k <= 1; k++) in |= u + k * P >= -ex && u + k * P <= b[16] + ex;
                if (!in) return 0;
            }
            if ((fl & YGFX__VF_TRIM) && !(s >= b[24] - ex && s <= b[25] + ex)) return 0;
            return 1;
        } else {
            double ga = 0.0;
            if (fl & YGFX__VF_DASH) {
                int k;
                ga = 1e300;
                for (k = -1; k <= 1; k++) ga = fmin(ga, fmax(fabs(u + k * P - 0.5 * b[16]) - 0.5 * b[16], 0.0));
            }
            if (fl & YGFX__VF_TRIM) ga = fmax(ga, fmax(fabs(s - 0.5 * ((double)b[24] + b[25])) - 0.5 * ((double)b[25] - b[24]), 0.0));
            return sqrt(ga * ga + nn * nn) - hh <= 0.0;
        }
    }
}

/* A primitive's kind and parameters into the shader's, px. sp: its four
 * shape parameters (units, degrees, counts). *L: the length dashes run
 * along (px; -1: none), *rad: a bounding radius about its center, *phw:
 * the half width of a path-like kind (dashes then follow its center line). */
static int ygfx__vkind(int shape, double w, double h, const float* sp, double ppu, float C[4], float D[4],
                         double* L, double* rad, double* phw, const char** err) {
    const double hx = 0.5 * w * ppu, hy = 0.5 * h * ppu;
    int i;
    for (i = 0; i < 4; i++) C[i] = D[i] = 0.0f;
    *L = -1.0; *phw = 0.0; *rad = sqrt(hx * hx + hy * hy);
    switch (shape) {
    case YGFX_RECT: case YGFX_RRECT: {
        double r[4], m = hx < hy ? hx : hy, sum = 0.0;
        if (!(hx > 0.0 && hy > 0.0)) { *err = "a rect needs w, h"; return YGFX_ERR_ARG; }
        for (i = 0; i < 4; i++) {
            r[i] = shape == YGFX_RECT ? (double)sp[1] * ppu : (double)sp[i] * ppu;
            r[i] = r[i] < 0.0 ? 0.0 : (r[i] > m ? m : r[i]);
            sum += r[i];
        }
        C[0] = (float)hx; C[1] = (float)hy; C[2] = (float)r[0]; C[3] = (float)r[1]; D[0] = (float)r[2]; D[1] = (float)r[3];
        *L = 4.0 * hx + 4.0 * hy - 2.0 * sum + 0.5 * YGFX__PI * sum;
        return 8;
    }
    case YGFX_CIRCLE:
        if (!(hx > 0.0)) { *err = "a circle needs w"; return YGFX_ERR_ARG; }
        C[0] = (float)hx; *L = YGFX__TAU * hx; *rad = hx;
        return 1;
    case YGFX_ANNULUS: {
        double ri = (double)sp[0] * ppu;
        if (!(ri >= 0.0 && ri < hx)) { *err = "an annulus needs 0 <= inner radius < w / 2"; return YGFX_ERR_ARG; }
        C[0] = (float)hx; C[2] = (float)ri; *L = YGFX__TAU * (hx + ri); *rad = hx;
        return 2;
    }
    case YGFX_LINE: case YGFX_CAPSULE: {
        double r0 = shape == YGFX_LINE ? 0.5 * sp[0] * ppu : (double)sp[0] * ppu;
        double r1 = shape == YGFX_LINE ? r0 : (double)sp[1] * ppu, Dl = 2.0 * hx - r0 - r1;
        if (!(r0 > 0.0 && r1 > 0.0 && Dl > fabs(r0 - r1))) {
            *err = "a line or capsule needs radii > 0 and a length past both ends"; return YGFX_ERR_ARG;
        }
        C[0] = (float)hx; C[2] = (float)r0; C[3] = (float)r1;
        *L = Dl; *phw = 0.5 * (r0 + r1); *rad = hx;
        return 11;
    }
    case YGFX_CROSS:
        if (!(sp[0] > 0.0f)) { *err = "a cross needs an arm width"; return YGFX_ERR_ARG; }
        C[0] = (float)hx; C[1] = (float)hy; C[2] = (float)(sp[0] * ppu);
        return 5;
    case YGFX_ARC: case YGFX_PIE: {
        double sw = (double)sp[0] * YGFX__PI / 180.0, a0 = (double)sp[2] * YGFX__PI / 180.0, rb = 0.5 * sp[1] * ppu;
        if (!(hx > 0.0 && sw > 0.0)) { *err = "an arc or pie needs w and a sweep"; return YGFX_ERR_ARG; }
        if (sw > YGFX__TAU) sw = YGFX__TAU;
        /* u: the middle direction; sin, cos of the half sweep */
        D[0] = (float)sw; D[1] = (float)a0; D[2] = (float)sin(0.5 * sw); D[3] = (float)cos(0.5 * sw); *rad = hx;
        if (shape == YGFX_PIE) {
            C[0] = (float)hx; C[1] = (float)cos(a0 + 0.5 * sw); C[2] = (float)sin(a0 + 0.5 * sw);
            *L = 2.0 * hx + hx * sw;
            return 10;
        }
        if (!(rb > 0.0 && rb < hx)) { *err = "an arc needs 0 < thickness < w"; return YGFX_ERR_ARG; }
        C[0] = (float)cos(a0 + 0.5 * sw); C[1] = (float)sin(a0 + 0.5 * sw); C[2] = (float)(hx - rb); C[3] = (float)rb;
        *L = (hx - rb) * sw; *phw = rb;
        return 9;
    }
    case YGFX_NGON: case YGFX_STAR: {
        double n = floor((double)sp[0] + 0.5), rho, rf, an, Tx, Ty, Ix, ex, ey, LE, be, tb, e4[4], H;
        if (!(n >= 3.0 && n <= 64.0 && hx > 0.0)) { *err = "an ngon or star needs w and 3 to 64 sides"; return YGFX_ERR_ARG; }
        an = YGFX__PI / n;
        rho = shape == YGFX_NGON ? cos(an) : (double)sp[1];
        rf = (shape == YGFX_NGON ? (double)sp[1] : (double)sp[2]) * ppu;
        if (!(rho > 0.0 && rho <= 1.0) || rf < 0.0) { *err = "a star needs 0 < inner ratio <= 1 and rounding >= 0"; return YGFX_ERR_ARG; }
        Tx = hx * cos(an); Ty = hx * sin(an); Ix = hx * rho;
        LE = sqrt((Tx - Ix) * (Tx - Ix) + Ty * Ty); ex = (Tx - Ix) / LE; ey = Ty / LE;
        be = acos(ygfx__clampd(ex * cos(an) + ey * sin(an), -1.0, 1.0));
        tb = 1.0 / tan(be) + (fabs(ex) > 1e-5 ? 1.0 / tan(acos(fabs(ex))) : 0.0);
        if (rf * tb > LE * (1.0 + 1e-9)) { *err = "the rounding is larger than the edges allow"; return YGFX_ERR_ARG; }
        {
            double lI;
            ygfx__star_consts(hx, n, rho, rf, &H, &lI);
            C[0] = (float)hx; C[1] = (float)n; C[2] = (float)rho; C[3] = (float)rf;
            D[0] = (float)cos(an); D[1] = (float)sin(an); D[2] = (float)H; D[3] = (float)lI;
        }
        *L = 2.0 * n * H; *rad = hx;
        (void)e4;
        return 13;
    }
    case YGFX_ELLIPSE: {
        double asp = hx > hy ? hx / hy : hy / hx;
        if (!(hx > 0.0 && hy > 0.0) || asp > 20.0) { *err = "an ellipse needs w, h with an aspect ratio up to 20"; return YGFX_ERR_ARG; }
        C[0] = (float)hx; C[1] = (float)hy;
        /* iterations from the aspect: below 1e-4 px measured up to 10:1,
         * 6.6e-4 px at 20:1 (docs/gfx.md) */
        C[2] = asp <= 2.0 ? 3.0f : (asp <= 4.5 ? 6.0f : 8.0f);
        C[3] = (float)ygfx__ell_quarter(hx, hy);
        *L = 4.0 * (double)C[3]; *rad = hx > hy ? hx : hy;
        return 14;
    }
    default:
        *err = "not a primitive of the vector program";
        return YGFX_ERR_ARG;
    }
}

/* Writes vec4 v of the data region, if it fits. */
static int ygfx__vput(float* vd, int k, double a, double b, double c, double d) {
    if (k < 0 || k >= YGFX__VDATA) return 0;
    vd[4 * k] = (float)a; vd[4 * k + 1] = (float)b; vd[4 * k + 2] = (float)c; vd[4 * k + 3] = (float)d;
    return 1;
}

static double ygfx__edge_reach(const ygfx_stim* s) {
    if (s->edge == YGFX_EDGE_COSINE && s->edge_width > 0.0f) return 0.5 * s->edge_width;
    if (s->edge == YGFX_EDGE_GAUSSIAN && s->edge_width > 0.0f) return 5.0 * s->edge_width;
    return 0.0;
}

static int ygfx__is_vector(const ygfx_stim* s) {
    int sh = s->shape;
    if (s->kind != YGFX_SHAPE || sh == YGFX_MASK_TEX) return 0;
    if (sh >= YGFX_RRECT) return 1;
    if (sh == YGFX_POLYGON && s->shape_p[1] > 0.0f) return 1;
    if (s->fx || (s->paint && s->paint->kind != YGFX_PAINT_SOLID) || s->dash[0] > 0.0f) return 1;
    return !(s->trim[0] == 0.0f && s->trim[1] == 0.0f) && !(s->trim[0] <= 0.0f && s->trim[1] >= 1.0f);
}

/* A POLYGON's boundary for the vector program: per vertex, the fillet's
 * tangent points, center, signed radius and arc lengths (3 vec4). */
static int ygfx__vpolygon(const ygfx_stim* s, double ppu, float* vd, int g0, double* L, double* ext, const char** err) {
    int n = (int)s->shape_p[0], i;
    double rf = (double)s->shape_p[1] * ppu, area = 0.0, v[32], td[16], sacc = 0.0, o;
    if (n < 3 || n > 16) { *err = "a polygon needs 3 to 16 vertices"; return YGFX_ERR_ARG; }
    for (i = 0; i < 2 * n; i++) v[i] = (double)s->p[i] * ppu;
    for (i = 0; i < n; i++) { int j = (i + 1) % n; area += v[2 * i] * v[2 * j + 1] - v[2 * j] * v[2 * i + 1]; }
    if (area == 0.0) { *err = "a polygon with no area"; return YGFX_ERR_ARG; }
    o = area > 0.0 ? 1.0 : -1.0;
    ext[0] = ext[1] = 0.0;
    for (i = 0; i < n; i++) {
        int ip = (i + n - 1) % n, in = (i + 1) % n;
        double u1x = v[2 * i] - v[2 * ip], u1y = v[2 * i + 1] - v[2 * ip + 1], u2x = v[2 * in] - v[2 * i], u2y = v[2 * in + 1] - v[2 * i + 1];
        double l1 = sqrt(u1x * u1x + u1y * u1y), l2 = sqrt(u2x * u2x + u2y * u2y), dl;
        if (l1 == 0.0 || l2 == 0.0) { *err = "a polygon with a repeated vertex"; return YGFX_ERR_ARG; }
        dl = acos(ygfx__clampd(-(u1x * u2x + u1y * u2y) / (l1 * l2), -1.0, 1.0));
        td[i] = (rf > 0.0 && dl < YGFX__PI - 1e-9) ? rf / tan(0.5 * dl) : 0.0;
        if (fabs(v[2 * i]) > ext[0]) ext[0] = fabs(v[2 * i]);
        if (fabs(v[2 * i + 1]) > ext[1]) ext[1] = fabs(v[2 * i + 1]);
    }
    for (i = 0; i < n; i++) {
        int in = (i + 1) % n;
        double ex = v[2 * in] - v[2 * i], ey = v[2 * in + 1] - v[2 * i + 1];
        if (td[i] + td[in] > sqrt(ex * ex + ey * ey) * (1.0 + 1e-9)) { *err = "the fillets are larger than the edges allow"; return YGFX_ERR_ARG; }
    }
    for (i = 0; i < n; i++) {
        int ip = (i + n - 1) % n, in = (i + 1) % n;
        double u1x = v[2 * i] - v[2 * ip], u1y = v[2 * i + 1] - v[2 * ip + 1], u2x = v[2 * in] - v[2 * i], u2y = v[2 * in + 1] - v[2 * i + 1];
        double l1 = sqrt(u1x * u1x + u1y * u1y), l2 = sqrt(u2x * u2x + u2y * u2y);
        double pinx, piny, poutx, pouty, cx = 0.0, cy = 0.0, rs = 0.0, arc = 0.0, sin_ = sacc;
        u1x /= l1; u1y /= l1; u2x /= l2; u2y /= l2;
        pinx = v[2 * i] - u1x * td[i]; piny = v[2 * i + 1] - u1y * td[i];
        poutx = v[2 * i] + u2x * td[i]; pouty = v[2 * i + 1] + u2y * td[i];
        if (td[i] > 0.0) {
            double dl = acos(ygfx__clampd(-(u1x * u2x + u1y * u2y), -1.0, 1.0)), bx = -u1x + u2x, by = -u1y + u2y, bl = sqrt(bx * bx + by * by);
            cx = v[2 * i] + bx / bl * rf / sin(0.5 * dl); cy = v[2 * i + 1] + by / bl * rf / sin(0.5 * dl);
            rs = (u1x * u2y - u1y * u2x) * o > 0.0 ? rf : -rf;
            arc = rf * (YGFX__PI - dl);
        }
        sacc += arc;
        if (!ygfx__vput(vd, g0 + 3 * i, pinx, piny, poutx, pouty) ||
            !ygfx__vput(vd, g0 + 3 * i + 1, cx, cy, rs, sin_) ||
            !ygfx__vput(vd, g0 + 3 * i + 2, sacc, rf > 0.0 ? o : 2.0 * o, v[2 * i], v[2 * i + 1])) return YGFX_ERR_FULL;
        {   /* the edge to the next vertex's fillet */
            double nx = v[2 * in] - u2x * td[in], ny = v[2 * in + 1] - u2y * td[in];
            sacc += sqrt((nx - poutx) * (nx - poutx) + (ny - pouty) * (ny - pouty));
        }
    }
    *L = sacc;
    return 0;
}

/* The fx block (10 vec4) of a stimulus. */
static int ygfx__vfx(const ygfx_fx* fx, float* vd, int o) {
    const ygfx_shadow* sh[3];
    int k, ok = 1;
    sh[0] = &fx->drop; sh[1] = &fx->glow; sh[2] = &fx->inner;
    for (k = 0; k < 3; k++) {
        ok &= ygfx__vput(vd, o + 2 * k, sh[k]->sigma, sh[k]->spread, sh[k]->opacity > 0.0f ? sh[k]->opacity : 0.0f, 0.0);
        ok &= ygfx__vput(vd, o + 2 * k + 1, sh[k]->color[0], sh[k]->color[1], sh[k]->color[2], 0.0);
    }
    for (k = 0; k < 2; k++) {
        const ygfx_band* b = &fx->band[k];
        ok &= ygfx__vput(vd, o + 6 + 2 * k, b->a1, b->a2, b->opacity > 0.0f && b->a2 > b->a1 ? b->opacity : 0.0f, 0.0);
        ok &= ygfx__vput(vd, o + 7 + 2 * k, b->color[0], b->color[1], b->color[2], 0.0);
    }
    return ok ? 0 : YGFX_ERR_FULL;
}

/* A color in linear device RGB into the paint's space. */
static void ygfx__to_space(const ygfx_gfx* g, int space, const float rgb[3], double out[3]) {
    int k;
    if (space == YGFX_SPACE_OKLAB) {
        static const double m2[9] = { 0.2104542553, 0.7936177850, -0.0040720468, 1.9779984951, -2.4285922050, 0.4505937099,
                                      0.0259040371, 0.7827717662, -0.8086757660 };
        double v[3], l[3];
        for (k = 0; k < 3; k++) v[k] = rgb[k];
        ygfx__mul3(g->ok_rgb2lms, v, l);
        for (k = 0; k < 3; k++) l[k] = cbrt(l[k] + g->ok_black[k]);
        ygfx__mul3(m2, l, out);
    } else if (space == YGFX_SPACE_DKL_POLAR) {
        double v[3], d[3], r;
        for (k = 0; k < 3; k++) v[k] = (double)rgb[k] - g->bg[k];
        ygfx__mul3(g->rgb2dkl, v, d);
        r = sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        out[0] = r > 0.0 ? asin(ygfx__clampd(d[0] / r, -1.0, 1.0)) : 0.0;
        out[1] = atan2(d[2], d[1]);
        out[2] = r;
    } else {
        for (k = 0; k < 3; k++) out[k] = rgb[k];
    }
}

/* The paint's space back to device RGB, on the CPU (the shader's last step). */
static void ygfx__from_space(const ygfx_gfx* g, int space, const double c[3], double rgb[3]) {
    int k;
    if (space == YGFX_SPACE_OKLAB) {
        double l[3];
        l[0] = c[0] + 0.3963377774 * c[1] + 0.2158037573 * c[2];
        l[1] = c[0] - 0.1055613458 * c[1] - 0.0638541728 * c[2];
        l[2] = c[0] - 0.0894841775 * c[1] - 1.2914855480 * c[2];
        for (k = 0; k < 3; k++) l[k] = l[k] * l[k] * l[k] - g->ok_black[k];
        ygfx__mul3(g->ok_lms2rgb, l, rgb);
    } else if (space == YGFX_SPACE_DKL_POLAR) {
        double d[3];
        d[0] = c[2] * sin(c[0]); d[1] = c[2] * cos(c[0]) * cos(c[1]); d[2] = c[2] * cos(c[0]) * sin(c[1]);
        ygfx__mul3(g->dkl2rgb, d, rgb);
        for (k = 0; k < 3; k++) rgb[k] += g->bg[k];
    } else {
        for (k = 0; k < 3; k++) rgb[k] = c[k];
    }
}

/* The paint block: header, geometry, colors from vec4 2 (stops or the
 * vertices'), and from vec4 18 the space's matrix rows. */
static int ygfx__vpaint(const ygfx_gfx* g, const ygfx_stim* s, const ygfx_paint* pt, double ppu, float* vd, int o,
                          int poly_g0, int* used, int* clipped, const char** err) {
    int n = pt->n, k, j, clip = 0;
    double c[3], prev_az = 0.0;
    if (pt->kind < YGFX_PAINT_LINEAR || pt->kind > YGFX_PAINT_VERTEX || pt->space > YGFX_SPACE_DKL_POLAR) {
        *err = "paint kind or space"; return YGFX_ERR_ARG;
    }
    if (pt->space == YGFX_SPACE_OKLAB && !g->has_oklab) { *err = "OKLAB paint needs a calibration with chromaticities"; return YGFX_ERR_REFUSED; }
    if (pt->space == YGFX_SPACE_DKL_POLAR && !g->has_dkl) { *err = "DKL_POLAR paint needs a calibration with spectra"; return YGFX_ERR_REFUSED; }
    if (pt->kind == YGFX_PAINT_VERTEX) {
        n = (int)s->shape_p[0];
        if (s->shape != YGFX_POLYGON || !pt->vertex_colors || ygfx__convex(s->p, n, 1.0) == 0) {
            *err = "VERTEX paint needs a convex POLYGON and vertex_colors"; return YGFX_ERR_ARG;
        }
    } else {
        if (n < 2 || n > 6) { *err = "a paint has 2 to 6 stops"; return YGFX_ERR_ARG; }
        for (k = 1; k < n; k++) if (pt->stops[k].t < pt->stops[k - 1].t) { *err = "paint stops must rise"; return YGFX_ERR_ARG; }
    }
    if (!ygfx__vput(vd, o, pt->kind, pt->space, pt->repeat ? 1 : 0, n)) return YGFX_ERR_FULL;
    if (pt->kind == YGFX_PAINT_VERTEX) {
        double r = 0.0;
        for (k = 0; k < n; k++) r = fmax(r, sqrt((double)s->p[2 * k] * s->p[2 * k] + (double)s->p[2 * k + 1] * s->p[2 * k + 1]) * ppu);
        if (!ygfx__vput(vd, o + 1, poly_g0, r > 0.0 ? 1.0 / r : 1.0, 0, 0)) return YGFX_ERR_FULL;
    } else if (pt->kind == YGFX_PAINT_RADIAL) {
        if (!(pt->x1 > 0.0f)) { *err = "a radial paint needs a radius"; return YGFX_ERR_ARG; }
        if (!ygfx__vput(vd, o + 1, pt->x0 * ppu, pt->y0 * ppu, pt->x1 * ppu, 0)) return YGFX_ERR_FULL;
    } else if (pt->kind == YGFX_PAINT_ANGULAR) {
        if (!ygfx__vput(vd, o + 1, pt->x0 * ppu, pt->y0 * ppu, (double)pt->x1 * YGFX__PI / 180.0, 0)) return YGFX_ERR_FULL;
    } else {
        if (pt->x0 == pt->x1 && pt->y0 == pt->y1) { *err = "a linear paint needs two points"; return YGFX_ERR_ARG; }
        if (!ygfx__vput(vd, o + 1, pt->x0 * ppu, pt->y0 * ppu, pt->x1 * ppu, pt->y1 * ppu)) return YGFX_ERR_FULL;
    }
    for (k = 0; k < n; k++) {
        const float* rgb = pt->kind == YGFX_PAINT_VERTEX ? pt->vertex_colors + 3 * k : pt->stops[k].color;
        ygfx__to_space(g, pt->space, rgb, c);
        if (pt->space == YGFX_SPACE_DKL_POLAR && k > 0) {   /* the shorter turn from the stop before */
            while (c[1] - prev_az > YGFX__PI) c[1] -= YGFX__TAU;
            while (c[1] - prev_az < -YGFX__PI) c[1] += YGFX__TAU;
        }
        prev_az = c[1];
        if (!ygfx__vput(vd, o + 2 + k, c[0], c[1], c[2], pt->kind == YGFX_PAINT_VERTEX ? 0.0 : pt->stops[k].t)) return YGFX_ERR_FULL;
    }
    if (pt->space != YGFX_SPACE_RGB) {
        const double* m = pt->space == YGFX_SPACE_OKLAB ? g->ok_lms2rgb : g->dkl2rgb;
        for (k = 0; k < 3; k++)
            if (!ygfx__vput(vd, o + 18 + k, m[3 * k], m[3 * k + 1], m[3 * k + 2], pt->space == YGFX_SPACE_DKL_POLAR ? g->bg[k] : g->ok_off[k]))
                return YGFX_ERR_FULL;
        *used = 21;
    } else {
        *used = 2 + n;
    }
    /* A path between colors in gamut can leave it in a nonlinear space:
     * sampled, 64 points between each pair of neighbors, and counted. */
    for (k = 0; k + 1 < n && !clip; k++) {
        double a[3], b[3], rgb[3];
        int i2;
        for (j = 0; j < 3; j++) { a[j] = vd[4 * (o + 2 + k) + j]; b[j] = vd[4 * (o + 3 + k) + j]; }
        for (i2 = 0; i2 <= 64 && !clip; i2++) {
            double t = i2 / 64.0, m[3];
            for (j = 0; j < 3; j++) m[j] = a[j] + (b[j] - a[j]) * t;
            ygfx__from_space(g, pt->space, m, rgb);
            for (j = 0; j < 3; j++) if (rgb[j] < -1e-6 || rgb[j] > 1.0 + 1e-6) clip = 1;
        }
    }
    if (clip) *clipped = 1;
    return 0;
}

/* Packs a vector stimulus: its block from vec4 1 (vec4 0, the placement,
 * is the caller's) and the data region after it. Returns the blocks used,
 * or a negative code with *err set. ext gets the content's half extents
 * px (the bounds), before edges and effects. */
static int ygfx__vpack(const ygfx_gfx* g, const ygfx_stim* s, float* b, double ext[2], int* clipped, const char** err) {
    const double ppu = (double)g->ppu * ygfx__scale(s), er = ygfx__edge_reach(s);
    float* vd = b + 36;
    int nd = 0, np = 0, fl = 0, i, rc, pathlike = 0, kindv = 0, poly_g0 = -1;
    double L = -1.0, qx = 0.0, qy = 0.0, phw = 0.0, kmax = 0.0, a1 = 0.0, a2 = 0.0, outer, reach, gate;
    *err = "";
    *clipped = 0;
    memset(b + 4, 0, (size_t)(16 * 64 - 4) * sizeof(float));
    if (s->join != YGFX_JOIN_ROUND && s->shape != YGFX_POLYLINE) {
        *err = "MITER and BEVEL in the vector program: POLYLINE only"; return YGFX_ERR_ARG;
    }
    if (s->shape == YGFX_COMPOUND) {
        double R = 0.0;
        np = (int)s->n_prims;
        if (!s->prims || np < 1 || np > YGFX_MAX_PRIMS) { *err = "a compound needs 1 to YGFX_MAX_PRIMS prims"; return YGFX_ERR_ARG; }
        for (i = 0; i < np; i++) {
            const ygfx_prim* pr = &s->prims[i];
            float C[4], D[4];
            double l, rad, h2, cx = (double)pr->x * ppu, cy = (double)pr->y * ppu, an = (double)pr->ori * YGFX__PI / 180.0;
            int k = ygfx__vkind(pr->shape, pr->w, pr->h > 0.0f ? pr->h : pr->w, pr->p, ppu, C, D, &l, &rad, &h2, err);
            if (k < 0) return k;
            if (pr->op > YGFX_OP_SMOOTH_SUBTRACT) { *err = "a prim's op"; return YGFX_ERR_ARG; }
            if (i > 0 && pr->op >= YGFX_OP_SMOOTH_UNION) fl |= YGFX__VF_SMOOTH;
            if (pr->k * ppu > kmax) kmax = pr->k * ppu;
            ygfx__vput(vd, 4 * i, cx, cy, cos(an), sin(an));
            ygfx__vput(vd, 4 * i + 1, k, i ? pr->op : 0, pr->k * ppu, pr->onion > 0.0f ? pr->onion * ppu : 0.0);
            ygfx__vput(vd, 4 * i + 2, C[0], C[1], C[2], C[3]);
            ygfx__vput(vd, 4 * i + 3, D[0], D[1], D[2], D[3]);
            rad = sqrt(cx * cx + cy * cy) + rad + (pr->onion > 0.0f ? pr->onion * ppu : 0.0);
            if (rad > R) R = rad;
        }
        R += kmax + (s->shape_p[3] > 0.0f ? s->shape_p[3] * ppu : 0.0);
        qx = qy = R;
        nd = 4 * np;
        kindv = YGFX_COMPOUND;
    } else {
        float C[4], D[4];
        double rad;
        int sh = s->shape;
        np = 1;
        if (sh == YGFX_POLYGON) {
            double e2[2];
            rc = ygfx__vpolygon(s, ppu, vd, 4, &L, e2, err);
            if (rc < 0) return rc;
            C[0] = C[1] = C[2] = C[3] = 0.0f; D[0] = D[1] = 0.0f;
            D[2] = 4.0f; D[3] = (float)(int)s->shape_p[0];
            kindv = 4; poly_g0 = 4; nd = 4 + 3 * (int)s->shape_p[0];
            qx = e2[0]; qy = e2[1];
        } else if (sh == YGFX_POLYLINE || sh == YGFX_QBEZIER) {
            int n = (int)s->n_path, k;
            double hw = 0.5 * s->shape_p[0] * ppu, sacc = 0.0, lim = s->miter_limit > 0.0f ? s->miter_limit : 4.0;
            if (!s->path || hw <= 0.0 || (sh == YGFX_QBEZIER ? n != 3 : (n < 2 || n > YGFX_MAX_PATH))) {
                *err = "a path needs points (QBEZIER 3, POLYLINE 2 to 224) and a width"; return YGFX_ERR_ARG;
            }
            for (k = 0; k < n; k++) {
                double x = (double)s->path[2 * k] * ppu, y = (double)s->path[2 * k + 1] * ppu;
                if (fabs(x) > qx) qx = fabs(x);
                if (fabs(y) > qy) qy = fabs(y);
            }
            if (sh == YGFX_POLYLINE) {
                for (k = 0; k < n; k++) {
                    double x = (double)s->path[2 * k] * ppu, y = (double)s->path[2 * k + 1] * ppu;
                    if (k > 0) {
                        double dx = x - (double)s->path[2 * k - 2] * ppu, dy = y - (double)s->path[2 * k - 1] * ppu, l = sqrt(dx * dx + dy * dy);
                        if (l == 0.0) { *err = "a polyline with a repeated point"; return YGFX_ERR_ARG; }
                        sacc += l;
                    }
                    ygfx__vput(vd, 4 + k, x, y, sacc, 0.0);
                }
                C[0] = C[1] = 0.0f; C[2] = (float)hw; C[3] = (float)s->join;
                D[0] = (float)lim; D[1] = (float)s->cap; D[2] = 4.0f; D[3] = (float)n;
                kindv = 16; nd = 4 + n;
                hw *= s->join == YGFX_JOIN_MITER ? lim : 1.0;
            } else {
                double Ax = s->path[0] * ppu, Ay = s->path[1] * ppu, Bx = s->path[2] * ppu, By = s->path[3] * ppu;
                double Cx = s->path[4] * ppu, Cy = s->path[5] * ppu, bx = Ax - 2.0 * Bx + Cx, by = Ay - 2.0 * By + Cy;
                double px = Ax, py = Ay;
                int q, m;
                /* Chords on the CPU, drawn as a polyline with round joins: a
                 * chord over dt leaves the curve by at most |b| dt^2 / 4, b =
                 * A - 2B + C, so m chords keep it within 0.005 px. (IQ's
                 * closed form plus a Newton step held 4e-5 px but doubled the
                 * vector program's compile time through ANGLE.) */
                m = (int)ceil(sqrt(sqrt(bx * bx + by * by) / (4.0 * 0.005)));
                if (m < 1) m = 1;
                if (m > YGFX_MAX_PATH - 1) { *err = "a Bezier this curved needs more than 223 chords at 0.005 px"; return YGFX_ERR_ARG; }
                if ((Bx - Ax) * (Bx - Ax) + (By - Ay) * (By - Ay) == 0.0 && (Cx - Bx) * (Cx - Bx) + (Cy - By) * (Cy - By) == 0.0) {
                    *err = "a Bezier with no length"; return YGFX_ERR_ARG;
                }
                for (q = 0; q <= m; q++) {
                    double t = (double)q / m, x = (1 - t) * (1 - t) * Ax + 2 * (1 - t) * t * Bx + t * t * Cx;
                    double y = (1 - t) * (1 - t) * Ay + 2 * (1 - t) * t * By + t * t * Cy;
                    if (q > 0) sacc += sqrt((x - px) * (x - px) + (y - py) * (y - py));
                    ygfx__vput(vd, 4 + q, x, y, sacc, 0.0);
                    px = x; py = y;
                }
                C[0] = C[1] = 0.0f; C[2] = (float)hw; C[3] = (float)YGFX_JOIN_ROUND;
                D[0] = 4.0f; D[1] = (float)s->cap; D[2] = 4.0f; D[3] = (float)(m + 1);
                kindv = 16; nd = 5 + m;
            }
            L = sacc; phw = 0.5 * s->shape_p[0] * ppu; pathlike = 1;
            qx += hw + (s->cap == YGFX_CAP_SQUARE ? phw : 0.0);
            qy += hw + (s->cap == YGFX_CAP_SQUARE ? phw : 0.0);
        } else {
            double hx, hy;
            float sp[4];
            int k;
            for (k = 0; k < 4; k++) sp[k] = s->shape_p[k];
            kindv = ygfx__vkind(sh, s->w, s->h, sp, ppu, C, D, &L, &rad, &phw, err);
            if (kindv < 0) return kindv;
            pathlike = sh == YGFX_LINE || sh == YGFX_CAPSULE || sh == YGFX_ARC;
            hx = 0.5 * s->w * ppu; hy = 0.5 * s->h * ppu;
            if (sh == YGFX_CIRCLE || sh == YGFX_ANNULUS || sh == YGFX_ARC || sh == YGFX_PIE ||
                sh == YGFX_NGON || sh == YGFX_STAR) hy = hx;
            if (sh == YGFX_LINE) hy = 0.5 * s->shape_p[0] * ppu;
            if (sh == YGFX_CAPSULE) hy = fmax(s->shape_p[0], s->shape_p[1]) * ppu;
            qx = hx; qy = hy;
            nd = 4;
        }
        ygfx__vput(vd, 0, 0.0, 0.0, 1.0, 0.0);
        ygfx__vput(vd, 1, kindv, 0.0, 0.0, 0.0);
        ygfx__vput(vd, 2, C[0], C[1], C[2], C[3]);
        ygfx__vput(vd, 3, D[0], D[1], D[2], D[3]);
    }
    /* the main layer: a fill, or the stroke's band */
    if (s->stroke > 0.0f) {
        float f1, f2;
        ygfx__band(s, &f1, &f2);
        a1 = f1; a2 = f2;
    }
    gate = s->gate * (s->group ? s->group->opacity : 1.0f);
    b[8] = s->opacity; b[9] = (float)gate; b[10] = s->offset;
    b[11] = (float)(0.5 * (a1 + a2)); b[15] = (float)(0.5 * (a2 - a1));
    ygfx__copy3(b + 12, s->color);
    b[20] = (float)s->shape; b[21] = (float)s->edge; b[22] = s->edge == YGFX_EDGE_HARD || s->edge_width < 0.0f ? 0.0f : s->edge_width;
    b[27] = (float)np;
    b[30] = s->shape == YGFX_COMPOUND && s->shape_p[3] > 0.0f ? (float)(s->shape_p[3] * ppu) : 0.0f;
    b[35] = (float)phw;
    /* dashes and trim run along the boundary, or a path-like kind's center
     * line; on a closed boundary they need a stroke */
    {
        int dsh = s->dash[0] > 0.0f;
        int trm = !(s->trim[0] == 0.0f && s->trim[1] == 0.0f) && !(s->trim[0] <= 0.0f && s->trim[1] >= 1.0f);
        if (dsh || trm) {
            double P, dash = s->dash[0], gap = s->dash[1] > 0.0f ? s->dash[1] : 0.0;
            if (L <= 0.0 || s->shape == YGFX_COMPOUND) { *err = "dashes and trim need a length (not a cross, mask or compound)"; return YGFX_ERR_ARG; }
            if (!(s->stroke > 0.0f) && !pathlike) { *err = "dashes and trim on a closed boundary need a stroke"; return YGFX_ERR_ARG; }
            if (trm && !(s->trim[0] >= 0.0f && s->trim[0] <= s->trim[1] && s->trim[1] <= 1.0f)) { *err = "trim: 0 <= start <= end <= 1"; return YGFX_ERR_ARG; }
            P = dash + gap;
            if (dsh && s->dash_snap && !pathlike) {
                double m = floor(L / P + 0.5);
                if (m < 1.0) m = 1.0;
                dash *= L / (m * P); gap *= L / (m * P);
            }
            b[16] = (float)dash; b[17] = (float)gap; b[18] = s->dash_offset;
            b[24] = (float)(s->trim[0] * L); b[25] = (float)(s->trim[1] * L);
            fl |= (dsh ? YGFX__VF_DASH : 0) | (trm ? YGFX__VF_TRIM : 0) | YGFX__VF_S;
        }
        b[19] = (float)L; b[26] = (float)s->cap;
    }
    /* reach past the boundary, px: the quad's margin */
    outer = s->offset + (s->stroke > 0.0f ? (a2 > 0.0 ? a2 : 0.0) : 0.0);
    if (outer < 0.0) outer = 0.0;
    reach = outer + er;
    b[31] = 1.0f;
    b[32] = b[33] = -1.0f;
    if (s->fx) {
        const ygfx_fx* fx = s->fx;
        double rad = ((double)s->ori + (s->group ? s->group->ori : 0.0f)) * YGFX__PI / 180.0, c = cos(rad), sn = sin(rad), r;
        int k;
        rc = ygfx__vfx(fx, vd, nd);
        if (rc < 0) { *err = "the data region is full"; return rc; }
        b[32] = (float)nd;
        nd += 10;
        fl |= YGFX__VF_FX;
        b[28] = (float)(fx->dx * c + fx->dy * sn);
        b[29] = (float)(-fx->dx * sn + fx->dy * c);
        if ((fx->drop.opacity > 0.0f || fx->inner.opacity > 0.0f) && (fx->dx != 0.0f || fx->dy != 0.0f)) b[31] = 2.0f;
        if (fx->flags & YGFX_FX_EXACT_BLUR) {
            if (!(kindv == 8 && vd[10] == 0.0f && vd[11] == 0.0f && vd[12] == 0.0f && vd[13] == 0.0f)) {
                *err = "YGFX_FX_EXACT_BLUR needs a RECT or RRECT with no corner radius"; return YGFX_ERR_ARG;
            }
            fl |= YGFX__VF_RBLUR;
        }
        if (fx->drop.opacity > 0.0f) {
            r = sqrt((double)fx->dx * fx->dx + (double)fx->dy * fx->dy) + fx->drop.spread + s->offset + 5.0 * fx->drop.sigma;
            if (r > reach) reach = r;
        }
        if (fx->glow.opacity > 0.0f) {
            r = (double)fx->glow.spread + s->offset + 5.0 * fx->glow.sigma;
            if (r > reach) reach = r;
        }
        for (k = 0; k < 2; k++)
            if (fx->band[k].opacity > 0.0f) {
                r = (double)fx->band[k].a2 + s->offset + er;
                if (r > reach) reach = r;
            }
    }
    if (s->paint && s->paint->kind != YGFX_PAINT_SOLID) {
        int used = 0;
        rc = ygfx__vpaint(g, s, s->paint, ppu, vd, nd, poly_g0, &used, clipped, err);
        if (rc < 0) return rc;
        b[33] = (float)nd;
        nd += used;
        fl |= YGFX__VF_PAINT;
    }
    if (nd > YGFX__VDATA) { *err = "the data region is full (247 vec4)"; return YGFX_ERR_FULL; }
    b[23] = (float)fl;
    b[4] = (float)qx; b[5] = (float)qy; b[6] = (float)ppu;
    b[7] = (float)(reach + 1.0);
    ext[0] = qx + outer; ext[1] = qy + outer;
    return nd <= 7 ? 1 : 1 + (nd - 7 + 15) / 16;
}

/* --- parameter tables --------------------------------------------------------- */

#define YGFX__K(k) (1u << (k))
#define YGFX__ALL  0x1FEu
#define YGFX__MOD  (YGFX__K(YGFX_GRATING) | YGFX__K(YGFX_GABOR) | YGFX__K(YGFX_NOISE) | \
                      YGFX__K(YGFX_IMAGE) | YGFX__K(YGFX_USER))
#define YGFX__COL  (YGFX__K(YGFX_SHAPE) | YGFX__K(YGFX_DOTS) | YGFX__K(YGFX_IMAGE) | YGFX__K(YGFX_USER) |                       YGFX__K(YGFX_TEXT))
#define YGFX__APT  (YGFX__K(YGFX_SHAPE) | YGFX__K(YGFX_GRATING) | YGFX__K(YGFX_NOISE) | \
                      YGFX__K(YGFX_USER) | YGFX__K(YGFX_DOTS))
#define YGFX__F(name, field, lo, hi, def, unit, doc, kinds) \
    { name, "f32", lo, hi, def, unit, doc, (uint32_t)offsetof(ygfx_stim, field), kinds }
#define YGFX__P(i) YGFX__F("p" #i, p[i], -1e30, 1e30, 0, "", "user parameter; POLYGON vertex coordinate", \
                               YGFX__K(YGFX_USER) | YGFX__K(YGFX_SHAPE))

static const ygfx_param ygfx__params[YGFX_P_COUNT] = {
    YGFX__F("visible", visible, 0, 1, 1, "", "drawn when 0.5 or more; onset and offset events", YGFX__ALL),
    YGFX__F("x", x, -1e6, 1e6, 0, "u", "anchor offset from the place point, +x right", YGFX__ALL),
    YGFX__F("y", y, -1e6, 1e6, 0, "u", "anchor offset from the place point, +y down", YGFX__ALL),
    YGFX__F("ax", ax, -10, 10, 0.5, "", "anchor x, fraction of the box from its left edge", YGFX__ALL),
    YGFX__F("ay", ay, -10, 10, 0.5, "", "anchor y, fraction of the box from its top edge", YGFX__ALL),
    YGFX__F("w", w, 0, 1e6, 0, "u", "box width; GABOR 0 = 8 sigma", YGFX__ALL),
    YGFX__F("h", h, 0, 1e6, 0, "u", "box height; 0 = w in constructors", YGFX__ALL),
    YGFX__F("ori", ori, -1e6, 1e6, 0, "deg", "rotation about the anchor, +x toward +y (clockwise)", YGFX__ALL),
    YGFX__F("contrast", contrast, -1e3, 1e3, 1, "", "modulation amplitude along dir", YGFX__MOD),
    YGFX__F("opacity", opacity, 0, 1, 1, "", "alpha of a color stimulus", YGFX__COL),
    YGFX__F("gate", gate, 0, 1, 1, "", "multiplier for flicker tracks", YGFX__ALL),
    YGFX__F("color_r", color[0], 0, 1, 0, "rgb", "linear device red", YGFX__COL),
    YGFX__F("color_g", color[1], 0, 1, 0, "rgb", "linear device green", YGFX__COL),
    YGFX__F("color_b", color[2], 0, 1, 0, "rgb", "linear device blue", YGFX__COL),
    YGFX__F("dir_r", dir[0], -1e3, 1e3, 0, "rgb", "modulation per unit contrast, red; all 0 = background", YGFX__MOD),
    YGFX__F("dir_g", dir[1], -1e3, 1e3, 0, "rgb", "modulation per unit contrast, green", YGFX__MOD),
    YGFX__F("dir_b", dir[2], -1e3, 1e3, 0, "rgb", "modulation per unit contrast, blue", YGFX__MOD),
    YGFX__F("sf", sf, 0, 1e6, 0, "cyc/u", "spatial frequency", YGFX__K(YGFX_GRATING) | YGFX__K(YGFX_GABOR)),
    YGFX__F("phase", phase, -1e9, 1e9, 0, "cyc", "carrier phase at the box center", YGFX__K(YGFX_GRATING) | YGFX__K(YGFX_GABOR)),
    YGFX__F("sigma", sigma, 0, 1e6, 0, "u", "Gaussian envelope SD", YGFX__K(YGFX_GABOR)),
    YGFX__F("aspect", aspect, 0, 1e6, 1, "", "envelope: y SD = sigma / aspect", YGFX__K(YGFX_GABOR)),
    YGFX__F("edge_width", edge_width, 0, 1e6, 0, "px", "COSINE full width, GAUSSIAN SD", YGFX__APT),
    YGFX__F("shape_p0", shape_p[0], 0, 1e6, 0, "u", "annulus inner radius, line and cross arm width, polygon n", YGFX__APT),
    YGFX__F("shape_p1", shape_p[1], 0, 1e6, 0, "u", "rect corner radius", YGFX__APT),
    YGFX__F("shape_p2", shape_p[2], 0, 1e6, 0, "u", "reserved", YGFX__APT),
    YGFX__F("shape_p3", shape_p[3], 0, 1e6, 0, "u", "reserved", YGFX__APT),
    YGFX__F("dot_size", dot_size, 0, 1e6, 0, "px", "dot diameter", YGFX__K(YGFX_DOTS)),
    YGFX__F("seed", seed, 0, 16777215, 0, "", "noise seed, an integer", YGFX__K(YGFX_NOISE)),
    YGFX__F("check", check, 1, 1e6, 1, "px", "noise check size", YGFX__K(YGFX_NOISE)),
    YGFX__P(0),  YGFX__P(1),  YGFX__P(2),  YGFX__P(3),  YGFX__P(4),  YGFX__P(5),  YGFX__P(6),  YGFX__P(7),
    YGFX__P(8),  YGFX__P(9),  YGFX__P(10), YGFX__P(11), YGFX__P(12), YGFX__P(13), YGFX__P(14), YGFX__P(15),
    YGFX__P(16), YGFX__P(17), YGFX__P(18), YGFX__P(19), YGFX__P(20), YGFX__P(21), YGFX__P(22), YGFX__P(23),
    YGFX__P(24), YGFX__P(25), YGFX__P(26), YGFX__P(27), YGFX__P(28), YGFX__P(29), YGFX__P(30), YGFX__P(31),
    YGFX__F("stroke", stroke, 0, 1e6, 0, "px", "stroke band width; 0 = fill", YGFX__APT),
    YGFX__F("miter_limit", miter_limit, 0, 1e6, 4, "", "MITER: miter length over stroke width before a bevel", YGFX__APT),
    YGFX__F("tint_r", tint[0], 0, 1e3, 1, "", "image tint, linear red factor", YGFX__K(YGFX_IMAGE)),
    YGFX__F("tint_g", tint[1], 0, 1e3, 1, "", "image tint, linear green factor", YGFX__K(YGFX_IMAGE)),
    YGFX__F("tint_b", tint[2], 0, 1e3, 1, "", "image tint, linear blue factor", YGFX__K(YGFX_IMAGE)),
    YGFX__F("tint_a", tint[3], 0, 1e3, 1, "", "image tint, alpha factor", YGFX__K(YGFX_IMAGE)),
    YGFX__F("src_x", src[0], 0, 65535, 0, "texel", "source rectangle, left", YGFX__K(YGFX_IMAGE)),
    YGFX__F("src_y", src[1], 0, 65535, 0, "texel", "source rectangle, top", YGFX__K(YGFX_IMAGE)),
    YGFX__F("src_w", src[2], 0, 65535, 0, "texel", "source rectangle width; 0 = the whole texture", YGFX__K(YGFX_IMAGE)),
    YGFX__F("src_h", src[3], 0, 65535, 0, "texel", "source rectangle height", YGFX__K(YGFX_IMAGE)),
    YGFX__F("offset", offset, -1e6, 1e6, 0, "px", "boundary moved out (+) or in (-)", YGFX__K(YGFX_SHAPE)),
    YGFX__F("dash", dash[0], 0, 1e6, 0, "px", "dash length along the boundary; 0 = none", YGFX__K(YGFX_SHAPE)),
    YGFX__F("gap", dash[1], 0, 1e6, 0, "px", "gap between dashes", YGFX__K(YGFX_SHAPE)),
    YGFX__F("dash_offset", dash_offset, -1e9, 1e9, 0, "px", "dash pattern shift along the boundary", YGFX__K(YGFX_SHAPE)),
    YGFX__F("trim_start", trim[0], 0, 1, 0, "", "drawn from this fraction of the length", YGFX__K(YGFX_SHAPE)),
    YGFX__F("trim_end", trim[1], 0, 1, 0, "", "drawn to this fraction; 0, 0 = all", YGFX__K(YGFX_SHAPE)),
    YGFX__F("size", size, 0, 1e6, 0, "u", "units per set unit: the font size per em", YGFX__K(YGFX_TEXT)),
    YGFX__F("noise_scale", gn_scale, 0, 1e6, 0, "u", "SIMPLEX: units per lattice cell", YGFX__K(YGFX_NOISE)),
    YGFX__F("noise_z", gn_z, -1e6, 1e6, 0, "cells", "SIMPLEX: the third coordinate; bind it to evolve the field", YGFX__K(YGFX_NOISE)),
    YGFX__F("noise_lacunarity", gn_lacunarity, 1, 8, 2, "", "SIMPLEX: frequency ratio of successive octaves", YGFX__K(YGFX_NOISE)),
    YGFX__F("noise_gain", gn_gain, 0, 1, 0.5, "", "SIMPLEX: amplitude ratio of successive octaves", YGFX__K(YGFX_NOISE)),
};

#define YGFX__GF(name, field, lo, hi, def, unit, doc) \
    { name, "f32", lo, hi, def, unit, doc, (uint32_t)offsetof(ygfx_group, field), 0u }
static const ygfx_param ygfx__group_params[YGFX_G_COUNT] = {
    YGFX__GF("visible", visible, 0, 1, 1, "", "members drawn when 0.5 or more"),
    YGFX__GF("x", x, -1e6, 1e6, 0, "u", "anchor offset from the place point, +x right"),
    YGFX__GF("y", y, -1e6, 1e6, 0, "u", "anchor offset from the place point, +y down"),
    YGFX__GF("ax", ax, -10, 10, 0.5, "", "anchor x, fraction of the group's box"),
    YGFX__GF("ay", ay, -10, 10, 0.5, "", "anchor y, fraction of the group's box"),
    YGFX__GF("w", w, 0, 1e6, 0, "u", "the group's box width"),
    YGFX__GF("h", h, 0, 1e6, 0, "u", "the group's box height"),
    YGFX__GF("ori", ori, -1e6, 1e6, 0, "deg", "rotation about the anchor, +x toward +y (clockwise)"),
    YGFX__GF("scale", scale, 0, 1e6, 1, "", "members' sizes in units; px quantities unscaled"),
    YGFX__GF("opacity", opacity, 0, 1, 1, "", "multiplies every member's gate"),
};

#define YGFX__XF(T, name, field, lo, hi, def, unit, doc) \
    { name, "f32", lo, hi, def, unit, doc, (uint32_t)offsetof(T, field), 0u }
static const ygfx_param ygfx__prim_params[] = {
    YGFX__XF(ygfx_prim, "x", x, -1e6, 1e6, 0, "u", "center in the compound's box frame, +x right"),
    YGFX__XF(ygfx_prim, "y", y, -1e6, 1e6, 0, "u", "center, +y down"),
    YGFX__XF(ygfx_prim, "w", w, 0, 1e6, 0, "u", "the primitive's box width, as the shape's w"),
    YGFX__XF(ygfx_prim, "h", h, 0, 1e6, 0, "u", "box height; 0 = w"),
    YGFX__XF(ygfx_prim, "ori", ori, -1e6, 1e6, 0, "deg", "rotation, +x toward +y (clockwise)"),
    YGFX__XF(ygfx_prim, "k", k, 0, 1e6, 0, "u", "SMOOTH_* blend radius"),
    YGFX__XF(ygfx_prim, "onion", onion, 0, 1e6, 0, "u", "half thickness of the shell |d| - onion; 0 = none"),
    YGFX__XF(ygfx_prim, "p0", p[0], -1e6, 1e6, 0, "", "shape parameter 0, as shape_p"),
    YGFX__XF(ygfx_prim, "p1", p[1], -1e6, 1e6, 0, "", "shape parameter 1"),
    YGFX__XF(ygfx_prim, "p2", p[2], -1e6, 1e6, 0, "", "shape parameter 2"),
    YGFX__XF(ygfx_prim, "p3", p[3], -1e6, 1e6, 0, "", "shape parameter 3"),
};
#define YGFX__SH(base, doc) \
    YGFX__XF(ygfx_fx, base ".sigma", base_.sigma, 0, 1e6, 0, "px", doc " Gaussian SD; 0 = hard"), \
    YGFX__XF(ygfx_fx, base ".spread", base_.spread, -1e6, 1e6, 0, "px", doc " silhouette moved out"), \
    YGFX__XF(ygfx_fx, base ".color_r", base_.color[0], 0, 1, 0, "rgb", doc " linear device red"), \
    YGFX__XF(ygfx_fx, base ".color_g", base_.color[1], 0, 1, 0, "rgb", doc " linear device green"), \
    YGFX__XF(ygfx_fx, base ".color_b", base_.color[2], 0, 1, 0, "rgb", doc " linear device blue"), \
    YGFX__XF(ygfx_fx, base ".opacity", base_.opacity, 0, 1, 0, "", doc " opacity; 0 = off")
#define YGFX__BD(base, i) \
    YGFX__XF(ygfx_fx, base ".a1", band[i].a1, -1e6, 1e6, 0, "px", "band inner offset of the boundary"), \
    YGFX__XF(ygfx_fx, base ".a2", band[i].a2, -1e6, 1e6, 0, "px", "band outer offset"), \
    YGFX__XF(ygfx_fx, base ".color_r", band[i].color[0], 0, 1, 0, "rgb", "band linear device red"), \
    YGFX__XF(ygfx_fx, base ".color_g", band[i].color[1], 0, 1, 0, "rgb", "band linear device green"), \
    YGFX__XF(ygfx_fx, base ".color_b", band[i].color[2], 0, 1, 0, "rgb", "band linear device blue"), \
    YGFX__XF(ygfx_fx, base ".opacity", band[i].opacity, 0, 1, 0, "", "band opacity; 0 = off")
#define base_ drop
static const ygfx_param ygfx__fx_params_drop[] = { YGFX__SH("drop", "drop shadow") };
#undef base_
#define base_ glow
static const ygfx_param ygfx__fx_params_glow[] = { YGFX__SH("glow", "outer glow") };
#undef base_
#define base_ inner
static const ygfx_param ygfx__fx_params_inner[] = { YGFX__SH("inner", "inner shadow") };
#undef base_
static const ygfx_param ygfx__fx_params_rest[] = {
    YGFX__XF(ygfx_fx, "dx", dx, -1e6, 1e6, 0, "px", "drop and inner shadow offset, screen +x right"),
    YGFX__XF(ygfx_fx, "dy", dy, -1e6, 1e6, 0, "px", "the offset, screen +y down"),
    YGFX__BD("band0", 0), YGFX__BD("band1", 1),
};
static ygfx_param ygfx__fx_params[2 + 3 * 6 + 12];
static const ygfx_param ygfx__paint_params[] = {
    YGFX__XF(ygfx_paint, "x0", x0, -1e6, 1e6, 0, "u", "LINEAR start, RADIAL and ANGULAR center x"),
    YGFX__XF(ygfx_paint, "y0", y0, -1e6, 1e6, 0, "u", "LINEAR start, the center y"),
    YGFX__XF(ygfx_paint, "x1", x1, -1e6, 1e6, 0, "", "LINEAR end x (u), RADIAL radius (u), ANGULAR start (deg)"),
    YGFX__XF(ygfx_paint, "y1", y1, -1e6, 1e6, 0, "u", "LINEAR end y"),
#define YGFX__ST(i) \
    YGFX__XF(ygfx_paint, "stop" #i ".t", stops[i].t, 0, 1, 0, "", "stop position"), \
    YGFX__XF(ygfx_paint, "stop" #i ".color_r", stops[i].color[0], 0, 1, 0, "rgb", "stop linear device red"), \
    YGFX__XF(ygfx_paint, "stop" #i ".color_g", stops[i].color[1], 0, 1, 0, "rgb", "stop linear device green"), \
    YGFX__XF(ygfx_paint, "stop" #i ".color_b", stops[i].color[2], 0, 1, 0, "rgb", "stop linear device blue")
    YGFX__ST(0), YGFX__ST(1), YGFX__ST(2), YGFX__ST(3), YGFX__ST(4), YGFX__ST(5),
};

YGFX_API const ygfx_param* ygfx_prim_params(int* n) {
    if (n) *n = (int)(sizeof ygfx__prim_params / sizeof ygfx__prim_params[0]);
    return ygfx__prim_params;
}

YGFX_API const ygfx_param* ygfx_fx_params(int* n) {
    /* one table from four: the shadows' share a macro */
    if (!ygfx__fx_params[0].name) {
        int k = 0, i;
        for (i = 0; i < 2; i++) ygfx__fx_params[k++] = ygfx__fx_params_rest[i];
        for (i = 0; i < 6; i++) ygfx__fx_params[k++] = ygfx__fx_params_drop[i];
        for (i = 0; i < 6; i++) ygfx__fx_params[k++] = ygfx__fx_params_glow[i];
        for (i = 0; i < 6; i++) ygfx__fx_params[k++] = ygfx__fx_params_inner[i];
        for (i = 2; i < 14; i++) ygfx__fx_params[k++] = ygfx__fx_params_rest[i];
    }
    if (n) *n = (int)(sizeof ygfx__fx_params / sizeof ygfx__fx_params[0]);
    return ygfx__fx_params;
}

YGFX_API const ygfx_param* ygfx_paint_params(int* n) {
    if (n) *n = (int)(sizeof ygfx__paint_params / sizeof ygfx__paint_params[0]);
    return ygfx__paint_params;
}

YGFX_API double ygfx_length(const ygfx_gfx* g, const ygfx_stim* s) {
    float blk[16 * 64];
    double ext[2];
    const char* err;
    int clip, n;
    ygfx_stim t;
    if (!g || !g->open || !s) return YGFX_ERR_ARG;
    if (s->kind != YGFX_SHAPE || s->shape == YGFX_MASK_TEX || s->shape == YGFX_COMPOUND || s->shape == YGFX_CROSS ||
        s->shape == YGFX_NO_APERTURE) return YGFX_ERR_ARG;
    t = *s;
    t.fx = NULL; t.paint = NULL; t.dash[0] = 0.0f; t.trim[0] = t.trim[1] = 0.0f;
    t.join = YGFX_JOIN_ROUND; t.shape_p[1] = t.shape == YGFX_POLYGON && t.shape_p[1] <= 0.0f ? 0.0f : t.shape_p[1];
    t.dash[0] = 1.0f; t.dash[1] = 1.0f; t.dash_snap = 0;
    if (t.stroke <= 0.0f) t.stroke = 1.0f;   /* a length needs no stroke; the pack asks for one */
    n = ygfx__vpack(g, &t, blk, ext, &clip, &err);
    if (n < 0) return n;
    return (double)blk[19];
}

YGFX_API const ygfx_param* ygfx_group_params(int* n) {
    if (n) *n = YGFX_G_COUNT;
    return ygfx__group_params;
}

YGFX_API const ygfx_param* ygfx_params(int* n) {
    if (n) *n = YGFX_P_COUNT;
    return ygfx__params;
}

YGFX_API int ygfx_apply(const ygfx_bind* b, int n, const float* values) {
    int i;
    if ((!b && n > 0) || n < 0 || !values) return YGFX_ERR_ARG;
    /* all or nothing: a bad entry changes no field */
    for (i = 0; i < n; i++) {
        if (b[i].field) continue;
        if (b[i].group ? b[i].param >= YGFX_G_COUNT : (!b[i].stim || b[i].param >= YGFX_P_COUNT))
            return YGFX_ERR_ARG;
    }
    for (i = 0; i < n; i++) {
        if (b[i].field) *b[i].field = values[b[i].channel];
        else if (b[i].group) *(float*)((char*)b[i].group + ygfx__group_params[b[i].param].offset) = values[b[i].channel];
        else *(float*)((char*)b[i].stim + ygfx__params[b[i].param].offset) = values[b[i].channel];
    }
    return n;
}

static const yscr_param ygfx__desc_params[] = {
    { "background.r", "f32", 0, 1, 0, "rgb", "background, linear device red" },
    { "background.g", "f32", 0, 1, 0, "rgb", "background, linear device green" },
    { "background.b", "f32", 0, 1, 0, "rgb", "background, linear device blue" },
    { "units", "enum", 0, 1, 0, "", "0 pixels, 1 degrees (one scale factor at the screen center)" },
    { "view.distance_mm", "f32", 0, 1e5, 0, "mm", "viewing distance, degrees only" },
    { "view.width_mm", "f32", 0, 1e5, 0, "mm", "width of the drawable area, degrees only" },
    { "dither", "enum", 0, 2, 0, "", "0 none (round), 1 ordered 8x8, 2 noise per frame" },
    { "seed", "u32", 0, 4294967295.0, 0, "", "noise dither seed" },
    { "output", "enum", 0, 0, 0, "", "0 8-bit (the only one in v0.1)" },
    { "stereo", "enum", 0, 0, 0, "", "0 mono (the only one in v0.1)" },
    { "max_draws", "i32", 0, 65536, 1024, "", "draws per frame" },
    { "cones", "enum", 0, 1, 0, "", "cones for DKL paint and ygfx_color(): 0 2 degree (SS2), 1 10 degree (SS10)" },
};

YGFX_API const yscr_param* ygfx_desc_params(int* n) {
    if (n) *n = (int)(sizeof ygfx__desc_params / sizeof ygfx__desc_params[0]);
    return ygfx__desc_params;
}

#ifdef __cplusplus
}
#endif

#endif /* YSP_GFX_IMPLEMENTATION_GUARD */
#endif /* YSP_GFX_IMPLEMENTATION */

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
 * ------------------------------------------------------------------------
 * Parts under the MIT license: the vector program's distance functions
 * (ygfx__glsl_vec0, _vec0b and _vec1) and their CPU forms (ygfx__vp_*,
 * ygfx__vsmin). Each such function names its source in a comment.
 *
 *   Inigo Quilez, 2D distance functions and the smooth minimum
 *   (iquilezles.org/articles/distfunctions2d, /articles/smin), with the
 *   notice of each Shadertoy source:
 *     The MIT License
 *     Copyright (c) 2015 Inigo Quilez   (rounded box, Shadertoy 4llXD7)
 *     Copyright (c) 2018 Inigo Quilez   (uneven capsule, Shadertoy 4lcBWn)
 *     Copyright (c) 2019 Inigo Quilez   (arc, Shadertoy wl23RK)
 *     Copyright (c) 2019 Inigo Quilez   (pie, Shadertoy 3l23RK)
 *     Copyright (c) 2019 Inigo Quilez   (star, Shadertoy 3tSGDy)
 *   The box and the smooth minimum come from the articles, which carry no
 *   per-function year; they are under the same terms.
 *
 *   0xfaded, the trig-free closest point on an ellipse
 *   (github.com/0xfaded/ellipse_demo):
 *     MIT License
 *     Copyright (c) 2017 Carl Chatfield
 *
 * Both under these terms:
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 * ------------------------------------------------------------------------ */
