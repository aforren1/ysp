/* net_loopback.c - ysp/net.h against the real liblsl on one machine: an
 * outlet and an inlet in one process, then in two.
 *
 *   net_loopback [--lib PATH] [--seconds S] [--two] [--quick]
 *
 *   One process (always):
 *     resolve   an inlet for a key nobody has: OPENING for 3 s, resolves
 *               once a second
 *     markers   500 string markers 10 ms apart; each carries its push's
 *               ysp_rt time; prints the arrival minus the push (the
 *               latency) and the mapped time minus the push's bracket
 *               middle (the clock mapping error), min, p50, p99, max
 *     stream    10 kHz, 4 channels, float32, for S seconds (10) from a
 *               producer thread into an inlet with a stream ring drained
 *               every 10 ms: samples, drops, gaps, the reader's time per
 *               sample, the measured rate, the ring's lag
 *     lifecycle the 10 kHz outlet destroyed for 3 s and made again: LOST,
 *               RUNNING, one GAP record
 *   --quick (CI): 100 markers and 2000 clock brackets.
 *   --two: the markers and the stream again, with the outlets in a child
 *   process (this program, --child), so the samples cross a process.
 *   ysp_rt is one clock for every process of a machine (QPC on Windows,
 *   CLOCK_MONOTONIC on Linux), so the child's push times compare with the
 *   parent's directly.
 *
 * liblsl: --lib, else $YSP_LSL_PATH, else the copy tools/vendor_lsl.py put
 * in third_party/liblsl/ (CMake passes its path), else ysp/net.h's default
 * names. Built with YSP_BUILD_LOOPBACK; not run by ctest (CI runs it after
 * the fetch). Exit code 0 when every
 * check passed, 1 when one failed, 77 when liblsl is not there (a skip),
 * 2 for a usage error.
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#define YSP_NET_IMPLEMENTATION
#include "ysp/net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#include <sys/wait.h>
#include <pthread.h>
#endif

static ynet_lsl g_lib;
static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); g_fail++; } } while (0)

static int cmp_i64(const void* a, const void* b) {
    int64_t x = *(const int64_t*)a, y = *(const int64_t*)b;
    return x < y ? -1 : (x > y);
}
static void dist(const char* what, int64_t* v, int n) {
    if (n <= 0) { printf("  %-34s n 0\n", what); return; }
    qsort(v, (size_t)n, sizeof v[0], cmp_i64);
    printf("  %-34s n %4d  min %8.1f  p50 %8.1f  p99 %8.1f  max %8.1f  us\n", what, n, (double)v[0] / 1e3,
           (double)v[n / 2] / 1e3, (double)v[n * 99 / 100] / 1e3, (double)v[n - 1] / 1e3);
}

/* --- markers ------------------------------------------------------------------------ */

#define NMARK 500
static int64_t g_lat[NMARK], g_err[NMARK];
static int g_nmark = NMARK;    /* --quick: 100 */
static int g_nclock = 10000;   /* --quick: 2000 */
static volatile int g_nm;
static void on_mark(void* ctx, const yin_event* e, const char* text) {
    long long push_mid = 0;
    int64_t now = yrt_now_ns();
    (void)ctx;
    if (g_nm < g_nmark && sscanf(text, "t=%lld", &push_mid) == 1) {
        g_lat[g_nm] = now - (int64_t)push_mid;
        g_err[g_nm] = e->t - (int64_t)push_mid;
        g_nm++;
    }
}

/* n markers 10 ms apart, each with the middle of its own push bracket's
 * first half (the time just before the push) as text. */
static void send_markers(const char* key, int n) {
    static ynet_outlet o;
    ynet_outlet_desc d;
    int i;
    int64_t next, end;
    memset(&d, 0, sizeof d);
    d.lib = &g_lib; d.role = "marks"; d.key = key; d.device = 1;
    if (!ynet_outlet_start(&o, &d)) { printf("outlet: %s\n", ynet_outlet_error(&o)); g_fail++; return; }
    end = yrt_now_ns() + 10000000000LL;
    while (!ynet_outlet_consumers(&o) && yrt_now_ns() < end) (void)yrt_sleep_ns(10000000);
    (void)yrt_sleep_ns(2000000000);  /* the inlet blocks in its first time correction (liblsl probes for it) */
    next = yrt_now_ns();
    for (i = 0; i < n; i++) {
        char text[40];
        int64_t t;
        next += 10000000;
        (void)yrt_sleep_until(next, 500000);
        /* the stamp liblsl gets is read right after this; the text is the push's time */
        t = yrt_now_ns();
        snprintf(text, sizeof text, "t=%lld", (long long)t);
        (void)ynet_out_mark(&o, (uint32_t)(i + 1), text);
    }
    (void)yrt_sleep_ns(300000000);
    ynet_outlet_stop(&o);
}

