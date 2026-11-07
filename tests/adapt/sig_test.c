/* sig_test.c - self-checking test for ysp/sig.h. Returns 0 when every
 * check passed, 1 after printing each failure.
 *
 * Each op against a closed form (the count sum and its bounds, a rate
 * integral, a filter's step response, a ramp's velocity, a delay, sums of
 * sines at known phases), the schedules, the tier and t_oldest rule, the
 * out-of-order drop, detectors (crossings, hysteresis, holds completed by a
 * sample and by a frame, arm), derived events, records and the digest,
 * reset, replay bit for bit, values at a past time, roles and adapters, a
 * custom op, ysig_set(), and the refusals of bad graphs. The header must
 * not call the allocator: malloc and its kin are renamed to undefined
 * symbols before the implementation, so a call fails the link.
 *
 *     gcc -std=c99 -Wall -Wextra -Wpedantic -Wshadow -Werror -O2 -Iinclude \
 *         -o sig_test tests/adapt/sig_test.c -lm && ./sig_test
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define malloc  ysig_test_no_malloc
#define calloc  ysig_test_no_calloc
#define realloc ysig_test_no_realloc
#define free    ysig_test_no_free
#define YSP_SIG_IMPLEMENTATION
#include "ysp/sig.h"
#undef malloc
#undef calloc
#undef realloc
#undef free

/* ---------------------------------------------------------------- harness */

static int g_failures = 0;
static const char* g_where = "";

#define CHECK(cond) do { if (!(cond)) { \
    fprintf(stderr, "sig_test [%s]: FAIL line %d: %s\n", g_where, __LINE__, #cond); g_failures++; } } while (0)
#define CHECK_NEAR(a, b, tol) do { double a_ = (double)(a), b_ = (double)(b); if (!(fabs(a_ - b_) <= (tol))) { \
    fprintf(stderr, "sig_test [%s]: FAIL line %d: %s = %.17g, want %.17g (tol %g)\n", g_where, __LINE__, #a, a_, b_, \
            (double)(tol)); g_failures++; } } while (0)

#define MS 1000000LL
#define PI_ 3.14159265358979323846

static uint64_t g_arena[(4u << 20) / 8];   /* 4 MB, 8-byte aligned: the header's need */
static float g_ch[64];

static ysig_node src_node(int kind, int control, int device, int lanes) {
    ysig_node n = ysig_node_of(YSIG_OP_SOURCE, -1, -1);
    n.p[0] = kind;
    n.p[1] = control;
    n.p[2] = device;
    n.lanes = (uint8_t)lanes;
    return n;
}

static yin_event delta(int64_t t, int device, float dx, float dy) {
    yin_event e;
    memset(&e, 0, sizeof e);
    e.t = t;
    e.kind = YIN_KIND_MOUSE;
    e.type = YIN_SAMPLE;
    e.control = YIN_AXIS_DELTA;
    e.device = (uint32_t)device;
    e.x = dx;
    e.y = dy;
    e.stamp = YIN_TIER_SIM;
    return e;
}

static yin_event axis(int64_t t, int kind, int control, int device, float x, float y, float v, int stamp) {
    yin_event e;
    memset(&e, 0, sizeof e);
    e.t = t;
    e.kind = (uint8_t)kind;
    e.type = YIN_SAMPLE;
    e.control = (uint32_t)control;
    e.device = (uint32_t)device;
    e.x = x;
    e.y = y;
    e.value = v;
    e.stamp = (uint8_t)stamp;
    return e;
}

static ysig_desc desc_of(const ysig_node* n, int count) {
    ysig_desc d;
    memset(&d, 0, sizeof d);
    d.nodes = n;
    d.n_nodes = count;
    d.channels = g_ch;
    d.n_channels = 64;
    return d;
}

static bool open_desc(ysig_graph* g, ysig_desc* d) {
    d->arena = g_arena;
    d->arena_size = ysig_arena_size(d);
    if (!d->arena_size || d->arena_size > sizeof g_arena) {
        char err[256];
        ysig_check(d, err, sizeof err);
        fprintf(stderr, "sig_test [%s]: arena %lu: %s\n", g_where, (unsigned long)d->arena_size, err);
        return false;
    }
    if (!ysig_open(g, d)) { fprintf(stderr, "sig_test [%s]: open: %s\n", g_where, ysig_error(g)); return false; }
    return true;
}

static ysig_frame_in frame_at(int64_t index, int64_t onset, int64_t base, const float* v, int nv) {
    ysig_frame_in f;
    memset(&f, 0, sizeof f);
    f.index = index;
    f.onset_ns = onset;
    f.base_ns = base;
    f.values = v;
    f.n_values = nv;
    return f;
}

/* ---------------------------------------------------------------- conventions */

static void test_conventions(void) {
    const ysig_param* p;
    int n, op;
    char want[64];
    g_where = "conventions";
    CHECK(sizeof(ysig_value) == 64);
    CHECK(sizeof(yin_event) == 48);
    CHECK(!strcmp(ysig_version(), YSIG_VERSION_STRING));
    p = ysig_op_params(YSIG_OP_NONE, &n);
    CHECK(p && n == 1 && !strcmp(p[0].name, "op") && p[0].max == YSIG_OP_COUNT - 1);
    for (op = 1; op < YSIG_OP_COUNT; op++) {
        /* the enum's values name each op by its number */
        snprintf(want, sizeof want, ",%s=%d", ysig_op_name(op), op);
        CHECK(p && strstr(p[0].values, want) != NULL);
        (void)ysig_op_params(op, &n);
        CHECK(n >= 0 && n <= YSIG_NP);
    }
    CHECK(ysig_op_params(YSIG_OP_COUNT, &n) == NULL && n == 0);
    CHECK(ysig_op_params(-1, &n) == NULL && n == 0);
    CHECK(ysig_op_name(YSIG_OP_COUNT) == NULL);
    /* the tier order: SIM best, UNKNOWN worst */
    CHECK(ysig_tier_worst(YIN_TIER_SIM, YIN_TIER_1) == YIN_TIER_1);
    CHECK(ysig_tier_worst(YIN_TIER_2, YIN_TIER_1) == YIN_TIER_2);
    CHECK(ysig_tier_worst(YIN_TIER_3, YIN_TIER_UNKNOWN) == YIN_TIER_UNKNOWN);
    CHECK(ysig_tier_worst(YIN_TIER_SIM, YIN_TIER_SIM) == YIN_TIER_SIM);
    CHECK(ysig_tier_worst(YIN_TIER_3, YIN_TIER_SIM) == YIN_TIER_3);
}

/* ---------------------------------------------------------------- integrate */

/* The largest n with o + g n <= hi, and the smallest with o + g n >= lo,
 * by search: the closed form of the bound rule. */
static int64_t n_hi(double o, double gn, double hi) {
    int64_t n = (int64_t)floor((hi - o) / gn) + 3;
    while (o + gn * (double)n > hi) n--;
    return n;
}
static int64_t n_lo(double o, double gn, double lo) {
    int64_t n = (int64_t)ceil((lo - o) / gn) - 3;
    while (o + gn * (double)n < lo) n++;
    return n;
}

