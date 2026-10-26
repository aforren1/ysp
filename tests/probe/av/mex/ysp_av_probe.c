/* ysp_av_probe.c - the ysp-av probe as a MEX file for MATLAB and Octave.
 * Not the ysp-av API (docs/av_probe.md). Command dispatch:
 *
 *   desc = ysp_av_probe('open', windowed, angle_dir, panic)   angle_dir '' = the MEX's folder
 *   ysp_av_probe('setup', n)
 *   f = ysp_av_probe('begin')     [index onset period] in s; [] after an abort
 *   r = ysp_av_probe('records')   n x 8 of the last begin(): index target onset residual dropped path tier flags
 *   ysp_av_probe('evaluate'); ysp_av_probe('gfx_begin'); ysp_av_probe('set', i, p, v)
 *   ysp_av_probe('draw', i); ysp_av_probe('gfx_end'); ysp_av_probe('flip_at', t)
 *   ysp_av_probe('frame_c')       one whole frame in C
 *   ysp_av_probe('close'); n = ysp_av_probe('dump', path, tag); c = ysp_av_probe('counts')
 *   ysp_av_probe('free')
 *   s = ysp_av_probe('modules'); ok = ysp_av_probe('preload', path); s = ysp_av_probe('displays')
 *   ysp_av_probe('watch_start'); s = ysp_av_probe('watch_stop')
 *   ysp_av_probe('keys', presses, delay_ms, gap_ms); k = ysp_av_probe('keys_status')
 *   s = ysp_av_probe('audio_open', null); t = ysp_av_probe('now'); ysp_av_probe('elevate')
 *   a = ysp_av_probe('abort_info'); P = ysp_av_probe('params')   [ori phase contrast]
 *
 * Indices are 0-based here, as in the core: this is a probe.
 */
#include "mex.h"
#include <math.h>
#include "../probe_core.h"

#define S(ns) ((double)(ns) * 1e-9)

static int cleanup_hooked = 0;
static void at_exit(void) {
    pv_close();
    pv_free();
}

static void fail(const char* id, const char* msg) { mexErrMsgIdAndTxt(id, "%s", msg); }

static double num(int nrhs, const mxArray* prhs[], int k, double def) {
    if (k >= nrhs) return def;
    if (!mxIsNumeric(prhs[k]) && !mxIsLogical(prhs[k])) fail("ysp_av_probe:arg", "numeric argument expected");
    return mxGetScalar(prhs[k]);
}

static void str(int nrhs, const mxArray* prhs[], int k, char* out, size_t cap) {
    out[0] = 0;
    if (k >= nrhs) return;
    if (!mxIsChar(prhs[k]) || mxGetString(prhs[k], out, (mwSize)cap)) fail("ysp_av_probe:arg", "string argument expected");
}

