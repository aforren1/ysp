/* probe_core.h - the shared core of the ysp-av probe (docs/av_probe.md),
 * with the window-thread additions of docs/winthread_probe.md: the mode
 * comes from YSCR_WINDOW_THREAD in the environment (read by the probe copy
 * of ysp/screen.h in tests/probe/winthread/include), pv_wt_state() and
 * pv_latency().
 *
 * Not the ysp-av API. One screen, one gfx, one timeline and an optional
 * audio device per process, driven one call at a time by a host (Python,
 * MATLAB, Octave) or by probe_c.c, so that the host's own cost per frame
 * is the only difference between the hosts. Every time the core keeps is
 * int64 ns on the ysp/rt.h clock; the frontends convert to seconds.
 *
 * Include it in one C file. Windows only: the probe measures the real
 * Windows backend, and the module/window checks are Win32.
 */
#ifndef YSP_AV_PROBE_CORE_H
#define YSP_AV_PROBE_CORE_H

#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif

#ifndef PV_NO_AUDIO
#define YSP_AUDIO_IMPLEMENTATION
#include "ysp/audio.h"
#endif
#define YSP_GFX_IMPLEMENTATION
#include "ysp/gfx.h"
#define YSP_TIMELINE_IMPLEMENTATION
#include "ysp/timeline.h"
/* video.h is compiled in to check that the five headers build as one
 * module; the probe plays no movie. */
#define YVID_NO_PL_MPEG
#define YSP_VIDEO_IMPLEMENTATION
#include "ysp/video.h"

#include <SDL3/SDL.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PV_MAX_STIMS  512
#define PV_MAX_FRAMES 90000      /* 25 min at 60 Hz */
#define PV_DOTS       100
#define PV_WATCH_MAX  256

typedef struct pv_frame {
    int64_t begin_call, begin_ret, flip_call, flip_ret;
    int64_t onset;               /* begin()'s prediction */
} pv_frame;

typedef struct pv_watch_ev {
    int64_t  t;                  /* ns from watch start */
    int32_t  hung, fg_ours, fg_ghost, iconic, covers;
} pv_watch_ev;

typedef struct pv_state {
    yscr_screen scr;
    ygfx_gfx    gfx;
    ytl_timeline tl;
    ytl_event   tl_events[8];
    ytl_event   fired[8];
    ygfx_stim   stims[PV_MAX_STIMS];
    int         n_stims;
    ygfx_bind   binds[8];
    ytl_key     keys[8][2];
    int         n_binds;
    ygfx_buf    dots;
    int         open, anchored;
    yscr_frame  f;
    /* per-frame log, allocated at open */
    pv_frame*    frames;
    yscr_record* recs;           /* by frame index; onset 0 = no record */
    int64_t      n_frames;       /* begin() calls that returned OK */
    int64_t      n_recs;
    int64_t      lost;           /* f.done_lost at the end */
    char         angle_dir[MAX_PATH * 3];
    char         msg[1024];
    /* the watcher and the key sender */
    HANDLE       watch_thread;
    volatile LONG watch_stop;
    pv_watch_ev  watch[PV_WATCH_MAX];
    volatile LONG n_watch;
    HANDLE       key_thread;
    int          key_presses, key_gap_ms, key_delay_ms;
    volatile LONG keys_sent, keys_skipped;
#ifndef PV_NO_AUDIO
    yau_audio   au;
    int         au_open;
#endif
} pv_state;

static pv_state* pv;   /* one per process, as screen.h keeps per-process state */
static int64_t pv_open_t;     /* when the screen opened (the window thread's CPU share) */

typedef struct pv_opts {
    int windowed;
    int display;            /* index into yscr_displays(); 0 = first */
    int w, h;
    const char* angle_dir;  /* NULL or "" = this module's directory */
    int panic;
    int foreground;         /* YSCR_FOREGROUND_*; 0 = AUTO */
    int backend;            /* yscr_backend; 0 = AUTO */
} pv_opts;

/* The directory of the module (.pyd, .mexw64, .exe) this code is in: the
 * bundle's directory, so ANGLE comes from the bundle and never from PATH. */
static int pv_module_dir(char* out, size_t cap) {
    HMODULE m = NULL;
    wchar_t w[MAX_PATH];
    DWORD n;
    wchar_t* slash;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)(void*)&pv_module_dir, &m)) return 0;
    n = GetModuleFileNameW(m, w, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return 0;
    slash = wcsrchr(w, L'\\');
    if (slash) *slash = 0;
    return WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)cap, NULL, NULL) > 0;
}

/* Loaded modules whose names matter to the probe (ANGLE, SDL, D3D compiler,
 * audio), with full paths. K32EnumProcessModules lives in kernel32 from
 * Windows 7 on: no psapi.lib. */
