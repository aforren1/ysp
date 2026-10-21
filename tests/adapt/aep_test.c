/* Self-checking tests for ysp/aep.h. No framework: every check prints a line
 * and bumps a counter, main() returns 0 only when the counter is zero.
 *
 * The file defines YSP_AEP_IMPLEMENTATION, so the header's private helpers are
 * in scope. Several checks need them: the numerics under test are the Cholesky,
 * the quadrature, the Laplace mode and the analytic marginal-likelihood
 * gradient, and none of those has a public entry point of its own.
 *
 *   cc -O2 -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Werror -Iinclude \
 *      -o aep_test tests/adapt/aep_test.c -lm && ./aep_test
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS   /* getenv, for YAEP_TEST_ONLY */
#endif
#define YSP_AEP_IMPLEMENTATION
#include "ysp/aep.h"

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

static int yaep_test_fails = 0;

/* Every quantity that passes through the header's yaep_real matrices loses
 * accuracy in a float build. The factor is not a guess: it is what the checks
 * below were measured to need, and the numbers they actually reach are printed
 * so the two builds can be compared. */
static const double tolx = (sizeof(yaep_real) == sizeof(double)) ? 1.0 : 2e7;
static const int real_is_double = (sizeof(yaep_real) == sizeof(double));

/* Worst observed errors, printed at the end. */
static double worst_chol = 0.0, worst_stat = 0.0, worst_var = 0.0;
static double worst_look = 0.0, worst_lm = 0.0, worst_grad = 0.0;

static double track(double* slot, double v) {
    if (v > *slot) *slot = v;
    return v;
}

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                       \
            printf(__VA_ARGS__);                                              \
            printf("\n");                                                     \
            yaep_test_fails++;                                               \
        }                                                                     \
    } while (0)

#define CLOSE(a, b, tol, ...)                                                 \
    do {                                                                      \
        double aa_ = (a), bb_ = (b);                                          \
        if (!(fabs(aa_ - bb_) <= (tol))) {                                    \
            printf("FAIL %s:%d: %.17g vs %.17g (tol %g): ", __FILE__,         \
                   __LINE__, aa_, bb_, (double)(tol));                        \
            printf(__VA_ARGS__);                                              \
            printf("\n");                                                     \
            yaep_test_fails++;                                               \
        }                                                                     \
    } while (0)

/* --- test-side linear algebra, independent of the header's -------------- */

/* Gauss-Jordan inverse with partial pivoting, n x n row-major. Deliberately a
 * different algorithm from the header's Cholesky so a shared mistake cannot
 * pass both sides of a comparison. */
static void tinv(double* A, int n) {
    double* I = (double*)calloc((size_t)n * n, sizeof(double));
    for (int i = 0; i < n; i++) I[i * n + i] = 1.0;
    for (int c = 0; c < n; c++) {
        int piv = c;
        double best = fabs(A[c * n + c]), d;
        for (int r = c + 1; r < n; r++)
            if (fabs(A[r * n + c]) > best) { best = fabs(A[r * n + c]); piv = r; }
        if (piv != c) {
            for (int k = 0; k < n; k++) {
                double t = A[c * n + k]; A[c * n + k] = A[piv * n + k]; A[piv * n + k] = t;
                t = I[c * n + k]; I[c * n + k] = I[piv * n + k]; I[piv * n + k] = t;
            }
        }
        d = A[c * n + c];
        for (int k = 0; k < n; k++) { A[c * n + k] /= d; I[c * n + k] /= d; }
        for (int r = 0; r < n; r++) {
            double f = A[r * n + c];
            if (r == c || f == 0.0) continue;
            for (int k = 0; k < n; k++) {
                A[r * n + k] -= f * A[c * n + k];
                I[r * n + k] -= f * I[c * n + k];
            }
        }
    }
    memcpy(A, I, (size_t)n * n * sizeof(double));
    free(I);
}

/* log|det A| by Gaussian elimination with partial pivoting, again on purpose
 * not the header's Cholesky. Destroys A. */
static double tlogdet(double* A, int n) {
    double acc = 0.0;
    for (int c = 0; c < n; c++) {
        int piv = c;
        double best = fabs(A[c * n + c]);
        for (int r = c + 1; r < n; r++)
            if (fabs(A[r * n + c]) > best) { best = fabs(A[r * n + c]); piv = r; }
        if (piv != c)
            for (int k = 0; k < n; k++) {
                double t = A[c * n + k]; A[c * n + k] = A[piv * n + k];
                A[piv * n + k] = t;
            }
        acc += log(fabs(A[c * n + c]));
        for (int r = c + 1; r < n; r++) {
            double f = A[r * n + c] / A[c * n + c];
            for (int k = c; k < n; k++) A[r * n + k] -= f * A[c * n + k];
        }
    }
    return acc;
}

/* --- deterministic uniforms --------------------------------------------- */

static uint64_t rng_state = 0x243F6A8885A308D3ull;

static double rng_u(void) {
    uint64_t z = (rng_state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z = z ^ (z >> 31);
    return (double)(z >> 11) * (1.0 / 9007199254740992.0);
}

static double rng_cb(void* ctx) { (void)ctx; return rng_u(); }

/* --- 1: Cholesky and triangular solves ---------------------------------- */

/* The binding reads its __version__ from yaep_version(), so the string must be
 * the three macros and nothing else. Compared at run time through strcmp
 * because a constant-condition CHECK is MSVC warning C4127 under /W4 /WX. */
static void test_version(void) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%d.%d.%d", YAEP_VERSION_MAJOR,
             YAEP_VERSION_MINOR, YAEP_VERSION_PATCH);
    CHECK(strcmp(yaep_version(), "0.16.0") == 0,
          "yaep_version() is %s, expected 0.16.0", yaep_version());
    CHECK(strcmp(yaep_version(), YAEP_VERSION_STRING) == 0,
          "yaep_version() %s disagrees with YAEP_VERSION_STRING %s",
          yaep_version(), YAEP_VERSION_STRING);
    CHECK(strcmp(buf, YAEP_VERSION_STRING) == 0,
          "YAEP_VERSION_STRING %s is not MAJOR.MINOR.PATCH %s",
          YAEP_VERSION_STRING, buf);
    printf("  version %s\n", yaep_version());
}

static void test_chol(void) {
    double A[9] = { 4.0, 2.0, 1.0,
                    2.0, 5.0, 3.0,
                    1.0, 3.0, 6.0 };
    yaep_real L[9];
    double b[3] = { 1.0, -2.0, 0.5 }, x[3], Ai[9], xref[3];
    for (int i = 0; i < 9; i++) L[i] = (yaep_real)A[i];
    CHECK(yaep__chol(L, 3, 3), "chol of an SPD matrix failed");
    for (int i = 0; i < 3; i++)
        for (int j = 0; j <= i; j++) {
            double s = 0.0;
            for (int k = 0; k <= j; k++) s += (double)L[i * 3 + k] * L[j * 3 + k];
            track(&worst_chol, fabs(s - A[i * 3 + j]));
            CLOSE(s, A[i * 3 + j], 1e-13 * tolx, "L L' at (%d,%d)", i, j);
        }
    memcpy(x, b, sizeof(b));
    yaep__tri_fwd(L, 3, 3, x);
    yaep__tri_bwd(L, 3, 3, x);
    memcpy(Ai, A, sizeof(A));
    tinv(Ai, 3);
    for (int i = 0; i < 3; i++) {
        xref[i] = 0.0;
        for (int j = 0; j < 3; j++) xref[i] += Ai[i * 3 + j] * b[j];
        track(&worst_chol, fabs(x[i] - xref[i]));
        CLOSE(x[i], xref[i], 1e-12 * tolx, "solve component %d", i);
    }
    /* A matrix that is not positive definite must be reported, not fixed. */
    {
        yaep_real B[4] = { 1.0, 2.0, 2.0, 1.0 };
        CHECK(!yaep__chol(B, 2, 2), "chol accepted an indefinite matrix");
    }
    /* The triangular inverse and its square, used by the hyper gradient. */
    {
        yaep_real C[9];
        double P[9] = { 0.0 };
        for (int i = 0; i < 9; i++) C[i] = (yaep_real)A[i];
        yaep__chol(C, 3, 3);
        {
            double tmp[3] = { 0.0 };
            yaep__tri_inv(C, 3, 3, tmp);
            yaep__tri_sqr(C, 3, 3, tmp);
        }
        memcpy(P, A, sizeof(A));
        tinv(P, 3);
        for (int i = 0; i < 9; i++) {
            track(&worst_chol, fabs((double)C[i] - P[i]));
            CLOSE((double)C[i], P[i], 1e-12 * tolx, "A^-1 entry %d", i);
        }
    }
}

/* --- 2: kernel symmetry and positive definiteness ----------------------- */

static void test_kernel(int kernel) {
    yaep_desc d;
    yaep_gp g;
    double X[16 * 3] = { 0.0 };
    yaep_real G[16 * 16];
    int n = 16, nd = 3;
    rng_state = 0x243F6A8885A308D3ull + 0x11ull;
    memset(&d, 0, sizeof(d));
    d.n_dims = nd;
    for (int i = 0; i < nd; i++) { d.lo[i] = -1.0; d.hi[i] = 2.0; }
    d.intensity_dim = 2;
    d.kernel = (yaep_kernel)kernel;
    d.target_p = 0.75;
    d.stop_trials = 10;
    CHECK(yaep_open(&g, &d), "open for the kernel test: %s", yaep_error(&g));
    for (int i = 0; i < n * nd; i++) X[i] = -1.0 + 3.0 * rng_u();
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++)
            G[i * n + j] = (yaep_real)yaep__kernel(&g, X + i * nd, X + j * nd);
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < i; j++)
            CLOSE((double)G[i * n + j], (double)G[j * n + i], 1e-15,
                  "kernel symmetry (%d,%d)", i, j);
        CHECK(G[i * n + i] > 0.0, "kernel diagonal %d is not positive", i);
    }
    for (int i = 0; i < n; i++) G[i * n + i] += (yaep_real)1e-6;
    CHECK(yaep__chol(G, n, n), "kernel %d Gram matrix is not positive definite",
          kernel);
    yaep_close(&g);
}

/* --- 3: Gauss-Hermite nodes --------------------------------------------- */

static void test_quad(void) {
    double x[YAEP_QUAD_N] = { 0.0 }, w[YAEP_QUAD_N] = { 0.0 };
    double want[7] = { 1.0, 0.0, 1.0, 0.0, 3.0, 0.0, 15.0 };
    yaep__gh_init(x, w, YAEP_QUAD_N);
    for (int k = 0; k <= 6; k++) {
        double acc = 0.0;
        for (int i = 0; i < YAEP_QUAD_N; i++)
            acc += w[i] * pow(YAEP__SQRT2 * x[i], (double)k);
        acc /= YAEP__SQRTPI;
        CLOSE(acc, want[k], 1e-11, "Gaussian moment %d", k);
    }
}

/* --- 4: exact Gaussian regression vs a dense solve ---------------------- */

static void test_gaussian_exact(void) {
    yaep_desc d;
    yaep_gp g;
    int n = 9, nd = 2;
    double xs[9 * 2] = { 0.0 }, ys[9] = { 0.0 };
    double Ky[81], ks[9], mu, sd, mref, vref, kxx;
    double probe[2] = { 0.31, -0.22 };
    rng_state = 0x243F6A8885A308D3ull + 0x22ull;
    memset(&d, 0, sizeof(d));
    d.n_dims = nd;
    d.lo[0] = -1.0; d.hi[0] = 1.0;
    d.lo[1] = -1.0; d.hi[1] = 1.0;
    d.lik = YAEP_LIK_GAUSSIAN;
    d.hyper.lengthscale[0] = 0.7;
    d.hyper.lengthscale[1] = 0.4;
    d.hyper.outputscale = 1.3;
    d.hyper.mean = 0.25;
    d.hyper.noise_sd = 0.3;
    d.stop_trials = n;
    CHECK(yaep_open(&g, &d), "open GAUSSIAN: %s", yaep_error(&g));
    for (int i = 0; i < n; i++) {
        xs[i * 2 + 0] = -1.0 + 2.0 * rng_u();
        xs[i * 2 + 1] = -1.0 + 2.0 * rng_u();
        ys[i] = sin(3.0 * xs[i * 2]) + 0.2 * rng_u();
        CHECK(yaep_update_real(&g, xs + i * 2, ys[i]) == YAEP_OK,
              "update_real %d", i);
    }
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++)
            Ky[i * n + j] = yaep__kernel(&g, xs + i * 2, xs + j * 2) +
                            (i == j ? 0.3 * 0.3 + 1e-6 : 0.0);
    tinv(Ky, n);
    for (int i = 0; i < n; i++) ks[i] = yaep__kernel(&g, xs + i * 2, probe);
    kxx = yaep__kernel(&g, probe, probe);
    mref = 0.25;
    vref = kxx;
    for (int i = 0; i < n; i++) {
        double row = 0.0;
        for (int j = 0; j < n; j++) row += Ky[i * n + j] * (ys[j] - 0.25);
        mref += ks[i] * row;
        row = 0.0;
        for (int j = 0; j < n; j++) row += Ky[i * n + j] * ks[j];
        vref -= ks[i] * row;
    }
    CHECK(yaep_predict_f(&g, probe, 0, &mu, &sd) == YAEP_OK, "predict_f");
    CLOSE(mu, mref, 1e-10, "Gaussian posterior mean");
    CLOSE(sd * sd, vref, 1e-10, "Gaussian posterior variance");
    /* The exact log marginal likelihood, from the same dense inverse. */
    {
        double quad = 0.0, ldet, Kc[81];
        for (int i = 0; i < n; i++)
            for (int j = 0; j < n; j++)
                Kc[i * n + j] = yaep__kernel(&g, xs + i * 2, xs + j * 2) +
                                (i == j ? 0.3 * 0.3 + 1e-6 : 0.0);
        for (int i = 0; i < n; i++)
            for (int j = 0; j < n; j++)
                quad += (ys[i] - 0.25) * Ky[i * n + j] * (ys[j] - 0.25);
        ldet = 0.5 * tlogdet(Kc, n);   /* the test's own elimination: log|Ky| */
        CLOSE(yaep_log_marginal(&g),
              -0.5 * quad - ldet - 0.5 * n * YAEP__LOG2PI, 1e-9,
              "Gaussian log marginal");
    }
    yaep_close(&g);
}

/* --- 5: the Laplace mode and its predictive variance -------------------- */

static void test_laplace_probit(void) {
    yaep_desc d;
    yaep_gp g;
    int n = 6;
    double xs[6] = { -0.9, -0.5, -0.1, 0.2, 0.6, 0.95 };
    int ys[6] = { 0, 0, 1, 0, 1, 1 };
    double K[36], A[36], ks[6], mu, sd, vref, kxx;
    double probe = 0.35;
    memset(&d, 0, sizeof(d));
    d.n_dims = 1;
    d.lo[0] = -1.0; d.hi[0] = 1.0;
    d.hyper.lengthscale[0] = 0.5;
    d.hyper.outputscale = 2.0;
    d.hyper.mean = -0.3;
    d.target_p = 0.75;
    d.stop_trials = n;
    CHECK(yaep_open(&g, &d), "open BERNOULLI: %s", yaep_error(&g));
    for (int i = 0; i < n; i++)
        CHECK(yaep_update(&g, xs + i, ys[i]) == YAEP_OK, "update %d", i);
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++)
            K[i * n + j] = yaep__kernel(&g, xs + i, xs + j) +
                           (i == j ? 1e-6 : 0.0);
    /* Stationarity: fhat = mean + K grad log p(y | fhat), with the gradient
     * recomputed here from the probit rather than read from the handle. */
    for (int i = 0; i < n; i++) {
        double acc = -0.3;
        for (int j = 0; j < n; j++) {
            double s = ys[j] ? 1.0 : -1.0, z = s * g.f[j];
            double gr = s * exp(-0.5 * z * z) / (2.5066282746310005 *
                                                 0.5 * erfc(-z / sqrt(2.0)));
            acc += K[i * n + j] * gr;
        }
        track(&worst_stat, fabs(g.f[i] - acc));
        CLOSE(g.f[i], acc, 1e-8 * tolx, "stationarity at site %d", i);
    }
    /* Predictive variance against a dense (K^-1 + W)^-1. */
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++) A[i * n + j] = K[i * n + j];
    tinv(A, n);
    for (int i = 0; i < n; i++) A[i * n + i] += g.W[i];
    tinv(A, n);   /* A = (K^-1 + W)^-1, the posterior covariance at the sites */
    for (int i = 0; i < n; i++) ks[i] = yaep__kernel(&g, xs + i, &probe);
    kxx = yaep__kernel(&g, &probe, &probe);
    {
        /* var = k** - k*' K^-1 k* + k*' K^-1 A K^-1 k*, which is the
         * same thing as k** - k*' (K + W^-1)^-1 k* written through A. */
        double Ki[36], v1[6], v2[6], t1 = 0.0, t2 = 0.0;
        memcpy(Ki, K, sizeof(K));
        tinv(Ki, n);
        for (int i = 0; i < n; i++) {
            v1[i] = 0.0;
            for (int j = 0; j < n; j++) v1[i] += Ki[i * n + j] * ks[j];
        }
        for (int i = 0; i < n; i++) {
            v2[i] = 0.0;
            for (int j = 0; j < n; j++) v2[i] += A[i * n + j] * v1[j];
        }
        for (int i = 0; i < n; i++) { t1 += ks[i] * v1[i]; t2 += v1[i] * v2[i]; }
        vref = kxx - t1 + t2;
    }
    CHECK(yaep_predict_f(&g, &probe, 0, &mu, &sd) == YAEP_OK, "predict_f");
    track(&worst_var, fabs(sd * sd - vref));
    CLOSE(sd * sd, vref, 1e-9 * tolx, "Laplace predictive variance");
    /* The probit closed form is what yaep_predict_p must return. */
    CLOSE(yaep_predict_p(&g, &probe),
          0.5 * erfc(-(mu / sqrt(1.0 + sd * sd)) / sqrt(2.0)), 1e-13,
          "predict_p closed form");   /* both sides read the same mu and sd */
    /* The Laplace log marginal likelihood (R&W eq. 3.32) is what the
     * hyperparameter fit maximizes, so it is worth one independent
     * evaluation: -1/2 a'(fhat - m) + log p(y | fhat) - 1/2 log|B|. */
    {
        double Ki[36], B[36], av[6], quad = 0.0, ll = 0.0, ldet;
        memcpy(Ki, K, sizeof(K));
        tinv(Ki, n);
        for (int i = 0; i < n; i++) {
            av[i] = 0.0;
            for (int j = 0; j < n; j++) av[i] += Ki[i * n + j] * (g.f[j] + 0.3);
        }
        for (int i = 0; i < n; i++) {
            double s = ys[i] ? 1.0 : -1.0;
            quad += av[i] * (g.f[i] + 0.3);
            ll += log(0.5 * erfc(-(s * g.f[i]) / sqrt(2.0)));
        }
        for (int i = 0; i < n; i++)
            for (int j = 0; j < n; j++)
                B[i * n + j] = (i == j ? 1.0 : 0.0) +
                               sqrt(g.W[i]) * K[i * n + j] * sqrt(g.W[j]);
        ldet = tlogdet(B, n);
        track(&worst_lm, fabs(yaep_log_marginal(&g) -
                              (-0.5 * quad + ll - 0.5 * ldet)));
        CLOSE(yaep_log_marginal(&g), -0.5 * quad + ll - 0.5 * ldet, 1e-8 * tolx,
              "Laplace log marginal likelihood");
    }
    yaep_close(&g);
}

/* --- 6: the analytic gradient against central differences --------------- */

static yaep_link yaep_link_gradient_probe = YAEP_LINK_PROBIT;
static double yaep_guess_gradient_probe = 0.0;
static double yaep_lapse_gradient_probe = 0.0;

static void test_gradient(yaep_lik lik, yaep_kernel kernel, const char* name) {
    yaep_desc d;
    yaep_gp g;
    yaep__param p[YAEP__NTHETA];
    double th[YAEP__NTHETA] = { 0.0 }, ga[YAEP__NTHETA] = { 0.0 }, gn[YAEP__NTHETA] = { 0.0 };
    double v0 = 0.0, vp = 0.0, vm = 0.0;
    int np, n = 12, nd = 2;
    rng_state = 0x243F6A8885A308D3ull + 0x33ull;
    memset(&d, 0, sizeof(d));
    d.n_dims = nd;
    d.lo[0] = -1.0; d.hi[0] = 1.0;
    d.lo[1] = -2.0; d.hi[1] = 2.0;
    d.intensity_dim = 1;
    d.kernel = kernel;
    d.lik = lik;
    d.link = yaep_link_gradient_probe;
    if (lik == YAEP_LIK_ORDINAL) { d.n_outcomes = 4; d.target_outcome = 2; }
    if (lik != YAEP_LIK_GAUSSIAN) {
        d.guess = yaep_guess_gradient_probe;
        d.lapse = yaep_lapse_gradient_probe;
    }
    d.target_p = 0.75;
    d.stop_trials = n;
    CHECK(yaep_open(&g, &d), "open %s: %s", name, yaep_error(&g));
    for (int i = 0; i < n; i++) {
        double x[2] = { 0.0 };
        x[0] = -1.0 + 2.0 * rng_u();
        x[1] = -2.0 + 4.0 * rng_u();
        if (lik == YAEP_LIK_GAUSSIAN) {
            CHECK(yaep_update_real(&g, x, 0.7 * x[1] + 0.3 * rng_u()) == YAEP_OK,
                  "%s update", name);
        } else {
            int k = (int)(rng_u() * (lik == YAEP_LIK_ORDINAL ? 4.0 : 2.0));
            CHECK(yaep_update(&g, x, k) == YAEP_OK, "%s update", name);
        }
    }
    np = yaep__params(&g, p);
    CHECK(np > 0, "%s has no free hyperparameters", name);
    yaep__theta_get(&g, p, np, th);
    CHECK(yaep__fit_eval(&g, p, np, th, &v0, ga) == YAEP_OK, "%s fit_eval", name);
    for (int i = 0; i < np; i++) {
        /* The step has to clear the resolution of the objective: a float build
         * computes it to about 1e-7 relative, so a 1e-5 step would differentiate
         * the noise. */
        double step = (real_is_double ? 1e-5 : 3e-3) * (1.0 + fabs(th[i]));
        double save = th[i], rel;
        th[i] = save + step;
        CHECK(yaep__fit_eval(&g, p, np, th, &vp, NULL) == YAEP_OK, "%s +h", name);
        th[i] = save - step;
        CHECK(yaep__fit_eval(&g, p, np, th, &vm, NULL) == YAEP_OK, "%s -h", name);
        th[i] = save;
        gn[i] = (vp - vm) / (2.0 * step);
        rel = fabs(ga[i] - gn[i]) / (fabs(gn[i]) > 1e-6 ? fabs(gn[i]) : 1e-6);
        track(&worst_grad, rel);
        CHECK(rel < (real_is_double ? 1e-4 : 6e-2),
              "%s gradient %d (kind %d dim %d): analytic %.12g, "
              "finite difference %.12g, relative error %.3g",
              name, i, p[i].kind, p[i].dim, ga[i], gn[i], rel);
    }
    yaep_close(&g);
}

/* --- 7: the rank-one look-ahead ----------------------------------------- */

/* The look-ahead variance at every candidate after one more observation, with W
 * frozen, is a rank-one downdate of the candidate covariance. The brute force
 * on the other side builds the (N + 1)-point posterior from scratch with the
 * same frozen W and the new site's precision appended. */
static void test_lookahead(void) {
    yaep_desc d;
    yaep_gp g;
    int n = 7, M, ja = 3, jb = 5, jc = 2;
    double xs[7] = { -0.85, -0.6, -0.2, 0.05, 0.4, 0.7, 0.9 };
    int ys[7] = { 0, 0, 1, 0, 1, 1, 1 };
    double A[64], cw, cov_ab, cov_aj, cov_jb, cov_jj, want, got;
    double ca[1] = { 0.0 }, cb[1] = { 0.0 }, cj[1] = { 0.0 };
    yaep__scr s;
    memset(&d, 0, sizeof(d));
    d.n_dims = 1;
    d.lo[0] = -1.0; d.hi[0] = 1.0;
    d.acq = YAEP_ACQ_EAVC;
    d.target_p = 0.75;
    d.n_candidates = 9;
    d.hyper.lengthscale[0] = 0.45;
    d.hyper.outputscale = 1.5;
    d.stop_trials = n;
    CHECK(yaep_open(&g, &d), "open look-ahead: %s", yaep_error(&g));
    for (int i = 0; i < n; i++)
        CHECK(yaep_update(&g, xs + i, ys[i]) == YAEP_OK, "update %d", i);
    M = yaep_n_candidates(&g);
    CHECK(M == 9, "candidate count %d", M);
    yaep__cand_cov_build(&g, 0);
    yaep__scr_of(&g, &s);
    yaep_candidate(&g, ja, ca);
    yaep_candidate(&g, jb, cb);
    yaep_candidate(&g, jc, cj);
    /* The candidate covariance itself, against k** - k*' (K + W^-1)^-1 k*. */
    {
        double Kw[64], kia[8], kib[8], v[8], acc = 0.0;
        for (int i = 0; i < n; i++)
            for (int j = 0; j < n; j++)
                Kw[i * n + j] = yaep__kernel(&g, xs + i, xs + j) +
                                (i == j ? 1e-6 + 1.0 / g.W[i] : 0.0);
        tinv(Kw, n);
        for (int i = 0; i < n; i++) {
            kia[i] = yaep__kernel(&g, xs + i, ca);
            kib[i] = yaep__kernel(&g, xs + i, cb);
        }
        for (int i = 0; i < n; i++) {
            v[i] = 0.0;
            for (int j = 0; j < n; j++) v[i] += Kw[i * n + j] * kib[j];
            acc += 0.0;
        }
        for (int i = 0; i < n; i++) acc += kia[i] * v[i];
        track(&worst_look, fabs((double)g.cand_cov[ja * M + jb] -
                                (yaep__kernel(&g, ca, cb) - acc)));
        CLOSE((double)g.cand_cov[ja * M + jb], yaep__kernel(&g, ca, cb) - acc,
              1e-10 * tolx, "candidate covariance (%d,%d)", ja, jb);
    }
    cov_ab = g.cand_cov[ja * M + jb];
    cov_aj = g.cand_cov[ja * M + jc];
    cov_jb = g.cand_cov[jc * M + jb];
    cov_jj = g.cand_cov[jc * M + jc];
    /* An arbitrary but fixed site precision, as the look-ahead would use. */
    cw = 0.37;
    want = cov_ab - cov_aj * cov_jb * cw / (1.0 + cw * cov_jj);
    {
        /* Brute force: the same posterior with the candidate appended as an
         * (n + 1)-th site of precision cw. */
        int m = n + 1;
        double kia[8], kib[8], v[8], acc = 0.0;
        double xall[8] = { 0.0 };
        for (int i = 0; i < n; i++) xall[i] = xs[i];
        xall[n] = cj[0];
        for (int i = 0; i < m; i++)
            for (int j = 0; j < m; j++)
                A[i * m + j] = yaep__kernel(&g, xall + i, xall + j) +
                               (i == j ? 1e-6 + (i < n ? 1.0 / g.W[i] : 1.0 / cw)
                                       : 0.0);
        tinv(A, m);
        for (int i = 0; i < m; i++) {
            kia[i] = yaep__kernel(&g, xall + i, ca);
            kib[i] = yaep__kernel(&g, xall + i, cb);
        }
        for (int i = 0; i < m; i++) {
            v[i] = 0.0;
            for (int j = 0; j < m; j++) v[i] += A[i * m + j] * kib[j];
        }
        for (int i = 0; i < m; i++) acc += kia[i] * v[i];
        got = yaep__kernel(&g, ca, cb) - acc;
    }
    track(&worst_look, fabs(want - got));
    CLOSE(want, got, 1e-10 * tolx, "rank-one look-ahead covariance");
    /* The diagonal case is the look-ahead variance the acquisitions use. */
    want = cov_jj - cov_jj * cov_jj * cw / (1.0 + cw * cov_jj);
    CLOSE(want, cov_jj / (1.0 + cw * cov_jj), 1e-13, "look-ahead variance form");
    yaep_close(&g);
}

/* --- 8: memory_size is exact -------------------------------------------- */

static void test_memory(void) {
    yaep_desc d;
    yaep_gp g;
    size_t need;
    void* buf;
    memset(&d, 0, sizeof(d));
    d.n_dims = 2;
    d.lo[0] = 0.0; d.hi[0] = 1.0;
    d.lo[1] = 0.0; d.hi[1] = 1.0;
    d.acq = YAEP_ACQ_EAVC;
    d.target_p = 0.75;
    d.n_candidates = 64;
    d.max_trials = 40;
    d.stop_trials = 40;
    need = yaep_memory_size(&d);
    CHECK(need > 0, "memory_size returned 0 for a valid desc");
    buf = malloc(need);
    d.memory = buf;
    d.memory_size = need;
    CHECK(yaep_open(&g, &d), "open with exactly yaep_memory_size() bytes: %s",
          yaep_error(&g));
    yaep_close(&g);
    d.memory_size = need - 1;
    CHECK(!yaep_open(&g, &d), "open accepted one byte less than the size");
    free(buf);
    /* A desc the header rejects has no size. */
    d.memory = NULL;
    d.memory_size = 0;
    d.n_dims = 0;
    CHECK(yaep_memory_size(&d) == 0, "memory_size of an invalid desc");
}

/* --- 9: Halton ---------------------------------------------------------- */

static void test_halton(void) {
    CLOSE(yaep__radinv(1, 2), 0.5, 1e-15, "halton base 2 point 1");
    CLOSE(yaep__radinv(2, 2), 0.25, 1e-15, "halton base 2 point 2");
    CLOSE(yaep__radinv(3, 2), 0.75, 1e-15, "halton base 2 point 3");
    CLOSE(yaep__radinv(1, 3), 1.0 / 3.0, 1e-15, "halton base 3 point 1");
    CLOSE(yaep__radinv(2, 3), 2.0 / 3.0, 1e-15, "halton base 3 point 2");
    CLOSE(yaep__radinv(3, 3), 1.0 / 9.0, 1e-15, "halton base 3 point 3");
    CHECK(yaep__nth_prime(0) == 2 && yaep__nth_prime(1) == 3 &&
          yaep__nth_prime(2) == 5 && yaep__nth_prime(7) == 19,
          "primes: %u %u %u %u", yaep__nth_prime(0), yaep__nth_prime(1),
          yaep__nth_prime(2), yaep__nth_prime(7));
}

/* --- 10: open rejects bad descs ----------------------------------------- */

