/* web_av_sync.c - a flash and a click planned for one time, in a browser:
 * what the two records say about sound minus light, and what the browser
 * says about its audio output. The web form of examples/audio/av_sync.c,
 * without the line boards.
 *
 * Each trial plans a flash (a light disk on a dark field for 3 frames;
 * WARNING: about one flash a second) on frame j, K frames ahead (K =
 * --lead, default 0.3 s, in frames), and a 2 ms click with yau_play_at()
 * at frame j's predicted onset. The flip record of frame j gives the
 * light's onset as ysp/screen.h estimates it, the click's record the
 * sound's as ysp/audio.h's fit puts it. In a browser both are the
 * browser's estimates (tier 5, docs/web.md): the flip's onset is the
 * frame callback's vsync, before the compositor's delay; the click's is
 * the fit of the AudioContext's getOutputTimestamp() reports, which carry
 * the browser's own figure for its output latency. Neither is light or
 * sound. "audio - flip" is what the two headers believe; a photodiode and
 * a microphone on an oscilloscope say what happens.
 *
 * It also records every audio callback's entry time and position report
 * (the YAU__ON_TICK measurement seam), for the callbacks' regularity.
 *
 * --sim: the simulated display and miniaudio's null device (CI); checks
 * that every trial has both records and |audio - flip| <= 2 ms.
 *
 * Usage: web_av_sync [--sim] [--n N] [--lead S] [--period FRAMES]
 *                    [--size WxH]
 *   --n N           trials (default 20; --sim 8)
 *   --lead S        how far ahead each pair is planned, s (default 0.3)
 *   --period FRAMES the audio period asked for; 0 = the backend's (a
 *                   ScriptProcessorNode rounds it up to a power of 2, an
 *                   AudioWorklet renders 128 frames a call)
 *   --size WxH      the window in CSS pixels (default 720x480)
 * Exit code: 0 (also after Shift+Esc); 1 when the screen did not open or a
 * --sim check failed; 2 for a bad argument. With no audio the flashes run
 * and the page says why.
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
/* No system header before the ysp headers: glibc fixes its feature set at
 * the first one, and rt.h's _DEFAULT_SOURCE must come first (CI Linux). */
struct yau_tick;
static void av_tick(const struct yau_tick* tk);
#define YAU__ON_TICK(au, tk) av_tick(tk)   /* measurement seam (ysp/audio.h) */
#define YSP_AUDIO_IMPLEMENTATION
#include "ysp/audio.h"
#define YSP_GFX_IMPLEMENTATION
#include "ysp/gfx.h"
#include "web_common.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
/* The AudioContext's own figures now, seconds; -1 when there are none. */
EM_JS(void, av_latency, (double* base, double* out), {
    var ma = window.miniaudio, d = null, i;
    for (i = 0; ma && i < ma.devices.length && !d; i++) if (ma.devices[i] && ma.devices[i].webaudio) d = ma.devices[i];
    var c = d ? d.webaudio : null;
    HEAPF64[base >> 3] = c && typeof c.baseLatency === 'number' ? c.baseLatency : -1;
    HEAPF64[out >> 3] = c && typeof c.outputLatency === 'number' ? c.outputLatency : -1;
})
EM_JS(void, av_done, (const char* json, const char* csv), {
    Module['yspCsv'] = UTF8ToString(csv);
    Module['yspResult'] = JSON.parse(UTF8ToString(json));
    if (Module['onYspDone']) Module['onYspDone'](Module['yspResult']);
})
EM_JS_DEPS(web_av_deps, "$UTF8ToString")
#else
static void av_latency(double* base, double* out) { *base = *out = -1; }
#endif

#define MAX_TRIALS   200
#define FLASH_FRAMES 3
#define MAX_TICKS    32768

typedef struct trial {
    int64_t     frame, plan;
    yau_id      id;
    yau_onset   a;
    int         a_rc, have_flip;
    yscr_record flip;
    double      base_s, out_s;
} trial;

