/* bounds.c - an input's bounds by loopback, into the rig profile
 * (docs/devices_spec.md 11.2 and 12, ysp/rigfile.h roles.<role>.bounds).
 *
 *   rigfile_bounds --list
 *       Lists the serial ports with their match keys. No device needed.
 *   rigfile_bounds --role ROLE --ref KEY [--n 200] [--width S] [--warmup S]
 *                  [--file PATH] [--rig NAME] [--rig-dir DIR] [--store]
 *       ROLE is an input of the rig profile with a device clock (family
 *       xid, line or photo), opened as the profile binds it. KEY is a line
 *       board (firmware/ysp_line/), the reference: its output pin (pin 3)
 *       is wired to ROLE's input (a line board's pin 2, a box's external
 *       input, a relay on a button). After --warmup seconds (default 6) for
 *       both clock fits, the board pulses its pin --n times (default 200)
 *       for --width seconds (default 0.02), 50 to 150 ms apart, and reports
 *       each change in its own clock ("O" reports, mapped by its fit). Each
 *       change is matched to the first press or release of ROLE within
 *       half a width of it. Event time minus the board's time of the change
 *       is ROLE's stamp minus event; its range is the bounds. The program
 *       writes n, missed, extra, lo (the least), p5, median, p95, hi (the
 *       most), mean and the samples to in_bounds.txt (--file), in seconds,
 *       and prints the file's SHA-256.
 *       --store keeps the file in the profile's loopback/ folder and writes
 *       ROLE's bounds into the profile, only when the run passed.
 *   rigfile_bounds --sim [--role ROLE ...] [--n 200] ...
 *       The same run on a virtual clock, in a second or so: two simulated
 *       line boards on native USB (125 us microframes and an exponential
 *       tail of 50 us), the reference 20 ppm fast, the input 35 ppm slow,
 *       the input stamping its edge 1 to 2 us after it. ROLE, when given,
 *       must be bound to a line board in the profile; its key is not used.
 *       It prints the model's truth beside the measurement and fails when
 *       the measured bounds are further from the true ones than the
 *       reference's own error. The software check of this procedure; the
 *       hardware run is the user's.
 *
 * Why the reference is a line board's own report and not the line-in of
 * devices_spec 12: the board stamps its pin change in its interrupt and its
 * clock is mapped by a bracket fit on native USB; ysp/audio.h's capture
 * times have no loopback check yet (tier 3), and a capture's fixed delay
 * would go into every bound. The reference's own error is in the
 * measurement: the file states its fit's widest bracket and spread.
 *
 * Exit code 0 when at least 90% of the changes were matched and the input
 * gave no event outside a match window (and, with --sim, the bounds agree
 * with the truth); 1 otherwise; 2 for a usage or setup error.
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#define YSP_RIGFILE_IMPLEMENTATION
#include "ysp/rigfile.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAXN   10000            /* pulses; two changes each               */
#define MAXE   (4 * MAXN + 1024)

/* --- the simulation: two line boards on a virtual clock ---------------- */

#define SIMQ  65536
#define SIMEV 256
#define SIM_STEP_NS 100000      /* the main loop's step of the virtual clock */

enum { SE_PIN, SE_Q, SE_I, SE_OUT };

typedef struct simev { int64_t at; int kind; uint32_t a; } simev;

typedef struct sboard {
    int64_t        t0;          /* host time of board tick 0               */
    double         ns_per_tick; /* the board's microsecond in host ns       */
    int64_t        next_sync;
    uint8_t        q[SIMQ];     /* bytes to the host and when each arrives  */
    int64_t        qt[SIMQ];
    uint32_t       qh, qtail;
    simev          ev[SIMEV];   /* what the board does, at host times       */
    int            nev;
    int            level;       /* its input pin                            */
    char           line[64];
    int            nline;
    uint64_t       rng;
    struct sboard* wire;        /* the board whose input its output drives  */
    int            truth;       /* record each output change as the truth   */
} sboard;