static void test_integrate(void) {
    ysig_node n[2];
    ysig_desc d;
    ysig_graph g;
    uint64_t rs = 12345;
    int i, k;
    double gn[2] = { 0.1, 0.3 }, o[2] = { 512, 384 };
    int64_t c[2] = { 0, 0 };
    g_where = "integrate counts";
    n[0] = src_node(YIN_KIND_MOUSE, YIN_AXIS_DELTA, 7, 2);
    n[1] = ysig_node_of(YSIG_OP_INTEGRATE, 0, -1);
    for (k = 0; k < 2; k++) { n[1].p[k] = gn[k]; n[1].p[4 + k] = o[k]; }
    d = desc_of(n, 2);
    if (!open_desc(&g, &d)) { g_failures++; return; }
    ysig_reset(&g, 0);
    CHECK(ysig_value_of(&g, 1)->v[0] == 512 && ysig_value_of(&g, 1)->v[1] == 384);
    /* 160 reaches of whole counts at gain 0.1 stay on the count grid: the
     * value is origin + gain x the exact sum (a float sum drifted 0.033 px) */
    for (i = 0; i < 160 * 400; i++) {
        int dx = (int)(ysig__mix(&rs) % 21) - 10, dy = (int)(ysig__mix(&rs) % 21) - 10;
        yin_event e = delta(1000 + i, 7, (float)dx, (float)dy);
        CHECK(ysig_feed(&g, &e) == 1);
        c[0] += dx;
        c[1] += dy;
    }
    for (k = 0; k < 2; k++) CHECK(ysig_value_of(&g, 1)->v[k] == o[k] + gn[k] * (double)c[k]);
    /* another device: not taken */
    {
        yin_event e = delta(200000, 8, 5, 5);
        CHECK(ysig_feed(&g, &e) == 0);
        for (k = 0; k < 2; k++) CHECK(ysig_value_of(&g, 1)->v[k] == o[k] + gn[k] * (double)c[k]);
    }
    /* NaN and a count no mouse sends are no move */
    {
        yin_event e = delta(200001, 7, NAN, 2e9f);
        CHECK(ysig_feed(&g, &e) == 1);
        for (k = 0; k < 2; k++) CHECK(ysig_value_of(&g, 1)->v[k] == o[k] + gn[k] * (double)c[k]);
    }
    /* bounds: the count sum stays inside, at the last grid point, and a
     * move back leaves the edge at once */
    g_where = "integrate bounds";
    n[1].p[8] = 0; n[1].p[12] = 1024;
    n[1].p[9] = 0; n[1].p[13] = 768;
    d = desc_of(n, 2);
    if (!open_desc(&g, &d)) { g_failures++; return; }
    ysig_reset(&g, 0);
    c[0] = c[1] = 0;
    for (i = 0; i < 20000; i++) {
        int dx = (int)(ysig__mix(&rs) % 401) - 200, dy = (int)(ysig__mix(&rs) % 401) - 200;
        yin_event e = delta(1000 + i, 7, (float)dx, (float)dy);
        ysig_feed(&g, &e);
        c[0] += dx;
        c[1] += dy;
        {
            int64_t h0 = n_hi(o[0], gn[0], 1024), l0 = n_lo(o[0], gn[0], 0);
            int64_t h1 = n_hi(o[1], gn[1], 768), l1 = n_lo(o[1], gn[1], 0);
            c[0] = c[0] > h0 ? h0 : c[0] < l0 ? l0 : c[0];
            c[1] = c[1] > h1 ? h1 : c[1] < l1 ? l1 : c[1];
        }
        if (ysig_value_of(&g, 1)->v[0] != o[0] + gn[0] * (double)c[0] ||
            ysig_value_of(&g, 1)->v[1] != o[1] + gn[1] * (double)c[1]) {
            CHECK(!"bounded count sum");
            break;
        }
    }
    /* reset: back to the origin */
    ysig_reset(&g, 50000);
    CHECK(ysig_value_of(&g, 1)->v[0] == 512 && ysig_value_of(&g, 1)->t == 50000);

    /* rate control: a joystick's deflection held, integrated over dt */
    g_where = "integrate rate";
    n[0] = src_node(YIN_KIND_GAMEPAD, YIN_AXIS_GAMEPAD, 0, 2);
    n[1] = ysig_node_of(YSIG_OP_INTEGRATE, 0, -1);
    n[1].p[0] = n[1].p[1] = 600;   /* px/s at full deflection */
    d = desc_of(n, 2);
    if (!open_desc(&g, &d)) { g_failures++; return; }
    ysig_reset(&g, 0);
    {
        yin_event e = axis(100 * MS, YIN_KIND_GAMEPAD, YIN_AXIS_GAMEPAD, 0, 0.5f, -0.25f, 0, 0);
        ysig_frame_in f;
        ysig_feed(&g, &e);
        CHECK(ysig_value_of(&g, 1)->v[0] == 0);
        /* a frame 50 ms on: the held deflection moved it, state kept */
        f = frame_at(0, 150 * MS, 0, NULL, 0);
        ysig_frame(&g, &f);
        CHECK_NEAR(ysig_value_of(&g, 1)->v[0], 600 * 0.5 * 0.05, 1e-9);
        CHECK_NEAR(ysig_value_of(&g, 1)->v[1], -600 * 0.25 * 0.05, 1e-9);
        CHECK(ysig_value_of(&g, 1)->t == 150 * MS);
        e = axis(300 * MS, YIN_KIND_GAMEPAD, YIN_AXIS_GAMEPAD, 0, 0, 0, 0, 0);
        ysig_feed(&g, &e);
        CHECK_NEAR(ysig_value_of(&g, 1)->v[0], 600 * 0.5 * 0.2, 1e-9);
        f = frame_at(1, 400 * MS, 0, NULL, 0);
        ysig_frame(&g, &f);
        CHECK_NEAR(ysig_value_of(&g, 1)->v[0], 60, 1e-9);
    }
}

/* ---------------------------------------------------------------- maps */

static void test_maps(void) {
    ysig_node n[16];
    ysig_desc d;
    ysig_graph g;
    yin_event e;
    const ysig_value* v;
    g_where = "maps";
    n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 0, 2);
    n[1] = ysig_node_of(YSIG_OP_AFFINE, 0, -1);              /* 90 deg about (10, 20) */
    n[1].p[4] = n[1].p[6] = 10; n[1].p[5] = n[1].p[7] = 20; n[1].p[8] = 90;
    n[2] = ysig_node_of(YSIG_OP_AFFINE, 0, -1);              /* the mirror */
    n[2].p[1] = n[2].p[2] = 1;
    n[3] = ysig_node_of(YSIG_OP_SCALE, 0, -1);
    n[3].p[0] = 2; n[3].p[1] = -3;
    n[4] = ysig_node_of(YSIG_OP_OFFSET, 0, -1);
    n[4].p[0] = 1; n[4].p[1] = -1;
    n[5] = ysig_node_of(YSIG_OP_CLAMP, 0, -1);
    n[5].p[0] = 0; n[5].p[4] = 4;                             /* x in [0, 4]; y free */
    n[6] = ysig_node_of(YSIG_OP_SELECT, 0, -1);
    n[6].lanes = 2; n[6].p[0] = 1; n[6].p[1] = 0;
    n[7] = ysig_node_of(YSIG_OP_JOIN, 6, 0);
    n[7].in[2] = -1;
    n[8] = ysig_node_of(YSIG_OP_SUM, 0, 3);
    n[8].p[1] = 0.5;
    n[9] = ysig_node_of(YSIG_OP_MEAN, 0, 3);
    n[10] = ysig_node_of(YSIG_OP_DIFF, 0, 3);
    n[11] = ysig_node_of(YSIG_OP_DEADZONE, 0, -1);
    n[11].p[0] = 5;
    n[12] = ysig_node_of(YSIG_OP_AFFINE, 0, -1);             /* identity: all zero */
    n[13] = ysig_node_of(YSIG_OP_SELECT, 0, -1);
    n[13].lanes = 1; n[13].p[0] = 0;
    n[14] = ysig_node_of(YSIG_OP_SCALE, 0, 13);               /* times x */
    d = desc_of(n, 15);
    if (!open_desc(&g, &d)) { g_failures++; return; }
    ysig_reset(&g, 0);
    e = axis(MS, YIN_KIND_PEN, YIN_AXIS_POSITION, 0, 13, 24, 0, YIN_TIER_SIM);
    ysig_feed(&g, &e);
    v = ysig_value_of(&g, 1);    /* (3, 4) about (10, 20) by 90 deg: (-4, 3) */
    CHECK_NEAR(v->v[0], 10 - 4, 1e-12);
    CHECK_NEAR(v->v[1], 20 + 3, 1e-12);
    v = ysig_value_of(&g, 2);
    CHECK(v->v[0] == 24 && v->v[1] == 13);
    v = ysig_value_of(&g, 3);
    CHECK(v->v[0] == 26 && v->v[1] == -72);
    v = ysig_value_of(&g, 4);
    CHECK(v->v[0] == 14 && v->v[1] == 23);
    v = ysig_value_of(&g, 5);
    CHECK(v->v[0] == 4 && v->v[1] == 24);
    v = ysig_value_of(&g, 6);
    CHECK(v->lanes == 2 && v->v[0] == 24 && v->v[1] == 13);
    v = ysig_value_of(&g, 7);
    CHECK(v->lanes == 4);
    CHECK(v->v[0] == 24 && v->v[1] == 13 && v->v[2] == 13 && v->v[3] == 24);
    v = ysig_value_of(&g, 8);        /* (13, 24) + 0.5 (26, -72) */
    CHECK(v->v[0] == 26 && v->v[1] == -12);
    v = ysig_value_of(&g, 9);
    CHECK(v->v[0] == 19.5 && v->v[1] == -24);
    v = ysig_value_of(&g, 10);
    CHECK(v->v[0] == -13 && v->v[1] == 96);
    v = ysig_value_of(&g, 11);       /* |(13, 24)| = 27.29.., shrunk by 5 */
    CHECK_NEAR(sqrt(v->v[0] * v->v[0] + v->v[1] * v->v[1]), sqrt(13.0 * 13 + 24 * 24) - 5, 1e-12);
    CHECK_NEAR(v->v[1] / v->v[0], 24.0 / 13, 1e-12);
    v = ysig_value_of(&g, 12);
    CHECK(v->v[0] == 13 && v->v[1] == 24);
    v = ysig_value_of(&g, 14);
    CHECK(v->v[0] == 169 && v->v[1] == 312);
    /* the mirror keeps the sign of zero: a skipped zero term */
    e = axis(2 * MS, YIN_KIND_PEN, YIN_AXIS_POSITION, 0, 7, -0.0f, 0, YIN_TIER_SIM);
    ysig_feed(&g, &e);
    v = ysig_value_of(&g, 2);
    CHECK(v->v[0] == 0 && signbit(v->v[0]) && v->v[1] == 7);
    /* inside the deadzone */
    e = axis(3 * MS, YIN_KIND_PEN, YIN_AXIS_POSITION, 0, 3, 4, 0, YIN_TIER_SIM);
    ysig_feed(&g, &e);
    CHECK(ysig_value_of(&g, 11)->v[0] == 0 && ysig_value_of(&g, 11)->v[1] == 0);

    /* an angle from a timeline channel: a rotation that turns on mid-reach */
    g_where = "affine angle input";
    n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 0, 2);
    n[1] = ysig_node_of(YSIG_OP_CHANNEL, -1, -1);
    n[1].p[0] = 3;
    n[2] = ysig_node_of(YSIG_OP_AFFINE, 0, 1);
    d = desc_of(n, 3);
    if (!open_desc(&g, &d)) { g_failures++; return; }
    ysig_reset(&g, 0);
    {
        float tl[4] = { 0, 0, 0, 0 };
        ysig_frame_in f = frame_at(0, 10 * MS, 0, tl, 4);
        ysig_frame(&g, &f);
        e = axis(11 * MS, YIN_KIND_PEN, YIN_AXIS_POSITION, 0, 1, 0, 0, YIN_TIER_SIM);
        ysig_feed(&g, &e);
        CHECK(ysig_value_of(&g, 2)->v[0] == 1 && ysig_value_of(&g, 2)->v[1] == 0);
        tl[3] = 90;
        f = frame_at(1, 20 * MS, 0, tl, 4);
        ysig_frame(&g, &f);    /* the affine runs at the frame: the angle changed */
        CHECK_NEAR(ysig_value_of(&g, 2)->v[0], 0, 1e-15);
        CHECK_NEAR(ysig_value_of(&g, 2)->v[1], 1, 1e-15);
        e = axis(21 * MS, YIN_KIND_PEN, YIN_AXIS_POSITION, 0, 0, 2, 0, YIN_TIER_SIM);
        ysig_feed(&g, &e);
        CHECK_NEAR(ysig_value_of(&g, 2)->v[0], -2, 1e-15);
        CHECK(ysig_value_of(&g, 2)->t == 21 * MS && ysig_value_of(&g, 2)->t_oldest == 20 * MS);
    }
}