static void test_open_reject(void) {
    yaep_gp g;
    yaep_desc ok;
    memset(&ok, 0, sizeof(ok));
    ok.n_dims = 2;
    ok.lo[0] = 0.0; ok.hi[0] = 1.0;
    ok.lo[1] = 0.0; ok.hi[1] = 1.0;
    ok.target_p = 0.75;
    ok.stop_trials = 20;
    CHECK(yaep_open(&g, &ok), "the reference desc must open: %s", yaep_error(&g));
    yaep_close(&g);
#define REJECT(mut, what)                                                     \
    do {                                                                      \
        yaep_desc d = ok;                                                    \
        mut;                                                                  \
        CHECK(!yaep_open(&g, &d), "open accepted %s", what);                 \
        yaep_close(&g);                                                      \
    } while (0)
    REJECT(d.n_dims = 0, "n_dims = 0");
    REJECT(d.n_dims = YAEP_MAX_DIMS + 1, "n_dims over the ceiling");
    REJECT(d.hi[1] = d.lo[1], "an empty box dimension");
    REJECT(d.intensity_dim = 2, "intensity_dim out of range");
    REJECT(d.stop_trials = 0, "no stop criterion");
    REJECT(d.max_trials = YAEP_MAX_TRIALS + 1, "max_trials over the ceiling");
    REJECT(d.lik = YAEP_LIK_ORDINAL, "ORDINAL without n_outcomes");
    REJECT((d.lik = YAEP_LIK_CATEGORICAL, d.n_outcomes = 2),
           "CATEGORICAL with 2 outcomes");
    REJECT((d.lik = YAEP_LIK_ORDINAL, d.n_outcomes = 4, d.target_outcome = 4),
           "target_outcome out of range");
    REJECT((d.acq = YAEP_ACQ_EAVC, d.target_p = 0.0),
           "a level-set acquisition without target_p");
    REJECT(d.target_p = 1.5, "target_p above 1");
    REJECT(d.hyper.outputscale = -1.0, "a negative output scale");
    REJECT(d.grid[1] = 3, "a product grid with a zero dimension");
    REJECT((d.candidates = ok.lo, d.n_candidates = 0),
           "an explicit candidate set with no count");
    REJECT((d.acq = YAEP_ACQ_EAVC, d.target_p = 0.75, d.n_candidates = 100000),
           "more look-ahead candidates than the cap");
    REJECT(d.n_candidates = -1, "a negative candidate count");
    REJECT(d.refit_every = -1, "a negative refit_every");
#undef REJECT
    CHECK(!yaep_open(NULL, &ok), "open accepted a null handle");
    memset(&g, 0, sizeof(g));
    CHECK(!yaep_open(&g, NULL), "open accepted a null desc");
    CHECK(yaep_n_trials(&g) == YAEP_ERR_CLOSED, "n_trials on a closed handle");
    CHECK(yaep_update(&g, ok.lo, 1) == YAEP_ERR_CLOSED, "update when closed");
    /* v0.15: a failing handle-changing call writes the message; close is
     * safe on a zeroed handle, after a failed open and twice. */
    CHECK(strstr(yaep_error(&g), "yaep_update") != NULL, "update message: %s", yaep_error(&g));
    yaep_close(&g);
    yaep_close(&g);
    yaep_close(NULL);
    CHECK(yaep_open(&g, &ok), "reopen: %s", yaep_error(&g));
    CHECK(yaep_error(&g)[0] == '\0', "open left a message: %s", yaep_error(&g));
    CHECK(yaep_update(&g, ok.lo, 7) == YAEP_ERR_ARG, "update with outcome 7");
    CHECK(strstr(yaep_error(&g), "yaep_update") != NULL, "update message: %s", yaep_error(&g));
    CHECK(yaep_next(&g, NULL) == YAEP_ERR_CLOSED, "next with a null x");
    CHECK(strstr(yaep_error(&g), "yaep_next") != NULL, "next message: %s", yaep_error(&g));
    CHECK(yaep_update_real(&g, ok.lo, 0.5) == YAEP_ERR_ARG, "update_real under BERNOULLI");
    CHECK(strstr(yaep_error(&g), "yaep_update_real") != NULL, "update_real message");
    yaep_close(&g);
    yaep_close(&g);
    CHECK(!yaep_is_open(&g), "open after close");
}

/* --- 11: a simulated 1-D probit observer -------------------------------- */

/* The observer: p(yes) = Phi(slope (x - x50)), so the 75% point is at
 * x50 + 0.6744898 / slope. */
#define OBS_SLOPE 8.0
#define OBS_X50   0.40
#define OBS_THR75 (OBS_X50 + 0.67448975019608171 / OBS_SLOPE)

static int observe(double x) {
    double p[2] = { 0.0 };
    p[1] = 0.5 * erfc(-(OBS_SLOPE * (x - OBS_X50)) / sqrt(2.0));
    p[0] = 1.0 - p[1];
    return yaep_simulate_outcome(p, 2, rng_u());
}

/* desc.refine_steps for run_observer(), so the refinement test can rerun the
 * same streams through it. */
static int obs_refine = 0;

static double run_observer(yaep_acq acq, int trials, const char* name, int rep) {
    yaep_desc d;
    yaep_gp g;
    double thr = 0.0, lo, hi;
    int rc;
    rng_state = 0x243F6A8885A308D3ull + (uint64_t)rep * 0x1000193ull;
    memset(&d, 0, sizeof(d));
    d.n_dims = 1;
    d.lo[0] = 0.0; d.hi[0] = 1.0;
    d.acq = acq;
    d.target_p = 0.75;
    d.n_candidates = 65;
    d.grid[0] = 65;
    d.n_init = 8;
    d.fit = true;
    d.fit_every = 10;
    d.stop_trials = trials;
    d.refine_steps = obs_refine;
    if (!yaep_open(&g, &d)) {
        CHECK(0, "open %s: %s", name, yaep_error(&g));
        return 1e9;
    }
    while (!yaep_done(&g)) {
        double x[1] = { 0.0 };
        int idx = yaep_next(&g, x);
        CHECK(idx >= -1, "%s next returned %d", name, idx);
        rc = yaep_update(&g, x, observe(x[0]));
        CHECK(rc == YAEP_OK, "%s update: %s", name, yaep_strerror(rc));
    }
    CHECK(yaep_n_trials(&g) == trials, "%s ran %d trials", name,
          yaep_n_trials(&g));
    rc = yaep_threshold(&g, NULL, 0.0, &thr, &lo, &hi);
    CHECK(rc == YAEP_OK, "%s threshold: %s", name, yaep_strerror(rc));
    CHECK(lo <= thr + 1e-9 && thr <= hi + 1e-9,
          "%s band [%g, %g] does not contain %g", name, lo, hi, thr);
    printf("  %-5s stream %d: threshold %.4f (true %.4f, error %+.4f), "
           "band %.3f wide, log marginal %.2f\n", name, rep, thr,
           OBS_THR75, thr - OBS_THR75, hi - lo, yaep_log_marginal(&g));
    yaep_close(&g);
    return fabs(thr - OBS_THR75);
}

/* The mean absolute threshold error over a few response streams. One stream
 * says nothing: the binomial noise of a hundred trials near the 75% point is
 * worth about 0.02 of stimulus here, so a single run lands anywhere inside 0.1
 * and a tolerance tight enough to be interesting would only be measuring the
 * seed. */
static double mean_error(yaep_acq acq, int trials, const char* name, int reps) {
    double acc = 0.0;
    for (int r = 0; r < reps; r++) acc += run_observer(acq, trials, name, r);
    return acc / (double)reps;
}

/* --- 12: the other three likelihoods recover their field ---------------- */

static void test_gaussian_field(void) {
    yaep_desc d;
    yaep_gp g;
    double rms = 0.0;
    int n = 0;
    rng_state = 0x243F6A8885A308D3ull + 0x44ull;
    memset(&d, 0, sizeof(d));
    d.n_dims = 1;
    d.lo[0] = 0.0; d.hi[0] = 1.0;
    d.lik = YAEP_LIK_GAUSSIAN;
    d.acq = YAEP_ACQ_BALV;
    d.target_value = 0.5;
    d.grid[0] = 33;
    d.n_init = 6;
    d.fit = true;
    d.fit_every = 20;
    d.stop_trials = 60;
    CHECK(yaep_open(&g, &d), "open GAUSSIAN field: %s", yaep_error(&g));
    while (!yaep_done(&g)) {
        double x[1] = { 0.0 };
        yaep_next(&g, x);
        CHECK(yaep_update_real(&g, x, sin(4.0 * x[0]) + 0.05 * (rng_u() - 0.5))
              == YAEP_OK, "gaussian field update");
    }
    for (int i = 0; i <= 20; i++) {
        double x = (double)i / 20.0, e;
        e = yaep_predict_p(&g, &x) - sin(4.0 * x);
        rms += e * e;
        n++;
    }
    rms = sqrt(rms / n);
    printf("  gaussian field rms error %.4f\n", rms);
    CHECK(rms < 0.05, "GAUSSIAN field rms error %.4f", rms);
    yaep_close(&g);
}

static void test_ordinal_field(void) {
    yaep_desc d;
    yaep_gp g;
    double cut[3] = { 0.0, 1.0, 2.0 };
    double rms = 0.0;
    int n = 0;
    rng_state = 0x243F6A8885A308D3ull + 0x55ull;
    memset(&d, 0, sizeof(d));
    d.n_dims = 1;
    d.lo[0] = 0.0; d.hi[0] = 1.0;
    d.lik = YAEP_LIK_ORDINAL;
    d.n_outcomes = 4;
    d.target_outcome = 2;
    d.target_p = 0.5;
    d.acq = YAEP_ACQ_BALD;
    d.grid[0] = 33;
    d.n_init = 8;
    d.fit = true;
    d.fit_every = 20;
    d.hyper.cutpoint[0] = cut[0];
    d.hyper.cutpoint[1] = cut[1];
    d.hyper.cutpoint[2] = cut[2];
    d.stop_trials = 150;
    CHECK(yaep_open(&g, &d), "open ORDINAL: %s", yaep_error(&g));
    while (!yaep_done(&g)) {
        double x[1], p[4], f;
        int k;
        yaep_next(&g, x);
        f = 4.0 * (x[0] - 0.5);   /* the simulated latent */
        {
            double prev = 0.0;
            for (int j = 0; j < 3; j++) {
                double c = 0.5 * erfc(-(cut[j] - f) / sqrt(2.0));
                p[j] = c - prev;
                prev = c;
            }
            p[3] = 1.0 - prev;
        }
        k = yaep_simulate_outcome(p, 4, rng_u());
        CHECK(yaep_update(&g, x, k) == YAEP_OK, "ordinal update");
    }
    for (int i = 0; i <= 20; i++) {
        double x = (double)i / 20.0, e, want;
        want = 1.0 - 0.5 * erfc(-(cut[1] - 4.0 * (x - 0.5)) / sqrt(2.0));
        e = yaep_predict_p(&g, &x) - want;   /* P(y >= 2) */
        rms += e * e;
        n++;
    }
    rms = sqrt(rms / n);
    printf("  ordinal P(y >= 2) rms error %.4f\n", rms);
    CHECK(rms < 0.12, "ORDINAL field rms error %.4f", rms);
    yaep_close(&g);
}

static void test_categorical_field(void) {
    yaep_desc d;
    yaep_gp g;
    double rms = 0.0;
    int n = 0;
    rng_state = 0x243F6A8885A308D3ull + 0x66ull;
    memset(&d, 0, sizeof(d));
    d.n_dims = 1;
    d.lo[0] = 0.0; d.hi[0] = 1.0;
    d.lik = YAEP_LIK_CATEGORICAL;
    d.n_outcomes = 3;
    d.target_outcome = 1;
    d.target_p = 0.5;
    d.acq = YAEP_ACQ_BALV;
    d.grid[0] = 25;
    d.n_init = 10;
    d.stop_trials = 120;
    CHECK(yaep_open(&g, &d), "open CATEGORICAL: %s", yaep_error(&g));
    while (!yaep_done(&g)) {
        double x[1], p[3], fs[3], m;
        int k;
        yaep_next(&g, x);
        fs[0] = 2.0 - 4.0 * x[0];
        fs[1] = 0.0;
        fs[2] = -2.0 + 4.0 * x[0];
        m = fs[0] > fs[2] ? fs[0] : fs[2];
        {
            double sum = 0.0;
            for (int c = 0; c < 3; c++) { p[c] = exp(fs[c] - m); sum += p[c]; }
            for (int c = 0; c < 3; c++) p[c] /= sum;
        }
        k = yaep_simulate_outcome(p, 3, rng_u());
        CHECK(yaep_update(&g, x, k) == YAEP_OK, "categorical update");
    }
    for (int i = 0; i <= 20; i++) {
        double x = (double)i / 20.0, p[3], want[3], fs[3], sum = 0.0;
        CHECK(yaep_predict_outcomes(&g, &x, p) == YAEP_OK, "predict_outcomes");
        fs[0] = 2.0 - 4.0 * x;
        fs[1] = 0.0;
        fs[2] = -2.0 + 4.0 * x;
        for (int c = 0; c < 3; c++) { want[c] = exp(fs[c]); sum += want[c]; }
        for (int c = 0; c < 3; c++) {
            double e = p[c] - want[c] / sum;
            rms += e * e;
            n++;
        }
    }
    rms = sqrt(rms / n);
    printf("  categorical class probability rms error %.4f\n", rms);
    CHECK(rms < 0.15, "CATEGORICAL field rms error %.4f", rms);
    /* The softmax mode satisfies the same stationarity condition as the
     * one-latent case, one block per class: f_c = m + K (y_c - pi_c). It is
     * the one cheap check on the whole of R&W Algorithm 3.3. */
    {
        int nt = yaep_n_trials(&g), ld = g.N_max;
        const yaep_trial* h = yaep_history(&g, &nt);
        double* Kd = (double*)malloc((size_t)nt * nt * sizeof(double));
        for (int i = 0; i < nt; i++)
            for (int j = 0; j < nt; j++)
                Kd[i * nt + j] = yaep__kernel(&g, h[i].x, h[j].x) +
                                 (i == j ? 1e-6 : 0.0);
        for (int c = 0; c < 3; c++)
            for (int i = 0; i < nt; i++) {
                double acc = g.hyper.mean;
                for (int j = 0; j < nt; j++) {
                    double fs[3], m2, sum = 0.0, pic;
                    for (int cc = 0; cc < 3; cc++) fs[cc] = g.f[cc * ld + j];
                    m2 = fs[0] > fs[1] ? fs[0] : fs[1];
                    if (fs[2] > m2) m2 = fs[2];
                    for (int cc = 0; cc < 3; cc++) sum += exp(fs[cc] - m2);
                    pic = exp(fs[c] - m2) / sum;
                    acc += Kd[i * nt + j] *
                           (((int)(h[j].y + 0.5) == c ? 1.0 : 0.0) - pic);
                }
                track(&worst_stat, fabs(g.f[c * ld + i] - acc));
                CLOSE(g.f[c * ld + i], acc, 1e-7 * tolx,
                      "softmax stationarity at class %d site %d", c, i);
            }
        free(Kd);
    }
    yaep_close(&g);
}

/* --- 13: the hyperparameter fit raises the marginal likelihood ---------- */

static void test_fit(void) {
    yaep_desc d;
    yaep_gp g;
    double before = 0.0, after = 0.0;
    int rc;
    rng_state = 0x243F6A8885A308D3ull + 0x77ull;
    memset(&d, 0, sizeof(d));
    d.n_dims = 1;
    d.lo[0] = 0.0; d.hi[0] = 1.0;
    d.n_candidates = 33;
    d.grid[0] = 33;
    d.target_p = 0.75;
    d.n_init = 30;
    d.stop_trials = 30;
    CHECK(yaep_open(&g, &d), "open fit: %s", yaep_error(&g));
    while (!yaep_done(&g)) {
        double x[1] = { 0.0 };
        yaep_next(&g, x);
        CHECK(yaep_update(&g, x, observe(x[0])) == YAEP_OK, "fit update");
    }
    before = yaep_log_marginal(&g);
    rc = yaep_fit(&g);
    CHECK(rc >= 0, "yaep_fit: %s", yaep_strerror(rc));
    after = yaep_log_marginal(&g);
    printf("  fit raised the log marginal from %.4f to %.4f\n", before, after);
    /* The fit maximizes the posterior, so the likelihood alone is allowed to
     * give a little back to the priors; it must not collapse, and it must move. */
    CHECK(after > before - 1.0, "the fit cost %.3f nats of likelihood",
          before - after);
    CHECK(fabs(after - before) > 1e-6, "the fit did not move the log marginal");
    {
        yaep_hyper h;
        memset(&h, 0, sizeof(h));
        memset(&h, 0, sizeof(h));
        CHECK(yaep_get_hyper(&g, &h) == YAEP_OK, "get_hyper");
        CHECK(h.lengthscale[0] > 0.0 && h.outputscale > 0.0,
              "fitted hyperparameters are not positive");
    }
    yaep_close(&g);
}

/* A fit on the schedule must not break the loop, and ties drawn from the
 * caller's generator must stay inside the candidate set. */
static void test_fit_every_and_rng(void) {
    yaep_desc d;
    yaep_gp g;
    rng_state = 0x243F6A8885A308D3ull + 0x88ull;
    memset(&d, 0, sizeof(d));
    d.n_dims = 2;
    d.lo[0] = 0.0; d.hi[0] = 1.0;
    d.lo[1] = 0.0; d.hi[1] = 1.0;
    d.intensity_dim = 1;
    d.kernel = YAEP_KERNEL_SEMIP;
    d.acq = YAEP_ACQ_LOCALMI;
    d.target_p = 0.75;
    d.n_candidates = 128;
    d.n_init = 6;
    d.fit = true;
    d.fit_every = 5;
    d.rng = rng_cb;
    d.stop_trials = 40;
    CHECK(yaep_open(&g, &d), "open fit_every: %s", yaep_error(&g));
    while (!yaep_done(&g)) {
        double x[2] = { 0.0 };
        int idx = yaep_next(&g, x);
        int rc;
        CHECK(idx >= -1 && idx < yaep_n_candidates(&g), "next index %d", idx);
        rc = yaep_update(&g, x, observe(x[1]));
        CHECK(rc == YAEP_OK, "fit_every update: %s", yaep_strerror(rc));
    }
    CHECK(yaep_n_trials(&g) == 40, "fit_every trials %d", yaep_n_trials(&g));
    {
        double ctx[1] = { 0.5 }, thr, lo, hi;
        int rc = yaep_threshold(&g, ctx, 0.0, &thr, &lo, &hi);
        CHECK(rc == YAEP_OK || rc == YAEP_ERR_NOCROSS, "semip threshold: %s",
              yaep_strerror(rc));
    }
    /* A subset call must return one of the indices it was given. yaep_done()
     * has fired, which does not stop the loop functions from working. */
    {
        int sub[3] = { 4, 9, 17 };
        double x[2] = { 0.0 };
        int idx;
        idx = yaep_next_subset(&g, sub, 3, x);
        CHECK(idx == 4 || idx == 9 || idx == 17, "next_subset returned %d", idx);
        CHECK(!(yaep_acq_score(&g, 4) != yaep_acq_score(&g, 4)),
              "acq_score is not a number");
    }
    yaep_close(&g);
}

/* --- the loop bookkeeping ----------------------------------------------- */

/* The same gradient check with the logistic link, which has its own
 * derivative chain for both the Bernoulli and the ordinal likelihood, and
 * again with a floor and a ceiling on the link, which add terms to all three
 * f-derivatives and to the cutpoint partials. */
static void test_gradient_logit(void) {
    yaep_link_gradient_probe = YAEP_LINK_LOGIT;
    test_gradient(YAEP_LIK_BERNOULLI, YAEP_KERNEL_RBF, "bernoulli/logit");
    test_gradient(YAEP_LIK_ORDINAL, YAEP_KERNEL_RBF, "ordinal/logit");
    yaep_link_gradient_probe = YAEP_LINK_PROBIT;
    yaep_guess_gradient_probe = 0.5;
    yaep_lapse_gradient_probe = 0.02;
    test_gradient(YAEP_LIK_BERNOULLI, YAEP_KERNEL_RBF, "bernoulli/2afc");
    test_gradient(YAEP_LIK_BERNOULLI, YAEP_KERNEL_SEMIP, "bernoulli/2afc/semip");
    test_gradient(YAEP_LIK_ORDINAL, YAEP_KERNEL_RBF, "ordinal/2afc");
    yaep_link_gradient_probe = YAEP_LINK_LOGIT;
    test_gradient(YAEP_LIK_BERNOULLI, YAEP_KERNEL_RBF, "bernoulli/2afc/logit");
    test_gradient(YAEP_LIK_ORDINAL, YAEP_KERNEL_RBF, "ordinal/2afc/logit");
    yaep_link_gradient_probe = YAEP_LINK_PROBIT;
    yaep_guess_gradient_probe = 0.0;
    yaep_lapse_gradient_probe = 0.0;
}

/* A two-interval task: the observer cannot score below 0.5, and the 75% point
 * of p = 0.5 + 0.5 Phi(slope (x - x50)) sits exactly at x50, so the true
 * threshold is known without inverting anything. */
static double afc_slope = 8.0, afc_x50 = 0.40;
/* p(correct) = 0.75 at 0.5 + 0.48 link = 0.75, i.e. link = 0.5208..., so the
 * true 75% threshold is a little above x50. */
#define AFC_THR75 (0.40 + 0.052248 / 8.0)

static int observe_afc(double x) {
    double p[2] = { 0.0 };
    p[1] = 0.5 + 0.48 * 0.5 * erfc(-(afc_slope * (x - afc_x50)) / sqrt(2.0));
    p[0] = 1.0 - p[1];
    return yaep_simulate_outcome(p, 2, rng_u());
}

static void test_2afc(void) {
    double acc = 0.0;
    int reps = 5;
    printf("two-interval observer (guess 0.5, lapse 0.02), 120 trials:\n");
    for (int r = 0; r < reps; r++) {
        yaep_desc d;
        yaep_gp g;
        double thr = 0.0, lo = 0.0, hi = 0.0;
        int rc;
        rng_state = 0x243F6A8885A308D3ull + 0xcc00ull + (uint64_t)r * 0x1000193ull;
        memset(&d, 0, sizeof(d));
        d.n_dims = 1;
        d.lo[0] = 0.0; d.hi[0] = 1.0;
        d.guess = 0.5;
        d.lapse = 0.02;
        d.acq = YAEP_ACQ_EAVC;
        d.target_p = 0.75;      /* = 0.5 + 0.48 * 0.5208..., i.e. x50 */
        d.n_candidates = 65;
        d.grid[0] = 65;
        /* A longer init phase and a later first fit than the yes/no runs get:
         * two-interval trials carry less information, and an unregularized fit
         * on eight of them can decide the whole field is at chance. MODEL in
         * the header says what that looks like and what else to do about it. */
        d.n_init = 16;
        d.fit = true;
        d.fit_every = 20;
        d.stop_trials = 120;
        CHECK(yaep_open(&g, &d), "open 2afc: %s", yaep_error(&g));
        while (!yaep_done(&g)) {
            double x[1] = { 0.0 };
            yaep_next(&g, x);
            CHECK(yaep_update(&g, x, observe_afc(x[0])) == YAEP_OK, "2afc update");
        }
        rc = yaep_threshold(&g, NULL, 0.0, &thr, &lo, &hi);
        CHECK(rc == YAEP_OK, "2afc threshold: %s", yaep_strerror(rc));
        printf("  stream %d: threshold %.4f (true %.4f, error %+.4f), "
               "band %.3f wide\n", r, thr, AFC_THR75, thr - AFC_THR75, hi - lo);
        acc += fabs(thr - AFC_THR75);
        /* The floor has to show up in the predictions themselves. */
        {
            double x0[1] = { 0.0 }, p[2];
            CHECK(yaep_predict_p(&g, x0) >= 0.5,
                  "the target quantity fell below the floor");
            CHECK(yaep_predict_outcomes(&g, x0, p) == YAEP_OK, "2afc outcomes");
            CLOSE(p[0] + p[1], 1.0, 1e-12, "2afc probabilities sum to 1");
            CHECK(p[1] >= 0.5 && p[1] <= 0.98 + 1e-12,
                  "2afc p(correct) = %g is outside the floor and ceiling", p[1]);
        }
        yaep_close(&g);
    }
    acc /= reps;
    printf("  mean absolute error %.4f\n", acc);
    CHECK(acc < 0.08, "2afc mean threshold error %.4f", acc);
}

static void test_loop_rules(void) {
    yaep_desc d;
    yaep_gp g;
    double x1[1] = { 0.0 }, x2[1] = { 0.0 };
    int i1, i2, n;
    const yaep_trial* h;
    memset(&d, 0, sizeof(d));
    d.n_dims = 1;
    d.lo[0] = 0.0; d.hi[0] = 1.0;
    d.n_candidates = 17;
    d.target_p = 0.75;
    d.n_init = 2;
    d.max_trials = 4;
    d.stop_trials = 4;
    CHECK(yaep_open(&g, &d), "open loop rules: %s", yaep_error(&g));
    i1 = yaep_next(&g, x1);
    i2 = yaep_next(&g, x2);
    CHECK(i1 == i2 && x1[0] == x2[0], "next twice without an update moved");
    CHECK(yaep_update(&g, x1, 1) == YAEP_OK, "update");
    h = yaep_history(&g, &n);
    CHECK(n == 1, "history length %d", n);
    CHECK(h[0].proposed == 1, "the trial was not marked as proposed");
    CHECK(h[0].init == 1, "the first trial is an init trial");
    /* A stimulus the caller changed is recorded, and marked as not proposed. */
    yaep_next(&g, x1);
    x2[0] = x1[0] * 0.5;
    CHECK(yaep_update(&g, x2, 0) == YAEP_OK, "update with a shifted stimulus");
    h = yaep_history(&g, &n);
    CHECK(h[1].proposed == 0, "a changed stimulus was marked as proposed");
    CLOSE(h[1].x[0], x2[0], 0.0, "the history records what was shown");
    /* Argument checks. */
    CHECK(yaep_update(&g, x1, 5) == YAEP_ERR_ARG, "outcome out of range");
    CHECK(yaep_update_real(&g, x1, 0.5) == YAEP_ERR_ARG,
          "update_real under a discrete likelihood");
    x2[0] = 2.0;
    CHECK(yaep_update(&g, x2, 1) == YAEP_ERR_ARG, "a stimulus outside the box");
    /* Fill to max_trials and check the ceiling. */
    x2[0] = 0.5;
    CHECK(yaep_update(&g, x2, 1) == YAEP_OK, "update 3");
    CHECK(yaep_update(&g, x2, 0) == YAEP_OK, "update 4");
    CHECK(yaep_stop_reason(&g) == YAEP_STOP_FULL, "stop reason %d",
          (int)yaep_stop_reason(&g));
    CHECK(yaep_update(&g, x2, 1) == YAEP_ERR_FULL, "update past max_trials");
    CHECK(yaep_next(&g, x1) == YAEP_ERR_FULL, "next past max_trials");
    yaep_close(&g);
    CHECK(!yaep_is_open(&g), "the handle is still open after close");
}

/* The configurations the checks above do not reach: a caller-owned candidate
 * list, the random acquisition, the logit link, the threshold-width stop rule,
 * and the caller's own memory. */
static void test_configurations(void) {
    yaep_desc d;
    yaep_gp g;
    static double cands[11];
    /* Static, not automatic: a megabyte of stack is more than MSVC's default
     * thread gets. */
    static unsigned char buf[1 << 20];
    size_t need;
    int seen_stop = 0;
    rng_state = 0x243F6A8885A308D3ull + 0x99ull;
    for (int i = 0; i < 11; i++) cands[i] = 0.1 * (double)i;
    memset(&d, 0, sizeof(d));
    d.n_dims = 1;
    d.lo[0] = 0.0; d.hi[0] = 1.0;
    d.link = YAEP_LINK_LOGIT;
    d.acq = YAEP_ACQ_RANDOM;
    d.candidates = cands;
    d.n_candidates = 11;
    d.n_init = 4;
    d.rng = rng_cb;
    d.fit = true;
    d.fit_every = 10;
    d.max_trials = 60;
    d.stop_trials = 0;
    d.stop_threshold_sd = 0.6;
    need = yaep_memory_size(&d);
    CHECK(need > 0 && need < sizeof(buf), "configuration memory %lu bytes",
          (unsigned long)need);
    d.memory = buf;
    d.memory_size = sizeof(buf);
    CHECK(yaep_open(&g, &d), "open configurations: %s", yaep_error(&g));
    CHECK(yaep_n_candidates(&g) == 11, "candidate count");
    {
        double c[1] = { 0.0 };
        CHECK(yaep_candidate(&g, 7, c) == YAEP_OK, "yaep_candidate");
        CLOSE(c[0], 0.7, 1e-15, "the caller's candidate list is what is used");
        CHECK(yaep_candidate(&g, 11, c) == YAEP_ERR_ARG, "candidate index");
    }
    while (!yaep_done(&g)) {
        double x[1] = { 0.0 };
        int idx = yaep_next(&g, x);
        CHECK(idx == -1, "RANDOM proposed candidate %d, not a free point", idx);
        CHECK(x[0] >= 0.0 && x[0] <= 1.0, "a point outside the box");
        CHECK(yaep_update(&g, x, observe(x[0])) == YAEP_OK, "random update");
        if (yaep_n_trials(&g) >= 60) break;
    }
    seen_stop = (int)yaep_stop_reason(&g);
    CHECK(seen_stop == (int)YAEP_STOP_THRESHOLD_SD ||
          seen_stop == (int)YAEP_STOP_FULL,
          "stop reason %d after %d trials", seen_stop, yaep_n_trials(&g));
    printf("  logit / random / caller candidates: %d trials, stop reason %d\n",
           yaep_n_trials(&g), seen_stop);
    {   /* The outcome probabilities are a distribution. */
        double x[1] = { 0.45 }, p[2];
        CHECK(yaep_predict_outcomes(&g, x, p) == YAEP_OK, "predict_outcomes");
        CLOSE(p[0] + p[1], 1.0, 1e-12, "Bernoulli probabilities sum to 1");
        CLOSE(p[1], yaep_predict_p(&g, x), 1e-12, "p[1] is the target quantity");
        CHECK(yaep_predict_p_var(&g, x) >= 0.0, "a negative variance");
    }
    yaep_close(&g);
    /* The same, ordinal, to check that the K probabilities are a distribution
     * and that the target quantity is their upper tail. */
    memset(&d, 0, sizeof(d));
    d.n_dims = 1;
    d.lo[0] = 0.0; d.hi[0] = 1.0;
    d.lik = YAEP_LIK_ORDINAL;
    d.n_outcomes = 4;
    d.target_outcome = 2;
    d.target_p = 0.5;
    d.acq = YAEP_ACQ_LOCALMI;
    d.n_candidates = 21;
    d.n_init = 6;
    d.stop_trials = 24;
    CHECK(yaep_open(&g, &d), "open ordinal outcomes: %s", yaep_error(&g));
    while (!yaep_done(&g)) {
        double x[1] = { 0.0 };
        yaep_next(&g, x);
        CHECK(yaep_update(&g, x, (int)(rng_u() * 4.0)) == YAEP_OK,
              "ordinal outcome update");
    }
    {
        double x[1] = { 0.6 }, p[4], sum = 0.0;
        CHECK(yaep_predict_outcomes(&g, x, p) == YAEP_OK, "ordinal outcomes");
        for (int i = 0; i < 4; i++) {
            CHECK(p[i] >= 0.0, "ordinal probability %d is negative", i);
            sum += p[i];
        }
        CLOSE(sum, 1.0, 1e-12, "ordinal probabilities sum to 1");
        CLOSE(yaep_predict_p(&g, x), p[2] + p[3], 1e-10,
              "the target quantity is P(y >= 2)");
    }
    yaep_close(&g);
}

/* The stepped fit must land where the whole fit lands, and must leave the model
 * consistent between steps. */
static void test_fit_step(void) {
    yaep_desc d;
    yaep_gp ga, gb;
    double ys[40] = { 0.0 };
    int n = 40, steps = 0, rc;
    rng_state = 0x243F6A8885A308D3ull + 0xaaull;
    memset(&d, 0, sizeof(d));
    d.n_dims = 1;
    d.lo[0] = 0.0; d.hi[0] = 1.0;
    d.n_candidates = 33;
    d.target_p = 0.75;
    d.n_init = 40;
    d.stop_trials = 40;
    CHECK(yaep_open(&ga, &d), "open fit_step a: %s", yaep_error(&ga));
    CHECK(yaep_open(&gb, &d), "open fit_step b: %s", yaep_error(&gb));
    for (int i = 0; i < n; i++) {
        double x[1] = { 0.0 };
        yaep_next(&ga, x);
        ys[i] = (double)observe(x[0]);
        CHECK(yaep_update(&ga, x, (int)ys[i]) == YAEP_OK, "fit_step update a");
        CHECK(yaep_update(&gb, x, (int)ys[i]) == YAEP_OK, "fit_step update b");
    }
    CHECK(yaep_fit(&ga) >= 0, "whole fit");
    while ((rc = yaep_fit_step(&gb)) > 0) {
        double lm = yaep_log_marginal(&gb);
        CHECK(lm == lm, "the stepped fit left the model without a marginal");
        if (++steps > 200) { CHECK(0, "yaep_fit_step does not terminate"); break; }
    }
    CHECK(rc == 0, "yaep_fit_step ended with %d", rc);
    printf("  the stepped fit took %d calls to reach %.6f (whole fit %.6f)\n",
           steps, yaep_log_marginal(&gb), yaep_log_marginal(&ga));
    CLOSE(yaep_log_marginal(&gb), yaep_log_marginal(&ga), 1e-9,
          "stepped and whole fits disagree");
    {
        yaep_hyper ha, hb;
        memset(&ha, 0, sizeof(ha));
        memset(&hb, 0, sizeof(hb));
        memset(&ha, 0, sizeof(ha));
        memset(&hb, 0, sizeof(hb));
        yaep_get_hyper(&ga, &ha);
        yaep_get_hyper(&gb, &hb);
        CLOSE(ha.lengthscale[0], hb.lengthscale[0], 1e-12, "fit lengthscale");
        CLOSE(ha.outputscale, hb.outputscale, 1e-12, "fit output scale");
    }
    yaep_close(&ga);
    yaep_close(&gb);
}