static yscr_screen scr;
static ygfx_gfx gfx;
static yau_audio au;
static trial trials[MAX_TRIALS];
static char csv[1 << 16], json[1 << 16];

/* The callback thread writes these; main reads them after yau_close(). */
static int64_t tick_t[MAX_TICKS];
static volatile int32_t n_ticks, n_reports;
static volatile int64_t first_report_t, last_report_t;

static void av_tick(const struct yau_tick* tk) {
    int32_t i = n_ticks;
    if (i < MAX_TICKS) { tick_t[i] = tk->t_entry; n_ticks = i + 1; }
    if (tk->pos >= 0) {
        if (!n_reports) first_report_t = tk->pos_t;
        last_report_t = tk->pos_t;
        n_reports = n_reports + 1;
    }
}

static int cmp_d(const void* a, const void* b) {
    double x = *(const double*)a, y = *(const double*)b;
    return x < y ? -1 : (x > y);
}

/* mean, sd, min, median, p99, max of v[0..n); sorts v */
typedef struct summary { int n; double mean, sd, min, med, p99, max; } summary;
static summary stats(double* v, int n) {
    summary s;
    int i;
    memset(&s, 0, sizeof s);
    s.n = n;
    if (n <= 0) return s;
    for (i = 0; i < n; i++) s.mean += v[i];
    s.mean /= n;
    for (i = 0; i < n; i++) s.sd += (v[i] - s.mean) * (v[i] - s.mean);
    s.sd = sqrt(s.sd / (n > 1 ? n - 1 : 1));
    qsort(v, (size_t)n, sizeof v[0], cmp_d);
    s.min = v[0];
    s.med = v[n / 2];
    s.p99 = v[(int)((n - 1) * 0.99 + 0.5)];
    s.max = v[n - 1];
    return s;
}

static size_t put_stat(char* b, size_t cap, const char* name, summary s) {
    return (size_t)snprintf(b, cap, "\"%s\":{\"n\":%d,\"mean\":%.4f,\"sd\":%.4f,\"min\":%.4f,\"median\":%.4f,"
                            "\"p99\":%.4f,\"max\":%.4f}", name, s.n, s.mean, s.sd, s.min, s.med, s.p99, s.max);
}

/* A JSON string of s into b (quotes and backslashes escaped). */
static size_t put_str(char* b, size_t cap, const char* s) {
    size_t k = 0;
    if (cap < 3) return 0;
    b[k++] = '"';
    for (; *s && k + 3 < cap; s++) {
        if (*s == '"' || *s == '\\') b[k++] = '\\';
        b[k++] = (*s >= 0 && *s < 32) ? ' ' : *s;
    }
    b[k++] = '"';
    b[k] = 0;
    return k;
}

