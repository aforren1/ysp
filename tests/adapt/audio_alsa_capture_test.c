/* audio_alsa_capture_test.c - ysp/audio.h's ALSA exclusive capture, its
 * device choice and its duplex device, on real sound hardware.
 *
 * Needs a card ALSA can open as hw: for capture (YAU_ALSA_TEST_DEVICE,
 * default "hw:0,0", which must be the default card's first capture device
 * and first playback device), records about 50 s of whatever is at the
 * input (nothing needs to be plugged in), so ctest does not run it. CMake
 * builds it as test_audio_alsa_capture on Linux with miniaudio; the
 * mutation list (tests/mutate/audio.toml, alsa-1x) runs it.
 *
 *   0. AUTO with a float desc: ALSA exclusive on the default card's first
 *      capture device at 32 bits, the position from ALSA's status
 *      (alsa:status). Where PulseAudio holds the card's capture PCM (an
 *      input or duplex profile): the open suspended it, and with
 *      desc.keep_sound_server it fails naming YAU_BACKEND_PULSE.
 *   1. yau_list_inputs(ALSA) lists the device's id.
 *   2. Exclusive with no device is refused; a name part that two inputs
 *      share is refused with their ids (where two share one); a part only
 *      one has is taken; "hw:0,0" and ":0,0" name the same input.
 *   3. 10 s each at 64 and 480-frame periods: no loss, the fit's spread
 *      under 50 us, the drift within 100 ppm, every report's backlog (the
 *      position less the frame after the block, which is ALSA's delay) at
 *      least 0 and under the buffer, and the newest frame recorded before
 *      now and within the buffer and 50 ms of it.
 *   4. Stalls of the device thread (YAU__ON_CAPTURE) at 64 frames: one
 *      under the buffer loses nothing; one 30 ms longer than the buffer is
 *      one loss, counted with the first block after it (no frame numbered
 *      from before it), found within the stall and 5 ms, and the frames
 *      after it lie within 100 us of the fit from before it.
 *   5. DUPLEX with AUTO (the output on the same card): 10 s at 64 frames,
 *      the silence written before the start, no underrun and no capture
 *      loss, both positions from the device, the input on the output's
 *      card, and a buffer of YAU__DUPLEX_PERIODS periods.
 *   5b. DUPLEX with a 16-bit input beside a 32-bit output at 960 and 1024-
 *      frame periods, where miniaudio splits each read into callbacks of 512
 *      and 448, and of 512 and 512: only a read's last callback reports
 *      (SPLIT READS), so every report is within 100 us sd of a line through
 *      them (10 to 31 us measured; one reported short is 9 to 11 ms off),
 *      none is set aside, and nothing is counted lost.
 *   5d. DUPLEX with the capture desc's keep_sound_server: refused, naming
 *      YAU_BACKEND_PULSE, where the server holds the card.
 *   6. An AUTO output, then an AUTO input on the same card: the input holds
 *      the card's suspension too, so after the output's close the card
 *      stays suspended, and after the input's it is resumed. exit() with
 *      both open (a child process): the card is resumed.
 *
 * Exit 0 on pass, 1 on a failed check; 0 with "SKIP" when the device does
 * not open, unless YAU_ALSA_TEST_REQUIRE=1 (the mutation list).
 */
#include "ysp/rt.h"

/* the stall hook: the capture block sleeps once, on the device thread */
static int64_t g_stall_ns = 0;
static int64_t g_stall_t = 0;      /* when it began, 0 = not yet */
static int64_t g_q_min = INT64_MAX, g_q_max = INT64_MIN;
static void tst_on_capture(const void* c, const void* tk, int32_t n);
#define YAU__ON_CAPTURE(c, tk, frames) tst_on_capture((const void*)(c), (const void*)(tk), (frames))

#define YSP_AUDIO_IMPLEMENTATION
#include "ysp/audio.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

/* every new report, for a line of its own (part 5) */
#define NREP 8192
static int64_t g_rp[NREP], g_rt[NREP], g_last_t = -1;
static int g_nrep = 0, g_keep = 0;

