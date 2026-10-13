/* audio_pulse_capture_test.c - ysp/audio.h's PulseAudio capture on a real
 * sound server: the refusals, the record stream's position, and a digital
 * loopback through a sink's monitor.
 *
 * Needs a PulseAudio server (or PipeWire's PulseAudio service) and pactl;
 * part 4 also needs the default card's hw: device (YAU_ALSA_TEST_DEVICE,
 * default "hw:0,0") for an AUTO output. It plays clicks at -40 dBFS on the
 * default sink for about 12 s, so ctest does not run it. CMake builds it as
 * test_audio_pulse_capture on Linux with miniaudio; the mutation list
 * (tests/mutate/audio.toml, pulse-0x) runs it.
 *
 *   1. With no device, where the server's default source is a sink's
 *      monitor: refused, naming desc.device (a monitor is a copy of the
 *      output, not a microphone).
 *   2. The default sink's monitor by name, 16-bit at the sink's rate, for
 *      10 s beside a PULSE output: the position from the device
 *      (pulse:timing-info), no loss, the fit's spread under 500 us, the
 *      drift within 100 ppm; 20 clicks played at set times are found in
 *      the capture, the capture time less the onset's between -6 and +1 ms
 *      (the monitor has the frames when the sink mixes them, before the
 *      sink's latency: -1.9 ms measured), and the 20 differences within
 *      0.5 ms of their median.
 *   2b. Where the card has an input source (an input or duplex profile;
 *      nothing need be plugged in): 10 s from it at its rate, the position
 *      from the device, the fit's spread under 50 us, no loss, and every
 *      new report within 30 us sd of a least-squares line through them
 *      (3.6 and 0.8 us measured; 114 to 121 us without the source's
 *      latency).
 *   3. DUPLEX on PULSE is refused.
 *   4. With an AUTO output open (ALSA exclusive, card 0 suspended): a
 *      PULSE capture of card 0's monitor is refused. With a PULSE capture
 *      of it open: the AUTO output is refused, naming the capture; after
 *      the capture's close it opens.
 *
 * Exit 0 on pass, 1 on a failed check; 0 with "SKIP" when no server
 * answers, unless YAU_PULSE_TEST_REQUIRE=1 (the mutation list).
 */
#include "ysp/rt.h"

/* every new report of the input, for a line of its own (part 2b): the
 * header's fit keeps the least late report of each 200 ms, which hides
 * steps a least-squares line over every report shows */
#define NREP 4096
static int64_t g_rp[NREP], g_rt[NREP], g_last_t = -1;
static volatile int g_nrep = 0, g_keep = 0;
static void tst_on_capture(const void* tk);
#define YAU__ON_CAPTURE(c, tk, frames) tst_on_capture((const void*)(tk))

#define YSP_AUDIO_IMPLEMENTATION
#include "ysp/audio.h"

static void tst_on_capture(const void* tkp) {
    const yau_tick* tk = (const yau_tick*)tkp;
    if (!g_keep || tk->pos < 0 || tk->pos_t == g_last_t || g_nrep >= NREP) return;
    g_last_t = tk->pos_t;
    g_rp[g_nrep] = tk->pos;
    g_rt[g_nrep] = tk->pos_t;
    g_nrep++;
}

/* the sd of the residuals of a least-squares line through the reports */
static double rep_sd(int skip) {
    double mx = 0, mt = 0, sxx = 0, sxt = 0, k, ss = 0;
    int i, n = g_nrep - skip;
    if (n < 10) return 1e9;
    for (i = skip; i < g_nrep; i++) { mx += (double)(g_rp[i] - g_rp[skip]); mt += (double)(g_rt[i] - g_rt[skip]); }
    mx /= n; mt /= n;
    for (i = skip; i < g_nrep; i++) {
        double x = (double)(g_rp[i] - g_rp[skip]) - mx, t = (double)(g_rt[i] - g_rt[skip]) - mt;
        sxx += x * x; sxt += x * t;
    }
    k = sxt / sxx;
    for (i = skip; i < g_nrep; i++) {
        double x = (double)(g_rp[i] - g_rp[skip]) - mx, t = (double)(g_rt[i] - g_rt[skip]) - mt;
        ss += (t - k * x) * (t - k * x);
    }
    return sqrt(ss / n);
}

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "audio_pulse_capture_test: FAIL line %d: %s\n", __LINE__, #c); g_failures++; } } while (0)

