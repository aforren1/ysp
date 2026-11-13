/* sig_arm_oracle.c - a time-order oracle for ysp/sig.h's arm in the past
 * (v0.6.0, AN ARM IN THE PAST). Returns 0 when no case is wrong.
 *
 * A makes the calls as generated, on a graph with a window. B makes the
 * same calls on a graph with no window, in the order the guarantee
 * states, built here from the calls alone (not from the header's log):
 *   - inputs (ysig_feed, ysig_frame, ysig_settle), resets, ysig_set,
 *     ysig_disarm and ysig_recenter stay where they were made;
 *   - each ysig_arm(t) goes after every input made before it whose time
 *     is at or before t (a late report from another device included),
 *     after every set, disarm and reset made before it, after every
 *     recenter made before it at or before t, and after every arm made
 *     before it at or before t; among arms in one gap, by (t, call order);
 *   - an arm A made after input later than t knows that input up to t is
 *     in, so it meets its detect's old hold up to t (the guarantee's own
 *     hold completion). B's graph cannot know that: no public call says
 *     "input is in up to t" for one detect without more (ysig_settle()
 *     completes every detect's hold and raises the graph's input time,
 *     which moves ring pushes and the skip and clamp counts for later
 *     late reports). So B makes that one completion itself, with the
 *     header's internal ysig__settle_det(g, node, t, 1), just before
 *     the arm.
 * In time-ordered input that is the calls sorted by (time, call order).
 * Both drain after the same calls. At the end A and B must hold the same
 * values, rings, op state, stats (but the window's own counts), and bound
 * channels; B's hits must all be in A's; each hit A handed out that B
 * does not have must be counted: retracted when the caller could have
 * taken it in time order (no later past arm is before it), superseded
 * otherwise; rows and derived events must be equal, or counted stale.
 * Cases whose window fell back are checked for "counted" only.
 *
 * Shapes: several detects (one on all() of two detects, one on a latch
 * of a hit, one on a view, frame-rate ones), emit and records, a channel
 * node and ysig_set of its index, arms later than the newest input,
 * set, disarm and recenter before an arm, late out-of-order reports from
 * a second device, bursts of arms at one time, drains at random points.
 *
 *     gcc -std=c99 -O1 -Iinclude -o sig_arm_oracle tests/adapt/sig_arm_oracle.c -lm
 *     ./sig_arm_oracle [cases] [seed] [show]
 * ORACLE_OFF=letters leaves shapes out (see off()); ORACLE_CASE=n prints
 * case n's calls when it is wrong.
 *
 * On v0.6.0 as reviewed (2026-11-13, Linux desktop, GCC 12.2 -O2; 5,000
 * cases a row, seed 7; the first failing check a case):
 *   shapes on                               compared  wrong
 *   arms only (in-order input, full window)    4,538  3,665  derived_lost 2,588 (6),
 *                                                            a taken hit counted superseded 456 (1),
 *                                                            state 311, rings 155, B's hit not in A 70
 *   + late reports (1)                         4,500  3,888  state, rings, values 1,518
 *   + ysig_set (3)                             4,545  4,150  state, rings, values 2,843
 *   + disarm (3)                               4,498  3,947
 *   + recenter, relative role (3)              4,494  3,968  ring entries 1,049
 *   + channel index sets (5)                   4,499  4,275  ring entries 1,609
 *   + future arms (4)                          4,499  3,416
 *   everything                                 3,458  3,276
 * with no arm at all: 0 of 2,000 wrong. One defect the review did not
 * list: an arm called later with an earlier time than a logged arm (no
 * input between) was placed after it, so the earlier time won.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define YSP_SIG_IMPLEMENTATION
#include "ysp/sig.h"

#define MS 1000000LL
#define MAXC 1200
#define NCH 8

enum { K_FEED, K_MOUSE, K_FRAME, K_ARM, K_RESET, K_DRAIN, K_SETA, K_SETCH, K_SETLV, K_DISARM, K_SETTLE, K_RECENTER };
typedef struct { int kind, dev, node; int64_t t; double x, y; int idx, ai; } call;   /* ai: A's index */
typedef struct { ysig_hit h[4096]; int n; int at[4096]; } hits;
typedef struct { uint64_t h; long n; } stream;

static uint64_t g_ar_a[(16u << 20) / 8], g_ar_b[(16u << 20) / 8];
static float g_ch_a[NCH], g_ch_b[NCH];
static uint64_t g_rs = 88172645463325252ull;
static uint32_t rnd(void) { g_rs ^= g_rs << 13; g_rs ^= g_rs >> 7; g_rs ^= g_rs << 17; return (uint32_t)(g_rs >> 11); }
static double rndf(void) { return (rnd() & 0xffffff) / (double)0x1000000; }