static int64_t g_now = 1000000000;
static sboard  g_sref, g_sin;
static int64_t g_truth[MAXE];   /* host time of each reference pin change  */
static int     g_ntruth;

static int64_t now_fn(void* ctx) { (void)ctx; return g_now; }

static double sim_u01(sboard* b) {
    b->rng ^= b->rng << 13; b->rng ^= b->rng >> 7; b->rng ^= b->rng << 17;
    return ((double)(b->rng >> 11) + 1.0) / 9007199254740992.0;
}

static uint32_t sim_ticks(const sboard* b, int64_t host) { return (uint32_t)(uint64_t)((double)(host - b->t0) / b->ns_per_tick); }

/* Bytes the board makes at host time t reach the host after a microframe
 * and a tail, in order. */
static void sim_send(sboard* b, int64_t t, const char* s, int n) {
    int64_t at = t + (int64_t)(sim_u01(b) * 125000.0 - 50000.0 * log(sim_u01(b)));
    int i;
    if (b->qtail != b->qh && at < b->qt[(b->qtail - 1) % SIMQ]) at = b->qt[(b->qtail - 1) % SIMQ];
    for (i = 0; i < n; i++) {
        b->q[b->qtail % SIMQ] = (uint8_t)s[i];
        b->qt[b->qtail % SIMQ] = at;
        b->qtail++;
    }
}

static void sim_add(sboard* b, int64_t at, int kind, uint32_t a) {
    if (b->nev < SIMEV) { b->ev[b->nev].at = at; b->ev[b->nev].kind = kind; b->ev[b->nev].a = a; b->nev++; }
}

/* Everything the board does up to host time t, in time order. */
static void sim_run(sboard* b, int64_t t) {
    for (;;) {
        int i, first = -1, n;
        int64_t te = INT64_MAX;
        simev e;
        char s[64];
        for (i = 0; i < b->nev; i++) if (b->ev[i].at < te) { te = b->ev[i].at; first = i; }
        if (b->next_sync <= t && b->next_sync <= te) {
            n = snprintf(s, sizeof s, "S %lu\n", (unsigned long)sim_ticks(b, b->next_sync));
            sim_send(b, b->next_sync, s, n);
            b->next_sync += 100000000;
            continue;
        }
        if (first < 0 || te > t) break;
        e = b->ev[first];
        b->ev[first] = b->ev[--b->nev];
        switch (e.kind) {
        case SE_PIN:            /* the input interrupt stamps 1 to 2 us after the edge */
            if ((int)e.a == b->level) break;
            b->level = (int)e.a;
            n = snprintf(s, sizeof s, "E %lu 1 %d\n", (unsigned long)sim_ticks(b, e.at + 1000 + (int64_t)(sim_u01(b) * 1000.0)), b->level);
            sim_send(b, e.at, s, n);
            break;
        case SE_Q:
            n = snprintf(s, sizeof s, "Q %lu %lu\n", (unsigned long)e.a, (unsigned long)sim_ticks(b, e.at));
            sim_send(b, e.at, s, n);
            break;
        case SE_I:
            sim_send(b, e.at, "I ysp-line 1 sim photodiode\n", 28);
            break;
        default:                /* SE_OUT: the pins change, then micros() is read */
            n = snprintf(s, sizeof s, "O %lu %lu\n", (unsigned long)sim_ticks(b, e.at + 500), (unsigned long)e.a);
            sim_send(b, e.at, s, n);
            if (b->wire) sim_add(b->wire, e.at, SE_PIN, e.a & 1u);
            if (b->truth && g_ntruth < MAXE) g_truth[g_ntruth++] = e.at;
            break;
        }
    }
}

static void* sim_open(void* user, const char* key, char* found, size_t cap) {
    (void)key;
    snprintf(found, cap, "sim");
    return user;
}