typedef BOOL (WINAPI *pv_enum_fn)(HANDLE, HMODULE*, DWORD, LPDWORD);
typedef DWORD (WINAPI *pv_name_fn)(HANDLE, HMODULE, LPWSTR, DWORD);
static int pv_modules(char* out, size_t cap, int all) {
    HMODULE k = GetModuleHandleW(L"kernel32.dll");
    pv_enum_fn en = (pv_enum_fn)(void*)GetProcAddress(k, "K32EnumProcessModules");
    pv_name_fn nm = (pv_name_fn)(void*)GetProcAddress(k, "K32GetModuleFileNameExW");
    static HMODULE mods[2048];
    DWORD need = 0, i, n;
    size_t used = 0;
    out[0] = 0;
    if (!en || !nm || !en(GetCurrentProcess(), mods, sizeof mods, &need)) return -1;
    n = need / sizeof(HMODULE);
    if (n > 2048) n = 2048;
    for (i = 0; i < n; i++) {
        wchar_t w[MAX_PATH];
        char u[MAX_PATH * 3], low[MAX_PATH * 3];
        size_t j;
        if (!nm(GetCurrentProcess(), mods[i], w, MAX_PATH)) continue;
        WideCharToMultiByte(CP_UTF8, 0, w, -1, u, (int)sizeof u, NULL, NULL);
        for (j = 0; u[j] && j < sizeof low - 1; j++) low[j] = (char)((u[j] >= 'A' && u[j] <= 'Z') ? u[j] + 32 : u[j]);
        low[j] = 0;
        if (!all && !strstr(low, "egl") && !strstr(low, "gles") && !strstr(low, "sdl") && !strstr(low, "d3dcompiler") &&
            !strstr(low, "d3d11") && !strstr(low, "dxgi") && !strstr(low, "mmdevapi") && !strstr(low, "audioses") &&
            !strstr(low, "ysp_av") && !strstr(low, "avrt") && !strstr(low, "ksuser") && !strstr(low, "winmm") &&
            !strstr(low, "dcomp") && !strstr(low, "opengl"))
            continue;
        if (used + strlen(u) + 2 >= cap) break;
        used += (size_t)snprintf(out + used, cap - used, "%s\n", u);
    }
    return (int)n;
}

/* DisableProcessWindowsGhosting(): for the whole process, for its life.
 * The header does not call it (docs/screen.md); the probe measures it. */
static void pv_noghost(void) { DisableProcessWindowsGhosting(); }

/* Pumps the window's messages between frames, as a binding's idle wait
 * would; the events are dropped (the probe reads none). */
static int pv_pump(void) {
    SDL_Event ev;
    int n = 0;
    if (!pv || !pv->open) return -1;
    while (yscr_poll(&pv->scr, &ev, NULL)) n++;
    return n;
}

static void* pv_preload(const char* path) {
    wchar_t w[MAX_PATH];
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, w, MAX_PATH)) return NULL;
    return (void*)LoadLibraryW(w);
}

static int pv_displays(char* out, size_t cap) {
    yscr_display_info d[8];
    int n = yscr_displays(d, 8), i;
    size_t used = 0;
    out[0] = 0;
    for (i = 0; i < n; i++)
        used += (size_t)snprintf(out + used, cap - used, "%d id=%u %s %dx%d %d/%d Hz%s\n", i, d[i].id, d[i].name,
                                 d[i].w, d[i].h, d[i].desktop.refresh_num, d[i].desktop.refresh_den,
                                 d[i].primary ? " primary" : "");
    return n;
}

static void pv_free(void) {
    if (!pv) return;
    free(pv->frames);
    free(pv->recs);
    free(pv);
    pv = NULL;
}

static void pv_store(const yscr_record* r) {
    if (r->index >= 0 && r->index < PV_MAX_FRAMES) {
        if (pv->recs[r->index].onset == 0 && r->onset != 0) pv->n_recs++;
        pv->recs[r->index] = *r;
    }
}

/* Every completed record, also those that complete in wait_flip() and
 * close(), which f.done never shows. */
static void pv_on_flip(void* ctx, const yscr_record* r, const yscr_trigger_result* t, int n) {
    (void)ctx; (void)t; (void)n;
    pv_store(r);
}

static void pv_watch_stop_(void);
static void pv_keys_join(void);

/* Returns 1, or 0 with pv_last_error(). */
static char pv_err[1024];
static const char* pv_last_error(void) { return pv_err; }

