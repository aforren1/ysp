/* bench_targets.c - the cost of target tweens per frame, against the value
 * array and a binding copy.
 *
 * ysp/timeline.h v0.6.0 lets a tween write a field of the caller (TARGETS).
 * Before, a tween wrote a channel of the value array and the caller copied
 * the array into its stimuli on every frame with a binding table
 * (ygfx_apply(binds, n, ytl_values(&tl))). This program times both paths
 * for 0, 1, 100 and 1000 animated fields, each a float in a 256-byte
 * stimulus, so that a write touches its own cache line as a stimulus field
 * does:
 *   targets  N target tweens (ytl_tween_to), one per field
 *   channels N channel tweens (ytl_tween), then the binding copy: the loop
 *            of ygfx_apply() for .field bindings, its check pass included
 *   held     the targets after their tweens ended (the field still owned)
 * Every tween is CUBIC_OUT over 1000 s, so it is in progress on every
 * frame. A sample is 64 evaluates between two clock reads, so the 100 ns
 * clock of Windows resolves a frame; the program prints the median and the
 * minimum of 400 samples, per frame and per field above the empty frame.
 *
 * Build (from the repository root):
 *     cc -O2 -Iinclude -o timeline_bench_targets examples/timeline/bench_targets.c -lm
 *     cl /O2 /Iinclude examples\timeline\bench_targets.c
 *
 * Usage: timeline_bench_targets [rounds]   (default 5; the configurations
 * run interleaved, round after round, and the minimum of the rounds'
 * medians is printed)
 * Exit code: 0, or 1 if a call failed.
 */
#define YRT_NO_THREADS
#define YSP_RT_IMPLEMENTATION
#include "ysp/rt.h"

/* The value-array path needs a channel per field. */
#define YTL_MAX_CHANNELS 1024
#define YSP_TIMELINE_IMPLEMENTATION
#include "ysp/timeline.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N_MAX 1000
#define BATCH 64
#define SAMPLES 400
#define PERIOD INT64_C(16666667)

typedef struct stim { float f[64]; } stim;   /* 256 bytes */

/* ygfx_bind's layout and ygfx_apply()'s loop for .field bindings, copied so
 * that this program needs no GL. */
typedef struct bind {
    void*    stim;
    uint16_t param, channel;
    void*    group;
    float*   field;
} bind;

static int apply(const bind* b, int n, const float* values) {
    int i;
    if ((!b && n > 0) || n < 0 || !values) return -1;
    for (i = 0; i < n; i++) {
        if (b[i].field) continue;
        return -1;
    }
    for (i = 0; i < n; i++) *b[i].field = values[b[i].channel];
    return n;
}

static stim g_stims[N_MAX];
static bind g_binds[N_MAX];
static ytl_event g_events[16];
static ytl_target_slot g_targets[N_MAX];
static ytl_tween_slot g_tweens[N_MAX];
static ytl_timeline g_tl;
static int64_t g_k;
static double g_sink;

enum { TARGETS, CHANNELS, HELD, N_MODES };
static const char* const g_mode_name[N_MODES] = { "targets", "channels", "held" };

static int setup(int mode, int n) {
    ytl_desc d;
    ytl_seq q;
    int i;
    memset(&d, 0, sizeof d);
    d.events = g_events;
    d.event_capacity = 16;
    d.n_channels = mode == CHANNELS ? n : 0;
    d.targets = g_targets;
    d.target_capacity = N_MAX;
    d.tweens = g_tweens;
    d.tween_capacity = N_MAX;
    if (!ytl_open(&g_tl, &d)) return -1;
    q = ytl_seq_on(&g_tl, 1);
    for (i = 0; i < n; i++) {
        g_stims[i].f[3] = (float)i;
        if (mode == CHANNELS) {
            ytl_tween_desc t;
            memset(&t, 0, sizeof t);
            t.to = 1000.0f;
            t.duration_ns = YRT_S(1000);
            t.ease = YTL_EASE_CUBIC_OUT;
            t.start_set = true;
            if (ytl_tween(&g_tl, i, 1, &t) < 0) return -1;
            memset(&g_binds[i], 0, sizeof g_binds[i]);
            g_binds[i].channel = (uint16_t)i;
            g_binds[i].field = &g_stims[i].f[3];
        } else {
            ytl_tween_to(&q, &g_stims[i].f[3], 1000.0f, mode == HELD ? YRT_MS(1) : YRT_S(1000),
                         YTL_EASE_CUBIC_OUT);
        }
    }
    if (q.err < 0) return -1;
    g_k = 0;
    return ytl_anchor(&g_tl, 1, PERIOD, 0);
}