/* ---------------------------------------------------------------- filters */

static void test_filters(void) {
    ysig_node n[8];
    ysig_desc d;
    ysig_graph g;
    yin_event e;
    int i;
    const double fc = 5, dt = 0.002;
    const double a = 1 - exp(-2 * PI_ * fc * dt), b = 1 - a;
    g_where = "lowpass";
    n[0] = src_node(YIN_KIND_BOX, 3, 0, 1);
    n[1] = ysig_node_of(YSIG_OP_LOWPASS, 0, -1);
    n[1].p[0] = fc;
    n[2] = ysig_node_of(YSIG_OP_LOWPASS, 0, -1);
    n[2].p[0] = fc; n[2].p[1] = 2;
    d = desc_of(n, 3);
    if (!open_desc(&g, &d)) { g_failures++; return; }
    ysig_reset(&g, 0);
    e = axis(0, YIN_KIND_BOX, 3, 0, 0, 0, 0, YIN_TIER_SIM);
    ysig_feed(&g, &e);    /* the first sample sets the filter */
    for (i = 1; i <= 200; i++) {
        e = axis((int64_t)i * 2 * MS, YIN_KIND_BOX, 3, 0, 0, 0, 1, YIN_TIER_SIM);
        ysig_feed(&g, &e);
        CHECK_NEAR(ysig_value_of(&g, 1)->v[0], 1 - pow(b, i), 1e-12);
        CHECK_NEAR(ysig_value_of(&g, 2)->v[0], 1 - pow(b, i) * (1 + i * a), 1e-12);
    }
    /* a frame 100 ms after the last sample: the held input's response,
     * state unchanged */
    {
        double y = ysig_value_of(&g, 1)->v[0];
        ysig_frame_in f = frame_at(0, 500 * MS, 0, NULL, 0);
        ysig_frame(&g, &f);
        CHECK_NEAR(ysig_value_of(&g, 1)->v[0], 1 - (1 - y) * exp(-2 * PI_ * fc * 0.1), 1e-12);
        e = axis(402 * MS, YIN_KIND_BOX, 3, 0, 0, 0, 1, YIN_TIER_SIM);
        ysig_feed(&g, &e);
        CHECK_NEAR(ysig_value_of(&g, 1)->v[0], 1 - pow(b, 201), 1e-12);
    }

    g_where = "velocity, speed, delay";
    n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 0, 2);
    n[1] = ysig_node_of(YSIG_OP_VELOCITY, 0, -1);
    n[1].p[0] = 0.010;
    n[2] = ysig_node_of(YSIG_OP_SPEED, 0, -1);
    n[2].p[0] = 0.010;
    n[3] = ysig_node_of(YSIG_OP_DELAY, 0, -1);
    n[3].p[0] = 0.025;
    d = desc_of(n, 4);
    if (!open_desc(&g, &d)) { g_failures++; return; }
    ysig_reset(&g, 0);
    for (i = 0; i <= 100; i++) {
        /* 300 px/s right, 400 px/s up, one sample per ms */
        e = axis((int64_t)i * MS, YIN_KIND_PEN, YIN_AXIS_POSITION, 0, (float)(0.3 * i), (float)(-0.4 * i), 0, YIN_TIER_SIM);
        ysig_feed(&g, &e);
        if (i >= 10) {
            CHECK_NEAR(ysig_value_of(&g, 1)->v[0], 300, 1e-3);
            CHECK_NEAR(ysig_value_of(&g, 1)->v[1], -400, 1e-3);
            CHECK_NEAR(ysig_value_of(&g, 2)->v[0], 500, 1e-3);
        } else {
            CHECK(ysig_value_of(&g, 2)->v[0] == 0);
        }
        if (i >= 25) CHECK(ysig_value_of(&g, 3)->v[0] == (double)(float)(0.3 * (i - 25)));
        else CHECK(ysig_value_of(&g, 3)->v[0] == 0);
    }
    /* a still hand sends nothing: at a frame the speed falls to 0 */
    {
        ysig_frame_in f = frame_at(0, 105 * MS, 0, NULL, 0);
        ysig_frame(&g, &f);
        /* from the sample of 95 ms over 10 ms: |(1.5, -2)| / 0.01 */
        CHECK_NEAR(ysig_value_of(&g, 2)->v[0], 250, 1e-3);
        f = frame_at(1, 111 * MS, 0, NULL, 0);
        ysig_frame(&g, &f);
        CHECK(ysig_value_of(&g, 2)->v[0] == 0);
        CHECK(ysig_value_of(&g, 3)->v[0] == (double)(float)(0.3 * 86));
    }
}

/* ---------------------------------------------------------------- generators */

