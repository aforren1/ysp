/* web_selftest.c - what a browser's frame loop can and cannot show about
 * timing: the measured refresh, the interval between frame callbacks as a
 * histogram, the clock's resolution, cross-origin isolation, visibility
 * changes, and, for keys pressed during the run, how late SDL stamped
 * them against the DOM event's own time.
 *
 * It draws a gray field with a bar that moves one step per frame, so a
 * skipped frame shows as a jump. Nothing here is a statement about light:
 * a browser reports when it began a frame, not when the display showed
 * it, and the compositor's delay after that is unknown (docs/web.md).
 *
 * The same program runs natively on ysp/screen.h's own backends (DXGI or
 * COMPOSITION on Windows), which is the comparison: the native onsets are
 * the OS's vblank times.
 *
 * Usage: web_selftest [--frames N] [--load-ms X] [--size WxH] [--sim]
 *   --frames N   frames to run (default 600, about 10 s at 60 Hz)
 *   --load-ms X  busy CPU time per frame, to show what a late frame does
 *   --size WxH   the window in CSS pixels (default 640x360)
 *   --sim        the simulated display (CI): checks that the loop runs
 * Exit code: 0; 1 when the screen did not open; 2 for a bad argument.
 * Shift+Esc (or the page's Stop button) ends the run early.
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#define YSP_SCREEN_IMPLEMENTATION
#include "ysp/screen.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
/* performance.now() in ms; the frame callback's start (screen.h sets it). */
EM_JS(double, web_perf_ms, (void), { return performance.now(); })
EM_JS(double, web_raf_run_ms, (void), { return Module['yscrRafRun'] || 0; })
EM_JS(double, web_raf_t_ms, (void), { return Module['yscrRafT'] || 0; })
/* A key SDL stamped, in performance.now() ms, for the page to pair with
 * the DOM event's timeStamp. */
EM_JS(void, web_key, (int down, double ms), {
    (Module['yspKeys'] = Module['yspKeys'] || []).push([down, ms]);
})
EM_JS(void, web_result, (const char* json), {
    Module['yspResult'] = JSON.parse(UTF8ToString(json));
    if (Module['onYspResult']) Module['onYspResult'](Module['yspResult']);
})
EM_JS_DEPS(web_selftest_deps, "$UTF8ToString")
static uint64_t perf_ns(void) { return (uint64_t)(web_perf_ms() * 1e6); }
#endif

#define MAX_FRAMES 36000
#define BINS 200          /* 0.25 ms each, 0 to 50 ms */
#define BIN_NS 250000

typedef void (*clear_color_fn)(float, float, float, float);
typedef void (*clear_fn)(unsigned int);
typedef void (*scissor_fn)(int, int, int, int);
typedef void (*enable_fn)(unsigned int);

static yscr_screen scr;
static int64_t iv[MAX_FRAMES], cost[MAX_FRAMES], resume[MAX_FRAMES];
static uint32_t hist[BINS + 1];

static void add_interval(int64_t x, int* n) {
    int b = (int)(x / BIN_NS);
    iv[(*n)++] = x;
    hist[b < 0 ? 0 : b > BINS ? BINS : b]++;
}

static int cmp64(const void* a, const void* b) {
    int64_t x = *(const int64_t*)a, y = *(const int64_t*)b;
    return x < y ? -1 : x > y;
}

static double pct(int64_t* v, int n, double p) {
    if (n <= 0) return 0;
    return (double)v[(int)((n - 1) * p + 0.5)];
}

int main(int argc, char** argv) {
    yscr_desc d;
    yscr_frame f;
    yscr_caps caps;
    yrt_clock_info ci;
    char line[1536], json[8192];
    int i, n = 0, n_iv = 0, n_res = 0, frames = 600, w = 640, h = 360, sim = 0, aborted = 0;
    double load_ms = 0;
    int64_t prev_onset = 0, prev_index = -1, dropped = 0, early = 0, late = 0, keys = 0;
    clear_color_fn clear_color;
    clear_fn clear;
    scissor_fn scissor;
    enable_fn enable, disable;
    /* The histogram: the browser's callback times in a browser, the
     * records' onsets (the OS's vblank times) on a native backend. */
    int on_grid = 1;
#if defined(__EMSCRIPTEN__)
    yrt_corr_desc cd;
    yrt_corr corr;
    int64_t perf_off = 0;   /* ysp_rt ns minus performance.now() ns */
    double prev_raf = 0;
#endif

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--load-ms") && i + 1 < argc) load_ms = atof(argv[++i]);
        else if (!strcmp(argv[i], "--size") && i + 1 < argc) {
            if (sscanf(argv[++i], "%dx%d", &w, &h) != 2) w = 0;
        } else if (!strcmp(argv[i], "--sim")) sim = 1;
        else w = 0;
        if (w <= 0 || h <= 0) {
            fprintf(stderr, "usage: web_selftest [--frames N] [--load-ms X] [--size WxH] [--sim]\n");
            return 2;
        }
    }
    if (frames < 2 || frames > MAX_FRAMES || load_ms < 0 || load_ms > 100) {
        fprintf(stderr, "web_selftest: --frames takes 2..%d, --load-ms 0..100\n", MAX_FRAMES);
        return 2;
    }
    memset(&d, 0, sizeof d);
    d.windowed = true;
    d.window_w = w;
    d.window_h = h;
    if (sim) d.backend = YSCR_BACKEND_SIM;