/* desc.refit_every trades an exact Newton refit per trial for a rank-one
 * update. Replaying one response stream into both settings says how much that
 * costs, and yaep_refit() must erase the difference. */
static void test_refit_every(void) {
    yaep_desc d;
    yaep_gp ga, gb;
    double ys[80], worst = 0.0, after = 0.0;
    /* Not a multiple of refit_every: the run has to END on a cheap update, or
     * the comparison only sees the exact refit that closed it. */
    int n = 78;
    rng_state = 0x243F6A8885A308D3ull + 0xbbull;
    memset(&d, 0, sizeof(d));
    d.n_dims = 1;
    d.lo[0] = 0.0; d.hi[0] = 1.0;
    d.acq = YAEP_ACQ_LSE;
    d.target_p = 0.75;
    d.n_candidates = 65;
    d.grid[0] = 65;
    d.n_init = 8;
    d.stop_trials = n;
    CHECK(yaep_open(&ga, &d), "open refit a: %s", yaep_error(&ga));
    d.refit_every = 5;
    CHECK(yaep_open(&gb, &d), "open refit b: %s", yaep_error(&gb));
    for (int i = 0; i < n; i++) {
        double x[1] = { 0.0 };
        yaep_next(&ga, x);          /* one stimulus stream for both handles */
        ys[i] = (double)observe(x[0]);
        CHECK(yaep_update(&ga, x, (int)ys[i]) == YAEP_OK, "refit update a");
        CHECK(yaep_update(&gb, x, (int)ys[i]) == YAEP_OK, "refit update b");
    }
    for (int i = 0; i <= 40; i++) {
        double x = (double)i / 40.0;
        double e = fabs(yaep_predict_p(&ga, &x) - yaep_predict_p(&gb, &x));
        if (e > worst) worst = e;
    }
    CHECK(yaep_refit(&gb) == YAEP_OK, "yaep_refit");
    for (int i = 0; i <= 40; i++) {
        double x = (double)i / 40.0;
        double e = fabs(yaep_predict_p(&ga, &x) - yaep_predict_p(&gb, &x));
        if (e > after) after = e;
    }
    printf("  refit_every 5 vs 1: worst P difference %.2e, after yaep_refit "
           "%.2e\n", worst, after);
    CHECK(worst < 5e-3, "refit_every 5 drifts by %.3g in probability", worst);
    CHECK(after < 1e-8 * tolx,
          "yaep_refit did not restore the exact posterior (%.3g)", after);
    {
        double ta = 0.0, tb = 0.0;
        CHECK(yaep_threshold(&ga, NULL, 0.0, &ta, NULL, NULL) == YAEP_OK,
              "refit threshold a");
        CHECK(yaep_threshold(&gb, NULL, 0.0, &tb, NULL, NULL) == YAEP_OK,
              "refit threshold b");
        CLOSE(ta, tb, 1e-6, "thresholds after yaep_refit");
    }
    yaep_close(&ga);
    yaep_close(&gb);
}

/* The batched predictions must agree with the one-point calls they replace. */
static void test_predict_many(void) {
    yaep_desc d;
    yaep_gp g;
    static double xs[900 * 2], pm[900], mum[900], sdm[900];
    int n = 0, nd = 2;
    double worst_p = 0.0, worst_mu = 0.0, worst_sd = 0.0;
    rng_state = 0x243F6A8885A308D3ull + 0xddull;
    memset(&d, 0, sizeof(d));
    d.n_dims = nd;
    d.lo[0] = 0.0; d.hi[0] = 1.0;
    d.lo[1] = -1.0; d.hi[1] = 1.0;
    d.intensity_dim = 1;
    d.target_p = 0.75;
    d.n_candidates = 64;
    d.n_init = 8;
    d.fit = true;
    d.fit_every = 20;
    d.stop_trials = 40;
    CHECK(yaep_open(&g, &d), "open predict_many: %s", yaep_error(&g));
    while (!yaep_done(&g)) {
        double x[2] = { 0.0 };
        yaep_next(&g, x);
        CHECK(yaep_update(&g, x, observe(x[1])) == YAEP_OK, "many update");
    }
    for (int i = 0; i < 30; i++)
        for (int j = 0; j < 30; j++) {
            xs[n * 2 + 0] = (double)i / 29.0;
            xs[n * 2 + 1] = -1.0 + 2.0 * (double)j / 29.0;
            n++;
        }
    CHECK(yaep_predict_p_many(&g, xs, n, pm) == YAEP_OK, "predict_p_many");
    CHECK(yaep_predict_f_many(&g, xs, n, 0, mum, sdm) == YAEP_OK,
          "predict_f_many");
    for (int i = 0; i < n; i++) {
        double mu1 = 0.0, sd1 = 0.0, p1;
        CHECK(yaep_predict_f(&g, xs + i * 2, 0, &mu1, &sd1) == YAEP_OK,
              "predict_f point %d", i);
        p1 = yaep_predict_p(&g, xs + i * 2);
        if (fabs(p1 - pm[i]) > worst_p) worst_p = fabs(p1 - pm[i]);
        if (fabs(mu1 - mum[i]) > worst_mu) worst_mu = fabs(mu1 - mum[i]);
        if (fabs(sd1 - sdm[i]) > worst_sd) worst_sd = fabs(sd1 - sdm[i]);
    }
    printf("  batched vs one-point over %d points: p %.2e, mu %.2e, sd %.2e\n",
           n, worst_p, worst_mu, worst_sd);
    CHECK(worst_p < 1e-12 * tolx, "predict_p_many differs by %.3g", worst_p);
    CHECK(worst_mu < 1e-12 * tolx, "predict_f_many mean differs by %.3g", worst_mu);
    CHECK(worst_sd < 1e-12 * tolx, "predict_f_many sd differs by %.3g", worst_sd);
    /* One output at a time, and the argument checks. */
    CHECK(yaep_predict_f_many(&g, xs, n, 0, mum, NULL) == YAEP_OK, "mu only");
    CHECK(yaep_predict_f_many(&g, xs, n, 0, NULL, sdm) == YAEP_OK, "sd only");
    CHECK(yaep_predict_f_many(&g, xs, n, 1, mum, sdm) == YAEP_ERR_ARG,
          "latent out of range");
    {
        double bad[2] = { 0.5, 5.0 };
        CHECK(yaep_predict_p_many(&g, bad, 1, pm) == YAEP_ERR_ARG,
              "a point outside the box");
    }
    yaep_close(&g);
}

/* A Cholesky that fails must report it, record the trial anyway, and leave the
 * posterior of the trials that did factor in force. Duplicate stimuli with no
 * jitter and no noise make a singular matrix on purpose. */
static void test_numeric_failure(void) {
    yaep_desc d;
    yaep_gp g;
    double x[1] = { 0.3 }, mu1 = 0.0, sd1 = 0.0, mu2 = 0.0, sd2 = 0.0;
    memset(&d, 0, sizeof(d));
    d.n_dims = 1;
    d.lo[0] = 0.0; d.hi[0] = 1.0;
    d.lik = YAEP_LIK_GAUSSIAN;
    d.acq = YAEP_ACQ_BALV;
    d.hyper.noise_sd = 1e-300;
    d.jitter = 1e-300;
    d.stop_trials = 8;
    CHECK(yaep_open(&g, &d), "open numeric: %s", yaep_error(&g));
    CHECK(yaep_update_real(&g, x, 1.0) == YAEP_OK, "first update");
    CHECK(yaep_predict_f(&g, x, 0, &mu1, &sd1) == YAEP_OK, "predict before");
    CHECK(yaep_update_real(&g, x, 1.0) == YAEP_ERR_NUMERIC,
          "a singular matrix was not reported");
    CHECK(yaep_n_trials(&g) == 2, "the failed trial was not recorded");
    CHECK(yaep_predict_f(&g, x, 0, &mu2, &sd2) == YAEP_OK, "predict after");
    CLOSE(mu2, mu1, 1e-12, "the previous posterior did not stay in force");
    yaep_close(&g);
}

/* --- desc.refine_steps ------------------------------------------------- */

/* The refinement climbs yaep__point_score(), so first that the point score
 * at a candidate IS the candidate's acquisition score (the straddle, for
 * EAVC), then that a refined proposal never scores below the grid winner it
 * started from and stays within one grid step of it, then that it runs under
 * every acquisition and under CATEGORICAL. */
static void refine_session(yaep_acq acq, yaep_lik lik, const char* name) {
    static const double h = 1.0 / 8.0;   /* the 9-point grid's step */
    yaep_desc d;
    yaep_gp g;
    yaep__scr s;
    int refined = 0, rc;
    double worst_id = 0.0;
    rng_state = 0x243F6A8885A308D3ull + 0x77ull + (uint64_t)acq;
    memset(&d, 0, sizeof(d));
    d.n_dims = 1;
    d.lo[0] = 0.0; d.hi[0] = 1.0;
    d.lik = lik;
    if (lik == YAEP_LIK_CATEGORICAL) { d.n_outcomes = 3; d.target_outcome = 1; }
    d.acq = acq;
    d.target_p = lik == YAEP_LIK_CATEGORICAL ? 0.5 : 0.75;
    d.grid[0] = 9;
    d.n_init = 6;
    d.refine_steps = 3;
    d.stop_trials = 40;
    if (!yaep_open(&g, &d)) {
        CHECK(0, "open refine %s: %s", name, yaep_error(&g));
        return;
    }
    yaep__scr_of(&g, &s);
    while (!yaep_done(&g)) {
        double x[1] = { 0.0 };
        int idx, out;
        bool init = yaep_n_trials(&g) < d.n_init;
        idx = yaep_next(&g, x);
        CHECK(idx >= -1, "refine %s: next returned %d", name, idx);
        CHECK(x[0] >= 0.0 && x[0] <= 1.0, "refine %s: %g outside the box",
              name, x[0]);
        if (!init) {
            /* The grid winner, from the scores next() just computed. */
            int jb = 0;
            double sb = yaep_acq_score(&g, 0), sx, sj;
            for (int j = 1; j < 9; j++) {
                double sc = yaep_acq_score(&g, j);
                if (sc > sb) { sb = sc; jb = j; }
            }
            if (acq != YAEP_ACQ_EAVC) {
                for (int j = 0; j < 9; j++) {
                    double sc = yaep_acq_score(&g, j);
                    double ps = yaep__point_score(&g, &s, g.cand + j);
                    double e = fabs(ps - sc) / (1.0 + fabs(sc));
                    if (e > worst_id) worst_id = e;
                }
            }
            /* The repeat guard's Halton point is also -1, and it clears
             * last_index; a refined point keeps the winner it came from. */
            if (idx == -1 && g.last_index >= 0) {
                int jw = g.last_index;
                sj = yaep__point_score(&g, &s, g.cand + jw);
                sx = yaep__point_score(&g, &s, x);
                refined++;
                CHECK(jw == jb || acq == YAEP_ACQ_EAVC ||
                      yaep_acq_score(&g, jw) >= sb - 1e-12 * (1.0 + fabs(sb)),
                      "refine %s: started from %d, not the winner %d", name, jw, jb);
                CHECK(fabs(x[0] - g.cand[jw]) <= h + 1e-12,
                      "refine %s: %g is more than a grid step from %g", name,
                      x[0], g.cand[jw]);
                CHECK(sx >= sj - 1e-12 * (1.0 + fabs(sj)),
                      "refine %s: refined score %.9g below the winner's %.9g",
                      name, sx, sj);
            }
        }
        if (lik == YAEP_LIK_CATEGORICAL) {
            double p[3] = { 0.0 };
            p[0] = 0.3; p[1] = 0.5 * erfc(-(6.0 * (x[0] - 0.5)) / sqrt(2.0)) * 0.7;
            p[2] = 1.0 - p[0] - p[1];
            out = yaep_simulate_outcome(p, 3, rng_u());
        } else {
            out = observe(x[0]);
        }
        rc = yaep_update(&g, x, out);
        CHECK(rc == YAEP_OK, "refine %s: update %s", name, yaep_strerror(rc));
    }
    CHECK(worst_id < 1e-9 * tolx,
          "refine %s: point score differs from the candidate score by %.2e",
          name, worst_id);
    CHECK(refined > 0, "refine %s: no proposal was refined in 34 trials", name);
    printf("  %-18s %2d of 34 proposals refined off the grid, point score vs "
           "candidate score %.1e\n", name, refined, worst_id);
    yaep_close(&g);
}

static void test_refine(void) {
    yaep_desc d;
    yaep_gp g;
    printf("desc.refine_steps:\n");
    memset(&d, 0, sizeof(d));
    d.n_dims = 1;
    d.lo[0] = 0.0; d.hi[0] = 1.0;
    d.target_p = 0.75;
    d.refine_steps = -1;
    CHECK(!yaep_open(&g, &d), "open accepted refine_steps = -1");
    d.refine_steps = YAEP__REFINE_MAX + 1;
    CHECK(!yaep_open(&g, &d), "open accepted refine_steps = %d",
          YAEP__REFINE_MAX + 1);
    refine_session(YAEP_ACQ_LSE, YAEP_LIK_BERNOULLI, "LSE");
    refine_session(YAEP_ACQ_BALV, YAEP_LIK_BERNOULLI, "BALV");
    refine_session(YAEP_ACQ_BALD, YAEP_LIK_BERNOULLI, "BALD");
    refine_session(YAEP_ACQ_LOCALMI, YAEP_LIK_BERNOULLI, "LOCALMI");
    refine_session(YAEP_ACQ_EAVC, YAEP_LIK_BERNOULLI, "EAVC (straddle)");
    refine_session(YAEP_ACQ_BALV, YAEP_LIK_CATEGORICAL, "BALV, CATEGORICAL");
}
/* --- YAEP_MODEL_PSYCHOMETRIC ------------------------------------------ */

/* The observer the model is built for: a threshold that curves across a context
 * c in [0, 1], and a probit rise of 7 dB from 10% to 90% along an intensity in
 * [0, 100] dB. */
#define PS_SIG  (7.0 / (2.0 * 1.2815515655446004))
static double ps_thr50(double c) { return 40.0 + 20.0 * sin(3.0 * c); }
static double ps_ptrue(const double* x) {
    return 0.5 * erfc(-((x[1] - ps_thr50(x[0])) / PS_SIG) / sqrt(2.0));
}

static void ps_desc(yaep_desc* d, yaep_acq acq, yaep_model model) {
    memset(d, 0, sizeof(*d));
    d->n_dims = 2;
    d->lo[0] = 0.0; d->hi[0] = 1.0;
    d->lo[1] = 0.0; d->hi[1] = 100.0;
    d->intensity_dim = 1;
    d->acq = acq;
    d->target_p = 0.75;
    d->grid[0] = 11; d->grid[1] = 21;
    d->n_init = 5;
    d->model = model;
}

/* E[q] and Var[q] by the header's own scheme but with n outer nodes, so the
 * node count can be checked for convergence against itself and against a
 * brute-force two-dimensional rule. */
static void ps_moments_n(const yaep_gp* g, const yaep__scr* s, double xint,
                         const yaep__mg* o, int n, double* eq, double* vq) {
    double gx[64], gw[64], e1 = 0.0, e2 = 0.0;
    yaep__gh_init(gx, gw, n);
    for (int k = 0; k < n; k++) {
        double gk = o->mg + YAEP__SQRT2 * sqrt(o->vg) * gx[k], muf, sdf, q1;
        double w = gw[k] / YAEP__SQRTPI;
        yaep__ps_node(o, xint, gk, &muf, &sdf);
        q1 = yaep__q_smooth(g, s, muf, sdf, 0.0);
        e1 += w * q1;
        e2 += w * (yaep__q_var(g, s, muf, sdf, 0.0) + q1 * q1);
    }
    *eq = e1;
    *vq = e2 - e1 * e1;
}

/* The same two moments with q evaluated at the exact latent and nothing else
 * shared with the header's scheme: 80 Gauss-Hermite nodes along g, and along m
 * a midpoint rule of 20000 cells over +-9 sd, fine enough to resolve the link
 * even where exp(g) makes it a step in m. */
static void ps_moments_brute(const yaep_gp* g, double xint, const yaep__mg* o,
                             double* eq, double* vq) {
    const int nv = 20000;
    double gx[80] = { 0.0 }, gw[80] = { 0.0 };
    double cm = o->cmg / o->vg, sm = sqrt(o->vm - o->cmg * o->cmg / o->vg);
    double h = 18.0 / nv, e1 = 0.0, e2 = 0.0;
    yaep__gh_init(gx, gw, 80);
    for (int i = 0; i < 80; i++) {
        double gv = o->mg + YAEP__SQRT2 * sqrt(o->vg) * gx[i], b = exp(gv);
        double a1 = 0.0, a2 = 0.0, ws = 0.0;
        for (int j = 0; j < nv; j++) {
            double v = -9.0 + (j + 0.5) * h, w = exp(-0.5 * v * v);
            double q = yaep__q_of_f(g, b * (xint - (o->mm + cm * (gv - o->mg) + sm * v)), 0.0);
            a1 += w * q; a2 += w * q * q; ws += w;
        }
        e1 += gw[i] / YAEP__SQRTPI * a1 / ws;
        e2 += gw[i] / YAEP__SQRTPI * a2 / ws;
    }
    *eq = e1;
    *vq = e2 - e1 * e1;
}

/* The dense posterior of (m, g) at the context of x from the 2N-latent
 * reference: A = K2^-1 and S = Sigma. Fills K2^-1 k* for the m and g columns
 * (2N each), the mean, and the 2 x 2 covariance. */
static void ps_dense_point(const yaep_gp* g, int n, const double* A, const double* S,
                           const double* hm, const double* hg, const double* x,
                           double* am, double* ag, double* mean, double* cov) {
    int n2 = 2 * n;
    double km[64], kg[64], c_mm = 0.0, c_gg = 0.0, c_mg = 0.0;
    for (int i = 0; i < n; i++) {
        km[i] = yaep__kctx(g, 0, g->X + (size_t)i * 2, x);
        kg[i] = yaep__kctx(g, 1, g->X + (size_t)i * 2, x);
    }
    for (int i = 0; i < n2; i++) {
        double sa = 0.0, sb = 0.0;
        for (int j = 0; j < n; j++) {
            sa += A[(size_t)i * n2 + j] * km[j];
            sb += A[(size_t)i * n2 + (n + j)] * kg[j];
        }
        am[i] = sa; ag[i] = sb;
    }
    mean[0] = g->hyper.mean; mean[1] = g->hyper.mean_g;
    for (int i = 0; i < n2; i++) {
        double zi = i < n ? hm[i] : hg[i - n];
        mean[0] += am[i] * zi;
        mean[1] += ag[i] * zi;
    }
    for (int i = 0; i < n2; i++) {
        double sa = 0.0, sb = 0.0;
        for (int j = 0; j < n2; j++) {
            sa += S[(size_t)i * n2 + j] * am[j];
            sb += S[(size_t)i * n2 + j] * ag[j];
        }
        c_mm += am[i] * sa; c_gg += ag[i] * sb; c_mg += am[i] * sb;
    }
    for (int j = 0; j < n; j++) { c_mm -= am[j] * km[j]; c_gg -= ag[n + j] * kg[j]; }
    cov[0] = c_mm + yaep__kctx(g, 0, x, x);
    cov[1] = c_mg;
    cov[2] = c_gg + yaep__kctx(g, 1, x, x);
}

static void test_psychometric_math(yaep_link link, const char* name) {
    enum { NT = 36 };
    static double K2[4 * NT * NT], A[4 * NT * NT], S[4 * NT * NT];
    yaep_desc d;
    yaep_gp g;
    yaep__scr s;
    double ps_stat = 0.0, worst_mean = 0.0, worst_cov = 0.0;
    double worst_q20 = 0.0, worst_qb = 0.0, worst_thr = 0.0;
    double worst_q20v = 0.0, worst_qbv = 0.0;
    int n, n2, ld, id = 1;
    double *hm, *hg, *um, *ug;
    rng_state = 0x243F6A8885A308D3ull + 0x5151ull + (uint64_t)link;
    ps_desc(&d, YAEP_ACQ_LSE, YAEP_MODEL_PSYCHOMETRIC);
    d.link = link;
    d.stop_trials = 200;
    /* Fixed lengthscales and a jitter of 1e-3: the dense reference inverts the
     * block prior outright, and at the default jitter the threshold GP's
     * matrix (variance 625) has a condition number the reference cannot
     * invert to better than 1e-5. The header never inverts it. */
    d.hyper.lengthscale[0] = 0.15;
    d.hyper.lengthscale_g[0] = 0.15;
    d.jitter = 1e-3;
    if (!yaep_open(&g, &d)) { CHECK(0, "open %s: %s", name, yaep_error(&g)); return; }
    /* Trials spread over the box, the responses from the observer. */
    for (int i = 0; i < NT; i++) {
        double x[2] = { 0.0 }, p[2] = { 0.0 };
        x[0] = rng_u();
        x[1] = ps_thr50(x[0]) + 12.0 * (rng_u() - 0.5);
        p[1] = ps_ptrue(x); p[0] = 1.0 - p[1];
        CHECK(yaep_update(&g, x, yaep_simulate_outcome(p, 2, rng_u())) == YAEP_OK,
              "%s: update %d", name, i);
    }
    n = g.N; n2 = 2 * n; ld = g.N_max;
    yaep__scr_of(&g, &s);
    hm = yaep__psv(&s, ld, YAEP__PS_HM); hg = yaep__psv(&s, ld, YAEP__PS_HG);
    um = yaep__psv(&s, ld, YAEP__PS_UM); ug = yaep__psv(&s, ld, YAEP__PS_UG);

    /* 1. The mode: z - mean = K grad log p(y | z), block by block. */
    {
        double gm[NT], gg[NT], hmax = 0.0;
        for (int i = 0; i < n; i++) {
            double xi = g.X[(size_t)i * 2 + id], b = exp(g.hyper.mean_g + hg[i]);
            double fv = b * (xi - g.hyper.mean - hm[i]), lp, d1, d2, d3;
            yaep__ll(&g, fv, g.y[i], &lp, &d1, &d2, &d3);
            gm[i] = -d1 * b;
            gg[i] = d1 * fv;
            if (fabs(hm[i]) > hmax) hmax = fabs(hm[i]);
            if (fabs(hg[i]) > hmax) hmax = fabs(hg[i]);
        }
        for (int i = 0; i < n; i++) {
            double rm = hm[i], rg = hg[i];
            for (int j = 0; j < n; j++) {
                rm -= g.Kmat[(size_t)i * ld + j] * gm[j];
                rg -= g.Kg[(size_t)i * ld + j] * gg[j];
            }
            track(&ps_stat, fabs(rm) / (1.0 + hmax));
            track(&ps_stat, fabs(rg) / (1.0 + hmax));
        }
    }

    /* 2. The dense 2N posterior: Sigma = (K2^-1 + W2)^-1 with K2 the
     * block-diagonal prior and W2 = U U' the Gauss-Newton Hessian. */
    memset(K2, 0, sizeof(double) * (size_t)n2 * n2);
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++) {
            K2[(size_t)i * n2 + j] = g.Kmat[(size_t)i * ld + j];
            K2[(size_t)(n + i) * n2 + (n + j)] = g.Kg[(size_t)i * ld + j];
        }
    memcpy(A, K2, sizeof(double) * (size_t)n2 * n2);
    tinv(A, n2);                                  /* A = K2^-1 */
    memcpy(S, A, sizeof(double) * (size_t)n2 * n2);
    for (int i = 0; i < n; i++) {
        S[(size_t)i * n2 + i] += um[i] * um[i];
        S[(size_t)i * n2 + (n + i)] += um[i] * ug[i];
        S[(size_t)(n + i) * n2 + i] += um[i] * ug[i];
        S[(size_t)(n + i) * n2 + (n + i)] += ug[i] * ug[i];
    }
    {
        /* 4. The log marginal likelihood, densely: Psi - 1/2 log|I + K2 W2|
         * with log|I + K2 W2| = log|K2| + log|K2^-1 + W2|. */
        static double T[4 * NT * NT];
        double quad = 0.0, ll = 0.0, lm;
        for (int i = 0; i < n2; i++) {
            double zi = i < n ? hm[i] : hg[i - n], acc = 0.0;
            for (int j = 0; j < n2; j++) acc += A[(size_t)i * n2 + j] * (j < n ? hm[j] : hg[j - n]);
            quad += zi * acc;
        }
        for (int i = 0; i < n; i++) {
            double xi = g.X[(size_t)i * 2 + id], b = exp(g.hyper.mean_g + hg[i]);
            double lp = 0.0, d1 = 0.0, d2 = 0.0, d3 = 0.0;
            yaep__ll(&g, b * (xi - g.hyper.mean - hm[i]), g.y[i], &lp, &d1, &d2, &d3);
            ll += lp;
        }
        memcpy(T, K2, sizeof(double) * (size_t)n2 * n2);
        lm = -0.5 * quad + ll - 0.5 * tlogdet(T, n2);
        memcpy(T, S, sizeof(double) * (size_t)n2 * n2);
        lm -= 0.5 * tlogdet(T, n2);
        CHECK(fabs(lm - yaep_log_marginal(&g)) < 1e-8 * tolx * (1.0 + fabs(lm)),
              "%s: log marginal %.12g, dense %.12g", name, yaep_log_marginal(&g), lm);
        track(&worst_lm, fabs(lm - yaep_log_marginal(&g)));
    }
    tinv(S, n2);                                  /* S = Sigma */
    for (int t0 = 0; t0 < 5; t0++) {
        double x[2] = { 0.0 }, km[NT] = { 0.0 }, kg[NT] = { 0.0 }, am[2 * NT] = { 0.0 }, ag[2 * NT] = { 0.0 };
        double c_mm = 0.0, c_gg = 0.0, c_mg = 0.0, mm, mg;
        yaep__mg o;
        memset(&o, 0, sizeof(o));
        x[0] = 0.1 + 0.2 * t0; x[1] = 50.0;
        yaep__ps_post(&g, &s, x, &o);
        for (int i = 0; i < n; i++) {
            km[i] = yaep__kctx(&g, 0, g.X + (size_t)i * 2, x);
            kg[i] = yaep__kctx(&g, 1, g.X + (size_t)i * 2, x);
        }
        /* am = K2^-1 k*_m (a 2N column), ag likewise for the g column. */
        for (int i = 0; i < n2; i++) {
            double sa = 0.0, sb = 0.0;
            for (int j = 0; j < n; j++) {
                sa += A[(size_t)i * n2 + j] * km[j];
                sb += A[(size_t)i * n2 + (n + j)] * kg[j];
            }
            am[i] = sa; ag[i] = sb;
        }
        mm = g.hyper.mean; mg = g.hyper.mean_g;
        for (int i = 0; i < n2; i++) {
            double zi = i < n ? hm[i] : hg[i - n];
            mm += am[i] * zi;
            mg += ag[i] * zi;
        }
        /* k*' K2^-1 k* and k*' K2^-1 S K2^-1 k*. */
        for (int i = 0; i < n2; i++) {
            double sa = 0.0, sb = 0.0;
            for (int j = 0; j < n2; j++) {
                sa += S[(size_t)i * n2 + j] * am[j];
                sb += S[(size_t)i * n2 + j] * ag[j];
            }
            c_mm += am[i] * sa; c_gg += ag[i] * sb; c_mg += am[i] * sb;
        }
        for (int j = 0; j < n; j++) {
            c_mm -= am[j] * km[j];
            c_gg -= ag[n + j] * kg[j];
        }
        c_mm += yaep__kctx(&g, 0, x, x);
        c_gg += yaep__kctx(&g, 1, x, x);
        track(&worst_mean, fabs(mm - o.mm) / (1.0 + fabs(mm)));
        track(&worst_mean, fabs(mg - o.mg) / (1.0 + fabs(mg)));
        track(&worst_cov, fabs(c_mm - o.vm) / (1.0 + c_mm));
        track(&worst_cov, fabs(c_gg - o.vg) / (1.0 + c_gg));
        track(&worst_cov, fabs(c_mg - o.cmg) / (1.0 + sqrt(c_mm * c_gg)));

        /* 5. The quadrature: 20 outer nodes against 60, and against a
         * brute-force rule in both coordinates, at three intensities around
         * the threshold at this context. */
        for (int k = -1; k <= 1; k++) {
            double xint = o.mm + k * 8.0, e20, v20, e60, v60, eb, vb, eh, vh;
            ps_moments_n(&g, &s, xint, &o, 20, &e20, &v20);
            ps_moments_n(&g, &s, xint, &o, 60, &e60, &v60);
            yaep__ps_moments(&g, &s, xint, &o, &eh, &vh);
            CHECK(eh == e20 && vh == v20, "%s: the header's rule is not the 20-node rule", name);
            track(&worst_q20, fabs(e20 - e60));
            track(&worst_q20v, fabs(v20 - v60));
            if (t0 == 2) {
                ps_moments_brute(&g, xint, &o, &eb, &vb);
                track(&worst_qb, fabs(e20 - eb));
                track(&worst_qbv, fabs(v20 - vb));
            }
        }

        /* 6. The threshold's closed form against quadrature over g of the
         * crossing m + f_t exp(-g), with m's conditional mean given g. */
        {
            double thr, lo, hi, ctx[1], ft = yaep__f_level(&g, 0.75);
            double gx[40], gw[40], e1 = 0.0, e2 = 0.0, sd, var;
            ctx[0] = x[0];
            if (yaep_threshold(&g, ctx, 0.0, &thr, &lo, &hi) == YAEP_OK) {
                yaep__gh_init(gx, gw, 40);
                for (int k = 0; k < 40; k++) {
                    double gk = o.mg + YAEP__SQRT2 * sqrt(o.vg) * gx[k];
                    double w = gw[k] / YAEP__SQRTPI;
                    double mc = o.mm + o.cmg / o.vg * (gk - o.mg);
                    double vc = o.vm - o.cmg * o.cmg / o.vg;
                    double xv = mc + ft * exp(-gk);
                    e1 += w * xv;
                    e2 += w * (vc + xv * xv);
                }
                var = e2 - e1 * e1;
                sd = var > 0.0 ? sqrt(var) : 0.0;
                track(&worst_thr, fabs(thr - e1));
                if (lo > d.lo[1] && hi < d.hi[1])
                    track(&worst_thr, fabs((hi - lo) - 2.0 * 1.96 * sd));
            }
        }
    }
    CHECK(ps_stat < (real_is_double ? 1e-6 : 1e-3), "%s: mode stationarity %.2e",
          name, ps_stat);
    /* 8. The look-ahead: the cross-covariance of (m, g) between two contexts,
     * from the V columns EAVC keeps, against the dense 2N posterior; and the
     * rank-one update one more trial at the first makes at the second,
     * against a dense Gaussian update of their joint 4 x 4 covariance. */
    {
        double xa[2] = { 0.3, 0.0 }, xb[2] = { 0.7, 50.0 };
        double ama[2 * NT] = { 0.0 }, aga[2 * NT] = { 0.0 }, amb[2 * NT] = { 0.0 }, agb[2 * NT] = { 0.0 };
        double mua[2], mub[2], ca[3], cb[3], C[4][4], cab[2][2], dcab[2][2];
        double v1a[NT], v2a[NT], J[2], d1, w, c, h[4], Ch[4], hCh = 0.0, vb[2];
        double worst_x = 0.0, worst_u = 0.0;
        yaep__mg oa, ob, ob2;
        memset(&oa, 0, sizeof(oa));
        memset(&ob, 0, sizeof(ob));
        memset(&ob2, 0, sizeof(ob2));
        ps_dense_point(&g, n, A, S, hm, hg, xa, ama, aga, mua, ca);
        ps_dense_point(&g, n, A, S, hm, hg, xb, amb, agb, mub, cb);
        yaep__ps_post(&g, &s, xa, &oa);
        memcpy(v1a, yaep__psv(&s, ld, YAEP__PS_V1), sizeof(double) * (size_t)n);
        memcpy(v2a, yaep__psv(&s, ld, YAEP__PS_V2), sizeof(double) * (size_t)n);
        yaep__ps_post(&g, &s, xb, &ob);
        {
            const double* v1b = yaep__psv(&s, ld, YAEP__PS_V1);
            const double* v2b = yaep__psv(&s, ld, YAEP__PS_V2);
            cab[0][0] = yaep__kctx(&g, 0, xa, xb) - yaep__dot(v1a, v1b, n);
            cab[0][1] = -yaep__dot(v1a, v2b, n);
            cab[1][0] = -yaep__dot(v2a, v1b, n);
            cab[1][1] = yaep__kctx(&g, 1, xa, xb) - yaep__dot(v2a, v2b, n);
        }
        /* dense cross: K_ab - k_a' A k_b + (A k_a)' S (A k_b) */
        for (int pq = 0; pq < 4; pq++) {
            const double* ka = (pq / 2) ? aga : ama;
            const double* kb = (pq % 2) ? agb : amb;
            double acc = (pq == 0) ? yaep__kctx(&g, 0, xa, xb)
                       : (pq == 3) ? yaep__kctx(&g, 1, xa, xb) : 0.0;
            /* k_a' A k_b = sum over a's kernel column of (A k_b) */
            for (int j = 0; j < n; j++) {
                double kaj = (pq / 2) ? yaep__kctx(&g, 1, g.X + (size_t)j * 2, xa)
                                      : yaep__kctx(&g, 0, g.X + (size_t)j * 2, xa);
                acc -= kaj * kb[(pq / 2) ? n + j : j];
            }
            for (int i = 0; i < 2 * n; i++) {
                double sb = 0.0;
                for (int j = 0; j < 2 * n; j++) sb += S[(size_t)i * 2 * n + j] * kb[j];
                acc += ka[i] * sb;
            }
            dcab[pq / 2][pq % 2] = acc;
        }
        for (int p0 = 0; p0 < 2; p0++)
            for (int q0 = 0; q0 < 2; q0++)
                track(&worst_x, fabs(cab[p0][q0] - dcab[p0][q0]) /
                      (1.0 + sqrt((p0 ? ca[2] : ca[0]) * (q0 ? cb[2] : cb[0]))));
        /* one more trial at a, outcome 1, 3 intensity units above its
         * threshold mean */
        yaep__ps_site(&g, &oa, oa.mm + 3.0, 1.0, J, &d1, &w, &c);
        vb[0] = cab[0][0] * J[0] + cab[1][0] * J[1];
        vb[1] = cab[0][1] * J[0] + cab[1][1] * J[1];
        yaep__ps_lookahead(&ob, vb, d1, w, c, &ob2);
        C[0][0] = ca[0]; C[0][1] = ca[1]; C[1][0] = ca[1]; C[1][1] = ca[2];
        C[2][2] = cb[0]; C[2][3] = cb[1]; C[3][2] = cb[1]; C[3][3] = cb[2];
        for (int p0 = 0; p0 < 2; p0++)
            for (int q0 = 0; q0 < 2; q0++) {
                C[p0][2 + q0] = dcab[p0][q0];
                C[2 + q0][p0] = dcab[p0][q0];
            }
        h[0] = J[0]; h[1] = J[1]; h[2] = 0.0; h[3] = 0.0;
        for (int i = 0; i < 4; i++) {
            Ch[i] = 0.0;
            for (int j = 0; j < 4; j++) Ch[i] += C[i][j] * h[j];
        }
        for (int i = 0; i < 4; i++) hCh += h[i] * Ch[i];
        {
            double den = 1.0 + w * hCh;
            double m2 = mub[0] + Ch[2] * d1 / den, g2 = mub[1] + Ch[3] * d1 / den;
            double vm2 = C[2][2] - Ch[2] * Ch[2] * w / den;
            double vg2 = C[3][3] - Ch[3] * Ch[3] * w / den;
            double cm2 = C[2][3] - Ch[2] * Ch[3] * w / den;
            track(&worst_u, fabs(m2 - ob2.mm) / (1.0 + fabs(m2)));
            track(&worst_u, fabs(g2 - ob2.mg) / (1.0 + fabs(g2)));
            track(&worst_u, fabs(vm2 - ob2.vm) / (1.0 + vm2));
            track(&worst_u, fabs(vg2 - ob2.vg) / (1.0 + vg2));
            track(&worst_u, fabs(cm2 - ob2.cmg) / (1.0 + sqrt(vm2 * vg2)));
            CHECK(fabs(vm2 - cb[0]) > 1e-6 * cb[0], "%s: the look-ahead did not move the "
                  "second context at all", name);
        }
        CHECK(worst_x < 1e-7 * tolx, "%s: cross-covariance vs dense %.2e", name, worst_x);
        CHECK(worst_u < 1e-7 * tolx, "%s: look-ahead vs dense update %.2e", name, worst_u);
        track(&worst_look, worst_u);
        printf("         look-ahead: cross-covariance of two contexts vs dense 2N %.1e, "
               "rank-one update vs a dense 4 x 4 update %.1e\n", worst_x, worst_u);
    }

    CHECK(worst_mean < 1e-9 * tolx, "%s: predictive mean vs dense %.2e", name, worst_mean);
    /* The reference inverts the block prior, whose condition number here is
     * about 1e6, so 1e-7 is its resolution and not the header's. */
    CHECK(worst_cov < 1e-7 * tolx, "%s: predictive covariance vs dense %.2e", name, worst_cov);
    /* E[q] under the probit is closed form in m, so its only error is the 20
     * nodes over g. Var[q], and under the logit E[q] too, go through the
     * one-latent model's 20-node rule in f, which is good to about 1e-4 of
     * probability when the latent sd is several units; that is the GP model's
     * accuracy as well. The tolerances are the measured errors with room of
     * about five. */
    CHECK(worst_q20 < (link == YAEP_LINK_PROBIT ? 1e-7 : 2e-3),
          "%s: E[q], 20 vs 60 outer nodes %.2e", name, worst_q20);
    CHECK(worst_qb < (link == YAEP_LINK_PROBIT ? 1e-7 : 5e-4),
          "%s: E[q] vs brute force %.2e", name, worst_qb);
    CHECK(worst_q20v < 2e-3, "%s: Var[q], 20 vs 60 outer nodes %.2e", name, worst_q20v);
    CHECK(worst_qbv < 5e-4, "%s: Var[q] vs brute force %.2e", name, worst_qbv);
    CHECK(worst_thr < 1e-8, "%s: threshold closed form vs quadrature %.2e", name, worst_thr);
    printf("  %-6s mode stationarity %.1e, predictive mean %.1e and covariance %.1e "
           "vs dense 2N,\n"
           "         E[q]: 20 vs 60 g-nodes %.1e, vs brute force %.1e; Var[q]: %.1e, "
           "%.1e;\n         threshold closed form vs quadrature %.1e\n",
           name, ps_stat, worst_mean, worst_cov, worst_q20, worst_qb, worst_q20v,
           worst_qbv, worst_thr);

    yaep_close(&g);
}

