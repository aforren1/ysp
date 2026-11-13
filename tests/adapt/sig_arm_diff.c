/* sig_arm_diff.c - randomized differential for ysp/sig.h's arm in the past
 * (v0.6.0, AN ARM IN THE PAST). Returns 0 when no case is wrong.
 *
 * A makes an arm at time t after more calls have fed input later than t;
 * B makes the same arm in call order, before the first input later than t
 * (after the last reset). From A's arm on, after every call, A and B must
 * agree exactly: every node's value, ring and state, the stats (but the
 * window's own counts), and every hit handed out. An arm that the window
 * cannot run again falls back by name (ysig_stats.arm_past_far, _full);
 * a hit the caller already had that the run again no longer finds is
 * counted: the armed detect's own at or after the arm's time as
 * superseded (arm_hits_superseded: its earlier arm's), any other as
 * retracted (arm_hits_retracted). Those cases are counted, not compared.
 *
 * The cases cover the review's list: two-source nodes (a sum of L and R
 * with late reports), one source with two devices out of order, equal
 * times, out-of-order samples a source drops, a view input (speed),
 * ysig_set() and ysig_recenter() inside the window, ysig_reset() and
 * ysig_reset_trial() anywhere, re-arms earlier than a found hit, entry
 * arm, fresh and now, hold 0 and above, repeat once and every,
 * hysteresis, lane, norm and rectangle tests, ysig_settle(), and users
 * of the detect's output (latch, peak, mux, all, delay).
 *
 *     gcc -std=c99 -O1 -Iinclude -o sig_arm_diff tests/adapt/sig_arm_diff.c -lm
 *     ./sig_arm_diff [cases] [seed]
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define YSP_SIG_IMPLEMENTATION
#include "ysp/sig.h"

#define MS 1000000LL
#define MAXC 400
#define NCAT 19

static uint64_t g_arena_a[(32u << 20) / 8], g_arena_b[(32u << 20) / 8];
static float g_ch_a[4], g_ch_b[4];
static uint64_t g_rs;
static uint32_t rnd(void) { g_rs ^= g_rs << 13; g_rs ^= g_rs >> 7; g_rs ^= g_rs << 17; return (uint32_t)(g_rs >> 11); }
static double rndf(void) { return (rnd() & 0xffffff) / (double)0x1000000; }

enum { K_FEED, K_FRAME, K_SET, K_RECENTER, K_RESET, K_RESET_TRIAL, K_ARM, K_DISARM, K_SETTLE, K_NOP };
typedef struct { int kind, dev, node, param; int64_t t; double x, y; } call;

static const char* g_cat[NCAT] = {
    "all cases", "detect on a two-source sum", "one source, two devices out of order", "equal-time samples",
    "out-of-order samples dropped", "detect on a view (speed)", "ysig_set in the window", "ysig_recenter in the window",
    "a reset before B's place", "a re-arm earlier than a found hit", "entry arm", "entry fresh", "entry now",
    "hold above 0", "repeat every", "hysteresis", "norm or rectangle test", "ysig_settle in the window",
    "an arm at the old hold's end" };
static long g_n[NCAT], g_exact[NCAT], g_back[NCAT], g_sup[NCAT], g_retr[NCAT], g_wrong[NCAT];

/* the graph: 13 experiment nodes, or a role and an adapter for recenter */
typedef struct { ysig_node n[16]; int nn, det, adapter; ysig_node ad[4]; ysig_role role; ysig_adapter adp; } graph_def;

static int open_graph(ysig_graph* g, const graph_def* gd, uint64_t* arena, float* ch) {
    ysig_desc d;
    memset(&d, 0, sizeof d);
    d.nodes = gd->n;
    d.n_nodes = gd->nn;
    d.channels = ch;
    d.n_channels = 4;
    d.arena = arena;
    d.catchup_frames = 8;
    d.catchup_calls = 2048;
    d.catchup_writes = 1 << 16;
    if (gd->adapter) {
        d.roles = &gd->role;
        d.n_roles = 1;
        d.adapters = &gd->adp;
        d.n_adapters = 1;
    }
    d.arena_size = ysig_arena_size(&d);
    if (!d.arena_size || d.arena_size > sizeof g_arena_a) {
        char e[256];
        ysig_check(&d, e, sizeof e);
        fprintf(stderr, "sig_arm_diff: check: %s\n", e);
        return 0;
    }
    if (!ysig_open(g, &d)) { fprintf(stderr, "sig_arm_diff: open: %s\n", ysig_error(g)); return 0; }
    return 1;
}

