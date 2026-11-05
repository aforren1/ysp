/* ysp_audio.c - MATLAB/Octave MEX binding for ysp/audio.h (builds to ysp_audio.<mexext>)
 *
 * Sound at a time on the ysp_rt clock for a Psychtoolbox experiment, with
 * the record of where each sound landed, and the one audio implementation
 * that the ysp-vis MEX files share (a movie's soundtrack).
 *
 *     h = ysp_audio('open', struct('backend', 'null'));     % [] or no struct: the default output
 *     b = ysp_audio('tone', h, struct('hz', 1000, 'duration', 0.1, 'peak', 0.03, 'ramp', 0.005));
 *     id = ysp_audio('play_at', h, b, ysp_audio('now') + 0.1);
 *     r = ysp_audio('wait', h, id);                         % r.onset, r.residual_ns, r.tier
 *     ysp_audio('close', h);
 *
 * TIMES are seconds (double), converted once to the nearest ns; a negative
 * or NaN time is refused. A record gives the exact ns as int64 beside each
 * time. Frame and sample numbers keep the header's numbering (0 is the
 * first). Output channels in a 'channels' list are 1-based.
 *
 * SAMPLES are a frames x channels matrix (double or single), as sound() and
 * audiowrite() take them; PsychPortAudio's channels x frames is the
 * transpose. A row vector is one channel.
 *
 * SHARING: the exported function ysp_audio_api_table() returns the table of
 * bindings/share/ysp_audio_api.h. Its acquire() takes a pointer to a handle
 * (uint64) and pins the device; 'close' refuses a pinned device
 * (ysp_audio:busy), and a cleared handle table leaves it open until the
 * last release().
 *
 * Build with build.m (miniaudio.h from YSP_MINIAUDIO_DIR or beside this
 * file). See README.md.
 */
#define PM_MOD "ysp_audio"

#define YSP_AUDIO_IMPLEMENTATION
#include "ysp/audio.h"
#define YSP_AUDIO_API_OWNER
#include "ysp_audio_api.h"

#include "ysp_mex_util.h"

#include <stddef.h>

/* The binding's own version (README.md, "Versions"); the second output of
 * 'version' is the header's. */
#define AUM_VERSION "1.0.2"   /* 1.0.2: WAV paths as UTF-8 (ysp/audio.h 0.7.3) */
#define AUM_S_MAX 4611686018.0

/* A path as UTF-8, which ysp/audio.h takes from 0.7.3; mxMalloc'd for this
 * call. mxArrayToString() gives MATLAB's local code page, where a folder
 * named with Omega or a CJK ideograph is lost. Octave keeps char as UTF-8
 * bytes and has no mxArrayToUTF8String(). */
static char* aum_path(const mxArray* a) {
    if (!mxIsChar(a)) pm_err("arg", "path must be a string");
#ifdef HAVE_OCTAVE
    return mxArrayToString(a);
#else
    return mxArrayToUTF8String(a);
#endif
}

enum { AUM_AUDIO = 1, AUM_BUFFER, AUM_STREAM, AUM_WAV, AUM_CAPTURE };

typedef struct aum_child {
    struct aum_child* next;
    int   kind;                 /* AUM_STREAM or AUM_WAV */
    int   open;
    void* obj;                  /* yau_stream* or yau_wav* */
    char* path;
} aum_child;

typedef struct aum_audio {
    int        kind;            /* AUM_AUDIO */
    int        pins;            /* acquire() calls not yet released */
    int        orphan;          /* the table was cleared while pinned */
    uint32_t   gen;             /* arena generation */
    uint64_t   id;
    aum_child* children;
    char*      device;
    yau_audio  au;
} aum_audio;

typedef struct aum_ref {         /* a buffer, stream or WAV: a part of an audio */
    int        kind;
    uint64_t   audio_id;
    aum_audio* a;               /* valid while audio_id is in the table */
    uint32_t   gen;
    yau_buf    buf;             /* AUM_BUFFER */
    aum_child* c;               /* AUM_STREAM, AUM_WAV */
} aum_ref;

typedef struct aum_capture {
    int          kind;          /* AUM_CAPTURE */
    char*        device;
    yau_capture  c;
} aum_capture;

static ysp_audio_api g_api;

/* ---- lifetime ---------------------------------------------------------------- */

static void aum_audio_shut(aum_audio* a) {
    aum_child* c;
    for (c = a->children; c; c = c->next) {
        if (!c->open) continue;
        c->open = 0;
        if (c->kind == AUM_STREAM) yau_stream_close(&a->au, (yau_stream*)c->obj);
        else yau_wav_close(&a->au, (yau_wav*)c->obj);
    }
    yau_close(&a->au);
    while (a->children) {
        c = a->children->next;
        free(a->children->obj);
        free(a->children->path);
        free(a->children);
        a->children = c;
    }
    free(a->device);
    free(a);
}

/* pm_cleanup (clear ysp_audio, clear mex, exit) and 'close'. A pinned
 * device stays open for the module that holds it; its release() frees it. */
static void aum_destroy(void* obj) {
    int kind = obj ? *(int*)obj : 0;
    if (!obj) return;
    if (kind == AUM_AUDIO) {
        aum_audio* a = (aum_audio*)obj;
        if (a->pins > 0) { a->orphan = 1; return; }
        aum_audio_shut(a);
    } else if (kind == AUM_CAPTURE) {
        aum_capture* c = (aum_capture*)obj;
        yau_capture_close(&c->c);
        free(c->device);
        free(c);
    } else {
        free(obj);
    }
}

static void* aum_find(uint64_t id) {
    pm_node* n;
    for (n = pm_list; n; n = n->next)
        if (n->id == id) return n->obj;
    return NULL;
}

static uint64_t aum_id(const mxArray* a) {
    if (mxIsUint64(a) && mxGetNumberOfElements(a) == 1) return *(const uint64_t*)mxGetData(a);
    if (mxIsDouble(a) && mxGetNumberOfElements(a) == 1 && mxGetScalar(a) >= 1) return (uint64_t)mxGetScalar(a);
    pm_err("handle", "handle must be a scalar uint64");
    return 0;
}

static aum_audio* aum_get_audio(const mxArray* x) {
    aum_audio* a = (aum_audio*)pm_lookup(x);
    if (a->kind != AUM_AUDIO) pm_err("handle", "an audio handle (from 'open') is needed here");
    return a;
}

static aum_ref* aum_get_ref(const mxArray* x, int kind) {
    aum_ref* r = (aum_ref*)pm_lookup(x);
    if (r->kind != kind && !(kind == AUM_STREAM && r->kind == AUM_WAV))
        pm_err("handle", kind == AUM_BUFFER ? "a buffer handle is needed here" : kind == AUM_WAV ? "a WAV handle (from 'wav_open') is needed here" : "a stream handle is needed here");
    if (aum_find(r->audio_id) != (void*)r->a) pm_err("closed", "the audio device of this handle is closed");
    if (r->kind == AUM_BUFFER && r->gen != r->a->gen) pm_err("arg", "the buffer was freed by 'arena_reset'");
    if ((r->kind == AUM_STREAM || r->kind == AUM_WAV) && !r->c->open) pm_err("closed", "the stream is closed");
    return r;
}

