/* gratings.c - counterphase flicker, contrast-modulated gratings, a
 * flashing checkerboard wedge, a render target seen through a user
 * shader, and an image as a graded mask: ysp/gfx.h's USER shaders driven
 * by ysp/timeline.h tracks.
 *
 * It does what PsychoPy's Coder demos stimuli/counterphase.py,
 * stimuli/secondOrderGratings.py, stimuli/rotatingFlashingWedge.py,
 * hardware/VSHD_Distortion.py and stimuli/face_jpg.py (the image as a
 * grating's mask), and the Builder demo Feature Demos/gratings/ show.
 * PsychoPy is GPL-3: its demos were read, no code was copied.
 *
 * Six tiles of 240 px in a 960 x 640 window, on a mid-gray background:
 *   1  counterphase flicker: a grating whose contrast is a timeline track,
 *      0.5 cos(2 pi f t) (two cosine-eased keys a half period apart,
 *      repeated); f is --tf (default 4 Hz). At the end the program gives
 *      the achieved f from the flip records: f times the least-squares
 *      slope of the planned onsets (where the track was sampled) on the
 *      flips' measured onsets, and the flips with a dropped vblank. A
 *      late frame shows its phase late, so drops pull the slope from 1.
 *   2  a contrast-modulated (second-order) grating: a still sine carrier
 *      (1/8 c/px) under an envelope (1/120 c/px, depth 1) that drifts at
 *      0.5 Hz; the envelope's phase is a sawtooth track bound to a USER
 *      parameter.
 *   3  the same envelope on a binary noise carrier (4 px checks, from
 *      ysp_hash2(), so the GPU's carrier is the CPU's ygfx_hash2()).
 *   4  a rotating checkerboard wedge (60 deg, 0.1 rev/s) whose checks
 *      reverse at 4 Hz: two tracks, an angle and a polarity (STEP keys).
 *   5  a drifting grating drawn into a 256 x 256 render target each frame,
 *      then shown through a USER shader that samples the target (ysp_tex0)
 *      with a barrel distortion, as for a display seen through a lens.
 *   6  a drifting grating whose amplitude is a graded image (128 x 128,
 *      made here: an oval with darker eyes and mouth): the image is the
 *      USER shader's ysp_tex0.
 * Tiles 5 and 6 need ysp/gfx.h v0.10.4: before it a USER draw bound no
 * texture (docs/gfx.md, v0.10.4).
 *
 * No calibration: scene values are device values, and nothing here is a
 * measurement of light. Contrast is at most 0.5 on tile 1.
 *
 * --sim: the simulated display and the null backend for 2 s. It checks
 * that no draw was refused, that tile 1's contrast on each frame was
 * 0.5 cos(2 pi f t) at the frame's planned onset (within 1e-6), and that
 * the achieved f equals --tf (the simulated display drops nothing). Exits
 * 1 on a mismatch.
 *
 * Usage: gfx_gratings [--sim] [--fullscreen] [--seconds S] [--tf HZ]
 *   --seconds S  run S seconds, then report (default: until Shift+Esc)
 * Exit code: 0; 1 when the screen or the gfx did not open, a draw was
 * refused or a --sim check failed; 2 for a bad argument.
 * On Windows set YSCR_ANGLE_DIR to ANGLE's directory.
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#define YSP_GFX_IMPLEMENTATION
#include "ysp/gfx.h"
#define YSP_TIMELINE_IMPLEMENTATION
#include "ysp/timeline.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { CPH, ENV, ROT, POL, DRIFT, N_CHANNELS };
enum { BASE = 1 };
#define S_NS    YRT_S(1)
#define TILE    240.0f
#define TGT     256
#define MASK    128
#define RING    4096   /* planned onsets by frame index */
#define PI      3.14159265358979323846

/* Tiles 2 and 3: p0 carrier c/px, p1 envelope c/px, p2 depth, p3 envelope
 * phase (cycles), p4 1 for the noise carrier, p5 its check size px. */