static int pv_open(const pv_opts* o) {
    yscr_desc sd;
    ygfx_desc gd;
    ytl_desc td;
    yscr_display_info d[8];
    int nd;
    if (pv) { snprintf(pv_err, sizeof pv_err, "already open"); return 0; }
    pv = (pv_state*)calloc(1, sizeof *pv);
    if (!pv) { snprintf(pv_err, sizeof pv_err, "out of memory"); return 0; }
    pv->frames = (pv_frame*)calloc(PV_MAX_FRAMES, sizeof(pv_frame));
    pv->recs = (yscr_record*)calloc(PV_MAX_FRAMES, sizeof(yscr_record));
    if (!pv->frames || !pv->recs) { pv_free(); snprintf(pv_err, sizeof pv_err, "out of memory"); return 0; }
    if (o->angle_dir && o->angle_dir[0]) snprintf(pv->angle_dir, sizeof pv->angle_dir, "%s", o->angle_dir);
    else pv_module_dir(pv->angle_dir, sizeof pv->angle_dir);

    memset(&sd, 0, sizeof sd);
    sd.windowed = o->windowed != 0;
    sd.window_w = o->w;
    sd.window_h = o->h;
    sd.angle_dir = pv->angle_dir;
    sd.panic = o->panic != 0;
    sd.foreground = o->foreground;
    sd.backend = (yscr_backend)o->backend;
    nd = yscr_displays(d, 8);
    if (o->display > 0) {
        if (o->display >= nd) { pv_free(); snprintf(pv_err, sizeof pv_err, "display %d of %d", o->display, nd); return 0; }
        sd.display = d[o->display].id;
    }
    memset(&gd, 0, sizeof gd);
    gd.background[0] = gd.background[1] = gd.background[2] = 0.5f;
    gd.max_draws = 1024;
    if (!ygfx_open_screen(&pv->gfx, &pv->scr, &sd, &gd)) {
        snprintf(pv_err, sizeof pv_err, "%s", ygfx_error(&pv->gfx));
        pv_free();
        return 0;
    }
    memset(&td, 0, sizeof td);
    td.events = pv->tl_events;
    td.event_capacity = 8;
    td.n_channels = 8;
    if (!ytl_open(&pv->tl, &td)) {
        snprintf(pv_err, sizeof pv_err, "%s", ytl_error(&pv->tl));
        ygfx_close(&pv->gfx); yscr_close(&pv->scr); pv_free();
        return 0;
    }
    {   /* one dot buffer that every dot field shares */
        float xy[2 * PV_DOTS];
        uint32_t seed = 1;
        int i = 0;
        while (i < PV_DOTS) {
            float u, v;
            seed = seed * 1664525u + 1013904223u; u = (float)(seed >> 8) / 8388608.0f - 1.0f;
            seed = seed * 1664525u + 1013904223u; v = (float)(seed >> 8) / 8388608.0f - 1.0f;
            if (u * u + v * v > 1.0f) continue;
            xy[2 * i] = u * 40; xy[2 * i + 1] = v * 40;
            i++;
        }
        pv->dots = ygfx_buffer(&pv->gfx, sizeof xy);
        ygfx_buffer_update(&pv->gfx, pv->dots, 0, xy, sizeof xy);
    }
    yscr_on_flip(&pv->scr, pv_on_flip, NULL);
    pv->open = 1;
    pv_open_t = yrt_now_ns();
    return 1;
}

/* kind: 0 gabor, 1 grating, 2 dot field (100 dots), 3 rect. Returns the
 * stimulus index, or -1. */
static int pv_add(int kind, float x, float y, float size) {
    ygfx_stim s;
    if (!pv || pv->n_stims >= PV_MAX_STIMS) return -1;
    if (kind == 0) {
        ygfx_gabor_desc g; memset(&g, 0, sizeof g);
        g.x = x; g.y = y; g.sf = 1 / 16.0f; g.sigma = size / 6; g.contrast = 0.5f;
        s = ygfx_gabor(&g);
    } else if (kind == 1) {
        ygfx_grating_desc g; memset(&g, 0, sizeof g);
        g.x = x; g.y = y; g.w = g.h = size; g.sf = 1 / 16.0f; g.contrast = 0.5f;
        g.aperture = YGFX_CIRCLE; g.edge = YGFX_EDGE_COSINE; g.edge_width = 4;
        s = ygfx_grating(&g);
    } else if (kind == 2) {
        ygfx_dots_desc d; memset(&d, 0, sizeof d);
        d.x = x; d.y = y; d.buf = pv->dots; d.count = PV_DOTS; d.dot_size = 3;
        d.edge = YGFX_EDGE_COSINE; d.edge_width = 1;
        d.color[0] = d.color[1] = d.color[2] = 0.8f;
        s = ygfx_dots(&d);
    } else {
        ygfx_shape_desc r; memset(&r, 0, sizeof r);
        r.shape = YGFX_RECT; r.x = x; r.y = y; r.w = size; r.h = size / 2;
        r.color[0] = 0.3f; r.color[1] = 0.3f; r.color[2] = 0.6f;
        s = ygfx_shape(&r);
    }
    pv->stims[pv->n_stims] = s;
    return pv->n_stims++;
}