static yau_stream* aum_stream_of(aum_ref* r) {
    return r->kind == AUM_STREAM ? (yau_stream*)r->c->obj : &((yau_wav*)r->c->obj)->stream;
}

/* ---- sharing ----------------------------------------------------------------- */

static void* aum_api_acquire(void* ref, char* err, size_t cap) {
    aum_audio* a = ref ? (aum_audio*)aum_find(*(const uint64_t*)ref) : NULL;
    if (!a || a->kind != AUM_AUDIO) { snprintf(err, cap, "ysp_audio: not an open audio handle"); return NULL; }
    if (!yau_is_open(&a->au)) { snprintf(err, cap, "ysp_audio: the device is closed"); return NULL; }
    a->pins++;
    return &a->au;
}

static void aum_api_release(void* p) {
    aum_audio* a;
    if (!p) return;
    a = (aum_audio*)(void*)((char*)p - offsetof(aum_audio, au));
    if (--a->pins == 0 && a->orphan) aum_audio_shut(a);
}

YSP_SHARE_EXPORT const ysp_audio_api* ysp_audio_api_table(void) {
    if (!g_api.head.abi) ysp_audio_api_fill(&g_api, YSP_SHARE_HOST_MEX, aum_api_acquire, aum_api_release);
    return &g_api;
}

/* ---- arguments --------------------------------------------------------------- */

/* Seconds to the nearest ns: fma() gives the product's rounding error (as
 * the Python binding). */
static int64_t aum_s_ns(double s, const char* what) {
    volatile double p;
    double e, n, r;
    if (!(s >= 0.0 && s < AUM_S_MAX)) pm_err("arg", "%s: seconds, >= 0 and below 2^62 ns (a negative or NaN time is refused)", what);
    p = s * 1e9;
    e = fma(s, 1e9, -p);
    n = floor(p + 0.5);
    r = (p - n) + e;
    if (r > 0.5) n += 1.0;
    else if (r < -0.5) n -= 1.0;
    return (int64_t)n;
}

static int64_t aum_time(const mxArray* a, const char* what) { return aum_s_ns(pm_scalar(a, what), what); }
static double aum_s(int64_t ns) { return (double)ns / 1e9; }

static int64_t aum_frames_of(double s, uint32_t rate, const char* what) {
    if (!(s >= 0.0 && s < 1e9)) pm_err("arg", "%s: seconds, >= 0", what);
    return (int64_t)floor(s * (double)rate + 0.5);
}

static mxArray* aum_i64(int64_t v) {
    mxArray* a = mxCreateNumericMatrix(1, 1, mxINT64_CLASS, mxREAL);
    *(int64_t*)mxGetData(a) = v;
    return a;
}

static double aum_opt(const mxArray* s, const char* name, double def) {
    const mxArray* f = s ? pm_field(s, name) : NULL;
    return f ? pm_scalar(f, name) : def;
}

static char* aum_opt_str(const mxArray* s, const char* name) {
    const mxArray* f = s ? pm_field(s, name) : NULL;
    char* v;
    if (!f) return NULL;
    v = pm_string(f, name);
    return v;
}

static const char* const aum_backends[] = { "auto", "wasapi", "coreaudio", "alsa", "pulse", "web", "null" };
static const char* const aum_samples[] = { "f32", "s16", "s24" };

static int aum_opt_enum(const mxArray* s, const char* name, const char* const* names, int n) {
    const mxArray* f = s ? pm_field(s, name) : NULL;
    return f ? pm_enum(f, name, names, n) : 0;
}

static char* aum_strdup(const char* s) {
    char* d;
    if (!s) return NULL;
    d = (char*)malloc(strlen(s) + 1);
    if (!d) pm_err("memory", "out of memory");
    strcpy(d, s);
    return d;
}

static uint64_t aum_mask(const mxArray* m) {
    double v[64];
    int n, i;
    uint64_t mask = 0;
    if (!m) return 0;
    n = pm_vector(m, "channels", v, 64);
    for (i = 0; i < n; i++) {
        if (v[i] != floor(v[i]) || v[i] < 1 || v[i] > 64) pm_err("arg", "channels: numbers 1 to 64");
        mask |= (uint64_t)1 << (int)(v[i] - 1);
    }
    return mask;
}

/* frames x channels (a row vector: one channel) to interleaved floats; malloc. */
static float* aum_samples_in(const mxArray* m, int64_t* frames, int* ch) {
    size_t r, c, i, k, n;
    float* out;
    if ((!mxIsDouble(m) && !mxIsSingle(m)) || mxIsComplex(m)) pm_err("arg", "samples: a real double or single matrix");
    r = mxGetM(m);
    c = mxGetN(m);
    if (r == 1) { r = c; c = 1; }
    if (r == 0 || c == 0) pm_err("arg", "samples: no samples");
    if (c > YAU_MAX_CHANNELS) pm_err("arg", "samples: at most %d channels (columns)", YAU_MAX_CHANNELS);
    n = r * c;
    out = (float*)malloc(n * sizeof(float));
    if (!out) pm_err("memory", "out of memory");
    for (i = 0; i < r; i++)
        for (k = 0; k < c; k++) {
            double v = mxIsDouble(m) ? mxGetPr(m)[i + k * r] : (double)((const float*)mxGetData(m))[i + k * r];
            if (!isfinite(v)) { free(out); pm_err("arg", "samples: sample %d is not finite", (int)(i + 1)); }
            out[i * c + k] = (float)v;
        }
    *frames = (int64_t)r;
    *ch = (int)c;
    return out;
}

static mxArray* aum_samples_out(const float* x, int64_t frames, int ch) {
    mxArray* m = mxCreateNumericMatrix((size_t)frames, (size_t)ch, mxSINGLE_CLASS, mxREAL);
    float* d = (float*)mxGetData(m);
    int64_t i;
    int k;
    for (i = 0; i < frames; i++)
        for (k = 0; k < ch; k++) d[i + k * frames] = x[i * ch + k];
    return m;
}

static void aum_fail(aum_audio* a, int code) {
    const char* id = code == YAU_ERR_ARG ? "arg" : code == YAU_ERR_CLOSED ? "closed" : code == YAU_ERR_FULL ? "full"
                   : code == YAU_ERR_FORMAT ? "format" : code == YAU_ERR_NOT_FOUND ? "not_found"
                   : code == YAU_ERR_LOST ? "lost" : code == YAU_ERR_BUSY ? "busy" : code == YAU_ERR_TIMEOUT ? "timeout"
                   : code == YAU_ERR_IO ? "io" : "error";
    const char* msg = a ? yau_error(&a->au) : "";
    pm_err(id, "%s", msg && msg[0] ? msg : yau_strerror(code));
}

static const char* aum_backend_name(int b) {
    return b >= 0 && b < 7 ? aum_backends[b] : b == YAU_BACKEND_CUSTOM ? "custom" : "?";
}

/* ---- records ------------------------------------------------------------------ */

