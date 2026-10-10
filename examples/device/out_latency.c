/* out_latency.c - an output's write-to-edge latency by loopback
 * (docs/devices_spec.md section 12): drive an output role, catch the edge
 * with an input role, and write the distribution to a file that a rig
 * profile can name by its SHA-256.
 *
 *   device_out_latency --list
 *       Lists the serial ports with their match keys. No device needed.
 *   device_out_latency --out FAMILY:KEY [--in KEY] [--n 200] [--width S]
 *                      [--warmup S] [--file PATH]
 *       FAMILY is lines (a serial port's DTR, bit 0, and RTS, bit 1),
 *       line, triggerbox, biosemi, mmbts, mmbts-s (switch S), xid, or
 *       parallel (KEY "parallel:", "parallel:0x378" or
 *       "parallel:/dev/parport0"). KEY is a match key (ysp/device.h, MATCH
 *       KEYS). --in is the line-protocol board (firmware/ysp_line/) whose
 *       input pin (YSP_PIN, default 2) takes the output's line, bit 0 of
 *       the code; without --in, the --out board is wired to itself (its
 *       output pin 3 to its input pin 2). The program pulses code 1 for
 *       --width seconds (default 0.005) every 50 to 150 ms, takes the first
 *       edge of either polarity the input reports after each write (a
 *       TTL-level adapter drives DTR low when it is asserted), and reports
 *       edge minus the time before the write: n, missed, min, p5, median,
 *       p95, max, mean, in seconds. With a line board as the output it also
 *       reports the board's own time of its pin change ("O" reports) minus
 *       the write.
 *   device_out_latency ... --store [--rig NAME] [--rig-dir DIR] [--role ROLE]
 *       Also keeps the result file in the rig profile's folder
 *       (loopback/<sha256>.txt, ysp/rigfile.h) and writes ROLE's (default
 *       trig) family, key and latency summary into the profile: the user's
 *       profile.json, or NAME.json, in the user's rig folder or in DIR.
 *       A role bound to another device is bound again; its old numbers go.
 *   device_out_latency --sim [--self] [--n 200] ...
 *       The same run against an in-process simulation: a USB-serial
 *       adapter's DTR (a USB control transfer: 0.05 ms plus up to one 1 ms
 *       frame) into a board on native USB (125 us microframes plus an
 *       exponential tail of 50 us), the board's clock 20 ppm fast; --self
 *       a board's own output pin into its input. It also prints the true
 *       latencies of the model, so the measurement can be checked against
 *       them. The software check of this procedure; the hardware run is
 *       the user's.
 *
 * The result file (default out_latency.txt) is plain "name value" lines,
 * durations in seconds, then the samples. The program prints its SHA-256:
 * the rig profile names the file by that hash (docs/devices_spec.md 11.2,
 * ysp/rigfile.h).
 * Edge times are the input board's own (FIT through ysp/rt.h), so the
 * file also states the input's per-event uncertainty (unc_us).
 *
 * Exit code 0 when at least 90% of the pulses gave an edge; 1 otherwise; 2
 * for a usage or setup error.
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#if !defined(__APPLE__)
#define YSP_PARALLEL_IMPLEMENTATION
#include "ysp/parallel.h"
#endif
#define YSP_DEVICE_IMPLEMENTATION
#include "ysp/device.h"
#define YSP_RIGFILE_IMPLEMENTATION
#include "ysp/rigfile.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
typedef CRITICAL_SECTION sim_mutex;
static void sim_lock_init(sim_mutex* m) { InitializeCriticalSection(m); }
static void sim_lock(sim_mutex* m)      { EnterCriticalSection(m); }
static void sim_unlock(sim_mutex* m)    { LeaveCriticalSection(m); }
#else
typedef pthread_mutex_t sim_mutex;
static void sim_lock_init(sim_mutex* m) { pthread_mutex_init(m, NULL); }
static void sim_lock(sim_mutex* m)      { pthread_mutex_lock(m); }
static void sim_unlock(sim_mutex* m)    { pthread_mutex_unlock(m); }
#endif

#define MAXN 10000

/* The result file's name in a rig profile: its SHA-256 (ysp/rigfile.h). */
static void sha256_hex(const uint8_t* msg, size_t n, char out[65]) {
    uint8_t d[32];
    int i;
    yrig_sha256(msg, n, d);
    for (i = 0; i < 32; i++) sprintf(out + 2 * i, "%02x", d[i]);
}

