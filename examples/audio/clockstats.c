/* audio_clockstats.c - record ysp/audio.h's device clock and callback on this
 * machine. Plays silence only.
 *
 *     audio_clockstats [--null] [--device NAME] [--exclusive [--long]]
 *                      [--period N] [--queue Q] [--seconds S] [--load N] [--frame-work MS]
 *                      [--voices N] [--streams N [--wav PATH] [--ring S]]
 *                      [--stall-at S --stall MS [--stall-every S]]
 *                      [--allocs | --allocs-control] [--csv PREFIX]
 *
 *   --device NAME  the playback device whose name contains NAME
 *   --exclusive    WASAPI exclusive mode; at most 30 s, because every other
 *                  program is silent meanwhile, unless --long is given
 *   --period N     frames per callback asked for (default: the header's)
 *   --queue Q      desc.queue: WASAPI shared periods queued ahead of the
 *                  engine (1 or 2; default 2)
 *   --seconds S    run length (default 10)
 *   --load N       N threads spin at normal priority
 *   --frame-work MS  a 60 Hz frame loop on the main thread (elevated), which
 *                  spins MS ms per frame and hands a silent 10 ms sound to
 *                  yau_play_at() each frame; its yau cost is measured
 *   --voices N     N silent sounds looping for the whole run (mixer cost)
 *   --streams N    N stereo streams (STREAMS) playing for the whole run, fed
 *                  silence in 4096-frame steps by one producer thread
 *   --wav PATH     the streams play this WAV file instead (looped), fed by
 *                  one yrt_pump (yau_wav_step), woken by the frame loop
 *   --ring S       ring of each stream in seconds (default 1)
 *   --stall-at S --stall MS  once, S seconds in, the callback sleeps MS ms:
 *                  an injected underrun
 *   --stall-every S  and again every S seconds after that
 *   --allocs       count C runtime heap calls after the warm-up (MSVC debug
 *                  build) and miniaudio's own; --allocs-control adds one
 *                  malloc per callback, which the count must see
 *   --csv PREFIX   PREFIX_ticks.csv (one row per callback) and
 *                  PREFIX_ring.csv (every ring record)
 *
 * Prints the describe line and a summary: callback intervals, underruns,
 * the queue (stream frames written minus the device position, at callback
 * entry),
 * GetPosition and render cost, the fit, the frame thread's cost, every
 * XRUN record (a halt found from late reports gives its length), and with
 * streams their gaps and the lowest ring fill the frame loop saw.
 *
 * Exit: 0 when the run completed, 1 when the device did not open, 2 usage.
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "ysp/rt.h"

typedef struct cs_tick {
    int64_t t_entry, pos, pos_t, w, t_hook, t_end;
} cs_tick;

#define CS_MAX 400000
static cs_tick g_ticks[CS_MAX];
static volatile long g_nticks = 0;
static int g_allocs_control = 0;

static void cs_on_tick(const void* au, const void* tk);
static void cs_on_end(void);
#define YAU__ON_TICK(au, tk) cs_on_tick((const void*)(au), (const void*)(tk))
#define YAU__ON_RENDER_END(au) cs_on_end()

#define YSP_AUDIO_IMPLEMENTATION
#include "ysp/audio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <crtdbg.h>
#include <process.h>
#else
#include <pthread.h>
#endif

static void cs_on_tick(const void* aup, const void* tkp) {
    const yau_audio* au = (const yau_audio*)aup;
    const yau_tick* tk = (const yau_tick*)tkp;
    long i = g_nticks;
    if (i >= CS_MAX) return;
    g_ticks[i].t_entry = tk->t_entry;
    g_ticks[i].pos = tk->pos;
    g_ticks[i].pos_t = tk->pos_t;
    g_ticks[i].w = au->w;
    g_ticks[i].t_hook = yrt_now_ns();
}
static void cs_on_end(void) {
    long i = g_nticks;
    if (i >= CS_MAX) return;
    g_ticks[i].t_end = yrt_now_ns();
    g_nticks = i + 1;
    if (g_allocs_control) free(malloc(16));
}

/* --- the injected stall, as an Audio source ---------------------------------- */
static int64_t g_stall_at = 0, g_stall_ns = 0, g_stall_every = 0;
static long g_stalls = 0;
static int64_t g_stall_req = 0;   /* for the summary */
static void stall_render(void* ctx, float* out, int32_t frames, const yau_clock* clk) {
    (void)ctx; (void)out; (void)frames; (void)clk;
    if (g_stall_ns && yrt_now_ns() >= g_stall_at) {
        int64_t ns = g_stall_ns;
        if (g_stall_every) g_stall_at += g_stall_every; else g_stall_ns = 0;
        g_stalls++;
        yrt_sleep_until(yrt_now_ns() + ns, 0);
    }
}
static const yau_source g_stall_src = { YAU_SOURCE_VERSION, "stall", stall_render };

