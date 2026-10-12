/* timeline_test_targets.h - TARGETS (v0.6.0), included by timeline_test.c
 * after its helpers. Written by the writer of the implementation, so a
 * misreading can be in the header, the manual and these checks alike. Two
 * things limit that: the eases and the repeat fold here are written apart
 * from the header's (pow() and floor division, not its expressions), and a
 * target tween is also compared bit for bit with ytl_tween() on a channel,
 * the path the earlier, blind tests cover.
 *
 * TL_MUT bits for this file start at bit 28 (MUT_TG_*). */

#define MUT_TG_FIRST  (1u << 28)  /* the model's first started tween drives   */
#define MUT_TG_REST   (1u << 29)  /* the model's start value is always rest  */
#define MUT_TG_ONSET  (1u << 30)  /* the model reads a STEP tween at the onset */

#define TG_CAP 64
static ytl_target_slot g_tgs[TG_CAP], g_tgs2[TG_CAP];
static ytl_tween_slot g_tws[2 * TG_CAP], g_tws2[2 * TG_CAP];

static bool tg_open(ytl_timeline* tl, ytl_event* store, int ecap, int ntg, int ntw, double lead, int second) {
    ytl_desc d;
    memset(&d, 0, sizeof d);
    if (store && ecap > 0) memset(store, 0, (size_t)ecap * sizeof *store);
    d.events = store;
    d.event_capacity = ecap;
    d.n_channels = 4;
    d.lead = lead;
    d.targets = second ? g_tgs2 : g_tgs;
    d.target_capacity = ntg;
    d.tweens = second ? g_tws2 : g_tws;
    d.tween_capacity = ntw;
    /* The pools may hold anything at open. */
    memset(d.targets, 0xA5, sizeof g_tgs);
    memset(d.tweens, 0x5A, sizeof g_tws);
    return ytl_open(tl, &d);
}

/* Frame k of a 60 Hz display whose base 1 was anchored at T0 at time 0. */
static int tg_frame(ytl_timeline* tl, int64_t k, ytl_event* fired, int cap) {
    return eval_at(tl, T0 + k * P60, P60, k, fired, cap);
}

static void open_fail_d(int line, const ytl_desc* d) {
    if (ytl_open(&g_tl, d)) fail(line, "open accepted a bad desc");
    else if (ytl_error(&g_tl)[0] == '\0') fail(line, "open failed with an empty message");
}

static ytl_anim tg_anim(void* target, float to, int64_t dur, int ease) {
    ytl_anim a;
    memset(&a, 0, sizeof a);
    a.target = target;
    a.to[0] = to;
    a.duration_ns = dur;
    a.ease = ease;
    return a;
}

/* e(u), written apart from the header: pow() and the easings.net forms. */
static double tg_shape(int ease, const ytl_curve* cv, double u) {
    switch (ease) {
    case YTL_EASE_COSINE:       return (1.0 - cos(REF_PI * u)) / 2.0;
    case YTL_EASE_QUAD_IN:      return pow(u, 2.0);
    case YTL_EASE_QUAD_OUT:     return 1.0 - pow(1.0 - u, 2.0);
    case YTL_EASE_QUAD_IN_OUT:  return u < 0.5 ? 2.0 * pow(u, 2.0) : 1.0 - pow(-2.0 * u + 2.0, 2.0) / 2.0;
    case YTL_EASE_CUBIC_IN:     return pow(u, 3.0);
    case YTL_EASE_CUBIC_OUT:    return 1.0 - pow(1.0 - u, 3.0);
    case YTL_EASE_CUBIC_IN_OUT: return u < 0.5 ? 4.0 * pow(u, 3.0) : 1.0 - pow(-2.0 * u + 2.0, 3.0) / 2.0;
    case YTL_EASE_BEZIER:       return ref_bezier(cv, u);
    default:                    return u;
    }
}

/* A model tween, resolved: from, to and the tangents hold their values. */
typedef struct tg_mtw {
    int64_t s, d;
    int cycles, ease, yoyo, stepped, kv, from_set, to_cur, immediate, resolved, id;
    ytl_curve cv;
    double from[4], to[4], tan[4];
} tg_mtw;

/* The value of a tween at t: the repeat folded by floor division, the
 * yoyo's return leg, the hold after the last cycle. */
static double tg_mval(const tg_mtw* w, int c, int64_t t) {
    int64_t L = w->d * (w->yoyo ? 2 : 1), ph;
    int multi = w->cycles > 1 || w->cycles == YTL_FOREVER;
    double v0, v1, u;
    if (t < w->s) return w->from[c];
    ph = t - w->s;
    if (multi) {
        int64_t k = ph / L;
        if (w->cycles > 0 && k >= w->cycles) return w->yoyo ? w->from[c] : w->to[c];
        ph -= k * L;
    } else if (ph >= L) {
        return w->yoyo ? w->from[c] : w->to[c];
    }
    if (w->yoyo && ph >= w->d && w->ease == YTL_EASE_STEP) {
        /* STEP keeps its square wave: `to` until the end of the return leg */
        v0 = w->to[c];
        v1 = w->from[c];
        ph -= w->d;
    } else {
        /* v0.7.0: the return leg is the forward leg at the mirrored time */
        if (w->yoyo && ph > w->d) ph = 2 * w->d - ph;
        v0 = w->from[c];
        v1 = w->to[c];
    }
    u = (double)ph / (double)w->d;
    if (w->kv) return ref_hermite(v0, w->tan[c], v1, u);
    if (w->ease == YTL_EASE_STEP) return v0;
    if (w->ease == YTL_EASE_LOG) return v0 * pow(v1 / v0, u);
    return v0 + (v1 - v0) * tg_shape(w->ease, &w->cv, u);
}

/* A model target: TARGETS read literally. tw[] in add order; the list
 * order is (start, id). */
typedef struct tg_mtg {
    void* ptr;
    int n, type, base, rest_set, ntw, written;
    double rest[4];
    double pre[4];     /* the field before the frame's evaluate wrote it */
    tg_mtw tw[48];
} tg_mtg;

static void tg_mrest(tg_mtg* g) {
    int c;
    if (g->rest_set) return;
    for (c = 0; c < g->n; c++) g->rest[c] = g->pre[c];
    g->rest_set = 1;
}

/* Indices of g's tweens in (start, id) order. */
static void tg_morder(const tg_mtg* g, int* ord) {
    int i, j;
    for (i = 0; i < g->ntw; i++) ord[i] = i;
    for (i = 1; i < g->ntw; i++) {
        for (j = i; j > 0; j--) {
            const tg_mtw* a = &g->tw[ord[j - 1]];
            const tg_mtw* b = &g->tw[ord[j]];
            if (a->s < b->s || (a->s == b->s && a->id < b->id)) break;
            { int x = ord[j]; ord[j] = ord[j - 1]; ord[j - 1] = x; }
        }
    }
}

static void tg_mresolve(tg_mtg* g, const int* ord, int i) {
    tg_mtw* w = &g->tw[ord[i]];
    double bv[4], b1[4];
    int c;
    if (w->resolved) return;
    for (c = 0; c < g->n; c++) {
        if (i > 0 && !(g_mut & MUT_TG_REST)) {
            bv[c] = tg_mval(&g->tw[ord[i - 1]], c, w->s);
            b1[c] = tg_mval(&g->tw[ord[i - 1]], c, w->s - 1000);
        } else {
            tg_mrest(g);
            bv[c] = b1[c] = g->rest[c];
        }
        if (!w->from_set) w->from[c] = (double)(float)bv[c];
        if (w->to_cur) w->to[c] = (double)(float)bv[c];
        if (w->kv) w->tan[c] = (double)(float)((bv[c] - b1[c]) / 1000.0 * (double)w->d);
    }
    w->resolved = 1;
}

/* The values the model writes at a frame whose base times are now and hi;
 * 0 when it writes nothing. */
static int tg_meval(tg_mtg* g, int64_t now, int64_t hi, double* v) {
    int ord[48], i, e = -1, c;
    if (g->ntw == 0) return 0;
    tg_morder(g, ord);
    for (i = 0; i < g->ntw; i++) {
        const tg_mtw* w = &g->tw[ord[i]];
        int stepped = w->stepped && !(g_mut & MUT_TG_ONSET);
        if (w->s <= (stepped ? hi : now)) {
            e = i;
            if (g_mut & MUT_TG_FIRST) break;
        }
    }
    for (i = 0; i <= e; i++) tg_mresolve(g, ord, i);
    if (e >= 0) {
        const tg_mtw* w = &g->tw[ord[e]];
        int stepped = w->stepped && !(g_mut & MUT_TG_ONSET);
        for (c = 0; c < g->n; c++) v[c] = tg_mval(w, c, stepped ? hi : now);
    } else if (g->tw[ord[0]].immediate) {
        tg_mresolve(g, ord, 0);
        for (c = 0; c < g->n; c++) v[c] = g->tw[ord[0]].from[c];
    } else if (g->written) {
        for (c = 0; c < g->n; c++) v[c] = g->rest[c];
    } else {
        return 0;
    }
    tg_mrest(g);
    g->written = 1;
    return 1;
}

static tg_mtw tg_mfrom_anim(const ytl_anim* a, int64_t s, int id) {
    tg_mtw w;
    int c;
    memset(&w, 0, sizeof w);
    w.s = s;
    w.d = a->duration_ns;
    w.cycles = a->cycles;
    w.ease = a->ease;
    w.yoyo = a->yoyo;
    w.stepped = a->ease == YTL_EASE_STEP;
    w.kv = a->keep_velocity;
    w.from_set = a->from_set;
    w.to_cur = a->to_current;
    w.immediate = a->immediate;
    w.cv = a->curve;
    w.id = id;
    for (c = 0; c < 4; c++) {
        w.from[c] = a->from[c];
        w.to[c] = a->to[c];
    }
    return w;
}

/* -------------------------------------------------------------- the calls */

