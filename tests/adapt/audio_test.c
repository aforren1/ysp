/* audio_test.c - self-checking test for ysp/audio.h's core: the plan of
 * a start frame from the fit, sample-exact placement, the device-clock fit
 * under drift and one-sided callback jitter, confirmation and tiers,
 * underruns, cancel, stop, gain, loops, routing, clipping, the ring
 * records, the queues, the strict open, synthesis and the output formats.
 * No framework: it returns 0 when every check passed and 1 after printing
 * each failure.
 *
 * It needs no miniaudio and no sound hardware: YAU_NO_MINIAUDIO builds
 * the core alone, and a scripted device (the Audio device extension) plays
 * a device whose clock runs at a chosen drift against the ysp_rt clock,
 * with seeded late-only callback jitter, a pipeline of L frames between a
 * rendered frame and the output, position reports stamped at the call (as
 * WASAPI's were measured), and injected stalls. Its output goes on a tape
 * indexed by stream frame, so the test checks where every sample landed.
 * The frame-thread side runs on a virtual clock through YAU__NOW. One
 * case runs the device on a real thread against the frame thread, for
 * ThreadSanitizer.
 *
 *     gcc -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Werror -O2 -Iinclude \
 *         -o audio_test tests/adapt/audio_test.c -lm -pthread && ./audio_test
 *     cl /nologo /W4 /WX /Iinclude tests\adapt\audio_test.c
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS   /* fopen for the tape and a WAV file */
#endif
#ifndef YAU_NO_MINIAUDIO
#define YAU_NO_MINIAUDIO
#endif
static int g_virtual = 1;
static long long g_vt = 1000000000000LL;
static long long vnow(void);
static void vsleep_until(long long t);
#define YAU__NOW() ((int64_t)vnow())
#define YAU__SLEEP_UNTIL(t) vsleep_until((long long)(t))

#define YSP_AUDIO_IMPLEMENTATION
#include "ysp/audio.h"

static long long vnow(void) { return g_virtual ? g_vt : (long long)yrt_now_ns(); }
static void vsleep_until(long long t) {
    if (!g_virtual) { yrt_sleep_until(t, 0); return; }
    if (t > g_vt) g_vt = t;
}

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#if defined(_WIN32)
#include <process.h>
#else
#include <pthread.h>
#endif

/* ---------------------------------------------------------------- harness */