static const char* const aum_onset_fields[] = { "id", "target", "target_ns", "onset", "onset_ns", "residual", "residual_ns",
                                                "start_frame", "end_frame", "device_pos", "rendered_at", "rendered_at_ns",
                                                "buffer_id", "flags", "tier", "sample", "gap_frames", "pending" };

static mxArray* aum_onset(const yau_onset* r, int pending) {
    mxArray* s = mxCreateStructMatrix(1, 1, 18, (const char**)aum_onset_fields);
    mxSetField(s, 0, "id", mxCreateDoubleScalar((double)r->id));
    mxSetField(s, 0, "target", mxCreateDoubleScalar(aum_s(r->target)));
    mxSetField(s, 0, "target_ns", aum_i64(r->target));
    mxSetField(s, 0, "onset", mxCreateDoubleScalar(aum_s(r->onset)));
    mxSetField(s, 0, "onset_ns", aum_i64(r->onset));
    mxSetField(s, 0, "residual", mxCreateDoubleScalar(aum_s(r->residual)));
    mxSetField(s, 0, "residual_ns", aum_i64(r->residual));
    mxSetField(s, 0, "start_frame", mxCreateDoubleScalar((double)r->start_frame));
    mxSetField(s, 0, "end_frame", mxCreateDoubleScalar((double)r->end_frame));
    mxSetField(s, 0, "device_pos", mxCreateDoubleScalar((double)r->device_pos));
    mxSetField(s, 0, "rendered_at", mxCreateDoubleScalar(aum_s(r->rendered_at)));
    mxSetField(s, 0, "rendered_at_ns", aum_i64(r->rendered_at));
    mxSetField(s, 0, "buffer_id", mxCreateDoubleScalar((double)r->buffer_id));
    mxSetField(s, 0, "flags", mxCreateDoubleScalar((double)r->flags));
    mxSetField(s, 0, "tier", mxCreateDoubleScalar((double)r->tier));
    mxSetField(s, 0, "sample", mxCreateDoubleScalar((double)r->sample));
    mxSetField(s, 0, "gap_frames", mxCreateDoubleScalar((double)r->gap_frames));
    mxSetField(s, 0, "pending", mxCreateLogicalScalar(pending != 0));
    return s;
}

static mxArray* aum_claim(int64_t ns) {
    return ns < 0 ? mxCreateDoubleMatrix(0, 0, mxREAL) : mxCreateDoubleScalar(aum_s(ns));
}

static mxArray* aum_caps(aum_audio* a) {
    static const char* const f[] = { "backend", "name", "rate", "channels", "period", "period_frames", "buffer",
                                     "buffer_frames", "exclusive", "pos_source", "drift_ppm", "fit_spread", "fit_points",
                                     "lead", "xruns", "worst_tier", "os_latency", "os_stream_latency", "os_latency_src",
                                     "gaps", "queue" };
    yau_caps c;
    mxArray* s = mxCreateStructMatrix(1, 1, 21, (const char**)f);
    double r;
    yau_get_caps(&a->au, &c);
    r = c.rate ? (double)c.rate : 1.0;
    mxSetField(s, 0, "backend", mxCreateString(aum_backend_name(c.backend)));
    mxSetField(s, 0, "name", mxCreateString(c.name));
    mxSetField(s, 0, "rate", mxCreateDoubleScalar(c.rate));
    mxSetField(s, 0, "channels", mxCreateDoubleScalar(c.channels));
    mxSetField(s, 0, "period", mxCreateDoubleScalar(c.period / r));
    mxSetField(s, 0, "period_frames", mxCreateDoubleScalar(c.period));
    mxSetField(s, 0, "buffer", mxCreateDoubleScalar(c.buffer / r));
    mxSetField(s, 0, "buffer_frames", mxCreateDoubleScalar(c.buffer));
    mxSetField(s, 0, "exclusive", mxCreateLogicalScalar(c.exclusive));
    mxSetField(s, 0, "pos_source", mxCreateString(c.pos_source == YAU_POS_DEVICE ? "device" : "callback"));
    mxSetField(s, 0, "drift_ppm", mxCreateDoubleScalar(c.drift_ppm));
    mxSetField(s, 0, "fit_spread", mxCreateDoubleScalar(c.fit_spread_ns / 1e9));
    mxSetField(s, 0, "fit_points", mxCreateDoubleScalar(c.fit_points));
    mxSetField(s, 0, "lead", mxCreateDoubleScalar(aum_s(c.lead_ns)));
    mxSetField(s, 0, "xruns", mxCreateDoubleScalar(c.xruns));
    mxSetField(s, 0, "worst_tier", mxCreateDoubleScalar(c.worst_tier));
    mxSetField(s, 0, "os_latency", aum_claim(c.os_latency_ns));
    mxSetField(s, 0, "os_stream_latency", aum_claim(c.os_stream_latency_ns));
    mxSetField(s, 0, "os_latency_src", mxCreateString(c.os_latency_src));
    mxSetField(s, 0, "gaps", mxCreateDoubleScalar(c.gaps));
    mxSetField(s, 0, "queue", mxCreateDoubleScalar(c.queue));
    return s;
}

static mxArray* aum_stream_info(const yau_stream* st) {
    static const char* const f[] = { "state", "started", "ready", "id", "first", "written", "read", "fill", "preroll",
                                     "next", "start_frame", "frames_to_start", "origin", "end", "gaps", "gap_frames",
                                     "discarded" };
    static const char* const states[] = { "idle", "queued", "waiting", "playing", "ended" };
    yau_stream_info i;
    mxArray* s = mxCreateStructMatrix(1, 1, 17, (const char**)f);
    yau_stream_get_info(st, &i);
    mxSetField(s, 0, "state", mxCreateString(i.state >= 0 && i.state <= 4 ? states[i.state] : "?"));
    mxSetField(s, 0, "started", mxCreateLogicalScalar(i.started));
    mxSetField(s, 0, "ready", mxCreateLogicalScalar(i.ready));
    mxSetField(s, 0, "id", mxCreateDoubleScalar((double)i.id));
    mxSetField(s, 0, "first", mxCreateDoubleScalar((double)i.first));
    mxSetField(s, 0, "written", mxCreateDoubleScalar((double)i.written));
    mxSetField(s, 0, "read", mxCreateDoubleScalar((double)i.read));
    mxSetField(s, 0, "fill", mxCreateDoubleScalar((double)i.fill));
    mxSetField(s, 0, "preroll", mxCreateDoubleScalar((double)i.preroll));
    mxSetField(s, 0, "next", mxCreateDoubleScalar((double)i.next));
    mxSetField(s, 0, "start_frame", mxCreateDoubleScalar((double)i.start_frame));
    mxSetField(s, 0, "frames_to_start", mxCreateDoubleScalar((double)i.frames_to_start));
    mxSetField(s, 0, "origin", mxCreateDoubleScalar((double)i.origin));
    mxSetField(s, 0, "end", mxCreateDoubleScalar((double)i.end));
    mxSetField(s, 0, "gaps", mxCreateDoubleScalar((double)i.gaps));
    mxSetField(s, 0, "gap_frames", mxCreateDoubleScalar((double)i.gap_frames));
    mxSetField(s, 0, "discarded", mxCreateDoubleScalar((double)i.discarded));
    return s;
}

