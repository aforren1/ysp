/* ysp_audio_api.h - the table that ysp.audio and ysp_audio export (ysp_share.h)
 *
 * Include ysp/audio.h first (its declarations; the owner also its
 * implementation), then this file.
 *
 *   owner     #define YSP_AUDIO_API_OWNER: ysp_audio_api_fill() fills a table
 *             with the header's functions and the host's acquire/release.
 *   consumer  ysp_audio_api_check() refuses a table it cannot use.
 *             #define YSP_AUDIO_API_SHIMS in ONE translation unit: it defines
 *             ysp_audio_api_ptr and every yau_* function in the list as a
 *             forwarder through it. That unit must not compile audio.h's
 *             implementation. With YSP_SHARE_PYTHON, ysp_audio_api_import_py()
 *             imports ysp.audio and sets the pointer; with YSP_SHARE_MEX,
 *             ysp_audio_api_import_mex() loads ysp_audio.<mexext> from the
 *             consumer's own directory.
 *
 * The list: the output, stream and WAV functions a consumer's objects call on
 * an audio handle it was given (ysp/video.h calls 16 of them).
 * Not in it: open and close (the owner's), the arena (the owner's memory:
 * a consumer gives a stream or WAV ring its own memory), capture, the
 * detectors and the WAV writer. The signatures are the header's own: the
 * owner's assignments and the shims do not compile when one drifts.
 */
#ifndef YSP_AUDIO_API_H
#define YSP_AUDIO_API_H

#ifndef YSP_AUDIO_H_INCLUDED
#error "include ysp/audio.h before ysp_audio_api.h"
#endif
#include "ysp_share.h"

#define YSP_AUDIO_API_CAPSULE "ysp.audio._C_API"   /* Python                */
#define YSP_AUDIO_API_SYMBOL  "ysp_audio_api_table"   /* MEX: the exported fn */
#define YSP_AUDIO_API_OWNER_MEX "ysp_audio"        /* MEX: the owner's file */

/* X(return type, name, (parameters), (arguments)); XV for void returns. */
#define YSP_AUDIO_API_FUNCS(X, XV) \
    X(const char*, yau_version, (void), ()) \
    X(const char*, yau_strerror, (int code), (code)) \
    X(const char*, yau_error, (const yau_audio* au), (au)) \
    X(bool, yau_is_open, (const yau_audio* au), (au)) \
    XV(yau_get_caps, (const yau_audio* au, yau_caps* out), (au, out)) \
    X(int, yau_describe, (const yau_audio* au, char* buf, size_t cap), (au, buf, cap)) \
    X(yau_id, yau_play_at, (yau_audio* au, yau_buf buf, int64_t t_ns), (au, buf, t_ns)) \
    X(yau_id, yau_play, (yau_audio* au, const yau_play_desc* d), (au, d)) \
    X(int, yau_cancel, (yau_audio* au, yau_id id), (au, id)) \
    X(int, yau_stop_at, (yau_audio* au, yau_id id, int64_t t_ns, int64_t ramp_ns), (au, id, t_ns, ramp_ns)) \
    X(int, yau_gain_at, (yau_audio* au, yau_id id, int64_t t_ns, float db, int64_t ramp_ns), (au, id, t_ns, db, ramp_ns)) \
    X(int, yau_update, (yau_audio* au), (au)) \
    X(int, yau_result, (yau_audio* au, yau_id id, yau_onset* out), (au, id, out)) \
    X(int, yau_wait, (yau_audio* au, yau_id id, int64_t timeout_ns, yau_onset* out), (au, id, timeout_ns, out)) \
    X(int64_t, yau_lead_ns, (const yau_audio* au), (au)) \
    X(int64_t, yau_frame_at, (const yau_audio* au, int64_t t_ns), (au, t_ns)) \
    X(int64_t, yau_time_of, (const yau_audio* au, int64_t frame), (au, frame)) \
    X(bool, yau_stream_open, (yau_audio* au, yau_stream* st, const yau_stream_desc* d), (au, st, d)) \
    XV(yau_stream_close, (yau_audio* au, yau_stream* st), (au, st)) \
    X(yau_id, yau_play_stream, (yau_audio* au, yau_stream* st, int64_t t_ns), (au, st, t_ns)) \
    X(float*, yau_stream_acquire, (yau_stream* st, int64_t* frames), (st, frames)) \
    X(int, yau_stream_commit, (yau_stream* st, int64_t frames), (st, frames)) \
    X(int64_t, yau_stream_write, (yau_stream* st, const float* src, int64_t frames), (st, src, frames)) \
    X(int, yau_stream_end, (yau_stream* st), (st)) \
    X(int, yau_stream_reset, (yau_stream* st, int64_t first), (st, first)) \
    X(int, yau_stream_get_info, (const yau_stream* st, yau_stream_info* out), (st, out)) \
    X(int, yau_stream_sample_at, (yau_audio* au, const yau_stream* st, int64_t t_ns, int64_t* sample), (au, st, t_ns, sample)) \
    X(int, yau_stream_time_of, (yau_audio* au, const yau_stream* st, int64_t sample, int64_t* t_ns), (au, st, sample, t_ns)) \
    X(bool, yau_wav_open, (yau_audio* au, yau_wav* w, const yau_wav_desc* d), (au, w, d)) \
    XV(yau_wav_close, (yau_audio* au, yau_wav* w), (au, w)) \
    X(int64_t, yau_wav_feed, (yau_wav* w, int64_t max_frames), (w, max_frames)) \
    X(int, yau_wav_seek, (yau_wav* w, int64_t sample), (w, sample)) \
    X(bool, yau_wav_wants, (const yau_wav* w), (w)) \
    X(bool, yau_wav_step, (void* w), (w)) \
    XV(yau_wav_on_msg, (void* w, const void* msg, uint32_t seq), (w, msg, seq)) \
    X(const char*, yau_wav_error, (const yau_wav* w), (w))