static void detect_params(ysig_node* n, int* cats) {
    int test = (int)(rnd() % 4);
    n->p[YSIG_DETECT_TEST] = test;
    n->p[YSIG_DETECT_LANE] = test >= YSIG_TEST_IN_RECT ? -1 : (int)(rnd() % 3) - 1;
    n->p[YSIG_DETECT_LEVEL] = 2 + rnd() % 6;
    n->p[YSIG_DETECT_A_X] = -3; n->p[YSIG_DETECT_A_Y] = -3; n->p[YSIG_DETECT_B_X] = 3; n->p[YSIG_DETECT_B_Y] = 3;
    if (n->p[YSIG_DETECT_LANE] < 0 && test < YSIG_TEST_IN_RECT) { n->p[YSIG_DETECT_A_X] = 0; n->p[YSIG_DETECT_A_Y] = 0; }
    n->p[YSIG_DETECT_HYSTERESIS] = (rnd() & 1) ? 1 : 0;
    n->p[YSIG_DETECT_HOLD] = (rnd() % 3) ? (rnd() % 8) * 0.001 : 0;
    n->p[YSIG_DETECT_START] = (rnd() % 4) ? 1 : 0;
    n->p[YSIG_DETECT_REPEAT] = rnd() & 1;
    n->p[YSIG_DETECT_ENTRY] = rnd() % 3;
    n->p[YSIG_DETECT_OUT] = (rnd() % 3) ? YSIG_OUT_HIT : YSIG_OUT_TEST;
    n->history = 64;
    cats[10 + (int)n->p[YSIG_DETECT_ENTRY]] = 1;
    if (n->p[YSIG_DETECT_HOLD] > 0) cats[13] = 1;
    if (n->p[YSIG_DETECT_REPEAT] > 0) cats[14] = 1;
    if (n->p[YSIG_DETECT_HYSTERESIS] > 0) cats[15] = 1;
    if (n->p[YSIG_DETECT_LANE] < 0) cats[16] = 1;
}

static ysig_node pen(int dev, int hist) {
    ysig_node n = ysig_node_of(YSIG_OP_SOURCE, -1, -1);
    n.lanes = 2; n.p[0] = YIN_KIND_PEN; n.p[1] = YIN_AXIS_POSITION; n.p[2] = dev; n.history = hist;
    return n;
}