/* The analytic hyperparameter gradient of the psychometric model against
 * central differences of its objective, over every free hyperparameter: the
 * two lengthscales, the two output scales, the two means, and the cutpoint
 * gaps under ORDINAL; with a floor and a ceiling too, where the Gauss-Newton
 * W is clamped and the true Hessian is what the implicit term needs. The
 * handle's Newton tolerance is 1e-13 here rather than the 1e-8 a session
 * uses, because at 1e-8 the objective is only good to about 1e-8 and a
 * difference step small enough to resolve 1e-4 divides that into 1e-3. */
static void ps_grad_case(yaep_link link, bool ord, double guess, double lapse,
                         const char* name) {
    yaep_desc d;
    yaep_gp g;
    yaep__param pp[YAEP__NTHETA];
    double th[YAEP__NTHETA], gr[YAEP__NTHETA], tw[YAEP__NTHETA], v, worst = 0.0;
    int np;
    rng_state = 0x243F6A8885A308D3ull + 0x6161ull + (uint64_t)link * 7u + (ord ? 3u : 0u);
    ps_desc(&d, YAEP_ACQ_LSE, YAEP_MODEL_PSYCHOMETRIC);
    d.link = link; d.guess = guess; d.lapse = lapse; d.stop_trials = 100;
    if (ord) { d.lik = YAEP_LIK_ORDINAL; d.n_outcomes = 4; d.target_outcome = 1; }
    if (!yaep_open(&g, &d)) { CHECK(0, "open %s: %s", name, yaep_error(&g)); return; }
    g.ps_tol = real_is_double ? 1e-13 : 1e-6;
    for (int i = 0; i < 40; i++) {
        double x[2], f, pr[4];
        x[0] = rng_u();
        x[1] = ps_thr50(x[0]) + 14.0 * (rng_u() - 0.5);
        f = (x[1] - ps_thr50(x[0])) / PS_SIG;
        if (ord) {
            double c1 = 0.5 * erfc(f / sqrt(2.0)), c2 = 0.5 * erfc((f - 1.0) / sqrt(2.0));
            double c3 = 0.5 * erfc((f - 2.0) / sqrt(2.0));
            pr[0] = c1; pr[1] = c2 - c1; pr[2] = c3 - c2; pr[3] = 1.0 - c3;
            yaep_update(&g, x, yaep_simulate_outcome(pr, 4, rng_u()));
        } else {
            pr[1] = guess + (1.0 - guess - lapse) * 0.5 * erfc(-f / sqrt(2.0));
            pr[0] = 1.0 - pr[1];
            yaep_update(&g, x, yaep_simulate_outcome(pr, 2, rng_u()));
        }
    }
    np = yaep__params(&g, pp);
    yaep__theta_get(&g, pp, np, th);
    /* Off the defaults, where the prior's own gradient is not zero. */
    for (int i = 0; i < np; i++)
        th[i] = yaep__clamp(th[i] + 0.1 * (double)(i % 3 - 1), pp[i].lo, pp[i].hi);
    CHECK(yaep__fit_eval(&g, pp, np, th, &v, gr) == YAEP_OK, "%s: gradient", name);
    for (int i = 0; i < np; i++) {
        double h = (real_is_double ? 1e-4 : 3e-3) * (1.0 + fabs(th[i])), vp = 0.0, vm = 0.0, fd;
        memcpy(tw, th, sizeof(double) * (size_t)np);
        tw[i] = th[i] + h;
        yaep__fit_eval(&g, pp, np, tw, &vp, NULL);
        tw[i] = th[i] - h;
        yaep__fit_eval(&g, pp, np, tw, &vm, NULL);
        fd = (vp - vm) / (2.0 * h);
        track(&worst, fabs(gr[i] - fd) / (1e-3 + fabs(fd)));
    }
    track(&worst_grad, worst);
    CHECK(np == (ord ? 8 : 6), "%s: %d free hyperparameters", name, np);
    /* In float the objective is good to about 1e-6 of itself, so differences
     * resolve the gradient to a few percent at best (7e-2 measured); the check
     * that means something is the double one. */
    CHECK(worst < (real_is_double ? 1e-4 : 0.15),
          "%s: analytic gradient vs differences %.2e", name, worst);
    printf("  %-24s analytic gradient vs central differences, %d hyperparameters: "
           "%.1e\n", name, np, worst);
    yaep_close(&g);
}

/* A whole session each, the psychometric model against the RBF GP, same
 * streams: the threshold across 30 contexts and the field in and out of the
 * transition band. */
static void ps_session(yaep_acq acq, yaep_model model, int rep, int trials,
                       double* thr_err, double* p_err, double* band_err) {
    yaep_desc d;
    yaep_gp g;
    double e = 0.0, eb = 0.0, et = 0.0;
    int nb = 0, nt = 0;
    rng_state = 0x243F6A8885A308D3ull + 0x7171ull + (uint64_t)rep * 0x1000193ull;
    ps_desc(&d, acq, model);
    d.fit = true;
    d.fit_every = 20;
    d.stop_trials = trials;
    if (!yaep_open(&g, &d)) { CHECK(0, "open session: %s", yaep_error(&g)); return; }
    while (!yaep_done(&g)) {
        double x[2] = { 0.0 }, p[2] = { 0.0 };
        int rc;
        yaep_next(&g, x);
        p[1] = ps_ptrue(x); p[0] = 1.0 - p[1];
        rc = yaep_update(&g, x, yaep_simulate_outcome(p, 2, rng_u()));
        CHECK(rc == YAEP_OK, "session update: %s", yaep_strerror(rc));
        if (rc != YAEP_OK) break;
    }
    for (int i = 0; i < 30; i++) {
        for (int j = 0; j < 30; j++) {
            double x[2], pt, pp;
            x[0] = i / 29.0; x[1] = 100.0 * j / 29.0;
            pt = ps_ptrue(x);
            pp = yaep_predict_p(&g, x);
            e += fabs(pp - pt);
            if (pt >= 0.05 && pt <= 0.95) { eb += fabs(pp - pt); nb++; }
        }
    }
    for (int i = 0; i < 30; i++) {
        double c[1], th, lo, hi;
        c[0] = i / 29.0;
        if (yaep_threshold(&g, c, 0.0, &th, &lo, &hi) == YAEP_OK) {
            et += fabs(th - (ps_thr50(c[0]) + 0.67448975019608171 * PS_SIG));
            nt++;
        }
    }
    *thr_err = nt ? et / nt : 1e9;
    *p_err = e / 900.0;
    *band_err = nb ? eb / nb : 1.0;
    CHECK(nt == 30, "session: the threshold crossed in %d of 30 contexts", nt);
    yaep_close(&g);
}

static void test_psychometric(void) {
    yaep_desc d;
    yaep_gp g;
    printf("YAEP_MODEL_PSYCHOMETRIC:\n");
    /* What the model refuses. */
    ps_desc(&d, YAEP_ACQ_LSE, YAEP_MODEL_PSYCHOMETRIC);
    d.stop_trials = 10;
    d.lik = YAEP_LIK_GAUSSIAN;
    CHECK(!yaep_open(&g, &d), "open accepted the psychometric model under GAUSSIAN");
    d.lik = YAEP_LIK_CATEGORICAL; d.n_outcomes = 3;
    CHECK(!yaep_open(&g, &d), "open accepted it under CATEGORICAL");
    d.lik = YAEP_LIK_BERNOULLI; d.n_outcomes = 0;
    d.kernel = YAEP_KERNEL_SEMIP;
    CHECK(!yaep_open(&g, &d), "open accepted it with the SEMIP kernel");
    d.kernel = YAEP_KERNEL_RBF; d.model = (yaep_model)7;
    CHECK(!yaep_open(&g, &d), "open accepted model = 7");

    test_psychometric_math(YAEP_LINK_PROBIT, "probit");
    test_psychometric_math(YAEP_LINK_LOGIT, "logit");
    ps_grad_case(YAEP_LINK_PROBIT, false, 0.0, 0.0, "probit");
    ps_grad_case(YAEP_LINK_LOGIT, false, 0.0, 0.0, "logit");
    ps_grad_case(YAEP_LINK_PROBIT, false, 0.5, 0.02, "probit, guess 0.5 lapse .02");
    ps_grad_case(YAEP_LINK_PROBIT, true, 0.0, 0.0, "ordinal, 4 outcomes");

    /* Ordinal outcomes and a floor, run through whole sessions. */
    {
        int bad = 0;
        rng_state = 0x243F6A8885A308D3ull + 0x9191ull;
        ps_desc(&d, YAEP_ACQ_BALD, YAEP_MODEL_PSYCHOMETRIC);
        d.lik = YAEP_LIK_ORDINAL; d.n_outcomes = 3; d.target_outcome = 1;
        d.fit = true; d.fit_every = 15; d.stop_trials = 45;
        CHECK(yaep_open(&g, &d), "open ordinal: %s", yaep_error(&g));
        while (!yaep_done(&g)) {
            double x[2], pr[3], po[3], f;
            yaep_next(&g, x);
            f = (x[1] - ps_thr50(x[0])) / PS_SIG;
            pr[0] = 1.0 - 0.5 * erfc(-f / sqrt(2.0));
            pr[2] = 0.5 * erfc(-(f - 1.0) / sqrt(2.0));
            pr[1] = 1.0 - pr[0] - pr[2];
            if (yaep_update(&g, x, yaep_simulate_outcome(pr, 3, rng_u())) != YAEP_OK) bad++;
            if (yaep_predict_outcomes(&g, x, po) != YAEP_OK ||
                fabs(po[0] + po[1] + po[2] - 1.0) > 1e-9) bad++;
        }
        CHECK(bad == 0, "ordinal psychometric session: %d failures", bad);
        yaep_close(&g);
        bad = 0;
        ps_desc(&d, YAEP_ACQ_LSE, YAEP_MODEL_PSYCHOMETRIC);
        d.guess = 0.5; d.lapse = 0.02; d.target_p = 0.75;
        d.fit = true; d.fit_every = 15; d.stop_trials = 45;
        CHECK(yaep_open(&g, &d), "open 2AFC: %s", yaep_error(&g));
        while (!yaep_done(&g)) {
            double x[2] = { 0.0 }, p[2] = { 0.0 };
            yaep_next(&g, x);
            p[1] = 0.5 + 0.48 * ps_ptrue(x); p[0] = 1.0 - p[1];
            if (yaep_update(&g, x, yaep_simulate_outcome(p, 2, rng_u())) != YAEP_OK) bad++;
        }
        {
            double ctx[1] = { 0.5 }, th, lo, hi;
            CHECK(yaep_threshold(&g, ctx, 0.0, &th, &lo, &hi) == YAEP_OK,
                  "2AFC psychometric threshold did not resolve");
        }
        CHECK(bad == 0, "2AFC psychometric session: %d failures", bad);
        yaep_close(&g);
        printf("  ordinal (3 outcomes, BALD) and 2AFC (guess 0.5, lapse 0.02) "
               "sessions ran clean\n");
    }

    /* The look-ahead acquisitions on the observer, and LocalMI's candidate
     * score against its own point score (its refinement objective). */
    {
        double te, pe, be, worst_id = 0.0;
        ps_session(YAEP_ACQ_EAVC, YAEP_MODEL_PSYCHOMETRIC, 0, 80, &te, &pe, &be);
        printf("  EAVC, 80 trials: threshold error %.2f dB, field %.4f, band %.4f\n",
               te, pe, be);
        CHECK(te < 2.0, "psychometric EAVC threshold error %.2f dB", te);
        ps_session(YAEP_ACQ_LOCALMI, YAEP_MODEL_PSYCHOMETRIC, 0, 80, &te, &pe, &be);
        printf("  LOCALMI, 80 trials: threshold error %.2f dB, field %.4f, band %.4f\n",
               te, pe, be);
        CHECK(te < 2.0, "psychometric LOCALMI threshold error %.2f dB", te);
        rng_state = 0x243F6A8885A308D3ull + 0x8181ull;
        ps_desc(&d, YAEP_ACQ_LOCALMI, YAEP_MODEL_PSYCHOMETRIC);
        d.stop_trials = 30;
        CHECK(yaep_open(&g, &d), "open LOCALMI: %s", yaep_error(&g));
        while (!yaep_done(&g)) {
            double x[2] = { 0.0 }, p[2] = { 0.0 };
            yaep_next(&g, x);
            if (yaep_n_trials(&g) >= d.n_init) {
                yaep__scr s;
                yaep__scr_of(&g, &s);
                for (int j = 0; j < g.M; j += 17) {
                    double sc = yaep_acq_score(&g, j);
                    double pt = yaep__point_score(&g, &s, yaep__cand(&g, j));
                    track(&worst_id, fabs(sc - pt) / (1.0 + fabs(sc)));
                }
            }
            p[1] = ps_ptrue(x); p[0] = 1.0 - p[1];
            yaep_update(&g, x, yaep_simulate_outcome(p, 2, rng_u()));
        }
        CHECK(worst_id == 0.0, "LOCALMI candidate vs point score %.2e", worst_id);
        yaep_close(&g);
    }

    /* The observer it is for, against the RBF GP on the same streams. */
    {
        double t_ps = 0.0, p_ps = 0.0, b_ps = 0.0, t_gp = 0.0, p_gp = 0.0, b_gp = 0.0;
        const int reps = 3, trials = 120;
        for (int r = 0; r < reps; r++) {
            double te = 0.0, pe = 0.0, be = 0.0;
            ps_session(YAEP_ACQ_LSE, YAEP_MODEL_PSYCHOMETRIC, r, trials, &te, &pe, &be);
            t_ps += te / reps; p_ps += pe / reps; b_ps += be / reps;
            ps_session(YAEP_ACQ_LSE, YAEP_MODEL_GP, r, trials, &te, &pe, &be);
            t_gp += te / reps; p_gp += pe / reps; b_gp += be / reps;
        }
        printf("  curved threshold, 7 dB rise, %d trials, %d streams, LSE: threshold "
               "error %.2f dB (GP %.2f),\n"
               "         field MAE(p) %.4f (GP %.4f), in the transition band %.4f "
               "(GP %.4f)\n", trials, reps, t_ps, t_gp, p_ps, p_gp, b_ps, b_gp);
        CHECK(t_ps < 2.0, "psychometric threshold error %.2f dB", t_ps);
        CHECK(b_ps < b_gp, "psychometric band error %.4f not below the GP's %.4f",
              b_ps, b_gp);
        CHECK(p_ps < 0.04, "psychometric field error %.4f", p_ps);
    }
}

/* --- regressions from the cross-method comparison ---------------------- */

/* Three runs of tests/compare/methods_compare.py that broke 0.4.1, as their
 * recorded trials (the stimuli the binding proposed and the responses its
 * seeded stream gave), replayed through yaep_update() on the comparison's
 * audiogram desc. The fit scheduled at the last trial of each is the one that
 * failed. */
/* audio-older-normal-b2, rep 14, psychometric balv: trials 1..85, replayed through
 * yaep_update(); the fit scheduled at trial 85 is the one that failed. */
static const double rep_on2_14_x[85][2] = {
    { -0.32293919380754232, 117.6747652888298 },
    { 3.0775600243359804, 17.852410078048706 },
    { 1.841811353340745, 78.344237450510263 },
    { -2.59765690471977, -12.692882474511862 },
    { -1.6292514139786363, 54.477283507585526 },
    { 0.34613528200824384, 23.012982547224709 },
    { -3, 34.53170280245142 },
    { 4, 16.836654487448477 },
    { 0.94535022349087294, 2.3544685349676135 },
    { 4, 4.6029589925279693 },
    { -3, 21.311896062463198 },
    { -1.0012982547224711, 12.567330897489695 },
    { -3, 8.0920893224749726 },
    { -0.84960747074131449, 1.7149752396937981 },
    { 1.7715881447401371, 12.496980915120714 },
    { 4, 11.283212344891062 },
    { -3, 2.4951684997055752 },
    { 1.9915761262698741, 2.5386471799175614 },
    { -0.81277174175132483, 9.2841185525986329 },
    { 4, 6.6020527848204011 },
    { 1.3950483150029445, 8.9157612626987355 },
    { -0.98019326001177687, 2.7931758072364898 },
    { -3, 8.2059179850559403 },
    { 4, 11.028683717572134 },
    { 2.0099939907648685, 3.7575477121982148 },
    { 4, 8.7584539199057829 },
    { 2.0332729194861616, 7.1711960977252351 },
    { 1.3078200567542695, 7.1711960977252351 },
    { 0.76820657677782334, 7.3822460448321783 },
    { -3, 5.4100235546967417 },
    { -3, 6.3743954596584675 },
    { -3, 7.0138887549322826 },
    { 1.2894021922592747, 7.355374742675183 },
    { 0.81642517202590947, 7.4257247250441631 },
    { 0.51355680026869588, 7.6099033699941128 },
    { 1.0461352820082441, 7.6099033699941128 },
    { 3.2956521319788012, 9.2406398723866463 },
    { -3, 6.4613528200824382 },
    { -3, 6.8731887901943205 },
    { 1.1268417382251212, 7.5395533876251299 },
    { 2.3150362965326816, 8.3900966300058872 },
    { 1.1241546080094218, 7.4525960272011593 },
    { 2.4737620787507364, 8.644625257324817 },
    { 1.146920340525615, 7.3822460448321783 },
    { 0.8120773040047109, 7.2850247603062019 },
    { -3, 6.391002837713458 },
    { -3, 6.7593601276133528 },
    { 0.49347819796820219, 7.4525960272011593 },
    { -3, 6.3743954596584675 },
    { 1.0190216814708521, 7.6099033699941128 },
    { 3.26585140122571, 9.9504831500294433 },
    { 1.0786231429770352, 7.5395533876251299 },
    { 2.595652131978802, 9.354468534967614 },
    { 0.87871376374779198, 7.1711960977252351 },
    { 2.5377114082781183, 9.4682971975485799 },
    { 4, 12.014794962639851 },
    { 0.5043478680211988, 6.6890101452443727 },
    { 2.595652131978802, 9.5117758777605665 },
    { 0.5043478680211988, 6.531702802451421 },
    { 0.5043478680211988, 6.7593601276133528 },
    { -3, 5.7515095424396421 },
    { 0.5043478680211988, 6.391002837713458 },
    { 4, 12.041666264796847 },
    { 3.2956521319788012, 10.476147782722292 },
    { 0.5043478680211988, 6.1902168147085188 },
    { 4, 12.724638240282648 },
    { 4, 12.155494927377813 },
    { -3, 5.1286236252208184 },
    { -3, 5.5404595953326989 },
    { -3, 5.9088168852325946 },
    { 0.5, 26.666666666666664 },
    { -3, 6.2771741751324912 },
    { -3, 6.6724027671893822 },
    { -3, 7.0573674351442683 },
    { -1.25, 73.333333333333329 },
    { -3, 7.4525960272011593 },
    { -3, 7.8375606951560464 },
    { -3, 7.6099033699941128 },
    { 2.25, -4.4444444444444446 },
    { -3, 7.3822460448321783 },
    { -3, 7.0842387373012636 },
    { -3, 7.5395533876251299 },
    { -2.125, 42.222222222222221 },
    { -3, 7.9079106775250265 },
    { -3, 7.7237320325750787 },
};
static const unsigned char rep_on2_14_y[85] = {
    1, 1, 1, 0, 1, 1, 1, 1, 0, 0, 1, 1, 1, 0, 1, 1, 0, 0, 1, 0, 1, 0, 1, 1, 0, 0, 0, 0, 1, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 0, 0, 1, 1, 1, 1, 0, 1, 0, 1, 0, 1, 1, 0, 0, 1, 1, 1, 1, 0, 1, 1, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 1, 1
};

/* audio-older-normal-b0.5, rep 6, psychometric bald: trials 1..105, replayed through
 * yaep_update(); the fit scheduled at trial 105 is the one that failed. */
static const double rep_on05_6_x[105][2] = {
    { 2.7927553374320269, 119.36388431116939 },
    { -1.4634717609733343, 29.583794809877872 },
    { -0.69559168443083763, 83.582067247480154 },
    { 2.2412964515388012, -6.2834766879677773 },
    { 1.1539589762687683, 50.303653087466955 },
    { 4, 43.75845391990579 },
    { -3, 28.355374742675185 },
    { -0.082971010998233674, 17.459540404667298 },
    { -3, 11.489130329947002 },
    { 4, 28.794082014944063 },
    { -1.019716119217466, -1.7306764100412197 },
    { 1.9617753955167827, 16.46829719754858 },
    { 4, 13.488224122239433 },
    { -3, 3.73067641004122 },
    { 4, 3.9583337352031536 },
    { 0.8212862362522082, 7.3822460448321783 },
    { -0.17506033347320754, 1.9861112450677179 },
    { 2.1248490457560361, 2.4248185173365941 },
    { -1.5198067399882231, 9.5821258601295476 },
    { -3, 8.2762679674249213 },
    { 4, 11.489130329947002 },
    { -3, 1.8019326001177698 },
    { 2.3063405604902849, 7.2684173822512115 },
    { 4, 6.9435387725633015 },
    { -1.2256341042734067, 2.9773544521864377 },
    { -3, 6.5048315002944248 },
    { 4, 9.8366544874484756 },
    { -3, 7.5395533876251299 },
    { 0.37593601276133531, 7.1277174175132485 },
    { -3, 8.3466179497939024 },
    { 4, 10.844505072622187 },
    { 0.5043478680211988, 6.3475241575014714 },
    { -3, 7.4525960272011593 },
    { 4, 11.787137637477917 },
    { 0.5666364597430813, 5.426630932751733 },
    { -3, 6.5751814826634059 },
    { 4, 10.801026392410201 },
    { -3, 7.355374742675183 },
    { 1.204347868021199, 5.3396735723277606 },
    { 4, 12.014794962639851 },
    { 0.55960146150618306, 5.2424522878017852 },
    { -3, 6.2771741751324912 },
    { -3, 7.3118960624631981 },
    { 4, 10.985205037360149 },
    { -3, 8.644625257324817 },
    { 2.1752415750147205, 6.786231429770349 },
    { 2.0999093792292425, 7.0138887549322826 },
    { 2.0397947215179602, 7.355374742675183 },
    { 1.9232789287212939, 7.5830320678371166 },
    { 1.7304045477289489, 7.7237320325750787 },
    { -3, 7.1711960977252351 },
    { -3, 8.0920893224749744 },
    { 1.838224604483218, 7.6802533523630929 },
    { 1.5418780347578025, 7.6802533523630929 },
    { -2.3021739340105989, 6.2336954949205055 },
    { -3, 8.2327892872129365 },
    { 1.6523852217277712, 7.8375606951560464 },
    { -2.3021739340105989, 5.8653382050206098 },
    { -3, 8.2059179850559421 },
    { -2.3021739340105989, 6.3475241575014714 },
    { 0.5, 26.666666666666664 },
    { -3, 8.1624393048439554 },
    { 1.204347868021199, 5.6376808798586753 },
    { 1.6751509542439647, 8 },
    { 3.2956521319788012, 10.660326427672238 },
    { -3, 7.2684173822512115 },
    { -3, 7.4257247250441631 },
    { -3, 7.7672107127870644 },
    { -1.25, 73.333333333333329 },
    { -2.4814915147342482, 6.9000600923513158 },
    { -3, 7.8375606951560464 },
    { 1.7163345512551527, 7.6802533523630929 },
    { -2.3403532281084387, 6.7158814474013688 },
    { -2.3021739340105989, 6.8297101099823356 },
    { -2.3035174991184491, 6.7158814474013688 },
    { 2.25, -4.4444444444444446 },
    { -2.3021739340105989, 6.9000600923513158 },
    { -2.3021739340105989, 7.0842387373012645 },
    { -2.3021739340105989, 7.0138887549322835 },
    { -2.125, 42.222222222222221 },
    { -3, 7.2684173822512115 },
    { -3, 7.9079106775250265 },
    { 1.204347868021199, 6.0495168499705576 },
    { 1.7347524157501475, 7.8644319973130408 },
    { 3.2956521319788012, 10.573369067248267 },
    { -3, 7.3553747426751821 },
    { -3, 7.8375606951560464 },
    { -3, 7.3822460448321783 },
    { 1.375, 88.888888888888872 },
    { -3, 7.8644319973130408 },
    { -3, 7.3822460448321783 },
    { 1.631280227017077, 7.7940820149440597 },
    { 3.2956521319788012, 10.871376374779182 },
    { -3, 7.2684173822512115 },
    { 2.9212862362522078, 10.13466179497939 },
    { 4, 12.935688187389589 },
    { 4, 12.724638240282648 },
    { 1.3907004469817459, 7.6533820502060976 },
    { 1.0347524157501473, 7.5664246897821261 },
    { 1.1425724725044164, 7.6099033699941128 },
    { 2.9142512380153098, 8.2059179850559403 },
    { -2.3021739340105989, 6.4178741398704542 },
    { -2.3021739340105989, 6.8731887901943205 },
    { -2.3021739340105989, 7.0138887549322835 },
    { 0.98218595248086249, 7.5664246897821261 },
};
static const unsigned char rep_on05_6_y[105] = {
    1, 1, 1, 0, 1, 1, 1, 1, 1, 1, 0, 1, 1, 0, 0, 1, 0, 0, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 1, 1, 1, 1, 0, 0, 1, 0, 1, 1, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 1, 0, 1, 0, 1, 1, 0, 1, 0, 1, 1, 0, 1, 0, 0, 0, 1, 1, 0, 1, 1, 0, 1, 0, 0, 0, 1, 1, 1, 0, 1, 0, 1, 0, 0, 1, 0, 1, 1, 1, 1, 0, 1, 0, 1, 1, 0, 1, 1, 0, 0, 0, 0, 1
};