int main(int argc, char** argv) {
    yscr_desc sd;
    ygfx_desc gd;
    yau_desc ad;
    yau_click_desc cd;
    yau_buf click = { 0 };
    yau_caps ac;
    yscr_frame f;
    ygfx_shape_desc sh;
    ygfx_stim disk, dot;
    char line[1024], aline[1024], aerr[512] = "";   /* holds yau_error() whole */
    static double v[MAX_TICKS], g[MAX_TICKS];
    double lead_s = 0.3;
    float u, scr_w, scr_h;
    int i, n = 0, sim = 0, failed = 0, aborted = 0, planned = 0, K = 0, period = 0, have_audio = 0;
    int win_w = 720, win_h = 480, bad = 0, lit;
    int64_t next_plan = -1, end_frame = -1, per = 0, t_end = 0;
    uint64_t rng = 0x2545F4914F6CDD1DULL;
    size_t k = 0, c = 0;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--sim")) sim = 1;
        else if (!strcmp(argv[i], "--n") && i + 1 < argc) n = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--lead") && i + 1 < argc) lead_s = atof(argv[++i]);
        else if (!strcmp(argv[i], "--period") && i + 1 < argc) period = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--size") && i + 1 < argc && web_size_arg(argv[i + 1], &win_w, &win_h)) i++;
        else {
            fprintf(stderr, "usage: web_av_sync [--sim] [--n N] [--lead S] [--period FRAMES] [--size WxH]\n");
            return 2;
        }
    }
    if (n == 0) n = sim ? 8 : 20;
    if (n < 1 || n > MAX_TRIALS || !(lead_s >= 0.05 && lead_s <= 2) || period < 0 || period > 16384) {
        fprintf(stderr, "web_av_sync: --n 1..%d, --lead 0.05..2 s, --period 0..16384\n", MAX_TRIALS);
        return 2;
    }

    /* The sound first: in a browser this runs inside the Start button's
     * click, the user gesture an AudioContext needs (iOS Safari above all). */
    memset(&ad, 0, sizeof ad);
    if (sim) ad.backend = YAU_BACKEND_NULL;
    ad.period = period;
    ad.arena_bytes = 1 << 20;
    if (yau_open(&au, &ad)) {
        have_audio = 1;
        memset(&cd, 0, sizeof cd);
        cd.dur_ns = YRT_MS(2);
        cd.peak = yau_db(-26.0f);
        click = yau_click(&au, &cd);
        if (!click.frames) { snprintf(aerr, sizeof aerr, "%s", yau_error(&au)); yau_close(&au); have_audio = 0; }
    } else {
        snprintf(aerr, sizeof aerr, "%s", yau_error(&au));
    }
    if (!have_audio) {
        fprintf(stderr, "web_av_sync: no audio: %s\n", aerr);
        if (sim) return 1;
    }

    memset(&sd, 0, sizeof sd);
    if (sim) sd.backend = YSCR_BACKEND_SIM;
    sd.windowed = true;
    sd.window_w = win_w;
    sd.window_h = win_h;
    memset(&gd, 0, sizeof gd);
    gd.background[0] = gd.background[1] = gd.background[2] = 0.05f;
    if (!ygfx_open_screen(&gfx, &scr, &sd, &gd)) {   /* opens, finishes and primes */
        fprintf(stderr, "web_av_sync: %s\n", ygfx_error(&gfx));
        if (have_audio) yau_close(&au);
        return 1;
    }
    ygfx_size(&gfx, &scr_w, &scr_h);
    u = (scr_w < scr_h ? scr_w : scr_h) / 480.0f;
    if (!(u > 0)) u = 1;
    memset(&sh, 0, sizeof sh);
    sh.shape = YGFX_ELLIPSE;
    sh.w = sh.h = 160 * u;
    sh.edge = YGFX_EDGE_COSINE;
    sh.edge_width = 1;
    sh.color[0] = sh.color[1] = sh.color[2] = 0.6f;
    disk = ygfx_shape(&sh);
    sh.w = sh.h = 8 * u;
    sh.color[0] = sh.color[1] = sh.color[2] = 0.35f;
    dot = ygfx_shape(&sh);
    ygfx_prime(&gfx);
#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)
    /* The ScriptProcessorNode's callbacks run on this thread, and the
     * screen's setup above held it for about 2 s. After such a stall its
     * blocks' playbackTime no longer told where they played (Firefox 157:
     * every onset 7.7 s off; Chrome 154: 51 to 63 skipped blocks), so the
     * sound opens again now. The page has had its tap: Chrome and Firefox
     * let the new AudioContext start. */
    if (have_audio) {
        yau_close(&au);
        have_audio = 0;
        n_ticks = 0;   /* the callbacks of this stream only */
        n_reports = 0;
        if (yau_open(&au, &ad)) {
            have_audio = 1;
            click = yau_click(&au, &cd);
            if (!click.frames) { snprintf(aerr, sizeof aerr, "%s", yau_error(&au)); yau_close(&au); have_audio = 0; }
        } else {
            snprintf(aerr, sizeof aerr, "%s", yau_error(&au));
        }
    }
