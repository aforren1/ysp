/* quest_async.c - run QUEST+ on a background thread, from a frame loop.
 *
 * The shape docs/adapt.md asks for in "Inference on a thread", and the
 * reason ysp/quest.h has an ASYNC section: a trial loop that must not miss a
 * 16 ms frame, and a selection that takes longer than the loop wants to wait.
 * The loop submits the response and goes back to drawing; the thread runs
 * yqst_update() and yqst_next(); the loop reads the next stimulus out of the
 * snapshot when the inter-trial interval ends. The frame budget stops being a
 * hard constraint and becomes a contract the program can check, which is what
 * this program checks and prints.
 *
 * The grid here is Psi-marginal (Prins 2013), 31 stimuli by 61 x 12 x 5 x 5
 * parameters with guess and lapse flagged nuisance: about 1.1 M table cells,
 * which examples/quest/bench.c measures at one to two milliseconds per
 * selection. That fits a frame, so this program also does the opposite
 * measurement: it reports how much of the interval the inference actually used,
 * which is the number that tells you whether a bigger grid would still fit.
 *
 * Nothing here needs hardware, a privilege or a display. The observer is
 * simulated from a fixed seed, so two runs on one machine differ only by the
 * machine's timing.
 *
 * Build (from the repository root):
 *     cc -O2 -pthread -Iinclude -o quest_async examples/quest/async.c -lm
 *     cl /O2 /Iinclude examples\quest\async.c
 *
 * Usage: quest_async [trials] [frames_per_trial]
 *     quest_async          # 60 trials, 30 frames (480 ms) of interval each
 *     quest_async 100 10   # 100 trials, 10 frames of interval
 *
 * Exit code: 0 always. A proposal that was not ready in time is a measurement,
 * not a failure; so is a full queue, which this program reports.
 */
#ifdef YRT_NO_THREADS
/* The async layer IS a thread; with YRT_NO_THREADS there is none to run, and
 * asking ysp/quest.h for YQST_ASYNC anyway is a compile error by design. */
#define YSP_QUEST_IMPLEMENTATION
#include "ysp/quest.h"
#include <stdio.h>
int main(void) {
    puts("quest_async: built with YRT_NO_THREADS, so there is no thread to run.");
    return 0;
}
#else

#define YQST_ASYNC
#define YSP_QUEST_IMPLEMENTATION
#include "ysp/quest.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NS 31
#define NA 61
#define NB 12
#define NG 5
#define NL 5
#define MAX_TRIALS 2000

static double g_stim[NS], g_alpha[NA], g_beta[NB], g_guess[NG], g_lapse[NL];

static void build_axes(void) {
    int i;
    for (i = 0; i < NS; i++) g_stim[i]  = -3.0 + 3.0 * (double)i / (NS - 1);
    for (i = 0; i < NA; i++) g_alpha[i] = -3.0 + 3.0 * (double)i / (NA - 1);
    for (i = 0; i < NB; i++) g_beta[i]  = 0.5 + 5.5 * (double)i / (NB - 1);
    for (i = 0; i < NG; i++) g_guess[i] = 0.45 + 0.10 * (double)i / (NG - 1);
    for (i = 0; i < NL; i++) g_lapse[i] = 0.06 * (double)i / (NL - 1);
}

/* splitmix64, so the observer is reproducible and nothing but libm is needed.
 * ysp/quest.h owns no generator: yqst_simulate() takes the variate. */
static uint64_t g_rng = 0x5EEDF00Dull;

