/* aep_sim - ysp/aep.h against a 2-D simulated observer with a known threshold.
 *
 * The observer is a detection task with one context dimension: the latent is
 * linear in intensity with a fixed slope, and the 75% threshold follows a
 * curve across the context. The program runs the level-set straddle, the
 * look-ahead EAVC and the global BALV over the same response stream (one
 * deterministic generator, reseeded per run) and prints the error of
 * yaep_threshold() at three contexts every ten trials, so the three
 * acquisitions can be read side by side.
 *
 * Exits 0 whatever the errors come out to: it is a demonstration, not a test.
 * tests/adapt/aep_test.c is the test.
 *
 *   cc -O2 -Iinclude -o aep_sim examples/aep/sim.c -lm
 *   cl /O2 /Iinclude examples\aep\sim.c
 */
#define YSP_AEP_IMPLEMENTATION
#include "ysp/aep.h"

#include <stdio.h>
#include <math.h>
#include <string.h>

#define N_TRIALS  150
#define SLOPE     3.0
#define TARGET_P  0.75
/* Phi^-1(0.75): the latent level the 75% threshold sits at. */
#define Z75       0.67448975019608171

/* The observer's 50% point as a function of context. */
static double x50_of(double c) {
    return -0.2 + 0.3 * sin(3.0 * c);
}

/* The true 75% threshold: the intensity where Phi(SLOPE (x - x50)) = 0.75. */
static double true_threshold(double c) {
    return x50_of(c) + Z75 / SLOPE;
}

/* --- a deterministic generator, so a run is reproducible ---------------- */

static uint64_t rng_state;

static double rng_u(void) {
    uint64_t z = (rng_state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z = z ^ (z >> 31);
    return (double)(z >> 11) * (1.0 / 9007199254740992.0);
}

static int observe(const double* x) {
    double p[2];
    p[1] = 0.5 * erfc(-(SLOPE * (x[1] - x50_of(x[0]))) / sqrt(2.0));
    p[0] = 1.0 - p[1];
    return yaep_simulate_outcome(p, 2, rng_u());
}

static const double ctxs[3] = { 0.2, 0.5, 0.8 };

static void report(const yaep_gp* g, int trial) {
    double err[3], band[3];
    int misses = 0;
    for (int i = 0; i < 3; i++) {
        double ctx[1], thr, lo, hi;
        ctx[0] = ctxs[i];
        if (yaep_threshold(g, ctx, 0.0, &thr, &lo, &hi) == YAEP_OK) {
            err[i] = thr - true_threshold(ctxs[i]);
            band[i] = hi - lo;
        } else {
            err[i] = band[i] = 0.0;
            misses++;
        }
    }
    printf("  %3d   %+7.3f %+7.3f %+7.3f    %5.3f %5.3f %5.3f  %s\n", trial,
           err[0], err[1], err[2], band[0], band[1], band[2],
           misses ? "(no crossing at one context)"
                  : (yaep_threshold_multi_cross(g) ? "(multiple crossings)" : ""));
}

static int run(yaep_acq acq, const char* name) {
    yaep_desc d;
    yaep_gp g;
    memset(&d, 0, sizeof(d));
    d.n_dims = 2;
    d.lo[0] = 0.0;  d.hi[0] = 1.0;    /* context */
    d.lo[1] = -1.0; d.hi[1] = 1.0;    /* intensity */
    d.intensity_dim = 1;
    d.kernel = YAEP_KERNEL_SEMIP;    /* linear in intensity, smooth in context */
    /* The observer's slope is 3 per unit of intensity, so the slope GP needs a
     * prior variance of about 9. The default output-scale ceiling of 10 is
     * right at that, and a hyperparameter pinned to its bound is a fit that
     * has run out of room, so both ceilings are lifted here. */
    d.hyperparams_max.outputscale = 25.0;
    d.hyperparams_max.outputscale_b = 25.0;
    d.acq = acq;
    d.target_p = TARGET_P;
    d.grid[0] = 21;
    d.grid[1] = 21;
    d.n_init = 10;
    d.fit = true;
    d.fit_every = 25;
    d.stop_trials = N_TRIALS;
    rng_state = 0x9E3779B97F4A7C15ull;
    if (!yaep_open(&g, &d)) {
        fprintf(stderr, "aep_sim: %s\n", yaep_error(&g));
        return 1;
    }
    printf("%s, %d trials, %d candidates, %.1f MB\n", name, N_TRIALS,
           yaep_n_candidates(&g),
           (double)yaep_memory_size(&d) / (1024.0 * 1024.0));
    printf("  trial  threshold error at c = 0.2, 0.5, 0.8   band width\n");
    while (!yaep_done(&g)) {
        double x[2];
        int rc;
        if (yaep_next(&g, x) < -1) break;
        rc = yaep_update(&g, x, observe(x));
        if (rc != YAEP_OK) {
            fprintf(stderr, "aep_sim: update: %s\n", yaep_strerror(rc));
            break;
        }
        if (yaep_n_trials(&g) % 10 == 0) report(&g, yaep_n_trials(&g));
    }
    {
        yaep_hyperparams h;
        memset(&h, 0, sizeof(h));
        yaep_get_hyper(&g, &h);
        printf("  fitted: context lengthscale %.3f, output scale %.3f, "
               "slope output scale %.3f, mean %+.3f, log marginal %.2f\n\n",
               h.lengthscale[0], h.outputscale, h.outputscale_b, h.mean,
               yaep_log_marginal(&g));
    }
    yaep_close(&g);
    return 0;
}

int main(void) {
    printf("aep_sim: 2-D observer, true 75%% threshold "
           "x = -0.2 + 0.3 sin(3 c) + %.3f\n\n", Z75 / SLOPE);
    if (run(YAEP_ACQ_LSE, "LSE (level-set straddle)")) return 1;
    if (run(YAEP_ACQ_EAVC, "EAVC (look-ahead level set)")) return 1;
    if (run(YAEP_ACQ_BALV, "BALV (global variance)")) return 1;
    return 0;
}
