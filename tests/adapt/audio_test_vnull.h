/* audio_test_vnull.h - BACKEND_NULL on ysp/rt.h's virtual clock (v0.6.0,
 * VIRTUAL CLOCK in ysp/audio.h), included by audio_test.c. The test's own
 * clock seam is off here (g_virtual 0), so the header reads ysp/rt.h's
 * clock, which yrt_virtual_begin() makes virtual: the null device's
 * blocks then come from a virtual timer at their own times. */

typedef struct vn_run {
    int64_t t0, onset[3], start[3], residual[3], rendered[3];
    int     rc[3], tier[3];
} vn_run;

static yau_audio g_vn;

/* Opens, plays three clicks at off-grid times, closes: one run. */
static void vn_one(vn_run* r) {
    yau_desc d;
    yau_caps c;
    yau_buf b;
    char line[512];
    int i;
    yau_id id[3];
    float* x;
    int64_t t_open;
    memset(r, 0, sizeof *r);
    memset(&d, 0, sizeof d);
    d.backend = YAU_BACKEND_NULL;
    d.arena_bytes = 1 << 20;
    t_open = yrt_now_ns();
    CHECK(yau_open(&g_vn, &d));
    if (!yau_is_open(&g_vn)) { fprintf(stderr, "audio_test: %s\n", yau_error(&g_vn)); return; }
    yau_get_caps(&g_vn, &c);
    CHECK_I(c.period, 480);
    CHECK_I(c.buffer, 1440);
    /* The slope is exact; the spread is not 0 where the compiler fuses
     * multiply-adds (Apple arm64 clang by default; gcc -march=native
     * -ffp-contract=fast measured 1.5e-8 ns), so it gets 1e-6 ns. */
    CHECK(c.drift_ppm == 0.0 && c.fit_spread_ns < 1e-6);
    /* the device starts inside open: stream frame 0 plays at the open's time */
    CHECK(llabs(yau_time_of(&g_vn, 0) - t_open) <= 1);
    yau_describe(&g_vn, line, sizeof line);
    CHECK(strstr(line, "backend=null (virtual clock)") != NULL && strstr(line, "device='virtual null'") != NULL);
    b = yau_alloc(&g_vn, 96, 2);
    x = b.frames;
    for (i = 0; x && i < 96 * 2; i++) x[i] = 0.5f;
    r->t0 = yrt_now_ns();
    for (i = 0; i < 3; i++) id[i] = yau_play_at(&g_vn, b, r->t0 + YRT_MS(100 + 37 * i) + 7777 * i);
    (void)yrt_sleep_until(r->t0 + YRT_MS(400), 0);
    for (i = 0; i < 3; i++) {
        yau_onset o;
        memset(&o, 0, sizeof o);
        r->rc[i] = yau_result(&g_vn, id[i], &o);
        r->onset[i] = o.onset - r->t0;
        r->start[i] = o.start_frame;
        r->residual[i] = o.residual;
        r->rendered[i] = o.rendered_at - r->t0;
        r->tier[i] = o.tier;
    }
    yau_close(&g_vn);
}

static void test_virtual_null(void) {
    vn_run a, b;
    int i;
    yau_desc d;
    g_virtual = 0;   /* YAU__NOW and YAU__SLEEP_UNTIL: ysp/rt.h's clock */
    CHECK_I(yrt_virtual_begin(0), 0);

    /* a real device on the virtual clock is refused */
    memset(&d, 0, sizeof d);
    d.backend = YAU_BACKEND_WASAPI;
    CHECK(!yau_open(&g_vn, &d));
    CHECK(strstr(yau_error(&g_vn), "virtual") != NULL);

    vn_one(&a);
    for (i = 0; i < 3; i++) {
        CHECK_I(a.rc[i], YAU_OK);
        CHECK_I(a.tier[i], YAU_TIER_SIM);
        /* on the plan within half a sample: the plan meets the sample grid */
        CHECK(llabs(a.residual[i]) <= 1000000000 / 48000 / 2 + 1);
    }
    /* the starts are the plan's frames: 100, 137 and 174 ms after t0 plus
     * the 7.8 us steps, from the open's first block */
    CHECK(a.start[1] - a.start[0] == (int64_t)llround((YRT_MS(37) + 7777) * 48000.0 / 1e9));
    vn_one(&b);
    /* a second run, later on the clock: the same to the ns from its t0 */
    CHECK(memcmp(a.onset, b.onset, sizeof a - offsetof(vn_run, onset)) == 0);
    for (i = 0; i < 3; i++)
        if (a.onset[i] != b.onset[i] || a.start[i] != b.start[i] || a.residual[i] != b.residual[i] || a.rendered[i] != b.rendered[i])
            fprintf(stderr, "audio_test: virtual null run 1 and 2, click %d: onset %lld %lld start %lld %lld residual %lld %lld rendered %lld %lld\n",
                    i, (long long)a.onset[i], (long long)b.onset[i], (long long)a.start[i], (long long)b.start[i],
                    (long long)a.residual[i], (long long)b.residual[i], (long long)a.rendered[i], (long long)b.rendered[i]);

    {   /* the capture side: a virtual null input moves with the clock */
        static yau_capture cap;
        yau_capture_desc cd;
        yau_capture_info ci;
        int64_t t;
        memset(&cd, 0, sizeof cd);
        cd.backend = YAU_BACKEND_NULL;
        cd.format.channels = 1;
        CHECK(yau_capture_open(&cap, &cd));
        t = yrt_now_ns();
        (void)yrt_sleep_until(t + YRT_S(1), 0);
        CHECK(yau_capture_update(&cap) >= 0);
        yau_capture_get_info(&cap, &ci);
        CHECK(ci.fit_ready && ci.tier == YAU_TIER_SIM && ci.drift_ppm == 0.0);
        CHECK(strcmp(ci.name, "virtual null") == 0);
        yau_capture_close(&cap);
        memset(&cd, 0, sizeof cd);
        cd.backend = YAU_BACKEND_WASAPI;
        CHECK(!yau_capture_open(&cap, &cd));

        /* DUPLEX: one virtual device for the output and a 4-channel input */
        memset(&cd, 0, sizeof cd);
        cd.format.channels = 4;
        memset(&d, 0, sizeof d);
        d.backend = YAU_BACKEND_NULL;
        d.arena_bytes = 1 << 20;
        d.capture = &cap;
        d.capture_desc = &cd;
        CHECK(yau_open(&g_vn, &d));
        if (yau_is_open(&g_vn)) {
            int64_t f0;
            CHECK(yau_capture_update(&cap) >= 0);
            yau_capture_get_info(&cap, &ci);
            f0 = ci.frames;
            t = yrt_now_ns();
            (void)yrt_sleep_until(t + YRT_S(1), 0);
            CHECK(yau_capture_update(&cap) >= 0);
            yau_capture_get_info(&cap, &ci);
            CHECK(ci.duplex && ci.fit_ready && ci.channels == 4 && ci.drift_ppm == 0.0);
            CHECK_I(ci.frames - f0, 48000);   /* 100 blocks of 480 in 1 s */
            yau_close(&g_vn);
        } else fprintf(stderr, "audio_test: %s\n", yau_error(&g_vn));
    }

    yrt_virtual_end();
    g_virtual = 1;
}