static double next_u(void) {
    uint64_t z = (g_rng += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z = z ^ (z >> 31);
    return (double)(z >> 11) * (1.0 / 9007199254740992.0);
}

/* The simulated observer, written out rather than called through
 * yqst_simulate(): between start() and stop() the quest handle belongs to the
 * thread, and that includes the read-only calls. It is the same log-Weibull the
 * grid assumes, evaluated at the truth. */
static int observe(double x, const double* truth, double u) {
    double f = 1.0 - exp(-pow(10.0, truth[1] * (x - truth[0])));
    double p1 = truth[2] + (1.0 - truth[2] - truth[3]) * f;
    return (u < p1) ? 1 : 0;
}

static yqst_quest g_q;
static yqst_async g_async;

int main(int argc, char** argv) {
    yqst_desc d;
    yqst_async_desc ad;
    yqst_snapshot snap;
    double truth[4], est[4] = {0};
    int trials = (argc > 1) ? (int)strtol(argv[1], NULL, 0) : 60;
    int per_trial = (argc > 2) ? (int)strtol(argv[2], NULL, 0) : 30;
    int64_t period_ns = YRT_MS(16);
    int64_t t, worst_wait_ns = 0, sum_wait_ns = 0;
    int trial, ready_next_frame = 0, ready_by_interval = 0, busy = 0;
    int worst_frames = 0, frame = 0, done_trials = 0;

    /* yqst_async_poll() and yqst_async_wait() fill the snapshot only when they
     * succeed; start it zeroed so a read after a failure is defined. */
    memset(&snap, 0, sizeof(snap));
    if (trials < 1) trials = 1;
    if (trials > MAX_TRIALS) trials = MAX_TRIALS;
    if (per_trial < 1) per_trial = 1;
    build_axes();

    truth[0] = -1.72;   /* threshold, log10 contrast */
    truth[1] = 3.10;    /* slope                     */
    truth[2] = 0.50;    /* 2AFC guess rate           */
    truth[3] = 0.02;    /* lapse rate                */

    memset(&d, 0, sizeof(d));
    d.pf = YQST_PF_GUMBEL;
    d.stim[0] = yqst_values(g_stim, NS);
    d.n_stim = 1;
    d.param[0] = yqst_values(g_alpha, NA);
    d.param[1] = yqst_values(g_beta, NB);
    d.param[2] = yqst_values(g_guess, NG);
    d.param[3] = yqst_values(g_lapse, NL);
    d.param[2].nuisance = true;   /* Psi-marginal: guess and lapse are nuisance */
    d.param[3].nuisance = true;
    d.n_param = 4;
    d.stop_trials = trials;
    if (!yqst_open(&g_q, &d)) {
        fprintf(stderr, "quest_async: %s\n", yqst_error(&g_q));
        return 0;
    }

    /* From here to yqst_async_close() the thread owns the handle: no yqst_*
     * call on it, not even a read. Everything the loop needs is in the
     * snapshot. */
    memset(&g_async, 0, sizeof(g_async));
    memset(&ad, 0, sizeof(ad));
    ad.quest = &g_q;
    ad.estimator = YQST_EST_MEAN;
    ad.below_normal = true;   /* never compete with the frame loop */
    if (!yqst_async_open(&g_async, &ad)) {
        fprintf(stderr, "quest_async: %s\n", yqst_async_error(&g_async));
        yqst_close(&g_q);
        return 0;
    }

    printf("quest_async: %d trials, %d x 16 ms of interval each, queue of %d\n",
           trials, per_trial, YQST_ASYNC_QUEUE);
    printf("             grid %d x (%d x %d x %d x %d), %d table cells, thread policy %s\n",
           NS, NA, NB, NG, NL, NS * NA * NB * NG * NL * 2,
           yrt_policy_name(yqst_async_policy(&g_async)));
    printf("             truth: threshold %.3f, slope %.2f\n\n", truth[0], truth[1]);
    printf("trial      x   k   seq  ready after  threshold     sd  entropy\n");

    /* The proposal for trial 1 was computed by yqst_async_open(), so it is
     * already in the snapshot that a poll returning 0 hands back. */
    if (yqst_async_poll(&g_async, &snap) < 0) {
        fprintf(stderr, "quest_async: nothing published\n");
        yqst_async_close(&g_async);
        yqst_close(&g_q);
        return 0;
    }

    t = yrt_now_ns();
    for (trial = 0; trial < trials; trial++) {
        int s = snap.proposed;
        int k, seq, f, waited_frames = -1;
        int64_t t_submit;

        if (s < 0) { printf("selection failed: %s\n", yqst_strerror(s)); break; }

        /* The trial itself: show the contrast, collect a response. Here the
         * observer answers instantly; in an experiment this is where the frames
         * of stimulus and response go. */
        k = observe(snap.stim[0], truth, next_u());

        /* Hand it over and return to the loop. This copies and returns. */
        t_submit = yrt_now_ns();
        seq = yqst_async_submit(&g_async, s, k);
        if (seq == YQST_ERR_BUSY) {
            /* The honest failure: the queue is full, the response is still
             * ours. A real loop would retry next frame; this one counts it and
             * waits for the thread to catch up, because it has nothing else to
             * do with the trial. */
            busy++;
            yqst_async_wait(&g_async, (uint32_t)snap.seq + 1, YRT_S(2), &snap);
            seq = yqst_async_submit(&g_async, s, k);
        }
        if (seq < 0) { printf("submit failed: %s\n", yqst_strerror(seq)); break; }

        /* The interval: keep drawing frames, and ask once per frame whether the
         * proposal for the next trial has landed. One atomic load and a struct
         * copy; it cannot stall the thread. */
        for (f = 0; f < per_trial; f++) {
            t += period_ns;
            (void)yrt_sleep_until(t, YRT_DEFAULT_SPIN_NS);
            frame++;
            if (waited_frames < 0) {
                int got = yqst_async_poll(&g_async, &snap);
                if (got >= seq) {
                    waited_frames = f + 1;
                    sum_wait_ns += yrt_now_ns() - t_submit;
                    if (yrt_now_ns() - t_submit > worst_wait_ns)
                        worst_wait_ns = yrt_now_ns() - t_submit;
                }
            }
        }
        if (waited_frames < 0) {
            /* Still not ready at the end of the interval: this is the call for
             * the end of an interval, and the loop pays for the rest of it. */
            int got = yqst_async_wait(&g_async, (uint32_t)seq, YRT_S(1), &snap);
            sum_wait_ns += yrt_now_ns() - t_submit;
            if (yrt_now_ns() - t_submit > worst_wait_ns)
                worst_wait_ns = yrt_now_ns() - t_submit;
            if (got < 0) { printf("wait failed: %s\n", yqst_strerror(got)); break; }
            waited_frames = per_trial + 1;
        } else {
            ready_by_interval++;
            if (waited_frames == 1) ready_next_frame++;
        }
        if (waited_frames > worst_frames) worst_frames = waited_frames;
        done_trials++;

        if (trial < 8 || (trial + 1) % 10 == 0)
            printf("%5d %6.3f %3d %5d %7d fr %10.4f %6.4f %8.4f\n",
                   trial + 1, g_stim[s], k, seq, waited_frames,
                   snap.estimate[0], snap.sd, snap.entropy);
        if (snap.done) { printf("stop criterion %d after %d trials\n",
                                (int)snap.stop, snap.n_trials); break; }
    }

    /* Hand the handle back. Every yqst_* call is legal again after this. */
    yqst_async_close(&g_async);

    printf("\n%d trials, %d frames drawn; the proposal for the next trial was ready\n"
           "on the FIRST frame after the response %d times, and somewhere inside the\n"
           "interval %d times; worst case %d frames.\n",
           done_trials, frame, ready_next_frame, ready_by_interval, worst_frames);
    /* Submit to the frame that noticed, which includes the frame the loop spent
     * asleep: an upper bound on the inference, not a measurement of it.
     * examples/quest/bench.c measures the selection itself. */
    printf("submit to the frame that noticed: worst %.2f ms, mean %.2f ms, both of\n"
           "which include the %.0f ms the loop was asleep inside that frame.\n",
           (double)worst_wait_ns / 1e6,
           done_trials ? (double)sum_wait_ns / (double)done_trials / 1e6 : 0.0,
           (double)period_ns / 1e6);
    if (busy) printf("the queue was full %d times (the loop kept the response)\n", busy);
    printf("the frame loop never waited for the inference except where the line\n"
           "above says it did: that is what the async layer buys.\n");

    yqst_estimate(&g_q, YQST_EST_MEAN, est);
    printf("\nfinal: threshold %.4f (truth %.4f, err %+.4f, sd %.4f), slope %.3f\n",
           est[0], truth[0], est[0] - truth[0], yqst_sd(&g_q, 0), est[1]);
    printf("       %d trials recorded, entropy %.4f bits\n",
           yqst_n_trials(&g_q), yqst_entropy(&g_q));
    yqst_close(&g_q);
    return 0;
}
#endif /* YRT_NO_THREADS */