/* audio-metabolic-b0.5, rep 4, gp lse: trials 1..145, replayed through
 * yaep_update(); the fit scheduled at trial 145 is the one that failed. */
static const double rep_met05_4_x[145][2] = {
    { 3.5513251898810267, 109.40802693367004 },
    { -2.1001692283898592, 11.039110831916332 },
    { -0.089919170364737511, 77.894264962524176 },
    { 1.5136529421433806, 36.537098903208971 },
    { 0.50160135794430971, 61.22373441234231 },
    { 1.0504831500294427, 110.79498822265163 },
    { -1.613556800268696, 98.934781979682029 },
    { -2.9333635402569187, 83.602052784820401 },
    { -2.5673762078750735, 120 },
    { 3.5511322773906779, 85 },
    { -2.9794082014944059, 64 },
    { 3.7106884285114821, 71 },
    { 4, 120 },
    { -3, 53.65156963479096 },
    { 4, 61.019926924690857 },
    { -1.1844505072622185, 50 },
    { 4, -20 },
    { -3, 41.575181482663403 },
    { -0.91355680026869601, 42.978260659894012 },
    { -3, 120 },
    { 2.3361412912433761, 71 },
    { 1.2162439304843955, 57.021739340105988 },
    { -3, 33.637680879858678 },
    { -3, 28.680253352363092 },
    { 0.3829710109982335, 120 },
    { -3, -20 },
    { -2.9964825008815508, 23.766304505079493 },
    { 4, 68.269323589958788 },
    { -2.558167275627576, 22 },
    { -0.20652180203179812, 36 },
    { -2.9978260659894009, 15.714975239693798 },
    { 3.7510416566199214, 78 },
    { -0.13553747426751847, 36 },
    { 4, 76.53170280245142 },
    { 3.8610356473847895, 85 },
    { 1.9390096630005891, 57.021739340105988 },
    { 0.39000600923513151, 36 },
    { -3, 22.04861064226299 },
    { -0.14257247250441657, 29 },
    { 0.21554949273778132, 29 },
    { 1.2249396665267926, 36 },
    { 4, 82.708030862227659 },
    { -1.5907910677525028, 22 },
    { -3, 22.644625257324815 },
    { 2.6276267967424918, 63.978260659894012 },
    { -3, 120 },
    { 4, 120 },
    { -3, 21.680253352363096 },
    { 3.8921799432457309, 78 },
    { 1.1198067399882232, 43 },
    { 1.8539553387625134, 50.021739340105995 },
    { 4, 76.645531465032391 },
    { 2.5723732032575075, 57 },
    { -0.33109898547556293, 29 },
    { -0.11277174175132504, 29 },
    { -3, 120 },
    { 1.0277174175132493, 36 },
    { 1.1794082014944061, 36 },
    { 2.6162439304843952, 57 },
    { -3, 19.935688187389591 },
    { 3.2794082014944057, 64 },
    { 3.9347071053647689, 71 },
    { 4, 120 },
    { 4, 76.504831500294429 },
    { -0.26446252573248191, 29 },
    { -3, 23.099939907648682 },
    { -3, 22.065218020317978 },
    { -3, 21.084238737301266 },
    { 2.6801932600117766, 57 },
    { -1.8389190422298318, 22 },
    { -1.5355374742675185, 22 },
    { 1.064553146503239, 35.978260659894012 },
    { 1.1723732032575078, 36.021739340105995 },
    { 1.8794082014944062, 43 },
    { -1.113466179497939, 22 },
    { -0.88375606951560459, 22 },
    { -3, 120 },
    { 2.6644625257324814, 57.021739340105988 },
    { -1.5864431997313042, 22 },
    { 4, 74.418780347578021 },
    { 3.9794082014944059, 78 },
    { 4, 77.013888754932282 },
    { 2.6758453919905785, 57 },
    { 4, 120 },
    { 4, 75.269323589958788 },
    { 3.9609903369994113, 78 },
    { 4, 77.566424689782124 },
    { -3, 120 },
    { -1.379498822265163, 22 },
    { -3, 20.163345512551523 },
    { -3, 22.346617949793902 },
    { -3, 21.653382050206098 },
    { 0.5, 26.666666666666664 },
    { -3, 20.900060092351318 },
    { 4, 76.461352820082439 },
    { -0.30295899252797059, 29 },
    { -0.21355680026869611, 29 },
    { 2.7310989854755623, 57 },
    { 3.2566424689782125, 64 },
    { 3.3065218020317975, 64.021739340105995 },
    { 1.7391002837713461, 43 },
    { -0.25039252925868566, 29 },
    { -0.17237320325750802, 29 },
    { 3.2653382050206097, 64 },
    { 3.3092089322474973, 63.978260659894012 },
    { -3, 120 },
    { 4, 120 },
    { -3, 120 },
    { 3.3416967932162884, 64 },
    { -0.11545887196702459, 29 },
    { -1.8020833132398424, 22 },
    { -1.6688103937536805, 22 },
    { -1.5469203405256149, 22 },
    { 3.3644625257324816, 63.978260659894012 },
    { 3.8886624441272817, 71 },
    { 3.3135568002686959, 64.021739340105995 },
    { 1.7347524157501475, 43 },
    { 3.2907910677525023, 64 },
    { -1.4602052784820403, 22 },
    { -0.14257247250441651, 29 },
    { -1.7056461227436699, 22 },
    { -1.6021739340105994, 22 },
    { 3.327626796742492, 64 },
    { -1.5154588719670246, 22 },
    { -0.12850247603062037, 29 },
    { -3, 120 },
    { 4, 120 },
    { -3, 120 },
    { -3, 19.908816885232596 },
    { -3, 21.241546080094217 },
    { -3, 20.829710109982337 },
    { -1.25, 73.333333333333329 },
    { -3, 22 },
    { -3, 21.609903369994115 },
    { -3, 21.268417382251211 },
    { 4, -20 },
    { 3.3547403972798837, 64 },
    { -3, 20.900060092351318 },
    { -0.057518148266340753, 29 },
    { 1.7006038169758575, 43 },
    { 3.3390096630005885, 64 },
    { 3.8645531465032388, 71 },
    { 3.3021739340105989, 64 },
    { 3.327626796742492, 64 },
    { -3, 120 },
};
static const unsigned char rep_met05_4_y[145] = {
    1, 0, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 1, 1, 1, 1, 0, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1, 0, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1, 0, 1, 1, 1, 0, 1, 1, 1, 1, 0, 1, 1, 1, 0, 0, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1, 0, 1, 0, 1, 1, 1, 1, 0, 1, 1, 1, 1, 0, 1, 0, 1, 1, 1, 1, 0, 1, 1, 1, 0, 1, 0, 1, 1, 1
};


static void audiogram_desc(yaep_desc* d, yaep_model model, yaep_acq acq) {
    memset(d, 0, sizeof(*d));
    d->n_dims = 2;
    d->lo[0] = -3.0; d->hi[0] = 4.0;
    d->lo[1] = -20.0; d->hi[1] = 120.0;
    d->intensity_dim = 1;
    d->grid[0] = 11; d->grid[1] = 21;
    d->refine_steps = 2;
    d->n_init = 5;
    d->fit = true;
    d->fit_every = 20;
    d->target_p = 0.75;
    d->stop_trials = 150;
    d->max_trials = 150;
    d->model = model;
    d->acq = acq;
}

static void replay(yaep_gp* g, const double (*X)[2], const unsigned char* Y, int n,
                   const char* name) {
    for (int i = 0; i < n; i++) {
        int rc = yaep_update(g, X[i], Y[i]);
        CHECK(rc == YAEP_OK, "%s: update %d: %s", name, i + 1, yaep_strerror(rc));
    }
}

/* Defect 1: a scheduled fit of the psychometric model restored its accepted
 * point from the REJECTED point's mode, the Gauss-Newton search ran into its
 * step cap on a saturated plateau, the header reported success, and the log
 * marginal likelihood read -1e49 from then on. */
static void test_regress_collapse(void) {
    struct { const double (*x)[2]; const unsigned char* y; int n; yaep_acq acq; const char* name; } run[2] = {
        { rep_on05_6_x, rep_on05_6_y, 105, YAEP_ACQ_BALD, "older-normal beta 0.5, rep 6" },
        { rep_on2_14_x, rep_on2_14_y, 85, YAEP_ACQ_BALV, "older-normal beta 2, rep 14" },
    };
    for (int r = 0; r < 2; r++) {
        yaep_desc d;
        yaep_gp g;
        int crossed = 0;
        double lm = 0.0;
        audiogram_desc(&d, YAEP_MODEL_PSYCHOMETRIC, run[r].acq);
        CHECK(yaep_open(&g, &d), "open: %s", yaep_error(&g));
        replay(&g, run[r].x, run[r].y, run[r].n, run[r].name);
        lm = yaep_log_marginal(&g);
        for (int c = 0; c < 8; c++) {
            double ctx[1], th, lo, hi;
            ctx[0] = -3.0 + 7.0 * c / 7.0;
            if (yaep_threshold(&g, ctx, 0.0, &th, &lo, &hi) == YAEP_OK) crossed++;
        }
        CHECK(lm == lm && lm > -1e3, "%s: log marginal %.4g after the fit", run[r].name, lm);
        CHECK(crossed == 8, "%s: the threshold crossed at %d of 8 contexts", run[r].name, crossed);
        printf("  %s: after the fit that collapsed in 0.4.1, log marginal %.2f, "
               "%d of 8 contexts cross\n", run[r].name, lm, crossed);
        yaep_close(&g);
    }
    /* The mode search itself: plant a warm start far from the mode (the latents
     * a rescaled K produces) and refit. What comes back has to be the mode a
     * cold start finds, not a plateau. */
    {
        yaep_desc d;
        yaep_gp g;
        yaep__scr s;
        double cold = 0.0, warm = 0.0;
        int ld;
        audiogram_desc(&d, YAEP_MODEL_PSYCHOMETRIC, YAEP_ACQ_BALD);
        d.fit = false;
        CHECK(yaep_open(&g, &d), "open: %s", yaep_error(&g));
        replay(&g, rep_on05_6_x, rep_on05_6_y, 60, "planted start");
        CHECK(yaep__infer(&g, YAEP__START_COLD, 0) == YAEP_OK, "cold fit");
        cold = yaep_log_marginal(&g);
        yaep__scr_of(&g, &s);
        ld = g.N_max;
        for (int i = 0; i < g.N; i++) {
            yaep__psv(&s, ld, YAEP__PS_AM)[i] = 1e6 * (i % 2 ? 1.0 : -1.0);
            yaep__psv(&s, ld, YAEP__PS_AG)[i] = 1e3;
        }
        CHECK(yaep_refit(&g) == YAEP_OK, "refit from a planted start");
        warm = yaep_log_marginal(&g);
        CHECK(fabs(warm - cold) < 1e-6 * (1.0 + fabs(cold)) * tolx,
              "planted warm start: log marginal %.10g, cold %.10g", warm, cold);
        printf("  a warm start planted at 1e6 comes back to the cold mode: %.6f against %.6f\n",
               warm, cold);
        yaep_close(&g);
    }
}

/* Defect 2: the GP model's fit at trial 145 of metabolic beta 0.5, rep 4,
 * jumped to a short-lengthscale mode the data favor by 12 nats, interpolated
 * the sampled columns, sent every unsampled one to the mean, and the
 * threshold error went from 2.0 to 37.8 dB. The level-set guard undoes it. */
static void test_regress_lengthscale(void) {
    yaep_desc d;
    yaep_gp g;
    yaep_hyper h;
    memset(&h, 0, sizeof(h));
    audiogram_desc(&d, YAEP_MODEL_GP, YAEP_ACQ_LSE);
    CHECK(yaep_open(&g, &d), "open: %s", yaep_error(&g));
    replay(&g, rep_met05_4_x, rep_met05_4_y, 145, "metabolic beta 0.5, rep 4");
    memset(&h, 0, sizeof(h));
    yaep_get_hyper(&g, &h);
    CHECK(h.lengthscale[0] > 1.5 && h.lengthscale[1] > 20.0,
          "metabolic beta 0.5, rep 4: lengthscales (%.2f, %.2f) after trial 145",
          h.lengthscale[0], h.lengthscale[1]);
    CHECK(g.fit_blocked, "the level-set guard did not act at trial 145");
    printf("  metabolic beta 0.5, rep 4: the fit at trial 145 was undone, "
           "lengthscales stay (%.2f, %.2f)\n", h.lengthscale[0], h.lengthscale[1]);
    yaep_close(&g);
}

/* Defect 3: AEPsych's novel discrimination function. p has a floor of 0.5 at
 * the bottom edge and rises steeply, so most responses are yes; without a prior
 * on the mean it ran to its bound, the model called the whole box above 0.75,
 * and most columns lost their crossing. */
static double nd_theta(double c) { double q = -1.0 + 0.2 * c; return 2.0 * (0.05 + 0.4 * q * q * c * c); }
static double nd_p(const double* x) { return 0.5 * erfc(-(2.0 * (x[1] + 1.0) / nd_theta(x[0])) / sqrt(2.0)); }

static void test_regress_novel(void) {
    const int reps = 5;
    double nocross = 0.0, mean = 0.0;
    for (int r = 0; r < reps; r++) {
        yaep_desc d;
        yaep_gp g;
        yaep_hyper h;
        memset(&h, 0, sizeof(h));
        rng_state = 0x243F6A8885A308D3ull + 0xD15Cull + (uint64_t)r * 0x1000193ull;
        memset(&d, 0, sizeof(d));
        d.n_dims = 2;
        d.lo[0] = -1.0; d.hi[0] = 1.0; d.lo[1] = -1.0; d.hi[1] = 1.0;
        d.intensity_dim = 1;
        d.grid[0] = 11; d.grid[1] = 21;
        d.refine_steps = 2; d.n_init = 5; d.fit = true; d.fit_every = 20;
        d.target_p = 0.75; d.stop_trials = 150; d.acq = YAEP_ACQ_EAVC;
        CHECK(yaep_open(&g, &d), "open: %s", yaep_error(&g));
        while (!yaep_done(&g)) {
            double x[2] = { 0.0 }, p[2] = { 0.0 };
            yaep_next(&g, x);
            p[1] = nd_p(x); p[0] = 1.0 - p[1];
            yaep_update(&g, x, yaep_simulate_outcome(p, 2, rng_u()));
        }
        for (int c = 0; c < 30; c++) {
            double ctx[1], th, lo, hi;
            ctx[0] = -1.0 + 2.0 * c / 29.0;
            if (yaep_threshold(&g, ctx, 0.0, &th, &lo, &hi) != YAEP_OK) nocross += 1.0 / reps;
        }
        memset(&h, 0, sizeof(h));
        yaep_get_hyper(&g, &h);
        mean += h.mean / reps;
        yaep_close(&g);
    }
    CHECK(nocross < 8.0, "novel discrimination, EAVC: %.1f of 30 contexts without a crossing", nocross);
    CHECK(mean < 2.0, "novel discrimination, EAVC: fitted mean %.2f", mean);
    printf("  novel discrimination, EAVC, %d runs of 150 trials: %.1f of 30 contexts "
           "without a crossing, fitted mean %.2f\n", reps, nocross, mean);
}

static void test_regressions(void) {
    printf("regressions from the cross-method comparison:\n");
    test_regress_collapse();
    test_regress_lengthscale();
    test_regress_novel();
}

/* --- desc.priors -------------------------------------------------------- */

/* The priors block: zero reports the measured defaults; the same values given
 * explicitly reproduce the default run to the last bit; a negative sd turns
 * one prior off and matches desc.no_hyper_prior for the model that has only
 * that one moving; and a changed center changes the fit the way the objective
 * says it should. */
static double priors_run(const yaep_priors* pr, bool no_prior, yaep_hyper* h) {
    yaep_desc d;
    yaep_gp g;
    double lm = 0.0;
    ps_desc(&d, YAEP_ACQ_LSE, YAEP_MODEL_PSYCHOMETRIC);
    d.stop_trials = 60;
    d.fit = true; d.fit_every = 20;
    d.no_hyper_prior = no_prior;
    if (pr) d.priors = *pr;
    rng_state = 0x243F6A8885A308D3ull + 0x5050ull;
    if (!yaep_open(&g, &d)) { CHECK(0, "open: %s", yaep_error(&g)); return 0.0; }
    while (!yaep_done(&g)) {
        double x[2] = { 0.0 }, p2[2] = { 0.0 };
        yaep_next(&g, x);
        p2[1] = ps_ptrue(x); p2[0] = 1.0 - p2[1];
        yaep_update(&g, x, yaep_simulate_outcome(p2, 2, rng_u()));
    }
    memset(h, 0, sizeof(*h));
    yaep_get_hyper(&g, h);
    lm = yaep_log_marginal(&g);
    yaep_close(&g);
    return lm;
}

static void test_priors(void) {
    yaep_desc d;
    yaep_gp g;
    yaep_priors pr, pr2;
    memset(&pr, 0, sizeof(pr));
    memset(&pr2, 0, sizeof(pr2));
    yaep_hyper h0, h1, h2;
    memset(&h0, 0, sizeof(h0));
    memset(&h1, 0, sizeof(h1));
    memset(&h2, 0, sizeof(h2));
    double lm0 = 0.0, lm1 = 0.0, lm2 = 0.0;
    printf("desc.priors:\n");
    ps_desc(&d, YAEP_ACQ_LSE, YAEP_MODEL_PSYCHOMETRIC);
    d.stop_trials = 10;
    CHECK(yaep_open(&g, &d), "open: %s", yaep_error(&g));
    CHECK(yaep_get_priors(&g, &pr) == YAEP_OK, "get_priors");
    CHECK(pr.lengthscale.center == 0.25 && pr.lengthscale.sd == 1.0 &&
          pr.outputscale.center == 625.0 && pr.outputscale.sd == 0.7 &&
          pr.outputscale_g.center == 0.3 && pr.mean.sd == 0.7 &&
          pr.mean_g.center == 0.25 && pr.mean_g.sd == 1.0 &&
          pr.mean_g.ceiling == 1.0 / 256.0 && pr.noise_sd.sd == -1.0,
          "the reported defaults are not the documented ones");
    yaep_close(&g);
    d.priors.outputscale_g.center = -1.0;
    CHECK(!yaep_open(&g, &d), "open accepted a negative prior center");
    d.priors.outputscale_g.center = 0.0;
    d.no_hyper_prior = true;
    CHECK(yaep_open(&g, &d), "open: %s", yaep_error(&g));
    yaep_get_priors(&g, &pr2);
    CHECK(pr2.lengthscale.sd == -1.0 && pr2.mean_g.sd == -1.0,
          "no_hyper_prior is not reported as every prior off");
    yaep_close(&g);

    lm0 = priors_run(NULL, false, &h0);
    lm1 = priors_run(&pr, false, &h1);       /* the defaults, given explicitly */
    CHECK(lm0 == lm1 && memcmp(&h0, &h1, sizeof(h0)) == 0,
          "the defaults given explicitly do not reproduce the default run");
    pr2 = pr;
    pr2.mean_g.center = 0.05;                /* a rise a twentieth of the axis */
    lm2 = priors_run(&pr2, false, &h2);
    CHECK(h2.mean_g > h0.mean_g, "a steeper prior slope did not steepen the fit "
          "(%.3f against %.3f)", h2.mean_g, h0.mean_g);
    printf("  defaults reported and reproduced bit for bit (log marginal %.4f); a prior "
           "rise of 1/20 of the axis moves the fitted mean log-slope from %.3f to %.3f"
           "\n", lm0, h0.mean_g, h2.mean_g);
    (void)lm2;
}

/* --- optimization acquisitions ----------------------------------------- */

/* A bump with its maximum of 1 at (0.3, 0.7) and -1 far from it. */
static double bump(const double* x) {
    double a = x[0] - 0.3, b = x[1] - 0.7;
    return 2.0 * exp(-(a * a + b * b) / (2.0 * 0.15 * 0.15)) - 1.0;
}

static double opt_session(yaep_acq acq, yaep_lik lik, bool minimize, int trials,
                          int rep, double* value) {
    yaep_desc d;
    yaep_gp g;
    double x[2], dx, dy;
    rng_state = 0x243F6A8885A308D3ull + 0x0B7ull + (uint64_t)rep * 0x1000193ull +
                (uint64_t)acq * 77u;
    memset(&d, 0, sizeof(d));
    d.n_dims = 2;
    d.lo[0] = 0.0; d.hi[0] = 1.0; d.lo[1] = 0.0; d.hi[1] = 1.0;
    d.lik = lik;
    d.acq = acq;
    d.minimize = minimize;
    d.grid[0] = 21; d.grid[1] = 21;
    d.n_init = 8; d.fit = true; d.fit_every = 10;
    d.refine_steps = 2;
    d.stop_trials = trials;
    d.rng = rng_cb;
    if (!yaep_open(&g, &d)) { CHECK(0, "open: %s", yaep_error(&g)); return 1.0; }
    while (!yaep_done(&g)) {
        double xn[2], fv;
        int rc;
        yaep_next(&g, xn);
        fv = minimize ? -bump(xn) : bump(xn);
        if (lik == YAEP_LIK_GAUSSIAN) {
            double u1 = rng_u(), u2 = rng_u();
            if (u1 < 1e-300) u1 = 1e-300;
            rc = yaep_update_real(&g, xn, fv + 0.1 * sqrt(-2.0 * log(u1)) * cos(6.283185307179586 * u2));
        } else {
            double p[2] = { 0.0 };
            p[1] = 0.5 * erfc(-(2.0 * fv) / sqrt(2.0)); p[0] = 1.0 - p[1];
            rc = yaep_update(&g, xn, yaep_simulate_outcome(p, 2, rng_u()));
        }
        CHECK(rc == YAEP_OK, "optimization update: %s", yaep_strerror(rc));
    }
    CHECK(yaep_argmax(&g, x, value) >= -1, "argmax");
    yaep_close(&g);
    dx = x[0] - 0.3; dy = x[1] - 0.7;
    return sqrt(dx * dx + dy * dy);
}

static void test_optimize(void) {
    static const yaep_acq acqs[3] = { YAEP_ACQ_UCB, YAEP_ACQ_EI, YAEP_ACQ_THOMPSON };
    static const char* const names[3] = { "UCB", "EI", "Thompson" };
    yaep_desc d;
    yaep_gp g;
    printf("optimization acquisitions:" "\n");
    memset(&d, 0, sizeof(d));
    d.n_dims = 2;
    d.lo[0] = 0.0; d.hi[0] = 1.0; d.lo[1] = 0.0; d.hi[1] = 1.0;
    d.acq = YAEP_ACQ_THOMPSON; d.stop_trials = 10;
    CHECK(!yaep_open(&g, &d), "open accepted Thompson without desc.rng");
    d.rng = rng_cb; d.model = YAEP_MODEL_PSYCHOMETRIC; d.intensity_dim = 1;
    d.target_p = 0.75;
    CHECK(!yaep_open(&g, &d), "open accepted Thompson with the psychometric model");

    /* EI's closed form under GAUSSIAN against the integral it is (GAUSSIAN
     * needs the double build). */
    if (real_is_double) {
        double worst = 0.0;
        memset(&d, 0, sizeof(d));
        d.n_dims = 1; d.lo[0] = 0.0; d.hi[0] = 1.0;
        d.lik = YAEP_LIK_GAUSSIAN; d.acq = YAEP_ACQ_EI; d.stop_trials = 10;
        CHECK(yaep_open(&g, &d), "open: %s", yaep_error(&g));
        for (int k = 0; k < 6; k++) {
            yaep__scr s;
            double mu = -0.6 + 0.3 * k, sd = 0.05 + 0.1 * k, acc = 0.0;
            const int n = 200000;
            yaep__scr_of(&g, &s);
            g.opt_best = 0.2;
            for (int i = 0; i < n; i++) {
                double m = mu - 10.0 * sd + 20.0 * sd * (i + 0.5) / n;
                double w = exp(-0.5 * (m - mu) * (m - mu) / (sd * sd)) / (sd * 2.5066282746310002);
                if (m > 0.2) acc += (m - 0.2) * w * 20.0 * sd / n;
            }
            track(&worst, fabs(yaep__ei(&g, &s, mu, sd, 0.0) - acc));
        }
        CHECK(worst < 1e-9, "EI closed form vs the integral %.2e", worst);
        printf("  EI's closed form under GAUSSIAN against its integral: %.1e" "\n", worst);
        yaep_close(&g);
    }

    for (int a = 0; a < 3; a++) {
        double dg = 0.0, db = 0.0, dm = 0.0, vg = 0.0, vm = 0.0, v;
        const int reps = 3;
        for (int r = 0; r < reps; r++) {
            if (real_is_double) {
                dg += opt_session(acqs[a], YAEP_LIK_GAUSSIAN, false, 40, r, &v) / reps;
                vg += v / reps;
                dm += opt_session(acqs[a], YAEP_LIK_GAUSSIAN, true, 40, r, &v) / reps;
                vm += v / reps;
            } else {
                vg = 1.0; vm = -1.0;
            }
            db += opt_session(acqs[a], YAEP_LIK_BERNOULLI, false, 80, r, &v) / reps;
        }
        printf("  %-8s argmax distance from the optimum: GAUSSIAN %.3f (value %.2f, true 1), "
               "minimized %.3f (value %.2f, true -1), BERNOULLI %.3f" "\n",
               names[a], dg, vg, dm, vm, db);
        CHECK(dg < 0.08, "%s: GAUSSIAN argmax %.3f from the optimum", names[a], dg);
        CHECK(dm < 0.08, "%s: minimized argmax %.3f from the optimum", names[a], dm);
        CHECK(fabs(vg - 1.0) < 0.2 && fabs(vm + 1.0) < 0.2,
              "%s: argmax values %.2f and %.2f", names[a], vg, vm);
        /* UCB is not held to the BERNOULLI bump: with a small bump and a floor
         * of 0.02, a run whose first trials all answer 0 sends UCB to the box
         * corners, where the latent sd is largest and a tail observation
         * shrinks it slowly, and 80 trials can pass before it finds the bump
         * (0.16 to 0.21 from the optimum on average over 10 runs at any beta).
         * EI and Thompson weigh the mean and do not wander. */
        if (acqs[a] != YAEP_ACQ_UCB)
            CHECK(db < 0.15, "%s: BERNOULLI argmax %.3f from the optimum", names[a], db);
    }
}

/* --- YAEP_LIK_PAIRWISE ------------------------------------------------- */

static void pair_desc(yaep_desc* d, yaep_acq acq) {
    memset(d, 0, sizeof(*d));
    d->n_dims = 2;
    d->lo[0] = 0.0; d->hi[0] = 1.0; d->lo[1] = 0.0; d->hi[1] = 1.0;
    d->lik = YAEP_LIK_PAIRWISE;
    d->acq = acq;
    d->grid[0] = 15; d->grid[1] = 15;
    d->n_init = 6;
    d->rng = rng_cb;
}

/* A preference observer with the bump of the optimization test as its
 * utility: x1 is preferred with probability Phi(2 (u(x1) - u(x2))). */
static int prefer(const double* x1, const double* x2) {
    double p[2] = { 0.0 };
    p[1] = 0.5 * erfc(-(2.0 * (bump(x1) - bump(x2))) / sqrt(2.0));
    p[0] = 1.0 - p[1];
    return yaep_simulate_outcome(p, 2, rng_u());
}

static void test_pairwise_math(void) {
    enum { NT = 24 };
    static double K[4 * NT * NT], A[4 * NT * NT], S[4 * NT * NT];
    yaep_desc d;
    yaep_gp g;
    yaep__scr s;
    double worst_k = 0.0, worst_mode = 0.0, pw_var = 0.0, worst_pair = 0.0;
    int n, n2, ld;
    rng_state = 0x243F6A8885A308D3ull + 0xFA17ull;
    pair_desc(&d, YAEP_ACQ_BALD);
    d.stop_trials = 100;
    d.hyper.lengthscale[0] = 0.3; d.hyper.lengthscale[1] = 0.3;
    d.hyper.outputscale = 1.5;
    d.jitter = 1e-8;
    CHECK(yaep_open(&g, &d), "open pairwise: %s", yaep_error(&g));
    for (int i = 0; i < NT; i++) {
        double x1[2] = { 0.0 }, x2[2] = { 0.0 };
        x1[0] = rng_u(); x1[1] = rng_u(); x2[0] = rng_u(); x2[1] = rng_u();
        CHECK(yaep_update_pair(&g, x1, x2, prefer(x1, x2)) == YAEP_OK, "update_pair %d", i);
    }
    n = g.N; n2 = 2 * n; ld = g.N_max;
    yaep__scr_of(&g, &s);
    /* The 2N point kernel, stimuli x1 then x2. */
    for (int i = 0; i < n2; i++)
        for (int j = 0; j < n2; j++) {
            const double* xi = i < n ? g.X + (size_t)i * 2 : g.X2 + (size_t)(i - n) * 2;
            const double* xj = j < n ? g.X + (size_t)j * 2 : g.X2 + (size_t)(j - n) * 2;
            K[(size_t)i * n2 + j] = yaep__kernel(&g, xi, xj) + (i == j ? 1e-8 : 0.0);
        }
    /* 1. The trial kernel is D K D' for the difference operator D. */
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++) {
            double v = K[(size_t)i * n2 + j] - K[(size_t)i * n2 + n + j] -
                       K[(size_t)(n + i) * n2 + j] + K[(size_t)(n + i) * n2 + n + j];
            double kd = g.Kmat[(size_t)i * ld + j] - (i == j ? 1e-8 : 0.0);
            if (i == j) v -= 2e-8;   /* the point kernel's two jitters */
            track(&worst_k, fabs(v - kd));
        }
    /* 2. The utility's predicted differences at the trials are the mode. */
    for (int i = 0; i < n; i++) {
        double m1 = 0.0, m2 = 0.0;
        yaep_predict_f(&g, g.X + (size_t)i * 2, 0, &m1, NULL);
        yaep_predict_f(&g, g.X2 + (size_t)i * 2, 0, &m2, NULL);
        track(&worst_mode, fabs((m1 - m2) - g.f[i]) / (1.0 + fabs(g.f[i])));
    }
    /* 3. The utility's predictive variance against the dense posterior over
     * the 2N point latents: Sigma = (K^-1 + D' W D)^-1. */
    memcpy(A, K, sizeof(double) * (size_t)n2 * n2);
    tinv(A, n2);
    memcpy(S, A, sizeof(double) * (size_t)n2 * n2);
    for (int i = 0; i < n; i++) {
        double w = g.W[i];
        S[(size_t)i * n2 + i] += w;
        S[(size_t)(n + i) * n2 + (n + i)] += w;
        S[(size_t)i * n2 + (n + i)] -= w;
        S[(size_t)(n + i) * n2 + i] -= w;
    }
    tinv(S, n2);
    for (int t0 = 0; t0 < 4; t0++) {
        double xs[2][2], ka[2][64], aa[2][64], mean[2], cov[2][2];
        for (int q = 0; q < 2; q++) {
            xs[q][0] = 0.15 + 0.2 * t0 + 0.1 * q; xs[q][1] = 0.8 - 0.15 * t0 - 0.2 * q;
            for (int i = 0; i < n2; i++) {
                const double* xi = i < n ? g.X + (size_t)i * 2 : g.X2 + (size_t)(i - n) * 2;
                ka[q][i] = yaep__kernel(&g, xi, xs[q]);
            }
            for (int i = 0; i < n2; i++) {
                double acc = 0.0;
                for (int j = 0; j < n2; j++) acc += A[(size_t)i * n2 + j] * ka[q][j];
                aa[q][i] = acc;
            }
            mean[q] = 0.0;
        }
        for (int q = 0; q < 2; q++)
            for (int r = 0; r < 2; r++) {
                double c = yaep__kernel(&g, xs[q], xs[r]);
                for (int i = 0; i < n2; i++) c -= ka[q][i] * aa[r][i];
                for (int i = 0; i < n2; i++) {
                    double acc = 0.0;
                    for (int j = 0; j < n2; j++) acc += S[(size_t)i * n2 + j] * aa[r][j];
                    c += aa[q][i] * acc;
                }
                cov[q][r] = c;
            }
        for (int q = 0; q < 2; q++) {
            double sd = 0.0;
            yaep_predict_f(&g, xs[q], 0, &mean[q], &sd);
            track(&pw_var, fabs(sd * sd - cov[q][q]) / (1.0 + cov[q][q]));
        }
        {
            double vd = cov[0][0] + cov[1][1] - 2.0 * cov[0][1];
            double pd = 0.5 * erfc(-((mean[0] - mean[1]) / sqrt(1.0 + vd)) / sqrt(2.0));
            track(&worst_pair, fabs(pd - yaep_predict_pair(&g, xs[0], xs[1])));
        }
    }
    CHECK(worst_k < (real_is_double ? 1e-12 : 1e-5), "pairwise: trial kernel vs D K D' %.2e", worst_k);
    CHECK(worst_mode < (real_is_double ? 1e-8 : 1e-5), "pairwise: predicted differences vs the mode %.2e", worst_mode);
    CHECK(pw_var < 1e-6 * tolx, "pairwise: utility variance vs dense 2N %.2e", pw_var);
    CHECK(worst_pair < 1e-6 * tolx, "pairwise: P(x1 preferred) vs dense %.2e", worst_pair);
    printf("  trial kernel vs D K D' %.1e, predicted differences vs the mode %.1e,\n"
           "  utility variance vs a dense 2N posterior %.1e, P(x1 preferred) %.1e\n",
           worst_k, worst_mode, pw_var, worst_pair);
    yaep_close(&g);

    /* 4. The analytic hyperparameter gradient against differences. */
    {
        yaep__param pp[YAEP__NTHETA];
        double th[YAEP__NTHETA], gr[YAEP__NTHETA], tw[YAEP__NTHETA], v, worst = 0.0;
        int np;
        pair_desc(&d, YAEP_ACQ_BALD);
        d.stop_trials = 100;
        rng_state = 0x243F6A8885A308D3ull + 0xFA18ull;
        CHECK(yaep_open(&g, &d), "open: %s", yaep_error(&g));
        for (int i = 0; i < 30; i++) {
            double x1[2] = { 0.0 }, x2[2] = { 0.0 };
            x1[0] = rng_u(); x1[1] = rng_u(); x2[0] = rng_u(); x2[1] = rng_u();
            yaep_update_pair(&g, x1, x2, prefer(x1, x2));
        }
        np = yaep__params(&g, pp);
        yaep__theta_get(&g, pp, np, th);
        for (int i = 0; i < np; i++)
            th[i] = yaep__clamp(th[i] + 0.1 * (double)(i % 3 - 1), pp[i].lo, pp[i].hi);
        yaep__fit_eval(&g, pp, np, th, &v, gr);
        for (int i = 0; i < np; i++) {
            double h = (real_is_double ? 1e-5 : 3e-3) * (1.0 + fabs(th[i])), vp, vm, fd;
            memcpy(tw, th, sizeof(double) * (size_t)np);
            tw[i] = th[i] + h; yaep__fit_eval(&g, pp, np, tw, &vp, NULL);
            tw[i] = th[i] - h; yaep__fit_eval(&g, pp, np, tw, &vm, NULL);
            fd = (vp - vm) / (2.0 * h);
            track(&worst, fabs(gr[i] - fd) / (1e-3 + fabs(fd)));
        }
        track(&worst_grad, worst);
        CHECK(np == 3, "pairwise: %d free hyperparameters, expected 3 (no mean)", np);
        CHECK(worst < (real_is_double ? 1e-4 : 5e-2), "pairwise gradient vs differences %.2e", worst);
        printf("  analytic gradient vs central differences over the 3 hyperparameters: %.1e\n",
               worst);
        yaep_close(&g);
    }
}

