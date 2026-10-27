/* ysp_timeline_api.h - the table that ysp.timeline and ysp_timeline export (ysp_share.h)
 *
 * Include ysp/timeline.h first, with YTL_MAX_CHANNELS 1024 as the owners
 * compile it (the consumer's check refuses another value), then this file.
 * The roles and macros are those of ysp_audio_api.h: YSP_TIMELINE_API_OWNER,
 * ysp_timeline_api_check(), YSP_TIMELINE_API_SHIMS (one unit, without
 * timeline.h's implementation; define YSP_TIMELINE_IMPLEMENTATION_GUARD
 * there too, so that ysp/video.h's and ysp/gfx.h's implementations do not
 * pull timeline.h's in), ysp_timeline_api_import_py() and
 * ysp_timeline_api_import_mex().
 *
 * The list: the frame-time functions a consumer calls on a timeline it was
 * given (ysp/video.h calls ytl_skip, ytl_anchor, ytl_rt_time, ytl_pause,
 * ytl_base_time, ytl_window, ytl_lead, ytl_is_open, ytl_get_rate; a frame
 * loop calls ytl_evaluate and ytl_values). Not in it: open and close, and
 * building (events, tracks, sequences, tweens), which the owner's binding
 * does because it owns the storage.
 *
 * ytl_evaluate through the Python table is the binding's evaluate: tween
 * callbacks run (it takes the GIL itself), Timeline.bind() targets are
 * written, the default frame index moves on, and a callback that raised
 * makes it return YSP_SHARE_ERR_HOST with the exception set.
 */
#ifndef YSP_TIMELINE_API_H
#define YSP_TIMELINE_API_H

#ifndef YSP_TIMELINE_H_INCLUDED
#error "include ysp/timeline.h before ysp_timeline_api.h"
#endif
#include "ysp_share.h"

#define YSP_TIMELINE_API_CAPSULE "ysp.timeline._C_API"
#define YSP_TIMELINE_API_SYMBOL  "ysp_timeline_api"
#define YSP_TIMELINE_API_OWNER_MEX "ysp_timeline"
#define YSP_TIMELINE_API_CHANNELS 1024   /* YTL_MAX_CHANNELS in both owners */

#define YSP_TIMELINE_API_FUNCS(X, XV) \
    X(const char*, ytl_version, (void), ()) \
    X(const char*, ytl_strerror, (int code), (code)) \
    X(const char*, ytl_error, (const ytl_timeline* tl), (tl)) \
    X(bool, ytl_is_open, (const ytl_timeline* tl), (tl)) \
    X(int, ytl_anchor, (ytl_timeline* tl, int base, int64_t rt_ns, int64_t base_time_ns), (tl, base, rt_ns, base_time_ns)) \
    X(int, ytl_pause, (ytl_timeline* tl, int base, int64_t rt_ns), (tl, base, rt_ns)) \
    X(int, ytl_resume, (ytl_timeline* tl, int base, int64_t rt_ns), (tl, base, rt_ns)) \
    X(int, ytl_stop, (ytl_timeline* tl, int base), (tl, base)) \
    X(int, ytl_skip, (ytl_timeline* tl, int base, int64_t rt_ns, int64_t base_time_ns), (tl, base, rt_ns, base_time_ns)) \
    X(bool, ytl_base_time, (const ytl_timeline* tl, int base, int64_t rt_ns, int64_t* out), (tl, base, rt_ns, out)) \
    X(int, ytl_rate, (ytl_timeline* tl, int base, int64_t rt_ns, int32_t num, int32_t den), (tl, base, rt_ns, num, den)) \
    X(bool, ytl_get_rate, (const ytl_timeline* tl, int base, int32_t* num, int32_t* den), (tl, base, num, den)) \
    X(bool, ytl_rt_time, (const ytl_timeline* tl, int base, int64_t bt_ns, int64_t* out), (tl, base, bt_ns, out)) \
    X(bool, ytl_window, (const ytl_timeline* tl, int base, const ytl_frame* f, int64_t* end), (tl, base, f, end)) \
    X(int, ytl_clamped, (ytl_timeline* tl, int base), (tl, base)) \
    X(int, ytl_evaluate, (ytl_timeline* tl, const ytl_frame* f, ytl_event* fired, int cap), (tl, f, fired, cap)) \
    X(bool, ytl_next_due, (const ytl_timeline* tl, int64_t* rt_ns), (tl, rt_ns)) \
    X(bool, ytl_horizon, (const ytl_timeline* tl, int base, int64_t* out), (tl, base, out)) \
    X(int, ytl_peek, (ytl_timeline* tl, int base, int64_t from_rt_ns, int64_t to_rt_ns, ytl_event* out, int cap), (tl, base, from_rt_ns, to_rt_ns, out, cap)) \
    X(double, ytl_lead, (const ytl_timeline* tl), (tl)) \
    X(const float*, ytl_values, (const ytl_timeline* tl), (tl)) \
    X(float, ytl_value, (const ytl_timeline* tl, int channel), (tl, channel)) \
    X(const ytl_event*, ytl_events, (const ytl_timeline* tl, int base, int* n), (tl, base, n)) \
    X(const ytl_event*, ytl_find, (const ytl_timeline* tl, int id), (tl, id))

#define YSP_TIMELINE_API_SIZES(S) S(ytl_timeline) S(ytl_frame) S(ytl_event)

