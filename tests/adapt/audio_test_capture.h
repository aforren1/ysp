/* audio_test_capture.h - the capture cases of audio_test.c (v0.5.0):
 * included by it, after its harness and the identity samples idv().
 *
 * A scripted input: device frame w is recorded at true time T0 + w * kd
 * (the device clock at a set drift), a block of a period is handed over
 * once its last frame and Lin more are in, late by a seeded exponential,
 * with a position report (the frame being recorded at a pass a little
 * before the call, stamped with that frame's true time) whose stamp
 * changes every report_every blocks, as WASAPI's capture clock was
 * measured to do. A loss skips device frames; a stall makes one block
 * late. Every sample is a function of its device frame (idv(), exact in
 * float), so a reader can check the frame of every sample it gets. */

typedef struct scap {
    uint32_t rate;
    uint16_t channels, out;
    int32_t  period, buffer, pos_source, tier;
    double   drift_ppm;
    int64_t  Lin;               /* frames recorded after a block before it is handed over */
    double   jitter_ns;         /* mean callback lateness                      */
    double   report_noise_ns;   /* mean lateness of a report's pass            */
    int      report_every;      /* the stamp changes every this many blocks    */
    uint64_t rng;
    int64_t  T0;
    int64_t  k, dev_next;       /* blocks handed over; the next device frame   */
    int64_t  skip;              /* device frames the next block loses          */
    int64_t  stall_ns;          /* the next block is this much later           */
    int64_t  last_pos, last_stamp, last_t;
    float  (*sig)(struct scap* s, int64_t w, int c);   /* NULL: idv(w, c)      */
    void*    host;
    yau_render_fn block;
    int      started, stopped, closed;
    unsigned char in_mem[8192 * 8 * 4];
} scap;

static scap g_sc;

static double sc_kd(const scap* s) { return 1e9 / (double)s->rate / (1.0 + s->drift_ppm * 1e-6); }
/* the true time device frame w was recorded */
static int64_t sc_truth(const scap* s, double w) { return s->T0 + (int64_t)floor(w * sc_kd(s) + 0.5); }
static double sc_rand(scap* s) {
    s->rng = s->rng * 6364136223846793005ull + 1442695040888963407ull;
    return ((double)(s->rng >> 11) + 0.5) / 9007199254740992.0;
}

static int sc_open(void* ctx, const yau_device_open* in, yau_device_caps* caps, char* err, size_t cap) {
    scap* s = (scap*)ctx;
    (void)err; (void)cap;
    CHECK_I(in->capture, 1);
    memset(caps, 0, sizeof *caps);
    caps->rate = s->rate;
    caps->channels = s->channels;
    caps->out = s->out;
    caps->period = s->period;
    caps->buffer = s->buffer;
    caps->pos_source = s->pos_source;
    caps->tier = s->tier;
    snprintf(caps->name, sizeof caps->name, "scripted input");
    return 0;
}
static int sc_start(void* ctx, yau_render_fn b, void* host) {
    scap* s = (scap*)ctx;
    s->block = b; s->host = host; s->started = 1;
    return 0;
}
static void sc_stop(void* ctx) { ((scap*)ctx)->stopped = 1; }
static void sc_close(void* ctx) { ((scap*)ctx)->closed = 1; }
static const yau_device g_capdev = { YAU_DEVICE_VERSION, "scripted input", sc_open, sc_start, sc_stop, sc_close, NULL };

static void sc_default(scap* s) {
    memset(s, 0, sizeof *s);
    s->rate = 48000; s->channels = 2; s->out = YAU_OUT_F32;
    s->period = 480; s->buffer = 1056; s->pos_source = YAU_POS_DEVICE; s->tier = 0;
    s->drift_ppm = -43; s->Lin = 480; s->jitter_ns = 1000000; s->report_noise_ns = 20000;
    s->report_every = 2; s->rng = 777;
    s->T0 = g_vt + 3000000;
    s->last_pos = -1;
}

/* One block: the device frames from dev_next (after any it lost), handed
 * over when its last frame and Lin more are recorded, late by the jitter. */
static void sc_block(scap* s) {
    double kd = sc_kd(s);
    int64_t i, n = s->period, w0, late, t;
    int c;
    yau_tick tk;
    s->dev_next += s->skip;
    s->skip = 0;
    w0 = s->dev_next;
    late = s->jitter_ns > 0 ? (int64_t)(-s->jitter_ns * log(sc_rand(s))) : 0;
    t = s->T0 + (int64_t)floor((double)(w0 + n + s->Lin) * kd + 0.5) + late + s->stall_ns;
    s->stall_ns = 0;
    /* after a stall the blocks come in order, at once, until they catch up */
    if (t < s->last_t) t = s->last_t + 2000;
    s->last_t = t;
    if (t > g_vt) g_vt = t;
    for (i = 0; i < n; i++)
        for (c = 0; c < s->channels; c++) {
            float v = s->sig ? s->sig(s, w0 + i, c) : idv(w0 + i, c);
            if (s->out == YAU_OUT_S16) ((int16_t*)(void*)s->in_mem)[i * s->channels + c] = (int16_t)floor((double)v * 32768.0 + 0.5);
            else ((float*)(void*)s->in_mem)[i * s->channels + c] = v;
        }
    memset(&tk, 0, sizeof tk);
    tk.t_entry = t;
    tk.pos = -1;
    if (s->pos_source == YAU_POS_DEVICE) {
        if (s->k % s->report_every == 0 || s->last_pos < 0) {
            int64_t pass = t - (s->report_noise_ns > 0 ? (int64_t)(-s->report_noise_ns * log(sc_rand(s))) : 0);
            int64_t f = (int64_t)floor((double)(pass - s->T0) / kd);
            s->last_pos = f;
            s->last_stamp = s->T0 + (int64_t)floor((double)f * kd + 0.5) + (t - pass);
        }
        tk.pos = s->last_pos;
        tk.pos_t = s->last_stamp;
    }
    s->block(s->host, s->in_mem, (int32_t)n, &tk);
    s->dev_next += n;
    s->k++;
}
static void sc_run_s(scap* s, double sec) {
    int64_t i, b = (int64_t)(sec * s->rate / s->period + 0.5);
    for (i = 0; i < b; i++) sc_block(s);
}