static void test_generators(void) {
    static const double comp[2 * 2 * 3] = { 2.0, 0.5, 0.3, 1.0, 1.25, -1.0,     /* x: a f phi, a f phi */
                                            1.5, 0.75, 2.0, 0.5, 2.5, 0.0 };    /* y */
    ysig_node n[8];
    ysig_desc d;
    ysig_graph g, g2;
    int i, k;
    g_where = "sines";
    n[0] = ysig_node_of(YSIG_OP_SINES, -1, -1);
    n[0].lanes = 2; n[0].p[0] = 2; n[0].p[1] = 5;
    n[0].data = comp; n[0].n_data = 12;
    n[1] = ysig_node_of(YSIG_OP_CHANNEL, -1, -1);
    n[1].p[0] = 1;
    n[2] = ysig_node_of(YSIG_OP_SCALE, 0, 1);
    n[3] = ysig_node_of(YSIG_OP_BIND, 2, -1);
    n[3].p[0] = 10;
    n[4] = ysig_node_of(YSIG_OP_NOISE, -1, -1);
    n[4].lanes = 1; n[4].p[0] = 99; n[4].p[1] = 2;
    d = desc_of(n, 5);
    if (!open_desc(&g, &d)) { g_failures++; return; }
    ysig_reset(&g, 0);
    for (i = 0; i < 600; i++) {
        float tl[2] = { 0, 0.5f };
        int64_t base = (int64_t)i * 16666667;
        double t = (double)base / 1e9, A = t < 5 ? t / 5 : 1;
        ysig_frame_in f = frame_at(i, 1000 * MS + base, base, tl, 2);
        ysig_frame(&g, &f);
        for (k = 0; k < 2; k++) {
            const double* q = comp + 6 * k;
            double s = q[0] * cos(2 * PI_ * q[1] * t + q[2]) + q[3] * cos(2 * PI_ * q[4] * t + q[5]);
            CHECK_NEAR(ysig_value_of(&g, 0)->v[k], A * s, 1e-12);
            CHECK(g_ch[10 + k] == (float)(ysig_value_of(&g, 0)->v[k] * 0.5));
        }
        CHECK(ysig_value_of(&g, 0)->t == 1000 * MS + base);
    }
    /* noise: the same numbers after a reset and in another graph */
    g_where = "noise";
    {
        double first[50], s = 0, s2 = 0;
        ysig_desc d2 = desc_of(n, 5);
        static uint64_t arena2[(256u << 10) / 8];
        ysig_reset(&g, 0);
        for (i = 0; i < 50; i++) {
            ysig_frame_in f = frame_at(i, i * MS, 0, NULL, 0);
            ysig_frame(&g, &f);
            first[i] = ysig_value_of(&g, 4)->v[0];
        }
        d2.arena = arena2;
        d2.arena_size = sizeof arena2;
        CHECK(ysig_open(&g2, &d2));
        ysig_reset(&g2, 0);
        for (i = 0; i < 20000; i++) {
            ysig_frame_in f = frame_at(i, i * MS, 0, NULL, 0);
            double z;
            ysig_frame(&g2, &f);
            z = ysig_value_of(&g2, 4)->v[0];
            if (i < 50) CHECK(z == first[i]);
            s += z;
            s2 += z * z;
        }
        s /= 20000;
        CHECK(fabs(s) < 5 * 2 / sqrt(20000.0));
        CHECK_NEAR(sqrt(s2 / 20000 - s * s), 2, 0.05);
        ysig_set(&g2, 4, 0, 100);
        ysig_reset(&g2, 0);
        {
            ysig_frame_in f = frame_at(0, 0, 0, NULL, 0);
            ysig_frame(&g2, &f);
            CHECK(ysig_value_of(&g2, 4)->v[0] != first[0]);
        }
    }
}

/* ---------------------------------------------------------------- time */

static int g_rows;
static ysig_row g_row[256];
static void on_row(void* user, const ysig_row* r) {
    (void)user;
    if (g_rows < 256) g_row[g_rows] = *r;
    g_rows++;
}

static void test_time(void) {
    ysig_node n[8];
    ysig_desc d;
    ysig_graph g;
    yin_event e, der[8];
    ysig_stats st;
    const ysig_value* v;
    g_where = "tier and t_oldest";
    n[0] = src_node(YIN_KIND_MOUSE, YIN_AXIS_DELTA, 1, 2);        /* left */
    n[0].p[4] = YIN_TIER_2;                                         /* the table's tier */
    n[1] = src_node(YIN_KIND_MOUSE, YIN_AXIS_DELTA, 2, 2);        /* right */
    n[2] = ysig_node_of(YSIG_OP_INTEGRATE, 0, -1);
    n[3] = ysig_node_of(YSIG_OP_INTEGRATE, 1, -1);
    n[4] = ysig_node_of(YSIG_OP_MEAN, 2, 3);
    n[5] = ysig_node_of(YSIG_OP_EMIT, 4, -1);
    n[5].p[0] = 7;
    n[6] = ysig_node_of(YSIG_OP_RECORD, 2, -1);                    /* left only */
    n[6].name = "left";
    d = desc_of(n, 7);
    d.on_row = on_row;
    if (!open_desc(&g, &d)) { g_failures++; return; }
    ysig_reset(&g, 0);
    g_rows = 0;
    e = delta(10 * MS, 1, 4, 0);
    e.stamp = 0;                       /* the table's: tier 2 */
    ysig_feed(&g, &e);
    e = delta(12 * MS, 2, 0, 6);
    e.stamp = YIN_TIER_3;
    ysig_feed(&g, &e);
    v = ysig_value_of(&g, 4);
    CHECK(v->v[0] == 2 && v->v[1] == 3);
    CHECK(v->t == 12 * MS && v->t_oldest == 10 * MS);
    CHECK(v->tier == YIN_TIER_3 && v->src == 1 && v->src_oldest == 0 && v->device == 2);
    CHECK(ysig_derived(&g, der, 8) == 2);
    CHECK(der[0].kind == YIN_KIND_USER + 7 && der[0].device == 5 && der[0].type == YIN_SAMPLE);
    CHECK(der[0].t == 10 * MS && der[0].stamp == YIN_TIER_2 && der[0].x == 2 && der[0].y == 0);
    CHECK(der[1].t == 12 * MS && der[1].stamp == YIN_TIER_3 && der[1].code == (1u | (0u << 16)));
    /* the right hand's sample ran nothing of the left's: one row */
    CHECK(g_rows == 1 && g_row[0].node == 6 && !strcmp(g_row[0].name, "left") && g_row[0].value.t == 10 * MS);
    /* an older sample of one source is dropped and counted; equal t passes */
    g_where = "out of order";
    e = delta(9 * MS, 1, 100, 0);
    CHECK(ysig_feed(&g, &e) == 0);
    e = delta(10 * MS, 1, 2, 0);
    CHECK(ysig_feed(&g, &e) == 1);
    ysig_get_stats(&g, &st);
    CHECK(st.dropped_order == 1 && st.taken == 3 && st.fed == 4);
    CHECK(ysig_value_of(&g, 2)->v[0] == 6);
    /* another source's older time is no drop: sources differ (ysp/input.h) */
    e = delta(11 * MS, 2, 0, 2);
    CHECK(ysig_feed(&g, &e) == 0);
    e = delta(13 * MS, 2, 0, 2);
    CHECK(ysig_feed(&g, &e) == 1);
    CHECK(ysig_value_of(&g, 4)->t_oldest == 10 * MS);
    /* the derived queue: 64, the rest counted */
    {
        int i;
        ysig_derived(&g, der, 8);
        while (ysig_derived(&g, der, 8) > 0) { }
        for (i = 0; i < 70; i++) {
            e = delta(20 * MS + i, 1, 1, 0);
            ysig_feed(&g, &e);
        }
        ysig_get_stats(&g, &st);
        CHECK(st.derived_lost == 6);
        CHECK(ysig_derived(&g, der, 8) == 8);
        ysig_reset(&g, 0);
        CHECK(ysig_derived(&g, der, 8) == 0);
    }
}

/* ---------------------------------------------------------------- detectors */

