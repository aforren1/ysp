/* sig_arm_spec_cases.c - unit cases for what sig_arm_ref.c cannot test:
 * the arm's own action at its place in R (the old hold met up to tau, the
 * entry rules, the fall and the rise) and the propagation of that change
 * through D (docs/sig.md, "An arm in the past: the semantics"). B in the
 * oracle runs the same no-window code, so these expected values are
 * derived by hand from the spec, case by case (the derivation is in each
 * case's comment).
 *
 *     gcc -std=c99 -O1 -Iinclude [-DSIG_ARM_REF_SPEC=1] -o sig_arm_spec_cases sig_arm_spec_cases.c -lm
 * Prints "FAIL case NAME: reason" for each wrong case; exit 1 when any.
 * SIG_ARM_REF_SPEC=1 adds the checks on ysig_flush(), arm_past_pinned and
 * arm_ahead_cancelled (a header with them), and cases 18 to 21 from the
 * amended text (b6676f4): a disarm meets its hold; no-window and no-room
 * fallbacks of ahead groups.
 *
 * 0.7.0 (v2-late 9834173, "Late input"): the expectations are the 0.7.0
 * ones by default; -DSIG_SPEC_V06 keeps the 0.6.x ones. Each case says
 * what the 0.7.0 text changes for it ("0.7.0:"). Only case 15 changes its
 * expected results; 15b is new.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define YSP_SIG_IMPLEMENTATION
#include "ysp/sig.h"

#ifndef SIG_ARM_REF_SPEC
#define SIG_ARM_REF_SPEC 0
#endif
#ifdef SIG_SPEC_V06
#define SPEC07 0
#else
#define SPEC07 1
#endif

#define MS 1000000LL
static uint64_t g_arena[(4u << 20) / 8];
static float g_ch[8];
static ysig_graph g;
static int g_fail;
static const char* g_case;

typedef struct { int node; double v0, v1; int64_t t; } rrow;
static rrow g_rows[256];
static int g_nrows;
static void on_row(void* u, const ysig_row* r) {
    (void)u;
    if (g_nrows < 256) { g_rows[g_nrows].node = r->node; g_rows[g_nrows].v0 = r->value.v[0]; g_rows[g_nrows].v1 = r->value.v[1]; g_rows[g_nrows].t = r->value.t; g_nrows++; }
}

typedef struct { double s; } cust_st;
static void cust_reset(void* st, const double* p, int64_t t) { (void)p; (void)t; ((cust_st*)st)->s = 0; }
static void cust_sample(void* st, const double* p, const ysig_value* in, int n_in, ysig_value* out) {
    (void)p; (void)n_in;
    ((cust_st*)st)->s += in[0].v[0];
    out->v[0] = ((cust_st*)st)->s;
}
static const ysig_op_vtable g_cust = { YSIG_OP_ABI, "runsum", { 0, 0, 0, 0 }, 0, sizeof(cust_st), NULL, 0, cust_reset, cust_sample, NULL };
static const ysig_op_vtable* const g_custs[1] = { &g_cust };

static void bad(const char* fmt, double a, double b) {
    char m[200];
    snprintf(m, sizeof m, fmt, a, b);
    printf("FAIL case %s: %s\n", g_case, m);
    g_fail++;
}
static void open_g(ysig_node* n, int nn, int window) {
    ysig_desc d;
    memset(&d, 0, sizeof d);
    d.nodes = n; d.n_nodes = nn; d.channels = g_ch; d.n_channels = 8; d.arena = g_arena;
    d.on_row = on_row; d.custom = g_custs; d.n_custom = 1;
    d.catchup_frames = window;
    d.arena_size = ysig_arena_size(&d);
    if (!d.arena_size || d.arena_size > sizeof g_arena || !ysig_open(&g, &d)) { printf("FAIL case %s: open\n", g_case); exit(2); }
    g_nrows = 0;
    ysig_reset(&g, 0);
}
static ysig_node pen(int dev) {
    ysig_node n = ysig_node_of(YSIG_OP_SOURCE, -1, -1);
    n.lanes = 2; n.p[0] = YIN_KIND_PEN; n.p[1] = YIN_AXIS_POSITION; n.p[2] = dev; n.history = 256;
    return n;
}
static ysig_node det(int in, double level, int entry, double hold, int start, int repeat) {
    ysig_node n = ysig_node_of(YSIG_OP_DETECT, in, -1);
    n.p[YSIG_DETECT_LANE] = 0; n.p[YSIG_DETECT_LEVEL] = level; n.p[YSIG_DETECT_ENTRY] = entry; n.p[YSIG_DETECT_HOLD] = hold;
    n.p[YSIG_DETECT_START] = start; n.p[YSIG_DETECT_REPEAT] = repeat; n.p[YSIG_DETECT_OUT] = YSIG_OUT_HIT;
    return n;
}
static void feed(int64_t t, double x, int dev, int ctl) {
    yin_event e;
    memset(&e, 0, sizeof e);
    e.t = t; e.type = YIN_SAMPLE; e.kind = YIN_KIND_PEN; e.control = (uint32_t)ctl; e.device = (uint32_t)dev; e.x = (float)x; e.value = (float)x;
    e.stamp = YIN_TIER_2;
    ysig_feed(&g, &e);
}
static void pen_at(int64_t t, double x) { feed(t, x, 1, YIN_AXIS_POSITION); }
static void frame(int64_t onset, int64_t poll) {
    ysig_frame_in f;
    memset(&f, 0, sizeof f);
    f.index = onset / MS; f.onset_ns = onset; f.poll_ns = poll; f.has_poll = true;
    ysig_frame(&g, &f);
}
static int hits(ysig_hit* h, int cap) {
    int n = 0, m;
    while (n < cap && (m = ysig_detected(&g, h + n, cap - n)) > 0) n += m;
    return n;
}
/* the hits handed out, against the expected times (ms) */
static void expect_hits(const ysig_hit* h, int n, const double* ms, int ne) {
    int i;
    if (n != ne) { bad("%g hits handed out, expected %g", n, ne); for (i = 0; i < n; i++) printf("    hit node %d t %.3f ms\n", h[i].node, h[i].t / 1e6); return; }
    for (i = 0; i < n; i++) if (h[i].t != (int64_t)(ms[i] * 1e6)) bad("a hit at %g ms, expected %g", h[i].t / 1e6, ms[i]);
}

