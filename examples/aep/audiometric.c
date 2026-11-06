/* aep_audiometric - the audiometric test function of Owen et al. 2021 on ysp/aep.h.
 *
 * Owen et al. (2021), "Adaptive nonparametric psychophysics"
 * (arXiv:2104.09549), the AEPsych paper, compares level-set and global
 * acquisition functions on a synthetic audiometric observer: a hearing
 * threshold in dB HL that varies with frequency, and a probit psychometric
 * function of intensity around it. This program runs that comparison on
 * ysp/aep.h's own acquisitions and adds the classical baseline the paper left
 * out, a set of interleaved weighted staircases from ysp/stair.h.
 *
 * THE OBSERVER
 * Four age-related-hearing-loss phenotypes, each given as a hearing threshold
 * at 8 audiometric frequencies (0.25 to 8 kHz). The numbers are the ones Owen
 * et al. scraped from Figure 2 of Dubno, Eckert, Lee, Matthews & Schmiedt
 * (2013), "Classifying human audiometric phenotypes of age-related hearing
 * loss from animal models", JARO 14:687-701; they are that paper's phenotype
 * means, not anything this file invented. Between the 8 points the curve is a
 * not-a-knot cubic spline in kHz (linear frequency, not log), which agrees with
 * SciPy's interp1d(kind='cubic') to the six digits SciPy was asked to print.
 * Beyond them, out to the 0.125 and 16 kHz ends of the box, this file extends
 * the end TANGENT rather than the end cubic; SciPy's fill_value="extrapolate"
 * extends the cubic, which reaches 91 dB HL at 16 kHz on the older-normal
 * phenotype from a 13 dB measurement at 8 kHz.
 *
 * The latent field is f(xc, xi) = (xi - theta(xc)) / beta with xc = log2 of
 * the frequency in kHz and xi the intensity in dB HL, and p = Phi(f). beta is
 * the psychometric width in dB: the whole probit rise takes about 5.2 * beta
 * dB, so beta = 0.2 is a step function on the scale of the box and beta = 10
 * is a ramp across half of it.
 *
 * THE PROTOCOL, as the paper defines it
 * 150 trials: 5 quasi-random init trials (the paper's Sobol, Halton here) then
 * 145 adaptive ones. Target threshold p = 0.75. One replication is one seed;
 * the methods are the paper's LSE, BALV and BALD, this header's EAVC and
 * LOCALMI, and YAEP_ACQ_RANDOM as the paper's quasi-random baseline, each on
 * the RBF probit GP with fitted hyperparameters, plus three of them again on
 * the semiparametric kernel, this header's analog of the paper's
 * linear-additive model, and five more (semip2-lse, -balv, -bald, -eavc and
 * -localmi) on YAEP_MODEL_PSYCHOMETRIC, Keeley et al. 2023's semiparametric
 * model in its own form: a threshold GP and a log-slope GP over frequency and
 * a probit in intensity. lse-mono, eavc-mono and bald-mono are lse, eavc and
 * bald with desc.monotone_dims set along the intensity, the posterior mean
 * projected to rise with level (MONOTONIC PROJECTION in ysp/aep.h).
 *
 * THE METRICS, as the paper defines them
 * On a 30 x 30 grid over the box, (a) the mean absolute error of the model's
 * E[p] against the true p, and (b) the mean absolute error of the 0.75
 * threshold in dB: per context column, the two grid points that bracket 0.75
 * along intensity, linearly interpolated, done identically for the truth and
 * for the model, averaged over the columns where both cross. Columns where one
 * does not cross are counted and reported, not scored. And (c), not the
 * paper's: MAE(p) again over the transition band alone, the grid points whose
 * true p is in [0.05, 0.95], as tests/compare/compare_gp_aepsych.py defines it.
 * The band is a few points per column, so (a) mostly measures the flat floor
 * and ceiling, and (c) is where a model has to represent the rise itself. The
 * CSV carries it as a last column, mae_band.
 *
 * THE CLASSICAL BASELINE
 * Eight interleaved weighted 1-up-1-down staircases, one per measured
 * frequency, round robin, 150 trials in total, 5 dB steps after a 10 dB
 * opening step, starting at 40 dB, threshold from the reversal mean. Scored
 * with the same two metrics by reading its 8 thresholds as a threshold curve
 * (linear in log2 frequency, end segments extended) and its p as
 * Phi((xi - threshold) / beta) with beta KNOWN. That last part is a best case
 * that no staircase gets in a real experiment: it is given the psychometric
 * width for free, while the GP has to learn it.
 *
 * WHAT IT COSTS
 * Measured on one core of an x86-64 desktop under WSL2, gcc -O2, at the end of
 * a 150-trial session on this box with M = 231 candidates: yaep_next() 1.0 to
 * 1.3 ms under LSE and 6.2 to 7.3 ms under EAVC, yaep_update() 0.6 to 0.9 ms,
 * one whole yaep_fit() 170 to 490 ms from cold, and the 900-point metric grid
 * 9.4 ms through yaep_predict_p_many() (10.5 us per point) against 9.7 to
 * 11.0 ms through 900 yaep_predict_p() calls (10.8 to 12.2 us per point). The
 * batched call is what this program scores with, and on this configuration it is
 * worth 10 to 20 percent of the grid rather than the factor of two its manual
 * quotes: 900 points over a 150 x 150 factor is 1.6 MB of reading either way and
 * the one-point path already streams it in order. So the hyperparameter fit is
 * the expensive part of a replication and desc.fit_every is the knob: 20 trials,
 * the default here, is about 7 fits and most of a 150-trial replication.
 * Nothing here is a timing measurement of a rig. The per-trial times in
 * the summary are CPU time from clock() over yaep_next() plus yaep_update()
 * only, so the metric grid and the CSV are not charged to the frame budget. A
 * scheduled hyperparameter fit runs inside yaep_update(), so it is inside that
 * number and not beside it, which is why a method's ms/trial is several times
 * its yaep_next(). The staircase prints 0.000 because 150 of its trials
 * together are under the resolution of clock().
 *
 * Build (from the repository root):
 *     cc -O2 -Iinclude -o aep_audiometric examples/aep/audiometric.c -lm
 *     cl /O2 /Iinclude examples\aep\audiometric.c
 *
 * Usage: aep_audiometric [phenotype] [beta] [reps] [methods] [csv] [fit_every]
 *     aep_audiometric
 *         the no-argument configuration: metabolic+sensory, beta 2, 3
 *         replications, every method, metrics every 10 trials, no CSV, and a
 *         100-trial session rather than 150, and every method but
 *         semip2-eavc, semip2-localmi and the -mono rows. About 8 s at -O2 on
 *         one idle core of an x86-64 desktop under WSL2, and under 20 s
 *         with the machine busy, so CI can run it inside 20 s.
 *     aep_audiometric all 2 20 all out.csv
 *         all four phenotypes at beta = 2, 20 replications, every method,
 *         per-trial curves to out.csv.
 *     aep_audiometric sensory all 5 lse,eavc,stair -
 *         one phenotype over every beta, 5 replications, three methods, no CSV.
 *
 *     phenotype   a name (older-normal, sensory, metabolic, metabolic+sensory),
 *                 an index 0..3, or all
 *     beta        a positive number, or all (0.2, 0.5, 1, 2, 5, 10)
 *     reps        replications per configuration, 1 to 200
 *     methods     a comma-separated list of the names in the table below, or all
 *     csv         a path for the per-trial curves, or - for none
 *     fit_every   trials between hyperparameter fits; 0 fits never
 *
 * Exit code: 0 on a completed run, 2 on a bad argument, 1 on an internal
 * failure (a spline self-check, an allocation, a CSV that will not open).
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
/* fopen() is C4996 under /W4 /WX, and fopen_s() is not portable. */
#define _CRT_SECURE_NO_WARNINGS
#endif