static yau_capture g_cap;
static float g_capmem[48000 * 2 * 2];
static unsigned char g_cring_mem[YRT_RING_BYTES(4096)];
static yrt_ring g_cring;

static bool cap_open(scap* s, yau_capture_desc* d) {
    yau_capture_desc dd;
    if (d) dd = *d; else memset(&dd, 0, sizeof dd);
    dd.backend = YAU_BACKEND_CUSTOM;
    dd.dev = &g_capdev;
    dd.dev_ctx = s;
    if (!dd.memory) { dd.memory = g_capmem; if (!dd.frames) dd.frames = 48000; }
    if (!dd.ring) { yrt_ring_open(&g_cring, &(yrt_ring_desc){ .memory = g_cring_mem, .bytes = sizeof g_cring_mem }); dd.ring = &g_cring; }
    return yau_capture_open(&g_cap, &dd);
}

static float g_rd[48000 * 2];

/* Reads everything reader r has and checks each sample against its device
 * frame: dev = capture frame + shift. Returns frames read; *wrong counts
 * samples that are not their frame's. */
static int64_t cap_drain(int r, int64_t shift, int64_t* wrong, int64_t* last_frame) {
    int64_t total = 0, n, f;
    const float* p;
    int C = g_cap.dcaps.channels;
    while ((p = yau_capture_acquire(&g_cap, r, &n, &f, NULL)) != NULL) {
        int64_t i;
        int c;
        for (i = 0; i < n; i++)
            for (c = 0; c < C; c++)
                if (p[i * C + c] != idv(f + i + shift, c)) (*wrong)++;
        if (last_frame) *last_frame = f + n - 1;
        CHECK_I(yau_capture_release(&g_cap, r, n), 0);
        total += n;
    }
    return total;
}

/* The fit maps capture frames to the ysp_rt clock: from reports, each
 * frame's time within a few microseconds of the true time it was recorded;
 * from callbacks, the same plus the constant the callback convention adds
 * (the frame after the block reaches it Lin frames later), which a line-in
 * test puts in desc.offset_ns. Drift +-100 ppm, report stamps that change
 * every other block, callbacks late by an exponential with a 1 ms mean. */
static void test_capture_fit(void) {
    static const double drifts[3] = { -43, 100, -100 };
    int src, di;
    for (src = 0; src < 2; src++)
        for (di = 0; di < 3; di++) {
            scap* s = &g_sc;
            yau_capture_info in;
            int64_t worst = 0, wrong = 0, f;
            double off;
            sc_default(s);
            s->drift_ppm = drifts[di];
            if (src == 1) s->pos_source = YAU_POS_CALLBACK;
            CHECK(cap_open(s, NULL));
            sc_run_s(s, 12.0);
            CHECK(cap_drain(0, 0, &wrong, NULL) > 0);
            CHECK_I(wrong, 0);
            (void)yau_capture_update(&g_cap);
            yau_capture_get_info(&g_cap, &in);
            CHECK(in.fit_ready);
            CHECK_I(in.tier, YAU_TIER_3);
            CHECK_I(in.pos_source, src == 0 ? YAU_POS_DEVICE : YAU_POS_CALLBACK);
            /* the same bound as playback's fit test: one seed, so the
             * error is the same noise at every drift */
            CHECK(fabs(in.drift_ppm - s->drift_ppm) < 0.5);
            off = src == 0 ? 0 : (double)s->Lin * sc_kd(s);
            for (f = s->dev_next - 48000; f < s->dev_next; f += 997) {
                int64_t e = yau_capture_time_of(&g_cap, f) - sc_truth(s, (double)f) - (int64_t)off;
                if (e < 0) e = -e;
                if (e > worst) worst = e;
            }
            /* the fractional form agrees with the integer one */
            CHECK(llabs(yau_capture_time_at(&g_cap, (double)(s->dev_next - 100)) - yau_capture_time_of(&g_cap, s->dev_next - 100)) <= 1);
            CHECK(llabs(yau_capture_frame_at(&g_cap, yau_capture_time_of(&g_cap, s->dev_next - 7)) - (s->dev_next - 7)) <= 0);
            printf("capture fit (%s, %+.0f ppm): drift %+.4f ppm, worst time error %.2f us, spread %.0f ns\n",
                   src == 0 ? "reports" : "callbacks", s->drift_ppm, in.drift_ppm, (double)worst / 1e3, in.fit_spread_ns);
            CHECK(worst < (src == 0 ? 15000 : 30000));
            CHECK_I(in.xruns, 0);
            CHECK_I(in.set_aside, 0);
            yau_capture_close(&g_cap);
            CHECK(!yau_capture_is_open(&g_cap));
            CHECK(s->stopped && s->closed);
        }
}