/* ORACLE_OFF: letters for shapes to leave out (a arms, l late reports,
 * s sets, d disarms, r recenters, c channel sets, f future arms, g
 * relative graph, w small windows), to check the oracle itself */
static int off(char c) { const char* o = getenv("ORACLE_OFF"); return o && strchr(o, c) != NULL; }

static ysig_node g_n[24], g_ad[2];
static ysig_role g_role;
static ysig_adapter g_adp;
static int g_nn, g_rel, g_dets[6], g_ndet, g_chnode;
static int g_K = 8, g_C = 4096, g_W = 1 << 16;
static stream g_rows_a, g_rows_b, *g_rows;
static call g_ca[MAXC], g_cb[MAXC];
static int g_na, g_nb;
static hits g_ha, g_hb;
static uint64_t g_der_a, g_der_b;
static long g_nder_a, g_nder_b;

static uint64_t mix(uint64_t h, const void* p, size_t n) {
    const unsigned char* c = (const unsigned char*)p;
    size_t i;
    for (i = 0; i < n; i++) { h ^= c[i]; h *= 1099511628211ull; }
    return h;
}
/* every column of a row (finding 7: metadata too) */
static void on_row(void* u, const ysig_row* r) {
    (void)u;
    g_rows->h = mix(g_rows->h, &r->node, sizeof r->node);
    g_rows->h = mix(g_rows->h, &r->per_frame, sizeof r->per_frame);
    g_rows->h = mix(g_rows->h, &r->frame, sizeof r->frame);
    g_rows->h = mix(g_rows->h, &r->onset, sizeof r->onset);
    g_rows->h = mix(g_rows->h, &r->value, sizeof r->value);
    g_rows->n++;
}

static void det_params(ysig_node* n, int one_lane, int view) {
    int test = (int)(rnd() % (one_lane ? 2 : 4));
    n->p[YSIG_DETECT_TEST] = test;
    n->p[YSIG_DETECT_LANE] = one_lane ? 0 : test >= 2 ? -1 : (int)(rnd() % 3) - 1;
    n->p[YSIG_DETECT_LEVEL] = one_lane && rnd() % 2 ? 0.5 : 1 + rnd() % 6;
    n->p[YSIG_DETECT_A_X] = -3; n->p[YSIG_DETECT_A_Y] = -3; n->p[YSIG_DETECT_B_X] = 3; n->p[YSIG_DETECT_B_Y] = 3;
    n->p[YSIG_DETECT_HYSTERESIS] = (rnd() & 1) ? 0.5 : 0;
    n->p[YSIG_DETECT_HOLD] = (rnd() % 3) ? (rnd() % 8) * 0.002 : 0;
    if (view && rnd() % 2) n->p[YSIG_DETECT_HOLD] = 0.002 * (1 + rnd() % 8);
    n->p[YSIG_DETECT_AT] = rnd() % 5 == 0 ? 1 : 0;
    n->p[YSIG_DETECT_START] = (rnd() % 3) ? 1 : 0;
    n->p[YSIG_DETECT_REPEAT] = rnd() & 1;
    n->p[YSIG_DETECT_ENTRY] = rnd() % 3;
    n->p[YSIG_DETECT_OUT] = (rnd() % 3) ? YSIG_OUT_HIT : YSIG_OUT_TEST;
    n->history = 64;
}

static ysig_node pen_src(int dev) {
    ysig_node n = ysig_node_of(YSIG_OP_SOURCE, -1, -1);
    n.lanes = 2; n.p[0] = YIN_KIND_PEN; n.p[1] = YIN_AXIS_POSITION; n.p[2] = dev; n.history = 1024;
    return n;
}