/* Binds stimulus i's parameter to a channel with a repeating linear track
 * from v0 to v1 over period_ns: a tween that never ends. */
static int pv_bind(int i, int param, int64_t period_ns, float v0, float v1) {
    ytl_track t;
    int ch;
    if (!pv || i < 0 || i >= pv->n_stims || pv->n_binds >= 8 || period_ns <= 0) return -1;
    ch = pv->n_binds;
    pv->keys[ch][0].time_ns = 0; pv->keys[ch][0].value = v0; pv->keys[ch][0].ease = YTL_EASE_LINEAR;
    pv->keys[ch][1].time_ns = period_ns; pv->keys[ch][1].value = v1; pv->keys[ch][1].ease = YTL_EASE_LINEAR;
    memset(&t, 0, sizeof t);
    t.keys = pv->keys[ch]; t.n_keys = 2; t.period_ns = period_ns;
    if (ytl_set_track(&pv->tl, ch, 1, &t) < 0) return -1;
    memset(&pv->binds[ch], 0, sizeof pv->binds[ch]);
    pv->binds[ch].stim = &pv->stims[i];
    pv->binds[ch].param = (uint16_t)param;
    pv->binds[ch].channel = (uint16_t)ch;
    pv->n_binds++;
    return ch;
}

/* The probe's scene, the same for every host: n stimuli on a grid of 80 px
 * around the center, kinds in turn gabor, grating, dot field, rect, each
 * 64 px; stimulus 0's phase follows a 1 s repeating tween. */
static int pv_setup(int n) {
    int i, cols = 1;
    while (cols * cols < n) cols++;
    for (i = 0; i < n; i++) {
        float x = (float)((i % cols) - (cols - 1) * 0.5) * 80.0f;
        float y = (float)((i / cols) - (cols - 1) * 0.5) * 80.0f;
        if (pv_add(i % 4, x, y, 64.0f) < 0) return -1;
    }
    if (n > 0 && pv_bind(0, YGFX_P_PHASE, YRT_S(1), 0.0f, 1.0f) < 0) return -1;
    ygfx_prime(&pv->gfx);
    return n;
}

/* Sets one parameter through the header's own table. */
static int pv_set(int i, int param, float v) {
    ygfx_bind b;
    if (!pv || i < 0 || i >= pv->n_stims) return -1;
    memset(&b, 0, sizeof b);
    b.stim = &pv->stims[i];
    b.param = (uint16_t)param;
    b.channel = 0;
    return ygfx_apply(&b, 1, &v);
}

/* begin(): the host releases its lock around this. Returns YSCR_OK,
 * YSCR_QUIT or < 0. */
static int pv_begin(void) {
    int64_t t0 = yrt_now_ns(), t1;
    int rc = yscr_begin(&pv->scr, &pv->f);
    t1 = yrt_now_ns();
    if (rc != YSCR_OK) return rc;
    pv->lost = pv->f.done_lost;
    if (pv->f.index >= 0 && pv->f.index < PV_MAX_FRAMES) {
        pv_frame* fr = &pv->frames[pv->f.index];
        fr->begin_call = t0; fr->begin_ret = t1; fr->onset = pv->f.onset;
    }
    pv->n_frames++;
    return rc;
}

static int pv_evaluate(void) {
    ytl_frame tf;
    if (!pv->anchored) { ytl_anchor(&pv->tl, 1, pv->f.onset, 0); pv->anchored = 1; }
    tf.onset = pv->f.onset; tf.period = pv->f.period; tf.index = pv->f.index;
    ytl_evaluate(&pv->tl, &tf, pv->fired, 8);
    if (pv->n_binds) ygfx_apply(pv->binds, pv->n_binds, ytl_values(&pv->tl));
    yscr_mark(&pv->scr, YSCR_PHASE_EVALUATE);
    return 0;
}

static int pv_gfx_begin(void) { return ygfx_begin(&pv->gfx, &pv->f); }
static int pv_draw(int i) {
    if (i < 0 || i >= pv->n_stims) return -1;
    return ygfx_draw(&pv->gfx, &pv->stims[i]);
}
static int pv_gfx_end(void) { return ygfx_end(&pv->gfx); }