static void make_graph(graph_def* gd, int* cats) {
    int in;
    memset(gd, 0, sizeof *gd);
    if (rnd() % 5 == 0) {
        /* a relative role: recenter moves its integrator */
        memset(&gd->role, 0, sizeof gd->role);
        gd->role.name = "hand"; gd->role.type = YSIG_TYPE_POSITION2D; gd->role.units = YSIG_UNITS_DISPLAY_PX; gd->role.allow_relative = true;
        gd->ad[0] = ysig_node_of(YSIG_OP_SOURCE, -1, -1);
        gd->ad[0].lanes = 2; gd->ad[0].p[0] = YIN_KIND_MOUSE; gd->ad[0].p[1] = YIN_AXIS_DELTA; gd->ad[0].history = 256;
        gd->ad[1] = ysig_node_of(YSIG_OP_INTEGRATE, 0, -1);
        gd->ad[1].p[0] = gd->ad[1].p[1] = 1; gd->ad[1].history = 256;
        memset(&gd->adp, 0, sizeof gd->adp);
        gd->adp.role = "hand"; gd->adp.name = "mouse"; gd->adp.type = YSIG_TYPE_POSITION2D_RELATIVE; gd->adp.units = YSIG_UNITS_DISPLAY_PX;
        gd->adp.nodes = gd->ad; gd->adp.n_nodes = 2;
        gd->adapter = 1;
        gd->n[0] = ysig_node_of(YSIG_OP_ROLE, -1, -1); gd->n[0].history = 256;
        gd->n[1] = ysig_node_of(YSIG_OP_DETECT, 0, -1);
        detect_params(&gd->n[1], cats);
        gd->n[2] = ysig_node_of(YSIG_OP_LATCH, 0, 1); gd->n[2].history = 64;
        gd->n[3] = ysig_node_of(YSIG_OP_PEAK, 0, 1); gd->n[3].history = 64;
        gd->nn = 4;
        gd->det = 1;
        return;
    }
    gd->n[0] = pen(1, (rnd() % 4) ? 4096 : 0);
    gd->n[1] = pen(2, 4096);
    gd->n[2] = pen(0, 4096);                                   /* any device: 1 and 2 */
    gd->n[3] = ysig_node_of(YSIG_OP_SUM, 0, 1); gd->n[3].history = 4096;
    gd->n[4] = ysig_node_of(YSIG_OP_AFFINE, 0, -1); gd->n[4].p[YSIG_AFFINE_ANGLE] = 30; gd->n[4].history = 4096;
    gd->n[5] = ysig_node_of(YSIG_OP_SPEED, 2, -1); gd->n[5].p[0] = 0.004; gd->n[5].history = 4096;
    gd->n[6] = ysig_node_of(YSIG_OP_OFFSET, 0, -1); gd->n[6].history = 4096;
    switch (rnd() % 6) {
    case 0: in = 0; break;
    case 1: in = 2; cats[2] = 1; break;
    case 2: in = 3; cats[1] = 1; break;
    case 3: in = 4; break;
    case 4: in = 5; cats[5] = 1; break;
    default: in = 6; break;
    }
    gd->n[7] = ysig_node_of(YSIG_OP_DETECT, in, -1);
    detect_params(&gd->n[7], cats);
    if (in == 5 && (int)gd->n[7].p[YSIG_DETECT_TEST] >= YSIG_TEST_IN_RECT) gd->n[7].p[YSIG_DETECT_TEST] = YSIG_TEST_LE;
    if (in == 5) gd->n[7].p[YSIG_DETECT_LANE] = 0;
    gd->n[8] = ysig_node_of(YSIG_OP_LATCH, (rnd() & 1) ? 4 : in, 7); gd->n[8].history = 64;
    gd->n[9] = ysig_node_of(YSIG_OP_PEAK, 2, 7); gd->n[9].history = 64;
    gd->n[10] = ysig_node_of(YSIG_OP_MUX, 0, 1); gd->n[10].in[2] = 7; gd->n[10].history = 64;
    gd->n[11] = ysig_node_of(YSIG_OP_ALL, 7, 3); gd->n[11].history = 64;
    gd->n[12] = ysig_node_of(YSIG_OP_DELAY, 7, -1); gd->n[12].p[0] = 0.003; gd->n[12].history = 64;
    gd->nn = 13;
    gd->det = 7;
}

static void do_call(ysig_graph* g, const call* c, int adapter) {
    yin_event e;
    ysig_frame_in f;
    switch (c->kind) {
    case K_FEED:
        memset(&e, 0, sizeof e);
        e.t = c->t; e.type = YIN_SAMPLE; e.device = (uint32_t)c->dev; e.x = (float)c->x; e.y = (float)c->y; e.stamp = YIN_TIER_2;
        if (adapter) { e.kind = YIN_KIND_MOUSE; e.control = YIN_AXIS_DELTA; }
        else { e.kind = YIN_KIND_PEN; e.control = YIN_AXIS_POSITION; }
        ysig_feed(g, &e);
        break;
    case K_FRAME:
        memset(&f, 0, sizeof f);
        f.index = c->t / MS; f.onset_ns = c->t; f.poll_ns = c->t - (int64_t)c->dev * 100000; f.has_poll = true;
        ysig_frame(g, &f);
        break;
    case K_SET: ysig_set(g, c->node, c->param, c->x); break;
    case K_RECENTER: ysig_recenter(g, 0, c->x, c->y, c->t); break;
    case K_RESET: ysig_reset(g, c->t); break;
    case K_RESET_TRIAL: ysig_reset_trial(g, c->t); break;
    case K_ARM: ysig_arm(g, c->node, c->t); break;
    case K_DISARM: ysig_disarm(g, c->node); break;
    case K_SETTLE: ysig_settle(g, c->t); break;
    default: break;
    }
}

static int is_input(const call* c, int64_t* t) {
    if (c->kind == K_FEED || c->kind == K_FRAME || c->kind == K_SETTLE) { *t = c->t; return 1; }
    return 0;
}

