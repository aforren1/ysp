/* audio_alsa_test.c - ysp/audio.h's ALSA exclusive mode and device choice
 * on real sound hardware.
 *
 * Needs a card ALSA can open as hw: (YAU_ALSA_TEST_DEVICE, default
 * "hw:0,0", which must be the default card's first playback device) and
 * plays about 15 s of silence, so ctest does not run it. CMake builds it as
 * test_audio_alsa on Linux with miniaudio; the mutation list
 * (tests/mutate/audio.toml, alsa-*) runs it.
 *
 *   0. AUTO with a float project: ALSA exclusive on the default card's
 *      first playback device at 32 bits. Where a PulseAudio server runs
 *      (pactl answers): open() suspended it (the describe line says so),
 *      its sinks are SUSPENDED while the device is open and not after
 *      close; with desc.keep_sound_server the open fails and names
 *      YAU_BACKEND_PULSE.
 *   1. yau_list_devices(ALSA) lists the test device's id.
 *   2. Exclusive mode with no device is refused (miniaudio would take
 *      "default", the sound server's plugin), and so is a name part that
 *      two devices share, with their ids in the message (only where two
 *      devices share a name; this machine has "HDA Intel PCH, HDMI 1"
 *      twice).
 *   3. "hw:0,0" and ":0,0" name the same device.
 *   4. Exclusive at 64-frame periods for 10 s: the position source is
 *      ALSA's status, no underrun or halt, the fit's spread under 50 us,
 *      the drift within 100 ppm, and the frame written last plays after
 *      now and within 1 s.
 *
 * Exit 0 on pass, 1 on a failed check; 0 with "SKIP" when the device does
 * not open, unless YAU_ALSA_TEST_REQUIRE=1 (the mutation list).
 */
#define YSP_AUDIO_IMPLEMENTATION
#include "ysp/audio.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_failures = 0;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "audio_alsa_test: FAIL line %d: %s\n", __LINE__, #c); g_failures++; } } while (0)

static yau_device_entry g_list[128];

int main(void) {
    static yau_audio au;
    yau_desc d;
    yau_caps c;
    char line[512], id[64];
    const char* dev = getenv("YAU_ALSA_TEST_DEVICE");
    const char* req = getenv("YAU_ALSA_TEST_REQUIRE");
    int n, i, j, found = 0, dup = -1;
    int64_t end, t_now, t_last;
    if (!dev || !dev[0]) dev = "hw:0,0";
    /* 0 */
    {
        int server = access("/usr/bin/pactl", X_OK) == 0 && system("pactl info > /dev/null 2>&1") == 0;
        memset(&d, 0, sizeof d);
        if (!yau_open(&au, &d)) {
            if (req && req[0] == '1') { fprintf(stderr, "audio_alsa_test: FAIL AUTO open: %s\n", yau_error(&au)); return 1; }
            printf("audio_alsa_test: SKIP (AUTO: %s)\n", yau_error(&au));
            return 0;
        }
        yrt_sleep_ns(YRT_MS(1000));
        (void)yau_update(&au);
        yau_describe(&au, line, sizeof line);
        printf("AUTO: %s\n", line);
        CHECK(strstr(line, "backend=ALSA") != NULL);
        CHECK(strstr(line, "share=exclusive") != NULL);
        CHECK(strstr(line, "out=s32") != NULL);
        CHECK(strstr(line, "pos=device") != NULL);
        {   /* the default card's first device is the one running */
            int cc = 0, dd = 0;
            char path[96];
            FILE* f;
            if (sscanf(dev, "hw:%d,%d", &cc, &dd) == 2) {
                snprintf(path, sizeof path, "/proc/asound/card%d/pcm%dp/sub0/status", cc, dd);
                f = fopen(path, "r");
                CHECK(f != NULL);
                if (f) {
                    char st[64] = { 0 };
                    CHECK(fgets(st, sizeof st, f) != NULL && strstr(st, "RUNNING") != NULL);
                    fclose(f);
                }
            }
        }
        if (server) {
            CHECK(strstr(line, "sound-server-suspended") != NULL);
            CHECK(system("pactl list short sinks | grep -q SUSPENDED") == 0);
        }
        yau_close(&au);
        if (server) {
            yrt_sleep_ns(YRT_MS(300));
            CHECK(system("pactl list short sinks | grep -q SUSPENDED") != 0);   /* resumed */
            memset(&d, 0, sizeof d);
            d.keep_sound_server = true;
            CHECK(!yau_open(&au, &d));
            printf("keep_sound_server: %s\n", yau_error(&au));
            CHECK(strstr(yau_error(&au), "YAU_BACKEND_PULSE") != NULL);
            if (yau_is_open(&au)) yau_close(&au);
        }
    }
    /* 1 */
    n = yau_list_devices(YAU_BACKEND_ALSA, g_list, 128);
    printf("ALSA lists %d playback devices\n", n);
    CHECK(n > 0);
    snprintf(id, sizeof id, ":%s", strncmp(dev, "hw:", 3) == 0 ? dev + 3 : dev);
    for (i = 0; i < n && i < 128; i++) {
        if (!strcmp(g_list[i].id, id)) found = 1;
        for (j = 0; j < i && dup < 0; j++) if (!strcmp(g_list[i].name, g_list[j].name)) dup = i;
    }
    CHECK(found);
    /* 2 */
    memset(&d, 0, sizeof d);
    d.backend = YAU_BACKEND_ALSA;
    d.exclusive = true;
    d.format.sample = YAU_S16;
    CHECK(!yau_open(&au, &d));
    printf("no device: %s\n", yau_error(&au));
    CHECK(strstr(yau_error(&au), "needs a card's device") != NULL);
    if (dup >= 0) {
        memset(&d, 0, sizeof d);
        d.backend = YAU_BACKEND_ALSA;
        d.format.sample = YAU_S16;
        d.device = g_list[dup].name;
        CHECK(!yau_open(&au, &d));
        printf("shared name: %s\n", yau_error(&au));
        CHECK(strstr(yau_error(&au), "give one's id") != NULL);
    }
    /* 3 and 4 */
    memset(&d, 0, sizeof d);
    d.backend = YAU_BACKEND_ALSA;
    d.exclusive = true;
    d.format.sample = YAU_S16;
    d.period = 64;
    d.device = dev;
    if (!yau_open(&au, &d)) {
        if (req && req[0] == '1') {
            fprintf(stderr, "audio_alsa_test: FAIL open %s: %s\n", dev, yau_error(&au));
            return 1;
        }
        printf("audio_alsa_test: SKIP (%s)\n", yau_error(&au));
        return g_failures ? 1 : 0;
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
    CHECK(c.pos_source == YAU_POS_DEVICE);
    CHECK(strstr(line, "alsa:status") != NULL);
    CHECK(strstr(line, "share=exclusive") != NULL);
    CHECK(c.xruns == 0);
    CHECK(c.fit_spread_ns > 0 && c.fit_spread_ns < 50000);
    CHECK(fabs(c.drift_ppm) < 100);
    t_now = yrt_now_ns();
    t_last = yau_time_of(&au, au.w);
    printf("frame written last plays %.2f ms from now\n", (double)(t_last - t_now) / 1e6);
    CHECK(t_last > t_now);
    CHECK(t_last < t_now + 1000000000LL);
    yau_close(&au);
    /* the other spelling of the same device */
    d.device = id;
    d.period = 0;
    CHECK(yau_open(&au, &d));
    if (yau_is_open(&au)) yau_close(&au);
    if (g_failures) return 1;
    printf("audio_alsa_test: all checks passed\n");
    return 0;
}
