/* timeline_trial.c - one trial's timeline on a simulated 60 Hz display.
 *
 * The USAGE example of ysp/timeline.h, run for real: a fixation point from
 * 0 to 0.5 s, then a grating whose contrast ramps up over 100 ms with a
 * raised cosine, holds at 0.5 and ramps down to end at 1.2 s, and a trigger
 * at the grating's onset. The trial runs twice from the same events: the
 * second run anchors the trial base at base time 0 again, which rewinds it.
 *
 * The display is simulated: frame k has its onset at k / 60 s on a grid,
 * and the second trial starts a third of a frame off that grid, so its
 * events land with a residual. Frame 30 of the simulated loop, the grating onset, is dropped
 * (the next onset is a frame later), so whatever was due on it lands late
 * on the frame after, and the residual says by how much.
 *
 * Prints one line per fired event: frame, predicted onset, kind, channel or
 * code, the residual in ms and whether it was late, then the contrast
 * channel every five frames of the first trial.
 *
 * Nothing here touches hardware and nothing is a timing measurement: the
 * onsets are made up, which is the point of a timeline that takes the
 * onset as an argument.
 *
 * Build (from the repository root):
 *     cc -O2 -Iinclude -o timeline_trial examples/timeline/trial.c -lm   # Linux / macOS
 *     cl /O2 /Iinclude examples\timeline\trial.c                         # Windows (MSVC)
 *
 * Usage: timeline_trial
 * Exit code: 0, or 1 if a call failed.
 */
#define YSP_TIMELINE_IMPLEMENTATION
#include "ysp/timeline.h"

#include <stdio.h>
#include <string.h>

enum { FIX_ON, GRATING_ON, CONTRAST, N_CHANNELS };
enum { TRIAL = 1 };

#define S YRT_S(1)

/* The keys stay valid while the channel uses them, so they are static. */
static const ytl_key ramp[] = {
    { S / 2,          0.0f, YTL_EASE_COSINE, 0, 0 },
    { S / 2 + S / 10, 0.5f, YTL_EASE_LINEAR, 0, 0 },
    { 11 * S / 10,    0.5f, YTL_EASE_COSINE, 0, 0 },
    { 6 * S / 5,      0.0f, YTL_EASE_LINEAR, 0, 0 },
};

static ytl_event storage[64];
static ytl_timeline tl;

static ytl_event ev(int64_t t, int kind, int target, int code) {
    ytl_event e;
    memset(&e, 0, sizeof(e));
    e.time_ns = t;
    e.base = TRIAL;
    e.kind = (uint8_t)kind;
    e.target = target;
    e.code = code;
    return e;
}

static const char* kind_name(int k) {
    switch (k) {
    case YTL_MARK:    return "mark";
    case YTL_TRIGGER: return "trigger";
    case YTL_ONSET:   return "onset";
    case YTL_OFFSET:  return "offset";
    case YTL_SET:     return "set";
    default:            return "user";
    }
}

/* Simulated grid: frame k at round(k * 1e9 / 60) ns. */
static int64_t grid(int64_t k) { return (k * S * 2 + 60) / 120; }

/* Run frames [k0, k0 + n) with the trial base anchored at `anchor`.
 * `drop` is a loop frame whose flip misses, so the loop skips a vblank. */
static int run_trial(int64_t* k, int n, int64_t anchor, int drop, bool show_contrast) {
    int i, j;
    int64_t period = grid(1) - grid(0);
    if (ytl_anchor(&tl, TRIAL, anchor, 0) < 0) return 1;
    for (i = 0; i < n; i++) {
        ytl_frame f;
        ytl_event fired[8];
        int nf;
        if (i == drop) (*k)++;
        f.onset = grid(*k);
        f.period = period;
        f.index = *k;
        nf = ytl_evaluate(&tl, &f, fired, 8);
        if (nf < 0) {
            fprintf(stderr, "evaluate: %s\n", ytl_strerror(nf));
            return 1;
        }
        for (j = 0; j < nf && j < 8; j++) {
            const ytl_event* e = &fired[j];
            printf("  frame %4lld  onset %9.3f ms  %-7s %s %2d  residual %+7.3f ms%s\n",
                   (long long)e->frame, (double)e->onset / 1e6, kind_name(e->kind),
                   e->kind == YTL_TRIGGER ? "code   " : "channel",
                   e->kind == YTL_TRIGGER ? e->code : e->target,
                   (double)e->residual / 1e6,
                   (e->flags & YTL_EV_LATE) ? "  late" : "");
        }
        if (show_contrast && i % 5 == 0 && i >= 25 && i <= 80)
            printf("                                   contrast at trial frame %2d: %.4f\n",
                   i, (double)ytl_value(&tl, CONTRAST));
        (*k)++;
    }
    return 0;
}

int main(void) {
    ytl_desc d;
    ytl_event e[5];
    int64_t k = 0;
    int rc;

    memset(&d, 0, sizeof(d));
    d.events = storage;
    d.event_capacity = 64;
    d.n_channels = N_CHANNELS;
    if (!ytl_open(&tl, &d)) {
        fprintf(stderr, "%s\n", ytl_error(&tl));
        return 1;
    }
    e[0] = ev(0,         YTL_ONSET,   FIX_ON, 0);
    e[1] = ev(S / 2,     YTL_OFFSET,  FIX_ON, 0);
    e[2] = ev(S / 2,     YTL_ONSET,   GRATING_ON, 0);
    e[3] = ev(S / 2,     YTL_TRIGGER, 0, 12);
    e[4] = ev(6 * S / 5, YTL_OFFSET,  GRATING_ON, 0);
    rc = ytl_add_n(&tl, e, 5);
    if (rc < 0) { fprintf(stderr, "add: %s\n", ytl_strerror(rc)); return 1; }
    rc = ytl_set_keys(&tl, CONTRAST, TRIAL, ramp, 4);
    if (rc < 0) { fprintf(stderr, "keys: %s\n", ytl_strerror(rc)); return 1; }

    printf("ysp_timeline %s, simulated 60 Hz\n", ytl_version());
    printf("trial 1: anchored on the grid at frame %lld; loop frame 30 drops\n", (long long)k);
    if (run_trial(&k, 80, grid(k), 30, true)) return 1;

    k += 10;
    printf("trial 2: anchored a third of a frame before frame %lld\n", (long long)k);
    if (run_trial(&k, 80, grid(k) - (grid(1) - grid(0)) / 3, -1, false)) return 1;
    return 0;
}