typedef struct { ysig_hit h[1024]; int n; } hits;
static void drain(ysig_graph* g, hits* r) {
    ysig_hit h[64];
    int k, m;
    while ((m = ysig_detected(g, h, 64)) > 0)
        for (k = 0; k < m && r->n < 1024; k++) r->h[r->n++] = h[k];
}
static int cmp_hit(const void* a, const void* b) {
    const ysig_hit* x = (const ysig_hit*)a;
    const ysig_hit* y = (const ysig_hit*)b;
    if (x->t != y->t) return x->t < y->t ? -1 : 1;
    if (x->node != y->node) return x->node < y->node ? -1 : 1;
    return memcmp(&x->value, &y->value, sizeof x->value);
}

/* A and B's whole state: values, rings, state blocks, stats */
static int g_dbg;
static const char* same_state(const ysig_graph* a, const ysig_graph* b) {
    int i, k;
    if (g_dbg)
        for (i = 0; i < a->n; i++)
            if (memcmp(&a->val[i], &b->val[i], sizeof(ysig_value)))
                printf("  node %d op %d: A (%g,%g) t %lld, B (%g,%g) t %lld\n", i, a->nd[i].op, a->val[i].v[0], a->val[i].v[1],
                       (long long)a->val[i].t, b->val[i].v[0], b->val[i].v[1], (long long)b->val[i].t);
    ysig_stats sa = a->st, sb = b->st;
    for (i = 0; i < a->n; i++) {
        const ysig__nd* na = &a->nd[i];
        const ysig__nd* nb = &b->nd[i];
        size_t sz = ysig__st_bytes(na);
        if (memcmp(&a->val[i], &b->val[i], sizeof(ysig_value))) return "a node's value";
        if (na->hist.n != nb->hist.n) return "a ring's length";
        for (k = 0; k < na->hist.n; k++)
            if (memcmp(ysig__ring_at(&na->hist, k), ysig__ring_at(&nb->hist, k), sizeof(ysig_value))) return "a ring's entry";
        if (memcmp(na->p, nb->p, sizeof na->p)) return "a node's parameters";
        if (na->op == YSIG_OP_VELOCITY || na->op == YSIG_OP_SPEED || na->op == YSIG_OP_DELAY) {
            const ysig__ring* ra = (const ysig__ring*)na->st;
            const ysig__ring* rb = (const ysig__ring*)nb->st;
            if (ra->n != rb->n) return "an op ring's length";
            for (k = 0; k < ra->n; k++)
                if (memcmp(ysig__ring_at(ra, k), ysig__ring_at(rb, k), sizeof(ysig_value))) return "an op ring's entry";
        } else if (sz && memcmp(na->st, nb->st, sz)) {
            return "a node's state";
        }
    }
    /* the window's own counts differ by design (A ran again, B did not) */
    sa.arm_caught_up = sb.arm_caught_up = 0;
    sa.arm_past_off = sb.arm_past_off = 0;
    sa.arm_past_far = sb.arm_past_far = 0;
    sa.arm_past_full = sb.arm_past_full = 0;
    sa.arm_hits_retracted = sb.arm_hits_retracted = 0;
    sa.arm_hits_superseded = sb.arm_hits_superseded = 0;
    sa.arm_rows_stale = sb.arm_rows_stale = 0;
    sa.arm_derived_stale = sb.arm_derived_stale = 0;
    if (memcmp(&sa, &sb, sizeof sa)) return "the stats";
    if (a->t_cover != b->t_cover) return "t_cover";
    return NULL;
}

