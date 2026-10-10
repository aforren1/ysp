/* Compile check: ysp/timeline.h as a C translation unit with the
 * implementation enabled. CMake and CI build this as C11 with warnings as
 * errors; timeline.cpp builds the same source as C++17. The header has
 * no threads, so there is no YTL_NO_THREADS variant. It runs and returns 0
 * to prove it links, and that a sequence built by calls and one built from
 * an op table both apply. */
#define YSP_TIMELINE_IMPLEMENTATION
#include "ysp/timeline.h"

#include <string.h>

/* The lifecycle and the calls v0.5.0 changed, one field per entry point, so
 * a changed signature is a compile error here. */
typedef struct tl_api {
    bool (*open)(ytl_timeline*, const ytl_desc*);
    void (*close)(ytl_timeline*);
    const char* (*error)(const ytl_timeline*);
    int (*peek)(ytl_timeline*, int, int64_t, int64_t, ytl_event*, int);
    int (*clamped)(ytl_timeline*, int);
    int (*check_ops)(ytl_timeline*, int, const ytl_op*, int, int*);
    void (*wait)(ytl_seq*, int64_t);
    int64_t (*ns)(double);
} tl_api;

static const tl_api g_api = {
    ytl_open, ytl_close, ytl_error, ytl_peek, ytl_clamped, ytl_check_ops, ytl_wait, ytl_ns
};

/* The unit macros are constant expressions: a static initializer. */
static const int64_t g_units[4] = { YRT_S(1), YRT_MS(1), YRT_US(1), YRT_NS(1) };

/* Every time field the caller writes, by name. */
static int names(void) {
    ytl_event e;
    ytl_key k;
    ytl_track tr;
    ytl_tween_desc tw;
    ytl_op op;
    memset(&e, 0, sizeof e);
    memset(&k, 0, sizeof k);
    memset(&tr, 0, sizeof tr);
    memset(&tw, 0, sizeof tw);
    memset(&op, 0, sizeof op);
    e.time_ns = YRT_MS(1);
    k.time_ns = YRT_MS(2);
    tr.t0_ns = YRT_MS(3);
    tr.period_ns = YRT_MS(4);
    tw.duration_ns = YRT_MS(5);
    tw.start_ns = YRT_MS(6);
    tw.delay_ns = YRT_MS(7);
    op.t_ns = YRT_MS(8);
    return e.time_ns + k.time_ns + tr.t0_ns + tr.period_ns + tw.duration_ns + tw.start_ns
           + tw.delay_ns + op.t_ns == YRT_MS(36) ? 0 : 1;
}

static int open_tl(ytl_timeline* tl, ytl_event* ev, ytl_key* keys) {
    ytl_desc d;
    memset(&d, 0, sizeof d);
    d.events = ev;
    d.event_capacity = 16;
    d.n_channels = 3;
    d.keys = keys;
    d.key_capacity = 32;
    return ytl_open(tl, &d) ? 0 : 1;
}

int main(void) {
    static ytl_event ev[16];
    static ytl_key keys[32];
    static ytl_timeline tl;
    ytl_tween_desc up;
    ytl_seq q;
    if (g_units[0] != INT64_C(1000000000) || g_units[1] != INT64_C(1000000)
        || g_units[2] != 1000 || g_units[3] != 1 || g_api.ns(0.5) != YRT_MS(500) || names())
        return 1;
    g_api.close(&tl);
    g_api.close(&tl);
    if (g_api.open(&tl, NULL) || g_api.error(&tl)[0] == '\0') return 1;
    if (open_tl(&tl, ev, keys)) return 1;
    if (g_api.clamped(&tl, 1) != 0 || g_api.peek(&tl, 1, 0, 1, NULL, 0) != 0) return 1;
    /* The builder: C99 and C++17 alike. */
    memset(&up, 0, sizeof up);
    up.to = 0.5f;
    up.duration_ns = YRT_MS(100);
    up.ease = YTL_EASE_COSINE;
    q = ytl_seq_on(&tl, 1);
    ytl_on(&q, 0);
    ytl_wait(&q, YRT_MS(500));
    ytl_off(&q, 0);
    ytl_trigger(&q, 12);
    ytl_to(&q, 2, &up);
    ytl_wait(&q, YRT_MS(600));
    up.to = 0.0f;
    ytl_then(&q, 2, &up);
    if (q.err != 0) return 1;
#ifndef __cplusplus
    /* The op table: designated initializers, C99 (C++20 in
     * timeline_ops.cpp). */
    {
        static const ytl_op trial[] = {
            YTL_ON(0),
            YTL_WAIT(YRT_MS(500)),
            YTL_OFF(0), YTL_SET_VALUE(1, 2.0f), YTL_TRIGGER(12), YTL_MARK(3, 7),
            YTL_TO(2, .to = 0.5f, .duration_ns = YRT_MS(100), .ease = YTL_EASE_COSINE),
            YTL_WAIT(YRT_MS(600)),
            YTL_THEN(2, .to = 0.0f, .duration_ns = YRT_MS(100), .ease = YTL_EASE_COSINE),
            YTL_AT(0),
        };
        int bad = 0;
        if (open_tl(&tl, ev, keys)) return 1;
        if (ytl_check_ops(&tl, 1, trial, YTL_COUNT(trial), &bad) != 0) return 1;
        q = ytl_seq_on(&tl, 1);
        if (ytl_run(&q, trial, YTL_COUNT(trial)) != 0) return 1;
    }
#endif
    g_api.close(&tl);
    return 0;
}