void mexFunction(int nlhs, mxArray* plhs[], int nrhs, const mxArray* prhs[]) {
    char cmd[32];
    static char buf[65536];
    (void)nlhs;
    if (!cleanup_hooked) { mexAtExit(at_exit); cleanup_hooked = 1; }
    if (nrhs < 1 || !mxIsChar(prhs[0])) fail("ysp_av_probe:arg", "first argument: a command");
    mxGetString(prhs[0], cmd, sizeof cmd);

    if (!strcmp(cmd, "begin")) {
        int rc;
        if (!pv || !pv->open) fail("ysp_av_probe:closed", "not open");
        rc = pv_begin();
        if (rc == YSCR_QUIT) { plhs[0] = mxCreateDoubleMatrix(0, 0, mxREAL); return; }
        if (rc != YSCR_OK) fail("ysp_av_probe:screen", yscr_error(&pv->scr));
        plhs[0] = mxCreateDoubleMatrix(1, 3, mxREAL);
        {
            double* p = mxGetPr(plhs[0]);
            p[0] = (double)pv->f.index; p[1] = S(pv->f.onset); p[2] = S(pv->f.period);
        }
    } else if (!strcmp(cmd, "records")) {
        int i, n = pv ? pv->f.n_done : 0;
        double* p;
        plhs[0] = mxCreateDoubleMatrix(n, 8, mxREAL);
        p = mxGetPr(plhs[0]);
        for (i = 0; i < n; i++) {
            const yscr_record* r = &pv->f.done[i];
            p[i] = (double)r->index; p[i + n] = S(r->target); p[i + 2 * n] = S(r->onset);
            p[i + 3 * n] = S(r->residual); p[i + 4 * n] = r->dropped; p[i + 5 * n] = r->path;
            p[i + 6 * n] = r->tier; p[i + 7 * n] = r->flags;
        }
    } else if (!strcmp(cmd, "evaluate")) {
        pv_evaluate();
    } else if (!strcmp(cmd, "gfx_begin")) {
        pv_gfx_begin();
    } else if (!strcmp(cmd, "set")) {
        pv_set((int)num(nrhs, prhs, 1, -1), (int)num(nrhs, prhs, 2, 0), (float)num(nrhs, prhs, 3, 0));
    } else if (!strcmp(cmd, "draw")) {
        if (pv_draw((int)num(nrhs, prhs, 1, -1)) != YGFX_OK) fail("ysp_av_probe:gfx", ygfx_error(&pv->gfx));
    } else if (!strcmp(cmd, "gfx_end")) {
        pv_gfx_end();
    } else if (!strcmp(cmd, "flip_at")) {
        double t = num(nrhs, prhs, 1, -1);
        if (!(t >= 0)) fail("ysp_av_probe:arg", "flip_at: t must be >= 0");
        if (pv_flip_at((int64_t)llround(t * 1e9)) < 0) fail("ysp_av_probe:screen", yscr_error(&pv->scr));
    } else if (!strcmp(cmd, "frame_c")) {
        int rc = pv_frame_c(1);
        if (rc < 0) fail("ysp_av_probe:screen", yscr_error(&pv->scr));
        plhs[0] = mxCreateLogicalScalar(rc == YSCR_OK);
    } else if (!strcmp(cmd, "open")) {
        pv_opts o;
        char angle[1024];
        memset(&o, 0, sizeof o);
        o.w = 1280; o.h = 720; o.foreground = YSCR_FOREGROUND_FORCE;
        o.windowed = num(nrhs, prhs, 1, 0) != 0;
        str(nrhs, prhs, 2, angle, sizeof angle);
        o.angle_dir = angle;
        o.panic = num(nrhs, prhs, 3, 0) != 0;
        if (!pv_open(&o)) fail("ysp_av_probe:open", pv_last_error());
        pv_describe(buf, sizeof buf);
        plhs[0] = mxCreateString(buf);
    } else if (!strcmp(cmd, "setup")) {
        if (!pv) fail("ysp_av_probe:closed", "not open");
        if (pv_setup((int)num(nrhs, prhs, 1, 1)) < 0) fail("ysp_av_probe:gfx", ygfx_error(&pv->gfx));
    } else if (!strcmp(cmd, "close")) {
        pv_close();
    } else if (!strcmp(cmd, "free")) {
        pv_close();
        pv_free();
    } else if (!strcmp(cmd, "dump")) {
        char path[1024], tag[256];
        str(nrhs, prhs, 1, path, sizeof path);
        str(nrhs, prhs, 2, tag, sizeof tag);
        if (!pv) fail("ysp_av_probe:closed", "not open");
        plhs[0] = mxCreateDoubleScalar(pv_dump(path, tag));
    } else if (!strcmp(cmd, "counts")) {
        double* p;
        plhs[0] = mxCreateDoubleMatrix(1, 3, mxREAL);
        p = mxGetPr(plhs[0]);
        if (pv) { p[0] = (double)pv->n_frames; p[1] = (double)pv->n_recs; p[2] = (double)pv->lost; }
    } else if (!strcmp(cmd, "describe")) {
        if (!pv || !pv->open) fail("ysp_av_probe:closed", "not open");
        pv_describe(buf, sizeof buf);
        plhs[0] = mxCreateString(buf);
    } else if (!strcmp(cmd, "modules")) {
        pv_modules(buf, sizeof buf, num(nrhs, prhs, 1, 0) != 0);
        plhs[0] = mxCreateString(buf);
    } else if (!strcmp(cmd, "preload")) {
        char path[1024];
        str(nrhs, prhs, 1, path, sizeof path);
        plhs[0] = mxCreateLogicalScalar(pv_preload(path) != NULL);
    } else if (!strcmp(cmd, "displays")) {
        pv_displays(buf, sizeof buf);
        plhs[0] = mxCreateString(buf);
    } else if (!strcmp(cmd, "watch_start")) {
        if (!pv || !pv->open) fail("ysp_av_probe:closed", "not open");
        pv_watch_start();
    } else if (!strcmp(cmd, "watch_stop")) {
        if (!pv) fail("ysp_av_probe:closed", "not open");
        pv_watch_stop(buf, sizeof buf);
        plhs[0] = mxCreateString(buf);
    } else if (!strcmp(cmd, "keys")) {
        if (!pv || !pv->open) fail("ysp_av_probe:closed", "not open");
        pv_keys((int)num(nrhs, prhs, 1, 1), (int)num(nrhs, prhs, 2, 500), (int)num(nrhs, prhs, 3, 300));
    } else if (!strcmp(cmd, "keys_status")) {
        double* p;
        plhs[0] = mxCreateDoubleMatrix(1, 2, mxREAL);
        p = mxGetPr(plhs[0]);
        if (pv) { p[0] = pv->keys_sent; p[1] = pv->keys_skipped; }
    } else if (!strcmp(cmd, "abort_info")) {
        double* p;
        plhs[0] = mxCreateDoubleMatrix(1, 2, mxREAL);
        p = mxGetPr(plhs[0]);
        if (pv) { p[0] = pv->f.abort; p[1] = pv->f.abort_presses; }
    } else if (!strcmp(cmd, "audio_open")) {
        if (!pv || !pv->open) fail("ysp_av_probe:closed", "not open");
        if (pv_audio_open(num(nrhs, prhs, 1, 1) != 0, buf, sizeof buf) < 0) fail("ysp_av_probe:audio", buf);
        plhs[0] = mxCreateString(buf);
    } else if (!strcmp(cmd, "noghost")) {
        pv_noghost();
    } else if (!strcmp(cmd, "now")) {
        plhs[0] = mxCreateDoubleScalar(S(yrt_now_ns()));
    } else if (!strcmp(cmd, "elevate")) {
        plhs[0] = mxCreateDoubleScalar((double)yrt_thread_elevate(NULL));
    } else if (!strcmp(cmd, "params")) {
        double* p;
        plhs[0] = mxCreateDoubleMatrix(1, 3, mxREAL);
        p = mxGetPr(plhs[0]);
        p[0] = YGFX_P_ORI; p[1] = YGFX_P_PHASE; p[2] = YGFX_P_CONTRAST;
    } else {
        fail("ysp_av_probe:arg", "unknown command");
    }
}
