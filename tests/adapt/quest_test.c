/* quest_test.c - self-checking tests for ysp/quest.h.
 *
 * No framework: every check prints a line to stderr and bumps a counter, and
 * main() returns 1 if the counter is not zero. Build and run:
 *
 *   cc -O2 -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Werror -Iinclude \
 *      -o quest_test tests/adapt/quest_test.c -lm && ./quest_test
 *
 * The reference sections below are written from the definitions (Watson 2017
 * eq. 8-11 and Bayes' rule), in double precision, without looking at how the
 * header arranges the arithmetic. They are the point of this file: the header
 * rearranges the entropy sum to avoid a division and stores the likelihood
 * table as float, and those two liberties are what has to be shown harmless.
 */
#define YSP_QUEST_IMPLEMENTATION
#include "ysp/quest.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0;
static int g_checks = 0;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        g_checks++;                                                           \
        if (!(cond)) {                                                        \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);              \
            fprintf(stderr, __VA_ARGS__);                                     \
            fputc('\n', stderr);                                              \
            g_fail++;                                                         \
        }                                                                     \
    } while (0)

#define CLOSE(a, b, tol, ...)                                                 \
    do {                                                                      \
        double aa_ = (a), bb_ = (b);                                          \
        g_checks++;                                                           \
        if (!(fabs(aa_ - bb_) <= (tol))) {                                    \
            fprintf(stderr, "FAIL %s:%d: %.17g vs %.17g (tol %g): ",          \
                    __FILE__, __LINE__, aa_, bb_, (double)(tol));             \
            fprintf(stderr, __VA_ARGS__);                                     \
            fputc('\n', stderr);                                              \
            g_fail++;                                                         \
        }                                                                     \
    } while (0)

/* yqst_posterior() and yqst_history() return NULL on a handle that is not
 * open. The test indexes what they return, so it goes through these: a NULL
 * is a failed check and a zeroed stand-in, never a dereference. */
static double     g_null_post[65536];
static yqst_trial g_null_hist[YQST_MAX_TRIALS];

/* Failures only, not checks: these run inside other checks' arithmetic, and
 * counting them would inflate the total without testing anything new. */
static const double* post_of(const yqst_quest* q) {
    const double* p = yqst_posterior(q);
    if (!p) { fputs("FAIL: yqst_posterior returned NULL\n", stderr); g_fail++; }
    return p ? p : g_null_post;
}

static const yqst_trial* hist_of(const yqst_quest* q, int* n) {
    const yqst_trial* h = yqst_history(q, n);
    if (!h) { fputs("FAIL: yqst_history returned NULL\n", stderr); g_fail++; }
    return h ? h : g_null_hist;
}

/* ======================================================================= *
 *  A deterministic generator, so a whole run replays from a seed.
 * ======================================================================= */

static uint64_t g_sm_state = 0;

static void sm_seed(uint64_t s) { g_sm_state = s; }