static void test_tg_basic(void) {
    static float x, y, xs[3];
    ytl_seq q;
    int id, k;
    double want;

    /* A to(): the field is the caller's before the start, then the tween's
     * from its value there (the rest value). */
    CHECK(tg_open(&g_tl, g_store, 64, 8, 16, 0.0, 0));
    x = 2.0f;
    q = ytl_seq_on(&g_tl, 1);
    ytl_at(&q, YRT_MS(100));
    id = ytl_tween_to(&q, &x, 10.0f, YRT_MS(200), YTL_EASE_LINEAR);
    CHECK_I(id, 0);
    CHECK_I(q.err, 0);
    CHECK_I(q.t, YRT_MS(100));
    CHECK_I(q.prev_start, YRT_MS(100));
    CHECK_I(q.prev_end, YRT_MS(300));
    CHECK_I(ytl_tween_count(&g_tl, &k), 1);
    CHECK_I(k, 1);
    CHECK_I(ytl_anchor(&g_tl, 1, T0, 0), 0);
    for (k = 0; k < 30; k++) {
        int64_t bt = k * P60;
        CHECK(tg_frame(&g_tl, k, NULL, 0) >= 0);
        if (bt < YRT_MS(100)) {
            /* not written: the caller's write stays */
            CHECK(x == 2.0f || x == 3.0f);
            x = 3.0f;
            continue;
        }
        want = bt >= YRT_MS(300) ? 10.0 : 3.0 + 7.0 * (double)(bt - YRT_MS(100)) / (double)YRT_MS(200);
        CHECK_F(x, want, 1e-5);
        /* From the first write on, the timeline owns the field. */
        x = -99.0f;
    }
    CHECK(x == -99.0f);
    CHECK_I(tg_frame(&g_tl, 30, NULL, 0), 0);
    CHECK(x == 10.0f);
    /* A kill hands the field back: it keeps its value, no write follows. */
    CHECK_I(ytl_kill(&g_tl, &x), 1);
    CHECK_I(ytl_tween_count(&g_tl, &k), 0);
    CHECK_I(k, 0);
    x = 5.0f;
    CHECK_I(tg_frame(&g_tl, 31, NULL, 0), 0);
    CHECK(x == 5.0f);
    CHECK_I(ytl_kill(&g_tl, &x), 0);
    CHECK_I(ytl_kill_tween(&g_tl, id), YTL_ERR_NOT_FOUND);
    CHECK_I(ytl_kill(&g_tl, NULL), YTL_ERR_ARG);

    /* from(): `from` before the start (immediate), the value at the start
     * as `to`. fromTo(): both. set(): the value from the start. */
    CHECK(tg_open(&g_tl, g_store, 64, 8, 16, 0.0, 0));
    x = 4.0f;
    y = 1.0f;
    q = ytl_seq_on(&g_tl, 1);
    ytl_at(&q, YRT_MS(200));
    CHECK(ytl_tween_from(&q, &x, 0.0f, YRT_MS(100), 0) >= 0);
    CHECK(ytl_tween_from_to(&q, &y, -1.0f, -3.0f, YRT_MS(100), 0) >= 0);
    ytl_wait(&q, YRT_MS(300));
    CHECK(ytl_tween_set(&q, &y, 9.0f) >= 0);
    CHECK_I(q.prev_start, YRT_MS(500));
    CHECK_I(q.prev_end, YRT_MS(500));
    CHECK_I(ytl_anchor(&g_tl, 1, T0, 0), 0);
    CHECK_I(tg_frame(&g_tl, 0, NULL, 0), 0);
    CHECK(x == 0.0f);
    CHECK(y == -1.0f);
    for (k = 1; k < 40; k++) {
        int64_t bt = k * P60;
        double u = (double)(bt - YRT_MS(200)) / (double)YRT_MS(100);
        tg_frame(&g_tl, k, NULL, 0);
        if (bt < YRT_MS(200)) {
            CHECK(x == 0.0f);
            CHECK(y == -1.0f);
        } else if (bt < YRT_MS(300)) {
            CHECK_F(x, 4.0 * u, 1e-5);
            CHECK_F(y, -1.0 - 2.0 * u, 1e-5);
        } else {
            CHECK(x == 4.0f);
            /* The set lands like an event at 500 ms: the frame nearest. */
            CHECK(y == (bt + P60 / 2 >= YRT_MS(500) ? 9.0f : -3.0f));
        }
    }

    /* Several components, and int32_t rounded to nearest. */
    {
        static float rgb[3];
        static int32_t iv[2];
        ytl_anim a;
        CHECK(tg_open(&g_tl, g_store, 64, 8, 16, 0.0, 0));
        rgb[0] = 0.0f; rgb[1] = 1.0f; rgb[2] = 0.5f;
        iv[0] = 0; iv[1] = 10;
        q = ytl_seq_on(&g_tl, 1);
        a = tg_anim(rgb, 1.0f, YRT_MS(100), YTL_EASE_LINEAR);
        a.n = 3;
        a.to[1] = 0.0f;
        a.to[2] = 0.5f;
        CHECK(ytl_animate(&q, &a) >= 0);
        a = tg_anim(iv, 3.0f, YRT_MS(100), YTL_EASE_LINEAR);
        a.type = YTL_I32;
        a.n = 2;
        a.to[1] = -10.0f;
        CHECK(ytl_animate(&q, &a) >= 0);
        CHECK_I(ytl_anchor(&g_tl, 1, T0, 0), 0);
        for (k = 0; k < 8; k++) {
            double u = (double)(k * P60) / (double)YRT_MS(100);
            if (u > 1.0) u = 1.0;
            tg_frame(&g_tl, k, NULL, 0);
            CHECK_F(rgb[0], u, 1e-6);
            CHECK_F(rgb[1], 1.0 - u, 1e-6);
            CHECK(rgb[2] == 0.5f);
            CHECK_I(iv[0], (int32_t)floor(3.0 * u + 0.5));
            CHECK_I(iv[1], (int32_t)floor(10.0 - 20.0 * u + 0.5));
        }
    }

    /* Stagger over an array: field i starts i x stagger later; the ids
     * follow in order; "<" and ">" span the whole call. */
    {
        ytl_anim a;
        CHECK(tg_open(&g_tl, g_store, 64, 8, 16, 0.0, 0));
        xs[0] = xs[1] = xs[2] = 0.0f;
        q = ytl_seq_on(&g_tl, 1);
        a = tg_anim(xs, 1.0f, YRT_MS(100), YTL_EASE_LINEAR);
        a.delay_ns = YRT_MS(50);
        a.stagger_ns = YRT_MS(30);
        CHECK_I(ytl_animate_n(&q, &a, sizeof xs[0], 3), 0);
        CHECK_I(q.prev_start, YRT_MS(50));
        CHECK_I(q.prev_end, YRT_MS(50) + 2 * YRT_MS(30) + YRT_MS(100));
        CHECK_I(ytl_tween_count(&g_tl, &k), 3);
        CHECK_I(k, 3);
        CHECK_I(ytl_anchor(&g_tl, 1, T0, 0), 0);
        for (k = 0; k < 20; k++) {
            int i;
            tg_frame(&g_tl, k, NULL, 0);
            for (i = 0; i < 3; i++) {
                int64_t s = YRT_MS(50) + i * YRT_MS(30), bt = k * P60;
                double u = bt < s ? 0.0 : (double)(bt - s) / (double)YRT_MS(100);
                CHECK_F(xs[i], u > 1.0 ? 1.0 : u, 1e-6);
            }
        }
        /* A negative stagger starts the last field first. */
        CHECK(tg_open(&g_tl, g_store, 64, 8, 16, 0.0, 0));
        q = ytl_seq_on(&g_tl, 1);
        ytl_at(&q, YRT_MS(100));
        a.delay_ns = 0;
        a.stagger_ns = -YRT_MS(40);
        CHECK_I(ytl_animate_n(&q, &a, sizeof xs[0], 3), 0);
        CHECK_I(q.prev_start, YRT_MS(20));
        CHECK_I(q.prev_end, YRT_MS(200));
    }
    ytl_close(&g_tl);
}

/* Every ease against tg_shape() at 60 Hz onsets; keys take the new eases
 * too, against the same reference. */
static void test_tg_eases(void) {
    static float x;
    int ease;
    for (ease = 0; ease <= YTL_EASE_CUBIC_IN_OUT; ease++) {
        ytl_seq q;
        ytl_anim a;
        ytl_key kk[2];
        tg_mtw w;
        int k;
        CHECK(tg_open(&g_tl, g_store, 64, 8, 16, 0.0, 0));
        x = 0.5f;
        q = ytl_seq_on(&g_tl, 1);
        ytl_at(&q, YRT_MS(10));
        a = tg_anim(&x, 4.0f, YRT_MS(500), ease);
        a.curve = mkcurve(0.42f, 0.0f, 0.58f, 1.0f);
        a.from_set = true;   /* LOG needs it; the rest value is 0.5 too */
        a.from[0] = 0.5f;
        CHECK(ytl_animate(&q, &a) >= 0);
        kk[0] = mkkey(YRT_MS(10), 0.5f, ease);
        kk[1] = mkkey(YRT_MS(510), 4.0f, YTL_EASE_LINEAR);
        if (ease != YTL_EASE_BEZIER) CHECK_I(ytl_set_keys(&g_tl, 1, 1, kk, 2), 0);
        w = tg_mfrom_anim(&a, YRT_MS(10), 0);
        w.from[0] = 0.5;
        CHECK_I(ytl_anchor(&g_tl, 1, T0, 0), 0);
        for (k = 1; k < 34; k++) {
            int64_t bt = k * P60, rd = bt + (ease == YTL_EASE_STEP ? P60 / 2 : 0);
            tg_frame(&g_tl, k, NULL, 0);
            CHECK_F(x, tg_mval(&w, 0, rd), 2e-6);
            if (ease != YTL_EASE_BEZIER) CHECK_F(ytl_value(&g_tl, 1), tg_mval(&w, 0, rd), 2e-6);
        }
    }
    ytl_close(&g_tl);
}

/* A random valid desc. */
static ytl_tween_desc tg_rnd_desc(void) {
    ytl_tween_desc d;
    memset(&d, 0, sizeof d);
    d.to = (float)rnd_range(-100, 100) / 8.0f;
    d.duration_ns = rnd_range(1, 400) * MS_NS / 2;
    d.ease = rnd_int(YTL_EASE_CUBIC_IN_OUT + 1);
    if (d.ease == YTL_EASE_BEZIER)
        d.curve = mkcurve((float)rnd_int(101) / 100.0f, (float)rnd_range(-50, 150) / 100.0f,
                          (float)rnd_int(101) / 100.0f, (float)rnd_range(-50, 150) / 100.0f);
    if (d.ease == YTL_EASE_LOG || rnd_int(4) == 0) {
        d.from_set = true;
        d.from = (float)rnd_range(1, 100) / 8.0f;
        if (d.ease == YTL_EASE_LOG) d.to = (float)rnd_range(1, 100) / 8.0f;
    }
    switch (rnd_int(6)) {
    case 0: d.cycles = 2 + rnd_int(3); break;
    case 1: d.cycles = YTL_FOREVER; break;
    default: d.cycles = rnd_int(2); break;
    }
    d.yoyo = rnd_int(3) == 0;
    if (rnd_int(4) == 0 && !d.from_set && !d.yoyo && d.cycles < 2 && d.cycles != YTL_FOREVER) {
        d.ease = YTL_EASE_LINEAR;
        d.keep_velocity = true;
    }
    return d;
}