/* The structs whose layout a consumer shares with the owner. */
#define YSP_AUDIO_API_SIZES(S) \
    S(yau_audio) S(yau_buf) S(yau_play_desc) S(yau_onset) S(yau_caps) S(yau_stream) \
    S(yau_stream_desc) S(yau_stream_info) S(yau_wav) S(yau_wav_desc) S(yau_reader)

#define YSP_AUDIO_API_F_(r, n, p, a) r (*n) p;
#define YSP_AUDIO_API_FV_(n, p, a) void (*n) p;
#define YSP_AUDIO_API_S_(t) uint32_t sizeof_##t;

typedef struct ysp_audio_api {
    ysp_share_head head;
    uint32_t max_channels;      /* YAU_MAX_CHANNELS                          */
    uint32_t max_voices;        /* YAU_MAX_VOICES                            */
    YSP_AUDIO_API_SIZES(YSP_AUDIO_API_S_)
    YSP_AUDIO_API_FUNCS(YSP_AUDIO_API_F_, YSP_AUDIO_API_FV_)
} ysp_audio_api;

#undef YSP_AUDIO_API_F_
#undef YSP_AUDIO_API_FV_
#undef YSP_AUDIO_API_S_

/* 0, or -1 with the reason in err (VERSIONS in ysp_share.h). */
YSP_SHARE_FN int ysp_audio_api_check(const ysp_audio_api* a, uint32_t host, char* err, size_t cap) {
    if (ysp_share_check_head(a ? &a->head : NULL, "audio", YAU_VERSION_STRING, host, sizeof(ysp_audio_api), err, cap) < 0)
        return -1;
    if (ysp_share_check_size("audio", "YAU_MAX_CHANNELS", a->max_channels, YAU_MAX_CHANNELS, err, cap) < 0) return -1;
    if (ysp_share_check_size("audio", "YAU_MAX_VOICES", a->max_voices, YAU_MAX_VOICES, err, cap) < 0) return -1;
#define YSP_AUDIO_API_C_(t) if (ysp_share_check_size("audio", #t, a->sizeof_##t, sizeof(t), err, cap) < 0) return -1;
    YSP_AUDIO_API_SIZES(YSP_AUDIO_API_C_)
#undef YSP_AUDIO_API_C_
    return 0;
}

