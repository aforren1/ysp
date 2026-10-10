/* trials_jitter_golden.h - golden digests of ysp/trials.h's jitter
 * draws, shared by trials_test.c and trials_jitter_repro.c (which
 * CI builds with -O3 -march=native and -Ofast). Every draw is integer
 * arithmetic on the variate's 53 bits, so the nanoseconds (and frames)
 * must be identical on every C library, compiler and flag set; a
 * difference anywhere is a bug. */
#ifndef YSP_TRIALS_JITTER_GOLDEN_H
#define YSP_TRIALS_JITTER_GOLDEN_H

#include <stdio.h>
#include <string.h>

#define JG_N 6
static const char* const jg_names[JG_N] = {
    "uniform 0.8-1.2 s", "uniform, 60000/1001 Hz frames", "choice of 4", "choice, 60 Hz frames",
    "exponential 0.5-2.0 s, scale 0.4", "exponential, 60000/1001 Hz frames"
};
static const uint64_t jg_want[JG_N] = {
    0xd2e231de82a90abcULL, 0xfa91de69b65221e5ULL, 0xc4d62850f9f90fd5ULL,
    0xd74230f52068797aULL, 0xf4a23384d974ccebULL, 0xc2c66e03f8a80f1aULL
};

/* FNV-1a of 200000 draws' ns and frames per distribution, splitmix seeded
 * 2026; returns the number that differ from jg_want and prints each. */
static int jg_check(int verbose) {
    /* v0.2.1 gave these in seconds (0.1, 0.25, 0.333333333333, 0.5 and the
     * intervals below); the digests are of the same nanoseconds. */
    static const int64_t vals[4] = { 100000000, 250000000, 333333333, 500000000 };
    ytr_jitter_desc j[JG_N];
    uint64_t st, h;
    int k, i, bad = 0;
    j[0] = ytr_uniform("a", YRT_MS(800), YRT_MS(1200));
    j[1] = ytr_frames(ytr_uniform("b", YRT_MS(800), YRT_MS(1200)), 60000, 1001);
    j[2] = ytr_choice("c", vals, 4);
    j[3] = ytr_frames(ytr_choice("d", vals, 4), 60, 1);
    j[4] = ytr_exponential("e", YRT_MS(500), YRT_MS(2000), YRT_MS(400));
    j[5] = ytr_frames(ytr_exponential("f", YRT_MS(500), YRT_MS(2000), YRT_MS(400)), 60000, 1001);
    for (k = 0; k < JG_N; k++) {
        st = 2026;
        h = 0xCBF29CE484222325ULL;
        for (i = 0; i < 200000; i++) {
            ytr_jitter_value v = ytr_jitter_draw(&j[k], ytr_splitmix, &st);
            h = (h ^ (uint64_t)v.ns) * 0x100000001B3ULL;
            h = (h ^ (uint64_t)v.frames) * 0x100000001B3ULL;
        }
        if (h != jg_want[k]) bad++;
        if (verbose || h != jg_want[k])
            printf("  jitter golden %-36s %016llx%s\n", jg_names[k], (unsigned long long)h,
                   h != jg_want[k] ? "  DIFFERS" : "");
    }
    return bad;
}

#endif