static ytl_anim tg_from_desc(void* field, const ytl_tween_desc* d) {
    ytl_anim a = tg_anim(field, d->to, d->duration_ns, d->ease);
    a.from[0] = d->from;
    a.from_set = d->from_set;
    a.curve = d->curve;
    a.cycles = d->cycles;
    a.yoyo = d->yoyo;
    a.keep_velocity = d->keep_velocity;
    return a;
}

/* "The same arithmetic as ytl_tween()": a target tween and a channel tween
 * with the same desc, the second taking over from the first, give the same
 * bits on every frame. */
static void test_tg_vs_tween(void) {
    static float x;
    int it, k, diff = 0, frames = 0;
    g_rng = 4242;
    for (it = 0; it < 400; it++) {
        ytl_tween_desc d1 = tg_rnd_desc(), d2 = tg_rnd_desc();
        int64_t s1 = rnd_range(0, 200) * MS_NS, s2 = s1 + rnd_range(0, 400) * MS_NS;
        int64_t kpost = (s1 + P60) / P60 + 1 + rnd_int(3);
        float init = (float)rnd_range(-20, 20) / 4.0f;
        ytl_seq q;
        ytl_anim a;
        CHECK(open_h(&g_tl, g_store, 8, 1, &init, 0.5));
        CHECK(tg_open(&g_tl2, g_store2, 8, 2, 4, 0.5, 1));
        x = init;
        d1.start_ns = s1;
        d1.start_set = true;
        CHECK_I(ytl_tween(&g_tl, 0, 1, &d1), 0);
        q = ytl_seq_on(&g_tl2, 1);
        ytl_at(&q, s1);
        a = tg_from_desc(&x, &d1);
        CHECK(ytl_animate(&q, &a) >= 0);
        ytl_anchor(&g_tl, 1, T0, 0);
        ytl_anchor(&g_tl2, 1, T0, 0);
        for (k = 0; k < 70; k++) {
            float vc;
            if (k == kpost) {
                ytl_tween_desc d = d2;
                d.start_ns = s2;
                d.start_set = true;
                CHECK_I(ytl_tween(&g_tl, 0, 1, &d), 0);
                ytl_at(&q, s2);
                a = tg_from_desc(&x, &d2);
                CHECK(ytl_animate(&q, &a) >= 0);
            }
            tg_frame(&g_tl, k, NULL, 0);
            tg_frame(&g_tl2, k, NULL, 0);
            vc = ytl_value(&g_tl, 0);
            frames++;
            /* Before the first start the channel holds its initial value and
             * the field is not written; both are init. */
            if (memcmp(&vc, &x, sizeof x) != 0) {
                if (diff++ < 5)
                    fprintf(stderr, "  vs tween: it %d frame %d: channel %.9g target %.9g\n", it, k, vc, x);
            }
        }
    }
    CHECK_I(diff, 0);
    CHECK(frames == 400 * 70);
}

/* The overwrite rules. */
static void test_tg_overwrite(void) {
    static float x, y;
    static int32_t ix;
    ytl_seq q;
    ytl_anim a;
    int ida, idb, k, n;

    /* The latest start drives the field; the earlier tween stays and drives
     * it again after a rewind. B starts from A's value at its start. */
    CHECK(tg_open(&g_tl, g_store, 64, 8, 16, 0.0, 0));
    x = 0.0f;
    q = ytl_seq_on(&g_tl, 1);
    ida = ytl_tween_to(&q, &x, 100.0f, YRT_MS(1000), 0);
    ytl_at(&q, YRT_MS(500));
    idb = ytl_tween_to(&q, &x, 0.0f, YRT_MS(250), 0);
    CHECK(ida == 0 && idb == 1);
    ytl_anchor(&g_tl, 1, T0, 0);
    for (k = 0; k < 60; k++) {
        int64_t bt = k * P60;
        double want = bt < YRT_MS(500) ? 100.0 * (double)bt / 1e9
                      : bt >= YRT_MS(750) ? 0.0 : 50.0 * (1.0 - (double)(bt - YRT_MS(500)) / (double)YRT_MS(250));
        tg_frame(&g_tl, k, NULL, 0);
        CHECK_F(x, want, 1e-4);
    }
    CHECK_I(ytl_anchor(&g_tl, 1, T0 + 60 * P60, YRT_MS(250)), 0);
    tg_frame(&g_tl, 60, NULL, 0);
    CHECK_F(x, 25.0, 1e-4);
    /* Equal starts: the later call wins. */
    ytl_at(&q, YRT_MS(500));
    CHECK_I(ytl_tween_to(&q, &x, 7.0f, 0, 0), 2);
    tg_frame(&g_tl, 61, NULL, 0);
    tg_frame(&g_tl, 76, NULL, 0);
    CHECK(x == 7.0f);
    CHECK_I(ytl_tween_count(&g_tl, NULL), 3);
    /* kill_tween of the winner: B drives again, from its kept start value. */
    CHECK_I(ytl_kill_tween(&g_tl, 2), 0);
    tg_frame(&g_tl, 77, NULL, 0);
    CHECK_F(x, 50.0 * (1.0 - (double)(YRT_MS(250) + 17 * P60 - YRT_MS(500)) / (double)YRT_MS(250)), 1e-4);

    /* Another base is YTL_ERR_BOUND, a different shape YTL_ERR_ARG;
     * OVERWRITE_ALL kills the field's tweens at the call, rebinds it, and
     * the new tween starts from the field's value then. */
    {
        ytl_seq q2 = ytl_seq_on(&g_tl, 2);
        CHECK_I(ytl_tween_to(&q2, &x, 1.0f, YRT_MS(10), 0), YTL_ERR_BOUND);
        CHECK_I(q2.err, YTL_ERR_BOUND);
        q2 = ytl_seq_on(&g_tl, 1);
        a = tg_anim(&x, 1.0f, YRT_MS(10), 0);
        a.n = 2;
        CHECK_I(ytl_animate(&q2, &a), YTL_ERR_ARG);
        q2 = ytl_seq_on(&g_tl, 1);
        a = tg_anim(&x, 1.0f, YRT_MS(10), 0);
        a.type = YTL_I32;
        CHECK_I(ytl_animate(&q2, &a), YTL_ERR_ARG);
        CHECK_I(ytl_anchor(&g_tl, 2, T0 + 77 * P60, 0), 0);
        q2 = ytl_seq_now(&g_tl, 2);
        y = x;
        a = tg_anim(&x, -1.0f, YRT_MS(100), 0);
        a.overwrite = YTL_OVERWRITE_ALL;
        CHECK(ytl_animate(&q2, &a) >= 0);
        CHECK_I(ytl_tween_count(&g_tl, &n), 1);
        CHECK_I(n, 1);
        tg_frame(&g_tl, 78, NULL, 0);
        CHECK_F(x, y + (-1.0 - y) * (double)P60 / (double)YRT_MS(100), 1e-4);
    }

    /* ytl_clear() of the base frees its targets; the field keeps its value. */
    y = x;
    CHECK(ytl_clear(&g_tl, 2) >= 0);
    CHECK_I(ytl_tween_count(&g_tl, &n), 0);
    CHECK_I(n, 0);
    tg_frame(&g_tl, 79, NULL, 0);
    CHECK(x == y);

    /* A sequence's cancel removes its tweens and its callback events. */
    CHECK(tg_open(&g_tl, g_store, 64, 8, 16, 0.0, 0));
    ix = 3;
    q = ytl_seq_on(&g_tl, 1);
    a = tg_anim(&ix, 8.0f, YRT_MS(100), 0);
    a.type = YTL_I32;
    a.on_complete = NULL;
    CHECK(ytl_animate(&q, &a) >= 0);
    CHECK(ytl_tween_to(&q, &x, 1.0f, YRT_MS(100), 0) >= 0);
    {
        ytl_seq q2 = ytl_seq_on(&g_tl, 1);
        CHECK(ytl_tween_to(&q2, &y, 1.0f, YRT_MS(100), 0) >= 0);
        CHECK_I(ytl_seq_cancel(&q), 0);
        CHECK_I(ytl_tween_count(&g_tl, &n), 1);
        CHECK_I(n, 1);
    }
    ytl_close(&g_tl);
}

/* Callbacks: events of kinds YTL_CALL_*, landing as events do, called after
 * every field is written, in firing order. */
typedef struct tg_log {
    int n;
    int id[16], kind[16];
    int64_t frame[16];
    float seen[16];
    int post, kill, nested, prune;
} tg_log;

static tg_log g_log;
static float g_cbx, g_cby;
static int64_t g_cb_frame;

static void tg_cb(ytl_timeline* tl, int id, void* user, int kind) {
    tg_log* lg = (tg_log*)user;
    if (lg->n < 16) {
        lg->id[lg->n] = id;
        lg->kind[lg->n] = kind;
        lg->frame[lg->n] = g_cb_frame;
        lg->seen[lg->n] = g_cbx;
        lg->n++;
    }
    if (lg->nested) {
        ytl_frame f;
        f.onset = T0 + g_cb_frame * P60;
        f.period = P60;
        f.index = g_cb_frame;
        lg->nested = ytl_evaluate(tl, &f, NULL, 0);
    }
    if (lg->kill) ytl_kill_tween(tl, id);
    if (lg->prune) ytl_prune(tl, 1);
    if (lg->post && kind == 1) {
        ytl_seq q = ytl_seq_now(tl, 1);
        ytl_tween_to(&q, &g_cby, 5.0f, 0, 0);
        lg->post = 0;
    }
}
static void tg_on_start(ytl_timeline* tl, int id, void* user) { tg_cb(tl, id, user, 0); }
static void tg_on_complete(ytl_timeline* tl, int id, void* user) { tg_cb(tl, id, user, 1); }

