/* ysp_vis_mex.h - the link between the ysp-vis core (ysp_vis.c) and its thin MEX files
 *
 * Not a library header. ysp-vis in MATLAB and Octave is one core library and
 * three thin MEX files (docs/bindings.md, ysp-vis):
 *
 *   ysp_vis.<mexext>      the core: ysp/screen.h, ysp/gfx.h, ysp/video.h, a
 *                         static SDL3, the handle table and every command.
 *                         It is a MEX file too, so it sits beside the others
 *                         with the same extension and finds ysp_audio and
 *                         ysp_timeline as share_consumer.c does; called
 *                         itself, it answers 'version' and 'info'.
 *   ysp_screen, ysp_gfx,  each about 1 kB of code: it loads the core from its
 *   ysp_video.<mexext>    own directory (ysp_share_mex_load, which returns the
 *                         module already loaded), attaches, and hands every
 *                         call to the core's dispatch. So a handle from
 *                         ysp_screen is a handle in ysp_gfx: one table.
 *
 * LIFETIME. Each thin MEX attaches once when it loads the core and detaches
 * in its mexAtExit hook (clear ysp_screen, clear mex, the exit), then frees
 * its reference to the core. The core closes every gfx and screen, and
 * gives back the audio and timeline modules, when the last thin MEX
 * detaches; until then, a clear of one MEX leaves the others' objects open.
 * The core's code stays mapped while any thin MEX holds it.
 *
 * Public domain / MIT-0, as the headers.
 */
#ifndef YSP_VIS_MEX_H
#define YSP_VIS_MEX_H

#include "mex.h"
#include <stdint.h>

/* The binding's own version (docs/bindings.md, Versions): the thin files and
 * the core must be the same build. */
#define YV_MEX_VERSION "1.2.0"   /* 1.1.0: ysp_gfx stimuli, lists, groups, textures, bindings; the slim frame; ysp_video movies;
                                  * 1.2.0: ysp_gfx text (fonts, texts laid out by Skribidi, glyph and word boxes) */
#define YV_CORE_ABI 1
#define YV_CORE_SYMBOL "ysp_vis_core"

enum { YV_WHICH_VIS = 0, YV_WHICH_SCREEN = 1, YV_WHICH_GFX = 2, YV_WHICH_VIDEO = 3 };

typedef struct yv_core {
    uint32_t    abi;
    uint32_t    size;
    const char* version;     /* YV_MEX_VERSION of the core                  */
    void (*attach)(void);    /* one per thin MEX that loaded the core       */
    void (*detach)(void);    /* the last one closes everything              */
    void (*dispatch)(int which, int nlhs, mxArray* plhs[], int nrhs, const mxArray* prhs[]);
} yv_core;

#ifdef YV_THIN
/* ---- a thin MEX file: define YV_THIN to YV_WHICH_SCREEN, _GFX or _VIDEO and
 * YV_THIN_NAME to its name, then include this file. */
#define YSP_SHARE_MEX   /* ysp_share_mex_load(): Windows only, as ysp-vis */
#include "ysp_share.h"

static const yv_core* yv_thin_core = NULL;
static void* yv_thin_module = NULL;

static void yv_thin_exit(void) {
    if (yv_thin_core) yv_thin_core->detach();
    yv_thin_core = NULL;
    /* Octave runs this hook on clear and keeps the file loaded: the next
     * call loads, attaches and hooks again. */
    ysp_share_mex_unload(yv_thin_module);
    yv_thin_module = NULL;
}

#if defined(__MINGW32__)
__declspec(dllexport)
#endif
void mexFunction(int nlhs, mxArray* plhs[], int nrhs, const mxArray* prhs[]) {
    if (!yv_thin_core) {
        char err[512];
        void* m = NULL;
        const yv_core* c = (const yv_core*)ysp_share_mex_load("ysp_vis", YV_CORE_SYMBOL, &m, err, sizeof err);
        if (!c) mexErrMsgIdAndTxt(YV_THIN_NAME ":load", "%s", err);
        if (c->abi != YV_CORE_ABI || c->size < sizeof(yv_core) || strcmp(c->version, YV_MEX_VERSION) != 0) {
            ysp_share_mex_unload(m);
            mexErrMsgIdAndTxt(YV_THIN_NAME ":load", "ysp_vis is version %s (core ABI %u), this file is %s: build them together (build.m)",
                              c->version, (unsigned)c->abi, YV_MEX_VERSION);
        }
        yv_thin_core = c;
        yv_thin_module = m;
        c->attach();
        mexAtExit(yv_thin_exit);
    }
    yv_thin_core->dispatch(YV_THIN, nlhs, plhs, nrhs, prhs);
}
#endif /* YV_THIN */

#endif /* YSP_VIS_MEX_H */