/* --- the simulation: an adapter's DTR into a board, or a board into itself -- */

#define SIMQ 65536

typedef struct simboard {
    sim_mutex mx;
    int64_t   t0;            /* the board's clock: (host - t0) / tick us   */
    double    tick;
    int64_t   next_sync;
    uint8_t   q[SIMQ];       /* bytes to the host and when each arrives    */
    int64_t   qt[SIMQ];
    uint32_t  qh, qtail;
    int64_t   pin_at[64];    /* scheduled changes of the input pin         */
    int       pin_lvl[64];
    int       npin;
    int       level;
    char      line[64];
    int       nline;
    uint64_t  rng;
    /* the model's truth: each edge minus the host call that caused it */
    int64_t   call_t;
    double    truth[MAXN];
    int       ntruth;
    int       want_truth;
} simboard;

static simboard g_sim;

static double sim_u01(simboard* b) {
    b->rng ^= b->rng << 13; b->rng ^= b->rng >> 7; b->rng ^= b->rng << 17;
    return ((double)(b->rng >> 11) + 1.0) / 9007199254740992.0;
}

static uint32_t sim_ticks(simboard* b, int64_t host) { return (uint32_t)(uint64_t)((double)(host - b->t0) / b->tick); }

/* Bytes made at host time t reach the host after a native-USB microframe
 * and a tail, in order. */
static void sim_send(simboard* b, int64_t t, const char* s, int n) {
    int64_t at = t + (int64_t)(sim_u01(b) * 125000.0 - 50000.0 * log(sim_u01(b)));
    int i;
    if (b->qtail != b->qh && at < b->qt[(b->qtail - 1) % SIMQ]) at = b->qt[(b->qtail - 1) % SIMQ];
    for (i = 0; i < n; i++) {
        b->q[b->qtail % SIMQ] = (uint8_t)s[i];
        b->qt[b->qtail % SIMQ] = at;
        b->qtail++;
    }
}

static void sim_pin(simboard* b, int64_t at, int lvl) {
    if (b->npin < 64) { b->pin_at[b->npin] = at; b->pin_lvl[b->npin++] = lvl; }
}

/* Everything the board does up to host time t. Under the lock. */
static void sim_run(simboard* b, int64_t t) {
    for (;;) {
        int i, first = -1;
        int64_t tp = INT64_MAX;
        char s[64];
        int n;
        for (i = 0; i < b->npin; i++) if (b->pin_at[i] < tp) { tp = b->pin_at[i]; first = i; }
        if (b->next_sync <= t && b->next_sync <= tp) {
            n = snprintf(s, sizeof s, "S %lu\n", (unsigned long)sim_ticks(b, b->next_sync));
            sim_send(b, b->next_sync, s, n);
            b->next_sync += 100000000;
            continue;
        }
        if (first < 0 || tp > t) break;
        if (b->pin_lvl[first] != b->level) {
            b->level = b->pin_lvl[first];
            /* the input interrupt stamps the edge 1 to 2 us after it */
            n = snprintf(s, sizeof s, "E %lu 1 %d\n", (unsigned long)sim_ticks(b, tp + 1000 + (int64_t)(sim_u01(b) * 1000.0)),
                         b->level);
            sim_send(b, tp, s, n);
            if (b->want_truth && b->ntruth < MAXN) { b->truth[b->ntruth++] = (double)(tp - b->call_t); b->want_truth = 0; }
        }
        b->pin_at[first] = b->pin_at[--b->npin];
        b->pin_lvl[first] = b->pin_lvl[b->npin];
    }
}

static void* sim_open(void* user, const char* key, char* found, size_t cap) {
    (void)key;
    snprintf(found, cap, "sim");
    return user;
}

static int sim_read(void* conn, uint8_t* buf, int cap, int timeout_ms) {
    simboard* b = (simboard*)conn;
    int64_t end = yrt_now_ns() + (int64_t)timeout_ms * 1000000;
    for (;;) {
        int n = 0;
        int64_t t = yrt_now_ns();
        sim_lock(&b->mx);
        sim_run(b, t);
        while (n < cap && b->qh != b->qtail && b->qt[b->qh % SIMQ] <= t) buf[n++] = b->q[b->qh++ % SIMQ];
        sim_unlock(&b->mx);
        if (n > 0) return n;
        if (t >= end) return 0;
        (void)yrt_sleep_ns(100000);
    }
}