/* flip_at(): the host releases its lock around this. */
static int pv_flip_at(int64_t t) {
    int64_t t2 = yrt_now_ns(), t3;
    int rc = yscr_flip_at(&pv->scr, t, NULL);
    t3 = yrt_now_ns();
    if (pv->f.index >= 0 && pv->f.index < PV_MAX_FRAMES) {
        pv->frames[pv->f.index].flip_call = t2;
        pv->frames[pv->f.index].flip_ret = t3;
    }
    return rc;
}

/* The whole frame for N stimuli in C: the reference loop. */
static int pv_frame_c(int set_each) {
    int rc = pv_begin(), i;
    if (rc != YSCR_OK) return rc;
    pv_evaluate();
    if (set_each)
        for (i = 0; i < pv->n_stims; i++) pv_set(i, YGFX_P_ORI, (float)(pv->f.index % 360));
    pv_gfx_begin();
    for (i = 0; i < pv->n_stims; i++) pv_draw(i);
    pv_gfx_end();
    return pv_flip_at(pv->f.onset);
}

static int pv_describe(char* out, size_t cap) {
    char a[1200], b[600], gl[400] = "";
    typedef const unsigned char* (APIENTRY *gs_fn)(unsigned);
    gs_fn gs = (gs_fn)(void*)yscr_gl_proc(&pv->scr, "glGetString");
    yscr_describe(&pv->scr, a, sizeof a);
    ygfx_describe(&pv->gfx, b, sizeof b);
    if (gs) {
        yscr_bind(&pv->scr);
        snprintf(gl, sizeof gl, "GL_VERSION=%s | GL_RENDERER=%s | GL_VENDOR=%s",
                 (const char*)gs(0x1F02), (const char*)gs(0x1F01), (const char*)gs(0x1F00));
    }
    return snprintf(out, cap, "%s\n%s\n%s\nangle_dir=%s window_thread=%d", a, b, gl, pv->angle_dir, (int)pv->scr.wt);
}

/* The window thread's CPU time over the screen's life (winthread probe). */
static int64_t pv_wt_cpu_ns = -1, pv_wt_life_ns;
static void pv_wt_cpu(void) {
    FILETIME c, e, k, u;
    if (!pv || !pv->scr.wt || !yscr__wt.thread || !GetThreadTimes(yscr__wt.thread, &c, &e, &k, &u)) return;
    pv_wt_cpu_ns = (int64_t)((((uint64_t)k.dwHighDateTime << 32) | k.dwLowDateTime) +
                             (((uint64_t)u.dwHighDateTime << 32) | u.dwLowDateTime)) * 100;
    pv_wt_life_ns = yrt_now_ns() - pv_open_t;
}

static void pv_close(void) {
    if (!pv) return;
    pv_wt_cpu();
    pv_watch_stop_();
    pv_keys_join();
#ifndef PV_NO_AUDIO
    if (pv->au_open) { yau_close(&pv->au); pv->au_open = 0; }
#endif
    if (pv->open) {
        yscr_wait_flip(&pv->scr, NULL);   /* the last records, through on_flip */
        ytl_close(&pv->tl);
        ygfx_close(&pv->gfx);
        yscr_close(&pv->scr);
        pv->open = 0;
    }
}

/* CSV of every frame: what the analysis reads. */
static int pv_dump(const char* path, const char* tag) {
    FILE* fp;
    int64_t i, n;
    if (!pv) return -1;
    fp = fopen(path, "wb");
    if (!fp) return -1;
    fprintf(fp, "# %s\n", tag ? tag : "");
    fprintf(fp, "index,begin_wait_ns,work_ns,flip_ns,outside_ns,predicted,target,planned,onset,residual,dropped,path,tier,flags\n");
    n = pv->n_frames < PV_MAX_FRAMES ? pv->n_frames : PV_MAX_FRAMES;
    for (i = 0; i < n; i++) {
        const pv_frame* f = &pv->frames[i];
        const yscr_record* r = &pv->recs[i];
        int64_t outside = i > 0 && pv->frames[i - 1].flip_ret ? f->begin_call - pv->frames[i - 1].flip_ret : -1;
        fprintf(fp, "%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%u,%u,%u,%u\n", (long long)i,
                (long long)(f->begin_ret - f->begin_call), (long long)(f->flip_call - f->begin_ret),
                (long long)(f->flip_ret - f->flip_call), (long long)outside, (long long)f->onset,
                (long long)r->target, (long long)r->planned, (long long)r->onset, (long long)r->residual,
                (unsigned)r->dropped, (unsigned)r->path, (unsigned)r->tier, (unsigned)r->flags);
    }
    fclose(fp);
    return (int)n;
}

/* --- the idle watcher: is the window hung, ghosted, still in front? ----- */