static void tst_on_capture(const void* cp, const void* tkp, int32_t n) {
    const yau_capture* c = (const yau_capture*)cp;
    const yau_tick* tk = (const yau_tick*)tkp;
    if (g_keep && tk->pos >= 0 && tk->pos_t != g_last_t && g_nrep < NREP) {
        g_last_t = tk->pos_t;
        g_rp[g_nrep] = tk->pos;
        g_rt[g_nrep] = tk->pos_t;
        g_nrep++;
    }
    if (tk->pos >= 0 && c->fit.ready) {
        int64_t q = tk->pos + c->p_off - (c->f + c->dev_lost + n);
        if (q < g_q_min) g_q_min = q;
        if (q > g_q_max) g_q_max = q;
    }
    if (g_stall_ns) {
        int64_t ns = g_stall_ns;
        g_stall_ns = 0;
        g_stall_t = yrt_now_ns();
        yrt_sleep_until(g_stall_t + ns, 0);
    }
}

/* the sd of the residuals of a least-squares line through the reports */
static double rep_sd(int skip) {
    double mx = 0, mt = 0, sxx = 0, sxt = 0, k, ss = 0;
    int i, n = g_nrep - skip;
    if (n < 10) return 1e12;
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

static int sinks_suspended(void) { return system("pactl list short sinks 2>/dev/null | grep -q SUSPENDED") == 0; }

static int g_failures = 0;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "audio_alsa_capture_test: FAIL line %d: %s\n", __LINE__, #c); g_failures++; } } while (0)

static yau_device_entry g_list[64];
static yau_capture g_cap;
static yau_audio g_au;
static float g_buf[48000 * 2];
static unsigned char g_ring_mem[YRT_RING_BYTES(8192)];
static yrt_ring g_ring;

/* Reads the ring for s seconds; returns the frames read. */
static int64_t run(double s, int out) {
    int64_t end = yrt_now_ns() + (int64_t)(s * 1e9), got = 0, k, first;
    while (yrt_now_ns() < end) {
        if (out) (void)yau_update(&g_au);
        while ((k = yau_capture_read(&g_cap, 0, g_buf, 48000, &first)) > 0) got += k;
        yrt_sleep_ns(YRT_MS(10));
    }
    (void)yau_capture_update(&g_cap);
    return got;
}

static int proc_running(const char* dev, char dir) {
    int cc = 0, dd = 0, ok = 0;
    char path[96], st[64] = { 0 };
    FILE* f;
    if (sscanf(dev, "hw:%d,%d", &cc, &dd) != 2) return 1;
    snprintf(path, sizeof path, "/proc/asound/card%d/pcm%d%c/sub0/status", cc, dd, dir);
    f = fopen(path, "r");
    if (!f) return 0;
    ok = fgets(st, sizeof st, f) != NULL && strstr(st, "RUNNING") != NULL;
    fclose(f);
    return ok;
}

static int proc_held(const char* dev) {
    int cc = 0, dd = 0, held = 0;
    char path[96], st[64] = { 0 };
    FILE* f;
    if (sscanf(dev, "hw:%d,%d", &cc, &dd) != 2) return 0;
    snprintf(path, sizeof path, "/proc/asound/card%d/pcm%dc/sub0/hw_params", cc, dd);
    f = fopen(path, "r");
    if (!f) return 0;
    held = fgets(st, sizeof st, f) != NULL && strstr(st, "closed") == NULL;
    fclose(f);
    return held;
}

static void info_line(const char* what) {
    char line[640];
    yau_capture_describe(&g_cap, line, sizeof line);
    printf("%s: %s\n", what, line);
}

/* part 6's child: both devices open, then exit() */
static int exit_child(void) {
    static yau_audio au2;
    static yau_capture cap2;
    yau_desc od2;
    yau_capture_desc cd2;
    memset(&od2, 0, sizeof od2);
    memset(&cd2, 0, sizeof cd2);
    if (!yau_open(&au2, &od2)) { fprintf(stderr, "child: %s\n", yau_error(&au2)); return 3; }
    if (!yau_capture_open(&cap2, &cd2)) { fprintf(stderr, "child: %s\n", yau_capture_error(&cap2)); return 4; }
    yrt_sleep_ns(YRT_MS(500));
    exit(0);   /* both open: the atexit handler resumes the card */
}

int main(void) {
    yau_capture_desc d;
    yau_capture_info in;
    char id[64];
    const char* dev = getenv("YAU_ALSA_TEST_DEVICE");
    const char* req = getenv("YAU_ALSA_TEST_REQUIRE");
    int n, i, j, found = 0, dup = -1, uniq = -1, p;
    if (getenv("YAU_ALSA_TEST_EXIT_CHILD")) return exit_child();
    if (!dev || !dev[0]) dev = "hw:0,0";
    snprintf(id, sizeof id, ":%s", strncmp(dev, "hw:", 3) == 0 ? dev + 3 : dev);
    yrt_ring_open(&g_ring, &(yrt_ring_desc){ .memory = g_ring_mem, .bytes = sizeof g_ring_mem });
    /* 0 */
    {
        int held = proc_held(dev);
        memset(&d, 0, sizeof d);
        if (!yau_capture_open(&g_cap, &d)) {
            if (req && req[0] == '1') {
                fprintf(stderr, "audio_alsa_capture_test: FAIL AUTO open: %s\n", yau_capture_error(&g_cap));
                return 1;
            }
            printf("audio_alsa_capture_test: SKIP (AUTO: %s)\n", yau_capture_error(&g_cap));
            return 0;
        }
        (void)run(1.0, 0);
        info_line("AUTO");
        yau_capture_get_info(&g_cap, &in);
        CHECK(in.exclusive);
        CHECK(in.in == YAU_OUT_S32);
        CHECK(in.pos_source == YAU_POS_DEVICE);
        CHECK(!strcmp(in.os_latency_src, "alsa:status"));
        CHECK(in.tier == YAU_TIER_3);
        CHECK(proc_running(dev, 'c'));
        {
            char line[640];
            yau_capture_describe(&g_cap, line, sizeof line);
            CHECK(strstr(line, "backend=ALSA") != NULL);
            if (held) CHECK(strstr(line, "sound-server-suspended") != NULL);
            else CHECK(strstr(line, "sound-server-suspended") == NULL);
        }
        yau_capture_close(&g_cap);
        if (held) {
            yrt_sleep_ns(YRT_MS(300));
            memset(&d, 0, sizeof d);
            d.keep_sound_server = true;
            CHECK(!yau_capture_open(&g_cap, &d));
            printf("keep_sound_server: %s\n", yau_capture_error(&g_cap));
            CHECK(strstr(yau_capture_error(&g_cap), "YAU_BACKEND_PULSE") != NULL);
        } else {
            printf("the sound server does not hold %s's capture PCM: no suspension to check\n", dev);
        }
    }
    /* 1 */
    n = yau_list_inputs(YAU_BACKEND_ALSA, g_list, 64);
    printf("ALSA lists %d inputs\n", n);
    CHECK(n > 0);
    for (i = 0; i < n && i < 64; i++) {
        int shared = 0;
        if (!strcmp(g_list[i].id, id)) found = 1;
        for (j = 0; j < n && j < 64; j++)
            if (j != i && !strcmp(g_list[i].name, g_list[j].name)) shared = 1;
        if (shared && dup < 0) dup = i;
        if (!shared && g_list[i].id[0] == ':' && uniq < 0) uniq = i;
    }
    CHECK(found);
    /* 2 */
    memset(&d, 0, sizeof d);
    d.backend = YAU_BACKEND_ALSA;
    d.exclusive = true;
    CHECK(!yau_capture_open(&g_cap, &d));
    printf("no device: %s\n", yau_capture_error(&g_cap));
    CHECK(strstr(yau_capture_error(&g_cap), "needs a card's device") != NULL);
    {
        /* "ALC" or the card name: in every analog input's name here */
        const char* part = NULL;
        for (i = 0; i < n && i < 64 && !part; i++)
            for (j = i + 1; j < n && j < 64; j++)
                if (g_list[i].id[0] == ':' && g_list[j].id[0] == ':' && strncmp(g_list[i].name, g_list[j].name, 8) == 0) {
                    static char pre[9];
                    memcpy(pre, g_list[i].name, 8);
                    pre[8] = 0;
                    part = pre;
                    break;
                }
        if (dup >= 0) part = g_list[dup].name;
        if (part) {
            d.device = part;
            CHECK(!yau_capture_open(&g_cap, &d));
            printf("shared name part '%s': %s\n", part, yau_capture_error(&g_cap));
            CHECK(strstr(yau_capture_error(&g_cap), "give one's id") != NULL);
        }
    }
    if (uniq >= 0) {
        d.device = g_list[uniq].name;
        CHECK(yau_capture_open(&g_cap, &d));
        if (yau_capture_is_open(&g_cap)) {
            yau_capture_get_info(&g_cap, &in);
            printf("unique name '%s' opened '%s'\n", g_list[uniq].name, in.name);
            yau_capture_close(&g_cap);
        }
    }
    d.device = id;
    CHECK(yau_capture_open(&g_cap, &d));
    if (yau_capture_is_open(&g_cap)) yau_capture_close(&g_cap);
    /* 3 */
    for (p = 0; p < 2; p++) {
        int64_t t_now, t_new, buf_ns;
        memset(&d, 0, sizeof d);
        d.backend = YAU_BACKEND_ALSA;
        d.exclusive = true;
        d.device = dev;
        d.period = p ? 480 : 64;
        if (!yau_capture_open(&g_cap, &d)) {
            fprintf(stderr, "audio_alsa_capture_test: FAIL open %s at %d: %s\n", dev, (int)d.period, yau_capture_error(&g_cap));
            return 1;
        }
        g_q_min = INT64_MAX;
        g_q_max = INT64_MIN;
        (void)run(10.0, 0);
        info_line(p ? "480 frames" : "64 frames");
        yau_capture_get_info(&g_cap, &in);
        printf("backlog %lld to %lld frames, buffer %d\n", (long long)g_q_min, (long long)g_q_max, (int)in.buffer);
        CHECK(in.pos_source == YAU_POS_DEVICE);
        CHECK(in.xruns == 0 && in.lost == 0 && in.dropped == 0);
        CHECK(in.fit_spread_ns > 0 && in.fit_spread_ns < 50000);
        CHECK(fabs(in.drift_ppm) < 100);
        CHECK(g_q_min >= 0);
        CHECK(g_q_max < in.buffer);
        t_now = yrt_now_ns();
        t_new = yau_capture_time_of(&g_cap, in.frames - 1);
        buf_ns = (int64_t)in.buffer * 1000000000LL / in.rate;
        printf("the newest frame was recorded %.2f ms ago\n", (double)(t_now - t_new) / 1e6);
        CHECK(t_new < t_now);
        CHECK(t_new > t_now - buf_ns - 50000000LL);
        yau_capture_close(&g_cap);
    }
    /* 4 */
    {
        int64_t buf_ns, f_ref = 0, t_ref, worst = 0, x;
        yau__fit before;
        yrt_event ev[256];
        int m, xr = 0;
        memset(&d, 0, sizeof d);
        d.backend = YAU_BACKEND_ALSA;
        d.exclusive = true;
        d.device = dev;
        d.period = 64;
        d.ring = &g_ring;
        CHECK(yau_capture_open(&g_cap, &d));
        if (!yau_capture_is_open(&g_cap)) return 1;
        yau_capture_get_info(&g_cap, &in);
        buf_ns = (int64_t)in.buffer * 1000000000LL / in.rate;
        CHECK(buf_ns >= 40000000);   /* 40 ms at 64 frames: 30 periods */
        if (buf_ns < 40000000) buf_ns = 40000000;
        (void)run(2.0, 0);
        g_stall_ns = buf_ns - 15000000;   /* rides it out */
        (void)run(1.0, 0);
        yau_capture_get_info(&g_cap, &in);
        printf("a stall of %.1f ms (buffer %.1f ms): xruns %u, lost %lld\n", (double)(buf_ns - 15000000) / 1e6,
               (double)buf_ns / 1e6, (unsigned)in.xruns, (long long)in.lost);
        CHECK(in.xruns == 0 && in.lost == 0);
        (void)run(4.0, 0);
        before = g_cap.fit_ft;
        while (yrt_ring_drain(&g_ring, ev, 256) > 0) {}
        g_stall_ns = buf_ns + 30000000;
        g_stall_t = 0;
        (void)run(7.0, 0);
        yau_capture_get_info(&g_cap, &in);
        printf("a stall of %.1f ms: xruns %u, lost %lld frames (%.2f ms)\n", (double)(buf_ns + 30000000) / 1e6,
               (unsigned)in.xruns, (long long)in.lost, (double)in.lost / in.rate * 1e3);
        CHECK(in.xruns == 1);
        CHECK(in.lost > 0);
        while ((m = yrt_ring_drain(&g_ring, ev, 256)) > 0)
            for (j = 0; j < m; j++)
                if (ev[j].source == YRT_SRC_AUDIO && ev[j].kind == YAU_EV_CAP_XRUN) {
                    double found_ms = ((double)(int64_t)ev[j].t_ns - (double)g_stall_t) / 1e6;
                    xr++;
                    f_ref = ev[j].u.i64[0] + ev[j].u.i64[1];
                    printf("  record: at frame %lld, %lld lost, %lld numbered from before, found %.2f ms after the "
                           "stall began\n", (long long)ev[j].u.i64[0], (long long)ev[j].u.i64[1],
                           (long long)(ev[j].u.i64[0] - ev[j].u.i64[2]), found_ms);
                    CHECK(ev[j].u.i64[2] == ev[j].u.i64[0]);
                    CHECK(found_ms < (double)(buf_ns + 30000000) / 1e6 + 5.0);
                }
        CHECK(xr == 1);
        /* the frames after the loss, on the line from before it */
        if (f_ref > 0 && before.ready)
            for (x = f_ref; x < f_ref + 5 * 48000 && x < in.frames; x += 997) {
                t_ref = yau__fit_time(&before, x) - yau_capture_time_of(&g_cap, x) + g_cap.desc.offset_ns;
                if (t_ref < 0) t_ref = -t_ref;
                if (t_ref > worst) worst = t_ref;
            }
        printf("  frames after it: at most %.1f us off the fit from before\n", (double)worst / 1e3);
        CHECK(f_ref > 0 && worst < 100000);
        /* WATCHDOG: a 300 ms stall; the frame thread asks every 2 ms */
        {
            int64_t end, t_lost = 0, t_back = 0;
            int rc;
            g_stall_t = 0;
            g_stall_ns = 300000000;
            end = yrt_now_ns() + 2000000000LL;
            while (yrt_now_ns() < end) {
                int64_t k, first;
                rc = yau_capture_update(&g_cap);
                if (rc == YAU_ERR_LOST && !t_lost) t_lost = yrt_now_ns();
                if (rc >= 0 && t_lost && !t_back) t_back = yrt_now_ns();
                while ((k = yau_capture_read(&g_cap, 0, g_buf, 48000, &first)) > 0) {}
                yrt_sleep_ns(YRT_MS(2));
            }
            yau_capture_get_info(&g_cap, &in);
            printf("a stall of 300 ms: LOST %.1f ms after it began, back %.1f ms after; xruns %u, lost %.2f ms in all\n",
                   t_lost && g_stall_t ? (double)(t_lost - g_stall_t) / 1e6 : -1.0,
                   t_back && g_stall_t ? (double)(t_back - g_stall_t) / 1e6 : -1.0, (unsigned)in.xruns,
                   (double)in.lost / in.rate * 1e3);
            CHECK(t_lost && g_stall_t && t_lost - g_stall_t > 195000000LL && t_lost - g_stall_t < 215000000LL);
            CHECK(t_back && t_back - g_stall_t > 300000000LL && t_back - g_stall_t < 320000000LL);
            CHECK(in.xruns == 2);
        }
        yau_capture_close(&g_cap);
    }
    /* 5 */
    {
        yau_desc od;
        yau_caps oc;
        char line[640];
        memset(&od, 0, sizeof od);
        memset(&d, 0, sizeof d);
        od.period = 64;
        od.capture = &g_cap;
        od.capture_desc = &d;
        if (!yau_open(&g_au, &od)) {
            fprintf(stderr, "audio_alsa_capture_test: FAIL duplex open: %s\n", yau_error(&g_au));
            g_failures++;
        } else {
            (void)run(10.0, 1);
            (void)yau_update(&g_au);
            yau_get_caps(&g_au, &oc);
            yau_describe(&g_au, line, sizeof line);
            printf("duplex output: %s\n", line);
            info_line("duplex input");
            yau_capture_get_info(&g_cap, &in);
            CHECK(in.duplex && in.exclusive);
            /* DUPLEX START: the silence written before the start */
            CHECK(((yau__ma*)(void*)g_au.backend_mem)->al_prefill == (int64_t)YAU__DUPLEX_PREFILL * oc.period);
            CHECK(oc.buffer == YAU__DUPLEX_PERIODS * oc.period);
            CHECK(oc.pos_source == YAU_POS_DEVICE && in.pos_source == YAU_POS_DEVICE);
            CHECK(oc.xruns == 0);
            CHECK(in.xruns == 0 && in.lost == 0);
            CHECK(proc_running(dev, 'c') && proc_running(dev, 'p'));
            yau_close(&g_au);
        }
    }
    /* 5b: a duplex device whose reads miniaudio splits into callbacks: a
     * 16-bit input beside a 32-bit output at 960 frames reads up to 960
     * frames and calls back 512 + 448 (the 512 not reported); at 1024,
     * 512 + 512 (each held one callback, the second reported) */
    {
        static const int32_t periods[2] = { 960, 1024 };
        int k;
        for (k = 0; k < 2; k++) {
            yau_desc od;
            yau_caps oc;
            char line[640];
            double sd;
            memset(&od, 0, sizeof od);
            memset(&d, 0, sizeof d);
            od.period = periods[k];
            d.format.sample = YAU_S16;
            od.capture = &g_cap;
            od.capture_desc = &d;
            g_nrep = 0;
            g_last_t = -1;
            g_keep = 1;
            if (!yau_open(&g_au, &od)) {
                fprintf(stderr, "audio_alsa_capture_test: FAIL split duplex open: %s\n", yau_error(&g_au));
                g_failures++;
                g_keep = 0;
                continue;
            }
            (void)run(k ? 6.0 : 10.0, 1);
            (void)yau_update(&g_au);
            g_keep = 0;
            yau_get_caps(&g_au, &oc);
            yau_describe(&g_au, line, sizeof line);
            printf("split duplex output: %s\n", line);
            info_line("split duplex input");
            yau_capture_get_info(&g_cap, &in);
            sd = rep_sd(g_nrep / 10);
            printf("split duplex at %d: %d reports, %.2f us sd from a line through them; chunk %d frames\n",
                   (int)oc.period, g_nrep, sd / 1e3, (int)((yau__ma*)(void*)g_au.backend_mem)->cap_al.chunk);
            CHECK(oc.period == periods[k]);
            CHECK(((yau__ma*)(void*)g_au.backend_mem)->cap_al.chunk == 512);
            CHECK(in.in == YAU_OUT_S16 && oc.out == YAU_OUT_S32);
            CHECK(in.xruns == 0 && in.lost == 0 && in.set_aside == 0);
            CHECK(in.fit_spread_ns > 0 && in.fit_spread_ns < 50000);
            /* 10 to 31 us measured; a callback reported short is 9 to 11 ms off */
            CHECK(sd < 100000);
            yau_close(&g_au);
        }
    }
    /* 5d: the capture desc's keep_sound_server holds for the duplex
     * device too (the output's card is busy: the server holds it) */
    if (system("pactl info > /dev/null 2>&1") == 0) {
        yau_desc od;
        memset(&od, 0, sizeof od);
        memset(&d, 0, sizeof d);
        d.keep_sound_server = true;
        od.capture = &g_cap;
        od.capture_desc = &d;
        CHECK(!yau_open(&g_au, &od));
        printf("duplex, the input's keep_sound_server: %s\n", yau_error(&g_au));
        CHECK(strstr(yau_error(&g_au), "YAU_BACKEND_PULSE") != NULL);
        if (yau_is_open(&g_au)) yau_close(&g_au);
    }
    /* 6: two hw: devices on one card. The output's open suspends the
     * card's sound server; the input that opens after it holds the card
     * too, so the output's close does not resume the card under it, and
     * the input's close does (the card's last). Then exit() with both
     * open, in a child: the card is resumed. */
    {
        yau_desc od;
        int server = system("pactl info > /dev/null 2>&1") == 0;
        memset(&od, 0, sizeof od);
        memset(&d, 0, sizeof d);
        CHECK(yau_open(&g_au, &od));
        CHECK(yau_capture_open(&g_cap, &d));
        if (yau_is_open(&g_au) && yau_capture_is_open(&g_cap)) {
            char line[640];
            yau_capture_describe(&g_cap, line, sizeof line);
            if (server) CHECK(strstr(line, "sound-server-suspended") != NULL);
            yau_close(&g_au);
            yrt_sleep_ns(YRT_MS(300));
            if (server) CHECK(sinks_suspended());
            printf("output closed first: the card %s\n", sinks_suspended() ? "stays suspended" : "was resumed");
            yau_capture_close(&g_cap);
            yrt_sleep_ns(YRT_MS(300));
            CHECK(!sinks_suspended());
        }
        if (server) {
            /* a new program: libpulse refuses a forked child of a process
             * that used it */
            pid_t pid = fork();
            if (pid == 0) {
                char* args[] = { (char*)"audio_alsa_capture_test", NULL };
                setenv("YAU_ALSA_TEST_EXIT_CHILD", "1", 1);
                execv("/proc/self/exe", args);
                _exit(5);
            } else if (pid > 0) {
                int st = 0;
                waitpid(pid, &st, 0);
                yrt_sleep_ns(YRT_MS(300));
                printf("exit() with both open: child %d, the card %s\n", WIFEXITED(st) ? WEXITSTATUS(st) : -1,
                       sinks_suspended() ? "stays suspended" : "was resumed");
                CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
                CHECK(!sinks_suspended());
            }
        }
    }
    if (g_failures) return 1;
    printf("audio_alsa_capture_test: all checks passed\n");
    return 0;
}
