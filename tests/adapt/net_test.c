/* net_test.c - self-checking test for ysp/net.h. No framework: it returns 0
 * when every check passed and 1 after printing each failure.
 *
 * liblsl is replaced by a fake: a ynet_lsl table of functions in this file
 * that keeps outlets, stream infos, inlets and their samples in fixed
 * arrays. The fake has two clocks besides the host's: this machine's LSL
 * clock (the host clock plus an offset) and a remote sender's (another
 * offset and a rate error), and time corrections whose estimates scatter
 * within half their round trip, as liblsl's do. Most of the test runs the
 * inlet in manual mode on a virtual clock: a pull that finds nothing
 * advances the clock by its timeout, so a minute takes milliseconds and
 * every run is the same. The reference for each event is the true host time
 * of its sample. One part runs the reader thread on the real clock with a
 * 10 kHz, 4-channel producer thread. ysp/screen.h (declarations only, no
 * SDL) and ysp/device.h come first, so the trigger channel and role glue are
 * built and called.
 *
 * It also checks the loader against a library that is not liblsl, and,
 * when liblsl is not installed, the NOLIB message.
 *
 *     gcc -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Werror -Iinclude \
 *         -o net_test tests/adapt/net_test.c -lsetupapi   (Windows; -ldl -lm -pthread on Linux)
 */