/* A reader that falls behind: the ring drops what does not fit, counts
 * it, and the reader sees a DROP gap where writing resumed, with the
 * capture frames jumping by exactly the frames dropped; every sample on
 * either side is still its own frame. Two readers: the slower one decides. */
static void test_capture_overflow(void) {
    scap* s = &g_sc;
    yau_capture_desc d;
    yau_capture_info in;
    int64_t wrong = 0, n, f, last = -1, got;
    int gap = 0, k;
    const float* p;
    sc_default(s);
    s->pos_source = YAU_POS_CALLBACK;
    memset(&d, 0, sizeof d);
    d.frames = 4800;          /* 0.1 s: 10 blocks */
    d.readers = 2;
    CHECK(cap_open(s, &d));
    for (k = 0; k < 30; k++) sc_block(s);   /* nobody reads: 20 blocks dropped */
    yau_capture_get_info(&g_cap, &in);
    CHECK_I(in.dropped, 20 * 480);
    CHECK_I(in.drops, 1);
    CHECK_I(in.fill[0], 4800);
    CHECK_I(in.fill[1], 4800);
    /* reader 0 catches up; reader 1 still has its 10 blocks: nothing more
     * fits for either */
    got = cap_drain(0, 0, &wrong, &last);
    CHECK_I(got, 4800);
    CHECK_I(last, 4799);
    sc_block(s);
    yau_capture_get_info(&g_cap, &in);
    CHECK_I(in.dropped, 21 * 480);
    /* reader 1 reads: room again; the next block lands after the gap */
    got = cap_drain(1, 0, &wrong, NULL);
    CHECK_I(got, 4800);
    sc_block(s);
    sc_block(s);
    p = yau_capture_acquire(&g_cap, 0, &n, &f, &gap);
    CHECK(p != NULL);
    CHECK_I(gap, YAU_GAP_DROP);
    CHECK_I(f, 31 * 480);      /* blocks 10..30 were dropped: the frames jumped */
    CHECK_I(n, 960);
    if (p) CHECK(p[0] == idv(31 * 480, 0) && p[2 * 959 + 1] == idv(32 * 480 + 479, 1));
    CHECK_I(yau_capture_release(&g_cap, 0, n), 0);
    CHECK_I(yau_capture_release(&g_cap, 0, 1), YAU_ERR_ARG);   /* past what was written */
    gap = 0;
    p = yau_capture_acquire(&g_cap, 1, &n, &f, &gap);
    CHECK_I(gap, YAU_GAP_DROP);
    CHECK_I(f, 31 * 480);
    CHECK_I(yau_capture_release(&g_cap, 1, n), 0);
    CHECK_I(wrong, 0);
    /* the records: one DROP of 21 blocks from frame 4800 */
    {
        yrt_event ev[64];
        int m = yrt_ring_drain(&g_cring, ev, 64), j, drops = 0;
        for (j = 0; j < m; j++)
            if (ev[j].source == YRT_SRC_AUDIO && ev[j].kind == YAU_EV_CAP_DROP) {
                drops++;
                CHECK_I(ev[j].u.i64[0], 4800);
                CHECK_I(ev[j].u.i64[1], 21 * 480);
            }
        CHECK_I(drops, 1);
    }
    /* a copy across the wrap and a gap: read stops at the gap */
    {
        int64_t ff = -1;
        for (k = 0; k < 9; k++) sc_block(s);
        got = yau_capture_read(&g_cap, 0, g_rd, 48000, &ff);
        CHECK_I(got, 9 * 480);
        CHECK_I(ff, 33 * 480);
        CHECK(g_rd[0] == idv(33 * 480, 0) && g_rd[2 * (got - 1) + 1] == idv(33 * 480 + got - 1, 1));
        (void)cap_drain(1, 0, &wrong, NULL);
    }
    printf("capture overflow: %lld frames dropped in 1 run, readers resumed on frame %d\n", (long long)(21 * 480), 31 * 480);
    yau_capture_close(&g_cap);
}

/* A device that loses frames (an overrun). With reports the loss shows as
 * the position moving ahead of the frames handed over for good: the
 * header counts it in whole periods, numbers the frames across it, and
 * every later sample is still its device frame and its time the truth's.
 * A stall that loses nothing gives no record. Without reports, the blocks
 * that come late for 200 ms restart the fit. */