/* pens (two devices, a sum, a view) or a relative role (recenter) */
static void make_graph(void) {
    static const int two[] = { 0, 2, 3, 5 };
    int in, base;
    memset(g_n, 0, sizeof g_n);
    g_rel = rnd() % 4 == 0 && !off('g');
    if (g_rel) {
        memset(&g_role, 0, sizeof g_role);
        g_role.name = "hand"; g_role.type = YSIG_TYPE_POSITION2D; g_role.units = YSIG_UNITS_DISPLAY_PX; g_role.allow_relative = true;
        g_ad[0] = ysig_node_of(YSIG_OP_SOURCE, -1, -1);
        g_ad[0].lanes = 2; g_ad[0].p[0] = YIN_KIND_MOUSE; g_ad[0].p[1] = YIN_AXIS_DELTA; g_ad[0].history = 1024;
        g_ad[1] = ysig_node_of(YSIG_OP_INTEGRATE, 0, -1);
        g_ad[1].p[0] = g_ad[1].p[1] = 1; g_ad[1].history = 1024;
        memset(&g_adp, 0, sizeof g_adp);
        g_adp.role = "hand"; g_adp.name = "mouse"; g_adp.type = YSIG_TYPE_POSITION2D_RELATIVE; g_adp.units = YSIG_UNITS_DISPLAY_PX;
        g_adp.nodes = g_ad; g_adp.n_nodes = 2;
        g_n[0] = ysig_node_of(YSIG_OP_ROLE, -1, -1); g_n[0].history = 1024;
        g_n[1] = ysig_node_of(YSIG_OP_AFFINE, 0, -1); g_n[1].history = 1024;
        g_n[2] = ysig_node_of(YSIG_OP_AFFINE, 0, -1); g_n[2].p[YSIG_AFFINE_ANGLE] = 10; g_n[2].history = 1024;
        g_n[3] = ysig_node_of(YSIG_OP_SUM, 1, 2); g_n[3].history = 1024;
    } else {
        g_n[0] = pen_src(1);
        g_n[1] = pen_src(2);
        g_n[2] = pen_src(0);
        g_n[3] = ysig_node_of(YSIG_OP_SUM, 0, 1); g_n[3].history = 1024;
    }
    g_n[4] = ysig_node_of(YSIG_OP_SPEED, 2, -1); g_n[4].p[0] = 0.004; g_n[4].history = 1024;
    g_n[5] = ysig_node_of(YSIG_OP_AFFINE, 0, -1); g_n[5].p[YSIG_AFFINE_ANGLE] = 30; g_n[5].history = 1024;
    in = rnd() % 5 == 0 ? 4 : two[rnd() % 4];
    g_n[6] = ysig_node_of(YSIG_OP_DETECT, in, -1); det_params(&g_n[6], in == 4, in == 4);
    in = rnd() % 5 == 0 ? 4 : two[rnd() % 4];
    g_n[7] = ysig_node_of(YSIG_OP_DETECT, in, -1); det_params(&g_n[7], in == 4, in == 4);
    g_n[8] = ysig_node_of(YSIG_OP_LATCH, rnd() & 1 ? 2 : g_n[6].in[0] == 4 ? 2 : g_n[6].in[0], 6); g_n[8].history = 64;
    g_n[9] = ysig_node_of(YSIG_OP_DETECT, 8, -1); det_params(&g_n[9], 0, 0);
    g_n[10] = ysig_node_of(YSIG_OP_ALL, 6, 7); g_n[10].history = 64;
    g_n[11] = ysig_node_of(YSIG_OP_PEAK, 2, 7); g_n[11].history = 64;
    g_n[12] = ysig_node_of(YSIG_OP_MUX, 0, 1); g_n[12].in[2] = 6; g_n[12].history = 64;
    g_n[13] = ysig_node_of(YSIG_OP_RECORD, 12, -1);
    g_n[14] = ysig_node_of(YSIG_OP_RECORD, 10, -1); g_n[14].p[0] = 1;
    g_n[15] = ysig_node_of(YSIG_OP_EMIT, 8, -1);
    g_n[16] = ysig_node_of(YSIG_OP_DELAY, 6, -1); g_n[16].p[0] = 0.003; g_n[16].history = 64;
    g_n[17] = ysig_node_of(YSIG_OP_DETECT, 10, -1); det_params(&g_n[17], 1, 0);
    g_n[18] = ysig_node_of(YSIG_OP_RECORD, 11, -1);
    base = 19;
    g_chnode = base;
    g_n[base] = ysig_node_of(YSIG_OP_CHANNEL, -1, -1); g_n[base].p[0] = rnd() % 2; g_n[base].history = 64;
    g_n[base + 1] = ysig_node_of(YSIG_OP_DETECT, base, -1); det_params(&g_n[base + 1], 1, 1);
    g_n[base + 2] = ysig_node_of(YSIG_OP_BIND, 6, -1); g_n[base + 2].p[0] = 4;
    g_n[base + 3] = ysig_node_of(YSIG_OP_RECORD, base, -1); g_n[base + 3].p[0] = 1;
    g_nn = base + 4;
    g_ndet = 0;
    g_dets[g_ndet++] = 6; g_dets[g_ndet++] = 7; g_dets[g_ndet++] = 9; g_dets[g_ndet++] = 17; g_dets[g_ndet++] = base + 1;
}

static int open_g(ysig_graph* g, uint64_t* ar, float* ch, int window) {
    ysig_desc d;
    memset(&d, 0, sizeof d);
    d.nodes = g_n; d.n_nodes = g_nn; d.channels = ch; d.n_channels = NCH; d.arena = ar;
    d.on_row = on_row;
    if (window) { d.catchup_frames = g_K; d.catchup_calls = g_C; d.catchup_writes = g_W; }
    if (g_rel) { d.roles = &g_role; d.n_roles = 1; d.adapters = &g_adp; d.n_adapters = 1; }
    d.arena_size = ysig_arena_size(&d);
    if (!d.arena_size || d.arena_size > sizeof g_ar_a) {
        char e[256];
        ysig_check(&d, e, sizeof e);
        fprintf(stderr, "sig_arm_oracle: check: %s\n", e);
        return 0;
    }
    if (!ysig_open(g, &d)) { fprintf(stderr, "sig_arm_oracle: open: %s\n", ysig_error(g)); return 0; }
    return 1;
}