static void test_detect(void) {
    ysig_node n[8];
    ysig_desc d;
    ysig_graph g;
    ysig_hit h[8];
    yin_event e;
    int i;
    g_where = "detect crossing";
    n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 0, 2);
    n[1] = ysig_node_of(YSIG_OP_DETECT, 0, -1);           /* |p - (10, 0)| >= 5, every, hysteresis 1 */
    n[1].p[0] = YSIG_TEST_GE; n[1].p[1] = -1; n[1].p[2] = 5; n[1].p[3] = 1; n[1].p[7] = 1;
    n[1].p[8] = 10;
    n[2] = ysig_node_of(YSIG_OP_DETECT, 0, -1);           /* x <= 0, once, armed by arm() */
    n[2].p[0] = YSIG_TEST_LE; n[2].p[1] = 0; n[2].p[2] = 0; n[2].p[6] = 1;
    n[3] = ysig_node_of(YSIG_OP_DETECT, 0, -1);           /* inside [0,4]x[0,4] for 0.5 s */
    n[3].p[0] = YSIG_TEST_IN_RECT; n[3].p[4] = 0.5; n[3].p[10] = 4; n[3].p[11] = 4;
    n[4] = ysig_node_of(YSIG_OP_DETECT, 0, -1);           /* outside, at frame rate */
    n[4].p[0] = YSIG_TEST_OUT_RECT; n[4].p[5] = 1; n[4].p[7] = 1; n[4].p[10] = 4; n[4].p[11] = 4;
    d = desc_of(n, 5);
    if (!open_desc(&g, &d)) { g_failures++; return; }
    ysig_reset(&g, 0);
    /* (0, 0) at reset: 10 from (10, 0), past 5: held, no hit */
    {
        static const float xs[] = { 12, 9, 15, 14.5f, 13.9f, 16, 4 };
        static const int hit[] = { 0, 0, 1, 0, 0, 1, 0 };
        /* 12: |2| < 5, ready; 15: hit; 14.5: no (still past); 13.9: below
         * 5 - 1? no, 3.9 < 4: ready; 16: hit; 4: |6| hit? not ready yet */
        for (i = 0; i < 7; i++) {
            int got;
            e = axis((i + 1) * 10 * MS, YIN_KIND_PEN, YIN_AXIS_POSITION, 0, xs[i], 0, 0, YIN_TIER_SIM);
            ysig_feed(&g, &e);
            got = ysig_detected(&g, h, 8);
            if (hit[i]) CHECK(got >= 1 && h[0].node == 1 && h[0].t == e.t && h[0].value.v[0] == xs[i]);
            else CHECK(got == 0 || h[0].node != 1);
        }
    }
    /* arm at t: a value past the test at arm is held; then crossing once */
    g_where = "detect arm";
    ysig_arm(&g, 2, 100 * MS);
    e = axis(110 * MS, YIN_KIND_PEN, YIN_AXIS_POSITION, 0, -1, 0, 0, YIN_TIER_SIM);
    ysig_feed(&g, &e);
    ysig_detected(&g, h, 8);
    CHECK(h[0].node == 2 && h[0].t == 110 * MS);
    e = axis(120 * MS, YIN_KIND_PEN, YIN_AXIS_POSITION, 0, 1, 0, 0, YIN_TIER_SIM);
    ysig_feed(&g, &e);
    e = axis(130 * MS, YIN_KIND_PEN, YIN_AXIS_POSITION, 0, -1, 0, 0, YIN_TIER_SIM);
    ysig_feed(&g, &e);
    CHECK(ysig_detected(&g, h, 8) == 0);   /* once: disarmed after the hit */
    ysig_arm(&g, 2, 140 * MS);              /* armed past the test: held */
    e = axis(150 * MS, YIN_KIND_PEN, YIN_AXIS_POSITION, 0, -2, 0, 0, YIN_TIER_SIM);
    ysig_feed(&g, &e);
    CHECK(ysig_detected(&g, h, 8) == 0);
    CHECK(ysig_value_of(&g, 2)->v[0] == 1);  /* the test's state */

    /* a hold: completed by a later sample (before it is tested) and by a
     * frame (a still hand sends nothing) */
    g_where = "detect hold";
    ysig_reset(&g, 0);
    e = axis(1000 * MS, YIN_KIND_PEN, YIN_AXIS_POSITION, 0, 9, 9, 0, YIN_TIER_SIM);
    ysig_feed(&g, &e);                                     /* outside; the reset's hold ended at 0.5 s */
    {
        int got = ysig_detected(&g, h, 8), found = 0;
        for (i = 0; i < got; i++)
            if (h[i].node == 3) { found = 1; CHECK(h[i].t == 500 * MS && h[i].t_found == 1000 * MS); }
        CHECK(found);
    }
    ysig_arm(&g, 3, 1000 * MS);
    e = axis(1100 * MS, YIN_KIND_PEN, YIN_AXIS_POSITION, 0, 2, 2, 0, YIN_TIER_SIM);
    ysig_feed(&g, &e);                                     /* inside from 1.1 s */
    e = axis(1700 * MS, YIN_KIND_PEN, YIN_AXIS_POSITION, 0, 9, 9, 0, YIN_TIER_SIM);
    ysig_feed(&g, &e);                                     /* out again, but the hold ended at 1.6 s */
    {
        int got = ysig_detected(&g, h, 8), found = 0;
        for (i = 0; i < got; i++)
            if (h[i].node == 3) { found = 1; CHECK(h[i].t == 1600 * MS && h[i].t_found == 1700 * MS); }
        CHECK(found);
    }
    ysig_reset(&g, 2000 * MS);   /* at (0, 0): inside at arm, the hold counts from the reset */
    {
        ysig_frame_in f = frame_at(0, 2400 * MS, 0, NULL, 0);
        int got, found = 0;
        ysig_frame(&g, &f);
        CHECK(ysig_detected(&g, h, 8) == 0);
        f = frame_at(1, 2516 * MS, 0, NULL, 0);
        ysig_frame(&g, &f);
        got = ysig_detected(&g, h, 8);
        for (i = 0; i < got; i++)
            if (h[i].node == 3) { found = 1; CHECK(h[i].t == 2500 * MS && h[i].t_found == 2516 * MS); }
        CHECK(found);
    }
    /* frame-rate test: the held value at each onset, crossing rule */
    g_where = "detect at frames";
    ysig_reset(&g, 0);
    ysig_detected(&g, h, 8);
    {
        ysig_frame_in f;
        int got, found = 0;
        e = axis(5 * MS, YIN_KIND_PEN, YIN_AXIS_POSITION, 0, 6, 1, 0, YIN_TIER_SIM);
        ysig_feed(&g, &e);
        ysig_detected(&g, h, 8);   /* none for node 4: frame rate */
        f = frame_at(0, 16 * MS, 0, NULL, 0);
        ysig_frame(&g, &f);
        got = ysig_detected(&g, h, 8);
        for (i = 0; i < got; i++)
            if (h[i].node == 4) { found = 1; CHECK(h[i].t == 16 * MS && h[i].value.t == 16 * MS); }
        CHECK(found);
        CHECK(ysig_value_of(&g, 4)->v[0] == 1);
    }
    /* the hit queue: 64, the rest counted */
    g_where = "hit queue";
    {
        ysig_stats st;
        ysig_reset(&g, 0);
        for (i = 0; i < 80; i++) {
            e = axis(10 * MS + i * 2, YIN_KIND_PEN, YIN_AXIS_POSITION, 0, 20, 0, 0, YIN_TIER_SIM);
            ysig_feed(&g, &e);
            e = axis(10 * MS + i * 2 + 1, YIN_KIND_PEN, YIN_AXIS_POSITION, 0, 10, 0, 0, YIN_TIER_SIM);
            ysig_feed(&g, &e);
        }
        ysig_get_stats(&g, &st);
        CHECK(st.hits_lost > 0 && g.n_hit == YSIG_QUEUE_CAP);
    }
}

/* ---------------------------------------------------------------- replay */

#define J_CAP 8192
static ysig_entry g_j[J_CAP];
static float g_jv[J_CAP][4];
static int g_nj;
static void on_journal(void* user, const ysig_entry* e) {
    (void)user;
    if (g_nj >= J_CAP) return;
    g_j[g_nj] = *e;
    if (e->kind == YSIG_J_FRAME && e->frame.n_values) {
        int k;
        for (k = 0; k < e->frame.n_values && k < 4; k++) g_jv[g_nj][k] = e->frame.values[k];
        g_j[g_nj].frame.values = g_jv[g_nj];
    }
    g_nj++;
}

