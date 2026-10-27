/* share_consumer.c - a MEX consumer of the ysp_audio and ysp_timeline tables (tests only)
 *
 * What a ysp-vis MEX file will be, at its smallest: audio.h and timeline.h
 * as declarations, the shims of bindings/share/ forwarding every call into
 * ysp_audio.<mexext> and ysp_timeline.<mexext>, which it loads from its own
 * directory. test_mex_share.m builds it into bindings/mex/ and drives it:
 * a handle opened by ysp_audio('open') in MATLAB or Octave must be the
 * object this module reaches through the table.
 *
 *     share_consumer('import')                     % struct: versions, ABI
 *     k = share_consumer('hold_audio', h)          % pin; k is a slot (1-based)
 *     id = share_consumer('play_at', k, t)         % a click in this module's memory
 *     r = share_consumer('wait', k, id)
 *     share_consumer('drop_audio', k)
 *     (and hold_timeline, anchor, evaluate, drop_timeline, check, bench)
 */
#ifndef _WIN32
#define _GNU_SOURCE   /* dladdr() */
#endif
#define YTL_MAX_CHANNELS 1024
#include "ysp/audio.h"
#include "ysp/timeline.h"
#define YSP_SHARE_MEX
#define YSP_AUDIO_API_SHIMS
#include "ysp_audio_api.h"
#define YSP_TIMELINE_API_SHIMS
#include "ysp_timeline_api.h"

#include "mex.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#ifndef _WIN32
#include <time.h>
#endif

#define SLOTS 8
static void* g_au[SLOTS];
static void* g_tl[SLOTS];
static float g_click[480];
static int g_hooked;

static void fail(const char* id, const char* msg) { mexErrMsgIdAndTxt(id, "%s", msg); }

/* The consumer's own exit: give back every pin, then its library refs. */
static void at_exit(void) {
    int i;
    for (i = 0; i < SLOTS; i++) {
        if (g_au[i]) { ysp_audio_api_ptr->head.release(g_au[i]); g_au[i] = NULL; }
        if (g_tl[i]) { ysp_timeline_api_ptr->head.release(g_tl[i]); g_tl[i] = NULL; }
    }
    ysp_share_mex_unload(ysp_audio_api_module);
    ysp_share_mex_unload(ysp_timeline_api_module);
    ysp_audio_api_module = ysp_timeline_api_module = NULL;
    ysp_audio_api_ptr = NULL;
    ysp_timeline_api_ptr = NULL;
    g_hooked = 0;
}

static void import(void) {
    char err[512];
    if (!g_hooked) { mexAtExit(at_exit); g_hooked = 1; }
    if (ysp_audio_api_import_mex(err, sizeof err) < 0) fail("share_consumer:import", err);
    if (ysp_timeline_api_import_mex(err, sizeof err) < 0) fail("share_consumer:import", err);
}

static int slot(const mxArray* a, void** table) {
    double d = mxGetScalar(a);
    int i = (int)d - 1;
    if (d != floor(d) || i < 0 || i >= SLOTS || !table[i]) fail("share_consumer:slot", "no object held in that slot");
    return i;
}

static int64_t ns(const mxArray* a) { return (int64_t)llround(mxGetScalar(a) * 1e9); }

static mxArray* hold(void** table, const ysp_share_head* h, const mxArray* handle) {
    char err[256];
    uint64_t id;
    int i;
    if (mxIsUint64(handle)) id = *(const uint64_t*)mxGetData(handle);
    else id = (uint64_t)mxGetScalar(handle);
    for (i = 0; i < SLOTS && table[i]; i++) {}
    if (i == SLOTS) fail("share_consumer:full", "slots full");
    table[i] = h->acquire(&id, err, sizeof err);
    if (!table[i]) fail("share_consumer:refused", err);
    return mxCreateDoubleScalar(i + 1);
}

/* Not yrt_now_ns(): this module has rt.h's declarations only. */
static double now_ns(void) {
#ifdef _WIN32
    LARGE_INTEGER c, f;
    QueryPerformanceCounter(&c);
    QueryPerformanceFrequency(&f);
    return (double)c.QuadPart * 1e9 / (double)f.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e9 + ts.tv_nsec;
#endif
}

static volatile float g_sink;