static void test_tg_callbacks(void) {
    ytl_seq q;
    ytl_anim a;
    ytl_event fired[8];
    int k, n, id, n_calls = 0;
    int64_t ks = -1, kc = -1;

    CHECK(tg_open(&g_tl, g_store, 64, 8, 16, 0.0, 0));
    memset(&g_log, 0, sizeof g_log);
    g_cbx = 0.0f;
    g_cby = 0.0f;
    q = ytl_seq_on(&g_tl, 1);
    ytl_at(&q, YRT_MS(100));
    a = tg_anim(&g_cbx, 1.0f, YRT_MS(200), 0);
    a.on_start = tg_on_start;
    a.on_complete = tg_on_complete;
    a.user = &g_log;
    id = ytl_animate(&q, &a);
    CHECK_I(id, 0);
    /* Events that mark where the callbacks land. */
    {
        ytl_event e = ev(1, YRT_MS(100), YTL_MARK, 0, 0.0f);
        CHECK(ytl_add(&g_tl, &e) >= 0);
        e.time_ns = YRT_MS(300);
        e.code = 1;
        CHECK(ytl_add(&g_tl, &e) >= 0);
    }
    g_log.post = 1;
    ytl_anchor(&g_tl, 1, T0, 0);
    for (k = 0; k < 30; k++) {
        int i;
        g_cb_frame = k;
        n = tg_frame(&g_tl, k, fired, 8);
        for (i = 0; i < n && i < 8; i++) {
            if (fired[i].kind == YTL_MARK) {
                if (fired[i].code == 0) ks = k;
                else kc = k;
            }
            if (fired[i].kind == YTL_CALL_START || fired[i].kind == YTL_CALL_COMPLETE) {
                CHECK_I(fired[i].code, id);
                CHECK(fired[i].user == (uint64_t)(uintptr_t)&g_log);
                n_calls++;
            }
        }
        /* The tween posted by on_complete runs from the next frame. */
        if (kc >= 0 && k == kc) CHECK(g_cby == 0.0f);
        if (kc >= 0 && k == kc + 1) CHECK(g_cby == 5.0f);
    }
    CHECK_I(n_calls, 2);
    CHECK_I(g_log.n, 2);
    CHECK_I(g_log.kind[0], 0);
    CHECK_I(g_log.kind[1], 1);
    CHECK_I(g_log.frame[0], ks);
    CHECK_I(g_log.frame[1], kc);
    /* The complete callback saw the value of its frame, already written. */
    CHECK_F(g_log.seen[1], (double)(kc * P60 - YRT_MS(100)) / (double)YRT_MS(200) > 1.0 ? 1.0
            : (double)(kc * P60 - YRT_MS(100)) / (double)YRT_MS(200), 1e-6);

    /* A rewind calls them again; a skip over them calls neither. */
    ytl_anchor(&g_tl, 1, T0 + 30 * P60, 0);
    for (k = 30; k < 60; k++) {
        g_cb_frame = k;
        tg_frame(&g_tl, k, NULL, 0);
    }
    CHECK_I(g_log.n, 4);
    CHECK(ytl_skip(&g_tl, 1, T0 + 60 * P60, 0) >= 0);
    CHECK(ytl_skip(&g_tl, 1, T0 + 60 * P60, YRT_MS(400)) >= 0);
    for (k = 60; k < 70; k++) {
        g_cb_frame = k;
        tg_frame(&g_tl, k, NULL, 0);
    }
    CHECK_I(g_log.n, 4);
    CHECK(g_cbx == 1.0f);

    /* ytl_evaluate() from a callback is YTL_ERR_ORDER; a kill in on_start
     * stops on_complete of the same frame. */
    CHECK(tg_open(&g_tl, g_store, 64, 8, 16, 0.0, 0));
    memset(&g_log, 0, sizeof g_log);
    g_log.nested = 1;
    g_log.kill = 1;
    q = ytl_seq_on(&g_tl, 1);
    a = tg_anim(&g_cbx, 1.0f, 0, 0);
    a.on_start = tg_on_start;
    a.on_complete = tg_on_complete;
    a.user = &g_log;
    CHECK(ytl_animate(&q, &a) >= 0);
    ytl_anchor(&g_tl, 1, T0, 0);
    g_cb_frame = 0;
    n = tg_frame(&g_tl, 0, fired, 8);
    CHECK_I(n, 2);
    CHECK_I(g_log.n, 1);
    CHECK_I(g_log.nested, YTL_ERR_ORDER);
    CHECK_I(ytl_tween_count(&g_tl, NULL), 0);
    /* A kill deletes the tween's callback events, as a cancel deletes a
     * sequence's, fired or not. */
    CHECK_I(n_events(&g_tl, 1), 0);
    q = ytl_seq_on(&g_tl, 1);
    ytl_at(&q, YRT_S(5));
    id = ytl_animate(&q, &a);
    CHECK_I(n_events(&g_tl, 1), 2);
    CHECK_I(ytl_kill_tween(&g_tl, id), 0);
    CHECK_I(n_events(&g_tl, 1), 0);
    /* YTL_FOREVER has no on_complete event. */
    a.duration_ns = YRT_MS(10);
    a.cycles = YTL_FOREVER;
    CHECK(ytl_animate(&q, &a) >= 0);
    CHECK_I(n_events(&g_tl, 1), 1);
    ytl_close(&g_tl);
}

/* A full pool refuses the call with a message that names it, and adds
 * nothing. An overwrite counts what its kill frees. */
static void test_tg_pools(void) {
    static float f[4];
    ytl_seq q;
    ytl_anim a;
    int n;
    CHECK(tg_open(&g_tl, g_store, 2, 2, 3, 0.0, 0));
    q = ytl_seq_on(&g_tl, 1);
    CHECK(ytl_tween_to(&q, &f[0], 1.0f, YRT_MS(10), 0) >= 0);
    a = tg_anim(&f[1], 1.0f, YRT_MS(10), 0);
    CHECK_I(ytl_animate_n(&q, &a, sizeof f[0], 2), YTL_ERR_FULL);
    CHECK(strstr(ytl_error(&g_tl), "target pool is full") != NULL);
    CHECK_I(q.err_call, 1);
    CHECK_I(ytl_tween_count(&g_tl, &n), 1);
    CHECK_I(n, 1);
    q = ytl_seq_on(&g_tl, 1);
    CHECK(ytl_tween_to(&q, &f[1], 1.0f, YRT_MS(10), 0) >= 0);
    CHECK(ytl_tween_to(&q, &f[1], 2.0f, YRT_MS(10), 0) >= 0);
    CHECK_I(ytl_tween_to(&q, &f[0], 2.0f, YRT_MS(10), 0), YTL_ERR_FULL);
    CHECK(strstr(ytl_error(&g_tl), "tween pool is full") != NULL);
    CHECK_I(ytl_tween_count(&g_tl, NULL), 3);
    /* An overwrite of f[1] frees two tweens, so it fits. */
    q = ytl_seq_on(&g_tl, 1);
    a = tg_anim(&f[1], 3.0f, YRT_MS(10), 0);
    a.overwrite = YTL_OVERWRITE_ALL;
    CHECK(ytl_animate(&q, &a) >= 0);
    CHECK_I(ytl_tween_count(&g_tl, &n), 2);
    CHECK_I(n, 2);
    /* Callbacks need events: 2 in a storage of 2, then none left, also for
     * an overwrite that frees a tween but no event... */
    a = tg_anim(&f[0], 1.0f, YRT_MS(10), 0);
    a.on_start = tg_on_start;
    a.on_complete = tg_on_complete;
    a.user = &g_log;
    CHECK(ytl_animate(&q, &a) >= 0);
    CHECK_I(n_events(&g_tl, 1), 2);
    CHECK_I(ytl_tween_count(&g_tl, NULL), 3);
    a.target = &f[1];
    a.overwrite = YTL_OVERWRITE_ALL;
    CHECK_I(ytl_animate(&q, &a), YTL_ERR_FULL);
    CHECK(strstr(ytl_error(&g_tl), "event storage is full") != NULL);
    CHECK_I(ytl_tween_count(&g_tl, NULL), 3);
    /* ...but an overwrite that deletes the events it needs fits. */
    q = ytl_seq_on(&g_tl, 1);
    a.target = &f[0];
    CHECK(ytl_animate(&q, &a) >= 0);
    CHECK_I(n_events(&g_tl, 1), 2);
    CHECK_I(ytl_tween_count(&g_tl, NULL), 2);
    ytl_close(&g_tl);

    /* No pools: every target call is YTL_ERR_FULL. Bad descs. */
    CHECK(open_h(&g_tl, g_store, 8, 1, NULL, 0.5));
    q = ytl_seq_on(&g_tl, 1);
    CHECK_I(ytl_tween_to(&q, &f[0], 1.0f, YRT_MS(10), 0), YTL_ERR_FULL);
    {
        ytl_desc d;
        memset(&d, 0, sizeof d);
        d.target_capacity = 1;
        open_fail_d(__LINE__, &d);
        memset(&d, 0, sizeof d);
        d.tween_capacity = -1;
        open_fail_d(__LINE__, &d);
    }
    CHECK(tg_open(&g_tl, g_store, 8, 4, 8, 0.0, 0));
#define TG_REJ(field, val) do { q = ytl_seq_on(&g_tl, 1); a = tg_anim(&f[0], 1.0f, YRT_MS(10), 0); \
        a.field = val; CHECK_I(ytl_animate(&q, &a), YTL_ERR_ARG); } while (0)
    TG_REJ(target, NULL);
    TG_REJ(n, 5);
    TG_REJ(n, -1);
    TG_REJ(type, 2);
    TG_REJ(overwrite, 2);
    TG_REJ(ease, NOT_EASE);
    TG_REJ(duration_ns, -1);
    TG_REJ(to[0], (float)make_inf());
    TG_REJ(delay_ns, LIM62);
    TG_REJ(stagger_ns, LIM62);
    q = ytl_seq_on(&g_tl, 1);
    a = tg_anim(&f[0], 1.0f, YRT_MS(10), YTL_EASE_LOG);
    a.from_set = true;
    a.from[0] = 1.0f;
    a.to_current = true;
    CHECK_I(ytl_animate(&q, &a), YTL_ERR_ARG);
    q = ytl_seq_on(&g_tl, 1);
    a = tg_anim(&f[0], 1.0f, YRT_MS(10), 0);
    a.n = 2;
    a.to[1] = (float)make_nan();
    CHECK_I(ytl_animate(&q, &a), YTL_ERR_ARG);
    CHECK_S(ytl_error(&g_tl), "ysp_timeline: sequence call 0: invalid argument");
    CHECK_I(ytl_animate_n(&q, &a, 4, 0), YTL_ERR_ARG);   /* sticky */
    q = ytl_seq_on(&g_tl, 1);
    a.n = 1;
    CHECK_I(ytl_animate_n(&q, &a, 4, 0), YTL_ERR_ARG);
    CHECK_I(ytl_tween_count(&g_tl, NULL), 0);
    ytl_close(&g_tl);
    q = ytl_seq_on(&g_tl, 1);
    CHECK_I(q.err, YTL_ERR_CLOSED);
    CHECK_I(ytl_kill(&g_tl, &f[0]), YTL_ERR_CLOSED);
    CHECK_I(ytl_tween_count(&g_tl, NULL), 0);
#undef TG_REJ
}