#if defined(__EMSCRIPTEN__)
    on_grid = sim;
#endif
    if (!yscr_open(&scr, &d)) { fprintf(stderr, "web_selftest: %s\n", yscr_error(&scr)); return 1; }
    yscr_get_caps(&scr, &caps);
    yrt_get_clock_info(&ci);
    clear_color = (clear_color_fn)yscr_gl_proc(&scr, "glClearColor");
    clear = (clear_fn)yscr_gl_proc(&scr, "glClear");
    scissor = (scissor_fn)yscr_gl_proc(&scr, "glScissor");
    enable = (enable_fn)yscr_gl_proc(&scr, "glEnable");
    disable = (enable_fn)yscr_gl_proc(&scr, "glDisable");
#if defined(__EMSCRIPTEN__)
    memset(&cd, 0, sizeof cd);
    cd.read = perf_ns;
    if (yrt_correlate(&cd, &corr) > 0) perf_off = corr.rt_ns - (int64_t)corr.other;
#endif
    yscr_describe(&scr, line, sizeof line);
    printf("%s\n", line);

    for (;;) {
        SDL_Event ev;
        int64_t t_in, t_out;
        int rc = yscr_begin(&scr, &f);
        if (rc == YSCR_QUIT) { aborted = 1; break; }
        if (rc != YSCR_OK) { fprintf(stderr, "web_selftest: %s\n", yscr_error(&scr)); break; }
        t_in = yrt_now_ns();
#if defined(__EMSCRIPTEN__)
        if (!sim && web_raf_run_ms() > 0 && n_res < MAX_FRAMES)
            resume[n_res++] = (int64_t)llround((web_perf_ms() - web_raf_run_ms()) * 1e6);
        if (!sim) {   /* the callbacks' own times, as the browser gave them */
            double t = web_raf_t_ms();
            if (prev_raf > 0 && t > prev_raf && n_iv < MAX_FRAMES) add_interval((int64_t)llround((t - prev_raf) * 1e6), &n_iv);
            prev_raf = t;
        }
#endif
        for (i = 0; i < f.n_done; i++) {
            const yscr_record* r = &f.done[i];
            if (r->flags & (YSCR_FLIP_SKIPPED | YSCR_FLIP_CANCELED)) continue;
            dropped += r->dropped;
            if (r->flags & YSCR_FLIP_EARLY) early++;
            if (r->flags & YSCR_FLIP_LATE_TARGET) late++;   /* the program was late, not the browser */
            if (on_grid && prev_onset && r->onset && r->index == prev_index + 1 && n_iv < MAX_FRAMES)
                add_interval(r->onset - prev_onset, &n_iv);
            if (r->onset) { prev_onset = r->onset; prev_index = r->index; }
        }
        while (yscr_poll(&scr, &ev, &t_in)) {
            if (ev.type == SDL_EVENT_KEY_DOWN || ev.type == SDL_EVENT_KEY_UP) {
                keys++;
#if defined(__EMSCRIPTEN__)
                web_key(ev.type == SDL_EVENT_KEY_DOWN, (double)(t_in - perf_off) * 1e-6);
#endif
            }
        }
        t_in = yrt_now_ns();
        if (clear) {
            int x = (int)((f.index * 4) % (caps.mode.w > 40 ? caps.mode.w - 40 : 1));
            clear_color(0.5f, 0.5f, 0.5f, 1.0f);
            clear(0x4000u);                       /* GL_COLOR_BUFFER_BIT */
            enable(0x0C11u);                      /* GL_SCISSOR_TEST */
            scissor(x, 0, 40, caps.mode.h);
            clear_color(0.9f, 0.9f, 0.9f, 1.0f);
            clear(0x4000u);
            disable(0x0C11u);
        }
        if (load_ms > 0) {
            int64_t until = yrt_now_ns() + (int64_t)(load_ms * 1e6);
            while (yrt_now_ns() < until) { }
        }
        if (yscr_flip(&scr) < 0) { fprintf(stderr, "web_selftest: %s\n", yscr_error(&scr)); break; }
        t_out = yrt_now_ns();
        if (n < MAX_FRAMES) cost[n] = t_out - t_in;
        if (++n >= frames) break;
    }
    yscr_wait_flip(&scr, NULL);
    yscr_describe(&scr, line, sizeof line);
    yscr_close(&scr);

    qsort(iv, (size_t)n_iv, sizeof iv[0], cmp64);
    qsort(cost, (size_t)(n < MAX_FRAMES ? n : MAX_FRAMES), sizeof cost[0], cmp64);
    qsort(resume, (size_t)n_res, sizeof resume[0], cmp64);
    {
        double m = 0, v = 0;
        size_t k;
        int b, nc = n < MAX_FRAMES ? n : MAX_FRAMES;
        for (i = 0; i < n_iv; i++) m += (double)iv[i];
        m = n_iv ? m / n_iv : 0;
        for (i = 0; i < n_iv; i++) v += ((double)iv[i] - m) * ((double)iv[i] - m);
        v = n_iv > 1 ? sqrt(v / (n_iv - 1)) : 0;
        printf("frames %d, intervals %d: mean %.4f ms, sd %.4f ms, min %.4f, p1 %.4f, p50 %.4f, p99 %.4f, max %.4f ms\n",
               n, n_iv, m * 1e-6, v * 1e-6, n_iv ? (double)iv[0] * 1e-6 : 0, pct(iv, n_iv, 0.01) * 1e-6,
               pct(iv, n_iv, 0.5) * 1e-6, pct(iv, n_iv, 0.99) * 1e-6, n_iv ? (double)iv[n_iv - 1] * 1e-6 : 0);
        printf("dropped %lld, late %lld, early %lld; frame cost p50 %.3f ms p99 %.3f ms; resume p50 %.3f ms p99 %.3f ms; "
               "clock step %lld ns; keys %lld%s\n", (long long)dropped, (long long)late, (long long)early, pct(cost, nc, 0.5) * 1e-6,
               pct(cost, nc, 0.99) * 1e-6, pct(resume, n_res, 0.5) * 1e-6, pct(resume, n_res, 0.99) * 1e-6,
               (long long)ci.resolution_ns, (long long)keys, aborted ? " (stopped)" : "");
        printf("%s\n", line);
        k = (size_t)snprintf(json, sizeof json,
                             "{\"frames\":%d,\"intervals\":%d,\"mean_ms\":%.5f,\"sd_ms\":%.5f,\"min_ms\":%.4f,\"p1_ms\":%.4f,"
                             "\"p50_ms\":%.4f,\"p99_ms\":%.4f,\"max_ms\":%.4f,\"dropped\":%lld,\"late\":%lld,\"early\":%lld,"
                             "\"cost_p50_ms\":%.4f,\"cost_p99_ms\":%.4f,\"resume_p50_ms\":%.4f,\"resume_p99_ms\":%.4f,"
                             "\"clock_step_ns\":%lld,\"load_ms\":%.3f,\"stopped\":%d,\"bin_ms\":0.25,\"hist\":[",
                             n, n_iv, m * 1e-6, v * 1e-6, n_iv ? (double)iv[0] * 1e-6 : 0, pct(iv, n_iv, 0.01) * 1e-6,
                             pct(iv, n_iv, 0.5) * 1e-6, pct(iv, n_iv, 0.99) * 1e-6,
                             n_iv ? (double)iv[n_iv - 1] * 1e-6 : 0, (long long)dropped, (long long)late, (long long)early,
                             pct(cost, nc, 0.5) * 1e-6, pct(cost, nc, 0.99) * 1e-6, pct(resume, n_res, 0.5) * 1e-6,
                             pct(resume, n_res, 0.99) * 1e-6, (long long)ci.resolution_ns, load_ms, aborted);
        for (b = 0; b <= BINS && k < sizeof json - 16; b++)
            k += (size_t)snprintf(json + k, sizeof json - k, "%s%u", b ? "," : "", (unsigned)hist[b]);
        if (k < sizeof json - 1600) {
            char esc[1536];
            size_t j = 0, q;
            for (q = 0; line[q] && j < sizeof esc - 2; q++) {   /* the describe line, JSON-escaped */
                if (line[q] == '"' || line[q] == '\\') esc[j++] = '\\';
                esc[j++] = line[q];
            }
            esc[j] = 0;
            snprintf(json + k, sizeof json - k, "],\"describe\":\"%s\"}", esc);
        }
#if defined(__EMSCRIPTEN__)
        web_result(json);
#endif
    }
    if (sim && (n != frames || n_iv < frames - 3)) {
        fprintf(stderr, "web_selftest: --sim: %d frames, %d intervals\n", n, n_iv);
        return 1;
    }
    return 0;
}