static void recv_markers(const char* key, int local_sender) {
    static ynet_inlet in;
    ynet_inlet_desc d;
    int64_t end;
    memset(&d, 0, sizeof d);
    d.lib = &g_lib; d.role = "marks_in"; d.key = key; d.device = 2; d.format = YNET_STRING; d.text_sink = on_mark;
    g_nm = 0;
    CHECK(ynet_inlet_start(&in, &d));
    if (local_sender) send_markers(key, g_nmark);
    else {
        end = yrt_now_ns() + 30000000000LL;
        while (g_nm < g_nmark && yrt_now_ns() < end) (void)yrt_sleep_ns(10000000);
    }
    (void)yrt_sleep_ns(200000000);
    ynet_inlet_stop(&in);
    printf("markers: %d of %d\n", g_nm, g_nmark);
    CHECK(g_nm == g_nmark);
    dist("arrival - push (latency)", g_lat, g_nm);
    dist("mapped t - push (mapping error)", g_err, g_nm);
}

/* --- the 10 kHz stream ---------------------------------------------------------------- */

static volatile int g_stop;
static ynet_outlet g_so;

static void stream_start(const char* key) {
    ynet_outlet_desc d;
    memset(&d, 0, sizeof d);
    d.lib = &g_lib; d.role = "emg"; d.key = key; d.device = 3; d.format = YNET_FLOAT32; d.channels = 4; d.rate_hz = 10000;
    if (!ynet_outlet_start(&g_so, &d)) { printf("outlet: %s\n", ynet_outlet_error(&g_so)); g_fail++; }
}

#if defined(_WIN32)
static unsigned __stdcall producer(void* arg)
#else
static void* producer(void* arg)
#endif
{
    /* 10 samples a ms, stamped 1 ms before their push, as a device sends blocks */
    float x[40];
    double ts[10];
    double l0 = (double)ynet_lsl_now_ns(&g_lib) / 1e9 - 1e-3;
    int64_t next = yrt_now_ns();
    uint64_t k = 0;
    int i;
    (void)arg;
    (void)yrt_thread_elevate(NULL);
    while (!g_stop) {
        for (i = 0; i < 10; i++, k++) {
            x[4 * i] = (float)((k / 50) % 2); x[4 * i + 1] = (float)(k % 1000000); x[4 * i + 2] = 0; x[4 * i + 3] = 0;
            ts[i] = l0 + (double)k * 1e-4;
        }
        (void)ynet_out_push(&g_so, x, 10, ts);
        next += 1000000;
        (void)yrt_sleep_until(next, 200000);
    }
    return 0;
}

static void run_producer(double secs) {
#if defined(_WIN32)
    uintptr_t th = _beginthreadex(NULL, 0, producer, NULL, 0, NULL);
    g_stop = 0;
    (void)yrt_sleep_ns((int64_t)(secs * 1e9));
    g_stop = 1;
    WaitForSingleObject((HANDLE)th, INFINITE);
    CloseHandle((HANDLE)th);
#else
    pthread_t th;
    g_stop = 0;
    pthread_create(&th, NULL, producer, NULL);
    (void)yrt_sleep_ns((int64_t)(secs * 1e9));
    g_stop = 1;
    pthread_join(th, NULL);
#endif
}