static void test_capture_loss(void) {
    scap* s = &g_sc;
    yau_capture_info in;
    int64_t wrong = 0, n, f, worst = 0, x;
    int gap = 0;
    const float* p;
    sc_default(s);
    s->jitter_ns = 300000;
    CHECK(cap_open(s, NULL));
    sc_run_s(s, 6.0);
    (void)cap_drain(0, 0, &wrong, NULL);
    /* a stall the device rides out: late, nothing lost */
    s->stall_ns = 60000000;
    sc_run_s(s, 1.0);
    (void)cap_drain(0, 0, &wrong, NULL);
    yau_capture_get_info(&g_cap, &in);
    CHECK_I(in.xruns, 0);
    CHECK_I(in.lost, 0);
    /* a loss of 6 periods */
    s->skip = 6 * 480;
    sc_run_s(s, 1.0);
    yau_capture_get_info(&g_cap, &in);
    CHECK_I(in.xruns, 1);
    CHECK_I(in.lost, 6 * 480);
    /* the blocks between the loss and its detection (200 ms) are numbered
     * as if nothing was lost; after the gap every frame is its own */
    {
        int64_t before = 0, after = 0;
        int g = 0;
        while ((p = yau_capture_acquire(&g_cap, 0, &n, &f, &g)) != NULL) {
            int64_t i;
            gap |= g;
            for (i = 0; i < n; i++) {
                if (p[2 * i] == idv(f + i, 0)) after++;
                else before++;
            }
            CHECK_I(yau_capture_release(&g_cap, 0, n), 0);
        }
        CHECK_I(gap, YAU_GAP_LOST);
        CHECK(after > 0);
        printf("capture loss: 2880 frames lost, found after %lld frames (%.0f ms) numbered across it\n",
               (long long)before, (double)before / 48.0);
        CHECK(before <= 23 * 480 && before >= 19 * 480);
    }
    sc_run_s(s, 2.0);
    (void)cap_drain(0, 0, &wrong, NULL);
    CHECK_I(wrong, 0);
    (void)yau_capture_update(&g_cap);
    for (x = s->dev_next - 24000; x < s->dev_next; x += 101) {
        int64_t e = yau_capture_time_of(&g_cap, x) - sc_truth(s, (double)x);
        if (e < 0) e = -e;
        if (e > worst) worst = e;
    }
    CHECK(worst < 20000);
    {
        yrt_event ev[256];
        int m, j, xr = 0;
        while ((m = yrt_ring_drain(&g_cring, ev, 256)) > 0)
            for (j = 0; j < m; j++)
                if (ev[j].source == YRT_SRC_AUDIO && ev[j].kind == YAU_EV_CAP_XRUN) { xr++; CHECK_I(ev[j].u.i64[1], 6 * 480); }
        CHECK_I(xr, 1);
    }
    yau_capture_close(&g_cap);

    /* callbacks only: the loss restarts the fit, the times are right again */
    sc_default(s);
    s->pos_source = YAU_POS_CALLBACK;
    s->jitter_ns = 300000;
    CHECK(cap_open(s, NULL));
    sc_run_s(s, 6.0);
    (void)cap_drain(0, 0, &wrong, NULL);
    s->skip = 6 * 480;
    sc_run_s(s, 7.0);
    (void)cap_drain(0, 6 * 480, &wrong, NULL);
    (void)yau_capture_update(&g_cap);
    yau_capture_get_info(&g_cap, &in);
    CHECK_I(in.xruns, 1);
    CHECK_I(in.lost, 0);
    worst = 0;
    for (x = s->dev_next - 6 * 480 - 24000; x < s->dev_next - 6 * 480; x += 101) {
        int64_t e = yau_capture_time_of(&g_cap, x) - sc_truth(s, (double)(x + 6 * 480)) - (int64_t)((double)s->Lin * sc_kd(s));
        if (e < 0) e = -e;
        if (e > worst) worst = e;
    }
    printf("capture loss without reports: fit restarted, worst time error after %.1f us\n", (double)worst / 1e3);
    CHECK(worst < 60000);
    yau_capture_close(&g_cap);
    CHECK_I(wrong, 0);
}

/* --- detectors -------------------------------------------------------------- */

/* A speech-like burst: a 120 Hz harmonic complex (5 harmonics, 1/h), a
 * raised-cosine attack of attack_ms, then sustain, then a 30 ms decay, at
 * peak amplitude a; under it white noise at noise rms. All from the
 * frame index and a seeded generator: the same on every platform. */
typedef struct burst { int64_t start; double attack_ms, len_ms, a; } burst;
static burst g_bursts[16];
static int g_nbursts = 0;
static double g_noise_rms = 0;
static float speech(int64_t w) {
    double v = 0;
    int b;
    uint64_t z = (uint64_t)w * 0x9E3779B97F4A7C15ull;
    z ^= z >> 29; z *= 0xBF58476D1CE4E5B9ull; z ^= z >> 32;
    v += g_noise_rms * 1.7320508 * (2.0 * ((double)(z >> 11) / 9007199254740992.0) - 1.0);
    for (b = 0; b < g_nbursts; b++) {
        const burst* u = &g_bursts[b];
        double t = (double)(w - u->start) / 48.0, env, s = 0;
        int h;
        if (t < 0 || t >= u->len_ms + 30) continue;
        if (t < u->attack_ms) env = 0.5 * (1 - cos(3.14159265358979 * t / u->attack_ms));
        else if (t < u->len_ms) env = 1;
        else env = 0.5 * (1 + cos(3.14159265358979 * (t - u->len_ms) / 30.0));
        for (h = 1; h <= 5; h++) s += sin(2 * 3.14159265358979 * 120.0 * h * (double)(w - u->start) / 48000.0) / h;
        v += u->a * env * s / 2.28;
    }
    return (float)v;
}

/* The definition, computed the slow way: RMS over the window ending at each
 * frame, the first crossing of on, interpolated as the header does. */