/* ---- buffers and streams ------------------------------------------------------- */

static mxArray* aum_ref_out(aum_audio* a, int kind, yau_buf b, aum_child* c) {
    aum_ref* r = (aum_ref*)calloc(1, sizeof(aum_ref));
    if (!r) pm_err("memory", "out of memory");
    r->kind = kind;
    r->audio_id = a->id;
    r->a = a;
    r->gen = a->gen;
    r->buf = b;
    r->c = c;
    return pm_handle_out(pm_register(r));
}

static mxArray* aum_buffer_out(aum_audio* a, yau_buf b, uint32_t id) {
    if (!b.frames) aum_fail(a, YAU_ERR_ARG);
    b.id = id;
    return aum_ref_out(a, AUM_BUFFER, b, NULL);
}

static aum_child* aum_child_new(aum_audio* a, int kind, size_t bytes) {
    aum_child* c = (aum_child*)calloc(1, sizeof(aum_child));
    if (!c || !(c->obj = calloc(1, bytes))) { free(c); pm_err("memory", "out of memory"); }
    c->kind = kind;
    c->next = a->children;
    a->children = c;
    return c;
}

static uint32_t aum_rate(aum_audio* a) { return a->au.dcaps.rate ? a->au.dcaps.rate : 48000; }

/* ---- module commands ----------------------------------------------------------- */

static ma_backend aum_ma_backend(int b, int* ok) {
    *ok = 1;
    switch (b) {
    case YAU_BACKEND_WASAPI: return ma_backend_wasapi;
    case YAU_BACKEND_COREAUDIO: return ma_backend_coreaudio;
    case YAU_BACKEND_ALSA: return ma_backend_alsa;
    case YAU_BACKEND_PULSE: return ma_backend_pulseaudio;
    case YAU_BACKEND_WEB: return ma_backend_webaudio;
    case YAU_BACKEND_NULL: return ma_backend_null;
    default: *ok = 0; return ma_backend_null;
    }
}

/* The names desc.device matches, from the miniaudio compiled in here (the
 * header lists none). */
static mxArray* aum_devices(int nrhs, const mxArray* prhs[]) {
    static const char* const f[] = { "name", "default", "backend" };
    int b = nrhs > 1 ? pm_enum(prhs[1], "backend", aum_backends, 7) : 0, capture = nrhs > 2 ? pm_bool(prhs[2], "capture") : 0, ok;
    ma_backend mb = aum_ma_backend(b, &ok);
    ma_context ctx;
    ma_device_info *pi = NULL, *ci = NULL, *infos;
    ma_uint32 pn = 0, cn = 0, n, i;
    ma_result r = ma_context_init(ok ? &mb : NULL, ok ? 1 : 0, NULL, &ctx);
    mxArray* s;
    if (r == MA_SUCCESS) r = ma_context_get_devices(&ctx, &pi, &pn, &ci, &cn);
    if (r != MA_SUCCESS) pm_err("error", "no device list (%s)", ma_result_description(r));
    infos = capture ? ci : pi;
    n = capture ? cn : pn;
    s = mxCreateStructMatrix(1, n, 3, (const char**)f);
    for (i = 0; i < n; i++) {
        mxSetField(s, i, "name", mxCreateString(infos[i].name));
        mxSetField(s, i, "default", mxCreateLogicalScalar(infos[i].isDefault != 0));
        mxSetField(s, i, "backend", mxCreateString(ma_get_backend_name(ctx.backend)));
    }
    ma_context_uninit(&ctx);
    return s;
}

static mxArray* aum_open(int nrhs, const mxArray* prhs[]) {
    static const char* const allowed[] = { "device", "backend", "rate", "channels", "sample", "exclusive", "period", "voices",
                                           "queue", "min_tier", "onset_offset", "arena_bytes", "device_index" };
    const mxArray* s = nrhs > 1 && !mxIsEmpty(prhs[1]) ? prhs[1] : NULL;
    aum_audio* a;
    yau_desc d;
    char* dev;
    double off;
    if (s) pm_check_fields(s, allowed, 13, "open");
    memset(&d, 0, sizeof d);
    d.format.rate = (uint32_t)aum_opt(s, "rate", 48000);
    d.format.channels = (uint16_t)aum_opt(s, "channels", 2);
    d.format.sample = (uint16_t)aum_opt_enum(s, "sample", aum_samples, 3);
    d.backend = (yau_backend)aum_opt_enum(s, "backend", aum_backends, 7);
    if (d.format.rate == 0 || d.format.channels < 1 || d.format.channels > YAU_MAX_CHANNELS) pm_err("arg", "rate > 0 and channels 1 to 64");
    d.exclusive = aum_opt(s, "exclusive", 0) != 0;
    d.period = (int32_t)aum_frames_of(aum_opt(s, "period", 0), d.format.rate, "period");
    d.voices = (int32_t)aum_opt(s, "voices", 0);
    d.queue = (int32_t)aum_opt(s, "queue", 0);
    d.min_tier = (int32_t)aum_opt(s, "min_tier", 0);
    off = aum_opt(s, "onset_offset", 0);
    if (!(off >= -1.0 && off <= 1.0)) pm_err("arg", "onset_offset: seconds, within +-1");
    d.onset_offset_ns = (int64_t)llround(off * 1e9);
    d.arena_bytes = (size_t)aum_opt(s, "arena_bytes", 0);
    d.device_index = (uint32_t)aum_opt(s, "device_index", 0);
    dev = aum_opt_str(s, "device");
    a = (aum_audio*)calloc(1, sizeof(aum_audio));
    if (!a) pm_err("memory", "out of memory");
    a->kind = AUM_AUDIO;
    a->device = aum_strdup(dev);
    d.device = a->device;
    if (!yau_open(&a->au, &d)) {
        char msg[512];
        snprintf(msg, sizeof msg, "%s", yau_error(&a->au));
        free(a->device);
        free(a);
        pm_err("open", "%s", msg);
    }
    a->id = pm_next_id;
    return pm_handle_out(pm_register(a));
}

static mxArray* aum_capture_open(int nrhs, const mxArray* prhs[]) {
    static const char* const allowed[] = { "device", "backend", "rate", "channels", "sample", "exclusive", "period", "ring",
                                           "offset", "device_index" };
    const mxArray* s = nrhs > 1 && !mxIsEmpty(prhs[1]) ? prhs[1] : NULL;
    aum_capture* c;
    yau_capture_desc d;
    double off;
    if (s) pm_check_fields(s, allowed, 10, "capture_open");
    memset(&d, 0, sizeof d);
    d.format.rate = (uint32_t)aum_opt(s, "rate", 48000);
    d.format.channels = (uint16_t)aum_opt(s, "channels", 0);
    d.format.sample = (uint16_t)aum_opt_enum(s, "sample", aum_samples, 3);
    d.backend = (yau_backend)aum_opt_enum(s, "backend", aum_backends, 7);
    if (d.format.rate == 0 || d.format.channels > YAU_MAX_CHANNELS) pm_err("arg", "rate > 0 and channels 0 to 64");
    d.exclusive = aum_opt(s, "exclusive", 0) != 0;
    d.period = (int32_t)aum_frames_of(aum_opt(s, "period", 0), d.format.rate, "period");
    d.frames = aum_frames_of(aum_opt(s, "ring", 0), d.format.rate, "ring");
    off = aum_opt(s, "offset", 0);
    if (!(off >= -1.0 && off <= 1.0)) pm_err("arg", "offset: seconds, within +-1");
    d.offset_ns = (int64_t)llround(off * 1e9);
    d.device_index = (uint32_t)aum_opt(s, "device_index", 0);
    c = (aum_capture*)calloc(1, sizeof(aum_capture));
    if (!c) pm_err("memory", "out of memory");
    c->kind = AUM_CAPTURE;
    c->device = aum_strdup(aum_opt_str(s, "device"));
    d.device = c->device;
    if (!yau_capture_open(&c->c, &d)) {
        char msg[512];
        snprintf(msg, sizeof msg, "%s", yau_capture_error(&c->c));
        free(c->device);
        free(c);
        pm_err("open", "%s", msg);
    }
    return pm_handle_out(pm_register(c));
}