static int sim_read(void* conn, uint8_t* buf, int cap, int64_t timeout_ns) {
    sboard* b = (sboard*)conn;
    int n = 0;
    sim_run(b, g_now);
    while (n < cap && b->qh != b->qtail && b->qt[b->qh % SIMQ] <= g_now) buf[n++] = b->q[b->qh++ % SIMQ];
    /* The main loop polls with timeout 0 and moves the clock itself; only
     * identify waits inside a read, before any pulse, so that wait may move
     * it here. */
    if (n == 0 && timeout_ns > 0) g_now += timeout_ns < SIM_STEP_NS ? timeout_ns : SIM_STEP_NS;
    return n;
}

/* Commands reach the board after a native-USB OUT transfer. */
static int sim_write(void* conn, const uint8_t* buf, int n) {
    sboard* b = (sboard*)conn;
    int64_t at = g_now + 30000 + (int64_t)(sim_u01(b) * 125000.0);
    int i;
    for (i = 0; i < n; i++) {
        char c = (char)buf[i];
        if (c != '\n') { if (b->nline < 63) b->line[b->nline++] = c; continue; }
        b->line[b->nline] = '\0';
        b->nline = 0;
        if (b->line[0] == 'q') sim_add(b, at, SE_Q, (uint32_t)strtoul(b->line + 2, NULL, 10));
        else if (b->line[0] == 'i') sim_add(b, at, SE_I, 0);
        else if (b->line[0] == 'o') sim_add(b, at, SE_OUT, (uint32_t)strtoul(b->line + 2, NULL, 10));
        else if (b->line[0] == 'p') {
            char* e;
            uint32_t code = (uint32_t)strtoul(b->line + 2, &e, 10);
            unsigned long us = strtoul(e, NULL, 10);
            sim_add(b, at, SE_OUT, code);
            sim_add(b, at + (int64_t)((double)us * b->ns_per_tick), SE_OUT, 0);
        }
    }
    return n;
}

static void sim_close(void* conn) { (void)conn; }

static void sim_init(sboard* b, double ppm, uint64_t seed, sboard* wire, int truth) {
    memset(b, 0, sizeof *b);
    b->t0 = g_now - 5000000000LL;
    b->ns_per_tick = 1000.0 * (1.0 - ppm * 1e-6);
    b->next_sync = g_now;
    b->rng = seed;
    b->wire = wire;
    b->truth = truth;
}

static ydev_transport sim_transport(sboard* b) {
    ydev_transport t;
    memset(&t, 0, sizeof t);
    t.open = sim_open;
    t.read = sim_read;
    t.write = sim_write;
    t.close = sim_close;
    t.user = b;
    return t;
}

/* --- the run ------------------------------------------------------------ */

typedef struct stamp { int64_t t; uint32_t unc_us; int used; } stamp;

static unsigned char g_ev_mem[YRT_RING_BYTES(4096)];
static yrt_ring      g_evring;
static unsigned char g_log_mem[YRT_RING_BYTES(16384)];
static yrt_ring      g_log;
static stamp         g_in[MAXE], g_ref[MAXE];
static int           g_nin, g_nref, g_sim;
static ydev_device   g_rdev, g_idev;
static yrig_profile  g_prof;
static int64_t       g_d[MAXE];
static double        g_unc[MAXE];

static void in_sink(void* ctx, const yin_event* e) {
    yrt_event r;
    (void)ctx;
    if (e->type != YIN_PRESS && e->type != YIN_RELEASE) return;
    memset(&r, 0, sizeof r);
    r.t_ns = (uint64_t)e->t;
    r.aux = e->unc_us;
    (void)yrt_ring_push(&g_evring, &r);
}

static int64_t clock_now(void) { return g_sim ? g_now : yrt_now_ns(); }

