/* sig_bench.c - what ysp/sig.h costs per sample and per frame on this
 * machine (docs/signal_spec.md section 9).
 *
 * Four raw mice at 8 kHz each (DELTA samples, interleaved by time, each
 * mouse 31.25 us after the one before) into a 32-node graph: the
 * visuomotor and bimanual graphs side by side, widened. Per mouse: a
 * source, a bounded integrator (px), an affine to cm with a ring of 4096
 * past values. Then a split mapping (two selects and a join) and a mean,
 * each turned by an affine, their speeds, a second-order low-pass and a
 * velocity, two emits (derived events), a sample record, a sum of 7 + 7
 * sines times a timeline channel, the tracking error, a frame-rate inside
 * detect, a sample-rate distance detect, two binds and a frame record.
 * A 60 Hz frame loop feeds each frame's 533 samples, drains the derived
 * events and hits, reads two values at the frame's onset (as for a flip
 * record), and runs ysig_frame().
 *
 * Reported: ns per sample (a frame's feed calls over its samples: mean,
 * median and p99 of the frames), ysig_frame() per call (median, p99, max),
 * the value reads, and the allocator calls during the run (glibc: this
 * program's malloc() counts them; elsewhere not counted). The run is also
 * journaled for its first second and replayed into a second graph: the
 * digests must agree (exit 1 if not). Times are ysp/rt.h clock reads
 * around each batch. Run it on a quiet machine, nothing else measuring;
 * CI runs --quick to check it works.
 *
 * Build (from the repository root):
 *     cc -O2 -Iinclude -o sig_bench examples/sig/bench.c -lm
 *
 * Usage: sig_bench [--quick] [--seconds S]   (default 60 s of input)
 * Exit code: 0, 1 if a call failed or the replay differs, 2 for a bad
 * argument.
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#define YRT_NO_THREADS
#define YSP_RT_IMPLEMENTATION
#include "ysp/rt.h"
#define YSP_SIG_IMPLEMENTATION
#include "ysp/sig.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Counts this program's allocator calls during the timed loop: glibc lets
 * a program's malloc() replace libc's, and __libc_malloc is the real one. */
static volatile int g_counting;
static long g_allocs;
#if defined(__GLIBC__) && !defined(__SANITIZE_ADDRESS__)
#define SIG_BENCH_COUNT_ALLOCS 1
extern void* __libc_malloc(size_t);
extern void* __libc_calloc(size_t, size_t);
extern void* __libc_realloc(void*, size_t);
void* malloc(size_t n) { if (g_counting) g_allocs++; return __libc_malloc(n); }
void* calloc(size_t a, size_t b) { if (g_counting) g_allocs++; return __libc_calloc(a, b); }
void* realloc(void* p, size_t n) { if (g_counting) g_allocs++; return __libc_realloc(p, n); }
#else
#define SIG_BENCH_COUNT_ALLOCS 0
#endif

#define RATE_HZ 8000
#define N_MICE  4
#define P_NS    16666667LL

enum { S0, S1, S2, S3, I0, I1, I2, I3, H0, H1, H2, H3, LX, RY, U1, U2, C1, C2, SP1, SP2, LP, VEL, EM1, EM2, REC,
       SINES, AMP, TGT, ERR, INSIDE, AWAY, BIND, N_NODES };

static ysig_node g_n[N_NODES];
static double g_sines[2 * 7 * 3];
static float g_ch[8];
static long g_rows;
static void on_row(void* user, const ysig_row* r) { (void)user; (void)r; g_rows++; }

#define J_CAP 40000
static ysig_entry* g_j;
static float g_jv[J_CAP][1];
static int g_nj, g_journal_on;
static uint64_t g_digest_live;
static int64_t g_rows_live;
static void on_journal(void* user, const ysig_entry* e) {
    (void)user;
    if (!g_journal_on || g_nj >= J_CAP) return;
    g_j[g_nj] = *e;
    if (e->kind == YSIG_J_FRAME && e->frame.n_values) {
        g_jv[g_nj][0] = e->frame.values[0];
        g_j[g_nj].frame.values = g_jv[g_nj];
        g_j[g_nj].frame.n_values = 1;
    }
    g_nj++;
}