static double ref_onset(const float* x, int64_t n, int64_t win, double on2) {
    int64_t i, j;
    double prev = 0;
    for (i = 0; i < n; i++) {
        double sum = 0, p;
        for (j = i - win + 1; j <= i; j++) if (j >= 0) sum += (double)x[j] * x[j];
        p = sum / (double)win;
        if (p >= on2) {
            double r0 = sqrt(prev), r1 = sqrt(p), l = sqrt(on2);
            return (double)(i - 1) + (l - r0) / (r1 - r0);
        }
        prev = p;
    }
    return -1;
}

static float g_sig[48000 * 4];

static int vkey_run(const yau_vkey_desc* d, const float* x, int64_t n, int64_t block, yau_vkey_event* ev, int cap) {
    static yau_vkey vk;
    int64_t i;
    int m = 0;
    CHECK_I(yau_vkey_init(&vk, d, 48000), 0);
    for (i = 0; i < n; i += block) {
        int64_t k = n - i < block ? n - i : block;
        int r = yau_vkey_feed(&vk, x + i, k, 1, 1000 + i, ev + m, cap - m);
        CHECK(r >= 0);
        if (r > 0) m += r;
    }
    return m;
}

static void test_vkey(void) {
    yau_vkey_desc d;
    yau_vkey_event ev[16], ev2[16];
    static yau_vkey vk;
    int64_t i, n = 48000 * 3;
    int m, m2, b;
    static const int64_t blocks[5] = { 1, 7, 256, 480, 4096 };
    memset(&d, 0, sizeof d);
    d.on_db = -40; d.off_db = -46;
    /* refusals */
    CHECK_I(yau_vkey_init(&vk, &d, 0), YAU_ERR_ARG);
    d.on_db = 0; CHECK_I(yau_vkey_init(&vk, &d, 48000), YAU_ERR_ARG);
    d.on_db = -40; d.off_db = -30; CHECK_I(yau_vkey_init(&vk, &d, 48000), YAU_ERR_ARG);
    d.off_db = -46; d.window_ns = YRT_MS(500); CHECK_I(yau_vkey_init(&vk, &d, 48000), YAU_ERR_ARG);
    d.window_ns = 0;
    CHECK_I(yau_vkey_init(&vk, &d, 48000), 0);
    CHECK_I(yau_vkey_feed(&vk, g_sig, 10, 1, 0, ev, 4), 0);
    d.channel = 2;
    CHECK_I(yau_vkey_init(&vk, &d, 48000), 0);
    CHECK_I(yau_vkey_feed(&vk, g_sig, 10, 2, 0, ev, 4), YAU_ERR_ARG);
    d.channel = 0;

    /* two words in noise at -60 dBFS: two onsets, two offsets */
    g_noise_rms = 0.001;
    g_nbursts = 2;
    g_bursts[0].start = 30000; g_bursts[0].attack_ms = 10; g_bursts[0].len_ms = 300; g_bursts[0].a = 0.1;
    g_bursts[1].start = 80000; g_bursts[1].attack_ms = 25; g_bursts[1].len_ms = 200; g_bursts[1].a = 0.05;
    for (i = 0; i < n; i++) g_sig[i] = speech(i);
    m = vkey_run(&d, g_sig, n, 480, ev, 16);
    CHECK_I(m, 4);
    if (m == 4) {
        double r0 = ref_onset(g_sig, n, 240, pow(10, -4.0)) + 1000, err0, err1;
        CHECK(ev[0].kind == YAU_VKEY_ON && ev[1].kind == YAU_VKEY_OFF && ev[2].kind == YAU_VKEY_ON && ev[3].kind == YAU_VKEY_OFF);
        CHECK(fabs(ev[0].frame - r0) < 1e-6);
        err0 = (ev[0].frame - 1000 - 30000) / 48.0;
        err1 = (ev[2].frame - 1000 - 80000) / 48.0;
        printf("voice key: onsets %.3f and %.3f ms after the bursts began (attack 10 and 25 ms, RMS -28.5 and "
               "-34.5 dBFS), offsets %.1f and %.1f ms after their decays\n", err0, err1,
               (ev[1].frame - 1000 - 30000) / 48.0 - 330, (ev[3].frame - 1000 - 80000) / 48.0 - 230);
        CHECK(err0 > 0 && err0 < 8);
        CHECK(err1 > 0 && err1 < 20);
        CHECK(ev[0].level_db > -30 && ev[0].level_db < -15);
    }
    /* the same events whatever the blocks */
    for (b = 0; b < 5; b++) {
        m2 = vkey_run(&d, g_sig, n, blocks[b], ev2, 16);
        CHECK_I(m2, m);
        if (m2 == m) for (i = 0; i < m; i++) CHECK(ev2[i].frame == ev[i].frame && ev2[i].kind == ev[i].kind);
    }
    /* noise alone, 10 dB under the threshold: nothing */
    g_nbursts = 0;
    g_noise_rms = 0.0032;
    for (i = 0; i < n; i++) g_sig[i] = speech(i);
    CHECK_I(vkey_run(&d, g_sig, n, 480, ev, 16), 0);
    /* a click (2 ms) is shorter than min_ns: rejected, no event */
    g_nbursts = 0;
    g_noise_rms = 0.0005;
    for (i = 0; i < n; i++) {
        g_sig[i] = speech(i);
        if (i >= 20000 && i < 20096) g_sig[i] += (float)(0.3 * sin((double)i * 0.4));
    }
    CHECK_I(yau_vkey_init(&vk, &d, 48000), 0);
    CHECK_I(yau_vkey_feed(&vk, g_sig, n, 1, 0, ev, 16), 0);
    CHECK_I((int)vk.rejected_, 1);
    /* hysteresis: a dip under on but over off ends nothing */
    for (i = 0; i < n; i++) {
        double a = i >= 10000 && i < 40000 ? (i >= 20000 && i < 22000 ? 0.009 : 0.03) : 0;
        g_sig[i] = (float)(a * sin((double)i * 0.3));
    }
    m = vkey_run(&d, g_sig, n, 480, ev, 16);
    CHECK_I(m, 2);
    /* a gap starts it over: no window carried across */
    CHECK_I(yau_vkey_init(&vk, &d, 48000), 0);
    CHECK_I(yau_vkey_feed(&vk, g_sig + 10000, 480, 1, 10000, ev, 16), 0);
    CHECK_I(yau_vkey_feed(&vk, g_sig + 30000, 4800, 1, 30000, ev, 16), 1);
    CHECK(ev[0].frame >= 30000);
    /* too many events for the caller's array: counted */
    CHECK_I(yau_vkey_init(&vk, &d, 48000), 0);
    CHECK_I(yau_vkey_feed(&vk, g_sig, n, 1, 0, ev, 1), 1);
    CHECK_I((int)vk.missed_, 1);
}