/* The input's events and the reference's reports, into the arrays. */
static void collect(void) {
    yrt_event ev[256];
    int n, k;
    while ((n = yrt_ring_drain(&g_evring, ev, 256)) > 0)
        for (k = 0; k < n && g_nin < MAXE; k++) {
            g_in[g_nin].t = (int64_t)ev[k].t_ns;
            g_in[g_nin].unc_us = ev[k].aux;
            g_in[g_nin++].used = 0;
        }
    while ((n = yrt_ring_drain(&g_log, ev, 256)) > 0)
        for (k = 0; k < n && g_nref < MAXE; k++)
            if (ev[k].source == YRT_SRC_DEVICE && ev[k].kind == YDEV_REC_OUT && ev[k].aux == g_rdev.d.device &&
                (ev[k].u.u16[18] & YDEV_OUT_REPORTED)) {
                g_ref[g_nref].t = ev[k].u.i64[0];
                g_ref[g_nref++].used = 0;
            }
}

/* dt of the clock: the virtual one in steps, polling both boards; the real
 * one in sleeps of at most 1 ms. */
static void advance(int64_t dt) {
    int64_t end = clock_now() + dt;
    while (clock_now() < end) {
        if (g_sim) {
            g_now += SIM_STEP_NS;
            (void)ydev_poll(&g_rdev, 0);
            (void)ydev_poll(&g_idev, 0);
        } else {
            int64_t left = end - clock_now();
            (void)yrt_sleep_ns(left < 1000000 ? left : 1000000);
        }
        collect();
    }
}

static int cmp_stamp(const void* a, const void* b) {
    int64_t x = ((const stamp*)a)->t, y = ((const stamp*)b)->t;
    return x < y ? -1 : (x > y);
}
static int cmp_i64(const void* a, const void* b) {
    int64_t x = *(const int64_t*)a, y = *(const int64_t*)b;
    return x < y ? -1 : (x > y);
}
static int cmp_d(const void* a, const void* b) {
    double x = *(const double*)a, y = *(const double*)b;
    return x < y ? -1 : (x > y);
}

static int64_t pctl(const int64_t* v, int n, double p) {
    double k = p * (double)(n - 1);
    int i = (int)k;
    if (i >= n - 1) return v[n - 1];
    return v[i] + (int64_t)floor((k - (double)i) * (double)(v[i + 1] - v[i]) + 0.5);
}

static const char* secs(char* out, int64_t ns) {
    uint64_t a = ns < 0 ? (uint64_t)0 - (uint64_t)ns : (uint64_t)ns;
    sprintf(out, "%s%llu.%09llu", ns < 0 ? "-" : "", (unsigned long long)(a / 1000000000u), (unsigned long long)(a % 1000000000u));
    return out;
}

static int list_ports(void) {
    yser_port_info info[64];
    int n = yser_list_ports(info, 64), i;
    if (n <= 0) { printf("no serial ports\n"); return 0; }
    for (i = 0; i < n && i < 64; i++) {
        char k[256];
        (void)ydev_port_key(&info[i], k, sizeof k);
        printf("%-12s %-40s %s\n", info[i].name, k, info[i].description);
    }
    return 0;
}

static void usage(void) {
    fprintf(stderr, "usage: rigfile_bounds --list\n"
                    "       rigfile_bounds --role ROLE --ref KEY [--n N] [--width S] [--warmup S] [--file PATH]\n"
                    "                      [--rig NAME] [--rig-dir DIR] [--store]\n"
                    "       rigfile_bounds --sim [--role ROLE] [--n N] ... (the same options)\n");
}

/* The profile at dir/name; 0, or 2 with a message. */
static int load_profile(const char* rig_name, const char* rig_dir, char* dir, size_t dcap, char* path, size_t pcap) {
    char err[512];
    if (rig_dir) snprintf(dir, dcap, "%s", rig_dir);
    else if (yrig_dir(dir, dcap) != YRIG_OK) {
        fprintf(stderr, "rigfile_bounds: no rig folder (the config folder is missing, or another user can write it)\n");
        return 2;
    }
    if (yrig_path(dir, rig_name, path, pcap) != YRIG_OK) {
        fprintf(stderr, "rigfile_bounds: \"%s\" is not a profile name\n", rig_name);
        return 2;
    }
    if (yrig_load_file(&g_prof, path, err, sizeof err) != YRIG_OK) {
        fprintf(stderr, "rigfile_bounds: %s (rigfile_profile --new and --bind make one)\n", err);
        return 2;
    }
    return 0;
}