static void build(void) {
    ysig_node* n = g_n;
    int k, a;
    for (k = 0; k < N_MICE; k++) {
        n[S0 + k] = ysig_node_of(YSIG_OP_SOURCE, -1, -1);
        n[S0 + k].lanes = 2;
        n[S0 + k].p[0] = YIN_KIND_MOUSE;
        n[S0 + k].p[1] = YIN_AXIS_DELTA;
        n[S0 + k].p[2] = 101 + k;
        n[I0 + k] = ysig_node_of(YSIG_OP_INTEGRATE, S0 + k, -1);
        n[I0 + k].p[0] = n[I0 + k].p[1] = 0.25;
        n[I0 + k].p[4] = 1024;
        n[I0 + k].p[5] = 768;
        n[I0 + k].p[12] = 2048;
        n[I0 + k].p[13] = 1536;
        n[H0 + k] = ysig_node_of(YSIG_OP_AFFINE, I0 + k, -1);
        n[H0 + k].p[0] = 1 / 32.0;
        n[H0 + k].p[3] = -1 / 32.0;
        n[H0 + k].p[4] = 1024;
        n[H0 + k].p[5] = 768;
        n[H0 + k].history = 4096;
    }
    n[LX] = ysig_node_of(YSIG_OP_SELECT, H0, -1);
    n[LX].lanes = 1;
    n[RY] = ysig_node_of(YSIG_OP_SELECT, H1, -1);
    n[RY].lanes = 1;
    n[RY].p[0] = 1;
    n[U1] = ysig_node_of(YSIG_OP_JOIN, LX, RY);
    n[U2] = ysig_node_of(YSIG_OP_MEAN, H2, H3);
    n[C1] = ysig_node_of(YSIG_OP_AFFINE, U1, -1);
    n[C1].p[8] = -90;
    n[C1].history = 4096;
    n[C2] = ysig_node_of(YSIG_OP_AFFINE, U2, -1);
    n[C2].p[1] = n[C2].p[2] = 1;
    n[SP1] = ysig_node_of(YSIG_OP_SPEED, C1, -1);
    n[SP1].p[0] = 0.010;
    n[SP2] = ysig_node_of(YSIG_OP_SPEED, C2, -1);
    n[SP2].p[0] = 0.010;
    n[LP] = ysig_node_of(YSIG_OP_LOWPASS, C1, -1);
    n[LP].p[0] = 30;
    n[LP].p[1] = 2;
    n[VEL] = ysig_node_of(YSIG_OP_VELOCITY, C2, -1);
    n[VEL].p[0] = 0.010;
    n[EM1] = ysig_node_of(YSIG_OP_EMIT, C1, -1);
    n[EM2] = ysig_node_of(YSIG_OP_EMIT, C2, -1);
    n[REC] = ysig_node_of(YSIG_OP_RECORD, LP, -1);
    n[REC].name = "filtered";
    for (a = 0; a < 2; a++)
        for (k = 0; k < 7; k++) {
            g_sines[(a * 7 + k) * 3] = 2.0 - 0.2 * k;
            g_sines[(a * 7 + k) * 3 + 1] = 0.1 + 0.3 * k + 0.05 * a;
            g_sines[(a * 7 + k) * 3 + 2] = 0.5 * k - a;
        }
    n[SINES] = ysig_node_of(YSIG_OP_SINES, -1, -1);
    n[SINES].lanes = 2;
    n[SINES].p[0] = 7;
    n[SINES].data = g_sines;
    n[SINES].n_data = 42;
    n[AMP] = ysig_node_of(YSIG_OP_CHANNEL, -1, -1);
    n[TGT] = ysig_node_of(YSIG_OP_SCALE, SINES, AMP);
    n[ERR] = ysig_node_of(YSIG_OP_DIFF, C1, TGT);
    n[INSIDE] = ysig_node_of(YSIG_OP_DETECT, ERR, -1);
    n[INSIDE].p[0] = YSIG_TEST_LE;
    n[INSIDE].p[1] = -1;
    n[INSIDE].p[2] = 0.8;
    n[INSIDE].p[5] = 1;
    n[INSIDE].p[7] = 1;
    n[AWAY] = ysig_node_of(YSIG_OP_DETECT, C2, -1);
    n[AWAY].p[0] = YSIG_TEST_GE;
    n[AWAY].p[1] = -1;
    n[AWAY].p[2] = 1;
    n[AWAY].p[3] = 0.1;
    n[AWAY].p[7] = 1;
    n[BIND] = ysig_node_of(YSIG_OP_BIND, C1, -1);
}