/* One frame of the mode: the evaluate, and the copy for the value array. */
static int frame(int mode, int n) {
    ytl_frame f;
    f.onset = PERIOD * (++g_k + 1);
    f.period = PERIOD;
    f.index = g_k;
    if (ytl_evaluate(&g_tl, &f, NULL, 0) < 0) return -1;
    if (mode == CHANNELS && apply(g_binds, n, ytl_values(&g_tl)) != n) return -1;
    return 0;
}

static int cmp_d(const void* a, const void* b) {
    double x = *(const double*)a, y = *(const double*)b;
    return x < y ? -1 : x > y;
}

/* The median and the minimum, in ns per frame, of SAMPLES batches. */
static int run(int mode, int n, double* med, double* mn) {
    static double s[SAMPLES];
    int i, j;
    if (setup(mode, n) < 0) return -1;
    for (i = 0; i < 200; i++)
        if (frame(mode, n) < 0) return -1;   /* warm the caches and the cursor */
    for (i = 0; i < SAMPLES; i++) {
        int64_t t0 = yrt_now_ns(), t1;
        for (j = 0; j < BATCH; j++)
            if (frame(mode, n) < 0) return -1;
        t1 = yrt_now_ns();
        s[i] = (double)(t1 - t0) / BATCH;
    }
    for (i = 0; i < n; i++) g_sink += g_stims[i].f[3];
    qsort(s, SAMPLES, sizeof s[0], cmp_d);
    *med = s[SAMPLES / 2];
    *mn = s[0];
    return 0;
}

int main(int argc, char** argv) {
    static const int ns[4] = { 0, 1, 100, 1000 };
    double med[N_MODES][4], mn[N_MODES][4];
    int rounds = argc > 1 ? atoi(argv[1]) : 5, r, m, i;
    if (rounds < 1 || rounds > 100) {
        fprintf(stderr, "usage: timeline_bench_targets [rounds 1..100]\n");
        return 1;
    }
    for (m = 0; m < N_MODES; m++)
        for (i = 0; i < 4; i++) med[m][i] = mn[m][i] = 1e30;
    for (r = 0; r < rounds; r++) {
        for (i = 0; i < 4; i++) {
            for (m = 0; m < N_MODES; m++) {
                double a, b;
                if (run(m, ns[i], &a, &b) < 0) {
                    fprintf(stderr, "timeline_bench_targets: %s\n", ytl_error(&g_tl));
                    return 1;
                }
                if (a < med[m][i]) med[m][i] = a;
                if (b < mn[m][i]) mn[m][i] = b;
            }
        }
    }
    printf("ysp/timeline.h %s, %d rounds of %d samples of %d frames; ns per frame, the "
           "minimum over rounds of the median (and of the minimum)\n",
           ytl_version(), rounds, SAMPLES, BATCH);
    printf("%-9s %18s %18s %18s %18s   per field above 0, at 1000\n", "fields", "0", "1", "100", "1000");
    for (m = 0; m < N_MODES; m++) {
        printf("%-9s", g_mode_name[m]);
        for (i = 0; i < 4; i++) printf("   %7.1f (%7.1f)", med[m][i], mn[m][i]);
        printf("   %.2f ns\n", (med[m][3] - med[m][0]) / 1000.0);
    }
    printf("fields in 0.5 ms: targets %.0f, channels %.0f (from the 1000-field rows)\n",
           5e5 / ((med[TARGETS][3] - med[TARGETS][0]) / 1000.0),
           5e5 / ((med[CHANNELS][3] - med[CHANNELS][0]) / 1000.0));
    return g_sink != g_sink;   /* keeps the writes; NaN only if something broke */
}
