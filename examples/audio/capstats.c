/* audio_capstats.c - record ysp/audio.h's capture clock and blocks on this
 * machine: callback timing, position reports, the fit and its quality.
 * Records the input; plays nothing (with --duplex, silence).
 *
 *     audio_capstats [--null] [--device NAME] [--exclusive] [--s16] [--period N]
 *                    [--seconds S] [--load N] [--duplex] [--ring S]
 *                    [--stall-at S --stall MS [--stall-every S]] [--csv PREFIX]
 *
 *   --device NAME  the input whose name contains NAME (default: the OS's)
 *   --exclusive    WASAPI exclusive mode; at most 30 s
 *   --s16          accept a 16-bit input (the default asks for 24 bits or
 *                  float: STRICT OPEN)
 *   --period N     frames per block asked for (default: the header's)
 *   --seconds S    run length (default 10)
 *   --load N       N threads spin at normal priority
 *   --duplex       open the default output with desc.capture: one device
 *                  thread plays (silence) and records (DUPLEX)
 *   --ring S       capture ring in seconds (default 2)
 *   --stall-at S --stall MS  once, S seconds in, the capture block sleeps MS
 *                  ms: an injected overrun
 *   --stall-every S  and again every S seconds after that
 *   --csv PREFIX   PREFIX_blocks.csv, one row per block
 *
 * The main thread is reader 0: it reads the ring every 10 ms and keeps the
 * peak and RMS levels. Prints the describe line and: block intervals and
 * sizes; the backlog (position minus the frame after the block) and the
 * age of each report's stamp; every block's lateness against the header's
 * final fit (the frame after the block reaching the callback, against the
 * fit's time for it); the reports' residuals against the same fit; the
 * cost of yau_capture_block; drops, losses and the levels.
 *
 * Exit: 0 when the run completed, 1 when the input did not open, 2 usage.
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "ysp/rt.h"

typedef struct cs_blk {
    int64_t t_entry, pos, pos_t, f, t_end;
    int32_t n;
} cs_blk;

#define CS_MAX 400000
static cs_blk g_blk[CS_MAX];
static volatile long g_nblk = 0;
static int64_t g_stall_at = 0, g_stall_ns = 0, g_stall_every = 0;
static long g_stalls = 0;

static void cs_on_capture(const void* c, const void* tk, int32_t n);
static void cs_on_capture_end(void);
#define YAU__ON_CAPTURE(c, tk, frames) cs_on_capture((const void*)(c), (const void*)(tk), (frames))
#define YAU__ON_CAPTURE_END(c) cs_on_capture_end()

#define YSP_AUDIO_IMPLEMENTATION
#include "ysp/audio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#if defined(_WIN32)
#include <process.h>
#else
#include <pthread.h>
#endif

static void cs_on_capture(const void* cp, const void* tkp, int32_t n) {
    const yau_capture* c = (const yau_capture*)cp;
    const yau_tick* tk = (const yau_tick*)tkp;
    long i = g_nblk;
    if (g_stall_ns && tk->t_entry >= g_stall_at) {
        int64_t ns = g_stall_ns;
        if (g_stall_every) g_stall_at += g_stall_every; else g_stall_ns = 0;
        g_stalls++;
        yrt_sleep_until(yrt_now_ns() + ns, 0);
    }
    if (i >= CS_MAX) return;
    g_blk[i].t_entry = tk->t_entry;
    g_blk[i].pos = tk->pos;
    g_blk[i].pos_t = tk->pos_t;
    g_blk[i].f = c->f;
    g_blk[i].n = n;
}
static void cs_on_capture_end(void) {
    long i = g_nblk;
    if (i >= CS_MAX) return;
    g_blk[i].t_end = yrt_now_ns();
    g_nblk = i + 1;
}

static volatile int g_stop = 0;
static volatile unsigned long g_sink = 0;
#if defined(_WIN32)
static unsigned __stdcall spin(void* p) { (void)p; while (!g_stop) g_sink++; return 0; }
#else
static void* spin(void* p) { (void)p; while (!g_stop) g_sink++; return NULL; }
#endif

static int cmp_i64(const void* a, const void* b) {
    int64_t x = *(const int64_t*)a, y = *(const int64_t*)b;
    return x < y ? -1 : x > y;
}
static int64_t pct(int64_t* v, long n, double p) {
    return n <= 0 ? 0 : v[(long)(p * (double)(n - 1) + 0.5)];
}
static void summary(const char* what, int64_t* v, long m, double scale, const char* unit) {
    if (m <= 0) { printf("%s: none\n", what); return; }
    qsort(v, (size_t)m, sizeof v[0], cmp_i64);
    printf("%s %s: min %.1f p1 %.1f p50 %.1f p99 %.1f p99.9 %.1f max %.1f (%ld)\n", what, unit, (double)v[0] / scale,
           (double)pct(v, m, 0.01) / scale, (double)pct(v, m, 0.5) / scale, (double)pct(v, m, 0.99) / scale,
           (double)pct(v, m, 0.999) / scale, (double)v[m - 1] / scale, m);
}

static yau_capture g_cap;
static yau_audio g_au;
static unsigned char g_ring_mem[YRT_RING_BYTES(65536)];
static yrt_ring g_ring;
static yrt_event g_ev[1024];
static int64_t g_tmp[CS_MAX];
static float g_buf[48000 * 16];

int main(int argc, char** argv) {
    yau_capture_desc cd;
    const char* csv = NULL;
    double seconds = 10, stall_at = 0, ring_s = 2;
    int i, load = 0, duplex = 0, nxr = 0, ndrop = 0;
    long n;
    char line[640];
    double peak = 0, sumsq = 0, nsamp = 0;
    int64_t read_total = 0;
    memset(&cd, 0, sizeof cd);
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--null")) cd.backend = YAU_BACKEND_NULL;
        else if (!strcmp(argv[i], "--device") && i + 1 < argc) cd.device = argv[++i];
        else if (!strcmp(argv[i], "--exclusive")) cd.exclusive = true;
        else if (!strcmp(argv[i], "--s16")) cd.format.sample = YAU_S16;
        else if (!strcmp(argv[i], "--period") && i + 1 < argc) cd.period = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "--load") && i + 1 < argc) load = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--duplex")) duplex = 1;
        else if (!strcmp(argv[i], "--ring") && i + 1 < argc) ring_s = atof(argv[++i]);
        else if (!strcmp(argv[i], "--stall-at") && i + 1 < argc) stall_at = atof(argv[++i]);
        else if (!strcmp(argv[i], "--stall") && i + 1 < argc) g_stall_ns = (int64_t)(atof(argv[++i]) * 1e6);
        else if (!strcmp(argv[i], "--stall-every") && i + 1 < argc) g_stall_every = (int64_t)(atof(argv[++i]) * 1e9);
        else if (!strcmp(argv[i], "--csv") && i + 1 < argc) csv = argv[++i];
        else { fprintf(stderr, "audio_capstats: unknown argument %s (see the top of the file)\n", argv[i]); return 2; }
    }
    if (seconds <= 0 || seconds > 660 || load < 0 || load > 32 || ring_s < 0.1 || ring_s > 20
        || (cd.exclusive && seconds > 30)) {
        fprintf(stderr, "audio_capstats: bad --seconds (exclusive: at most 30), --load or --ring\n");
        return 2;
    }
    yrt_ring_open(&g_ring, &(yrt_ring_desc){ .memory = g_ring_mem, .bytes = sizeof g_ring_mem });
    cd.ring = &g_ring;
    cd.frames = (int64_t)(ring_s * 48000);
    g_stall_at = yrt_now_ns() + (int64_t)(stall_at * 1e9);
    if (duplex) {
        yau_desc d;
        memset(&d, 0, sizeof d);
        d.backend = cd.backend;
        d.exclusive = cd.exclusive;
        d.period = cd.period;
        d.arena_bytes = 1 << 16;
        d.capture = &g_cap;
        d.capture_desc = &cd;
        if (!yau_open(&g_au, &d)) { fprintf(stderr, "%s\n", yau_error(&g_au)); return 1; }
    } else if (!yau_capture_open(&g_cap, &cd)) {
        fprintf(stderr, "%s\n", yau_capture_error(&g_cap));
        return 1;
    }
    for (i = 0; i < load; i++) {
#if defined(_WIN32)
        _beginthreadex(NULL, 0, spin, NULL, 0, NULL);
#else
        pthread_t th; pthread_create(&th, NULL, spin, NULL); pthread_detach(th);
#endif
    }
    {
        int64_t t_end = yrt_now_ns() + (int64_t)(seconds * 1e9);
        int C = g_cap.dcaps.channels;
        while (yrt_now_ns() < t_end) {
            int64_t got, first;
            int k;
            yrt_sleep_until(yrt_now_ns() + 10000000, 0);
            if (duplex) (void)yau_update(&g_au);
            while ((got = yau_capture_read(&g_cap, 0, g_buf, (int64_t)(sizeof g_buf / sizeof g_buf[0]) / C, &first)) > 0) {
                int64_t j;
                for (j = 0; j < got * C; j++) {
                    double a = fabs((double)g_buf[j]);
                    if (a > peak) peak = a;
                    sumsq += a * a;
                }
                nsamp += (double)(got * C);
                read_total += got;
            }
            while ((k = yrt_ring_drain(&g_ring, g_ev, 1024)) > 0) {
                int j;
                for (j = 0; j < k; j++) {
                    if (g_ev[j].source != YRT_SRC_AUDIO) continue;
                    if (g_ev[j].kind == YAU_EV_CAP_XRUN) {
                        nxr++;
                        printf("capture xrun at frame %lld: %lld frames lost%s\n", (long long)g_ev[j].u.i64[0],
                               (long long)g_ev[j].u.i64[1], g_ev[j].u.i64[1] ? "" : " (count not known; fit restarted)");
                    } else if (g_ev[j].kind == YAU_EV_CAP_DROP) {
                        ndrop++;
                    }
                }
            }
        }
    }
    (void)yau_capture_update(&g_cap);
    yau_capture_describe(&g_cap, line, sizeof line);
    g_stop = 1;
    puts(line);
    if (duplex) {
        char pl[512];
        yau_describe(&g_au, pl, sizeof pl);
        puts(pl);
    }
    n = g_nblk;
    if (n > 2) {
        long m = 0;
        int32_t nmin = INT32_MAX, nmax = 0;
        for (i = 1; i < n; i++) g_tmp[m++] = g_blk[i].t_entry - g_blk[i - 1].t_entry;
        summary("block interval", g_tmp, m, 1e3, "us");
        for (i = 0; i < n; i++) { if (g_blk[i].n < nmin) nmin = g_blk[i].n; if (g_blk[i].n > nmax) nmax = g_blk[i].n; }
        printf("frames per block %d..%d, %ld blocks\n", (int)nmin, (int)nmax, n);
        m = 0;
        for (i = 0; i < n; i++) if (g_blk[i].pos >= 0) g_tmp[m++] = g_blk[i].pos - (g_blk[i].f + g_blk[i].n);
        if (m) summary("backlog (position - frame after the block)", g_tmp, m, 1, "frames");
        m = 0;
        for (i = 0; i < n; i++) if (g_blk[i].pos >= 0) g_tmp[m++] = g_blk[i].t_entry - g_blk[i].pos_t;
        if (m) summary("report stamp age at the block", g_tmp, m, 1e3, "us");
        m = 0;
        for (i = 0; i < n; i++) {
            int64_t t = yau_capture_time_of(&g_cap, g_blk[i].f + g_blk[i].n) - g_cap.desc.offset_ns;
            if (t > 0) g_tmp[m++] = g_blk[i].t_entry - t;
        }
        summary("block lateness against the final fit (entry - fit time of the frame after it)", g_tmp, m, 1e3, "us");
        m = 0;
        for (i = 1; i < n; i++)
            if (g_blk[i].pos >= 0 && g_blk[i].pos_t != g_blk[i - 1].pos_t) {
                int64_t t = yau_capture_time_of(&g_cap, g_blk[i].pos) - g_cap.desc.offset_ns;
                if (t > 0) g_tmp[m++] = g_blk[i].pos_t - t;
            }
        if (m) summary("report residual against the final fit (new stamps only)", g_tmp, m, 1e3, "us");
        m = 0;
        for (i = 0; i < n; i++) g_tmp[m++] = g_blk[i].t_end - g_blk[i].t_entry;
        {
            double mean = 0;
            int j;
            for (j = 0; j < m; j++) mean += (double)g_tmp[j];
            printf("yau_capture_block cost mean %.2f us\n", mean / (double)m / 1e3);
        }
        summary("yau_capture_block cost (with the report)", g_tmp, m, 1e3, "us");
    }
    {
        yau_capture_info in;
        yau_capture_get_info(&g_cap, &in);
        printf("read %lld frames of %lld; dropped %lld in %u runs (%d records); lost %lld; xruns %u (%d records); "
               "set aside %u\n", (long long)read_total, (long long)in.frames, (long long)in.dropped, (unsigned)in.drops,
               ndrop, (long long)in.lost, (unsigned)in.xruns, nxr, (unsigned)in.set_aside);
        printf("level: peak %.1f dBFS, rms %.1f dBFS over %d channels\n", peak > 0 ? 20 * log10(peak) : -999.0,
               nsamp > 0 && sumsq > 0 ? 10 * log10(sumsq / nsamp) : -999.0, (int)in.channels);
    }
    if (g_stalls) printf("stalls injected %ld, %.1f ms each\n", g_stalls, (double)g_stall_ns / 1e6);
    if (csv) {
        char path[512];
        FILE* f;
        snprintf(path, sizeof path, "%s_blocks.csv", csv);
        f = fopen(path, "w");
        if (f) {
            fprintf(f, "t_entry,frames,frame,pos,pos_t,cost_ns\n");
            for (i = 0; i < n; i++)
                fprintf(f, "%lld,%d,%lld,%lld,%lld,%lld\n", (long long)g_blk[i].t_entry, (int)g_blk[i].n,
                        (long long)g_blk[i].f, (long long)g_blk[i].pos, (long long)g_blk[i].pos_t,
                        (long long)(g_blk[i].t_end - g_blk[i].t_entry));
            fclose(f);
        }
    }
    if (duplex) yau_close(&g_au);
    else yau_capture_close(&g_cap);
    return 0;
}
