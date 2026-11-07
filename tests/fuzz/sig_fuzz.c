/* sig_fuzz.c - fuzz target for ysp/sig.h: ysig_check() and ysig_open() over
 * arbitrary node arrays (a definition's graph is untrusted input), then
 * every call on a graph that opened. The input bytes are the graph:
 *   byte 0     the node count (1..40) and whether a role and two adapters
 *              (a mouse and a pen, chosen by later bytes) come with it
 *   per node   op, four inputs (-1..n, so forward edges and cycles occur),
 *              lanes, history, then 16 parameters, each a byte that picks a
 *              value from a table (zeros, small integers, fractions, huge,
 *              negative, NaN, infinity), and for sines a data length
 *   the rest   a stream of events (kind, control, device, time steps that
 *              go back too), frames, resets, sets, arms, value reads
 * Every input must return without a sanitizer report, and a graph that
 * opened must replay its own journal to the same digest. A refusal is a
 * correct answer to a bad graph.
 *
 * libFuzzer, clang:
 *     clang -O1 -g -fsanitize=fuzzer,address,undefined -Iinclude tests/fuzz/sig_fuzz.c -o sig_fuzz -lm
 * Self-driven (no libFuzzer: gcc; random inputs and mutations of the
 * seeds, N iterations; on this kernel ASan needs setarch -R):
 *     gcc -std=c11 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -DYSP_FUZZ_SELF \
 *         -Iinclude tests/fuzz/sig_fuzz.c -o sig_self -lm && setarch -R ./sig_self 1000000
 * Replay of files: -DYSP_FUZZ_REPLAY in place of -DYSP_FUZZ_SELF, the files
 * as arguments.
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#define YSP_SIG_IMPLEMENTATION
#include "ysp/sig.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct rd { const uint8_t* p; size_t n, at; } rd;
static unsigned rb(rd* r) { return r->at < r->n ? r->p[r->at++] : 0u; }

static const double g_vals[32] = { 0, 1, -1, 2, 3, 0.5, 0.25, 4, 7, 10, 100, 1000, -5, 0.01, 1e-6, 30,
                                   300, 1e9, -1e9, 1e12, 1e300, -1e300, 65535, 4294967295.0, 0.001, 8000, 90,
                                   -90, 1e-300, 0, 0, 0 };

static uint64_t g_arena[(4u << 20) / 8];
static uint64_t g_arena2[(4u << 20) / 8];
static float g_ch[16];
static volatile double g_sink;
static long long g_taken, g_rows_all, g_hits_all, g_refused;

#define J_CAP 2048
static ysig_entry g_j[J_CAP];
static float g_jv[J_CAP][4];
static int g_nj;
static void on_journal(void* user, const ysig_entry* e) {
    (void)user;
    if (g_nj >= J_CAP) { g_nj = J_CAP + 1; return; }
    g_j[g_nj] = *e;
    if (e->kind == YSIG_J_FRAME) {
        int k;
        for (k = 0; k < e->frame.n_values && k < 4; k++) g_jv[g_nj][k] = e->frame.values[k];
        g_j[g_nj].frame.values = e->frame.n_values ? g_jv[g_nj] : NULL;
    }
    g_nj++;
}
static void on_row(void* user, const ysig_row* r) { (void)user; g_sink += r->value.v[0]; }

/* a custom op, so the vtable path is fuzzed too */
static void cu_sample(void* st, const double* p, const ysig_value* in, int n_in, ysig_value* out) {
    double* s = (double*)st;
    (void)n_in;
    *s += in[0].v[0] * p[0];
    out->v[0] = *s;
}
static const ysig_param cu_params[] = { { "k", "f64", -10, 10, 0, "", "gain", NULL } };
static const ysig_op_vtable cu_vt = { YSIG_OP_ABI, "acc", { 0, 0, 0, 0 }, 1, sizeof(double), cu_params, 1, NULL,
                                      cu_sample, NULL };

