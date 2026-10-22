/* timeline_test.c - self-checking test for ysp/timeline.h. No framework:
 * it returns 0 when every check passed and 1 after printing each failure.
 *
 * Build and run it with the warnings as errors, and again under the
 * sanitizers:
 *
 *     gcc -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Werror -O2 -Iinclude \
 *         -o tl_test tests/adapt/timeline_test.c -lm && ./tl_test
 *     gcc -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Werror -O1 -g -Iinclude \
 *         -fsanitize=address,undefined -fno-sanitize-recover=all \
 *         -o tl_test tests/adapt/timeline_test.c -lm && ./tl_test
 *     cl /nologo /W4 /WX /Iinclude tests\adapt\timeline_test.c
 *
 * Written from the manual (the comment block and the declarations of
 * ysp/timeline.h), not from the implementation. The quantization, the
 * easings and the randomized check use models written here from the
 * manual's wording, so the two can disagree. Where the manual reads two
 * ways, the check takes the literal reading and the comment says so.
 *
 * TL_MUT=<bits> breaks the track model on purpose (the MUT_* bits below,
 * the tween takeover's included); every bit must make the run fail.
 * TL_TRACE=<seed> traces a model seed, its tweens and takeovers too.
 *
 * Two checks take the literal reading of a tween's "now" ("the base time
 * of the base's last evaluated onset") where the base's time went back:
 * after a rewinding anchor, and after a pause at an earlier RT time. The
 * randomized model sets start_set in those cases, so it does not depend
 * on the reading.
 */
#define YSP_TIMELINE_IMPLEMENTATION
#include "ysp/timeline.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

/* The handle's state without its message: a failing call writes the
 * message (v0.5.0) and must change nothing else. */
#define TL_STATE offsetof(ytl_timeline, error)

/* ---------------------------------------------------------------- harness */

static int g_failures = 0;

static void fail(int line, const char* what) {
    fprintf(stderr, "timeline_test: FAIL at line %d: %s\n", line, what);
    g_failures++;
}

static void fail_i(int line, const char* what, long long got, long long want) {
    fprintf(stderr, "timeline_test: FAIL at line %d: %s (got %lld, want %lld)\n",
            line, what, got, want);
    g_failures++;
}

static void fail_f(int line, const char* what, double got, double want) {
    fprintf(stderr, "timeline_test: FAIL at line %d: %s (got %.9g, want %.9g)\n",
            line, what, got, want);
    g_failures++;
}

static void fail_s(int line, const char* what, const char* got, const char* want) {
    fprintf(stderr, "timeline_test: FAIL at line %d: %s\n  got:  [%s]\n  want: [%s]\n",
            line, what, got, want);
    g_failures++;
}

/* long long, not long: long is 32 bits on Windows and times are 64. */
#define CHECK(cond) do { if (!(cond)) fail(__LINE__, #cond); } while (0)
#define CHECK_I(got, want) do { long long g_ = (long long)(got), w_ = (long long)(want); \
    if (g_ != w_) fail_i(__LINE__, #got, g_, w_); } while (0)
#define CHECK_F(got, want, tol) do { double g_ = (double)(got), w_ = (double)(want); \
    if (!(fabs(g_ - w_) <= (tol))) fail_f(__LINE__, #got, g_, w_); } while (0)
#define CHECK_S(got, want) do { const char* g_ = (got); const char* w_ = (want); \
    if (strcmp(g_, w_) != 0) fail_s(__LINE__, #got, g_, w_); } while (0)

/* Built at run time: MSVC rejects 0.0/0.0 as a constant expression. */
static volatile double g_zero = 0.0;
static double make_nan(void) { return g_zero / g_zero; }
static double make_inf(void) { return 1.0 / g_zero; }

#define S_NS  YRT_S(1)
#define MS_NS YRT_MS(1)
#define P60   INT64_C(16666667)
#define T0    YRT_S(1)
#define LIM62 INT64_C(4611686018427387904)
/* The first number that is not a ytl_ease: 7 until v0.6.0 added four. */
#define NOT_EASE 11

/* Handles are about 12 KB each, so not on the stack. */
#define STORE_CAP 1024
static ytl_timeline g_tl, g_tl2, g_closed;
static ytl_event g_store[STORE_CAP], g_store2[STORE_CAP];
static ytl_event g_fired[STORE_CAP];
static ytl_event g_snap[STORE_CAP];

/* ---------------------------------------------------------------- the rng */

static uint64_t g_rng = 1;

static uint64_t sm64(void) {
    uint64_t z = (g_rng += UINT64_C(0x9E3779B97F4A7C15));
    z = (z ^ (z >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94D049BB133111EB);
    return z ^ (z >> 31);
}

static int rnd_int(int n) { return (int)(sm64() % (uint64_t)n); }

/* Uniform in [lo, hi]. */
static int64_t rnd_range(int64_t lo, int64_t hi) {
    return lo + (int64_t)(sm64() % (uint64_t)(hi - lo + 1));
}

/* ---------------------------------------------------------------- helpers */

static ytl_event ev(int base, int64_t time, int kind, int target, float value) {
    ytl_event e;
    memset(&e, 0, sizeof e);
    e.time_ns = time;
    e.base = (uint8_t)base;
    e.kind = (uint8_t)kind;
    e.target = target;
    e.value = value;
    return e;
}

static int add1(ytl_timeline* tl, int base, int64_t time, int kind, int target, float value) {
    ytl_event e = ev(base, time, kind, target, value);
    return ytl_add(tl, &e);
}

static ytl_key mkkey(int64_t t, float v, int ease) {
    ytl_key k;
    memset(&k, 0, sizeof k);
    k.time_ns = t;
    k.value = v;
    k.ease = (uint8_t)ease;
    return k;
}

static ytl_key mkbez(int64_t t, float v, int curve) {
    ytl_key k = mkkey(t, v, YTL_EASE_BEZIER);
    k.curve = (uint16_t)curve;
    return k;
}

static ytl_curve mkcurve(float x1, float cy1, float x2, float cy2) {
    ytl_curve c;
    c.x1 = x1;
    c.y1 = cy1;
    c.x2 = x2;
    c.y2 = cy2;
    return c;
}

static ytl_track trk_keys(const ytl_key* k, int n) {
    ytl_track tr;
    memset(&tr, 0, sizeof tr);
    tr.keys = k;
    tr.n_keys = n;
    return tr;
}

static ytl_track trk_samples(const float* s, int n, int64_t t0, int64_t rate, int interp) {
    ytl_track tr;
    memset(&tr, 0, sizeof tr);
    tr.samples = s;
    tr.n_samples = n;
    tr.t0_ns = t0;
    tr.rate = rate;
    tr.interp = interp;
    return tr;
}

/* The v0.1.0 ytl_sample(keys, n, t). */
static double ksample(const ytl_key* k, int n, int64_t t) {
    ytl_track tr = trk_keys(k, n);
    return ytl_sample(&tr, t);
}

static bool open_h(ytl_timeline* tl, ytl_event* store, int cap, int nch,
                   const float* init, double lead) {
    ytl_desc d;
    memset(&d, 0, sizeof d);
    if (store && cap > 0) memset(store, 0, (size_t)cap * sizeof *store);
    d.events = store;
    d.event_capacity = cap;
    d.n_channels = nch;
    d.initial = init;
    /* Every check here that passes 0.0 means no lead at all. Since v0.2.0
     * a desc lead of 0 means the default, half a frame, so 0.0 maps to
     * YTL_LEAD_NONE; test_lead_default() checks the raw 0. */
    d.lead = lead == 0.0 ? YTL_LEAD_NONE : lead;
    return ytl_open(tl, &d);
}

static int eval_at(ytl_timeline* tl, int64_t onset, int64_t period, int64_t index,
                   ytl_event* fired, int cap) {
    ytl_frame f;
    f.onset = onset;
    f.period = period;
    f.index = index;
    return ytl_evaluate(tl, &f, fired, cap);
}

static int n_events(const ytl_timeline* tl, int base) {
    int n = -1;
    (void)ytl_events(tl, base, &n);
    return n;
}

static int key_cmp(const ytl_event* a, const ytl_event* b) {
    if (a->base != b->base) return a->base < b->base ? -1 : 1;
    if (a->time_ns != b->time_ns) return a->time_ns < b->time_ns ? -1 : 1;
    if (a->id != b->id) return a->id < b->id ? -1 : 1;
    return 0;
}

/* RT target order of the report: onset - residual, then id. */
static int report_cmp(const ytl_event* a, const ytl_event* b) {
    int64_t ta = a->onset - a->residual, tb = b->onset - b->residual;
    if (ta != tb) return ta < tb ? -1 : 1;
    if (a->id != b->id) return a->id < b->id ? -1 : 1;
    return 0;
}

static void check_sorted(const ytl_timeline* tl, int line) {
    int n = 0, i, b, total = 0;
    const ytl_event* p = ytl_events(tl, YTL_ALL_BASES, &n);
    if (n > 0 && !p) { fail(line, "ytl_events(ALL) is NULL with n > 0"); return; }
    for (i = 1; i < n; i++)
        if (key_cmp(&p[i - 1], &p[i]) >= 0) { fail(line, "storage not sorted by base, time, id"); break; }
    for (b = 0; b < YTL_MAX_BASES; b++) {
        int nb = 0;
        const ytl_event* q = ytl_events(tl, b, &nb);
        for (i = 0; i < nb; i++)
            if (q[i].base != b) { fail(line, "ytl_events(base) holds another base"); break; }
        total += nb;
    }
    if (total != n) fail_i(line, "sum of per-base ytl_events counts", total, n);
}

static bool is_pending_reset(const ytl_event* e) {
    return e && e->frame == -1 && e->onset == 0 && e->residual == 0 && e->flags == 0;
}

/* ------------------------------------------- exact reference for RATE */

/* floor(d * a / b) for |d| < 2^63 and a, b in [1, 2^31): the 94-bit
 * product in 32-bit limbs, then long division by b. Written apart from
 * the header's split of d by b, so the two can disagree. *big is set when
 * the exact value does not fit an int64. */
static int64_t ref_floor_md(int64_t d, int64_t a, int64_t b, int* big) {
    uint64_t m = d < 0 ? (uint64_t)0 - (uint64_t)d : (uint64_t)d;
    uint64_t lo = (m & 0xFFFFFFFFu) * (uint64_t)a;
    uint64_t hi = (m >> 32) * (uint64_t)a;
    uint64_t top = hi + (lo >> 32);
    uint64_t qt = top / (uint64_t)b, rt = top % (uint64_t)b;
    uint64_t low = (rt << 32) | (lo & 0xFFFFFFFFu);
    uint64_t ql = low / (uint64_t)b, rl = low % (uint64_t)b, q;
    *big = 0;
    if (qt >> 31) {
        *big = 1;
        return d < 0 ? INT64_MIN : INT64_MAX;
    }
    q = (qt << 32) + ql;
    if (d >= 0) return (int64_t)q;
    if (rl != 0) q++;
    if (q > (uint64_t)INT64_MAX) {
        *big = 1;
        return INT64_MIN;
    }
    return -(int64_t)q;
}

static int64_t ref_ceil_md(int64_t d, int64_t a, int64_t b, int* big) {
    int64_t f = ref_floor_md(-d, a, b, big);
    return f == INT64_MIN ? INT64_MAX : -f;
}

#if defined(__SIZEOF_INT128__)
/* A second reference where the compiler has 128-bit integers: checks the
 * limb code itself. */
__extension__ typedef __int128 ref_i128;
static int64_t i128_floor_md(int64_t d, int64_t a, int64_t b, int* big) {
    ref_i128 p = (ref_i128)d * a, q = p / b;
    if (p % b < 0) q--;
    *big = q > INT64_MAX || q < INT64_MIN;
    return *big ? (q < 0 ? INT64_MIN : INT64_MAX) : (int64_t)q;
}
#endif

static long long g_ref_checks, g_ref_bad;

static int64_t gcd64(int64_t x, int64_t y) {
    while (y) { int64_t t = x % y; x = y; y = t; }
    return x;
}

/* The manual's mapping of a running base with anchor (art, abt) and rate
 * num/den: base time at RT t, and the first RT time reaching base time T. */
static int64_t ref_bt(int64_t art, int64_t abt, int64_t num, int64_t den, int64_t t) {
    int big;
    int64_t x = ref_floor_md(t - art, num, den, &big);
#if defined(__SIZEOF_INT128__)
    {
        int big2;
        int64_t y = i128_floor_md(t - art, num, den, &big2);
        g_ref_checks++;
        if (y != x || big != big2) g_ref_bad++;
    }
#endif
    return abt + x;
}

static int64_t ref_rt(int64_t art, int64_t abt, int64_t num, int64_t den, int64_t T) {
    int big;
    return art + ref_ceil_md(T - abt, den, num, &big);
}

/* ------------------------------------------------- reference easing model */

#define REF_PI 3.14159265358979323846

static double ref_ease(int ease, double v0, double v1, double u) {
    switch (ease) {
    case YTL_EASE_STEP:     return v0;
    case YTL_EASE_COSINE:   return v0 + (v1 - v0) * ((1.0 - cos(REF_PI * u)) / 2.0);
    case YTL_EASE_QUAD_IN:  return v0 + (v1 - v0) * (u * u);
    case YTL_EASE_QUAD_OUT: return v0 + (v1 - v0) * (1.0 - (1.0 - u) * (1.0 - u));
    case YTL_EASE_LOG:      return v0 * pow(v1 / v0, u);
    default:                  return v0 + (v1 - v0) * u;
    }
}

/* TL_MUT=<bits> breaks the track model on purpose. Each bit must make the
 * run fail; that shows the checks can see the difference. */
static unsigned g_mut;
#define MUT_PHANTOM   1u   /* end samples repeated, not extrapolated          */
#define MUT_STEP_NS   2u   /* sample i at t0 + i * trunc(1e9 / rate)          */
#define MUT_HOLD_LATE 4u   /* the hold after the last cycle starts a cycle late */
#define MUT_HOLD_FOLD 8u   /* the hold reads start + period folded: start     */
#define MUT_BEZ_PARAM 16u  /* the curve parameter taken for x                 */
#define MUT_STEP_ONSET 32u /* STEP tracks read at the onset, not the window end */
#define MUT_TW_FROM_CALL 64u /* a tween's from read at the call, not at its start */
#define MUT_TW_SLOPE  128u /* keep_velocity ignores the old track's slope      */
#define MUT_TW_YOYO   256u /* yoyo's return leg missing                         */
/* v0.3.0: skip and peek, in the random model (ops on) only. */
#define MUT_SKIP_FIRES   512u    /* a skip is only an anchor: what it passes fires late */
#define MUT_SKIP_NOCHAN  1024u   /* a skipped SET does not count for its channel   */
#define MUT_SKIP_REACH   2048u   /* a skip leaves the reach of an evaluated base   */
#define MUT_SKIP_NOW     4096u   /* a skip leaves the furthest onset: no rewind back */
#define MUT_SKIP_AT      8192u   /* the event exactly at bt is skipped too         */
#define MUT_SKIP_UNREW   16384u  /* a rewind leaves skipped events skipped         */
#define MUT_PEEK_END     32768u  /* peek's window closed at to_rt                  */
#define MUT_PEEK_FIRED   65536u  /* peek lists fired events too                    */
#define MUT_PEEK_ONSET   131072u /* a peek copy's onset is its base time           */
#define MUT_SKIP_LATEW   262144u /* a skip back leaves waiting late events pending */
/* RATE (v0.4.0) */
#define MUT_RATE_REWIND   (1u << 19) /* a rate change rewinds like an anchor         */
#define MUT_RATE_TRUNC    (1u << 20) /* the scaled time truncates toward 0, not floor */
#define MUT_RATE_LEAD     (1u << 21) /* window end bt(onset) + L, not bt(onset + L)  */
#define MUT_RATE_INVFLOOR (1u << 22) /* the RT time of a base time rounds down      */
#define MUT_RATE_PAUSE1   (1u << 23) /* a pause freezes at rate 1                   */
#define MUT_RATE_ORDER    (1u << 24) /* the report ordered by onset - residual      */
#define MUT_RATE_NOW      (1u << 25) /* a rate change leaves "now"                  */
#define MUT_RATE_SAME     (1u << 26) /* the same rate re-anchors a running base     */
#define MUT_RATE_ANCHOR1  (1u << 27) /* an anchor or skip resets the rate to 1/1    */

#define NS_E9 INT64_C(1000000000)

/* CSS cubic-bezier(): x(s) and y(s) from (0, 0) to (1, 1). */
static double bez_x(const ytl_curve* c, double s) {
    double a = 1.0 - s;
    return 3.0 * a * a * s * (double)c->x1 + 3.0 * a * s * s * (double)c->x2 + s * s * s;
}

static double bez_y(const ytl_curve* c, double s) {
    double a = 1.0 - s;
    return 3.0 * a * a * s * (double)c->y1 + 3.0 * a * s * s * (double)c->y2 + s * s * s;
}

/* The smallest s in [0, 1] with x(s) >= u, by bisection: x rises with s
 * when x1 and x2 are in [0, 1]. */
static double bez_s(const ytl_curve* c, double u) {
    double lo = 0.0, hi = 1.0;
    int i;
    if (u <= 0.0) return 0.0;
    if (u >= 1.0) return 1.0;
    for (i = 0; i < 80; i++) {
        double m = 0.5 * (lo + hi);
        if (bez_x(c, m) < u) lo = m; else hi = m;
    }
    return hi;
}

static double ref_bezier(const ytl_curve* c, double u) {
    if (g_mut & MUT_BEZ_PARAM) return bez_y(c, u);
    return bez_y(c, bez_s(c, u));
}

/* Every y a solver may return that stops within tolx of u in x. Where x
 * is flat in s (x1 or x2 at a limit) that is a wide range of s. */
static void bez_range(const ytl_curve* c, double u, double tolx, double* lo, double* hi) {
    double sa, sb;
    int i;
    if (g_mut & MUT_BEZ_PARAM) { *lo = *hi = bez_y(c, u); return; }
    sa = bez_s(c, u - tolx);
    sb = bez_s(c, u + tolx);
    *lo = *hi = bez_y(c, sa);
    for (i = 1; i <= 64; i++) {
        double y = bez_y(c, sa + (sb - sa) * (double)i / 64.0);
        if (y < *lo) *lo = y;
        if (y > *hi) *hi = y;
    }
}

/* TRACKS, by a linear scan: first key's value before it, last key's value
 * from it on, otherwise the last key i with t_i <= t, whose successor is
 * then strictly later (so at equal times the later key wins). */
static double ref_keyed(const ytl_key* k, int n, const ytl_curve* cv, int64_t t) {
    int i = 0, j;
    double u;
    if (!k || n < 1) return 0.0;
    if (t < k[0].time_ns) return k[0].value;
    if (t >= k[n - 1].time_ns) return k[n - 1].value;
    for (j = 0; j < n; j++) if (k[j].time_ns <= t) i = j;
    u = (double)(t - k[i].time_ns) / (double)(k[i + 1].time_ns - k[i].time_ns);
    if (k[i].ease == YTL_EASE_BEZIER) {
        double y = cv ? ref_bezier(&cv[k[i].curve], u) : u;
        return (double)k[i].value + ((double)k[i + 1].value - (double)k[i].value) * y;
    }
    return ref_ease(k[i].ease, k[i].value, k[i + 1].value, u);
}

static double ref_sample(const ytl_key* k, int n, int64_t t) {
    return ref_keyed(k, n, NULL, t);
}

/* Where t falls on a sampled track: -1 before t0, else the index i of the
 * sample at or before t, and *fnum = (t - t0) rate - i 1e9, the fraction's
 * numerator over 1e9. Integer arithmetic: sample i is at t0 + i 1e9 / rate
 * exactly. (t - t0) rate may not fit 64 bits, so it is split at whole
 * seconds; r rate < 1e18 does. */
static int64_t ref_sidx(const ytl_track* tr, int64_t t, int64_t* fnum) {
    int64_t d = t - tr->t0_ns, q, r;
    *fnum = 0;
    if (d < 0) return -1;
    if (g_mut & MUT_STEP_NS) {
        int64_t step = NS_E9 / tr->rate, i = d / step;
        *fnum = (d - i * step) * NS_E9 / step;
        return i;
    }
    q = d / NS_E9;
    r = d % NS_E9;
    if (q > (int64_t)tr->n_samples) return (int64_t)tr->n_samples;   /* past the end */
    *fnum = r * tr->rate % NS_E9;
    return q * tr->rate + r * tr->rate / NS_E9;
}

static double ref_sampled(const ytl_track* tr, int64_t t) {
    const float* v = tr->samples;
    int n = tr->n_samples;
    int64_t i, fnum;
    double u, p0, p1, p2, p3;
    if (!v || n < 1) return 0.0;
    i = ref_sidx(tr, t, &fnum);
    if (i < 0) return v[0];
    if (i >= n - 1) return v[n - 1];
    u = (double)fnum / 1e9;
    p1 = v[i];
    p2 = v[i + 1];
    switch (tr->interp) {
    case YTL_INTERP_STEP:
        return p1;
    case YTL_INTERP_CUBIC:
        /* Phantoms: v_-1 = 2 v_0 - v_1, v_n = 2 v_n-1 - v_n-2. */
        p0 = i > 0 ? (double)v[i - 1] : (g_mut & MUT_PHANTOM) ? p1 : 2.0 * p1 - p2;
        p3 = i + 2 < n ? (double)v[i + 2] : (g_mut & MUT_PHANTOM) ? p2 : 2.0 * p2 - p1;
        return 0.5 * (2.0 * p1 + (p2 - p0) * u + (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3) * u * u +
                      (3.0 * p1 - p0 - 3.0 * p2 + p3) * u * u * u);
    default:
        return p1 + (p2 - p1) * u;
    }
}

static double ref_base(const ytl_track* tr, int64_t t) {
    if (tr->keys && tr->n_keys > 0) return ref_keyed(tr->keys, tr->n_keys, tr->curves, t);
    if (tr->samples && tr->n_samples > 0) return ref_sampled(tr, t);
    return 0.0;
}

/* REPEAT: from the start (first key's time, or t0), t reads the track at
 * start + ((t - start) mod period) for `repeats` cycles (0: forever), then
 * holds the value at start + period read without repeating. Before the
 * start, no repeat. */
static double ref_track(const ytl_track* tr, int64_t t) {
    int64_t start, k;
    if (tr->period_ns <= 0) return ref_base(tr, t);
    if (tr->keys && tr->n_keys > 0) start = tr->keys[0].time_ns;
    else if (tr->samples && tr->n_samples > 0) start = tr->t0_ns;
    else return 0.0;
    if (t < start) return ref_base(tr, t);
    k = (t - start) / tr->period_ns;
    if (tr->repeats > 0 && k >= (int64_t)tr->repeats + ((g_mut & MUT_HOLD_LATE) ? 1 : 0))
        return ref_base(tr, (g_mut & MUT_HOLD_FOLD) ? start : start + tr->period_ns);
    return ref_base(tr, start + (t - start) % tr->period_ns);
}

/* The track a tween stands for: two keys, from at start to `to` at
 * start + duration, the curve copied. */
static ytl_track tween_track(ytl_key* kk, ytl_curve* cc, int64_t start, int64_t dur,
                               float from, float to, int ease, const ytl_curve* c) {
    ytl_track tr;
    kk[0] = mkkey(start, from, ease);
    kk[1] = mkkey(start + dur, to, YTL_EASE_LINEAR);
    tr = trk_keys(kk, 2);
    if (c) {
        *cc = *c;
        tr.curves = cc;
        tr.n_curves = 1;
    }
    return tr;
}

/* A STEP track (TRACKS): sampled with YTL_INTERP_STEP, or keyed with
 * every key but the last YTL_EASE_STEP, a tween included. An evaluate
 * reads it at the end of the lead window, base time at onset + lead x
 * period; every other track at the onset. One key: no key but the last
 * breaks the rule, and the value is constant anyway. */
static int trk_is_step(const ytl_track* tr) {
    int i;
    if (g_mut & MUT_STEP_ONSET) return 0;
    if (tr->keys && tr->n_keys > 0) {
        for (i = 0; i + 1 < tr->n_keys; i++)
            if (tr->keys[i].ease != YTL_EASE_STEP) return 0;
        return 1;
    }
    return tr->samples && tr->n_samples > 0 && tr->interp == YTL_INTERP_STEP;
}

static double tol_of(double v) { return 1e-6 * (1.0 + fabs(v)); }
/* For ytl_sample's double against the model's: the same +, -, *, / in
 * another order. */
static double tol_d(double v) { return 1e-9 * (1.0 + fabs(v)); }

/* ------------------------------------------------ tween reference model */

static ytl_tween_desc twd(float to, int64_t dur) {
    ytl_tween_desc d;
    memset(&d, 0, sizeof d);
    d.to = to;
    d.duration_ns = dur;
    return d;
}

static ytl_tween_desc twd_at(float to, int64_t dur, int64_t start) {
    ytl_tween_desc d = twd(to, dur);
    d.start_ns = start;
    d.start_set = true;
    return d;
}

/* A channel's driver in the model: a track the caller set, or a tween as
 * it is at its start (TWEENS). own: the keys and the curve are k[] and c,
 * so the struct may be copied. herm: keep_velocity's Hermite segment. */
typedef struct rdrv {
    ytl_track tr;
    ytl_key   k[3];
    ytl_curve c;
    int         own, herm;
    double      hf, hm, ht;   /* Hermite: from, start slope x duration, to */
    int64_t     hs, hd;       /* Hermite: start and duration               */
    int         yo;           /* v0.7.0 yoyo: the return leg is the forward
                               * leg backward in time (not STEP)          */
    int64_t     ys, yd;       /* yoyo: start and duration                  */
} rdrv;

static ytl_track rdrv_track(const rdrv* r) {
    ytl_track t = r->tr;
    if (r->own) {
        t.keys = r->k;
        t.curves = t.n_curves > 0 ? &r->c : NULL;
    }
    return t;
}

/* The cubic Hermite segment from `from` with tangent m0 (slope x duration)
 * to `to` with tangent 0, at u in [0, 1]. */
static double ref_hermite(double from, double m0, double to, double u) {
    double u2 = u * u, u3 = u2 * u;
    return (2.0 * u3 - 3.0 * u2 + 1.0) * from + (u3 - 2.0 * u2 + u) * m0 + (3.0 * u2 - 2.0 * u3) * to;
}

static double rdrv_value(const rdrv* r, int64_t t) {
    ytl_track tr;
    if (r->herm) {
        /* Before the start (after a rewind): the start value, as a keyed
         * track gives its first key's value before it. */
        if (t < r->hs) return r->hf;
        if (t >= r->hs + r->hd) return r->ht;
        return ref_hermite(r->hf, r->hm, r->ht, (double)(t - r->hs) / (double)r->hd);
    }
    /* A yoyo's return leg: GSAP's rule read literally, the forward leg's
     * value at the mirrored time, after the repeat's fold. */
    if (r->yo && t > r->ys) {
        int64_t ph = t - r->ys, per = 2 * r->yd;
        if (r->tr.period_ns > 0) {
            int64_t q = ph / per;
            if (r->tr.repeats > 0 && q >= r->tr.repeats) ph = per;
            else ph -= q * per;
        }
        if (ph > r->yd && ph < per) t = r->ys + (per - ph);
    }
    tr = rdrv_track(r);
    return ref_track(&tr, t);
}

static int rdrv_step(const rdrv* r) {
    ytl_track tr;
    if (r->herm) return 0;
    tr = rdrv_track(r);
    return trk_is_step(&tr);
}

static rdrv rdrv_plain(const ytl_track* tr) {
    rdrv r;
    memset(&r, 0, sizeof r);
    r.tr = *tr;
    return r;
}

/* THE TAKEOVER: the tween `d` taking over at its start s from the old
 * driver (NULL: no track, the channel holds `held`). from is the old
 * track's value at s; with keep_velocity the start slope is the old
 * track's over the last microsecond before s. at_call is the channel's
 * value when the tween was posted, for MUT_TW_FROM_CALL only. Keys: from
 * at s with d's ease, `to` at s + duration (with d's ease under yoyo), and
 * under yoyo from again at s + 2 duration; cycles repeat the whole. Since
 * v0.7.0 a yoyo's return leg is read at the mirrored time (rdrv_value), but
 * STEP's, which keeps its square wave. */
static void rdrv_tween(rdrv* r, const ytl_tween_desc* d, int64_t s, const rdrv* old, double held,
                       double at_call) {
    double from;
    int n = 2, yoyo = d->yoyo ? 1 : 0, multi = d->cycles > 1 || d->cycles == YTL_FOREVER;
    memset(r, 0, sizeof *r);
    if (d->from_set) from = (double)d->from;
    else if (g_mut & MUT_TW_FROM_CALL) from = at_call;
    else from = old ? rdrv_value(old, s) : held;
    from = (double)(float)from;   /* the keys are float */
    if (d->keep_velocity) {
        r->herm = 1;
        r->hf = from;
        r->ht = (double)d->to;
        r->hs = s;
        r->hd = d->duration_ns;
        r->hm = (old && !(g_mut & MUT_TW_SLOPE))
                    ? (rdrv_value(old, s) - rdrv_value(old, s - 1000)) / 1000.0 * (double)d->duration_ns
                    : 0.0;
        return;
    }
    r->own = 1;
    r->k[0] = mkkey(s, (float)from, d->ease);
    r->k[1] = mkkey(s + d->duration_ns, d->to, yoyo ? d->ease : YTL_EASE_LINEAR);
    if (yoyo && !(g_mut & MUT_TW_YOYO)) {
        r->k[2] = mkkey(s + 2 * d->duration_ns, (float)from, YTL_EASE_LINEAR);
        n = 3;
        if (d->ease != YTL_EASE_STEP) {
            r->yo = 1;
            r->ys = s;
            r->yd = d->duration_ns;
        }
    }
    if (d->ease == YTL_EASE_BEZIER) {
        r->c = d->curve;
        r->tr.n_curves = 1;
    }
    r->tr.n_keys = n;
    if (multi) {
        r->tr.period_ns = (yoyo ? 2 : 1) * d->duration_ns;
        r->tr.repeats = d->cycles == YTL_FOREVER ? 0 : d->cycles;
    }
}

static int curve_ok(const ytl_curve* c) {
    return c->x1 >= 0.0f && c->x1 <= 1.0f && c->x2 >= 0.0f && c->x2 <= 1.0f &&
           fabs((double)c->y1) <= 3.4e38 && fabs((double)c->y2) <= 3.4e38;
}

/* TWEENS' rejections, but the time limits (the callers stay far from them). */
static int tw_valid(const ytl_tween_desc* d) {
    int multi = d->cycles > 1 || d->cycles == YTL_FOREVER;
    if (d->duration_ns < 0) return 0;
    if (!(fabs((double)d->to) <= 3.4e38)) return 0;
    if (d->from_set && !(fabs((double)d->from) <= 3.4e38)) return 0;
    if (d->ease < 0 || d->ease > YTL_EASE_BEZIER) return 0;
    if (d->ease == YTL_EASE_BEZIER && !curve_ok(&d->curve)) return 0;
    if (d->ease == YTL_EASE_LOG && (!d->from_set || d->from <= 0.0f || d->to <= 0.0f)) return 0;
    if (d->cycles < YTL_FOREVER) return 0;
    if ((multi || d->yoyo) && d->duration_ns == 0) return 0;
    if (d->keep_velocity &&
        (d->ease != YTL_EASE_LINEAR || d->from_set || d->yoyo || multi || d->duration_ns == 0)) return 0;
    return 1;
}

/* ------------------------------------------------------- 1. open and misc */

static void open_fail(int line, ytl_event* store, int cap, int nch, const float* init, double lead) {
    if (open_h(&g_tl, store, cap, nch, init, lead)) fail(line, "open accepted a bad desc");
    else if (ytl_error(&g_tl)[0] == '\0') fail(line, "open failed with an empty message");
}

static void test_open(void) {
    float init[4] = { 0.5f, -1.0f, 2.0f, 3.0f };
    float bad[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    const float* v;
    ytl_event e;
    ytl_desc d;
    int i;

    /* A zeroed handle is not open. */
    CHECK(!ytl_is_open(&g_closed));
    CHECK(ytl_values(&g_closed) == NULL);
    e = ev(0, 0, YTL_MARK, 0, 0.0f);
    CHECK_I(ytl_add(&g_closed, &e), YTL_ERR_CLOSED);
    CHECK_I(eval_at(&g_closed, 0, 0, 0, NULL, 0), YTL_ERR_CLOSED);
    CHECK_I(ytl_anchor(&g_closed, 1, 0, 0), YTL_ERR_CLOSED);

    open_fail(__LINE__, g_store, -1, 4, NULL, 0.0);
    open_fail(__LINE__, NULL, 4, 4, NULL, 0.0);
    open_fail(__LINE__, g_store, 4, -1, NULL, 0.0);
    open_fail(__LINE__, g_store, 4, YTL_MAX_CHANNELS + 1, NULL, 0.0);
    open_fail(__LINE__, g_store, 4, 4, NULL, -0.01);
    open_fail(__LINE__, g_store, 4, 4, NULL, -0.99);
    open_fail(__LINE__, g_store, 4, 4, NULL, -1.01);
    open_fail(__LINE__, g_store, 4, 4, NULL, 1.0);
    open_fail(__LINE__, g_store, 4, 4, NULL, 1.5);
    open_fail(__LINE__, g_store, 4, 4, NULL, make_nan());
    open_fail(__LINE__, g_store, 4, 4, NULL, make_inf());
    open_fail(__LINE__, g_store, 4, 4, NULL, -make_inf());
    bad[2] = (float)make_nan();
    open_fail(__LINE__, g_store, 4, 4, bad, 0.0);
    bad[2] = 0.0f;
    bad[1] = (float)make_inf();
    open_fail(__LINE__, g_store, 4, 4, bad, 0.0);
    bad[1] = (float)-make_inf();
    open_fail(__LINE__, g_store, 4, 4, bad, 0.0);

    /* A failed open leaves a never-opened handle closed. */
    memset(&d, 0, sizeof d);
    d.event_capacity = -1;
    CHECK(!ytl_open(&g_closed, &d));
    CHECK(!ytl_is_open(&g_closed));
    CHECK(ytl_values(&g_closed) == NULL);

    /* Accepted edges. */
    CHECK(open_h(&g_tl, NULL, 0, 0, NULL, 0.0));
    CHECK_S(ytl_error(&g_tl), "");
    CHECK(ytl_is_open(&g_tl));
    CHECK(open_h(&g_tl, g_store, 4, YTL_MAX_CHANNELS, NULL, 0.999));
    v = ytl_values(&g_tl);
    CHECK(v != NULL);
    if (v) for (i = 0; i < YTL_MAX_CHANNELS; i++) if (v[i] != 0.0f) { fail(__LINE__, "NULL initial is not all 0"); break; }

    /* initial is copied at open. */
    CHECK(open_h(&g_tl, g_store, 4, 4, init, 0.5));
    init[0] = 99.0f;
    v = ytl_values(&g_tl);
    CHECK(v != NULL);
    if (v) {
        CHECK(v[0] == 0.5f);
        CHECK(v[1] == -1.0f);
        CHECK(v[2] == 2.0f);
        CHECK(v[3] == 3.0f);
    }
    CHECK(ytl_value(&g_tl, 1) == -1.0f);

    /* open() resets the events and restarts the ids. */
    CHECK_I(add1(&g_tl, 0, 5, YTL_MARK, 0, 0.0f), 0);
    CHECK_I(add1(&g_tl, 0, 6, YTL_MARK, 0, 0.0f), 1);
    CHECK(open_h(&g_tl, g_store, 4, 4, NULL, 0.0));
    CHECK_I(n_events(&g_tl, YTL_ALL_BASES), 0);
    CHECK_I(add1(&g_tl, 0, 5, YTL_MARK, 0, 0.0f), 0);
}

static void test_misc(void) {
    char buf[64];
    int c;
    CHECK_I(ytl_ns(0.0), 0);
    CHECK_I(ytl_ns(1.0), S_NS);
    CHECK_I(ytl_ns(-2.0), -2 * S_NS);
    CHECK_I(ytl_ns(0.25), 250000000);
    CHECK_I(ytl_ns(1.4e-9), 1);
    CHECK_I(ytl_ns(1.6e-9), 2);
    CHECK_I(ytl_ns(-1.4e-9), -1);
    CHECK_I(ytl_ns(-1.6e-9), -2);
    CHECK_I(ytl_ns(0.4e-9), 0);
    /* 1/1024 s is exactly 976562.5 ns: a tie, away from zero (to-even
     * would give 976562). */
    CHECK_I(ytl_ns(1.0 / 1024.0), 976563);
    CHECK_I(ytl_ns(-1.0 / 1024.0), -976563);
    CHECK_I(ytl_ns(3.0 / 1024.0), 2929688);
    CHECK_I(ytl_ns(4611686018.0), INT64_C(4611686018000000000));
    CHECK_I(ytl_ns(5e9), LIM62);
    CHECK_I(ytl_ns(1e300), LIM62);
    CHECK_I(ytl_ns(-1e300), -LIM62);
    CHECK_I(ytl_ns(make_inf()), LIM62);
    CHECK_I(ytl_ns(-make_inf()), -LIM62);
    CHECK_I(ytl_ns(make_nan()), 0);

    for (c = -6; c <= -1; c++) {
        const char* s = ytl_strerror(c);
        CHECK(s != NULL);
        if (s) { CHECK(s[0] != '\0'); CHECK(strcmp(s, "ok") != 0); }
    }
    CHECK(strcmp(ytl_strerror(YTL_ERR_ARG), ytl_strerror(YTL_ERR_ORDER)) != 0);
    CHECK_S(ytl_strerror(0), "ok");
    CHECK_S(ytl_strerror(7), "ok");

    snprintf(buf, sizeof buf, "%d.%d.%d", YTL_VERSION_MAJOR, YTL_VERSION_MINOR,
             YTL_VERSION_PATCH);
    CHECK_S(buf, YTL_VERSION_STRING);
    CHECK_S(ytl_version(), YTL_VERSION_STRING);
}

/* --------------------------------------------------------- 2. quantization */

static int64_t ceil_div(int64_t a, int64_t b) {   /* b > 0 */
    int64_t q = a / b;
    if (a % b != 0 && a > 0) q++;
    return q;
}

/* The grid frame an event at RT time t lands on: the first k >= 0 with
 * T0 + k P + w >= t. */
static int64_t quant_k(int64_t t, int64_t P, int64_t w) {
    int64_t k = ceil_div(t - w - T0, P);
    return k < 0 ? 0 : k;
}

/* The grid's period P is the frame period in whole ns. Displays now run
 * from 60 Hz to 500 Hz and beyond, and nothing in the header may assume a
 * rate, so the grid checks run at several. */
static void quant_grid(int64_t P, int lead_num, int lead_den, int base, uint64_t seed) {
    enum { NE = 300, NF = 420 };
    static int64_t ev_rt[NE];
    static int ev_id[NE];
    static int cnt[NF];
    const int64_t w = P * lead_num / lead_den;   /* truncated, P > 0 */
    int64_t off = 0;   /* base time = RT + off */
    int i, k, bad_n = 0, bad_order = 0, bad_land = 0, first_bad = -1;

    g_rng = seed;
    CHECK(open_h(&g_tl, g_store, NE + 4, 1, NULL, (double)lead_num / (double)lead_den));
    if (base != 0) {
        CHECK_I(ytl_anchor(&g_tl, base, T0 + 12345, 500 * MS_NS), 0);
        off = 500 * MS_NS - (T0 + 12345);
    }
    memset(cnt, 0, sizeof cnt);
    for (i = 0; i < NE; i++) {
        ytl_event e;
        int64_t t = rnd_range(T0 - P, T0 + 400 * P);
        int64_t g = T0 + (t - T0) / P * P;
        if (i % 10 == 0) t = g;            /* on a frame                 */
        if (i % 10 == 1) t = g + w;        /* on a window edge           */
        if (i % 10 == 2) t = g + w + 1;    /* just past a window edge    */
        ev_rt[i] = t;
        e = ev(base, t + off, YTL_MARK, 0, 0.0f);
        e.code = i;
        ev_id[i] = ytl_add(&g_tl, &e);
        CHECK_I(ev_id[i], i);
        k = (int)quant_k(t, P, w);
        if (k < NF) cnt[k]++;
    }
    for (k = 0; k < NF; k++) {
        int64_t onset = T0 + (int64_t)k * P;
        int n = eval_at(&g_tl, onset, P, 1000 + k, g_fired, STORE_CAP), j;
        if (n != cnt[k]) { if (!bad_n) fail_i(__LINE__, "events fired on a grid frame", n, cnt[k]); bad_n++; }
        for (j = 0; j < n && j < STORE_CAP; j++) {
            int src = g_fired[j].code;
            if (src < 0 || src >= NE || quant_k(ev_rt[src], P, w) != k) bad_land++;
            if (j > 0 && report_cmp(&g_fired[j - 1], &g_fired[j]) >= 0) bad_order++;
        }
    }
    for (i = 0; i < NE; i++) {
        const ytl_event* e = ytl_find(&g_tl, ev_id[i]);
        int64_t kk = quant_k(ev_rt[i], P, w), on = T0 + kk * P;
        if (!e || e->frame != 1000 + kk || e->onset != on || e->residual != on - ev_rt[i] ||
            (unsigned)e->flags != YTL_EV_FIRED) {
            if (first_bad < 0) first_bad = i;
            bad_land++;
        }
        /* Nearest frame, except events before the first frame. */
        if (e && kk > 0 && lead_num * 2 == lead_den && (e->residual > P / 2 || e->residual < -P / 2)) bad_land++;
    }
    if (first_bad >= 0) {
        const ytl_event* e = ytl_find(&g_tl, ev_id[first_bad]);
        int64_t kk = quant_k(ev_rt[first_bad], P, w);
        fprintf(stderr, "  quant P %lld lead %d/%d base %d: event rt %lld want frame %lld residual %lld; "
                "got frame %lld onset %lld residual %lld flags %u\n",
                (long long)P, lead_num, lead_den, base, (long long)ev_rt[first_bad], (long long)(1000 + kk),
                (long long)(T0 + kk * P - ev_rt[first_bad]),
                e ? (long long)e->frame : -99, e ? (long long)e->onset : -99,
                e ? (long long)e->residual : -99, e ? (unsigned)e->flags : 0u);
    }
    if (bad_land) fail_i(__LINE__, "grid landings that disagree with the model", bad_land, 0);
    if (bad_order) fail_i(__LINE__, "grid reports out of RT target order", bad_order, 0);
    (void)bad_n;
}

/* Period 0: lead does nothing; first onset at or after the event. */
static void quant_vrr(uint64_t seed) {
    enum { NE = 250, NF = 300 };
    static int64_t on[NF], ev_t[NE];
    static int ev_id[NE];
    int i, k, bad = 0;
    g_rng = seed;
    on[0] = T0;
    for (k = 1; k < NF; k++) on[k] = on[k - 1] + rnd_range(4 * MS_NS, 20 * MS_NS);
    CHECK(open_h(&g_tl, g_store, NE + 4, 1, NULL, 0.5));
    for (i = 0; i < NE; i++) {
        ytl_event e;
        ev_t[i] = rnd_range(T0 - 10 * MS_NS, on[NF - 10]);
        if (i % 7 == 0) ev_t[i] = on[rnd_int(NF - 10)];
        e = ev(0, ev_t[i], YTL_MARK, 0, 0.0f);
        ev_id[i] = ytl_add(&g_tl, &e);
    }
    for (k = 0; k < NF; k++) eval_at(&g_tl, on[k], 0, k, NULL, 0);
    for (i = 0; i < NE; i++) {
        const ytl_event* e = ytl_find(&g_tl, ev_id[i]);
        int want = 0;
        while (on[want] < ev_t[i]) want++;
        if (!e || e->frame != want || e->onset != on[want] || e->residual != on[want] - ev_t[i]) bad++;
    }
    if (bad) fail_i(__LINE__, "period-0 landings that disagree with the model", bad, 0);
}

/* ---------------------------------------------------------------- 3. bases */

static void test_bases(void) {
    static ytl_key kk[2];
    float init[6] = { 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f };
    int64_t t = 0;
    int id_a, id_b, id_s2, k, n, total;

    kk[0] = mkkey(0, 0.0f, YTL_EASE_LINEAR);
    kk[1] = mkkey(S_NS, 1000.0f, YTL_EASE_LINEAR);

    CHECK(open_h(&g_tl, g_store, 32, 6, init, 0.0));
    CHECK(ytl_base_time(&g_tl, 0, 123, &t));
    CHECK_I(t, 123);
    CHECK(!ytl_base_time(&g_tl, 1, 0, &t));
    CHECK(!ytl_base_time(&g_tl, YTL_MAX_BASES, 0, &t));
    CHECK(!ytl_base_time(&g_tl, -1, 0, &t));
    CHECK_I(ytl_anchor(&g_tl, 0, 0, 0), YTL_ERR_ARG);
    CHECK_I(ytl_pause(&g_tl, 0, 0), YTL_ERR_ARG);
    CHECK_I(ytl_stop(&g_tl, 0), YTL_ERR_ARG);
    CHECK_I(ytl_anchor(&g_tl, YTL_MAX_BASES, 0, 0), YTL_ERR_ARG);
    CHECK_I(ytl_anchor(&g_tl, -1, 0, 0), YTL_ERR_ARG);
    CHECK_I(ytl_pause(&g_tl, YTL_MAX_BASES, 0), YTL_ERR_ARG);
    CHECK_I(ytl_resume(&g_tl, YTL_MAX_BASES, 0), YTL_ERR_ARG);
    CHECK_I(ytl_stop(&g_tl, YTL_MAX_BASES), YTL_ERR_ARG);
    /* |base_time| >= 2^62 is rejected. */
    CHECK_I(ytl_anchor(&g_tl, 1, 0, LIM62), YTL_ERR_ARG);
    CHECK_I(ytl_anchor(&g_tl, 1, 0, -LIM62), YTL_ERR_ARG);
    CHECK_I(ytl_anchor(&g_tl, 1, 0, INT64_MAX), YTL_ERR_ARG);
    CHECK(!ytl_base_time(&g_tl, 1, 0, &t));   /* the rejected anchors left it stopped */
    CHECK_I(ytl_anchor(&g_tl, 1, 0, LIM62 - 1), 0);
    CHECK_I(ytl_stop(&g_tl, 1), 0);
    CHECK(ytl_base_time(&g_tl, 0, 5, &t));
    CHECK_I(t, 5);
    CHECK_I(ytl_pause(&g_tl, 1, 0), YTL_ERR_ORDER);
    CHECK_I(ytl_resume(&g_tl, 1, 0), YTL_ERR_ORDER);
    CHECK_I(ytl_anchor(&g_tl, 1, 1000, 50), 0);
    CHECK(ytl_base_time(&g_tl, 1, 1100, &t));
    CHECK_I(t, 150);
    CHECK(ytl_base_time(&g_tl, 1, 900, &t));
    CHECK_I(t, -50);
    CHECK_I(ytl_resume(&g_tl, 1, 1100), YTL_ERR_ORDER);
    CHECK_I(ytl_pause(&g_tl, 1, 1200), 0);
    CHECK(ytl_base_time(&g_tl, 1, 99999, &t));
    CHECK_I(t, 250);
    CHECK_I(ytl_pause(&g_tl, 1, 1300), YTL_ERR_ORDER);
    CHECK_I(ytl_resume(&g_tl, 1, 2000), 0);
    CHECK(ytl_base_time(&g_tl, 1, 2100, &t));
    CHECK_I(t, 350);
    CHECK_I(ytl_pause(&g_tl, 1, 2200), 0);
    CHECK_I(ytl_anchor(&g_tl, 1, 3000, 7), 0);   /* from paused */
    CHECK(ytl_base_time(&g_tl, 1, 3010, &t));
    CHECK_I(t, 17);
    CHECK_I(ytl_stop(&g_tl, 1), 0);
    CHECK(!ytl_base_time(&g_tl, 1, 3010, &t));
    CHECK_I(ytl_pause(&g_tl, 1, 3020), YTL_ERR_ORDER);
    CHECK_I(ytl_resume(&g_tl, 1, 3020), YTL_ERR_ORDER);
    CHECK_I(ytl_anchor(&g_tl, 1, 3000, 7), 0);   /* from stopped */
    CHECK_I(ytl_stop(&g_tl, 1), 0);

    /* A stopped base: nothing fires, no track writes, nothing due. */
    id_a = add1(&g_tl, 2, 0, YTL_ONSET, 0, 0.0f);
    CHECK(id_a >= 0);
    CHECK(add1(&g_tl, 2, -1000, YTL_MARK, 0, 0.0f) >= 0);
    CHECK_I(ytl_set_keys(&g_tl, 1, 2, kk, 2), 0);
    total = 0;
    for (k = 0; k <= 120; k++) total += eval_at(&g_tl, (int64_t)k * P60, P60, k, NULL, 0);
    CHECK_I(total, 0);
    CHECK(ytl_value(&g_tl, 0) == 0.5f);
    CHECK(ytl_value(&g_tl, 1) == 0.5f);
    CHECK(is_pending_reset(ytl_find(&g_tl, id_a)));
    CHECK(!ytl_next_due(&g_tl, &t));

    /* Stopping holds the channels. */
    CHECK(add1(&g_tl, 1, 0, YTL_SET, 2, 5.0f) >= 0);
    CHECK_I(ytl_anchor(&g_tl, 1, 3 * S_NS, 0), 0);
    CHECK_I(eval_at(&g_tl, 3 * S_NS, P60, 1000, NULL, 0), 1);
    CHECK(ytl_value(&g_tl, 2) == 5.0f);
    id_s2 = add1(&g_tl, 1, 10 * MS_NS, YTL_SET, 2, 7.0f);
    CHECK_I(ytl_stop(&g_tl, 1), 0);
    total = 0;
    for (k = 1; k <= 20; k++) total += eval_at(&g_tl, 3 * S_NS + k * P60, P60, 1000 + k, NULL, 0);
    CHECK_I(total, 0);
    CHECK(ytl_value(&g_tl, 2) == 5.0f);
    CHECK(is_pending_reset(ytl_find(&g_tl, id_s2)));
    CHECK_I(ytl_set_keys(&g_tl, 3, 3, kk, 2), 0);
    CHECK_I(ytl_anchor(&g_tl, 3, 3500 * MS_NS, 0), 0);
    eval_at(&g_tl, 3500 * MS_NS, P60, 1100, NULL, 0);
    CHECK(ytl_value(&g_tl, 3) == 0.0f);
    eval_at(&g_tl, 3500 * MS_NS + 10 * P60, P60, 1110, NULL, 0);
    CHECK(ytl_value(&g_tl, 3) == (float)ksample(kk, 2, 10 * P60));
    CHECK_I(ytl_stop(&g_tl, 3), 0);
    eval_at(&g_tl, 3500 * MS_NS + 20 * P60, P60, 1120, NULL, 0);
    CHECK(ytl_value(&g_tl, 3) == (float)ksample(kk, 2, 10 * P60));

    /* A paused base evaluates at its frozen time. */
    CHECK_I(ytl_anchor(&g_tl, 4, 4 * S_NS, 0), 0);
    CHECK_I(eval_at(&g_tl, 4 * S_NS, 0, 2000, NULL, 0), 0);
    CHECK_I(ytl_pause(&g_tl, 4, 4500 * MS_NS), 0);
    CHECK_I(ytl_set_keys(&g_tl, 4, 4, kk, 2), 0);
    id_a = add1(&g_tl, 4, 400 * MS_NS, YTL_MARK, 0, 0.0f);
    id_b = add1(&g_tl, 4, 600 * MS_NS, YTL_MARK, 0, 0.0f);
    n = eval_at(&g_tl, 5 * S_NS, 0, 2001, g_fired, 8);
    CHECK_I(n, 1);
    if (n == 1) {
        CHECK_I(g_fired[0].id, id_a);
        CHECK_I(g_fired[0].residual, 100 * MS_NS);
        CHECK_I(g_fired[0].onset, 5 * S_NS);
        CHECK_I(g_fired[0].frame, 2001);
        CHECK_I(g_fired[0].flags, YTL_EV_FIRED);
    }
    CHECK_F(ytl_value(&g_tl, 4), 500.0, 1e-3);
    CHECK_I(eval_at(&g_tl, 6 * S_NS, 0, 2002, NULL, 0), 0);
    CHECK_F(ytl_value(&g_tl, 4), 500.0, 1e-3);
    CHECK(ytl_base_time(&g_tl, 4, 6 * S_NS, &t));
    CHECK_I(t, 500 * MS_NS);
    CHECK_I(ytl_resume(&g_tl, 4, 7 * S_NS), 0);
    CHECK(ytl_base_time(&g_tl, 4, 7100 * MS_NS, &t));
    CHECK_I(t, 600 * MS_NS);
    n = eval_at(&g_tl, 7100 * MS_NS, 0, 2003, g_fired, 8);
    CHECK_I(n, 1);
    if (n == 1) { CHECK_I(g_fired[0].id, id_b); CHECK_I(g_fired[0].residual, 0); }
    CHECK_F(ytl_value(&g_tl, 4), 600.0, 1e-3);
}

/* ------------------------------------------------------------- 4. channels */

static void test_channels(void) {
    static ytl_key kk[2];
    float init[8];
    ytl_event b[4];
    int i, n, next;

    for (i = 0; i < 8; i++) init[i] = 0.25f;
    kk[0] = mkkey(0, 0.0f, YTL_EASE_LINEAR);
    kk[1] = mkkey(1000, 1.0f, YTL_EASE_LINEAR);
    CHECK(open_h(&g_tl, g_store, 24, 8, init, 0.0));

    CHECK_I(add1(&g_tl, 0, 100, YTL_ONSET, 0, 0.0f), 0);
    CHECK_I(add1(&g_tl, 0, 200, YTL_OFFSET, 0, 0.0f), 1);
    CHECK_I(add1(&g_tl, 0, 150, YTL_SET, 1, 3.5f), 2);
    CHECK_I(eval_at(&g_tl, 50, 0, 0, NULL, 0), 0);
    CHECK(ytl_value(&g_tl, 0) == 0.25f);
    CHECK(ytl_value(&g_tl, 1) == 0.25f);
    CHECK_I(eval_at(&g_tl, 120, 0, 1, NULL, 0), 1);
    CHECK(ytl_value(&g_tl, 0) == 1.0f);
    CHECK_I(eval_at(&g_tl, 160, 0, 2, NULL, 0), 1);
    CHECK(ytl_value(&g_tl, 1) == 3.5f);
    CHECK_I(eval_at(&g_tl, 250, 0, 3, NULL, 0), 1);
    CHECK(ytl_value(&g_tl, 0) == 0.0f);

    /* A late event with an earlier time does not overwrite. */
    CHECK_I(add1(&g_tl, 0, 100, YTL_SET, 1, 7.0f), 3);
    n = eval_at(&g_tl, 260, 0, 4, g_fired, 4);
    CHECK_I(n, 1);
    if (n == 1) {
        CHECK_I(g_fired[0].flags, YTL_EV_FIRED | YTL_EV_LATE);
        CHECK_I(g_fired[0].residual, 160);
    }
    CHECK(ytl_value(&g_tl, 1) == 3.5f);
    /* Same time, later id: it is the latest (time, id). */
    CHECK_I(add1(&g_tl, 0, 150, YTL_SET, 1, 9.0f), 4);
    CHECK_I(eval_at(&g_tl, 270, 0, 5, NULL, 0), 1);
    CHECK(ytl_value(&g_tl, 1) == 9.0f);
    CHECK_I(add1(&g_tl, 0, 149, YTL_SET, 1, 11.0f), 5);
    CHECK_I(eval_at(&g_tl, 280, 0, 6, NULL, 0), 1);
    CHECK(ytl_value(&g_tl, 1) == 9.0f);

    /* The payload comes back unchanged. */
    b[0] = ev(0, 290, YTL_MARK, -7, 1.5f);
    b[0].code = 42;
    b[0].user = UINT64_C(0x0123456789ABCDEF);
    CHECK_I(ytl_add(&g_tl, &b[0]), 6);
    CHECK_I(add1(&g_tl, 0, 295, 200, 12345, -2.0f), 7);
    n = eval_at(&g_tl, 300, 0, 7, g_fired, 4);
    CHECK_I(n, 2);
    if (n == 2) {
        CHECK_I(g_fired[0].id, 6);
        CHECK_I(g_fired[0].target, -7);
        CHECK_I(g_fired[0].code, 42);
        CHECK(g_fired[0].user == UINT64_C(0x0123456789ABCDEF));
        CHECK(g_fired[0].value == 1.5f);
        CHECK_I(g_fired[0].kind, YTL_MARK);
        CHECK_I(g_fired[1].id, 7);
        CHECK_I(g_fired[1].kind, 200);
        CHECK_I(g_fired[1].target, 12345);
    }

    /* Validation. */
    CHECK_I(add1(&g_tl, 0, 0, 5, 0, 0.0f), YTL_ERR_ARG);
    CHECK_I(add1(&g_tl, 0, 0, 15, 0, 0.0f), YTL_ERR_ARG);
    CHECK_I(add1(&g_tl, 0, 1000, 16, 0, 0.0f), 8);
    CHECK_I(add1(&g_tl, 0, 1000, 255, 0, 0.0f), 9);
    CHECK_I(add1(&g_tl, 0, 0, YTL_ONSET, 8, 0.0f), YTL_ERR_ARG);
    CHECK_I(add1(&g_tl, 0, 0, YTL_ONSET, -1, 0.0f), YTL_ERR_ARG);
    CHECK_I(add1(&g_tl, 0, 0, YTL_SET, 3, (float)make_nan()), YTL_ERR_ARG);
    CHECK_I(add1(&g_tl, 0, 0, YTL_SET, 3, (float)make_inf()), YTL_ERR_ARG);
    CHECK_I(add1(&g_tl, YTL_MAX_BASES, 0, YTL_MARK, 0, 0.0f), YTL_ERR_ARG);
    CHECK_I(add1(&g_tl, 255, 0, YTL_MARK, 0, 0.0f), YTL_ERR_ARG);
    CHECK_I(add1(&g_tl, 0, 1000, YTL_TRIGGER, 999, 0.0f), 10);   /* any trigger channel >= 0 */
    CHECK_I(add1(&g_tl, 0, 1000, YTL_TRIGGER, -1, 0.0f), YTL_ERR_ARG); /* v0.8.1 */

    /* Binding: two bases, events vs keys both ways. */
    CHECK_I(add1(&g_tl, 1, 0, YTL_ONSET, 0, 0.0f), YTL_ERR_BOUND);
    CHECK_I(add1(&g_tl, 2, 0, YTL_SET, 1, 1.0f), YTL_ERR_BOUND);
    CHECK_I(ytl_set_keys(&g_tl, 0, 0, kk, 2), YTL_ERR_BOUND);
    CHECK_I(ytl_set_keys(&g_tl, 2, 1, kk, 2), 0);
    CHECK_I(add1(&g_tl, 1, 0, YTL_ONSET, 2, 0.0f), YTL_ERR_BOUND);
    CHECK_I(add1(&g_tl, 0, 0, YTL_ONSET, 2, 0.0f), YTL_ERR_BOUND);
    /* set_keys replaces keys on any base and rebinds the channel. */
    CHECK_I(ytl_set_keys(&g_tl, 2, 2, kk, 2), 0);
    CHECK_I(add1(&g_tl, 2, 0, YTL_ONSET, 2, 0.0f), YTL_ERR_BOUND);
    CHECK_I(ytl_anchor(&g_tl, 2, 400, 500), 0);   /* base 1 stays stopped */
    CHECK_I(eval_at(&g_tl, 400, 0, 8, NULL, 0), 0);
    CHECK_F(ytl_value(&g_tl, 2), 0.5, 1e-6);       /* written by base 2 */
    CHECK_I(ytl_stop(&g_tl, 2), 0);

    /* add_n: all or none. */
    n = n_events(&g_tl, YTL_ALL_BASES);
    b[0] = ev(0, 1000, YTL_MARK, 0, 0.0f);
    b[1] = ev(1, 1000, YTL_ONSET, 3, 0.0f);
    b[2] = ev(2, 1000, YTL_OFFSET, 3, 0.0f);
    CHECK_I(ytl_add_n(&g_tl, b, 3), YTL_ERR_BOUND);
    CHECK_I(n_events(&g_tl, YTL_ALL_BASES), n);
    b[1] = ev(0, 1000, 7, 0, 0.0f);
    CHECK_I(ytl_add_n(&g_tl, b, 2), YTL_ERR_ARG);
    b[1] = ev(0, 1000, YTL_SET, 3, (float)make_nan());
    CHECK_I(ytl_add_n(&g_tl, b, 2), YTL_ERR_ARG);
    b[1] = ev(1, 1000, YTL_ONSET, 0, 0.0f);   /* ch0 is base 0's */
    CHECK_I(ytl_add_n(&g_tl, b, 2), YTL_ERR_BOUND);
    CHECK_I(n_events(&g_tl, YTL_ALL_BASES), n);
    CHECK(ytl_add_n(&g_tl, b, -1) < 0);
    CHECK_I(ytl_add_n(&g_tl, b, 0), 11);   /* failed adds consumed no ids */
    b[0] = ev(1, 1000, YTL_ONSET, 3, 0.0f);
    b[1] = ev(1, 2000, YTL_OFFSET, 3, 0.0f);
    b[2] = ev(3, 5, YTL_MARK, 0, 0.0f);
    CHECK_I(ytl_add_n(&g_tl, b, 3), 11);
    CHECK(ytl_find(&g_tl, 11) && ytl_find(&g_tl, 11)->time_ns == 1000 && ytl_find(&g_tl, 11)->kind == YTL_ONSET);
    CHECK(ytl_find(&g_tl, 12) && ytl_find(&g_tl, 12)->time_ns == 2000 && ytl_find(&g_tl, 12)->kind == YTL_OFFSET);
    CHECK(ytl_find(&g_tl, 13) && ytl_find(&g_tl, 13)->base == 3 && ytl_find(&g_tl, 13)->time_ns == 5);
    CHECK_I(add1(&g_tl, 0, 3000, YTL_MARK, 0, 0.0f), 14);
    CHECK(is_pending_reset(ytl_find(&g_tl, 14)));
    check_sorted(&g_tl, __LINE__);

    /* FULL. */
    next = 15;
    while (n_events(&g_tl, YTL_ALL_BASES) < 24) {
        CHECK_I(add1(&g_tl, 0, 4000, YTL_MARK, 0, 0.0f), next);
        next++;
        if (next > 40) break;
    }
    CHECK_I(add1(&g_tl, 0, 4000, YTL_MARK, 0, 0.0f), YTL_ERR_FULL);
    CHECK_I(ytl_remove(&g_tl, 15), 0);
    b[0] = ev(0, 1, YTL_MARK, 0, 0.0f);
    b[1] = ev(0, 2, YTL_MARK, 0, 0.0f);
    CHECK_I(ytl_add_n(&g_tl, b, 2), YTL_ERR_FULL);
    CHECK_I(n_events(&g_tl, YTL_ALL_BASES), 23);
    CHECK_I(ytl_add_n(&g_tl, b, 1), next);
    check_sorted(&g_tl, __LINE__);
}

/* --------------------------------------------------------------- 5. tracks */

static void test_sample(void) {
    static ytl_key k[16];
    const int64_t M = MS_NS;
    int64_t t;
    int i, j, n, bad = 0;

    k[0] = mkkey(0, 1.0f, YTL_EASE_LINEAR);
    k[1] = mkkey(100 * M, 3.0f, YTL_EASE_STEP);
    k[2] = mkkey(200 * M, 5.0f, YTL_EASE_COSINE);
    k[3] = mkkey(300 * M, 1.0f, YTL_EASE_QUAD_IN);
    k[4] = mkkey(400 * M, 9.0f, YTL_EASE_QUAD_OUT);
    k[5] = mkkey(500 * M, 2.0f, YTL_EASE_LOG);
    k[6] = mkkey(600 * M, 32.0f, YTL_EASE_LINEAR);
    for (t = -50 * M; t <= 700 * M; t += 7 * M + 1) {
        double want = ref_sample(k, 7, t), got = ksample(k, 7, t);
        if (!(fabs(got - want) <= tol_of(want))) bad++;
    }
    if (bad) fail_i(__LINE__, "ytl_sample points off the model", bad, 0);
    for (i = 0; i < 7; i++) CHECK(ksample(k, 7, k[i].time_ns) == (double)k[i].value);
    CHECK_F(ksample(k, 7, 50 * M), 2.0, 1e-6);
    CHECK_F(ksample(k, 7, 150 * M), 3.0, 1e-6);
    CHECK_F(ksample(k, 7, 199 * M), 3.0, 1e-6);
    CHECK_F(ksample(k, 7, 250 * M), 3.0, 1e-6);
    CHECK_F(ksample(k, 7, 350 * M), 3.0, 1e-6);
    CHECK_F(ksample(k, 7, 450 * M), 3.75, 1e-6);
    CHECK_F(ksample(k, 7, 550 * M), 8.0, 1e-6);
    CHECK(ksample(k, 7, -1) == 1.0);
    CHECK(ksample(k, 7, INT64_C(1000000000000000000)) == 32.0);
    CHECK(ksample(NULL, 3, 5) == 0.0);
    CHECK(ksample(k, 0, 5) == 0.0);
    /* n < 0 is not a track set_track accepts, and ytl_sample does no
     * validation, so v0.1.0's check of it is gone. */
    CHECK(ksample(k, 1, -5) == 1.0);
    CHECK(ksample(k, 1, 5) == 1.0);

    /* Equal times: a jump; at the shared time the later key. */
    k[0] = mkkey(10, 0.0f, YTL_EASE_LINEAR);
    k[1] = mkkey(20, 1.0f, YTL_EASE_LINEAR);
    k[2] = mkkey(20, 5.0f, YTL_EASE_LINEAR);
    k[3] = mkkey(30, 6.0f, YTL_EASE_LINEAR);
    CHECK_F(ksample(k, 4, 19), 0.9, 1e-6);
    CHECK(ksample(k, 4, 20) == 5.0);
    CHECK_F(ksample(k, 4, 25), 5.5, 1e-6);
    CHECK(ksample(k, 4, 30) == 6.0);
    CHECK(ksample(k, 4, 9) == 0.0);
    CHECK(ksample(k, 3, 20) == 5.0);   /* jump at the end */
    CHECK_F(ksample(k, 3, 19), 0.9, 1e-6);

    /* Random tracks of every easing. */
    g_rng = 77;
    bad = 0;
    for (i = 0; i < 300; i++) {
        n = 1 + rnd_int(10);
        t = rnd_range(-100 * M, 100 * M);
        for (j = 0; j < n; j++) {
            if (j > 0 && rnd_int(7) != 0) t += rnd_range(1, 50 * M);
            k[j] = mkkey(t, (float)(0.1 + (double)rnd_int(1000) / 100.0), rnd_int(6));
        }
        for (j = 0; j < 60 + n; j++) {
            int64_t ts = j < n ? k[j].time_ns : rnd_range(k[0].time_ns - 10 * M, k[n - 1].time_ns + 10 * M);
            double want = ref_sample(k, n, ts), got = ksample(k, n, ts);
            if (!(fabs(got - want) <= tol_of(want))) {
                if (!bad) fprintf(stderr, "  sample: n %d t %lld got %.9g want %.9g\n",
                                  n, (long long)ts, got, want);
                bad++;
            }
        }
    }
    if (bad) fail_i(__LINE__, "random ytl_sample points off the model", bad, 0);
}

static void test_tracks(void) {
    static ytl_key ramp[2], bad[3], d[12];
    float init[4] = { 0.75f, 0.25f, -2.0f, 0.0f };
    int64_t onset, bt, prev;
    int k, i, mism = 0;

    CHECK(open_h(&g_tl, g_store, 16, 4, init, 0.5));

    /* Rejections. */
    bad[0] = mkkey(10, 1.0f, YTL_EASE_LINEAR); bad[1] = mkkey(5, 2.0f, YTL_EASE_LINEAR);
    CHECK_I(ytl_set_keys(&g_tl, 0, 1, bad, 2), YTL_ERR_ARG);
    bad[0] = mkkey(0, 1.0f, NOT_EASE); bad[1] = mkkey(10, 2.0f, YTL_EASE_LINEAR);
    CHECK_I(ytl_set_keys(&g_tl, 0, 1, bad, 2), YTL_ERR_ARG);
    bad[0] = mkkey(0, (float)make_nan(), YTL_EASE_LINEAR);
    CHECK_I(ytl_set_keys(&g_tl, 0, 1, bad, 2), YTL_ERR_ARG);
    bad[0] = mkkey(0, 1.0f, YTL_EASE_LINEAR); bad[1] = mkkey(10, (float)make_inf(), YTL_EASE_LINEAR);
    CHECK_I(ytl_set_keys(&g_tl, 0, 1, bad, 2), YTL_ERR_ARG);
    bad[0] = mkkey(0, 0.0f, YTL_EASE_LOG); bad[1] = mkkey(10, 1.0f, YTL_EASE_LINEAR);
    CHECK_I(ytl_set_keys(&g_tl, 0, 1, bad, 2), YTL_ERR_ARG);
    bad[0] = mkkey(0, 1.0f, YTL_EASE_LOG); bad[1] = mkkey(10, -1.0f, YTL_EASE_LINEAR);
    CHECK_I(ytl_set_keys(&g_tl, 0, 1, bad, 2), YTL_ERR_ARG);
    bad[1] = mkkey(10, 0.0f, YTL_EASE_LINEAR);
    CHECK_I(ytl_set_keys(&g_tl, 0, 1, bad, 2), YTL_ERR_ARG);
    bad[0] = mkkey(0, 1.0f, YTL_EASE_LINEAR); bad[1] = mkkey(10, 2.0f, YTL_EASE_LINEAR);
    CHECK_I(ytl_set_keys(&g_tl, -1, 1, bad, 2), YTL_ERR_ARG);
    CHECK_I(ytl_set_keys(&g_tl, 4, 1, bad, 2), YTL_ERR_ARG);
    CHECK_I(ytl_set_keys(&g_tl, 0, YTL_MAX_BASES, bad, 2), YTL_ERR_ARG);
    CHECK_I(ytl_set_keys(&g_tl, 0, -1, bad, 2), YTL_ERR_ARG);
    CHECK_I(ytl_set_keys(&g_tl, 0, 1, bad, -1), YTL_ERR_ARG);
    CHECK_I(ytl_set_keys(&g_tl, 0, 1, NULL, 2), YTL_ERR_ARG);
    /* Literal: "The last key's ease is not used", so a LOG on the last key
     * with a negative value is no LOG segment. */
    bad[0] = mkkey(0, 1.0f, YTL_EASE_LINEAR); bad[1] = mkkey(10, -1.0f, YTL_EASE_LOG);
    CHECK_I(ytl_set_keys(&g_tl, 3, 2, bad, 2), 0);
    CHECK_I(ytl_set_keys(&g_tl, 3, 2, NULL, 0), 0);
    /* The rejections left channel 0 free. */
    CHECK_I(ytl_set_keys(&g_tl, 0, 2, bad, 2), 0);
    CHECK_I(ytl_set_keys(&g_tl, 0, 2, NULL, 0), 0);
    CHECK(ytl_value(&g_tl, 0) == 0.75f);

    /* The track writes at the base time of the onset; lead does not apply. */
    ramp[0] = mkkey(0, 0.0f, YTL_EASE_LINEAR);
    ramp[1] = mkkey(S_NS, 1000.0f, YTL_EASE_LINEAR);
    CHECK_I(ytl_set_keys(&g_tl, 0, 1, ramp, 2), 0);
    CHECK(ytl_value(&g_tl, 0) == 0.75f);   /* changes at the next evaluate */
    CHECK_I(ytl_anchor(&g_tl, 1, 5 * S_NS, 0), 0);
    for (k = 0; k <= 70; k++) {
        onset = 5 * S_NS + k * P60;
        bt = k * P60;
        eval_at(&g_tl, onset, P60, k, NULL, 0);
        if (ytl_value(&g_tl, 0) != (float)ksample(ramp, 2, bt) ||
            !(fabs(ytl_value(&g_tl, 0) - ref_sample(ramp, 2, bt)) <= tol_of(ref_sample(ramp, 2, bt)))) {
            if (!mism) fprintf(stderr, "  ramp: bt %lld got %.9g want %.9g\n", (long long)bt,
                               (double)ytl_value(&g_tl, 0), ref_sample(ramp, 2, bt));
            mism++;
        }
    }
    if (mism) fail_i(__LINE__, "ramp values off the base time of the onset", mism, 0);
    onset = 5 * S_NS + 70 * P60;
    CHECK_I(ytl_set_keys(&g_tl, 0, 1, NULL, 0), 0);
    CHECK(ytl_value(&g_tl, 0) == 0.75f);   /* at once */
    eval_at(&g_tl, onset + P60, P60, 71, NULL, 0);
    CHECK(ytl_value(&g_tl, 0) == 0.75f);

    /* Forward-moving and jumping sequences agree with ytl_sample. */
    g_rng = 99;
    bt = 0;
    for (i = 0; i < 12; i++) {
        if (i > 0) bt += rnd_int(5) == 0 ? 0 : rnd_range(1, 250 * MS_NS);
        d[i] = mkkey(bt, (float)(0.5 + (double)rnd_int(400) / 40.0), rnd_int(6));
    }
    CHECK_I(ytl_set_keys(&g_tl, 1, 2, d, 12), 0);
    onset += 2 * P60;
    CHECK_I(ytl_anchor(&g_tl, 2, onset, -50 * MS_NS), 0);
    mism = 0;
    prev = onset;
    for (k = 0; k < 600; k++) {
        int64_t t = 0;
        onset = prev + rnd_range(0, P60);
        prev = onset;
        eval_at(&g_tl, onset, P60, 100 + k, NULL, 0);
        CHECK(ytl_base_time(&g_tl, 2, onset, &t));
        if (ytl_value(&g_tl, 1) != (float)ksample(d, 12, t)) mism++;
    }
    for (k = 0; k < 300; k++) {
        int64_t t = rnd_range(-100 * MS_NS, d[11].time_ns + 100 * MS_NS);
        onset = prev + rnd_range(1, P60);
        prev = onset;
        CHECK_I(ytl_anchor(&g_tl, 2, onset, t), 0);
        eval_at(&g_tl, onset, P60, 1000 + k, NULL, 0);
        if (ytl_value(&g_tl, 1) != (float)ksample(d, 12, t)) mism++;
        if (!(fabs(ytl_value(&g_tl, 1) - ref_sample(d, 12, t)) <= 1e-5 * (1.0 + fabs(ref_sample(d, 12, t))))) mism++;
    }
    if (mism) fail_i(__LINE__, "keyed channel values off ytl_sample", mism, 0);
}

/* ------------------------------------------------- 5b. sampled tracks */

static float g_big[600000];
static float g_sv[8192];
static int g_cv_prints;

/* A channel's value against the track it stands for, read at base time bt:
 * ytl_sample's value stored as float, and the model's within float
 * rounding. Returns 1 on a mismatch. */
static int chan_vs(int ch, const ytl_track* tr, int64_t bt) {
    float v = ytl_value(&g_tl, ch), s = (float)ytl_sample(tr, bt);
    double want = ref_track(tr, bt);
    if (v != s || !(fabs((double)v - want) <= tol_of(want))) {
        if (g_cv_prints++ < 8)
            fprintf(stderr, "  channel %d at base time %lld: got %.9g, ytl_sample %.9g, model %.9g\n",
                    ch, (long long)bt, (double)v, (double)s, want);
        return 1;
    }
    return 0;
}

/* The base time a track on base b is read at for a frame at `onset` whose
 * lead window is w ns (TRACKS): the window's end for a STEP track, the
 * onset otherwise. A paused base gives its frozen time either way. */
static int64_t read_bt(int b, const ytl_track* tr, int64_t onset, int64_t w) {
    int64_t t = 0;
    (void)ytl_base_time(&g_tl, b, trk_is_step(tr) ? onset + w : onset, &t);
    return t;
}

/* The first whole ns at or after sample i's exact time t0 + i 1e9 / rate. */
static int64_t samp_ceil(int64_t t0, int64_t i, int64_t rate) {
    return t0 + (i * NS_E9 + rate - 1) / rate;
}

static int samp_exact(int64_t i, int64_t rate) { return (i * NS_E9) % rate == 0; }

/* Placement, read through STEP on a table whose sample i is i: sample i
 * shows from the first whole ns at or after its exact time and not 1 ns
 * before. A step rounded to whole ns drifts off this within a few samples
 * at any rate that does not divide 1e9. */
static void sampled_place(int64_t rate, int64_t t0, int n) {
    ytl_track st = trk_samples(g_big, n, t0, rate, YTL_INTERP_STEP);
    ytl_track li = trk_samples(g_big, n, t0, rate, YTL_INTERP_LINEAR);
    int64_t i, last = samp_ceil(t0, n - 1, rate);
    int bad = 0, bad_m = 0, bad_x = 0, f0 = g_failures;
    for (i = 1; i < n; i++) {
        int64_t e = samp_ceil(t0, i, rate);
        double a = ytl_sample(&st, e), b = ytl_sample(&st, e - 1);
        if (a != (double)i || b != (double)(i - 1)) {
            if (!bad) fprintf(stderr, "  sample %lld (first ns %lld): got %.9g there, %.9g 1 ns before\n",
                              (long long)i, (long long)e, a, b);
            bad++;
        }
        if (a != ref_track(&st, e) || b != ref_track(&st, e - 1)) bad_m++;
        if (samp_exact(i, rate) && ytl_sample(&li, e) != (double)i) bad_x++;
    }
    if (bad) fail_i(__LINE__, "STEP samples off t0 + i 1e9 / rate", bad, 0);
    if (bad_m) fail_i(__LINE__, "STEP samples off the model", bad_m, 0);
    if (bad_x) fail_i(__LINE__, "LINEAR not exact on a sample at a whole ns", bad_x, 0);
    /* Before t0 the first sample; from the last sample's time on, the last. */
    CHECK(ytl_sample(&st, t0 - 1) == 0.0);
    CHECK(ytl_sample(&li, t0 - 1) == 0.0);
    CHECK(ytl_sample(&li, t0 - 10 * S_NS) == 0.0);
    CHECK(ytl_sample(&li, t0) == 0.0);
    CHECK(ytl_sample(&li, last) == (double)(n - 1));
    CHECK(ytl_sample(&st, last) == (double)(n - 1));
    CHECK(ytl_sample(&li, last + 1) == (double)(n - 1));
    CHECK(ytl_sample(&li, last + 1000 * S_NS) == (double)(n - 1));
    CHECK(ytl_sample(&li, last - 1) < (double)(n - 1));
    CHECK(ytl_sample(&st, last - 1) == (double)(n - 2));
    if (g_failures != f0)
        fprintf(stderr, "  in sampled_place(rate %lld, t0 %lld, n %d)\n", (long long)rate, (long long)t0, n);
}

/* LINEAR, STEP and CUBIC against the model at random times, on samples,
 * 1 ns before them, half way, and next to both ends. */
static void sampled_interp(int64_t rate, int64_t t0, int n, uint64_t seed) {
    int64_t last = samp_ceil(t0, n - 1, rate), step = NS_E9 / rate + 1;
    int interp, j, f0 = g_failures;
    g_rng = seed;
    for (j = 0; j < n; j++) g_sv[j] = (float)(rnd_int(2001) - 1000) / 64.0f;
    for (interp = 0; interp < 3; interp++) {
        ytl_track tr = trk_samples(g_sv, n, t0, rate, interp);
        int bad = 0, bad_on = 0;
        for (j = 0; j < 5000; j++) {
            int64_t i = rnd_range(0, n - 1), t;
            double got, want;
            switch (j % 5) {
            case 0: t = rnd_range(t0 - S_NS, last + S_NS); break;
            case 1: t = samp_ceil(t0, i, rate); break;
            case 2: t = samp_ceil(t0, i, rate) - 1; break;
            case 3: t = t0 + (2 * i + 1) * NS_E9 / (2 * rate); break;
            default:
                t = rnd_int(2) ? t0 + rnd_range(-2, 2 * step + 2) : last + rnd_range(-2 * step - 2, 2);
                break;
            }
            got = ytl_sample(&tr, t);
            want = ref_track(&tr, t);
            if (!(fabs(got - want) <= tol_d(want))) {
                if (!bad) fprintf(stderr, "  interp %d t %lld: got %.17g want %.17g\n", interp, (long long)t, got, want);
                bad++;
            }
            if (j % 5 == 1 && samp_exact(i, rate) && got != (double)g_sv[i]) bad_on++;
        }
        if (bad) fail_i(__LINE__, "sampled values off the model", bad, 0);
        if (bad_on) fail_i(__LINE__, "sampled value not exact on a sample", bad_on, 0);
    }
    if (g_failures != f0)
        fprintf(stderr, "  in sampled_interp(rate %lld, t0 %lld, n %d)\n", (long long)rate, (long long)t0, n);
}

static void test_sampled(void) {
    /* 120, 7 and 999983 (prime) do not divide 1e9; 1e9 is a sample per ns. */
    static const int64_t rates[6] = { 120, 7, 999983, INT64_C(1000000000), 1, 60 };
    static const int ns[6] = { 600000, 3000, 200000, 200000, 100, 5000 };
    static const int64_t t0s[3] = { 0, -5 * YRT_S(1) + 3, 777777777 };
    int r, z, i;
    for (i = 0; i < 600000; i++) g_big[i] = (float)i;
    for (r = 0; r < 6; r++)
        for (z = 0; z < 3; z++) sampled_place(rates[r], t0s[z], ns[r]);
    for (r = 0; r < 6; r++)
        for (z = 0; z < 3; z++)
            sampled_interp(rates[r], t0s[z], ns[r] < 8192 ? ns[r] : 8192, 1000 + 10 * (uint64_t)r + (uint64_t)z);
}

static void test_sampled_cubic(void) {
    static float q[4] = { 0.0f, 1.0f, 4.0f, 9.0f }, two[2] = { 3.0f, -5.0f }, one[1] = { 2.5f };
    const int64_t T = -S_NS, M = MS_NS;
    ytl_track c = trk_samples(q, 4, T, 10, YTL_INTERP_CUBIC), l;
    int64_t t;
    int interp, bad = 0;

    /* By hand: v = i^2, which Catmull-Rom reproduces inside; the linear
     * phantoms (-1 before, 14 after) bend the end segments: 0.375 and
     * 6.375, where repeated ends would give 0.3125 and 6.3125. */
    CHECK_F(ytl_sample(&c, T + 50 * M), 0.375, 1e-12);
    CHECK_F(ytl_sample(&c, T + 150 * M), 2.25, 1e-12);
    CHECK_F(ytl_sample(&c, T + 250 * M), 6.375, 1e-12);
    CHECK(ytl_sample(&c, T + 100 * M) == 1.0);
    CHECK(ytl_sample(&c, T + 200 * M) == 4.0);
    CHECK(ytl_sample(&c, T + 300 * M) == 9.0);
    CHECK(ytl_sample(&c, T - 1) == 0.0);
    CHECK(ytl_sample(&c, T + 10 * S_NS) == 9.0);
    /* The model gives the same by-hand values. */
    CHECK_F(ref_track(&c, T + 50 * M), 0.375, 1e-12);
    CHECK_F(ref_track(&c, T + 150 * M), 2.25, 1e-12);
    CHECK_F(ref_track(&c, T + 250 * M), 6.375, 1e-12);

    /* Two samples: CUBIC is LINEAR. */
    c = trk_samples(two, 2, T, 3, YTL_INTERP_CUBIC);
    l = trk_samples(two, 2, T, 3, YTL_INTERP_LINEAR);
    for (t = T - 10; t <= T + S_NS / 3 + 10; t += 1234567) {
        double a = ytl_sample(&c, t), b = ytl_sample(&l, t);
        if (!(fabs(a - b) <= 1e-12 * (1.0 + fabs(b)))) bad++;
    }
    for (t = T + S_NS / 3 - 3; t <= T + S_NS / 3 + 3; t++)
        if (!(fabs(ytl_sample(&c, t) - ytl_sample(&l, t)) <= 1e-12)) bad++;
    if (bad) fail_i(__LINE__, "two-sample CUBIC differs from LINEAR", bad, 0);
    /* T + S / 6 is 166666666 ns: u = 0.499999998, not 0.5. */
    CHECK_F(ytl_sample(&c, T + S_NS / 6), 3.0 - 8.0 * 0.499999998, 1e-12);

    /* One sample: that value everywhere. */
    for (interp = 0; interp < 3; interp++) {
        ytl_track s = trk_samples(one, 1, T, 7, interp);
        CHECK(ytl_sample(&s, T - 1) == 2.5);
        CHECK(ytl_sample(&s, T) == 2.5);
        CHECK(ytl_sample(&s, T + 1) == 2.5);
        CHECK(ytl_sample(&s, T + 100 * S_NS) == 2.5);
    }
}

/* USAGE's 10-minute target and more: 600000 samples at 120/s (83 min). */
static void test_long_table(void) {
    const int N = 600000;
    const int64_t rate = 120, t0 = -7 * S_NS + 3, R = S_NS, P = 2 * MS_NS;
    int64_t last, t, onset, idx = 0, bt0;
    int i, interp, k, bad = 0, bad_e = 0;
    ytl_track tr;

    for (i = 0; i < N; i++)
        g_big[i] = (float)(40.0 * sin(0.0131 * (double)i) + 25.0 * cos(0.00377 * (double)i + 1.0) +
                           3.0 * sin(0.21 * (double)i));
    last = samp_ceil(t0, N - 1, rate);
    g_rng = 4242;
    for (interp = 0; interp < 3; interp++) {
        tr = trk_samples(g_big, N, t0, rate, interp);
        for (k = 0; k < 20000; k++) {
            double got, want;
            t = rnd_range(t0 - S_NS, last + S_NS);
            got = ytl_sample(&tr, t);
            want = ref_track(&tr, t);
            if (!(fabs(got - want) <= tol_d(want))) {
                if (!bad) fprintf(stderr, "  long table interp %d t %lld: got %.17g want %.17g\n",
                                  interp, (long long)t, got, want);
                bad++;
            }
        }
        for (t = t0 - S_NS; t <= last + S_NS; t += 7777777) {
            double got = ytl_sample(&tr, t), want = ref_track(&tr, t);
            if (!(fabs(got - want) <= tol_d(want))) {
                if (!bad) fprintf(stderr, "  long table interp %d t %lld: got %.17g want %.17g\n",
                                  interp, (long long)t, got, want);
                bad++;
            }
        }
    }
    if (bad) fail_i(__LINE__, "long-table values off the model", bad, 0);

    /* Through evaluate: forward at 500 Hz from before t0, forward past the
     * end, then jumps. */
    tr = trk_samples(g_big, N, t0, rate, YTL_INTERP_CUBIC);
    CHECK(open_h(&g_tl, g_store, 4, 1, NULL, 0.5));
    CHECK_I(ytl_set_track(&g_tl, 0, 1, &tr), 0);
    bt0 = t0 - 100 * MS_NS;
    CHECK_I(ytl_anchor(&g_tl, 1, R, bt0), 0);
    onset = R;
    for (k = 0; k < 30000; k++, onset += P) {
        eval_at(&g_tl, onset, P, idx++, NULL, 0);
        bad_e += chan_vs(0, &tr, bt0 + (onset - R));
    }
    bt0 = last - 30 * S_NS;
    CHECK_I(ytl_anchor(&g_tl, 1, onset, bt0), 0);
    for (k = 0; k < 30000; k++, onset += P) {
        eval_at(&g_tl, onset, P, idx++, NULL, 0);
        bad_e += chan_vs(0, &tr, bt0 + (int64_t)k * P);
    }
    CHECK(ytl_value(&g_tl, 0) == g_big[N - 1]);
    for (k = 0; k < 3000; k++, onset += P) {
        bt0 = rnd_range(t0 - S_NS, last + S_NS);
        CHECK_I(ytl_anchor(&g_tl, 1, onset, bt0), 0);
        eval_at(&g_tl, onset, P, idx++, NULL, 0);
        bad_e += chan_vs(0, &tr, bt0);
    }
    if (bad_e) fail_i(__LINE__, "long-table channel values off ytl_sample", bad_e, 0);
}

/* ------------------------------------------------------- 5c. REPEAT */

static void test_repeat(void) {
    static ytl_key k[4];
    static float sv[10];
    const int64_t M = MS_NS, ST = 1000, T = -3 * S_NS;
    ytl_track tr;
    int i, bad = 0, bad_hi = 0, rep;

    /* Keyed: 0 -> 10 over 100 ms, 10 until 150 ms, then 4 toward 20 at
     * 400 ms; period 200 ms. The value at start + period is
     * 4 + 16 * 50 / 250 = 7.2; the key at 400 ms is never reached. */
    k[0] = mkkey(ST, 0.0f, YTL_EASE_LINEAR);
    k[1] = mkkey(ST + 100 * M, 10.0f, YTL_EASE_STEP);
    k[2] = mkkey(ST + 150 * M, 4.0f, YTL_EASE_LINEAR);
    k[3] = mkkey(ST + 400 * M, 20.0f, YTL_EASE_LINEAR);
    tr = trk_keys(k, 4);
    tr.period_ns = 200 * M;
    tr.repeats = 3;
    /* Before the start, no repeat: the first key's value. */
    CHECK(ytl_sample(&tr, ST - 1) == 0.0);
    CHECK(ytl_sample(&tr, -S_NS) == 0.0);
    CHECK(ytl_sample(&tr, ST) == 0.0);
    CHECK_F(ytl_sample(&tr, ST + 50 * M), 5.0, 1e-12);
    CHECK(ytl_sample(&tr, ST + 120 * M) == 10.0);
    CHECK_F(ytl_sample(&tr, ST + 199 * M), 4.0 + 16.0 * 49.0 / 250.0, 1e-9);
    CHECK(ytl_sample(&tr, ST + 200 * M) == 0.0);   /* the second cycle */
    CHECK_F(ytl_sample(&tr, ST + 250 * M), 5.0, 1e-12);
    CHECK(ytl_sample(&tr, ST + 520 * M) == 10.0);
    CHECK_F(ytl_sample(&tr, ST + 600 * M - 1), 4.0 + 16.0 * (double)(50 * M - 1) / (double)(250 * M), 1e-9);
    /* After 3 cycles: the value at start + period, read without folding. */
    CHECK_F(ytl_sample(&tr, ST + 600 * M), 7.2, 1e-6);
    CHECK_F(ytl_sample(&tr, ST + 600 * M + 1), 7.2, 1e-6);
    CHECK_F(ytl_sample(&tr, ST + 10 * S_NS), 7.2, 1e-6);
    CHECK_F(ytl_sample(&tr, INT64_C(1000000000000000)), 7.2, 1e-6);
    CHECK_F(ref_track(&tr, ST + 600 * M), 7.2, 1e-6);
    CHECK_F(ref_track(&tr, ST + 10 * S_NS), 7.2, 1e-6);
    tr.repeats = 1;
    CHECK_F(ytl_sample(&tr, ST + 200 * M - 1), 4.0 + 16.0 * (double)(50 * M - 1) / (double)(250 * M), 1e-9);
    CHECK_F(ytl_sample(&tr, ST + 200 * M), 7.2, 1e-6);
    CHECK_F(ref_track(&tr, ST + 200 * M), 7.2, 1e-6);
    /* repeats 0: forever. */
    tr.repeats = 0;
    CHECK_F(ytl_sample(&tr, ST + 1000 * 200 * M + 50 * M), 5.0, 1e-12);
    CHECK(ytl_sample(&tr, ST + INT64_C(123456789) * 200 * M + 120 * M) == 10.0);
    CHECK(ytl_sample(&tr, ST - 1) == 0.0);
    /* Random times, three counts; a key past start + period never shows. */
    g_rng = 31337;
    for (rep = 0; rep < 3; rep++) {
        tr.repeats = rep == 0 ? 0 : rep == 1 ? 3 : 1;
        for (i = 0; i < 5000; i++) {
            int64_t t = i % 2 ? rnd_range(ST - S_NS, ST + 2 * S_NS) : rnd_range(ST, ST + INT64_C(1000000000000));
            double got = ytl_sample(&tr, t), want = ref_track(&tr, t);
            if (!(fabs(got - want) <= tol_d(want))) {
                if (!bad) fprintf(stderr, "  repeat keyed t %lld: got %.17g want %.17g\n", (long long)t, got, want);
                bad++;
            }
            if (got > 10.0) bad_hi++;
        }
    }
    if (bad) fail_i(__LINE__, "repeated keyed values off the model", bad, 0);
    if (bad_hi) fail_i(__LINE__, "a key past start + period was reached", bad_hi, 0);

    /* Sampled, sample i = i at 100/s from t0 = -3 s; period 55 ms, which is
     * not a whole number of samples. The value at start + period is 5.5. */
    for (i = 0; i < 10; i++) sv[i] = (float)i;
    tr = trk_samples(sv, 10, T, 100, YTL_INTERP_LINEAR);
    tr.period_ns = 55 * M;
    tr.repeats = 4;
    CHECK(ytl_sample(&tr, T - 1) == 0.0);
    CHECK_F(ytl_sample(&tr, T + 23 * M), 2.3, 1e-9);
    CHECK_F(ytl_sample(&tr, T + 55 * M + 23 * M), 2.3, 1e-9);
    CHECK(ytl_sample(&tr, T + 2 * 55 * M) == 0.0);
    CHECK_F(ytl_sample(&tr, T + 3 * 55 * M + 54 * M), 5.4, 1e-9);
    CHECK_F(ytl_sample(&tr, T + 4 * 55 * M - 1), 5.4999999, 1e-9);
    CHECK_F(ytl_sample(&tr, T + 4 * 55 * M), 5.5, 1e-9);
    CHECK_F(ytl_sample(&tr, T + 4 * 55 * M + 1), 5.5, 1e-9);
    CHECK_F(ytl_sample(&tr, T + 100 * S_NS), 5.5, 1e-9);
    CHECK_F(ref_track(&tr, T + 4 * 55 * M), 5.5, 1e-9);
    tr.repeats = 0;
    CHECK_F(ytl_sample(&tr, T + 1000 * 55 * M + 23 * M), 2.3, 1e-9);
    /* A period longer than the table: the last sample until the wrap. */
    tr.period_ns = 200 * M;
    CHECK(ytl_sample(&tr, T + 150 * M) == 9.0);
    CHECK_F(ytl_sample(&tr, T + 200 * M + 45 * M), 4.5, 1e-9);
    tr.repeats = 2;
    CHECK(ytl_sample(&tr, T + 400 * M) == 9.0);
    CHECK(ytl_sample(&tr, T + 400 * M - 1) == 9.0);
    CHECK_F(ytl_sample(&tr, T + 200 * M + 1), 1e-7, 1e-12);
    /* CUBIC and STEP with a repeat, at random. */
    bad = 0;
    for (rep = 0; rep < 4; rep++) {
        tr = trk_samples(sv, 10, T, 100, rep % 2 ? YTL_INTERP_CUBIC : YTL_INTERP_STEP);
        tr.period_ns = rep < 2 ? 37 * M + 1 : 333 * M;
        tr.repeats = rep;
        for (i = 0; i < 5000; i++) {
            int64_t t = rnd_range(T - 50 * M, T + 3 * S_NS);
            double got = ytl_sample(&tr, t), want = ref_track(&tr, t);
            if (!(fabs(got - want) <= tol_d(want))) {
                if (!bad) fprintf(stderr, "  repeat sampled t %lld: got %.17g want %.17g\n", (long long)t, got, want);
                bad++;
            }
        }
    }
    if (bad) fail_i(__LINE__, "repeated sampled values off the model", bad, 0);
}

/* USAGE's 7.5 Hz flicker for 10 minutes then off, through evaluate at a
 * display rate of hz, with frames at the exact multiples of 1/hz rounded
 * to ns and then moved by up to +-jit ns, as a predicted onset is. It is a
 * STEP track, so it is read at the end of the lead window: each frame
 * against the square wave there. At 60 Hz the edges fall on frame onsets;
 * with lock set, frame k must show on exactly when k mod 8 < 4. */
static int64_t flicker_run(int64_t hz, int lead_num, int lead_den, int64_t bt_first, int64_t jit, int lock) {
    static ytl_key sq[2];
    const int64_t R = S_NS, P = (NS_E9 + hz / 2) / hz, w = P * lead_num / lead_den;
    const int64_t FP = 4 * S_NS / 30, HALF = 2 * S_NS / 30, END = 4500 * FP;
    ytl_track tr;
    int64_t k, bad = 0, bad_s = 0, bad_m = 0, bad_lock = 0, n_on = 0, n_off = 0;
    int f0 = g_failures;

    sq[0] = mkkey(0, 1.0f, YTL_EASE_STEP);
    sq[1] = mkkey(HALF, 0.0f, YTL_EASE_STEP);
    tr = trk_keys(sq, 2);
    tr.period_ns = FP;
    tr.repeats = 4500;
    CHECK(open_h(&g_tl, g_store, 4, 1, NULL, (double)lead_num / (double)lead_den));
    CHECK_I(ytl_set_track(&g_tl, 0, 1, &tr), 0);
    CHECK_I(ytl_anchor(&g_tl, 1, R, bt_first), 0);
    for (k = 0; ; k++) {
        int64_t onset = R + (k * NS_E9 + hz / 2) / hz + (jit ? rnd_range(-jit, jit) : 0);
        int64_t rd = bt_first + (onset - R) + w;   /* the window's end on the base */
        float v, want;
        if (rd > END + S_NS) break;
        eval_at(&g_tl, onset, P, k, NULL, 0);
        v = ytl_value(&g_tl, 0);
        want = rd < 0 ? 1.0f : rd >= END ? 0.0f : (rd % FP < HALF ? 1.0f : 0.0f);
        if (v != want) {
            if (!bad) fprintf(stderr, "  flicker frame %lld read at %lld: got %g want %g\n",
                              (long long)k, (long long)rd, (double)v, (double)want);
            bad++;
        }
        if (v != (float)ytl_sample(&tr, rd)) bad_s++;
        if (v != (float)ref_track(&tr, rd)) bad_m++;
        if (lock && rd < END - P && v != ((k % 8) < 4 ? 1.0f : 0.0f)) bad_lock++;
        if (v != 0.0f) n_on++; else n_off++;
    }
    if (bad) fail_i(__LINE__, "flicker frames off the square wave at the window's end", bad, 0);
    if (bad_s) fail_i(__LINE__, "flicker frames off ytl_sample at the window's end", bad_s, 0);
    if (bad_m) fail_i(__LINE__, "flicker frames off the model", bad_m, 0);
    if (lock && bad_lock) fail_i(__LINE__, "60 Hz flicker frames off frame k mod 8 < 4", bad_lock, 0);
    if (n_on < 1000 || n_off < 1000) fail_i(__LINE__, "flicker: too few on or off frames", n_on < n_off ? n_on : n_off, 1000);
    if (g_failures != f0)
        fprintf(stderr, "  in flicker_run(%lld Hz, lead %d/%d, first base time %lld, jitter %lld)\n",
                (long long)hz, lead_num, lead_den, (long long)bt_first, (long long)jit);
    return bad_lock;
}

static void test_flicker(void) {
    int64_t wrong;
    g_rng = 7575;
    (void)flicker_run(60, 1, 2, 0, 1000, 1);
    (void)flicker_run(60, 1, 2, 0, 0, 1);
    (void)flicker_run(144, 1, 4, 0, 300, 0);
    (void)flicker_run(240, 1, 2, -37 * MS_NS, 300, 0);
    (void)flicker_run(500, 0, 1, -37 * MS_NS, 100, 0);
    /* Lead 0 reads the STEP track at the onset too, so with onset noise
     * the frames next to an edge fall on either side of it (the manual's
     * reason for reading at the window's end). Not a failure of the
     * header: a check that lead 0 does read at the onset. */
    wrong = 0;
    {
        static ytl_key sq[2];
        const int64_t FP = 4 * S_NS / 30, HALF = 2 * S_NS / 30;
        ytl_track tr;
        int64_t k;
        sq[0] = mkkey(0, 1.0f, YTL_EASE_STEP);
        sq[1] = mkkey(HALF, 0.0f, YTL_EASE_STEP);
        tr = trk_keys(sq, 2);
        tr.period_ns = FP;
        CHECK(open_h(&g_tl, g_store, 4, 1, NULL, 0.0));
        CHECK_I(ytl_set_track(&g_tl, 0, 1, &tr), 0);
        CHECK_I(ytl_anchor(&g_tl, 1, S_NS, 0), 0);
        for (k = 0; k < 36000; k++) {
            int64_t onset = S_NS + (k * NS_E9 + 30) / 60 + rnd_range(-1000, 1000);
            eval_at(&g_tl, onset, P60, k, NULL, 0);
            if (ytl_value(&g_tl, 0) != ((k % 8) < 4 ? 1.0f : 0.0f)) wrong++;
            if (ytl_value(&g_tl, 0) != (float)ytl_sample(&tr, onset - S_NS)) {
                fail(__LINE__, "lead 0: STEP track not read at the onset");
                break;
            }
        }
    }
    if (wrong == 0) fail(__LINE__, "lead 0 with onset noise: no frame off the frame-locked wave (vacuous)");
}

/* -------------------------------------------------------- 5d. BEZIER */

static const ytl_curve g_bz[10] = {
    { 0.0f, 0.0f, 1.0f, 1.0f },     /* CSS linear: y(s) = x(s)            */
    { 0.42f, 0.0f, 0.58f, 1.0f },   /* CSS ease-in-out                    */
    { 0.25f, 0.1f, 0.25f, 1.0f },   /* CSS ease                           */
    { 0.68f, -0.6f, 0.32f, 1.6f },  /* overshoots both keys               */
    { 0.0f, 1.0f, 1.0f, 0.0f },     /* x at the limits                    */
    { 1.0f, 0.0f, 0.0f, 1.0f },     /* x at the limits, the other way     */
    { 0.0f, 0.0f, 0.0f, 0.0f },     /* y(s) = x(s) = s^3                  */
    { 1.0f, 1.0f, 1.0f, 1.0f },     /* y(s) = x(s), x flat at s = 1       */
    { 0.0f, 0.5f, 0.0f, 1.0f },     /* x = s^3: steep at the start        */
    { 1.0f, 0.0f, 1.0f, 0.3f },     /* x flat at the end                  */
};

static void test_bezier(void) {
    static ytl_key k[2], k2[2];
    int c, j, bad = 0, bad_lin = 0, bad2 = 0;
    double ymin = 1.0, ymax = 0.0;
    g_rng = 606;
    for (c = 0; c < 10; c++) {
        ytl_track tr, tr2;
        k[0] = mkbez(0, 0.0f, c);
        k[1] = mkkey(S_NS, 1.0f, YTL_EASE_LINEAR);
        k2[0] = mkbez(0, 2.0f, c);
        k2[1] = mkkey(S_NS, -3.0f, YTL_EASE_LINEAR);
        tr = trk_keys(k, 2);
        tr.curves = g_bz;
        tr.n_curves = 10;
        tr2 = tr;
        tr2.keys = k2;
        CHECK(ytl_sample(&tr, 0) == 0.0);
        CHECK(ytl_sample(&tr, S_NS) == 1.0);
        CHECK(ytl_sample(&tr2, -1) == 2.0);
        for (j = 0; j <= 3000; j++) {
            int64_t t = j <= 1000 ? (int64_t)j * (S_NS / 1000) : j <= 1100 ? (int64_t)(j - 1000) : rnd_range(0, S_NS - 1);
            double u = (double)t / 1e9, got = ytl_sample(&tr, t), got2 = ytl_sample(&tr2, t), lo, hi;
            /* "The y is found to 1e-12 in x": any y within that, with a
             * little room for rounding in x. */
            bez_range(&g_bz[c], u, 1.5e-12, &lo, &hi);
            if (!(got >= lo - 1e-12 && got <= hi + 1e-12)) {
                if (!bad) fprintf(stderr, "  bezier %d u %.17g: got %.17g, y in [%.17g, %.17g]\n", c, u, got, lo, hi);
                bad++;
            }
            if (!(got2 >= 2.0 - 5.0 * hi - 1e-11 && got2 <= 2.0 - 5.0 * lo + 1e-11)) bad2++;
            if ((c == 0 || c == 6 || c == 7) && !(fabs(got - u) <= 1e-11)) {
                if (!bad_lin) fprintf(stderr, "  bezier %d (linear) u %.17g: got %.17g\n", c, u, got);
                bad_lin++;
            }
            if (c == 3) {
                if (got < ymin) ymin = got;
                if (got > ymax) ymax = got;
            }
        }
    }
    if (bad) fail_i(__LINE__, "BEZIER y off the bisection model", bad, 0);
    if (bad2) fail_i(__LINE__, "BEZIER value off v0 + (v1 - v0) y", bad2, 0);
    if (bad_lin) fail_i(__LINE__, "cubic-bezier(0,0,1,1) and y = x curves differ from LINEAR", bad_lin, 0);
    CHECK(ymin < -0.05);
    CHECK(ymax > 1.05);
    /* By hand: ease-in-out is 0.5 at u = 0.5, near enough: 0.42f + 0.58f
     * is not 1 in float, so the curve is not quite symmetric. */
    k[0] = mkbez(0, 0.0f, 1);
    k[1] = mkkey(S_NS, 1.0f, YTL_EASE_LINEAR);
    {
        ytl_track tr = trk_keys(k, 2);
        tr.curves = g_bz;
        tr.n_curves = 10;
        CHECK_F(ytl_sample(&tr, S_NS / 2), 0.5, 1e-6);
        CHECK_F(ref_track(&tr, S_NS / 2), 0.5, 1e-6);
        /* And the overshoot curve below 0 early, as y1 < 0 makes it. */
        k[0] = mkbez(0, 0.0f, 3);
        CHECK(ytl_sample(&tr, S_NS / 5) < 0.0);
        CHECK(ref_track(&tr, S_NS / 5) < 0.0);
        CHECK_F(ref_track(&tr, S_NS / 5), ytl_sample(&tr, S_NS / 5), 1e-9);
    }
}

/* ------------------------------------------------- 5e. set_track rules */

#define REJ_TRACK(trv) CHECK_I(ytl_set_track(&g_tl, 0, 1, &(trv)), YTL_ERR_ARG)
#define ACC_TRACK(trv) CHECK_I(ytl_set_track(&g_tl, 3, 1, &(trv)), 0)

static void test_set_track(void) {
    static ytl_key k[3], kb[3];
    static float s[4] = { 1.0f, 2.0f, 3.0f, 4.0f }, sb[4];
    static ytl_curve cv[2], cb[2];
    float init[4] = { 0.5f, -0.5f, 2.0f, 0.0f };
    ytl_track gk, gs, b;
    ytl_tween_desc td;
    int id;

    CHECK(open_h(&g_tl, g_store, 16, 4, init, 0.0));
    k[0] = mkkey(0, 1.0f, YTL_EASE_LINEAR);
    k[1] = mkkey(10, 2.0f, YTL_EASE_LINEAR);
    k[2] = mkkey(20, 3.0f, YTL_EASE_LINEAR);
    cv[0] = mkcurve(0.42f, 0.0f, 0.58f, 1.0f);
    cv[1] = mkcurve(0.25f, 0.1f, 0.25f, 1.0f);
    gk = trk_keys(k, 3);
    gs = trk_samples(s, 4, 0, 100, YTL_INTERP_LINEAR);
    ACC_TRACK(gk);   /* a keyed track's rate 0 is not a sampled rate */
    ACC_TRACK(gs);

    /* Neither form, or both. */
    memset(&b, 0, sizeof b); REJ_TRACK(b);
    b = gk; b.keys = NULL; REJ_TRACK(b);
    b = gk; b.n_keys = 0; REJ_TRACK(b);        /* literal: no keys is not the keyed form */
    b = gs; b.samples = NULL; REJ_TRACK(b);
    b = gs; b.n_samples = 0; REJ_TRACK(b);
    b = gk; b.samples = s; b.n_samples = 4; b.rate = 100; REJ_TRACK(b);
    /* Negative counts. */
    b = gk; b.n_keys = -1; REJ_TRACK(b);
    b = gs; b.n_samples = -1; REJ_TRACK(b);
    b = gk; b.curves = cv; b.n_curves = -1; REJ_TRACK(b);
    /* Keys: order, ease, value, LOG. */
    b = trk_keys(kb, 3);
    memcpy(kb, k, sizeof kb); kb[2].time_ns = 5; REJ_TRACK(b);
    memcpy(kb, k, sizeof kb); kb[0].ease = NOT_EASE; REJ_TRACK(b);
    memcpy(kb, k, sizeof kb); kb[1].ease = 255; REJ_TRACK(b);
    memcpy(kb, k, sizeof kb); kb[2].ease = NOT_EASE; REJ_TRACK(b);   /* the last key's too */
    memcpy(kb, k, sizeof kb); kb[0].value = (float)make_nan(); REJ_TRACK(b);
    memcpy(kb, k, sizeof kb); kb[1].value = (float)-make_inf(); REJ_TRACK(b);
    memcpy(kb, k, sizeof kb); kb[2].value = (float)make_inf(); REJ_TRACK(b);
    memcpy(kb, k, sizeof kb); kb[0].ease = YTL_EASE_LOG; kb[0].value = 0.0f; REJ_TRACK(b);
    memcpy(kb, k, sizeof kb); kb[0].ease = YTL_EASE_LOG; kb[1].value = -1.0f; REJ_TRACK(b);
    memcpy(kb, k, sizeof kb); kb[1].ease = YTL_EASE_LOG; kb[2].value = 0.0f; REJ_TRACK(b);
    memcpy(kb, k, sizeof kb); kb[2].ease = YTL_EASE_LOG; kb[2].value = -1.0f; ACC_TRACK(b);
    /* Samples. */
    b = gs; b.samples = sb;
    memcpy(sb, s, sizeof sb); sb[0] = (float)make_nan(); REJ_TRACK(b);
    memcpy(sb, s, sizeof sb); sb[3] = (float)make_inf(); REJ_TRACK(b);
    memcpy(sb, s, sizeof sb); sb[1] = (float)-make_inf(); REJ_TRACK(b);
    memcpy(sb, s, sizeof sb); ACC_TRACK(b);
    /* Curves: y finite, x in [0, 1]. */
    memcpy(kb, k, sizeof kb);
    kb[0] = mkbez(0, 1.0f, 1);
    b = trk_keys(kb, 3);
    b.curves = cb;
    b.n_curves = 2;
    memcpy(cb, cv, sizeof cb); ACC_TRACK(b);
    memcpy(cb, cv, sizeof cb); cb[1].y1 = (float)make_nan(); REJ_TRACK(b);
    memcpy(cb, cv, sizeof cb); cb[1].y2 = (float)make_inf(); REJ_TRACK(b);
    memcpy(cb, cv, sizeof cb); cb[1].x1 = -0.25f; REJ_TRACK(b);
    memcpy(cb, cv, sizeof cb); cb[1].x2 = 1.25f; REJ_TRACK(b);
    memcpy(cb, cv, sizeof cb); cb[1].x1 = (float)make_nan(); REJ_TRACK(b);
    memcpy(cb, cv, sizeof cb); cb[1].x2 = 1.0000001f; REJ_TRACK(b);
    memcpy(cb, cv, sizeof cb); cb[1].x1 = -1e-30f; REJ_TRACK(b);
    memcpy(cb, cv, sizeof cb); cb[1].y1 = -3.0f; cb[1].y2 = 4.0f; ACC_TRACK(b);   /* y may leave [0, 1] */
    memcpy(cb, cv, sizeof cb); cb[1].x1 = 0.0f; cb[1].x2 = 1.0f; ACC_TRACK(b);
    memcpy(cb, cv, sizeof cb); cb[1].x1 = 1.0f; cb[1].x2 = 0.0f; ACC_TRACK(b);
    /* Literal: the rule names any curve, so curve 0, which no key uses,
     * is checked too. */
    memcpy(cb, cv, sizeof cb); cb[0].x1 = 2.0f; REJ_TRACK(b);
    memcpy(cb, cv, sizeof cb); cb[0].y2 = (float)make_nan(); REJ_TRACK(b);
    /* Curve index below n_curves, on every key but the last. */
    memcpy(cb, cv, sizeof cb);
    kb[0] = mkbez(0, 1.0f, 2); REJ_TRACK(b);
    kb[0] = mkbez(0, 1.0f, 1); kb[1] = mkbez(10, 2.0f, 5); REJ_TRACK(b);
    kb[1] = mkkey(10, 2.0f, YTL_EASE_LINEAR); kb[2] = mkbez(20, 3.0f, 99); ACC_TRACK(b);
    kb[2] = mkkey(20, 3.0f, YTL_EASE_LINEAR);
    b.curves = NULL; b.n_curves = 0; REJ_TRACK(b);   /* index 1 is not below 0 */
    /* n_curves > 0 with curves NULL, no BEZIER key. */
    b = gk; b.n_curves = 1; REJ_TRACK(b);
    /* Rate. */
    b = gs; b.rate = 0; REJ_TRACK(b);
    b = gs; b.rate = -1; REJ_TRACK(b);
    b = gs; b.rate = NS_E9 + 1; REJ_TRACK(b);
    b = gs; b.rate = 1; ACC_TRACK(b);
    b = gs; b.rate = NS_E9; ACC_TRACK(b);
    /* A last sample at or past 2^62, its time rounded up to a whole ns.
     * gs has 4 samples: at rate 1 the last is t0 + 3 s; at rate 3 it is
     * t0 + 1 s exactly; at rate 7 it is t0 + 3/7 s = t0 + 428571428.57 ns,
     * which rounds up to 428571429. */
    b = gs; b.rate = 1; b.t0_ns = LIM62 - 3 * S_NS; REJ_TRACK(b);              /* last at 2^62         */
    b = gs; b.rate = 1; b.t0_ns = LIM62 - 3 * S_NS - 1; ACC_TRACK(b);          /* last at 2^62 - 1 ns  */
    b = gs; b.rate = 3; b.t0_ns = LIM62 - S_NS; REJ_TRACK(b);
    b = gs; b.rate = 3; b.t0_ns = LIM62 - S_NS - 1; ACC_TRACK(b);
    b = gs; b.rate = 7; b.t0_ns = LIM62 - 428571429; REJ_TRACK(b);             /* rounds up to 2^62    */
    b = gs; b.rate = 7; b.t0_ns = LIM62 - 428571430; ACC_TRACK(b);
    /* Interp. */
    b = gs; b.interp = 3; REJ_TRACK(b);
    b = gs; b.interp = -1; REJ_TRACK(b);
    b = gs; b.interp = YTL_INTERP_CUBIC; ACC_TRACK(b);
    /* Period and repeats. */
    b = gk; b.period_ns = -1; REJ_TRACK(b);
    b = gs; b.period_ns = -5; REJ_TRACK(b);
    b = gk; b.period_ns = LIM62; REJ_TRACK(b);
    b = gk; b.period_ns = LIM62 - 1; ACC_TRACK(b);
    b = gk; b.repeats = -1; REJ_TRACK(b);
    b = gk; b.period_ns = 10; b.repeats = -1; REJ_TRACK(b);
    b = gk; b.repeats = 2; REJ_TRACK(b);   /* repeats > 0 with period 0 */
    b = gk; b.period_ns = 10; b.repeats = 2; ACC_TRACK(b);
    /* Times beyond +-2^62. */
    b = trk_keys(kb, 3);
    memcpy(kb, k, sizeof kb); kb[2].time_ns = LIM62 + 1; REJ_TRACK(b);
    memcpy(kb, k, sizeof kb); kb[0].time_ns = -LIM62 - 1; REJ_TRACK(b);
    b = gs; b.t0_ns = -LIM62 - 1; REJ_TRACK(b);
    b = gs; b.t0_ns = LIM62 + 1; REJ_TRACK(b);
    /* Channel and base. */
    CHECK_I(ytl_set_track(&g_tl, 4, 1, &gk), YTL_ERR_ARG);
    CHECK_I(ytl_set_track(&g_tl, -1, 1, &gk), YTL_ERR_ARG);
    CHECK_I(ytl_set_track(&g_tl, 0, YTL_MAX_BASES, &gk), YTL_ERR_ARG);
    CHECK_I(ytl_set_track(&g_tl, 0, -1, &gk), YTL_ERR_ARG);

    /* The rejections left channel 0 free, at its initial value. */
    CHECK(ytl_value(&g_tl, 0) == 0.5f);
    id = add1(&g_tl, 5, 0, YTL_SET, 0, 1.0f);
    CHECK(id >= 0);
    CHECK_I(ytl_remove(&g_tl, id), 0);
    /* A rejected set_track leaves the track it would have replaced. */
    CHECK_I(ytl_set_track(&g_tl, 1, 1, &gk), 0);
    b = gs; b.interp = 3;
    CHECK_I(ytl_set_track(&g_tl, 1, 1, &b), YTL_ERR_ARG);
    CHECK_I(ytl_anchor(&g_tl, 1, S_NS, 15), 0);
    eval_at(&g_tl, S_NS, 0, 0, NULL, 0);
    CHECK_F(ytl_value(&g_tl, 1), 2.5, 1e-6);
    /* NULL removes at once: the initial value, and the channel is free. */
    CHECK_I(ytl_set_track(&g_tl, 1, 1, NULL), 0);
    CHECK(ytl_value(&g_tl, 1) == -0.5f);
    eval_at(&g_tl, S_NS + 10, 0, 1, NULL, 0);
    CHECK(ytl_value(&g_tl, 1) == -0.5f);
    id = add1(&g_tl, 2, 0, YTL_SET, 1, 1.0f);
    CHECK(id >= 0);
    /* A channel with events takes no track and no tween. */
    CHECK_I(ytl_set_track(&g_tl, 1, 2, &gs), YTL_ERR_BOUND);
    CHECK_I(ytl_set_track(&g_tl, 1, 3, &gk), YTL_ERR_BOUND);
    td = twd_at(1.0f, 10, 0);
    CHECK_I(ytl_tween(&g_tl, 1, 2, &td), YTL_ERR_BOUND);
    CHECK_I(ytl_remove(&g_tl, id), 0);
    CHECK_I(ytl_set_track(&g_tl, 1, 2, &gs), 0);   /* free again */
    /* A tracked channel takes no event, on its base or another. */
    CHECK_I(add1(&g_tl, 2, 0, YTL_SET, 1, 1.0f), YTL_ERR_BOUND);
    CHECK_I(add1(&g_tl, 0, 0, YTL_ONSET, 1, 0.0f), YTL_ERR_BOUND);
    /* A tween replaces the track, on another base (which stops the track
     * at once), and binds the same way. */
    CHECK_I(ytl_tween(&g_tl, 1, 3, &td), 0);
    CHECK_I(add1(&g_tl, 3, 0, YTL_SET, 1, 1.0f), YTL_ERR_BOUND);
    CHECK_I(add1(&g_tl, 2, 0, YTL_SET, 1, 1.0f), YTL_ERR_BOUND);
    CHECK_I(ytl_set_track(&g_tl, 1, 3, NULL), 0);
    CHECK(ytl_value(&g_tl, 1) == -0.5f);
    CHECK(add1(&g_tl, 3, 0, YTL_SET, 1, 1.0f) >= 0);
    /* clear frees a channel with a track: initial value, free. */
    CHECK_I(ytl_set_track(&g_tl, 2, 4, &gs), 0);
    CHECK_I(ytl_anchor(&g_tl, 4, S_NS + 20, 25 * MS_NS), 0);
    eval_at(&g_tl, S_NS + 20, 0, 2, NULL, 0);
    CHECK_F(ytl_value(&g_tl, 2), 3.5, 1e-6);
    CHECK_I(ytl_clear(&g_tl, 4), 0);
    CHECK(ytl_value(&g_tl, 2) == 2.0f);
    CHECK(add1(&g_tl, 5, 0, YTL_SET, 2, 1.0f) >= 0);
    /* set_keys is set_track with keys only: same value. */
    CHECK_I(ytl_set_keys(&g_tl, 0, 1, k, 3), 0);
    eval_at(&g_tl, S_NS + 30, 0, 3, NULL, 0);
    CHECK_F(ytl_value(&g_tl, 0), ksample(k, 3, 15 + 30), 1e-6);
}

/* ----------------------------------------------------------- 5f. tweens */

static int64_t g_tw_R;
static int64_t g_tw_frame;

static void tw_frame(int64_t bt) {
    eval_at(&g_tl, g_tw_R + bt, P60, g_tw_frame++, NULL, 0);
}

/* A channel's value against a model driver read at base time bt, within
 * float rounding. Returns 1 on a mismatch. */
static int chk_drv(int ch, const rdrv* r, int64_t bt) {
    double want = rdrv_value(r, bt), got = (double)ytl_value(&g_tl, ch);
    if (!(fabs(got - want) <= tol_of(want))) {
        if (g_cv_prints++ < 8)
            fprintf(stderr, "  channel %d at base time %lld: got %.9g, model %.9g\n", ch, (long long)bt, got, want);
        return 1;
    }
    return 0;
}

/* Tweens from a function whose frame is gone before the evaluate, with the
 * desc on that frame (its inline curve included) changed after the call. */
static void tween_from_helper(int ch, int base, int64_t start, int64_t dur, float to) {
    ytl_tween_desc d = twd_at(to, dur, start);
    volatile ytl_tween_desc* vd = &d;
    d.ease = YTL_EASE_BEZIER;
    d.curve = mkcurve(0.42f, 0.0f, 0.58f, 1.0f);
    CHECK_I(ytl_tween(&g_tl, ch, base, &d), 0);
    vd->curve.x1 = 1.0f;
    vd->curve.y1 = 5.0f;
    vd->curve.x2 = 0.0f;
    vd->curve.y2 = -5.0f;
    vd->to = -7.0f;
    vd->duration_ns = 1;
    vd->start_ns = 0;
}

static void stack_scribble(void) {
    volatile unsigned char buf[8192];
    int i;
    for (i = 0; i < (int)sizeof buf; i++) buf[i] = (unsigned char)(0xA5 ^ i);
}

static void test_tween(void) {
    static ytl_key tk[2];
    static ytl_curve tc;
    const int64_t M = MS_NS;
    float init[6] = { 0.25f, 0.0f, 0.5f, 0.0f, 0.0f, 0.0f };
    ytl_track tr;
    ytl_tween_desc d;
    rdrv r1, r2;
    int64_t bt;
    int bad;

    g_cv_prints = 0;
    CHECK(open_h(&g_tl, g_store, 16, 6, init, 0.5));
    g_tw_R = S_NS;
    g_tw_frame = 0;
    CHECK_I(ytl_anchor(&g_tl, 1, g_tw_R, 0), 0);

    /* A channel with no track starts from its value (THE TAKEOVER), held
     * until the start, and shows `to` from start + duration on. */
    d = twd_at(1.25f, 200 * M, 100 * M);
    CHECK_I(ytl_tween(&g_tl, 0, 1, &d), 0);
    CHECK(ytl_value(&g_tl, 0) == 0.25f);
    tr = tween_track(tk, &tc, 100 * M, 200 * M, 0.25f, 1.25f, YTL_EASE_LINEAR, NULL);
    bad = 0;
    for (bt = 0; bt <= 400 * M; bt += 5 * M) {
        tw_frame(bt);
        bad += chan_vs(0, &tr, bt);
        if (bt == 95 * M || bt == 100 * M) CHECK(ytl_value(&g_tl, 0) == 0.25f);
        if (bt == 200 * M) CHECK(ytl_value(&g_tl, 0) == 0.75f);
        if (bt == 300 * M || bt == 400 * M) CHECK(ytl_value(&g_tl, 0) == 1.25f);
    }
    if (bad) fail_i(__LINE__, "tween values off the model", bad, 0);

    /* A tween posted during another: the first runs on until the second's
     * start, and the second starts from the first's value there,
     * 10 x 550 / 1000 = 5.5, not from the 4.72 the last frame before the
     * call showed (the v0.1.0 rule). */
    d = twd_at(10.0f, 1000 * M, 400 * M);
    CHECK_I(ytl_tween(&g_tl, 1, 1, &d), 0);
    rdrv_tween(&r1, &d, 400 * M, NULL, 0.0, 0.0);
    for (bt = 404 * M; bt <= 872 * M; bt += 4 * M) tw_frame(bt);
    CHECK_F(ytl_value(&g_tl, 1), 4.72, 1e-5);
    d = twd_at(0.0f, 200 * M, 950 * M);
    d.ease = YTL_EASE_COSINE;
    CHECK_I(ytl_tween(&g_tl, 1, 1, &d), 0);
    rdrv_tween(&r2, &d, 950 * M, &r1, 0.0, (double)ytl_value(&g_tl, 1));
    bad = 0;
    for (bt = 900 * M; bt <= 1300 * M; bt += 5 * M) {
        tw_frame(bt);
        bad += chk_drv(1, bt < 950 * M ? &r1 : &r2, bt);
        if (bt == 920 * M) CHECK_F(ytl_value(&g_tl, 1), 5.2, 1e-5);   /* the first, running on */
        if (bt == 950 * M) CHECK_F(ytl_value(&g_tl, 1), 5.5, 1e-5);
        if (bt == 1050 * M) CHECK_F(ytl_value(&g_tl, 1), 2.75, 1e-5);
    }
    CHECK(ytl_value(&g_tl, 1) == 0.0f);
    if (bad) fail_i(__LINE__, "tween during a tween off the model", bad, 0);

    /* Duration 0 jumps at start. */
    d = twd_at(3.0f, 0, 1350 * M);
    CHECK_I(ytl_tween(&g_tl, 2, 1, &d), 0);
    tw_frame(1310 * M);
    CHECK(ytl_value(&g_tl, 2) == 0.5f);
    tw_frame(1350 * M - 1);
    CHECK(ytl_value(&g_tl, 2) == 0.5f);
    tw_frame(1350 * M);
    CHECK(ytl_value(&g_tl, 2) == 3.0f);
    tw_frame(1351 * M);
    CHECK(ytl_value(&g_tl, 2) == 3.0f);

    /* LOG needs from_set, even on a channel whose value is > 0, and both
     * ends > 0. */
    d = twd_at(2.0f, 0, 1355 * M);
    CHECK_I(ytl_tween(&g_tl, 3, 1, &d), 0);
    tw_frame(1360 * M);
    CHECK(ytl_value(&g_tl, 3) == 2.0f);
    d = twd_at(8.0f, 100 * M, 1400 * M);
    d.ease = YTL_EASE_LOG;
    CHECK_I(ytl_tween(&g_tl, 3, 1, &d), YTL_ERR_ARG);   /* no from_set */
    d.from_set = true;
    d.from = 2.0f;
    d.to = 0.0f;
    CHECK_I(ytl_tween(&g_tl, 3, 1, &d), YTL_ERR_ARG);
    d.to = -1.0f;
    CHECK_I(ytl_tween(&g_tl, 3, 1, &d), YTL_ERR_ARG);
    d.to = 8.0f;
    d.from = 0.0f;
    CHECK_I(ytl_tween(&g_tl, 3, 1, &d), YTL_ERR_ARG);
    d.from = -2.0f;
    CHECK_I(ytl_tween(&g_tl, 3, 1, &d), YTL_ERR_ARG);
    d.from = 2.0f;
    CHECK_I(ytl_tween(&g_tl, 3, 1, &d), 0);
    tr = tween_track(tk, &tc, 1400 * M, 100 * M, 2.0f, 8.0f, YTL_EASE_LOG, NULL);
    bad = 0;
    for (bt = 1380 * M; bt <= 1550 * M; bt += 2 * M) {
        tw_frame(bt);
        bad += chan_vs(3, &tr, bt);
        if (bt == 1450 * M) CHECK_F(ytl_value(&g_tl, 3), 4.0, 1e-5);
    }
    if (bad) fail_i(__LINE__, "LOG tween off the model", bad, 0);

    /* BEZIER needs a valid inline curve, and the desc is copied. */
    d = twd_at(2.0f, 300 * M, 1600 * M);
    d.ease = YTL_EASE_BEZIER;
    d.curve = mkcurve(1.5f, 0.0f, 0.5f, 1.0f);
    CHECK_I(ytl_tween(&g_tl, 4, 1, &d), YTL_ERR_ARG);
    d.curve = mkcurve(0.5f, (float)make_nan(), 0.5f, 1.0f);
    CHECK_I(ytl_tween(&g_tl, 4, 1, &d), YTL_ERR_ARG);
    tween_from_helper(4, 1, 1600 * M, 300 * M, 2.0f);
    stack_scribble();
    d.curve = mkcurve(0.42f, 0.0f, 0.58f, 1.0f);
    tr = tween_track(tk, &tc, 1600 * M, 300 * M, 0.0f, 2.0f, YTL_EASE_BEZIER, &d.curve);
    /* Other rejections leave the waiting tween in place. */
    d = twd_at(2.0f, 300 * M, 1600 * M);
    d.ease = NOT_EASE;
    CHECK_I(ytl_tween(&g_tl, 4, 1, &d), YTL_ERR_ARG);
    d = twd_at((float)make_nan(), 300 * M, 1600 * M);
    CHECK_I(ytl_tween(&g_tl, 4, 1, &d), YTL_ERR_ARG);
    d = twd_at((float)make_inf(), 300 * M, 1600 * M);
    CHECK_I(ytl_tween(&g_tl, 4, 1, &d), YTL_ERR_ARG);
    d = twd_at(2.0f, -1, 1600 * M);
    CHECK_I(ytl_tween(&g_tl, 4, 1, &d), YTL_ERR_ARG);
    CHECK_I(ytl_tween(&g_tl, 4, 1, NULL), YTL_ERR_ARG);
    d = twd_at(2.0f, 300 * M, 1600 * M);
    CHECK_I(ytl_tween(&g_tl, 6, 1, &d), YTL_ERR_ARG);
    CHECK_I(ytl_tween(&g_tl, -1, 1, &d), YTL_ERR_ARG);
    CHECK_I(ytl_tween(&g_tl, 4, YTL_MAX_BASES, &d), YTL_ERR_ARG);
    CHECK_I(ytl_tween(&g_tl, 4, -1, &d), YTL_ERR_ARG);
    d = twd_at(2.0f, 0, LIM62 + 1);
    CHECK_I(ytl_tween(&g_tl, 4, 1, &d), YTL_ERR_ARG);
    d = twd_at(2.0f, 100, LIM62 - 10);
    CHECK_I(ytl_tween(&g_tl, 4, 1, &d), YTL_ERR_ARG);
    bad = 0;
    for (bt = 1560 * M; bt <= 1950 * M; bt += 3 * M) {
        tw_frame(bt);
        bad += chan_vs(4, &tr, bt);
    }
    if (bad) fail_i(__LINE__, "BEZIER tween off the copied curve", bad, 0);
    CHECK_F(ytl_sample(&tr, 1750 * M), 1.0, 1e-6);   /* the symmetric curve at u = 0.5 */

    /* BOUND both ways. */
    CHECK(add1(&g_tl, 0, 0, YTL_SET, 5, 1.0f) >= 0);
    d = twd_at(1.0f, 10, 0);
    CHECK_I(ytl_tween(&g_tl, 5, 0, &d), YTL_ERR_BOUND);
    CHECK_I(add1(&g_tl, 1, 0, YTL_SET, 0, 1.0f), YTL_ERR_BOUND);
    CHECK_I(add1(&g_tl, 2, 0, YTL_SET, 0, 1.0f), YTL_ERR_BOUND);

    /* clear of the tweens' base frees them, at their initial values. */
    CHECK_I(ytl_clear(&g_tl, 1), 0);
    CHECK(ytl_value(&g_tl, 0) == 0.25f);
    CHECK(ytl_value(&g_tl, 1) == 0.0f);
    CHECK(ytl_value(&g_tl, 2) == 0.5f);
    CHECK(ytl_value(&g_tl, 3) == 0.0f);
    CHECK(ytl_value(&g_tl, 4) == 0.0f);
    CHECK(add1(&g_tl, 2, 0, YTL_SET, 0, 1.0f) >= 0);
    CHECK(add1(&g_tl, 3, 0, YTL_SET, 4, 1.0f) >= 0);
    tw_frame(2000 * M);
    CHECK(ytl_value(&g_tl, 4) == 0.0f);   /* base 3 is stopped */
}

/* YRT_S and YRT_MS: rounded to nearest, both signs, constant
 * expressions. */
static void test_tween_macros(void) {
    static const int64_t st[6] = { YRT_MS(200), YRT_S(0.2), YRT_S(-0.25), YRT_MS(-1.25),
                                   YRT_S(1.5), YRT_MS(0) };
    int i, bad = 0;
    CHECK_I(st[0], 200000000);
    CHECK_I(st[1], 200000000);
    CHECK_I(st[2], -250000000);
    CHECK_I(st[3], -1250000);
    CHECK_I(st[4], 1500000000);
    CHECK_I(st[5], 0);
    CHECK_I(YRT_S(2.0000000004), INT64_C(2000000000));
    CHECK_I(YRT_S(2.0000000006), INT64_C(2000000001));
    CHECK_I(YRT_S(-2.0000000004), -INT64_C(2000000000));
    CHECK_I(YRT_S(-2.0000000006), -INT64_C(2000000001));
    CHECK_I(YRT_S(-0.0000000014), -1);
    CHECK_I(YRT_S(-0.0000000016), -2);
    CHECK_I(YRT_S(0.999999999), 999999999);
    CHECK_I(YRT_S(3600.0), 3600 * S_NS);
    CHECK_I(YRT_S(-3), -3 * S_NS);
    CHECK_I(YRT_MS(200), 200 * MS_NS);
    CHECK_I(YRT_MS(0.0000004), 0);
    CHECK_I(YRT_MS(0.0000006), 1);
    CHECK_I(YRT_MS(-0.0000004), 0);
    CHECK_I(YRT_MS(-0.0000006), -1);
    CHECK_I(YRT_MS(16.6666667), 16666667);
    CHECK_I(YRT_MS(-16.6666667), -16666667);
    CHECK_I(YRT_MS(1.5), 1500000);
    CHECK_I(YTL_FOREVER, -1);
    /* Away from ties, YRT_S agrees with ytl_ns. */
    g_rng = 5150;
    for (i = 0; i < 4000; i++) {
        double x = (double)rnd_range(-INT64_C(1000000000000000), INT64_C(1000000000000000)) * 1e-13;
        if (YRT_S(x) != ytl_ns(x)) {
            if (!bad) fprintf(stderr, "  YRT_S(%.17g) = %lld, ytl_ns %lld\n", x, (long long)YRT_S(x),
                              (long long)ytl_ns(x));
            bad++;
        }
    }
    if (bad) fail_i(__LINE__, "YRT_S differs from ytl_ns", bad, 0);
}

/* Opens g_tl with 8 channels at 0.5 and lead 0.5. */
static void tw_open(void) {
    float init[8] = { 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f };
    CHECK(open_h(&g_tl, g_store, 16, 8, init, 0.5));
}

/* A jump (duration 0, LINEAR: read at the onset) to 9 on a channel at 0.5
 * with no track shows 0.5 at base time s - 1 and 9 at s. The base runs
 * with base time bt0 at RT rt0. */
static void jump_probe(int line, int ch, int64_t rt0, int64_t bt0, int64_t s) {
    eval_at(&g_tl, rt0 + (s - 1 - bt0), P60, g_tw_frame++, NULL, 0);
    if (ytl_value(&g_tl, ch) != 0.5f)
        fail_f(line, "tween took over before its start (base time s - 1)", (double)ytl_value(&g_tl, ch), 0.5);
    eval_at(&g_tl, rt0 + (s - bt0), P60, g_tw_frame++, NULL, 0);
    if (ytl_value(&g_tl, ch) != 9.0f)
        fail_f(line, "tween did not take over at its start (base time s)", (double)ytl_value(&g_tl, ch), 9.0);
}

/* start_set, "now" and delay, and a zeroed desc. */
static void test_tween_now(void) {
    static ytl_key kr[2];
    const int64_t M = MS_NS, R = S_NS;
    ytl_tween_desc d;
    int k;

    g_tw_frame = 0;
    /* Anchored, never evaluated: now is the anchor's base time. */
    tw_open();
    CHECK_I(ytl_anchor(&g_tl, 2, R, 300 * M), 0);
    d = twd(9.0f, 0);
    d.delay_ns = 100 * M;
    CHECK_I(ytl_tween(&g_tl, 0, 2, &d), 0);
    jump_probe(__LINE__, 0, R, 300 * M, 400 * M);
    /* The same with the anchor's RT time in the future. */
    tw_open();
    CHECK_I(ytl_anchor(&g_tl, 2, R + 50 * M, 300 * M), 0);
    d.delay_ns = 10 * M;
    CHECK_I(ytl_tween(&g_tl, 0, 2, &d), 0);
    jump_probe(__LINE__, 0, R + 50 * M, 300 * M, 310 * M);

    /* Never anchored: 0. The tween waits on the stopped base. */
    tw_open();
    d.delay_ns = 50 * M;
    CHECK_I(ytl_tween(&g_tl, 1, 3, &d), 0);
    eval_at(&g_tl, R, P60, g_tw_frame++, NULL, 0);
    CHECK(ytl_value(&g_tl, 1) == 0.5f);
    CHECK_I(ytl_anchor(&g_tl, 3, 2 * R, 0), 0);
    jump_probe(__LINE__, 1, 2 * R, 0, 50 * M);

    /* start_set: start + delay, a negative delay too. */
    tw_open();
    CHECK_I(ytl_anchor(&g_tl, 1, R, 0), 0);
    d = twd_at(9.0f, 0, 200 * M);
    d.delay_ns = 30 * M;
    CHECK_I(ytl_tween(&g_tl, 0, 1, &d), 0);
    d.delay_ns = 0;
    CHECK_I(ytl_tween(&g_tl, 1, 1, &d), 0);
    d = twd_at(9.0f, 0, 500 * M);
    d.delay_ns = -100 * M;
    CHECK_I(ytl_tween(&g_tl, 2, 1, &d), 0);
    jump_probe(__LINE__, 1, R, 0, 200 * M);
    jump_probe(__LINE__, 0, R, 0, 230 * M);
    jump_probe(__LINE__, 2, R, 0, 400 * M);

    /* After evaluates: the base time of the last evaluated onset. */
    tw_open();
    CHECK_I(ytl_anchor(&g_tl, 1, R, 0), 0);
    for (k = 0; k <= 6; k++) eval_at(&g_tl, R + k * P60, P60, g_tw_frame++, NULL, 0);
    d = twd(9.0f, 0);
    d.delay_ns = 20 * M;
    CHECK_I(ytl_tween(&g_tl, 0, 1, &d), 0);
    jump_probe(__LINE__, 0, R, 0, 6 * P60 + 20 * M);

    /* Paused: an evaluate of a paused base is at its frozen time. */
    tw_open();
    CHECK_I(ytl_anchor(&g_tl, 1, R, 0), 0);
    eval_at(&g_tl, R, P60, g_tw_frame++, NULL, 0);
    CHECK_I(ytl_pause(&g_tl, 1, R + 70 * M), 0);
    eval_at(&g_tl, R + 100 * M, P60, g_tw_frame++, NULL, 0);
    d = twd(9.0f, 0);
    d.delay_ns = 10 * M;
    CHECK_I(ytl_tween(&g_tl, 0, 1, &d), 0);
    eval_at(&g_tl, R + 150 * M, P60, g_tw_frame++, NULL, 0);
    CHECK(ytl_value(&g_tl, 0) == 0.5f);
    CHECK_I(ytl_resume(&g_tl, 1, R + 200 * M), 0);
    jump_probe(__LINE__, 0, R + 200 * M, 70 * M, 80 * M);

    /* Base 0 is never anchored: now is 0 before the first evaluate, then
     * the last onset. */
    tw_open();
    d = twd(9.0f, 0);
    d.delay_ns = R + 5 * M;
    CHECK_I(ytl_tween(&g_tl, 0, 0, &d), 0);
    jump_probe(__LINE__, 0, 0, 0, R + 5 * M);
    d.delay_ns = 7 * M;
    CHECK_I(ytl_tween(&g_tl, 1, 0, &d), 0);
    jump_probe(__LINE__, 1, 0, 0, R + 12 * M);

    /* An anchor after the last evaluate sets now to its base time: after
     * a rewinding anchor to 0, now is 0 (not the last onset's 500 ms, and
     * not the rewind's just-before-0), so a delay of 10 ms starts at
     * 10 ms. */
    tw_open();
    CHECK_I(ytl_anchor(&g_tl, 1, R, 0), 0);
    eval_at(&g_tl, R + 500 * M, P60, g_tw_frame++, NULL, 0);
    CHECK_I(ytl_anchor(&g_tl, 1, R + 510 * M, 0), 0);
    d = twd(9.0f, 0);
    d.delay_ns = 10 * M;
    CHECK_I(ytl_tween(&g_tl, 0, 1, &d), 0);
    jump_probe(__LINE__, 0, R + 510 * M, 0, 10 * M);
    /* Literal reading: a pause at an RT time before the last onset moves
     * the base back without a rewind; the next evaluate's onset is then
     * at 60 ms, behind the furthest (100 ms), and now is 60 ms. */
    tw_open();
    CHECK_I(ytl_anchor(&g_tl, 1, R, 0), 0);
    eval_at(&g_tl, R + 100 * M, P60, g_tw_frame++, NULL, 0);
    CHECK_I(ytl_pause(&g_tl, 1, R + 60 * M), 0);
    eval_at(&g_tl, R + 110 * M, P60, g_tw_frame++, NULL, 0);
    d = twd(9.0f, 0);
    d.delay_ns = 20 * M;
    CHECK_I(ytl_tween(&g_tl, 0, 1, &d), 0);
    CHECK_I(ytl_resume(&g_tl, 1, R + 200 * M), 0);
    eval_at(&g_tl, R + 220 * M, P60, g_tw_frame++, NULL, 0);   /* base time 80 ms */
    if (ytl_value(&g_tl, 0) != 9.0f)
        fail(__LINE__, "literal reading: now is the last evaluated onset's base time (60 ms), but the tween "
                       "had not taken over at 60 + 20 ms (the furthest evaluated, 100 ms, gives 120 ms)");
    /* And after a forward anchor: the anchor's base time, 300 ms. */
    tw_open();
    CHECK_I(ytl_anchor(&g_tl, 1, R, 0), 0);
    eval_at(&g_tl, R + 100 * M, P60, g_tw_frame++, NULL, 0);
    CHECK_I(ytl_anchor(&g_tl, 1, R + 100 * M, 300 * M), 0);
    d.delay_ns = 250 * M;
    CHECK_I(ytl_tween(&g_tl, 0, 1, &d), 0);
    jump_probe(__LINE__, 0, R + 100 * M, 300 * M, 550 * M);
    /* A pause after the last evaluate: the frozen time. Evaluated at
     * 100 ms, paused at 140 ms, so now is 140 ms; resumed later, a delay
     * of 20 ms starts at 160 ms. */
    tw_open();
    CHECK_I(ytl_anchor(&g_tl, 1, R, 0), 0);
    eval_at(&g_tl, R + 100 * M, P60, g_tw_frame++, NULL, 0);
    CHECK_I(ytl_pause(&g_tl, 1, R + 140 * M), 0);
    d = twd(9.0f, 0);
    d.delay_ns = 20 * M;
    CHECK_I(ytl_tween(&g_tl, 0, 1, &d), 0);
    CHECK_I(ytl_resume(&g_tl, 1, R + 300 * M), 0);
    jump_probe(__LINE__, 0, R + 300 * M, 140 * M, 160 * M);

    /* A zeroed desc: a LINEAR jump to 0 now, from a held value or a
     * running track. It changes nothing before the next evaluate. */
    tw_open();
    kr[0] = mkkey(0, 0.0f, YTL_EASE_LINEAR);
    kr[1] = mkkey(S_NS, 10.0f, YTL_EASE_LINEAR);
    CHECK_I(ytl_set_keys(&g_tl, 1, 1, kr, 2), 0);
    CHECK_I(ytl_anchor(&g_tl, 1, R, 0), 0);
    eval_at(&g_tl, R + 10 * M, P60, g_tw_frame++, NULL, 0);
    CHECK(ytl_value(&g_tl, 0) == 0.5f);
    CHECK_F(ytl_value(&g_tl, 1), 0.1, 1e-6);
    memset(&d, 0, sizeof d);
    CHECK_I(ytl_tween(&g_tl, 0, 1, &d), 0);
    CHECK_I(ytl_tween(&g_tl, 1, 1, &d), 0);
    CHECK(ytl_value(&g_tl, 0) == 0.5f);
    CHECK_F(ytl_value(&g_tl, 1), 0.1, 1e-6);
    eval_at(&g_tl, R + 10 * M, P60, g_tw_frame++, NULL, 0);   /* the same onset: now itself */
    CHECK(ytl_value(&g_tl, 0) == 0.0f);
    CHECK(ytl_value(&g_tl, 1) == 0.0f);
    eval_at(&g_tl, R + 500 * M, P60, g_tw_frame++, NULL, 0);
    CHECK(ytl_value(&g_tl, 0) == 0.0f);
    CHECK(ytl_value(&g_tl, 1) == 0.0f);
    /* Before any evaluate on an anchored base: at the anchor's base time. */
    tw_open();
    CHECK_I(ytl_anchor(&g_tl, 2, R, 40 * M), 0);
    CHECK_I(ytl_tween(&g_tl, 2, 2, &d), 0);
    eval_at(&g_tl, R - 1, P60, g_tw_frame++, NULL, 0);
    CHECK(ytl_value(&g_tl, 2) == 0.5f);
    eval_at(&g_tl, R, P60, g_tw_frame++, NULL, 0);
    CHECK(ytl_value(&g_tl, 2) == 0.0f);
}

/* A model channel as a chain of drivers: d[0] (or nothing, the initial
 * value, when none0) until at[1], d[1] until at[2], and so on. */
typedef struct rstage {
    rdrv    d[3];
    int64_t at[3];
    int     n, none0;
    double  init;
} rstage;

static double rstage_value(const rstage* s, int64_t t) {
    int k = 0, i;
    for (i = 1; i < s->n; i++)
        if (t >= s->at[i]) k = i;
    if (k == 0 && s->none0) return s->init;
    return rdrv_value(&s->d[k], t);
}

static void rstage_tween(rstage* st, const ytl_tween_desc* d, int64_t s, double at_call) {
    const rdrv* old = (st->n == 1 && st->none0) ? NULL : &st->d[st->n - 1];
    rdrv_tween(&st->d[st->n], d, s, old, st->init, at_call);
    st->at[st->n] = s;
    st->n++;
}

enum { GR_N = 7, GR_NCH = 8, GR_K = 26 };
static float g_gr[GR_N][GR_NCH][GR_K];
static rstage g_rst[GR_NCH];

/* THE TAKEOVER: from is the old track's value at the start, for keyed,
 * sampled, repeating and tween old tracks, with and without
 * keep_velocity, and does not depend on the frames. The same calls run on
 * seven frame grids (1, 2, 3, 5, 6 and 10 ms and an irregular one), the
 * second tweens posted at base time 60 ms for a start of 123.456789 ms
 * that no grid has; every frame against the model, and the values at
 * every 30 ms, which all grids have, compared across grids bit for bit. */
static void test_tween_from(void) {
    static ytl_key k0[4], k2[3], k5[2], k7[2];
    static float s1[40], s3[30];
    static const int64_t gp[6] = { 1000000, 2000000, 3000000, 5000000, 6000000, 10000000 };
    const int64_t M = MS_NS, R = S_NS, S = 123456789, TC = 60 * M, END = 750 * M, C30 = 30 * M;
    float init[GR_NCH] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.5f, 0.0f, 2.0f, 0.0f };
    ytl_track trs[GR_NCH];
    ytl_tween_desc d1[GR_NCH], d2[GR_NCH];
    int g, ch, i, k, bad = 0, bad_g = 0;
    int64_t bt, idx = 0;

    k0[0] = mkkey(0, 1.0f, YTL_EASE_COSINE);
    k0[1] = mkkey(200 * M, 4.0f, YTL_EASE_QUAD_IN);
    k0[2] = mkkey(450 * M, -2.0f, YTL_EASE_LINEAR);
    k0[3] = mkkey(900 * M, 3.0f, YTL_EASE_LINEAR);
    for (i = 0; i < 40; i++) s1[i] = (float)(2.0 * sin(0.3 * (double)i) + 0.1 * (double)i);
    k2[0] = mkkey(10 * M, 0.0f, YTL_EASE_LINEAR);
    k2[1] = mkkey(60 * M, 5.0f, YTL_EASE_QUAD_OUT);
    k2[2] = mkkey(80 * M, 2.0f, YTL_EASE_LINEAR);
    for (i = 0; i < 30; i++) s3[i] = (float)((i * 7) % 11) - 5.0f;
    k5[0] = mkkey(0, 0.0f, YTL_EASE_COSINE);
    k5[1] = mkkey(600 * M, 10.0f, YTL_EASE_LINEAR);
    k7[0] = mkkey(0, 0.0f, YTL_EASE_LINEAR);
    k7[1] = mkkey(S_NS, 10.0f, YTL_EASE_LINEAR);
    memset(trs, 0, sizeof trs);
    trs[0] = trk_keys(k0, 4);                                          /* keyed            */
    trs[1] = trk_samples(s1, 40, -100 * M, 60, YTL_INTERP_CUBIC);    /* sampled          */
    trs[2] = trk_keys(k2, 3);                                          /* repeating keyed  */
    trs[2].period_ns = 75 * M;
    trs[3] = trk_samples(s3, 30, 0, 120, YTL_INTERP_LINEAR);         /* repeating sampled */
    trs[3].period_ns = 47 * M;
    trs[5] = trk_keys(k5, 2);                                          /* keyed, kv after  */
    trs[7] = trk_keys(k7, 2);                                          /* kv after kv      */
    /* First tweens, posted before the anchor: channel 4 and 6 have no
     * track before them; 7's is a keep_velocity takeover at 50 ms. */
    d1[4] = twd_at(5.0f, 400 * M, 20 * M);
    d1[4].from_set = true;
    d1[4].from = 1.0f;
    d1[6] = twd_at(6.0f, 500 * M, 0);
    d1[6].from_set = true;
    d1[6].from = -3.0f;
    d1[6].ease = YTL_EASE_QUAD_OUT;
    d1[7] = twd_at(-4.0f, 300 * M, 50 * M);
    d1[7].keep_velocity = true;
    /* The second tweens, posted at 60 ms. Channel 1's start is now + delay. */
    d2[0] = twd_at(7.0f, 250 * M, S);
    d2[0].ease = YTL_EASE_COSINE;
    d2[1] = twd(-1.0f, 300 * M);
    d2[1].delay_ns = S - TC;
    d2[2] = twd_at(3.0f, 200 * M, S);
    d2[2].ease = YTL_EASE_QUAD_IN;
    d2[3] = twd_at(2.0f, 260 * M, S);
    d2[3].ease = YTL_EASE_BEZIER;
    d2[3].curve = mkcurve(0.42f, 0.0f, 0.58f, 1.0f);
    d2[4] = twd_at(0.0f, 200 * M, S);
    d2[4].ease = YTL_EASE_COSINE;
    d2[5] = twd_at(4.0f, 300 * M, S);
    d2[5].keep_velocity = true;
    d2[6] = twd_at(0.0f, 250 * M, S);
    d2[6].keep_velocity = true;
    d2[7] = twd_at(1.0f, 200 * M, S);
    d2[7].keep_velocity = true;

    for (ch = 0; ch < GR_NCH; ch++) {
        rstage* st = &g_rst[ch];
        memset(st, 0, sizeof *st);
        st->n = 1;
        st->init = (double)init[ch];
        if (ch == 4 || ch == 6) st->none0 = 1;
        else st->d[0] = rdrv_plain(&trs[ch]);
    }
    rstage_tween(&g_rst[4], &d1[4], 20 * M, (double)init[4]);
    rstage_tween(&g_rst[6], &d1[6], 0, (double)init[6]);
    rstage_tween(&g_rst[7], &d1[7], 50 * M, (double)init[7]);
    for (ch = 0; ch < GR_NCH; ch++) rstage_tween(&g_rst[ch], &d2[ch], S, rstage_value(&g_rst[ch], TC));
    /* By hand: the repeating keyed track at S reads 48.456789 ms, 5 x
     * 38.456789 / 50; the first tween on channel 4, 1 + 4 x 103.456789 /
     * 400. */
    CHECK_F(rdrv_value(&g_rst[2].d[1], S), 5.0 * 38.456789 / 50.0, 1e-5);
    CHECK_F(rdrv_value(&g_rst[4].d[2], S), 1.0 + 4.0 * 103.456789 / 400.0, 1e-5);

    g_rng = 777;
    g_cv_prints = 0;
    for (g = 0; g < GR_N; g++) {
        CHECK(open_h(&g_tl, g_store, 8, GR_NCH, init, 0.5));
        for (ch = 0; ch < GR_NCH; ch++)
            if (ch != 4 && ch != 6) CHECK_I(ytl_set_track(&g_tl, ch, 1, &trs[ch]), 0);
        CHECK_I(ytl_tween(&g_tl, 4, 1, &d1[4]), 0);
        CHECK_I(ytl_tween(&g_tl, 6, 1, &d1[6]), 0);
        CHECK_I(ytl_tween(&g_tl, 7, 1, &d1[7]), 0);
        CHECK_I(ytl_anchor(&g_tl, 1, R, 0), 0);
        bt = 0;
        for (;;) {
            eval_at(&g_tl, R + bt, g < 6 ? gp[g] : 5 * M, idx++, NULL, 0);
            for (ch = 0; ch < GR_NCH; ch++) {
                double want = rstage_value(&g_rst[ch], bt), got = (double)ytl_value(&g_tl, ch);
                if (!(fabs(got - want) <= tol_of(want))) {
                    if (g_cv_prints++ < 8)
                        fprintf(stderr, "  takeover grid %d channel %d base time %lld: got %.9g, model %.9g\n", g,
                                ch, (long long)bt, got, want);
                    bad++;
                }
                if (bt % C30 == 0) g_gr[g][ch][bt / C30] = ytl_value(&g_tl, ch);
            }
            if (bt == TC)
                for (ch = 0; ch < GR_NCH; ch++) CHECK_I(ytl_tween(&g_tl, ch, 1, &d2[ch]), 0);
            if (bt >= END) break;
            if (g < 6) {
                bt += gp[g];
            } else {
                int64_t nx = bt + rnd_range(100000, 7 * M), m = (bt / C30 + 1) * C30;
                bt = nx > m ? m : nx;
            }
        }
    }
    if (bad) fail_i(__LINE__, "takeover values off the model", bad, 0);
    for (g = 1; g < GR_N; g++)
        for (ch = 0; ch < GR_NCH; ch++)
            for (k = 0; k < GR_K; k++)
                if (g_gr[g][ch][k] != g_gr[0][ch][k]) {
                    if (!bad_g)
                        fprintf(stderr, "  grid %d channel %d at %d ms: %.9g, 1 ms grid %.9g\n", g, ch, k * 30,
                                (double)g_gr[g][ch][k], (double)g_gr[0][ch][k]);
                    bad_g++;
                }
    if (bad_g) fail_i(__LINE__, "takeover values depend on the frame grid", bad_g, 0);
}

/* keep_velocity: no step in value or in velocity at the start, and the
 * Hermite model. Frames every 1 ms, the start half way between two. */
static void test_tween_velocity(void) {
    static ytl_key kr[2];
    static float sv[200];
    static float v[4][601];
    const int64_t M = MS_NS, R = S_NS, S = 500 * M + 500000;
    float init[8] = { 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f };
    ytl_track tr, ts;
    ytl_tween_desc d;
    rdrv old_r, old_s, r[4];
    double left, right, mright, slope;
    int ch, k, bad = 0;

    g_cv_prints = 0;
    for (k = 0; k < 200; k++) sv[k] = (float)(3.0 * sin(0.1 * (double)k));
    kr[0] = mkkey(0, 0.0f, YTL_EASE_LINEAR);
    kr[1] = mkkey(S_NS, 10.0f, YTL_EASE_LINEAR);
    tr = trk_keys(kr, 2);
    ts = trk_samples(sv, 200, 0, 120, YTL_INTERP_CUBIC);
    old_r = rdrv_plain(&tr);
    old_s = rdrv_plain(&ts);
    CHECK(open_h(&g_tl, g_store, 8, 8, init, 0.5));
    CHECK_I(ytl_set_track(&g_tl, 0, 1, &tr), 0);
    CHECK_I(ytl_set_track(&g_tl, 1, 1, &tr), 0);
    CHECK_I(ytl_set_track(&g_tl, 2, 1, &ts), 0);
    /* 0: a ramp at 10/s retargeted to 4; 1: the same without
     * keep_velocity, whose velocity steps; 2: a sampled sine; 3: no track
     * (literal: velocity 0, so a smoothstep from 1 to 3). */
    d = twd_at(4.0f, 300 * M, S);
    d.keep_velocity = true;
    CHECK_I(ytl_tween(&g_tl, 0, 1, &d), 0);
    rdrv_tween(&r[0], &d, S, &old_r, 0.0, 0.0);
    d.keep_velocity = false;
    CHECK_I(ytl_tween(&g_tl, 1, 1, &d), 0);
    rdrv_tween(&r[1], &d, S, &old_r, 0.0, 0.0);
    d = twd_at(0.0f, 400 * M, S);
    d.keep_velocity = true;
    CHECK_I(ytl_tween(&g_tl, 2, 1, &d), 0);
    rdrv_tween(&r[2], &d, S, &old_s, 0.0, 0.0);
    d = twd_at(3.0f, 200 * M, S);
    d.keep_velocity = true;
    CHECK_I(ytl_tween(&g_tl, 3, 1, &d), 0);
    rdrv_tween(&r[3], &d, S, NULL, 1.0, 1.0);
    CHECK_I(ytl_anchor(&g_tl, 1, R, 0), 0);
    for (k = 0; k <= 600; k++) {
        int64_t bt = 400 * M + (int64_t)k * M;
        eval_at(&g_tl, R + bt, M, k, NULL, 0);
        for (ch = 0; ch < 4; ch++) {
            v[ch][k] = ytl_value(&g_tl, ch);
            if (bt < S) {
                const rdrv* o = ch == 2 ? &old_s : &old_r;
                if (ch == 3) {
                    if (v[ch][k] != 1.0f) bad++;
                } else {
                    bad += chk_drv(ch, o, bt);
                }
            } else {
                bad += chk_drv(ch, &r[ch], bt);
            }
        }
    }
    if (bad) fail_i(__LINE__, "keep_velocity values off the Hermite model", bad, 0);
    /* Frame 100 is at 500 ms, frame 101 at 501 ms; the start is between. */
    left = ((double)v[0][100] - (double)v[0][99]) / 1e-3;
    right = ((double)v[0][102] - (double)v[0][101]) / 1e-3;
    mright = (rdrv_value(&r[0], 502 * M) - rdrv_value(&r[0], 501 * M)) / 1e-3;
    CHECK_F(left, 10.0, 0.01);
    CHECK_F(right, mright, 5e-3);
    if (!(fabs(right - left) < 1.0)) fail_f(__LINE__, "keep_velocity: velocity steps at the start", right, left);
    if (!(fabs((double)v[0][101] - 5.01) < 1e-4))
        fail_f(__LINE__, "keep_velocity: value steps at the start", (double)v[0][101], 5.01);
    /* The finite differences see a step: without keep_velocity, 10/s to
     * (4 - 5.005) / 0.3 s. */
    left = ((double)v[1][100] - (double)v[1][99]) / 1e-3;
    right = ((double)v[1][102] - (double)v[1][101]) / 1e-3;
    CHECK(fabs(right - left) > 10.0);
    /* The sampled sine: its slope over the last microsecond, continued. */
    slope = (ref_track(&ts, S) - ref_track(&ts, S - 1000)) * 1e6;   /* per second */
    left = ((double)v[2][100] - (double)v[2][99]) / 1e-3;
    right = ((double)v[2][102] - (double)v[2][101]) / 1e-3;
    if (!(fabs(right - left) < 3.0)) fail_f(__LINE__, "keep_velocity on a sampled track: velocity steps", right, left);
    CHECK(fabs(slope) > 10.0);   /* not vacuous: the sine moves fast there */
    /* No track: velocity 0 at the start. */
    CHECK(fabs((double)v[3][101] - 1.0) < 1e-3);
    CHECK_F(v[3][100 + 100], 1.0 + 2.0 * (3.0 * 0.4975 * 0.4975 - 2.0 * 0.4975 * 0.4975 * 0.4975), 1e-5);
}

/* cycles and yoyo, against the model and by hand. */
static void test_tween_cycles(void) {
    const int64_t M = MS_NS, R = S_NS, S = 50 * M;
    float init[8] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
    ytl_tween_desc d[8];
    rdrv r[8];
    int ch, bad = 0, bad23 = 0;
    int64_t bt, onset = R, far;

    g_cv_prints = 0;
    CHECK(open_h(&g_tl, g_store, 8, 8, init, 0.5));
    d[0] = twd_at(1.0f, 100 * M, S);
    d[0].from_set = true;
    d[0].from = 0.0f;
    d[0].cycles = 3;
    d[1] = d[0];
    d[1].cycles = YTL_FOREVER;
    d[2] = d[0];
    d[2].cycles = 0;
    d[3] = d[0];
    d[3].cycles = 1;
    d[4] = twd_at(6.0f, 100 * M, S);
    d[4].from_set = true;
    d[4].from = 2.0f;
    d[4].ease = YTL_EASE_COSINE;
    d[4].yoyo = true;
    d[5] = twd_at(1.0f, 40 * M, S);
    d[5].from_set = true;
    d[5].from = -1.0f;
    d[5].yoyo = true;
    d[5].cycles = 3;
    d[6] = twd_at(1.0f, 100 * M, S);
    d[6].from_set = true;
    d[6].from = 0.0f;
    d[6].ease = YTL_EASE_QUAD_IN;
    d[6].yoyo = true;
    d[7] = twd_at(1.0f, 60 * M, S);
    d[7].from_set = true;
    d[7].from = 0.0f;
    d[7].ease = YTL_EASE_BEZIER;
    d[7].curve = mkcurve(0.68f, -0.6f, 0.32f, 1.6f);
    d[7].yoyo = true;
    d[7].cycles = YTL_FOREVER;
    for (ch = 0; ch < 8; ch++) {
        CHECK_I(ytl_tween(&g_tl, ch, 1, &d[ch]), 0);
        rdrv_tween(&r[ch], &d[ch], S, NULL, 0.0, 0.0);
    }
    CHECK_I(ytl_anchor(&g_tl, 1, R, 0), 0);
    for (bt = 0; bt <= 1000 * M; bt += M) {
        float v0, v1, v4, v5, v6;
        onset = R + bt;
        eval_at(&g_tl, onset, M, bt / M, NULL, 0);
        /* No track before them: the channels hold 0 until the start. */
        for (ch = 0; ch < 8; ch++) {
            if (bt >= S) bad += chk_drv(ch, &r[ch], bt);
            else if (ytl_value(&g_tl, ch) != 0.0f) bad++;
        }
        if (ytl_value(&g_tl, 2) != ytl_value(&g_tl, 3)) bad23++;
        v0 = ytl_value(&g_tl, 0);
        v1 = ytl_value(&g_tl, 1);
        v4 = ytl_value(&g_tl, 4);
        v5 = ytl_value(&g_tl, 5);
        v6 = ytl_value(&g_tl, 6);
        if (bt == S + 99 * M) CHECK_F(v0, 0.99, 1e-6);
        if (bt == S + 100 * M) {
            CHECK(v0 == 0.0f);   /* the second cycle */
            CHECK(ytl_value(&g_tl, 2) == 1.0f);   /* once: `to` from the end */
        }
        if (bt == S + 250 * M) {
            CHECK_F(v0, 0.5, 1e-6);
            CHECK_F(v1, 0.5, 1e-6);
        }
        if (bt == S + 300 * M) {
            CHECK(v0 == 1.0f);   /* after 3 cycles: the value at start + duration */
            CHECK(v1 == 0.0f);   /* forever: the fourth cycle */
        }
        if (bt == S + 350 * M) CHECK_F(v1, 0.5, 1e-6);
        if (bt == S + 50 * M) CHECK_F(v4, 4.0, 1e-5);
        if (bt == S + 100 * M) CHECK_F(v4, 6.0, 1e-6);
        if (bt == S + 150 * M) CHECK_F(v4, 4.0, 1e-5);   /* on the way back */
        if (bt == S + 200 * M || bt == S + 300 * M) CHECK(v4 == 2.0f);   /* back at from, and held */
        if (bt == S + 20 * M || bt == S + 60 * M) CHECK_F(v5, 0.0, 1e-6);
        if (bt == S + 40 * M) CHECK_F(v5, 1.0, 1e-6);
        if (bt == S + 80 * M) CHECK_F(v5, -1.0, 1e-6);
        if (bt == S + 239 * M) CHECK_F(v5, -0.95, 1e-6);
        if (bt == S + 240 * M || bt == S + 500 * M) CHECK(v5 == -1.0f);
        /* v0.7.0: the return leg is the forward leg backward in time (GSAP),
         * so QUAD_IN back is 0 + (1 - 0) 0.5^2 = 0.25 half way; the v0.6.0
         * rule (the same ease again) gave 0.75. And it ends fast: 10 ms
         * before the end, 0.1^2. */
        if (bt == S + 150 * M && !(fabs((double)v6 - 0.25) <= 1e-6))
            fail_f(__LINE__, "yoyo's return leg is not the forward leg mirrored in time", (double)v6, 0.25);
        if (bt == S + 190 * M) CHECK_F(v6, 0.01, 1e-6);
    }
    if (bad) fail_i(__LINE__, "cycles and yoyo values off the model", bad, 0);
    if (bad23) fail_i(__LINE__, "cycles 0 and cycles 1 differ", bad23, 0);
    /* Far on: forever keeps cycling, the counted ones hold. */
    far = S + INT64_C(123456789) * 100 * M + 25 * M;
    onset += M;
    CHECK_I(ytl_anchor(&g_tl, 1, onset, far), 0);
    eval_at(&g_tl, onset, M, 2000, NULL, 0);
    for (ch = 0; ch < 8; ch++) bad += chk_drv(ch, &r[ch], far);
    CHECK_F(ytl_value(&g_tl, 1), 0.25, 1e-6);
    CHECK(ytl_value(&g_tl, 0) == 1.0f);
    CHECK(ytl_value(&g_tl, 4) == 2.0f);
    CHECK(ytl_value(&g_tl, 5) == -1.0f);
    CHECK(ytl_value(&g_tl, 6) == 0.0f);
    if (bad) fail_i(__LINE__, "cycles far on off the model", bad, 0);
}

/* STEP tweens: taken over at the end of the lead window, from the old
 * track's value at the start; a yoyo STEP tween is a STEP track whose
 * edges land where events at the same times land; USAGE's flicker as a
 * tween. */
static void test_tween_step(void) {
    static ytl_key kr[2];
    static const int lnum[6] = { 0, 1, 1, 3, 999, 1 }, lden[6] = { 1, 4, 2, 4, 1000, 3 };
    const int64_t M = MS_NS, R = S_NS, P = 10 * M;
    ytl_track tr;
    ytl_tween_desc d;
    rdrv old, r0, r1, rw;
    int trial, lead, j, bad = 0, bad_m = 0, n_edge = 0;
    int64_t bt, wrong = 0;

    kr[0] = mkkey(0, 0.0f, YTL_EASE_LINEAR);
    kr[1] = mkkey(S_NS, 100.0f, YTL_EASE_LINEAR);
    tr = trk_keys(kr, 2);
    old = rdrv_plain(&tr);
    g_cv_prints = 0;
    for (lead = 0; lead < 2; lead++) {
        int64_t w = lead ? P / 2 : 0;
        CHECK(open_h(&g_tl, g_store, 8, 4, NULL, lead ? 0.5 : 0.0));
        CHECK_I(ytl_set_track(&g_tl, 0, 1, &tr), 0);
        CHECK_I(ytl_set_track(&g_tl, 1, 1, &tr), 0);
        d = twd_at(50.0f, 30 * M, 103 * M);
        d.ease = YTL_EASE_STEP;
        CHECK_I(ytl_tween(&g_tl, 0, 1, &d), 0);
        rdrv_tween(&r0, &d, 103 * M, &old, 0.0, 0.0);
        d.ease = YTL_EASE_LINEAR;
        CHECK_I(ytl_tween(&g_tl, 1, 1, &d), 0);
        rdrv_tween(&r1, &d, 103 * M, &old, 0.0, 0.0);
        CHECK_I(ytl_anchor(&g_tl, 1, R, 0), 0);
        for (bt = 0; bt <= 200 * M; bt += P) {
            int64_t we = bt + ((g_mut & MUT_STEP_ONSET) ? 0 : w);
            eval_at(&g_tl, R + bt, P, bt / P, NULL, 0);
            bad += chk_drv(0, we < 103 * M ? &old : &r0, we < 103 * M ? bt : we);
            bad += chk_drv(1, bt < 103 * M ? &old : &r1, bt);
            if (bt == 100 * M) {
                /* Lead 0.5: the window ends at 105 ms, so the STEP tween
                 * shows from = the ramp at 103 ms; the LINEAR one waits. */
                CHECK_F(ytl_value(&g_tl, 0), lead ? 10.3 : 10.0, 1e-5);
                CHECK_F(ytl_value(&g_tl, 1), 10.0, 1e-5);
            }
            if (bt == 110 * M) CHECK_F(ytl_value(&g_tl, 0), 10.3, 1e-5);
            if (bt == 130 * M) CHECK(ytl_value(&g_tl, 0) == (lead ? 50.0f : 10.3f));
        }
    }
    if (bad) fail_i(__LINE__, "STEP tween takeover off the window's end", bad, 0);

    /* Random: a yoyo STEP tween over 2 cycles, 0 -> 1 -> 0 -> 1 -> 0 with
     * edges at te, te + dd, te + 2 dd, te + 3 dd, against SET events at
     * those times, at every lead, period 0 too, with onset noise. */
    g_rng = 9191;
    bad = 0;
    for (trial = 0; trial < 200; trial++) {
        int li = trial % 6;
        int64_t PP = trial % 7 == 0 ? 0 : trial % 3 == 0 ? P60 : trial % 3 == 1 ? 6944444 : 2000000;
        int64_t te = rnd_range(0, 50 * P60), dd = rnd_range(1, 3 * P60), onset = R, bt0 = rnd_range(-3 * P60, 0);
        int64_t w = PP * lnum[li] / lden[li], k;
        CHECK(open_h(&g_tl, g_store, 8, 2, NULL, (double)lnum[li] / (double)lden[li]));
        for (j = 0; j < 4; j++) CHECK(add1(&g_tl, 1, te + j * dd, YTL_SET, 0, j % 2 == 0 ? 1.0f : 0.0f) >= 0);
        d = twd_at(1.0f, dd, te - dd);
        d.from_set = true;
        d.from = 0.0f;
        d.ease = YTL_EASE_STEP;
        d.yoyo = true;
        d.cycles = 2;
        CHECK_I(ytl_tween(&g_tl, 1, 1, &d), 0);
        rdrv_tween(&rw, &d, te - dd, NULL, 0.0, 0.0);
        CHECK_I(ytl_anchor(&g_tl, 1, R, bt0), 0);
        for (k = 0; ; k++) {
            int64_t b2, rd;
            float ve, vt, want;
            eval_at(&g_tl, onset, PP, k, NULL, 0);
            b2 = bt0 + (onset - R);
            rd = rdrv_step(&rw) ? b2 + w : b2;
            ve = ytl_value(&g_tl, 0);
            vt = ytl_value(&g_tl, 1);
            want = rd < te - dd ? 0.0f : (float)rdrv_value(&rw, rd);
            if (vt != ve) {
                if (!bad)
                    fprintf(stderr, "  yoyo STEP: trial %d lead %d/%d P %lld te %lld dd %lld frame bt %lld: tween %g, "
                                    "events %g\n", trial, lnum[li], lden[li], (long long)PP, (long long)te,
                            (long long)dd, (long long)b2, (double)vt, (double)ve);
                bad++;
            }
            if (vt != want) bad_m++;
            if (k > 0 && ve != 0.0f) n_edge++;
            if (b2 > te + 4 * dd + 3 * (PP ? PP : 20 * M)) break;
            onset += PP ? PP + rnd_range(-PP / 50, PP / 50) : rnd_range(4 * M, 20 * M);
        }
    }
    if (bad) fail_i(__LINE__, "yoyo STEP tween edges not on the events' frames", bad, 0);
    if (bad_m) fail_i(__LINE__, "yoyo STEP tween off the model at the window's end", bad_m, 0);
    if (n_edge == 0) fail(__LINE__, "yoyo STEP: no frame showed 1 (vacuous)");

    /* USAGE's 7.5 Hz flicker as a tween: 1 -> 0 -> 1, STEP, forever. At
     * 60 Hz with 1 us of onset noise frame k shows 1 when k mod 8 < 4. */
    CHECK(open_h(&g_tl, g_store, 4, 1, NULL, 0.5));
    d = twd_at(0.0f, 2 * S_NS / 30, 0);
    d.from_set = true;
    d.from = 1.0f;
    d.ease = YTL_EASE_STEP;
    d.yoyo = true;
    d.cycles = YTL_FOREVER;
    CHECK_I(ytl_tween(&g_tl, 0, 1, &d), 0);
    CHECK_I(ytl_anchor(&g_tl, 1, R, 0), 0);
    for (bt = 0; bt < 3600; bt++) {
        eval_at(&g_tl, R + (bt * NS_E9 + 30) / 60 + rnd_range(-1000, 1000), P60, bt, NULL, 0);
        if (ytl_value(&g_tl, 0) != ((bt % 8) < 4 ? 1.0f : 0.0f)) wrong++;
    }
    if (wrong) fail_i(__LINE__, "tween flicker frames off k mod 8 < 4", wrong, 0);
}

/* Retargeting a moving channel just before a frame: the next frame shows
 * where the old motion would have been, with no repeated position. */
static void test_tween_retarget(void) {
    static ytl_key kr[2];
    ytl_track tr;
    ytl_tween_desc d;
    rdrv old, r[3];
    const int64_t R = S_NS, M = MS_NS;
    float v10[3], v11[3], v12[3];
    int ch, k, bad = 0;

    g_cv_prints = 0;
    CHECK(open_h(&g_tl, g_store, 8, 4, NULL, 0.5));
    kr[0] = mkkey(0, 0.0f, YTL_EASE_LINEAR);
    kr[1] = mkkey(S_NS, 100.0f, YTL_EASE_LINEAR);
    tr = trk_keys(kr, 2);
    old = rdrv_plain(&tr);
    for (ch = 0; ch < 3; ch++) CHECK_I(ytl_set_track(&g_tl, ch, 1, &tr), 0);
    CHECK_I(ytl_anchor(&g_tl, 1, R, 0), 0);
    for (k = 0; k <= 10; k++) {
        eval_at(&g_tl, R + k * P60, P60, k, NULL, 0);
        for (ch = 0; ch < 3; ch++) bad += chk_drv(ch, &old, k * P60);
    }
    for (ch = 0; ch < 3; ch++) v10[ch] = ytl_value(&g_tl, ch);
    /* 0: start one frame on (now + P); 1: the same with keep_velocity;
     * 2: start now, the frame already shown. */
    d = twd(0.0f, 300 * M);
    d.delay_ns = P60;
    CHECK_I(ytl_tween(&g_tl, 0, 1, &d), 0);
    rdrv_tween(&r[0], &d, 11 * P60, &old, 0.0, (double)v10[0]);
    d.keep_velocity = true;
    CHECK_I(ytl_tween(&g_tl, 1, 1, &d), 0);
    rdrv_tween(&r[1], &d, 11 * P60, &old, 0.0, (double)v10[1]);
    d = twd(0.0f, 300 * M);
    CHECK_I(ytl_tween(&g_tl, 2, 1, &d), 0);
    rdrv_tween(&r[2], &d, 10 * P60, &old, 0.0, (double)v10[2]);
    for (k = 11; k <= 40; k++) {
        eval_at(&g_tl, R + k * P60, P60, k, NULL, 0);
        for (ch = 0; ch < 3; ch++) {
            bad += chk_drv(ch, &r[ch], k * P60);
            if (k == 11) v11[ch] = ytl_value(&g_tl, ch);
            if (k == 12) v12[ch] = ytl_value(&g_tl, ch);
        }
    }
    if (bad) fail_i(__LINE__, "retargeted values off the model", bad, 0);
    CHECK_F(v11[0], 100.0 * (double)(11 * P60) / 1e9, 1e-4);   /* where the ramp would be */
    CHECK_F(v11[1], 100.0 * (double)(11 * P60) / 1e9, 1e-4);
    CHECK(v11[0] - v10[0] > 1.0f);   /* no repeated position */
    CHECK(v11[1] - v10[1] > 1.0f);
    CHECK(v12[0] < v11[0]);          /* LINEAR turns at once */
    CHECK(v12[1] - v11[1] > 1.0f);   /* keep_velocity carries on */
    CHECK(v11[2] < v10[2]);          /* from the frame shown, toward 0 */
    CHECK_F(v11[2], (double)v10[2] * (1.0 - (double)P60 / 3e8), 1e-4);
}

static void rej_tw(int line, int ch, int base, const ytl_tween_desc* d) {
    int ret = ytl_tween(&g_tl, ch, base, d);
    if (ret != YTL_ERR_ARG) fail_i(line, "ytl_tween did not reject with YTL_ERR_ARG", ret, YTL_ERR_ARG);
}

static void acc_tw(int line, int ch, const ytl_tween_desc* d) {
    int ret = ytl_tween(&g_tl, ch, 1, d);
    if (ret != 0) fail_i(line, "ytl_tween rejected a good desc", ret, 0);
}

/* Every rejection TWEENS lists, each alone; none changes the channel. */
static void test_tween_reject(void) {
    static ytl_key kr[2];
    const int64_t M = MS_NS, R = S_NS;
    const int CYC_MIN = -2147483647 - 1;
    ytl_tween_desc g, b;
    int pass, ch;

    kr[0] = mkkey(0, 0.0f, YTL_EASE_LINEAR);
    kr[1] = mkkey(S_NS, 10.0f, YTL_EASE_LINEAR);
    for (pass = 0; pass < 2; pass++) {
        /* 0: a channel with a track and a waiting tween; 1: a free one. */
        ch = pass;
        tw_open();
        CHECK_I(ytl_set_keys(&g_tl, 0, 1, kr, 2), 0);
        g = twd_at(50.0f, 0, 300 * M);
        CHECK_I(ytl_tween(&g_tl, 0, 1, &g), 0);
        g = twd_at(2.0f, 100 * M, 200 * M);
        CHECK_I(ytl_tween(&g_tl, ch, 1, NULL), YTL_ERR_ARG);
        b = g; b.duration_ns = -1; rej_tw(__LINE__, ch, 1, &b);
        b = g; b.start_ns = LIM62; rej_tw(__LINE__, ch, 1, &b);
        b = g; b.start_ns = -LIM62; rej_tw(__LINE__, ch, 1, &b);
        b = g; b.delay_ns = LIM62; rej_tw(__LINE__, ch, 1, &b);
        b = g; b.delay_ns = -LIM62; rej_tw(__LINE__, ch, 1, &b);
        b = g; b.start_set = false; b.delay_ns = LIM62; rej_tw(__LINE__, ch, 1, &b);
        b = g; b.start_ns = LIM62 - 100 * M; rej_tw(__LINE__, ch, 1, &b);   /* start + duration at 2^62 */
        b = g; b.start_ns = LIM62 - 200 * M; b.yoyo = true; rej_tw(__LINE__, ch, 1, &b);   /* x 2 */
        b = g; b.to = (float)make_nan(); rej_tw(__LINE__, ch, 1, &b);
        b = g; b.to = (float)make_inf(); rej_tw(__LINE__, ch, 1, &b);
        b = g; b.to = (float)-make_inf(); rej_tw(__LINE__, ch, 1, &b);
        b = g; b.from_set = true; b.from = (float)make_nan(); rej_tw(__LINE__, ch, 1, &b);
        b = g; b.from_set = true; b.from = (float)make_inf(); rej_tw(__LINE__, ch, 1, &b);
        b = g; b.from_set = true; b.from = (float)-make_inf(); rej_tw(__LINE__, ch, 1, &b);
        b = g; b.ease = NOT_EASE; rej_tw(__LINE__, ch, 1, &b);
        b = g; b.ease = -1; rej_tw(__LINE__, ch, 1, &b);
        b = g; b.ease = 255; rej_tw(__LINE__, ch, 1, &b);
        b = g; b.ease = YTL_EASE_BEZIER; b.curve = mkcurve(1.5f, 0.0f, 0.5f, 1.0f); rej_tw(__LINE__, ch, 1, &b);
        b = g; b.ease = YTL_EASE_BEZIER; b.curve = mkcurve(0.5f, 0.0f, -0.25f, 1.0f); rej_tw(__LINE__, ch, 1, &b);
        b = g; b.ease = YTL_EASE_BEZIER; b.curve = mkcurve(0.5f, 0.0f, 1.0000001f, 1.0f); rej_tw(__LINE__, ch, 1, &b);
        b = g; b.ease = YTL_EASE_BEZIER; b.curve = mkcurve((float)make_nan(), 0.0f, 0.5f, 1.0f); rej_tw(__LINE__, ch, 1, &b);
        b = g; b.ease = YTL_EASE_BEZIER; b.curve = mkcurve(0.5f, (float)make_nan(), 0.5f, 1.0f); rej_tw(__LINE__, ch, 1, &b);
        b = g; b.ease = YTL_EASE_BEZIER; b.curve = mkcurve(0.5f, 0.0f, 0.5f, (float)make_inf()); rej_tw(__LINE__, ch, 1, &b);
        b = g; b.ease = YTL_EASE_LOG; b.to = 5.0f; rej_tw(__LINE__, ch, 1, &b);   /* no from_set */
        b = g; b.ease = YTL_EASE_LOG; b.from_set = true; b.from = 2.0f; b.to = 0.0f; rej_tw(__LINE__, ch, 1, &b);
        b = g; b.ease = YTL_EASE_LOG; b.from_set = true; b.from = 2.0f; b.to = -1.0f; rej_tw(__LINE__, ch, 1, &b);
        b = g; b.ease = YTL_EASE_LOG; b.from_set = true; b.from = 0.0f; b.to = 5.0f; rej_tw(__LINE__, ch, 1, &b);
        b = g; b.ease = YTL_EASE_LOG; b.from_set = true; b.from = -3.0f; b.to = 5.0f; rej_tw(__LINE__, ch, 1, &b);
        b = g; b.cycles = -2; rej_tw(__LINE__, ch, 1, &b);
        b = g; b.cycles = CYC_MIN; rej_tw(__LINE__, ch, 1, &b);
        b = g; b.duration_ns = 0; b.cycles = 2; rej_tw(__LINE__, ch, 1, &b);
        b = g; b.duration_ns = 0; b.cycles = YTL_FOREVER; rej_tw(__LINE__, ch, 1, &b);
        b = g; b.duration_ns = 0; b.yoyo = true; rej_tw(__LINE__, ch, 1, &b);
        b = g; b.keep_velocity = true; b.ease = YTL_EASE_COSINE; rej_tw(__LINE__, ch, 1, &b);
        b = g; b.keep_velocity = true; b.ease = YTL_EASE_STEP; rej_tw(__LINE__, ch, 1, &b);
        b = g; b.keep_velocity = true; b.ease = YTL_EASE_BEZIER; b.curve = mkcurve(0.42f, 0.0f, 0.58f, 1.0f); rej_tw(__LINE__, ch, 1, &b);
        b = g; b.keep_velocity = true; b.from_set = true; b.from = 1.0f; rej_tw(__LINE__, ch, 1, &b);
        b = g; b.keep_velocity = true; b.yoyo = true; rej_tw(__LINE__, ch, 1, &b);
        b = g; b.keep_velocity = true; b.cycles = 2; rej_tw(__LINE__, ch, 1, &b);
        b = g; b.keep_velocity = true; b.cycles = YTL_FOREVER; rej_tw(__LINE__, ch, 1, &b);   /* literal: more than one */
        b = g; b.keep_velocity = true; b.duration_ns = 0; rej_tw(__LINE__, ch, 1, &b);
        /* Not TWEENS', but arguments. */
        rej_tw(__LINE__, -1, 1, &g);
        rej_tw(__LINE__, 8, 1, &g);
        rej_tw(__LINE__, ch, -1, &g);
        rej_tw(__LINE__, ch, YTL_MAX_BASES, &g);
        if (pass == 0) {
            /* The track and the waiting tween are as they were. */
            CHECK_I(ytl_anchor(&g_tl, 1, R, 0), 0);
            eval_at(&g_tl, R + 290 * M, P60, 0, NULL, 0);
            CHECK_F(ytl_value(&g_tl, 0), 2.9, 1e-6);
            eval_at(&g_tl, R + 300 * M, P60, 1, NULL, 0);
            CHECK(ytl_value(&g_tl, 0) == 50.0f);
        } else {
            /* Still free, and at its value. */
            CHECK(ytl_value(&g_tl, 1) == 0.5f);
            CHECK(add1(&g_tl, 2, 0, YTL_SET, 1, 1.0f) >= 0);
        }
    }

    /* Accepted edges, each on channel 7 (each replaces the last). */
    tw_open();
    g = twd_at(2.0f, 100 * M, 200 * M);
    b = g; b.start_ns = LIM62 - 100 * M - 1; acc_tw(__LINE__, 7, &b);
    b = g; b.start_ns = LIM62 - 200 * M - 1; b.yoyo = true; acc_tw(__LINE__, 7, &b);
    b = g; b.start_ns = -LIM62 + 1; acc_tw(__LINE__, 7, &b);
    b = g; b.start_ns = 0; b.delay_ns = -LIM62 + 1; acc_tw(__LINE__, 7, &b);
    b = g; b.delay_ns = -150 * M; acc_tw(__LINE__, 7, &b);
    b = g; b.duration_ns = 0; acc_tw(__LINE__, 7, &b);
    /* Literal: from is checked only with from_set, the curve only with
     * BEZIER, and cycles alone are not multiplied into the time limit. */
    b = g; b.from = (float)make_nan(); acc_tw(__LINE__, 7, &b);
    b = g; b.curve = mkcurve(2.0f, (float)make_nan(), -1.0f, 0.0f); acc_tw(__LINE__, 7, &b);
    b = g; b.start_ns = LIM62 - 200 * M - 1; b.cycles = 5; acc_tw(__LINE__, 7, &b);
    b = g; b.ease = YTL_EASE_BEZIER; acc_tw(__LINE__, 7, &b);   /* cubic-bezier(0, 0, 0, 0) is a curve */
    b = g; b.ease = YTL_EASE_LOG; b.from_set = true; b.from = 2.0f; b.to = 5.0f; acc_tw(__LINE__, 7, &b);
    b = g; b.keep_velocity = true; b.cycles = 1; acc_tw(__LINE__, 7, &b);
    b = g; b.keep_velocity = true; acc_tw(__LINE__, 7, &b);
    b = g; b.cycles = YTL_FOREVER; acc_tw(__LINE__, 7, &b);
    b = g; b.cycles = 1000000; b.yoyo = true; acc_tw(__LINE__, 7, &b);
    b = g; b.ease = YTL_EASE_STEP; b.yoyo = true; acc_tw(__LINE__, 7, &b);
    b = g; b.to = -3.4e38f; acc_tw(__LINE__, 7, &b);
}

/* Cancellation and replacement: set_track (NULL and set_keys too) cancels
 * a waiting tween; a second tween before the first starts replaces it,
 * later or earlier; a clear of the base cancels it (literal: the channel
 * is freed). */
static void test_tween_cancel(void) {
    static ytl_key kr[2], kn[2];
    const int64_t M = MS_NS, R = S_NS;
    ytl_track tr, tn;
    ytl_tween_desc d;
    int64_t bt;
    int ch;

    kr[0] = mkkey(0, 0.0f, YTL_EASE_LINEAR);
    kr[1] = mkkey(S_NS, 10.0f, YTL_EASE_LINEAR);
    kn[0] = mkkey(0, 0.0f, YTL_EASE_LINEAR);
    kn[1] = mkkey(S_NS, -10.0f, YTL_EASE_LINEAR);
    tr = trk_keys(kr, 2);
    tn = trk_keys(kn, 2);
    tw_open();
    for (ch = 0; ch < 7; ch++)
        if (ch != 2 && ch != 5) CHECK_I(ytl_set_track(&g_tl, ch, 1, &tr), 0);
    d = twd_at(50.0f, 0, 300 * M);
    for (ch = 0; ch < 7; ch++)
        if (ch != 5) CHECK_I(ytl_tween(&g_tl, ch, 1, &d), 0);
    CHECK_I(ytl_tween(&g_tl, 5, 2, &d), 0);
    CHECK_I(ytl_anchor(&g_tl, 1, R, 0), 0);
    CHECK_I(ytl_anchor(&g_tl, 2, R, 0), 0);
    for (bt = 0; bt <= 600 * M; bt += 10 * M) {
        double ramp = 10.0 * (double)bt / 1e9;
        eval_at(&g_tl, R + bt, P60, bt / (10 * M), NULL, 0);
        if (bt <= 200 * M) {
            for (ch = 0; ch < 7; ch++)
                if (ch != 2 && ch != 5) CHECK_F(ytl_value(&g_tl, ch), ramp, 1e-6);
            CHECK(ytl_value(&g_tl, 2) == 0.5f);
            CHECK(ytl_value(&g_tl, 5) == 0.5f);
        } else {
            CHECK_F(ytl_value(&g_tl, 0), -ramp, 1e-6);   /* the new track, no takeover */
            CHECK(ytl_value(&g_tl, 1) == 0.5f);           /* removed: initial, no takeover */
            CHECK(ytl_value(&g_tl, 2) == 0.5f);
            if (bt < 400 * M) CHECK_F(ytl_value(&g_tl, 3), ramp, 1e-6);
            else CHECK(ytl_value(&g_tl, 3) == -50.0f);
            if (bt < 250 * M) CHECK_F(ytl_value(&g_tl, 4), ramp, 1e-6);
            else CHECK(ytl_value(&g_tl, 4) == -50.0f);
            CHECK(ytl_value(&g_tl, 5) == 0.5f);
            CHECK_F(ytl_value(&g_tl, 6), -ramp, 1e-6);
        }
        if (bt == 200 * M) {
            CHECK_I(ytl_set_track(&g_tl, 0, 1, &tn), 0);
            CHECK_I(ytl_set_track(&g_tl, 1, 1, NULL), 0);
            CHECK(ytl_value(&g_tl, 1) == 0.5f);
            CHECK_I(ytl_set_keys(&g_tl, 2, 1, NULL, 0), 0);
            d = twd_at(-50.0f, 0, 400 * M);
            CHECK_I(ytl_tween(&g_tl, 3, 1, &d), 0);
            d = twd_at(-50.0f, 0, 250 * M);
            CHECK_I(ytl_tween(&g_tl, 4, 1, &d), 0);
            CHECK_I(ytl_clear(&g_tl, 2), 0);
            CHECK(ytl_value(&g_tl, 5) == 0.5f);
            CHECK_I(ytl_set_keys(&g_tl, 6, 1, kn, 2), 0);
        }
    }
    /* Removed, set_keys'd away and cleared channels are free. */
    CHECK(add1(&g_tl, 3, 0, YTL_SET, 1, 1.0f) >= 0);
    CHECK(add1(&g_tl, 3, 0, YTL_SET, 2, 1.0f) >= 0);
    CHECK(add1(&g_tl, 3, 0, YTL_SET, 5, 1.0f) >= 0);
}

/* A track on another base stops at once at its last value; a tween on
 * another base replaces a waiting one. */
static void test_tween_cross_base(void) {
    static ytl_key kb[2], kr[2];
    const int64_t M = MS_NS, R = S_NS;
    ytl_tween_desc d;
    int64_t bt;
    double u;

    kb[0] = mkkey(5 * S_NS, 0.0f, YTL_EASE_LINEAR);
    kb[1] = mkkey(6 * S_NS, 10.0f, YTL_EASE_LINEAR);
    kr[0] = mkkey(0, 0.0f, YTL_EASE_LINEAR);
    kr[1] = mkkey(S_NS, 10.0f, YTL_EASE_LINEAR);
    tw_open();
    CHECK_I(ytl_set_keys(&g_tl, 0, 2, kb, 2), 0);
    CHECK_I(ytl_set_keys(&g_tl, 1, 2, kb, 2), 0);
    CHECK_I(ytl_set_keys(&g_tl, 2, 1, kr, 2), 0);
    d = twd_at(50.0f, 0, 300 * M);
    CHECK_I(ytl_tween(&g_tl, 2, 1, &d), 0);
    CHECK_I(ytl_anchor(&g_tl, 1, R, 0), 0);
    CHECK_I(ytl_anchor(&g_tl, 2, R, 5 * S_NS), 0);
    for (bt = 0; bt <= 100 * M; bt += 10 * M) eval_at(&g_tl, R + bt, P60, bt / (10 * M), NULL, 0);
    CHECK_F(ytl_value(&g_tl, 0), 1.0, 1e-6);
    CHECK_F(ytl_value(&g_tl, 2), 1.0, 1e-6);
    d = twd_at(20.0f, 100 * M, 300 * M);
    CHECK_I(ytl_tween(&g_tl, 0, 1, &d), 0);
    CHECK_F(ytl_value(&g_tl, 0), 1.0, 1e-6);   /* stopped, at its value */
    d.keep_velocity = true;
    CHECK_I(ytl_tween(&g_tl, 1, 1, &d), 0);
    /* Channel 2: a tween on base 2 replaces the waiting one on base 1 and
     * stops the base-1 track. */
    d = twd_at(-5.0f, 0, 5 * S_NS + 200 * M);
    CHECK_I(ytl_tween(&g_tl, 2, 2, &d), 0);
    CHECK_I(add1(&g_tl, 2, 0, YTL_SET, 0, 1.0f), YTL_ERR_BOUND);
    CHECK_I(add1(&g_tl, 1, 0, YTL_SET, 0, 1.0f), YTL_ERR_BOUND);
    for (bt = 110 * M; bt <= 500 * M; bt += 10 * M) {
        float v0, v1, v2;
        eval_at(&g_tl, R + bt, P60, bt / (10 * M), NULL, 0);
        v0 = ytl_value(&g_tl, 0);
        v1 = ytl_value(&g_tl, 1);
        v2 = ytl_value(&g_tl, 2);
        if (bt < 300 * M) {
            CHECK_F(v0, 1.0, 1e-6);
            CHECK_F(v1, 1.0, 1e-6);
        } else {
            u = bt >= 400 * M ? 1.0 : (double)(bt - 300 * M) / 1e8;
            CHECK_F(v0, 1.0 + 19.0 * u, 1e-5);
            /* Literal: a stopped track has no velocity, so keep_velocity
             * starts at rest. */
            CHECK_F(v1, 1.0 + 19.0 * (3.0 * u * u - 2.0 * u * u * u), 1e-5);
        }
        if (bt < 200 * M) CHECK_F(v2, 1.0, 1e-6);
        else CHECK(v2 == -5.0f);
    }
}

/* A rewind does not undo a takeover; a waiting tween survives a rewind. */
static void test_tween_rewind(void) {
    static ytl_key kr[2];
    const int64_t M = MS_NS, R = S_NS;
    ytl_track tr;
    ytl_tween_desc d;
    rdrv old, r0;
    int64_t bt, onset = R, idx = 0;
    int bad = 0;

    g_cv_prints = 0;
    kr[0] = mkkey(0, 0.0f, YTL_EASE_LINEAR);
    kr[1] = mkkey(S_NS, 10.0f, YTL_EASE_LINEAR);
    tr = trk_keys(kr, 2);
    old = rdrv_plain(&tr);
    tw_open();
    CHECK_I(ytl_set_track(&g_tl, 0, 1, &tr), 0);
    d = twd_at(50.0f, 200 * M, 300 * M);
    CHECK_I(ytl_tween(&g_tl, 0, 1, &d), 0);
    rdrv_tween(&r0, &d, 300 * M, &old, 0.0, 0.0);
    CHECK_I(ytl_anchor(&g_tl, 1, R, 0), 0);
    for (bt = 0; bt <= 600 * M; bt += 10 * M) {
        onset = R + bt;
        eval_at(&g_tl, onset, P60, idx++, NULL, 0);
        bad += chk_drv(0, bt < 300 * M ? &old : &r0, bt);
    }
    onset += 10 * M;
    CHECK_I(ytl_anchor(&g_tl, 1, onset, 0), 0);
    for (bt = 0; bt <= 500 * M; bt += 10 * M) {
        eval_at(&g_tl, onset + bt, P60, idx++, NULL, 0);
        bad += chk_drv(0, &r0, bt);
        if (bt == 100 * M) CHECK_F(ytl_value(&g_tl, 0), 3.0, 1e-6);   /* the tween's from, not the ramp's 1 */
        if (bt == 400 * M) CHECK_F(ytl_value(&g_tl, 0), 26.5, 1e-5);
    }
    if (bad) fail_i(__LINE__, "values after a rewind off the takeover", bad, 0);

    /* A tween still waiting survives a rewind and takes over at its start
     * from the old track's value there. */
    tw_open();
    CHECK_I(ytl_set_track(&g_tl, 0, 1, &tr), 0);
    d = twd_at(50.0f, 0, 300 * M);
    CHECK_I(ytl_tween(&g_tl, 0, 1, &d), 0);
    CHECK_I(ytl_anchor(&g_tl, 1, R, 0), 0);
    for (bt = 0; bt <= 200 * M; bt += 10 * M) {
        onset = R + bt;
        eval_at(&g_tl, onset, P60, idx++, NULL, 0);
    }
    onset += 10 * M;
    CHECK_I(ytl_anchor(&g_tl, 1, onset, 0), 0);
    for (bt = 0; bt <= 400 * M; bt += 10 * M) {
        eval_at(&g_tl, onset + bt, P60, idx++, NULL, 0);
        if (bt < 300 * M) CHECK_F(ytl_value(&g_tl, 0), 10.0 * (double)bt / 1e9, 1e-6);
        else CHECK(ytl_value(&g_tl, 0) == 50.0f);
    }
}

/* ----------------------------------- 5g. evaluate writes every kind of track */

static int te_check(const ytl_track* trs, int64_t onset, int64_t P, int64_t idx) {
    int ch, bad = 0;
    eval_at(&g_tl, onset, P, idx, NULL, 0);
    for (ch = 0; ch < 6; ch++) bad += chan_vs(ch, &trs[ch], read_bt(2, &trs[ch], onset, P / 2));
    return bad;
}

static void test_track_eval(void) {
    static ytl_key k0[5], k3[4], tk[2];
    static ytl_curve c0[2], tc;
    static float s1[400], s2[30000], s5[50];
    static ytl_track trs[6];
    static const int64_t rates[3] = { 1000000, 16666667, 6944444 };
    const int64_t M = MS_NS;
    float init[6] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.75f, 0.0f };
    int64_t onset = S_NS, idx = 0, P, bt = 0;
    float prev[6];
    int i, k, ch, bad = 0, bad_p = 0, seq;

    g_cv_prints = 0;
    for (i = 0; i < 400; i++) s1[i] = (float)(3.0 * sin(0.05 * (double)i) + 0.5 * cos(0.31 * (double)i));
    for (i = 0; i < 30000; i++) s2[i] = (float)((i * 7919) % 1000) / 100.0f;
    for (i = 0; i < 50; i++) s5[i] = (float)(i % 7) - 3.0f;
    c0[0] = mkcurve(0.68f, -0.6f, 0.32f, 1.6f);
    c0[1] = mkcurve(0.42f, 0.0f, 0.58f, 1.0f);
    k0[0] = mkbez(0, 1.0f, 1);
    k0[1] = mkkey(300 * M, 5.0f, YTL_EASE_COSINE);
    k0[2] = mkbez(500 * M, 2.0f, 0);
    k0[3] = mkkey(900 * M, 3.0f, YTL_EASE_QUAD_IN);
    k0[4] = mkkey(1200 * M, 4.0f, YTL_EASE_LINEAR);
    trs[0] = trk_keys(k0, 5);
    trs[0].curves = c0;
    trs[0].n_curves = 2;
    trs[1] = trk_samples(s1, 400, -500 * M, 120, YTL_INTERP_CUBIC);
    trs[2] = trk_samples(s2, 30000, 1 * M, 999983, YTL_INTERP_LINEAR);
    trs[2].period_ns = 37 * M;
    k3[0] = mkkey(200 * M, 0.0f, YTL_EASE_STEP);
    k3[1] = mkkey(300 * M, 1.0f, YTL_EASE_QUAD_IN);
    k3[2] = mkkey(400 * M, 3.0f, YTL_EASE_LINEAR);
    k3[3] = mkkey(450 * M, 2.0f, YTL_EASE_LINEAR);
    trs[3] = trk_keys(k3, 4);
    trs[3].period_ns = 250 * M;
    trs[3].repeats = 7;
    trs[5] = trk_samples(s5, 50, -2 * S_NS, 7, YTL_INTERP_STEP);   /* a STEP track */
    trs[5].period_ns = 3 * S_NS + 1;
    trs[5].repeats = 2;

    CHECK(open_h(&g_tl, g_store, 8, 6, init, 0.5));
    for (ch = 0; ch < 6; ch++)
        if (ch != 4) CHECK_I(ytl_set_track(&g_tl, ch, 2, &trs[ch]), 0);
    {
        /* No track before it: from is the channel's value, 0.75, held
         * until the start, as the tween's first key gives before it. */
        ytl_tween_desc d = twd_at(-3.0f, 700 * M, 100 * M);
        d.ease = YTL_EASE_QUAD_OUT;
        CHECK_I(ytl_tween(&g_tl, 4, 2, &d), 0);
    }
    trs[4] = tween_track(tk, &tc, 100 * M, 700 * M, 0.75f, -3.0f, YTL_EASE_QUAD_OUT, NULL);
    for (ch = 0; ch < 6; ch++) CHECK(ytl_value(&g_tl, ch) == init[ch]);   /* nothing before an evaluate */
    CHECK_I(ytl_anchor(&g_tl, 2, onset, -600 * M), 0);

    g_rng = 2468;
    for (seq = 0; seq < 6; seq++) {
        int nf = seq == 0 ? 3000 : seq == 1 ? 300 : seq == 2 ? 500 : seq == 3 ? 1000 : seq == 4 ? 2000 : 1500;
        P = seq == 0 ? rates[0] : seq == 4 ? rates[2] : rates[1];
        if (seq == 4) CHECK_I(ytl_anchor(&g_tl, 2, onset, -100 * M), 0);
        for (k = 0; k < nf; k++) {
            if (seq == 2) {   /* backward, a frame at a time */
                CHECK(ytl_base_time(&g_tl, 2, onset, &bt));
                CHECK_I(ytl_anchor(&g_tl, 2, onset, bt - rnd_range(0, 20 * M)), 0);
            } else if (seq == 3) {   /* jumps, a quarter of them onto keys and samples */
                int r = rnd_int(4);
                if (r == 0) bt = k0[rnd_int(5)].time_ns + rnd_range(-1, 1);
                else if (r == 1) bt = k3[rnd_int(4)].time_ns + rnd_range(-1, 1) + (int64_t)rnd_int(9) * 250 * M;
                else bt = rnd_range(-3 * S_NS, 10 * S_NS);
                CHECK_I(ytl_anchor(&g_tl, 2, onset, bt), 0);
            } else if (seq == 5) {   /* small steps within segments, then a jump */
                if (k % 100 == 0) CHECK_I(ytl_anchor(&g_tl, 2, onset, rnd_range(-S_NS, 8 * S_NS)), 0);
            }
            bad += te_check(trs, onset, P, idx++);
            onset += seq == 3 ? rnd_range(1, P) : seq == 5 ? rnd_range(0, 3 * M) : P;
        }
    }
    if (bad) fail_i(__LINE__, "tracked channels off ytl_sample at the read time", bad, 0);

    /* Paused: every track at the frozen time, frame after frame. */
    CHECK_I(ytl_pause(&g_tl, 2, onset), 0);
    bad = te_check(trs, onset, P60, idx++);
    for (ch = 0; ch < 6; ch++) prev[ch] = ytl_value(&g_tl, ch);
    for (k = 0; k < 20; k++) {
        onset += P60;
        bad += te_check(trs, onset, P60, idx++);
        for (ch = 0; ch < 6; ch++)
            if (ytl_value(&g_tl, ch) != prev[ch]) bad_p++;
    }
    if (bad) fail_i(__LINE__, "paused tracked channels off ytl_sample", bad, 0);
    if (bad_p) fail_i(__LINE__, "paused tracked channels moved", bad_p, 0);
}

/* ------------------------------------- 5h. STEP tracks at the lead window */

/* A STEP track is read at the end of the lead window, so its change
 * lands on the frame an event at the same time lands on, at any lead; a
 * LINEAR track with the same keys, and a mixed track, at the onset. */
static void test_step_read(void) {
    static ytl_key ks[2], kl[2], km[4], tk[2];
    static float s01[2] = { 0.0f, 1.0f };
    static ytl_curve tc;
    static const int lnum[6] = { 0, 1, 1, 3, 999, 1 }, lden[6] = { 1, 4, 2, 4, 1000, 3 };
    const int64_t M = MS_NS, R = S_NS;
    ytl_track trs[6];
    ytl_tween_desc td;
    int trial, ch, bad = 0, bad_lin = 0, early = 0, lead_frames = 0;

    /* By hand, P = 10 ms: STEP keys change at 25 ms. Lead 0: the frame at
     * 30 ms shows it; lead 0.5: the frame at 20 ms (window end 25 ms). */
    for (trial = 0; trial < 2; trial++) {
        int64_t bt;
        ks[0] = mkkey(15 * M, 0.0f, YTL_EASE_STEP);
        ks[1] = mkkey(25 * M, 1.0f, YTL_EASE_LINEAR);   /* the last key's ease is not used */
        kl[0] = mkkey(15 * M, 0.0f, YTL_EASE_LINEAR);
        kl[1] = mkkey(25 * M, 1.0f, YTL_EASE_LINEAR);
        km[0] = mkkey(0, 0.0f, YTL_EASE_LINEAR);
        km[1] = mkkey(10 * M, 0.0f, YTL_EASE_STEP);
        km[2] = mkkey(25 * M, 1.0f, YTL_EASE_LINEAR);
        km[3] = mkkey(40 * M, 1.0f, YTL_EASE_LINEAR);
        CHECK(open_h(&g_tl, g_store, 8, 7, NULL, trial ? 0.5 : 0.0));
        CHECK_I(ytl_set_keys(&g_tl, 0, 1, ks, 2), 0);
        CHECK_I(ytl_set_keys(&g_tl, 1, 1, kl, 2), 0);
        CHECK_I(ytl_set_keys(&g_tl, 2, 1, km, 4), 0);
        td = twd_at(1.0f, 10 * M, 15 * M);
        td.ease = YTL_EASE_STEP;
        CHECK_I(ytl_tween(&g_tl, 3, 1, &td), 0);
        td = twd_at(1.0f, 0, 25 * M);
        td.ease = YTL_EASE_STEP;
        CHECK_I(ytl_tween(&g_tl, 4, 1, &td), 0);
        td.ease = YTL_EASE_LINEAR;
        CHECK_I(ytl_tween(&g_tl, 5, 1, &td), 0);
        CHECK(add1(&g_tl, 1, 25 * M, YTL_SET, 6, 1.0f) >= 0);
        CHECK_I(ytl_anchor(&g_tl, 1, R, 0), 0);
        for (bt = 0; bt <= 40 * M; bt += 10 * M) {
            float want_step = (trial ? bt + 5 * M : bt) >= 25 * M ? 1.0f : 0.0f;
            eval_at(&g_tl, R + bt, 10 * M, bt / (10 * M), NULL, 0);
            CHECK(ytl_value(&g_tl, 0) == want_step);
            CHECK(ytl_value(&g_tl, 3) == want_step);   /* tween with STEP ease */
            CHECK(ytl_value(&g_tl, 4) == want_step);   /* STEP jump, duration 0 */
            CHECK(ytl_value(&g_tl, 6) == want_step);   /* the event */
            CHECK(ytl_value(&g_tl, 1) == (float)ksample(kl, 2, bt));   /* at the onset */
            CHECK(ytl_value(&g_tl, 2) == (float)ksample(km, 4, bt));   /* mixed: at the onset */
            CHECK(ytl_value(&g_tl, 5) == (bt >= 25 * M ? 1.0f : 0.0f));   /* LINEAR jump: at the onset */
            if (bt == 20 * M) {
                CHECK_F(ytl_value(&g_tl, 1), 0.5, 1e-6);
                CHECK(ytl_value(&g_tl, 2) == 0.0f);
            }
        }
    }

    /* Random: change time te, lead, period (0 is variable refresh), and
     * onset noise. Channel 0 an event at te; 1 keyed STEP keys; 2 sampled
     * STEP at 1e9/s; 3 sampled STEP at 7/s with sample 1 just before te;
     * 4 a STEP tween; 5 LINEAR with channel 1's keys, read at the onset. */
    g_rng = 8080;
    for (trial = 0; trial < 300; trial++) {
        int li = trial % 6;
        int64_t P = trial % 7 == 0 ? 0 : trial % 3 == 0 ? P60 : trial % 3 == 1 ? 6944444 : 2000000;
        int64_t te = rnd_range(0, 50 * P60), d = rnd_range(1, 3 * P60), onset = R, bt0 = rnd_range(-3 * P60, 0);
        int64_t k;
        CHECK(open_h(&g_tl, g_store, 8, 6, NULL, (double)lnum[li] / (double)lden[li]));
        ks[0] = mkkey(te - d, 0.0f, YTL_EASE_STEP);
        ks[1] = mkkey(te, 1.0f, YTL_EASE_LINEAR);
        kl[0] = mkkey(te - d, 0.0f, YTL_EASE_LINEAR);
        kl[1] = mkkey(te, 1.0f, YTL_EASE_LINEAR);
        trs[1] = trk_keys(ks, 2);
        trs[2] = trk_samples(s01, 2, te - 1, NS_E9, YTL_INTERP_STEP);
        trs[3] = trk_samples(s01, 2, te - 142857143, 7, YTL_INTERP_STEP);   /* sample 1 at te - 1/7 ns */
        trs[5] = trk_keys(kl, 2);
        CHECK(add1(&g_tl, 1, te, YTL_SET, 0, 1.0f) >= 0);
        for (ch = 1; ch < 6; ch++)
            if (ch != 4) CHECK_I(ytl_set_track(&g_tl, ch, 1, &trs[ch]), 0);
        td = twd_at(1.0f, 5, te - 5);
        td.ease = YTL_EASE_STEP;
        CHECK_I(ytl_tween(&g_tl, 4, 1, &td), 0);
        trs[4] = tween_track(tk, &tc, te - 5, 5, 0.0f, 1.0f, YTL_EASE_STEP, NULL);
        CHECK_I(ytl_anchor(&g_tl, 1, R, bt0), 0);
        for (k = 0; ; k++) {
            int64_t bt;
            float ve;
            eval_at(&g_tl, onset, P, k, NULL, 0);
            bt = bt0 + (onset - R);
            ve = ytl_value(&g_tl, 0);
            for (ch = 1; ch < 5; ch++) {
                if (ytl_value(&g_tl, ch) != ve) {
                    if (!bad) fprintf(stderr, "  step read: trial %d lead %d/%d P %lld te %lld frame bt %lld: "
                                      "channel %d %g, event channel %g\n", trial, lnum[li], lden[li],
                                      (long long)P, (long long)te, (long long)bt, ch,
                                      (double)ytl_value(&g_tl, ch), (double)ve);
                    bad++;
                }
            }
            if (ytl_value(&g_tl, 5) != (float)ytl_sample(&trs[5], bt)) bad_lin++;
            if (ve == 1.0f && bt < te) early++;
            if (bt > te + 3 * (P ? P : 20 * M)) break;
            if (lnum[li] > 0 && P > 0) lead_frames++;
            onset += P ? P + rnd_range(-P / 50, P / 50) : rnd_range(4 * M, 20 * M);
        }
    }
    if (bad) fail_i(__LINE__, "STEP track changes not on the event's frame", bad, 0);
    if (bad_lin) fail_i(__LINE__, "LINEAR track with STEP's keys not read at the onset", bad_lin, 0);
    if (early == 0 || lead_frames == 0) fail(__LINE__, "step read: no frame showed a change early (vacuous)");
}

/* --------------------------------------------------------- 6. the report */

static int g_rep[8];

static void report_setup(void) {
    CHECK(open_h(&g_tl, g_store, 32, 2, NULL, 0.0));
    CHECK_I(ytl_anchor(&g_tl, 1, 1000, 0), 0);
    CHECK_I(ytl_anchor(&g_tl, 2, 2000, 500), 0);
    g_rep[0] = add1(&g_tl, 0, 1300, YTL_MARK, 0, 0.0f);    /* RT 1300 */
    g_rep[1] = add1(&g_tl, 1, 100, YTL_MARK, 0, 0.0f);     /* RT 1100 */
    g_rep[2] = add1(&g_tl, 2, 0, YTL_MARK, 0, 0.0f);       /* RT 1500 */
    g_rep[3] = add1(&g_tl, 1, 300, YTL_MARK, 0, 0.0f);     /* RT 1300 */
    g_rep[4] = add1(&g_tl, 0, 1100, YTL_MARK, 0, 0.0f);    /* RT 1100 */
    g_rep[5] = add1(&g_tl, 2, -400, YTL_MARK, 0, 0.0f);    /* RT 1100 */
    g_rep[6] = add1(&g_tl, 0, 1050, YTL_TRIGGER, 0, 0.0f); /* RT 1050 */
    g_rep[7] = add1(&g_tl, 1, 5000, YTL_MARK, 0, 0.0f);    /* RT 6000, pending */
}

static void test_report(void) {
    static const int order[7] = { 6, 1, 4, 5, 0, 3, 2 };
    static const int64_t rt[7] = { 1050, 1100, 1100, 1100, 1300, 1300, 1500 };
    int i, n;

    report_setup();
    n = eval_at(&g_tl, 3000, 0, 7, g_fired, 16);
    CHECK_I(n, 7);
    for (i = 0; i < 7 && i < n; i++) {
        CHECK_I(g_fired[i].id, g_rep[order[i]]);
        CHECK_I(g_fired[i].onset - g_fired[i].residual, rt[i]);
        CHECK_I(g_fired[i].frame, 7);
    }
    CHECK(is_pending_reset(ytl_find(&g_tl, g_rep[7])));

    report_setup();
    for (i = 0; i < 8; i++) g_fired[i].id = -999;
    n = eval_at(&g_tl, 3000, 0, 7, g_fired, 3);
    CHECK_I(n, 7);
    CHECK_I(g_fired[0].id, g_rep[6]);
    CHECK_I(g_fired[1].id, g_rep[1]);
    CHECK_I(g_fired[2].id, g_rep[4]);
    CHECK_I(g_fired[3].id, -999);
    for (i = 0; i < 7; i++) {
        const ytl_event* e = ytl_find(&g_tl, g_rep[i]);
        CHECK(e && (e->flags & YTL_EV_FIRED) && e->frame == 7);
    }

    report_setup();
    CHECK_I(eval_at(&g_tl, 3000, 0, 7, NULL, 0), 7);
    CHECK_I(eval_at(&g_tl, 2999, 0, 8, NULL, 0), YTL_ERR_ORDER);
    CHECK_I(eval_at(&g_tl, 3000, -1, 8, NULL, 0), YTL_ERR_ARG);
    CHECK_I(eval_at(&g_tl, 3000, 0, 8, g_fired, -1), YTL_ERR_ARG);
    CHECK_I(eval_at(&g_tl, 3000, 0, 8, NULL, 2), YTL_ERR_ARG);
    /* Equal onset: only what was added since. */
    CHECK_I(eval_at(&g_tl, 3000, 0, 8, NULL, 0), 0);
    i = add1(&g_tl, 0, 2500, YTL_MARK, 0, 0.0f);
    n = eval_at(&g_tl, 3000, 0, 9, g_fired, 4);
    CHECK_I(n, 1);
    if (n == 1) {
        CHECK_I(g_fired[0].id, i);
        CHECK_I(g_fired[0].residual, 500);
        CHECK_I(g_fired[0].flags, YTL_EV_FIRED | YTL_EV_LATE);
    }
    i = add1(&g_tl, 1, 1999, YTL_MARK, 0, 0.0f);
    n = eval_at(&g_tl, 3000, 0, 10, g_fired, 4);
    CHECK_I(n, 1);
    if (n == 1) { CHECK_I(g_fired[0].id, i); CHECK_I(g_fired[0].residual, 1); }
    CHECK_I(eval_at(&g_tl, 3000, 0, 11, NULL, 0), 0);
}

/* --------------------------------------------------------- 7. late events */

static void test_late(void) {
    const int64_t P = P60, O = S_NS, W = P60 / 2;   /* 8333333 */
    int a, b, c, d, f, g, n, i;

    CHECK(open_h(&g_tl, g_store, 32, 2, NULL, 0.5));
    CHECK_I(eval_at(&g_tl, O, P, 0, NULL, 0), 0);
    a = add1(&g_tl, 0, O + W, YTL_MARK, 0, 0.0f);       /* at the window: late */
    b = add1(&g_tl, 0, O + W - 5, YTL_MARK, 0, 0.0f);
    c = add1(&g_tl, 0, O + W + 1, YTL_MARK, 0, 0.0f);   /* past it: not late */
    d = add1(&g_tl, 0, 0, YTL_MARK, 0, 0.0f);
    CHECK_I(ytl_anchor(&g_tl, 1, O, 0), 0);
    n = eval_at(&g_tl, O + P, P, 1, g_fired, 8);
    CHECK_I(n, 4);
    if (n == 4) {
        CHECK_I(g_fired[0].id, d);
        CHECK_I(g_fired[1].id, b);
        CHECK_I(g_fired[2].id, a);
        CHECK_I(g_fired[3].id, c);
    }
    CHECK(ytl_find(&g_tl, a) && (unsigned)ytl_find(&g_tl, a)->flags == (YTL_EV_FIRED | YTL_EV_LATE));
    CHECK(ytl_find(&g_tl, b) && (unsigned)ytl_find(&g_tl, b)->flags == (YTL_EV_FIRED | YTL_EV_LATE));
    CHECK(ytl_find(&g_tl, c) && (unsigned)ytl_find(&g_tl, c)->flags == YTL_EV_FIRED);
    CHECK(ytl_find(&g_tl, d) && (unsigned)ytl_find(&g_tl, d)->flags == (YTL_EV_FIRED | YTL_EV_LATE));
    CHECK(ytl_find(&g_tl, a) && ytl_find(&g_tl, a)->residual == P - W);
    CHECK(ytl_find(&g_tl, c) && ytl_find(&g_tl, c)->residual == P - W - 1);
    CHECK(ytl_find(&g_tl, d) && ytl_find(&g_tl, d)->residual == O + P);
    /* Base 1: its last window ended at P + W. */
    f = add1(&g_tl, 1, P, YTL_MARK, 0, 0.0f);
    g = add1(&g_tl, 1, P + W + 1, YTL_MARK, 0, 0.0f);
    n = eval_at(&g_tl, O + 2 * P, P, 2, g_fired, 8);
    CHECK_I(n, 2);
    for (i = 0; i < n && i < 2; i++) {
        if (g_fired[i].id == f) {
            CHECK_I(g_fired[i].flags, YTL_EV_FIRED | YTL_EV_LATE);
            CHECK_I(g_fired[i].residual, P);
        } else {
            CHECK_I(g_fired[i].id, g);
            CHECK_I(g_fired[i].flags, YTL_EV_FIRED);
            CHECK_I(g_fired[i].residual, P - W - 1);
        }
    }
}

/* ---------------------------------------------------------------- 8. rewind */

static void test_rewind(void) {
    const int64_t P = 10 * MS_NS, R = S_NS, M = MS_NS;
    int id[8], k, n, y;
    const ytl_event* e;

    CHECK(open_h(&g_tl, g_store, 32, 4, NULL, 0.0));
    id[0] = add1(&g_tl, 1, 0, YTL_SET, 0, 1.0f);
    id[1] = add1(&g_tl, 1, 100 * M, YTL_SET, 0, 2.0f);
    id[2] = add1(&g_tl, 1, 200 * M, YTL_SET, 0, 3.0f);
    id[3] = add1(&g_tl, 1, 300 * M, YTL_SET, 0, 4.0f);
    id[4] = add1(&g_tl, 1, 0, YTL_MARK, 0, 0.0f);
    id[5] = add1(&g_tl, 1, 150 * M, YTL_ONSET, 1, 0.0f);
    id[6] = add1(&g_tl, 1, 250 * M, YTL_TRIGGER, 0, 0.0f);
    CHECK_I(ytl_anchor(&g_tl, 1, R, 0), 0);
    for (k = 0; k <= 25; k++) eval_at(&g_tl, R + k * P, P, k, NULL, 0);
    CHECK(ytl_find(&g_tl, id[0]) && ytl_find(&g_tl, id[0])->frame == 0);
    CHECK(ytl_find(&g_tl, id[1]) && ytl_find(&g_tl, id[1])->frame == 10);
    CHECK(ytl_find(&g_tl, id[5]) && ytl_find(&g_tl, id[5])->frame == 15);
    CHECK(ytl_find(&g_tl, id[2]) && ytl_find(&g_tl, id[2])->frame == 20);
    CHECK(ytl_find(&g_tl, id[6]) && ytl_find(&g_tl, id[6])->frame == 25);
    CHECK(ytl_value(&g_tl, 0) == 3.0f);
    CHECK(ytl_value(&g_tl, 1) == 1.0f);

    /* Anchor back to 100 ms (the base's last onset was at 250 ms): every
     * fired event at or after 100 ms, 100 ms included, is pending again at
     * once, and the channels are recomputed. */
    CHECK_I(ytl_anchor(&g_tl, 1, R + 26 * P, 100 * M), 0);
    CHECK(is_pending_reset(ytl_find(&g_tl, id[1])));
    CHECK(is_pending_reset(ytl_find(&g_tl, id[2])));
    CHECK(is_pending_reset(ytl_find(&g_tl, id[5])));
    CHECK(is_pending_reset(ytl_find(&g_tl, id[6])));
    CHECK(is_pending_reset(ytl_find(&g_tl, id[3])));
    e = ytl_find(&g_tl, id[0]);
    CHECK(e && e->frame == 0 && e->onset == R && (unsigned)e->flags == YTL_EV_FIRED);
    e = ytl_find(&g_tl, id[4]);
    CHECK(e && e->frame == 0 && e->onset == R && (unsigned)e->flags == YTL_EV_FIRED);
    CHECK(ytl_value(&g_tl, 0) == 1.0f);
    CHECK(ytl_value(&g_tl, 1) == 0.0f);
    n = eval_at(&g_tl, R + 26 * P, P, 26, g_fired, 8);
    CHECK_I(n, 1);
    if (n >= 1) { CHECK_I(g_fired[0].id, id[1]); CHECK_I(g_fired[0].residual, 0); }
    CHECK(ytl_value(&g_tl, 0) == 2.0f);
    for (k = 27; k <= 46; k++) eval_at(&g_tl, R + k * P, P, k, NULL, 0);
    e = ytl_find(&g_tl, id[5]);
    CHECK(e && e->frame == 31 && e->onset == R + 31 * P && e->residual == 0 && (unsigned)e->flags == YTL_EV_FIRED);
    CHECK(ytl_find(&g_tl, id[2]) && ytl_find(&g_tl, id[2])->frame == 36);
    CHECK(ytl_find(&g_tl, id[6]) && ytl_find(&g_tl, id[6])->frame == 41);
    CHECK(ytl_find(&g_tl, id[3]) && ytl_find(&g_tl, id[3])->frame == 46);
    CHECK(ytl_value(&g_tl, 0) == 4.0f);
    CHECK(ytl_value(&g_tl, 1) == 1.0f);

    /* Base time of the last onset is 300 ms. An anchor exactly there
     * rewinds the event exactly there. */
    CHECK_I(ytl_anchor(&g_tl, 1, R + 47 * P, 300 * M), 0);
    CHECK(is_pending_reset(ytl_find(&g_tl, id[3])));
    CHECK(ytl_value(&g_tl, 0) == 3.0f);
    n = eval_at(&g_tl, R + 47 * P, P, 47, g_fired, 8);
    CHECK_I(n, 1);
    if (n >= 1) { CHECK_I(g_fired[0].id, id[3]); CHECK_I(g_fired[0].frame, 47); }

    /* An anchor at a later base time rewinds nothing; the next evaluate
     * fires what it passed over, late by the residual, not flagged LATE. */
    id[7] = add1(&g_tl, 1, 320 * M, YTL_SET, 0, 8.0f);
    CHECK_I(ytl_anchor(&g_tl, 1, R + 48 * P, 400 * M), 0);
    CHECK(ytl_find(&g_tl, id[3]) && ytl_find(&g_tl, id[3])->frame == 47);
    n = eval_at(&g_tl, R + 48 * P, P, 48, g_fired, 8);
    CHECK_I(n, 1);
    if (n >= 1) {
        CHECK_I(g_fired[0].id, id[7]);
        CHECK_I(g_fired[0].residual, 80 * M);
        CHECK_I(g_fired[0].flags, YTL_EV_FIRED);
    }
    CHECK(ytl_value(&g_tl, 0) == 8.0f);

    /* Last onset base time 400 ms; 401 rewinds nothing. The event at 390
     * is late (reach 400) and fires next. */
    CHECK_I(ytl_anchor(&g_tl, 1, R + 49 * P, 401 * M), 0);
    id[7] = add1(&g_tl, 1, 390 * M, YTL_SET, 2, 6.0f);
    CHECK_I(eval_at(&g_tl, R + 49 * P, P, 49, NULL, 0), 1);
    CHECK(ytl_value(&g_tl, 2) == 6.0f);
    /* An anchor at a later bt (405 > 401) whose RT time is after the next
     * onset: that window ends at 385, behind the reach (401). An evaluate
     * never un-fires, so 390 stays fired. A late event past the window
     * (398 <= reach) waits for it, pending with flags 0. */
    CHECK_I(ytl_anchor(&g_tl, 1, R + 50 * P + 20 * M, 405 * M), 0);
    y = add1(&g_tl, 1, 398 * M, YTL_SET, 3, 5.0f);
    CHECK_I(eval_at(&g_tl, R + 50 * P, P, 50, NULL, 0), 0);
    e = ytl_find(&g_tl, id[7]);
    CHECK(e && e->frame == 49 && (unsigned)e->flags == (YTL_EV_FIRED | YTL_EV_LATE));
    CHECK(ytl_value(&g_tl, 2) == 6.0f);
    CHECK(is_pending_reset(ytl_find(&g_tl, y)));
    n = eval_at(&g_tl, R + 50 * P + 15 * M, P, 51, g_fired, 8);   /* base time 400 */
    CHECK_I(n, 1);
    if (n >= 1) {
        CHECK_I(g_fired[0].id, y);
        CHECK_I(g_fired[0].residual, 2 * M);
        CHECK_I(g_fired[0].flags, YTL_EV_FIRED | YTL_EV_LATE);
    }
    CHECK(ytl_value(&g_tl, 3) == 5.0f);
    /* REWIND's own case: an earlier bt (395 <= 401) whose RT time is after
     * the next onset. At once: 398 pending again, 390 stays fired. The
     * next onset is at base time 380: nothing fires, nothing un-fires. */
    CHECK_I(ytl_anchor(&g_tl, 1, R + 51 * P + 20 * M, 395 * M), 0);
    CHECK(is_pending_reset(ytl_find(&g_tl, y)));
    CHECK(ytl_value(&g_tl, 3) == 0.0f);
    e = ytl_find(&g_tl, id[7]);
    CHECK(e && e->frame == 49);
    CHECK_I(eval_at(&g_tl, R + 51 * P + 5 * M, P, 52, NULL, 0), 0);   /* base time 380 */
    e = ytl_find(&g_tl, id[7]);
    CHECK(e && e->frame == 49);
    CHECK(ytl_value(&g_tl, 2) == 6.0f);
    n = eval_at(&g_tl, R + 51 * P + 23 * M, P, 53, g_fired, 8);   /* base time 398 */
    CHECK_I(n, 1);
    if (n >= 1) {
        CHECK_I(g_fired[0].id, y);
        CHECK_I(g_fired[0].residual, 0);
        CHECK_I(g_fired[0].flags, YTL_EV_FIRED);   /* re-fired, not added late */
    }
    check_sorted(&g_tl, __LINE__);
}

/* Two anchors at base time 0 run a trial twice. */
static void test_trial_twice(void) {
    const int64_t P = P60, R1 = 2 * S_NS, R2 = R1 + 41 * P60;
    static int seen1[16], seen2[16];
    const ytl_event* p;
    float v1[3];
    int id[6], i, k, n, np;

    CHECK(open_h(&g_tl, g_store, 32, 3, NULL, 0.0));
    id[0] = add1(&g_tl, 1, 0, YTL_ONSET, 0, 0.0f);
    id[1] = add1(&g_tl, 1, 35 * MS_NS + 7, YTL_SET, 1, 0.5f);
    id[2] = add1(&g_tl, 1, 100 * MS_NS, YTL_TRIGGER, 0, 0.0f);
    id[3] = add1(&g_tl, 1, 100 * MS_NS, YTL_MARK, 0, 0.0f);
    id[4] = add1(&g_tl, 1, 333 * MS_NS, YTL_MARK, 0, 0.0f);
    id[5] = add1(&g_tl, 1, 500 * MS_NS, YTL_OFFSET, 0, 0.0f);
    for (i = 0; i < 6; i++) CHECK_I(id[i], i);
    memset(seen1, 0, sizeof seen1);
    memset(seen2, 0, sizeof seen2);

    CHECK_I(ytl_anchor(&g_tl, 1, R1, 0), 0);
    for (k = 0; k <= 40; k++) {
        n = eval_at(&g_tl, R1 + k * P, P, k, g_fired, 16);
        for (i = 0; i < n && i < 16; i++) if (g_fired[i].id >= 0 && g_fired[i].id < 16) seen1[g_fired[i].id]++;
    }
    p = ytl_events(&g_tl, 1, &np);
    CHECK_I(np, 6);
    if (np == 6 && p) memcpy(g_snap, p, 6 * sizeof *p);
    for (i = 0; i < 3; i++) v1[i] = ytl_value(&g_tl, i);

    CHECK_I(ytl_anchor(&g_tl, 1, R2, 0), 0);
    for (k = 41; k <= 81; k++) {
        n = eval_at(&g_tl, R1 + k * P, P, k, g_fired, 16);
        for (i = 0; i < n && i < 16; i++) if (g_fired[i].id >= 0 && g_fired[i].id < 16) seen2[g_fired[i].id]++;
    }
    for (i = 0; i < 6; i++) {
        if (seen1[i] != 1) fail_i(__LINE__, "trial run 1: times event fired", seen1[i], 1);
        if (seen2[i] != 1) fail_i(__LINE__, "trial run 2: times event fired", seen2[i], 1);
    }
    p = ytl_events(&g_tl, 1, &np);
    CHECK_I(np, 6);
    for (i = 0; i < 6 && i < np && p; i++) {
        CHECK_I(p[i].id, g_snap[i].id);
        CHECK_I(p[i].residual, g_snap[i].residual);
        CHECK_I(p[i].flags, g_snap[i].flags);
        CHECK_I(p[i].frame, g_snap[i].frame + 41);
        CHECK_I(p[i].onset, g_snap[i].onset + 41 * P);
    }
    for (i = 0; i < 3; i++) CHECK(ytl_value(&g_tl, i) == v1[i]);
    (void)R2;
}

/* --------------------------------------------- 9. remove, prune and clear */

static void test_remove(void) {
    static ytl_key kk[2];
    float init[6] = { 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f };
    int A, B, C, D, E, F, G, H, I, J, K, L, X, next;
    float v2;
    int64_t t = 0;
    const ytl_event* e;

    kk[0] = mkkey(0, 0.0f, YTL_EASE_LINEAR);
    kk[1] = mkkey(1000, 1000.0f, YTL_EASE_LINEAR);
    CHECK(open_h(&g_tl, g_store, 32, 6, init, 0.0));
    CHECK_I(ytl_anchor(&g_tl, 1, 0, 0), 0);
    CHECK_I(ytl_anchor(&g_tl, 2, 0, 0), 0);
    A = add1(&g_tl, 1, 10, YTL_SET, 0, 3.0f);
    B = add1(&g_tl, 1, 20, YTL_SET, 0, 5.0f);
    C = add1(&g_tl, 1, 100, YTL_SET, 0, 7.0f);
    D = add1(&g_tl, 1, 15, YTL_ONSET, 1, 0.0f);
    E = add1(&g_tl, 1, 5, YTL_MARK, 0, 0.0f);
    L = add1(&g_tl, 1, 400, YTL_OFFSET, 1, 0.0f);
    F = add1(&g_tl, 2, 10, YTL_ONSET, 2, 0.0f);
    G = add1(&g_tl, 2, 50, YTL_SET, 2, 4.0f);
    CHECK_I(ytl_set_keys(&g_tl, 3, 1, kk, 2), 0);
    CHECK_I(ytl_set_keys(&g_tl, 4, 2, kk, 2), 0);
    check_sorted(&g_tl, __LINE__);
    CHECK_I(n_events(&g_tl, 1), 6);
    CHECK_I(n_events(&g_tl, 2), 2);
    CHECK_I(n_events(&g_tl, YTL_ALL_BASES), 8);

    CHECK_I(eval_at(&g_tl, 30, 0, 30, NULL, 0), 5);
    CHECK(ytl_value(&g_tl, 0) == 5.0f);
    CHECK(ytl_value(&g_tl, 1) == 1.0f);
    CHECK(ytl_value(&g_tl, 2) == 1.0f);
    CHECK_F(ytl_value(&g_tl, 3), 30.0, 1e-4);

    CHECK_I(ytl_remove(&g_tl, B), 0);
    CHECK(ytl_find(&g_tl, B) == NULL);
    CHECK(ytl_value(&g_tl, 0) == 3.0f);
    CHECK_I(ytl_remove(&g_tl, C), 0);
    CHECK(ytl_value(&g_tl, 0) == 3.0f);
    CHECK_I(ytl_remove(&g_tl, A), 0);
    CHECK(ytl_value(&g_tl, 0) == 0.5f);
    H = add1(&g_tl, 2, 1000, YTL_ONSET, 0, 0.0f);   /* channel 0 is free */
    CHECK(H >= 0);
    CHECK_I(ytl_remove(&g_tl, H), 0);
    CHECK_I(ytl_remove(&g_tl, B), YTL_ERR_NOT_FOUND);
    CHECK_I(ytl_remove(&g_tl, 9999), YTL_ERR_NOT_FOUND);
    CHECK_I(ytl_remove(&g_tl, -1), YTL_ERR_NOT_FOUND);
    e = ytl_find(&g_tl, D);
    CHECK(e && e->base == 1 && e->time_ns == 15 && e->kind == YTL_ONSET && e->target == 1 &&
          e->frame == 30 && e->onset == 30 && e->residual == 15);
    CHECK(ytl_find(&g_tl, 9999) == NULL);
    check_sorted(&g_tl, __LINE__);

    CHECK_I(eval_at(&g_tl, 60, 0, 60, NULL, 0), 1);   /* G */
    CHECK(ytl_value(&g_tl, 2) == 4.0f);
    CHECK_I(ytl_prune(&g_tl, 2), 2);
    CHECK_I(n_events(&g_tl, 2), 0);
    CHECK(ytl_value(&g_tl, 2) == 4.0f);
    CHECK_F(ytl_value(&g_tl, 4), 60.0, 1e-4);
    I = add1(&g_tl, 1, 2000, YTL_ONSET, 2, 0.0f);   /* channel 2 is free */
    CHECK(I >= 0);
    CHECK_I(ytl_remove(&g_tl, I), 0);
    CHECK(ytl_value(&g_tl, 2) == 4.0f);   /* I never fired: no recompute */
    v2 = ytl_value(&g_tl, 2);   /* later checks are relative, so one cause fails once */
    if (v2 != 4.0f) fprintf(stderr, "  channel 2 after removing an unfired event: %g\n", (double)v2);
    CHECK_I(add1(&g_tl, 1, 0, YTL_ONSET, 4, 0.0f), YTL_ERR_BOUND);   /* keys survive prune */
    CHECK_I(ytl_prune(&g_tl, 1), 2);   /* D, E */
    CHECK(ytl_find(&g_tl, D) == NULL && ytl_find(&g_tl, E) == NULL);
    CHECK(ytl_value(&g_tl, 1) == 1.0f);
    CHECK_I(n_events(&g_tl, 1), 1);   /* L */

    J = add1(&g_tl, 1, 70, YTL_SET, 5, 9.0f);
    K = add1(&g_tl, 1, 500, YTL_SET, 5, 6.0f);
    X = add1(&g_tl, 0, 85, YTL_MARK, 0, 0.0f);
    CHECK_I(eval_at(&g_tl, 80, 0, 80, NULL, 0), 1);
    CHECK(ytl_value(&g_tl, 5) == 9.0f);
    /* Rewind base 1 to 0: channels recompute from what remains. */
    CHECK_I(ytl_anchor(&g_tl, 1, 90, 0), 0);
    CHECK(is_pending_reset(ytl_find(&g_tl, J)));
    CHECK(ytl_value(&g_tl, 5) == 0.5f);
    CHECK(ytl_value(&g_tl, 1) == 0.5f);   /* L is pending, D was pruned */
    /* Channel 2 is free (pruned, then bound by I, then I removed): it is
     * not base 1's, so the rewind of base 1 leaves it. */
    CHECK(ytl_value(&g_tl, 2) == v2);
    CHECK_I(eval_at(&g_tl, 90, 0, 90, NULL, 0), 1);   /* X */
    CHECK_F(ytl_value(&g_tl, 3), 0.0, 1e-6);
    CHECK_I(ytl_prune(&g_tl, YTL_ALL_BASES), 1);
    CHECK(ytl_find(&g_tl, X) == NULL);
    check_sorted(&g_tl, __LINE__);

    /* clear(1): events, keys, channels back to initial and free. */
    CHECK_I(n_events(&g_tl, 1), 3);   /* L, J, K */
    next = add1(&g_tl, 0, 5000, YTL_MARK, 0, 0.0f);
    CHECK(ytl_value(&g_tl, 2) == v2);
    CHECK_I(ytl_clear(&g_tl, 1), 3);
    CHECK(ytl_value(&g_tl, 2) == v2);   /* free (pruned, then I removed): untouched */
    CHECK_I(n_events(&g_tl, 1), 0);
    CHECK(ytl_value(&g_tl, 1) == 0.5f);
    CHECK(ytl_value(&g_tl, 3) == 0.5f);
    CHECK(ytl_value(&g_tl, 5) == 0.5f);
    CHECK(ytl_base_time(&g_tl, 1, 100, &t));
    CHECK_I(t, 10);
    CHECK_I(ytl_set_keys(&g_tl, 3, 2, kk, 2), 0);
    CHECK_I(add1(&g_tl, 2, 1000, YTL_SET, 5, 1.0f), next + 1);   /* ids never reused */
    CHECK(add1(&g_tl, 2, 1000, YTL_ONSET, 1, 0.0f) >= 0);
    CHECK_I(n_events(&g_tl, YTL_ALL_BASES), 3);
    CHECK_I(ytl_clear(&g_tl, YTL_ALL_BASES), 3);
    CHECK_I(n_events(&g_tl, YTL_ALL_BASES), 0);
    CHECK(ytl_value(&g_tl, 3) == 0.5f);
    CHECK(ytl_value(&g_tl, 4) == 0.5f);
    CHECK(ytl_value(&g_tl, 1) == 0.5f);
    CHECK(ytl_value(&g_tl, 5) == 0.5f);
    CHECK(ytl_value(&g_tl, 2) == v2);   /* freed by a prune: clear leaves it */
    CHECK_I(ytl_set_keys(&g_tl, 4, 1, kk, 2), 0);   /* free after clear */
    CHECK_I(add1(&g_tl, 0, 1, YTL_MARK, 0, 0.0f), next + 3);
    (void)F; (void)G; (void)L; (void)K;
}

/* Points the manual settled after the first run of this test. */
static void test_settled(void) {
    static ytl_key kk[2];
    const int64_t R = S_NS;
    int64_t t = 0;
    int id, k, total, n;
    const ytl_event* e;

    /* A pause with a lead does not un-fire an event a frame already
     * showed early, and it does not fire again after resume. */
    CHECK(open_h(&g_tl, g_store, 8, 2, NULL, 0.5));
    id = add1(&g_tl, 1, 20 * MS_NS, YTL_ONSET, 1, 0.0f);
    CHECK_I(ytl_anchor(&g_tl, 1, R, 0), 0);
    CHECK_I(eval_at(&g_tl, R, P60, 0, NULL, 0), 0);
    n = eval_at(&g_tl, R + P60, P60, 1, g_fired, 4);   /* window ends at 25 ms */
    CHECK_I(n, 1);
    if (n >= 1) CHECK_I(g_fired[0].residual, P60 - 20 * MS_NS);
    CHECK_I(ytl_pause(&g_tl, 1, R + 17 * MS_NS), 0);   /* frozen at 17 ms < 20 ms */
    total = 0;
    for (k = 2; k <= 5; k++) total += eval_at(&g_tl, R + k * P60, P60, k, NULL, 0);
    CHECK_I(ytl_resume(&g_tl, 1, R + 6 * P60), 0);
    for (k = 6; k <= 12; k++) total += eval_at(&g_tl, R + k * P60, P60, k, NULL, 0);
    CHECK_I(total, 0);
    e = ytl_find(&g_tl, id);
    CHECK(e && e->frame == 1 && e->onset == R + P60 && (unsigned)e->flags == YTL_EV_FIRED);
    CHECK(ytl_value(&g_tl, 1) == 1.0f);
    CHECK(!ytl_next_due(&g_tl, &t));

    /* A late add is pending with flags 0 until it fires. */
    id = add1(&g_tl, 0, R, YTL_MARK, 0, 0.0f);
    CHECK(is_pending_reset(ytl_find(&g_tl, id)));
    CHECK_I(eval_at(&g_tl, R + 13 * P60, P60, 13, NULL, 0), 1);
    e = ytl_find(&g_tl, id);
    CHECK(e && (unsigned)e->flags == (YTL_EV_FIRED | YTL_EV_LATE));

    /* Lateness is by the reach, not by the last window. A pause behind the
     * reach: an event added between the frozen time and the reach is late,
     * waits for the window, and fires with LATE. */
    CHECK(open_h(&g_tl, g_store, 8, 2, NULL, 0.0));
    CHECK_I(ytl_anchor(&g_tl, 1, R, 0), 0);
    CHECK_I(eval_at(&g_tl, R + 100 * MS_NS, 0, 0, NULL, 0), 0);   /* reach 100 ms */
    CHECK_I(ytl_pause(&g_tl, 1, R + 50 * MS_NS), 0);           /* frozen at 50 ms */
    CHECK_I(eval_at(&g_tl, R + 110 * MS_NS, 0, 1, NULL, 0), 0);
    id = add1(&g_tl, 1, 70 * MS_NS, YTL_MARK, 0, 0.0f);
    CHECK_I(eval_at(&g_tl, R + 120 * MS_NS, 0, 2, NULL, 0), 0);  /* waits */
    CHECK(is_pending_reset(ytl_find(&g_tl, id)));
    CHECK_I(ytl_resume(&g_tl, 1, R + 130 * MS_NS), 0);
    n = eval_at(&g_tl, R + 150 * MS_NS, 0, 3, g_fired, 4);       /* base time 70 ms */
    CHECK_I(n, 1);
    if (n >= 1) {
        CHECK_I(g_fired[0].residual, 0);
        CHECK_I(g_fired[0].flags, YTL_EV_FIRED | YTL_EV_LATE);
    }
    /* A rewinding anchor whose RT time is after the next onset: the reach
     * is just before bt, so bt - 2 is late and bt is not. */
    CHECK_I(ytl_anchor(&g_tl, 2, R + 150 * MS_NS, 0), 0);
    CHECK_I(eval_at(&g_tl, R + 250 * MS_NS, 0, 4, NULL, 0), 0);   /* base 2 at 100 ms */
    CHECK_I(ytl_anchor(&g_tl, 2, R + 300 * MS_NS, 80 * MS_NS), 0);
    id = add1(&g_tl, 2, 80 * MS_NS - 2, YTL_MARK, 0, 0.0f);
    k = add1(&g_tl, 2, 80 * MS_NS, YTL_MARK, 0, 0.0f);
    CHECK_I(eval_at(&g_tl, R + 260 * MS_NS, 0, 5, NULL, 0), 0);   /* base 2 at 40 ms */
    /* Added after a window behind the reach: still at or before it. */
    total = add1(&g_tl, 2, 80 * MS_NS - 5, YTL_MARK, 0, 0.0f);
    n = eval_at(&g_tl, R + 300 * MS_NS, 0, 6, g_fired, 4);        /* base 2 at 80 ms */
    CHECK_I(n, 3);
    e = ytl_find(&g_tl, total);
    CHECK(e && e->residual == 5 && (unsigned)e->flags == (YTL_EV_FIRED | YTL_EV_LATE));
    e = ytl_find(&g_tl, id);
    CHECK(e && e->residual == 2 && (unsigned)e->flags == (YTL_EV_FIRED | YTL_EV_LATE));
    e = ytl_find(&g_tl, k);
    CHECK(e && e->residual == 0 && (unsigned)e->flags == YTL_EV_FIRED);

    /* Base 0: every base call is ARG. */
    CHECK_I(ytl_resume(&g_tl, 0, 0), YTL_ERR_ARG);
    CHECK_I(ytl_pause(&g_tl, 0, 0), YTL_ERR_ARG);
    CHECK_I(ytl_stop(&g_tl, 0), YTL_ERR_ARG);
    CHECK_I(ytl_anchor(&g_tl, 0, 0, 0), YTL_ERR_ARG);

    /* The last key's ease must be a ytl_ease, though it is not used.
     * (10 is YTL_EASE_CUBIC_IN_OUT since v0.6.0, so 11 is the first that
     * is not.) */
    kk[0] = mkkey(0, 1.0f, YTL_EASE_LINEAR);
    kk[1] = mkkey(10, 2.0f, NOT_EASE);
    CHECK_I(ytl_set_keys(&g_tl, 0, 1, kk, 2), YTL_ERR_ARG);
    kk[1] = mkkey(10, 2.0f, 255);
    CHECK_I(ytl_set_keys(&g_tl, 0, 1, kk, 2), YTL_ERR_ARG);
    kk[1] = mkkey(10, 0.0f, YTL_EASE_LOG);   /* LOG's > 0 check skips it */
    CHECK_I(ytl_set_keys(&g_tl, 0, 1, kk, 2), 0);
    /* Keyed: events are BOUND until set_keys with n = 0. */
    CHECK_I(add1(&g_tl, 1, 0, YTL_ONSET, 0, 0.0f), YTL_ERR_BOUND);
    CHECK_I(ytl_set_keys(&g_tl, 0, 1, NULL, 0), 0);
    CHECK(add1(&g_tl, 2, 0, YTL_ONSET, 0, 0.0f) >= 0);
    CHECK_I(ytl_set_keys(&g_tl, 0, 2, kk, 2), YTL_ERR_BOUND);

    /* open() with a NULL or bad desc fails and leaves the handle closed,
     * also a handle that was open. */
    CHECK(!ytl_open(&g_closed, NULL));
    CHECK(!ytl_is_open(&g_closed));
    CHECK(ytl_values(&g_closed) == NULL);
    CHECK(open_h(&g_tl, g_store, 8, 2, NULL, 0.0));
    CHECK(!ytl_open(&g_tl, NULL));
    CHECK(ytl_error(&g_tl)[0] != 0);
    CHECK(!ytl_is_open(&g_tl));
    CHECK(ytl_values(&g_tl) == NULL);
    CHECK_I(add1(&g_tl, 0, 0, YTL_MARK, 0, 0.0f), YTL_ERR_CLOSED);
    CHECK(open_h(&g_tl, g_store, 8, 2, NULL, 0.0));
    CHECK(!open_h(&g_tl, g_store, 8, 2, NULL, 2.0));
    CHECK(!ytl_is_open(&g_tl));
    CHECK(ytl_values(&g_tl) == NULL);
}

/* ------------------------------------------------------------ 10. next_due */

static void test_next_due(void) {
    int64_t t = 0;
    CHECK(open_h(&g_tl, g_store, 16, 2, NULL, 0.0));
    CHECK(!ytl_next_due(&g_tl, &t));
    CHECK(add1(&g_tl, 0, 500, YTL_MARK, 0, 0.0f) >= 0);
    CHECK(ytl_next_due(&g_tl, &t));
    CHECK_I(t, 500);
    CHECK(add1(&g_tl, 1, 0, YTL_MARK, 0, 0.0f) >= 0);   /* stopped: not due */
    CHECK(ytl_next_due(&g_tl, &t));
    CHECK_I(t, 500);
    CHECK_I(ytl_anchor(&g_tl, 1, 100, 0), 0);
    CHECK(ytl_next_due(&g_tl, &t));
    CHECK_I(t, 100);
    CHECK_I(ytl_pause(&g_tl, 1, 150), 0);   /* paused: not due */
    CHECK(ytl_next_due(&g_tl, &t));
    CHECK_I(t, 500);
    CHECK_I(ytl_resume(&g_tl, 1, 300), 0);   /* base time 50 at RT 300 */
    CHECK(ytl_next_due(&g_tl, &t));
    CHECK_I(t, 250);
    CHECK_I(eval_at(&g_tl, 260, 0, 0, NULL, 0), 1);
    CHECK(ytl_next_due(&g_tl, &t));
    CHECK_I(t, 500);
    CHECK(add1(&g_tl, 0, 10, YTL_MARK, 0, 0.0f) >= 0);   /* late: in the past */
    CHECK(ytl_next_due(&g_tl, &t));
    CHECK_I(t, 10);
    CHECK_I(eval_at(&g_tl, 270, 0, 1, NULL, 0), 1);
    CHECK(ytl_next_due(&g_tl, &t));
    CHECK_I(t, 500);
    CHECK_I(eval_at(&g_tl, 600, 0, 2, NULL, 0), 1);
    CHECK(!ytl_next_due(&g_tl, &t));
    CHECK(add1(&g_tl, 1, 1000, YTL_MARK, 0, 0.0f) >= 0);
    CHECK(ytl_next_due(&g_tl, &t));
    CHECK_I(t, 1250);
}

/* -------------------------------------------------------------- 11. replay */

static uint64_t fnv(uint64_t h, const void* p, size_t n) {
    const unsigned char* c = (const unsigned char*)p;
    size_t i;
    for (i = 0; i < n; i++) { h ^= c[i]; h *= UINT64_C(1099511628211); }
    return h;
}

static ytl_key g_rk6[4], g_rk7[3], g_rkb[3];
static float g_rs[64];
static ytl_curve g_rc[2];
static ytl_track g_rtr[3];

/* Channels 6 and 7 keyed, 8 swapped between sampled, repeated and
 * BEZIER tracks and none, 9 tweened. */
static int g_replay_rate;
static int g_v030_model;   /* TL_V030_MODEL: only the v0.3.0 model runs */   /* RATE: rate and window calls among the ops too */

static uint64_t replay_run(ytl_timeline* tl, ytl_event* store) {
    float init[10] = { 0.0f, 0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f, 0.9f };
    ytl_event fired[8], b[3];
    uint64_t h = UINT64_C(14695981039346656037);
    int64_t onset = T0;
    int step, r, ret, n, nf, j;

    g_rng = 0xC0FFEE;
    if (!open_h(tl, store, 64, 10, init, 0.5)) return 0;
    ytl_set_keys(tl, 6, 1, g_rk6, 4);
    ytl_set_keys(tl, 7, 0, g_rk7, 3);
    ytl_set_track(tl, 8, 2, &g_rtr[0]);
    for (step = 0; step < 600; step++) {
        r = rnd_int(g_replay_rate ? 118 : 110);
        if (r >= 114) {
            ytl_frame wf;
            int64_t end = 0;
            wf.onset = onset + rnd_range(0, P60);
            wf.period = P60;
            wf.index = step;
            ret = ytl_window(tl, rnd_int(5), &wf, &end);
            h = fnv(h, &end, sizeof end);
        } else if (r >= 110) {
            static const int32_t rn[6] = { 1, 1, 2, 1001, 3, 25 }, rd[6] = { 1, 2, 1, 1000, 7, 24 };
            int k = rnd_int(6);
            ret = ytl_rate(tl, 1 + rnd_int(3), onset + rnd_range(-P60, 2 * P60), rn[k], rd[k]);
        } else if (r < 35) {
            int base = rnd_int(4), kind = rnd_int(5);
            int64_t t = base == 0 ? onset + rnd_range(-3 * P60, 10 * P60) : rnd_range(-3 * P60, 80 * P60);
            ytl_event e = ev(base, t, kind, rnd_int(6), (float)rnd_int(50));
            e.user = sm64();
            ret = ytl_add(tl, &e);
        } else if (r < 42) {
            for (j = 0; j < 3; j++) b[j] = ev(rnd_int(4), rnd_range(0, 60 * P60), rnd_int(5), rnd_int(6), 1.0f);
            ret = ytl_add_n(tl, b, 1 + rnd_int(3));
        } else if (r < 55) {
            ret = ytl_remove(tl, rnd_int(step + 1));
        } else if (r < 61) {
            ret = ytl_anchor(tl, 1 + rnd_int(3), onset + rnd_range(-P60, 2 * P60), rnd_range(-P60, 40 * P60));
        } else if (r < 64) {
            ret = ytl_pause(tl, 1 + rnd_int(3), onset + rnd_range(0, P60));
        } else if (r < 67) {
            ret = ytl_resume(tl, 1 + rnd_int(3), onset + rnd_range(0, P60));
        } else if (r < 69) {
            ret = ytl_prune(tl, rnd_int(5) - 1);
        } else if (r < 70) {
            ret = ytl_clear(tl, 2 + rnd_int(2));
        } else if (r < 73) {
            ret = ytl_skip(tl, 1 + rnd_int(3), onset + rnd_range(-P60, 2 * P60), rnd_range(-P60, 60 * P60));
        } else if (r < 75) {
            int64_t from = onset + rnd_range(-5 * P60, 5 * P60);
            memset(fired, 0, sizeof fired);
            ret = ytl_peek(tl, rnd_int(5) - 1, from, from + rnd_range(0, 20 * P60), fired, 8);
            nf = ret < 8 ? ret : 8;
            if (nf > 0) h = fnv(h, fired, (size_t)nf * sizeof fired[0]);
        } else if (r < 100) {
            onset += rnd_int(8) == 0 ? 0 : P60;
            memset(fired, 0, sizeof fired);
            n = eval_at(tl, onset, P60, step, fired, 8);
            nf = n < 8 ? n : 8;
            if (nf > 0) h = fnv(h, fired, (size_t)nf * sizeof fired[0]);
            if (ytl_values(tl)) h = fnv(h, ytl_values(tl), 10 * sizeof(float));
            ret = n;
        } else if (r < 105) {
            int pick = rnd_int(4);
            ret = ytl_set_track(tl, 8, 1 + rnd_int(3), pick == 3 ? NULL : &g_rtr[pick]);
        } else {
            /* Every field at random; many are rejected, and the codes are
             * hashed too. Channel 8 or 9, so a tween also takes over from
             * the swapped tracks. */
            ytl_tween_desc d;
            int base = rnd_int(4), ch = 8 + rnd_int(2);
            memset(&d, 0, sizeof d);
            d.ease = rnd_int(7);
            d.start_set = rnd_int(3) != 0;
            d.start_ns = base == 0 ? onset + rnd_range(-3 * P60, 10 * P60) : rnd_range(-3 * P60, 60 * P60);
            d.delay_ns = rnd_int(3) == 0 ? rnd_range(-P60, 5 * P60) : 0;
            d.duration_ns = rnd_int(4) == 0 ? 0 : rnd_range(1, 20 * P60);
            d.to = (float)rnd_int(40) / 4.0f;
            d.from = (float)(1 + rnd_int(40)) / 4.0f;
            d.from_set = d.ease == YTL_EASE_LOG || rnd_int(3) == 0;
            if (d.ease == YTL_EASE_BEZIER) d.curve = g_rc[rnd_int(2)];
            d.cycles = rnd_int(5) - 1;
            d.yoyo = rnd_int(3) == 0;
            if (rnd_int(3) == 0) {
                d.keep_velocity = true;
                d.ease = YTL_EASE_LINEAR;
                d.from_set = false;
                d.yoyo = false;
                d.cycles = rnd_int(2);
                if (d.duration_ns == 0) d.duration_ns = P60;
            }
            ret = ytl_tween(tl, ch, base, &d);
        }
        h = fnv(h, &ret, sizeof ret);
    }
    return h;
}

static void test_replay(void) {
    uint64_t h1, h2;
    int i;
    const float* v1;
    const float* v2;
    g_rk6[0] = mkkey(0, 0.0f, YTL_EASE_COSINE);
    g_rk6[1] = mkkey(10 * P60, 2.0f, YTL_EASE_LOG);
    g_rk6[2] = mkkey(30 * P60, 9.0f, YTL_EASE_QUAD_OUT);
    g_rk6[3] = mkkey(50 * P60, 1.0f, YTL_EASE_LINEAR);
    g_rk7[0] = mkkey(T0, 0.0f, YTL_EASE_LINEAR);
    g_rk7[1] = mkkey(T0 + S_NS, 1.0f, YTL_EASE_STEP);
    g_rk7[2] = mkkey(T0 + 2 * S_NS, 3.0f, YTL_EASE_LINEAR);
    for (i = 0; i < 64; i++) g_rs[i] = (float)((i * 37) % 64) / 8.0f;
    g_rc[0] = mkcurve(0.42f, 0.0f, 0.58f, 1.0f);
    g_rc[1] = mkcurve(0.68f, -0.6f, 0.32f, 1.6f);
    g_rkb[0] = mkbez(0, 1.0f, 1);
    g_rkb[1] = mkkey(40 * MS_NS, 3.0f, YTL_EASE_COSINE);
    g_rkb[2] = mkkey(90 * MS_NS, 0.5f, YTL_EASE_LINEAR);
    g_rtr[0] = trk_samples(g_rs, 64, 0, 120, YTL_INTERP_CUBIC);
    g_rtr[0].period_ns = 300 * MS_NS;
    g_rtr[0].repeats = 5;
    g_rtr[1] = trk_keys(g_rkb, 3);
    g_rtr[1].curves = g_rc;
    g_rtr[1].n_curves = 2;
    g_rtr[1].period_ns = 100 * MS_NS;
    g_rtr[2] = trk_samples(g_rs, 16, -S_NS, 7, YTL_INTERP_STEP);
    h1 = replay_run(&g_tl, g_store);
    h2 = replay_run(&g_tl2, g_store2);
    CHECK(h1 != 0);
    CHECK(h1 == h2);
    CHECK(memcmp(g_store, g_store2, 64 * sizeof g_store[0]) == 0);
    CHECK_I(n_events(&g_tl, YTL_ALL_BASES), n_events(&g_tl2, YTL_ALL_BASES));
    v1 = ytl_values(&g_tl);
    v2 = ytl_values(&g_tl2);
    CHECK(v1 && v2 && memcmp(v1, v2, 10 * sizeof(float)) == 0);
    /* RATE: the same with rate changes and window queries among the calls,
     * and the whole handles compared. */
    g_replay_rate = 1;
    h1 = replay_run(&g_tl, g_store);
    h2 = replay_run(&g_tl2, g_store2);
    g_replay_rate = 0;
    CHECK(h1 != 0);
    CHECK(h1 == h2);
    CHECK(memcmp(g_store, g_store2, 64 * sizeof g_store[0]) == 0);
    CHECK(memcmp(&g_tl.bases, &g_tl2.bases, sizeof g_tl.bases) == 0);
    CHECK(memcmp(ytl_values(&g_tl), ytl_values(&g_tl2), 10 * sizeof(float)) == 0);
}

/* ------------------------------------------- 11b. lead, skip and peek */

/* QUANTIZATION: ytl_lead() is the lead in use, and a window ends
 * lead x period, truncated, after the onset. */
static void test_lead(void) {
    static ytl_event s[4];
    static ytl_timeline t;
    const double in[5] = { 0.0, 0.25, 0.5, 0.999, YTL_LEAD_NONE };
    const double want[5] = { 0.5, 0.25, 0.5, 0.999, 0.0 };
    const int64_t P = P60;
    int k;
    CHECK(ytl_lead(NULL) == 0.0);
    memset(&t, 0, sizeof t);
    CHECK(ytl_lead(&t) == 0.0);
    for (k = 0; k < 5; k++) {
        ytl_desc d;
        int64_t w;
        int a, b;
        memset(&d, 0, sizeof d);
        memset(s, 0, sizeof s);
        d.events = s;
        d.event_capacity = 4;
        d.n_channels = 1;
        d.lead = in[k];
        CHECK(ytl_open(&t, &d));
        CHECK(ytl_lead(&t) == want[k]);
        w = (int64_t)(ytl_lead(&t) * (double)P);
        a = add1(&t, 0, 10 * P + w, YTL_MARK, 0, 0.0f);
        b = add1(&t, 0, 10 * P + w + 1, YTL_MARK, 0, 0.0f);
        CHECK_I(eval_at(&t, 10 * P, P, 10, NULL, 0), 1);
        CHECK(ytl_find(&t, a) && ytl_find(&t, a)->frame == 10);
        CHECK(is_pending_reset(ytl_find(&t, b)));
    }
    CHECK(!open_h(&t, s, 4, 1, NULL, 2.0));
    CHECK(ytl_lead(&t) == 0.0);
}

#define SK_N 4096
static ytl_event g_sk_a[SK_N], g_sk_b[SK_N];
static ytl_timeline g_ska, g_skb;

/* A movie base (1): a MARK every second for an hour (code = the second)
 * and a SET of channel 0 to the second every 10 s. 3960 events. */
static void skip_movie(ytl_timeline* tl, ytl_event* st) {
    int i;
    CHECK(open_h(tl, st, SK_N, 3, NULL, 0.5));
    for (i = 0; i < 3600; i++) {
        ytl_event e = ev(1, (int64_t)i * S_NS, YTL_MARK, 0, 0.0f);
        e.code = i;
        CHECK(ytl_add(tl, &e) >= 0);
        if (i % 10 == 0) CHECK(add1(tl, 1, (int64_t)i * S_NS, YTL_SET, 0, (float)i) >= 0);
    }
}

static const ytl_event* find_at(const ytl_timeline* tl, int base, int64_t t, int kind) {
    int n = 0, i;
    const ytl_event* p = ytl_events(tl, base, &n);
    for (i = 0; i < n; i++)
        if (p[i].time_ns == t && p[i].kind == kind) return &p[i];
    return NULL;
}

static int count_flags(const ytl_timeline* tl, int base, unsigned mask) {
    int n = 0, i, c = 0;
    const ytl_event* p = ytl_events(tl, base, &n);
    for (i = 0; i < n; i++)
        if (p[i].flags & mask) c++;
    return c;
}

static void test_skip_seek(void) {
    const int64_t P = P60, R = 5 * S_NS, B = 1800 * S_NS, R2 = R + 60 * P60, R3 = R2 + 2 * S_NS;
    const ytl_event* e;
    int n, i, k, x, prunable;
    int64_t due = 0;

    skip_movie(&g_ska, g_sk_a);
    skip_movie(&g_skb, g_sk_b);
    CHECK_I(ytl_anchor(&g_ska, 1, R, 0), 0);
    CHECK_I(ytl_anchor(&g_skb, 1, R, 0), 0);
    for (k = 0; k < 60; k++) {   /* one second of play: the events at 0 fire */
        (void)eval_at(&g_ska, R + k * P, P, k, NULL, 0);
        (void)eval_at(&g_skb, R + k * P, P, k, NULL, 0);
    }
    CHECK_I(count_flags(&g_ska, 1, YTL_EV_FIRED), 2);

    /* The 30-minute seek. Skipped: MARKs 1..1799 and SETs 10..1790. The
     * channel already shows the last SET passed over. */
    CHECK_I(ytl_skip(&g_ska, 1, R2, B), 1799 + 179);
    CHECK_I(ytl_anchor(&g_skb, 1, R2, B), 0);
    CHECK(ytl_value(&g_ska, 0) == 1790.0f);
    e = find_at(&g_ska, 1, 1000 * S_NS, YTL_MARK);
    CHECK(e && e->flags == YTL_EV_SKIPPED && e->frame == -1 && e->onset == R2
          && e->residual == B - 1000 * S_NS && e->code == 1000);
    e = find_at(&g_ska, 1, 0, YTL_MARK);
    CHECK(e && e->flags == YTL_EV_FIRED && e->frame == 0);
    e = find_at(&g_ska, 1, B, YTL_MARK);
    CHECK(is_pending_reset(e));
    CHECK(ytl_next_due(&g_ska, &due));
    CHECK_I(due, R2);

    /* The next frame: the skip fires only what is at B; the anchor fires
     * all 1980 on the same frame. Both channels end the same. */
    n = eval_at(&g_ska, R2, P, 60, g_fired, 16);
    CHECK_I(n, 2);
    for (i = 0; i < n && i < 2; i++) {
        CHECK_I(g_fired[i].time_ns, B);
        CHECK_I(g_fired[i].residual, 0);
        CHECK_I(g_fired[i].flags, YTL_EV_FIRED);
    }
    CHECK_I(eval_at(&g_skb, R2, P, 60, NULL, 0), 1980);
    CHECK(ytl_value(&g_ska, 0) == 1800.0f);
    CHECK(ytl_value(&g_skb, 0) == 1800.0f);
    CHECK(ytl_next_due(&g_ska, &due));
    CHECK_I(due, R2 + S_NS);

    /* The reach moved to the seek: an add before B is late and fires on
     * the next frame; one after waits for its time. */
    x = add1(&g_ska, 1, B - 5 * S_NS, YTL_MARK, 0, 0.0f);
    k = add1(&g_ska, 1, B + 2 * P, YTL_MARK, 0, 0.0f);
    n = eval_at(&g_ska, R2 + P, P, 61, g_fired, 16);
    CHECK_I(n, 1);
    if (n >= 1) {
        CHECK_I(g_fired[0].id, x);
        CHECK_I(g_fired[0].flags, YTL_EV_FIRED | YTL_EV_LATE);
        CHECK_I(g_fired[0].residual, P + 5 * S_NS);
    }
    CHECK(is_pending_reset(ytl_find(&g_ska, k)));

    /* An anchor back to 1000 s: skipped events from there are pending
     * again, the ones before stay skipped, and the channel is recomputed
     * from what is fired or skipped. */
    CHECK_I(ytl_anchor(&g_ska, 1, R3, 1000 * S_NS), 0);
    CHECK(is_pending_reset(find_at(&g_ska, 1, 1000 * S_NS, YTL_MARK)));
    CHECK(is_pending_reset(find_at(&g_ska, 1, 1500 * S_NS, YTL_SET)));
    CHECK(is_pending_reset(find_at(&g_ska, 1, B, YTL_MARK)));
    e = find_at(&g_ska, 1, 999 * S_NS, YTL_MARK);
    CHECK(e && e->flags == YTL_EV_SKIPPED && e->onset == R2);
    CHECK(ytl_value(&g_ska, 0) == 990.0f);
    n = eval_at(&g_ska, R3, P, 200, g_fired, 16);
    CHECK_I(n, 2);
    for (i = 0; i < n && i < 2; i++) {
        CHECK_I(g_fired[i].time_ns, 1000 * S_NS);
        CHECK_I(g_fired[i].flags, YTL_EV_FIRED);
    }
    CHECK(ytl_value(&g_ska, 0) == 1000.0f);

    /* Prune deletes the fired and the skipped; channel values stay. */
    prunable = count_flags(&g_ska, 1, YTL_EV_FIRED | YTL_EV_SKIPPED);
    CHECK(prunable > 1000);
    n = n_events(&g_ska, 1);
    CHECK_I(ytl_prune(&g_ska, 1), prunable);
    CHECK_I(n_events(&g_ska, 1), n - prunable);
    CHECK_I(count_flags(&g_ska, 1, YTL_EV_FIRED | YTL_EV_SKIPPED), 0);
    CHECK(ytl_value(&g_ska, 0) == 1000.0f);
    check_sorted(&g_ska, __LINE__);
}

/* A skip back is an anchor back, when no late event waits before bt. */
static void test_skip_back(void) {
    const int64_t P = P60, R = S_NS;
    int k;
    skip_movie(&g_ska, g_sk_a);
    skip_movie(&g_skb, g_sk_b);
    CHECK_I(ytl_anchor(&g_ska, 1, R, 0), 0);
    CHECK_I(ytl_anchor(&g_skb, 1, R, 0), 0);
    for (k = 0; k < 600; k++) {
        (void)eval_at(&g_ska, R + k * P, P, k, NULL, 0);
        (void)eval_at(&g_skb, R + k * P, P, k, NULL, 0);
    }
    CHECK(ytl_value(&g_ska, 0) == 0.0f);   /* 600 frames is 9.99 s: SET 10 not yet */
    CHECK_I(ytl_skip(&g_ska, 1, R + 600 * P, 3 * S_NS), 0);
    CHECK_I(ytl_anchor(&g_skb, 1, R + 600 * P, 3 * S_NS), 0);
    CHECK(memcmp(g_sk_a, g_sk_b, sizeof g_sk_a) == 0);
    for (k = 600; k < 700; k++) {
        CHECK_I(eval_at(&g_ska, R + k * P, P, k, NULL, 0), eval_at(&g_skb, R + k * P, P, k, NULL, 0));
    }
    CHECK(memcmp(g_sk_a, g_sk_b, sizeof g_sk_a) == 0);
    CHECK(ytl_value(&g_ska, 0) == ytl_value(&g_skb, 0));
}

/* A skip back also skips a late event before bt still waiting for a
 * window; the anchor leaves it to fire late. */
static void test_skip_late_wait(void) {
    const int64_t P = 10 * MS_NS, M = MS_NS;
    ytl_timeline* t[2];
    ytl_event* st[2];
    int j, id90[2], id97[2], k, n;
    t[0] = &g_tl;
    t[1] = &g_tl2;
    st[0] = g_store;
    st[1] = g_store2;
    for (j = 0; j < 2; j++) {
        CHECK(open_h(t[j], st[j], 16, 2, NULL, 0.5));
        id97[j] = add1(t[j], 1, 97 * M, YTL_MARK, 0, 0.0f);
        CHECK_I(ytl_anchor(t[j], 1, 0, 0), 0);
        for (k = 0; k <= 10; k++) (void)eval_at(t[j], k * P, P, k, NULL, 0);   /* reach 105 ms */
        CHECK(ytl_find(t[j], id97[j]) && ytl_find(t[j], id97[j])->frame == 10);
        /* Forward to 101 ms, but at an RT time after the next onset: that
         * window ends at 76 ms, behind the reach. */
        CHECK_I(ytl_anchor(t[j], 1, 140 * M, 101 * M), 0);
        id90[j] = add1(t[j], 1, 90 * M, YTL_MARK, 0, 0.0f);    /* late: <= reach */
        CHECK_I(eval_at(t[j], 110 * M, P, 11, NULL, 0), 0);      /* it waits */
        CHECK(is_pending_reset(ytl_find(t[j], id90[j])));
    }
    /* Back to 95 ms (<= the furthest onset, 100 ms): 97 is pending again
     * in both; the skip also skips 90. */
    CHECK_I(ytl_skip(&g_tl, 1, 120 * M, 95 * M), 1);
    CHECK_I(ytl_anchor(&g_tl2, 1, 120 * M, 95 * M), 0);
    CHECK(is_pending_reset(ytl_find(&g_tl, id97[0])));
    CHECK(is_pending_reset(ytl_find(&g_tl2, id97[1])));
    CHECK(ytl_find(&g_tl, id90[0]) && ytl_find(&g_tl, id90[0])->flags == YTL_EV_SKIPPED
          && ytl_find(&g_tl, id90[0])->residual == 5 * M);
    n = eval_at(&g_tl, 120 * M, P, 12, g_fired, 4);
    CHECK_I(n, 1);
    if (n >= 1) { CHECK_I(g_fired[0].id, id97[0]); CHECK_I(g_fired[0].flags, YTL_EV_FIRED); }
    n = eval_at(&g_tl2, 120 * M, P, 12, g_fired, 4);
    CHECK_I(n, 2);
    if (n >= 2) {
        CHECK_I(g_fired[0].id, id90[1]);
        CHECK_I(g_fired[0].flags, YTL_EV_FIRED | YTL_EV_LATE);
        CHECK_I(g_fired[1].id, id97[1]);
    }
}

/* Refusals change nothing; a stopped or paused base runs after a skip; an
 * event exactly at bt is not skipped; remove of a skipped event that the
 * channel shows recomputes the channel. */
static void test_skip_misc(void) {
    static ytl_timeline snap;
    const int64_t M = MS_NS;
    int i, a, s5, x, n;
    int64_t bt = 0;

    CHECK(open_h(&g_tl, g_store, 32, 4, NULL, 0.5));
    for (i = 0; i < 10; i++) CHECK(add1(&g_tl, 2, i * M, YTL_MARK, 0, 0.0f) >= 0);
    s5 = add1(&g_tl, 2, 5 * M, YTL_SET, 1, 7.0f);
    a = add1(&g_tl, 2, 4 * M, YTL_SET, 1, 3.0f);
    memcpy(&snap, &g_tl, sizeof snap);
    memcpy(g_snap, g_store, 32 * sizeof g_store[0]);
    CHECK_I(ytl_skip(&g_tl, 0, 0, 0), YTL_ERR_ARG);
    CHECK_I(ytl_skip(&g_tl, YTL_MAX_BASES, 0, 0), YTL_ERR_ARG);
    CHECK_I(ytl_skip(&g_tl, -1, 0, 0), YTL_ERR_ARG);
    CHECK_I(ytl_skip(&g_tl, 2, 0, LIM62), YTL_ERR_ARG);
    CHECK_I(ytl_skip(&g_tl, 2, 0, -LIM62), YTL_ERR_ARG);
    CHECK(memcmp(&snap, &g_tl, TL_STATE) == 0);
    CHECK_S(ytl_error(&g_tl), "ysp_timeline: ytl_skip: invalid argument");
    CHECK(memcmp(g_snap, g_store, 32 * sizeof g_store[0]) == 0);
    CHECK_I(ytl_skip(&g_closed, 2, 0, 0), YTL_ERR_CLOSED);
    CHECK_I(ytl_skip(NULL, 2, 0, 0), YTL_ERR_CLOSED);

    /* Base 2 was never anchored: MARKs 0..4 and the SET at 4 ms are
     * skipped; the MARK and SET exactly at 5 ms are not. */
    CHECK_I(ytl_skip(&g_tl, 2, 1000 * M, 5 * M), 6);
    CHECK(ytl_base_time(&g_tl, 2, 1001 * M, &bt));
    CHECK_I(bt, 6 * M);
    CHECK(ytl_value(&g_tl, 1) == 3.0f);
    CHECK(is_pending_reset(ytl_find(&g_tl, s5)));
    x = add1(&g_tl, 2, 3 * M, YTL_MARK, 0, 0.0f);   /* late: the base passed it */
    n = eval_at(&g_tl, 1000 * M, 0, 0, g_fired, 8);
    CHECK_I(n, 3);
    CHECK(ytl_find(&g_tl, x) && ytl_find(&g_tl, x)->flags == (YTL_EV_FIRED | YTL_EV_LATE));
    CHECK(ytl_find(&g_tl, s5) && ytl_find(&g_tl, s5)->flags == YTL_EV_FIRED);
    CHECK(ytl_value(&g_tl, 1) == 7.0f);

    /* Remove the shown SET; the skipped one at 4 ms shows again. */
    CHECK_I(ytl_remove(&g_tl, s5), 0);
    CHECK(ytl_value(&g_tl, 1) == 3.0f);
    CHECK_I(ytl_remove(&g_tl, a), 0);
    CHECK(ytl_value(&g_tl, 1) == 0.0f);   /* channel free again: initial */

    /* A skip runs a paused base; a paused seek is a skip and a pause. */
    CHECK_I(ytl_anchor(&g_tl, 3, 0, 0), 0);
    CHECK_I(ytl_pause(&g_tl, 3, 10), 0);
    CHECK_I(ytl_skip(&g_tl, 3, 20, 50), 0);
    CHECK(ytl_base_time(&g_tl, 3, 30, &bt));
    CHECK_I(bt, 60);
    CHECK_I(ytl_pause(&g_tl, 3, 20), 0);
    CHECK(ytl_base_time(&g_tl, 3, 999, &bt));
    CHECK_I(bt, 50);
    /* A skip to where the base already is skips nothing more. */
    CHECK_I(ytl_skip(&g_tl, 2, 2000 * M, 5 * M), 0);
    check_sorted(&g_tl, __LINE__);
}

/* Tracks are a function of base time: after a skip and after an anchor to
 * the same place the next frame's values are the same bits, a waiting
 * tween included, which takes over from the old track at its start. */
static void test_skip_tracks(void) {
    static ytl_key ramp[2];
    ytl_timeline* t[2];
    ytl_event* st[2];
    ytl_tween_desc d;
    const int64_t P = P60, S = S_NS;
    int j;
    double from, want;
    ramp[0] = mkkey(0, 0.0f, YTL_EASE_LINEAR);
    ramp[1] = mkkey(100 * S, 10.0f, YTL_EASE_LINEAR);
    t[0] = &g_tl;
    t[1] = &g_tl2;
    st[0] = g_store;
    st[1] = g_store2;
    for (j = 0; j < 2; j++) {
        CHECK(open_h(t[j], st[j], 16, 3, NULL, 0.5));
        CHECK_I(ytl_set_keys(t[j], 1, 1, ramp, 2), 0);
        CHECK_I(ytl_set_keys(t[j], 2, 1, ramp, 2), 0);
        CHECK_I(ytl_anchor(t[j], 1, S, 0), 0);
        CHECK_I(eval_at(t[j], S, P, 0, NULL, 0), 0);
        d = twd_at(3.0f, 20 * S, 40 * S);
        CHECK_I(ytl_tween(t[j], 2, 1, &d), 0);
    }
    CHECK_I(ytl_skip(&g_tl, 1, 2 * S, 50 * S), 0);
    CHECK_I(ytl_anchor(&g_tl2, 1, 2 * S, 50 * S), 0);
    CHECK_I(eval_at(&g_tl, 2 * S, P, 1, NULL, 0), 0);
    CHECK_I(eval_at(&g_tl2, 2 * S, P, 1, NULL, 0), 0);
    CHECK(memcmp(ytl_values(&g_tl), ytl_values(&g_tl2), 3 * sizeof(float)) == 0);
    CHECK_F(ytl_value(&g_tl, 1), 5.0, 1e-6);
    from = 4.0;                                   /* the ramp at the tween's start */
    want = from + (3.0 - from) * 0.5;             /* halfway through, linear */
    CHECK_F(ytl_value(&g_tl, 2), want, 1e-5);
}

/* PEEK on fixed bases and times (the RT times of report_setup()). */
static int g_pk[9];
static ytl_timeline g_pk_snap;

static int peek_pure(int line, int base, int64_t from, int64_t to, ytl_event* out, int cap) {
    int r, i;
    memcpy(&g_pk_snap, &g_tl, sizeof g_tl);
    memcpy(g_snap, g_store, 32 * sizeof g_store[0]);
    for (i = 0; i < 16; i++) g_fired[i].id = -999;
    r = ytl_peek(&g_tl, base, from, to, out, cap);
    if (memcmp(&g_pk_snap, &g_tl, TL_STATE) != 0) fail(line, "peek changed the handle");
    if (r < 0 && !strstr(ytl_error(&g_tl), "ytl_peek")) fail(line, "a failed peek wrote no message");
    if (memcmp(g_snap, g_store, 32 * sizeof g_store[0]) != 0) fail(line, "peek changed the storage");
    if (out == g_fired && cap >= 0 && cap < 16 && g_fired[cap].id != -999) fail(line, "peek wrote past cap");
    return r;
}

/* out[0..n) has the ids want[0..n) (indexes into g_pk) at RT times rt[]. */
static void peek_expect(int line, int n, const int* want, const int64_t* rt) {
    int i;
    for (i = 0; i < n; i++) {
        const ytl_event* s = ytl_find(&g_tl, g_fired[i].id);
        if (g_fired[i].id != g_pk[want[i]]) { fail_i(line, "peek id", g_fired[i].id, g_pk[want[i]]); return; }
        if (g_fired[i].onset != rt[i]) fail_i(line, "peek onset (RT time)", g_fired[i].onset, rt[i]);
        if (g_fired[i].residual != 0 || g_fired[i].frame != -1 || g_fired[i].flags != 0)
            fail(line, "peek copy: residual 0, frame -1, flags 0");
        if (!s || s->time_ns != g_fired[i].time_ns || s->base != g_fired[i].base || s->kind != g_fired[i].kind
            || s->code != g_fired[i].code || s->user != g_fired[i].user)
            fail(line, "peek copy differs from the storage");
    }
}

static void test_peek(void) {
    static const int all[7] = { 6, 1, 4, 5, 0, 3, 2 };
    static const int64_t all_rt[7] = { 1050, 1100, 1100, 1100, 1300, 1300, 1500 };
    int i, n;
    int ids[3];
    int64_t rts[3];

    report_setup();   /* base 1 at (1000, 0), base 2 at (2000, 500) */
    for (i = 0; i < 8; i++) g_pk[i] = g_rep[i];
    g_pk[8] = add1(&g_tl, 3, 10, YTL_MARK, 0, 0.0f);   /* base 3 stopped */
    n = peek_pure(__LINE__, YTL_ALL_BASES, 0, 2000, g_fired, 16);
    CHECK_I(n, 7);
    if (n == 7) peek_expect(__LINE__, 7, all, all_rt);
    /* The window is [from, to). */
    n = peek_pure(__LINE__, YTL_ALL_BASES, 1100, 1300, g_fired, 16);
    CHECK_I(n, 3);
    if (n == 3) peek_expect(__LINE__, 3, all + 1, all_rt + 1);
    n = peek_pure(__LINE__, YTL_ALL_BASES, 1101, 1301, g_fired, 16);
    CHECK_I(n, 2);
    if (n == 2) peek_expect(__LINE__, 2, all + 4, all_rt + 4);
    CHECK_I(peek_pure(__LINE__, YTL_ALL_BASES, 1100, 1100, g_fired, 16), 0);
    CHECK_I(peek_pure(__LINE__, YTL_ALL_BASES, 1099, 1100, g_fired, 16), 0);
    CHECK_I(peek_pure(__LINE__, YTL_ALL_BASES, 1100, 1101, g_fired, 16), 3);
    /* One base at a time; base 3 is stopped; base 1's pending event at
     * 6000 is in a later window. */
    n = peek_pure(__LINE__, 1, 0, 2000, g_fired, 16);
    CHECK_I(n, 2);
    ids[0] = 1; ids[1] = 3; rts[0] = 1100; rts[1] = 1300;
    if (n == 2) peek_expect(__LINE__, 2, ids, rts);
    n = peek_pure(__LINE__, 2, 0, 2000, g_fired, 16);
    CHECK_I(n, 2);
    ids[0] = 5; ids[1] = 2; rts[0] = 1100; rts[1] = 1500;
    if (n == 2) peek_expect(__LINE__, 2, ids, rts);
    n = peek_pure(__LINE__, 0, 0, 2000, g_fired, 16);
    CHECK_I(n, 3);
    ids[0] = 6; ids[1] = 4; ids[2] = 0; rts[0] = 1050; rts[1] = 1100; rts[2] = 1300;
    if (n == 3) peek_expect(__LINE__, 3, ids, rts);
    CHECK_I(peek_pure(__LINE__, 3, -LIM62 + 1, LIM62 - 1, g_fired, 16), 0);
    CHECK_I(peek_pure(__LINE__, 1, 0, 6000, g_fired, 16), 2);
    CHECK_I(peek_pure(__LINE__, 1, 0, 6001, g_fired, 16), 3);
    /* The cap: the earliest, and the total. NULL with cap 0 counts. */
    n = peek_pure(__LINE__, YTL_ALL_BASES, 0, 2000, g_fired, 3);
    CHECK_I(n, 7);
    peek_expect(__LINE__, 3, all, all_rt);
    CHECK_I(peek_pure(__LINE__, YTL_ALL_BASES, 0, 2000, NULL, 0), 7);
    CHECK_I(peek_pure(__LINE__, YTL_ALL_BASES, 0, 2000, g_fired, 0), 7);
    /* Refusals. */
    CHECK_I(peek_pure(__LINE__, YTL_ALL_BASES, 0, 2000, g_fired, -1), YTL_ERR_ARG);
    CHECK_I(peek_pure(__LINE__, YTL_ALL_BASES, 0, 2000, NULL, 2), YTL_ERR_ARG);
    CHECK_I(peek_pure(__LINE__, YTL_ALL_BASES, 2000, 1999, g_fired, 4), YTL_ERR_ARG);
    CHECK_I(peek_pure(__LINE__, YTL_MAX_BASES, 0, 2000, g_fired, 4), YTL_ERR_ARG);
    CHECK_I(peek_pure(__LINE__, -2, 0, 2000, g_fired, 4), YTL_ERR_ARG);
    CHECK_I(peek_pure(__LINE__, 1, -LIM62, 0, g_fired, 4), YTL_ERR_ARG);
    CHECK_I(peek_pure(__LINE__, 1, 0, LIM62, g_fired, 4), YTL_ERR_ARG);
    CHECK_I(ytl_peek(&g_closed, 1, 0, 1, NULL, 0), YTL_ERR_CLOSED);
    CHECK_I(ytl_peek(NULL, 1, 0, 1, NULL, 0), YTL_ERR_CLOSED);

    /* Fired events are not pending: after a frame at 1100, four are gone. */
    CHECK_I(eval_at(&g_tl, 1100, 0, 0, NULL, 0), 4);
    n = peek_pure(__LINE__, YTL_ALL_BASES, 0, 2000, g_fired, 16);
    CHECK_I(n, 3);
    if (n == 3) peek_expect(__LINE__, 3, all + 4, all_rt + 4);
    /* A late event is pending, at its RT time in the past. */
    g_pk[0] = add1(&g_tl, 0, 1000, YTL_MARK, 0, 0.0f);
    n = peek_pure(__LINE__, YTL_ALL_BASES, 0, 2000, g_fired, 16);
    CHECK_I(n, 4);
    ids[0] = 0; rts[0] = 1000;
    if (n >= 1) peek_expect(__LINE__, 1, ids, rts);
    CHECK_I(peek_pure(__LINE__, YTL_ALL_BASES, 1001, 2000, g_fired, 16), 3);
    /* A paused base has no RT times; resumed, its times move. Base 2 at
     * RT 1200 is at base time -300; resumed at 3000. */
    CHECK_I(ytl_pause(&g_tl, 2, 1200), 0);
    CHECK_I(peek_pure(__LINE__, 2, -LIM62 + 1, LIM62 - 1, g_fired, 16), 0);
    CHECK_I(ytl_resume(&g_tl, 2, 3000), 0);
    n = peek_pure(__LINE__, 2, 0, 4000, g_fired, 16);
    CHECK_I(n, 1);   /* -400 fired at 1100; 0 is at RT 3300 */
    ids[0] = 2; rts[0] = 3300;
    if (n == 1) peek_expect(__LINE__, 1, ids, rts);
    /* A skipped event is not pending: base 1 to 400 skips 300. */
    CHECK_I(ytl_skip(&g_tl, 1, 1150, 400), 1);
    n = peek_pure(__LINE__, 1, -LIM62 + 1, LIM62 - 1, g_fired, 16);
    CHECK_I(n, 1);
    ids[0] = 7; rts[0] = 1150 + 4600;
    if (n == 1) peek_expect(__LINE__, 1, ids, rts);
}

/* The audio path's use: each 60 Hz frame peeks [last to, onset + L) with
 * L the audio lead plus a frame, then evaluates. Every event is seen in
 * exactly one peek, at least L - P before its RT time and before the frame
 * that fires it, and fires exactly once. */
static void test_peek_lookahead(void) {
    enum { NE = 200 };
    static int seen[NE + 8], fired_n[NE + 8];
    static int64_t seen_at[NE + 8], rt_of[NE + 8];
    static ytl_event out[64];
    const int64_t P = P60, R = S_NS, L = 55 * MS_NS + P60;
    int64_t to = R, worst = INT64_MAX;
    int i, k, n, bad_lead = 0, bad_order = 0, bad_seen = 0, bad_fire = 0;
    g_rng = 4242;
    CHECK(open_h(&g_tl, g_store, NE + 8, 1, NULL, 0.5));
    CHECK_I(ytl_anchor(&g_tl, 1, R, 0), 0);
    memset(seen, 0, sizeof seen);
    memset(fired_n, 0, sizeof fired_n);
    for (i = 0; i < NE; i++) {
        int64_t t = rnd_range(L, 20 * S_NS);
        int id = add1(&g_tl, 1, t, YTL_MARK, 0, 0.0f);
        CHECK_I(id, i);
        rt_of[i] = R + t;
    }
    for (k = 0; k < 22 * 60; k++) {
        int64_t T = R + k * P;
        n = ytl_peek(&g_tl, 1, to, T + L, out, 64);
        CHECK(n >= 0 && n <= 64);
        for (i = 0; i < n && i < 64; i++) {
            int id = out[i].id;
            if (id < 0 || id >= NE || seen[id]) { bad_seen++; continue; }
            seen[id] = 1;
            seen_at[id] = T;
            if (out[i].onset != rt_of[id]) bad_seen++;
            if (rt_of[id] - T < L - P) bad_lead++;
            if (rt_of[id] - T < worst) worst = rt_of[id] - T;
        }
        to = T + L;
        n = eval_at(&g_tl, T, P, k, out, 64);
        for (i = 0; i < n && i < 64; i++) {
            int id = out[i].id;
            if (id < 0 || id >= NE) { bad_fire++; continue; }
            fired_n[id]++;
            if (!seen[id] || seen_at[id] >= T) bad_order++;
        }
    }
    for (i = 0; i < NE; i++) {
        if (!seen[i]) bad_seen++;
        if (fired_n[i] != 1) bad_fire++;
    }
    CHECK_I(bad_seen, 0);
    CHECK_I(bad_lead, 0);
    CHECK_I(bad_order, 0);
    CHECK_I(bad_fire, 0);
    CHECK(worst >= L - P);
}

/* ------------------------------------------------- 11c. RATE and window */

/* Base `b` running from (art, abt) at num/den, through the calls: an
 * anchor, then the rate at the anchor's RT time, which keeps (art, abt). */
static void rate_setup(ytl_timeline* tl, int b, int64_t art, int64_t abt, int32_t num, int32_t den) {
    CHECK_I(ytl_anchor(tl, b, art, abt), 0);
    CHECK_I(ytl_rate(tl, b, art, num, den), 0);
}

/* The exact base time, with *oor set when it is at or past +-2^62. */
static int64_t ref_bt_range(int64_t art, int64_t abt, int64_t num, int64_t den, int64_t t, int* oor) {
    int big;
    int64_t q = ref_floor_md(t - art, num, den, &big);
    *oor = 1;
    if (big) return 0;
    if (q > 0 && q >= LIM62 - abt) return 0;
    if (q < 0 && q <= -LIM62 - abt) return 0;
    *oor = abt + q >= LIM62 || abt + q <= -LIM62;
    return abt + q;
}

static void test_rate_arith(void) {
    static const int64_t nums[9] = { 1, 2, 3, 7, 1000, 1001, 24000, 2147483646, 2147483647 };
    int i, bad = 0, bad_inv = 0, bad_oor = 0, n_oor = 0, n_inv = 0;
    g_rng = 4242;
    CHECK(open_h(&g_tl, g_store, 4, 1, NULL, 0.5));
    for (i = 0; i < 200000; i++) {
        int64_t num = rnd_int(3) ? nums[rnd_int(9)] : rnd_range(1, 2147483647);
        int64_t den = rnd_int(3) ? nums[rnd_int(9)] : rnd_range(1, 2147483647);
        int64_t g = gcd64(num, den), art, abt, t, x = 0, want, T;
        int oor, cls = rnd_int(4);
        bool ok;
        art = rnd_int(2) ? rnd_range(-LIM62 + 1, LIM62 - 1) : rnd_range(-1000000, 1000000);
        abt = rnd_int(2) ? rnd_range(-LIM62 + 1, LIM62 - 1) : rnd_range(-1000000, 1000000);
        if (cls == 0) t = art + rnd_range(-3 * den, 3 * den);
        else if (cls == 1) t = art + rnd_range(-(INT64_C(1) << 40), INT64_C(1) << 40);
        else t = rnd_range(-LIM62 + 1, LIM62 - 1);
        if (t >= LIM62 || t <= -LIM62) t = art;
        rate_setup(&g_tl, 1, art, abt, (int32_t)num, (int32_t)den);
        num /= g;
        den /= g;
        want = ref_bt_range(art, abt, num, den, t, &oor);
        ok = ytl_base_time(&g_tl, 1, t, &x);
        if (oor) {
            n_oor++;
            if (ok) bad_oor++;
        } else if (!ok || x != want) {
            if (!bad)
                fprintf(stderr, "  base_time %lld/%lld anchor (%lld, %lld) t %lld: got %lld (%d), want %lld\n",
                        (long long)num, (long long)den, (long long)art, (long long)abt, (long long)t,
                        (long long)x, (int)ok, (long long)want);
            bad++;
        }
        /* The inverse: the first RT ns at which the base reaches T. */
        T = oor ? abt + rnd_range(-1000, 1000) : want + rnd_range(-2, 2);
        if (T >= LIM62 || T <= -LIM62) continue;
        {
            int big;
            int64_t c = ref_ceil_md(T - abt, den, num, &big), r = 0, b0 = 0, b1 = 0;
            int in = !big && !(c > 0 && c >= LIM62 - art) && !(c < 0 && c <= -LIM62 - art);
            if (in && (art + c >= LIM62 || art + c <= -LIM62)) in = 0;
            ok = ytl_rt_time(&g_tl, 1, T, &r);
            if (ok != (in != 0) || (ok && r != art + c)) {
                bad_inv++;
            } else if (ok && r - 1 > -LIM62) {
                n_inv++;
                /* bt(r) >= T > bt(r - 1), read back through the header */
                if (ytl_base_time(&g_tl, 1, r, &b0) && ytl_base_time(&g_tl, 1, r - 1, &b1)
                    && !(b0 >= T && b1 < T))
                    bad_inv++;
            }
        }
    }
    CHECK_I(bad, 0);
    CHECK_I(bad_oor, 0);
    CHECK_I(bad_inv, 0);
    CHECK(n_oor > 1000 && n_inv > 100000);
    /* Floor, not truncation, before the anchor: at 1/2 from (0, 0). */
    rate_setup(&g_tl, 2, 0, 0, 1, 2);
    {
        static const int64_t t[7] = { -3, -2, -1, 0, 1, 2, 3 }, w[7] = { -2, -1, -1, 0, 0, 1, 1 };
        int k;
        int64_t x;
        for (k = 0; k < 7; k++) {
            x = 99;
            CHECK(ytl_base_time(&g_tl, 2, t[k], &x));
            CHECK_I(x, w[k]);
        }
        /* and the inverse rounds up: base time 1 is reached at RT 2, -1 at
         * RT -2, 0 at RT 0 */
        CHECK(ytl_rt_time(&g_tl, 2, 1, &x));
        CHECK_I(x, 2);
        CHECK(ytl_rt_time(&g_tl, 2, -1, &x));
        CHECK_I(x, -2);
        CHECK(ytl_rt_time(&g_tl, 2, 0, &x));
        CHECK_I(x, 0);
    }
}

static void test_rate_calls(void) {
    const int64_t M = MS_NS, P = 10 * MS_NS;
    int32_t n = 0, d = 0;
    int64_t x = 0;
    int k;
    ytl_frame f;
    ytl_tween_desc td;
    /* refusals */
    CHECK_I(ytl_rate(&g_closed, 1, 0, 1, 2), YTL_ERR_CLOSED);
    CHECK(!ytl_get_rate(&g_closed, 1, &n, &d));
    CHECK(!ytl_rt_time(&g_closed, 0, 0, &x));
    CHECK_I(ytl_clamped(&g_closed, 1), YTL_ERR_CLOSED);
    CHECK(open_h(&g_tl, g_store, 64, 4, NULL, 0.5));
    CHECK_I(ytl_rate(&g_tl, 0, 0, 1, 2), YTL_ERR_ARG);
    CHECK_I(ytl_rate(&g_tl, YTL_MAX_BASES, 0, 1, 2), YTL_ERR_ARG);
    CHECK_I(ytl_rate(&g_tl, -1, 0, 1, 2), YTL_ERR_ARG);
    CHECK_I(ytl_rate(&g_tl, 1, 0, 0, 2), YTL_ERR_ARG);
    CHECK_I(ytl_rate(&g_tl, 1, 0, 1, 0), YTL_ERR_ARG);
    CHECK_I(ytl_rate(&g_tl, 1, 0, -1, 2), YTL_ERR_ARG);
    CHECK_I(ytl_rate(&g_tl, 1, 0, 1, -2), YTL_ERR_ARG);
    CHECK_I(ytl_rate(&g_tl, 1, LIM62, 1, 2), YTL_ERR_ARG);
    CHECK_I(ytl_rate(&g_tl, 1, -LIM62, 1, 2), YTL_ERR_ARG);
    CHECK(ytl_get_rate(&g_tl, 1, &n, &d) && n == 1 && d == 1);   /* nothing changed */
    CHECK_I(ytl_rate(&g_tl, 1, LIM62 - 1, 1, 1), 0);
    CHECK(!ytl_get_rate(&g_tl, 1, NULL, &d));
    CHECK(!ytl_get_rate(&g_tl, 1, &n, NULL));
    CHECK(!ytl_get_rate(&g_tl, -1, &n, &d));
    CHECK(!ytl_get_rate(&g_tl, YTL_MAX_BASES, &n, &d));
    CHECK(ytl_get_rate(&g_tl, 0, &n, &d) && n == 1 && d == 1);
    CHECK_I(ytl_clamped(&g_tl, -1), YTL_ERR_ARG);
    CHECK_I(ytl_clamped(&g_tl, 1), 0);
    /* reduced by the gcd */
    CHECK_I(ytl_rate(&g_tl, 1, 0, 2, 4), 0);
    CHECK(ytl_get_rate(&g_tl, 1, &n, &d) && n == 1 && d == 2);
    CHECK_I(ytl_rate(&g_tl, 1, 0, 1000, 1000), 0);
    CHECK(ytl_get_rate(&g_tl, 1, &n, &d) && n == 1 && d == 1);
    CHECK_I(ytl_rate(&g_tl, 1, 0, 2147483647, 2147483646), 0);
    CHECK(ytl_get_rate(&g_tl, 1, &n, &d) && n == 2147483647 && d == 2147483646);
    CHECK_I(ytl_rate(&g_tl, 1, 0, 48000, 2002), 0);
    CHECK(ytl_get_rate(&g_tl, 1, &n, &d) && n == 24000 && d == 1001);
    /* rt_time and window refusals */
    CHECK(!ytl_rt_time(&g_tl, 1, 0, &x));                 /* stopped */
    CHECK(ytl_rt_time(&g_tl, 0, 1234, &x) && x == 1234);  /* the RT base */
    CHECK(!ytl_rt_time(&g_tl, 0, LIM62, &x));
    CHECK(!ytl_rt_time(&g_tl, 0, 0, NULL));
    CHECK(!ytl_rt_time(&g_tl, YTL_MAX_BASES, 0, &x));
    f.onset = T0;
    f.period = P;
    f.index = 0;
    CHECK(!ytl_window(&g_tl, 1, &f, &x));                 /* stopped */
    CHECK(ytl_window(&g_tl, 0, &f, &x) && x == T0 + P / 2);
    CHECK(!ytl_window(&g_tl, 0, NULL, &x));
    CHECK(!ytl_window(&g_tl, 0, &f, NULL));
    CHECK(!ytl_window(&g_tl, -1, &f, &x));
    f.period = -1;
    CHECK(!ytl_window(&g_tl, 0, &f, &x));
    f.period = 0;
    CHECK(ytl_window(&g_tl, 0, &f, &x) && x == T0);
    /* LEAD_NONE: the window ends at the onset */
    CHECK(open_h(&g_tl2, g_store2, 4, 1, NULL, 0.0));
    f.period = P;
    CHECK(ytl_window(&g_tl2, 0, &f, &x) && x == T0);

    /* A stopped base keeps the rate for its anchor. */
    CHECK_I(ytl_rate(&g_tl, 2, 0, 1, 2), 0);
    CHECK_I(ytl_anchor(&g_tl, 2, T0, 0), 0);
    CHECK(ytl_base_time(&g_tl, 2, T0 + 11, &x) && x == 5);
    /* A pause at 1/3 freezes the floor; a rate while paused changes nothing
     * now and runs from the resume. */
    rate_setup(&g_tl, 3, T0, 0, 1, 3);
    CHECK_I(ytl_pause(&g_tl, 3, T0 + 10), 0);
    CHECK(ytl_base_time(&g_tl, 3, T0 + 999, &x) && x == 3);
    f.onset = T0 + 999;
    f.period = P;
    CHECK(ytl_window(&g_tl, 3, &f, &x) && x == 3);          /* paused: the frozen time */
    CHECK_I(ytl_rate(&g_tl, 3, T0 + 500, 1, 4), 0);
    CHECK(ytl_base_time(&g_tl, 3, T0 + 999, &x) && x == 3);
    CHECK(!ytl_rt_time(&g_tl, 3, 5, &x));                    /* paused */
    CHECK_I(ytl_resume(&g_tl, 3, T0 + 1000), 0);
    CHECK(ytl_base_time(&g_tl, 3, T0 + 1008, &x) && x == 5);
    CHECK(ytl_get_rate(&g_tl, 3, &n, &d) && n == 1 && d == 4);
    /* anchor, skip, stop, clear, prune keep it; open resets it */
    CHECK_I(ytl_anchor(&g_tl, 3, T0, 0), 0);
    CHECK(ytl_get_rate(&g_tl, 3, &n, &d) && n == 1 && d == 4);
    CHECK(ytl_skip(&g_tl, 3, T0, 100) >= 0);
    CHECK_I(ytl_stop(&g_tl, 3), 0);
    CHECK(ytl_clear(&g_tl, 3) >= 0);
    CHECK(ytl_prune(&g_tl, YTL_ALL_BASES) >= 0);
    CHECK(ytl_get_rate(&g_tl, 3, &n, &d) && n == 1 && d == 4);
    CHECK(open_h(&g_tl, g_store, 64, 4, NULL, 0.5));
    CHECK(ytl_get_rate(&g_tl, 3, &n, &d) && n == 1 && d == 1);

    /* No rewind at the change point: P = 10 ms, lead 0.5, MARKs every
     * 5 ms on base 1. Frames 0 to 4; frame 4's window ends at 45 ms. A
     * change at frame 4's onset (base time 40 ms) to 1/10 leaves every
     * fired event fired, the one at 40 ms included, and the storage as it
     * was. */
    CHECK(open_h(&g_tl, g_store, 64, 4, NULL, 0.5));
    CHECK_I(ytl_anchor(&g_tl, 1, T0, 0), 0);
    for (k = 0; k <= 20; k++) CHECK(add1(&g_tl, 1, k * 5 * M, YTL_MARK, 0, 0.0f) >= 0);
    for (k = 0; k <= 4; k++) (void)eval_at(&g_tl, T0 + k * P, P, k, NULL, 0);
    CHECK_I(find_at(&g_tl, 1, 40 * M, YTL_MARK)->frame, 4);
    CHECK_I(find_at(&g_tl, 1, 45 * M, YTL_MARK)->frame, 4);
    memcpy(g_snap, g_store, 64 * sizeof g_store[0]);
    CHECK_I(ytl_rate(&g_tl, 1, T0 + 4 * P, 1, 10), 0);
    CHECK(memcmp(g_snap, g_store, 64 * sizeof g_store[0]) == 0);
    /* Frame 5's window ends at 40 + 15/10 = 41.5 ms, behind the reach
     * (45 ms). An event added at 44 ms is late and waits for frame 8,
     * whose window ends at 44.5 ms. */
    f.onset = T0 + 5 * P;
    f.period = P;
    f.index = 5;
    CHECK(ytl_window(&g_tl, 1, &f, &x) && x == 41 * M + M / 2);
    CHECK(add1(&g_tl, 1, 44 * M, YTL_MARK, 0, 0.0f) >= 0);
    for (k = 5; k <= 7; k++) CHECK_I(eval_at(&g_tl, T0 + k * P, P, k, NULL, 0), 0);
    CHECK_I(eval_at(&g_tl, T0 + 8 * P, P, 8, g_fired, 4), 1);
    CHECK_I(g_fired[0].time_ns, 44 * M);
    CHECK_I(g_fired[0].flags, YTL_EV_FIRED | YTL_EV_LATE);
    CHECK_I(g_fired[0].residual, 0);   /* base time at frame 8's onset is 44 ms */
    CHECK_I(find_at(&g_tl, 1, 45 * M, YTL_MARK)->frame, 4);
    for (k = 9; k <= 13; k++) CHECK_I(eval_at(&g_tl, T0 + k * P, P, k, NULL, 0), 0);
    CHECK_I(eval_at(&g_tl, T0 + 14 * P, P, 14, NULL, 0), 1);   /* 50 ms: window end 50.5 */
    /* An anchor at 40 ms still rewinds: the change moved no fired event. */
    CHECK_I(ytl_anchor(&g_tl, 1, T0 + 15 * P, 40 * M), 0);
    CHECK(is_pending_reset(find_at(&g_tl, 1, 40 * M, YTL_MARK)));
    CHECK_I(find_at(&g_tl, 1, 35 * M, YTL_MARK)->frame, 3);

    /* "now" after a change between onsets is the base time at the change:
     * frames at 0 and 10 ms, a change to 1/2 at 15 ms (base time 15 ms),
     * then a 10 ms tween from 0 to 1. At frame 2 (base time 15 + 2.5 ms)
     * it shows 0.25. With "now" at the last onset (10 ms) it would show
     * 0.75. */
    CHECK(open_h(&g_tl, g_store, 64, 4, NULL, 0.5));
    CHECK_I(ytl_anchor(&g_tl, 1, T0, 0), 0);
    (void)eval_at(&g_tl, T0, P, 0, NULL, 0);
    (void)eval_at(&g_tl, T0 + P, P, 1, NULL, 0);
    CHECK_I(ytl_rate(&g_tl, 1, T0 + 15 * M, 1, 2), 0);
    td = twd(1.0f, 10 * M);
    CHECK_I(ytl_tween(&g_tl, 0, 1, &td), 0);
    (void)eval_at(&g_tl, T0 + 2 * P, P, 2, NULL, 0);
    CHECK_F(ytl_value(&g_tl, 0), 0.25, 1e-6);
    /* The same rate again changes nothing, "now" included: a tween posted
     * after it starts at frame 2's base time (17.5 ms). */
    memcpy(&g_tl2, &g_tl, sizeof g_tl);
    CHECK_I(ytl_rate(&g_tl, 1, T0 + 23 * M, 2, 4), 0);
    CHECK(memcmp(&g_tl2, &g_tl, sizeof g_tl) == 0);
    td = twd(1.0f, 10 * M);
    CHECK_I(ytl_tween(&g_tl, 1, 1, &td), 0);
    (void)eval_at(&g_tl, T0 + 3 * P, P, 3, NULL, 0);   /* base time 22.5 ms */
    CHECK_F(ytl_value(&g_tl, 1), 0.5, 1e-6);
}

/* Quantization under a rate: an event at base time T lands on the first
 * grid frame k with T0 + k P + w >= rt(T), the rate-1 rule applied to the
 * RT time its base reaches it, with residual bt(onset) - T, and in the
 * report by rt(T) then id. */
static void rate_grid(int64_t P, int lead_num, int lead_den, int64_t num, int64_t den, uint64_t seed) {
    enum { NE = 300, NF = 420 };
    static int64_t ev_t[NE];
    static int ev_id[NE];
    const int64_t w = lead_num ? P * lead_num / lead_den : 0;
    const int64_t art = T0 + 12345, abt = 500 * MS_NS;
    int i, k, bad = 0, bad_order = 0, first_bad = -1, n_land = 0;
    g_rng = seed;
    CHECK(open_h(&g_tl, g_store, NE + 4, 1, NULL, lead_num ? (double)lead_num / (double)lead_den : 0.0));
    rate_setup(&g_tl, 3, art, abt, (int32_t)num, (int32_t)den);
    for (i = 0; i < NE; i++) {
        int64_t kk = rnd_int(NF - 20), t;
        int c = i % 10;
        if (c == 0) t = ref_bt(art, abt, num, den, T0 + kk * P);
        else if (c == 1) t = ref_bt(art, abt, num, den, T0 + kk * P + w);
        else if (c == 2) t = ref_bt(art, abt, num, den, T0 + kk * P + w) + 1;
        else if (c == 3) t = ref_bt(art, abt, num, den, T0 + kk * P + w) - 1;
        else t = rnd_range(ref_bt(art, abt, num, den, T0 - P), ref_bt(art, abt, num, den, T0 + (NF - 20) * P));
        ev_t[i] = t;
        ev_id[i] = add1(&g_tl, 3, t, YTL_MARK, 0, 0.0f);
        CHECK_I(ev_id[i], i);
    }
    for (k = 0; k < NF; k++) {
        int n = eval_at(&g_tl, T0 + k * P, P, 1000 + k, g_fired, STORE_CAP), j;
        for (j = 1; j < n && j < STORE_CAP; j++) {
            int64_t a = ref_rt(art, abt, num, den, g_fired[j - 1].time_ns);
            int64_t b = ref_rt(art, abt, num, den, g_fired[j].time_ns);
            if (a > b || (a == b && g_fired[j - 1].id > g_fired[j].id)) bad_order++;
        }
    }
    for (i = 0; i < NE; i++) {
        const ytl_event* e = ytl_find(&g_tl, ev_id[i]);
        int64_t kk = quant_k(ref_rt(art, abt, num, den, ev_t[i]), P, w), on = T0 + kk * P;
        if (kk >= NF) continue;
        n_land++;
        if (!e || e->frame != 1000 + kk || e->onset != on
            || e->residual != ref_bt(art, abt, num, den, on) - ev_t[i] || e->flags != YTL_EV_FIRED) {
            if (first_bad < 0) first_bad = i;
            bad++;
        }
    }
    if (first_bad >= 0) {
        const ytl_event* e = ytl_find(&g_tl, ev_id[first_bad]);
        fprintf(stderr, "  rate grid P %lld lead %d/%d rate %lld/%lld: event %lld want frame %lld; got %lld residual %lld\n",
                (long long)P, lead_num, lead_den, (long long)num, (long long)den, (long long)ev_t[first_bad],
                (long long)(1000 + quant_k(ref_rt(art, abt, num, den, ev_t[first_bad]), P, w)),
                e ? (long long)e->frame : -99, e ? (long long)e->residual : -99);
    }
    if (bad) fail_i(__LINE__, "rate grid landings that disagree with the reference", bad, 0);
    if (bad_order) fail_i(__LINE__, "rate grid reports out of RT target order", bad_order, 0);
    if (n_land < NE * 9 / 10) fail_i(__LINE__, "rate grid: too few events in the grid (vacuous)", n_land, NE);
}

static void test_rate_quant(void) {
    static const int64_t periods[6] = { 16666667, 6944444, 4166667, 2777778, 2000000, 1000000 };
    static const int64_t rn[6] = { 1, 2, 1001, 1000, 3, 25 }, rd[6] = { 2, 1, 1000, 1001, 7, 24 };
    static const int ln[3] = { 1, 1, 0 }, ld[3] = { 2, 4, 1 };
    int pi, ri, li;
    for (pi = 0; pi < 6; pi++)
        for (ri = 0; ri < 6; ri++)
            for (li = 0; li < 3; li++)
                rate_grid(periods[pi], ln[li], ld[li], rn[ri], rd[ri], (uint64_t)(700 + 100 * pi + 10 * ri + li));
}

/* The change point swept across a frame: a change from r0 to r1 at RT
 * time c, made after frame K-1 is evaluated and before frame K, with c
 * from frame K-1's onset to frame K+1's (and on the onsets, on the window
 * ends, and a ns either side). Frames before K use the old mapping, frames
 * from K the new one, anchored at (c, old bt(c)). Every event lands on the
 * first frame whose window end reaches it; that covers the windows behind
 * the reach after a drop, since nothing un-fires. */
static void rate_sweep(int64_t P, int lead_num, int lead_den, int64_t n0, int64_t d0, int64_t n1, int64_t d1,
                       long* n_checked) {
    enum { NE = 260, NF = 44, K = 20 };
    static int64_t ev_t[NE], ends[NF], ons[NF];
    static int ev_id[NE];
    const int64_t w = lead_num ? P * lead_num / lead_den : 0, art = T0 + 777;
    int64_t cs[48];
    int nc = 0, ci, i, k, bad = 0, bad_win = 0;
    for (i = 0; i <= 32; i++) cs[nc++] = T0 + (K - 1) * P + i * 2 * P / 32;
    cs[nc++] = T0 + K * P - 1;
    cs[nc++] = T0 + K * P + 1;
    cs[nc++] = T0 + K * P + w;
    cs[nc++] = T0 + K * P + w - 1;
    cs[nc++] = T0 + K * P + w + 1;
    cs[nc++] = T0 + (K - 1) * P + w;
    cs[nc++] = T0 + (K - 1) * P + w + 1;
    for (ci = 0; ci < nc; ci++) {
        const int64_t c = cs[ci], btc = ref_bt(art, 0, n0, d0, c);
        g_rng = 9000 + (uint64_t)ci;
        for (k = 0; k < NF; k++) {
            ons[k] = T0 + k * P;
            ends[k] = k < K ? ref_bt(art, 0, n0, d0, ons[k] + w) : ref_bt(c, btc, n1, d1, ons[k] + w);
        }
        CHECK(open_h(&g_tl, g_store, NE + 4, 1, NULL, lead_num ? (double)lead_num / (double)lead_den : 0.0));
        rate_setup(&g_tl, 1, art, 0, (int32_t)n0, (int32_t)d0);
        for (i = 0; i < NE; i++) {
            int kk = K - 4 + rnd_int(12), c3 = i % 8;
            int64_t t;
            if (c3 < 3) t = ends[kk] + (c3 - 1);
            else if (c3 < 6) t = ref_bt(art, 0, n0, d0, ons[kk] + w) + (c3 - 4);
            else if (c3 == 6) t = ref_bt(c, btc, n1, d1, ons[kk] + w) + rnd_range(-1, 1);
            else t = rnd_range(ends[K - 6], ends[NF - 4]);
            ev_t[i] = t;
            ev_id[i] = add1(&g_tl, 1, t, YTL_MARK, 0, 0.0f);
        }
        for (k = 0; k < NF; k++) {
            ytl_frame f;
            int64_t e = 0;
            if (k == K) CHECK_I(ytl_rate(&g_tl, 1, c, (int32_t)n1, (int32_t)d1), 0);
            f.onset = ons[k];
            f.period = P;
            f.index = k;
            if (!ytl_window(&g_tl, 1, &f, &e) || e != ends[k]) bad_win++;
            (void)ytl_evaluate(&g_tl, &f, NULL, 0);
        }
        for (i = 0; i < NE; i++) {
            const ytl_event* e = ytl_find(&g_tl, ev_id[i]);
            int64_t want = -1, res = 0;
            for (k = 0; k < NF; k++) {
                if (ends[k] >= ev_t[i]) {
                    want = k;
                    res = (k < K ? ref_bt(art, 0, n0, d0, ons[k]) : ref_bt(c, btc, n1, d1, ons[k])) - ev_t[i];
                    break;
                }
            }
            (*n_checked)++;
            if (!e || e->frame != want || (want >= 0 && (e->residual != res || e->onset != ons[want]))) {
                if (!bad)
                    fprintf(stderr, "  sweep P %lld lead %d/%d %lld/%lld -> %lld/%lld at %lld: event %lld "
                            "want frame %lld, got %lld\n", (long long)P, lead_num, lead_den, (long long)n0,
                            (long long)d0, (long long)n1, (long long)d1, (long long)(c - T0 - K * P),
                            (long long)ev_t[i], (long long)want, e ? (long long)e->frame : -99);
                bad++;
            }
        }
    }
    if (bad) fail_i(__LINE__, "sweep landings that disagree with the reference", bad, 0);
    if (bad_win) fail_i(__LINE__, "sweep windows that disagree with the reference", bad_win, 0);
}

static void test_rate_sweep(void) {
    static const int64_t pr[5][4] = { { 1, 1, 1, 2 }, { 1, 2, 3, 1 }, { 1001, 1000, 999, 1000 },
                                      { 1, 1, 1, 10 }, { 7, 3, 1, 1 } };
    static const int64_t periods[2] = { 2000000, 1000000 };
    long n = 0;
    int pi, ri;
    for (pi = 0; pi < 2; pi++)
        for (ri = 0; ri < 5; ri++) {
            rate_sweep(periods[pi], 1, 2, pr[ri][0], pr[ri][1], pr[ri][2], pr[ri][3], &n);
            rate_sweep(periods[pi], 0, 1, pr[ri][0], pr[ri][1], pr[ri][2], pr[ri][3], &n);
        }
    CHECK(n > 200000);   /* 2 periods x 5 pairs x 2 leads x 40 change points x 260 events */
}

static void test_rate_tracks(void) {
    const int64_t M = MS_NS;
    int i, bad = 0, trial;
    ytl_tween_desc td;
    /* A 200 ms tween on a base at 1/2 takes 400 ms of RT. */
    CHECK(open_h(&g_tl, g_store, 8, 4, NULL, 0.5));
    rate_setup(&g_tl, 1, T0, 0, 1, 2);
    td = twd_at(1.0f, 200 * M, 0);
    CHECK_I(ytl_tween(&g_tl, 0, 1, &td), 0);
    for (i = 0; i <= 10; i++) {
        (void)eval_at(&g_tl, T0 + i * 40 * M, 0, i, NULL, 0);
        CHECK_F(ytl_value(&g_tl, 0), i / 10.0, 1e-6);
    }
    /* keep_velocity across a rate change: the value is continuous and the
     * RT velocity steps by exactly the ratio of the rates (1 to 1/2). The
     * old track moves 10 per s; the tween takes over at 500 ms; the rate
     * changes at RT 700 ms. Frames every 1 ms. */
    {
        static ytl_key lin[2];
        int64_t c = T0 + 700 * M;
        double v[5];
        lin[0] = mkkey(0, 0.0f, YTL_EASE_LINEAR);
        lin[1] = mkkey(S_NS, 10.0f, YTL_EASE_LINEAR);
        CHECK(open_h(&g_tl, g_store, 8, 4, NULL, 0.5));
        CHECK_I(ytl_anchor(&g_tl, 1, T0, 0), 0);
        CHECK_I(ytl_set_keys(&g_tl, 0, 1, lin, 2), 0);
        td = twd_at(20.0f, S_NS, 500 * M);
        td.keep_velocity = true;
        CHECK_I(ytl_tween(&g_tl, 0, 1, &td), 0);
        for (i = 0; i < 698; i++) (void)eval_at(&g_tl, T0 + i * M, M, i, NULL, 0);
        for (i = 0; i < 5; i++) {
            if (i == 2) CHECK_I(ytl_rate(&g_tl, 1, c, 1, 2), 0);
            (void)eval_at(&g_tl, c + (i - 2) * M, M, 698 + i, NULL, 0);
            v[i] = ytl_value(&g_tl, 0);
        }
        /* v[2] is at the change; the steps before and after it */
        CHECK_F((v[3] - v[2]) / (v[2] - v[1]), 0.5, 0.01);
        CHECK_F((v[4] - v[3]) / (v[1] - v[0]), 0.5, 0.01);
    }
    /* A STEP track's change lands on the frame an event at the same time
     * lands on, at 1001/1000 and 1/3, 300 random cases each. */
    for (trial = 0; trial < 600; trial++) {
        static ytl_key st[2];
        int64_t P, art, T;
        int32_t num = trial < 300 ? 1001 : 1, den = trial < 300 ? 1000 : 3;
        int fs = -1, fe = -1, k;
        double lead;
        g_rng = 5000 + (uint64_t)trial;
        P = rnd_int(2) ? P60 : 2000000;
        art = T0 + rnd_range(0, P);
        lead = rnd_int(2) ? 0.5 : 0.0;
        CHECK(open_h(&g_tl, g_store, 4, 3, NULL, lead));
        rate_setup(&g_tl, 1, art, 0, num, den);
        T = rnd_range(5 * P, 30 * P) * num / den;
        if (rnd_int(2))
            T = ref_bt(art, 0, num, den, T0 + rnd_range(5, 30) * P + (lead > 0 ? P / 2 : 0)) + rnd_range(-1, 1);
        st[0] = mkkey(T - S_NS, 0.0f, YTL_EASE_STEP);
        st[1] = mkkey(T, 1.0f, YTL_EASE_LINEAR);
        CHECK_I(ytl_set_keys(&g_tl, 1, 1, st, 2), 0);
        CHECK(add1(&g_tl, 1, T, YTL_ONSET, 2, 0.0f) >= 0);
        for (k = 0; k < 400 && (fs < 0 || fe < 0); k++) {
            (void)eval_at(&g_tl, T0 + k * P, P, k, NULL, 0);
            if (fs < 0 && ytl_value(&g_tl, 1) == 1.0f) fs = k;
            if (fe < 0 && ytl_value(&g_tl, 2) == 1.0f) fe = k;
        }
        if (fs != fe || fs < 0) bad++;
    }
    CHECK_I(bad, 0);
}

static void test_rate_peek(void) {
    const int64_t M = MS_NS;
    static const int32_t rn[2] = { 1, 7 }, rd[2] = { 3, 3 };
    int ri, i, bad = 0;
    for (ri = 0; ri < 2; ri++) {
        int64_t due = 0, mdue = INT64_MAX;
        g_rng = 77 + (uint64_t)ri;
        CHECK(open_h(&g_tl, g_store, 64, 1, NULL, 0.5));
        rate_setup(&g_tl, 2, T0 + 5, 1000, rn[ri], rd[ri]);
        for (i = 0; i < 50; i++) CHECK(add1(&g_tl, 2, 1000 + rnd_range(0, 3000), YTL_MARK, 0, 0.0f) >= 0);
        for (i = 0; i < 50; i++) {
            const ytl_event* e = ytl_find(&g_tl, i);
            int64_t r = ref_rt(T0 + 5, 1000, rn[ri], rd[ri], e->time_ns);
            int j;
            if (r < mdue) mdue = r;
            for (j = -1; j <= 1; j++) {
                int n = ytl_peek(&g_tl, 2, r + j, r + j + 1, g_fired, 64), m, in = 0;
                for (m = 0; m < n && m < 64; m++)
                    if (g_fired[m].id == i) {
                        in = 1;
                        if (g_fired[m].onset != r || g_fired[m].residual != 0) bad++;
                    }
                if (in != (j == 0)) bad++;
            }
        }
        CHECK(ytl_next_due(&g_tl, &due));
        CHECK_I(due, mdue);
    }
    CHECK_I(bad, 0);
    /* The report's order across bases at 1/2 and 3/1: base 1's event at
     * 4 ms is reached at RT 8 ms, base 2's at 27 ms at RT 9 ms, so base 1's
     * comes first. By onset - residual it would be 9 against 7 ms. */
    CHECK(open_h(&g_tl, g_store, 8, 1, NULL, 0.5));
    rate_setup(&g_tl, 1, T0, 0, 1, 2);
    rate_setup(&g_tl, 2, T0, 0, 3, 1);
    CHECK(add1(&g_tl, 2, 27 * M, YTL_MARK, 0, 0.0f) >= 0);
    CHECK(add1(&g_tl, 1, 4 * M, YTL_MARK, 0, 0.0f) >= 0);
    CHECK_I(eval_at(&g_tl, T0 + 10 * M, 0, 0, g_fired, 4), 2);
    CHECK_I(g_fired[0].base, 1);
    CHECK_I(g_fired[0].residual, M);
    CHECK_I(g_fired[1].base, 2);
    CHECK_I(g_fired[1].residual, 3 * M);
}

/* No drift: 1001/1000 held for 10 hours is the exact floor at any time,
 * and 1000 changes leave the base behind the exact rational time by less
 * than n + 1 ns after n of them, never ahead. */
static void test_rate_drift(void) {
    const int64_t H10 = 36000 * S_NS;
    int i, bad = 0, ahead = 0;
    int64_t x = 0, exact1000 = 0, last = T0, worst = 0;
    g_rng = 31337;
    CHECK(open_h(&g_tl, g_store, 4, 1, NULL, 0.5));
    rate_setup(&g_tl, 1, T0, 0, 1001, 1000);
    for (i = 0; i < 10000; i++) {
        int64_t t = T0 + rnd_range(0, H10);
        if (!ytl_base_time(&g_tl, 1, t, &x) || x != ref_bt(T0, 0, 1001, 1000, t)) bad++;
    }
    CHECK(ytl_base_time(&g_tl, 1, T0 + H10, &x));
    CHECK_I(x, H10 + H10 / 1000);
    CHECK_I(bad, 0);
    CHECK(open_h(&g_tl, g_store, 4, 1, NULL, 0.5));
    rate_setup(&g_tl, 1, T0, 0, 1001, 1000);
    for (i = 1; i <= 1000; i++) {
        int64_t c = T0 + i * (36 * S_NS) + rnd_range(0, 999), lag;
        exact1000 += (c - last) * (i % 2 ? 1001 : 999);
        last = c;
        CHECK_I(ytl_rate(&g_tl, 1, c, i % 2 ? 999 : 1001, 1000), 0);
        CHECK(ytl_base_time(&g_tl, 1, c, &x));
        lag = exact1000 - x * 1000;   /* thousandths of a ns */
        if (lag < 0) ahead++;
        if (lag > worst) worst = lag;
        if (lag >= (int64_t)(i + 1) * 1000) bad++;
    }
    CHECK_I(ahead, 0);
    CHECK_I(bad, 0);
    CHECK(worst > 1000);   /* not vacuous: the floors did add up */
}

/* Out of range: a time at or past +-2^62 becomes +-2^62 only with a
 * record (ytl_clamped), the queries refuse it, and comparisons with
 * event times keep their exact answers. */
static void test_rate_clamp(void) {
    int64_t x = 0;
    ytl_frame f;
    CHECK(open_h(&g_tl, g_store, 8, 1, NULL, 0.5));
    rate_setup(&g_tl, 1, 0, 0, 2147483647, 1);
    CHECK(add1(&g_tl, 1, LIM62 - 1, YTL_MARK, 0, 0.0f) >= 0);
    CHECK_I(ytl_clamped(&g_tl, 1), 0);
    CHECK(!ytl_base_time(&g_tl, 1, INT64_C(1) << 40, &x));
    f.onset = INT64_C(1) << 40;
    f.period = P60;
    f.index = 0;
    CHECK(!ytl_window(&g_tl, 1, &f, &x));
    CHECK_I(ytl_evaluate(&g_tl, &f, g_fired, 4), 1);
    CHECK_I(g_fired[0].residual, 1);
    CHECK_I(ytl_clamped(&g_tl, 1), 1);
    CHECK_I(ytl_rate(&g_tl, 1, INT64_C(1) << 41, 1, 2), YTL_ERR_ARG);
    CHECK_I(ytl_pause(&g_tl, 1, INT64_C(1) << 41), 0);
    CHECK_I(ytl_clamped(&g_tl, 1), 2);
    CHECK(!ytl_base_time(&g_tl, 1, 0, &x));
    /* the far side: before the anchor the base is at -2^62, and an event
     * just inside the range is not reached */
    CHECK(open_h(&g_tl, g_store, 8, 1, NULL, 0.5));
    rate_setup(&g_tl, 2, 0, 0, 2147483647, 1);
    CHECK(add1(&g_tl, 2, -LIM62 + 1, YTL_MARK, 0, 0.0f) >= 0);
    f.onset = -(INT64_C(1) << 40);
    f.index = 1;
    CHECK_I(ytl_evaluate(&g_tl, &f, NULL, 0), 0);
    CHECK(is_pending_reset(find_at(&g_tl, 2, -LIM62 + 1, YTL_MARK)));
    CHECK_I(ytl_clamped(&g_tl, 2), 1);
    /* a base that reaches an event only past the range: next_due says
     * +2^62, a time no clock reaches */
    CHECK(open_h(&g_tl, g_store, 8, 1, NULL, 0.5));
    rate_setup(&g_tl, 3, 0, 0, 1, 2147483647);
    CHECK(add1(&g_tl, 3, INT64_C(1) << 40, YTL_MARK, 0, 0.0f) >= 0);
    CHECK(ytl_next_due(&g_tl, &x));
    CHECK_I(x, LIM62);
    CHECK(!ytl_rt_time(&g_tl, 3, INT64_C(1) << 40, &x));
    CHECK(ytl_rt_time(&g_tl, 3, 1, &x) && x == 2147483647);
    CHECK_I(ytl_clamped(&g_tl, 3), 0);
    /* Exactly 2^62 is out of range, at rate 1 as at a rate, and an
     * evaluate that meets it says so. */
    CHECK(open_h(&g_tl, g_store, 8, 1, NULL, 0.5));
    CHECK_I(ytl_anchor(&g_tl, 1, 0, LIM62 - 10), 0);
    rate_setup(&g_tl, 2, 0, LIM62 - 20, 2, 1);
    CHECK(ytl_base_time(&g_tl, 1, 9, &x) && x == LIM62 - 1);
    CHECK(!ytl_base_time(&g_tl, 1, 10, &x));
    CHECK(ytl_base_time(&g_tl, 2, 9, &x) && x == LIM62 - 2);
    CHECK(!ytl_base_time(&g_tl, 2, 10, &x));
    f.onset = 9;
    f.period = 0;
    f.index = 0;
    (void)ytl_evaluate(&g_tl, &f, NULL, 0);
    CHECK_I(ytl_clamped(&g_tl, 1), 0);
    CHECK_I(ytl_clamped(&g_tl, 2), 0);
    f.onset = 10;
    f.index = 1;
    (void)ytl_evaluate(&g_tl, &f, NULL, 0);
    CHECK_I(ytl_clamped(&g_tl, 1), 1);
    CHECK_I(ytl_clamped(&g_tl, 2), 1);
}

/* ------------------------------------------------- 11d. SEQUENCES */

#define SQ_ARENA 4096
static ytl_key g_ka[SQ_ARENA], g_kb[SQ_ARENA], g_kc[SQ_ARENA];
static ytl_timeline g_tl3;
static ytl_event g_store3[STORE_CAP];

static bool seq_open(ytl_timeline* tl, ytl_event* st, int cap, ytl_key* keys, int kcap, int nch,
                     const float* init) {
    ytl_desc d;
    memset(&d, 0, sizeof d);
    if (st && cap > 0) memset(st, 0, (size_t)cap * sizeof *st);
    if (keys && kcap > 0) memset(keys, 0, (size_t)kcap * sizeof *keys);
    d.events = st;
    d.event_capacity = cap;
    d.n_channels = nch;
    d.initial = init;
    d.keys = keys;
    d.key_capacity = kcap;
    return ytl_open(tl, &d);
}

enum { SQ_FIX, SQ_GRATING, SQ_CONTRAST, SQ_NCH };

static ytl_tween_desc sq_tw(float to, int64_t dur, int ease) {
    ytl_tween_desc d;
    memset(&d, 0, sizeof d);
    d.to = to;
    d.duration_ns = dur;
    d.ease = ease;
    return d;
}

/* The rig_spec 4.5.1 trial, with the builder. */
static void seq_trial_builder(ytl_timeline* tl, ytl_seq* out) {
    ytl_seq q = ytl_seq_on(tl, 1);
    ytl_tween_desc up = sq_tw(0.5f, YRT_MS(100), YTL_EASE_COSINE);
    ytl_tween_desc down = sq_tw(0.0f, YRT_MS(100), YTL_EASE_COSINE);
    ytl_on(&q, SQ_FIX);
    ytl_wait(&q, YRT_MS(500));
    ytl_off(&q, SQ_FIX);
    ytl_on(&q, SQ_GRATING);
    ytl_trigger(&q, 12);
    ytl_to(&q, SQ_CONTRAST, &up);
    ytl_wait(&q, YRT_MS(600));
    ytl_then(&q, SQ_CONTRAST, &down);
    ytl_off(&q, SQ_GRATING);
    *out = q;
}

/* The same trial as a table. */
static void seq_trial_ops(ytl_op* ops, int* n) {
    int k = 0;
    memset(ops, 0, 9 * sizeof *ops);
    ops[k].op = YTL_OP_ON; ops[k++].ch = SQ_FIX;
    ops[k].op = YTL_OP_WAIT; ops[k++].t_ns = YRT_MS(500);
    ops[k].op = YTL_OP_OFF; ops[k++].ch = SQ_FIX;
    ops[k].op = YTL_OP_ON; ops[k++].ch = SQ_GRATING;
    ops[k].op = YTL_OP_TRIGGER; ops[k++].code = 12;
    ops[k].op = YTL_OP_TO; ops[k].ch = SQ_CONTRAST; ops[k++].tween = sq_tw(0.5f, YRT_MS(100), YTL_EASE_COSINE);
    ops[k].op = YTL_OP_WAIT; ops[k++].t_ns = YRT_MS(600);
    ops[k].op = YTL_OP_THEN; ops[k].ch = SQ_CONTRAST; ops[k++].tween = sq_tw(0.0f, YRT_MS(100), YTL_EASE_COSINE);
    ops[k].op = YTL_OP_OFF; ops[k++].ch = SQ_GRATING;
    *n = k;
}

/* The draft trial's CONTRAST: both ramps play in full, a re-anchor replays
 * them bit for bit, and the table gives the same as the builder. */
static void test_seq_trial(void) {
    static float va[100], vb[100];
    static ytl_key ref[4];
    static ytl_op ops[9];
    const int64_t M = MS_NS;
    ytl_seq q;
    int k, n_ops, n1[100], n2[100], bad_ref = 0;
    ref[0] = mkkey(500 * M, 0.0f, YTL_EASE_COSINE);
    ref[1] = mkkey(600 * M, 0.5f, YTL_EASE_STEP);
    ref[2] = mkkey(1100 * M, 0.5f, YTL_EASE_COSINE);
    ref[3] = mkkey(1200 * M, 0.0f, YTL_EASE_LINEAR);
    CHECK(seq_open(&g_tl, g_store, 64, g_ka, 64, SQ_NCH, NULL));
    seq_trial_builder(&g_tl, &q);
    CHECK_I(q.err, 0);
    CHECK_I(q.n_calls, 9);
    CHECK_I(q.t, YRT_MS(1200));
    CHECK_I(ytl_anchor(&g_tl, 1, T0, 0), 0);
    for (k = 0; k < 90; k++) {
        int64_t bt = k * P60;
        float want = (float)ksample(ref, 4, bt);
        n1[k] = eval_at(&g_tl, T0 + bt, P60, k, NULL, 0);
        va[k] = ytl_value(&g_tl, SQ_CONTRAST);
        if (va[k] != want) bad_ref++;
    }
    CHECK_I(bad_ref, 0);
    /* The ramp up plays: frame 33 is at 550 ms, half way up. The draft's
     * lowering left 0 there (the second tween replaced the first). */
    CHECK_F(va[33], 0.25, 1e-3);
    CHECK(va[35] > 0.4f);
    CHECK_F(va[36], 0.5, 0.0);
    CHECK_F(va[69], 0.25, 1e-3);   /* and the ramp down */
    CHECK_F(va[72], 0.0, 0.0);
    /* Re-run by re-anchor: the same values on every frame, bit for bit. */
    CHECK_I(ytl_anchor(&g_tl, 1, T0 + 100 * P60, 0), 0);
    for (k = 0; k < 90; k++) {
        n2[k] = eval_at(&g_tl, T0 + (100 + k) * P60, P60, 100 + k, NULL, 0);
        vb[k] = ytl_value(&g_tl, SQ_CONTRAST);
    }
    CHECK(memcmp(va, vb, 90 * sizeof va[0]) == 0);
    CHECK(memcmp(n1, n2, 90 * sizeof n1[0]) == 0);
    /* The table: the same storage and the same values. */
    seq_trial_ops(ops, &n_ops);
    CHECK(seq_open(&g_tl2, g_store2, 64, g_kb, 64, SQ_NCH, NULL));
    CHECK(seq_open(&g_tl3, g_store3, 64, g_kc, 64, SQ_NCH, NULL));
    {
        ytl_seq r = ytl_seq_on(&g_tl2, 1), b;
        int bad = 99;
        CHECK_I(ytl_check_ops(&g_tl2, 1, ops, n_ops, &bad), 0);
        CHECK_I(bad, -1);
        CHECK_I(ytl_run(&r, ops, n_ops), 0);
        CHECK_I(r.err, 0);
        CHECK_I(r.t, YRT_MS(1200));
        seq_trial_builder(&g_tl3, &b);
        CHECK_I(b.err, 0);
    }
    CHECK_I(ytl_anchor(&g_tl2, 1, T0, 0), 0);
    CHECK_I(ytl_anchor(&g_tl3, 1, T0, 0), 0);
    bad_ref = 0;
    for (k = 0; k < 90; k++) {
        (void)eval_at(&g_tl2, T0 + k * P60, P60, k, NULL, 0);
        (void)eval_at(&g_tl3, T0 + k * P60, P60, k, NULL, 0);
        if (ytl_value(&g_tl2, SQ_CONTRAST) != va[k] || ytl_value(&g_tl3, SQ_CONTRAST) != va[k]) bad_ref++;
    }
    CHECK_I(bad_ref, 0);
    CHECK(memcmp(g_store2, g_store3, 64 * sizeof g_store2[0]) == 0);
}

/* A failed ytl_run changes nothing (memcmp of the handle, the storage
 * and the arena), reports the op's index, and agrees with ytl_check_ops. */
static void seq_run_fail(int line, ytl_op* ops, int n, int want_rc, int want_bad, int ecap, int kcap) {
    static ytl_timeline snap;
    static ytl_event sst[64];
    static ytl_key sk[64];
    ytl_seq q;
    int rc, bad = 99;
    CHECK(seq_open(&g_tl, g_store, ecap, g_ka, kcap, SQ_NCH, NULL));
    q = ytl_seq_on(&g_tl, 1);
    memcpy(&snap, &g_tl, sizeof snap);
    memcpy(sst, g_store, sizeof sst);
    memcpy(sk, g_ka, sizeof sk);
    rc = ytl_check_ops(&g_tl, 1, ops, n, &bad);
    if (rc != want_rc || bad != want_bad) fail_i(line, "check_ops code x 1000 + index", rc * 1000 + bad, want_rc * 1000 + want_bad);
    rc = ytl_run(&q, ops, n);
    if (rc != want_rc || q.err != want_rc || q.err_call != want_bad)
        fail_i(line, "run code x 1000 + err_call", rc * 1000 + q.err_call, want_rc * 1000 + want_bad);
    if (memcmp(&snap, &g_tl, TL_STATE) != 0 || memcmp(sst, g_store, sizeof sst) != 0 || memcmp(sk, g_ka, sizeof sk) != 0)
        fail(line, "a failed ytl_run changed the handle, the storage or the arena");
    if (strncmp(ytl_error(&g_tl), "ysp_timeline: sequence call ", 28) != 0)
        fail(line, "a failed ytl_run wrote no sequence message");
    /* sticky: nothing after it */
    ytl_on(&q, SQ_FIX);
    if (n_events(&g_tl, 1) != 0) fail(line, "a call after the error added an event");
}

static void test_seq_run(void) {
    static ytl_op ops[12];
    int n;
    seq_trial_ops(ops, &n);
    /* a zeroed op */
    seq_trial_ops(ops, &n);
    memset(&ops[3], 0, sizeof ops[3]);
    seq_run_fail(__LINE__, ops, n, YTL_ERR_ARG, 3, 64, 64);
    /* start_set: the cursor wins */
    seq_trial_ops(ops, &n);
    ops[5].tween.start_set = true;
    seq_run_fail(__LINE__, ops, n, YTL_ERR_ARG, 5, 64, 64);
    /* FOREVER */
    seq_trial_ops(ops, &n);
    ops[7].tween.cycles = YTL_FOREVER;
    seq_run_fail(__LINE__, ops, n, YTL_ERR_ARG, 7, 64, 64);
    /* an event on the tweened channel */
    seq_trial_ops(ops, &n);
    ops[8].ch = SQ_CONTRAST;
    seq_run_fail(__LINE__, ops, n, YTL_ERR_BOUND, 8, 64, 64);
    /* a tween on an event channel */
    seq_trial_ops(ops, &n);
    ops[7].ch = SQ_FIX;
    seq_run_fail(__LINE__, ops, n, YTL_ERR_BOUND, 7, 64, 64);
    /* overlap: the second tween starts inside the first */
    seq_trial_ops(ops, &n);
    ops[6].t_ns = YRT_MS(50);
    seq_run_fail(__LINE__, ops, n, YTL_ERR_ARG, 7, 64, 64);
    /* a channel out of range */
    seq_trial_ops(ops, &n);
    ops[3].ch = SQ_NCH;
    seq_run_fail(__LINE__, ops, n, YTL_ERR_ARG, 3, 64, 64);
    /* the storage: 5 events need 5 */
    seq_trial_ops(ops, &n);
    seq_run_fail(__LINE__, ops, n, YTL_ERR_FULL, 8, 4, 64);
    /* the arena: the CONTRAST block is 5 slots with its header, and the
     * check wants room for one more copy of it: 10. 9 is refused at the
     * op that makes it 5. */
    seq_trial_ops(ops, &n);
    seq_run_fail(__LINE__, ops, n, YTL_ERR_FULL, 7, 64, 9);
    {
        ytl_seq q;
        CHECK(seq_open(&g_tl, g_store, 64, g_ka, 10, SQ_NCH, NULL));
        q = ytl_seq_on(&g_tl, 1);
        CHECK_I(ytl_run(&q, ops, n), 0);
        /* the builder needs only what it uses: 5 slots */
        CHECK(seq_open(&g_tl, g_store, 64, g_ka, 5, SQ_NCH, NULL));
        seq_trial_builder(&g_tl, &q);
        CHECK_I(q.err, 0);
        /* and with 4 the second tween is refused, the first kept */
        CHECK(seq_open(&g_tl, g_store, 64, g_ka, 4, SQ_NCH, NULL));
        seq_trial_builder(&g_tl, &q);
        CHECK_I(q.err, YTL_ERR_FULL);
        CHECK_I(q.err_call, 7);
        CHECK_I(n_events(&g_tl, 1), 4);   /* the call after the error added nothing */
        CHECK_I(ytl_anchor(&g_tl, 1, T0, 0), 0);
        (void)eval_at(&g_tl, T0 + YRT_MS(700), 0, 0, NULL, 0);
        CHECK_F(ytl_value(&g_tl, SQ_CONTRAST), 0.5, 0.0);
    }
    /* no arena at all */
    seq_trial_ops(ops, &n);
    seq_run_fail(__LINE__, ops, n, YTL_ERR_FULL, 5, 64, 0);
    /* builder refusals and the sticky error */
    {
        ytl_seq q;
        ytl_tween_desc d = sq_tw(1.0f, YRT_MS(10), 0);
        int64_t r;
        CHECK(seq_open(&g_tl, g_store, 64, g_ka, 64, SQ_NCH, NULL));
        q = ytl_seq_on(&g_closed, 1);
        CHECK_I(q.err, YTL_ERR_CLOSED);
        CHECK_I(q.err_call, -1);
        q = ytl_seq_on(&g_tl, YTL_MAX_BASES);
        CHECK_I(q.err, YTL_ERR_ARG);
        q = ytl_seq_on(&g_tl, 1);
        ytl_wait(&q, LIM62);
        CHECK_I(q.err, YTL_ERR_ARG);
        CHECK_I(q.err_call, 0);
        r = ytl_to(&q, SQ_CONTRAST, &d);
        CHECK_I(r, 0);
        CHECK_I(q.n_calls, 2);
        CHECK_I(q.err_call, 0);
        q = ytl_seq_on(&g_tl, 1);
        CHECK_I(ytl_to(&q, SQ_CONTRAST, NULL), 0);
        CHECK_I(q.err, YTL_ERR_ARG);
        q = ytl_seq_on(&g_tl, 1);
        d.cycles = 3;
        d.yoyo = true;
        CHECK_I(ytl_to(&q, SQ_CONTRAST, &d), 3 * 2 * YRT_MS(10));
        CHECK_I(q.t, 0);
        d.keep_velocity = true;
        d.cycles = 0;
        d.yoyo = false;
        ytl_at(&q, YRT_MS(100));
        CHECK_I(ytl_to(&q, SQ_CONTRAST, &d), YRT_MS(100));   /* keep_velocity after the first: refused */
        CHECK_I(q.err, YTL_ERR_ARG);
        CHECK_I(q.err_call, 2);
    }
}

/* ytl_seq_cancel: the sequence's events and blocks go; what was on the
 * base before stays. */
static void test_seq_cancel(void) {
    static ytl_key lin[2];
    ytl_seq q;
    ytl_tween_desc d = sq_tw(2.0f, YRT_MS(100), 0);
    float v3;
    int c;
    lin[0] = mkkey(0, 0.0f, 0);
    lin[1] = mkkey(S_NS, 1.0f, 0);
    CHECK(seq_open(&g_tl, g_store, 64, g_ka, 64, 6, NULL));
    CHECK(add1(&g_tl, 1, 0, YTL_ONSET, 0, 0.0f) >= 0);
    CHECK(add1(&g_tl, 1, 0, YTL_SET, 1, 3.0f) >= 0);   /* ch 1 keeps this one */
    CHECK_I(ytl_set_keys(&g_tl, 3, 1, lin, 2), 0);
    CHECK_I(ytl_anchor(&g_tl, 1, T0, 0), 0);
    (void)eval_at(&g_tl, T0, P60, 0, NULL, 0);
    q = ytl_seq_now(&g_tl, 1);
    ytl_on(&q, 1);
    ytl_to(&q, 2, &d);   /* starts at now: the next evaluate promotes it */
    ytl_wait(&q, YRT_MS(50));
    ytl_to(&q, 3, &d);   /* takes over ch 3 at 50 ms: still waiting at the cancel */
    ytl_set_value(&q, 4, 7.0f);
    CHECK_I(q.err, 0);
    CHECK_I(n_events(&g_tl, 1), 4);
    (void)eval_at(&g_tl, T0 + P60, P60, 1, NULL, 0);   /* the ONSET on ch 1 fires */
    CHECK_F(ytl_value(&g_tl, 1), 1.0, 0.0);
    c = ytl_seq_cancel(&q);
    CHECK_F(ytl_value(&g_tl, 1), 3.0, 0.0);           /* recomputed from the SET */
    CHECK_F(ytl_value(&g_tl, 2), 2.0 * (double)P60 / (double)YRT_MS(100), 1e-6);   /* promoted: held */
    CHECK_I(c, 2);
    CHECK_I(n_events(&g_tl, 1), 2);
    CHECK(ytl_find(&g_tl, 0) != NULL);
    /* ch 2 is free again, ch 3 still runs its keys */
    CHECK(add1(&g_tl, 1, 0, YTL_ONSET, 2, 0.0f) >= 0);
    (void)eval_at(&g_tl, T0 + YRT_MS(500), P60, 2, NULL, 0);
    v3 = ytl_value(&g_tl, 3);
    CHECK_F(v3, 0.5, 1e-6);
    /* the sequence does nothing more */
    ytl_on(&q, 5);
    CHECK_I(q.err, YTL_ERR_CLOSED);
    CHECK_I(n_events(&g_tl, 1), 3);
    CHECK_I(ytl_seq_cancel(&q), YTL_ERR_CLOSED);
}

/* A sequence's one tween equals ytl_tween() with the same desc: the
 * same takeover from a moving track, the same keys, the same bits. */
static void test_seq_vs_tween(void) {
    static ytl_key mv[3];
    int trial, k, bad = 0, bad_n = 0;
    g_rng = 4711;
    mv[0] = mkkey(0, -1.0f, YTL_EASE_LINEAR);
    mv[1] = mkkey(YRT_MS(700), 3.0f, YTL_EASE_QUAD_IN);
    mv[2] = mkkey(S_NS, 2.0f, YTL_EASE_LINEAR);
    for (trial = 0; trial < 400; trial++) {
        ytl_tween_desc d;
        ytl_seq q;
        int64_t c = rnd_range(0, YRT_MS(600)), end;
        bool moving = rnd_int(3) != 0;
        memset(&d, 0, sizeof d);
        d.to = (float)rnd_int(64) / 8.0f - 2.0f;
        d.duration_ns = rnd_range(1, YRT_MS(300));
        d.ease = rnd_int(7);
        if (d.ease == YTL_EASE_LOG) d.ease = YTL_EASE_QUAD_OUT;
        if (d.ease == YTL_EASE_BEZIER) d.curve = mkcurve(0.42f, -0.2f, 0.58f, 1.3f);
        d.from_set = rnd_int(3) == 0;
        d.from = (float)rnd_int(64) / 8.0f;
        d.cycles = rnd_int(4);
        d.yoyo = rnd_int(3) == 0;
        d.delay_ns = rnd_int(2) ? rnd_range(-YRT_MS(50), YRT_MS(50)) : 0;
        if (rnd_int(3) == 0) {
            d.keep_velocity = true;
            d.ease = 0;
            d.from_set = false;
            d.yoyo = false;
            d.cycles = rnd_int(2);
        }
        CHECK(seq_open(&g_tl, g_store, 8, g_ka, 256, 2, NULL));
        CHECK(seq_open(&g_tl2, g_store2, 8, g_kb, 256, 2, NULL));
        if (moving) {
            CHECK_I(ytl_set_keys(&g_tl, 1, 1, mv, 3), 0);
            CHECK_I(ytl_set_keys(&g_tl2, 1, 1, mv, 3), 0);
        }
        q = ytl_seq_on(&g_tl, 1);
        ytl_at(&q, c);
        end = ytl_to(&q, 1, &d);
        if (q.err != 0) { bad_n++; continue; }
        d.start_ns = c;
        d.start_set = true;
        CHECK_I(ytl_tween(&g_tl2, 1, 1, &d), 0);
        d.start_set = false;
        CHECK_I(end, c + d.delay_ns + (d.cycles > 1 ? d.cycles : 1) * d.duration_ns * (d.yoyo ? 2 : 1));
        CHECK_I(ytl_anchor(&g_tl, 1, T0, 0), 0);
        CHECK_I(ytl_anchor(&g_tl2, 1, T0, 0), 0);
        for (k = 0; k < 90; k++) {
            int64_t on = T0 + k * (P60 / 2);
            (void)eval_at(&g_tl, on, P60 / 2, k, NULL, 0);
            (void)eval_at(&g_tl2, on, P60 / 2, k, NULL, 0);
            if (ytl_value(&g_tl, 1) != ytl_value(&g_tl2, 1)) {
                if (!bad)
                    fprintf(stderr, "  seq vs tween trial %d frame %d: %.9g against %.9g (ease %d cycles %d yoyo %d kv %d)\n",
                            trial, k, (double)ytl_value(&g_tl, 1), (double)ytl_value(&g_tl2, 1), d.ease,
                            d.cycles, (int)d.yoyo, (int)d.keep_velocity);
                bad++;
                break;
            }
        }
    }
    CHECK_I(bad, 0);
    CHECK_I(bad_n, 0);
}

/* The arena under clears: a clear takes its blocks' room back, and the
 * blocks of other bases still sample right after they move. */
static void test_seq_arena(void) {
    ytl_tween_desc d = sq_tw(4.0f, YRT_MS(200), YTL_EASE_COSINE);
    ytl_seq q;
    float want;
    int i;
    CHECK(seq_open(&g_tl, g_store, 16, g_ka, 12, 6, NULL));
    q = ytl_seq_on(&g_tl, 1);
    ytl_then(&q, 0, &d);
    ytl_then(&q, 0, &d);          /* 1 + 4 slots, grown in place */
    CHECK_I(q.err, 0);
    q = ytl_seq_on(&g_tl, 2);
    ytl_then(&q, 1, &d);          /* 1 + 2 */
    ytl_then(&q, 2, &d);          /* 1 + 2: 11 of 12 */
    CHECK_I(q.err, 0);
    ytl_then(&q, 1, &d);          /* needs a copy of 5: does not fit */
    CHECK_I(q.err, YTL_ERR_FULL);
    CHECK_I(ytl_clear(&g_tl, 1), 0);
    q = ytl_seq_on(&g_tl, 2);
    ytl_at(&q, YRT_MS(400));
    ytl_then(&q, 1, &d);          /* a new block of this sequence: 3 */
    ytl_then(&q, 1, &d);          /* grows at the top: 2 more */
    CHECK_I(q.err, 0);
    CHECK_I(ytl_anchor(&g_tl, 2, T0, 0), 0);
    for (i = 0; i < 60; i++) {
        int64_t bt = i * P60;
        (void)eval_at(&g_tl, T0 + bt, P60, i, NULL, 0);
        /* ch 2's tween runs from 200 to 400 ms (after ytl_then on ch 1) */
        want = (float)(bt < YRT_MS(200) ? 0.0
                       : bt < YRT_MS(400) ? 4.0 * (0.5 - 0.5 * cos(3.14159265358979323846 * (double)(bt - YRT_MS(200))
                                                                      / (double)YRT_MS(200)))
                                            : 4.0);
        CHECK_F(ytl_value(&g_tl, 2), want, 1e-5);
    }
}

/* ytl_on_n: staggered onsets, all or none. */
static void test_seq_on_n(void) {
    static const int dots[4] = { 0, 1, 2, 3 };
    static const int with_bound[4] = { 0, 1, 5, 2 };
    static ytl_key k1[1];
    ytl_seq q;
    const ytl_event* e;
    int n, i;
    k1[0] = mkkey(0, 1.0f, 0);
    CHECK(seq_open(&g_tl, g_store, 8, g_ka, 16, 6, NULL));
    CHECK_I(ytl_set_keys(&g_tl, 5, 1, k1, 1), 0);
    q = ytl_seq_on(&g_tl, 1);
    ytl_at(&q, YRT_MS(100));
    ytl_on_n(&q, with_bound, 4, YRT_MS(20));
    CHECK_I(q.err, YTL_ERR_BOUND);
    CHECK_I(n_events(&g_tl, 1), 0);                 /* none of the four */
    q = ytl_seq_on(&g_tl, 1);
    ytl_at(&q, YRT_MS(100));
    ytl_on_n(&q, dots, 4, YRT_MS(20));
    CHECK_I(q.err, 0);
    CHECK_I(q.t, YRT_MS(100));                    /* the cursor stays */
    e = ytl_events(&g_tl, 1, &n);
    CHECK_I(n, 4);
    for (i = 0; i < n && i < 4; i++) {
        CHECK_I(e[i].time_ns, YRT_MS(100) + i * YRT_MS(20));
        CHECK_I(e[i].target, dots[i]);
        CHECK_I(e[i].kind, YTL_ONSET);
    }
    ytl_on_n(&q, dots, 4, YRT_MS(20));          /* 4 more do not fit in 8 - 4 = 4? they do */
    CHECK_I(q.err, 0);
    ytl_on_n(&q, dots, 1, 0);                     /* the ninth does not */
    CHECK_I(q.err, YTL_ERR_FULL);
    CHECK_I(n_events(&g_tl, 1), 8);
}

/* A sequence's tween on a channel tracked on another base stops that track
 * at its value, as ytl_tween() does, and takes over from that value. */
static void test_seq_cross_base(void) {
    static ytl_key mv[2];
    ytl_tween_desc d = sq_tw(5.0f, YRT_MS(100), 0);
    ytl_seq q;
    int k, bad = 0;
    mv[0] = mkkey(0, 0.0f, 0);
    mv[1] = mkkey(S_NS, 10.0f, 0);
    CHECK(seq_open(&g_tl, g_store, 8, g_ka, 64, 2, NULL));
    CHECK(seq_open(&g_tl2, g_store2, 8, g_kb, 64, 2, NULL));
    for (k = 0; k < 2; k++) {
        ytl_timeline* t = k ? &g_tl2 : &g_tl;
        CHECK_I(ytl_set_keys(t, 0, 2, mv, 2), 0);
        CHECK_I(ytl_anchor(t, 2, T0, 0), 0);
        CHECK_I(ytl_anchor(t, 1, T0, -YRT_MS(300)), 0);
        (void)eval_at(t, T0 + YRT_MS(200), P60, 0, NULL, 0);   /* base 2 at 200 ms: 2.0 */
    }
    CHECK_F(ytl_value(&g_tl, 0), 2.0, 1e-6);
    q = ytl_seq_on(&g_tl, 1);
    ytl_to(&q, 0, &d);                            /* base 1 time 0, 100 ms of RT later */
    d.start_ns = 0;
    d.start_set = true;
    CHECK_I(ytl_tween(&g_tl2, 0, 1, &d), 0);
    for (k = 1; k < 40; k++) {
        int64_t on = T0 + YRT_MS(200) + k * P60;
        (void)eval_at(&g_tl, on, P60, k, NULL, 0);
        (void)eval_at(&g_tl2, on, P60, k, NULL, 0);
        if (ytl_value(&g_tl, 0) != ytl_value(&g_tl2, 0)) bad++;
    }
    CHECK_I(bad, 0);
    CHECK_F(ytl_value(&g_tl, 0), 5.0, 0.0);
}

/* The random model: tables of random ops, with errors among them, built
 * three ways: by the builder calls, by ytl_run, and by hand with
 * ytl_add and ytl_set_track of keys that this test lowers on its own
 * from the manual's rules. The channels are idle before the table, so the
 * takeover's value is the channel's initial value, which a hand-built
 * keyed track holds before its first key with a STEP key. */
enum { SM_NCH = 6, SM_OPS = 16, SM_KEYS = 256 };
static ytl_op sm_ops[SM_OPS];
static ytl_key sm_keys[SM_NCH][SM_KEYS];
static ytl_curve sm_curves[SM_NCH][2 * SM_OPS];

/* A yoyo's return leg by hand (v0.7.0): the reverse ease 1 - e(1 - u), and
 * for BEZIER the curve turned half a turn about (1/2, 1/2), in float. */
static int sm_rev_ease(int e) {
    return e == YTL_EASE_QUAD_IN ? YTL_EASE_QUAD_OUT : e == YTL_EASE_QUAD_OUT ? YTL_EASE_QUAD_IN
         : e == YTL_EASE_CUBIC_IN ? YTL_EASE_CUBIC_OUT : e == YTL_EASE_CUBIC_OUT ? YTL_EASE_CUBIC_IN : e;
}
static ytl_key g_kh[SQ_ARENA];
static long sm_stat_err[8], sm_stat_ok, sm_stat_tw, sm_stat_ev;

static void sm_gen(int n) {
    int i;
    for (i = 0; i < n; i++) {
        ytl_op* o = &sm_ops[i];
        int r = rnd_int(100), tw;
        memset(o, 0, sizeof *o);
        if (r < 12) o->op = YTL_OP_ON;
        else if (r < 18) o->op = YTL_OP_OFF;
        else if (r < 24) o->op = YTL_OP_SET_VALUE;
        else if (r < 28) o->op = YTL_OP_TRIGGER;
        else if (r < 31) o->op = YTL_OP_MARK;
        else if (r < 45) o->op = YTL_OP_WAIT;
        else if (r < 49) o->op = YTL_OP_AT;
        else if (r < 75) o->op = YTL_OP_TO;
        else o->op = YTL_OP_THEN;
        tw = o->op == YTL_OP_TO || o->op == YTL_OP_THEN;
        /* Events mostly on 0..2 and tweens on 3..5, so BOUND does not
         * dominate; sometimes any channel. */
        o->ch = rnd_int(10) == 0 ? rnd_int(SM_NCH) : (tw ? 3 : 0) + rnd_int(3);
        o->value = (float)rnd_int(32) / 4.0f;
        o->code = (int32_t)rnd_range(-1000, 1000);
        o->user = sm64();
        /* v0.8.0: a trigger's ch is its trigger channel; sometimes a
         * negative one, from the code so the random stream stays. */
        if (o->op == YTL_OP_TRIGGER && o->code % 29 == 0) o->ch = -1 - o->ch;
        if (o->op == YTL_OP_WAIT) o->t_ns = rnd_range(0, YRT_MS(200));
        if (o->op == YTL_OP_AT) o->t_ns = rnd_range(0, YRT_MS(1500));
        if (tw) {
            static const int eases[6] = { 0, 1, 2, 3, 4, 6 };
            ytl_tween_desc* d = &o->tween;
            d->to = (float)rnd_int(64) / 8.0f - 3.0f;
            d->duration_ns = rnd_int(6) == 0 ? 0 : rnd_range(1, YRT_MS(300));
            d->ease = eases[rnd_int(6)];
            if (d->ease == YTL_EASE_BEZIER)
                d->curve = rnd_int(2) ? mkcurve(0.42f, 0.0f, 0.58f, 1.0f) : mkcurve(0.7f, -0.5f, 0.3f, 1.5f);
            d->from_set = rnd_int(3) == 0;
            d->from = (float)rnd_int(64) / 8.0f;
            d->cycles = rnd_int(4);
            d->yoyo = rnd_int(4) == 0;
            if (d->duration_ns == 0 && rnd_int(4) != 0) {
                d->cycles = 0;
                d->yoyo = false;
            }
            d->delay_ns = rnd_int(3) == 0 ? rnd_range(-YRT_MS(50), YRT_MS(100)) : 0;
        }
        /* errors */
        r = rnd_int(100);
        if (r < 2) o->op = 0;
        else if (r < 4) o->ch = SM_NCH;
        else if (r < 6 && tw) o->tween.start_set = true;
        else if (r < 8 && tw) o->tween.cycles = YTL_FOREVER;
    }
}

static int sm_model_op(ytl_timeline* h, const float* init, const ytl_op* o, int* kind, int* nk, int* nc,
                       int64_t* end, int64_t* tp);

/* The model: applies the ops to h by hand up to the first error, whose
 * code it returns with its index in *bad. */
static int sm_model(ytl_timeline* h, const float* init, int n, int* bad) {
    int kind[SM_NCH], nk[SM_NCH], nc[SM_NCH], i, ch, rc = 0;
    int64_t end[SM_NCH], t = 0;
    memset(kind, 0, sizeof kind);
    memset(nk, 0, sizeof nk);
    memset(nc, 0, sizeof nc);
    memset(end, 0, sizeof end);
    *bad = -1;
    for (i = 0; i < n && rc == 0; i++) {
        const ytl_op* o = &sm_ops[i];
        rc = sm_model_op(h, init, o, kind, nk, nc, end, &t);
        if (rc < 0) *bad = i;
    }
    /* What came before the error stays, as the builder leaves it. */
    for (ch = 0; ch < SM_NCH; ch++) {
        ytl_track tr;
        if (kind[ch] != 2) continue;
        tr = trk_keys(sm_keys[ch], nk[ch]);
        tr.curves = sm_curves[ch];
        tr.n_curves = nc[ch];
        if (ytl_set_track(h, ch, 1, &tr) != 0) return -101;
    }
    return rc;
}

static int sm_model_op(ytl_timeline* h, const float* init, const ytl_op* o, int* kind, int* nk, int* nc,
                       int64_t* end, int64_t* tp) {
    int64_t t = *tp;
    int ch;
    switch (o->op) {
    case YTL_OP_WAIT: t += o->t_ns; break;
    case YTL_OP_AT: t = o->t_ns; break;
    case YTL_OP_ON: case YTL_OP_OFF: case YTL_OP_SET_VALUE:
    case YTL_OP_TRIGGER: case YTL_OP_MARK: {
        ytl_event e;
        bool drives = o->op == YTL_OP_ON || o->op == YTL_OP_OFF || o->op == YTL_OP_SET_VALUE;
        if (drives && (o->ch < 0 || o->ch >= SM_NCH)) return YTL_ERR_ARG;
        if (o->op == YTL_OP_TRIGGER && o->ch < 0) return YTL_ERR_ARG;
        if (drives && kind[o->ch] == 2) return YTL_ERR_BOUND;
        memset(&e, 0, sizeof e);
        e.time_ns = t;
        e.base = 1;
        e.kind = (uint8_t)(o->op == YTL_OP_ON ? YTL_ONSET : o->op == YTL_OP_OFF ? YTL_OFFSET
                           : o->op == YTL_OP_SET_VALUE ? YTL_SET
                           : o->op == YTL_OP_TRIGGER ? YTL_TRIGGER : YTL_MARK);
        if (drives || o->op == YTL_OP_TRIGGER) e.target = o->ch;
        if (o->op == YTL_OP_SET_VALUE) e.value = o->value;
        if (o->op == YTL_OP_TRIGGER || o->op == YTL_OP_MARK) e.code = o->code;
        if (o->op == YTL_OP_MARK) e.user = o->user;
        if (ytl_add(h, &e) < 0) return -100;
        if (drives) kind[o->ch] = 1;
        sm_stat_ev++;
        break;
    }
    case YTL_OP_TO: case YTL_OP_THEN: {
        const ytl_tween_desc* d = &o->tween;
        int64_t s, cyc, cn, j;
        ytl_key* k;
        float from;
        int ci = 0, per;
        if (o->ch < 0 || o->ch >= SM_NCH) return YTL_ERR_ARG;
        ch = o->ch;
        if (kind[ch] == 1) return YTL_ERR_BOUND;
        if (d->start_set || d->cycles == YTL_FOREVER) return YTL_ERR_ARG;
        if ((d->cycles > 1 || d->yoyo) && d->duration_ns == 0) return YTL_ERR_ARG;
        s = t + d->delay_ns;
        if (kind[ch] == 2 && s < end[ch]) return YTL_ERR_ARG;
        k = sm_keys[ch];
        if (kind[ch] != 2) {
            k[0] = mkkey(s, init[ch], YTL_EASE_STEP);
            nk[ch] = 1;
            from = init[ch];
        } else {
            k[nk[ch] - 1].ease = YTL_EASE_STEP;
            from = k[nk[ch] - 1].value;
        }
        if (d->from_set) from = d->from;
        if (d->ease == YTL_EASE_BEZIER) {
            sm_curves[ch][nc[ch]] = d->curve;
            ci = nc[ch]++;
            if (d->yoyo) {
                ytl_curve* m = &sm_curves[ch][nc[ch]++];
                m->x1 = 1.0f - d->curve.x2;
                m->y1 = 1.0f - d->curve.y2;
                m->x2 = 1.0f - d->curve.x1;
                m->y2 = 1.0f - d->curve.y1;
            }
        }
        cn = d->cycles > 1 ? d->cycles : 1;
        cyc = d->duration_ns * (d->yoyo ? 2 : 1);
        per = d->yoyo ? 3 : 2;
        for (j = 0; j < cn; j++) {
            ytl_key* a = &k[nk[ch]];
            a[0] = mkkey(s + j * cyc, from, d->ease);
            a[1] = mkkey(s + j * cyc + d->duration_ns, d->to, d->yoyo ? sm_rev_ease(d->ease) : d->ease);
            a[0].curve = a[1].curve = (uint16_t)ci;
            if (d->yoyo && d->ease == YTL_EASE_BEZIER) a[1].curve = (uint16_t)(ci + 1);
            if (d->yoyo) {
                a[2] = mkkey(s + j * cyc + 2 * d->duration_ns, from, d->ease);
                a[2].curve = (uint16_t)ci;
            }
            nk[ch] += per;
        }
        end[ch] = s + cn * cyc;
        kind[ch] = 2;
        sm_stat_tw++;
        if (o->op == YTL_OP_THEN) t = end[ch];
        break;
    }
    default:
        return YTL_ERR_ARG;
    }
    *tp = t;
    return 0;
}

static void sm_builder(ytl_seq* q, int n) {
    int i;
    for (i = 0; i < n; i++) {
        const ytl_op* o = &sm_ops[i];
        switch (o->op) {
        case YTL_OP_ON: ytl_on(q, o->ch); break;
        case YTL_OP_OFF: ytl_off(q, o->ch); break;
        case YTL_OP_SET_VALUE: ytl_set_value(q, o->ch, o->value); break;
        case YTL_OP_TRIGGER:
            if (o->ch == 0) ytl_trigger(q, o->code);
            else ytl_trigger_ch(q, o->ch, o->code);
            break;
        case YTL_OP_MARK: ytl_mark(q, o->code, o->user); break;
        case YTL_OP_WAIT: ytl_wait(q, o->t_ns); break;
        case YTL_OP_AT: ytl_at(q, o->t_ns); break;
        case YTL_OP_TO: (void)ytl_to(q, o->ch, &o->tween); break;
        case YTL_OP_THEN: ytl_then(q, o->ch, &o->tween); break;
        default:
            /* A bad op has no builder call; the table form reports it. */
            if (q->err == 0) (void)ytl_run(q, o, 1);
            return;
        }
    }
}

static void test_seq_model(void) {
    static ytl_timeline snap;
    int trial, bad_vals = 0, bad_store = 0, bad_err = 0, bad_run = 0;
    float init[SM_NCH];
    for (trial = 0; trial < 600; trial++) {
        int n, want, wbad, rc, bad, ch, k;
        ytl_seq qb, qr;
        g_rng = 9100 + (uint64_t)trial;
        n = 3 + rnd_int(SM_OPS - 2);
        for (ch = 0; ch < SM_NCH; ch++) init[ch] = (float)rnd_int(16) / 4.0f - 1.0f;
        sm_gen(n);
        CHECK(seq_open(&g_tl, g_store, 64, g_ka, SQ_ARENA, SM_NCH, init));    /* builder */
        CHECK(seq_open(&g_tl2, g_store2, 64, g_kb, SQ_ARENA, SM_NCH, init));  /* run */
        CHECK(seq_open(&g_tl3, g_store3, 64, g_kh, SQ_ARENA, SM_NCH, init));  /* by hand */
        want = sm_model(&g_tl3, init, n, &wbad);
        if (want < -99) {
            bad_err++;
            continue;
        }
        sm_stat_err[-want]++;
        if (want == 0) sm_stat_ok++;
        rc = ytl_check_ops(&g_tl2, 1, sm_ops, n, &bad);
        if (rc != want || bad != wbad) {
            if (!bad_err) fprintf(stderr, "  seq model trial %d: check_ops %d at %d, model %d at %d\n", trial, rc, bad, want, wbad);
            bad_err++;
        }
        qr = ytl_seq_on(&g_tl2, 1);
        memcpy(&snap, &g_tl2, sizeof snap);
        rc = ytl_run(&qr, sm_ops, n);
        if (rc != want || (want != 0 && (qr.err_call != wbad || memcmp(&snap, &g_tl2, TL_STATE) != 0))) bad_run++;
        qb = ytl_seq_on(&g_tl, 1);
        sm_builder(&qb, n);
        if (qb.err != want || (want != 0 && sm_ops[wbad].op != 0 && qb.err_call != wbad)) {
            if (!bad_err)
                fprintf(stderr, "  seq model trial %d: builder %d at %d, model %d at %d\n", trial, qb.err, qb.err_call, want, wbad);
            bad_err++;
        }
        CHECK_I(ytl_anchor(&g_tl, 1, T0, 0), 0);
        CHECK_I(ytl_anchor(&g_tl2, 1, T0, 0), 0);
        CHECK_I(ytl_anchor(&g_tl3, 1, T0, 0), 0);
        for (k = 0; k < 160; k++) {
            int64_t on = T0 + k * P60;
            (void)eval_at(&g_tl, on, P60, k, NULL, 0);
            (void)eval_at(&g_tl2, on, P60, k, NULL, 0);
            (void)eval_at(&g_tl3, on, P60, k, NULL, 0);
            for (ch = 0; ch < SM_NCH; ch++) {
                float h = ytl_value(&g_tl3, ch);
                if (ytl_value(&g_tl, ch) != h || (want == 0 && ytl_value(&g_tl2, ch) != h)) {
                    if (!bad_vals)
                        fprintf(stderr, "  seq model trial %d frame %d ch %d: builder %.9g run %.9g hand %.9g\n", trial, k,
                                ch, (double)ytl_value(&g_tl, ch), (double)ytl_value(&g_tl2, ch), (double)h);
                    bad_vals++;
                    k = 1000;
                    break;
                }
            }
        }
        if (memcmp(g_store, g_store3, 64 * sizeof g_store[0]) != 0) bad_store++;
        if (want == 0 && memcmp(g_store2, g_store3, 64 * sizeof g_store[0]) != 0) bad_store++;
    }
    CHECK_I(bad_vals, 0);
    CHECK_I(bad_store, 0);
    CHECK_I(bad_err, 0);
    CHECK_I(bad_run, 0);
    if (sm_stat_ok < 150 || sm_stat_err[-YTL_ERR_ARG] < 100 || sm_stat_err[-YTL_ERR_BOUND] < 30 || sm_stat_tw < 1000)
        fail_i(__LINE__, "seq model: too few clean tables, ARG, BOUND or tweens (vacuous)", sm_stat_ok, 150);
    printf("timeline_test: seq model: %ld clean tables, %ld ARG, %ld BOUND, %ld tweens, %ld events\n",
           sm_stat_ok, sm_stat_err[-YTL_ERR_ARG], sm_stat_err[-YTL_ERR_BOUND], sm_stat_tw, sm_stat_ev);
}

/* -------------------------------------------- 12. the randomized model */

enum { M_CAP = 48, M_NCH = 8, M_EVCH = 6 };

/* Passed by the base: fired, or skipped (SKIP). */
#define M_DONE (YTL_EV_FIRED | YTL_EV_SKIPPED)

typedef struct mev {
    ytl_event e;
    int late;    /* added at or before its base's reach             */
    int ambig;   /* LATE bit not checked: see the note in m_rewind() */
    int64_t key; /* the report's order when it fired: the RT time its base
                  * reached it (EVALUATE), onset - residual when paused */
} mev;

typedef struct mbase {
    int state;          /* 0 stopped, 1 running, 2 paused */
    int64_t art, abt;   /* running: base time abt at RT art */
    int64_t frozen;     /* paused */
    int64_t last_hi;    /* the reach: furthest window end since the last rewind */
    int64_t last_on;    /* furthest base time of an evaluated onset, same span */
    int evaluated;
    /* A tween's "now" (TWEENS): the base time of the last evaluated onset,
     * the last anchor's base time, and how many anchors, pauses and
     * resumes since the last evaluate of the base. */
    int64_t ev_bt, anc_bt;
    int ev_any, moved;
    int64_t nowv;       /* "now" as TWEENS defines it: set by every evaluate,
                         * anchor, pause and resume of the base, and by a
                         * rate change of a running base */
    int64_t num, den;   /* RATE, reduced */
    int chg;            /* a rate change since the last evaluate (a statistic) */
} mbase;

static mev m_ev[M_CAP + 8];
static int m_n, m_next_id;
static mbase m_b[YTL_MAX_BASES];
static float m_init[M_NCH], m_trackv[M_NCH];
static int64_t m_onset, m_index, m_period, m_w, m_pe;
static int m_has_onset;
static ytl_key m_k6[6], m_k7[5];
/* Channels 6 and 7: the driver each has (none after a tween on another
 * base stopped it), its base, and a tween waiting for its start, resolved
 * (m_ps the absolute start, m_pcall the value at the call, m_pstep a STEP
 * tween, promoted at the window's end). set_track and tween replace them;
 * nothing removes them, so they stay BOUND. */
static rdrv m_drv[M_NCH];
static int m_has_drv[M_NCH], m_tb[M_NCH], m_pend[M_NCH], m_pstep[M_NCH];
static ytl_tween_desc m_pd[M_NCH];
static int64_t m_ps[M_NCH];
static float m_pcall[M_NCH];
static ytl_key m_krk[M_NCH][3];
static float m_samp[64];
static const ytl_curve m_curv[2] = { { 0.42f, 0.0f, 0.58f, 1.0f }, { 0.68f, -0.6f, 0.32f, 1.6f } };
static mev m_fired[M_CAP + 8];
static int m_nf;
static uint64_t m_seed;
static uint64_t m_trace_seed;   /* TL_TRACE */
static int m_step;
/* What the randomized check exercised, printed on a pass so that a
 * vacuous run shows. */
static long m_stat_eval, m_stat_fired, m_stat_late, m_stat_arew, m_stat_wrew, m_stat_unfired, m_stat_err, m_stat_wait, m_stat_trk;
static long m_stat_tw, m_stat_twrej, m_stat_promo, m_stat_kv, m_stat_xbase, m_stat_twnow;
static long m_stat_skip, m_stat_skipped, m_stat_skipback, m_stat_skiplate, m_stat_peek, m_stat_peeked, m_stat_peektrunc;
static long m_stat_rate, m_stat_rate_run, m_stat_rate_behind, m_stat_rate_fired, m_stat_win, m_stat_rtt;
static const char* m_op;

static int m_fail(const char* what, long long got, long long want) {
    fprintf(stderr, "timeline_test: FAIL model seed %llu step %d op %s: %s (got %lld, want %lld)\n",
            (unsigned long long)m_seed, m_step, m_op, what, got, want);
    g_failures++;
    return 1;
}

static void print_ev(const char* tag, const ytl_event* e) {
    fprintf(stderr, "  %s: id %d base %d time %lld kind %d target %d value %g flags %u "
            "frame %lld onset %lld residual %lld\n", tag, e->id, e->base, (long long)e->time_ns,
            e->kind, e->target, (double)e->value, (unsigned)e->flags, (long long)e->frame,
            (long long)e->onset, (long long)e->residual);
}

static int m_scaled(int b) { return b != 0 && m_b[b].num != m_b[b].den; }

/* RATE: base time anchor_bt + floor((rt - anchor_rt) num / den), from the
 * exact reference; at 1/1 it is v0.3.0's sum. */
static int64_t m_bt(int b, int64_t rt) {
    const mbase* mb = &m_b[b];
    if (b == 0) return rt;
    if (mb->state == 2) return mb->frozen;
    if (!m_scaled(b)) return mb->abt + (rt - mb->art);
    if (g_mut & MUT_RATE_TRUNC) {
        int big;
        int64_t d = rt - mb->art;
        return mb->abt + (d < 0 ? -ref_floor_md(-d, mb->num, mb->den, &big) : ref_floor_md(d, mb->num, mb->den, &big));
    }
    return ref_bt(mb->art, mb->abt, mb->num, mb->den, rt);
}

/* The RT time a running base reaches base time T: ceil, so that it is the
 * first RT ns with m_bt >= T. */
static int64_t m_rt(int b, int64_t T) {
    const mbase* mb = &m_b[b];
    if (b == 0) return T;
    if (!m_scaled(b)) return mb->art + (T - mb->abt);
    if (g_mut & MUT_RATE_INVFLOOR) {
        int big;
        return mb->art + ref_floor_md(T - mb->abt, mb->den, mb->num, &big);
    }
    return ref_rt(mb->art, mb->abt, mb->num, mb->den, T);
}

/* A tween's "now" on base b (TWEENS): the base time of the last evaluated
 * onset, unless an anchor, pause or resume came after it, which set now
 * to the anchor's base time or the frozen time; 0 on a base never
 * evaluated or anchored. Base 0 is never anchored. *ok is always 1: the
 * rule has no reading left open. */
static int64_t m_now(int b, int* ok) {
    *ok = 1;
    if (b == 0) return m_has_onset ? m_onset : 0;
    return m_b[b].nowv;
}

static int is_chan_kind(int kind) {
    return kind == YTL_ONSET || kind == YTL_OFFSET || kind == YTL_SET;
}

/* -2 keyed, -1 free, else the base of its events. */
static int m_bind(int ch) {
    int i;
    if (ch == 6 || ch == 7) return -2;
    for (i = 0; i < m_n; i++)
        if (is_chan_kind(m_ev[i].e.kind) && m_ev[i].e.target == ch) return m_ev[i].e.base;
    return -1;
}

static float m_chan_value(int ch) {
    int i, best = -1;
    for (i = 0; i < m_n; i++) {
        const ytl_event* e = &m_ev[i].e;
        if (!is_chan_kind(e->kind) || e->target != ch || !(e->flags & ((g_mut & MUT_SKIP_NOCHAN) ? YTL_EV_FIRED : M_DONE))) continue;
        if (best < 0 || e->time_ns > m_ev[best].e.time_ns ||
            (e->time_ns == m_ev[best].e.time_ns && e->id > m_ev[best].e.id)) best = i;
    }
    if (best < 0) return m_init[ch];
    if (m_ev[best].e.kind == YTL_ONSET) return 1.0f;
    if (m_ev[best].e.kind == YTL_OFFSET) return 0.0f;
    return m_ev[best].e.value;
}

static void m_unfire(mev* m) {
    m->e.flags = 0;
    m->e.frame = -1;
    m->e.onset = 0;
    m->e.residual = 0;
    m->late = 0;
}

/* A rewinding anchor of base b at bt: fired events with time >= bt are
 * pending again. A pending event that was added late and is now past the
 * reach: the manual fixes lateness at add and sets the flag at fire, but
 * does not say whether a rewind past it clears it, so its LATE bit is not
 * compared afterwards. */
static void m_rewind(int b, int64_t bt) {
    int i;
    for (i = 0; i < m_n; i++) {
        mev* m = &m_ev[i];
        if (m->e.base != b || m->e.time_ns < bt) continue;
        if (m->e.flags & ((g_mut & MUT_SKIP_UNREW) ? YTL_EV_FIRED : M_DONE)) { m_unfire(m); m_stat_unfired++; }
        else if (m->late) m->ambig = 1;
    }
}

static int fired_cmp(const void* a, const void* b) {
    const mev* x = (const mev*)a;
    const mev* y = (const mev*)b;
    if (x->key != y->key) return x->key < y->key ? -1 : 1;
    if (x->e.id != y->e.id) return x->e.id < y->e.id ? -1 : 1;
    return 0;
}

static int store_cmp(const void* a, const void* b) {
    return key_cmp(&((const mev*)a)->e, &((const mev*)b)->e);
}

static void m_evaluate(int64_t onset, int64_t index) {
    int b, i, ch;
    m_nf = 0;
    for (b = 0; b < YTL_MAX_BASES; b++) {
        int64_t hi, bt_on;
        if (b != 0 && m_b[b].state == 0) continue;
        bt_on = m_bt(b, onset);
        if (m_b[b].state == 2 && b != 0) hi = m_b[b].frozen;
        else if ((g_mut & MUT_RATE_LEAD) && m_scaled(b)) hi = bt_on + m_w;
        else hi = m_bt(b, onset + m_w);
        /* An evaluate never un-fires: a window behind the reach fires
         * only what it reaches; later events wait. */
        if (m_b[b].evaluated && hi < m_b[b].last_hi) {
            m_stat_wrew++;
            if (m_b[b].chg) m_stat_rate_behind++;
        }
        m_b[b].chg = 0;
        for (i = 0; i < m_n; i++) {
            mev* m = &m_ev[i];
            if (m->e.base != b || (m->e.flags & M_DONE) || m->e.time_ns > hi) continue;
            m->e.flags = (uint16_t)(YTL_EV_FIRED | (m->late ? YTL_EV_LATE : 0u));
            m->e.frame = index;
            m->e.onset = onset;
            m->e.residual = bt_on - m->e.time_ns;
            m->key = m_scaled(b) && m_b[b].state == 1 && !(g_mut & MUT_RATE_ORDER)
                     ? m_rt(b, m->e.time_ns) : onset - m->e.residual;
            m_fired[m_nf++] = *m;
            m_stat_fired++;
            if (m_scaled(b)) m_stat_rate_fired++;
            if (m->late) m_stat_late++;
        }
        for (i = 0; i < m_n; i++)
            if (m_ev[i].e.base == b && !(m_ev[i].e.flags & M_DONE) && m_ev[i].late) m_stat_wait++;
        if (!m_b[b].evaluated || hi > m_b[b].last_hi) m_b[b].last_hi = hi;
        if (!m_b[b].evaluated || bt_on > m_b[b].last_on) m_b[b].last_on = bt_on;
        m_b[b].evaluated = 1;
        m_b[b].ev_bt = bt_on;
        m_b[b].nowv = bt_on;
        m_b[b].ev_any = 1;
        m_b[b].moved = 0;
        /* A waiting tween takes over once its read time reaches its start
         * (the window's end for a STEP tween, else the onset), from the
         * old driver's value at the start. Then a STEP track at the
         * window's end, any other at the onset. */
        for (ch = 6; ch < M_NCH; ch++) {
            if (m_tb[ch] != b) continue;
            if (m_pend[ch] && (m_pstep[ch] ? hi : bt_on) >= m_ps[ch]) {
                rdrv nd;
                rdrv_tween(&nd, &m_pd[ch], m_ps[ch], m_has_drv[ch] ? &m_drv[ch] : NULL, (double)m_trackv[ch],
                           (double)m_pcall[ch]);
                if (m_trace_seed == m_seed)
                    fprintf(stderr, "  [%d] takeover ch%d b%d at %lld (onset bt %lld) old %s %.9g -> from %.9g\n", m_step,
                            ch, b, (long long)m_ps[ch], (long long)bt_on, m_has_drv[ch] ? "track" : "held",
                            m_has_drv[ch] ? rdrv_value(&m_drv[ch], m_ps[ch]) : (double)m_trackv[ch],
                            nd.herm ? nd.hf : (double)nd.k[0].value);
                m_drv[ch] = nd;
                m_has_drv[ch] = 1;
                m_pend[ch] = 0;
                m_stat_promo++;
            }
            if (m_has_drv[ch])
                m_trackv[ch] = (float)rdrv_value(&m_drv[ch], rdrv_step(&m_drv[ch]) ? hi : bt_on);
        }
    }
    qsort(m_fired, (size_t)m_nf, sizeof m_fired[0], fired_cmp);
}

static const char* ev_diff(const ytl_event* g, const mev* m) {
    const ytl_event* w = &m->e;
    unsigned mask;
    if (g->id != w->id) return "id";
    if (g->time_ns != w->time_ns) return "time";
    if (g->base != w->base) return "base";
    if (g->kind != w->kind) return "kind";
    if (g->target != w->target) return "target";
    if (g->value != w->value) return "value";
    if (g->code != w->code) return "code";
    if (g->user != w->user) return "user";
    if (g->reserved_ != 0) return "reserved_";
    if (g->frame != w->frame) return "frame";
    if (g->onset != w->onset) return "onset";
    if (g->residual != w->residual) return "residual";
    /* Pending: flags 0, LATE included (set at fire). */
    mask = (w->flags & YTL_EV_FIRED) && m->ambig ? ~YTL_EV_LATE : 0xFFFFu;
    if ((g->flags & mask) != (w->flags & mask)) return "flags";
    return NULL;
}

static int m_check(void) {
    static mev sorted[M_CAP + 8];
    const ytl_event* p;
    const float* v;
    int n = -1, i, ch;
    p = ytl_events(&g_tl, YTL_ALL_BASES, &n);
    if (n != m_n) return m_fail("event count", n, m_n);
    memcpy(sorted, m_ev, (size_t)m_n * sizeof m_ev[0]);
    qsort(sorted, (size_t)m_n, sizeof sorted[0], store_cmp);
    for (i = 0; i < n; i++) {
        const char* d = ev_diff(&p[i], &sorted[i]);
        if (d) {
            print_ev("got ", &p[i]);
            print_ev("want", &sorted[i].e);
            return m_fail(d, i, i);
        }
    }
    v = ytl_values(&g_tl);
    if (!v) return m_fail("values NULL", 0, 1);
    for (ch = 0; ch < M_EVCH; ch++) {
        float want = m_chan_value(ch);
        if (v[ch] != want) {
            fprintf(stderr, "  channel %d got %g want %g\n", ch, (double)v[ch], (double)want);
            return m_fail("event channel value", ch, ch);
        }
    }
    for (ch = 6; ch < 8; ch++) {
        if (!(fabs((double)v[ch] - (double)m_trackv[ch]) <= 1e-5 * (1.0 + fabs((double)m_trackv[ch])))) {
            fprintf(stderr, "  channel %d got %.9g want %.9g\n", ch, (double)v[ch], (double)m_trackv[ch]);
            return m_fail("track channel value", ch, ch);
        }
    }
    return 0;
}

static int64_t m_rand_time(int b) {
    int64_t now = m_has_onset ? m_onset : T0, t;
    if (b != 0 && m_b[b].state == 0) t = rnd_range(-5 * m_pe, 30 * m_pe);
    else t = m_bt(b, now) + rnd_range(-4 * m_pe, 8 * m_pe);
    if (rnd_int(3) == 0) t -= t % m_pe;   /* ties on a coarse grid */
    /* RATE: a scaled base moves num/den base ns per RT ns, so a window
     * edge is near the base time of a coming window's end, not a multiple
     * of the period. Only on scaled bases: the v0.3.0 seeds draw the same. */
    if (m_scaled(b) && m_b[b].state != 0 && rnd_int(2) == 0)
        t = m_bt(b, now + (int64_t)rnd_int(6) * m_pe + m_w) + rnd_range(-1, 1);
    /* On or next to a window edge: the last one, or one a few frames on. */
    if (rnd_int(5) == 0 && m_b[b].evaluated)
        t = m_b[b].last_hi + (int64_t)rnd_int(4) * m_pe + rnd_range(-1, 1);
    /* At the base time of the last onset, where an anchor rewinds from. */
    else if (rnd_int(6) == 0 && m_b[b].evaluated)
        t = m_b[b].last_on + rnd_range(-1, 1);
    return t;
}

static ytl_event m_rand_event(void) {
    ytl_event e;
    int r = rnd_int(100), b = rnd_int(4);
    memset(&e, 0, sizeof e);
    e.base = (uint8_t)b;
    e.time_ns = m_rand_time(b);
    e.code = (int32_t)rnd_range(-100000, 100000);
    e.user = sm64();
    e.value = (float)rnd_int(400) / 8.0f - 10.0f;
    e.target = (int32_t)rnd_range(-1000, 1000);
    /* The header assigns these; the caller's garbage must not survive. */
    e.id = 4242;
    e.flags = (uint16_t)0xFFFFu;
    e.frame = 77;
    e.onset = 88;
    e.residual = 99;
    if (r < 15) e.kind = YTL_MARK;
    else if (r < 25) {
        /* v0.8.1 refuses a negative trigger channel; keep about 1 in 16
         * negative (from the drawn code, so the random stream is the same) */
        e.kind = YTL_TRIGGER;
        if (e.target < 0 && (e.code & 7) != 0) e.target = -e.target;
    }
    else if (r < 45) { e.kind = YTL_ONSET; e.target = rnd_int(M_EVCH); }
    else if (r < 60) { e.kind = YTL_OFFSET; e.target = rnd_int(M_EVCH); }
    else if (r < 85) { e.kind = YTL_SET; e.target = rnd_int(M_EVCH); }
    else if (r < 92) e.kind = (uint8_t)(16 + rnd_int(240));
    else if (r < 95) e.kind = (uint8_t)(5 + rnd_int(11));
    else if (r < 97) { e.kind = YTL_SET; e.target = rnd_int(M_EVCH); e.value = (float)make_nan(); }
    else if (r < 98) { e.kind = YTL_MARK; e.base = YTL_MAX_BASES; }
    else { e.kind = YTL_ONSET; e.target = 6 + rnd_int(2); }
    /* Mostly on the channel's own base, so BOUND does not dominate. */
    if (is_chan_kind(e.kind) && e.target >= 0 && e.target < M_EVCH && e.base < YTL_MAX_BASES &&
        m_bind(e.target) >= 0 && rnd_int(5) != 0) {
        e.base = (uint8_t)m_bind(e.target);
        e.time_ns = m_rand_time(e.base);
    }
    return e;
}

/* The expected code of an add of es[0..n), and how many error classes it
 * has; with more than one, any negative code is accepted. */
static int m_predict(const ytl_event* es, int n, int* classes) {
    int tmp[M_NCH], ch, i, arg = 0, bound = 0, full = 0;
    for (ch = 0; ch < M_NCH; ch++) tmp[ch] = m_bind(ch);
    for (i = 0; i < n; i++) {
        const ytl_event* e = &es[i];
        int bad = 0;
        if (e->base >= YTL_MAX_BASES || (e->kind >= 5 && e->kind <= 15)) { arg = 1; bad = 1; }
        if (e->kind == YTL_TRIGGER && e->target < 0) { arg = 1; bad = 1; }
        if (is_chan_kind(e->kind)) {
            if (e->target < 0 || e->target >= M_NCH) { arg = 1; continue; }
            if (e->kind == YTL_SET && !(fabs((double)e->value) <= 3.4e38)) { arg = 1; bad = 1; }
            if (tmp[e->target] == -2 || (tmp[e->target] >= 0 && tmp[e->target] != e->base)) bound = 1;
            else if (!bad) tmp[e->target] = e->base;
        }
    }
    if (m_n + n > M_CAP) full = 1;
    *classes = arg + bound + full;
    if (arg) return YTL_ERR_ARG;
    if (bound) return YTL_ERR_BOUND;
    if (full) return YTL_ERR_FULL;
    return 0;
}

/* TL_TRACE=<seed> prints the model's view of every step of that seed. */
static void m_trace(const char* op, int b, int64_t x, int64_t y, int ret) {
    if (m_trace_seed != m_seed) return;
    fprintf(stderr, "  [%d] %s b%d %lld %lld ret %d", m_step, op, b, (long long)x, (long long)y, ret);
    if (b >= 0 && b < YTL_MAX_BASES)
        fprintf(stderr, " | state %d reach %lld last_on %lld", m_b[b].state,
                (long long)m_b[b].last_hi, (long long)m_b[b].last_on);
    fputc('\n', stderr);
}

static void m_append(const ytl_event* es, int n) {
    int i;
    for (i = 0; i < n; i++) {
        mev* m = &m_ev[m_n++];
        const mbase* bb = &m_b[es[i].base];
        m->e = es[i];
        m->e.id = m_next_id++;
        m->e.flags = 0;
        m->e.frame = -1;
        m->e.onset = 0;
        m->e.residual = 0;
        m->late = bb->evaluated && es[i].time_ns <= bb->last_hi;
        m->ambig = 0;
        m_trace("added", es[i].base, m->e.id, es[i].time_ns, m->late);
    }
}

static int m_check_add(int ret, const ytl_event* es, int n) {
    int classes = 0, want = m_predict(es, n, &classes);
    if (want == 0) {
        if (ret != m_next_id) return m_fail("add return", ret, m_next_id);
        m_append(es, n);
    } else {
        if (ret >= 0) return m_fail("add accepted a bad batch", ret, want);
        m_stat_err++;
        if (classes == 1 && ret != want) return m_fail("add error code", ret, want);
    }
    return 0;
}

/* SKIP, from the manual: an anchor (with its rewind), then every pending
 * event of the base before bt is skipped (flags SKIPPED, frame -1, onset
 * rt, residual bt - time), and the reach and the furthest evaluated onset
 * move to just before bt when they were behind it. */
static int m_op_skip(int64_t now) {
    int b = rnd_int(12) == 0 ? (rnd_int(2) ? 0 : YTL_MAX_BASES) : 1 + rnd_int(3);
    int ok = b > 0 && b < YTL_MAX_BASES;
    int64_t rt = now + rnd_range(-2 * m_pe, 3 * m_pe), bt;
    int q = rnd_int(5), ret, want = 0, i, rewound = 0;
    m_op = "skip";
    if (q == 1 && ok && m_b[b].evaluated) {
        bt = m_b[b].last_on + rnd_range(-3, 3) * (m_pe / 3);            /* near the rewind threshold */
    } else if (q == 2 && ok && m_b[b].state != 0) {
        bt = m_bt(b, rt) + rnd_range(-10 * m_pe, 30 * m_pe);            /* mostly forward */
    } else if (q == 3 && ok && m_n > 0) {
        const mev* m = &m_ev[rnd_int(m_n)];                             /* on an event's time */
        bt = (m->e.base == b ? m->e.time_ns : rnd_range(0, 40 * m_pe)) + rnd_range(-1, 1);
    } else {
        bt = rnd_range(-5 * m_pe, 60 * m_pe);
    }
    ret = ytl_skip(&g_tl, b, rt, bt);
    if (!ok) {
        if (ret != YTL_ERR_ARG) return m_fail("skip bad base", ret, YTL_ERR_ARG);
        m_trace("skip", b, rt, bt, ret);
        return 0;
    }
    if (m_b[b].evaluated && bt <= m_b[b].last_on) {
        m_rewind(b, bt);
        m_b[b].last_hi = bt - 1;
        m_b[b].last_on = bt - 1;
        m_stat_arew++;
        m_stat_skipback++;
        rewound = 1;
    }
    m_b[b].state = 1;
    m_b[b].art = rt;
    m_b[b].abt = bt;
    m_b[b].anc_bt = bt;
    m_b[b].nowv = bt;
    m_b[b].moved++;
    if (g_mut & MUT_RATE_ANCHOR1) m_b[b].num = m_b[b].den = 1;
    if (!(g_mut & MUT_SKIP_FIRES)) {
        for (i = 0; i < m_n; i++) {
            mev* m = &m_ev[i];
            if (m->e.base != b || (m->e.flags & M_DONE)) continue;
            if (m->e.time_ns > bt || (m->e.time_ns == bt && !(g_mut & MUT_SKIP_AT))) continue;
            if (rewound && m->late && (g_mut & MUT_SKIP_LATEW)) continue;
            if (m->late) m_stat_skiplate++;
            m->e.flags = YTL_EV_SKIPPED;
            m->e.frame = -1;
            m->e.onset = rt;
            m->e.residual = bt - m->e.time_ns;
            m->late = 0;
            m->ambig = 0;
            want++;
        }
        if (!m_b[b].evaluated || (bt - 1 > m_b[b].last_hi && !(g_mut & MUT_SKIP_REACH))) m_b[b].last_hi = bt - 1;
        if (!m_b[b].evaluated || (bt - 1 > m_b[b].last_on && !(g_mut & MUT_SKIP_NOW))) m_b[b].last_on = bt - 1;
        m_b[b].evaluated = 1;
    }
    m_stat_skip++;
    m_stat_skipped += want;
    m_trace("skip", b, rt, bt, ret);
    if (ret != want) return m_fail("skip return", ret, want);
    return 0;
}

/* PEEK, from the manual: the pending events of running bases (base 0
 * always runs) with RT time in [from, to), by RT time then id, onset the
 * RT time, residual 0; and nothing in the handle or the storage changes. */
static int m_op_peek(int64_t now) {
    static ytl_timeline snap_tl;
    static ytl_event snap_store[M_CAP];
    static mev want[M_CAP + 8];
    int r = rnd_int(20), b, cap, ret, nw = 0, i, lim;
    int64_t from, to;
    ytl_event* outp;
    m_op = "peek";
    b = r == 0 ? (rnd_int(2) ? YTL_MAX_BASES : -2) : r < 8 ? YTL_ALL_BASES : rnd_int(4);
    from = now + rnd_range(-10 * m_pe, 10 * m_pe);
    to = from + (rnd_int(15) == 0 ? -rnd_range(1, m_pe) : rnd_range(0, 30 * m_pe));
    if (rnd_int(3) == 0 && m_n > 0) {
        /* An edge on an event's RT time, or a ns either side of it. */
        const mev* m = &m_ev[rnd_int(m_n)];
        int eb = m->e.base;
        if (eb == 0 || m_b[eb].state == 1) {
            int64_t rte = m_rt(eb, m->e.time_ns);
            if (rnd_int(2)) from = rte + rnd_range(-1, 1);
            else to = rte + rnd_range(-1, 1);
            if (to < from && rnd_int(4) != 0) { int64_t x = to; to = from; from = x; }
        }
    }
    cap = rnd_int(6);
    outp = (cap == 0 && rnd_int(2)) ? NULL : g_fired;
    for (i = 0; i < 16; i++) g_fired[i].id = -999;
    memcpy(&snap_tl, &g_tl, sizeof g_tl);
    memcpy(snap_store, g_store, sizeof snap_store);
    ret = ytl_peek(&g_tl, b, from, to, outp, cap);
    if (memcmp(&snap_tl, &g_tl, TL_STATE) != 0) return m_fail("peek changed the handle", 1, 0);
    if (memcmp(snap_store, g_store, sizeof snap_store) != 0) return m_fail("peek changed the storage", 1, 0);
    m_trace("peek", b, from, to, ret);
    if (b < YTL_ALL_BASES || b >= YTL_MAX_BASES || to < from) {
        if (ret != YTL_ERR_ARG) return m_fail("peek bad argument", ret, YTL_ERR_ARG);
        return 0;
    }
    for (i = 0; i < m_n; i++) {
        const mev* m = &m_ev[i];
        int eb = m->e.base;
        int64_t rte;
        if (b != YTL_ALL_BASES && eb != b) continue;
        if (eb != 0 && m_b[eb].state != 1) continue;
        if (m->e.flags & ((g_mut & MUT_PEEK_FIRED) ? YTL_EV_SKIPPED : M_DONE)) continue;
        rte = m_rt(eb, m->e.time_ns);
        if (rte < from || rte > to || (rte == to && !(g_mut & MUT_PEEK_END))) continue;
        want[nw] = *m;
        want[nw].e.onset = (g_mut & MUT_PEEK_ONSET) ? m->e.time_ns : rte;
        want[nw].e.residual = 0;
        want[nw].key = want[nw].e.onset;
        want[nw].ambig = 0;
        nw++;
    }
    qsort(want, (size_t)nw, sizeof want[0], fired_cmp);
    m_stat_peek++;
    m_stat_peeked += nw;
    if (nw > cap) m_stat_peektrunc++;
    if (ret != nw) return m_fail("peek return", ret, nw);
    lim = nw < cap ? nw : cap;
    for (i = 0; i < lim; i++) {
        const char* d = ev_diff(&g_fired[i], &want[i]);
        if (d) {
            print_ev("got ", &g_fired[i]);
            print_ev("want", &want[i].e);
            return m_fail(d, i, i);
        }
    }
    if (outp && cap < 16 && g_fired[cap].id != -999) return m_fail("peek wrote past cap", cap, cap);
    return 0;
}

/* RATE, from the manual: base 1 .. 7 only, num and den >= 1, stored
 * reduced; a running base re-anchors at (rt, its time at rt) with no rewind
 * and "now" there; a paused or stopped base only stores the rate; the rate
 * it has changes nothing. */
static int m_op_rate(int64_t now) {
    static const int32_t rn[16] = { 1, 1, 2, 1001, 999, 3, 7, 1, 25, 2, 1000, 2147483647, 0, 1, -1, 5 };
    static const int32_t rd[16] = { 1, 2, 1, 1000, 1000, 7, 3, 1000, 24, 4, 1000, 2147483646, 1, 0, 2, 5 };
    int b = rnd_int(12) == 0 ? (rnd_int(2) ? 0 : YTL_MAX_BASES) : 1 + rnd_int(3);
    int k = rnd_int(16), ret, want;
    int64_t num = rn[k], den = rd[k], rt = rnd_int(4) == 0 ? now : now + rnd_range(-3 * m_pe, 3 * m_pe);
    int32_t gn = -9, gd = -9;
    m_op = "rate";
    ret = ytl_rate(&g_tl, b, rt, rn[k], rd[k]);
    want = b < 1 || b >= YTL_MAX_BASES || num < 1 || den < 1 ? YTL_ERR_ARG : 0;
    m_trace("rate", b, num, den, ret);
    if (ret != want) return m_fail("rate return", ret, want);
    if (want != 0) return 0;
    {
        mbase* mb = &m_b[b];
        int64_t g = gcd64(num, den);
        num /= g;
        den /= g;
        if (num != mb->num || den != mb->den || ((g_mut & MUT_RATE_SAME) && mb->state == 1)) {
            if (mb->state == 1) {
                int64_t bt = m_bt(b, rt);
                if ((g_mut & MUT_RATE_REWIND) && mb->evaluated && bt <= mb->last_on) {
                    m_rewind(b, bt);
                    mb->last_hi = bt - 1;
                    mb->last_on = bt - 1;
                }
                mb->art = rt;
                mb->abt = bt;
                if (!(g_mut & MUT_RATE_NOW)) mb->nowv = bt;
                mb->moved++;
                mb->chg = 1;
                m_stat_rate_run++;
            }
            mb->num = num;
            mb->den = den;
        }
        m_stat_rate++;
        if (!ytl_get_rate(&g_tl, b, &gn, &gd)) return m_fail("get_rate", 0, 1);
        if (gn != mb->num || gd != mb->den) return m_fail("get_rate num", gn, mb->num);
    }
    return 0;
}

/* ytl_window(): the evaluate's window end, changing nothing. */
static int m_op_window(int64_t now) {
    static ytl_timeline snap_tl;
    int b = rnd_int(10) == 0 ? (rnd_int(2) ? -1 : YTL_MAX_BASES) : rnd_int(4);
    ytl_frame f;
    int64_t end = -77, want = 0;
    bool ok, wok;
    m_op = "window";
    f.onset = now + rnd_range(0, 2 * m_pe);
    f.period = m_period;
    f.index = m_index;
    memcpy(&snap_tl, &g_tl, sizeof g_tl);
    ok = ytl_window(&g_tl, b, &f, &end);
    if (memcmp(&snap_tl, &g_tl, sizeof g_tl) != 0) return m_fail("window changed the handle", 1, 0);
    wok = b >= 0 && b < YTL_MAX_BASES && (b == 0 || m_b[b].state != 0);
    if (wok) want = b != 0 && m_b[b].state == 2 ? m_b[b].frozen : m_bt(b, f.onset + m_w);
    if (ok != wok) return m_fail("window ok", ok, wok);
    if (ok && end != want) return m_fail("window end", end, want);
    m_stat_win++;
    return 0;
}

/* ytl_rt_time(): the first RT ns at which a running base reaches bt. */
static int m_op_rt_time(int64_t now) {
    int b = rnd_int(10) == 0 ? YTL_MAX_BASES : rnd_int(4);
    int64_t bt, x = -77;
    bool ok, wok;
    m_op = "rt_time";
    if (b < YTL_MAX_BASES && m_n > 0 && rnd_int(2)) bt = m_ev[rnd_int(m_n)].e.time_ns + rnd_range(-1, 1);
    else bt = (b < YTL_MAX_BASES && (b == 0 || m_b[b].state != 0) ? m_bt(b, now) : 0) + rnd_range(-5 * m_pe, 10 * m_pe);
    ok = ytl_rt_time(&g_tl, b, bt, &x);
    wok = b < YTL_MAX_BASES && (b == 0 || m_b[b].state == 1);
    if (ok != wok) return m_fail("rt_time ok", ok, wok);
    if (ok) {
        int64_t w = m_rt(b, bt);
        if (x != w) return m_fail("rt_time", x, w);
        /* The defining property, from the base time side. */
        if (m_bt(b, x) < bt || m_bt(b, x - 1) >= bt) return m_fail("rt_time is not the first RT ns", x, w);
    }
    m_stat_rtt++;
    return 0;
}

/* ops: also skip and peek (v0.3.0); 2: also rate, window and rt_time
 * (v0.4.0). Off, the run is v0.2.0's, op for op. */
static int model_run(uint64_t seed, int lead_num, int lead_den, int64_t period, int steps, int ops) {
    int ch, i;
    m_seed = seed;
    m_op = "open";
    m_step = -1;
    g_rng = seed;
    memset(m_b, 0, sizeof m_b);
    for (i = 0; i < YTL_MAX_BASES; i++) m_b[i].num = m_b[i].den = 1;
    m_b[0].state = 1;
    m_n = 0;
    m_next_id = 0;
    m_has_onset = 0;
    m_onset = 0;
    m_index = 0;
    m_period = period;
    m_w = period * lead_num / lead_den;
    m_pe = period > 0 ? period : 10 * MS_NS;
    for (ch = 0; ch < M_NCH; ch++) { m_init[ch] = 0.125f * (float)ch - 0.3f; m_trackv[ch] = m_init[ch]; }
    m_k6[0] = mkkey(0, 0.0f, YTL_EASE_LINEAR);
    m_k6[1] = mkkey(5 * m_pe, 2.0f, YTL_EASE_COSINE);
    m_k6[2] = mkkey(5 * m_pe, 7.0f, YTL_EASE_STEP);
    m_k6[3] = mkkey(12 * m_pe, 3.0f, YTL_EASE_QUAD_IN);
    m_k6[4] = mkkey(20 * m_pe, 3.5f, YTL_EASE_LINEAR);
    m_k6[5] = mkkey(30 * m_pe, 10.0f, YTL_EASE_LINEAR);
    m_k7[0] = mkkey(T0 + 2 * m_pe, 1.0f, YTL_EASE_LINEAR);
    m_k7[1] = mkkey(T0 + 10 * m_pe, 4.0f, YTL_EASE_QUAD_OUT);
    m_k7[2] = mkkey(T0 + 40 * m_pe, 2.0f, YTL_EASE_STEP);
    m_k7[3] = mkkey(T0 + 40 * m_pe, 6.0f, YTL_EASE_COSINE);
    m_k7[4] = mkkey(T0 + 80 * m_pe, 0.5f, YTL_EASE_LINEAR);
    for (i = 0; i < 64; i++) m_samp[i] = (float)((i * 29) % 64) / 8.0f - 3.0f;
    memset(m_drv, 0, sizeof m_drv);
    memset(m_pend, 0, sizeof m_pend);
    memset(m_has_drv, 0, sizeof m_has_drv);
    {
        ytl_track t6 = trk_keys(m_k6, 6), t7 = trk_keys(m_k7, 5);
        m_drv[6] = rdrv_plain(&t6);
        m_drv[7] = rdrv_plain(&t7);
    }
    m_has_drv[6] = m_has_drv[7] = 1;
    m_tb[6] = 1;
    m_tb[7] = 0;
    if (!open_h(&g_tl, g_store, M_CAP, M_NCH, m_init, (double)lead_num / (double)lead_den))
        return m_fail("open", 0, 1);
    /* open_h maps 0 to YTL_LEAD_NONE, whose lead in use is 0. */
    if (ytl_lead(&g_tl) != (double)lead_num / (double)lead_den)
        return m_fail("ytl_lead x 1000", (long long)(ytl_lead(&g_tl) * 1000.0), lead_num * 1000 / lead_den);
    if (ytl_set_keys(&g_tl, 6, 1, m_k6, 6) != 0) return m_fail("set_keys 6", 0, 1);
    if (ytl_set_keys(&g_tl, 7, 0, m_k7, 5) != 0) return m_fail("set_keys 7", 0, 1);
    if (m_check()) return 1;

    for (m_step = 0; m_step < steps; m_step++) {
        int r = rnd_int(ops >= 2 ? 140 : ops ? 126 : 110), ret;
        int64_t now = m_has_onset ? m_onset : T0;
        /* Near full, mostly remove, so FULL does not dominate the adds. */
        if (m_n >= M_CAP - 6 && r < 40 && rnd_int(5) != 0) r = 45;
        if (r < 30) {
            ytl_event e = m_rand_event();
            m_op = "add";
            ret = ytl_add(&g_tl, &e);
            if (m_check_add(ret, &e, 1)) return 1;
        } else if (r < 40) {
            ytl_event es[4];
            int n = rnd_int(8) == 0 ? 0 : 1 + rnd_int(4);
            m_op = "add_n";
            for (i = 0; i < n; i++) es[i] = m_rand_event();
            ret = ytl_add_n(&g_tl, es, n);
            if (m_check_add(ret, es, n)) return 1;
        } else if (r < 55) {
            int id, idx = -1, want;
            m_op = "remove";
            if (m_n > 0 && rnd_int(10) != 0) id = m_ev[rnd_int(m_n)].e.id;
            else id = rnd_int(m_next_id + 3) - 1;
            for (i = 0; i < m_n; i++) if (m_ev[i].e.id == id) idx = i;
            want = idx >= 0 ? 0 : YTL_ERR_NOT_FOUND;
            ret = ytl_remove(&g_tl, id);
            if (ret != want) return m_fail("remove return", ret, want);
            if (idx >= 0) {
                memmove(&m_ev[idx], &m_ev[idx + 1], (size_t)(m_n - idx - 1) * sizeof m_ev[0]);
                m_n--;
            }
        } else if (r < 63) {
            int b = rnd_int(12) == 0 ? (rnd_int(2) ? 0 : YTL_MAX_BASES) : 1 + rnd_int(3);
            int64_t rt = now + rnd_range(-2 * m_pe, 3 * m_pe), bt;
            int q = rnd_int(4);
            m_op = "anchor";
            if (q == 0) bt = 0;
            else if (q == 1 && b > 0 && b < YTL_MAX_BASES && m_b[b].evaluated) bt = m_b[b].last_on + rnd_range(-3, 3) * (m_pe / 3);
            else if (q == 2 && b > 0 && b < YTL_MAX_BASES && m_b[b].state != 0) bt = m_bt(b, rt) + rnd_range(-20 * m_pe, 5 * m_pe);
            else bt = rnd_range(-5 * m_pe, 40 * m_pe);
            ret = ytl_anchor(&g_tl, b, rt, bt);
            if (b == 0 || b == YTL_MAX_BASES) {
                if (ret != YTL_ERR_ARG) return m_fail("anchor bad base", ret, YTL_ERR_ARG);
            } else {
                if (ret != 0) return m_fail("anchor return", ret, 0);
                /* "Furthest base time of an onset evaluated": read as
                 * since the last rewind, like the reach; a rewind sets
                 * both to just before bt. */
                if (m_b[b].evaluated && bt <= m_b[b].last_on) {
                    m_rewind(b, bt);
                    m_b[b].last_hi = bt - 1;
                    m_b[b].last_on = bt - 1;
                    m_stat_arew++;
                }
                m_b[b].state = 1;
                m_b[b].art = rt;
                m_b[b].abt = bt;
                m_b[b].anc_bt = bt;
                m_b[b].nowv = bt;
                m_b[b].moved++;
                if (g_mut & MUT_RATE_ANCHOR1) m_b[b].num = m_b[b].den = 1;
            }
            m_trace("anchor", b, rt, bt, ret);
        } else if (r < 68) {
            int b = rnd_int(15) == 0 ? 0 : 1 + rnd_int(3), want;
            int64_t rt = now + rnd_range(-m_pe, 2 * m_pe);
            m_op = "pause";
            want = b == 0 ? YTL_ERR_ARG : (m_b[b].state != 1 ? YTL_ERR_ORDER : 0);
            ret = ytl_pause(&g_tl, b, rt);
            if (ret != want) return m_fail("pause return", ret, want);
            if (want == 0) { m_b[b].frozen = (g_mut & MUT_RATE_PAUSE1) && m_scaled(b) ? m_b[b].abt + (rt - m_b[b].art) : m_bt(b, rt);
                             m_b[b].state = 2; m_b[b].moved++;
                             m_b[b].nowv = m_b[b].frozen; }
            m_trace("pause", b, rt, m_b[b].frozen, ret);
        } else if (r >= 136) {
            if (m_op_rt_time(now)) return 1;
        } else if (r >= 132) {
            if (m_op_window(now)) return 1;
        } else if (r >= 126) {
            if (m_op_rate(now)) return 1;
        } else if (r >= 118) {
            if (m_op_peek(now)) return 1;
        } else if (r >= 110) {
            if (m_op_skip(now)) return 1;
        } else if (r >= 100) {
            /* Replace channel 6's or 7's driver: its keys again, a sampled
             * track with or without a repeat, a repeated keyed track with
             * a BEZIER segment, or a tween that waits for its start. */
            int tch = 6 + rnd_int(2), b = rnd_int(4), q = rnd_int(5);
            int64_t bnow = m_bt(b, now);
            ytl_track tr = trk_keys(m_k6, 6);
            if (q == 0) {
                tr = tch == 6 ? trk_keys(m_k6, 6) : trk_keys(m_k7, 5);
            } else if (q == 1) {
                static const int64_t mrates[4] = { 120, 7, 60, 999983 };
                tr = trk_samples(m_samp, 64, bnow + rnd_range(-20 * m_pe, 5 * m_pe), mrates[rnd_int(4)], rnd_int(3));
                if (rnd_int(2)) { tr.period_ns = rnd_range(m_pe / 2, 30 * m_pe); tr.repeats = rnd_int(4); }
            } else if (q == 2) {
                int64_t st = bnow + rnd_range(-10 * m_pe, 5 * m_pe);
                m_krk[tch][0] = mkbez(st, 1.0f, rnd_int(2));
                m_krk[tch][1] = mkkey(st + rnd_range(1, 4 * m_pe), 6.0f, rnd_int(5));
                m_krk[tch][2] = mkkey(m_krk[tch][1].time_ns + rnd_range(0, 4 * m_pe), -2.0f, YTL_EASE_LINEAR);
                tr = trk_keys(m_krk[tch], 3);
                tr.curves = m_curv;
                tr.n_curves = 2;
                tr.period_ns = rnd_range(m_pe, 10 * m_pe);
                tr.repeats = rnd_int(5);
            }
            if (q < 3) {
                m_op = "set_track";
                ret = ytl_set_track(&g_tl, tch, b, &tr);
                if (ret != 0) return m_fail("set_track return", ret, 0);
                m_drv[tch] = rdrv_plain(&tr);
                m_has_drv[tch] = 1;
                m_pend[tch] = 0;   /* set_track cancels a waiting tween */
                m_tb[tch] = b;
            } else {
                static const int cyc[6] = { 0, 0, 1, 2, 3, YTL_FOREVER };
                ytl_tween_desc d;
                int okn = 0, want;
                int64_t bn = m_now(b, &okn);
                memset(&d, 0, sizeof d);
                d.to = (float)rnd_int(64) / 8.0f - 2.0f;
                d.duration_ns = rnd_int(4) == 0 ? 0 : rnd_range(1, 15 * m_pe);
                d.ease = rnd_int(7);
                if (d.ease == YTL_EASE_BEZIER) {
                    d.curve = m_curv[rnd_int(2)];
                    if (rnd_int(10) == 0) d.curve.x2 = 1.5f;
                }
                d.from_set = rnd_int(3) == 0;
                d.from = (float)rnd_int(64) / 8.0f - 2.0f;
                if (d.ease == YTL_EASE_LOG && rnd_int(6) != 0) {
                    d.from_set = true;
                    d.from = (float)(1 + rnd_int(32)) / 8.0f;
                    d.to = (float)(1 + rnd_int(32)) / 8.0f;
                }
                d.cycles = cyc[rnd_int(6)];
                d.yoyo = rnd_int(4) == 0;
                if (rnd_int(3) == 0) {
                    d.keep_velocity = true;
                    if (rnd_int(5) != 0) {   /* mostly a valid one */
                        d.ease = YTL_EASE_LINEAR;
                        d.from_set = false;
                        d.yoyo = false;
                        d.cycles = rnd_int(2);
                        if (d.duration_ns == 0) d.duration_ns = rnd_range(1, 15 * m_pe);
                    }
                }
                d.start_set = !okn || rnd_int(2) == 0;
                d.start_ns = bnow + rnd_range(-3 * m_pe, 6 * m_pe);
                if (rnd_int(3) == 0) d.delay_ns = rnd_range(-2 * m_pe, 3 * m_pe);
                want = tw_valid(&d) ? 0 : YTL_ERR_ARG;
                m_op = "tween";
                ret = ytl_tween(&g_tl, tch, b, &d);
                if (ret != want) return m_fail("tween return", ret, want);
                if (ret == 0) {
                    if (m_has_drv[tch] && m_tb[tch] != b) {   /* another base: stopped at its value */
                        m_has_drv[tch] = 0;
                        m_stat_xbase++;
                    }
                    m_pend[tch] = 1;
                    m_pd[tch] = d;
                    m_ps[tch] = (d.start_set ? d.start_ns : bn) + d.delay_ns;
                    m_pcall[tch] = m_trackv[tch];
                    m_pstep[tch] = d.ease == YTL_EASE_STEP && !(g_mut & MUT_STEP_ONSET);
                    m_tb[tch] = b;
                    m_stat_tw++;
                    if (m_trace_seed == m_seed)
                        fprintf(stderr, "  [%d] tween ch%d b%d start %lld (set %d, now %lld, delay %lld) dur %lld "
                                "to %g from %g (set %d) ease %d cycles %d yoyo %d kv %d; value %g\n", m_step, tch, b,
                                (long long)m_ps[tch], (int)d.start_set, (long long)bn, (long long)d.delay_ns,
                                (long long)d.duration_ns, (double)d.to, (double)d.from, (int)d.from_set, d.ease,
                                d.cycles, (int)d.yoyo, (int)d.keep_velocity, (double)m_trackv[tch]);
                    if (d.keep_velocity) m_stat_kv++;
                    if (!d.start_set) m_stat_twnow++;
                } else {
                    m_stat_twrej++;
                }
            }
            m_stat_trk++;
            m_trace(m_op, b, tch, q, ret);
        } else if (r < 73) {
            int b = 1 + rnd_int(3), want;
            int64_t rt = now + rnd_range(0, 2 * m_pe);
            m_op = "resume";
            want = m_b[b].state != 2 ? YTL_ERR_ORDER : 0;
            ret = ytl_resume(&g_tl, b, rt);
            if (ret != want) return m_fail("resume return", ret, want);
            if (want == 0) { m_b[b].state = 1; m_b[b].art = rt; m_b[b].abt = m_b[b].frozen; m_b[b].moved++;
                             m_b[b].nowv = m_b[b].frozen; }
            m_trace("resume", b, rt, m_b[b].frozen, ret);
        } else {
            int64_t inc, onset, due = 0, mdue = 0;
            int cap = rnd_int(7), n, lim, has, mhas = 0;
            ytl_event* fp;
            m_op = "evaluate";
            if (m_has_onset && rnd_int(40) == 0) {
                ret = eval_at(&g_tl, m_onset - 1, m_period, m_index, g_fired, 4);
                if (ret != YTL_ERR_ORDER) return m_fail("evaluate backward onset", ret, YTL_ERR_ORDER);
                if (m_check()) return 1;
                continue;
            }
            if (m_period > 0) {
                int r2 = rnd_int(100);
                inc = r2 < 12 ? 0 : r2 < 85 ? m_pe : r2 < 95 ? 2 * m_pe : rnd_range(3 * m_pe, 12 * m_pe);
            } else {
                inc = rnd_int(8) == 0 ? 0 : rnd_range(MS_NS, 25 * MS_NS);
            }
            onset = m_has_onset ? m_onset + inc : T0;
            fp = (cap == 0 && rnd_int(2)) ? NULL : g_fired;
            for (i = 0; i < 16; i++) g_fired[i].id = -999;
            n = eval_at(&g_tl, onset, m_period, m_index, fp, cap);
            m_evaluate(onset, m_index);
            m_stat_eval++;
            for (i = 1; i < 4; i++) m_trace("eval", i, onset, m_bt(i, onset), n);
            m_onset = onset;
            m_has_onset = 1;
            if (n != m_nf) return m_fail("evaluate return", n, m_nf);
            lim = n < cap ? n : cap;
            for (i = 0; i < lim; i++) {
                const char* d = ev_diff(&g_fired[i], &m_fired[i]);
                if (d) {
                    print_ev("got ", &g_fired[i]);
                    print_ev("want", &m_fired[i].e);
                    return m_fail(d, i, i);
                }
            }
            if (fp && cap < 16 && g_fired[cap].id != -999) return m_fail("wrote past cap", cap, cap);
            m_index += 1 + rnd_int(2);
            has = ytl_next_due(&g_tl, &due);
            for (i = 0; i < m_n; i++) {
                const ytl_event* e = &m_ev[i].e;
                int64_t t;
                if (e->flags & M_DONE) continue;
                if (e->base != 0 && m_b[e->base].state != 1) continue;
                t = m_rt(e->base, e->time_ns);
                if (!mhas || t < mdue) { mdue = t; mhas = 1; }
            }
            if (has != (mhas != 0)) return m_fail("next_due found", has, mhas);
            if (has && due != mdue) return m_fail("next_due time", due, mdue);
        }
        if (m_check()) return 1;
    }
    return 0;
}

static void test_model(void) {
    static const struct { uint64_t seed; int num, den; int64_t period; } cfg[] = {
        { 1, 0, 1, P60 }, { 2, 1, 2, P60 }, { 3, 1, 4, P60 }, { 4, 1, 2, 0 },
        { 5, 0, 1, 0 },   { 6, 1, 2, P60 }, { 7, 0, 1, P60 }, { 8, 3, 4, P60 },
        { 9, 0, 1, P60 }, { 10, 1, 2, P60 },
    };
    size_t i;
    const char* tr = getenv("TL_TRACE");
    m_trace_seed = tr ? (uint64_t)strtoull(tr, NULL, 10) : 0;
    for (i = 0; i < sizeof cfg / sizeof cfg[0]; i++)
        (void)model_run(cfg[i].seed, cfg[i].num, cfg[i].den, cfg[i].period, 800, 0);
    /* The same leads and periods with skip and peek among the ops; other
     * seeds, so the v0.2.0 runs above stay as they were. */
    for (i = 0; i < sizeof cfg / sizeof cfg[0]; i++)
        (void)model_run(cfg[i].seed + 100, cfg[i].num, cfg[i].den, cfg[i].period, 1000, 1);
    /* RATE: rate, window and rt_time among the ops; other seeds again, so
     * the 20 runs above stay v0.3.0's. Two more at 500 Hz. TL_V030_MODEL
     * stops here, so the statistics line can be compared with v0.3.0's. */
    if (g_v030_model) return;
    for (i = 0; i < sizeof cfg / sizeof cfg[0]; i++)
        (void)model_run(cfg[i].seed + 200, cfg[i].num, cfg[i].den, cfg[i].period, 1000, 2);
    (void)model_run(221, 1, 2, 2000000, 1500, 2);
    (void)model_run(222, 0, 1, 2000000, 1500, 2);
}

/* QUANTIZATION: a desc lead of 0 is the default, 0.5; YTL_LEAD_NONE is
 * never early. An event a third of a period after frame 5's onset lands on
 * frame 5 (a third early) by default and on frame 6 under NONE. */
static void test_lead_default(void) {
    static ytl_event sa[4], sb[4];
    static ytl_timeline ta, tb;
    const int64_t P = P60;
    const double leads[3] = { 0.0, 0.5, YTL_LEAD_NONE };
    int64_t landed[3], resid[3];
    int li, k;
    for (li = 0; li < 3; li++) {
        ytl_desc d;
        ytl_event e;
        const ytl_event* r;
        memset(&d, 0, sizeof d);
        memset(sa, 0, sizeof sa);
        d.events = sa;
        d.event_capacity = 4;
        d.n_channels = 1;
        d.lead = leads[li];
        CHECK(ytl_open(&ta, &d));
        e = ev(0, 5 * P + P / 3, YTL_ONSET, 0, 0.0f);
        CHECK_I(ytl_add(&ta, &e), 0);
        for (k = 0; k <= 8; k++) (void)eval_at(&ta, k * P, P, k, NULL, 0);
        r = ytl_find(&ta, 0);
        CHECK(r != NULL);
        landed[li] = r ? r->frame : -99;
        resid[li] = r ? r->residual : -99;
    }
    CHECK_I(landed[0], 5);
    CHECK_I(resid[0], -(P / 3));
    CHECK_I(landed[1], 5);
    CHECK_I(landed[2], 6);
    CHECK_I(resid[2], P - P / 3);
    /* The default and an explicit 0.5 write the same bytes. */
    {
        ytl_desc d;
        ytl_event e;
        memset(&d, 0, sizeof d);
        memset(sa, 0, sizeof sa);
        memset(sb, 0, sizeof sb);
        d.n_channels = 1;
        d.event_capacity = 4;
        d.events = sa;
        CHECK(ytl_open(&ta, &d));
        d.events = sb;
        d.lead = 0.5;
        CHECK(ytl_open(&tb, &d));
        for (k = 0; k < 4; k++) {
            e = ev(0, k * P + (k + 1) * P / 5, YTL_MARK, 0, 0.0f);
            CHECK(ytl_add(&ta, &e) >= 0);
            CHECK(ytl_add(&tb, &e) >= 0);
        }
        for (k = 0; k <= 6; k++) {
            (void)eval_at(&ta, k * P, P, k, NULL, 0);
            (void)eval_at(&tb, k * P, P, k, NULL, 0);
        }
        CHECK(memcmp(sa, sb, sizeof sa) == 0);
    }
}

/* -------------------------------------------- v0.5.0: close and messages */

static void test_close_messages(void) {
    static ytl_timeline z;
    ytl_frame f;
    ytl_event e;
    ytl_desc d;
    ytl_seq q;
    int bad = -2;
    ytl_op op;

    /* close: on a zeroed handle, twice, after a failed open, NULL. */
    memset(&z, 0, sizeof z);
    ytl_close(&z);
    ytl_close(&z);
    CHECK(!ytl_is_open(&z));
    memset(&d, 0, sizeof d);
    d.n_channels = -1;
    CHECK(!ytl_open(&z, &d));
    ytl_close(&z);
    ytl_close(&z);
    CHECK(ytl_error(&z)[0] != '\0');
    ytl_close(NULL);

    /* After a close every call is YTL_ERR_CLOSED and says so. */
    CHECK(open_h(&g_tl, g_store, 8, 4, NULL, 0.0));
    ytl_close(&g_tl);
    CHECK(!ytl_is_open(&g_tl));
    ytl_close(&g_tl);
    CHECK_I(ytl_anchor(&g_tl, 1, 0, 0), YTL_ERR_CLOSED);
    CHECK_S(ytl_error(&g_tl), "ysp_timeline: ytl_anchor: timeline is not open");
    CHECK(ytl_values(&g_tl) == NULL);

    /* open clears the message; each failing int call writes its own. */
    CHECK(open_h(&g_tl, g_store, 1, 4, NULL, 0.0));
    CHECK_S(ytl_error(&g_tl), "");
    e = ev(1, 0, YTL_MARK, 0, 0.0f);
    CHECK(ytl_add(&g_tl, &e) >= 0);
    CHECK_S(ytl_error(&g_tl), "");
    CHECK_I(ytl_add(&g_tl, &e), YTL_ERR_FULL);
    CHECK_S(ytl_error(&g_tl), "ysp_timeline: ytl_add: event storage is full");
    CHECK_I(ytl_remove(&g_tl, 77), YTL_ERR_NOT_FOUND);
    CHECK_S(ytl_error(&g_tl), "ysp_timeline: ytl_remove: no event with that id");
    CHECK_I(ytl_pause(&g_tl, 2, 0), YTL_ERR_ORDER);
    CHECK_S(ytl_error(&g_tl), "ysp_timeline: ytl_pause: call out of order");
    CHECK_I(ytl_clamped(&g_tl, 9), YTL_ERR_ARG);
    CHECK_S(ytl_error(&g_tl), "ysp_timeline: ytl_clamped: invalid argument");
    CHECK_I(ytl_tween(&g_tl, 0, 1, NULL), YTL_ERR_ARG);
    CHECK_S(ytl_error(&g_tl), "ysp_timeline: ytl_tween: invalid argument");
    f.onset = 100; f.period = 10; f.index = 0;
    CHECK_I(ytl_evaluate(&g_tl, &f, NULL, 0), 0);
    f.onset = 50;
    CHECK_I(ytl_evaluate(&g_tl, &f, NULL, 0), YTL_ERR_ORDER);
    CHECK_S(ytl_error(&g_tl), "ysp_timeline: ytl_evaluate: call out of order");
    /* A bool query that answers false writes nothing. */
    CHECK(!ytl_rt_time(&g_tl, 9, 0, &f.onset));
    CHECK_S(ytl_error(&g_tl), "ysp_timeline: ytl_evaluate: call out of order");

    /* A sequence's sticky error names the call; check_ops names the op. */
    q = ytl_seq_on(&g_tl, 1);
    ytl_wait(&q, LIM62);
    CHECK_I(q.err, YTL_ERR_ARG);
    CHECK_S(ytl_error(&g_tl), "ysp_timeline: sequence call 0: invalid argument");
    q = ytl_seq_on(&g_tl, YTL_MAX_BASES);
    CHECK_S(ytl_error(&g_tl), "ysp_timeline: sequence on base 8: invalid argument");
    memset(&op, 0, sizeof op);
    CHECK_I(ytl_check_ops(&g_tl, 1, &op, 1, &bad), YTL_ERR_ARG);
    CHECK_I(bad, 0);
    CHECK_S(ytl_error(&g_tl), "ysp_timeline: ytl_check_ops: op 0: invalid argument");
    ytl_close(&g_tl);
}

/* ------------------------------------- 11e. TRIGGER CHANNELS (v0.8.0) */

static void test_trigger_ch(void) {
    static const ytl_op tab[] = { YTL_TRIGGER(16), YTL_TRIGGER_CH(5, 17) };
    static const ytl_op bad_tab[] = { YTL_TRIGGER(1), YTL_TRIGGER_CH(-1, 2) };
    static const int want_ch[] = { 0, 3, INT32_MAX, 0, 5 };
    static const int32_t want_code[] = { 12, 13, 14, 16, 17 };
    ytl_event fired[8], peek[8];
    ytl_seq q;
    int n, i, bad = 99;
    CHECK(seq_open(&g_tl, g_store, 64, g_ka, 64, SQ_NCH, NULL));
    q = ytl_seq_on(&g_tl, 1);
    ytl_wait(&q, YRT_MS(10));
    ytl_trigger(&q, 12);
    ytl_trigger_ch(&q, 3, 13);
    ytl_trigger_ch(&q, INT32_MAX, 14);
    CHECK_I(ytl_run(&q, tab, YTL_COUNT(tab)), 0);
    CHECK_I(q.err, 0);
    CHECK_I(ytl_anchor(&g_tl, 1, T0, 0), 0);
    /* the look-ahead and the report both carry the channel */
    CHECK_I(ytl_peek(&g_tl, 1, T0, T0 + YRT_MS(20), peek, 8), 5);
    CHECK_I(eval_at(&g_tl, T0 + YRT_MS(10), YRT_MS(16), 1, fired, 8), 5);
    for (i = 0; i < 5; i++) {
        CHECK_I(fired[i].kind, YTL_TRIGGER);
        CHECK_I(fired[i].target, want_ch[i]);
        CHECK_I(fired[i].code, want_code[i]);
        CHECK_I(peek[i].target, want_ch[i]);
    }
    /* a negative trigger channel: the call, the table, the check */
    CHECK(seq_open(&g_tl, g_store, 64, g_ka, 64, SQ_NCH, NULL));
    q = ytl_seq_on(&g_tl, 1);
    ytl_trigger(&q, 1);
    ytl_trigger_ch(&q, -1, 2);
    CHECK_I(q.err, YTL_ERR_ARG);
    CHECK_I(q.err_call, 1);
    CHECK_I(n_events(&g_tl, 1), 1);
    CHECK(seq_open(&g_tl, g_store, 64, g_ka, 64, SQ_NCH, NULL));
    CHECK_I(ytl_check_ops(&g_tl, 1, bad_tab, 2, &bad), YTL_ERR_ARG);
    CHECK_I(bad, 1);
    {
        ytl_op ops[2];
        memcpy(ops, bad_tab, sizeof ops);
        seq_run_fail(__LINE__, ops, 2, YTL_ERR_ARG, 1, 64, 64);
    }
    /* a trigger channel is not a timeline channel: past n_channels is
     * fine, and it binds nothing, so a tween can use the same number */
    CHECK(seq_open(&g_tl, g_store, 64, g_ka, 64, SQ_NCH, NULL));
    {
        ytl_op ops[2];
        memset(ops, 0, sizeof ops);
        ops[0].op = YTL_OP_TRIGGER; ops[0].ch = SQ_NCH + 7;
        ops[1].op = YTL_OP_TO; ops[1].ch = SQ_CONTRAST; ops[1].tween = sq_tw(1.0f, YRT_MS(10), 0);
        CHECK_I(ytl_check_ops(&g_tl, 1, ops, 2, &bad), 0);
        ops[0].ch = SQ_CONTRAST;
        CHECK_I(ytl_check_ops(&g_tl, 1, ops, 2, &bad), 0);
    }
    /* v0.8.1: ytl_add() and ytl_add_n() refuse a negative trigger channel
     * too, all or none, with the message; 0 and INT32_MAX pass, and the
     * other reported-only kinds keep any target */
    {
        ytl_event e[3];
        int before = n_events(&g_tl, 0);
        memset(e, 0, sizeof e);
        e[0].kind = YTL_TRIGGER;
        e[0].target = INT32_MAX;
        e[1].kind = YTL_TRIGGER;
        e[1].target = -5;
        e[2].kind = YTL_MARK;
        e[2].target = -5;
        CHECK_I(ytl_add(&g_tl, &e[1]), YTL_ERR_ARG);
        CHECK_S(ytl_error(&g_tl), "ysp_timeline: ytl_add: invalid argument");
        e[1].target = INT32_MIN;
        CHECK_I(ytl_add_n(&g_tl, e, 3), YTL_ERR_ARG);
        CHECK_I(n_events(&g_tl, 0), before);
        e[1].target = 0;
        CHECK(ytl_add_n(&g_tl, e, 3) >= 0);
        CHECK_I(n_events(&g_tl, 0), before + 3);
    }
    n = 0;
    CHECK(ytl_op_params(YTL_OP_TRIGGER, &n) != NULL && n == 2);
}

/* ------------------------------------------- 11f. PARAMETERS (v0.8.0) */

typedef struct tp_field { const char* name; size_t off; const char* type; } tp_field;
#define TP_TW(f) (offsetof(ytl_op, tween) + offsetof(ytl_tween_desc, f))
#define TP_CV(f) (offsetof(ytl_op, tween) + offsetof(ytl_tween_desc, curve) + offsetof(ytl_curve, f))
/* Every field of ytl_op, written here from the declarations. */
static const tp_field tp_op[] = {
    { "op", offsetof(ytl_op, op), "enum" },          { "ch", offsetof(ytl_op, ch), "i32" },
    { "t_ns", offsetof(ytl_op, t_ns), "i64" },       { "code", offsetof(ytl_op, code), "i32" },
    { "value", offsetof(ytl_op, value), "f32" },     { "user", offsetof(ytl_op, user), "u64" },
    { "tween.to", TP_TW(to), "f32" },                { "tween.from", TP_TW(from), "f32" },
    { "tween.duration_ns", TP_TW(duration_ns), "i64" }, { "tween.start_ns", TP_TW(start_ns), "i64" },
    { "tween.delay_ns", TP_TW(delay_ns), "i64" },
    { "tween.curve.x1", TP_CV(x1), "f32" }, { "tween.curve.y1", TP_CV(y1), "f32" },
    { "tween.curve.x2", TP_CV(x2), "f32" }, { "tween.curve.y2", TP_CV(y2), "f32" },
    { "tween.ease", TP_TW(ease), "enum" },           { "tween.cycles", TP_TW(cycles), "i32" },
    { "tween.from_set", TP_TW(from_set), "bool" },   { "tween.start_set", TP_TW(start_set), "bool" },
    { "tween.yoyo", TP_TW(yoyo), "bool" },           { "tween.keep_velocity", TP_TW(keep_velocity), "bool" },
};
static const tp_field tp_desc[] = {
    { "event_capacity", offsetof(ytl_desc, event_capacity), "i32" },
    { "n_channels", offsetof(ytl_desc, n_channels), "i32" },
    { "lead", offsetof(ytl_desc, lead), "f64" },
    { "key_capacity", offsetof(ytl_desc, key_capacity), "i32" },
    { "target_capacity", offsetof(ytl_desc, target_capacity), "i32" },
    { "tween_capacity", offsetof(ytl_desc, tween_capacity), "i32" },
};
#define TP_TWEEN "ch tween.to tween.from tween.duration_ns tween.delay_ns tween.curve.x1 tween.curve.y1 " \
                 "tween.curve.x2 tween.curve.y2 tween.ease tween.cycles tween.from_set tween.yoyo tween.keep_velocity"
/* What each kind reads, from the manual, in ytl_op order. A tween's
 * start_ns and start_set are read only to be refused, so they are no row. */
static const char* const tp_reads[10] = {
    "op", "ch", "ch", "ch value", "ch code", "code user", "t_ns", "t_ns", TP_TWEEN, TP_TWEEN };

static const tp_field* tp_find(const tp_field* f, int n, const char* name) {
    int i;
    for (i = 0; i < n; i++) if (strcmp(f[i].name, name) == 0) return &f[i];
    return NULL;
}

static void tp_write(void* base, const tp_field* f, double v) {
    char* p = (char*)base + f->off;
    if (strcmp(f->type, "i64") == 0) { int64_t x = (int64_t)v; memcpy(p, &x, sizeof x); }
    else if (strcmp(f->type, "u64") == 0) { uint64_t x = (uint64_t)v; memcpy(p, &x, sizeof x); }
    else if (strcmp(f->type, "f32") == 0) { float x = (float)v; memcpy(p, &x, sizeof x); }
    else if (strcmp(f->type, "f64") == 0) memcpy(p, &v, sizeof v);
    else if (strcmp(f->type, "bool") == 0) { bool x = v != 0.0; memcpy(p, &x, sizeof x); }
    else { int32_t x = (int32_t)v; memcpy(p, &x, sizeof x); }   /* i32, enum: int */
}

/* "name=value,..." in value order, names lower case, values contiguous
 * from min to max. Returns the pair count, or -1. */
static int tp_values(const char* s, double min, double max, const char* const* names, const int* nums, int nn) {
    int k = 0, j;
    while (s && *s) {
        char name[32];
        int len = 0;
        long v;
        char* end;
        while (*s && *s != '=') {
            if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '_') || len >= 31) return -1;
            name[len++] = *s++;
        }
        name[len] = 0;
        if (*s++ != '=' || len == 0) return -1;
        v = strtol(s, &end, 10);
        if (end == s || (double)v != min + k) return -1;
        for (j = 0; j < nn; j++)
            if (strcmp(names[j], name) == 0 && nums[j] != (int)v) return -1;
        s = end;
        if (*s == ',') s++;
        else if (*s) return -1;
        k++;
    }
    return (double)(k - 1) == max - min ? k : -1;
}

/* A valid op of `kind` for the row `name`: the field at its bounds must
 * pass what the other fields allow. */
static ytl_op tp_base(int kind, const char* name) {
    ytl_op o;
    memset(&o, 0, sizeof o);
    o.op = kind;
    switch (kind) {
    case YTL_OP_ON: case YTL_OP_OFF: o.ch = 2; break;
    case YTL_OP_SET_VALUE: o.ch = 2; o.value = 0.25f; break;
    case YTL_OP_TRIGGER: o.ch = 1; o.code = 12; break;
    case YTL_OP_MARK: o.code = 3; o.user = 5; break;
    case YTL_OP_WAIT: o.t_ns = YRT_MS(20); break;
    case YTL_OP_AT: o.t_ns = YRT_MS(40); break;
    default:
        o.ch = 2;
        o.tween.to = 0.5f;
        if (strcmp(name, "tween.yoyo") == 0 || strcmp(name, "tween.cycles") == 0 ||
            strcmp(name, "tween.keep_velocity") == 0 || strcmp(name, "ch") == 0)
            o.tween.duration_ns = YRT_MS(100);
        if (strncmp(name, "tween.curve.", 12) == 0) o.tween.ease = YTL_EASE_BEZIER;
        if (strcmp(name, "tween.from") == 0) o.tween.from_set = true;
        break;
    }
    return o;
}

/* One op on a new sequence at 10 ms, then 40 frames: the storage, the
 * arena and every value, into *out. */
typedef struct tp_run { int rc; int n; ytl_event ev[8]; ytl_key keys[64]; float v[40][4]; } tp_run;
static void tp_apply(const ytl_op* o, tp_run* r) {
    static ytl_event st[8];
    static ytl_key ka[64];
    const ytl_event* e;
    ytl_seq q;
    int i;
    memset(r, 0, sizeof *r);
    if (!seq_open(&g_tl3, st, 8, ka, 64, 4, NULL)) { r->rc = -100; return; }
    q = ytl_seq_on(&g_tl3, 1);
    ytl_wait(&q, YRT_MS(10));
    r->rc = ytl_run(&q, o, 1);
    (void)ytl_anchor(&g_tl3, 1, T0, 0);
    for (i = 0; i < 40; i++) {
        (void)eval_at(&g_tl3, T0 + (int64_t)i * YRT_MS(5), YRT_MS(5), i, NULL, 0);
        memcpy(r->v[i], ytl_values(&g_tl3), sizeof r->v[i]);
    }
    e = ytl_events(&g_tl3, YTL_ALL_BASES, &r->n);
    if (r->n > 0 && r->n <= 8) memcpy(r->ev, e, (size_t)r->n * sizeof *e);
    memcpy(r->keys, ka, sizeof ka);
}

static int tp_check1(const ytl_op* o) {
    static ytl_event st[8];
    int bad;
    if (!seq_open(&g_tl3, st, 8, g_kb, SQ_ARENA, YTL_MAX_CHANNELS, NULL)) return -100;
    return ytl_check_ops(&g_tl3, 1, o, 1, &bad);
}

static void test_params(void) {
    static tp_run ra, rb;
    static const char* const op_names[] = { "none", "on", "off", "set_value", "trigger", "mark", "wait", "at", "to", "then" };
    static const int op_nums[] = { YTL_OP_NONE, YTL_OP_ON, YTL_OP_OFF, YTL_OP_SET_VALUE, YTL_OP_TRIGGER,
                                   YTL_OP_MARK, YTL_OP_WAIT, YTL_OP_AT, YTL_OP_TO, YTL_OP_THEN };
    static const char* const ease_names[] = { "linear", "step", "cosine", "quad_in", "quad_out", "log", "bezier",
                                              "quad_in_out", "cubic_in", "cubic_out", "cubic_in_out" };
    static const int ease_nums[] = { YTL_EASE_LINEAR, YTL_EASE_STEP, YTL_EASE_COSINE, YTL_EASE_QUAD_IN,
                                     YTL_EASE_QUAD_OUT, YTL_EASE_LOG, YTL_EASE_BEZIER, YTL_EASE_QUAD_IN_OUT,
                                     YTL_EASE_CUBIC_IN, YTL_EASE_CUBIC_OUT, YTL_EASE_CUBIC_IN_OUT };
    const int64_t LIM = INT64_C(1) << 62;
    const ytl_param* t;
    int kind, i, j, n;
    n = 7;
    CHECK(ytl_op_params(YTL_OP_THEN + 1, &n) == NULL && n == 0);
    n = 7;
    CHECK(ytl_op_params(-1, &n) == NULL && n == 0);
    CHECK(ytl_op_params(YTL_OP_ON, NULL) != NULL);
    for (kind = YTL_OP_NONE; kind <= YTL_OP_THEN; kind++) {
        const char* s;
        size_t prev_off = 0;
        t = ytl_op_params(kind, &n);
        CHECK(t != NULL && n > 0);
        if (!t) continue;
        /* the rows are exactly the fields the kind reads, in ytl_op order */
        for (i = 0, s = tp_reads[kind]; *s; i++) {
            size_t len = strcspn(s, " ");
            if (i >= n || strlen(t[i].name) != len || strncmp(t[i].name, s, len) != 0)
                fail_s(__LINE__, "op row", i < n ? t[i].name : "(none)", s);
            s += len;
            if (*s == ' ') s++;
        }
        CHECK_I(n, i);
        for (i = 0; i < n; i++) {
            const tp_field* f = tp_find(tp_op, YTL_COUNT(tp_op), t[i].name);
            size_t L = strlen(t[i].name);
            bool ns = L > 3 && strcmp(t[i].name + L - 3, "_ns") == 0;
            CHECK(f != NULL);
            if (!f) continue;
            CHECK_S(t[i].type, f->type);
            CHECK(i == 0 || f->off > prev_off);
            prev_off = f->off;
            CHECK(t[i].min <= t[i].def && t[i].def <= t[i].max);
            CHECK(t[i].unit && t[i].doc && t[i].doc[0]);
            CHECK(strcmp(t[i].unit, ns ? "ns" : "") == 0);
            CHECK((t[i].values != NULL) == (strcmp(t[i].type, "enum") == 0));
            if (strcmp(t[i].type, "bool") == 0) CHECK(t[i].min == 0 && t[i].max == 1);
            for (j = 0; j < i; j++) CHECK(strcmp(t[i].name, t[j].name) != 0);
            if (strcmp(t[i].name, "op") == 0)
                CHECK_I(tp_values(t[i].values, t[i].min, t[i].max, op_names, op_nums, 10), 10);
            if (strcmp(t[i].name, "tween.ease") == 0)
                CHECK_I(tp_values(t[i].values, t[i].min, t[i].max, ease_names, ease_nums, 11), 11);
        }
        if (kind == YTL_OP_NONE) continue;
        /* def is what a zero field means: the field at 0 and at def give
         * the same storage, arena and values */
        for (i = 0; i < n; i++) {
            const tp_field* f = tp_find(tp_op, YTL_COUNT(tp_op), t[i].name);
            ytl_op a = tp_base(kind, "ch"), b;   /* "ch": a tween with a duration */
            if (!f) continue;
            tp_write(&a, f, 0.0);
            b = a;
            tp_write(&b, f, t[i].def);
            tp_apply(&a, &ra);
            tp_apply(&b, &rb);
            if (ra.rc != 0 || memcmp(&ra, &rb, sizeof ra) != 0)
                fail_s(__LINE__, "zero and def differ", tp_reads[kind], t[i].name);
            if (strcmp(t[i].name, "tween.cycles") == 0) {
                /* def is the count in use, not another spelling of 0:
                 * the tween ends after def plays */
                ytl_seq q;
                CHECK(seq_open(&g_tl3, g_store3, 8, g_kb, 64, 4, NULL));
                q = ytl_seq_on(&g_tl3, 1);
                CHECK_I(ytl_to(&q, a.ch, &a.tween), (int64_t)t[i].def * a.tween.duration_ns);
            }
        }
        /* the fields that are no row change nothing (start_set excepted:
         * it is read to be refused) */
        {
            ytl_op a = tp_base(kind, ""), b = a;
            for (j = 0; j < YTL_COUNT(tp_op); j++) {
                const tp_field* f = &tp_op[j];
                double junk = strcmp(f->type, "bool") == 0 ? 1.0 : strcmp(f->type, "i64") == 0 ? (double)YRT_MS(3) : 3.0;
                bool row = false;
                for (i = 0; i < n; i++) if (strcmp(t[i].name, f->name) == 0) row = true;
                if (row || strcmp(f->name, "op") == 0 || strcmp(f->name, "tween.start_set") == 0) continue;
                tp_write(&b, f, junk);
            }
            tp_apply(&a, &ra);
            tp_apply(&b, &rb);
            if (ra.rc != 0 || memcmp(&ra, &rb, sizeof ra) != 0) fail_s(__LINE__, "a field that is no row changed the op", tp_reads[kind], "");
        }
        /* min and max pass as far as the field goes; past them is refused */
        for (i = 0; i < n; i++) {
            const tp_field* f = tp_find(tp_op, YTL_COUNT(tp_op), t[i].name);
            ytl_op o;
            int rc;
            if (!f || strcmp(t[i].type, "bool") == 0) continue;
            o = tp_base(kind, t[i].name); tp_write(&o, f, t[i].min);
            rc = tp_check1(&o);
            if (rc == YTL_ERR_ARG) fail_s(__LINE__, "min refused", tp_reads[kind], t[i].name);
            o = tp_base(kind, t[i].name); tp_write(&o, f, t[i].max);
            rc = tp_check1(&o);
            if (rc == YTL_ERR_ARG) fail_s(__LINE__, "max refused", tp_reads[kind], t[i].name);
            if (strcmp(t[i].type, "i64") == 0) {
                int64_t x;
                o = tp_base(kind, t[i].name); x = t[i].min == 0 ? -1 : -LIM;
                memcpy((char*)&o + f->off, &x, sizeof x);
                if (tp_check1(&o) >= 0) fail_s(__LINE__, "below min passed", tp_reads[kind], t[i].name);
                o = tp_base(kind, t[i].name); x = LIM;
                memcpy((char*)&o + f->off, &x, sizeof x);
                if (tp_check1(&o) >= 0) fail_s(__LINE__, "above max passed", tp_reads[kind], t[i].name);
            } else if (strcmp(t[i].type, "i32") == 0 || strcmp(t[i].type, "enum") == 0) {
                if (t[i].min > -2147483648.0) {
                    o = tp_base(kind, t[i].name); tp_write(&o, f, t[i].min - 1);
                    if (tp_check1(&o) >= 0) fail_s(__LINE__, "below min passed", tp_reads[kind], t[i].name);
                }
                if (t[i].max < 2147483647.0) {
                    o = tp_base(kind, t[i].name); tp_write(&o, f, t[i].max + 1);
                    if (tp_check1(&o) >= 0) fail_s(__LINE__, "above max passed", tp_reads[kind], t[i].name);
                }
            } else if (strcmp(t[i].type, "f32") == 0) {
                o = tp_base(kind, t[i].name);
                tp_write(&o, f, t[i].max < 1e30 ? t[i].max + 0.25 : make_inf());
                if (tp_check1(&o) >= 0) fail_s(__LINE__, "above max passed", tp_reads[kind], t[i].name);
                o = tp_base(kind, t[i].name);
                tp_write(&o, f, t[i].min > -1e30 ? t[i].min - 0.25 : -make_inf());
                if (tp_check1(&o) >= 0) fail_s(__LINE__, "below min passed", tp_reads[kind], t[i].name);
            }
        }
    }
    /* the desc */
    t = ytl_desc_params(&n);
    CHECK_I(n, YTL_COUNT(tp_desc));
    for (i = 0; i < n; i++) {
        const tp_field* f = tp_find(tp_desc, YTL_COUNT(tp_desc), t[i].name);
        ytl_desc a, b;
        CHECK(f != NULL);
        if (!f) continue;
        CHECK_S(t[i].type, f->type);
        CHECK(f == &tp_desc[i]);   /* in ytl_desc order */
        CHECK(t[i].min <= t[i].def && t[i].def <= t[i].max && t[i].values == NULL && t[i].doc && t[i].unit);
        memset(&a, 0, sizeof a);
        b = a;
        tp_write(&b, f, t[i].def);
        CHECK(ytl_open(&g_tl, &a) && ytl_open(&g_tl3, &b));
        CHECK(memcmp(&g_tl, &g_tl3, TL_STATE) == 0);
        /* and the value in use is def itself, not another spelling of 0 */
        if (strcmp(t[i].name, "lead") == 0) CHECK_F(ytl_lead(&g_tl), t[i].def, 0.0);
        memset(&a, 0, sizeof a);
        tp_write(&a, f, t[i].min);
        CHECK(ytl_open(&g_tl, &a));
        tp_write(&a, f, t[i].min - (strcmp(f->type, "f64") == 0 ? 0.5 : 1.0));
        CHECK(!ytl_open(&g_tl, &a));
    }
    {   /* the bounds that need no storage behind them */
        ytl_desc d;
        memset(&d, 0, sizeof d);
        d.lead = 0.99999999999999989;
        CHECK(ytl_open(&g_tl, &d));
        d.lead = 1.0;
        CHECK(!ytl_open(&g_tl, &d));
        d.lead = YTL_LEAD_NONE;   /* the sentinel the doc names */
        CHECK(ytl_open(&g_tl, &d));
        d.lead = 0;
        d.n_channels = YTL_MAX_CHANNELS;
        CHECK(ytl_open(&g_tl, &d));
        d.n_channels = YTL_MAX_CHANNELS + 1;
        CHECK(!ytl_open(&g_tl, &d));
    }
}

#include "timeline_test_targets.h"

/* ------------------------------------------------------------------- main */

int main(void) {
    const char* mu = getenv("TL_MUT");
    const char* v3 = getenv("TL_V030_MODEL");
    g_v030_model = v3 != NULL;
    g_mut = mu ? (unsigned)strtoul(mu, NULL, 10) : 0u;
    test_open();
    test_close_messages();
    test_lead_default();
    test_misc();
    {
        /* 60, 144, 240, 360, 500 and 1000 Hz, periods rounded to ns. */
        static const int64_t periods[] = { 16666667, 6944444, 4166667, 2777778,
                                           2000000, 1000000 };
        int pi;
        for (pi = 0; pi < (int)(sizeof periods / sizeof periods[0]); pi++) {
            uint64_t sd = 11 + 10 * (uint64_t)pi;
            quant_grid(periods[pi], 0, 1, 0, sd);
            quant_grid(periods[pi], 1, 2, 0, sd + 1);
            quant_grid(periods[pi], 1, 4, 0, sd + 2);
            quant_grid(periods[pi], 0, 1, 3, sd + 3);
            quant_grid(periods[pi], 1, 2, 5, sd + 4);
        }
    }
    quant_vrr(16);
    test_bases();
    test_channels();
    test_sample();
    test_tracks();
    test_sampled();
    test_sampled_cubic();
    test_long_table();
    test_repeat();
    test_flicker();
    test_bezier();
    test_set_track();
    test_tween();
    test_tween_macros();
    test_tween_now();
    test_tween_from();
    test_tween_velocity();
    test_tween_cycles();
    test_tween_step();
    test_tween_retarget();
    test_tween_reject();
    test_tween_cancel();
    test_tween_cross_base();
    test_tween_rewind();
    test_track_eval();
    test_step_read();
    test_report();
    test_late();
    test_rewind();
    test_trial_twice();
    test_settled();
    test_remove();
    test_next_due();
    test_replay();
    test_lead();
    test_skip_seek();
    test_skip_back();
    test_skip_late_wait();
    test_skip_misc();
    test_skip_tracks();
    test_peek();
    test_peek_lookahead();
    test_rate_arith();
    test_rate_calls();
    test_rate_quant();
    test_rate_sweep();
    test_rate_tracks();
    test_rate_peek();
    test_rate_drift();
    test_rate_clamp();
    test_seq_trial();
    test_seq_run();
    test_seq_cancel();
    test_seq_vs_tween();
    test_seq_arena();
    test_seq_on_n();
    test_seq_cross_base();
    test_seq_model();
    test_trigger_ch();
    test_params();
    test_targets();
    test_model();
    if (m_stat_promo < 100 || m_stat_kv < 20 || m_stat_xbase < 20 || m_stat_twnow < 50 || m_stat_twrej < 20)
        fail(__LINE__, "model: too few tween takeovers, keep_velocity, cross-base, now or rejected tweens (vacuous)");
    if (m_stat_skip < 300 || m_stat_skipped < 300 || m_stat_skipback < 50 || m_stat_skiplate < 20
        || m_stat_peek < 300 || m_stat_peeked < 300 || m_stat_peektrunc < 50)
        fail(__LINE__, "model: too few skips, skipped events, skips back, late skips or peeks (vacuous)");
    if (g_ref_bad) fail(__LINE__, "the limb reference disagrees with __int128");
    if (!g_v030_model && (m_stat_rate < 300 || m_stat_rate_run < 100 || m_stat_rate_behind < 20 || m_stat_rate_fired < 300
        || m_stat_win < 200 || m_stat_rtt < 200))
        fail(__LINE__, "model: too few rate changes, changes while running, windows behind the reach "
             "after one, events fired on scaled bases, windows or rt_times (vacuous)");
    fprintf(g_failures ? stderr : stdout,
            "timeline_test: %s (model: %ld evaluates, %ld fired, %ld late, "
            "%ld anchor rewinds, %ld windows behind reach, %ld un-fired, %ld rejected adds, %ld late waits, "
            "%ld track ops; tweens %ld accepted, %ld rejected, %ld took over, %ld keep_velocity, "
            "%ld cross-base, %ld from now; %ld skips (%ld back) skipped %ld events (%ld late); "
            "%ld peeks listed %ld events, %ld over the cap; %ld rate changes (%ld running, %ld windows "
            "behind the reach after one, %ld fired on scaled bases), %ld windows, %ld rt_times; "
            "%lld reference checks against __int128, %lld bad)\n",
            g_failures ? "FAILED" : "all checks passed",
            m_stat_eval, m_stat_fired, m_stat_late, m_stat_arew, m_stat_wrew, m_stat_unfired, m_stat_err, m_stat_wait,
            m_stat_trk, m_stat_tw, m_stat_twrej, m_stat_promo, m_stat_kv, m_stat_xbase, m_stat_twnow,
            m_stat_skip, m_stat_skipback, m_stat_skipped, m_stat_skiplate, m_stat_peek, m_stat_peeked,
            m_stat_peektrunc, m_stat_rate, m_stat_rate_run, m_stat_rate_behind, m_stat_rate_fired,
            m_stat_win, m_stat_rtt, g_ref_checks, g_ref_bad);
    if (g_failures) {
        fprintf(stderr, "timeline_test: %d failure(s)\n", g_failures);
        return 1;
    }
    return 0;
}
