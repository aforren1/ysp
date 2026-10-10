/* timeline_tracking.c - a 10-minute sum-of-sines target, a flicker and a
 * tween, on a simulated 500 Hz display.
 *
 * The second USAGE example of ysp/timeline.h, run for real. Three channels
 * on one trial base:
 *   target_x   a sum of seven sines from 0.15 to 4.95 Hz, the kind of
 *              target a visuomotor tracking task follows, as a SAMPLED
 *              track: 120 samples per second for 10 minutes (72001
 *              floats, 288 KB), read with Catmull-Rom interpolation
 *   luminance  a 7.5 Hz square-wave flicker for the same 10 minutes: two
 *              STEP keys and a REPEAT of 4500 cycles, then off
 *   contrast   a TWEEN at 5 minutes from its current value to 0 over
 *              200 ms with a raised cosine
 * The frames are at round(k * 1e9 / 500) ns for 10 minutes and 1 second.
 *
 * Here the program computes the samples with sin(), which stands in for
 * the pack tool. It then checks every frame against the analytic signal,
 * and against the exact 7.5 Hz square wave (edges at multiples of 1/15 s,
 * not the track's period rounded to a whole ns) with each edge on its
 * nearest frame, as the default half-frame lead places it. It prints the
 * worst target error as a fraction of the target's peak, the number of
 * frames whose flicker value was wrong (it must be 0), and the contrast
 * at the start, the middle and the end of the tween.
 *
 * Nothing here touches hardware and nothing is a timing measurement.
 *
 * Build (from the repository root):
 *     cc -O2 -Iinclude -o timeline_tracking examples/timeline/tracking.c -lm   # Linux / macOS
 *     cl /O2 /Iinclude examples\timeline\tracking.c                            # Windows (MSVC)
 *
 * Usage: timeline_tracking
 * Exit code: 0, or 1 if a call failed or a check missed.
 */
#define YSP_TIMELINE_IMPLEMENTATION
#include "ysp/timeline.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

enum { TARGET_X, LUMINANCE, CONTRAST, N_CHANNELS };
enum { TRIAL = 1 };

#define S YRT_S(1)
#define RATE 120                      /* samples per second               */
#define MINUTES 10
#define N_SAMPLES (MINUTES * 60 * RATE + 1)
#define HZ 500                        /* the display                      */

static const double freq[7] = { 0.15, 0.35, 0.75, 1.35, 2.15, 3.25, 4.95 };

static double target(double t) {
    double x = 0.0;
    int i;
    for (i = 0; i < 7; i++)
        x += (1.0 / freq[i]) * sin(2.0 * 3.14159265358979323846 * freq[i] * t + 1.7 * i);
    return x;
}

static float xs[N_SAMPLES];
static ytl_event storage[4];
static ytl_timeline tl;

/* The keys stay valid while the channel uses them, so they are static. */
static const ytl_key square[] = {
    { 0,          1.0f, YTL_EASE_STEP, 0, 0 },
    { 2 * S / 30, 0.0f, YTL_EASE_STEP, 0, 0 },
};