static void build_node(rd* r, ysig_node* nd, int i, int n, double* data) {
    int k, op = (int)(rb(r) % 26);
    *nd = ysig_node_of(op == 25 ? YSIG_OP_CUSTOM : op, -1, -1);
    for (k = 0; k < 4; k++) {
        int v = (int)(rb(r) % (unsigned)(n + 2)) - 1;
        nd->in[k] = (int16_t)(k < 2 || v < i ? v : -1);
    }
    nd->lanes = (uint8_t)(rb(r) % 6);
    k = (int)rb(r);
    nd->history = k < 200 ? 0 : k - 199;
    for (k = 0; k < YSIG_NP; k++) {
        unsigned b = rb(r);
        nd->p[k] = b < 224 ? g_vals[b % 29] : b < 240 ? (double)(b - 224) : b == 254 ? NAN : b == 255 ? INFINITY : -(double)(b - 240);
    }
    if (op == YSIG_OP_SINES || (rb(r) & 31) == 0) {
        int m = (int)(rb(r) % 48);
        for (k = 0; k < m; k++) data[k] = g_vals[rb(r) % 29];
        nd->data = data;
        nd->n_data = m;
    }
    nd->name = (i & 1) ? "node" : NULL;
}

static void run_one(const uint8_t* data, size_t size) {
    static ysig_node nodes[48], mouse[3], pen[2];
    static double ndata[48][48], adata[5][48];
    const ysig_op_vtable* vts[1];
    ysig_role role;
    ysig_adapter ad;
    ysig_desc d;
    ysig_graph g, g2;
    rd r;
    char err[256];
    int n, i, ops = 0;
    unsigned b0;
    int64_t t = 1000;
    r.p = data;
    r.n = size;
    r.at = 0;
    b0 = rb(&r);
    n = 1 + (int)(b0 % 40);
    for (i = 0; i < n; i++) build_node(&r, &nodes[i], i, n, ndata[i]);
    memset(&d, 0, sizeof d);
    d.nodes = nodes;
    d.n_nodes = n;
    d.channels = g_ch;
    d.n_channels = (int)(rb(&r) % 17);
    vts[0] = &cu_vt;
    d.custom = vts;
    d.n_custom = 1;
    d.on_row = on_row;
    d.journal = on_journal;
    if (b0 & 0x80) {
        /* a role and its adapter: a mouse, or a pen, from the bytes */
        memset(&role, 0, sizeof role);
        role.name = "hand";
        role.type = (uint8_t)(rb(&r) % 7);
        role.units = (uint8_t)(rb(&r) % 6);
        role.allow_relative = rb(&r) & 1;
        memset(&ad, 0, sizeof ad);
        ad.role = (rb(&r) & 7) ? "hand" : "eye";
        ad.type = (uint8_t)(rb(&r) % 7);
        ad.units = (uint8_t)(rb(&r) % 6);
        if (rb(&r) & 1) {
            for (i = 0; i < 3; i++) build_node(&r, &mouse[i], i, 3, adata[i]);
            ad.nodes = mouse;
            ad.n_nodes = 1 + (int)(rb(&r) % 3);
        } else {
            for (i = 0; i < 2; i++) build_node(&r, &pen[i], i, 2, adata[3 + i]);
            ad.nodes = pen;
            ad.n_nodes = 1 + (int)(rb(&r) % 2);
        }
        d.roles = &role;
        d.n_roles = 1;
        d.adapters = &ad;
        d.n_adapters = 1;
    }
    err[0] = 0;
    if (!ysig_check(&d, err, sizeof err)) { g_sink += (double)strlen(err); g_refused++; return; }
    d.arena_size = ysig_arena_size(&d);
    if (!d.arena_size || d.arena_size > sizeof g_arena) return;
    d.arena = g_arena;
    if (!ysig_open(&g, &d)) { fprintf(stderr, "sig_fuzz: check passed, open refused: %s\n", ysig_error(&g)); abort(); }
    g_nj = 0;
    ysig_reset(&g, t);
    while (r.at < r.n && ops < 4000) {
        unsigned c = rb(&r);
        ops++;
        switch (c % 8) {
        case 0: case 1: case 2: case 3: {
            yin_event e;
            unsigned k = rb(&r);
            memset(&e, 0, sizeof e);
            t += (int64_t)(rb(&r) % 16) * 100000 - ((k & 0x80) ? 300000 : 0);
            e.t = t;
            e.kind = (uint8_t)(k % 9);
            e.type = (uint8_t)(1 + rb(&r) % 4);
            e.control = rb(&r) % 4;
            e.device = rb(&r) % 4;
            e.x = (float)g_vals[rb(&r) % 29];
            e.y = (float)((int)rb(&r) - 128);
            e.value = (float)g_vals[rb(&r) % 29];
            e.stamp = (uint8_t)(rb(&r) % 6);
            g_sink += ysig_feed(&g, &e);
            break;
        }
        case 4: {
            ysig_frame_in f;
            float v[4];
            int k;
            for (k = 0; k < 4; k++) v[k] = (float)g_vals[rb(&r) % 29];
            memset(&f, 0, sizeof f);
            t += 16666667;
            f.index = ops;
            f.onset_ns = t;
            f.base_ns = (int64_t)rb(&r) * 10000000 - 500000000;
            f.values = v;
            f.n_values = (int)(rb(&r) % 5);
            g_sink += ysig_frame(&g, &f);
            break;
        }
        case 5: {
            ysig_hit h[8];
            yin_event e[8];
            ysig_value v;
            int node = (int)(rb(&r) % (unsigned)(g.n + 1));
            g_sink += ysig_detected(&g, h, (int)(rb(&r) % 9));
            g_sink += ysig_derived(&g, e, (int)(rb(&r) % 9));
            if (ysig_value_at(&g, node, t - (int64_t)rb(&r) * 1000000, &v) >= 0) g_sink += v.v[0];
            if (ysig_value_of(&g, node)) g_sink += ysig_value_of(&g, node)->v[0];
            break;
        }
        case 6: {
            int node = (int)(rb(&r) % (unsigned)(g.n + 1)), prm = (int)(rb(&r) % 17);
            unsigned b = rb(&r);
            double v = b < 240 ? g_vals[b % 29] : NAN;
            if (b & 1) g_sink += ysig_set(&g, node, prm, v);
            else g_sink += ysig_set_data(&g, node, prm, v);
            break;
        }
        default: {
            int node = (int)(rb(&r) % (unsigned)(g.n + 1));
            unsigned b = rb(&r);
            if (b % 3 == 0) ysig_reset(&g, t);
            else if (b % 3 == 1) g_sink += ysig_arm(&g, node, t - (int64_t)(b % 7) * 1000000);
            else g_sink += ysig_disarm(&g, node);
            break;
        }
        }
    }
    /* the journal replays to the same digest */
    if (g_nj <= J_CAP) {
        ysig_stats a, b;
        int nj = g_nj;
        ysig_get_stats(&g, &a);
        g_taken += a.taken;
        g_rows_all += a.rows;
        g_hits_all += a.hits;
        d.journal = NULL;
        d.arena = g_arena2;
        if (!ysig_open(&g2, &d)) abort();
        if (ysig_replay(&g2, g_j, nj) != nj) { fprintf(stderr, "sig_fuzz: replay refused\n"); abort(); }
        ysig_get_stats(&g2, &b);
        if (a.digest != b.digest || a.rows != b.rows || a.hits != b.hits) {
            fprintf(stderr, "sig_fuzz: replay digest %016llx, live %016llx\n", (unsigned long long)b.digest,
                    (unsigned long long)a.digest);
            abort();
        }
        for (i = 0; i < g.n; i++)
            if (memcmp(ysig_value_of(&g, i), ysig_value_of(&g2, i), sizeof(ysig_value)) &&
                !(isnan(ysig_value_of(&g, i)->v[0]) || isnan(ysig_value_of(&g, i)->v[1]) ||
                  isnan(ysig_value_of(&g, i)->v[2]) || isnan(ysig_value_of(&g, i)->v[3]))) {
                fprintf(stderr, "sig_fuzz: replay value of node %d differs\n", i);
                abort();
            }
    }
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);
int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    run_one(data, size);
    return 0;
}