#endif

    yscr_describe(&scr, line, sizeof line);
    printf("%s\n", line);
    if (have_audio) {
        yau_describe(&au, aline, sizeof aline);
        printf("%s\n", aline);
    } else {
        snprintf(aline, sizeof aline, "ysp_audio: none (%s)", aerr);
    }
    web_ready();

    for (;;) {
        SDL_Event ev;
        int rc = yscr_begin(&scr, &f);
        if (rc == YSCR_QUIT) { aborted = 1; break; }
        if (rc != YSCR_OK) { fprintf(stderr, "web_av_sync: %s\n", yscr_error(&scr)); failed = 1; break; }
        if (!K) {
            per = f.period > 0 ? f.period : YRT_S(1.0 / 60);
            K = (int)((YRT_S(lead_s) + per - 1) / per);
            /* 2 s on the dot first: the screen's setup stalled the main thread
             * after the sound opened, and the fit starts over after a stall */
            next_plan = f.index + (int64_t)(YRT_S(2) / per);
        }
        for (i = 0; i < f.n_done; i++) {
            int j;
            for (j = 0; j < planned; j++)
                if (trials[j].frame == f.done[i].index) { trials[j].flip = f.done[i]; trials[j].have_flip = 1; }
        }
        if (planned < n && f.index >= next_plan) {   /* the next pair: K frames ahead */
            trial* t = &trials[planned++];
            memset(t, 0, sizeof *t);
            t->frame = -1;
            t->plan = f.onset + (int64_t)K * per;
            t->id = have_audio ? yau_play_at(&au, click, t->plan) : -1;
            t->a_rc = t->id > 0 ? YAU_PENDING : (int)t->id;
            av_latency(&t->base_s, &t->out_s);
            rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
            next_plan = f.index + K + FLASH_FRAMES + (int64_t)(YRT_MS(800) / per) + (int64_t)(rng % (uint64_t)(YRT_MS(400) / per + 1));
            if (planned == n) end_frame = f.index + K + FLASH_FRAMES + (int64_t)(YRT_MS(500) / per);
        }
        /* the flash starts on the first frame whose predicted onset is the
         * plan's, to half a frame: a frame the browser skipped in between
         * does not move it (ysp/timeline.h's landing rule) */
        for (i = 0, lit = 0; i < planned; i++) {
            trial* t = &trials[i];
            if (t->frame < 0 && f.onset >= t->plan - per / 2) t->frame = f.index;
            if (t->frame >= 0 && f.index >= t->frame && f.index < t->frame + FLASH_FRAMES) lit = 1;
        }
        ygfx_begin(&gfx, &f);
        if (lit) ygfx_draw(&gfx, &disk);
        ygfx_draw(&gfx, &dot);
        ygfx_end(&gfx);
        yscr_flip(&scr);
        while (yscr_poll(&scr, &ev, NULL)) { }
        if (have_audio) {
            (void)yau_update(&au);
            for (i = 0; i < planned; i++)
                if (trials[i].a_rc == YAU_PENDING) trials[i].a_rc = yau_result(&au, trials[i].id, &trials[i].a);
        }
        if (end_frame >= 0 && f.index >= end_frame) {   /* the last records, at most 2 s more */
            int pending = 0;
            if (!t_end) t_end = f.onset;
            for (i = 0; i < planned; i++) pending += trials[i].a_rc == YAU_PENDING || !trials[i].have_flip;
            if (!pending || f.onset - t_end > YRT_S(2)) break;
        }
    }
    yscr_wait_flip(&scr, NULL);
    yscr_describe(&scr, line, sizeof line);
    if (have_audio) {
        yau_get_caps(&au, &ac);
        yau_describe(&au, aline, sizeof aline);
        yau_close(&au);
    } else {
        memset(&ac, 0, sizeof ac);
    }

    /* the trials */
    c = (size_t)snprintf(csv, sizeof csv, "trial,frame,flip_minus_plan_s,audio_minus_plan_s,audio_minus_flip_s,flip_tier,"
                                          "flip_dropped,audio_tier,audio_flags,render_to_onset_s,base_latency_s,output_latency_s\n");
    k = (size_t)snprintf(json, sizeof json, "{\"trials\":[");
    {
        static double av[MAX_TRIALS], ro[MAX_TRIALS];
        int nav = 0, nro = 0;
        summary s_av, s_ro, s_iv, s_gap;
        double nominal_ms = have_audio && ac.rate ? 1e3 * (double)ac.period / (double)ac.rate : 0;
        int n_iv = 0, n_gap = 0, n_long = 0, n_zero = 0;
        double min_pos = 1e9;
        printf("trial  frame   flip - plan  audio - plan  audio - flip  (ms)\n");
        for (i = 0; i < planned; i++) {
            trial* t = &trials[i];
            int ok = t->a_rc == YAU_OK && t->have_flip && t->flip.onset;
            double fp = t->have_flip && t->flip.onset ? (double)(t->flip.onset - t->plan) * 1e-6 : NAN;
            double ap = t->a_rc == YAU_OK ? (double)(t->a.onset - t->plan) * 1e-6 : NAN;
            double d = ok ? (double)(t->a.onset - t->flip.onset) * 1e-6 : NAN;
            double r = t->a_rc == YAU_OK ? (double)(t->a.onset - t->a.rendered_at) * 1e-6 : NAN;
            if (ok) av[nav++] = d;
            if (t->a_rc == YAU_OK) ro[nro++] = r;
            printf("%5d %6lld  %+12.3f  %+12.3f  %+12.3f  tiers %u/%u flags 0x%03x\n", i, (long long)t->frame, fp, ap, d,
                   (unsigned)t->flip.tier, (unsigned)t->a.tier, (unsigned)t->a.flags);
            if (c < sizeof csv)
                c += (size_t)snprintf(csv + c, sizeof csv - c, "%d,%lld,%.6f,%.6f,%.6f,%u,%u,%u,%u,%.6f,%.6f,%.6f\n", i,
                                      (long long)t->frame, fp * 1e-3, ap * 1e-3, d * 1e-3, (unsigned)t->flip.tier,
                                      (unsigned)t->flip.dropped, (unsigned)t->a.tier, (unsigned)t->a.flags, r * 1e-3,
                                      t->base_s, t->out_s);
            if (k < sizeof json - 256)
                k += (size_t)snprintf(json + k, sizeof json - k,
                                      "%s{\"flip_minus_plan_ms\":%.4f,\"audio_minus_plan_ms\":%.4f,\"audio_minus_flip_ms\":%.4f,"
                                      "\"flip_tier\":%u,\"audio_tier\":%u,\"audio_flags\":%u,\"render_to_onset_ms\":%.3f,"
                                      "\"output_latency_ms\":%.3f,\"base_latency_ms\":%.3f}",
                                      i ? "," : "", isnan(fp) ? -1e9 : fp, isnan(ap) ? -1e9 : ap, isnan(d) ? -1e9 : d,
                                      (unsigned)t->flip.tier, (unsigned)t->a.tier, (unsigned)t->a.flags, isnan(r) ? -1e9 : r,
                                      t->out_s * 1e3, t->base_s * 1e3);
            /* --sim: the sound on plan, the flash on plan or whole frames
             * late (a loaded host's frame loop falls behind, as a real
             * display's would; CI macOS showed 1 to 4 frames) */
            double per_ms = (double)per * 1e-6, late = per_ms > 0 ? floor(fp / per_ms + 0.5) : 0;
            int flip_ok = ok && late >= 0 && late <= 6 && fabs(fp - late * per_ms) < 0.01;
            if (sim && (!flip_ok || t->flip.tier != YSCR_TIER_SIM || t->a.tier != YAU_TIER_SIM || fabs(ap) > 2.0)) {
                fprintf(stderr, "web_av_sync: --sim: trial %d is off (audio %s, flip %d, %.3f ms)\n", i,
                        yau_strerror(t->a_rc), t->have_flip, d);
                bad = 1;
            }
        }
        /* the callbacks: entry-to-entry intervals, ms */
        for (i = 1; i < n_ticks && i < MAX_TICKS; i++) {
            double x = (double)(tick_t[i] - tick_t[i - 1]) * 1e-6;
            if (x == 0) n_zero++;
            else if (x > 0 && x < min_pos) min_pos = x;
            if (nominal_ms > 0 && x > 1.5 * nominal_ms) n_long++;
            v[n_iv++] = x;
            if (x >= 2.0) g[n_gap++] = x;   /* between bursts: a worklet renders several blocks per device callback */
        }
        s_av = stats(av, nav);
        s_ro = stats(ro, nro);
        s_iv = stats(v, n_iv);
        s_gap = stats(g, n_gap);
        printf("audio - flip: n %d mean %+.3f sd %.3f min %+.3f max %+.3f ms\n", s_av.n, s_av.mean, s_av.sd, s_av.min, s_av.max);
        printf("callbacks %d (nominal %.3f ms): p50 %.3f p99 %.3f max %.3f ms, over 1.5x %d, zero %d, smallest step %.3f ms; "
               "reports %d\n", (int)n_ticks, nominal_ms, s_iv.med, s_iv.p99, s_iv.max, n_long, n_zero,
               min_pos < 1e9 ? min_pos : 0.0, (int)n_reports);
        printf("gaps of 2 ms or more: n %d p50 %.3f p99 %.3f max %.3f ms\n", s_gap.n, s_gap.med, s_gap.p99, s_gap.max);
        printf("%s\n%s\n", line, aline);
        if (k < sizeof json - 1200) {
            k += (size_t)snprintf(json + k, sizeof json - k, "],");
            k += put_stat(json + k, sizeof json - k, "audio_minus_flip_ms", s_av);
            k += (size_t)snprintf(json + k, sizeof json - k, ",");
            k += put_stat(json + k, sizeof json - k, "render_to_onset_ms", s_ro);
            k += (size_t)snprintf(json + k, sizeof json - k, ",");
            k += put_stat(json + k, sizeof json - k, "callback_interval_ms", s_iv);
            k += (size_t)snprintf(json + k, sizeof json - k, ",");
            k += put_stat(json + k, sizeof json - k, "callback_gap_ms", s_gap);
            k += (size_t)snprintf(json + k, sizeof json - k,
                                  ",\"callbacks\":%d,\"callback_nominal_ms\":%.4f,\"callbacks_over_1_5x\":%d,\"callback_zero\":%d,"
                                  "\"callback_step_ms\":%.4f,\"reports\":%d,\"report_span_s\":%.3f,\"xruns\":%u,\"worst_tier\":%d,"
                                  "\"drift_ppm\":%.2f,\"fit_spread_us\":%.1f,\"lead_ms\":%.3f,\"period\":%d,\"rate\":%u,"
                                  "\"have_audio\":%d,\"stopped\":%d,\"screen\":",
                                  (int)n_ticks, nominal_ms, n_long, n_zero, min_pos < 1e9 ? min_pos : 0.0, (int)n_reports,
                                  n_reports > 1 ? (double)(last_report_t - first_report_t) * 1e-9 : 0.0, (unsigned)ac.xruns,
                                  (int)ac.worst_tier, ac.drift_ppm, ac.fit_spread_ns * 1e-3, (double)ac.lead_ns * 1e-6,
                                  (int)ac.period, (unsigned)ac.rate, have_audio, aborted);
            k += put_str(json + k, sizeof json - k, line);
            k += (size_t)snprintf(json + k, sizeof json - k, ",\"audio\":");
            k += put_str(json + k, sizeof json - k, aline);
            if (k < sizeof json - 2) { json[k++] = '}'; json[k] = 0; }
        }
    }
    ygfx_close(&gfx);
    yscr_close(&scr);
#if defined(__EMSCRIPTEN__)
    av_done(json, csv);
#endif
    if (sim && (bad || planned != n)) return 1;
    if (sim) printf("--sim: every sound within 2 ms of its plan, every flash on its frame or whole frames late\n");
    return failed;
}