static void test_replay(void) {
    static const double comp[6] = { 1.5, 0.4, 0.1, 0.7, 1.1, 2.0 };
    ysig_node n[16];
    ysig_desc d;
    ysig_graph g, g2;
    static uint64_t arena2[(1u << 20) / 8];
    ysig_stats s1, s2;
    ysig_hit h[64];
    yin_event der[64];
    uint64_t rs = 7;
    int i, k, nn = 0;
    g_where = "replay";
    n[nn++] = src_node(YIN_KIND_MOUSE, YIN_AXIS_DELTA, 1, 2);          /* 0 */
    n[nn++] = src_node(YIN_KIND_MOUSE, YIN_AXIS_DELTA, 2, 2);          /* 1 */
    n[nn] = ysig_node_of(YSIG_OP_INTEGRATE, 0, -1); n[nn].p[0] = n[nn].p[1] = 0.1; nn++;   /* 2 */
    n[nn] = ysig_node_of(YSIG_OP_INTEGRATE, 1, -1); n[nn].p[0] = n[nn].p[1] = 0.3; nn++;   /* 3 */
    n[nn] = ysig_node_of(YSIG_OP_MEAN, 2, 3); nn++;                                      /* 4 */
    n[nn] = ysig_node_of(YSIG_OP_LOWPASS, 4, -1); n[nn].p[0] = 8; n[nn].p[1] = 2; nn++;  /* 5 */
    n[nn] = ysig_node_of(YSIG_OP_SINES, -1, -1); n[nn].lanes = 1; n[nn].p[0] = 2; n[nn].data = comp; n[nn].n_data = 6; nn++; /* 6 */
    n[nn] = ysig_node_of(YSIG_OP_CHANNEL, -1, -1); nn++;                                 /* 7 */
    n[nn] = ysig_node_of(YSIG_OP_AFFINE, 5, 7); nn++;                                    /* 8 */
    n[nn] = ysig_node_of(YSIG_OP_SPEED, 8, -1); n[nn].p[0] = 0.01; nn++;                 /* 9 */
    n[nn] = ysig_node_of(YSIG_OP_DETECT, 9, -1); n[nn].p[2] = 20; n[nn].p[7] = 1; nn++;  /* 10 */
    n[nn] = ysig_node_of(YSIG_OP_RECORD, 8, -1); n[nn].name = "cursor"; nn++;           /* 11 */
    n[nn] = ysig_node_of(YSIG_OP_RECORD, 6, -1); n[nn].p[0] = 1; nn++;                  /* 12 */
    n[nn] = ysig_node_of(YSIG_OP_EMIT, 8, -1); nn++;                                    /* 13 */
    n[nn] = ysig_node_of(YSIG_OP_NOISE, -1, -1); n[nn].p[0] = 5; n[nn].p[2] = 3; nn++;  /* 14 */
    n[nn] = ysig_node_of(YSIG_OP_RECORD, 14, -1); n[nn].p[0] = 1; nn++;                 /* 15 */
    d = desc_of(n, nn);
    d.journal = on_journal;
    if (!open_desc(&g, &d)) { g_failures++; return; }
    g_nj = 0;
    ysig_reset(&g, 0);
    for (i = 0; i < 300; i++) {
        float tl[1];
        ysig_frame_in f;
        for (k = 0; k < 16; k++) {
            int dev = 1 + (int)(ysig__mix(&rs) % 2);
            yin_event e = delta((int64_t)i * 16 * MS + k * MS + (dev == 2 ? 500000 : 0), dev,
                                (float)((int)(ysig__mix(&rs) % 9) - 4), (float)((int)(ysig__mix(&rs) % 9) - 4));
            ysig_feed(&g, &e);
        }
        tl[0] = (float)(i * 0.3);
        f = frame_at(i, (int64_t)(i + 1) * 16 * MS, (int64_t)i * 16 * MS, tl, 1);
        ysig_frame(&g, &f);
        if (i == 100) ysig_set(&g, 5, 0, 4);
        if (i == 150) ysig_reset(&g, f.onset_ns);
        if (i == 200) ysig_arm(&g, 10, f.onset_ns);
        while (ysig_detected(&g, h, 64) > 0) { }
        while (ysig_derived(&g, der, 64) > 0) { }
    }
    CHECK(g_nj < J_CAP);
    ysig_get_stats(&g, &s1);
    d.journal = NULL;
    d.arena = arena2;
    d.arena_size = sizeof arena2;
    CHECK(ysig_open(&g2, &d));
    CHECK(ysig_replay(&g2, g_j, g_nj) == g_nj);
    ysig_get_stats(&g2, &s2);
    CHECK(s1.digest == s2.digest && s1.rows == s2.rows && s1.hits == s2.hits && s1.rows > 600);
    for (i = 0; i < nn; i++) CHECK(!memcmp(ysig_value_of(&g, i), ysig_value_of(&g2, i), sizeof(ysig_value)));
    /* a journal entry of no kind stops the replay */
    {
        ysig_entry bad;
        memset(&bad, 0, sizeof bad);
        CHECK(ysig_replay(&g2, &bad, 1) == -1);
    }
}

/* ---------------------------------------------------------------- history */

static void test_history(void) {
    ysig_node n[8];
    ysig_desc d;
    ysig_graph g;
    ysig_value v;
    yin_event e;
    int i;
    g_where = "value at";
    n[0] = src_node(YIN_KIND_MOUSE, YIN_AXIS_DELTA, 1, 2);
    n[1] = src_node(YIN_KIND_MOUSE, YIN_AXIS_DELTA, 2, 2);
    n[2] = ysig_node_of(YSIG_OP_INTEGRATE, 0, -1);
    n[2].history = 8;
    n[3] = ysig_node_of(YSIG_OP_INTEGRATE, 1, -1);
    n[4] = ysig_node_of(YSIG_OP_JOIN, 2, 3);
    n[5] = ysig_node_of(YSIG_OP_AFFINE, 2, -1);   /* the left hand alone: one source */
    n[6] = ysig_node_of(YSIG_OP_MEAN, 5, 3);
    n[6].history = 64;   /* after two sources: computed from 5 and 3 at t */
    d = desc_of(n, 7);
    if (!open_desc(&g, &d)) { g_failures++; return; }
    ysig_reset(&g, 0);
    CHECK(ysig_value_at(&g, 6, 0, &v) == 1 && v.v[0] == 0);
    CHECK(ysig_value_at(&g, 4, 0, &v) == -1);   /* no history */
    for (i = 1; i <= 20; i++) {
        e = delta(i * MS, 1, 1, 0);
        ysig_feed(&g, &e);
        e = delta(i * MS + 500000, 2, 0, 1);
        ysig_feed(&g, &e);
    }
    /* left at 15.7 ms: its sample of 15 ms (15 counts); right of 12.5 ms */
    CHECK(ysig_value_at(&g, 2, 15 * MS + 700000, &v) == 1 && v.v[0] == 15 && v.t == 15 * MS);
    CHECK(ysig_value_at(&g, 6, 12 * MS + 700000, &v) == 1 && v.v[0] == 6 && v.v[1] == 6);
    CHECK(v.t == 12 * MS + 500000 && v.t_oldest == 12 * MS);
    CHECK(ysig_value_at(&g, 6, 12 * MS + 200000, &v) == 1 && v.v[1] == 5.5);
    /* older than the left's 8 entries: its oldest, and 0 */
    CHECK(ysig_value_at(&g, 2, 5 * MS, &v) == 0 && v.v[0] == 13);
    CHECK(ysig_value_at(&g, 6, -1, &v) == 0);
    CHECK(ysig_value_at(&g, 99, 5 * MS, &v) == -1);
}

/* ---------------------------------------------------------------- roles */

/* A custom op: a mass on a damped spring driven by its input (state in the
 * arena), and a custom generator (a counter). */
typedef struct spring_st { double x, v; int64_t t; int init; } spring_st;
static const ysig_param spring_params[] = {
    { "k", "f64", 0, 1e6, 0, "1/s2", "stiffness", NULL },
    { "c", "f64", 0, 1e6, 0, "1/s", "damping", NULL },
};
static void spring_reset(void* st, const double* p, int64_t t) { spring_st* s = (spring_st*)st; (void)p; memset(s, 0, sizeof *s); s->t = t; }
static void spring_sample(void* st, const double* p, const ysig_value* in, int n_in, ysig_value* out) {
    spring_st* s = (spring_st*)st;
    double dt = s->init ? (double)(in[0].t - s->t) / 1e9 : 0;
    (void)n_in;
    s->v += dt * (p[0] * (in[0].v[0] - s->x) - p[1] * s->v);
    s->x += dt * s->v;
    s->t = in[0].t;
    s->init = 1;
    out->v[0] = s->x;
}
static const ysig_op_vtable spring_vt = { YSIG_OP_ABI, "spring", { 1, 0, 0, 0 }, 1, sizeof(spring_st),
                                          spring_params, 2, spring_reset, spring_sample, NULL };
static void count_frame(void* st, const double* p, int64_t onset, ysig_value* out) {
    double* c = (double*)st;
    (void)p; (void)onset;
    *c += 1;
    out->v[0] = *c;
}
static const ysig_op_vtable count_vt = { YSIG_OP_ABI, "count", { 0, 0, 0, 0 }, 1, sizeof(double), NULL, 0, NULL, NULL,
                                         count_frame };