/* Both handles are sized by macros at compile time. 150 trials in one GP
 * session and at most 19 in one staircase, so the defaults (512 and 1024)
 * would be mostly empty history. */
#define YAEP_MAX_TRIALS 160
#define YST_MAX_TRIALS 32
#define YST_MAX_REVERSALS 32

#define YSP_AEP_IMPLEMENTATION
#include "ysp/aep.h"

#define YSP_STAIR_IMPLEMENTATION
#include "ysp/stair.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* --- the test field ---------------------------------------------------- */

#define NF      8        /* audiometric frequencies with measured thresholds */
#define NPHENO  4
#define NGRID   30       /* the paper's metric grid, per dimension           */

#define N_TRIALS  150      /* the paper's session; the config field may shorten it */
#define N_INIT    5
#define TARGET_P  0.75
#define Z75       0.67448975019608171  /* Phi^-1(0.75) */
#define SQRT1_2   0.70710678118654752
#define STAIR_START 40.0

/* The stimulus box: log2 frequency in kHz, and intensity in dB HL. */
#define XC_LO (-3.0)
#define XC_HI ( 4.0)
#define XI_LO (-20.0)
#define XI_HI (120.0)

/* The candidate set: a product grid, 7 dB apart along intensity (Hughson and
 * Westlake's clinical descending step) and 0.7 octaves along frequency, which is
 * fine enough to see the shape of an audiogram. Both counts are overridable at
 * the compiler (-DGRID_C=15 -DGRID_I=25) so the set can be swept.
 * M = GRID_C * GRID_I = 231, and M is what next()
 * costs a kernel row and a triangular solve of, so it is the first knob to
 * turn: EAVC is quadratic in it and measured 22.8 ms per trial at M = 375
 * against 8.8 at M = 231. Coarser was also MORE accurate here, on 3
 * replications: LSE's threshold error at 100 trials was 3.5 dB at M = 231 and
 * 7.3 dB at M = 375, and semip-balv's 1.9 dB against 11.8. A candidate set
 * restricts where the acquisition may put a trial, which is a regularizer on a
 * fit that has no prior; three replications cannot size that effect, only
 * notice it. */
#ifndef GRID_C
#define GRID_C 11
#endif
#ifndef GRID_I
#define GRID_I 21
#endif

/* Every fit bound is ysp/aep.h's default here, which took measuring rather than
 * trusting: the box is 140 dB wide and 7 octaves long, the bounds are the fit's
 * only regularization (it has no hyperparameter prior), and a bound in the
 * wrong place is the difference between a fit that describes 150 trials and one
 * that interpolates them. Zero means "leave the default"; override any of these
 * at the compiler to see what it costs. What a sweep of them on
 * metabolic+sensory found, 3 replications, beta 0.5 and 2, on a finer candidate
 * grid than the one above (15 x 25):
 *   LS_INT_MIN    lower bound on the intensity lengthscale, dB. Default
 *                 0.05 * 140 = 7 dB, which is wider than the whole psychometric
 *                 rise below beta = 1.5; dropping it to 2 dB changed not one
 *                 digit of either metric, because the fit never asks for less.
 *   OS_MAX_RBF    upper bound on the RBF output scale, a prior VARIANCE.
 *                 Default 10. Raising it to 50 costs 0.03 of MAE(p) and stalls
 *                 the surface error after 50 trials: the fit spends the room on
 *                 interpolating, and the log marginal likelihood goes from
 *                 about -24 to about -15 where 150 binary trials at a 75% level
 *                 cannot honestly beat about -84.
 *   OS_MAX_SEMIP, OS_B_MAX_SEMIP  the same two for the semiparametric
 *                 intercept and slope GPs. Raising either (to 400 and 1e4, and
 *                 to 100) moves MAE(p) by 0.002 and MAE(threshold) by 0.02 dB.
 *                 The semiparametric fit is not short of room: with a level-set
 *                 acquisition it separates its own trials, which sends the slope
 *                 output scale to its ceiling and the log marginal likelihood to
 *                 -2.7, and leaves the threshold near the middle of the box. */
#ifndef LS_INT_MIN
#define LS_INT_MIN 0.0
#endif
#ifndef OS_MAX_RBF
#define OS_MAX_RBF 0.0
#endif
#ifndef OS_MAX_SEMIP
#define OS_MAX_SEMIP 0.0
#endif
#ifndef OS_B_MAX_SEMIP
#define OS_B_MAX_SEMIP 0.0
#endif

static const double audio_f[NF] = { 0.25, 0.5, 1.0, 2.0, 3.0, 4.0, 6.0, 8.0 };

static const char* const pheno_name[NPHENO] = {
    "older-normal", "sensory", "metabolic", "metabolic+sensory"
};

/* Dubno et al. 2013 JARO 14:687-701, Fig. 2, as scraped by Owen et al. 2021:
 * mean hearing threshold in dB HL per phenotype at the 8 frequencies above. */
static const double pheno_thr[NPHENO][NF] = {
    {  6.82,  5.49,  3.51,  5.91,  6.70, 10.09, 13.47, 12.97 },
    {  5.52,  4.19,  5.62, 19.84, 42.00, 53.33, 62.05, 66.09 },
    { 21.23, 22.01, 24.24, 33.93, 41.36, 47.17, 54.12, 58.31 },
    { 20.26, 20.71, 21.97, 37.49, 53.18, 64.02, 75.01, 76.61 }
};

static const double all_betas[6] = { 0.2, 0.5, 1.0, 2.0, 5.0, 10.0 };

/* --- a not-a-knot cubic spline ----------------------------------------- */