static int store(const char* text, size_t tl, const char* dir, const char* path, const char* role) {
    char sha[65], err[512], a[40], b[40];
    yrig_loopback lb;
    yrig_role* r;
    if (yrig_read_loopback(text, tl, &lb, err, sizeof err) != YRIG_FILE_IN_BOUNDS) {
        fprintf(stderr, "rigfile_bounds: not stored: %s\n", err);
        return 2;
    }
    /* again: another tool may have written the profile during the run */
    if (yrig_load_file(&g_prof, path, err, sizeof err) != YRIG_OK) {
        fprintf(stderr, "rigfile_bounds: not stored: %s\n", err);
        return 2;
    }
    if (!(r = yrig_find(&g_prof, role))) {
        fprintf(stderr, "rigfile_bounds: not stored: the profile has no role \"%s\" any more\n", role);
        return 2;
    }
    if (yrig_store_loopback(dir, text, tl, sha, err, sizeof err) != YRIG_OK) {
        fprintf(stderr, "rigfile_bounds: not stored: %s\n", err);
        return 2;
    }
    r->bounds = lb;
    if (yrig_save_file(&g_prof, path, err, sizeof err) != YRIG_OK) {
        fprintf(stderr, "rigfile_bounds: not stored: %s\n", err);
        return 2;
    }
    printf("stored: loopback/%s.txt in %s; roles.%s.bounds %s to %s s in %s\n", sha, dir, role, secs(a, lb.lo_ns), secs(b, lb.hi_ns),
           path);
    return 0;
}

