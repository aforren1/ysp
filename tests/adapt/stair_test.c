/* stair_test.c - self-checking test for ysp/stair.h. No framework: it
 * returns 0 when every check passed and 1 after printing each failure.
 *
 * Build it with small capacities so the YST_ERR_FULL path costs a few
 * hundred trials instead of a thousand:
 *
 *     gcc -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Werror -Iinclude \
 *         -DYST_MAX_TRIALS=128 -DYST_MAX_REVERSALS=32 \
 *         -o stair_test tests/adapt/stair_test.c -lm
 *
 * The traces below are derived by hand from the manual, trial by trial, not
 * captured from a run of this header. Where one is ambiguous in the v0.0
 * specification the manual now says which reading the code takes, and the
 * trace follows the manual.
 */
#ifndef YST_MAX_TRIALS
#define YST_MAX_TRIALS 128
#endif
#ifndef YST_MAX_REVERSALS
#define YST_MAX_REVERSALS 32
#endif

#define YSP_STAIR_IMPLEMENTATION
#include "ysp/stair.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------- harness */

static int g_failures = 0;

static void fail(int line, const char* what) {
    fprintf(stderr, "stair_test: FAIL at line %d: %s\n", line, what);
    g_failures++;
}

static void fail_d(int line, const char* what, double got, double want) {
    fprintf(stderr, "stair_test: FAIL at line %d: %s (got %.17g, want %.17g)\n",
            line, what, got, want);
    g_failures++;
}

static void fail_i(int line, const char* what, long got, long want) {
    fprintf(stderr, "stair_test: FAIL at line %d: %s (got %ld, want %ld)\n",
            line, what, got, want);
    g_failures++;
}