/* Segment i is a[i] + b[i] t + c[i] t^2 + d[i] t^3 with t = x - x[i]. The last
 * entry holds the value and the slope at the right knot, so the two
 * extrapolation branches are the same polynomial with c and d dropped. */
typedef struct spline {
    int    n;
    double x[NF], a[NF], b[NF], c[NF], d[NF];
} spline;

/* Fit the second derivatives of a not-a-knot spline through n >= 4 knots with
 * strictly increasing x. The end conditions ask for a continuous third
 * derivative across the second and the second-to-last knot, which puts one
 * entry outside the tridiagonal band in each end row; one row operation
 * against the neighboring row clears it, so the solve stays a Thomas sweep
 * and not a dense factorization. */
static void spline_fit(spline* s, const double* x, const double* y, int n) {
    double h[NF], dl[NF], dd[NF], du[NF], r[NF], m[NF];
    int i;

    s->n = n;
    for (i = 0; i < n; i++) s->x[i] = x[i];
    for (i = 0; i < n - 1; i++) h[i] = x[i + 1] - x[i];

    for (i = 1; i < n - 1; i++) {
        dl[i] = h[i - 1];
        dd[i] = 2.0 * (h[i - 1] + h[i]);
        du[i] = h[i];
        r[i]  = 6.0 * ((y[i + 1] - y[i]) / h[i] - (y[i] - y[i - 1]) / h[i - 1]);
    }
    /* Row 0 is h1 m0 - (h0 + h1) m1 + h0 m2 = 0; row 1 carries h1 in the m2
     * column, so h0 / h1 of it takes that column out of row 0. */
    {
        double k = h[0] / h[1];
        dl[0] = 0.0;
        dd[0] = h[1] - k * dl[1];
        du[0] = -(h[0] + h[1]) - k * dd[1];
        r[0]  = -k * r[1];
    }
    {
        double k = h[n - 2] / h[n - 3];
        dl[n - 1] = -(h[n - 3] + h[n - 2]) - k * dd[n - 2];
        dd[n - 1] = h[n - 3] - k * du[n - 2];
        du[n - 1] = 0.0;
        r[n - 1]  = -k * r[n - 2];
    }
    for (i = 1; i < n; i++) {
        double w = dl[i] / dd[i - 1];
        dd[i] -= w * du[i - 1];
        r[i]  -= w * r[i - 1];
    }
    m[n - 1] = r[n - 1] / dd[n - 1];
    for (i = n - 2; i >= 0; i--) m[i] = (r[i] - du[i] * m[i + 1]) / dd[i];

    for (i = 0; i < n - 1; i++) {
        s->a[i] = y[i];
        s->c[i] = 0.5 * m[i];
        s->d[i] = (m[i + 1] - m[i]) / (6.0 * h[i]);
        s->b[i] = (y[i + 1] - y[i]) / h[i] - h[i] * (2.0 * m[i] + m[i + 1]) / 6.0;
    }
    s->a[n - 1] = y[n - 1];
    s->b[n - 1] = s->b[n - 2] + h[n - 2] * (2.0 * s->c[n - 2]
                                            + 3.0 * s->d[n - 2] * h[n - 2]);
    s->c[n - 1] = s->d[n - 1] = 0.0;
}

/* The spline inside [x0, xn-1], and the end segment's tangent line outside it.
 * Extrapolating the end CUBIC instead would put the 16 kHz end of the box a
 * long way from anything the 8 kHz measurement supports. */
static double spline_eval(const spline* s, double x) {
    int i = 0, n = s->n;
    double t;
    if (x <= s->x[0]) return s->a[0] + s->b[0] * (x - s->x[0]);
    if (x >= s->x[n - 1]) return s->a[n - 1] + s->b[n - 1] * (x - s->x[n - 1]);
    while (i < n - 2 && x >= s->x[i + 1]) i++;
    t = x - s->x[i];
    return s->a[i] + t * (s->b[i] + t * (s->c[i] + t * s->d[i]));
}

/* Interpolation of the knots, a continuous second derivative at the interior
 * knots, and a continuous third derivative at the two not-a-knot points. The
 * three properties that say the solve above is the spline it claims to be. */
static int spline_selfcheck(void) {
    int p, i, bad = 0;
    for (p = 0; p < NPHENO; p++) {
        spline s;
        spline_fit(&s, audio_f, pheno_thr[p], NF);
        for (i = 0; i < NF; i++) {
            double e = fabs(spline_eval(&s, audio_f[i]) - pheno_thr[p][i]);
            if (!(e < 1e-9)) {
                printf("spline: knot %d of %s off by %g\n", i, pheno_name[p], e);
                bad++;
            }
        }
        for (i = 1; i < NF - 1; i++) {
            double h = audio_f[i] - audio_f[i - 1];
            double left  = 2.0 * s.c[i - 1] + 6.0 * s.d[i - 1] * h;
            double right = 2.0 * s.c[i];
            if (!(fabs(left - right) < 1e-9 * (1.0 + fabs(right)))) {
                printf("spline: f'' jumps by %g at knot %d of %s\n",
                       left - right, i, pheno_name[p]);
                bad++;
            }
        }
        if (!(fabs(s.d[0] - s.d[1]) < 1e-12 * (1.0 + fabs(s.d[0]))) ||
            !(fabs(s.d[NF - 3] - s.d[NF - 2]) < 1e-12 * (1.0 + fabs(s.d[NF - 3])))) {
            printf("spline: not-a-knot end condition violated for %s\n",
                   pheno_name[p]);
            bad++;
        }
    }
    return bad;
}

/* --- the observer ------------------------------------------------------ */

static double normal_cdf(double z) {
    return 0.5 * erfc(-z * SQRT1_2);
}

/* One (phenotype, beta) pair, with the metric grid and the truth on it
 * precomputed: the truth does not change over a replication, and evaluating it
 * 900 times per scored trial otherwise shows up next to the model. */
typedef struct field {
    int    pheno;
    double beta;
    spline sp;
    double xc[NGRID], xi[NGRID];
    /* The metric grid as one n x n_dims array, so a scored trial is one
     * yaep_predict_p_many() call instead of 900 yaep_predict_p() calls. The
     * order is context-major, intensity-minor, the same as truth_p. */
    double xs[NGRID * NGRID * 2];
    double truth_p[NGRID * NGRID];
    double truth_thr[NGRID];
    int    truth_cross[NGRID];
} field;

static double theta_true(const field* fd, double xc) {
    return spline_eval(&fd->sp, exp2(xc));
}

static double p_true(const field* fd, double xc, double xi) {
    return normal_cdf((xi - theta_true(fd, xc)) / fd->beta);
}