#define YSP_TIMELINE_API_F_(r, n, p, a) r (*n) p;
#define YSP_TIMELINE_API_FV_(n, p, a) void (*n) p;
#define YSP_TIMELINE_API_S_(t) uint32_t sizeof_##t;

typedef struct ysp_timeline_api {
    ysp_share_head head;
    uint32_t max_channels;      /* YTL_MAX_CHANNELS                          */
    uint32_t max_bases;         /* YTL_MAX_BASES                             */
    YSP_TIMELINE_API_SIZES(YSP_TIMELINE_API_S_)
    YSP_TIMELINE_API_FUNCS(YSP_TIMELINE_API_F_, YSP_TIMELINE_API_FV_)
} ysp_timeline_api;

#undef YSP_TIMELINE_API_F_
#undef YSP_TIMELINE_API_FV_
#undef YSP_TIMELINE_API_S_

YSP_SHARE_FN int ysp_timeline_api_check(const ysp_timeline_api* a, uint32_t host, char* err, size_t cap) {
    if (ysp_share_check_head(a ? &a->head : NULL, "timeline", YTL_VERSION_STRING, host, sizeof(ysp_timeline_api), err,
                             cap) < 0)
        return -1;
    if (ysp_share_check_size("timeline", "YTL_MAX_CHANNELS", a->max_channels, YTL_MAX_CHANNELS, err, cap) < 0) return -1;
    if (ysp_share_check_size("timeline", "YTL_MAX_BASES", a->max_bases, YTL_MAX_BASES, err, cap) < 0) return -1;
#define YSP_TIMELINE_API_C_(t) if (ysp_share_check_size("timeline", #t, a->sizeof_##t, sizeof(t), err, cap) < 0) return -1;
    YSP_TIMELINE_API_SIZES(YSP_TIMELINE_API_C_)
#undef YSP_TIMELINE_API_C_
    return 0;
}

#ifdef YSP_TIMELINE_API_OWNER
YSP_SHARE_FN void ysp_timeline_api_fill(ysp_timeline_api* a, uint32_t host, void* (*acquire)(void*, char*, size_t),
                                        void (*release)(void*)) {
    memset(a, 0, sizeof *a);
    a->head.abi = YSP_SHARE_ABI;
    a->head.size = (uint32_t)sizeof *a;
    a->head.host = host;
    a->head.library = "timeline";
    a->head.version = YTL_VERSION_STRING;
    a->head.acquire = acquire;
    a->head.release = release;
    a->max_channels = YTL_MAX_CHANNELS;
    a->max_bases = YTL_MAX_BASES;
#define YSP_TIMELINE_API_S_(t) a->sizeof_##t = (uint32_t)sizeof(t);
#define YSP_TIMELINE_API_F_(r, n, p, x) a->n = n;
#define YSP_TIMELINE_API_FV_(n, p, x) a->n = n;
    YSP_TIMELINE_API_SIZES(YSP_TIMELINE_API_S_)
    YSP_TIMELINE_API_FUNCS(YSP_TIMELINE_API_F_, YSP_TIMELINE_API_FV_)
#undef YSP_TIMELINE_API_S_
#undef YSP_TIMELINE_API_F_
#undef YSP_TIMELINE_API_FV_
}
#endif

#ifdef YSP_TIMELINE_API_SHIMS
#ifdef YSP_TIMELINE_IMPLEMENTATION
#error "the shims replace timeline.h's implementation: do not compile both in one module"
#endif
#ifdef __cplusplus
extern "C" {
#endif
const ysp_timeline_api* ysp_timeline_api_ptr = NULL;
#define YSP_TIMELINE_API_F_(r, n, p, a) r n p { return ysp_timeline_api_ptr->n a; }
#define YSP_TIMELINE_API_FV_(n, p, a) void n p { ysp_timeline_api_ptr->n a; }
YSP_TIMELINE_API_FUNCS(YSP_TIMELINE_API_F_, YSP_TIMELINE_API_FV_)
#undef YSP_TIMELINE_API_F_
#undef YSP_TIMELINE_API_FV_
#ifdef __cplusplus
}
#endif

#ifdef YSP_SHARE_PYTHON
static int ysp_timeline_api_import_py(void) {
    char err[256];
    const ysp_timeline_api* a = (const ysp_timeline_api*)PyCapsule_Import(YSP_TIMELINE_API_CAPSULE, 0);
    if (!a) return -1;
    if (ysp_timeline_api_check(a, YSP_SHARE_HOST_PYTHON, err, sizeof err) < 0) {
        PyErr_SetString(PyExc_ImportError, err);
        return -1;
    }
    ysp_timeline_api_ptr = a;
    return 0;
}
#endif

#ifdef YSP_SHARE_MEX
static void* ysp_timeline_api_module = NULL;
static int ysp_timeline_api_import_mex(char* err, size_t cap) {
    const ysp_timeline_api* a;
    void* m = NULL;
    if (ysp_timeline_api_ptr) return 0;
    a = (const ysp_timeline_api*)ysp_share_mex_load(YSP_TIMELINE_API_OWNER_MEX, YSP_TIMELINE_API_SYMBOL, &m, err, cap);
    if (!a) return -1;
    if (ysp_timeline_api_check(a, YSP_SHARE_HOST_MEX, err, cap) < 0) { ysp_share_mex_unload(m); return -1; }
    ysp_timeline_api_module = m;
    ysp_timeline_api_ptr = a;
    return 0;
}
#endif
#endif /* YSP_TIMELINE_API_SHIMS */

#endif /* YSP_TIMELINE_API_H */