static HWND pv_hwnd(void) {
    SDL_Window* w = yscr_window(&pv->scr);
    if (!w) return NULL;
    return (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(w), SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
}

static int pv_is_ghost(HWND h) {
    char cls[64];
    return h && GetClassNameA(h, cls, sizeof cls) && strcmp(cls, "Ghost") == 0;
}

static DWORD WINAPI pv_watch_main(void* arg) {
    HWND h = (HWND)arg;
    int64_t t0 = yrt_now_ns();
    pv_watch_ev last;
    HMONITOR mon = MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST);
    memset(&last, 0xff, sizeof last);
    while (!InterlockedCompareExchange(&pv->watch_stop, 0, 0)) {
        pv_watch_ev e;
        HWND fg = GetForegroundWindow();
        RECT wr; MONITORINFO mi;
        mi.cbSize = sizeof mi;
        e.t = yrt_now_ns() - t0;
        e.hung = IsHungAppWindow(h) ? 1 : 0;
        e.fg_ours = fg == h;
        e.fg_ghost = pv_is_ghost(fg);
        e.iconic = IsIconic(h) ? 1 : 0;
        e.covers = GetWindowRect(h, &wr) && GetMonitorInfoW(mon, &mi) && EqualRect(&wr, &mi.rcMonitor);
        if (e.hung != last.hung || e.fg_ours != last.fg_ours || e.fg_ghost != last.fg_ghost ||
            e.iconic != last.iconic || e.covers != last.covers) {
            LONG k = pv->n_watch;
            if (k < PV_WATCH_MAX) { pv->watch[k] = e; InterlockedIncrement(&pv->n_watch); }
            last = e;
        }
        Sleep(50);
    }
    return 0;
}

static int pv_watch_start(void) {
    HWND h = pv_hwnd();
    if (!h || pv->watch_thread) return -1;
    pv->watch_stop = 0;
    pv->n_watch = 0;
    pv->watch_thread = CreateThread(NULL, 0, pv_watch_main, h, 0, NULL);
    return pv->watch_thread ? 0 : -1;
}

static void pv_watch_stop_(void) {
    if (!pv || !pv->watch_thread) return;
    InterlockedExchange(&pv->watch_stop, 1);
    WaitForSingleObject(pv->watch_thread, INFINITE);
    CloseHandle(pv->watch_thread);
    pv->watch_thread = NULL;
}

/* Stops the watcher and writes its transitions: "t_s hung fg_ours fg_ghost iconic covers". */
static int pv_watch_stop(char* out, size_t cap) {
    LONG i;
    size_t used = 0;
    pv_watch_stop_();
    out[0] = 0;
    for (i = 0; i < pv->n_watch && used < cap; i++)
        used += (size_t)snprintf(out + used, cap - used, "%.3f hung=%d fg_ours=%d fg_ghost=%d iconic=%d covers=%d\n",
                                 pv->watch[i].t * 1e-9, pv->watch[i].hung, pv->watch[i].fg_ours,
                                 pv->watch[i].fg_ghost, pv->watch[i].iconic, pv->watch[i].covers);
    return (int)pv->n_watch;
}

/* --- Shift+Esc from a helper thread, sent only while our window (or its
 * ghost) is in front, so no other program gets the keys. -------------- */

static void pv_send_shift_esc(void) {
    INPUT in[4];
    memset(in, 0, sizeof in);
    in[0].type = INPUT_KEYBOARD; in[0].ki.wScan = 0x2A; in[0].ki.dwFlags = KEYEVENTF_SCANCODE;
    in[1].type = INPUT_KEYBOARD; in[1].ki.wScan = 0x01; in[1].ki.dwFlags = KEYEVENTF_SCANCODE;
    in[2].type = INPUT_KEYBOARD; in[2].ki.wScan = 0x01; in[2].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
    in[3].type = INPUT_KEYBOARD; in[3].ki.wScan = 0x2A; in[3].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
    SendInput(4, in, sizeof(INPUT));
}

static DWORD WINAPI pv_key_main(void* arg) {
    HWND h = (HWND)arg;
    int i;
    Sleep((DWORD)pv->key_delay_ms);
    for (i = 0; i < pv->key_presses; i++) {
        HWND fg = GetForegroundWindow();
        if (fg == h || (pv_is_ghost(fg) && IsHungAppWindow(h))) {
            pv_send_shift_esc();
            InterlockedIncrement(&pv->keys_sent);
        } else InterlockedIncrement(&pv->keys_skipped);
        Sleep((DWORD)pv->key_gap_ms);
    }
    return 0;
}