/* The first pair of grid points that brackets `target` along the column,
 * linearly interpolated. The same routine runs on the truth and on the model,
 * so the grid's own coarseness cancels out of the difference; a model column
 * that wanders back across the target is read at its lowest crossing, which is
 * what yaep_threshold() reports too. */
static int column_threshold(const double* p, const double* xi, double target,
                            double* out) {
    int j;
    for (j = 0; j < NGRID - 1; j++) {
        double a = p[j] - target, b = p[j + 1] - target;
        if ((a <= 0.0 && b >= 0.0) || (a >= 0.0 && b <= 0.0)) {
            if (b == a) continue;
            *out = xi[j] + (xi[j + 1] - xi[j]) * (-a) / (b - a);
            return 1;
        }
    }
    return 0;
}

static void field_init(field* fd, int pheno, double beta) {
    int i, j;
    fd->pheno = pheno;
    fd->beta = beta;
    spline_fit(&fd->sp, audio_f, pheno_thr[pheno], NF);
    for (i = 0; i < NGRID; i++) {
        double u = (double)i / (double)(NGRID - 1);
        fd->xc[i] = XC_LO + (XC_HI - XC_LO) * u;
        fd->xi[i] = XI_LO + (XI_HI - XI_LO) * u;
    }
    for (i = 0; i < NGRID; i++) {
        for (j = 0; j < NGRID; j++) {
            fd->truth_p[i * NGRID + j] = p_true(fd, fd->xc[i], fd->xi[j]);
            fd->xs[2 * (i * NGRID + j)]     = fd->xc[i];
            fd->xs[2 * (i * NGRID + j) + 1] = fd->xi[j];
        }
        fd->truth_cross[i] = column_threshold(&fd->truth_p[i * NGRID], fd->xi,
                                              TARGET_P, &fd->truth_thr[i]);
        if (!fd->truth_cross[i]) fd->truth_thr[i] = 0.0;
    }
}

/* The two metrics, given the model's E[p] on the same grid, and MAE(p) again
 * over the transition band alone, the grid points whose true p is in
 * [0.05, 0.95] (the definition tests/compare/compare_gp_aepsych.py uses). The
 * band is where a model has to represent the psychometric rise, and it is a
 * few points per column, so the whole-grid MAE(p) mostly measures the flat
 * floor and ceiling. */
static void score(const field* fd, const double* model_p, double* mae_p,
                  double* mae_thr, int* n_missing, double* mae_band) {
    double sp = 0.0, st = 0.0, sb = 0.0;
    int i, j, nt = 0, miss = 0, nb = 0;
    for (i = 0; i < NGRID; i++) {
        double thr;
        for (j = 0; j < NGRID; j++) {
            double e = fabs(model_p[i * NGRID + j] - fd->truth_p[i * NGRID + j]);
            double tp = fd->truth_p[i * NGRID + j];
            sp += e;
            if (tp >= 0.05 && tp <= 0.95) { sb += e; nb++; }
        }
        if (column_threshold(&model_p[i * NGRID], fd->xi, TARGET_P, &thr)) {
            if (fd->truth_cross[i]) {
                st += fabs(thr - fd->truth_thr[i]);
                nt++;
            } else {
                miss++;
            }
        } else {
            miss++;
        }
    }
    *mae_p = sp / (double)(NGRID * NGRID);
    *mae_thr = nt ? st / (double)nt : -1.0;
    *n_missing = miss;
    *mae_band = nb ? sb / (double)nb : -1.0;
}

/* --- the generator ----------------------------------------------------- */

/* splitmix64, seeded from the replication index alone, so a replication is the
 * same response stream whatever order the methods run in and whichever
 * configuration is asked for on the command line. */
static uint64_t g_rng;

static void rng_seed(int rep) {
    g_rng = 0x243F6A8885A308D3ULL + 0x9E3779B97F4A7C15ULL * (uint64_t)(rep + 1);
}

static double uniform01(void) {
    uint64_t z;
    g_rng += 0x9E3779B97F4A7C15ULL;
    z = g_rng;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    z = z ^ (z >> 31);
    return (double)(z >> 11) * (1.0 / 9007199254740992.0);
}

static double rng_for_gp(void* ctx) {
    (void)ctx;
    return uniform01();
}

/* --- methods ----------------------------------------------------------- */

#define NMARK 4
static const int marks[NMARK] = { 25, 50, 100, 150 };

#define NBOUND 6
static const char* const bound_name[NBOUND] = {
    "ls_freq", "ls_int", "outscale", "mean", "outscale_b", "mean_g"
};

typedef struct method {
    const char*  name;
    int          is_stair;
    yaep_acq    acq;
    yaep_kernel kernel;
    yaep_model  model;
    int          monotone;   /* desc.monotone_dims along the intensity */
} method;

/* semip2-* is YAEP_MODEL_PSYCHOMETRIC, Keeley et al. 2023's semiparametric
 * model in its own form: a threshold GP and a log-slope GP over frequency, and a
 * probit in intensity. */
static const method methods[] = {
    { "lse",        0, YAEP_ACQ_LSE,     YAEP_KERNEL_RBF,   YAEP_MODEL_GP, 0 },
    { "eavc",       0, YAEP_ACQ_EAVC,    YAEP_KERNEL_RBF,   YAEP_MODEL_GP, 0 },
    { "localmi",    0, YAEP_ACQ_LOCALMI, YAEP_KERNEL_RBF,   YAEP_MODEL_GP, 0 },
    { "balv",       0, YAEP_ACQ_BALV,    YAEP_KERNEL_RBF,   YAEP_MODEL_GP, 0 },
    { "bald",       0, YAEP_ACQ_BALD,    YAEP_KERNEL_RBF,   YAEP_MODEL_GP, 0 },
    { "random",     0, YAEP_ACQ_RANDOM,  YAEP_KERNEL_RBF,   YAEP_MODEL_GP, 0 },
    { "semip-lse",  0, YAEP_ACQ_LSE,     YAEP_KERNEL_SEMIP, YAEP_MODEL_GP, 0 },
    { "semip-balv", 0, YAEP_ACQ_BALV,    YAEP_KERNEL_SEMIP, YAEP_MODEL_GP, 0 },
    { "semip-bald", 0, YAEP_ACQ_BALD,    YAEP_KERNEL_SEMIP, YAEP_MODEL_GP, 0 },
    { "semip2-lse", 0, YAEP_ACQ_LSE,     YAEP_KERNEL_RBF,   YAEP_MODEL_PSYCHOMETRIC, 0 },
    { "semip2-balv",0, YAEP_ACQ_BALV,    YAEP_KERNEL_RBF,   YAEP_MODEL_PSYCHOMETRIC, 0 },
    { "semip2-bald",0, YAEP_ACQ_BALD,    YAEP_KERNEL_RBF,   YAEP_MODEL_PSYCHOMETRIC, 0 },
    { "semip2-eavc",0, YAEP_ACQ_EAVC,    YAEP_KERNEL_RBF,   YAEP_MODEL_PSYCHOMETRIC, 0 },
    { "semip2-localmi",0, YAEP_ACQ_LOCALMI, YAEP_KERNEL_RBF, YAEP_MODEL_PSYCHOMETRIC, 0 },
    { "lse-mono",   0, YAEP_ACQ_LSE,     YAEP_KERNEL_RBF,   YAEP_MODEL_GP, 1 },
    { "eavc-mono",  0, YAEP_ACQ_EAVC,    YAEP_KERNEL_RBF,   YAEP_MODEL_GP, 1 },
    { "bald-mono",  0, YAEP_ACQ_BALD,    YAEP_KERNEL_RBF,   YAEP_MODEL_GP, 1 },
    { "stair",      1, YAEP_ACQ_LSE,     YAEP_KERNEL_RBF,   YAEP_MODEL_GP, 0 }
};
#define NMETHOD ((int)(sizeof(methods) / sizeof(methods[0])))

