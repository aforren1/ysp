/* aep_async - the GP off the frame loop: ysp/aep.h's async layer in a 16 ms loop.
 *
 * The shape docs/adapt.md calls for in "Inference on a thread", and the
 * answer to FRAME BUDGET for a configuration that does not fit one: the trial
 * loop submits the response and goes back to drawing frames, the pump thread
 * runs yaep_update(), yaep_next() and yaep_threshold(), and the loop asks
 * "is the next stimulus ready?" one frame later. What it costs when the answer
 * is no is one more frame of the same stimulus, not a dropped frame, and this
 * program counts how often that happened.
 *
 * The observer is the 2-D detection observer of examples/aep/sim.c: a threshold
 * curve across one context dimension, a probit psychometric function of
 * intensity. The acquisition is EAVC, the look-ahead one, which is the
 * expensive one on purpose: at N = 200 with 225 candidates a proposal is tens
 * of milliseconds, several frames' worth, so the late count is not zero and the
 * output says something. desc.fit_in_idle is on, so the hyperparameter fit runs
 * in whatever time is left between trials and costs the loop nothing.
 *
 * There is no display here: a frame is yrt_sleep_until() and the "drawing" is
 * the sleep. Nothing needs hardware or a privilege.
 *
 *   cc -O2 -pthread -Iinclude -o aep_async examples/aep/async.c -lm   # Linux / macOS
 *   cl /O2 /Iinclude examples\aep\async.c                            # Windows (MSVC)
 *
 * Exit code: 0. A late proposal is a measurement, not a failure.
 */
#define YAEP_ASYNC
#define YSP_AEP_IMPLEMENTATION
#include "ysp/aep.h"

#include <stdio.h>
#include <math.h>
#include <string.h>

#define FRAME_NS   YRT_NS(16666667)   /* 60 Hz */
#define N_TRIALS   200
#define SLOPE      3.0
#define Z75        0.67448975019608171
#define MAX_WAIT_FRAMES 60       /* a proposal later than a second is a bug */
/* A wall-clock cap, because how many frames a trial waits depends on what else
 * the machine is doing and this is an example, not a session. */
#define TIME_CAP_NS YRT_S(15)

/* The observer: the 50% point moves with the context, the rise is fixed. */
static double x50_of(double c) { return -0.2 + 0.3 * sin(3.0 * c); }
static double true_threshold(double c) { return x50_of(c) + Z75 / SLOPE; }

static uint64_t rng_state = 0x9E3779B97F4A7C15ull;

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

