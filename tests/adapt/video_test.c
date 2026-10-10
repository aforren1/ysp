/* video_test.c - self-checking test for ysp/video.h's core: the due-frame
 * rule against an exact model, the cadence of every rate on 60 and 30 Hz,
 * onset noise (and the never-early control that fails under it), slips
 * against a model of the clock and the grid, display drops, decode stalls
 * and the catch-up seek, seeks across GOPs, loop, end, manual mode, pause
 * and resume, the timeline's annotations against the frames shown, the
 * follow clock, the records field by field, the canonical check at open,
 * the frame sequence container in every format (raw and QOI, file, memory
 * and reader), the QOI codec, the YUV conversion against a double
 * reference, XXH64 against its published vectors, replay, the heap, the
 * soundtrack on ysp/audio.h's scripted device (every sample on its frame,
 * the movie clock on the audio's, controls, loop, late start, refusals),
 * and Media Foundation on generated clips (Windows, YVID_TEST_MEDIA).
 * No framework: it returns 0 when every check passed and 1 after printing
 * each failure.
 *
 * No GPU, no display, no pl_mpeg: a scripted decoder (the Video decoder
 * extension) and scripted display frames on a virtual clock, and ysp/gfx.h
 * on the simulated display with a recording backend, so every upload is
 * checked byte for byte. One case runs the decode thread for real.
 *
 *     gcc -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Werror -O2 -Iinclude \
 *         -o video_test tests/adapt/video_test.c -lm -pthread -ldl && ./video_test
 *     cl /nologo /W4 /WX /Iinclude tests\adapt\video_test.c
 *
 * With YVID_TEST_QOI_REF and qoi.h (phoboslab/qoi ffb2d2c) on the include
 * path, the encoder's bytes are compared with the reference encoder's.
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#ifndef YSCR_NO_SDL
#define YSCR_NO_SDL
#endif
#ifndef YVID_NO_PL_MPEG
#define YVID_NO_PL_MPEG
#endif
static int g_virtual = 1;
static long long g_vt = 1000000000000LL;
static long long vnow(void);
#define YVID__NOW() ((int64_t)vnow())

/* ysp/audio.h first, on the same virtual clock, with no miniaudio: the
 * soundtrack's cases drive its scripted device */
#define YAU_NO_MINIAUDIO
static void vsleep_until(long long t);
#define YAU__NOW() ((int64_t)vnow())
#define YAU__SLEEP_UNTIL(t) vsleep_until((long long)(t))
#define YSP_AUDIO_IMPLEMENTATION
#include "ysp/audio.h"

#define YSP_VIDEO_IMPLEMENTATION
#include "ysp/video.h"

#ifdef YVID_TEST_QOI_REF
    #if defined(_MSC_VER)
        #pragma warning(push)
        #pragma warning(disable: 4244 4267)
    #endif
    #define QOI_IMPLEMENTATION
    #include "qoi.h"
    #if defined(_MSC_VER)
        #pragma warning(pop)
    #endif
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static long long vnow(void) { return g_virtual ? g_vt : (long long)yrt_now_ns(); }
static void vsleep_until(long long t) {
    if (!g_virtual) { yrt_sleep_until(t, 0); return; }
    if (t > g_vt) g_vt = t;
}

/* ---------------------------------------------------------------- harness */

static int g_failures = 0;
static const char* g_case = "";
static void fail(int line, const char* what) {
    fprintf(stderr, "video_test [%s]: FAIL at line %d: %s\n", g_case, line, what);
    g_failures++;
}
static void fail_i(int line, const char* what, long long got, long long want) {
    fprintf(stderr, "video_test [%s]: FAIL at line %d: %s (got %lld, want %lld)\n", g_case, line, what, got, want);
    g_failures++;
}
#define CHECK(c) do { if (!(c)) fail(__LINE__, #c); } while (0)
#define CHECK_I(got, want) do { long long g_ = (long long)(got), w_ = (long long)(want); \
    if (g_ != w_) fail_i(__LINE__, #got " == " #want, g_, w_); } while (0)

static uint64_t g_rng = 88172645463325252ull;
static uint64_t rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17; return g_rng; }
static double urnd(void) { return (double)(rnd() >> 11) / 9007199254740992.0; }

/* ------------------------------------------------------- scripted decoder */

#define FW 4
#define FH 2
#define FB (FW * FH * 4)

typedef struct sdec {
    int32_t  num, den, gop, format;
    int64_t  frames;          /* the stream's length                           */
    int      report_frames;   /* the stream reports its length                 */
    int      random_access;
    int      timescale;       /* 0: out.index; else pts in these units         */
    uint8_t  matrix, range, transfer, primaries, siting;
    int32_t  w, h;
    int64_t  pos;
    int      fail_open;
    /* faults */
    int64_t  stall_frame;     /* this frame is not decodable until ...         */
    int64_t  stall_until;     /* ... this virtual time                         */
    int      use_hold;        /* ... or, with use_hold, while hold is set: the */
    uint32_t hold;            /* frame thread clears it (an atomic store)      */
    int      stall_all;       /* every frame from stall_frame on stalls: an    */
                              /* outage, not one bad frame a seek can pass     */
    int64_t  bad_ts_frame;    /* this frame reports the next frame's time      */
    int64_t  bad_hash_frame;
    int64_t  end_early;       /* the stream ends here (< frames)               */
    int64_t  fail_frame;
    /* counts */
    int64_t  seeks, discards, decodes, last_key;
} sdec;

static sdec g_sd;

static void sd_fill(int64_t i, uint8_t* p) {
    int k;
    for (k = 0; k < 8; k++) p[k] = (uint8_t)((uint64_t)i >> (8 * k));
    for (k = 8; k < FB; k++) p[k] = (uint8_t)(i * 7 + k);
}

static int sd_open(void* ctx, const yvid_decoder_open* in, yvid_stream* out, char* err, size_t cap) {
    sdec* s = (sdec*)ctx;
    (void)in;
    if (s->fail_open) { snprintf(err, cap, "scripted failure"); return YVID_ERR_DECODER; }
    out->w = s->w; out->h = s->h; out->format = s->format;
    out->fps_num = s->num; out->fps_den = s->den;
    out->frames = s->report_frames ? s->frames : -1;
    out->gop = s->gop; out->codec = YVID_CODEC_CUSTOM;
    out->matrix = s->matrix; out->range = s->range; out->transfer = s->transfer;
    out->primaries = s->primaries; out->siting = s->siting;
    out->timescale = s->timescale;
    out->caps = YVID_DEC_CPU | (s->random_access ? YVID_DEC_RANDOM_ACCESS : 0u);
    s->pos = 0;
    return YVID_OK;
}

static int sd_next(void* ctx, yvid_planes* dst, yvid_out* out) {
    sdec* s = (sdec*)ctx;
    int64_t i = s->pos;
    if (i >= s->frames || (s->end_early > 0 && i >= s->end_early)) return YVID_ENDED;
    if (s->stall_frame >= 0 && (i == s->stall_frame || (s->stall_all && i > s->stall_frame)) &&
        (s->use_hold ? yvid__ld32(&s->hold) != 0 : vnow() < s->stall_until))
        return YVID_PENDING;
    if (i == s->fail_frame) return YVID_ERR_DECODER;
    s->pos++;
    if (!dst) { s->discards++; return YVID_OK; }
    s->decodes++;
    sd_fill(i, dst->data[0]);
    {
        int64_t ti = i == s->bad_ts_frame ? i + 1 : i;
        if (s->timescale > 0) {
            out->index = -1;
            /* the container's truncated time, as Media Foundation gives */
            out->pts = ti * s->den * s->timescale / s->num;
        } else {
            out->index = ti;
        }
    }
    out->hash = yvid_xxh64(dst->data[0], FB, 0) ^ (i == s->bad_hash_frame ? 1u : 0u);
    out->flags = YVID_OUT_HAS_HASH | (i % s->gop == 0 ? YVID_OUT_KEYFRAME : 0u);
    return YVID_OK;
}

static int sd_seek(void* ctx, int64_t key, int64_t key_pts) {
    sdec* s = (sdec*)ctx;
    (void)key_pts;
    if (!s->random_access && key % s->gop != 0) return YVID_ERR_ARG;
    s->pos = key;
    s->seeks++;
    s->last_key = key;
    return YVID_OK;
}

static void sd_close(void* ctx) { (void)ctx; }

static const yvid_decoder g_dec = { YVID_DECODER_VERSION, "scripted", sd_open, sd_next, sd_seek, sd_close, NULL };

static void sd_default(sdec* s, int32_t num, int32_t den, int64_t frames, int gop) {
    memset(s, 0, sizeof *s);
    s->num = num; s->den = den; s->frames = frames; s->gop = gop;
    s->format = YVID_FMT_RGBA8; s->w = FW; s->h = FH;
    s->report_frames = 1;
    s->random_access = gop == 1;
    s->stall_frame = -1; s->bad_ts_frame = -1; s->bad_hash_frame = -1; s->fail_frame = -1;
}

/* ------------------------------------------------ gfx with a recording backend */

static yscr_screen g_scr;
static ygfx_gfx g_gfx;
static ygfx_backend g_rec;
static ygfx__null g_null_ctx;
static int64_t g_up_n;
static int64_t g_up_frame;         /* the index coded in the last RGBA8 upload */
static uint64_t g_up_hash;
static size_t g_up_bytes;          /* w x h x bpp of the texture, set by the case */
static uint8_t g_up_copy[64 * 64 * 16];
/* A planar upload is one call per plane, Y first: each frame's planes are
 * collected in order from g_planes_off = 0, and the Y plane is kept. */
static uint8_t g_planes[1 << 20];
static size_t g_planes_off;
#if YVID__MF
/* Only the Media Foundation cases read these; elsewhere clang's
 * -Wunused-but-set-global (emcc 6) fails the build. */
static int g_up_w;
static const uint8_t* g_up_y;
#endif
static int g_plane_i;     /* planes uploaded since the frame began */
static uint8_t g_up_row32[8192];   /* the Y plane's row 32, copied at upload: the
                                    * slot goes back to the decoder after it */

static int rec_update(void* c, uint32_t id, int x, int y, int w, int h, const void* data, size_t stride) {
    const uint8_t* p = (const uint8_t*)data;
    int k;
    (void)c; (void)id; (void)x; (void)y; (void)w; (void)h; (void)stride;
    g_up_n++;
#if YVID__MF
    g_up_w = w;
    if (g_plane_i == 0) g_up_y = p;
#endif
    if (g_plane_i++ == 0) {
        if (h > 32 && stride <= sizeof g_up_row32) memcpy(g_up_row32, p + (size_t)32 * stride, stride);
    }
    if (stride > 0 && g_planes_off + (size_t)h * stride <= sizeof g_planes) {
        memcpy(g_planes + g_planes_off, p, (size_t)h * stride);
        g_planes_off += (size_t)h * stride;
    }
    g_up_frame = 0;
    for (k = 0; k < 8; k++) g_up_frame = (int64_t)((uint64_t)g_up_frame | (uint64_t)p[k] << (8 * k));
    g_up_hash = g_up_bytes ? yvid_xxh64(p, g_up_bytes, 0) : 0;
    if (g_up_bytes <= sizeof g_up_copy) memcpy(g_up_copy, p, g_up_bytes);
    return YGFX_OK;
}

static void gfx_open(void) {
    yscr_desc d;
    ygfx_desc gd;
    memset(&d, 0, sizeof d);
    d.backend = YSCR_BACKEND_SIM;
    if (!yscr_open(&g_scr, &d)) { fprintf(stderr, "sim screen: %s\n", yscr_error(&g_scr)); exit(1); }
    g_rec = ygfx__null_backend;
    g_rec.texture_update = rec_update;
    memset(&gd, 0, sizeof gd);
    gd.screen = &g_scr;
    gd.backend = &g_rec;
    gd.backend_ctx = &g_null_ctx;
    gd.width = 64; gd.height = 64;
    if (!ygfx_open(&g_gfx, &gd)) { fprintf(stderr, "gfx: %s\n", ygfx_error(&g_gfx)); exit(1); }
}

/* ------------------------------------------------------------ records */

static unsigned char g_ring_mem[YRT_RING_BYTES(65536)];
static yrt_ring g_ring;

#define MAXD 120000
typedef struct frec {
    int64_t t, frame, due, mt;
    uint32_t flags, shows;
    int dec, why, tier, have;
} frec;
static frec g_fr[MAXD];
typedef struct drec { int64_t t, first, count; int why; int64_t display; } drec;
static drec g_dr[20000];
static int g_ndr;
static yrt_event g_oth[4096];
static int g_noth;

static void rec_reset(void) {
    yrt_ring_desc rd;
    memset(g_fr, 0, sizeof g_fr);
    g_ndr = 0;
    g_noth = 0;
    rd.memory = g_ring_mem;
    rd.bytes = sizeof g_ring_mem;
    yrt_ring_open(&g_ring, &rd);
}

static void drain(void) {
    static yrt_event ev[2048];
    int n, i;
    while ((n = yrt_ring_drain(&g_ring, ev, 2048)) > 0) {
        for (i = 0; i < n; i++) {
            const yrt_event* e = &ev[i];
            if (e->source != YRT_SRC_VIDEO) continue;
            if (e->kind == YVID_EV_FRAME) {
                uint32_t d = e->u.u32[9];
                if (d < MAXD) {
                    frec* r = &g_fr[d];
                    r->t = (int64_t)e->t_ns; r->frame = e->u.i64[0]; r->due = e->u.i64[1]; r->mt = e->u.i64[2];
                    r->flags = e->u.u32[6]; r->shows = e->u.u32[8];
                    r->dec = (int)YVID_EV_DECISION_OF(e->u.u16[14]);
                    r->why = (int)YVID_EV_WHY_OF(e->u.u16[14]);
                    r->tier = (int)YVID_EV_TIER_OF(e->u.u16[14]);
                    r->have++;
                }
            } else if (e->kind == YVID_EV_DROP) {
                if (g_ndr < 20000) {
                    drec* r = &g_dr[g_ndr++];
                    r->t = (int64_t)e->t_ns; r->first = e->u.i64[0]; r->count = e->u.i64[1];
                    r->why = e->u.u16[14]; r->display = e->u.u32[9];
                }
            } else if (g_noth < 4096) {
                g_oth[g_noth++] = *e;
            }
        }
    }
}

static int count_other(uint16_t kind) {
    int i, n = 0;
    for (i = 0; i < g_noth; i++) n += g_oth[i].kind == kind;
    return n;
}
static const yrt_event* find_other(uint16_t kind, int nth) {
    int i;
    for (i = 0; i < g_noth; i++) if (g_oth[i].kind == kind && nth-- == 0) return &g_oth[i];
    return NULL;
}

/* ------------------------------------------------------------ display */

typedef struct disp {
    int64_t  T0;
    double   P;               /* true period, ns                               */
    int64_t  P_report;        /* f.period                                      */
    double   noise_ns;        /* uniform +-                                    */
    int64_t  k, vb;
    yscr_record cur;        /* the flip of the frame being drawn             */
    yscr_record prev;       /* the previous flip's record, completed          */
    int      has_prev;
    int      late_next;       /* this frame's flip shows this many vblanks late */
    int      not_shown;       /* this frame's flip is skipped                  */
    int      tier;
    uint16_t flags;           /* for this frame's flip record                  */
    int      started;
    int      hold;            /* this frame's flip record arrives a frame late */
    yscr_record held;
    int      has_held;
    yscr_record done[2];    /* f.done: every record completed since the last */
    int      n_done;
} disp;

static int64_t disp_t(const disp* d, int64_t vb) { return d->T0 + (int64_t)floor((double)vb * d->P + 0.5); }

static void disp_init(disp* d, double hz, double ppm) {
    memset(d, 0, sizeof *d);
    d->T0 = 2000000000000LL;
    d->P = 1e9 / hz / (1.0 + ppm * 1e-6);
    d->P_report = (int64_t)floor(d->P + 0.5);
    d->tier = YSCR_TIER_SIM;
}

/* The next display frame. The previous flip's record arrives with it, as
 * yscr_begin() delivers it; late_next, not_shown and flags set before the
 * call describe the flip of the frame drawn before. */
static void disp_next(disp* d, yscr_frame* f) {
    int64_t onset;
    if (d->started) {
        int late = d->late_next;
        d->prev = d->cur;
        d->prev.dropped = (uint32_t)late;
        d->prev.onset = d->not_shown ? 0 : disp_t(d, d->vb + late);
        d->prev.planned = disp_t(d, d->vb);
        d->prev.tier = (uint8_t)d->tier;
        d->prev.flags = (uint16_t)(d->flags | (d->not_shown ? YSCR_FLIP_SKIPPED : 0));
        d->late_next = 0;
        d->not_shown = 0;
        d->flags = 0;
        d->has_prev = 1;
        d->vb += 1 + late;
        d->k++;
    }
    {
        /* f.done: a held record first, then the previous frame's, unless
         * that one is held in turn */
        int nd = 0, hold = d->hold;
        d->hold = 0;
        if (d->has_held) { d->done[nd++] = d->held; d->has_held = 0; }
        if (d->has_prev && hold) { d->held = d->prev; d->has_held = 1; }
        else if (d->has_prev) d->done[nd++] = d->prev;
        d->n_done = nd;
    }
    d->started = 1;
    onset = disp_t(d, d->vb);
    if (d->noise_ns > 0) onset += (int64_t)floor((urnd() * 2.0 - 1.0) * d->noise_ns + 0.5);
    memset(f, 0, sizeof *f);
    f->onset = onset;
    f->period = d->P_report;
    f->index = d->k;
    f->vblank = d->vb;
    f->last = d->n_done > 0 ? &d->done[d->n_done - 1] : NULL;
    f->done = d->n_done > 0 ? d->done : NULL;
    f->n_done = d->n_done;
    memset(&d->cur, 0, sizeof d->cur);
    d->cur.index = d->k;
    d->cur.target = onset;
    g_vt = onset - d->P_report / 2;
    g_planes_off = 0;   /* the next upload's first plane is Y */
    g_plane_i = 0;
}

/* ------------------------------------------------------------ movie */

static yvid_movie g_mv;

static void desc_default(yvid_desc* vd) {
    memset(vd, 0, sizeof *vd);
    vd->decoder = &g_dec;
    vd->decoder_ctx = &g_sd;
    vd->inline_decode = true;
    vd->ring = &g_ring;
    vd->refresh_num = 60; vd->refresh_den = 1;
    vd->ahead = 6;
}

static int open_mv(const yvid_desc* vd) {
    g_up_bytes = FB;
    if (!yvid_open(&g_mv, &g_gfx, vd)) { fprintf(stderr, "video_test [%s]: open: %s\n", g_case, yvid_error(&g_mv)); return 0; }
    return 1;
}

/* Runs n display frames; returns the last update's code. */
static ytl_event g_tl_ev[8192];
static ytl_timeline g_tl;
static ytl_timeline* g_eval_tl;   /* run() and settle() evaluate it */
static void tl_open(void);

static void eval_tl(const yscr_frame* f) {
    ytl_frame tf;
    if (!g_eval_tl) return;
    tf.onset = f->onset; tf.period = f->period; tf.index = f->index;
    ytl_evaluate(g_eval_tl, &tf, NULL, 0);
}

static int run(disp* d, int64_t n, int* codes_bad) {
    yscr_frame f;
    int64_t i;
    int rc = YVID_OK;
    for (i = 0; i < n; i++) {
        disp_next(d, &f);
        rc = yvid_update(&g_mv, &f);
        eval_tl(&f);
        if (rc < 0 && codes_bad) (*codes_bad)++;
        if ((i & 255) == 0) drain();
    }
    drain();
    return rc;
}

