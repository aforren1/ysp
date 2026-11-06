/* clock_offset.c - CLOCK_MONOTONIC (ysp/rt.h's clock on macOS before
 * 0.10.0) against mach_absolute_time(), CoreAudio's host time, and against
 * mach_continuous_time(), every 0.1 s (a line a second). The offset is the
 * largest of 20000 read pairs: CLOCK_MONOTONIC counts whole microseconds,
 * so a pair reads up to 1 us low. docs/audio.md, "CoreAudio".
 *
 *     clang -O2 tests/probe/coreaudio/clock_offset.c -o clock_offset && ./clock_offset [SECONDS]
 */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <stdint.h>
#include <unistd.h>
#include <mach/mach_time.h>
static mach_timebase_info_data_t tb;
static int64_t absns(uint64_t t) { return (int64_t)(t / tb.denom * tb.numer + (t % tb.denom) * tb.numer / tb.denom); }
static int64_t mono(void) { struct timespec m; clock_gettime(CLOCK_MONOTONIC, &m); return (int64_t)m.tv_sec * 1000000000 + m.tv_nsec; }
int main(int argc, char** argv) {
    int secs = argc > 1 ? atoi(argv[1]) : 30;
    mach_timebase_info(&tb);
    int64_t first = 0, t_first = 0;
    for (int s = 0; s <= secs * 10; s++) {
        /* mono truncates to 1 us, so mono - abs is late by 0..1 us: the max over many
         * reads is the offset within a read's time */
        int64_t best = INT64_MIN, bestc = INT64_MIN;
        for (int i = 0; i < 20000; i++) {
            uint64_t a = mach_absolute_time();
            int64_t m = mono();
            uint64_t c = mach_continuous_time();
            int64_t d = m - absns(a);
            if (d > best) best = d;
            int64_t dc = m - absns(c);
            if (dc > bestc) bestc = dc;
        }
        int64_t now = absns(mach_absolute_time());
        if (s == 0) { first = best; t_first = now; }
        if (s % 10 == 0)
            printf("t %6.1f s  mono-abs %lld ns  change %+lld ns  slope %+.3f ppm  mono-cont %lld\n", (now - t_first) / 1e9,
                   (long long)best, (long long)(best - first),
                   now > t_first ? (double)(best - first) / (double)(now - t_first) * 1e6 : 0.0, (long long)bestc);
        usleep(100000);
    }
    return 0;
}