/* Commands to the board arrive after a native-USB OUT transfer. */
static int sim_write(void* conn, const uint8_t* buf, int n) {
    simboard* b = (simboard*)conn;
    int64_t t = yrt_now_ns(), at;
    int i;
    sim_lock(&b->mx);
    at = t + 30000 + (int64_t)(sim_u01(b) * 125000.0);
    for (i = 0; i < n; i++) {
        char c = (char)buf[i], s[96];
        int k;
        if (c != '\n') { if (b->nline < 63) b->line[b->nline++] = c; continue; }
        b->line[b->nline] = '\0';
        b->nline = 0;
        if (b->line[0] == 'q') {
            k = snprintf(s, sizeof s, "Q %s %lu\n", b->line + 2, (unsigned long)sim_ticks(b, at));
            sim_send(b, at, s, k);
        } else if (b->line[0] == 'i') {
            sim_send(b, at, "I ysp-line 1 sim out_latency\n", 29);
        } else if (b->line[0] == 'o' || b->line[0] == 'p') {
            /* the output pin wired to the input pin (--self) */
            unsigned long code = 0, us = 0;
            char* e;
            code = strtoul(b->line + 2, &e, 10);
            if (b->line[0] == 'p') us = strtoul(e, NULL, 10);
            k = snprintf(s, sizeof s, "O %lu %lu\n", (unsigned long)sim_ticks(b, at), code);
            sim_send(b, at, s, k);
            b->call_t = t;
            b->want_truth = 1;
            sim_pin(b, at, (int)(code & 1u));
            if (us) {
                k = snprintf(s, sizeof s, "O %lu 0\n", (unsigned long)sim_ticks(b, at + (int64_t)us * 1000));
                sim_send(b, at + (int64_t)us * 1000, s, k);
                sim_pin(b, at + (int64_t)us * 1000, 0);
            }
        }
    }
    sim_unlock(&b->mx);
    return n;
}

static void sim_close(void* conn) { (void)conn; }

/* The adapter: DTR (bit 0) reaches the board's pin after a USB control
 * transfer, which waits for the next 1 ms frame. A TTL-level adapter
 * drives the pin low when DTR is asserted. */
static void* ad_open(void* user, const char* key, char* found, size_t cap) {
    (void)key;
    snprintf(found, cap, "sim-adapter");
    return user;
}
static int ad_write(void* conn, const uint8_t* buf, int n) { (void)conn; (void)buf; return n; }
static int ad_lines(void* conn, uint32_t code, uint32_t changed) {
    simboard* b = (simboard*)conn;
    int64_t t = yrt_now_ns();
    if (!(changed & 1u)) return 0;
    sim_lock(&b->mx);
    if (code & 1u) { b->call_t = t; b->want_truth = 1; }
    sim_pin(b, t + 50000 + (int64_t)(sim_u01(b) * 1000000.0), (code & 1u) ? 0 : 1);
    sim_unlock(&b->mx);
    return 0;
}

static void sim_init(int self) {
    memset(&g_sim, 0, sizeof g_sim);
    sim_lock_init(&g_sim.mx);
    g_sim.t0 = yrt_now_ns() - 5000000000LL;
    g_sim.tick = 1000.0 * (1.0 - 20e-6);
    g_sim.next_sync = yrt_now_ns();
    g_sim.level = self ? 0 : 1;   /* a board pin idles low, a TTL adapter's DTR high */
    g_sim.rng = 0x9E3779B97F4A7C15ull;
}

/* --- edges from the input ---------------------------------------------------- */

static unsigned char g_edge_mem[YRT_RING_BYTES(4096)];
static yrt_ring      g_edges;
static unsigned char g_log_mem[YRT_RING_BYTES(65536)];
static yrt_ring      g_log;

static void edge_sink(void* ctx, const yin_event* e) {
    yrt_event r;
    (void)ctx;
    if (e->type != YIN_PRESS && e->type != YIN_RELEASE) return;
    memset(&r, 0, sizeof r);
    r.t_ns = (uint64_t)e->t;
    r.kind = e->type;
    r.aux = e->unc_us;
    (void)yrt_ring_push(&g_edges, &r);
}

static int cmp_d(const void* a, const void* b) {
    double x = *(const double*)a, y = *(const double*)b;
    return x < y ? -1 : (x > y);
}

