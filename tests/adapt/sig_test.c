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
        CHECK(st.dropped_order == 1 && ysig_value_of(&g, 0)->v[0] == 9);
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
    /* pinned: the same with the late report from a second device on the
     * detect's own source (any device): it must come before the arm, and
     * the 21 ms sample came before it. Counted; no hit at 21 ms */
    n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 0, 2); n[0].history = 64;
    n[1] = ysig_node_of(YSIG_OP_DETECT, 0, -1);
    n[1].p[YSIG_DETECT_LANE] = 0; n[1].p[YSIG_DETECT_LEVEL] = 5; n[1].p[YSIG_DETECT_START] = 1;
    SP_OPEN(2, 4);
    e = PEN_E(1 * MS, 1, 0, 0, YIN_TIER_2); ysig_feed(&g, &e);
    f = frame_at(1, 16 * MS, 0, NULL, 0); ysig_frame(&g, &f);
    e = PEN_E(21 * MS, 1, 10, 0, YIN_TIER_2); ysig_feed(&g, &e);
    e = PEN_E(19 * MS, 2, 0, 0, YIN_TIER_2); ysig_feed(&g, &e);
    CHECK(ysig_arm(&g, 1, 20 * MS) && ysig_detected(&g, h, 64) == 0);
    ysig_get_stats(&g, &st);
    CHECK(st.arm_past_pinned == 1);
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
        CHECK(g.wt->n == 0 && ((ysig__det_st*)g.nd[1].st)->t_arm == 60 * MS);
        /* a disarm takes effect at the end of its gap (S1): the next input */
        e = PEN_E(75 * MS, 2, 0, 0, YIN_TIER_2); ysig_feed(&g, &e);
        ysig_get_stats(&g, &st);
        CHECK(st.arm_ahead_cancelled == 0);
        CHECK(((ysig__det_st*)g.nd[1].st)->armed == (what == 1));
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
    /* rule 2: a group's footprint has its set's nodes: input of the set's
     * node at or before t, made after the 21 ms crossing, comes before the
     * group, so the group comes after the crossing too (pinned) */
    n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 1, 2); n[0].history = 64;
    n[2] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 2, 2); n[2].history = 64;
    n[3] = ysig_node_of(YSIG_OP_OFFSET, 2, -1);
    SP_OPEN(4, 4);
    e = PEN_E(1 * MS, 1, 0, 0, YIN_TIER_2); ysig_feed(&g, &e);
    f = frame_at(1, 16 * MS, 0, NULL, 0); ysig_frame(&g, &f);
    e = PEN_E(21 * MS, 1, 10, 0, YIN_TIER_2); ysig_feed(&g, &e);
    e = PEN_E(19 * MS, 2, 0, 0, YIN_TIER_2); ysig_feed(&g, &e);
    CHECK(ysig_set(&g, 3, YSIG_OFFSET_D_X, 1));
    CHECK(ysig_arm(&g, 1, 20 * MS) && ysig_detected(&g, h, 256) == 0);
    ysig_get_stats(&g, &st);
    CHECK(st.arm_past_pinned == 1);
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
    /* an image keeps 64 hits: a run again that queues more, then a frame,
     * does not keep that frame's checkpoint */
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
    CHECK(m > 64 && g.cu->n_ck == k);
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
static int g_rp_rows, g_rp_ooo;
static int64_t g_rp_last;
static void rp_on_row(void* u, const ysig_row* r) { (void)u; if (r->frame <= g_rp_last) g_rp_ooo++; g_rp_last = r->frame; g_rp_rows++; }
static void rp_feed(int64_t t, double x, int ctl, int dev) {
    yin_event e;
    memset(&e, 0, sizeof e);
    e.t = t; e.type = YIN_SAMPLE; e.kind = YIN_KIND_PEN; e.control = (uint32_t)ctl; e.device = (uint32_t)dev;
    e.x = (float)x; e.value = (float)x; e.stamp = YIN_TIER_2;
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
    d.channels = g_ch; d.n_channels = 64;
    d.arena_size = ysig_arena_size(&d);
    if (!d.arena_size || d.arena_size > sizeof g_rp_ar || !ysig_open(&g_rp, &d)) { g_failures++; return 0; }
    ysig_reset(&g_rp, 0);
    g_rp_rows = g_rp_ooo = 0;
    g_rp_last = -1;
    return 1;
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
    CHECK(s1.arm_past_pinned - s0.arm_past_pinned == 1);
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
    /* S3: an arm call lets in a waiting group it meets at or before its
     * time: a past arm at 15 ms of the same detect (input of another
     * source moved N to 20) lets in the arm waiting at 15 ms, which R
     * puts first (equal times in call order) */
    n[0] = src_node(YIN_KIND_PEN, YIN_AXIS_POSITION, 1, 2);
    n[1] = ysig_node_of(YSIG_OP_DETECT, 0, -1); n[1].p[YSIG_DETECT_LANE] = 0; n[1].p[YSIG_DETECT_LEVEL] = 5; n[1].p[YSIG_DETECT_START] = 1;
    n[2] = src_node(YIN_KIND_PEN, YIN_AXIS_PEN_PRESSURE, 3, 1);
    n[3] = n[1];
    if (!rp_open(n, 4, 4, 0, false, false)) return;
    rp_feed(10 * MS, 0, YIN_AXIS_POSITION, 1);
    CHECK(ysig_arm(&g_rp, 1, 15 * MS) && g_rp.wt->n == 1);
    rp_feed(20 * MS, 1, YIN_AXIS_PEN_PRESSURE, 3);
    CHECK(ysig_arm(&g_rp, 1, 15 * MS) && g_rp.wt->n == 0);
    CHECK(((ysig__det_st*)g_rp.nd[1].st)->armed && ((ysig__det_st*)g_rp.nd[1].st)->t_arm == 15 * MS);
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
    if (g_failures) { fprintf(stderr, "sig_test: %d failures\n", g_failures); return 1; }
    printf("sig_test: all checks passed\n");
    return 0;
}