/* Mouse k's counts at sample j: each hand on its own slow circle. */
static void counts(int k, int64_t j, int64_t* kx, int64_t* ky) {
    double t = (double)j / RATE_HZ, a = 2 * 3.14159265358979 * (0.3 + 0.1 * k) * t;
    *kx = (int64_t)lround(400 * cos(a) + 50 * sin(7.3 * t + k));
    *ky = (int64_t)lround(400 * sin(a));
}

static int cmp_i64(const void* a, const void* b) {
    int64_t x = *(const int64_t*)a, y = *(const int64_t*)b;
    return x < y ? -1 : x > y;
}

static uint64_t g_arena_a[(8u << 20) / 8], g_arena_b[(8u << 20) / 8];

int main(int argc, char** argv) {
    ysig_desc d;
    ysig_graph g, g2;
    ysig_stats st, st2;
    double seconds = 60;
    int quick = 0, i, failed = 0;
    int64_t n_frames, f, *t_feed, *t_frame, *t_read, n_samples = 0, last[N_MICE][2];
    yin_event der[64];
    ysig_hit hits[64];
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--quick")) quick = 1;
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atof(argv[++i]);
        else { fprintf(stderr, "usage: sig_bench [--quick] [--seconds S]\n"); return 2; }
    }
    if (quick) seconds = 2;
    if (!(seconds >= 1 && seconds <= 3600)) { fprintf(stderr, "sig_bench: --seconds in 1..3600\n"); return 2; }
    build();
    memset(&d, 0, sizeof d);
    d.nodes = g_n;
    d.n_nodes = N_NODES;
    d.channels = g_ch;
    d.n_channels = 8;
    d.on_row = on_row;
    d.journal = on_journal;
    d.arena = g_arena_a;
    d.arena_size = sizeof g_arena_a;
    if (!ysig_open(&g, &d)) { fprintf(stderr, "sig_bench: %s\n", ysig_error(&g)); return 1; }
    g_j = (ysig_entry*)malloc(sizeof(ysig_entry) * J_CAP);
    n_frames = (int64_t)(seconds * 60);
    t_feed = (int64_t*)malloc(sizeof(int64_t) * (size_t)n_frames * 3);
    if (!g_j || !t_feed) return 1;
    t_frame = t_feed + n_frames;
    t_read = t_frame + n_frames;
    memset(last, 0, sizeof last);
    printf("ysp/sig.h %s: %d nodes, %d sources at %d Hz, %.0f s of input, arena %lu bytes (of %lu)\n",
           ysig_version(), g.n, N_MICE, RATE_HZ, seconds, (unsigned long)ysig_arena_size(&d),
           (unsigned long)sizeof g_arena_a);
    g_journal_on = 1;
    ysig_reset(&g, 0);
    g_counting = 1;
    for (f = 0; f < n_frames; f++) {
        int64_t onset = (f + 1) * P_NS, t0, t1, t2, t3, j0 = f * P_NS * RATE_HZ / 1000000000, j1 = (f + 1) * P_NS * RATE_HZ / 1000000000, j;
        int nf = 0, k;
        float amp = (float)(f < 300 ? (double)f / 300 : 1.0);
        ysig_frame_in fi;
        ysig_value v;
        static yin_event batch[N_MICE * 200];
        /* the frame's samples, made before the clock starts */
        for (j = j0; j < j1; j++)
            for (k = 0; k < N_MICE; k++) {
                yin_event* e = &batch[nf++];
                int64_t kx, ky;
                counts(k, j, &kx, &ky);
                memset(e, 0, sizeof *e);
                e->t = j * (1000000000 / RATE_HZ) + k * (1000000000 / RATE_HZ / N_MICE);
                e->kind = YIN_KIND_MOUSE;
                e->type = YIN_SAMPLE;
                e->control = YIN_AXIS_DELTA;
                e->device = (uint32_t)(101 + k);
                e->x = (float)(kx - last[k][0]);
                e->y = (float)(ky - last[k][1]);
                last[k][0] = kx;
                last[k][1] = ky;
            }
        if (f == 60) {
            /* the journal's end: the live graph's digest here */
            ysig_stats s60;
            ysig_get_stats(&g, &s60);
            g_digest_live = s60.digest;
            g_rows_live = s60.rows;
            g_journal_on = 0;
        }
        t0 = yrt_now_ns();
        /* a program drains after each event, as it feeds the collector */
        for (k = 0; k < nf; k++) {
            if (ysig_feed(&g, &batch[k]) != 1) failed = 1;
            while (ysig_derived(&g, der, 64) > 0) { }
            while (ysig_detected(&g, hits, 64) > 0) { }
        }
        t1 = yrt_now_ns();
        /* the values at the last frame's onset, as a flip record asks */
        ysig_value_at(&g, H0, onset - P_NS, &v);
        ysig_value_at(&g, C1, onset - P_NS, &v);
        t2 = yrt_now_ns();
        memset(&fi, 0, sizeof fi);
        fi.index = f;
        fi.onset_ns = onset;
        fi.base_ns = f * P_NS;
        fi.values = &amp;
        fi.n_values = 1;
        fi.poll_ns = onset;   /* the frame's samples run up to its onset */
        fi.has_poll = true;
        if (ysig_frame(&g, &fi) != 0) failed = 1;
        while (ysig_detected(&g, hits, 64) > 0) { }
        t3 = yrt_now_ns();
        t_feed[f] = (t1 - t0) * 1000 / (nf ? nf : 1);   /* ps per sample, for the median */
        t_read[f] = t2 - t1;
        t_frame[f] = t3 - t2;
        n_samples += nf;
    }
    g_counting = 0;
    ysig_get_stats(&g, &st);
    {
        double mean = 0;
        for (f = 0; f < n_frames; f++) mean += (double)t_feed[f];
        mean /= (double)n_frames;
        qsort(t_feed, (size_t)n_frames, sizeof *t_feed, cmp_i64);
        qsort(t_frame, (size_t)n_frames, sizeof *t_frame, cmp_i64);
        qsort(t_read, (size_t)n_frames, sizeof *t_read, cmp_i64);
        printf("%lld samples in %lld frames (%lld a frame); %lld rows, %lld hits, %lld derived events lost\n",
               (long long)n_samples, (long long)n_frames, (long long)(n_samples / n_frames), (long long)st.rows,
               (long long)st.hits, (long long)st.derived_lost);
        printf("per sample (a frame's feeds and drains / its samples): mean %.1f ns, median %.1f ns, p99 %.1f ns\n",
               mean / 1000, (double)t_feed[n_frames / 2] / 1000, (double)t_feed[n_frames * 99 / 100] / 1000);
        printf("per frame, ysig_frame(): median %lld ns, p99 %lld ns, max %lld ns\n", (long long)t_frame[n_frames / 2],
               (long long)t_frame[n_frames * 99 / 100], (long long)t_frame[n_frames - 1]);
        printf("two ysig_value_at() reads (one ring, one computed from four rings): median %lld ns, p99 %lld ns\n",
               (long long)t_read[n_frames / 2], (long long)t_read[n_frames * 99 / 100]);
        printf("a frame's whole graph work at %.0f samples: %.1f us at the median\n", (double)(n_samples / n_frames),
               ((double)t_feed[n_frames / 2] / 1000 * (double)(n_samples / n_frames) + (double)t_frame[n_frames / 2] +
                (double)t_read[n_frames / 2]) / 1000);
    }
    if (SIG_BENCH_COUNT_ALLOCS) printf("allocator calls during the run: %ld\n", g_allocs);
    else printf("allocator calls during the run: not counted on this platform\n");
    /* the first second, replayed into a new graph: the same rows and hits,
     * bit for bit, as the live run's at that point */
    d.journal = NULL;
    d.arena = g_arena_b;
    if (!ysig_open(&g2, &d)) { fprintf(stderr, "sig_bench: %s\n", ysig_error(&g2)); return 1; }
    if (ysig_replay(&g2, g_j, g_nj) != g_nj) failed = 1;
    ysig_get_stats(&g2, &st2);
    printf("replay of the first second (%d journal entries): digest %016llx, live %016llx; %lld rows, live %lld: %s\n",
           g_nj, (unsigned long long)st2.digest, (unsigned long long)g_digest_live, (long long)st2.rows,
           (long long)g_rows_live, st2.digest == g_digest_live && st2.rows == g_rows_live && g_rows_live > 0 ? "equal" : "DIFFERENT");
    if (st2.digest != g_digest_live || st2.rows != g_rows_live || g_rows_live == 0) failed = 1;
    free(g_j);
    free(t_feed);
    return failed;
}
