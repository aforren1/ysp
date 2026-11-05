/* gapdiag.c - the grid of ysp/screen.h across an idle gap (docs/screen.md,
 * "The grid after a gap"; README.md here). Windows only.
 *
 *   gapdiag [--dxgi] [--windowed] [--pre S] [--gap S] [--post N] [--wt on|off]
 *           [--csv PATH] [--wake] [--dwm]
 *
 * Opens a screen, runs frames for S seconds (GL clears), sleeps the gap
 * with no call, runs N frames. The presenter's completions and acquire are
 * wrapped, so every OS vblank time is logged with the grid's state before
 * the core used it: the nearest grid vblank and the offset of the OS time
 * from it. --wake also logs the idle-wake waiter's vblank wakes (COMPOSITION):
 * a thread of this program waits on the same D3DKMT event. --dwm logs
 * DWM's vblank time (DwmGetCompositionTimingInfo) every 20 ms.
 */
#if defined(_MSC_VER)
#define _CRT_SECURE_NO_WARNINGS
#endif
#define YSP_SCREEN_IMPLEMENTATION
#include "ysp/screen.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <windows.h>
#include <dwmapi.h>

typedef void (__stdcall *cc_fn)(float, float, float, float);
typedef void (__stdcall *cl_fn)(unsigned int);

static yscr_screen scr;
static yscr_presenter wrapped;
static const yscr_presenter* orig;

#define MAXL 400000
typedef struct {
    int64_t t_call;       /* when the core read it */
    int64_t t;            /* the OS time */
    int64_t count;        /* the OS count */
    int64_t nc;           /* the grid's nearest vblank to t */
    int64_t grid_t;       /* its time */
    double  period;       /* the grid's period */
    int64_t ref_span;     /* vblanks the period was fitted over */
    int32_t src;          /* 0 completion, 1 acquire's newest, 2 a waiter wake */
    int32_t path, flags;
    int64_t frame;        /* frames begun so far */
} ent;
static ent* L;
static volatile LONG nL;
static int64_t g_frames;

static void log_one(int src, const yscr_vblank* v, int64_t t) {
    LONG k = InterlockedIncrement(&nL) - 1;
    ent* e;
    if (k >= MAXL) return;
    e = &L[k];
    e->t_call = yrt_now_ns();
    e->t = t;
    e->count = v ? v->count : 0;
    e->src = src;
    e->path = v ? v->path : 0;
    e->flags = v ? v->flags : 0;
    e->frame = g_frames;
    if (scr.have_anchor && t) {
        e->nc = scr.vb_count + (int64_t)llround((double)(t - scr.vb_t) / scr.period_f);
        e->grid_t = scr.vb_t + (int64_t)llround((double)(e->nc - scr.vb_count) * scr.period_f);
        e->period = scr.period_f;
        e->ref_span = scr.vb_count - scr.ref_count;
    }
}

static int w_completions(void* ctx, yscr_vblank* out, int cap) {
    int n = orig->completions(ctx, out, cap), i;
    for (i = 0; i < n; i++) if (out[i].t_ns) log_one(0, &out[i], out[i].t_ns);
    return n;
}
static int w_acquire(void* ctx, int64_t deadline, yscr_vblank* newest) {
    int rc = orig->acquire(ctx, deadline, newest);
    if (rc >= 0 && newest && newest->t_ns) log_one(1, newest, newest->t_ns);
    return rc;
}

/* DWM's last vblank (DwmGetCompositionTimingInfo), sampled every 20 ms from a
 * thread of this program: src 3, t = qpcVBlank on the ysp_rt clock, count =
 * cRefresh, path = the refresh period in us. */
static volatile LONG g_dwm_stop;
static int g_dwm;
static void dwm_sample(int src) {
    DWM_TIMING_INFO ti;
    yscr_vblank v;
    memset(&ti, 0, sizeof ti);
    ti.cbSize = sizeof ti;
    if (DwmGetCompositionTimingInfo(NULL, &ti) != S_OK) return;
    memset(&v, 0, sizeof v);
    v.count = (int64_t)ti.cRefresh;
    v.path = (uint8_t)0;
    log_one(src, &v, yrt_ticks_to_ns((int64_t)ti.qpcVBlank));
}
static DWORD WINAPI dwm_main(void* p) {
    (void)p;
    while (!g_dwm_stop) { dwm_sample(3); Sleep(20); }
    return 0;
}

/* the waiter's wakes: the same D3DKMT event as the header's idle-wake waiter */
static volatile LONG g_wake_stop;
static DWORD WINAPI wake_main(void* p) {
    yscr__wake_t* k = (yscr__wake_t*)p;
    while (!g_wake_stop) {
        if (k->wait(&k->w) == 0) log_one(2, NULL, yrt_now_ns());
        else Sleep(4);
    }
    return 0;
}