void mexFunction(int nlhs, mxArray* plhs[], int nrhs, const mxArray* prhs[]) {
    char cmd[32];
    (void)nlhs;
    if (nrhs < 1 || mxGetString(prhs[0], cmd, sizeof cmd)) fail("share_consumer:usage", "a command");
    import();
    if (!strcmp(cmd, "import")) {
        const char* f[] = { "abi", "audio", "timeline", "channels" };
        plhs[0] = mxCreateStructMatrix(1, 1, 4, f);
        mxSetField(plhs[0], 0, "abi", mxCreateDoubleScalar(ysp_audio_api_ptr->head.abi));
        mxSetField(plhs[0], 0, "audio", mxCreateString(yau_version()));
        mxSetField(plhs[0], 0, "timeline", mxCreateString(ytl_version()));
        mxSetField(plhs[0], 0, "channels", mxCreateDoubleScalar(ysp_timeline_api_ptr->max_channels));
    } else if (!strcmp(cmd, "hold_audio")) {
        plhs[0] = hold(g_au, &ysp_audio_api_ptr->head, prhs[1]);
    } else if (!strcmp(cmd, "drop_audio")) {
        int i = slot(prhs[1], g_au);
        ysp_audio_api_ptr->head.release(g_au[i]);
        g_au[i] = NULL;
    } else if (!strcmp(cmd, "hold_timeline")) {
        plhs[0] = hold(g_tl, &ysp_timeline_api_ptr->head, prhs[1]);
    } else if (!strcmp(cmd, "drop_timeline")) {
        int i = slot(prhs[1], g_tl);
        ysp_timeline_api_ptr->head.release(g_tl[i]);
        g_tl[i] = NULL;
    } else if (!strcmp(cmd, "info")) {
        yau_audio* au = (yau_audio*)g_au[slot(prhs[1], g_au)];
        yau_caps c;
        char line[512];
        const char* f[] = { "open", "rate", "lead", "describe" };
        yau_get_caps(au, &c);
        yau_describe(au, line, sizeof line);
        plhs[0] = mxCreateStructMatrix(1, 1, 4, f);
        mxSetField(plhs[0], 0, "open", mxCreateLogicalScalar(yau_is_open(au)));
        mxSetField(plhs[0], 0, "rate", mxCreateDoubleScalar(c.rate));
        mxSetField(plhs[0], 0, "lead", mxCreateDoubleScalar((double)yau_lead_ns(au) / 1e9));
        mxSetField(plhs[0], 0, "describe", mxCreateString(line));
    } else if (!strcmp(cmd, "play_at")) {
        yau_audio* au = (yau_audio*)g_au[slot(prhs[1], g_au)];
        yau_buf b;
        yau_id id;
        int k;
        for (k = 0; k < 480; k++) g_click[k] = k < 48 ? 0.01f : 0.0f;
        memset(&b, 0, sizeof b);
        b.frames = g_click;
        b.n = 480;
        b.channels = 1;
        b.id = 77;
        id = yau_play_at(au, b, ns(prhs[2]));
        if (id < 0) fail("share_consumer:play", yau_error(au));
        plhs[0] = mxCreateDoubleScalar((double)id);
    } else if (!strcmp(cmd, "wait")) {
        yau_audio* au = (yau_audio*)g_au[slot(prhs[1], g_au)];
        yau_onset r;
        const char* f[] = { "rc", "id", "onset", "target", "tier", "buffer_id" };
        int rc;
        memset(&r, 0, sizeof r);
        rc = yau_wait(au, (yau_id)mxGetScalar(prhs[2]), 2000000000LL, &r);
        plhs[0] = mxCreateStructMatrix(1, 1, 6, f);
        mxSetField(plhs[0], 0, "rc", mxCreateDoubleScalar(rc));
        mxSetField(plhs[0], 0, "id", mxCreateDoubleScalar((double)r.id));
        mxSetField(plhs[0], 0, "onset", mxCreateDoubleScalar((double)r.onset / 1e9));
        mxSetField(plhs[0], 0, "target", mxCreateDoubleScalar((double)r.target / 1e9));
        mxSetField(plhs[0], 0, "tier", mxCreateDoubleScalar(r.tier));
        mxSetField(plhs[0], 0, "buffer_id", mxCreateDoubleScalar(r.buffer_id));
    } else if (!strcmp(cmd, "anchor")) {
        ytl_timeline* tl = (ytl_timeline*)g_tl[slot(prhs[1], g_tl)];
        plhs[0] = mxCreateDoubleScalar(ytl_anchor(tl, (int)mxGetScalar(prhs[2]), ns(prhs[3]), ns(prhs[4])));
    } else if (!strcmp(cmd, "evaluate")) {
        ytl_timeline* tl = (ytl_timeline*)g_tl[slot(prhs[1], g_tl)];
        ytl_frame f;
        ytl_event fired[16];
        int n, k, nch = (int)mxGetScalar(prhs[5]);
        const float* v;
        f.onset = ns(prhs[2]);
        f.period = ns(prhs[3]);
        f.index = (int64_t)mxGetScalar(prhs[4]);
        n = ytl_evaluate(tl, &f, fired, 16);
        v = ytl_values(tl);
        plhs[0] = mxCreateDoubleScalar(n);
        if (nlhs > 1) {
            plhs[1] = mxCreateDoubleMatrix(1, (size_t)nch, mxREAL);
            for (k = 0; k < nch; k++) mxGetPr(plhs[1])[k] = v ? v[k] : 0.0;
        }
    } else if (!strcmp(cmd, "check")) {
        char what[32], err[256] = "";
        int rc;
        mxGetString(prhs[1], what, sizeof what);
        if (what[0] == 'a') {
            ysp_audio_api a = *ysp_audio_api_ptr;
            static char ver[32];
            /* An owner of another version: the header's own numbers, one moved. */
            int ma = YAU_VERSION_MAJOR, mi = YAU_VERSION_MINOR, pa = YAU_VERSION_PATCH;
            if (!strcmp(what, "a_abi")) a.head.abi += 1;
            else if (!strcmp(what, "a_version")) { strcpy(ver, "0.0.1"); a.head.version = ver; }
            else if (!strcmp(what, "a_patch")) { snprintf(ver, sizeof ver, "%d.%d.%d", ma, mi, pa + 1); a.head.version = ver; }
            else if (!strcmp(what, "a_minor")) { snprintf(ver, sizeof ver, "%d.%d.%d", ma, mi + 1, pa); a.head.version = ver; }
            else if (!strcmp(what, "a_sizeof")) a.sizeof_yau_audio += 8;
            else if (!strcmp(what, "a_host")) a.head.host = YSP_SHARE_HOST_PYTHON;
            rc = ysp_audio_api_check(&a, YSP_SHARE_HOST_MEX, err, sizeof err);
        } else {
            ysp_timeline_api a = *ysp_timeline_api_ptr;
            static char ver[32];
            int ma = YTL_VERSION_MAJOR, mi = YTL_VERSION_MINOR, pa = YTL_VERSION_PATCH;
            if (!strcmp(what, "t_channels")) a.max_channels = 256;
            else if (!strcmp(what, "t_patch")) { snprintf(ver, sizeof ver, "%d.%d.%d", ma, mi, pa + 7); a.head.version = ver; }
            else if (!strcmp(what, "t_minor")) { snprintf(ver, sizeof ver, "%d.%d.%d", ma, mi + 1, pa); a.head.version = ver; }
            rc = ysp_timeline_api_check(&a, YSP_SHARE_HOST_MEX, err, sizeof err);
        }
        plhs[0] = mxCreateDoubleScalar(rc);
        if (nlhs > 1) plhs[1] = mxCreateString(err);
    } else if (!strcmp(cmd, "bench")) {
        /* ns per ytl_value(): through the shim, through the table, inline. */
        ytl_timeline* tl = (ytl_timeline*)g_tl[slot(prhs[1], g_tl)];
        long long n = (long long)mxGetScalar(prhs[2]), k;
        int reps = (int)mxGetScalar(prhs[3]), r;
        double best[3] = { 1e300, 1e300, 1e300 }, t0, dt;
        float (*fn)(const ytl_timeline*, int) = ysp_timeline_api_ptr->ytl_value;
        const char* f[] = { "shim", "table", "inline" };
        for (r = 0; r < reps; r++) {
            float acc = 0.0f;
            t0 = now_ns();
            for (k = 0; k < n; k++) acc += ytl_value(tl, (int)(k & 1));
            g_sink = acc;
            dt = (now_ns() - t0) / (double)n; if (dt < best[0]) best[0] = dt;
            acc = 0.0f;
            t0 = now_ns();
            for (k = 0; k < n; k++) acc += fn(tl, (int)(k & 1));
            g_sink = acc;
            dt = (now_ns() - t0) / (double)n; if (dt < best[1]) best[1] = dt;
            acc = 0.0f;
            t0 = now_ns();
            for (k = 0; k < n; k++) acc += ((volatile float*)tl->values)[k & 1];
            g_sink = acc;
            dt = (now_ns() - t0) / (double)n; if (dt < best[2]) best[2] = dt;
        }
        plhs[0] = mxCreateStructMatrix(1, 1, 3, f);
        for (r = 0; r < 3; r++) mxSetField(plhs[0], 0, f[r], mxCreateDoubleScalar(best[r]));
    } else {
        fail("share_consumer:usage", "unknown command");
    }
}