/* --- load ---------------------------------------------------------------------- */
static volatile int g_stop = 0;
static volatile unsigned long g_sink = 0;
#if defined(_WIN32)
static unsigned __stdcall spin(void* p) { (void)p; while (!g_stop) g_sink++; return 0; }
#else
static void* spin(void* p) { (void)p; while (!g_stop) g_sink++; return NULL; }
#endif

/* --- allocation hook --------------------------------------------------------------- */
#if defined(_WIN32) && defined(_DEBUG)
static volatile long g_crt = 0;
static int g_counting = 0;
static int __cdecl alloc_hook(int type, void* d, size_t n, int bt, long r, const unsigned char* f, int l) {
    (void)type; (void)d; (void)n; (void)bt; (void)r; (void)f; (void)l;
    if (g_counting) InterlockedIncrement(&g_crt);
    return 1;
}
#endif

static int cmp_i64(const void* a, const void* b) {
    int64_t x = *(const int64_t*)a, y = *(const int64_t*)b;
    return x < y ? -1 : x > y;
}
static int64_t pct(int64_t* v, long n, double p) {
    long k;
    if (n <= 0) return 0;
    k = (long)(p * (double)(n - 1) + 0.5);
    return v[k];
}

static yau_audio au;
static unsigned char g_ring_mem[YRT_RING_BYTES(65536)];
static yrt_ring g_ring;
static yrt_event g_ev[1024];
static int64_t g_tmp[CS_MAX];
static float g_silence[48000 * 2];
static int64_t g_frame_cost[60 * 700];
#define CS_XR 256
static yrt_event g_xr[CS_XR];
static int n_xr = 0;

/* --- streams ------------------------------------------------------------------------- */
#define CS_STREAMS 8
static int g_nstreams = 0;
static double g_ring_s = 1.0;
#if YAU_VERSION_MINOR >= 2
static yau_stream g_st[CS_STREAMS];
static yau_wav g_wav[CS_STREAMS];
static float* g_ringmem[CS_STREAMS];
static const char* g_wav_path = NULL;
static yrt_pump g_pump;
static yau_stream* cs_stream(int i) { return g_wav_path ? &g_wav[i].stream : &g_st[i]; }
/* the producer of silence: acquire, zero, commit; then sleep 5 ms */
#if defined(_WIN32)
static unsigned __stdcall producer(void* p)
#else
static void* producer(void* p)
#endif
{
    (void)p;
    while (!g_stop) {
        int i;
        for (i = 0; i < g_nstreams; i++) {
            int64_t n;
            float* f;
            while ((f = yau_stream_acquire(&g_st[i], &n)) != NULL) {
                if (n > 4096) n = 4096;
                memset(f, 0, sizeof(float) * 2 * (size_t)n);
                yau_stream_commit(&g_st[i], n);
            }
        }
        yrt_sleep_until(yrt_now_ns() + 5000000, 0);
    }
#if defined(_WIN32)
    return 0;
#else
    return NULL;
#endif
}
/* one pump feeds every WAV stream, a block each in turn */
static bool cs_wav_step(void* ctx) {
    int i;
    bool more = false;
    (void)ctx;
    for (i = 0; i < g_nstreams; i++) more = yau_wav_step(&g_wav[i]) || more;
    return more;
}
static void cs_wav_msg(void* ctx, const void* msg, uint32_t seq) { (void)ctx; (void)msg; (void)seq; }
#endif

/* Once a frame: the lowest fill seen, and the pump woken when a WAV ring is
 * below half (inside the frame thread's timed region with --frame-work). */