static int is_input(const call* c, int64_t* t) {
    if (c->kind == K_FEED || c->kind == K_MOUSE || c->kind == K_FRAME || c->kind == K_SETTLE) { *t = c->t; return 1; }
    return 0;
}

static void drain_all(ysig_graph* g, hits* r, uint64_t* dh, long* nd, int at) {
    ysig_hit h[64];
    yin_event e[64];
    int k, m;
    while ((m = ysig_detected(g, h, 64)) > 0)
        for (k = 0; k < m && r->n < 4096; k++) { r->at[r->n] = at; r->h[r->n++] = h[k]; }
    while ((m = ysig_derived(g, e, 64)) > 0)
        for (k = 0; k < m; k++) { *dh = mix(*dh, &e[k], sizeof e[k]); (*nd)++; }
}

static void do_call(ysig_graph* g, const call* c, hits* r, uint64_t* dh, long* nd) {
    yin_event e;
    ysig_frame_in f;
    float v[NCH];
    int k;
    switch (c->kind) {
    case K_FEED:
    case K_MOUSE:
        memset(&e, 0, sizeof e);
        e.t = c->t; e.type = YIN_SAMPLE; e.device = (uint32_t)c->dev; e.x = (float)c->x; e.y = (float)c->y; e.stamp = YIN_TIER_2;
        if (c->kind == K_FEED) { e.kind = YIN_KIND_PEN; e.control = YIN_AXIS_POSITION; }
        else { e.kind = YIN_KIND_MOUSE; e.control = YIN_AXIS_DELTA; e.device = 1; }
        ysig_feed(g, &e);
        break;
    case K_FRAME:
        memset(&f, 0, sizeof f);
        for (k = 0; k < NCH; k++) v[k] = (float)((c->idx >> k) & 1);
        f.index = c->t / MS; f.onset_ns = c->t; f.poll_ns = c->t - (int64_t)c->dev * 300000; f.has_poll = true;
        f.values = v; f.n_values = NCH;
        ysig_frame(g, &f);
        break;
    case K_ARM:
#ifdef YSIG_ARM_REENTRY_COUNTED
        if (c->dev == 99) ysig__settle_det(g, c->node, c->t, 1);   /* B: input up to t is in */
#else
        if (c->dev == 99) ysig__settle_det(g, c->node, c->t);      /* v0.6.0 as reviewed */
#endif
        ysig_arm(g, c->node, c->t);
        break;
    case K_RESET: drain_all(g, r, dh, nd, c->idx); ysig_reset(g, c->t); break;   /* a reset clears undelivered hits: callers drain first */
    case K_DRAIN: drain_all(g, r, dh, nd, c->idx); break;
    case K_SETA: ysig_set(g, 5, YSIG_AFFINE_ANGLE, c->x); break;
    case K_SETCH: ysig_set(g, g_chnode, YSIG_CHANNEL_CHANNEL, (double)c->dev); break;
    case K_SETLV: ysig_set(g, c->node, YSIG_DETECT_LEVEL, c->x); break;
    case K_DISARM: ysig_disarm(g, c->node); break;
    case K_SETTLE: ysig_settle(g, c->t); break;
    case K_RECENTER: ysig_recenter(g, 0, c->x, c->y, c->t); break;
    }
}

/* B's order (the guarantee), from the calls alone */
static void build_b(void) {
    int k, p;
    g_nb = 0;
    for (k = 0; k < g_na; k++) {
        const call* c = &g_ca[k];
        g_ca[k].ai = k;
        if (c->kind != K_ARM) { g_cb[g_nb++] = *c; continue; }
        /* after the last call that must come first */
        for (p = g_nb; p > 0; p--) {
            const call* q = &g_cb[p - 1];
            int64_t ti;
            if (is_input(q, &ti) && ti <= c->t) break;
            if (q->kind == K_RESET || q->kind == K_SETA || q->kind == K_SETCH || q->kind == K_SETLV || q->kind == K_DISARM) break;
            if (q->kind == K_RECENTER && q->t <= c->t) break;
            if (q->kind == K_ARM && q->t <= c->t) break;
        }
        memmove(&g_cb[p + 1], &g_cb[p], sizeof(call) * (size_t)(g_nb - p));
        g_cb[p] = *c;
        {
            /* made after input later than t (since the last reset)? */
            int q2;
            int64_t ti;
            g_cb[p].dev = 0;
            for (q2 = k - 1; q2 >= 0 && g_ca[q2].kind != K_RESET; q2--)
                if (is_input(&g_ca[q2], &ti) && ti > c->t) { g_cb[p].dev = 99; break; }
        }
        g_nb++;
    }
}