static const char* cm_body =
    "float ysp_main(vec2 p) {\n"
    "    float carrier = cos(2.0 * ysp_PI * ysp_param(0) * p.x);\n"
    "    if (ysp_param(4) > 0.5) {\n"
    "        uvec2 c = uvec2(ivec2(floor(p / ysp_param(5))) + 32768);\n"
    "        carrier = (ysp_hash2(c, 7u) & 1u) == 1u ? 1.0 : -1.0;\n"
    "    }\n"
    "    float env = 0.5 + 0.5 * ysp_param(2) * cos(2.0 * ysp_PI * (ysp_param(1) * p.y - ysp_param(3)));\n"
    "    return carrier * env;\n"
    "}\n";
/* Tile 4: p0 the wedge's start (deg), p1 its width, p2 the inner radius
 * px, p3 a ring's width px, p4 a sector's width deg, p5 polarity +-1. */
static const char* wedge_body =
    "float ysp_main(vec2 p) {\n"
    "    float r = length(p), rel = mod(degrees(atan(p.y, p.x)) - ysp_param(0), 360.0);\n"
    "    if (rel >= ysp_param(1) || r < ysp_param(2)) return 0.0;\n"
    "    int k = int(floor(r / ysp_param(3))) + int(floor(rel / ysp_param(4)));\n"
    "    return ((k & 1) == 0 ? 1.0 : -1.0) * ysp_param(5);\n"
    "}\n";
/* Tile 5: p0 the barrel coefficient, p1 the target's size in texels. The
 * target holds g in [-1, 1] in red (a grating drawn with dir 1, 1, 1). */
static const char* lens_body =
    "float ysp_main(vec2 p) {\n"
    "    vec2 q = p / ysp_size.xy;\n"
    "    vec2 d = q * (1.0 + ysp_param(0) * dot(q, q));\n"
    "    if (abs(d.x) > 1.0 || abs(d.y) > 1.0) return 0.0;\n"
    "    vec2 x = (d * 0.5 + 0.5) * ysp_param(1);\n"
    "    return ysp_bilinear(ysp_tex0, x, vec4(0.0, 0.0, ysp_param(1), ysp_param(1))).r;\n"
    "}\n";
/* Tile 6: p0 carrier c/px, p1 the mask's size in texels, p2 phase. */
static const char* mask_body =
    "float ysp_main(vec2 p) {\n"
    "    vec2 x = (p / ysp_size.xy * 0.5 + 0.5) * ysp_param(1);\n"
    "    float m = ysp_bilinear(ysp_tex0, x, vec4(0.0, 0.0, ysp_param(1), ysp_param(1))).r;\n"
    "    return m * cos(2.0 * ysp_PI * (ysp_param(0) * p.x - ysp_param(2)));\n"
    "}\n";

static yscr_screen scr;
static ygfx_gfx gfx;
static ytl_timeline tl;
static ytl_event tl_events[8];
static ygfx_stim cph, cm_sine, cm_noise, wedge, tgt_grating, lens, masked;
static const ygfx_bind binds[] = {
    { .stim = &cph,         .param = YGFX_P_CONTRAST,  .channel = CPH   },
    { .stim = &cm_sine,     .param = YGFX_P_P0 + 3,    .channel = ENV   },
    { .stim = &cm_noise,    .param = YGFX_P_P0 + 3,    .channel = ENV   },
    { .stim = &wedge,       .param = YGFX_P_P0 + 0,    .channel = ROT   },
    { .stim = &wedge,       .param = YGFX_P_P0 + 5,    .channel = POL   },
    { .stim = &tgt_grating, .param = YGFX_P_PHASE,     .channel = DRIFT },
    { .stim = &masked,      .param = YGFX_P_P0 + 2,    .channel = DRIFT },
};
static int64_t planned[RING];
static unsigned char mask_px[MASK * MASK];