static int pv_keys(int presses, int delay_ms, int gap_ms) {
    HWND h = pv_hwnd();
    if (!h || pv->key_thread) return -1;
    pv->key_presses = presses; pv->key_delay_ms = delay_ms; pv->key_gap_ms = gap_ms;
    pv->keys_sent = pv->keys_skipped = 0;
    pv->key_thread = CreateThread(NULL, 0, pv_key_main, h, 0, NULL);
    return pv->key_thread ? 0 : -1;
}

static void pv_keys_join(void) {
    if (!pv || !pv->key_thread) return;
    WaitForSingleObject(pv->key_thread, INFINITE);
    CloseHandle(pv->key_thread);
    pv->key_thread = NULL;
}

/* --- the window thread's state, and input latency (winthread probe) ---- */

static int pv_wt_state(char* out, size_t cap) {
    HWND h = pv && pv->open ? pv_hwnd() : NULL;
    return snprintf(out, cap, "wt=%d released=%ld iconic=%d hung=%d wt_alive=%d wt_cpu_ms=%.1f over_s=%.1f",
                    pv ? (int)pv->scr.wt : -1, (long)yscr__wt.released, h ? IsIconic(h) != 0 : -1,
                    h ? IsHungAppWindow(h) != 0 : -1, yscr__wt_alive(1000000000LL), pv_wt_cpu_ns * 1e-6,
                    pv_wt_life_ns * 1e-9);
}

/* The cost of one synchronous command to the window thread (a text input
 * toggle, a gamepad open): n no-op round trips, the median in ns. */
static void pv_noop(void* p) { (void)p; }
static int pv_lat_cmp_(const void* a, const void* b) {
    int64_t x = *(const int64_t*)a, y = *(const int64_t*)b;
    return x < y ? -1 : x > y;
}
static int64_t pv_wt_roundtrip(int n, int64_t* max_ns) {
    static int64_t v[1000];
    int i;
    if (!yscr__wt.thread || n <= 0 || n > 1000) return -1;
    for (i = 0; i < n; i++) {
        int64_t t = yrt_now_ns();
        yscr__wt_run(pv_noop, NULL);
        v[i] = yrt_now_ns() - t;
    }
    qsort(v, (size_t)n, sizeof *v, pv_lat_cmp_);
    *max_ns = v[n - 1];
    return v[n / 2];
}

#define PV_LAT_MAX 2000
typedef struct pv_lat { int64_t send, got, stamp; } pv_lat;
static struct {
    pv_lat        r[PV_LAT_MAX];
    volatile LONG armed, stop, skipped;
    int           kind, n;
    HWND          h;
} pv_l;

/* kind 0: the B key by scan code (SDL's raw keyboard path); kind 1: a left
 * click where the cursor is (the message path). */