int main(void) {
    yaep_desc d;
    yaep_gp g;
    yaep_async a;
    yaep_async_desc ad;
    yaep_snapshot s;
    int64_t frame_deadline, t_start;
    int ready_next_frame = 0, late = 0, busy = 0, frames = 0, worst_frames = 0;
    long total_wait = 0;

    memset(&d, 0, sizeof(d));
    memset(&a, 0, sizeof(a));
    memset(&g, 0, sizeof(g));
    d.n_dims = 2;
    d.lo[0] = 0.0;  d.hi[0] = 1.0;     /* context   */
    d.lo[1] = -1.0; d.hi[1] = 1.0;     /* intensity */
    d.intensity_dim = 1;
    d.acq = YAEP_ACQ_EAVC;
    d.target_p = 0.75;
    d.grid[0] = 15;
    d.grid[1] = 15;
    d.n_init = 10;
    d.fit = true;
    d.max_trials = N_TRIALS;
    d.stop_trials = N_TRIALS;
    if (!yaep_open(&g, &d)) {
        fprintf(stderr, "aep_async: %s\n", yaep_error(&g));
        return 1;
    }
    memset(&ad, 0, sizeof(ad));
    ad.gp = &g;
    ad.fit_in_idle = true;       /* the fit lives in the gaps */
    ad.below_normal = true;      /* never above the frame loop */
    ad.context[0] = ctxs[1];     /* the snapshot's threshold, at c = 0.5 */
    if (!yaep_async_open(&a, &ad)) {
        fprintf(stderr, "aep_async: %s\n", yaep_async_error(&a));
        yaep_close(&g);
        return 1;
    }
    printf("aep_async: %d trials, EAVC on a %d-point grid, %d ms frames, "
           "fit in the gaps\n", N_TRIALS, yaep_n_candidates(&g),
           (int)(FRAME_NS / 1000000));
    printf("thread policy: %s\n",
           yaep_async_policy(&a) == YRT_POLICY_BELOW_NORMAL
           ? "below normal" : "normal (the OS refused the drop)");
    printf("  trial   frames waited   threshold at c = 0.5   error   log marg\n");

    t_start = yrt_now_ns();
    frame_deadline = t_start + FRAME_NS;
    yaep_async_poll(&a, &s);    /* the proposal start() made, seq 0 */
    for (int trial = 1; trial <= N_TRIALS; trial++) {
        int outcome, seq, waited = 0;
        /* One frame showing the stimulus, then the response. */
        yrt_sleep_until(frame_deadline, 0);
        frame_deadline += FRAME_NS;
        frames++;
        outcome = observe(s.x);
        seq = yaep_async_submit(&a, s.x, outcome);
        while (seq == YAEP_ERR_BUSY) {       /* only if the pump fell behind */
            busy++;
            yrt_sleep_until(frame_deadline, 0);
            frame_deadline += FRAME_NS;
            frames++;
            seq = yaep_async_submit(&a, s.x, outcome);
        }
        if (seq < 0) { fprintf(stderr, "aep_async: submit: %s\n",
                               yaep_strerror(seq)); break; }
        /* Frames, not blocking: this is where an experiment would draw. */
        for (;;) {
            yrt_sleep_until(frame_deadline, 0);
            frame_deadline += FRAME_NS;
            frames++;
            waited++;
            if (yaep_async_poll(&a, &s) >= seq) break;
            if (waited >= MAX_WAIT_FRAMES) break;
        }
        if (waited <= 1) ready_next_frame++; else late++;
        if (waited > worst_frames) worst_frames = waited;
        total_wait += waited;
        if (trial % 25 == 0) {
            double err = s.threshold_rc == YAEP_OK
                         ? s.threshold - true_threshold(ctxs[1]) : 0.0;
            printf("   %4d   %13d   %20.4f  %+.4f  %9.2f\n", trial, waited,
                   s.threshold, err, s.log_marginal);
        }
        if (s.done) break;
        if (yrt_now_ns() - t_start > TIME_CAP_NS) {
            printf("   (stopping at trial %d: out of the example's time cap)\n",
                   trial);
            break;
        }
    }
    printf("\n%d trials in %.1f s of frames (%d frames): the proposal was "
           "ready by the next frame %d times and late %d times; the wait was "
           "%.1f frames on average and %d at worst (%d ms); the queue was full "
           "%d times\n",
           yaep_n_trials(&g), (double)(yrt_now_ns() - t_start) / 1e9,
           frames, ready_next_frame, late,
           yaep_n_trials(&g) ? (double)total_wait / yaep_n_trials(&g) : 0.0,
           worst_frames, (int)((int64_t)worst_frames * FRAME_NS / 1000000),
           busy);

    yaep_async_close(&a);        /* drains; the handle is ours again */
    {
        yaep_hyper h;
        memset(&h, 0, sizeof(h));
        yaep_get_hyper(&g, &h);
        printf("final: %d trials, log marginal %.2f, lengthscales %.3f / %.3f, "
               "output scale %.3f\n", yaep_n_trials(&g), yaep_log_marginal(&g),
               h.lengthscale[0], h.lengthscale[1], h.outputscale);
        for (int i = 0; i < 3; i++) {
            double ctx[1], thr, lo, hi;
            int rc;
            ctx[0] = ctxs[i];
            rc = yaep_threshold(&g, ctx, 0.0, &thr, &lo, &hi);
            if (rc == YAEP_OK)
                printf("  threshold at c = %.1f: %+.4f, true %+.4f, "
                       "error %+.4f, band [%+.3f, %+.3f]\n", ctxs[i], thr,
                       true_threshold(ctxs[i]), thr - true_threshold(ctxs[i]),
                       lo, hi);
            else
                printf("  threshold at c = %.1f: %s\n", ctxs[i],
                       yaep_strerror(rc));
        }
    }
    yaep_close(&g);
    return 0;
}