#if YAU_VERSION_MINOR >= 2
static int64_t g_low_fill = INT64_MAX;
#endif
static void cs_streams_frame(void) {
#if YAU_VERSION_MINOR >= 2
    int i, wake = 0;
    for (i = 0; i < g_nstreams; i++) {
        yau_stream_info in;
        yau_stream_get_info(cs_stream(i), &in);
        if (in.started && in.fill < g_low_fill) g_low_fill = in.fill;
        if (g_wav_path && yau_wav_wants(&g_wav[i])) wake = 1;
    }
    if (wake) {
        int64_t m = -1;
        (void)yrt_pump_submit(&g_pump, &m);
    }
#endif
}

int main(int argc, char** argv) {
    yau_desc d;
    const char* csv = NULL;
    double seconds = 10, stall_at = 0, frame_work = -1;
    int i, load = 0, voices = 0, allocs = 0, long_excl = 0;
    long n, nframes = 0;
    char line[512];
    FILE* fr = NULL;
    uint32_t ma0 = 0;
    memset(&d, 0, sizeof d);
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--null")) d.backend = YAU_BACKEND_NULL;
        else if (!strcmp(argv[i], "--device") && i + 1 < argc) d.device = argv[++i];
        else if (!strcmp(argv[i], "--exclusive")) d.exclusive = true;
        else if (!strcmp(argv[i], "--long")) long_excl = 1;
        else if (!strcmp(argv[i], "--queue") && i + 1 < argc) d.queue = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--period") && i + 1 < argc) d.period = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "--load") && i + 1 < argc) load = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--frame-work") && i + 1 < argc) frame_work = atof(argv[++i]);
        else if (!strcmp(argv[i], "--voices") && i + 1 < argc) voices = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--streams") && i + 1 < argc) g_nstreams = atoi(argv[++i]);
#if YAU_VERSION_MINOR >= 2
        else if (!strcmp(argv[i], "--wav") && i + 1 < argc) g_wav_path = argv[++i];
#endif
        else if (!strcmp(argv[i], "--ring") && i + 1 < argc) g_ring_s = atof(argv[++i]);
        else if (!strcmp(argv[i], "--stall-at") && i + 1 < argc) stall_at = atof(argv[++i]);
        else if (!strcmp(argv[i], "--stall") && i + 1 < argc) g_stall_req = g_stall_ns = (int64_t)(atof(argv[++i]) * 1e6);
        else if (!strcmp(argv[i], "--stall-every") && i + 1 < argc) g_stall_every = (int64_t)(atof(argv[++i]) * 1e9);
        else if (!strcmp(argv[i], "--allocs")) allocs = 1;
        else if (!strcmp(argv[i], "--allocs-control")) { allocs = 1; g_allocs_control = 1; }
        else if (!strcmp(argv[i], "--csv") && i + 1 < argc) csv = argv[++i];
        else { fprintf(stderr, "audio_clockstats: unknown argument %s (see the top of the file)\n", argv[i]); return 2; }
    }
    if (d.exclusive && seconds > 30 && !long_excl) { fprintf(stderr, "audio_clockstats: --exclusive runs at most 30 s without --long\n"); return 2; }
    if (seconds <= 0 || seconds > 660 || voices < 0 || voices > 32 || load < 0 || load > 32) { fprintf(stderr, "audio_clockstats: bad --seconds, --voices or --load\n"); return 2; }
#if YAU_VERSION_MINOR >= 2
    if (g_nstreams < 0 || g_nstreams > CS_STREAMS || g_ring_s < 0.05 || g_ring_s > 4) { fprintf(stderr, "audio_clockstats: --streams 0..%d, --ring 0.05..4\n", CS_STREAMS); return 2; }
    for (i = 0; i < g_nstreams; i++) {
        /* before open: the ring memory is the run's, not the arena's */
        g_ringmem[i] = (float*)calloc((size_t)(g_ring_s * 192000.0) * 2, sizeof(float));
        if (!g_ringmem[i]) return 2;
    }
#else
    if (g_nstreams) { fprintf(stderr, "audio_clockstats: --streams needs ysp/audio.h 0.2\n"); return 2; }
#endif
    yrt_ring_open(&g_ring, &(yrt_ring_desc){ .memory = g_ring_mem, .bytes = sizeof g_ring_mem });
    d.ring = &g_ring;
    d.arena_bytes = 1 << 16;
    if (voices + g_nstreams > 32) d.voices = voices + g_nstreams;   /* a stream takes a voice */
    if (allocs) { static float arena[1 << 14]; d.arena = arena; d.arena_bytes = sizeof arena; }
    if (g_stall_ns) {
        d.source = &g_stall_src;
        g_stall_at = yrt_now_ns() + (int64_t)(stall_at * 1e9);   /* from before open */
    }
    else g_stall_ns = 0;
