/* ysp_vis_core.h - the host-independent part of ysp-vis (Python ysp._vis, MEX ysp_vis)
 *
 * Not a library header. The ysp-vis modules (bindings/python/ysp_vis's
 * ysp._vis, bindings/mex/ysp_vis.c) include it once, in their one
 * translation unit, after Python.h or mex.h and after defining
 * YSP_SHARE_PYTHON or YSP_SHARE_MEX. It compiles:
 *
 *   - the implementations of ysp/screen.h, ysp/gfx.h, ysp/video.h,
 *     ysp/color.h and ysp/rt.h;
 *   - ysp/audio.h and ysp/timeline.h as declarations only, with the
 *     forwarders of bindings/share/ (the shims): ysp.audio and
 *     ysp.timeline (ysp_audio and ysp_timeline in MEX) own the one
 *     implementation of each (docs/bindings.md, "Sharing one
 *     implementation across modules");
 *   - what both hosts do the same way: the module's own directory (always
 *     desc.angle_dir, docs/av_probe.md section 2), the open options, the
 *     ring of flip records, the input events of a frame, the window calls,
 *     seconds to ns; and, in ysp_vis_gfx.h (included at the end), the gfx:
 *     stimuli, lists, groups, textures, instances and timeline bindings.
 *
 * THREADS. A screen has one frame thread (the host's). The functions here
 * run on it; the host keeps a second thread out (Python: BusyError while a
 * call runs without the GIL). The on_flip hook runs on the frame thread
 * inside begin(), flip_at(), wait_flip() and close(), so the record ring
 * needs no lock. No host callback runs on a C thread.
 *
 * MEMORY. A screen's record ring (YV_RECORDS) and event ring (YV_EVENTS)
 * are allocated once at open; begin(), flip(), records() and poll()
 * allocate nothing in C.
 *
 * Public domain / MIT-0, as the headers.
 */
#ifndef YSP_VIS_CORE_H
#define YSP_VIS_CORE_H

#if !defined(YSP_SHARE_PYTHON) && !defined(YSP_SHARE_MEX)
#error "define YSP_SHARE_PYTHON or YSP_SHARE_MEX before including ysp_vis_core.h"
#endif

/* The owners compile ysp/timeline.h with 1024 channels; the check refuses
 * another value. */
#define YTL_MAX_CHANNELS 1024
/* pl_mpeg.h (MPEG-1) comes from third_party/vis/pl_mpeg (tools/vendor_vis.py). */

/* Declarations only: the owners' modules hold the implementations. */
#include "ysp/audio.h"
#include "ysp/timeline.h"
#define YSP_TIMELINE_IMPLEMENTATION_GUARD   /* video.h must not pull timeline.h's in */
#define YSP_AUDIO_API_SHIMS
#include "ysp_audio_api.h"
#define YSP_TIMELINE_API_SHIMS
#include "ysp_timeline_api.h"

/* Text: one curve set per font file of each fallback list (ysp_vis_text.h),
 * so more than the header's 8; a set is a texture pair, made on first use. */
#define YGFX_MAX_CSETS 32

#define YSP_SCREEN_IMPLEMENTATION
#include "ysp/screen.h"
#define YSP_VIDEO_IMPLEMENTATION
#include "ysp/video.h"   /* brings gfx.h's, color.h's and rt.h's */
/* Text layout: pack/layout (Skribidi, HarfBuzz), linked as static libraries
 * (tools/vendor_vis.py --layout); it calls ysp/outline.h, compiled here. */
#define YSP_OUTLINE_IMPLEMENTATION
#include "ysp/outline.h"
#include "ysp/layout.h"

#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* The distribution's version tracks screen.h's, with a fourth number for a
 * change of the binding alone (docs/bindings.md, ysp-vis). */
#define YV_VERSION YSCR_VERSION_STRING ".1"   /* .1: text, phase C (a binding-only change since 0.10.2) */

#define YV_RECORDS 4096   /* flip records kept between records() calls */
#define YV_EVENTS  1024   /* input events kept between poll() calls    */

/* ---- seconds ------------------------------------------------------------- */

#define YV_S_MAX 9223372036.0   /* INT64_MAX ns, in s */

/* Seconds to the nearest ns, as the other bindings round (fma gives the
 * product's rounding error). 0, or -1 for NaN, infinity, or a negative
 * time when `negative` is 0. */