/* Running mean and 95% interval over replications. Sums, not a list: the
 * interval is the normal one, 1.96 sd / sqrt(n), so nothing else is needed. */
typedef struct acc {
    double n, s, s2;
} acc;

static void acc_add(acc* a, double v) {
    a->n += 1.0;
    a->s += v;
    a->s2 += v * v;
}

static double acc_mean(const acc* a) {
    return a->n > 0.0 ? a->s / a->n : -1.0;
}

static double acc_ci(const acc* a) {
    double var;
    if (a->n < 2.0) return 0.0;
    var = (a->s2 - a->s * a->s / a->n) / (a->n - 1.0);
    if (var < 0.0) var = 0.0;
    return 1.96 * sqrt(var / a->n);
}

typedef struct mstat {
    acc p[NMARK], thr[NMARK], band[NMARK];
    acc ms, logmarg, miss;
    int runs, numeric, multi_cross, failed;
    int bound[NBOUND];
} mstat;

typedef struct config {
    int    reps;
    int    n_trials;
    int    metric_every;
    int    fit_every;
    FILE*  csv;
} config;

static int is_scored(int n, int every) {
    int i;
    for (i = 0; i < NMARK; i++) if (n == marks[i]) return 1;
    return every > 0 && n % every == 0;
}

static void record(mstat* st, const double* mp, const double* mt,
                   const double* mb) {
    int i;
    for (i = 0; i < NMARK; i++) {
        if (mp[i] >= 0.0) acc_add(&st->p[i], mp[i]);
        if (mt[i] >= 0.0) acc_add(&st->thr[i], mt[i]);
        if (mb[i] >= 0.0) acc_add(&st->band[i], mb[i]);
    }
}

static void csv_row(FILE* csv, const field* fd, const char* name, int rep,
                    int trial, double mae_p, double mae_thr, int miss,
                    double ms, double mae_band) {
    if (!csv) return;
    fprintf(csv, "%s,%g,%s,%d,%d,%.6f,%.4f,%d,%.4f,%.6f\n", pheno_name[fd->pheno],
            fd->beta, name, rep, trial, mae_p, mae_thr, miss, ms, mae_band);
}

/* --- the GP runs ------------------------------------------------------- */

/* How far a fitted hyperparameter is from its bounds, in the log units the fit
 * works in. A value pinned at a bound is a fit that ran out of room, and the
 * manual says to look for it, so every run is checked and the counts are
 * printed under the summary. */
static void check_bounds(const yaep_hyper* h, const yaep_desc* d, int* out) {
    double ls_lo[2], ls_hi[2], os_lo, os_hi;
    int k;
    for (k = 0; k < 2; k++) {
        double span = d->hi[k] - d->lo[k];
        ls_lo[k] = d->hyper_min.lengthscale[k] > 0.0
                   ? d->hyper_min.lengthscale[k] : 0.05 * span;
        ls_hi[k] = d->hyper_max.lengthscale[k] > 0.0
                   ? d->hyper_max.lengthscale[k] : 2.0 * span;
        if (h->lengthscale[k] <= ls_lo[k] * 1.001 ||
            h->lengthscale[k] >= ls_hi[k] * 0.999) out[k]++;
    }
    if (d->model == YAEP_MODEL_PSYCHOMETRIC) {
        /* The threshold GP's variance is in dB^2 and centered on a quarter of
         * the axis squared; its mean lives on the intensity axis; and the mean
         * log-slope has bounds of its own. ysp/aep.h's defaults, restated. */
        double span = XI_HI - XI_LO, os_m = 0.0625 * span * span;
        os_lo = 0.1 * os_m;
        os_hi = 10.0 * os_m;
        if (h->outputscale <= os_lo * 1.001 || h->outputscale >= os_hi * 0.999) out[2]++;
        if (h->mean <= XI_LO + 1e-6 * span || h->mean >= XI_HI - 1e-6 * span) out[3]++;
        if (h->mean_g <= log(0.25 / span) + 1e-3 || h->mean_g >= log(256.0 / span) - 1e-3)
            out[5]++;
        return;
    }
    os_lo = d->hyper_min.outputscale > 0.0 ? d->hyper_min.outputscale : 0.1;
    os_hi = d->hyper_max.outputscale > 0.0 ? d->hyper_max.outputscale : 10.0;
    if (h->outputscale <= os_lo * 1.001 || h->outputscale >= os_hi * 0.999) out[2]++;
    {
        double m_lo = d->hyper_min.mean != 0.0 ? d->hyper_min.mean : -5.0;
        double m_hi = d->hyper_max.mean != 0.0 ? d->hyper_max.mean : 5.0;
        if (h->mean <= m_lo * 0.999 || h->mean >= m_hi * 0.999) out[3]++;
    }
    if (d->kernel == YAEP_KERNEL_SEMIP) {
        os_lo = d->hyper_min.outputscale_b > 0.0 ? d->hyper_min.outputscale_b : 0.1;
        os_hi = d->hyper_max.outputscale_b > 0.0 ? d->hyper_max.outputscale_b : 10.0;
        if (h->outputscale_b <= os_lo * 1.001 ||
            h->outputscale_b >= os_hi * 0.999) out[4]++;
    }
}