#ifdef YSP_AUDIO_API_OWNER
YSP_SHARE_FN void ysp_audio_api_fill(ysp_audio_api* a, uint32_t host, void* (*acquire)(void*, char*, size_t),
                                     void (*release)(void*)) {
    memset(a, 0, sizeof *a);
    a->head.abi = YSP_SHARE_ABI;
    a->head.size = (uint32_t)sizeof *a;
    a->head.host = host;
    a->head.library = "audio";
    a->head.version = YAU_VERSION_STRING;
    a->head.acquire = acquire;
    a->head.release = release;
    a->max_channels = YAU_MAX_CHANNELS;
    a->max_voices = YAU_MAX_VOICES;
#define YSP_AUDIO_API_S_(t) a->sizeof_##t = (uint32_t)sizeof(t);
#define YSP_AUDIO_API_F_(r, n, p, x) a->n = n;
#define YSP_AUDIO_API_FV_(n, p, x) a->n = n;
    YSP_AUDIO_API_SIZES(YSP_AUDIO_API_S_)
    YSP_AUDIO_API_FUNCS(YSP_AUDIO_API_F_, YSP_AUDIO_API_FV_)
#undef YSP_AUDIO_API_S_
#undef YSP_AUDIO_API_F_
#undef YSP_AUDIO_API_FV_
}
#endif

#ifdef YSP_AUDIO_API_SHIMS
#ifdef YSP_AUDIO_IMPLEMENTATION
#error "the shims replace audio.h's implementation: do not compile both in one module"
#endif
#ifdef __cplusplus
extern "C" {
#endif
const ysp_audio_api* ysp_audio_api_ptr = NULL;
#define YSP_AUDIO_API_F_(r, n, p, a) r n p { return ysp_audio_api_ptr->n a; }
#define YSP_AUDIO_API_FV_(n, p, a) void n p { ysp_audio_api_ptr->n a; }
YSP_AUDIO_API_FUNCS(YSP_AUDIO_API_F_, YSP_AUDIO_API_FV_)
#undef YSP_AUDIO_API_F_
#undef YSP_AUDIO_API_FV_
#ifdef __cplusplus
}
#endif

#ifdef YSP_SHARE_PYTHON
/* Imports ysp.audio and sets ysp_audio_api_ptr; 0, or -1 with ImportError
 * set. Call it from the consumer's module init. */
static int ysp_audio_api_import_py(void) {
    char err[256];
    const ysp_audio_api* a = (const ysp_audio_api*)PyCapsule_Import(YSP_AUDIO_API_CAPSULE, 0);
    if (!a) return -1;
    if (ysp_audio_api_check(a, YSP_SHARE_HOST_PYTHON, err, sizeof err) < 0) {
        PyErr_SetString(PyExc_ImportError, err);
        return -1;
    }
    ysp_audio_api_ptr = a;
    return 0;
}
#endif

#ifdef YSP_SHARE_MEX
static void* ysp_audio_api_module = NULL;
/* Loads ysp_audio.<mexext> from this module's directory (once) and sets
 * ysp_audio_api_ptr; 0, or -1 with err. */
static int ysp_audio_api_import_mex(char* err, size_t cap) {
    const ysp_audio_api* a;
    void* m = NULL;
    if (ysp_audio_api_ptr) return 0;
    a = (const ysp_audio_api*)ysp_share_mex_load(YSP_AUDIO_API_OWNER_MEX, YSP_AUDIO_API_SYMBOL, &m, err, cap);
    if (!a) return -1;
    if (ysp_audio_api_check(a, YSP_SHARE_HOST_MEX, err, cap) < 0) { ysp_share_mex_unload(m); return -1; }
    ysp_audio_api_module = m;
    ysp_audio_api_ptr = a;
    return 0;
}
#endif
#endif /* YSP_AUDIO_API_SHIMS */

#endif /* YSP_AUDIO_API_H */