static int yv_s_to_ns(double s, int negative, int64_t* out) {
    volatile double p;
    double e, n, r;
    if (!(s == s) || s >= YV_S_MAX || s <= -YV_S_MAX || (!negative && s < 0.0)) return -1;
    p = s * 1e9;
    e = fma(s, 1e9, -p);
    n = floor(p + 0.5);
    r = (p - n) + e;
    if (r > 0.5) n += 1.0;
    else if (r < -0.5) n -= 1.0;
    *out = (int64_t)n;
    return 0;
}

static double yv_s(int64_t ns) { return (double)ns / 1e9; }

/* ---- the module's own directory -------------------------------------------- */

/* UTF-8, without the trailing separator; "" when unknown. ysp/screen.h loads
 * libEGL.dll from it by full path, so another ANGLE in the process (MATLAB's,
 * a Qt program's) is never the one used (docs/av_probe.md, section 2). */
static const char* yv_module_dir(void) {
    static char dir[1024];
    static int done;
    if (done) return dir;
    done = 1;
#if defined(_WIN32)
    {
        HMODULE self = NULL;
        wchar_t path[1024];
        DWORD n, i, cut = 0;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                (LPCWSTR)(const void*)&yv_module_dir, &self))
            return dir;
        n = GetModuleFileNameW(self, path, (DWORD)(sizeof path / sizeof path[0]));
        if (n == 0 || n >= sizeof path / sizeof path[0] - 1) return dir;
        for (i = 0; i < n; i++) if (path[i] == L'\\' || path[i] == L'/') cut = i;
        path[cut] = 0;
        if (WideCharToMultiByte(CP_UTF8, 0, path, -1, dir, (int)sizeof dir, NULL, NULL) <= 0) dir[0] = 0;
    }
#endif
    return dir;
}

/* ---- open options ------------------------------------------------------------ */
/* One table for both hosts, so a Python keyword and a MATLAB field are the
 * same option with the same unit. Times are seconds. */

enum { YV_BOOL, YV_I32, YV_U32, YV_F64, YV_SEC, YV_SEC_SIGNED, YV_ENUM, YV_SIZE, YV_MODE };

typedef struct yv_name { const char* name; int value; } yv_name;

static const yv_name yv_backends[] = {
    { "auto", YSCR_BACKEND_AUTO }, { "dxgi_flip", YSCR_BACKEND_DXGI_FLIP },
    { "composition", YSCR_BACKEND_COMPOSITION }, { "sim", YSCR_BACKEND_SIM }, { NULL, 0 } };
static const yv_name yv_settles[] = {
    { "auto", YSCR_SETTLE_AUTO }, { "strict", YSCR_SETTLE_STRICT }, { "warn", YSCR_SETTLE_WARN },
    { "off", YSCR_SETTLE_OFF }, { NULL, 0 } };
static const yv_name yv_foregrounds[] = {
    { "auto", YSCR_FOREGROUND_AUTO }, { "honor", YSCR_FOREGROUND_HONOR }, { "force", YSCR_FOREGROUND_FORCE },
    { NULL, 0 } };
static const yv_name yv_window_threads[] = {
    { "auto", YSCR_WINDOW_THREAD_AUTO }, { "on", YSCR_WINDOW_THREAD_ON }, { "off", YSCR_WINDOW_THREAD_OFF },
    { NULL, 0 } };
static const yv_name yv_corners[] = {
    { "top_left", YSCR_TOP_LEFT }, { "top_right", YSCR_TOP_RIGHT }, { "bottom_left", YSCR_BOTTOM_LEFT },
    { "bottom_right", YSCR_BOTTOM_RIGHT }, { NULL, 0 } };

typedef struct yv_opt {
    const char*    name;
    int            type;
    size_t         off;     /* in yscr_desc */
    const yv_name* names;   /* YV_ENUM */
    const char*    doc;
} yv_opt;