static aum_capture* aum_get_capture(const mxArray* x) {
    aum_capture* c = (aum_capture*)pm_lookup(x);
    if (c->kind != AUM_CAPTURE) pm_err("handle", "a capture handle (from 'capture_open') is needed here");
    return c;
}

static mxArray* aum_capture_info(aum_capture* c) {
    static const char* const f[] = { "name", "rate", "channels", "period", "buffer", "exclusive", "fit_ready", "pos_source",
                                     "tier", "drift_ppm", "fit_spread", "fit_points", "xruns", "drops", "frames", "dropped",
                                     "lost", "fill" };
    yau_capture_info i;
    mxArray* s = mxCreateStructMatrix(1, 1, 18, (const char**)f);
    double r;
    yau_capture_update(&c->c);
    yau_capture_get_info(&c->c, &i);
    r = i.rate ? (double)i.rate : 1.0;
    mxSetField(s, 0, "name", mxCreateString(i.name));
    mxSetField(s, 0, "rate", mxCreateDoubleScalar(i.rate));
    mxSetField(s, 0, "channels", mxCreateDoubleScalar(i.channels));
    mxSetField(s, 0, "period", mxCreateDoubleScalar(i.period / r));
    mxSetField(s, 0, "buffer", mxCreateDoubleScalar(i.buffer / r));
    mxSetField(s, 0, "exclusive", mxCreateLogicalScalar(i.exclusive));
    mxSetField(s, 0, "fit_ready", mxCreateLogicalScalar(i.fit_ready));
    mxSetField(s, 0, "pos_source", mxCreateString(i.pos_source == YAU_POS_DEVICE ? "device" : "callback"));
    mxSetField(s, 0, "tier", mxCreateDoubleScalar(i.tier));
    mxSetField(s, 0, "drift_ppm", mxCreateDoubleScalar(i.drift_ppm));
    mxSetField(s, 0, "fit_spread", mxCreateDoubleScalar(i.fit_spread_ns / 1e9));
    mxSetField(s, 0, "fit_points", mxCreateDoubleScalar(i.fit_points));
    mxSetField(s, 0, "xruns", mxCreateDoubleScalar(i.xruns));
    mxSetField(s, 0, "drops", mxCreateDoubleScalar(i.drops));
    mxSetField(s, 0, "frames", mxCreateDoubleScalar((double)i.frames));
    mxSetField(s, 0, "dropped", mxCreateDoubleScalar((double)i.dropped));
    mxSetField(s, 0, "lost", mxCreateDoubleScalar((double)i.lost));
    mxSetField(s, 0, "fill", mxCreateDoubleScalar((double)i.fill[0]));
    return s;
}

static mxArray* aum_time_pair(int64_t t, int nlhs, mxArray* plhs[]) {
    if (nlhs > 1) plhs[1] = t < 0 ? mxCreateDoubleMatrix(0, 0, mxREAL) : aum_i64(t);
    return t < 0 ? mxCreateDoubleMatrix(0, 0, mxREAL) : mxCreateDoubleScalar(aum_s(t));
}

/* ---- mexFunction --------------------------------------------------------------- */