static void test_pairwise(void) {
    static const yaep_acq acqs[3] = { YAEP_ACQ_BALD, YAEP_ACQ_BALV, YAEP_ACQ_THOMPSON };
    static const char* const names[3] = { "BALD", "BALV", "Thompson" };
    yaep_desc d;
    yaep_gp g;
    double x[2] = { 0.0 };
    printf("YAEP_LIK_PAIRWISE:\n");
    pair_desc(&d, YAEP_ACQ_LSE); d.stop_trials = 10;
    CHECK(!yaep_open(&g, &d), "open accepted LSE with PAIRWISE");
    pair_desc(&d, YAEP_ACQ_BALD); d.stop_trials = 10; d.guess = 0.5;
    CHECK(!yaep_open(&g, &d), "open accepted a guess with PAIRWISE");
    pair_desc(&d, YAEP_ACQ_BALD); d.stop_trials = 10;
    CHECK(yaep_open(&g, &d), "open: %s", yaep_error(&g));
    CHECK(yaep_next(&g, x) < -1, "yaep_next accepted a PAIRWISE handle");
    CHECK(yaep_update(&g, x, 1) == YAEP_ERR_ARG, "yaep_update accepted a PAIRWISE handle");
    yaep_close(&g);
    test_pairwise_math();
    for (int a = 0; a < 3; a++) {
        double dist = 0.0;
        const int reps = 3;
        for (int r = 0; r < reps; r++) {
            rng_state = 0x243F6A8885A308D3ull + 0xFA20ull + (uint64_t)r * 0x1000193ull +
                        (uint64_t)a * 131u;
            pair_desc(&d, acqs[a]);
            d.fit = true; d.fit_every = 10; d.refine_steps = 2;
            d.stop_trials = 60;
            CHECK(yaep_open(&g, &d), "open: %s", yaep_error(&g));
            while (!yaep_done(&g)) {
                double x1[2] = { 0.0 }, x2[2] = { 0.0 };
                CHECK(yaep_next_pair(&g, x1, x2) == YAEP_OK, "next_pair");
                CHECK(yaep_update_pair(&g, x1, x2, prefer(x1, x2)) == YAEP_OK, "update_pair");
            }
            yaep_argmax(&g, x, NULL);
            dist += hypot(x[0] - 0.3, x[1] - 0.7) / reps;
            yaep_close(&g);
        }
        printf("  %-8s 60 comparisons, argmax of the utility %.3f from the preferred stimulus\n",
               names[a], dist);
        /* BALV maximizes the uncertainty of a comparison and spreads its
         * pairs rather than closing in on the peak: 0.107 on average over 10
         * runs, against 0.061 for BALD and 0.077 for Thompson. */
        CHECK(dist < (acqs[a] == YAEP_ACQ_BALV ? 0.2 : 0.1),
              "pairwise %s: argmax %.3f from the optimum", names[a], dist);
    }
}

static void test_simulate_outcome(void) {
    double p[3] = { 0.2, 0.3, 0.5 };
    CHECK(yaep_simulate_outcome(p, 3, 0.0) == 0, "simulate at u = 0");
    CHECK(yaep_simulate_outcome(p, 3, 0.19) == 0, "simulate at u = 0.19");
    CHECK(yaep_simulate_outcome(p, 3, 0.21) == 1, "simulate at u = 0.21");
    CHECK(yaep_simulate_outcome(p, 3, 0.51) == 2, "simulate at u = 0.51");
    CHECK(yaep_simulate_outcome(p, 3, 1.0) == 2, "simulate at u = 1");
    CHECK(yaep_simulate_outcome(NULL, 3, 0.5) == YAEP_ERR_ARG, "null probs");
}

/* --- the async layer, only when it was compiled in --------------------- */
#ifdef YAEP_ASYNC

/* One 1-D session, driven synchronously, recording what it showed and what it
 * heard, so the async run can be handed the same responses. */
#define ASYNC_TRIALS 40

static double async_x[ASYNC_TRIALS];
static int async_y[ASYNC_TRIALS];

static void async_reference(yaep_gp* g, yaep_desc* d) {
    rng_state = 0x243F6A8885A308D3ull + 0xee00ull;
    memset(d, 0, sizeof(*d));
    d->n_dims = 1;
    d->lo[0] = 0.0; d->hi[0] = 1.0;
    d->acq = YAEP_ACQ_LSE;
    d->target_p = 0.75;
    d->n_candidates = 33;
    d->grid[0] = 33;
    d->n_init = 6;
    d->fit = true;
    d->fit_every = 10;
    d->stop_trials = ASYNC_TRIALS;
    CHECK(yaep_open(g, d), "open async reference: %s", yaep_error(g));
    for (int i = 0; i < ASYNC_TRIALS; i++) {
        double x[1] = { 0.0 };
        yaep_next(g, x);
        async_x[i] = x[0];
        async_y[i] = observe(x[0]);
        CHECK(yaep_update(g, x, async_y[i]) == YAEP_OK, "reference update");
    }
}

/* The same session through the pump. The proposals come out of the snapshot,
 * the responses out of the same observer with the same seed, so if the layer
 * changes nothing the two runs are the same run. */
static void test_async(void) {
    yaep_desc d;
    yaep_gp ref, g;
    yaep_async a;
    yaep_snapshot s;
    memset(&s, 0, sizeof(s));
    int n = 0, nref = 0;
    const yaep_trial *href, *hasync;
    async_reference(&ref, &d);
    href = yaep_history(&ref, &nref);

    memset(&a, 0, sizeof(a));
    memset(&g, 0, sizeof(g));
    CHECK(yaep_open(&g, &d), "open async gp: %s", yaep_error(&g));
    {
        yaep_async_desc ad;
        memset(&ad, 0, sizeof(ad));
        ad.gp = &g;
        CHECK(yaep_async_open(&a, &ad), "async start: %s", yaep_async_error(&a));
        CHECK(yaep_async_is_running(&a), "the thread is not running");
    }
    rng_state = 0x243F6A8885A308D3ull + 0xee00ull;
    CHECK(yaep_async_poll(&a, &s) == 0, "the first poll is seq 0");
    for (int i = 0; i < ASYNC_TRIALS; i++) {
        int seq, rc, y;
        CLOSE(s.x[0], async_x[i], 0.0, "async proposal %d differs", i);
        y = observe(s.x[0]);
        CHECK(y == async_y[i], "async response %d differs", i);
        seq = yaep_async_submit(&a, s.x, y);
        CHECK(seq > 0, "submit %d: %s", i, yaep_strerror(seq));
        rc = yaep_async_wait(&a, (uint32_t)seq, YRT_S(20), &s);
        CHECK(rc >= seq, "wait %d returned %d (%s)", i, rc, yaep_strerror(rc));
        CHECK(s.update_rc == YAEP_OK, "update_rc %d on trial %d",
              s.update_rc, i);
        CHECK(s.n_trials == i + 1, "n_trials %d at trial %d", s.n_trials, i);
    }
    CHECK(s.done, "the async run did not reach its stop criterion");
    CHECK(s.numeric == 0, "%d numeric failures on the thread", s.numeric);
    yaep_async_close(&a);
    CHECK(!yaep_async_is_running(&a), "still running after stop");

    /* Bit for bit: the history, the posterior mode and the log marginal. */
    hasync = yaep_history(&g, &n);
    CHECK(n == nref, "async %d trials, reference %d", n, nref);
    CHECK(memcmp(href, hasync, (size_t)n * sizeof(yaep_trial)) == 0,
          "the async history is not the reference history");
    CHECK(memcmp(ref.f, g.f, (size_t)n * sizeof(double)) == 0,
          "the async posterior mode differs");
    CLOSE(yaep_log_marginal(&g), yaep_log_marginal(&ref), 0.0,
          "the async log marginal differs");
    {
        double t1 = 0.0, t2 = 0.0;
        CHECK(yaep_threshold(&g, NULL, 0.0, &t1, NULL, NULL) == YAEP_OK,
              "async threshold");
        CHECK(yaep_threshold(&ref, NULL, 0.0, &t2, NULL, NULL) == YAEP_OK,
              "reference threshold");
        CLOSE(t1, t2, 0.0, "the async threshold differs");
        printf("  async run matches the synchronous run bit for bit; "
               "threshold %.4f, snapshot threshold %.4f\n", t1, s.threshold);
        CLOSE(s.threshold, t1, 1e-12, "the snapshot threshold is stale");
    }
    yaep_close(&g);
    yaep_close(&ref);
}

/* With fit_in_idle the thread fits between trials: the log marginal has to
 * move without any submit, and the run still has to converge. */
static void test_async_fit_in_idle(void) {
    yaep_desc d;
    yaep_gp g;
    yaep_async a;
    yaep_async_desc ad;
    yaep_snapshot s;
    memset(&s, 0, sizeof(s));
    double lm_first = 0.0, lm_last = 0.0;
    int idle_gain = 0;
    rng_state = 0x243F6A8885A308D3ull + 0xef00ull;
    memset(&d, 0, sizeof(d));
    memset(&a, 0, sizeof(a));
    memset(&g, 0, sizeof(g));
    d.n_dims = 1;
    d.lo[0] = 0.0; d.hi[0] = 1.0;
    d.acq = YAEP_ACQ_EAVC;
    d.target_p = 0.75;
    d.n_candidates = 33;
    d.grid[0] = 33;
    d.n_init = 8;
    d.fit = true;
    d.fit_every = 10;          /* ignored while fit_in_idle is set */
    d.stop_trials = 60;
    CHECK(yaep_open(&g, &d), "open fit_in_idle: %s", yaep_error(&g));
    memset(&ad, 0, sizeof(ad));
    ad.gp = &g;
    ad.fit_in_idle = true;
    ad.below_normal = true;
    CHECK(yaep_async_open(&a, &ad), "fit_in_idle start: %s",
          yaep_async_error(&a));
    CHECK(g.desc.fit_every == 0, "fit_every was not parked");
    yaep_async_poll(&a, &s);
    for (int i = 0; i < 60; i++) {
        int seq, rc;
        double lm_before = 0.0, lm_after = 0.0;
        int y = observe(s.x[0]);
        seq = yaep_async_submit(&a, s.x, y);
        CHECK(seq > 0, "fit_in_idle submit: %s", yaep_strerror(seq));
        rc = yaep_async_wait(&a, (uint32_t)seq, YRT_S(20), &s);
        CHECK(rc >= seq, "fit_in_idle wait: %s", yaep_strerror(rc));
        lm_before = s.log_marginal;
        if (i == 5) lm_first = lm_before;
        /* Hand the thread an idle interval, as a frame loop would. */
        yrt_sleep_until(yrt_now_ns() + 12000000ull, 0);
        yaep_async_poll(&a, &s);
        lm_after = s.log_marginal;
        if (lm_after > lm_before + 1e-9) idle_gain++;
        lm_last = lm_after;
    }
    printf("  fit_in_idle: the log marginal improved in an idle gap on %d of "
           "60 trials, %.3f to %.3f\n", idle_gain, lm_first, lm_last);
    CHECK(idle_gain > 0, "the idle fit never improved the log marginal");
    yaep_async_close(&a);
    CHECK(g.desc.fit_every == 10, "fit_every was not restored");
    {
        double thr = 0.0;
        CHECK(yaep_threshold(&g, NULL, 0.0, &thr, NULL, NULL) == YAEP_OK,
              "fit_in_idle threshold");
        printf("  fit_in_idle threshold %.4f (true %.4f)\n", thr, OBS_THR75);
        CHECK(fabs(thr - OBS_THR75) < 0.1,
              "fit_in_idle threshold error %.4f", fabs(thr - OBS_THR75));
    }
    yaep_close(&g);
}

/* A full queue must say so and copy nothing, and a stop must deliver what it
 * accepted. */
static void test_async_queue(void) {
    yaep_desc d;
    yaep_gp g;
    yaep_async a;
    yaep_async_desc ad;
    int accepted = 0, busy = 0;
    rng_state = 0x243F6A8885A308D3ull + 0xf000ull;
    memset(&d, 0, sizeof(d));
    memset(&a, 0, sizeof(a));
    memset(&g, 0, sizeof(g));
    d.n_dims = 1;
    d.lo[0] = 0.0; d.hi[0] = 1.0;
    d.acq = YAEP_ACQ_EAVC;       /* slow enough that the queue can fill */
    d.target_p = 0.75;
    d.n_candidates = 257;
    d.n_init = 4;
    d.max_trials = 300;
    d.stop_trials = 300;
    CHECK(yaep_open(&g, &d), "open queue test: %s", yaep_error(&g));
    memset(&ad, 0, sizeof(ad));
    ad.gp = &g;
    CHECK(yaep_async_open(&a, &ad), "queue start: %s", yaep_async_error(&a));
    for (int i = 0; i < 200 && busy < 3; i++) {
        double x[1] = { 0.0 };
        int rc;
        x[0] = 0.01 + 0.98 * ((double)(i % 17) / 17.0);
        rc = yaep_async_submit(&a, x, i & 1);
        if (rc == YAEP_ERR_BUSY) busy++;
        else { CHECK(rc > 0, "submit %d: %s", i, yaep_strerror(rc)); accepted++; }
    }
    printf("  queue: %d accepted, %d refused with YAEP_ERR_BUSY, %d pending "
           "at stop\n", accepted, busy, yaep_async_pending(&a));
    CHECK(busy >= 1, "the queue never filled, so BUSY was never tested");
    /* Argument checks, on the calling thread. */
    {
        double bad[1] = { 2.0 }, ok[1] = { 0.5 };
        CHECK(yaep_async_submit(&a, bad, 1) == YAEP_ERR_ARG, "x outside the box");
        CHECK(yaep_async_submit(&a, ok, 7) == YAEP_ERR_ARG, "outcome out of range");
        CHECK(yaep_async_submit(&a, NULL, 1) == YAEP_ERR_ARG, "null x");
        CHECK(yaep_async_submit_real(&a, ok, 0.5) == YAEP_ERR_ARG,
              "submit_real under a discrete likelihood");
    }
    yaep_async_close(&a);          /* drains */
    CHECK(yaep_n_trials(&g) == accepted,
          "stop dropped responses: %d trials of %d accepted",
          yaep_n_trials(&g), accepted);
    CHECK(yaep_async_submit(&a, async_x, 1) == YAEP_ERR_CLOSED,
          "submit after stop");
    CHECK(yaep_async_pending(&a) == YAEP_ERR_CLOSED, "pending after stop");
    yaep_close(&g);
    /* A start that cannot work says why and leaves nothing running. */
    {
        yaep_async b;
        yaep_async_desc bd;
        memset(&b, 0, sizeof(b));
        memset(&bd, 0, sizeof(bd));
        CHECK(!yaep_async_open(&b, &bd), "start with no gp");
        CHECK(yaep_async_error(&b)[0] != 0, "start left no message");
        CHECK(!yaep_async_is_running(&b), "a failed start left a thread");
        yaep_async_close(&b);   /* safe on a handle that never started */
    }
}
#endif /* YAEP_ASYNC */

/* --- mixed parameter kinds --------------------------------------------- */

/* A field over an intensity x in [0, 1], a 3-level categorical context c and
 * an integer context k in 0..4: the 50% point is 0.35 + off[c] + 0.04 k. The
 * levels are unordered on purpose, so level 1 is the far one and treating c as
 * a continuous axis puts the wrong neighbor next to it. */
static const double mixed_off[3] = { 0.0, 0.15, 0.05 };

static double mixed_thr(int c, int k) { return 0.35 + mixed_off[c] + 0.04 * (double)k; }

static int mixed_observer(const double* x) {
    int c = (int)floor(x[1] + 0.5), k = (int)floor(x[2] + 0.5);
    double p = 0.5 * erfc(-((x[0] - mixed_thr(c, k)) / 0.08) / sqrt(2.0));
    return rng_u() < p ? 1 : 0;
}

static void mixed_desc(yaep_desc* d, bool kinds, yaep_model model) {
    memset(d, 0, sizeof(*d));
    d->n_dims = 3;
    d->lo[0] = 0.0; d->hi[0] = 1.0;
    d->lo[1] = 0.0; d->hi[1] = 2.0;
    d->lo[2] = 0.0; d->hi[2] = 4.0;
    d->grid[0] = 21; d->grid[1] = 3; d->grid[2] = 5;
    if (kinds) {
        d->dim_kind[1] = YAEP_DIM_CATEGORICAL;
        d->dim_levels[1] = 3;
        d->dim_kind[2] = YAEP_DIM_INTEGER;
    }
    d->model = model;
    d->target_p = 0.5;
    d->n_init = 12;
    d->fit = true;
    d->fit_every = 20;
    d->refine_steps = 2;
    d->stop_trials = 120;
}

/* Threshold RMSE over the 15 cells after a session, or -1 if a proposal or a
 * stored trial was not an integer or level where the kinds say it must be. */
static double mixed_session(bool kinds, yaep_model model, int stream) {
    yaep_desc d;
    yaep_gp g;
    double se = 0.0;
    int bad = 0, nh = 0;
    const yaep_trial* h;
    mixed_desc(&d, kinds, model);
    rng_state = 0x243F6A8885A308D3ull + (uint64_t)stream * 0x9E3779B97F4A7C15ull;
    if (!yaep_open(&g, &d)) {
        CHECK(0, "mixed open: %s", yaep_error(&g));
        return -1.0;
    }
    while (!yaep_done(&g)) {
        double x[3] = { 0.0 };
        yaep_next(&g, x);
        if (kinds && (x[1] != floor(x[1]) || x[2] != floor(x[2]))) bad++;
        yaep_update(&g, x, mixed_observer(x));
    }
    h = yaep_history(&g, &nh);
    for (int i = 0; i < nh && kinds; i++)
        if (h[i].x[1] != floor(h[i].x[1]) || h[i].x[2] != floor(h[i].x[2])) bad++;
    for (int c = 0; c < 3; c++)
        for (int k = 0; k < 5; k++) {
            double ctx[2], t = 0.0;
            ctx[0] = (double)c; ctx[1] = (double)k;
            if (yaep_threshold(&g, ctx, 0.0, &t, NULL, NULL) != YAEP_OK) t = 1.0;
            se += (t - mixed_thr(c, k)) * (t - mixed_thr(c, k));
        }
    yaep_close(&g);
    return bad ? -1.0 : sqrt(se / 15.0);
}

static void test_mixed(void) {
    yaep_desc d;
    yaep_gp g;
    printf("mixed parameter kinds:\n");

    /* 1. The kernel: a categorical pair is os exp(-(1 - delta) / l), an
     * integer coordinate counts as its nearest integer. */
    mixed_desc(&d, true, YAEP_MODEL_GP);
    d.hyper.lengthscale[0] = 0.2;
    d.hyper.lengthscale[1] = 0.7;
    d.hyper.lengthscale[2] = 1.5;
    d.hyper.outputscale = 1.3;
    CHECK(yaep_open(&g, &d), "mixed open: %s", yaep_error(&g));
    {
        double a[3] = { 0.3, 0.0, 1.0 }, b[3] = { 0.5, 2.0, 3.0 }, b2[3] = { 0.5, 1.8, 3.4 };
        double want = 1.3 * exp(-0.5 * (1.0 + 4.0 / 2.25)) * exp(-1.0 / 0.7);
        CLOSE(yaep__kernel(&g, a, b), want, 1e-15, "categorical and integer kernel");
        CLOSE(yaep__kernel(&g, a, b2), yaep__kernel(&g, a, b), 0.0,
              "the kernel on the rounded values");
        b[1] = 0.0;
        CLOSE(yaep__kernel(&g, a, b), 1.3 * exp(-0.5 * (1.0 + 4.0 / 2.25)), 1e-15,
              "same level");
    }
    {
        double x[3] = { 0.4, 1.2, 2.6 };
        CHECK(yaep_update(&g, x, 1) == YAEP_OK, "mixed update");
        CHECK(yaep_history(&g, NULL)[0].x[1] == 1.0 &&
              yaep_history(&g, NULL)[0].x[2] == 3.0,
              "an update stores the level and the integer shown");
    }
    yaep_close(&g);

    /* 2. Descriptions the kinds rule out. */
    mixed_desc(&d, true, YAEP_MODEL_GP);
    d.dim_kind[0] = YAEP_DIM_INTEGER;
    CHECK(!yaep_open(&g, &d), "an INTEGER intensity is refused");
    mixed_desc(&d, true, YAEP_MODEL_GP);
    d.hi[1] = 3.0;
    CHECK(!yaep_open(&g, &d), "a CATEGORICAL box other than [0, levels - 1] is refused");
    mixed_desc(&d, true, YAEP_MODEL_GP);
    d.hi[2] = 4.5;
    CHECK(!yaep_open(&g, &d), "an INTEGER box with a fractional edge is refused");
    mixed_desc(&d, true, YAEP_MODEL_GP);
    d.grid[2] = 7;
    CHECK(!yaep_open(&g, &d), "an INTEGER grid finer than the integers is refused");
    mixed_desc(&d, true, YAEP_MODEL_GP);
    d.grid[1] = 0;
    CHECK(yaep_open(&g, &d) && yaep_n_candidates(&g) == 21 * 3 * 5,
          "a CATEGORICAL grid of 0 enumerates the levels");
    yaep_close(&g);
    mixed_desc(&d, true, YAEP_MODEL_GP);
    memset(d.grid, 0, sizeof(d.grid));
    d.n_candidates = 256;
    CHECK(yaep_open(&g, &d), "mixed Halton open: %s", yaep_error(&g));
    {
        int bad = 0, seen[3] = { 0, 0, 0 };
        for (int j = 0; j < yaep_n_candidates(&g); j++) {
            double c[3] = { 0.0 };
            yaep_candidate(&g, j, c);
            if (c[1] != floor(c[1]) || c[2] != floor(c[2]) || c[1] < 0.0 || c[1] > 2.0 ||
                c[2] < 0.0 || c[2] > 4.0) bad++;
            else seen[(int)c[1]]++;
        }
        CHECK(bad == 0 && seen[0] > 60 && seen[1] > 60 && seen[2] > 60,
              "Halton candidates on the levels and integers (%d bad, %d %d %d per level)",
              bad, seen[0], seen[1], seen[2]);
    }
    yaep_close(&g);

    /* 3. The analytic gradient against differences, GP (RBF and SEMIP) and
     * psychometric, on a categorical and an integer dimension. */
    for (int arm = 0; arm < 3; arm++) {
        yaep__param p[YAEP__NTHETA];
        double th[YAEP__NTHETA], ga[YAEP__NTHETA], v0 = 0.0, vp = 0.0, vm = 0.0, worst = 0.0;
        int np;
        mixed_desc(&d, true, arm == 2 ? YAEP_MODEL_PSYCHOMETRIC : YAEP_MODEL_GP);
        if (arm == 1) d.kernel = YAEP_KERNEL_SEMIP;
        d.stop_trials = 60;
        rng_state = 0x243F6A8885A308D3ull + 0x3141ull * (uint64_t)(arm + 1);
        CHECK(yaep_open(&g, &d), "mixed gradient open: %s", yaep_error(&g));
        if (arm == 2) g.ps_tol = real_is_double ? 1e-13 : 1e-6;
        for (int i = 0; i < 60; i++) {
            double x[3] = { 0.0 };
            x[0] = rng_u();
            x[1] = floor(rng_u() * 3.0);
            x[2] = floor(rng_u() * 5.0);
            yaep_update(&g, x, mixed_observer(x));
        }
        np = yaep__params(&g, p);
        yaep__theta_get(&g, p, np, th);
        CHECK(yaep__fit_eval(&g, p, np, th, &v0, ga) == YAEP_OK, "mixed fit_eval");
        for (int i = 0; i < np; i++) {
            double step = (real_is_double ? 1e-5 : 3e-3) * (1.0 + fabs(th[i]));
            double save = th[i], gn, rel;
            if (p[i].kind != YAEP__P_LS && p[i].kind != YAEP__P_LSB &&
                p[i].kind != YAEP__P_LSG) continue;
            th[i] = save + step;
            yaep__fit_eval(&g, p, np, th, &vp, NULL);
            th[i] = save - step;
            yaep__fit_eval(&g, p, np, th, &vm, NULL);
            th[i] = save;
            gn = (vp - vm) / (2.0 * step);
            rel = fabs(ga[i] - gn) / (fabs(gn) > 1e-6 ? fabs(gn) : 1e-6);
            if (rel > worst) worst = rel;
        }
        track(&worst_grad, worst);
        CHECK(worst < (real_is_double ? 1e-4 : 6e-2),
              "mixed gradient arm %d: lengthscale relative error %.3g", arm, worst);
        printf("  %s lengthscale gradient vs differences %.1e\n",
               arm == 0 ? "GP/RBF" : arm == 1 ? "GP/SEMIP" : "psychometric", worst);
        yaep_close(&g);
    }

    /* 4. Sessions: every proposal lands on a level and an integer, and the
     * kinds are measured against the same field with both contexts declared
     * CONTINUOUS. */
    {
        static const char* const names[3] = { "GP, kinds", "GP, all continuous",
                                               "psychometric, kinds" };
        double rm[3] = { 0.0, 0.0, 0.0 };
        int streams = 4;
        for (int s = 0; s < streams; s++) {
            double r0 = mixed_session(true, YAEP_MODEL_GP, s);
            double r1 = mixed_session(false, YAEP_MODEL_GP, s);
            double r2 = mixed_session(true, YAEP_MODEL_PSYCHOMETRIC, s);
            CHECK(r0 >= 0.0 && r2 >= 0.0, "stream %d: a proposal off the levels or integers", s);
            rm[0] += r0 / streams;
            rm[1] += r1 / streams;
            rm[2] += r2 / streams;
        }
        CHECK(rm[0] < 0.06 && rm[2] < 0.06, "mixed threshold RMSE %.3f / %.3f", rm[0], rm[2]);
        for (int a = 0; a < 3; a++)
            printf("  %-20s threshold RMSE over 15 cells after 120 trials, %d streams: %.4f\n",
                   names[a], streams, rm[a]);
    }
}

/* --- monotonic projection ---------------------------------------------- */

/* A 2-D field whose true p rises along x[1] except for a dip, so an unprojected
 * posterior mean has somewhere to decrease. */
static int mono_observer(const double* x) {
    double f = 3.0 * (x[1] - 0.45) - 1.2 * exp(-((x[1] - 0.6) * (x[1] - 0.6)) / 0.005) +
               0.5 * x[0];
    return rng_u() < 0.5 * erfc(-f / sqrt(2.0)) ? 1 : 0;
}

static void mono_desc(yaep_desc* d, yaep_acq acq, bool grid) {
    memset(d, 0, sizeof(*d));
    d->n_dims = 2;
    d->lo[0] = 0.0; d->hi[0] = 1.0;
    d->lo[1] = 0.0; d->hi[1] = 1.0;
    d->intensity_dim = 1;
    if (grid) { d->grid[0] = 9; d->grid[1] = 21; }
    else d->n_candidates = 200;
    d->acq = acq;
    d->target_p = 0.6;
    d->hyper.lengthscale[0] = 0.4;
    d->hyper.lengthscale[1] = 0.06;
    d->monotone_dims = 1u << 1;
    d->stop_trials = 80;
}