static double sm_u(void) {
    uint64_t z = (g_sm_state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z = z ^ (z >> 31);
    return (double)(z >> 11) * (1.0 / 9007199254740992.0);
}

/* A generator that hands out a fixed list, so a tie or a subset draw has a
 * known answer. */
typedef struct {
    const double* v;
    int n, i;
} seq_rng;

static double seq_rng_fn(void* ctx) {
    seq_rng* r = (seq_rng*)ctx;
    double u = r->v[r->i % r->n];
    r->i++;
    return u;
}

/* ======================================================================= *
 *  REFERENCE MODEL: 5 stimuli x (4 x 3 x 1 x 1) parameters, K = 2
 * ======================================================================= */

#define RS 5
#define RA 4
#define RB 3
#define RP (RA * RB)

static double ref_x[RS];
static double ref_alpha[RA];
static double ref_beta[RB];
static const double REF_GAMMA = 0.5;
static const double REF_LAMBDA = 0.03;

static void ref_build_axes(void) {
    int i;
    for (i = 0; i < RS; i++) ref_x[i] = -2.5 + 2.0 * (double)i / (double)(RS - 1);
    for (i = 0; i < RA; i++) ref_alpha[i] = -2.0 + 1.5 * (double)i / (double)(RA - 1);
    for (i = 0; i < RB; i++) ref_beta[i] = 1.0 + 3.0 * (double)i / (double)(RB - 1);
}

/* p(correct) for the log-Weibull, written straight from the manual. */
static double ref_p1(double x, double alpha, double beta) {
    double f = 1.0 - exp(-pow(10.0, beta * (x - alpha)));
    return REF_GAMMA + (1.0 - REF_GAMMA - REF_LAMBDA) * f;
}

/* Parameter grid index t = ia * RB + ib, last axis fastest; the two fixed
 * axes have one point each and so do not enter the index. */
static double ref_lik(int s, int t, int k) {
    double p1 = ref_p1(ref_x[s], ref_alpha[t / RB], ref_beta[t % RB]);
    return (k == 1) ? p1 : 1.0 - p1;
}

static void ref_prior(double* post) {
    int t;
    for (t = 0; t < RP; t++) post[t] = 1.0 / (double)RP;
}

static void ref_update(double* post, int s, int k) {
    double w[RP] = {0}, sum = 0.0;
    int t;
    for (t = 0; t < RP; t++) { w[t] = post[t] * ref_lik(s, t, k); sum += w[t]; }
    for (t = 0; t < RP; t++) post[t] = w[t] / sum;
}

/* Expected posterior entropy in bits, by the definition: for every outcome,
 * normalize the updated posterior and take its entropy, then average under
 * the outcome's marginal probability. */
static double ref_expected_entropy(const double* post, int s) {
    double e = 0.0;
    int k, t;
    for (k = 0; k < 2; k++) {
        double w[RP] = {0}, pk = 0.0, h = 0.0;
        for (t = 0; t < RP; t++) { w[t] = post[t] * ref_lik(s, t, k); pk += w[t]; }
        if (pk <= 0.0) continue;
        for (t = 0; t < RP; t++) {
            double qq = w[t] / pk;
            if (qq > 0.0) h -= qq * log(qq) / log(2.0);
        }
        e += pk * h;
    }
    return e;
}

static int ref_argmin_entropy(const double* post) {
    double best = ref_expected_entropy(post, 0);
    int s, arg = 0;
    for (s = 1; s < RS; s++) {
        double v = ref_expected_entropy(post, s);
        if (v < best) { best = v; arg = s; }
    }
    return arg;
}

static void ref_desc(yqst_desc* d, bool no_table) {
    memset(d, 0, sizeof(*d));
    d->pf = YQST_PF_GUMBEL;
    d->stim[0] = yqst_values(ref_x, RS);
    d->n_stim = 1;
    d->param[0] = yqst_values(ref_alpha, RA);
    d->param[1] = yqst_values(ref_beta, RB);
    d->param[2] = yqst_fixed(REF_GAMMA);
    d->param[3] = yqst_fixed(REF_LAMBDA);
    d->n_param = 4;
    d->stop_trials = 1000;
    d->no_table = no_table;
}

/* The fixed outcome sequence the reference and the header both replay. */
static const int ref_seq_s[] = { 2, 0, 4, 1, 3, 2, 4 };
static const int ref_seq_k[] = { 1, 0, 1, 1, 0, 1, 0 };
#define REF_SEQ_N ((int)(sizeof(ref_seq_s) / sizeof(ref_seq_s[0])))

static yqst_quest g_q;
static yqst_quest g_q2;

static void test_reference(bool no_table) {
    yqst_desc d;
    double post[RP] = {0};
    double tol = no_table ? 1e-12 : 1e-6;
    int i, t, s;

    ref_desc(&d, no_table);
    CHECK(yqst_open(&g_q, &d), "open: %s", yqst_error(&g_q));
    CHECK(yqst_n_stim(&g_q) == RS, "S = %d", yqst_n_stim(&g_q));
    CHECK(yqst_n_param(&g_q) == RP, "P = %d", yqst_n_param(&g_q));
    CHECK(yqst_n_outcomes(&g_q) == 2, "K = %d", yqst_n_outcomes(&g_q));
    ref_prior(post);

    for (i = 0; i <= REF_SEQ_N; i++) {
        const double* hp = post_of(&g_q);
        double gap_best = 0.0, gap_second = 0.0;
        int arg;
        for (t = 0; t < RP; t++)
            CLOSE(hp[t], post[t], tol, "posterior cell %d after %d trials (no_table=%d)",
                  t, i, (int)no_table);
        for (s = 0; s < RS; s++)
            CLOSE(yqst_expected_entropy(&g_q, s), ref_expected_entropy(post, s), tol,
                  "expected entropy of stimulus %d after %d trials (no_table=%d)",
                  s, i, (int)no_table);
        /* The argmin can only be compared when the reference itself is not in
         * a near-tie; check that the test is asking a fair question. */
        arg = ref_argmin_entropy(post);
        gap_best = ref_expected_entropy(post, arg);
        gap_second = HUGE_VAL;
        for (s = 0; s < RS; s++)
            if (s != arg && ref_expected_entropy(post, s) < gap_second)
                gap_second = ref_expected_entropy(post, s);
        CHECK(gap_second - gap_best > 1e-5, "trial %d: the reference argmin is a near-tie (%g)",
              i, gap_second - gap_best);
        CHECK(yqst_next(&g_q) == arg, "trial %d: selected %d, reference %d (no_table=%d)",
              i, yqst_next(&g_q), arg, (int)no_table);
        CHECK(yqst_next(&g_q) == arg, "yqst_next is not stable without an update");
        /* Entropy of the joint posterior, in bits. */
        {
            double h = 0.0;
            for (t = 0; t < RP; t++) if (post[t] > 0.0) h -= post[t] * log(post[t]) / log(2.0);
            CLOSE(yqst_entropy(&g_q), h, tol, "yqst_entropy after %d trials", i);
        }
        if (i == REF_SEQ_N) break;
        CHECK(yqst_update(&g_q, ref_seq_s[i], ref_seq_k[i]) == YQST_OK, "update %d", i);
        ref_update(post, ref_seq_s[i], ref_seq_k[i]);
    }

    /* The history records both what was proposed and what was shown. */
    {
        int n = -1;
        const yqst_trial* h = hist_of(&g_q, &n);
        CHECK(n == REF_SEQ_N, "history holds %d trials", n);
        CHECK(yqst_n_trials(&g_q) == REF_SEQ_N, "n_trials = %d", yqst_n_trials(&g_q));
        for (i = 0; i < n; i++) {
            CHECK(h[i].stim_index == ref_seq_s[i], "history %d stim_index", i);
            CHECK(h[i].outcome == (uint8_t)ref_seq_k[i], "history %d outcome", i);
            CHECK(h[i].proposed_index >= 0 && h[i].proposed_index < RS,
                  "history %d proposed_index = %d", i, h[i].proposed_index);
            CLOSE(h[i].stim[0], ref_x[ref_seq_s[i]], 0.0, "history %d stim value", i);
        }
    }
    yqst_close(&g_q);
}

/* The two storage paths must agree to float precision, and the estimators
 * must agree with what the caller can compute from post_of(). */
static void test_no_table_matches(void) {
    yqst_desc a, b;
    int i;
    ref_desc(&a, false);
    ref_desc(&b, true);
    CHECK(yqst_open(&g_q, &a), "open table: %s", yqst_error(&g_q));
    CHECK(yqst_open(&g_q2, &b), "open no_table: %s", yqst_error(&g_q2));
    for (i = 0; i < REF_SEQ_N; i++) {
        CHECK(yqst_next(&g_q) == yqst_next(&g_q2), "trial %d: table %d, no_table %d",
              i, yqst_next(&g_q), yqst_next(&g_q2));
        yqst_update(&g_q, ref_seq_s[i], ref_seq_k[i]);
        yqst_update(&g_q2, ref_seq_s[i], ref_seq_k[i]);
    }
    for (i = 0; i < RP; i++)
        CLOSE(post_of(&g_q)[i], post_of(&g_q2)[i], 1e-6,
              "table vs no_table posterior cell %d", i);
    CHECK(g_q.table != NULL, "the table path has no table");
    CHECK(g_q2.table == NULL, "no_table kept a table");
    yqst_close(&g_q);
    yqst_close(&g_q2);
}

/* An off-grid update at a value that happens to be a grid point must give the
 * same posterior as the indexed update. */
static void test_update_values(void) {
    yqst_desc d;
    double sv[1] = {0};
    int i;
    ref_desc(&d, false);
    CHECK(yqst_open(&g_q, &d), "open: %s", yqst_error(&g_q));
    CHECK(yqst_open(&g_q2, &d), "open: %s", yqst_error(&g_q2));
    for (i = 0; i < REF_SEQ_N; i++) {
        sv[0] = yqst_stim_value(&g_q, ref_seq_s[i], 0);
        CHECK(yqst_update(&g_q, ref_seq_s[i], ref_seq_k[i]) == YQST_OK, "update %d", i);
        CHECK(yqst_update_values(&g_q2, sv, ref_seq_k[i]) == YQST_OK, "update_values %d", i);
    }
    for (i = 0; i < RP; i++)
        CLOSE(post_of(&g_q)[i], post_of(&g_q2)[i], 1e-6,
              "on-grid update_values disagrees at cell %d", i);
    {
        int n = -1;
        const yqst_trial* h = hist_of(&g_q2, &n);
        CHECK(h[0].stim_index == -1, "update_values recorded a grid index");
        CLOSE(h[0].stim[0], ref_x[ref_seq_s[0]], 0.0, "update_values stimulus value");
    }
    /* Halfway between two grid points: a different, still exact, update. */
    sv[0] = 0.5 * (ref_x[0] + ref_x[1]);
    CHECK(yqst_update_values(&g_q2, sv, 1) == YQST_OK, "off-grid update");
    sv[0] = (double)NAN;
    CHECK(yqst_update_values(&g_q2, sv, 1) == YQST_ERR_ARG, "NaN stimulus was accepted");
    yqst_close(&g_q);
    yqst_close(&g_q2);
}

/* ======================================================================= *
 *  MEMORY
 * ======================================================================= */

static void test_memory(void) {
    yqst_desc d;
    size_t need;
    unsigned char* buf;
    size_t i;
    const size_t guard = 32;

    ref_desc(&d, false);
    need = yqst_memory_size(&d);
    CHECK(need > 0, "memory_size returned 0");
    buf = (unsigned char*)malloc(need + guard);
    CHECK(buf != NULL, "malloc failed");
    if (!buf) return;
    memset(buf, 0xA5, need + guard);

    d.memory = buf;
    d.memory_size = need - 1;
    CHECK(!yqst_open(&g_q, &d), "open succeeded with one byte too few");
    CHECK(strstr(yqst_error(&g_q), "too small") != NULL,
          "message does not name the problem: %s", yqst_error(&g_q));
    CHECK(strstr(yqst_error(&g_q), yqst_strerror(YQST_ERR_MEMORY)) != NULL,
          "message does not name YQST_ERR_MEMORY: %s", yqst_error(&g_q));

    d.memory_size = need;
    CHECK(yqst_open(&g_q, &d), "open failed at exactly memory_size: %s", yqst_error(&g_q));
    CHECK(g_q.mem == buf, "open did not use the caller's buffer");
    CHECK(g_q.mem_size == need, "open claimed %llu bytes, memory_size said %llu",
          (unsigned long long)g_q.mem_size, (unsigned long long)need);
    /* Drive a few trials so every block in the arena is touched, then check
     * that nothing past memory_size was. */
    yqst_next(&g_q);
    yqst_update(&g_q, 2, 1);
    yqst_next(&g_q);
    for (i = 0; i < guard; i++)
        CHECK(buf[need + i] == 0xA5, "open wrote %llu bytes past memory_size",
              (unsigned long long)(i + 1));
    CHECK(!g_q.mem_owned, "the handle thinks it owns the caller's buffer");
    yqst_close(&g_q);
    free(buf);

    /* An unaligned buffer is still usable: memory_size carries the slack. */
    buf = (unsigned char*)malloc(need + 8);
    CHECK(buf != NULL, "malloc failed");
    if (!buf) return;
    d.memory = buf + 1;
    d.memory_size = need;
    CHECK(yqst_open(&g_q, &d), "open failed on an unaligned buffer: %s", yqst_error(&g_q));
    yqst_next(&g_q);
    yqst_close(&g_q);
    free(buf);

    /* And with no buffer at all, open owns one malloc. */
    d.memory = NULL;
    d.memory_size = 0;
    CHECK(yqst_open(&g_q, &d), "open without a buffer: %s", yqst_error(&g_q));
    CHECK(g_q.mem_owned, "open did not take its own memory");
    yqst_close(&g_q);
    CHECK(g_q.mem == NULL, "close did not release the memory");
    yqst_close(&g_q);   /* close twice must be safe */
}

/* ======================================================================= *
 *  PSYCHOMETRIC FUNCTIONS
 * ======================================================================= */

/* Phi written as an erf, which is not how the header writes it. */
static double ref_phi(double z) { return 0.5 * (1.0 + erf(z / sqrt(2.0))); }

static void test_pfs(void) {
    static const yqst_pf pfs[] = { YQST_PF_GUMBEL, YQST_PF_WEIBULL, YQST_PF_LOGISTIC,
                                   YQST_PF_NORMAL, YQST_PF_HYPSEC };
    static const double xs[] = { 0.25, 0.9, 1.0, 1.6, 3.0 };
    double par[4] = {0};
    int ip, ix;
    for (ip = 0; ip < 5; ip++) {
        yqst_desc d;
        memset(&d, 0, sizeof(d));
        d.pf = pfs[ip];
        d.stim[0] = yqst_values(xs, 5);
        d.n_stim = 1;
        d.param[0] = yqst_fixed(1.0);
        d.param[1] = yqst_fixed(2.0);
        d.param[2] = yqst_fixed(0.25);
        d.param[3] = yqst_fixed(0.04);
        d.n_param = 4;
        d.stop_trials = 10;
        CHECK(yqst_open(&g_q, &d), "pf %d open: %s", ip, yqst_error(&g_q));
        par[0] = 1.0; par[1] = 2.0; par[2] = 0.25; par[3] = 0.04;
        for (ix = 0; ix < 5; ix++) {
            double p[2], f = 0.0, want, x = xs[ix], z = par[1] * (x - par[0]);
            CHECK(yqst_p(&g_q, ix, par, p) == YQST_OK, "yqst_p");
            switch (pfs[ip]) {
            case YQST_PF_GUMBEL:   f = 1.0 - exp(-pow(10.0, z)); break;
            case YQST_PF_WEIBULL:  f = 1.0 - exp(-pow(x / par[0], par[1])); break;
            case YQST_PF_LOGISTIC: f = 1.0 / (1.0 + exp(-z)); break;
            case YQST_PF_NORMAL:   f = ref_phi(z); break;
            case YQST_PF_HYPSEC:   f = (2.0 / 3.14159265358979323846) *
                                       atan(exp(1.57079632679489661923 * z)); break;
            default: break;
            }
            want = par[2] + (1.0 - par[2] - par[3]) * f;
            CLOSE(p[1], want, 1e-12, "pf %d at x = %g", ip, x);
            CLOSE(p[0] + p[1], 1.0, 1e-15, "pf %d outcomes do not sum to 1", ip);
        }
        /* A few anchors that do not depend on the formula being read back. */
        if (pfs[ip] == YQST_PF_NORMAL || pfs[ip] == YQST_PF_LOGISTIC ||
            pfs[ip] == YQST_PF_HYPSEC) {
            double p[2] = {0}, st[1] = {0};
            st[0] = par[0];
            CHECK(yqst_p_values(&g_q, st, par, p) == YQST_OK, "yqst_p_values");
            CLOSE(p[1], par[2] + 0.5 * (1.0 - par[2] - par[3]), 1e-12,
                  "pf %d is not at half height at x = alpha", ip);
        }
        if (pfs[ip] == YQST_PF_WEIBULL) {
            double p[2] = {0}, st[1] = {0};
            st[0] = par[0];
            CHECK(yqst_p_values(&g_q, st, par, p) == YQST_OK, "yqst_p_values");
            CLOSE(p[1], par[2] + (1.0 - par[2] - par[3]) * (1.0 - exp(-1.0)), 1e-12,
                  "the Weibull is not at 1 - 1/e at x = alpha");
        }
        yqst_close(&g_q);
    }
}

/* ======================================================================= *
 *  NUISANCE MARGINALIZATION
 * ======================================================================= */

/* p(correct) with the lapse of this cell, for the nuisance reference. */
static double ref_p1_lam(double x, double alpha, double beta, double lam) {
    double f = 1.0 - exp(-pow(10.0, beta * (x - alpha)));
    return REF_GAMMA + (1.0 - REF_GAMMA - lam) * f;
}

/* A grid with a free threshold, a free slope and a nuisance lapse axis. The
 * selection entropy must be the entropy of the posterior marginalized over
 * the lapse, not of the joint. */
static void test_nuisance(void) {
    static const double lapse[3] = { 0.01, 0.03, 0.06 };
    yqst_desc d;
    int s, i, t, k;
    const int NB = RB, NL = 3;
    const int NP = RA * RB * 3;
    double marg_h;

    memset(&d, 0, sizeof(d));
    d.pf = YQST_PF_GUMBEL;
    d.stim[0] = yqst_values(ref_x, RS);
    d.n_stim = 1;
    d.param[0] = yqst_values(ref_alpha, RA);
    d.param[1] = yqst_values(ref_beta, RB);
    d.param[2] = yqst_fixed(REF_GAMMA);
    d.param[3] = yqst_values(lapse, NL);
    d.param[3].nuisance = true;
    d.n_param = 4;
    d.stop_trials = 1000;
    d.no_table = true;   /* double throughout, so the tolerance can be tight */
    CHECK(yqst_open(&g_q, &d), "open: %s", yqst_error(&g_q));
    CHECK(yqst_n_param(&g_q) == NP, "P = %d, want %d", yqst_n_param(&g_q), NP);

    for (i = 0; i < 4; i++) {
        const double* post = post_of(&g_q);
        /* Marginal over the nuisance axis: the lapse is the last axis, so it
         * is the fastest index and the marginal sums blocks of NL. */
        double marg[RA * RB] = {0}, h = 0.0;
        int m;
        for (m = 0; m < RA * RB; m++) {
            double sum = 0.0;
            for (k = 0; k < NL; k++) sum += post[m * NL + k];
            marg[m] = sum;
        }
        for (m = 0; m < RA * RB; m++) if (marg[m] > 0.0) h -= marg[m] * log(marg[m]) / log(2.0);
        CLOSE(yqst_entropy(&g_q), h, 1e-12, "marginal entropy after %d trials", i);

        for (s = 0; s < RS; s++) {
            /* Expected entropy of the MARGINAL posterior, by the definition. */
            double e = 0.0;
            for (k = 0; k < 2; k++) {
                double w[RA * RB * 3] = {0}, pk = 0.0, hk = 0.0, mk[RA * RB] = {0};
                int mm, b;
                for (t = 0; t < NP; t++) {
                    double pp = ref_p1_lam(ref_x[s], ref_alpha[t / (NB * NL)],
                                           ref_beta[(t / NL) % NB], lapse[t % NL]);
                    double lik = (k == 1) ? pp : 1.0 - pp;
                    w[t] = post[t] * lik;
                    pk += w[t];
                }
                for (mm = 0; mm < RA * RB; mm++) {
                    double sum = 0.0;
                    for (b = 0; b < NL; b++) sum += w[mm * NL + b];
                    mk[mm] = sum;
                }
                if (pk <= 0.0) continue;
                for (mm = 0; mm < RA * RB; mm++) {
                    double qq = mk[mm] / pk;
                    if (qq > 0.0) hk -= qq * log(qq) / log(2.0);
                }
                e += pk * hk;
            }
            CLOSE(yqst_expected_entropy(&g_q, s), e, 1e-12,
                  "marginalized expected entropy, stimulus %d, trial %d", s, i);
        }
        CHECK(yqst_update(&g_q, (i * 2) % RS, i % 2) == YQST_OK, "update %d", i);
    }
    marg_h = yqst_entropy(&g_q);

    /* The same grid and the same trials without the flag: the entropy of the
     * joint is above the entropy of the marginal, and the selection scores
     * differ, so the flag is doing something. */
    d.param[3].nuisance = false;
    CHECK(yqst_open(&g_q2, &d), "open: %s", yqst_error(&g_q2));
    for (i = 0; i < 4; i++) yqst_update(&g_q2, (i * 2) % RS, i % 2);
    CHECK(yqst_entropy(&g_q2) > marg_h + 1e-9,
          "the joint entropy (%g) is not above the marginal one (%g)",
          yqst_entropy(&g_q2), marg_h);
    for (s = 0; s < RS; s++)
        CHECK(yqst_expected_entropy(&g_q2, s) > yqst_expected_entropy(&g_q, s) + 1e-12,
              "stimulus %d: joint score %g, marginalized score %g", s,
              yqst_expected_entropy(&g_q2, s), yqst_expected_entropy(&g_q, s));
    yqst_close(&g_q);
    yqst_close(&g_q2);
}

/* ======================================================================= *
 *  PLACEMENT RULES
 * ======================================================================= */

static int nearest_stim(double target) {
    int s, best = 0;
    double bd = fabs(ref_x[0] - target);
    for (s = 1; s < RS; s++) {
        double dd = fabs(ref_x[s] - target);
        if (dd < bd - 1e-9) { bd = dd; best = s; }
    }
    return best;
}

static void test_placement(void) {
    static const yqst_select rules[] = { YQST_SELECT_QUANTILE, YQST_SELECT_MEAN,
                                         YQST_SELECT_MODE };
    int ir, i;
    for (ir = 0; ir < 3; ir++) {
        yqst_desc d;
        ref_desc(&d, false);
        d.select = rules[ir];
        d.select_param = 0;
        d.select_quantile = (rules[ir] == YQST_SELECT_QUANTILE) ? 0.5 : 0.0;
        CHECK(yqst_open(&g_q, &d), "open: %s", yqst_error(&g_q));
        for (i = 0; i < REF_SEQ_N; i++) {
            double marg[RA] = {0}, target = 0.0, c = 0.0;
            int a;
            CHECK(yqst_marginal(&g_q, 0, marg) == RA, "yqst_marginal");
            switch (rules[ir]) {
            case YQST_SELECT_MEAN:
                for (a = 0; a < RA; a++) target += marg[a] * ref_alpha[a];
                break;
            case YQST_SELECT_MODE: {
                int best = 0;
                for (a = 1; a < RA; a++) if (marg[a] > marg[best]) best = a;
                target = ref_alpha[best];
                break;
            }
            default:
                for (a = 0; a < RA; a++) {
                    c += marg[a];
                    if (c >= 0.5) { target = ref_alpha[a]; break; }
                }
                break;
            }
            CHECK(yqst_next(&g_q) == nearest_stim(target),
                  "rule %d trial %d: selected %d, want %d (target %g)",
                  ir, i, yqst_next(&g_q), nearest_stim(target), target);
            yqst_update(&g_q, ref_seq_s[i], ref_seq_k[i]);
        }
        /* The quantile rule and the median estimator must agree on the same
         * marginal. */
        if (rules[ir] == YQST_SELECT_QUANTILE) {
            double est[4] = {0};
            CHECK(yqst_estimate(&g_q, YQST_EST_MEDIAN, est) == YQST_OK, "estimate");
            CLOSE(est[0], yqst_quantile(&g_q, 0, 0.5), 0.0, "median vs quantile");
        }
        yqst_close(&g_q);
    }
}

/* ======================================================================= *
 *  TIES AND RANDOM SUBSETS
 * ======================================================================= */

/* A model that ignores the stimulus, so every stimulus has exactly the same
 * expected entropy and the tiebreak rule is the only thing choosing. */
static void flat_pf(void* ctx, const double* stim, const double* params, double* p) {
    (void)ctx; (void)stim;
    p[1] = params[0];
    p[0] = 1.0 - params[0];
}

static void flat_desc(yqst_desc* d) {
    static const double pv[3] = { 0.3, 0.5, 0.7 };
    memset(d, 0, sizeof(*d));
    d->pf = YQST_PF_CUSTOM;
    d->pf_fn = flat_pf;
    d->n_outcomes = 2;
    d->stim[0] = yqst_values(ref_x, RS);
    d->n_stim = 1;
    d->param[0] = yqst_values(pv, 3);
    d->n_param = 1;
    d->stop_trials = 1000;
}

static void test_ties(void) {
    yqst_desc d;
    int i;

    /* LOWEST */
    flat_desc(&d);
    CHECK(yqst_open(&g_q, &d), "open: %s", yqst_error(&g_q));
    for (i = 0; i < 3; i++) {
        CHECK(yqst_next(&g_q) == 0, "TIE_LOWEST picked %d", yqst_next(&g_q));
        yqst_update(&g_q, 2, 1);
    }
    yqst_close(&g_q);

    /* NEAREST: after a trial at index 3 the tie resolves to 3 itself. */
    flat_desc(&d);
    d.tiebreak = YQST_TIE_NEAREST;
    CHECK(yqst_open(&g_q, &d), "open: %s", yqst_error(&g_q));
    CHECK(yqst_next(&g_q) == 0, "TIE_NEAREST before any trial picked %d", yqst_next(&g_q));
    yqst_update(&g_q, 3, 1);
    CHECK(yqst_next(&g_q) == 3, "TIE_NEAREST after a trial at 3 picked %d", yqst_next(&g_q));
    yqst_update(&g_q, 1, 0);
    CHECK(yqst_next(&g_q) == 1, "TIE_NEAREST after a trial at 1 picked %d", yqst_next(&g_q));
    yqst_close(&g_q);

    /* ALTERNATE */
    flat_desc(&d);
    d.tiebreak = YQST_TIE_ALTERNATE;
    CHECK(yqst_open(&g_q, &d), "open: %s", yqst_error(&g_q));
    for (i = 0; i < 4; i++) {
        int want = (i % 2 == 0) ? 0 : RS - 1;
        CHECK(yqst_next(&g_q) == want, "TIE_ALTERNATE trial %d picked %d, want %d",
              i, yqst_next(&g_q), want);
        yqst_update(&g_q, 2, i % 2);
    }
    yqst_close(&g_q);

    /* RANDOM: the draw picks the j-th tied candidate, j = floor(u * count). */
    {
        static const double us[3] = { 0.0, 0.5, 0.99 };
        static const int want[3] = { 0, 2, 4 };
        seq_rng r;
        r.v = us; r.n = 3; r.i = 0;
        flat_desc(&d);
        d.tiebreak = YQST_TIE_RANDOM;
        d.rng = seq_rng_fn;
        d.rng_ctx = &r;
        CHECK(yqst_open(&g_q, &d), "open: %s", yqst_error(&g_q));
        for (i = 0; i < 3; i++) {
            CHECK(yqst_next(&g_q) == want[i], "TIE_RANDOM draw %d picked %d, want %d",
                  i, yqst_next(&g_q), want[i]);
            yqst_update(&g_q, 2, i % 2);
        }
        CHECK(r.i == 3, "TIE_RANDOM drew %d variates for 3 ties", r.i);
        yqst_close(&g_q);
    }

    /* A tolerance wide enough to swallow every difference makes every
     * stimulus tied, even on a grid whose entropies differ. */
    {
        yqst_desc t;
        ref_desc(&t, false);
        t.tie_tolerance = 1e9;
        t.tiebreak = YQST_TIE_ALTERNATE;
        CHECK(yqst_open(&g_q, &t), "open: %s", yqst_error(&g_q));
        CHECK(yqst_next(&g_q) == 0, "wide tolerance, first pick");
        yqst_update(&g_q, 0, 1);
        CHECK(yqst_next(&g_q) == RS - 1, "wide tolerance, second pick");
        yqst_close(&g_q);
    }
}

static void test_subset(void) {
    yqst_desc d;
    /* 0.0 -> 0, 0.25 -> 1, 0.85 -> 4 on a 5-point grid. */
    static const double us[3] = { 0.0, 0.25, 0.85 };
    seq_rng r;
    int global, chosen, i;
    double best;
    int explicit_set[2] = {0};

    ref_desc(&d, false);
    CHECK(yqst_open(&g_q2, &d), "open: %s", yqst_error(&g_q2));
    global = yqst_next(&g_q2);

    r.v = us; r.n = 3; r.i = 0;
    d.rng = seq_rng_fn;
    d.rng_ctx = &r;
    d.subset_size = 3;
    CHECK(yqst_open(&g_q, &d), "open: %s", yqst_error(&g_q));
    chosen = yqst_next(&g_q);
    CHECK(chosen == 0 || chosen == 1 || chosen == 4, "subset selection picked %d", chosen);
    best = HUGE_VAL;
    for (i = 0; i < 3; i++) {
        int s = (i == 0) ? 0 : (i == 1 ? 1 : 4);
        double e = yqst_expected_entropy(&g_q, s);
        if (e < best - 1e-12) { best = e; }
    }
    CLOSE(yqst_expected_entropy(&g_q, chosen), best, 1e-12,
          "the subset selection is not the best of the subset");
    CHECK(r.i == 3, "subset_size 3 drew %d variates", r.i);
    yqst_close(&g_q);

    /* yqst_next_subset does the same with the caller's list. */
    explicit_set[0] = (global + 1) % RS;
    explicit_set[1] = (global + 2) % RS;
    {
        int pick = yqst_next_subset(&g_q2, explicit_set, 2);
        double e0 = yqst_expected_entropy(&g_q2, explicit_set[0]);
        double e1 = yqst_expected_entropy(&g_q2, explicit_set[1]);
        CHECK(pick == explicit_set[0] || pick == explicit_set[1],
              "next_subset returned %d, outside the list", pick);
        CLOSE(yqst_expected_entropy(&g_q2, pick), (e0 < e1) ? e0 : e1, 1e-12,
              "next_subset did not take the better of the two");
        CHECK(yqst_next_subset(&g_q2, explicit_set, 0) == YQST_ERR_ARG, "empty subset");
        CHECK(yqst_next_subset(&g_q2, NULL, 2) == YQST_ERR_ARG, "null subset");
        explicit_set[0] = RS;
        CHECK(yqst_next_subset(&g_q2, explicit_set, 2) == YQST_ERR_ARG, "out-of-range subset");
    }
    yqst_close(&g_q2);
}

/* ======================================================================= *
 *  STOPPING
 * ======================================================================= */

static void test_stopping(void) {
    yqst_desc d;
    int i;

    /* Trials. */
    ref_desc(&d, false);
    d.stop_trials = 3;
    CHECK(yqst_open(&g_q, &d), "open: %s", yqst_error(&g_q));
    for (i = 0; i < 3; i++) {
        CHECK(!yqst_done(&g_q), "done fired after %d of 3 trials", i);
        yqst_update(&g_q, 2, i % 2);
    }
    CHECK(yqst_done(&g_q), "done did not fire at 3 trials");
    CHECK(yqst_stop_reason(&g_q) == YQST_STOP_TRIALS, "stop reason %d",
          (int)yqst_stop_reason(&g_q));
    CHECK(yqst_update(&g_q, 2, 1) == YQST_OK, "a trial after done was refused");
    yqst_close(&g_q);

    /* Entropy, mid-run: the uniform prior over 12 cells is log2(12) bits. */
    ref_desc(&d, false);
    d.stop_trials = 0;
    d.stop_entropy = 2.0;
    CHECK(yqst_open(&g_q, &d), "open: %s", yqst_error(&g_q));
    CLOSE(yqst_entropy(&g_q), log(12.0) / log(2.0), 1e-12, "prior entropy");
    CHECK(!yqst_done(&g_q), "the entropy criterion fired on the prior");
    for (i = 0; i < 40 && !yqst_done(&g_q); i++) yqst_update(&g_q, yqst_next(&g_q), 1);
    CHECK(yqst_done(&g_q), "the entropy criterion never fired");
    CHECK(yqst_stop_reason(&g_q) == YQST_STOP_ENTROPY, "stop reason %d",
          (int)yqst_stop_reason(&g_q));
    CHECK(yqst_entropy(&g_q) <= 2.0, "stopped at %g bits", yqst_entropy(&g_q));
    yqst_close(&g_q);

    /* sd, mid-run. */
    ref_desc(&d, false);
    d.stop_trials = 0;
    d.stop_sd = 0.35;
    d.stop_sd_param = 0;
    CHECK(yqst_open(&g_q, &d), "open: %s", yqst_error(&g_q));
    CHECK(!yqst_done(&g_q), "the sd criterion fired on the prior");
    for (i = 0; i < 60 && !yqst_done(&g_q); i++) yqst_update(&g_q, yqst_next(&g_q), i % 3 ? 1 : 0);
    CHECK(yqst_done(&g_q), "the sd criterion never fired");
    CHECK(yqst_stop_reason(&g_q) == YQST_STOP_SD, "stop reason %d", (int)yqst_stop_reason(&g_q));
    CHECK(yqst_sd(&g_q, 0) <= 0.35, "stopped at sd %g", yqst_sd(&g_q, 0));
    yqst_close(&g_q);

    /* sd, at open: a bound no posterior can exceed fires before any trial. */
    ref_desc(&d, false);
    d.stop_trials = 0;
    d.stop_sd = 100.0;
    CHECK(yqst_open(&g_q, &d), "open: %s", yqst_error(&g_q));
    CHECK(yqst_done(&g_q) && yqst_stop_reason(&g_q) == YQST_STOP_SD,
          "a bound above the prior sd did not fire at open");
    yqst_close(&g_q);

    /* Full history. */
    ref_desc(&d, false);
    d.stop_trials = YQST_MAX_TRIALS * 4;
    CHECK(yqst_open(&g_q, &d), "open: %s", yqst_error(&g_q));
    for (i = 0; i < YQST_MAX_TRIALS; i++)
        CHECK(yqst_update(&g_q, i % RS, i % 2) == YQST_OK, "update %d refused", i);
    CHECK(yqst_stop_reason(&g_q) == YQST_STOP_FULL, "stop reason %d at a full history",
          (int)yqst_stop_reason(&g_q));
    CHECK(yqst_done(&g_q), "done did not fire at a full history");
    CHECK(yqst_update(&g_q, 0, 1) == YQST_ERR_FULL, "a trial past the end was accepted");
    CHECK(yqst_n_trials(&g_q) == YQST_MAX_TRIALS, "n_trials = %d", yqst_n_trials(&g_q));
    yqst_close(&g_q);
}

/* ======================================================================= *
 *  ESTIMATORS
 * ======================================================================= */

static void test_estimates(void) {
    yqst_desc d;
    const double* post;
    double est[4] = {0}, marg[RA] = {0}, m0 = 0.0, m2 = 0.0, c = 0.0;
    int i, best, want_median = 0;

    ref_desc(&d, false);
    CHECK(yqst_open(&g_q, &d), "open: %s", yqst_error(&g_q));
    for (i = 0; i < REF_SEQ_N; i++) yqst_update(&g_q, ref_seq_s[i], ref_seq_k[i]);
    post = post_of(&g_q);

    CHECK(yqst_marginal(&g_q, 0, marg) == RA, "yqst_marginal returned the wrong count");
    {
        double sum = 0.0;
        for (i = 0; i < RA; i++) {
            double blk = 0.0;
            int b;
            for (b = 0; b < RB; b++) blk += post[i * RB + b];
            CLOSE(marg[i], blk, 1e-15, "marginal cell %d", i);
            sum += marg[i];
        }
        CLOSE(sum, 1.0, 1e-12, "the marginal is not normalized");
    }
    for (i = 0; i < RA; i++) { m0 += marg[i] * ref_alpha[i]; m2 += marg[i] * ref_alpha[i] * ref_alpha[i]; }
    CHECK(yqst_estimate(&g_q, YQST_EST_MEAN, est) == YQST_OK, "estimate MEAN");
    CLOSE(est[0], m0, 1e-12, "marginal mean of alpha");
    CLOSE(est[2], REF_GAMMA, 1e-15, "a fixed parameter's mean is not its value");
    CLOSE(yqst_sd(&g_q, 0), sqrt(m2 - m0 * m0), 1e-12, "marginal sd of alpha");
    CLOSE(yqst_sd(&g_q, 2), 0.0, 1e-15, "a fixed parameter has a nonzero sd");

    best = 0;
    for (i = 1; i < RP; i++) if (post[i] > post[best]) best = i;
    CHECK(yqst_estimate(&g_q, YQST_EST_MODE, est) == YQST_OK, "estimate MODE");
    CLOSE(est[0], ref_alpha[best / RB], 0.0, "joint mode alpha");
    CLOSE(est[1], ref_beta[best % RB], 0.0, "joint mode beta");

    for (i = 0; i < RA; i++) { c += marg[i]; if (c >= 0.5) { want_median = i; break; } }
    CHECK(yqst_estimate(&g_q, YQST_EST_MEDIAN, est) == YQST_OK, "estimate MEDIAN");
    CLOSE(est[0], ref_alpha[want_median], 0.0, "marginal median of alpha");
    CLOSE(yqst_quantile(&g_q, 0, 0.5), ref_alpha[want_median], 0.0, "quantile 0.5");
    CHECK(yqst_quantile(&g_q, 0, 0.0) != yqst_quantile(&g_q, 0, 0.0) ,
          "quantile 0 must be NaN");
    CHECK(yqst_quantile(&g_q, 0, 1.0) != yqst_quantile(&g_q, 0, 1.0),
          "quantile 1 must be NaN");
    CHECK(yqst_quantile(&g_q, 9, 0.5) != yqst_quantile(&g_q, 9, 0.5),
          "quantile on a bad axis must be NaN");
    CHECK(yqst_estimate(&g_q, (yqst_estimator)99, est) == YQST_ERR_ARG, "bad estimator");
    CHECK(yqst_quantile(&g_q, 0, 0.99) >= yqst_quantile(&g_q, 0, 0.01),
          "the quantile function is not monotone");
    yqst_close(&g_q);
}

/* ======================================================================= *
 *  A CUSTOM MODEL WITH THREE OUTCOMES
 * ======================================================================= */

/* "Less / same / more": two criteria around a point of subjective equality.
 * params = (pse, sigma, criterion). */
static void three_pf(void* ctx, const double* stim, const double* params, double* p) {
    double lo, hi;
    (void)ctx;
    lo = 0.5 * (1.0 + erf(((-params[2]) - (stim[0] - params[0])) / (params[1] * sqrt(2.0))));
    hi = 0.5 * (1.0 + erf((( params[2]) - (stim[0] - params[0])) / (params[1] * sqrt(2.0))));
    p[0] = lo;             /* "less"  */
    p[1] = hi - lo;        /* "same"  */
    p[2] = 1.0 - hi;       /* "more"  */
}

static void test_three_outcomes(void) {
    static const double pse[4] = { -0.6, -0.2, 0.2, 0.6 };
    static const double sig[3] = { 0.4, 0.8, 1.6 };
    static const double crit[2] = { 0.3, 0.9 };
    yqst_desc d;
    double post[4 * 3 * 2] = {0};
    double p[3] = {0}, par[3] = {0}, sv[1] = {0};
    int i, t, k, counts[3] = {0};

    memset(&d, 0, sizeof(d));
    d.pf = YQST_PF_CUSTOM;
    d.pf_fn = three_pf;
    d.n_outcomes = 3;
    d.stim[0] = yqst_linspace(-2.0, 2.0, 7);
    d.n_stim = 1;
    d.param[0] = yqst_values(pse, 4);
    d.param[1] = yqst_values(sig, 3);
    d.param[2] = yqst_values(crit, 2);
    d.n_param = 3;
    d.stop_trials = 20;
    CHECK(yqst_open(&g_q, &d), "open: %s", yqst_error(&g_q));
    CHECK(yqst_n_outcomes(&g_q) == 3, "K = %d", yqst_n_outcomes(&g_q));
    CHECK(yqst_n_param(&g_q) == 24, "P = %d", yqst_n_param(&g_q));

    for (t = 0; t < 24; t++) post[t] = 1.0 / 24.0;
    for (i = 0; i < 6; i++) {
        int s = yqst_next(&g_q);
        int outcome = i % 3;
        double sum = 0.0;
        for (t = 0; t < 24; t++) {
            double pr[3] = {0};
            sv[0] = yqst_stim_value(&g_q, s, 0);
            par[0] = pse[t / 6]; par[1] = sig[(t / 2) % 3]; par[2] = crit[t % 2];
            three_pf(NULL, sv, par, pr);
            post[t] *= pr[outcome];
            sum += post[t];
        }
        for (t = 0; t < 24; t++) post[t] /= sum;
        CHECK(yqst_update(&g_q, s, outcome) == YQST_OK, "update %d", i);
        for (t = 0; t < 24; t++)
            CLOSE(post_of(&g_q)[t], post[t], 1e-6, "K=3 posterior cell %d, trial %d", t, i);
    }
    CHECK(yqst_update(&g_q, 0, 3) == YQST_ERR_ARG, "outcome K was accepted");

    /* yqst_simulate must reach all three outcomes and follow the cumulative. */
    par[0] = 0.0; par[1] = 0.8; par[2] = 0.5;
    sv[0] = yqst_stim_value(&g_q, 3, 0);
    three_pf(NULL, sv, par, p);
    counts[0] = counts[1] = counts[2] = 0;
    for (i = 0; i < 3000; i++) {
        int k2 = yqst_simulate(&g_q, 3, par, (double)i / 3000.0);
        CHECK(k2 >= 0 && k2 < 3, "simulate returned %d", k2);
        if (k2 >= 0 && k2 < 3) counts[k2]++;
    }
    for (k = 0; k < 3; k++)
        CLOSE((double)counts[k] / 3000.0, p[k], 2e-3, "simulate outcome %d frequency", k);
    CHECK(yqst_simulate(&g_q, 3, par, 1.0) == YQST_ERR_ARG, "u = 1 was accepted");
    CHECK(yqst_simulate(&g_q, 3, par, -0.1) == YQST_ERR_ARG, "u < 0 was accepted");
    yqst_close(&g_q);
}

/* ======================================================================= *
 *  DESCRIPTIONS open() MUST REJECT
 * ======================================================================= */

static void bad_pf(void* ctx, const double* stim, const double* params, double* p) {
    (void)ctx; (void)stim; (void)params;
    p[0] = 0.4;
    p[1] = 0.4;   /* sums to 0.8 */
}

static void reject(yqst_desc* d, const char* what) {
    bool ok = yqst_open(&g_q, d);
    g_checks++;
    if (ok) {
        fprintf(stderr, "FAIL: open accepted an invalid desc (%s)\n", what);
        g_fail++;
        yqst_close(&g_q);
        return;
    }
    g_checks++;
    if (yqst_error(&g_q)[0] == '\0') {
        fprintf(stderr, "FAIL: open rejected %s without a message\n", what);
        g_fail++;
    }
    g_checks++;
    if (yqst_memory_size(d) != 0) {
        fprintf(stderr, "FAIL: memory_size is nonzero for an invalid desc (%s)\n", what);
        g_fail++;
    }
}

static void test_rejections(void) {
    yqst_desc d;
    static const double neg[3] = { 1.0, -1.0, 1.0 };
    static const double zero[3] = { 0.0, 0.0, 0.0 };
    static double joint_zero[RP];

    CHECK(!yqst_open(&g_q, NULL), "open accepted a null desc");
    CHECK(yqst_error(&g_q)[0] != '\0', "a null desc left no message");
    /* A null handle must be refused before the desc is read at all. The desc
     * is deliberately garbage to show that, and garbage on purpose: an
     * uninitialized struct would be undefined behavior (clang says so). */
    memset(&d, 0xA5, sizeof(d));
    CHECK(!yqst_open(NULL, &d), "open accepted a null handle");
    CHECK(yqst_memory_size(NULL) == 0, "memory_size of a null desc");

    ref_desc(&d, false); d.n_stim = 0;                    reject(&d, "n_stim = 0");
    ref_desc(&d, false); d.n_stim = YQST_MAX_STIM_DIMS + 1; reject(&d, "n_stim too large");
    ref_desc(&d, false); d.n_param = 0;                   reject(&d, "n_param = 0");
    ref_desc(&d, false); d.n_param = 3;                   reject(&d, "a built-in with n_param = 3");
    ref_desc(&d, false); d.param[1].n = 0;                reject(&d, "an axis with n = 0");
    ref_desc(&d, false); d.stim[0] = yqst_linspace(0.0, (double)NAN, 4);
                                                          reject(&d, "a non-finite axis bound");
    ref_desc(&d, false); d.param[0].prior = neg;          reject(&d, "a negative prior weight");
    ref_desc(&d, false); d.param[1].prior = zero;         reject(&d, "a prior that sums to zero");
    ref_desc(&d, false); d.joint_prior = joint_zero;      reject(&d, "a joint prior that sums to zero");
    ref_desc(&d, false); d.stim[0].nuisance = true;       reject(&d, "a nuisance stimulus axis");
    ref_desc(&d, false); d.stop_trials = 0;               reject(&d, "no stop criterion");
    ref_desc(&d, false); d.tiebreak = YQST_TIE_RANDOM;    reject(&d, "TIE_RANDOM without rng");
    ref_desc(&d, false); d.subset_size = 2;               reject(&d, "subset_size without rng");
    ref_desc(&d, false); d.tie_tolerance = -1.0;          reject(&d, "a negative tie_tolerance");
    ref_desc(&d, false); d.select = YQST_SELECT_MEAN; d.select_param = 9;
                                                          reject(&d, "select_param out of range");
    ref_desc(&d, false); d.select = YQST_SELECT_QUANTILE; d.select_quantile = 1.0;
                                                          reject(&d, "select_quantile = 1");
    ref_desc(&d, false); d.select = YQST_SELECT_QUANTILE; d.select_quantile = -0.2;
                                                          reject(&d, "a negative select_quantile");
    ref_desc(&d, false); d.select = (yqst_select)9;       reject(&d, "select out of range");
    ref_desc(&d, false); d.tiebreak = (yqst_tiebreak)9;   reject(&d, "tiebreak out of range");
    ref_desc(&d, false); d.pf = (yqst_pf)99;              reject(&d, "pf out of range");
    ref_desc(&d, false); d.stop_trials = 0; d.stop_sd = 0.1; d.stop_sd_param = 7;
                                                          reject(&d, "stop_sd_param out of range");
    ref_desc(&d, false); d.stop_entropy = -1.0;           reject(&d, "a negative stop_entropy");
    ref_desc(&d, false); d.memory = (void*)&d; d.memory_size = 0;
                                                          reject(&d, "memory without memory_size");
    ref_desc(&d, false); d.n_outcomes = 3;                reject(&d, "a built-in with 3 outcomes");

    /* The Weibull needs a positive stimulus axis and a positive alpha. */
    ref_desc(&d, false); d.pf = YQST_PF_WEIBULL;          reject(&d, "a Weibull on a negative axis");

    /* Custom models. */
    ref_desc(&d, false); d.pf = YQST_PF_CUSTOM; d.n_outcomes = 2;
                                                          reject(&d, "YQST_PF_CUSTOM without pf_fn");
    ref_desc(&d, false); d.pf = YQST_PF_CUSTOM; d.pf_fn = bad_pf; d.n_outcomes = 1;
                                                          reject(&d, "n_outcomes = 1");
    ref_desc(&d, false); d.pf = YQST_PF_CUSTOM; d.pf_fn = bad_pf;
    d.n_outcomes = YQST_MAX_OUTCOMES + 1;                 reject(&d, "n_outcomes over the maximum");

    /* A callback whose outcomes do not sum to 1, with and without a table. */
    ref_desc(&d, false); d.pf = YQST_PF_CUSTOM; d.pf_fn = bad_pf; d.n_outcomes = 2;
    {
        bool ok = yqst_open(&g_q, &d);
        CHECK(!ok, "open accepted a pf_fn whose outcomes sum to 0.8");
        CHECK(strstr(yqst_error(&g_q), "sum") != NULL, "message: %s", yqst_error(&g_q));
        if (ok) yqst_close(&g_q);
        d.no_table = true;
        ok = yqst_open(&g_q, &d);
        CHECK(!ok, "open accepted a bad pf_fn under no_table");
        if (ok) yqst_close(&g_q);
    }

    /* Calls on a closed handle. */
    memset(&g_q2, 0, sizeof(g_q2));
    CHECK(yqst_next(&g_q2) == YQST_ERR_CLOSED, "next on a closed handle");
    CHECK(yqst_update(&g_q2, 0, 0) == YQST_ERR_CLOSED, "update on a closed handle");
    CHECK(yqst_n_trials(&g_q2) == YQST_ERR_CLOSED, "n_trials on a closed handle");
    CHECK(yqst_posterior(&g_q2) == NULL, "posterior of a closed handle");
    CHECK(!yqst_done(&g_q2), "done on a closed handle");
    CHECK(!yqst_is_open(&g_q2), "is_open on a closed handle");
    CHECK(yqst_next(NULL) == YQST_ERR_ARG, "next(NULL)");
    CHECK(yqst_update(NULL, 0, 0) == YQST_ERR_ARG, "update(NULL)");
    CHECK(yqst_strerror(YQST_ERR_MEMORY)[0] != '\0', "strerror");
    CHECK(strcmp(yqst_strerror(0), "ok") == 0, "strerror(0)");
    CHECK(yqst_strerror(-99)[0] != '\0', "strerror of an unknown code");
    yqst_close(&g_q2);

    /* v0.6: a failing handle-changing call writes the message; close is safe
     * on a zeroed handle, after a failed open and twice. */
    memset(&g_q2, 0, sizeof(g_q2));
    CHECK(yqst_next(&g_q2) == YQST_ERR_CLOSED, "next on a zeroed handle");
    CHECK(strstr(yqst_error(&g_q2), "yqst_next") != NULL, "next message: %s", yqst_error(&g_q2));
    yqst_close(&g_q2);
    yqst_close(&g_q2);
    yqst_close(NULL);
    ref_desc(&d, false); d.n_stim = 0;
    CHECK(!yqst_open(&g_q2, &d), "open accepted n_stim = 0");
    yqst_close(&g_q2);
    yqst_close(&g_q2);
    ref_desc(&d, false);
    CHECK(yqst_open(&g_q2, &d), "open: %s", yqst_error(&g_q2));
    CHECK(yqst_error(&g_q2)[0] == '\0', "open left a message: %s", yqst_error(&g_q2));
    CHECK(yqst_update(&g_q2, 0, 7) == YQST_ERR_ARG, "update with outcome 7");
    CHECK(strstr(yqst_error(&g_q2), "yqst_update") != NULL, "update message: %s", yqst_error(&g_q2));
    CHECK(yqst_update_values(&g_q2, NULL, 0) == YQST_ERR_ARG, "update_values(NULL)");
    CHECK(strstr(yqst_error(&g_q2), "yqst_update_values") != NULL, "update_values message");
    CHECK(yqst_next_subset(&g_q2, NULL, 1) == YQST_ERR_ARG, "next_subset(NULL)");
    CHECK(strstr(yqst_error(&g_q2), "yqst_next_subset") != NULL, "next_subset message");
    yqst_close(&g_q2);
    yqst_close(&g_q2);
    CHECK(!yqst_is_open(&g_q2), "open after close");
}

/* ======================================================================= *
 *  GRID ARITHMETIC
 * ======================================================================= */

static void test_grids(void) {
    yqst_desc d;
    int sub[2] = {0}, flat, i;
    double sv[2] = {0};

    memset(&d, 0, sizeof(d));
    d.pf = YQST_PF_GUMBEL;
    d.stim[0] = yqst_linspace(-2.0, 0.0, 3);
    d.stim[1] = yqst_linspace(1.0, 4.0, 4);
    d.n_stim = 2;
    d.param[0] = yqst_values(ref_alpha, RA);
    d.param[1] = yqst_values(ref_beta, RB);
    d.param[2] = yqst_fixed(0.5);
    d.param[3] = yqst_fixed(0.02);
    d.n_param = 4;
    d.stop_trials = 5;
    CHECK(yqst_open(&g_q, &d), "open: %s", yqst_error(&g_q));
    CHECK(yqst_n_stim(&g_q) == 12, "S = %d", yqst_n_stim(&g_q));
    sub[0] = 2; sub[1] = 1;
    flat = yqst_stim_index(&g_q, sub);
    CHECK(flat == 2 * 4 + 1, "stim_index = %d", flat);       /* last axis fastest */
    CLOSE(yqst_stim_value(&g_q, flat, 0), 0.0, 1e-15, "axis 0 value");
    CLOSE(yqst_stim_value(&g_q, flat, 1), 2.0, 1e-15, "axis 1 value");
    CHECK(yqst_stim_values(&g_q, flat, sv) == 2, "stim_values count");
    CLOSE(sv[0], 0.0, 1e-15, "stim_values[0]");
    CLOSE(sv[1], 2.0, 1e-15, "stim_values[1]");
    sv[0] = -0.9; sv[1] = 2.2;
    CHECK(yqst_stim_nearest(&g_q, sv) == 1 * 4 + 1, "stim_nearest = %d",
          yqst_stim_nearest(&g_q, sv));
    for (i = 0; i < 12; i++) {
        double v[2] = {0};
        int s2[2] = {0};
        yqst_stim_values(&g_q, i, v);
        s2[0] = i / 4; s2[1] = i % 4;
        CHECK(yqst_stim_index(&g_q, s2) == i, "round trip at %d", i);
        CHECK(yqst_stim_nearest(&g_q, v) == i, "nearest of a grid point at %d", i);
    }
    sub[0] = 3;
    CHECK(yqst_stim_index(&g_q, sub) == YQST_ERR_ARG, "an out-of-range subscript");
    CHECK(yqst_stim_value(&g_q, 99, 0) != yqst_stim_value(&g_q, 99, 0), "value of a bad index");
    CHECK(yqst_stim_value(&g_q, 0, 9) != yqst_stim_value(&g_q, 0, 9), "value on a bad axis");
    {
        int p4[4] = {0};
        double pv[4] = {0};
        p4[0] = 1; p4[1] = 2; p4[2] = 0; p4[3] = 0;
        flat = yqst_param_index(&g_q, p4);
        CHECK(flat == (1 * RB + 2), "param_index = %d", flat);
        CHECK(yqst_param_values(&g_q, flat, pv) == 4, "param_values count");
        CLOSE(pv[0], ref_alpha[1], 1e-15, "param_values[0]");
        CLOSE(pv[1], ref_beta[2], 1e-15, "param_values[1]");
        CLOSE(yqst_param_value(&g_q, flat, 3), 0.02, 1e-15, "param_value of a fixed axis");
    }
    /* A built-in ignores the extra stimulus axes, so the selection is free to
     * use them; this is the configuration the manual warns about. */
    CHECK(yqst_next(&g_q) >= 0, "next on a two-axis stimulus grid");
    yqst_close(&g_q);

    /* A prior built by yqst_prior_normal is a Gaussian on the axis. */
    {
        yqst_axis ax = yqst_values(ref_alpha, RA);
        double w[RA] = {0}, sum = 0.0;
        yqst_desc pd;
        CHECK(yqst_prior_normal(&ax, -1.0, 0.5, w) == 0, "prior_normal failed");
        for (i = 0; i < RA; i++) {
            double z = (ref_alpha[i] + 1.0) / 0.5;
            CLOSE(w[i], exp(-0.5 * z * z), 1e-15, "prior weight %d", i);
        }
        CHECK(yqst_prior_normal(&ax, 0.0, 0.0, w) == YQST_ERR_ARG, "prior_normal accepted sd = 0");
        CHECK(yqst_prior_normal(NULL, 0.0, 1.0, w) == YQST_ERR_ARG, "prior_normal accepted a null axis");
        ref_desc(&pd, false);
        pd.param[0].prior = w;
        CHECK(yqst_open(&g_q, &pd), "open with a prior: %s", yqst_error(&g_q));
        for (i = 0; i < RA; i++) sum += w[i];
        {
            double marg[RA] = {0};
            yqst_marginal(&g_q, 0, marg);
            for (i = 0; i < RA; i++)
                CLOSE(marg[i], w[i] / sum, 1e-12, "the prior is not installed at cell %d", i);
        }
        yqst_close(&g_q);
    }
}

/* ======================================================================= *
 *  A SIMULATED PSI RUN
 * ======================================================================= */

#define SIM_REPS 20
#define SIM_TRIALS 100

static void test_simulated_run(void) {
    yqst_desc d;
    static double alpha_grid[41], beta_grid[15];
    double truth[4] = {0};
    double a_err = 0.0, a_bias = 0.0, b_err = 0.0, b_bias = 0.0, sd_sum = 0.0;
    int i, rep;

    for (i = 0; i < 41; i++) alpha_grid[i] = -3.0 + 3.0 * (double)i / 40.0;
    for (i = 0; i < 15; i++) beta_grid[i] = 0.5 + 5.5 * (double)i / 14.0;

    truth[0] = -1.72;   /* threshold, log10 contrast */
    truth[1] = 3.1;     /* slope                     */
    truth[2] = 0.5;     /* 2AFC guess rate           */
    truth[3] = 0.02;    /* lapse rate                */

    memset(&d, 0, sizeof(d));
    d.pf = YQST_PF_GUMBEL;
    d.stim[0] = yqst_linspace(-3.0, 0.0, 31);
    d.n_stim = 1;
    d.param[0] = yqst_values(alpha_grid, 41);
    d.param[1] = yqst_values(beta_grid, 15);
    d.param[2] = yqst_fixed(0.5);
    d.param[3] = yqst_fixed(0.02);
    d.n_param = 4;
    d.stop_trials = SIM_TRIALS;

    /* One PRNG stream across the replications, seeded once, so the whole
     * block is a function of the seed below and nothing else. */
    sm_seed(0x5EEDF00Dull);
    for (rep = 0; rep < SIM_REPS; rep++) {
        double est[4] = {0};
        CHECK(yqst_open(&g_q, &d), "open: %s", yqst_error(&g_q));
        while (!yqst_done(&g_q)) {
            int s = yqst_next(&g_q);
            int k;
            CHECK(s >= 0, "next returned %d", s);
            if (s < 0) break;
            k = yqst_simulate(&g_q, s, truth, sm_u());
            CHECK(k == 0 || k == 1, "simulate returned %d", k);
            CHECK(yqst_update(&g_q, s, k) == YQST_OK, "update");
        }
        CHECK(yqst_n_trials(&g_q) == SIM_TRIALS, "the run stopped at %d trials",
              yqst_n_trials(&g_q));
        CHECK(yqst_estimate(&g_q, YQST_EST_MEAN, est) == YQST_OK, "estimate");
        /* Per run, only a loose sanity bound: one run in twenty is allowed to
         * be unlucky, and the aggregate below is the real check. */
        CHECK(fabs(est[0] - truth[0]) < 0.4, "run %d: threshold %.4f, truth %.4f",
              rep, est[0], truth[0]);
        CHECK(yqst_sd(&g_q, 0) < 0.25, "run %d: threshold sd %.4f after %d trials",
              rep, yqst_sd(&g_q, 0), SIM_TRIALS);
        a_err += fabs(est[0] - truth[0]);
        a_bias += est[0] - truth[0];
        b_err += fabs(est[1] - truth[1]);
        b_bias += est[1] - truth[1];
        sd_sum += yqst_sd(&g_q, 0);
        yqst_close(&g_q);
    }
    a_err /= SIM_REPS; a_bias /= SIM_REPS; b_err /= SIM_REPS;
    b_bias /= SIM_REPS; sd_sum /= SIM_REPS;
    printf("  %d Psi runs of %d trials: threshold mean |error| %.4f, bias %+.4f, "
           "posterior sd %.4f; slope mean |error| %.3f, bias %+.3f\n",
           SIM_REPS, SIM_TRIALS, a_err, a_bias, sd_sum, b_err, b_bias);
    /* Stated tolerances: the threshold recovers to within 0.12 log10 units on
     * average, with a bias under a tenth of a log unit (the grid step is
     * 0.075), and the slope to within 1.5 of 3.1. */
    CHECK(a_err < 0.12, "mean absolute threshold error %.4f", a_err);
    CHECK(fabs(a_bias) < 0.10, "threshold bias %+.4f", a_bias);
    CHECK(b_err < 1.5, "mean absolute slope error %.3f", b_err);
    CHECK(fabs(b_bias) < 1.2, "slope bias %+.3f", b_bias);
    CHECK(sd_sum < 0.10, "mean posterior threshold sd %.4f", sd_sum);
}

/* ======================================================================= *
 *  THE REMAINING CORNERS
 * ======================================================================= */

/* A model that can only ever produce outcome 1. Outcome 0 is then impossible
 * under every parameter point, and no posterior can absorb it. */
static void impossible_pf(void* ctx, const double* stim, const double* params, double* p) {
    (void)ctx; (void)stim; (void)params;
    p[0] = 0.0;
    p[1] = 1.0;
}

static void test_extras(void) {
    yqst_desc d;
    static double jp[RP];
    double marg[RA] = {0}, sum = 0.0;
    int i;

    /* A joint prior that does not factor: put all the mass on two cells. */
    ref_desc(&d, false);
    for (i = 0; i < RP; i++) jp[i] = 0.0;
    jp[1] = 2.0;
    jp[RP - 1] = 1.0;
    d.joint_prior = jp;
    CHECK(yqst_open(&g_q, &d), "open with a joint prior: %s", yqst_error(&g_q));
    for (i = 0; i < RP; i++) {
        double want = (i == 1) ? (2.0 / 3.0) : ((i == RP - 1) ? (1.0 / 3.0) : 0.0);
        CLOSE(post_of(&g_q)[i], want, 1e-15, "joint prior cell %d", i);
    }
    CLOSE(yqst_entropy(&g_q), -(2.0 / 3.0) * log(2.0 / 3.0) / log(2.0)
                              - (1.0 / 3.0) * log(1.0 / 3.0) / log(2.0), 1e-12,
          "entropy of a two-point prior");
    /* A posterior with exact zeros must not produce a NaN anywhere. */
    for (i = 0; i < RS; i++)
        CHECK(yqst_expected_entropy(&g_q, i) == yqst_expected_entropy(&g_q, i),
              "expected entropy of stimulus %d is NaN with a sparse prior", i);
    CHECK(yqst_update(&g_q, 2, 1) == YQST_OK, "update with a sparse prior");
    yqst_marginal(&g_q, 0, marg);
    for (i = 0; i < RA; i++) sum += marg[i];
    CLOSE(sum, 1.0, 1e-12, "a sparse posterior is not normalized");
    yqst_close(&g_q);

    /* An outcome no parameter point can produce changes nothing. */
    ref_desc(&d, false);
    d.pf = YQST_PF_CUSTOM;
    d.pf_fn = impossible_pf;
    d.n_outcomes = 2;
    d.n_param = 1;
    d.param[0] = yqst_values(ref_alpha, RA);
    CHECK(yqst_open(&g_q, &d), "open: %s", yqst_error(&g_q));
    CHECK(yqst_update(&g_q, 0, 0) == YQST_ERR_ARG, "an impossible outcome was accepted");
    CHECK(yqst_n_trials(&g_q) == 0, "an impossible outcome was recorded");
    for (i = 0; i < RA; i++)
        CLOSE(post_of(&g_q)[i], 1.0 / RA, 1e-15,
              "an impossible outcome disturbed the posterior at %d", i);
    CHECK(yqst_update(&g_q, 0, 1) == YQST_OK, "the possible outcome was refused");
    {   /* Uninformative: every cell has the same likelihood, so nothing moves. */
        double sv[1] = {0};
        sv[0] = ref_x[0];
        CHECK(yqst_update_values(&g_q, sv, 0) == YQST_ERR_ARG,
              "an impossible off-grid outcome was accepted");
    }
    yqst_close(&g_q);

    /* TIE_NEAREST inside a caller's subset resolves within the subset. */
    {
        int list[3] = {0};
        flat_desc(&d);
        d.tiebreak = YQST_TIE_NEAREST;
        CHECK(yqst_open(&g_q, &d), "open: %s", yqst_error(&g_q));
        yqst_update(&g_q, 4, 1);
        list[0] = 1; list[1] = 3; list[2] = 0;
        CHECK(yqst_next_subset(&g_q, list, 3) == 3,
              "TIE_NEAREST in a subset picked %d, want 3", yqst_next_subset(&g_q, list, 3));
        /* The subset answer is the cached proposal. */
        CHECK(yqst_next(&g_q) == 3, "yqst_next did not repeat the subset proposal");
        yqst_close(&g_q);
    }

    /* yqst_p at parameter values between grid points. */
    ref_desc(&d, false);
    CHECK(yqst_open(&g_q, &d), "open: %s", yqst_error(&g_q));
    {
        double par[4] = {0}, p[2] = {0}, sv[1] = {0};
        par[0] = -1.234; par[1] = 2.345; par[2] = REF_GAMMA; par[3] = REF_LAMBDA;
        CHECK(yqst_p(&g_q, 3, par, p) == YQST_OK, "yqst_p off the parameter grid");
        CLOSE(p[1], ref_p1(ref_x[3], par[0], par[1]), 1e-12, "yqst_p off-grid value");
        sv[0] = -0.123;
        CHECK(yqst_p_values(&g_q, sv, par, p) == YQST_OK, "yqst_p_values off both grids");
        CLOSE(p[1], ref_p1(sv[0], par[0], par[1]), 1e-12, "yqst_p_values off-grid value");
        par[0] = (double)NAN;
        CHECK(yqst_p(&g_q, 3, par, p) == YQST_ERR_ARG, "a NaN parameter was accepted");
        CHECK(yqst_p(&g_q, RS, par, p) == YQST_ERR_ARG, "a bad stimulus index was accepted");
        CHECK(yqst_p(&g_q, 0, NULL, p) == YQST_ERR_ARG, "null params");
    }
    yqst_close(&g_q);
}

/* ======================================================================= *
 *  THE DECOMPOSED SCORE AGAINST THE DEFINITION
 *
 *  The table path does not compute the expected entropy the way the manual
 *  writes it. It uses
 *      E[H] = H(theta) - H(y | s) + sum_theta post(theta) h(s, theta)
 *  with h tabulated as an extra row at open. This checks the identity itself,
 *  separately from the float storage: the direct formula is evaluated here in
 *  double, from the very floats the header stored, so the only difference
 *  left is the decomposition and the rounding of h to float.
 * ======================================================================= */

static void test_decomposition(void) {
    yqst_desc d;
    double post[RP] = {0};
    double worst_table = 0.0, worst_lik = 0.0;
    int i, s, t, k;

    /* Table path: read the stored floats back and apply the definition. */
    ref_desc(&d, false);
    CHECK(yqst_open(&g_q, &d), "open: %s", yqst_error(&g_q));
    CHECK(g_q.rows == 3, "the table has %d rows, want K+1 = 3", g_q.rows);
    ref_prior(post);
    for (i = 0; i <= REF_SEQ_N; i++) {
        for (s = 0; s < RS; s++) {
            const float* base = g_q.table
                                + (size_t)s * (size_t)g_q.rows * (size_t)RP;
            double direct = 0.0, got, diff;
            for (k = 0; k < 2; k++) {
                double w[RP] = {0}, pk = 0.0, h = 0.0;
                for (t = 0; t < RP; t++) {
                    /* Read the table exactly as the header stores it, so what
                     * is left over is the decomposition and nothing else. */
                    w[t] = post_of(&g_q)[t]
                           * (double)base[(size_t)k * RP + (size_t)t];
                    pk += w[t];
                }
                if (pk <= 0.0) continue;
                for (t = 0; t < RP; t++) {
                    double qq = w[t] / pk;
                    if (qq > 0.0) h -= qq * log(qq) / log(2.0);
                }
                direct += pk * h;
            }
            got = yqst_expected_entropy(&g_q, s);
            diff = fabs(got - direct);
            if (diff > worst_table) worst_table = diff;
            CLOSE(got, direct, 1e-6,
                  "decomposed vs direct, stimulus %d after %d trials", s, i);
        }
        if (i == REF_SEQ_N) break;
        yqst_update(&g_q, ref_seq_s[i], ref_seq_k[i]);
        ref_update(post, ref_seq_s[i], ref_seq_k[i]);
    }
    yqst_close(&g_q);

    /* No-table path: the header uses the direct formula there, so the same
     * comparison the other way round, in double on both sides, isolates the
     * algebra from the float table entirely. */
    ref_desc(&d, true);
    CHECK(yqst_open(&g_q, &d), "open: %s", yqst_error(&g_q));
    CHECK(g_q.rows == 2, "no_table kept an entropy row");
    for (i = 0; i <= REF_SEQ_N; i++) {
        double hpost = 0.0;
        const double* hp = post_of(&g_q);
        for (t = 0; t < RP; t++) if (hp[t] > 0.0) hpost -= hp[t] * log(hp[t]) / log(2.0);
        for (s = 0; s < RS; s++) {
            /* The decomposition, written out here. */
            double hy = 0.0, hcond = 0.0, got, diff;
            double pk[2] = {0};
            for (k = 0; k < 2; k++) {
                pk[k] = 0.0;
                for (t = 0; t < RP; t++) pk[k] += hp[t] * ref_lik(s, t, k);
            }
            for (k = 0; k < 2; k++) if (pk[k] > 0.0) hy -= pk[k] * log(pk[k]) / log(2.0);
            for (t = 0; t < RP; t++) {
                double cell = 0.0;
                for (k = 0; k < 2; k++) {
                    double l = ref_lik(s, t, k);
                    if (l > 0.0) cell -= l * log(l) / log(2.0);
                }
                hcond += hp[t] * cell;
            }
            got = yqst_expected_entropy(&g_q, s);
            diff = fabs(got - (hpost - hy + hcond));
            if (diff > worst_lik) worst_lik = diff;
            CLOSE(got, hpost - hy + hcond, 1e-12,
                  "direct vs decomposed (no_table), stimulus %d after %d trials", s, i);
        }
        if (i == REF_SEQ_N) break;
        yqst_update(&g_q, ref_seq_s[i], ref_seq_k[i]);
    }
    yqst_close(&g_q);
    printf("  decomposition: worst |decomposed - direct| = %.3g through the float table, "
           "%.3g in double\n", worst_table, worst_lik);
}

/* ======================================================================= *
 *  A NUISANCE AXIS THAT IS NOT WHERE THE POSTERIOR WANTS IT
 *
 *  The posterior is stored with the nuisance axes first, so a desc that
 *  flags a middle axis makes the internal and the public orders differ and
 *  every boundary has to translate. Grid: alpha (4) x beta (3, NUISANCE) x
 *  gamma (1) x lambda (2), so the public flat index is
 *  t = ia*6 + ib*2 + il.
 * ======================================================================= */

#define PNA 4
#define PNB 3
#define PNL 2
#define PNP (PNA * PNB * PNL)

static const double pn_lapse[PNL] = { 0.01, 0.05 };

static double pn_lik(int s, int t, int k) {
    double p1 = ref_p1_lam(ref_x[s], ref_alpha[t / 6], ref_beta[(t / 2) % PNB],
                           pn_lapse[t % PNL]);
    return (k == 1) ? p1 : 1.0 - p1;
}

static void test_permuted_nuisance(bool no_table) {
    yqst_desc d;
    double post[PNP] = {0};
    double tol = no_table ? 1e-12 : 1e-6;
    int i, t, s, k, m;

    memset(&d, 0, sizeof(d));
    d.pf = YQST_PF_GUMBEL;
    d.stim[0] = yqst_values(ref_x, RS);
    d.n_stim = 1;
    d.param[0] = yqst_values(ref_alpha, RA);
    d.param[1] = yqst_values(ref_beta, RB);
    d.param[1].nuisance = true;          /* the middle axis, not the last */
    d.param[2] = yqst_fixed(REF_GAMMA);
    d.param[3] = yqst_values(pn_lapse, PNL);
    d.n_param = 4;
    d.stop_trials = 1000;
    d.no_table = no_table;
    CHECK(yqst_open(&g_q, &d), "open: %s", yqst_error(&g_q));
    CHECK(yqst_n_param(&g_q) == PNP, "P = %d, want %d", yqst_n_param(&g_q), PNP);
    CHECK(g_q.permuted, "the internal order did not change for a middle nuisance axis");
    CHECK(g_q.perm[0] == 1, "perm[0] = %d, want the nuisance axis", g_q.perm[0]);
    CHECK(g_q.nuis_block == PNB, "nuis_block = %d", g_q.nuis_block);
    CHECK(g_q.n_marg == PNA * PNL, "n_marg = %d", g_q.n_marg);

    for (t = 0; t < PNP; t++) post[t] = 1.0 / (double)PNP;
    for (i = 0; i <= REF_SEQ_N; i++) {
        const double* hp = post_of(&g_q);
        double marg[PNA * PNL] = {0}, h = 0.0;
        /* post_of() must come back in the CALLER's axis order. */
        for (t = 0; t < PNP; t++)
            CLOSE(hp[t], post[t], tol, "permuted posterior cell %d after %d trials", t, i);
        /* Marginal over the nuisance axis, in public terms. */
        for (m = 0; m < PNA * PNL; m++) {
            double sum = 0.0;
            int ia = m / PNL, il = m % PNL, ib;
            for (ib = 0; ib < PNB; ib++) sum += post[ia * 6 + ib * 2 + il];
            marg[m] = sum;
        }
        for (m = 0; m < PNA * PNL; m++)
            if (marg[m] > 0.0) h -= marg[m] * log(marg[m]) / log(2.0);
        CLOSE(yqst_entropy(&g_q), h, tol, "permuted marginal entropy after %d trials", i);

        /* Single-axis marginals and the estimators, all in public terms. */
        {
            double ma[RA] = {0}, mb[RB] = {0}, ml[PNL] = {0}, est[4] = {0}, mean_a = 0.0, mean_b = 0.0;
            int ia, ib, il, best = 0;
            CHECK(yqst_marginal(&g_q, 0, ma) == RA, "marginal 0");
            CHECK(yqst_marginal(&g_q, 1, mb) == RB, "marginal 1");
            CHECK(yqst_marginal(&g_q, 3, ml) == PNL, "marginal 3");
            for (ia = 0; ia < RA; ia++) {
                double sum = 0.0;
                for (ib = 0; ib < PNB; ib++)
                    for (il = 0; il < PNL; il++) sum += post[ia * 6 + ib * 2 + il];
                CLOSE(ma[ia], sum, tol, "permuted marginal of alpha at %d", ia);
                mean_a += sum * ref_alpha[ia];
            }
            for (ib = 0; ib < PNB; ib++) {
                double sum = 0.0;
                for (ia = 0; ia < RA; ia++)
                    for (il = 0; il < PNL; il++) sum += post[ia * 6 + ib * 2 + il];
                CLOSE(mb[ib], sum, tol, "permuted marginal of beta at %d", ib);
                mean_b += sum * ref_beta[ib];
            }
            for (il = 0; il < PNL; il++) {
                double sum = 0.0;
                for (ia = 0; ia < RA; ia++)
                    for (ib = 0; ib < PNB; ib++) sum += post[ia * 6 + ib * 2 + il];
                CLOSE(ml[il], sum, tol, "permuted marginal of lambda at %d", il);
            }
            CHECK(yqst_estimate(&g_q, YQST_EST_MEAN, est) == YQST_OK, "estimate");
            CLOSE(est[0], mean_a, tol, "permuted mean of alpha");
            CLOSE(est[1], mean_b, tol, "permuted mean of beta");
            CLOSE(est[2], REF_GAMMA, 1e-15, "permuted mean of a fixed axis");
            /* The mode is compared by mass, not by coordinates: this
             * posterior has exact ties (two slopes that no trial so far can
             * tell apart), and either cell is a mode. */
            for (t = 1; t < PNP; t++) if (post[t] > post[best]) best = t;
            CHECK(yqst_estimate(&g_q, YQST_EST_MODE, est) == YQST_OK, "estimate");
            {
                int ia2 = -1, ib2 = -1, il2 = -1, z;
                for (z = 0; z < RA; z++)   if (est[0] == ref_alpha[z]) ia2 = z;
                for (z = 0; z < PNB; z++)  if (est[1] == ref_beta[z])  ib2 = z;
                for (z = 0; z < PNL; z++)  if (est[3] == pn_lapse[z])  il2 = z;
                CHECK(ia2 >= 0 && ib2 >= 0 && il2 >= 0,
                      "the mode is not a grid point (%g, %g, %g)", est[0], est[1], est[3]);
                if (ia2 >= 0 && ib2 >= 0 && il2 >= 0)
                    CLOSE(post[ia2 * 6 + ib2 * 2 + il2], post[best], tol,
                          "the permuted mode is not at the maximum, trial %d", i);
            }
        }

        /* The marginalized selection score, from the definition. */
        for (s = 0; s < RS; s++) {
            double e = 0.0;
            for (k = 0; k < 2; k++) {
                double w[PNP] = {0}, pk = 0.0, hk = 0.0, mk[PNA * PNL] = {0};
                int ia, ib, il;
                for (t = 0; t < PNP; t++) { w[t] = post[t] * pn_lik(s, t, k); pk += w[t]; }
                for (m = 0; m < PNA * PNL; m++) {
                    double sum = 0.0;
                    ia = m / PNL; il = m % PNL;
                    for (ib = 0; ib < PNB; ib++) sum += w[ia * 6 + ib * 2 + il];
                    mk[m] = sum;
                }
                if (pk <= 0.0) continue;
                for (m = 0; m < PNA * PNL; m++) {
                    double qq = mk[m] / pk;
                    if (qq > 0.0) hk -= qq * log(qq) / log(2.0);
                }
                e += pk * hk;
            }
            CLOSE(yqst_expected_entropy(&g_q, s), e, tol,
                  "permuted marginalized score, stimulus %d, trial %d", s, i);
        }
        if (i == REF_SEQ_N) break;
        CHECK(yqst_update(&g_q, ref_seq_s[i], ref_seq_k[i]) == YQST_OK, "update %d", i);
        {
            double sum = 0.0;
            for (t = 0; t < PNP; t++) {
                post[t] *= pn_lik(ref_seq_s[i], t, ref_seq_k[i]);
                sum += post[t];
            }
            for (t = 0; t < PNP; t++) post[t] /= sum;
        }
    }
    yqst_close(&g_q);
}

/* ======================================================================= *
 *  THE BATCH PSYCHOMETRIC FUNCTION
 *
 *  What is under test is the plumbing, not a model: the transpose from the
 *  callback's cell-major output into the header's outcome-major rows, the
 *  order of the parameter matrix the callback is handed, the validation, and
 *  the arena. So every batch callback here is written as a loop over the
 *  per-cell callback beside it, which is what a binding's vectorized callable
 *  computes by other means. Identical doubles in, so the two paths must
 *  produce tables that are equal BIT FOR BIT.
 * ======================================================================= */

/* A four-parameter logistic, one cell at a time. */
static void pair_pf(void* ctx, const double* stim, const double* params, double* p) {
    double z = params[1] * (stim[0] - params[0]);
    (void)ctx;
    p[1] = params[2] + (1.0 - params[2] - params[3]) / (1.0 + exp(-z));
    p[0] = 1.0 - p[1];
}

static void pair_pf_batch(void* ctx, const double* stims, int S,
                          const double* params, int P, float* out) {
    int s, i, k;
    for (s = 0; s < S; s++) {
        for (i = 0; i < P; i++) {
            double p[2] = {0};
            pair_pf(ctx, stims + (size_t)s, params + (size_t)i * 4, p);
            for (k = 0; k < 2; k++)
                out[((size_t)s * (size_t)P + (size_t)i) * 2 + (size_t)k] = (float)p[k];
        }
    }
}

/* Three outcomes, to be sure the transpose is not a K = 2 accident. */
static void three_pf_batch(void* ctx, const double* stims, int S,
                           const double* params, int P, float* out) {
    int s, i, k;
    for (s = 0; s < S; s++) {
        for (i = 0; i < P; i++) {
            double p[3] = {0};
            three_pf(ctx, stims + (size_t)s, params + (size_t)i * 3, p);
            for (k = 0; k < 3; k++)
                out[((size_t)s * (size_t)P + (size_t)i) * 3 + (size_t)k] = (float)p[k];
        }
    }
}

/* Outcomes that do not sum to 1, for the rejection path. */
static void bad_pf_batch(void* ctx, const double* stims, int S,
                         const double* params, int P, float* out) {
    int s, i;
    (void)ctx; (void)stims; (void)params;
    for (s = 0; s < S; s++)
        for (i = 0; i < P; i++) {
            out[((size_t)s * (size_t)P + (size_t)i) * 2 + 0] = 0.4f;
            out[((size_t)s * (size_t)P + (size_t)i) * 2 + 1] = 0.4f;
        }
}

static void pair_desc(yqst_desc* d, bool batch, bool no_table, bool nuisance) {
    memset(d, 0, sizeof(*d));
    d->pf = YQST_PF_CUSTOM;
    if (batch) d->pf_batch = pair_pf_batch;
    else       d->pf_fn = pair_pf;
    d->n_outcomes = 2;
    d->stim[0] = yqst_values(ref_x, RS);
    d->n_stim = 1;
    d->param[0] = yqst_values(ref_alpha, RA);
    d->param[1] = yqst_values(ref_beta, RB);
    d->param[1].nuisance = nuisance;   /* the middle axis: forces a permutation */
    d->param[2] = yqst_fixed(0.5);
    d->param[3] = yqst_values(pn_lapse, PNL);
    d->n_param = 4;
    d->stop_trials = 1000;
    d->no_table = no_table;
}

static void test_batch_pf(void) {
    yqst_desc dc, db;
    int i, pass;

    /* Once with the caller's axis order intact, once with a nuisance axis in
     * the middle so the header permutes the parameter grid under the
     * callback. If the matrix rows and the output rows ever disagreed, the
     * second pass would catch it. */
    for (pass = 0; pass < 2; pass++) {
        bool nuisance = (pass == 1);
        size_t floats;
        pair_desc(&dc, false, false, nuisance);
        pair_desc(&db, true, false, nuisance);
        CHECK(yqst_open(&g_q, &dc), "per-cell open: %s", yqst_error(&g_q));
        CHECK(yqst_open(&g_q2, &db), "batch open: %s", yqst_error(&g_q2));
        CHECK(yqst_memory_size(&db) > yqst_memory_size(&dc),
              "the batch desc does not ask for more memory (%llu vs %llu)",
              (unsigned long long)yqst_memory_size(&db),
              (unsigned long long)yqst_memory_size(&dc));
        CHECK(g_q2.param_matrix != NULL, "no parameter matrix for the batch path");
        CHECK(g_q.param_matrix == NULL, "the per-cell path allocated a parameter matrix");
        CHECK(g_q2.permuted == nuisance, "pass %d: permuted = %d", pass, (int)g_q2.permuted);

        /* The tables must be equal bit for bit, including the entropy row. */
        floats = (size_t)g_q.S * (size_t)g_q.rows * (size_t)g_q.P;
        CHECK(g_q.rows == g_q2.rows, "rows differ: %d vs %d", g_q.rows, g_q2.rows);
        CHECK(memcmp(g_q.table, g_q2.table, floats * sizeof(float)) == 0,
              "pass %d: the batch table is not bit for bit the per-cell table", pass);

        /* Drive both and require identical selections and identical posteriors. */
        for (i = 0; i < REF_SEQ_N; i++) {
            int a = yqst_next(&g_q), b = yqst_next(&g_q2);
            CHECK(a == b, "pass %d trial %d: per-cell picked %d, batch picked %d", pass, i, a, b);
            CLOSE(yqst_expected_entropy(&g_q, a), yqst_expected_entropy(&g_q2, b), 0.0,
                  "pass %d trial %d: the scores differ", pass, i);
            yqst_update(&g_q, a, ref_seq_k[i]);
            yqst_update(&g_q2, b, ref_seq_k[i]);
        }
        CHECK(memcmp(post_of(&g_q), post_of(&g_q2),
                     (size_t)g_q.P * sizeof(double)) == 0,
              "pass %d: the posteriors differ", pass);
        CLOSE(yqst_entropy(&g_q), yqst_entropy(&g_q2), 0.0, "pass %d: entropy", pass);

        /* Off-grid: the batch path goes through the same callback with S = 1
         * and every parameter point, but its likelihoods come back as floats,
         * so this one agrees to float precision and not exactly. */
        {
            double sv[1] = {0};
            double e1[4] = {0}, e2[4] = {0};
            sv[0] = 0.5 * (ref_x[1] + ref_x[2]);
            CHECK(yqst_update_values(&g_q, sv, 1) == YQST_OK, "per-cell off-grid update");
            CHECK(yqst_update_values(&g_q2, sv, 1) == YQST_OK, "batch off-grid update");
            CHECK(yqst_estimate(&g_q, YQST_EST_MEAN, e1) == YQST_OK, "estimate");
            CHECK(yqst_estimate(&g_q2, YQST_EST_MEAN, e2) == YQST_OK, "estimate");
            for (i = 0; i < 4; i++)
                CLOSE(e1[i], e2[i], 1e-6, "pass %d: estimate %d after an off-grid update", pass, i);
        }

        /* One cell through the public model calls, also float-rounded. */
        {
            double par[4] = {0}, p1[2] = {0}, p2[2] = {0}, sv[1] = {0};
            par[0] = -1.1; par[1] = 2.2; par[2] = 0.5; par[3] = 0.03;
            sv[0] = -0.75;
            CHECK(yqst_p_values(&g_q, sv, par, p1) == YQST_OK, "yqst_p_values per-cell");
            CHECK(yqst_p_values(&g_q2, sv, par, p2) == YQST_OK, "yqst_p_values batch");
            CLOSE(p1[1], p2[1], 1e-6, "pass %d: yqst_p_values", pass);
            CLOSE(p2[0] + p2[1], 1.0, 1e-6, "pass %d: batch outcomes do not sum to 1", pass);
            CHECK(yqst_simulate(&g_q2, 2, par, 0.5) >= 0, "simulate through the batch path");
        }
        yqst_close(&g_q);
        yqst_close(&g_q2);
    }

    /* no_table with a batch callback: allowed, and the same answers. */
    pair_desc(&dc, false, true, true);
    pair_desc(&db, true, true, true);
    CHECK(yqst_open(&g_q, &dc), "per-cell no_table open: %s", yqst_error(&g_q));
    CHECK(yqst_open(&g_q2, &db), "batch no_table open: %s", yqst_error(&g_q2));
    CHECK(g_q2.table == NULL, "no_table kept a table");
    for (i = 0; i < REF_SEQ_N; i++) {
        int a = yqst_next(&g_q), b = yqst_next(&g_q2);
        CHECK(a == b, "no_table trial %d: per-cell %d, batch %d", i, a, b);
        CLOSE(yqst_expected_entropy(&g_q, a), yqst_expected_entropy(&g_q2, b), 1e-6,
              "no_table trial %d: the scores differ", i);
        yqst_update(&g_q, a, ref_seq_k[i]);
        yqst_update(&g_q2, b, ref_seq_k[i]);
    }
    for (i = 0; i < yqst_n_param(&g_q); i++)
        CLOSE(post_of(&g_q)[i], post_of(&g_q2)[i], 1e-6,
              "no_table batch posterior cell %d", i);
    yqst_close(&g_q);
    yqst_close(&g_q2);

    /* Three outcomes through the batch path, against the per-cell twin. */
    {
        static const double pse[4] = { -0.6, -0.2, 0.2, 0.6 };
        static const double sig[3] = { 0.4, 0.8, 1.6 };
        static const double crit[2] = { 0.3, 0.9 };
        size_t floats;
        memset(&dc, 0, sizeof(dc));
        dc.pf = YQST_PF_CUSTOM;
        dc.n_outcomes = 3;
        dc.stim[0] = yqst_linspace(-2.0, 2.0, 7);
        dc.n_stim = 1;
        dc.param[0] = yqst_values(pse, 4);
        dc.param[1] = yqst_values(sig, 3);
        dc.param[2] = yqst_values(crit, 2);
        dc.n_param = 3;
        dc.stop_trials = 20;
        db = dc;
        dc.pf_fn = three_pf;
        db.pf_batch = three_pf_batch;
        CHECK(yqst_open(&g_q, &dc), "K=3 per-cell open: %s", yqst_error(&g_q));
        CHECK(yqst_open(&g_q2, &db), "K=3 batch open: %s", yqst_error(&g_q2));
        floats = (size_t)g_q.S * (size_t)g_q.rows * (size_t)g_q.P;
        CHECK(memcmp(g_q.table, g_q2.table, floats * sizeof(float)) == 0,
              "the K=3 batch table is not bit for bit the per-cell table");
        for (i = 0; i < 6; i++) {
            int a = yqst_next(&g_q), b = yqst_next(&g_q2);
            CHECK(a == b, "K=3 trial %d: per-cell %d, batch %d", i, a, b);
            yqst_update(&g_q, a, i % 3);
            yqst_update(&g_q2, b, i % 3);
        }
        CHECK(memcmp(post_of(&g_q), post_of(&g_q2),
                     (size_t)g_q.P * sizeof(double)) == 0, "the K=3 posteriors differ");
        yqst_close(&g_q);
        yqst_close(&g_q2);
    }

    /* The arena, for a batch desc: exactly memory_size opens, one byte fewer
     * does not, and nothing past it is touched. */
    {
        size_t need;
        unsigned char* buf;
        const size_t guard = 32;
        size_t z;
        pair_desc(&db, true, false, true);
        need = yqst_memory_size(&db);
        buf = (unsigned char*)malloc(need + guard);
        CHECK(buf != NULL, "malloc");
        if (buf) {
            memset(buf, 0x5A, need + guard);
            db.memory = buf;
            db.memory_size = need - 1;
            CHECK(!yqst_open(&g_q, &db), "batch open succeeded one byte short");
            db.memory_size = need;
            CHECK(yqst_open(&g_q, &db), "batch open at exactly memory_size: %s", yqst_error(&g_q));
            yqst_next(&g_q);
            yqst_update(&g_q, 2, 1);
            yqst_next(&g_q);
            for (z = 0; z < guard; z++)
                CHECK(buf[need + z] == 0x5A, "batch open wrote past memory_size");
            yqst_close(&g_q);
            free(buf);
        }
    }

    /* Rejections. */
    pair_desc(&dc, false, false, false);
    dc.pf_fn = NULL;
    reject(&dc, "YQST_PF_CUSTOM with neither pf_fn nor pf_batch");
    pair_desc(&dc, false, false, false);
    dc.pf_batch = pair_pf_batch;
    reject(&dc, "YQST_PF_CUSTOM with both pf_fn and pf_batch");
    {
        bool ok;
        pair_desc(&dc, true, false, false);
        dc.pf_batch = bad_pf_batch;
        ok = yqst_open(&g_q, &dc);
        CHECK(!ok, "open accepted a pf_batch whose outcomes sum to 0.8");
        CHECK(strstr(yqst_error(&g_q), "pf_batch") != NULL,
              "the message does not name pf_batch: %s", yqst_error(&g_q));
        if (ok) yqst_close(&g_q);
        dc.no_table = true;
        ok = yqst_open(&g_q, &dc);
        CHECK(!ok, "open accepted a bad pf_batch under no_table");
        CHECK(strstr(yqst_error(&g_q), "pf_batch") != NULL,
              "the no_table message does not name pf_batch: %s", yqst_error(&g_q));
        if (ok) yqst_close(&g_q);
    }
    /* A built-in ignores both callbacks rather than tripping over them. */
    {
        yqst_desc d;
        ref_desc(&d, false);
        d.pf_batch = bad_pf_batch;
        CHECK(yqst_open(&g_q, &d), "a built-in with a stray pf_batch: %s", yqst_error(&g_q));
        CHECK(g_q.desc.pf_batch == NULL, "the stray pf_batch was kept");
        CHECK(g_q.param_matrix == NULL, "the stray pf_batch took memory");
        {
            double par[4] = {0}, p[2] = {0};
            par[0] = -1.5; par[1] = 3.0; par[2] = REF_GAMMA; par[3] = REF_LAMBDA;
            CHECK(yqst_p(&g_q, 2, par, p) == YQST_OK, "yqst_p");
            CLOSE(p[1], ref_p1(ref_x[2], par[0], par[1]), 1e-12,
                  "the stray pf_batch changed the built-in");
        }
        yqst_close(&g_q);
    }
}

/* ======================================================================= *
 *  THE ASYNC LAYER
 *
 *  Only under YQST_ASYNC, and the test is built both ways. The claim under
 *  test is that the layer changes nothing about the inference: the same
 *  responses in the same order, driven through the thread, must leave the same
 *  posterior BIT FOR BIT as a synchronous run, because it is the same code on
 *  the same handle. The rest is queue behavior.
 * ======================================================================= */
#ifdef YQST_ASYNC

/* Zeroed starting points for the structs the async calls fill. yqst_async_poll()
 * and yqst_async_wait() write a snapshot only when they succeed, and gcc 16 at
 * -O3 cannot see that the checks read it only then, so every local starts as a
 * copy of these and a read on a failure path reads zeros, never an
 * indeterminate value. Static and without an initializer, which zeroes them in
 * C and in C++ alike (a const one would need an initializer in C++). */
static yqst_snapshot   g_snap_zero;
static yqst_async_desc g_ad_zero;

/* A grid big enough that one update plus one selection takes a while (about a
 * millisecond here), so the queue-full and not-ready-yet cases can actually be
 * reached from a submit loop. */
static double as_alpha[61], as_beta[12], as_guess[5], as_lapse[5];
static double as_stim[31];

static void async_axes(void) {
    int i;
    for (i = 0; i < 31; i++) as_stim[i]  = -3.0 + 3.0 * (double)i / 30.0;
    for (i = 0; i < 61; i++) as_alpha[i] = -3.0 + 3.0 * (double)i / 60.0;
    for (i = 0; i < 12; i++) as_beta[i]  = 0.5 + 5.5 * (double)i / 11.0;
    for (i = 0; i < 5; i++)  as_guess[i] = 0.45 + 0.10 * (double)i / 4.0;
    for (i = 0; i < 5; i++)  as_lapse[i] = 0.06 * (double)i / 4.0;
}

static void async_desc_of(yqst_desc* d, bool big) {
    memset(d, 0, sizeof(*d));
    d->pf = YQST_PF_GUMBEL;
    d->stim[0] = big ? yqst_values(as_stim, 31) : yqst_values(ref_x, RS);
    d->n_stim = 1;
    d->param[0] = big ? yqst_values(as_alpha, 61) : yqst_values(ref_alpha, RA);
    d->param[1] = big ? yqst_values(as_beta, 12)  : yqst_values(ref_beta, RB);
    if (big) {
        d->param[2] = yqst_values(as_guess, 5);
        d->param[3] = yqst_values(as_lapse, 5);
        d->param[2].nuisance = true;
        d->param[3].nuisance = true;
    } else {
        d->param[2] = yqst_fixed(REF_GAMMA);
        d->param[3] = yqst_fixed(REF_LAMBDA);
    }
    d->n_param = 4;
    d->stop_trials = 10000;
}

static yqst_async g_async;

/* An async run against a synchronous replay of the same responses. */
static void test_async_matches(void) {
    yqst_desc d;
    yqst_async_desc ad = g_ad_zero;
    yqst_snapshot snap = g_snap_zero;
    int outcomes[12] = {0}, proposals[12] = {0};
    int i, seq, rc;

    async_desc_of(&d, false);
    CHECK(yqst_open(&g_q, &d), "async: open: %s", yqst_error(&g_q));
    memset(&g_async, 0, sizeof(g_async));
    memset(&ad, 0, sizeof(ad));
    ad.quest = &g_q;
    ad.estimator = YQST_EST_MEAN;
    CHECK(yqst_async_open(&g_async, &ad), "async start: %s", yqst_async_error(&g_async));
    CHECK(yqst_async_is_running(&g_async), "async is not running after start");

    /* The proposal made at start: poll returns 0 and the snapshot is valid. */
    rc = yqst_async_poll(&g_async, &snap);
    CHECK(rc == 0, "the initial poll returned %d, want 0", rc);
    CHECK(snap.seq == 0, "initial snapshot seq = %u", snap.seq);
    CHECK(snap.proposed >= 0 && snap.proposed < RS, "initial proposal %d", snap.proposed);
    CHECK(snap.n_trials == 0, "initial n_trials = %d", snap.n_trials);
    CLOSE(snap.stim[0], ref_x[snap.proposed], 0.0, "initial snapshot stimulus value");

    /* One trial at a time, waiting for each: that makes the run
     * deterministic, which is what lets the replay be compared bit for bit. */
    for (i = 0; i < 12; i++) {
        proposals[i] = snap.proposed;
        outcomes[i] = (i * 5 + 1) % 3 ? 1 : 0;
        seq = yqst_async_submit(&g_async, proposals[i], outcomes[i]);
        CHECK(seq == i + 1, "submit %d returned %d", i, seq);
        rc = yqst_async_wait(&g_async, (uint32_t)seq, YRT_S(30), &snap);
        CHECK(rc >= seq, "wait for seq %d returned %d", seq, rc);
        CHECK(snap.seq == (uint32_t)seq, "snapshot seq %u after waiting for %d",
              snap.seq, seq);
        CHECK(snap.n_trials == i + 1, "snapshot n_trials %d at trial %d", snap.n_trials, i);
        CHECK(snap.update_rc == YQST_OK, "the thread's update returned %d", snap.update_rc);
        CHECK(snap.proposed >= 0, "proposal %d at trial %d", snap.proposed, i);
        CHECK(yqst_async_pending(&g_async) == 0, "pending after a wait");
    }
    yqst_async_close(&g_async);
    CHECK(!yqst_async_is_running(&g_async), "still running after stop");

    /* The handle is the caller's again. */
    CHECK(yqst_n_trials(&g_q) == 12, "n_trials after the async run = %d",
          yqst_n_trials(&g_q));

    /* The same responses, synchronously, on a second handle. */
    CHECK(yqst_open(&g_q2, &d), "sync open: %s", yqst_error(&g_q2));
    for (i = 0; i < 12; i++) {
        int s = yqst_next(&g_q2);
        CHECK(s == proposals[i], "trial %d: sync proposed %d, async proposed %d",
              i, s, proposals[i]);
        CHECK(yqst_update(&g_q2, s, outcomes[i]) == YQST_OK, "sync update %d", i);
    }
    CHECK(memcmp(post_of(&g_q), post_of(&g_q2),
                 (size_t)yqst_n_param(&g_q) * sizeof(double)) == 0,
          "the async posterior is not bit for bit the synchronous one");
    CHECK(yqst_next(&g_q) == yqst_next(&g_q2), "the next proposals differ");
    {
        double ea[4] = {0}, eb[4] = {0};
        yqst_estimate(&g_q, YQST_EST_MEAN, ea);
        yqst_estimate(&g_q2, YQST_EST_MEAN, eb);
        for (i = 0; i < 4; i++)
            CLOSE(ea[i], eb[i], 0.0, "estimate %d differs between async and sync", i);
        /* And the snapshot the thread published last says the same thing. */
        for (i = 0; i < 4; i++)
            CLOSE(snap.estimate[i], ea[i], 0.0, "snapshot estimate %d", i);
        CLOSE(snap.entropy, yqst_entropy(&g_q), 0.0, "snapshot entropy");
        CLOSE(snap.sd, yqst_sd(&g_q, 0), 0.0, "snapshot sd");
    }
    yqst_close(&g_q);
    yqst_close(&g_q2);
}

/* The queue: a full one is reported and loses nothing, a poll before the
 * thread is finished still answers with the previous seq, and a stop drains. */
static void test_async_queue(void) {
    yqst_desc d;
    yqst_async_desc ad = g_ad_zero;
    yqst_snapshot snap = g_snap_zero;
    int acc_stim[256] = {0}, acc_out[256] = {0};
    int i, n_acc = 0, busy = 0, last_seq = 0, rc, prev;

    async_axes();
    async_desc_of(&d, true);
    CHECK(yqst_open(&g_q, &d), "async big open: %s", yqst_error(&g_q));
    memset(&g_async, 0, sizeof(g_async));
    memset(&ad, 0, sizeof(ad));
    ad.quest = &g_q;
    ad.below_normal = true;      /* exercise the priority path too */
    CHECK(yqst_async_open(&g_async, &ad), "async start: %s", yqst_async_error(&g_async));
    prev = yqst_async_poll(&g_async, &snap);
    CHECK(prev == 0, "initial poll on the big grid returned %d", prev);

    /* Submit as fast as the loop can. The thread needs about a millisecond per
     * response, so the queue fills; every submit that is accepted is recorded
     * so the replay below knows exactly what the thread was given. */
    for (i = 0; i < 64 && busy < 3; i++) {
        int s = (i * 7) % 31, o = (i % 4) ? 1 : 0;
        rc = yqst_async_submit(&g_async, s, o);
        if (rc == YQST_ERR_BUSY) {
            busy++;
            /* A full queue must not have consumed the response: the caller
             * keeps it and retries, which is what the next iteration does with
             * the same i, so nothing is lost. Give the thread a moment. */
            /* Generous on purpose: this is a correctness test, and under
             * ThreadSanitizer with the thread below normal priority a drain of
             * the whole queue has been measured past two seconds. A hang would
             * still show as a timeout. */
            rc = yqst_async_wait(&g_async, (uint32_t)(last_seq), YRT_S(30), &snap);
            CHECK(rc >= 0, "wait after a full queue returned %d", rc);
            i--;
            continue;
        }
        CHECK(rc > 0, "submit %d returned %d", i, rc);
        if (rc <= 0) break;
        CHECK(rc == last_seq + 1, "submit returned %d after %d", rc, last_seq);
        last_seq = rc;
        acc_stim[n_acc] = s;
        acc_out[n_acc] = o;
        n_acc++;
        /* A poll in the middle of the run: whatever it returns must be a seq
         * the thread really has finished, never ahead of what was submitted,
         * and never behind what a previous poll reported. */
        rc = yqst_async_poll(&g_async, &snap);
        CHECK(rc >= prev, "poll went backwards: %d after %d", rc, prev);
        CHECK(rc <= last_seq, "poll returned %d, beyond the submitted %d", rc, last_seq);
        CHECK(snap.seq == (uint32_t)rc, "poll's seq and snapshot disagree");
        CHECK(snap.n_trials == rc, "snapshot n_trials %d at seq %d", snap.n_trials, rc);
        prev = rc;
    }
    CHECK(busy > 0, "the queue never filled: %d submits accepted, %d rejected",
          n_acc, busy);
    CHECK(n_acc > YQST_ASYNC_QUEUE, "only %d submits were accepted", n_acc);

    /* Stop drains: every accepted response must have been applied. */
    yqst_async_close(&g_async);
    CHECK(yqst_n_trials(&g_q) == n_acc,
          "stop did not drain: %d trials applied of %d accepted",
          yqst_n_trials(&g_q), n_acc);

    /* And the replay of exactly those responses matches, bit for bit. */
    CHECK(yqst_open(&g_q2, &d), "replay open: %s", yqst_error(&g_q2));
    for (i = 0; i < n_acc; i++)
        CHECK(yqst_update(&g_q2, acc_stim[i], acc_out[i]) == YQST_OK, "replay update %d", i);
    CHECK(memcmp(post_of(&g_q), post_of(&g_q2),
                 (size_t)yqst_n_param(&g_q) * sizeof(double)) == 0,
          "the drained async posterior is not the replay's");
    yqst_close(&g_q);
    yqst_close(&g_q2);
}

/* Off-grid submits, and the calls that should refuse. */
static void test_async_edges(void) {
    yqst_desc d;
    yqst_async_desc ad = g_ad_zero;
    yqst_snapshot snap = g_snap_zero;
    double sv[1] = {0};
    int i, seq, rc;

    /* Every call on a zeroed handle. */
    memset(&g_async, 0, sizeof(g_async));
    CHECK(!yqst_async_is_running(&g_async), "a zeroed handle claims to run");
    CHECK(yqst_async_submit(&g_async, 0, 0) == YQST_ERR_ARG, "submit on a zeroed handle");
    CHECK(yqst_async_pending(&g_async) == YQST_ERR_CLOSED, "pending on a zeroed handle");
    CHECK(yqst_async_poll(&g_async, NULL) == YQST_ERR_CLOSED, "poll on a zeroed handle");
    CHECK(yqst_async_wait(&g_async, 1, 0, NULL) == YQST_ERR_CLOSED, "wait on a zeroed handle");
    CHECK(yqst_async_error(&g_async)[0] == '\0', "a zeroed handle has a message");
    yqst_async_close(&g_async);   /* must be safe */

    /* Descs start() refuses. */
    CHECK(!yqst_async_open(&g_async, NULL), "start accepted a null desc");
    CHECK(yqst_async_error(&g_async)[0] != '\0', "no message for a null desc");
    memset(&ad, 0, sizeof(ad));
    CHECK(!yqst_async_open(&g_async, &ad), "start accepted a desc without a quest");
    memset(&g_q, 0, sizeof(g_q));
    ad.quest = &g_q;
    CHECK(!yqst_async_open(&g_async, &ad), "start accepted a closed quest");
    CHECK(strstr(yqst_async_error(&g_async), "not open") != NULL,
          "message: %s", yqst_async_error(&g_async));

    async_desc_of(&d, false);
    CHECK(yqst_open(&g_q, &d), "open: %s", yqst_error(&g_q));
    ad.quest = &g_q;
    ad.estimator = (yqst_estimator)99;
    CHECK(!yqst_async_open(&g_async, &ad), "start accepted a bad estimator");
    ad.estimator = YQST_EST_MODE;
    CHECK(yqst_async_open(&g_async, &ad), "start: %s", yqst_async_error(&g_async));

    /* Arguments the submit path rejects without troubling the thread. */
    CHECK(yqst_async_submit(&g_async, RS, 0) == YQST_ERR_ARG, "a stimulus index past the grid");
    CHECK(yqst_async_submit(&g_async, -1, 0) == YQST_ERR_ARG, "a negative stimulus index");
    CHECK(yqst_async_submit(&g_async, 0, 2) == YQST_ERR_ARG, "an outcome >= K");
    CHECK(yqst_async_submit_values(&g_async, NULL, 1) == YQST_ERR_ARG, "null values");
    sv[0] = (double)NAN;
    CHECK(yqst_async_submit_values(&g_async, sv, 1) == YQST_ERR_ARG, "a NaN value");

    /* An off-grid trial through the thread, against the synchronous form. */
    sv[0] = 0.5 * (ref_x[1] + ref_x[2]);
    seq = yqst_async_submit_values(&g_async, sv, 1);
    CHECK(seq > 0, "submit_values returned %d", seq);
    rc = yqst_async_wait(&g_async, (uint32_t)seq, YRT_S(30), &snap);
    CHECK(rc == seq, "wait returned %d for seq %d", rc, seq);
    CHECK(snap.n_trials == 1, "n_trials after an off-grid submit = %d", snap.n_trials);
    /* YQST_EST_MODE was asked for, so the snapshot carries grid points. */
    CHECK(snap.estimate[0] == ref_alpha[0] || snap.estimate[0] == ref_alpha[1] ||
          snap.estimate[0] == ref_alpha[2] || snap.estimate[0] == ref_alpha[3],
          "the mode estimate %g is not a grid point", snap.estimate[0]);
    CHECK(yqst_async_wait(&g_async, (uint32_t)seq + 5, YRT_MS(1), NULL) == YQST_ERR_TIMEOUT,
          "a wait for a seq nobody submitted did not time out");
    yqst_async_close(&g_async);
    CHECK(yqst_async_submit(&g_async, 0, 1) == YQST_ERR_CLOSED, "submit after stop");
    CHECK(yqst_async_poll(&g_async, &snap) >= 0,
          "poll after stop should still hand back the last snapshot");
    {
        int n = -1;
        const yqst_trial* h = hist_of(&g_q, &n);
        CHECK(n == 1, "history after the async off-grid trial = %d", n);
        CHECK(h[0].stim_index == -1, "the off-grid trial kept a grid index");
        CLOSE(h[0].stim[0], sv[0], 0.0, "the off-grid stimulus value");
    }
    yqst_close(&g_q);
    yqst_async_close(&g_async);   /* twice must be safe */
    for (i = 0; i < 2; i++) yqst_async_close(&g_async);
    CHECK(strcmp(yqst_strerror(YQST_ERR_BUSY), "queue is full") == 0, "strerror BUSY");
    CHECK(strcmp(yqst_strerror(YQST_ERR_TIMEOUT), "timed out") == 0, "strerror TIMEOUT");
}

/* queue_depth is the session's policy under the YQST_ASYNC_QUEUE capacity. */
static void test_async_depth(void) {
    yqst_desc d;
    yqst_async_desc ad = g_ad_zero;
    yqst_snapshot snap = g_snap_zero;
    int i, busy = 0, accepted = 0, last = 0, rc;

    async_axes();
    async_desc_of(&d, true);
    CHECK(yqst_open(&g_q, &d), "open: %s", yqst_error(&g_q));
    memset(&g_async, 0, sizeof(g_async));
    memset(&ad, 0, sizeof(ad));
    ad.quest = &g_q;
    ad.queue_depth = YQST_ASYNC_QUEUE + 1;
    CHECK(!yqst_async_open(&g_async, &ad), "start accepted a depth past the capacity");
    CHECK(strstr(yqst_async_error(&g_async), "queue_depth") != NULL,
          "message: %s", yqst_async_error(&g_async));
    ad.queue_depth = -1;
    CHECK(!yqst_async_open(&g_async, &ad), "start accepted a negative depth");

    /* Depth 1: lockstep. On a grid that takes about a millisecond per trial, a
     * second submit right behind the first finds the only slot taken. */
    ad.queue_depth = 1;
    CHECK(yqst_async_open(&g_async, &ad), "start: %s", yqst_async_error(&g_async));
    for (i = 0; i < 6; i++) {
        rc = yqst_async_submit(&g_async, (i * 5) % 31, i % 2);
        if (rc == YQST_ERR_BUSY) { busy++; continue; }
        CHECK(rc > 0, "submit returned %d", rc);
        if (rc > 0) { accepted++; last = rc; }
        CHECK(yqst_async_pending(&g_async) <= 1, "depth 1 queued %d",
              yqst_async_pending(&g_async));
    }
    CHECK(busy > 0, "depth 1 never pushed back in 6 back-to-back submits");
    rc = yqst_async_wait(&g_async, (uint32_t)last, YRT_S(30), &snap);
    CHECK(rc == last, "wait returned %d, want %d", rc, last);
    yqst_async_close(&g_async);
    CHECK(yqst_n_trials(&g_q) == accepted, "applied %d of %d accepted",
          yqst_n_trials(&g_q), accepted);
    yqst_close(&g_q);
}

#endif /* YQST_ASYNC */

/* ======================================================================= *
 *  SNAPSHOTS
 *
 *  A session saved at trial c and resumed must be the uninterrupted session,
 *  bit for bit: same proposals, same posterior, same history. Every
 *  configuration is cut at several points, and twice at each: once between
 *  an update and the next selection, and once between a selection and its
 *  update, where the proposal cache is live and the header's generator has
 *  already been drawn from. Both generators are the caller's, so the test
 *  saves their states beside the snapshot exactly as a caller must.
 * ======================================================================= */

#define SNAP_N 24

static double sm_next(uint64_t* st) {
    uint64_t z = (*st += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z = z ^ (z >> 31);
    return (double)(z >> 11) * (1.0 / 9007199254740992.0);
}

static double snap_rng_fn(void* ctx) { return sm_next((uint64_t*)ctx); }

static const char* snap_name(int config) {
    switch (config) {
    case 0:  return "joint";
    case 1:  return "nuisance in the middle (permuted)";
    case 2:  return "nuisance last";
    case 3:  return "pf_batch, permuted";
    default: return "subset and random ties, through desc.rng";
    }
}

/* `rng` is where desc.rng_ctx points: the caller's generator state. */
static void snap_desc(yqst_desc* d, int config, uint64_t* rng) {
    switch (config) {
    case 0:
        ref_desc(d, false);
        break;
    case 1: case 2:
        memset(d, 0, sizeof(*d));
        d->pf = YQST_PF_GUMBEL;
        d->stim[0] = yqst_values(ref_x, RS);
        d->n_stim = 1;
        d->param[0] = yqst_values(ref_alpha, RA);
        d->param[1] = yqst_values(ref_beta, RB);
        d->param[2] = yqst_fixed(REF_GAMMA);
        d->param[3] = yqst_values(pn_lapse, PNL);
        if (config == 1) d->param[1].nuisance = true;
        else             d->param[3].nuisance = true;
        d->n_param = 4;
        break;
    case 3:
        pair_desc(d, true, false, true);
        break;
    default:
        ref_desc(d, false);
        d->subset_size = 3;
        d->tiebreak = YQST_TIE_RANDOM;
        d->rng = snap_rng_fn;
        d->rng_ctx = rng;
        break;
    }
    d->stop_trials = 1000;
}

/* Trials [from, to). With `leave_pending`, the last one is selected but not
 * answered, so the save sees a live proposal. */
static void snap_drive(yqst_quest* q, int from, int to, bool leave_pending,
                       uint64_t* obs, int* props) {
    static const double truth[4] = { -1.3, 2.5, 0.5, 0.03 };
    int i;
    for (i = from; i < to; i++) {
        int s = yqst_next(q), k;
        props[i] = s;
        if (leave_pending && i == to - 1) return;
        k = yqst_simulate(q, s, truth, sm_next(obs));
        yqst_update(q, s, k);
    }
}

static bool snap_same_history(const yqst_quest* a, const yqst_quest* b) {
    int na = -1, nb = -2, i, j;
    const yqst_trial* ha = hist_of(a, &na);
    const yqst_trial* hb = hist_of(b, &nb);
    if (na != nb) return false;
    for (i = 0; i < na; i++) {
        if (ha[i].stim_index != hb[i].stim_index ||
            ha[i].proposed_index != hb[i].proposed_index ||
            ha[i].outcome != hb[i].outcome)
            return false;
        for (j = 0; j < YQST_MAX_STIM_DIMS; j++)
            if (memcmp(&ha[i].stim[j], &hb[i].stim[j], sizeof(double)) != 0) return false;
    }
    return true;
}

static void test_snapshot_resume(void) {
    static const int cuts[] = { 0, 1, 7, SNAP_N - 1 };
    int config, ci, phase;
    for (config = 0; config < 5; config++) {
        yqst_desc d;
        uint64_t rng0 = 0x1234ABCDull, obs0 = 0x9876FEDCull, rng, obs, rng_final;
        int ref_props[SNAP_N] = {0};
        double ref_est[4] = {0};

        /* The uninterrupted run. */
        rng = rng0; obs = obs0;
        snap_desc(&d, config, &rng);
        CHECK(yqst_open(&g_q2, &d), "%s: open: %s", snap_name(config), yqst_error(&g_q2));
        snap_drive(&g_q2, 0, SNAP_N, false, &obs, ref_props);
        yqst_estimate(&g_q2, YQST_EST_MEAN, ref_est);
        rng_final = rng;   /* `rng` is reused by the cut runs below */

        for (ci = 0; ci < (int)(sizeof(cuts) / sizeof(cuts[0])); ci++) {
            for (phase = 0; phase < 2; phase++) {
                int c = cuts[ci], props[SNAP_N] = {0}, rc, i;
                bool pending = (phase == 1);
                size_t n;
                unsigned char* buf;
                uint64_t rng_saved, obs_saved, rng_resumed;
                double est[4] = {0};
                if (pending && c == 0) continue;   /* nothing selected yet */

                /* Run to the cut and save, beside the two generator states. */
                rng = rng0; obs = obs0;
                snap_desc(&d, config, &rng);
                CHECK(yqst_open(&g_q, &d), "open: %s", yqst_error(&g_q));
                snap_drive(&g_q, 0, c, pending, &obs, props);
                n = yqst_save_size(&g_q);
                CHECK(n > 0, "save_size is 0");
                buf = (unsigned char*)malloc(n + 1);
                if (!buf) { yqst_close(&g_q); continue; }
                rc = yqst_save(&g_q, buf, n);
                CHECK(rc == (int)n, "%s cut %d: save wrote %d of %lu bytes",
                      snap_name(config), c, rc, (unsigned long)n);
                CHECK(yqst_save(&g_q, buf, n - 1) == YQST_ERR_ARG, "save into a short buffer");
                rng_saved = rng; obs_saved = obs;
                yqst_close(&g_q);

                /* Resume in a fresh handle, with the generator restored into a
                 * DIFFERENT variable: the snapshot does not carry it. */
                rng_resumed = rng_saved;
                snap_desc(&d, config, &rng_resumed);
                memset(&g_q, 0, sizeof(g_q));
                CHECK(yqst_open_saved(&g_q, &d, buf, n), "%s cut %d phase %d: load: %s",
                      snap_name(config), c, phase, yqst_error(&g_q));
                CHECK(yqst_n_trials(&g_q) == (pending ? c - 1 : c),
                      "%s cut %d: n_trials %d after load", snap_name(config), c,
                      yqst_n_trials(&g_q));
                obs = obs_saved;
                snap_drive(&g_q, pending ? c - 1 : c, SNAP_N, false, &obs, props);

                for (i = 0; i < SNAP_N; i++)
                    CHECK(props[i] == ref_props[i],
                          "%s cut %d phase %d: proposal %d is %d, uninterrupted %d",
                          snap_name(config), c, phase, i, props[i], ref_props[i]);
                {
                    /* Named and checked, so the size can never be a negative
                     * error code turned into a huge size_t. */
                    int np = yqst_n_param(&g_q);
                    CHECK(np > 0 && np == yqst_n_param(&g_q2), "P after load = %d", np);
                    if (np > 0)
                        CHECK(memcmp(post_of(&g_q), post_of(&g_q2),
                                     (size_t)np * sizeof(double)) == 0,
                              "%s cut %d phase %d: the resumed posterior is not bit for bit",
                              snap_name(config), c, phase);
                }
                CHECK(snap_same_history(&g_q, &g_q2), "%s cut %d phase %d: history differs",
                      snap_name(config), c, phase);
                yqst_estimate(&g_q, YQST_EST_MEAN, est);
                for (i = 0; i < 4; i++)
                    CLOSE(est[i], ref_est[i], 0.0, "%s cut %d: estimate %d",
                          snap_name(config), c, i);
                CLOSE(yqst_entropy(&g_q), yqst_entropy(&g_q2), 0.0, "%s cut %d: entropy",
                      snap_name(config), c);
                if (config == 4)
                    CHECK(rng_resumed == rng_final, "cut %d phase %d: the header's generator "
                          "did not end where the uninterrupted run's did", c, phase);
                yqst_close(&g_q);
                free(buf);
            }
        }
        yqst_close(&g_q2);
    }
}

/* A save after the stop has fired resumes as stopped. */
static void test_snapshot_stopped(void) {
    yqst_desc d;
    unsigned char buf[4096];
    int props[SNAP_N] = {0}, n;
    uint64_t obs = 7;
    ref_desc(&d, false);
    d.stop_trials = 5;
    CHECK(yqst_open(&g_q, &d), "open: %s", yqst_error(&g_q));
    snap_drive(&g_q, 0, 5, false, &obs, props);
    CHECK(yqst_done(&g_q), "not done after 5 of 5");
    n = yqst_save(&g_q, buf, sizeof(buf));
    CHECK(n > 0, "save returned %d", n);
    yqst_close(&g_q);
    CHECK(yqst_open_saved(&g_q, &d, buf, (size_t)n), "load: %s", yqst_error(&g_q));
    CHECK(yqst_done(&g_q), "a stopped session resumed as running");
    CHECK(yqst_stop_reason(&g_q) == YQST_STOP_TRIALS, "stop reason %d",
          (int)yqst_stop_reason(&g_q));
    yqst_close(&g_q);
}

static void snap_reject(const yqst_desc* d, const unsigned char* buf, size_t len,
                        const char* want, const char* what) {
    bool ok;
    memset(&g_q, 0, sizeof(g_q));
    ok = yqst_open_saved(&g_q, d, buf, len);
    CHECK(!ok, "load accepted %s", what);
    CHECK(!yqst_is_open(&g_q), "a failed load (%s) left the handle open", what);
    CHECK(strstr(yqst_error(&g_q), want) != NULL, "%s: message \"%s\" lacks \"%s\"",
          what, yqst_error(&g_q), want);
    if (ok) yqst_close(&g_q);
}

static void test_snapshot_rejects(void) {
    yqst_desc d, e;
    unsigned char* buf;
    unsigned char* bad;
    int props[SNAP_N] = {0}, n, i;
    size_t off_counters, off_post;
    uint64_t obs = 11;
    static double alpha2[RA];

    ref_desc(&d, false);
    CHECK(yqst_open(&g_q, &d), "open: %s", yqst_error(&g_q));
    snap_drive(&g_q, 0, 6, false, &obs, props);
    n = (int)yqst_save_size(&g_q);
    buf = (unsigned char*)malloc((size_t)n + 1);
    bad = (unsigned char*)malloc((size_t)n + 1);
    if (!buf || !bad) { free(buf); free(bad); yqst_close(&g_q); return; }
    CHECK(yqst_save(&g_q, buf, (size_t)n) == n, "save");
    /* Where the counters and the posterior start, from the layout: the
     * history is n_trials x (n_stim f64, two i32, one u8). */
    off_post = (size_t)n - (size_t)yqst_n_trials(&g_q) * (8 * 1 + 4 + 4 + 1)
               - (size_t)yqst_n_param(&g_q) * 8;
    off_counters = off_post - 5 * 4;
    yqst_close(&g_q);
    CHECK(yqst_save(&g_q, buf, (size_t)n) == YQST_ERR_CLOSED, "save on a closed handle");
    CHECK(yqst_save_size(&g_q) == 0, "save_size on a closed handle");

    /* A load into the caller's buffer works like any other open. */
    {
        size_t need = yqst_memory_size(&d);
        void* mem = malloc(need);
        if (mem) {
            e = d;
            e.memory = mem;
            e.memory_size = need;
            CHECK(yqst_open_saved(&g_q, &e, buf, (size_t)n), "load into desc.memory: %s",
                  yqst_error(&g_q));
            CHECK(!g_q.mem_owned, "the load took its own memory");
            yqst_close(&g_q);
            free(mem);
        }
    }

    snap_reject(&d, NULL, 0, "not a ysp_quest snapshot", "a null snapshot");
    memcpy(bad, buf, (size_t)n); bad[0] = 'X';
    snap_reject(&d, bad, (size_t)n, "not a ysp_quest snapshot", "a wrong magic");
    memcpy(bad, buf, (size_t)n); bad[4] = 2;
    snap_reject(&d, bad, (size_t)n, "format", "a wrong format");
    snap_reject(&d, buf, (size_t)n - 1, "corrupt or truncated", "a truncated snapshot");
    snap_reject(&d, buf, 12, "does not match", "a snapshot cut inside its desc");
    memcpy(bad, buf, (size_t)n); bad[n] = 0;
    snap_reject(&d, bad, (size_t)n + 1, "after the snapshot's end", "a trailing byte");

    /* Descs that disagree with the snapshot, each named. */
    e = d; e.tie_tolerance = 1e-6;
    snap_reject(&e, buf, (size_t)n, "tie_tolerance", "a different tie_tolerance");
    for (i = 0; i < RA; i++) alpha2[i] = ref_alpha[i];
    alpha2[2] += 1e-12;
    e = d; e.param[0] = yqst_values(alpha2, RA);
    snap_reject(&e, buf, (size_t)n, "param[].values", "a grid point off by 1e-12");
    e = d; e.param[1].nuisance = true;
    snap_reject(&e, buf, (size_t)n, "param[].nuisance", "a different nuisance flag");
    e = d; e.stop_trials = 999;
    snap_reject(&e, buf, (size_t)n, "stop_trials", "a different stop_trials");
    e = d; e.select = YQST_SELECT_MEAN;
    snap_reject(&e, buf, (size_t)n, "select", "a different selection rule");
    e = d; e.no_table = true;
    snap_reject(&e, buf, (size_t)n, "no_table", "no_table switched on");

    /* The same model through the other callback is still a different desc:
     * the two differ in precision off the grid. */
    {
        unsigned char* b2;
        int n2;
        pair_desc(&e, false, false, false);
        CHECK(yqst_open(&g_q, &e), "open: %s", yqst_error(&g_q));
        n2 = (int)yqst_save_size(&g_q);
        b2 = (unsigned char*)malloc((size_t)n2);
        if (b2) {
            CHECK(yqst_save(&g_q, b2, (size_t)n2) == n2, "save");
            yqst_close(&g_q);
            e.pf_fn = NULL;
            e.pf_batch = pair_pf_batch;
            snap_reject(&e, b2, (size_t)n2, "pf_fn/pf_batch", "a pf_fn snapshot loaded as pf_batch");
            free(b2);
        } else {
            yqst_close(&g_q);
        }
    }

    /* Numbers inside the state that cannot be right. */
    memcpy(bad, buf, (size_t)n);
    bad[off_counters] = 0xff; bad[off_counters + 1] = 0xff;
    bad[off_counters + 2] = 0xff; bad[off_counters + 3] = 0x7f;
    snap_reject(&d, bad, (size_t)n, "counters are corrupt", "n_trials of 2^31 - 1");
    memcpy(bad, buf, (size_t)n);
    for (i = 0; i < 8; i++) bad[off_post + (size_t)i] = 0xff;   /* a NaN */
    snap_reject(&d, bad, (size_t)n, "posterior is corrupt", "a NaN in the posterior");
    memcpy(bad, buf, (size_t)n);
    bad[n - 1] = 0xff;                                  /* the last outcome */
    snap_reject(&d, bad, (size_t)n, "history is corrupt", "an outcome of 255");
    free(buf);
    free(bad);
}

/* The version macros agree with each other and with the implementation, and
 * the axis sizes come back as the desc stated them, in the caller's order even
 * where a nuisance axis made the header permute its posterior. */
static void test_version_and_axis_n(void) {
    char want[32];
    yqst_desc d;
    int i;
    snprintf(want, sizeof(want), "%d.%d.%d",
             YQST_VERSION_MAJOR, YQST_VERSION_MINOR, YQST_VERSION_PATCH);
    CHECK(strcmp(YQST_VERSION_STRING, want) == 0,
          "YQST_VERSION_STRING %s does not match the numbers %s", YQST_VERSION_STRING, want);
    CHECK(strcmp(yqst_version(), YQST_VERSION_STRING) == 0,
          "yqst_version() %s, macro %s", yqst_version(), YQST_VERSION_STRING);
    /* Pinned, and compared at run time: a constant condition is MSVC's C4127,
     * and the string from the implementation is the thing worth pinning. */
    CHECK(strcmp(yqst_version(), "0.7.0") == 0, "version %s, want 0.7.0", yqst_version());

    memset(&d, 0, sizeof(d));
    d.pf = YQST_PF_GUMBEL;
    d.stim[0] = yqst_linspace(-2.0, 0.0, 3);
    d.stim[1] = yqst_linspace(1.0, 4.0, 4);
    d.n_stim = 2;
    d.param[0] = yqst_values(ref_alpha, RA);
    d.param[1] = yqst_values(ref_beta, RB);
    d.param[1].nuisance = true;           /* permutes the posterior internally */
    d.param[2] = yqst_fixed(0.5);
    d.param[3] = yqst_values(pn_lapse, PNL);
    d.n_param = 4;
    d.stop_trials = 5;
    memset(&g_q, 0, sizeof(g_q));
    CHECK(yqst_stim_axis_n(&g_q, 0) == YQST_ERR_CLOSED, "axis_n on a closed handle");
    CHECK(yqst_param_axis_n(NULL, 0) == YQST_ERR_ARG, "axis_n on a null handle");
    CHECK(yqst_open(&g_q, &d), "open: %s", yqst_error(&g_q));
    CHECK(g_q.permuted, "the test meant to exercise a permuted handle");
    CHECK(yqst_stim_axis_n(&g_q, 0) == 3, "stim axis 0 = %d", yqst_stim_axis_n(&g_q, 0));
    CHECK(yqst_stim_axis_n(&g_q, 1) == 4, "stim axis 1 = %d", yqst_stim_axis_n(&g_q, 1));
    CHECK(yqst_param_axis_n(&g_q, 0) == RA, "param axis 0 = %d", yqst_param_axis_n(&g_q, 0));
    CHECK(yqst_param_axis_n(&g_q, 1) == RB, "param axis 1 = %d", yqst_param_axis_n(&g_q, 1));
    CHECK(yqst_param_axis_n(&g_q, 2) == 1, "param axis 2 = %d", yqst_param_axis_n(&g_q, 2));
    CHECK(yqst_param_axis_n(&g_q, 3) == PNL, "param axis 3 = %d", yqst_param_axis_n(&g_q, 3));
    CHECK(yqst_stim_axis_n(&g_q, 2) == YQST_ERR_ARG, "stim axis past n_stim");
    CHECK(yqst_stim_axis_n(&g_q, -1) == YQST_ERR_ARG, "negative stim axis");
    CHECK(yqst_param_axis_n(&g_q, 4) == YQST_ERR_ARG, "param axis past n_param");
    {
        int prod_s = 1, prod_p = 1;
        for (i = 0; i < 2; i++) prod_s *= yqst_stim_axis_n(&g_q, i);
        for (i = 0; i < 4; i++) prod_p *= yqst_param_axis_n(&g_q, i);
        CHECK(prod_s == yqst_n_stim(&g_q), "the stimulus axes do not multiply to S");
        CHECK(prod_p == yqst_n_param(&g_q), "the parameter axes do not multiply to P");
    }
    yqst_close(&g_q);
}

/* --- parameter table (v0.7.0) --------------------------------------------- */

#include "param_table_check.h"

static void ptc_fail(int line, const char* what) {
    g_checks++;
    g_fail++;
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, line, what);
}

#define PF(name, member, kind, n) { name, offsetof(yqst_desc, member), kind, n, NULL }
#define PE(name, member, n, v) { name, offsetof(yqst_desc, member), 'e', n, v }
static const ptc_field g_pfields[] = {
    PF("n_stim", n_stim, 'i', 0),
    PF("stim[].values", stim[0].values, 'D', 0),
    PF("stim[].lo", stim[0].lo, 'd', 0),
    PF("stim[].hi", stim[0].hi, 'd', 0),
    PF("stim[].n", stim[0].n, 'i', 0),
    PF("n_param", n_param, 'i', 0),
    PF("param[].values", param[0].values, 'D', 0),
    PF("param[].lo", param[0].lo, 'd', 0),
    PF("param[].hi", param[0].hi, 'd', 0),
    PF("param[].n", param[0].n, 'i', 0),
    PF("param[].prior", param[0].prior, 'D', 0),
    PF("param[].nuisance", param[0].nuisance, 'b', 0),
    PF("joint_prior", joint_prior, 'D', 0),
    PE("pf", pf, YQST_PF_CUSTOM + 1, "gumbel=0,weibull=1,logistic=2,normal=3,hypsec=4,custom=5"),
    PF("n_outcomes", n_outcomes, 'i', 0),
    PE("select", select, YQST_SELECT_MODE + 1, "entropy=0,quantile=1,mean=2,mode=3"),
    PF("select_param", select_param, 'i', 0),
    PF("select_quantile", select_quantile, 'd', 0),
    PE("tiebreak", tiebreak, YQST_TIE_RANDOM + 1, "lowest=0,nearest=1,alternate=2,random=3"),
    PF("tie_tolerance", tie_tolerance, 'd', 0),
    PF("subset_size", subset_size, 'i', 0),
    PF("stop_trials", stop_trials, 'i', 0),
    PF("stop_entropy", stop_entropy, 'd', 0),
    PF("stop_sd", stop_sd, 'd', 0),
    PF("stop_sd_param", stop_sd_param, 'i', 0),
    PF("no_table", no_table, 'b', 0),
};
#undef PF
#undef PE
#define NPF ((int)(sizeof g_pfields / sizeof g_pfields[0]))

/* The stimuli a desc proposes over a fixed response stream. */
static void param_run(const yqst_desc* d, int* out, int n) {
    int i, s;
    for (i = 0; i < n; i++) out[i] = -9;
    if (!yqst_open(&g_q, d)) return;
    for (i = 0; i < n; i++) {
        s = yqst_next(&g_q);
        out[i] = s;
        if (s < 0 || yqst_update(&g_q, s, (i * 7 + 3) % 5 < 3) < 0) break;
    }
    yqst_close(&g_q);
}

static void test_params(void) {
    static ptc_row rows[64];
    static unsigned char snap[1 << 16];
    const yqst_param* p;
    yqst_desc base, d;
    int n = 0, i, b, len;
    char msg[200];
    p = yqst_params(&n);
    CHECK(p != NULL && n == NPF && n <= 64, "yqst_params: %d rows, the test knows %d", n, NPF);
    if (!p || n > 64) return;
    for (i = 0; i < n; i++) PTC_ROW(rows[i], p[i]);
    ptc_check_cover(rows, n, g_pfields, NPF);

    /* Two descs with every field at its default but the required ones: the
     * QUEST+ default, and QUEST's quantile placement, where select_param and
     * select_quantile matter. */
    for (b = 0; b < 2; b++) {
        memset(&base, 0, sizeof base);
        base.stim[0] = yqst_linspace(-2.0, 0.0, 21);
        base.n_stim = 1;
        base.param[0] = yqst_linspace(-2.0, 0.0, 11);
        base.param[1] = yqst_values(ref_beta, RB);
        base.param[2] = yqst_fixed(0.5);
        base.param[3] = yqst_fixed(0.02);
        base.n_param = 4;
        base.stop_trials = 20;
        base.stop_entropy = 1e-6;   /* either stop alone is enough, so zero each */
        if (b == 1) base.select = YQST_SELECT_QUANTILE;
        for (i = 0; i < n; i++) {
            const ptc_field* f;
            int k = ptc_check_row(&rows[i], g_pfields, NPF), t;
            double got;
            if (k < 0) continue;
            f = &g_pfields[k];
            if (f->kind == 'e' && b == 0) {
                d = base;
                ptc_set(&d, f, rows[i].max + 1.0);
                CHECK(!yqst_open(&g_q, &d), "%s = %g opened", f->name, rows[i].max + 1.0);
            }
            if (!ptc_scalar(f)) continue;
            d = base;
            ptc_set(&d, f, 0.0);
            if (!yqst_open(&g_q, &d)) {
                CHECK(ptc_required(&rows[i]), "zero %s refused (%s), its doc does not say required",
                      f->name, yqst_error(&g_q));
                continue;
            }
            /* open() writes the defaults into its copy of the desc; the outcome
             * count is the one field it keeps elsewhere. */
            got = strcmp(f->name, "n_outcomes") == 0 ? (double)yqst_n_outcomes(&g_q)
                                                     : ptc_get(&g_q.desc, f);
            CHECK(got == rows[i].def, "zero %s opens as %g, def is %g", f->name, got, rows[i].def);
            yqst_close(&g_q);
            {
                int a[20], c[20];
                param_run(&d, a, 20);
                ptc_set(&d, f, rows[i].def);
                param_run(&d, c, 20);
                for (t = 0; t < 20; t++)
                    if (a[t] != c[t]) {
                        snprintf(msg, sizeof msg, "%s = 0 and = def propose differently at %d", f->name, t);
                        ptc_fail(__LINE__, msg);
                        break;
                    }
            }
        }
    }
    CHECK(yqst_params(NULL) == p, "the table is static");

    /* SNAPSHOTS names the magic; v0.7.0 corrected the text, not the bytes. */
    CHECK(yqst_open(&g_q, &base), "open: %s", yqst_error(&g_q));
    len = yqst_save(&g_q, snap, sizeof snap);
    CHECK(len > 8 && memcmp(snap, "YQST", 4) == 0, "snapshot magic");
    yqst_close(&g_q);
}
#undef NPF

int main(void) {
    ref_build_axes();
    test_reference(false);
    test_reference(true);
    test_no_table_matches();
    test_update_values();
    test_memory();
    test_pfs();
    test_nuisance();
    test_placement();
    test_ties();
    test_subset();
    test_stopping();
    test_estimates();
    test_three_outcomes();
    test_rejections();
    test_grids();
    test_extras();
    test_decomposition();
    test_permuted_nuisance(false);
    test_permuted_nuisance(true);
    test_batch_pf();
    test_version_and_axis_n();
    test_snapshot_resume();
    test_snapshot_stopped();
    test_snapshot_rejects();
    test_params();
#ifdef YQST_ASYNC
    test_async_matches();
    test_async_queue();
    test_async_edges();
    test_async_depth();
#endif
    test_simulated_run();

    if (g_fail) {
        fprintf(stderr, "quest_test: %d of %d checks FAILED\n", g_fail, g_checks);
        return 1;
    }
    printf("quest_test: %d checks passed\n", g_checks);
    return 0;
}