static double pctl(const double* v, int n, double p) {
    double k = p * (double)(n - 1);
    int i = (int)k;
    if (n == 0) return 0.0;
    if (i >= n - 1) return v[n - 1];
    return v[i] + (k - (double)i) * (v[i + 1] - v[i]);
}

static int family_of(const char* s, bool* latched) {
    static const struct { const char* name; int fam; } t[] = {
        { "lines", YBOX_LINES }, { "line", YBOX_LINE }, { "triggerbox", YBOX_TRIGGERBOX }, { "biosemi", YBOX_BIOSEMI },
        { "mmbts", YBOX_MMBTS }, { "mmbts-s", YBOX_MMBTS }, { "xid", YBOX_XID }, { "parallel", YBOX_PARALLEL } };
    size_t i;
    *latched = strcmp(s, "mmbts-s") == 0;
    for (i = 0; i < sizeof t / sizeof t[0]; i++) if (strcmp(s, t[i].name) == 0) return t[i].fam;
    return 0;
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
    fprintf(stderr, "usage: device_out_latency --list\n"
                    "       device_out_latency --out FAMILY:KEY [--in KEY] [--n N] [--width S] [--warmup S] [--file PATH]\n"
                    "       device_out_latency --sim [--self] [--n N] [--width S] [--warmup S] [--file PATH]\n"
                    "       ... --store [--rig NAME] [--rig-dir DIR] [--role ROLE]\n");
}

static ydev_device g_out, g_in;
static ydev_roles  g_roles;
#if !defined(__APPLE__)
static ypar_port   g_par;
#endif
static double      g_lat[MAXN], g_rep[MAXN], g_wid[MAXN], g_unc[MAXN];
static yrig_profile g_prof;

/* The result into the rig profile (--store): the file to loopback/, the
 * role's family, key and summary to the profile. 0, or 2 with a message. */
static int store(const char* text, size_t tl, const char* rig_name, const char* rig_dir, const char* role, int fam, bool latched,
                 const char* key) {
    char dir[600], path[700], sha[65], err[512];
    yrig_role* r;
    yrig_loopback lb;
    int rc;
    if (yrig_read_latency(text, tl, &lb, err, sizeof err) != YRIG_OK) {
        fprintf(stderr, "out_latency: not stored: %s\n", err);
        return 2;
    }
    if (rig_dir) snprintf(dir, sizeof dir, "%s", rig_dir);
    else if (yrig_dir(dir, sizeof dir) != YRIG_OK) {
        fprintf(stderr, "out_latency: not stored: no rig folder (the config folder is missing, or another user can write it)\n");
        return 2;
    }
    if (yrig_path(dir, rig_name, path, sizeof path) != YRIG_OK) {
        fprintf(stderr, "out_latency: not stored: \"%s\" is not a profile name\n", rig_name);
        return 2;
    }
    rc = yrig_load_file(&g_prof, path, err, sizeof err);
    if (rc == YRIG_ERR_MISSING) yrig_init(&g_prof, rig_name ? rig_name : "rig");
    else if (rc != YRIG_OK) {
        fprintf(stderr, "out_latency: not stored: %s\n", err);
        return 2;
    }
    r = yrig_find(&g_prof, role);
    if ((!r || strcmp(r->family, ybox_family_name(fam)) != 0 || strcmp(r->key, key) != 0) &&
        yrig_bind(&g_prof, role, fam, key) != YRIG_OK) {
        fprintf(stderr, "out_latency: not stored: cannot bind role \"%s\" (1 to 31 of A-Z a-z 0-9 _ . -, at most %d roles)\n", role,
                YRIG_MAX_ROLES);
        return 2;
    }
    r = yrig_find(&g_prof, role);
    r->latched = latched;
    if (yrig_store_loopback(dir, text, tl, sha, err, sizeof err) != YRIG_OK) {
        fprintf(stderr, "out_latency: not stored: %s\n", err);
        return 2;
    }
    r->latency = lb;
    if (yrig_save_file(&g_prof, path, err, sizeof err) != YRIG_OK) {
        fprintf(stderr, "out_latency: not stored: %s\n", err);
        return 2;
    }
    printf("stored: loopback/%s.txt in %s; roles.%s (%s, %s) in %s\n", sha, dir, role, r->family, r->key, path);
    return 0;
}