#if defined(_WIN32) && defined(_DEBUG)
    /* open with the caller's arena: the C runtime calls during open, minus
     * miniaudio's own (it allocates through the header's callbacks), are
     * the header's */
    if (allocs) { _CrtSetAllocHook(alloc_hook); g_counting = 1; }
#endif
    if (!yau_open(&au, &d)) { fprintf(stderr, "%s\n", yau_error(&au)); return 1; }
#if defined(_WIN32) && defined(_DEBUG)
    if (allocs) {
        g_counting = 0;
        printf("allocs at open: C runtime %ld, of which miniaudio %u\n", (long)g_crt,
               (unsigned)((yau__ma*)(void*)au.backend_mem)->allocs);
        g_crt = 0;
    }
#endif
    for (i = 0; i < voices; i++) {
        yau_play_desc pd;
        memset(&pd, 0, sizeof pd);
        pd.buf.frames = g_silence; pd.buf.n = 48000; pd.buf.channels = 2;
        pd.loops = YAU_FOREVER;
        if (yau_play(&au, &pd) <= 0) { fprintf(stderr, "voice %d refused\n", i); }
    }
#if YAU_VERSION_MINOR >= 2
    for (i = 0; i < g_nstreams; i++) {
        int64_t frames = (int64_t)(g_ring_s * (double)au.dcaps.rate);
        if (g_wav_path) {
            yau_wav_desc wd;
            memset(&wd, 0, sizeof wd);
            wd.path = g_wav_path; wd.loops = YAU_FOREVER; wd.ring = frames; wd.memory = g_ringmem[i];
            if (!yau_wav_open(&au, &g_wav[i], &wd)) { fprintf(stderr, "%s\n", yau_wav_error(&g_wav[i])); return 1; }
            (void)yau_wav_feed(&g_wav[i], 0);
        } else {
            yau_stream_desc sd;
            int64_t nf;
            float* f;
            memset(&sd, 0, sizeof sd);
            sd.memory = g_ringmem[i]; sd.frames = frames;
            if (!yau_stream_open(&au, &g_st[i], &sd)) { fprintf(stderr, "%s\n", yau_error(&au)); return 1; }
            while ((f = yau_stream_acquire(&g_st[i], &nf)) != NULL) yau_stream_commit(&g_st[i], nf);
        }
        if (yau_play_stream(&au, cs_stream(i), 0) <= 0) fprintf(stderr, "stream %d refused\n", i);
    }
    if (g_nstreams && g_wav_path) {
        yrt_pump_desc pd;
        memset(&pd, 0, sizeof pd);
        pd.msg_size = 8; pd.capacity = 4; pd.on_msg = cs_wav_msg; pd.on_idle = cs_wav_step;
        if (!yrt_pump_open(&g_pump, &pd)) { fprintf(stderr, "%s\n", yrt_pump_error(&g_pump)); return 1; }
    } else if (g_nstreams) {
#if defined(_WIN32)
        _beginthreadex(NULL, 0, producer, NULL, 0, NULL);
#else
        pthread_t th; pthread_create(&th, NULL, producer, NULL); pthread_detach(th);
#endif
        /* the thread's own setup (the C runtime's, ysp_rt's timer) before
         * --allocs starts counting */
        yrt_sleep_until(yrt_now_ns() + 100000000, 0);
    }