static void test_roles(void) {
    ysig_node exp_n[4], mouse[2], pen[2];
    ysig_role role;
    ysig_adapter ad;
    ysig_desc d;
    ysig_graph g;
    static ysig_value out_m[64], out_p[64];
    int i, k;
    g_where = "roles";
    exp_n[0] = ysig_node_of(YSIG_OP_ROLE, -1, -1);
    exp_n[0].p[0] = 0;
    exp_n[1] = ysig_node_of(YSIG_OP_AFFINE, 0, -1);
    exp_n[1].p[4] = exp_n[1].p[6] = 512; exp_n[1].p[5] = exp_n[1].p[7] = 384; exp_n[1].p[8] = -30;
    exp_n[2] = ysig_node_of(YSIG_OP_DETECT, 0, -1);
    exp_n[2].p[1] = -1; exp_n[2].p[2] = 20; exp_n[2].p[8] = 512; exp_n[2].p[9] = 384;
    memset(&role, 0, sizeof role);
    role.name = "hand";
    role.type = YSIG_TYPE_POSITION2D;
    role.units = YSIG_UNITS_DISPLAY_PX;
    /* the mouse: counts integrated from the start */
    mouse[0] = src_node(YIN_KIND_MOUSE, YIN_AXIS_DELTA, 0, 2);
    mouse[1] = ysig_node_of(YSIG_OP_INTEGRATE, 0, -1);
    mouse[1].p[0] = mouse[1].p[1] = 0.25; mouse[1].p[4] = 512; mouse[1].p[5] = 384;
    /* the tablet: an absolute position */
    pen[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 0, 2);
    pen[1] = ysig_node_of(YSIG_OP_OFFSET, 0, -1);
    memset(&ad, 0, sizeof ad);
    ad.role = "hand";
    ad.name = "raw mouse";
    ad.type = YSIG_TYPE_POSITION2D_RELATIVE;
    ad.units = YSIG_UNITS_DISPLAY_PX;
    ad.nodes = mouse;
    ad.n_nodes = 2;
    d = desc_of(exp_n, 3);
    d.roles = &role;
    d.n_roles = 1;
    d.adapters = &ad;
    d.n_adapters = 1;
    /* relative under position2d: refused without allow_relative */
    {
        char err[256];
        CHECK(!ysig_check(&d, err, sizeof err) && strstr(err, "allow_relative"));
        role.allow_relative = true;
        CHECK(ysig_check(&d, err, sizeof err));
        ad.units = YSIG_UNITS_WORKSPACE_CM;
        CHECK(!ysig_check(&d, err, sizeof err) && strstr(err, "units"));
        ad.units = YSIG_UNITS_DISPLAY_PX;
        ad.type = YSIG_TYPE_SCALAR;
        CHECK(!ysig_check(&d, err, sizeof err) && strstr(err, "type"));
        ad.type = YSIG_TYPE_POSITION2D_RELATIVE;
    }
    for (k = 0; k < 2; k++) {
        if (k == 1) { ad.nodes = pen; ad.type = YSIG_TYPE_POSITION2D; ad.name = "tablet"; }
        if (!open_desc(&g, &d)) { g_failures++; return; }
        CHECK(ysig_adapter_node(&g, 0, 0) == 3 && ysig_adapter_node(&g, 0, 1) == 4 && ysig_adapter_node(&g, 0, 2) == -1);
        ysig_reset(&g, 0);
        if (k == 1) {
            /* a pen starts where it is; place it on the start */
            yin_event e = axis(0, YIN_KIND_PEN, YIN_AXIS_POSITION, 0, 512, 384, 0, YIN_TIER_SIM);
            ysig_feed(&g, &e);
        }
        for (i = 1; i <= 64; i++) {
            int64_t cx = i * 3, cy = -i * 2;   /* the scripted hand, in counts */
            yin_event e = k == 0 ? delta(i * MS, 9, 3, -2)
                                 : axis(i * MS, YIN_KIND_PEN, YIN_AXIS_POSITION, 0, (float)(512 + 0.25 * (double)cx),
                                        (float)(384 + 0.25 * (double)cy), 0, YIN_TIER_SIM);
            ysig_feed(&g, &e);
            if (k) out_p[i - 1] = *ysig_value_of(&g, 1);
            else out_m[i - 1] = *ysig_value_of(&g, 1);
        }
    }
    /* the same task graph gives the same cursor from either device */
    for (i = 0; i < 64; i++)
        CHECK(out_m[i].v[0] == out_p[i].v[0] && out_m[i].v[1] == out_p[i].v[1] && out_m[i].t == out_p[i].t);
    /* refusals around roles */
    {
        char err[256];
        ysig_adapter two[2];
        two[0] = ad;
        two[1] = ad;
        d.adapters = two;
        d.n_adapters = 2;
        CHECK(!ysig_check(&d, err, sizeof err) && strstr(err, "two adapters"));
        two[1].role = "eye";
        CHECK(!ysig_check(&d, err, sizeof err) && strstr(err, "no declared role"));
        d.n_adapters = 0;
        CHECK(!ysig_check(&d, err, sizeof err) && strstr(err, "no adapter"));
        d.adapters = &ad;
        d.n_adapters = 1;
        pen[1] = ysig_node_of(YSIG_OP_ROLE, 0, -1);
        pen[1].in[0] = -1;
        CHECK(!ysig_check(&d, err, sizeof err) && strstr(err, "inside an adapter"));
        pen[1] = ysig_node_of(YSIG_OP_SELECT, 0, -1);
        pen[1].lanes = 1;
        CHECK(!ysig_check(&d, err, sizeof err) && strstr(err, "lanes"));
    }

    /* custom ops: a spring after the pen's x, a counter */
    g_where = "custom op";
    {
        ysig_node n[5];
        const ysig_op_vtable* vts[2];
        int64_t t;
        double x = 0, v = 0;
        vts[0] = &spring_vt;
        vts[1] = &count_vt;
        n[0] = src_node(YIN_KIND_BOX, 1, 0, 1);
        n[1] = ysig_node_of(YSIG_OP_CUSTOM + 0, 0, -1);
        n[1].p[0] = 400; n[1].p[1] = 10;
        n[2] = ysig_node_of(YSIG_OP_CUSTOM + 1, -1, -1);
        n[2].lanes = 1;
        d = desc_of(n, 3);
        d.custom = vts;
        d.n_custom = 2;
        if (!open_desc(&g, &d)) { g_failures++; return; }
        ysig_reset(&g, 0);
        for (t = 0; t <= 200; t++) {
            yin_event e = axis(t * MS, YIN_KIND_BOX, 1, 0, 0, 0, 1, YIN_TIER_SIM);
            double dt = t ? 0.001 : 0;
            ysig_feed(&g, &e);
            v += dt * (400 * (1 - x) - 10 * v);
            x += dt * v;
            CHECK(ysig_value_of(&g, 1)->v[0] == x);
        }
        for (i = 0; i < 5; i++) {
            ysig_frame_in f = frame_at(i, 300 * MS + i, 0, NULL, 0);
            ysig_frame(&g, &f);
        }
        CHECK(ysig_value_of(&g, 2)->v[0] == 5 && ysig_value_of(&g, 2)->t == 300 * MS + 4);
        /* a wrong ABI is refused at check */
        {
            ysig_op_vtable bad = spring_vt;
            char err[256];
            bad.version = 99;
            vts[0] = &bad;
            CHECK(!ysig_check(&d, err, sizeof err) && strstr(err, "ABI"));
            vts[0] = &spring_vt;
            n[1].p[0] = -1;   /* outside its table */
            CHECK(!ysig_check(&d, err, sizeof err) && strstr(err, "out of"));
        }
    }
}

/* ---------------------------------------------------------------- set */