static int cmp_hit(const ysig_hit* x, const ysig_hit* y) {
    if (x->t != y->t || x->node != y->node) return 1;
    return memcmp(&x->value, &y->value, sizeof x->value) != 0;
}

static int g_bad = -1;
static const char* same_state(const ysig_graph* a, const ysig_graph* b) {
    int i, k;
    g_bad = -1;
    ysig_stats sa = a->st, sb = b->st;
    for (i = 0; i < a->n; i++) {
        const ysig__nd* x = &a->nd[i];
        const ysig__nd* y = &b->nd[i];
        size_t sz = ysig__st_bytes(x);
        g_bad = i;
        if (memcmp(&a->val[i], &b->val[i], sizeof(ysig_value))) return "a value";
        if (x->hist.n != y->hist.n) return "a ring's length";
        for (k = 0; k < x->hist.n; k++)
            if (memcmp(ysig__ring_at(&x->hist, k), ysig__ring_at(&y->hist, k), sizeof(ysig_value))) return "a ring entry";
        if (x->op == YSIG_OP_VELOCITY || x->op == YSIG_OP_SPEED || x->op == YSIG_OP_DELAY) {
            const ysig__ring* ra = (const ysig__ring*)x->st;
            const ysig__ring* rb = (const ysig__ring*)y->st;
            if (ra->n != rb->n) return "an op ring's length";
            for (k = 0; k < ra->n; k++)
                if (memcmp(ysig__ring_at(ra, k), ysig__ring_at(rb, k), sizeof(ysig_value))) return "an op ring entry";
        } else if (sz && memcmp(x->st, y->st, sz)) {
            return "a state block";
        }
    }
    sa.arm_caught_up = sb.arm_caught_up = 0;
    sa.arm_past_off = sb.arm_past_off = 0;
    sa.arm_past_far = sb.arm_past_far = 0;
    sa.arm_past_full = sb.arm_past_full = 0;
    sa.arm_hits_retracted = sb.arm_hits_retracted = 0;
    sa.arm_hits_superseded = sb.arm_hits_superseded = 0;
    sa.arm_rows_stale = sb.arm_rows_stale = 0;
    sa.arm_derived_stale = sb.arm_derived_stale = 0;
#ifdef YSIG_ARM_REENTRY_COUNTED
    sa.arm_reentry = sb.arm_reentry = 0;
#endif
    /* the digest hashes the order things ran in: equal-time calls in
     * another, equivalent order leave the same state (compared above and
     * after) with another digest */
    sa.digest = sb.digest = 0;
    if (sa.hits_lost != sb.hits_lost) return "stats.hits_lost";
    if (sa.derived_lost != sb.derived_lost) return "stats.derived_lost";
    if (sa.hits != sb.hits) return "stats.hits";
    if (memcmp(&sa, &sb, sizeof sa)) return "the stats";
    if (memcmp(g_ch_a, g_ch_b, sizeof g_ch_a)) return "a bound channel";
    return NULL;
}

/* the review's findings, by the shape a case has */
enum { F_ALL, F1, F2, F3, F4, F5, F6, F7, NF };
static const char* g_fname[NF] = {
    "all cases",
    "1: an arm after a late report at or before t",
    "2: an arm of a detect on a view or at frame rate",
    "3: set, disarm or recenter before a past arm",
    "4: an arm later than the newest input",
    "5: ysig_set of a channel node's index",
    "6: a past arm with undrained queues",
    "7: rows or derived events (metadata)" };
static long g_n_case[NF], g_n_wrong[NF], g_n_back[NF];