#endif
    for (i = 0; i < load; i++) {
#if defined(_WIN32)
        _beginthreadex(NULL, 0, spin, NULL, 0, NULL);
#else
        pthread_t th; pthread_create(&th, NULL, spin, NULL); pthread_detach(th);
#endif
    }
    if (csv) {
        char path[512];
        snprintf(path, sizeof path, "%s_ring.csv", csv);
        fr = fopen(path, "w");
        if (fr) fprintf(fr, "source,kind,t_ns,aux,i0,i1,i2,i3,f1,f2,u6,u7,u16_18,u16_19\n");
    }
    {
        int64_t t_end = yrt_now_ns() + (int64_t)(seconds * 1e9);
        int64_t next = yrt_now_ns();
        yrt_policy pol = frame_work >= 0 ? yrt_thread_elevate(NULL) : YRT_POLICY_NONE;
        (void)pol;
        if (allocs) {
            ma0 = ((yau__ma*)(void*)au.backend_mem)->allocs;
#if defined(_WIN32) && defined(_DEBUG)
            _CrtSetAllocHook(alloc_hook);
            g_counting = 1;
#endif
        }
        while (yrt_now_ns() < t_end) {
            int k;
            next += 16666667;
            yrt_sleep_until(next, YRT_DEFAULT_SPIN_NS);
            if (frame_work >= 0) {
                int64_t a = yrt_now_ns(), b;
                yau_buf sb;
                memset(&sb, 0, sizeof sb);
                sb.frames = g_silence; sb.n = 480; sb.channels = 2;
                (void)yau_play_at(&au, sb, a + yau_lead_ns(&au) + YRT_MS(20));
                (void)yau_update(&au);
                cs_streams_frame();
                b = yrt_now_ns();
                if (nframes < (long)(sizeof g_frame_cost / sizeof g_frame_cost[0])) g_frame_cost[nframes++] = b - a;
                if (frame_work > 0) yrt_spin_until((int64_t)(a + (int64_t)(frame_work * 1e6)));
            } else {
                (void)yau_update(&au);
                cs_streams_frame();
            }
            while ((k = yrt_ring_drain(&g_ring, g_ev, 1024)) > 0) {
                int j;
                for (j = 0; j < k; j++) {
                    const yrt_event* e = &g_ev[j];
                    if (e->source == YRT_SRC_AUDIO && e->kind == YAU_EV_XRUN && n_xr < CS_XR)
                        g_xr[n_xr++] = *e;
                }
                if (!fr) continue;
                for (j = 0; j < k; j++) {
                    const yrt_event* e = &g_ev[j];
                    fprintf(fr, "%u,%u,%llu,%u,%lld,%lld,%lld,%lld,%.9f,%.3f,%u,%u,%u,%u\n",
                            (unsigned)e->source, (unsigned)e->kind, (unsigned long long)e->t_ns, (unsigned)e->aux,
                            (long long)e->u.i64[0], (long long)e->u.i64[1], (long long)e->u.i64[2], (long long)e->u.i64[3],
                            e->u.f64[1], e->u.f64[2], (unsigned)e->u.u32[6], (unsigned)e->u.u32[7],
                            (unsigned)e->u.u16[18], (unsigned)e->u.u16[19]);
                }
            }
        }
#if defined(_WIN32) && defined(_DEBUG)
        g_counting = 0;
#endif
    }
    yau_describe(&au, line, sizeof line);
    g_stop = 1;
#if YAU_VERSION_MINOR >= 2
    if (g_nstreams && g_wav_path) yrt_pump_close(&g_pump);
    if (g_nstreams) {
        yau_caps c;
        yau_get_caps(&au, &c);
        printf("streams %d (%s, ring %.2f s): gaps %u, lowest fill %.1f ms\n", g_nstreams,
               g_wav_path ? "wav on a pump" : "silence from a thread", g_ring_s, (unsigned)c.gaps,
               g_low_fill == INT64_MAX ? -1.0 : (double)g_low_fill * 1e3 / (double)c.rate);
    }