static int run_gp(const field* fd, const method* m, int rep, const config* cfg,
                  mstat* st) {
    yaep_desc d;
    yaep_gp g;
    double mp[NMARK], mt[NMARK], mb[NMARK], model_p[NGRID * NGRID];
    clock_t t_all, t_score = 0;
    int i, n = 0, last_miss = 0;

    memset(&d, 0, sizeof(d));
    d.model = m->model;
    d.n_dims = 2;
    d.lo[0] = XC_LO; d.hi[0] = XC_HI;
    d.lo[1] = XI_LO; d.hi[1] = XI_HI;
    d.intensity_dim = 1;
    d.kernel = m->kernel;
    d.acq = m->acq;
    if (m->monotone) d.monotone_dims = 1u << 1;
    d.target_p = TARGET_P;
    d.grid[0] = GRID_C;
    d.grid[1] = GRID_I;
    d.n_init = N_INIT;
    d.fit = true;
    d.fit_every = cfg->fit_every;
    d.stop_trials = cfg->n_trials;
    d.max_trials = cfg->n_trials;
    d.hyper_min.lengthscale[1] = LS_INT_MIN;
    if (m->kernel == YAEP_KERNEL_SEMIP) {
        d.hyper_max.outputscale = OS_MAX_SEMIP;
        d.hyper_max.outputscale_b = OS_B_MAX_SEMIP;
    } else {
        d.hyper_max.outputscale = OS_MAX_RBF;
    }
    if (m->acq == YAEP_ACQ_RANDOM) {
        /* The quasi-random baseline is the same Halton set every replication
         * unless it is given a generator, and a baseline with no
         * replication-to-replication spread is not a baseline. */
        d.rng = rng_for_gp;
        d.rng_ctx = NULL;
    }

    for (i = 0; i < NMARK; i++) mp[i] = mt[i] = mb[i] = -1.0;
    rng_seed(rep);
    if (!yaep_open(&g, &d)) {
        fprintf(stderr, "aep_audiometric: %s\n", yaep_error(&g));
        st->failed++;
        return 1;
    }
    st->runs++;
    /* One clock() per run rather than two per trial: a trial here is a
     * millisecond or two and clock() is only accurate to one on Windows, so the
     * scored blocks are timed instead and taken back out of the total. */
    t_all = clock();
    while (!yaep_done(&g)) {
        double x[2], p[2];
        int rc;
        if (yaep_next(&g, x) < -1) break;
        p[1] = p_true(fd, x[0], x[1]);
        p[0] = 1.0 - p[1];
        rc = yaep_update(&g, x, yaep_simulate_outcome(p, 2, uniform01()));
        if (rc == YAEP_ERR_NUMERIC) {
            st->numeric++;
        } else if (rc != YAEP_OK) {
            fprintf(stderr, "aep_audiometric: %s: update: %s\n", m->name,
                    yaep_strerror(rc));
            st->failed++;
            break;
        }
        n = yaep_n_trials(&g);
        if (is_scored(n, cfg->metric_every)) {
            double mae_p, mae_thr, ms, mae_band;
            int miss, rcp;
            clock_t t0 = clock();
            ms = 1000.0 * (double)(t0 - t_all - t_score) / (double)CLOCKS_PER_SEC
                 / (double)n;
            rcp = yaep_predict_p_many(&g, fd->xs, NGRID * NGRID, model_p);
            if (rcp != YAEP_OK) {
                fprintf(stderr, "aep_audiometric: %s: predict_p_many: %s\n",
                        m->name, yaep_strerror(rcp));
                st->failed++;
                break;
            }
            score(fd, model_p, &mae_p, &mae_thr, &miss, &mae_band);
            last_miss = miss;
            for (i = 0; i < NMARK; i++) {
                if (n == marks[i]) { mp[i] = mae_p; mt[i] = mae_thr; mb[i] = mae_band; }
            }
            csv_row(cfg->csv, fd, m->name, rep, n, mae_p, mae_thr, miss, ms,
                    mae_band);
            t_score += clock() - t0;
        }
    }
    if (n > 0) {
        yaep_hyper h;
        double spent = (double)(clock() - t_all - t_score) / (double)CLOCKS_PER_SEC;
        memset(&h, 0, sizeof(h));
        yaep_get_hyper(&g, &h);
        check_bounds(&h, &d, st->bound);
        acc_add(&st->logmarg, yaep_log_marginal(&g));
        acc_add(&st->ms, 1000.0 * spent / (double)n);
        acc_add(&st->miss, (double)last_miss);
        if (yaep_threshold_multi_cross(&g)) st->multi_cross++;
    }
    record(st, mp, mt, mb);
    yaep_close(&g);
    return 0;
}

/* --- the staircase baseline -------------------------------------------- */

/* The 8 estimated thresholds read as a curve: linear between the measured
 * frequencies in log2 f, and the end segment extended beyond them. The truth
 * extrapolates linearly too (in kHz, not in octaves), so neither end of the
 * box is free for the staircase, but neither is it a wall. */
static double stair_curve(const double* thr, double xc) {
    double xm[NF];
    int i;
    for (i = 0; i < NF; i++) xm[i] = log2(audio_f[i]);
    if (xc <= xm[0])
        return thr[0] + (thr[1] - thr[0]) * (xc - xm[0]) / (xm[1] - xm[0]);
    if (xc >= xm[NF - 1])
        return thr[NF - 1] + (thr[NF - 1] - thr[NF - 2]) * (xc - xm[NF - 1])
                             / (xm[NF - 1] - xm[NF - 2]);
    for (i = 0; i < NF - 2; i++) if (xc < xm[i + 1]) break;
    return thr[i] + (thr[i + 1] - thr[i]) * (xc - xm[i]) / (xm[i + 1] - xm[i]);
}