#define YV_OFF(f) offsetof(yscr_desc, f)
static const yv_opt yv_opts[] = {
    { "windowed", YV_BOOL, YV_OFF(windowed), NULL, "a window, not the full screen" },
    { "size", YV_SIZE, 0, NULL, "the window's (w, h) in pixels; (0, 0) = 800 x 600" },
    { "display", YV_U32, YV_OFF(display), NULL, "a display's id from displays(); 0 = the primary" },
    { "mode", YV_MODE, 0, NULL, "a listed mode (w, h, refresh_num, refresh_den) to switch to; none = the desktop's" },
    { "backend", YV_ENUM, YV_OFF(backend), yv_backends, "auto, dxgi_flip, composition or sim" },
    { "lead", YV_F64, YV_OFF(lead), NULL, "flip_at's snap: 0 = 0.5 (the nearest vblank); -1 = never early" },
    { "show_cursor", YV_BOOL, YV_OFF(show_cursor), NULL, "keep the cursor over the window" },
    { "min_tier", YV_I32, YV_OFF(min_tier), NULL, "flag records whose tier is worse; 0 = off" },
    { "onset_offset", YV_SEC_SIGNED, YV_OFF(onset_offset_ns), NULL, "seconds added to every onset (photodiode)" },
    { "sim_period", YV_SEC, YV_OFF(sim_period_ns), NULL, "the sim backend's period; 0 = 1/60 s" },
    { "depth", YV_I32, YV_OFF(depth), NULL, "0 = from the path; 1 to 8 pins it" },
    { "depth_learn", YV_BOOL, YV_OFF(depth_learn), NULL, "misses raise the depth (opt-in)" },
    { "settle", YV_ENUM, YV_OFF(settle), yv_settles, "auto, strict, warn or off" },
    { "settle_flips", YV_I32, YV_OFF(settle_flips), NULL, "clean flips in a row; 0 = 6" },
    { "settle_min", YV_SEC_SIGNED, YV_OFF(settle_min_ns), NULL, "seconds from open; 0 = default, < 0 = none" },
    { "settle_max", YV_SEC, YV_OFF(settle_max_ns), NULL, "the cap in seconds; 0 = default" },
    { "foreground", YV_ENUM, YV_OFF(foreground), yv_foregrounds, "auto, honor or force" },
    { "window_thread", YV_ENUM, YV_OFF(window_thread), yv_window_threads, "auto, on or off" },
    { "panic", YV_BOOL, YV_OFF(panic), NULL, "arm the panic watchdog (fullscreen)" },
    { "panic_presses", YV_I32, YV_OFF(panic_presses), NULL, "presses that panic; 0 = 3" },
    { "panic_window", YV_SEC, YV_OFF(panic_window_ns), NULL, "within this time; 0 = 2 s" },
    { "panic_grace", YV_SEC, YV_OFF(panic_grace_ns), NULL, "no panic this long after an abort report; 0 = 10 s" },
    { "keep_os_gamma", YV_BOOL, YV_OFF(keep_os_gamma), NULL, "fullscreen: leave the OS gamma ramp alone" },
    { "key_dedup", YV_SEC_SIGNED, YV_OFF(key_dedup_ns), NULL, "a key's second report within this is dropped; 0 = 50 ms, < 0 = off" },
    { "gamepads", YV_BOOL, YV_OFF(gamepads), NULL, "open every gamepad" },
    { "patch", YV_BOOL, YV_OFF(patch.on), NULL, "the photodiode patch" },
    { "patch_size", YV_I32, YV_OFF(patch.size), NULL, "the patch's side in pixels; 0 = 32" },
    { "patch_corner", YV_ENUM, YV_OFF(patch.corner), yv_corners, "top_left, top_right, bottom_left or bottom_right" },
    { NULL, 0, 0, NULL, NULL },
};
#undef YV_OFF

static const yv_opt* yv_find_opt(const char* name) {
    int i;
    for (i = 0; yv_opts[i].name; i++) if (strcmp(yv_opts[i].name, name) == 0) return &yv_opts[i];
    return NULL;
}

static int yv_lower_eq(const char* a, const char* b) {
    for (; *a && *b; a++, b++) {
        char c = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
        if (c != *b) return 0;
    }
    return *a == 0 && *b == 0;
}

static int yv_name_value(const yv_name* t, const char* s, int* out) {
    int i;
    for (i = 0; t[i].name; i++) if (yv_lower_eq(s, t[i].name)) { *out = t[i].value; return 0; }
    return -1;
}

static const char* yv_value_name(const yv_name* t, int v) {
    int i;
    for (i = 0; t[i].name; i++) if (t[i].value == v) return t[i].name;
    return "?";
}