/* Edges at known fractional frames: a ramp crosses the level exactly where
 * the interpolation puts it; the dead time; the polarity. */
static void test_edge(void) {
    yau_edge e;
    yau_edge_desc d;
    double at[8];
    int i, m;
    memset(&d, 0, sizeof d);
    CHECK_I(yau_edge_init(&e, &d, 48000), 0);   /* 0 dBFS: |x| >= 1 */
    d.level_db = 1; CHECK_I(yau_edge_init(&e, &d, 48000), YAU_ERR_ARG);
    d.level_db = (float)(20.0 * log10(0.25));
    d.polarity = 2; CHECK_I(yau_edge_init(&e, &d, 48000), YAU_ERR_ARG);
    d.polarity = 0;
    for (i = 0; i < 48000; i++) g_sig[i] = 0;
    /* a ramp through 0.25 at frame 1000.4: 0.1 a frame from 0.21 at 1000 */
    for (i = 1000; i < 1010; i++) g_sig[i] = 0.21f + 0.1f * (float)(i - 1000);
    g_sig[999] = 0.11f;
    /* a second edge inside the dead time: not seen; a third after it, negative */
    g_sig[1500] = 0.9f;
    for (i = 5000; i < 5004; i++) g_sig[i] = -0.5f;
    g_sig[4999] = -0.125f;
    CHECK_I(yau_edge_init(&e, &d, 48000), 0);
    m = 0;
    for (i = 0; i < 48000; i += 480) {
        int r = yau_edge_feed(&e, g_sig + i, 480, 1, i, at + m, 8 - m);
        CHECK(r >= 0);
        if (r > 0) m += r;
    }
    CHECK_I(m, 2);
    if (m == 2) {
        double want0 = 1000.0 + (0.25 - (double)g_sig[1000]) / ((double)g_sig[1001] - (double)g_sig[1000]);
        double want1 = 4999.0 + (0.25 - 0.125) / (0.5 - 0.125);
        CHECK(fabs(at[0] - want0) < 1e-6);
        CHECK(fabs(at[1] - want1) < 1e-6);
        printf("edges at %.4f and %.4f (want %.4f, %.4f)\n", at[0], at[1], want0, want1);
    }
    /* positive only: the negative one is not an edge */
    d.polarity = 1;
    CHECK_I(yau_edge_init(&e, &d, 48000), 0);
    CHECK_I(yau_edge_feed(&e, g_sig, 48000, 1, 0, at, 8), 1);
    d.polarity = -1;
    CHECK_I(yau_edge_init(&e, &d, 48000), 0);
    CHECK_I(yau_edge_feed(&e, g_sig, 48000, 1, 0, at, 8), 1);
    CHECK(fabs(at[0] - 4999.3333333) < 1e-6);
    /* a level held past the dead time is one edge, not one per dead time */
    for (i = 0; i < 48000; i++) g_sig[i] = i >= 100 ? 0.5f : 0.0f;
    d.polarity = 0;
    CHECK_I(yau_edge_init(&e, &d, 48000), 0);
    CHECK_I(yau_edge_feed(&e, g_sig, 48000, 1, 0, at, 8), 1);
}

/* The voice key on the ysp_rt clock, end to end: bursts at known true
 * times go through the scripted input (reports, drift, jitter), a reader
 * runs the voice key, and each onset's time from yau_capture_time_at() is
 * the true burst start plus the detector's own delay. */