/* One more display frame, so the last frame's flip record arrives. */
static void settle(disp* d) {
    yscr_frame f;
    disp_next(d, &f);
    yvid_update(&g_mv, &f);
    eval_tl(&f);
    drain();
}

/* An independent model of the due frame: exact rationals in long double
 * would round, so in integers with the definition itself: the largest i
 * with ceil(i den 1e9 / num) <= x. */
static int64_t model_due(int32_t num, int32_t den, int64_t x) {
    int64_t i;
    if (x < 0) return -1;
    i = (int64_t)floor((double)x * num / ((double)den * 1e9));
    while (i > 0 && yvid_frame_time(num, den, i) > x) i--;
    while (yvid_frame_time(num, den, i + 1) <= x) i++;
    return i;
}

/* ================================================================ cases */

static void test_pure(void) {
    int64_t i;
    int k;
    g_case = "pure";
    /* frame times: the ceiling of the exact value */
    CHECK_I(yvid_frame_time(30, 1, 1), 33333334);
    CHECK_I(yvid_frame_time(30, 1, 3), 100000000);
    CHECK_I(yvid_frame_time(24000, 1001, 1), 41708334);
    CHECK_I(yvid_frame_time(24000, 1001, 24000), INT64_C(1001000000000));
    CHECK_I(yvid_frame_time(60000, 1001, 432000), INT64_C(7207200000000));
    /* due: against the definition at and around every boundary */
    for (k = 0; k < 4; k++) {
        static const int32_t nums[4] = { 30, 24000, 25, 60000 }, dens[4] = { 1, 1001, 1, 1001 };
        for (i = 0; i < 3000; i++) {
            int64_t t = yvid_frame_time(nums[k], dens[k], i);
            CHECK_I(yvid_due(nums[k], dens[k], t, 0), i);
            CHECK_I(yvid_due(nums[k], dens[k], t - 1, 0), i - 1);
            CHECK_I(yvid_due(nums[k], dens[k], t + 8333333, 0), model_due(nums[k], dens[k], t + 8333333));
        }
        /* far out: two hours and 2^62 ns */
        for (i = 0; i < 2000; i++) {
            int64_t x = (int64_t)(urnd() * 7.2e12);
            CHECK_I(yvid_due(nums[k], dens[k], x, 0), model_due(nums[k], dens[k], x));
        }
        CHECK(yvid_due(nums[k], dens[k], (INT64_C(1) << 62) - 1, 0) > 0);
    }
    CHECK_I(yvid_due(30, 1, -1, 0), -1);
    CHECK_I(yvid_lead_ns(0.0, 16666667), 8333333);
    CHECK_I(yvid_lead_ns(YVID_LEAD_NONE, 16666667), 0);
    CHECK_I(yvid_lead_ns(0.25, 16666667), 4166666);
    CHECK_I(yvid_lead_ns(0.5, 0), 0);
    /* the lead rule is ysp/timeline.h's own: the same truncation */
    CHECK_I(yvid_lead_ns(0.5, 16666667), (int64_t)(0.5 * (double)16666667));
    /* XXH64: python-xxhash 3 vectors */
    {
        uint8_t b[768];
        for (k = 0; k < 768; k++) b[k] = (uint8_t)(k & 255);
        CHECK(yvid_xxh64("", 0, 0) == UINT64_C(0xef46db3751d8e999));
        CHECK(yvid_xxh64("a", 1, 0) == UINT64_C(0xd24ec4f1a98c6e5b));
        CHECK(yvid_xxh64("abc", 3, 0) == UINT64_C(0x44bc2cf5ad770999));
        CHECK(yvid_xxh64(b, 100, 0) == UINT64_C(0x6ac1e58032166597));
        CHECK(yvid_xxh64(b, 768, 0) == UINT64_C(0x8e03c838c596036f));
        CHECK(yvid_xxh64("", 0, 12345) == UINT64_C(0x95584af7701f808d));
        CHECK(yvid_xxh64(b, 768, 12345) == UINT64_C(0x6ecac135863f12b2));
        /* streaming in uneven pieces gives the same */
        {
            yvid__xxh s;
            yvid__xxh_init(&s, 0);
            yvid__xxh_update(&s, b, 1); yvid__xxh_update(&s, b + 1, 30); yvid__xxh_update(&s, b + 31, 2);
            yvid__xxh_update(&s, b + 33, 700); yvid__xxh_update(&s, b + 733, 35);
            CHECK(yvid__xxh_digest(&s) == UINT64_C(0x8e03c838c596036f));
        }
    }
}

static void test_qoi(void) {
    static uint8_t img[61 * 37 * 4], back[61 * 37 * 4], enc[61 * 37 * 5 + 64];
    int t, i;
    g_case = "qoi";
    for (t = 0; t < 7; t++) {
        int64_t n;
        int w = t == 0 ? 1 : 61, h = t == 0 ? 1 : 37;
        if (t == 6) {
            /* steps around every DIFF and LUMA bound: g by -34..33, r and b
             * within -9..8 of g's step */
            int g = 128, r = 128, b = 128;
            for (i = 0; i < w * h; i++) {
                int dg = (int)(rnd() % 68) - 34;
                g = (g + dg) & 255;
                r = (r + dg + (int)(rnd() % 18) - 9) & 255;
                b = (b + dg + (int)(rnd() % 18) - 9) & 255;
                img[4 * i] = (uint8_t)r; img[4 * i + 1] = (uint8_t)g; img[4 * i + 2] = (uint8_t)b; img[4 * i + 3] = 255;
            }
        } else
        for (i = 0; i < w * h * 4; i++) {
            switch (t) {
            case 0: case 1: img[i] = (uint8_t)rnd(); break;                       /* noise: RGBA */
            case 2: img[i] = (uint8_t)((i / 4) % 7 == 0 ? 200 : 10); break;       /* runs */
            case 3: img[i] = (uint8_t)(i / 4 + (i & 3)); break;                   /* small steps: DIFF, LUMA */
            case 4: img[i] = (uint8_t)((i & 3) == 3 ? 255 : ((i / 4) * 37) & 255); break;
            default: img[i] = (uint8_t)((i / 4) % 64 < 3 ? 0 : (i & 3) == 3 ? 255 : (i / 256) * 11); break;
            }
        }
        n = yvid_qoi_encode(img, w, h, 4, enc, sizeof enc);
        CHECK(n > 22);
        CHECK_I(yvid_qoi_decode(enc, (size_t)n, back, w, h), YVID_OK);
        CHECK(memcmp(img, back, (size_t)(w * h * 4)) == 0);
#ifdef YVID_TEST_QOI_REF
        {
            qoi_desc qd;
            int rn = 0;
            void* ref;
            qd.width = (unsigned)w; qd.height = (unsigned)h; qd.channels = 4; qd.colorspace = 1;
            ref = qoi_encode(img, &qd, &rn);
            CHECK(ref != NULL);
            CHECK_I(rn, n);
            CHECK(ref && rn == n && memcmp(ref, enc, (size_t)n) == 0);
            free(ref);
        }
#endif
        /* every truncation fails cleanly, never overruns */
        for (i = 0; i < n - 8; i += 1 + (int)(n / 50)) CHECK(yvid_qoi_decode(enc, (size_t)i, back, w, h) < 0);
        CHECK(yvid_qoi_decode(enc, (size_t)n, back, w + 1, h) < 0);
    }
#ifdef YVID_TEST_QOI_REF
    printf("video_test: QOI bytes compared with the reference qoi.h\n");
#endif
}

/* The conversion against the same chroma weights and matrix in double. */
static void test_yuv(void) {
    /* rgba through a uint32_t array: the conversion stores whole pixels and
     * refuses an unaligned buffer, and a uint8_t array has no 4-byte
     * alignment (it happened to have it everywhere but macOS) */
    static uint32_t rgba32[37 * 23];
    static uint8_t planes[37 * 23 * 2];
    uint8_t* rgba = (uint8_t*)rgba32;
    static int16_t rows[256];
    int fm, mx, rg, st, ch, worst = 0;
    g_case = "yuv";
    CHECK(sizeof rows >= yvid_yuv_rows_bytes(37));
    for (fm = 0; fm < 2; fm++)
    for (mx = YVID_MATRIX_BT601; mx <= YVID_MATRIX_BT2020; mx++)
    for (rg = YVID_RANGE_LIMITED; rg <= YVID_RANGE_FULL; rg++)
    for (st = YVID_SITING_LEFT; st <= YVID_SITING_CENTER; st++)
    for (ch = 0; ch < 2; ch++) {
        int32_t format = fm ? YVID_FMT_NV12 : YVID_FMT_I420;
        int32_t w = 37, h = 23, cw = 19, chh = 12, x, y, i;
        yvid_planes p;
        double kr = mx == YVID_MATRIX_BT601 ? 0.299 : mx == YVID_MATRIX_BT709 ? 0.2126 : 0.2627;
        double kb = mx == YVID_MATRIX_BT601 ? 0.114 : mx == YVID_MATRIX_BT709 ? 0.0722 : 0.0593;
        yvid__planes_layout(format, w, h, planes, &p);
        for (i = 0; i < (int)sizeof planes; i++) planes[i] = (uint8_t)rnd();
        CHECK_I(yvid_yuv_to_rgba(&p, format, w, h, mx, rg, st, ch, rgba, w * 4, rows), YVID_OK);
        for (y = 0; y < h; y++) for (x = 0; x < w; x++) {
            double Y = planes[y * w + x], U, V, yp, up, vp, rgb[3];
            int c;
            /* chroma at this pixel */
            {
                double su = 0, sv = 0;
                int dy, dx;
                for (dy = 0; dy < 2; dy++) for (dx = 0; dx < 3; dx++) {
                    double wy, wx;
                    int cy, cx;
                    if (ch == YVID_CHROMA_NEAREST) { wy = dy == 0; wx = dx == 0; cy = y / 2; cx = x / 2; }
                    else {
                        cy = dy == 0 ? y / 2 : ((y & 1) ? y / 2 + 1 : y / 2 - 1);
                        wy = dy == 0 ? 0.75 : 0.25;
                        if (st == YVID_SITING_LEFT) {
                            cx = dx == 0 ? x / 2 : x / 2 + 1;
                            wx = (x & 1) ? (dx < 2 ? 0.5 : 0) : (dx == 0 ? 1.0 : 0);
                        } else {
                            cx = dx == 0 ? x / 2 : ((x & 1) ? x / 2 + 1 : x / 2 - 1);
                            wx = dx == 0 ? 0.75 : dx == 1 ? 0.25 : 0;
                        }
                    }
                    if (cy < 0) cy = 0;
                    if (cy >= chh) cy = chh - 1;
                    if (cx < 0) cx = 0;
                    if (cx >= cw) cx = cw - 1;
                    if (wx * wy == 0) continue;
                    if (fm) { su += wx * wy * p.data[1][cy * p.stride[1] + 2 * cx]; sv += wx * wy * p.data[1][cy * p.stride[1] + 2 * cx + 1]; }
                    else { su += wx * wy * p.data[1][cy * p.stride[1] + cx]; sv += wx * wy * p.data[2][cy * p.stride[2] + cx]; }
                }
                U = su; V = sv;
            }
            if (rg == YVID_RANGE_LIMITED) { yp = (Y - 16) / 219; up = (U - 128) / 224; vp = (V - 128) / 224; }
            else { yp = Y / 255; up = (U - 128) / 255; vp = (V - 128) / 255; }
            rgb[0] = yp + 2 * (1 - kr) * vp;
            rgb[2] = yp + 2 * (1 - kb) * up;
            rgb[1] = (yp - kr * rgb[0] - kb * rgb[2]) / (1 - kr - kb);
            for (c = 0; c < 3; c++) {
                double v = floor(rgb[c] * 255.0 + 0.5);
                int d;
                if (v < 0) v = 0;
                if (v > 255) v = 255;
                d = abs((int)v - (int)rgba[(y * w + x) * 4 + c]);
                if (d > worst) worst = d;
            }
            CHECK(rgba[(y * w + x) * 4 + 3] == 255);
        }
    }
    CHECK(worst <= 1);
    printf("video_test: YUV to RGBA8 worst difference from double: %d code\n", worst);
}

/* 30 on 60: every frame on screen exactly twice, DUE; the same under onset
 * noise of 2 and 20 us; never-early under 2 us of noise misplaces frames. */
static void test_multiple_and_noise(void) {
    static const double noise[3] = { 0, 2000, 20000 };
    int t;
    for (t = 0; t < 4; t++) {
        yvid_desc vd;
        disp d;
        int64_t k, wrong = 0, bad_why = 0;
        g_case = t < 3 ? "30 on 60" : "30 on 60, never early";
        rec_reset();
        sd_default(&g_sd, 30, 1, 400, 1);
        desc_default(&vd);
        if (t == 3) vd.lead = YVID_LEAD_NONE;
        if (!open_mv(&vd)) return;
        disp_init(&d, 60.0, 0);
        d.P = 1e9 / 60.0;
        d.noise_ns = t < 3 ? noise[t] : 2000;
        CHECK(yvid_play_at(&g_mv, YVID_ASAP) > 0);
        g_up_n = 0;
        run(&d, 700, NULL);
        settle(&d);
        CHECK_I(g_up_n, (int64_t)g_mv.info.shown);   /* one upload per frame shown, none per repeat */
        for (k = 0; k < 700; k++) {
            const frec* r = &g_fr[k];
            int64_t want = k / 2;
            if (!r->have) { wrong++; continue; }
            if (r->frame != want) wrong++;
            if (r->why != YVID_WHY_DUE) bad_why++;
            if (r->dec != (k % 2 == 0 ? YVID_SHOWN : YVID_REPEATED)) bad_why++;
        }
        if (t < 3) {
            CHECK_I(wrong, 0);
            CHECK_I(bad_why, 0);
            CHECK_I(g_ndr, 0);
            CHECK_I(g_mv.info.multiple, 2);
        } else {
            CHECK(wrong > 0);   /* the control: noise moves frames under never-early */
            printf("video_test: never-early under 2 us noise: %lld of 700 display frames wrong (lead 0.5: 0)\n", (long long)wrong);
        }
        yvid_close(&g_mv);
    }
}

/* A cadence that judders, against the exact model with the nominal grid. */
static void run_cadence(const char* name, int32_t num, int32_t den, double hz, int64_t n_disp,
                        int expect_lo, int expect_hi) {
    yvid_desc vd;
    disp d;
    int64_t k, wrong = 0, drift = 0, cad_rep = 0, cad_drop = 0, dropped = 0;
    int64_t shows[4096];
    int64_t frames = 4000;
    char line[1024];
    g_case = name;
    rec_reset();
    sd_default(&g_sd, num, den, frames, 1);
    desc_default(&vd);
    vd.refresh_num = (int32_t)hz; vd.refresh_den = 1;
    if (!open_mv(&vd)) return;
    disp_init(&d, hz, 0.01);   /* 10 ppb: no exact tie of onset and boundary */
    CHECK(yvid_play_at(&g_mv, YVID_ASAP) > 0);
    run(&d, n_disp, NULL);
    settle(&d);
    memset(shows, 0, sizeof shows);
    for (k = 0; k < n_disp; k++) {
        const frec* r = &g_fr[k];
        int64_t L = yvid_lead_ns(0.5, d.P_report);
        int64_t m = disp_t(&d, k) - disp_t(&d, 0);
        int64_t want = model_due(num, den, m + L);
        if (!r->have || r->frame != want) wrong++;
        if (r->why == YVID_WHY_DRIFT) drift++;
        if (r->dec == YVID_REPEATED && r->why == YVID_WHY_CADENCE) cad_rep++;
        if (r->have && r->frame < 4096) shows[r->frame]++;
    }
    for (k = 0; k < g_ndr; k++) {
        dropped += g_dr[k].count;
        if (g_dr[k].why == YVID_WHY_CADENCE) cad_drop += g_dr[k].count;
    }
    CHECK_I(wrong, 0);
    CHECK_I(drift, 0);
    CHECK_I(dropped, cad_drop);
    for (k = 1; k < 4096 && shows[k + 1]; k++) {
        if (shows[k] && (shows[k] < expect_lo || shows[k] > expect_hi)) { fail_i(__LINE__, "showings outside the cadence", shows[k], expect_hi); break; }
    }
    if (hz / ((double)num / den) > 1.0001) {
        /* one CADENCE repeat per frame shown more than floor(R/r) times */
        int64_t q = (int64_t)floor(hz / ((double)num / den)), extra = 0;
        for (k = 0; k < 4096; k++) if (shows[k] > q) extra += shows[k] - q;
        CHECK_I(cad_rep, extra);
        CHECK(cad_rep > 0);
    } else if (hz / ((double)num / den) < 0.9999) {
        CHECK(cad_drop > 0);
    } else {
        CHECK_I(cad_rep + cad_drop, 0);
    }
    yvid_describe(&g_mv, line, sizeof line);
    printf("video_test: %s: %s\n", name, strstr(line, "display") ? strstr(line, "display") : line);
    yvid_close(&g_mv);
}

static void test_cadences(void) {
    run_cadence("24000/1001 on 60", 24000, 1001, 60.0, 3000, 2, 3);
    run_cadence("25 on 60", 25, 1, 60.0, 3000, 2, 3);
    run_cadence("60 on 60", 60, 1, 60.0, 3000, 1, 1);
    run_cadence("60 on 30", 60, 1, 30.0, 1500, 1, 1);
    run_cadence("50 on 60", 50, 1, 60.0, 3000, 1, 2);
    /* the describe line states the 3:2 cadence and its durations */
    {
        yvid_desc vd;
        char line[1024];
        g_case = "describe cadence";
        rec_reset();
        sd_default(&g_sd, 24000, 1001, 100, 1);
        desc_default(&vd);
        if (open_mv(&vd)) {
            yvid_describe(&g_mv, line, sizeof line);
            CHECK(strstr(line, "cadence 3:2:3:2") != NULL);
            CHECK(strstr(line, "33.3 to 50.0 ms") != NULL);
            CHECK(g_mv.info.multiple == 0);
            yvid_close(&g_mv);
        }
        /* strict: refused, with the reason */
        vd.strict_cadence = true;
        CHECK(!yvid_open(&g_mv, &g_gfx, &vd));
        CHECK(strstr(yvid_error(&g_mv), "not a multiple") != NULL);
        sd_default(&g_sd, 30, 1, 100, 1);
        CHECK(open_mv(&vd));
        yvid_close(&g_mv);
    }
}

/* Slips: the display runs off its nominal rate against the ysp_rt clock.
 * Each DRIFT record must sit where an exact model of the clock and the grid
 * puts a frame on screen once or three times. */