int main(int argc, char** argv) {
    long cases = argc > 1 ? atol(argv[1]) : getenv("SIG_ARM_CASES") ? atol(getenv("SIG_ARM_CASES")) : 3000, it;
    long shown = 0;
    int c;
    g_rs = argc > 2 ? strtoull(argv[2], NULL, 10) : getenv("SIG_ARM_SEED") ? strtoull(getenv("SIG_ARM_SEED"), NULL, 10) : 88172645463325252ull;
    g_dbg = argc > 3 ? atoi(argv[3]) : 0;
    if (!g_rs) g_rs = 1;
    for (it = 0; it < cases; it++) {
        static call cs[MAXC + 1];
        static graph_def gd;
        static ysig_graph ga, gb;
        static hits ha, hb;
        int cats[NCAT];
        int nc = 0, k, a_pos, b_pos, last_reset = -1, ties = rnd() % 3 == 0;
        int64_t t = 0, onset = 16 * MS, last[3] = { 0, 0, 0 }, t_arm, t_reset = 0;
        double x = 0, y = 0;
        const char* why = NULL;
        long sup = 0;
        ysig_stats s0, s1;
        memset(&s0, 0, sizeof s0);
        memset(&s1, 0, sizeof s1);
        memset(cats, 0, sizeof cats);
        cats[0] = 1;
        make_graph(&gd, cats);
        /* the calls */
        {
            int nfeed = 30 + (int)(rnd() % 200);
            while (nc < MAXC - 8 && nfeed-- > 0) {
                int r = (int)(rnd() % 100);
                t += ties && rnd() % 3 == 0 ? 0 : (int64_t)(1 + rnd() % 3000) * 1000;
                while (t > onset && nc < MAXC - 8) {
                    cs[nc].kind = K_FRAME; cs[nc].t = onset; cs[nc].dev = (int)(rnd() % 3); nc++;
                    onset += 16 * MS + (int64_t)(rnd() % 3) * 1000;
                }
                if (rnd() % 4 == 0) { x = (rndf() - 0.5) * 16; y = (rndf() - 0.5) * 16; }
                else { x += (rndf() - 0.5) * 3; y += (rndf() - 0.5) * 3; }
                cs[nc].kind = K_FEED;
                cs[nc].dev = gd.adapter ? 1 : 1 + (int)(rnd() % 2);
                cs[nc].t = t;
                cs[nc].x = gd.adapter ? (rndf() - 0.5) * 6 : x;
                cs[nc].y = gd.adapter ? (rndf() - 0.5) * 6 : y;
                if (r < 8 && !gd.adapter) {
                    /* an out-of-order sample: older than its device's last */
                    cs[nc].t = last[cs[nc].dev] - (int64_t)(rnd() % 4000) * 1000;
                } else if (r < 16 && !gd.adapter) {
                    /* a late report of the other device: after its own last */
                    int o = 3 - cs[nc].dev;
                    cs[nc].dev = o;
                    cs[nc].t = last[o] + (t - last[o]) / 2;
                }
                if (cs[nc].t > last[cs[nc].dev]) last[cs[nc].dev] = cs[nc].t;
                nc++;
                r = (int)(rnd() % 1000);
                if (r < 25 && !gd.adapter) {
                    cs[nc].kind = K_SET; cs[nc].node = 6; cs[nc].param = 0; cs[nc].x = (rndf() - 0.5) * 10; nc++;
                } else if (r < 45 && gd.adapter) {
                    cs[nc].kind = K_RECENTER; cs[nc].t = t; cs[nc].x = (rndf() - 0.5) * 10; cs[nc].y = (rndf() - 0.5) * 10; nc++;
                } else if (r < 55) {
                    cs[nc].kind = (rnd() & 1) ? K_RESET : K_RESET_TRIAL; cs[nc].t = t; nc++;
                } else if (r < 115) {
                    /* the detect armed live, sometimes in the past itself */
                    cs[nc].kind = K_ARM; cs[nc].node = gd.det; cs[nc].t = t - (int64_t)(rnd() % 4) * 2 * MS; nc++;
                } else if (r < 125) {
                    cs[nc].kind = K_DISARM; cs[nc].node = gd.det; nc++;
                } else if (r < 160) {
                    cs[nc].kind = K_SETTLE; cs[nc].t = t; nc++;
                }
            }
            cs[nc].kind = K_FRAME; cs[nc].t = onset; cs[nc].dev = 0; nc++;
        }
        /* A's call position, then the arm's time and B's position */
        a_pos = 1 + (int)(rnd() % (unsigned)nc);
        for (k = 0; k < a_pos; k++)
            if (cs[k].kind == K_RESET || cs[k].kind == K_RESET_TRIAL) { last_reset = k; t_reset = cs[k].t; }
        {
            int64_t tmax = t_reset, ti;
            for (k = last_reset + 1; k < a_pos; k++) if (is_input(&cs[k], &ti) && ti > tmax) tmax = ti;
            t_arm = t_reset + (int64_t)(rndf() * (double)(tmax - t_reset));
            if (rnd() % 4 == 0 && a_pos > last_reset + 1) {
                int j = last_reset + 1 + (int)(rnd() % (unsigned)(a_pos - last_reset - 1));
                if (is_input(&cs[j], &ti)) t_arm = ti;   /* exactly an input's time */
            } else if (rnd() % 3 == 0) {
                /* exactly the end of the hold the detect has at A's call
                 * (met or not yet found), or of the hit it found: the arm
                 * completes the old hold first (the player's re-arm at the
                 * time of an exit tied with that hold) */
                const ysig__det_st* ds;
                const ysig__nd* dn;
                if (!open_graph(&gb, &gd, g_arena_b, g_ch_b)) return 2;
                ysig_reset(&gb, 0);
                for (k = 0; k < a_pos; k++) do_call(&gb, &cs[k], gd.adapter);
                dn = &gb.nd[gd.det];
                ds = (const ysig__det_st*)dn->st;
                if (ds->armed && ds->holding && dn->kns[0] > 0 && ds->t_start + dn->kns[0] >= t_reset) { t_arm = ds->t_start + dn->kns[0]; cats[18] = 1; }
                else if (ds->fired && dn->kns[0] > 0 && ds->t_hit >= t_reset) { t_arm = ds->t_hit; cats[18] = 1; }
            }
        }
        /* B: after the last call before A's that must come first (input at
         * or before t, a set, a disarm, a recenter or an arm at or before
         * t), the rule of v0.6.0 (AN ARM IN THE PAST) */
        for (b_pos = a_pos; b_pos > last_reset + 1; b_pos--) {
            const call* q = &cs[b_pos - 1];
            int64_t ti;
            if (is_input(q, &ti)) { if (ti <= t_arm) break; continue; }
            if (q->kind == K_SET || q->kind == K_DISARM || q->kind == K_RESET || q->kind == K_RESET_TRIAL) break;
            if ((q->kind == K_RECENTER || q->kind == K_ARM) && q->t <= t_arm) break;
        }
        /* and after the call that found a hit at or before t that A has
         * handed out by its arm (the latest call before A's arm whose time
         * is the hit's t_found): A run to its arm on gb, draining as A */
        {
            static hits hp;
            int m;
            hp.n = 0;
            if (!open_graph(&gb, &gd, g_arena_b, g_ch_b)) return 2;
            ysig_reset(&gb, 0);
            for (k = 0; k < a_pos; k++) { do_call(&gb, &cs[k], gd.adapter); drain(&gb, &hp); }
            for (m = 0; m < hp.n; m++) {
                int64_t tc;
                if (hp.h[m].t > t_arm) continue;
                for (k = a_pos - 1; k >= b_pos; k--)
                    if ((is_input(&cs[k], &tc) || (cs[k].kind == K_ARM && (tc = cs[k].t, 1))) && tc == hp.h[m].t_found) break;
                if (k >= b_pos) b_pos = k + 1;
            }
        }
        /* another arm of the detect between B's place and A's call (one
         * later than t, or made in the past and placed by the rule itself)
         * B would not place as A did: no call here, as before */
        for (k = b_pos; k < a_pos; k++) if (cs[k].kind == K_ARM) cs[k].kind = K_NOP;
        if (last_reset >= 0) cats[8] = 1;
        /* the categories that the window between B's place and A's call reaches */
        for (k = b_pos; k < a_pos; k++) {
            int64_t ti;
            if (cs[k].kind == K_SET) cats[6] = 1;
            if (cs[k].kind == K_RECENTER) cats[7] = 1;
            if (cs[k].kind == K_SETTLE) cats[17] = 1;
            if (cs[k].kind == K_FEED && k > 0 && cs[k - 1].kind == K_FEED && cs[k - 1].t == cs[k].t) cats[3] = 1;
            if (cs[k].kind == K_FEED && is_input(&cs[k], &ti)) {
                int j;
                for (j = 0; j < k; j++) if (cs[j].kind == K_FEED && cs[j].dev == cs[k].dev && cs[j].t > cs[k].t) { cats[4] = 1; break; }
            }
        }
        /* run A and B */
        if (!open_graph(&ga, &gd, g_arena_a, g_ch_a) || !open_graph(&gb, &gd, g_arena_b, g_ch_b)) return 2;
        ha.n = hb.n = 0;
        ysig_reset(&ga, 0);
        ysig_reset(&gb, 0);
        for (k = 0; k <= nc && !why; k++) {
            if (k == b_pos) {
                {
                    int m;
                    for (m = 0; m < hb.n; m++) hb.h[m].reserved_ = 1;   /* before B's arm */
                }
                ysig_arm(&gb, gd.det, t_arm);
                if (g_dbg > 1 && it == g_dbg - 2) {
                    const ysig__det_st* db = (const ysig__det_st*)gb.nd[gd.det].st;
                    const ysig_value* sv = &gb.val[gb.nd[gd.det].in[0]];
                    printf("  B's arm at call %d: holding %d start %lld vs dev %u tier %u; its input dev %u tier %u t %lld\n", k, db->holding,
                           (long long)db->t_start, db->v_start.device, db->v_start.tier, sv->device, sv->tier, (long long)sv->t);
                }
                drain(&gb, &hb);   /* as A drains after its arm: a reset next would clear the queue */
            }
            if (k == a_pos) {
                int m;
                /* a hit found before A's arm call, after t: a re-arm earlier than it */
                for (m = 0; m < ha.n; m++) if (ha.h[m].node == gd.det && ha.h[m].t > t_arm) cats[9] = 1;
                ysig_get_stats(&ga, &s0);
                if (g_dbg > 1 && it == g_dbg - 2) {
                    ysig__cu* cu = ga.cu;
                    uint64_t q;
                    int m2;
                    for (m2 = 0; m2 < cu->n_ck; m2++) {
                        const ysig__ck* ck = &cu->ck[(cu->ck0 + m2) % cu->K];
                        printf("  ck %d: call %llu cover %lld\n", m2, (unsigned long long)ck->call, (long long)ck->cover);
                    }
                    for (q = cu->c_tail; q < cu->c_head; q++) {
                        const ysig_entry* e2 = &cu->calls[q % (uint64_t)cu->C].e;
                        printf("  log %llu: kind %d t %lld ev.t %lld dev %u\n", (unsigned long long)q, e2->kind, (long long)e2->t, (long long)e2->event.t, e2->event.device);
                    }
                }
                /* handed out before A's arm: the earlier arm's */
                for (m = 0; m < ha.n; m++) ha.h[m].reserved_ = 1;
                ysig_arm(&ga, gd.det, t_arm);
                ysig_get_stats(&ga, &s1);
                if (g_dbg > 1 && it == g_dbg - 2) {
                    const ysig__det_st* da = (const ysig__det_st*)ga.nd[gd.det].st;
                    const ysig__det_st* db = (const ysig__det_st*)gb.nd[gd.det].st;
                    printf("  after A's arm: A armed %d holding %d start %lld vs dev %u tier %u; B armed %d holding %d start %lld vs dev %u tier %u\n",
                           da->armed, da->holding, (long long)da->t_start, da->v_start.device, da->v_start.tier,
                           db->armed, db->holding, (long long)db->t_start, db->v_start.device, db->v_start.tier);
                }
                drain(&ga, &ha);
                if (s1.arm_past_far > s0.arm_past_far || s1.arm_past_full > s0.arm_past_full) { why = "fell back"; break; }
                if (s1.arm_hits_retracted > s0.arm_hits_retracted) { why = "retracted"; break; }
                sup = (long)(s1.arm_hits_superseded - s0.arm_hits_superseded);
                why = same_state(&ga, &gb);
                if (why) break;
            }
            if (k == nc) break;
            do_call(&ga, &cs[k], gd.adapter);
            do_call(&gb, &cs[k], gd.adapter);
            drain(&ga, &ha);
            drain(&gb, &hb);
            if (k >= a_pos) {
                why = same_state(&ga, &gb);
                if (!why && k > a_pos) {
                    /* after the arm's call both hand out the same */
                    int m;
                    for (m = 0; m < 64 && !why; m++) { }
                }
            }
        }
        if (!why) {
            /* every hit handed out, as a set */
            qsort(ha.h, (size_t)ha.n, sizeof(ysig_hit), cmp_hit);
            qsort(hb.h, (size_t)hb.n, sizeof(ysig_hit), cmp_hit);
            /* A hands out what B does, and, before its arm, hits later than
             * t that the new history does not give as they were
             * (superseded: sup of them; the new arm hands out again what
             * it finds); B never had those. A hit in both: node, time and value, and where the
             * time is after the arm the value that found it */
            static char used[sizeof ha.h / sizeof ha.h[0]];
            int ia, ib, pass;
            long extra = 0;
            memset(used, 0, sizeof used);
            for (ib = 0; ib < hb.n && !why; ib++) {
                int got = -1;
                /* A's copies handed out after its arm first (a hit after t
                 * that a later logged arm made can be one A had before) */
                int passes = 2;
                for (pass = 0; pass < passes && got < 0; pass++)
                    for (ia = 0; ia < ha.n && got < 0; ia++) {
                        int p0 = ha.h[ia].reserved_ && ha.h[ia].t > t_arm;
                        if (used[ia] || p0 != pass) continue;
                        if (ha.h[ia].node == hb.h[ib].node && ha.h[ia].t == hb.h[ib].t && !memcmp(&ha.h[ia].value, &hb.h[ib].value, sizeof(ysig_value)) &&
                            (ha.h[ia].t_found == hb.h[ib].t_found || ha.h[ia].t <= t_arm))
                            got = ia;
                    }
                if (got < 0) {
                    why = "the hits handed out";
                    if (g_dbg) printf("  B's hit %d@%lld/%lld not in A\n", hb.h[ib].node, (long long)hb.h[ib].t, (long long)hb.h[ib].t_found);
                } else used[got] = 1;
            }
            for (ia = 0; ia < ha.n && !why; ia++) {
                if (used[ia]) continue;
                if (ha.h[ia].reserved_ && ha.h[ia].t > t_arm) extra++;
                else {
                    why = "the hits handed out";
                    if (g_dbg) printf("  A's hit %d@%lld/%lld not in B\n", ha.h[ia].node, (long long)ha.h[ia].t, (long long)ha.h[ia].t_found);
                }
            }
            /* superseded counts at least the extra hits (more when equal-time
             * copies of one hit pair up differently: on the safe side) */
            if (!why && extra > sup) { why = "the superseded hits"; if (g_dbg) printf("  extra %ld superseded %ld\n", extra, sup); }
            if (!why && sup > 0) why = "superseded";
        }
        for (c = 0; c < NCAT; c++) {
            if (!cats[c]) continue;
            g_n[c]++;
            if (!why) g_exact[c]++;
            else if (!strcmp(why, "fell back")) g_back[c]++;
            else if (!strcmp(why, "retracted")) g_retr[c]++;
            else if (!strcmp(why, "superseded")) g_sup[c]++;
            else g_wrong[c]++;
        }
        if (why && strcmp(why, "fell back") && strcmp(why, "retracted") && strcmp(why, "superseded") && shown++ < 5) {
            printf("case %ld wrong: %s (a_pos %d b_pos %d of %d, t_arm %lld)\n", it, why, a_pos, b_pos, nc, (long long)t_arm);
            if (g_dbg) {
                printf("  detect: test %g lane %g hold %g start %g rep %g entry %g out %g in %d; caught up %lld\n", gd.n[gd.det].p[0], gd.n[gd.det].p[1],
                       gd.n[gd.det].p[4], gd.n[gd.det].p[6], gd.n[gd.det].p[7], gd.n[gd.det].p[12], gd.n[gd.det].p[13], gd.n[gd.det].in[0],
                       (long long)(s1.arm_caught_up - s0.arm_caught_up));
                printf("  A hits:");
                for (k = 0; k < ha.n; k++) printf(" %d@%lld/%lld", ha.h[k].node, (long long)ha.h[k].t, (long long)ha.h[k].t_found);
                printf("\n  B hits:");
                for (k = 0; k < hb.n; k++) printf(" %d@%lld/%lld", hb.h[k].node, (long long)hb.h[k].t, (long long)hb.h[k].t_found);
                printf("\n");
                for (k = b_pos > 3 ? b_pos - 3 : 0; k <= a_pos && k < nc; k++)
                    printf("  %c%3d kind %d dev %d t %lld x %g\n", k == b_pos ? 'B' : k == a_pos ? 'A' : ' ', k, cs[k].kind, cs[k].dev, (long long)cs[k].t, cs[k].x);
            }
        }
    }
    printf("%-40s %9s %9s %9s %10s %9s %6s\n", "category", "cases", "exact", "fell back", "superseded", "retracted", "wrong");
    for (c = 0; c < NCAT; c++) printf("%-40s %9ld %9ld %9ld %10ld %9ld %6ld\n", g_cat[c], g_n[c], g_exact[c], g_back[c], g_sup[c], g_retr[c], g_wrong[c]);
    return g_wrong[0] ? 1 : 0;
}