static float sc_speech(scap* s, int64_t w, int c) { (void)s; (void)c; return speech(w); }
static void test_vkey_clock(void) {
    scap* s = &g_sc;
    yau_vkey_desc d;
    static yau_vkey vk;
    yau_vkey_event ev[32];
    int m = 0, j;
    int64_t n, f, worst = 0;
    const float* p;
    sc_default(s);
    s->sig = sc_speech;
    s->channels = 1;
    s->drift_ppm = 61;
    g_noise_rms = 0.001;
    g_nbursts = 4;
    for (j = 0; j < 4; j++) {
        g_bursts[j].start = 48000 * 8 + j * 48000 + j * 977;
        g_bursts[j].attack_ms = 5;
        g_bursts[j].len_ms = 250;
        g_bursts[j].a = 0.2;
    }
    CHECK(cap_open(s, NULL));
    memset(&d, 0, sizeof d);
    d.on_db = -40;
    CHECK_I(yau_vkey_init(&vk, &d, 48000), 0);
    for (j = 0; j < 1300; j++) {
        sc_block(s);
        while ((p = yau_capture_acquire(&g_cap, 0, &n, &f, NULL)) != NULL) {
            int r = yau_vkey_feed(&vk, p, n, 1, f, ev + m, 32 - m);
            if (r > 0) m += r;
            (void)yau_capture_release(&g_cap, 0, n);
        }
    }
    (void)yau_capture_update(&g_cap);
    CHECK_I(m, 8);
    for (j = 0; j < m; j += 2) {
        int64_t t = yau_capture_time_at(&g_cap, ev[j].frame);
        int64_t truth = sc_truth(s, (double)g_bursts[j / 2].start);
        /* the detector's delay, from the same samples without the clock */
        int64_t delay = (int64_t)((ev[j].frame - (double)g_bursts[j / 2].start) * sc_kd(s));
        int64_t e = t - truth - delay;
        if (e < 0) e = -e;
        if (e > worst) worst = e;
    }
    printf("voice key on the clock: 4 onsets, time error beyond the detector's delay at most %.2f us\n", (double)worst / 1e3);
    CHECK(worst < 15000);
    yau_capture_close(&g_cap);
    g_nbursts = 0;
}

/* --- the WAV writer ---------------------------------------------------------- */

static int64_t file_read(void* ctx, int64_t off, void* dst, int64_t n) {
    FILE* f = (FILE*)ctx;
    if (fseek(f, (long)off, SEEK_SET) != 0) return -1;
    return (int64_t)fread(dst, 1, (size_t)n, f);
}

/* Written and read back by the header's own parser, to the bit: float,
 * 16 and 24 bits, 1, 2 and 4 channels; rounding and clipping of the
 * integer forms; RF64's header past 4 GB; a capture saved with a gap. */
static void test_wavw(void) {
    static const uint16_t forms[3] = { YAU_WAV_F32, YAU_WAV_S16, YAU_WAV_S24 };
    static const uint16_t chans[3] = { 1, 2, 4 };
    static yau_wavw w;
    yau_wavw_desc d;
    int fi, ci;
    char path[64];
    snprintf(path, sizeof path, "audio_test_wavw.wav");
    memset(&d, 0, sizeof d);
    CHECK(!yau_wavw_open(&w, &d));
    CHECK(strstr(yau_wavw_error(&w), "path") != NULL);
    for (fi = 0; fi < 3; fi++)
        for (ci = 0; ci < 3; ci++) {
            yau_wav_info info;
            char err[256];
            yau_wav_desc rd;
            int64_t i, nf = 1001, bad = 0;
            int C = chans[ci];
            FILE* f;
            d.path = path; d.rate = 48000; d.channels = (uint16_t)C; d.format = forms[fi];
            CHECK(yau_wavw_open(&w, &d));
            for (i = 0; i < nf * C; i++) g_sig[i] = (i % 3 == 0 ? -1 : 1) * idv(i / C, (int)(i % C));
            CHECK_I(yau_wavw_write(&w, g_sig, 400), 400);
            CHECK_I(yau_wavw_write(&w, g_sig + 400 * C, nf - 400), nf - 400);
            CHECK_I(w.frames, nf);
            yau_wavw_close(&w);
            yau_wavw_close(&w);
            memset(&rd, 0, sizeof rd);
            rd.path = path;
            CHECK_I(yau_wav_probe(&rd, &info, err, sizeof err), 0);
            CHECK_I(info.rate, 48000);
            CHECK_I(info.channels, C);
            CHECK_I(info.frames, nf);
            CHECK_I(info.format, forms[fi]);
            /* the samples, through the parser's own widening */
            f = fopen(path, "rb");
            CHECK(f != NULL);
            if (f) {
                static unsigned char raw[1001 * 4 * 4];
                static float back[1001 * 4];
                int64_t bytes = nf * C * yau__wav_bytes(forms[fi]);
                CHECK_I(file_read(f, info.data_offset, raw, bytes), bytes);
                CHECK_I(yau__widen(back, raw, nf * C, forms[fi]), -1);
                for (i = 0; i < nf * C; i++) if (back[i] != g_sig[i]) bad++;
                fclose(f);
            }
            CHECK_I(bad, 0);
        }
    /* rounding to nearest and clipping */
    d.channels = 1; d.format = YAU_WAV_S16;
    CHECK(yau_wavw_open(&w, &d));
    g_sig[0] = 1.5f; g_sig[1] = -1.0f; g_sig[2] = 0.5f / 32768.0f; g_sig[3] = 1.0f;
    CHECK_I(yau_wavw_write(&w, g_sig, 4), 4);
    CHECK_I(w.clipped, 2);
    yau_wavw_close(&w);
    {
        FILE* f = fopen(path, "rb");
        unsigned char raw[8];
        CHECK(f != NULL);
        if (f) {
            CHECK_I(file_read(f, 80, raw, 8), 8);
            CHECK_I((int16_t)yau__le16(raw), 32767);
            CHECK_I((int16_t)yau__le16(raw + 2), -32768);
            CHECK_I((int16_t)yau__le16(raw + 4), 1);
            CHECK_I((int16_t)yau__le16(raw + 6), 32767);
            fclose(f);
        }
    }
    /* past 4 GB the header is RF64 with ds64 */
    {
        unsigned char h[YAU__WAVW_HDR];
        uint64_t data = 5000000000ull;
        w.rate_ = 48000; w.ch_ = 2; w.format_ = YAU_WAV_F32;
        yau__wavw_header(&w, h, data);
        CHECK(memcmp(h, "RF64", 4) == 0 && memcmp(h + 12, "ds64", 4) == 0);
        CHECK(yau__le32(h + 4) == 0xFFFFFFFFu && yau__le32(h + 76) == 0xFFFFFFFFu);
        CHECK(yau__le64(h + 28) == data && yau__le64(h + 20) == data + YAU__WAVW_HDR - 8);
        CHECK(yau__le64(h + 36) == data / 8);
        yau__wavw_header(&w, h, 1000);
        CHECK(memcmp(h, "RIFF", 4) == 0 && memcmp(h + 12, "JUNK", 4) == 0 && yau__le32(h + 4) == 1000 + YAU__WAVW_HDR - 8);
    }
    /* a capture saved across a gap: silence for the dropped frames, so the
     * file's sample s is capture frame s */
    {
        scap* s = &g_sc;
        yau_capture_desc cd;
        yau_wav_info info;
        char err[256];
        yau_wav_desc rd;
        int64_t got, i, bad = 0;
        int k;
        sc_default(s);
        s->pos_source = YAU_POS_CALLBACK;
        memset(&cd, 0, sizeof cd);
        cd.frames = 4800;
        CHECK(cap_open(s, &cd));
        d.channels = 2; d.format = YAU_WAV_F32;
        CHECK(yau_wavw_open(&w, &d));
        for (k = 0; k < 15; k++) sc_block(s);   /* 5 blocks dropped */
        got = yau_capture_save(&g_cap, 0, &w, 0);
        CHECK_I(got, 4800);
        for (k = 0; k < 5; k++) sc_block(s);
        got = yau_capture_save(&g_cap, 0, &w, 0);
        CHECK_I(got, 2400);
        CHECK_I(w.frames, 20 * 480);
        yau_wavw_close(&w);
        yau_capture_close(&g_cap);
        memset(&rd, 0, sizeof rd);
        rd.path = path;
        CHECK_I(yau_wav_probe(&rd, &info, err, sizeof err), 0);
        CHECK_I(info.frames, 20 * 480);
        {
            FILE* f = fopen(path, "rb");
            static unsigned char raw[9600 * 8];
            static float back[9600 * 2];
            if (f) {
                CHECK_I(file_read(f, info.data_offset, raw, 9600 * 8), 9600 * 8);
                (void)yau__widen(back, raw, 9600 * 2, YAU_WAV_F32);
                for (i = 0; i < 9600; i++) {
                    float want0 = i >= 4800 && i < 7200 ? 0.0f : idv(i, 0);
                    if (back[2 * i] != want0) bad++;
                }
                fclose(f);
            }
        }
        CHECK_I(bad, 0);
    }
    remove(path);
}