static void recv_stream(const char* key, double secs, int local_sender, int lifecycle) {
    static ynet_inlet in;
    static unsigned char mem[YNET_STREAM_BYTES(1 << 16, 4)];
    static ynet_stream ring;
    static unsigned char logmem[YRT_RING_BYTES(4096)];
    static yrt_event recs[4096];
    static float x[8192 * 4];
    static int64_t t[8192];
    yrt_ring log;
    yrt_ring_desc rd;
    ynet_inlet_desc d;
    ynet_stats st;
    int64_t end, got = 0, lag = 0;
    int n, i, gaps = 0;
    rd.memory = logmem; rd.bytes = sizeof logmem;
    memset(&log, 0, sizeof log);
    (void)yrt_ring_open(&log, &rd);
    CHECK(ynet_stream_init(&ring, mem, sizeof mem, 4));
    memset(&d, 0, sizeof d);
    d.lib = &g_lib; d.role = "emg_in"; d.key = key; d.device = 4; d.format = YNET_FLOAT32; d.channels = 4;
    d.stream = &ring; d.ring = &log;
    CHECK(ynet_inlet_start(&in, &d));
    if (local_sender) {
        stream_start(key);
        end = yrt_now_ns() + 10000000000LL;
        while (ynet_inlet_state(&in) != YNET_RUNNING && yrt_now_ns() < end) (void)yrt_sleep_ns(10000000);
#if defined(_WIN32)
        {
            uintptr_t th;
            g_stop = 0;
            th = _beginthreadex(NULL, 0, producer, NULL, 0, NULL);
#else
        {
            pthread_t th;
            g_stop = 0;
            pthread_create(&th, NULL, producer, NULL);
#endif
            end = yrt_now_ns() + (int64_t)(secs * 1e9);
            while (yrt_now_ns() < end) {
                (void)yrt_sleep_ns(10000000);
                while ((n = ynet_stream_read(&ring, t, NULL, x, 8192)) > 0) {
                    int64_t now = yrt_now_ns();
                    for (i = 0; i < n; i++) if (now - t[i] > lag) lag = now - t[i];
                    got += n;
                }
            }
            g_stop = 1;
#if defined(_WIN32)
            WaitForSingleObject((HANDLE)th, INFINITE);
            CloseHandle((HANDLE)th);
#else
            pthread_join(th, NULL);
#endif
        }
    } else {
        end = yrt_now_ns() + (int64_t)((secs + 15) * 1e9);
        while (yrt_now_ns() < end) {
            (void)yrt_sleep_ns(10000000);
            while ((n = ynet_stream_read(&ring, t, NULL, x, 8192)) > 0) {
                int64_t now = yrt_now_ns();
                for (i = 0; i < n; i++) if (now - t[i] > lag) lag = now - t[i];
                got += n;
            }
            ynet_inlet_stats(&in, &st);
            if (st.samples > 0 && st.state == YNET_LOST) break;   /* the child ended */
        }
    }
    (void)yrt_sleep_ns(200000000);
    while ((n = ynet_stream_read(&ring, NULL, NULL, x, 8192)) > 0) got += n;
    ynet_inlet_stats(&in, &st);
    printf("stream 10 kHz x 4: %llu samples (%lld read), %.0f Hz, dropped %llu, gaps %llu (%llu missing), clamped %llu, "
           "reader %.1f ns per sample (%.1f samples a chunk), ring lag max %.1f ms; offset %+.1f us, rtt %.1f us\n",
           (unsigned long long)st.samples, (long long)got, st.measured_hz, (unsigned long long)st.dropped,
           (unsigned long long)st.gaps, (unsigned long long)st.missing, (unsigned long long)st.clamped,
           st.samples ? (double)st.busy_ns / (double)st.samples : 0.0,
           st.chunks ? (double)st.samples / (double)st.chunks : 0.0, (double)lag / 1e6, st.offset_s * 1e6, st.rtt_s * 1e6);
    printf("  local fit: %+.3f ppm, widest bracket %.2f us; remote fit: %+.3f ppm, %u points, spread %.1f us\n",
           st.local.ppm, (double)st.local.width_ns / 1e3, st.remote.ppm, (unsigned)st.remote.points,
           (double)st.remote.spread_ns / 1e3);
    CHECK(st.samples > 0 && (uint64_t)got == st.samples);
    CHECK(st.dropped == 0);
    CHECK(st.gaps == 0);
    if (lifecycle) {
        int64_t t_gone;
        ynet_outlet_stop(&g_so);
        t_gone = yrt_now_ns();
        (void)yrt_sleep_ns(3000000000LL);
        printf("lifecycle: outlet destroyed: %s after 3 s\n", ynet_state_name(ynet_inlet_state(&in)));
        CHECK(ynet_inlet_state(&in) == YNET_LOST);
        stream_start(key);
        run_producer(3.0);
        ynet_inlet_stats(&in, &st);
        printf("lifecycle: made again: %s, opens %u\n", ynet_state_name(st.state), (unsigned)st.opens);
        CHECK(st.state == YNET_RUNNING && st.opens == 2);
        ynet_outlet_stop(&g_so);
        n = yrt_ring_drain(&log, recs, 4096);
        for (i = 0; i < n; i++)
            if (recs[i].source == YRT_SRC_NET && recs[i].kind == YNET_REC_GAP) {
                gaps++;
                printf("lifecycle: GAP from %.3f s to %.3f s after the outlet went\n",
                       (double)(recs[i].u.i64[0] - t_gone) / 1e9, (double)(recs[i].u.i64[1] - t_gone) / 1e9);
            }
        CHECK(gaps == 1);
    } else if (local_sender) {
        ynet_outlet_stop(&g_so);
    }
    ynet_inlet_stop(&in);
}

/* lsl_local_clock() against yrt_now_ns(): 10000 brackets over 2 s. The
 * offset's spread is what the local fit has to remove. */
static void clock_check(void) {
    static int64_t w[10000], off[10000];
    int i;
    int64_t base;
    for (i = 0; i < g_nclock; i++) {
        int64_t a = yrt_now_ns();
        int64_t l = ynet_lsl_now_ns(&g_lib);
        int64_t b = yrt_now_ns();
        w[i] = b - a;
        off[i] = l - (a + b) / 2;
        (void)yrt_sleep_ns(200000);
    }
    base = off[0];
    for (i = 0; i < g_nclock; i++) off[i] -= base;
    printf("clock: lsl_local_clock - ysp_rt at the first bracket %+.6f s\n", (double)base / 1e9);
    dist("bracket width", w, g_nclock);
    dist("offset - first offset", off, g_nclock);
}

static void resolve_timeout(void) {
    static ynet_inlet in;
    ynet_inlet_desc d;
    ynet_stats st;
    char key[80];
    memset(&d, 0, sizeof d);
    snprintf(key, sizeof key, "lsl:ysp-no-such-stream-%u", (unsigned)(yrt_now_ns() % 1000000u));
    d.lib = &g_lib; d.role = "none"; d.key = key; d.device = 5; d.resolve_ns = 250000000;
    CHECK(ynet_inlet_start(&in, &d));
    (void)yrt_sleep_ns(3000000000LL);
    ynet_inlet_stats(&in, &st);
    printf("resolve: %s after 3 s, %llu resolves\n", ynet_state_name(st.state), (unsigned long long)st.resolves);
    CHECK(st.state == YNET_OPENING && st.resolves >= 2 && st.resolves <= 4);
    ynet_inlet_stop(&in);
}

static int spawn_child(const char* self, const char* lib, double secs) {
    char cmd[1024];
#if defined(_WIN32)
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    snprintf(cmd, sizeof cmd, "\"%s\" --child --seconds %.1f%s%s%s%s", self, secs, g_nmark < NMARK ? " --quick" : "",
             lib ? " --lib \"" : "", lib ? lib : "", lib ? "\"" : "");
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) return -1;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
#else
    pid_t p = fork();
    char s[32];
    (void)cmd;
    snprintf(s, sizeof s, "%.1f", secs);
    if (p < 0) return -1;
    if (p == 0) {
        const char* q = g_nmark < NMARK ? "--quick" : "--child";   /* --child twice is harmless */
        if (lib) execl(self, self, "--child", q, "--seconds", s, "--lib", lib, (char*)NULL);
        else execl(self, self, "--child", q, "--seconds", s, (char*)NULL);
        _exit(127);
    }
    return 0;
#endif
}