static void test_drift(void) {
    /* not 500: at 500 ppm the slips fall exactly on frame boundaries */
    static const double ppms[2] = { 487.3, -487.3 };
    int t;
    for (t = 0; t < 2; t++) {
        yvid_desc vd;
        disp d;
        int64_t k, n = 9000, wrong = 0, drift_rec = 0, model_slips = 0, first_slip = -1;
        int64_t prev_due = -1, run_len = 0;
        g_case = t == 0 ? "drift +487.3 ppm" : "drift -487.3 ppm";
        rec_reset();
        sd_default(&g_sd, 30, 1, 20000, 1);
        desc_default(&vd);
        if (!open_mv(&vd)) return;
        disp_init(&d, 60.0, ppms[t]);
        CHECK(yvid_play_at(&g_mv, YVID_ASAP) > 0);
        run(&d, n, NULL);
        settle(&d);
        for (k = 0; k < n; k++) {
            const frec* r = &g_fr[k];
            int64_t L = yvid_lead_ns(0.5, d.P_report);
            int64_t m = disp_t(&d, k) - disp_t(&d, 0);
            int64_t want = model_due(30, 1, m + L);
            if (!r->have || r->frame != want) wrong++;
            if (r->why == YVID_WHY_DRIFT) drift_rec++;
            if (want != prev_due) {
                /* the frame before ran run_len display frames: a slip if not 2
                 * (the first frame's run starts at the anchor, so it counts) */
                if (prev_due >= 0 && (run_len != 2 || want != prev_due + 1)) {
                    model_slips += want - prev_due > 1 ? want - prev_due - 1 + (run_len != 2) : 1;
                    if (first_slip < 0) first_slip = k;
                }
                run_len = 1;
                prev_due = want;
            } else {
                run_len++;
            }
        }
        for (k = 0; k < g_ndr; k++) if (g_dr[k].why == YVID_WHY_DRIFT) drift_rec++;
        if (getenv("YVID_TEST_DEBUG")) {
            int shown = 0;
            for (k = 0; k < n && shown < 12; k++) if (g_fr[k].why == YVID_WHY_DRIFT) { printf("  drift at %lld frame %lld dec %d\n", (long long)k, (long long)g_fr[k].frame, g_fr[k].dec); shown++; }
        }
        CHECK_I(wrong, 0);
        CHECK(model_slips > 0);
        CHECK_I(drift_rec, model_slips);
        /* first slip: half a period of phase at 487.3 ppm, 1026 frames */
        CHECK(first_slip > 1000 && first_slip < 1050);
        printf("video_test: %s: %lld slips in %lld display frames, first at %lld (predicted 1026)\n",
               g_case, (long long)model_slips, (long long)n, (long long)first_slip);
        yvid_close(&g_mv);
    }
}

/* A flip that misses its vblank: DISPLAY_LATE drops, LATE on its record. */
static void test_display_late(void) {
    yvid_desc vd;
    disp d;
    yscr_frame f;
    int64_t k;
    int late_flags = 0, n_late_drop = 0;
    g_case = "display late";
    rec_reset();
    sd_default(&g_sd, 60, 1, 1000, 1);
    desc_default(&vd);
    if (!open_mv(&vd)) return;
    disp_init(&d, 60.0, 0);
    CHECK(yvid_play_at(&g_mv, YVID_ASAP) > 0);
    for (k = 0; k < 300; k++) {
        disp_next(&d, &f);
        yvid_update(&g_mv, &f);
        if (k == 100) d.late_next = 1;     /* flip 100 shows a vblank late */
        if (k == 200) d.late_next = 2;
    }
    settle(&d);
    drain();
    for (k = 0; k < 300; k++) if (g_fr[k].flags & YVID_F_LATE) late_flags++;
    for (k = 0; k < g_ndr; k++) {
        CHECK_I(g_dr[k].why, YVID_WHY_DISPLAY_LATE);
        n_late_drop += (int)g_dr[k].count;
    }
    CHECK_I(late_flags, 2);
    CHECK(g_fr[100].flags & YVID_F_LATE);
    CHECK_I(n_late_drop, 3);
    CHECK_I(g_ndr, 2);
    CHECK_I(g_fr[101].frame, g_fr[100].frame + 2);
    CHECK_I(g_fr[101].why, YVID_WHY_DUE);
    /* the flip's own onset is the record's time */
    CHECK_I(g_fr[100].t, disp_t(&d, 101));
    yvid_close(&g_mv);
}

/* Decode stalls of 1, 3 and 20 frames; a GOP decoder seeks to catch up. */
static void test_decode_stall(void) {
    static const int stalls[3] = { 1, 3, 20 };
    int t, gopc;
    for (gopc = 0; gopc < 2; gopc++)
    for (t = 0; t < 3; t++) {
        yvid_desc vd;
        disp d;
        int64_t k, late_rep = 0, late_drop = 0, dues_late = 0, shown_late = 0;
        char name[64];
        snprintf(name, sizeof name, "decode stall %d%s", stalls[t], gopc ? ", GOP 10" : "");
        g_case = name;
        rec_reset();
        sd_default(&g_sd, 60, 1, 2000, gopc ? 10 : 1);
        g_sd.stall_frame = 300;
        desc_default(&vd);
        if (!open_mv(&vd)) return;
        disp_init(&d, 60.0, 0);
        CHECK(yvid_play_at(&g_mv, YVID_ASAP) > 0);
        /* the stall begins when frame 300 would be decoded and lasts until
         * stalls[t] frames after it was due */
        g_sd.stall_until = disp_t(&d, 300) + (int64_t)(stalls[t] - 1) * d.P_report + d.P_report / 4;
        run(&d, 600, NULL);
        settle(&d);
        for (k = 0; k < 600; k++) {
            const frec* r = &g_fr[k];
            if (r->why == YVID_WHY_DECODE_LATE && r->dec == YVID_REPEATED) late_rep++;
            if (r->why == YVID_WHY_DECODE_LATE && r->dec == YVID_SHOWN) shown_late++;
            /* a frame shown before its time or on time is DUE; never late as DUE */
            if (r->dec == YVID_SHOWN && r->why == YVID_WHY_DUE && r->frame != k) dues_late++;
        }
        for (k = 0; k < g_ndr; k++) {
            CHECK_I(g_dr[k].why, YVID_WHY_DECODE_LATE);
            late_drop += g_dr[k].count;
        }
        /* random access skips the stalled frame once it is older than due;
         * a GOP decoder waits for its keyframe, then catches up */
        CHECK_I(late_rep, gopc ? (stalls[t] < 10 ? stalls[t] : 10) : 1);
        CHECK_I(late_drop, gopc ? (stalls[t] < 10 ? stalls[t] : 10) : 1);
        CHECK_I(shown_late, 0);
        CHECK_I(dues_late, 0);
        CHECK_I(g_mv.info.drops_by[YVID_WHY_DECODE_LATE], late_drop);
        if (gopc && stalls[t] == 20) { CHECK_I(g_sd.seeks, 2); CHECK_I(g_sd.discards, 0); }   /* open, then 310 */
        if (gopc && stalls[t] == 3) { CHECK_I(g_sd.seeks, 1); CHECK_I(g_sd.discards, 3); }
        CHECK_I(g_fr[599].frame, 599);   /* back on schedule */
        yvid_close(&g_mv);
    }
}

/* Seek forward and back across GOPs, resume ASAP and stay paused. */
static void test_seek(void) {
    static const int64_t targets[4] = { 133, 37, 40, 199 };
    yvid_desc vd;
    disp d;
    yscr_frame f;
    int s, k;
    int64_t skipped_first = -1;
    g_case = "seek";
    rec_reset();
    /* an annotation at every frame on the movie base: a seek skips the
     * ones it passes over, none fires late */
    tl_open();
    for (k = 0; k < 200; k++) {
        ytl_event e;
        memset(&e, 0, sizeof e);
        e.base = 4; e.kind = YTL_MARK; e.time = yvid_frame_time(30, 1, k); e.code = k;
        ytl_add(&g_tl, &e);
    }
    g_eval_tl = &g_tl;
    sd_default(&g_sd, 30, 1, 200, 8);
    desc_default(&vd);
    vd.timeline = &g_tl;
    vd.base = 4;
    if (!open_mv(&vd)) { g_eval_tl = NULL; return; }
    disp_init(&d, 60.0, 0);
    CHECK(yvid_play_at(&g_mv, YVID_ASAP) > 0);
    run(&d, 50, NULL);
    for (s = 0; s < 4; s++) {
        int64_t id = yvid_seek_frame(&g_mv, targets[s], YVID_ASAP);
        int64_t before = d.k + 1, first = -1;
        const yrt_event* ev;
        CHECK(id > 0);
        CHECK_I(yvid_result(&g_mv, id, NULL), YVID_PENDING);
        run(&d, 20, NULL);
        settle(&d);
        for (k = (int)before; k < (int)before + 20; k++) if (g_fr[k].have) { first = k; break; }
        CHECK(first >= 0);
        if (first >= 0) {
            CHECK_I(g_fr[first].frame, targets[s]);
            CHECK_I(g_fr[first].why, YVID_WHY_SEEK);
            CHECK_I(g_fr[first].dec, YVID_SHOWN);
        }
        CHECK_I(g_up_frame >= targets[s], 1);
        ev = find_other((uint16_t)YVID_EV_SEEK, s);
        CHECK(ev != NULL);
        if (ev) {
            CHECK_I(ev->u.i64[0], targets[s]);
            CHECK_I(ev->u.i64[1], targets[s] - targets[s] % 8);
            CHECK_I(ev->u.i64[2], targets[s] % 8);
            CHECK_I(ev->u.u32[8], id);
            if (first >= 0) CHECK_I((int64_t)ev->t_ns, disp_t(&d, first));
            if (s == 0) {
                /* forward from about frame 25: every annotation before 133
                 * not yet fired is skipped, and the record says how many */
                int ne = 0, i, n_sk = 0;
                const ytl_event* te = ytl_events(&g_tl, 4, &ne);
                for (i = 0; i < ne; i++) n_sk += (te[i].flags & YTL_EV_SKIPPED) != 0;
                skipped_first = ev->u.u32[9];
                CHECK_I(ev->u.u32[9], n_sk);
                CHECK(n_sk > 90 && n_sk < 120);
                printf("video_test: a seek to frame 133 skipped %d annotations; none fired late\n", n_sk);
            }
        }
        CHECK_I(yvid_result(&g_mv, id, NULL), YVID_OK);
    }
    /* the seek to the last frame then ends */
    CHECK_I(yvid_update(&g_mv, (disp_next(&d, &f), &f)), YVID_ENDED);
    /* stay paused: the target is shown and held; play resumes from it */
    {
        int64_t id = yvid_seek_frame(&g_mv, 100, YVID_STAY_PAUSED);
        CHECK(id > 0);
        run(&d, 10, NULL);
        CHECK_I(g_up_frame, 100);
        CHECK_I(g_mv.state, YVID__PAUSED);
        k = (int)d.k;
        CHECK(yvid_play_at(&g_mv, YVID_ASAP) > 0);
        run(&d, 10, NULL);
        settle(&d);
        {
            int j, first = -1;
            for (j = k + 1; j < k + 11; j++) if (g_fr[j].have) { first = j; break; }
            CHECK(first >= 0);
            if (first >= 0) { CHECK_I(g_fr[first].frame, 100); CHECK_I(g_fr[first + 2].frame, 101); }
        }
    }
    CHECK_I(g_mv.info.ts_mismatch, 0);
    /* no annotation fired late: each within a period of its time */
    {
        int ne = 0, i, late = 0;
        const ytl_event* te = ytl_events(&g_tl, 4, &ne);
        for (i = 0; i < ne; i++)
            if ((te[i].flags & YTL_EV_FIRED) && (te[i].residual >= d.P_report || te[i].residual < -d.P_report)) late++;
        CHECK_I(late, 0);
        CHECK((int64_t)g_mv.info.skipped >= skipped_first);
    }
    g_eval_tl = NULL;
    yvid_close(&g_mv);
}

/* Loop with a timeline: frame 0 follows N-1, LOOP records, and the movie
 * base rewinds so an annotation at frame 0 fires again each cycle. */

static void tl_open(void) {
    ytl_desc td;
    memset(&td, 0, sizeof td);
    td.events = g_tl_ev;
    td.event_capacity = 8192;
    if (!ytl_open(&g_tl, &td)) { fprintf(stderr, "timeline: %s\n", ytl_error(&g_tl)); exit(1); }
}

static void test_loop_end(void) {
    yvid_desc vd;
    disp d;
    yscr_frame f;
    int64_t k;
    int fired0 = 0, loops = 0, rc = 0;
    ytl_event e;
    g_case = "loop";
    rec_reset();
    tl_open();
    memset(&e, 0, sizeof e);
    e.base = 1; e.kind = YTL_MARK; e.time = 0; e.code = 1000;
    ytl_add(&g_tl, &e);
    sd_default(&g_sd, 30, 1, 10, 1);
    desc_default(&vd);
    vd.loop = true;
    vd.timeline = &g_tl;
    vd.base = 1;
    if (!open_mv(&vd)) return;
    disp_init(&d, 60.0, 0);
    CHECK(yvid_play_at(&g_mv, YVID_ASAP) > 0);
    for (k = 0; k < 100; k++) {
        ytl_event fired[8];
        ytl_frame tf;
        int n, i;
        disp_next(&d, &f);
        CHECK_I(yvid_update(&g_mv, &f), YVID_OK);
        tf.onset = f.onset; tf.period = f.period; tf.index = f.index;
        n = ytl_evaluate(&g_tl, &tf, fired, 8);
        for (i = 0; i < n && i < 8; i++) {
            if (fired[i].code == 1000) {
                fired0++;
                /* fires on the display frame that shows frame 0 */
                CHECK_I(g_mv.last.frame, 0);
                CHECK_I(g_mv.last.decision, YVID_SHOWN);
            }
        }
    }
    settle(&d);
    for (k = 0; k < 100; k++) {
        CHECK_I(g_fr[k].frame, (k / 2) % 10);
        if (g_fr[k].frame == 0 && g_fr[k].dec == YVID_SHOWN && k > 0) { CHECK_I(g_fr[k].why, YVID_WHY_LOOP); loops++; }
    }
    CHECK_I(fired0, 5);
    CHECK_I(loops, 4);
    {
        int i, n = 0;
        for (i = 0; i < g_noth; i++) n += g_oth[i].kind == YVID_EV_LOOP && (int64_t)g_oth[i].t_ns < disp_t(&d, 100);
        CHECK_I(n, 4);
    }
    CHECK_I(g_ndr, 0);
    yvid_close(&g_mv);

    /* end: the last frame stays, ENDED from the frame whose due is N */
    g_case = "end";
    rec_reset();
    sd_default(&g_sd, 30, 1, 10, 1);
    desc_default(&vd);
    if (!open_mv(&vd)) return;
    disp_init(&d, 60.0, 0);
    CHECK(yvid_play_at(&g_mv, YVID_ASAP) > 0);
    for (k = 0; k < 30; k++) {
        disp_next(&d, &f);
        rc = yvid_update(&g_mv, &f);
        CHECK_I(rc, k < 20 ? YVID_OK : YVID_ENDED);
    }
    settle(&d);
    CHECK_I(g_fr[19].frame, 9);
    CHECK_I(g_fr[20].have, 0);
    CHECK_I(count_other((uint16_t)YVID_EV_END), 1);
    CHECK_I(g_up_frame, 9);
    CHECK_I(yvid_play_at(&g_mv, YVID_ASAP), YVID_ERR_ORDER);
    yvid_close(&g_mv);
}

/* Manual mode: any frame on this display frame; the movie base is held at
 * its time, so its annotation fires there. */
static void test_manual(void) {
    static const int64_t seq[8] = { 5, 6, 7, 2, 2, 150, 151, 0 };
    yvid_desc vd;
    disp d;
    yscr_frame f;
    int s, gopc;
    for (gopc = 0; gopc < 2; gopc++) {
        g_case = gopc ? "manual, GOP 16" : "manual";
        rec_reset();
        tl_open();
        {
            int i;
            for (i = 0; i < 200; i++) {
                ytl_event e;
                memset(&e, 0, sizeof e);
                e.base = 2; e.kind = YTL_MARK; e.time = yvid_frame_time(30, 1, i); e.code = i;
                ytl_add(&g_tl, &e);
            }
            {
                /* between frames 5 and 6: a step to 6 fires it */
                ytl_event e;
                memset(&e, 0, sizeof e);
                e.base = 2; e.kind = YTL_MARK; e.time = yvid_frame_time(30, 1, 6) - 1000000; e.code = 1000;
                ytl_add(&g_tl, &e);
            }
        }
        sd_default(&g_sd, 30, 1, 200, gopc ? 16 : 1);
        desc_default(&vd);
        vd.timeline = &g_tl;
        vd.base = 2;
        if (!open_mv(&vd)) return;
        disp_init(&d, 60.0, 0);
        for (s = 0; s < 8; s++) {
            ytl_event fired[16];
            ytl_frame tf;
            int n, i, saw = 0;
            CHECK_I(yvid_show(&g_mv, seq[s]), YVID_OK);
            disp_next(&d, &f);
            CHECK_I(yvid_update(&g_mv, &f), YVID_OK);
            CHECK_I(g_up_frame, seq[s]);
            CHECK_I(g_mv.last.frame, seq[s]);
            CHECK_I(g_mv.last.why, YVID_WHY_MANUAL);
            tf.onset = f.onset; tf.period = f.period; tf.index = f.index;
            n = ytl_evaluate(&g_tl, &tf, fired, 16);
            /* a jump skips what it passes over: from 2 to 150 the 147
             * annotations 3 .. 149, so only 150's fires; a step fires one */
            {
                int ne = 0;
                const ytl_event* ev = ytl_events(&g_tl, 2, &ne);
                for (i = 0; i < ne; i++) if (ev[i].code == seq[s] && ev[i].frame == f.index) saw = 1;
            }
            /* skipped: 0 .. 4 at the first show; at the jump from 2 to 150
             * the 147 between and the one before 6 (pending again after the
             * step back to 2) */
            if (s == 1) { int k2, hit = 0; for (k2 = 0; k2 < n && k2 < 16; k2++) hit |= fired[k2].code == 1000; CHECK(hit); }
            if (s == 5) { CHECK_I(n, 1); CHECK_I(g_mv.info.skipped, 5 + 148); }
            if (s == 6) CHECK_I(n, 1);
            if (s != 4 && !saw) { fprintf(stderr, "  step %d frame %lld: %d fired\n", s, (long long)seq[s], n); fail(__LINE__, "annotation of the frame shown"); }
        }
        settle(&d);
        CHECK_I(g_mv.info.shown, 7);
        yvid_close(&g_mv);
    }
}

/* Pause and resume: no records while paused, the movie time continues. */
static void test_pause(void) {
    yvid_desc vd;
    disp d;
    int64_t k, id, paused_at = -1, n_have = 0;
    const yrt_event* ev;
    g_case = "pause";
    rec_reset();
    sd_default(&g_sd, 30, 1, 1000, 1);
    desc_default(&vd);
    if (!open_mv(&vd)) return;
    disp_init(&d, 60.0, 0);
    CHECK(yvid_play_at(&g_mv, YVID_ASAP) > 0);
    run(&d, 100, NULL);
    id = yvid_pause_at(&g_mv, disp_t(&d, 110) + 3000000);   /* nearest is 110 */
    CHECK(id > 0);
    run(&d, 50, NULL);
    ev = find_other((uint16_t)YVID_EV_PAUSE, 0);
    CHECK(ev != NULL);
    if (ev) { paused_at = (int64_t)ev->t_ns; CHECK_I(paused_at, disp_t(&d, 110)); CHECK_I(ev->u.i64[1], id); }
    for (k = 111; k < 150; k++) n_have += g_fr[k].have;
    CHECK_I(n_have, 0);
    CHECK(yvid_play_at(&g_mv, YVID_ASAP) > 0);
    run(&d, 20, NULL);
    settle(&d);
    /* display 110 showed frame 55 (its first showing); the resume shows 55's
     * second showing, then 56 */
    CHECK_I(g_fr[110].frame, 55);
    for (k = 150; k < 170; k++) if (g_fr[k].have) break;
    CHECK(k < 170);
    if (k < 170) { CHECK_I(g_fr[k].frame, 55); CHECK_I(g_fr[k + 1].frame, 56); CHECK_I(g_fr[k].dec, YVID_REPEATED); }
    CHECK_I(g_ndr, 0);
    yvid_close(&g_mv);
}

/* The timeline and the video use one rule: an annotation at frame i fires
 * on the display frame that first shows frame i (or, for a frame the
 * cadence drops, the first display frame past it), under onset noise. */