static int run_stair(const field* fd, int rep, const config* cfg, mstat* st) {
    static yst_stair s[NF];
    yst_desc d;
    double mp[NMARK], mt[NMARK], mb[NMARK], model_p[NGRID * NGRID], thr[NF];
    clock_t t_all, t_score = 0;
    int i, t, last_miss = 0;

    memset(&d, 0, sizeof(d));
    d.start = STAIR_START;
    d.n_up = 1;
    d.n_down = 1;
    d.step_type = YST_STEP_LIN;      /* the level IS dB HL */
    /* Kaernbach's weighted rule: the harder (down) step is scaled so the track
     * converges on 75% with one response per step. 10 dB until the first
     * reversal so that 18 trials can still reach a 77 dB threshold from a
     * 40 dB start, then the clinical 5 dB. */
    d.steps[0] = 10.0;
    d.steps[1] = 5.0;
    d.n_steps = 2;
    d.step_down_scale = yst_weighted_scale(TARGET_P);
    d.min = XI_LO;
    d.max = XI_HI;
    d.stop_trials = cfg->n_trials / NF + 2;
    d.stop_at_limit = 0;

    for (i = 0; i < NMARK; i++) mp[i] = mt[i] = mb[i] = -1.0;
    rng_seed(rep);
    for (i = 0; i < NF; i++) {
        if (!yst_open(&s[i], &d)) {
            fprintf(stderr, "aep_audiometric: stair: %s\n", yst_error(&s[i]));
            st->failed++;
            return 1;
        }
    }
    st->runs++;
    t_all = clock();
    for (t = 1; t <= cfg->n_trials; t++) {
        int k = (t - 1) % NF;
        double level, p, xc = log2(audio_f[k]);
        level = yst_next(&s[k]);
        p = p_true(fd, xc, level);
        if (yst_update(&s[k], level, yst_simulate_response(p, uniform01())) < 0) {
            fprintf(stderr, "aep_audiometric: stair: update failed\n");
            st->failed++;
            break;
        }
        if (is_scored(t, cfg->metric_every)) {
            double mae_p, mae_thr, ms, mae_band;
            int miss, c, j;
            clock_t t0 = clock();
            ms = 1000.0 * (double)(t0 - t_all - t_score) / (double)CLOCKS_PER_SEC
                 / (double)t;
            for (i = 0; i < NF; i++) {
                thr[i] = yst_estimate(&s[i], YST_EST_REVERSALS);
                /* Before the first counted reversal there is nothing to
                 * average, so the running estimate is the level the rule would
                 * propose next. A real experiment reads the same thing. */
                if (!(thr[i] == thr[i])) thr[i] = yst_estimate(&s[i], YST_EST_LAST);
            }
            for (c = 0; c < NGRID; c++) {
                double th = stair_curve(thr, fd->xc[c]);
                for (j = 0; j < NGRID; j++)
                    model_p[c * NGRID + j] = normal_cdf((fd->xi[j] - th) / fd->beta);
            }
            score(fd, model_p, &mae_p, &mae_thr, &miss, &mae_band);
            last_miss = miss;
            for (i = 0; i < NMARK; i++) {
                if (t == marks[i]) { mp[i] = mae_p; mt[i] = mae_thr; mb[i] = mae_band; }
            }
            csv_row(cfg->csv, fd, "stair", rep, t, mae_p, mae_thr, miss, ms,
                    mae_band);
            t_score += clock() - t0;
        }
    }
    acc_add(&st->ms, 1000.0 * (double)(clock() - t_all - t_score)
                     / (double)CLOCKS_PER_SEC / (double)cfg->n_trials);
    acc_add(&st->miss, (double)last_miss);
    record(st, mp, mt, mb);
    return 0;
}

/* --- reporting --------------------------------------------------------- */

static void print_table(const char* title, const mstat* st, const int* use,
                        int n_use) {
    int i, k;
    printf("\n%s\n", title);
    printf("method          trial        MAE(p)            band MAE(p)       "
           "MAE(thr, dB)        ms/trial\n");
    for (i = 0; i < n_use; i++) {
        const mstat* s = &st[use[i]];
        int first = 1;
        for (k = 0; k < NMARK; k++) {
            if (s->p[k].n < 1.0) continue;
            printf("%-14s  %5d   %6.4f +- %6.4f   %6.4f +- %6.4f   ",
                   first ? methods[use[i]].name : "",
                   marks[k], acc_mean(&s->p[k]), acc_ci(&s->p[k]),
                   acc_mean(&s->band[k]), acc_ci(&s->band[k]));
            if (s->thr[k].n >= 1.0)
                printf("%7.2f +- %6.2f", acc_mean(&s->thr[k]), acc_ci(&s->thr[k]));
            else
                printf("      -            ");
            /* On the method's first row, because a short session leaves the
             * later marks empty. */
            if (first) printf("   %8.3f", acc_mean(&s->ms));
            printf("\n");
            first = 0;
        }
    }
}

static void print_diag(const mstat* st, const int* use, int n_use) {
    int i, b;
    printf("\ndiagnostics (per run: 0.75 columns with no crossing out of %d, "
           "log marginal, bounds)\n", NGRID);
    printf("method          runs  numeric  multi  nocross   logmarg   bounds hit\n");
    for (i = 0; i < n_use; i++) {
        const mstat* s = &st[use[i]];
        if (!s->runs) continue;
        printf("%-14s  %4d  %7d  %5d  %7.2f", methods[use[i]].name, s->runs,
               s->numeric, s->multi_cross, acc_mean(&s->miss));
        if (s->logmarg.n >= 1.0) printf("  %8.1f  ", acc_mean(&s->logmarg));
        else printf("         -  ");
        for (b = 0; b < NBOUND; b++)
            if (s->bound[b]) printf("%s %d  ", bound_name[b], s->bound[b]);
        if (s->failed) printf("FAILED %d", s->failed);
        printf("\n");
    }
}

/* --- command line ------------------------------------------------------ */

static int parse_pheno(const char* a, int* out) {
    int i;
    if (!strcmp(a, "all")) { *out = -1; return 1; }
    for (i = 0; i < NPHENO; i++) if (!strcmp(a, pheno_name[i])) { *out = i; return 1; }
    if (a[0] >= '0' && a[0] <= '3' && a[1] == '\0') { *out = a[0] - '0'; return 1; }
    return 0;
}

static int parse_methods(const char* a, int* use, int* n_use) {
    const char* p = a;
    int i;
    *n_use = 0;
    if (!strcmp(a, "all")) {
        for (i = 0; i < NMETHOD; i++) use[(*n_use)++] = i;
        return 1;
    }
    while (*p) {
        const char* q = strchr(p, ',');
        size_t len = q ? (size_t)(q - p) : strlen(p);
        int found = -1;
        for (i = 0; i < NMETHOD; i++)
            if (strlen(methods[i].name) == len && !strncmp(p, methods[i].name, len))
                found = i;
        if (found < 0) return 0;
        use[(*n_use)++] = found;
        p = q ? q + 1 : p + len;
    }
    return *n_use > 0;
}

static void usage(void) {
    int i;
    fprintf(stderr,
            "usage: aep_audiometric [phenotype] [beta] [reps] [methods] [csv] "
            "[fit_every]\n"
            "  phenotype: all, 0..3, or one of");
    for (i = 0; i < NPHENO; i++) fprintf(stderr, " %s", pheno_name[i]);
    fprintf(stderr, "\n  beta:      all, or a positive number\n"
                    "  reps:      1..200\n  methods:   all, or a comma list of");
    for (i = 0; i < NMETHOD; i++) fprintf(stderr, " %s", methods[i].name);
    fprintf(stderr, "\n  csv:       a path, or - for none\n"
                    "  fit_every: trials between hyperparameter fits\n");
}