/* A number for a scalar option; 0, or -1 with the reason in err. */
static int yv_opt_num(yscr_desc* d, const yv_opt* o, double v, char* err, size_t cap) {
    char* p = (char*)d + o->off;
    int64_t ns;
    switch (o->type) {
    case YV_BOOL: *(bool*)p = v != 0.0; return 0;
    case YV_I32:
    case YV_U32:
        if (!(v == floor(v)) || v < (o->type == YV_U32 ? 0.0 : -2147483648.0) || v > 4294967295.0 ||
            (o->type == YV_I32 && v > 2147483647.0)) {
            snprintf(err, cap, "%s: an integer", o->name);
            return -1;
        }
        if (o->type == YV_I32) *(int32_t*)p = (int32_t)v;
        else *(uint32_t*)p = (uint32_t)v;
        return 0;
    case YV_F64:
        if (!(v == v)) { snprintf(err, cap, "%s: a number, not NaN", o->name); return -1; }
        *(double*)p = v;
        return 0;
    case YV_SEC:
    case YV_SEC_SIGNED:
        if (yv_s_to_ns(v, o->type == YV_SEC_SIGNED, &ns) < 0) {
            snprintf(err, cap, "%s: seconds%s (NaN and infinity are refused)", o->name,
                     o->type == YV_SEC ? ", >= 0" : "");
            return -1;
        }
        *(int64_t*)p = ns;
        return 0;
    default:
        snprintf(err, cap, "%s: not a number option", o->name);
        return -1;
    }
}

static int yv_opt_enum(yscr_desc* d, const yv_opt* o, const char* s, char* err, size_t cap) {
    int v, i;
    size_t len = 0;
    char list[160];
    if (o->type != YV_ENUM) { snprintf(err, cap, "%s: not a name option", o->name); return -1; }
    if (yv_name_value(o->names, s, &v) == 0) { *(int32_t*)((char*)d + o->off) = v; return 0; }
    list[0] = 0;
    for (i = 0; o->names[i].name && len + 1 < sizeof list; i++)
        len += (size_t)snprintf(list + len, sizeof list - len, "%s%s", i ? ", " : "", o->names[i].name);
    snprintf(err, cap, "%s: '%s' is not one of %s", o->name, s, list);
    return -1;
}

/* ---- the screen ------------------------------------------------------------- */

struct yv_gfx;

typedef struct yv_screen {
    yscr_screen  s;
    yscr_frame   f;            /* the last begin()'s frame                */
    int          open;
    int          in_frame;     /* between begin() and flip()              */
    yscr_record* rec;          /* YV_RECORDS, from on_flip                */
    uint32_t     rec_head, rec_n;
    uint64_t     rec_lost;     /* records overwritten before records()    */
    uint64_t     rec_total;
    yin_event    ev[YV_EVENTS];
    uint32_t     ev_head, ev_n;
    uint64_t     ev_lost;      /* events overwritten before poll()        */
    struct yv_gfx* gfx;        /* the open gfx: one per screen (its output
                                * stage writes every pixel of the frame)   */
    struct yv_gfx* gstart;     /* a gfx started in on_context (the open
                                * option gfx), until a host opens it      */
    int            frame_row;  /* MEX: begin gives the frame as a row (the
                                * open option frame), not a struct        */
} yv_screen;

/* ysp_vis_video.h: the movies on a gfx get each flip record (their FRAME
 * records), and close before the gfx does. */
static void yv_movies_flip(struct yv_gfx* g, const yscr_record* r);
static void yv_movies_close(struct yv_gfx* g);

static void yv_on_flip(void* ctx, const yscr_record* r, const yscr_trigger_result* trig, int n_trig) {
    yv_screen* v = (yv_screen*)ctx;
    uint32_t i;
    (void)trig;
    (void)n_trig;
    if (v->gfx) yv_movies_flip(v->gfx, r);
    if (!v->rec) return;
    i = (v->rec_head + v->rec_n) % YV_RECORDS;
    v->rec[i] = *r;
    v->rec_total++;
    if (v->rec_n < YV_RECORDS) v->rec_n++;
    else { v->rec_head = (v->rec_head + 1) % YV_RECORDS; v->rec_lost++; }
}

/* The records completed since the last call, oldest first; at most cap. */
static int yv_records(yv_screen* v, yscr_record* out, int cap) {
    int n = 0;
    while (v->rec_n > 0 && n < cap) {
        out[n++] = v->rec[v->rec_head];
        v->rec_head = (v->rec_head + 1) % YV_RECORDS;
        v->rec_n--;
    }
    return n;
}

/* SDL's queue to the event ring: the input events (keys, mouse, touch, pen,
 * gamepads) as ysp/input.h events with their own times. Done at every
 * begin(), so a frame loop that never polls does not fill SDL's queue;
 * the times are the events', not the drain's. */