/* A set lands on the frame where an event at the same time lands, at
 * every lead, also behind a ramp whose start is in the same window. */
static void test_tg_step_quant(void) {
    static float x;
    static const double leads[3] = { 0.0, 0.25, YTL_LEAD_NONE };
    int it, li, bad = 0;
    g_rng = 99;
    for (li = 0; li < 3; li++) {
        for (it = 0; it < 300; it++) {
            int64_t P = rnd_range(1000000, 17000000), w = leads[li] == YTL_LEAD_NONE ? 0
                : (int64_t)((leads[li] == 0.0 ? 0.5 : leads[li]) * (double)P);
            int64_t kk = rnd_range(2, 20), ts = kk * P + w + rnd_range(-2, 2);
            ytl_seq q;
            int k, kev = -1, ktg = -1;
            ytl_desc d;
            memset(&d, 0, sizeof d);
            d.events = g_store;
            d.event_capacity = 8;
            d.n_channels = 1;
            d.lead = leads[li];
            d.targets = g_tgs;
            d.target_capacity = 2;
            d.tweens = g_tws;
            d.tween_capacity = 4;
            CHECK(ytl_open(&g_tl, &d));
            x = 0.0f;
            q = ytl_seq_on(&g_tl, 1);
            ytl_at(&q, ts - rnd_range(1, P));
            CHECK(ytl_tween_to(&q, &x, 5.0f, YRT_S(1), 0) >= 0);
            ytl_at(&q, ts);
            CHECK(ytl_tween_set(&q, &x, 7.0f) >= 0);
            CHECK(add1(&g_tl, 1, ts, YTL_SET, 0, 7.0f) >= 0);
            ytl_anchor(&g_tl, 1, 0, 0);
            for (k = 0; k < 30; k++) {
                eval_at(&g_tl, k * P, P, k, NULL, 0);
                if (kev < 0 && ytl_value(&g_tl, 0) == 7.0f) kev = k;
                if (ktg < 0 && x == 7.0f) ktg = k;
            }
            if (kev != ktg || kev < 0) bad++;
            /* v0.7.0: the set's record names the frame that showed it. */
            {
                const ytl_event* es = ytl_events(&g_tl, 1, &k);
                int i, nrec = 0;
                for (i = 0; i < k; i++) {
                    if (es[i].kind != YTL_TARGET_SET) continue;
                    nrec++;
                    if (es[i].frame != ktg || es[i].time_ns != ts || !(es[i].flags & YTL_EV_FIRED)) bad++;
                }
                if (nrec != 1) bad++;
            }
        }
    }
    CHECK_I(bad, 0);
    ytl_close(&g_tl);
}

/* A trial with every kind of call, recorded frame by frame. */
#define TGR_N 11
static float g_rf[TGR_N];
static int32_t g_ri;
static void tg_rinit(void) {
    int i;
    for (i = 0; i < TGR_N; i++) g_rf[i] = (float)i * 0.5f;
    g_ri = 7;
}

static void tg_rbuild(ytl_timeline* tl) {
    ytl_seq q = ytl_seq_on(tl, 1);
    ytl_anim a;
    ytl_tween_to(&q, &g_rf[0], 100.0f, YRT_MS(300), YTL_EASE_CUBIC_OUT);
    ytl_at(&q, YRT_MS(500));
    ytl_tween_to(&q, &g_rf[0], 0.0f, YRT_MS(200), YTL_EASE_QUAD_IN_OUT);
    ytl_at(&q, YRT_MS(100));
    a = tg_anim(&g_rf[1], 3.0f, YRT_MS(150), YTL_EASE_COSINE);
    a.from_set = true;
    a.from[0] = -3.0f;
    a.yoyo = true;
    a.cycles = 3;
    a.immediate = true;
    ytl_animate(&q, &a);
    a = tg_anim(&g_rf[2], 1.0f, YRT_MS(100), YTL_EASE_CUBIC_IN_OUT);
    a.n = 3;
    a.to[1] = 0.0f;
    a.to[2] = 0.25f;
    a.stagger_ns = YRT_MS(70);
    ytl_animate_n(&q, &a, 3 * sizeof(float), 2);
    ytl_at(&q, YRT_MS(250));
    ytl_tween_set(&q, &g_rf[8], 1.0f);
    ytl_at(&q, YRT_MS(700));
    ytl_tween_set(&q, &g_rf[8], 0.0f);
    ytl_at(&q, YRT_MS(200));
    ytl_tween_to(&q, &g_rf[9], 2.0f, YRT_MS(100), 0);
    ytl_at(&q, q.prev_end);    /* ">" */
    a = tg_anim(&g_rf[9], 0.0f, YRT_MS(300), 0);
    a.keep_velocity = true;
    ytl_animate(&q, &a);
    a = tg_anim(&g_ri, 20.0f, YRT_MS(400), YTL_EASE_CUBIC_IN);
    a.type = YTL_I32;
    a.cycles = YTL_FOREVER;
    ytl_animate(&q, &a);
    ytl_at(&q, YRT_MS(300));
    ytl_tween_from(&q, &g_rf[10], 9.0f, YRT_MS(100), 0);
}

#define TGR_FRAMES 90
static float g_rec[TGR_FRAMES][TGR_N + 1];

static void tg_rrow(float* row) {
    memcpy(row, g_rf, sizeof g_rf);
    memcpy(&row[TGR_N], &g_ri, sizeof g_ri);
}

/* Rewind and seek give the same bits as continuous play; two handles given
 * the same calls give the same bytes. */
static void test_tg_replay(void) {
    float row[TGR_N + 1];
    int k, bad = 0, j;
    CHECK(tg_open(&g_tl, g_store, 64, 16, 32, 0.0, 0));
    tg_rinit();
    tg_rbuild(&g_tl);
    CHECK_I(ytl_tween_count(&g_tl, &j), 11);
    CHECK_I(j, 8);
    ytl_anchor(&g_tl, 1, T0, 0);
    for (k = 0; k < TGR_FRAMES; k++) {
        tg_frame(&g_tl, k, NULL, 0);
        tg_rrow(g_rec[k]);
    }
    /* The fields are not all constant: the record is not vacuous. */
    CHECK(g_rec[10][0] != g_rec[20][0] && g_rec[10][1] != g_rec[12][1] && g_rec[5][3] != g_rec[8][3]);
    CHECK(g_rec[0][1] == -3.0f);              /* immediate */
    CHECK(g_rec[0][10] == 9.0f);              /* from(): immediate */
    CHECK(g_rec[0][9] == 4.5f);               /* not written yet */
    /* Again from 0, after a rewind. */
    ytl_anchor(&g_tl, 1, T0 + TGR_FRAMES * P60, 0);
    for (k = 0; k < TGR_FRAMES; k++) {
        tg_frame(&g_tl, TGR_FRAMES + k, NULL, 0);
        tg_rrow(row);
        if (memcmp(row, g_rec[k], sizeof row) != 0) bad++;
    }
    CHECK_I(bad, 0);
    /* After a rewind to 0, the fields written before show their rest
     * values; the immediate ones show their `from`. */
    ytl_anchor(&g_tl, 1, T0 + 2 * TGR_FRAMES * P60, 0);
    tg_frame(&g_tl, 2 * TGR_FRAMES, NULL, 0);
    tg_rrow(row);
    CHECK(row[0] == 0.0f && row[1] == -3.0f && row[8] == 4.0f && row[9] == 4.5f);

    /* A fresh handle seeks into the middle with ytl_skip(): from there on,
     * the values of continuous play. */
    for (j = 5; j < 80; j += 7) {
        CHECK(tg_open(&g_tl2, g_store2, 64, 16, 32, 0.0, 1));
        tg_rinit();
        tg_rbuild(&g_tl2);
        CHECK(ytl_skip(&g_tl2, 1, T0, j * P60) >= 0);
        for (k = j; k < TGR_FRAMES; k++) {
            eval_at(&g_tl2, T0 + (k - j) * P60, P60, k, NULL, 0);
            tg_rrow(row);
            if (memcmp(row, g_rec[k], sizeof row) != 0) {
                if (bad++ < 3) fprintf(stderr, "  seek to frame %d: frame %d differs\n", j, k);
            }
        }
    }
    CHECK_I(bad, 0);

    /* The same calls into two handles: the same pools, storages, fields. */
    CHECK(tg_open(&g_tl, g_store, 64, 16, 32, 0.0, 0));
    CHECK(tg_open(&g_tl2, g_store2, 64, 16, 32, 0.0, 1));
    memset(g_tgs, 0, sizeof g_tgs);
    memset(g_tgs2, 0, sizeof g_tgs2);
    for (j = 0; j < 2; j++) {
        ytl_timeline* tl = j ? &g_tl2 : &g_tl;
        tg_rinit();
        tg_rbuild(tl);
        ytl_anchor(tl, 1, T0, 0);
        for (k = 0; k < 40; k++) tg_frame(tl, k, NULL, 0);
        ytl_anchor(tl, 1, T0 + 40 * P60, YRT_MS(150));
        ytl_kill(tl, &g_rf[1]);
        for (k = 40; k < 60; k++) tg_frame(tl, k, NULL, 0);
        tg_rrow(j ? row : g_rec[0]);
    }
    CHECK(memcmp(row, g_rec[0], sizeof row) == 0);
    CHECK(memcmp(g_store, g_store2, 64 * sizeof g_store[0]) == 0);
    CHECK(memcmp(g_tgs, g_tgs2, sizeof g_tgs) == 0);
    CHECK(memcmp(g_tws, g_tws2, sizeof g_tws) == 0);
    ytl_close(&g_tl);
    ytl_close(&g_tl2);
}

/* The random model: TARGETS' rules written here (tg_meval) against the
 * header over random calls, frames, rewinds, skips, pauses and kills. */
static int g_tg_stat_add, g_tg_stat_rew, g_tg_stat_kill, g_tg_stat_step, g_tg_stat_imm;