static int g_failures = 0;
static void fail(int line, const char* what) {
    fprintf(stderr, "audio_test: FAIL at line %d: %s\n", line, what);
    g_failures++;
}
static void fail_i(int line, const char* what, long long got, long long want) {
    fprintf(stderr, "audio_test: FAIL at line %d: %s (got %lld, want %lld)\n", line, what, got, want);
    g_failures++;
}
#define CHECK(c) do { if (!(c)) fail(__LINE__, #c); } while (0)
#define CHECK_I(got, want) do { long long g_ = (long long)(got), w_ = (long long)(want); \
    if (g_ != w_) fail_i(__LINE__, #got " == " #want, g_, w_); } while (0)

/* ---------------------------------------------------------- scripted device */

#define TAPE_FRAMES (48000 * 20)

struct sdev;
typedef struct sdev {
    /* what the device is */
    uint32_t rate;
    uint16_t channels, out;
    int32_t  period, buffer, pos_source, tier;
    uint8_t  map[YAU_MAX_CHANNELS];
    double   drift_ppm;     /* the device clock runs fast by this much         */
    int64_t  L;             /* frames from render to output                    */
    double   jitter_ns;     /* mean callback lateness (exponential), 0 = none  */
    double   report_noise_ns;  /* mean lateness of a report's position         */
    int64_t  pos_jump;      /* frames added to the reports only               */
    int      stall_halts;   /* a stall halts the position (else it counts)    */
    int64_t  halt_pipe;     /* with stall_halts: the engine reads this far ahead
                               of the position, so it runs dry and the position
                               halts this short of the frames written, for whole
                               periods (0: at the frames written)             */
    int64_t  stale_ns;      /* the next stale_n reports read a position this
                               much older than their stamp                    */
    int      stale_n;
    int64_t  halt_ns;       /* the last halt, for the checks                  */
    int64_t  stood_ahead;   /* with halt_pipe: the report that reads the
                               position standing is this many frames ahead
                               (measured at queue 2: about 9 ms early)       */
    int64_t  last_t;        /* with halt_pipe: callbacks after a halt come in
                               order, not before the late one                */
    uint64_t rng;
    int      fail_open;
    /* the run */
    int64_t  T0;            /* ysp_rt time of stream frame -L's output         */
    int64_t  g;             /* silent frames the device inserted (stalls)      */
    int64_t  k;             /* blocks rendered                                 */
    int64_t  W;
    int64_t  stall_ns;      /* the next block is this late                     */
    void*    host;
    yau_render_fn render;
    float*   tape;          /* channels floats per stream frame, or NULL       */
    void   (*on_block)(struct sdev* s, const float* out, int32_t n);  /* f32 only */
    unsigned char out_mem[8192 * 8 * 4];
    int      started, stopped, closed;
    int32_t  queue_asked;   /* yau_device_open.queue at the last open          */
} sdev;

static sdev g_sd;

static double sd_kd(const sdev* s) { return 1e9 / (double)s->rate / (1.0 + s->drift_ppm * 1e-6); }

/* The true output time of stream frame w. */
static int64_t sd_truth(const sdev* s, int64_t w) {
    return s->T0 + (int64_t)floor(((double)(w + s->L + s->g)) * sd_kd(s) + 0.5);
}
/* The frame on which t falls by the header's rule, from the truth. */
static int64_t sd_frame_of(const sdev* s, int64_t t) {
    double x = (double)(t - s->T0) / sd_kd(s) - (double)(s->L + s->g);
    return (int64_t)ceil(x - 0.5);
}

/* The device's request time for stream frame w (the callback-time line). */
static int64_t sd_event(const sdev* s, int64_t w) {
    return s->T0 + (int64_t)floor(((double)(w + s->g)) * sd_kd(s) + 0.5);
}

static double sd_rand(sdev* s) {
    s->rng = s->rng * 6364136223846793005ull + 1442695040888963407ull;
    return ((double)(s->rng >> 11) + 0.5) / 9007199254740992.0;
}

static int sd_open(void* ctx, const yau_device_open* in, yau_device_caps* caps, char* err, size_t cap) {
    sdev* s = (sdev*)ctx;
    (void)in;
    if (s->fail_open) { snprintf(err, cap, "scripted: refused"); return YAU_ERR_LOST; }
    memset(caps, 0, sizeof *caps);
    caps->rate = s->rate;
    caps->channels = s->channels;
    caps->out = s->out;
    memcpy(caps->map, s->map, sizeof caps->map);
    caps->period = s->period;
    caps->buffer = s->buffer;
    caps->pos_source = s->pos_source;
    caps->tier = s->tier;
    snprintf(caps->name, sizeof caps->name, "scripted");
#if YAU_VERSION_MINOR >= 3
    s->queue_asked = in->queue;
    caps->queue = in->queue;   /* a device that keeps the queue it was asked for */
#endif
    return 0;
}
static int sd_start(void* ctx, yau_render_fn r, void* host) {
    sdev* s = (sdev*)ctx;
    s->render = r; s->host = host; s->started = 1;
    return 0;
}
static void sd_stop(void* ctx) { ((sdev*)ctx)->stopped = 1; }
static void sd_close(void* ctx) { ((sdev*)ctx)->closed = 1; }
static const yau_device g_dev = { YAU_DEVICE_VERSION, "scripted", sd_open, sd_start, sd_stop, sd_close, NULL };

static size_t sd_bytes(const sdev* s) {
    return s->out == YAU_OUT_S16 ? 2u : s->out == YAU_OUT_S24 ? 3u : 4u;
}

/* Render one block: the device's event for block k, the callback late by
 * the jitter, a report of the engine's position at the latest pass, stamped
 * at the call. */
static void sd_block(sdev* s) {
    double kd = sd_kd(s);
    int64_t e = s->T0 + (int64_t)floor(((double)(s->k * s->period + s->L + s->g) - (double)s->L) * kd + 0.5);
    int64_t late = s->jitter_ns > 0 ? (int64_t)(-s->jitter_ns * log(sd_rand(s))) : 0;
    int64_t t = e + late + s->stall_ns, pass;
    int halted = 0;
    yau_tick tk;
    int64_t i;
    if (s->halt_pipe > 0) {
        if (t < s->last_t) t = s->last_t;
        s->last_t = t;
    }
    if (t > g_vt) g_vt = t;
    /* the report: the position a little before its stamp (an engine pass
     * the call reads), late by an exponential with a small mean */
    pass = t - (s->report_noise_ns > 0 ? (int64_t)(-s->report_noise_ns * log(sd_rand(s))) : 0);
    if (s->stall_ns && s->stall_halts && s->halt_pipe > 0) {
        /* WASAPI shared, measured: the engine needed frame W when the
         * position was halt_pipe short of it; it ran dry, the position
         * stood there, and the next passes came whole periods later. The
         * report of this block may read the position standing. */
        double need = (double)s->T0 + (double)(s->W - s->halt_pipe + s->L) * kd;
        if ((double)t > need) {
            double per = (double)s->period * kd;
            int64_t h = (int64_t)floor(ceil(((double)t - need) / per) * per + 0.5);
            s->T0 += h;
            s->halt_ns = h;
            s->stall_ns = 0;
            memset(&tk, 0, sizeof tk);
            tk.t_entry = t;
            tk.pos = s->W - s->halt_pipe + s->stood_ahead;
            tk.pos_t = t;
            goto rendered;
        }
        s->stall_ns = 0;
    }
    if (s->stall_ns) {
        /* The device ran dry and played silence until this block came; its
         * position counted the silence (the case the header must handle). */
        int64_t gap = (int64_t)floor((double)(pass - s->T0) / kd + 1e-3) - s->L - (s->k * s->period + s->g);
        if (gap > 0) {
            if (s->stall_halts) {
                /* WASAPI's way (measured): the position halts at the frames
                 * written, and later frames play later by the time it stood */
                s->T0 += (int64_t)floor((double)gap * kd + 0.5);
                pass = t;
                halted = 1;
            } else {
                s->g += gap;
            }
        }
        s->stall_ns = 0;
    }
    memset(&tk, 0, sizeof tk);
    tk.t_entry = t;
    if (s->pos_source == YAU_POS_DEVICE) {
        /* frames played, a count; its stamp is that frame's true time plus
         * the report's own lateness (pass = t - lateness), so the least late
         * report of a bucket is close to the truth */
        int64_t f = (int64_t)floor((double)(pass - s->T0) / kd + 1e-3);
        tk.pos = f - s->L + s->pos_jump;
        if (tk.pos < 0) tk.pos = 0;
        tk.pos_t = s->T0 + (int64_t)floor((double)f * kd + 0.5) + (t - pass);
        if (halted) { tk.pos = s->W + s->g; tk.pos_t = t; }   /* stood at the frames written */
        if (s->stale_n > 0) {
            int64_t fs = (int64_t)floor((double)(pass - s->stale_ns - s->T0) / kd + 1e-3);
            tk.pos = fs - s->L;
            s->stale_n--;
        }
    } else {
        tk.pos = -1;
    }
rendered:
    s->render(s->host, s->out_mem, s->period, &tk);
    if (s->on_block) s->on_block(s, (const float*)(const void*)s->out_mem, s->period);
    if (s->tape && s->W + s->period <= TAPE_FRAMES) {
        for (i = 0; i < (int64_t)s->period * s->channels; i++) {
            float v;
            if (s->out == YAU_OUT_F32) v = ((float*)(void*)s->out_mem)[i];
            else if (s->out == YAU_OUT_S16) v = (float)((int16_t*)(void*)s->out_mem)[i] / 32768.0f;
            else if (s->out == YAU_OUT_S32) v = (float)((double)((int32_t*)(void*)s->out_mem)[i] / 2147483648.0);
            else {
                const unsigned char* b = s->out_mem + i * 3;
                int32_t x = (int32_t)((uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16));
                if (x & 0x800000) x -= 0x1000000;
                v = (float)x / 8388608.0f;
            }
            s->tape[s->W * s->channels + i] = v;
        }
    }
    s->W += s->period;
    s->k++;
    (void)sd_bytes;
}

static void sd_run(sdev* s, int64_t blocks) { int64_t i; for (i = 0; i < blocks; i++) sd_block(s); }
static void sd_run_s(sdev* s, double sec) { sd_run(s, (int64_t)(sec * s->rate / s->period + 0.5)); }

static float* g_tape;
static yau_audio g_au;
static unsigned char g_ring_mem[YRT_RING_BYTES(16384)];
static yrt_ring g_ring;
static float g_arena[1 << 20];

static void sd_default(sdev* s) {
    memset(s, 0, sizeof *s);
    s->rate = 48000; s->channels = 2; s->out = YAU_OUT_F32;
    s->period = 480; s->buffer = 1440; s->pos_source = YAU_POS_DEVICE; s->tier = YAU_TIER_2;
    s->map[0] = YAU_CH_FL; s->map[1] = YAU_CH_FR;
    s->drift_ppm = 37; s->L = 1900; s->jitter_ns = 300000; s->rng = 12345;
    s->report_noise_ns = 20000;
    s->T0 = g_vt + 5000000;
    s->tape = g_tape;
    if (g_tape) memset(g_tape, 0, sizeof(float) * TAPE_FRAMES * 2);
}

static bool open_dev(sdev* s, yau_desc* d) {
    yau_desc dd;
    if (d) dd = *d; else memset(&dd, 0, sizeof dd);
    dd.backend = YAU_BACKEND_CUSTOM;
    dd.dev = &g_dev;
    dd.dev_ctx = s;
    if (!dd.ring) { yrt_ring_open(&g_ring, &(yrt_ring_desc){ .memory = g_ring_mem, .bytes = sizeof g_ring_mem }); dd.ring = &g_ring; }
    if (!dd.arena) { dd.arena = g_arena; dd.arena_bytes = sizeof g_arena; }
    return yau_open(&g_au, &dd);
}

static float g_buf[48000 * 2];

/* ------------------------------------------------------------------- cases */

/* Plans: every start frame is the frame the truth puts the target on, and
 * the samples are on the tape exactly there. */
static void test_plan_and_placement(void) {
    sdev* s = &g_sd;
    int i, exact = 0, within1 = 0, n = 200;
    int64_t worst = 0;
    sd_default(s);
    CHECK(open_dev(s, NULL));
    sd_run_s(s, 12.0);   /* past the slope span */
    for (i = 0; i < 48000; i++) g_buf[i] = (float)((i % 997) + 1) / 2000.0f;   /* never 0 */
    for (i = 0; i < n; i++) {
        yau_buf b;
        yau_onset r = { 0 };
        int64_t t, want, got;
        int rc;
        memset(&b, 0, sizeof b);
        b.frames = g_buf; b.n = 100; b.channels = 1; b.id = 77;
        /* targets between frames, at every phase, 60 to 160 ms ahead */
        t = g_vt + 60000000 + (int64_t)(sd_rand(s) * 100000000.0);
        CHECK(yau_play_at(&g_au, b, t) > 0);
        sd_run_s(s, 0.25);
        rc = yau_result(&g_au, (yau_id)(g_au.next_id), &r);
        CHECK_I(rc, YAU_OK);
        want = sd_frame_of(s, t);
        got = r.start_frame;
        if (got == want) exact++;
        else {
            /* a miss is allowed only for a target within 2 us of a tie */
            double x = (double)(t - s->T0) / sd_kd(s) - (double)(s->L + s->g);
            double tie = fabs((x - floor(x)) - 0.5) * sd_kd(s);
            if (tie < 2000.0) exact++;
            else fail_i(__LINE__, "start frame off the truth, target far from a tie", got, want);
        }
        if (llabs(got - want) <= 1) within1++;
        if (llabs(sd_truth(s, got) - r.onset) > worst) worst = llabs(sd_truth(s, got) - r.onset);
        CHECK(r.tier == YAU_TIER_2);
        CHECK(!(r.flags & (YAU_ONSET_PENDING | YAU_ONSET_LATE | YAU_ONSET_UNCONFIRMED)));
        CHECK_I(r.buffer_id, 77);
        CHECK(r.end_frame == got + 100);
        /* the tape: zero before, the buffer from `got` on */
        if (got > 0 && got + 100 < TAPE_FRAMES) {
            CHECK(g_tape[(got - 1) * 2] == 0.0f);
            CHECK(g_tape[got * 2] == g_buf[0]);
            CHECK(g_tape[got * 2 + 1] == g_buf[0]);
            CHECK(g_tape[(got + 99) * 2] == g_buf[99]);
            CHECK(g_tape[(got + 100) * 2] == 0.0f);
        }
    }
    printf("plan: %d of %d start frames exact, %d within one; onset vs truth worst %.1f us\n",
           exact, n, within1, (double)worst / 1e3);
    CHECK_I(exact, n);
    CHECK_I(within1, n);
    CHECK(worst < 20833 / 2);
    {
        yau_caps c;
        yau_get_caps(&g_au, &c);
        printf("plan: drift %.3f ppm (scripted 37), spread %.0f ns\n", c.drift_ppm, c.fit_spread_ns);
        CHECK(fabs(c.drift_ppm - 37.0) < 1.0);
        CHECK(c.lead_ns > 0);
        CHECK(yau_frame_at(&g_au, yau_time_of(&g_au, 123456)) == 123456);
    }
    yau_close(&g_au);
}

/* The fit under drift. Callback times (late, never early): the envelope
 * recovers the rate and the offset with 1 ms of one-sided jitter, and least
 * squares on the same points misses the offset by their mean lateness,
 * which is why the header uses the envelope there. Position reports, late
 * by 20 us on average: least squares on the bucket minima recovers both. */
static void test_fit_drift(void) {
    sdev* s = &g_sd;
    double drifts[3] = { 100.0, -100.0, 0.0 };
    int j;
    for (j = 0; j < 3; j++) {
        yau_caps c;
        int64_t probe, err_env;
        double sxx = 0, sxy = 0, mx = 0, my = 0, b, a;
        int32_t i, n;
        sd_default(s);
        s->tape = NULL;
        s->drift_ppm = drifts[j];
        s->jitter_ns = 1000000;
        s->pos_source = YAU_POS_CALLBACK;
        s->tier = 0;
        CHECK(open_dev(s, NULL));
        sd_run_s(s, 60.0);
        yau_update(&g_au);
        yau_get_caps(&g_au, &c);
        probe = g_au.w + 4800;
        /* callback times map a block's first frame to its device event */
        err_env = yau_time_of(&g_au, probe) - sd_event(s, probe);
        /* least squares on the same points */
        n = g_au.pt_n;
        for (i = 0; i < n; i++) {
            int32_t q = (g_au.pt_head + i) % YAU__FIT_PTS;
            mx += (double)(g_au.pt_w[q] - g_au.pt_w[g_au.pt_head]);
            my += (double)(g_au.pt_t[q] - g_au.pt_t[g_au.pt_head]);
        }
        mx /= n; my /= n;
        for (i = 0; i < n; i++) {
            int32_t q = (g_au.pt_head + i) % YAU__FIT_PTS;
            double x = (double)(g_au.pt_w[q] - g_au.pt_w[g_au.pt_head]) - mx;
            double y = (double)(g_au.pt_t[q] - g_au.pt_t[g_au.pt_head]) - my;
            sxx += x * x; sxy += x * y;
        }
        b = sxy / sxx;
        a = my - b * mx;   /* least squares, at x = mx */
        {
            double x = (double)(probe - g_au.pt_w[g_au.pt_head]);
            int64_t t_ols = g_au.pt_t[g_au.pt_head] + (int64_t)(a + b * x);
            int64_t err_ols = t_ols - sd_event(s, probe);
            printf("fit: drift %+.0f ppm -> %+.3f ppm; offset error envelope %+.1f us, least squares %+.1f us\n",
                   drifts[j], c.drift_ppm, (double)err_env / 1e3, (double)err_ols / 1e3);
            CHECK(fabs(c.drift_ppm - drifts[j]) < 0.5);
            CHECK(llabs(err_env) < 20000);
            CHECK(err_ols > 2 * llabs(err_env));
        }
        yau_close(&g_au);
        /* position reports */
        sd_default(s);
        s->tape = NULL;
        s->drift_ppm = drifts[j];
        CHECK(open_dev(s, NULL));
        sd_run_s(s, 60.0);
        yau_update(&g_au);
        yau_get_caps(&g_au, &c);
        probe = g_au.w + 4800;
        err_env = yau_time_of(&g_au, probe) - sd_truth(s, probe);
        printf("fit: reports, drift %+.0f ppm -> %+.3f ppm; offset error %+.1f us\n",
               drifts[j], c.drift_ppm, (double)err_env / 1e3);
        CHECK(fabs(c.drift_ppm - drifts[j]) < 0.5);
        CHECK(llabs(err_env) < 5000);
        yau_close(&g_au);
    }
}

/* LATE, cancel, stop with a ramp, gain, loops, routing, clipping. */
static void test_controls(void) {
    sdev* s = &g_sd;
    yau_buf b;
    yau_play_desc pd;
    yau_onset r = { 0 };
    yau_id id, id2;
    int64_t f, i;
    sd_default(s);
    s->jitter_ns = 0;
    CHECK(open_dev(s, NULL));
    sd_run_s(s, 8.0);   /* past the slope span */
    for (i = 0; i < 48000 * 2; i++) g_buf[i] = 0.5f;
    memset(&b, 0, sizeof b);
    b.frames = g_buf; b.n = 4800; b.channels = 1;

    /* a target in the past: LATE, started on the first reachable frame */
    id = yau_play_at(&g_au, b, g_vt - 50000000);
    sd_run(s, 1);
    sd_run_s(s, 0.2);
    CHECK_I(yau_result(&g_au, id, &r), YAU_OK);
    CHECK(r.flags & YAU_ONSET_LATE);
    CHECK(r.residual > 40000000);
    sd_run_s(s, 0.2);

    /* cancel before the start: nothing plays */
    id = yau_play_at(&g_au, b, g_vt + 500000000);
    CHECK(yau_cancel(&g_au, id) == 0);
    sd_run_s(s, 1.0);
    CHECK_I(yau_result(&g_au, id, &r), YAU_OK);
    CHECK(r.flags & YAU_ONSET_CANCELED);
    CHECK_I(r.start_frame, -1);
    f = sd_frame_of(s, r.target);
    CHECK(g_tape[f * 2] == 0.0f);

    /* stop at t with a 5 ms ramp: the fade ends on t's frame */
    id = yau_play_at(&g_au, b, g_vt + 100000000);
    {
        int64_t stop_t = g_vt + 150000000, sf;
        CHECK(yau_stop_at(&g_au, id, stop_t, YRT_MS(5)) == 0);
        sd_run_s(s, 0.4);
        CHECK_I(yau_result(&g_au, id, &r), YAU_OK);
        CHECK(r.flags & YAU_ONSET_STOPPED);
        sf = sd_frame_of(s, stop_t);
        CHECK(llabs(r.end_frame - sf) <= 1);
        CHECK(g_tape[(sf - 240) * 2] < 0.5f + 1e-6f && g_tape[(sf - 241) * 2] > 0.4999f);
        CHECK(fabs(g_tape[(sf - 120) * 2] - 0.25f) < 0.01f);   /* half way down the cosine */
        CHECK(fabs(g_tape[(sf - 180) * 2] - 0.4268f) < 0.01f);  /* a quarter: 0.5 (1 + cos(pi / 4)) / 2 */
        CHECK(g_tape[(sf + 1) * 2] == 0.0f);
    }

    /* gain: -6.02 dB from t, no ramp */
    id = yau_play_at(&g_au, b, g_vt + 100000000);
    {
        int64_t gt = g_vt + 130000000, gf;
        CHECK(yau_gain_at(&g_au, id, gt, -6.0206f, 0) == 0);
        sd_run_s(s, 0.3);
        gf = sd_frame_of(s, gt);
        if (fabs(g_tape[(gf - 2) * 2] - 0.5f) >= 1e-6f)
            printf("gain: %g %g %g %g at gf-2..gf+1\n", g_tape[(gf - 2) * 2], g_tape[(gf - 1) * 2],
                   g_tape[gf * 2], g_tape[(gf + 1) * 2]);
        CHECK(fabs(g_tape[(gf - 2) * 2] - 0.5f) < 1e-6f);
        CHECK(fabs(g_tape[(gf + 2) * 2] - 0.25f) < 1e-4f);
        /* on the frame itself, unless gt is within 2 us of a tie */
        if (fabs(((double)(gt - s->T0) / sd_kd(s) - (double)(s->L + s->g))
                 - floor((double)(gt - s->T0) / sd_kd(s) - (double)(s->L + s->g)) - 0.5) * sd_kd(s) > 2000.0) {
            CHECK(fabs(g_tape[(gf - 1) * 2] - 0.5f) < 1e-6f);
            CHECK(fabs(g_tape[gf * 2] - 0.25f) < 1e-4f);
        }
    }

    /* loops: three passes; routing: right channel only */
    memset(&pd, 0, sizeof pd);
    pd.buf = b; pd.buf.n = 100; pd.loops = 2; pd.channels = 2; pd.at_ns = g_vt + 100000000;
    id = yau_play(&g_au, &pd);
    sd_run_s(s, 0.3);
    CHECK_I(yau_result(&g_au, id, &r), YAU_OK);
    CHECK_I(r.end_frame - r.start_frame, 300);
    CHECK(g_tape[r.start_frame * 2] == 0.0f && g_tape[r.start_frame * 2 + 1] == 0.5f);
    CHECK(g_tape[(r.start_frame + 299) * 2 + 1] == 0.5f && g_tape[(r.start_frame + 300) * 2 + 1] == 0.0f);

    /* forever, until stopped */
    pd.loops = YAU_FOREVER; pd.channels = 0; pd.at_ns = g_vt + 100000000;
    id = yau_play(&g_au, &pd);
    sd_run_s(s, 0.5);
    CHECK_I(yau_result(&g_au, id, &r), YAU_OK);
    CHECK_I(r.end_frame, 0);
    CHECK(yau_stop_at(&g_au, 0, 0, 0) == 0);
    sd_run_s(s, 0.2);
    CHECK_I(yau_result(&g_au, id, &r), YAU_OK);
    CHECK(r.end_frame > 0 && (r.flags & YAU_ONSET_STOPPED));

    /* clipping: two overlapping sounds at 0.5 + 0.6 */
    for (i = 0; i < 1000; i++) g_buf[48000 + i] = 0.6f;
    id = yau_play_at(&g_au, b, g_vt + 100000000);
    {
        yau_buf b2 = b;
        b2.frames = g_buf + 48000; b2.n = 1000;
        id2 = yau_play_at(&g_au, b2, g_vt + 100000000);
    }
    sd_run_s(s, 0.3);
    CHECK_I(yau_result(&g_au, id, &r), YAU_OK);
    CHECK(r.flags & YAU_ONSET_CLIPPED);
    CHECK(g_tape[r.start_frame * 2] == 1.0f);
    CHECK_I(yau_result(&g_au, id2, &r), YAU_OK);
    CHECK(r.flags & YAU_ONSET_CLIPPED);
    sd_run_s(s, 0.2);
    yau_close(&g_au);
}

/* An underrun: the record of the sound that played through it is XRUN and
 * UNCONFIRMED, tier 3; the device counted its silence, the header shifted its
 * map, and sounds after it land on the truth again. */
/* A plan made before the slope is known is made again in every callback
 * until the sound starts: handed over 6 s ahead in the first second, at 100
 * ppm, it still lands on the truth. */
static void test_replan(void) {
    sdev* s = &g_sd;
    yau_buf b;
    yau_onset r = { 0 };
    yau_id id;
    int64_t t;
    sd_default(s);
    s->drift_ppm = 100;
    CHECK(open_dev(s, NULL));
    sd_run_s(s, 1.0);
    memset(&b, 0, sizeof b);
    b.frames = g_buf; b.n = 100; b.channels = 1;
    t = g_vt + 6000000000LL + 7777;
    id = yau_play_at(&g_au, b, t);
    sd_run_s(s, 6.5);
    CHECK_I(yau_result(&g_au, id, &r), YAU_OK);
    CHECK(llabs(r.start_frame - sd_frame_of(s, t)) <= 1);
    printf("replan: start frame %lld, truth %lld\n", (long long)r.start_frame, (long long)sd_frame_of(s, t));
    yau_close(&g_au);
}

static void test_xrun(void) {
    sdev* s = &g_sd;
    yau_buf b;
    yau_onset r = { 0 };
    yau_id id;
    int64_t t;
    sd_default(s);
    s->jitter_ns = 100000;
    CHECK(open_dev(s, NULL));
    sd_run_s(s, 12.0);
    for (t = 0; t < 48000; t++) g_buf[t] = 0.25f;
    memset(&b, 0, sizeof b);
    b.frames = g_buf; b.n = 24000; b.channels = 1;
    id = yau_play_at(&g_au, b, g_vt + 60000000);
    sd_run_s(s, 0.1);
    s->stall_ns = 80000000;   /* 80 ms against a 40 ms pipeline */
    sd_run_s(s, 1.0);
    CHECK_I(yau_result(&g_au, id, &r), YAU_OK);
    CHECK(r.flags & YAU_ONSET_XRUN);
    CHECK(s->g > 0);
    CHECK_I(g_au.p_off, s->g);
    CHECK(g_au.xruns_ft >= 1);
    sd_run_s(s, 2.0);
    t = g_vt + 100000000;
    id = yau_play_at(&g_au, b, t);
    sd_run_s(s, 0.5);
    CHECK_I(yau_result(&g_au, id, &r), YAU_OK);
    CHECK(llabs(r.start_frame - sd_frame_of(s, t)) <= 1);
    CHECK(!(r.flags & YAU_ONSET_XRUN) && r.tier == YAU_TIER_2);
    /* The position jumps ahead of the fit with no late callback (measured
     * under load on WASAPI, by up to tens of ms, less than the queue). A short jump:
     * flagged, kept out of the fit, the fit unharmed. */
    {
        yau_caps c0, c1;
        uint32_t x0;
        yau_update(&g_au);
        yau_get_caps(&g_au, &c0);
        x0 = g_au.xruns_ft;
        t = g_vt + 80000000;
        id = yau_play_at(&g_au, b, t);
        s->pos_jump = 1400;
        sd_run_s(s, 0.5);
        s->pos_jump = 0;
        sd_run_s(s, 1.5);
        yau_update(&g_au);
        yau_get_caps(&g_au, &c1);
        CHECK(g_au.xruns_ft >= x0 + 1);
        CHECK_I(yau_result(&g_au, id, &r), YAU_OK);
        CHECK(r.flags & YAU_ONSET_XRUN);
        CHECK(fabs(c1.drift_ppm - c0.drift_ppm) < 1.0);
        t = g_vt + 100000000;
        id = yau_play_at(&g_au, b, t);
        sd_run_s(s, 0.5);
        CHECK_I(yau_result(&g_au, id, &r), YAU_OK);
        CHECK(llabs(r.start_frame - sd_frame_of(s, t)) <= 1);
        /* a jump that lasts: the fit starts over from the reports as they are */
        x0 = g_au.xruns_ft;
        s->pos_jump = 1400;
        sd_run_s(s, 4.0);
        yau_update(&g_au);
        CHECK(g_au.xruns_ft == x0 + 1);
        CHECK(g_au.fit.ready);
        CHECK(llabs(yau__fit_frame(&g_au.fit, g_vt) - (sd_frame_of(s, g_vt) + 1400)) <= 2);
        s->pos_jump = 0;
    }
    yau_close(&g_au);

    /* The WASAPI case, measured: the position halts during the stall and the
     * timing moves; the fit starts over and later sounds land on the truth. */
    sd_default(s);
    s->stall_halts = 1;
    CHECK(open_dev(s, NULL));
    sd_run_s(s, 8.0);
    s->stall_ns = 80000000;
    sd_run_s(s, 0.2);
    yau_update(&g_au);
    CHECK(g_au.xruns_ft >= 1);
    CHECK_I(g_au.p_off, 0);
    sd_run_s(s, 7.0);
    t = g_vt + 100000000;
    id = yau_play_at(&g_au, b, t);
    sd_run_s(s, 0.5);
    CHECK_I(yau_result(&g_au, id, &r), YAU_OK);
    CHECK(llabs(r.start_frame - sd_frame_of(s, t)) <= 1);
    yau_close(&g_au);
}

/* HALT: the underrun that WASAPI shared showed after stalls of 20 to 35 ms
 * (docs/audio.md): the engine runs dry before the position reaches the
 * frames written, the position stands for whole periods, and every later
 * frame plays that much later. x_per: how far past the queue's margin the
 * stall goes, in periods (under 0: ridden out). A sound planned before the
 * halt on a frame it moved: XRUN, UNCONFIRMED, tier 3, its time the new
 * fit's, which is the truth. The XRUN record has the halt. A later sound
 * is on the truth's frame, tier 2. ahead: the report of the late block
 * reads the position that many frames ahead, so it is early and has an
 * XRUN record of its own; the halt's record continues it, one underrun. */
static void halt_case(double x_per, int64_t want_periods, int64_t ahead) {
    sdev* s = &g_sd;
    yau_buf b;
    yau_onset r = { 0 };
    yau_id id;
    yrt_event ev[512];
    int64_t t, fa, ws, xr_halt = -1, xr_at = -1, stall;
    uint32_t x0;
    int n, i, xr_seen = 0;
    uint32_t xr_aux[4] = { 0, 0, 0, 0 };
    double kd;
    sd_default(s);
    s->jitter_ns = 50000;
    s->stall_halts = 1;
    s->halt_pipe = 480;
    s->stood_ahead = ahead;
    CHECK(open_dev(s, NULL));
    kd = sd_kd(s);
    sd_run_s(s, 8.0);
    yau_update(&g_au);
    x0 = g_au.xruns_ft;
    memset(&b, 0, sizeof b);
    b.frames = g_buf; b.n = 100; b.channels = 1;
    t = g_vt + 100000000 + 3333;
    fa = sd_frame_of(s, t);
    id = yau_play_at(&g_au, b, t);
    /* stall the block after the one that renders the sound: its frame is
     * between the halt (W - pipe) and the frames written */
    ws = (fa / s->period + 1) * s->period;
    while (s->W < ws) sd_block(s);
    while (yrt_ring_drain(&g_ring, ev, 512) > 0) {}
    stall = (int64_t)(((double)(s->L - s->halt_pipe) + x_per * (double)s->period) * kd);
    s->stall_ns = stall;
    s->halt_ns = 0;
    sd_run_s(s, 1.0);
    yau_update(&g_au);
    CHECK_I(s->halt_ns, (int64_t)floor((double)want_periods * (double)s->period * kd + 0.5));
    CHECK_I(g_au.xruns_ft, x0 + (want_periods > 0));
    CHECK_I(yau_result(&g_au, id, &r), YAU_OK);
    CHECK_I(r.start_frame, fa);
    if (want_periods > 0) {
        CHECK((r.flags & YAU_ONSET_XRUN) && (r.flags & YAU_ONSET_UNCONFIRMED));
        CHECK_I(r.tier, YAU_TIER_3);
        /* its time is the new fit's: the frame played after the halt.
         * With the early report the sound completes on that report, with
         * the old fit's time: which is right is not known (no loopback
         * ran), and the record says XRUN, tier 3. */
        if (ahead == 0) {
            CHECK(llabs(r.onset - sd_truth(s, fa)) < 60000);
            CHECK(llabs(r.residual - s->halt_ns) < 60000);
        }
    } else {
        CHECK(!(r.flags & YAU_ONSET_XRUN));
        CHECK_I(r.tier, YAU_TIER_2);
    }
    while ((n = yrt_ring_drain(&g_ring, ev, 512)) > 0)
        for (i = 0; i < n; i++)
            if (ev[i].source == YRT_SRC_AUDIO && ev[i].kind == YAU_EV_XRUN) {
                if (xr_seen < 4) xr_aux[xr_seen] = ev[i].aux;
                xr_seen++;
                xr_halt = ev[i].u.i64[2];
                xr_at = ev[i].u.i64[3];
            }
    CHECK_I(xr_seen, want_periods > 0 ? 1 + (ahead > 0) : 0);
    if (ahead > 0) CHECK_I(xr_aux[1], xr_aux[0]);
    if (want_periods > 0) {
        CHECK(llabs(xr_halt - s->halt_ns) < 60000);
        CHECK(xr_at >= ws - s->halt_pipe && xr_at < ws + 4 * s->period);
    }
    printf("halt: stall %.1f ms, halt %.1f ms, seen %d, record %.3f ms, onset - truth %lld ns\n",
           (double)stall / 1e6, (double)s->halt_ns / 1e6, xr_seen, (double)xr_halt / 1e6,
           (long long)(r.onset - sd_truth(s, fa)));
    sd_run_s(s, 1.0);
    t = g_vt + 100000000;
    id = yau_play_at(&g_au, b, t);
    sd_run_s(s, 0.5);
    CHECK_I(yau_result(&g_au, id, &r), YAU_OK);
    CHECK(llabs(r.start_frame - sd_frame_of(s, t)) <= 1);
    CHECK(!(r.flags & YAU_ONSET_XRUN) && r.tier == YAU_TIER_2);
    yau_close(&g_au);
}

/* Reports late by most of a period with no halt (a call preempted between
 * the position and its stamp): two in a row, or one at a time, are not a
 * halt, and the sound they pass is confirmed, tier 2, on its frame. */
static void stale_case(int n_in_row, int times) {
    sdev* s = &g_sd;
    yau_buf b;
    yau_onset r = { 0 };
    yau_id id;
    int64_t t, fc;
    uint32_t x0;
    int k;
    sd_default(s);
    s->jitter_ns = 50000;
    s->stall_halts = 1;
    s->halt_pipe = 480;
    CHECK(open_dev(s, NULL));
    sd_run_s(s, 8.0);
    yau_update(&g_au);
    x0 = g_au.xruns_ft;
    memset(&b, 0, sizeof b);
    b.frames = g_buf; b.n = 100; b.channels = 1;
    t = g_vt + 60000000;
    fc = sd_frame_of(s, t);
    id = yau_play_at(&g_au, b, t);
    for (k = 0; k < times; k++) {
        s->stale_ns = 9000000;
        s->stale_n = n_in_row;
        sd_run(s, n_in_row + 1);
    }
    sd_run_s(s, 0.5);
    yau_update(&g_au);
    CHECK_I(g_au.xruns_ft, x0);
    CHECK_I(yau_result(&g_au, id, &r), YAU_OK);
    CHECK_I(r.start_frame, fc);
    CHECK(!(r.flags & YAU_ONSET_XRUN) && r.tier == YAU_TIER_2);
    yau_close(&g_au);
}

static void test_halt(void) {
    halt_case(-0.5, 0, 0);   /* ridden out */
    halt_case(0.3, 1, 0);    /* the report of the late block is on time */
    halt_case(0.7, 1, 0);    /* it is late by most of the halt: not its length */
    halt_case(1.5, 2, 0);
    halt_case(2.7, 3, 0);
    halt_case(0.1, 1, 360);  /* it is early: one underrun, two records */
    stale_case(2, 1);
    stale_case(1, 4);
}

/* A device with callback times only: planned onsets, tier 3; an underrun
 * from a gap between callbacks. */
static void test_callback_source(void) {
    sdev* s = &g_sd;
    yau_buf b;
    yau_onset r = { 0 };
    yau_id id;
    sd_default(s);
    s->pos_source = YAU_POS_CALLBACK;
    s->tier = 0;
    CHECK(open_dev(s, NULL));
    sd_run_s(s, 12.0);
    memset(&b, 0, sizeof b);
    b.frames = g_buf; b.n = 100; b.channels = 1;
    id = yau_play_at(&g_au, b, g_vt + 80000000);
    sd_run_s(s, 0.2);
    CHECK_I(yau_result(&g_au, id, &r), YAU_OK);
    CHECK(r.flags & YAU_ONSET_UNCONFIRMED);
    CHECK_I(r.tier, YAU_TIER_3);
    CHECK_I(r.device_pos, -1);
    /* the plan is the callback-time line: frame for t is the one rendered
     * at about t (the offset to the output is not known) */
    s->stall_ns = 100000000;
    sd_run_s(s, 0.2);
    yau_update(&g_au);
    CHECK(g_au.xruns_ft >= 1);
    yau_close(&g_au);

    /* The null device's case: callback times only and no hardware buffer
     * (tier SIM). The same 100 ms gap is timer jitter there, not an
     * underrun: no XRUN, and the fit keeps its points (on a loaded macOS
     * CI VM every gap restarted the fit, so open() never returned). */
    {
        uint32_t n_before;
        sd_default(s);
        s->pos_source = YAU_POS_CALLBACK;
        s->tier = YAU_TIER_SIM;
        CHECK(open_dev(s, NULL));
        sd_run_s(s, 2.0);
        yau_update(&g_au);
        n_before = g_au.fit_ft.n;
        CHECK(g_au.fit_ft.ready);
        s->stall_ns = 100000000;
        sd_run_s(s, 0.2);
        yau_update(&g_au);
        CHECK_I(g_au.xruns_ft, 0);
        CHECK(g_au.fit_ft.ready);
        CHECK(g_au.fit_ft.n >= n_before);
        yau_close(&g_au);
    }
}

/* The records in the ring, field by field. */
static void test_ring(void) {
    sdev* s = &g_sd;
    yau_buf b;
    yau_onset r = { 0 };
    yau_id id;
    yrt_event ev[512];
    int n, i, seen_onset = 0, seen_end = 0, seen_fit = 0, seen_open = 0, seen_clock = 0;
    yau_desc d;
    sd_default(s);
    memset(&d, 0, sizeof d);
    d.device_index = 5;
    d.min_tier = 2;   /* the scripted path is tier 2: not below it */
    CHECK(open_dev(s, &d));
    sd_run_s(s, 2.0);
    memset(&b, 0, sizeof b);
    b.frames = g_buf; b.n = 480; b.channels = 1; b.id = 4242;
    id = yau_play_at(&g_au, b, g_vt + 80000000);
    sd_run_s(s, 0.3);
    CHECK_I(yau_result(&g_au, id, &r), YAU_OK);
    while ((n = yrt_ring_drain(&g_ring, ev, 512)) > 0) {
        for (i = 0; i < n; i++) {
            const yrt_event* e = &ev[i];
            if (e->source == YRT_SRC_RT && e->kind == YRT_KIND_CLOCK && e->aux == 0x41550005u) seen_clock++;
            if (e->source != YRT_SRC_AUDIO) continue;
            if (e->kind == YAU_EV_ONSET && e->aux == (uint32_t)id) {
                seen_onset++;
                CHECK_I((int64_t)e->t_ns, r.onset);
                CHECK_I(e->u.i64[0], r.target);
                CHECK_I(e->u.i64[1], r.start_frame);
                CHECK(e->u.i64[2] > r.start_frame);
                CHECK_I(e->u.i64[3], r.rendered_at);
                CHECK_I(e->u.u32[8], 4242);
                CHECK_I(e->u.u16[18], r.flags);
                CHECK_I(YAU_EV_TIER_OF(e->u.u16[19]), YAU_TIER_2);
                CHECK_I(YAU_EV_DEVICE_OF(e->u.u16[19]), 5);
            }
            if (e->kind == YAU_EV_END && e->aux == (uint32_t)id) { seen_end++; CHECK_I(e->u.i64[0], r.start_frame + 480); CHECK_I(e->u.i64[1], 480); }
            if (e->kind == YAU_EV_FIT) { seen_fit++; CHECK(e->u.f64[1] > 47990 && e->u.f64[1] < 48010); }
            if (e->kind == YAU_EV_OPEN) { seen_open++; CHECK_I(e->u.i32[0], 48000); CHECK_I(e->u.i32[1], 2); CHECK_I(e->u.i32[8], YAU_POS_DEVICE); }
        }
    }
    CHECK_I(seen_onset, 1);
    CHECK_I(seen_end, 1);
    CHECK(seen_fit > 0);
    CHECK_I(seen_open, 1);
    CHECK(seen_clock >= 1);
    CHECK(!(r.flags & YAU_ONSET_BELOW_TIER));
    yau_close(&g_au);
    /* min_tier 1: a tier-2 onset is flagged, and the run goes on */
    sd_default(s);
    d.min_tier = 1;
    CHECK(open_dev(s, &d));
    sd_run_s(s, 1.0);
    id = yau_play_at(&g_au, b, g_vt + 80000000);
    sd_run_s(s, 0.3);
    CHECK_I(yau_result(&g_au, id, &r), YAU_OK);
    CHECK(r.flags & YAU_ONSET_BELOW_TIER);
    yau_close(&g_au);
}

/* The queues and the voices, the results table, close. */
static void test_limits(void) {
    sdev* s = &g_sd;
    yau_buf b;
    yau_desc d;
    yau_onset r = { 0 };
    yau_id first, id;
    int i;
    sd_default(s);
    memset(&d, 0, sizeof d);
    d.voices = 4;
    CHECK(open_dev(s, &d));
    sd_run_s(s, 1.0);
    memset(&b, 0, sizeof b);
    b.frames = g_buf; b.n = 480; b.channels = 1;
    first = yau_play_at(&g_au, b, g_vt + 500000000);
    for (i = 1; i < 4; i++) CHECK(yau_play_at(&g_au, b, g_vt + 500000000) > 0);
    CHECK_I(yau_play_at(&g_au, b, g_vt + 500000000), YAU_ERR_FULL);
    /* bad buffers */
    b.channels = 3;
    CHECK_I(yau_play_at(&g_au, b, 0), YAU_ERR_FORMAT);
    b.channels = 1; b.frames = NULL;
    CHECK_I(yau_play_at(&g_au, b, 0), YAU_ERR_ARG);
    b.frames = g_buf;
    /* close: never-started sounds are CANCELED */
    yau_close(&g_au);
    CHECK(s->stopped && s->closed);
    CHECK(!yau_is_open(&g_au));

    /* 256 commands fill the queue when nothing renders */
    sd_default(s);
    memset(&d, 0, sizeof d);
    d.voices = 64;
    CHECK(open_dev(s, &d));
    for (i = 0; i < YAU__CMDS; i++) CHECK(yau_stop_at(&g_au, 0, 0, 0) == 0);
    CHECK_I(yau_stop_at(&g_au, 0, 0, 0), YAU_ERR_FULL);
    sd_run(s, 10);
    /* the results table keeps the last 256 */
    first = 0;
    for (i = 0; i < 300; i++) {
        id = yau_play_at(&g_au, b, 0);
        if (i == 0) first = id;
        sd_run(s, 1);
    }
    sd_run_s(s, 0.5);
    CHECK_I(yau_result(&g_au, first, &r), YAU_ERR_NOT_FOUND);
    CHECK_I(yau_result(&g_au, id, &r), YAU_OK);
    CHECK_I(g_au.outstanding, 0);
    yau_close(&g_au);
    (void)first;
}

/* STRICT OPEN: each mismatch is refused with a message that names it. */
static void test_strict_open(void) {
    sdev* s = &g_sd;
    yau_desc d;
    static const uint8_t lr[2] = { YAU_CH_FL, YAU_CH_FR }, rl[2] = { YAU_CH_FR, YAU_CH_FL };
    sd_default(s);
    s->rate = 44100;
    CHECK(!open_dev(s, NULL));
    CHECK(strstr(yau_error(&g_au), "44100") && strstr(yau_error(&g_au), "48000"));
    CHECK(s->closed);
    sd_default(s);
    s->channels = 6;
    CHECK(!open_dev(s, NULL));
    CHECK(strstr(yau_error(&g_au), "6 channels") != NULL);
    sd_default(s);
    s->out = YAU_OUT_S16;
    memset(&d, 0, sizeof d);
    d.format.sample = YAU_S24;
    CHECK(!open_dev(s, &d));
    CHECK(strstr(yau_error(&g_au), "s16") != NULL);
    sd_default(s);
    d.format.sample = YAU_F32;
    d.format.map = rl;
    CHECK(!open_dev(s, &d));
    CHECK(strstr(yau_error(&g_au), "channel 0") != NULL);
    d.format.map = lr;
    CHECK(open_dev(s, &d));
    yau_close(&g_au);
    sd_default(s);
    s->fail_open = 1;
    CHECK(!open_dev(s, NULL));
    CHECK(strstr(yau_error(&g_au), "refused") != NULL);
    memset(&d, 0, sizeof d);
    d.backend = YAU_BACKEND_WASAPI;
    CHECK(!yau_open(&g_au, &d));   /* YAU_NO_MINIAUDIO */
    CHECK(!yau_open(&g_au, NULL));
}

/* Output formats: one sound at unity gain reproduces 16- and 24-bit values
 * bit for bit. */
static void test_formats(void) {
    sdev* s = &g_sd;
    int outs[3] = { YAU_OUT_S16, YAU_OUT_S32, YAU_OUT_S24 };
    int j;
    for (j = 0; j < 3; j++) {
        yau_buf b;
        yau_onset r = { 0 };
        yau_id id;
        int i, bad = 0;
        double scale = outs[j] == YAU_OUT_S16 ? 32768.0 : 8388608.0;
        yau_desc d;
        memset(&d, 0, sizeof d);
        if (outs[j] == YAU_OUT_S16) d.format.sample = YAU_S16;
        sd_default(s);
        s->out = (uint16_t)outs[j];
        if (!open_dev(s, &d)) { fail(__LINE__, yau_error(&g_au)); continue; }
        sd_run_s(s, 1.0);
        for (i = 0; i < 4000; i++) g_buf[i] = (float)((double)((i * 7919) % (int)(2 * scale) - (int)scale) / scale);
        memset(&b, 0, sizeof b);
        b.frames = g_buf; b.n = 4000; b.channels = 1;
        id = yau_play_at(&g_au, b, g_vt + 80000000);
        sd_run_s(s, 0.3);
        CHECK_I(yau_result(&g_au, id, &r), YAU_OK);
        for (i = 0; i < 4000; i++) if (g_tape[(r.start_frame + i) * 2] != g_buf[i]) bad++;
        CHECK_I(bad, 0);
        CHECK(!(r.flags & YAU_ONSET_CLIPPED));
        yau_close(&g_au);
    }
}

/* Synthesis: levels, ramps, seeds, the noise refusal, the arena. */
static void test_synthesis(void) {
    sdev* s = &g_sd;
    yau_tone_desc td;
    yau_noise_desc nd;
    yau_click_desc cd;
    yau_buf t, n1, n2;
    double ss = 0, mx = 0;
    int64_t i;
    float pk = 0;
    yau_id id;
    yau_onset r = { 0 };
    sd_default(s);
    CHECK(open_dev(s, NULL));
    sd_run_s(s, 1.0);
    CHECK(fabs(yau_db(-20.0f) - 0.1f) < 1e-6f);
    memset(&td, 0, sizeof td);
    td.hz = 1000; td.dur_ns = YRT_MS(100); td.peak = 0.5f; td.ramp_ns = YRT_MS(10);
    t = yau_tone(&g_au, &td);
    CHECK(t.frames && t.n == 4800 && t.channels == 1);
    if (t.frames) {
        CHECK(t.frames[0] == 0.0f);
        for (i = 480; i < 4320; i++) if (fabs(t.frames[i]) > mx) mx = fabs(t.frames[i]);
        CHECK(fabs(mx - 0.5) < 1e-4);
        CHECK(fabs(t.frames[480 + 12]) > 0.4999f);   /* a quarter cycle in */
        CHECK(fabs(t.frames[4799]) < 1e-3f);
    }
    td.hz = 30000;
    CHECK(yau_tone(&g_au, &td).frames == NULL);
    td.hz = 1000; td.peak = 0;
    CHECK(yau_tone(&g_au, &td).frames == NULL);
    memset(&nd, 0, sizeof nd);
    nd.dur_ns = YRT_S(1); nd.rms = 0.1f; nd.seed = 99;
    n1 = yau_noise(&g_au, &nd);
    n2 = yau_noise(&g_au, &nd);
    CHECK(n1.frames && n2.frames);
    if (n1.frames && n2.frames) {
        CHECK(memcmp(n1.frames, n2.frames, sizeof(float) * (size_t)n1.n) == 0);
        for (i = 0; i < n1.n; i++) ss += (double)n1.frames[i] * n1.frames[i];
        CHECK(fabs(sqrt(ss / (double)n1.n) - 0.1) < 0.002);
    }
    nd.seed = 100;
    n2 = yau_noise(&g_au, &nd);
    CHECK(n2.frames && n2.frames[0] != n1.frames[0]);
    nd.rms = 0.3f;
    CHECK(yau_noise(&g_au, &nd).frames == NULL);
    CHECK(strstr(yau_error(&g_au), "peaks at") != NULL);
    printf("synthesis: refusal says: %s\n", yau_error(&g_au));
    nd.dist = YAU_UNIFORM;
    n2 = yau_noise(&g_au, &nd);
    CHECK(n2.frames != NULL);
    CHECK(yau_fill_noise(g_buf, 1000, 1, 48000, &nd, &pk) == 0 && pk <= 0.3f * 1.7321f);
    memset(&cd, 0, sizeof cd);
    cd.peak = -0.25f;
    n2 = yau_click(&g_au, &cd);
    CHECK(n2.frames && n2.n == 1 && n2.frames[0] == -0.25f);
    /* the arena: busy while one of its buffers waits or plays */
    id = yau_play_at(&g_au, t, g_vt + 80000000);
    CHECK_I(yau_arena_reset(&g_au), YAU_ERR_BUSY);
    sd_run_s(s, 0.4);
    CHECK_I(yau_result(&g_au, id, &r), YAU_OK);
    CHECK_I(yau_arena_reset(&g_au), 0);
    CHECK(yau_alloc(&g_au, (int64_t)sizeof g_arena, 1).frames == NULL);   /* too big */
    CHECK(yau_mix(g_buf, 10, 1, g_buf + 100, 20, 5, 1.0f) == 0);
    yau_close(&g_au);
}

/* The device on a real thread against the frame thread: plays, cancels and
 * updates race with renders. ThreadSanitizer checks the two queues. */
static uint32_t g_thr_stop = 0;   /* atomic: the header's own helpers */
#if defined(_WIN32)
static unsigned __stdcall thr_main(void* p)
#else
static void* thr_main(void* p)
#endif
{
    sdev* s = (sdev*)p;
    yau_rt_thread_init(s->host);
    while (!yau__ld32(&g_thr_stop)) {
        yau_tick tk;
        memset(&tk, 0, sizeof tk);
        tk.t_entry = yrt_now_ns();
        tk.pos = s->W > s->L ? s->W - s->L : 0;
        tk.pos_t = tk.t_entry;
        s->render(s->host, s->out_mem, s->period, &tk);
        if (s->tape && s->W + s->period <= TAPE_FRAMES)
            memcpy(s->tape + s->W * 2, s->out_mem, sizeof(float) * 2 * (size_t)s->period);
        s->W += s->period;
        yrt_sleep_until(yrt_now_ns() + 1000000, 0);
    }
#if defined(_WIN32)
    return 0;
#else
    return NULL;
#endif
}

static void test_threads(void) {
    sdev* s = &g_sd;
    yau_buf b;
    yau_id ids[400];
    int i, done = 0, n = 400;
    sd_default(s);
    s->tape = NULL;
    s->period = 48;
    s->L = 96;
    g_virtual = 0;
    CHECK(open_dev(s, NULL));
    memset(&b, 0, sizeof b);
    b.frames = g_buf; b.n = 48; b.channels = 1;
    yau__st32(&g_thr_stop, 0);
    {
#if defined(_WIN32)
        HANDLE th = (HANDLE)_beginthreadex(NULL, 0, thr_main, s, 0, NULL);
#else
        pthread_t th;
        pthread_create(&th, NULL, thr_main, s);
#endif
        for (i = 0; i < n; i++) {
            ids[i] = yau_play_at(&g_au, b, 0);
            if (ids[i] == YAU_ERR_FULL) { i--; yrt_sleep_until(yrt_now_ns() + 200000, 0); continue; }
            if (i % 3 == 0) (void)yau_cancel(&g_au, ids[i]);
            if (i % 50 == 0) (void)yau_stop_at(&g_au, 0, 0, 0);
            (void)yau_update(&g_au);
        }
        for (i = 0; i < 2000 && done < n; i++) {
            int j;
            yrt_sleep_until(yrt_now_ns() + 1000000, 0);
            done = 0;
            for (j = 0; j < n; j++) {
                yau_onset r = { 0 };
                int rc = yau_result(&g_au, ids[j], &r);
                if (rc == YAU_OK || rc == YAU_ERR_NOT_FOUND) done++;
            }
        }
        yau__st32(&g_thr_stop, 1);
#if defined(_WIN32)
        WaitForSingleObject(th, INFINITE);
        CloseHandle(th);
#else
        pthread_join(th, NULL);
#endif
    }
    printf("threads: %d of %d sounds completed\n", done, n);
    CHECK_I(done, n);
    yau_close(&g_au);
    CHECK_I(g_au.outstanding, 0);
    g_virtual = 1;
}

/* The mixer's regression tape: buffer voices through every path of the
 * gain, fade and routing kernel (mono to stereo, masks, a stereo buffer,
 * a gain ramp, a stop fade, loops, clipping), on f32 and s16 devices. With
 * --tape FILE the tape is written there; the v0.2.0 kernel refactor was
 * checked against the file the v0.1.0 header wrote, byte for byte. */
static uint64_t tape_regression(const char* path) {
    sdev* s = &g_sd;
    int outs[2] = { YAU_OUT_F32, YAU_OUT_S16 };
    uint64_t h = 1469598103934665603ull;
    FILE* fp = path ? fopen(path, "wb") : NULL;
    int j;
    for (j = 0; j < 2; j++) {
        yau_desc d;
        yau_buf mono, st2;
        yau_play_desc pd;
        yau_id id;
        int64_t i, t0, n;
        memset(&d, 0, sizeof d);
        if (outs[j] == YAU_OUT_S16) d.format.sample = YAU_S16;
        sd_default(s);
        s->out = (uint16_t)outs[j];
        if (!open_dev(s, &d)) { fail(__LINE__, yau_error(&g_au)); continue; }
        sd_run_s(s, 1.0);
        for (i = 0; i < 48000; i++) g_buf[i] = (float)((i % 997) + 1) / 2000.0f;
        for (i = 0; i < 24000; i++) { g_buf[48000 + 2 * i] = 0.3f; g_buf[48000 + 2 * i + 1] = -(float)((i % 101) + 1) / 400.0f; }
        memset(&mono, 0, sizeof mono);
        mono.frames = g_buf; mono.n = 9000; mono.channels = 1;
        memset(&st2, 0, sizeof st2);
        st2.frames = g_buf + 48000; st2.n = 7000; st2.channels = 2;
        t0 = g_vt + 80000000;
        (void)yau_play_at(&g_au, mono, t0);
        memset(&pd, 0, sizeof pd);
        pd.buf = mono; pd.at_ns = t0 + 3000000; pd.channels = 2; pd.db = -3.0f;
        id = yau_play(&g_au, &pd);
        (void)yau_gain_at(&g_au, id, t0 + 50000000, -12.0f, YRT_MS(3));
        pd.buf = st2; pd.at_ns = t0 + 7777; pd.channels = 0; pd.db = 0;
        id = yau_play(&g_au, &pd);
        (void)yau_stop_at(&g_au, id, t0 + 120000000, YRT_MS(5));
        pd.buf = st2; pd.at_ns = t0 + 20000000; pd.channels = 1; pd.db = 2.0f;
        (void)yau_play(&g_au, &pd);
        pd.buf = mono; pd.buf.n = 333; pd.loops = 4; pd.at_ns = t0 + 40000000; pd.channels = 0; pd.db = 6.0f;
        (void)yau_play(&g_au, &pd);
        pd.loops = 0; pd.buf = mono; pd.at_ns = 0;
        (void)yau_play(&g_au, &pd);
        sd_run_s(s, 1.0);
        n = s->W < TAPE_FRAMES ? s->W : TAPE_FRAMES;
        for (i = 0; i < n * 2; i++) {
            uint32_t b;
            memcpy(&b, &g_tape[i], 4);
            h = (h ^ b) * 1099511628211ull;
        }
        if (fp) fwrite(g_tape, sizeof(float), (size_t)(n * 2), fp);
        yau_close(&g_au);
    }
    if (fp) fclose(fp);
    printf("tape: FNV-1a %016llx\n", (unsigned long long)h);
    return h;
}

#if YAU_VERSION_MINOR >= 2
/* ----------------------------------------------------------------- streams */

/* Identity samples: never 0, and multiples of 2^-13 below 0.5, so a sum of
 * a few of them, or of them and the buffers below, is exact in float in
 * any order. */
static float idv(int64_t s, int c) { return (float)((((s * 2 + c) % 4093) + 1)) / 8192.0f; }

static yau_stream g_st, g_st2;
static float g_ring_a[1000 * 2], g_ring_b[48000 * 2];

/* The producer: writes identity samples from *next on, through acquire and
 * commit, in chunks that cycle through awkward sizes; at most `max`. */
static int64_t feed(yau_stream* st, int64_t* next, int64_t max, int ch) {
    static const int64_t sizes[5] = { 1, 479, 997, 4097, 13 };
    static int k = 0;
    int64_t done = 0;
    for (;;) {
        int64_t n, i;
        int c;
        float* p;
        if (done >= max) break;
        p = yau_stream_acquire(st, &n);
        if (!p) break;
        if (n > sizes[k % 5]) n = sizes[k % 5];
        if (n > max - done) n = max - done;
        k++;
        for (i = 0; i < n; i++) for (c = 0; c < ch; c++) p[i * ch + c] = idv(*next + i, c);
        if (yau_stream_commit(st, n) != 0) { fail(__LINE__, "commit refused"); break; }
        *next += n;
        done += n;
    }
    return done;
}

/* Checks every output frame of one stream play against the identity: zero
 * before its start and after its end, sample f - origin or silence (a gap)
 * between. */
typedef struct chk {
    yau_stream* st;
    int     ch;
    int64_t bad, data, silent, first_bad;
} chk;
static chk g_chk;
static float (*g_chk_val)(int64_t, int) = NULL;   /* NULL = idv */
static void chk_val(float (*val)(int64_t, int)) { g_chk_val = val; }

static void chk_block(sdev* s, const float* out, int32_t n) {
    yau_stream_info in;
    int64_t i, endf = INT64_MAX;
    yau_stream_get_info(g_chk.st, &in);
    if (in.started && in.state == YAU_STREAM_ENDED) endf = in.origin + in.next;
    for (i = 0; i < n; i++) {
        int64_t f = s->W + i;
        float a = out[i * 2], b = out[i * 2 + 1];
        int ok;
        if (!in.started || f < in.start_frame || f >= endf) ok = a == 0.0f && b == 0.0f;
        else if (a == 0.0f && b == 0.0f) { ok = 1; g_chk.silent++; }
        else {
            int64_t smp = f - in.origin;
            float (*val)(int64_t, int) = g_chk_val ? g_chk_val : idv;
            ok = a == val(smp, 0) && b == val(smp, g_chk.ch == 1 ? 0 : 1);
            g_chk.data++;
        }
        if (!ok && g_chk.bad++ == 0) g_chk.first_bad = f;
    }
}

static void chk_start(yau_stream* st, int ch) {
    memset(&g_chk, 0, sizeof g_chk);
    g_chk.st = st;
    g_chk.ch = ch;
    g_chk_val = NULL;
    g_sd.on_block = chk_block;
}

/* Blocks of the scripted device with the producer keeping the ring full. */
static void run_fed(sdev* s, yau_stream* st, int64_t* next, int ch, double sec) {
    int64_t b, nb = (int64_t)(sec * s->rate / s->period + 0.5);
    for (b = 0; b < nb; b++) { (void)feed(st, next, INT64_MAX, ch); sd_block(s); }
}

static int near_tie(const sdev* s, int64_t t) {
    double x = (double)(t - s->T0) / sd_kd(s) - (double)(s->L + s->g);
    return fabs((x - floor(x)) - 0.5) * sd_kd(s) < 2000.0;
}

/* Placement exact to the frame: a 60 s stream through a 1000-frame ring
 * (2880 wraps), then short plays at every phase between frames, at three
 * drifts, with a reset before each. */
static void test_stream_placement(void) {
    sdev* s = &g_sd;
    double drifts[3] = { 37.0, 100.0, -100.0 };
    int j;
    for (j = 0; j < 3; j++) {
        yau_stream_info in;
        yau_onset r = { 0 };
        yau_id id;
        int64_t next, t, want;
        int k, exact = 0, plays = j == 0 ? 10 : 20;
        sd_default(s);
        s->tape = NULL;
        s->drift_ppm = drifts[j];
        CHECK(open_dev(s, NULL));
        sd_run_s(s, 8.0);
        CHECK(yau_stream_open(&g_au, &g_st, &(yau_stream_desc){ .memory = g_ring_a, .frames = 1000, .id = 31, .first = 5 }));
        next = 5;
        (void)feed(&g_st, &next, INT64_MAX, 2);
        chk_start(&g_st, 2);
        if (j == 0) {
            t = g_vt + 70000000 + 12345;
            id = yau_play_stream(&g_au, &g_st, t);
            CHECK(id > 0);
            run_fed(s, &g_st, &next, 2, 60.0);
            CHECK_I(yau_result(&g_au, id, &r), YAU_OK);
            yau_stream_get_info(&g_st, &in);
            want = sd_frame_of(s, t);
            if (!near_tie(s, t)) CHECK_I(r.start_frame, want);
            CHECK_I(in.origin, r.start_frame - 5);
            CHECK_I(r.sample, 5);
            CHECK_I(r.tier, YAU_TIER_2);
            CHECK(!(r.flags & (YAU_ONSET_UNCONFIRMED | YAU_ONSET_LATE | YAU_ONSET_GAP)));
            CHECK_I(r.buffer_id, 31);
            CHECK_I(in.gaps, 0);
            CHECK(g_chk.data > 48000 * 59);
            printf("stream: 60 s through a 1000-frame ring, %lld frames checked, %lld wrong\n",
                   (long long)g_chk.data, (long long)g_chk.bad);
            CHECK(yau_stop_at(&g_au, id, 0, 0) == 0);
            run_fed(s, &g_st, &next, 2, 0.2);
            CHECK_I(yau_result(&g_au, id, &r), YAU_OK);
            CHECK(r.end_frame > 0 && (r.flags & YAU_ONSET_STOPPED));
            CHECK_I(g_chk.bad, 0);
        }
        for (k = 0; k < plays; k++) {
            int64_t first = 1000 + k * 7919;
            CHECK_I(yau_stream_reset(&g_st, first), 0);
            next = first;
            (void)feed(&g_st, &next, 900, 2);
            CHECK_I(yau_stream_end(&g_st), 0);
            chk_start(&g_st, 2);
            t = g_vt + 60000000 + (int64_t)(sd_rand(s) * 100000000.0);
            id = yau_play_stream(&g_au, &g_st, t);
            CHECK(id > 0);
            sd_run_s(s, 0.25);
            CHECK_I(yau_result(&g_au, id, &r), YAU_OK);
            want = sd_frame_of(s, t);
            if (r.start_frame == want || near_tie(s, t)) exact++;
            else fail_i(__LINE__, "stream start off the truth", r.start_frame, want);
            CHECK_I(r.end_frame, r.start_frame + 900);
            CHECK_I(r.sample, first);
            CHECK(r.tier == YAU_TIER_2 && !(r.flags & YAU_ONSET_STOPPED));
            CHECK_I(g_chk.data, 900);
            CHECK_I(g_chk.bad, 0);
        }
        printf("stream: drift %+.0f ppm, %d of %d starts exact\n", drifts[j], exact, plays);
        CHECK_I(exact, plays);
        s->on_block = NULL;
        yau_stream_close(&g_au, &g_st);
        yau_close(&g_au);
    }
}

/* The producer side alone: regions, refusals, space after a block. */
static void test_stream_ring(void) {
    sdev* s = &g_sd;
    int64_t n, n2, next = 0;
    float* p;
    float tmp[600 * 2];
    int i;
    yau_stream_info in;
    sd_default(s);
    CHECK(open_dev(s, NULL));
    sd_run_s(s, 1.0);
    CHECK(yau_stream_open(&g_au, &g_st, &(yau_stream_desc){ .memory = g_ring_a, .frames = 1000 }));
    p = yau_stream_acquire(&g_st, &n);
    CHECK(p == g_ring_a && n == 1000);
    CHECK_I(yau_stream_commit(&g_st, 1001), YAU_ERR_ARG);
    CHECK_I(yau_stream_commit(&g_st, 700), 0);
    p = yau_stream_acquire(&g_st, &n);
    CHECK(p == g_ring_a + 1400 && n == 300);
    CHECK_I(yau_stream_commit(&g_st, 300), 0);
    CHECK(yau_stream_acquire(&g_st, &n) == NULL && n == 0);
    for (i = 0; i < 1200; i++) tmp[i] = 0.25f;
    CHECK_I(yau_stream_write(&g_st, tmp, 600), 0);
    yau_stream_get_info(&g_st, &in);
    CHECK(in.fill == 1000 && in.written == 1000 && in.read == 0 && in.state == YAU_STREAM_IDLE);
    CHECK(in.ready && in.preroll == 250 && !in.started && in.start_frame == -1);
    /* the device reads 480: the space is exactly that, and wraps */
    CHECK(yau_play_stream(&g_au, &g_st, 0) > 0);
    CHECK_I(yau_play_stream(&g_au, &g_st, 0), YAU_ERR_BUSY);
    CHECK_I(yau_stream_reset(&g_st, 0), YAU_ERR_BUSY);
    yau_stream_get_info(&g_st, &in);
    CHECK_I(in.state, YAU_STREAM_QUEUED);
    sd_block(s);
    yau_stream_get_info(&g_st, &in);
    CHECK_I(in.state, YAU_STREAM_PLAYING);
    CHECK_I(in.read, 480);
    CHECK_I(yau_stream_write(&g_st, tmp, 600), 480);
    CHECK(yau_stream_acquire(&g_st, &n) == NULL);
    sd_block(s);
    p = yau_stream_acquire(&g_st, &n);
    CHECK(p == g_ring_a + 2 * 480 && n == 480);
    (void)feed(&g_st, &next, 100, 2);
    p = yau_stream_acquire(&g_st, &n2);
    CHECK(p != NULL && n2 == 380);
    CHECK_I(yau_stream_end(&g_st), 0);
    CHECK(yau_stream_acquire(&g_st, &n) == NULL);
    CHECK_I(yau_stream_commit(&g_st, 0), YAU_ERR_ARG);
    sd_run_s(s, 0.3);
    yau_stream_get_info(&g_st, &in);
    CHECK_I(in.state, YAU_STREAM_ENDED);
    CHECK_I(in.next, 1580);
    CHECK_I(yau_stream_reset(&g_st, 77), 0);
    yau_stream_get_info(&g_st, &in);
    CHECK(in.state == YAU_STREAM_IDLE && in.first == 77 && in.written == 77 && in.fill == 0 && in.end == -1);
    /* refusals */
    {
        yau_play_desc pd;
        memset(&pd, 0, sizeof pd);
        pd.stream = &g_st; pd.loops = 1;
        CHECK_I(yau_play(&g_au, &pd), YAU_ERR_ARG);
        pd.loops = 0; pd.offset = 3;
        CHECK_I(yau_play(&g_au, &pd), YAU_ERR_ARG);
        pd.offset = 0; pd.buf.frames = g_buf; pd.buf.n = 10;
        CHECK_I(yau_play(&g_au, &pd), YAU_ERR_ARG);
    }
    CHECK(!yau_stream_open(&g_au, &g_st2, &(yau_stream_desc){ .memory = g_ring_b, .frames = 959 }));
    CHECK(!yau_stream_open(&g_au, &g_st2, &(yau_stream_desc){ .memory = g_ring_b, .channels = 3 }));
    CHECK(strstr(yau_error(&g_au), "yau_stream_open") != NULL);
    CHECK(!yau_stream_open(&g_au, &g_st2, &(yau_stream_desc){ .memory = g_ring_b, .first = -1 }));
    CHECK(!yau_stream_open(&g_au, &g_st2, &(yau_stream_desc){ .memory = g_ring_b, .frames = 1000, .preroll = 1001 }));
    /* the arena: a stream holds it until released */
    CHECK(yau_stream_open(&g_au, &g_st2, &(yau_stream_desc){ .frames = 48000 }));
    CHECK_I(yau_arena_reset(&g_au), YAU_ERR_BUSY);
    yau_stream_close(&g_au, &g_st2);
    CHECK_I(yau_arena_reset(&g_au), 0);
    CHECK(!yau_stream_open(&g_au, &g_st2, &(yau_stream_desc){ .frames = (int64_t)sizeof g_arena }));
    CHECK(strstr(yau_error(&g_au), "arena") != NULL);
    /* the stream of another handle */
    {
        static yau_audio other;
        yau_play_desc pd;
        memset(&pd, 0, sizeof pd);
        pd.stream = &g_st;
        other.open = 1;   /* enough to reach the check: the stream is not its */
        other.voices_max = 4;
        CHECK_I(yau_play(&other, &pd), YAU_ERR_ARG);
        other.open = 0;
    }
    yau_stream_close(&g_au, &g_st);
    yau_close(&g_au);
}

/* v0.4.0: close never fails. A stream the device has let go of closes at
 * once; a playing one is canceled, and close waits for the device. Here the
 * device's blocks run on this thread, so nothing renders during the wait:
 * close gives up after the bound (virtual time), leaves the stream detached
 * with its memory and the arena hold in place, and the next block ends the
 * play. The frame thread's failures write yau_error(). */
static void test_stream_close(void) {
    sdev* s = &g_sd;
    int64_t next = 0, t0;
    yau_stream_info in;
    yau_stream z;
    sd_default(s);
    CHECK(open_dev(s, NULL));
    sd_run_s(s, 1.0);
    memset(&z, 0, sizeof z);
    yau_stream_close(&g_au, &z);                 /* zeroed, twice */
    yau_stream_close(&g_au, &z);
    CHECK(!yau_stream_open(&g_au, &g_st2, &(yau_stream_desc){ .memory = g_ring_b, .frames = 959 }));
    CHECK(strstr(yau_error(&g_au), "2 periods") != NULL);
    yau_stream_close(&g_au, &g_st2);             /* after a failed open */
    CHECK(yau_stream_open(&g_au, &g_st2, &(yau_stream_desc){ .frames = 48000 }));
    (void)feed(&g_st2, &next, 4000, 2);
    CHECK(yau_play_stream(&g_au, &g_st2, 0) > 0);
    sd_block(s);
    yau_stream_get_info(&g_st2, &in);
    CHECK(in.state == YAU_STREAM_WAITING || in.state == YAU_STREAM_PLAYING);   /* the device holds it */
    t0 = g_vt;
    yau_stream_close(&g_au, &g_st2);
    CHECK(g_vt - t0 >= 20000000 && g_vt - t0 <= 1001000000);   /* the bound */
    CHECK_I(yau_arena_reset(&g_au), YAU_ERR_BUSY);              /* hold kept */
    CHECK(strstr(yau_error(&g_au), "yau_arena_reset") != NULL);
    sd_block(s);                                                /* the cancel */
    yau_stream_get_info(&g_st2, &in);
    CHECK_I(in.state, YAU_STREAM_ENDED);
    yau_stream_close(&g_au, &g_st2);             /* detached: nothing more */
    /* frame-thread failures name the call */
    CHECK_I(yau_cancel(&g_au, -3), YAU_ERR_ARG);
    CHECK(strstr(yau_error(&g_au), "yau_cancel") != NULL);
    CHECK_I(yau_result(&g_au, 999999, NULL), YAU_ERR_NOT_FOUND);
    CHECK(strstr(yau_error(&g_au), "yau_result") != NULL);
    CHECK(yau_play_at(&g_au, yau_click(&g_au, &(yau_click_desc){ .peak = 0.5f }), -1) < 0);
    CHECK(strstr(yau_error(&g_au), "yau_play_at") != NULL);
    yau_close(&g_au);
    CHECK_I(yau_update(&g_au), YAU_ERR_CLOSED);
    CHECK(strstr(yau_error(&g_au), "yau_update") != NULL);
}

/* Underruns of the ring: silence exactly on the frames whose samples were
 * missing, every later sample on its planned frame, one GAP per run. */
static void test_stream_gaps(void) {
    sdev* s = &g_sd;
    yau_onset r = { 0 };
    yau_stream_info in;
    yau_id id;
    int64_t next = 0, t, b, origin;
    yrt_event ev[512];
    int n, i, ngap = 0, nstream = 0, nend = 0;
    int64_t gsum = 0;
    sd_default(s);
    s->tape = NULL;
    CHECK(open_dev(s, NULL));
    sd_run_s(s, 6.0);
    while (yrt_ring_drain(&g_ring, ev, 512) > 0) {}
    CHECK(yau_stream_open(&g_au, &g_st, &(yau_stream_desc){ .memory = g_ring_b, .frames = 4801, .id = 9 }));
    (void)feed(&g_st, &next, 4800, 2);
    chk_start(&g_st, 2);
    t = g_vt + 80000000;
    id = yau_play_stream(&g_au, &g_st, t);
    run_fed(s, &g_st, &next, 2, 1.0);
    yau_stream_get_info(&g_st, &in);
    origin = in.origin;
    /* three gaps: the producer stops for 5, 2 and 7 blocks after the ring
     * runs dry and then writes on from where it stopped, so the first
     * samples it writes have missed their frames */
    for (i = 0; i < 3; i++) {
        int stall = i == 0 ? 5 : i == 1 ? 2 : 7;
        int64_t before = g_chk.silent;
        for (b = 0; b < stall + 10; b++) sd_block(s);   /* 10 blocks drain the ring */
        run_fed(s, &g_st, &next, 2, 0.5);
        CHECK(g_chk.silent > before);
    }
    yau_stream_end(&g_st);
    run_fed(s, &g_st, &next, 2, 0.5);
    CHECK_I(yau_result(&g_au, id, &r), YAU_OK);
    yau_stream_get_info(&g_st, &in);
    printf("stream gaps: %u runs, %lld silent frames, %lld discarded, %lld wrong\n", (unsigned)in.gaps,
           (long long)in.gap_frames, (long long)in.discarded, (long long)g_chk.bad);
    CHECK_I(g_chk.bad, 0);
    CHECK_I(in.gaps, 3);
    CHECK_I(in.gap_frames, g_chk.silent);
    CHECK_I(r.gap_frames, g_chk.silent);
    CHECK_I(in.discarded, in.gap_frames);   /* every missing sample came, late */
    CHECK(r.flags & YAU_ONSET_GAP);
    CHECK(!(r.flags & YAU_ONSET_STOPPED));
    CHECK_I(r.end_frame, origin + next);
    CHECK_I(in.state, YAU_STREAM_ENDED);
    {
        yau_caps c;
        yau_get_caps(&g_au, &c);
        CHECK_I(c.gaps, 3);
    }
    while ((n = yrt_ring_drain(&g_ring, ev, 512)) > 0) {
        for (i = 0; i < n; i++) {
            const yrt_event* e = &ev[i];
            if (e->source != YRT_SRC_AUDIO || e->aux != (uint32_t)id) continue;
            if (e->kind == YAU_EV_GAP) {
                ngap++;
                gsum += e->u.i64[1];
                CHECK_I(e->u.i64[2], e->u.i64[0] - origin);
                CHECK_I(e->u.u32[8], 9);
                /* the fit's time when the gap closed; refits since move it a little */
                CHECK(llabs((int64_t)e->t_ns - yau__fit_time(&g_au.fit, e->u.i64[0])) < 2000);
                CHECK_I(e->u.u16[18], 0);
            }
            if (e->kind == YAU_EV_STREAM) {
                nstream++;
                CHECK_I(e->u.i64[0], origin);
                CHECK_I(e->u.i64[1], r.start_frame);
                CHECK_I(e->u.i64[2], 0);
                CHECK_I(e->u.i64[3], 0);
                CHECK_I(e->u.u32[8], 9);
            }
            if (e->kind == YAU_EV_END) {
                nend++;
                CHECK_I(e->u.i64[2], in.gap_frames);
                CHECK_I(e->u.i64[3], next);
                CHECK_I(e->u.i64[1], next - in.gap_frames);
            }
        }
    }
    CHECK_I(ngap, 3);
    CHECK_I(nstream, 1);
    CHECK_I(nend, 1);
    CHECK_I(gsum, in.gap_frames);
    s->on_block = NULL;

    /* a gap at the start: an empty ring at the start frame */
    CHECK_I(yau_stream_reset(&g_st, 0), 0);
    next = 0;
    chk_start(&g_st, 2);
    t = g_vt + 80000000;
    id = yau_play_stream(&g_au, &g_st, t);
    sd_run_s(s, 0.15);
    run_fed(s, &g_st, &next, 2, 0.3);
    CHECK_I(yau_result(&g_au, id, &r), YAU_OK);
    yau_stream_get_info(&g_st, &in);
    CHECK(r.flags & YAU_ONSET_GAP);
    if (!near_tie(s, t)) CHECK_I(r.start_frame, sd_frame_of(s, t));
    CHECK_I(r.sample, 0);
    CHECK_I(r.tier, YAU_TIER_2);
    CHECK(in.gaps == 1 && in.gap_frames > 0);
    CHECK_I(g_chk.bad, 0);
    CHECK(yau_stop_at(&g_au, id, 0, 0) == 0);
    sd_run_s(s, 0.1);
    s->on_block = NULL;
    yau_close(&g_au);
}

/* A late start skips the samples it missed: origin from the plan. At 0 it
 * waits for the preroll, or not with YAU_PREROLL_NONE. */
static void test_stream_late_and_asap(void) {
    sdev* s = &g_sd;
    yau_onset r = { 0 };
    yau_stream_info in;
    yau_id id;
    int64_t next = 0, t, plan;
    yrt_event ev[512];
    int n, i, seen = 0;
    sd_default(s);
    s->tape = NULL;
    s->jitter_ns = 0;
    CHECK(open_dev(s, NULL));
    sd_run_s(s, 6.0);
    CHECK(yau_stream_open(&g_au, &g_st, &(yau_stream_desc){ .memory = g_ring_b, .frames = 9600 }));
    (void)feed(&g_st, &next, INT64_MAX, 2);
    chk_start(&g_st, 2);
    t = g_vt - 30000000;
    plan = yau__fit_frame(&g_au.fit, t);
    while (yrt_ring_drain(&g_ring, ev, 512) > 0) {}
    id = yau_play_stream(&g_au, &g_st, t);
    run_fed(s, &g_st, &next, 2, 0.3);
    CHECK_I(yau_result(&g_au, id, &r), YAU_OK);
    yau_stream_get_info(&g_st, &in);
    CHECK(r.flags & YAU_ONSET_LATE);
    CHECK(!(r.flags & YAU_ONSET_GAP));
    CHECK(llabs(in.origin - plan) <= 1);
    CHECK_I(r.sample, r.start_frame - in.origin);
    CHECK(r.sample > 1400);   /* 30 ms and more of samples skipped */
    CHECK_I(in.discarded, r.sample);
    CHECK_I(g_chk.bad, 0);
    while ((n = yrt_ring_drain(&g_ring, ev, 512)) > 0)
        for (i = 0; i < n; i++)
            if (ev[i].source == YRT_SRC_AUDIO && ev[i].kind == YAU_EV_STREAM) {
                seen++;
                CHECK_I(ev[i].u.i64[3], r.sample);
                CHECK(ev[i].u.u16[18] & YAU_ONSET_LATE);
            }
    CHECK_I(seen, 1);
    CHECK(yau_stop_at(&g_au, id, 0, 0) == 0);
    run_fed(s, &g_st, &next, 2, 0.1);

    /* late with too little data: LATE and GAP */
    CHECK_I(yau_stream_reset(&g_st, 0), 0);
    next = 0;
    (void)feed(&g_st, &next, 100, 2);
    chk_start(&g_st, 2);
    id = yau_play_stream(&g_au, &g_st, g_vt - 30000000);
    sd_run_s(s, 0.1);
    run_fed(s, &g_st, &next, 2, 0.2);
    CHECK_I(yau_result(&g_au, id, &r), YAU_OK);
    CHECK((r.flags & YAU_ONSET_LATE) && (r.flags & YAU_ONSET_GAP));
    CHECK_I(g_chk.bad, 0);
    CHECK(yau_stop_at(&g_au, id, 0, 0) == 0);
    sd_run_s(s, 0.1);

    /* at 0: waits for the preroll (a quarter ring, 2400 frames) */
    CHECK_I(yau_stream_reset(&g_st, 0), 0);
    next = 0;
    chk_start(&g_st, 2);
    id = yau_play_stream(&g_au, &g_st, 0);
    sd_run(s, 3);
    yau_stream_get_info(&g_st, &in);
    CHECK(!in.started && in.state == YAU_STREAM_WAITING && !in.ready);
    (void)feed(&g_st, &next, 2399, 2);
    sd_run(s, 2);
    yau_stream_get_info(&g_st, &in);
    CHECK(!in.started);
    (void)feed(&g_st, &next, 1, 2);
    yau_stream_get_info(&g_st, &in);
    CHECK(in.ready);
    {
        int64_t w_next = g_au.w;
        sd_block(s);
        yau_stream_get_info(&g_st, &in);
        CHECK(in.started);
        CHECK_I(in.start_frame, w_next);
        CHECK_I(in.origin, w_next);
    }
    run_fed(s, &g_st, &next, 2, 0.3);
    CHECK_I(yau_result(&g_au, id, &r), YAU_OK);
    CHECK(!(r.flags & (YAU_ONSET_GAP | YAU_ONSET_LATE)));
    CHECK_I(r.target, 0);
    CHECK_I(g_chk.bad, 0);
    CHECK(yau_stop_at(&g_au, id, 0, 0) == 0);
    sd_run_s(s, 0.1);

    /* at 0 with fewer frames than the preroll and the end in: starts */
    CHECK_I(yau_stream_reset(&g_st, 0), 0);
    next = 0;
    (void)feed(&g_st, &next, 300, 2);
    yau_stream_end(&g_st);
    id = yau_play_stream(&g_au, &g_st, 0);
    sd_run_s(s, 0.2);
    CHECK_I(yau_result(&g_au, id, &r), YAU_OK);
    CHECK_I(r.end_frame - r.start_frame, 300);

    /* YAU_PREROLL_NONE: at once, a gap when empty */
    CHECK(yau_stream_open(&g_au, &g_st, &(yau_stream_desc){ .memory = g_ring_b, .frames = 9600, .preroll = YAU_PREROLL_NONE }));
    id = yau_play_stream(&g_au, &g_st, 0);
    sd_run(s, 2);
    yau_stream_get_info(&g_st, &in);
    CHECK(in.started && in.gaps == 1);
    CHECK(yau_stop_at(&g_au, id, 0, 0) == 0);
    sd_run_s(s, 0.1);
    /* a cancel of an at-0 stream still waiting for data: CANCELED */
    CHECK(yau_stream_open(&g_au, &g_st, &(yau_stream_desc){ .memory = g_ring_b, .frames = 9600 }));
    id = yau_play_stream(&g_au, &g_st, 0);
    sd_run(s, 2);
    CHECK(yau_cancel(&g_au, id) == 0);
    sd_run_s(s, 0.1);
    CHECK_I(yau_result(&g_au, id, &r), YAU_OK);
    CHECK((r.flags & YAU_ONSET_CANCELED) && r.start_frame == -1 && r.sample == -1);
    yau_stream_get_info(&g_st, &in);
    CHECK_I(in.state, YAU_STREAM_ENDED);
    s->on_block = NULL;
    yau_close(&g_au);
}

/* Voices and streams in one run: the tape is the model sum, bit for bit.
 * Then a stream and a buffer with the same samples, targets, gain change
 * and stop fade on the two channels give the same values: one kernel. */
static void test_stream_mix(void) {
    sdev* s = &g_sd;
    yau_play_desc pd;
    yau_onset r = { 0 }, rb = { 0 }, rs = { 0 };
    yau_stream_info in;
    yau_id ida, idb, ids, idv1;
    int64_t i, f, t, next = 0, n2 = 0, bad = 0;
    sd_default(s);
    s->jitter_ns = 0;
    CHECK(open_dev(s, NULL));
    sd_run_s(s, 1.0);
    /* buffer samples are multiples of 2^-13 too, so every sum is exact */
    for (i = 0; i < 48000; i++) g_buf[i] = (float)((i % 511) + 1) / 8192.0f;
    CHECK(yau_stream_open(&g_au, &g_st, &(yau_stream_desc){ .memory = g_ring_b, .frames = 9600, .channels = 1 }));
    CHECK(yau_stream_open(&g_au, &g_st2, &(yau_stream_desc){ .frames = 9600 }));
    (void)feed(&g_st, &next, INT64_MAX, 1);
    (void)feed(&g_st2, &n2, INT64_MAX, 2);
    t = g_vt + 100000000;
    memset(&pd, 0, sizeof pd);
    pd.stream = &g_st; pd.at_ns = t; pd.channels = 2;              /* mono stream, right only */
    ida = yau_play(&g_au, &pd);
    pd.stream = &g_st2; pd.at_ns = t + 1000000; pd.channels = 0;   /* stereo stream */
    idb = yau_play(&g_au, &pd);
    memset(&pd, 0, sizeof pd);
    pd.buf.frames = g_buf; pd.buf.n = 3000; pd.buf.channels = 1; pd.at_ns = t + 2000000; pd.channels = 1;
    idv1 = yau_play(&g_au, &pd);
    sd_run_s(s, 0.25);
    CHECK_I(yau_result(&g_au, ida, &r), YAU_OK);
    CHECK_I(yau_result(&g_au, idb, &rb), YAU_OK);
    CHECK_I(yau_result(&g_au, idv1, &rs), YAU_OK);
    for (f = r.start_frame - 5; f < r.start_frame + 6000; f++) {
        float L = 0, R = 0;
        if (f >= r.start_frame) R += idv(f - r.start_frame, 0);
        if (f >= rb.start_frame) { L += idv(f - rb.start_frame, 0); R += idv(f - rb.start_frame, 1); }
        if (f >= rs.start_frame && f < rs.start_frame + 3000) L += g_buf[f - rs.start_frame];
        if (g_tape[f * 2] != L || g_tape[f * 2 + 1] != R) bad++;
    }
    CHECK_I(bad, 0);
    yau_stop_at(&g_au, 0, 0, 0);
    sd_run_s(s, 0.2);

    /* the same sound as a buffer on the left and a stream on the right */
    for (i = 0; i < 48000; i++) g_buf[i] = 0.5f - (float)(i % 97) / 1000.0f;
    CHECK_I(yau_stream_reset(&g_st, 0), 0);
    CHECK_I(yau_stream_write(&g_st, g_buf, 9600), 9600);
    t = g_vt + 100000000;
    memset(&pd, 0, sizeof pd);
    pd.buf.frames = g_buf; pd.buf.n = 9600; pd.buf.channels = 1; pd.at_ns = t; pd.channels = 1; pd.db = -2.0f;
    idv1 = yau_play(&g_au, &pd);
    memset(&pd, 0, sizeof pd);
    pd.stream = &g_st; pd.at_ns = t; pd.channels = 2; pd.db = -2.0f;
    ids = yau_play(&g_au, &pd);
    CHECK(yau_gain_at(&g_au, idv1, t + 30000000, -9.0f, YRT_MS(4)) == 0);
    CHECK(yau_gain_at(&g_au, ids, t + 30000000, -9.0f, YRT_MS(4)) == 0);
    CHECK(yau_stop_at(&g_au, idv1, t + 120000000, YRT_MS(5)) == 0);
    CHECK(yau_stop_at(&g_au, ids, t + 120000000, YRT_MS(5)) == 0);
    sd_run_s(s, 0.4);
    CHECK_I(yau_result(&g_au, idv1, &rb), YAU_OK);
    CHECK_I(yau_result(&g_au, ids, &rs), YAU_OK);
    CHECK_I(rb.start_frame, rs.start_frame);
    CHECK_I(rb.end_frame, rs.end_frame);
    CHECK((rs.flags & YAU_ONSET_STOPPED) && (rb.flags & YAU_ONSET_STOPPED));
    bad = 0;
    for (f = rb.start_frame - 5; f < rb.end_frame + 5; f++) if (g_tape[f * 2] != g_tape[f * 2 + 1]) bad++;
    CHECK_I(bad, 0);
    CHECK(g_tape[(rb.end_frame - 120) * 2] > 0.0f && g_tape[rb.end_frame * 2] == 0.0f);
    CHECK(g_tape[(rb.start_frame + 2400) * 2] < g_tape[(rb.start_frame + 100) * 2] * 0.5f);   /* -7 dB */
    yau_stream_get_info(&g_st, &in);
    CHECK_I(in.state, YAU_STREAM_ENDED);
    /* clipping flags every contributor, the stream included */
    CHECK_I(yau_stream_reset(&g_st, 0), 0);
    for (i = 0; i < 4800; i++) g_buf[i] = 0.7f;
    CHECK_I(yau_stream_write(&g_st, g_buf, 4800), 4800);
    t = g_vt + 100000000;
    ids = yau_play_stream(&g_au, &g_st, t);
    memset(&pd, 0, sizeof pd);
    pd.buf.frames = g_buf; pd.buf.n = 4800; pd.buf.channels = 1; pd.at_ns = t;
    idv1 = yau_play(&g_au, &pd);
    sd_run_s(s, 0.3);
    CHECK_I(yau_result(&g_au, ids, &rs), YAU_OK);
    CHECK(rs.flags & YAU_ONSET_CLIPPED);
    yau_stream_close(&g_au, &g_st2);
    yau_close(&g_au);
}

/* Device underruns while a stream plays; a device with callback times only;
 * close in the middle of a stream. */
static void test_stream_device(void) {
    sdev* s = &g_sd;
    yau_onset r = { 0 };
    yau_stream_info in;
    yau_id id;
    int64_t next = 0;
    int halts;
    for (halts = 0; halts < 2; halts++) {
        sd_default(s);
        s->tape = NULL;
        s->stall_halts = halts;
        CHECK(open_dev(s, NULL));
        sd_run_s(s, 6.0);
        CHECK(yau_stream_open(&g_au, &g_st, &(yau_stream_desc){ .memory = g_ring_b, .frames = 48000 }));
        next = 0;
        (void)feed(&g_st, &next, INT64_MAX, 2);
        chk_start(&g_st, 2);
        /* rendered, not yet confirmed when the device stalls */
        id = yau_play_stream(&g_au, &g_st, g_vt + 80000000);
        run_fed(s, &g_st, &next, 2, 0.06);
        s->stall_ns = 80000000;
        run_fed(s, &g_st, &next, 2, 1.0);
        CHECK_I(yau_result(&g_au, id, &r), YAU_OK);
        CHECK(r.flags & YAU_ONSET_XRUN);
        CHECK(r.flags & YAU_ONSET_UNCONFIRMED);
        yau_stream_get_info(&g_st, &in);
        CHECK_I(in.gaps, 0);
        CHECK_I(g_chk.bad, 0);    /* in stream frames, the identity holds */
        s->on_block = NULL;
        yau_close(&g_au);
    }
    /* callback times only: unconfirmed, tier 3 */
    sd_default(s);
    s->tape = NULL;
    s->pos_source = YAU_POS_CALLBACK;
    s->tier = 0;
    CHECK(open_dev(s, NULL));
    sd_run_s(s, 3.0);
    CHECK(yau_stream_open(&g_au, &g_st, &(yau_stream_desc){ .memory = g_ring_b, .frames = 48000 }));
    next = 0;
    (void)feed(&g_st, &next, 4800, 2);
    yau_stream_end(&g_st);
    id = yau_play_stream(&g_au, &g_st, g_vt + 80000000);
    sd_run_s(s, 0.3);
    CHECK_I(yau_result(&g_au, id, &r), YAU_OK);
    CHECK((r.flags & YAU_ONSET_UNCONFIRMED) && r.tier == YAU_TIER_3 && r.device_pos == -1);
    CHECK_I(r.end_frame - r.start_frame, 4800);
    /* close mid-stream: the stream ENDED, its position kept */
    CHECK_I(yau_stream_reset(&g_st, 0), 0);
    next = 0;
    (void)feed(&g_st, &next, 48000, 2);
    id = yau_play_stream(&g_au, &g_st, 0);
    sd_run_s(s, 0.2);
    yau_close(&g_au);
    yau_stream_get_info(&g_st, &in);
    CHECK_I(in.state, YAU_STREAM_ENDED);
    CHECK(in.next > 0 && in.next < 48000);
}

/* Many gaps in one play go to the event ring and the counters only: the
 * queue that carries ONSET and END loses nothing. */
static void test_stream_gap_storm(void) {
    sdev* s = &g_sd;
    yau_onset r = { 0 };
    yau_stream_info in;
    yau_id id;
    int64_t next = 0;
    int k;
    sd_default(s);
    s->tape = NULL;
    CHECK(open_dev(s, NULL));
    sd_run_s(s, 2.0);
    CHECK(yau_stream_open(&g_au, &g_st, &(yau_stream_desc){ .memory = g_ring_b, .frames = 960, .preroll = YAU_PREROLL_NONE }));
    id = yau_play_stream(&g_au, &g_st, 0);
    /* every third block the producer writes nothing, then twice a block:
     * one gap of one block each time, and its samples come late */
    for (k = 0; k < 900; k++) {
        (void)feed(&g_st, &next, k % 3 == 2 ? 0 : k % 3 == 0 && k > 0 ? 960 : 480, 2);
        sd_block(s);
    }
    yau_stream_end(&g_st);
    sd_run_s(s, 0.2);
    CHECK_I(yau_result(&g_au, id, &r), YAU_OK);
    yau_stream_get_info(&g_st, &in);
    printf("stream gap storm: %u gaps, %u messages dropped\n", (unsigned)in.gaps, (unsigned)g_au.msgs_dropped);
    CHECK_I(in.gaps, 300);
    CHECK_I(in.gap_frames, 300 * 480);
    CHECK_I(g_au.msgs_dropped, 0);
    CHECK(r.end_frame > 0);
    CHECK_I(r.gap_frames, in.gap_frames);
    yau_close(&g_au);
    CHECK_I(g_au.outstanding, 0);
}

/* The producer on a real thread, racing the device thread: identity
 * samples in random chunks with random stalls that cause gaps, while the
 * frame thread plays, stops and replays the stream 40 times and the
 * producer resets it between plays. Checked after the run from the tape
 * and each play's record. ThreadSanitizer checks the ring and the state. */
static uint32_t g_prod_stop = 0, g_prod_reset = 0;
static uint64_t g_prod_rng = 99;
static double prod_rand(void) {
    g_prod_rng = g_prod_rng * 6364136223846793005ull + 1442695040888963407ull;
    return ((double)(g_prod_rng >> 11) + 0.5) / 9007199254740992.0;
}
#if defined(_WIN32)
static unsigned __stdcall prod_main(void* p)
#else
static void* prod_main(void* p)
#endif
{
    yau_stream* st = (yau_stream*)p;
    int64_t next = 0;
    while (!yau__ld32(&g_prod_stop)) {
        if (yau__ld32(&g_prod_reset)) {
            if (yau_stream_reset(st, 0) == 0) { next = 0; yau__st32(&g_prod_reset, 0); }
            yrt_sleep_until(yrt_now_ns() + 200000, 0);
            continue;
        }
        (void)feed(st, &next, 1 + (int64_t)(prod_rand() * 3000.0), 2);
        if (prod_rand() < 0.1) yrt_sleep_until(yrt_now_ns() + (int64_t)(prod_rand() * 40e6), 0);
        else yrt_sleep_until(yrt_now_ns() + 2000000, 0);
    }
#if defined(_WIN32)
    return 0;
#else
    return NULL;
#endif
}

static void test_stream_threads(void) {
    sdev* s = &g_sd;
    yau_onset recs[40];
    int i, n = 40, done = 0;
    int64_t bad = 0, frames = 0, silent_total = 0, gaps_total = 0;
    sd_default(s);
    s->period = 48;
    s->L = 96;
    g_virtual = 0;
    CHECK(open_dev(s, NULL));
    CHECK(yau_stream_open(&g_au, &g_st, &(yau_stream_desc){ .memory = g_ring_b, .frames = 960, .preroll = 480 }));
    yau__st32(&g_thr_stop, 0);
    yau__st32(&g_prod_stop, 0);
    yau__st32(&g_prod_reset, 0);
    {
#if defined(_WIN32)
        HANDLE th = (HANDLE)_beginthreadex(NULL, 0, thr_main, s, 0, NULL);
        HANDLE ph = (HANDLE)_beginthreadex(NULL, 0, prod_main, &g_st, 0, NULL);
#else
        pthread_t th, ph;
        pthread_create(&th, NULL, thr_main, s);
        pthread_create(&ph, NULL, prod_main, &g_st);
#endif
        for (i = 0; i < n; i++) {
            yau_stream_info in;
            yau_id id;
            int k, rc = YAU_PENDING;
            memset(&recs[i], 0, sizeof recs[i]);
            for (k = 0; k < 2000; k++) {
                yau_stream_get_info(&g_st, &in);
                if (in.state == YAU_STREAM_IDLE && !yau__ld32(&g_prod_reset)) break;
                yrt_sleep_until(yrt_now_ns() + 1000000, 0);
            }
            id = yau_play_stream(&g_au, &g_st, 0);
            if (id <= 0) { fail_i(__LINE__, "stream play refused", id, 1); break; }
            yrt_sleep_until(yrt_now_ns() + 20000000 + (int64_t)(sd_rand(s) * 30e6), 0);
            CHECK(yau_stop_at(&g_au, id, 0, 0) == 0);
            for (k = 0; k < 2000; k++) {
                rc = yau_result(&g_au, id, &recs[i]);
                if (rc == YAU_OK && (recs[i].end_frame > 0 || (recs[i].flags & YAU_ONSET_CANCELED))) break;
                yrt_sleep_until(yrt_now_ns() + 1000000, 0);
            }
            CHECK_I(rc, YAU_OK);
            if (rc == YAU_OK) done++;
            yau__st32(&g_prod_reset, 1);
        }
        yau__st32(&g_prod_stop, 1);
        yau__st32(&g_thr_stop, 1);
#if defined(_WIN32)
        WaitForSingleObject(ph, INFINITE); CloseHandle(ph);
        WaitForSingleObject(th, INFINITE); CloseHandle(th);
#else
        pthread_join(ph, NULL);
        pthread_join(th, NULL);
#endif
    }
    /* every frame: zero outside the plays; inside one, its identity or a
     * gap; the gaps add up to each record's count */
    {
        int64_t f, lim = s->W < TAPE_FRAMES ? s->W : TAPE_FRAMES;
        int p = 0;
        for (f = 0; f < lim; f++) {
            float a = g_tape[f * 2], b = g_tape[f * 2 + 1];
            while (p < done && (recs[p].start_frame < 0 || f >= recs[p].end_frame)) p++;
            if (p < done && f >= recs[p].start_frame) {
                int64_t smp = f - (recs[p].start_frame - recs[p].sample);
                if (a == 0.0f && b == 0.0f) silent_total++;
                else if (a != idv(smp, 0) || b != idv(smp, 1)) bad++;
                frames++;
            } else if (a != 0.0f || b != 0.0f) {
                bad++;
            }
        }
        for (i = 0; i < done; i++) gaps_total += recs[i].gap_frames;
    }
    printf("stream threads: %d plays, %lld frames, %lld silent, %lld wrong\n", done, (long long)frames,
           (long long)silent_total, (long long)bad);
    CHECK_I(done, n);
    CHECK_I(bad, 0);
    CHECK_I(silent_total, gaps_total);
    CHECK(frames > 0);
    yau_close(&g_au);
    g_virtual = 1;
}

/* ------------------------------------------------------------------- WAV */

/* WAV files built in memory. The values are the identity values with a
 * sign (so the 24-bit sign extension is checked), each exact in 16 bits,
 * in 24 bits and in float. */
enum { WK_S16, WK_S24, WK_S24_32, WK_F32, WK_F32X, WK_RF64, WK_BW64, WK_N };
static const char* g_wk_name[WK_N] = { "s16", "s24", "s24in32", "f32", "f32 extensible", "rf64 s16", "bw64 s24" };
static unsigned char g_wavmem[1 << 20];
static size_t g_fmt_off, g_data_off;
static int64_t g_wav_A = 1;
static float wv(int64_t s, int c) { float v = idv(s, c); return s % 3 == 0 ? -v : v; }
static float wv_loop(int64_t s, int c) { return wv(s % g_wav_A, c); }
static void put16(unsigned char* p, uint32_t v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void put32(unsigned char* p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }
static void put64(unsigned char* p, uint64_t v) { put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }

static size_t make_wav(int kind, int ch, int64_t frames, uint32_t rate, int64_t nan_at) {
    unsigned char* p = g_wavmem;
    int bytes = kind == WK_S16 || kind == WK_RF64 ? 2 : kind == WK_S24 || kind == WK_BW64 ? 3 : 4;
    int ext = kind == WK_S24_32 || kind == WK_F32X;
    int isf = kind == WK_F32 || kind == WK_F32X;
    int big = kind == WK_RF64 || kind == WK_BW64;
    size_t o = 12, data_bytes = (size_t)frames * (size_t)ch * (size_t)bytes;
    int64_t i;
    int c;
    memcpy(p, kind == WK_RF64 ? "RF64" : kind == WK_BW64 ? "BW64" : "RIFF", 4);
    memcpy(p + 8, "WAVE", 4);
    if (big) {
        memcpy(p + o, "ds64", 4); put32(p + o + 4, 28);
        put64(p + o + 16, (uint64_t)data_bytes); put64(p + o + 24, (uint64_t)frames); put32(p + o + 32, 0);
        o += 36;
    }
    g_fmt_off = o;
    memcpy(p + o, "fmt ", 4); put32(p + o + 4, ext ? 40 : 16);
    put16(p + o + 8, ext ? 0xFFFE : isf ? 3 : 1);
    put16(p + o + 10, (uint32_t)ch);
    put32(p + o + 12, rate);
    put32(p + o + 16, rate * (uint32_t)(ch * bytes));
    put16(p + o + 20, (uint32_t)(ch * bytes));
    put16(p + o + 22, (uint32_t)(bytes * 8));
    if (ext) {
        static const unsigned char tail[14] = { 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 };
        put16(p + o + 24, 22);
        put16(p + o + 26, isf ? 32 : 24);
        put32(p + o + 28, ch == 2 ? 3u : 4u);
        put16(p + o + 32, isf ? 3 : 1);
        memcpy(p + o + 34, tail, 14);
    }
    o += ext ? 48 : 24;
    if (kind == WK_BW64) {   /* an odd chunk: its pad byte must be skipped */
        memcpy(p + o, "LIST", 4); put32(p + o + 4, 5); memset(p + o + 8, 'x', 6);
        o += 14;
    }
    memcpy(p + o, "data", 4); put32(p + o + 4, big ? 0xFFFFFFFFu : (uint32_t)data_bytes);
    o += 8;
    g_data_off = o;
    for (i = 0; i < frames; i++) {
        for (c = 0; c < ch; c++) {
            unsigned char* q = p + o + (size_t)((i * ch + c) * bytes);
            float v = wv(i, c);
            if (bytes == 2) put16(q, (uint32_t)(int32_t)(v * 32768.0f));
            else if (bytes == 3) { uint32_t u = (uint32_t)(int32_t)(v * 8388608.0f); q[0] = (unsigned char)u; q[1] = (unsigned char)(u >> 8); q[2] = (unsigned char)(u >> 16); }
            else if (!isf) put32(q, (uint32_t)(int32_t)(v * 8388608.0f) << 8);
            else {
                uint32_t u;
                if (i == nan_at) v = (float)NAN;
                memcpy(&u, &v, 4);
                put32(q, u);
            }
        }
    }
    o += data_bytes;
    put32(p + 4, big ? 0xFFFFFFFFu : (uint32_t)(o - 8));
    if (big) put64(p + 20, (uint64_t)(o - 8));
    return o;
}

/* A byte-range reader over g_wavmem, as a pack entry would be. */
static int64_t mem_read(void* ctx, int64_t off, void* dst, int64_t n) {
    (void)ctx;
    memcpy(dst, g_wavmem + off, (size_t)n);
    return n;
}

static yau_wav g_wav, g_wav2;
static float g_ring_c[48000 * 2];


static void test_wav(void) {
    sdev* s = &g_sd;
    yau_onset r = { 0 };
    yau_stream_info in;
    yau_id id;
    int k;
    size_t n;
    sd_default(s);
    s->tape = NULL;
    CHECK(open_dev(s, NULL));
    sd_run_s(s, 3.0);
    /* every form: parsed, streamed, played, each sample exact */
    for (k = 0; k < WK_N; k++) {
        int ch = k == WK_S24 ? 1 : 2;
        n = make_wav(k, ch, 3000, 48000, -1);
        CHECK(yau_wav_open(&g_au, &g_wav, &(yau_wav_desc){ .data = g_wavmem, .size = n, .ring = 1000, .memory = g_ring_c, .id = 70 + (uint32_t)k }));
        if (!g_wav.open_) { fail(__LINE__, yau_wav_error(&g_wav)); continue; }
        CHECK_I(g_wav.info.frames, 3000);
        CHECK_I(g_wav.info.channels, ch);
        CHECK_I(g_wav.info.rf64, k == WK_RF64 || k == WK_BW64);
        CHECK_I(g_wav.info.data_offset, (int64_t)g_data_off);
        CHECK_I(yau_wav_feed(&g_wav, 0), 1000);
        g_wav_A = 3000;
        chk_start(&g_wav.stream, ch);
        chk_val(wv_loop);
        id = yau_play_stream(&g_au, &g_wav.stream, g_vt + 80000000);
        {
            int b;
            for (b = 0; b < 60; b++) { (void)yau_wav_feed(&g_wav, 0); sd_block(s); }
        }
        CHECK_I(yau_result(&g_au, id, &r), YAU_OK);
        CHECK_I(r.end_frame - r.start_frame, 3000);
        CHECK(!(r.flags & (YAU_ONSET_GAP | YAU_ONSET_STOPPED)));
        CHECK_I(r.buffer_id, 70 + k);
        CHECK_I(g_chk.data, 3000);
        if (g_chk.bad) printf("wav %s: %lld wrong from frame %lld\n", g_wk_name[k], (long long)g_chk.bad, (long long)g_chk.first_bad);
        CHECK_I(g_chk.bad, 0);
        /* the whole file into the arena: the same values */
        {
            yau_buf b = yau_wav_load(&g_au, &(yau_wav_desc){ .data = g_wavmem, .size = n, .id = 5 });
            int64_t i, wrong = 0;
            CHECK(b.frames && b.n == 3000 && b.channels == ch && b.id == 5);
            if (b.frames) for (i = 0; i < 3000 * ch; i++) if (b.frames[i] != wv(i / ch, (int)(i % ch))) wrong++;
            CHECK_I(wrong, 0);
            CHECK_I(yau_arena_reset(&g_au), 0);
        }
        yau_wav_close(&g_au, &g_wav);
    }
    s->on_block = NULL;

    /* loops over a ring smaller than the file; seek after the end; a seek
     * through on_msg while playing waits for the end */
    n = make_wav(WK_S16, 2, 3000, 48000, -1);
    CHECK(yau_wav_open(&g_au, &g_wav, &(yau_wav_desc){ .data = g_wavmem, .size = n, .loops = 2, .ring = 1000, .memory = g_ring_c }));
    (void)yau_wav_feed(&g_wav, 0);
    chk_start(&g_wav.stream, 2);
    chk_val(wv_loop);
    id = yau_play_stream(&g_au, &g_wav.stream, 0);
    {
        int b;
        for (b = 0; b < 40; b++) { (void)yau_wav_feed(&g_wav, 0); sd_block(s); }
    }
    CHECK_I(yau_result(&g_au, id, &r), YAU_OK);
    CHECK_I(r.end_frame - r.start_frame, 9000);
    CHECK_I(g_chk.data, 9000);
    CHECK_I(g_chk.bad, 0);
    CHECK_I(yau_wav_seek(&g_wav, 9000), YAU_ERR_ARG);
    CHECK_I(yau_wav_seek(&g_wav, 4567), 0);
    (void)yau_wav_feed(&g_wav, 0);
    chk_start(&g_wav.stream, 2);
    chk_val(wv_loop);
    id = yau_play_stream(&g_au, &g_wav.stream, 0);
    sd_run(s, 3);
    CHECK_I(yau_wav_seek(&g_wav, 100), YAU_ERR_BUSY);
    {
        yau_wav_msg m;
        m.seek = 100;
        yau_wav_on_msg(&g_wav, &m, 1);
        CHECK(yau_wav_wants(&g_wav));
        CHECK_I(yau_wav_feed(&g_wav, 0), 0);   /* the play has not ended */
    }
    CHECK(yau_stop_at(&g_au, id, 0, YRT_MS(2)) == 0);
    sd_run(s, 10);
    CHECK_I(yau_result(&g_au, id, &r), YAU_OK);
    CHECK_I(r.sample, 4567);
    CHECK_I(g_chk.bad, 0);
    (void)yau_wav_step(&g_wav);   /* applies the seek, feeds a block */
    yau_stream_get_info(&g_wav.stream, &in);
    CHECK(in.state == YAU_STREAM_IDLE && in.first == 100 && in.fill > 0);
    chk_start(&g_wav.stream, 2);
    chk_val(wv_loop);
    while (yau_wav_step(&g_wav)) {}
    id = yau_play_stream(&g_au, &g_wav.stream, 0);
    {
        int b;
        for (b = 0; b < 20; b++) { (void)yau_wav_step(&g_wav); sd_block(s); }
    }
    CHECK_I(yau_result(&g_au, id, &r), YAU_OK);
    CHECK_I(r.sample, 100);
    CHECK_I(g_chk.bad, 0);
    CHECK(yau_stop_at(&g_au, id, 0, 0) == 0);
    sd_run(s, 3);
    yau_wav_close(&g_au, &g_wav);
    s->on_block = NULL;

    /* a path and a reader give the same samples as memory */
    {
        static float ring_a[4000 * 2], ring_b[4000 * 2], ring_d[4000 * 2];
        static yau_reader rd = { mem_read, 0 };
        const char* path = "ysp_audio_test_tmp.wav";
        FILE* f;
        n = make_wav(WK_S24_32, 2, 3000, 48000, -1);
        f = fopen(path, "wb");
        CHECK(f != NULL);
        if (f) { fwrite(g_wavmem, 1, n, f); fclose(f); }
        rd.size = (int64_t)n;
        CHECK(yau_wav_open(&g_au, &g_wav, &(yau_wav_desc){ .path = path, .ring = 4000, .memory = ring_a }));
        CHECK(yau_wav_open(&g_au, &g_wav2, &(yau_wav_desc){ .reader = &rd, .ring = 4000, .memory = ring_b }));
        CHECK_I(yau_wav_feed(&g_wav, 0), 3000);
        CHECK_I(yau_wav_feed(&g_wav2, 0), 3000);
        CHECK(memcmp(ring_a, ring_b, sizeof(float) * 6000) == 0);
        yau_wav_close(&g_au, &g_wav2);
        CHECK(yau_wav_open(&g_au, &g_wav2, &(yau_wav_desc){ .data = g_wavmem, .size = n, .ring = 4000, .memory = ring_d }));
        CHECK_I(yau_wav_feed(&g_wav2, 0), 3000);
        CHECK(memcmp(ring_a, ring_d, sizeof(float) * 6000) == 0);
        /* v0.2.1: the probe gives the open's info from a path, a reader
         * and memory, with no device */
        {
            yau_wav_info pi;
            char pe[256];
            CHECK_I(yau_wav_probe(&(yau_wav_desc){ .path = path }, &pi, pe, sizeof pe), 0);
            CHECK(memcmp(&pi, &g_wav.info, sizeof pi) == 0);
            CHECK_I(yau_wav_probe(&(yau_wav_desc){ .reader = &rd }, &pi, pe, sizeof pe), 0);
            CHECK(memcmp(&pi, &g_wav.info, sizeof pi) == 0);
            CHECK_I(yau_wav_probe(&(yau_wav_desc){ .data = g_wavmem, .size = n }, &pi, NULL, 0), 0);
            CHECK(pi.rate == 48000 && pi.channels == 2 && pi.frames == 3000 && pi.format == YAU_WAV_S24_32);
            CHECK_I(yau_wav_probe(&(yau_wav_desc){ .path = "no_such_file.wav" }, &pi, pe, sizeof pe), YAU_ERR_IO);
            CHECK_I(yau_wav_probe(NULL, &pi, pe, sizeof pe), YAU_ERR_ARG);
        }
        yau_wav_close(&g_au, &g_wav);
        yau_wav_close(&g_au, &g_wav2);
        CHECK(remove(path) == 0);   /* every reader closed the file (on Windows an open file cannot be removed) */
        CHECK(!yau_wav_open(&g_au, &g_wav, &(yau_wav_desc){ .path = "no_such_file.wav" }));
        CHECK_I(g_wav.error_code, YAU_ERR_IO);
    }

    /* a float that is not a number ends the stream before it */
    n = make_wav(WK_F32, 2, 3000, 48000, 1234);
    CHECK(yau_wav_open(&g_au, &g_wav, &(yau_wav_desc){ .data = g_wavmem, .size = n, .ring = 4000, .memory = g_ring_c }));
    CHECK_I(yau_wav_feed(&g_wav, 0), YAU_ERR_FORMAT);
    yau_stream_get_info(&g_wav.stream, &in);
    CHECK_I(in.end, 1234);
    CHECK_I(in.written, 1234);
    CHECK(strstr(yau_wav_error(&g_wav), "1234") != NULL);
    CHECK(yau_wav_load(&g_au, &(yau_wav_desc){ .data = g_wavmem, .size = n }).frames == NULL);
    yau_wav_close(&g_au, &g_wav);

    /* refusals, each with its message */
    {
        struct { int kind, ch; uint32_t rate; size_t patch_at; uint32_t patch16; int cut; const char* says; } cases[] = {
            { WK_S16, 2, 44100, 0, 0, 0, "44100" },
            { WK_S16, 3, 48000, 0, 0, 0, "3 channels" },
            { WK_S16, 2, 48000, 22, 8, 0, "8 bits" },          /* bits */
            { WK_S24_32, 2, 48000, 26, 32, 0, "32 valid" },    /* valid bits */
            { WK_F32, 2, 48000, 22, 64, 0, "64 bits" },
            { WK_S16, 2, 48000, 8, 7, 0, "tag 7" },            /* mu-law */
            { WK_S16, 2, 48000, 20, 5, 0, "block align" },
            { WK_S24_32, 2, 48000, 40, 1, 0, "subformat" },    /* the GUID */
            { WK_S16, 2, 48000, 0, 0, 100, "past the end" },
            { WK_S16, 2, 48000, 0, 0, -1, "no data chunk" },
        };
        size_t i;
        for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
            size_t len = make_wav(cases[i].kind, cases[i].ch, 3000, cases[i].rate, -1);
            if (cases[i].patch_at) put16(g_wavmem + g_fmt_off + cases[i].patch_at, cases[i].patch16);
            if (cases[i].cut > 0) len -= (size_t)cases[i].cut;
            if (cases[i].cut < 0) len = g_data_off - 8;
            CHECK(!yau_wav_open(&g_au, &g_wav, &(yau_wav_desc){ .data = g_wavmem, .size = len }));
            CHECK_I(g_wav.error_code, YAU_ERR_FORMAT);
            if (!strstr(yau_wav_error(&g_wav), cases[i].says)) {
                printf("wav refusal %d says: %s\n", (int)i, yau_wav_error(&g_wav));
                fail(__LINE__, "a WAV refusal without its reason");
            }
            /* v0.2.1: the probe refuses the form, and reports the rate and
             * channels for the caller to compare */
            {
                yau_wav_info pi;
                char pe[256];
                int prc = yau_wav_probe(&(yau_wav_desc){ .data = g_wavmem, .size = len }, &pi, pe, sizeof pe);
                if (i == 0) CHECK(prc == 0 && pi.rate == 44100);
                else if (i == 1) CHECK(prc == 0 && pi.channels == 3);
                else {
                    CHECK_I(prc, YAU_ERR_FORMAT);
                    if (!strstr(pe, cases[i].says)) fail(__LINE__, "a probe refusal without its reason");
                }
            }
        }
        memcpy(g_wavmem, "RIFX", 4);
        CHECK(!yau_wav_open(&g_au, &g_wav, &(yau_wav_desc){ .data = g_wavmem, .size = 100 }));
        CHECK_I(g_wav.error_code, YAU_ERR_FORMAT);
        CHECK(!yau_wav_open(&g_au, &g_wav, &(yau_wav_desc){ .data = g_wavmem, .size = 100, .path = "x.wav" }));
        CHECK_I(g_wav.error_code, YAU_ERR_ARG);
        printf("wav: refusal example: %s\n", yau_wav_error(&g_wav));
    }
    yau_close(&g_au);

    /* the EXTENSIBLE speaker mask against the project map */
    {
        static const uint8_t lr[2] = { YAU_CH_FL, YAU_CH_FR };
        yau_desc d;
        sd_default(s);
        s->tape = NULL;
        memset(&d, 0, sizeof d);
        d.format.map = lr;
        CHECK(open_dev(s, &d));
        n = make_wav(WK_S24_32, 2, 3000, 48000, -1);
        CHECK(yau_wav_open(&g_au, &g_wav, &(yau_wav_desc){ .data = g_wavmem, .size = n, .ring = 4000, .memory = g_ring_c }));
        yau_wav_close(&g_au, &g_wav);
        put32(g_wavmem + g_fmt_off + 28, 6u);   /* FR, FC */
        CHECK(!yau_wav_open(&g_au, &g_wav, &(yau_wav_desc){ .data = g_wavmem, .size = n }));
        CHECK_I(g_wav.error_code, YAU_ERR_FORMAT);
        CHECK(strstr(yau_wav_error(&g_wav), "speaker") != NULL);
        yau_close(&g_au);
    }
}

#if !defined(YRT_NO_THREADS)
/* A WAV on a real yrt_pump (yau_wav_step and yau_wav_on_msg), the
 * device on a real thread, the frame thread waking the pump and seeking
 * through messages between plays. ThreadSanitizer checks the lot. */
static void test_wav_pump(void) {
    sdev* s = &g_sd;
    static yrt_pump pump;
    yau_onset recs[12];
    int i, nplay = 12, done = 0;
    int64_t bad = 0, frames = 0, seeks[12];
    size_t n;
    sd_default(s);
    s->period = 48;
    s->L = 96;
    g_virtual = 0;
    CHECK(open_dev(s, NULL));
    n = make_wav(WK_S16, 2, 5000, 48000, -1);
    g_wav_A = 5000;
    CHECK(yau_wav_open(&g_au, &g_wav, &(yau_wav_desc){ .data = g_wavmem, .size = n, .loops = YAU_FOREVER, .ring = 1920, .memory = g_ring_c }));
    memset(&pump, 0, sizeof pump);
    CHECK(yrt_pump_open(&pump, &(yrt_pump_desc){ .msg_size = sizeof(yau_wav_msg), .capacity = 8,
                                                      .on_msg = yau_wav_on_msg, .on_idle = yau_wav_step, .ctx = &g_wav }));
    yau__st32(&g_thr_stop, 0);
    {
#if defined(_WIN32)
        HANDLE th = (HANDLE)_beginthreadex(NULL, 0, thr_main, s, 0, NULL);
#else
        pthread_t th;
        pthread_create(&th, NULL, thr_main, s);
#endif
        for (i = 0; i < nplay; i++) {
            yau_wav_msg m;
            yau_id id;
            int k, rc = YAU_PENDING;
            yau_stream_info in;
            seeks[i] = (int64_t)(sd_rand(s) * 20000.0);
            m.seek = seeks[i];
            while (yrt_pump_submit(&pump, &m) < 0) yrt_sleep_until(yrt_now_ns() + 1000000, 0);
            for (k = 0; k < 3000; k++) {
                yau_stream_get_info(&g_wav.stream, &in);
                if (in.state == YAU_STREAM_IDLE && in.first == seeks[i] && in.ready) break;
                m.seek = -1;
                if (yau_wav_wants(&g_wav)) (void)yrt_pump_submit(&pump, &m);
                yrt_sleep_until(yrt_now_ns() + 1000000, 0);
            }
            id = yau_play_stream(&g_au, &g_wav.stream, 0);
            if (id <= 0) { fail_i(__LINE__, "wav play refused", id, 1); break; }
            for (k = 0; k < 30; k++) {
                m.seek = -1;
                if (yau_wav_wants(&g_wav)) (void)yrt_pump_submit(&pump, &m);
                yrt_sleep_until(yrt_now_ns() + 1000000, 0);
            }
            CHECK(yau_stop_at(&g_au, id, 0, 0) == 0);
            for (k = 0; k < 3000; k++) {
                rc = yau_result(&g_au, id, &recs[i]);
                if (rc == YAU_OK && recs[i].end_frame > 0) break;
                yrt_sleep_until(yrt_now_ns() + 1000000, 0);
            }
            CHECK_I(rc, YAU_OK);
            if (rc == YAU_OK) done++;
        }
        yau__st32(&g_thr_stop, 1);
#if defined(_WIN32)
        WaitForSingleObject(th, INFINITE); CloseHandle(th);
#else
        pthread_join(th, NULL);
#endif
    }
    yrt_pump_close(&pump);
    {
        int64_t f, lim = s->W < TAPE_FRAMES ? s->W : TAPE_FRAMES;
        int p = 0;
        for (f = 0; f < lim; f++) {
            float a = g_tape[f * 2], b = g_tape[f * 2 + 1];
            while (p < done && f >= recs[p].end_frame) p++;
            if (p < done && f >= recs[p].start_frame) {
                int64_t smp = f - (recs[p].start_frame - recs[p].sample);
                if ((a != 0.0f || b != 0.0f) && (a != wv_loop(smp, 0) || b != wv_loop(smp, 1))) bad++;
                frames++;
            } else if (a != 0.0f || b != 0.0f) {
                bad++;
            }
        }
        for (i = 0; i < done; i++) if (recs[i].sample != seeks[i]) fail_i(__LINE__, "play did not start at its seek", recs[i].sample, seeks[i]);
    }
    printf("wav pump: %d plays, %lld frames, %lld wrong\n", done, (long long)frames, (long long)bad);
    CHECK_I(done, nplay);
    CHECK_I(bad, 0);
    yau_wav_close(&g_au, &g_wav);
    yau_close(&g_au);
    g_virtual = 1;
}
#endif

#endif /* YAU_VERSION_MINOR >= 2 */

#if YAU_VERSION_MINOR >= 3
/* QUEUE (v0.3.0): desc.queue reaches the device and the caps (0 means 2),
 * out-of-range values are refused, and the gate's threshold. The gate itself waits on
 * WASAPI's event; audio_clockstats --queue measures it on hardware. */
static void test_queue(void) {
    sdev* s = &g_sd;
    yau_desc d;
    yau_caps c;
    char line[512];
    int q;
    for (q = 0; q <= 2; q++) {
        sd_default(s);
        memset(&d, 0, sizeof d);
        d.queue = q;
        CHECK(open_dev(s, &d));
        CHECK_I(s->queue_asked, q == 0 ? 2 : q);    /* 0 = the default, 2 */
        yau_get_caps(&g_au, &c);
        CHECK_I(c.queue, q == 0 ? 2 : q);
        yau_describe(&g_au, line, sizeof line);
        CHECK(strstr(line, q == 1 ? "queue=1" : "queue=2") != NULL);
        yau_close(&g_au);
    }
    sd_default(s);
    memset(&d, 0, sizeof d);
    d.queue = 3;
    CHECK(!open_dev(s, &d));
    CHECK(strstr(yau_error(&g_au), "queue 0..2") != NULL);
    d.queue = -1;
    CHECK(!open_dev(s, &d));
    /* the threshold: the measured endpoint's 480-frame period and 1056-frame
     * buffer (docs/audio.md, "Periods and buffers") */
    CHECK_I(YAU__QUEUE_PAD(1, 480, 1056, 0), 0);
    CHECK_I(YAU__QUEUE_PAD(2, 480, 1056, 0), 480);
    CHECK_I(YAU__QUEUE_PAD(3, 480, 1056, 0), -1);    /* 576 frames of room, not 960 */
    CHECK_I(YAU__QUEUE_PAD(2, 480, 960, 0), 480);
    CHECK_I(YAU__QUEUE_PAD(2, 480, 959, 0), -1);
    CHECK_I(YAU__QUEUE_PAD(0, 480, 1056, 0), -1);
    CHECK_I(YAU__QUEUE_PAD(1, 480, 1056, 1), -1);    /* exclusive: no gate */
    CHECK_I(YAU__QUEUE_PAD(1, 0, 1056, 0), -1);
    CHECK_I(YAU__QUEUE_PAD(1, 144, 288, 0), 0);
}
#endif

static void test_misc(void) {
    int n = 0;
    const yau_param* p = yau_params(&n);
    CHECK(p && n >= 10);
    CHECK(strcmp(yau_strerror(YAU_ERR_FULL), "ok") != 0);
    CHECK(strcmp(yau_strerror(0), "ok") == 0);
    CHECK(strcmp(yau_version(), YAU_VERSION_STRING) == 0);
    {
        yau_buf z;
        memset(&z, 0, sizeof z);
        CHECK_I(yau_play_at(NULL, z, 0), YAU_ERR_ARG);
    }
}

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    g_tape = (float*)calloc((size_t)TAPE_FRAMES * 2, sizeof(float));
    if (!g_tape) return 2;
    if (argc == 3 && strcmp(argv[1], "--tape") == 0) {
        (void)tape_regression(argv[2]);
        free(g_tape);
        return g_failures ? 1 : 0;
    }
    test_plan_and_placement();
    test_fit_drift();
    test_controls();
    test_replan();
    test_xrun();
    test_halt();
    test_callback_source();
    test_ring();
    test_limits();
    test_strict_open();
    test_formats();
    test_synthesis();
    test_threads();
    (void)tape_regression(NULL);
#if YAU_VERSION_MINOR >= 2
    test_stream_ring();
    test_stream_close();
    test_stream_placement();
    test_stream_gaps();
    test_stream_late_and_asap();
    test_stream_mix();
    test_stream_device();
    test_stream_gap_storm();
    test_stream_threads();
    test_wav();
#if !defined(YRT_NO_THREADS)
    test_wav_pump();
#endif
#endif
#if YAU_VERSION_MINOR >= 3
    test_queue();
#endif
    test_misc();
    free(g_tape);
    if (g_failures) { fprintf(stderr, "audio_test: %d failure(s)\n", g_failures); return 1; }
    printf("audio_test: all checks passed\n");
    return 0;
}