int main(int argc, char** argv) {
    const char* lib = NULL;
    double secs = 10;
    int two = 0, child = 0, i, rc;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--lib") && i + 1 < argc) lib = argv[++i];
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) secs = atof(argv[++i]);
        else if (!strcmp(argv[i], "--two")) two = 1;
        else if (!strcmp(argv[i], "--quick")) { g_nmark = 100; g_nclock = 2000; }
        else if (!strcmp(argv[i], "--child")) child = 1;
        else { fprintf(stderr, "usage: net_loopback [--lib PATH] [--seconds S] [--two] [--quick]\n"); return 2; }
    }
#ifdef YNET_LOOPBACK_LIB
    /* CMake's copy from tools/vendor_lsl.py, unless --lib or YSP_LSL_PATH names another */
    if (!lib && !getenv("YSP_LSL_PATH")) lib = YNET_LOOPBACK_LIB;
#endif
    if ((rc = ynet_lsl_load(&g_lib, lib)) != YNET_OK) {
        printf("skip: %s\n", g_lib.error);
        return 77;
    }
    if (child) {
        /* the sender of --two: markers, then the stream; then it ends */
        send_markers("lsl:ysp-loop-marks-2:Markers:ysp-loop-child", g_nmark);
        stream_start("lsl:ysp-loop-emg-2:EMG:ysp-loop-child");
        (void)yrt_sleep_ns(2000000000LL);
        run_producer(secs);
        (void)yrt_sleep_ns(500000000);
        ynet_outlet_stop(&g_so);
        ynet_lsl_unload(&g_lib);
        return 0;
    }
    printf("liblsl %d from %s\n", (int)g_lib.version, g_lib.path);
    clock_check();
    resolve_timeout();
    printf("-- one process\n");
    recv_markers("lsl:ysp-loop-marks:Markers:ysp-loop", 1);
    recv_stream("lsl:ysp-loop-emg:EMG:ysp-loop", secs, 1, 1);
    if (two) {
        printf("-- two processes\n");
        if (spawn_child(argv[0], lib, secs) != 0) { printf("FAIL: the child did not start\n"); g_fail++; }
        else {
            recv_markers("lsl:ysp-loop-marks-2:Markers:ysp-loop-child", 0);
            recv_stream("lsl:ysp-loop-emg-2:EMG:ysp-loop-child", secs, 0, 0);
#if !defined(_WIN32)
            (void)wait(NULL);
#endif
        }
    }
    ynet_lsl_unload(&g_lib);
    printf("%s\n", g_fail ? "FAILED" : "ok");
    return g_fail ? 1 : 0;
}