static void test_tg_model_seed(uint64_t seed) {
    static float fa[2], fb[2];
    static int32_t fi;
    static tg_mtg m[4];
    void* ptrs[4];
    int types[4] = { YTL_F32, YTL_F32, YTL_F32, YTL_I32 }, ns[4] = { 1, 1, 2, 1 };
    int64_t onset = T0, cursor_bt = 0;
    int step, i, c, paused = 0, next_id = 0, bad = 0;
    int64_t frame = 0;
    const char* tr = getenv("TG_TRACE");
    int trace = tr && (uint64_t)strtoul(tr, NULL, 10) == seed;
    ptrs[0] = &fa[0];
    ptrs[1] = &fa[1];
    ptrs[2] = fb;
    ptrs[3] = &fi;
    g_rng = seed;
    CHECK(tg_open(&g_tl, g_store, 64, 4, 2 * TG_CAP, 0.0, 0));
    fa[0] = 1.0f; fa[1] = -2.0f; fb[0] = 0.5f; fb[1] = 3.0f; fi = 4;
    memset(m, 0, sizeof m);
    for (i = 0; i < 4; i++) {
        m[i].ptr = ptrs[i];
        m[i].type = types[i];
        m[i].n = ns[i];
        m[i].base = 1;
    }
    ytl_anchor(&g_tl, 1, onset, 0);
    for (step = 0; step < 500; step++) {
        int op = rnd_int(20);
        if (op < 4) {
            /* a tween on a random field, at a time around now */
            int t = rnd_int(4), kind = rnd_int(5);
            tg_mtg* g = &m[t];
            ytl_anim a = tg_anim(ptrs[t], (float)rnd_range(-40, 40) / 4.0f, rnd_range(0, 300) * MS_NS,
                                 rnd_int(YTL_EASE_CUBIC_IN_OUT + 1));
            int64_t s = cursor_bt + rnd_range(-100, 400) * MS_NS;
            ytl_seq q;
            int id;
            a.type = types[t];
            a.n = ns[t];
            for (c = 1; c < 4; c++) a.to[c] = (float)rnd_range(-40, 40) / 4.0f;
            if (a.ease == YTL_EASE_LOG || a.ease == YTL_EASE_BEZIER) a.ease = YTL_EASE_STEP;
            if (kind == 1) { a.from_set = true; a.to_current = true; a.immediate = rnd_int(2); }
            if (kind == 2) { a.from_set = true; a.immediate = rnd_int(2); }
            if (kind == 3) { a.ease = YTL_EASE_STEP; a.duration_ns = 0; }
            for (c = 0; c < 4; c++) a.from[c] = (float)rnd_range(-40, 40) / 4.0f;
            if (a.duration_ns > 0 && rnd_int(4) == 0) { a.cycles = 2 + rnd_int(2); a.yoyo = rnd_int(2); }
            if (kind == 4 && a.duration_ns > 0 && !a.from_set && !a.cycles) {
                a.ease = YTL_EASE_LINEAR;
                a.keep_velocity = true;
            }
            q = ytl_seq_on(&g_tl, 1);
            ytl_at(&q, s);
            id = ytl_animate(&q, &a);
            if (id < 0) continue;
            CHECK_I(id, next_id);
            next_id++;
            if (trace)
                fprintf(stderr, "step %d: add id %d field %d kind %d s %.3f d %.3f ease %d to %g from %g fs %d tc %d im %d cyc %d yoyo %d kv %d\n",
                        step, id, t, kind, s / 1e6, a.duration_ns / 1e6, a.ease, a.to[0], a.from[0], a.from_set,
                        a.to_current, a.immediate, a.cycles, a.yoyo, a.keep_velocity);
            if (g->ntw >= 48) {
                CHECK_I(ytl_kill_tween(&g_tl, id), 0);
                continue;
            }
            g->tw[g->ntw++] = tg_mfrom_anim(&a, s, id);
            g_tg_stat_add++;
            if (a.ease == YTL_EASE_STEP) g_tg_stat_step++;
            if (a.immediate) g_tg_stat_imm++;
        } else if (op == 4 && rnd_int(3) == 0) {
            /* a rewind or a seek on the trial base */
            int64_t bt = rnd_range(-50, 1500) * MS_NS;
            if (rnd_int(2)) ytl_anchor(&g_tl, 1, onset, bt);
            else ytl_skip(&g_tl, 1, onset, bt);
            if (trace) fprintf(stderr, "step %d: seek to %.3f\n", step, bt / 1e6);
            paused = 0;
            g_tg_stat_rew++;
        } else if (op == 5 && rnd_int(3) == 0) {
            if (!paused) { ytl_pause(&g_tl, 1, onset); paused = 1; }
            else { ytl_resume(&g_tl, 1, onset); paused = 0; }
        } else if (op == 6 && rnd_int(4) == 0) {
            int t = rnd_int(4);
            if (m[t].ntw && rnd_int(2)) {
                int j = rnd_int(m[t].ntw);
                CHECK_I(ytl_kill_tween(&g_tl, m[t].tw[j].id), 0);
                m[t].tw[j] = m[t].tw[--m[t].ntw];
                if (m[t].ntw == 0) { m[t].rest_set = 0; m[t].written = 0; }
            } else {
                CHECK_I(ytl_kill(&g_tl, ptrs[t]), m[t].ntw);
                m[t].ntw = 0;
                m[t].rest_set = 0;
                m[t].written = 0;
            }
            g_tg_stat_kill++;
        } else {
            int64_t bt, hi;
            onset += P60 + (rnd_int(8) == 0 ? rnd_range(0, 3) * P60 : 0);
            for (i = 0; i < 4; i++)
                for (c = 0; c < ns[i]; c++)
                    m[i].pre[c] = types[i] == YTL_I32 ? (double)((int32_t*)ptrs[i])[c] : (double)((float*)ptrs[i])[c];
            CHECK(eval_at(&g_tl, onset, P60, frame++, NULL, 0) >= 0);
            if (!ytl_base_time(&g_tl, 1, onset, &bt)) continue;
            if (paused) hi = bt;
            else if (!ytl_base_time(&g_tl, 1, onset + P60 / 2, &hi)) continue;
            cursor_bt = bt;
            if (trace) fprintf(stderr, "step %d: frame at %.3f hi %.3f%s\n", step, bt / 1e6, hi / 1e6, paused ? " paused" : "");
            for (i = 0; i < 4; i++) {
                double v[4];
                if (!tg_meval(&m[i], bt, hi, v)) continue;
                for (c = 0; c < ns[i]; c++) {
                    double got = types[i] == YTL_I32 ? (double)((int32_t*)ptrs[i])[c] : (double)((float*)ptrs[i])[c];
                    double want = types[i] == YTL_I32 ? floor(v[c] + 0.5) : v[c];
                    if (!(fabs(got - want) <= 1e-4 * (1.0 + fabs(want)))) {
                        if (bad++ < 3)
                            fprintf(stderr, "  tg model seed %llu step %d field %d.%d: got %.9g want %.9g\n",
                                    (unsigned long long)seed, step, i, c, got, want);
                    }
                }
            }
        }
    }
    if (bad) fail(__LINE__, "target model disagrees");
    ytl_close(&g_tl);
}

static void test_tg_model(void) {
    uint64_t s;
    for (s = 1; s <= 12; s++) test_tg_model_seed(1000 + s);
    if (g_tg_stat_add < 1000 || g_tg_stat_rew < 50 || g_tg_stat_kill < 30 || g_tg_stat_step < 200
        || g_tg_stat_imm < 50)
        fail(__LINE__, "target model: too few tweens, rewinds, kills, steps or immediates (vacuous)");
    printf("timeline_test: target model: %d tweens (%d STEP, %d immediate), %d rewinds or seeks, %d kills\n",
           g_tg_stat_add, g_tg_stat_step, g_tg_stat_imm, g_tg_stat_rew, g_tg_stat_kill);
}

/* ---------------------------------------------------------------- v0.7.0 */

/* Float distance in ulps, with -0 and +0 the same. */
static int64_t tg_ulps(float a, float b) {
    int32_t ia, ib;
    memcpy(&ia, &a, sizeof ia);
    memcpy(&ib, &b, sizeof ib);
    if (ia < 0) ia = INT32_MIN - ia;
    if (ib < 0) ib = INT32_MIN - ib;
    return ia > ib ? (int64_t)ia - ib : (int64_t)ib - ia;
}

/* Yoyo: the return leg is the forward leg backward in time (GSAP), STEP
 * excepted (test_tween_step). Every other ease, BEZIER on an asymmetric
 * curve, as a channel tween, a sequence tween and a target tween with 2
 * cycles at 1 ms onsets: the three give the same bits, each is within 2e-6
 * of tg_shape() read at the mirrored time, and v(s + d + x) is measured
 * against v(s + d - x) in float ulps. */
static ytl_key g_yk[64];

static void test_tg_yoyo(void) {
    static float x, v[201];
    const int64_t M = MS_NS, S = 10 * MS_NS, D = 100 * MS_NS;
    int ease, k, bad_ref = 0, bad_paths = 0, max_ease = -1;
    int64_t max_ulps = 0;
    for (ease = 0; ease <= YTL_EASE_CUBIC_IN_OUT; ease++) {
        ytl_desc d;
        ytl_tween_desc td;
        ytl_seq q;
        ytl_anim a;
        const double fr = 0.5, to = 4.0;
        if (ease == YTL_EASE_STEP) continue;
        memset(&d, 0, sizeof d);
        d.events = g_store;
        d.event_capacity = 8;
        d.n_channels = 2;
        d.keys = g_yk;
        d.key_capacity = 64;
        d.targets = g_tgs;
        d.target_capacity = 2;
        d.tweens = g_tws;
        d.tween_capacity = 4;
        CHECK(ytl_open(&g_tl, &d));
        memset(&td, 0, sizeof td);
        td.from = (float)fr;
        td.from_set = true;
        td.to = (float)to;
        td.duration_ns = D;
        td.ease = ease;
        td.curve = mkcurve(0.05f, 0.7f, 0.1f, 1.0f);   /* a strong ease-out */
        td.yoyo = true;
        td.cycles = 2;
        td.start_ns = S;
        td.start_set = true;
        CHECK_I(ytl_tween(&g_tl, 0, 1, &td), 0);
        td.start_set = false;
        td.start_ns = 0;
        q = ytl_seq_on(&g_tl, 1);
        ytl_at(&q, S);
        CHECK_I(ytl_to(&q, 1, &td), S + 4 * D);
        x = (float)fr;
        a = tg_from_desc(&x, &td);
        CHECK(ytl_animate(&q, &a) >= 0);
        CHECK_I(q.err, 0);
        CHECK_I(ytl_anchor(&g_tl, 1, T0, 0), 0);
        for (k = 0; k <= 450; k++) {
            int64_t bt = k * M, ph = bt - S;
            float vc, vs;
            double want, u;
            eval_at(&g_tl, T0 + bt, M, k, NULL, 0);
            if (bt < S) continue;   /* the channels hold 0 before the start */
            vc = ytl_value(&g_tl, 0);
            vs = ytl_value(&g_tl, 1);
            if (memcmp(&vc, &vs, sizeof vc) != 0 || memcmp(&vc, &x, sizeof vc) != 0) {
                if (bad_paths++ < 3)
                    fprintf(stderr, "  yoyo ease %d at %d ms: channel %.9g sequence %.9g target %.9g\n", ease, k,
                            (double)vc, (double)vs, (double)x);
            }
            /* The rule read literally: fold the cycle, mirror the return
             * leg, read the forward leg. */
            if (ph >= 4 * D) {
                ph = 0;
            } else {
                ph %= 2 * D;
                if (ph > D) ph = 2 * D - ph;
            }
            u = (double)ph / (double)D;
            want = ease == YTL_EASE_LOG ? fr * pow(to / fr, u) : fr + (to - fr) * tg_shape(ease, &td.curve, u);
            if (!(fabs((double)vc - want) <= 2e-6 * (1.0 + fabs(want)))) {
                if (bad_ref++ < 3)
                    fprintf(stderr, "  yoyo ease %d at %d ms: got %.9g, the forward leg mirrored %.9g\n", ease, k,
                            (double)vc, want);
            }
            if (bt <= S + 2 * D) v[(bt - S) / M] = vc;
        }
        for (k = 1; k < 100; k++) {
            int64_t e = tg_ulps(v[100 + k], v[100 - k]);
            if (e > max_ulps) {
                max_ulps = e;
                max_ease = ease;
            }
        }
        /* CUBIC_OUT's return leg starts slow and ends fast; v0.6.0's started
         * fast. */
        if (ease == YTL_EASE_CUBIC_OUT) CHECK(fabs(v[110] - v[100]) * 100.0 < fabs(v[200] - v[190]));
        ytl_close(&g_tl);
    }
    CHECK_I(bad_paths, 0);
    CHECK_I(bad_ref, 0);
    CHECK(max_ulps <= 4);
    printf("timeline_test: yoyo: the return leg within %lld float ulp of the forward leg mirrored (ease %d)\n",
           (long long)max_ulps, max_ease);
}

