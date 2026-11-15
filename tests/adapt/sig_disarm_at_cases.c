/* sig_disarm_at_cases.c - hand-derived cases for ysig_disarm_at(g, d, t)
 * (docs/sig.md at v2-late 9834173, "Late input (proposed for 0.7.0)":
 * "ysig_disarm_at()", "The cancel, by key", the key of a call, and "A
 * disarm at its time ... first meets its detect's hold up to that time").
 * The reviewer's B (the 0.6.1 header with no window) has no such call, so
 * each expected result here is derived from the text alone, in the case's
 * comment. They run against the 0.7.0 build.
 *
 *     gcc -std=c99 -O1 -Iinclude -o sig_disarm_at_cases sig_disarm_at_cases.c -lm
 * Prints "FAIL case NAME: reason" for each wrong case; exit 1 when any.
 *
 * Assumed API (from the text): bool ysig_disarm_at(ysig_graph*, int node,
 * int64_t t_ns). Every case runs with a window (catchup_frames 4); the
 * cases marked "also with no window" have no late call and must give the
 * same hits with a zeroed window (the second B, for this call only).
 *
 * Graph: S1, S2: pen sources of devices 1 and 2 (lane 0 = x). A detect is
 * GE on lane 0, level 1 (x >= 1 passes), not armed at start, no repeat,
 * hit output, with the entry and hold of the case.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define YSP_SIG_IMPLEMENTATION
#include "ysp/sig.h"

#define MS 1000000LL
static uint64_t g_arena[(8u << 20) / 8];
static ysig_graph g;
static int g_fail;
static const char* g_case;

static void bad(const char* fmt, double a, double b) {
    char m[200];
    snprintf(m, sizeof m, fmt, a, b);
    printf("FAIL case %s: %s\n", g_case, m);
    g_fail++;
}
static void open_g(ysig_node* n, int nn, int window) {
    ysig_desc d;
    memset(&d, 0, sizeof d);
    d.nodes = n; d.n_nodes = nn; d.arena = g_arena;
    if (window) { d.catchup_frames = 4; d.catchup_calls = 1024; d.catchup_writes = 8192; }
    d.arena_size = ysig_arena_size(&d);
    if (!d.arena_size || d.arena_size > sizeof g_arena || !ysig_open(&g, &d)) { printf("FAIL case %s: open\n", g_case); exit(2); }
    ysig_reset(&g, 0);
}
static ysig_node pen(int dev) {
    ysig_node n = ysig_node_of(YSIG_OP_SOURCE, -1, -1);
    n.lanes = 2; n.p[0] = YIN_KIND_PEN; n.p[1] = YIN_AXIS_POSITION; n.p[2] = dev; n.history = 256;
    return n;
}
static ysig_node det(int in, int entry, double hold) {
    ysig_node n = ysig_node_of(YSIG_OP_DETECT, in, -1);
    n.p[YSIG_DETECT_TEST] = YSIG_TEST_GE; n.p[YSIG_DETECT_LANE] = 0; n.p[YSIG_DETECT_LEVEL] = 1;
    n.p[YSIG_DETECT_ENTRY] = entry; n.p[YSIG_DETECT_HOLD] = hold;
    n.p[YSIG_DETECT_START] = 1; n.p[YSIG_DETECT_REPEAT] = 0; n.p[YSIG_DETECT_OUT] = YSIG_OUT_HIT;
    n.history = 64;
    return n;
}
static void feed(int dev, double ms, double x) {
    yin_event e;
    memset(&e, 0, sizeof e);
    e.t = (int64_t)(ms * MS); e.type = YIN_SAMPLE; e.kind = YIN_KIND_PEN; e.control = YIN_AXIS_POSITION; e.device = (uint32_t)dev;
    e.x = (float)x; e.stamp = YIN_TIER_2;
    ysig_feed(&g, &e);
}
/* the hits handed out since the last call, against (node, ms) pairs */
static void expect(const char* when, const int* node, const double* ms, int ne) {
    ysig_hit h[64];
    int n = 0, m, i;
    while ((m = ysig_detected(&g, h + n, 64 - n)) > 0) n += m;
    if (n != ne) {
        printf("FAIL case %s (%s): %d hits handed out, expected %d\n", g_case, when, n, ne);
        for (i = 0; i < n; i++) printf("    hit node %d t %.3f ms\n", h[i].node, h[i].t / 1e6);
        g_fail++;
        return;
    }
    for (i = 0; i < n; i++)
        if (h[i].node != node[i] || h[i].t != (int64_t)(ms[i] * MS)) {
            printf("FAIL case %s (%s): hit node %d at %.3f ms, expected node %d at %g\n", g_case, when, h[i].node, h[i].t / 1e6, node[i], ms[i]);
            g_fail++;
        }
}
static void expect_none(const char* when) { expect(when, NULL, NULL, 0); }
static long long voided(void) {
    ysig_stats s;
    ysig_get_stats(&g, &s);
    return (long long)(s.arm_hits_superseded + s.arm_hits_retracted);
}

