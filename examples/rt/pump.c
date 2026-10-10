/* rt_pump.c - move a too-slow computation off the frame loop with yrt_pump.
 *
 * The shape docs/adapt.md calls for in "Inference on a thread": a trial
 * loop that must not miss a frame, and an inference step that sometimes does
 * not fit in one. The loop submits the trial's outcome and goes back to
 * drawing; the pump runs the inference; the loop asks "is the result ready?"
 * on the next frame and counts how often the answer was yes.
 *
 * The inference here is fake: it sleeps a pseudo-random 1 to 30 ms, which
 * straddles the 16 ms frame on purpose, so the output shows both outcomes.
 * The seed is fixed, so two runs on one machine differ only by the machine.
 * on_idle stands in for yaep_fit_step: one step of background fitting per
 * call, while nothing is queued.
 *
 * Nothing here needs hardware, a privilege or a display.
 *
 * Build (from the repository root):
 *     cc -O2 -pthread -Iinclude -o rt_pump examples/rt/pump.c     # Linux / macOS
 *     cl /O2 /Iinclude examples\rt\pump.c                         # Windows (MSVC)
 *     emcc -O2 -pthread -sPROXY_TO_PTHREAD=1 -sEXIT_RUNTIME=1 -Iinclude \
 *          -o rt_pump.js examples/rt/pump.c && node rt_pump.js    # wasm
 * or:  cmake -B build && cmake --build build
 *
 * A wasm build without -pthread compiles, cannot start the pump, says so and
 * exits 0. In a browser the frame loop would be requestAnimationFrame rather
 * than yrt_sleep_until(); the pump side is the same. See WEBASSEMBLY in
 * ysp/rt.h.
 *
 * Usage: rt_pump [frames] [frame_ms]
 *     rt_pump           # 120 frames of 16 ms, about 2 seconds
 *     rt_pump 300 10    # 300 frames of 10 ms
 *
 * Exit code: 0 always. A late result is a measurement, not a failure; so is a
 * full ring, which this program counts and reports rather than retrying,
 * because a trial loop cannot wait.
 */
#define YSP_RT_IMPLEMENTATION
#include "ysp/rt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef YRT_NO_THREADS
int main(void) {
    puts("rt_pump: built with YRT_NO_THREADS, so there is no pump to run.");
    return 0;
}
#else

#define MAX_FRAMES 5000

/* One trial's outcome, the message the frame loop hands the pump. Small and
 * flat: every submit copies exactly this. */
typedef struct trial_msg {
    int      frame;
    uint32_t work_ns_k;   /* how long the fake inference will take, in us */
} trial_msg;

/* What the pump publishes and the frame loop reads, under the pump's lock.
 * In a real experiment this is the next stimulus level and a posterior
 * summary. */
typedef struct published {
    int      last_frame;   /* the frame whose outcome produced this        */
    uint32_t last_seq;
    double   level;        /* "the next stimulus level"                   */
    int      fit_steps;    /* how much background fitting has been done   */
    bool     fit_done;
} published;

typedef struct pump_ctx {
    yrt_pump* pump;
    published   pub;       /* guarded by yrt_pump_lock()                */
    uint64_t    worst_ns;  /* pump thread only, read after the join       */
    uint64_t    total_ns;
    int         messages;
} pump_ctx;

/* The measurement arrays are static and fixed: indexing them by seq costs
 * nothing, and a heap block would only add a page fault to a timed loop. */
static int g_sub_frame[MAX_FRAMES];
static int g_done_frame[MAX_FRAMES];

/* xorshift32, so the work times are reproducible and the program depends on
 * nothing. rand() would pull in a global the pump thread shares with main. */
static uint32_t xs32(uint32_t* s) {
    uint32_t x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *s = x;
    return x;
}

/* The inference. Runs on the pump thread with neither lock held, takes the
 * lock only for the publish at the end: that is the discipline ysp/rt.h's PUMP
 * section asks for, and it is why the frame loop can hold the lock for as long
 * as it likes without slowing this down. */