static void yv_drain(yv_screen* v) {
    SDL_Event ev;
    yin_event e;
    int64_t t;
    while (yscr_poll(&v->s, &ev, &t)) {
        if (!yscr_event_input(&v->s, &ev, &e)) continue;
        v->ev[(v->ev_head + v->ev_n) % YV_EVENTS] = e;
        if (v->ev_n < YV_EVENTS) v->ev_n++;
        else { v->ev_head = (v->ev_head + 1) % YV_EVENTS; v->ev_lost++; }
    }
}

static int yv_events(yv_screen* v, yin_event* out, int cap) {
    int n = 0;
    while (v->ev_n > 0 && n < cap) {
        out[n++] = v->ev[v->ev_head];
        v->ev_head = (v->ev_head + 1) % YV_EVENTS;
        v->ev_n--;
    }
    return n;
}

static void yv_gfx_on_context(void* ctx, yscr_screen* s);
static void yv_gstart_drop(yv_screen* v);

/* Opens the screen with the module's directory as angle_dir. true, or
 * false with yscr_error(). The caller has zeroed v, and set v->gstart when
 * the gfx's programs are to compile while the screen settles (OPEN START
 * in ysp/gfx.h). */
static bool yv_open(yv_screen* v, yscr_desc* d) {
    v->rec = (yscr_record*)calloc(YV_RECORDS, sizeof(yscr_record));
    if (!v->rec) return false;
    d->angle_dir = yv_module_dir();
    if (v->gstart) {
        d->on_context = yv_gfx_on_context;
        d->on_context_ctx = v->gstart;
    }
    if (!yscr_open(&v->s, d)) return false;
    yscr_on_flip(&v->s, yv_on_flip, v);
    v->open = 1;
    return true;
}

static void yv_gfx_close(struct yv_gfx* g);

/* Closes the gfx on it first: its GL objects go before the context. */
static void yv_close(yv_screen* v) {
    if (v->gfx) yv_gfx_close(v->gfx);
    yv_gstart_drop(v);
    if (v->open) yscr_close(&v->s);
    v->open = 0;
    v->in_frame = 0;
}

static void yv_free(yv_screen* v) {
    if (!v) return;
    yv_close(v);
    free(v->rec);
    free(v);
}

static int yv_begin(yv_screen* v) {
    int rc;
    if (v->in_frame) return YSCR_ERR_ORDER;
    rc = yscr_begin(&v->s, &v->f);
    if (rc == YSCR_OK) v->in_frame = 1;
    yv_drain(v);
    return rc;
}

static int yv_flip_at(yv_screen* v, int64_t t_ns, int at) {
    int rc = at ? yscr_flip_at(&v->s, t_ns, NULL) : yscr_flip(&v->s);
    v->in_frame = 0;
    return rc;
}

/* ---- window calls (WINDOW THREAD): the host never calls SDL itself ---------- */

typedef struct yv_wcall { yv_screen* v; const char* text; int on; bool ok; } yv_wcall;

static void yv_w_title(void* c) {
    yv_wcall* w = (yv_wcall*)c;
    w->ok = SDL_SetWindowTitle(yscr_window(&w->v->s), w->text);
}
static void yv_w_cursor(void* c) {
    yv_wcall* w = (yv_wcall*)c;
    w->ok = w->on ? SDL_ShowCursor() : SDL_HideCursor();
}
static void yv_w_grab(void* c) {
    yv_wcall* w = (yv_wcall*)c;
    w->ok = SDL_SetWindowMouseGrab(yscr_window(&w->v->s), w->on != 0);
}

/* 0; YSCR_ERR_NOT_IMPLEMENTED without a window (SIM); the window call's
 * error; YSCR_ERR_LOST when SDL refused. */
static int yv_window(yv_screen* v, void (*fn)(void*), const char* text, int on) {
    yv_wcall w;
    int rc;
    if (!yscr_window(&v->s)) return YSCR_ERR_NOT_IMPLEMENTED;
    w.v = v;
    w.text = text;
    w.on = on;
    w.ok = false;
    rc = yscr_window_call(&v->s, fn, &w);
    if (rc < 0) return rc;
    return w.ok ? 0 : YSCR_ERR_LOST;
}

/* ---- gfx: ysp/gfx.h's stimuli, lists, groups, bindings (ysp_vis_gfx.h) ------ */

#include "ysp_vis_gfx.h"
#include "ysp_vis_text.h"   /* text: fonts, layout, glyph and word boxes */

#include "ysp_vis_video.h"   /* movies: ysp.video, ysp_video */

#endif /* YSP_VIS_CORE_H */