/* A set leaves a YTL_TARGET_SET record, which lands, rewinds, skips and goes
 * with its tween as a callback's event does. */
static void test_tg_records(void) {
    static float x, y, rgb[3];
    ytl_seq q;
    ytl_anim a;
    ytl_event fired[8];
    const ytl_event* es;
    int id, id2, k, n, i, krec, kx = -1;
    CHECK(tg_open(&g_tl, g_store, 64, 8, 16, 0.0, 0));
    x = 0.0f;
    q = ytl_seq_on(&g_tl, 1);
    ytl_at(&q, YRT_MS(100));
    id = ytl_tween_set(&q, &x, 3.0f);
    CHECK(id >= 0);
    es = ytl_events(&g_tl, 1, &n);
    CHECK_I(n, 1);
    CHECK_I(es[0].kind, YTL_TARGET_SET);
    CHECK_I(es[0].code, id);
    CHECK(es[0].value == 3.0f);
    CHECK(es[0].user == (uint64_t)(uintptr_t)&x);
    CHECK_I(es[0].time_ns, YRT_MS(100));
    CHECK_I(es[0].frame, -1);
    /* Only a set: not a jump with another ease, a STEP with a duration, or a
     * STEP to the value at its start. A set of 3 records to[0]. */
    CHECK(ytl_tween_to(&q, &y, 1.0f, 0, YTL_EASE_LINEAR) >= 0);
    CHECK(ytl_tween_to(&q, &y, 1.0f, YRT_MS(10), YTL_EASE_STEP) >= 0);
    a = tg_anim(&y, 0.0f, 0, YTL_EASE_STEP);
    a.to_current = true;
    CHECK(ytl_animate(&q, &a) >= 0);
    CHECK_I(n_events(&g_tl, 1), 1);
    a = tg_anim(rgb, 0.25f, 0, YTL_EASE_STEP);
    a.n = 3;
    a.to[1] = 0.5f;
    a.to[2] = 0.75f;
    id2 = ytl_animate(&q, &a);
    CHECK(id2 >= 0);
    CHECK_I(q.err, 0);
    es = ytl_events(&g_tl, 1, &n);
    CHECK_I(n, 2);
    CHECK(n == 2 && es[1].code == id2 && es[1].value == 0.25f && es[1].user == (uint64_t)(uintptr_t)rgb);
    /* It lands on the frame that shows the set, and is in the report. */
    ytl_anchor(&g_tl, 1, T0, 0);
    krec = -1;
    for (k = 0; k < 12; k++) {
        n = tg_frame(&g_tl, k, fired, 8);
        for (i = 0; i < n && i < 8; i++) {
            if (fired[i].kind != YTL_TARGET_SET || fired[i].code != id) continue;
            krec = k;
            CHECK_I(fired[i].frame, k);
        }
        if (kx < 0 && x == 3.0f) kx = k;
    }
    CHECK(kx > 0);
    CHECK_I(krec, kx);
    /* A rewind fires it again; a skip over it does not. */
    ytl_anchor(&g_tl, 1, T0 + 12 * P60, 0);
    krec = -1;
    for (k = 12; k < 24; k++) {
        n = tg_frame(&g_tl, k, fired, 8);
        for (i = 0; i < n && i < 8; i++)
            if (fired[i].kind == YTL_TARGET_SET && fired[i].code == id) krec = k;
    }
    CHECK_I(krec, 12 + kx);
    CHECK(ytl_skip(&g_tl, 1, T0 + 24 * P60, 0) >= 0);
    CHECK(ytl_skip(&g_tl, 1, T0 + 24 * P60, YRT_MS(400)) >= 0);
    es = ytl_events(&g_tl, 1, &n);
    for (i = 0; i < n; i++)
        if (es[i].code == id) CHECK(es[i].flags == YTL_EV_SKIPPED && es[i].frame == -1);
    /* A kill deletes it, fired or not. */
    CHECK_I(ytl_kill_tween(&g_tl, id), 0);
    es = ytl_events(&g_tl, 1, &n);
    CHECK_I(n, 1);
    CHECK(n == 1 && es[0].code == id2);

    /* A set needs an event of the storage, all or none; an overwrite that
     * kills a set frees its record; a cancel deletes it. */
    CHECK(tg_open(&g_tl, g_store, 1, 4, 8, 0.0, 0));
    q = ytl_seq_on(&g_tl, 1);
    CHECK(ytl_tween_set(&q, &x, 1.0f) >= 0);
    CHECK_I(ytl_tween_set(&q, &y, 1.0f), YTL_ERR_FULL);
    CHECK(strstr(ytl_error(&g_tl), "event storage is full") != NULL);
    CHECK_I(ytl_tween_count(&g_tl, NULL), 1);
    q = ytl_seq_on(&g_tl, 1);
    a = tg_anim(&x, 2.0f, 0, YTL_EASE_STEP);
    a.overwrite = YTL_OVERWRITE_ALL;
    CHECK(ytl_animate(&q, &a) >= 0);
    CHECK_I(n_events(&g_tl, 1), 1);
    CHECK_I(ytl_tween_count(&g_tl, NULL), 1);
    CHECK_I(ytl_seq_cancel(&q), 1);
    CHECK_I(n_events(&g_tl, 1), 0);
    ytl_close(&g_tl);
}

/* A trial whose fields are retargeted all the time, for test_tg_prune(). */
static void tgp_build(ytl_timeline* tl, float* f) {
    ytl_seq q = ytl_seq_on(tl, 1);
    ytl_anim a;
    int i;
    static const int eases[3] = { YTL_EASE_LINEAR, YTL_EASE_CUBIC_OUT, YTL_EASE_QUAD_IN };
    for (i = 0; i < 31; i++) {
        ytl_at(&q, i * YRT_MS(40));
        ytl_tween_to(&q, &f[0], (float)(i % 5), YRT_MS(70), eases[i % 3]);
    }
    for (i = 0; i < 40; i++) {
        ytl_at(&q, i * YRT_MS(30) + YRT_MS(5));
        ytl_tween_set(&q, &f[1], (float)(i % 4));
    }
    ytl_at(&q, 0);
    a = tg_anim(&f[2], 3.0f, YRT_MS(120), YTL_EASE_CUBIC_OUT);
    a.yoyo = true;
    a.cycles = 3;
    ytl_animate(&q, &a);
    ytl_at(&q, YRT_MS(500));
    ytl_tween_to(&q, &f[2], -1.0f, YRT_MS(200), YTL_EASE_COSINE);
    ytl_at(&q, YRT_MS(900));
    ytl_tween_from_to(&q, &f[2], 5.0f, 6.0f, YRT_MS(300), YTL_EASE_QUAD_OUT);
    /* f[3]: a long tween with an on_complete, overridden at 100 ms. */
    ytl_at(&q, 0);
    a = tg_anim(&f[3], 9.0f, YRT_S(10), YTL_EASE_LINEAR);
    a.on_complete = tg_on_complete;
    a.user = &g_log;
    ytl_animate(&q, &a);
    ytl_at(&q, YRT_MS(100));
    ytl_tween_to(&q, &f[3], 1.0f, YRT_MS(50), YTL_EASE_LINEAR);
    if (q.err < 0) fail(__LINE__, "tgp_build: a call failed");
}

/* ytl_prune() frees the tweens a later tween has overridden for good, and
 * sets the horizon: from there on a rewind or a seek replays the fields bit
 * for bit; before it, the fields show what remains. */