static void on_trial(void* ctx, const void* msg, uint32_t seq) {
    pump_ctx* k = (pump_ctx*)ctx;
    const trial_msg* m = (const trial_msg*)msg;
    uint64_t t0 = yrt_now_ns();
    uint64_t took;
    /* Stand-in for yqst_update + yqst_next, or a GP refit. A sleep rather than
     * a spin so the numbers are about the pump, not about a busy core. */
    (void)yrt_sleep_until(t0 + (int64_t)m->work_ns_k * 1000, 0);
    took = yrt_now_ns() - t0;
    if (took > k->worst_ns) k->worst_ns = took;
    k->total_ns += took;
    k->messages++;

    yrt_pump_lock(k->pump);
    k->pub.last_frame = m->frame;
    k->pub.last_seq   = seq;
    k->pub.level      = 0.5 + 0.001 * (double)m->frame;
    yrt_pump_unlock(k->pump);
}

/* One step of a background fit, whenever the ring is empty. Returning true
 * asks for another call; returning false lets the thread block until the next
 * submit, which is what a finished fit does. */
static bool on_fit_step(void* ctx) {
    pump_ctx* k = (pump_ctx*)ctx;
    bool more;
    yrt_pump_lock(k->pump);
    more = !k->pub.fit_done;
    yrt_pump_unlock(k->pump);
    /* Converged: say so and let the thread block, instead of being called in a
     * loop for the rest of the session. */
    if (!more) return false;
    /* A step, not a fit: the manual asks on_idle to return in a few ms so a
     * queued message is never stuck behind it. */
    (void)yrt_sleep_until(yrt_now_ns() + 500000ull, 0);
    yrt_pump_lock(k->pump);
    k->pub.fit_steps++;
    k->pub.fit_done = k->pub.fit_steps >= 200;
    more = !k->pub.fit_done;
    yrt_pump_unlock(k->pump);
    return more;
}