static void test_monotone(void) {
    yaep_desc d;
    yaep_gp g;
    yaep__scr s;
    double worst_p = 0.0, worst_c = 0.0, worst_t = 0.0;
    int drops_on = 0, drops_off = 0, pdrops_on = 0, fdrops_on = 0;
    printf("monotonic projection:\n");

    for (int grid = 1; grid >= 0; grid--) {
        mono_desc(&d, YAEP_ACQ_LSE, grid != 0);
        rng_state = 0x243F6A8885A308D3ull + 0x6D6Full + (uint64_t)grid;
        CHECK(yaep_open(&g, &d), "mono open: %s", yaep_error(&g));
        for (int i = 0; i < 60; i++) {
            double x[2] = { 0.0 };
            x[0] = rng_u();
            x[1] = 0.3 + 0.5 * rng_u();
            yaep_update(&g, x, mono_observer(x));
        }
        yaep__scr_of(&g, &s);
        /* 1. predict_p is the link of the largest unprojected mean on the
         * line below x, against a brute force through predict_f. */
        for (int r = 0; r < 20; r++) {
            double x[2], y[2], mu, sd, best, want;
            int n = yaep__line_n(&g, 1);
            x[0] = rng_u();
            x[1] = rng_u();
            yaep_predict_f(&g, x, 0, &best, &sd);
            for (int k = 0; k < n; k++) {
                y[0] = x[0];
                y[1] = (double)k / (double)(n - 1);
                if (!(y[1] < x[1])) continue;
                yaep_predict_f(&g, y, 0, &mu, NULL);
                if (mu > best) best = mu;
            }
            want = yaep__q_smooth(&g, &s, best, sd, 0.0);
            track(&worst_p, fabs(yaep_predict_p(&g, x) - want));
            {
                double pm = 0.0;
                yaep_predict_p_many(&g, x, 1, &pm);
                track(&worst_p, fabs(pm - want));
            }
        }
        /* 2. The level-set acquisition's candidate means are the same
         * projection, on the grid by a running maximum. */
        {
            double x[2] = { 0.0 };
            yaep_next(&g, x);
            for (int j = 0; j < g.M; j++) {
                double mu = 0.0, sd = 0.0;
                const double* c = yaep__cand(&g, j);
                yaep_predict_f(&g, c, 0, &mu, &sd);
                track(&worst_c, fabs(g.cand_mu[j] - yaep__mono_mu(&g, &s, c, mu, s.t1)));
            }
        }
        /* 3. The projected mean never decreases on the candidate line, and
         * the threshold sits at the target on the projected curve. Between
         * line points it can, and E[p] can anywhere, which is what a
         * projection of the mean and not a constraint means; the counts are
         * printed. */
        for (int c = 0; c < 5; c++) {
            double xs[2 * 101], p[101], ctx = 0.25 * c, thr;
            for (int k = 0; k <= 100; k++) { xs[2 * k] = ctx; xs[2 * k + 1] = 0.01 * k; }
            yaep_predict_p_many(&g, xs, 101, p);
            for (int k = 1; k <= 100; k++) if (p[k] < p[k - 1] - 1e-12) pdrops_on++;
            for (int k = 0; k <= 100; k++) {
                double sd = 0.0, other = 0.0;
                yaep__target_latent_p(&g, &s, xs + 2 * k, &p[k], &sd, &other, s.t1, s.t2);
            }
            for (int k = 1; k <= 100; k++) if (p[k] < p[k - 1] - 1e-12) fdrops_on++;
            {
                /* On the line's own points the projection is a running
                 * maximum, so it cannot step down there. */
                int nl = yaep__line_n(&g, 1);
                double prev = -HUGE_VAL;
                for (int k = 0; k < nl; k++) {
                    double pt[2], mu, sd, other;
                    pt[0] = ctx;
                    pt[1] = (double)k / (double)(nl - 1);
                    yaep__target_latent_p(&g, &s, pt, &mu, &sd, &other, s.t1, s.t2);
                    if (mu < prev - 1e-12) drops_on++;
                    prev = mu;
                }
            }
            if (yaep_threshold(&g, &ctx, 0.0, &thr, NULL, NULL) == YAEP_OK) {
                double pt[2] = { 0.0 };
                pt[0] = ctx; pt[1] = thr;
                track(&worst_t, fabs(yaep_predict_p(&g, pt) - 0.6));
            }
        }
        yaep_close(&g);
        d.monotone_dims = 0u;
        rng_state = 0x243F6A8885A308D3ull + 0x6D6Full + (uint64_t)grid;
        CHECK(yaep_open(&g, &d), "mono open: %s", yaep_error(&g));
        for (int i = 0; i < 60; i++) {
            double x[2] = { 0.0 };
            x[0] = rng_u();
            x[1] = 0.3 + 0.5 * rng_u();
            yaep_update(&g, x, mono_observer(x));
        }
        for (int c = 0; c < 5; c++) {
            double xs[2 * 101] = { 0.0 }, p[101] = { 0.0 };
            for (int k = 0; k <= 100; k++) { xs[2 * k] = 0.25 * c; xs[2 * k + 1] = 0.01 * k; }
            for (int k = 0; k <= 100; k++) {
                double sd = 0.0;
                yaep_predict_f(&g, xs + 2 * k, 0, &p[k], &sd);
            }
            for (int k = 1; k <= 100; k++) if (p[k] < p[k - 1] - 1e-12) drops_off++;
        }
        yaep_close(&g);
    }
    CHECK(worst_p < 1e-12, "projected predict_p vs brute force %.2e", worst_p);
    CHECK(worst_c < 1e-12, "projected candidate means vs point by point %.2e", worst_c);
    CHECK(drops_on == 0, "the projected mean decreased %d times on the candidate lines",
          drops_on);
    CHECK(drops_off > 0, "the unprojected mean never dipped, so the test tests nothing");
    CHECK(worst_t < 1e-6, "p at the projected threshold %.2e from the target", worst_t);
    printf("  predict_p vs brute force %.1e, candidate means %.1e, p at the threshold %.1e;\n"
           "  steps down along 10 lines of 100: latent mean %d projected, %d unprojected;\n"
           "  E[p] with the projected mean %d (its variance still varies along the line)\n",
           worst_p, worst_c, worst_t, fdrops_on, drops_off, pdrops_on);

    /* 4. Descriptions the projection rules out. */
    mono_desc(&d, YAEP_ACQ_LSE, true);
    d.model = YAEP_MODEL_PSYCHOMETRIC;
    CHECK(!yaep_open(&g, &d), "monotone_dims with the psychometric model is refused");
    mono_desc(&d, YAEP_ACQ_LSE, true);
    d.monotone_dims = 1u << 2;
    CHECK(!yaep_open(&g, &d), "monotone_dims past n_dims is refused");
    mono_desc(&d, YAEP_ACQ_LSE, true);
    d.dim_kind[0] = YAEP_DIM_CATEGORICAL;
    d.dim_levels[0] = 2;
    d.monotone_dims = 1u;
    CHECK(!yaep_open(&g, &d), "a monotone CATEGORICAL dimension is refused");

    /* 5. A session under each level-set acquisition. */
    {
        static const yaep_acq acqs[3] = { YAEP_ACQ_LSE, YAEP_ACQ_EAVC, YAEP_ACQ_LOCALMI };
        for (int a = 0; a < 3; a++) {
            int n = 0;
            mono_desc(&d, acqs[a], true);
            d.fit = true;
            d.fit_every = 20;
            d.refine_steps = 2;
            memset(&d.hyper, 0, sizeof(d.hyper));
            rng_state = 0x243F6A8885A308D3ull + 0x77ull * (uint64_t)(a + 1);
            CHECK(yaep_open(&g, &d), "mono session open: %s", yaep_error(&g));
            while (!yaep_done(&g)) {
                double x[2] = { 0.0 };
                if (yaep_next(&g, x) < -1) break;
                if (yaep_update(&g, x, mono_observer(x)) != YAEP_OK) break;
                n++;
            }
            CHECK(n == 80, "mono session %d ran %d trials", a, n);
            yaep_close(&g);
        }
    }
}

/* --- conjugate-gradient Newton steps ------------------------------------ */

/* The same trials through two handles, one factoring every Newton step and one
 * solving them by preconditioned conjugate gradients from trial 17 on: the
 * modes, the log marginals and the predictions have to agree to the Newton
 * tolerance, and the second handle has to have taken the path. */
static void test_pcg(void) {
    static const yaep_lik liks[3] = { YAEP_LIK_BERNOULLI, YAEP_LIK_ORDINAL,
                                       YAEP_LIK_PAIRWISE };
    static const char* const names[3] = { "BERNOULLI", "ORDINAL", "PAIRWISE" };
    printf("conjugate-gradient Newton steps:\n");
    for (int l = 0; l < 3; l++) {
        yaep_desc d;
        yaep_gp ga, gb;
        double worst_f = 0.0, pcg_lm = 0.0, worst_p = 0.0;
        int iters = 0, updates = 0, n = 200;
        memset(&d, 0, sizeof(d));
        d.n_dims = 2;
        d.lo[0] = 0.0; d.hi[0] = 1.0; d.lo[1] = 0.0; d.hi[1] = 1.0;
        d.lik = liks[l];
        d.n_outcomes = liks[l] == YAEP_LIK_ORDINAL ? 4 : 0;
        d.acq = liks[l] == YAEP_LIK_PAIRWISE ? YAEP_ACQ_BALD : YAEP_ACQ_LSE;
        d.target_p = 0.5;
        d.grid[0] = 9; d.grid[1] = 9;
        d.fit = true;
        d.fit_every = 50;
        d.stop_trials = n;
        d.pcg_threshold = -1;
        CHECK(yaep_open(&ga, &d), "pcg open: %s", yaep_error(&ga));
        d.pcg_threshold = 16;
        CHECK(yaep_open(&gb, &d), "pcg open: %s", yaep_error(&gb));
        rng_state = 0x243F6A8885A308D3ull + 0x9C9ull * (uint64_t)(l + 1);
        for (int i = 0; i < n; i++) {
            double x[2], x2[2], f;
            int y;
            x[0] = rng_u(); x[1] = rng_u();
            x2[0] = rng_u(); x2[1] = rng_u();
            f = 4.0 * (x[0] + x[1] - 1.0) + 2.0 * sin(6.0 * x[0]);
            if (liks[l] == YAEP_LIK_PAIRWISE) {
                f -= 4.0 * (x2[0] + x2[1] - 1.0) + 2.0 * sin(6.0 * x2[0]);
                y = rng_u() < 0.5 * erfc(-f / sqrt(2.0)) ? 1 : 0;
                yaep_update_pair(&ga, x, x2, y);
                yaep_update_pair(&gb, x, x2, y);
            } else {
                double z = f + (rng_u() - 0.5) * 3.0;
                y = liks[l] == YAEP_LIK_ORDINAL
                    ? (z < -1.5 ? 0 : z < 0.0 ? 1 : z < 1.5 ? 2 : 3) : (z > 0.0 ? 1 : 0);
                yaep_update(&ga, x, y);
                yaep_update(&gb, x, y);
            }
            if (gb.pcg_iters > 0) { iters += gb.pcg_iters; updates++; }
            for (int k = 0; k < gb.N; k++)
                track(&worst_f, fabs(ga.f[k] - gb.f[k]) / (1.0 + fabs(ga.f[k])));
            track(&pcg_lm, fabs(yaep_log_marginal(&ga) - yaep_log_marginal(&gb)) /
                             (1.0 + fabs(yaep_log_marginal(&ga))));
        }
        for (int j = 0; j < yaep_n_candidates(&ga); j++) {
            double c[2], ma, mb, sa, sb;
            yaep_candidate(&ga, j, c);
            yaep_predict_f(&ga, c, 0, &ma, &sa);
            yaep_predict_f(&gb, c, 0, &mb, &sb);
            track(&worst_p, fabs(ma - mb) + fabs(sa - sb));
        }
        CHECK(updates > 150, "%s: %d updates took the conjugate-gradient path", names[l], updates);
        CHECK(worst_f < (real_is_double ? 1e-8 : 1e-3), "%s: modes differ by %.2e", names[l], worst_f);
        CHECK(pcg_lm < (real_is_double ? 1e-8 : 1e-3), "%s: log marginals differ by %.2e",
              names[l], pcg_lm);
        CHECK(worst_p < (real_is_double ? 1e-8 : 1e-3), "%s: predictions differ by %.2e",
              names[l], worst_p);
        printf("  %-9s %3d updates by conjugate gradients, %.1f iterations each; against\n"
               "            factoring: mode %.1e, log marginal %.1e, prediction %.1e\n",
               names[l], updates, updates ? (double)iters / updates : 0.0, worst_f, pcg_lm,
               worst_p);
        yaep_close(&ga);
        yaep_close(&gb);
    }
}

/* --- snapshots ---------------------------------------------------------- */

/* A session cut at a trial, saved, loaded into a handle filled with garbage,
 * and finished, against the same session uninterrupted: every proposal and the
 * final snapshot itself have to agree to the bit. The outcomes are a hash of
 * the trial index and the stimulus, so both runs see the same responses to the
 * same stimuli without sharing a generator; the desc's generator state is the
 * caller's, carried across the cut as the manual says to. */
typedef struct snap_gen { uint64_t s; } snap_gen;

