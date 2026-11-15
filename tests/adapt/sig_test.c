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
    f.poll_ns = onset;   /* input known up to the onset, as on the virtual clock */
    f.has_poll = true;
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

/* The p[] names: each YSIG_<OP>_<ROW> is the row of that name in the op's
 * table, and every row of a built-in op has exactly one, so a program that
 * writes p[] by name writes the row the table documents. */
typedef struct pname { int op, idx; const char* name; } pname;
#define PN(op, c) { YSIG_OP_##op, c, #c }
static const pname g_pnames[] = {
    PN(SOURCE, YSIG_SOURCE_KIND), PN(SOURCE, YSIG_SOURCE_CONTROL), PN(SOURCE, YSIG_SOURCE_DEVICE),
    PN(SOURCE, YSIG_SOURCE_FIELDS), PN(SOURCE, YSIG_SOURCE_TIER), PN(SOURCE, YSIG_SOURCE_TYPE),
    PN(ROLE, YSIG_ROLE_ROLE), PN(INTEGRATE, YSIG_INTEGRATE_GAIN_X), PN(INTEGRATE, YSIG_INTEGRATE_GAIN_Y),
    PN(INTEGRATE, YSIG_INTEGRATE_GAIN_Z), PN(INTEGRATE, YSIG_INTEGRATE_GAIN_W),
    PN(INTEGRATE, YSIG_INTEGRATE_ORIGIN_X), PN(INTEGRATE, YSIG_INTEGRATE_ORIGIN_Y),
    PN(INTEGRATE, YSIG_INTEGRATE_ORIGIN_Z), PN(INTEGRATE, YSIG_INTEGRATE_ORIGIN_W),
    PN(INTEGRATE, YSIG_INTEGRATE_LOW_X), PN(INTEGRATE, YSIG_INTEGRATE_LOW_Y), PN(INTEGRATE, YSIG_INTEGRATE_LOW_Z),
    PN(INTEGRATE, YSIG_INTEGRATE_LOW_W), PN(INTEGRATE, YSIG_INTEGRATE_HIGH_X),
    PN(INTEGRATE, YSIG_INTEGRATE_HIGH_Y), PN(INTEGRATE, YSIG_INTEGRATE_HIGH_Z),
    PN(INTEGRATE, YSIG_INTEGRATE_HIGH_W), PN(LOWPASS, YSIG_LOWPASS_CUTOFF), PN(LOWPASS, YSIG_LOWPASS_ORDER),
    PN(LOWPASS, YSIG_LOWPASS_RATE), PN(VELOCITY, YSIG_VELOCITY_LAG), PN(VELOCITY, YSIG_VELOCITY_CAPACITY),
    PN(SPEED, YSIG_SPEED_LAG), PN(SPEED, YSIG_SPEED_CAPACITY), PN(DELAY, YSIG_DELAY_DELAY),
    PN(DELAY, YSIG_DELAY_CAPACITY), PN(DEADZONE, YSIG_DEADZONE_RADIUS), PN(AFFINE, YSIG_AFFINE_M_XX),
    PN(AFFINE, YSIG_AFFINE_M_XY), PN(AFFINE, YSIG_AFFINE_M_YX), PN(AFFINE, YSIG_AFFINE_M_YY),
    PN(AFFINE, YSIG_AFFINE_PRE_X), PN(AFFINE, YSIG_AFFINE_PRE_Y), PN(AFFINE, YSIG_AFFINE_POST_X),
    PN(AFFINE, YSIG_AFFINE_POST_Y), PN(AFFINE, YSIG_AFFINE_ANGLE), PN(SCALE, YSIG_SCALE_K_X),
    PN(SCALE, YSIG_SCALE_K_Y), PN(SCALE, YSIG_SCALE_K_Z), PN(SCALE, YSIG_SCALE_K_W), PN(OFFSET, YSIG_OFFSET_D_X),
    PN(OFFSET, YSIG_OFFSET_D_Y), PN(OFFSET, YSIG_OFFSET_D_Z), PN(OFFSET, YSIG_OFFSET_D_W),
    PN(CLAMP, YSIG_CLAMP_LOW_X), PN(CLAMP, YSIG_CLAMP_LOW_Y), PN(CLAMP, YSIG_CLAMP_LOW_Z),
    PN(CLAMP, YSIG_CLAMP_LOW_W), PN(CLAMP, YSIG_CLAMP_HIGH_X), PN(CLAMP, YSIG_CLAMP_HIGH_Y),
    PN(CLAMP, YSIG_CLAMP_HIGH_Z), PN(CLAMP, YSIG_CLAMP_HIGH_W), PN(SELECT, YSIG_SELECT_LANE_X),
    PN(SELECT, YSIG_SELECT_LANE_Y), PN(SELECT, YSIG_SELECT_LANE_Z), PN(SELECT, YSIG_SELECT_LANE_W),
    PN(SUM, YSIG_SUM_W_0), PN(SUM, YSIG_SUM_W_1), PN(SUM, YSIG_SUM_W_2), PN(SUM, YSIG_SUM_W_3),
    PN(MEAN, YSIG_MEAN_W_0), PN(MEAN, YSIG_MEAN_W_1), PN(MEAN, YSIG_MEAN_W_2), PN(MEAN, YSIG_MEAN_W_3),
    PN(SINES, YSIG_SINES_N), PN(SINES, YSIG_SINES_RAMP), PN(NOISE, YSIG_NOISE_SEED), PN(NOISE, YSIG_NOISE_SD),
    PN(NOISE, YSIG_NOISE_CUTOFF), PN(CHANNEL, YSIG_CHANNEL_CHANNEL), PN(BIND, YSIG_BIND_CHANNEL),
    PN(DETECT, YSIG_DETECT_TEST), PN(DETECT, YSIG_DETECT_LANE), PN(DETECT, YSIG_DETECT_LEVEL),
    PN(DETECT, YSIG_DETECT_HYSTERESIS), PN(DETECT, YSIG_DETECT_HOLD), PN(DETECT, YSIG_DETECT_AT),
    PN(DETECT, YSIG_DETECT_START), PN(DETECT, YSIG_DETECT_REPEAT), PN(DETECT, YSIG_DETECT_A_X),
    PN(DETECT, YSIG_DETECT_A_Y), PN(DETECT, YSIG_DETECT_B_X), PN(DETECT, YSIG_DETECT_B_Y),
    PN(DETECT, YSIG_DETECT_ENTRY), PN(RECORD, YSIG_RECORD_PER), PN(EMIT, YSIG_EMIT_KIND),
    PN(EMIT, YSIG_EMIT_CONTROL), PN(DETECT, YSIG_DETECT_OUT), PN(DISTANCE, YSIG_DISTANCE_POINT_X),
    PN(DISTANCE, YSIG_DISTANCE_POINT_Y), PN(DISTANCE, YSIG_DISTANCE_POINT_Z), PN(PEAK, YSIG_PEAK_LANE),
    PN(SILENT, YSIG_SILENT_AFTER)
};
#undef PN

static void test_param_names(void) {
    uint32_t seen[YSIG_OP_COUNT];
    char want[64];
    int i, k, n, op;
    g_where = "param names";
    memset(seen, 0, sizeof seen);
    for (i = 0; i < (int)(sizeof g_pnames / sizeof g_pnames[0]); i++) {
        const pname* q = &g_pnames[i];
        const ysig_param* p = ysig_op_params(q->op, &n);
        const char* on = ysig_op_name(q->op);
        size_t w = 0;
        CHECK(p != NULL && q->idx >= 0 && q->idx < n);
        if (!p || q->idx < 0 || q->idx >= n || !on) continue;
        w += (size_t)snprintf(want, sizeof want, "YSIG_");
        for (k = 0; on[k] && w + 1 < sizeof want; k++) want[w++] = (char)(on[k] - 'a' + 'A');
        want[w++] = '_';
        for (k = 0; p[q->idx].name[k] && w + 1 < sizeof want; k++) {
            char c = p[q->idx].name[k];
            want[w++] = c == '.' ? '_' : (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
        }
        want[w] = 0;
        if (strcmp(want, q->name) != 0)
            fprintf(stderr, "sig_test [%s]: %s is row %d, \"%s\" (want %s)\n", g_where, q->name, q->idx,
                    p[q->idx].name, want);
        CHECK(!strcmp(want, q->name));
        CHECK(!(seen[q->op] & (1u << q->idx)));   /* one name per row */
        seen[q->op] |= 1u << q->idx;
    }
    for (op = 1; op < YSIG_OP_COUNT; op++) {
        (void)ysig_op_params(op, &n);
        CHECK(seen[op] == (n ? (1u << n) - 1u : 0u));   /* every row named */
    }
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
    n[9].p[0] = 3;                                            /* weighted */
    n[10] = ysig_node_of(YSIG_OP_DIFF, 0, 3);
    n[11] = ysig_node_of(YSIG_OP_DEADZONE, 0, -1);
    n[11].p[0] = 5;
    n[12] = ysig_node_of(YSIG_OP_AFFINE, 0, -1);             /* identity: all zero */
    n[13] = ysig_node_of(YSIG_OP_SELECT, 0, -1);
    n[13].lanes = 1; n[13].p[0] = 0;
    n[14] = ysig_node_of(YSIG_OP_SCALE, 0, 13);               /* times x */
    n[15] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 0, 0);
    n[15].p[3] = 1 | 4;                                       /* x and value */
    d = desc_of(n, 16);
    if (!open_desc(&g, &d)) { g_failures++; return; }
    ysig_reset(&g, 0);
    e = axis(MS, YIN_KIND_PEN, YIN_AXIS_POSITION, 0, 13, 24, 0.5f, YIN_TIER_SIM);
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
    v = ysig_value_of(&g, 9);        /* (3 (13, 24) + (26, -72)) / 4 */
    CHECK(v->v[0] == 16.25 && v->v[1] == 0);
    v = ysig_value_of(&g, 10);
    CHECK(v->v[0] == -13 && v->v[1] == 96);
    v = ysig_value_of(&g, 11);       /* |(13, 24)| = 27.29.., shrunk by 5 */
    CHECK_NEAR(sqrt(v->v[0] * v->v[0] + v->v[1] * v->v[1]), sqrt(13.0 * 13 + 24 * 24) - 5, 1e-12);
    CHECK_NEAR(v->v[1] / v->v[0], 24.0 / 13, 1e-12);
    v = ysig_value_of(&g, 12);
    CHECK(v->v[0] == 13 && v->v[1] == 24);
    v = ysig_value_of(&g, 14);
    CHECK(v->v[0] == 169 && v->v[1] == 312);
    v = ysig_value_of(&g, 15);
    CHECK(v->lanes == 2 && v->v[0] == 13 && v->v[1] == 0.5);
    /* the mirror keeps the sign of zero: a skipped zero term */
    e = axis(2 * MS, YIN_KIND_PEN, YIN_AXIS_POSITION, 0, 7, -0.0f, 0, YIN_TIER_SIM);
    ysig_feed(&g, &e);
    v = ysig_value_of(&g, 2);
    CHECK(v->v[0] == 0 && 1.0 / v->v[0] < 0 && v->v[1] == 7);   /* -0; MinGW's signbit converts to float */
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
        /* silent for 5 ms, under the 10 ms lag: the last sample's speed stands */
        CHECK_NEAR(ysig_value_of(&g, 2)->v[0], 500, 1e-3);
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
    /* the right hand has not reported: its part has no stamp, the worst */
    CHECK(der[0].t == 10 * MS && der[0].stamp == YIN_TIER_UNKNOWN && der[0].x == 2 && der[0].y == 0);
    CHECK(der[0].code == (0u | (1u << 16)));   /* newest: the left's source; oldest: the right's reset */
    CHECK(der[1].t == 12 * MS && der[1].stamp == YIN_TIER_3 && der[1].code == (1u | (0u << 16)));
    /* the right hand's sample ran nothing of the left's: one row */
    CHECK(g_rows == 1 && g_row[0].node == 6 && !strcmp(g_row[0].name, "left") && g_row[0].value.t == 10 * MS);
    CHECK(g_row[0].per_frame == 0 && g_row[0].value.v[0] == 4);
    /* raw mouse counts out of order are summed, not dropped: a sum does
     * not depend on the order of its terms */
    g_where = "out of order";
    e = delta(9 * MS, 1, 100, 0);
    CHECK(ysig_feed(&g, &e) == 1);
    e = delta(10 * MS, 1, 2, 0);
    CHECK(ysig_feed(&g, &e) == 1);
    ysig_get_stats(&g, &st);
    CHECK(st.dropped_order == 0 && st.taken == 4 && st.fed == 4);
    CHECK(ysig_value_of(&g, 2)->v[0] == 106);
    e = delta(13 * MS, 2, 0, 2);
    CHECK(ysig_feed(&g, &e) == 1);
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
        static const float xs[] = { 12, 9, 15, 14.5f, 15.5f, 13.9f, 16, 4 };
        static const int hit[] = { 0, 0, 1, 0, 0, 0, 1, 0 };
        /* 12: |2| < 5, ready; 15: hit; 14.5: 4.5 is under 5 but not under
         * 5 - 1: not ready; 15.5: no hit; 13.9: 3.9 < 4, ready; 16: hit;
         * 4: |6|, not ready yet */
        for (i = 0; i < 8; i++) {
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
    e = axis(160 * MS, YIN_KIND_PEN, YIN_AXIS_POSITION, 0, 1, 0, 0, YIN_TIER_SIM);
    ysig_feed(&g, &e);
    ysig_arm(&g, 2, 300 * MS);                /* ready; from 0.3 s on */
    e = axis(170 * MS, YIN_KIND_PEN, YIN_AXIS_POSITION, 0, -1, 0, 0, YIN_TIER_SIM);
    ysig_feed(&g, &e);
    /* the arm at 0.3 s waits for input past its time (v0.6.0): the arm at
     * 140 ms still stands, and 170 ms is its hit */
    CHECK(ysig_detected(&g, h, 8) == 1 && h[0].node == 2 && h[0].t == 170 * MS);
    e = axis(310 * MS, YIN_KIND_PEN, YIN_AXIS_POSITION, 0, 1, 0, 0, YIN_TIER_SIM);
    ysig_feed(&g, &e);
    e = axis(320 * MS, YIN_KIND_PEN, YIN_AXIS_POSITION, 0, -1, 0, 0, YIN_TIER_SIM);
    ysig_feed(&g, &e);
    {
        int got = ysig_detected(&g, h, 8), found = 0;
        for (i = 0; i < got; i++) found |= h[i].node == 2 && h[i].t == 320 * MS;
        CHECK(found);
    }

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

/* ---------------------------------------------------------------- digest */

/* A graph of exact arithmetic (counts, the integer grid) and its digest:
 * the same number on every host. */
static void test_digest(void) {
    ysig_node n[4];
    ysig_desc d;
    ysig_graph g;
    ysig_stats st;
    int i;
    g_where = "digest";
    n[0] = src_node(YIN_KIND_MOUSE, YIN_AXIS_DELTA, 0, 2);
    n[1] = ysig_node_of(YSIG_OP_INTEGRATE, 0, -1);
    n[1].p[0] = n[1].p[1] = 0.25;
    n[2] = ysig_node_of(YSIG_OP_RECORD, 1, -1);
    n[3] = ysig_node_of(YSIG_OP_DETECT, 1, -1);
    n[3].p[2] = 10; n[3].p[7] = 1; n[3].p[3] = 1;
    d = desc_of(n, 4);
    if (!open_desc(&g, &d)) { g_failures++; return; }
    ysig_reset(&g, 0);
    for (i = 0; i < 200; i++) {
        yin_event e = delta(i * MS, 3, (float)((i * 7) % 11 - 5), (float)((i * 3) % 7 - 3));
        ysig_feed(&g, &e);
    }
    ysig_get_stats(&g, &st);
    if (st.digest != 0xfa88821532bc868aull) fprintf(stderr, "  digest %016llx\n", (unsigned long long)st.digest);
    CHECK(st.rows == 200 && st.digest == 0xfa88821532bc868aull);
    /* derived events, bound channel values and frame onsets are in the
     * digest too: a graph without a record or a detect still has one */
    {
        uint64_t dg[3];
        int run;
        n[2] = ysig_node_of(YSIG_OP_EMIT, 1, -1);
        n[3] = ysig_node_of(YSIG_OP_BIND, 1, -1);
        d = desc_of(n, 4);
        for (run = 0; run < 3; run++) {
            ysig_frame_in f;
            if (!open_desc(&g, &d)) { g_failures++; return; }
            ysig_reset(&g, 0);
            for (i = 0; i < 20; i++) {
                /* run 1: one count moves from sample 8 to 7, so only the
                 * derived events differ, not the bound end value */
                yin_event e = delta(i * MS, 3, run == 1 && i == 7 ? 2.0f : run == 1 && i == 8 ? 0.0f : 1.0f, 0);
                ysig_feed(&g, &e);
            }
            f = frame_at(0, run == 2 ? 30 * MS + 1 : 30 * MS, 0, NULL, 0);
            ysig_frame(&g, &f);
            ysig_get_stats(&g, &st);
            dg[run] = st.digest;
        }
        CHECK(dg[0] != dg[1] && dg[0] != dg[2] && st.rows == 0 && st.hits == 0);
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
    /* a second report at the left's last time replaces its entry: the
     * ring keeps one value per time */
    e = delta(20 * MS, 1, 1, 0);
    ysig_feed(&g, &e);
    CHECK(ysig_value_at(&g, 2, 20 * MS, &v) == 1 && v.v[0] == 21);
    /* left at 15.7 ms: its sample of 15 ms (15 counts); right of 12.5 ms */
    CHECK(ysig_value_at(&g, 2, 15 * MS + 700000, &v) == 1 && v.v[0] == 15 && v.t == 15 * MS);
    CHECK(ysig_value_at(&g, 6, 12 * MS + 700000, &v) == 1 && v.v[0] == 6 && v.v[1] == 6);
    CHECK(v.t == 12 * MS + 500000 && v.t_oldest == 12 * MS);
    CHECK(ysig_value_at(&g, 6, 12 * MS + 200000, &v) == 1 && v.v[1] == 5.5);
    /* older than the left's 8 entries: its oldest, and 0 */
    CHECK(ysig_value_at(&g, 2, 5 * MS, &v) == 0 && v.v[0] == 13);
    CHECK(ysig_value_at(&g, 6, -1, &v) == 0);
    /* the right's report of 31.5 ms comes before the left's of 31 ms: the
     * mean at 31.2 ms is from each hand's own ring, not from the order */
    e = delta(30 * MS, 1, 1, 0);
    ysig_feed(&g, &e);
    e = delta(31 * MS + 500000, 2, 0, 1);
    ysig_feed(&g, &e);
    e = delta(31 * MS, 1, 1, 0);
    ysig_feed(&g, &e);
    CHECK(ysig_value_at(&g, 6, 31 * MS + 200000, &v) == 1 && v.v[0] == 23.0 / 2 && v.v[1] == 20.0 / 2);
    CHECK(v.t == 31 * MS && v.t_oldest == 20 * MS + 500000);
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

/* ---------------------------------------------------------------- review */

/* Order per (source, device); a value's t and source are the arriving
 * report's; positions out of order are dropped per device. */
static void test_order(void) {
    ysig_node n[6];
    ysig_desc d;
    ysig_graph g;
    yin_event e, der[8];
    ysig_stats st;
    g_where = "order per device";
    /* two mice on one any-device source: the second's earlier report
     * still counts (it was dropped when order was per source) */
    n[0] = src_node(YIN_KIND_MOUSE, YIN_AXIS_DELTA, 0, 2);
    n[1] = ysig_node_of(YSIG_OP_INTEGRATE, 0, -1);
    n[2] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 0, 2);
    d = desc_of(n, 3);
    if (!open_desc(&g, &d)) { g_failures++; return; }
    ysig_reset(&g, 0);
    e = delta(10 * MS, 3, 5, 0);
    ysig_feed(&g, &e);
    e = delta(10 * MS - 200000, 4, 7, 0);
    CHECK(ysig_feed(&g, &e) == 1);
    CHECK(ysig_value_of(&g, 1)->v[0] == 12);
    /* positions: two pens interleave without a drop; within one pen an
     * older sample is dropped */
    e = axis(20 * MS, YIN_KIND_PEN, YIN_AXIS_POSITION, 1, 1, 1, 0, YIN_TIER_SIM);
    CHECK(ysig_feed(&g, &e) == 1);
    e = axis(19 * MS, YIN_KIND_PEN, YIN_AXIS_POSITION, 2, 2, 2, 0, YIN_TIER_SIM);
    CHECK(ysig_feed(&g, &e) == 1);
    e = axis(18 * MS, YIN_KIND_PEN, YIN_AXIS_POSITION, 1, 3, 3, 0, YIN_TIER_SIM);
    CHECK(ysig_feed(&g, &e) == 0);
    ysig_get_stats(&g, &st);
    CHECK(st.dropped_order == 1 && ysig_value_of(&g, 2)->v[0] == 2);

    /* a cursor of two hands: the report that arrives makes the value, and
     * the derived event carries its time and its source (P4) */
    g_where = "arriving report";
    n[0] = src_node(YIN_KIND_MOUSE, YIN_AXIS_DELTA, 1, 2);
    n[1] = src_node(YIN_KIND_MOUSE, YIN_AXIS_DELTA, 2, 2);
    n[2] = ysig_node_of(YSIG_OP_INTEGRATE, 0, -1);
    n[3] = ysig_node_of(YSIG_OP_INTEGRATE, 1, -1);
    n[4] = ysig_node_of(YSIG_OP_MEAN, 2, 3);
    n[5] = ysig_node_of(YSIG_OP_EMIT, 4, -1);
    d = desc_of(n, 6);
    if (!open_desc(&g, &d)) { g_failures++; return; }
    ysig_reset(&g, 0);
    e = delta(10 * MS, 1, 0, 0);
    ysig_feed(&g, &e);
    ysig_derived(&g, der, 8);
    e = delta(10 * MS - 300000, 2, 100, 0);
    e.unc_us = 250;
    ysig_feed(&g, &e);
    CHECK(ysig_derived(&g, der, 8) == 1);
    /* the report is R's, at its own time: an emit carries the true t,
     * which need not rise in arrival order (ysp/input.h) */
    CHECK(der[0].x == 50 && der[0].t == 10 * MS - 300000 && (der[0].code & 0xFFFFu) == 1 && der[0].unc_us == 250);
    {
        ysig_stats st2;
        ysig_get_stats(&g, &st2);
        CHECK(st2.time_clamped == 0 && ysig_value_of(&g, 4)->t == 10 * MS - 300000);
    }
    CHECK(ysig_value_of(&g, 4)->t_oldest == 10 * MS - 300000 && (der[0].code >> 16) == 1);
}

/* Views at a frame use input up to the poll, never the gap to a planned
 * onset (P8); history rings keep sample times only (P1). */
static void test_views(void) {
    ysig_node n[4];
    ysig_desc d;
    ysig_graph g;
    ysig_hit h[8];
    ysig_value v;
    ysig_stats st;
    int k, fr, total = 0;
    g_where = "views and the poll";
    n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 0, 2);
    n[1] = ysig_node_of(YSIG_OP_SPEED, 0, -1);
    n[1].p[0] = 0.010;
    n[2] = ysig_node_of(YSIG_OP_DETECT, 1, -1);              /* movement end: speed <= 50 */
    n[2].p[0] = YSIG_TEST_LE; n[2].p[2] = 50; n[2].p[6] = 1; n[2].p[7] = 1;
    n[3] = ysig_node_of(YSIG_OP_VELOCITY, 0, -1);
    n[3].p[0] = 0.002;
    n[3].history = 1024;
    d = desc_of(n, 4);
    if (!open_desc(&g, &d)) { g_failures++; return; }
    ysig_reset(&g, 0);
    /* 1000 px/s, a sample a ms; the poll at 16.67 k ms, the planned onset a
     * frame later: no movement-end hit while moving */
    k = 1;
    for (fr = 1; fr <= 30; fr++) {
        int64_t poll = fr * 16666667LL;
        ysig_frame_in f = frame_at(fr, poll + 16666667LL, 0, NULL, 0);
        f.poll_ns = poll;
        for (; k * MS <= poll; k++) {
            yin_event e = axis(k * MS, YIN_KIND_PEN, YIN_AXIS_POSITION, 0, (float)k, 0, 0, YIN_TIER_SIM);
            ysig_feed(&g, &e);
        }
        if (fr == 2) ysig_arm(&g, 2, poll);
        ysig_frame(&g, &f);
        total += ysig_detected(&g, h, 8);
        if (fr == 10) CHECK_NEAR(ysig_value_of(&g, 1)->v[0], 1000, 1e-6);
    }
    CHECK(total == 0);
    /* the hand stops at 500 ms; polls 5 ms on, then 11 ms on */
    {
        ysig_frame_in f = frame_at(31, 530 * MS, 0, NULL, 0);
        f.poll_ns = k * MS + 4 * MS;
        ysig_frame(&g, &f);
        CHECK_NEAR(ysig_value_of(&g, 1)->v[0], 1000, 1e-6);
        CHECK(ysig_detected(&g, h, 8) == 0);
        f.index = 32;
        f.onset_ns = 547 * MS;
        f.poll_ns = (k - 1) * MS + 11 * MS;
        ysig_frame(&g, &f);
        CHECK(ysig_value_of(&g, 1)->v[0] == 0 && ysig_value_of(&g, 1)->t == f.poll_ns);
        CHECK(ysig_detected(&g, h, 8) == 1 && h[0].t == f.poll_ns);
        /* the velocity's ring keeps samples: at the poll, the last one */
        CHECK(ysig_value_of(&g, 3)->v[0] == 0);
        CHECK(ysig_value_at(&g, 3, f.poll_ns, &v) == 1 && v.v[0] == 1000 && v.t == (k - 1) * MS);
    }
    /* history: samples between a poll and the last planned onset are kept,
     * and value_at gives a sample's value, not a frame's view */
    ysig_get_stats(&g, &st);
    CHECK(st.history_skipped == 0);
    CHECK(ysig_value_at(&g, 3, 250 * MS + 500000, &v) == 1 && v.t == 250 * MS && v.v[0] == 1000);
    /* a graph with a view refuses a frame without a poll on every call,
     * before any input too: a configuration error, not a data one; poll 0
     * with has_poll is a valid time */
    if (!open_desc(&g, &d)) { g_failures++; return; }
    ysig_reset(&g, 0);
    {
        ysig_frame_in f;
        memset(&f, 0, sizeof f);   /* zeroed: no poll */
        f.onset_ns = 10 * MS;
        CHECK(ysig_frame(&g, &f) == -1 && strstr(ysig_error(&g), "no poll"));
        f.has_poll = true;
        CHECK(ysig_frame(&g, &f) == 0);
    }
    for (k = 1; k <= 20; k++) {
        yin_event e = axis(k * MS, YIN_KIND_PEN, YIN_AXIS_POSITION, 0, (float)k, 0, 0, YIN_TIER_SIM);
        ysig_feed(&g, &e);
    }
    {
        /* an event no source takes says nothing of the pen: a replay's
         * journal would not have it, so it does not move the bound */
        yin_event key = axis(40 * MS, YIN_KIND_KEYBOARD, 4, 0, 0, 0, 1, 0);
        ysig_frame_in f = frame_at(1, 60 * MS, 0, NULL, 0);
        key.type = YIN_PRESS;
        CHECK(ysig_feed(&g, &key) == 0);
        f.has_poll = false;
        CHECK(ysig_frame(&g, &f) == -1 && strstr(ysig_error(&g), "no poll"));
        f.has_poll = true;
        f.poll_ns = 20 * MS;
        CHECK(ysig_frame(&g, &f) == 0);
        CHECK_NEAR(ysig_value_of(&g, 1)->v[0], 1000, 1e-6);
        /* a poll before the newest event fed: counted with the excess; the
         * frame runs, and the speed, whose last sample is past the poll,
         * stands */
        f.index = 2;
        f.poll_ns = 15 * MS;
        CHECK(ysig_frame(&g, &f) == 0);
        ysig_get_stats(&g, &st);
        CHECK(st.late_poll == 1 && st.late_poll_max_ns == 5 * MS);
        CHECK_NEAR(ysig_value_of(&g, 1)->v[0], 1000, 1e-6);
    }
}

/* The third review: ysig_frame never refuses because of data (R1, R1b,
 * R2); a frame's value takes its newest input's time (R3); a sink's time
 * never goes back; least-recently-used order slots (lru.c). */
static int g_frows;
static ysig_row g_frow[64];
static void f_row(void* user, const ysig_row* r);
static yin_event pen3(int64_t t, int dev, float v) {
    return axis(t, YIN_KIND_PEN, YIN_AXIS_POSITION, dev, v, 0, v, YIN_TIER_SIM);
}
#ifndef __cplusplus
/* YSIG_NODE must build what ysig_node_of() and assignments build: the two
 * unused inputs -1 (0 is a real node), every field not named zero. */
static void test_node_macro(void) {
    static const double data[3] = { 1, 2, 3 };
    ysig_node a = ysig_node_of(YSIG_OP_DETECT, 3, -1), b;
    int i;
    a.p[YSIG_DETECT_LEVEL] = 12;
    a.p[YSIG_DETECT_HOLD] = 0.5;
    a.name = "hold";
    a.lanes = 1;
    a.history = 8;
    a.data = data;
    a.n_data = 3;
    b = YSIG_NODE(YSIG_OP_DETECT, 3, -1, .name = "hold", .lanes = 1, .history = 8, .data = data, .n_data = 3,
                  .p = { [YSIG_DETECT_LEVEL] = 12, [YSIG_DETECT_HOLD] = 0.5 });
    CHECK(b.op == a.op && b.lanes == a.lanes && b.history == a.history);
    CHECK(b.data == a.data && b.n_data == a.n_data && !strcmp(b.name, a.name));
    for (i = 0; i < 4; i++) CHECK(b.in[i] == a.in[i]);
    for (i = 0; i < YSIG_NP; i++) CHECK(b.p[i] == a.p[i]);
}
#endif

static void test_review3(void) {
    ysig_node n[5];
    ysig_desc d;
    ysig_graph g;
    ysig_stats st;
    yin_event e;
    int k, bad = 0;
    g_where = "a stamp in the future";
    n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 0, 1);
    n[1] = ysig_node_of(YSIG_OP_BIND, 0, -1);
    d = desc_of(n, 2);
    if (!open_desc(&g, &d)) { g_failures++; return; }
    ysig_reset(&g, 0);
    e = pen3(10 * MS, 1, 1);
    ysig_feed(&g, &e);
    e = pen3(10 * MS + 60LL * 1000000000LL, 2, 2);   /* 60 s ahead: a device clock glitch */
    ysig_feed(&g, &e);
    for (k = 1; k <= 600; k++) {
        ysig_frame_in f;
        e = pen3(10 * MS + k * 16 * MS, 1, (float)(3 + k));
        ysig_feed(&g, &e);
        f = frame_at(k, e.t + 16 * MS, 0, NULL, 0);
        f.poll_ns = e.t;
        if (ysig_frame(&g, &f) != 0) bad++;
    }
    CHECK(bad == 0 && g_ch[0] == 603);
    ysig_get_stats(&g, &st);
    CHECK(st.late_poll == 600);
    /* INT64_MAX on a new device of an any-device source */
    if (!open_desc(&g, &d)) { g_failures++; return; }
    ysig_reset(&g, 0);
    e = pen3(10 * MS, 1, 1);
    ysig_feed(&g, &e);
    e = pen3(INT64_MAX, 3, 2);
    ysig_feed(&g, &e);
    {
        ysig_frame_in f = frame_at(1, 30 * MS, 0, NULL, 0);
        f.poll_ns = 20 * MS;
        CHECK(ysig_frame(&g, &f) == 0 && g_ch[0] == 2);
        ysig_get_stats(&g, &st);
        CHECK(st.late_poll == 1 && st.late_poll_max_ns == INT64_MAX - 20 * MS);
        /* no view: a frame without a poll runs, and is no late poll */
        f.has_poll = false;
        CHECK(ysig_frame(&g, &f) == 0);
        ysig_get_stats(&g, &st);
        CHECK(st.late_poll == 1);
    }
    /* the given poll is used as is: pen A still since 60 ms, pen B at
     * 140 ms, a poll of 90 ms: A has been silent 30 ms by the poll, under
     * its 40 ms lag, so its speed stands; B's later stamp moves nothing
     * and is counted */
    n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 1, 1);
    n[1] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 2, 1);
    n[2] = ysig_node_of(YSIG_OP_SPEED, 0, -1);
    n[2].p[0] = 0.040;
    d = desc_of(n, 3);
    if (!open_desc(&g, &d)) { g_failures++; return; }
    ysig_reset(&g, 0);
    for (k = 1; k <= 60; k++) {
        e = pen3(k * MS, 1, (float)k);
        ysig_feed(&g, &e);
    }
    CHECK_NEAR(ysig_value_of(&g, 2)->v[0], 1000, 1e-6);
    e = pen3(140 * MS, 2, 0);
    ysig_feed(&g, &e);
    {
        ysig_frame_in f = frame_at(1, 160 * MS, 0, NULL, 0);
        f.poll_ns = 90 * MS;
        CHECK(ysig_frame(&g, &f) == 0);
        CHECK_NEAR(ysig_value_of(&g, 2)->v[0], 1000, 1e-6);
        ysig_get_stats(&g, &st);
        CHECK(st.late_poll == 1 && st.late_poll_max_ns == 50 * MS);
    }
    /* the final review's S1: after one bad stamp (60 s ahead, or INT64_MAX
     * and a reset) the views still run to the poll: speed 1000 at every
     * onset while the hand moves, no movement-end hit */
    g_where = "one bad stamp";
    {
        static const int64_t badt[3] = { 0, 60LL * 1000000000LL, INT64_MAX };
        int c, fr;
        for (c = 0; c < 3; c++) {
            ysig_hit h[8];
            int total = 0, ok_speed = 1;
            n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 0, 2);
            n[1] = ysig_node_of(YSIG_OP_SPEED, 0, -1);
            n[1].p[0] = 0.010;
            n[2] = ysig_node_of(YSIG_OP_DETECT, 1, -1);
            n[2].p[0] = YSIG_TEST_LE; n[2].p[2] = 50; n[2].p[6] = 1; n[2].p[7] = 1;
            n[3] = ysig_node_of(YSIG_OP_BIND, 1, -1);
            n[4] = src_node(YIN_KIND_SYNC, 0, 0, 1);   /* a TTL line */
            d = desc_of(n, 5);
            if (!open_desc(&g, &d)) { g_failures++; return; }
            ysig_reset(&g, 0);
            if (badt[c]) {
                e = axis(badt[c], YIN_KIND_SYNC, 0, 1, 1, 0, 1, YIN_TIER_SIM);
                ysig_feed(&g, &e);
            }
            if (c == 2) ysig_reset(&g, 0);
            k = 1;
            for (fr = 1; fr <= 30; fr++) {
                int64_t poll = fr * 16666667LL;
                ysig_frame_in f = frame_at(fr, poll + 16666667LL, 0, NULL, 0);
                f.poll_ns = poll;
                for (; k * MS <= poll; k++) {
                    e = pen3(k * MS, 1, (float)k);
                    ysig_feed(&g, &e);
                }
                if (fr == 2) ysig_arm(&g, 2, poll);
                CHECK(ysig_frame(&g, &f) == 0);
                total += ysig_detected(&g, h, 8);
                if (fr > 1 && g_ch[0] != 1000) ok_speed = 0;
            }
            CHECK(total == 0 && ok_speed);
        }
    }

    /* the final review's S2: a hold armed at 100 ms with both hands in;
     * hand B's report of 99 ms, outside, arrives late: it cannot hit or
     * start a hold, but it breaks the one that runs */
    g_where = "a late failing report";
    n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 1, 1);
    n[1] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 2, 1);
    n[2] = ysig_node_of(YSIG_OP_JOIN, 0, 1);
    n[3] = ysig_node_of(YSIG_OP_DETECT, 2, -1);
    n[3].p[0] = YSIG_TEST_LE; n[3].p[1] = -1; n[3].p[2] = 10; n[3].p[4] = 0.5; n[3].p[6] = 1;
    d = desc_of(n, 4);
    if (!open_desc(&g, &d)) { g_failures++; return; }
    ysig_reset(&g, 0);
    e = pen3(90 * MS, 1, 0);
    ysig_feed(&g, &e);
    e = pen3(91 * MS, 2, 0);
    ysig_feed(&g, &e);
    ysig_arm(&g, 3, 100 * MS);
    e = pen3(99 * MS, 2, 50);
    ysig_feed(&g, &e);
    {
        ysig_hit h[4];
        ysig_frame_in f = frame_at(1, 700 * MS, 0, NULL, 0);
        ysig_frame(&g, &f);
        CHECK(ysig_detected(&g, h, 4) == 0);
        /* B back in at 800 ms: the hold counts from there */
        e = pen3(800 * MS, 2, 0);
        ysig_feed(&g, &e);
        f = frame_at(2, 1400 * MS, 0, NULL, 0);
        ysig_frame(&g, &f);
        CHECK(ysig_detected(&g, h, 4) == 1 && h[0].t == 1300 * MS);
    }

    g_where = "reset keeps the fed time";
    /* a reset at the planned onset, the frame polled before it (a live
     * run's first trial) */
    if (!open_desc(&g, &d)) { g_failures++; return; }
    ysig_reset(&g, 1000 * MS);
    {
        ysig_frame_in f = frame_at(1, 1000 * MS, 0, NULL, 0);
        f.poll_ns = 990 * MS;
        CHECK(ysig_frame(&g, &f) == 0);
        ysig_get_stats(&g, &st);
        CHECK(st.late_poll == 0);
    }

    g_where = "a frame's newest input";
    n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 1, 1);
    n[1] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 2, 1);
    n[2] = ysig_node_of(YSIG_OP_SPEED, 0, -1);
    n[2].p[0] = 0.010;
    n[3] = ysig_node_of(YSIG_OP_JOIN, 2, 1);
    n[4] = ysig_node_of(YSIG_OP_RECORD, 3, -1);
    d = desc_of(n, 5);
    if (!open_desc(&g, &d)) { g_failures++; return; }
    ysig_reset(&g, 0);
    for (k = 1; k <= 20; k++) {
        e = pen3(k * MS, 1, (float)k);
        ysig_feed(&g, &e);
    }
    e = pen3(25 * MS, 2, 7);
    ysig_feed(&g, &e);
    CHECK(ysig_value_of(&g, 3)->t == 25 * MS && ysig_value_of(&g, 3)->src == 1);
    {
        ysig_frame_in f = frame_at(1, 40 * MS, 0, NULL, 0);
        f.poll_ns = 26 * MS;   /* A silent 6 ms, under the lag: the speed stands */
        ysig_frame(&g, &f);
        CHECK(ysig_value_of(&g, 3)->t == 25 * MS && ysig_value_of(&g, 3)->src == 1);
    }
    /* a sample-pass value whose t goes back (A's report of 22 ms after
     * B's of 25 ms): the record's rows carry the true times, out of order,
     * and nothing is clamped */
    g_frows = 0;
    d.on_row = f_row;
    if (!open_desc(&g, &d)) { g_failures++; return; }
    ysig_reset(&g, 0);
    e = pen3(25 * MS, 2, 7);
    ysig_feed(&g, &e);
    e = pen3(22 * MS, 1, 1);
    ysig_feed(&g, &e);
    ysig_get_stats(&g, &st);
    CHECK(g_frows == 2 && g_frow[0].value.t == 25 * MS && g_frow[1].value.t == 22 * MS && st.time_clamped == 0);

    /* a detect on two hands: a crossing hit has the time of the report that
     * met the test, also out of order; a hold's clock never goes back */
    g_where = "detect clock";
    n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 1, 1);
    n[1] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 2, 1);
    n[2] = ysig_node_of(YSIG_OP_SUM, 0, 1);
    n[3] = ysig_node_of(YSIG_OP_DETECT, 2, -1);       /* sum >= 10, every */
    n[3].p[0] = YSIG_TEST_GE; n[3].p[2] = 10; n[3].p[7] = 1;
    n[4] = ysig_node_of(YSIG_OP_DETECT, 2, -1);       /* sum >= 5 for 100 ms */
    n[4].p[0] = YSIG_TEST_GE; n[4].p[2] = 5; n[4].p[4] = 0.1;
    d = desc_of(n, 5);
    if (!open_desc(&g, &d)) { g_failures++; return; }
    ysig_reset(&g, 0);
    e = pen3(25 * MS, 2, 4);                          /* B: 4, under both levels */
    ysig_feed(&g, &e);
    e = pen3(22 * MS, 1, 7);                          /* A late: 11, past both; the hold starts at 25 ms */
    ysig_feed(&g, &e);
    {
        ysig_hit h[4];
        int nh = ysig_detected(&g, h, 4);
        CHECK(nh == 1 && h[0].node == 3 && h[0].t == 22 * MS);
        e = pen3(124 * MS, 2, 4);                     /* still past 5: the hold ends at 125 ms, not 122 */
        ysig_feed(&g, &e);
        CHECK(ysig_detected(&g, h, 4) == 0);
        e = pen3(126 * MS, 2, 4);
        ysig_feed(&g, &e);
        nh = ysig_detected(&g, h, 4);
        CHECK(nh == 1 && h[0].node == 4 && h[0].t == 125 * MS);
    }
    ysig_get_stats(&g, &st);
    CHECK(st.time_clamped == 2);   /* both detects saw A's 22 ms after 25 ms */
    /* a reset starts the clock again: a trial at earlier times holds from
     * its own samples */
    ysig_reset(&g, 0);
    e = pen3(10 * MS, 2, 4);
    ysig_feed(&g, &e);
    e = pen3(11 * MS, 1, 7);
    ysig_feed(&g, &e);
    e = pen3(112 * MS, 2, 4);
    ysig_feed(&g, &e);
    {
        ysig_hit h[4];
        int nh = ysig_detected(&g, h, 4), found = 0, k2;
        for (k2 = 0; k2 < nh; k2++) found |= h[k2].node == 4 && h[k2].t == 111 * MS;
        CHECK(found);
    }

    g_where = "least recently used slots";
    n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 0, 1);
    d = desc_of(n, 1);
    if (!open_desc(&g, &d)) { g_failures++; return; }
    ysig_reset(&g, 0);
    for (k = 1; k <= 8; k++) {
        e = pen3(100 + k, k, 0);
        ysig_feed(&g, &e);
    }
    e = pen3(200, 1, 0);
    ysig_feed(&g, &e);     /* device 1 busy again */
    e = pen3(300, 9, 0);
    ysig_feed(&g, &e);     /* takes device 2's slot */
    e = pen3(150, 1, 0);
    CHECK(ysig_feed(&g, &e) == 0);
    e = pen3(50, 2, 0);
    CHECK(ysig_feed(&g, &e) == 1);   /* device 2 back, late: passes (the 8-device limit) */
    ysig_get_stats(&g, &st);
    CHECK(st.dropped_order == 1 && st.order_evicted == 2);
}

/* A still input with the poll given: speed 0, a low-pass and a held stick's
 * rate integral run on to the poll (the review's Q1). */
static void test_still(void) {
    ysig_node n[5];
    ysig_desc d;
    ysig_graph g;
    int k, fr;
    g_where = "still input";
    n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 0, 1);
    n[1] = ysig_node_of(YSIG_OP_SPEED, 0, -1);
    n[1].p[0] = 0.010;
    n[2] = ysig_node_of(YSIG_OP_LOWPASS, 0, -1);
    n[2].p[0] = 5;
    n[3] = src_node(YIN_KIND_GAMEPAD, YIN_AXIS_GAMEPAD, 0, 1);
    n[4] = ysig_node_of(YSIG_OP_INTEGRATE, 3, -1);
    d = desc_of(n, 5);
    if (!open_desc(&g, &d)) { g_failures++; return; }
    ysig_reset(&g, 0);
    {
        yin_event e = axis(MS, YIN_KIND_GAMEPAD, YIN_AXIS_GAMEPAD, 1, 0, 0, 1.0f, YIN_TIER_SIM);
        ysig_feed(&g, &e);
    }
    for (k = 2; k <= 100; k++) {
        yin_event e = axis(k * MS, YIN_KIND_PEN, YIN_AXIS_POSITION, 1, 0, 0, (float)k, YIN_TIER_SIM);
        ysig_feed(&g, &e);
    }
    for (fr = 1; fr <= 120; fr++) {
        int64_t onset = 100 * MS + fr * 16666667LL;
        ysig_frame_in f = frame_at(fr, onset, 0, NULL, 0);
        f.poll_ns = onset - MS;
        CHECK(ysig_frame(&g, &f) == 0);
    }
    CHECK(ysig_value_of(&g, 1)->v[0] == 0);
    CHECK_NEAR(ysig_value_of(&g, 2)->v[0], 100, 1e-6);
    /* held at 1.0 from 1 ms to the last poll */
    CHECK_NEAR(ysig_value_of(&g, 4)->v[0], (double)(100 * MS + 120 * 16666667LL - MS - MS) / 1e9, 1e-9);
}

/* The second review: values at a time of a node with a generator input
 * (Q2), the digest's sign flips (Q3), value_at from on_row (Q4), busy
 * devices keep their order slot (Q5), a delay's time downstream (F). */
static ysig_graph* g_q4;
static int g_q4_rc;
static ysig_value g_q4_v;
static void q4_row(void* user, const ysig_row* r) {
    (void)user;
    if (r->value.t == 20 * MS) g_q4_rc = ysig_value_at(g_q4, 4, 5 * MS, &g_q4_v);
}
static void f_row(void* user, const ysig_row* r) {
    (void)user;
    if (g_frows < 64) g_frow[g_frows++] = *r;
}
static uint64_t q3_run(float a, float b) {
    ysig_node n[2];
    ysig_desc d;
    ysig_graph g;
    ysig_stats st;
    yin_event e;
    n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 0, 1);
    n[1] = ysig_node_of(YSIG_OP_RECORD, 0, -1);
    d = desc_of(n, 2);
    if (!open_desc(&g, &d)) return 0;
    ysig_reset(&g, 0);
    e = axis(MS, YIN_KIND_PEN, YIN_AXIS_POSITION, 1, 0, 0, a, YIN_TIER_SIM);
    ysig_feed(&g, &e);
    e = axis(2 * MS, YIN_KIND_PEN, YIN_AXIS_POSITION, 1, 0, 0, b, YIN_TIER_SIM);
    ysig_feed(&g, &e);
    ysig_get_stats(&g, &st);
    return st.digest;
}

static void test_review2(void) {
    static double comp[3] = { 1, 1, 0 };   /* cos(2 pi t) */
    ysig_node n[6];
    ysig_desc d;
    ysig_graph g;
    ysig_value v;
    ysig_stats st;
    yin_event e;
    int fr, k;
    g_where = "value at, generator input";
    n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 0, 1);
    n[1] = ysig_node_of(YSIG_OP_SINES, -1, -1);
    n[1].p[0] = 1; n[1].data = comp; n[1].n_data = 3;
    n[2] = ysig_node_of(YSIG_OP_SUM, 0, 1);       /* hand + perturbation */
    n[2].history = 256;
    d = desc_of(n, 3);
    if (!open_desc(&g, &d)) { g_failures++; return; }
    ysig_reset(&g, 0);
    e = axis(MS, YIN_KIND_PEN, YIN_AXIS_POSITION, 1, 0, 0, 10, YIN_TIER_SIM);
    ysig_feed(&g, &e);
    for (fr = 1; fr <= 30; fr++) {
        ysig_frame_in f = frame_at(fr, fr * 16666667LL, fr * 16666667LL, NULL, 0);
        ysig_frame(&g, &f);
    }
    CHECK(ysig_value_at(&g, 2, 12 * 16666667LL, &v) == 1);
    CHECK_NEAR(v.v[0], 10 + cos(2 * PI_ * (double)(12 * 16666667LL) / 1e9), 1e-12);

    g_where = "digest sign flips";
    CHECK(q3_run(1, 2) != q3_run(-1, -2));
    CHECK(q3_run(0.0f, 0.0f) != q3_run(-0.0f, -0.0f));
    CHECK(q3_run(1, 2) != q3_run(1, 4) && q3_run(1, 2) == q3_run(1, 2));

    g_where = "value at from on_row";
    n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 1, 1);
    n[1] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 2, 1);
    n[2] = ysig_node_of(YSIG_OP_OFFSET, 0, -1);
    n[2].history = 64;
    n[3] = ysig_node_of(YSIG_OP_OFFSET, 1, -1);
    n[3].history = 64;
    n[4] = ysig_node_of(YSIG_OP_JOIN, 2, 3);
    n[4].history = 64;
    n[5] = ysig_node_of(YSIG_OP_RECORD, 4, -1);
    d = desc_of(n, 6);
    d.on_row = q4_row;
    g_q4 = &g;
    g_q4_rc = -9;
    if (!open_desc(&g, &d)) { g_failures++; return; }
    ysig_reset(&g, 0);
    e = axis(MS, YIN_KIND_PEN, YIN_AXIS_POSITION, 1, 0, 0, 1, YIN_TIER_SIM);
    ysig_feed(&g, &e);
    e = axis(2 * MS, YIN_KIND_PEN, YIN_AXIS_POSITION, 2, 0, 0, 2, YIN_TIER_SIM);
    ysig_feed(&g, &e);
    e = axis(20 * MS, YIN_KIND_PEN, YIN_AXIS_POSITION, 1, 0, 0, 3, YIN_TIER_SIM);
    ysig_feed(&g, &e);
    CHECK(g_q4_rc == 1 && g_q4_v.t == 2 * MS && g_q4_v.v[0] == 1 && g_q4_v.v[1] == 2);

    g_where = "order slots";
    n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 0, 1);
    d = desc_of(n, 1);
    if (!open_desc(&g, &d)) { g_failures++; return; }
    ysig_reset(&g, 0);
    /* device 1 reports between every other device's: the ninth device
     * takes the least recently seen slot, not device 1's */
    for (k = 2; k <= 9; k++) {
        e = axis(100 * MS + 2 * k, YIN_KIND_PEN, YIN_AXIS_POSITION, 1, 0, 0, 1, YIN_TIER_SIM);
        ysig_feed(&g, &e);
        e = axis(100 * MS + 2 * k + 1, YIN_KIND_PEN, YIN_AXIS_POSITION, (uint32_t)k, 0, 0, (float)k, YIN_TIER_SIM);
        ysig_feed(&g, &e);
    }
    e = axis(50 * MS, YIN_KIND_PEN, YIN_AXIS_POSITION, 1, 0, 0, 99, YIN_TIER_SIM);
    CHECK(ysig_feed(&g, &e) == 0);
    ysig_get_stats(&g, &st);
    CHECK(st.dropped_order == 1 && st.order_evicted == 1 && ysig_value_of(&g, 0)->v[0] == 9);

    g_where = "a delay's time downstream";
    n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 0, 1);
    n[1] = ysig_node_of(YSIG_OP_DELAY, 0, -1);
    n[1].p[0] = 0.005;
    n[2] = ysig_node_of(YSIG_OP_OFFSET, 1, -1);
    n[2].p[0] = 100;
    n[3] = ysig_node_of(YSIG_OP_RECORD, 1, -1);
    n[4] = ysig_node_of(YSIG_OP_RECORD, 2, -1);
    d = desc_of(n, 5);
    d.on_row = f_row;
    if (!open_desc(&g, &d)) { g_failures++; return; }
    ysig_reset(&g, 0);
    g_frows = 0;
    for (k = 1; k <= 20; k++) {
        e = axis(k * MS, YIN_KIND_PEN, YIN_AXIS_POSITION, 1, 0, 0, (float)k, YIN_TIER_SIM);
        ysig_feed(&g, &e);
    }
    CHECK(g_frows == 40);
    for (k = 0; k + 1 < g_frows && k < 64; k += 2) {
        CHECK(g_frow[k].node == 3 && g_frow[k + 1].node == 4);
        CHECK(g_frow[k].value.t == g_frow[k + 1].value.t && g_frow[k + 1].value.v[0] == g_frow[k].value.v[0] + 100);
    }
    CHECK(g_frow[39].value.t == 15 * MS && g_frow[39].value.v[0] == 115);
}

/* A custom op with two inputs reads its inputs (P9). */
static void add_sample(void* st, const double* p, const ysig_value* in, int n_in, ysig_value* out) {
    (void)st; (void)p;
    out->v[0] = n_in == 2 ? in[0].v[0] + in[1].v[0] : -1;
}
static void gen_frame(void* st, const double* p, int64_t onset, ysig_value* out) {
    (void)st; (void)p; (void)onset;
    out->v[0] = 7;
}

static void test_review(void) {
    static const ysig_op_vtable add_vt = { YSIG_OP_ABI, "add", { 1, 1, 0, 0 }, 1, 0, NULL, 0, NULL, add_sample, NULL };
    static const ysig_op_vtable gen_vt = { YSIG_OP_ABI, "seven", { 0, 0, 0, 0 }, 1, 0, NULL, 0, NULL, NULL, gen_frame };
    const ysig_op_vtable* cv[2];
    ysig_node n[6];
    ysig_desc d;
    ysig_graph g;
    ysig_hit h[4];
    ysig_value v;
    yin_event e;
    char err[256];
    int k;
    cv[0] = &add_vt;
    cv[1] = &gen_vt;
    g_where = "custom inputs";
    n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 0, 1);
    n[1] = ysig_node_of(YSIG_OP_OFFSET, 0, -1);
    n[1].p[0] = 1000;
    n[2] = ysig_node_of(YSIG_OP_SCALE, 0, -1);
    n[2].p[0] = 2;
    n[3] = ysig_node_of(YSIG_OP_CUSTOM, 0, 2);
    n[4] = ysig_node_of(YSIG_OP_CUSTOM + 1, -1, -1);
    d = desc_of(n, 5);
    d.custom = cv;
    d.n_custom = 2;
    if (!open_desc(&g, &d)) { g_failures++; return; }
    ysig_reset(&g, 0);
    e = axis(MS, YIN_KIND_PEN, YIN_AXIS_POSITION, 0, 0, 0, 3, YIN_TIER_SIM);
    ysig_feed(&g, &e);
    CHECK(ysig_value_of(&g, 3)->v[0] == 9);
    {
        ysig_frame_in f = frame_at(0, 2 * MS, 0, NULL, 0);
        ysig_frame(&g, &f);
        CHECK(ysig_value_of(&g, 4)->v[0] == 7);
        ysig_reset(&g, 3 * MS);   /* a custom generator is zero at reset, as the built-ins */
        CHECK(ysig_value_of(&g, 4)->v[0] == 0);
    }
    /* a source before any sample: no value, tier UNKNOWN, no ring entry */
    g_where = "source before a sample";
    n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 0, 2);
    n[0].history = 8;
    d = desc_of(n, 1);
    if (!open_desc(&g, &d)) { g_failures++; return; }
    ysig_reset(&g, 0);
    CHECK(ysig_value_of(&g, 0)->tier == YIN_TIER_UNKNOWN && ysig_value_at(&g, 0, 0, &v) == 0);

    /* stateful sample-rate ops with no source upstream are refused (P2) */
    g_where = "no source upstream";
    n[0] = ysig_node_of(YSIG_OP_CHANNEL, -1, -1);
    n[1] = ysig_node_of(YSIG_OP_INTEGRATE, 0, -1);
    d = desc_of(n, 2);
    CHECK(!ysig_check(&d, err, sizeof err) && strstr(err, "no source upstream"));
    n[1] = ysig_node_of(YSIG_OP_DELAY, 0, -1);
    CHECK(!ysig_check(&d, err, sizeof err) && strstr(err, "no source upstream"));
    n[0] = ysig_node_of(YSIG_OP_NOISE, -1, -1);
    n[1] = ysig_node_of(YSIG_OP_LOWPASS, 0, -1);
    n[1].p[0] = 5;
    CHECK(!ysig_check(&d, err, sizeof err) && strstr(err, "no source upstream"));
    n[1] = ysig_node_of(YSIG_OP_VELOCITY, 0, -1);
    n[1].p[0] = 0.01;
    CHECK(!ysig_check(&d, err, sizeof err) && strstr(err, "no source upstream"));
    n[1] = ysig_node_of(YSIG_OP_SCALE, 0, -1);   /* stateless: fine */
    CHECK(ysig_check(&d, err, sizeof err));

    /* mean's weights summing to 0 */
    g_where = "mean weights";
    n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 0, 1);
    n[1] = ysig_node_of(YSIG_OP_MEAN, 0, 0);
    n[1].p[0] = 1; n[1].p[1] = -1;
    d = desc_of(n, 2);
    CHECK(!ysig_check(&d, err, sizeof err) && strstr(err, "sum to 0"));
    n[1].p[1] = 2;
    if (!open_desc(&g, &d)) { g_failures++; return; }
    CHECK(!ysig_set(&g, 1, 1, -1) && ysig_set(&g, 1, 1, 3));

    /* a hold armed while its test is true: from the arm (entry arm), or
     * after a fresh entry (entry fresh) (P5) */
    g_where = "hold entry";
    for (k = 0; k < 2; k++) {
        int nh;
        n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 0, 2);
        n[1] = ysig_node_of(YSIG_OP_DETECT, 0, -1);
        n[1].p[0] = YSIG_TEST_LE; n[1].p[1] = -1; n[1].p[2] = 10; n[1].p[4] = 0.5; n[1].p[6] = 1;
        n[1].p[12] = k;
        d = desc_of(n, 2);
        if (!open_desc(&g, &d)) { g_failures++; return; }
        ysig_reset(&g, 0);
        e = axis(MS, YIN_KIND_PEN, YIN_AXIS_POSITION, 0, 0, 0, 0, YIN_TIER_SIM);
        ysig_feed(&g, &e);
        ysig_arm(&g, 1, 2 * MS);
        {
            ysig_frame_in f = frame_at(1, 600 * MS, 0, NULL, 0);
            ysig_frame(&g, &f);
        }
        nh = ysig_detected(&g, h, 4);
        if (k == 0) CHECK(nh == 1 && h[0].t == 502 * MS);
        else {
            CHECK(nh == 0);
            /* out and in again: the hold counts from the entry */
            e = axis(700 * MS, YIN_KIND_PEN, YIN_AXIS_POSITION, 0, 20, 0, 0, YIN_TIER_SIM);
            ysig_feed(&g, &e);
            e = axis(710 * MS, YIN_KIND_PEN, YIN_AXIS_POSITION, 0, 0, 0, 0, YIN_TIER_SIM);
            ysig_feed(&g, &e);
            e = axis(1300 * MS, YIN_KIND_PEN, YIN_AXIS_POSITION, 0, 1, 0, 0, YIN_TIER_SIM);
            ysig_feed(&g, &e);
            CHECK(ysig_detected(&g, h, 4) == 1 && h[0].t == 1210 * MS);
        }
    }

    /* integrate's counts are a raw mouse's only (P6); a source's kind and
     * control do not change after open (P7) */
    g_where = "counts mode";
    n[0] = src_node(YIN_KIND_BOX, 1, 0, 1);
    n[1] = ysig_node_of(YSIG_OP_INTEGRATE, 0, -1);
    d = desc_of(n, 2);
    if (!open_desc(&g, &d)) { g_failures++; return; }
    ysig_reset(&g, 0);
    for (k = 1; k <= 1001; k++) {
        e = axis(k * MS, YIN_KIND_BOX, 1, 1, 0, 0, 0.4f, YIN_TIER_SIM);
        ysig_feed(&g, &e);
    }
    CHECK_NEAR(ysig_value_of(&g, 1)->v[0], (double)0.4f, 1e-9);
    CHECK(!ysig_set(&g, 0, 0, YIN_KIND_MOUSE) && !ysig_set(&g, 0, 1, YIN_AXIS_DELTA) && ysig_set(&g, 0, 2, 3));

    /* times at the ends of int64: no overflow (UBSan checks the run) */
    g_where = "extreme times";
    n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 0, 1);
    n[1] = ysig_node_of(YSIG_OP_VELOCITY, 0, -1);
    n[1].p[0] = 0.01;
    n[2] = ysig_node_of(YSIG_OP_DETECT, 0, -1);
    n[2].p[0] = YSIG_TEST_GE; n[2].p[2] = -1; n[2].p[4] = 1;
    n[3] = ysig_node_of(YSIG_OP_DELAY, 0, -1);
    n[3].p[0] = 1;
    n[4] = ysig_node_of(YSIG_OP_LOWPASS, 0, -1);
    n[4].p[0] = 1;
    n[5] = ysig_node_of(YSIG_OP_INTEGRATE, 0, -1);
    d = desc_of(n, 6);
    if (!open_desc(&g, &d)) { g_failures++; return; }
    ysig_reset(&g, INT64_MIN + 5);
    e = axis(INT64_MIN + 10, YIN_KIND_PEN, YIN_AXIS_POSITION, 0, 0, 0, 1, YIN_TIER_SIM);
    ysig_feed(&g, &e);
    {
        ysig_frame_in f = frame_at(1, INT64_MIN + 20, 0, NULL, 0);
        ysig_frame(&g, &f);
        ysig_reset(&g, INT64_MAX - 5);
        e.t = INT64_MAX - 1;
        ysig_feed(&g, &e);
        f = frame_at(2, INT64_MAX, 0, NULL, 0);
        ysig_frame(&g, &f);
        e.t = INT64_MAX;
        ysig_feed(&g, &e);
    }
    CHECK(ysig_detected(&g, h, 4) >= 0);
}

/* ---------------------------------------------------------------- v0.2 ops: mux, latch, distance, peak, all, silent */

#define PEN_E(t, dev, x, y, tier) axis((t), YIN_KIND_PEN, YIN_AXIS_POSITION, (dev), (float)(x), (float)(y), 0, (tier))

/* A row's value read back at its own time, from inside on_row, against the
 * value the row carries: a past read must work during a pass. */
static const ysig_graph* g_vat_graph;
static int g_vat_node, g_vat_checked, g_vat_bad;
static void on_row_vat(void* user, const ysig_row* r) {
    ysig_value v;
    (void)user;
    if (r->per_frame) return;
    {
        int rc = ysig_value_at(g_vat_graph, g_vat_node, r->value.t, &v);
        if (rc < 0) { g_vat_bad++; return; }
        if (rc == 0) return;     /* older than the rings: nothing to compare */
        g_vat_checked++;
        if (v.v[0] != r->value.v[0]) g_vat_bad++;
    }
}

static void test_mux(void) {
    ysig_node n[7];
    ysig_desc d;
    ysig_graph g;
    const ysig_value* v;
    ysig_value at;
    float tl[1];
    ysig_frame_in f;
    yin_event e;
    g_where = "mux";
    n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 1, 2);        /* a */
    n[1] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 2, 2);        /* b */
    n[2] = ysig_node_of(YSIG_OP_CHANNEL, -1, -1);                  /* a step sets the selector */
    n[3] = ysig_node_of(YSIG_OP_MUX, 0, 1); n[3].in[2] = 2; n[3].history = 64;
    n[4] = ysig_node_of(YSIG_OP_BIND, 3, -1);
    n[5] = ysig_node_of(YSIG_OP_RECORD, 3, -1);
    d = desc_of(n, 6);
    d.on_row = on_row_vat;
    if (!open_desc(&g, &d)) { g_failures++; return; }
    g_vat_graph = &g; g_vat_node = 3; g_vat_checked = g_vat_bad = 0;
    ysig_reset(&g, 1000 * MS);
    v = ysig_value_of(&g, 3);
    /* before any sample: b chosen (selector 0), no value yet */
    CHECK(v->t == 1000 * MS && v->tier == YIN_TIER_UNKNOWN && v->v[0] == 0);
    /* a sample of the input not chosen changes nothing on the output */
    e = PEN_E(1010 * MS, 1, 1, 2, YIN_TIER_1); ysig_feed(&g, &e);
    CHECK(v->t == 1000 * MS && v->tier == YIN_TIER_UNKNOWN && v->v[0] == 0);
    e = PEN_E(1012 * MS, 2, 3, 4, YIN_TIER_2); ysig_feed(&g, &e);
    CHECK(v->v[0] == 3 && v->v[1] == 4 && v->t == 1012 * MS && v->tier == YIN_TIER_2 && v->src == 1);
    /* a step's channel chooses a at a frame */
    tl[0] = 1;
    f = frame_at(1, 1020 * MS, 0, tl, 1);
    CHECK(ysig_frame(&g, &f) == 0);
    CHECK(v->v[0] == 1 && v->v[1] == 2 && v->t == 1010 * MS && v->tier == YIN_TIER_1 && v->src == 0);
    CHECK(g_ch[0] == 1 && g_ch[1] == 2);
    /* a value at a past time: the selector's frame ring and each input's
     * sample ring, so before the frame's onset b (and its time), after it a */
    CHECK(ysig_value_at(&g, 3, 1015 * MS, &at) == 1 && at.v[0] == 3 && at.t == 1012 * MS);
    CHECK(ysig_value_at(&g, 3, 1025 * MS, &at) == 1 && at.v[0] == 1 && at.t == 1010 * MS && at.src == 0);
    /* older than the rings: rc 0, the oldest given */
    CHECK(ysig_value_at(&g, 3, 1005 * MS, &at) == 0);
    /* out of order across sources: a's report arrives after b's newer one */
    tl[0] = 0;
    f = frame_at(2, 1040 * MS, 0, tl, 1);
    ysig_frame(&g, &f);                                        /* back to b */
    e = PEN_E(1041 * MS, 2, 9, 9, YIN_TIER_SIM); ysig_feed(&g, &e);
    e = PEN_E(1030 * MS, 1, 7, 7, YIN_TIER_SIM); ysig_feed(&g, &e);
    CHECK(v->v[0] == 9 && v->t == 1041 * MS);          /* a's late report, not chosen, moves nothing */
    tl[0] = 1;
    f = frame_at(3, 1050 * MS, 0, tl, 1);
    ysig_frame(&g, &f);
    CHECK(v->v[0] == 7 && v->t == 1030 * MS && v->device == 1);
    CHECK(g_vat_bad == 0 && g_vat_checked > 0);
    /* selector NaN counts as zero: b */
    tl[0] = NAN;
    f = frame_at(4, 1060 * MS, 0, tl, 1);
    ysig_frame(&g, &f);
    CHECK(v->v[0] == 9);
    /* extreme times: a stamp at the ends of int64 */
    e = PEN_E(INT64_MAX, 2, 11, 11, YIN_TIER_SIM); ysig_feed(&g, &e);
    CHECK(v->v[0] == 11 && v->t == INT64_MAX);
    ysig_reset_trial(&g, 2000 * MS);   /* no adapters: every node */
    CHECK(v->t == 2000 * MS && v->v[0] == 0 && v->tier == YIN_TIER_UNKNOWN);
    /* a detect's state as the selector, within one sample's pass: the
     * crossing sample itself is the first chosen */
    n[2] = ysig_node_of(YSIG_OP_DETECT, 0, -1);                 /* a.x >= 5 */
    n[2].p[YSIG_DETECT_LEVEL] = 5;
    n[2].p[YSIG_DETECT_REPEAT] = 1;
    n[3] = ysig_node_of(YSIG_OP_MUX, 0, 1); n[3].in[2] = 2;
    d = desc_of(n, 4);
    d.on_row = NULL;
    if (!open_desc(&g, &d)) { g_failures++; return; }
    ysig_reset(&g, 0);
    v = ysig_value_of(&g, 3);
    e = PEN_E(10 * MS, 2, 3, 3, YIN_TIER_SIM); ysig_feed(&g, &e);
    CHECK(v->v[0] == 3);
    e = PEN_E(11 * MS, 1, 4, 0, YIN_TIER_SIM); ysig_feed(&g, &e);
    CHECK(v->v[0] == 3);
    e = PEN_E(12 * MS, 1, 6, 0, YIN_TIER_SIM); ysig_feed(&g, &e);
    CHECK(v->v[0] == 6 && v->t == 12 * MS);
    e = PEN_E(13 * MS, 1, 1, 0, YIN_TIER_SIM); ysig_feed(&g, &e);
    CHECK(v->v[0] == 3 && v->t == 10 * MS);
    /* a view chosen at a planned onset ahead of the poll: a mux passes the
     * view's value as of the poll, the velocity read at the poll */
    {
        ysig_node m[5];
        m[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 1, 2);
        m[1] = ysig_node_of(YSIG_OP_VELOCITY, 0, -1); m[1].p[0] = 0.01;
        m[2] = ysig_node_of(YSIG_OP_OFFSET, 0, -1);
        m[3] = ysig_node_of(YSIG_OP_CHANNEL, -1, -1);
        m[4] = ysig_node_of(YSIG_OP_MUX, 1, 2); m[4].in[2] = 3;
        d = desc_of(m, 5);
        if (!open_desc(&g, &d)) { g_failures++; return; }
        ysig_reset(&g, 0);
        for (int i = 0; i < 40; i++) {
            e = PEN_E(i * MS, 1, i, 0, YIN_TIER_SIM);
            ysig_feed(&g, &e);
        }
        tl[0] = 1;
        f = frame_at(1, 60 * MS, 0, tl, 1);
        f.poll_ns = 39 * MS;                                    /* still moving at the poll */
        ysig_frame(&g, &f);
        CHECK_NEAR(ysig_value_of(&g, 4)->v[0], 1000, 1e-6);
        f = frame_at(2, 75 * MS, 0, tl, 1);
        f.poll_ns = 60 * MS;                                    /* silent for the lag up to the poll */
        ysig_frame(&g, &f);
        CHECK(ysig_value_of(&g, 4)->v[0] == 0);
    }
}

static void test_latch(void) {
    ysig_node n[8];
    ysig_desc d;
    ysig_graph g;
    const ysig_value* v;
    ysig_value at;
    ysig_hit h[4];
    float tl[1];
    ysig_frame_in f;
    yin_event e;
    g_where = "latch";
    n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 1, 2);
    n[1] = ysig_node_of(YSIG_OP_DETECT, 0, -1);                  /* x >= 5, the sample that crossed */
    n[1].p[YSIG_DETECT_LEVEL] = 5;
    n[1].p[YSIG_DETECT_OUT] = YSIG_OUT_HIT;
    n[2] = ysig_node_of(YSIG_OP_LATCH, 0, 1); n[2].history = 64;
    n[3] = ysig_node_of(YSIG_OP_RECORD, 2, -1);
    d = desc_of(n, 4);
    d.on_row = on_row_vat;
    if (!open_desc(&g, &d)) { g_failures++; return; }
    g_vat_graph = &g; g_vat_node = 2; g_vat_checked = g_vat_bad = 0;
    ysig_reset(&g, 5 * MS);
    v = ysig_value_of(&g, 2);
    /* before any sample, a pass-through of the held (empty) input */
    CHECK(v->t == 5 * MS && v->tier == YIN_TIER_UNKNOWN);
    e = PEN_E(10 * MS, 1, 1, 7, YIN_TIER_2); ysig_feed(&g, &e);
    CHECK(v->v[0] == 1 && v->v[1] == 7 && v->t == 10 * MS && v->tier == YIN_TIER_2);
    /* the crossing sample is the value and time held, and is the hit's */
    e = PEN_E(20 * MS, 1, 6, 8, YIN_TIER_2); ysig_feed(&g, &e);
    CHECK(v->v[0] == 6 && v->v[1] == 8 && v->t == 20 * MS);
    CHECK(ysig_detected(&g, h, 4) == 1 && h[0].t == 20 * MS && h[0].value.v[0] == v->v[0] && h[0].value.v[1] == v->v[1]);
    e = PEN_E(30 * MS, 1, 9, 9, YIN_TIER_2); ysig_feed(&g, &e);
    CHECK(v->v[0] == 6 && v->t == 20 * MS);
    f = frame_at(1, 40 * MS, 0, NULL, 0);
    ysig_frame(&g, &f);
    CHECK(v->v[0] == 6 && v->t == 20 * MS);
    /* past values: the ring, read inside on_row too */
    CHECK(g_vat_bad == 0 && g_vat_checked == 3);
    CHECK(ysig_value_at(&g, 2, 15 * MS, &at) == 1 && at.v[0] == 1 && at.t == 10 * MS);
    CHECK(ysig_value_at(&g, 2, 25 * MS, &at) == 1 && at.v[0] == 6 && at.t == 20 * MS);
    /* a reset lets go */
    ysig_reset(&g, 50 * MS);
    CHECK(v->t == 50 * MS && v->v[0] == 0);
    e = PEN_E(55 * MS, 1, 2, 2, YIN_TIER_2); ysig_feed(&g, &e);
    CHECK(v->v[0] == 2 && v->t == 55 * MS);
    /* a value past the level at the reset: the hit is held (no ready), so
     * no rise, and the input goes on passing */
    ysig_reset(&g, 60 * MS);
    {
        ysig_node m[5];
        /* a hold that a still hand completes at a frame: the trigger rises
         * in the frame's pass and the latch takes the hit's value and time */
        m[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 1, 2);
        m[1] = ysig_node_of(YSIG_OP_DETECT, 0, -1);
        m[1].p[YSIG_DETECT_LEVEL] = 5; m[1].p[YSIG_DETECT_HOLD] = 0.05;
        m[1].p[YSIG_DETECT_OUT] = YSIG_OUT_HIT;
        m[2] = ysig_node_of(YSIG_OP_LATCH, 0, 1); m[2].history = 16;
        m[3] = ysig_node_of(YSIG_OP_RECORD, 2, -1); m[3].p[0] = 1;
        d = desc_of(m, 4);
        if (!open_desc(&g, &d)) { g_failures++; return; }
        ysig_reset(&g, 0);
        v = ysig_value_of(&g, 2);
        e = PEN_E(10 * MS, 1, 6, 1, YIN_TIER_2); ysig_feed(&g, &e);
        CHECK(v->v[0] == 6 && v->t == 10 * MS);
        e = PEN_E(30 * MS, 1, 7, 2, YIN_TIER_2); ysig_feed(&g, &e);
        CHECK(v->v[0] == 7);                                   /* not completed: passes */
        f = frame_at(1, 70 * MS, 0, NULL, 0);                  /* still hand: 10 + 50 ms passed at the onset */
        ysig_frame(&g, &f);
        CHECK(ysig_detected(&g, h, 4) == 1 && h[0].t == 60 * MS && h[0].value.v[0] == 6);
        CHECK(v->v[0] == 6 && v->v[1] == 1 && v->t == 60 * MS);    /* the recorded hit's value and time */
        CHECK(ysig_value_at(&g, 2, 65 * MS, &at) == 1 && at.v[0] == 6 && at.t == 60 * MS);   /* the frame's capture is in the ring */
        e = PEN_E(80 * MS, 1, 1, 1, YIN_TIER_2); ysig_feed(&g, &e);
        CHECK(v->v[0] == 6 && v->t == 60 * MS);                   /* held */
        /* the trigger on a different input takes the held value of the latch's input */
        m[1] = ysig_node_of(YSIG_OP_CHANNEL, -1, -1);
        m[3] = ysig_node_of(YSIG_OP_RECORD, 2, -1);
        d = desc_of(m, 4);
        if (!open_desc(&g, &d)) { g_failures++; return; }
        ysig_reset(&g, 0);
        v = ysig_value_of(&g, 2);
        e = PEN_E(10 * MS, 1, 3, 4, YIN_TIER_1); ysig_feed(&g, &e);
        tl[0] = 1;
        f = frame_at(1, 30 * MS, 0, tl, 1);
        ysig_frame(&g, &f);
        CHECK(v->v[0] == 3 && v->t == 10 * MS && v->tier == YIN_TIER_1);
        e = PEN_E(40 * MS, 1, 5, 5, YIN_TIER_1); ysig_feed(&g, &e);
        CHECK(v->v[0] == 3 && v->t == 10 * MS);
        /* a channel's rise at a frame holds the reset's value when no
         * sample came (tier unknown, as the source before its first) */
        tl[0] = 1;
        ysig_reset(&g, 100 * MS);
        v = ysig_value_of(&g, 2);
        CHECK(v->t == 100 * MS);
        f = frame_at(2, 120 * MS, 0, tl, 1);
        ysig_frame(&g, &f);
        e = PEN_E(121 * MS, 1, 8, 8, YIN_TIER_1); ysig_feed(&g, &e);
        CHECK(v->t == 100 * MS && v->v[0] == 0 && v->tier == YIN_TIER_UNKNOWN);
        /* a trigger already true at a reset is no rise: fall and rise again */
        m[1] = ysig_node_of(YSIG_OP_DETECT, 0, -1);
        m[1].p[YSIG_DETECT_TEST] = YSIG_TEST_LE; m[1].p[YSIG_DETECT_LEVEL] = 1;
        d = desc_of(m, 3);
        if (!open_desc(&g, &d)) { g_failures++; return; }
        ysig_reset(&g, 0);
        v = ysig_value_of(&g, 2);
        e = PEN_E(10 * MS, 1, 0.5, 0, YIN_TIER_SIM); ysig_feed(&g, &e);
        CHECK(v->v[0] == 0.5);                             /* the test true: passes, no rise yet */
        e = PEN_E(20 * MS, 1, 3, 0, YIN_TIER_SIM); ysig_feed(&g, &e);
        e = PEN_E(30 * MS, 1, 0.25, 0, YIN_TIER_SIM); ysig_feed(&g, &e);
        CHECK(v->v[0] == 0.25 && v->t == 30 * MS);           /* the rise's sample, the crossing */
        e = PEN_E(40 * MS, 1, 3, 0, YIN_TIER_SIM); ysig_feed(&g, &e);
        CHECK(v->v[0] == 0.25);
        m[1] = ysig_node_of(YSIG_OP_CHANNEL, -1, -1);
        d = desc_of(m, 3);
        if (!open_desc(&g, &d)) { g_failures++; return; }
        ysig_reset(&g, 200 * MS);
        {
            /* selector zero to NaN is no rise; NaN to 1 is */
            float tn[1];
            v = ysig_value_of(&g, 2);
            tn[0] = NAN;
            f = frame_at(3, 210 * MS, 0, tn, 1);
            ysig_frame(&g, &f);
            e = PEN_E(211 * MS, 1, 2, 2, YIN_TIER_1); ysig_feed(&g, &e);
            CHECK(v->v[0] == 2 && v->t == 211 * MS);
            tn[0] = 2.5f;
            f = frame_at(4, 220 * MS, 0, tn, 1);
            ysig_frame(&g, &f);
            CHECK(v->v[0] == 2 && v->t == 211 * MS);
        }
    }
    /* out of order: two devices on an any-device source; a late report that
     * crosses is the sample that made the trigger true */
    n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 0, 2);
    n[1] = ysig_node_of(YSIG_OP_DETECT, 0, -1);
    n[1].p[YSIG_DETECT_LEVEL] = 5;
    n[2] = ysig_node_of(YSIG_OP_LATCH, 0, 1);
    d = desc_of(n, 3);
    d.on_row = NULL;
    if (!open_desc(&g, &d)) { g_failures++; return; }
    ysig_reset(&g, 0);
    v = ysig_value_of(&g, 2);
    e = PEN_E(50 * MS, 1, 1, 0, YIN_TIER_SIM); ysig_feed(&g, &e);
    e = PEN_E(40 * MS, 2, 9, 0, YIN_TIER_SIM); ysig_feed(&g, &e);
    CHECK(v->v[0] == 9 && v->t == 40 * MS && v->device == 2);
    /* extreme times */
    ysig_reset(&g, INT64_MIN + 3);
    e = PEN_E(INT64_MIN + 5, 1, 9, 0, YIN_TIER_SIM); ysig_feed(&g, &e);
    CHECK(v->v[0] == 9 && v->t == INT64_MIN + 5);
}

static void test_distance(void) {
    ysig_node n[8];
    ysig_desc d;
    ysig_graph g;
    const ysig_value* v;
    ysig_value at;
    float tl[3];
    ysig_frame_in f;
    yin_event e;
    g_where = "distance";
    n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 1, 2);
    n[1] = ysig_node_of(YSIG_OP_DISTANCE, 0, -1);
    n[1].p[YSIG_DISTANCE_POINT_X] = 3;
    n[1].p[YSIG_DISTANCE_POINT_Y] = 4;
    /* the point from a second input: a channel pair, frame-updated, so a
     * past read needs the selector's and the point's rings */
    n[2] = ysig_node_of(YSIG_OP_CHANNEL, -1, -1); n[2].p[0] = 0;
    n[3] = ysig_node_of(YSIG_OP_CHANNEL, -1, -1); n[3].p[0] = 1;
    n[4] = ysig_node_of(YSIG_OP_JOIN, 2, 3);
    n[5] = ysig_node_of(YSIG_OP_DISTANCE, 0, 4); n[5].history = 32;
    n[6] = ysig_node_of(YSIG_OP_RECORD, 5, -1);
    d = desc_of(n, 7);
    d.on_row = on_row_vat;
    if (!open_desc(&g, &d)) { g_failures++; return; }
    g_vat_graph = &g; g_vat_node = 5; g_vat_checked = g_vat_bad = 0;
    ysig_reset(&g, 0);
    v = ysig_value_of(&g, 1);
    /* before any sample: the distance of the zero value, tier unknown */
    CHECK_NEAR(v->v[0], 5, 0);
    CHECK(v->tier == YIN_TIER_UNKNOWN && v->t == 0);
    e = PEN_E(10 * MS, 1, 0, 0, YIN_TIER_1); ysig_feed(&g, &e);
    CHECK(v->t == 10 * MS && v->tier == YIN_TIER_1 && v->lanes == 1);
    tl[0] = 3; tl[1] = 4; tl[2] = 0;
    f = frame_at(1, 20 * MS, 0, tl, 3);
    ysig_frame(&g, &f);
    CHECK_NEAR(ysig_value_of(&g, 5)->v[0], 5, 0);
    e = PEN_E(25 * MS, 1, 3, 0, YIN_TIER_1); ysig_feed(&g, &e);
    CHECK_NEAR(ysig_value_of(&g, 5)->v[0], 4, 0);
    /* a past read before the frame moved the point: (0,0) vs (0,0)... */
    CHECK(ysig_value_at(&g, 5, 15 * MS, &at) == 1 && at.v[0] == 0);
    CHECK(ysig_value_at(&g, 5, 25 * MS, &at) == 1 && at.v[0] == 4);
    CHECK(g_vat_bad == 0 && g_vat_checked == 2);
    /* a 3-lane value */
    {
        ysig_node m[2];
        m[0] = src_node(YIN_KIND_GAMEPAD, 0, 0, 3);
        m[0].p[YSIG_SOURCE_FIELDS] = 7;
        m[1] = ysig_node_of(YSIG_OP_DISTANCE, 0, -1);
        m[1].p[YSIG_DISTANCE_POINT_Z] = -2;
        d = desc_of(m, 2);
        if (!open_desc(&g, &d)) { g_failures++; return; }
        ysig_reset(&g, 0);
        e = axis(5 * MS, YIN_KIND_GAMEPAD, 0, 1, 1, 2, 2, YIN_TIER_SIM);
        ysig_feed(&g, &e);
        CHECK_NEAR(ysig_value_of(&g, 1)->v[0], 4.58257569495584, 1e-12);
    }
    /* extreme: a huge coordinate gives a finite or infinite distance, no trap */
    {
        ysig_node m[2];
        m[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 1, 2);
        m[1] = ysig_node_of(YSIG_OP_DISTANCE, 0, -1);
        d = desc_of(m, 2);
        if (!open_desc(&g, &d)) { g_failures++; return; }
        ysig_reset(&g, INT64_MAX);
        e = PEN_E(INT64_MAX, 1, 1e30f, 1e30f, YIN_TIER_SIM);
        ysig_feed(&g, &e);
        CHECK(ysig_value_of(&g, 1)->v[0] > 1e30);
        CHECK(ysig_value_of(&g, 1)->t == INT64_MAX);
    }
}

static void test_peak(void) {
    ysig_node n[7];
    ysig_desc d;
    ysig_graph g;
    const ysig_value* v;
    ysig_value at;
    float tl[1];
    ysig_frame_in f;
    yin_event e;
    g_where = "peak";
    n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 1, 2);
    n[1] = src_node(YIN_KIND_BOX, 0, 0, 1);                    /* a gate from another device */
    n[2] = ysig_node_of(YSIG_OP_PEAK, 0, -1);                  /* lane 0, no gate */
    n[2].history = 32;
    n[3] = ysig_node_of(YSIG_OP_PEAK, 0, -1);                  /* the norm */
    n[3].p[YSIG_PEAK_LANE] = -1;
    n[4] = ysig_node_of(YSIG_OP_PEAK, 0, 1);                   /* gated by the box */
    n[4].p[YSIG_PEAK_LANE] = 1;
    n[5] = ysig_node_of(YSIG_OP_RECORD, 2, -1);
    d = desc_of(n, 6);
    d.on_row = on_row_vat;
    if (!open_desc(&g, &d)) { g_failures++; return; }
    g_vat_graph = &g; g_vat_node = 2; g_vat_checked = g_vat_bad = 0;
    ysig_reset(&g, 3 * MS);
    v = ysig_value_of(&g, 2);
    CHECK(v->v[0] == 0 && v->t == 3 * MS && v->lanes == 1 && v->tier == YIN_TIER_UNKNOWN);
    /* NaN as the first sample is no peak: the output keeps its reset */
    e = PEN_E(4 * MS, 1, NAN, 0, YIN_TIER_2); ysig_feed(&g, &e);
    CHECK(v->v[0] == 0 && v->t == 3 * MS);
    /* a first value that is negative is the peak: the largest so far */
    e = PEN_E(10 * MS, 1, -2, 3, YIN_TIER_2); ysig_feed(&g, &e);
    CHECK(v->v[0] == -2 && v->t == 10 * MS && v->tier == YIN_TIER_2);
    e = PEN_E(20 * MS, 1, 5, 0, YIN_TIER_SIM); ysig_feed(&g, &e);
    CHECK(v->v[0] == 5 && v->t == 20 * MS);
    e = PEN_E(30 * MS, 1, 4, 4, YIN_TIER_SIM); ysig_feed(&g, &e);
    CHECK(v->v[0] == 5 && v->t == 20 * MS);                 /* the peak sample's time, not the newest */
    CHECK_NEAR(ysig_value_of(&g, 3)->v[0], 5.656854249492381, 1e-12);
    CHECK(ysig_value_of(&g, 3)->t == 30 * MS);
    CHECK(ysig_value_at(&g, 2, 25 * MS, &at) == 1 && at.v[0] == 5 && at.t == 20 * MS);
    CHECK(ysig_value_at(&g, 2, 15 * MS, &at) == 1 && at.v[0] == -2);
    CHECK(g_vat_bad == 0 && g_vat_checked == 4);
    /* NaN is no peak */
    e = PEN_E(31 * MS, 1, NAN, 0, YIN_TIER_SIM); ysig_feed(&g, &e);
    CHECK(v->v[0] == 5 && v->t == 20 * MS);
    /* the gate (node 4, lane 1): zero, so it holds and sets nothing */
    CHECK(ysig_value_of(&g, 4)->v[0] == 0 && ysig_value_of(&g, 4)->t == 3 * MS);
    /* the gate rises from a report of another source: the peak restarts
     * from the value held then, at that value's time */
    e = axis(32 * MS, YIN_KIND_BOX, 0, 2, 0, 0, 1, YIN_TIER_SIM); ysig_feed(&g, &e);
    v = ysig_value_of(&g, 4);
    CHECK(v->v[0] == 0 && v->t == 31 * MS);               /* lane 1 of (NaN, 0) */
    e = PEN_E(40 * MS, 1, 0, 2, YIN_TIER_SIM); ysig_feed(&g, &e);
    CHECK(v->v[0] == 2 && v->t == 40 * MS);
    e = PEN_E(50 * MS, 1, 0, 1, YIN_TIER_SIM); ysig_feed(&g, &e);
    CHECK(v->v[0] == 2 && v->t == 40 * MS);
    /* an out-of-order report: a later t already kept; two sources */
    e = axis(45 * MS, YIN_KIND_BOX, 0, 2, 0, 0, 1, YIN_TIER_SIM); ysig_feed(&g, &e);
    CHECK(v->v[0] == 2);
    /* the gate falls: hold */
    e = axis(55 * MS, YIN_KIND_BOX, 0, 2, 0, 0, 0, YIN_TIER_SIM); ysig_feed(&g, &e);
    e = PEN_E(60 * MS, 1, 0, 9, YIN_TIER_SIM); ysig_feed(&g, &e);
    CHECK(v->v[0] == 2 && v->t == 40 * MS);
    /* it rises again: a fresh peak, not the old 2 */
    e = axis(65 * MS, YIN_KIND_BOX, 0, 2, 0, 0, 1, YIN_TIER_SIM); ysig_feed(&g, &e);
    CHECK(v->v[0] == 9 && v->t == 60 * MS);
    e = PEN_E(70 * MS, 1, 0, 3, YIN_TIER_SIM); ysig_feed(&g, &e);
    CHECK(v->v[0] == 9);
    /* a new rise forgets the old peak, also a larger one */
    e = axis(72 * MS, YIN_KIND_BOX, 0, 2, 0, 0, 0, YIN_TIER_SIM); ysig_feed(&g, &e);
    e = PEN_E(74 * MS, 1, 0, 1, YIN_TIER_SIM); ysig_feed(&g, &e);
    e = axis(75 * MS, YIN_KIND_BOX, 0, 2, 0, 0, 1, YIN_TIER_SIM); ysig_feed(&g, &e);
    CHECK(v->v[0] == 1 && v->t == 74 * MS);
    e = PEN_E(76 * MS, 1, 0, 9, YIN_TIER_SIM); ysig_feed(&g, &e);
    CHECK(v->v[0] == 9 && v->t == 76 * MS);
    /* a reset clears the peak; a frame in between changes nothing */
    f = frame_at(1, 80 * MS, 0, NULL, 0);
    ysig_frame(&g, &f);
    CHECK(v->v[0] == 9 && v->t == 76 * MS);
    ysig_reset(&g, 100 * MS);
    v = ysig_value_of(&g, 2);
    CHECK(v->v[0] == 0 && v->t == 100 * MS);
    e = PEN_E(110 * MS, 1, 1, 1, YIN_TIER_SIM); ysig_feed(&g, &e);
    CHECK(v->v[0] == 1 && v->t == 110 * MS);
    /* a gate from a channel: it rises at a frame, restarting at the held
     * value, and a value at a past time sees the frame's restart */
    {
        ysig_node m[4];
        m[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 1, 2);
        m[1] = ysig_node_of(YSIG_OP_CHANNEL, -1, -1);
        m[2] = ysig_node_of(YSIG_OP_PEAK, 0, 1); m[2].history = 16;
        d = desc_of(m, 3);
        d.on_row = NULL;
        if (!open_desc(&g, &d)) { g_failures++; return; }
        ysig_reset(&g, 0);
        v = ysig_value_of(&g, 2);
        e = PEN_E(10 * MS, 1, 8, 0, YIN_TIER_SIM); ysig_feed(&g, &e);
        CHECK(v->v[0] == 0 && v->t == 0);                  /* the gate is shut */
        tl[0] = 1;
        f = frame_at(1, 20 * MS, 0, tl, 1);
        ysig_frame(&g, &f);
        CHECK(v->v[0] == 8 && v->t == 10 * MS);          /* restart at the held value */
        CHECK(ysig_value_at(&g, 2, 25 * MS, &at) == 1 && at.v[0] == 8 && at.t == 10 * MS);   /* before any sample after it */
        e = PEN_E(30 * MS, 1, 3, 0, YIN_TIER_SIM); ysig_feed(&g, &e);
        CHECK(v->v[0] == 8);
        e = PEN_E(40 * MS, 1, 12, 0, YIN_TIER_SIM); ysig_feed(&g, &e);
        CHECK(v->v[0] == 12 && v->t == 40 * MS);
        CHECK(ysig_value_at(&g, 2, 15 * MS, &at) == 1 && at.v[0] == 8 && at.t == 10 * MS);
        CHECK(ysig_value_at(&g, 2, 35 * MS, &at) == 1 && at.v[0] == 8);
        /* a frame's change of a held input is no sample: a mux that
         * switches to a larger value at a frame leaves the peak alone */
        {
            ysig_node q[6];
            float t2[1];
            q[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 1, 2);
            q[1] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 2, 2);
            q[2] = ysig_node_of(YSIG_OP_CHANNEL, -1, -1);
            q[3] = ysig_node_of(YSIG_OP_MUX, 0, 1); q[3].in[2] = 2;
            q[4] = ysig_node_of(YSIG_OP_PEAK, 3, -1);
            d = desc_of(q, 5);
            if (!open_desc(&g, &d)) { g_failures++; return; }
            ysig_reset(&g, 0);
            v = ysig_value_of(&g, 4);
            e = PEN_E(10 * MS, 1, 20, 0, YIN_TIER_SIM); ysig_feed(&g, &e);   /* a, not chosen */
            e = PEN_E(11 * MS, 2, 3, 0, YIN_TIER_SIM); ysig_feed(&g, &e);    /* b, chosen */
            CHECK(v->v[0] == 3);
            t2[0] = 1;                                                      /* a chosen at a frame: 20, a held value */
            f = frame_at(1, 20 * MS, 0, t2, 1);
            ysig_frame(&g, &f);
            CHECK(v->v[0] == 3 && v->t == 11 * MS);                         /* no sample: no change */
            e = PEN_E(21 * MS, 1, 25, 0, YIN_TIER_SIM); ysig_feed(&g, &e);    /* a's next sample is one */
            CHECK(v->v[0] == 25 && v->t == 21 * MS);
        }
    }
    /* extreme times in a peak's data */
    {
        ysig_node m[2];
        m[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 1, 2);
        m[1] = ysig_node_of(YSIG_OP_PEAK, 0, -1);
        d = desc_of(m, 2);
        if (!open_desc(&g, &d)) { g_failures++; return; }
        ysig_reset(&g, INT64_MIN + 1);
        e = PEN_E(INT64_MAX, 1, 2, 0, YIN_TIER_SIM); ysig_feed(&g, &e);
        CHECK(ysig_value_of(&g, 1)->v[0] == 2 && ysig_value_of(&g, 1)->t == INT64_MAX);
    }
}

static void test_all(void) {
    ysig_node n[8];
    ysig_desc d;
    ysig_graph g;
    const ysig_value* v;
    ysig_value at;
    ysig_hit h[4];
    float tl[2];
    ysig_frame_in f;
    yin_event e;
    g_where = "all";
    /* the bimanual reach's "in the target and slow": two detects into an
     * all, and one hold detect on it */
    n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 1, 2);
    n[1] = ysig_node_of(YSIG_OP_DETECT, 0, -1);                 /* x >= 5 */
    n[1].p[YSIG_DETECT_LEVEL] = 5;
    n[2] = ysig_node_of(YSIG_OP_DETECT, 0, -1);                 /* y >= 3 */
    n[2].p[YSIG_DETECT_LANE] = 1; n[2].p[YSIG_DETECT_LEVEL] = 3;
    n[3] = ysig_node_of(YSIG_OP_ALL, 1, 2);
    n[4] = ysig_node_of(YSIG_OP_DETECT, 3, -1);                 /* all for 50 ms */
    n[4].p[YSIG_DETECT_LEVEL] = 0.5; n[4].p[YSIG_DETECT_HOLD] = 0.05;
    d = desc_of(n, 5);
    if (!open_desc(&g, &d)) { g_failures++; return; }
    ysig_reset(&g, 2 * MS);
    v = ysig_value_of(&g, 3);
    CHECK(v->v[0] == 0 && v->t == 2 * MS);
    /* the state lanes carry their value's time and tier through */
    e = PEN_E(10 * MS, 1, 5, 2, YIN_TIER_2); ysig_feed(&g, &e);
    CHECK(v->v[0] == 0 && v->t == 10 * MS && v->tier == YIN_TIER_2);
    e = PEN_E(20 * MS, 1, 6, 3, YIN_TIER_SIM); ysig_feed(&g, &e);
    CHECK(v->v[0] == 1 && v->t == 20 * MS);
    e = PEN_E(30 * MS, 1, 6, 4, YIN_TIER_SIM); ysig_feed(&g, &e);
    e = PEN_E(40 * MS, 1, 1, 9, YIN_TIER_SIM); ysig_feed(&g, &e);
    CHECK(v->v[0] == 0);
    CHECK(ysig_detected(&g, h, 4) == 2);                       /* the two tests' own hits, once each */
    /* a hold on both tests held: the single detect fires at start + hold */
    e = PEN_E(50 * MS, 1, 7, 7, YIN_TIER_SIM); ysig_feed(&g, &e);
    CHECK(ysig_detected(&g, h, 4) == 0);
    e = PEN_E(90 * MS, 1, 7, 7, YIN_TIER_SIM); ysig_feed(&g, &e);
    e = PEN_E(101 * MS, 1, 7, 7, YIN_TIER_SIM); ysig_feed(&g, &e);
    CHECK(ysig_detected(&g, h, 4) == 1 && h[0].t == 100 * MS && h[0].node == 4);
    /* a frame-updated input: channel into all, past value through rings */
    n[1] = ysig_node_of(YSIG_OP_CHANNEL, -1, -1); n[1].p[0] = 0;
    n[2] = ysig_node_of(YSIG_OP_CHANNEL, -1, -1); n[2].p[0] = 1;
    n[3] = ysig_node_of(YSIG_OP_ALL, 1, 2); n[3].history = 16;
    d = desc_of(n, 4);
    if (!open_desc(&g, &d)) { g_failures++; return; }
    ysig_reset(&g, 0);
    v = ysig_value_of(&g, 3);
    tl[0] = 1; tl[1] = 1;
    f = frame_at(1, 20 * MS, 0, tl, 2);
    ysig_frame(&g, &f);
    CHECK(v->v[0] == 1);
    tl[1] = 0;
    f = frame_at(2, 40 * MS, 0, tl, 2);
    ysig_frame(&g, &f);
    CHECK(v->v[0] == 0);
    CHECK(ysig_value_at(&g, 3, 30 * MS, &at) == 1 && at.v[0] == 1);
    CHECK(ysig_value_at(&g, 3, 50 * MS, &at) == 1 && at.v[0] == 0);
    /* NaN is not nonzero */
    tl[0] = NAN; tl[1] = 1;
    f = frame_at(3, 60 * MS, 0, tl, 2);
    ysig_frame(&g, &f);
    CHECK(v->v[0] == 0);
    /* all's inputs may have any lanes: lane 0 only */
    {
        ysig_node m[3];
        m[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 1, 2);
        m[1] = ysig_node_of(YSIG_OP_ALL, 0, 0);
        d = desc_of(m, 2);
        if (!open_desc(&g, &d)) { g_failures++; return; }
        ysig_reset(&g, 0);
        e = PEN_E(1 * MS, 1, 0, 5, YIN_TIER_SIM); ysig_feed(&g, &e);
        CHECK(ysig_value_of(&g, 1)->v[0] == 0);
        e = PEN_E(2 * MS, 1, 2, 0, YIN_TIER_SIM); ysig_feed(&g, &e);
        CHECK(ysig_value_of(&g, 1)->v[0] == 1);
    }
}

static void test_silent(void) {
    ysig_node n[5];
    ysig_desc d;
    ysig_graph g;
    const ysig_value* v;
    ysig_value at;
    ysig_frame_in f;
    yin_event e;
    ysig_stats st;
    g_where = "silent";
    n[0] = src_node(YIN_KIND_EYE, 0, 1, 2);
    n[1] = ysig_node_of(YSIG_OP_SILENT, 0, -1);
    n[1].p[YSIG_SILENT_AFTER] = 0.05;
    n[1].history = 64;
    n[2] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 9, 2);   /* another device */
    n[3] = ysig_node_of(YSIG_OP_SILENT, 2, -1);
    n[3].p[YSIG_SILENT_AFTER] = 0.01;
    d = desc_of(n, 4);
    if (!open_desc(&g, &d)) { g_failures++; return; }
    ysig_reset(&g, 100 * MS);
    v = ysig_value_of(&g, 1);
    /* before any sample: counted from the reset, at the poll */
    CHECK(v->v[0] == 0);
    f = frame_at(1, 200 * MS, 0, NULL, 0);
    f.poll_ns = 130 * MS;            /* the onset is planned; input known to 130 */
    ysig_frame(&g, &f);
    CHECK(v->v[0] == 0);
    f.poll_ns = 151 * MS;
    ysig_frame(&g, &f);
    CHECK(v->v[0] == 1 && v->t == 151 * MS);
    /* a sample arrives: 0 at once, at its t */
    e = axis(160 * MS, YIN_KIND_EYE, 0, 1, 3, 4, 0, YIN_TIER_2);
    ysig_feed(&g, &e);
    CHECK(v->v[0] == 0 && v->t == 160 * MS && v->tier == YIN_TIER_2);
    /* the gap to a planned onset is not silence: only the poll's time counts */
    f = frame_at(2, 400 * MS, 0, NULL, 0);
    f.poll_ns = 200 * MS;
    ysig_frame(&g, &f);
    CHECK(v->v[0] == 0);
    f.poll_ns = 209 * MS;
    ysig_frame(&g, &f);
    CHECK(v->v[0] == 0);
    f.poll_ns = 210 * MS;
    ysig_frame(&g, &f);
    CHECK(v->v[0] == 1);
    /* a past read judges at that time, at most at the last poll */
    CHECK(ysig_value_at(&g, 1, 155 * MS, &at) == 0 && at.v[0] == 1);   /* before the first sample: from the reset */
    CHECK(ysig_value_at(&g, 1, 165 * MS, &at) == 1 && at.v[0] == 0);
    CHECK(ysig_value_at(&g, 1, 212 * MS, &at) == 1 && at.v[0] == 1);
    CHECK(ysig_value_at(&g, 1, 999 * MS, &at) == 1 && at.v[0] == 1 && at.t == 210 * MS);    /* the poll bounds it */
    /* a far-future stamp on another source moves nothing here */
    e = PEN_E(INT64_MAX - 5, 9, 1, 1, YIN_TIER_SIM);
    ysig_feed(&g, &e);
    f = frame_at(3, 500 * MS, 0, NULL, 0);
    f.poll_ns = 500 * MS;
    ysig_frame(&g, &f);
    CHECK(v->v[0] == 1);
    ysig_get_stats(&g, &st);
    CHECK(st.late_poll == 1);
    /* out of order within two devices of one any-device source: the
     * silence runs from the newest time, not the arrival */
    {
        ysig_node q[2];
        q[0] = src_node(YIN_KIND_EYE, 0, 0, 2);
        q[1] = ysig_node_of(YSIG_OP_SILENT, 0, -1); q[1].p[0] = 0.05;
        d = desc_of(q, 2);
        if (!open_desc(&g, &d)) { g_failures++; return; }
        ysig_reset(&g, 0);
        e = axis(100 * MS, YIN_KIND_EYE, 0, 1, 1, 1, 0, YIN_TIER_SIM); ysig_feed(&g, &e);
        e = axis(40 * MS, YIN_KIND_EYE, 0, 2, 1, 1, 0, YIN_TIER_SIM); ysig_feed(&g, &e);
        f = frame_at(1, 120 * MS, 0, NULL, 0);
        ysig_frame(&g, &f);
        CHECK(ysig_value_of(&g, 1)->v[0] == 0);              /* 120 - 100 < 50 */
        f = frame_at(2, 149 * MS, 0, NULL, 0);
        ysig_frame(&g, &f);
        CHECK(ysig_value_of(&g, 1)->v[0] == 0);
        f = frame_at(3, 150 * MS, 0, NULL, 0);
        ysig_frame(&g, &f);
        CHECK(ysig_value_of(&g, 1)->v[0] == 1);             /* exactly `after` */
        n[1] = ysig_node_of(YSIG_OP_SILENT, 0, -1);
        n[1].p[YSIG_SILENT_AFTER] = 0.05; n[1].history = 64;
        d = desc_of(n, 4);
        if (!open_desc(&g, &d)) { g_failures++; return; }
        ysig_reset(&g, 100 * MS);
        v = ysig_value_of(&g, 1);
    }
    /* the same source with a stamp past the poll: not silent while it stands */
    e = axis(600 * MS, YIN_KIND_EYE, 0, 1, 1, 1, 0, YIN_TIER_SIM);
    ysig_feed(&g, &e);
    f = frame_at(4, 590 * MS, 0, NULL, 0);
    f.poll_ns = 580 * MS;
    ysig_frame(&g, &f);
    CHECK(v->v[0] == 0);
    /* extreme: a poll near the ends saturates */
    f = frame_at(5, INT64_MAX, 0, NULL, 0);
    f.poll_ns = INT64_MAX;
    ysig_frame(&g, &f);
    CHECK(v->v[0] == 1);
    ysig_reset(&g, INT64_MIN + 2);
    v = ysig_value_of(&g, 1);
    CHECK(v->v[0] == 0);
    f = frame_at(6, INT64_MIN + 9, 0, NULL, 0);
    f.poll_ns = INT64_MIN + 9;
    ysig_frame(&g, &f);
    CHECK(v->v[0] == 0);
    /* a past read before the next frame is bounded by the reset's time,
     * not by the poll before the reset */
    n[1] = ysig_node_of(YSIG_OP_SILENT, 0, -1);
    n[1].p[YSIG_SILENT_AFTER] = 0.05; n[1].history = 64;
    d = desc_of(n, 4);
    if (!open_desc(&g, &d)) { g_failures++; return; }
    ysig_reset(&g, 0);
    f = frame_at(1, 5000 * MS, 0, NULL, 0);
    ysig_frame(&g, &f);
    ysig_reset(&g, 0);
    CHECK(ysig_value_at(&g, 1, 1000 * MS, &at) == 0 && at.v[0] == 0);
    /* a graph with silent needs the poll */
    f = frame_at(7, 0, 0, NULL, 0);
    f.has_poll = false;
    CHECK(ysig_frame(&g, &f) == -1 && strstr(ysig_error(&g), "silent"));
    /* the reset's time, across a trial with an adapter: reset_trial counts
     * from its own t, a role's silence too */
    {
        ysig_node exp_n[2], ad_n[1];
        ysig_role role;
        ysig_adapter ad;
        memset(&role, 0, sizeof role);
        role.name = "hand"; role.type = YSIG_TYPE_POSITION2D; role.units = YSIG_UNITS_DISPLAY_PX;
        ad_n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 0, 2);
        memset(&ad, 0, sizeof ad);
        ad.role = "hand"; ad.name = "pen"; ad.type = YSIG_TYPE_POSITION2D; ad.units = YSIG_UNITS_DISPLAY_PX;
        ad.nodes = ad_n; ad.n_nodes = 1;
        exp_n[0] = ysig_node_of(YSIG_OP_ROLE, -1, -1);
        exp_n[1] = ysig_node_of(YSIG_OP_SILENT, 0, -1);
        exp_n[1].p[YSIG_SILENT_AFTER] = 0.05;
        d = desc_of(exp_n, 2);
        d.roles = &role; d.n_roles = 1; d.adapters = &ad; d.n_adapters = 1;
        if (!open_desc(&g, &d)) { g_failures++; return; }
        ysig_reset(&g, 0);
        v = ysig_value_of(&g, 1);
        e = PEN_E(10 * MS, 1, 3, 4, YIN_TIER_SIM);
        ysig_feed(&g, &e);
        f = frame_at(1, 80 * MS, 0, NULL, 0);
        ysig_frame(&g, &f);
        CHECK(v->v[0] == 1);
        ysig_reset_trial(&g, 80 * MS);       /* the adapter keeps its sample's time; the silent restarts here */
        CHECK(v->v[0] == 0);
        f = frame_at(2, 120 * MS, 0, NULL, 0);
        ysig_frame(&g, &f);
        CHECK(v->v[0] == 0);
        f = frame_at(3, 131 * MS, 0, NULL, 0);
        ysig_frame(&g, &f);
        CHECK(v->v[0] == 1);
    }
}

/* A detect's output as an input: its state and, with out hit, its hits. */
static void test_detect_out(void) {
    ysig_node n[4];
    ysig_desc d;
    ysig_graph g;
    const ysig_value* v;
    ysig_value at;
    ysig_hit h[4];
    ysig_frame_in f;
    yin_event e;
    g_where = "detect out";
    n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 1, 2);
    n[1] = ysig_node_of(YSIG_OP_DETECT, 0, -1);
    n[1].p[YSIG_DETECT_LEVEL] = 5;
    n[1].p[YSIG_DETECT_REPEAT] = 1;
    n[1].p[YSIG_DETECT_HYSTERESIS] = 1;
    n[1].p[YSIG_DETECT_OUT] = YSIG_OUT_HIT;
    n[1].history = 16;
    d = desc_of(n, 2);
    if (!open_desc(&g, &d)) { g_failures++; return; }
    ysig_reset(&g, 0);
    v = ysig_value_of(&g, 1);
    CHECK(v->v[0] == 0);
    e = PEN_E(10 * MS, 1, 6, 0, YIN_TIER_2); ysig_feed(&g, &e);
    CHECK(v->v[0] == 1 && v->t == 10 * MS && v->tier == YIN_TIER_2);
    CHECK(ysig_detected(&g, h, 4) == 1);
    /* still past the level: the hit stands until ready again; 4.5 is not
     * past the hysteresis (5 - 1), so not ready */
    e = PEN_E(20 * MS, 1, 4.5, 0, YIN_TIER_2); ysig_feed(&g, &e);
    CHECK(v->v[0] == 1);
    e = PEN_E(30 * MS, 1, 3.5, 0, YIN_TIER_2); ysig_feed(&g, &e);
    CHECK(v->v[0] == 0);
    e = PEN_E(40 * MS, 1, 7, 0, YIN_TIER_2); ysig_feed(&g, &e);
    CHECK(v->v[0] == 1);
    CHECK(ysig_detected(&g, h, 4) == 1);
    /* the ring holds the output at each value tested */
    CHECK(ysig_value_at(&g, 1, 25 * MS, &at) == 1 && at.v[0] == 1);
    CHECK(ysig_value_at(&g, 1, 35 * MS, &at) == 1 && at.v[0] == 0);
    CHECK(ysig_value_at(&g, 1, 5 * MS, &at) == 1 && at.v[0] == 0);   /* the reset's entry */
    /* the row cannot change after open: the schedules depend on it */
    CHECK(!ysig_set(&g, 1, YSIG_DETECT_OUT, YSIG_OUT_TEST));
    /* a re-arm clears the output; one ahead of the input waits for input
     * past its time (v0.6.0) */
    CHECK(ysig_arm(&g, 1, 50 * MS) && v->v[0] == 1);
    e = PEN_E(60 * MS, 1, 0, 0, YIN_TIER_2); ysig_feed(&g, &e);
    CHECK(v->v[0] == 0);
    CHECK(ysig_arm(&g, 1, 60 * MS) && v->v[0] == 0);
    /* one-hit detect: the hit stands past the failing value, until the
     * next arm; a hold completed by the next sample rises in that pass */
    n[1] = ysig_node_of(YSIG_OP_DETECT, 0, -1);
    n[1].p[YSIG_DETECT_LEVEL] = 5;
    n[1].p[YSIG_DETECT_HOLD] = 0.02;
    n[1].p[YSIG_DETECT_OUT] = YSIG_OUT_HIT;
    d = desc_of(n, 2);
    if (!open_desc(&g, &d)) { g_failures++; return; }
    ysig_reset(&g, 0);
    v = ysig_value_of(&g, 1);
    e = PEN_E(10 * MS, 1, 6, 1, YIN_TIER_SIM); ysig_feed(&g, &e);
    CHECK(v->v[0] == 0);
    f = frame_at(1, 20 * MS, 0, NULL, 0);
    ysig_frame(&g, &f);
    CHECK(v->v[0] == 0);
    e = PEN_E(31 * MS, 1, 6, 1, YIN_TIER_SIM); ysig_feed(&g, &e);
    CHECK(v->v[0] == 1);                                   /* found by the next sample */
    CHECK(ysig_detected(&g, h, 4) == 1 && h[0].t == 30 * MS);
    e = PEN_E(40 * MS, 1, 0, 0, YIN_TIER_SIM); ysig_feed(&g, &e);
    CHECK(v->v[0] == 1);                                   /* a one-hit detect: until a new arm */
    /* a hit output is 0 at a reset, whatever the test says */
    n[1] = ysig_node_of(YSIG_OP_DETECT, 0, -1);
    n[1].p[YSIG_DETECT_TEST] = YSIG_TEST_LE;
    n[1].p[YSIG_DETECT_LEVEL] = 5;
    n[1].p[YSIG_DETECT_OUT] = YSIG_OUT_HIT;
    d = desc_of(n, 2);
    if (!open_desc(&g, &d)) { g_failures++; return; }
    ysig_reset(&g, 0);
    CHECK(ysig_value_of(&g, 1)->v[0] == 0);
    /* a frame-rate detect as a selector input: the users run at frames */
    {
        ysig_node m[4];
        float tl[1];
        m[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 1, 2);
        m[1] = ysig_node_of(YSIG_OP_DETECT, 0, -1);
        m[1].p[YSIG_DETECT_LEVEL] = 5; m[1].p[YSIG_DETECT_AT] = 1;
        m[2] = ysig_node_of(YSIG_OP_OFFSET, 0, -1);
        m[3] = ysig_node_of(YSIG_OP_MUX, 0, 2); m[3].in[2] = 1;
        d = desc_of(m, 4);
        if (!open_desc(&g, &d)) { g_failures++; return; }
        ysig_reset(&g, 0);
        v = ysig_value_of(&g, 3);
        (void)tl;
        e = PEN_E(10 * MS, 1, 6, 1, YIN_TIER_SIM); ysig_feed(&g, &e);
        CHECK(ysig_value_of(&g, 1)->v[0] == 0);              /* not tested yet: frame rate */
        CHECK(v->v[0] == 6);                                  /* b (offset) is the same value here */
        f = frame_at(1, 20 * MS, 0, NULL, 0);
        ysig_frame(&g, &f);
        CHECK(ysig_value_of(&g, 1)->v[0] == 1 && ysig_value_of(&g, 1)->t == 20 * MS);
        CHECK(v->v[0] == 6);
    }
}

static void test_reset_trial(void) {
    ysig_node exp_n[6], ad_n[2];
    ysig_role role;
    ysig_adapter ad;
    ysig_desc d;
    ysig_graph g, g2;
    static uint64_t arena2[(1u << 20) / 8];
    const ysig_value* hand;
    yin_event e;
    int i;
    ysig_stats st;
    g_where = "reset_trial";
    memset(&role, 0, sizeof role);
    role.name = "hand"; role.type = YSIG_TYPE_POSITION2D_RELATIVE; role.units = YSIG_UNITS_DISPLAY_PX;
    ad_n[0] = src_node(YIN_KIND_MOUSE, YIN_AXIS_DELTA, 0, 2);
    ad_n[1] = ysig_node_of(YSIG_OP_INTEGRATE, 0, -1);
    ad_n[1].p[0] = ad_n[1].p[1] = 1; ad_n[1].p[4] = 500;
    memset(&ad, 0, sizeof ad);
    ad.role = "hand"; ad.name = "mouse"; ad.type = YSIG_TYPE_POSITION2D_RELATIVE; ad.units = YSIG_UNITS_DISPLAY_PX;
    ad.nodes = ad_n; ad.n_nodes = 2;
    exp_n[0] = ysig_node_of(YSIG_OP_ROLE, -1, -1);
    exp_n[1] = ysig_node_of(YSIG_OP_LOWPASS, 0, -1); exp_n[1].p[0] = 10;
    exp_n[2] = ysig_node_of(YSIG_OP_DETECT, 0, -1);
    exp_n[2].p[YSIG_DETECT_LEVEL] = 10; exp_n[2].p[YSIG_DETECT_A_X] = 500;
    exp_n[2].p[YSIG_DETECT_LANE] = -1;
    exp_n[3] = ysig_node_of(YSIG_OP_LATCH, 0, 2);
    exp_n[4] = ysig_node_of(YSIG_OP_PEAK, 0, -1);
    d = desc_of(exp_n, 5);
    d.roles = &role; d.n_roles = 1; d.adapters = &ad; d.n_adapters = 1;
    if (!open_desc(&g, &d)) { g_failures++; return; }
    ysig_reset(&g, 0);
    hand = ysig_value_of(&g, 0);
    for (i = 1; i <= 30; i++) { e = delta(i * MS, 1, 1, 0); ysig_feed(&g, &e); }
    CHECK(hand->v[0] == 530);
    CHECK(ysig_value_of(&g, 3)->v[0] == 510);              /* latched at the crossing */
    CHECK(ysig_value_of(&g, 4)->v[0] == 530);
    /* a trial starts: the hand keeps its place, the experiment's nodes
     * start over */
    ysig_reset_trial(&g, 100 * MS);
    CHECK(hand->v[0] == 530);
    CHECK(ysig_value_of(&g, ysig_adapter_node(&g, 0, 1))->v[0] == 530);
    CHECK(ysig_value_of(&g, 3)->v[0] == 530);             /* the latch let go */
    CHECK(ysig_value_of(&g, 4)->v[0] == 0);
    /* the count sum goes on from where it was */
    e = delta(101 * MS, 1, 2, 0); ysig_feed(&g, &e);
    CHECK(hand->v[0] == 532);
    /* the detect re-armed from the held hand (past the level at the arm,
     * so held): no hit from the held 532 */
    {
        ysig_hit h[4];
        CHECK(ysig_detected(&g, h, 4) == 0);
    }
    /* an order slot survives: a mouse's counts are never dropped, so use a
     * pen adapter: its last time is kept across the trial */
    {
        ysig_node a_n[1], e_n[1];
        ysig_adapter pad;
        role.type = YSIG_TYPE_POSITION2D;
        role.allow_relative = false;
        a_n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 1, 2);
        e_n[0] = ysig_node_of(YSIG_OP_ROLE, -1, -1);
        pad = ad;
        pad.nodes = a_n; pad.n_nodes = 1; pad.type = YSIG_TYPE_POSITION2D;
        d = desc_of(e_n, 1);
        d.roles = &role; d.n_roles = 1; d.adapters = &pad; d.n_adapters = 1;
        if (!open_desc(&g, &d)) { g_failures++; return; }
        ysig_reset(&g, 0);
        e = PEN_E(50 * MS, 1, 1, 1, YIN_TIER_SIM); ysig_feed(&g, &e);
        ysig_reset_trial(&g, 60 * MS);
        e = PEN_E(40 * MS, 1, 9, 9, YIN_TIER_SIM); ysig_feed(&g, &e);
        ysig_get_stats(&g, &st);
        CHECK(st.dropped_order == 1);
        CHECK(ysig_value_of(&g, 0)->v[0] == 1);            /* the older report dropped */
        ysig_reset(&g, 60 * MS);                             /* a full reset forgets the order */
        e = PEN_E(40 * MS, 1, 9, 9, YIN_TIER_SIM); ysig_feed(&g, &e);
        ysig_get_stats(&g, &st);
        /* v0.7.0: a report before the last barrier's time is dropped (R
         * does not have it either), counted in late_barrier */
        CHECK(st.dropped_order == 1 && st.late_barrier == 1 && ysig_value_of(&g, 0)->v[0] != 9);
        e = PEN_E(60 * MS, 1, 9, 9, YIN_TIER_SIM); ysig_feed(&g, &e);
        CHECK(ysig_value_of(&g, 0)->v[0] == 9);
        role.type = YSIG_TYPE_POSITION2D_RELATIVE;
    }
    /* no adapters: ysig_reset_trial is ysig_reset, node for node */
    {
        ysig_node m[4];
        m[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 1, 2);
        m[1] = ysig_node_of(YSIG_OP_LOWPASS, 0, -1); m[1].p[0] = 10;
        m[2] = ysig_node_of(YSIG_OP_PEAK, 0, -1);
        m[3] = ysig_node_of(YSIG_OP_DETECT, 0, -1); m[3].p[YSIG_DETECT_LEVEL] = 100;
        d = desc_of(m, 4);
        if (!open_desc(&g, &d)) { g_failures++; return; }
        d.arena = arena2; d.arena_size = sizeof arena2;
        CHECK(ysig_open(&g2, &d));
        for (i = 1; i <= 20; i++) {
            e = PEN_E(i * MS, 1, 3.5f * i, -i, YIN_TIER_SIM);
            ysig_feed(&g, &e);
            ysig_feed(&g2, &e);
        }
        ysig_reset(&g, 200 * MS);
        ysig_reset_trial(&g2, 200 * MS);
        for (i = 0; i < 4; i++) CHECK(!memcmp(ysig_value_of(&g, i), ysig_value_of(&g2, i), sizeof(ysig_value)));
        e = PEN_E(210 * MS, 1, 1, 1, YIN_TIER_SIM);
        ysig_feed(&g, &e);
        ysig_feed(&g2, &e);
        for (i = 0; i < 4; i++) CHECK(!memcmp(ysig_value_of(&g, i), ysig_value_of(&g2, i), sizeof(ysig_value)));
    }
    /* a reset with times at the ends of int64 */
    ysig_reset_trial(&g, INT64_MAX);
    CHECK(ysig_value_of(&g, 0)->t == INT64_MAX);
    ysig_reset_trial(&g, INT64_MIN);
    CHECK(ysig_value_of(&g, 0)->t == INT64_MIN);
}

static void test_recenter(void) {
    ysig_node exp_n[8], ad_n[4];
    ysig_role role;
    ysig_adapter ad;
    ysig_desc d;
    ysig_graph g;
    const ysig_value* hand;
    ysig_frame_in f;
    yin_event e;
    ysig_hit h[4];
    ysig_stats st;
    int adapter_end;
    g_where = "recenter";
    memset(&role, 0, sizeof role);
    role.name = "hand"; role.type = YSIG_TYPE_POSITION2D; role.units = YSIG_UNITS_DISPLAY_PX; role.allow_relative = true;
    ad_n[0] = src_node(YIN_KIND_MOUSE, YIN_AXIS_DELTA, 0, 2);
    ad_n[1] = ysig_node_of(YSIG_OP_INTEGRATE, 0, -1);
    ad_n[1].p[0] = ad_n[1].p[1] = 0.5; ad_n[1].p[4] = 10; ad_n[1].p[5] = 20;
    ad_n[2] = ysig_node_of(YSIG_OP_SCALE, 1, -1);
    ad_n[2].p[0] = ad_n[2].p[1] = 2;
    ad_n[3] = ysig_node_of(YSIG_OP_OFFSET, 2, -1);
    ad_n[3].p[0] = 100;
    memset(&ad, 0, sizeof ad);
    ad.role = "hand"; ad.name = "mouse"; ad.type = YSIG_TYPE_POSITION2D_RELATIVE; ad.units = YSIG_UNITS_DISPLAY_PX;
    ad.nodes = ad_n; ad.n_nodes = 4;
    exp_n[0] = ysig_node_of(YSIG_OP_ROLE, -1, -1);
    exp_n[1] = ysig_node_of(YSIG_OP_LOWPASS, 0, -1); exp_n[1].p[0] = 1;
    exp_n[2] = ysig_node_of(YSIG_OP_VELOCITY, 0, -1); exp_n[2].p[0] = 0.01;
    exp_n[3] = ysig_node_of(YSIG_OP_DETECT, 0, -1);        /* x >= 122 */
    exp_n[3].p[YSIG_DETECT_LEVEL] = 122; exp_n[3].p[YSIG_DETECT_OUT] = YSIG_OUT_HIT;
    exp_n[4] = ysig_node_of(YSIG_OP_LATCH, 0, 3);
    exp_n[5] = ysig_node_of(YSIG_OP_PEAK, 0, -1);
    exp_n[6] = ysig_node_of(YSIG_OP_BIND, 1, -1);
    exp_n[7] = ysig_node_of(YSIG_OP_SILENT, 0, -1); exp_n[7].p[YSIG_SILENT_AFTER] = 0.05;
    d = desc_of(exp_n, 8);
    d.roles = &role; d.n_roles = 1; d.adapters = &ad; d.n_adapters = 1;
    if (!open_desc(&g, &d)) { g_failures++; return; }
    ysig_reset(&g, 0);
    hand = ysig_value_of(&g, 0);
    CHECK(hand->v[0] == 120 && hand->v[1] == 40);       /* 100 + 2 x 10, 2 x 20 */
    e = delta(5 * MS, 1, 4, 0); ysig_feed(&g, &e);
    CHECK(hand->v[0] == 124);
    CHECK(ysig_detected(&g, h, 4) == 1);                    /* crossed at 124: latched */
    /* the role placed at (104, 40): through the offset and the scale, the
     * integrator lands on (2, 20) */
    CHECK(ysig_recenter(&g, 0, 104, 40, 6 * MS));
    CHECK(hand->v[0] == 104 && hand->v[1] == 40);
    adapter_end = ysig_adapter_node(&g, 0, 1);
    CHECK(ysig_value_of(&g, adapter_end)->v[0] == 2);
    /* downstream filters restart at the new place, a velocity does not spike */
    CHECK(ysig_value_of(&g, 1)->v[0] == 104 && ysig_value_of(&g, 1)->v[1] == 40);
    CHECK(ysig_value_of(&g, 2)->v[0] == 0);
    CHECK(ysig_detected(&g, h, 4) == 0);
    /* a latch stays latched at the old value, a peak keeps its peak, and
     * a silent its clock (5 ms before the recenter, a poll 5 ms later) */
    {
        ysig_frame_in fv = frame_at(99, 5 * MS + 49 * MS, 0, NULL, 0);
        ysig_frame(&g, &fv);
        CHECK(ysig_value_of(&g, 7)->v[0] == 0);
        fv = frame_at(99, 5 * MS + 50 * MS, 0, NULL, 0);
        ysig_frame(&g, &fv);
        CHECK(ysig_value_of(&g, 7)->v[0] == 1);
        CHECK(ysig_recenter(&g, 0, 104, 40, 56 * MS));   /* a second place: the silence stands */
        CHECK(ysig_value_of(&g, 7)->v[0] == 1);
    }
    /* still the old value after those frames */
    CHECK(ysig_value_of(&g, 4)->v[0] == 124 && ysig_value_of(&g, 4)->v[1] == 40);
    CHECK(ysig_value_of(&g, 5)->v[0] == 124);
    /* the bind sees it at the next frame, with no sample: a still hand */
    f = frame_at(1, 8 * MS, 0, NULL, 0);
    ysig_frame(&g, &f);
    CHECK(g_ch[0] == 104);
    /* it continues on the count grid: +4 counts at 0.5 x 2 */
    e = delta(9 * MS, 1, 4, 2); ysig_feed(&g, &e);
    CHECK(hand->v[0] == 108 && hand->v[1] == 42);
    /* a reset to the integrator's own origin clears the placement */
    ysig_reset(&g, 20 * MS);
    CHECK(hand->v[0] == 120 && hand->v[1] == 40);
    /* a bounded integrator: a point inside; a next count at the bound */
    ad_n[1].p[8] = 0; ad_n[1].p[9] = 0; ad_n[1].p[12] = 100; ad_n[1].p[13] = 100;
    d.nodes = exp_n; d.n_nodes = 8;
    if (!open_desc(&g, &d)) { g_failures++; return; }
    ysig_reset(&g, 0);
    hand = ysig_value_of(&g, 0);
    CHECK(ysig_recenter(&g, 0, 100 + 2 * 99.5, 0, 0));        /* integrator at 99.5 */
    e = delta(1 * MS, 1, 4, 0); ysig_feed(&g, &e);
    CHECK(hand->v[0] == 300);                              /* the grid point last inside: 100 */
    CHECK(!ysig_recenter(&g, 0, 100 + 2 * 120.0, 0, 0));
    CHECK(strstr(ysig_error(&g), "bounds") != NULL);
    ysig_get_stats(&g, &st);
    CHECK(st.fed == 1);
    /* refusals: bad arguments, no reader of the role */
    CHECK(!ysig_recenter(&g, -1, 0, 0, 0) && strstr(ysig_error(&g), "bad role"));
    CHECK(!ysig_recenter(&g, 0, NAN, 0, 0) && strstr(ysig_error(&g), "not finite"));
    CHECK(!ysig_recenter(&g, 1, 0, 0, 0) && strstr(ysig_error(&g), "no node reads role 1"));
    CHECK(!ysig_recenter(NULL, 0, 0, 0, 0));
    /* an absolute adapter */
    {
        ysig_node pn[1], ex[1];
        ysig_adapter pa;
        role.type = YSIG_TYPE_POSITION2D; role.allow_relative = false;
        pn[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 0, 2);
        ex[0] = ysig_node_of(YSIG_OP_ROLE, -1, -1);
        pa = ad; pa.nodes = pn; pa.n_nodes = 1; pa.type = YSIG_TYPE_POSITION2D;
        d = desc_of(ex, 1);
        d.roles = &role; d.n_roles = 1; d.adapters = &pa; d.n_adapters = 1;
        if (!open_desc(&g, &d)) { g_failures++; return; }
        ysig_reset(&g, 0);
        CHECK(!ysig_recenter(&g, 0, 1, 1, 0) && strstr(ysig_error(&g), "not relative"));
        role.allow_relative = true;
    }
    /* a relative adapter with no integrator, and one that transforms the
     * integrator through an affine */
    {
        ysig_node rn[2], ex[1];
        ysig_adapter ra;
        rn[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 0, 2);
        rn[1] = ysig_node_of(YSIG_OP_SCALE, 0, -1);
        ex[0] = ysig_node_of(YSIG_OP_ROLE, -1, -1);
        ra = ad; ra.nodes = rn; ra.n_nodes = 2;
        role.type = YSIG_TYPE_POSITION2D; role.allow_relative = true;
        d = desc_of(ex, 1);
        d.roles = &role; d.n_roles = 1; d.adapters = &ra; d.n_adapters = 1;
        if (!open_desc(&g, &d)) { g_failures++; return; }
        ysig_reset(&g, 0);
        CHECK(!ysig_recenter(&g, 0, 1, 1, 0) && strstr(ysig_error(&g), "no integrator"));
        rn[0] = src_node(YIN_KIND_MOUSE, YIN_AXIS_DELTA, 0, 2);
        rn[1] = ysig_node_of(YSIG_OP_INTEGRATE, 0, -1);
        {
            ysig_node r3[3];
            memcpy(r3, rn, sizeof rn);
            r3[2] = ysig_node_of(YSIG_OP_AFFINE, 1, -1);
            ra.nodes = r3; ra.n_nodes = 3;
            if (!open_desc(&g, &d)) { g_failures++; return; }
            ysig_reset(&g, 0);
            CHECK(!ysig_recenter(&g, 0, 1, 1, 0) && strstr(ysig_error(&g), "affine"));
        }
    }
    /* a rate integrator recenters too (the scale is in its chain) */
    {
        ysig_node rn[3], ex[1];
        ysig_adapter ra;
        rn[0] = src_node(YIN_KIND_GAMEPAD, 0, 0, 2);
        rn[1] = ysig_node_of(YSIG_OP_INTEGRATE, 0, -1);
        rn[2] = ysig_node_of(YSIG_OP_SCALE, 1, -1); rn[2].p[0] = rn[2].p[1] = 2;
        ex[0] = ysig_node_of(YSIG_OP_ROLE, -1, -1);
        ra = ad; ra.nodes = rn; ra.n_nodes = 3;
        d = desc_of(ex, 1);
        d.roles = &role; d.n_roles = 1; d.adapters = &ra; d.n_adapters = 1;
        if (!open_desc(&g, &d)) { g_failures++; return; }
        ysig_reset(&g, 0);
        hand = ysig_value_of(&g, 0);
        CHECK(ysig_recenter(&g, 0, 10, 8, 5 * MS));
        CHECK(hand->v[0] == 10 && hand->v[1] == 8);
        CHECK(ysig_value_of(&g, ysig_adapter_node(&g, 0, 1))->v[0] == 5);
        e = axis(10 * MS, YIN_KIND_GAMEPAD, 0, 1, 1000, 0, 0, YIN_TIER_SIM);
        ysig_feed(&g, &e);                                     /* first: dt 0 */
        e = axis(20 * MS, YIN_KIND_GAMEPAD, 0, 1, 1000, 0, 0, YIN_TIER_SIM);
        ysig_feed(&g, &e);                                     /* 1000 x 10 ms = 10, x 2 = 20 more */
        CHECK(hand->v[0] == 30 && hand->v[1] == 8);
    }
}

static void test_ops_refusals(void) {
    ysig_node n[5];
    g_where = "ops refusals";
    n[0] = src_node(YIN_KIND_MOUSE, YIN_AXIS_DELTA, 0, 2);
    n[1] = ysig_node_of(YSIG_OP_INTEGRATE, 0, -1);
    n[2] = ysig_node_of(YSIG_OP_SELECT, 1, -1); n[2].lanes = 1;
    n[3] = ysig_node_of(YSIG_OP_MUX, 1, 2); n[3].in[2] = 2;
    expect_refused(n, 4, "same lanes");
    n[3] = ysig_node_of(YSIG_OP_MUX, 1, 1);
    expect_refused(n, 4, "takes 3 to 3 inputs");
    n[3] = ysig_node_of(YSIG_OP_LATCH, 1, -1);
    expect_refused(n, 4, "takes 2 to 2 inputs");
    n[3] = ysig_node_of(YSIG_OP_DISTANCE, 2, -1);
    expect_refused(n, 4, "2 or 3 lanes");
    n[3] = ysig_node_of(YSIG_OP_DISTANCE, 1, 2);
    expect_refused(n, 4, "the point has 1 lanes");
    n[3] = ysig_node_of(YSIG_OP_PEAK, 2, -1);
    n[3].p[YSIG_PEAK_LANE] = 1;
    expect_refused(n, 4, "lane 1 of 1");
    n[3] = ysig_node_of(YSIG_OP_ALL, 2, -1);
    expect_refused(n, 4, "takes 2 to 4 inputs");
    /* a stateful op with no source: a peak or a silent on a generator */
    n[0] = ysig_node_of(YSIG_OP_CHANNEL, -1, -1);
    n[1] = ysig_node_of(YSIG_OP_PEAK, 0, -1);
    expect_refused(n, 2, "keeps state per sample");
    n[1] = ysig_node_of(YSIG_OP_SILENT, 0, -1);
    n[1].p[YSIG_SILENT_AFTER] = 1;
    expect_refused(n, 2, "keeps state per sample");
    /* a silent's after must be positive */
    n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 0, 2);
    n[1] = ysig_node_of(YSIG_OP_SILENT, 0, -1);
    expect_refused(n, 2, "out of");
    n[1].p[YSIG_SILENT_AFTER] = 0.1;
    n[2] = ysig_node_of(YSIG_OP_RECORD, 0, -1);
    n[3] = ysig_node_of(YSIG_OP_LATCH, 0, 2);
    expect_refused(n, 4, "is a sink");
    /* a peak's lane after open: the table's limits, then the input's */
    {
        ysig_desc d;
        ysig_graph g;
        n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 0, 2);
        n[1] = ysig_node_of(YSIG_OP_PEAK, 0, -1);
        n[2] = ysig_node_of(YSIG_OP_DISTANCE, 0, -1);
        d = desc_of(n, 3);
        if (!open_desc(&g, &d)) { g_failures++; return; }
        CHECK(ysig_set(&g, 1, YSIG_PEAK_LANE, 1));
        CHECK(!ysig_set(&g, 1, YSIG_PEAK_LANE, 2));
        CHECK(!ysig_set(&g, 1, YSIG_PEAK_LANE, 5));
        CHECK(ysig_set(&g, 2, YSIG_DISTANCE_POINT_X, 3));
        CHECK(!ysig_set(&g, 0, 99, 0));
    }
}

/* The new ops at a frame whose onset is ahead of the poll, a past read of
 * a detect selector and rows read inside on_row. */
static void test_ops_views(void) {
    ysig_node n[8];
    ysig_desc d;
    ysig_graph g;
    ysig_value at;
    float tl[1];
    ysig_frame_in f;
    yin_event e;
    int i;
    g_where = "ops views";
    n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 1, 2);
    n[1] = ysig_node_of(YSIG_OP_VELOCITY, 0, -1); n[1].p[0] = 0.01;
    n[2] = ysig_node_of(YSIG_OP_DISTANCE, 1, -1);             /* the velocity's norm, a view */
    n[3] = ysig_node_of(YSIG_OP_SPEED, 0, -1); n[3].p[0] = 0.01;
    n[4] = ysig_node_of(YSIG_OP_PEAK, 3, -1);
    n[5] = ysig_node_of(YSIG_OP_SILENT, 0, -1); n[5].p[0] = 0.03; n[5].history = 16;
    n[6] = ysig_node_of(YSIG_OP_CHANNEL, -1, -1);
    n[7] = ysig_node_of(YSIG_OP_ALL, 5, 6); n[7].history = 16;
    d = desc_of(n, 8);
    d.on_row = on_row_vat;
    if (!open_desc(&g, &d)) { g_failures++; return; }
    g_vat_graph = &g; g_vat_node = 7; g_vat_checked = g_vat_bad = 0;
    ysig_reset(&g, 0);
    /* moving: 1000 px/s */
    for (i = 0; i < 40; i++) {
        e = PEN_E(i * MS, 1, i, 0, YIN_TIER_SIM);
        ysig_feed(&g, &e);
    }
    /* a frame's planned onset far ahead of the poll: still moving at the
     * poll, so the views stand, and the peak holds the sampled speed */
    tl[0] = 1;
    f = frame_at(1, 200 * MS, 0, tl, 1);
    f.poll_ns = 40 * MS;
    ysig_frame(&g, &f);
    CHECK_NEAR(ysig_value_of(&g, 2)->v[0], 1000, 1e-6);
    CHECK_NEAR(ysig_value_of(&g, 3)->v[0], 1000, 1e-6);
    CHECK_NEAR(ysig_value_of(&g, 4)->v[0], 1000, 1e-6);
    CHECK(ysig_value_of(&g, 5)->v[0] == 0);
    CHECK(ysig_value_of(&g, 7)->v[0] == 0);
    /* a still input: no samples for the silence's time up to the poll; the
     * velocity's view falls to 0, a peak keeps its sampled peak */
    f = frame_at(2, 300 * MS, 0, tl, 1);
    f.poll_ns = 80 * MS;
    ysig_frame(&g, &f);
    CHECK(ysig_value_of(&g, 2)->v[0] == 0);
    CHECK_NEAR(ysig_value_of(&g, 4)->v[0], 1000, 1e-6);
    CHECK(ysig_value_of(&g, 5)->v[0] == 1 && ysig_value_of(&g, 5)->t == 80 * MS);
    CHECK(ysig_value_of(&g, 7)->v[0] == 1);
    /* a past read of the all: the silence at that time, bounded by the poll */
    CHECK(ysig_value_at(&g, 7, 60 * MS, &at) == 1 && at.v[0] == 0);       /* the channel still 0 */
    CHECK(ysig_value_at(&g, 7, 250 * MS, &at) == 1 && at.v[0] == 1);      /* its frame at 200 ms, silent by the poll */
    CHECK(ysig_value_at(&g, 7, 999 * MS, &at) == 1 && at.v[0] == 1);
    /* a detect selector at a past time, from the detect's ring */
    {
        ysig_node m[5];
        m[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 1, 2);
        m[1] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 2, 2);
        m[2] = ysig_node_of(YSIG_OP_DETECT, 0, -1); m[2].p[YSIG_DETECT_LEVEL] = 5; m[2].p[YSIG_DETECT_REPEAT] = 1;
        m[3] = ysig_node_of(YSIG_OP_MUX, 0, 1); m[3].in[2] = 2; m[3].history = 16;
        m[4] = ysig_node_of(YSIG_OP_RECORD, 3, -1);
        d = desc_of(m, 5);
        d.on_row = on_row_vat;
        if (!open_desc(&g, &d)) { g_failures++; return; }
        g_vat_graph = &g; g_vat_node = 3; g_vat_checked = g_vat_bad = 0;
        ysig_reset(&g, 0);
        e = PEN_E(10 * MS, 2, 3, 3, YIN_TIER_SIM); ysig_feed(&g, &e);
        e = PEN_E(20 * MS, 1, 6, 6, YIN_TIER_SIM); ysig_feed(&g, &e);
        e = PEN_E(30 * MS, 1, 1, 1, YIN_TIER_SIM); ysig_feed(&g, &e);
        CHECK(ysig_value_at(&g, 3, 15 * MS, &at) == 1 && at.v[0] == 3);   /* selector false: b */
        CHECK(ysig_value_at(&g, 3, 25 * MS, &at) == 1 && at.v[0] == 6 && at.t == 20 * MS);    /* after the crossing: a */
        CHECK(ysig_value_at(&g, 3, 35 * MS, &at) == 1 && at.v[0] == 3 && at.t == 10 * MS);    /* the test false again */
        CHECK(g_vat_bad == 0 && g_vat_checked == 3);
    }
}

/* Every new op together: a journaled run with resets of both kinds and
 * recenters replays bit for bit, and the digest moves with the output. */
static void test_ops_replay(void) {
    static uint64_t arena2[(2u << 20) / 8];
    ysig_node exp_n[12], ad_n[2];
    ysig_role role;
    ysig_adapter ad;
    ysig_desc d;
    uint64_t dg[3];
    int run;
    g_where = "ops replay";
    memset(&role, 0, sizeof role);
    role.name = "hand"; role.type = YSIG_TYPE_POSITION2D_RELATIVE; role.units = YSIG_UNITS_DISPLAY_PX;
    ad_n[0] = src_node(YIN_KIND_MOUSE, YIN_AXIS_DELTA, 0, 2);
    ad_n[1] = ysig_node_of(YSIG_OP_INTEGRATE, 0, -1);
    ad_n[1].p[0] = ad_n[1].p[1] = 0.25;
    memset(&ad, 0, sizeof ad);
    ad.role = "hand"; ad.name = "mouse"; ad.type = YSIG_TYPE_POSITION2D_RELATIVE; ad.units = YSIG_UNITS_DISPLAY_PX;
    ad.nodes = ad_n; ad.n_nodes = 2;
    exp_n[0] = ysig_node_of(YSIG_OP_ROLE, -1, -1);
    exp_n[1] = ysig_node_of(YSIG_OP_CHANNEL, -1, -1);
    exp_n[2] = ysig_node_of(YSIG_OP_DETECT, 0, -1);
    exp_n[2].p[YSIG_DETECT_LANE] = -1; exp_n[2].p[YSIG_DETECT_LEVEL] = 8; exp_n[2].p[YSIG_DETECT_REPEAT] = 1;
    exp_n[2].p[YSIG_DETECT_OUT] = YSIG_OUT_HIT;
    exp_n[3] = ysig_node_of(YSIG_OP_OFFSET, 0, -1);
    exp_n[4] = ysig_node_of(YSIG_OP_MUX, 0, 3); exp_n[4].in[2] = 1;
    exp_n[5] = ysig_node_of(YSIG_OP_LATCH, 4, 2);
    exp_n[6] = ysig_node_of(YSIG_OP_DISTANCE, 4, -1);
    exp_n[7] = ysig_node_of(YSIG_OP_PEAK, 6, -1);
    exp_n[8] = ysig_node_of(YSIG_OP_ALL, 2, 1);
    exp_n[9] = ysig_node_of(YSIG_OP_SILENT, 0, -1); exp_n[9].p[YSIG_SILENT_AFTER] = 0.05;
    exp_n[10] = ysig_node_of(YSIG_OP_JOIN, 5, 7);
    exp_n[11] = ysig_node_of(YSIG_OP_RECORD, 10, -1);
    for (run = 0; run < 3; run++) {
        ysig_graph g, g2;
        ysig_stats s1, s2;
        uint64_t rs = 7;
        int i, k;
        d = desc_of(exp_n, 12);
        d.roles = &role; d.n_roles = 1; d.adapters = &ad; d.n_adapters = 1;
        d.journal = on_journal;
        if (!open_desc(&g, &d)) { g_failures++; return; }
        g_nj = 0;
        ysig_reset(&g, 0);
        for (i = 0; i < 200; i++) {
            float tl[1];
            ysig_frame_in f;
            for (k = 0; k < 8; k++) {
                yin_event e = delta((int64_t)i * 16 * MS + k * MS, 1, (float)((int)(ysig__mix(&rs) % 9) - 4),
                                    (float)((int)(ysig__mix(&rs) % 9) - 4));
                ysig_feed(&g, &e);
            }
            tl[0] = (float)(i % 5 == 0);
            f = frame_at(i, (int64_t)(i + 1) * 16 * MS, 0, tl, 1);
            ysig_frame(&g, &f);
            if (i == 50) ysig_reset_trial(&g, f.onset_ns);
            if (i == 70) CHECK(ysig_recenter(&g, 0, run == 1 ? 3.0 : 1.5, -1, f.onset_ns));
            if (i == 90) ysig_reset(&g, f.onset_ns);
            {
                ysig_hit hh[8];
                while (ysig_detected(&g, hh, 8) > 0) { }
            }
        }
        if (run == 2) {
            ysig_frame_in f = frame_at(1000, 8000 * MS, 0, NULL, 0);
            ysig_frame(&g, &f);
        }
        CHECK(ysig_recenter(&g, 0, 2, 2, 8100 * MS));                 /* its time replays too */
        CHECK(g_nj < J_CAP);
        ysig_get_stats(&g, &s1);
        dg[run] = s1.digest;
        d.journal = NULL;
        d.arena = arena2;
        d.arena_size = sizeof arena2;
        CHECK(ysig_arena_size(&d) <= sizeof arena2);
        CHECK(ysig_open(&g2, &d));
        CHECK(ysig_replay(&g2, g_j, g_nj) == g_nj);
        ysig_get_stats(&g2, &s2);
        CHECK(s1.digest == s2.digest && s1.rows == s2.rows && s1.hits == s2.hits && s1.rows > 0);
        for (i = 0; i < g.n; i++) CHECK(!memcmp(ysig_value_of(&g, i), ysig_value_of(&g2, i), sizeof(ysig_value)));
    }
    CHECK(dg[0] != dg[1] && dg[0] != dg[2]);
}

/* ---------------------------------------------------------------- v0.2 review: T numbers of the probes */

#define PEN1_E(t, dev, v) axis((t), YIN_KIND_PEN, 0, (dev), 0, 0, (float)(v), YIN_TIER_SIM)

static bool open_adapter(ysig_graph* g, ysig_node* n, int nn, ysig_node* an, int ann, int adtype, const ysig_op_vtable* const* cu) {
    static ysig_role role;
    static ysig_adapter ad;
    ysig_desc d;
    memset(&role, 0, sizeof role);
    role.name = "hand"; role.type = YSIG_TYPE_POSITION2D; role.units = YSIG_UNITS_DISPLAY_PX; role.allow_relative = true;
    memset(&ad, 0, sizeof ad);
    ad.role = "hand"; ad.name = "a"; ad.type = (uint8_t)adtype; ad.units = YSIG_UNITS_DISPLAY_PX; ad.nodes = an; ad.n_nodes = ann;
    d = desc_of(n, nn);
    d.roles = &role; d.n_roles = 1; d.adapters = &ad; d.n_adapters = 1;
    if (cu) { d.custom = cu; d.n_custom = 1; }
    return open_desc(g, &d);
}

static ysig_frame_in frame_poll(int64_t index, int64_t onset, int64_t poll, const float* v, int nv) {
    ysig_frame_in f = frame_at(index, onset, onset, v, nv);
    f.poll_ns = poll;
    return f;
}

/* T1, T2: a recenter has a time */
static void test_recenter_time(void) {
    ysig_node a[2], n[3];
    ysig_graph g;
    ysig_value v;
    ysig_frame_in f;
    yin_event e;
    int ai;
    g_where = "recenter time";
    a[0] = src_node(YIN_KIND_MOUSE, YIN_AXIS_DELTA, 0, 2);
    a[1] = ysig_node_of(YSIG_OP_INTEGRATE, 0, -1); a[1].history = 256;
    n[0] = ysig_node_of(YSIG_OP_ROLE, -1, -1);
    n[1] = ysig_node_of(YSIG_OP_OFFSET, 0, -1); n[1].history = 256;
    if (!open_adapter(&g, n, 2, a, 2, YSIG_TYPE_POSITION2D_RELATIVE, NULL)) { g_failures++; return; }
    ysig_reset(&g, 0);
    e = delta(10 * MS, 1, 40, 0); ysig_feed(&g, &e);
    e = delta(20 * MS, 1, 60, 0); ysig_feed(&g, &e);
    CHECK(ysig_recenter(&g, 0, 0, 0, 30 * MS));
    ai = ysig_adapter_node(&g, 0, 1);
    CHECK(ysig_value_of(&g, ai)->t == 30 * MS && ysig_value_of(&g, 0)->t == 30 * MS);
    /* the history before it stands */
    CHECK(ysig_value_at(&g, ai, 20 * MS, &v) == 1 && v.v[0] == 100 && v.t == 20 * MS);
    CHECK(ysig_value_at(&g, 1, 20 * MS, &v) == 1 && v.v[0] == 100);
    CHECK(ysig_value_at(&g, 1, 15 * MS, &v) == 1 && v.v[0] == 40);
    CHECK(ysig_value_at(&g, ai, 30 * MS, &v) == 1 && v.v[0] == 0 && v.t == 30 * MS);
    CHECK(ysig_value_at(&g, 1, 35 * MS, &v) == 1 && v.v[0] == 0);
    /* a rate integrator's previous time is the recenter's */
    a[0] = src_node(YIN_KIND_GAMEPAD, YIN_AXIS_GAMEPAD, 0, 2);
    a[1] = ysig_node_of(YSIG_OP_INTEGRATE, 0, -1);
    n[0] = ysig_node_of(YSIG_OP_ROLE, -1, -1);
    if (!open_adapter(&g, n, 1, a, 2, YSIG_TYPE_POSITION2D_RELATIVE, NULL)) { g_failures++; return; }
    ysig_reset(&g, 0);
    e = axis(1 * MS, YIN_KIND_GAMEPAD, YIN_AXIS_GAMEPAD, 1, 1, 0, 0, YIN_TIER_SIM); ysig_feed(&g, &e);
    e = axis(2 * MS, YIN_KIND_GAMEPAD, YIN_AXIS_GAMEPAD, 1, 1, 0, 0, YIN_TIER_SIM); ysig_feed(&g, &e);
    CHECK(ysig_recenter(&g, 0, 0, 0, 5000 * MS));
    f = frame_poll(1, 5017 * MS, 5001 * MS, NULL, 0);
    ysig_frame(&g, &f);
    CHECK_NEAR(ysig_value_of(&g, 0)->v[0], 0.001, 1e-9);
}

/* T9, T11, T12: the chain back to the integrator; what a recenter restarts */
static int g_cu_resets;
static void cu_reset(void* st, const double* p, int64_t t) { (void)st; (void)p; (void)t; g_cu_resets++; }
static void cu_sample(void* st, const double* p, const ysig_value* in, int n_in, ysig_value* out) {
    (void)st; (void)p; (void)n_in; out->v[0] = in[0].v[0];
}
static void test_recenter_chain(void) {
    static const ysig_op_vtable cu = { YSIG_OP_ABI, "pass", { 0, 0, 0, 0 }, 1, 8, NULL, 0, cu_reset, cu_sample, NULL };
    const ysig_op_vtable* cus[1];
    ysig_node a[4], n[8];
    ysig_graph g;
    ysig_frame_in f;
    yin_event e;
    g_where = "recenter chain";
    a[0] = src_node(YIN_KIND_MOUSE, YIN_AXIS_DELTA, 0, 2);
    a[1] = ysig_node_of(YSIG_OP_INTEGRATE, 0, -1);
    a[1].p[0] = a[1].p[1] = 0.25; a[1].p[4] = a[1].p[5] = 100; a[1].p[12] = a[1].p[13] = 1000;
    a[2] = ysig_node_of(YSIG_OP_OFFSET, 1, -1); a[2].p[0] = 10; a[2].p[1] = 20;
    a[3] = ysig_node_of(YSIG_OP_SCALE, 2, -1); a[3].p[0] = 2; a[3].p[1] = 4;
    n[0] = ysig_node_of(YSIG_OP_ROLE, -1, -1);
    if (!open_adapter(&g, n, 1, a, 4, YSIG_TYPE_POSITION2D_RELATIVE, NULL)) { g_failures++; return; }
    ysig_reset(&g, 0);
    CHECK(ysig_recenter(&g, 0, 50, 120, 2 * MS));
    CHECK(ysig_value_of(&g, 0)->v[0] == 50 && ysig_value_of(&g, 0)->v[1] == 120);
    e = delta(3 * MS, 1, 4, 4); ysig_feed(&g, &e);
    CHECK(ysig_value_of(&g, 0)->v[0] == 52 && ysig_value_of(&g, 0)->v[1] == 124);
    /* T11: a point that the scale makes infinite is refused */
    a[1] = ysig_node_of(YSIG_OP_INTEGRATE, 0, -1);
    a[2] = ysig_node_of(YSIG_OP_SCALE, 1, -1); a[2].p[0] = a[2].p[1] = 1e-300;
    if (!open_adapter(&g, n, 1, a, 3, YSIG_TYPE_POSITION2D_RELATIVE, NULL)) { g_failures++; return; }
    ysig_reset(&g, 0);
    CHECK(!ysig_recenter(&g, 0, 1e300, 0, 1 * MS) && strstr(ysig_error(&g), "not finite after"));
    e = delta(2 * MS, 1, 1, 0); ysig_feed(&g, &e);
    CHECK(isfinite(ysig_value_of(&g, 0)->v[0]));
    /* T12: rate integrator, custom op, latch, peak keep their state; a
     * velocity, a speed, a lowpass and a delay restart */
    cus[0] = &cu;
    a[1] = ysig_node_of(YSIG_OP_INTEGRATE, 0, -1);
    n[0] = ysig_node_of(YSIG_OP_ROLE, -1, -1);
    n[1] = ysig_node_of(YSIG_OP_INTEGRATE, 0, -1); n[1].p[4] = 3;   /* the experiment's rate integrate of the hand */
    n[2] = ysig_node_of(YSIG_OP_CUSTOM, 0, -1);
    n[3] = ysig_node_of(YSIG_OP_LOWPASS, 0, -1); n[3].p[0] = 10;
    n[4] = ysig_node_of(YSIG_OP_VELOCITY, 0, -1); n[4].p[0] = 0.01;
    n[5] = ysig_node_of(YSIG_OP_SPEED, 0, -1); n[5].p[0] = 0.01;
    n[6] = ysig_node_of(YSIG_OP_DELAY, 0, -1); n[6].p[0] = 0.001;
    n[7] = ysig_node_of(YSIG_OP_OFFSET, 1, -1);                    /* fed by a node that keeps its state */
    if (!open_adapter(&g, n, 8, a, 2, YSIG_TYPE_POSITION2D_RELATIVE, cus)) { g_failures++; return; }
    ysig_reset(&g, 0);
    e = delta(100 * MS, 1, 2, 0); ysig_feed(&g, &e);
    e = delta(1100 * MS, 1, 0, 0); ysig_feed(&g, &e);
    f = frame_poll(1, 1110 * MS, 1100 * MS, NULL, 0);
    ysig_frame(&g, &f);
    {
        double kept = ysig_value_of(&g, 1)->v[0];
        int64_t kept_t = ysig_value_of(&g, 7)->t;
        double custom = ysig_value_of(&g, 2)->v[0];
        int resets = g_cu_resets;
        CHECK_NEAR(kept, 5, 1e-9);                                  /* 3 + 2 x 1 s */
        CHECK(ysig_recenter(&g, 0, 0, 0, 1200 * MS));
        CHECK(ysig_value_of(&g, 1)->v[0] == kept);
        CHECK(ysig_value_of(&g, 7)->v[0] == kept && ysig_value_of(&g, 7)->t == kept_t);
        CHECK(g_cu_resets == resets && ysig_value_of(&g, 2)->v[0] == custom);
        CHECK(ysig_value_of(&g, 3)->v[0] == 0);                      /* lowpass from the new point */
        CHECK(ysig_value_of(&g, 4)->v[0] == 0 && ysig_value_of(&g, 5)->v[0] == 0);
        CHECK(ysig_value_of(&g, 6)->v[0] == 0);
    }
}

/* T3, T4: a peak does not depend on arrival order, and no value is no peak */
static void test_peak_order(void) {
    ysig_node n[4];
    ysig_desc d;
    ysig_graph g;
    ysig_value v;
    ysig_stats st;
    float c = 1;
    ysig_frame_in f;
    yin_event e;
    g_where = "peak order";
    n[0] = src_node(YIN_KIND_PEN, 0, 0, 1);
    n[1] = ysig_node_of(YSIG_OP_PEAK, 0, -1); n[1].history = 64;
    d = desc_of(n, 2);
    if (!open_desc(&g, &d)) { g_failures++; return; }
    ysig_reset(&g, 0);
    e = PEN1_E(10 * MS, 1, 5); ysig_feed(&g, &e);
    e = PEN1_E(9 * MS, 2, 7); ysig_feed(&g, &e);
    CHECK(ysig_value_of(&g, 1)->v[0] == 7 && ysig_value_of(&g, 1)->t == 9 * MS);   /* its own t: the peak sample's */
    CHECK(ysig_value_at(&g, 1, 20 * MS, &v) == 1 && v.v[0] == 7);
    e = PEN1_E(11 * MS, 1, 1); ysig_feed(&g, &e);                                  /* not a peak: no skip counted */
    ysig_get_stats(&g, &st);
    CHECK(st.history_skipped == 0);
    CHECK(ysig_value_at(&g, 1, 20 * MS, &v) == 1 && v.v[0] == 7);
    /* a gate rising before any sample does not start from "no value" */
    n[1] = ysig_node_of(YSIG_OP_CHANNEL, -1, -1);
    n[2] = ysig_node_of(YSIG_OP_PEAK, 0, 1);
    d = desc_of(n, 3);
    if (!open_desc(&g, &d)) { g_failures++; return; }
    ysig_reset(&g, 0);
    f = frame_poll(1, 10 * MS, 5 * MS, &c, 1);
    ysig_frame(&g, &f);
    CHECK(ysig_value_of(&g, 2)->v[0] == 0);
    e = PEN1_E(20 * MS, 1, -5); ysig_feed(&g, &e);
    e = PEN1_E(21 * MS, 1, -3); ysig_feed(&g, &e);
    CHECK(ysig_value_of(&g, 2)->v[0] == -3 && ysig_value_of(&g, 2)->tier == YIN_TIER_SIM);
    /* a sample before the rise, while the gate is shut, is a value */
    ysig_reset(&g, 100 * MS);
    e = PEN1_E(110 * MS, 1, 4); ysig_feed(&g, &e);
    f = frame_poll(2, 120 * MS, 120 * MS, &c, 1);
    ysig_frame(&g, &f);
    CHECK(ysig_value_of(&g, 2)->v[0] == 4 && ysig_value_of(&g, 2)->t == 110 * MS);
    /* a reset forgets that a sample came: its held zero is no value */
    ysig_reset(&g, 200 * MS);
    f = frame_poll(3, 210 * MS, 210 * MS, &c, 1);
    ysig_frame(&g, &f);
    e = PEN1_E(220 * MS, 1, -5); ysig_feed(&g, &e);
    e = PEN1_E(221 * MS, 1, -3); ysig_feed(&g, &e);
    CHECK(ysig_value_of(&g, 2)->v[0] == -3);
}

/* T5, T14: a stamp later than the poll is believed only up to the poll */
static void test_late_stamp(void) {
    ysig_node a[2], n[2];
    ysig_desc d;
    ysig_graph g;
    ysig_stats st;
    ysig_frame_in f;
    yin_event e;
    int k;
    g_where = "late stamp";
    a[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 0, 2);
    n[0] = ysig_node_of(YSIG_OP_ROLE, -1, -1);
    n[1] = ysig_node_of(YSIG_OP_SILENT, 0, -1); n[1].p[0] = 0.1;
    if (!open_adapter(&g, n, 2, a, 1, YSIG_TYPE_POSITION2D, NULL)) { g_failures++; return; }
    ysig_reset(&g, 0);
    e = PEN_E(10 * MS, 1, 1, 0, YIN_TIER_SIM); ysig_feed(&g, &e);
    e = PEN_E(3600000LL * MS, 1, 2, 0, YIN_TIER_SIM); ysig_feed(&g, &e);
    f = frame_poll(1, 20 * MS, 20 * MS, NULL, 0);
    ysig_frame(&g, &f);
    ysig_get_stats(&g, &st);
    CHECK(st.late_poll == 1 && st.late_poll_max_ns == 3600000LL * MS - 20 * MS);
    ysig_reset_trial(&g, 100 * MS);
    for (k = 1; k <= 50; k++) { e = PEN_E((100 + k) * MS, 1, 10 + k, 0, YIN_TIER_SIM); ysig_feed(&g, &e); }
    ysig_get_stats(&g, &st);
    CHECK(ysig_value_of(&g, 0)->v[0] == 60 && st.dropped_order == 0);
    f = frame_poll(2, 1000 * MS, 1000 * MS, NULL, 0);
    ysig_frame(&g, &f);
    CHECK(ysig_value_of(&g, 1)->v[0] == 1);
    /* T14: raw mouse counts, never dropped; the silent's own clock comes
     * down: silent after the real silence */
    a[0] = src_node(YIN_KIND_MOUSE, YIN_AXIS_DELTA, 0, 2);
    a[1] = ysig_node_of(YSIG_OP_INTEGRATE, 0, -1);
    if (!open_adapter(&g, n, 2, a, 2, YSIG_TYPE_POSITION2D_RELATIVE, NULL)) { g_failures++; return; }
    ysig_reset(&g, 0);
    e = delta(10 * MS, 1, 1, 0); ysig_feed(&g, &e);
    f = frame_poll(1, 500 * MS, 500 * MS, NULL, 0);
    ysig_frame(&g, &f);
    CHECK(ysig_value_of(&g, 1)->v[0] == 1);
    e = delta(3600000LL * MS, 1, 1, 0); ysig_feed(&g, &e);
    CHECK(ysig_value_of(&g, 1)->v[0] == 0);
    f = frame_poll(2, 1500 * MS, 1500 * MS, NULL, 0);           /* believed up to this poll */
    ysig_frame(&g, &f);
    CHECK(ysig_value_of(&g, 1)->v[0] == 0);
    f = frame_poll(3, 1599 * MS, 1599 * MS, NULL, 0);
    ysig_frame(&g, &f);
    CHECK(ysig_value_of(&g, 1)->v[0] == 0);
    f = frame_poll(4, 1600 * MS, 1600 * MS, NULL, 0);
    ysig_frame(&g, &f);
    CHECK(ysig_value_of(&g, 1)->v[0] == 1);
    /* the stamp stands when it is within the poll */
    ysig_reset(&g, 2000 * MS);
    e = delta(2050 * MS, 1, 1, 0); ysig_feed(&g, &e);
    f = frame_poll(5, 2100 * MS, 2100 * MS, NULL, 0);
    ysig_frame(&g, &f);
    CHECK(ysig_value_of(&g, 1)->v[0] == 0);
    /* the documented limit: the stamp's effect on rings and hold clocks
     * stays until the node's reset, and a reset clears it */
    {
        ysig_node m[2];
        ysig_hit h[4];
        ysig_value at;
        int nh = 0;
        m[0] = src_node(YIN_KIND_PEN, 0, 0, 1);
        m[1] = ysig_node_of(YSIG_OP_DETECT, 0, -1);
        m[1].p[YSIG_DETECT_TEST] = YSIG_TEST_LE; m[1].p[YSIG_DETECT_LEVEL] = 10; m[1].p[YSIG_DETECT_HOLD] = 0.05;
        m[1].p[YSIG_DETECT_ENTRY] = 1;
        d = desc_of(m, 2);
        if (!open_desc(&g, &d)) { g_failures++; return; }
        ysig_reset(&g, 0);
        e = PEN1_E(1 * MS, 1, 50); ysig_feed(&g, &e);
        e = PEN1_E(3600000LL * MS, 2, 50); ysig_feed(&g, &e);
        f = frame_poll(1, 20 * MS, 20 * MS, NULL, 0); ysig_frame(&g, &f);
        for (k = 0; k < 20; k++) {
            e = PEN1_E((40 + k * 10) * MS, 1, 1); ysig_feed(&g, &e);
            f = frame_poll(2 + k, (45 + k * 10) * MS, (45 + k * 10) * MS, NULL, 0); ysig_frame(&g, &f);
            nh += ysig_detected(&g, h, 4);
        }
        CHECK(nh == 0);
        ysig_reset(&g, 1000 * MS);
        e = PEN1_E(1001 * MS, 1, 50); ysig_feed(&g, &e);
        e = PEN1_E(1002 * MS, 1, 1); ysig_feed(&g, &e);
        f = frame_poll(30, 1100 * MS, 1100 * MS, NULL, 0); ysig_frame(&g, &f);
        CHECK(ysig_detected(&g, h, 4) == 1 && h[0].t == 1052 * MS);
        m[0].history = 64;
        d = desc_of(m, 1);
        if (!open_desc(&g, &d)) { g_failures++; return; }
        ysig_reset(&g, 0);
        e = PEN1_E(3600000LL * MS, 2, 9); ysig_feed(&g, &e);
        f = frame_poll(1, 20 * MS, 20 * MS, NULL, 0); ysig_frame(&g, &f);
        e = PEN1_E(30 * MS, 1, 4); ysig_feed(&g, &e);
        ysig_get_stats(&g, &st);
        CHECK(st.history_skipped == 1 && st.late_poll == 1);
        CHECK(ysig_value_of(&g, 0)->v[0] == 4);                       /* the sample flowed; the ring skipped it */
        CHECK(ysig_value_at(&g, 0, 34 * MS, &at) == 0);
    }
    /* only a clearly bad stamp changes anything: exactly 1 s past the poll
     * is believed, 1 ns more is not, also on a frame that lowers another
     * source's bad stamp (literals: the limit's value is part of the test) */
    {
        ysig_node m[3];
        m[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 1, 2);
        m[1] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 2, 2);
        m[2] = ysig_node_of(YSIG_OP_SILENT, 0, -1); m[2].p[0] = 0.1;
        d = desc_of(m, 3);
        if (!open_desc(&g, &d)) { g_failures++; return; }
        ysig_reset(&g, 0);
        e = PEN_E(1100 * MS, 1, 1, 0, YIN_TIER_SIM); ysig_feed(&g, &e);
        e = PEN_E(10100 * MS, 2, 1, 0, YIN_TIER_SIM); ysig_feed(&g, &e);
        f = frame_poll(1, 100 * MS, 100 * MS, NULL, 0); ysig_frame(&g, &f);
        e = PEN_E(200 * MS, 1, 2, 0, YIN_TIER_SIM); ysig_feed(&g, &e);   /* older than the believed stamp */
        ysig_get_stats(&g, &st);
        CHECK(st.dropped_order == 1 && ysig_value_of(&g, 0)->v[0] == 1);
        f = frame_poll(2, 1150 * MS, 1150 * MS, NULL, 0); ysig_frame(&g, &f);
        CHECK(ysig_value_of(&g, 2)->v[0] == 0);                         /* 50 ms after the believed stamp */
        ysig_reset(&g, 0);
        e = PEN_E(1100 * MS + 1, 1, 1, 0, YIN_TIER_SIM); ysig_feed(&g, &e);
        f = frame_poll(3, 100 * MS, 100 * MS, NULL, 0); ysig_frame(&g, &f);
        e = PEN_E(200 * MS, 1, 2, 0, YIN_TIER_SIM); ysig_feed(&g, &e);
        ysig_get_stats(&g, &st);
        CHECK(st.dropped_order == 1 && ysig_value_of(&g, 0)->v[0] == 2);  /* no new drop */
        f = frame_poll(4, 1150 * MS, 1150 * MS, NULL, 0); ysig_frame(&g, &f);
        CHECK(ysig_value_of(&g, 2)->v[0] == 1);                         /* 950 ms after the 200 ms sample */
    }
    /* no poll, no recovery: a graph with no view keeps dropping */
    n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 1, 2);
    d = desc_of(n, 1);
    if (!open_desc(&g, &d)) { g_failures++; return; }
    ysig_reset(&g, 0);
    e = PEN_E(3600000LL * MS, 1, 2, 0, YIN_TIER_SIM); ysig_feed(&g, &e);
    f = frame_poll(1, 20 * MS, 20 * MS, NULL, 0);
    f.has_poll = false;
    CHECK(ysig_frame(&g, &f) == 0);
    e = PEN_E(30 * MS, 1, 5, 0, YIN_TIER_SIM); ysig_feed(&g, &e);
    ysig_get_stats(&g, &st);
    CHECK(st.dropped_order == 1 && ysig_value_of(&g, 0)->v[0] == 2);
}

/* T7: a re-armed hit detect gives a new rise */
static void test_rearm(void) {
    ysig_node n[6];
    ysig_desc d;
    ysig_graph g;
    yin_event e;
    int through;
    g_where = "re-arm";
    for (through = 0; through < 2; through++) {
        int gate;
        n[0] = src_node(YIN_KIND_PEN, 0, 0, 1);
        n[1] = ysig_node_of(YSIG_OP_DETECT, 0, -1);
        n[1].p[YSIG_DETECT_LEVEL] = 5; n[1].p[YSIG_DETECT_OUT] = YSIG_OUT_HIT; n[1].p[YSIG_DETECT_START] = 1;
        n[2] = ysig_node_of(YSIG_OP_ALL, 1, 1);
        gate = through ? 2 : 1;
        n[3] = ysig_node_of(YSIG_OP_PEAK, 0, gate);
        d = desc_of(n, 4);
        if (!open_desc(&g, &d)) { g_failures++; return; }
        ysig_reset(&g, 0);
        e = PEN1_E(1 * MS, 1, 0); ysig_feed(&g, &e);
        ysig_arm(&g, 1, 2 * MS);
        e = PEN1_E(3 * MS, 1, 100); ysig_feed(&g, &e);
        CHECK(ysig_value_of(&g, 3)->v[0] == 100);
        e = PEN1_E(4 * MS, 1, 0); ysig_feed(&g, &e);
        ysig_arm(&g, 1, 5 * MS);
        /* it waits for input past 5 ms (v0.6.0): the output still stands */
        CHECK(ysig_value_of(&g, 1)->v[0] == 1);
        e = PEN1_E(6 * MS, 1, 6); ysig_feed(&g, &e);
        CHECK(ysig_value_of(&g, 3)->v[0] == 6 && ysig_value_of(&g, 3)->t == 6 * MS);
    }
}

/* G16: entry now. With no hold, a test true at the arm is a hit at the
 * arm's time; a frame-rate detect tests at the next onset; entry arm keeps
 * the crossing rule; a hit output rises at the arm, after a fall, so a
 * latch freezes on it; a reset that arms hits too; replay agrees. */
static void test_entry_now(void) {
    ysig_node n[4];
    ysig_desc d;
    ysig_graph g, g2;
    static uint64_t arena2[(1u << 20) / 8];
    ysig_stats s1, s2;
    ysig_hit h[8];
    ysig_value at;
    ysig_frame_in f;
    yin_event e;
    const ysig_param* rows;
    int nr;
    g_where = "entry now";
    rows = ysig_op_params(YSIG_OP_DETECT, &nr);
    CHECK(nr > YSIG_DETECT_ENTRY && rows[YSIG_DETECT_ENTRY].max == YSIG_ENTRY_NOW &&
          strstr(rows[YSIG_DETECT_ENTRY].values, ",now=2") != NULL);
    /* a sample-rate region: inside at the arm */
    n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 1, 2);
    n[1] = ysig_node_of(YSIG_OP_DETECT, 0, -1);
    n[1].p[YSIG_DETECT_TEST] = YSIG_TEST_LE; n[1].p[YSIG_DETECT_LANE] = -1; n[1].p[YSIG_DETECT_LEVEL] = 5;
    n[1].p[YSIG_DETECT_START] = 1; n[1].p[YSIG_DETECT_ENTRY] = YSIG_ENTRY_NOW; n[1].p[YSIG_DETECT_OUT] = YSIG_OUT_HIT;
    n[1].history = 16;
    n[2] = ysig_node_of(YSIG_OP_LATCH, 0, 1);
    d = desc_of(n, 3);
    d.journal = on_journal;
    g_nj = 0;
    if (!open_desc(&g, &d)) { g_failures++; return; }
    ysig_reset(&g, 0);
    e = PEN_E(10 * MS, 1, 1, 2, YIN_TIER_2); ysig_feed(&g, &e);
    CHECK(ysig_detected(&g, h, 8) == 0);                       /* not armed yet */
    /* input is in up to 15 ms, so an arm then is made at once (v0.6.0:
     * an arm ahead of the input waits for it) */
    CHECK(ysig_settle(&g, 15 * MS) == 0);
    CHECK(ysig_arm(&g, 1, 15 * MS));
    CHECK(ysig_detected(&g, h, 8) == 1 && h[0].t == 15 * MS && h[0].t_found == 15 * MS && h[0].node == 1);
    CHECK(h[0].value.v[0] == 1 && h[0].value.v[1] == 2 && h[0].value.t == 10 * MS);
    CHECK(ysig_value_of(&g, 1)->v[0] == 1 && ysig_value_of(&g, 1)->t == 15 * MS);
    CHECK(ysig_value_of(&g, 2)->v[0] == 1 && ysig_value_of(&g, 2)->v[1] == 2);   /* the latch froze */
    CHECK(ysig_value_at(&g, 1, 14 * MS, &at) == 1 && at.v[0] == 0);
    CHECK(ysig_value_at(&g, 1, 15 * MS, &at) == 1 && at.v[0] == 1);
    e = PEN_E(20 * MS, 1, 3, 0, YIN_TIER_2); ysig_feed(&g, &e);
    e = PEN_E(21 * MS, 1, 9, 0, YIN_TIER_2); ysig_feed(&g, &e);
    e = PEN_E(22 * MS, 1, 3, 0, YIN_TIER_2); ysig_feed(&g, &e);
    CHECK(ysig_detected(&g, h, 8) == 0);                       /* one hit per arm */
    CHECK(ysig_value_of(&g, 2)->v[0] == 1);                    /* still frozen */
    /* a re-arm while the output stands: a fall, then a rise, both at the arm */
    CHECK(ysig_settle(&g, 25 * MS) == 0);
    CHECK(ysig_arm(&g, 1, 25 * MS));
    CHECK(ysig_detected(&g, h, 8) == 1 && h[0].t == 25 * MS && h[0].value.v[0] == 3);
    CHECK(ysig_value_of(&g, 1)->v[0] == 1);
    /* outside at the arm: no hit; the first value inside is one (ready) */
    e = PEN_E(30 * MS, 1, 9, 0, YIN_TIER_2); ysig_feed(&g, &e);
    CHECK(ysig_settle(&g, 35 * MS) == 0);
    CHECK(ysig_arm(&g, 1, 35 * MS));
    CHECK(ysig_detected(&g, h, 8) == 0 && ysig_value_of(&g, 1)->v[0] == 0);
    e = PEN_E(40 * MS, 1, 4, 0, YIN_TIER_2); ysig_feed(&g, &e);
    CHECK(ysig_detected(&g, h, 8) == 1 && h[0].t == 40 * MS);
    /* replay of the journal: the same hits and values */
    ysig_get_stats(&g, &s1);
    d.journal = NULL;
    d.arena = arena2;
    d.arena_size = sizeof arena2;
    CHECK(ysig_open(&g2, &d));
    CHECK(ysig_replay(&g2, g_j, g_nj) == g_nj);
    ysig_get_stats(&g2, &s2);
    CHECK(s1.digest == s2.digest && s1.hits == s2.hits && s1.hits == 3);
    CHECK(!memcmp(ysig_value_of(&g, 2), ysig_value_of(&g2, 2), sizeof(ysig_value)));
    /* entry arm with no hold: the crossing rule, no hit at the arm */
    n[1].p[YSIG_DETECT_ENTRY] = YSIG_ENTRY_ARM;
    d = desc_of(n, 3);
    if (!open_desc(&g, &d)) { g_failures++; return; }
    ysig_reset(&g, 0);
    e = PEN_E(10 * MS, 1, 1, 2, YIN_TIER_2); ysig_feed(&g, &e);
    CHECK(ysig_arm(&g, 1, 15 * MS) && ysig_detected(&g, h, 8) == 0 && ysig_value_of(&g, 1)->v[0] == 0);
    e = PEN_E(20 * MS, 1, 3, 0, YIN_TIER_2); ysig_feed(&g, &e);
    CHECK(ysig_detected(&g, h, 8) == 0);
    /* a reset that arms (start reset): a hit at the reset's time, output 1 */
    n[1].p[YSIG_DETECT_ENTRY] = YSIG_ENTRY_NOW; n[1].p[YSIG_DETECT_START] = 0;
    n[1].p[YSIG_DETECT_TEST] = YSIG_TEST_GE; n[1].p[YSIG_DETECT_LEVEL] = -1;   /* always true */
    d = desc_of(n, 2);
    if (!open_desc(&g, &d)) { g_failures++; return; }
    ysig_reset(&g, 7 * MS);
    /* v0.4.1: the source has no sample after the reset: no hit there; the
     * first sample is tested, ready (test_review_fixes has the held case) */
    CHECK(ysig_detected(&g, h, 8) == 0 && ysig_value_of(&g, 1)->v[0] == 0);
    e = PEN_E(8 * MS, 1, 0, 0, YIN_TIER_2); ysig_feed(&g, &e);
    CHECK(ysig_detected(&g, h, 8) == 1 && h[0].t == 8 * MS);
    CHECK(ysig_value_of(&g, 1)->v[0] == 1 && ysig_value_of(&g, 1)->t == 8 * MS);
    /* frame rate: armed between frames, a hit at the next onset */
    n[1] = ysig_node_of(YSIG_OP_DETECT, 0, -1);
    n[1].p[YSIG_DETECT_TEST] = YSIG_TEST_LE; n[1].p[YSIG_DETECT_LANE] = -1; n[1].p[YSIG_DETECT_LEVEL] = 5;
    n[1].p[YSIG_DETECT_START] = 1; n[1].p[YSIG_DETECT_ENTRY] = YSIG_ENTRY_NOW; n[1].p[YSIG_DETECT_AT] = 1;
    d = desc_of(n, 2);
    if (!open_desc(&g, &d)) { g_failures++; return; }
    ysig_reset(&g, 0);
    e = PEN_E(10 * MS, 1, 1, 0, YIN_TIER_2); ysig_feed(&g, &e);
    CHECK(ysig_arm(&g, 1, 15 * MS) && ysig_detected(&g, h, 8) == 0);
    f = frame_at(1, 20 * MS, 0, NULL, 0);
    ysig_frame(&g, &f);
    CHECK(ysig_detected(&g, h, 8) == 1 && h[0].t == 20 * MS);
    /* the same with entry arm: held at the arm, no hit at the onset */
    n[1].p[YSIG_DETECT_ENTRY] = YSIG_ENTRY_ARM;
    d = desc_of(n, 2);
    if (!open_desc(&g, &d)) { g_failures++; return; }
    ysig_reset(&g, 0);
    e = PEN_E(10 * MS, 1, 1, 0, YIN_TIER_2); ysig_feed(&g, &e);
    CHECK(ysig_arm(&g, 1, 15 * MS));
    ysig_frame(&g, &f);
    CHECK(ysig_detected(&g, h, 8) == 0);
    /* with a hold, now is arm: the hold counts from the arm */
    n[1] = ysig_node_of(YSIG_OP_DETECT, 0, -1);
    n[1].p[YSIG_DETECT_TEST] = YSIG_TEST_LE; n[1].p[YSIG_DETECT_LANE] = -1; n[1].p[YSIG_DETECT_LEVEL] = 5;
    n[1].p[YSIG_DETECT_START] = 1; n[1].p[YSIG_DETECT_ENTRY] = YSIG_ENTRY_NOW; n[1].p[YSIG_DETECT_HOLD] = 0.1;
    d = desc_of(n, 2);
    if (!open_desc(&g, &d)) { g_failures++; return; }
    ysig_reset(&g, 0);
    e = PEN_E(10 * MS, 1, 1, 0, YIN_TIER_2); ysig_feed(&g, &e);
    CHECK(ysig_arm(&g, 1, 15 * MS) && ysig_detected(&g, h, 8) == 0);
    f = frame_at(1, 115 * MS, 0, NULL, 0);
    ysig_frame(&g, &f);
    CHECK(ysig_detected(&g, h, 8) == 1 && h[0].t == 115 * MS);
    /* past the table: refused at open */
    n[1].p[YSIG_DETECT_ENTRY] = 3;
    expect_refused(n, 2, "entry");
}

/* v0.6.0: an arm in the past (AN ARM IN THE PAST). With no window (a
 * zeroed desc) it tests the newest value, counted; with one, the graph runs
 * again from a checkpoint with the arm where it would have been made:
 * several hits each with its rise (a latch freezes at the first), the ring
 * as live feeding would have it, a found hit not handed out twice, a view's
 * frame values; ysig_settle() completes a hold; a replay agrees; an arm
 * past the window, logs that ran full, a retracted hit and changed rows are
 * counted by name; the desc's limits are refused. tests/adapt/sig_arm_diff.c
 * is the randomized differential. */
static void sig_window(ysig_desc* d, int frames, int calls) {
    d->catchup_frames = frames;
    d->catchup_calls = calls;
    d->catchup_writes = 4096;
}
/* The third review of 0.6.0 (2026-11-13): one case per finding, from the
 * reviewer's reproducers (scratchpad/sigrev), with the window of 4. */
static ysig_graph* g_reent_g;
static int g_reent_ok = -1;
static void on_row_reenter(void* u, const ysig_row* r) {
    (void)u; (void)r;
    if (g_reent_ok < 0) g_reent_ok = ysig_arm(g_reent_g, 1, 1 * MS) ? 1 : 0;
}
/* The semantics of an arm in the past (docs/sig.md), its own examples: the
 * second review's r11, r11b, r14, r15, and rule 3's cancels, the pinned
 * count, final-only rows and derived events, and the images' 64 hits. */
static int g_sp_rows;
static int64_t g_sp_row_t[64];
static double g_sp_row_v[64];
static void on_row_sp(void* u, const ysig_row* r) {
    (void)u;
    if (g_sp_rows < 64) { g_sp_row_t[g_sp_rows] = r->value.t; g_sp_row_v[g_sp_rows] = r->value.v[0]; }
    g_sp_rows++;
}
static int sp_hits(ysig_graph* g, ysig_hit* h, int node, int64_t t) {
    int n = ysig_detected(g, h, 64), k, found = 0;
    for (k = 0; k < n; k++) if (h[k].node == node && h[k].t == t) found++;
    return n == 1 && found == 1;
}
static void test_arm_spec(void) {
    static uint64_t ar[(4u << 20) / 8];
    ysig_node n[6];
    ysig_desc d;
    ysig_graph g;
    ysig_stats st;
    ysig_hit h[64];
    ysig_frame_in f;
    yin_event e;
    int64_t t;
    int what;
    g_where = "arm semantics";
#define SP_OPEN(nn, K) do { d = desc_of(n, (nn)); if (K) sig_window(&d, (K), 1024); d.arena = ar; d.arena_size = sizeof ar; \
        if (!ysig_open(&g, &d)) { printf("open: %s\n", ysig_error(&g)); g_failures++; return; } ysig_reset(&g, 0); } while (0)
    /* r11 (A): the hand crosses at 21 ms; then, after input later than
     * 20 ms, nothing, a set of an unrelated node, a late report of a
     * device that does not feed the detect, or a disarm of the detect;
     * then an arm at 20 ms. The set and the disarm are the arm's group and
     * move with it; the late report's footprint is disjoint. Each: the
     * hit at 21 ms, run again */
    for (what = 0; what < 4; what++) {
        n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 1, 2); n[0].history = 64;
        n[1] = ysig_node_of(YSIG_OP_DETECT, 0, -1);
        n[1].p[YSIG_DETECT_LANE] = 0; n[1].p[YSIG_DETECT_LEVEL] = 5; n[1].p[YSIG_DETECT_START] = 1;
        n[2] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 2, 2); n[2].history = 64;
        n[3] = ysig_node_of(YSIG_OP_OFFSET, 2, -1);
        SP_OPEN(4, 4);
        e = PEN_E(1 * MS, 1, 0, 0, YIN_TIER_2); ysig_feed(&g, &e);
        e = PEN_E(1 * MS, 2, 0, 0, YIN_TIER_2); ysig_feed(&g, &e);
        f = frame_at(1, 16 * MS, 0, NULL, 0); ysig_frame(&g, &f);
        e = PEN_E(21 * MS, 1, 10, 0, YIN_TIER_2); ysig_feed(&g, &e);
        e = PEN_E(25 * MS, 1, 0, 0, YIN_TIER_2); ysig_feed(&g, &e);
        if (what == 1) CHECK(ysig_set(&g, 3, YSIG_OFFSET_D_X, 1));
        if (what == 2) { e = PEN_E(19 * MS, 2, 0, 0, YIN_TIER_2); ysig_feed(&g, &e); }
        if (what == 3) CHECK(ysig_disarm(&g, 1));
        CHECK(ysig_arm(&g, 1, 20 * MS));
        CHECK(sp_hits(&g, h, 1, 21 * MS));
        ysig_get_stats(&g, &st);
        CHECK(st.arm_caught_up == 1 && st.arm_past_pinned == 0 && st.arm_hits_retracted == 0);
    }
    /* v0.7.0: the same with the late report from a second device on the
     * detect's own source (any device). In 0.6.x it stayed after the 21 ms
     * sample (the arm pinned behind it, no hit); now it goes to its place
     * by key, before the 21 ms sample (a run again), and the arm at 20
     * goes between them: the hit at 21 ms */
    n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 0, 2); n[0].history = 64;
    n[1] = ysig_node_of(YSIG_OP_DETECT, 0, -1);
    n[1].p[YSIG_DETECT_LANE] = 0; n[1].p[YSIG_DETECT_LEVEL] = 5; n[1].p[YSIG_DETECT_START] = 1;
    SP_OPEN(2, 4);
    e = PEN_E(1 * MS, 1, 0, 0, YIN_TIER_2); ysig_feed(&g, &e);
    f = frame_at(1, 16 * MS, 0, NULL, 0); ysig_frame(&g, &f);
    e = PEN_E(21 * MS, 1, 10, 0, YIN_TIER_2); ysig_feed(&g, &e);
    e = PEN_E(19 * MS, 2, 0, 0, YIN_TIER_2); ysig_feed(&g, &e);
    CHECK(ysig_arm(&g, 1, 20 * MS) && sp_hits(&g, h, 1, 21 * MS));
    ysig_get_stats(&g, &st);
    CHECK(st.arm_past_pinned == 0 && st.late_far == 0 && st.late_full == 0);
    /* r11b: a 15 ms hold; in at 19 ms, out 21 to 29, in at 30; a set of
     * an unrelated node, then the arm at 20 ms: the hold starts at 30 ms
     * (out at 20 by the 19 ms value? in: the hold from 20 breaks at 21),
     * so the hit is at 45 ms, not 35 */
    for (what = 0; what < 2; what++) {
        n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 1, 2); n[0].history = 64;
        n[1] = ysig_node_of(YSIG_OP_DETECT, 0, -1);
        n[1].p[YSIG_DETECT_LANE] = 0; n[1].p[YSIG_DETECT_LEVEL] = 5; n[1].p[YSIG_DETECT_START] = 1;
        n[1].p[YSIG_DETECT_HOLD] = 0.015;
        n[2] = ysig_node_of(YSIG_OP_OFFSET, 0, -1);
        SP_OPEN(3, 4);
        e = PEN_E(1 * MS, 1, 0, 0, YIN_TIER_2); ysig_feed(&g, &e);
        f = frame_at(1, 16 * MS, 0, NULL, 0); ysig_frame(&g, &f);
        e = PEN_E(19 * MS, 1, 10, 0, YIN_TIER_2); ysig_feed(&g, &e);
        for (t = 21 * MS; t <= 29 * MS; t += MS) { e = PEN_E(t, 1, 0, 0, YIN_TIER_2); ysig_feed(&g, &e); }
        e = PEN_E(30 * MS, 1, 10, 0, YIN_TIER_2); ysig_feed(&g, &e);
        if (what) CHECK(ysig_set(&g, 2, YSIG_OFFSET_D_X, 1));
        CHECK(ysig_arm(&g, 1, 20 * MS));
        for (t = 31 * MS; t <= 50 * MS; t += MS) { e = PEN_E(t, 1, 10, 0, YIN_TIER_2); ysig_feed(&g, &e); }
        CHECK(sp_hits(&g, h, 1, 45 * MS));
    }
    /* r14 (D): a hold of 20 ms from 1 ms ends at 21 ms; the 30 ms sample
     * finds it. A latch behind delay(hit output, 0 s) froze at the hit;
     * a re-arm at 21 ms is a fall and a rise at 21 ms that reach the
     * latch through the delay, as samples: it stays frozen at 10 */
    n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 0, 2); n[0].history = 64;
    n[1] = ysig_node_of(YSIG_OP_DETECT, 0, -1);
    n[1].p[YSIG_DETECT_LANE] = 0; n[1].p[YSIG_DETECT_LEVEL] = 5; n[1].p[YSIG_DETECT_START] = 1;
    n[1].p[YSIG_DETECT_HOLD] = 0.02; n[1].p[YSIG_DETECT_OUT] = YSIG_OUT_HIT; n[1].history = 64;
    n[2] = ysig_node_of(YSIG_OP_DELAY, 1, -1); n[2].p[0] = 0; n[2].history = 64;
    n[3] = ysig_node_of(YSIG_OP_LATCH, 0, 2); n[3].history = 64;
    SP_OPEN(4, 4);
    for (t = 1 * MS; t <= 15 * MS; t += MS) {
        e = PEN_E(t, 1, 10, 0, YIN_TIER_2); ysig_feed(&g, &e);
        if (t == 1 * MS) CHECK(ysig_arm(&g, 1, 1 * MS));
    }
    f = frame_at(1, 16 * MS, 0, NULL, 0); ysig_frame(&g, &f);
    e = PEN_E(30 * MS, 1, 10, 0, YIN_TIER_2); ysig_feed(&g, &e);
    CHECK(sp_hits(&g, h, 1, 21 * MS));
    CHECK(ysig_arm(&g, 1, 21 * MS));
    e = PEN_E(40 * MS, 1, 50, 0, YIN_TIER_2); ysig_feed(&g, &e);
    CHECK(ysig_value_of(&g, 3)->v[0] == 10);
    /* r15 (E): an arm at 60 ms ahead of the input waits; frames at 16 and
     * 33 ms, a crossing at 41 ms, then an arm at 40 ms goes before it: a
     * hit at 41 ms, and the arm at 60 ms is made when input passes 60 ms */
    for (what = 1; what <= 4; what += 3) {
        n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 0, 2); n[0].history = 64;
        n[1] = ysig_node_of(YSIG_OP_DETECT, 0, -1);
        n[1].p[YSIG_DETECT_LANE] = 0; n[1].p[YSIG_DETECT_LEVEL] = 5; n[1].p[YSIG_DETECT_START] = 1;
        SP_OPEN(2, what);
        e = PEN_E(1 * MS, 1, 0, 0, YIN_TIER_2); ysig_feed(&g, &e);
        CHECK(ysig_arm(&g, 1, 60 * MS));
        f = frame_at(1, 16 * MS, 0, NULL, 0); ysig_frame(&g, &f);
        f = frame_at(2, 33 * MS, 0, NULL, 0); ysig_frame(&g, &f);
        e = PEN_E(41 * MS, 1, 10, 0, YIN_TIER_2); ysig_feed(&g, &e);
        CHECK(ysig_detected(&g, h, 64) == 0);   /* not armed: the arm at 60 ms waits */
        e = PEN_E(45 * MS, 1, 0, 0, YIN_TIER_2); ysig_feed(&g, &e);
        CHECK(ysig_arm(&g, 1, 40 * MS));
        CHECK(sp_hits(&g, h, 1, 41 * MS));
        CHECK(((ysig__det_st*)g.nd[1].st)->t_arm == 40 * MS);
        e = PEN_E(70 * MS, 1, 0, 0, YIN_TIER_2); ysig_feed(&g, &e);
        CHECK(((ysig__det_st*)g.nd[1].st)->t_arm == 60 * MS);
    }
    /* rule 3: a disarm cancels a waiting arm of its detect with a later
     * time. Alone (the input time), and in an exit's group (the next
     * state's arm at 50 ms, before the waiting 60 ms) */
    for (what = 0; what < 2; what++) {
        n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 0, 2); n[0].history = 64;
        n[1] = ysig_node_of(YSIG_OP_DETECT, 0, -1);
        n[1].p[YSIG_DETECT_LANE] = 0; n[1].p[YSIG_DETECT_LEVEL] = 5; n[1].p[YSIG_DETECT_START] = 1;
        n[2] = ysig_node_of(YSIG_OP_DETECT, 0, -1);
        n[2].p[YSIG_DETECT_LANE] = 0; n[2].p[YSIG_DETECT_LEVEL] = 100; n[2].p[YSIG_DETECT_START] = 1;
        SP_OPEN(3, 4);
        e = PEN_E(40 * MS, 1, 0, 0, YIN_TIER_2); ysig_feed(&g, &e);
        CHECK(ysig_arm(&g, 1, 60 * MS));
        CHECK(ysig_disarm(&g, 1));
        if (what) CHECK(ysig_arm(&g, 2, 50 * MS));
        e = PEN_E(70 * MS, 1, 0, 0, YIN_TIER_2); ysig_feed(&g, &e);
        e = PEN_E(80 * MS, 1, 10, 0, YIN_TIER_2); ysig_feed(&g, &e);
        CHECK(ysig_detected(&g, h, 64) == 0);
        ysig_get_stats(&g, &st);
        CHECK(st.arm_ahead_cancelled == 1 && !((ysig__det_st*)g.nd[1].st)->armed);
    }
    /* a waiting arm goes in before a later state call that meets it,
     * also when input another detect reads moved the input time past its
     * time: a disarm then goes after the arm (made, then disarmed: no
     * cancel), and a set applies after the arm */
    for (what = 0; what < 2; what++) {
        n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 1, 2); n[0].history = 64;
        n[1] = ysig_node_of(YSIG_OP_DETECT, 0, -1);
        n[1].p[YSIG_DETECT_LANE] = 0; n[1].p[YSIG_DETECT_LEVEL] = 5; n[1].p[YSIG_DETECT_START] = 1;
        n[2] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 2, 2); n[2].history = 64;
        SP_OPEN(3, 4);
        e = PEN_E(40 * MS, 1, 0, 0, YIN_TIER_2); ysig_feed(&g, &e);
        CHECK(ysig_arm(&g, 1, 60 * MS) && g.wt->n == 1);
        e = PEN_E(70 * MS, 2, 0, 0, YIN_TIER_2); ysig_feed(&g, &e);
        CHECK(g.wt->n == 1);   /* that input shares no node with it */
        if (what == 0) CHECK(ysig_disarm(&g, 1));
        else CHECK(ysig_set(&g, 1, YSIG_DETECT_LEVEL, 100));
        /* v0.7.0: a plain disarm is at N (70 ms, the ruling): it lets the
         * arm in first. A set is at N(F(c)), 40 ms (only device 1 reaches
         * the detect): before the arm's 60 ms, it lets nothing in, and the
         * arm still waits ("The time of a call") */
        if (what == 0) CHECK(g.wt->n == 0 && ((ysig__det_st*)g.nd[1].st)->t_arm == 60 * MS);
        else CHECK(g.wt->n == 1);
        /* a disarm takes effect at the end of its gap (S1): the next input */
        e = PEN_E(75 * MS, 2, 0, 0, YIN_TIER_2); ysig_feed(&g, &e);
        ysig_get_stats(&g, &st);
        CHECK(st.arm_ahead_cancelled == 0);
        /* the disarm ended the arm; after the set the arm still waits
         * (device 2's input does not reach it) */
        CHECK(!((ysig__det_st*)g.nd[1].st)->armed && g.wt->n == (what == 1));
    }
    /* a barrier cancels every waiting arm; its group's set goes in first */
    n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 0, 2); n[0].history = 64;
    n[1] = ysig_node_of(YSIG_OP_DETECT, 0, -1);
    n[1].p[YSIG_DETECT_LANE] = 0; n[1].p[YSIG_DETECT_LEVEL] = 5; n[1].p[YSIG_DETECT_START] = 1;
    SP_OPEN(2, 4);
    e = PEN_E(40 * MS, 1, 0, 0, YIN_TIER_2); ysig_feed(&g, &e);
    CHECK(ysig_set(&g, 1, YSIG_DETECT_LEVEL, 7));
    CHECK(ysig_arm(&g, 1, 60 * MS));
    CHECK(g.nd[1].p[YSIG_DETECT_LEVEL] == 5);   /* the set waits with its arm */
    ysig_reset_trial(&g, 100 * MS);
    ysig_get_stats(&g, &st);
    CHECK(st.arm_ahead_cancelled == 1 && g.nd[1].p[YSIG_DETECT_LEVEL] == 7);
    e = PEN_E(110 * MS, 1, 10, 0, YIN_TIER_2); ysig_feed(&g, &e);
    CHECK(ysig_detected(&g, h, 64) == 0);   /* START 1: not armed after the reset */
    /* the input time after a reset is its time: an arm then is made at
     * once, not waiting */
    SP_OPEN(2, 4);
    e = PEN_E(10 * MS, 1, 10, 0, YIN_TIER_2); ysig_feed(&g, &e);
    ysig_reset_trial(&g, 20 * MS);
    CHECK(ysig_arm(&g, 1, 20 * MS) && g.wt->n == 0);
    CHECK(((ysig__det_st*)g.nd[1].st)->armed && ((ysig__det_st*)g.nd[1].st)->t_arm == 20 * MS);
    CHECK(ysig_arm(&g, 1, 21 * MS) && g.wt->n == 1);
    /* final-only rows: a record of the hit output; its rows come once no
     * arm can change them (catchup_frames frames later, or at a flush),
     * and a past arm that changes them counts nothing stale */
    n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 1, 2); n[0].history = 64;
    n[1] = ysig_node_of(YSIG_OP_DETECT, 0, -1);
    n[1].p[YSIG_DETECT_LANE] = 0; n[1].p[YSIG_DETECT_LEVEL] = 5; n[1].p[YSIG_DETECT_START] = 1;
    n[1].p[YSIG_DETECT_OUT] = YSIG_OUT_HIT; n[1].history = 64;
    n[2] = ysig_node_of(YSIG_OP_RECORD, 1, -1);
    n[3] = ysig_node_of(YSIG_OP_EMIT, 1, -1);
    d = desc_of(n, 4); sig_window(&d, 2, 1024); d.rows_final = true; d.derived_final = true; d.on_row = on_row_sp;
    d.arena = ar; d.arena_size = sizeof ar;
    CHECK(ysig_open(&g, &d));
    ysig_reset(&g, 0);
    g_sp_rows = 0;
    e = PEN_E(1 * MS, 1, 0, 0, YIN_TIER_2); ysig_feed(&g, &e);
    e = PEN_E(21 * MS, 1, 10, 0, YIN_TIER_2); ysig_feed(&g, &e);
    e = PEN_E(25 * MS, 1, 0, 0, YIN_TIER_2); ysig_feed(&g, &e);
    CHECK(g_sp_rows == 0 && ysig_derived(&g, &e, 1) == 0);   /* held */
    CHECK(ysig_arm(&g, 1, 20 * MS) && sp_hits(&g, h, 1, 21 * MS));
    ysig_get_stats(&g, &st);
    CHECK(st.arm_caught_up == 1 && st.arm_rows_stale == 0 && st.arm_derived_stale == 0);
    f = frame_at(1, 30 * MS, 0, NULL, 0); ysig_frame(&g, &f);
    CHECK(g_sp_rows == 0);   /* the reset's checkpoint is still the oldest */
    f = frame_at(2, 40 * MS, 0, NULL, 0); ysig_frame(&g, &f);
    CHECK(g_sp_rows == 3 && g_sp_row_v[1] == 1 && g_sp_row_t[1] == 21 * MS && g_sp_row_v[2] == 1);
    CHECK(ysig_derived(&g, &e, 1) == 1 && e.t == 1 * MS);
    e = PEN_E(41 * MS, 1, 0, 0, YIN_TIER_2); ysig_feed(&g, &e);
    CHECK(ysig_flush(&g) && g_sp_rows == 4);
    CHECK(ysig_arm(&g, 1, 35 * MS));   /* a barrier: placed after it, made where it is */
    ysig_get_stats(&g, &st);
    CHECK(st.arm_past_far == 0 && st.arm_caught_up == 1 && g_sp_rows == 4);
    /* an image keeps 64 hits, as the queue outside a run again */
    CHECK(g.cu->img_size < sizeof(ysig_hit) * 1024);
#undef SP_OPEN
}

/* More of the semantics, one case per rule the mutation list reaches. */
static void test_arm_spec2(void) {
    static uint64_t ar[(8u << 20) / 8];
    ysig_node n[40];
    ysig_desc d;
    ysig_graph g;
    ysig_stats st;
    ysig_hit h[256];
    ysig_frame_in f;
    yin_event e;
    int64_t t;
    int k, m;
    g_where = "arm semantics 2";
#define SP_OPEN(nn, K) do { d = desc_of(n, (nn)); if (K) sig_window(&d, (K), 1024); d.arena = ar; d.arena_size = sizeof ar; \
        if (!ysig_open(&g, &d)) { printf("open: %s\n", ysig_error(&g)); g_failures++; return; } ysig_reset(&g, 0); } while (0)
    /* an arm made at its time, then a late report before that time: before
     * the arm, no hit (a crossing), and a failing one breaks the hold */
    n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 0, 2); n[0].history = 64;
    n[1] = ysig_node_of(YSIG_OP_DETECT, 0, -1);
    n[1].p[YSIG_DETECT_LANE] = 0; n[1].p[YSIG_DETECT_LEVEL] = 5; n[1].p[YSIG_DETECT_START] = 1;
    SP_OPEN(2, 4);
    e = PEN_E(10 * MS, 1, 0, 0, YIN_TIER_2); ysig_feed(&g, &e);
    CHECK(ysig_settle(&g, 20 * MS) == 0 && ysig_arm(&g, 1, 20 * MS));
    e = PEN_E(15 * MS, 2, 9, 0, YIN_TIER_2); ysig_feed(&g, &e);
    CHECK(ysig_detected(&g, h, 256) == 0);
    n[1].p[YSIG_DETECT_HOLD] = 0.02;
    SP_OPEN(2, 4);
    e = PEN_E(10 * MS, 1, 9, 0, YIN_TIER_2); ysig_feed(&g, &e);
    CHECK(ysig_settle(&g, 20 * MS) == 0 && ysig_arm(&g, 1, 20 * MS));   /* holding from 20 ms */
    e = PEN_E(15 * MS, 2, 0, 0, YIN_TIER_2); ysig_feed(&g, &e);           /* late: the hand was out */
    e = PEN_E(45 * MS, 1, 9, 0, YIN_TIER_2); ysig_feed(&g, &e);
    CHECK(ysig_detected(&g, h, 256) == 0);
    n[1].p[YSIG_DETECT_HOLD] = 0;
    /* rule 2: a set in no group at or before the arm's time, sharing its
     * node, comes before it: an entry-now arm at 20 ms tests the new level */
    n[1].p[YSIG_DETECT_ENTRY] = YSIG_ENTRY_NOW;
    SP_OPEN(2, 4);
    e = PEN_E(10 * MS, 1, 3, 0, YIN_TIER_2); ysig_feed(&g, &e);
    CHECK(ysig_set(&g, 1, YSIG_DETECT_LEVEL, 2));
    e = PEN_E(30 * MS, 1, 3, 0, YIN_TIER_2); ysig_feed(&g, &e);
    CHECK(ysig_arm(&g, 1, 20 * MS) && sp_hits(&g, h, 1, 20 * MS));
    n[1].p[YSIG_DETECT_ENTRY] = YSIG_ENTRY_ARM;
    /* a group's footprint has its set's nodes: input of the set's node at
     * or before t, made after the 21 ms crossing, comes before the group.
     * v0.7.0 (R by key, L2): the group goes before the crossing all the
     * same (the 19 ms input meets the set, not the crossing): not pinned,
     * the arm at 20 is ready on the held 0 and the crossing is its hit */
    n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 1, 2); n[0].history = 64;
    n[2] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 2, 2); n[2].history = 64;
    n[3] = ysig_node_of(YSIG_OP_OFFSET, 2, -1);
    SP_OPEN(4, 4);
    e = PEN_E(1 * MS, 1, 0, 0, YIN_TIER_2); ysig_feed(&g, &e);
    f = frame_at(1, 16 * MS, 0, NULL, 0); ysig_frame(&g, &f);
    e = PEN_E(21 * MS, 1, 10, 0, YIN_TIER_2); ysig_feed(&g, &e);
    e = PEN_E(19 * MS, 2, 0, 0, YIN_TIER_2); ysig_feed(&g, &e);
    CHECK(ysig_set(&g, 3, YSIG_OFFSET_D_X, 1));
    CHECK(ysig_arm(&g, 1, 20 * MS) && sp_hits(&g, h, 1, 21 * MS));
    ysig_get_stats(&g, &st);
    CHECK(st.arm_past_pinned == 0);
    /* rule 3: input at the waiting arm's own time goes before it; a later
     * one lets it in */
    SP_OPEN(2, 4);
    e = PEN_E(40 * MS, 1, 0, 0, YIN_TIER_2); ysig_feed(&g, &e);
    CHECK(ysig_arm(&g, 1, 60 * MS) && g.wt->n == 1);
    e = PEN_E(60 * MS, 1, 0, 0, YIN_TIER_2); ysig_feed(&g, &e);
    CHECK(g.wt->n == 1);
    e = PEN_E(61 * MS, 1, 0, 0, YIN_TIER_2); ysig_feed(&g, &e);
    CHECK(g.wt->n == 0);
    /* a waiting list full (32): an arm made at once meets the old hold
     * only on the input known (here 1 ms), not on unfed time */
    n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 1, 2);
    for (k = 1; k < 35; k++) {
        n[k] = ysig_node_of(YSIG_OP_DETECT, 0, -1);
        n[k].p[YSIG_DETECT_LANE] = 0; n[k].p[YSIG_DETECT_LEVEL] = 5;
    }
    n[1].p[YSIG_DETECT_HOLD] = 0.05;   /* armed at the reset, holding from 1 ms */
    SP_OPEN(35, 4);
    e = PEN_E(1 * MS, 1, 9, 0, YIN_TIER_2); ysig_feed(&g, &e);
    while (ysig_detected(&g, h, 256) > 0) { }   /* the others' hits at 1 ms */
    for (k = 2; k <= 33; k++) CHECK(ysig_arm(&g, k, 100 * MS));
    CHECK(g.wt->n == 32 && ysig_arm(&g, 1, 60 * MS) && g.wt->n == 32);
    ysig_get_stats(&g, &st);
    CHECK(st.arm_past_full == 1 && ysig_detected(&g, h, 256) == 0);
    /* and the same when a run again makes it again */
    e = PEN_E(70 * MS, 1, 9, 0, YIN_TIER_2); ysig_feed(&g, &e);
    while (ysig_detected(&g, h, 256) > 0) { }
    CHECK(ysig_arm(&g, 34, 65 * MS));
    ysig_get_stats(&g, &st);
    m = ysig_detected(&g, h, 256);
    for (k = 0; k < m; k++) CHECK(h[k].node != 1);
    CHECK(st.arm_caught_up == 1);
    /* a disarm at its time meets its detect's hold first: the hold of
     * 20 ms from 1 ms, found at 30 ms; then a group (disarm, an arm of
     * another detect) at 22 ms runs again: the hit at 21 ms stands */
    n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 1, 2); n[0].history = 64;
    n[1] = ysig_node_of(YSIG_OP_DETECT, 0, -1);
    n[1].p[YSIG_DETECT_LANE] = 0; n[1].p[YSIG_DETECT_LEVEL] = 5; n[1].p[YSIG_DETECT_HOLD] = 0.02;
    n[2] = ysig_node_of(YSIG_OP_DETECT, 0, -1);
    n[2].p[YSIG_DETECT_LANE] = 0; n[2].p[YSIG_DETECT_LEVEL] = 100; n[2].p[YSIG_DETECT_START] = 1;
    SP_OPEN(3, 4);
    for (t = 1 * MS; t <= 15 * MS; t += MS) { e = PEN_E(t, 1, 9, 0, YIN_TIER_2); ysig_feed(&g, &e); }
    f = frame_at(1, 16 * MS, 0, NULL, 0); ysig_frame(&g, &f);
    e = PEN_E(30 * MS, 1, 9, 0, YIN_TIER_2); ysig_feed(&g, &e);
    CHECK(sp_hits(&g, h, 1, 21 * MS));
    CHECK(ysig_disarm(&g, 1) && ysig_arm(&g, 2, 22 * MS));
    ysig_get_stats(&g, &st);
    CHECK(st.arm_caught_up == 1 && st.arm_hits_retracted == 0);
    /* a group that disarms a detect ends its arm at the group's time: that
     * detect's hits later than it were the ended arm's, superseded (a
     * caller that ends the arm took none), not retracted. The hold of
     * 20 ms from 1 ms, found at 30 ms; a group (disarm it, arm another) at
     * 18 ms voids the 21 ms hit */
    SP_OPEN(3, 4);
    for (t = 1 * MS; t <= 15 * MS; t += MS) { e = PEN_E(t, 1, 9, 0, YIN_TIER_2); ysig_feed(&g, &e); }
    f = frame_at(1, 16 * MS, 0, NULL, 0); ysig_frame(&g, &f);
    e = PEN_E(30 * MS, 1, 9, 0, YIN_TIER_2); ysig_feed(&g, &e);
    CHECK(sp_hits(&g, h, 1, 21 * MS));
    CHECK(ysig_disarm(&g, 1) && ysig_arm(&g, 2, 18 * MS));
    ysig_get_stats(&g, &st);
    CHECK(st.arm_caught_up == 1 && st.arm_hits_retracted == 0 && st.arm_hits_superseded == 1);
    /* v0.7.0: an image keeps no queue (a run again keeps the live queue's
     * entries made before its checkpoint): a run again that queues more
     * than 64, then a frame, keeps that frame's checkpoint (0.6.x did not) */
    n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 1, 2); n[0].history = 256;
    n[1] = ysig_node_of(YSIG_OP_DETECT, 0, -1);
    n[1].p[YSIG_DETECT_LANE] = 0; n[1].p[YSIG_DETECT_LEVEL] = 5; n[1].p[YSIG_DETECT_REPEAT] = 1;
    SP_OPEN(2, 4);
    for (k = 0; k < 200; k++) {
        e = PEN_E((int64_t)(k + 1) * 50000, 1, (k & 1) ? 9 : 0, 0, YIN_TIER_2); ysig_feed(&g, &e);
        while (ysig_detected(&g, h, 256) > 0) { }
    }
    CHECK(ysig_arm(&g, 1, 1));
    m = g.n_hit;
    k = g.cu->n_ck;
    f = frame_at(1, 16 * MS, 0, NULL, 0); ysig_frame(&g, &f);
    CHECK(m > 64 && g.cu->n_ck == k + 1);
    /* D: a re-arm's fall reaches a record of the hit output: a row of 0 at
     * the arm's time */
    n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 1, 2); n[0].history = 64;
    n[1] = ysig_node_of(YSIG_OP_DETECT, 0, -1);
    n[1].p[YSIG_DETECT_LANE] = 0; n[1].p[YSIG_DETECT_LEVEL] = 5; n[1].p[YSIG_DETECT_OUT] = YSIG_OUT_HIT;
    n[2] = ysig_node_of(YSIG_OP_RECORD, 1, -1);
    d = desc_of(n, 3); sig_window(&d, 4, 1024); d.arena = ar; d.arena_size = sizeof ar; d.on_row = on_row_sp;
    CHECK(ysig_open(&g, &d));
    ysig_reset(&g, 0);
    e = PEN_E(10 * MS, 1, 9, 0, YIN_TIER_2); ysig_feed(&g, &e);
    g_sp_rows = 0;
    CHECK(ysig_settle(&g, 12 * MS) == 0 && ysig_arm(&g, 1, 12 * MS));
    CHECK(g_sp_rows == 1 && g_sp_row_v[0] == 0 && g_sp_row_t[0] == 12 * MS);
#undef SP_OPEN
}

/* The independent oracle's reproducers (2026-11-13, the reviewer, from
 * the text): H1, H2, H4 to H8 and S1, one case each. */
static uint64_t g_rp_ar[(8u << 20) / 8];
static ysig_graph g_rp;
static int g_rp_rows, g_rp_ooo, g_rp_crows;
static int64_t g_rp_last;
static void rp_on_row(void* u, const ysig_row* r) { (void)u; if (r->frame <= g_rp_last) g_rp_ooo++; g_rp_last = r->frame; g_rp_rows++; }
static void rp_feed(int64_t t, double x, int ctl, int dev) {
    yin_event e;
    memset(&e, 0, sizeof e);
    e.t = t; e.type = YIN_SAMPLE; e.kind = YIN_KIND_PEN; e.control = (uint32_t)ctl; e.device = (uint32_t)dev;
    e.x = (float)x; e.value = (float)x; e.stamp = YIN_TIER_2;
    ysig_feed(&g_rp, &e);
}
static void rp_feed2(int64_t t, double x, double y, int dev) {
    yin_event e;
    memset(&e, 0, sizeof e);
    e.t = t; e.type = YIN_SAMPLE; e.kind = YIN_KIND_PEN; e.control = YIN_AXIS_POSITION; e.device = (uint32_t)dev;
    e.x = (float)x; e.y = (float)y; e.stamp = YIN_TIER_2;
    ysig_feed(&g_rp, &e);
}
static void rp_frame(int64_t o) {
    float v[1] = { 0 };
    ysig_frame_in f;
    memset(&f, 0, sizeof f);
    f.index = o / MS; f.onset_ns = o; f.poll_ns = o; f.has_poll = true; f.values = v; f.n_values = 1;
    ysig_frame(&g_rp, &f);
}
static int rp_open(ysig_node* n, int nn, int K, int calls, bool rows_final, bool derived_final) {
    ysig_desc d;
    memset(&d, 0, sizeof d);
    d.nodes = n; d.n_nodes = nn; d.arena = g_rp_ar; d.catchup_frames = K; d.catchup_calls = calls;
    d.rows_final = rows_final; d.derived_final = derived_final; d.on_row = rp_on_row;
    d.catchup_rows = g_rp_crows;
    d.channels = g_ch; d.n_channels = 64;
    d.arena_size = ysig_arena_size(&d);
    if (!d.arena_size || d.arena_size > sizeof g_rp_ar || !ysig_open(&g_rp, &d)) { g_failures++; return 0; }
    ysig_reset(&g_rp, 0);
    g_rp_rows = g_rp_ooo = 0;
    g_rp_last = -1;
    return 1;
}
/* The reviewer's 2192: a group's disarm takes effect at the group's end
 * (S1), after its other calls, in a run again too. The disarm meets d's
 * hold (a hit at 20 ms through latch(offset X, d)); the set of X in the
 * same group comes first, so the row has X's new value. A past arm then
 * runs the group again (final-only rows): the row is the same */
static double g_d2192_last;
static int g_d2192_n;
static void d2192_row(void* u, const ysig_row* r) { (void)u; if (r->node == 4) { g_d2192_last = r->value.v[0]; g_d2192_n++; } }
static void test_disarm_at_gap_end(void) {
    int K;
    g_where = "a group's disarm at its end";
    for (K = 0; K <= 4; K += 4) {
        ysig_node n[7];
        ysig_desc d;
        n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 1, 2); n[0].history = 64;
        n[1] = ysig_node_of(YSIG_OP_OFFSET, 0, -1);
        n[2] = ysig_node_of(YSIG_OP_DETECT, 0, -1); n[2].p[YSIG_DETECT_LANE] = 0; n[2].p[YSIG_DETECT_LEVEL] = 5; n[2].p[YSIG_DETECT_HOLD] = 0.01;
        n[2].p[YSIG_DETECT_START] = 1; n[2].p[YSIG_DETECT_OUT] = YSIG_OUT_HIT; n[2].history = 64;
        n[3] = ysig_node_of(YSIG_OP_LATCH, 1, 2); n[3].history = 64;
        n[4] = ysig_node_of(YSIG_OP_RECORD, 3, -1);
        n[5] = ysig_node_of(YSIG_OP_DETECT, 0, -1); n[5].p[YSIG_DETECT_LANE] = 0; n[5].p[YSIG_DETECT_LEVEL] = 5; n[5].p[YSIG_DETECT_START] = 1; n[5].history = 64;
        n[6] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 2, 2);
        memset(&d, 0, sizeof d);
        d.nodes = n; d.n_nodes = 7; d.arena = g_rp_ar; d.catchup_frames = K; d.rows_final = K > 0; d.on_row = d2192_row;
        d.arena_size = ysig_arena_size(&d);
        if (!d.arena_size || d.arena_size > sizeof g_rp_ar || !ysig_open(&g_rp, &d)) { g_failures++; return; }
        ysig_reset(&g_rp, 0);
        g_d2192_n = 0;
        rp_feed(10 * MS, 10, YIN_AXIS_POSITION, 1);
        ysig_arm(&g_rp, 2, 10 * MS);
        rp_feed(15 * MS, 10, YIN_AXIS_POSITION, 1);
        rp_feed(22 * MS, 0, YIN_AXIS_POSITION, 2);   /* N 22: the hold met by then */
        ysig_disarm(&g_rp, 2);
        ysig_set(&g_rp, 1, YSIG_OFFSET_D_X, 100);
        ysig_arm(&g_rp, 5, 22 * MS);
        ysig_arm(&g_rp, 5, 12 * MS);   /* past: a run again over the group */
        ysig_flush(&g_rp);
        CHECK(g_d2192_n == 3 && g_d2192_last == 110);
    }
    /* The same group ahead (its arm at 30 ms): it waits whole and goes in
     * at the 35 ms input, its disarm after its set there too (the oracle's
     * case 3578 of seed 31415: a waiting group's disarm made before its
     * recenters). With no window the set stays where made and the disarm
     * waits: R is the same here, as nothing lies between them */
    {
        int got_n[2] = { 0, 0 };
        double got_last[2] = { 0, 0 };
        for (K = 0; K <= 4; K += 4) {
            ysig_node n[7];
            ysig_desc d;
            n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 1, 2); n[0].history = 64;
            n[1] = ysig_node_of(YSIG_OP_OFFSET, 0, -1);
            n[2] = ysig_node_of(YSIG_OP_DETECT, 0, -1); n[2].p[YSIG_DETECT_LANE] = 0; n[2].p[YSIG_DETECT_LEVEL] = 5; n[2].p[YSIG_DETECT_HOLD] = 0.01;
            n[2].p[YSIG_DETECT_START] = 1; n[2].p[YSIG_DETECT_OUT] = YSIG_OUT_HIT; n[2].history = 64;
            n[3] = ysig_node_of(YSIG_OP_LATCH, 1, 2); n[3].history = 64;
            n[4] = ysig_node_of(YSIG_OP_RECORD, 3, -1);
            n[5] = ysig_node_of(YSIG_OP_DETECT, 0, -1); n[5].p[YSIG_DETECT_LANE] = 0; n[5].p[YSIG_DETECT_LEVEL] = 5; n[5].p[YSIG_DETECT_START] = 1; n[5].history = 64;
            n[6] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 2, 2);
            memset(&d, 0, sizeof d);
            d.nodes = n; d.n_nodes = 7; d.arena = g_rp_ar; d.catchup_frames = K; d.rows_final = K > 0; d.on_row = d2192_row;
            d.arena_size = ysig_arena_size(&d);
            if (!d.arena_size || d.arena_size > sizeof g_rp_ar || !ysig_open(&g_rp, &d)) { g_failures++; return; }
            ysig_reset(&g_rp, 0);
            g_d2192_n = 0;
            g_d2192_last = 0;
            rp_feed(10 * MS, 10, YIN_AXIS_POSITION, 1);
            ysig_arm(&g_rp, 2, 10 * MS);
            rp_feed(15 * MS, 10, YIN_AXIS_POSITION, 1);
            ysig_disarm(&g_rp, 2);
            ysig_set(&g_rp, 1, YSIG_OFFSET_D_X, 100);
            ysig_arm(&g_rp, 5, 30 * MS);   /* ahead: the group waits */
            rp_feed(35 * MS, 10, YIN_AXIS_POSITION, 1);
            ysig_flush(&g_rp);
            got_n[K / 4] = g_d2192_n;
            got_last[K / 4] = g_d2192_last;
        }
        CHECK(got_n[0] == got_n[1] && got_last[0] == got_last[1]);
    }
}

/* A hit is the same as one handed out only when its whole value is
 * (docs/sig.md, "Hits, rows and derived events"). B (hold 5 ms) reads a
 * mux that detect A switches from device 2 to device 1. A's arm in the
 * past (15 ms, before B's at 20) makes B's hit at 25 ms again with
 * device 1's sources: equal lanes and time, another value. The handed-out
 * hit is retracted and the new one queued (mutant 232 matched on the lanes
 * alone; the oracle's case 286 of seed 20261114, reduced) */
static void test_hit_whole_value(void) {
    ysig_node n[5];
    ysig_hit h[8];
    ysig_stats st;
    int m, k, found = 0;
    g_where = "a hit matched by its whole value";
    n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 1, 2); n[0].history = 64;
    n[1] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 2, 2); n[1].history = 64;
    n[2] = ysig_node_of(YSIG_OP_DETECT, 0, -1); n[2].p[YSIG_DETECT_LANE] = 0; n[2].p[YSIG_DETECT_LEVEL] = 5; n[2].p[YSIG_DETECT_START] = 1;
    n[2].p[YSIG_DETECT_ENTRY] = YSIG_ENTRY_NOW; n[2].p[YSIG_DETECT_OUT] = YSIG_OUT_HIT; n[2].history = 64;   /* 0 until its hit: the mux reads device 2 */
    n[3] = ysig_node_of(YSIG_OP_MUX, 0, 1); n[3].in[2] = 2; n[3].history = 64;
    n[4] = ysig_node_of(YSIG_OP_DETECT, 3, -1); n[4].p[YSIG_DETECT_LANE] = 0; n[4].p[YSIG_DETECT_LEVEL] = 5; n[4].p[YSIG_DETECT_HOLD] = 0.005;
    n[4].p[YSIG_DETECT_START] = 1; n[4].p[YSIG_DETECT_OUT] = YSIG_OUT_HIT; n[4].history = 64;
    if (!rp_open(n, 5, 4, 0, false, false)) return;
    rp_feed(10 * MS, 10, YIN_AXIS_POSITION, 1);
    rp_feed(10 * MS, 10, YIN_AXIS_POSITION, 2);
    rp_feed(20 * MS, 10, YIN_AXIS_POSITION, 1);
    rp_feed(20 * MS, 10, YIN_AXIS_POSITION, 2);
    ysig_arm(&g_rp, 4, 20 * MS);
    rp_feed(30 * MS, 10, YIN_AXIS_POSITION, 1);
    rp_feed(30 * MS, 10, YIN_AXIS_POSITION, 2);
    m = ysig_detected(&g_rp, h, 8);
    CHECK(m == 1 && h[0].node == 4 && h[0].t == 25 * MS && h[0].value.device == 2);
    ysig_arm(&g_rp, 2, 15 * MS);
    m = ysig_detected(&g_rp, h, 8);
    for (k = 0; k < m; k++) if (h[k].node == 4 && h[k].t == 25 * MS && h[k].value.device == 1) found++;
    ysig_get_stats(&g_rp, &st);
    CHECK(found == 1 && st.arm_hits_retracted == 1 && st.arm_hits_superseded == 0);
}

/* Exactness (docs/sig.md): arm_unverified, one shape per condition, each
 * with a control that stays 0. Nodes: s1 (any device), d1 on it, d2 on
 * d1 (so d2's arm meets d1's group);
 * s2 (device 3, pressure) and an offset X on it. */
static int64_t ex_unv(void) {
    ysig_stats st;
    ysig_get_stats(&g_rp, &st);
    return st.arm_unverified;
}
static int ex_open(ysig_node* n, int K) {
    n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 0, 2); n[0].history = 64;   /* any device */
    n[1] = ysig_node_of(YSIG_OP_DETECT, 0, -1); n[1].p[YSIG_DETECT_LANE] = 0; n[1].p[YSIG_DETECT_LEVEL] = 5; n[1].p[YSIG_DETECT_START] = 1; n[1].history = 64;
    n[2] = ysig_node_of(YSIG_OP_DETECT, 1, -1); n[2].p[YSIG_DETECT_LANE] = 0; n[2].p[YSIG_DETECT_LEVEL] = 0.5; n[2].p[YSIG_DETECT_START] = 1; n[2].history = 64;
    n[3] = src_node(YIN_KIND_PEN, YIN_AXIS_PEN_PRESSURE, 3, 1);
    n[4] = ysig_node_of(YSIG_OP_OFFSET, 3, -1);
    if (!rp_open(n, 5, K, 0, false, false)) return 0;
    rp_feed(10 * MS, 0, YIN_AXIS_POSITION, 1);
    rp_feed(10 * MS, 0, YIN_AXIS_PEN_PRESSURE, 3);
    return 1;
}
/* Two hands, each a device: lh (device 1) and rh (device 2), d on lh, e on
 * rh (entry now, a hit output). lh's sample at 30 ms is fed, rh's at 30
 * ms not yet, and e is armed at 25 ms. The bimanual definition's entry
 * (t_v2_two_hands). 0.6.1 made G (e's group) at its place, "pending" until
 * L came. v0.7.0 classes a group by its own footprint: N(F(G)) is rh's
 * 20 ms, so G is ahead and waits for rh's next sample (docs/sig.md, "Late
 * input": "pending groups no longer arise"). rh9: rh's 20 ms value meets
 * e's test; calls: the window's log (0: the default) */
static void pg_open(int rh9, int calls) {
    ysig_node n[4];
    n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 1, 2); n[0].history = 64;
    n[1] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 2, 2); n[1].history = 64;
    n[2] = ysig_node_of(YSIG_OP_DETECT, 0, -1); n[2].p[YSIG_DETECT_LANE] = 0; n[2].p[YSIG_DETECT_LEVEL] = 5; n[2].p[YSIG_DETECT_START] = 1; n[2].history = 64;
    n[3] = ysig_node_of(YSIG_OP_DETECT, 1, -1); n[3].p[YSIG_DETECT_LANE] = 0; n[3].p[YSIG_DETECT_LEVEL] = 5; n[3].p[YSIG_DETECT_START] = 1;
    n[3].p[YSIG_DETECT_ENTRY] = YSIG_ENTRY_NOW; n[3].p[YSIG_DETECT_OUT] = YSIG_OUT_HIT; n[3].history = 64;
    if (!rp_open(n, 4, 4, calls, false, false)) return;
    rp_feed(10 * MS, 0, YIN_AXIS_POSITION, 1);
    rp_feed(10 * MS, 0, YIN_AXIS_POSITION, 2);
    rp_feed(20 * MS, 0, YIN_AXIS_POSITION, 1);
    rp_feed(20 * MS, rh9 ? 9 : 0, YIN_AXIS_POSITION, 2);
    rp_feed(30 * MS, 0, YIN_AXIS_POSITION, 1);
    ysig_arm(&g_rp, 2, 25 * MS);   /* the first hand's control: past */
    ysig_arm(&g_rp, 3, 25 * MS);   /* G: ahead by its footprint, waits */
}
/* e's hits since the last read: how many, and the last one's time */
static int64_t g_pg_t;
static int pg_e_hits(void) {
    ysig_hit h[8];
    int m, k, n = 0;
    while ((m = ysig_detected(&g_rp, h, 8)) > 0)
        for (k = 0; k < m; k++) if (h[k].node == 3) { n++; g_pg_t = h[k].t; }
    return n;
}
static int64_t pg_late_unv0(void);
static int64_t pg_late_unv(void) {
    ysig_stats st;
    ysig_get_stats(&g_rp, &st);
    return st.late_unverified;
}
/* The oracle's reason 6 shape (mutant 331 in 0.6.1), under T1's closure.
 * Sources s1 to s4 on devices 1 to 4; e on s3 (G), w on s2 (W); X =
 * all(w, e) gives G and W a node in common; Y = all(w, k), k on s1, makes
 * s1's input meet W but not G. Inputs to 20 ms on s1 to s3. W arms w at
 * 27 ms and G arms e at 25 ms: both ahead by their footprints (v0.7.0),
 * both wait. s1 at 31 ms lets W in, and with X, T1's closure lets G in
 * right before it (it meets W, smaller key): R's order, exact. The
 * control (no X): G still waits */
static void pg_reason6(int with_x) {
    ysig_node n[9];
    int k;
    for (k = 0; k < 4; k++) { n[k] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, k + 1, 2); n[k].history = 64; }
    n[4] = ysig_node_of(YSIG_OP_DETECT, 1, -1); n[4].p[YSIG_DETECT_LANE] = 0; n[4].p[YSIG_DETECT_LEVEL] = 5; n[4].p[YSIG_DETECT_START] = 1; n[4].history = 64;   /* w */
    n[5] = ysig_node_of(YSIG_OP_DETECT, 2, -1); n[5].p[YSIG_DETECT_LANE] = 0; n[5].p[YSIG_DETECT_LEVEL] = 5; n[5].p[YSIG_DETECT_START] = 1; n[5].history = 64;   /* e */
    n[6] = ysig_node_of(YSIG_OP_OFFSET, 0, -1);                                 /* k */
    n[7] = ysig_node_of(YSIG_OP_ALL, 4, 6); n[7].history = 64;                 /* Y */
    n[8] = ysig_node_of(YSIG_OP_ALL, 4, with_x ? 5 : 6); n[8].history = 64;    /* X (or a second Y) */
    if (!rp_open(n, 9, 4, 0, false, false)) return;
    for (k = 1; k <= 3; k++) { rp_feed(10 * MS, 0, YIN_AXIS_POSITION, k); rp_feed(20 * MS, 0, YIN_AXIS_POSITION, k); }
    ysig_arm(&g_rp, 4, 27 * MS);   /* W, ahead */
    rp_feed(30 * MS, 0, YIN_AXIS_POSITION, 4);
    ysig_arm(&g_rp, 5, 25 * MS);   /* G, ahead too */
    CHECK(ex_unv() == 0 && g_rp.wt->n == 2);
    rp_feed(31 * MS, 0, YIN_AXIS_POSITION, 1);
    CHECK(ex_unv() == 0 && g_rp.wt->n == (with_x ? 0 : 1));
    CHECK(((ysig__det_st*)g_rp.nd[5].st)->t_arm == (with_x ? 25 * MS : 0));
}

static void test_pending(void) {
    ysig_stats st;
    int k;
    g_where = "two hands (0.6.1's pending groups)";
    pg_reason6(1);
    pg_reason6(0);
    /* G waits; rh's 30 ms sample lets it in (L), exact; a crossing after
     * it hits */
    pg_open(0, 0);
    CHECK(ex_unv() == 0 && g_rp.wt->n == 1);
    rp_feed(30 * MS, 0, YIN_AXIS_POSITION, 2);
    CHECK(g_rp.wt->n == 0);
    rp_feed(40 * MS, 9, YIN_AXIS_POSITION, 2);
    CHECK(ex_unv() == 0 && pg_e_hits() == 1 && g_pg_t == 40 * MS);
    /* a frame lets it in too (every node); rh's 30 ms sample after it is a
     * late input that becomes G's L ("Exactness for late calls", 2):
     * placed before the frame, exact */
    pg_open(0, 0);
    rp_frame(33 * MS);
    CHECK(ex_unv() == 0 && g_rp.wt->n == 0);
    rp_feed(30 * MS, 0, YIN_AXIS_POSITION, 2);
    CHECK(ex_unv() == 0 && pg_late_unv() == 0);
    /* rh's 20 ms value meets the test: 0.6.1 read the entry-now hit at the
     * arm's call (counted). Now the arm waits; L makes it at 25 on the held
     * 9: the hit at 25, exact */
    pg_open(1, 0);
    CHECK(ex_unv() == 0 && pg_e_hits() == 0);
    rp_feed(30 * MS, 0, YIN_AXIS_POSITION, 2);
    CHECK(ex_unv() == 0 && pg_e_hits() == 1 && g_pg_t == 25 * MS);
    /* an input at or before its time comes before it in R (key: an input
     * first at one time) and does not let it in: L makes it on that value
     * (0.6.1 counted both) */
    for (k = 22; k <= 25; k += 3) {
        pg_open(0, 0);
        rp_feed(k * MS, 9, YIN_AXIS_POSITION, 2);
        CHECK(ex_unv() == 0 && g_rp.wt->n == 1);
        rp_feed(30 * MS, 0, YIN_AXIS_POSITION, 2);
        CHECK(ex_unv() == 0 && pg_e_hits() == 1 && g_pg_t == 25 * MS);
    }
    /* the state ends: a plain disarm, at N (30 ms, the ruling): it lets
     * the arm in, which it then ends, exact; no crossing hits after */
    pg_open(0, 0);
    ysig_disarm(&g_rp, 3);
    rp_feed(31 * MS, 0, YIN_AXIS_POSITION, 1);   /* ends the disarm's gap */
    rp_feed(40 * MS, 9, YIN_AXIS_POSITION, 2);
    CHECK(ex_unv() == 0 && pg_e_hits() == 0);
    /* ysig_disarm_at() at the exit time 30: the same */
    pg_open(0, 0);
    CHECK(ysig_disarm_at(&g_rp, 3, 30 * MS) && g_rp.wt->n == 0);
    rp_feed(40 * MS, 9, YIN_AXIS_POSITION, 2);
    CHECK(ex_unv() == 0 && pg_e_hits() == 0);
    /* another arm of e, at 30, 50 or 26 ms: ahead by its footprint too,
     * both wait (0.6.1 counted G) */
    for (k = 0; k < 3; k++) {
        static const int at[3] = { 30, 50, 26 };
        pg_open(0, 0);
        ysig_arm(&g_rp, 3, at[k] * MS);
        CHECK(ex_unv() == 0 && g_rp.wt->n == 2);
    }
    /* a barrier cancels the waiting arm (0.6.1 counted G) */
    pg_open(0, 0);
    ysig_flush(&g_rp);
    ysig_get_stats(&g_rp, &st);
    CHECK(ex_unv() == 0 && g_rp.wt->n == 0 && st.arm_ahead_cancelled == 1);
    pg_open(0, 0);
    ysig_reset_trial(&g_rp, 40 * MS);
    ysig_get_stats(&g_rp, &st);
    CHECK(ex_unv() == 0 && st.arm_ahead_cancelled == 1);
    /* the window's log drops the calls before L (a 32-call log, the first
     * hand alone for 40 calls): G waits outside the log, L lets it in */
    pg_open(0, 32);
    for (k = 0; k < 40; k++) rp_feed((31 + k) * MS, 0, YIN_AXIS_POSITION, 1);
    CHECK(ex_unv() == 0 && g_rp.wt->n == 1);
    rp_feed(80 * MS, 0, YIN_AXIS_POSITION, 2);
    CHECK(ex_unv() == 0 && g_rp.wt->n == 0);
}

static int64_t pg_late_unv0(void) { return pg_late_unv(); }

static void test_arm_exact(void) {
    ysig_node n[5];
    int v;
    g_where = "exactness";
    {
        /* condition 1: a waiting group let in between another's calls (0.6.x).
         * y (on x's hit output) waits at 30 ms with a set of X, x at 20 ms;
         * device 3's input lets y's group in (it meets X); 0.6.x let x in
         * after the set, inside y's block (1), and y's L meets only the set
         * (2 (a)) */
        ysig_node m[7];
        m[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 1, 2); m[0].history = 64;
        m[1] = ysig_node_of(YSIG_OP_DETECT, 0, -1); m[1].p[YSIG_DETECT_LANE] = 0; m[1].p[YSIG_DETECT_LEVEL] = 5; m[1].p[YSIG_DETECT_START] = 1;
        m[1].p[YSIG_DETECT_OUT] = YSIG_OUT_HIT; m[1].history = 64;
        m[2] = ysig_node_of(YSIG_OP_DETECT, 1, -1); m[2].p[YSIG_DETECT_LANE] = 0; m[2].p[YSIG_DETECT_LEVEL] = 0.5; m[2].p[YSIG_DETECT_START] = 1; m[2].history = 64;
        m[3] = src_node(YIN_KIND_PEN, YIN_AXIS_PEN_PRESSURE, 3, 1);
        m[4] = ysig_node_of(YSIG_OP_OFFSET, 3, -1);
        m[5] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 2, 2);   /* moves N only */
        if (!rp_open(m, 6, 4, 0, false, false)) return;
        rp_feed(10 * MS, 0, YIN_AXIS_POSITION, 1);
        rp_feed(10 * MS, 0, YIN_AXIS_PEN_PRESSURE, 3);
        ysig_arm(&g_rp, 1, 20 * MS);
        ysig_set(&g_rp, 4, YSIG_OFFSET_D_X, 3);
        ysig_arm(&g_rp, 2, 30 * MS);
        rp_feed(25 * MS, 0, YIN_AXIS_POSITION, 2);   /* N(x) 25: x fails 1 alone */
        rp_feed(35 * MS, 0, YIN_AXIS_PEN_PRESSURE, 3);
        /* v0.7.0: T1's closure lets x in right before y's group (it meets
         * y, smaller key), not inside its block: condition 1 holds for x,
         * and N(x) is 25 (device 2), past its 20: exact by 2. y's L meets
         * only the set (2 (a)): y alone is counted */
        CHECK(g_rp.wt->n == 0 && ex_unv() == 1);
    }
    /* 2 (a): L meets the group's set, not its arm (device 3 lets in d1's
     * group with a set of X); the control: device 1's input, which meets
     * the arm */
    for (v = 0; v < 2; v++) {
        if (!ex_open(n, 4)) return;
        ysig_set(&g_rp, 4, YSIG_OFFSET_D_X, 3);
        ysig_arm(&g_rp, 1, 20 * MS);
        if (v) rp_feed(25 * MS, 0, YIN_AXIS_PEN_PRESSURE, 3);
        else rp_feed(25 * MS, 0, YIN_AXIS_POSITION, 1);
        CHECK(g_rp.wt->n == 0 && ex_unv() == v);
    }
    /* 2 (a), a barrier between G and L: a flush lets in d1's waiting group
     * (its set), the arm cancelled; the control: an arm alone (nothing of
     * it goes in) */
    for (v = 0; v < 2; v++) {
        if (!ex_open(n, 4)) return;
        if (v) ysig_set(&g_rp, 4, YSIG_OFFSET_D_X, 3);
        ysig_arm(&g_rp, 1, 20 * MS);
        ysig_flush(&g_rp);
        CHECK(g_rp.wt->n == 0 && ex_unv() == v);
    }
    /* 2 (b): between G (d1 at 20 ms) and L (device 1 at 30), d2's group
     * (at 22 ms) has a set of d1 (it meets G); the control: d2's arm
     * alone (the exception) */
    for (v = 0; v < 2; v++) {
        if (!ex_open(n, 4)) return;
        ysig_arm(&g_rp, 1, 20 * MS);
        if (v) ysig_set(&g_rp, 1, YSIG_DETECT_LEVEL, 4);
        ysig_arm(&g_rp, 2, 22 * MS);
        rp_feed(30 * MS, 0, YIN_AXIS_POSITION, 1);
        CHECK(g_rp.wt->n == 0 && ex_unv() == v);
    }
    /* 2 (b) with a window, a later group placed between: d1 armed at 20
     * ms in the past (N(G) 10, L the 30 ms input); then d2's group with a
     * set of d1, at 22 ms, goes between them */
    for (v = 0; v < 2; v++) {
        if (!ex_open(n, 4)) return;
        rp_feed(30 * MS, 0, YIN_AXIS_POSITION, 1);
        ysig_arm(&g_rp, 1, 20 * MS);
        CHECK(ex_unv() == 0);
        if (v) ysig_set(&g_rp, 1, YSIG_DETECT_LEVEL, 4);
        ysig_arm(&g_rp, 2, 22 * MS);
        CHECK(ex_unv() == v);
    }
    /* 2 (c): a disarm cancelled d1's waiting arm, whose group has a disarm
     * of d2: input past 20 ms lets that disarm in (the reviewer's reason
     * 8 is the same at a flush); the control: the group has a set */
    for (v = 0; v < 2; v++) {
        if (!ex_open(n, 4)) return;
        if (v) ysig_disarm(&g_rp, 2);
        else ysig_set(&g_rp, 1, YSIG_DETECT_LEVEL, 4);
        ysig_arm(&g_rp, 1, 20 * MS);
        rp_feed(12 * MS, 0, YIN_AXIS_POSITION, 1);
        ysig_disarm(&g_rp, 1);
        rp_feed(25 * MS, 0, YIN_AXIS_POSITION, 1);
        CHECK(g_rp.wt->n == 0 && ex_unv() == 1);   /* cancelled: (c), or (a) with no disarm */
    }
    /* 2 (b)'s exception, a group of an arm and a disarm (the player's
     * exit at the time its control was armed for): G arms d2 (on d1) at
     * 20 ms, H disarms d1 and arms it at 20 ms, later; one input lets
     * both in. The disarm meets G: the exception holds. The control: H
     * with a set too (of X: only the disarm meets G, but H is no longer
     * an arm and disarms). Ahead (both wait, the input lets them in) and
     * past
     * (the input first) */
    for (v = 0; v < 4; v++) {
        if (!ex_open(n, 4)) return;
        if (v & 2) rp_feed(30 * MS, 0, YIN_AXIS_POSITION, 1);
        ysig_arm(&g_rp, 2, 20 * MS);
        if (v & 1) ysig_set(&g_rp, 4, YSIG_OFFSET_D_X, 3);   /* it meets only H's own */
        ysig_disarm(&g_rp, 1);
        ysig_arm(&g_rp, 1, 20 * MS);
        if (!(v & 2)) rp_feed(30 * MS, 0, YIN_AXIS_POSITION, 1);
        CHECK(g_rp.wt->n == 0 && ex_unv() == (v & 1));
    }
    /* 2 (b)'s exception needs H's own L to be G's (the reviewer's 489):
     * G arms d9 (on latch(s1, d6)) at 20 ms, H arms d6 at 20 ms, both in
     * the past; device 2's input at 25 meets H through all(d6, d7), not
     * G, so it lets H in before G's L (device 1 at 30). The control: no
     * device 2 input between */
    for (v = 0; v < 2; v++) {
        ysig_node an[7];
        an[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 1, 2); an[0].history = 64;
        an[1] = ysig_node_of(YSIG_OP_DETECT, 0, -1); an[1].p[YSIG_DETECT_LANE] = 0; an[1].p[YSIG_DETECT_LEVEL] = 5; an[1].p[YSIG_DETECT_START] = 1; an[1].history = 64;
        an[2] = ysig_node_of(YSIG_OP_LATCH, 0, 1); an[2].history = 64;
        an[3] = ysig_node_of(YSIG_OP_DETECT, 2, -1); an[3].p[YSIG_DETECT_LANE] = 0; an[3].p[YSIG_DETECT_LEVEL] = 5; an[3].p[YSIG_DETECT_START] = 1; an[3].history = 64;
        an[4] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 2, 2); an[4].history = 64;
        an[5] = ysig_node_of(YSIG_OP_DETECT, 4, -1); an[5].p[YSIG_DETECT_LANE] = 0; an[5].p[YSIG_DETECT_LEVEL] = 5; an[5].history = 64;
        an[6] = ysig_node_of(YSIG_OP_ALL, 1, 5); an[6].history = 64;
        if (!rp_open(an, 7, 4, 0, false, false)) return;
        rp_feed(10 * MS, 0, YIN_AXIS_POSITION, 1);
        rp_feed(10 * MS, 0, YIN_AXIS_POSITION, 2);
        if (!v) rp_feed(25 * MS, 0, YIN_AXIS_POSITION, 2);
        rp_feed(30 * MS, 0, YIN_AXIS_POSITION, 1);
        ysig_arm(&g_rp, 3, 20 * MS);
        CHECK(ex_unv() == 0);
        ysig_arm(&g_rp, 1, 20 * MS);
        CHECK(ex_unv() == !v);
    }
    /* 2 (b)'s exception needs no group K that waits for L too, earlier
     * than G, linked to H (case 1061 of the oracle's seed 777016): K arms
     * e (on device 2) at 20 ms, G arms d (on any device) at 20 ms, H
     * disarms e and d and arms d at 20 ms, all in the past; device 2's
     * input at 30 ms is the L of all three. In R, L lets K in first; K's
     * arm lets H in, whose disarm of d cancels G's waiting arm. The
     * control: K at 25 ms (later than G, so L lets G in first) */
    for (v = 0; v < 2; v++) {
        ysig_node kn[4];
        kn[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 0, 2); kn[0].history = 64;   /* any device */
        kn[1] = ysig_node_of(YSIG_OP_DETECT, 0, -1); kn[1].p[YSIG_DETECT_LANE] = 0; kn[1].p[YSIG_DETECT_LEVEL] = 5; kn[1].p[YSIG_DETECT_START] = 1; kn[1].history = 64;
        kn[2] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 2, 2); kn[2].history = 64;
        kn[3] = ysig_node_of(YSIG_OP_DETECT, 2, -1); kn[3].p[YSIG_DETECT_LANE] = 0; kn[3].p[YSIG_DETECT_LEVEL] = 5; kn[3].p[YSIG_DETECT_START] = 1; kn[3].history = 64;
        if (!rp_open(kn, 4, 4, 0, false, false)) return;
        rp_feed(10 * MS, 0, YIN_AXIS_POSITION, 2);
        rp_feed(30 * MS, 0, YIN_AXIS_POSITION, 2);
        ysig_arm(&g_rp, 3, (v ? 20 : 25) * MS);   /* K */
        ysig_arm(&g_rp, 1, 20 * MS);              /* G */
        CHECK(ex_unv() == 0);
        ysig_disarm(&g_rp, 3);                    /* H */
        ysig_disarm(&g_rp, 1);
        ysig_arm(&g_rp, 1, 20 * MS);
        CHECK(ex_unv() == v);
    }
    /* The same through a chain placed after H: K arms e, G arms d, H
     * disarms d and arms f, then J disarms e and arms f, all at 20 ms. In
     * R, K's arm lets J in, J's arm lets H in, H's disarm cancels G. J
     * meets K and H, not G: 2 (b) is tested again over every call between
     * G and L, so G counts at J's going-in (and H, which J's link to K
     * makes unverified too). d, e and f are on devices 1, 2 and 3; L is a
     * settle at 30 ms (it meets every node). The control: J without the
     * disarm of e (no link) */
    for (v = 0; v < 2; v++) {
        ysig_node kn[6];
        kn[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 1, 2); kn[0].history = 64;
        kn[1] = ysig_node_of(YSIG_OP_DETECT, 0, -1); kn[1].p[YSIG_DETECT_LANE] = 0; kn[1].p[YSIG_DETECT_LEVEL] = 5; kn[1].p[YSIG_DETECT_START] = 1; kn[1].history = 64;
        kn[2] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 2, 2); kn[2].history = 64;
        kn[3] = ysig_node_of(YSIG_OP_DETECT, 2, -1); kn[3].p[YSIG_DETECT_LANE] = 0; kn[3].p[YSIG_DETECT_LEVEL] = 5; kn[3].p[YSIG_DETECT_START] = 1; kn[3].history = 64;
        kn[4] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 3, 2); kn[4].history = 64;
        kn[5] = ysig_node_of(YSIG_OP_DETECT, 4, -1); kn[5].p[YSIG_DETECT_LANE] = 0; kn[5].p[YSIG_DETECT_LEVEL] = 5; kn[5].p[YSIG_DETECT_START] = 1; kn[5].history = 64;
        if (!rp_open(kn, 6, 4, 0, false, false)) return;
        rp_feed(10 * MS, 0, YIN_AXIS_POSITION, 1);
        ysig_settle(&g_rp, 30 * MS);
        ysig_arm(&g_rp, 3, 20 * MS);   /* K */
        ysig_arm(&g_rp, 1, 20 * MS);   /* G */
        ysig_disarm(&g_rp, 1);         /* H */
        ysig_arm(&g_rp, 5, 20 * MS);
        CHECK(ex_unv() == 0);
        if (v) ysig_disarm(&g_rp, 3);  /* J */
        ysig_arm(&g_rp, 5, 20 * MS);
        CHECK(ex_unv() == 2 * v);
    }
    /* 2 (c) at any N(G): a disarm of d1 cancels d1's waiting arm, whose
     * group has a disarm of d2; device 3 moves N to 30 ms, device 1 at 35
     * lets the group's disarm in with N(G) past its time (the reviewer's
     * 490 and 1821). The control: N(G) at its time (device 3 at 20) */
    for (v = 0; v < 2; v++) {
        if (!ex_open(n, 4)) return;
        ysig_disarm(&g_rp, 2);
        ysig_arm(&g_rp, 1, 20 * MS);
        rp_feed(12 * MS, 0, YIN_AXIS_POSITION, 1);
        ysig_disarm(&g_rp, 1);
        rp_feed((v ? 30 : 20) * MS, 0, YIN_AXIS_PEN_PRESSURE, 3);
        CHECK(g_rp.wt->n == 1 && ex_unv() == 0);
        rp_feed(35 * MS, 0, YIN_AXIS_POSITION, 1);
        CHECK(g_rp.wt->n == 0 && ex_unv() == v);
    }
    /* 4: a disarm in no group right before G. (a) d1 waits at 20 ms; a
     * disarm of d2 in no group; device 1 at 25 ends its gap and lets d1
     * in right after it (the reviewer's 883). The control: device 3's
     * input ends the gap first. (b) d1 armed at 20 ms in the past goes
     * right after a disarm of d2 in no group (the reviewer's 404). The
     * control: the disarm before device 1's 10 ms input */
    for (v = 0; v < 2; v++) {
        if (!ex_open(n, 4)) return;
        ysig_arm(&g_rp, 1, 20 * MS);
        ysig_disarm(&g_rp, 2);
        if (!v) rp_feed(12 * MS, 0, YIN_AXIS_PEN_PRESSURE, 3);
        rp_feed(25 * MS, 0, YIN_AXIS_POSITION, 1);
        CHECK(g_rp.wt->n == 0 && ex_unv() == v);
    }
    /* (c) a state call later in an open gap lets d1 in, after the gap's
     * earlier call: a disarm of d3 (it does not meet d1, so it lets
     * nothing in itself), or a set of X. v 0: a set, then an input ends
     * the gap: 0. v 1: a disarm, then an input: the disarm (in no group)
     * lies in the gap d1's arm closes, 4. v 2: a disarm, then an arm of
     * d3 at N closes the gap: 4 (a disarm of another group), and 1. v 3:
     * a set, then the arm: d1 is inside that group's block, 1 */
    for (v = 0; v < 4; v++) {
        ysig_node cn6[6];
        if (!ex_open(cn6, 4)) return;
        cn6[5] = ysig_node_of(YSIG_OP_DETECT, 3, -1); cn6[5].p[YSIG_DETECT_LEVEL] = 5; cn6[5].p[YSIG_DETECT_START] = 1; cn6[5].history = 64;
        if (!rp_open(cn6, 6, 4, 0, false, false)) return;
        rp_feed(10 * MS, 0, YIN_AXIS_POSITION, 1);
        rp_feed(10 * MS, 0, YIN_AXIS_PEN_PRESSURE, 3);
        ysig_arm(&g_rp, 1, 20 * MS);
        rp_feed(30 * MS, 0, YIN_AXIS_PEN_PRESSURE, 3);   /* N 30: d1 still waits */
        if (v == 1 || v == 2) ysig_disarm(&g_rp, 5);
        else ysig_set(&g_rp, 4, YSIG_OFFSET_D_X, 1);
        CHECK(g_rp.wt->n == 1);
        /* v0.7.0: the set is at N(F(c)), 10 ms (device 1 alone reaches d1):
         * earlier than d1's 20, it lets nothing in (0.6.x: at N, 30 ms, it
         * let d1 in, and the shapes above counted 1 or 4). d1 waits; device
         * 3's input does not meet it; nothing is counted */
        ysig_set(&g_rp, 1, YSIG_DETECT_LEVEL, 4);
        CHECK(g_rp.wt->n == 1);
        if (v >= 2) ysig_arm(&g_rp, 5, 30 * MS);
        rp_feed(31 * MS, 0, YIN_AXIS_PEN_PRESSURE, 3);
        /* S1' (2026-11-15): with the arm at 30 (v 2, 3) the group is
         * present and its set lets in at its N(F(c)), 10, as with no arm
         * (S1, now withdrawn, let d1 in at 30 inside the group's block) */
        CHECK(ex_unv() == 0 && g_rp.wt->n == 1);
    }
    /* (d) the reviewer's 1137: a present arm (of d2, at N) lets d1 in
     * right before itself, after its own group's disarm of d3: d1 is in
     * that group's block (1) and the disarm in the gap d1's arm closes
     * (4); v 2: a set of X instead of the disarm, 1 alone. The control
     * (v 0): the arm alone */
    for (v = 0; v < 3; v++) {
        ysig_node cn6[6];
        if (!ex_open(cn6, 4)) return;
        cn6[5] = ysig_node_of(YSIG_OP_DETECT, 3, -1); cn6[5].p[YSIG_DETECT_LEVEL] = 5; cn6[5].p[YSIG_DETECT_START] = 1; cn6[5].history = 64;
        if (!rp_open(cn6, 6, 4, 0, false, false)) return;
        rp_feed(10 * MS, 0, YIN_AXIS_POSITION, 1);
        rp_feed(10 * MS, 0, YIN_AXIS_PEN_PRESSURE, 3);
        ysig_arm(&g_rp, 1, 20 * MS);
        rp_feed(30 * MS, 0, YIN_AXIS_PEN_PRESSURE, 3);
        if (v == 1) ysig_disarm(&g_rp, 5);
        if (v == 2) ysig_set(&g_rp, 4, YSIG_OFFSET_D_X, 1);
        CHECK(g_rp.wt->n == 1);
        /* v0.7.0, classes by the group's footprint: the arm alone (v 0) is
         * ahead (only device 1 reaches d2, at 10 ms) and waits too, letting
         * nothing in. With the disarm of d3 or the set of X, device 3's 30
         * ms reaches the group: present, it lets d1 in as before */
        ysig_arm(&g_rp, 2, 30 * MS);
        CHECK(g_rp.wt->n == (v ? 0 : 2) && ex_unv() == (v > 0));
    }
    for (v = 0; v < 2; v++) {
        n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 0, 2); n[0].history = 64;
        n[1] = ysig_node_of(YSIG_OP_DETECT, 0, -1); n[1].p[YSIG_DETECT_LANE] = 0; n[1].p[YSIG_DETECT_LEVEL] = 5; n[1].p[YSIG_DETECT_START] = 1; n[1].history = 64;
        n[2] = ysig_node_of(YSIG_OP_DETECT, 1, -1); n[2].p[YSIG_DETECT_LANE] = 0; n[2].p[YSIG_DETECT_LEVEL] = 0.5; n[2].p[YSIG_DETECT_START] = 1; n[2].history = 64;
        n[3] = src_node(YIN_KIND_PEN, YIN_AXIS_PEN_PRESSURE, 3, 1);
        n[4] = ysig_node_of(YSIG_OP_OFFSET, 3, -1);
        if (!rp_open(n, 5, 4, 0, false, false)) return;
        if (!v) ysig_disarm(&g_rp, 2);
        rp_feed(10 * MS, 0, YIN_AXIS_POSITION, 1);
        if (v) ysig_disarm(&g_rp, 2);
        rp_feed(30 * MS, 0, YIN_AXIS_POSITION, 1);
        ysig_arm(&g_rp, 1, 20 * MS);
        CHECK(ex_unv() == v);
    }
    /* 3 is for a group that waits at its place only: a present arm, then
     * a late report of another device that meets it: 0 */
    if (!ex_open(n, 4)) return;
    rp_feed(20 * MS, 0, YIN_AXIS_POSITION, 1);
    ysig_arm(&g_rp, 1, 20 * MS);
    rp_feed(15 * MS, 0, YIN_AXIS_POSITION, 2);
    CHECK(ex_unv() == 0);
    /* and a past group with N(G) at or past its time (device 3's input at
     * 30 ms before its place, device 1's at 12 the call it follows) */
    if (!ex_open(n, 4)) return;
    rp_feed(30 * MS, 0, YIN_AXIS_PEN_PRESSURE, 3);
    rp_feed(12 * MS, 0, YIN_AXIS_POSITION, 1);
    rp_feed(31 * MS, 0, YIN_AXIS_PEN_PRESSURE, 3);   /* after the place: a run again */
    ysig_arm(&g_rp, 1, 20 * MS);
    rp_feed(15 * MS, 0, YIN_AXIS_POSITION, 2);
    CHECK(ex_unv() == 0);
    /* 3: a late report: d1 armed at 25 ms in the past (L: device 2's
     * input at 30, which s1 takes too), then device 1's sample at 20 ms
     * (newer than device 1's last); the controls: one at 25 ms (not
     * late), and one of device 3 at 20 (another footprint) */
    for (v = 0; v < 3; v++) {
        if (!ex_open(n, 4)) return;
        rp_feed(30 * MS, 0, YIN_AXIS_POSITION, 2);
        ysig_arm(&g_rp, 1, 25 * MS);
        CHECK(ex_unv() == 0);
        if (v == 0) rp_feed(20 * MS, 0, YIN_AXIS_POSITION, 1);
        else if (v == 1) rp_feed(25 * MS, 0, YIN_AXIS_POSITION, 1);
        else rp_feed(20 * MS, 0, YIN_AXIS_PEN_PRESSURE, 3);
        /* v0.7.0: device 1's 20 ms sample is a late input, placed before
         * the group (key 20 < 25), not a late report: nothing counted */
        CHECK(ex_unv() == 0 && pg_late_unv0() == 0);
    }
}

static void test_arm_repro(void) {
    ysig_node n[4];
    ysig_stats s0, s1;
    ysig_hit h[64];
    yin_event ev[64];
    int K, m, k, got, f, barrier;
    g_where = "oracle reproducers";
    /* H1: inputs older than the window's log that pin a group: the frames
     * a detect on a channel node reads */
    n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 0, 2);
    n[1] = ysig_node_of(YSIG_OP_CHANNEL, -1, -1);
    n[2] = ysig_node_of(YSIG_OP_DETECT, 1, -1); n[2].p[YSIG_DETECT_LEVEL] = 0.5; n[2].p[YSIG_DETECT_START] = 1; n[2].p[YSIG_DETECT_AT] = 1;
    if (!rp_open(n, 3, 1, 0, false, false)) return;
    rp_feed(1 * MS, 0, YIN_AXIS_POSITION, 1); rp_frame(16 * MS); rp_frame(33 * MS); rp_frame(50 * MS);
    ysig_arm(&g_rp, 2, 5 * MS);
    rp_feed(52 * MS, 0, YIN_AXIS_POSITION, 1);
    ysig_get_stats(&g_rp, &s0);
    ysig_arm(&g_rp, 2, 5 * MS);
    ysig_get_stats(&g_rp, &s1);
    /* v0.7.0: never pinned; a call before the window that must follow the
     * group (the frames, by the oldest image's keys) puts its place before
     * the window: arm_past_far */
    CHECK(s1.arm_past_pinned == s0.arm_past_pinned && s1.arm_past_far - s0.arm_past_far == 1);
    /* H2: a disarm in a waiting group cancels when that group goes in */
    n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 0, 2);
    n[1] = ysig_node_of(YSIG_OP_DETECT, 0, -1); n[1].p[YSIG_DETECT_LANE] = 0; n[1].p[YSIG_DETECT_LEVEL] = 5; n[1].p[YSIG_DETECT_START] = 1;
    n[2] = n[1];
    if (!rp_open(n, 3, 4, 0, false, false)) return;
    rp_feed(1 * MS, 0, YIN_AXIS_POSITION, 1);
    ysig_arm(&g_rp, 1, 50 * MS);
    rp_feed(2 * MS, 0, YIN_AXIS_POSITION, 1);
    ysig_disarm(&g_rp, 1);
    ysig_arm(&g_rp, 2, 20 * MS);
    rp_feed(30 * MS, 0, YIN_AXIS_POSITION, 1);
    ysig_get_stats(&g_rp, &s1);
    CHECK(s1.arm_ahead_cancelled == 1);
    rp_feed(60 * MS, 10, YIN_AXIS_POSITION, 1);
    m = ysig_detected(&g_rp, h, 64);
    for (k = 0; k < m; k++) CHECK(h[k].node != 1);
    /* H4: a feed every source drops is no input call: set, the dropped
     * feed and the arm are one group, the hit at 20 ms */
    n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 0, 2);
    n[1] = ysig_node_of(YSIG_OP_DETECT, 0, -1); n[1].p[YSIG_DETECT_LANE] = 0; n[1].p[YSIG_DETECT_LEVEL] = 5; n[1].p[YSIG_DETECT_START] = 1;
    if (!rp_open(n, 2, 4, 0, false, false)) return;
    rp_feed(10 * MS, 3, YIN_AXIS_POSITION, 1); rp_frame(16 * MS); rp_feed(20 * MS, 4.5, YIN_AXIS_POSITION, 1);
    ysig_set(&g_rp, 1, YSIG_DETECT_LEVEL, 4);
    rp_feed(15 * MS, 0, YIN_AXIS_POSITION, 1);   /* older than device 1's last: dropped */
    ysig_arm(&g_rp, 1, 12 * MS);
    CHECK(ysig_detected(&g_rp, h, 64) == 1 && h[0].t == 20 * MS);
    /* H5 (S1): a disarm in a past group meets the hold up to the group's
     * time, also with no window; H6 (S2): a present arm lets in an earlier
     * waiting arm it meets; S1: a disarm in an ahead group waits with it */
    for (K = 0; K <= 4; K += 4) {
        n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 1, 2);
        n[1] = ysig_node_of(YSIG_OP_DETECT, 0, -1); n[1].p[YSIG_DETECT_LANE] = 0; n[1].p[YSIG_DETECT_LEVEL] = 5;
        n[1].p[YSIG_DETECT_HOLD] = 0.004; n[1].p[YSIG_DETECT_START] = 1;
        n[2] = src_node(YIN_KIND_PEN, YIN_AXIS_PEN_PRESSURE, 3, 1);
        n[3] = ysig_node_of(YSIG_OP_DETECT, 2, -1); n[3].p[YSIG_DETECT_LANE] = 0; n[3].p[YSIG_DETECT_LEVEL] = 5; n[3].p[YSIG_DETECT_START] = 1;
        if (!rp_open(n, 4, K, 0, false, false)) return;
        rp_feed(1 * MS, 10, YIN_AXIS_POSITION, 1);
        ysig_arm(&g_rp, 1, 1 * MS);
        rp_feed(10 * MS, 0, YIN_AXIS_PEN_PRESSURE, 3);
        ysig_disarm(&g_rp, 1);
        ysig_arm(&g_rp, 3, 3 * MS);
        CHECK(ysig_detected(&g_rp, h, 64) == 0);
        n[1].p[YSIG_DETECT_HOLD] = 0.008;
        if (!rp_open(n, 3, K, 0, false, false)) return;
        rp_feed(10 * MS, 10, YIN_AXIS_POSITION, 1);
        ysig_arm(&g_rp, 1, 15 * MS);
        rp_feed(20 * MS, 1, YIN_AXIS_PEN_PRESSURE, 3);
        ysig_arm(&g_rp, 1, 20 * MS);
        rp_feed(25 * MS, 10, YIN_AXIS_POSITION, 1);
        rp_feed(30 * MS, 10, YIN_AXIS_POSITION, 1);
        CHECK(ysig_detected(&g_rp, h, 64) == 1 && h[0].t == 28 * MS);
        n[1].p[YSIG_DETECT_HOLD] = 0.01;
        if (!rp_open(n, 2, K, 0, false, false)) return;
        rp_feed(1 * MS, 10, YIN_AXIS_POSITION, 1);
        ysig_arm(&g_rp, 1, 1 * MS);
        rp_feed(2 * MS, 10, YIN_AXIS_POSITION, 1);
        ysig_disarm(&g_rp, 1);
        ysig_arm(&g_rp, 1, 15 * MS);
        CHECK(ysig_detected(&g_rp, h, 64) == 0);
        rp_feed(20 * MS, 10, YIN_AXIS_POSITION, 1);
        CHECK(ysig_detected(&g_rp, h, 64) == 1 && h[0].t == 11 * MS);
    }
    /* H9: a state call that lets a waiting arm in, in a gap that turns out
     * to be an ahead group: the group waits whole, the trigger too, until
     * it goes in (a set of an unrelated offset, or of a channel node's
     * index) */
    for (K = 0; K < 2; K++) {
        ysig_node mm[6];
        mm[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 1, 2);
        mm[1] = ysig_node_of(YSIG_OP_DETECT, 0, -1); mm[1].p[YSIG_DETECT_LANE] = 0; mm[1].p[YSIG_DETECT_LEVEL] = 5; mm[1].p[YSIG_DETECT_START] = 1;
        mm[2] = src_node(YIN_KIND_PEN, YIN_AXIS_PEN_PRESSURE, 3, 1);
        mm[3] = K ? ysig_node_of(YSIG_OP_CHANNEL, -1, -1) : ysig_node_of(YSIG_OP_OFFSET, 2, -1);
        if (K) mm[3].p[0] = 0;
        mm[4] = ysig_node_of(YSIG_OP_DETECT, 0, -1); mm[4].p[YSIG_DETECT_LANE] = 0; mm[4].p[YSIG_DETECT_LEVEL] = 5; mm[4].p[YSIG_DETECT_START] = 1;
        if (!rp_open(mm, 5, 4, 0, false, false)) return;
        rp_feed(10 * MS, 0, YIN_AXIS_POSITION, 1);
        rp_feed(10 * MS, 1, YIN_AXIS_PEN_PRESSURE, 3);
        CHECK(ysig_arm(&g_rp, 1, 15 * MS) && g_rp.wt->n == 1);
        rp_feed(20 * MS, 1, YIN_AXIS_PEN_PRESSURE, 3);
        if (K) CHECK(ysig_set(&g_rp, 3, YSIG_CHANNEL_CHANNEL, 0));
        else CHECK(ysig_set(&g_rp, 3, YSIG_OFFSET_D_X, 3));
        CHECK(ysig_set(&g_rp, 1, YSIG_DETECT_LEVEL, 4));
        CHECK(ysig_arm(&g_rp, 4, 30 * MS));
        CHECK(g_rp.wt->n == 2 && g_rp.nd[1].p[YSIG_DETECT_LEVEL] == 5);
        if (!K) CHECK(ysig_value_of(&g_rp, 3)->v[0] == 1);
        rp_feed(40 * MS, 0, YIN_AXIS_POSITION, 1);   /* past both: in by time */
        CHECK(g_rp.wt->n == 0 && g_rp.nd[1].p[YSIG_DETECT_LEVEL] == 4);
        CHECK(((ysig__det_st*)g_rp.nd[1].st)->t_arm == 15 * MS && ((ysig__det_st*)g_rp.nd[4].st)->t_arm == 30 * MS);
        ysig_get_stats(&g_rp, &s1);
        CHECK(s1.arm_past_full == 0 && s1.arm_past_off == 0);
    }
    /* H9 with a recenter: a relative mouse role; a detect on the role's
     * x waits ahead (15 ms); input of another source moves N to 20 ms; a
     * recenter (it meets the detect: the role reads the integrator) in a
     * gap that an ahead arm (30 ms) closes. The recenter waits with its
     * group: the integrator does not move, the detect's arm stays waiting,
     * until input past both lets them in, in order */
    {
        ysig_node en[4], mo[2];
        ysig_role role;
        ysig_adapter ad;
        ysig_desc d2;
        memset(&role, 0, sizeof role);
        role.name = "hand"; role.type = YSIG_TYPE_POSITION2D; role.units = YSIG_UNITS_DISPLAY_PX; role.allow_relative = true;
        mo[0] = src_node(YIN_KIND_MOUSE, YIN_AXIS_DELTA, 0, 2);
        mo[1] = ysig_node_of(YSIG_OP_INTEGRATE, 0, -1);
        mo[1].p[0] = mo[1].p[1] = 1;
        memset(&ad, 0, sizeof ad);
        ad.role = "hand"; ad.name = "mouse"; ad.type = YSIG_TYPE_POSITION2D_RELATIVE; ad.units = YSIG_UNITS_DISPLAY_PX;
        ad.nodes = mo; ad.n_nodes = 2;
        en[0] = ysig_node_of(YSIG_OP_ROLE, -1, -1); en[0].p[0] = 0;
        en[1] = ysig_node_of(YSIG_OP_DETECT, 0, -1); en[1].p[YSIG_DETECT_LANE] = 0; en[1].p[YSIG_DETECT_LEVEL] = 50; en[1].p[YSIG_DETECT_START] = 1;
        en[2] = src_node(YIN_KIND_PEN, YIN_AXIS_PEN_PRESSURE, 3, 1);
        en[3] = ysig_node_of(YSIG_OP_DETECT, 0, -1); en[3].p[YSIG_DETECT_LANE] = 0; en[3].p[YSIG_DETECT_LEVEL] = 50; en[3].p[YSIG_DETECT_START] = 1;
        memset(&d2, 0, sizeof d2);
        d2.nodes = en; d2.n_nodes = 4; d2.roles = &role; d2.n_roles = 1; d2.adapters = &ad; d2.n_adapters = 1;
        d2.channels = g_ch; d2.n_channels = 64; d2.catchup_frames = 4;
        d2.arena = g_rp_ar; d2.arena_size = ysig_arena_size(&d2);
        if (!d2.arena_size || d2.arena_size > sizeof g_rp_ar || !ysig_open(&g_rp, &d2)) { g_failures++; return; }
        ysig_reset(&g_rp, 0);
        {
            yin_event e2 = delta(10 * MS, 1, 5, 0);
            ysig_feed(&g_rp, &e2);
        }
        CHECK(ysig_value_of(&g_rp, 0)->v[0] == 5);
        CHECK(ysig_arm(&g_rp, 1, 15 * MS) && g_rp.wt->n == 1);
        rp_feed(20 * MS, 1, YIN_AXIS_PEN_PRESSURE, 3);
        CHECK(ysig_recenter(&g_rp, 0, 100, 0, 20 * MS));
        CHECK(ysig_arm(&g_rp, 3, 30 * MS));
        CHECK(g_rp.wt->n == 2 && ysig_value_of(&g_rp, 0)->v[0] == 5);   /* the recenter waits, the integrator stays */
        {
            yin_event e2 = delta(40 * MS, 1, 1, 0);
            ysig_feed(&g_rp, &e2);
        }
        CHECK(g_rp.wt->n == 0 && ysig_value_of(&g_rp, 0)->v[0] == 101);
        CHECK(((ysig__det_st*)g_rp.nd[1].st)->t_arm == 15 * MS && ((ysig__det_st*)g_rp.nd[3].st)->t_arm == 30 * MS);
        ysig_get_stats(&g_rp, &s1);
        CHECK(s1.arm_past_full == 0 && s1.arm_past_off == 0);
    }
    /* S3 (0.6.x): an arm call lets in a waiting group it meets at or
     * before its time: a past arm at 15 ms of the same detect (input of
     * another source moved N to 20) let in the arm waiting at 15 ms */
    n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 1, 2);
    n[1] = ysig_node_of(YSIG_OP_DETECT, 0, -1); n[1].p[YSIG_DETECT_LANE] = 0; n[1].p[YSIG_DETECT_LEVEL] = 5; n[1].p[YSIG_DETECT_START] = 1;
    n[2] = src_node(YIN_KIND_PEN, YIN_AXIS_PEN_PRESSURE, 3, 1);
    n[3] = n[1];
    for (K = 0; K <= 4; K += 4) {
        if (!rp_open(n, 4, K, 0, false, false)) return;
        rp_feed(10 * MS, 0, YIN_AXIS_POSITION, 1);
        CHECK(ysig_arm(&g_rp, 1, 15 * MS) && g_rp.wt->n == 1);
        rp_feed(20 * MS, 1, YIN_AXIS_PEN_PRESSURE, 3);
        /* v0.7.0: by its footprint the second arm is ahead too (device 3
         * does not reach the detect): both wait; device 1's next sample
         * lets both in, by key (call order at one time) */
        CHECK(ysig_arm(&g_rp, 1, 15 * MS) && g_rp.wt->n == 2);
        rp_feed(21 * MS, 0, YIN_AXIS_POSITION, 1);
        CHECK(g_rp.wt->n == 0);
        CHECK(((ysig__det_st*)g_rp.nd[1].st)->armed && ((ysig__det_st*)g_rp.nd[1].st)->t_arm == 15 * MS);
    }
    /* H7: final-only rows with small logs and a fallback: every frame row
     * once, in order, by the flush */
    n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 0, 2);
    n[1] = ysig_node_of(YSIG_OP_RECORD, 0, -1); n[1].p[0] = 1;
    n[2] = ysig_node_of(YSIG_OP_DETECT, 0, -1); n[2].p[YSIG_DETECT_LANE] = 0; n[2].p[YSIG_DETECT_LEVEL] = 5; n[2].p[YSIG_DETECT_START] = 1;
    if (!rp_open(n, 3, 3, 32, true, false)) return;
    for (f = 1; f <= 20; f++) {
        int ns = f % 5 == 0 ? 40 : 3;
        for (k = 0; k < ns; k++) rp_feed((f - 1) * 16 * MS + (k + 1) * 300000, 0, YIN_AXIS_POSITION, 1);
        if (f % 3 == 0) ysig_arm(&g_rp, 2, (f - 1) * 16 * MS);
        rp_frame(f * 16 * MS);
    }
    ysig_flush(&g_rp);
    ysig_get_stats(&g_rp, &s1);
    CHECK(g_rp_rows == 20 && g_rp_ooo == 0 && s1.arm_past_full >= 1);
    /* H8: a barrier makes 240 derived events final at once: none lost to
     * the 64-event queue, a caller that drains until 0 gets them all */
    for (barrier = 0; barrier < 3; barrier++) {
        n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 0, 2);
        n[1] = ysig_node_of(YSIG_OP_EMIT, 0, -1);
        n[2] = ysig_node_of(YSIG_OP_RECORD, 0, -1);
        if (!rp_open(n, 3, 8, 0, true, true)) return;
        got = 0;
        for (f = 1; f <= 6; f++) {
            for (k = 0; k < 40; k++) {
                rp_feed((f - 1) * 16 * MS + (k + 1) * 400000, 0, YIN_AXIS_POSITION, 1);
                while ((m = ysig_derived(&g_rp, ev, 64)) > 0) got += m;
            }
            rp_frame(f * 16 * MS);
            while ((m = ysig_derived(&g_rp, ev, 64)) > 0) got += m;
        }
        if (barrier == 0) ysig_flush(&g_rp);
        else if (barrier == 1) ysig_reset(&g_rp, 100 * MS);
        else ysig_reset_trial(&g_rp, 100 * MS);
        while ((m = ysig_derived(&g_rp, ev, 64)) > 0) got += m;
        ysig_get_stats(&g_rp, &s1);
        CHECK(got == 240 && g_rp_rows == 240 && s1.derived_lost == 0);
    }
    /* H12: an ahead arm lets in nothing at its call. A set with an arm at
     * 20 ms waits; input of another stream moves N to 30; an arm at 40 ms
     * of the same detect (ahead) leaves the set waiting, with a window;
     * with none the set stays where made. Input past 40 lets both in */
    for (K = 0; K <= 4; K += 4) {
        n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_PEN_PRESSURE, 3, 1);
        n[1] = ysig_node_of(YSIG_OP_OFFSET, 0, -1);
        n[2] = ysig_node_of(YSIG_OP_DETECT, 1, -1); n[2].p[YSIG_DETECT_LEVEL] = 5; n[2].p[YSIG_DETECT_START] = 1;
        n[3] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 1, 2);
        if (!rp_open(n, 4, K, 0, false, false)) return;
        rp_feed(10 * MS, 1, YIN_AXIS_PEN_PRESSURE, 3);
        ysig_set(&g_rp, 1, YSIG_OFFSET_D_X, 3);
        ysig_arm(&g_rp, 2, 20 * MS);
        rp_feed(30 * MS, 0, YIN_AXIS_POSITION, 1);
        ysig_arm(&g_rp, 2, 40 * MS);
        CHECK(ysig_value_of(&g_rp, 1)->v[0] == (K ? 1 : 4));
        CHECK(g_rp.wt->n == 2);
        rp_feed(45 * MS, 2, YIN_AXIS_PEN_PRESSURE, 3);
        CHECK(ysig_value_of(&g_rp, 1)->v[0] == 5 && g_rp.wt->n == 0);
        CHECK(((ysig__det_st*)g_rp.nd[2].st)->t_arm == 40 * MS);
    }
    /* H12, the other half: the ahead arm lets in when its group goes in.
     * x (stream 1) waits at 20 ms; y, a detect on x's output, waits at 30
     * ms with a set of an offset on stream 2. Input of stream 2 past 30
     * lets y's group in (it does not meet x); y's arm then lets x in, at
     * its own time, before itself (with a window: with none the set
     * stays where made, and the group is the arm alone) */
    {
        ysig_node m6[5];
        m6[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 1, 2);
        m6[1] = ysig_node_of(YSIG_OP_DETECT, 0, -1); m6[1].p[YSIG_DETECT_LANE] = 0; m6[1].p[YSIG_DETECT_LEVEL] = 5; m6[1].p[YSIG_DETECT_START] = 1;
        m6[1].p[YSIG_DETECT_OUT] = YSIG_OUT_HIT;
        m6[2] = ysig_node_of(YSIG_OP_DETECT, 1, -1); m6[2].p[YSIG_DETECT_LANE] = 0; m6[2].p[YSIG_DETECT_LEVEL] = 0.5; m6[2].p[YSIG_DETECT_START] = 1;
        m6[3] = src_node(YIN_KIND_PEN, YIN_AXIS_PEN_PRESSURE, 3, 1);
        m6[4] = ysig_node_of(YSIG_OP_OFFSET, 3, -1);
        if (!rp_open(m6, 5, 4, 0, false, false)) return;
        rp_feed(10 * MS, 0, YIN_AXIS_POSITION, 1);
        rp_feed(10 * MS, 0, YIN_AXIS_PEN_PRESSURE, 3);
        ysig_arm(&g_rp, 1, 20 * MS);
        ysig_set(&g_rp, 4, YSIG_OFFSET_D_X, 3);
        ysig_arm(&g_rp, 2, 30 * MS);
        rp_feed(25 * MS, 0, YIN_AXIS_PEN_PRESSURE, 3);
        CHECK(g_rp.wt->n == 2);
        rp_feed(35 * MS, 0, YIN_AXIS_PEN_PRESSURE, 3);
        CHECK(g_rp.wt->n == 0 && ysig_value_of(&g_rp, 4)->v[0] == 3);
        CHECK(((ysig__det_st*)g_rp.nd[1].st)->armed && ((ysig__det_st*)g_rp.nd[1].st)->t_arm == 20 * MS);
        CHECK(((ysig__det_st*)g_rp.nd[2].st)->armed && ((ysig__det_st*)g_rp.nd[2].st)->t_arm == 30 * MS);
    }
    /* H13: a past group's set lets a waiting arm in at the group's place,
     * not at the set's call. Detect a (on an offset of stream 1) waits at
     * 20 ms; input of stream 2 moves N to 30; a set of the offset (it
     * meets a) and an arm of b (on stream 2) at 25 ms make a past group.
     * Its place is before the 30 ms input: not pinned, a armed at 20 */
    n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_PEN_PRESSURE, 3, 1);
    n[1] = ysig_node_of(YSIG_OP_OFFSET, 0, -1);
    n[2] = ysig_node_of(YSIG_OP_DETECT, 1, -1); n[2].p[YSIG_DETECT_LEVEL] = 5; n[2].p[YSIG_DETECT_START] = 1;
    n[3] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 1, 2);
    {
        ysig_node m5[5];
        memcpy(m5, n, sizeof n);
        m5[4] = ysig_node_of(YSIG_OP_DETECT, 3, -1); m5[4].p[YSIG_DETECT_LANE] = 0; m5[4].p[YSIG_DETECT_LEVEL] = 5; m5[4].p[YSIG_DETECT_START] = 1;
        if (!rp_open(m5, 5, 4, 0, false, false)) return;
        rp_feed(10 * MS, 1, YIN_AXIS_PEN_PRESSURE, 3);
        ysig_arm(&g_rp, 2, 20 * MS);
        rp_feed(30 * MS, 0, YIN_AXIS_POSITION, 1);
        CHECK(g_rp.wt->n == 1);
        ysig_set(&g_rp, 1, YSIG_OFFSET_D_X, 3);
        ysig_get_stats(&g_rp, &s0);
        ysig_arm(&g_rp, 4, 25 * MS);
        ysig_get_stats(&g_rp, &s1);
        CHECK(s1.arm_past_pinned == s0.arm_past_pinned && s1.arm_caught_up == s0.arm_caught_up + 1);
        CHECK(g_rp.wt->n == 0 && ysig_value_of(&g_rp, 1)->v[0] == 4);
        CHECK(((ysig__det_st*)g_rp.nd[2].st)->armed && ((ysig__det_st*)g_rp.nd[2].st)->t_arm == 20 * MS);
        /* an arm at 15 ms instead: the set at 15 lets in nothing (20 is
         * later), and the waiting arm stays waiting */
        if (!rp_open(m5, 5, 4, 0, false, false)) return;
        rp_feed(10 * MS, 1, YIN_AXIS_PEN_PRESSURE, 3);
        ysig_arm(&g_rp, 2, 20 * MS);
        rp_feed(30 * MS, 0, YIN_AXIS_POSITION, 1);
        ysig_set(&g_rp, 1, YSIG_OFFSET_D_X, 3);
        /* v0.7.0: the set is at N(F(c)), 10 ms (the stream it moves):
         * earlier than a's 20, it lets nothing in (0.6.x: at N, 30 ms, it
         * let a in for now) */
        CHECK(g_rp.wt->n == 1);
        ysig_arm(&g_rp, 4, 15 * MS);
        CHECK(g_rp.wt->n == 1 && !((ysig__det_st*)g_rp.nd[2].st)->armed && ysig_value_of(&g_rp, 1)->v[0] == 4);
        rp_feed(35 * MS, 2, YIN_AXIS_PEN_PRESSURE, 3);
        CHECK(g_rp.wt->n == 0 && ((ysig__det_st*)g_rp.nd[2].st)->t_arm == 20 * MS);
    }
    /* H11: final-only derived events. (a) Events queued and not drained
     * when a run again starts stay (no drains until the flush; an emit per
     * sample: every one comes). (b) The final events a run again hands out
     * as its held buffer runs full are not dropped: an emit on a detect's
     * hit output, a past arm each frame (each adds a fall and a rise); a
     * held buffer of 48 gives what one of 256 gives */
    n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 0, 2);
    n[1] = ysig_node_of(YSIG_OP_EMIT, 0, -1);
    n[2] = ysig_node_of(YSIG_OP_DETECT, 0, -1); n[2].p[YSIG_DETECT_LANE] = 0; n[2].p[YSIG_DETECT_LEVEL] = 5; n[2].p[YSIG_DETECT_START] = 1;
    if (!rp_open(n, 3, 2, 64, false, true)) return;
    got = 0;
    for (f = 1; f <= 6; f++) {
        for (k = 0; k < 6; k++) rp_feed((f - 1) * 16 * MS + (k + 1) * MS, 0, YIN_AXIS_POSITION, 1);
        rp_frame(f * 16 * MS);
        /* past: a run again. v0.7.0: inside the window (after the last
         * frame but one, 3.5 ms into it); one at the frame before it is
         * before the oldest image now (the samples after it must follow
         * it: arm_past_far, not a pinned group placed in the window) */
        if (f >= 3) ysig_arm(&g_rp, 2, (f - 1) * 16 * MS + 3500000);
    }
    ysig_flush(&g_rp);
    while ((m = ysig_derived(&g_rp, ev, 64)) > 0) got += m;
    ysig_get_stats(&g_rp, &s1);
    CHECK(got == 36 && s1.derived_lost == 0 && s1.arm_caught_up >= 1);
    {
        int want = -1;
        for (barrier = 0; barrier < 2; barrier++) {
            n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 0, 2);
            n[1] = ysig_node_of(YSIG_OP_EMIT, 0, -1);
            n[2] = ysig_node_of(YSIG_OP_DETECT, 0, -1); n[2].p[YSIG_DETECT_LANE] = 0; n[2].p[YSIG_DETECT_LEVEL] = 5; n[2].p[YSIG_DETECT_START] = 1;
            n[2].p[YSIG_DETECT_REPEAT] = 1; n[2].p[YSIG_DETECT_OUT] = YSIG_OUT_HIT;
            n[3] = ysig_node_of(YSIG_OP_EMIT, 2, -1);
            g_rp_crows = barrier ? 48 : 0;
            k = rp_open(n, 4, 8, 64, false, true);
            g_rp_crows = 0;
            if (!k) return;
            got = 0;
            for (f = 1; f <= 6; f++) {
                for (k = 0; k < 4; k++) {
                    rp_feed((f - 1) * 16 * MS + (k + 1) * 3200000, (k & 1) ? 10 : 0, YIN_AXIS_POSITION, 1);
                    while ((m = ysig_derived(&g_rp, ev, 64)) > 0) got += m;
                }
                rp_frame(f * 16 * MS);
                if (f > 1) ysig_arm(&g_rp, 2, (f - 1) * 16 * MS);
                while ((m = ysig_derived(&g_rp, ev, 64)) > 0) got += m;
            }
            ysig_flush(&g_rp);
            while ((m = ysig_derived(&g_rp, ev, 64)) > 0) got += m;
            ysig_get_stats(&g_rp, &s1);
            CHECK(s1.derived_lost == 0 && s1.arm_caught_up == 5 && s1.arm_past_full == 0);
            if (barrier) CHECK(got == want);
            want = got;
        }
    }
    /* O3: a value earlier than the arm, taken after it, that fails the
     * test makes a detect ready again, as before the arm (no hit, no hold
     * start; a fresh entry's failure). A fresh detect (le 5) armed at 25
     * ms on a held 3 waits for a failure; a late 6 at 20 ms is one; 4 at
     * 35 ms is then a hit */
    for (K = 0; K <= 4; K += 4) {
        n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_PEN_PRESSURE, 3, 1);
        n[1] = ysig_node_of(YSIG_OP_DETECT, 0, -1); n[1].p[YSIG_DETECT_TEST] = 1; n[1].p[YSIG_DETECT_LEVEL] = 5;
        n[1].p[YSIG_DETECT_START] = 1; n[1].p[YSIG_DETECT_ENTRY] = YSIG_ENTRY_FRESH;
        n[2] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 1, 2);
        if (!rp_open(n, 3, K, 0, false, false)) return;
        rp_feed(10 * MS, 3, YIN_AXIS_PEN_PRESSURE, 3);
        rp_feed(30 * MS, 0, YIN_AXIS_POSITION, 1);
        ysig_arm(&g_rp, 1, 25 * MS);
        rp_feed(20 * MS, 6, YIN_AXIS_PEN_PRESSURE, 3);
        CHECK(ysig_detected(&g_rp, h, 64) == 0);
        rp_feed(35 * MS, 4, YIN_AXIS_PEN_PRESSURE, 3);
        CHECK(ysig_detected(&g_rp, h, 64) == 1 && h[0].node == 1 && h[0].t == 35 * MS);
    }
    /* H16: an arm that goes into R lets in a waiting group with its own
     * time and a later arm call after itself (rule 2: the earlier call
     * first). D1 (entry now) on S, latch L(S, D1), D2 (entry now) on L,
     * both armed at 20 ms: D2's hit carries L after D1's hit (value.t 20
     * ms), with no window (both wait, the 30 ms input lets them in) and
     * with one (both past) */
    for (K = 0; K <= 4; K += 4) {
        int k2, found = 0;
        n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 1, 2); n[0].history = 256;
        n[1] = ysig_node_of(YSIG_OP_DETECT, 0, -1);
        n[2] = ysig_node_of(YSIG_OP_LATCH, 0, 1);
        n[3] = ysig_node_of(YSIG_OP_DETECT, 2, -1);
        for (k2 = 1; k2 <= 3; k2 += 2) {
            n[k2].p[YSIG_DETECT_TEST] = YSIG_TEST_IN_RECT;
            n[k2].p[YSIG_DETECT_A_X] = n[k2].p[YSIG_DETECT_A_Y] = -3;
            n[k2].p[YSIG_DETECT_B_X] = n[k2].p[YSIG_DETECT_B_Y] = 3;
            n[k2].p[YSIG_DETECT_ENTRY] = YSIG_ENTRY_NOW;
            n[k2].p[YSIG_DETECT_OUT] = YSIG_OUT_HIT;
            n[k2].p[YSIG_DETECT_START] = 1;
        }
        if (!rp_open(n, 4, K, 0, false, false)) return;
        rp_feed2(10 * MS, 1, 1, 1);
        rp_feed2(15 * MS, 2, 2, 1);
        if (K) rp_feed2(30 * MS, 2.5, 2.5, 1);
        ysig_arm(&g_rp, 1, 20 * MS);
        ysig_arm(&g_rp, 3, 20 * MS);
        if (!K) rp_feed2(30 * MS, 2.5, 2.5, 1);
        m = ysig_detected(&g_rp, h, 64);
        for (k = 0; k < m; k++) if (h[k].node == 3) { found = 1; CHECK(h[k].t == 20 * MS && h[k].value.t == 20 * MS); }
        CHECK(found);
    }
    /* H12 and H16 with values: a waiting arm's going-in lets in, at its
     * time, an earlier group before itself and an equal time's later one
     * after itself. (a) y (entry now, on x's hit output) waits at 30 ms
     * with a set of an offset on device 2; x (entry now, on device 1, in
     * range) waits at 20 ms. Device 2's input past 30 lets y's group in
     * (not x: it does not meet x), y's arm lets x in first: x hits at 20,
     * so y's held value at 30 is 1 and y hits at 30. (b) x and z (entry
     * now, on a latch of x's hit) wait at 20 ms, x's group with the set:
     * device 2's input lets x's group in, x's arm lets z in after itself */
    {
        ysig_node qn[6];
        int sub;
        for (sub = 0; sub < 2; sub++) {
            int found = 0;
            qn[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 1, 2); qn[0].history = 64;
            qn[1] = ysig_node_of(YSIG_OP_DETECT, 0, -1); qn[1].p[YSIG_DETECT_LANE] = 0; qn[1].p[YSIG_DETECT_LEVEL] = 5; qn[1].p[YSIG_DETECT_START] = 1;
            qn[1].p[YSIG_DETECT_ENTRY] = YSIG_ENTRY_NOW; qn[1].p[YSIG_DETECT_OUT] = YSIG_OUT_HIT; qn[1].history = 64;
            qn[2] = ysig_node_of(YSIG_OP_LATCH, 0, 1); qn[2].history = 64;
            qn[3] = ysig_node_of(YSIG_OP_DETECT, sub ? 2 : 1, -1); qn[3].p[YSIG_DETECT_LANE] = 0; qn[3].p[YSIG_DETECT_LEVEL] = sub ? 5 : 0.5; qn[3].p[YSIG_DETECT_START] = 1;
            qn[3].p[YSIG_DETECT_ENTRY] = YSIG_ENTRY_NOW; qn[3].p[YSIG_DETECT_OUT] = YSIG_OUT_HIT; qn[3].history = 64;
            qn[4] = src_node(YIN_KIND_PEN, YIN_AXIS_PEN_PRESSURE, 3, 1);
            qn[5] = ysig_node_of(YSIG_OP_OFFSET, 4, -1);
            if (!rp_open(qn, 6, 4, 0, false, false)) return;
            rp_feed(10 * MS, 10, YIN_AXIS_POSITION, 1);
            rp_feed(10 * MS, 0, YIN_AXIS_PEN_PRESSURE, 3);
            if (!sub) {
                ysig_arm(&g_rp, 1, 20 * MS);
                ysig_set(&g_rp, 5, YSIG_OFFSET_D_X, 3);
                ysig_arm(&g_rp, 3, 30 * MS);
            } else {
                ysig_set(&g_rp, 5, YSIG_OFFSET_D_X, 3);
                ysig_arm(&g_rp, 1, 20 * MS);
                ysig_arm(&g_rp, 3, 20 * MS);
            }
            CHECK(g_rp.wt->n == 2);
            rp_feed(35 * MS, 0, YIN_AXIS_PEN_PRESSURE, 3);
            CHECK(g_rp.wt->n == 0);
            m = ysig_detected(&g_rp, h, 64);
            for (k = 0; k < m; k++) if (h[k].node == 3) { found = 1; CHECK(h[k].t == (sub ? 20 : 30) * MS); }
            CHECK(found);
        }
    }
    /* Residual 1414: a run again whose undo log runs full at its last but
     * one call clears the window with the last call still logged (a late
     * report of device 2 at 0.5 ms).
     * With no checkpoint the log is not kept: a later past arm falls back,
     * counted, and is not made where the stale log puts it */
    {
        int W, kk, stale = 0;
        for (W = 8; W <= 24; W++)
            for (kk = 2; kk <= 4; kk++) {
                ysig_node sn[5];
                ysig_desc d3;
                int j;
                sn[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 0, 2); sn[0].history = 64;
                sn[1] = ysig_node_of(YSIG_OP_DETECT, 0, -1); sn[1].p[YSIG_DETECT_LANE] = 0; sn[1].p[YSIG_DETECT_LEVEL] = 5; sn[1].p[YSIG_DETECT_START] = 1;
                sn[1].p[YSIG_DETECT_OUT] = YSIG_OUT_HIT; sn[1].p[YSIG_DETECT_ENTRY] = YSIG_ENTRY_NOW; sn[1].p[YSIG_DETECT_REPEAT] = 1; sn[1].history = 64;
                sn[2] = ysig_node_of(YSIG_OP_DELAY, 1, -1); sn[2].history = 64;
                sn[3] = src_node(YIN_KIND_PEN, YIN_AXIS_PEN_PRESSURE, 2, 1);
                sn[4] = ysig_node_of(YSIG_OP_DETECT, 3, -1); sn[4].p[YSIG_DETECT_LANE] = 0; sn[4].p[YSIG_DETECT_LEVEL] = 5; sn[4].p[YSIG_DETECT_START] = 1; sn[4].history = 64;
                memset(&d3, 0, sizeof d3);
                d3.nodes = sn; d3.n_nodes = 5; d3.arena = g_rp_ar; d3.catchup_frames = 4; d3.catchup_calls = 256; d3.catchup_writes = W;
                d3.channels = g_ch; d3.n_channels = 64;
                d3.arena_size = ysig_arena_size(&d3);
                if (!d3.arena_size || d3.arena_size > sizeof g_rp_ar || !ysig_open(&g_rp, &d3)) { g_failures++; return; }
                ysig_reset(&g_rp, 0);
                for (j = 0; j < kk; j++) rp_feed((1 + j) * MS, (j & 1) ? 0 : 10, YIN_AXIS_POSITION, 1);
                rp_feed(500000, 1, YIN_AXIS_PEN_PRESSURE, 2);   /* the call left logged: writes little */
                ysig_arm(&g_rp, 1, 1 * MS);
                if (g_rp.cu->n_ck == 0 && g_rp.cu->c_head > g_rp.cu->c_tail) {
                    stale++;
                    ysig_get_stats(&g_rp, &s0);
                    ysig_arm(&g_rp, 4, 750000);   /* after the stale call by time and footprint */
                    ysig_get_stats(&g_rp, &s1);
                    CHECK(s1.arm_past_full - s0.arm_past_full + s1.arm_past_far - s0.arm_past_far == 1);
                }
            }
        /* v0.7.0: the run again that a write-full log starts no longer
         * leaves this state on these shapes (stale is 0 here); the check
         * above stays for a shape that does */
        (void)stale;
    }
    /* Residual 1650: a past arm lets a waiting group in at its place, not
     * at the end of the log. x (on y's hit output) waits at 20 ms; input
     * of device 2 (it meets y's users through a latch, not x) moves N to
     * 30; y armed at 25 ms lets x in. Its place is before the 30 ms input:
     * not pinned */
    {
        ysig_node pn[5];
        pn[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 1, 2); pn[0].history = 64;
        pn[1] = ysig_node_of(YSIG_OP_DETECT, 0, -1); pn[1].p[YSIG_DETECT_LANE] = 0; pn[1].p[YSIG_DETECT_LEVEL] = 5; pn[1].p[YSIG_DETECT_START] = 1;
        pn[1].p[YSIG_DETECT_OUT] = YSIG_OUT_HIT; pn[1].history = 64;
        pn[2] = ysig_node_of(YSIG_OP_DETECT, 1, -1); pn[2].p[YSIG_DETECT_LANE] = 0; pn[2].p[YSIG_DETECT_LEVEL] = 0.5; pn[2].p[YSIG_DETECT_START] = 1; pn[2].history = 64;
        pn[3] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 2, 2); pn[3].history = 64;
        pn[4] = ysig_node_of(YSIG_OP_LATCH, 3, 1); pn[4].history = 64;
        if (!rp_open(pn, 5, 4, 0, false, false)) return;
        rp_frame(10 * MS);
        rp_feed(12 * MS, 0, YIN_AXIS_POSITION, 1);
        rp_feed(12 * MS, 0, YIN_AXIS_POSITION, 2);
        ysig_arm(&g_rp, 2, 20 * MS);
        rp_feed(30 * MS, 0, YIN_AXIS_POSITION, 2);
        CHECK(g_rp.wt->n == 1);
        ysig_get_stats(&g_rp, &s0);
        ysig_arm(&g_rp, 1, 25 * MS);
        ysig_get_stats(&g_rp, &s1);
        CHECK(s1.arm_past_pinned == s0.arm_past_pinned && s1.arm_caught_up == s0.arm_caught_up + 1);
        CHECK(g_rp.wt->n == 0 && ((ysig__det_st*)g_rp.nd[2].st)->t_arm == 20 * MS);
    }
    /* H10: the held rows ran full and the window dropped its checkpoint:
     * an arm whose place it held falls back as arm_past_full, not far */
    n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 0, 2);
    n[1] = ysig_node_of(YSIG_OP_RECORD, 0, -1);
    n[2] = ysig_node_of(YSIG_OP_DETECT, 0, -1); n[2].p[YSIG_DETECT_LANE] = 0; n[2].p[YSIG_DETECT_LEVEL] = 5; n[2].p[YSIG_DETECT_START] = 1;
    g_rp_crows = 8;
    if (!rp_open(n, 3, 3, 32, true, false)) { g_rp_crows = 0; return; }
    g_rp_crows = 0;
    rp_frame(16 * MS);
    for (k = 0; k < 20; k++) rp_feed(16 * MS + (k + 1) * 500000, 0, YIN_AXIS_POSITION, 1);
    ysig_arm(&g_rp, 2, 20 * MS);
    ysig_get_stats(&g_rp, &s1);
    CHECK(s1.arm_past_full == 1 && s1.arm_past_far == 0);
}

/* The second review of 0.6.0's fix round (2026-11-13). */
static ysig_graph* g_r2_g;
static int g_r2_ok[12];
static int g_r2_n;
/* every call that changes the graph, from inside another call: each must
 * be refused (false or -1, nothing done) */
static void r2_reenter_all(void) {
    yin_event e = PEN_E(50 * MS, 1, 99, 99, YIN_TIER_2);
    ysig_frame_in f = frame_at(9, 60 * MS, 0, NULL, 0);
    ysig_entry j;
    int k = 0;
    memset(&j, 0, sizeof j);
    j.kind = YSIG_J_SETTLE;
    g_r2_ok[k++] = ysig_feed(g_r2_g, &e) >= 0;
    g_r2_ok[k++] = ysig_frame(g_r2_g, &f) == 0;
    g_r2_ok[k++] = ysig_set(g_r2_g, 1, YSIG_DETECT_LEVEL, 3);
    g_r2_ok[k++] = ysig_settle(g_r2_g, 40 * MS) >= 0;
    g_r2_ok[k++] = ysig_disarm(g_r2_g, 1);
    g_r2_ok[k++] = ysig_replay(g_r2_g, &j, 1) >= 0;
    g_r2_ok[k++] = ysig_arm(g_r2_g, 1, 1 * MS);
    ysig_reset(g_r2_g, 0);
    ysig_reset_trial(g_r2_g, 0);
    g_r2_n = k;
}
static int g_r2_in;
static void on_row_r2(void* u, const ysig_row* r) {
    (void)u; (void)r;
    if (!g_r2_in) { g_r2_in = 1; r2_reenter_all(); }
}
static void on_journal_r2(void* u, const ysig_entry* e) {
    (void)u;
    if (!g_r2_in && e->kind == YSIG_J_FEED) { g_r2_in = 1; r2_reenter_all(); }
}
static void r2_custom_sample(void* st, const double* p, const ysig_value* in, int n_in, ysig_value* out) {
    (void)st; (void)p; (void)n_in;
    out->v[0] = in[0].v[0];
    if (!g_r2_in) { g_r2_in = 1; r2_reenter_all(); }
}
static void test_arm_review2(void) {
    static uint64_t ar[(4u << 20) / 8];
    static const ysig_op_vtable pass_vt = { YSIG_OP_ABI, "pass", { 2, 0, 0, 0 }, 1, 0, NULL, 0, NULL, r2_custom_sample, NULL };
    const ysig_op_vtable* cv[1];
    ysig_node n[6];
    ysig_desc d;
    ysig_graph g;
    ysig_stats st;
    ysig_hit h[64];
    ysig_frame_in f;
    yin_event e;
    int k, m, way;
    g_where = "review 4";
    /* C (r12): D2 on a mux of pens 2 and 3 that D1's hit selects. D1's hit
     * (20 ms) and D2's (25 ms) are drained together; the caller takes D1's
     * and re-arms D1 at 20 ms. The run again voids D2's hit: the caller
     * may have taken it with D1's, so it is retracted (the player's guard
     * stops), not superseded, which only the re-armed detect's own later
     * hits are */
    n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 1, 2); n[0].history = 64;
    n[1] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 2, 2); n[1].history = 64;
    n[2] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 3, 2); n[2].history = 64;
    n[3] = ysig_node_of(YSIG_OP_DETECT, 0, -1);
    n[3].p[YSIG_DETECT_LANE] = 0; n[3].p[YSIG_DETECT_LEVEL] = 5; n[3].p[YSIG_DETECT_OUT] = YSIG_OUT_HIT; n[3].history = 64;
    n[4] = ysig_node_of(YSIG_OP_MUX, 1, 2); n[4].in[2] = 3; n[4].history = 64;
    n[5] = ysig_node_of(YSIG_OP_DETECT, 4, -1);
    n[5].p[YSIG_DETECT_LANE] = 0; n[5].p[YSIG_DETECT_LEVEL] = 5; n[5].p[YSIG_DETECT_REPEAT] = 1; n[5].history = 64;
    d = desc_of(n, 6); sig_window(&d, 4, 1024); d.arena = ar; d.arena_size = sizeof ar;
    if (!ysig_open(&g, &d)) { printf("open: %s\n", ysig_error(&g)); g_failures++; return; }
    ysig_reset(&g, 0);
    for (k = 1; k <= 3; k++) { e = PEN_E(1 * MS, k, 0, 0, YIN_TIER_2); ysig_feed(&g, &e); }
    f = frame_at(1, 16 * MS, 0, NULL, 0); ysig_frame(&g, &f);
    e = PEN_E(20 * MS, 1, 10, 0, YIN_TIER_2); ysig_feed(&g, &e);
    e = PEN_E(25 * MS, 2, 10, 0, YIN_TIER_2); ysig_feed(&g, &e);
    m = ysig_detected(&g, h, 64);
    CHECK(m == 2 && h[0].node == 3 && h[0].t == 20 * MS && h[1].node == 5 && h[1].t == 25 * MS);
    CHECK(ysig_arm(&g, 3, 20 * MS) && ysig_detected(&g, h, 64) == 0);
    ysig_get_stats(&g, &st);
    CHECK(st.arm_caught_up == 1 && st.arm_hits_retracted == 1 && st.arm_hits_superseded == 0);
    /* G and the suspicion: a call that changes the graph, from on_row,
     * the journal or a custom op during another call, is refused and
     * counted (call_reentry; ysig_arm() in arm_reentry); the outer call
     * runs as with no callback */
    for (way = 0; way < 3; way++) {
        n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 1, 2); n[0].history = 64;
        n[1] = ysig_node_of(YSIG_OP_DETECT, 0, -1);
        n[1].p[YSIG_DETECT_LANE] = 0; n[1].p[YSIG_DETECT_LEVEL] = 5; n[1].history = 64;
        n[2] = way == 2 ? ysig_node_of(YSIG_OP_CUSTOM, 0, -1) : ysig_node_of(YSIG_OP_RECORD, 0, -1);
        d = desc_of(n, 3); sig_window(&d, 4, 1024); d.arena = ar; d.arena_size = sizeof ar;
        if (way == 0) d.on_row = on_row_r2;
        if (way == 1) d.journal = on_journal_r2;
        if (way == 2) { cv[0] = &pass_vt; d.custom = cv; d.n_custom = 1; }
        if (!ysig_open(&g, &d)) { printf("open: %s\n", ysig_error(&g)); g_failures++; return; }
        ysig_reset(&g, 0);
        g_r2_g = &g;
        g_r2_in = 0;
        g_r2_n = 0;
        memset(g_r2_ok, 0xff, sizeof g_r2_ok);
        e = PEN_E(2 * MS, 1, 10, 0, YIN_TIER_2); ysig_feed(&g, &e);
        CHECK(g_r2_n == 7);
        for (k = 0; k < g_r2_n; k++) CHECK(g_r2_ok[k] == 0);
        ysig_get_stats(&g, &st);
        CHECK(st.call_reentry == 8 && st.arm_reentry == 1);
        /* nothing the refused calls asked for happened: the hand at 10,
         * the hit at 2 ms (level 5, armed, not reset) */
        CHECK(ysig_value_of(&g, 0)->v[0] == 10 && ysig_value_of(&g, 0)->t == 2 * MS);
        CHECK(ysig_detected(&g, h, 64) == 1 && h[0].t == 2 * MS && g.nd[1].p[YSIG_DETECT_LEVEL] == 5);
        /* and from outside a callback the same calls work */
        CHECK(ysig_set(&g, 1, YSIG_DETECT_LEVEL, 3) && ysig_disarm(&g, 1));
    }
}

static void test_arm_review(void) {
    static uint64_t ar[(4u << 20) / 8];
    ysig_node n[5];
    ysig_desc d;
    ysig_graph g;
    ysig_stats st;
    ysig_hit h[64];
    ysig_frame_in f;
    yin_event e;
    int64_t t;
    int k, m;
    g_where = "review 3";
#define R3_OPEN(nn) do { d = desc_of(n, (nn)); sig_window(&d, 4, 1024); d.arena = ar; d.arena_size = sizeof ar; \
        if (!ysig_open(&g, &d)) { printf("open: %s\n", ysig_error(&g)); g_failures++; return; } ysig_reset(&g, 0); } while (0)
    /* 1: a late report (dev 2 at 20 ms, after dev 1 at 25 ms) gives the hit
     * the caller takes; a re-arm at 20 ms goes after that report: the test
     * true at the arm is held, no hit again, nothing counted */
    n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 1, 2); n[0].history = 64;
    n[1] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 2, 2); n[1].history = 64;
    n[2] = ysig_node_of(YSIG_OP_SUM, 0, 1); n[2].history = 64;
    n[3] = ysig_node_of(YSIG_OP_DETECT, 2, -1);
    n[3].p[YSIG_DETECT_LANE] = 0; n[3].p[YSIG_DETECT_LEVEL] = 5;
    R3_OPEN(4);
    e = PEN_E(1 * MS, 1, 0, 0, YIN_TIER_2); ysig_feed(&g, &e);
    e = PEN_E(1 * MS, 2, 0, 0, YIN_TIER_2); ysig_feed(&g, &e);
    f = frame_at(1, 16 * MS, 0, NULL, 0); ysig_frame(&g, &f);
    e = PEN_E(25 * MS, 1, 0, 0, YIN_TIER_2); ysig_feed(&g, &e);
    e = PEN_E(20 * MS, 2, 10, 0, YIN_TIER_2); ysig_feed(&g, &e);
    CHECK(ysig_detected(&g, h, 64) == 1 && h[0].t == 20 * MS);
    CHECK(ysig_arm(&g, 3, 20 * MS));
    e = PEN_E(30 * MS, 1, 0, 0, YIN_TIER_2); ysig_feed(&g, &e);
    CHECK(ysig_detected(&g, h, 64) == 0);
    ysig_get_stats(&g, &st);
    CHECK(st.arm_hits_superseded == 0 && st.arm_hits_retracted == 0);
    /* 2: a hold on a view (speed), met at 51 ms, found by the 55 ms sample
     * and taken; a re-arm at 51 ms keeps it: the latch stays frozen */
    n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 0, 2); n[0].history = 256;
    n[1] = ysig_node_of(YSIG_OP_SPEED, 0, -1); n[1].p[YSIG_SPEED_LAG] = 0.004;
    n[2] = ysig_node_of(YSIG_OP_DETECT, 1, -1);
    n[2].p[YSIG_DETECT_TEST] = YSIG_TEST_LE; n[2].p[YSIG_DETECT_LANE] = 0; n[2].p[YSIG_DETECT_LEVEL] = 1;
    n[2].p[YSIG_DETECT_START] = 1; n[2].p[YSIG_DETECT_HOLD] = 0.05; n[2].p[YSIG_DETECT_OUT] = YSIG_OUT_HIT;
    n[3] = ysig_node_of(YSIG_OP_LATCH, 0, 2);
    R3_OPEN(4);
    for (t = 1 * MS; t <= 45 * MS; t += MS) {
        e = PEN_E(t, 1, 0, 0, YIN_TIER_2); ysig_feed(&g, &e);
        if (t == 1 * MS) ysig_arm(&g, 2, 1 * MS);
        if (t == 15 * MS) { f = frame_at(1, 16 * MS, 0, NULL, 0); f.poll_ns = 15 * MS; ysig_frame(&g, &f); }
        if (t == 32 * MS) { f = frame_at(2, 33 * MS, 0, NULL, 0); f.poll_ns = 32 * MS; ysig_frame(&g, &f); }
    }
    f = frame_at(3, 50 * MS, 0, NULL, 0); f.poll_ns = 45 * MS; ysig_frame(&g, &f);
    e = PEN_E(55 * MS, 1, 0, 0, YIN_TIER_2); ysig_feed(&g, &e);
    m = ysig_detected(&g, h, 64);
    ysig_get_stats(&g, &st);
    k = (int)st.hits;
    CHECK(ysig_arm(&g, 2, m == 1 ? h[0].t : 51 * MS));
    e = PEN_E(70 * MS, 1, 20, 0, YIN_TIER_2); ysig_feed(&g, &e);
    ysig_get_stats(&g, &st);
    CHECK(m == 1 && (int)st.hits >= k && st.arm_hits_retracted == 0 && ysig_value_of(&g, 3)->v[0] == 0);
    /* 2b: a frame-rate detect's hold met at 51 ms, which only the next
     * onset (66 ms) would find; input fed to 60 ms. An arm at 55 ms meets
     * it first: the hit at 51 ms is the earlier arm's */
    n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 0, 2); n[0].history = 256;
    n[1] = ysig_node_of(YSIG_OP_DETECT, 0, -1);
    n[1].p[YSIG_DETECT_TEST] = YSIG_TEST_LE; n[1].p[YSIG_DETECT_LANE] = 0; n[1].p[YSIG_DETECT_LEVEL] = 1;
    n[1].p[YSIG_DETECT_START] = 1; n[1].p[YSIG_DETECT_HOLD] = 0.05; n[1].p[YSIG_DETECT_AT] = 1;
    R3_OPEN(2);
    for (t = 1 * MS; t <= 60 * MS; t += MS) {
        e = PEN_E(t, 1, 0, 0, YIN_TIER_2); ysig_feed(&g, &e);
        if (t == 1 * MS) ysig_arm(&g, 1, 1 * MS);
        if (t == 15 * MS) { f = frame_at(1, 16 * MS, 0, NULL, 0); f.poll_ns = 15 * MS; ysig_frame(&g, &f); }
        if (t == 32 * MS) { f = frame_at(2, 33 * MS, 0, NULL, 0); f.poll_ns = 32 * MS; ysig_frame(&g, &f); }
        if (t == 49 * MS) { f = frame_at(3, 50 * MS, 0, NULL, 0); f.poll_ns = 49 * MS; ysig_frame(&g, &f); }
    }
    CHECK(ysig_detected(&g, h, 64) == 0);
    CHECK(ysig_arm(&g, 1, 55 * MS) && ysig_detected(&g, h, 64) == 1 && h[0].t == 51 * MS);
    /* 3: a disarm, then an arm in the past: the arm goes after the disarm
     * (it was made after it), so the detect is armed */
    n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 0, 2); n[0].history = 64;
    n[1] = ysig_node_of(YSIG_OP_DETECT, 0, -1);
    n[1].p[YSIG_DETECT_LANE] = 0; n[1].p[YSIG_DETECT_LEVEL] = 5; n[1].p[YSIG_DETECT_START] = 1;
    R3_OPEN(2);
    for (t = 1 * MS; t <= 40 * MS; t += MS) { e = PEN_E(t, 1, t >= 30 * MS ? 9 : 0, 0, YIN_TIER_2); ysig_feed(&g, &e); }
    ysig_disarm(&g, 1);
    CHECK(ysig_arm(&g, 1, 20 * MS));
    CHECK(((ysig__det_st*)g.nd[1].st)->armed == 1 || ysig_detected(&g, h, 64) == 1);
    /* the same with ysig_set of the level before the arm: the new level
     * from the arm on (9 >= 8: a hit at 41 ms, none at 30 ms) */
    R3_OPEN(2);
    for (t = 1 * MS; t <= 40 * MS; t += MS) { e = PEN_E(t, 1, t >= 30 * MS ? 6 : 0, 0, YIN_TIER_2); ysig_feed(&g, &e); }
    CHECK(ysig_set(&g, 1, YSIG_DETECT_LEVEL, 8));
    CHECK(ysig_arm(&g, 1, 20 * MS) && ysig_detected(&g, h, 64) == 0);
    e = PEN_E(41 * MS, 1, 9, 0, YIN_TIER_2); ysig_feed(&g, &e);
    CHECK(ysig_detected(&g, h, 64) == 1 && h[0].t == 41 * MS);
    /* 4: an arm ahead of the input (150 ms, input to 50 ms) does not meet
     * the earlier arm's hold on unfed time */
    n[1].p[YSIG_DETECT_HOLD] = 0.1; n[1].p[YSIG_DETECT_OUT] = YSIG_OUT_HIT; n[1].p[YSIG_DETECT_LEVEL] = 5;
    R3_OPEN(2);
    e = PEN_E(1 * MS, 1, 10, 0, YIN_TIER_2); ysig_feed(&g, &e);
    ysig_arm(&g, 1, 1 * MS);
    e = PEN_E(50 * MS, 1, 10, 0, YIN_TIER_2); ysig_feed(&g, &e);
    CHECK(ysig_arm(&g, 1, 150 * MS) && ysig_detected(&g, h, 64) == 0);
    e = PEN_E(60 * MS, 1, 0, 0, YIN_TIER_2); ysig_feed(&g, &e);
    CHECK(ysig_detected(&g, h, 64) == 0);
    /* 5: ysig_set of a channel node's channel above the ones read at the
     * open is refused (the window keeps a frame's values up to those) */
    n[0] = ysig_node_of(YSIG_OP_CHANNEL, -1, -1); n[0].p[YSIG_CHANNEL_CHANNEL] = 1;
    n[1] = ysig_node_of(YSIG_OP_DETECT, 0, -1); n[1].p[YSIG_DETECT_LEVEL] = 0.5; n[1].p[YSIG_DETECT_AT] = 1;
    R3_OPEN(2);
    CHECK(!ysig_set(&g, 0, YSIG_CHANNEL_CHANNEL, 3) && ysig_set(&g, 0, YSIG_CHANNEL_CHANNEL, 0));
    /* 6: a run again of 120 emitted events does not count them lost (they
     * are not handed out again); nor 111 new hits (the caller drains) */
    n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 0, 2); n[0].history = 256;
    n[1] = ysig_node_of(YSIG_OP_EMIT, 0, -1);
    n[2] = ysig_node_of(YSIG_OP_DETECT, 0, -1);
    n[2].p[YSIG_DETECT_LANE] = 0; n[2].p[YSIG_DETECT_LEVEL] = 0.5; n[2].p[YSIG_DETECT_START] = 1; n[2].p[YSIG_DETECT_REPEAT] = 1;
    R3_OPEN(3);
    for (t = 0, k = 0; k < 240; k++) {
        yin_event ev[64];
        t += MS / 4;
        e = PEN_E(t, 1, k & 1, 0, YIN_TIER_2); ysig_feed(&g, &e);
        if ((k + 1) % 60 == 0) { f = frame_at(k, t, 0, NULL, 0); ysig_frame(&g, &f); while (ysig_derived(&g, ev, 64) > 0) { } }
    }
    CHECK(ysig_arm(&g, 2, 16 * MS));
    m = 0;
    while ((k = ysig_detected(&g, h, 64)) > 0) m += k;
    ysig_get_stats(&g, &st);
    CHECK(st.derived_lost == 0 && st.hits_lost == 0 && m > 64 && st.arm_caught_up == 1);
    /* 8: ysig_arm() from on_row is refused and counted */
    n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 0, 2);
    n[1] = ysig_node_of(YSIG_OP_DETECT, 0, -1); n[1].p[YSIG_DETECT_START] = 1;
    n[2] = ysig_node_of(YSIG_OP_RECORD, 0, -1);
    d = desc_of(n, 3); sig_window(&d, 4, 1024); d.arena = ar; d.arena_size = sizeof ar; d.on_row = on_row_reenter;
    if (!ysig_open(&g, &d)) { g_failures++; return; }
    ysig_reset(&g, 0);
    g_reent_g = &g;
    g_reent_ok = -1;
    e = PEN_E(2 * MS, 1, 1, 0, YIN_TIER_2); ysig_feed(&g, &e);
    ysig_get_stats(&g, &st);
    CHECK(g_reent_ok == 0 && st.arm_reentry == 1 && ysig_arm(&g, 1, 1 * MS));
    /* 9: the frames-with-values count is 64 bits */
    CHECK(sizeof(((ysig__cu*)0)->slot_next) == 8);
    /* an arm made later with an earlier time than a logged arm (no input
     * between their times): the later time is the one in effect */
    n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 0, 2); n[0].history = 64;
    n[1] = ysig_node_of(YSIG_OP_DETECT, 0, -1); n[1].p[YSIG_DETECT_START] = 1;
    R3_OPEN(2);
    for (t = 1 * MS; t <= 30 * MS; t += 2 * MS) { e = PEN_E(t, 1, 0, 0, YIN_TIER_2); ysig_feed(&g, &e); }
    ysig_arm(&g, 1, 22 * MS);
    ysig_arm(&g, 1, 21 * MS + MS / 2);
    CHECK(((ysig__det_st*)g.nd[1].st)->t_arm == 22 * MS);
#undef R3_OPEN
}

/* A standing hit output: its rise is at the hit, a later evaluation at
 * its own time, so a hold downstream completes from samples and the ring
 * takes every entry (the second review's r13b and r13: an output that
 * kept the hit's time held a hold on all(hit, hit) until a frame, and
 * skipped every ring entry after a late report's hit). No window. */
static void test_hit_out_time(void) {
    ysig_node n[5];
    ysig_desc d;
    ysig_graph g;
    ysig_value v;
    ysig_stats st;
    ysig_hit h[8];
    yin_event e;
    int64_t t;
    int late, k;
    g_where = "hit output time";
    for (late = 0; late < 2; late++) {
        n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 0, 2);
        n[0].history = 1024;
        n[1] = ysig_node_of(YSIG_OP_DETECT, 0, -1);
        n[1].p[YSIG_DETECT_LANE] = 0; n[1].p[YSIG_DETECT_LEVEL] = 5; n[1].p[YSIG_DETECT_OUT] = YSIG_OUT_HIT;
        n[1].history = 64;
        n[2] = ysig_node_of(YSIG_OP_OFFSET, 0, -1);
        n[3] = ysig_node_of(YSIG_OP_ALL, 1, 1);
        n[4] = ysig_node_of(YSIG_OP_DETECT, 3, -1);   /* a 10 ms hold on all(hit, hit) */
        n[4].p[YSIG_DETECT_LANE] = 0; n[4].p[YSIG_DETECT_LEVEL] = 0.5; n[4].p[YSIG_DETECT_HOLD] = 0.01;
        n[4].p[YSIG_DETECT_OUT] = YSIG_OUT_HIT; n[4].history = 64;
        d = desc_of(n, 5);
        if (!open_desc(&g, &d)) { g_failures++; return; }
        ysig_reset(&g, 0);
        if (!late) {
            e = PEN_E(19 * MS, 1, 0, 0, YIN_TIER_2); ysig_feed(&g, &e);
            e = PEN_E(20 * MS, 1, 10, 0, YIN_TIER_2); ysig_feed(&g, &e);   /* the hit at 20 ms */
            for (t = 21 * MS; t <= 60 * MS; t += MS) { e = PEN_E(t, 1, 10, 0, YIN_TIER_2); ysig_feed(&g, &e); }
        } else {
            e = PEN_E(30 * MS, 1, 0, 0, YIN_TIER_2); ysig_feed(&g, &e);
            e = PEN_E(20 * MS, 2, 10, 0, YIN_TIER_2); ysig_feed(&g, &e);   /* a late report: the hit at 20 ms */
            for (t = 31 * MS; t <= 60 * MS; t += MS) { e = PEN_E(t, 1, 10, 0, YIN_TIER_2); ysig_feed(&g, &e); }
        }
        CHECK(ysig_value_of(&g, 1)->v[0] == 1 && ysig_value_of(&g, 1)->t == 60 * MS);
        /* (after a late report the rise is older than the ring's newest
         * entry: skipped and counted, as any late entry) */
        if (!late) CHECK(ysig_value_at(&g, 1, 20 * MS, &v) == 1 && v.v[0] == 1 && v.t == 20 * MS);
        CHECK(ysig_value_at(&g, 1, 50 * MS, &v) == 1 && v.v[0] == 1 && v.t == 50 * MS);
        CHECK(ysig_value_of(&g, 3)->t == 60 * MS);
        k = ysig_detected(&g, h, 8);
        /* in order the hold runs from the hit (20 ms); after the late
         * report from the hold detect's clock, which does not go back
         * (30 ms, counted in time_clamped) */
        CHECK(k == 2 && h[0].node == 1 && h[0].t == 20 * MS && h[1].node == 4 && h[1].t == (late ? 40 : 30) * MS);
        /* the hold's rise is at the hit, found by a later sample, with the
         * sources of the value that started the hold (the hit at 20 ms;
         * after the late report, its device) */
        CHECK(ysig_value_at(&g, 4, h[1].t, &v) == 1 && v.v[0] == 1 && v.t == h[1].t && v.t_oldest == 20 * MS &&
              v.device == (late ? 2u : 1u));
        ysig_get_stats(&g, &st);
        CHECK(st.history_skipped == (late ? 2 : 0));
    }
}

static void test_arm_window(void) {
    ysig_node n[4];
    ysig_desc d;
    ysig_graph g, g2;
    static uint64_t arena2[(1u << 22) / 8];
    ysig_stats st, st2;
    ysig_hit h[8];
    ysig_value v;
    ysig_frame_in f;
    yin_event e;
    int k;
    char err[256];
    g_where = "arm window";
    /* a crossing x >= 5, every crossing, a latch of the hand on its hits */
    n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 1, 2);
    n[0].history = 64;
    n[1] = ysig_node_of(YSIG_OP_DETECT, 0, -1);
    n[1].p[YSIG_DETECT_TEST] = YSIG_TEST_GE; n[1].p[YSIG_DETECT_LANE] = 0; n[1].p[YSIG_DETECT_LEVEL] = 5;
    n[1].p[YSIG_DETECT_START] = 1; n[1].p[YSIG_DETECT_REPEAT] = 1; n[1].p[YSIG_DETECT_OUT] = YSIG_OUT_HIT;
    n[1].history = 64;
    n[2] = ysig_node_of(YSIG_OP_LATCH, 0, 1);
    /* 1: no window: the newest value (0), counted, no hit until a crossing */
    d = desc_of(n, 3);
    if (!open_desc(&g, &d)) { g_failures++; return; }
    ysig_reset(&g, 0);
    e = PEN_E(1 * MS, 1, 0, 0, YIN_TIER_2); ysig_feed(&g, &e);
    e = PEN_E(2 * MS, 1, 6, 0, YIN_TIER_2); ysig_feed(&g, &e);
    CHECK(ysig_arm(&g, 1, 1 * MS) && ysig_detected(&g, h, 8) == 0);
    ysig_get_stats(&g, &st);
    CHECK(st.arm_past_off == 1 && st.arm_caught_up == 0);
    /* 2: a window: two crossings after the arm's time are two hits, the
     * latch froze at the first, the ring holds the rise and the fall */
    d = desc_of(n, 3);
    sig_window(&d, 2, 256);
    d.journal = on_journal;
    g_nj = 0;
    if (!open_desc(&g, &d)) { g_failures++; return; }
    ysig_reset(&g, 0);
    e = PEN_E(1 * MS, 1, 0, 0, YIN_TIER_2); ysig_feed(&g, &e);
    e = PEN_E(2 * MS, 1, 0, 0, YIN_TIER_2); ysig_feed(&g, &e);
    e = PEN_E(3 * MS, 1, 6, 1, YIN_TIER_2); ysig_feed(&g, &e);
    e = PEN_E(4 * MS, 1, 0, 0, YIN_TIER_2); ysig_feed(&g, &e);
    e = PEN_E(5 * MS, 1, 7, 2, YIN_TIER_2); ysig_feed(&g, &e);
    CHECK(ysig_arm(&g, 1, 1 * MS));
    CHECK(ysig_detected(&g, h, 8) == 2 && h[0].t == 3 * MS && h[1].t == 5 * MS && h[0].t_found == 3 * MS);
    CHECK(ysig_value_of(&g, 2)->v[0] == 6 && ysig_value_of(&g, 2)->v[1] == 1 && ysig_value_of(&g, 2)->t == 3 * MS);
    CHECK(ysig_value_at(&g, 1, 3 * MS, &v) == 1 && v.v[0] == 1);
    CHECK(ysig_value_at(&g, 1, 4 * MS, &v) == 1 && v.v[0] == 0);
    CHECK(ysig_value_of(&g, 1)->v[0] == 1 && ysig_value_of(&g, 1)->t == 5 * MS);
    ysig_get_stats(&g, &st);
    CHECK(st.arm_caught_up == 1 && st.hits == 2);
    /* 3: a re-arm at 2 ms, before the hits already handed out: found again
     * and handed out again, as the new arm's (the earlier arm's are
     * superseded) */
    CHECK(ysig_arm(&g, 1, 2 * MS) && ysig_detected(&g, h, 8) == 2 && h[0].t == 3 * MS && h[1].t == 5 * MS);
    ysig_get_stats(&g, &st);
    CHECK(st.arm_caught_up == 2 && st.arm_hits_retracted == 0 && st.arm_hits_superseded == 2);
    /* the journal holds each call once, the arms at their calls: reset,
     * five samples, two arms (a run again journals nothing) */
    CHECK(g_nj == 8);
    /* replay of the journal: the same digest, hits and latch */
    d.journal = NULL;
    d.arena = arena2;
    d.arena_size = sizeof arena2;
    CHECK(ysig_open(&g2, &d));
    CHECK(ysig_replay(&g2, g_j, g_nj) == g_nj);
    ysig_get_stats(&g2, &st2);
    CHECK(st.digest == st2.digest && st.hits == st2.hits && st2.arm_caught_up == 2);
    CHECK(!memcmp(ysig_value_of(&g, 2), ysig_value_of(&g2, 2), sizeof(ysig_value)));
    /* 5: a hold that a frame covers, armed after it (3.11 rule 2): a hit at
     * start + hold at the arm's call; ysig_settle() finds the next one */
    n[1].p[YSIG_DETECT_TEST] = YSIG_TEST_LE; n[1].p[YSIG_DETECT_LANE] = -1; n[1].p[YSIG_DETECT_LEVEL] = 5;
    n[1].p[YSIG_DETECT_HOLD] = 0.003; n[1].p[YSIG_DETECT_REPEAT] = 0;
    d = desc_of(n, 3);
    sig_window(&d, 2, 256);
    if (!open_desc(&g, &d)) { g_failures++; return; }
    ysig_reset(&g, 0);
    e = PEN_E(1 * MS, 1, 1, 0, YIN_TIER_2); ysig_feed(&g, &e);
    f = frame_at(1, 13333333, 0, NULL, 0);
    ysig_frame(&g, &f);
    CHECK(ysig_arm(&g, 1, 2966667));
    CHECK(ysig_detected(&g, h, 8) == 1 && h[0].t == 5966667 && h[0].t_found == 13333333);
    CHECK(ysig_arm(&g, 1, 14 * MS) && ysig_detected(&g, h, 8) == 0);
    CHECK(ysig_settle(&g, 16 * MS) == 0 && ysig_detected(&g, h, 8) == 0);
    CHECK(ysig_settle(&g, 17 * MS) == 0 && ysig_detected(&g, h, 8) == 1 && h[0].t == 17 * MS && h[0].t_found == 17 * MS);
    /* a re-arm before a hit handed out that restarts the hold: the hit at
     * 20 ms is its earlier arm's, which the re-arm ends (superseded,
     * counted); the new one at 21 ms */
    e = PEN_E(15 * MS, 1, 1, 0, YIN_TIER_2); ysig_feed(&g, &e);
    CHECK(ysig_arm(&g, 1, 17 * MS));
    e = PEN_E(20 * MS, 1, 1, 0, YIN_TIER_2); ysig_feed(&g, &e);
    CHECK(ysig_detected(&g, h, 8) == 1 && h[0].t == 20 * MS);
    e = PEN_E(22 * MS, 1, 1, 0, YIN_TIER_2); ysig_feed(&g, &e);
    CHECK(ysig_arm(&g, 1, 18 * MS));
    CHECK(ysig_detected(&g, h, 8) == 1 && h[0].t == 21 * MS);
    ysig_get_stats(&g, &st);
    CHECK(st.arm_hits_superseded == 1 && st.arm_hits_retracted == 0);
    /* a settle does not complete a frame-rate detect's hold, nor one not
     * yet met; its hit rises at start + hold */
    {
        ysig_node m[2];
        m[0] = n[0];
        m[1] = n[1];
        m[1].p[YSIG_DETECT_AT] = 1;
        d = desc_of(m, 2);
        sig_window(&d, 2, 256);
        d.arena = arena2;   /* g keeps g_arena */
        d.arena_size = sizeof arena2;
        if (!ysig_open(&g2, &d)) { g_failures++; return; }
        ysig_reset(&g2, 0);
        e = PEN_E(1 * MS, 1, 1, 0, YIN_TIER_2); ysig_feed(&g2, &e);
        CHECK(ysig_arm(&g2, 1, 1 * MS) && ysig_settle(&g2, 10 * MS) == 0 && ysig_detected(&g2, h, 8) == 0);
    }
    CHECK(ysig_arm(&g, 1, 30 * MS) && ysig_settle(&g, 33 * MS) == 0 && ysig_detected(&g, h, 8) == 1);
    CHECK(ysig_value_of(&g, 1)->v[0] == 1 && ysig_value_of(&g, 1)->t == 33 * MS && ysig_value_of(&g, 2)->v[0] == 1);
    /* 6: an arm before the window's oldest checkpoint: the newest value,
     * counted; logs that ran full: counted */
    for (k = 2; k < 6; k++) { f = frame_at(k, (int64_t)k * 16 * MS, 0, NULL, 0); ysig_frame(&g, &f); }
    CHECK(ysig_arm(&g, 1, 23 * MS));
    ysig_get_stats(&g, &st);
    CHECK(st.arm_past_far == 1);
    d = desc_of(n, 3);
    sig_window(&d, 2, 4);
    if (!open_desc(&g, &d)) { g_failures++; return; }
    ysig_reset(&g, 0);
    for (k = 1; k <= 8; k++) { e = PEN_E(k * MS, 1, 9, 0, YIN_TIER_2); ysig_feed(&g, &e); }
    CHECK(ysig_arm(&g, 1, 2 * MS));
    ysig_get_stats(&g, &st);
    CHECK(st.arm_past_full == 1 && st.arm_caught_up == 0);
    /* after a frame the window works again: a crossing after the arm */
    f = frame_at(1, 10 * MS, 0, NULL, 0);
    ysig_frame(&g, &f);
    e = PEN_E(11 * MS, 1, 1, 0, YIN_TIER_2); ysig_feed(&g, &e);
    e = PEN_E(12 * MS, 1, 9, 0, YIN_TIER_2); ysig_feed(&g, &e);
    e = PEN_E(13 * MS, 1, 1, 0, YIN_TIER_2); ysig_feed(&g, &e);
    CHECK(ysig_arm(&g, 1, 11 * MS));
    ysig_get_stats(&g, &st);
    CHECK(st.arm_caught_up == 1 && st.arm_past_full == 1);
    /* 7: a reset is a barrier: an arm after it with no later input is
     * made where it is */
    ysig_reset(&g, 0);
    CHECK(ysig_arm(&g, 1, 1 * MS));
    ysig_get_stats(&g, &st);
    CHECK(st.arm_past_full == 1 && st.arm_caught_up == 1 && st.arm_past_far == 0);
    /* 8: a record of the detect's output, rows that the run again changed */
    n[1].p[YSIG_DETECT_TEST] = YSIG_TEST_GE; n[1].p[YSIG_DETECT_LANE] = 0; n[1].p[YSIG_DETECT_HOLD] = 0;
    n[2] = ysig_node_of(YSIG_OP_RECORD, 1, -1);
    d = desc_of(n, 3);
    sig_window(&d, 2, 256);
    d.on_row = on_row;
    if (!open_desc(&g, &d)) { g_failures++; return; }
    ysig_reset(&g, 0);
    g_rows = 0;
    e = PEN_E(1 * MS, 1, 0, 0, YIN_TIER_2); ysig_feed(&g, &e);
    e = PEN_E(2 * MS, 1, 6, 0, YIN_TIER_2); ysig_feed(&g, &e);
    k = g_rows;
    CHECK(ysig_arm(&g, 1, 1 * MS) && g_rows == k);   /* not passed again */
    ysig_get_stats(&g, &st);
    CHECK(st.arm_rows_stale == 1);
    /* rows that the run again changes in their metadata only: two samples
     * at one time on one device, equal but for tier and unc_us, and a
     * record of a latch of the hand on the detect's hit. Before the arm
     * the latch follows the hand (the second row has the second sample's
     * tier and unc_us); the arm at 1 ms goes before both, the detect hits
     * at the first and the latch holds it, so the run again's second row
     * has the first sample's: the same t and value (mutant 247) */
    n[1].p[YSIG_DETECT_START] = 1; n[1].p[YSIG_DETECT_REPEAT] = 0;
    n[1].p[YSIG_DETECT_ENTRY] = YSIG_ENTRY_ARM; n[1].p[YSIG_DETECT_OUT] = YSIG_OUT_HIT;
    n[2] = ysig_node_of(YSIG_OP_LATCH, 0, 1);
    n[3] = ysig_node_of(YSIG_OP_RECORD, 2, -1);
    d = desc_of(n, 4);
    sig_window(&d, 2, 256);
    d.on_row = on_row;
    if (!open_desc(&g, &d)) { g_failures++; return; }
    ysig_reset(&g, 0);
    g_rows = 0;
    e = PEN_E(2 * MS, 1, 6, 0, YIN_TIER_2); e.unc_us = 5; ysig_feed(&g, &e);
    e = PEN_E(2 * MS, 1, 6, 0, YIN_TIER_1); e.unc_us = 0; ysig_feed(&g, &e);
    CHECK(g_rows == 2 && g_row[1].value.t == 2 * MS && g_row[1].value.v[0] == 6 && g_row[1].value.tier == YIN_TIER_1 &&
          g_row[1].value.unc_us == 0);
    CHECK(ysig_arm(&g, 1, 1 * MS));
    CHECK(ysig_detected(&g, h, 8) == 1 && h[0].t == 2 * MS);
    ysig_get_stats(&g, &st);
    CHECK(st.arm_caught_up == 1 && st.arm_rows_stale == 1);
    CHECK(ysig_value_of(&g, 2)->t == 2 * MS && ysig_value_of(&g, 2)->v[0] == 6 && ysig_value_of(&g, 2)->tier == YIN_TIER_2 &&
          ysig_value_of(&g, 2)->unc_us == 5);
    /* a frame's channel values are kept with the call: the caller's array
     * changes after the frame, the run again reads what the frame had */
    {
        ysig_node m[2];
        float vals[1];
        m[0] = ysig_node_of(YSIG_OP_CHANNEL, -1, -1);
        m[0].p[0] = 0;
        m[1] = ysig_node_of(YSIG_OP_DETECT, 0, -1);
        m[1].p[YSIG_DETECT_TEST] = YSIG_TEST_GE; m[1].p[YSIG_DETECT_LANE] = 0; m[1].p[YSIG_DETECT_LEVEL] = 5;
        m[1].p[YSIG_DETECT_START] = 1; m[1].p[YSIG_DETECT_AT] = 1;
        d = desc_of(m, 2);
        sig_window(&d, 2, 256);
        if (!open_desc(&g, &d)) { g_failures++; return; }
        ysig_reset(&g, 0);
        vals[0] = 0;
        f = frame_at(1, 16 * MS, 0, vals, 1); ysig_frame(&g, &f);
        vals[0] = 9;
        f = frame_at(2, 32 * MS, 0, vals, 1); ysig_frame(&g, &f);
        vals[0] = 0;
        CHECK(ysig_arm(&g, 1, 20 * MS) && ysig_detected(&g, h, 8) == 1 && h[0].t == 32 * MS);
    }
    /* a hit of another detect, on the re-armed one's output, that the new
     * history does not find: retracted (the caller may have acted on it),
     * counted apart from the re-armed node's own */
    {
        ysig_node m[3];
        m[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 1, 2);
        m[0].history = 64;
        m[1] = ysig_node_of(YSIG_OP_DETECT, 0, -1);
        m[1].p[YSIG_DETECT_TEST] = YSIG_TEST_LE; m[1].p[YSIG_DETECT_LANE] = -1; m[1].p[YSIG_DETECT_LEVEL] = 5;
        m[1].p[YSIG_DETECT_HOLD] = 0.003; m[1].p[YSIG_DETECT_START] = 0; m[1].p[YSIG_DETECT_OUT] = YSIG_OUT_HIT;
        m[1].history = 64;
        m[2] = ysig_node_of(YSIG_OP_DETECT, 1, -1);
        m[2].p[YSIG_DETECT_TEST] = YSIG_TEST_GE; m[2].p[YSIG_DETECT_LANE] = 0; m[2].p[YSIG_DETECT_LEVEL] = 0.5;
        m[2].p[YSIG_DETECT_START] = 0; m[2].p[YSIG_DETECT_REPEAT] = 1;
        d = desc_of(m, 3);
        sig_window(&d, 2, 256);
        if (!open_desc(&g, &d)) { g_failures++; return; }
        ysig_reset(&g, 0);
        e = PEN_E(1 * MS, 1, 1, 0, YIN_TIER_2); ysig_feed(&g, &e);
        e = PEN_E(5 * MS, 1, 1, 0, YIN_TIER_2); ysig_feed(&g, &e);
        CHECK(ysig_detected(&g, h, 8) == 2 && h[0].t == 3 * MS && h[1].t == 3 * MS);
        CHECK(ysig_arm(&g, 1, 2 * MS) && ysig_detected(&g, h, 8) == 2 && h[0].t == 5 * MS && h[1].t == 5 * MS);
        ysig_get_stats(&g, &st);
        /* both hits are later than the arm's time. The re-armed detect's
         * own was its earlier arm's: superseded. The other detect's the
         * caller may have taken with it: retracted (the second review, C) */
        CHECK(st.arm_hits_superseded == 1 && st.arm_hits_retracted == 1);
        /* a re-arm at its own hit's time, which input after that time
         * found: the arm goes before that input and meets the earlier
         * arm's hold up to its time, so its own hit stands. The other
         * detect, on this one's hit output, sees the arm's fall and rise
         * at that time as values (the semantics, D), so its hit at 9.5 ms
         * stands too: nothing more is counted (0.6.0 before the second
         * review counted it retracted, a named exception) */
        e = PEN_E(9 * MS, 1, 1, 0, YIN_TIER_2); ysig_feed(&g, &e);
        CHECK(ysig_detected(&g, h, 8) == 0);
        CHECK(ysig_arm(&g, 1, 6500000) && ysig_detected(&g, h, 8) == 0);
        e = PEN_E(10 * MS, 1, 1, 0, YIN_TIER_2); ysig_feed(&g, &e);
        CHECK(ysig_detected(&g, h, 8) == 2 && h[0].t == 9500000 && h[1].t == 9500000);
        CHECK(ysig_arm(&g, 1, 9500000) && ysig_detected(&g, h, 8) == 0);
        ysig_get_stats(&g, &st);
        CHECK(st.arm_hits_superseded == 1 && st.arm_hits_retracted == 1 && st.arm_caught_up == 3);
        /* an arm at the time the old hold would end, ahead of the input
         * (finding 4): it waits (v0.6.0), so no hit on unfed time. The
         * 5 ms sample is input past 3 ms: the arm goes in before it and
         * meets the old hold (from the reset) at 3 ms, the earlier arm's
         * hit; the new arm's hold starts at 3 ms and ends a hold later */
        d = desc_of(m, 2);
        sig_window(&d, 2, 256);
        if (!open_desc(&g, &d)) { g_failures++; return; }
        ysig_reset(&g, 0);
        e = PEN_E(1 * MS, 1, 1, 0, YIN_TIER_2); ysig_feed(&g, &e);
        CHECK(ysig_arm(&g, 1, 3 * MS) && ysig_detected(&g, h, 8) == 0);
        e = PEN_E(5 * MS, 1, 1, 0, YIN_TIER_2); ysig_feed(&g, &e);
        CHECK(ysig_detected(&g, h, 8) == 1 && h[0].t == 3 * MS);
        e = PEN_E(6 * MS, 1, 1, 0, YIN_TIER_2); ysig_feed(&g, &e);
        CHECK(ysig_detected(&g, h, 8) == 1 && h[0].t == 6 * MS);
        ysig_get_stats(&g, &st);
        CHECK(st.arm_caught_up == 0 && st.hits == 2);
    }
    /* the desc's limits */
    d = desc_of(n, 3);
    sig_window(&d, 9, 256);
    CHECK(!ysig_check(&d, err, sizeof err) && strstr(err, "catchup_frames 9"));
    sig_window(&d, 2, -1);
    CHECK(!ysig_check(&d, err, sizeof err));
}

/* G17: ysig_set() on a stateless node recomputes it and its stateless
 * users from their inputs' held values at once: a frame test and a bind
 * read the new map with no new sample. No ring entry; a stateful node
 * keeps its value until its next sample; replay agrees. */
static void test_set_recompute(void) {
    ysig_node n[8];
    ysig_desc d;
    ysig_graph g, g2;
    static uint64_t arena2[(1u << 20) / 8];
    ysig_stats s1, s2;
    ysig_hit h[8];
    ysig_value at;
    ysig_frame_in f;
    yin_event e;
    int i;
    g_where = "set recompute";
    n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 1, 2);
    n[1] = ysig_node_of(YSIG_OP_AFFINE, 0, -1); n[1].history = 8;          /* the cursor */
    n[2] = ysig_node_of(YSIG_OP_SCALE, 1, -1);                              /* a stateless user */
    n[2].p[YSIG_SCALE_K_X] = n[2].p[YSIG_SCALE_K_Y] = 2;
    n[3] = ysig_node_of(YSIG_OP_DETECT, 1, -1);                             /* a frame test on it */
    n[3].p[YSIG_DETECT_TEST] = YSIG_TEST_LE; n[3].p[YSIG_DETECT_LANE] = 0; n[3].p[YSIG_DETECT_LEVEL] = 5;
    n[3].p[YSIG_DETECT_AT] = 1; n[3].p[YSIG_DETECT_ENTRY] = YSIG_ENTRY_NOW;
    n[4] = ysig_node_of(YSIG_OP_BIND, 1, -1);
    n[5] = ysig_node_of(YSIG_OP_INTEGRATE, 0, -1); n[5].p[YSIG_INTEGRATE_GAIN_X] = 1;   /* stateful */
    n[6] = ysig_node_of(YSIG_OP_OFFSET, 5, -1);                             /* after a stateful node */
    n[7] = ysig_node_of(YSIG_OP_PEAK, 0, -1); n[7].history = 8;             /* lane 0 */
    d = desc_of(n, 8);
    d.journal = on_journal;
    g_nj = 0;
    if (!open_desc(&g, &d)) { g_failures++; return; }
    ysig_reset(&g, 0);
    e = PEN_E(1 * MS, 1, 10, 0, YIN_TIER_SIM); ysig_feed(&g, &e);
    f = frame_at(1, 10 * MS, 0, NULL, 0);
    ysig_frame(&g, &f);
    CHECK(ysig_detected(&g, h, 8) == 0 && g_ch[0] == 10.0f);
    /* turn the cursor 90 deg (y down: x toward y) with the pen still */
    CHECK(ysig_set(&g, 1, YSIG_AFFINE_ANGLE, 90));
    CHECK(fabs(ysig_value_of(&g, 1)->v[0]) < 1e-12 && fabs(ysig_value_of(&g, 1)->v[1] - 10) < 1e-12);
    CHECK(fabs(ysig_value_of(&g, 2)->v[1] - 20) < 1e-12);                   /* its user too */
    CHECK(ysig_value_of(&g, 1)->t == 1 * MS);                               /* the input's time */
    CHECK(ysig_value_at(&g, 1, 1 * MS, &at) == 1 && at.v[0] == 10);         /* no ring entry */
    f = frame_at(2, 20 * MS, 0, NULL, 0);
    ysig_frame(&g, &f);
    CHECK(ysig_detected(&g, h, 8) == 1 && h[0].t == 20 * MS);               /* the frame test reads it */
    CHECK(fabsf(g_ch[0]) < 1e-6f && g_ch[1] == 10.0f);                      /* the bind writes it */
    /* a stateful node and its user: unchanged until the next sample */
    {
        double held = ysig_value_of(&g, 5)->v[0];
        CHECK(ysig_value_of(&g, 6)->v[0] == held);
        CHECK(ysig_set(&g, 5, YSIG_INTEGRATE_GAIN_X, 2));
        CHECK(ysig_value_of(&g, 5)->v[0] == held && ysig_value_of(&g, 6)->v[0] == held);
        CHECK(ysig_set(&g, 6, YSIG_OFFSET_D_X, 1));
        CHECK(ysig_value_of(&g, 5)->v[0] == held && ysig_value_of(&g, 6)->v[0] == held + 1);   /* the offset is stateless */
    }
    /* a peak keeps its peak sample's time: no recompute of a stateful op */
    e = PEN_E(30 * MS, 1, 3, 0, YIN_TIER_SIM); ysig_feed(&g, &e);
    CHECK(ysig_value_of(&g, 7)->v[0] == 10 && ysig_value_of(&g, 7)->t == 1 * MS);
    CHECK(ysig_set(&g, 7, YSIG_PEAK_LANE, 0));
    CHECK(ysig_value_of(&g, 7)->v[0] == 10 && ysig_value_of(&g, 7)->t == 1 * MS);
    CHECK(ysig_value_at(&g, 7, 40 * MS, &at) == 1 && at.v[0] == 10 && at.t == 1 * MS);   /* no entry for a stateful node */
    /* replay: the same values and digest */
    ysig_get_stats(&g, &s1);
    d.journal = NULL;
    d.arena = arena2;
    d.arena_size = sizeof arena2;
    CHECK(ysig_open(&g2, &d));
    CHECK(ysig_replay(&g2, g_j, g_nj) == g_nj);
    ysig_get_stats(&g2, &s2);
    CHECK(s1.digest == s2.digest && s1.hits == s2.hits);
    for (i = 0; i < 8; i++) CHECK(!memcmp(ysig_value_of(&g, i), ysig_value_of(&g2, i), sizeof(ysig_value)));
}

/* v0.4.1, the phase-1 review: a reset-armed entry-now hit rises for its
 * users (a latch freezes, a peak's gate restarts); no hit at an arm on an
 * input with no sample, the first sample inside is one; after ysig_set()
 * a past read at or after the set's time gives the new value, before it
 * the old, and the held value's t and sources stay; replay agrees. */
static void test_review_fixes(void) {
    ysig_node n[5];
    ysig_desc d;
    ysig_graph g, g2;
    static uint64_t arena2[(1u << 20) / 8];
    ysig_stats s1, s2;
    ysig_hit h[8];
    ysig_value at;
    ysig_frame_in f;
    yin_event e;
    int i;
    g_where = "review fixes";
    /* T1: a latch on a reset-armed entry-now hit */
    n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 1, 2);
    n[1] = ysig_node_of(YSIG_OP_DETECT, 0, -1);
    n[1].p[YSIG_DETECT_TEST] = YSIG_TEST_LE; n[1].p[YSIG_DETECT_LANE] = -1; n[1].p[YSIG_DETECT_LEVEL] = 5;
    n[1].p[YSIG_DETECT_ENTRY] = YSIG_ENTRY_NOW; n[1].p[YSIG_DETECT_OUT] = YSIG_OUT_HIT; n[1].history = 8;
    n[2] = ysig_node_of(YSIG_OP_LATCH, 0, 1);
    n[3] = ysig_node_of(YSIG_OP_SPEED, 0, -1); n[3].p[YSIG_SPEED_LAG] = 0.005;
    n[4] = ysig_node_of(YSIG_OP_PEAK, 3, 1);
    d = desc_of(n, 5);
    if (!open_desc(&g, &d)) { g_failures++; return; }
    /* after the open's reset no sample: no hit, the output 0 */
    CHECK(ysig_detected(&g, h, 8) == 0 && ysig_value_of(&g, 1)->v[0] == 0);
    /* the first sample inside after a reset with no sample is the hit, at its time */
    ysig_reset(&g, 5 * MS);
    CHECK(ysig_detected(&g, h, 8) == 0);
    e = PEN_E(10 * MS, 1, 1, 2, YIN_TIER_2); ysig_feed(&g, &e);
    CHECK(ysig_detected(&g, h, 8) == 1 && h[0].t == 10 * MS);
    CHECK(ysig_value_of(&g, 2)->v[0] == 1 && ysig_value_of(&g, 2)->v[1] == 2);   /* froze there */
    /* a reset with a held sample: the source resets too, so feed a graph
     * whose source keeps its value: an adapter */
    {
        ysig_node ex[4], ad[1];
        ysig_role ro;
        ysig_adapter a;
        memset(&ro, 0, sizeof ro);
        ro.name = "hand"; ro.type = YSIG_TYPE_POSITION2D; ro.units = YSIG_UNITS_DISPLAY_PX;
        ad[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 1, 2);
        memset(&a, 0, sizeof a);
        a.role = "hand"; a.name = "pen"; a.type = YSIG_TYPE_POSITION2D; a.units = YSIG_UNITS_DISPLAY_PX; a.nodes = ad; a.n_nodes = 1;
        ex[0] = ysig_node_of(YSIG_OP_ROLE, -1, -1);
        ex[1] = n[1]; ex[1].in[0] = 0;
        ex[2] = ysig_node_of(YSIG_OP_LATCH, 0, 1);
        ex[3] = ysig_node_of(YSIG_OP_PEAK, 0, 1); ex[3].p[YSIG_PEAK_LANE] = 0;
        d = desc_of(ex, 4);
        d.roles = &ro; d.n_roles = 1; d.adapters = &a; d.n_adapters = 1;
        d.journal = on_journal;
        g_nj = 0;
        if (!open_desc(&g, &d)) { g_failures++; return; }
        ysig_reset(&g, 0);
        e = PEN_E(10 * MS, 1, 1, 2, YIN_TIER_2); ysig_feed(&g, &e);
        while (ysig_detected(&g, h, 8) > 0) { }
        e = PEN_E(12 * MS, 1, 3, 0, YIN_TIER_2); ysig_feed(&g, &e);
        ysig_reset_trial(&g, 15 * MS);                        /* the hand is in at the reset */
        CHECK(ysig_detected(&g, h, 8) == 1 && h[0].t == 15 * MS && h[0].value.v[0] == 3);
        CHECK(ysig_value_of(&g, 1)->v[0] == 1 && ysig_value_of(&g, 1)->t == 15 * MS);
        e = PEN_E(30 * MS, 1, 9, 0, YIN_TIER_2); ysig_feed(&g, &e);
        CHECK(ysig_value_of(&g, 2)->v[0] == 3 && ysig_value_of(&g, 2)->v[1] == 0);   /* the latch froze at the reset's hit */
        CHECK(ysig_value_of(&g, 3)->v[0] == 9);                                      /* the gate rose: the peak runs */
        CHECK(ysig_value_at(&g, 1, 14 * MS, &at) >= 0);
        ysig_get_stats(&g, &s1);
        d.journal = NULL;
        d.arena = arena2;
        d.arena_size = sizeof arena2;
        CHECK(ysig_open(&g2, &d));
        CHECK(ysig_replay(&g2, g_j, g_nj) == g_nj);
        ysig_get_stats(&g2, &s2);
        CHECK(s1.digest == s2.digest && s1.hits == s2.hits);
        for (i = 0; i < 4; i++) CHECK(!memcmp(ysig_value_of(&g, i), ysig_value_of(&g2, i), sizeof(ysig_value)));
    }
    /* T3: value_at and value_of agree after a set; a read before it, the old */
    n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 1, 2);
    n[1] = ysig_node_of(YSIG_OP_AFFINE, 0, -1); n[1].history = 8;
    n[2] = ysig_node_of(YSIG_OP_DETECT, 1, -1);
    n[2].p[YSIG_DETECT_TEST] = YSIG_TEST_LE; n[2].p[YSIG_DETECT_LANE] = 0; n[2].p[YSIG_DETECT_LEVEL] = 5;
    n[2].p[YSIG_DETECT_AT] = 1; n[2].p[YSIG_DETECT_ENTRY] = YSIG_ENTRY_NOW;
    d = desc_of(n, 3);
    d.journal = on_journal;
    g_nj = 0;
    if (!open_desc(&g, &d)) { g_failures++; return; }
    ysig_reset(&g, 0);
    e = PEN_E(1 * MS, 1, 10, 0, YIN_TIER_SIM); ysig_feed(&g, &e);
    f = frame_at(1, 10 * MS, 0, NULL, 0); ysig_frame(&g, &f);
    CHECK(ysig_set(&g, 1, YSIG_AFFINE_ANGLE, 90));
    f = frame_at(2, 20 * MS, 0, NULL, 0); ysig_frame(&g, &f);
    CHECK(ysig_detected(&g, h, 8) == 1 && h[0].t == 20 * MS);
    CHECK(ysig_value_at(&g, 1, 20 * MS, &at) == 1 && fabs(at.v[0]) < 1e-12 && at.v[1] == 10);
    CHECK(ysig_value_at(&g, 1, 10 * MS, &at) == 1 && fabs(at.v[0]) < 1e-12);    /* the set's time: the last onset */
    CHECK(ysig_value_at(&g, 1, 9 * MS, &at) == 1 && at.v[0] == 10 && at.v[1] == 0);   /* before it: the old map */
    CHECK(ysig_value_of(&g, 1)->t == 1 * MS);                                     /* t stays the sample's */
    ysig_get_stats(&g, &s1);
    d.journal = NULL;
    d.arena = arena2;
    d.arena_size = sizeof arena2;
    CHECK(ysig_open(&g2, &d));
    CHECK(ysig_replay(&g2, g_j, g_nj) == g_nj);
    CHECK(ysig_value_at(&g2, 1, 20 * MS, &at) == 1 && fabs(at.v[0]) < 1e-12 && at.v[1] == 10);
    CHECK(ysig_value_at(&g2, 1, 9 * MS, &at) == 1 && at.v[0] == 10);
    ysig_get_stats(&g2, &s2);
    CHECK(s1.digest == s2.digest);
    /* T6: a set keeps a two-input node's t and sources */
    n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 1, 2);
    n[1] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 2, 2);
    n[2] = ysig_node_of(YSIG_OP_SUM, 0, 1);
    d = desc_of(n, 3);
    if (!open_desc(&g, &d)) { g_failures++; return; }
    ysig_reset(&g, 0);
    e = PEN_E(50 * MS, 2, 1, 1, YIN_TIER_SIM); ysig_feed(&g, &e);
    e = PEN_E(30 * MS, 1, 1, 1, YIN_TIER_SIM); ysig_feed(&g, &e);
    at = *ysig_value_of(&g, 2);
    CHECK(ysig_set(&g, 2, YSIG_SUM_W_0, 2));
    CHECK(ysig_value_of(&g, 2)->t == at.t && ysig_value_of(&g, 2)->src == at.src && ysig_value_of(&g, 2)->device == at.device);
    CHECK(ysig_value_of(&g, 2)->v[0] == 3 && at.v[0] == 2);
}

/* T16: a hold on a frame-updated input completes for a still hand */
static void test_hold_frame_input(void) {
    ysig_node n[5];
    ysig_desc d;
    ysig_graph g;
    ysig_hit h[4];
    ysig_value v;
    ysig_frame_in f;
    yin_event e;
    float c[1];
    int kind, k, nh;
    g_where = "hold on a frame-updated input";
    /* a mux with a channel selector, a mux with a frame-rate detect's
     * selector, a latch with a channel trigger */
    for (kind = 0; kind < 3; kind++) {
        n[0] = src_node(YIN_KIND_PEN, 0, 0, 1);
        n[1] = ysig_node_of(YSIG_OP_CHANNEL, -1, -1);
        if (kind == 1) {
            n[1] = ysig_node_of(YSIG_OP_DETECT, 0, -1);
            n[1].p[YSIG_DETECT_TEST] = YSIG_TEST_LE; n[1].p[YSIG_DETECT_LEVEL] = 100; n[1].p[YSIG_DETECT_AT] = 1;
        }
        if (kind == 2) { n[2] = ysig_node_of(YSIG_OP_LATCH, 0, 1); }
        else { n[2] = ysig_node_of(YSIG_OP_MUX, 0, 0); n[2].in[2] = 1; }
        n[3] = ysig_node_of(YSIG_OP_DETECT, 2, -1);
        n[3].p[YSIG_DETECT_TEST] = YSIG_TEST_LE; n[3].p[YSIG_DETECT_LEVEL] = 10; n[3].p[YSIG_DETECT_HOLD] = 0.5;
        d = desc_of(n, 4);
        if (!open_desc(&g, &d)) { g_failures++; return; }
        ysig_reset(&g, 0);
        c[0] = kind == 2 ? 0.0f : 1.0f;
        f = frame_at(1, 5 * MS, 0, c, 1); ysig_frame(&g, &f);
        e = PEN1_E(10 * MS, 1, 1); ysig_feed(&g, &e);
        nh = 0;
        for (k = 2; k <= 100; k++) {
            int got;
            /* the poll is 2 ms before the onset: the hold ends on the poll */
            f = frame_poll(k, k * 16 * MS, k * 16 * MS - 2 * MS, c, 1);
            ysig_frame(&g, &f);
            got = ysig_detected(&g, h, 4);
            if (got) { CHECK(h[0].t_found == k * 16 * MS - 2 * MS && h[0].t == 500 * MS); nh += got; }
        }
        CHECK(nh == 1);
    }
    /* a hold that starts in a frame's pass starts at the known time, not at
     * the value's old t; a hit output rises at the hit */
    n[0] = src_node(YIN_KIND_PEN, 0, 0, 1);
    n[1] = ysig_node_of(YSIG_OP_CHANNEL, -1, -1);
    n[2] = ysig_node_of(YSIG_OP_OFFSET, 0, -1); n[2].p[0] = 50;
    n[3] = ysig_node_of(YSIG_OP_MUX, 0, 2); n[3].in[2] = 1;
    n[4] = ysig_node_of(YSIG_OP_DETECT, 3, -1);
    n[4].p[YSIG_DETECT_TEST] = YSIG_TEST_LE; n[4].p[YSIG_DETECT_LEVEL] = 10; n[4].p[YSIG_DETECT_HOLD] = 0.1;
    n[4].p[YSIG_DETECT_OUT] = YSIG_OUT_HIT; n[4].history = 32;
    d = desc_of(n, 5);
    if (!open_desc(&g, &d)) { g_failures++; return; }
    ysig_reset(&g, 0);
    e = PEN1_E(10 * MS, 1, 1); ysig_feed(&g, &e);              /* b is 51: the test is false */
    c[0] = 1;
    f = frame_poll(1, 200 * MS, 198 * MS, c, 1); ysig_frame(&g, &f);   /* a chosen: true; the hold starts at 198 */
    f = frame_poll(2, 300 * MS, 297 * MS, c, 1); ysig_frame(&g, &f);
    CHECK(ysig_detected(&g, h, 4) == 0);
    f = frame_poll(3, 310 * MS, 298 * MS, c, 1); ysig_frame(&g, &f);
    CHECK(ysig_detected(&g, h, 4) == 1 && h[0].t == 298 * MS && h[0].t_found == 298 * MS);
    CHECK(ysig_value_of(&g, 4)->v[0] == 1 && ysig_value_of(&g, 4)->t == 298 * MS);
    /* U2: a frame-rate detect's hold ends on the onset */
    n[1] = ysig_node_of(YSIG_OP_DETECT, 0, -1);
    n[1].p[YSIG_DETECT_TEST] = YSIG_TEST_LE; n[1].p[YSIG_DETECT_LEVEL] = 10; n[1].p[YSIG_DETECT_HOLD] = 0.1;
    n[1].p[YSIG_DETECT_AT] = 1; n[1].p[YSIG_DETECT_ENTRY] = 1; n[1].p[YSIG_DETECT_START] = 1;
    d = desc_of(n, 2);
    if (!open_desc(&g, &d)) { g_failures++; return; }
    ysig_reset(&g, 0);
    ysig_arm(&g, 1, 0);
    e = PEN1_E(1 * MS, 1, 50); ysig_feed(&g, &e);
    f = frame_poll(1, 20 * MS, 10 * MS, NULL, 0); ysig_frame(&g, &f);     /* the test false: ready */
    e = PEN1_E(25 * MS, 1, 1); ysig_feed(&g, &e);
    f = frame_poll(2, 40 * MS, 30 * MS, NULL, 0); ysig_frame(&g, &f);     /* true: the hold starts at 30 */
    f = frame_poll(3, 139 * MS, 129 * MS, NULL, 0); ysig_frame(&g, &f);   /* the hold started at the onset, 40 ms */
    CHECK(ysig_detected(&g, h, 4) == 0);
    f = frame_poll(4, 140 * MS, 130 * MS, NULL, 0); ysig_frame(&g, &f);
    CHECK(ysig_detected(&g, h, 4) == 1 && h[0].t == 140 * MS && h[0].t_found == 140 * MS);
    /* a hit output's rise is at the hit when a later sample finds it */
    n[1] = ysig_node_of(YSIG_OP_DETECT, 0, -1);
    n[1].p[YSIG_DETECT_TEST] = YSIG_TEST_LE; n[1].p[YSIG_DETECT_LEVEL] = 10; n[1].p[YSIG_DETECT_HOLD] = 0.1;
    n[1].p[YSIG_DETECT_OUT] = YSIG_OUT_HIT; n[1].history = 32;
    d = desc_of(n, 2);
    if (!open_desc(&g, &d)) { g_failures++; return; }
    ysig_reset(&g, 0);
    e = PEN1_E(140 * MS, 1, 1); ysig_feed(&g, &e);
    CHECK(ysig_detected(&g, h, 4) == 1 && h[0].t == 100 * MS && h[0].t_found == 140 * MS);
    CHECK(ysig_value_of(&g, 1)->v[0] == 1 && ysig_value_of(&g, 1)->t == 100 * MS);
    CHECK(ysig_value_at(&g, 1, 120 * MS, &v) == 1 && v.v[0] == 1);
    /* U2: a sample-rate detect on a sample input ends on the onset */
    n[1] = ysig_node_of(YSIG_OP_DETECT, 0, -1);
    n[1].p[YSIG_DETECT_TEST] = YSIG_TEST_LE; n[1].p[YSIG_DETECT_LEVEL] = 10; n[1].p[YSIG_DETECT_HOLD] = 0.1;
    n[1].p[YSIG_DETECT_REPEAT] = 1; n[1].p[YSIG_DETECT_OUT] = YSIG_OUT_HIT; n[1].history = 32;
    d = desc_of(n, 2);
    if (!open_desc(&g, &d)) { g_failures++; return; }
    ysig_reset(&g, 0);
    e = PEN1_E(10 * MS, 1, 1); ysig_feed(&g, &e);             /* armed at the reset with the zero value: true from 0 */
    f = frame_poll(1, 99 * MS, 50 * MS, NULL, 0); ysig_frame(&g, &f);
    CHECK(ysig_detected(&g, h, 4) == 0 && ysig_value_of(&g, 1)->v[0] == 0);
    /* found at the onset, though the poll is before the hold's end; the
     * rise at the hit (100 ms): a past read between the hit and the frame
     * says 1, and the sample that follows, older than the onset, enters
     * the ring */
    f = frame_poll(2, 300 * MS, 90 * MS, NULL, 0); ysig_frame(&g, &f);
    CHECK(ysig_detected(&g, h, 4) == 1 && h[0].t == 100 * MS && h[0].t_found == 300 * MS);
    CHECK(ysig_value_of(&g, 1)->v[0] == 1 && ysig_value_of(&g, 1)->t == 100 * MS);
    CHECK(ysig_value_at(&g, 1, 120 * MS, &v) == 1 && v.v[0] == 1);
    CHECK(ysig_value_at(&g, 1, 90 * MS, &v) == 1 && v.v[0] == 0);
    e = PEN1_E(170 * MS, 1, 50); ysig_feed(&g, &e);            /* the test fails: ready again, fired 0 */
    CHECK(ysig_value_of(&g, 1)->v[0] == 0);
    CHECK(ysig_value_at(&g, 1, 175 * MS, &v) == 1 && v.v[0] == 0);
    {
        ysig_stats st;
        ysig_get_stats(&g, &st);
        CHECK(st.history_skipped == 0);
    }
}

/* U1 to U5 of the second review */
static void test_u(void) {
    ysig_graph g;
    ysig_desc d;
    ysig_frame_in f;
    yin_event e;
    ysig_value at;
    ysig_stats st;
    const int64_t fp = 16666667LL;
    int k;
    g_where = "second review";
    /* U2: found on the frame f7b5417 found it: the poll a frame before the
     * onset, a hold to 510 ms on a sample-rate source, a still hand */
    {
        ysig_node n[2];
        ysig_hit h[4];
        int found = 0;
        n[0] = src_node(YIN_KIND_PEN, 0, 0, 1);
        n[1] = ysig_node_of(YSIG_OP_DETECT, 0, -1);
        n[1].p[YSIG_DETECT_TEST] = YSIG_TEST_LE; n[1].p[YSIG_DETECT_LEVEL] = 10; n[1].p[YSIG_DETECT_HOLD] = 0.5;
        n[1].p[YSIG_DETECT_ENTRY] = 1;
        d = desc_of(n, 2);
        if (!open_desc(&g, &d)) { g_failures++; return; }
        ysig_reset(&g, 0);
        e = PEN1_E(1 * MS, 1, 50); ysig_feed(&g, &e);
        e = PEN1_E(10 * MS, 1, 1); ysig_feed(&g, &e);
        for (k = 1; k <= 60 && !found; k++) {
            f = frame_poll(k, (k + 1) * fp, k * fp, NULL, 0);
            ysig_frame(&g, &f);
            if (ysig_detected(&g, h, 4)) { found = k; CHECK(h[0].t == 510 * MS && h[0].t_found == (k + 1) * fp); }
        }
        CHECK(found == 30);                                           /* 31 frames = 516.7 ms, the first onset past 510 */
    }
    /* U1: one bad stamp does not re-key a generator's ring: the same past
     * reads with and without it */
    {
        static const double comp[3] = { 1, 1, 0 };
        ysig_node n[3];
        double out[2][30];
        int bad;
        n[0] = src_node(YIN_KIND_PEN, 0, 0, 1);
        n[1] = ysig_node_of(YSIG_OP_SINES, -1, -1); n[1].p[0] = 1; n[1].data = comp; n[1].n_data = 3;
        n[2] = ysig_node_of(YSIG_OP_SUM, 0, 1); n[2].history = 256;
        d = desc_of(n, 3);
        for (bad = 0; bad < 2; bad++) {
            if (!open_desc(&g, &d)) { g_failures++; return; }
            ysig_reset(&g, 0);
            e = PEN1_E(1 * MS, 1, 10); ysig_feed(&g, &e);
            if (bad) { e = PEN1_E(3600000LL * MS, 2, 10); ysig_feed(&g, &e); }
            for (k = 1; k <= 30; k++) { f = frame_poll(k, (k + 2) * fp, k * fp, NULL, 0); f.base_ns = (k + 2) * fp; ysig_frame(&g, &f); }
            for (k = 0; k < 30; k++) {
                CHECK(ysig_value_at(&g, 2, (k + 3) * fp + 1000, &at) == 1);
                out[bad][k] = at.v[0];
                CHECK_NEAR(at.v[0], 10 + cos(2 * PI_ * (double)((k + 3) * fp) / 1e9), 1e-9);
            }
        }
        for (k = 0; k < 30; k++) CHECK(out[0][k] == out[1][k]);
    }
    /* U5: a poll a little behind the newest sample changes nothing */
    {
        static const int64_t gaps[3] = { 0, 2 * MS, 5 * MS };
        int gi;
        for (gi = 0; gi < 3; gi++) {
            ysig_node n[3];
            int sample = 1, fr_i;
            n[0] = src_node(YIN_KIND_PEN, 0, 0, 1);
            n[1] = ysig_node_of(YSIG_OP_SPEED, 0, -1); n[1].p[0] = 0.010;
            n[2] = ysig_node_of(YSIG_OP_DELAY, 0, -1); n[2].p[0] = 0.002;
            d = desc_of(n, 3);
            if (!open_desc(&g, &d)) { g_failures++; return; }
            ysig_reset(&g, 0);
            for (fr_i = 1; fr_i <= 120; fr_i++) {
                int64_t now = fr_i * fp;
                for (; sample * MS <= now; sample++) {
                    e = PEN1_E(sample * MS, 1, sample); ysig_feed(&g, &e);
                    if (fr_i > 3) {
                        CHECK(fabs(ysig_value_of(&g, 1)->v[0] - 1000) < 1e-6);
                        CHECK(ysig_value_of(&g, 2)->v[0] == sample - 2);
                    }
                }
                f = frame_poll(fr_i, now + fp, now - gaps[gi], NULL, 0);
                ysig_frame(&g, &f);
            }
            ysig_get_stats(&g, &st);
            CHECK(st.dropped_order == 0);
        }
    }
    /* U4: value_at after an arm agrees with the live value */
    {
        ysig_node n[5];
        n[0] = src_node(YIN_KIND_PEN, 0, 1, 1);
        n[1] = src_node(YIN_KIND_PEN, 0, 2, 1);
        n[2] = ysig_node_of(YSIG_OP_DETECT, 0, -1);
        n[2].p[YSIG_DETECT_LEVEL] = 5; n[2].p[YSIG_DETECT_OUT] = YSIG_OUT_HIT; n[2].p[YSIG_DETECT_START] = 1;
        n[3] = ysig_node_of(YSIG_OP_MUX, 0, 1); n[3].in[2] = 2; n[3].history = 32;
        n[4] = ysig_node_of(YSIG_OP_BIND, 3, -1);
        d = desc_of(n, 5);
        if (!open_desc(&g, &d)) { g_failures++; return; }
        ysig_reset(&g, 0);
        e = PEN1_E(1 * MS, 2, 100); ysig_feed(&g, &e);
        e = PEN1_E(2 * MS, 1, 1); ysig_feed(&g, &e);
        ysig_arm(&g, 2, 3 * MS);
        e = PEN1_E(4 * MS, 1, 7); ysig_feed(&g, &e);
        CHECK(ysig_value_of(&g, 3)->v[0] == 7);
        CHECK(ysig_settle(&g, 5 * MS) == 0);   /* input in to 5 ms: the arm is made at once */
        CHECK(ysig_arm(&g, 2, 5 * MS));
        CHECK(ysig_value_of(&g, 3)->v[0] == 100);
        CHECK(ysig_value_at(&g, 3, 6 * MS, &at) == 1 && at.v[0] == 100);
        CHECK(ysig_value_at(&g, 3, 4500000, &at) == 1 && at.v[0] == 7);       /* before the arm: the hit stood */
    }
    /* U3 is documented, not coded: a recenter at the poll's time is in
     * order with the samples that follow */
    {
        ysig_node a[2], n[1];
        int id;
        a[0] = src_node(YIN_KIND_MOUSE, YIN_AXIS_DELTA, 0, 2);
        a[1] = ysig_node_of(YSIG_OP_INTEGRATE, 0, -1); a[1].history = 64;
        n[0] = ysig_node_of(YSIG_OP_ROLE, -1, -1);
        if (!open_adapter(&g, n, 1, a, 2, YSIG_TYPE_POSITION2D_RELATIVE, NULL)) { g_failures++; return; }
        ysig_reset(&g, 0);
        e = delta(10 * MS, 1, 40, 0); ysig_feed(&g, &e);
        CHECK(ysig_recenter(&g, 0, 0, 0, 12 * MS));
        e = delta(15 * MS, 1, 5, 0); ysig_feed(&g, &e);
        e = delta(20 * MS, 1, 5, 0); ysig_feed(&g, &e);
        id = ysig_adapter_node(&g, 0, 1);
        CHECK(ysig_value_of(&g, id)->v[0] == 10);
        CHECK(ysig_value_at(&g, id, 100 * MS, &at) == 1 && at.v[0] == 10);
        ysig_get_stats(&g, &st);
        CHECK(st.history_skipped == 0);
    }
}

/* v0.7.0, late input (docs/sig.md, "Late input"): a call goes into R by
 * its key, and a late one runs the window again. */
static int64_t g_lt_hit[8];
static int lt_hits(void) {
    ysig_hit h[8];
    int m, k, n = 0;
    while ((m = ysig_detected(&g_rp, h, 8)) > 0) for (k = 0; k < m && n < 8; k++) g_lt_hit[n++] = h[k].t;
    return n;
}
static void lt_det_open(double hold_s, int K) {
    ysig_node n[2];
    n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 0, 2); n[0].history = 64;   /* any device */
    n[1] = ysig_node_of(YSIG_OP_DETECT, 0, -1); n[1].p[YSIG_DETECT_LANE] = 0; n[1].p[YSIG_DETECT_LEVEL] = 5;
    n[1].p[YSIG_DETECT_START] = 1; n[1].p[YSIG_DETECT_REPEAT] = 1; n[1].p[YSIG_DETECT_HOLD] = hold_s; n[1].history = 64;
    (void)rp_open(n, 2, K, 0, false, false);
}
/* R1 and R2 (the coordinator's rulings of 2026-11-15, docs/sig.md "Late
 * input"): a frame, a settle and a state call at one time go by call
 * order; a frame or a settle lets in the waiting groups at or before its
 * onset; L(G) can be a frame at tau; a plain disarm placed in a group's
 * gap counts the group (condition 4). Each case with a control. A detect
 * at frame rate, met at <= 5, a hit at the frame where it is armed */
static void ft_open(void) {
    ysig_node n[2];
    n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 0, 2); n[0].history = 64;
    n[1] = ysig_node_of(YSIG_OP_DETECT, 0, -1);
    n[1].p[YSIG_DETECT_TEST] = YSIG_TEST_LE; n[1].p[YSIG_DETECT_LANE] = 0; n[1].p[YSIG_DETECT_LEVEL] = 5;
    n[1].p[YSIG_DETECT_START] = 1; n[1].p[YSIG_DETECT_ENTRY] = YSIG_ENTRY_NOW; n[1].p[YSIG_DETECT_AT] = 1; n[1].history = 64;
    (void)rp_open(n, 2, 4, 0, false, false);
}
static int ft_bad(void) {
    ysig_stats st;
    ysig_get_stats(&g_rp, &st);
    return (int)(st.arm_unverified + st.late_unverified + st.late_far + st.late_full + st.late_off + st.arm_hits_retracted +
                 st.late_hits_voided + st.arm_past_far + st.arm_past_full + st.arm_past_off + st.arm_past_pinned);
}
static void test_frame_ties(void) {
    ysig_stats st;
    int v;
    g_where = "frame ties (R1, R2)";
    /* the player's 500 Hz tie: a state ends at a frame's onset (4 ms), the
     * next arms there after a settle at it, then the frame: it comes after
     * the arm in R and sees it, a hit at 4 with no run again (0.7.0 by
     * class put the frame first: a hit at 6). The control: the frame made
     * before the arm does not see it: a hit at the next frame */
    for (v = 0; v < 2; v++) {
        ft_open();
        rp_feed(3 * MS, 1, YIN_AXIS_POSITION, 1);
        CHECK(ysig_settle(&g_rp, 4 * MS) >= 0);
        if (!v) { CHECK(ysig_arm(&g_rp, 1, 4 * MS)); rp_frame(4 * MS); }
        else { rp_frame(4 * MS); CHECK(ysig_arm(&g_rp, 1, 4 * MS)); }
        rp_frame(6 * MS);
        ysig_get_stats(&g_rp, &st);
        CHECK(lt_hits() == 1 && g_lt_hit[0] == (v ? 6 : 4) * MS);
        CHECK(st.arm_caught_up == 0 && st.late_frames == 0 && ft_bad() == 0);
    }
    /* a waiting group at tau 4 goes in right before the frame at 4
     * (inclusive), which is its L(G): a hit at 4, not counted. The
     * control: tau 5 waits past the frame at 4 and goes in at the frame at
     * 6, an input later than tau */
    for (v = 0; v < 2; v++) {
        ft_open();
        rp_feed(1 * MS, 1, YIN_AXIS_POSITION, 1);
        CHECK(ysig_arm(&g_rp, 1, (v ? 5 : 4) * MS) && g_rp.wt->n == 1);
        rp_frame(4 * MS);
        CHECK(g_rp.wt->n == v);
        rp_frame(6 * MS);
        CHECK(lt_hits() == 1 && g_lt_hit[0] == (v ? 6 : 4) * MS && ft_bad() == 0);
    }
    /* ysig_disarm_at(4) then the frame at 4: the frame comes after the
     * disarm and does not see the arm. The control: the frame first, a hit
     * at 4, then the disarm */
    for (v = 0; v < 2; v++) {
        ft_open();
        rp_feed(1 * MS, 9, YIN_AXIS_POSITION, 1);
        CHECK(ysig_arm(&g_rp, 1, 1 * MS));
        rp_frame(2 * MS);
        rp_feed(3 * MS, 1, YIN_AXIS_POSITION, 1);
        if (!v) { CHECK(ysig_disarm_at(&g_rp, 1, 4 * MS)); rp_frame(4 * MS); }
        else { rp_frame(4 * MS); CHECK(ysig_disarm_at(&g_rp, 1, 4 * MS)); }
        CHECK(lt_hits() == v && (!v || g_lt_hit[0] == 4 * MS) && ft_bad() == 0);
    }
    /* a set, then an arm ahead at 4 (one group, the set in its gap), then
     * the frame at 4: the frame lets the group in and sees it, a hit at 4.
     * The control: the frame between the set and the arm ends the set's
     * gap (no group) and comes before the arm: a hit at the next frame */
    for (v = 0; v < 2; v++) {
        ft_open();
        rp_feed(1 * MS, 7, YIN_AXIS_POSITION, 1);
        CHECK(ysig_set(&g_rp, 1, YSIG_DETECT_LEVEL, 8));
        if (!v) { CHECK(ysig_arm(&g_rp, 1, 4 * MS)); rp_frame(4 * MS); }
        else { rp_frame(4 * MS); CHECK(ysig_arm(&g_rp, 1, 4 * MS)); }
        rp_frame(6 * MS);
        CHECK(lt_hits() == 1 && g_lt_hit[0] == (v ? 6 : 4) * MS && ft_bad() == 0);
    }
    /* a feed at 4 after the frame at 4 is late (a feed is first at one
     * time, L3): placed before the frame, whose pass then sees it, a hit
     * at 4 by a run again. The control: the feed before the frame */
    for (v = 0; v < 2; v++) {
        ft_open();
        rp_feed(1 * MS, 9, YIN_AXIS_POSITION, 1);
        CHECK(ysig_arm(&g_rp, 1, 1 * MS));
        if (!v) { rp_frame(4 * MS); rp_feed(4 * MS, 1, YIN_AXIS_POSITION, 1); }
        else { rp_feed(4 * MS, 1, YIN_AXIS_POSITION, 1); rp_frame(4 * MS); }
        ysig_get_stats(&g_rp, &st);
        CHECK(lt_hits() == 1 && g_lt_hit[0] == 4 * MS && ft_bad() == 0);
        CHECK(v ? st.arm_caught_up == 0 : 1);
    }
    /* R2: a group let in by ysig_disarm_at (X1) at tau 20; a plain disarm
     * made after it, at N (10), goes before the group's arm by key, into
     * the gap the arm closes: condition 4 counts the group. The control:
     * the plain disarm before the arm is in the arm's own group */
    for (v = 0; v < 2; v++) {
        ft_open();
        rp_feed(10 * MS, 9, YIN_AXIS_POSITION, 1);
        if (v) CHECK(ysig_disarm(&g_rp, 1));
        CHECK(ysig_arm(&g_rp, 1, 20 * MS) && g_rp.wt->n == 1);
        CHECK(ysig_disarm_at(&g_rp, 1, 30 * MS) && g_rp.wt->n == 0);
        if (!v) CHECK(ysig_disarm(&g_rp, 1));
        rp_feed(40 * MS, 9, YIN_AXIS_POSITION, 1);
        ysig_get_stats(&g_rp, &st);
        CHECK(st.arm_unverified == !v);
    }
}

/* The oracle's case 438 (the reviewer's trace), reduced: a present arm's
 * group (a set of a channel node's index, then an arm at N) made after a
 * ysig_disarm_at let in a group at a later tau. R holds that group and the
 * disarm_at with greater keys, so the present group is late: it goes to
 * its place. At R's end, a frame placed before the disarm_at by key ran
 * again before the set, and read the old channel (1, value 0). The
 * control: the set made before the waiting arm, in its gap */
static void frame2(int64_t o, float c0, float c1) {
    float v[2];
    ysig_frame_in f;
    v[0] = c0; v[1] = c1;
    memset(&f, 0, sizeof f);
    f.index = o / MS; f.onset_ns = o; f.poll_ns = o; f.has_poll = true; f.values = v; f.n_values = 2;
    ysig_frame(&g_rp, &f);
}
static void test_late_present_group(void) {
    int v;
    g_where = "a present group behind a let-in group (case 438)";
    for (v = 0; v < 2; v++) {
        ysig_node n[3];
        n[0] = ysig_node_of(YSIG_OP_CHANNEL, -1, -1); n[0].p[0] = 1; n[0].history = 64;
        n[1] = ysig_node_of(YSIG_OP_DETECT, 0, -1); n[1].p[YSIG_DETECT_LEVEL] = 5; n[1].history = 64;
        n[2] = ysig_node_of(YSIG_OP_DETECT, 0, -1); n[2].p[YSIG_DETECT_LEVEL] = 5; n[2].history = 64;
        if (!rp_open(n, 3, 4, 0, false, false)) return;
        frame2(10 * MS, 1, 0);
        if (v) CHECK(ysig_set(&g_rp, 0, 0, 0));
        CHECK(ysig_arm(&g_rp, 1, 20 * MS) && g_rp.wt->n == 1);
        CHECK(ysig_disarm_at(&g_rp, 1, 30 * MS) && g_rp.wt->n == 0);
        if (!v) CHECK(ysig_set(&g_rp, 0, 0, 0));
        CHECK(ysig_arm(&g_rp, 2, 10 * MS));
        frame2(25 * MS, 1, 0);
        CHECK(ysig_value_of(&g_rp, 0)->v[0] == 1 && ysig_value_of(&g_rp, 0)->t == 25 * MS);
    }
}

/* Hits handed out so far, by node d */
static int v2_hits(int d, int64_t* last) {
    ysig_hit h[16];
    int m, k, n = 0;
    while ((m = ysig_detected(&g_rp, h, 16)) > 0)
        for (k = 0; k < m; k++) if (h[k].node == d) { n++; if (last) *last = h[k].t; }
    return n;
}
/* The reviewer's 438 family at 5feeb5b (cases 379, 477, 557, 438, 462,
 * 690), reduced: ysig_disarm_at(d, 20) with N at 10 meets d's hold (a hit
 * at 15 of the arm at 10, handed out), then a plain ysig_disarm(d) at N. By key the plain
 * disarm (10) goes before the disarm_at (20): d ends at 10 and the hit at
 * 15 is not R's. A disarm takes effect at its gap's end, a gap is in call
 * order ("Groups"), and a read in an open gap sees the detect still armed:
 * the hit stands until an input ends the gap. Then the disarm goes to its
 * place, and the voided hit counts in arm_hits_superseded (a late state
 * call keeps the 0.6.x split; later than the disarm's time, of the detect
 * it disarms). The control: no plain disarm, nothing voided */
static void test_v2_plain_disarm(void) {
    ysig_stats st;
    int v;
    g_where = "a plain disarm before a disarm_at by key (the 438 family)";
    for (v = 0; v < 2; v++) {
        ysig_node n[3];
        int64_t t = 0;
        n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 1, 2); n[0].history = 64;
        n[1] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 2, 2); n[1].history = 64;
        n[2] = ysig_node_of(YSIG_OP_DETECT, 0, -1); n[2].p[YSIG_DETECT_LEVEL] = 5; n[2].p[YSIG_DETECT_HOLD] = 0.005;
        n[2].p[YSIG_DETECT_START] = 1; n[2].history = 64;
        if (!rp_open(n, 3, 4, 0, false, false)) return;
        rp_feed(10 * MS, 9, YIN_AXIS_POSITION, 1);
        CHECK(ysig_arm(&g_rp, 2, 10 * MS));
        CHECK(ysig_disarm_at(&g_rp, 2, 20 * MS));
        CHECK(v2_hits(2, &t) == 1 && t == 15 * MS);
        if (v) CHECK(ysig_disarm(&g_rp, 2));
        ysig_get_stats(&g_rp, &st);
        CHECK(st.arm_hits_superseded == 0);
        rp_feed(30 * MS, 0, YIN_AXIS_POSITION, 2);   /* ends the gap */
        ysig_get_stats(&g_rp, &st);
        CHECK(st.arm_hits_superseded == v && st.arm_hits_retracted == 0 && st.late_hits_voided == 0);
    }
}
/* Three shapes that predate X1 and R1, each with a control (v 0):
 * (a) a disarm_at with t later than N already in R, then a present group
 *     [set d's level, arm d at N]: by key the group goes first, so d is
 *     armed at 10 with level 1 and the disarm_at meets its hold (a hit at
 *     15). The control has no disarm_at: the hold waits for input.
 * (b) the W2 shape: a plain disarm keyed at N (30) above N(F(G)) (20, the
 *     second device's last report), then an arm of d at 25 made after it:
 *     by key the arm comes first, made (an entry-now hit at 25) and then
 *     ended at 30, so a later crossing does not hit. The control has no
 *     disarm: the crossing at 40 hits again.
 * (c) a state call outside a group keyed at N(F(c)) (30, a third device
 *     that reaches only F(c)) above a present group's tau (10), the two
 *     meeting at a sum: by key the group goes first, so the sum reads d's
 *     entry-now rise at 10 with the old offset. The control: the set in
 *     the group's gap. */
static void test_v2_old_shapes(void) {
    ysig_stats st;
    ysig_value r;
    int v, K;
    for (K = 4; K >= 0; K -= 4) {
        g_where = K ? "old shapes, (a) a disarm_at already in R" : "old shapes, (a) with no window";
        for (v = 0; v < 2; v++) {
            ysig_node n[2];
            int64_t t = 0;
            n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 1, 2); n[0].history = 64;
            n[1] = ysig_node_of(YSIG_OP_DETECT, 0, -1); n[1].p[YSIG_DETECT_LEVEL] = 5; n[1].p[YSIG_DETECT_HOLD] = 0.005;
            n[1].p[YSIG_DETECT_START] = 1; n[1].history = 64;
            if (!rp_open(n, 2, K, 0, false, false)) return;
            rp_feed(10 * MS, 3, YIN_AXIS_POSITION, 1);
            if (v) CHECK(ysig_disarm_at(&g_rp, 1, 30 * MS));
            CHECK(ysig_set(&g_rp, 1, YSIG_DETECT_LEVEL, 1));
            CHECK(ysig_arm(&g_rp, 1, 10 * MS));
            ysig_get_stats(&g_rp, &st);
            if (!K) CHECK(st.late_off == v && v2_hits(1, NULL) == 0);
            else CHECK(st.late_off == 0 && v2_hits(1, &t) == v && (!v || t == 15 * MS));
        }
    }
    g_where = "old shapes, (b) the W2 shape";
    for (v = 0; v < 2; v++) {
        ysig_node n[3];
        int64_t t = 0;
        n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 1, 2); n[0].history = 64;
        n[1] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 2, 2); n[1].history = 64;
        n[2] = ysig_node_of(YSIG_OP_DETECT, 1, -1); n[2].p[YSIG_DETECT_LEVEL] = 5; n[2].p[YSIG_DETECT_REPEAT] = 1;
        n[2].p[YSIG_DETECT_ENTRY] = YSIG_ENTRY_NOW; n[2].p[YSIG_DETECT_OUT] = YSIG_OUT_HIT; n[2].history = 64;
        if (!rp_open(n, 3, 4, 0, false, false)) return;
        rp_feed(10 * MS, 0, YIN_AXIS_POSITION, 1);
        rp_feed(10 * MS, 0, YIN_AXIS_POSITION, 2);
        rp_feed(20 * MS, 9, YIN_AXIS_POSITION, 2);
        rp_feed(30 * MS, 0, YIN_AXIS_POSITION, 1);
        if (v) CHECK(ysig_disarm(&g_rp, 2));
        rp_feed(31 * MS, 0, YIN_AXIS_POSITION, 1);   /* ends the disarm's gap */
        (void)v2_hits(2, NULL);   /* the reset's arm: a hit at 20 */
        CHECK(ysig_arm(&g_rp, 2, 25 * MS) && g_rp.wt->n == 1);
        rp_feed(32 * MS, 0, YIN_AXIS_POSITION, 2);
        CHECK(g_rp.wt->n == 0 && v2_hits(2, &t) == 1 && t == 25 * MS);
        rp_feed(40 * MS, 9, YIN_AXIS_POSITION, 2);
        CHECK(v2_hits(2, &t) == (v ? 0 : 1));
        ysig_get_stats(&g_rp, &st);
        CHECK(st.late_far == 0 && st.late_full == 0 && st.late_off == 0);
    }
    /* (c) G present, so the order by key alone decides. The sum (5) is
     * computed from its inputs' rings at the time asked (two sources), so
     * its value at tau does not show the order; the delay (7) after it
     * takes the arm's rise at tau as a sample, with the sum as R has it
     * there ("A change at an arm reaches every node (D)").
     * (c1) the set in no group keyed at N(F(c)) (30, device 3 reaches only
     *      F(c)) above tau (10): G goes first, the delay reads 2 at 10.
     *      The control: the set in G's gap, at tau, 102.
     * (c2) N(F(c)) (30) below tau (35) below N (40, device 5 reaches
     *      nothing of either): the set goes first, the delay reads 102 at
     *      35. The control: no set, 2.
     * (c3) the set after G in call order, keyed at N(F(c)) (10) below tau
     *      (35; device 4 makes G present through a sum, 9, that D(c) does
     *      not hold): a late call, it goes before G when its gap ends, and
     *      the delay reads 102 at 35. The control: the set before G's gap. */
    for (K = 0; K < 3; K++) {
        g_where = K == 0 ? "old shapes, (c1) N(F(c)) above a present tau" :
                  K == 1 ? "old shapes, (c2) N(F(c)) below a present tau below N" : "old shapes, (c3) a late set before a present group";
        for (v = 0; v < 2; v++) {
            ysig_node n[11];
            int64_t tau = K ? 35 * MS : 10 * MS;
            double want;
            n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 1, 1); n[0].history = 64;
            n[1] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 2, 1); n[1].history = 64;
            n[2] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 3, 1); n[2].history = 64;
            n[3] = ysig_node_of(YSIG_OP_OFFSET, 0, -1); n[3].history = 64;
            n[4] = ysig_node_of(YSIG_OP_DETECT, 1, -1); n[4].p[YSIG_DETECT_LEVEL] = 5;
            n[4].p[YSIG_DETECT_ENTRY] = YSIG_ENTRY_NOW; n[4].p[YSIG_DETECT_OUT] = YSIG_OUT_HIT; n[4].history = 64;
            n[5] = ysig_node_of(YSIG_OP_SUM, 3, 4); n[5].history = 64;
            n[6] = ysig_node_of(YSIG_OP_SUM, 3, 2); n[6].history = 64;
            n[7] = ysig_node_of(YSIG_OP_DELAY, 5, -1); n[7].history = 64;
            n[8] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 4, 1); n[8].history = 64;
            n[9] = ysig_node_of(YSIG_OP_SUM, 4, 8); n[9].history = 64;
            n[10] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 5, 1); n[10].history = 64;
            if (!rp_open(n, 11, 4, 0, false, false)) return;
            rp_feed(10 * MS, 1, YIN_AXIS_POSITION, 1);
            rp_feed(10 * MS, 9, YIN_AXIS_POSITION, 2);
            if (K == 0) {
                rp_feed(30 * MS, 0, YIN_AXIS_POSITION, 3);
                if (!v) CHECK(ysig_set(&g_rp, 3, 0, 100));
                rp_feed(31 * MS, 0, YIN_AXIS_POSITION, 3);   /* ends the set's gap: in no group */
                if (v) CHECK(ysig_set(&g_rp, 3, 0, 100));
                CHECK(ysig_arm(&g_rp, 4, tau));
                CHECK(g_rp.wt->nfg == (v ? 31 * MS : tau));   /* the control's group is past */
                want = v ? 102 : 2;
            } else if (K == 1) {
                rp_feed(30 * MS, 1, YIN_AXIS_POSITION, 1);
                rp_feed(40 * MS, 0, YIN_AXIS_POSITION, 5);
                if (v) CHECK(ysig_set(&g_rp, 3, 0, 100));
                rp_feed(41 * MS, 0, YIN_AXIS_POSITION, 5);   /* ends the set's gap */
                rp_feed(35 * MS, 9, YIN_AXIS_POSITION, 2);
                CHECK(ysig_arm(&g_rp, 4, tau) && g_rp.wt->nfg == tau);
                want = v ? 102 : 2;
            } else {
                rp_feed(35 * MS, 0, YIN_AXIS_POSITION, 4);
                if (!v) { CHECK(ysig_set(&g_rp, 3, 0, 100)); rp_feed(36 * MS, 0, YIN_AXIS_POSITION, 3); }
                CHECK(ysig_arm(&g_rp, 4, tau) && g_rp.wt->nfg == tau);
                if (v) CHECK(ysig_set(&g_rp, 3, 0, 100));
                rp_feed(41 * MS, 0, YIN_AXIS_POSITION, 3);   /* ends the set's gap: in no group */
                want = 102;
            }
            CHECK(ysig_value_at(&g_rp, 5, tau, &r) && r.v[0] == want);
            CHECK(ysig_value_at(&g_rp, 7, tau, &r) && r.v[0] == want && r.t == tau);
            CHECK(ysig_value_of(&g_rp, 5)->v[0] == (K == 1 && !v ? 2 : 102));
            ysig_get_stats(&g_rp, &st);
            CHECK(st.late_far == 0 && st.late_full == 0 && st.late_off == 0);
        }
    }
}

/* S1' (2026-11-15): a state call of a present or ahead group lets in at
 * its call's N(F(c)); a past group's calls at its tau; a state call in no
 * group at N(F(c)); a plain disarm in no group at N. Nodes: device 1
 * reaches e (3), device 3 reaches d (2, entry now, a hit output). X (5),
 * an offset of device 5 (never fed), feeds a sum with device 1 (6) and
 * one with d (7): D(X) meets D(d) and device 1's footprint, which does
 * not meet D(d). d's arm at 19 waits (device 3 last at 10). Each shape
 * has a control (v 0). */
static int s1_open(int K) {
    ysig_node n[8];
    n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 1, 1); n[0].history = 64;
    n[1] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 3, 1); n[1].history = 64;
    n[2] = ysig_node_of(YSIG_OP_DETECT, 1, -1); n[2].p[YSIG_DETECT_LEVEL] = 5;
    n[2].p[YSIG_DETECT_ENTRY] = YSIG_ENTRY_NOW; n[2].p[YSIG_DETECT_OUT] = YSIG_OUT_HIT; n[2].history = 64;
    n[3] = ysig_node_of(YSIG_OP_DETECT, 0, -1); n[3].p[YSIG_DETECT_LEVEL] = 5; n[3].history = 64;
    n[4] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 5, 1); n[4].history = 64;
    n[5] = ysig_node_of(YSIG_OP_OFFSET, 4, -1); n[5].history = 64;
    n[6] = ysig_node_of(YSIG_OP_SUM, 5, 0); n[6].history = 64;
    n[7] = ysig_node_of(YSIG_OP_SUM, 5, 2); n[7].history = 64;
    if (!rp_open(n, 8, K, 0, false, false)) return 0;
    rp_feed(10 * MS, 0, YIN_AXIS_POSITION, 1);
    rp_feed(10 * MS, 9, YIN_AXIS_POSITION, 3);
    (void)v2_hits(2, NULL);   /* the reset's arm */
    CHECK(ysig_arm(&g_rp, 2, 19 * MS) && g_rp.wt->n == 1);
    return 1;
}
/* S1' (2026-11-15): a present group's state call lets in at its call's
 * N(F(c)), as before S1. The oracle's case 334 (on 20261121), reduced: a
 * present group [set d's level, arm e at 24], present by e's device. The
 * set lets in at 10: d waits. d's device's report at 26 lets it in; by key
 * it goes before the set and tests the old level: a hit at 19. With a
 * window it is placed there; with none it is made after the set, a late
 * call (late_off), no hit. The control: no set.
 * The deciding case (the reviewer): then d's device reports 15 with a
 * value that makes d's test false. Under S1 the set let d in at 24 and
 * its hit at 19 was handed out; the report at 15 voided it
 * (late_hits_voided, a guard stop). At N(F(c)) d waits until its
 * device's input is complete: nothing handed out, nothing voided, no hit.
 * Control: the report at 15 keeps the test true, the hit at 19 (window). */
static void test_v2_present_key(void) {
    ysig_stats st;
    int v, K;
    for (K = 4; K >= 0; K -= 4) {
        g_where = K ? "S1', a present group (case 334)" : "S1', a present group, no window";
        for (v = 0; v < 2; v++) {
            int64_t t = 0;
            if (!s1_open(K)) return;
            rp_feed(24 * MS, 0, YIN_AXIS_POSITION, 1);
            if (v) CHECK(ysig_set(&g_rp, 2, YSIG_DETECT_LEVEL, 20));
            CHECK(ysig_arm(&g_rp, 3, 24 * MS) && g_rp.wt->n == 1);
            ysig_get_stats(&g_rp, &st);
            CHECK(st.late_off == 0 && st.late_far == 0 && st.late_full == 0);
            CHECK(v2_hits(2, NULL) == 0);
            rp_feed(26 * MS, 9, YIN_AXIS_POSITION, 3);
            ysig_get_stats(&g_rp, &st);
            CHECK(g_rp.wt->n == 0 && st.late_off == (K ? 0 : v) && st.late_far == 0 && st.late_full == 0);
            CHECK(v2_hits(2, &t) == (!K && v ? 0 : 1) && (!K && v ? 1 : t == 19 * MS));
        }
        g_where = K ? "S1', the deciding case" : "S1', the deciding case, no window";
        for (v = 0; v < 2; v++) {
            int64_t t = 0;
            if (!s1_open(K)) return;
            rp_feed(24 * MS, 0, YIN_AXIS_POSITION, 1);
            CHECK(ysig_set(&g_rp, 2, YSIG_DETECT_LEVEL, 20));
            CHECK(ysig_arm(&g_rp, 3, 24 * MS) && g_rp.wt->n == 1);
            CHECK(v2_hits(2, NULL) == 0);
            rp_feed(15 * MS, v ? 1 : 9, YIN_AXIS_POSITION, 3);
            CHECK(g_rp.wt->n == 1 && v2_hits(2, NULL) == 0);
            rp_feed(26 * MS, 9, YIN_AXIS_POSITION, 3);
            ysig_get_stats(&g_rp, &st);
            CHECK(g_rp.wt->n == 0 && st.late_hits_voided == 0 && st.arm_hits_superseded == 0 && st.arm_hits_retracted == 0);
            CHECK(st.late_far == 0 && st.late_full == 0);
            CHECK(v2_hits(2, &t) == (K && !v) && (K && !v ? t == 19 * MS : 1));
        }
    }
}
/* S1', the other shapes:
 * (a) a past group [set d's level, arm e at 24], past by device 1 at 30:
 *     the set lets in at 24 (a past group's calls keep tau), not at its
 *     N(F(c)) (10): d goes in right before it, a hit at 19 with the old
 *     level. Control: no set.
 * (b) an ahead group [set d's level, arm e at 40]: the set lets in at
 *     its N(F(c)) (10, nothing); when its group goes in (device 1 at 45),
 *     T1's closure lets d in before it (they meet, d's key is smaller): a
 *     hit at 19. Control: no set, d is not in the group's footprint and
 *     waits.
 * (c) a set in no group lets in at N(F(c)): a set of d's level reaches
 *     only device 3's nodes (10), so d waits; a set of X (v 0) reaches
 *     device 1's too (30), and lets d in.
 * (d) a plain disarm in no group lets in at N (30): d is made, an
 *     entry-now hit at 19, then ended. At N(F(c)), 10, it would cancel
 *     d. Control: N is 10 too, and the disarm cancels d's arm. */
static void test_v2_s1(void) {
    ysig_stats st;
    int v;
    g_where = "S1', a past group";
    for (v = 0; v < 2; v++) {
        int64_t t = 0;
        if (!s1_open(4)) return;
        rp_feed(30 * MS, 0, YIN_AXIS_POSITION, 1);
        if (v) CHECK(ysig_set(&g_rp, 2, YSIG_DETECT_LEVEL, 20));
        CHECK(g_rp.wt->n == 1);
        CHECK(ysig_arm(&g_rp, 3, 24 * MS) && g_rp.wt->n == !v);
        CHECK(v2_hits(2, &t) == v && (v ? t == 19 * MS : 1));
        ysig_get_stats(&g_rp, &st);
        CHECK(st.late_far == 0 && st.late_full == 0 && st.arm_past_far == 0 && st.arm_past_full == 0);
    }
    g_where = "S1', an ahead group";
    for (v = 0; v < 2; v++) {
        int64_t t = 0;
        if (!s1_open(4)) return;
        rp_feed(24 * MS, 0, YIN_AXIS_POSITION, 1);
        if (v) CHECK(ysig_set(&g_rp, 2, YSIG_DETECT_LEVEL, 20));
        CHECK(ysig_arm(&g_rp, 3, 40 * MS) && g_rp.wt->n == 2);
        rp_feed(45 * MS, 0, YIN_AXIS_POSITION, 1);
        CHECK(g_rp.wt->n == !v);
        CHECK(v2_hits(2, &t) == v && (v ? t == 19 * MS : 1));
        ysig_get_stats(&g_rp, &st);
        CHECK(st.late_far == 0 && st.late_full == 0);
    }
    g_where = "S1', a set in no group";
    for (v = 0; v < 2; v++) {
        if (!s1_open(4)) return;
        rp_feed(30 * MS, 0, YIN_AXIS_POSITION, 1);
        if (v) CHECK(ysig_set(&g_rp, 2, YSIG_DETECT_LEVEL, 20));
        else CHECK(ysig_set(&g_rp, 5, YSIG_OFFSET_D_X, 1));
        CHECK(g_rp.wt->n == v);
        rp_feed(31 * MS, 0, YIN_AXIS_POSITION, 1);   /* ends the gap: in no group */
        CHECK(g_rp.wt->n == v);
    }
    g_where = "S1', a plain disarm in no group";
    for (v = 0; v < 2; v++) {
        int64_t t = 0;
        if (!s1_open(4)) return;
        if (v) rp_feed(30 * MS, 0, YIN_AXIS_POSITION, 1);
        CHECK(ysig_disarm(&g_rp, 2) && g_rp.wt->n == !v);
        rp_feed(31 * MS, 0, YIN_AXIS_POSITION, 1);   /* ends the gap */
        ysig_get_stats(&g_rp, &st);
        CHECK(st.arm_ahead_cancelled == !v && (!v || g_rp.wt->n == 0));
        CHECK(v2_hits(2, &t) == v && (v ? t == 19 * MS : 1));
        rp_feed(40 * MS, 9, YIN_AXIS_POSITION, 3);
        CHECK(v2_hits(2, NULL) == 0);
    }
}
/* The oracle's case 971 (OFF=l 20261122), reduced: an ahead group W
 * [set X's offset to -1, arm d at tw], X an offset of device 3 and d a
 * detect of X, waits; ysig_disarm_at(d, 60) cancels W's arm by key, so W
 * waits with its set alone. A present group G [set X to 0.25, arm e at
 * 74] follows, present by device 2. In R, W's set (key tw) goes before
 * G's (74): after device 3's report at 75, X is 0.25 plus the report's
 * 0.5. Device 3's report lets W in; by key it is a late call. The header
 * placed a let-in group by key only when its arm was live, so W's set
 * was made after G's and X read -1 plus 0.5. v: tw 72 (S1: G's set lets
 * W in at its arm, the oracle's case) or 74 (the same tau, made before
 * G: S1's strict let-in leaves it waiting for the report). The control
 * (c 0): no set in G, X -1 plus 0.5 */
static void test_v2_lost_set(void) {
    int v, c;
    g_where = "a cancelled group's set placed by key (case 971)";
    for (v = 0; v < 2; v++)
        for (c = 0; c < 2; c++) {
            ysig_node n[5];
            ysig_stats st;
            n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 2, 1); n[0].history = 64;
            n[1] = src_node(YIN_KIND_PEN, YIN_AXIS_PEN_PRESSURE, 3, 1); n[1].history = 64;
            n[2] = ysig_node_of(YSIG_OP_OFFSET, 1, -1); n[2].history = 64;
            n[3] = ysig_node_of(YSIG_OP_DETECT, 2, -1); n[3].p[YSIG_DETECT_LEVEL] = 5; n[3].history = 64;
            n[4] = ysig_node_of(YSIG_OP_DETECT, 0, -1); n[4].p[YSIG_DETECT_LEVEL] = 5; n[4].history = 64;
            if (!rp_open(n, 5, 4, 0, false, false)) return;
            rp_feed(10 * MS, 0, YIN_AXIS_POSITION, 2);
            rp_feed(10 * MS, 0, YIN_AXIS_PEN_PRESSURE, 3);
            rp_feed(66 * MS, 0, YIN_AXIS_POSITION, 2);
            CHECK(ysig_set(&g_rp, 2, YSIG_OFFSET_D_X, -1));
            CHECK(ysig_arm(&g_rp, 3, (v ? 74 : 72) * MS) && g_rp.wt->n == 1);
            CHECK(ysig_disarm_at(&g_rp, 3, 60 * MS));
            rp_feed(74 * MS, 0, YIN_AXIS_POSITION, 2);
            if (c) CHECK(ysig_set(&g_rp, 2, YSIG_OFFSET_D_X, 0.25));
            CHECK(ysig_arm(&g_rp, 4, 74 * MS));
            rp_feed(75 * MS, 0.5, YIN_AXIS_PEN_PRESSURE, 3);
            ysig_get_stats(&g_rp, &st);
            CHECK(g_rp.wt->n == 0 && st.arm_ahead_cancelled == 1);
            CHECK(ysig_value_of(&g_rp, 2)->v[0] == (c ? 0.75 : -0.5) && ysig_value_of(&g_rp, 2)->t == 75 * MS);
            CHECK(st.late_far == 0 && st.late_full == 0 && st.late_off == 0);
        }
}
/* The player harness, 60 Hz seed 102788 (10 ms late), reduced: a settle
 * at 1016 makes the arm of r0 at tau 1001 a past group G that waits at
 * its place (N(G) 990), L its settle. The trial's end, ysig_disarm_at(r0,
 * 1001), is late (before the settle by key) and goes right after G; by R2
 * it lets G in and is G's L. A late report at tau + 9 ms then goes after
 * it, outside G's span: no 2 (b) retest, and the disarm_at is 2 (b)'s
 * exception anyway. The header kept the settle as G's L and counted
 * late_unverified (a state call between G and the report). R's hit: G's
 * entry-now hit at 1001. The control (v 0): the report at tau - 1 ms goes
 * before G by key, so G's entry reads 0 (no hit), and nothing is counted:
 * a late report goes to its place, condition 3 is for one placed after G */
static void test_v2_exit_late(void) {
    int v;
    g_where = "a late report after a disarm_at that is G's L (102788)";
    for (v = 0; v < 2; v++) {
        ysig_node n[2];
        ysig_stats st;
        int64_t t = 0;
        n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 1, 1); n[0].history = 64;
        n[1] = ysig_node_of(YSIG_OP_DETECT, 0, -1); n[1].p[YSIG_DETECT_LEVEL] = 5;
        n[1].p[YSIG_DETECT_ENTRY] = YSIG_ENTRY_NOW; n[1].history = 64;
        if (!rp_open(n, 2, 4, 0, false, false)) return;
        rp_feed(990 * MS, 9, YIN_AXIS_POSITION, 1);
        (void)v2_hits(1, NULL);   /* the reset's arm */
        CHECK(ysig_settle(&g_rp, 1016 * MS) >= 0);
        CHECK(ysig_arm(&g_rp, 1, 1001 * MS) && g_rp.wt->n == 0);
        CHECK(ysig_disarm_at(&g_rp, 1, 1001 * MS));
        rp_feed((v ? 1010 : 1000) * MS, 0, YIN_AXIS_POSITION, 1);
        CHECK(ysig_settle(&g_rp, 1017 * MS) >= 0);
        ysig_get_stats(&g_rp, &st);
        CHECK(st.late_unverified == 0 && st.arm_unverified == 0 && st.late_far == 0 && st.late_full == 0);
        CHECK(v2_hits(1, &t) == v && (v ? t == 1001 * MS : 1));
    }
}
/* The player harness, 60 Hz seed 102401 (0, 5 and 10 ms late), reduced:
 * G = [arm d at 1001] waits (N(G) 990) and a settle at 1016 lets it in, G's
 * L. The state's next until arms d again at 1001: H, a past group that
 * waits at its place with the settle as its L too, 2 (b)'s exception (no
 * count). The state's end, ysig_disarm_at(d, 1001), is late (before the
 * settle by key) and goes right after H. By R2 it is G's L now, and X1
 * makes it H's L too (it lets in every waiting group at or before its t
 * that meets d), so H is still the exception, G's arm before H's. The 2 (b)
 * retest with the disarm_at as L counted H's arm (late_unverified). The
 * control (v 1): H = [set d's level, arm d at 1001] has a set, so it is not
 * the exception, and G is counted at H's place (arm_unverified) */
static void test_v2_two_untils(void) {
    int v;
    g_where = "a late disarm_at after a second arm at tau (102401)";
    for (v = 0; v < 2; v++) {
        ysig_node n[2];
        ysig_stats st;
        int64_t t = 0;
        n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 1, 1); n[0].history = 64;
        n[1] = ysig_node_of(YSIG_OP_DETECT, 0, -1); n[1].p[YSIG_DETECT_LEVEL] = 5;
        n[1].p[YSIG_DETECT_ENTRY] = YSIG_ENTRY_NOW; n[1].history = 64;
        if (!rp_open(n, 2, 4, 0, false, false)) return;
        rp_feed(990 * MS, 9, YIN_AXIS_POSITION, 1);
        (void)v2_hits(1, NULL);
        CHECK(ysig_arm(&g_rp, 1, 1001 * MS) && g_rp.wt->n == 1);
        CHECK(ysig_settle(&g_rp, 1016 * MS) >= 0 && g_rp.wt->n == 0);
        CHECK(v2_hits(1, &t) == 1 && t == 1001 * MS);
        if (v) CHECK(ysig_set(&g_rp, 1, YSIG_DETECT_LEVEL, 5));
        CHECK(ysig_arm(&g_rp, 1, 1001 * MS));
        CHECK(ysig_disarm_at(&g_rp, 1, 1001 * MS));
        ysig_get_stats(&g_rp, &st);
        CHECK(st.late_unverified == 0 && st.arm_unverified == v && st.late_far == 0 && st.late_full == 0);
        CHECK(st.late_hits_voided == 0 && st.arm_hits_retracted == 0);
    }
}
static void test_late(void) {
    ysig_stats st;
    int v, k;
    g_where = "late input";
    /* a late report goes to its place: the crossing moves from 15 to 13 ms
     * (the coordinator's ruling of 2026-11-15: the voided 15 ms hit counts
     * in late_hits_voided, a guard stop; the oracle's case 763 of seed
     * 20261115, reduced). The control: a late report that changes no hit
     * handed out counts nothing */
    for (v = 0; v < 2; v++) {
        lt_det_open(0, 4);
        rp_feed(1 * MS, 0, YIN_AXIS_POSITION, 1);
        ysig_arm(&g_rp, 1, 1 * MS);
        rp_feed(10 * MS, 0, YIN_AXIS_POSITION, 1);
        rp_feed(15 * MS, 9, YIN_AXIS_POSITION, 1);
        CHECK(lt_hits() == 1 && g_lt_hit[0] == 15 * MS);
        rp_feed(13 * MS, v ? 0 : 9, YIN_AXIS_POSITION, 2);   /* device 2, late */
        ysig_get_stats(&g_rp, &st);
        if (!v) CHECK(lt_hits() == 1 && g_lt_hit[0] == 13 * MS && st.late_hits_voided == 1);
        else CHECK(lt_hits() == 0 && st.late_hits_voided == 0);
        CHECK(st.arm_hits_superseded == 0 && st.arm_hits_retracted == 0 && st.late_far == 0 && st.late_unverified == 0);
    }
    /* ysig_disarm_at() at a state's exit time, then a late report before
     * that time (the player's path at a state's end): the report goes
     * before the disarm (key 9 < 12), breaks the hold, and voids the hit
     * at 10 that the disarm met (late_hits_voided). Not counted
     * unverified: a disarm with a time is not deferred to its gap's end
     * (S1). The control: a late report after the exit time (13 ms) is not
     * late for the disarm: the hit stands */
    for (v = 0; v < 2; v++) {
        lt_det_open(0.01, 4);
        rp_feed(0, 9, YIN_AXIS_POSITION, 1);
        ysig_arm(&g_rp, 1, 0);
        rp_feed(4 * MS, 9, YIN_AXIS_POSITION, 1);
        rp_feed(8 * MS, 9, YIN_AXIS_POSITION, 1);
        CHECK(ysig_disarm_at(&g_rp, 1, 12 * MS));
        CHECK(lt_hits() == 1 && g_lt_hit[0] == 10 * MS);
        rp_feed((v ? 13 : 9) * MS, 0, YIN_AXIS_POSITION, 2);
        ysig_get_stats(&g_rp, &st);
        CHECK(lt_hits() == 0 && st.late_hits_voided == (v ? 0 : 1) && st.late_unverified == 0);
    }
    /* the same with a plain ysig_disarm() (at N, 8 ms) and a late report at
     * 8: in R the disarm's gap is open again after the report (S1), where
     * the header made it at the report's call: late_unverified (the
     * oracle's case 411 shape). The control: a report at 9, after the
     * disarm's key, is not late for it */
    for (v = 0; v < 2; v++) {
        lt_det_open(0.01, 4);
        rp_feed(0, 0, YIN_AXIS_POSITION, 1);
        ysig_arm(&g_rp, 1, 0);
        rp_feed(8 * MS, 0, YIN_AXIS_POSITION, 1);
        CHECK(ysig_disarm(&g_rp, 1));
        rp_feed((v ? 9 : 8) * MS, 0, YIN_AXIS_POSITION, 2);
        ysig_get_stats(&g_rp, &st);
        CHECK(st.late_unverified == !v);
    }
    /* a report before the last barrier is dropped (late_barrier), not fed */
    lt_det_open(0, 4);
    rp_feed(10 * MS, 1, YIN_AXIS_POSITION, 1);
    ysig_reset_trial(&g_rp, 20 * MS);
    ysig_get_stats(&g_rp, &st);
    k = (int)st.fed;
    rp_feed(15 * MS, 7, YIN_AXIS_POSITION, 2);
    ysig_get_stats(&g_rp, &st);
    CHECK(st.late_barrier == 1 && st.fed == k && ysig_value_of(&g_rp, 0)->v[0] != 7);
    /* with no window: made at R's end, counted late_off */
    lt_det_open(0, 0);
    rp_feed(10 * MS, 1, YIN_AXIS_POSITION, 1);
    rp_feed(5 * MS, 2, YIN_AXIS_POSITION, 2);
    ysig_get_stats(&g_rp, &st);
    CHECK(st.late_off == 1 && ysig_value_of(&g_rp, 0)->v[0] == 2);
    /* with a window: R's value, the newest by time */
    lt_det_open(0, 4);
    rp_feed(10 * MS, 1, YIN_AXIS_POSITION, 1);
    rp_feed(5 * MS, 2, YIN_AXIS_POSITION, 2);
    ysig_get_stats(&g_rp, &st);
    CHECK(st.late_off == 0 && ysig_value_of(&g_rp, 0)->v[0] == 1 && ysig_value_of(&g_rp, 0)->t == 10 * MS);
    /* a late report before a frame: the frame's bound channel as R gives it
     * differs from what was drawn: late_frames 1, once. The control: a
     * report after the frame's onset is not late for it */
    for (v = 0; v < 2; v++) {
        ysig_node n[2];
        n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 0, 2); n[0].history = 64;
        n[1] = ysig_node_of(YSIG_OP_BIND, 0, -1); n[1].p[0] = 0;
        if (!rp_open(n, 2, 4, 0, false, false)) return;
        rp_feed(10 * MS, 1, YIN_AXIS_POSITION, 1);
        rp_frame(16 * MS);
        CHECK(g_ch[0] == 1);
        rp_feed((v ? 17 : 15) * MS, 5, YIN_AXIS_POSITION, 2);
        rp_frame(32 * MS);
        ysig_get_stats(&g_rp, &st);
        CHECK(st.late_frames == !v && g_ch[0] == 5);
    }
    /* catchup_ns: images catchup_ns / 8 apart in input time, the oldest
     * gone once the next oldest is catchup_ns behind: at 500 Hz (2 ms
     * frames) with 8 images and 16 ms, the window keeps about 16 ms, and a
     * report 30 ms late falls back (late_far); one 10 ms late is placed */
    for (v = 0; v < 2; v++) {
        ysig_node n[2];
        ysig_desc d;
        n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 0, 2); n[0].history = 64;
        n[1] = ysig_node_of(YSIG_OP_BIND, 0, -1); n[1].p[0] = 0;
        memset(&d, 0, sizeof d);
        d.nodes = n; d.n_nodes = 2; d.arena = g_rp_ar; d.catchup_frames = 8; d.catchup_ns = 16 * MS;
        d.channels = g_ch; d.n_channels = 64;
        d.arena_size = ysig_arena_size(&d);
        if (!d.arena_size || d.arena_size > sizeof g_rp_ar || !ysig_open(&g_rp, &d)) { g_failures++; return; }
        ysig_reset(&g_rp, 0);
        for (k = 1; k <= 50; k++) { rp_feed(k * 2 * MS - MS, k, YIN_AXIS_POSITION, 1); rp_frame(k * 2 * MS); }
        CHECK(g_rp.cu->n_ck >= 2 && g_rp.cu->n_ck <= 8);
        CHECK(g_rp.cu->ck[(g_rp.cu->ck0 + g_rp.cu->n_ck - 1) % g_rp.cu->K].cover - g_rp.cu->ck[(g_rp.cu->ck0 + 1) % g_rp.cu->K].cover < 16 * MS);
        rp_feed((v ? 90 : 70) * MS, 0, YIN_AXIS_POSITION, 2);
        ysig_get_stats(&g_rp, &st);
        CHECK(st.late_far == !v);
    }
    /* ysig_feed_n(): the same results as one ysig_feed() each (L7) */
    {
        static const int dev[8] = { 1, 1, 2, 1, 2, 2, 1, 2 };
        static const int ms[8] = { 3, 9, 5, 12, 8, 14, 15, 11 };
        yin_event ev[8];
        ysig_value a[3];
        ysig_stats sa, sb;
        for (k = 0; k < 8; k++) {
            memset(&ev[k], 0, sizeof ev[k]);
            ev[k].t = ms[k] * MS; ev[k].type = YIN_SAMPLE; ev[k].kind = YIN_KIND_PEN; ev[k].control = YIN_AXIS_POSITION;
            ev[k].device = (uint32_t)dev[k]; ev[k].x = (float)(k * 2); ev[k].stamp = YIN_TIER_2;
        }
        for (v = 0; v < 2; v++) {
            lt_det_open(0.002, 4);
            ysig_arm(&g_rp, 1, 0);
            if (v) CHECK(ysig_feed_n(&g_rp, ev, 8) == 7);   /* device 2's 11 after its 14: dropped */
            else for (k = 0; k < 8; k++) ysig_feed(&g_rp, &ev[k]);
            rp_frame(16 * MS);
            if (!v) { a[0] = *ysig_value_of(&g_rp, 0); a[1] = *ysig_value_of(&g_rp, 1); ysig_get_stats(&g_rp, &sa); k = lt_hits(); a[2].t = k ? g_lt_hit[0] : -1; }
            else {
                int m = lt_hits();
                ysig_get_stats(&g_rp, &sb);
                CHECK(!memcmp(&a[0], ysig_value_of(&g_rp, 0), sizeof a[0]) && !memcmp(&a[1], ysig_value_of(&g_rp, 1), sizeof a[1]));
                CHECK(m == k && (m == 0 || g_lt_hit[0] == a[2].t) && sa.hits == sb.hits && sa.taken == sb.taken);
            }
        }
    }
}

/* v0.7.0: ysig_detected_to(), ysig_derived_to() (docs/sig.md, "Holding
 * hits and derived events"): one case per sentence, each with a control */
static int hd_to(int64_t t, int cap) {
    ysig_hit h[8];
    int m = ysig_detected_to(&g_rp, h, cap < 8 ? cap : 8, t), k;
    for (k = 0; k < m; k++) g_lt_hit[k] = h[k].t;
    return m;
}
static void hd_der_open(int K, int calls) {
    ysig_node n[3];
    n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 0, 2); n[0].history = 64;   /* any device */
    n[1] = ysig_node_of(YSIG_OP_INTEGRATE, 0, -1);
    n[2] = ysig_node_of(YSIG_OP_EMIT, 1, -1);
    (void)rp_open(n, 3, K, calls, false, false);
}
static void test_hold(void) {
    ysig_stats st;
    yin_event ev[8];
    int v, k, m;
    g_where = "holding hits and events";
    /* "hands out, in R's order among them, at most cap queued hits whose
     * time is at or before t ... The queued hits later than t stay queued
     * and are not handed out." Control: ysig_detected() hands out all */
    for (v = 0; v < 2; v++) {
        lt_det_open(0, 4);
        rp_feed(1 * MS, 0, YIN_AXIS_POSITION, 1);
        ysig_arm(&g_rp, 1, 1 * MS);
        for (k = 1; k <= 3; k++) { rp_feed(k * 5 * MS, 9, YIN_AXIS_POSITION, 1); rp_feed(k * 5 * MS + MS, 0, YIN_AXIS_POSITION, 1); }
        if (!v) {
            CHECK(hd_to(10 * MS, 8) == 2 && g_lt_hit[0] == 5 * MS && g_lt_hit[1] == 10 * MS);
            CHECK(hd_to(14 * MS, 8) == 0 && g_rp.n_hit == 1);
            CHECK(hd_to(INT64_MAX, 8) == 1 && g_lt_hit[0] == 15 * MS);
        } else {
            CHECK(lt_hits() == 3 && g_lt_hit[0] == 5 * MS && g_lt_hit[2] == 15 * MS);
        }
        /* at most cap */
        lt_det_open(0, 4);
        rp_feed(1 * MS, 0, YIN_AXIS_POSITION, 1);
        ysig_arm(&g_rp, 1, 1 * MS);
        for (k = 1; k <= 3; k++) { rp_feed(k * 5 * MS, 9, YIN_AXIS_POSITION, 1); rp_feed(k * 5 * MS + MS, 0, YIN_AXIS_POSITION, 1); }
        m = hd_to(v ? INT64_MAX : 10 * MS, 1);
        CHECK(m == 1 && g_lt_hit[0] == 5 * MS && g_rp.n_hit == 2);
    }
    /* "a run again that no longer gives a queued hit removes it, with no
     * count": the hit at 15 is held (not handed out) and a late report
     * moves the crossing to 13. Control: handed out, it counts in
     * late_hits_voided */
    for (v = 0; v < 2; v++) {
        lt_det_open(0, 4);
        rp_feed(1 * MS, 0, YIN_AXIS_POSITION, 1);
        ysig_arm(&g_rp, 1, 1 * MS);
        rp_feed(10 * MS, 0, YIN_AXIS_POSITION, 1);
        rp_feed(15 * MS, 9, YIN_AXIS_POSITION, 1);
        CHECK(hd_to(v ? 15 * MS : 12 * MS, 8) == v);
        rp_feed(13 * MS, 9, YIN_AXIS_POSITION, 2);   /* device 2, late */
        ysig_get_stats(&g_rp, &st);
        CHECK(lt_hits() == 1 && g_lt_hit[0] == 13 * MS && st.late_hits_voided == v);
        CHECK(st.arm_hits_superseded == 0 && st.arm_hits_retracted == 0 && st.late_unverified == 0);
    }
    /* "a run again that gives it again leaves it queued once". Control:
     * handed out, it is not queued again, and nothing counts */
    for (v = 0; v < 2; v++) {
        lt_det_open(0, 4);
        rp_feed(1 * MS, 0, YIN_AXIS_POSITION, 1);
        ysig_arm(&g_rp, 1, 1 * MS);
        rp_feed(10 * MS, 0, YIN_AXIS_POSITION, 1);
        rp_feed(15 * MS, 9, YIN_AXIS_POSITION, 1);
        CHECK(hd_to(v ? 15 * MS : 12 * MS, 8) == v);
        rp_feed(13 * MS, 0, YIN_AXIS_POSITION, 2);   /* late, changes no hit */
        ysig_get_stats(&g_rp, &st);
        CHECK(st.late_hits_voided == 0 && st.arm_hits_retracted == 0 && st.arm_caught_up == 0);
        m = lt_hits();
        CHECK(m == !v && (v || g_lt_hit[0] == 15 * MS));
    }
    /* "The queue holds YSIG_QUEUE_CAP plus catchup_calls hits; a hit that
     * finds it full is counted in hits_lost." Control: with no window, 64 */
    for (v = 0; v < 2; v++) {
        ysig_node n[2];
        n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 0, 2); n[0].history = 64;
        n[1] = ysig_node_of(YSIG_OP_DETECT, 0, -1); n[1].p[YSIG_DETECT_LANE] = 0; n[1].p[YSIG_DETECT_LEVEL] = 5;
        n[1].p[YSIG_DETECT_REPEAT] = 1; n[1].history = 64;
        if (!rp_open(n, 2, v ? 0 : 4, 100, false, false)) return;
        ysig_arm(&g_rp, 1, 0);
        for (k = 0; k < 200; k++) { rp_feed((k * 2 + 1) * MS, 9, YIN_AXIS_POSITION, 1); rp_feed((k * 2 + 2) * MS, 0, YIN_AXIS_POSITION, 1); }
        ysig_get_stats(&g_rp, &st);
        CHECK(st.hits == 200 && g_rp.n_hit == (v ? 64 : 164) && st.hits_lost == (v ? 136 : 36));
    }
    /* "A reset or a trial reset empties the queue, held hits too."
     * Control: no reset, the held hit stays */
    for (v = 0; v < 3; v++) {
        lt_det_open(0, 4);
        rp_feed(1 * MS, 0, YIN_AXIS_POSITION, 1);
        ysig_arm(&g_rp, 1, 1 * MS);
        rp_feed(15 * MS, 9, YIN_AXIS_POSITION, 1);
        CHECK(hd_to(12 * MS, 8) == 0);
        if (v == 1) ysig_reset_trial(&g_rp, 20 * MS);
        if (v == 2) ysig_reset(&g_rp, 20 * MS);
        CHECK(lt_hits() == (v == 0));
    }
    /* derived events: the same sentences. The integral of the samples of
     * any device (a step hold: x times the time to the next sample),
     * emitted at each sample */
    /* "hands out ... at most cap queued events whose time is at or before
     * t ...; later ones stay queued and are not handed out". Control:
     * ysig_derived() hands out all */
    for (v = 0; v < 2; v++) {
        hd_der_open(4, 0);
        rp_feed(10 * MS, 1, YIN_AXIS_POSITION, 1);
        rp_feed(15 * MS, 1, YIN_AXIS_POSITION, 1);
        if (!v) {
            CHECK(ysig_derived_to(&g_rp, ev, 8, 12 * MS) == 1 && ev[0].t == 10 * MS && g_rp.n_der == 1);
            CHECK(ysig_derived_to(&g_rp, ev, 8, 14 * MS) == 0);
            CHECK(ysig_derived_to(&g_rp, ev, 8, INT64_MAX) == 1 && ev[0].t == 15 * MS);
        } else {
            CHECK(ysig_derived(&g_rp, ev, 8) == 2 && ev[1].t == 15 * MS);
        }
    }
    /* "a run again that no longer gives it, or gives it otherwise, removes
     * it with no count and queues the new one; arm_derived_stale counts only
     * events handed out": a late sample of 5 at 13 makes a new event there
     * and the integral at 15 is 0.013, not 0.005. Control: the event at 15
     * handed out */
    for (v = 0; v < 2; v++) {
        hd_der_open(4, 0);
        rp_feed(10 * MS, 1, YIN_AXIS_POSITION, 1);
        rp_feed(15 * MS, 1, YIN_AXIS_POSITION, 1);
        CHECK(ysig_derived_to(&g_rp, ev, 8, v ? 15 * MS : 12 * MS) == 1 + v);
        CHECK(!v || fabs(ev[1].x - 0.005) < 1e-6);
        rp_feed(13 * MS, 5, YIN_AXIS_POSITION, 2);
        ysig_get_stats(&g_rp, &st);
        CHECK(st.arm_derived_stale == v && st.late_unverified == 0 && st.late_far == 0);
        m = ysig_derived(&g_rp, ev, 8);
        CHECK(m == 2 && ev[0].t == 13 * MS && ev[1].t == 15 * MS && fabs(ev[1].x - 0.013) < 1e-6);
    }
    /* a run again that gives it again leaves it queued once: a late sample
     * of 1, the value held, changes no integral. Control: handed out, it is
     * not queued again, and is stale all the same: its node's sequence in R
     * now has the event at 13 before it, which the caller did not have */
    for (v = 0; v < 2; v++) {
        hd_der_open(4, 0);
        rp_feed(10 * MS, 1, YIN_AXIS_POSITION, 1);
        rp_feed(15 * MS, 1, YIN_AXIS_POSITION, 1);
        CHECK(ysig_derived_to(&g_rp, ev, 8, v ? 15 * MS : 12 * MS) == 1 + v);
        rp_feed(13 * MS, 1, YIN_AXIS_POSITION, 2);
        ysig_get_stats(&g_rp, &st);
        CHECK(st.arm_derived_stale == v);
        m = ysig_derived(&g_rp, ev, 8);
        CHECK(m == 2 - v && ev[0].t == 13 * MS && (v || (ev[1].t == 15 * MS && fabs(ev[1].x - 0.005) < 1e-6)));
    }
    /* "The queue holds YSIG_QUEUE_CAP plus catchup_calls events; an event
     * that finds it full is counted in derived_lost." Control: no window */
    for (v = 0; v < 2; v++) {
        hd_der_open(v ? 0 : 4, 100);
        for (k = 0; k < 200; k++) rp_feed((k + 1) * MS, 1, YIN_AXIS_POSITION, 1);
        ysig_get_stats(&g_rp, &st);
        CHECK(g_rp.n_der == (v ? 64 : 164) && st.derived_lost == (v ? 136 : 36));
    }
    /* "A reset or a trial reset empties it." Control: no reset */
    for (v = 0; v < 3; v++) {
        hd_der_open(4, 0);
        rp_feed(15 * MS, 1, YIN_AXIS_POSITION, 1);
        CHECK(ysig_derived_to(&g_rp, ev, 8, 12 * MS) == 0);
        if (v == 1) ysig_reset_trial(&g_rp, 20 * MS);
        if (v == 2) ysig_reset(&g_rp, 20 * MS);
        CHECK(ysig_derived(&g_rp, ev, 8) == (v == 0));
    }
}

int main(void) {
    test_conventions();
    test_param_names();
    test_integrate();
    test_maps();
    test_filters();
    test_generators();
    test_time();
    test_detect();
    test_replay();
    test_digest();
    test_history();
    test_roles();
    test_set();
    test_refusals();
    test_order();
    test_views();
    test_still();
    test_review2();
    test_review3();
#ifndef __cplusplus
    test_node_macro();
#endif
    test_review();
    test_mux();
    test_latch();
    test_distance();
    test_peak();
    test_all();
    test_silent();
    test_detect_out();
    test_reset_trial();
    test_recenter();
    test_ops_refusals();
    test_ops_views();
    test_ops_replay();
    test_recenter_time();
    test_recenter_chain();
    test_peak_order();
    test_late_stamp();
    test_rearm();
    test_hold_frame_input();
    test_u();
    test_entry_now();
    test_set_recompute();
    test_review_fixes();
    test_hit_out_time();
    test_arm_window();
    test_arm_review();
    test_arm_review2();
    test_arm_spec();
    test_arm_spec2();
    test_arm_repro();
    test_arm_exact();
    test_disarm_at_gap_end();
    test_hit_whole_value();
    test_pending();
    test_late();
    test_frame_ties();
    test_late_present_group();
    test_v2_plain_disarm();
    test_v2_old_shapes();
    test_v2_present_key();
    test_v2_s1();
    test_v2_lost_set();
    test_v2_exit_late();
    test_v2_two_untils();
    test_hold();
    if (g_failures) { fprintf(stderr, "sig_test: %d failures\n", g_failures); return 1; }
    printf("sig_test: all checks passed\n");
    return 0;
}