int main(int argc, char** argv) {
    yrt_pump pump;
    yrt_pump_desc d;
    pump_ctx k;
    yrt_report rep;
    char line[256];
    uint32_t rng = 0x5eed1234u;
    uint32_t next_stamp = 1u;
    int frames = (argc > 1) ? (int)strtol(argv[1], NULL, 0) : 120;
    uint64_t period_ns = (argc > 2)
        ? (uint64_t)strtoul(argv[2], NULL, 0) * 1000000ull
        : 16000000ull;
    int f, i, submitted = 0, full = 0, tail;
    int ready = 0, late = 0, worst_lat = 0, saw_level = 0;
    uint64_t worst_wake = 0, t;
    published snap;

    memset(&snap, 0, sizeof(snap));
    if (frames < 2) frames = 2;
    if (frames > MAX_FRAMES) frames = MAX_FRAMES;
    if (period_ns < 1000000ull) period_ns = 1000000ull;

    memset(&k, 0, sizeof(k));
    k.pump = &pump;
    memset(&pump, 0, sizeof(pump));
    memset(&d, 0, sizeof(d));
    d.msg_size = sizeof(trial_msg);
    d.capacity = 8;          /* eight trials of slack, and no heap: 8 x 8
                              * bytes fits the handle's inline ring */
    d.on_msg   = on_trial;
    d.on_idle  = on_fit_step;
    d.ctx      = &k;
    if (!yrt_pump_open(&pump, &d)) {
        fprintf(stderr, "rt_pump: pump start failed: %s\n", yrt_pump_error(&pump));
        return 0;            /* documented: this program always exits 0 */
    }

    /* The frame loop elevates itself; the pump deliberately did not. Print the
     * pair, because "the result was ready" means something different at each
     * rung. */
    yrt_report_get(&rep, yrt_thread_elevate(NULL));
    yrt_describe(&rep, line, sizeof(line));
    puts(line);
    printf("ysp_rt %s, pump policy: %s (it never climbs the ladder; see PUMP)\n",
           yrt_version(), yrt_policy_name(yrt_pump_policy(&pump)));
    printf("%d frames of %.1f ms, inference 1 to 30 ms, ring of %u\n\n",
           frames, (double)period_ns / 1e6, d.capacity);

    t = yrt_now_ns();
    for (f = 0; f < frames; f++) {
        trial_msg m;
        uint32_t done;
        uint64_t wake;
        int rc;

        t += period_ns;
        wake = yrt_sleep_until(t, YRT_DEFAULT_SPIN_NS);
        if (wake > worst_wake) worst_wake = wake;

        /* The whole point: one atomic load per frame tells the loop which
         * results have landed. Everything up to done_seq finished before this
         * frame started. */
        done = yrt_pump_done_seq(&pump);
        while (next_stamp <= done) { g_done_frame[next_stamp - 1] = f; next_stamp++; }

        /* Read the publication under the lock, as a trial loop would to pick
         * the next level. Holding this lock cannot stall the inference. */
        yrt_pump_lock(&pump);
        snap = k.pub;
        yrt_pump_unlock(&pump);
        if (snap.last_seq != 0u) saw_level++;

        m.frame = f;
        m.work_ns_k = 1000u + xs32(&rng) % 29000u;   /* 1 to 30 ms, in us */
        rc = yrt_pump_submit(&pump, &m);
        if (rc == YRT_ERR_FULL) {
            /* The honest failure: the pump is behind and the loop keeps its
             * message rather than growing an invisible queue. */
            full++;
        } else if (rc < 0) {
            fprintf(stderr, "rt_pump: submit: %s\n", yrt_strerror(rc));
            break;
        } else {
            g_sub_frame[rc - 1] = f;
            submitted++;
        }
    }

    /* Keep the frame clock running while the tail drains, so the last few
     * messages get a latency in frames like every other one. */
    for (tail = 0; tail < 200 && (int)next_stamp <= submitted; tail++) {
        uint32_t done;
        t += period_ns;
        (void)yrt_sleep_until(t, YRT_DEFAULT_SPIN_NS);
        done = yrt_pump_done_seq(&pump);
        while (next_stamp <= done) { g_done_frame[next_stamp - 1] = frames + tail; next_stamp++; }
    }
    /* The stop drains, so every accepted message did run; anything the tail
     * loop did not see landed after the last frame it watched. */
    yrt_pump_close(&pump);
    while ((int)next_stamp <= submitted) {
        g_done_frame[next_stamp - 1] = frames + tail;
        next_stamp++;
    }

    /* "Ready by the next frame" is latency <= 1 frame, because a result that
     * lands during frame f is first SEEN by the poll at the top of frame f+1.
     * That is what the frame loop can act on, so it is what gets counted. */
    for (i = 0; i < submitted; i++) {
        int lat = g_done_frame[i] - g_sub_frame[i];
        if (lat <= 1) ready++;
        else {
            late++;
            if (lat > worst_lat) worst_lat = lat;
        }
    }

    printf("frames            %d\n", frames);
    printf("submitted         %d\n", submitted);
    printf("ring full         %d  (message kept by the caller)\n", full);
    printf("ready next frame  %d\n", ready);
    printf("late              %d  (worst %d frames behind)\n", late, worst_lat);
    printf("background fit    %d steps%s\n", k.pub.fit_steps,
           k.pub.fit_done ? ", finished" : "");
    printf("inference mean    %.1f ms of a %.1f ms frame\n",
           k.messages ? (double)k.total_ns / (double)k.messages / 1e6 : 0.0,
           (double)period_ns / 1e6);
    printf("inference worst   %.1f ms\n", (double)k.worst_ns / 1e6);
    printf("frame wake worst  %.3f ms late\n", (double)worst_wake / 1e6);
    printf("messages run      %d\n", k.messages);
    printf("frames with a level to read  %d\n", saw_level);
    printf("last level        %.3f from frame %d\n", snap.level, snap.last_frame);
    /* The mean above is the whole story of the two counts: one job per frame
     * whose mean is close to the frame is a queue that grows, and a pump turns
     * that into a latency the loop can SEE rather than a missed frame. Halve
     * the work (rt_pump 120 32 doubles the frame instead) and "late" goes to
     * nearly zero. */

    yrt_thread_cleanup();
    return 0;
}

#endif /* YRT_NO_THREADS */