static uint64_t snap_mix(uint64_t z) {
    z += 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

static double snap_rng(void* ctx) {
    snap_gen* gen = (snap_gen*)ctx;
    gen->s = snap_mix(gen->s);
    return (double)(gen->s >> 11) * (1.0 / 9007199254740992.0);
}

static double snap_u(int n, const double* x, int nd) {
    uint64_t h = (uint64_t)n * 0x9E3779B97F4A7C15ull;
    for (int i = 0; i < nd; i++) {
        uint64_t b;
        memcpy(&b, &x[i], sizeof(b));
        h = snap_mix(h ^ b);
    }
    return (double)(snap_mix(h) >> 11) * (1.0 / 9007199254740992.0);
}

typedef struct snap_cfg {
    const char* name;
    int trials;
    void (*setup)(yaep_desc* d);
} snap_cfg;

static void snap_base(yaep_desc* d) {
    memset(d, 0, sizeof(*d));
    d->n_dims = 2;
    d->lo[0] = 0.0; d->hi[0] = 1.0; d->lo[1] = 0.0; d->hi[1] = 1.0;
    d->intensity_dim = 1;
    d->target_p = 0.6;
    d->grid[0] = 7; d->grid[1] = 11;
    d->fit = true;
    d->fit_every = 8;
    d->n_init = 6;
    d->stop_trials = 400;
}
static void snap_lse(yaep_desc* d) { snap_base(d); d->refine_steps = 2; }
static void snap_eavc(yaep_desc* d) {
    snap_base(d);
    d->acq = YAEP_ACQ_EAVC;
    memset(d->grid, 0, sizeof(d->grid));
    d->n_candidates = 48;
}
static void snap_ps(yaep_desc* d) {
    snap_base(d);
    d->model = YAEP_MODEL_PSYCHOMETRIC;
    d->acq = YAEP_ACQ_BALD;
}
static void snap_ps_eavc(yaep_desc* d) {
    snap_base(d);
    d->model = YAEP_MODEL_PSYCHOMETRIC;
    d->acq = YAEP_ACQ_EAVC;
}
static void snap_cat(yaep_desc* d) {
    snap_base(d);
    d->lik = YAEP_LIK_CATEGORICAL;
    d->n_outcomes = 3;
    d->fit_every = 12;
}
static void snap_ord(yaep_desc* d) {
    snap_base(d);
    d->lik = YAEP_LIK_ORDINAL;
    d->n_outcomes = 3;
    d->refit_every = 4;
    d->acq = YAEP_ACQ_BALV;
}
static void snap_gauss(yaep_desc* d) {
    snap_base(d);
    d->lik = YAEP_LIK_GAUSSIAN;
    d->acq = YAEP_ACQ_UCB;
}
static void snap_pair(yaep_desc* d) {
    snap_base(d);
    d->lik = YAEP_LIK_PAIRWISE;
    d->acq = YAEP_ACQ_THOMPSON;
}
static void snap_ps_pcg(yaep_desc* d) {
    snap_ps(d);
    d->fit_pcg = true;
}
static void snap_lse_pcg(yaep_desc* d) {
    snap_lse(d);
    d->fit_pcg = true;
}
static void snap_mixed(yaep_desc* d) {
    snap_base(d);
    d->n_dims = 3;
    d->lo[2] = 0.0; d->hi[2] = 2.0;
    d->dim_kind[2] = YAEP_DIM_CATEGORICAL;
    d->dim_levels[2] = 3;
    d->monotone_dims = 1u << 1;
    d->pcg_threshold = 10;
    d->refine_steps = 1;
}

/* One session; at trial `cut` (after the proposal when `pending`) it is saved,
 * the handle closed and filled with garbage, and loaded back. Writes the
 * proposals to xs and returns the final snapshot, malloc'd, in *fin. */
static int snap_session(const snap_cfg* c, int cut, bool pending, double* xs,
                        unsigned char** fin, size_t* fin_len) {
    yaep_desc d;
    yaep_gp g;
    snap_gen gen;
    int nd, loaded = 0;
    c->setup(&d);
    gen.s = 0x5EEDull;
    /* Without a generator the init points are the header's Halton counter,
     * which is state the snapshot has to carry; with one, they are draws. */
    if (d.acq == YAEP_ACQ_THOMPSON || d.acq == YAEP_ACQ_EAVC || d.lik == YAEP_LIK_ORDINAL) {
        d.rng = snap_rng;
        d.rng_ctx = &gen;
    }
    nd = d.n_dims;
    if (!yaep_open(&g, &d)) { CHECK(0, "%s: open: %s", c->name, yaep_error(&g)); return 0; }
    for (int n = 0; n < c->trials; n++) {
        double x[YAEP_MAX_DIMS] = { 0.0 }, x2[YAEP_MAX_DIMS] = { 0.0 };
        bool pair = d.lik == YAEP_LIK_PAIRWISE;
        for (int phase = 0; phase < 2; phase++) {
            if (n == cut && (phase == 1) == pending) {
                size_t len = yaep_save_size(&g);
                unsigned char* buf = (unsigned char*)malloc(len);
                int wrote = yaep_save(&g, buf, len);
                CHECK(wrote == (int)len, "%s: save wrote %d of %lu", c->name, wrote,
                      (unsigned long)len);
                yaep_close(&g);
                memset(&g, 0xA5, sizeof(g));
                loaded = yaep_open_saved(&g, &d, buf, len);
                CHECK(loaded, "%s: load at %d: %s", c->name, cut, yaep_error(&g));
                free(buf);
                if (!loaded) return 0;
            }
            if (phase == 0) {
                if (pair) yaep_next_pair(&g, x, x2);
                else yaep_next(&g, x);
            }
        }
        memcpy(xs + (size_t)n * 2 * YAEP_MAX_DIMS, x, sizeof(x));
        if (pair) {
            double u1 = x[0] - 0.4 * x[1], u2 = x2[0] - 0.4 * x2[1];
            memcpy(xs + (size_t)n * 2 * YAEP_MAX_DIMS + YAEP_MAX_DIMS, x2, sizeof(x2));
            yaep_update_pair(&g, x, x2, snap_u(n, x, nd) < 0.5 * erfc(-(u1 - u2) * 3.0 / sqrt(2.0)));
        } else {
            double f = 6.0 * (x[1] - 0.3 - 0.3 * x[0]);
            double u = snap_u(n, x, nd);
            if (d.lik == YAEP_LIK_GAUSSIAN) {
                yaep_update_real(&g, x, 0.5 * f + (u - 0.5));
            } else {
                double p = 0.5 * erfc(-f / sqrt(2.0));
                int y = u < p ? 1 : 0;
                if (d.lik == YAEP_LIK_CATEGORICAL) y = u < p * 0.6 ? 1 : (u < p ? 2 : 0);
                if (d.lik == YAEP_LIK_ORDINAL) y = u < p * p ? 2 : (u < p ? 1 : 0);
                yaep_update(&g, x, y);
            }
        }
        if (n % 7 == 3) yaep_fit_step(&g);   /* a stepped fit is part of the state */
    }
    *fin_len = yaep_save_size(&g);
    *fin = (unsigned char*)malloc(*fin_len);
    yaep_save(&g, *fin, *fin_len);
    yaep_close(&g);
    return 1;
}

static void test_snapshot(void) {
    static const snap_cfg cfgs[] = {
        { "GP LSE refined",        40, snap_lse },
        { "GP EAVC",               30, snap_eavc },
        { "psychometric BALD",     30, snap_ps },
        { "psychometric EAVC",     24, snap_ps_eavc },
        { "CATEGORICAL",           30, snap_cat },
        { "ORDINAL refit_every 4", 30, snap_ord },
        { "GAUSSIAN UCB",          30, snap_gauss },
        { "PAIRWISE Thompson",     30, snap_pair },
        { "mixed, monotone, PCG",  30, snap_mixed },
        { "GP LSE, fit_pcg",       40, snap_lse_pcg },
        { "psychometric, fit_pcg", 30, snap_ps_pcg }
    };
    int ncfg = (int)(sizeof(cfgs) / sizeof(cfgs[0])), resumes = 0, mismatches = 0;
    size_t biggest = 0;
    printf("snapshots:\n");
    for (int c = 0; c < ncfg; c++) {
        int T = cfgs[c].trials;
        double* ref = (double*)calloc((size_t)T * 2 * YAEP_MAX_DIMS, sizeof(double));
        double* got = (double*)calloc((size_t)T * 2 * YAEP_MAX_DIMS, sizeof(double));
        unsigned char *fref = NULL, *fgot = NULL;
        size_t lref = 0, lgot = 0;
        const int cuts[6] = { 0, 1, 5, 9, 17, 23 };
        if (!real_is_double && cfgs[c].setup == snap_gauss) {   /* double only */
            free(ref);
            free(got);
            continue;
        }
        if (!snap_session(&cfgs[c], -1, false, ref, &fref, &lref)) continue;
        if (lref > biggest) biggest = lref;
        for (int k = 0; k < 6; k++) {
            for (int pend = 0; pend < 2; pend++) {
                bool same;
                if (cuts[k] >= T) continue;
                memset(got, 0, (size_t)T * 2 * YAEP_MAX_DIMS * sizeof(double));
                if (!snap_session(&cfgs[c], cuts[k], pend != 0, got, &fgot, &lgot)) continue;
                same = memcmp(ref, got, (size_t)T * 2 * YAEP_MAX_DIMS * sizeof(double)) == 0 &&
                       lgot == lref && memcmp(fref, fgot, lref) == 0;
                CHECK(same, "%s: resumed at trial %d%s differs from the uninterrupted run",
                      cfgs[c].name, cuts[k], pend ? " (proposal pending)" : "");
                resumes++;
                if (!same) mismatches++;
                free(fgot);
            }
        }
        free(fref);
        free(ref);
        free(got);
    }
    printf("  %d resumes over %d configurations, %d differ from the uninterrupted run\n"
           "  (proposals and the final snapshot compared byte for byte); largest\n"
           "  final snapshot %lu bytes\n", resumes, ncfg, mismatches, (unsigned long)biggest);

    /* A snapshot the desc does not match, a truncated one, and a foreign one. */
    {
        yaep_desc d;
        yaep_gp g;
        unsigned char* buf;
        size_t len;
        snap_lse(&d);
        CHECK(yaep_open(&g, &d), "snapshot open");
        for (int n = 0; n < 10; n++) {
            double x[2] = { 0.0 };
            yaep_next(&g, x);
            yaep_update(&g, x, x[1] > 0.5);
        }
        len = yaep_save_size(&g);
        buf = (unsigned char*)malloc(len);
        CHECK(yaep_save(&g, buf, len - 1) == YAEP_ERR_ARG, "save into a short buffer");
        yaep_save(&g, buf, len);
        yaep_close(&g);
        d.target_p = 0.7;
        CHECK(!yaep_open_saved(&g, &d, buf, len) && strstr(yaep_error(&g), "target_p") &&
              !yaep_is_open(&g), "a desc that differs is named: %s", yaep_error(&g));
        d.target_p = 0.6;
        CHECK(!yaep_open_saved(&g, &d, buf, len - 3), "a truncated snapshot is refused");
        buf[2] = 'T';
        CHECK(!yaep_open_saved(&g, &d, buf, len), "a foreign snapshot is refused");
        buf[2] = 'G';
        CHECK(yaep_open_saved(&g, &d, buf, len), "the snapshot itself loads: %s", yaep_error(&g));
        yaep_close(&g);
        free(buf);
    }
}

/* --- the fit's budget and tolerance -------------------------------------- */

/* yaep_fit() says whether it converged, and repeating it until
 * yaep_fit_delta() is small reaches what one fit with a large budget reaches. */
static void test_fit_budget(void) {
    yaep_desc d;
    yaep_gp ga, gb;
    int rounds = 0, rc = 0;
    double la = 0.0, lb = 0.0;
    printf("fit budget and tolerance:\n");
    memset(&d, 0, sizeof(d));
    d.n_dims = 2;
    d.lo[0] = 0.0; d.hi[0] = 1.0; d.lo[1] = 0.0; d.hi[1] = 1.0;
    d.target_p = 0.5;
    d.grid[0] = 9; d.grid[1] = 9;
    d.stop_trials = 400;
    d.fit_max_evals = 4;
    CHECK(yaep_open(&ga, &d), "budget open: %s", yaep_error(&ga));
    CHECK(yaep_fit(&ga) == 1, "a fit with nothing to fit to has converged");
    d.fit_max_evals = 400;
    d.fit_tol = 1e-6;
    CHECK(yaep_open(&gb, &d), "budget open: %s", yaep_error(&gb));
    rng_state = 0x243F6A8885A308D3ull + 0xF17ull;
    for (int i = 0; i < 120; i++) {
        double x[2], f;
        int y;
        x[0] = rng_u(); x[1] = rng_u();
        f = 3.0 * (x[1] - 0.5) + 1.5 * sin(5.0 * x[0]);
        y = rng_u() < 0.5 * erfc(-f / sqrt(2.0)) ? 1 : 0;
        yaep_update(&ga, x, y);
        yaep_update(&gb, x, y);
    }
    rc = yaep_fit(&ga);
    CHECK(rc == 0, "four evaluations do not converge this fit (returned %d)", rc);
    CHECK(yaep_fit_delta(&ga) > 0.0, "the budgeted fit moved the objective by %g",
          yaep_fit_delta(&ga));
    while (rc == 0 && ++rounds < 200) {
        rc = yaep_fit(&ga);
        if (yaep_fit_delta(&ga) < 1e-7) break;
    }
    CHECK(rc >= 0, "a repeated fit failed: %s", yaep_strerror(rc));
    rc = yaep_fit(&gb);
    CHECK(rc == 1, "one fit with a budget of 400 converged (returned %d)", rc);
    la = yaep_log_marginal(&ga);
    lb = yaep_log_marginal(&gb);
    CHECK(fabs(la - lb) < 1e-5, "repeated fits reach %.6f, one long fit %.6f", la, lb);
    printf("  a 4-evaluation fit repeated %d times reaches log marginal %.6f,\n"
           "  one fit of up to 400 evaluations to tolerance 1e-6 %.6f\n",
           rounds + 1, la, lb);
    yaep_close(&ga);
    yaep_close(&gb);
}

/* --- regression: a floor that swallows the mean (csf6) ------------------- */

/* The first 30 trials of replication 4 of the 6-D contrast sensitivity problem
 * in tests/compare/methods_compare.py (Sobol stimuli, responses from its seeded
 * stream), with guess = 0.5. Without a prior on the mean, the first fit (trial
 * 10, five yes) sends the mean to its bound, where p is the floor everywhere,
 * the log marginal is exactly 10 ln 0.5 and the gradient in the mean vanishes;
 * v0.4.1 stayed there for all 300 trials. The mean prior of v0.5.0 is what
 * keeps it out: the test runs both, so the mechanism stays documented. */
static const double csf6_rep4[30][7] = {
    { -1.2766851712949574, -0.96831516036763787, 7.7132341824471951, 5.4078615987673402, 1.9480876373127103, 0.45676186680793762, 0 },
    { -0.070414334535598755, -0.1850840044207871, 11.233065500855446, 0.73709239112213254, 7.8522888738662004, 8.5433981753885746, 1 },
    { -0.37912975950166583, -1.1581477909348905, 4.7950835525989532, 4.9327587159350514, 6.5891689984127879, 5.6521382182836533, 0 },
    { -0.87494994979351759, -0.74698125896975398, 16.255717556923628, 3.1033528414554894, 3.4922555424273014, 2.5669422931969166, 0 },
    { -0.94977311231195927, -1.3380071274004877, 14.800013843923807, 2.2973774624988437, 4.7801763797178864, 1.4997863862663507, 1 },
    { -0.70311243692412972, -0.55403783870860934, 6.2507878616452217, 4.1265789554454386, 7.3140470068901777, 9.6634560357779264, 1 },
    { -0.34752122405916452, -0.77746636653319001, 17.708302848041058, 1.5433653285726905, 9.4213911136612296, 7.3983318451792002, 1 },
    { -1.3983796848915517, -0.36557312356308103, 1.2379962392151356, 6.2139421650208533, 2.9541469141840935, 4.2338334303349257, 0 },
    { -1.4105143952183425, -1.254501226823777, 19.616173300892115, 4.3655000049620867, 8.4859298076480627, 3.304549315944314, 0 },
    { -0.21892792638391256, -0.63752411445602775, 1.7494169250130653, 2.534515134524554, 1.4551063040271401, 5.2197058033198118, 1 },
    { -0.62065950455144048, -1.0719813820905983, 12.87489976733923, 6.7940532248467207, 3.8435652107000351, 8.1091337744146585, 1 },
    { -1.1017981879413128, -0.071220962796360254, 5.7611884362995625, 2.1248660641722381, 5.8160194670781493, 1.1961984913796186, 1 },
    { -0.8159481855109334, -0.88480634288862348, 2.8797345422208309, 1.3060961663722992, 6.8226532731205225, 4.7382918000221252, 1 },
    { -0.55458309268578887, -0.26856738561764359, 15.756354257464409, 5.9750789939425886, 5.4121605129912496, 6.5740504302084446, 1 },
    { -0.10597287677228451, -1.4526598225347698, 9.6113371849060059, 3.3533841781318188, 2.1804215125739574, 8.8397858291864395, 1 },
    { -1.1715386160649359, -0.45262609189376235, 11.754252444952726, 5.1841767258010805, 9.7732063783332705, 2.0036359317600727, 1 },
    { -1.1861754627898335, -1.4790091523900628, 2.1822295151650906, 1.3921318626962602, 3.6344077838584781, 6.5432037506252527, 1 },
    { -0.18510826444253325, -0.41452229674905539, 19.26651157438755, 6.4731229804456234, 6.1658010892570019, 4.6299592684954405, 1 },
    { -0.48723476193845272, -0.91700498946011066, 5.3285272419452667, 2.4547493844293058, 8.2734949560835958, 2.3634716216474771, 1 },
    { -0.78957005823031068, -0.22459415718913078, 13.224715273827314, 3.8739302009344101, 1.8081658873707056, 9.2783090751618147, 1 },
    { -1.0403089332394302, -1.1100508477538824, 15.323691908270121, 4.6799613940529525, 3.0959902321919799, 5.4037314653396606, 0 },
    { -0.58842099364846945, -0.044837303459644318, 3.2295512035489082, 3.2609725128859282, 8.9982634633779526, 3.5672819055616856, 1 },
    { -0.23942444147542119, -1.2984172375872731, 12.187063805758953, 5.6667942772619426, 7.734944031573832, 0.67708022892475128, 1 },
    { -1.483791422098875, -0.60526825021952391, 9.2616766877472401, 0.58600788004696369, 4.6404950227588415, 7.5125484354794025, 1 },
    { -1.3310310635715723, -0.83901915326714516, 10.176474843174219, 3.6060311892069876, 5.6744164749979973, 9.7131852805614471, 1 },
    { -0.32114209095016122, -0.31579778529703617, 8.6866739764809608, 5.026806547306478, 4.2663838705047965, 1.6282988525927067, 1 },
    { -0.71774957422167063, -1.4054063698276877, 17.312159202992916, 1.0469139949418604, 1.0344496052712202, 3.8929568976163864, 0 },
    { -1.0289095058105886, -0.49839020613580942, 3.8214875943958759, 6.1263138158246875, 8.625302174128592, 6.9799119420349598, 1 },
    { -0.89544617431238294, -1.2079803524538875, 7.3072283528745174, 6.9451394793577492, 9.6319124437868595, 8.3392688725143671, 0 },
    { -0.45240578055381775, -0.68548569548875093, 13.826419040560722, 1.865931642241776, 2.6029997793957591, 0.17507041804492474, 1 }
};

static void test_regression_csf6(void) {
    static const double lo[6] = { -1.5, -1.5, 0.0, 0.5, 1.0, 0.0 };
    static const double hi[6] = { 0.0, 0.0, 20.0, 7.0, 10.0, 10.0 };
    printf("regression: a 0.5 floor and the mean (csf6 replication 4):\n");
    for (int prior = 1; prior >= 0; prior--) {
        yaep_desc d;
        yaep_gp g;
        memset(&d, 0, sizeof(d));
        d.n_dims = 6;
        for (int i = 0; i < 6; i++) { d.lo[i] = lo[i]; d.hi[i] = hi[i]; }
        d.target_p = 0.75;
        d.n_candidates = 1000;
        d.n_init = 10;
        d.fit = true;
        d.fit_every = 20;
        d.stop_trials = 300;
        d.max_trials = 300;
        d.guess = 0.5;
        d.no_hyper_prior = !prior;
        CHECK(yaep_open(&g, &d), "csf6 open: %s", yaep_error(&g));
        for (int n = 0; n < 30; n++) {
            yaep_update(&g, csf6_rep4[n], (int)csf6_rep4[n][6]);
            if (n + 1 == 10 || n + 1 == 30) {
                yaep_hyper h;
                memset(&h, 0, sizeof(h));
                double pmin = 1.0, pmax = 0.0, flat = (n + 1) * log(0.5);
                yaep_get_hyper(&g, &h);
                for (int j = 0; j < 200; j++) {
                    double c[6], p;
                    yaep_candidate(&g, j, c);
                    p = yaep_predict_p(&g, c);
                    if (p < pmin) pmin = p;
                    if (p > pmax) pmax = p;
                }
                if (prior) {
                    /* At trial 10 the floor is the likelihood's own maximum
                     * (five of ten yes at a 0.5 floor), so only the prior
                     * keeps the model off it; by trial 30 the data leave it. */
                    CHECK(h.mean > -2.0 && pmax - pmin > 0.03 &&
                          (n + 1 == 10 || yaep_log_marginal(&g) > flat + 0.5),
                          "trial %d: the mean %.3f, p %.3f to %.3f, log marginal %.3f (floor %.3f)",
                          n + 1, h.mean, pmin, pmax, yaep_log_marginal(&g), flat);
                } else if (n + 1 == 10) {
                    CHECK(pmax - pmin < 0.01 && fabs(yaep_log_marginal(&g) - flat) < 0.01,
                          "without the prior the stuck state did not form (mean %.3f)", h.mean);
                }
                printf("  %s trial %2d: mean %6.3f, p %.3f to %.3f, log marginal %7.3f (floor %7.3f)\n",
                       prior ? "prior   " : "no prior", n + 1, h.mean, pmin, pmax,
                       yaep_log_marginal(&g), flat);
            }
        }
        yaep_close(&g);
    }
}

/* --- conjugate-gradient fits (desc.fit_pcg) ---------------------------- */

/* The same trials through two handles, one fitting by factorizations and one
 * with desc.fit_pcg, each fitted to convergence: the two fits have to reach
 * the same hyperparameters, log marginal and predictions to well inside the
 * fit's own tolerance. */
/* p, or under PAIRWISE (which has no p at a point) the utility's mean. */
static double fit_pcg_q(const yaep_gp* g, const double* x) {
    double mu = 0.0, sd;
    if (g->desc.lik != YAEP_LIK_PAIRWISE) return yaep_predict_p(g, x);
    yaep_predict_f(g, x, 0, &mu, &sd);
    return mu;
}

static void test_fit_pcg(void) {
    static const char* const names[4] = { "BERNOULLI", "ORDINAL", "PAIRWISE", "psychometric" };
    printf("desc.fit_pcg:\n");
    for (int c = 0; c < 4; c++) {
        yaep_desc d;
        yaep_gp ga, gb;
        yaep_hyper ha, hb;
        memset(&ha, 0, sizeof(ha));
        memset(&hb, 0, sizeof(hb));
        double dh = 0.0, dp = 0.0, dl;
        int n = c == 3 ? 120 : 150, ra = 0, rb = 0;
        memset(&d, 0, sizeof(d));
        d.n_dims = 2;
        d.lo[0] = 0.0; d.hi[0] = 1.0; d.lo[1] = 0.0; d.hi[1] = 1.0;
        d.intensity_dim = 1;
        d.lik = c == 1 ? YAEP_LIK_ORDINAL : c == 2 ? YAEP_LIK_PAIRWISE : YAEP_LIK_BERNOULLI;
        d.n_outcomes = c == 1 ? 3 : 0;
        d.model = c == 3 ? YAEP_MODEL_PSYCHOMETRIC : YAEP_MODEL_GP;
        d.acq = c == 2 ? YAEP_ACQ_BALD : YAEP_ACQ_LSE;
        d.target_p = 0.6;
        d.grid[0] = 9; d.grid[1] = 9;
        d.stop_trials = n;
        d.fit_max_evals = 400;
        d.fit_tol = 1e-7;
        CHECK(yaep_open(&ga, &d), "fit_pcg open: %s", yaep_error(&ga));
        d.fit_pcg = true;
        CHECK(yaep_open(&gb, &d), "fit_pcg open: %s", yaep_error(&gb));
        rng_state = 0x243F6A8885A308D3ull + 0xFC6ull * (uint64_t)(c + 1);
        for (int i = 0; i < n; i++) {
            double x[2], x2[2], f;
            int y;
            x[0] = rng_u(); x[1] = rng_u();
            x2[0] = rng_u(); x2[1] = rng_u();
            f = 5.0 * (x[1] - 0.4 - 0.3 * x[0] * x[0]);
            if (c == 2) {
                f = 2.0 * (sin(4.0 * x[0]) + x[1] - sin(4.0 * x2[0]) - x2[1]);
                y = rng_u() < 0.5 * erfc(-f / sqrt(2.0)) ? 1 : 0;
                yaep_update_pair(&ga, x, x2, y);
                yaep_update_pair(&gb, x, x2, y);
            } else {
                double u = rng_u(), p = 0.5 * erfc(-f / sqrt(2.0));
                y = c == 1 ? (u < p * p ? 2 : u < p ? 1 : 0) : (u < p ? 1 : 0);
                yaep_update(&ga, x, y);
                yaep_update(&gb, x, y);
            }
        }
        {
            /* One stepped fit in lockstep: after every step, rejected ones
             * included, the two handles have to stand at the same point, so
             * the state a rejection restores is checked, not only where the
             * fit ends. */
            int sa = 1, sb = 1, steps = 0;
            while (sa > 0 && sb > 0 && steps < 200) {
                double pt[2] = { 0.3, 0.6 };
                sa = yaep_fit_step(&ga);
                sb = yaep_fit_step(&gb);
                steps++;
                track(&dp, fabs(fit_pcg_q(&ga, pt) - fit_pcg_q(&gb, pt)));
            }
            CHECK(sa == sb, "%s: the stepped fits ended apart (%d, %d) after %d steps",
                  names[c], sa, sb, steps);
        }
        for (int r = 0; r < 50 && (ra = yaep_fit(&ga)) == 0; r++) { }
        for (int r = 0; r < 50 && (rb = yaep_fit(&gb)) == 0; r++) { }
        CHECK(ra == 1 && rb == 1, "%s: fits ended %d and %d", names[c], ra, rb);
        memset(&ha, 0, sizeof(ha));
        memset(&hb, 0, sizeof(hb));
        yaep_get_hyper(&ga, &ha);
        yaep_get_hyper(&gb, &hb);
        for (int i = 0; i < 2; i++) {
            track(&dh, fabs(log(ha.lengthscale[i] / hb.lengthscale[i])));
            if (c == 3) track(&dh, fabs(log(ha.lengthscale_g[i] / hb.lengthscale_g[i])));
        }
        track(&dh, fabs(log(ha.outputscale / hb.outputscale)));
        track(&dh, fabs(ha.mean - hb.mean));
        dl = fabs(yaep_log_marginal(&ga) - yaep_log_marginal(&gb));
        for (int j = 0; j < yaep_n_candidates(&ga); j++) {
            double x[2] = { 0.0 };
            yaep_candidate(&ga, j, x);
            track(&dp, fabs(fit_pcg_q(&ga, x) - fit_pcg_q(&gb, x)));
        }
        CHECK(dl < (real_is_double ? 1e-6 : 1e-4) && dh < 1e-3 && dp < 1e-4,
              "%s: fit_pcg against factoring: log marginal %.2e, hyperparameters %.2e, p %.2e",
              names[c], dl, dh, dp);
        printf("  %-12s against factoring: log marginal %.1e, hyperparameters %.1e (log), "
               "p %.1e%s\n", names[c], dl, dh, dp,
               c == 3 ? "" : "");
        yaep_close(&ga);
        yaep_close(&gb);
    }
}

/* --- caller memory at any alignment ------------------------------------- */

/* desc.memory need not be 8-byte aligned: yaep_open() aligns the base and
 * yaep_memory_size() asks for the slack. Before v0.14.1 the scratch pointers
 * of every later call were laid out from the unaligned address, so a buffer
 * on an odd address read the quadrature weights a few bytes off (on a macOS
 * build: outcome probabilities summing to 8e280). The same session through a
 * malloc'd handle and through a static buffer at each offset 0..7 has to agree
 * to the bit, and predict_outcomes() has to write every one of its K slots. */
static void test_memory_alignment(void) {
    static unsigned char buf[(1 << 20) + 16];
    static const yaep_lik liks[3] = { YAEP_LIK_BERNOULLI, YAEP_LIK_ORDINAL,
                                       YAEP_LIK_CATEGORICAL };
    int bad = 0, unwritten = 0;
    printf("caller memory at every alignment:\n");
    for (int l = 0; l < 3; l++) {
        double ref_lm = 0.0, ref_p[YAEP_MAX_OUTCOMES] = { 0.0 }, ref_x[40] = { 0.0 };
        int K = l == 0 ? 2 : l == 1 ? 4 : 3;
        for (int off = -1; off < 8; off++) {
            yaep_desc d;
            yaep_gp g;
            double p[YAEP_MAX_OUTCOMES + 1], xq[1] = { 0.45 }, sum = 0.0;
            memset(&d, 0, sizeof(d));
            d.n_dims = 1;
            d.lo[0] = 0.0; d.hi[0] = 1.0;
            d.lik = liks[l];
            d.n_outcomes = l == 0 ? 0 : K;
            d.link = YAEP_LINK_LOGIT;
            d.target_p = 0.6;
            d.grid[0] = 11;
            d.n_init = 4;
            d.fit = true;
            d.fit_every = 10;
            d.stop_trials = 40;
            d.max_trials = 40;
            if (off >= 0) {
                CHECK(yaep_memory_size(&d) + 8 <= sizeof(buf), "alignment buffer too small");
                d.memory = buf + off;
                d.memory_size = sizeof(buf) - 8;
            }
            CHECK(yaep_open(&g, &d), "alignment open: %s", yaep_error(&g));
            rng_state = 0x243F6A8885A308D3ull + 0xA116ull * (uint64_t)(l + 1);
            for (int i = 0; i < 40; i++) {
                double x[1] = { 0.0 };
                yaep_next(&g, x);
                if (off < 0) ref_x[i] = x[0];
                else if (x[0] != ref_x[i]) bad++;
                yaep_update(&g, x, (int)(rng_u() * (double)K));
            }
            /* NaN in every slot, and one past the end, so a slot the call
             * leaves unwritten shows. */
            for (int k = 0; k <= YAEP_MAX_OUTCOMES; k++) p[k] = (double)NAN;
            CHECK(yaep_predict_outcomes(&g, xq, p) == YAEP_OK, "alignment predict_outcomes");
            for (int k = 0; k < K; k++) {
                if (!(p[k] == p[k])) unwritten++;
                sum += p[k];
            }
            if (p[K] == p[K]) unwritten++;   /* written past K */
            CLOSE(sum, 1.0, 1e-12, "lik %d offset %d: outcome probabilities sum to 1", l, off);
            if (off < 0) {
                ref_lm = yaep_log_marginal(&g);
                memcpy(ref_p, p, sizeof(ref_p));
            } else {
                if (yaep_log_marginal(&g) != ref_lm) bad++;
                if (memcmp(ref_p, p, (size_t)K * sizeof(double)) != 0) bad++;
            }
            yaep_close(&g);
        }
    }
    CHECK(bad == 0, "%d differences between malloc'd and caller memory at some offset", bad);
    CHECK(unwritten == 0, "predict_outcomes left %d slots unwritten or wrote past K", unwritten);
    printf("  BERNOULLI, ORDINAL (4), CATEGORICAL (3): malloc and offsets 0..7 agree to the bit,\n"
           "  every outcome slot written (%d differences, %d unwritten)\n", bad, unwritten);
}

/* --- parameter table (v0.16.0) ------------------------------------------- */

#include "param_table_check.h"

static void ptc_fail(int line, const char* what) {
    printf("FAIL %s:%d: %s\n", __FILE__, line, what);
    yaep_test_fails++;
}

#define PF(name, member, kind, n) { name, offsetof(yaep_desc, member), kind, n, NULL }
#define PE(name, member, kind, n, v) { name, offsetof(yaep_desc, member), kind, n, v }
#define PH(h, m, kind) PF(#h "." #m, h.m, kind, 0)
#define PP(p) PF("priors." #p ".center", priors.p.center, 'd', 0), \
              PF("priors." #p ".sd", priors.p.sd, 'd', 0)
static const ptc_field g_afields[] = {
    PF("n_dims", n_dims, 'i', 0), PF("lo", lo, 'D', 0), PF("hi", hi, 'D', 0),
    PF("intensity_dim", intensity_dim, 'i', 0),
    PE("lik", lik, 'e', YAEP_LIK_PAIRWISE + 1, "bernoulli=0,ordinal=1,categorical=2,gaussian=3,pairwise=4"),
    PF("n_outcomes", n_outcomes, 'i', 0),
    PE("kernel", kernel, 'e', YAEP_KERNEL_SEMIP + 1, "rbf=0,semip=1"),
    PE("link", link, 'e', YAEP_LINK_LOGIT + 1, "probit=0,logit=1"),
    PF("guess", guess, 'd', 0), PF("lapse", lapse, 'd', 0),
#define PHYP(h) PH(h, lengthscale, 'D'), PH(h, outputscale, 'd'), PH(h, mean, 'd'),           \
    PH(h, lengthscale_b, 'D'), PH(h, outputscale_b, 'd'), PH(h, cutpoint, 'D'),                \
    PH(h, noise_sd, 'd'), PH(h, lengthscale_g, 'D'), PH(h, outputscale_g, 'd'), PH(h, mean_g, 'd')
    PHYP(hyper),
    PF("fit", fit, 'b', 0), PF("fit_every", fit_every, 'i', 0),
    PF("no_hyper_prior", no_hyper_prior, 'b', 0), PF("refit_every", refit_every, 'i', 0),
    PHYP(hyper_min), PHYP(hyper_max),
#undef PHYP
    PF("jitter", jitter, 'd', 0),
    PE("acq", acq, 'e', YAEP_ACQ_THOMPSON + 1,
       "lse=0,eavc=1,localmi=2,balv=3,bald=4,random=5,ucb=6,ei=7,thompson=8"),
    PF("target_p", target_p, 'd', 0), PF("target_value", target_value, 'd', 0),
    PF("target_outcome", target_outcome, 'i', 0), PF("acq_beta", acq_beta, 'd', 0),
    PF("n_init", n_init, 'i', 0), PF("candidates", candidates, 'D', 0),
    PF("n_candidates", n_candidates, 'i', 0), PF("grid", grid, 'I', 0),
    PF("stop_trials", stop_trials, 'i', 0), PF("stop_threshold_sd", stop_threshold_sd, 'd', 0),
    PF("stop_context", stop_context, 'D', 0), PF("max_trials", max_trials, 'i', 0),
    PF("refine_steps", refine_steps, 'i', 0),
    PE("model", model, 'e', YAEP_MODEL_PSYCHOMETRIC + 1, "gp=0,psychometric=1"),
    PP(lengthscale), PP(outputscale), PP(outputscale_b), PP(outputscale_g), PP(mean), PP(mean_g),
    PF("priors.mean_g.ceiling", priors.mean_g.ceiling, 'd', 0), PP(noise_sd),
    PF("minimize", minimize, 'b', 0),
    PE("dim_kind", dim_kind, 'E', YAEP_DIM_CATEGORICAL + 1, "continuous=0,integer=1,categorical=2"),
    PF("dim_levels", dim_levels, 'I', 0),
    PF("monotone_dims", monotone_dims, 'u', 0), PF("pcg_threshold", pcg_threshold, 'i', 0),
    PF("fit_max_evals", fit_max_evals, 'i', 0), PF("fit_tol", fit_tol, 'd', 0),
    PF("fit_pcg", fit_pcg, 'b', 0),
};
#undef PF
#undef PE
#undef PH
#undef PP
#define NAF ((int)(sizeof g_afields / sizeof g_afields[0]))

/* The contexts a row's default is checked in: the default (the GP model,
 * BERNOULLI, LSE) and one per likelihood, kernel and model a row can be
 * about, each with desc.fit off and on. */
enum { PCX_GP, PCX_UCB, PCX_GAUSS, PCX_ORD, PCX_SEMIP, PCX_PSY, PCX_N };

static void pcx_desc(yaep_desc* d, int cx, bool fit) {
    memset(d, 0, sizeof *d);
    d->n_dims = 2;
    d->lo[0] = 0.0; d->hi[0] = 1.0;
    d->lo[1] = -1.0; d->hi[1] = 1.0;
    d->target_p = 0.7;
    d->stop_trials = 10;
    d->stop_threshold_sd = 1e-9;   /* either stop alone is enough, so zero each */
    d->n_candidates = 64;
    d->fit = fit;
    d->fit_every = fit ? 3 : 0;
    switch (cx) {
    case PCX_UCB:   d->acq = YAEP_ACQ_UCB; break;
    case PCX_GAUSS: d->lik = YAEP_LIK_GAUSSIAN; d->target_value = 0.4; break;
    case PCX_ORD:   d->lik = YAEP_LIK_ORDINAL; d->n_outcomes = 3; break;
    case PCX_SEMIP: d->kernel = YAEP_KERNEL_SEMIP; break;
    case PCX_PSY:   d->model = YAEP_MODEL_PSYCHOMETRIC; break;
    default: break;
    }
}

/* Ten trials of a fixed observer; on success g stays open and *len is the
 * snapshot's length, the state in out[*start..*len). */
static bool pcx_run(yaep_gp* g, const yaep_desc* d, unsigned char* out, size_t cap,
                    int* start, int* len) {
    double x[YAEP_MAX_DIMS];
    yaep__w w;
    int t;
    if (!yaep_open(g, d)) return false;
    for (t = 0; t < 10; t++) {
        double v;
        if (yaep_next(g, x) < -1) break;
        v = x[0] - 0.4 - 0.2 * x[1] + ((t % 4 == 1) ? 0.3 : 0.0);
        if (d->lik == YAEP_LIK_GAUSSIAN) yaep_update_real(g, x, v);
        else if (d->lik == YAEP_LIK_ORDINAL) yaep_update(g, x, v < 0.0 ? 0 : v < 0.25 ? 1 : 2);
        else yaep_update(g, x, v > 0.0);
    }
    memset(&w, 0, sizeof w);
    yaep__put_desc(&w, &g->desc);
    *start = 9 + (int)w.pos;
    *len = yaep_save(g, out, cap);
    return *len > *start;
}

/* The fit's bounds, which only show in the state when a fit reaches one. */
static int pcx_bounds(const yaep_gp* g, double* lohi) {
    yaep__param p[YAEP__NTHETA];
    int np = yaep__params(g, p), i;
    for (i = 0; i < np; i++) { lohi[2 * i] = p[i].lo; lohi[2 * i + 1] = p[i].hi; }
    return np;
}

static unsigned char g_pa[1 << 20], g_pb[1 << 20];

static void test_params(void) {
    static ptc_row rows[160];
    static yaep_gp ga, gb;
    const yaep_param* p;
    yaep_desc base, d;
    int n = 0, i, cx, fit, compared = 0;
    char msg[512];
    p = yaep_params(&n);
    CHECK(p && n == NAF && n <= 160, "yaep_params: %d rows, the test knows %d", n, NAF);
    if (!p || n > 160) return;
    for (i = 0; i < n; i++) PTC_ROW(rows[i], p[i]);
    ptc_check_cover(rows, n, g_afields, NAF);
    CHECK(yaep_params(NULL) == p, "the table is static");

    /* Structure, enums refused one past their last value, and the zero of
     * every scalar either opens or is documented as required, in the
     * default context. */
    pcx_desc(&base, PCX_GP, false);
    for (i = 0; i < n; i++) {
        int k = ptc_check_row(&rows[i], g_afields, NAF);
        const ptc_field* f;
        if (k < 0) continue;
        f = &g_afields[k];
        if (f->kind == 'e') {
            d = base;
            ptc_set(&d, f, rows[i].max + 1.0);
            CHECK(!yaep_open(&ga, &d), "%s = %g opened", f->name, rows[i].max + 1.0);
            yaep_close(&ga);
        }
        if (f->kind == 'E') {
            d = base;
            d.dim_kind[1] = (yaep_dim_kind)(int)(rows[i].max + 1.0);
            CHECK(!yaep_open(&ga, &d), "dim_kind past its values opened");
            yaep_close(&ga);
        }
        if (!ptc_scalar(f)) continue;
        d = base;
        ptc_set(&d, f, 0.0);
        if (!yaep_open(&ga, &d))
            CHECK(ptc_required(&rows[i]), "zero %s refused (%s), its doc does not say required",
                  f->name, yaep_error(&ga));
        yaep_close(&ga);
    }

    /* def is what a zero means: a zero field and the field at def give the
     * same session, state byte for byte, the same resolved priors and the
     * same fit bounds, in every context the row's doc does not name as an
     * exception ("(psychometric: ...)") and every context it is about (a doc
     * that starts "psychometric:", "gaussian:" or "ordinal:"). A hyper row is
     * a fixed value only with the fit off, and its zero is a fit's free
     * parameter, so its bounds list differs by design. */
    for (cx = 0; cx < PCX_N; cx++) {
        for (fit = 0; fit < 2; fit++) {
            pcx_desc(&base, cx, fit != 0);
            for (i = 0; i < n; i++) {
                const ptc_field* f;
                int k = ptc_find(g_afields, NAF, rows[i].name), sa, la, sb, lb, na, nb;
                double ba[2 * YAEP__NTHETA], bb[2 * YAEP__NTHETA];
                bool hyp;
                const char* doc = rows[i].doc;
                if (k < 0 || !ptc_scalar(&g_afields[k]) || rows[i].def == 0.0) continue;
                f = &g_afields[k];
                hyp = strncmp(f->name, "hyper.", 6) == 0;
                if (fit && hyp) continue;
                if (cx == PCX_PSY && strstr(doc, "(psychometric:")) continue;
                if ((strncmp(doc, "psychometric:", 13) == 0 && cx != PCX_PSY) ||
                    (strncmp(doc, "gaussian:", 9) == 0 && cx != PCX_GAUSS) ||
                    (strncmp(doc, "ordinal:", 8) == 0 && cx != PCX_ORD))
                    continue;
                d = base;
                ptc_set(&d, f, 0.0);
                if (!pcx_run(&ga, &d, g_pa, sizeof g_pa, &sa, &la)) { yaep_close(&ga); continue; }
                ptc_set(&d, f, rows[i].def);
                if (!pcx_run(&gb, &d, g_pb, sizeof g_pb, &sb, &lb)) {
                    snprintf(msg, sizeof msg, "context %d fit %d: %s = def does not run: %s",
                             cx, fit, f->name, yaep_error(&gb));
                    ptc_fail(__LINE__, msg);
                } else {
                    compared++;
                    na = pcx_bounds(&ga, ba);
                    nb = pcx_bounds(&gb, bb);
                    if (hyp) na = nb = 0;
                    if (la - sa != lb - sb || memcmp(g_pa + sa, g_pb + sb, (size_t)(la - sa)) != 0 ||
                        memcmp(&ga.priors, &gb.priors, sizeof ga.priors) != 0 || na != nb ||
                        memcmp(ba, bb, sizeof(double) * 2 * (size_t)na) != 0) {
                        snprintf(msg, sizeof msg, "context %d fit %d: %s = 0 and %s = %g differ",
                                 cx, fit, f->name, f->name, rows[i].def);
                        ptc_fail(__LINE__, msg);
                    }
                }
                yaep_close(&ga);
                yaep_close(&gb);
            }
        }
    }
    CHECK(compared > 250, "only %d default comparisons ran", compared);

    /* The rows whose zero is a rule, not a number: the doc's rule, checked
     * against the handle. */
    pcx_desc(&base, PCX_GP, false);
    CHECK(yaep_open(&ga, &base), "open: %s", yaep_error(&ga));
    CLOSE(ga.hyper.lengthscale[0], 0.25, 0.0, "hyper.lengthscale 0 = a quarter of the range");
    CLOSE(ga.hyper.lengthscale[1], 0.5, 0.0, "hyper.lengthscale 0 = a quarter of the range");
    CHECK(yaep__n_init(&ga) == 2 * 2 + 2, "n_init 0 = 2 n_dims + 2");
    yaep_close(&ga);
    pcx_desc(&base, PCX_GP, true);
    CHECK(yaep_open(&ga, &base), "open: %s", yaep_error(&ga));
    {
        double lohi[2 * YAEP__NTHETA];
        int np = pcx_bounds(&ga, lohi);
        CHECK(np >= 2, "two lengthscales fitted");
        CLOSE(lohi[0], log(0.05 * 1.0), 1e-15, "hyper_min.lengthscale 0 = 0.05 range");
        CLOSE(lohi[1], log(2.0 * 1.0), 1e-15, "hyper_max.lengthscale 0 = 2 ranges");
        CLOSE(lohi[3], log(2.0 * 2.0), 1e-15, "hyper_max.lengthscale 0 = 2 ranges, dim 1");
    }
    yaep_close(&ga);
    pcx_desc(&base, PCX_ORD, false);
    {
        int sa, la, sb, lb;
        CHECK(pcx_run(&ga, &base, g_pa, sizeof g_pa, &sa, &la), "ordinal run");
        base.target_outcome = 1;
        CHECK(pcx_run(&gb, &base, g_pb, sizeof g_pb, &sb, &lb), "ordinal run");
        CHECK(la - sa == lb - sb && memcmp(g_pa + sa, g_pb + sb, (size_t)(la - sa)) == 0,
              "ordinal target_outcome 0 = 1");
        yaep_close(&ga);
        yaep_close(&gb);
    }
    printf("  parameter table: %d rows, %d default comparisons\n", n, compared);
}
#undef NAF

/* v0.16.0: a snapshot whose discrete outcome is not one of the outcomes is
 * refused; one read back used to index the cutpoints with it (found by
 * tests/fuzz/adapt_fuzz.c). The bad value is put in the handle and saved,
 * since the writer copies whatever the handle holds. */
static void test_snapshot_outcomes(void) {
    static const double bad[] = { -1.0, 3.0, 0.5, 1e300, -2147483648.0 };
    static yaep_gp g, h;
    yaep_desc d;
    int sa, la, i, which;
    pcx_desc(&d, PCX_ORD, false);
    for (which = 0; which < 2; which++)
        for (i = 0; i < (int)(sizeof bad / sizeof bad[0]); i++) {
            CHECK(pcx_run(&g, &d, g_pa, sizeof g_pa, &sa, &la), "ordinal run");
            CHECK(yaep_open_saved(&h, &d, g_pa, (size_t)la), "a good snapshot: %s", yaep_error(&h));
            yaep_close(&h);
            if (which == 0) g.y[3] = bad[i];
            else g.history[3].y = bad[i];
            la = yaep_save(&g, g_pa, sizeof g_pa);
            CHECK(!yaep_open_saved(&h, &d, g_pa, (size_t)la) && !yaep_is_open(&h) &&
                  strstr(yaep_error(&h), "corrupt") != NULL,
                  "outcome %g in %s loaded", bad[i], which ? "the history" : "y");
            yaep_close(&g);
        }
    /* GAUSSIAN outcomes are values: any finite one loads, a NaN does not.
     * GAUSSIAN needs the double build. */
    if (!real_is_double) return;
    pcx_desc(&d, PCX_GAUSS, false);
    CHECK(pcx_run(&g, &d, g_pa, sizeof g_pa, &sa, &la), "gaussian run");
    g.y[2] = 1e6;
    g.history[2].y = 1e6;
    la = yaep_save(&g, g_pa, sizeof g_pa);
    CHECK(yaep_open_saved(&h, &d, g_pa, (size_t)la), "a large gaussian outcome: %s", yaep_error(&h));
    yaep_close(&h);
    g.y[2] = nan("");
    la = yaep_save(&g, g_pa, sizeof g_pa);
    CHECK(!yaep_open_saved(&h, &d, g_pa, (size_t)la), "a NaN gaussian outcome loaded");
    yaep_close(&g);
    yaep_close(&h);
}

int main(void) {
    /* YAEP_TEST_ONLY=params runs the parameter table and snapshot outcome
     * checks alone: seconds instead of minutes, for their mutants
     * (tests/mutate/aep.toml). */
    const char* only = getenv("YAEP_TEST_ONLY");
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("ysp/aep.h tests\n");
    if (only && strcmp(only, "params") == 0) {
        test_version();
        test_params();
        test_snapshot_outcomes();
        if (yaep_test_fails == 0) {
            printf("all ysp/aep.h tests passed\n");
            return 0;
        }
        printf("%d ysp/aep.h check(s) failed\n", yaep_test_fails);
        return 1;
    }
    test_version();
    test_chol();
    test_quad();
    test_halton();
    test_kernel(YAEP_KERNEL_RBF);
    test_kernel(YAEP_KERNEL_SEMIP);
    test_memory();
    test_memory_alignment();
    test_open_reject();
    test_simulate_outcome();
    if (real_is_double) test_numeric_failure();
    test_configurations();
    test_fit_step();
    test_refit_every();
    test_predict_many();
    test_2afc();
    if (real_is_double) test_gaussian_exact();
    test_laplace_probit();
    test_lookahead();
    test_gradient(YAEP_LIK_BERNOULLI, YAEP_KERNEL_RBF, "bernoulli/rbf");
    test_gradient(YAEP_LIK_BERNOULLI, YAEP_KERNEL_SEMIP, "bernoulli/semip");
    test_gradient(YAEP_LIK_ORDINAL, YAEP_KERNEL_RBF, "ordinal/rbf");
    test_gradient(YAEP_LIK_ORDINAL, YAEP_KERNEL_SEMIP, "ordinal/semip");
    if (real_is_double) {
        test_gradient(YAEP_LIK_GAUSSIAN, YAEP_KERNEL_RBF, "gaussian/rbf");
        test_gradient(YAEP_LIK_GAUSSIAN, YAEP_KERNEL_SEMIP, "gaussian/semip");
    }
    test_gradient_logit();
    test_loop_rules();
    printf("simulated 1-D probit observer, 100 trials, 8 response streams:\n");
    {
        double e_lse = mean_error(YAEP_ACQ_LSE, 100, "LSE", 8);
        double e_eavc = mean_error(YAEP_ACQ_EAVC, 100, "EAVC", 8);
        double e_bald = mean_error(YAEP_ACQ_BALD, 100, "BALD", 8);
        printf("  mean absolute error: LSE %.4f, EAVC %.4f, BALD %.4f\n",
               e_lse, e_eavc, e_bald);
        CHECK(e_lse < 0.08, "LSE mean threshold error %.4f", e_lse);
        CHECK(e_eavc < 0.08, "EAVC mean threshold error %.4f", e_eavc);
        CHECK(e_bald < 0.08, "BALD mean threshold error %.4f", e_bald);
    }
    test_refine();
    {
        /* The same eight LSE streams with two rounds of refinement. */
        double e_ref = 0.0;
        obs_refine = 2;
        e_ref = mean_error(YAEP_ACQ_LSE, 100, "LSE+r", 8);
        obs_refine = 0;
        printf("  mean absolute error: LSE with refine_steps = 2 %.4f\n",
               e_ref);
        CHECK(e_ref < 0.08, "refined LSE mean threshold error %.4f", e_ref);
    }
    if (real_is_double) test_gaussian_field();
    test_ordinal_field();
    test_categorical_field();
    test_fit();
    test_fit_every_and_rng();
    test_psychometric();
    test_regressions();
    test_regression_csf6();
    test_priors();
    test_optimize();
    test_pairwise();
    test_mixed();
    test_monotone();
    test_pcg();
    test_snapshot();
    test_fit_budget();
    test_fit_pcg();
    test_params();
    test_snapshot_outcomes();
#ifdef YAEP_ASYNC
    printf("async layer (YAEP_ASYNC):\n");
    test_async();
    test_async_fit_in_idle();
    test_async_queue();
#endif
    printf("worst observed error, YAEP_REAL = %s (%u bytes):\n",
           real_is_double ? "double" : "float", (unsigned)sizeof(yaep_real));
    printf("  Cholesky and inverse %.2e, Laplace stationarity %.2e\n",
           worst_chol, worst_stat);
    printf("  predictive variance %.2e, log marginal %.2e\n",
           worst_var, worst_lm);
    printf("  look-ahead covariance %.2e, gradient vs differences %.2e (relative)\n",
           worst_look, worst_grad);
    if (yaep_test_fails == 0) {
        printf("all ysp/aep.h tests passed\n");
        return 0;
    }
    printf("%d ysp/aep.h check(s) failed\n", yaep_test_fails);
    return 1;
}