int main(int argc, char** argv) {
    const char* out_arg = NULL;
    const char* in_key = NULL;
    const char* path = "out_latency.txt";
    const char* rig_name = NULL;
    const char* rig_dir = NULL;
    const char* role = "trig";
    int do_store = 0;
    int n = 200, i, sim = 0, self = 0, fam = 0, nl = 0, nr = 0, nw = 0, nu = 0, missed = 0;
    double width_s = 0.005, warmup_s = 6.0;
    bool latched = false;
    char out_key[256], famname[32];
    ydev_desc od, id;
    yrt_ring_desc rd;
    static char text[MAXN * 16 + 4096];
    size_t tl = 0;
    char hex[65];
    ydev_stats ost, ist;

    for (i = 1; i < argc; i++) {
        const char* a = argv[i];
        const char* v = i + 1 < argc ? argv[i + 1] : NULL;
        if (strcmp(a, "--list") == 0) return list_ports();
        else if (strcmp(a, "--sim") == 0) sim = 1;
        else if (strcmp(a, "--self") == 0) self = 1;
        else if (strcmp(a, "--out") == 0 && v) { out_arg = v; i++; }
        else if (strcmp(a, "--in") == 0 && v) { in_key = v; i++; }
        else if (strcmp(a, "--n") == 0 && v) { n = atoi(v); i++; }
        else if (strcmp(a, "--width") == 0 && v) { width_s = atof(v); i++; }
        else if (strcmp(a, "--warmup") == 0 && v) { warmup_s = atof(v); i++; }
        else if (strcmp(a, "--file") == 0 && v) { path = v; i++; }
        else if (strcmp(a, "--store") == 0) do_store = 1;
        else if (strcmp(a, "--rig") == 0 && v) { rig_name = v; i++; }
        else if (strcmp(a, "--rig-dir") == 0 && v) { rig_dir = v; i++; }
        else if (strcmp(a, "--role") == 0 && v) { role = v; i++; }
        else { usage(); return 2; }
    }
    if (n < 1 || n > MAXN || width_s <= 0.0 || width_s > 0.04 || warmup_s < 0.0) { usage(); return 2; }
    if (sim) {
        snprintf(famname, sizeof famname, "%s", self ? "line" : "lines");
        fam = self ? YBOX_LINE : YBOX_LINES;
        snprintf(out_key, sizeof out_key, "sim:%s", self ? "board" : "adapter");
    } else {
        const char* colon = out_arg ? strchr(out_arg, ':') : NULL;
        if (!colon || (size_t)(colon - out_arg) >= sizeof famname) { usage(); return 2; }
        memcpy(famname, out_arg, (size_t)(colon - out_arg));
        famname[colon - out_arg] = '\0';
        fam = family_of(famname, &latched);
        snprintf(out_key, sizeof out_key, "%s", colon + 1);
        if (!fam) { fprintf(stderr, "out_latency: unknown family %s\n", famname); return 2; }
        if (!in_key && fam != YBOX_LINE) {
            fprintf(stderr, "out_latency: --in KEY is needed unless the output is a line board wired to itself\n");
            return 2;
        }
        self = !in_key;
    }

    memset(&rd, 0, sizeof rd);
    rd.memory = g_edge_mem;
    rd.bytes = sizeof g_edge_mem;
    if (!yrt_ring_open(&g_edges, &rd)) return 2;
    rd.memory = g_log_mem;
    rd.bytes = sizeof g_log_mem;
    if (!yrt_ring_open(&g_log, &rd)) return 2;
    if (sim) sim_init(self);

    memset(&od, 0, sizeof od);
    od.role = "out";
    od.family = fam;
    od.key = out_key;
    od.roles = &g_roles;
    od.ring = &g_log;
    od.latched = latched;
    if (self) { od.sink = edge_sink; }
    if (sim && !self) {
        od.transport.open = ad_open;
        od.transport.write = ad_write;
        od.transport.close = sim_close;
        od.transport.lines = ad_lines;
        od.transport.user = &g_sim;
    } else if (sim) {
        od.transport.open = sim_open;
        od.transport.read = sim_read;
        od.transport.write = sim_write;
        od.transport.close = sim_close;
        od.transport.user = &g_sim;
    }
#if !defined(__APPLE__)
    if (fam == YBOX_PARALLEL) od.transport = ydev_parallel_transport(&g_par);
#endif
    if (!ydev_start(&g_out, &od)) { fprintf(stderr, "out_latency: %s\n", ydev_error(&g_out)); return 2; }
    if (!self) {
        memset(&id, 0, sizeof id);
        id.role = "in";
        id.family = YBOX_LINE;
        id.key = sim ? "sim:board" : in_key;
        id.roles = &g_roles;
        id.kind = YIN_KIND_SYNC;
        id.sink = edge_sink;
        id.ring = &g_log;
        if (sim) {
            id.transport.open = sim_open;
            id.transport.read = sim_read;
            id.transport.write = sim_write;
            id.transport.close = sim_close;
            id.transport.user = &g_sim;
        }
        if (!ydev_start(&g_in, &id)) { fprintf(stderr, "out_latency: %s\n", ydev_error(&g_in)); ydev_stop(&g_out); return 2; }
    }
    /* both running, then the warm-up: the input's fit needs its span */
    for (i = 0; i < 100; i++) {
        if (ydev_state(&g_out) == YDEV_RUNNING && (self || ydev_state(&g_in) == YDEV_RUNNING)) break;
        (void)yrt_sleep_ns(100000000);
    }
    if (ydev_state(&g_out) != YDEV_RUNNING || (!self && ydev_state(&g_in) != YDEV_RUNNING)) {
        fprintf(stderr, "out_latency: not running after 10 s: out %s%s%s\n", ydev_state_name(ydev_state(&g_out)),
                self ? "" : ", in ", self ? "" : ydev_state_name(ydev_state(&g_in)));
        ydev_stop(&g_out);
        if (!self) ydev_stop(&g_in);
        return 2;
    }
    ydev_get_stats(&g_out, &ost);
    printf("out_latency: out %s at %s (role %d), %s; warming up %.1f s\n", famname, ost.found, g_out.d.device,
           self ? "wired to its own input" : "into the board at --in", warmup_s);
    (void)yrt_sleep_ns((int64_t)(warmup_s * 1e9));
    {
        yrt_event junk[256];
        while (yrt_ring_drain(&g_edges, junk, 256) > 0) { }
    }

    for (i = 0; i < n; i++) {
        ydev_out_info oi;
        int64_t until;
        int got = 0, rc;
        rc = ydev_out_pulse(&g_out, 1, (int64_t)(width_s * 1e9 + 0.5));
        ydev_out_last(&g_out, &oi);
        if (rc != YDEV_OK) { fprintf(stderr, "out_latency: pulse %d: error %d\n", i, rc); missed++; continue; }
        until = oi.t_before + 100000000;
        while (yrt_now_ns() < until && got < 2) {
            yrt_event r[16];
            int k, m = yrt_ring_drain(&g_edges, r, 16);
            for (k = 0; k < m; k++) {
                int64_t t = (int64_t)r[k].t_ns;
                if (t < oi.t_before - 5000000) continue;
                if (got == 0) {
                    g_lat[nl++] = (double)(t - oi.t_before) / 1e9;
                    g_unc[nu++] = (double)r[k].aux * 1e-6;
                    g_wid[nw] = (double)t;
                    got = 1;
                } else if (got == 1) {
                    g_wid[nw] = ((double)t - g_wid[nw]) / 1e9;
                    nw++;
                    got = 2;
                }
            }
            if (m == 0) (void)yrt_sleep_ns(200000);
        }
        if (!got) missed++;
        (void)yrt_sleep_ns((int64_t)(50e6 + 100e6 * (double)rand() / (double)RAND_MAX));
    }
    ydev_get_stats(&g_out, &ost);
    if (!self) ydev_get_stats(&g_in, &ist); else ist = ost;
    ydev_stop(&g_out);
    if (!self) ydev_stop(&g_in);

    /* the board's own reports of its pin changes ("O"), against the write */
    {
        static yrt_event recs[65536];
        int m = yrt_ring_drain(&g_log, recs, 65536), k;
        int64_t last_write = 0;
        for (k = 0; k < m && nr < MAXN; k++) {
            const yrt_event* r = &recs[k];
            if (r->source != YRT_SRC_DEVICE || r->kind != YDEV_REC_OUT || r->aux != (uint32_t)g_out.d.device) continue;
            if ((r->u.u16[18] & YDEV_OUT_PULSE) && !(r->u.u16[18] & YDEV_OUT_FAILED)) last_write = r->u.i64[0];
            else if ((r->u.u16[18] & YDEV_OUT_REPORTED) && r->u.u32[8] != 0 && last_write)
                g_rep[nr++] = (double)(r->u.i64[0] - last_write) / 1e9;
        }
    }

    qsort(g_lat, (size_t)nl, sizeof g_lat[0], cmp_d);
    qsort(g_rep, (size_t)nr, sizeof g_rep[0], cmp_d);
    qsort(g_wid, (size_t)nw, sizeof g_wid[0], cmp_d);
    qsort(g_unc, (size_t)nu, sizeof g_unc[0], cmp_d);
    {
        double mean = 0.0;
        time_t now = time(NULL);
        char date[32];
        int k;
        struct tm* gm = gmtime(&now);
        strftime(date, sizeof date, "%Y-%m-%dT%H:%M:%SZ", gm);
        for (k = 0; k < nl; k++) mean += g_lat[k];
        mean = nl ? mean / nl : 0.0;
#define PUT(...) do { int w_ = snprintf(text + tl, sizeof text - tl, __VA_ARGS__); if (w_ > 0) tl += (size_t)w_; } while (0)
        PUT("# ysp output latency: edge minus the time before the write (docs/devices_spec.md 12)\n");
        PUT("format ysp-out-latency 1\n");
        PUT("date %s\n", date);
        PUT("ysp_device %s\n", ydev_version());
        PUT("out_family %s%s\n", famname, sim ? " (simulated)" : "");
        PUT("out_key %s\n", out_key);
        PUT("out_found %s\n", ost.found);
        PUT("in_key %s\n", self ? out_key : (sim ? "sim:board" : in_key));
        PUT("in_ident %s\n", ist.ident);
        PUT("wiring %s\n", self ? "the board's output pin into its input pin" : "the output's bit 0 into the board's input pin");
        PUT("width_s %.6f\n", width_s);
        PUT("n %d\n", nl);
        PUT("missed %d\n", missed);
        if (nl) {
            PUT("min_s %.7f\n", g_lat[0]);
            PUT("p5_s %.7f\n", pctl(g_lat, nl, 0.05));
            PUT("median_s %.7f\n", pctl(g_lat, nl, 0.5));
            PUT("p95_s %.7f\n", pctl(g_lat, nl, 0.95));
            PUT("max_s %.7f\n", g_lat[nl - 1]);
            PUT("mean_s %.7f\n", mean);
        }
        if (nu) PUT("in_unc_median_s %.7f\n", pctl(g_unc, nu, 0.5));
        if (nw) PUT("width_at_input_median_s %.7f\n", pctl(g_wid, nw, 0.5));
        if (nr) {
            PUT("reported_n %d\n", nr);
            PUT("reported_median_s %.7f\n", pctl(g_rep, nr, 0.5));
            PUT("reported_p95_s %.7f\n", pctl(g_rep, nr, 0.95));
        }
        PUT("in_fit_ppm %.3f\n", ist.fit.ppm);
        PUT("samples_s");
        for (k = 0; k < nl; k++) PUT(" %.7f", g_lat[k]);
        PUT("\n");
#undef PUT
    }
    {
        FILE* f = fopen(path, "wb");
        if (!f || fwrite(text, 1, tl, f) != tl) { fprintf(stderr, "out_latency: cannot write %s\n", path); if (f) fclose(f); return 2; }
        fclose(f);
    }
    sha256_hex((const uint8_t*)text, tl, hex);
    {
        /* the file without its samples line, on the console */
        const char* s = strstr(text, "samples_s");
        fwrite(text, 1, s ? (size_t)(s - text) : tl, stdout);
    }
    printf("sha256 %s  %s\n", hex, path);
    if (sim && g_sim.ntruth > 0) {
        double* tr = g_sim.truth;
        int m = g_sim.ntruth, k;
        for (k = 0; k < m; k++) tr[k] /= 1e9;
        qsort(tr, (size_t)m, sizeof tr[0], cmp_d);
        printf("simulated truth (pin change minus the transport call): n %d, p5 %.7f, median %.7f, p95 %.7f, max %.7f s\n",
               m, pctl(tr, m, 0.05), pctl(tr, m, 0.5), pctl(tr, m, 0.95), tr[m - 1]);
    }
    if (do_store) {
        int rc = store(text, tl, rig_name, rig_dir, role, fam, latched, out_key);
        if (rc) return rc;
    }
    return nl >= (n * 9) / 10 ? 0 : 1;
}