int main(int argc, char** argv) {
    yscr_desc d;
    yscr_frame f;
    int i, wt = 0, post = 120, wake = 0;
    double pre = 2, gap = 10;
    const char* csv = NULL;
    char line[1200];
    cc_fn cc; cl_fn cl;
    HANDLE wth = NULL;
    memset(&d, 0, sizeof d);
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--dxgi")) d.backend = YSCR_BACKEND_DXGI_FLIP;
        else if (!strcmp(argv[i], "--windowed")) { d.windowed = true; d.window_w = 1280; d.window_h = 720; }
        else if (!strcmp(argv[i], "--pre") && i + 1 < argc) pre = atof(argv[++i]);
        else if (!strcmp(argv[i], "--gap") && i + 1 < argc) gap = atof(argv[++i]);
        else if (!strcmp(argv[i], "--post") && i + 1 < argc) post = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--wt") && i + 1 < argc) { i++; wt = !strcmp(argv[i], "on") ? 1 : !strcmp(argv[i], "off") ? 2 : 0; }
        else if (!strcmp(argv[i], "--csv") && i + 1 < argc) csv = argv[++i];
        else if (!strcmp(argv[i], "--wake")) wake = 1;
        else if (!strcmp(argv[i], "--dwm")) g_dwm = 1;
        else { fprintf(stderr, "bad argument %s\n", argv[i]); return 2; }
    }
    L = (ent*)calloc(MAXL, sizeof *L);
    d.window_thread = wt;
    d.foreground = YSCR_FOREGROUND_FORCE;
    yrt_thread_elevate(NULL);
    if (!yscr_open(&scr, &d)) { fprintf(stderr, "open: %s\n", yscr_error(&scr)); return 1; }
    orig = scr.pr;
    wrapped = *orig;
    wrapped.completions = w_completions;
    wrapped.acquire = w_acquire;
    scr.pr = &wrapped;
    cc = (cc_fn)yscr_gl_proc(&scr, "glClearColor");
    cl = (cl_fn)yscr_gl_proc(&scr, "glClear");
    yscr_describe(&scr, line, sizeof line);
    printf("%s\n", line);
    if (wake && scr.wake) {
        /* a second waiter on the same adapter and source */
        yscr__wake_t* k = (yscr__wake_t*)calloc(1, sizeof *k);
        *k = *(yscr__wake_t*)scr.wake;
        wth = CreateThread(NULL, 0, wake_main, k, 0, NULL);
    }
    {
        HANDLE dth = g_dwm ? CreateThread(NULL, 0, dwm_main, NULL, 0, NULL) : NULL;
        (void)dth;
    }
    {
        int64_t until = yrt_now_ns() + (int64_t)(pre * 1e9);
        while (yrt_now_ns() < until) {
            if (yscr_begin(&scr, &f) != YSCR_OK) break;
            g_frames++;
            cc(0.1f, 0.1f, 0.1f, 1); cl(0x4000);
            yscr_flip(&scr);
        }
    }
    printf("before the gap: frames %lld, period %.4f ns (%+.2f ppm against the mode), fitted over %lld vblanks, "
           "grid_follow %d\n", (long long)g_frames, scr.period_f, (scr.period_f / scr.nominal_f - 1) * 1e6,
           (long long)(scr.vb_count - scr.ref_count), scr.grid_follow);
    fflush(stdout);
    Sleep((DWORD)(gap * 1000));
    {
        int unstable = 0, k, first_unst = -1, last_unst = -1;
        double max_err = 0;
        for (k = 0; k < post; k++) {
            int j;
            if (yscr_begin(&scr, &f) != YSCR_OK) break;
            g_frames++;
            for (j = 0; j < f.n_done; j++) {
                const yscr_record* r = &f.done[j];
                if (r->index < g_frames - post - 1) continue;
                if (r->flags & YSCR_FLIP_GRID_UNSTABLE) {
                    unstable++;
                    if (first_unst < 0) first_unst = (int)r->index;
                    last_unst = (int)r->index;
                }
            }
            cc(0.1f, 0.1f, 0.1f, 1); cl(0x4000);
            yscr_flip(&scr);
        }
        yscr_wait_flip(&scr, NULL);
        (void)max_err;
        printf("after the gap: GRID_UNSTABLE records %d (frames %d to %d)\n", unstable, first_unst, last_unst);
    }
    g_wake_stop = 1;
    g_dwm_stop = 1;
    if (wth) { WaitForSingleObject(wth, 1000); CloseHandle(wth); }
    yscr_describe(&scr, line, sizeof line);
    printf("%s\n", line);
    scr.pr = orig;
    yscr_close(&scr);
    if (csv) {
        FILE* fp = fopen(csv, "w");
        LONG k, n = nL < MAXL ? nL : MAXL;
        fprintf(fp, "t_call,t,count,nc,grid_t,off_ns,period,ref_span,src,path,flags,frame\n");
        for (k = 0; k < n; k++) {
            const ent* e = &L[k];
            fprintf(fp, "%lld,%lld,%lld,%lld,%lld,%lld,%.6f,%lld,%d,%d,%d,%lld\n", (long long)e->t_call, (long long)e->t,
                    (long long)e->count, (long long)e->nc, (long long)e->grid_t, (long long)(e->grid_t ? e->t - e->grid_t : 0),
                    e->period, (long long)e->ref_span, e->src, e->path, e->flags, (long long)e->frame);
        }
        fclose(fp);
        printf("logged %ld OS times to %s\n", (long)n, csv);
    }
    return 0;
}