int main(void) {
    ysig_hit h[64];
    ysig_node n[8];
    int k;
    ysig_stats s;

    /* 1. The old hold met up to tau, a detect on a view. speed(pen) <= 1,
     * hold 50 ms, armed at 1 ms (holding: still). Samples to 45 ms, frames
     * 16, 33, 50 (polls 15, 32, 45), a sample at 55 ms finds the hit at 51.
     * The caller takes it and re-arms at 51 ms. At the arm's place (after
     * the frame at 50, before the 55 ms sample) the arm first meets the
     * old hold up to 51: the earlier arm's hit at 51, the one handed out
     * (not again: R's hit, matched). The latch on the hit output froze at
     * its rise (x 0) and stays frozen when the hand moves.
     * 0.7.0: the samples at 16 and 33 ms, fed after the frames with those
     * onsets, are late (at one time a feed comes before a frame) and go
     * before them; the frames' views read input up to the polls (15, 32),
     * so nothing a frame reads changes. The expected results stay. */
    g_case = "hold-met-on-view";
    n[0] = pen(0);
    n[1] = ysig_node_of(YSIG_OP_SPEED, 0, -1); n[1].p[0] = 0.004;
    n[2] = det(1, 1, 0, 0.05, 1, 0); n[2].p[YSIG_DETECT_TEST] = YSIG_TEST_LE;
    n[3] = ysig_node_of(YSIG_OP_LATCH, 0, 2);
    open_g(n, 4, 4);
    for (k = 1; k <= 45; k++) {
        pen_at(k * MS, 0);
        if (k == 1) ysig_arm(&g, 2, 1 * MS);
        if (k == 15) frame(16 * MS, 15 * MS);
        if (k == 32) frame(33 * MS, 32 * MS);
    }
    frame(50 * MS, 45 * MS);
    pen_at(55 * MS, 0);
    { double e[] = { 51 }; expect_hits(h, hits(h, 64), e, 1); }
    ysig_arm(&g, 2, 51 * MS);
    pen_at(70 * MS, 20);
    { int m = hits(h, 64); if (m) bad("%g hits after the re-arm, expected %g", m, 0); }
    if (ysig_value_of(&g, 3)->v[0] != 0) bad("latch x %g, expected %g (frozen at the earlier arm's hit)", ysig_value_of(&g, 3)->v[0], 0);
    ysig_get_stats(&g, &s);
    if (s.arm_hits_retracted || s.arm_hits_superseded) bad("retracted %g superseded %g, expected 0 and 0", (double)s.arm_hits_retracted, (double)s.arm_hits_superseded);

    /* 2. The same at frame rate: x >= 5 tested at onsets, hold 50 ms from
     * the onset at 10 ms; onsets 10, 26.7, 43.4, 60.1: the hit at 60.0,
     * found at 60.1. Re-armed at 60.0: met at the arm's place, the latch
     * (x 10) stays frozen. */
    g_case = "hold-met-at-frame-rate";
    n[0] = pen(0);
    n[1] = det(0, 5, 0, 0.05, 1, 0); n[1].p[YSIG_DETECT_AT] = 1;
    n[2] = ysig_node_of(YSIG_OP_LATCH, 0, 1);
    open_g(n, 3, 4);
    pen_at(1 * MS, 10);
    ysig_arm(&g, 1, 10 * MS);
    {
        int64_t o;
        int got = 0;
        for (o = 10 * MS; o <= 70 * MS; o += 16 * MS + 700000) {
            frame(o, o);
            if (hits(h, 64) && !got) { got = 1; if (h[0].t != 60 * MS) bad("hit at %g ms, expected %g", h[0].t / 1e6, 60); ysig_arm(&g, 1, h[0].t); }
        }
        if (!got) bad("%g hits, expected %g", 0, 1);
    }
    pen_at(80 * MS, 20);
    if (ysig_value_of(&g, 2)->v[0] != 10) bad("latch x %g, expected %g", ysig_value_of(&g, 2)->v[0], 10);

    /* 3. Entry now at a past arm: x >= 5, entry now, armed by ysig_arm.
     * Samples at 10 (x 10), frame 16, 20 (x 10); then arm at 12 ms. Its
     * place: after the 10 ms sample (at or before tau, meets), before the
     * frame at 16. Its test on the held value (x 10) is true: an entry-now
     * hit at 12 ms with the 10 ms sample's value. Then the 20 ms sample:
     * held, no hit. */
    g_case = "entry-now-past";
    n[0] = pen(0);
    n[1] = det(0, 5, YSIG_ENTRY_NOW, 0, 1, 0);
    open_g(n, 2, 4);
    pen_at(10 * MS, 10); frame(16 * MS, 10 * MS); pen_at(20 * MS, 10);
    ysig_arm(&g, 1, 12 * MS);
    { double e[] = { 12 }; int m = hits(h, 64); expect_hits(h, m, e, 1); if (m == 1 && h[0].value.t != 10 * MS) bad("its value at %g ms, expected %g", h[0].value.t / 1e6, 10); }

    /* 4. Entry arm with a hold, past: hold 5 ms; x 10 at 10, 14, 18, 22;
     * arm at 12 (made after 22). Holding from 12: the 14 ms sample is
     * before 17; the 18 ms sample finds the hit at 17. */
    g_case = "entry-arm-hold-past";
    n[0] = pen(0);
    n[1] = det(0, 5, YSIG_ENTRY_ARM, 0.005, 1, 0);
    open_g(n, 2, 4);
    pen_at(10 * MS, 10); pen_at(14 * MS, 10); frame(16 * MS, 14 * MS); pen_at(18 * MS, 10); pen_at(22 * MS, 10);
    ysig_arm(&g, 1, 12 * MS);
    { double e[] = { 17 }; int m = hits(h, 64); expect_hits(h, m, e, 1); if (m == 1 && h[0].t_found != 18 * MS) bad("found at %g ms, expected %g", h[0].t_found / 1e6, 18); }

    /* 5. Entry fresh: the same, then out at 25 and in at 30, 36: true at
     * the arm waits for a fresh entry; the hold starts at 30, the 36 ms
     * sample finds the hit at 35. */
    g_case = "entry-fresh-past";
    n[0] = pen(0);
    n[1] = det(0, 5, YSIG_ENTRY_FRESH, 0.005, 1, 0);
    open_g(n, 2, 4);
    pen_at(10 * MS, 10); pen_at(14 * MS, 10); frame(16 * MS, 14 * MS); pen_at(18 * MS, 10); pen_at(22 * MS, 10);
    ysig_arm(&g, 1, 12 * MS);
    pen_at(25 * MS, 0); pen_at(30 * MS, 10); pen_at(36 * MS, 10);
    { double e[] = { 35 }; expect_hits(h, hits(h, 64), e, 1); }

    /* 6. D: a fall reaches a record. x >= 5 armed at reset, one hit, a
     * record of its output per sample. 5 ms x 10: the hit, row (1, 5);
     * 8 ms x 10: row (1, 8) (a standing hit's later evaluation at its own
     * time); a present arm at 8 ms: the fall, row (0, 8); its test is true
     * (entry arm, no hold): held. 10 ms x 10: row (0, 10). */
    g_case = "fall-reaches-record";
    n[0] = pen(0);
    n[1] = det(0, 5, YSIG_ENTRY_ARM, 0, 0, 0);
    n[2] = ysig_node_of(YSIG_OP_RECORD, 1, -1);
    open_g(n, 3, 4);
    pen_at(5 * MS, 10); pen_at(8 * MS, 10);
    ysig_arm(&g, 1, 8 * MS);
    pen_at(10 * MS, 10);
    {
        static const double v[] = { 1, 1, 0, 0 };
        static const int64_t t[] = { 5 * MS, 8 * MS, 8 * MS, 10 * MS };
        int j, m = 0;
        for (j = 0; j < g_nrows; j++) if (g_rows[j].node == 2) {
            if (m < 4 && (g_rows[j].v0 != v[m] || g_rows[j].t != t[m])) bad("row value %g at %g ms differs", g_rows[j].v0, g_rows[j].t / 1e6);
            m++;
        }
        if (m != 4) bad("%g rows, expected %g", m, 4);
    }

    /* 7. D: a rise reaches a latch through delay(0). x >= 5, entry now,
     * armed by ysig_arm. 5 ms x 10; a present arm at 5: the entry-now hit,
     * the rise at 5 ms; delay(0) takes it as a sample at 5, the latch on it
     * freezes on its input's held value (x 10). 8 ms x 20: stays 10. */
    g_case = "rise-through-delay-to-latch";
    n[0] = pen(0);
    n[1] = det(0, 5, YSIG_ENTRY_NOW, 0, 1, 0);
    n[2] = ysig_node_of(YSIG_OP_DELAY, 1, -1);
    n[3] = ysig_node_of(YSIG_OP_LATCH, 0, 2);
    open_g(n, 4, 4);
    pen_at(5 * MS, 10);
    ysig_arm(&g, 1, 5 * MS);
    pen_at(8 * MS, 20);
    if (ysig_value_of(&g, 3)->v[0] != 10) bad("latch x %g, expected %g", ysig_value_of(&g, 3)->v[0], 10);

    /* 8. D: a custom op's sample() runs on the rise. runsum(hit output):
     * 1 ms x 10 with the detect disarmed: + 0; a present arm at 1, entry
     * now: the rise, + 1; 2 ms x 10: the hit stands, + 1: 2. */
    g_case = "rise-runs-custom-op";
    n[0] = pen(0);
    n[1] = det(0, 5, YSIG_ENTRY_NOW, 0, 1, 0);
    n[2] = ysig_node_of(YSIG_OP_CUSTOM, 1, -1);
    open_g(n, 3, 4);
    pen_at(1 * MS, 10);
    ysig_arm(&g, 1, 1 * MS);
    pen_at(2 * MS, 10);
    if (ysig_value_of(&g, 2)->v[0] != 2) bad("custom op %g, expected %g", ysig_value_of(&g, 2)->v[0], 2);

    /* 9. D: an emit emits the rise. emit(hit output): 1 ms x 10 (0 at 1),
     * a present arm at 1, entry now: the rise (1 at 1). */
    g_case = "rise-emits";
    n[0] = pen(0);
    n[1] = det(0, 5, YSIG_ENTRY_NOW, 0, 1, 0);
    n[2] = ysig_node_of(YSIG_OP_EMIT, 1, -1);
    open_g(n, 3, 4);
    pen_at(1 * MS, 10);
    ysig_arm(&g, 1, 1 * MS);
    {
        yin_event e[16];
        int m = ysig_derived(&g, e, 16);
        if (m != 2 || e[0].x != 0 || e[1].x != 1 || e[1].t != 1 * MS) bad("%g derived events (last x %g), expected 2: 0 then 1 at 1 ms", m, m ? e[m - 1].x : -1);
    }

    /* 10. D: a detect tests a rise and a fall at one arm (the named
     * exception removed). D1 x >= 5 entry now, one hit; D2 on D1's output
     * >= 0.5, every crossing, armed at reset. 1 ms x 10; present arm at 1:
     * rise, D2 hit at 1. 3 ms x 10; present arm at 3: the fall (D2 ready
     * again), then the rise (D2 hit at 3). */
    g_case = "detect-on-hit-output";
    n[0] = pen(0);
    n[1] = det(0, 5, YSIG_ENTRY_NOW, 0, 1, 0);
    n[2] = det(1, 0.5, YSIG_ENTRY_ARM, 0, 0, 1);
    open_g(n, 3, 4);
    pen_at(1 * MS, 10);
    ysig_arm(&g, 1, 1 * MS);
    pen_at(3 * MS, 10);
    ysig_arm(&g, 1, 3 * MS);
    {
        int m = hits(h, 64), j, c2 = 0;
        int64_t t2[4];
        for (j = 0; j < m; j++) if (h[j].node == 2 && c2 < 4) t2[c2++] = h[j].t;
        if (c2 != 2 || t2[0] != 1 * MS || t2[1] != 3 * MS) bad("D2: %g hits (first at %g ms), expected 2, at 1 and 3", c2, c2 ? t2[0] / 1e6 : -1);
    }

    /* 11. An ahead arm waits. x >= 5 entry now. 1 ms x 10; arm at 10 (ahead
     * of N = 1): nothing yet. 5 ms x 10 (not past 10): still waiting. 12 ms
     * x 10: the arm goes in before it, tests the held value (the 5 ms
     * sample): a hit at 10 with that value; the 12 ms sample: held. */
    g_case = "ahead-arm-waits";
    n[0] = pen(0);
    n[1] = det(0, 5, YSIG_ENTRY_NOW, 0, 1, 0);
    open_g(n, 2, 4);
    pen_at(1 * MS, 10);
    ysig_arm(&g, 1, 10 * MS);
    if (hits(h, 64)) bad("%g hits at the ahead arm's call, expected %g", 1, 0);
    pen_at(5 * MS, 10);
    if (hits(h, 64)) bad("%g hits after input before tau, expected %g", 1, 0);
    pen_at(12 * MS, 10);
    { double e[] = { 10 }; int m = hits(h, 64); expect_hits(h, m, e, 1); if (m == 1 && h[0].value.t != 5 * MS) bad("its value at %g ms, expected %g", h[0].value.t / 1e6, 5); }

    /* 12. E: arm at 60 (ahead), frames 16, 33, crossings at 41 and 50
     * (every crossing, entry now), then arm at 40. R: frames, arm 40 (x 0:
     * ready), 41 (hit), 45 (ready), 50 (hit), arm 60 before the 70 ms
     * input (entry now, held x 10: a hit at 60). */
    g_case = "ahead-then-earlier-arm";
    n[0] = pen(0);
    n[1] = det(0, 5, YSIG_ENTRY_NOW, 0, 1, 1);
    open_g(n, 2, 4);
    pen_at(1 * MS, 0);
    ysig_arm(&g, 1, 60 * MS);
    frame(16 * MS, 15 * MS); frame(33 * MS, 32 * MS);
    pen_at(41 * MS, 10); pen_at(45 * MS, 0); pen_at(50 * MS, 10);
    ysig_arm(&g, 1, 40 * MS);
    pen_at(70 * MS, 10);
    { double e[] = { 41, 50, 60 }; expect_hits(h, hits(h, 64), e, 3); }

    /* 13. A disarm cancels a waiting arm: arm at 50 (ahead of N = 1), a
     * sample at 5, disarm (time 5 < 50): cancelled. 60 ms x 10: no hit.
     * 0.7.0: the cancel is by key; the plain disarm's time is N = 5 (the
     * ruling), smaller than the arm's 50: cancelled. Unchanged. */
    g_case = "disarm-cancels-waiting";
    n[0] = pen(0);
    n[1] = det(0, 5, YSIG_ENTRY_ARM, 0, 1, 0);
    open_g(n, 2, 4);
    pen_at(1 * MS, 0);
    ysig_arm(&g, 1, 50 * MS);
    pen_at(5 * MS, 0);
    ysig_disarm(&g, 1);
    pen_at(60 * MS, 10);
    if (hits(h, 64)) bad("%g hits, expected %g", 1, 0);
#if SIG_ARM_REF_SPEC
    ysig_get_stats(&g, &s);
    if (s.arm_ahead_cancelled != 1) bad("arm_ahead_cancelled %g, expected %g", (double)s.arm_ahead_cancelled, 1);

    /* 14. A barrier cancels a waiting arm: arm at 50, ysig_flush(), 60 ms
     * x 10: no hit, one cancelled. */
    g_case = "flush-cancels-waiting";
    n[0] = pen(0);
    n[1] = det(0, 5, YSIG_ENTRY_ARM, 0, 1, 0);
    open_g(n, 2, 4);
    pen_at(1 * MS, 0);
    ysig_arm(&g, 1, 50 * MS);
    ysig_flush(&g);
    pen_at(60 * MS, 10);
    if (hits(h, 64)) bad("%g hits, expected %g", 1, 0);
    ysig_get_stats(&g, &s);
    if (s.arm_ahead_cancelled != 1) bad("arm_ahead_cancelled %g, expected %g", (double)s.arm_ahead_cancelled, 1);

    /* 15. Pinned: a detect on an any-device pen; 1 ms dev 1 x 0, frame 16,
     * 30 ms dev 1 x 0, a late report of dev 2 at 15; arm at 20: its place
     * is after the late report (meets), after the 30 ms input (meets): one
     * arm_past_pinned.
     * 0.7.0 (changed): the dev 2 report at 15 is late (R holds the frame
     * at 16 and the 30 ms input, both meeting, with greater keys) and goes
     * to its place, before the frame. The arm at 20 (past: N(F) = 30) goes
     * after the frame and before the 30 ms input: not pinned
     * (arm_past_pinned 0). Every x is 0 < 5: no hit. */
    g_case = "pinned";
    n[0] = pen(0);
    n[1] = det(0, 5, YSIG_ENTRY_ARM, 0, 1, 0);
    open_g(n, 2, 4);
    feed(1 * MS, 0, 1, YIN_AXIS_POSITION); frame(16 * MS, 15 * MS); feed(30 * MS, 0, 1, YIN_AXIS_POSITION); feed(15 * MS, 0, 2, YIN_AXIS_POSITION);
    ysig_arm(&g, 1, 20 * MS);
    ysig_get_stats(&g, &s);
    if (s.arm_past_pinned != (SPEC07 ? 0 : 1)) bad("arm_past_pinned %g, expected %g", (double)s.arm_past_pinned, SPEC07 ? 0 : 1);
    if (SPEC07 && hits(h, 64)) bad("%g hits, expected %g", 1, 0);

    /* 15b. The same with x 10 at 30 ms, so the order shows in a hit. 0.6.x:
     * pinned; the arm's place is after the 30 ms input, its entry test on
     * the held x 10 is true (entry arm, no hold): held, no hit. 0.7.0: the
     * arm goes before the 30 ms input; its test on the held x 0 (the 15 ms
     * report) is false: ready; the 30 ms input crosses: a hit at 30, found
     * at 30, handed out at the arm's call (the window runs again). */
    g_case = "pinned-crossing";
    n[0] = pen(0);
    n[1] = det(0, 5, YSIG_ENTRY_ARM, 0, 1, 0);
    open_g(n, 2, 4);
    feed(1 * MS, 0, 1, YIN_AXIS_POSITION); frame(16 * MS, 15 * MS); feed(30 * MS, 10, 1, YIN_AXIS_POSITION); feed(15 * MS, 0, 2, YIN_AXIS_POSITION);
    ysig_arm(&g, 1, 20 * MS);
    if (SPEC07) { double e[] = { 30 }; expect_hits(h, hits(h, 64), e, 1); }
    else if (hits(h, 64)) bad("%g hits, expected %g", 1, 0);
    ysig_get_stats(&g, &s);
    if (s.arm_past_pinned != (SPEC07 ? 0 : 1)) bad("arm_past_pinned %g, expected %g", (double)s.arm_past_pinned, SPEC07 ? 0 : 1);
#endif

    /* 16. Not pinned: the late report is on input the detect does not
     * read (pen pressure, device 3). 1 ms x 0 (pen dev 1), frame 16, 30 ms
     * x 10, pressure at 15; arm at 20: its place is after the frame at 16
     * (the pressure report passes), before 30 ms: the crossing at 30 is
     * the new arm's hit.
     * 0.7.0: the pressure report at 15 is late against the frame at 16 (a
     * frame meets every footprint) and goes before it; the arm's place and
     * the hit at 30 stay. */
    g_case = "late-report-elsewhere";
    n[0] = pen(1);
    n[1] = det(0, 5, YSIG_ENTRY_ARM, 0, 1, 0);
    n[2] = ysig_node_of(YSIG_OP_SOURCE, -1, -1);
    n[2].lanes = 1; n[2].p[0] = YIN_KIND_PEN; n[2].p[1] = YIN_AXIS_PEN_PRESSURE; n[2].p[2] = 3;
    open_g(n, 3, 4);
    feed(1 * MS, 0, 1, YIN_AXIS_POSITION); frame(16 * MS, 15 * MS); feed(30 * MS, 10, 1, YIN_AXIS_POSITION); feed(15 * MS, 1, 3, YIN_AXIS_PEN_PRESSURE);
    ysig_arm(&g, 1, 20 * MS);
    { double e[] = { 30 }; expect_hits(h, hits(h, 64), e, 1); }
#if SIG_ARM_REF_SPEC
    ysig_get_stats(&g, &s);
    if (s.arm_past_pinned) bad("arm_past_pinned %g, expected %g", (double)s.arm_past_pinned, 0);
#endif

    /* 17. A group moves back with its arm: level 5; 10 ms x 3, frame 16,
     * 20 ms x 5; then ysig_set(level 4) and arm at 12 (one group, past).
     * Its place: after the 10 ms sample. With level 4 the held x 3 is
     * false: ready; the 20 ms sample crosses: a hit at 20. */
    g_case = "group-set-then-arm";
    n[0] = pen(0);
    n[1] = det(0, 5, YSIG_ENTRY_ARM, 0, 1, 0);
    open_g(n, 2, 4);
    pen_at(10 * MS, 3); frame(16 * MS, 10 * MS); pen_at(20 * MS, 5);
    ysig_set(&g, 1, YSIG_DETECT_LEVEL, 4);
    ysig_arm(&g, 1, 12 * MS);
    { double e[] = { 20 }; expect_hits(h, hits(h, 64), e, 1); }

#if SIG_ARM_REF_SPEC
    /* b6676f4: the cases the amended text adds. */

    /* 18. A disarm first meets its detect's hold up to its time. Hold 10 ms
     * from 1 ms (x 10); input of another device at 12 ms makes N 12; a
     * disarm in no group (time N = 12): the hold ended at 11, the earlier
     * arm's hit, then the arm ends.
     * 0.7.0: unchanged by the coordinator's ruling (a plain disarm keeps N,
     * 12). With N(F(c)) (1 ms: only device 1 reaches the detect) the hold
     * would not be met: no hit. */
    g_case = "disarm-meets-hold";
    n[0] = pen(1);
    n[1] = det(0, 5, YSIG_ENTRY_ARM, 0.01, 1, 0);
    n[2] = ysig_node_of(YSIG_OP_SOURCE, -1, -1);
    n[2].lanes = 1; n[2].p[0] = YIN_KIND_PEN; n[2].p[1] = YIN_AXIS_PEN_PRESSURE; n[2].p[2] = 3;
    open_g(n, 3, 4);
    feed(1 * MS, 10, 1, YIN_AXIS_POSITION);
    ysig_arm(&g, 1, 1 * MS);
    feed(12 * MS, 1, 3, YIN_AXIS_PEN_PRESSURE);
    ysig_disarm(&g, 1);
    feed(13 * MS, 10, 3, YIN_AXIS_PEN_PRESSURE);   /* ends the gap */
    { double e[] = { 11 }; expect_hits(h, hits(h, 64), e, 1); }

    /* 19. The same disarm in a past group (with ysig_arm of another detect
     * at 8 ms): its time is 8 ms, the hold (ends 11) is not met: no hit.
     * With a window and without (a no-window past group is made at its
     * call positions; the disarm's time is still its group's).
     * 0.7.0: the group (past: N(F) = 12 through the pressure source) goes
     * before the 12 ms input by key; with no window that is a late call,
     * made at R's end and counted in late_off. The disarm's time is 8 in
     * both: no hit, unchanged. */
    {
        int K;
        for (K = 4; K >= 0; K -= 4) {
            g_case = K ? "disarm-in-past-group-window" : "disarm-in-past-group-no-window";
            n[0] = pen(1);
            n[1] = det(0, 5, YSIG_ENTRY_ARM, 0.01, 1, 0);
            n[2] = ysig_node_of(YSIG_OP_SOURCE, -1, -1);
            n[2].lanes = 1; n[2].p[0] = YIN_KIND_PEN; n[2].p[1] = YIN_AXIS_PEN_PRESSURE; n[2].p[2] = 3;
            n[3] = det(2, 5, YSIG_ENTRY_ARM, 0, 1, 0);
            open_g(n, 4, K);
            feed(1 * MS, 10, 1, YIN_AXIS_POSITION);
            ysig_arm(&g, 1, 1 * MS);
            feed(12 * MS, 1, 3, YIN_AXIS_PEN_PRESSURE);
            ysig_disarm(&g, 1);
            ysig_arm(&g, 3, 8 * MS);
            { int m = hits(h, 64); if (m) bad("%g hits, expected %g (the hold ends at 11 ms, after the group's 8 ms)", m, 0); }
        }
    }

    /* 20. Without a window an ahead group's state calls stay where made and
     * only the arm waits, counted in arm_past_off. Level 5; 10 ms x 4.5;
     * set level 4 and arm at 15 (entry now; ahead of N = 10): the set is
     * made now, the arm waits; 12 ms x 4.5 (not past 15); 20 ms x 4.5 lets
     * the arm in: its test on the held value (x 4.5 >= 4): a hit at 15. */
    g_case = "no-window-ahead-group";
    n[0] = pen(0);
    n[1] = det(0, 5, YSIG_ENTRY_NOW, 0, 1, 0);
    open_g(n, 2, 0);
    pen_at(10 * MS, 4.5);
    ysig_set(&g, 1, YSIG_DETECT_LEVEL, 4);
    ysig_arm(&g, 1, 15 * MS);
    ysig_get_stats(&g, &s);
    if (s.arm_past_off != 1) bad("arm_past_off %g, expected %g", (double)s.arm_past_off, 1);
    pen_at(12 * MS, 4.5);
    if (hits(h, 64)) bad("%g hits before input past 15 ms, expected %g", 1, 0);
    pen_at(20 * MS, 4.5);
    { double e[] = { 15 }; expect_hits(h, hits(h, 64), e, 1); }

    /* 21. No room: at most 32 arms wait. 33 ahead arms of one detect (taus
     * 20..52 ms, N = 1): the 33rd is made at once on the input known,
     * counted in arm_past_full. */
    g_case = "ahead-no-room";
    n[0] = pen(0);
    n[1] = det(0, 5, YSIG_ENTRY_ARM, 0, 1, 1);
    open_g(n, 2, 4);
    pen_at(1 * MS, 0);
    for (k = 0; k < 33; k++) ysig_arm(&g, 1, (20 + k) * MS);
    ysig_get_stats(&g, &s);
    if (s.arm_past_full != 1) bad("arm_past_full %g, expected %g", (double)s.arm_past_full, 1);
#endif

    printf("%s: %d failed\n", g_fail ? "sig_arm_spec_cases" : "sig_arm_spec_cases ok", g_fail);
    return g_fail ? 1 : 0;
}