static void test_set(void) {
    ysig_node n[6];
    ysig_desc d;
    ysig_graph g;
    yin_event e;
    g_where = "set";
    n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 0, 2);
    n[1] = ysig_node_of(YSIG_OP_AFFINE, 0, -1);
    n[2] = ysig_node_of(YSIG_OP_VELOCITY, 0, -1);
    n[2].p[0] = 0.01;
    n[3] = ysig_node_of(YSIG_OP_BIND, 1, -1);
    n[3].p[0] = 60;
    n[4] = ysig_node_of(YSIG_OP_RECORD, 1, -1);
    d = desc_of(n, 5);
    if (!open_desc(&g, &d)) { g_failures++; return; }
    ysig_reset(&g, 0);
    e = axis(MS, YIN_KIND_PEN, YIN_AXIS_POSITION, 0, 2, 0, 0, YIN_TIER_SIM);
    ysig_feed(&g, &e);
    CHECK(ysig_value_of(&g, 1)->v[0] == 2);
    CHECK(ysig_set(&g, 1, 8, 90));              /* at once, state kept */
    e.t = 2 * MS;
    ysig_feed(&g, &e);
    CHECK_NEAR(ysig_value_of(&g, 1)->v[1], 2, 1e-15);
    CHECK(!ysig_set(&g, 1, 8, 1e9));            /* out of the table */
    CHECK(!ysig_set(&g, 1, 9, 1));              /* a parameter affine does not read */
    CHECK(!ysig_set(&g, 1, 0, NAN));
    CHECK(!ysig_set(&g, 2, 1, 512));            /* sizes the arena */
    CHECK(ysig_set(&g, 2, 0, 0.02));
    CHECK(!ysig_set(&g, 3, 0, 63));             /* channels 63, 64 of 64 */
    CHECK(ysig_set(&g, 3, 0, 62));
    CHECK(!ysig_set(&g, 4, 0, 1));              /* a record's rate is the schedule's */
    CHECK(!ysig_set(&g, 0, 3, 1));              /* a source's lanes */
    CHECK(ysig_set(&g, 0, 2, 5));               /* its device */
    e.t = 3 * MS;
    CHECK(ysig_feed(&g, &e) == 0);
    e.device = 5;
    CHECK(ysig_feed(&g, &e) == 1);
    CHECK(!ysig_set(&g, 9, 0, 1) && !ysig_set(&g, -1, 0, 1) && !ysig_set(NULL, 0, 0, 1));
    CHECK(!ysig_set_data(&g, 1, 0, 1));
    CHECK(!ysig_arm(&g, 1, 0) && !ysig_disarm(&g, 1));
}

/* ---------------------------------------------------------------- refusals */

static void expect_refused(const ysig_node* n, int count, const char* want) {
    ysig_desc d = desc_of(n, count);
    char err[256];
    ysig_graph g;
    bool ok = ysig_check(&d, err, sizeof err);
    if (ok || !strstr(err, want)) {
        fprintf(stderr, "sig_test [%s]: FAIL: want a refusal with \"%s\", got %s \"%s\"\n", g_where, want,
                ok ? "a pass" : "a refusal", err);
        g_failures++;
    }
    CHECK(ysig_arena_size(&d) == 0);
    d.arena = g_arena;
    d.arena_size = sizeof g_arena;
    CHECK(!ysig_open(&g, &d) && strstr(ysig_error(&g), want));
}

static void test_refusals(void) {
    static const double three[3] = { 1, 1, 0 };
    ysig_node n[6];
    ysig_desc d;
    ysig_graph g;
    char err[256];
    g_where = "refusals";
    n[0] = src_node(YIN_KIND_MOUSE, YIN_AXIS_DELTA, 0, 2);
    n[1] = ysig_node_of(YSIG_OP_INTEGRATE, 0, -1);
    n[2] = ysig_node_of(YSIG_OP_SUM, 1, 2);       /* itself: a cycle */
    expect_refused(n, 3, "a cycle");
    n[1].in[0] = 2;                                /* a later node */
    n[2] = ysig_node_of(YSIG_OP_OFFSET, 1, -1);
    expect_refused(n, 3, "a cycle");
    n[1].in[0] = 0;
    n[2] = ysig_node_of(YSIG_OP_OFFSET, 1, -1);
    n[3] = ysig_node_of(YSIG_OP_SELECT, 1, -1);
    n[3].lanes = 1;
    n[4] = ysig_node_of(YSIG_OP_DIFF, 2, 3);       /* 2 lanes minus 1 */
    expect_refused(n, 5, "different count");
    n[4] = ysig_node_of(99, 2, -1);
    expect_refused(n, 5, "unknown op");
    n[4] = ysig_node_of(YSIG_OP_CUSTOM, 2, -1);
    expect_refused(n, 5, "unknown op");
    n[4] = ysig_node_of(YSIG_OP_OFFSET, 2, -1);
    n[4].in[0] = -1; n[4].in[1] = 2;
    expect_refused(n, 5, "after a -1");
    n[4] = ysig_node_of(YSIG_OP_OFFSET, 2, -1);
    n[4].in[2] = -3;
    expect_refused(n, 5, "input 2 is -3");
    n[4] = ysig_node_of(YSIG_OP_DIFF, 2, -1);
    expect_refused(n, 5, "takes 2 to 2 inputs");
    n[4] = ysig_node_of(YSIG_OP_JOIN, 2, 1);
    n[4].in[2] = 3;
    expect_refused(n, 5, "at most 4");
    n[4] = ysig_node_of(YSIG_OP_SELECT, 2, -1);
    n[4].lanes = 2; n[4].p[1] = 2;
    expect_refused(n, 5, "lane 2 of a 2-lane");
    n[4] = ysig_node_of(YSIG_OP_LOWPASS, 2, -1);
    n[4].p[0] = 0;
    expect_refused(n, 5, "out of");
    n[4].p[0] = 300; n[4].p[2] = 500;
    expect_refused(n, 5, "Nyquist");
    n[4] = ysig_node_of(YSIG_OP_OFFSET, 2, -1);
    n[4].p[13] = NAN;
    expect_refused(n, 5, "not finite");
    n[4] = ysig_node_of(YSIG_OP_BIND, 2, -1);
    n[4].p[0] = 63;
    expect_refused(n, 5, "channels 63..64");
    n[4] = ysig_node_of(YSIG_OP_OFFSET, 4, -1);
    n[4].in[0] = 2;
    n[4].lanes = 3;
    expect_refused(n, 5, "the op gives 2");
    n[4] = ysig_node_of(YSIG_OP_SINES, -1, -1);
    n[4].p[0] = 1; n[4].lanes = 2; n[4].data = three; n[4].n_data = 3;
    expect_refused(n, 5, "data has 3 values");
    n[4] = ysig_node_of(YSIG_OP_OFFSET, 2, -1);
    n[4].data = three; n[4].n_data = 3;
    expect_refused(n, 5, "data on an op");
    n[4] = ysig_node_of(YSIG_OP_RECORD, 2, -1);
    n[5] = ysig_node_of(YSIG_OP_OFFSET, 4, -1);
    expect_refused(n, 6, "is a sink");
    n[4] = ysig_node_of(YSIG_OP_ROLE, -1, -1);
    expect_refused(n, 5, "role 0 not declared");
    n[1].p[8] = 10; n[1].p[12] = 5;
    expect_refused(n, 2, "not below high");
    n[1].p[12] = 20; n[1].p[0] = -1;
    expect_refused(n, 2, "positive gain");
    n[1].p[0] = 0;
    n[1].history = -1;
    expect_refused(n, 2, "history");
    n[1].history = 0;
    n[1].p[8] = n[1].p[12] = 0;
    n[2] = ysig_node_of(YSIG_OP_DETECT, 1, -1);
    n[2].p[0] = YSIG_TEST_IN_RECT; n[2].p[8] = 5;
    expect_refused(n, 3, "low corner");
    /* arena: too small, unaligned */
    n[2] = ysig_node_of(YSIG_OP_OFFSET, 1, -1);
    d = desc_of(n, 3);
    d.arena = g_arena;
    d.arena_size = ysig_arena_size(&d) - 1;
    CHECK(!ysig_open(&g, &d) && strstr(ysig_error(&g), "needs"));
    d.arena_size += 1;
    CHECK(ysig_open(&g, &d));
    d.arena = (char*)g_arena + 4;
    CHECK(!ysig_open(&g, &d) && strstr(ysig_error(&g), "aligned"));
    CHECK(!ysig_check(NULL, err, sizeof err));
    d = desc_of(n, 0);
    CHECK(!ysig_check(&d, err, sizeof err) && strstr(err, "no nodes"));
    /* a closed or failed graph refuses every call */
    CHECK(ysig_feed(&g, NULL) == -1);
    CHECK(ysig_frame(&g, NULL) == -1);
    CHECK(ysig_value_of(&g, 0) == NULL);
}

int main(void) {
    test_conventions();
    test_integrate();
    test_maps();
    test_filters();
    test_generators();
    test_time();
    test_detect();
    test_replay();
    test_history();
    test_roles();
    test_set();
    test_refusals();
    if (g_failures) { fprintf(stderr, "sig_test: %d failures\n", g_failures); return 1; }
    printf("sig_test: all checks passed\n");
    return 0;
}