static yau_capture g_cap;
static yau_audio g_au;
static float g_buf[48000 * 2];

/* One line of a command's output, or "" */
static void sh1(const char* cmd, char* out, size_t cap) {
    FILE* f = popen(cmd, "r");
    out[0] = 0;
    if (!f) return;
    if (fgets(out, (int)cap, f)) out[strcspn(out, "\r\n")] = 0;
    pclose(f);
}

int main(void) {
    yau_capture_desc cd;
    yau_capture_info in;
    yau_desc od;
    char sink[256], src[256], mon[300], rate_s[32], line[640];
    const char* req = getenv("YAU_PULSE_TEST_REQUIRE");
    uint32_t rate;
    sh1("pactl get-default-sink 2>/dev/null", sink, sizeof sink);
    if (!sink[0]) {
        if (req && req[0] == '1') { fprintf(stderr, "audio_pulse_capture_test: FAIL: no PulseAudio server\n"); return 1; }
        printf("audio_pulse_capture_test: SKIP (no PulseAudio server)\n");
        return 0;
    }
    sh1("pactl get-default-source 2>/dev/null", src, sizeof src);
    snprintf(mon, sizeof mon, "%s.monitor", sink);
    /* the sink's rate now: the monitor's, and a PULSE output's */
    {
        char cmd[512];
        snprintf(cmd, sizeof cmd, "pactl list short sinks | awk -v s='%s' '$2==s {for (i = 1; i <= NF; i++) if ($i ~ /Hz$/) print $i}' | tr -dc 0-9", sink);
        sh1(cmd, rate_s, sizeof rate_s);
        rate = (uint32_t)atoi(rate_s);
        if (!rate) rate = 48000;
    }
    printf("default sink %s at %u Hz, default source %s\n", sink, (unsigned)rate, src);
    /* 1 */
    memset(&cd, 0, sizeof cd);
    cd.backend = YAU_BACKEND_PULSE;
    cd.format.sample = YAU_S16;
    cd.format.rate = rate;
    if (strstr(src, ".monitor")) {
        CHECK(!yau_capture_open(&g_cap, &cd));
        printf("default monitor: %s\n", yau_capture_error(&g_cap));
        CHECK(strstr(yau_capture_error(&g_cap), "monitor") != NULL);
        CHECK(strstr(yau_capture_error(&g_cap), "desc.device") != NULL);
    } else {
        printf("the default source is not a monitor: no refusal to check\n");
    }
    /* 2 */
    memset(&od, 0, sizeof od);
    od.backend = YAU_BACKEND_PULSE;
    od.format.sample = YAU_S16;
    od.format.rate = rate;
    if (!yau_open(&g_au, &od)) {
        fprintf(stderr, "audio_pulse_capture_test: FAIL output open: %s\n", yau_error(&g_au));
        return 1;
    }
    cd.device = mon;
    if (!yau_capture_open(&g_cap, &cd)) {
        fprintf(stderr, "audio_pulse_capture_test: FAIL monitor open: %s\n", yau_capture_error(&g_cap));
        yau_close(&g_au);
        return 1;
    }
    {
        yau_tone_desc td;
        yau_buf click;
        yau_edge e;
        yau_edge_desc ed;
        yau_id ids[20];
        int64_t t0 = yrt_now_ns() + 1500000000LL, end, first, k, tcap[20];
        double diff[20], med, worst = 0;
        int nfound = 0, i;
        memset(&td, 0, sizeof td);
        td.hz = 2000;
        td.dur_ns = YRT_MS(2);
        td.peak = 0.01f;   /* -40 dBFS */
        click = yau_tone(&g_au, &td);
        CHECK(click.frames != NULL);
        for (i = 0; i < 20; i++) ids[i] = yau_play_at(&g_au, click, t0 + (int64_t)i * 400000000LL);
        memset(&ed, 0, sizeof ed);
        ed.level_db = -50;
        ed.dead_ns = YRT_MS(100);
        CHECK(yau_edge_init(&e, &ed, rate) == 0);
        end = t0 + 20 * 400000000LL + 600000000LL;
        while (yrt_now_ns() < end) {
            (void)yau_update(&g_au);
            while ((k = yau_capture_read(&g_cap, 0, g_buf, 24000, &first)) > 0) {
                double ev[8];
                int m = yau_edge_feed(&e, g_buf, k, g_cap.dcaps.channels, first, ev, 8), j;
                for (j = 0; j < m && nfound < 20; j++) tcap[nfound++] = yau_capture_time_at(&g_cap, ev[j]);
            }
            yrt_sleep_ns(YRT_MS(10));
        }
        (void)yau_capture_update(&g_cap);
        for (i = 0; i < nfound; i++) {
            yau_onset r;
            int rc = yau_wait(&g_au, ids[i], 1000000000LL, &r);
            diff[i] = rc == YAU_OK ? (double)(tcap[i] - r.onset) / 1e6 : 1e9;
        }
        yau_capture_describe(&g_cap, line, sizeof line);
        printf("monitor: %s\n", line);
        yau_describe(&g_au, line, sizeof line);
        printf("output: %s\n", line);
        yau_capture_get_info(&g_cap, &in);
        CHECK(in.pos_source == YAU_POS_DEVICE);
        CHECK(!strcmp(in.os_latency_src, "pulse:timing-info"));
        CHECK(in.xruns == 0 && in.lost == 0 && in.dropped == 0);
        CHECK(in.fit_spread_ns > 0 && in.fit_spread_ns < 500000);
        CHECK(fabs(in.drift_ppm) < 100);
        CHECK(nfound == 20);
        {
            double s[20];
            int a, b;
            for (a = 0; a < nfound; a++) s[a] = diff[a];
            for (a = 1; a < nfound; a++) for (b = a; b > 0 && s[b - 1] > s[b]; b--) { double t = s[b]; s[b] = s[b - 1]; s[b - 1] = t; }
            med = nfound ? s[nfound / 2] : 1e9;
            for (a = 0; a < nfound; a++) if (fabs(diff[a] - med) > worst) worst = fabs(diff[a] - med);
            printf("loopback: %d of 20 clicks found; capture time less onset time: median %+.3f ms, "
                   "min %+.3f, max %+.3f, all within %.3f ms of the median\n", nfound, med, nfound ? s[0] : 0.0,
                   nfound ? s[nfound - 1] : 0.0, worst);
        }
        /* the monitor has a sink's frames when the sink mixes them, a
         * sink latency (2.7 to 3.5 ms measured) before they play: early */
        CHECK(med > -6.0 && med < 1.0);
        CHECK(worst < 0.5);
    }
    yau_capture_close(&g_cap);
    yau_close(&g_au);
    CHECK(yau__pas.caps[0] == 0);   /* the closed capture no longer counts on card 0 */
    /* 2b */
    {
        char in_src[256], in_rate[32];
        sh1("pactl list short sources | awk '$2 !~ /monitor$/ && $2 ~ /^alsa_input/ {print $2; exit}'", in_src, sizeof in_src);
        if (in_src[0]) {
            char cmd[512];
            snprintf(cmd, sizeof cmd, "pactl list short sources | awk -v s='%s' '$2==s {for (i = 1; i <= NF; i++) if ($i ~ /Hz$/) print $i}' | tr -dc 0-9", in_src);
            sh1(cmd, in_rate, sizeof in_rate);
            memset(&cd, 0, sizeof cd);
            cd.backend = YAU_BACKEND_PULSE;
            cd.format.sample = YAU_S16;
            cd.format.rate = (uint32_t)atoi(in_rate);
            cd.device = in_src;
            g_nrep = 0;
            g_last_t = -1;
            g_keep = 1;
            if (!yau_capture_open(&g_cap, &cd)) {
                fprintf(stderr, "audio_pulse_capture_test: FAIL source open: %s\n", yau_capture_error(&g_cap));
                g_failures++;
            } else {
                int64_t end = yrt_now_ns() + 10000000000LL, first;
                double sd;
                while (yrt_now_ns() < end) {
                    while (yau_capture_read(&g_cap, 0, g_buf, 24000, &first) > 0) {}
                    yrt_sleep_ns(YRT_MS(10));
                }
                (void)yau_capture_update(&g_cap);
                yau_capture_describe(&g_cap, line, sizeof line);
                printf("source: %s\n", line);
                yau_capture_get_info(&g_cap, &in);
                CHECK(in.pos_source == YAU_POS_DEVICE);
                CHECK(in.xruns == 0 && in.lost == 0 && in.dropped == 0);
                CHECK(in.fit_spread_ns > 0 && in.fit_spread_ns < 50000);
                CHECK(fabs(in.drift_ppm) < 100);
                yau_capture_close(&g_cap);
                g_keep = 0;
                sd = rep_sd(g_nrep / 10);
                printf("source: %d reports, %.2f us sd from a line through them\n", g_nrep, sd / 1e3);
                CHECK(sd < 30000);
            }
        } else {
            printf("no input source on a card (the profile is output only): part 2b skipped\n");
        }
    }
    /* 3 */
    memset(&od, 0, sizeof od);
    memset(&cd, 0, sizeof cd);
    od.backend = YAU_BACKEND_PULSE;
    od.format.sample = YAU_S16;
    od.format.rate = rate;
    cd.format.sample = YAU_S16;
    cd.format.rate = rate;
    cd.device = mon;
    od.capture = &g_cap;
    od.capture_desc = &cd;
    CHECK(!yau_open(&g_au, &od));
    printf("PULSE duplex: %s\n", yau_error(&g_au));
    CHECK(strstr(yau_error(&g_au), "DUPLEX on PulseAudio is refused") != NULL);
    /* 4 */
    memset(&od, 0, sizeof od);
    if (!yau_open(&g_au, &od)) {
        printf("no AUTO output (%s): part 4 skipped\n", yau_error(&g_au));
        /* a closed capture must not keep the card */
        CHECK(strstr(yau_error(&g_au), "PulseAudio capture") == NULL);
    } else {
        memset(&cd, 0, sizeof cd);
        cd.backend = YAU_BACKEND_PULSE;
        cd.format.sample = YAU_S16;
        cd.format.rate = rate;
        cd.device = mon;
        CHECK(!yau_capture_open(&g_cap, &cd));
        printf("monitor beside AUTO: %s\n", yau_capture_error(&g_cap));
        CHECK(strstr(yau_capture_error(&g_cap), "suspended") != NULL);
        yau_close(&g_au);
        yrt_sleep_ns(YRT_MS(300));
        CHECK(yau_capture_open(&g_cap, &cd));
        if (yau_capture_is_open(&g_cap)) {
            memset(&od, 0, sizeof od);
            CHECK(!yau_open(&g_au, &od));
            printf("AUTO beside a monitor capture: %s\n", yau_error(&g_au));
            CHECK(strstr(yau_error(&g_au), "PulseAudio capture") != NULL);
            if (yau_is_open(&g_au)) yau_close(&g_au);
            yau_capture_close(&g_cap);
            /* the capture's close lets the card go again (the count, since
             * an idle sink may have closed its PCM and need no suspension) */
            CHECK(yau__pas.caps[0] == 0);
            yrt_sleep_ns(YRT_MS(300));
            CHECK(yau_open(&g_au, &od));
            if (yau_is_open(&g_au)) yau_close(&g_au);
        }
    }
    if (g_failures) return 1;
    printf("audio_pulse_capture_test: all checks passed\n");
    return 0;
}