int main(int argc, char** argv) {
    static mstat st[NMETHOD], pooled[NMETHOD];
    config cfg;
    int use[NMETHOD], n_use = 0;
    int pheno = NPHENO - 1, i, pi, bi;
    int n_pheno = 1, n_beta = 1;
    double beta = 2.0;
    const double* betas = &beta;
    const char* csv_path = NULL;
    clock_t t_start;

    cfg.reps = 3;
    cfg.n_trials = N_TRIALS;
    cfg.metric_every = 10;
    cfg.fit_every = 20;
    cfg.csv = NULL;
    /* CI runs this program with no arguments and wants an answer in 20 s, and 30
     * fitted 150-trial sessions are a minute and a half of the measured cost
     * below. The no-argument configuration therefore stops at 100 trials; every
     * other setting is the one an explicit run uses, so the code path CI
     * exercises is the same one. */
    if (argc == 1) cfg.n_trials = 100;

    if (spline_selfcheck() != 0) {
        fprintf(stderr, "aep_audiometric: the spline self-check failed\n");
        return 1;
    }
    if (argc > 1 && !parse_pheno(argv[1], &pheno)) { usage(); return 2; }
    if (argc > 2) {
        if (!strcmp(argv[2], "all")) {
            betas = all_betas;
            n_beta = (int)(sizeof(all_betas) / sizeof(all_betas[0]));
        } else {
            beta = strtod(argv[2], NULL);
            if (!(beta > 0.0) || !(beta < 1e3)) { usage(); return 2; }
        }
    }
    if (argc > 3) {
        cfg.reps = (int)strtol(argv[3], NULL, 10);
        if (cfg.reps < 1 || cfg.reps > 200) { usage(); return 2; }
        cfg.metric_every = 5;   /* an explicit run is not the CI run */
    }
    if (argc > 4) {
        if (!parse_methods(argv[4], use, &n_use)) { usage(); return 2; }
    }
    if (argc > 5 && strcmp(argv[5], "-")) csv_path = argv[5];
    if (argc > 6) {
        cfg.fit_every = (int)strtol(argv[6], NULL, 10);
        if (cfg.fit_every < 0) { usage(); return 2; }
    }
    /* Every method, except that the CI configuration leaves out the
     * psychometric model's two look-ahead methods: semip2-eavc alone is about
     * 6 of the 20 seconds CI allows, and tests/adapt/aep_test.c already
     * runs both look-ahead paths of that model. The -mono rows are left out
     * for the same reason, and the test runs the projection too. */
    if (n_use == 0)
        for (i = 0; i < NMETHOD; i++) {
            if (argc == 1 && (!strcmp(methods[i].name, "semip2-eavc") ||
                              !strcmp(methods[i].name, "semip2-localmi") ||
                              methods[i].monotone)) continue;
            use[n_use++] = i;
        }
    if (pheno < 0) { pheno = 0; n_pheno = NPHENO; }
    if (csv_path) {
        cfg.csv = fopen(csv_path, "w");
        if (!cfg.csv) {
            fprintf(stderr, "aep_audiometric: cannot write %s\n", csv_path);
            return 1;
        }
        fprintf(cfg.csv, "phenotype,beta,method,rep,trial,mae_p,mae_thr_db,"
                         "nocross_cols,ms_per_trial,mae_band\n");
    }

    printf("aep_audiometric: Owen et al. 2021 (arXiv:2104.09549) audiometric "
           "comparison\n");
    printf("observer: Dubno et al. 2013 JARO 14:687-701 Fig. 2 phenotypes, "
           "not-a-knot spline in kHz\n");
    printf("box: log2 f/kHz in [%.0f, %.0f], intensity in [%.0f, %.0f] dB HL; "
           "%d trials (%d Halton), target p = %.2f\n", XC_LO, XC_HI, XI_LO,
           XI_HI, cfg.n_trials, N_INIT, TARGET_P);
    printf("candidates: %d x %d grid; hyperparameters fitted every %d trials; "
           "metrics every %d trials on a %d x %d grid\n", GRID_C, GRID_I,
           cfg.fit_every, cfg.metric_every, NGRID, NGRID);
    printf("%d replication%s per configuration, %d method%s, %d phenotype%s, "
           "%d beta%s\n", cfg.reps, cfg.reps == 1 ? "" : "s", n_use,
           n_use == 1 ? "" : "s", n_pheno, n_pheno == 1 ? "" : "s", n_beta,
           n_beta == 1 ? "" : "s");

    t_start = clock();
    for (pi = 0; pi < n_pheno; pi++) {
        for (bi = 0; bi < n_beta; bi++) {
            field fd;
            char title[128];
            int rep;
            field_init(&fd, pheno + pi, betas[bi]);
            memset(st, 0, sizeof(st));
            for (i = 0; i < n_use; i++) {
                for (rep = 0; rep < cfg.reps; rep++) {
                    if (methods[use[i]].is_stair)
                        run_stair(&fd, rep, &cfg, &st[use[i]]);
                    else
                        run_gp(&fd, &methods[use[i]], rep, &cfg, &st[use[i]]);
                }
            }
            snprintf(title, sizeof title, "== %s, beta = %g, %d replications ==",
                    pheno_name[fd.pheno], fd.beta, cfg.reps);
            print_table(title, st, use, n_use);
            print_diag(st, use, n_use);
            for (i = 0; i < n_use; i++) {
                int k, b;
                mstat* a = &pooled[use[i]];
                const mstat* s = &st[use[i]];
                for (k = 0; k < NMARK; k++) {
                    a->p[k].n += s->p[k].n;   a->p[k].s += s->p[k].s;
                    a->p[k].s2 += s->p[k].s2;
                    a->thr[k].n += s->thr[k].n; a->thr[k].s += s->thr[k].s;
                    a->thr[k].s2 += s->thr[k].s2;
                    a->band[k].n += s->band[k].n; a->band[k].s += s->band[k].s;
                    a->band[k].s2 += s->band[k].s2;
                }
                a->ms.n += s->ms.n; a->ms.s += s->ms.s; a->ms.s2 += s->ms.s2;
                a->miss.n += s->miss.n; a->miss.s += s->miss.s;
                a->miss.s2 += s->miss.s2;
                a->logmarg.n += s->logmarg.n; a->logmarg.s += s->logmarg.s;
                a->logmarg.s2 += s->logmarg.s2;
                a->runs += s->runs; a->numeric += s->numeric;
                a->multi_cross += s->multi_cross; a->failed += s->failed;
                for (b = 0; b < NBOUND; b++) a->bound[b] += s->bound[b];
            }
            fflush(stdout);
        }
    }
    if (n_pheno * n_beta > 1) {
        print_table("== pooled over every phenotype and beta ==", pooled, use,
                    n_use);
        print_diag(pooled, use, n_use);
    }
    printf("\ntotal %.1f s of CPU time. The interval is 1.96 sd / sqrt(reps) "
           "over replications,\nnot an interval on one replication: a single "
           "seed says nothing about a method.\n",
           (double)(clock() - t_start) / (double)CLOCKS_PER_SEC);
    if (cfg.csv) fclose(cfg.csv);
    return 0;
}