int main(void) {
    const int64_t period = 4 * S / 30;           /* 7.5 Hz, 133333333 ns  */
    const int64_t repeats = 4500;
    const int64_t tween_at = 5 * 60 * S, tween_len = S / 5;
    ytl_desc d;
    ytl_track path, flicker;
    double peak = 0.0, worst = 0.0, worst_t = 0.0, worst_in = 0.0;
    float initial[N_CHANNELS] = { 0.0f, 0.0f, 1.0f };
    long long wrong_flicker = 0, k, n_frames = (MINUTES * 60 + 1) * (long long)HZ;
    int i, rc;
    bool posted = false;

    for (i = 0; i < 7; i++) peak += 1.0 / freq[i];
    for (i = 0; i < N_SAMPLES; i++) xs[i] = (float)target((double)i / RATE);

    memset(&d, 0, sizeof(d));
    d.events = storage;
    d.event_capacity = 4;
    d.n_channels = N_CHANNELS;
    d.initial = initial;
    if (!ytl_open(&tl, &d)) {
        fprintf(stderr, "%s\n", ytl_error(&tl));
        return 1;
    }

    memset(&path, 0, sizeof(path));
    path.samples = xs;
    path.n_samples = N_SAMPLES;
    path.rate = RATE;
    path.interp = YTL_INTERP_CUBIC;
    rc = ytl_set_track(&tl, TARGET_X, TRIAL, &path);
    if (rc < 0) { fprintf(stderr, "path: %s\n", ytl_strerror(rc)); return 1; }

    memset(&flicker, 0, sizeof(flicker));
    flicker.keys = square;
    flicker.n_keys = 2;
    flicker.period_ns = period;
    flicker.repeats = (int)repeats;
    rc = ytl_set_track(&tl, LUMINANCE, TRIAL, &flicker);
    if (rc < 0) { fprintf(stderr, "flicker: %s\n", ytl_strerror(rc)); return 1; }

    ytl_anchor(&tl, TRIAL, 0, 0);
    printf("ysp_timeline %s: %d samples at %d/s (%.0f KB), %d Hz display, %lld frames\n",
           ytl_version(), N_SAMPLES, RATE, N_SAMPLES * 4.0 / 1024.0, HZ, n_frames);

    for (k = 0; k < n_frames; k++) {
        ytl_frame f;
        int64_t bt;
        double err;
        float want_lum;
        f.onset = (k * S * 2 + HZ) / (2 * HZ);
        f.period = ((k + 1) * S * 2 + HZ) / (2 * HZ) - f.onset;
        f.index = k;
        bt = f.onset;                             /* the base is anchored at 0 */
        if (bt >= tween_at && !posted) {
            /* A script's fade, posted on the first frame at 5 minutes. A
             * MARK at its end is what a script would await. */
            ytl_event end;
            ytl_tween_desc fade;
            memset(&end, 0, sizeof(end));
            end.time_ns = bt + tween_len;
            end.base = TRIAL;
            /* Field by field so the example also builds as C++17; in C99
             * this is &(ytl_tween_desc){ .start_ns = bt, .start_set = true,
             * .duration_ns = tween_len, .ease = YTL_EASE_COSINE }. */
            memset(&fade, 0, sizeof(fade));
            fade.start_ns = bt;
            fade.start_set = true;
            fade.duration_ns = tween_len;
            fade.ease = YTL_EASE_COSINE;
            if (ytl_tween(&tl, CONTRAST, TRIAL, &fade) < 0 || ytl_add(&tl, &end) < 0) {
                fprintf(stderr, "tween failed\n");
                return 1;
            }
            posted = true;
            printf("tween posted at %.3f s, contrast %.4f\n", (double)bt / 1e9,
                   (double)ytl_value(&tl, CONTRAST));
        }
        if (ytl_evaluate(&tl, &f, NULL, 0) < 0) return 1;

        err = fabs((double)ytl_value(&tl, TARGET_X) - target((double)bt / 1e9));
        if (bt <= (int64_t)(N_SAMPLES - 1) * S / RATE && err > worst) { worst = err; worst_t = (double)bt / 1e9; }
        if (bt >= S / RATE && bt <= (int64_t)(N_SAMPLES - 2) * S / RATE && err > worst_in) worst_in = err;
        {
            /* The exact 7.5 Hz wave, edges at multiples of 1/15 s, read
             * where the default lead reads a STEP track: half a period
             * after the onset, so each edge shows on its nearest frame. */
            int64_t w = bt + f.period / 2;
            int64_t halves = (w / S) * 15 + (w % S) * 15 / S;
            want_lum = halves >= 2 * repeats ? 0.0f : (halves % 2 == 0 ? 1.0f : 0.0f);
        }
        if (ytl_value(&tl, LUMINANCE) != want_lum) wrong_flicker++;
        if (bt >= tween_at && bt < tween_at + tween_len + 3 * S / HZ
            && (k % 25 == 0 || bt >= tween_at + tween_len))
            printf("  %.3f s  contrast %.4f\n", (double)bt / 1e9, (double)ytl_value(&tl, CONTRAST));
    }
    printf("target: worst error %.2e of the peak (%.2f) at %.4f s; %.2e away from the end segments\n",
           worst / peak, peak, worst_t, worst_in / peak);
    printf("flicker: %lld frames wrong of %lld\n", wrong_flicker, n_frames);
    printf("after 10 minutes: luminance %.1f (off), contrast %.1f\n",
           (double)ytl_value(&tl, LUMINANCE), (double)ytl_value(&tl, CONTRAST));
    return worst / peak < 5e-5 && wrong_flicker == 0 && ytl_value(&tl, CONTRAST) == 0.0f ? 0 : 1;
}