/* The handle's refusals and the strict open of an input. */
static void test_capture_open(void) {
    scap* s = &g_sc;
    yau_capture_desc d;
    char line[512];
    memset(&d, 0, sizeof d);
    CHECK(!yau_capture_open(&g_cap, NULL));
    CHECK(strlen(yau_capture_error(&g_cap)) > 0);
    d.readers = 3;
    CHECK(!cap_open(&g_sc, &d));
    sc_default(s);
    d.readers = 0;
    s->rate = 44100;
    CHECK(!cap_open(s, &d));
    CHECK(strstr(yau_capture_error(&g_cap), "44100") != NULL);
    CHECK(s->closed);
    sc_default(s);
    d.format.channels = 1;
    CHECK(!cap_open(s, &d));
    d.format.channels = 0;
    d.frames = 1000;   /* under 4 periods */
    CHECK(!cap_open(s, &d));
    d.frames = 0;
    d.format.sample = YAU_S24;
    s->out = YAU_OUT_S16;
    CHECK(!cap_open(s, &d));
    d.format.sample = YAU_S16;
    CHECK(cap_open(s, &d));
    sc_run_s(s, 1.0);
    {
        int64_t wrong = 0, n;
        int64_t fr;
        const float* p = yau_capture_acquire(&g_cap, 0, &n, &fr, NULL);
        /* s16 widened exactly: idv values are multiples of 2^-13 */
        CHECK(p != NULL && p[0] == idv(0, 0));
        (void)wrong;
    }
    CHECK(yau_capture_acquire(&g_cap, 1, &d.frames, NULL, NULL) == NULL);   /* reader 1 is off */
    CHECK_I(yau_capture_release(&g_cap, 1, 0), YAU_ERR_ARG);
    CHECK(yau_capture_describe(&g_cap, line, sizeof line) > 0);
    CHECK(strstr(line, "scripted input") != NULL);
    yau_capture_close(&g_cap);
    yau_capture_close(&g_cap);
    CHECK(yau_capture_time_of(&g_cap, 0) == -1);
}

static void test_capture(void) {
    test_capture_open();
    test_capture_fit();
    test_capture_overflow();
    test_capture_loss();
    test_vkey();
    test_edge();
    test_vkey_clock();
    test_wavw();
}