#define CHECK(cond) do { if (!(cond)) fail(__LINE__, #cond); } while (0)
#define CHECK_I(got, want) do { long g_ = (long)(got), w_ = (long)(want); \
    if (g_ != w_) fail_i(__LINE__, #got, g_, w_); } while (0)
#define CHECK_D(got, want, tol) do { double g_ = (got), w_ = (want); \
    if (!close_to(g_, w_, (tol))) fail_d(__LINE__, #got, g_, w_); } while (0)
#define CHECK_NAN(got) do { double g_ = (got); \
    if (!is_nan(g_)) fail_d(__LINE__, #got " is NaN", g_, 0.0); } while (0)

/* Written without a self-comparison so no compiler mistakes it for a typo. */
static bool is_nan(double x) { return !(x >= 0.0) && !(x < 0.0); }

/* A NaN the compiler cannot fold: yst_weighted_scale() refuses anything
 * outside (0, 1). */
static double nan_value(void) { return yst_weighted_scale(2.0); }

static bool close_to(double a, double b, double tol) {
    double d = a - b;
    if (is_nan(a) || is_nan(b)) return false;
    if (d < 0.0) d = -d;
    return d <= tol;
}

#define TOL 1e-12

/* Fill a desc with the fields nearly every case here shares. */
static void base_desc(yst_desc* d) {
    memset(d, 0, sizeof(*d));
    d->start = 10.0;
    d->steps[0] = 1.0;
    d->n_steps = 1;
    d->stop_trials = 8;
}

/* Replay a fixed response sequence, showing exactly what was proposed. */
static void replay(yst_stair* s, const int* resp, int n) {
    int i;
    for (i = 0; i < n; i++) {
        double x = yst_next(s);
        int rc = yst_update(s, x, resp[i]);
        if (rc < 0) fail(__LINE__, yst_strerror(rc));
    }
}

static void check_proposals(const yst_stair* s, const double* want, int n, double tol) {
    const yst_trial* h;
    int got = 0, i;
    h = yst_history(s, &got);
    CHECK_I(got, n);
    if (!h || got != n) return;
    for (i = 0; i < n; i++) {
        if (!close_to(h[i].proposed, want[i], tol))
            fail_d(__LINE__, "history proposed", h[i].proposed, want[i]);
    }
}

static void check_reversals(const yst_stair* s, const double* lev, const int* tr,
                            int n, double tol) {
    int i;
    CHECK_I(yst_n_reversals(s), n);
    for (i = 0; i < n; i++) {
        if (!close_to(yst_reversal_level(s, i), lev[i], tol))
            fail_d(__LINE__, "reversal level", yst_reversal_level(s, i), lev[i]);
        CHECK_I(yst_reversal_trial(s, i), tr[i]);
    }
    CHECK_NAN(yst_reversal_level(s, n));
    CHECK_I(yst_reversal_trial(s, n), -1);
}

static yst_stair g_s;   /* one shared handle: yst_open() resets it fully */

/* ------------------------------------------------------- 1-up-1-down, LIN */

static void test_updown_1_1(void) {
    static const int resp[10] = { 1, 1, 0, 0, 1, 0, 1, 1, 1, 0 };
    static const double want[10] = { 10, 9, 8, 9, 10, 9, 10, 9, 8, 7 };
    static const double rlev[5] = { 8, 10, 9, 10, 7 };
    static const int rtr[5] = { 2, 4, 5, 6, 9 };
    yst_desc d;
    const yst_trial* h;
    int n = 0;

    base_desc(&d);
    d.stop_trials = 10;
    CHECK(yst_open(&g_s, &d));
    CHECK(yst_is_open(&g_s));
    CHECK_I(strlen(yst_error(&g_s)), 0);

    replay(&g_s, resp, 10);
    check_proposals(&g_s, want, 10, TOL);
    check_reversals(&g_s, rlev, rtr, 5, TOL);
    CHECK_I(yst_n_trials(&g_s), 10);
    CHECK(yst_done(&g_s));
    CHECK_I(yst_stop_reason(&g_s), YST_STOP_TRIALS);
    CHECK_D(yst_next(&g_s), 8.0, TOL);

    /* Every trial steps under 1-up-1-down, and the marked ones turn. */
    h = yst_history(&g_s, &n);
    CHECK_I(n, 10);
    if (h) {
        int i;
        for (i = 0; i < 10; i++) CHECK(h[i].direction != 0);
        CHECK_I(h[0].reversal, 0);
        CHECK_I(h[2].reversal, 1);
        CHECK_I(h[4].reversal, 1);
        CHECK_I(h[9].reversal, 1);
        CHECK_I(h[0].direction, -1);
        CHECK_I(h[2].direction, 1);
        CHECK_I(h[0].step_index, 0);
    }

    /* Estimators over the whole track: n_steps is 1 and the initial rule is
     * off, so every reversal and every trial counts. */
    CHECK_I(yst_estimate_count(&g_s, YST_EST_REVERSALS), 5);
    CHECK_I(yst_estimate_count(&g_s, YST_EST_MEDIAN_REV), 5);
    CHECK_I(yst_estimate_count(&g_s, YST_EST_TRIALS), 10);
    CHECK_I(yst_estimate_count(&g_s, YST_EST_LAST), 1);
    CHECK_D(yst_estimate(&g_s, YST_EST_REVERSALS), 44.0 / 5.0, TOL);
    CHECK_D(yst_estimate(&g_s, YST_EST_MEDIAN_REV), 9.0, TOL);
    CHECK_D(yst_estimate(&g_s, YST_EST_TRIALS), 89.0 / 10.0, TOL);
    CHECK_D(yst_estimate(&g_s, YST_EST_LAST), 8.0, TOL);

    /* Updates after the stop are still recorded and the reason does not move. */
    CHECK(yst_update(&g_s, yst_next(&g_s), 1) >= 0);
    CHECK_I(yst_n_trials(&g_s), 11);
    CHECK(yst_done(&g_s));
    CHECK_I(yst_stop_reason(&g_s), YST_STOP_TRIALS);
}

/* ------------------------------------------------------- 1-up-2-down, LIN */

static void test_updown_1_2(void) {
    static const int resp[14] = { 1, 1, 1, 0, 1, 1, 0, 0, 1, 1, 1, 1, 0, 1 };
    static const double want[14] = { 20, 20, 18, 18, 20, 20, 18, 20,
                                     22, 22, 20, 20, 18, 20 };
    static const double rlev[5] = { 18, 20, 18, 22, 18 };
    static const int rtr[5] = { 3, 5, 6, 9, 12 };
    yst_desc d;
    int rc;

    base_desc(&d);
    d.start = 20.0;
    d.n_down = 2;
    d.steps[0] = 2.0;
    d.stop_trials = 14;
    CHECK(yst_open(&g_s, &d));

    /* The first trial only moves a counter: no step, no reversal, no stop. */
    rc = yst_update(&g_s, yst_next(&g_s), resp[0]);
    CHECK_I(rc, 0);
    replay(&g_s, resp + 1, 13);

    check_proposals(&g_s, want, 14, TOL);
    check_reversals(&g_s, rlev, rtr, 5, TOL);
    CHECK_I(yst_stop_reason(&g_s), YST_STOP_TRIALS);
    CHECK_D(yst_next(&g_s), 20.0, TOL);
    CHECK_D(yst_convergence_p(1, 2, 0.0), 0.70710678118654752, 1e-12);
}

/* ------------------------------------------------------- 1-up-3-down, LIN */

static void test_updown_1_3(void) {
    static const int resp[9] = { 1, 1, 1, 1, 0, 1, 1, 1, 0 };
    static const double want[9] = { 40, 40, 40, 36, 36, 40, 40, 40, 36 };
    static const double rlev[3] = { 36, 40, 36 };
    static const int rtr[3] = { 4, 7, 8 };
    yst_desc d;
    int rc;

    base_desc(&d);
    d.start = 40.0;
    d.n_down = 3;
    d.steps[0] = 4.0;
    d.stop_trials = 0;
    d.stop_reversals = 3;
    CHECK(yst_open(&g_s, &d));

    replay(&g_s, resp, 8);
    CHECK(!yst_done(&g_s));
    rc = yst_update(&g_s, yst_next(&g_s), resp[8]);
    CHECK_I(rc, YST_EVENT_STEP | YST_EVENT_REVERSAL | YST_EVENT_DONE);

    check_proposals(&g_s, want, 9, TOL);
    check_reversals(&g_s, rlev, rtr, 3, TOL);
    CHECK(yst_done(&g_s));
    CHECK_I(yst_stop_reason(&g_s), YST_STOP_REVERSALS);
    CHECK_D(yst_next(&g_s), 40.0, TOL);
    CHECK_D(yst_convergence_p(1, 3, 0.0), 0.79370052598409974, 1e-12);
}

/* ------------------------------------------------------- 2-up-1-down, LIN */

static void test_updown_2_1(void) {
    static const int resp[6] = { 1, 0, 0, 1, 0, 1 };
    static const double want[6] = { 10, 9, 9, 10, 9, 9 };
    static const double rlev[2] = { 9, 10 };
    static const int rtr[2] = { 2, 3 };
    yst_desc d;

    base_desc(&d);
    d.n_up = 2;
    d.stop_trials = 6;
    CHECK(yst_open(&g_s, &d));
    replay(&g_s, resp, 6);
    check_proposals(&g_s, want, 6, TOL);
    check_reversals(&g_s, rlev, rtr, 2, TOL);
    CHECK_D(yst_next(&g_s), 8.0, TOL);
    CHECK_D(yst_convergence_p(2, 1, 0.0), 0.29289321881345248, 1e-12);
}

/* -------------------------------------- step schedule and the initial rule */

static void test_schedule_and_initial_rule(void) {
    static const int resp[11] = { 1, 1, 0, 1, 1, 1, 0, 0, 1, 1, 1 };
    static const double want[11] = { 40, 32, 24, 28, 28, 28, 26, 28, 30, 30, 30 };
    static const double rlev[4] = { 24, 28, 26, 30 };
    static const int rtr[4] = { 2, 5, 6, 10 };
    static const int widx[11] = { 0, 0, 1, 1, 1, 2, 2, 2, 2, 2, 2 };
    yst_desc d;
    const yst_trial* h;
    int n = 0, i;

    base_desc(&d);
    d.start = 40.0;
    d.n_down = 3;
    d.initial_rule = true;
    d.steps[0] = 8.0; d.steps[1] = 4.0; d.steps[2] = 2.0;
    d.n_steps = 3;
    d.stop_trials = 0;
    d.stop_reversals = 4;
    CHECK(yst_open(&g_s, &d));

    replay(&g_s, resp, 11);
    check_proposals(&g_s, want, 11, TOL);
    check_reversals(&g_s, rlev, rtr, 4, TOL);
    CHECK_I(yst_stop_reason(&g_s), YST_STOP_REVERSALS);
    CHECK_D(yst_next(&g_s), 28.0, TOL);

    /* The trial that causes a reversal already steps by the new size. */
    h = yst_history(&g_s, &n);
    CHECK_I(n, 11);
    if (h) for (i = 0; i < 11; i++) CHECK_I(h[i].step_index, widx[i]);

    /* Default windows skip the schedule and, with the initial rule on, the
     * first reversal: that leaves reversals 2 and 3 and trials 5 to 10. */
    CHECK_I(yst_estimate_count(&g_s, YST_EST_REVERSALS), 2);
    CHECK_I(yst_estimate_count(&g_s, YST_EST_TRIALS), 6);
    CHECK_D(yst_estimate(&g_s, YST_EST_REVERSALS), 28.0, TOL);
    CHECK_D(yst_estimate(&g_s, YST_EST_MEDIAN_REV), 28.0, TOL);
    CHECK_D(yst_estimate(&g_s, YST_EST_TRIALS), 172.0 / 6.0, TOL);

    /* An explicit window is a plain "last N", with neither exclusion. */
    d.est_reversals = 3;
    d.est_trials = 4;
    CHECK(yst_open(&g_s, &d));
    replay(&g_s, resp, 11);
    CHECK_I(yst_estimate_count(&g_s, YST_EST_REVERSALS), 3);
    CHECK_I(yst_estimate_count(&g_s, YST_EST_TRIALS), 4);
    CHECK_D(yst_estimate(&g_s, YST_EST_REVERSALS), 84.0 / 3.0, TOL);
    CHECK_D(yst_estimate(&g_s, YST_EST_TRIALS), 118.0 / 4.0, TOL);

    /* An odd median picks a middle level rather than averaging. */
    CHECK_D(yst_estimate(&g_s, YST_EST_MEDIAN_REV), 28.0, TOL);
}

/* ----------------------------------------------- an empty estimate window */

static void test_empty_windows(void) {
    yst_desc d;
    base_desc(&d);
    d.n_steps = 2;
    d.steps[1] = 0.5;
    d.initial_rule = true;
    CHECK(yst_open(&g_s, &d));
    CHECK_I(yst_estimate_count(&g_s, YST_EST_REVERSALS), 0);
    CHECK_I(yst_estimate_count(&g_s, YST_EST_TRIALS), 0);
    CHECK_NAN(yst_estimate(&g_s, YST_EST_REVERSALS));
    CHECK_NAN(yst_estimate(&g_s, YST_EST_MEDIAN_REV));
    CHECK_NAN(yst_estimate(&g_s, YST_EST_TRIALS));
    /* YST_EST_LAST always has a value on an open handle. */
    CHECK_D(yst_estimate(&g_s, YST_EST_LAST), 10.0, TOL);
}

/* ------------------------------------------------------ LIN, LOG, DB steps */

static void test_step_types(void) {
    static const int resp[4] = { 1, 1, 0, 0 };
    double want[4];
    yst_desc d;

    /* LOG: a step adds to log10(level). */
    base_desc(&d);
    d.start = 0.5;
    d.step_type = YST_STEP_LOG;
    d.steps[0] = 0.3;
    d.stop_trials = 4;
    CHECK(yst_open(&g_s, &d));
    want[0] = 0.5;
    want[1] = 0.5 * pow(10.0, -0.3);
    want[2] = 0.5 * pow(10.0, -0.6);
    want[3] = 0.5 * pow(10.0, -0.3);
    replay(&g_s, resp, 4);
    check_proposals(&g_s, want, 4, 1e-12);
    CHECK_D(yst_next(&g_s), 0.5, 1e-12);
    CHECK_I(yst_n_reversals(&g_s), 1);
    CHECK_D(yst_reversal_level(&g_s, 0), 0.5 * pow(10.0, -0.6), 1e-12);
    /* The mean is a geometric one: log10 levels are -0.30103, -0.60103,
     * -0.90103 and -0.60103, which average to -0.60103. */
    CHECK_D(yst_estimate(&g_s, YST_EST_TRIALS), 0.5 * pow(10.0, -0.3), 1e-12);
    CHECK_D(yst_estimate(&g_s, YST_EST_REVERSALS), 0.5 * pow(10.0, -0.6), 1e-12);

    /* A shown level of 0 has no logarithm, so update() refuses it. */
    CHECK_I(yst_update(&g_s, 0.0, 1), YST_ERR_ARG);
    CHECK_I(yst_update(&g_s, -1.0, 1), YST_ERR_ARG);

    /* DB: a step adds to 20 log10(level), so 6 dB is about a factor of 2. */
    base_desc(&d);
    d.start = 1.0;
    d.step_type = YST_STEP_DB;
    d.steps[0] = 6.0;
    d.stop_trials = 3;
    CHECK(yst_open(&g_s, &d));
    want[0] = 1.0;
    want[1] = pow(10.0, -6.0 / 20.0);
    want[2] = pow(10.0, -12.0 / 20.0);
    replay(&g_s, resp, 3);
    check_proposals(&g_s, want, 3, 1e-12);
    CHECK_D(yst_next(&g_s), pow(10.0, -6.0 / 20.0), 1e-12);
    CHECK_I(yst_n_reversals(&g_s), 1);
    CHECK_D(yst_reversal_level(&g_s, 0), pow(10.0, -12.0 / 20.0), 1e-12);
    /* The dB levels are 0, -6 and -12, which average to -6 dB. */
    CHECK_D(yst_estimate(&g_s, YST_EST_TRIALS),
            pow(10.0, (0.0 - 6.0 - 12.0) / 3.0 / 20.0), 1e-12);
}

/* ---------------------------------- harder_is_up and the shown-vs-proposed */

static void test_harder_is_up_and_shown(void) {
    static const int resp[3] = { 1, 1, 0 };
    static const double want[3] = { 10, 11, 12 };
    yst_desc d;
    const yst_trial* h;
    int n = 0;

    base_desc(&d);
    d.harder_is_up = true;
    d.stop_trials = 3;
    CHECK(yst_open(&g_s, &d));
    replay(&g_s, resp, 3);
    check_proposals(&g_s, want, 3, TOL);
    CHECK_I(yst_n_reversals(&g_s), 1);
    CHECK_D(yst_reversal_level(&g_s, 0), 12.0, TOL);
    CHECK_D(yst_next(&g_s), 11.0, TOL);
    h = yst_history(&g_s, &n);
    if (h) { CHECK_I(h[0].direction, 1); CHECK_I(h[2].direction, -1); }

    /* A caller that quantizes the level: the rule steps from its own
     * proposal, the history keeps both, and the two estimators differ. */
    base_desc(&d);
    d.stop_trials = 2;
    CHECK(yst_open(&g_s, &d));
    CHECK(yst_update(&g_s, 10.4, 1) >= 0);
    CHECK_D(yst_next(&g_s), 9.0, TOL);
    CHECK(yst_update(&g_s, 9.4, 0) >= 0);
    CHECK_D(yst_next(&g_s), 10.0, TOL);
    h = yst_history(&g_s, &n);
    CHECK_I(n, 2);
    if (h) {
        CHECK_D(h[0].proposed, 10.0, TOL);
        CHECK_D(h[0].shown, 10.4, TOL);
        CHECK_D(h[1].proposed, 9.0, TOL);
        CHECK_D(h[1].shown, 9.4, TOL);
    }
    CHECK_D(yst_estimate(&g_s, YST_EST_REVERSALS), 9.0, TOL);
    CHECK_D(yst_estimate(&g_s, YST_EST_TRIALS), (10.4 + 9.4) / 2.0, TOL);
}

/* ----------------------------------------------- limits and STOP_LIMIT */

static void test_limits(void) {
    static const int wrong[3] = { 0, 0, 0 };
    static const int mixed[3] = { 0, 0, 1 };
    yst_desc d;
    const yst_trial* h;
    int n = 0, rc;

    /* Every response incorrect drives the level into max and holds it. */
    base_desc(&d);
    d.start = 5.0;
    d.min = 1.0;
    d.max = 5.0;
    d.stop_trials = 50;
    d.stop_at_limit = 3;
    CHECK(yst_open(&g_s, &d));
    rc = yst_update(&g_s, yst_next(&g_s), wrong[0]);
    CHECK_I(rc, YST_EVENT_STEP);                 /* clamped, still a step */
    CHECK_D(yst_next(&g_s), 5.0, TOL);
    CHECK(yst_update(&g_s, yst_next(&g_s), wrong[1]) >= 0);
    CHECK(!yst_done(&g_s));
    rc = yst_update(&g_s, yst_next(&g_s), wrong[2]);
    CHECK_I(rc, YST_EVENT_STEP | YST_EVENT_DONE);
    CHECK(yst_done(&g_s));
    CHECK_I(yst_stop_reason(&g_s), YST_STOP_LIMIT);
    CHECK_I(yst_n_trials(&g_s), 3);

    /* A clamped step still sets the direction, so the turn away registers. */
    CHECK(yst_open(&g_s, &d));
    replay(&g_s, mixed, 3);
    CHECK_I(yst_n_reversals(&g_s), 1);
    CHECK_D(yst_reversal_level(&g_s, 0), 5.0, TOL);
    CHECK_D(yst_next(&g_s), 4.0, TOL);

    /* desc.start outside the limits is clamped, not rejected. */
    base_desc(&d);
    d.start = 99.0;
    d.min = 1.0;
    d.max = 5.0;
    CHECK(yst_open(&g_s, &d));
    CHECK_D(yst_next(&g_s), 5.0, TOL);

    /* One zero bound leaves the limits off unless use_limits says otherwise. */
    base_desc(&d);
    d.start = 2.0;
    d.min = 0.0;
    d.max = 5.0;
    d.stop_trials = 4;
    CHECK(yst_open(&g_s, &d));
    replay(&g_s, wrong, 3);
    CHECK_D(yst_next(&g_s), 5.0, TOL);           /* 2 + 3 steps, no clamp */

    base_desc(&d);
    d.start = 2.0;
    d.min = 0.0;
    d.max = 4.0;
    d.use_limits = true;
    d.stop_trials = 4;
    CHECK(yst_open(&g_s, &d));
    replay(&g_s, wrong, 3);
    CHECK_D(yst_next(&g_s), 4.0, TOL);
    h = yst_history(&g_s, &n);
    CHECK_I(n, 3);
    if (h) CHECK_D(h[2].proposed, 4.0, TOL);
}

/* ------------------------------------------ YST_STOP_FULL, YST_ERR_FULL */

static void test_full(void) {
    yst_desc d;
    int i, rc = 0;

    /* No reversal ever happens, so only the hard ceiling can stop it. */
    base_desc(&d);
    d.start = 1000.0;
    d.n_down = 3;
    d.stop_trials = 0;
    d.stop_reversals = YST_MAX_REVERSALS;
    CHECK(yst_open(&g_s, &d));
    for (i = 0; i < YST_MAX_TRIALS; i++) {
        rc = yst_update(&g_s, yst_next(&g_s), 1);
        if (rc < 0) { fail(__LINE__, yst_strerror(rc)); break; }
    }
    CHECK_I(yst_n_reversals(&g_s), 0);
    CHECK_I(yst_n_trials(&g_s), YST_MAX_TRIALS);
    CHECK(yst_done(&g_s));
    CHECK_I(yst_stop_reason(&g_s), YST_STOP_FULL);
    CHECK(rc >= 0 && (rc & YST_EVENT_DONE) != 0);
    CHECK_I(yst_update(&g_s, yst_next(&g_s), 1), YST_ERR_FULL);
    CHECK_I(yst_n_trials(&g_s), YST_MAX_TRIALS);

    /* Reversals past YST_MAX_REVERSALS are counted but not recorded. */
    base_desc(&d);
    d.stop_trials = YST_MAX_TRIALS;
    d.stop_reversals = 0;
    CHECK(yst_open(&g_s, &d));
    for (i = 0; i < YST_MAX_TRIALS; i++)
        CHECK(yst_update(&g_s, yst_next(&g_s), i % 2) >= 0);
    CHECK(yst_n_reversals(&g_s) > YST_MAX_REVERSALS);
    CHECK_NAN(yst_reversal_level(&g_s, YST_MAX_REVERSALS));
    CHECK_I(yst_reversal_trial(&g_s, YST_MAX_REVERSALS), -1);
    CHECK_I(yst_estimate_count(&g_s, YST_EST_REVERSALS), YST_MAX_REVERSALS);
    CHECK_I(yst_stop_reason(&g_s), YST_STOP_TRIALS);
}

/* ---------------------------------------------- the weighted up/down rule */

static void test_weighted(void) {
    static const int resp[4] = { 1, 0, 1, 1 };
    static const double want[4] = { 10, 9, 12, 11 };
    static const double rlev[2] = { 9, 12 };
    static const int rtr[2] = { 1, 2 };
    yst_desc d;

    CHECK_D(yst_weighted_scale(0.75), 1.0 / 3.0, 1e-15);
    CHECK_D(yst_weighted_scale(0.5), 1.0, 1e-15);
    CHECK_NAN(yst_weighted_scale(0.0));
    CHECK_NAN(yst_weighted_scale(1.0));
    CHECK_NAN(yst_weighted_scale(-0.25));
    CHECK_NAN(yst_weighted_scale(2.0));

    CHECK_D(yst_convergence_p(1, 1, yst_weighted_scale(0.75)), 0.75, 1e-15);
    CHECK_D(yst_convergence_p(1, 1, 0.0), 0.5, 1e-15);
    CHECK_D(yst_convergence_p(0, 0, 0.0), 0.5, 1e-15);   /* 0 means 1 */
    CHECK_D(yst_convergence_p(1, 4, 0.0), 0.84089641525371454, 1e-12);
    CHECK_D(yst_convergence_p(2, 1, 0.0), 1.0 - sqrt(0.5), 1e-12);
    CHECK_NAN(yst_convergence_p(2, 2, 0.0));
    CHECK_NAN(yst_convergence_p(1, 1, -1.0));

    base_desc(&d);
    d.steps[0] = 3.0;
    d.step_down_scale = yst_weighted_scale(0.75);   /* down step is 1.0 */
    d.stop_trials = 4;
    CHECK(yst_open(&g_s, &d));
    replay(&g_s, resp, 4);
    check_proposals(&g_s, want, 4, 1e-12);
    check_reversals(&g_s, rlev, rtr, 2, 1e-12);
    CHECK_D(yst_next(&g_s), 10.0, 1e-12);

    /* harder_is_up moves the small step upward: the scale follows difficulty,
     * not the sign of the level change. */
    base_desc(&d);
    d.steps[0] = 3.0;
    d.step_down_scale = yst_weighted_scale(0.75);
    d.harder_is_up = true;
    d.stop_trials = 2;
    CHECK(yst_open(&g_s, &d));
    CHECK(yst_update(&g_s, 10.0, 1) >= 0);
    CHECK_D(yst_next(&g_s), 11.0, 1e-12);
    CHECK(yst_update(&g_s, 11.0, 0) >= 0);
    CHECK_D(yst_next(&g_s), 8.0, 1e-12);
    CHECK_D(yst_reversal_level(&g_s, 0), 11.0, 1e-12);
}

/* --------------------------------------------------------------------- ASA */

static void test_asa(void) {
    static const int resp[6] = { 1, 1, 0, 1, 0, 0 };
    double want[6];
    static const int rtr[3] = { 2, 3, 4 };
    double rlev[3];
    yst_desc d;

    want[0] = 10.0;
    want[1] = 9.75;                     /* c/m = 1/1, (1 - 0.75) = 0.25 down */
    want[2] = 9.5;
    want[3] = 9.5 + 0.375;              /* reversal: m = 2, 0.75/2 up        */
    want[4] = 9.875 - 1.0 / 12.0;       /* reversal: m = 3, 0.25/3 down      */
    want[5] = want[4] + 0.1875;         /* reversal: m = 4, 0.75/4 up        */
    rlev[0] = want[2];
    rlev[1] = want[3];
    rlev[2] = want[4];

    base_desc(&d);
    d.rule = YST_RULE_ASA;
    d.target_p = 0.75;
    d.steps[0] = 1.0;
    d.stop_trials = 6;
    CHECK(yst_open(&g_s, &d));
    replay(&g_s, resp, 6);
    check_proposals(&g_s, want, 6, 1e-12);
    check_reversals(&g_s, rlev, rtr, 3, 1e-12);
    /* ASA reports the next proposal. */
    CHECK_D(yst_estimate(&g_s, YST_EST_LAST), want[5] + 0.1875, 1e-12);
    CHECK_I(yst_stop_reason(&g_s), YST_STOP_TRIALS);

    /* harder_is_up flips the sign of every ASA move. */
    base_desc(&d);
    d.rule = YST_RULE_ASA;
    d.target_p = 0.75;
    d.steps[0] = 1.0;
    d.harder_is_up = true;
    d.stop_trials = 2;
    CHECK(yst_open(&g_s, &d));
    CHECK(yst_update(&g_s, 10.0, 1) >= 0);
    CHECK_D(yst_next(&g_s), 10.25, 1e-12);
    CHECK(yst_update(&g_s, 10.25, 0) >= 0);
    CHECK_D(yst_next(&g_s), 10.25 - 0.75, 1e-12);

    /* A LOG ASA moves log10(level). */
    base_desc(&d);
    d.rule = YST_RULE_ASA;
    d.target_p = 0.5;
    d.step_type = YST_STEP_LOG;
    d.start = 1.0;
    d.steps[0] = 0.2;
    d.stop_trials = 2;
    CHECK(yst_open(&g_s, &d));
    CHECK(yst_update(&g_s, 1.0, 1) >= 0);
    CHECK_D(yst_next(&g_s), pow(10.0, -0.1), 1e-12);
}

/* --------------------------------------------- closed handles and bad args */

static void test_closed_and_bad_args(void) {
    static yst_stair z;
    yst_desc d;
    const yst_trial* h;
    int n = 123;

    memset(&z, 0, sizeof(z));
    CHECK(!yst_is_open(&z));
    CHECK_NAN(yst_next(&z));
    CHECK_I(yst_update(&z, 1.0, 1), YST_ERR_CLOSED);
    CHECK(strstr(yst_error(&z), "not open") != NULL);   /* v0.2: update writes */
    CHECK(!yst_done(&z));
    CHECK_I(yst_stop_reason(&z), YST_STOP_NONE);
    CHECK_NAN(yst_estimate(&z, YST_EST_REVERSALS));
    CHECK_NAN(yst_estimate(&z, YST_EST_TRIALS));
    CHECK_NAN(yst_estimate(&z, YST_EST_MEDIAN_REV));
    CHECK_NAN(yst_estimate(&z, YST_EST_LAST));
    CHECK_I(yst_estimate_count(&z, YST_EST_REVERSALS), 0);
    CHECK_I(yst_estimate_count(&z, YST_EST_LAST), 0);
    CHECK_I(yst_n_trials(&z), 0);
    CHECK_I(yst_n_reversals(&z), 0);
    h = yst_history(&z, &n);
    CHECK(h == NULL);
    CHECK_I(n, 0);
    CHECK_NAN(yst_reversal_level(&z, 0));
    CHECK_I(yst_reversal_trial(&z, 0), -1);

    /* A null handle answers the same way. */
    CHECK_NAN(yst_next(NULL));
    CHECK_I(yst_update(NULL, 1.0, 1), YST_ERR_ARG);
    CHECK(!yst_done(NULL));
    CHECK(!yst_is_open(NULL));
    CHECK_I(yst_stop_reason(NULL), YST_STOP_NONE);
    CHECK_NAN(yst_estimate(NULL, YST_EST_LAST));
    CHECK_I(yst_estimate_count(NULL, YST_EST_LAST), 0);
    CHECK_I(yst_n_trials(NULL), 0);
    CHECK_I(yst_n_reversals(NULL), 0);
    CHECK(yst_history(NULL, NULL) == NULL);
    CHECK_NAN(yst_reversal_level(NULL, 0));
    CHECK_I(yst_reversal_trial(NULL, 0), -1);
    CHECK(strlen(yst_error(NULL)) > 0);
    CHECK(!yst_open(NULL, NULL));

    /* Bad arguments to update() on a good handle. */
    base_desc(&d);
    CHECK(yst_open(&g_s, &d));
    CHECK_I(yst_update(&g_s, 10.0, 2), YST_ERR_ARG);
    CHECK(strstr(yst_error(&g_s), "response") != NULL);
    CHECK_I(yst_update(&g_s, 10.0, -1), YST_ERR_ARG);
    CHECK_I(yst_update(&g_s, HUGE_VAL, 1), YST_ERR_ARG);
    CHECK_I(yst_update(&g_s, -HUGE_VAL, 0), YST_ERR_ARG);
    CHECK_I(yst_update(&g_s, nan_value(), 1), YST_ERR_ARG);
    CHECK_I(yst_n_trials(&g_s), 0);
    /* A LIN staircase may legitimately be shown zero or a negative level. */
    CHECK(yst_update(&g_s, 0.0, 1) >= 0);
    CHECK(yst_update(&g_s, -3.0, 0) >= 0);
    CHECK_I(yst_n_trials(&g_s), 2);

    CHECK_I(strcmp(yst_strerror(YST_OK), "ok"), 0);
    CHECK(strlen(yst_strerror(YST_ERR_ARG)) > 0);
    CHECK(strlen(yst_strerror(YST_ERR_CLOSED)) > 0);
    CHECK(strlen(yst_strerror(YST_ERR_FULL)) > 0);
    CHECK(strlen(yst_strerror(-99)) > 0);

    CHECK_I(yst_simulate_response(0.5, 0.4), 1);
    CHECK_I(yst_simulate_response(0.5, 0.5), 0);
    CHECK_I(yst_simulate_response(0.5, 0.6), 0);
    CHECK_I(yst_simulate_response(0.0, 0.0), 0);
    CHECK_I(yst_simulate_response(1.0, 0.999999), 1);

    /* close: safe on a zeroed handle, after a failed open and twice; a
     * closed handle answers as a never-opened one. */
    memset(&z, 0, sizeof(z));
    yst_close(&z);
    yst_close(NULL);
    CHECK(!yst_open(&z, NULL));
    yst_close(&z);
    CHECK(yst_open(&g_s, &d));
    yst_close(&g_s);
    yst_close(&g_s);
    CHECK(!yst_is_open(&g_s));
    CHECK_NAN(yst_next(&g_s));
    CHECK_I(yst_update(&g_s, 1.0, 1), YST_ERR_CLOSED);
}

/* ------------------------------------------------- every rejected desc */

static void reject(int line, const yst_desc* d, const char* what) {
    if (yst_open(&g_s, d)) {
        fail(line, what);
        return;
    }
    if (yst_is_open(&g_s)) fail(line, "handle open after a refused desc");
    if (strlen(yst_error(&g_s)) == 0) fail(line, "no message after a refused desc");
}

#define REJECT(d) reject(__LINE__, (d), "yst_open accepted a bad desc")

static void test_bad_descs(void) {
    yst_desc d;

    reject(__LINE__, NULL, "yst_open accepted a null desc");

    base_desc(&d); d.rule = (yst_rule)7;                      REJECT(&d);
    base_desc(&d); d.step_type = (yst_step_type)9;            REJECT(&d);
    base_desc(&d); d.start = HUGE_VAL;                          REJECT(&d);
    base_desc(&d); d.start = nan_value();                       REJECT(&d);
    base_desc(&d); d.n_steps = 0;                               REJECT(&d);
    base_desc(&d); d.n_steps = YST_MAX_STEPS + 1;             REJECT(&d);
    base_desc(&d); d.steps[0] = 0.0;                            REJECT(&d);
    base_desc(&d); d.steps[0] = -1.0;                           REJECT(&d);
    base_desc(&d); d.n_steps = 2; d.steps[1] = HUGE_VAL;        REJECT(&d);
    base_desc(&d); d.n_up = -1;                                 REJECT(&d);
    base_desc(&d); d.n_down = -1;                               REJECT(&d);
    base_desc(&d); d.step_down_scale = -1.0;                    REJECT(&d);
    base_desc(&d); d.step_down_scale = HUGE_VAL;                REJECT(&d);
    base_desc(&d); d.stop_reversals = -1;                       REJECT(&d);
    base_desc(&d); d.stop_at_limit = -1;                        REJECT(&d);
    base_desc(&d); d.est_reversals = -1;                        REJECT(&d);
    base_desc(&d); d.est_trials = -1;                           REJECT(&d);
    base_desc(&d); d.stop_trials = -1;                          REJECT(&d);
    base_desc(&d); d.stop_trials = 0;                           REJECT(&d);
    base_desc(&d); d.stop_trials = 0;
                   d.stop_reversals = YST_MAX_REVERSALS + 1;  REJECT(&d);
    base_desc(&d); d.stop_trials = YST_MAX_TRIALS + 1;        REJECT(&d);
    base_desc(&d); d.min = 5.0; d.max = 1.0;                    REJECT(&d);
    base_desc(&d); d.use_limits = true; d.min = HUGE_VAL;       REJECT(&d);
    base_desc(&d); d.use_limits = true; d.max = HUGE_VAL;       REJECT(&d);

    base_desc(&d); d.step_type = YST_STEP_LOG; d.start = 0.0; REJECT(&d);
    base_desc(&d); d.step_type = YST_STEP_LOG; d.start = -1.0; REJECT(&d);
    base_desc(&d); d.step_type = YST_STEP_DB; d.start = 0.0;  REJECT(&d);
    base_desc(&d); d.step_type = YST_STEP_LOG; d.start = 1.0;
                   d.use_limits = true; d.max = 2.0;            REJECT(&d);
    base_desc(&d); d.step_type = YST_STEP_DB; d.start = 1.0;
                   d.min = -2.0; d.max = 2.0;                   REJECT(&d);

    base_desc(&d); d.rule = YST_RULE_ASA;                     REJECT(&d);
    base_desc(&d); d.rule = YST_RULE_ASA; d.target_p = 0.0;   REJECT(&d);
    base_desc(&d); d.rule = YST_RULE_ASA; d.target_p = 1.0;   REJECT(&d);
    base_desc(&d); d.rule = YST_RULE_ASA; d.target_p = -0.5;  REJECT(&d);
    base_desc(&d); d.rule = YST_RULE_ASA; d.target_p = 0.75;
                   d.n_steps = 2; d.steps[1] = 0.5;             REJECT(&d);
    base_desc(&d); d.rule = YST_RULE_ASA; d.target_p = 0.75;
                   d.step_down_scale = 0.5;                     REJECT(&d);

    /* And the same base desc, unmodified, is accepted. */
    base_desc(&d);
    CHECK(yst_open(&g_s, &d));
    CHECK(yst_is_open(&g_s));
}

/* ----------------------------------------- a simulated Weibull observer */

/* splitmix64, so the whole run is reproducible from one seed and nothing
 * here depends on the platform's rand(). */
static uint64_t g_rng = 0x853C49E6748FEA9BULL;

static double next_uniform(void) {
    uint64_t z;
    g_rng += 0x9E3779B97F4A7C15ULL;
    z = g_rng;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    z = z ^ (z >> 31);
    return (double)(z >> 11) * (1.0 / 9007199254740992.0);
}

/* 2AFC Weibull: guessing at 0.5, threshold scale 1, slope 3. */
#define WB_ALPHA 1.0
#define WB_BETA  3.0
#define WB_GAMMA 0.5

static double weibull_p(double x) {
    if (x <= 0.0) return WB_GAMMA;
    return WB_GAMMA + (1.0 - WB_GAMMA) * (1.0 - exp(-pow(x / WB_ALPHA, WB_BETA)));
}

/* The level where the observer is correct a proportion p of the time. */
static double weibull_level(double p) {
    double q = (p - WB_GAMMA) / (1.0 - WB_GAMMA);
    return WB_ALPHA * pow(-log(1.0 - q), 1.0 / WB_BETA);
}

/* Run `reps` staircases of YST_MAX_TRIALS trials, each starting 25% above
 * the level the rule should converge on, and pool everything after `burn`
 * trials. Two things must come out right: the proportion correct over the
 * pooled trials, and the proportion correct AT the pooled geometric mean
 * level. Both are compared with yst_convergence_p. */
static void sim_case(const char* name, int n_up, int n_down, double scale,
                     int reps, int burn, double tol) {
    static yst_stair s;
    yst_desc d;
    double p_star = yst_convergence_p(n_up, n_down, scale);
    double x_star = weibull_level(p_star);
    double sum_log = 0.0;
    long n_correct = 0, n_pooled = 0;
    int r, i;

    memset(&d, 0, sizeof(d));
    d.start = x_star * 1.25;
    d.n_up = n_up;
    d.n_down = n_down;
    d.step_down_scale = scale;
    d.step_type = YST_STEP_LOG;
    d.steps[0] = 0.02;          /* a fine step, so the track sits tight      */
    d.n_steps = 1;
    d.stop_trials = YST_MAX_TRIALS;
    d.min = x_star * 0.01;
    d.max = x_star * 100.0;

    for (r = 0; r < reps; r++) {
        if (!yst_open(&s, &d)) { fail(__LINE__, yst_error(&s)); return; }
        for (i = 0; i < YST_MAX_TRIALS; i++) {
            double x = yst_next(&s);
            int resp = yst_simulate_response(weibull_p(x), next_uniform());
            if (yst_update(&s, x, resp) < 0) { fail(__LINE__, "update"); return; }
            if (i >= burn) {
                sum_log += log10(x);
                n_correct += resp;
                n_pooled++;
            }
        }
        if (yst_stop_reason(&s) != YST_STOP_TRIALS) {
            fail(__LINE__, "simulated staircase did not stop on trials");
            return;
        }
    }

    {
        double p_emp = (double)n_correct / (double)n_pooled;
        double x_bar = pow(10.0, sum_log / (double)n_pooled);
        double p_at_level = weibull_p(x_bar);
        printf("  %-22s p* = %.4f  empirical = %.4f  p(mean level) = %.4f"
               "  (%ld trials)\n", name, p_star, p_emp, p_at_level, n_pooled);
        if (!close_to(p_emp, p_star, tol))
            fail_d(__LINE__, "empirical proportion correct", p_emp, p_star);
        if (!close_to(p_at_level, p_star, tol))
            fail_d(__LINE__, "proportion correct at the converged level",
                   p_at_level, p_star);
    }
}

static void test_simulation(void) {
    printf("stair_test: simulated Weibull observer\n");
    sim_case("1-up-2-down", 1, 2, 0.0, 3000, 60, 0.01);
    sim_case("1-up-3-down", 1, 3, 0.0, 3000, 60, 0.01);
    sim_case("weighted 1-up-1-down", 1, 1, yst_weighted_scale(0.75),
             3000, 60, 0.01);
}

/* ------------------------------------------------- parameter table (v0.3) */

#include "param_table_check.h"

static void ptc_fail(int line, const char* what) { fail(line, what); }

#define F(name, kind, n) { #name, offsetof(yst_desc, name), kind, n, NULL }
#define FE(name, n, v) { #name, offsetof(yst_desc, name), 'e', n, v }
static const ptc_field g_fields[] = {
    F(start, 'd', 0), FE(rule, YST_RULE_ASA + 1, "updown=0,asa=1"), F(n_up, 'i', 0),
    F(n_down, 'i', 0), FE(step_type, YST_STEP_DB + 1, "lin=0,log=1,db=2"), F(steps, 'D', 0),
    F(n_steps, 'i', 0),
    F(step_down_scale, 'd', 0), F(target_p, 'd', 0), F(min, 'd', 0), F(max, 'd', 0),
    F(use_limits, 'b', 0), F(harder_is_up, 'b', 0), F(initial_rule, 'b', 0),
    F(stop_reversals, 'i', 0), F(stop_trials, 'i', 0), F(stop_at_limit, 'i', 0),
    F(est_reversals, 'i', 0), F(est_trials, 'i', 0),
};
#undef F
#undef FE
#define NF ((int)(sizeof g_fields / sizeof g_fields[0]))

/* Proposals of a fixed response stream: what a field's value changes. */
static void param_run(const yst_desc* d, double* out, int n) {
    static yst_stair s;
    int i;
    for (i = 0; i < n; i++) out[i] = -1.0;
    if (!yst_open(&s, d)) return;
    for (i = 0; i < n; i++) {
        out[i] = yst_next(&s);
        if (yst_update(&s, out[i], (i * 7 + 3) % 5 < 3) < 0) break;
    }
    yst_close(&s);
}

static void test_params(void) {
    static ptc_row rows[64];
    static yst_stair s;
    const yst_param* p;
    yst_desc base, d;
    int n = 0, i;
    p = yst_params(&n);
    CHECK(p != NULL);
    CHECK(n == NF);
    if (!p || n > 64) return;
    for (i = 0; i < n; i++) PTC_ROW(rows[i], p[i]);
    ptc_check_cover(rows, n, g_fields, NF);

    /* A desc that opens with every field at its default, steps over a range
     * that limits would bite, and has reversals for the estimates. */
    memset(&base, 0, sizeof base);
    base.start = 10.0;
    base.steps[0] = 2.0;
    base.steps[1] = 1.0;
    base.n_steps = 2;
    base.stop_trials = 40;
    base.stop_reversals = 30;   /* either stop alone is enough, so zero each */

    for (i = 0; i < n; i++) {
        const ptc_field* f;
        int k = ptc_check_row(&rows[i], g_fields, NF);
        char msg[160];
        if (k < 0) continue;
        f = &g_fields[k];
        if (f->kind == 'e') {
            /* The list is the enum: one past its last value is refused. */
            d = base;
            ptc_set(&d, f, rows[i].max + 1.0);
            CHECK(!yst_open(&s, &d));
        }
        if (!ptc_scalar(f)) continue;
        /* A zero field opens and reads back as def, or the row says the field
         * is required. */
        d = base;
        ptc_set(&d, f, 0.0);
        if (!yst_open(&s, &d)) {
            snprintf(msg, sizeof msg, "zero %s refused, but its doc does not say required", f->name);
            if (!ptc_required(&rows[i])) fail(__LINE__, msg);
            continue;
        }
        if (ptc_get(&s.desc, f) != rows[i].def) {
            snprintf(msg, sizeof msg, "zero %s opens as %g, def is %g", f->name,
                     ptc_get(&s.desc, f), rows[i].def);
            fail(__LINE__, msg);
        }
        /* And def itself runs the same staircase as the zero. */
        {
            double a[40], b[40];
            int t;
            param_run(&d, a, 40);
            ptc_set(&d, f, rows[i].def);
            param_run(&d, b, 40);
            for (t = 0; t < 40; t++)
                if (a[t] != b[t]) {
                    snprintf(msg, sizeof msg, "%s = 0 and %s = def differ at trial %d", f->name, f->name, t);
                    fail(__LINE__, msg);
                    break;
                }
        }
    }
    {
        int m = -1;
        CHECK(yst_params(NULL) == p);
        yst_params(&m);
        CHECK_I(m, n);
    }
}
#undef NF

/* ------------------------------------------------------- snapshots (v0.3) */

/* A response for trial t of stream `seed`: a fixed, irregular pattern. */
static int snap_resp(int seed, int t) {
    unsigned x = (unsigned)(seed * 2654435761u) ^ (unsigned)(t * 40503u + 977u);
    x ^= x >> 13;
    x *= 0x5bd1e995u;
    x ^= x >> 15;
    return (x % 100u) < 62u;
}

/* The shown level is not always the proposal: every fifth trial shows a
 * quantized one, which the snapshot must carry as recorded. */
static double snap_shown(const yst_stair* s, int t) {
    double x = yst_next(s);
    return (t % 5 == 4) ? floor(x * 64.0 + 0.5) / 64.0 + 1.0 / 128.0 : x;
}

static void snap_run(yst_stair* s, int seed, int from, int to) {
    int t;
    for (t = from; t < to; t++) {
        int rc = yst_update(s, snap_shown(s, t), snap_resp(seed, t));
        if (rc < 0 && rc != YST_ERR_FULL) fail(__LINE__, yst_error(s));
    }
}

static unsigned char g_snap_a[64 * 1024], g_snap_b[64 * 1024];

static void snap_case(const char* name, const yst_desc* d, int seed, int n) {
    static yst_stair ref, cut, res;
    static const int cuts[] = { 0, 1, 2, 7, 23, 64, 127, 128, 200 };
    int c, la, lb;
    char msg[400];
    if (!yst_open(&ref, d)) { fail(__LINE__, yst_error(&ref)); return; }
    snap_run(&ref, seed, 0, n);
    la = yst_save(&ref, g_snap_a, sizeof g_snap_a);
    CHECK(la > 0 && (size_t)la == yst_save_size(&ref));
    for (c = 0; c < (int)(sizeof cuts / sizeof cuts[0]); c++) {
        int k = cuts[c], len, i;
        if (k > n) continue;
        if (!yst_open(&cut, d)) { fail(__LINE__, yst_error(&cut)); return; }
        snap_run(&cut, seed, 0, k);
        len = yst_save(&cut, g_snap_b, sizeof g_snap_b);
        yst_close(&cut);
        /* The resumed handle starts as garbage: a load owes nothing to what
         * the handle held. */
        memset(&res, 0xA5, sizeof res);
        if (len < 0 || !yst_open_saved(&res, d, g_snap_b, (size_t)len)) {
            snprintf(msg, sizeof msg, "%s: load at %d: %s", name, k, yst_error(&res));
            fail(__LINE__, msg);
            continue;
        }
        CHECK(yst_error(&res)[0] == '\0');
        CHECK_I(yst_n_trials(&res), k < YST_MAX_TRIALS ? k : YST_MAX_TRIALS);
        snap_run(&res, seed, k, n);
        lb = yst_save(&res, g_snap_b, sizeof g_snap_b);
        if (lb != la || memcmp(g_snap_a, g_snap_b, (size_t)la) != 0) {
            snprintf(msg, sizeof msg, "%s: resumed at %d, the final snapshot differs", name, k);
            fail(__LINE__, msg);
        }
        for (i = YST_EST_REVERSALS; i <= YST_EST_LAST; i++) {
            double ea = yst_estimate(&ref, (yst_estimator)i), eb = yst_estimate(&res, (yst_estimator)i);
            CHECK((is_nan(ea) && is_nan(eb)) || ea == eb);
        }
        CHECK(yst_next(&res) == yst_next(&ref));
        CHECK(yst_stop_reason(&res) == yst_stop_reason(&ref));
        /* The handles themselves, not only the bytes the same writer makes
         * of them: a field the writer loses would be lost from both. */
        {
            int na = 0, nb = 0, t;
            const yst_trial* ha = yst_history(&ref, &na);
            const yst_trial* hb = yst_history(&res, &nb);
            CHECK_I(nb, na);
            for (t = 0; t < na && t < nb; t++)
                if (ha[t].proposed != hb[t].proposed || ha[t].shown != hb[t].shown ||
                    ha[t].response != hb[t].response || ha[t].reversal != hb[t].reversal ||
                    ha[t].direction != hb[t].direction || ha[t].step_index != hb[t].step_index) {
                    snprintf(msg, sizeof msg, "%s: resumed at %d, trial %d differs", name, k, t);
                    fail(__LINE__, msg);
                    break;
                }
            CHECK_I(yst_n_reversals(&res), yst_n_reversals(&ref));
            for (t = 0; t < yst_n_reversals(&ref) && t < YST_MAX_REVERSALS; t++)
                CHECK(yst_reversal_level(&res, t) == yst_reversal_level(&ref, t) &&
                      yst_reversal_trial(&res, t) == yst_reversal_trial(&ref, t));
            CHECK(res.level == ref.level && res.run_correct == ref.run_correct &&
                  res.run_incorrect == ref.run_incorrect && res.direction == ref.direction &&
                  res.step_index == ref.step_index && res.at_limit_run == ref.at_limit_run);
        }
        yst_close(&res);
    }
    yst_close(&ref);
}

static void test_snapshot_resume(void) {
    yst_desc d;

    /* 1-up-3-down in log10 units with a schedule, limits and the initial rule. */
    memset(&d, 0, sizeof d);
    d.start = 0.5;
    d.n_down = 3;
    d.step_type = YST_STEP_LOG;
    d.steps[0] = 0.3; d.steps[1] = 0.15; d.steps[2] = 0.075;
    d.n_steps = 3;
    d.min = 0.01; d.max = 1.0;
    d.initial_rule = true;
    d.stop_reversals = 12;
    snap_case("log 1u3d", &d, 1, 90);

    /* Weighted 1-up-1-down in dB, harder is up. */
    memset(&d, 0, sizeof d);
    d.start = 2.0;
    d.step_type = YST_STEP_DB;
    d.steps[0] = 2.0;
    d.n_steps = 1;
    d.step_down_scale = yst_weighted_scale(0.75);
    d.harder_is_up = true;
    d.stop_trials = 60;
    snap_case("weighted dB", &d, 2, 60);

    /* ASA, linear. */
    memset(&d, 0, sizeof d);
    d.rule = YST_RULE_ASA;
    d.start = 5.0;
    d.steps[0] = 2.0;
    d.n_steps = 1;
    d.target_p = 0.8;
    d.stop_trials = 50;
    snap_case("asa", &d, 3, 50);

    /* Past the reversal record and past the trial cap: 1-up-1-down at a
     * real 0 bound, run beyond YST_MAX_TRIALS so the last cuts load a full
     * staircase. */
    memset(&d, 0, sizeof d);
    d.start = 3.0;
    d.steps[0] = 1.0;
    d.n_steps = 1;
    d.use_limits = true;
    d.min = 0.0; d.max = 6.0;
    d.stop_at_limit = 9;
    d.stop_trials = YST_MAX_TRIALS;
    snap_case("full", &d, 4, YST_MAX_TRIALS + 20);
}

/* Put a little-endian i32 at `at`. */
static void put_i32_at(unsigned char* b, size_t at, int v) {
    unsigned u = (unsigned)v;
    b[at] = (unsigned char)u; b[at + 1] = (unsigned char)(u >> 8);
    b[at + 2] = (unsigned char)(u >> 16); b[at + 3] = (unsigned char)(u >> 24);
}

static void expect_refused(int line, const yst_desc* d, const unsigned char* b, size_t len,
                           const char* says) {
    static yst_stair s;
    if (yst_open_saved(&s, d, b, len)) { fail(line, "a bad snapshot loaded"); return; }
    if (yst_is_open(&s)) fail(line, "a failed load left the handle open");
    if (says && !strstr(yst_error(&s), says)) fail(line, yst_error(&s));
}

static void test_snapshot_refusals(void) {
    static yst_stair s, t;
    yst_desc d, e;
    int len, i, loaded = 0, refused = 0;
    size_t at_n;

    base_desc(&d);
    d.stop_trials = 30;
    d.n_up = 1;             /* folds to the same as 0 */
    CHECK(yst_save_size(&s) == 0);     /* zeroed */
    CHECK_I(yst_save(&s, g_snap_a, sizeof g_snap_a), YST_ERR_CLOSED);
    CHECK_I(yst_save(NULL, g_snap_a, sizeof g_snap_a), YST_ERR_ARG);
    if (!yst_open(&s, &d)) { fail(__LINE__, yst_error(&s)); return; }
    snap_run(&s, 9, 0, 20);
    len = yst_save(&s, g_snap_a, sizeof g_snap_a);
    CHECK(len > 0);
    CHECK_I(len, 143 + 20 * 20 + 12 * yst_n_reversals(&s));   /* the manual's sizes */
    CHECK_I(yst_save(&s, g_snap_b, (size_t)len - 1), YST_ERR_ARG);
    CHECK_I(yst_save(&s, NULL, sizeof g_snap_b), YST_ERR_ARG);
    CHECK_I(yst_save(&s, g_snap_b, (size_t)len), len);
    if (len <= 0) return;

    /* A desc that folds to the same staircase loads; one that does not is
     * named. */
    e = d;
    e.n_up = 0;
    CHECK(yst_open_saved(&t, &e, g_snap_a, (size_t)len));
    e.stop_trials = 31;
    expect_refused(__LINE__, &e, g_snap_a, (size_t)len, "desc.stop_trials does not match");
    e = d;
    e.steps[0] = 1.5;
    expect_refused(__LINE__, &e, g_snap_a, (size_t)len, "desc.steps does not match");
    expect_refused(__LINE__, NULL, g_snap_a, (size_t)len, "null desc");
    expect_refused(__LINE__, &d, NULL, (size_t)len, "null snapshot");

    /* Every truncation, and a byte past the end. */
    for (i = 0; i < len; i++) expect_refused(__LINE__, &d, g_snap_a, (size_t)i, NULL);
    memcpy(g_snap_b, g_snap_a, (size_t)len);
    g_snap_b[len] = 0;
    expect_refused(__LINE__, &d, g_snap_b, (size_t)len + 1, "after the snapshot's end");
    g_snap_b[0] = 'X';
    expect_refused(__LINE__, &d, g_snap_b, (size_t)len, "not a ysp_stair snapshot");
    memcpy(g_snap_b, g_snap_a, (size_t)len);
    g_snap_b[4] = 2;
    expect_refused(__LINE__, &d, g_snap_b, (size_t)len, "format");

    /* The counters sit right after the desc: 8 + the desc's bytes. */
    at_n = 8 + 8 + 4 * 5 + 8 * (size_t)d.n_steps + 8 * 4 + 3 + 4 * 5;
    memcpy(g_snap_b, g_snap_a, (size_t)len);
    put_i32_at(g_snap_b, at_n, YST_MAX_TRIALS + 1);
    expect_refused(__LINE__, &d, g_snap_b, (size_t)len, "YST_MAX_TRIALS");
    memcpy(g_snap_b, g_snap_a, (size_t)len);
    put_i32_at(g_snap_b, at_n + 4, 21);      /* more reversals than trials */
    expect_refused(__LINE__, &d, g_snap_b, (size_t)len, "counters");
    memcpy(g_snap_b, g_snap_a, (size_t)len);
    put_i32_at(g_snap_b, at_n + 20, 1);      /* step_index past n_steps */
    expect_refused(__LINE__, &d, g_snap_b, (size_t)len, "counters");
    /* The level, then the first trial: proposed, shown, response, reversal,
     * direction, step_index. */
    memcpy(g_snap_b, g_snap_a, (size_t)len);
    memset(g_snap_b + at_n + 32, 0xff, 8);   /* a NaN level */
    expect_refused(__LINE__, &d, g_snap_b, (size_t)len, "counters");
    memcpy(g_snap_b, g_snap_a, (size_t)len);
    g_snap_b[at_n + 40 + 16] = 2;            /* a response of 2 */
    expect_refused(__LINE__, &d, g_snap_b, (size_t)len, "history");
    memcpy(g_snap_b, g_snap_a, (size_t)len);
    g_snap_b[at_n + 40 + 18] = 2;            /* a direction of 2 */
    expect_refused(__LINE__, &d, g_snap_b, (size_t)len, "history");
    memcpy(g_snap_b, g_snap_a, (size_t)len);
    memset(g_snap_b + at_n + 40 + 8, 0xff, 8); /* a NaN shown level */
    expect_refused(__LINE__, &d, g_snap_b, (size_t)len, "history");
    CHECK(yst_n_reversals(&s) > 0);
    memcpy(g_snap_b, g_snap_a, (size_t)len);
    put_i32_at(g_snap_b, at_n + 40 + 20 * (size_t)yst_n_trials(&s) + 4 + 8, yst_n_trials(&s));
    expect_refused(__LINE__, &d, g_snap_b, (size_t)len, "reversals");   /* a trial past the end */
    /* The reversal count field is the last i32 before the reversals. */
    memcpy(g_snap_b, g_snap_a, (size_t)len);
    put_i32_at(g_snap_b, at_n + 40 + 20 * (size_t)yst_n_trials(&s), yst_n_reversals(&s) + 1);
    expect_refused(__LINE__, &d, g_snap_b, (size_t)len, "YST_MAX_REVERSALS");

    /* Every single-bit flip either loads or is refused with the handle
     * closed and a message; none may crash or leave a half-open handle. */
    for (i = 0; i < len * 8; i++) {
        memcpy(g_snap_b, g_snap_a, (size_t)len);
        g_snap_b[i / 8] ^= (unsigned char)(1u << (i % 8));
        if (yst_open_saved(&t, &d, g_snap_b, (size_t)len)) {
            loaded++;
            CHECK(yst_error(&t)[0] == '\0');
            (void)yst_next(&t);
            (void)yst_estimate(&t, YST_EST_MEDIAN_REV);
        } else {
            refused++;
            CHECK(!yst_is_open(&t) && yst_error(&t)[0] != '\0');
        }
    }
    CHECK(refused > 0 && loaded > 0);
    yst_close(&s);
    yst_close(&t);
}

/* -------------------------------------------------------------------- main */

/* The version macros agree with each other and with the implementation. */
static void test_version(void) {
    char want[32];
    snprintf(want, sizeof(want), "%d.%d.%d",
             YST_VERSION_MAJOR, YST_VERSION_MINOR, YST_VERSION_PATCH);
    CHECK(strcmp(YST_VERSION_STRING, want) == 0);
    CHECK(strcmp(yst_version(), YST_VERSION_STRING) == 0);
    /* Pinned, and compared at run time: a constant condition is MSVC's C4127,
     * and the string from the implementation is the thing worth pinning. */
    CHECK(strcmp(yst_version(), "0.3.0") == 0);
}

int main(void) {
    test_version();
    test_updown_1_1();
    test_updown_1_2();
    test_updown_1_3();
    test_updown_2_1();
    test_schedule_and_initial_rule();
    test_empty_windows();
    test_step_types();
    test_harder_is_up_and_shown();
    test_limits();
    test_full();
    test_weighted();
    test_asa();
    test_closed_and_bad_args();
    test_bad_descs();
    test_simulation();
    test_params();
    test_snapshot_resume();
    test_snapshot_refusals();

    if (g_failures != 0) {
        fprintf(stderr, "stair_test: %d check(s) failed\n", g_failures);
        return 1;
    }
    printf("stair_test: all checks passed (YST_MAX_TRIALS=%d, "
           "YST_MAX_REVERSALS=%d, sizeof(yst_stair)=%lu)\n",
           YST_MAX_TRIALS, YST_MAX_REVERSALS,
           (unsigned long)sizeof(yst_stair));
    return 0;
}