static void pv_send_probe(int kind) {
    INPUT in[2];
    memset(in, 0, sizeof in);
    if (kind == 0) {
        in[0].type = INPUT_KEYBOARD; in[0].ki.wScan = 0x30; in[0].ki.dwFlags = KEYEVENTF_SCANCODE;
        in[1].type = INPUT_KEYBOARD; in[1].ki.wScan = 0x30; in[1].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
    } else {
        in[0].type = INPUT_MOUSE; in[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
        in[1].type = INPUT_MOUSE; in[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    }
    SendInput(2, in, sizeof(INPUT));
}

static DWORD WINAPI pv_lat_main(void* arg) {
    int i = 0;
    uint32_t seed = 12345;
    (void)arg;
    while (i < pv_l.n && !pv_l.stop) {
        if (!InterlockedCompareExchange(&pv_l.armed, 0, 0)) { Sleep(1); continue; }
        seed = seed * 1664525u + 1013904223u;
        Sleep(20 + (seed >> 16) % 30);   /* not locked to the frame phase */
        if (GetForegroundWindow() != pv_l.h) { InterlockedIncrement(&pv_l.skipped); Sleep(100); continue; }
        InterlockedExchange(&pv_l.armed, 0);
        pv_l.r[i].send = yrt_now_ns();
        pv_send_probe(pv_l.kind);
        i++;
    }
    return 0;
}

static int pv_lat_cmp(const void* a, const void* b) {
    int64_t x = *(const int64_t*)a, y = *(const int64_t*)b;
    return x < y ? -1 : x > y;
}

static void pv_lat_sum(const char* what, int64_t* v, int n, char* out, size_t cap, size_t* used) {
    if (n <= 0) { *used += (size_t)snprintf(out + *used, cap - *used, "%s: none\n", what); return; }
    qsort(v, (size_t)n, sizeof *v, pv_lat_cmp);
    *used += (size_t)snprintf(out + *used, cap - *used, "%s ms: min %.3f p50 %.3f p90 %.3f p99 %.3f max %.3f (n %d)\n", what,
                              v[0] * 1e-6, v[n / 2] * 1e-6, v[(n * 9) / 10] * 1e-6, v[(n * 99) / 100] * 1e-6, v[n - 1] * 1e-6, n);
}

static int pv_lat_hit(int kind, const SDL_Event* ev) {
    return kind == 0 ? (ev->type == SDL_EVENT_KEY_DOWN && ev->key.scancode == SDL_SCANCODE_B)
                     : ev->type == SDL_EVENT_MOUSE_BUTTON_DOWN;
}

/* n probes of one kind; the frame thread either spins on yscr_poll() or
 * runs the scene's frames and polls after each begin(). "avail" is the
 * send to the poll that returned the event, "stamp" the send to the
 * event's own time on the ysp_rt clock. */
static int pv_latency(int kind, int n, int in_frames, char* out, size_t cap) {
    HANDLE th;
    int i, got = 0;
    size_t used = 0;
    static int64_t a[PV_LAT_MAX], st[PV_LAT_MAX];
    SDL_Event ev;
    if (!pv || !pv->open || n > PV_LAT_MAX) return -1;
    memset(&pv_l, 0, sizeof pv_l);
    pv_l.kind = kind; pv_l.n = n; pv_l.h = pv_hwnd();
    if (kind == 1) {
        RECT r;
        GetWindowRect(pv_l.h, &r);
        SetCursorPos((r.left + r.right) / 2, (r.top + r.bottom) / 2);
    }
    while (yscr_poll(&pv->scr, &ev, NULL)) { }
    th = CreateThread(NULL, 0, pv_lat_main, NULL, 0, NULL);
    for (i = 0; i < n; i++) {
        int64_t end = yrt_now_ns() + YRT_S(20);
        int found = 0;
        InterlockedExchange(&pv_l.armed, 1);
        while (!found && yrt_now_ns() < end) {
            int64_t t = 0;
            if (in_frames) {
                int rc = pv_begin(), k;
                if (rc != YSCR_OK) break;
                while (yscr_poll(&pv->scr, &ev, &t))
                    if (!found && pv_lat_hit(kind, &ev)) { pv_l.r[i].got = yrt_now_ns(); pv_l.r[i].stamp = t; found = 1; }
                pv_evaluate();
                pv_gfx_begin();
                for (k = 0; k < pv->n_stims; k++) pv_draw(k);
                pv_gfx_end();
                pv_flip_at(pv->f.onset);
            } else if (yscr_poll(&pv->scr, &ev, &t) && pv_lat_hit(kind, &ev)) {
                pv_l.r[i].got = yrt_now_ns(); pv_l.r[i].stamp = t; found = 1;
            }
        }
        if (!found) break;
        a[got] = pv_l.r[i].got - pv_l.r[i].send;
        st[got] = pv_l.r[i].stamp - pv_l.r[i].send;
        got++;
    }
    InterlockedExchange(&pv_l.stop, 1);
    WaitForSingleObject(th, INFINITE);
    CloseHandle(th);
    used += (size_t)snprintf(out + used, cap - used, "latency kind=%s mode=%s wt=%d probes=%d skipped=%ld\n",
                             kind ? "click" : "key", in_frames ? "frames" : "spin", (int)pv->scr.wt, got, (long)pv_l.skipped);
    pv_lat_sum("avail", a, got, out, cap, &used);
    pv_lat_sum("stamp", st, got, out, cap, &used);
    return got;
}

/* --- audio beside the screen: load and init conflicts only ------------- */

static int pv_audio_open(int null_device, char* out, size_t cap) {
#ifndef PV_NO_AUDIO
    yau_desc d;
    if (!pv || pv->au_open) return -1;
    memset(&d, 0, sizeof d);
    d.backend = null_device ? YAU_BACKEND_NULL : YAU_BACKEND_AUTO;
    if (!yau_open(&pv->au, &d)) { snprintf(out, cap, "%s", yau_error(&pv->au)); return -1; }
    pv->au_open = 1;
    yau_describe(&pv->au, out, cap);
    {   /* one short quiet tone, so the device's render path runs */
        yau_tone_desc t; yau_buf b;
        memset(&t, 0, sizeof t);
        t.hz = 1000; t.dur_ns = YRT_MS(50); t.peak = 0.01f; t.ramp_ns = YRT_MS(5);
        b = yau_tone(&pv->au, &t);
        yau_play_at(&pv->au, b, yrt_now_ns() + YRT_MS(100));
    }
    return 0;
#else
    (void)null_device;
    snprintf(out, cap, "built with PV_NO_AUDIO");
    return -1;
#endif
}

#endif