#include "ysp/screen.h"
#define YSP_DEVICE_IMPLEMENTATION
#include "ysp/device.h"
#define YSP_NET_IMPLEMENTATION
#include "ysp/net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#if !defined(_WIN32)
#include <pthread.h>
#endif

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond) do { g_checks++; if (!(cond)) { \
    fprintf(stderr, "net_test: FAIL at line %d: %s\n", __LINE__, #cond); g_failures++; } } while (0)
#define CHECK_I(got, want) do { long long g_ = (long long)(got), w_ = (long long)(want); g_checks++; \
    if (g_ != w_) { fprintf(stderr, "net_test: FAIL at line %d: %s (got %lld, want %lld)\n", \
                            __LINE__, #got, g_, w_); g_failures++; } } while (0)

static uint64_t g_rng = 0x9E3779B97F4A7C15ull;
static double u01(void) {
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17;
    return ((double)(g_rng >> 11) + 1.0) / 9007199254740992.0;
}

/* --- clocks ------------------------------------------------------------------------- */

static int64_t g_now = 1000000000000LL;   /* the host clock reads 1000 s           */
static int     g_real;                    /* the thread part: the real clock       */
static int64_t host_now(void) { return g_real ? yrt_now_ns() : g_now; }
static int64_t now_fn(void* ctx) { (void)ctx; return host_now(); }

/* This machine's LSL clock: the host clock plus 123.456789 s. */
#define LOCAL_OFF_NS 123456789000LL
static double local_of(int64_t h) { return (double)(h + LOCAL_OFF_NS) / 1e9; }

/* A remote sender's clock: offset and rate error, set per test. */
static int64_t g_roff = 0;       /* ns */
static double  g_rppm = 0.0;
static double  g_rtt_ns = 0.0;   /* the time corrections' round trip */
static int     g_corr_fail = 0;  /* time corrections time out          */
static double remote_of(int64_t h) {
    return ((double)(h - 1000000000000LL) * (1.0 + g_rppm * 1e-6) + 1000000000000.0 + (double)g_roff) / 1e9;
}

/* --- the fake liblsl ------------------------------------------------------------------- */

#if defined(_WIN32)
static CRITICAL_SECTION g_mx;
static void fk_lock(void) { EnterCriticalSection(&g_mx); }
static void fk_unlock(void) { LeaveCriticalSection(&g_mx); }
static void fk_lock_init(void) { InitializeCriticalSection(&g_mx); }
#else
static pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;
static void fk_lock(void) { pthread_mutex_lock(&g_mx); }
static void fk_unlock(void) { pthread_mutex_unlock(&g_mx); }
static void fk_lock_init(void) { }
#endif

#define FK_OUTLETS 6
#define FK_CAP     (1 << 18)
#define FK_SCAP    4096
#define FK_CH      8

typedef struct fk_outlet {
    int     used, alive, remote;    /* remote: stamps on the remote clock      */
    char    name[64], type[32], sid[64], host[64], uid[48];
    int     fmt, ch;
    double  rate;
    int64_t n;                      /* samples written                         */
    double  ts[FK_CAP];
    int64_t avail[FK_CAP];          /* host time an inlet can see it           */
    float   x[(size_t)FK_CAP * FK_CH];
    char    str[FK_SCAP][48];
    int32_t iv[FK_SCAP];
    int     consumers;
} fk_outlet;

typedef struct fk_info { int used, outlet; } fk_info;
typedef struct fk_inlet { int used, outlet, opened; int64_t pos; int64_t last_corr; double est_off, est_remote, est_unc; } fk_inlet;

static fk_outlet g_out[FK_OUTLETS];
static fk_info   g_info[64];
static fk_inlet  g_in[16];
static int       g_strings;       /* strings handed out and not yet freed */
static int64_t   g_delay_ns = 1000000;   /* delivery delay of a pushed sample */

static int32_t fk_version(void) { return 116; }
static double  fk_clock(void) { return local_of(host_now()); }
static void    fk_destroy_string(char* s) { fk_lock(); g_strings--; fk_unlock(); free(s); }

static void* fk_info_new(int outlet) {
    int i;
    for (i = 0; i < 64; i++)
        if (!g_info[i].used) { g_info[i].used = 1; g_info[i].outlet = outlet; return &g_info[i]; }
    return NULL;
}

static void* fk_create_streaminfo(const char* name, const char* type, int32_t ch, double rate, int fmt, const char* sid) {
    int i;
    void* r = NULL;
    fk_lock();
    for (i = 0; i < FK_OUTLETS; i++)
        if (!g_out[i].used) {
            fk_outlet* o = &g_out[i];
            o->used = 1;
            o->alive = 0;            /* an outlet makes it alive */
            o->remote = 0;
            snprintf(o->name, sizeof o->name, "%s", name);
            snprintf(o->type, sizeof o->type, "%s", type);
            snprintf(o->sid, sizeof o->sid, "%s", sid);
            snprintf(o->host, sizeof o->host, "testhost");
            snprintf(o->uid, sizeof o->uid, "uid-%d", i);
            o->fmt = fmt;
            o->ch = ch;
            o->rate = rate;
            o->n = 0;
            o->consumers = 0;
            r = fk_info_new(i);
            break;
        }
    fk_unlock();
    return r;
}
static void fk_destroy_streaminfo(void* p) { fk_lock(); ((fk_info*)p)->used = 0; fk_unlock(); }
static fk_outlet* fk_of(void* info) { return &g_out[((fk_info*)info)->outlet]; }
static const char* fk_get_name(void* p) { return fk_of(p)->name; }
static const char* fk_get_type(void* p) { return fk_of(p)->type; }
static const char* fk_get_sid(void* p) { return fk_of(p)->sid; }
static const char* fk_get_host(void* p) { return fk_of(p)->host; }
static const char* fk_get_uid(void* p) { return fk_of(p)->uid; }
static int32_t fk_get_ch(void* p) { return fk_of(p)->ch; }
static double fk_get_rate(void* p) { return fk_of(p)->rate; }
static int fk_get_fmt(void* p) { return fk_of(p)->fmt; }

/* The predicate's "key='value'" terms, as ysp/net.h writes them. */
static int fk_pred_match(const char* pred, const fk_outlet* o) {
    const char* p = pred;
    while (*p) {
        char key[32], val[96];
        size_t k = 0, v = 0;
        while (*p == ' ') p++;
        if (strncmp(p, "and ", 4) == 0) { p += 4; continue; }
        while (*p && *p != '=' && k + 1 < sizeof key) key[k++] = *p++;
        key[k] = '\0';
        if (*p != '=' || p[1] != '\'') return 0;
        p += 2;
        while (*p && *p != '\'' && v + 1 < sizeof val) val[v++] = *p++;
        val[v] = '\0';
        if (*p != '\'') return 0;
        p++;
        if (!strcmp(key, "name") && strcmp(val, o->name)) return 0;
        if (!strcmp(key, "type") && strcmp(val, o->type)) return 0;
        if (!strcmp(key, "source_id") && strcmp(val, o->sid)) return 0;
        if (!strcmp(key, "hostname") && strcmp(val, o->host)) return 0;
    }
    return 1;
}

static int g_resolve_calls;
static double g_resolve_timeout;
static int32_t fk_resolve_bypred(void** buf, uint32_t n, const char* pred, int32_t minimum, double timeout) {
    int i, m = 0;
    (void)minimum;
    fk_lock();
    g_resolve_calls++;
    g_resolve_timeout = timeout;
    for (i = 0; i < FK_OUTLETS && (uint32_t)m < n; i++)
        if (g_out[i].used && g_out[i].alive && fk_pred_match(pred, &g_out[i])) buf[m++] = fk_info_new(i);
    fk_unlock();
    if (m == 0 && !g_real) g_now += (int64_t)(timeout * 1e9);   /* the resolve waited */
    return m;
}
static int32_t fk_resolve_all(void** buf, uint32_t n, double wait) {
    int i, m = 0;
    (void)wait;
    fk_lock();
    for (i = 0; i < FK_OUTLETS && (uint32_t)m < n; i++)
        if (g_out[i].used && g_out[i].alive) buf[m++] = fk_info_new(i);
    fk_unlock();
    return m;
}

static void* fk_create_outlet(void* info, int32_t chunk, int32_t maxbuf) {
    fk_outlet* o;
    (void)chunk; (void)maxbuf;
    fk_lock();
    o = fk_of(info);
    o->alive = 1;
    fk_unlock();
    return o;
}
static void fk_destroy_outlet(void* p) { fk_lock(); ((fk_outlet*)p)->alive = 0; ((fk_outlet*)p)->used = 0; fk_unlock(); }

static int32_t fk_push_str(void* p, const char** data, double ts, int32_t pt) {
    fk_outlet* o = (fk_outlet*)p;
    int64_t i;
    (void)pt;
    fk_lock();
    i = o->n;
    if (i < FK_SCAP) {
        o->ts[i] = ts;
        o->avail[i] = host_now() + g_delay_ns;
        snprintf(o->str[i], sizeof o->str[i], "%s", data[0]);
        o->n++;
    }
    fk_unlock();
    return 0;
}
static int32_t fk_push_i(void* p, const int32_t* data, double ts, int32_t pt) {
    fk_outlet* o = (fk_outlet*)p;
    int64_t i;
    (void)pt;
    fk_lock();
    i = o->n;
    if (i < FK_SCAP) {
        o->ts[i] = ts;
        o->avail[i] = host_now() + g_delay_ns;
        o->iv[i] = data[0];
        o->n++;
    }
    fk_unlock();
    return 0;
}
static int32_t fk_push_ftnp(void* p, const float* x, unsigned long n, const double* ts, int32_t pt) {
    fk_outlet* o = (fk_outlet*)p;
    unsigned long k, m = n / (unsigned long)o->ch;
    (void)pt;
    fk_lock();
    for (k = 0; k < m && o->n < FK_CAP; k++) {
        int64_t i = o->n++;
        o->ts[i] = ts[k];
        o->avail[i] = host_now() + g_delay_ns;
        memcpy(&o->x[(size_t)i * FK_CH], x + (size_t)k * (size_t)o->ch, (size_t)o->ch * sizeof(float));
    }
    fk_unlock();
    return 0;
}
static int32_t fk_have_consumers(void* p) { int r; fk_lock(); r = ((fk_outlet*)p)->consumers > 0; fk_unlock(); return r; }

static void* fk_create_inlet(void* info, int32_t buflen, int32_t chunk, int32_t recover) {
    int i;
    void* r = NULL;
    (void)buflen; (void)chunk; (void)recover;
    fk_lock();
    for (i = 0; i < 16; i++)
        if (!g_in[i].used) {
            memset(&g_in[i], 0, sizeof g_in[i]);
            g_in[i].used = 1;
            g_in[i].outlet = ((fk_info*)info)->outlet;
            r = &g_in[i];
            break;
        }
    fk_unlock();
    return r;
}
static void fk_destroy_inlet(void* p) {
    fk_inlet* in = (fk_inlet*)p;
    fk_lock();
    if (in->opened && g_out[in->outlet].consumers > 0) g_out[in->outlet].consumers--;
    in->used = 0;
    fk_unlock();
}
static void fk_open_stream(void* p, double timeout, int32_t* ec) {
    fk_inlet* in = (fk_inlet*)p;
    (void)timeout;
    fk_lock();
    in->opened = 1;
    /* samples pushed from now on: those not yet delivered */
    in->pos = 0;
    while (in->pos < g_out[in->outlet].n && g_out[in->outlet].avail[in->pos] <= host_now()) in->pos++;
    g_out[in->outlet].consumers++;
    fk_unlock();
    *ec = 0;
}

/* liblsl's estimate: the offset of the best probe, within half its round
 * trip of the truth; new every 2 s, cached between. */
static double fk_time_correction_ex(void* p, double* remote, double* unc, double timeout, int32_t* ec) {
    fk_inlet* in = (fk_inlet*)p;
    fk_outlet* o = &g_out[in->outlet];
    int64_t h = host_now();
    (void)timeout;
    if (g_corr_fail) { *ec = -1; return 0.0; }
    *ec = 0;
    if (!in->last_corr || h - in->last_corr >= 2000000000) {
        double rtt = g_rtt_ns * (0.5 + u01());            /* round trips vary */
        double err = (u01() - 0.5) * rtt;                 /* within rtt / 2   */
        double r = o->remote ? remote_of(h) : local_of(h);
        in->est_remote = r;
        in->est_off = (local_of(h) - r) + err / 1e9;
        in->est_unc = rtt / 1e9;
        in->last_corr = h;
    }
    *remote = in->est_remote;
    *unc = in->est_unc;
    return in->est_off;
}

/* Wait for a sample: on the virtual clock the clock jumps to it, or by the
 * whole timeout when none comes in it. */
static int fk_wait(fk_inlet* in, double timeout, int32_t* ec) {
    fk_outlet* o = &g_out[in->outlet];
    int64_t end = host_now() + (int64_t)(timeout * 1e9);
    for (;;) {
        int64_t h;
        fk_lock();
        h = host_now();
        if (!o->alive || !o->used) { fk_unlock(); *ec = -2; return 0; }
        if (in->pos < o->n && o->avail[in->pos] <= h) { fk_unlock(); return 1; }
        if (!g_real) {
            int64_t next = in->pos < o->n ? o->avail[in->pos] : end;
            if (next > end) next = end;
            if (next <= h) { fk_unlock(); *ec = 0; return 0; }
            g_now = next;
            fk_unlock();
            continue;
        }
        fk_unlock();
        if (h >= end) { *ec = 0; return 0; }
        (void)yrt_sleep_ns(200000);
    }
}

static double fk_pull_str(void* p, char** buf, int32_t n, double timeout, int32_t* ec) {
    fk_inlet* in = (fk_inlet*)p;
    fk_outlet* o = &g_out[in->outlet];
    double ts = 0.0;
    (void)n;
    *ec = 0;
    buf[0] = (char*)malloc(48);       /* liblsl allocates even on a timeout */
    buf[0][0] = '\0';
    fk_lock(); g_strings++; fk_unlock();
    if (fk_wait(in, timeout, ec)) {
        fk_lock();
        snprintf(buf[0], 48, "%s", o->str[in->pos]);
        ts = o->ts[in->pos];
        in->pos++;
        fk_unlock();
    }
    return ts;
}
static double fk_pull_i(void* p, int32_t* buf, int32_t n, double timeout, int32_t* ec) {
    fk_inlet* in = (fk_inlet*)p;
    fk_outlet* o = &g_out[in->outlet];
    double ts = 0.0;
    (void)n;
    *ec = 0;
    if (fk_wait(in, timeout, ec)) {
        fk_lock();
        buf[0] = o->iv[in->pos];
        ts = o->ts[in->pos];
        in->pos++;
        fk_unlock();
    }
    return ts;
}
static unsigned long fk_pull_chunk_f(void* p, float* data, double* ts, unsigned long ndata, unsigned long nts,
                                     double timeout, int32_t* ec) {
    fk_inlet* in = (fk_inlet*)p;
    fk_outlet* o = &g_out[in->outlet];
    unsigned long k = 0;
    *ec = 0;
    if (!fk_wait(in, timeout, ec)) return 0;
    fk_lock();
    while (k < nts && (k + 1) * (unsigned long)o->ch <= ndata && in->pos < o->n && o->avail[in->pos] <= host_now()) {
        memcpy(data + k * (unsigned long)o->ch, &o->x[(size_t)in->pos * FK_CH], (size_t)o->ch * sizeof(float));
        ts[k] = o->ts[in->pos];
        in->pos++;
        k++;
    }
    fk_unlock();
    return k * (unsigned long)o->ch;
}

static ynet_lsl g_lib;
static void fake_lib(void) {
    memset(&g_lib, 0, sizeof g_lib);
    g_lib.library_version = fk_version;
    g_lib.local_clock = fk_clock;
    g_lib.destroy_string = fk_destroy_string;
    g_lib.create_streaminfo = fk_create_streaminfo;
    g_lib.destroy_streaminfo = fk_destroy_streaminfo;
    g_lib.get_name = fk_get_name;
    g_lib.get_type = fk_get_type;
    g_lib.get_source_id = fk_get_sid;
    g_lib.get_hostname = fk_get_host;
    g_lib.get_uid = fk_get_uid;
    g_lib.get_channel_count = fk_get_ch;
    g_lib.get_nominal_srate = fk_get_rate;
    g_lib.get_channel_format = fk_get_fmt;
    g_lib.resolve_bypred = fk_resolve_bypred;
    g_lib.resolve_all = fk_resolve_all;
    g_lib.create_outlet = fk_create_outlet;
    g_lib.destroy_outlet = fk_destroy_outlet;
    g_lib.push_sample_strtp = fk_push_str;
    g_lib.push_sample_itp = fk_push_i;
    g_lib.push_chunk_ftnp = fk_push_ftnp;
    g_lib.have_consumers = fk_have_consumers;
    g_lib.create_inlet = fk_create_inlet;
    g_lib.destroy_inlet = fk_destroy_inlet;
    g_lib.open_stream = fk_open_stream;
    g_lib.time_correction_ex = fk_time_correction_ex;
    g_lib.pull_sample_str = fk_pull_str;
    g_lib.pull_sample_i = fk_pull_i;
    g_lib.pull_chunk_f = fk_pull_chunk_f;
    g_lib.version = 116;
}

static int fk_infos_alive(void) { int i, n = 0; for (i = 0; i < 64; i++) n += g_info[i].used; return n; }
static int fk_inlets_alive(void) { int i, n = 0; for (i = 0; i < 16; i++) n += g_in[i].used; return n; }

/* A remote stream the test writes directly: another machine's outlet. */
static int fk_remote_stream(const char* name, const char* type, const char* sid, int fmt, int ch, double rate) {
    void* info = fk_create_streaminfo(name, type, ch, rate, fmt, sid);
    int idx = ((fk_info*)info)->outlet;
    fk_destroy_streaminfo(info);
    g_out[idx].alive = 1;
    g_out[idx].remote = 1;
    return idx;
}

/* n samples of a remote float stream, the first at true host time h0. */
static void fk_remote_samples(int idx, int64_t h0, int n, double rate, float (*val)(int64_t k, int c), int64_t k0) {
    fk_outlet* o = &g_out[idx];
    int i, c;
    for (i = 0; i < n && o->n < FK_CAP; i++) {
        int64_t h = h0 + (int64_t)((double)i * 1e9 / rate);
        int64_t j = o->n++;
        o->ts[j] = remote_of(h);
        /* delivered in 1 ms blocks, 1 to 3 ms late */
        o->avail[j] = (h / 1000000 + 1) * 1000000 + 1000000 + (int64_t)(u01() * 2e6);
        for (c = 0; c < o->ch; c++) o->x[(size_t)j * FK_CH + (size_t)c] = val ? val(k0 + i, c) : (float)c;
    }
}

/* --- sinks and records -------------------------------------------------------------- */

#define MAXEV 4096
static yin_event g_ev[MAXEV];
static char      g_txt[MAXEV][48];
static int       g_nev;
static void sink(void* ctx, const yin_event* e) { (void)ctx; if (g_nev < MAXEV) g_ev[g_nev++] = *e; }
static void text_sink(void* ctx, const yin_event* e, const char* t) {
    (void)ctx; (void)e;
    if (g_nev > 0 && g_nev <= MAXEV) snprintf(g_txt[g_nev - 1], sizeof g_txt[0], "%s", t);
}

static unsigned char g_ring_mem[YRT_RING_BYTES(1 << 14)];
static yrt_ring g_ring;
static yrt_event g_rec[1 << 14];
static int g_nrec;
static void ring_open(void) {
    yrt_ring_desc rd;
    rd.memory = g_ring_mem;
    rd.bytes = sizeof g_ring_mem;
    memset(&g_ring, 0, sizeof g_ring);
    (void)yrt_ring_open(&g_ring, &rd);
    g_nrec = 0;
}
static void ring_drain(void) {
    int n;
    while ((n = yrt_ring_drain(&g_ring, g_rec + g_nrec, (1 << 14) - g_nrec)) > 0) g_nrec += n;
}
static int count_rec(uint16_t source, uint16_t kind) {
    int i, n = 0;
    for (i = 0; i < g_nrec; i++) n += g_rec[i].source == source && g_rec[i].kind == kind;
    return n;
}
static const yrt_event* find_rec(uint16_t source, uint16_t kind, int nth) {
    int i;
    for (i = 0; i < g_nrec; i++)
        if (g_rec[i].source == source && g_rec[i].kind == kind && nth-- == 0) return &g_rec[i];
    return NULL;
}
static int find_text(const char* prefix) {
    int i;
    for (i = 0; i < g_nrec; i++)
        if (g_rec[i].source == YRT_SRC_NET && g_rec[i].kind == YNET_REC_TEXT &&
            strncmp(g_rec[i].u.text, prefix, strlen(prefix)) == 0) return 1;
    return 0;
}

static void reset_fake(void) {
    memset(g_out, 0, sizeof g_out);
    memset(g_info, 0, sizeof g_info);
    memset(g_in, 0, sizeof g_in);
    g_nev = 0;
    memset(g_txt, 0, sizeof g_txt);
    g_roff = 0; g_rppm = 0; g_rtt_ns = 100000; g_corr_fail = 0; g_delay_ns = 1000000;
    ring_open();
}

static void poll_until(ynet_inlet* in, int64_t until) {
    while (g_now < until) {
        int64_t before = g_now;
        ynet_inlet_poll(in, YRT_MS(10));
        if (g_now == before) g_now += 100000;   /* nothing waited: move on */
    }
}

/* --- tests --------------------------------------------------------------------------- */

static void test_loader(void) {
    ynet_lsl lib;
    bool ok;
    memset(&lib, 0, sizeof lib);
    ynet_lsl_close(&lib);                       /* a zeroed lib: safe */
    ok = ynet_lsl_open(&lib, "ysp-no-such-liblsl-here");
    CHECK(!ok);
    CHECK(strstr(ynet_lsl_error(&lib), "liblsl not found") != NULL);
    CHECK(strstr(ynet_lsl_error(&lib), "ysp-no-such-liblsl-here") != NULL);
    CHECK(lib.handle == NULL);
    ynet_lsl_close(&lib);                       /* after a failed open */
    ynet_lsl_close(&lib);                       /* twice */
    /* a library that is there but is not liblsl */
#if defined(_WIN32)
    ok = ynet_lsl_open(&lib, "C:\\Windows\\System32\\kernel32.dll");
#elif defined(__APPLE__)
    ok = ynet_lsl_open(&lib, "/usr/lib/libSystem.B.dylib");
#else
    ok = ynet_lsl_open(&lib, "libm.so.6");
#endif
    CHECK(!ok);
    CHECK(strstr(ynet_lsl_error(&lib), "lsl_library_version") != NULL);
    CHECK(lib.handle == NULL);
    /* the default names: liblsl is not installed where CI runs this */
    ok = ynet_lsl_open(&lib, NULL);
    if (ok) {
        printf("net_test: liblsl found at %s, version %d\n", lib.path, (int)lib.version);
        CHECK(lib.version >= 113);
        CHECK(ynet_lsl_error(&lib)[0] == '\0');
        ynet_lsl_close(&lib);
    } else {
        CHECK(strstr(ynet_lsl_error(&lib), "YSP_LSL_PATH") != NULL);
    }
    ynet_lsl_close(&lib);
    CHECK(strcmp(ynet_strerror(YNET_ERR_NOLIB), "liblsl not found") == 0);
    CHECK(strcmp(ynet_version(), YNET_VERSION_STRING) == 0);
}

static void test_keys(void) {
    ynet_key k;
    char p[256], big[300];
    CHECK(ynet_key_parse("lsl:Data", &k) && !strcmp(k.name, "Data") && !k.type[0]);
    CHECK(ynet_key_parse("lsl:Data:EEG:dev1:host-a", &k) && !strcmp(k.hostname, "host-a") && !strcmp(k.source_id, "dev1"));
    CHECK(ynet_key_parse("lsl::Markers::", &k) && !k.name[0] && !strcmp(k.type, "Markers"));
    CHECK(!ynet_key_parse("serial:0403:6001::", &k));
    CHECK(!ynet_key_parse("lsl:a:b:c:d:e", &k));          /* a fifth field */
    CHECK(!ynet_key_parse("lsl:o'brien", &k));            /* XPath cannot escape it */
    CHECK(!ynet_key_parse("lsl:a\tb", &k));
    memset(big, 'a', sizeof big);
    memcpy(big, "lsl:", 4);
    big[4 + 64] = '\0';                                   /* a 64-byte name: too long */
    CHECK(!ynet_key_parse(big, &k));
    big[4 + 63] = '\0';
    CHECK(ynet_key_parse(big, &k));
    CHECK(ynet_key_parse("lsl:Data::LabStreamer:", &k) && ynet_key_predicate(&k, p, sizeof p) > 0 &&
          !strcmp(p, "name='Data' and source_id='LabStreamer'"));
    CHECK(ynet_key_parse("lsl::::", &k) && ynet_key_predicate(&k, p, sizeof p) < 0);   /* no field set */
    CHECK(ynet_key_parse("lsl:Data:EEG:dev1:h", &k) && ynet_key_predicate(&k, p, 20) < 0);   /* does not fit */
    CHECK(ynet_key_match("lsl:Data::", "lsl:Data:EEG:x:h"));
    CHECK(ynet_key_match("lsl::EEG", "lsl:Data:EEG:x:h"));
    CHECK(!ynet_key_match("lsl:Data:Markers", "lsl:Data:EEG:x:h"));
    CHECK(!ynet_key_match("lsl:data", "lsl:Data:EEG:x:h"));    /* LSL compares exactly */
}

static void test_stream_ring(void) {
    static unsigned char mem[YNET_STREAM_BYTES(8, 3) + 5];
    ynet_stream s;
    int64_t t[16], tt[8];
    double l[16], ll[8];
    float x[48], y[24];
    int i, n;
    CHECK_I(ynet_stream_init(&s, mem, 10, 3), YNET_ERR_ARG);   /* an init: 0 or a code (v0.2.0) */
    CHECK_I(ynet_stream_init(&s, mem, sizeof mem, 0), YNET_ERR_ARG);
    CHECK_I(ynet_stream_init(&s, mem, sizeof mem, 65), YNET_ERR_ARG);
    CHECK_I(ynet_stream_init(&s, mem + 1, sizeof mem - 1, 3), 0);   /* any alignment */
    CHECK_I(ynet_stream_capacity(&s), 8);
    CHECK(((uintptr_t)s.rec & 7u) == 0);
    CHECK(!ynet_stream_newest(&s, t, l, x));
    for (i = 0; i < 10; i++) { tt[i % 8] = i; }
    for (i = 0; i < 8; i++) { t[i] = 100 + i; l[i] = 0.5 * i; x[3 * i] = (float)i; x[3 * i + 1] = 1; x[3 * i + 2] = 2; }
    CHECK_I(ynet__stream_put(&s, t, l, x, 6, 3), 6);
    CHECK_I(ynet__stream_put(&s, t + 6, l + 6, x + 18, 2, 3), 2);
    CHECK_I(ynet__stream_put(&s, t, l, x, 3, 3), 0);           /* full: refused, counted */
    CHECK_I(ynet_stream_dropped(&s), 3);
    /* the newest sample even when the ring refused it: a display wants it */
    CHECK(ynet_stream_newest(&s, tt, ll, y) && tt[0] == 102 && ll[0] == 1.0 && y[0] == 2.0f);
    n = ynet_stream_read(&s, tt, ll, y, 5);
    CHECK_I(n, 5);
    CHECK(tt[0] == 100 && tt[4] == 104 && y[3 * 4] == 4.0f && y[3 * 4 + 2] == 2.0f && ll[2] == 1.0);
    CHECK_I(ynet__stream_put(&s, t, l, x, 4, 3), 4);           /* wraps */
    n = ynet_stream_read(&s, tt, NULL, NULL, 8);
    CHECK_I(n, 7);
    CHECK(tt[0] == 105 && tt[2] == 107 && tt[3] == 100 && tt[6] == 103);
    CHECK_I(ynet_stream_read(&s, tt, ll, y, 8), 0);
    (void)tt; (void)ll;
}

/* Markers out and in, one process, the virtual clock. */
static void test_markers(void) {
    static ynet_outlet o, oi;
    static ynet_inlet in, ii;
    ynet_outlet_desc od;
    ynet_inlet_desc id;
    ynet_out_info info;
    ynet_stats st;
    int i, ok_time = 1, ok_unc = 1;
    int64_t t_before[20], t_after[20];
    const yrt_event* r;
    reset_fake();
    memset(&od, 0, sizeof od);
    od.lib = &g_lib;
    od.role = "marks";
    od.key = "lsl:ysp-markers:Markers:booth2";
    od.device = 3;
    od.ring = &g_ring;
    od.now = now_fn;
    CHECK(ynet_outlet_open(&o, &od));
    CHECK_I(ynet_outlet_consumers(&o), 0);
    memset(&id, 0, sizeof id);
    id.lib = &g_lib;
    id.role = "marks_in";
    id.key = "lsl:ysp-markers::booth2:testhost";
    id.device = 4;
    id.format = YNET_STRING;
    id.sink = sink;
    id.text_sink = text_sink;
    id.ring = &g_ring;
    id.manual = true;
    id.now = now_fn;
    CHECK(ynet_inlet_open(&in, &id));
    CHECK_I(ynet_inlet_state(&in), YNET_OPENING);
    poll_until(&in, g_now + 100000000);
    CHECK_I(ynet_inlet_state(&in), YNET_RUNNING);
    CHECK_I(ynet_outlet_consumers(&o), 1);
    for (i = 0; i < 20; i++) {
        char text[64];
        g_now += 50000000 + (int64_t)(u01() * 1e7);
        snprintf(text, sizeof text, i == 7 ? "%d" : "trial %d target_on and a long tail of text", i + 100);
        CHECK_I(ynet_out_mark(&o, (uint32_t)(i + 1), text), YNET_OK);
        ynet_out_last(&o, &info);
        t_before[i] = info.t_before;
        t_after[i] = info.t_after;
        CHECK(info.seq == (uint32_t)(i + 1) && info.code == (uint32_t)(i + 1) && (info.flags & YNET_OUT_MARK));
        poll_until(&in, g_now + 5000000);
    }
    CHECK_I(g_nev, 20);
    for (i = 0; i < g_nev && i < 20; i++) {
        /* the event is the push, within the fits' uncertainty */
        int64_t err = g_ev[i].t - (t_before[i] + t_after[i]) / 2;
        if (llabs(err) > (int64_t)g_ev[i].unc_us * 1000 + 1000) ok_time = 0;
        if (g_ev[i].unc_us == 0 || g_ev[i].unc_us == 65535) ok_unc = 0;
        CHECK(g_ev[i].kind == YIN_KIND_SYNC && g_ev[i].device == 4 && g_ev[i].type == YIN_PRESS);
        CHECK(g_ev[i].flags & YIN_DEVTICKS);
    }
    CHECK(ok_time);
    CHECK(ok_unc);
    CHECK_I(g_ev[7].code, 107);                                 /* a number as text */
    CHECK_I(g_ev[6].code, 0);
    CHECK(strcmp(g_txt[6], "trial 106 target_on and a long tail of text") == 0);
    /* mark_at: a stamp of a ysp_rt time, 12.345 ms back */
    {
        int64_t target = g_now - 12345000;
        CHECK_I(ynet_out_mark_at(&o, 77, NULL, target), YNET_OK);
        ynet_out_last(&o, &info);
        CHECK((info.flags & YNET_OUT_AT) && !(info.flags & YNET_OUT_MARK) && info.t_target == target);
        poll_until(&in, g_now + 5000000);
        CHECK_I(g_nev, 21);
        CHECK(llabs(g_ev[20].t - target) <= (int64_t)g_ev[20].unc_us * 1000 + 1000);
        CHECK_I(g_ev[20].code, 77);                             /* the code as decimal text */
        CHECK(fabs(ynet_outlet_to_lsl(&o, target) - local_of(target)) < 1e-6);
        CHECK_I(ynet_out_mark_at(&o, 1, NULL, 0), YNET_ERR_ARG);
    }
    CHECK_I(ynet_out_push(&o, NULL, 0, NULL), YNET_ERR_FORMAT);
    ynet_inlet_stats(&in, &st);
    CHECK_I(st.events, 21);
    CHECK_I(st.clamped, 0);
    CHECK_I(st.refused, 0);
    CHECK(st.corrections >= 1 && st.local.ready);
    {   /* v0.3.0: liblsl's correction in ns, rounded to nearest */
        const fk_inlet* f = (const fk_inlet*)in.inlet;
        CHECK(st.lsl_offset_ns == (int64_t)llround(f->est_off * 1e9));
        CHECK(st.lsl_rtt_ns == (int64_t)llround(f->est_unc * 1e9) && st.lsl_rtt_ns >= 50000 && st.lsl_rtt_ns <= 150000);
    }
    CHECK(!strcmp(st.found.name, "ysp-markers") && !strcmp(st.found.type, "Markers") && !strcmp(st.uid, "uid-0"));
    CHECK(llabs(ynet_inlet_map(&in, local_of(g_now)) - g_now) < 100000);
    /* a mark stamped 50 ms ahead arrives before its time: clamped to its read */
    {
        int64_t ahead = g_now + 50000000;
        CHECK_I(ynet_out_mark_at(&o, 5, NULL, ahead), YNET_OK);
        poll_until(&in, g_now + 5000000);
        ynet_inlet_stats(&in, &st);
        CHECK_I(st.clamped, 1);
        CHECK(g_nev == 22 && g_ev[21].t < ahead - 40000000 && g_ev[21].t <= g_now);
    }
    /* records */
    ring_drain();
    CHECK_I(count_rec(YRT_SRC_NET, YNET_REC_OUT), 22);
    r = find_rec(YRT_SRC_NET, YNET_REC_OUT, 0);
    CHECK(r && r->aux == 3 && r->u.i64[0] == t_before[0] && r->u.i64[1] == t_after[0] && r->u.u32[8] == 1 &&
          r->u.u16[19] == 1 && (r->u.u16[18] & YNET_OUT_MARK) && r->u.i64[3] == 0);
    CHECK(r && llabs(r->u.i64[2] - (int64_t)(local_of(t_before[0]) * 1e9)) < 2000);
    CHECK(find_text("mark trial 100 target_on and a long tai"));
    CHECK(find_text("in trial 100"));
    CHECK(find_text("role marks"));
    CHECK(count_rec(YRT_SRC_NET, YNET_REC_OFFSET) >= 1);
    CHECK_I(count_rec(YRT_SRC_NET, YNET_REC_STREAM), 1);
    CHECK(find_text("host testhost"));
    /* int32 markers */
    memset(&od, 0, sizeof od);
    od.lib = &g_lib; od.role = "codes"; od.key = "lsl:ysp-codes:Markers"; od.device = 5; od.format = YNET_INT32;
    od.now = now_fn;
    CHECK(ynet_outlet_open(&oi, &od));
    memset(&id, 0, sizeof id);
    id.lib = &g_lib; id.role = "codes_in"; id.key = "lsl:ysp-codes"; id.device = 6; id.format = YNET_INT32;
    id.kind = YIN_KIND_BOX; id.sink = sink; id.manual = true; id.now = now_fn;
    CHECK(ynet_inlet_open(&ii, &id));
    poll_until(&ii, g_now + 100000000);
    g_nev = 0;
    CHECK_I(ynet_out_mark(&oi, 254, "text goes to the log only"), YNET_OK);
    poll_until(&ii, g_now + 5000000);
    CHECK(g_nev == 1 && g_ev[0].code == 254 && g_ev[0].kind == YIN_KIND_BOX && g_ev[0].device == 6);
    ynet_inlet_close(&ii);
    ynet_outlet_close(&oi);
    ynet_inlet_close(&in);
    CHECK_I(ynet_inlet_state(&in), YNET_CLOSED);
    ynet_outlet_close(&o);
    CHECK_I(ynet_out_mark(&o, 1, NULL), YNET_ERR_STATE);
    CHECK_I(fk_infos_alive(), 0);
    CHECK_I(fk_inlets_alive(), 0);
    CHECK_I(g_strings, 0);
}

/* A ramp from 0 to 2 over 1000 samples with noise of +-0.08: it crosses
 * 1.0 several times near the middle. */
static float ramp(int64_t k, int c) {
    double v = k < 1000 ? 2.0 * (double)k / 1000.0 : 2.0;
    (void)c;
    return (float)(v + (u01() - 0.5) * 0.16);
}

static float toggle(int64_t k, int c) { return c == 1 ? (float)((k / 10) % 2) : 0.0f; }

static float square(int64_t k, int c) {
    /* channel 0: a 50 Hz square wave of 0 and 2 with noise of 0.05; 1: a ramp */
    if (c == 0) return ((k / 100) % 2 ? 2.0f : 0.0f) + (float)((u01() - 0.5) * 0.1);
    return (float)(k % 1000) / 1000.0f;
}

/* A remote 10 kHz, 6-channel stream: the clock map, edges, gaps, the ring. */
static void test_remote_stream(void) {
    static ynet_inlet in;
    static unsigned char mem[YNET_STREAM_BYTES(1 << 16, 6)];
    static ynet_stream ring;
    static int64_t tt[4096];
    static double ll[4096];
    static float xx[4096 * 6];
    ynet_inlet_desc id;
    ynet_stats st;
    int idx, i, n, edges = 0, edges_ok = 1;
    int64_t h0, total = 0, worst_late = 0, worst_early = 0, sum_err = 0;
    double max_abs_after = 0;
    reset_fake();
    g_roff = 987654321000LL;    /* another clock, 987.654321 s apart */
    g_rppm = 50.0;              /* and 50 ppm fast */
    g_rtt_ns = 400000;          /* corrections with 0.2 to 0.6 ms round trips */
    idx = fk_remote_stream("Data", "LabStreamer", "ls-0001", YNET_FLOAT32, 6, 10000.0);
    h0 = g_now + 200000000;
    fk_remote_samples(idx, h0, 200000, 10000.0, square, 0);          /* 20 s */
    CHECK_I(ynet_stream_init(&ring, mem, sizeof mem, 6), 0);
    memset(&id, 0, sizeof id);
    id.lib = &g_lib; id.role = "ref"; id.key = "lsl:Data::"; id.device = 7;
    id.format = YNET_FLOAT32; id.channels = 6; id.stream = &ring;
    id.edges[0].channel = 0; id.edges[0].level = 1.0f; id.edges[0].hysteresis = 0.4f;
    id.n_edges = 1;
    id.sink = sink; id.ring = &g_ring; id.manual = true; id.now = now_fn;
    CHECK(ynet_inlet_open(&in, &id));
    while (g_now < h0 + 21000000000LL) {
        int64_t before = g_now;
        ynet_inlet_poll(&in, YRT_MS(10));
        if (g_now == before) g_now += 100000;
        while ((n = ynet_stream_read(&ring, tt, ll, xx, 4096)) > 0)
            for (i = 0; i < n; i++) {
                /* the truth: the host time whose remote clock reads ll[i] */
                double hs = ((ll[i] * 1e9 - 1000000000000.0 - (double)g_roff) / (1.0 + g_rppm * 1e-6)) + 1000000000000.0;
                int64_t err = tt[i] - (int64_t)llround(hs);
                if (tt[i] - h0 > 10000000000LL && fabs((double)err) > max_abs_after) max_abs_after = fabs((double)err);
                if (err > worst_late) worst_late = err;
                if (err < worst_early) worst_early = err;
                sum_err += err;
                total++;
            }
    }
    ynet_inlet_stats(&in, &st);
    CHECK_I(total, 200000);
    CHECK_I(st.samples, 200000);
    CHECK_I(st.gaps, 0);
    CHECK_I(st.dropped, 0);
    CHECK(st.remote.ready && st.remote.slope);
    CHECK(fabs(st.remote.ppm - (-50.0 / (1.0 + 50e-6))) < 5.0);   /* the remote tick is short by 50 ppm */
    printf("net_test: remote 10 kHz stream, 50 ppm, round trips 0.2 to 0.6 ms: map error after 10 s max |%.1f| us; "
           "all: %+.1f to %+.1f us, mean %+.1f us; remote fit %+.2f ppm, width %.1f us; unc_us %lld\n",
           max_abs_after / 1e3, (double)worst_early / 1e3, (double)worst_late / 1e3,
           (double)sum_err / (double)(total ? total : 1) / 1e3, st.remote.ppm, (double)st.remote.width_ns / 1e3,
           (long long)((in.unc_ns + 999) / 1000));
    CHECK(max_abs_after < 400000.0);     /* within the round trips' half widths */
    /* edges: every 100 samples, at the first sample past the band */
    for (i = 0; i < g_nev; i++) {
        int64_t k, err;
        if (g_ev[i].device != 7) continue;
        k = 100 * (int64_t)(edges + 1);                         /* the sample of this edge */
        err = g_ev[i].t - (h0 + k * 100000);
        if (llabs(err) > 1000000 || g_ev[i].type != ((k / 100) % 2 ? YIN_PRESS : YIN_RELEASE) || g_ev[i].control != 1)
            edges_ok = 0;
        edges++;
    }
    CHECK_I(edges, 1999);
    CHECK(edges_ok);
    ynet_inlet_close(&in);
    ring_drain();
    CHECK(count_rec(YRT_SRC_RT, YRT_KIND_FIT) > 5);
    CHECK(count_rec(YRT_SRC_NET, YNET_REC_OFFSET) >= 9);
    CHECK_I(fk_infos_alive(), 0);
}

/* Before any correction: read times, unc 65535. Then gaps, silence,
 * disappear and return. */
static void test_lifecycle(void) {
    static ynet_inlet in;
    static unsigned char mem[YNET_STREAM_BYTES(256, 2)];
    static ynet_stream ring;
    ynet_inlet_desc id;
    ynet_stats st;
    int idx, idx2, n;
    int64_t t_lost, t_back, h;
    const yrt_event* r;
    reset_fake();
    g_corr_fail = 1;
    idx = fk_remote_stream("Force", "Analog", "f1", YNET_FLOAT32, 2, 1000.0);
    CHECK_I(ynet_stream_init(&ring, mem, sizeof mem, 2), 0);
    memset(&id, 0, sizeof id);
    id.lib = &g_lib; id.role = "force"; id.key = "lsl:Force"; id.device = 9; id.stream = &ring;
    id.edges[0].channel = 1; id.edges[0].level = 0.5f; id.n_edges = 1;
    id.sink = sink; id.ring = &g_ring; id.manual = true; id.now = now_fn;
    CHECK(ynet_inlet_open(&in, &id));
    poll_until(&in, g_now + 10000000);
    CHECK_I(ynet_inlet_state(&in), YNET_RUNNING);
    h = g_now;
    fk_remote_samples(idx, h, 100, 1000.0, toggle, 0);
    fk_remote_samples(idx, h + 105000000, 100, 1000.0, toggle, 105);   /* 5 samples missing */
    poll_until(&in, h + 220000000);
    ynet_inlet_stats(&in, &st);
    CHECK_I(st.samples, 200);
    CHECK_I(st.gaps, 1);
    CHECK_I(st.missing, 5);
    CHECK_I(st.corrections, 0);
    n = ynet_stream_read(&ring, NULL, NULL, NULL, 256);
    CHECK_I(n, 200);
    /* the ring holds 256: 300 more overflow it */
    fk_remote_samples(idx, h + 220000000, 300, 1000.0, NULL, 0);
    poll_until(&in, h + 530000000);
    ynet_inlet_stats(&in, &st);
    CHECK_I(st.dropped, 44);
    CHECK_I(ynet_stream_dropped(&ring), 44);
    ring_drain();
    CHECK_I(count_rec(YRT_SRC_NET, YNET_REC_DROP), 1);
    r = find_rec(YRT_SRC_NET, YNET_REC_STREAM_GAP, 0);
    CHECK(r && r->u.u64[2] == 5 && r->aux == 9);
    (void)ynet_stream_read(&ring, NULL, NULL, NULL, 256);
    /* silence: no sample for 1 s */
    poll_until(&in, h + 1700000000);
    CHECK_I(ynet_inlet_state(&in), YNET_LOST);
    /* it sends again: running, a gap record */
    g_corr_fail = 0;
    g_out[idx].n = 0;
    t_lost = g_now;
    fk_remote_samples(idx, g_now + 100000000, 3000, 1000.0, NULL, 0);
    poll_until(&in, g_now + 2500000000LL);
    CHECK_I(ynet_inlet_state(&in), YNET_RUNNING);
    ynet_inlet_stats(&in, &st);
    CHECK_I(st.opens, 2);
    CHECK(st.corrections >= 1);
    /* the outlet goes away: LOST at once (liblsl's lost error); another
     * process makes it again: RUNNING, and the remote fit starts over */
    g_out[idx].alive = 0;
    poll_until(&in, g_now + 50000000);
    CHECK_I(ynet_inlet_state(&in), YNET_LOST);
    t_back = g_now + 1500000000LL;
    poll_until(&in, t_back);
    CHECK_I(ynet_inlet_state(&in), YNET_LOST);         /* nothing at the key */
    g_out[idx].used = 0;
    g_roff = 5000000000LL;                              /* another clock now */
    idx2 = fk_remote_stream("Force", "Analog", "f1", YNET_FLOAT32, 2, 1000.0);
    fk_remote_samples(idx2, g_now + 100000000, 2000, 1000.0, NULL, 0);
    {
        /* right after the reopen, the map is already the new clock's */
        int64_t until = g_now + 2500000000LL;
        while (g_now < until && ynet_inlet_state(&in) != YNET_RUNNING) poll_until(&in, g_now + 1000000);
        CHECK_I(ynet_inlet_state(&in), YNET_RUNNING);
        CHECK(llabs(ynet_inlet_map(&in, remote_of(g_now)) - g_now) < 1000000);
        poll_until(&in, until);
    }
    CHECK_I(ynet_inlet_state(&in), YNET_RUNNING);
    ynet_inlet_stats(&in, &st);
    CHECK_I(st.opens, 3);
    CHECK(llabs(ynet_inlet_map(&in, remote_of(g_now)) - g_now) < 1000000);   /* the new clock */
    ynet_inlet_close(&in);
    ring_drain();
    CHECK_I(count_rec(YRT_SRC_NET, YNET_REC_GAP), 2);
    r = find_rec(YRT_SRC_NET, YNET_REC_GAP, 0);
    CHECK(r && r->u.i64[0] < t_lost && r->u.i64[1] > t_lost && r->u.i64[1] - r->u.i64[0] > 1000000000);
    {
        int i, silent = 0, disc = 0;
        for (i = 0; i < g_nrec; i++)
            if (g_rec[i].source == YRT_SRC_NET && g_rec[i].kind == YNET_REC_STATE && g_rec[i].u.u32[1] == YNET_LOST) {
                silent += g_rec[i].u.u32[2] == YNET_WHY_SILENT;
                disc += g_rec[i].u.u32[2] == YNET_WHY_DISCONNECTED;
            }
        CHECK_I(silent, 1);
        CHECK_I(disc, 1);
    }
    (void)t_back;
    /* events before any correction: the read time, unc 65535 */
    {
        int i, seen = 0;
        for (i = 0; i < g_nev; i++)
            if (g_ev[i].device == 9 && g_ev[i].unc_us == 65535) seen++;
        CHECK(seen > 0);
    }
    CHECK_I(fk_infos_alive(), 0);
    CHECK_I(fk_inlets_alive(), 0);
}

/* Hysteresis, and corrections whose round trip is too wide. */
static void test_band_and_refusal(void) {
    static ynet_inlet in;
    ynet_inlet_desc id;
    ynet_stats st;
    int idx, i, press = 0, release = 0;
    reset_fake();
    g_rtt_ns = 16000000;                    /* 8 to 24 ms round trips: over 5 ms */
    idx = fk_remote_stream("Ramp", "Analog", "r1", YNET_FLOAT32, 1, 1000.0);
    fk_remote_samples(idx, g_now + 100000000, 2000, 1000.0, ramp, 0);
    memset(&id, 0, sizeof id);
    id.lib = &g_lib; id.role = "ramp"; id.key = "lsl:Ramp"; id.device = 12; id.manual = true; id.now = now_fn;
    id.edges[0].channel = 0; id.edges[0].level = 1.0f; id.edges[0].hysteresis = 0.4f; id.n_edges = 1;
    id.sink = sink;
    CHECK(ynet_inlet_open(&in, &id));
    g_nev = 0;
    poll_until(&in, g_now + 2500000000LL);
    for (i = 0; i < g_nev; i++) {
        press += g_ev[i].type == YIN_PRESS;
        release += g_ev[i].type == YIN_RELEASE;
    }
    CHECK_I(press, 1);                      /* noise inside the band: one crossing */
    CHECK_I(release, 0);
    ynet_inlet_stats(&in, &st);
    CHECK(st.corrections >= 1 && st.refused == st.corrections);
    CHECK(!st.remote.ready);                /* the fit took none of them */
    CHECK(g_nev == 1 && g_ev[0].unc_us >= 4000);   /* half the round trip at least */
    ynet_inlet_close(&in);
}

static void test_resolve(void) {
    static ynet_inlet in;
    ynet_inlet_desc id;
    ynet_stats st;
    int64_t t0;
    int calls;
    reset_fake();
    memset(&id, 0, sizeof id);
    id.lib = &g_lib; id.role = "none"; id.key = "lsl:NoSuchStream"; id.device = 2; id.manual = true; id.now = now_fn;
    id.ring = &g_ring;
    id.resolve_ns = 250000000;
    CHECK(ynet_inlet_open(&in, &id));
    t0 = g_now;
    calls = g_resolve_calls;
    poll_until(&in, t0 + 5050000000LL);
    ynet_inlet_stats(&in, &st);
    CHECK_I(ynet_inlet_state(&in), YNET_OPENING);
    CHECK(st.resolves >= 4 && st.resolves <= 6);          /* one a second */
    CHECK_I(g_resolve_calls - calls, (int)st.resolves);
    CHECK(fabs(g_resolve_timeout - 0.25) < 1e-9);
    ynet_inlet_close(&in);
    /* two streams match: no match, said once */
    reset_fake();
    (void)fk_remote_stream("Twin", "Markers", "a", YNET_STRING, 1, 0.0);
    (void)fk_remote_stream("Twin", "Markers", "b", YNET_STRING, 1, 0.0);
    id.key = "lsl:Twin";
    CHECK(ynet_inlet_open(&in, &id));
    poll_until(&in, g_now + 3500000000LL);
    ynet_inlet_stats(&in, &st);
    CHECK_I(ynet_inlet_state(&in), YNET_OPENING);
    CHECK(st.ambiguous >= 3);
    ynet_inlet_close(&in);
    ring_drain();
    CHECK(find_text("ambiguous 2 streams match"));
    {
        int i, n = 0;
        for (i = 0; i < g_nrec; i++)
            n += g_rec[i].kind == YNET_REC_TEXT && strncmp(g_rec[i].u.text, "ambiguous", 9) == 0;
        CHECK_I(n, 1);
    }
    /* the source_id tells them apart */
    ring_open();
    id.key = "lsl:Twin::b";
    CHECK(ynet_inlet_open(&in, &id));
    poll_until(&in, g_now + 1000000000LL);
    CHECK_I(ynet_inlet_state(&in), YNET_RUNNING);
    ynet_inlet_close(&in);
    /* the wrong stream: FAILED, not tried again */
    id.key = "lsl:Twin::a";
    id.format = YNET_FLOAT32;
    CHECK(ynet_inlet_open(&in, &id));
    poll_until(&in, g_now + 3000000000LL);
    ynet_inlet_stats(&in, &st);
    CHECK_I(ynet_inlet_state(&in), YNET_FAILED);
    CHECK_I(st.resolves, 1);
    CHECK(strstr(ynet_inlet_error(&in), "format 3") != NULL);
    ynet_inlet_close(&in);
    id.format = YNET_STRING;
    id.channels = 2;
    CHECK(ynet_inlet_open(&in, &id));
    poll_until(&in, g_now + 1000000000LL);
    CHECK_I(ynet_inlet_state(&in), YNET_FAILED);
    ynet_inlet_close(&in);
    /* an edge on a channel the stream lacks */
    id.channels = 0;
    id.format = 0;
    id.n_edges = 1;
    id.edges[0].channel = 3;
    CHECK(ynet_inlet_open(&in, &id));
    poll_until(&in, g_now + 1000000000LL);
    CHECK_I(ynet_inlet_state(&in), YNET_FAILED);
    ynet_inlet_close(&in);
    CHECK_I(fk_infos_alive(), 0);
    CHECK_I(fk_inlets_alive(), 0);
}

static void test_desc(void) {
    static ynet_inlet in;
    static ynet_outlet o;
    static ynet_stream bad;
    ynet_inlet_desc id;
    ynet_outlet_desc od;
    ynet_lsl empty;
    reset_fake();
    memset(&empty, 0, sizeof empty);
    memset(&id, 0, sizeof id);
    id.lib = &g_lib; id.role = "r"; id.key = "lsl:x"; id.device = 1; id.manual = true; id.now = now_fn;
    CHECK(ynet_inlet_open(&in, &id));
    ynet_inlet_close(&in);
    id.lib = &empty;
    CHECK(!ynet_inlet_open(&in, &id) && strstr(ynet_inlet_error(&in), "desc.lib"));
    id.lib = &g_lib;
    id.key = "lsl:::";
    CHECK(!ynet_inlet_open(&in, &id) && strstr(ynet_inlet_error(&in), "desc.key"));
    id.key = "serial:x";
    CHECK(!ynet_inlet_open(&in, &id));
    id.key = "lsl:x";
    id.device = 0;
    CHECK(!ynet_inlet_open(&in, &id));
    id.device = 1;
    id.format = 2;              /* double64: not supported */
    CHECK(!ynet_inlet_open(&in, &id));
    id.format = 0;
    id.channels = 65;
    CHECK(!ynet_inlet_open(&in, &id));
    id.channels = 0;
    id.n_edges = 9;
    CHECK(!ynet_inlet_open(&in, &id));
    id.n_edges = 1;
    id.edges[0].channel = 64;
    CHECK(!ynet_inlet_open(&in, &id));
    id.edges[0].channel = 0;
    id.edges[0].hysteresis = -1;
    CHECK(!ynet_inlet_open(&in, &id));
    id.n_edges = 0;
    memset(&bad, 0, sizeof bad);
    id.stream = &bad;
    CHECK(!ynet_inlet_open(&in, &id) && strstr(ynet_inlet_error(&in), "desc.stream"));
    id.stream = NULL;
    id.role = NULL;
    CHECK(!ynet_inlet_open(&in, &id));
    CHECK(!ynet_inlet_open(&in, NULL));
    memset(&od, 0, sizeof od);
    od.lib = &g_lib; od.role = "o"; od.key = "lsl::Markers"; od.device = 1; od.now = now_fn;
    CHECK(!ynet_outlet_open(&o, &od) && strstr(ynet_outlet_error(&o), "with a name"));
    od.key = "lsl:m";
    od.channels = 2;
    CHECK(!ynet_outlet_open(&o, &od) && strstr(ynet_outlet_error(&o), "one channel"));
    od.channels = 0;
    od.rate_hz = -1;
    CHECK(!ynet_outlet_open(&o, &od));
    od.rate_hz = 0;
    od.device = 0;
    CHECK(!ynet_outlet_open(&o, &od));
    od.device = 1;
    CHECK(ynet_outlet_open(&o, &od));
    ynet_outlet_close(&o);
    ynet_outlet_close(&o);       /* twice is safe */
    ynet_inlet_close(&in);
    {
        /* key and role are copied into the handles (v0.2.0) */
        char key[64], role[16], longrole[YNET_ROLE_MAX + 1];
        snprintf(key, sizeof key, "%s", "lsl:x");
        snprintf(role, sizeof role, "%s", "r");
        id.key = key;
        id.role = role;
        CHECK(ynet_inlet_open(&in, &id));
        key[0] = role[0] = '\0';
        CHECK(strcmp(in.d.key, "lsl:x") == 0 && strcmp(in.d.role, "r") == 0);
        ynet_inlet_close(&in);
        snprintf(key, sizeof key, "%s", "lsl:m");
        snprintf(role, sizeof role, "%s", "o");
        od.key = key;
        od.role = role;
        CHECK(ynet_outlet_open(&o, &od));
        key[0] = role[0] = '\0';
        CHECK(strcmp(o.d.key, "lsl:m") == 0 && strcmp(o.d.role, "o") == 0);
        ynet_outlet_close(&o);
        memset(longrole, 'r', sizeof longrole);
        longrole[YNET_ROLE_MAX] = '\0';       /* 32 characters: one too many */
        id.key = "lsl:x";
        id.role = longrole;
        CHECK(!ynet_inlet_open(&in, &id) && strstr(ynet_inlet_error(&in), "YNET_ROLE_MAX"));
        od.key = "lsl:m";
        od.role = longrole;
        CHECK(!ynet_outlet_open(&o, &od) && strstr(ynet_outlet_error(&o), "YNET_ROLE_MAX"));
        ynet_inlet_close(&in);      /* after a failed open */
        ynet_outlet_close(&o);
    }
    CHECK_I(fk_infos_alive(), 0);
}

/* ynet_inlet_open() returns while OPENING; the wait (v0.2.0). */
static void test_wait(void) {
    static ynet_outlet o;
    static ynet_inlet in;
    ynet_outlet_desc od;
    ynet_inlet_desc id;
    reset_fake();
    memset(&od, 0, sizeof od);
    od.lib = &g_lib; od.role = "marks"; od.key = "lsl:ysp-wait:Markers:w1"; od.device = 3; od.now = now_fn;
    CHECK(ynet_outlet_open(&o, &od));
    memset(&id, 0, sizeof id);
    id.lib = &g_lib; id.role = "in"; id.key = "lsl:ysp-wait::w1"; id.device = 4; id.manual = true; id.now = now_fn;
    id.format = YNET_STRING;
    CHECK(ynet_inlet_open(&in, &id));
    CHECK_I(ynet_inlet_state(&in), YNET_OPENING);
    CHECK_I(ynet_inlet_wait_running(&in, YRT_S(2)), YNET_RUNNING);
    CHECK_I(ynet_inlet_wait_running(&in, 0), YNET_RUNNING);
    ynet_inlet_close(&in);
    CHECK_I(ynet_inlet_wait_running(&in, YRT_S(1)), YNET_ERR_STATE);
    CHECK(strstr(ynet_inlet_error(&in), "not open") != NULL);
    /* a wrong stream: FAILED at once, the reason kept */
    id.format = YNET_INT32;
    CHECK(ynet_inlet_open(&in, &id));
    CHECK_I(ynet_inlet_wait_running(&in, YRT_S(2)), YNET_ERR_STATE);
    CHECK(strstr(ynet_inlet_error(&in), "has format") != NULL);
    ynet_inlet_close(&in);
    /* nobody sends: a timeout with a message */
    id.format = YNET_STRING;
    id.key = "lsl:ysp-nobody::";
    CHECK(ynet_inlet_open(&in, &id));
    CHECK_I(ynet_inlet_wait_running(&in, YRT_MS(100)), YNET_ERR_TIMEOUT);
    CHECK(strstr(ynet_inlet_error(&in), "not running in time") != NULL);
    ynet_inlet_close(&in);
    ynet_outlet_close(&o);
    CHECK_I(fk_infos_alive(), 0);
    printf("  the wait for RUNNING: running, closed, a wrong stream, nobody: ok\n");
}

/* A synthetic LabStreamer in this process: an outlet with the Data stream's
 * name, format, channels and rate; the inlet as the rig binds it. Also the
 * trigger channel and role glue. */
static yin_event g_edge_ev[64];
static int g_nedge;
static void edge_sink(void* ctx, const yin_event* e) { (void)ctx; if (g_nedge < 64) g_edge_ev[g_nedge++] = *e; }

static void test_labstreamer_like(void) {
    static ynet_outlet data, marks;
    static ynet_inlet in, lat;
    static unsigned char mem[YNET_STREAM_BYTES(1 << 15, 6)];
    static ynet_stream ring;
    static const char* const names[] = { "black", "white" };
    static ydev_roles roles;
    ynet_outlet_desc od;
    ynet_inlet_desc id;
    ynet_stats st;
    yscr_trigger_desc ch;
    yscr_trigger_info ti;
    float x[6 * 100];
    int i, flip_ok = 1;
    int64_t onset[10];
    reset_fake();
    memset(&roles, 0, sizeof roles);
    CHECK_I(ynet_role(&roles, "marks"), 1);
    CHECK_I(ynet_role(&roles, "ref"), 2);
    CHECK_I(ynet_role(&roles, "marks"), 1);
    CHECK_I(ynet_role(&roles, "a-name-that-is-longer-than-31-bytes"), 0);
    CHECK(ydev_role_device(&roles, 1) == NULL);
    memset(&od, 0, sizeof od);
    od.lib = &g_lib; od.role = "ls_data"; od.key = "lsl:Data:LabStreamer:ls-sim"; od.device = 20;
    od.format = YNET_FLOAT32; od.channels = 6; od.rate_hz = 10000.0; od.now = now_fn;
    CHECK(ynet_outlet_open(&data, &od));
    memset(&od, 0, sizeof od);
    od.lib = &g_lib; od.role = "marks"; od.key = "lsl:ysp-flips:Markers"; od.device = ynet_role(&roles, "marks");
    od.code_names = names; od.n_code_names = 2; od.ring = &g_ring; od.now = now_fn;
    CHECK(ynet_outlet_open(&marks, &od));
    CHECK_I(ynet_stream_init(&ring, mem, sizeof mem, 6), 0);
    memset(&id, 0, sizeof id);
    id.lib = &g_lib; id.role = "ref"; id.key = "lsl:Data::"; id.device = ynet_role(&roles, "ref");
    id.format = YNET_FLOAT32; id.channels = 6; id.stream = &ring;
    id.edges[0].channel = 0; id.edges[0].level = 1.0f; id.edges[0].hysteresis = 0.2f; id.n_edges = 1;
    id.sink = edge_sink; id.manual = true; id.now = now_fn;
    CHECK(ynet_inlet_open(&in, &id));
    memset(&id, 0, sizeof id);
    id.lib = &g_lib; id.role = "lat"; id.key = "lsl:ysp-flips"; id.device = 21; id.format = YNET_STRING;
    id.sink = sink; id.text_sink = text_sink; id.manual = true; id.now = now_fn;
    CHECK(ynet_inlet_open(&lat, &id));
    poll_until(&in, g_now + 50000000);
    poll_until(&lat, g_now + 50000000);
    CHECK_I(ynet_inlet_state(&in), YNET_RUNNING);
    CHECK_I(ynet_inlet_state(&lat), YNET_RUNNING);
    g_nev = 0;
    g_nedge = 0;
    ch = ynet_trigger_channel(&marks, 0);
    CHECK(ch.ctx == &marks && !strcmp(ch.name, "marks"));
    /* 10 flips; the light changes 5.3 ms after each deadline; the Data
     * stream runs on without a break, pushed in blocks after the fact */
    {
        int64_t s0 = g_now, chg[10];
        int64_t next_k = 0;
        for (i = 0; i < 10; i++) {
            int64_t t0 = g_now, upto;
            onset[i] = t0 + 3000000;
            chg[i] = onset[i] + 5300000;
            memset(&ti, 0, sizeof ti);
            ti.code = (uint32_t)((i + 1) % 2);
            ti.deadline_ns = onset[i];
            g_now = onset[i];                                 /* the worker runs at the deadline */
            ch.fn(ch.ctx, &ti);
            g_now = onset[i] + 12000000;                      /* the samples, after they happened */
            upto = (g_now - s0) / 100000;
            while (next_k < upto) {
                double ts[100];
                int m = 0;
                for (; m < 100 && next_k < upto; m++, next_k++) {
                    int64_t h = s0 + next_k * 100000;
                    int light = 0, c, j;
                    for (j = 0; j <= i; j++) if (h >= chg[j]) light = (j + 1) % 2;
                    for (c = 0; c < 6; c++) x[6 * m + c] = c == 0 ? (light ? 2.0f : 0.0f) : (float)c;
                    ts[m] = local_of(h);
                }
                CHECK_I(ynet_out_push(&data, x, m, ts), YNET_OK);
            }
            poll_until(&in, g_now + 3000000);
            poll_until(&lat, g_now + 1000000);
        }
    }
    ti.flags = YSCR_TRIG_FLUSHED;
    ch.fn(ch.ctx, &ti);                                       /* the screen's close: no marker */
    poll_until(&lat, g_now + 5000000);
    CHECK_I(g_nev, 10);
    for (i = 0; i < 10 && i < g_nev; i++) {
        if (llabs(g_ev[i].t - onset[i]) > (int64_t)g_ev[i].unc_us * 1000 + 1000) flip_ok = 0;   /* the deadline */
        if (strcmp(g_txt[i], names[(i + 1) % 2])) flip_ok = 0;
    }
    CHECK(flip_ok);
    /* the light edges, 5.3 ms after the deadline, to the sample (100 us) */
    CHECK_I(g_nedge, 10);
    flip_ok = 1;
    for (i = 0; i < g_nedge && i < 10; i++) {
        int64_t d = g_edge_ev[i].t - onset[i], u = (int64_t)g_edge_ev[i].unc_us * 1000;
        if (d < 5300000 - u || d > 5400000 + u) {
            flip_ok = 0;
            fprintf(stderr, "net_test: edge %d at %+.1f us after its deadline, unc %d us\n", i, (double)d / 1e3,
                    (int)g_edge_ev[i].unc_us);
        }
        if (g_edge_ev[i].type != (i % 2 == 0 ? YIN_PRESS : YIN_RELEASE)) flip_ok = 0;
    }
    CHECK(flip_ok);
    ynet_inlet_stats(&in, &st);
    CHECK(st.samples > 1000 && st.samples == data.samples);
    CHECK_I(st.gaps, 0);
    ring_drain();
    {
        const yrt_event* r = find_rec(YRT_SRC_NET, YNET_REC_OUT, 0);
        CHECK(r && (r->u.u16[18] & YNET_OUT_FLIP) && (r->u.u16[18] & YNET_OUT_AT) && r->u.i64[3] == onset[0] && r->aux == 1);
    }
    CHECK_I(count_rec(YRT_SRC_NET, YNET_REC_OUT), 10);
    /* numeric push stamped now: the rate spaces the samples */
    CHECK_I(ynet_out_push(&data, x, 10, NULL), YNET_OK);
    {
        int64_t last = g_out[0].n - 1;
        CHECK(fabs(g_out[0].ts[last] - local_of(g_now)) < 1e-9 && fabs(g_out[0].ts[last] - g_out[0].ts[last - 1] - 1e-4) < 1e-9);
    }
    CHECK_I(ynet_out_mark(&data, 1, NULL), YNET_ERR_FORMAT);
    {
        ynet_found f[8];
        int n = ynet_resolve_all(&g_lib, f, 8, YRT_MS(100));
        CHECK_I(n, 2);
        CHECK(n == 2 && !strcmp(f[0].key, "lsl:Data:LabStreamer:ls-sim:testhost") && f[0].channels == 6 &&
              f[0].format == YNET_FLOAT32 && f[0].rate_hz == 10000.0);
    }
    ynet_inlet_close(&lat);
    ynet_inlet_close(&in);
    ynet_outlet_close(&marks);
    ynet_outlet_close(&data);
    CHECK_I(fk_infos_alive(), 0);
    CHECK_I(fk_inlets_alive(), 0);
    CHECK_I(g_strings, 0);
}

/* --- the reader thread on the real clock: 10 kHz, 4 channels --------------------------- */

static volatile uint32_t g_prod_stop;   /* through the header's atomics: the producer reads it */
static ynet_outlet g_prod_out;
static uint64_t g_prod_n;

#if defined(_WIN32)
static DWORD WINAPI producer(LPVOID arg)
#else
static void* producer(void* arg)
#endif
{
    /* 10 samples every 1 ms, each stamped on the LSL clock 1 ms before its
     * push, as a device stamps at its clock and sends a block later */
    float x[40];
    double ts[10];
    int64_t next = yrt_now_ns();
    double l0 = fk_clock() - 1e-3;
    uint64_t k = 0;
    int i;
    (void)arg;
    (void)yrt_thread_elevate(NULL);
    while (!ynet__ld(&g_prod_stop)) {
        for (i = 0; i < 10; i++, k++) {
            x[4 * i] = (float)((k / 50) % 2);
            x[4 * i + 1] = (float)k; x[4 * i + 2] = 0.5f; x[4 * i + 3] = -1.0f;
            ts[i] = l0 + (double)k * 1e-4;
        }
        (void)ynet_out_push(&g_prod_out, x, 10, ts);
        g_prod_n = k;
        next += 1000000;
        (void)yrt_sleep_until(next, 200000);
    }
#if defined(_WIN32)
    return 0;
#else
    return NULL;
#endif
}

static void test_thread_10khz(void) {
    static ynet_inlet in;
    static unsigned char mem[YNET_STREAM_BYTES(1 << 15, 4)];
    static ynet_stream ring;
    static int64_t tt[8192];
    static double ll[8192];
    static float xx[8192 * 4];
    ynet_outlet_desc od;
    ynet_inlet_desc id;
    ynet_stats st;
    int64_t end, got = 0, max_lag = 0, newest_ok = 0;
    uint64_t pushed;
    float expect = 0;
    int order_ok = 1, n, i;
    double secs = 5.0;
    const char* env = getenv("YNET_TEST_SECONDS");
    if (env && atof(env) > 0) secs = atof(env);
    reset_fake();
    g_real = 1;
    g_delay_ns = 0;
    g_rtt_ns = 50000;
    memset(&od, 0, sizeof od);
    od.lib = &g_lib; od.role = "sim"; od.key = "lsl:Sim:EMG:sim-1"; od.device = 30;
    od.format = YNET_FLOAT32; od.channels = 4; od.rate_hz = 10000.0;
    CHECK(ynet_outlet_open(&g_prod_out, &od));
    CHECK_I(ynet_stream_init(&ring, mem, sizeof mem, 4), 0);
    memset(&id, 0, sizeof id);
    id.lib = &g_lib; id.role = "emg"; id.key = "lsl:Sim"; id.device = 31; id.format = YNET_FLOAT32; id.channels = 4;
    id.stream = &ring; id.edges[0].channel = 0; id.edges[0].level = 0.5f; id.n_edges = 1; id.sink = sink;
    id.ring = &g_ring;
    CHECK(ynet_inlet_open(&in, &id));
    end = yrt_now_ns() + 3000000000LL;
    while (yrt_now_ns() < end && ynet_inlet_state(&in) != YNET_RUNNING) (void)yrt_sleep_ns(1000000);
    CHECK_I(ynet_inlet_state(&in), YNET_RUNNING);
    ynet__st(&g_prod_stop, 0);
#if defined(_WIN32)
    {
        HANDLE th = CreateThread(NULL, 0, producer, NULL, 0, NULL);
#else
    {
        pthread_t th;
        pthread_create(&th, NULL, producer, NULL);
#endif
        end = yrt_now_ns() + (int64_t)(secs * 1e9);
        while (yrt_now_ns() < end) {
            int64_t tn;
            (void)yrt_sleep_ns(10000000);                    /* a writer that drains every 10 ms */
            while ((n = ynet_stream_read(&ring, tt, ll, xx, 8192)) > 0) {
                int64_t now = yrt_now_ns();
                for (i = 0; i < n; i++) {
                    if (got == 0) expect = xx[4 * i + 1];
                    if (xx[4 * i + 1] != expect) order_ok = 0;
                    expect = xx[4 * i + 1] + 1.0f;
                    if (now - tt[i] > max_lag) max_lag = now - tt[i];
                }
                got += n;
            }
            if (ynet_stream_newest(&ring, &tn, NULL, NULL)) newest_ok++;
        }
        ynet__st(&g_prod_stop, 1);
#if defined(_WIN32)
        WaitForSingleObject(th, INFINITE);
        CloseHandle(th);
#else
        pthread_join(th, NULL);
#endif
    }
    pushed = g_prod_n;
    (void)yrt_sleep_ns(100000000);
    while ((n = ynet_stream_read(&ring, NULL, NULL, xx, 8192)) > 0) {
        for (i = 0; i < n; i++) {
            if (xx[4 * i + 1] != expect) order_ok = 0;
            expect = xx[4 * i + 1] + 1.0f;
        }
        got += n;
    }
    ynet_inlet_stats(&in, &st);
    ynet_inlet_close(&in);
    ynet_outlet_close(&g_prod_out);
    g_real = 0;
    printf("net_test: reader thread, 10 kHz x 4 channels for %.0f s: %lld pushed, %lld in the ring, %llu samples, "
           "%llu chunks (%.1f samples each), dropped %llu, gaps %llu; reader busy %.1f ns per sample; "
           "ring read lag max %.1f ms; measured %.0f Hz\n", secs, (long long)pushed, (long long)got,
           (unsigned long long)st.samples, (unsigned long long)st.chunks,
           st.chunks ? (double)st.samples / (double)st.chunks : 0.0, (unsigned long long)st.dropped,
           (unsigned long long)st.gaps, st.samples ? (double)st.busy_ns / (double)st.samples : 0.0,
           (double)max_lag / 1e6, st.measured_hz);
    CHECK(order_ok);
    CHECK_I(st.dropped, 0);
    CHECK_I(st.gaps, 0);
    CHECK((uint64_t)got == st.samples);
    CHECK(st.samples + 20 >= pushed && st.samples <= pushed);   /* the last pushes may land after the stop */
    CHECK(newest_ok > 0);
    CHECK_I(st.clamped, 0);
}

int main(void) {
    fk_lock_init();
    fake_lib();
    test_loader();
    test_keys();
    test_stream_ring();
    test_desc();
    test_wait();
    test_markers();
    test_remote_stream();
    test_lifecycle();
    test_resolve();
    test_band_and_refusal();
    test_labstreamer_like();
    test_thread_10khz();
    if (g_failures) {
        fprintf(stderr, "net_test: %d of %d checks failed\n", g_failures, g_checks);
        return 1;
    }
    printf("net_test: all %d checks passed\n", g_checks);
    return 0;
}