int main(int argc, char** argv) {
    long cases = argc > 1 ? atol(argv[1]) : getenv("SIG_ORACLE_CASES") ? atol(getenv("SIG_ORACLE_CASES")) : 3000, it, wrong = 0, back = 0;
    const char* env_seed = getenv("SIG_ORACLE_SEED");
    int show = argc > 3 ? atoi(argv[3]) : 0, w;
    const char* whyname[32];
    long whys[32];
    int nwhy = 0;
    g_rs = argc > 2 ? strtoull(argv[2], NULL, 10) : env_seed ? strtoull(env_seed, NULL, 10) : g_rs;
    if (!g_rs) g_rs = 1;
    for (it = 0; it < cases; it++) {
        static ysig_graph ga, gb;
        int64_t t = 0, onset = 16 * MS, last[3] = { 0, 0, 0 }, newest = 0;
        double x = 0, y = 0;
        int k, j, nfeed = 40 + (int)(rnd() % 250), drains = 0;
        int f[NF];
        const char* why = NULL;
        ysig_stats sa;
        memset(f, 0, sizeof f);
        f[F_ALL] = 1;
        make_graph();
        if (rnd() % 3 == 0 && !off('w')) { g_K = 1 + (int)(rnd() % 8); g_C = 32 << (rnd() % 8); g_W = 128 << (rnd() % 10); }
        else { g_K = 8; g_C = 4096; g_W = 1 << 16; }
        g_na = 0;
        while (g_na < MAXC - 24 && nfeed-- > 0) {
            int r = (int)(rnd() % 1000);
            t += (rnd() % 4 == 0) ? 0 : (int64_t)(1 + rnd() % 2500) * 1000;
            while (t > onset && g_na < MAXC - 24) {
                g_ca[g_na].kind = K_FRAME; g_ca[g_na].t = onset; g_ca[g_na].dev = (int)(rnd() % 3); g_ca[g_na].idx = (int)(rnd() & 0xff); g_na++;
                if (onset > newest) newest = onset;
                if (rnd() % 3 == 0) { g_ca[g_na].kind = K_DRAIN; g_ca[g_na].idx = drains++; g_na++; }
                onset += 16 * MS + (int64_t)(rnd() % 3) * 1000;
            }
            if (rnd() % 4 == 0) { x = (rndf() - 0.5) * 16; y = (rndf() - 0.5) * 16; }
            else { x += (rndf() - 0.5) * 3; y += (rndf() - 0.5) * 3; }
            if (g_rel) {
                g_ca[g_na].kind = K_MOUSE; g_ca[g_na].t = t; g_ca[g_na].x = (rndf() - 0.5) * 4; g_ca[g_na].y = (rndf() - 0.5) * 4;
            } else {
                g_ca[g_na].kind = K_FEED; g_ca[g_na].dev = 1 + (int)(rnd() % 2); g_ca[g_na].t = t; g_ca[g_na].x = x; g_ca[g_na].y = y;
                if (r < 60 && !off('l')) g_ca[g_na].t = last[g_ca[g_na].dev] - (int64_t)(rnd() % 3000) * 1000;
                else if (r < 140 && !off('l')) { int o = 3 - g_ca[g_na].dev; g_ca[g_na].dev = o; g_ca[g_na].t = last[o] + (t - last[o]) / 2; }
                if (g_ca[g_na].t > last[g_ca[g_na].dev]) last[g_ca[g_na].dev] = g_ca[g_na].t;
            }
            if (g_ca[g_na].t > newest) newest = g_ca[g_na].t;
            g_na++;
            r = (int)(rnd() % 1000);
            if (r < 90 && !off('a')) {
                /* a burst of arms at one time: a state's entry, in the past,
                 * at an input's time, or later than the newest input */
                int m = 1 + (int)(rnd() % 4), sh = (int)(rnd() % 4);
                int64_t ta = t - (int64_t)(rnd() % 30) * MS;
                if (sh == 1 && g_na > 1) {
                    int q = (int)(rnd() % (unsigned)g_na);
                    int64_t tq;
                    if (is_input(&g_ca[q], &tq)) ta = tq;   /* exactly an input's time */
                }
                if (sh == 2 && !off('f')) ta = newest + (int64_t)(rnd() % 20) * MS;
                if (ta < 0) ta = 0;
                /* a set, disarm or recenter just before, as the player's
                 * 3.12 path makes them */
                if (rnd() % 3 == 0) {
                    int s = (int)(rnd() % 4);
                    if ((s == 0 && off('s')) || (s == 1 && off('d')) || (s == 2 && off('r')) || (s == 3 && off('c'))) s = -1;
                    if (s < 0) { g_ca[g_na].kind = K_DRAIN; g_ca[g_na].idx = drains++; }
                    else if (s == 0) { g_ca[g_na].kind = K_SETLV; g_ca[g_na].node = g_dets[rnd() % (unsigned)g_ndet]; g_ca[g_na].x = 0.5 + rnd() % 6; }
                    else if (s == 1) { g_ca[g_na].kind = K_DISARM; g_ca[g_na].node = g_dets[rnd() % (unsigned)g_ndet]; }
                    else if (s == 2 && g_rel) { g_ca[g_na].kind = K_RECENTER; g_ca[g_na].t = ta < newest ? ta : newest; g_ca[g_na].x = (rndf() - 0.5) * 10; g_ca[g_na].y = (rndf() - 0.5) * 10; }
                    else { g_ca[g_na].kind = K_SETCH; g_ca[g_na].dev = (int)(rnd() % (unsigned)(g_n[g_chnode].p[0] + 1)); }
                    g_na++;
                }
                for (j = 0; j < m; j++) { g_ca[g_na].kind = K_ARM; g_ca[g_na].node = g_dets[rnd() % (unsigned)g_ndet]; g_ca[g_na].t = ta; g_na++; }
                if (rnd() & 1) { g_ca[g_na].kind = K_DRAIN; g_ca[g_na].idx = drains++; g_na++; }
            } else if (r < 120) {
                g_ca[g_na].kind = K_DRAIN; g_ca[g_na].idx = drains++; g_na++;
            } else if (r < 125) {
                g_ca[g_na].kind = K_RESET; g_ca[g_na].t = t; g_ca[g_na].idx = drains++; g_na++;
            } else if (r < 140 && !off('s')) {
                g_ca[g_na].kind = K_SETA; g_ca[g_na].x = (rndf() - 0.5) * 90; g_na++;
            } else if (r < 150 && !off('c')) {
                g_ca[g_na].kind = K_SETCH; g_ca[g_na].dev = (int)(rnd() % (unsigned)(g_n[g_chnode].p[0] + 1)); g_na++;
            } else if (r < 180) {
                g_ca[g_na].kind = K_SETTLE; g_ca[g_na].t = t; g_na++;
            } else if (r < 190 && g_rel && !off('r')) {
                g_ca[g_na].kind = K_RECENTER; g_ca[g_na].t = t - (int64_t)(rnd() % 5) * MS; g_ca[g_na].x = (rndf() - 0.5) * 10; g_ca[g_na].y = (rndf() - 0.5) * 10;
                if (g_ca[g_na].t < 0) g_ca[g_na].t = 0;
                g_na++;
            }
        }
        g_ca[g_na].kind = K_FRAME; g_ca[g_na].t = onset; g_ca[g_na].idx = 0; g_na++;
        g_ca[g_na].kind = K_DRAIN; g_ca[g_na].idx = drains++; g_na++;
        for (k = 0; k < g_na; k++) g_ca[k].ai = k;
        /* the shapes this case has */
        {
            int64_t mx = INT64_MIN, ti;
            int prev_ctl = 0;
            for (k = 0; k < g_na; k++) {
                const call* c = &g_ca[k];
                if (is_input(c, &ti)) {
                    if (c->kind == K_FEED && ti < mx) {
                        for (j = k + 1; j < g_na && j < k + 40; j++) if (g_ca[j].kind == K_ARM && g_ca[j].t >= ti && g_ca[j].t < mx) f[F1] = 1;
                    }
                    if (ti > mx) mx = ti;
                    prev_ctl = 0;
                } else if (c->kind == K_ARM) {
                    const ysig_node* n = &g_n[c->node];
                    if (c->t < mx && (n->p[YSIG_DETECT_AT] == 1 || n->in[0] == 4 || n->in[0] == g_chnode)) f[F2] = 1;
                    if (c->t < mx && prev_ctl) f[F3] = 1;
                    if (c->t > mx) f[F4] = 1;
                } else if (c->kind == K_SETLV || c->kind == K_DISARM || c->kind == K_RECENTER || c->kind == K_SETCH) {
                    prev_ctl = 1;
                    if (c->kind == K_SETCH) f[F5] = 1;
                }
            }
        }
        if (!open_g(&ga, g_ar_a, g_ch_a, 1) || !open_g(&gb, g_ar_b, g_ch_b, 0)) return 2;
        memset(&g_rows_a, 0, sizeof g_rows_a);
        memset(&g_rows_b, 0, sizeof g_rows_b);
        memset(g_ch_a, 0, sizeof g_ch_a);
        memset(g_ch_b, 0, sizeof g_ch_b);
        g_ha.n = g_hb.n = 0;
        g_der_a = g_der_b = 0;
        g_nder_a = g_nder_b = 0;
        g_rows = &g_rows_a;
        ysig_reset(&ga, 0);
        for (k = 0; k < g_na; k++) {
            int64_t cu = ga.st.arm_caught_up;
            if (g_ca[k].kind == K_ARM && ga.n_hit > 0 && ga.st.arm_caught_up >= 0) f[F6] = 1;
            do_call(&ga, &g_ca[k], &g_ha, &g_der_a, &g_nder_a);
            (void)cu;
        }
        build_b();   /* after A: B needs the hits A handed out */
        g_rows = &g_rows_b;
        ysig_reset(&gb, 0);
        for (k = 0; k < g_nb; k++) do_call(&gb, &g_cb[k], &g_hb, &g_der_b, &g_nder_b);
        ysig_get_stats(&ga, &sa);
        if (sa.arm_rows_stale || sa.arm_derived_stale || g_rows_a.h != g_rows_b.h || g_der_a != g_der_b) f[F7] = 1;
        if (sa.arm_past_far || sa.arm_past_full) {
            /* fell back: counted, not compared */
            back++;
            for (w = 0; w < NF; w++) if (f[w]) { g_n_case[w]++; g_n_back[w]++; }
            continue;
        }
        why = same_state(&ga, &gb);
        if (!why) {
            /* B's hits are all in A's; A's others are counted: retracted
             * when no later past arm is before it, else superseded */
            static char used[4096];
            int ia, ib;
            long must_retract = 0, extra = 0;
            memset(used, 0, sizeof used);
            for (ib = 0; ib < g_hb.n && !why; ib++) {
                /* the copy A handed out last: an earlier equal one was the
                 * earlier arm's, before a re-arm handed it out again */
                for (ia = g_ha.n - 1; ia >= 0; ia--)
                    if (!used[ia] && !cmp_hit(&g_ha.h[ia], &g_hb.h[ib])) { used[ia] = 1; break; }
                if (ia < 0) why = "a hit B has, A not";
            }
            for (ia = 0; ia < g_ha.n && !why; ia++) {
                int before = 0;
                if (used[ia]) continue;
                extra++;
                /* the drain that handed it out, then the arms after it */
                for (k = 0; k < g_na; k++) if ((g_ca[k].kind == K_DRAIN || g_ca[k].kind == K_RESET) && g_ca[k].idx == g_ha.at[ia]) break;
                for (; k < g_na; k++) if (g_ca[k].kind == K_ARM && g_ca[k].t < g_ha.h[ia].t) before = 1;
                if (!before) must_retract++;
            }
            if (!why && extra != sa.arm_hits_retracted + sa.arm_hits_superseded) why = "extra hits != retracted + superseded";
            /* at least those: counting more as retracted is on the safe side */
            if (!why && sa.arm_hits_retracted < must_retract) why = "a hit taken at or before an arm counted superseded";
            if (!why && (g_rows_a.h != g_rows_b.h || g_rows_a.n != g_rows_b.n) && !sa.arm_rows_stale) why = "rows changed, not counted";
            if (!why && (g_der_a != g_der_b || g_nder_a != g_nder_b) && !sa.arm_derived_stale) why = "derived events changed, not counted";
        }
        for (w = 0; w < NF; w++) if (f[w]) { g_n_case[w]++; if (why) g_n_wrong[w]++; }
        if (why) {
            for (w = 0; w < nwhy && strcmp(whyname[w], why); w++) { }
            if (w == nwhy && nwhy < 32) { whyname[nwhy] = why; whys[nwhy++] = 0; }
            if (w < 32) whys[w]++;
            wrong++;
            if (getenv("ORACLE_CASE") && atol(getenv("ORACLE_CASE")) == it) {
                int q;
                for (q = 0; q < g_na; q++) printf("  A%3d kind %d node %d dev %d t %.3f\n", q, g_ca[q].kind, g_ca[q].node, g_ca[q].dev, g_ca[q].t / 1e6);
                for (q = 0; q < g_nb; q++) if (g_cb[q].kind == K_ARM) printf("  B arm at %d: node %d t %.3f\n", q, g_cb[q].node, g_cb[q].t / 1e6);
                if (g_bad >= 0 && g_n[g_bad].op == YSIG_OP_DETECT) {
                    const ysig__det_st* da = (const ysig__det_st*)ga.nd[g_bad].st;
                    const ysig__det_st* db = (const ysig__det_st*)gb.nd[g_bad].st;
                    printf("  A det: t_arm %.3f t_start %.3f t_hit %.3f armed %d ready %d holding %d state %d fired %d\n", da->t_arm / 1e6, da->t_start / 1e6, da->t_hit / 1e6, da->armed, da->ready, da->holding, da->state, da->fired);
                    printf("  B det: t_arm %.3f t_start %.3f t_hit %.3f armed %d ready %d holding %d state %d fired %d\n", db->t_arm / 1e6, db->t_start / 1e6, db->t_hit / 1e6, db->armed, db->ready, db->holding, db->state, db->fired);
                    printf("  det params: test %g lane %g hold %g at %g start %g rep %g entry %g out %g in %d\n", g_n[g_bad].p[0], g_n[g_bad].p[1], g_n[g_bad].p[4], g_n[g_bad].p[5], g_n[g_bad].p[6], g_n[g_bad].p[7], g_n[g_bad].p[12], g_n[g_bad].p[13], g_n[g_bad].in[0]);
                }
            }
            if (show && wrong <= show) {
                printf("case %ld wrong: %s node %d op %d (A %d calls, B %d; rel %d; shapes", it, why, g_bad, g_bad >= 0 ? g_n[g_bad].op : -1, g_na, g_nb, g_rel);
                for (w = 1; w < NF; w++) if (f[w]) printf(" %d", w);
                printf(")\n");
            }
        }
    }
    printf("%-50s %9s %9s %9s %9s\n", "finding (shape)", "cases", "compared", "fell back", "wrong");
    for (w = 0; w < NF; w++) printf("%-50s %9ld %9ld %9ld %9ld\n", g_fname[w], g_n_case[w], g_n_case[w] - g_n_back[w], g_n_back[w], g_n_wrong[w]);
    for (w = 0; w < nwhy; w++) printf("  why: %s: %ld\n", whyname[w], whys[w]);
    (void)back;
    return wrong ? 1 : 0;
}