int main(int argc, char** argv) {
    const char* role = NULL;
    const char* ref_key = NULL;
    const char* out = "in_bounds.txt";
    const char* rig_name = NULL;
    const char* rig_dir = NULL;
    int n = 200, i, k, do_store = 0, nd = 0, missed = 0, extra = 0, ok, sim_ok = 1, nu = 0;
    double width_s = 0.02, warmup_s = 6.0;
    int64_t width_ns, half;
    char dir[600], path[700], a[40];
    const yrig_role* ro = NULL;
    ydev_desc rd, base;
    yrt_ring_desc ringd;
    ydev_stats rst, ist;
    static char text[MAXE * 16 + 4096];
    size_t tl = 0;
    char hex[65], date[32];
    uint64_t rng = 0x9E3779B97F4A7C15ull;

    for (i = 1; i < argc; i++) {
        const char* s = argv[i];
        const char* v = i + 1 < argc ? argv[i + 1] : NULL;
        if (strcmp(s, "--list") == 0) return list_ports();
        else if (strcmp(s, "--sim") == 0) g_sim = 1;
        else if (strcmp(s, "--role") == 0 && v) { role = v; i++; }
        else if (strcmp(s, "--ref") == 0 && v) { ref_key = v; i++; }
        else if (strcmp(s, "--n") == 0 && v) { n = atoi(v); i++; }
        else if (strcmp(s, "--width") == 0 && v) { width_s = atof(v); i++; }
        else if (strcmp(s, "--warmup") == 0 && v) { warmup_s = atof(v); i++; }
        else if (strcmp(s, "--file") == 0 && v) { out = v; i++; }
        else if (strcmp(s, "--store") == 0) do_store = 1;
        else if (strcmp(s, "--rig") == 0 && v) { rig_name = v; i++; }
        else if (strcmp(s, "--rig-dir") == 0 && v) { rig_dir = v; i++; }
        else { usage(); return 2; }
    }
    if (n < 1 || n > MAXN || width_s < 0.002 || width_s > 0.04 || warmup_s < 0.0 || (!g_sim && (!role || !ref_key)) ||
        (do_store && !role)) {
        usage();
        return 2;
    }
    width_ns = (int64_t)(width_s * 1e9 + 0.5);
    half = width_ns / 2;
    if (role) {
        int rc = load_profile(rig_name, rig_dir, dir, sizeof dir, path, sizeof path);
        if (rc) return rc;
        if (!(ro = yrig_find(&g_prof, role)) || !ro->key[0]) {
            fprintf(stderr, "rigfile_bounds: the profile %s has no device bound to role \"%s\"\n", path, role);
            return 2;
        }
        if (ro->family_id != YBOX_XID && ro->family_id != YBOX_LINE && ro->family_id != YBOX_PHOTO) {
            fprintf(stderr, "rigfile_bounds: roles.%s is a %s: bounds are for an input with a device clock (xid, line, photo)\n", role,
                    ro->family);
            return 2;
        }
        if (g_sim && ro->family_id != YBOX_LINE) {
            fprintf(stderr, "rigfile_bounds: --sim models a line board; roles.%s is a %s\n", role, ro->family);
            return 2;
        }
    }

    memset(&ringd, 0, sizeof ringd);
    ringd.memory = g_ev_mem;
    ringd.bytes = sizeof g_ev_mem;
    if (!yrt_ring_open(&g_evring, &ringd)) return 2;
    ringd.memory = g_log_mem;
    ringd.bytes = sizeof g_log_mem;
    if (!yrt_ring_open(&g_log, &ringd)) return 2;
    if (g_sim) {
        sim_init(&g_sref, 20.0, 0x9E3779B97F4A7C15ull, &g_sin, 1);
        sim_init(&g_sin, -35.0, 0xD1B54A32D192ED03ull, NULL, 0);
    }

    memset(&rd, 0, sizeof rd);
    rd.role = "bounds_ref";
    rd.family = YBOX_LINE;
    rd.key = g_sim ? "sim:ref" : ref_key;
    rd.device = 1;
    rd.ring = &g_log;
    memset(&base, 0, sizeof base);
    base.device = 2;
    base.sink = in_sink;
    if (g_sim) {
        rd.transport = sim_transport(&g_sref);
        base.transport = sim_transport(&g_sin);
        rd.manual = base.manual = true;
        rd.now = base.now = now_fn;
    }
    if (!ydev_open(&g_rdev, &rd)) { fprintf(stderr, "rigfile_bounds: reference: %s\n", ydev_error(&g_rdev)); return 2; }
    if (role) ok = yrig_open_device(&g_idev, &g_prof, role, &base);
    else {
        base.role = "in";
        base.family = YBOX_LINE;
        base.key = "sim:in";
        ok = ydev_open(&g_idev, &base);
    }
    if (!ok) {
        fprintf(stderr, "rigfile_bounds: input: %s\n", ydev_error(&g_idev));
        ydev_close(&g_rdev);
        return 2;
    }
    /* both running, then the warm-up: each fit needs its span */
    {
        int64_t until = clock_now() + YRT_S(10);
        for (;;) {
            int rs = ydev_state(&g_rdev), is = ydev_state(&g_idev);
            if (rs == YDEV_RUNNING && is == YDEV_RUNNING) break;
            if (rs == YDEV_FAILED || is == YDEV_FAILED || clock_now() >= until) {
                fprintf(stderr, "rigfile_bounds: reference %s (%s); input %s (%s)\n", ydev_state_name(rs), ydev_error(&g_rdev),
                        ydev_state_name(is), ydev_error(&g_idev));
                ydev_close(&g_rdev);
                ydev_close(&g_idev);
                return 2;
            }
            advance(YRT_MS(1));
        }
    }
    ydev_get_stats(&g_rdev, &rst);
    ydev_get_stats(&g_idev, &ist);
    printf("rigfile_bounds: input %s (%s) at %s, \"%s\"; reference at %s, \"%s\"; warming up %.1f s%s\n", role ? role : "in",
           role ? ro->family : "line", ist.found, ist.ident, rst.found, rst.ident, warmup_s, g_sim ? " (simulated)" : "");
    advance((int64_t)(warmup_s * 1e9));
    g_nin = g_nref = 0;
    g_ntruth = 0;

    for (i = 0; i < n; i++) {
        int rc = ydev_out_pulse(&g_rdev, 1, width_ns);
        if (rc != YDEV_OK) fprintf(stderr, "rigfile_bounds: pulse %d: %s\n", i, ydev_strerror(rc));
        rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
        advance(width_ns + YRT_MS(50) + (int64_t)(rng % 100000000u));
    }
    advance(YRT_MS(200));
    ydev_get_stats(&g_rdev, &rst);
    ydev_get_stats(&g_idev, &ist);
    ydev_close(&g_rdev);
    ydev_close(&g_idev);
    collect();

    /* each change of the reference to the first event within half a width */
    qsort(g_in, (size_t)g_nin, sizeof g_in[0], cmp_stamp);
    qsort(g_ref, (size_t)g_nref, sizeof g_ref[0], cmp_stamp);
    for (i = 0, k = 0; i < g_nref; i++) {
        while (k < g_nin && g_in[k].t < g_ref[i].t - half) k++;
        if (k < g_nin && g_in[k].t < g_ref[i].t + half) {
            g_in[k].used = 1;
            g_ref[i].used = 1;
            g_d[nd++] = g_in[k].t - g_ref[i].t;
            g_unc[nu++] = (double)g_in[k].unc_us * 1e-6;
            k++;
        } else missed++;
    }
    missed += (2 * n > g_nref) ? 2 * n - g_nref : 0;   /* changes the board never reported */
    for (k = 0; k < g_nin; k++) if (!g_in[k].used) extra++;

    if (g_sim) {
        /* the model's truth: each matched pair against the true pin change */
        int64_t in_lo = INT64_MAX, in_hi = INT64_MIN, ref_max = 0, ref_lo = INT64_MAX, ref_hi = INT64_MIN;
        int m = 0, j = 0;
        for (i = 0, k = 0; i < g_nref; i++) {
            int64_t tp, ein, eref;
            if (!g_ref[i].used) continue;
            while (j + 1 < g_ntruth && llabs(g_truth[j + 1] - g_ref[i].t) < llabs(g_truth[j] - g_ref[i].t)) j++;
            if (j >= g_ntruth || llabs(g_truth[j] - g_ref[i].t) > YRT_MS(1)) continue;
            tp = g_truth[j];
            eref = g_ref[i].t - tp;
            ein = g_d[m] + eref;          /* the input's stamp minus the true change */
            m++;
            if (ein < in_lo) in_lo = ein;
            if (ein > in_hi) in_hi = ein;
            if (eref < ref_lo) ref_lo = eref;
            if (eref > ref_hi) ref_hi = eref;
            if (llabs(eref) > ref_max) ref_max = llabs(eref);
        }
        (void)k;
        if (m > 0 && nd > 0) {
            int64_t lo = INT64_MAX, hi = INT64_MIN;
            char b1[40], b2[40], b3[40];
            for (i = 0; i < nd; i++) { if (g_d[i] < lo) lo = g_d[i]; if (g_d[i] > hi) hi = g_d[i]; }
            printf("simulated truth over %d changes: the input's stamp minus the change %s to %s s; the reference's %s to %s s\n", m,
                   secs(b1, in_lo), secs(b2, in_hi), secs(b3, ref_lo), secs(a, ref_hi));
            sim_ok = llabs(lo - in_lo) <= ref_max && llabs(hi - in_hi) <= ref_max;
            if (!sim_ok) fprintf(stderr, "rigfile_bounds: the measured bounds are further from the truth than the reference's error\n");
        } else sim_ok = 0;
    }

    qsort(g_d, (size_t)nd, sizeof g_d[0], cmp_i64);
    qsort(g_unc, (size_t)nu, sizeof g_unc[0], cmp_d);
    {
        time_t now = time(NULL);
        double mean = 0.0;
        strftime(date, sizeof date, "%Y-%m-%dT%H:%M:%SZ", gmtime(&now));
        for (i = 0; i < nd; i++) mean += (double)g_d[i];
        mean = nd ? mean / nd : 0.0;
#define PUT(...) do { int w_ = snprintf(text + tl, sizeof text - tl, __VA_ARGS__); if (w_ > 0) tl += (size_t)w_; } while (0)
        PUT("# ysp input bounds: the input's event time minus the reference board's own time of its pin change "
            "(docs/devices_spec.md 12)\n");
        PUT("format ysp-in-bounds 1\n");
        PUT("date %s\n", date);
        PUT("ysp_rig %s\n", yrig_version());
        PUT("in_role %s\n", role ? role : "in");
        PUT("in_family %s%s\n", role ? ro->family : "line", g_sim ? " (simulated)" : "");
        PUT("in_key %s\n", role ? ro->key : "sim:in");
        PUT("in_found %s\n", ist.found);
        PUT("in_ident %s\n", ist.ident);
        PUT("ref_key %s\n", g_sim ? "sim:ref" : ref_key);
        PUT("ref_ident %s\n", rst.ident);
        PUT("wiring the reference board's output pin into the input\n");
        PUT("width_s %s\n", secs(a, width_ns));
        PUT("changes %d\n", 2 * n);
        PUT("n %d\n", nd);
        PUT("missed %d\n", missed);
        PUT("extra %d\n", extra);
        if (nd) {
            PUT("lo_s %s\n", secs(a, g_d[0]));
            PUT("p5_s %s\n", secs(a, pctl(g_d, nd, 0.05)));
            PUT("median_s %s\n", secs(a, pctl(g_d, nd, 0.5)));
            PUT("p95_s %s\n", secs(a, pctl(g_d, nd, 0.95)));
            PUT("hi_s %s\n", secs(a, g_d[nd - 1]));
            PUT("mean_s %s\n", secs(a, (int64_t)floor(mean + 0.5)));
        }
        if (nu) PUT("in_unc_median_s %.7f\n", g_unc[nu / 2]);
        PUT("in_fit_ppm %.3f\n", ist.fit.ppm);
        PUT("ref_fit_ppm %.3f\n", rst.fit.ppm);
        PUT("ref_fit_width_s %s\n", secs(a, rst.fit.width_ns));
        PUT("ref_fit_spread_s %s\n", secs(a, rst.fit.spread_ns));
        PUT("samples_s");
        for (i = 0; i < nd; i++) PUT(" %s", secs(a, g_d[i]));
        PUT("\n");
#undef PUT
    }
    {
        FILE* f = fopen(out, "wb");
        uint8_t d[32];
        const char* s = strstr(text, "samples_s");
        if (!f || fwrite(text, 1, tl, f) != tl) { fprintf(stderr, "rigfile_bounds: cannot write %s\n", out); if (f) fclose(f); return 2; }
        fclose(f);
        yrig_sha256(text, tl, d);
        for (i = 0; i < 32; i++) sprintf(hex + 2 * i, "%02x", d[i]);
        fwrite(text, 1, s ? (size_t)(s - text) : tl, stdout);
    }
    printf("sha256 %s  %s\n", hex, out);
    ok = nd > 0 && nd >= (2 * n * 9) / 10 && extra == 0 && sim_ok;
    if (!ok) {
        if (do_store) fprintf(stderr, "rigfile_bounds: not stored: the run did not pass (n %d of %d, extra %d)\n", nd, 2 * n, extra);
        return 1;
    }
    if (do_store) return store(text, tl, dir, path, role);
    return 0;
}