static ygfx_stim user(const char* body, const char* name, float x, float y, const float* p, int n, ygfx_tex tex) {
    ygfx_pipeline_desc pd;
    ygfx_user_desc d;
    memset(&pd, 0, sizeof pd);
    pd.body = body; pd.mode = YGFX_MODULATION; pd.name = name;
    memset(&d, 0, sizeof d);
    d.pipe = ygfx_pipeline(&gfx, &pd);
    if (!d.pipe.id) fprintf(stderr, "gfx_gratings: %s: %s\n", name, ygfx_error(&gfx));
    d.x = x; d.y = y; d.w = d.h = TILE; d.contrast = 0.4f;
    d.aperture = YGFX_CIRCLE; d.edge = YGFX_EDGE_COSINE; d.edge_width = 12;
    d.p = p; d.n_p = n; d.tex = tex;
    return ygfx_user(&d);
}

/* A track that repeats keys every period, forever. */
static int track(int ch, const ytl_key* keys, int n, int64_t period) {
    ytl_track t;
    memset(&t, 0, sizeof t);
    t.keys = keys; t.n_keys = n; t.period_ns = period;
    return ytl_set_track(&tl, ch, BASE, &t);
}

int main(int argc, char** argv) {
    yscr_desc sd;
    ygfx_desc gd;
    ytl_desc td;
    yscr_frame f;
    ytl_event fired[8];
    double tf = 4.0, seconds = 0, worst = 0, tf_ach = NAN;
    int i, sim = 0, fullscreen = 0, refused = 0, dropped = 0, frames = 0, rc, first = 1;
    int64_t t0 = 0, x0 = 0, y0 = 0;
    double sx = 0, sy = 0, sxx = 0, sxy = 0, nfit = 0;   /* x a flip's measured onset, y its planned one */
    ygfx_tex target, mask;

    memset(&sd, 0, sizeof sd);
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--sim")) sim = 1;
        else if (!strcmp(argv[i], "--fullscreen")) fullscreen = 1;
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "--tf") && i + 1 < argc) tf = atof(argv[++i]);
        else { fprintf(stderr, "usage: gfx_gratings [--sim] [--fullscreen] [--seconds S] [--tf HZ]\n"); return 2; }
    }
    if (!(tf > 0 && tf <= 30) || !(seconds >= 0)) { fprintf(stderr, "gfx_gratings: --tf takes 0..30 Hz, --seconds >= 0\n"); return 2; }
    if (sim && seconds == 0) seconds = 2;
    if (sim) sd.backend = YSCR_BACKEND_SIM;
    sd.windowed = !fullscreen;
    sd.window_w = 960;
    sd.window_h = 640;
    if (!yscr_open(&scr, &sd)) { fprintf(stderr, "gfx_gratings: %s\n", yscr_error(&scr)); return 1; }
    memset(&gd, 0, sizeof gd);
    gd.screen = &scr;
    gd.background[0] = gd.background[1] = gd.background[2] = 0.5f;
    if (!ygfx_open(&gfx, &gd)) { fprintf(stderr, "gfx_gratings: %s\n", ygfx_error(&gfx)); return 1; }

    /* the target of tile 5 and the image of tile 6 */
    {
        ygfx_target_desc t;
        ygfx_texture_desc m;
        int x, y;
        memset(&t, 0, sizeof t);
        t.w = t.h = TGT;
        target = ygfx_target(&gfx, &t);
        for (y = 0; y < MASK; y++)
            for (x = 0; x < MASK; x++) {
                double u = (x + 0.5) / MASK * 2 - 1, v = (y + 0.5) / MASK * 2 - 1, r2 = u * u / 0.45 + v * v / 0.7;
                double m0 = exp(-r2 * r2 * 2), dark = 0;
                dark += 0.8 * exp(-((u + 0.3) * (u + 0.3) + (v + 0.2) * (v + 0.2)) / 0.01);   /* eyes */
                dark += 0.8 * exp(-((u - 0.3) * (u - 0.3) + (v + 0.2) * (v + 0.2)) / 0.01);
                dark += 0.6 * exp(-(u * u / 0.06 + (v - 0.4) * (v - 0.4) / 0.005));            /* mouth */
                m0 *= dark > 1 ? 0 : 1 - dark;
                mask_px[y * MASK + x] = (unsigned char)(255.0 * m0 + 0.5);
            }
        memset(&m, 0, sizeof m);
        m.w = m.h = MASK; m.format = YGFX_R8; m.data = mask_px;
        mask = ygfx_texture(&gfx, &m);
        if (!target.id || !mask.id) { fprintf(stderr, "gfx_gratings: %s\n", ygfx_error(&gfx)); return 1; }
    }
    {
        ygfx_grating_desc g;
        static const float p_sine[6] = { 1 / 8.0f, 1 / 120.0f, 1, 0, 0, 4 };
        static const float p_noise[6] = { 1 / 8.0f, 1 / 120.0f, 1, 0, 1, 4 };
        static const float p_wedge[6] = { 0, 60, 12, 15, 15, 1 };
        static const float p_lens[2] = { -0.25f, (float)TGT };   /* barrel */
        static const float p_mask[3] = { 1 / 16.0f, (float)MASK, 0 };
        static const ygfx_tex none = { 0 };
        memset(&g, 0, sizeof g);
        g.x = -300; g.y = -150; g.w = TILE; g.sf = 1 / 30.0f;
        g.aperture = YGFX_CIRCLE; g.edge = YGFX_EDGE_COSINE; g.edge_width = 12;
        cph = ygfx_grating(&g);
        cm_sine = user(cm_body, "cm sine", 0, -150, p_sine, 6, none);
        cm_noise = user(cm_body, "cm noise", 300, -150, p_noise, 6, none);
        wedge = user(wedge_body, "wedge", -300, 150, p_wedge, 6, none);
        wedge.contrast = 0.8f;
        lens = user(lens_body, "lens", 0, 150, p_lens, 2, target);
        masked = user(mask_body, "mask", 300, 150, p_mask, 3, mask);
        memset(&g, 0, sizeof g);
        g.w = g.h = TGT * 1.5f; g.sf = 1 / 24.0f; g.ori = 30;   /* turned: covers the whole target */
        g.dir[0] = g.dir[1] = g.dir[2] = 1;   /* the target holds g itself */
        tgt_grating = ygfx_grating(&g);
        if (!cm_sine.pipe.id || !cm_noise.pipe.id || !wedge.pipe.id || !lens.pipe.id || !masked.pipe.id) return 1;
    }

    /* the tracks, all on one base anchored at the first frame */
    {
        static ytl_key cph_keys[3], env_keys[2], rot_keys[2], pol_keys[2], drift_keys[2];
        int64_t T = (int64_t)llround(1e9 / tf);
        memset(&td, 0, sizeof td);
        td.events = tl_events;
        td.event_capacity = 8;
        td.n_channels = N_CHANNELS;
        if (!ytl_open(&tl, &td)) { fprintf(stderr, "gfx_gratings: %s\n", ytl_error(&tl)); return 1; }
        /* a cosine ease from +0.5 to -0.5 over half a period is 0.5 cos(2 pi f t) */
        cph_keys[0].time_ns = 0;     cph_keys[0].value = 0.5f;  cph_keys[0].ease = YTL_EASE_COSINE;
        cph_keys[1].time_ns = T / 2; cph_keys[1].value = -0.5f; cph_keys[1].ease = YTL_EASE_COSINE;
        cph_keys[2].time_ns = T;     cph_keys[2].value = 0.5f;
        env_keys[1].time_ns = 2 * S_NS;  env_keys[1].value = 1;      /* 0.5 Hz sawtooth */
        rot_keys[1].time_ns = 10 * S_NS; rot_keys[1].value = 360;    /* 0.1 rev/s */
        pol_keys[0].value = 1;  pol_keys[0].ease = YTL_EASE_STEP; /* reversals at 4 Hz */
        pol_keys[1].time_ns = S_NS / 8; pol_keys[1].value = -1; pol_keys[1].ease = YTL_EASE_STEP;
        drift_keys[1].time_ns = S_NS; drift_keys[1].value = 1;       /* 1 Hz drift */
        if (track(CPH, cph_keys, 3, T) < 0 || track(ENV, env_keys, 2, 2 * S_NS) < 0 ||
            track(ROT, rot_keys, 2, 10 * S_NS) < 0 || track(POL, pol_keys, 2, S_NS / 4) < 0 ||
            track(DRIFT, drift_keys, 2, S_NS) < 0) {
            fprintf(stderr, "gfx_gratings: a track was refused\n");
            return 1;
        }
    }
    printf("gfx_gratings: counterphase %.3g Hz | CM sine | CM noise\n"
           "              wedge        | target through a lens | image as a mask\n", tf);

    while ((rc = yscr_begin(&scr, &f)) == YSCR_OK) {
        const float* v;
        if (first) { ytl_anchor(&tl, BASE, f.onset, 0); t0 = f.onset; first = 0; }
        if (seconds > 0 && f.onset - t0 >= (int64_t)(seconds * 1e9)) break;
        planned[f.index % RING] = f.onset;
        for (i = 0; i < f.n_done; i++) {
            const yscr_record* r = &f.done[i];
            double x, y;
            if (r->onset == 0 || f.index - r->index >= RING) continue;
            if (nfit == 0) { x0 = r->onset; y0 = planned[r->index % RING]; }
            x = (double)(r->onset - x0) * 1e-9;
            y = (double)(planned[r->index % RING] - y0) * 1e-9;
            sx += x; sy += y; sxx += x * x; sxy += x * y; nfit++;
            dropped += r->dropped ? 1 : 0;
        }
        ytl_evaluate(&tl, &(ytl_frame){ f.onset, f.period, f.index }, fired, 8);
        ygfx_apply(binds, (int)(sizeof binds / sizeof binds[0]), ytl_values(&tl));
        v = ytl_values(&tl);
        {   /* the track against the closed form at the planned onset */
            double want = 0.5 * cos(2 * PI * tf * (double)(f.onset - t0) * 1e-9);
            double e = fabs((double)v[CPH] - want);
            if (e > worst) worst = e;
        }
        yscr_mark(&scr, YSCR_PHASE_EVALUATE);
        ygfx_begin(&gfx, &f);
        ygfx_begin_target(&gfx, target, (const float[4]){ 0, 0, 0, 0 });
        refused += ygfx_draw(&gfx, &tgt_grating) != YGFX_OK;
        ygfx_end_target(&gfx);
        refused += ygfx_draw(&gfx, &cph) != YGFX_OK;
        refused += ygfx_draw(&gfx, &cm_sine) != YGFX_OK;
        refused += ygfx_draw(&gfx, &cm_noise) != YGFX_OK;
        refused += ygfx_draw(&gfx, &wedge) != YGFX_OK;
        refused += ygfx_draw(&gfx, &lens) != YGFX_OK;
        refused += ygfx_draw(&gfx, &masked) != YGFX_OK;
        if (refused && frames == 0) fprintf(stderr, "gfx_gratings: a draw was refused: %s\n", ygfx_error(&gfx));
        ygfx_end(&gfx);
        yscr_flip(&scr);
        frames++;
    }
    if (rc != YSCR_OK && rc != YSCR_QUIT) fprintf(stderr, "gfx_gratings: %s\n", yscr_error(&scr));
    ygfx_close(&gfx);
    yscr_close(&scr);

    /* the achieved frequency: the phase drawn against the time it was on the screen */
    if (nfit > 2 && nfit * sxx - sx * sx > 0) {
        tf_ach = tf * (nfit * sxy - sx * sy) / (nfit * sxx - sx * sx);
        printf("counterphase: %.6f Hz achieved for %.6f Hz asked (%.0f flip records, %d with dropped vblanks); "
               "the track's largest error %.2e\n", tf_ach, tf, nfit, dropped, worst);
    } else printf("counterphase: too few flip records to measure\n");
    printf("%d frames, %d draws refused\n", frames, refused);
    if (refused) return 1;
    if (sim && (!(fabs(tf_ach - tf) <= 1e-9 * tf) || worst > 1e-6 || dropped)) {
        fprintf(stderr, "gfx_gratings: --sim: achieved %.9f Hz for %.9f, track error %.2e, %d drops\n", tf_ach, tf,
                worst, dropped);
        return 1;
    }
    if (sim) printf("--sim: the track and the achieved frequency as planned\n");
    return 0;
}