YSP_SHARE_MEX_ENTRY void mexFunction(int nlhs, mxArray* plhs[], int nrhs, const mxArray* prhs[]) {
    char* cmd;
    aum_audio* a;
    pm_destroy = aum_destroy;
    /* Handles start at 0x4155 << 32, so a handle of another module (whose
     * counter starts at 1) is not taken for one of these by acquire(); still
     * exact as a double. */
    if (pm_next_id == 1) pm_next_id = ((uint64_t)0x4155 << 32) + 1;
    if (!g_api.head.abi) ysp_audio_api_fill(&g_api, YSP_SHARE_HOST_MEX, aum_api_acquire, aum_api_release);
    if (nrhs < 1 || !mxIsChar(prhs[0])) pm_err("usage", "first argument must be a command string");
    cmd = mxArrayToString(prhs[0]);

    if (strcmp(cmd, "version") == 0) {
        plhs[0] = mxCreateString(AUM_VERSION);
        if (nlhs > 1) plhs[1] = mxCreateString(yau_version());
        return;
    }
    if (strcmp(cmd, "now") == 0) {
        int64_t t = yrt_now_ns();
        plhs[0] = mxCreateDoubleScalar(aum_s(t));
        if (nlhs > 1) plhs[1] = aum_i64(t);
        return;
    }
    if (strcmp(cmd, "db") == 0) {
        pm_nargs(nrhs, 2, "g = ysp_audio('db', x)");
        plhs[0] = mxCreateDoubleScalar(yau_db((float)pm_scalar(prhs[1], "x")));
        return;
    }
    if (strcmp(cmd, "devices") == 0) { plhs[0] = aum_devices(nrhs, prhs); return; }
    if (strcmp(cmd, "open") == 0) { plhs[0] = aum_open(nrhs, prhs); return; }
    if (strcmp(cmd, "capture_open") == 0) { plhs[0] = aum_capture_open(nrhs, prhs); return; }
    if (strcmp(cmd, "wav_info") == 0) {
        static const char* const f[] = { "rate", "channels", "format", "frames", "duration", "rf64" };
        yau_wav_desc d;
        yau_wav_info info;
        char err[256] = "";
        int rc;
        pm_nargs(nrhs, 2, "info = ysp_audio('wav_info', path)");
        memset(&d, 0, sizeof d);
        d.path = aum_path(prhs[1]);
        if ((rc = yau_wav_probe(&d, &info, err, sizeof err)) < 0) pm_err(rc == YAU_ERR_IO ? "io" : "format", "%s", err[0] ? err : yau_strerror(rc));
        plhs[0] = mxCreateStructMatrix(1, 1, 6, (const char**)f);
        mxSetField(plhs[0], 0, "rate", mxCreateDoubleScalar(info.rate));
        mxSetField(plhs[0], 0, "channels", mxCreateDoubleScalar(info.channels));
        mxSetField(plhs[0], 0, "format", mxCreateDoubleScalar(info.format));
        mxSetField(plhs[0], 0, "frames", mxCreateDoubleScalar((double)info.frames));
        mxSetField(plhs[0], 0, "duration", mxCreateDoubleScalar(info.rate ? (double)info.frames / info.rate : 0.0));
        mxSetField(plhs[0], 0, "rf64", mxCreateLogicalScalar(info.rf64));
        return;
    }

    pm_nargs(nrhs, 2, "ysp_audio(command, h, ...)");

    /* ---- buffers, streams and WAV readers ---- */
    if (strcmp(cmd, "samples") == 0) {
        aum_ref* r = aum_get_ref(prhs[1], AUM_BUFFER);
        plhs[0] = aum_samples_out(r->buf.frames, r->buf.n, r->buf.channels);
        return;
    }
    if ((strncmp(cmd, "stream_", 7) == 0 || strncmp(cmd, "wav_", 4) == 0 || strcmp(cmd, "sample_at") == 0) &&
        strcmp(cmd, "stream_open") != 0 && strcmp(cmd, "wav_open") != 0) {
        int wav = cmd[0] == 'w';
        aum_ref* r = aum_get_ref(prhs[1], wav ? AUM_WAV : AUM_STREAM);
        yau_stream* st = aum_stream_of(r);
        int rc;
        if (strcmp(cmd, "stream_write") == 0) {
            int64_t n, k;
            int ch;
            float* f;
            pm_nargs(nrhs, 3, "n = ysp_audio('stream_write', s, samples)");
            if (r->kind != AUM_STREAM) pm_err("arg", "a WAV fills itself: 'wav_feed'");
            f = aum_samples_in(prhs[2], &n, &ch);
            if (ch != st->ch_) { free(f); pm_err("arg", "samples have %d channels, the stream %d", ch, (int)st->ch_); }
            k = yau_stream_write(st, f, n);
            free(f);
            if (k < 0) pm_err("arg", "%s", yau_strerror((int)k));
            plhs[0] = mxCreateDoubleScalar((double)k);
        } else if (strcmp(cmd, "stream_end") == 0) {
            if ((rc = yau_stream_end(st)) < 0) pm_err("arg", "%s", yau_strerror(rc));
        } else if (strcmp(cmd, "stream_reset") == 0) {
            if ((rc = yau_stream_reset(st, nrhs > 2 ? (int64_t)pm_scalar(prhs[2], "first") : 0)) < 0)
                pm_err("busy", "reset: the stream is playing (busy until it has ended)");
        } else if (strcmp(cmd, "stream_info") == 0) {
            plhs[0] = aum_stream_info(st);
        } else if (strcmp(cmd, "stream_close") == 0 || strcmp(cmd, "wav_close") == 0) {
            r->c->open = 0;
            if (r->c->kind == AUM_STREAM) yau_stream_close(&r->a->au, st);
            else yau_wav_close(&r->a->au, (yau_wav*)r->c->obj);
            pm_unregister(r);
            free(r);
        } else if (strcmp(cmd, "sample_at") == 0) {
            int64_t smp;
            pm_nargs(nrhs, 3, "sample = ysp_audio('sample_at', s, t)");
            rc = yau_stream_sample_at(&r->a->au, st, aum_time(prhs[2], "t"), &smp);
            if (rc < 0) aum_fail(r->a, rc);
            plhs[0] = rc == YAU_PENDING ? mxCreateDoubleMatrix(0, 0, mxREAL) : mxCreateDoubleScalar((double)smp);
        } else if (strcmp(cmd, "stream_time_of") == 0) {
            int64_t t = -1;
            pm_nargs(nrhs, 3, "[t, t_ns] = ysp_audio('stream_time_of', s, sample)");
            rc = yau_stream_time_of(&r->a->au, st, (int64_t)pm_scalar(prhs[2], "sample"), &t);
            if (rc < 0) aum_fail(r->a, rc);
            plhs[0] = aum_time_pair(rc == YAU_PENDING ? -1 : t, nlhs, plhs);
        } else if (strcmp(cmd, "wav_feed") == 0) {
            yau_wav* w = (yau_wav*)r->c->obj;
            int64_t n = yau_wav_feed(w, nrhs > 2 ? (int64_t)pm_scalar(prhs[2], "max_frames") : 0);
            if (n < 0) pm_err("io", "%s", yau_wav_error(w));
            plhs[0] = mxCreateDoubleScalar((double)n);
        } else if (strcmp(cmd, "wav_seek") == 0) {
            pm_nargs(nrhs, 3, "ysp_audio('wav_seek', w, sample)");
            if ((rc = yau_wav_seek((yau_wav*)r->c->obj, (int64_t)pm_scalar(prhs[2], "sample"))) < 0)
                pm_err(rc == YAU_ERR_BUSY ? "busy" : "arg", rc == YAU_ERR_BUSY ? "seek: the WAV is playing (stop it, then seek)" : "%s",
                       yau_wav_error((yau_wav*)r->c->obj));
        } else if (strcmp(cmd, "wav_wants") == 0) {
            plhs[0] = mxCreateLogicalScalar(yau_wav_wants((yau_wav*)r->c->obj));
        } else {
            pm_err("usage", "unknown command '%s'", cmd);
        }
        return;
    }
    if (strncmp(cmd, "capture_", 8) == 0) {
        aum_capture* c = aum_get_capture(prhs[1]);
        if (strcmp(cmd, "capture_close") == 0) {
            pm_unregister(c);
            aum_destroy(c);
        } else if (strcmp(cmd, "capture_read") == 0) {
            yau_capture_info info;
            int64_t maxf, n, first = -1;
            float* buf;
            yau_capture_update(&c->c);
            yau_capture_get_info(&c->c, &info);
            maxf = nrhs > 2 ? (int64_t)pm_scalar(prhs[2], "max_frames") : info.fill[0];
            if (maxf < 0) maxf = info.fill[0];
            buf = (float*)malloc((size_t)(maxf > 0 ? maxf : 1) * info.channels * sizeof(float));
            if (!buf) pm_err("memory", "out of memory");
            n = yau_capture_read(&c->c, 0, buf, maxf, &first);
            if (n < 0) { free(buf); pm_err("arg", "%s", yau_capture_error(&c->c)); }
            plhs[0] = aum_samples_out(buf, n, info.channels);
            free(buf);
            if (nlhs > 1) plhs[1] = mxCreateDoubleScalar((double)first);
        } else if (strcmp(cmd, "capture_time_of") == 0) {
            double f;
            pm_nargs(nrhs, 3, "[t, t_ns] = ysp_audio('capture_time_of', c, frame)");
            f = pm_scalar(prhs[2], "frame");
            plhs[0] = aum_time_pair(f == floor(f) ? yau_capture_time_of(&c->c, (int64_t)f) : yau_capture_time_at(&c->c, f), nlhs, plhs);
        } else if (strcmp(cmd, "capture_frame_at") == 0) {
            int64_t f;
            pm_nargs(nrhs, 3, "frame = ysp_audio('capture_frame_at', c, t)");
            f = yau_capture_frame_at(&c->c, aum_time(prhs[2], "t"));
            plhs[0] = f < 0 ? mxCreateDoubleMatrix(0, 0, mxREAL) : mxCreateDoubleScalar((double)f);
        } else if (strcmp(cmd, "capture_info") == 0) {
            plhs[0] = aum_capture_info(c);
        } else if (strcmp(cmd, "capture_describe") == 0) {
            char line[1024];
            yau_capture_describe(&c->c, line, sizeof line);
            plhs[0] = mxCreateString(line);
        } else {
            pm_err("usage", "unknown command '%s'", cmd);
        }
        return;
    }

    /* ---- the device ---- */
    a = aum_get_audio(prhs[1]);
    if (strcmp(cmd, "close") == 0) {
        if (a->pins > 0) pm_err("busy", "%d other module object(s) use this device (a movie's soundtrack?); close them first", a->pins);
        pm_unregister(a);
        aum_destroy(a);
    } else if (strcmp(cmd, "pins") == 0) {
        plhs[0] = mxCreateDoubleScalar(a->pins);
    } else if (strcmp(cmd, "is_open") == 0) {
        plhs[0] = mxCreateLogicalScalar(yau_is_open(&a->au));
    } else if (strcmp(cmd, "error") == 0) {
        plhs[0] = mxCreateString(yau_error(&a->au));
    } else if (strcmp(cmd, "caps") == 0) {
        plhs[0] = aum_caps(a);
    } else if (strcmp(cmd, "describe") == 0) {
        char line[1024];
        yau_describe(&a->au, line, sizeof line);
        plhs[0] = mxCreateString(line);
    } else if (strcmp(cmd, "lead") == 0) {
        int64_t l = yau_lead_ns(&a->au);
        plhs[0] = mxCreateDoubleScalar(aum_s(l));
        if (nlhs > 1) plhs[1] = aum_i64(l);
    } else if (strcmp(cmd, "frame_at") == 0) {
        int64_t f;
        pm_nargs(nrhs, 3, "frame = ysp_audio('frame_at', h, t)");
        f = yau_frame_at(&a->au, aum_time(prhs[2], "t"));
        plhs[0] = f < 0 ? mxCreateDoubleMatrix(0, 0, mxREAL) : mxCreateDoubleScalar((double)f);
    } else if (strcmp(cmd, "time_of") == 0) {
        pm_nargs(nrhs, 3, "[t, t_ns] = ysp_audio('time_of', h, frame)");
        plhs[0] = aum_time_pair(yau_time_of(&a->au, (int64_t)pm_scalar(prhs[2], "frame")), nlhs, plhs);
    } else if (strcmp(cmd, "update") == 0) {
        int n = yau_update(&a->au);
        if (n < 0) aum_fail(a, n);
        plhs[0] = mxCreateDoubleScalar(n);
    } else if (strcmp(cmd, "tone") == 0 || strcmp(cmd, "noise") == 0 || strcmp(cmd, "click") == 0) {
        static const char* const tf[] = { "hz", "duration", "peak", "ramp", "phase", "channels", "id" };
        static const char* const nf[] = { "duration", "rms", "ramp", "seed", "dist", "channels", "id" };
        static const char* const cf[] = { "peak", "duration", "id" };
        static const char* const dists[] = { "gauss", "uniform" };
        const mxArray* s;
        yau_buf b;
        pm_nargs(nrhs, 3, "b = ysp_audio('tone' | 'noise' | 'click', h, struct)");
        s = prhs[2];
        if (cmd[0] == 't') {
            yau_tone_desc d;
            pm_check_fields(s, tf, 7, "tone");
            memset(&d, 0, sizeof d);
            d.hz = aum_opt(s, "hz", 0);
            d.dur_ns = aum_s_ns(aum_opt(s, "duration", 0), "duration");
            d.peak = (float)aum_opt(s, "peak", 0);
            d.ramp_ns = aum_s_ns(aum_opt(s, "ramp", 0), "ramp");
            d.phase = aum_opt(s, "phase", 0);
            d.channels = (uint16_t)aum_opt(s, "channels", 1);
            b = yau_tone(&a->au, &d);
        } else if (cmd[0] == 'n') {
            yau_noise_desc d;
            pm_check_fields(s, nf, 7, "noise");
            memset(&d, 0, sizeof d);
            d.dur_ns = aum_s_ns(aum_opt(s, "duration", 0), "duration");
            d.rms = (float)aum_opt(s, "rms", 0);
            d.ramp_ns = aum_s_ns(aum_opt(s, "ramp", 0), "ramp");
            d.seed = pm_field(s, "seed") ? pm_u64(pm_field(s, "seed"), "seed") : 0;
            d.dist = aum_opt_enum(s, "dist", dists, 2);
            d.channels = (uint16_t)aum_opt(s, "channels", 1);
            b = yau_noise(&a->au, &d);
        } else {
            yau_click_desc d;
            pm_check_fields(s, cf, 3, "click");
            memset(&d, 0, sizeof d);
            d.peak = (float)aum_opt(s, "peak", 0);
            d.dur_ns = aum_s_ns(aum_opt(s, "duration", 0), "duration");
            b = yau_click(&a->au, &d);
        }
        plhs[0] = aum_buffer_out(a, b, (uint32_t)aum_opt(s, "id", 0));
    } else if (strcmp(cmd, "buffer") == 0) {
        int64_t n;
        int ch;
        float* f;
        yau_buf b;
        pm_nargs(nrhs, 3, "b = ysp_audio('buffer', h, samples [, id])");
        f = aum_samples_in(prhs[2], &n, &ch);
        b = yau_alloc(&a->au, n, (uint16_t)ch);
        if (b.frames) memcpy(b.frames, f, (size_t)(n * ch) * sizeof(float));
        free(f);
        plhs[0] = aum_buffer_out(a, b, nrhs > 3 ? (uint32_t)pm_scalar(prhs[3], "id") : 0);
    } else if (strcmp(cmd, "load_wav") == 0) {
        yau_wav_desc d;
        yau_buf b;
        pm_nargs(nrhs, 3, "b = ysp_audio('load_wav', h, path [, id])");
        memset(&d, 0, sizeof d);
        d.path = aum_path(prhs[2]);
        b = yau_wav_load(&a->au, &d);
        if (!b.frames) pm_err("format", "%s", yau_error(&a->au));
        plhs[0] = aum_buffer_out(a, b, nrhs > 3 ? (uint32_t)pm_scalar(prhs[3], "id") : 0);
    } else if (strcmp(cmd, "arena_reset") == 0) {
        int rc = yau_arena_reset(&a->au);
        if (rc < 0) aum_fail(a, rc);
        a->gen++;
    } else if (strcmp(cmd, "play_at") == 0) {
        aum_ref* r;
        yau_id id;
        pm_nargs(nrhs, 4, "id = ysp_audio('play_at', h, b, t)");
        r = aum_get_ref(prhs[2], AUM_BUFFER);
        if (r->a != a) pm_err("arg", "the buffer belongs to another device");
        id = yau_play_at(&a->au, r->buf, aum_time(prhs[3], "t"));
        if (id < 0) aum_fail(a, (int)id);
        plhs[0] = mxCreateDoubleScalar((double)id);
    } else if (strcmp(cmd, "play") == 0) {
        static const char* const allowed[] = { "at", "db", "channels", "offset", "frames", "loops" };
        const mxArray* o = nrhs > 3 && !mxIsEmpty(prhs[3]) ? prhs[3] : NULL;
        aum_ref* r;
        yau_play_desc d;
        yau_id id;
        pm_nargs(nrhs, 3, "id = ysp_audio('play', h, sound [, struct('at', t, 'db', g, 'channels', [1 2], ...)])");
        if (o) pm_check_fields(o, allowed, 6, "play");
        r = (aum_ref*)pm_lookup(prhs[2]);
        r = aum_get_ref(prhs[2], r->kind == AUM_BUFFER ? AUM_BUFFER : AUM_STREAM);
        if (r->a != a) pm_err("arg", "the sound belongs to another device");
        memset(&d, 0, sizeof d);
        d.at_ns = aum_s_ns(aum_opt(o, "at", 0), "at");
        d.db = (float)aum_opt(o, "db", 0);
        d.channels = aum_mask(o ? pm_field(o, "channels") : NULL);
        if (r->kind == AUM_BUFFER) {
            d.buf = r->buf;
            d.offset = (int64_t)aum_opt(o, "offset", 0);
            d.frames = (int64_t)aum_opt(o, "frames", 0);
            d.loops = (int32_t)aum_opt(o, "loops", 0);
            if (d.loops < 0) d.loops = YAU_FOREVER;
        } else {
            if (aum_opt(o, "offset", 0) || aum_opt(o, "frames", 0) || aum_opt(o, "loops", 0))
                pm_err("arg", "offset, frames and loops are for buffers; a WAV loops itself");
            d.stream = aum_stream_of(r);
        }
        id = yau_play(&a->au, &d);
        if (id < 0) aum_fail(a, (int)id);
        plhs[0] = mxCreateDoubleScalar((double)id);
    } else if (strcmp(cmd, "cancel") == 0) {
        int rc;
        pm_nargs(nrhs, 3, "ysp_audio('cancel', h, id)");
        if ((rc = yau_cancel(&a->au, (yau_id)pm_scalar(prhs[2], "id"))) < 0) aum_fail(a, rc);
    } else if (strcmp(cmd, "stop") == 0) {
        int rc = yau_stop_at(&a->au, nrhs > 2 ? (yau_id)pm_scalar(prhs[2], "id") : 0, nrhs > 3 ? aum_time(prhs[3], "at") : 0,
                             nrhs > 4 ? aum_time(prhs[4], "ramp") : 0);
        if (rc < 0) aum_fail(a, rc);
    } else if (strcmp(cmd, "gain") == 0) {
        int rc;
        pm_nargs(nrhs, 4, "ysp_audio('gain', h, id, db [, at [, ramp]])");
        rc = yau_gain_at(&a->au, (yau_id)pm_scalar(prhs[2], "id"), nrhs > 4 ? aum_time(prhs[4], "at") : 0,
                         (float)pm_scalar(prhs[3], "db"), nrhs > 5 ? aum_time(prhs[5], "ramp") : 0);
        if (rc < 0) aum_fail(a, rc);
    } else if (strcmp(cmd, "result") == 0 || strcmp(cmd, "wait") == 0) {
        yau_onset r;
        int rc;
        yau_id id;
        pm_nargs(nrhs, 3, "r = ysp_audio('result' | 'wait', h, id [, timeout])");
        id = (yau_id)pm_scalar(prhs[2], "id");
        if (cmd[0] == 'r') rc = yau_result(&a->au, id, &r);
        else {
            double to = nrhs > 3 ? pm_scalar(prhs[3], "timeout") : INFINITY;
            rc = yau_wait(&a->au, id, to == INFINITY ? INT64_MAX : aum_s_ns(to, "timeout"), &r);   /* Inf: forever */
        }
        if (rc < 0) aum_fail(a, rc);
        plhs[0] = aum_onset(&r, rc == YAU_PENDING);
    } else if (strcmp(cmd, "stream_open") == 0) {
        static const char* const allowed[] = { "channels", "ring", "preroll", "first", "id" };
        const mxArray* o = nrhs > 2 && !mxIsEmpty(prhs[2]) ? prhs[2] : NULL;
        yau_stream_desc d;
        aum_child* c;
        const mxArray* pre;
        if (o) pm_check_fields(o, allowed, 5, "stream_open");
        memset(&d, 0, sizeof d);
        d.channels = (uint16_t)aum_opt(o, "channels", 0);
        d.frames = aum_frames_of(aum_opt(o, "ring", 0), aum_rate(a), "ring");
        pre = o ? pm_field(o, "preroll") : NULL;
        if (pre) {
            if (mxIsChar(pre)) {
                if (!pm_lower_eq(pm_string(pre, "preroll"), "none")) pm_err("arg", "preroll: seconds, or 'none'");
                d.preroll = YAU_PREROLL_NONE;
            } else {
                d.preroll = aum_frames_of(pm_scalar(pre, "preroll"), aum_rate(a), "preroll");
                if (d.preroll == 0) d.preroll = YAU_PREROLL_NONE;
            }
        }
        d.first = (int64_t)aum_opt(o, "first", 0);
        d.id = (uint32_t)aum_opt(o, "id", 0);
        c = aum_child_new(a, AUM_STREAM, sizeof(yau_stream));
        if (!yau_stream_open(&a->au, (yau_stream*)c->obj, &d)) aum_fail(a, YAU_ERR_ARG);
        c->open = 1;
        { yau_buf none; memset(&none, 0, sizeof none); plhs[0] = aum_ref_out(a, AUM_STREAM, none, c); }
    } else if (strcmp(cmd, "wav_open") == 0) {
        static const char* const allowed[] = { "loops", "start", "ring", "id" };
        const mxArray* o = nrhs > 3 && !mxIsEmpty(prhs[3]) ? prhs[3] : NULL;
        yau_wav_desc d;
        aum_child* c;
        yau_wav* w;
        double loops;
        pm_nargs(nrhs, 3, "w = ysp_audio('wav_open', h, path [, struct('loops', n, 'start', s, 'ring', seconds, 'id', k)])");
        if (o) pm_check_fields(o, allowed, 4, "wav_open");
        memset(&d, 0, sizeof d);
        c = aum_child_new(a, AUM_WAV, sizeof(yau_wav));
        c->path = aum_strdup(aum_path(prhs[2]));
        d.path = c->path;
        loops = aum_opt(o, "loops", 0);
        d.loops = loops < 0 || isinf(loops) ? YAU_FOREVER : (int32_t)loops;
        d.start = (int64_t)aum_opt(o, "start", 0);
        d.ring = aum_frames_of(aum_opt(o, "ring", 0), aum_rate(a), "ring");
        d.id = (uint32_t)aum_opt(o, "id", 0);
        w = (yau_wav*)c->obj;
        if (!yau_wav_open(&a->au, w, &d)) pm_err(w->error_code == YAU_ERR_IO ? "io" : "format", "%s", yau_wav_error(w));
        c->open = 1;
        { yau_buf none; memset(&none, 0, sizeof none); plhs[0] = aum_ref_out(a, AUM_WAV, none, c); }
    } else {
        pm_err("usage", "unknown command '%s'", cmd);
    }
}