static void run_agree(const char* name, int32_t num, int32_t den, double hz, double noise) {
    yvid_desc vd;
    disp d;
    yscr_frame f;
    int64_t k, n = 2000, frames = 3000, mism = 0, fired_n = 0, twice = 0;
    static int64_t fired_at[4000];
    static int64_t first_show[4000];
    g_case = name;
    rec_reset();
    tl_open();
    for (k = 0; k < frames; k++) {
        ytl_event e;
        memset(&e, 0, sizeof e);
        e.base = 3; e.kind = YTL_MARK; e.time = yvid_frame_time(num, den, k); e.code = (int32_t)k;
        ytl_add(&g_tl, &e);
        fired_at[k] = -1;
        first_show[k] = -1;
    }
    sd_default(&g_sd, num, den, frames, 1);
    desc_default(&vd);
    vd.refresh_num = (int32_t)hz;
    vd.timeline = &g_tl;
    vd.base = 3;
    if (!open_mv(&vd)) return;
    disp_init(&d, hz, 0);
    d.noise_ns = noise;
    CHECK(yvid_play_at(&g_mv, YVID_ASAP) > 0);
    for (k = 0; k < n; k++) {
        ytl_event fired[16];
        ytl_frame tf;
        int nf, i;
        disp_next(&d, &f);
        yvid_update(&g_mv, &f);
        if (g_mv.has_last && g_mv.last.display == f.index && g_mv.last.decision == YVID_SHOWN && g_mv.last.frame < 4000)
            first_show[g_mv.last.frame] = f.index;
        tf.onset = f.onset; tf.period = f.period; tf.index = f.index;
        nf = ytl_evaluate(&g_tl, &tf, fired, 16);
        for (i = 0; i < nf && i < 16; i++) {
            int64_t c = fired[i].code;
            if (fired_at[c] >= 0) twice++;
            fired_at[c] = f.index;
            fired_n++;
        }
        if ((k & 255) == 0) drain();
    }
    for (k = 0; k < frames; k++) {
        int64_t want = first_show[k];
        if (fired_at[k] < 0 && want < 0) continue;
        if (want < 0) {
            /* dropped by the cadence: the first display frame showing a later one */
            int64_t j;
            for (j = k + 1; j < frames && first_show[j] < 0; j++) {}
            want = j < frames ? first_show[j] : -1;
        }
        if (fired_at[k] != want) mism++;
    }
    CHECK(fired_n > 100);
    CHECK_I(mism, 0);
    CHECK_I(twice, 0);   /* the base never rewound in play */
    yvid_close(&g_mv);
}

static void test_timeline_agreement(void) {
    run_agree("agree 23.976 on 60, 20 us noise", 24000, 1001, 60.0, 20000);
    run_agree("agree 30 on 60, 2 us noise", 30, 1, 60.0, 2000);
    run_agree("agree 60 on 30, 20 us noise", 60, 1, 30.0, 20000);
    /* a timeline with another lead: the movie takes it, and a desc.lead that
     * differs is refused */
    {
        yvid_desc vd;
        ytl_desc td;
        g_case = "agree lead";
        memset(&td, 0, sizeof td);
        td.events = g_tl_ev; td.event_capacity = 16; td.lead = YTL_LEAD_NONE;
        CHECK(ytl_open(&g_tl, &td));
        sd_default(&g_sd, 30, 1, 100, 1);
        desc_default(&vd);
        vd.timeline = &g_tl; vd.base = 1;
        CHECK(open_mv(&vd));
        CHECK(g_mv.info.lead == 0.0);
        yvid_close(&g_mv);
        vd.lead = 0.5;
        CHECK(!yvid_open(&g_mv, &g_gfx, &vd));
        CHECK(strstr(yvid_error(&g_mv), "one rule") != NULL);
    }
}

/* Following another clock (a soundtrack's fit), 200 ppm fast: the movie
 * time is the clock's; the residual stays within half a period. */
typedef struct sclock { int64_t t0; double ns_per_unit; } sclock;
static int64_t sc_unit_at(void* ctx, int64_t t) {
    const sclock* c = (const sclock*)ctx;
    return (int64_t)floor((double)(t - c->t0) / c->ns_per_unit + 0.5);
}

static void test_follow(void) {
    yvid_desc vd;
    disp d;
    yscr_frame f;
    sclock c;
    int64_t k, n = 6000, worst = 0, drift = 0, flagged = 0, first_drift = -1;
    g_case = "follow";
    rec_reset();
    sd_default(&g_sd, 30, 1, 10000, 1);
    desc_default(&vd);
    if (!open_mv(&vd)) return;
    disp_init(&d, 60.0, 0);
    c.t0 = d.T0 - 12345678;
    c.ns_per_unit = 1e9 / 48000.0 / (1.0 + 200e-6);    /* the device runs fast */
    CHECK(yvid_play_at(&g_mv, YVID_ASAP) > 0);
    for (k = 0; k < n; k++) {
        disp_next(&d, &f);
        CHECK_I(yvid_follow(&g_mv, &f, sc_unit_at, &c, 48000), YVID_OK);
        yvid_update(&g_mv, &f);
        if ((k & 255) == 0) drain();
    }
    settle(&d);
    for (k = 10; k < n; k++) {
        const frec* r = &g_fr[k];
        int64_t res;
        if (!r->have) continue;
        res = r->t - r->due;    /* how late on screen against the clock */
        if (r->dec == YVID_SHOWN && llabs(res) > worst) worst = llabs(res);
        if (r->why == YVID_WHY_DRIFT) { if (!drift) first_drift = k; drift++; }
        if (r->flags & YVID_F_AUDIO_CLOCK) flagged++;
        /* the movie time is the clock's */
        if (k == 3000) {
            int64_t want = (int64_t)floor((double)(sc_unit_at(&c, r->t) - sc_unit_at(&c, g_fr[0].t)) * 1e9 / 48000.0 + 0.5);
            CHECK(llabs(r->mt - want) < 30000);
        }
    }
    CHECK(worst <= d.P_report / 2 + 30000);
    /* 200 ppm of a 16.7 ms period is 3.3 us of phase per display frame: the
     * first slip after half a period (2500 frames), then one each 5000 */
    CHECK(drift >= 1 && drift <= 3);
    CHECK(first_drift > 2400 && first_drift < 2600);
    CHECK(flagged > n / 2);
    CHECK_I(count_other((uint16_t)YVID_EV_CLOCK), 1);
    printf("video_test: follow 200 ppm: worst |onset - due| of a shown frame %.3f ms, %lld slips in %.0f s\n",
           worst / 1e6, (long long)drift, n / 60.0);
    yvid_close(&g_mv);
}

/* The FRAME record field by field, and the flags from the flip records. */
static void test_records(void) {
    yvid_desc vd;
    disp d;
    yscr_frame f;
    int64_t k;
    g_case = "records";
    rec_reset();
    sd_default(&g_sd, 30, 1, 1000, 1);
    g_sd.timescale = 90000;
    g_sd.bad_ts_frame = 20;
    g_sd.bad_hash_frame = 25;
    desc_default(&vd);
    vd.movie_index = 7;
    vd.min_tier = 2;
    if (!open_mv(&vd)) return;
    disp_init(&d, 60.0, 0);
    d.tier = YSCR_TIER_SIM;
    CHECK(yvid_play_at(&g_mv, YVID_ASAP) > 0);
    for (k = 0; k < 80; k++) {
        disp_next(&d, &f);
        yvid_update(&g_mv, &f);
        if (k == 30) d.flags = YSCR_FLIP_ESTIMATED;
        if (k == 31) d.not_shown = 1;
        if (k == 60) d.hold = 1;   /* its record arrives with frame 62's, before 61's */
    }
    settle(&d);
    {
        const frec* r = &g_fr[10];
        CHECK_I(r->frame, 5);
        CHECK_I(r->dec, YVID_SHOWN);
        CHECK_I(r->t, disp_t(&d, 10));
        CHECK_I(r->due, disp_t(&d, 0) + yvid_frame_time(30, 1, 5));
        CHECK_I(r->mt, disp_t(&d, 10) - disp_t(&d, 0));
        CHECK_I(r->shows, 1);
        CHECK_I(r->tier, YSCR_TIER_SIM);
        CHECK(r->flags & YVID_F_BELOW_TIER);
        CHECK_I(g_fr[11].shows, 2);
        CHECK_I(g_fr[11].dec, YVID_REPEATED);
    }
    CHECK(g_fr[40].flags & YVID_F_TS_MISMATCH);
    CHECK(!(g_fr[42].flags & YVID_F_TS_MISMATCH));
    CHECK(g_fr[50].flags & YVID_F_HASH_MISMATCH);
    CHECK(g_fr[30].flags & YVID_F_ESTIMATED);
    /* a record that came a frame late, in f.done ahead of the newer one,
     * still completes its own frame with its own onset */
    CHECK(!(g_fr[60].flags & YVID_F_ESTIMATED));
    CHECK_I(g_fr[60].t, disp_t(&d, 60));
    CHECK(!(g_fr[61].flags & YVID_F_ESTIMATED));
    CHECK(g_fr[31].flags & YVID_F_NOT_SHOWN);
    CHECK_I(g_mv.info.ts_mismatch, 1);
    CHECK_I(g_mv.info.hash_mismatch, 1);
    {
        char line[1024];
        yvid_describe(&g_mv, line, sizeof line);
        CHECK(strstr(line, "NOT CANONICAL") != NULL);
    }
    /* every record's aux is the movie index; the OPEN record first */
    {
        const yrt_event* o = find_other((uint16_t)YVID_EV_OPEN, 0);
        CHECK(o != NULL);
        if (o) { CHECK_I(o->aux, 7); CHECK_I(o->u.i32[0], FW); CHECK_I(o->u.i32[3], 30); CHECK_I(o->u.i32[7], 2); }
    }
    {
        yvid_record r;
        memset(&r, 0, sizeof r);
        CHECK_I(yvid_last(&g_mv, &r), YVID_OK);
        CHECK_I(r.display, 80);
    }
    yvid_close(&g_mv);
}