#if defined(YSP_FUZZ_SELF) || defined(YSP_FUZZ_SEEDS)
static uint64_t g_rng = 0x5167u;
static uint64_t gen(void) {
    uint64_t z = (g_rng += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
static uint64_t mix(void) { return gen(); }
/* Seeds: graphs that open (a cursor, two hands, a target with a detect), as
 * byte strings in the decoder's form, so mutations start near valid. */
static size_t seed(int which, uint8_t* o) {
    size_t n = 0;
    int i, k;
    /* op, in0..in3 (stored + 1 under the modulo), lanes, history, 16 params, a data flag */
    static const uint8_t cursor[][24] = {
        { 1, 0, 0, 0, 0, 2, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 },   /* source mouse delta */
        { 3, 1, 0, 0, 0, 0, 0, 6, 6, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 },   /* integrate 0.25 */
        { 9, 2, 0, 0, 0, 0, 210, 0, 0, 0, 0, 0, 0, 0, 0, 26, 0, 0, 0, 0, 0, 0, 0, 1 }, /* affine 90 deg, history */
        { 22, 3, 0, 0, 0, 0, 0, 0, 2, 9, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 },  /* detect |v| >= 10 */
        { 24, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 },  /* emit */
        { 23, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 },  /* record */
        { 5, 3, 0, 0, 0, 0, 0, 13, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 },  /* velocity 10 ms */
        { 21, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 },  /* bind */
    };
    int count = which == 0 ? 4 : 8;
    if (which >= 2) {
        /* a random graph in the decoder's form, biased to open: inputs
         * earlier, lanes from the op, parameters mostly small; the event
         * stream from the mice and the pen */
        int nn = 2 + (int)(gen() % 30), ops[] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25 };
        o[n++] = (uint8_t)((nn - 1) | (gen() % 4 == 0 ? 0x80 : 0));
        for (i = 0; i < nn; i++) {
            int op = i < 3 ? 1 : ops[gen() % (sizeof ops / sizeof ops[0])];
            o[n++] = (uint8_t)op;
            for (k = 0; k < 4; k++) {
                /* inputs by the op: none for a source or a generator, two
                 * for diff, one or two for the combiners, one else */
                int gen_op = op == 1 || op == 18 || op == 19 || op == 20, two = op == 17 || ((op == 14 || op == 15 || op == 16 || op == 9 || op == 10) && gen() % 2);
                int pick = !gen_op && i && (k == 0 || (k == 1 && two)) ? 1 + (int)(gen() % (unsigned)i) : 0;
                o[n++] = (uint8_t)pick;
            }
            o[n++] = (uint8_t)(op == 1 || op == 18 ? 2 : op == 13 || op == 19 ? 1 + gen() % 2 : 0);
            o[n++] = (uint8_t)(gen() % 8 == 0 ? 200 + gen() % 50 : 0);
            for (k = 0; k < 16; k++) {
                uint64_t m = gen() % 10;
                /* a source: a mouse (1) or a pen (3, at g_vals[4]); delta or
                 * position; device 0 or 1 */
                o[n++] = (uint8_t)(op == 1 ? (k == 0 ? (gen() % 2 ? 1 : 4) : k == 1 || k == 2 ? gen() % 2 : 0)
                                   : op >= 4 && op <= 7 && k == 0 ? (op == 4 ? 15 : 13)
                                   : op == 18 && k == 0 ? 1
                                   : m < 8 ? 0 : m < 9 ? 1 + gen() % 9 : gen());
            }
            o[n++] = (uint8_t)(op == 18 ? 6 : 1);
        }
        o[n++] = 16;
        for (i = 0; i < 600 && n + 12 < 16000; i++) {
            uint64_t m = gen() % 12;
            o[n++] = (uint8_t)(m < 8 ? 0 : m == 8 ? 4 : (uint8_t)gen());
            o[n++] = (uint8_t)(gen() % 2 ? 1 : 3);   /* kind: mouse, pen */
            o[n++] = (uint8_t)(gen() % 16);
            o[n++] = 2;
            o[n++] = (uint8_t)(gen() % 2);
            o[n++] = (uint8_t)(gen() % 3);
            o[n++] = (uint8_t)(gen() % 29);
            o[n++] = (uint8_t)(120 + gen() % 17);
            o[n++] = (uint8_t)(gen() % 29);
            o[n++] = (uint8_t)(gen() % 6);
        }
        return n;
    }
    o[n++] = (uint8_t)(count - 1);
    for (i = 0; i < count; i++)
        for (k = 0; k < 24; k++) o[n++] = cursor[i][k];
    o[n++] = 4;   /* channels */
    for (i = 0; i < 300; i++) {
        o[n++] = (uint8_t)(i % 5 == 4 ? 4 : i % 7 == 6 ? 5 : 0);
        o[n++] = 1;   /* mouse */
        o[n++] = 2;   /* 0.2 ms on */
        o[n++] = 2;   /* sample */
        o[n++] = 1;   /* delta */
        o[n++] = 0;
        o[n++] = (uint8_t)(i % 29);
        o[n++] = (uint8_t)(128 + i % 7);
        o[n++] = (uint8_t)(i % 3);
        o[n++] = 4;
    }
    return n;
}
#endif

#if defined(YSP_FUZZ_SELF)
int main(int argc, char** argv) {
    static uint8_t buf[1 << 14];
    long iters = argc > 1 ? atol(argv[1]) : 100000, it;
    long opened = 0;
    if (argc > 2) g_rng = (uint64_t)strtoull(argv[2], NULL, 10);
    for (it = 0; it < iters; it++) {
        size_t n, k;
        uint64_t m = mix();
        if (m % 4 == 0) {
            /* random bytes */
            n = (size_t)(mix() % 2048);
            for (k = 0; k < n; k++) buf[k] = (uint8_t)mix();
        } else {
            /* a seed with a few bytes changed, inserted or cut */
            int edits = 1 + (int)(mix() % 8), e;
            n = seed((int)(m % 6), buf);
            for (e = 0; e < edits; e++) {
                size_t at = (size_t)(mix() % n);
                switch (mix() % 4) {
                case 0: case 1: buf[at] = (uint8_t)mix(); break;
                case 2: if (n + 1 < sizeof buf) { memmove(buf + at + 1, buf + at, n - at); buf[at] = (uint8_t)mix(); n++; } break;
                default: if (n > 2) { memmove(buf + at, buf + at + 1, n - at - 1); n--; } break;
                }
            }
        }
        g_ch[0] = 0;
        g_nj = 0;
        run_one(buf, n);
        opened += g_nj > 0;   /* a graph opened and ran: not only refusals */
    }
    printf("sig_fuzz: %ld inputs, %lld refused by ysig_check(), %ld graphs opened and ran: %lld events taken, %lld rows, "
           "%lld hits\n", iters, g_refused, opened, g_taken, g_rows_all, g_hits_all);
    return 0;
}
#elif defined(YSP_FUZZ_SEEDS)
int main(int argc, char** argv) {
    static uint8_t buf[1 << 14];
    int i;
    if (argc < 2) { fprintf(stderr, "usage: seeds DIR\n"); return 1; }
    for (i = 0; i < 12; i++) {
        char path[1024];
        FILE* f;
        size_t n = seed(i < 2 ? i : 2, buf);
        snprintf(path, sizeof path, "%s/seed%d", argv[1], i);
        f = fopen(path, "wb");
        if (!f) return 1;
        fwrite(buf, 1, n, f);
        fclose(f);
    }
    return 0;
}
#elif defined(YSP_FUZZ_REPLAY)
int main(int argc, char** argv) {
    static uint8_t buf[1 << 20];
    int i, done = 0;
    for (i = 1; i < argc; i++) {
        FILE* f = fopen(argv[i], "rb");
        size_t n;
        if (!f) continue;
        n = fread(buf, 1, sizeof buf, f);
        fclose(f);
        run_one(buf, n);
        done++;
    }
    printf("sig_fuzz: replayed %d inputs\n", done);
    return 0;
}
#endif
