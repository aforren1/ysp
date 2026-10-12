/* audio_pulse_test.c - ysp/audio.h's PulseAudio position source on a real
 * sound server.
 *
 * Needs a PulseAudio server (or PipeWire's PulseAudio service) and plays
 * 10 s of silence, so ctest does not run it. CMake builds it as
 * test_audio_pulse on Linux with miniaudio; the mutation list
 * (tests/mutate/audio.toml, pulse-*) runs it.
 *
 *   1. A 16-bit project opens on YAU_BACKEND_PULSE (a float project is
 *      refused where the sink runs s16, docs/audio.md "Linux: first runs").
 *   2. The position source is the device (PulseAudio's stream timing info),
 *      and the OS claim names it.
 *   3. Over 10 s: no underrun or halt, the fit's spread under 50 us (2 to
 *      6 us measured; callback times gave 450 to 620 us), the drift within
 *      100 ppm.
 *   4. The fit is on the ysp_rt clock and not ahead of the frames written:
 *      the frame written last plays after now, and within 1 s.
 *
 * Exit 0 on pass, 1 on a failed check; 0 with "SKIP" when no PulseAudio
 * server answers, unless YAU_PULSE_TEST_REQUIRE=1 (the mutation list).
 */
#define YSP_AUDIO_IMPLEMENTATION
#include "ysp/audio.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "audio_pulse_test: FAIL line %d: %s\n", __LINE__, #c); g_failures++; } } while (0)

int main(void) {
    static yau_audio au;
    yau_desc d;
    yau_caps c;
    char line[512];
    int64_t end, t_now, t_last;
    const char* req = getenv("YAU_PULSE_TEST_REQUIRE");
    memset(&d, 0, sizeof d);
    d.backend = YAU_BACKEND_PULSE;
    d.format.sample = YAU_S16;
    if (!yau_open(&au, &d)) {
        if (req && req[0] == '1') {
            fprintf(stderr, "audio_pulse_test: FAIL open: %s\n", yau_error(&au));
            return 1;
        }
        printf("audio_pulse_test: SKIP (%s)\n", yau_error(&au));
        return 0;
    }
    end = yrt_now_ns() + 10000000000LL;
    while (yrt_now_ns() < end) {
        (void)yau_update(&au);
        yrt_sleep_ns(YRT_MS(20));
    }
    (void)yau_update(&au);
    yau_get_caps(&au, &c);
    yau_describe(&au, line, sizeof line);
    printf("%s\n", line);
    printf("pos_source %d, drift %+.2f ppm, spread %.1f us, xruns %u\n", (int)c.pos_source, c.drift_ppm,
           c.fit_spread_ns / 1e3, (unsigned)c.xruns);
    CHECK(c.pos_source == YAU_POS_DEVICE);
    CHECK(strstr(line, "pulse:timing-info") != NULL);
    CHECK(c.xruns == 0);
    CHECK(c.fit_spread_ns > 0 && c.fit_spread_ns < 50000);
    CHECK(fabs(c.drift_ppm) < 100);
    /* the last frame written is still ahead of the output, and on the
     * ysp_rt clock: within the buffer and the sink's latency of now */
    t_now = yrt_now_ns();
    t_last = yau_time_of(&au, au.w);
    printf("frame written last plays %.2f ms from now\n", (double)(t_last - t_now) / 1e6);
    CHECK(t_last > t_now);
    CHECK(t_last < t_now + 1000000000LL);
    yau_close(&au);
    if (g_failures) return 1;
    printf("audio_pulse_test: all checks passed\n");
    return 0;
}