/* The canonical check: each field refused alone, with its message. */
static void test_open_refusals(void) {
    yvid_desc vd;
    uint8_t idx[YVID__IDX_HDR + 8 * 100];
    yvid__canon c;
    int k;
    g_case = "open refusals";
    rec_reset();
    sd_default(&g_sd, 30, 1, 100, 1);
    desc_default(&vd);
    CHECK(open_mv(&vd)); yvid_close(&g_mv);
#define REFUSE(setup, needle) do { sd_default(&g_sd, 30, 1, 100, 1); desc_default(&vd); setup; \
        if (yvid_open(&g_mv, &g_gfx, &vd)) { fail(__LINE__, "opened: " #setup); yvid_close(&g_mv); } \
        else if (!strstr(yvid_error(&g_mv), needle)) { fprintf(stderr, "  message: %s\n", yvid_error(&g_mv)); fprintf(stderr, "  wanted: %s\n", needle); fail(__LINE__, "message lacks the reason"); } } while (0)
    REFUSE(g_sd.format = YVID_FMT_P010, "10-bit");
    REFUSE(g_sd.format = YVID_FMT_I420, "matrix is unspecified");
    REFUSE((g_sd.format = YVID_FMT_I420, g_sd.matrix = 2, g_sd.range = 1, g_sd.transfer = 2, g_sd.primaries = 2), "siting is unspecified");
    REFUSE((g_sd.format = YVID_FMT_NV12, g_sd.matrix = 2, g_sd.range = 0, g_sd.transfer = 2, g_sd.primaries = 2, g_sd.siting = 2), "range is unspecified");
    REFUSE(g_sd.num = 0, "rate");
    REFUSE((g_sd.num = 1000000, g_sd.den = 1), "rate");
    REFUSE(g_sd.gop = 0, "GOP");
    REFUSE(g_sd.report_frames = 0, "no frames");
    REFUSE(g_sd.fail_open = 1, "scripted failure");
    REFUSE(vd.gpu_path = YVID_PATH_SHARED, "SHARED GPU path is not built");
    REFUSE(vd.gpu_path = YVID_PATH_GPU, "YVID_PATH_GPU is for Media Foundation");
    REFUSE(vd.hw_decode = (yvid_hw)7, "hw_decode");
    REFUSE(vd.light = YVID_LIGHT_EOTF, "for YUV movies");
    REFUSE(vd.ahead = 99, "ahead");
    REFUSE(vd.lead = 1.5, "lead");
#if YVID__MF
    REFUSE(vd.backend = YVID_BACKEND_MF, "no index");
#else
    REFUSE(vd.backend = YVID_BACKEND_MF, "Media Foundation");
#endif
    /* an index that disagrees with the stream, field by field */
    memset(&c, 0, sizeof c);
    c.codec = YVID_CODEC_CUSTOM; c.w = FW; c.h = FH; c.format = YVID_FMT_RGBA8;
    c.fps_num = 30; c.fps_den = 1; c.frames = 100; c.gop = 1;
    c.matrix = 1; c.range = 2; c.transfer = 1; c.primaries = 1; c.siting = 1;
    yvid__index_header(idx, &c, 0, 0, 0);
    memset(idx + YVID__IDX_HDR, 0, 800);
    sd_default(&g_sd, 30, 1, 100, 1);
    desc_default(&vd);
    vd.index = idx; vd.index_size = sizeof idx;
    CHECK(open_mv(&vd));
    if (g_mv.open) {
        /* the index's hashes are zero: every frame is flagged */
        disp d;
        disp_init(&d, 60.0, 0);
        CHECK(yvid_play_at(&g_mv, YVID_ASAP) > 0);
        run(&d, 10, NULL);
        CHECK(g_mv.info.hash_mismatch == 0);   /* the stream's own hash wins */
        yvid_close(&g_mv);
    }
    {
        static const struct { int off; const char* needle; } fields[] = {
            { 20, "width" }, { 24, "height" }, { 28, "format" }, { 32, "rate numerator" }, { 48, "GOP" }, { 40, "frames" } };
        for (k = 0; k < 6; k++) {
            uint8_t bad[sizeof idx];
            memcpy(bad, idx, sizeof idx);
            yvid__w32(bad + fields[k].off, yvid__r32(bad + fields[k].off) + (fields[k].off == 40 ? (uint32_t)-1 : 1u));
            yvid__w64(bad + 120, yvid_xxh64(bad, 120, 0));
            REFUSE((vd.index = bad, vd.index_size = sizeof bad), fields[k].needle);
        }
        {
            uint8_t bad[sizeof idx];
            memcpy(bad, idx, sizeof idx);
            bad[30] ^= 1;   /* without fixing the hash */
            REFUSE((vd.index = bad, vd.index_size = sizeof bad), "damaged");
            memcpy(bad, idx, sizeof idx);
            REFUSE((vd.index = bad, vd.index_size = 100), "not a ysp_video index");
        }
    }
#undef REFUSE
}

/* A stream that ends before its index says, a decoder that fails. */
static void test_decoder_faults(void) {
    yvid_desc vd;
    disp d;
    int rc;
    g_case = "decoder faults";
    rec_reset();
    sd_default(&g_sd, 30, 1, 100, 1);
    g_sd.end_early = 40;
    desc_default(&vd);
    if (!open_mv(&vd)) return;
    disp_init(&d, 60.0, 0);
    CHECK(yvid_play_at(&g_mv, YVID_ASAP) > 0);
    rc = run(&d, 120, NULL);
    CHECK_I(rc, YVID_ERR_DECODER);
    CHECK(strstr(yvid_error(&g_mv), "ended at frame 40") != NULL);
    CHECK_I(g_up_frame, 39);   /* the last good frame stays */
    yvid_close(&g_mv);
    sd_default(&g_sd, 30, 1, 100, 1);
    g_sd.fail_frame = 10;
    desc_default(&vd);
    if (!open_mv(&vd)) return;
    CHECK(yvid_play_at(&g_mv, YVID_ASAP) > 0);
    rc = run(&d, 40, NULL);
    CHECK_I(rc, YVID_ERR_DECODER);
    CHECK(strstr(yvid_error(&g_mv), "frame 10") != NULL);
    yvid_close(&g_mv);
}

/* The same inputs twice: records equal byte for byte. Also the heap: no
 * call per frame. */
static yrt_event g_rep[2][8192];
static int g_nrep[2];

static void test_replay_and_heap(void) {
    int t;
    uint64_t h0 = 0, h1 = 0;
    for (t = 0; t < 2; t++) {
        yvid_desc vd;
        disp d;
        yscr_frame f;
        int64_t k;
        g_case = "replay";
        rec_reset();
        g_rng = 12345;
        sd_default(&g_sd, 24000, 1001, 5000, 4);
        g_sd.stall_frame = 200;
        desc_default(&vd);
        if (!open_mv(&vd)) return;
        disp_init(&d, 60.0, 30.0);
        g_sd.stall_until = disp_t(&d, 500);
        d.noise_ns = 20000;
        CHECK(yvid_play_at(&g_mv, YVID_ASAP) > 0);
        for (k = 0; k < 3000; k++) {
            if (k == 100) h0 = yvid_heap_calls();
            if (k == 1000) d.late_next = 1;
            if (k == 1500) yvid_seek_frame(&g_mv, 1200, YVID_ASAP);
            disp_next(&d, &f);
            yvid_update(&g_mv, &f);
            if (k == 2000) yvid_pause_at(&g_mv, YVID_ASAP);
            if (k == 2100) yvid_play_at(&g_mv, YVID_ASAP);
            if (g_nrep[t] < 8000) g_nrep[t] += yrt_ring_drain(&g_ring, g_rep[t] + g_nrep[t], 8000 - g_nrep[t]);
        }
        h1 = yvid_heap_calls();
        CHECK_I(h1 - h0, 0);
        yvid_close(&g_mv);
    }
    g_case = "replay";
    CHECK(g_nrep[0] > 2000);
    CHECK_I(g_nrep[0], g_nrep[1]);
    {
        int i, diff = 0;
        for (i = 0; i < g_nrep[0] && i < g_nrep[1]; i++) {
            /* tid and seq are the ring's; the rest must match */
            yrt_event a = g_rep[0][i], b = g_rep[1][i];
            a.tid = b.tid = 0; a.seq = b.seq = 0;
            if (a.kind == YVID_EV_OPEN || a.kind == YVID_EV_DECODE) a.t_ns = b.t_ns = 0;
            if (memcmp(&a, &b, sizeof a) != 0) diff++;
        }
        CHECK_I(diff, 0);
    }
}

/* ------------------------------------------------- frame sequence container */

static void fill_frame(int32_t format, int32_t w, int32_t h, int64_t i, uint8_t* p, size_t n) {
    size_t k;
    (void)w; (void)h;
    if (format == YVID_FMT_R16F || format == YVID_FMT_RGBA16F || format == YVID_FMT_R32F || format == YVID_FMT_RGBA32F) {
        float* fp = (float*)(void*)p;
        for (k = 0; k < n / 4; k++) fp[k] = (float)((double)((i * 131 + (int64_t)k * 7) % 1000) / 999.0);
        return;
    }
    for (k = 0; k < n; k++) p[k] = (uint8_t)(rnd() >> 13);
    (void)i;
}

static void test_seq(void) {
    static const int32_t formats[11] = { YVID_FMT_R8, YVID_FMT_RG8, YVID_FMT_RGBA8, YVID_FMT_R16F,
        YVID_FMT_RGBA16F, YVID_FMT_R32F, YVID_FMT_RGBA32F, YVID_FMT_R16, YVID_FMT_I420, YVID_FMT_NV12,
        YVID_FMT_RGBA8 };
    static uint8_t frames[12][21 * 13 * 16 + 64];
    static uint8_t padded[21 * 13 * 16 * 2];
    static uint32_t expect32[21 * 13 * 4];   /* the conversion needs 4-byte alignment */
    uint8_t* expect = (uint8_t*)expect32;
    static int16_t rows[1024];   /* >= yvid_yuv_rows_bytes(21) */
    const char* path = "video_test.yspseq";
    int t;
    g_case = "seq";
    CHECK(sizeof rows >= yvid_yuv_rows_bytes(21));
    for (t = 0; t < 11; t++) {
        yvid_seq w;
        yvid_seq_desc sd;
        yvid_desc vd;
        disp d;
        yscr_frame f;
        int32_t W = 21, H = 13, fmt = formats[t];
        int qoi = t == 10, k, src;
        yvid_planes pl;
        size_t fb = yvid__planes_layout(fmt, W, H, NULL, &pl);
        char name[64];
        snprintf(name, sizeof name, "seq %s%s", yvid__fmt_name(fmt), qoi ? " QOI" : "");
        g_case = name;
        memset(&sd, 0, sizeof sd);
        sd.path = path; sd.w = W; sd.h = H; sd.format = fmt; sd.compression = qoi ? YVID_SEQ_QOI : YVID_SEQ_RAW;
        sd.fps_num = 25; sd.fps_den = 1;
        if (yvid__is_yuv(fmt)) { sd.matrix = YVID_MATRIX_BT709; sd.range = YVID_RANGE_LIMITED; sd.transfer = YVID_TRC_BT1886; sd.primaries = YVID_PRIM_BT709; sd.siting = YVID_SITING_LEFT; }
        if (!yvid_seq_create(&w, &sd)) { fail(__LINE__, yvid_seq_error(&w)); continue; }
        for (k = 0; k < 12; k++) {
            const void* planes[3];
            int32_t strides[3];
            yvid_planes in;
            int p, y;
            fill_frame(fmt, W, H, k, frames[k], fb);
            /* hand it over with padded rows, to check the strides */
            yvid__planes_layout(fmt, W, H, frames[k], &in);
            {
                uint8_t* dst = padded;
                for (p = 0; p < 3; p++) {
                    planes[p] = NULL; strides[p] = 0;
                    if (!in.data[p]) continue;
                    strides[p] = in.stride[p] + 8;
                    planes[p] = dst;
                    for (y = 0; y < in.h[p]; y++) memcpy(dst + (size_t)y * (size_t)strides[p], in.data[p] + (size_t)y * (size_t)in.stride[p], (size_t)in.stride[p]);
                    dst += (size_t)strides[p] * (size_t)in.h[p];
                }
            }
            CHECK_I(yvid_seq_write(&w, planes, strides, NULL), YVID_OK);
        }
        CHECK_I(yvid_seq_close(&w), YVID_OK);
        /* from the file and from memory (test_reader covers a reader) */
        for (src = 0; src < 2; src++) {
            static uint8_t filebuf[1 << 20];
            size_t fsize = 0;
            {
                FILE* fh = fopen(path, "rb");
                CHECK(fh != NULL);
                if (fh) { fsize = fread(filebuf, 1, sizeof filebuf, fh); fclose(fh); }
            }
            memset(&vd, 0, sizeof vd);
            vd.inline_decode = true;
            vd.refresh_num = 50; vd.refresh_den = 1;
            vd.ahead = 3;
            if (src == 0) vd.path = path;
            else { vd.data = filebuf; vd.size = fsize; }
            g_up_bytes = yvid__is_yuv(fmt) ? 0 : (size_t)W * (size_t)H * (size_t)yvid__bpt(fmt);
            if (!yvid_open(&g_mv, &g_gfx, &vd)) { fail(__LINE__, yvid_error(&g_mv)); fprintf(stderr, "  %s\n", yvid_error(&g_mv)); break; }
            CHECK_I(g_mv.info.frames, 12);
            CHECK_I(g_mv.info.format, fmt);
            disp_init(&d, 50.0, 0);
            /* random access in manual mode: every frame value-exact */
            for (k = 0; k < 24; k++) {
                int64_t i = (int64_t)(rnd() % 12);
                CHECK_I(yvid_show(&g_mv, i), YVID_OK);
                disp_next(&d, &f);
                g_planes_off = 0; g_plane_i = 0;
                CHECK_I(yvid_update(&g_mv, &f), YVID_OK);
                if (yvid__is_yuv(fmt)) {
                    /* the planes go up as stored, for the gfx's video program */
                    size_t pb = yvid__planes_layout(fmt, W, H, NULL, NULL);
                    CHECK(g_planes_off == 0 || g_planes_off == pb);
                    if (g_planes_off) CHECK(memcmp(g_planes, frames[i], pb) == 0);
                    (void)expect; (void)rows;
                } else {
                    CHECK(memcmp(g_up_copy, frames[i], g_up_bytes) == 0);
                    CHECK(g_up_hash == yvid_xxh64(frames[i], g_up_bytes, 0));
                }
            }
            CHECK_I(g_mv.info.hash_mismatch, 0);
            CHECK_I(g_mv.info.ts_mismatch, 0);
            yvid_close(&g_mv);
        }
    }
    /* damage: a flipped byte in a frame is a hash mismatch; a header that was
     * never written is refused */
    {
        static uint8_t buf[1 << 20];
        FILE* fh;
        size_t n;
        yvid_desc vd;
        disp d;
        int k;
        g_case = "seq damage";
        fh = fopen(path, "rb");
        n = fh ? fread(buf, 1, sizeof buf, fh) : 0;
        if (fh) fclose(fh);
        CHECK(n > 8192);
        buf[4096 + 64 + 5] ^= 0x10;   /* frame 0's data (the RGBA8 QOI file: decodes, wrong) */
        memset(&vd, 0, sizeof vd);
        vd.inline_decode = true; vd.refresh_num = 50; vd.ahead = 2;
        vd.data = buf; vd.size = n;
        g_up_bytes = 21 * 13 * 4;
        if (yvid_open(&g_mv, &g_gfx, &vd)) {
            yscr_frame f;
            disp_init(&d, 50.0, 0);
            yvid_show(&g_mv, 0);
            disp_next(&d, &f);
            k = yvid_update(&g_mv, &f);
            CHECK(k == YVID_OK || k == YVID_ERR_DECODER);
            CHECK(g_mv.info.hash_mismatch == 1 || k == YVID_ERR_DECODER);
            yvid_close(&g_mv);
        } else {
            fail(__LINE__, yvid_error(&g_mv));
        }
        buf[4096 + 64 + 5] ^= 0x10;
        buf[72] ^= 1;   /* the header hash */
        CHECK(!yvid_open(&g_mv, &g_gfx, &vd));
        CHECK(strstr(yvid_error(&g_mv), "damaged") != NULL);
        buf[72] ^= 1;
        vd.size = 5000;   /* truncated: the index is past the end */
        CHECK(!yvid_open(&g_mv, &g_gfx, &vd));
        /* probe reads the header alone */
        {
            yvid_info info;
            char e[200];
            vd.size = n;
            CHECK(yvid_probe(&vd, &info, e, sizeof e));
            CHECK_I(info.frames, 12);
            CHECK_I(info.w, 21);
            CHECK_I(info.fps_num, 25);
        }
    }
    remove(path);
}

/* A reader source: the frame sequence through byte ranges in odd pieces. */
static uint8_t g_rd_buf[1 << 20];
static int64_t rd_read(void* ctx, int64_t off, void* buf, int64_t n) {
    (void)ctx;
    if (n > 777) n = 777;
    memcpy(buf, g_rd_buf + off, (size_t)n);
    return n;
}

static void test_reader(void) {
    yvid_seq w;
    yvid_seq_desc sd;
    yvid_desc vd;
    yvid_reader rd;
    disp d;
    yscr_frame f;
    static uint8_t px[9 * 7 * 4];
    const void* planes[3] = { px, NULL, NULL };
    int32_t strides[3] = { 9 * 4, 0, 0 };
    const char* path = "ysp_video_test_rd.yspseq";
    FILE* fh;
    size_t n;
    int k;
    g_case = "reader";
    memset(&sd, 0, sizeof sd);
    sd.path = path; sd.w = 9; sd.h = 7; sd.format = YVID_FMT_RGBA8; sd.fps_num = 30;
    sd.max_frames = 5;
    CHECK(yvid_seq_create(&w, &sd));
    for (k = 0; k < 5; k++) { memset(px, k + 1, sizeof px); CHECK_I(yvid_seq_write(&w, planes, strides, NULL), YVID_OK); }
    CHECK_I(yvid_seq_write(&w, planes, strides, NULL), YVID_ERR_FULL);   /* max_frames */
    CHECK_I(yvid_seq_close(&w), YVID_OK);
    fh = fopen(path, "rb");
    n = fh ? fread(g_rd_buf, 1, sizeof g_rd_buf, fh) : 0;
    if (fh) fclose(fh);
    remove(path);
    memset(&vd, 0, sizeof vd);
    rd.read = rd_read; rd.size = (int64_t)n;
    vd.reader = &rd;
    vd.inline_decode = true; vd.refresh_num = 60; vd.ahead = 2;
    g_up_bytes = sizeof px;
    if (!yvid_open(&g_mv, &g_gfx, &vd)) { fail(__LINE__, yvid_error(&g_mv)); return; }
    disp_init(&d, 60.0, 0);
    for (k = 4; k >= 0; k--) {
        yvid_show(&g_mv, k);
        disp_next(&d, &f);
        CHECK_I(yvid_update(&g_mv, &f), YVID_OK);
        CHECK_I(g_up_copy[0], k + 1);
        CHECK_I(g_up_copy[sizeof px - 1], k + 1);
    }
    yvid_close(&g_mv);
}

/* The decode thread for real, against the frame thread, on the ysp_rt clock. */
static void test_threaded(void) {
#if !defined(YRT_NO_THREADS)
    yvid_desc vd;
    yscr_frame f;
    int64_t k, t0, P = 4000000, last = -1, order_bad = 0, shown = 0;
    int64_t shown_after_stall = 0, shown_after_seek = 0, k_end, recover = -1;
    int rc = YVID_OK;
    g_case = "threaded";
    g_virtual = 0;
    rec_reset();
    sd_default(&g_sd, 250, 1, 1000, 5);
    /* a decoder that is not ready for a while (PENDING) on the real decode
     * thread: the slot it took stays with that thread (the free queue has
     * the frame thread as its only producer; ThreadSanitizer checks) */
    g_sd.stall_frame = 40;
    g_sd.stall_all = 1;
    g_sd.stall_until = yrt_now_ns() + 300000000;
    desc_default(&vd);
    vd.inline_decode = false;
    vd.refresh_num = 250;
    vd.ahead = 4;
    if (!open_mv(&vd)) { g_virtual = 1; return; }
    CHECK(yvid_play_at(&g_mv, YVID_ASAP) > 0);
    t0 = yrt_now_ns() + 20000000;
    k_end = (g_sd.stall_until - t0 + P - 1) / P;   /* the first display frame after the stall */
    for (k = 0; k < 400 && rc >= 0; k++) {
        memset(&f, 0, sizeof f);
        f.onset = t0 + k * P; f.period = P; f.index = k; f.vblank = k;
        yrt_sleep_until((int64_t)(f.onset - P / 2), 0);
        rc = yvid_update(&g_mv, &f);
        if (g_mv.has_last && g_mv.last.display == k && g_mv.last.decision == YVID_SHOWN) {
            if (g_mv.last.frame <= last) order_bad++;
            last = g_mv.last.frame;
            shown++;
            if (k > 100 && k <= 200) shown_after_stall++;
            if (k > 200) shown_after_seek++;
            if (k >= k_end && k <= 200 && recover < 0) recover = k - k_end;
        }
        if (k == 200) yvid_seek_frame(&g_mv, 50, YVID_ASAP), last = -1;
    }
    /* This test is for the threads (order, slot ownership, no deadlock, the
     * seek recovers), not throughput: on a shared CI runner (macOS,
     * 2026-10-08: shown <= 200 of 400) the sleeps and the decode thread are
     * late at will, and throughput is the video bench's. Before v0.2.1 no
     * frame was shown between the end of the stall and the seek (the decode
     * thread slept after PENDING). The recovery is measured here and bounded
     * loosely, 50 display frames (200 ms), for a loaded runner; the exact
     * count is test_threaded_stall's. */
    printf("video_test: threaded: shown %lld of 400 (after the stall %lld, after the seek %lld); "
           "the first frame %lld display frames after the stall\n",
           (long long)shown, (long long)shown_after_stall, (long long)shown_after_seek, (long long)recover);
    CHECK(rc >= 0);
    CHECK_I(order_bad, 0);
    CHECK(shown_after_seek >= 10);
    CHECK(recover >= 0 && recover <= 50);
    yvid_close(&g_mv);
    g_virtual = 1;
#endif
}

/* Waits until the decode thread has nothing to do: every slot full, or the
 * decoder PENDING. 0 after 2 s. */
static int wait_settled(void) {
    int64_t until = yrt_now_ns() + 2000000000;
    while (!yvid__ld32(&g_mv.idle)) {
        if (yrt_now_ns() > until) return 0;
        yrt_sleep_until(yrt_now_ns() + 100000, 0);
    }
    return 1;
}

/* The decode thread for real on a virtual display: after each update the
 * frame thread waits for the decode thread to settle, so the count of
 * display frames is exact. A decoder that is PENDING from frame 40 on until
 * display frame R must show the due frame on display frame R + 1 (the
 * update of R wakes the thread, which seeks to the due frame's keyframe
 * and decodes it), drop the frames passed over as DECODE_LATE, and then
 * show a new frame on every display frame. */
static void test_threaded_stall(void) {
#if !defined(YRT_NO_THREADS)
    yvid_desc vd;
    disp d;
    yscr_frame f;
    const int64_t R = 120, K = 200;
    int64_t k, first = -1, first_frame = -1, f30 = -1, shown_after = 0, late_drop = 0, settled_bad = 0;
    int i;
    g_case = "threaded stall";
    g_virtual = 0;   /* the decode thread reads the clock (decode times): the real one, not the test's variable */
    rec_reset();
    sd_default(&g_sd, 250, 1, 1000, 5);
    g_sd.stall_frame = 40;
    g_sd.stall_all = 1;
    g_sd.use_hold = 1;
    g_sd.hold = 1;
    desc_default(&vd);
    vd.inline_decode = false;
    vd.refresh_num = 250;
    vd.ahead = 4;
    if (!open_mv(&vd)) { g_virtual = 1; return; }
    if (g_mv.inline_mode) { yvid_close(&g_mv); g_virtual = 1; return; }   /* no decode thread on this target */
    disp_init(&d, 250.0, 0);
    CHECK(yvid_play_at(&g_mv, YVID_ASAP) > 0);
    for (k = 0; k < K; k++) {
        if (k == R) yvid__st32(&g_sd.hold, 0);
        disp_next(&d, &f);
        CHECK(yvid_update(&g_mv, &f) >= 0);
        if (g_mv.has_last && g_mv.last.display == k && g_mv.last.decision == YVID_SHOWN) {
            if (k == 30) f30 = g_mv.last.frame;
            if (k > R) {
                shown_after++;
                if (first < 0) { first = k; first_frame = g_mv.last.frame; }
            }
        }
        if (!settled_bad && !wait_settled()) settled_bad = 1;
        if ((k & 63) == 0) drain();
    }
    drain();
    for (i = 0; i < g_ndr; i++) if (g_dr[i].why == YVID_WHY_DECODE_LATE) late_drop += g_dr[i].count;
    printf("video_test: threaded stall: the first frame on display frame %lld (the stall ended before %lld), "
           "frame %lld; %lld dropped DECODE_LATE\n", (long long)first, (long long)R, (long long)first_frame, (long long)late_drop);
    CHECK_I(settled_bad, 0);
    CHECK_I(first, R + 1);
    CHECK(f30 >= 0);
    CHECK_I(first_frame, f30 + (R + 1 - 30));   /* the due frame, one a display frame */
    CHECK_I(late_drop, first_frame - 40);       /* 40 .. first_frame - 1 */
    CHECK_I(g_mv.info.drops_by[YVID_WHY_DECODE_LATE], late_drop);
    CHECK_I(shown_after, K - (R + 1));          /* back on schedule */
    yvid_close(&g_mv);
    g_virtual = 1;
#endif
}

/* ------------------------------------------------------------- soundtrack
 * ysp/audio.h on a scripted device (its Audio device extension) on the same
 * virtual clock as the display: a device clock at a set drift, position
 * reports stamped at the call, a tape of every output sample. The
 * soundtrack is a test source whose every sample says which it is, so each
 * output sample is checked against the sample that belongs on its frame. */

#define ATAPE (48000 * 60)
typedef struct adev {
    int64_t  T0;            /* ysp_rt time of stream frame -L's output          */
    double   drift_ppm;
    int64_t  L;             /* frames from render to output                      */
    int64_t  k, W;
    yau_render_fn render;
    void*    host;
    float    out[480 * 2];
} adev;
static adev g_ad;
static float g_atape[ATAPE * 2];

static double ad_kd(const adev* a) { return 1e9 / 48000.0 / (1.0 + a->drift_ppm * 1e-6); }
static int64_t ad_event(const adev* a) { return a->T0 + (int64_t)floor((double)(a->k * 480) * ad_kd(a) + 0.5); }
/* The stream frame on which t falls (nearest), from the truth. */
static int64_t ad_frame_of(const adev* a, int64_t t) {
    return (int64_t)ceil((double)(t - a->T0) / ad_kd(a) - (double)a->L - 0.5);
}
static int ad_open(void* ctx, const yau_device_open* in, yau_device_caps* caps, char* err, size_t cap) {
    (void)ctx; (void)in; (void)err; (void)cap;
    memset(caps, 0, sizeof *caps);
    caps->rate = 48000; caps->channels = 2; caps->out = YAU_OUT_F32;
    caps->map[0] = YAU_CH_FL; caps->map[1] = YAU_CH_FR;
    caps->period = 480; caps->buffer = 1440; caps->pos_source = YAU_POS_DEVICE; caps->tier = YAU_TIER_2;
    snprintf(caps->name, sizeof caps->name, "scripted");
    return 0;
}
static int ad_start(void* ctx, yau_render_fn r, void* host) { adev* a = (adev*)ctx; a->render = r; a->host = host; return 0; }
static void ad_stop(void* ctx) { (void)ctx; }
static void ad_close(void* ctx) { (void)ctx; }
static const yau_device g_adevice = { YAU_DEVICE_VERSION, "scripted", ad_open, ad_start, ad_stop, ad_close, NULL };

/* Renders every block whose callback is due by t, with a position report
 * stamped at the call. */
static void ad_until(adev* a, int64_t t) {
    while (ad_event(a) <= t) {
        yau_tick tk;
        int64_t e = ad_event(a), f;
        memset(&tk, 0, sizeof tk);
        tk.t_entry = e;
        f = (int64_t)floor((double)(e - a->T0) / ad_kd(a) + 1e-3);
        tk.pos = f - a->L > 0 ? f - a->L : 0;
        tk.pos_t = a->T0 + (int64_t)floor((double)f * ad_kd(a) + 0.5);
        a->render(a->host, a->out, 480, &tk);
        if (a->W + 480 <= ATAPE) memcpy(&g_atape[a->W * 2], a->out, sizeof a->out);
        a->W += 480;
        a->k++;
    }
}

static yau_audio g_au;
static float g_au_arena[1 << 18];

static bool au_open(double drift_ppm, int64_t T0) {
    yau_desc d;
    memset(&g_ad, 0, sizeof g_ad);
    memset(g_atape, 0, sizeof g_atape);
    g_ad.T0 = T0; g_ad.drift_ppm = drift_ppm; g_ad.L = 1900;
    memset(&d, 0, sizeof d);
    d.backend = YAU_BACKEND_CUSTOM;
    d.dev = &g_adevice;
    d.dev_ctx = &g_ad;
    d.arena = g_au_arena; d.arena_bytes = sizeof g_au_arena;
    d.ring = &g_ring;
    if (!yau_open(&g_au, &d)) { fprintf(stderr, "yau_open: %s\n", yau_error(&g_au)); return false; }
    return true;
}

/* The identity source: sample s of channel c is idv(s, c), never 0, exact
 * in a float and in a float plus 0.25. */
static float idv(int64_t s, int c) { return (float)((((s * 2 + c) % 4093) + 1)) / 8192.0f; }
/* A float WAV in memory (WAVE_FORMAT_IEEE_FLOAT) of `frames` identity
 * samples at `rate` Hz and `ch` channels; ysp/audio.h's WAV reader feeds it. */
static uint8_t g_wav[44 + 480000 * 2 * 4 + 64];
static size_t g_wav_n;
static void wav_le(uint8_t* p, uint32_t v, int n) { int k; for (k = 0; k < n; k++) p[k] = (uint8_t)(v >> (8 * k)); }
static void make_wav(uint32_t rate, uint16_t ch, int64_t frames) {
    uint32_t data = (uint32_t)(frames * ch * 4);
    int64_t i;
    int c;
    if (44 + (size_t)data > sizeof g_wav) { fail(__LINE__, "test WAV too long"); g_wav_n = 0; return; }
    memcpy(g_wav, "RIFF", 4); wav_le(g_wav + 4, 36 + data, 4); memcpy(g_wav + 8, "WAVEfmt ", 8);
    wav_le(g_wav + 16, 16, 4); wav_le(g_wav + 20, 3, 2); wav_le(g_wav + 22, ch, 2); wav_le(g_wav + 24, rate, 4);
    wav_le(g_wav + 28, rate * ch * 4, 4); wav_le(g_wav + 32, (uint32_t)ch * 4, 2); wav_le(g_wav + 34, 32, 2);
    memcpy(g_wav + 36, "data", 4); wav_le(g_wav + 40, data, 4);
    for (i = 0; i < frames; i++)
        for (c = 0; c < ch; c++) {
            float v = idv(i, c);
            memcpy(g_wav + 44 + (size_t)(i * ch + c) * 4, &v, 4);
        }
    g_wav_n = 44 + (size_t)data;
}
static void wav_desc(yvid_soundtrack_desc* sd) {
    memset(sd, 0, sizeof *sd);
    sd->data = g_wav;
    sd->size = g_wav_n;
}

/* One display frame: the audio device run up to the frame thread's time,
 * the fit moved to the frame thread, the follow, the update. */
static int snd_frame(disp* d, yscr_frame* f) {
    disp_next(d, f);
    ad_until(&g_ad, g_vt);
    yau_update(&g_au);
    CHECK(yvid_follow_audio(&g_mv, &g_au, f) >= 0);
    return yvid_update(&g_mv, f);
}

/* The movie time the audio truth gives at RT time t: the (fractional)
 * sample playing then, over the rate. */
static int64_t truth_mt(int64_t origin, int64_t t) {
    double x = (double)(t - g_ad.T0) / ad_kd(&g_ad) - (double)g_ad.L - (double)origin;
    return (int64_t)floor(x * 1e9 / 48000.0 + 0.5);
}

/* Output frames w0..w1-1 hold the soundtrack's sample (frame - origin) of
 * the pass, plus `tone` from frame t0 to t1-1. Returns the bad samples. */
static int64_t tape_check(int64_t w0, int64_t w1, int64_t origin, int64_t pass, int64_t t0, int64_t t1, float tone) {
    int64_t w, bad = 0;
    for (w = w0; w < w1 && w < ATAPE; w++) {
        int64_t s = w - origin;
        int c;
        float add = (w >= t0 && w < t1) ? tone : 0.0f;
        for (c = 0; c < 2; c++) if (g_atape[w * 2 + c] != idv(pass > 0 ? s % pass : s, c) + add) bad++;
    }
    return bad;
}
static int64_t tape_silent(int64_t w0, int64_t w1) {
    int64_t w, bad = 0;
    for (w = w0; w < w1 && w < ATAPE; w++) bad += g_atape[w * 2] != 0.0f || g_atape[w * 2 + 1] != 0.0f;
    return bad;
}

/* ysp/audio.h fits the device's slope once its points span 5 s; before
 * that a plan is off by the drift times the time since the fit started. */
#define SND_WARM 420

static const yrt_event* last_sound(int what) {
    int i;
    const yrt_event* r = NULL;
    for (i = 0; i < g_noth; i++) if (g_oth[i].kind == YVID_EV_SOUND && (int)g_oth[i].u.u32[8] == what) r = &g_oth[i];
    return r;
}

static int64_t snd_origin(void) {
    yau_stream_info si;
    yau_stream_get_info(&((yvid__snd*)g_mv.snd)->w.stream, &si);
    return si.started ? si.origin : INT64_MIN;
}

/* Opens audio and a movie of `frames` at 30 fps with the identity soundtrack. */
static int snd_setup(disp* d, double drift_ppm, int64_t frames, bool loop) {
    yvid_desc vd;
    yvid_soundtrack_desc sdd;
    rec_reset();
    disp_init(d, 60.0, 0);
    g_vt = d->T0 - 3000000000LL;
    if (!au_open(drift_ppm, g_vt + 1000000)) { fail(__LINE__, "audio open"); return 0; }
    sd_default(&g_sd, 30, 1, frames, 10);
    desc_default(&vd);
    vd.loop = loop;
    if (!open_mv(&vd)) { yau_close(&g_au); return 0; }
    make_wav(48000, 2, frames * 1600);
    wav_desc(&sdd);
    CHECK_I(yvid_soundtrack(&g_mv, &g_au, &sdd), YVID_OK);
    return 1;
}

static void snd_teardown(disp* d) {
    yscr_frame f;
    int k;
    yvid_pause_at(&g_mv, YVID_ASAP);
    for (k = 0; k < 30; k++) snd_frame(d, &f);   /* the stop reaches the device */
    yvid_close(&g_mv);
    yau_close(&g_au);
}

/* Start, every sample on its frame, the movie clock on the audio's, a tone
 * mixed beside it, no allocation per frame. */
static void test_snd_lock(double drift_ppm) {
    disp d;
    yscr_frame f;
    int64_t k, origin, t_start, first_shown = -1, worst = 0, heap0 = 0, tone_f0 = -1, n_mt = 0;
    yau_id tone_id = 0;
    static float tone[480 * 2];
    const yrt_event* ev;
    g_case = drift_ppm > 0 ? "soundtrack lock +" : "soundtrack lock -";
    if (!snd_setup(&d, drift_ppm, 300, false)) return;
    for (k = 0; k < SND_WARM; k++) snd_frame(&d, &f);   /* the fit has its slope */
    CHECK(yvid_play_at(&g_mv, YVID_ASAP) > 0);
    for (k = 0; k < 480 * 2; k++) tone[k] = 0.25f;
    for (k = 0; k < 560; k++) {
        CHECK(snd_frame(&d, &f) >= 0);
        if (k == 120) heap0 = (int64_t)yvid_heap_calls();
        if (k == 200) {
            /* feedback during the movie: a voice on the same device */
            yau_buf b;
            memset(&b, 0, sizeof b);
            b.frames = tone; b.n = 480; b.channels = 2;
            tone_id = yau_play_at(&g_au, b, f.onset + 100000000);
        }
        if (first_shown < 0 && g_mv.has_last && g_mv.last.decision == YVID_SHOWN) first_shown = g_mv.last.predicted;
    }
    CHECK_I((int64_t)yvid_heap_calls() - heap0, 0);
    drain();
    ev = last_sound(1);
    CHECK(ev != NULL);
    origin = snd_origin();
    CHECK(origin != INT64_MIN);
    if (!ev || origin == INT64_MIN) { snd_teardown(&d); return; }
    t_start = ev->u.i64[2];
    CHECK_I(ev->u.i64[1], 0);   /* sample 0 is movie time 0 */
    /* the sound's first sample nearest the shared target, which is a
     * display onset: the first frame shows on it (a target between onsets
     * puts every frame up to half a period off the sound) */
    CHECK(llabs(origin - ad_frame_of(&g_ad, t_start)) <= 1);
    /* the follow reads the soundtrack's own sample, not the device's stream
     * through an anchor (that path writes a CLOCK record) */
    CHECK_I(count_other((uint16_t)YVID_EV_CLOCK), 0);
    CHECK(first_shown >= 0 && llabs(first_shown - t_start) <= 1000);
    {
        yau_onset o;
        memset(&o, 0, sizeof o);
        CHECK(tone_id > 0 && yau_result(&g_au, tone_id, &o) == YAU_OK);
        tone_f0 = o.start_frame;
        CHECK(llabs(tone_f0 - ad_frame_of(&g_ad, o.target)) <= 1);
    }
    CHECK_I(tape_check(origin, origin + 480000 < g_ad.W ? origin + 480000 : g_ad.W, origin, 0, tone_f0, tone_f0 + 480, 0.25f), 0);
    CHECK_I(tape_silent(0, origin), 0);
    /* the movie time at each onset is the audio's: the sample on that
     * frame, to the fit's error and half a sample */
    for (k = 0; k < MAXD && k < SND_WARM + 600; k++) {
        const frec* r = &g_fr[k];
        int64_t e;
        if (!r->have || !(r->flags & YVID_F_AUDIO_CLOCK)) continue;
        e = llabs(r->mt - truth_mt(origin, r->t));
        if (e > worst) worst = e;
        n_mt++;
    }
    CHECK(n_mt > 400);
    CHECK(worst < 30000);
    printf("video_test: soundtrack at %+.0f ppm: start on the target's frame %+lld, %lld frames on the audio clock, worst movie time against the audio %.1f us\n",
           drift_ppm, (long long)(origin - ad_frame_of(&g_ad, t_start)), (long long)n_mt, worst / 1e3);
    snd_teardown(&d);
}

/* Pause, resume, seek: the stop on its frame, silence, the resume at the
 * sample nearest the resumed movie time, the seek's at the target's. */
static void test_snd_controls(void) {
    disp d;
    yscr_frame f;
    int64_t k, o1, o2, o3, stop_f, w_resume, s0;
    yrt_event pr;
    int64_t pause_id;
    const yrt_event* ev;
    g_case = "soundtrack controls";
    /* A failed yvid_result() leaves pr unwritten; the checks still read it. */
    memset(&pr, 0, sizeof pr);
    if (!snd_setup(&d, 37.0, 300, false)) return;
    for (k = 0; k < SND_WARM; k++) snd_frame(&d, &f);
    yvid_play_at(&g_mv, YVID_ASAP);
    for (k = 0; k < 120; k++) snd_frame(&d, &f);
    o1 = snd_origin();
    pause_id = yvid_pause_at(&g_mv, YVID_ASAP);
    for (k = 0; k < 40; k++) snd_frame(&d, &f);
    drain();
    ev = last_sound(2);
    CHECK(ev != NULL);
    CHECK_I(yvid_result(&g_mv, pause_id, &pr), YVID_OK);
    stop_f = ev ? ad_frame_of(&g_ad, ev->u.i64[2]) : 0;
    w_resume = g_ad.W;
    CHECK_I(tape_check(o1, stop_f - 1, o1, 0, 0, 0, 0.0f), 0);
    CHECK_I(tape_silent(stop_f + 1, w_resume), 0);
    /* the video paused on the frame nearest the same time */
    CHECK(llabs((int64_t)pr.t_ns - (ev ? ev->u.i64[2] : 0)) <= d.P_report / 2 + 1);
    yvid_play_at(&g_mv, YVID_ASAP);
    for (k = 0; k < 120; k++) snd_frame(&d, &f);
    drain();
    ev = last_sound(1);
    o2 = snd_origin();
    s0 = ev ? ev->u.i64[1] : -1;
    /* resumed one display period after the frozen movie time */
    CHECK_I(s0, yvid__snd_sample(&g_mv, pr.u.i64[0] + d.P_report, 0));
    CHECK(ev && llabs(o2 + s0 - ad_frame_of(&g_ad, ev->u.i64[2])) <= 1);
    CHECK_I(tape_silent(stop_f + 1, o2 + s0), 0);
    CHECK_I(tape_check(o2 + s0, g_ad.W, o2, 0, 0, 0, 0.0f), 0);
    /* a seek forward, ASAP */
    w_resume = g_ad.W;
    yvid_seek_frame(&g_mv, 200, YVID_ASAP);
    for (k = 0; k < 120; k++) snd_frame(&d, &f);
    drain();
    ev = last_sound(1);
    o3 = snd_origin();
    CHECK(ev && ev->u.i64[1] == 200 * 1600);
    CHECK(ev && llabs(o3 + 200 * 1600 - ad_frame_of(&g_ad, ev->u.i64[2])) <= 1);
    CHECK_I(tape_check(o3 + 200 * 1600, g_ad.W, o3, 0, 0, 0, 0.0f), 0);
    /* nothing from before the seek after its stop */
    {
        const yrt_event* st = last_sound(2);
        int64_t sf = st ? ad_frame_of(&g_ad, st->u.i64[2] > 0 ? st->u.i64[2] : 0) : w_resume;
        (void)sf;
        CHECK_I(tape_silent(w_resume + 2 * 1440, o3 + 200 * 1600), 0);
    }
    snd_teardown(&d);
}

/* A loop: the source's pass again and again, gapless, with the video's. */
static void test_snd_loop(void) {
    disp d;
    yscr_frame f;
    int64_t k, origin;
    g_case = "soundtrack loop";
    if (!snd_setup(&d, 37.0, 150, true)) return;   /* 5 s: 240000 samples */
    for (k = 0; k < SND_WARM; k++) snd_frame(&d, &f);
    yvid_play_at(&g_mv, YVID_ASAP);
    for (k = 0; k < 1000; k++) CHECK(snd_frame(&d, &f) >= 0);   /* 16.7 s: three cycles */
    drain();
    origin = snd_origin();
    CHECK(origin != INT64_MIN);
    CHECK(g_ad.W - origin > 3 * 240000);
    CHECK_I(tape_check(origin, g_ad.W, origin, 240000, 0, 0, 0.0f), 0);
    CHECK(count_other((uint16_t)YVID_EV_LOOP) >= 3);
    snd_teardown(&d);
}

/* A timed start inside the audio's lead: ysp_audio starts it late and skips
 * the samples it missed, so every later sample is still on its frame. */
static void test_snd_late(void) {
    disp d;
    yscr_frame f;
    int64_t k, origin, t;
    g_case = "soundtrack late";
    if (!snd_setup(&d, 37.0, 300, false)) return;
    for (k = 0; k < SND_WARM; k++) snd_frame(&d, &f);
    t = g_vt + 5000000;   /* 5 ms: inside the lead */
    yvid_play_at(&g_mv, t);
    for (k = 0; k < 200; k++) snd_frame(&d, &f);
    origin = snd_origin();
    CHECK(origin != INT64_MIN);
    CHECK(llabs(origin - ad_frame_of(&g_ad, t)) <= 1);   /* origin from the plan, not the late frame */
    {
        int64_t w0 = origin;
        while (w0 < g_ad.W && g_atape[w0 * 2] == 0.0f) w0++;
        CHECK(w0 > origin);   /* it did start late */
        CHECK_I(tape_check(w0, g_ad.W, origin, 0, 0, 0, 0.0f), 0);
    }
    snd_teardown(&d);
}

static void test_snd_refusals(void) {
    disp d;
    yvid_desc vd;
    yvid_soundtrack_desc sdd;
    static const struct { int32_t num, den; int64_t frames, samples; bool loop; int ok; const char* why; } t[] = {
        { 30, 1, 150, 240000, false, 1, NULL },
        { 30, 1, 150, 239999, false, 0, "240000 are needed" },
        { 30, 1, 150, 240001, false, 0, "240000 are needed" },
        { 30000, 1001, 151, 241842, false, 1, NULL },              /* 241841.6 rounds to 241842 */
        { 30000, 1001, 151, 241841, false, 0, "241842 are needed" },
        { 30000, 1001, 151, 241843, false, 0, "241842 are needed" },
        { 256, 1, 1, 188, false, 1, NULL },                        /* 187.5: a tie, up */
        { 256, 1, 1, 187, false, 0, "188 are needed" },
        { 30000, 1001, 151, 241842, true, 0, "multiple of 5" },     /* not a whole number of samples */
        { 30000, 1001, 150, 240240, true, 1, NULL },
    };
    size_t i;
    g_case = "soundtrack refusals";
    disp_init(&d, 60.0, 0);
    g_vt = d.T0 - 3000000000LL;
    if (!au_open(0.0, g_vt + 1000000)) { fail(__LINE__, "audio open"); return; }
    for (i = 0; i < sizeof t / sizeof t[0]; i++) {
        int rc;
        sd_default(&g_sd, t[i].num, t[i].den, t[i].frames, 1);
        desc_default(&vd);
        vd.loop = t[i].loop;
        if (!open_mv(&vd)) continue;
        make_wav(48000, 2, t[i].samples);
        wav_desc(&sdd);
        rc = yvid_soundtrack(&g_mv, &g_au, &sdd);
        if (t[i].ok) { if (rc != YVID_OK) { fprintf(stderr, "  case %d: %s\n", (int)i, yvid_error(&g_mv)); fail(__LINE__, "refused a right length"); } }
        else if (rc != YVID_ERR_REFUSED || !strstr(yvid_error(&g_mv), t[i].why)) {
            fprintf(stderr, "  case %d: %d '%s', wanted '%s'\n", (int)i, rc, yvid_error(&g_mv), t[i].why);
            fail(__LINE__, "refusal");
        }
        yvid_close(&g_mv);
    }
    /* rate and channels */
    sd_default(&g_sd, 30, 1, 150, 1);
    desc_default(&vd);
    if (open_mv(&vd)) {
        /* rate and channels: ysp_audio's refusals, passed through */
        make_wav(44100, 2, 220500);
        wav_desc(&sdd);
        CHECK_I(yvid_soundtrack(&g_mv, &g_au, &sdd), YVID_ERR_REFUSED);
        CHECK(strstr(yvid_error(&g_mv), "44100") != NULL);
        make_wav(48000, 3, 240000);
        wav_desc(&sdd);
        CHECK_I(yvid_soundtrack(&g_mv, &g_au, &sdd), YVID_ERR_REFUSED);
        CHECK(strstr(yvid_error(&g_mv), "3") != NULL);
        make_wav(48000, 2, 240000);
        wav_desc(&sdd);
        yvid_play_at(&g_mv, YVID_ASAP);
        CHECK_I(yvid_soundtrack(&g_mv, &g_au, &sdd), YVID_ERR_ORDER);   /* after a play */
        yvid_close(&g_mv);
    }
    yau_close(&g_au);
}

/* Without the follow, the movie base alone must agree with the sound: both
 * start at the shared target, so at 0 ppm the base's time at each onset is
 * the sample playing then, to half a sample. The display misses the
 * target's vblank, so the first frame lands a period after the sound
 * started: the base still starts at the target. And the sample of a movie
 * time is the nearest one. */
static void test_snd_base(void) {
    disp d;
    yscr_frame f;
    yvid_movie m;
    int64_t k, origin, worst = 0, n = 0;
    int missed = 0;
    g_case = "soundtrack base, no follow";
    memset(&m, 0, sizeof m);
    m.snd_rate = 48000; m.snd_frames = 1000;
    CHECK_I(yvid__snd_sample(&m, 10416, 0), 0);     /* 0.49997 samples */
    CHECK_I(yvid__snd_sample(&m, 10417, 0), 1);     /* 0.50002 */
    CHECK_I(yvid__snd_sample(&m, 20833, 0), 1);     /* 0.99998 */
    CHECK_I(yvid__snd_sample(&m, 1000000000, 2), 2000 + 48000);
    if (!snd_setup(&d, 0.0, 300, false)) return;
    for (k = 0; k < SND_WARM; k++) snd_frame(&d, &f);
    yvid_play_at(&g_mv, YVID_ASAP);
    for (k = 0; k < 300; k++) {
        disp_next(&d, &f);
        ad_until(&g_ad, g_vt);
        yau_update(&g_au);
        CHECK(yvid_update(&g_mv, &f) >= 0);
        if (!missed && g_mv.snd_phase == 2 && llabs(f.onset + d.P_report - g_mv.snd_t) < 1000) {
            d.late_next = 1;   /* this flip takes the target's vblank */
            missed = 1;
        }
    }
    CHECK_I(missed, 1);
    drain();
    origin = snd_origin();
    CHECK(origin != INT64_MIN);
    for (k = 0; k < MAXD && k < SND_WARM + 300; k++) {
        const frec* r = &g_fr[k];
        int64_t e;
        if (!r->have || r->dec == 0 || r->mt <= 0) continue;
        e = llabs(r->mt - truth_mt(origin, r->t));
        if (e > worst) worst = e;
        n++;
    }
    CHECK(n > 200);
    CHECK(worst < 30000);
    snd_teardown(&d);
}

static void test_soundtrack(void) {
    test_snd_base();
    test_snd_lock(37.0);
    test_snd_lock(-400.0);
    test_snd_controls();
    test_snd_loop();
    test_snd_late();
    test_snd_refusals();
}

/* ------------------------------------------------------------ base rates
 * ysp/timeline.h's base rates (v0.4.0): the movie base at num/den. The due
 * frame is the base time the frame's window reaches (ytl_window), so an
 * annotation at t(i) on the movie base fires on the display frame that
 * first shows frame i at any rate, as at rate 1. */

/* As run_agree, with the base at rn/rd from the start, or changed to rn/rd
 * at display frame `change_at` (rate 1 before it). */
static void run_agree_rate(const char* name, int32_t num, int32_t den, double hz, double noise,
                           int32_t rn, int32_t rd, int64_t change_at) {
    yvid_desc vd;
    disp d;
    yscr_frame f;
    int64_t k, n = 2400, frames = 3000, mism = 0, fired_n = 0, twice = 0, drift_near = 0, k_change = -1;
    static int64_t fired_at[4000];
    static int64_t first_show[4000];
    g_case = name;
    rec_reset();
    tl_open();
    for (k = 0; k < frames; k++) {
        ytl_event e;
        memset(&e, 0, sizeof e);
        e.base = 3; e.kind = YTL_MARK; e.time = yvid_frame_time(num, den, k); e.code = (int32_t)k;
        ytl_add(&g_tl, &e);
        fired_at[k] = -1;
        first_show[k] = -1;
    }
    if (change_at < 0) CHECK_I(ytl_rate(&g_tl, 3, 0, rn, rd), 0);   /* a stopped base keeps it */
    sd_default(&g_sd, num, den, frames, 1);
    desc_default(&vd);
    vd.refresh_num = (int32_t)hz;
    vd.timeline = &g_tl;
    vd.base = 3;
    if (!open_mv(&vd)) return;
    disp_init(&d, hz, 0);
    d.noise_ns = noise;
    CHECK(yvid_play_at(&g_mv, YVID_ASAP) > 0);
    for (k = 0; k < n; k++) {
        ytl_event fired[16];
        ytl_frame tf;
        int nf, i;
        disp_next(&d, &f);
        if (k == change_at) { CHECK_I(ytl_rate(&g_tl, 3, f.onset, rn, rd), 0); k_change = f.index; }
        yvid_update(&g_mv, &f);
        if (g_mv.has_last && g_mv.last.display == f.index && g_mv.last.decision == YVID_SHOWN && g_mv.last.frame < 4000)
            first_show[g_mv.last.frame] = f.index;
        tf.onset = f.onset; tf.period = f.period; tf.index = f.index;
        nf = ytl_evaluate(&g_tl, &tf, fired, 16);
        for (i = 0; i < nf && i < 16; i++) {
            int64_t c = fired[i].code;
            if (fired_at[c] >= 0) twice++;
            fired_at[c] = f.index;
            fired_n++;
        }
        if ((k & 255) == 0) drain();
    }
    settle(&d);
    for (k = 0; k < frames; k++) {
        int64_t want = first_show[k];
        if (fired_at[k] < 0 && want < 0) continue;
        if (want < 0) {
            int64_t j;
            for (j = k + 1; j < frames && first_show[j] < 0; j++) {}
            want = j < frames ? first_show[j] : -1;
        }
        if (fired_at[k] != want) mism++;
    }
    /* a change of rate is not a slip */
    if (k_change >= 0) for (k = k_change; k < k_change + 3 && k < MAXD; k++) drift_near += g_fr[k].have && g_fr[k].why == YVID_WHY_DRIFT;
    CHECK(fired_n > 100);
    CHECK_I(mism, 0);
    CHECK_I(twice, 0);
    CHECK_I(drift_near, 0);
    CHECK(count_other((uint16_t)YVID_EV_RATE) >= 1);
    yvid_close(&g_mv);
}

static void test_rates(void) {
    run_agree_rate("rate 1/2: 30 on 60, 20 us noise", 30, 1, 60.0, 20000, 1, 2, -1);
    run_agree_rate("rate 1001/1000: 23.976 on 60", 24000, 1001, 60.0, 20000, 1001, 1000, -1);
    run_agree_rate("rate 2/1: 30 on 60", 30, 1, 60.0, 20000, 2, 1, -1);
    run_agree_rate("rate 1 to 1/2 at frame 500", 30, 1, 60.0, 20000, 1, 2, 500);
    /* the cadence at the rate: 15 fps of movie time on 60 Hz is a multiple */
    {
        yvid_desc vd;
        disp d;
        yscr_frame f;
        yvid_info info;
        const yrt_event* ev;
        int64_t k, pause_id;
        yrt_event pr;
        g_case = "rate 1/2: cadence, pause, record due";
        memset(&pr, 0, sizeof pr);   /* read even when yvid_result() fails */
        rec_reset();
        tl_open();
        sd_default(&g_sd, 30, 1, 1000, 1);
        desc_default(&vd);
        vd.timeline = &g_tl; vd.base = 2;
        if (open_mv(&vd)) {
            disp_init(&d, 60.0, 0);
            CHECK_I(ytl_rate(&g_tl, 2, 0, 1, 2), 0);
            yvid_play_at(&g_mv, YVID_ASAP);
            for (k = 0; k < 200; k++) { disp_next(&d, &f); yvid_update(&g_mv, &f); }
            yvid_get_info(&g_mv, &info);
            CHECK_I(info.multiple, 4);
            drain();
            ev = find_other((uint16_t)YVID_EV_RATE, 0);
            CHECK(ev && ev->u.i32[0] == 1 && ev->u.i32[1] == 2 && ev->u.i32[2] == 4);
            /* the record's due is the RT time the base reached the frame,
             * repeats included; no slip on an exact grid */
            {
                int64_t drift = 0, checked = 0;
                for (k = 20; k < 190; k++) {
                    const frec* r = &g_fr[k];
                    int64_t rt = 0;
                    if (!r->have || r->dec == 0) continue;
                    drift += r->why == YVID_WHY_DRIFT;
                    if (k < 150) continue;
                    CHECK(ytl_rt_time(&g_tl, 2, yvid_frame_time(30, 1, r->frame), &rt));
                    CHECK_I(r->due, rt);
                    checked++;
                }
                CHECK_I(drift, 0);
                CHECK(checked > 30);
            }
            /* a resume starts one display period of RT later: half a period
             * of movie time */
            pause_id = yvid_pause_at(&g_mv, YVID_ASAP);
            for (k = 0; k < 5; k++) { disp_next(&d, &f); yvid_update(&g_mv, &f); }
            CHECK_I(yvid_result(&g_mv, pause_id, &pr), YVID_OK);
            {
                int64_t pid = yvid_play_at(&g_mv, YVID_ASAP);
                yrt_event pl;
                memset(&pl, 0, sizeof pl);
                for (k = 0; k < 20; k++) { disp_next(&d, &f); yvid_update(&g_mv, &f); }
                CHECK_I(yvid_result(&g_mv, pid, &pl), YVID_OK);
                CHECK_I(pl.u.i64[0], pr.u.i64[0] + d.P_report / 2);
            }
            yvid_close(&g_mv);
        }
    }
    /* 23.976 at 1/2 judders on 60 Hz: CADENCE, never DRIFT, 10 ppb off the
     * exact grid (where ties would be) */
    {
        yvid_desc vd;
        disp d;
        yscr_frame f;
        int64_t k, drift = 0, cad = 0;
        g_case = "rate 1/2: 23.976 judders";
        rec_reset();
        tl_open();
        sd_default(&g_sd, 24000, 1001, 1000, 1);
        desc_default(&vd);
        vd.timeline = &g_tl; vd.base = 1;
        if (open_mv(&vd)) {
            disp_init(&d, 60.0, 0.01);
            CHECK_I(ytl_rate(&g_tl, 1, 0, 1, 2), 0);
            yvid_play_at(&g_mv, YVID_ASAP);
            for (k = 0; k < 900; k++) { disp_next(&d, &f); yvid_update(&g_mv, &f); if ((k & 255) == 0) drain(); }
            settle(&d);
            for (k = 20; k < 880; k++) { drift += g_fr[k].have && g_fr[k].why == YVID_WHY_DRIFT; cad += g_fr[k].have && g_fr[k].why == YVID_WHY_CADENCE; }
            CHECK_I(drift, 0);
            (void)cad;   /* 5.005 display frames per frame: a sixth showing each 200 frames, too few here */
            {
                yvid_info info;
                yvid_get_info(&g_mv, &info);
                CHECK_I(info.multiple, 0);
                CHECK(fabs(info.per_frame - 5.005) < 1e-6);
            }
            yvid_close(&g_mv);
        }
    }
    /* a loop at 1/2: the wrap at the RT time the base reached t(N); the
     * annotation at frame 0 fires once per cycle */
    {
        yvid_desc vd;
        disp d;
        yscr_frame f;
        ytl_event e;
        int64_t k;
        int fired0 = 0;
        g_case = "rate 1/2: loop";
        rec_reset();
        tl_open();
        memset(&e, 0, sizeof e);
        e.base = 1; e.kind = YTL_MARK; e.time = 0; e.code = 7;
        ytl_add(&g_tl, &e);
        sd_default(&g_sd, 24000, 1001, 10, 1);
        desc_default(&vd);
        vd.loop = true; vd.timeline = &g_tl; vd.base = 1;
        if (open_mv(&vd)) {
            CHECK_I(ytl_rate(&g_tl, 1, 0, 1, 2), 0);
            yvid_play_at(&g_mv, YVID_ASAP);
            disp_init(&d, 60.0, 0.01);   /* 10 ppb off the exact grid */
            for (k = 0; k < 1020; k++) {   /* 17 s: 10 frames of 23.976 at 1/2 last 0.83 s, 20 cycles */
                ytl_event fired[8];
                ytl_frame tf;
                int nf, i;
                disp_next(&d, &f);
                yvid_update(&g_mv, &f);
                tf.onset = f.onset; tf.period = f.period; tf.index = f.index;
                nf = ytl_evaluate(&g_tl, &tf, fired, 8);
                for (i = 0; i < nf; i++) fired0 += fired[i].code == 7;
            }
            drain();
            CHECK(fired0 >= 19 && fired0 <= 21);
            CHECK(count_other((uint16_t)YVID_EV_LOOP) >= 18);
            /* each cycle lasts twice the movie's duration of RT: the wraps
             * stay on that grid, with no error that adds up */
            {
                const yrt_event* l0 = find_other((uint16_t)YVID_EV_LOOP, 0);
                int c;
                for (c = 1; l0 && c < 18; c++) {
                    const yrt_event* lc = find_other((uint16_t)YVID_EV_LOOP, c);
                    int64_t want = (int64_t)l0->t_ns + (int64_t)c * 2 * yvid_frame_time(24000, 1001, 10);
                    if (lc && llabs((int64_t)lc->t_ns - want) > d.P_report + 1) fail_i(__LINE__, "loop wrap off the rate's grid", (long long)lc->t_ns, (long long)want);
                }
            }
            /* at the wrap the base keeps the overshoot: the movie time steps
             * by half a period, less the duration */
            {
                int c, n_ok = 0;
                for (c = 0; c < 18; c++) {
                    const yrt_event* lc = find_other((uint16_t)YVID_EV_LOOP, c);
                    int64_t k2;
                    if (!lc) break;
                    for (k2 = 1; k2 < MAXD; k2++) if (g_fr[k2].have && (int64_t)g_fr[k2].t == (int64_t)lc->t_ns) break;
                    if (k2 >= MAXD || !g_fr[k2 - 1].have) continue;
                    if (llabs(g_fr[k2].mt - (g_fr[k2 - 1].mt + d.P_report / 2 - yvid_frame_time(24000, 1001, 10))) > 2)
                        fail_i(__LINE__, "movie time across the wrap", (long long)g_fr[k2].mt, (long long)(g_fr[k2 - 1].mt + d.P_report / 2 - yvid_frame_time(24000, 1001, 10)));
                    else n_ok++;
                }
                CHECK(n_ok >= 15);
            }
            yvid_close(&g_mv);
        }
    }
    /* strict_cadence: a rate that judders holds the frame and refuses */
    {
        yvid_desc vd;
        disp d;
        yscr_frame f;
        int64_t k, held;
        int rc = YVID_OK;
        g_case = "rate: strict_cadence";
        rec_reset();
        tl_open();
        sd_default(&g_sd, 30, 1, 1000, 1);
        desc_default(&vd);
        vd.timeline = &g_tl; vd.base = 1; vd.strict_cadence = true;
        if (open_mv(&vd)) {
            disp_init(&d, 60.0, 0);
            yvid_play_at(&g_mv, YVID_ASAP);
            for (k = 0; k < 60; k++) { disp_next(&d, &f); CHECK_I(yvid_update(&g_mv, &f), YVID_OK); }
            CHECK_I(ytl_rate(&g_tl, 1, f.onset + d.P_report, 3, 4), 0);   /* 22.5 fps on 60 Hz */
            held = g_mv.shown_g;
            for (k = 0; k < 30; k++) { disp_next(&d, &f); rc = yvid_update(&g_mv, &f); }
            CHECK_I(rc, YVID_ERR_REFUSED);
            CHECK(g_mv.shown_g <= held + 1);
            CHECK(strstr(yvid_error(&g_mv), "strict_cadence") != NULL);
            drain();
            {
                const yrt_event* ev = find_other((uint16_t)YVID_EV_RATE, 0);
                CHECK(ev && ev->u.u32[8] == 1);
            }
            CHECK_I(ytl_rate(&g_tl, 1, f.onset + d.P_report, 1, 2), 0);   /* back to a multiple */
            for (k = 0; k < 30; k++) { disp_next(&d, &f); rc = yvid_update(&g_mv, &f); }
            CHECK_I(rc, YVID_OK);
            yvid_close(&g_mv);
        }
    }
    /* a soundtrack refuses a rate other than 1 */
    {
        yvid_desc vd;
        yvid_soundtrack_desc sdd;
        disp d;
        g_case = "rate: soundtrack";
        disp_init(&d, 60.0, 0);
        g_vt = d.T0 - 3000000000LL;
        rec_reset();
        tl_open();
        if (au_open(0.0, g_vt + 1000000)) {
            sd_default(&g_sd, 30, 1, 150, 1);
            desc_default(&vd);
            vd.timeline = &g_tl; vd.base = 1;
            if (open_mv(&vd)) {
                CHECK_I(ytl_rate(&g_tl, 1, 0, 1, 2), 0);
                make_wav(48000, 2, 240000);
                wav_desc(&sdd);
                CHECK_I(yvid_soundtrack(&g_mv, &g_au, &sdd), YVID_ERR_REFUSED);
                CHECK(strstr(yvid_error(&g_mv), "rate 1 only") != NULL);
                yvid_close(&g_mv);
            }
            yau_close(&g_au);
        }
    }
}

/* ------------------------------------------------- Media Foundation (Windows)
 * Real decoding of the clips tests/media/make_video_clips.sh makes, in the
 * directory YVID_TEST_MEDIA names. Each frame carries its index in 16 bars
 * across the top rows, so the test reads which frame the upload holds.
 * Skipped, with a message, without the clips or without Media Foundation. */
#if YVID__MF

static char g_media[512];

static const char* media(const char* name) {
    static char p[4][1024];
    static int k;
    k = (k + 1) & 3;
    snprintf(p[k], sizeof p[k], "%s/%s", g_media, name);
    return p[k];
}

static void index_desc_bt709(yvid_index_desc* d) {
    memset(d, 0, sizeof *d);
    d->matrix = YVID_MATRIX_BT709; d->range = YVID_RANGE_LIMITED; d->transfer = YVID_TRC_BT1886;
    d->primaries = YVID_PRIM_BT709; d->siting = YVID_SITING_LEFT;
}

/* The frame number in the last RGBA8 upload: bit k is bar k, read at row 32
 * (luma 235 is white, 16 black). */
static int up_bars(void) {
    int k, v = 0;
    if (!g_up_y || g_up_w < 16) return -1;
    for (k = 0; k < 16; k++) {
        int x = (k * g_mv.info.w) / 16 + g_mv.info.w / 32;
        if (g_up_row32[x] > 128) v |= 1 << k;
    }
    return v;
}

static void mf_desc(yvid_desc* vd, const char* clip) {
    memset(vd, 0, sizeof *vd);
    vd->path = media(clip);
    vd->inline_decode = true;
    vd->ring = &g_ring;
    vd->refresh_num = 60; vd->refresh_den = 1;
    vd->ahead = 4;
}

/* The XXH64 of each of the first 3 frames of clip.nv12 (ffmpeg's decoder,
 * tight NV12) must be the index's: the same byte stream that
 * yvid__hash_planes() hashes. */
static void mf_ref_check(const char* clip, int w, int h) {
    char p[300], ip[300];
    yvid__index ix;
    yvid_desc vd;
    char err[300];
    size_t fb = (size_t)w * (size_t)h + (size_t)((w + 1) / 2) * 2 * (size_t)((h + 1) / 2);
    uint8_t* b = (uint8_t*)malloc(fb);
    FILE* fh;
    int k;
    snprintf(p, sizeof p, "%s.nv12", clip);
    snprintf(ip, sizeof ip, "%s.mp4", clip);
    fh = fopen(media(p), "rb");
    if (!fh || !b) { printf("video_test: no %s; the ffmpeg reference check skipped\n", media(p)); if (fh) fclose(fh); free(b); return; }
    memset(&vd, 0, sizeof vd);
    vd.path = media(ip);
    if (yvid__index_load(&vd, &ix, 1, err, sizeof err) < 0) { fail(__LINE__, err); fclose(fh); free(b); return; }
    for (k = 0; k < 3; k++) {
        if (fread(b, 1, fb, fh) != fb) { fail(__LINE__, "short reference"); break; }
        if (yvid_xxh64(b, fb, 0) != ix.hashes[k]) { fprintf(stderr, "  %s frame %d\n", clip, k); fail(__LINE__, "Media Foundation's bytes differ from ffmpeg's"); }
    }
    fclose(fh);
    free(b);
    yvid__free(ix.hashes);
}

static void test_mf_index(void) {
    static const struct { const char* clip; int64_t frames; const char* why; } t[] = {
        { "c_720p30.mp4", 300, NULL }, { "c_1080p30.mp4", 300, NULL }, { "c_1080p60.mp4", 600, NULL },
        { "c_2997.mp4", 150, NULL }, { "c_23976.mp4", 96, NULL }, { "c_crop.mp4", 60, NULL },
        { "c_audio.mp4", 90, NULL },
        { "r_bframes.mp4", 0, "B-frames" }, { "r_vfr.mp4", 0, "no constant rate" }, { "r_gop.mp4", 0, "GOP length changes" },
        { "r_10bit.mp4", 0, "High 10" }, { "r_444.mp4", 0, "4:4:4" }, { "r_sar.mp4", 0, "square pixels" },
        { "r_interlace.mp4", 0, "interlaced" }, { "r_mpeg4.mp4", 0, "not H.264 or HEVC" },
        { "r_rotation.mp4", 0, "rotated" }, { "r_ts1000.mp4", 0, "no constant rate" }, { "r_offset.mp4", 0, "edit list" },
    };
    yvid_index_desc d;
    char err[600];
    size_t i;
    int64_t n;
    g_case = "mf index";
    for (i = 0; i < sizeof t / sizeof t[0]; i++) {
        index_desc_bt709(&d);
        err[0] = 0;
        n = yvid_index_make(media(t[i].clip), NULL, &d, err, sizeof err);
        if (t[i].why == NULL) {
            if (n != t[i].frames) { fprintf(stderr, "  %s: %s\n", t[i].clip, err); fail_i(__LINE__, "index frames", n, t[i].frames); }
        } else if (n >= 0 || !strstr(err, t[i].why)) {
            fprintf(stderr, "  %s: %lld, '%s', wanted '%s'\n", t[i].clip, (long long)n, err, t[i].why);
            fail(__LINE__, "refusal");
        }
    }
    /* the index's first hashes against ffmpeg's decoder, frame for frame */
    {
        static const struct { const char* clip; int w, h; } r[] = {
            { "c_720p30", 1280, 720 }, { "c_1080p30", 1920, 1080 }, { "c_crop", 1280, 718 } };
        for (i = 0; i < sizeof r / sizeof r[0]; i++) mf_ref_check(r[i].clip, r[i].w, r[i].h);
    }
    /* HEVC needs a decoder that Windows may not have */
    index_desc_bt709(&d);
    n = yvid_index_make(media("c_hevc.mp4"), NULL, &d, err, sizeof err);
    if (n < 0) {
        if (strstr(err, "HEVC")) printf("video_test: HEVC skipped: %s\n", err);
        else { fprintf(stderr, "  c_hevc: %s\n", err); fail(__LINE__, "HEVC index"); }
    } else CHECK_I(n, 120);
    /* the color comes from the stream's SPS (Media Foundation reports none):
     * an empty desc takes it, a desc that contradicts it is refused */
    {
        yvid__index ix;
        yvid_desc vq;
        memset(&d, 0, sizeof d);
        n = yvid_index_make(media("c_crop.mp4"), media("c_crop.vui.yspvi"), &d, err, sizeof err);
        if (n != 60) { fprintf(stderr, "  %s\n", err); fail(__LINE__, "index from the SPS's color"); }
        memset(&vq, 0, sizeof vq);
        vq.path = media("c_crop.mp4");
        {
            static uint8_t ib[4096];
            FILE* fh = fopen(media("c_crop.vui.yspvi"), "rb");
            size_t got = fh ? fread(ib, 1, sizeof ib, fh) : 0;
            if (fh) fclose(fh);
            vq.index = ib; vq.index_size = got;
            if (yvid__index_load(&vq, &ix, 0, err, sizeof err) == YVID_OK) {
                CHECK_I(ix.c.matrix, YVID_MATRIX_BT709); CHECK_I(ix.c.range, YVID_RANGE_LIMITED);
                CHECK_I(ix.c.transfer, YVID_TRC_BT1886); CHECK_I(ix.c.primaries, YVID_PRIM_BT709);
                CHECK_I(ix.c.siting, YVID_SITING_LEFT);
            } else fail(__LINE__, err);
        }
        remove(media("c_crop.vui.yspvi"));
        memset(&d, 0, sizeof d);
        d.matrix = YVID_MATRIX_BT601;
        n = yvid_index_make(media("c_crop.mp4"), media("c_crop.vui.yspvi"), &d, err, sizeof err);
        CHECK(n < 0 && strstr(err, "the stream states matrix"));
        remove(media("c_crop.vui.yspvi"));
        /* three different fields: each in its place */
        memset(&d, 0, sizeof d);
        n = yvid_index_make(media("c_mixcolor.mp4"), media("c_mixcolor.vui.yspvi"), &d, err, sizeof err);
        if (n != 30) { fprintf(stderr, "  %s\n", err); fail(__LINE__, "c_mixcolor index"); }
        else {
            static uint8_t ib2[4096];
            FILE* fh = fopen(media("c_mixcolor.vui.yspvi"), "rb");
            size_t got = fh ? fread(ib2, 1, sizeof ib2, fh) : 0;
            if (fh) fclose(fh);
            vq.path = media("c_mixcolor.mp4");
            vq.index = ib2; vq.index_size = got;
            if (yvid__index_load(&vq, &ix, 0, err, sizeof err) == YVID_OK) {
                CHECK_I(ix.c.matrix, YVID_MATRIX_BT601); CHECK_I(ix.c.transfer, YVID_TRC_SRGB);
                CHECK_I(ix.c.primaries, YVID_PRIM_BT709);
            } else fail(__LINE__, err);
        }
        remove(media("c_mixcolor.vui.yspvi"));
        /* HEVC's VUI is not read: its color must be stated */
        memset(&d, 0, sizeof d);
        n = yvid_index_make(media("c_hevc.mp4"), media("c_hevc.vui.yspvi"), &d, err, sizeof err);
        CHECK(n < 0 && (strstr(err, "does not state its matrix") || strstr(err, "HEVC")));
        remove(media("c_hevc.vui.yspvi"));
    }
}

/* Plays a clip from frame 0 to the end on 60 Hz and checks every shown
 * frame's bar code against its index; returns the shown count. */
static int32_t g_mf_w, g_mf_h;
static int64_t mf_play_all(const yvid_desc* vd, int64_t frames, int check_heap) {
    disp d;
    yscr_frame f;
    yvid_info info;
    int64_t k, shown = 0, bad = 0, heap0 = 0;
    int rc = YVID_OK;
    rec_reset();
    g_up_bytes = 0;
    if (!yvid_open(&g_mv, &g_gfx, vd)) { fprintf(stderr, "  open: %s\n", yvid_error(&g_mv)); fail(__LINE__, "open"); return 0; }
    disp_init(&d, 60.0, 0.01);
    CHECK(yvid_play_at(&g_mv, YVID_ASAP) > 0);
    for (k = 0; k < frames * 3 + 60 && rc == YVID_OK; k++) {
        disp_next(&d, &f);
        if (k == 120) heap0 = (int64_t)yvid_heap_calls();
        rc = yvid_update(&g_mv, &f);
        if (g_mv.has_last && g_mv.last.display == f.index && g_mv.last.decision == YVID_SHOWN) {
            shown++;
            if (up_bars() != (int)(g_mv.last.frame & 0xffff)) bad++;
        }
    }
    CHECK_I(rc, YVID_ENDED);
    CHECK_I(bad, 0);
    if (check_heap) CHECK_I((int64_t)yvid_heap_calls() - heap0, 0);
    yvid_get_info(&g_mv, &info);
    CHECK_I(info.ts_mismatch, 0);
    CHECK_I(info.hash_mismatch, 0);
    g_mf_w = info.w; g_mf_h = info.h;
    yvid_close(&g_mv);
    return shown;
}

static void test_mf_play(void) {
    yvid_desc vd;
    yvid_index_desc id;
    char err[600], line[1024];
    int hw;
    g_case = "mf play";
    mf_desc(&vd, "c_720p30.mp4");
    CHECK_I(mf_play_all(&vd, 300, 1), 300);
    /* 1080p: coded 1088 rows, the chroma plane after all of them */
    mf_desc(&vd, "c_1080p30.mp4");
    CHECK_I(mf_play_all(&vd, 300, 1), 300);
    CHECK_I(g_mf_w, 1920); CHECK_I(g_mf_h, 1080);   /* the display aperture, not the coded 1088 */
    mf_desc(&vd, "c_crop.mp4");
    CHECK_I(mf_play_all(&vd, 60, 0), 60);
    CHECK_I(g_mf_w, 1280); CHECK_I(g_mf_h, 718);
    /* 24000/1001: the rate is the index's, fitted from the sample times */
    mf_desc(&vd, "c_23976.mp4");
    CHECK(mf_play_all(&vd, 96, 0) == 96);
    /* every decoder gives the software decoder's bytes (the index's) */
    g_case = "mf decoders";
    for (hw = YVID_HW_OFF; hw <= YVID_HW_DXVA; hw++) {
        yvid_info info;
        mf_desc(&vd, "c_1080p60.mp4");
        vd.hw_decode = (yvid_hw)hw;
        vd.refresh_num = 120;
        CHECK_I(mf_play_all(&vd, 600, 0), 600);
        if (yvid_open(&g_mv, &g_gfx, &vd)) {
            yvid_get_info(&g_mv, &info);
            yvid_describe(&g_mv, line, sizeof line);
            printf("video_test: hw %d: %.140s\n", hw, line);
            yvid_close(&g_mv);
        }
    }
    /* an index made by DXVA serves the software decoder */
    index_desc_bt709(&id);
    id.hw = YVID_HW_DXVA;
    CHECK_I(yvid_index_make(media("c_720p30.mp4"), media("c_720p30.dxva.yspvi"), &id, err, sizeof err), 300);
    mf_desc(&vd, "c_720p30.mp4");
    {
        FILE* fh = fopen(media("c_720p30.dxva.yspvi"), "rb");
        static uint8_t ib[8192];
        size_t n = fh ? fread(ib, 1, sizeof ib, fh) : 0;
        if (fh) fclose(fh);
        vd.index = ib; vd.index_size = n;
        vd.hw_decode = YVID_HW_OFF;
        CHECK_I(mf_play_all(&vd, 300, 0), 300);
    }
    remove(media("c_720p30.dxva.yspvi"));
}

static void test_mf_seek(void) {
    static const int64_t targets[] = { 133, 7, 30, 299, 59, 61, 0, 250 };
    yvid_desc vd;
    disp d;
    yscr_frame f;
    size_t i;
    int64_t k;
    g_case = "mf seek";
    mf_desc(&vd, "c_1080p30.mp4");
    rec_reset();
    if (!yvid_open(&g_mv, &g_gfx, &vd)) { fail(__LINE__, yvid_error(&g_mv)); return; }
    disp_init(&d, 60.0, 0.01);
    yvid_play_at(&g_mv, YVID_ASAP);
    for (k = 0; k < 20; k++) { disp_next(&d, &f); yvid_update(&g_mv, &f); }
    for (i = 0; i < sizeof targets / sizeof targets[0]; i++) {
        int landed = 0;
        CHECK(yvid_seek_frame(&g_mv, targets[i], YVID_STAY_PAUSED) > 0);
        for (k = 0; k < 40 && !landed; k++) {
            disp_next(&d, &f);
            CHECK(yvid_update(&g_mv, &f) >= 0);
            landed = g_mv.state == YVID__PAUSED && g_mv.shown_g == targets[i];
        }
        CHECK(landed);
        if (up_bars() != (int)targets[i]) fail_i(__LINE__, "the seek's frame", up_bars(), targets[i]);
    }
    {
        yvid_info info;
        yvid_get_info(&g_mv, &info);
        CHECK_I(info.hash_mismatch, 0);
        CHECK_I(info.ts_mismatch, 0);
    }
    yvid_close(&g_mv);
}

static uint8_t* g_mf_file;
static int64_t g_mf_size;
static int64_t mf_rd(void* ctx, int64_t off, void* buf, int64_t n) {
    (void)ctx;
    if (off < 0 || off >= g_mf_size) return -1;
    if (n > g_mf_size - off) n = g_mf_size - off;
    if (n > 65536) n = 65536;   /* short reads, as a range request gives */
    memcpy(buf, g_mf_file + off, (size_t)n);
    return n;
}

static uint8_t* slurp(const char* path, int64_t* n) {
    FILE* fh = fopen(path, "rb");
    uint8_t* b;
    long sz;
    *n = 0;
    if (!fh) return NULL;
    fseek(fh, 0, SEEK_END); sz = ftell(fh); fseek(fh, 0, SEEK_SET);
    b = (uint8_t*)malloc((size_t)sz);
    if (b && fread(b, 1, (size_t)sz, fh) != (size_t)sz) { free(b); b = NULL; }
    fclose(fh);
    *n = b ? sz : 0;
    return b;
}

static void test_mf_sources(void) {
    yvid_desc vd;
    yvid_reader rd;
    int64_t isz = 0;
    uint8_t* ix;
    char line[1024];
    g_case = "mf sources";
    g_mf_file = slurp(media("c_720p30.mp4"), &g_mf_size);
    ix = slurp(media("c_720p30.mp4.yspvi"), &isz);
    if (!g_mf_file || !ix) { fail(__LINE__, "read the clip"); free(g_mf_file); free(ix); return; }
    mf_desc(&vd, "c_720p30.mp4");
    vd.path = NULL; vd.data = g_mf_file; vd.size = (size_t)g_mf_size; vd.index = ix; vd.index_size = (size_t)isz;
    CHECK_I(mf_play_all(&vd, 300, 0), 300);
    mf_desc(&vd, "c_720p30.mp4");
    rd.read = mf_rd; rd.size = g_mf_size;
    vd.path = NULL; vd.reader = &rd; vd.index = ix; vd.index_size = (size_t)isz;
    CHECK_I(mf_play_all(&vd, 300, 0), 300);
    /* one byte changed inside the media data: the size and the ends still
     * match the index, the frame hashes do not */
    g_mf_file[g_mf_size / 2] ^= 0x55;
    mf_desc(&vd, "c_720p30.mp4");
    vd.path = NULL; vd.data = g_mf_file; vd.size = (size_t)g_mf_size; vd.index = ix; vd.index_size = (size_t)isz;
    if (yvid_open(&g_mv, &g_gfx, &vd)) {
        disp d;
        yscr_frame f;
        int k, rc = YVID_OK;
        yvid_info info;
        disp_init(&d, 60.0, 0.01);
        yvid_play_at(&g_mv, YVID_ASAP);
        for (k = 0; k < 700 && rc == YVID_OK; k++) { disp_next(&d, &f); rc = yvid_update(&g_mv, &f); }
        yvid_get_info(&g_mv, &info);
        yvid_describe(&g_mv, line, sizeof line);
        CHECK(info.hash_mismatch > 0);
        CHECK(strstr(line, "NOT CANONICAL: hash mismatches") != NULL);
        yvid_close(&g_mv);
    } else fail(__LINE__, yvid_error(&g_mv));
    /* a stale index: another clip's */
    {
        int64_t isz2 = 0;
        uint8_t* ix2 = slurp(media("c_crop.mp4.yspvi"), &isz2);
        mf_desc(&vd, "c_720p30.mp4");
        vd.index = ix2; vd.index_size = (size_t)isz2;
        CHECK(!yvid_open(&g_mv, &g_gfx, &vd));
        CHECK(strstr(yvid_error(&g_mv), "index") != NULL);
        free(ix2);
    }
    free(g_mf_file); g_mf_file = NULL;
    free(ix);
}

/* The decode thread for real, with the frame thread in an STA (as SDL3
 * makes it) and Media Foundation in the MTA. */
static void test_mf_threaded(void) {
    yvid_desc vd;
    yscr_frame f;
    int64_t k, t0, P = 16666667, last = -1, order_bad = 0, shown = 0, bars_bad = 0;
    int rc = YVID_OK;
    HRESULT co = E_FAIL;
    /* looked up, so the test links nothing beyond what the header does */
    typedef HRESULT (WINAPI *co_init_fn)(void*, DWORD);
    typedef void (WINAPI *co_uninit_fn)(void);
    HMODULE ole = LoadLibraryW(L"ole32.dll");
    co_init_fn co_init = ole ? (co_init_fn)(void (*)(void))GetProcAddress(ole, "CoInitializeEx") : NULL;
    co_uninit_fn co_uninit = ole ? (co_uninit_fn)(void (*)(void))GetProcAddress(ole, "CoUninitialize") : NULL;
    g_case = "mf threaded";
    g_virtual = 0;
    if (co_init) co = co_init(NULL, COINIT_APARTMENTTHREADED);
    mf_desc(&vd, "c_720p30.mp4");
    vd.inline_decode = false;
    rec_reset();
    g_up_bytes = 0;
    if (!yvid_open(&g_mv, &g_gfx, &vd)) { fail(__LINE__, yvid_error(&g_mv)); g_virtual = 1; return; }
    CHECK(yvid_play_at(&g_mv, YVID_ASAP) > 0);
    t0 = yrt_now_ns() + 20000000;
    for (k = 0; k < 400 && rc == YVID_OK; k++) {
        memset(&f, 0, sizeof f);
        f.onset = t0 + k * P; f.period = P; f.index = k; f.vblank = k;
        yrt_sleep_until((int64_t)(f.onset - P / 2), 0);
        g_planes_off = 0; g_plane_i = 0;
        rc = yvid_update(&g_mv, &f);
        if (g_mv.has_last && g_mv.last.display == k && g_mv.last.decision == YVID_SHOWN) {
            if (g_mv.last.frame <= last) order_bad++;
            if (up_bars() != (int)g_mv.last.frame) bars_bad++;
            last = g_mv.last.frame;
            shown++;
        }
        if (k == 200) yvid_seek_frame(&g_mv, 50, YVID_ASAP), last = -1;
    }
    CHECK(rc >= 0);
    CHECK_I(order_bad, 0);
    CHECK_I(bars_bad, 0);
    if (shown < 150) fail_i(__LINE__, "frames shown", shown, 150);
    yvid_close(&g_mv);
    if (SUCCEEDED(co) && co_uninit) co_uninit();
    g_virtual = 1;
}

static void test_mf(void) {
    const char* dir = getenv("YVID_TEST_MEDIA");
    char probe[600];
    FILE* fh;
    if (!dir || !dir[0]) { printf("video_test: Media Foundation cases skipped: set YVID_TEST_MEDIA to the clips of tests/media/make_video_clips.sh\n"); return; }
    snprintf(g_media, sizeof g_media, "%s", dir);
    snprintf(probe, sizeof probe, "%s/c_720p30.mp4", dir);
    fh = fopen(probe, "rb");
    if (!fh) { printf("video_test: Media Foundation cases skipped: no %s\n", probe); return; }
    fclose(fh);
    {
        char err[256];
        if (yvid__mf_load(err, sizeof err) < 0) { printf("video_test: Media Foundation cases skipped: %s\n", err); return; }
        yvid__mf_unload();
    }
    test_mf_index();
    test_mf_play();
    test_mf_seek();
    test_mf_sources();
    test_mf_threaded();
}
#else
static void test_mf(void) { printf("video_test: Media Foundation cases skipped: not Windows, or YVID_NO_MF\n"); }
#endif

int main(void) {
    gfx_open();
    test_pure();
    test_qoi();
    test_yuv();
    test_multiple_and_noise();
    test_cadences();
    test_drift();
    test_display_late();
    test_decode_stall();
    test_seek();
    test_loop_end();
    test_manual();
    test_pause();
    test_timeline_agreement();
    test_follow();
    test_records();
    test_open_refusals();
    test_decoder_faults();
    test_replay_and_heap();
    test_seq();
    test_reader();
    test_threaded();
    test_threaded_stall();
    test_soundtrack();
    test_rates();
    test_mf();
    ygfx_close(&g_gfx);
    yscr_close(&g_scr);
    if (g_failures) { fprintf(stderr, "video_test: %d failure(s)\n", g_failures); return 1; }
    printf("video_test: all checks passed\n");
    return 0;
}