int main(void) {
    ysig_node n[8];
    int w;

    /* 1. An arm before t: made, then ended at t. Detect D on S1, entry
     * arm, no hold. 0 @ 10 ms, arm(D, 10) (present: N(F) = 10), 5 @ 12
     * (passes: hit at 12, R's and handed out), 0 @ 14. disarm_at(D, 20):
     * key (20, state) is greater than every call in R that meets D (all at
     * or before 14), so it is not late: it goes at R's end and ends D's arm
     * at 20 (the hold is 0, nothing more to meet). 0 @ 22 and 5 @ 24: D is
     * not armed, no hit. Also with no window. */
    for (w = 1; w >= 0; w--) {
        g_case = w ? "before-t" : "before-t (no window)";
        n[0] = pen(1); n[1] = det(0, YSIG_ENTRY_ARM, 0);
        open_g(n, 2, w);
        feed(1, 10, 0);
        ysig_arm(&g, 1, 10 * MS);
        feed(1, 12, 5);
        { static const int nd[] = { 1 }; static const double ms[] = { 12 }; expect("after 12", nd, ms, 1); }
        feed(1, 14, 0);
        ysig_disarm_at(&g, 1, 20 * MS);
        feed(1, 22, 0);
        feed(1, 24, 5);
        expect_none("after the disarm");
    }

    /* 2. An arm later than t: cancelled. 0 @ 10, arm(D, 30): N(F) = 10, so
     * the group is ahead and waits. 0 @ 20 (not past 30, does not let it
     * in). disarm_at(D, 25): its key (25) is smaller than the arm's group
     * key (30), and the arm was made before it, so the arm is cancelled
     * (counted in arm_ahead_cancelled: it was waiting). 5 @ 35 lets in
     * nothing: no hit. Also with no window. */
    for (w = 1; w >= 0; w--) {
        ysig_stats s;
        g_case = w ? "after-t-waiting" : "after-t-waiting (no window)";
        n[0] = pen(1); n[1] = det(0, YSIG_ENTRY_ARM, 0);
        open_g(n, 2, w);
        feed(1, 10, 0);
        ysig_arm(&g, 1, 30 * MS);
        feed(1, 20, 0);
        ysig_disarm_at(&g, 1, 25 * MS);
        feed(1, 35, 5);
        expect_none("after 35");
        ysig_get_stats(&g, &s);
        if (s.arm_ahead_cancelled != 1) bad("arm_ahead_cancelled %g, expected %g", (double)s.arm_ahead_cancelled, 1);
    }

    /* 3. An arm later than t, already let in: cancelled by key (U5), a run
     * again takes it out. 0 @ 10, 5 @ 30 (D not armed: no hit). arm(D,
     * 20), entry arm: past (N(F) = 30); its place is after 0 @ 10, before
     * 5 @ 30; at its place the held value 0 fails; 5 @ 30 passes: hit at
     * 30, handed out. disarm_at(D, 15): key 15 is smaller than the group's
     * (20) and the arm was made before it: the arm is cancelled, the
     * disarm goes before it in R (a late call: R holds 5 @ 30 and the
     * group, both meeting D with greater keys), and the window runs again.
     * R has no hit; the hit at 30 handed out is counted (superseded or
     * retracted: 1 in all). */
    g_case = "after-t-let-in";
    n[0] = pen(1); n[1] = det(0, YSIG_ENTRY_ARM, 0);
    open_g(n, 2, 1);
    feed(1, 10, 0);
    feed(1, 30, 5);
    ysig_arm(&g, 1, 20 * MS);
    { static const int nd[] = { 1 }; static const double ms[] = { 30 }; expect("after the arm", nd, ms, 1); }
    ysig_disarm_at(&g, 1, 15 * MS);
    expect_none("after the disarm");
    if (voided() != 1) bad("superseded + retracted %g, expected %g", (double)voided(), 1);
    feed(1, 40, 0);
    feed(1, 45, 5);
    expect_none("after 45");

    /* 4. An arm at t, equal time (U1's reading for disarm_at). 5 @ 10
     * (D not armed). arm(D, 20), entry now: ahead (N(F) = 10), waits.
     * disarm_at(D, 20): same time, both state class; the arm's call is
     * earlier, so the arm's group key is the smaller: no cancel; in R the
     * arm comes first, made, then the disarm ends it. The disarm at 20 does
     * not let the group in (a state call lets in a group with tau earlier
     * than its time only), so the arm waits for input past 20 on its
     * footprint. 0 @ 30 lets it in: at its place (after 5 @ 10) entry now
     * on the held 5 passes: a hit at 20, handed out now; then the disarm
     * ends the arm (the disarm moves after the arm in R: the window runs
     * again). No window: the arm let in after a disarm with a greater key
     * is a late call there, made at R's end (late_off), and the disarm
     * does not end it: not run here. */
    g_case = "at-t-equal-time";
    n[0] = pen(1); n[1] = det(0, YSIG_ENTRY_NOW, 0);
    open_g(n, 2, 1);
    feed(1, 10, 5);
    ysig_arm(&g, 1, 20 * MS);
    ysig_disarm_at(&g, 1, 20 * MS);
    expect_none("after the disarm");
    feed(1, 30, 0);
    { static const int nd[] = { 1 }; static const double ms[] = { 20 }; expect("after 30", nd, ms, 1); }
    feed(1, 32, 0);
    feed(1, 34, 5);
    expect_none("after 34 (ended)");

    /* 5. Equal time, the other call order: disarm_at(D, 20), then arm(D,
     * 20). The disarm's call is earlier: its key is the smaller, so R has
     * the disarm, then the arm; the arm is made after the disarm and is
     * not ended or cancelled (a disarm cancels arms made before it only).
     * 5 @ 10, then the two calls (the arm ahead: N(F) = 10), then 0 @ 30
     * lets the arm in: entry now on the held 5 at 20: a hit at 20. The arm
     * stays armed: no repeat, so no second hit at 34. */
    g_case = "at-t-disarm-first";
    n[0] = pen(1); n[1] = det(0, YSIG_ENTRY_NOW, 0);
    open_g(n, 2, 1);
    feed(1, 10, 5);
    ysig_disarm_at(&g, 1, 20 * MS);
    ysig_arm(&g, 1, 20 * MS);
    feed(1, 30, 0);
    { static const int nd[] = { 1 }; static const double ms[] = { 20 }; expect("after 30", nd, ms, 1); }

    /* 6. A hold met up to t. Entry arm, hold 10 ms. 5 @ 0, arm(D, 0)
     * (present), 5 @ 4, 5 @ 8. disarm_at(D, 12): it first meets D's hold
     * up to 12 (start 0 + 10 = 10 <= 12): the arm's hit at 10, handed out
     * at this call, then the end. Then a late failing report, 0 @ 9 (a
     * second device on the same detect would be cleaner; device 1's own
     * order allows 9 after 8): it must precede the disarm (key 9 < 12,
     * meets D), so it goes before it, breaks the hold at 9, and R has no
     * hit: the one handed out is counted (1). */
    g_case = "hold-met-up-to-t";
    n[0] = pen(1); n[1] = det(0, YSIG_ENTRY_ARM, 10 * 1e-3);
    open_g(n, 2, 1);
    feed(1, 0, 5);
    ysig_arm(&g, 1, 0);
    feed(1, 4, 5);
    feed(1, 8, 5);
    expect_none("before the disarm");
    ysig_disarm_at(&g, 1, 12 * MS);
    { static const int nd[] = { 1 }; static const double ms[] = { 10 }; expect("after the disarm", nd, ms, 1); }
    feed(1, 9, 0);
    expect_none("after the late 9 ms report");
    if (voided() != 1) bad("superseded + retracted %g, expected %g", (double)voided(), 1);

    /* 7. U1, plain ysig_disarm() at N (the ruling). S1 (device 1), S2
     * (device 2), C2 a detect on S2, entry now. 5 @ 95 on device 2, 0 @
     * 100 on device 1 (N = 100). arm(C2, 100): N(F(C2)) = 95, ahead, waits.
     * ysig_disarm(C2): time N = 100; the arm's group key (100, state,
     * earlier call) is the smaller: no cancel; R has the arm, then the
     * disarm. 0 @ 105 on device 2 lets the arm in: entry now on the held 5
     * at 100: a hit at 100; then the disarm ends it. 5 @ 110: no hit. */
    g_case = "U1-plain-disarm-at-N";
    n[0] = pen(1); n[1] = pen(2); n[2] = det(1, YSIG_ENTRY_NOW, 0);
    open_g(n, 3, 1);
    feed(2, 95, 5);
    feed(1, 100, 0);
    ysig_arm(&g, 2, 100 * MS);
    ysig_disarm(&g, 2);
    expect_none("after the disarm");
    feed(2, 105, 0);
    { static const int nd[] = { 2 }; static const double ms[] = { 100 }; expect("after 105", nd, ms, 1); }
    feed(2, 108, 0);
    feed(2, 110, 5);
    expect_none("after 110 (ended)");

    /* 8. U5, the closure. S1 (device 1), S2 (device 2); D1 on S1, D2 on
     * S2, both entry now; K a detect on D1's hit output (not armed). W8 =
     * [set(K, level 0.5), arm(D2, 10)]: F(W8) = D(D2) + D(K) meets F(W7) =
     * D(D1) at K, while device 2's footprint D(S2) does not meet D(D1).
     * 5 @ 5 on device 1, 0 @ 5 on device 2. W7 = arm(D1, 10), then W8:
     * both ahead (N(F) = 5). 0 @ 12 on device 2 meets W8 only: it lets W8
     * in and, right before it, W7 (meets W8 at K, smaller key: the
     * closure). At W7's place D1's entry now on the held 5 passes: a hit
     * of D1 at 10, handed out; D2's held 0 fails. disarm_at(D1, 8): key 8
     * is smaller than W7's (10) and W7's arm was made before it: cancelled
     * by key though already let in; the run again takes it out: R has no
     * D1 hit, and the one handed out is counted (1). 5 @ 15 on device 1:
     * D1 not armed, no hit. */
    g_case = "U5-closure";
    n[0] = pen(1); n[1] = pen(2); n[2] = det(0, YSIG_ENTRY_NOW, 0); n[3] = det(1, YSIG_ENTRY_NOW, 0);
    n[4] = det(2, YSIG_ENTRY_NOW, 0);
    open_g(n, 5, 1);
    feed(1, 5, 5);
    feed(2, 5, 0);
    ysig_arm(&g, 2, 10 * MS);
    ysig_set(&g, 4, YSIG_DETECT_LEVEL, 0.5);
    ysig_arm(&g, 3, 10 * MS);
    feed(2, 12, 0);
    { static const int nd[] = { 2 }; static const double ms[] = { 10 }; expect("after 12", nd, ms, 1); }
    ysig_disarm_at(&g, 2, 8 * MS);
    expect_none("after the disarm");
    if (voided() != 1) bad("superseded + retracted %g, expected %g", (double)voided(), 1);
    feed(1, 15, 5);
    expect_none("after 15");

    /* 9. A waiting arm before t is let in by the disarm. 5 @ 10. arm(D,
     * 15), entry now: ahead (N(F) = 10). disarm_at(D, 20): a state call of
     * time 20, later than 15, that meets D: it lets the group in (rule 3),
     * right before itself; the arm is made at 15 (entry now on the held 5:
     * a hit at 15, handed out at this call), then ended at 20. 5 @ 25: no
     * hit. Also with no window. */
    for (w = 1; w >= 0; w--) {
        g_case = w ? "before-t-waiting" : "before-t-waiting (no window)";
        n[0] = pen(1); n[1] = det(0, YSIG_ENTRY_NOW, 0);
        open_g(n, 2, w);
        feed(1, 10, 5);
        ysig_arm(&g, 1, 15 * MS);
        ysig_disarm_at(&g, 1, 20 * MS);
        { static const int nd[] = { 1 }; static const double ms[] = { 15 }; expect("after the disarm", nd, ms, 1); }
        feed(1, 22, 0);
        feed(1, 25, 5);
        expect_none("after 25");
    }

    if (g_fail) { printf("sig_disarm_at_cases: %d failed\n", g_fail); return 1; }
    printf("sig_disarm_at_cases ok: 0 failed\n");
    return 0;
}