#endif
    if (allocs) {
        uint32_t ma1 = ((yau__ma*)(void*)au.backend_mem)->allocs;
#if defined(_WIN32) && defined(_DEBUG)
        printf("allocs: C runtime heap calls during the run %ld, miniaudio %u, callbacks %ld, frames %ld\n",
               (long)g_crt, (unsigned)(ma1 - ma0), (long)g_nticks, nframes);
#else
        printf("allocs: miniaudio %u (C runtime count needs an MSVC debug build)\n", (unsigned)(ma1 - ma0));
#endif
    }
    yau_close(&au);
    if (fr) fclose(fr);
    puts(line);
    n = g_nticks;
    if (csv) {
        char path[512];
        FILE* ft;
        snprintf(path, sizeof path, "%s_ticks.csv", csv);
        ft = fopen(path, "w");
        if (ft) {
            fprintf(ft, "t_entry,pos,pos_t,w,getpos_ns,render_ns\n");
            for (i = 0; i < n; i++)
                fprintf(ft, "%lld,%lld,%lld,%lld,%lld,%lld\n", (long long)g_ticks[i].t_entry,
                        (long long)g_ticks[i].pos, (long long)g_ticks[i].pos_t, (long long)g_ticks[i].w,
                        (long long)(g_ticks[i].t_hook - g_ticks[i].t_entry),
                        (long long)(g_ticks[i].t_end - g_ticks[i].t_hook));
            fclose(ft);
        }
    }
    if (n > 2) {
        long m = 0;
        for (i = 1; i < n; i++) g_tmp[m++] = g_ticks[i].t_entry - g_ticks[i - 1].t_entry;
        qsort(g_tmp, (size_t)m, sizeof g_tmp[0], cmp_i64);
        printf("callbacks %ld; interval us p1 %.1f p50 %.1f p99 %.1f p99.9 %.1f max %.1f\n", n,
               pct(g_tmp, m, 0.01) / 1e3, pct(g_tmp, m, 0.5) / 1e3, pct(g_tmp, m, 0.99) / 1e3,
               pct(g_tmp, m, 0.999) / 1e3, g_tmp[m - 1] / 1e3);
        m = 0;
        for (i = 0; i < n; i++) g_tmp[m++] = g_ticks[i].t_hook - g_ticks[i].t_entry;
        qsort(g_tmp, (size_t)m, sizeof g_tmp[0], cmp_i64);
        printf("GetPosition us p50 %.2f p99 %.2f max %.2f\n", pct(g_tmp, m, 0.5) / 1e3, pct(g_tmp, m, 0.99) / 1e3, g_tmp[m - 1] / 1e3);
        m = 0;
        for (i = 0; i < n; i++)
            if (g_ticks[i].pos > 0) g_tmp[m++] = g_ticks[i].w - g_ticks[i].pos;
        if (m > 0) {
            qsort(g_tmp, (size_t)m, sizeof g_tmp[0], cmp_i64);
            printf("queue (written - position) frames min %lld p1 %lld p50 %lld p99 %lld max %lld\n", (long long)g_tmp[0],
                   (long long)pct(g_tmp, m, 0.01), (long long)pct(g_tmp, m, 0.5), (long long)pct(g_tmp, m, 0.99),
                   (long long)g_tmp[m - 1]);
        }
        m = 0;
        for (i = 0; i < n; i++) g_tmp[m++] = g_ticks[i].t_end - g_ticks[i].t_hook;
        qsort(g_tmp, (size_t)m, sizeof g_tmp[0], cmp_i64);
        {
            double mean = 0;
            for (i = 0; i < m; i++) mean += (double)g_tmp[i];
            mean /= (double)m;
            printf("render us mean %.2f p50 %.2f p99 %.2f max %.2f (%d voices, %d streams)\n", mean / 1e3,
                   pct(g_tmp, m, 0.5) / 1e3, pct(g_tmp, m, 0.99) / 1e3, g_tmp[m - 1] / 1e3, voices, g_nstreams);
        }
    }
    if (g_stalls) printf("stalls injected %ld, %.1f ms each\n", g_stalls, (double)g_stall_req / 1e6);
    printf("xrun records %d\n", n_xr);
    for (i = 0; i < n_xr; i++)
        printf("  xrun %d at %.3f s: frame %lld, gap %lld frames, halt %.3f ms at position %lld\n", i + 1,
               n > 0 ? (double)((int64_t)g_xr[i].t_ns - g_ticks[0].t_entry) / 1e9 : 0.0,
               (long long)g_xr[i].u.i64[0], (long long)g_xr[i].u.i64[1],
               (double)g_xr[i].u.i64[2] / 1e6, (long long)g_xr[i].u.i64[3]);
    if (nframes > 0) {
        double mean = 0;
        for (i = 0; i < nframes; i++) mean += (double)g_frame_cost[i];
        mean /= (double)nframes;
        qsort(g_frame_cost, (size_t)nframes, sizeof g_frame_cost[0], cmp_i64);
        printf("frame thread play_at+update us mean %.2f p99 %.2f max %.2f (%ld frames)\n", mean / 1e3,
               pct(g_frame_cost, nframes, 0.99) / 1e3, g_frame_cost[nframes - 1] / 1e3, nframes);
    }
    return 0;
}
