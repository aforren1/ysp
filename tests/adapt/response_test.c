/* response_test.c - self-checking test for ysp/response.h. No framework:
 * it returns 0 when every check passed and 1 after printing each failure.
 *
 *     gcc -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Werror -Iinclude \
 *         -o response_test tests/adapt/response_test.c -lm
 *
 * Every case is a synthetic stream of inputs and onsets with the result it
 * must give: no display, no SDL library. With YRSP_TEST_SDL defined and
 * SDL3's headers on the include path, the SDL adapter is checked too, on
 * SDL_Event structs filled by hand (it calls no SDL function, so nothing is
 * linked). ysp/screen.h, ysp/audio.h and ysp/timeline.h are included for
 * their structs only, for the onset helpers and the frame-loop helpers
 * (v0.4); ysp/trials.h for its data writer, which YRSP_DATA_COLS feeds.
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#if defined(YRSP_TEST_SDL)
#include <SDL3/SDL_events.h>
#endif
#include "ysp/screen.h"
#include "ysp/audio.h"
#include "ysp/timeline.h"
#define YSP_TRIALS_IMPLEMENTATION         /* the data writer, for YRSP_DATA_COLS */
#include "ysp/trials.h"
#define YSP_RESPONSE_IMPLEMENTATION
#include "ysp/response.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- harness */

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond) do { g_checks++; if (!(cond)) { \
    fprintf(stderr, "response_test: FAIL at line %d: %s\n", __LINE__, #cond); g_failures++; } } while (0)
#define CHECK_I(got, want) do { long long g_ = (long long)(got), w_ = (long long)(want); g_checks++; \
    if (g_ != w_) { fprintf(stderr, "response_test: FAIL at line %d: %s (got %lld, want %lld)\n", \
                            __LINE__, #got, g_, w_); g_failures++; } } while (0)
#define CHECK_D(got, want) do { double g_ = (got), w_ = (want); g_checks++; \
    if (!(fabs(g_ - w_) <= 1e-12)) { fprintf(stderr, "response_test: FAIL at line %d: %s (got %.12g, " \
                                             "want %.12g)\n", __LINE__, #got, g_, w_); g_failures++; } } while (0)
#define CHECK_S(got, want) do { const char* g_ = (got); const char* w_ = (want); g_checks++; \
    if (!g_ || !w_ || strcmp(g_, w_) != 0) { fprintf(stderr, "response_test: FAIL at line %d: %s " \
        "(got [%s], want [%s])\n", __LINE__, #got, g_ ? g_ : "(null)", w_ ? w_ : "(null)"); g_failures++; } } while (0)
#define CHECK_HAS(str, sub) do { const char* s_ = (str); g_checks++; \
    if (!s_ || !strstr(s_, (sub))) { fprintf(stderr, "response_test: FAIL at line %d: [%s] lacks [%s]\n", \
                                             __LINE__, s_ ? s_ : "(null)", (sub)); g_failures++; } } while (0)

#define MS(x) ((int64_t)(x) * 1000000)
#define SC_F 9u
#define SC_J 13u
#define SC_K 14u

static yrsp_collector g_c;

static yrsp_input ev(int kind, int type, int64_t t, uint32_t control) {
    yrsp_input in;
    memset(&in, 0, sizeof in);
    in.kind = (uint8_t)kind;
    in.type = (uint8_t)type;
    in.t = t;
    in.control = control;
    in.value = type == YRSP_PRESS ? 1.0f : 0.0f;
    if (kind == YRSP_KIND_KEYBOARD) in.code = control == SC_F ? 'f' : control == SC_J ? 'j' : 'k';
    return in;
}

static int key(int64_t t, uint32_t sc, int down) {
    yrsp_input in = ev(YRSP_KIND_KEYBOARD, down ? YRSP_PRESS : YRSP_RELEASE, t, sc);
    return yrsp_feed(&g_c, &in);
}

static int key_repeat(int64_t t, uint32_t sc) {
    yrsp_input in = ev(YRSP_KIND_KEYBOARD, YRSP_PRESS, t, sc);
    in.flags = YRSP_IN_REPEAT;
    return yrsp_feed(&g_c, &in);
}

static int tap(int64_t t, uint32_t sc) {
    key(t, sc, 1);
    return key(t + MS(80), sc, 0);
}

static int sample(int kind, int64_t t, uint32_t control, float v, float x, float y) {
    yrsp_input in = ev(kind, YRSP_SAMPLE, t, control);
    in.value = v;
    in.x = x;
    in.y = y;
    return yrsp_feed(&g_c, &in);
}

static int onset(int64_t t, int final) {
    yrsp_onset o;
    memset(&o, 0, sizeof o);
    o.t_ns = t;
    o.final = (uint8_t)final;
    o.tier = (uint8_t)(final ? 1 : 0);
    o.src = (uint8_t)(final ? YRSP_ONSET_FLIP : YRSP_ONSET_PLAN);
    o.frame = 42;
    o.residual_ns = 1234;
    return yrsp_set_onset(&g_c, &o);
}

static const yrsp_choice g_fj[] = { { "f", NULL, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
                                      { "j", "right", 0, 0, 0, 0, 0, 0, 0, 0, 0 } };

static yrsp_desc fj_desc(double duration, double min_rt) {
    yrsp_desc d;
    memset(&d, 0, sizeof d);
    d.choices = g_fj;
    d.n_choices = 2;
    d.duration_ns = YRT_S(duration);
    d.min_rt_ns = YRT_S(min_rt);
    return d;
}

static yrsp_result finish(void) {
    yrsp_result r;
    CHECK_I(yrsp_finish(&g_c, &r), YRSP_ENDED);
    return r;
}

/* ------------------------------------------------------------------ cases */

static void check_names(void) {
    CHECK_I(yrsp_scancode("f"), 9);
    CHECK_I(yrsp_scancode("F"), 9);
    CHECK_I(yrsp_scancode("a"), 4);
    CHECK_I(yrsp_scancode("z"), 29);
    CHECK_I(yrsp_scancode("1"), 30);
    CHECK_I(yrsp_scancode("0"), 39);
    CHECK_I(yrsp_scancode(" "), 44);
    CHECK_I(yrsp_scancode("Space"), 44);
    CHECK_I(yrsp_scancode("ArrowLeft"), 80);
    CHECK_I(yrsp_scancode("left"), 80);
    CHECK_I(yrsp_scancode("Enter"), 40);
    CHECK_I(yrsp_scancode("/"), 56);
    CHECK_I(yrsp_scancode("f12"), 69);
    CHECK_I(yrsp_scancode("ff"), -1);
    CHECK_I(yrsp_scancode(""), -1);
    CHECK_I(yrsp_scancode(NULL), -1);
    CHECK_S(yrsp_key_name(9), "f");
    CHECK_S(yrsp_key_name(39), "0");
    CHECK_S(yrsp_key_name(44), "space");
    CHECK_S(yrsp_key_name(80), "left");
    CHECK_S(yrsp_key_name(40), "enter");
    CHECK(yrsp_key_name(200) == NULL);
    CHECK_S(yrsp_version(), YRSP_VERSION_STRING);
}

static void check_validation(void) {
    yrsp_desc d;
    yrsp_choice bad[2];
    yrsp_source src;
    yrsp_input trace[4];
    memset(bad, 0, sizeof bad);
    d = fj_desc(1.5, 0.1);
    CHECK(yrsp_open(&g_c, &d));
    CHECK_S(yrsp_error(&g_c), "");
    d = fj_desc(1.5, 0.1);
    d.n_choices = YRSP_MAX_CHOICES + 1;
    CHECK(!yrsp_open(&g_c, &d));
    CHECK_HAS(yrsp_error(&g_c), "n_choices");
    d = fj_desc(1.5, 0.1);
    d.choices = NULL;
    CHECK(!yrsp_open(&g_c, &d));
    d = fj_desc(-1, 0);
    CHECK(!yrsp_open(&g_c, &d));
    CHECK_HAS(yrsp_error(&g_c), "duration");
    d = fj_desc(1.5, 0.1);
    d.min_rt_ns = -1;
    CHECK(!yrsp_open(&g_c, &d));
    CHECK_HAS(yrsp_error(&g_c), "min_rt_ns");
    d = fj_desc(1.5, 0.1);
    d.max_hold_ns = YRT_S(1e6) + 1;
    CHECK(!yrsp_open(&g_c, &d));
    CHECK_HAS(yrsp_error(&g_c), "max_hold_ns");
    d = fj_desc(0.5, 0.5);
    CHECK(!yrsp_open(&g_c, &d));
    CHECK_HAS(yrsp_error(&g_c), "min_rt_ns");
    d = fj_desc(0, 0.5);                       /* no deadline: any minimum */
    CHECK(yrsp_open(&g_c, &d));
    d = fj_desc(1.001, 0);                     /* YRT_S rounds 1000999999.99... */
    CHECK(yrsp_open(&g_c, &d));
    CHECK_I(g_c.duration_ns, 1001000000);
    d = fj_desc(1, 0);
    d.mode = YRSP_CHOICES_NONE;
    CHECK(!yrsp_open(&g_c, &d));
    CHECK_HAS(yrsp_error(&g_c), "NONE");
    d = fj_desc(1, 0);
    d.mode = 7;
    CHECK(!yrsp_open(&g_c, &d));
    memset(&d, 0, sizeof d);
    d.mode = YRSP_CHOICES_LIST;
    CHECK(!yrsp_open(&g_c, &d));
    /* key names */
    memset(&d, 0, sizeof d);
    bad[0].key = "ff";
    d.choices = bad;
    d.n_choices = 1;
    CHECK(!yrsp_open(&g_c, &d));
    CHECK_HAS(yrsp_error(&g_c), "'ff'");
    bad[0].key = "f";
    bad[0].control = 13;
    CHECK(!yrsp_open(&g_c, &d));
    CHECK_HAS(yrsp_error(&g_c), "scancode 9");
    bad[0].control = 9;
    CHECK(yrsp_open(&g_c, &d));
    bad[0].key = NULL;
    bad[0].control = 0;
    CHECK(!yrsp_open(&g_c, &d));
    CHECK_HAS(yrsp_error(&g_c), "no key");
    bad[0].match = YRSP_MATCH_KEYCODE;
    CHECK(!yrsp_open(&g_c, &d));
    bad[0].key = "f";
    bad[0].code = 'g';
    CHECK(!yrsp_open(&g_c, &d));
    bad[0].code = 0;
    CHECK(yrsp_open(&g_c, &d));
    CHECK_I(g_c.ch[0].code, 'f');
    bad[0].kind = YRSP_KIND_BOX;
    CHECK(!yrsp_open(&g_c, &d));            /* a key name on a box */
    /* crossings */
    memset(bad, 0, sizeof bad);
    bad[0].kind = YRSP_KIND_GAMEPAD;
    bad[0].cross = YRSP_CROSS_RISING;
    bad[0].level = (float)NAN;
    CHECK(!yrsp_open(&g_c, &d));
    bad[0].level = 0.5f;
    bad[0].hysteresis = -0.1f;
    CHECK(!yrsp_open(&g_c, &d));
    bad[0].hysteresis = 0.1f;
    CHECK(yrsp_open(&g_c, &d));
    bad[0].kind = YRSP_KIND_KEYBOARD;
    CHECK(!yrsp_open(&g_c, &d));
    bad[0].kind = YRSP_KIND_MOUSE;
    bad[0].cross = YRSP_CROSS_DISTANCE;
    bad[0].level = 0;
    CHECK(!yrsp_open(&g_c, &d));
    bad[0].cross = 9;
    CHECK(!yrsp_open(&g_c, &d));
    bad[0].cross = 0;
    bad[0].kind = 16;
    CHECK(!yrsp_open(&g_c, &d));
    /* sources, trace, counts */
    d = fj_desc(1, 0);
    memset(&src, 0, sizeof src);
    src.lo_ns = 5000;
    src.hi_ns = 1000;
    d.sources = &src;
    d.n_sources = 1;
    CHECK(!yrsp_open(&g_c, &d));
    d.n_sources = YRSP_MAX_SOURCES + 1;
    CHECK(!yrsp_open(&g_c, &d));
    d = fj_desc(1, 0);
    d.trace_cap = 4;
    CHECK(!yrsp_open(&g_c, &d));
    d.trace = trace;
    CHECK(yrsp_open(&g_c, &d));
    d.trace_cap = -1;
    CHECK(!yrsp_open(&g_c, &d));
    d = fj_desc(1, 0);
    d.max_responses = -1;
    CHECK(!yrsp_open(&g_c, &d));
    CHECK(!yrsp_open(&g_c, NULL));
    CHECK(!yrsp_open(NULL, &d));
    /* a collector that failed init refuses calls */
    CHECK_I(yrsp_feed(&g_c, NULL), YRSP_ERR_STATE);
    {
        yrsp_input in = ev(0, YRSP_PRESS, 0, SC_F);
        yrsp_result r;
        CHECK_I(yrsp_feed(&g_c, &in), YRSP_ERR_STATE);
        CHECK_I(yrsp_update(&g_c, 0), YRSP_ERR_STATE);
        CHECK_I(yrsp_finish(&g_c, &r), YRSP_ERR_STATE);
        d = fj_desc(1, 0);
        CHECK(yrsp_open(&g_c, &d));
        in.type = 0;
        CHECK_I(yrsp_feed(&g_c, &in), YRSP_ERR_ARG);
        in.type = 9;
        CHECK_I(yrsp_feed(&g_c, &in), YRSP_ERR_ARG);
        in.type = YRSP_PRESS;
        in.kind = 16;
        CHECK_I(yrsp_feed(&g_c, &in), YRSP_ERR_ARG);
        CHECK_I(yrsp_finish(&g_c, NULL), YRSP_ERR_ARG);
        CHECK_I(yrsp_set_onset(&g_c, NULL), YRSP_ERR_ARG);
    }
}

/* The basic trial: rt from the onset, the first press ends it. */
static void check_basic(void) {
    yrsp_desc d = fj_desc(1.5, 0.1);
    yrsp_result r;
    const yrsp_entry* e;
    int n;
    CHECK(yrsp_open(&g_c, &d));
    CHECK_I(key(MS(1), SC_F, 1), YRSP_IDLE);          /* before arm: held state only */
    CHECK_I(key(MS(50), SC_F, 0), YRSP_IDLE);
    yrsp_arm(&g_c, MS(100));
    CHECK_I(yrsp_update(&g_c, MS(5000)), YRSP_OPEN); /* no onset, no deadline */
    CHECK_I(onset(MS(600), 0), YRSP_OPEN);
    CHECK_I(onset(MS(601), 1), YRSP_OPEN);
    CHECK_I(key(MS(1001), SC_J, 1), YRSP_ENDED);
    CHECK_I(key(MS(1101), SC_J, 0), YRSP_ENDED);
    r = finish();
    CHECK_D(YRT_TO_S(r.rt_ns), 0.4);
    CHECK_D(YRT_TO_S(r.rt_key_duration_ns), 0.1);
    CHECK_I(r.response, 1);
    CHECK_S(r.response_name, "right");
    CHECK_I(r.control, SC_J);
    CHECK_I(r.code, 'j');
    CHECK_I(r.t_response, MS(1001));
    CHECK_I(r.t_onset, MS(601));
    CHECK_I(r.onset_tier, 1);
    CHECK_I(r.onset_src, YRSP_ONSET_FLIP);
    CHECK_I(r.onset_frame, 42);
    CHECK_I(r.onset_residual, 1234);
    CHECK_I(r.flags, YRSP_R_RESPONDED);
    CHECK_I(r.n_responses, 1);
    e = yrsp_entries(&g_c, &n);
    CHECK_I(n, 1);
    CHECK_I(e[0].flags, YRSP_E_VALID | YRSP_E_RELEASED | YRSP_E_ENDED);
    /* the name defaults to the key */
    yrsp_arm(&g_c, MS(2000));
    onset(MS(2100), 1);
    tap(MS(2400), SC_F);
    r = finish();
    CHECK_S(r.response_name, "f");
    CHECK_I(r.response, 0);
}

static void check_bounds(void) {
    yrsp_desc d = fj_desc(1.5, 0.1);
    yrsp_result r;
    const yrsp_entry* e;
    int n;
    CHECK(yrsp_open(&g_c, &d));
    d.persist = true;
    CHECK(yrsp_open(&g_c, &d));
    yrsp_arm(&g_c, 0);
    tap(MS(500), SC_F);                                  /* before the onset arrives: EARLY */
    onset(MS(1000), 1);
    tap(MS(1000) + MS(100) - 1, SC_F);                   /* 1 ns short of the minimum */
    tap(MS(1000) + MS(300), SC_J);
    tap(MS(1000) + MS(1500) - 1, SC_F);                  /* last ns of the window */
    tap(MS(1000) + MS(1500), SC_J);                      /* at the deadline: LATE */
    tap(MS(1000) - 1, SC_J);                             /* just before the onset */
    r = finish();
    e = yrsp_entries(&g_c, &n);
    CHECK_I(n, 6);
    CHECK_I(e[0].flags & YRSP_E_CLASS_MASK, YRSP_E_EARLY);
    CHECK_I(e[1].flags & YRSP_E_CLASS_MASK, YRSP_E_ANTICIPATION);
    CHECK_I(e[2].flags & YRSP_E_CLASS_MASK, YRSP_E_VALID);
    CHECK_I(e[3].flags & YRSP_E_CLASS_MASK, YRSP_E_VALID);
    CHECK_I(e[4].flags & YRSP_E_CLASS_MASK, YRSP_E_LATE);
    CHECK_I(e[5].flags & YRSP_E_CLASS_MASK, YRSP_E_EARLY);
    CHECK_I(r.n_responses, 2);
    CHECK_I(r.n_anticipations, 1);
    CHECK_I(r.n_early, 2);
    CHECK_I(r.n_late, 1);
    CHECK_D(YRT_TO_S(r.rt_ns), 0.3);
    /* exactly at the minimum is valid; and a negative rt is EARLY */
    d = fj_desc(1.5, 0.1);
    CHECK(yrsp_open(&g_c, &d));
    yrsp_arm(&g_c, 0);
    onset(MS(1000), 1);
    CHECK_I(key(MS(1100), SC_F, 1), YRSP_ENDED);
    r = finish();
    CHECK_D(YRT_TO_S(r.rt_ns), 0.1);
    /* duration 0: no deadline */
    d = fj_desc(0, 0);
    CHECK(yrsp_open(&g_c, &d));
    yrsp_arm(&g_c, 0);
    onset(MS(10), 1);
    CHECK_I(yrsp_update(&g_c, MS(100000)), YRSP_OPEN);
    CHECK_I(key(MS(90000), SC_F, 1), YRSP_ENDED);
    r = finish();
    CHECK_D(YRT_TO_S(r.rt_ns), 89.99);
    CHECK(!(r.flags & YRSP_R_TIMEOUT));
    /* no deadline and no response: no timeout either */
    key(MS(90100), SC_F, 0);
    yrsp_arm(&g_c, MS(200000));
    onset(MS(200010), 1);
    r = finish();
    CHECK_I(r.flags, 0);
}

static void check_deadline(void) {
    yrsp_desc d = fj_desc(1.0, 0);
    yrsp_result r;
    CHECK(yrsp_open(&g_c, &d));
    yrsp_arm(&g_c, 0);
    CHECK_I(yrsp_update(&g_c, MS(3000)), YRSP_OPEN);   /* no onset yet */
    onset(MS(500), 1);
    CHECK_I(yrsp_update(&g_c, MS(1500) - 1), YRSP_OPEN);
    CHECK_I(yrsp_update(&g_c, MS(1500)), YRSP_ENDED);
    /* a press stamped before the deadline, delivered after ENDED: valid */
    CHECK_I(key(MS(1499), SC_F, 1), YRSP_ENDED);
    key(MS(1550), SC_J, 1);                                 /* late */
    r = finish();
    CHECK_D(YRT_TO_S(r.rt_ns), 0.999);
    CHECK_I(r.n_late, 1);
    CHECK(r.flags & YRSP_R_RESPONDED);
    /* no response: timeout, NaN */
    CHECK(yrsp_open(&g_c, &d));
    yrsp_arm(&g_c, 0);
    onset(MS(500), 1);
    CHECK_I(yrsp_update(&g_c, MS(1600)), YRSP_ENDED);
    r = finish();
    CHECK(r.rt_ns == YRSP_NO_TIME);
    CHECK(r.rt_key_duration_ns == YRSP_NO_TIME);
    CHECK_I(r.response, -1);
    CHECK(r.response_name == NULL);
    CHECK_I(r.flags, YRSP_R_TIMEOUT);
    /* update after finish stays ENDED; a later press is logged, late */
    CHECK_I(yrsp_update(&g_c, MS(9999)), YRSP_ENDED);
    tap(MS(1700), SC_F);
    r = finish();
    CHECK_I(r.n_entries, 1);
    CHECK_I(r.n_late, 1);
    CHECK_I(r.n_responses, 0);
    /* finish() before any end: every later press is AFTER_END */
    CHECK(yrsp_open(&g_c, &d));
    yrsp_arm(&g_c, 0);
    onset(MS(500), 1);
    r = finish();
    CHECK_I(key(MS(700), SC_F, 1), YRSP_ENDED);
    r = finish();
    CHECK_I(r.n_after_end, 1);
    CHECK_I(r.n_responses, 0);
    /* a report delivered after finish(), stamped before the deadline */
    CHECK(yrsp_open(&g_c, &d));
    yrsp_arm(&g_c, 0);
    onset(MS(500), 1);
    yrsp_update(&g_c, MS(1600));
    r = finish();
    CHECK(r.flags & YRSP_R_TIMEOUT);
    key(MS(1400), SC_J, 1);
    r = finish();
    CHECK_D(YRT_TO_S(r.rt_ns), 0.9);
}

static void check_ending(void) {
    yrsp_desc d = fj_desc(2.0, 0.1);
    yrsp_result r;
    const yrsp_entry* e;
    int n;
    CHECK(yrsp_open(&g_c, &d));
    yrsp_arm(&g_c, 0);
    onset(MS(100), 1);
    CHECK_I(key(MS(150), SC_F, 1), YRSP_OPEN);           /* anticipation: does not end */
    key(MS(160), SC_F, 0);
    CHECK_I(key(MS(500), SC_J, 1), YRSP_ENDED);
    key(MS(600), SC_J, 0);
    tap(MS(700), SC_F);                                     /* after the end */
    r = finish();
    e = yrsp_entries(&g_c, &n);
    CHECK_I(n, 3);
    CHECK(e[2].flags & YRSP_E_AFTER_END);
    CHECK(!(e[2].flags & YRSP_E_VALID));
    CHECK_I(r.n_responses, 1);
    CHECK_I(r.n_after_end, 1);
    CHECK_I(r.n_anticipations, 1);
    CHECK_I(r.response, 1);
    /* persist: three responses, the first is reported */
    d.persist = true;
    CHECK(yrsp_open(&g_c, &d));
    yrsp_arm(&g_c, 0);
    onset(MS(100), 1);
    CHECK_I(tap(MS(400), SC_F), YRSP_OPEN);
    CHECK_I(tap(MS(800), SC_J), YRSP_OPEN);
    CHECK_I(tap(MS(1200), SC_F), YRSP_OPEN);
    r = finish();
    CHECK_I(r.n_responses, 3);
    CHECK_D(YRT_TO_S(r.rt_ns), 0.3);
    CHECK_I(r.response, 0);
    /* persist with max_responses 2: ends at the second */
    d.max_responses = 2;
    CHECK(yrsp_open(&g_c, &d));
    yrsp_arm(&g_c, 0);
    onset(MS(100), 1);
    CHECK_I(tap(MS(400), SC_F), YRSP_OPEN);
    CHECK_I(key(MS(800), SC_J, 1), YRSP_ENDED);
    tap(MS(1200), SC_F);
    r = finish();
    CHECK_I(r.n_responses, 2);
    CHECK_I(r.n_after_end, 1);
    e = yrsp_entries(&g_c, &n);
    CHECK(e[1].flags & YRSP_E_ENDED);
    /* max_responses 1 with persist is the default trial */
    d.max_responses = 1;
    CHECK(yrsp_open(&g_c, &d));
    yrsp_arm(&g_c, 0);
    onset(MS(100), 1);
    CHECK_I(key(MS(400), SC_F, 1), YRSP_ENDED);
}

static void check_modes(void) {
    yrsp_desc d;
    yrsp_result r;
    yrsp_input in;
    const yrsp_entry* e;
    int n;
    static const yrsp_choice kc[] = { { "z", NULL, 0, YRSP_MATCH_KEYCODE, 0, 0, 0, 0, 0, 0, 0 } };
    static const yrsp_choice boxes[] = { { NULL, "left", YRSP_KIND_BOX, 0, 0, 0, 2, 1, 0, 0, 0 },
                                           { NULL, "right", YRSP_KIND_BOX, 0, 0, 0, 0, 2, 0, 0, 0 } };
    /* ALL with kinds 0: the keyboard only, a click is no response */
    memset(&d, 0, sizeof d);
    CHECK(yrsp_open(&g_c, &d));
    yrsp_arm(&g_c, 0);
    onset(MS(100), 1);
    in = ev(YRSP_KIND_MOUSE, YRSP_PRESS, MS(200), 1);
    CHECK_I(yrsp_feed(&g_c, &in), YRSP_OPEN);
    CHECK_I(key(MS(300), 77, 1), YRSP_ENDED);
    r = finish();
    CHECK_I(r.n_not_choice, 1);
    CHECK_I(r.response, -1);
    CHECK_I(r.control, 77);
    CHECK_D(YRT_TO_S(r.rt_ns), 0.2);
    /* ALL with every kind */
    d.kinds = YRSP_KINDS_ALL;
    CHECK(yrsp_open(&g_c, &d));
    yrsp_arm(&g_c, 0);
    onset(MS(100), 1);
    CHECK_I(yrsp_feed(&g_c, &in), YRSP_ENDED);
    r = finish();
    CHECK_I(r.kind, YRSP_KIND_MOUSE);
    /* every kind but SYNC: a scanner pulse is logged, not a response */
    CHECK(yrsp_open(&g_c, &d));
    yrsp_arm(&g_c, 0);
    onset(MS(100), 1);
    in = ev(YRSP_KIND_SYNC, YRSP_PRESS, MS(150), 1);
    CHECK_I(yrsp_feed(&g_c, &in), YRSP_OPEN);
    in = ev(YRSP_KIND_MOUSE, YRSP_PRESS, MS(200), 1);
    CHECK_I(yrsp_feed(&g_c, &in), YRSP_ENDED);
    r = finish();
    CHECK_I(r.kind, YRSP_KIND_MOUSE);
    CHECK_I(r.n_not_choice, 1);
    /* the SYNC bit named: the pulse is the response (wait for the scanner) */
    d.kinds = YRSP_KIND_BIT(YRSP_KIND_SYNC);
    CHECK(yrsp_open(&g_c, &d));
    yrsp_arm(&g_c, 0);
    onset(MS(100), 1);
    CHECK_I(key(MS(120), SC_F, 1), YRSP_OPEN);
    in = ev(YRSP_KIND_SYNC, YRSP_PRESS, MS(150), 1);
    CHECK_I(yrsp_feed(&g_c, &in), YRSP_ENDED);
    r = finish();
    CHECK_I(r.kind, YRSP_KIND_SYNC);
    /* every bit but one named by hand is a mask, not "every kind" */
    d.kinds = (uint16_t)(YRSP_KINDS_ALL & ~YRSP_KIND_BIT(YRSP_KIND_PEN));
    CHECK(yrsp_open(&g_c, &d));
    yrsp_arm(&g_c, 0);
    onset(MS(100), 1);
    CHECK_I(yrsp_feed(&g_c, &in), YRSP_ENDED);
    in = ev(YRSP_KIND_MOUSE, YRSP_PRESS, MS(200), 1);
    /* one kind bit */
    d.kinds = YRSP_KIND_BIT(YRSP_KIND_MOUSE);
    CHECK(yrsp_open(&g_c, &d));
    yrsp_arm(&g_c, 0);
    onset(MS(100), 1);
    CHECK_I(key(MS(150), SC_F, 1), YRSP_OPEN);
    CHECK_I(yrsp_feed(&g_c, &in), YRSP_ENDED);
    /* NONE: logged, never a response, runs to the deadline */
    memset(&d, 0, sizeof d);
    d.mode = YRSP_CHOICES_NONE;
    d.duration_ns = YRT_S(1);
    CHECK(yrsp_open(&g_c, &d));
    yrsp_arm(&g_c, 0);
    onset(MS(100), 1);
    CHECK_I(tap(MS(300), SC_F), YRSP_OPEN);
    CHECK_I(yrsp_update(&g_c, MS(1100)), YRSP_ENDED);
    r = finish();
    CHECK_I(r.n_not_choice, 1);
    CHECK_I(r.flags, YRSP_R_TIMEOUT);
    /* KEYCODE: the layout's z is on the US y key (QWERTZ) */
    memset(&d, 0, sizeof d);
    d.choices = kc;
    d.n_choices = 1;
    CHECK(yrsp_open(&g_c, &d));
    yrsp_arm(&g_c, 0);
    onset(MS(100), 1);
    in = ev(YRSP_KIND_KEYBOARD, YRSP_PRESS, MS(200), 29);   /* US z key, types y */
    in.code = 'y';
    CHECK_I(yrsp_feed(&g_c, &in), YRSP_OPEN);
    in = ev(YRSP_KIND_KEYBOARD, YRSP_PRESS, MS(300), 28);   /* US y key, types z */
    in.code = 'z';
    CHECK_I(yrsp_feed(&g_c, &in), YRSP_ENDED);
    r = finish();
    CHECK_I(r.control, 28);
    CHECK_I(r.code, 'z');
    CHECK_S(r.response_name, "z");
    /* scancode (default): the US f key whatever it types */
    d = fj_desc(1, 0);
    CHECK(yrsp_open(&g_c, &d));
    yrsp_arm(&g_c, 0);
    onset(MS(100), 1);
    in = ev(YRSP_KIND_KEYBOARD, YRSP_PRESS, MS(200), SC_F);
    in.code = 'u';                                              /* Dvorak */
    in.mods = 0x0001;
    CHECK_I(yrsp_feed(&g_c, &in), YRSP_ENDED);
    e = yrsp_entries(&g_c, &n);
    CHECK_I(e[0].code, 'u');
    CHECK_I(e[0].control, SC_F);
    CHECK_I(e[0].mods, 1);
    /* box buttons with a device filter */
    memset(&d, 0, sizeof d);
    d.choices = boxes;
    d.n_choices = 2;
    CHECK(yrsp_open(&g_c, &d));
    yrsp_arm(&g_c, 0);
    onset(MS(100), 1);
    in = ev(YRSP_KIND_BOX, YRSP_PRESS, MS(200), 1);
    in.device = 3;                                              /* button 1 of the wrong box */
    CHECK_I(yrsp_feed(&g_c, &in), YRSP_OPEN);
    in = ev(YRSP_KIND_BOX, YRSP_PRESS, MS(300), 2);
    in.device = 3;                                              /* button 2, any box */
    CHECK_I(yrsp_feed(&g_c, &in), YRSP_ENDED);
    r = finish();
    CHECK_S(r.response_name, "right");
    CHECK_I(r.n_not_choice, 1);
    CHECK_I(r.device, 3);
}

static void check_held(void) {
    yrsp_desc d = fj_desc(2.0, 0);
    yrsp_result r;
    const yrsp_entry* e;
    int n;
    CHECK(yrsp_open(&g_c, &d));
    key(MS(10), SC_F, 1);                                  /* down before the window */
    yrsp_arm(&g_c, MS(100));
    onset(MS(200), 1);
    CHECK_I(key_repeat(MS(600), SC_F), YRSP_OPEN);       /* its OS repeats: no response */
    CHECK_I(key_repeat(MS(633), SC_F), YRSP_OPEN);
    CHECK_I(key(MS(700), SC_F, 1), YRSP_OPEN);           /* R2: down again, no up: held */
    key(MS(800), SC_F, 0);
    CHECK_I(key(MS(900), SC_F, 1), YRSP_ENDED);          /* a new press */
    r = finish();
    CHECK(r.flags & YRSP_R_HELD_AT_OPEN);
    CHECK_I(r.n_held, 3);
    CHECK_D(YRT_TO_S(r.rt_ns), 0.7);
    /* a repeat whose press was never seen (the window got the focus while
     * the key was down) is held too */
    CHECK(yrsp_open(&g_c, &d));
    yrsp_arm(&g_c, MS(100));
    onset(MS(200), 1);
    CHECK_I(key_repeat(MS(600), SC_F), YRSP_OPEN);
    r = finish();
    CHECK_I(r.n_held, 1);
    CHECK_I(r.n_entries, 0);
    /* allow_held_key: the press is the response; its repeats are the same
     * press, and the release closes them all */
    d.allow_held_key = true;
    d.persist = true;
    CHECK(yrsp_open(&g_c, &d));
    yrsp_arm(&g_c, MS(100));
    onset(MS(200), 1);
    key(MS(500), SC_F, 1);
    key_repeat(MS(1000), SC_F);
    key_repeat(MS(1033), SC_F);
    key(MS(1200), SC_F, 0);
    r = finish();
    CHECK_D(YRT_TO_S(r.rt_ns), 0.3);
    CHECK_D(YRT_TO_S(r.rt_key_duration_ns), 0.7);
    e = yrsp_entries(&g_c, &n);
    CHECK_I(n, 3);
    CHECK_I(e[2].t_release, MS(1200));
    d.persist = false;
    /* allow_held_key: the first repeat is the response, flagged HELD */
    d.allow_held_key = true;
    CHECK(yrsp_open(&g_c, &d));
    key(MS(10), SC_F, 1);
    yrsp_arm(&g_c, MS(100));
    onset(MS(200), 1);
    CHECK_I(key_repeat(MS(600), SC_F), YRSP_ENDED);
    r = finish();
    e = yrsp_entries(&g_c, &n);
    CHECK(e[0].flags & YRSP_E_HELD);
    CHECK_D(YRT_TO_S(r.rt_ns), 0.4);
    CHECK(r.flags & YRSP_R_HELD_AT_OPEN);
    /* a held key that is no choice: not logged without allow_held_key */
    d.allow_held_key = false;
    CHECK(yrsp_open(&g_c, &d));
    key(MS(10), SC_K, 1);
    yrsp_arm(&g_c, MS(100));
    onset(MS(200), 1);
    key_repeat(MS(600), SC_K);
    r = finish();
    CHECK_I(r.n_entries, 0);
    CHECK(!(r.flags & YRSP_R_HELD_AT_OPEN));
    /* the held state survives arm(): released before the next window */
    CHECK(yrsp_open(&g_c, &d));
    key(MS(10), SC_J, 1);
    key(MS(50), SC_J, 0);
    yrsp_arm(&g_c, MS(100));
    onset(MS(200), 1);
    CHECK_I(key(MS(400), SC_J, 1), YRSP_ENDED);
    r = finish();
    CHECK(!(r.flags & YRSP_R_HELD_AT_OPEN));
    /* a box button held across windows */
    {
        static const yrsp_choice b1[] = { { NULL, NULL, YRSP_KIND_BOX, 0, 0, 0, 0, 1, 0, 0, 0 } };
        yrsp_input in = ev(YRSP_KIND_BOX, YRSP_PRESS, MS(10), 1);
        memset(&d, 0, sizeof d);
        d.choices = b1;
        d.n_choices = 1;
        CHECK(yrsp_open(&g_c, &d));
        in.device = 7;
        yrsp_feed(&g_c, &in);
        yrsp_arm(&g_c, MS(100));
        r = finish();
        CHECK(r.flags & YRSP_R_HELD_AT_OPEN);
        yrsp_arm(&g_c, MS(200));
        onset(MS(300), 1);
        in.t = MS(400);
        CHECK_I(yrsp_feed(&g_c, &in), YRSP_OPEN);        /* held */
        in.device = 8;                                        /* another box's button 1 */
        in.t = MS(500);
        CHECK_I(yrsp_feed(&g_c, &in), YRSP_ENDED);
    }
}

/* v0.2.0: second reports are the producer's to drop (ysp/screen.h's
 * bridge); fed to the collector, the measured patterns of SDL 3.4's double
 * report are what R1 and R2 make of them. */
static void check_double(void) {
    yrsp_desc d = fj_desc(2.0, 0);
    yrsp_result r;
    const yrsp_entry* e;
    int n;
    /* a virtual-key tap, down up (raw), down up (message) 9 ms later: two
     * presses; the second is AFTER_END, as the first ended the window */
    CHECK(yrsp_open(&g_c, &d));
    yrsp_arm(&g_c, 0);
    onset(MS(100), 1);
    key(MS(500), SC_F, 1);
    key(MS(500) + 30000, SC_F, 0);
    key(MS(509), SC_F, 1);
    key(MS(509) + 30000, SC_F, 0);
    r = finish();
    e = yrsp_entries(&g_c, &n);
    CHECK_I(n, 2);
    CHECK_I(r.n_responses, 1);
    CHECK_I(r.n_after_end, 1);
    CHECK_D(YRT_TO_S(r.rt_ns), 0.4);
    CHECK_D(YRT_TO_S(r.rt_key_duration_ns), 0.00003);
    CHECK(e[1].flags & YRSP_E_RELEASED);
    /* with persist, two responses: the filter belongs before yrsp_feed() */
    d.persist = true;
    CHECK(yrsp_open(&g_c, &d));
    yrsp_arm(&g_c, 0);
    onset(MS(100), 1);
    key(MS(500), SC_F, 1);
    key(MS(500) + 30000, SC_F, 0);
    key(MS(509), SC_F, 1);
    key(MS(509) + 30000, SC_F, 0);
    r = finish();
    CHECK_I(r.n_responses, 2);
    /* down, down, up, up: the second down is held (R2), one up closes */
    CHECK(yrsp_open(&g_c, &d));
    yrsp_arm(&g_c, 0);
    onset(MS(100), 1);
    key(MS(500), SC_F, 1);
    key(MS(495), SC_F, 1);
    key(MS(600), SC_F, 0);
    key(MS(601), SC_F, 0);
    r = finish();
    CHECK_I(r.n_responses, 1);
    CHECK_I(r.n_held, 1);
    CHECK_D(YRT_TO_S(r.rt_ns), 0.4);
    CHECK_D(YRT_TO_S(r.rt_key_duration_ns), 0.1);
    CHECK_I(g_c.n_stray, 1);
    /* a 100 ms hold: SDL marks the second report a repeat */
    CHECK(yrsp_open(&g_c, &d));
    yrsp_arm(&g_c, 0);
    onset(MS(100), 1);
    key(MS(500), SC_F, 1);
    key_repeat(MS(511), SC_F);
    key(MS(600), SC_F, 0);
    r = finish();
    CHECK_I(r.n_responses, 1);
    CHECK_I(r.n_held, 1);
    /* two different keys 2 ms apart both count */
    CHECK(yrsp_open(&g_c, &d));
    yrsp_arm(&g_c, 0);
    onset(MS(100), 1);
    key(MS(500), SC_F, 1);
    key(MS(502), SC_J, 1);
    r = finish();
    CHECK_I(r.n_responses, 2);
}

static int dkey(int64_t t, uint32_t sc, uint32_t dev, int type, int repeat) {
    yrsp_input in = ev(YRSP_KIND_KEYBOARD, type, t, sc);
    in.device = dev;
    if (repeat) in.flags = YRSP_IN_REPEAT;
    return yrsp_feed(&g_c, &in);
}

/* v0.1.1: two keyboards are two devices; a raw report and its message
 * report (device 0) are one press. */
static void check_devices(void) {
    yrsp_desc d = fj_desc(5.0, 0);
    yrsp_result r;
    const yrsp_entry* e;
    int n;
    static const yrsp_choice only22[] = { { "f", NULL, 0, 0, 0, 0, 22, 0, 0, 0, 0 } };
    d.persist = true;
    /* two keyboards press F 5 ms apart: two responses */
    CHECK(yrsp_open(&g_c, &d));
    yrsp_arm(&g_c, 0);
    onset(MS(100), 1);
    dkey(MS(500), SC_F, 11, YRSP_PRESS, 0);
    dkey(MS(505), SC_F, 22, YRSP_PRESS, 0);
    r = finish();
    CHECK_I(r.n_responses, 2);
    /* as SDL gives it: B's press comes as a repeat while A holds F */
    CHECK(yrsp_open(&g_c, &d));
    yrsp_arm(&g_c, 0);
    onset(MS(100), 1);
    dkey(MS(1000), SC_F, 11, YRSP_PRESS, 0);
    dkey(MS(1005), SC_F, 22, YRSP_PRESS, 1);
    dkey(MS(1100), SC_F, 22, YRSP_RELEASE, 0);
    r = finish();
    e = yrsp_entries(&g_c, &n);
    CHECK_I(r.n_responses, 2);
    CHECK_I(n, 2);
    CHECK_I(e[0].t_release, 0);              /* SDL dropped A's release */
    CHECK_I(e[1].t_release, MS(1100));
    CHECK_I(e[1].device, 22);
    /* A's own OS repeat while A holds: held */
    CHECK(yrsp_open(&g_c, &d));
    yrsp_arm(&g_c, 0);
    onset(MS(100), 1);
    dkey(MS(3000), SC_F, 11, YRSP_PRESS, 0);
    dkey(MS(3500), SC_F, 11, YRSP_PRESS, 1);
    r = finish();
    CHECK_I(r.n_responses, 1);
    CHECK_I(r.n_held, 1);
    /* a raw report (the keyboard's id) and its message report (0) while
     * the first is down: device 0 matches it, so held (R2) */
    CHECK(yrsp_open(&g_c, &d));
    yrsp_arm(&g_c, 0);
    onset(MS(100), 1);
    dkey(MS(2000), SC_F, 11, YRSP_PRESS, 0);
    dkey(MS(2009), SC_F, 0, YRSP_PRESS, 0);
    dkey(MS(2050), SC_F, 11, YRSP_RELEASE, 0);
    r = finish();
    CHECK_I(r.n_responses, 1);
    CHECK_I(r.n_held, 1);
    CHECK_I(r.device, 11);
    CHECK_D(YRT_TO_S(r.rt_key_duration_ns), 0.05);
    /* releases per device */
    CHECK(yrsp_open(&g_c, &d));
    yrsp_arm(&g_c, 0);
    onset(MS(100), 1);
    dkey(MS(4000), SC_J, 11, YRSP_PRESS, 0);
    dkey(MS(4010), SC_J, 22, YRSP_PRESS, 0);
    dkey(MS(4100), SC_J, 22, YRSP_RELEASE, 0);
    e = yrsp_entries(&g_c, &n);
    CHECK_I(n, 2);
    CHECK_I(e[0].t_release, 0);
    CHECK_I(e[1].t_release, MS(4100));
    dkey(MS(4200), SC_J, 11, YRSP_RELEASE, 0);
    CHECK_I(e[0].t_release, MS(4200));
    /* a choice on keyboard 22: held at open only when 22 holds it */
    memset(&d, 0, sizeof d);
    d.choices = only22;
    d.n_choices = 1;
    CHECK(yrsp_open(&g_c, &d));
    dkey(MS(10), SC_F, 11, YRSP_PRESS, 0);
    yrsp_arm(&g_c, MS(100));
    onset(MS(200), 1);
    CHECK_I(dkey(MS(300), SC_F, 11, YRSP_PRESS, 1), YRSP_OPEN);   /* keyboard 11: no choice */
    CHECK_I(dkey(MS(400), SC_F, 22, YRSP_PRESS, 1), YRSP_ENDED);  /* 22's first press */
    r = finish();
    CHECK(!(r.flags & YRSP_R_HELD_AT_OPEN));
    CHECK_I(r.device, 22);
    CHECK(yrsp_open(&g_c, &d));
    dkey(MS(10), SC_F, 22, YRSP_PRESS, 0);
    yrsp_arm(&g_c, MS(100));
    r = finish();
    CHECK(r.flags & YRSP_R_HELD_AT_OPEN);
}

/* v0.1.2: ysp/screen.h's raw mouse reports, per device; the cursor. */
static void check_raw_mice(void) {
    yscr_mouse_event m;
    yrsp_input in[12];
    yrsp_cursor cur;
    yrsp_source src;
    yrsp_desc d;
    yrsp_result r;
    int n, i;
    static const yrsp_choice left77[] = { { NULL, "left", YRSP_KIND_MOUSE, 0, 0, 0, 0x77, 1, 0, 0, 0 } };
    memset(&m, 0, sizeof m);
    m.t = MS(500);
    m.device = 0x77;
    m.down = YSCR_MOUSE_LEFT | YSCR_MOUSE_RIGHT;
    m.up = YSCR_MOUSE_X2;
    m.dx = 5;
    m.dy = -2;
    m.wheel = -120;
    m.hwheel = 240;
    m.flags = YSCR_MOUSE_UNLISTED;
    n = yrsp_from_mouse(&m, in, 12);
    CHECK_I(n, 5);
    CHECK_I(in[0].type, YRSP_PRESS);
    CHECK_I(in[0].control, 1);
    CHECK_I(in[1].type, YRSP_PRESS);
    CHECK_I(in[1].control, 3);
    CHECK_I(in[2].type, YRSP_RELEASE);
    CHECK_I(in[2].control, 5);
    CHECK_I(in[3].type, YRSP_SAMPLE);
    CHECK_I(in[3].control, YRSP_AXIS_DELTA);
    CHECK(in[3].x == 5.0f && in[3].y == -2.0f);
    CHECK_I(in[4].control, YRSP_AXIS_WHEEL);
    CHECK(in[4].value == -1.0f && in[4].x == 2.0f);
    for (i = 0; i < n; i++) {
        CHECK_I(in[i].kind, YRSP_KIND_MOUSE);
        CHECK_I(in[i].device, 0x77);
        CHECK_I(in[i].t, MS(500));
        CHECK_I(in[i].flags, YRSP_IN_UNLISTED);
    }
    CHECK_I(yrsp_from_mouse(&m, in, 2), 2);
    memset(&m, 0, sizeof m);
    m.down = YSCR_MOUSE_MIDDLE | YSCR_MOUSE_X1;
    n = yrsp_from_mouse(&m, in, 12);
    CHECK_I(n, 2);
    CHECK_I(in[0].control, 2);
    CHECK_I(in[1].control, 4);
    memset(&m, 0, sizeof m);
    CHECK_I(yrsp_from_mouse(&m, in, 12), 0);
    m.flags = YSCR_MOUSE_ABSOLUTE;
    m.dx = 65535;
    n = yrsp_from_mouse(&m, in, 12);
    CHECK_I(n, 1);
    CHECK_I(in[0].control, YRSP_AXIS_ABSOLUTE);
    CHECK(in[0].x == 1.0f && in[0].y == 0.0f);
    /* a choice on one mouse: the other mouse's click is no choice */
    memset(&d, 0, sizeof d);
    d.choices = left77;
    d.n_choices = 1;
    CHECK(yrsp_open(&g_c, &d));
    yrsp_arm(&g_c, 0);
    onset(MS(100), 1);
    memset(&m, 0, sizeof m);
    m.device = 0x88;
    m.down = YSCR_MOUSE_LEFT;
    m.t = MS(300);
    n = yrsp_from_mouse(&m, in, 12);
    CHECK_I(yrsp_feed(&g_c, &in[0]), YRSP_OPEN);
    m.device = 0x77;
    m.t = MS(350);
    n = yrsp_from_mouse(&m, in, 12);
    CHECK_I(yrsp_feed(&g_c, &in[0]), YRSP_ENDED);
    r = finish();
    CHECK_I(r.device, 0x77);
    CHECK_I(r.n_not_choice, 1);
    CHECK_D(YRT_TO_S(r.rt_ns), 0.25);
    /* the cursor: a gain, clamped, one device */
    memset(&cur, 0, sizeof cur);
    cur.device = 0x77;
    cur.gain = 2;
    cur.w = 100;
    cur.h = 50;
    cur.x = cur.y = 10;
    memset(&m, 0, sizeof m);
    m.device = 0x77;
    m.dx = 5;
    m.dy = -10;
    yrsp_from_mouse(&m, in, 12);
    CHECK_I(yrsp_cursor_feed(&cur, &in[0]), 1);
    CHECK(cur.x == 20.0f && cur.y == 0.0f);
    m.dx = 100;
    m.dy = 100;
    yrsp_from_mouse(&m, in, 12);
    yrsp_cursor_feed(&cur, &in[0]);
    CHECK(cur.x == 100.0f && cur.y == 50.0f);
    m.device = 0x88;
    m.dx = -50;
    yrsp_from_mouse(&m, in, 12);
    CHECK_I(yrsp_cursor_feed(&cur, &in[0]), 0);
    cur.device = 0;
    cur.gain = 0;                              /* 0 = 1 */
    CHECK_I(yrsp_cursor_feed(&cur, &in[0]), 1);
    CHECK(cur.x == 50.0f);
    m.flags = YSCR_MOUSE_ABSOLUTE;
    yrsp_from_mouse(&m, in, 12);
    CHECK_I(yrsp_cursor_feed(&cur, &in[0]), 0);
    CHECK_I(yrsp_sdl_mouse_stamp(0), YRSP_TIER_3);
    CHECK_I(yrsp_sdl_mouse_stamp(65661), 0);
    src = yrsp_raw_mouse_source();
    CHECK_I(src.kind, YRSP_KIND_MOUSE);
    CHECK_I(src.partial, 1);
    CHECK(src.note != NULL);
}

static void check_onset_refine(void) {
    yrsp_desc d = fj_desc(1.0, 0.1);
    yrsp_result r;
    const yrsp_entry* e;
    int n;
    CHECK(yrsp_open(&g_c, &d));
    /* the plan, then the final onset a frame later */
    yrsp_arm(&g_c, 0);
    onset(MS(1000), 0);
    CHECK_I(key(MS(1400), SC_F, 1), YRSP_ENDED);
    onset(MS(1017), 1);
    r = finish();
    CHECK_D(YRT_TO_S(r.rt_ns), 0.383);
    CHECK(!(r.flags & YRSP_R_ONSET_PLAN));
    CHECK(!(r.flags & YRSP_R_RECLASSIFIED));
    /* a plan does not replace the final onset */
    CHECK(yrsp_open(&g_c, &d));
    yrsp_arm(&g_c, 0);
    onset(MS(1017), 1);
    onset(MS(1000), 0);
    key(MS(1400), SC_F, 1);
    r = finish();
    CHECK_I(r.t_onset, MS(1017));
    /* the press that ended the window becomes an anticipation */
    CHECK(yrsp_open(&g_c, &d));
    yrsp_arm(&g_c, 0);
    onset(MS(1000), 0);
    CHECK_I(key(MS(1105), SC_F, 1), YRSP_ENDED);
    tap(MS(1300), SC_J);                                   /* after the end */
    onset(MS(1017), 1);
    r = finish();
    e = yrsp_entries(&g_c, &n);
    CHECK(e[0].flags & YRSP_E_ANTICIPATION);
    CHECK(e[0].flags & YRSP_E_RECLASSIFIED);
    CHECK(e[0].flags & YRSP_E_ENDED);
    CHECK(r.flags & YRSP_R_ENDED_EARLY);
    CHECK(r.flags & YRSP_R_RECLASSIFIED);
    CHECK(!(r.flags & YRSP_R_RESPONDED));
    CHECK(r.rt_ns == YRSP_NO_TIME);
    /* only a plan: flagged */
    CHECK(yrsp_open(&g_c, &d));
    yrsp_arm(&g_c, 0);
    onset(MS(1000), 0);
    key(MS(1400), SC_F, 1);
    r = finish();
    CHECK(r.flags & YRSP_R_ONSET_PLAN);
    CHECK_I(r.onset_src, YRSP_ONSET_PLAN);
    CHECK_D(YRT_TO_S(r.rt_ns), 0.4);
    /* no onset at all */
    CHECK(yrsp_open(&g_c, &d));
    yrsp_arm(&g_c, 0);
    key(MS(1400), SC_J, 1);
    r = finish();
    CHECK(r.flags & YRSP_R_NO_ONSET);
    CHECK(!(r.flags & YRSP_R_RESPONDED));
    CHECK_I(r.n_early, 1);
    /* an onset given after a press that falls in its window: ends then */
    CHECK(yrsp_open(&g_c, &d));
    yrsp_arm(&g_c, 0);
    key(MS(1400), SC_J, 1);
    CHECK_I(onset(MS(1000), 1), YRSP_ENDED);
    r = finish();
    CHECK_D(YRT_TO_S(r.rt_ns), 0.4);
    CHECK(r.flags & YRSP_R_RECLASSIFIED);
    /* set_onset before arm is ignored */
    CHECK(yrsp_open(&g_c, &d));
    CHECK_I(onset(MS(1), 1), YRSP_IDLE);
}

static void check_out_of_order(void) {
    static const yrsp_choice ch[] = { { "f", NULL, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
                                        { NULL, "box", YRSP_KIND_BOX, 0, 0, 0, 0, 1, 0, 0, 0 } };
    yrsp_desc d;
    yrsp_result r;
    yrsp_input in;
    memset(&d, 0, sizeof d);
    d.choices = ch;
    d.n_choices = 2;
    d.duration_ns = YRT_S(2);
    CHECK(yrsp_open(&g_c, &d));
    yrsp_arm(&g_c, 0);
    onset(MS(100), 1);
    CHECK_I(key(MS(500), SC_F, 1), YRSP_ENDED);
    in = ev(YRSP_KIND_BOX, YRSP_PRESS, MS(450), 1);   /* read late, stamped earlier */
    yrsp_feed(&g_c, &in);
    r = finish();
    CHECK_S(r.response_name, "box");
    CHECK_D(YRT_TO_S(r.rt_ns), 0.35);
    CHECK_I(r.n_responses, 2);
    CHECK_I(r.n_after_end, 0);
    /* persist with max 1 arriving out of order before the end: the earliest ends */
    d.persist = true;
    d.max_responses = 2;
    CHECK(yrsp_open(&g_c, &d));
    yrsp_arm(&g_c, 0);
    key(MS(600), SC_F, 1);                                 /* before the onset is known */
    key(MS(700), SC_F, 0);
    in.t = MS(300);
    yrsp_feed(&g_c, &in);
    CHECK_I(onset(MS(100), 1), YRSP_ENDED);
    r = finish();
    CHECK_I(r.n_responses, 2);
    CHECK_S(r.response_name, "box");
}

static void check_release(void) {
    yrsp_desc d = fj_desc(2.0, 0);
    yrsp_result r;
    d.wait_for_key_release = true;
    d.max_hold_ns = YRT_MS(500);
    CHECK(yrsp_open(&g_c, &d));
    yrsp_arm(&g_c, 0);
    onset(MS(100), 1);
    CHECK_I(key(MS(400), SC_F, 1), YRSP_OPEN);           /* waits for the release */
    CHECK_I(key(MS(450), SC_J, 1), YRSP_OPEN);           /* after the end */
    CHECK_I(yrsp_update(&g_c, MS(600)), YRSP_OPEN);     /* not yet max_hold */
    CHECK_I(key(MS(650), SC_F, 0), YRSP_ENDED);
    r = finish();
    CHECK_D(YRT_TO_S(r.rt_ns), 0.3);
    CHECK_D(YRT_TO_S(r.rt_key_duration_ns), 0.25);
    CHECK_I(r.n_after_end, 1);
    CHECK(!(r.flags & YRSP_R_HOLD_TIMEOUT));
    /* max_hold gives up */
    CHECK(yrsp_open(&g_c, &d));
    yrsp_arm(&g_c, 0);
    onset(MS(100), 1);
    CHECK_I(key(MS(400), SC_F, 1), YRSP_OPEN);
    CHECK_I(yrsp_update(&g_c, MS(899)), YRSP_OPEN);
    CHECK_I(yrsp_update(&g_c, MS(900)), YRSP_ENDED);
    r = finish();
    CHECK(r.flags & YRSP_R_HOLD_TIMEOUT);
    CHECK(r.rt_key_duration_ns == YRSP_NO_TIME);
    /* a release after finish() pairs; finish() again reports it */
    key(MS(1000), SC_F, 0);
    r = finish();
    CHECK_D(YRT_TO_S(r.rt_key_duration_ns), 0.6);
    /* the default max_hold is 2 s */
    d.max_hold_ns = 0;
    CHECK(yrsp_open(&g_c, &d));
    CHECK_I(g_c.max_hold_ns, MS(2000));
    /* the release pairs with the newest press of that key */
    d = fj_desc(2.0, 0);
    d.persist = true;
    CHECK(yrsp_open(&g_c, &d));
    yrsp_arm(&g_c, 0);
    onset(MS(100), 1);
    tap(MS(300), SC_F);
    key(MS(600), SC_F, 1);
    key(MS(610), SC_J, 1);
    key(MS(650), SC_J, 0);
    key(MS(900), SC_F, 0);
    r = finish();
    {
        int n;
        const yrsp_entry* e = yrsp_entries(&g_c, &n);
        CHECK_I(n, 3);
        CHECK_I(e[0].t_release, MS(380));
        CHECK_I(e[1].t_release, MS(900));
        CHECK_I(e[2].t_release, MS(650));
    }
    /* a stray release (never down) is dropped */
    key(MS(950), SC_K, 0);
    CHECK_I(g_c.n_stray, 1);
}

static void check_crossings(void) {
    static const yrsp_choice trig[] = {
        { NULL, "trigger", YRSP_KIND_GAMEPAD, 0, YRSP_CROSS_RISING, 0, 0, YRSP_AXIS_GAMEPAD + 5, 0, 0.5f, 0.2f } };
    static const yrsp_choice fall[] = {
        { NULL, "stick", YRSP_KIND_GAMEPAD, 0, YRSP_CROSS_FALLING, 0, 0, YRSP_AXIS_GAMEPAD + 1, 0, -0.5f, 0.0f } };
    static const yrsp_choice move[] = {
        { NULL, "move", YRSP_KIND_MOUSE, 0, YRSP_CROSS_DISTANCE, 0, 0, YRSP_AXIS_POSITION, 0, 10.0f, 0.0f } };
    static const yrsp_choice pen[] = {
        { NULL, "pen", YRSP_KIND_PEN, 0, YRSP_CROSS_RISING, 0, 0, YRSP_AXIS_PEN_PRESSURE, 0, 0.05f, 0.0f } };
    yrsp_desc d;
    yrsp_result r;
    const yrsp_entry* e;
    int n;
    const int G = YRSP_KIND_GAMEPAD;
    const uint32_t RT = YRSP_AXIS_GAMEPAD + 5;
    memset(&d, 0, sizeof d);
    d.choices = trig;
    d.n_choices = 1;
    d.duration_ns = YRT_S(2);
    d.persist = true;
    CHECK(yrsp_open(&g_c, &d));
    yrsp_arm(&g_c, 0);
    onset(MS(100), 1);
    sample(G, MS(200), RT, 0.1f, 0, 0);
    sample(G, MS(300), RT, 0.49f, 0, 0);
    sample(G, MS(400), RT, 0.5f, 0, 0);                    /* crossing, at this sample */
    sample(G, MS(410), RT, 0.45f, 0, 0);                   /* jitter around the level: no re-arm */
    sample(G, MS(420), RT, 0.55f, 0, 0);
    sample(G, MS(500), RT, 0.29f, 0, 0);                   /* below 0.3: re-armed */
    sample(G, MS(600), RT, 0.6f, 0, 0);                    /* the second crossing */
    sample(G, MS(610), RT, 0.6f, 0, 0);
    sample(G + 0, MS(620), RT - 1, 0.9f, 0, 0);           /* another axis */
    r = finish();
    e = yrsp_entries(&g_c, &n);
    CHECK_I(n, 2);
    CHECK(e[0].flags & YRSP_E_CROSSING);
    CHECK_I(e[0].t, MS(400));
    CHECK_I(e[1].t, MS(600));
    CHECK_D(YRT_TO_S(r.rt_ns), 0.3);
    CHECK_S(r.response_name, "trigger");
    CHECK(r.rt_key_duration_ns == YRSP_NO_TIME);
    /* past the level at arm: held, until it re-arms */
    d.persist = false;
    CHECK(yrsp_open(&g_c, &d));
    sample(G, MS(10), RT, 0.8f, 0, 0);
    yrsp_arm(&g_c, MS(50));
    onset(MS(100), 1);
    CHECK_I(sample(G, MS(200), RT, 0.9f, 0, 0), YRSP_OPEN);
    CHECK_I(sample(G, MS(300), RT, 0.1f, 0, 0), YRSP_OPEN);
    CHECK_I(sample(G, MS(400), RT, 0.7f, 0, 0), YRSP_ENDED);
    r = finish();
    CHECK(r.flags & YRSP_R_HELD_AT_OPEN);
    CHECK_D(YRT_TO_S(r.rt_ns), 0.3);
    /* the end needs a crossing, not a release, with wait_for_key_release */
    d.wait_for_key_release = true;
    CHECK(yrsp_open(&g_c, &d));
    yrsp_arm(&g_c, 0);
    onset(MS(100), 1);
    CHECK_I(sample(G, MS(400), RT, 1.0f, 0, 0), YRSP_ENDED);
    /* falling */
    memset(&d, 0, sizeof d);
    d.choices = fall;
    d.n_choices = 1;
    CHECK(yrsp_open(&g_c, &d));
    yrsp_arm(&g_c, 0);
    onset(MS(100), 1);
    CHECK_I(sample(G, MS(200), YRSP_AXIS_GAMEPAD + 1, -0.4f, 0, 0), YRSP_OPEN);
    CHECK_I(sample(G, MS(300), YRSP_AXIS_GAMEPAD + 1, -0.5f, 0, 0), YRSP_ENDED);
    /* movement onset: 10 px from the position at arm */
    memset(&d, 0, sizeof d);
    d.choices = move;
    d.n_choices = 1;
    CHECK(yrsp_open(&g_c, &d));
    sample(YRSP_KIND_MOUSE, MS(10), 0, 0, 100, 100);
    yrsp_arm(&g_c, MS(50));
    onset(MS(100), 1);
    CHECK_I(sample(YRSP_KIND_MOUSE, MS(200), 0, 0, 106, 107.9f), YRSP_OPEN);
    CHECK_I(sample(YRSP_KIND_MOUSE, MS(300), 0, 0, 106, 108), YRSP_ENDED);
    r = finish();
    CHECK_D(YRT_TO_S(r.rt_ns), 0.2);
    e = yrsp_entries(&g_c, &n);
    CHECK(fabs(e[0].value - 10.0f) < 1e-4);
    /* no position before arm: the first sample after it is the reference */
    CHECK(yrsp_open(&g_c, &d));
    yrsp_arm(&g_c, MS(50));
    onset(MS(100), 1);
    CHECK_I(sample(YRSP_KIND_MOUSE, MS(150), 0, 0, 500, 500), YRSP_OPEN);
    CHECK_I(sample(YRSP_KIND_MOUSE, MS(200), 0, 0, 509, 500), YRSP_OPEN);
    CHECK_I(sample(YRSP_KIND_MOUSE, MS(250), 0, 0, 500, 490), YRSP_ENDED);
    /* pen pressure onset from no sample: the first contact crosses */
    memset(&d, 0, sizeof d);
    d.choices = pen;
    d.n_choices = 1;
    CHECK(yrsp_open(&g_c, &d));
    yrsp_arm(&g_c, 0);
    onset(MS(100), 1);
    CHECK_I(sample(YRSP_KIND_PEN, MS(350), YRSP_AXIS_PEN_PRESSURE, 0.3f, 10, 10), YRSP_ENDED);
    /* crossings in ALL mode do not exist */
    memset(&d, 0, sizeof d);
    d.kinds = YRSP_KINDS_ALL;
    CHECK(yrsp_open(&g_c, &d));
    yrsp_arm(&g_c, 0);
    onset(MS(100), 1);
    CHECK_I(sample(G, MS(400), RT, 1.0f, 0, 0), YRSP_OPEN);
}

static void check_trace(void) {
    static yrsp_input tr[5];
    yrsp_desc d = fj_desc(2.0, 0);
    yrsp_result r;
    int i;
    d.trace = tr;
    d.trace_cap = 5;
    d.trace_kinds = YRSP_KIND_BIT(YRSP_KIND_MOUSE);
    CHECK(yrsp_open(&g_c, &d));
    sample(YRSP_KIND_MOUSE, MS(1), 0, 0, 1, 1);           /* before arm: not traced */
    yrsp_arm(&g_c, MS(10));
    for (i = 0; i < 4; i++) sample(YRSP_KIND_MOUSE, MS(20 + i), 0, 0, (float)i, 0);
    sample(YRSP_KIND_PEN, MS(30), 0, 0, 0, 0);            /* another kind */
    key(MS(40), SC_K, 1);
    r = finish();
    CHECK_I(r.n_trace, 4);
    CHECK(!(r.flags & YRSP_R_TRACE_FULL));
    CHECK_I(tr[3].t, MS(23));
    CHECK(tr[3].x == 3.0f);
    yrsp_arm(&g_c, MS(100));
    for (i = 0; i < 7; i++) sample(YRSP_KIND_MOUSE, MS(120 + i), 0, 0, (float)i, 0);
    r = finish();
    CHECK_I(r.n_trace, 5);
    CHECK(r.flags & YRSP_R_TRACE_FULL);
    sample(YRSP_KIND_MOUSE, MS(300), 0, 0, 0, 0);         /* after finish: not traced */
    r = finish();
    CHECK_I(r.n_trace, 5);
    /* trace_kinds 0: every kind, keys too */
    d.trace_kinds = 0;
    CHECK(yrsp_open(&g_c, &d));
    yrsp_arm(&g_c, 0);
    key(MS(40), SC_K, 1);
    sample(YRSP_KIND_PEN, MS(30), 0, 0, 0, 0);
    r = finish();
    CHECK_I(r.n_trace, 2);
}

static void check_capacity(void) {
    yrsp_desc d = fj_desc(5.0, 0);
    yrsp_result r;
    int i;
    CHECK(yrsp_open(&g_c, &d));
    yrsp_arm(&g_c, 0);
    onset(MS(100), 1);
    for (i = 0; i < 100; i++) tap(MS(200) + (int64_t)i * MS(10), 50 + (uint32_t)(i % 40));  /* no choices */
    CHECK_I(key(MS(3000), SC_F, 1), YRSP_ENDED);
    r = finish();
    CHECK(r.flags & YRSP_R_LOG_FULL);
    CHECK_I(r.n_entries, YRSP_LOG_CAP - YRSP_LOG_RESERVE + 1);
    CHECK_I(r.n_lost, 100 - (YRSP_LOG_CAP - YRSP_LOG_RESERVE));
    CHECK_D(YRT_TO_S(r.rt_ns), 2.9);
    /* choices use every slot */
    d.persist = true;
    CHECK(yrsp_open(&g_c, &d));
    yrsp_arm(&g_c, 0);
    onset(MS(100), 1);
    for (i = 0; i < 70; i++) tap(MS(200) + (int64_t)i * MS(60), i % 2 ? SC_F : SC_J);
    r = finish();
    CHECK_I(r.n_entries, YRSP_LOG_CAP);
    CHECK_I(r.n_lost, 6);
}

static void check_quality(void) {
    yrsp_source src[3];
    yrsp_desc d;
    yrsp_result r;
    yrsp_input in;
    static const yrsp_choice b1[] = { { NULL, "b", YRSP_KIND_BOX, 0, 0, 0, 0, 1, 0, 0, 0 },
                                        { "f", NULL, 0, 0, 0, 0, 0, 0, 0, 0, 0 } };
    memset(src, 0, sizeof src);
    src[0].kind = YRSP_KIND_KEYBOARD;
    src[0].tier = YRSP_TIER_UNKNOWN;
    src[0].partial = 1;
    src[0].lo_ns = 160000;
    src[0].hi_ns = 670000;
    src[1].kind = YRSP_KIND_BOX;
    src[1].tier = YRSP_TIER_3;
    src[1].lo_ns = 1000000;
    src[1].hi_ns = 16000000;
    src[2].kind = YRSP_KIND_BOX;
    src[2].device = 5;
    src[2].tier = YRSP_TIER_1;
    src[2].lo_ns = -50000;
    src[2].hi_ns = 50000;
    memset(&d, 0, sizeof d);
    d.choices = b1;
    d.n_choices = 2;
    d.sources = src;
    d.n_sources = 3;
    CHECK(yrsp_open(&g_c, &d));
    yrsp_arm(&g_c, 0);
    onset(MS(100), 1);
    key(MS(300), SC_F, 1);
    r = finish();
    CHECK_I(r.stamp_tier, YRSP_TIER_UNKNOWN);
    CHECK_I(r.stamp_partial, 1);
    CHECK_I(r.stamp_lo_ns, 160000);
    CHECK_I(r.stamp_hi_ns, 670000);
    key(MS(400), SC_F, 0);
    /* the device's own entry wins over the kind's */
    yrsp_arm(&g_c, 0);
    onset(MS(100), 1);
    in = ev(YRSP_KIND_BOX, YRSP_PRESS, MS(300), 1);
    in.device = 5;
    yrsp_feed(&g_c, &in);
    r = finish();
    CHECK_I(r.stamp_tier, YRSP_TIER_1);
    CHECK_I(r.stamp_lo_ns, -50000);
    /* another box: the kind's */
    yrsp_arm(&g_c, 0);
    onset(MS(100), 1);
    in.device = 6;
    in.t = MS(1300);
    yrsp_feed(&g_c, &in);
    r = finish();
    CHECK_I(r.stamp_tier, YRSP_TIER_3);
    CHECK_I(r.stamp_hi_ns, 16000000);
    in.type = YRSP_RELEASE;                               /* box 6 lets go */
    in.t = MS(1400);
    yrsp_feed(&g_c, &in);
    in.type = YRSP_PRESS;
    /* input.stamp overrides the tier; the table's bounds then do not apply */
    yrsp_arm(&g_c, 0);
    onset(MS(100), 1);
    in.device = 6;
    in.t = MS(2300);
    in.stamp = YRSP_TIER_SIM;
    yrsp_feed(&g_c, &in);
    r = finish();
    CHECK_I(r.stamp_tier, YRSP_TIER_SIM);
    CHECK_I(r.stamp_lo_ns, 0);
    CHECK_I(r.stamp_hi_ns, 0);
}

/* v0.6: the parameter table. The switches have no default, so -Wswitch
 * (CI's -Wall -Werror) stops the build when an enum gains a value, and the
 * value check below then fails until the table lists it. */
static const char* choices_name(int v) {
    switch ((yrsp_choices)v) {
    case YRSP_CHOICES_ALL: return "all";
    case YRSP_CHOICES_LIST: return "list";
    case YRSP_CHOICES_NONE: return "none";
    }
    return NULL;
}
static const char* kind_name(int v) {
    switch ((yin_kind)v) {
    case YIN_KIND_KEYBOARD: return "keyboard";
    case YIN_KIND_MOUSE: return "mouse";
    case YIN_KIND_TOUCH: return "touch";
    case YIN_KIND_PEN: return "pen";
    case YIN_KIND_GAMEPAD: return "gamepad";
    case YIN_KIND_BOX: return "box";
    case YIN_KIND_EYE: return "eye";
    case YIN_KIND_SYNC: return "sync";
    case YIN_KIND_USER: return "user";
    }
    return NULL;
}
static const char* match_name(int v) {
    switch ((yrsp_match)v) {
    case YRSP_MATCH_SCANCODE: return "scancode";
    case YRSP_MATCH_KEYCODE: return "keycode";
    }
    return NULL;
}
static const char* cross_name(int v) {
    switch ((yrsp_cross)v) {
    case YRSP_CROSS_NONE: return "none";
    case YRSP_CROSS_RISING: return "rising";
    case YRSP_CROSS_FALLING: return "falling";
    case YRSP_CROSS_DISTANCE: return "distance";
    }
    return NULL;
}

/* An enum row's "name=value" list: values rising, each the C enum's own
 * name for it, inside min..max, and every value of the enum listed. */
static void check_values(const yrsp_param* p, const char* (*name_of)(int)) {
    const char* s = p->values;
    int prev = -1, seen[64], v, bad = 0;
    memset(seen, 0, sizeof seen);
    CHECK(s != NULL);
    while (s && *s && !bad) {
        const char* eq = strchr(s, '=');
        const char* end = strchr(s, ',');
        char name[48];
        if (!end) end = s + strlen(s);
        if (!eq || eq > end || (size_t)(eq - s) >= sizeof name) { bad = 1; break; }
        memcpy(name, s, (size_t)(eq - s));
        name[eq - s] = '\0';
        v = atoi(eq + 1);
        if (v <= prev || v >= 64 || v < p->min || v > p->max || !name_of(v) || strcmp(name_of(v), name)) bad = 1;
        else seen[v] = 1;
        prev = v;
        s = *end ? end + 1 : end;
    }
    for (v = 0; v < 64 && !bad; v++)
        if (name_of(v) && !seen[v]) bad = 1;
    g_checks++;
    if (bad) {
        fprintf(stderr, "response_test: FAIL: %s's values [%s] are not its enum's\n", p->name, p->values ? p->values : "");
        g_failures++;
    }
}

typedef struct rsp_field { const char* name; size_t off, size; int choice; } rsp_field;
#define RSP_F(f) { #f, offsetof(yrsp_desc, f), sizeof(((yrsp_desc*)0)->f), 0 }
#define RSP_C(f) { "choices." #f, offsetof(yrsp_choice, f), sizeof(((yrsp_choice*)0)->f), 1 }
static const rsp_field g_rsp_fields[] = {
    RSP_F(mode), RSP_F(kinds), RSP_F(min_rt_ns), RSP_F(duration_ns), RSP_F(persist), RSP_F(max_responses),
    RSP_F(allow_held_key), RSP_F(wait_for_key_release), RSP_F(max_hold_ns), RSP_F(trace_cap), RSP_F(trace_kinds),
    RSP_C(key), RSP_C(name), RSP_C(kind), RSP_C(match), RSP_C(cross), RSP_C(device), RSP_C(control), RSP_C(code),
    RSP_C(level), RSP_C(hysteresis),
};

static double rsp_read(const void* base, const rsp_field* f, const char* type) {
    const unsigned char* b = (const unsigned char*)base + f->off;
    if (!strcmp(type, "f32")) { float x; memcpy(&x, b, 4); return x; }
    if (!strcmp(type, "i32") || (!strcmp(type, "enum") && f->size == 4)) { int32_t x; memcpy(&x, b, 4); return x; }
    if (f->size == 1) return *b;
    if (f->size == 2) { uint16_t x; memcpy(&x, b, 2); return x; }
    if (f->size == 4) { uint32_t x; memcpy(&x, b, 4); return x; }
    { int64_t x; memcpy(&x, b, 8); return (double)x; }
}

static void check_params(void) {
    static yrsp_collector z;
    int n = -1, i, j, k, rows_of[sizeof g_rsp_fields / sizeof g_rsp_fields[0]];
    const yrsp_param* p = yrsp_params(&n);
    const int nf = (int)(sizeof g_rsp_fields / sizeof g_rsp_fields[0]);
    yrsp_desc d;
    yrsp_choice ch;
    memset(rows_of, 0, sizeof rows_of);
    CHECK(p != NULL && n == nf);
    CHECK(yrsp_params(NULL) == p);
    /* zero fields, one key: what open makes of them */
    memset(&d, 0, sizeof d);
    memset(&ch, 0, sizeof ch);
    ch.key = "f";
    d.choices = &ch;
    d.n_choices = 1;
    CHECK(yrsp_open(&z, &d));
    for (i = 0; i < n; i++) {
        const rsp_field* f = NULL;
        size_t want = 0;
        for (j = 0; j < nf; j++)
            if (!strcmp(p[i].name, g_rsp_fields[j].name)) { f = &g_rsp_fields[j]; rows_of[j]++; }
        g_checks++;
        if (!f) { fprintf(stderr, "response_test: FAIL: row %s names no field\n", p[i].name); g_failures++; continue; }
        CHECK(p[i].type && p[i].unit && p[i].doc && p[i].doc[0]);
        CHECK(p[i].min <= p[i].def && p[i].def <= p[i].max);
        for (k = 0; k < i; k++) CHECK(strcmp(p[i].name, p[k].name) != 0);
        /* the unit follows the name: x_ns is ns, and nothing else is */
        CHECK((strcmp(p[i].unit, "ns") == 0) == (strlen(p[i].name) > 3 && !strcmp(p[i].name + strlen(p[i].name) - 3, "_ns")));
        if (!strcmp(p[i].type, "bool")) want = sizeof(bool);
        else if (!strcmp(p[i].type, "u16")) want = 2;
        else if (!strcmp(p[i].type, "u32") || !strcmp(p[i].type, "i32") || !strcmp(p[i].type, "f32")) want = 4;
        else if (!strcmp(p[i].type, "i64")) want = 8;
        else if (!strcmp(p[i].type, "str")) want = sizeof(const char*);
        else if (!strcmp(p[i].type, "enum")) want = f->size;
        g_checks++;
        if (want != f->size) { fprintf(stderr, "response_test: FAIL: row %s: type %s, field of %d bytes\n",
                                       p[i].name, p[i].type, (int)f->size); g_failures++; }
        CHECK((p[i].values != NULL) == (strcmp(p[i].type, "enum") == 0));
        if (!strcmp(p[i].name, "mode")) check_values(&p[i], choices_name);
        else if (!strcmp(p[i].name, "choices.kind")) check_values(&p[i], kind_name);
        else if (!strcmp(p[i].name, "choices.match")) check_values(&p[i], match_name);
        else if (!strcmp(p[i].name, "choices.cross")) check_values(&p[i], cross_name);
        else CHECK(p[i].values == NULL);
        /* the default: a zero field after open (the key sets control and
         * names the choice; strings have no number) */
        if (!strcmp(p[i].type, "str") || !strcmp(p[i].name, "choices.control")) continue;
        {
            double got;
            if (!strcmp(p[i].name, "min_rt_ns")) got = (double)z.min_rt_ns;
            else if (!strcmp(p[i].name, "duration_ns")) got = (double)z.duration_ns;
            else if (!strcmp(p[i].name, "max_hold_ns")) got = (double)z.max_hold_ns;
            else if (!strcmp(p[i].name, "mode")) continue;   /* the key made it LIST: below */
            else got = rsp_read(f->choice ? (const void*)&z.ch[0] : (const void*)&z.d, f, p[i].type);
            g_checks++;
            if (got != p[i].def) {
                fprintf(stderr, "response_test: FAIL: row %s: a zero field is %.17g, the table says %.17g\n",
                        p[i].name, got, p[i].def);
                g_failures++;
            }
        }
    }
    for (j = 0; j < nf; j++) CHECK(rows_of[j] == 1);
    yrsp_close(&z);
    /* mode without choices */
    memset(&d, 0, sizeof d);
    CHECK(yrsp_open(&z, &d));
    CHECK_I(z.d.mode, p[0].def);
    CHECK_S(p[0].name, "mode");
    yrsp_close(&z);
}

static void check_strerror(void) {
    CHECK_S(yrsp_strerror(YRSP_ENDED), "ended");
    CHECK_S(yrsp_strerror(YRSP_ERR_ARG), "bad argument");
}

static void check_helpers(void) {
    yscr_record fr;
    yau_onset au;
    ytl_event le;
    yrsp_onset o;
    memset(&fr, 0, sizeof fr);
    fr.index = 7;
    fr.onset = MS(500);
    fr.planned = MS(483);
    fr.residual = 250;
    fr.tier = 2;
    o = yrsp_onset_flip(&fr);
    CHECK_I(o.t_ns, MS(500));
    CHECK_I(o.frame, 7);
    CHECK_I(o.tier, 2);
    CHECK_I(o.final, 1);
    CHECK_I(o.src, YRSP_ONSET_FLIP);
    CHECK_I(o.residual_ns, 250);
    fr.flags = YSCR_FLIP_PENDING;
    CHECK_I(yrsp_onset_flip(&fr).final, 0);
    fr.flags = 0;
    fr.onset = 0;                       /* never shown */
    o = yrsp_onset_flip(&fr);
    CHECK_I(o.t_ns, MS(483));
    CHECK_I(o.final, 0);
    CHECK_I(o.src, YRSP_ONSET_PLAN);
    memset(&au, 0, sizeof au);
    au.onset = MS(900);
    au.residual = -10;
    au.tier = 2;
    au.flags = YAU_ONSET_PENDING;
    o = yrsp_onset_audio(&au);
    CHECK_I(o.final, 0);
    CHECK_I(o.src, YRSP_ONSET_AUDIO);
    au.flags = 0;
    o = yrsp_onset_audio(&au);
    CHECK_I(o.final, 1);
    CHECK_I(o.t_ns, MS(900));
    CHECK_I(o.frame, -1);
    memset(&le, 0, sizeof le);
    le.onset = MS(1000);
    le.residual = 5;
    le.frame = 60;
    o = yrsp_onset_landing(&le);
    CHECK_I(o.t_ns, MS(1000));
    CHECK_I(o.final, 0);
    CHECK_I(o.frame, 60);
    CHECK_I(o.src, YRSP_ONSET_PLAN);
}

/* v0.4: the plan kept beside the final onset, yrsp_plan_frame(), and the
 * frame-loop helpers on filled structs: a frame's done records, the fired
 * events of a timeline evaluate, and the data writer's columns. */
typedef struct mem_out { char buf[4096]; size_t n; } mem_out;
static bool mem_write(void* ctx, const void* bytes, size_t n) {
    mem_out* m = (mem_out*)ctx;
    if (m->n + n >= sizeof m->buf) return false;
    memcpy(m->buf + m->n, bytes, n);
    m->n += n;
    m->buf[m->n] = 0;
    return true;
}

static void check_frame_loop(void) {
    static ytr_data w;
    static mem_out mo;
    yrsp_desc d = fj_desc(1.5, 0.1);
    yscr_record rec[3];
    yscr_frame fr;
    ytl_event fired[3];
    yrsp_result r;
    yrsp_onset o;
    ytr_data_desc dd;
    const ytr_col cols[] = { YTR_COL_SET("trial", YTR_COL_INT), YRSP_DATA_COLS };
    CHECK(yrsp_open(&g_c, &d));
    CHECK_I(yrsp_plan_frame(&g_c), -1);           /* not armed */
    yrsp_arm(&g_c, 0);
    CHECK_I(yrsp_plan_frame(&g_c), -1);           /* no onset */

    /* the landing: ONSET of channel 2 at fired[1]; a MARK and another channel around it */
    memset(fired, 0, sizeof fired);
    fired[0].kind = YTL_ONSET; fired[0].target = 1; fired[0].frame = 30; fired[0].onset = MS(400);
    fired[1].kind = YTL_ONSET; fired[1].target = 2; fired[1].frame = 31; fired[1].onset = MS(500);
    fired[1].residual = 7;
    fired[2].kind = YTL_MARK; fired[2].target = 2; fired[2].frame = 31;
    CHECK_I(yrsp_set_landing(&g_c, fired, 3, 5), -1);
    CHECK_I(yrsp_plan_frame(&g_c), -1);
    CHECK_I(yrsp_set_landing(&g_c, NULL, 3, 2), -1);
    CHECK_I(yrsp_set_landing(&g_c, fired, 1, 2), -1);   /* n bounds the search */
    fired[0].kind = YTL_OFFSET; fired[0].target = 2;    /* an OFFSET of the channel is not its onset */
    CHECK_I(yrsp_set_landing(&g_c, fired, 3, 2), 1);
    CHECK_I(yrsp_plan_frame(&g_c), 31);

    /* a press 0.45 s after the plan; the record of frame 31 comes later */
    key(MS(950), SC_F, 1);
    memset(rec, 0, sizeof rec);
    rec[0].index = 30; rec[0].onset = MS(400); rec[0].tier = 1;
    rec[1].index = 31; rec[1].onset = MS(517); rec[1].residual = 16666667; rec[1].dropped = 1; rec[1].tier = 2;
    rec[2].index = 32; rec[2].onset = MS(534); rec[2].tier = 1;
    memset(&fr, 0, sizeof fr);
    fr.onset = MS(960);
    fr.done = rec;
    fr.n_done = 1;                                    /* frame 30 only: still the plan */
    CHECK_I(yrsp_update_frame(&g_c, &fr), YRSP_ENDED);
    CHECK_I(yrsp_plan_frame(&g_c), 31);
    fr.done = rec + 1;
    fr.n_done = 2;                                    /* frame 31's record, a frame late */
    yrsp_update_frame(&g_c, &fr);
    CHECK_I(yrsp_plan_frame(&g_c), -1);               /* final now */
    r = finish();
    CHECK_I(r.t_onset, MS(517));
    CHECK_I(r.rt_ns, MS(433));
    CHECK_I(r.onset_src, YRSP_ONSET_FLIP);
    CHECK_I(r.onset_tier, 2);
    CHECK_I(r.onset_residual, 16666667);
    CHECK_I(r.plan_residual, 7);                      /* the landing's, kept */
    CHECK_I(r.onset_frame, 31);
    CHECK(!(r.flags & YRSP_R_ONSET_PLAN));

    /* the deadline runs from the final onset, through f->onset */
    yrsp_arm(&g_c, 0);
    yrsp_set_landing(&g_c, fired, 3, 2);
    fr.done = rec + 1;
    fr.n_done = 1;
    fr.onset = MS(517) + YRT_S(1.5) - 1;
    CHECK_I(yrsp_update_frame(&g_c, &fr), YRSP_OPEN);
    fr.n_done = 0;
    fr.onset = MS(517) + YRT_S(1.5);
    CHECK_I(yrsp_update_frame(&g_c, &fr), YRSP_ENDED);
    r = finish();
    CHECK(r.flags & YRSP_R_TIMEOUT);

    /* a never-shown frame's record is a plan: it does not replace the
     * landing's residual, and the frame stays the one awaited */
    yrsp_arm(&g_c, 0);
    yrsp_set_landing(&g_c, fired, 3, 2);
    rec[1].onset = 0;
    rec[1].planned = MS(500);
    rec[1].flags = YSCR_FLIP_SKIPPED;
    fr.done = rec + 1;
    fr.n_done = 1;
    fr.onset = MS(600);
    yrsp_update_frame(&g_c, &fr);
    r = finish();
    CHECK_I(r.plan_residual, 7);
    CHECK(r.flags & YRSP_R_ONSET_PLAN);
    CHECK_I(yrsp_plan_frame(&g_c), 31);

    /* a sound's plan has no frame; a plan set before arm() is not kept */
    yrsp_arm(&g_c, 0);
    memset(&o, 0, sizeof o);
    o.t_ns = MS(100);
    o.frame = -1;
    o.src = YRSP_ONSET_AUDIO;
    yrsp_set_onset(&g_c, &o);
    CHECK_I(yrsp_plan_frame(&g_c), -1);
    r = finish();
    CHECK_I(r.plan_residual, 0);
    yrsp_close(&g_c);
    CHECK_I(yrsp_plan_frame(&g_c), -1);
    yrsp_arm(&g_c, 0);
    CHECK(yrsp_open(&g_c, &d));
    r = finish();
    CHECK_I(r.plan_residual, YRSP_NO_TIME);           /* no plan came */
    CHECK_I(yrsp_update_frame(&g_c, NULL), YRSP_ERR_ARG);

    /* the data columns: the result's fields, an empty field where there is none */
    memset(&dd, 0, sizeof dd);
    dd.write = mem_write;
    dd.write_ctx = &mo;
    dd.cols = cols;
    dd.n_cols = (int)(sizeof cols / sizeof cols[0]);
    CHECK(ytr_data_open(&w, &dd));
    CHECK_S(mo.buf, "trial,response,rt_s,rt_key_duration_s,onset_src,onset_tier,onset_residual_s,landing_residual_s,"
                    "onset_frame,rsp_tier,rsp_flags,n_anticipations,n_early\n");
    yrsp_arm(&g_c, 0);
    yrsp_set_landing(&g_c, fired, 3, 2);
    key(MS(500) + MS(50), SC_J, 1);                   /* an anticipation */
    key(MS(500) + MS(60), SC_J, 0);
    key(MS(500) + MS(250), SC_J, 1);
    key(MS(500) + MS(330), SC_J, 0);
    rec[1].onset = MS(500);
    rec[1].residual = 1000;
    rec[1].flags = 0;
    fr.done = rec + 1;
    fr.n_done = 1;
    yrsp_update_frame(&g_c, &fr);
    r = finish();
    CHECK_I(yrsp_data_set(&w, &r), YRSP_N_DATA_COLS);
    CHECK_I(ytr_data_set_int(&w, 0, 1), 0);
    CHECK_I(ytr_data_write(&w, NULL), 0);
    CHECK_HAS(mo.buf, "\n1,right,0.250000000,0.080000000,1,2,0.000001000,0.000000007,31,0,1,1,0\n");
    mo.n = 0;
    yrsp_arm(&g_c, 0);                                /* no onset, no response */
    r = finish();
    CHECK_I(yrsp_data_set(&w, &r), YRSP_N_DATA_COLS);
    CHECK_I(ytr_data_write(&w, NULL), 0);
    CHECK_S(mo.buf, ",,,,,,,,,," "18,0,0\n");      /* NO_ONSET | TIMEOUT */
    CHECK_I(yrsp_data_set(&w, NULL), YTR_ERR_ARG);
    ytr_data_close(&w);
    yrsp_close(&g_c);
}

#if defined(YRSP_TEST_SDL)
static void check_sdl(void) {
    SDL_Event e;
    yrsp_input in;
    yrsp_sdl_ctx ctx;
    yrsp_source s;
    memset(&ctx, 0, sizeof ctx);
    memset(&e, 0, sizeof e);
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.down = true;
    e.key.scancode = SDL_SCANCODE_F;
    e.key.key = SDLK_F;
    e.key.which = 12;
    e.key.mod = SDL_KMOD_LSHIFT;
    CHECK_I(yrsp_from_sdl(&e, 777, NULL, &in), 1);
    CHECK_I(in.kind, YRSP_KIND_KEYBOARD);
    CHECK_I(in.type, YRSP_PRESS);
    CHECK_I(in.t, 777);
    CHECK_I(in.control, SC_F);
    CHECK_I(in.code, 'f');
    CHECK_I(in.device, 12);
    CHECK_I(in.mods, SDL_KMOD_LSHIFT);
    CHECK_I(in.stamp, 0);
    CHECK_I(in.flags, 0);
    e.key.repeat = true;
    yrsp_from_sdl(&e, 777, &ctx, &in);           /* ctx: raw path off */
    CHECK_I(in.flags, YRSP_IN_REPEAT);
    CHECK_I(in.stamp, YRSP_TIER_3);
    ctx.raw_keyboard = true;
    yrsp_from_sdl(&e, 777, &ctx, &in);
    CHECK_I(in.stamp, 0);
    e.type = SDL_EVENT_KEY_UP;
    e.key.down = false;
    yrsp_from_sdl(&e, 777, &ctx, &in);
    CHECK_I(in.type, YRSP_RELEASE);
    /* mouse; synthetic from touch and pen dropped */
    memset(&e, 0, sizeof e);
    e.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    e.button.down = true;
    e.button.button = 3;
    e.button.x = 10.5f;
    e.button.y = 20;
    CHECK_I(yrsp_from_sdl(&e, 1, NULL, &in), 1);
    CHECK_I(in.kind, YRSP_KIND_MOUSE);
    CHECK_I(in.control, 3);
    CHECK(in.x == 10.5f);
    CHECK_I(in.stamp, YRSP_TIER_3);            /* which 0: SDL's absolute mode */
    e.button.which = 5;                           /* relative mode: raw */
    yrsp_from_sdl(&e, 1, NULL, &in);
    CHECK_I(in.stamp, 0);
    e.button.which = SDL_TOUCH_MOUSEID;
    CHECK_I(yrsp_from_sdl(&e, 1, NULL, &in), 0);
    e.button.which = SDL_PEN_MOUSEID;
    CHECK_I(yrsp_from_sdl(&e, 1, &ctx, &in), 0);
    ctx.keep_synthetic = true;
    CHECK_I(yrsp_from_sdl(&e, 1, &ctx, &in), 1);
    CHECK_I(in.flags, YRSP_IN_SYNTHETIC);
    ctx.keep_synthetic = false;
    memset(&e, 0, sizeof e);
    e.type = SDL_EVENT_MOUSE_MOTION;
    e.motion.x = 5;
    e.motion.y = 6;
    CHECK_I(yrsp_from_sdl(&e, 1, NULL, &in), 1);
    CHECK_I(in.type, YRSP_SAMPLE);
    CHECK_I(in.control, YRSP_AXIS_POSITION);
    e.motion.which = SDL_PEN_MOUSEID;
    CHECK_I(yrsp_from_sdl(&e, 1, NULL, &in), 0);
    /* touch */
    memset(&e, 0, sizeof e);
    e.type = SDL_EVENT_FINGER_DOWN;
    e.tfinger.touchID = 9;
    e.tfinger.fingerID = 4;
    e.tfinger.x = 0.25f;
    e.tfinger.pressure = 0.5f;
    CHECK_I(yrsp_from_sdl(&e, 1, NULL, &in), 1);
    CHECK_I(in.kind, YRSP_KIND_TOUCH);
    CHECK_I(in.type, YRSP_PRESS);
    CHECK_I(in.control, 4);
    CHECK_I(in.device, 9);
    CHECK(in.value == 0.5f);
    e.type = SDL_EVENT_FINGER_MOTION;
    yrsp_from_sdl(&e, 1, NULL, &in);
    CHECK_I(in.type, YRSP_SAMPLE);
    e.type = SDL_EVENT_FINGER_CANCELED;
    yrsp_from_sdl(&e, 1, NULL, &in);
    CHECK_I(in.type, YRSP_RELEASE);
    e.tfinger.touchID = SDL_PEN_TOUCHID;
    CHECK_I(yrsp_from_sdl(&e, 1, NULL, &in), 0);
    /* pen */
    memset(&e, 0, sizeof e);
    e.type = SDL_EVENT_PEN_DOWN;
    e.ptouch.down = true;
    e.ptouch.eraser = true;
    e.ptouch.which = 2;
    CHECK_I(yrsp_from_sdl(&e, 1, NULL, &in), 1);
    CHECK_I(in.kind, YRSP_KIND_PEN);
    CHECK_I(in.type, YRSP_PRESS);
    CHECK_I(in.control, 0);
    CHECK_I(in.flags, YRSP_IN_ERASER);
    memset(&e, 0, sizeof e);
    e.type = SDL_EVENT_PEN_AXIS;
    e.paxis.axis = SDL_PEN_AXIS_PRESSURE;
    e.paxis.value = 0.75f;
    yrsp_from_sdl(&e, 1, NULL, &in);
    CHECK_I(in.type, YRSP_SAMPLE);
    CHECK_I(in.control, YRSP_AXIS_PEN_PRESSURE);
    CHECK(in.value == 0.75f);
    e.paxis.axis = SDL_PEN_AXIS_YTILT;
    yrsp_from_sdl(&e, 1, NULL, &in);
    CHECK_I(in.control, YRSP_AXIS_PEN + 2);
    memset(&e, 0, sizeof e);
    e.type = SDL_EVENT_PEN_BUTTON_DOWN;
    e.pbutton.down = true;
    e.pbutton.button = 1;
    yrsp_from_sdl(&e, 1, NULL, &in);
    CHECK_I(in.control, 1);
    CHECK_I(in.type, YRSP_PRESS);
    e.type = SDL_EVENT_PEN_PROXIMITY_OUT;
    yrsp_from_sdl(&e, 1, NULL, &in);
    CHECK_I(in.type, YRSP_PROXIMITY);
    CHECK(in.value == 0.0f);
    e.type = SDL_EVENT_PEN_MOTION;
    yrsp_from_sdl(&e, 1, NULL, &in);
    CHECK_I(in.type, YRSP_SAMPLE);
    /* gamepad */
    memset(&e, 0, sizeof e);
    e.type = SDL_EVENT_GAMEPAD_AXIS_MOTION;
    e.gaxis.axis = SDL_GAMEPAD_AXIS_LEFTX;
    e.gaxis.value = -32768;
    yrsp_from_sdl(&e, 1, NULL, &in);
    CHECK(in.value == -1.0f);
    CHECK_I(in.control, YRSP_AXIS_GAMEPAD);
    e.gaxis.axis = SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
    e.gaxis.value = 32767;
    yrsp_from_sdl(&e, 1, NULL, &in);
    CHECK(in.value == 1.0f);
    CHECK_I(in.control, YRSP_AXIS_GAMEPAD + 5);
    e.gaxis.value = 0;
    yrsp_from_sdl(&e, 1, NULL, &in);
    CHECK(in.value == 0.0f);
    memset(&e, 0, sizeof e);
    e.type = SDL_EVENT_GAMEPAD_BUTTON_UP;
    e.gbutton.button = SDL_GAMEPAD_BUTTON_SOUTH;
    yrsp_from_sdl(&e, 1, NULL, &in);
    CHECK_I(in.type, YRSP_RELEASE);
    CHECK_I(in.kind, YRSP_KIND_GAMEPAD);
    /* the rest */
    memset(&e, 0, sizeof e);
    e.type = SDL_EVENT_WINDOW_EXPOSED;
    CHECK_I(yrsp_from_sdl(&e, 1, NULL, &in), 0);
    /* sources */
    s = yrsp_sdl_source(&ctx, YRSP_KIND_KEYBOARD);
    CHECK_I(s.kind, YRSP_KIND_KEYBOARD);
#if defined(_WIN32)
    CHECK_I(s.partial, 1);
    CHECK_I(s.hi_ns, 670000);
    ctx.raw_keyboard = false;
    s = yrsp_sdl_source(&ctx, YRSP_KIND_KEYBOARD);
    CHECK_I(s.tier, YRSP_TIER_3);
    s = yrsp_sdl_source(&ctx, YRSP_KIND_PEN);
    CHECK_I(s.tier, YRSP_TIER_3);
#endif
    CHECK(s.note != NULL);
}
#endif

/* Random streams: the result agrees with a direct reading of the entries. */
static uint64_t g_rng = 0x9E3779B97F4A7C15ULL;
static uint32_t rnd(uint32_t n) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return (uint32_t)(g_rng % n);
}

static void check_property(void) {
    int round, i, bad = 0;
    for (round = 0; round < 20000 && bad < 5; round++) {
        yrsp_desc d = fj_desc(rnd(3) ? 0.5 + rnd(1000) / 1000.0 : 0, rnd(200) / 1000.0);
        yrsp_result r;
        const yrsp_entry* e;
        int n, n_valid = 0, n_cls = 0, best = -1;
        int64_t on = MS(200 + rnd(400));
        if (d.duration_ns > 0 && d.min_rt_ns >= d.duration_ns) d.min_rt_ns = 0;
        d.persist = rnd(2) != 0;
        d.max_responses = (int)rnd(3);
        d.allow_held_key = rnd(2) != 0;
        d.wait_for_key_release = rnd(3) == 0;
        if (!yrsp_open(&g_c, &d)) { bad++; CHECK(0); continue; }
        yrsp_arm(&g_c, 0);
        if (rnd(4)) onset(on - MS(rnd(30)), 0);
        for (i = 0; i < 40; i++) {
            uint32_t k = rnd(4);
            uint32_t sc = k == 0 ? SC_F : k == 1 ? SC_J : k == 2 ? SC_K : 4 + rnd(3);
            int64_t t = MS(rnd(2500)) + rnd(1000000);
            switch (rnd(5)) {
            case 0: case 1: key(t, sc, 1); break;
            case 2: case 3: key(t, sc, 0); break;
            default: key_repeat(t, sc); break;
            }
            if (rnd(10) == 0) yrsp_update(&g_c, MS(rnd(3000)));
            if (i == 20 && rnd(2)) onset(on, 1);
        }
        yrsp_finish(&g_c, &r);
        e = yrsp_entries(&g_c, &n);
        for (i = 0; i < n; i++) {
            uint16_t k = (uint16_t)(e[i].flags & YRSP_E_CLASS_MASK);
            int64_t rt = e[i].t - g_c.onset.t_ns;
            /* one class at most, and only one that its rt allows */
            if ((k & (k - 1)) != 0) n_cls++;
            if (k == YRSP_E_VALID) {
                n_valid++;
                if (!g_c.have_onset || rt < g_c.min_rt_ns || (g_c.duration_ns && rt >= g_c.duration_ns)) n_cls++;
                if (e[i].flags & (YRSP_E_NOT_CHOICE | YRSP_E_AFTER_END)) n_cls++;
                if (best < 0 || e[i].t < e[best].t) best = i;
            }
        }
        if (n_cls || n_valid != r.n_responses || best != r.entry ||
            (best >= 0 && r.rt_ns != e[best].t - g_c.onset.t_ns) ||
            r.n_entries != n || n > YRSP_LOG_CAP) {
            bad++;
            fprintf(stderr, "response_test: property round %d: classes %d valid %d/%d best %d/%d\n", round,
                    n_cls, n_valid, r.n_responses, best, r.entry);
        }
        g_checks++;
    }
    if (bad) g_failures++;
}

/* v0.3.0: close, the messages of failing calls, YRSP_NO_TIME. */
static void check_close_messages(void) {
    static yrsp_collector z;
    yrsp_desc d = fj_desc(1.5, 0.1);
    yrsp_input in;
    yrsp_result r;
    memset(&z, 0, sizeof z);
    yrsp_close(&z);                            /* zeroed, twice */
    yrsp_close(&z);
    yrsp_close(NULL);
    CHECK_I(yrsp_feed(&z, NULL), YRSP_ERR_STATE);
    CHECK_S(yrsp_error(&z), "ysp_response: yrsp_feed: the collector is not open");
    d.n_choices = -1;
    CHECK(!yrsp_open(&z, &d));
    yrsp_close(&z);                            /* after a failed open */
    CHECK_HAS(yrsp_error(&z), "n_choices");
    d = fj_desc(1.5, 0.1);
    CHECK(yrsp_open(&g_c, &d));
    CHECK_S(yrsp_error(&g_c), "");
    memset(&in, 0, sizeof in);                 /* type NONE: refused */
    CHECK_I(yrsp_feed(&g_c, &in), YRSP_ERR_ARG);
    CHECK_S(yrsp_error(&g_c), "ysp_response: yrsp_feed: bad argument");
    CHECK_I(yrsp_set_onset(&g_c, NULL), YRSP_ERR_ARG);
    CHECK_S(yrsp_error(&g_c), "ysp_response: yrsp_set_onset: bad argument");
    CHECK_I(yrsp_finish(&g_c, NULL), YRSP_ERR_ARG);
    CHECK_S(yrsp_error(&g_c), "ysp_response: yrsp_finish: bad argument");
    yrsp_arm(&g_c, 0);
    CHECK_I(yrsp_finish(&g_c, &r), YRSP_ENDED);
    CHECK(r.rt_ns == YRSP_NO_TIME && r.rt_key_duration_ns == YRSP_NO_TIME);
    yrsp_close(&g_c);
    yrsp_close(&g_c);
    CHECK_I(yrsp_update(&g_c, 0), YRSP_ERR_STATE);
    CHECK_S(yrsp_error(&g_c), "ysp_response: yrsp_update: the collector is not open");
}

/* v0.7, CODES SENT: a code sent at the press (ysp/device.h's rule) against
 * the final responses. */
static yrsp_sent sent_for(int64_t t, uint32_t sc, int choice, int written) {
    yrsp_sent x;
    memset(&x, 0, sizeof x);
    x.t = t;
    x.kind = YRSP_KIND_KEYBOARD;
    x.control = sc;
    x.choice = (int16_t)choice;
    x.written = (uint8_t)written;
    return x;
}

/* One window: plan onset at 1 s, a press at press_t of sc, the given codes
 * sent, the final onset (0 = none), then the result's flags. */
static uint32_t sent_case(int64_t press_t, uint32_t sc, const yrsp_sent* x, int n, int total, int64_t final_onset,
                          int32_t* n_sent) {
    yrsp_result r;
    yrsp_arm(&g_c, 0);
    onset(MS(1000), 0);
    if (press_t) key(press_t, sc, 1);
    CHECK_I(yrsp_set_sent(&g_c, x, n, total), 0);
    if (final_onset) onset(final_onset, 1);
    yrsp_update(&g_c, MS(5000));
    r = finish();
    if (press_t) key(press_t + MS(50), sc, 0);
    if (n_sent) *n_sent = r.n_sent;
    return r.flags;
}

static void check_sent(void) {
    yrsp_desc d = fj_desc(1.0, 0.1);
    yrsp_sent x[3];
    int32_t ns = -1;
    uint32_t fl;
    CHECK(yrsp_open(&g_c, &d));
    /* agree: the code for F at 1.2 s, F the response */
    x[0] = sent_for(MS(1200), SC_F, 0, 1);
    fl = sent_case(MS(1200), SC_F, x, 1, 1, MS(1017), &ns);
    CHECK(!(fl & YRSP_R_SENT_MISMATCH) && (fl & YRSP_R_RESPONDED));
    CHECK_I(ns, 1);
    /* no codes told: no check */
    yrsp_arm(&g_c, 0);
    onset(MS(1000), 1);
    key(MS(1200), SC_F, 1);
    {
        yrsp_result r = finish();
        CHECK(!(r.flags & YRSP_R_SENT_MISMATCH) && r.n_sent == 0);
    }
    key(MS(1250), SC_F, 0);
    /* an anticipation once the final onset came: fired at 1.15 s on the
     * plan (150 ms), 90 ms from the final onset */
    x[0] = sent_for(MS(1150), SC_F, 0, 1);
    fl = sent_case(MS(1150), SC_F, x, 1, 1, MS(1060), NULL);
    CHECK((fl & YRSP_R_SENT_MISMATCH) && !(fl & YRSP_R_RESPONDED));
    /* late: 1.0 s from the onset */
    x[0] = sent_for(MS(2000), SC_F, 0, 1);
    fl = sent_case(MS(2000), SC_F, x, 1, 1, 0, NULL);
    CHECK((fl & YRSP_R_SENT_MISMATCH) && !(fl & YRSP_R_RESPONDED));
    /* early: before the onset */
    x[0] = sent_for(MS(900), SC_F, 0, 1);
    fl = sent_case(MS(900), SC_F, x, 1, 1, 0, NULL);
    CHECK(fl & YRSP_R_SENT_MISMATCH);
    /* another choice's code: J's code for an F press */
    x[0] = sent_for(MS(1200), SC_F, 1, 1);
    fl = sent_case(MS(1200), SC_F, x, 1, 1, 0, NULL);
    CHECK((fl & YRSP_R_SENT_MISMATCH) && (fl & YRSP_R_RESPONDED));
    /* a press that is no choice */
    x[0] = sent_for(MS(1200), SC_K, 0, 1);
    fl = sent_case(MS(1200), SC_K, x, 1, 1, 0, NULL);
    CHECK(fl & YRSP_R_SENT_MISMATCH);
    /* a press the collector never got */
    x[0] = sent_for(MS(1300), SC_F, 0, 1);
    fl = sent_case(MS(1200), SC_F, x, 1, 1, 0, NULL);
    CHECK((fl & YRSP_R_SENT_MISMATCH) && (fl & YRSP_R_RESPONDED));
    /* the response's code and one for a press the collector never got */
    x[0] = sent_for(MS(1200), SC_F, 0, 1);
    x[1] = sent_for(MS(1300), SC_F, 0, 1);
    fl = sent_case(MS(1200), SC_F, x, 2, 2, 0, NULL);
    CHECK(fl & YRSP_R_SENT_MISMATCH);
    /* the response without its code: none sent, or its write failed */
    fl = sent_case(MS(1200), SC_F, NULL, 0, 0, 0, &ns);
    CHECK((fl & YRSP_R_SENT_MISMATCH) && ns == 0);
    x[0] = sent_for(MS(1200), SC_F, 0, 0);
    fl = sent_case(MS(1200), SC_F, x, 1, 1, 0, &ns);
    CHECK((fl & YRSP_R_SENT_MISMATCH) && ns == 0);
    /* no response and no code: agree */
    fl = sent_case(0, SC_F, NULL, 0, 0, 0, NULL);
    CHECK(!(fl & YRSP_R_SENT_MISMATCH) && (fl & YRSP_R_TIMEOUT));
    /* a code not kept cannot be checked */
    x[0] = sent_for(MS(1200), SC_F, 0, 1);
    fl = sent_case(MS(1200), SC_F, x, 1, 2, 0, NULL);
    CHECK(fl & YRSP_R_SENT_MISMATCH);
    /* the collector refused the fired press: an earlier-stamped J arrived
     * after the F the code went out for, and J is the response */
    yrsp_arm(&g_c, 0);
    onset(MS(1000), 1);
    key(MS(1300), SC_F, 1);
    key(MS(1250), SC_J, 1);
    x[0] = sent_for(MS(1300), SC_F, 0, 1);
    CHECK_I(yrsp_set_sent(&g_c, x, 1, 1), 0);
    {
        yrsp_result r = finish();
        CHECK(r.response == 1 && (r.flags & YRSP_R_SENT_MISMATCH));
    }
    /* both coded: F's code marks a press that is not the response */
    x[1] = sent_for(MS(1250), SC_J, 1, 1);
    CHECK_I(yrsp_set_sent(&g_c, x, 2, 2), 0);
    {
        yrsp_result r = finish();
        CHECK(r.response == 1 && r.n_responses == 2 && (r.flags & YRSP_R_SENT_MISMATCH));
    }
    key(MS(1400), SC_F, 0);
    key(MS(1400), SC_J, 0);
    /* two codes for one press */
    x[0] = x[1] = sent_for(MS(1200), SC_F, 0, 1);
    fl = sent_case(MS(1200), SC_F, x, 2, 2, 0, &ns);
    CHECK((fl & YRSP_R_SENT_MISMATCH) && ns == 2);
    /* arm clears the codes: a window with no set_sent is not checked */
    yrsp_arm(&g_c, 0);
    onset(MS(1000), 1);
    key(MS(1200), SC_F, 1);
    {
        yrsp_result r = finish();
        CHECK(!(r.flags & YRSP_R_SENT_MISMATCH) && r.n_sent == 0);
    }
    key(MS(1250), SC_F, 0);
    /* arguments */
    CHECK_I(yrsp_set_sent(&g_c, x, -1, 0), YRSP_ERR_ARG);
    CHECK_I(yrsp_set_sent(&g_c, x, 2, 1), YRSP_ERR_ARG);
    CHECK_I(yrsp_set_sent(&g_c, NULL, 1, 1), YRSP_ERR_ARG);
    CHECK_HAS(yrsp_error(&g_c), "yrsp_set_sent");
    yrsp_close(&g_c);
    CHECK_I(yrsp_set_sent(&g_c, x, 1, 1), YRSP_ERR_STATE);
    /* ALL mode has no choice index: the code's choice is not compared */
    {
        yrsp_desc all = d;
        all.choices = NULL;
        all.n_choices = 0;
        CHECK(yrsp_open(&g_c, &all));
        x[0] = sent_for(MS(1200), SC_K, 0, 1);
        fl = sent_case(MS(1200), SC_K, x, 1, 1, 0, NULL);
        CHECK(!(fl & YRSP_R_SENT_MISMATCH) && (fl & YRSP_R_RESPONDED));
        yrsp_close(&g_c);
    }
    /* persist: every response needs its code, and only responses */
    d.persist = true;
    CHECK(yrsp_open(&g_c, &d));
    yrsp_arm(&g_c, 0);
    onset(MS(1000), 1);
    key(MS(1200), SC_F, 1);
    key(MS(1250), SC_F, 0);
    key(MS(1400), SC_J, 1);
    key(MS(1450), SC_J, 0);
    x[0] = sent_for(MS(1200), SC_F, 0, 1);
    x[1] = sent_for(MS(1400), SC_J, 1, 1);
    CHECK_I(yrsp_set_sent(&g_c, x, 2, 2), 0);
    {
        yrsp_result r = finish();
        CHECK(r.n_responses == 2 && !(r.flags & YRSP_R_SENT_MISMATCH) && r.n_sent == 2);
    }
    /* persist: a code for an anticipation beside the two responses' */
    key(MS(1050), SC_F, 1);
    key(MS(1060), SC_F, 0);
    x[2] = sent_for(MS(1050), SC_F, 0, 1);
    CHECK_I(yrsp_set_sent(&g_c, x, 3, 3), 0);
    {
        yrsp_result r = finish();
        CHECK(r.n_anticipations == 1 && (r.flags & YRSP_R_SENT_MISMATCH));
    }
    CHECK_I(yrsp_set_sent(&g_c, x, 1, 1), 0);    /* J's code missing */
    {
        yrsp_result r = finish();
        CHECK(r.flags & YRSP_R_SENT_MISMATCH);
    }
    yrsp_close(&g_c);
    printf("  codes sent: agree, anticipation, late, early, other choice, not a choice, unseen, missing, failed, "
           "refused, doubled, persist: ok\n");
}

int main(void) {
    printf("ysp_response %s\n", yrsp_version());
    check_names();
    check_close_messages();
    check_validation();
    check_basic();
    check_bounds();
    check_deadline();
    check_ending();
    check_modes();
    check_held();
    check_double();
    check_devices();
    check_raw_mice();
    check_onset_refine();
    check_out_of_order();
    check_release();
    check_crossings();
    check_trace();
    check_capacity();
    check_quality();
    check_strerror();
    check_params();
    check_helpers();
    check_frame_loop();
    check_sent();
#if defined(YRSP_TEST_SDL)
    check_sdl();
#endif
    check_property();
    if (g_failures) {
        fprintf(stderr, "response_test: %d of %d checks FAILED\n", g_failures, g_checks);
        return 1;
    }
    printf("response_test: all %d checks passed\n", g_checks);
    return 0;
}