static void test_tg_prune(void) {
    static float fa[4], fb[4], x, y;
    ytl_seq q;
    ytl_frame f;
    int k, n_a, n_b, j, bad = 0, differ = 0, id, maxc = 0, full = -1;
    int64_t h = -1, end = -2, seeks[3];
    CHECK(tg_open(&g_tl, g_store, 128, 4, 2 * TG_CAP, 0.0, 0));
    CHECK(tg_open(&g_tl2, g_store2, 128, 4, 2 * TG_CAP, 0.0, 1));
    memset(&g_log, 0, sizeof g_log);
    memset(fa, 0, sizeof fa);
    memset(fb, 0, sizeof fb);
    tgp_build(&g_tl, fa);
    tgp_build(&g_tl2, fb);
    CHECK(!ytl_horizon(&g_tl, 1, &h));
    ytl_anchor(&g_tl, 1, T0, 0);
    ytl_anchor(&g_tl2, 1, T0, 0);
    for (k = 0; k < 60; k++) {
        tg_frame(&g_tl, k, NULL, 0);
        tg_frame(&g_tl2, k, NULL, 0);
        if (memcmp(fa, fb, sizeof fa) != 0) bad++;
    }
    f.onset = T0 + 59 * P60;
    f.period = P60;
    f.index = 59;
    CHECK(ytl_window(&g_tl, 1, &f, &end));
    n_b = ytl_tween_count(&g_tl, NULL);
    CHECK(ytl_prune(&g_tl, 1) > 0);
    n_a = ytl_tween_count(&g_tl, NULL);
    /* At 983 ms: f[0]: 25 started, 24 freed; f[1]: 33 sets, 32 freed;
     * f[2]: 3 started, 2 freed; f[3]: none, the long tween waits for its
     * on_complete. */
    CHECK_I(n_b - n_a, 24 + 32 + 2);
    CHECK(ytl_horizon(&g_tl, 1, &h));
    CHECK_I(h, end + 1);
    /* Continuous play, then rewinds and seeks inside the window. */
    for (k = 60; k < 90; k++) {
        tg_frame(&g_tl, k, NULL, 0);
        tg_frame(&g_tl2, k, NULL, 0);
        if (memcmp(fa, fb, sizeof fa) != 0) bad++;
    }
    seeks[0] = h;
    seeks[1] = h + 123456789;
    seeks[2] = h + YRT_MS(50);
    for (j = 0; j < 3; j++) {
        int64_t k0 = 90 + j * 30;
        if (j < 2) {
            CHECK_I(ytl_anchor(&g_tl, 1, T0 + k0 * P60, seeks[j]), 0);
            CHECK_I(ytl_anchor(&g_tl2, 1, T0 + k0 * P60, seeks[j]), 0);
        } else {
            CHECK(ytl_skip(&g_tl, 1, T0 + k0 * P60, seeks[j]) >= 0);
            CHECK(ytl_skip(&g_tl2, 1, T0 + k0 * P60, seeks[j]) >= 0);
        }
        for (k = (int)k0; k < k0 + 30; k++) {
            tg_frame(&g_tl, k, NULL, 0);
            tg_frame(&g_tl2, k, NULL, 0);
            if (memcmp(fa, fb, sizeof fa) != 0) {
                if (bad++ < 3) fprintf(stderr, "  prune: seek %d frame %d differs\n", j, k);
            }
        }
    }
    CHECK_I(bad, 0);
    /* Before the horizon the freed tweens are gone: f[0] and f[1] show their
     * rest values before their first remaining tween. */
    CHECK_I(ytl_anchor(&g_tl, 1, T0 + 180 * P60, 0), 0);
    CHECK_I(ytl_anchor(&g_tl2, 1, T0 + 180 * P60, 0), 0);
    for (k = 180; k < 200; k++) {
        tg_frame(&g_tl, k, NULL, 0);
        tg_frame(&g_tl2, k, NULL, 0);
        if (memcmp(fa, fb, sizeof fa) != 0) differ++;
        if (fa[0] != 0.0f || fa[1] != 0.0f) bad++;
    }
    CHECK(differ > 0);
    CHECK_I(bad, 0);
    /* A prune after that rewind keeps the horizon: what the first deleted
     * is still gone. */
    {
        int64_t h2 = -1;
        CHECK(ytl_prune(&g_tl, 1) >= 0);
        CHECK(ytl_horizon(&g_tl, 1, &h2));
        CHECK_I(h2, h);
    }
    /* A clear ends the horizon; bad arguments are false. */
    CHECK_I(ytl_horizon(&g_tl, 0, &h), 0);
    CHECK(!ytl_horizon(&g_tl, 8, &h));
    CHECK(!ytl_horizon(&g_tl, 1, NULL));
    CHECK(ytl_clear(&g_tl, 1) >= 0);
    CHECK(!ytl_horizon(&g_tl, 1, &h));
    ytl_close(&g_tl);
    ytl_close(&g_tl2);
    CHECK(!ytl_horizon(&g_tl, 1, &h));

    /* An event still to fire keeps its tween: a long tween's on_complete,
     * and a late on_start. */
    CHECK(tg_open(&g_tl, g_store, 16, 2, 8, 0.0, 0));
    memset(&g_log, 0, sizeof g_log);
    x = 0.0f;
    q = ytl_seq_on(&g_tl, 1);
    {
        ytl_anim a = tg_anim(&x, 1.0f, YRT_S(2), YTL_EASE_LINEAR);
        a.on_complete = tg_on_complete;
        a.user = &g_log;
        CHECK(ytl_animate(&q, &a) >= 0);
        ytl_at(&q, YRT_MS(100));
        CHECK(ytl_tween_to(&q, &x, 5.0f, YRT_MS(50), 0) >= 0);
        ytl_anchor(&g_tl, 1, T0, 0);
        for (k = 0; k < 30; k++) tg_frame(&g_tl, k, NULL, 0);
        CHECK(ytl_prune(&g_tl, 1) >= 0);
        CHECK_I(ytl_tween_count(&g_tl, NULL), 2);
        for (k = 30; k < 130; k++) tg_frame(&g_tl, k, NULL, 0);
        CHECK_I(g_log.n, 1);
        CHECK(ytl_prune(&g_tl, 1) >= 0);
        CHECK_I(ytl_tween_count(&g_tl, NULL), 1);
        /* A tween added in the past, before the driver's start, with an
         * on_start that fires late at the next evaluate. */
        a = tg_anim(&x, 7.0f, YRT_MS(10), YTL_EASE_LINEAR);
        a.on_start = tg_on_start;
        a.user = &g_log;
        ytl_at(&q, YRT_MS(50));
        CHECK(ytl_animate(&q, &a) >= 0);
        CHECK(ytl_prune(&g_tl, 1) >= 0);
        CHECK_I(ytl_tween_count(&g_tl, NULL), 2);
        tg_frame(&g_tl, 130, NULL, 0);
        CHECK_I(g_log.n, 2);
        CHECK(x == 5.0f);
        CHECK(ytl_prune(&g_tl, 1) >= 0);
        CHECK_I(ytl_tween_count(&g_tl, NULL), 1);
    }

    /* A prune from a callback keeps the tweens whose calls are queued or
     * running. A, A2 and B start at 0, B last; A's on_start prunes, and A's
     * on_complete and A2's on_start of the same frame still run. */
    CHECK(tg_open(&g_tl, g_store, 16, 2, 8, 0.0, 0));
    memset(&g_log, 0, sizeof g_log);
    q = ytl_seq_on(&g_tl, 1);
    {
        ytl_anim a = tg_anim(&x, 1.0f, 0, YTL_EASE_LINEAR);
        a.on_start = tg_on_start;
        a.on_complete = tg_on_complete;
        a.user = &g_log;
        id = ytl_animate(&q, &a);
        a.on_complete = NULL;
        CHECK(ytl_animate(&q, &a) >= 0);
        CHECK(ytl_tween_to(&q, &x, 2.0f, 0, 0) >= 0);
        g_log.prune = 1;
        ytl_anchor(&g_tl, 1, T0, 0);
        g_cb_frame = 0;
        tg_frame(&g_tl, 0, NULL, 0);
        CHECK_I(g_log.n, 3);
        CHECK(g_log.n == 3 && g_log.id[1] == id && g_log.kind[1] == 1 && g_log.id[2] == id + 1);
        /* A2's own call freed A, whose calls had run. */
        CHECK_I(ytl_tween_count(&g_tl, NULL), 2);
        g_log.prune = 0;
        CHECK(ytl_prune(&g_tl, 1) >= 0);
        CHECK_I(ytl_tween_count(&g_tl, NULL), 1);
        CHECK(x == 2.0f);
    }

    /* A script on the RT base that retargets a field every frame and sets
     * another every 5th, with a prune every 8th, for 100,000 frames (28
     * minutes at 60 Hz) in a pool of 16 tweens and 64 events. A second
     * handle plays the same calls with YTL_OVERWRITE_ALL, whose new tween
     * starts from the value written last, which is the takeover's here: the
     * same bits on every frame. Without the prune the pool is full within 16
     * frames. */
    for (j = 0; j < 2; j++) {
        static float x2, y2;
        CHECK(tg_open(&g_tl, g_store, 64, 2, 16, 0.0, 0));
        CHECK(tg_open(&g_tl2, g_store2, 64, 2, 4, 0.0, 1));
        x = y = x2 = y2 = 0.0f;
        bad = 0;
        for (k = 0; k < 100000; k++) {
            int rc;
            ytl_anim a;
            q = ytl_seq_now(&g_tl, YTL_BASE_RT);
            rc = ytl_tween_to(&q, &x, (float)(k % 7), YRT_MS(100), YTL_EASE_CUBIC_OUT);
            if (rc >= 0 && k % 5 == 0) rc = ytl_tween_set(&q, &y, (float)(k % 3));
            if (rc < 0) {
                full = k;
                break;
            }
            q = ytl_seq_now(&g_tl2, YTL_BASE_RT);
            a = tg_anim(&x2, (float)(k % 7), YRT_MS(100), YTL_EASE_CUBIC_OUT);
            a.overwrite = YTL_OVERWRITE_ALL;
            ytl_animate(&q, &a);
            if (k % 5 == 0) {
                a = tg_anim(&y2, (float)(k % 3), 0, YTL_EASE_STEP);
                a.overwrite = YTL_OVERWRITE_ALL;
                ytl_animate(&q, &a);
            }
            CHECK_I(q.err, 0);
            /* Between the posts and the frame, so the newest tween has
             * started (at the last onset) but has no start value yet. */
            if (j == 0 && k % 8 == 7) {
                ytl_prune(&g_tl, YTL_BASE_RT);
                ytl_prune(&g_tl2, YTL_BASE_RT);
            }
            eval_at(&g_tl, T0 + k * P60, P60, k, NULL, 0);
            eval_at(&g_tl2, T0 + k * P60, P60, k, NULL, 0);
            if (memcmp(&x, &x2, sizeof x) != 0 || memcmp(&y, &y2, sizeof y) != 0) {
                if (bad++ < 3) fprintf(stderr, "  prune: long run frame %d: %.9g %.9g against %.9g %.9g\n", k,
                                       (double)x, (double)y, (double)x2, (double)y2);
            }
            if (ytl_tween_count(&g_tl, NULL) > maxc) maxc = ytl_tween_count(&g_tl, NULL);
        }
        CHECK_I(bad, 0);
        if (j == 0) {
            CHECK_I(full, -1);
            CHECK(maxc <= 12);
            CHECK(n_events(&g_tl, YTL_BASE_RT) <= 3);
            printf("timeline_test: prune: 100000 frames of retargets in a pool of 16, at most %d tweens live\n", maxc);
        } else {
            CHECK(full > 0 && full <= 16);
        }
        ytl_close(&g_tl);
        ytl_close(&g_tl2);
    }
}

static void test_targets(void) {
    test_tg_basic();
    test_tg_eases();
    test_tg_vs_tween();
    test_tg_overwrite();
    test_tg_callbacks();
    test_tg_pools();
    test_tg_step_quant();
    test_tg_replay();
    test_tg_model();
    test_tg_yoyo();
    test_tg_records();
    test_tg_prune();
}
