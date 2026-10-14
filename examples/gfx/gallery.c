/* gfx_gallery.c - a visual tour of ysp/gfx.h: one labeled tile per feature.
 *
 * Eight pages of up to twelve tiles in a 1200 x 760 window: gratings,
 * gabors, noise and dots; shapes, strokes and joins; dashes, trim,
 * compounds and effects; paint, color spaces and blend modes; images,
 * masks, groups, render targets and glyph runs; v0.4's instances (with a
 * hit test under the mouse), a planar NV12 picture, premultiplied alpha,
 * shared glyph buffers and the program cache; v0.6's curve runs (a word
 * whose letters enter one after another, artwork layers with a hit test)
 * and the blur pass (a glow); v0.7 to v0.10: uniform against Gaussian
 * noise with their histograms, simplex fBm by octaves, a turning word on
 * the exact-area and the rays programs, the output stage's three roundings
 * of a shallow ramp, and an additive layer. Each tile is one function
 * that makes its stimuli at setup and draws them in the frame, so a tile
 * is also the shortest code for its feature. Some tiles move through
 * ysp/timeline.h bindings: a rotation and a dash offset on repeating keyed
 * tracks; trim, contrast, a smooth merge, a group's scale and a tilt on
 * yoyo tweens. All of them run on one time base that restarts with each
 * page.
 *
 * The labels are glyph runs. ysp/gfx.h has no text layout and no atlas
 * tool, and an MSDF atlas needs msdf-atlas-gen, so a 5 x 7 pixel font is
 * embedded below and made into a distance atlas at setup with
 * ygfx_sdf_from_mask(). The page's tile list is also printed.
 *
 * Colors are linear light through a nominal calibration (sRGB's primaries,
 * gamma 2.2) with synthetic primary spectra, because the OKLAB and DKL
 * paint spaces and the cone and DKL directions need them. It is NOT a
 * measurement of this display. Low to moderate contrast, slow motion and
 * no flicker, because the display may be somebody's working screen; key
 * repeats do not turn pages for the same reason.
 *
 * Usage: gfx_gallery [--sim] [--page N] [--frames N] [--composition]
 *                    [--topmost] [--shots PREFIX] [--cache DIR] [--no-cache]
 *   Right, Down, Space, Page Down: next page; Left, Up, Page Up: previous;
 *   1 to 8: that page; a tap on the right half: next, on the left half:
 *   previous; Shift+Esc or closing the window: quit.
 *   --sim          the simulated display and the null backend, for CI:
 *                  draws each page once and exits
 *   --page N       start on page N (1 to 8)
 *   --frames N     stop after N frames
 *   --composition  the composition swapchain (Windows)
 *   --topmost      keep the window on top and take the foreground (Windows)
 *   --shots PREFIX show each page for 120 frames, read the output back, write
 *                  PREFIX-pN.ppm, then quit
 *   --cache DIR    keep compiled programs in DIR (ygfx_file_cache_init:
 *                  it writes files there); the default is the per-user
 *                  folder of ygfx_default_cache_dir(), when there is one
 *   --no-cache     compile every program; read and write no cache file
 * Exit code: 0; 1 when the screen or the gfx did not open or a draw was
 * refused; 2 for a bad argument.
 * On Windows set YSCR_ANGLE_DIR to ANGLE's directory.
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#define YSP_GFX_IMPLEMENTATION
#include "ysp/gfx.h"
#define YSP_TIMELINE_IMPLEMENTATION
#include "ysp/timeline.h"
#define YSP_OUTLINE_IMPLEMENTATION
#include "ysp/outline.h"   /* page 7: the curve sets */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
/* The web page shows the canvas when the pages are made and primed. */
EM_JS(void, gallery_ready, (void), { if (Module['onYspReady']) Module['onYspReady'](); })
#else
static void gallery_ready(void) {}
#endif

#if defined(_WIN32)
/* Windows gives the foreground to a program started from the background
 * after it sends one input event; a zero-size mouse move is that event. */
static void take_foreground(SDL_Window* w) {
    HWND h = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(w), SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
    INPUT in;
    memset(&in, 0, sizeof in);
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(1, &in, sizeof in);
    SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetForegroundWindow(h);
}
#endif

#define WIN_W 1200
#define WIN_H 760
#define PI_F 3.14159265f
#define S_NS YRT_S(1)
#define SHOT_FRAMES 120

static yscr_screen scr;
static ygfx_gfx gfx;
static ycol_cal cal;

/* yscr_open() calls this once its GL context exists, before it settles
 * the display (0.5 to 2.5 s). The gfx submits its programs and returns; the
 * driver compiles them on its own threads while the display settles, and
 * ygfx_open() waits for the rest (ysp/screen.h CONTEXT HOOK, ysp/gfx.h
 * OPEN START). A failure here is reported by ygfx_open(). */
static void start_gfx(void* ctx, yscr_screen* s) {
    ygfx_desc* gd = (ygfx_desc*)ctx;
    gd->screen = s;
    ygfx_open_start(&gfx, gd);
}
static ytl_timeline tl;
static ytl_event tl_storage[4];   /* tracks and tweens only: no events */

/* Linear device RGB. The background is near mid-gray on the display
 * (0.18 is about 0.46 of the code range through gamma 2.2). */
static const float BG[3]     = { 0.18f, 0.18f, 0.18f };
static const float PANEL[3]  = { 0.22f, 0.22f, 0.22f };
static const float INK[3]    = { 0.55f, 0.55f, 0.55f };
static const float GRAY[3]   = { 0.40f, 0.40f, 0.40f };
static const float LIGHT[3]  = { 0.62f, 0.62f, 0.62f };
static const float DARK[3]   = { 0.05f, 0.05f, 0.05f };
static const float RED[3]    = { 0.55f, 0.10f, 0.07f };
static const float GREEN[3]  = { 0.10f, 0.42f, 0.12f };
static const float BLUE[3]   = { 0.08f, 0.16f, 0.60f };
static const float AMBER[3]  = { 0.60f, 0.32f, 0.04f };
static const float TEAL[3]   = { 0.05f, 0.38f, 0.40f };
static const float VIOLET[3] = { 0.32f, 0.10f, 0.50f };

/* --- timeline channels and bindings ---------------------------------------- */

enum { CH_ROT, CH_MARCH, CH_TRIM, CH_CONTRAST, CH_MERGE, CH_TILT, CH_PULSE, CH_SIGMA,
       CH_LETTER_Y, CH_LETTER_G = CH_LETTER_Y + 7, CH_NOISE_Z = CH_LETTER_G + 7, CH_QUARTER, N_CH };
enum { PAGE_BASE = 1 };

static ygfx_bind binds[64];
static int n_binds;
static int64_t page_t0;

static void bind_stim(ygfx_stim* s, int param, int ch) {
    memset(&binds[n_binds], 0, sizeof binds[0]);
    binds[n_binds].stim = s; binds[n_binds].param = (uint16_t)param; binds[n_binds].channel = (uint16_t)ch;
    n_binds++;
}
static void bind_group(ygfx_group* g, int param, int ch) {
    memset(&binds[n_binds], 0, sizeof binds[0]);
    binds[n_binds].group = g; binds[n_binds].param = (uint16_t)param; binds[n_binds].channel = (uint16_t)ch;
    n_binds++;
}
/* Any float: a primitive's x, a paint's start angle. */
static void bind_field(float* field, int ch) {
    memset(&binds[n_binds], 0, sizeof binds[0]);
    binds[n_binds].field = field; binds[n_binds].channel = (uint16_t)ch;
    n_binds++;
}

/* A tween that runs between two values forever, from base time 0, so that
 * re-anchoring the page base at 0 replays it. */
static int yoyo(int ch, float from, float to, double seconds) {
    ytl_tween_desc d;
    memset(&d, 0, sizeof d);
    d.from = from; d.from_set = true; d.to = to;
    d.duration_ns = ytl_ns(seconds);
    d.start_ns = 0; d.start_set = true;
    d.ease = YTL_EASE_COSINE; d.cycles = YTL_FOREVER; d.yoyo = true;
    return ytl_tween(&tl, ch, PAGE_BASE, &d);
}

/* A linear ramp from 0 to `to` that repeats every period: the wrap is a
 * whole turn or a whole dash period, so it does not show. */
static const ytl_key rot_keys[2]   = { { 0, 0.0f, YTL_EASE_LINEAR, 0, 0 }, { 10 * S_NS, 360.0f, YTL_EASE_LINEAR, 0, 0 } };
static const ytl_key march_keys[2] = { { 0, 0.0f, YTL_EASE_LINEAR, 0, 0 }, { S_NS, 22.0f, YTL_EASE_LINEAR, 0, 0 } };
/* Page 8: simplex z at 0.1 cells per second (r 0.92 a second; it wraps
 * after 5 minutes), and quarter turns that stop for 2.5 s at each quarter,
 * where a run takes the exact-area program again. */
static const ytl_key z_keys[2] = { { 0, 0.0f, YTL_EASE_LINEAR, 0, 0 }, { 300 * S_NS, 30.0f, YTL_EASE_LINEAR, 0, 0 } };
static const ytl_key quarter_keys[9] = {
    { 0, 0.0f, YTL_EASE_LINEAR, 0, 0 },                 { 2500000000, 0.0f, YTL_EASE_COSINE, 0, 0 },
    { 5 * S_NS, 90.0f, YTL_EASE_LINEAR, 0, 0 },         { 7500000000, 90.0f, YTL_EASE_COSINE, 0, 0 },
    { 10 * S_NS, 180.0f, YTL_EASE_LINEAR, 0, 0 },       { 12500000000, 180.0f, YTL_EASE_COSINE, 0, 0 },
    { 15 * S_NS, 270.0f, YTL_EASE_LINEAR, 0, 0 },       { 17500000000, 270.0f, YTL_EASE_COSINE, 0, 0 },
    { 20 * S_NS, 360.0f, YTL_EASE_LINEAR, 0, 0 } };

static int stagger_tracks(void);   /* page 7 */

static int timeline_setup(void) {
    static const float initial[N_CH] = { 0, 0, 1, 0.3f, 60, 0, 1 };
    ytl_desc td;
    ytl_track tr;
    int rc = 0;
    memset(&td, 0, sizeof td);
    td.events = tl_storage; td.event_capacity = 4; td.n_channels = N_CH; td.initial = initial;
    if (!ytl_open(&tl, &td)) { fprintf(stderr, "gfx_gallery: %s\n", ytl_error(&tl)); return -1; }
    memset(&tr, 0, sizeof tr);
    tr.keys = rot_keys; tr.n_keys = 2; tr.period_ns = 10 * S_NS;          /* 36 degrees per second */
    rc |= ytl_set_track(&tl, CH_ROT, PAGE_BASE, &tr);
    tr.keys = march_keys; tr.period_ns = S_NS;                            /* 22 px per second */
    rc |= ytl_set_track(&tl, CH_MARCH, PAGE_BASE, &tr);
    tr.keys = z_keys; tr.period_ns = 300 * S_NS;
    rc |= ytl_set_track(&tl, CH_NOISE_Z, PAGE_BASE, &tr);
    tr.keys = quarter_keys; tr.n_keys = 9; tr.period_ns = 20 * S_NS;     /* 360 = 0: the wrap does not show */
    rc |= ytl_set_track(&tl, CH_QUARTER, PAGE_BASE, &tr);
    rc |= yoyo(CH_TRIM, 0.03f, 1.0f, 1.5);       /* never 0: trim 0, 0 means the whole */
    rc |= yoyo(CH_CONTRAST, 0.05f, 0.4f, 2.0);
    rc |= yoyo(CH_MERGE, 95.0f, 10.0f, 2.5);
    rc |= yoyo(CH_TILT, -12.0f, 12.0f, 3.0);
    rc |= yoyo(CH_PULSE, 0.8f, 1.15f, 2.0);
    rc |= yoyo(CH_SIGMA, 1.5f, 6.0f, 2.5);
    rc |= stagger_tracks();
    if (rc < 0) { fprintf(stderr, "gfx_gallery: a track or tween was refused\n"); return -1; }
    return 0;
}

/* --- drawing helpers ---------------------------------------------------------- */

static const char* cur_tile = "";
static const char* reported_tile;
static long refused;

/* Every draw is checked: a refused draw names its tile once, and the exit
 * code says so, which makes the --sim run in CI a check of every tile's
 * desc against the header's rules. */
static void put_n(const ygfx_stim* s, int n) {
    int rc = ygfx_draw_n(&gfx, s, n);
    if (rc < 0) {
        refused++;
        if (reported_tile != cur_tile) {
            fprintf(stderr, "gfx_gallery: tile \"%s\": %s (%s)\n", cur_tile, ygfx_strerror(rc), ygfx_error(&gfx));
            reported_tile = cur_tile;
        }
    }
}
static void put(const ygfx_stim* s) { put_n(s, 1); }

static void rgb(float* dst, const float* src) { dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; }

/* A shape desc at a screen point with the edge every tile uses: a raised
 * cosine 1.5 px wide, which keeps edges smooth without blurring them. */
static ygfx_shape_desc shape_desc(ygfx_shape_kind k, float x, float y, float w, float h, const float* color) {
    ygfx_shape_desc d;
    memset(&d, 0, sizeof d);
    d.place = YGFX_TOP_LEFT;
    d.shape = k; d.x = x; d.y = y; d.w = w; d.h = h;
    d.edge = YGFX_EDGE_COSINE; d.edge_width = 1.5f;
    rgb(d.color, color);
    return d;
}
static ygfx_stim shape(ygfx_shape_kind k, float x, float y, float w, float h, const float* color) {
    ygfx_shape_desc d = shape_desc(k, x, y, w, h, color);
    return ygfx_shape(&d);
}

static double page_seconds(const yscr_frame* f) { return (double)(f->onset - page_t0) * 1e-9; }

static ygfx_paint paint2(int kind, int space, float x0, float y0, float x1, float y1, const float* c0, const float* c1) {
    ygfx_paint p;
    memset(&p, 0, sizeof p);
    p.kind = (uint8_t)kind; p.space = (uint8_t)space; p.n = 2;
    p.x0 = x0; p.y0 = y0; p.x1 = x1; p.y1 = y1;
    p.stops[0].t = 0; rgb(p.stops[0].color, c0);
    p.stops[1].t = 1; rgb(p.stops[1].color, c1);
    return p;
}

static ygfx_prim prim(int kind, int op, float x, float y, float w, float h) {
    ygfx_prim p;
    memset(&p, 0, sizeof p);
    p.shape = (uint8_t)kind; p.op = (uint8_t)op; p.x = x; p.y = y; p.w = w; p.h = h;
    return p;
}

static ygfx_stim compound(const ygfx_prim* pr, int n, float x, float y, float w, float h, const float* color) {
    ygfx_compound_desc d;
    memset(&d, 0, sizeof d);
    d.place = YGFX_TOP_LEFT; d.prims = pr; d.n = n; d.x = x; d.y = y; d.w = w; d.h = h;
    d.edge = YGFX_EDGE_COSINE; d.edge_width = 1.5f;
    rgb(d.color, color);
    return ygfx_compound(&d);
}

/* --- the font: 5 x 7 pixels, ' ' to '_', rows top first, bit 4 the left
 *     column. Drawn for this example; public domain like the rest. ---------- */

#define FONT_FIRST ' '
#define FONT_N     64
static const uint8_t font5x7[FONT_N][7] = {
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }, /* space */
    { 0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04 }, /* ! */
    { 0x0A, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x00 }, /* " */
    { 0x0A, 0x0A, 0x1F, 0x0A, 0x1F, 0x0A, 0x0A }, /* # */
    { 0x04, 0x0F, 0x14, 0x0E, 0x05, 0x1E, 0x04 }, /* $ */
    { 0x18, 0x19, 0x02, 0x04, 0x08, 0x13, 0x03 }, /* % */
    { 0x0C, 0x12, 0x14, 0x08, 0x15, 0x12, 0x0D }, /* & */
    { 0x0C, 0x04, 0x08, 0x00, 0x00, 0x00, 0x00 }, /* ' */
    { 0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02 }, /* ( */
    { 0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08 }, /* ) */
    { 0x00, 0x04, 0x15, 0x0E, 0x15, 0x04, 0x00 }, /* * */
    { 0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00 }, /* + */
    { 0x00, 0x00, 0x00, 0x00, 0x0C, 0x04, 0x08 }, /* , */
    { 0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00 }, /* - */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C }, /* . */
    { 0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x00 }, /* / */
    { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E }, /* 0 */
    { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E }, /* 1 */
    { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F }, /* 2 */
    { 0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E }, /* 3 */
    { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 }, /* 4 */
    { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E }, /* 5 */
    { 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E }, /* 6 */
    { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 }, /* 7 */
    { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E }, /* 8 */
    { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C }, /* 9 */
    { 0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00 }, /* : */
    { 0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x04, 0x08 }, /* ; */
    { 0x02, 0x04, 0x08, 0x10, 0x08, 0x04, 0x02 }, /* < */
    { 0x00, 0x00, 0x1F, 0x00, 0x1F, 0x00, 0x00 }, /* = */
    { 0x08, 0x04, 0x02, 0x01, 0x02, 0x04, 0x08 }, /* > */
    { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04 }, /* ? */
    { 0x0E, 0x11, 0x17, 0x15, 0x17, 0x10, 0x0E }, /* @ */
    { 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 }, /* A */
    { 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E }, /* B */
    { 0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E }, /* C */
    { 0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C }, /* D */
    { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F }, /* E */
    { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10 }, /* F */
    { 0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F }, /* G */
    { 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 }, /* H */
    { 0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E }, /* I */
    { 0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C }, /* J */
    { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 }, /* K */
    { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F }, /* L */
    { 0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11 }, /* M */
    { 0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11 }, /* N */
    { 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E }, /* O */
    { 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10 }, /* P */
    { 0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D }, /* Q */
    { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 }, /* R */
    { 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E }, /* S */
    { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 }, /* T */
    { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E }, /* U */
    { 0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04 }, /* V */
    { 0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A }, /* W */
    { 0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11 }, /* X */
    { 0x11, 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04 }, /* Y */
    { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F }, /* Z */
    { 0x0E, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0E }, /* [ */
    { 0x00, 0x10, 0x08, 0x04, 0x02, 0x01, 0x00 }, /* \ */
    { 0x0E, 0x02, 0x02, 0x02, 0x02, 0x02, 0x0E }, /* ] */
    { 0x04, 0x0A, 0x11, 0x00, 0x00, 0x00, 0x00 }, /* ^ */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F }, /* _ */
};

/* The atlas: each glyph's ink at FONT_S texels per font pixel with
 * FONT_PAD texels around it, the distance an edge, a stroke or an offset
 * may reach outside the ink. */
#define FONT_S     8
#define FONT_PAD   12
#define CELL_W     (5 * FONT_S + 2 * FONT_PAD)
#define CELL_H     (7 * FONT_S + 2 * FONT_PAD)
#define ATLAS_COLS 8
#define ATLAS_W    (ATLAS_COLS * CELL_W)
#define ATLAS_H    ((FONT_N + ATLAS_COLS - 1) / ATLAS_COLS * CELL_H)

static uint8_t atlas_mask[ATLAS_W * ATLAS_H];
static float atlas_sdf[ATLAS_W * ATLAS_H];
static ygfx_tex atlas;

static int font_setup(void) {
    ygfx_texture_desc td;
    int g, r, c, i, j;
    for (g = 0; g < FONT_N; g++)
        for (r = 0; r < 7; r++)
            for (c = 0; c < 5; c++) {
                int x0 = g % ATLAS_COLS * CELL_W + FONT_PAD + c * FONT_S, y0 = g / ATLAS_COLS * CELL_H + FONT_PAD + r * FONT_S;
                if (!(font5x7[g][r] & (0x10 >> c))) continue;
                for (j = 0; j < FONT_S; j++)
                    for (i = 0; i < FONT_S; i++) atlas_mask[(y0 + j) * ATLAS_W + x0 + i] = 255;
            }
    /* pad 0: the cells' own padding is the atlas's */
    if (ygfx_sdf_from_mask(atlas_sdf, atlas_mask, ATLAS_W, ATLAS_H, 0) < 0) return -1;
    memset(&td, 0, sizeof td);
    td.w = ATLAS_W; td.h = ATLAS_H; td.format = YGFX_R16F; td.data = atlas_sdf; td.sdf = YGFX_SDF_DIST;
    td.sdf_range = 2 * FONT_PAD;   /* the range rule: distances are good to the cells' padding */
    atlas = ygfx_texture(&gfx, &td);
    return atlas.id ? 0 : -1;
}

/* Glyph records for text with its first character's ink at x, y (px), px
 * screen pixels per font pixel; '\n' starts a line. Layout is the
 * caller's: a fixed advance of 6 font pixels, no kerning. Returns the
 * count written. */
static int layout(ygfx_glyph* out, int cap, const char* s, float x, float y, float px) {
    float scale = px / FONT_S, pen = x;
    int n = 0;
    for (; *s; s++) {
        int c = (unsigned char)*s;
        if (c == '\n') { pen = x; y += 9 * px; continue; }
        if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
        if (c > FONT_FIRST && c < FONT_FIRST + FONT_N && n < cap) {
            int g = c - FONT_FIRST;
            memset(&out[n], 0, sizeof out[n]);
            out[n].x = pen - FONT_PAD * scale; out[n].y = y - FONT_PAD * scale;
            out[n].sx = (float)(g % ATLAS_COLS * CELL_W); out[n].sy = (float)(g / ATLAS_COLS * CELL_H);
            out[n].sw = (float)CELL_W; out[n].sh = (float)CELL_H;
            n++;
        }
        pen += 6 * px;
    }
    return n;
}

/* A run whose box w x h has its anchor at the screen point x, y; one
 * instanced draw. */
static ygfx_stim text_run(ygfx_buf b, int count, float px, ygfx_align anchor, float x, float y, float w, float h,
                            const float* color) {
    ygfx_glyphs_desc d;
    memset(&d, 0, sizeof d);
    d.place = YGFX_TOP_LEFT; d.anchor = anchor;
    d.atlas = atlas; d.buf = b; d.count = (uint32_t)count; d.scale = px / FONT_S;
    d.x = x; d.y = y; d.w = w; d.h = h;
    /* Glyph edges on whole pixels with a 1 px cosine edge are crisp: every
     * pixel center is half a pixel inside or outside. */
    d.edge = YGFX_EDGE_COSINE; d.edge_width = 1.0f;
    rgb(d.color, color);
    return ygfx_glyphs(&d);
}

/* =========================================================================== *
 * The tiles. Each takes the center of its tile's drawing area in screen px:
 * with f NULL it makes its stimuli (setup), else it draws them.
 * =========================================================================== */

typedef void (*tile_fn)(float x, float y, const yscr_frame* f);

/* --- page 1: gratings, gabors, noise, dots ------------------------------------ */

static void t_gabor_drift(float x, float y, const yscr_frame* f) {
    static ygfx_stim g;
    if (!f) {
        ygfx_gabor_desc d;
        memset(&d, 0, sizeof d);
        d.place = YGFX_TOP_LEFT; d.x = x; d.y = y;
        d.sigma = 22; d.sf = 1 / 24.0f; d.contrast = 0.3f;
        g = ygfx_gabor(&d);
        return;
    }
    g.phase = (float)fmod(page_seconds(f), 1.0);   /* from the predicted onset */
    put(&g);
}

static void t_gabor_tween(float x, float y, const yscr_frame* f) {
    static ygfx_stim g;
    if (!f) {
        ygfx_gabor_desc d;
        memset(&d, 0, sizeof d);
        d.place = YGFX_TOP_LEFT; d.x = x; d.y = y;
        d.sigma = 22; d.sf = 1 / 20.0f; d.ori = 45;
        g = ygfx_gabor(&d);
        bind_stim(&g, YGFX_P_CONTRAST, CH_CONTRAST);
        return;
    }
    put(&g);
}

static void t_gabor_track(float x, float y, const yscr_frame* f) {
    static ygfx_stim g;
    if (!f) {
        ygfx_gabor_desc d;
        memset(&d, 0, sizeof d);
        d.place = YGFX_TOP_LEFT; d.x = x; d.y = y;
        d.sigma = 24; d.sf = 1 / 16.0f; d.contrast = 0.3f; d.aspect = 2;   /* half as long as wide */
        g = ygfx_gabor(&d);
        bind_stim(&g, YGFX_P_ORI, CH_ROT);
        return;
    }
    put(&g);
}

static void t_gabor_array(float x, float y, const yscr_frame* f) {
    static ygfx_stim g[16];
    int i;
    if (!f) {
        ygfx_gabor_desc d;
        memset(&d, 0, sizeof d);
        d.place = YGFX_TOP_LEFT; d.sigma = 6; d.sf = 1 / 9.0f; d.contrast = 0.35f;
        for (i = 0; i < 16; i++) {
            d.x = x + (float)(i % 4 * 46 - 69); d.y = y + (float)(i / 4 * 46 - 69);
            d.ori = (float)(i * 67 % 180);
            g[i] = ygfx_gabor(&d);
        }
        return;
    }
    put_n(g, 16);   /* one block each, batched: 16 per instanced draw */
}

static void t_grating_circle(float x, float y, const yscr_frame* f) {
    static ygfx_stim g;
    if (!f) {
        ygfx_grating_desc d;
        memset(&d, 0, sizeof d);
        d.place = YGFX_TOP_LEFT; d.x = x; d.y = y; d.w = 180;
        d.sf = 1 / 18.0f; d.ori = 30; d.contrast = 0.3f;
        d.aperture = YGFX_CIRCLE; d.edge = YGFX_EDGE_COSINE; d.edge_width = 20;
        g = ygfx_grating(&d);
        return;
    }
    put(&g);
}

static void t_grating_square(float x, float y, const yscr_frame* f) {
    static ygfx_stim g;
    if (!f) {
        ygfx_grating_desc d;
        memset(&d, 0, sizeof d);
        d.place = YGFX_TOP_LEFT; d.x = x; d.y = y; d.w = 210; d.h = 150;
        d.sf = 1 / 30.0f; d.contrast = 0.25f; d.square = 1;
        d.aperture = YGFX_RECT; d.edge = YGFX_EDGE_GAUSSIAN; d.edge_width = 3;
        g = ygfx_grating(&d);
        return;
    }
    put(&g);
}

static void t_plaid(float x, float y, const yscr_frame* f) {
    static ygfx_stim g[2];
    int i;
    if (!f) {
        ygfx_grating_desc d;
        memset(&d, 0, sizeof d);
        d.place = YGFX_TOP_LEFT; d.x = x; d.y = y; d.w = 180;
        d.sf = 1 / 20.0f; d.contrast = 0.15f;
        d.aperture = YGFX_CIRCLE; d.edge = YGFX_EDGE_COSINE; d.edge_width = 16;
        for (i = 0; i < 2; i++) { d.ori = (float)(45 + 90 * i); g[i] = ygfx_grating(&d); }
        return;
    }
    /* each component drifts at 0.5 Hz; the plaid moves straight down */
    g[0].phase = g[1].phase = (float)fmod(0.5 * page_seconds(f), 1.0);
    g[1].phase = -g[1].phase;
    put_n(g, 2);
}

static void t_user_radial(float x, float y, const yscr_frame* f) {
    static const char* body =
        "float ysp_main(vec2 p) {\n"
        "    return cos(2.0 * ysp_PI * (length(p) * ysp_param(0) - ysp_param(1)));\n"
        "}\n";
    static ygfx_stim s;
    if (!f) {
        ygfx_pipeline_desc pd;
        ygfx_user_desc d;
        float p[2] = { 1 / 16.0f, 0 };   /* cycles per px, phase */
        memset(&pd, 0, sizeof pd);
        pd.body = body; pd.mode = YGFX_MODULATION; pd.name = "radial";
        memset(&d, 0, sizeof d);
        d.pipe = ygfx_pipeline(&gfx, &pd);
        if (!d.pipe.id) { fprintf(stderr, "gfx_gallery: radial shader: %s\n", ygfx_error(&gfx)); refused++; }
        d.place = YGFX_TOP_LEFT; d.x = x; d.y = y; d.w = 180; d.contrast = 0.3f;
        d.aperture = YGFX_CIRCLE; d.edge = YGFX_EDGE_COSINE; d.edge_width = 24;
        d.p = p; d.n_p = 2;
        s = ygfx_user(&d);
        return;
    }
    s.p[1] = (float)fmod(0.5 * page_seconds(f), 1.0);   /* rings move outward at 0.5 Hz */
    put(&s);
}

static void t_noise(float x, float y, const yscr_frame* f) {
    static ygfx_stim s;
    if (!f) {
        ygfx_noise_desc d;
        memset(&d, 0, sizeof d);
        d.place = YGFX_TOP_LEFT; d.x = x; d.y = y; d.w = 180;
        d.check = 4; d.seed = 7; d.contrast = 0.3f;
        d.aperture = YGFX_CIRCLE; d.edge = YGFX_EDGE_COSINE; d.edge_width = 12;
        s = ygfx_noise(&d);
        return;
    }
    put(&s);   /* static: a new seed each frame would flicker */
}

static void t_noise_ring(float x, float y, const yscr_frame* f) {
    static ygfx_stim s;
    if (!f) {
        ygfx_noise_desc d;
        memset(&d, 0, sizeof d);
        d.place = YGFX_TOP_LEFT; d.x = x; d.y = y; d.w = 170;
        d.check = 3; d.seed = 11; d.dist = YGFX_BINARY; d.contrast = 0.25f;
        d.aperture = YGFX_CIRCLE; d.edge = YGFX_EDGE_COSINE; d.edge_width = 2;
        d.stroke = 30; d.stroke_align = YGFX_STROKE_INSIDE;
        s = ygfx_noise(&d);
        return;
    }
    put(&s);
}

/* n positions inside a circle of radius r, from a fixed LCG. */
static ygfx_buf dot_buffer(int n, float r, uint32_t seed) {
    static float xy[2 * 400];
    ygfx_buf b;
    int i = 0;
    while (i < n) {
        float u, v;
        seed = seed * 1664525u + 1013904223u; u = (float)(seed >> 8) / 8388608.0f - 1.0f;
        seed = seed * 1664525u + 1013904223u; v = (float)(seed >> 8) / 8388608.0f - 1.0f;
        if (u * u + v * v > 1.0f) continue;
        xy[2 * i] = u * r; xy[2 * i + 1] = v * r;
        i++;
    }
    b = ygfx_buffer(&gfx, (size_t)n * 8);
    ygfx_buffer_update(&gfx, b, 0, xy, (size_t)n * 8);
    return b;
}

static void t_dots(float x, float y, const yscr_frame* f) {
    static ygfx_stim s;
    if (!f) {
        ygfx_dots_desc d;
        memset(&d, 0, sizeof d);
        d.place = YGFX_TOP_LEFT; d.x = x; d.y = y;
        d.buf = dot_buffer(300, 86, 1); d.count = 300; d.dot_size = 5;
        d.edge = YGFX_EDGE_COSINE; d.edge_width = 1;
        rgb(d.color, LIGHT);
        d.aperture = YGFX_CIRCLE; d.w = 180;
        s = ygfx_dots(&d);
        bind_stim(&s, YGFX_P_ORI, CH_ROT);   /* the field turns, the dots go with it */
        return;
    }
    put(&s);
}

static void t_dots_ring(float x, float y, const yscr_frame* f) {
    static ygfx_stim s;
    if (!f) {
        ygfx_dots_desc d;
        memset(&d, 0, sizeof d);
        d.place = YGFX_TOP_LEFT; d.x = x; d.y = y;
        d.buf = dot_buffer(70, 110, 2); d.count = 70; d.dot_size = 16;
        d.edge = YGFX_EDGE_COSINE; d.edge_width = 1;
        rgb(d.color, AMBER);
        d.aperture = YGFX_RECT; d.w = 230; d.h = 150;   /* dots whose centers are outside are not drawn */
        d.stroke = 2.5f;
        s = ygfx_dots(&d);
        return;
    }
    put(&s);
}

/* --- page 2: shapes, strokes and joins ---------------------------------------- */

static void t_basic(float x, float y, const yscr_frame* f) {
    static ygfx_stim s[4];
    if (!f) {
        s[0] = shape(YGFX_CIRCLE, x - 60, y - 42, 68, 0, GRAY);
        s[1] = shape(YGFX_ANNULUS, x + 60, y - 42, 72, 0, TEAL);
        s[1].shape_p[0] = 20;                                   /* inner radius */
        s[2] = shape(YGFX_CROSS, x - 60, y + 45, 64, 64, AMBER);
        s[2].shape_p[0] = 14;                                   /* arm width */
        s[3] = shape(YGFX_LINE, x + 60, y + 45, 90, 0, LIGHT);
        s[3].shape_p[0] = 10; s[3].ori = -30;                   /* width; round caps */
        return;
    }
    put_n(s, 4);
}

static void t_rects(float x, float y, const yscr_frame* f) {
    static ygfx_stim s[3];
    if (!f) {
        s[0] = shape(YGFX_RECT, x - 65, y - 42, 100, 64, GRAY);
        s[1] = shape(YGFX_RECT, x + 65, y - 42, 100, 64, BLUE);
        s[1].shape_p[1] = 18;                                   /* corner radius */
        s[2] = shape(YGFX_RRECT, x, y + 48, 160, 70, VIOLET);
        s[2].shape_p[0] = 0; s[2].shape_p[1] = 16; s[2].shape_p[2] = 35; s[2].shape_p[3] = 6;  /* TL, TR, BR, BL */
        return;
    }
    put_n(s, 3);
}

static void t_ellipse_capsule(float x, float y, const yscr_frame* f) {
    static ygfx_stim s[2];
    if (!f) {
        ygfx_shape_desc d;
        s[0] = shape(YGFX_ELLIPSE, x, y - 42, 200, 70, TEAL);
        d = shape_desc(YGFX_CAPSULE, x, y + 50, 200, 0, AMBER);
        d.shape_p[0] = 26; d.shape_p[1] = 9;                    /* left and right end radii */
        s[1] = ygfx_shape(&d);
        return;
    }
    put_n(s, 2);
}

static void t_arc_pie(float x, float y, const yscr_frame* f) {
    static ygfx_stim s[2];
    if (!f) {
        s[0] = shape(YGFX_ARC, x - 68, y, 125, 0, LIGHT);
        s[0].shape_p[0] = 270; s[0].shape_p[1] = 16; s[0].shape_p[2] = 135;   /* sweep, thickness, start */
        s[1] = shape(YGFX_PIE, x + 68, y, 125, 0, AMBER);
        s[1].shape_p[0] = 300; s[1].shape_p[2] = 30;                           /* sweep, start */
        return;
    }
    put_n(s, 2);
}

static void t_ngon_star(float x, float y, const yscr_frame* f) {
    static ygfx_stim s[2];
    if (!f) {
        s[0] = shape(YGFX_NGON, x - 68, y, 120, 0, GREEN);
        s[0].shape_p[0] = 6; s[0].shape_p[1] = 10;              /* sides, rounding */
        s[1] = shape(YGFX_STAR, x + 68, y, 130, 0, AMBER);
        s[1].shape_p[0] = 5; s[1].shape_p[1] = 0.45f; s[1].shape_p[2] = 4;   /* points, inner / outer, rounding */
        return;
    }
    put_n(s, 2);
}

static void t_polygon(float x, float y, const yscr_frame* f) {
    static const float arrow[14] = { -60, -14, 6, -14, 6, -38, 60, 0, 6, 38, 6, 14, -60, 14 };
    static ygfx_stim s[2];
    if (!f) {
        ygfx_shape_desc d = shape_desc(YGFX_POLYGON, x, y - 46, 120, 76, GRAY);
        d.shape_p[0] = 7; d.vertices = arrow;
        s[0] = ygfx_shape(&d);
        d.y = y + 48; d.shape_p[1] = 8; rgb(d.color, BLUE);    /* a fillet of 8 px at every corner */
        s[1] = ygfx_shape(&d);
        return;
    }
    put_n(s, 2);
}

static void t_stroke_align(float x, float y, const yscr_frame* f) {
    static const ygfx_stroke_align align[3] = { YGFX_STROKE_INSIDE, YGFX_STROKE_CENTER, YGFX_STROKE_OUTSIDE };
    static ygfx_stim s[6];
    int i;
    if (!f) {
        for (i = 0; i < 3; i++) {
            ygfx_shape_desc d = shape_desc(YGFX_RECT, x + (float)(i - 1) * 88, y, 56, 56, GRAY);
            s[2 * i] = ygfx_shape(&d);                        /* the nominal shape */
            d.stroke = 10; d.stroke_align = align[i]; rgb(d.color, AMBER);
            s[2 * i + 1] = ygfx_shape(&d);
        }
        return;
    }
    put_n(s, 6);
}

static void t_joins(float x, float y, const yscr_frame* f) {
    static float pent[10];
    static ygfx_stim s[3];
    int i;
    if (!f) {
        ygfx_shape_desc d;
        for (i = 0; i < 5; i++) {
            pent[2 * i] = 40 * cosf((-90.0f + 72.0f * (float)i) * PI_F / 180);
            pent[2 * i + 1] = 40 * sinf((-90.0f + 72.0f * (float)i) * PI_F / 180);
        }
        d = shape_desc(YGFX_POLYGON, x - 88, y + 4, 80, 80, LIGHT);
        d.shape_p[0] = 5; d.vertices = pent; d.stroke = 10; d.join = YGFX_JOIN_MITER;
        s[0] = ygfx_shape(&d);
        d.x = x; d.join = YGFX_JOIN_ROUND;
        s[1] = ygfx_shape(&d);
        /* a right angle's miter is 1.414 widths, so a limit of 1.2 bevels it */
        d = shape_desc(YGFX_RECT, x + 88, y, 60, 60, LIGHT);
        d.stroke = 10; d.join = YGFX_JOIN_MITER; d.miter_limit = 1.2f;
        s[2] = ygfx_shape(&d);
        return;
    }
    put_n(s, 3);
}

static const float zig[14] = { -90, 15, -60, -15, -30, 15, 0, -15, 30, 15, 60, -15, 90, 15 };

static void t_polyline_joins(float x, float y, const yscr_frame* f) {
    static ygfx_stim s[3];
    int i;
    if (!f) {
        static const ygfx_join j[3] = { YGFX_JOIN_MITER, YGFX_JOIN_BEVEL, YGFX_JOIN_ROUND };
        for (i = 0; i < 3; i++) {
            ygfx_shape_desc d = shape_desc(YGFX_POLYLINE, x, y + (float)(i - 1) * 58, 180, 30, i == 1 ? TEAL : LIGHT);
            d.path = zig; d.n_path = 7; d.shape_p[0] = 10; d.join = j[i];   /* width */
            s[i] = ygfx_shape(&d);
        }
        return;
    }
    put_n(s, 3);
}

static void t_caps(float x, float y, const yscr_frame* f) {
    static const float seg[4] = { -80, 0, 80, 0 };
    static ygfx_stim s[5];
    int i;
    if (!f) {
        for (i = 0; i < 3; i++) {
            ygfx_shape_desc d = shape_desc(YGFX_POLYLINE, x, y + (float)(i - 1) * 55, 160, 16, AMBER);
            d.path = seg; d.n_path = 2; d.shape_p[0] = 18; d.cap = (ygfx_cap)i;
            s[i] = ygfx_shape(&d);
        }
        /* the path's ends, for reference */
        for (i = 0; i < 2; i++) {
            s[3 + i] = shape(YGFX_LINE, x + (float)(i * 160 - 80), y, 170, 0, LIGHT);
            s[3 + i].shape_p[0] = 1.5f; s[3 + i].ori = 90;
        }
        return;
    }
    put_n(s, 5);
}

static void t_qbezier(float x, float y, const yscr_frame* f) {
    static const float arch[6] = { -100, 30, 0, -90, 100, 30 };
    static const float dip[6] = { -100, -10, -20, 70, 100, -10 };
    static ygfx_stim s[2];
    if (!f) {
        ygfx_shape_desc d = shape_desc(YGFX_QBEZIER, x, y - 20, 200, 120, TEAL);
        d.path = arch; d.n_path = 3; d.shape_p[0] = 10; d.cap = YGFX_CAP_ROUND;
        s[0] = ygfx_shape(&d);
        d.y = y + 20; d.path = dip; d.shape_p[0] = 4; d.cap = YGFX_CAP_BUTT; rgb(d.color, LIGHT);
        s[1] = ygfx_shape(&d);
        return;
    }
    put_n(s, 2);
}

static void t_offset(float x, float y, const yscr_frame* f) {
    static ygfx_stim s[3];
    if (!f) {
        ygfx_shape_desc d = shape_desc(YGFX_STAR, x, y, 140, 0, DARK);
        d.shape_p[0] = 5; d.shape_p[1] = 0.5f;
        d.offset = 10; s[0] = ygfx_shape(&d);
        d.offset = 0; rgb(d.color, GRAY); s[1] = ygfx_shape(&d);
        d.offset = -10; rgb(d.color, AMBER); s[2] = ygfx_shape(&d);
        return;
    }
    put_n(s, 3);
}

/* --- page 3: dashes, trim, compounds, effects ---------------------------------- */

static void t_dash_march(float x, float y, const yscr_frame* f) {
    static ygfx_stim s;
    if (!f) {
        ygfx_shape_desc d = shape_desc(YGFX_CIRCLE, x, y, 150, 0, AMBER);
        d.stroke = 5; d.dash[0] = 14; d.dash[1] = 8;            /* a period of 22 px, as the track's */
        s = ygfx_shape(&d);
        bind_stim(&s, YGFX_P_DASH_OFFSET, CH_MARCH);
        return;
    }
    put(&s);
}

static void t_dash_snap(float x, float y, const yscr_frame* f) {
    static ygfx_stim s[2];
    if (!f) {
        ygfx_shape_desc d = shape_desc(YGFX_NGON, x - 68, y, 120, 0, LIGHT);
        d.shape_p[0] = 6; d.shape_p[1] = 8;
        d.stroke = 6; d.dash[0] = 16; d.dash[1] = 10; d.dash_snap = true; d.cap = YGFX_CAP_ROUND;
        s[0] = ygfx_shape(&d);
        d = shape_desc(YGFX_ELLIPSE, x + 70, y, 110, 150, TEAL);
        d.stroke = 6; d.dash[0] = 12; d.dash[1] = 8; d.cap = YGFX_CAP_BUTT;
        s[1] = ygfx_shape(&d);
        return;
    }
    put_n(s, 2);
}

static void t_trim_spin(float x, float y, const yscr_frame* f) {
    static ygfx_stim s[2];
    if (!f) {
        ygfx_shape_desc d = shape_desc(YGFX_CIRCLE, x, y, 140, 0, PANEL);
        d.stroke = 10; d.color[0] = d.color[1] = d.color[2] = 0.28f;   /* the track it runs on */
        s[0] = ygfx_shape(&d);
        rgb(d.color, TEAL); d.cap = YGFX_CAP_ROUND; d.trim[0] = 0; d.trim[1] = 0.5f;
        s[1] = ygfx_shape(&d);
        bind_stim(&s[1], YGFX_P_TRIM_END, CH_TRIM);
        bind_stim(&s[1], YGFX_P_ORI, CH_ROT);
        return;
    }
    put_n(s, 2);
}

static void t_trim_path(float x, float y, const yscr_frame* f) {
    static const float arch[6] = { -100, 25, 0, -60, 100, 25 };
    static ygfx_stim s[2];
    if (!f) {
        ygfx_shape_desc d = shape_desc(YGFX_POLYLINE, x, y - 45, 180, 30, AMBER);
        d.path = zig; d.n_path = 7; d.shape_p[0] = 6; d.cap = YGFX_CAP_ROUND; d.trim[1] = 0.5f;
        s[0] = ygfx_shape(&d);
        d = shape_desc(YGFX_QBEZIER, x, y + 50, 200, 85, LIGHT);
        d.path = arch; d.n_path = 3; d.shape_p[0] = 6; d.cap = YGFX_CAP_ROUND; d.trim[1] = 0.5f;
        s[1] = ygfx_shape(&d);
        bind_stim(&s[0], YGFX_P_TRIM_END, CH_TRIM);
        bind_stim(&s[1], YGFX_P_TRIM_END, CH_TRIM);
        return;
    }
    put_n(s, 2);
}

static void t_ops(float x, float y, const yscr_frame* f) {
    static ygfx_prim pr[4][2];
    static ygfx_stim s[4];
    static const int ops[4] = { YGFX_OP_UNION, YGFX_OP_SUBTRACT, YGFX_OP_INTERSECT, YGFX_OP_XOR };
    int i;
    if (!f) {
        for (i = 0; i < 4; i++) {
            pr[i][0] = prim(YGFX_CIRCLE, YGFX_OP_UNION, -14, 0, 66, 0);
            pr[i][1] = prim(YGFX_RECT, ops[i], 18, 6, 56, 56);
            s[i] = compound(pr[i], 2, x + (float)(i % 2 * 130 - 65), y + (float)(i / 2 * 92 - 46), 120, 80, TEAL);
        }
        return;
    }
    put_n(s, 4);
}

static void t_smooth(float x, float y, const yscr_frame* f) {
    static ygfx_prim pr[2];
    static ygfx_stim s;
    if (!f) {
        pr[0] = prim(YGFX_CIRCLE, YGFX_OP_UNION, -45, 0, 90, 0);
        pr[1] = prim(YGFX_CIRCLE, YGFX_OP_SMOOTH_UNION, 60, 0, 60, 0);
        pr[1].k = 30;                                           /* blend radius */
        s = compound(pr, 2, x, y, 240, 120, VIOLET);
        bind_field(&pr[1].x, CH_MERGE);
        return;
    }
    put(&s);
}

static void t_onion(float x, float y, const yscr_frame* f) {
    static ygfx_prim a[2], b[2];
    static ygfx_stim s[2];
    if (!f) {
        ygfx_compound_desc d;
        a[0] = prim(YGFX_CIRCLE, YGFX_OP_UNION, -18, -14, 70, 0);
        a[1] = prim(YGFX_RECT, YGFX_OP_SMOOTH_UNION, 16, 16, 60, 60);
        a[1].k = 16;
        memset(&d, 0, sizeof d);
        d.place = YGFX_TOP_LEFT; d.prims = a; d.n = 2; d.x = x - 65; d.y = y; d.w = 120; d.h = 120;
        d.onion = 4;                                            /* a shell 8 px wide around the result */
        d.edge = YGFX_EDGE_COSINE; d.edge_width = 1.5f; rgb(d.color, LIGHT);
        s[0] = ygfx_compound(&d);
        b[0] = a[0]; b[1] = a[1];
        b[0].onion = b[1].onion = 4; b[1].op = YGFX_OP_UNION;  /* each a shell, then the union */
        s[1] = compound(b, 2, x + 65, y, 120, 120, AMBER);
        return;
    }
    put_n(s, 2);
}

static void t_gear(float x, float y, const yscr_frame* f) {
    static ygfx_prim pr[6];
    static ygfx_fx fx;
    static ygfx_stim s;
    int i;
    if (!f) {
        pr[0] = prim(YGFX_STAR, YGFX_OP_UNION, 0, 0, 150, 0);
        pr[0].p[0] = 12; pr[0].p[1] = 0.82f; pr[0].p[2] = 2;   /* teeth */
        for (i = 0; i < 4; i++)
            pr[1 + i] = prim(YGFX_CIRCLE, YGFX_OP_SUBTRACT, 36 * cosf((float)i * PI_F / 2), 36 * sinf((float)i * PI_F / 2), 26, 0);
        pr[5] = prim(YGFX_NGON, YGFX_OP_SUBTRACT, 0, 0, 30, 0);
        pr[5].p[0] = 6;
        memset(&fx, 0, sizeof fx);
        fx.dx = 5; fx.dy = 7; fx.drop.sigma = 6; fx.drop.opacity = 0.6f;
        s = compound(pr, 6, x, y, 150, 150, GRAY);
        s.fx = &fx;
        bind_stim(&s, YGFX_P_ORI, CH_ROT);
        return;
    }
    put(&s);
}

static void t_drop_glow(float x, float y, const yscr_frame* f) {
    static ygfx_fx card_fx, glow_fx;
    static ygfx_stim s[2];
    if (!f) {
        ygfx_shape_desc d = shape_desc(YGFX_RRECT, x - 62, y - 4, 110, 80, LIGHT);
        d.shape_p[0] = d.shape_p[1] = d.shape_p[2] = d.shape_p[3] = 12;
        memset(&card_fx, 0, sizeof card_fx);
        card_fx.dx = 6; card_fx.dy = 8; card_fx.drop.sigma = 7; card_fx.drop.opacity = 0.55f;   /* black */
        d.fx = &card_fx;
        s[0] = ygfx_shape(&d);
        d = shape_desc(YGFX_CIRCLE, x + 68, y, 70, 0, AMBER);
        memset(&glow_fx, 0, sizeof glow_fx);
        glow_fx.glow.sigma = 9; glow_fx.glow.spread = 2; glow_fx.glow.opacity = 0.9f;
        rgb(glow_fx.glow.color, AMBER);
        d.fx = &glow_fx;
        s[1] = ygfx_shape(&d);
        return;
    }
    put_n(s, 2);
}

static void t_inner_bands(float x, float y, const yscr_frame* f) {
    static ygfx_fx in_fx, band_fx;
    static ygfx_stim s[2];
    if (!f) {
        ygfx_shape_desc d = shape_desc(YGFX_CIRCLE, x - 66, y, 110, 0, GRAY);
        memset(&in_fx, 0, sizeof in_fx);
        in_fx.dx = 6; in_fx.dy = 6; in_fx.inner.sigma = 5; in_fx.inner.opacity = 0.7f;   /* black */
        d.fx = &in_fx;
        s[0] = ygfx_shape(&d);
        d = shape_desc(YGFX_STAR, x + 66, y, 120, 0, TEAL);
        d.shape_p[0] = 5; d.shape_p[1] = 0.5f; d.shape_p[2] = 3;
        memset(&band_fx, 0, sizeof band_fx);
        band_fx.band[0].a1 = -5; band_fx.band[0].a2 = -3; band_fx.band[0].opacity = 1; rgb(band_fx.band[0].color, LIGHT);
        band_fx.band[1].a1 = 4;  band_fx.band[1].a2 = 7;  band_fx.band[1].opacity = 1; rgb(band_fx.band[1].color, AMBER);
        d.fx = &band_fx;
        s[1] = ygfx_shape(&d);
        return;
    }
    put_n(s, 2);
}

static void t_exact_blur(float x, float y, const yscr_frame* f) {
    static ygfx_fx fx[2];
    static ygfx_stim s[2];
    int i;
    if (!f) {
        for (i = 0; i < 2; i++) {
            ygfx_shape_desc d = shape_desc(YGFX_RECT, x + (float)(i * 140 - 74), y - 6, 80, 80, LIGHT);
            memset(&fx[i], 0, sizeof fx[i]);
            fx[i].dx = 10; fx[i].dy = 10; fx[i].drop.sigma = 10; fx[i].drop.opacity = 0.8f;
            fx[i].flags = i ? YGFX_FX_EXACT_BLUR : 0u;        /* right: the erf product */
            d.fx = &fx[i];
            s[i] = ygfx_shape(&d);
        }
        return;
    }
    put_n(s, 2);
}

/* --- page 4: paint, color spaces, blend modes ---------------------------------- */

static void t_spaces(float x, float y, const yscr_frame* f) {
    /* opposite on the L-M axis: blue to yellow would leave the gamut in DKL
     * polar, whose path keeps the radius while it turns */
    static const float red[3] = { 0.45f, 0.08f, 0.06f }, green[3] = { 0.06f, 0.40f, 0.10f };
    static ygfx_paint p[3];
    static ygfx_stim s[3];
    int i;
    if (!f) {
        for (i = 0; i < 3; i++) {
            ygfx_shape_desc d = shape_desc(YGFX_RRECT, x, y + (float)(i - 1) * 54, 240, 40, GRAY);
            d.shape_p[0] = d.shape_p[1] = d.shape_p[2] = d.shape_p[3] = 6;
            p[i] = paint2(YGFX_PAINT_LINEAR, i, -120, 0, 120, 0, red, green);   /* RGB, OKLAB, DKL_POLAR */
            d.paint = &p[i];
            s[i] = ygfx_shape(&d);
        }
        return;
    }
    put_n(s, 3);
}

static void t_six_stops(float x, float y, const yscr_frame* f) {
    static const float hue[6][3] = { { 0.50f, 0.05f, 0.05f }, { 0.45f, 0.42f, 0.04f }, { 0.05f, 0.42f, 0.06f },
                                     { 0.04f, 0.38f, 0.42f }, { 0.06f, 0.08f, 0.55f }, { 0.45f, 0.06f, 0.45f } };
    static ygfx_paint p[2];
    static ygfx_stim s[2];
    int i, k;
    if (!f) {
        for (i = 0; i < 2; i++) {
            ygfx_shape_desc d = shape_desc(YGFX_RRECT, x, y + (float)(i * 2 - 1) * 36, 240, 56, GRAY);
            d.shape_p[0] = d.shape_p[1] = d.shape_p[2] = d.shape_p[3] = 6;
            memset(&p[i], 0, sizeof p[i]);
            p[i].kind = YGFX_PAINT_LINEAR; p[i].space = (uint8_t)(i ? YGFX_SPACE_OKLAB : YGFX_SPACE_RGB);
            p[i].n = 6; p[i].x0 = -120; p[i].x1 = 120;
            for (k = 0; k < 6; k++) { p[i].stops[k].t = (float)k / 5; rgb(p[i].stops[k].color, hue[k]); }
            d.paint = &p[i];
            s[i] = ygfx_shape(&d);
        }
        return;
    }
    put_n(s, 2);
}

static void t_radial(float x, float y, const yscr_frame* f) {
    static ygfx_paint p;
    static ygfx_stim s;
    if (!f) {
        ygfx_shape_desc d = shape_desc(YGFX_CIRCLE, x, y, 175, 0, GRAY);
        memset(&p, 0, sizeof p);
        p.kind = YGFX_PAINT_RADIAL; p.space = YGFX_SPACE_OKLAB; p.repeat = 1; p.n = 3;
        p.x0 = 0; p.y0 = 0; p.x1 = 30;                          /* center, radius of one period */
        p.stops[0].t = 0; rgb(p.stops[0].color, TEAL);
        p.stops[1].t = 0.5f; rgb(p.stops[1].color, AMBER);
        p.stops[2].t = 1; rgb(p.stops[2].color, TEAL);
        d.paint = &p;
        s = ygfx_shape(&d);
        return;
    }
    put(&s);
}

static void t_angular(float x, float y, const yscr_frame* f) {
    static ygfx_paint p;
    static ygfx_stim s;
    int k;
    if (!f) {
        static const float* hue[6] = { RED, AMBER, GREEN, TEAL, BLUE, RED };
        ygfx_shape_desc d = shape_desc(YGFX_CIRCLE, x, y, 175, 0, GRAY);
        memset(&p, 0, sizeof p);
        p.kind = YGFX_PAINT_ANGULAR; p.n = 6;                 /* RGB: mixes inside the gamut */
        for (k = 0; k < 6; k++) { p.stops[k].t = (float)k / 5; rgb(p.stops[k].color, hue[k]); }
        d.paint = &p;
        s = ygfx_shape(&d);
        bind_field(&p.x1, CH_ROT);                              /* the start angle, degrees */
        return;
    }
    put(&s);
}

static void t_vertex(float x, float y, const yscr_frame* f) {
    static const float tri[6] = { -55, 45, 55, 45, 0, -55 };
    static float hex[12], tri_rgb[9], hex_rgb[18];
    static ygfx_paint p[2];
    static ygfx_stim s[2];
    static const float* hue[6] = { RED, AMBER, GREEN, TEAL, BLUE, VIOLET };
    int k;
    if (!f) {
        ygfx_shape_desc d = shape_desc(YGFX_POLYGON, x - 66, y + 4, 110, 100, GRAY);
        for (k = 0; k < 3; k++) rgb(tri_rgb + 3 * k, hue[2 * k]);
        for (k = 0; k < 6; k++) {
            hex[2 * k] = 55 * cosf((float)k * PI_F / 3); hex[2 * k + 1] = 55 * sinf((float)k * PI_F / 3);
            rgb(hex_rgb + 3 * k, hue[k]);
        }
        memset(p, 0, sizeof p);
        p[0].kind = p[1].kind = YGFX_PAINT_VERTEX;
        p[0].vertex_colors = tri_rgb; p[1].vertex_colors = hex_rgb;
        d.shape_p[0] = 3; d.vertices = tri; d.paint = &p[0];
        s[0] = ygfx_shape(&d);
        d.x = x + 68; d.y = y; d.w = d.h = 110; d.shape_p[0] = 6; d.vertices = hex; d.paint = &p[1];
        s[1] = ygfx_shape(&d);
        return;
    }
    put_n(s, 2);
}

static void t_paint_stroke(float x, float y, const yscr_frame* f) {
    static ygfx_paint p;
    static ygfx_stim s;
    if (!f) {
        ygfx_shape_desc d = shape_desc(YGFX_STAR, x, y, 170, 0, GRAY);
        d.shape_p[0] = 6; d.shape_p[1] = 0.55f; d.shape_p[2] = 4;
        d.stroke = 12;
        p = paint2(YGFX_PAINT_LINEAR, YGFX_SPACE_OKLAB, -70, -70, 70, 70, VIOLET, AMBER);
        d.paint = &p;
        s = ygfx_shape(&d);
        return;
    }
    put(&s);
}

/* Three discs at the corners of a triangle, in one blend mode. */
static void three_discs(ygfx_stim* s, float x, float y, ygfx_blend_mode mode, const float c[3][3], float opacity) {
    int i;
    for (i = 0; i < 3; i++) {
        float a = (-90.0f + 120.0f * (float)i) * PI_F / 180;
        ygfx_shape_desc d = shape_desc(YGFX_CIRCLE, x + 34 * cosf(a), y + 6 + 34 * sinf(a), 100, 0, c[i]);
        d.blend = mode; d.opacity = opacity;
        s[i] = ygfx_shape(&d);
    }
}

static void t_blend_over(float x, float y, const yscr_frame* f) {
    static const float c[3][3] = { { 0.55f, 0.06f, 0.05f }, { 0.06f, 0.45f, 0.08f }, { 0.06f, 0.10f, 0.60f } };
    static ygfx_stim s[3];
    if (!f) { three_discs(s, x, y, YGFX_BLEND_MODE_OVER, c, 0.75f); return; }
    put_n(s, 3);
}

static void t_blend_add(float x, float y, const yscr_frame* f) {
    static const float c[3][3] = { { 0.35f, 0, 0 }, { 0, 0.35f, 0 }, { 0, 0, 0.35f } };
    static ygfx_stim s[3];
    if (!f) { three_discs(s, x, y, YGFX_BLEND_MODE_ADD, c, 1); return; }
    put_n(s, 3);
}

static void t_blend_multiply(float x, float y, const yscr_frame* f) {
    static const float c[3][3] = { { 0.15f, 0.85f, 0.85f }, { 0.85f, 0.15f, 0.85f }, { 0.85f, 0.85f, 0.15f } };
    static ygfx_stim s[4];
    if (!f) {
        s[0] = shape(YGFX_RECT, x, y, 230, 180, LIGHT);           /* the card the filters lie on */
        s[0].shape_p[1] = 8;
        three_discs(s + 1, x, y, YGFX_BLEND_MODE_MULTIPLY, c, 1);
        return;
    }
    put_n(s, 4);
}

static void t_dkl_gabor(float x, float y, const yscr_frame* f) {
    static ygfx_stim g;
    if (!f) {
        static const float lm[3] = { 0, 1, 0 };   /* DKL: luminance, L-M, S */
        ygfx_gabor_desc d;
        memset(&d, 0, sizeof d);
        d.place = YGFX_TOP_LEFT; d.x = x; d.y = y; d.sigma = 22; d.sf = 1 / 30.0f;
        if (ycol_cal_dir_dkl(&cal, BG, lm, d.dir) < 0) { fprintf(stderr, "gfx_gallery: no DKL direction\n"); refused++; }
        d.contrast = 0.9f * ycol_max_contrast(BG, d.dir);   /* in gamut at the peaks */
        g = ygfx_gabor(&d);
        return;
    }
    g.phase = (float)fmod(0.5 * page_seconds(f), 1.0);
    put(&g);
}

static void t_s_cone(float x, float y, const yscr_frame* f) {
    static ygfx_stim g;
    if (!f) {
        static const float s_only[3] = { 0, 0, 1 };   /* cone contrasts L, M, S */
        ygfx_grating_desc d;
        memset(&d, 0, sizeof d);
        d.place = YGFX_TOP_LEFT; d.x = x; d.y = y; d.w = 175; d.sf = 1 / 40.0f; d.ori = 90;
        d.aperture = YGFX_CIRCLE; d.edge = YGFX_EDGE_COSINE; d.edge_width = 20;
        if (ycol_cal_dir_cone(&cal, BG, s_only, d.dir) < 0) { fprintf(stderr, "gfx_gallery: no cone direction\n"); refused++; }
        d.contrast = 0.6f * ycol_max_contrast(BG, d.dir);
        g = ygfx_grating(&d);
        return;
    }
    put(&g);
}

/* --- page 5: images, masks, groups, targets, text ------------------------------ */

/* Four 8 x 8 sprites in a 16 x 16 RGBA8 atlas, linear values. */
static ygfx_tex sprites;
static const char* const sprite_art[4][8] = {
    { "..####..", ".######.", "#ww##ww#", "#wb##wb#", "########", "########", "########", "#.#..#.#" },
    { "........", ".##.##..", "#######.", "#######.", ".#####..", "..###...", "...#....", "........" },
    { "##..##..", "##..##..", "..##..##", "..##..##", "##..##..", "##..##..", "..##..##", "..##..##" },
    { "...##...", "..#ww#..", ".#w..w#.", "#w....w#", "#w....w#", ".#w..w#.", "..#ww#..", "...##..." },
};

static int sprite_setup(void) {
    static uint8_t px[16 * 16 * 4];
    ygfx_texture_desc td;
    int k, r, c;
    for (k = 0; k < 4; k++)
        for (r = 0; r < 8; r++)
            for (c = 0; c < 8; c++) {
                uint8_t* p = px + ((k / 2 * 8 + r) * 16 + k % 2 * 8 + c) * 4;
                char ch = sprite_art[k][r][c];
                const float* col = ch == '#' ? (k == 1 ? RED : (k == 3 ? TEAL : AMBER)) : (ch == 'w' ? LIGHT : BLUE);
                if (ch == '.') { p[0] = p[1] = p[2] = p[3] = 0; continue; }
                p[0] = (uint8_t)(col[0] * 255 + 0.5f); p[1] = (uint8_t)(col[1] * 255 + 0.5f);
                p[2] = (uint8_t)(col[2] * 255 + 0.5f); p[3] = 255;
            }
    memset(&td, 0, sizeof td);
    td.w = td.h = 16; td.format = YGFX_RGBA8; td.data = px;
    sprites = ygfx_texture(&gfx, &td);
    return sprites.id ? 0 : -1;
}

static ygfx_stim sprite(float x, float y, int k, float size, bool linear) {
    ygfx_image_desc d;
    ygfx_stim s;
    memset(&d, 0, sizeof d);
    d.place = YGFX_TOP_LEFT; d.tex = sprites; d.x = x; d.y = y; d.w = d.h = size; d.linear = linear;
    d.src[0] = (float)(k % 2 * 8); d.src[1] = (float)(k / 2 * 8); d.src[2] = d.src[3] = 8;
    s = ygfx_image(&gfx, &d);
    return s;
}

static void t_sprite_nearest(float x, float y, const yscr_frame* f) {
    static ygfx_stim s;
    if (!f) { s = sprite(x, y, 0, 128, false); return; }
    put(&s);
}

static void t_sprite_linear(float x, float y, const yscr_frame* f) {
    static ygfx_stim s;
    if (!f) { s = sprite(x, y, 0, 128, true); return; }
    put(&s);
}

static void t_atlas_tint(float x, float y, const yscr_frame* f) {
    static const float tint[4][4] = { { 1, 1, 1, 1 }, { 1, 1, 1, 0.5f }, { 0.4f, 1, 1, 1 }, { 1, 0.6f, 0.3f, 1 } };
    static ygfx_stim s[4];
    int i, k;
    if (!f) {
        for (i = 0; i < 4; i++) {
            s[i] = sprite(x + (float)(i % 2 * 84 - 42), y + (float)(i / 2 * 84 - 42), i, 64, false);
            for (k = 0; k < 4; k++) s[i].tint[k] = tint[i][k];   /* multiplies linear light */
        }
        return;
    }
    put_n(s, 4);
}

static void t_image_mod(float x, float y, const yscr_frame* f) {
    static float px[64 * 64];
    static ygfx_stim s;
    int i, j;
    if (!f) {
        ygfx_texture_desc td;
        ygfx_image_desc d;
        /* a windmill: six cycles around, its envelope peaking at 12 texels, near 0 at the corners */
        for (j = 0; j < 64; j++)
            for (i = 0; i < 64; i++) {
                float u = (float)i + 0.5f - 32, v = (float)j + 0.5f - 32, r = sqrtf(u * u + v * v) / 12;
                px[j * 64 + i] = cosf(6 * atan2f(v, u)) * r * expf(0.5f * (1 - r * r));
            }
        memset(&td, 0, sizeof td);
        td.w = td.h = 64; td.format = YGFX_R32F; td.data = px;
        memset(&d, 0, sizeof d);
        d.tex = ygfx_texture(&gfx, &td);
        d.place = YGFX_TOP_LEFT; d.x = x; d.y = y; d.w = d.h = 176; d.linear = true;
        d.modulation = true; d.contrast = 0.35f;
        s = ygfx_image(&gfx, &d);
        return;
    }
    put(&s);
}

/* A heart from a bitmap, as an artist's mask would come: the signed
 * distance texture with 16 texels of padding for strokes and edges. */
static ygfx_tex heart;

static int heart_setup(void) {
    enum { M = 128, P = 16 };
    static uint8_t mask[M * M];
    static float sdf[(M + 2 * P) * (M + 2 * P)];
    ygfx_texture_desc td;
    int i, j;
    for (j = 0; j < M; j++)
        for (i = 0; i < M; i++) {
            float u = ((float)i + 0.5f - 64) / 48, v = (68 - (float)j - 0.5f) / 48, a = u * u + v * v - 1;
            mask[j * M + i] = (uint8_t)(a * a * a - u * u * v * v * v <= 0 ? 255 : 0);
        }
    if (ygfx_sdf_from_mask(sdf, mask, M, M, P) < 0) return -1;
    memset(&td, 0, sizeof td);
    td.w = td.h = M + 2 * P; td.format = YGFX_R16F; td.data = sdf;
    heart = ygfx_texture(&gfx, &td);
    return heart.id ? 0 : -1;
}

static void t_mask(float x, float y, const yscr_frame* f) {
    static ygfx_stim s[2];
    if (!f) {
        ygfx_shape_desc d = shape_desc(YGFX_MASK_TEX, x, y, 176, 176, RED);   /* 1.1 px per texel */
        d.mask = heart;
        s[0] = ygfx_shape(&d);
        d.stroke = 4; d.stroke_align = YGFX_STROKE_OUTSIDE; rgb(d.color, LIGHT);
        s[1] = ygfx_shape(&d);
        return;
    }
    put_n(s, 2);
}

static void t_mask_grating(float x, float y, const yscr_frame* f) {
    static ygfx_stim g;
    if (!f) {
        ygfx_grating_desc d;
        memset(&d, 0, sizeof d);
        d.place = YGFX_TOP_LEFT; d.x = x; d.y = y; d.w = 176; d.sf = 1 / 14.0f; d.ori = 60; d.contrast = 0.35f;
        d.aperture = YGFX_MASK_TEX; d.mask = heart; d.edge = YGFX_EDGE_COSINE; d.edge_width = 6;
        g = ygfx_grating(&d);
        return;
    }
    g.phase = (float)fmod(page_seconds(f), 1.0);
    put(&g);
}

static void t_group(float x, float y, const yscr_frame* f) {
    static ygfx_group grp;
    static ygfx_stim s[7];
    static const ygfx_shape_kind kind[6] = { YGFX_CIRCLE, YGFX_RECT, YGFX_NGON, YGFX_STAR, YGFX_CROSS, YGFX_ELLIPSE };
    static const float* col[6] = { RED, GREEN, BLUE, AMBER, TEAL, VIOLET };
    int i;
    if (!f) {
        ygfx_group_desc gd;
        ygfx_gabor_desc g;
        memset(&gd, 0, sizeof gd);
        gd.place = YGFX_TOP_LEFT; gd.x = x; gd.y = y;          /* a point: 0 x 0 */
        grp = ygfx_group_make(&gd);
        for (i = 0; i < 6; i++) {
            float a = (float)i * PI_F / 3;
            ygfx_shape_desc d = shape_desc(kind[i], 66 * cosf(a), 66 * sinf(a), 30, kind[i] == YGFX_ELLIPSE ? 18.0f : 30.0f, col[i]);
            d.place = YGFX_CENTER;                            /* the group's point; x, y in its frame */
            d.shape_p[0] = kind[i] == YGFX_NGON ? 3.0f : (kind[i] == YGFX_STAR ? 5.0f : (kind[i] == YGFX_CROSS ? 8.0f : 0.0f));
            d.ori = (float)i * 60;
            d.group = &grp;
            s[i] = ygfx_shape(&d);
        }
        memset(&g, 0, sizeof g);
        g.sigma = 12; g.sf = 1 / 12.0f; g.contrast = 0.3f; g.group = &grp;   /* sf is divided by the scale */
        s[6] = ygfx_gabor(&g);
        bind_group(&grp, YGFX_G_ORI, CH_ROT);
        bind_group(&grp, YGFX_G_SCALE, CH_PULSE);
        return;
    }
    put_n(s, 7);
}

static void t_group_vs_target(float x, float y, const yscr_frame* f) {
    static ygfx_group grp;
    static ygfx_stim member[2], inside[2], flat;
    static ygfx_tex t;
    static int filled;
    static const float clear[4] = { 0, 0, 0, 0 };
    int i;
    if (!f) {
        ygfx_group_desc gd;
        ygfx_target_desc td;
        ygfx_image_desc d;
        memset(&gd, 0, sizeof gd);
        gd.place = YGFX_TOP_LEFT; gd.x = x - 66; gd.y = y; gd.opacity = 0.5f;
        grp = ygfx_group_make(&gd);
        for (i = 0; i < 2; i++) {
            ygfx_shape_desc sd = shape_desc(YGFX_CIRCLE, (float)(i * 40 - 20), 0, 76, 0, LIGHT);
            sd.place = YGFX_CENTER; sd.group = &grp;
            member[i] = ygfx_shape(&sd);
            /* the same discs in the target's own pixels, at full opacity */
            inside[i] = shape(YGFX_CIRCLE, (float)(64 + i * 40 - 20), 48, 76, 0, LIGHT);
        }
        memset(&td, 0, sizeof td);
        td.w = 128; td.h = 96;
        t = ygfx_target(&gfx, &td);
        memset(&d, 0, sizeof d);
        d.place = YGFX_TOP_LEFT; d.tex = t; d.x = x + 66; d.y = y; d.opacity = 0.5f;
        flat = ygfx_image(&gfx, &d);
        return;
    }
    if (!filled) {   /* draw once; the target keeps it */
        ygfx_begin_target(&gfx, t, clear);
        put_n(inside, 2);
        ygfx_end_target(&gfx);
        filled = 1;
    }
    put_n(member, 2);   /* each disc at 0.5: the overlap is denser */
    put(&flat);         /* the layer at 0.5: uniform */
}

static void t_target_turn(float x, float y, const yscr_frame* f) {
    static ygfx_stim inside[3], img;
    static ygfx_tex t;
    static int filled;
    static const float clear[4] = { 0, 0, 0, 0 };
    if (!f) {
        ygfx_target_desc td;
        ygfx_image_desc d;
        ygfx_shape_desc sd;
        memset(&td, 0, sizeof td);
        td.w = td.h = 96; td.linear = true;
        t = ygfx_target(&gfx, &td);
        sd = shape_desc(YGFX_CIRCLE, 48, 48, 88, 0, TEAL);
        sd.stroke = 5;
        inside[0] = ygfx_shape(&sd);
        sd = shape_desc(YGFX_STAR, 48, 48, 70, 0, AMBER);
        sd.shape_p[0] = 4; sd.shape_p[1] = 0.4f;
        inside[1] = ygfx_shape(&sd);
        inside[2] = shape(YGFX_CIRCLE, 48, 48, 14, 0, DARK);
        memset(&d, 0, sizeof d);
        d.place = YGFX_TOP_LEFT; d.tex = t; d.x = x; d.y = y; d.w = d.h = 168; d.linear = true;
        img = ygfx_image(&gfx, &d);
        bind_stim(&img, YGFX_P_ORI, CH_ROT);
        return;
    }
    if (!filled) {
        ygfx_begin_target(&gfx, t, clear);
        put_n(inside, 3);
        ygfx_end_target(&gfx);
        filled = 1;
    }
    put(&img);
}

static void t_target_add(float x, float y, const yscr_frame* f) {
    static ygfx_stim inside[5], img;
    static ygfx_tex t;
    static int filled;
    static const float clear[4] = { 0, 0, 0, 0 };
    int i;
    if (!f) {
        ygfx_target_desc td;
        ygfx_image_desc d;
        ygfx_gabor_desc g;
        memset(&td, 0, sizeof td);
        td.w = td.h = 176;                                      /* RGBA16F: negative increments too */
        t = ygfx_target(&gfx, &td);
        memset(&g, 0, sizeof g);
        g.place = YGFX_TOP_LEFT; g.sigma = 11; g.sf = 1 / 11.0f; g.contrast = 0.35f;
        for (i = 0; i < 5; i++) {
            float a = (float)i * 2 * PI_F / 5;
            g.x = i ? 88 + 56 * cosf(a) : 88; g.y = i ? 88 + 56 * sinf(a) : 88; g.ori = (float)i * 36;
            inside[i] = ygfx_gabor(&g);
        }
        memset(&d, 0, sizeof d);
        d.place = YGFX_TOP_LEFT; d.tex = t; d.x = x; d.y = y; d.add = true;
        img = ygfx_image(&gfx, &d);
        return;
    }
    if (!filled) {
        ygfx_begin_target(&gfx, t, clear);
        put_n(inside, 5);
        ygfx_end_target(&gfx);
        filled = 1;
    }
    put(&img);
}

/* "YSP" at 8 px per font pixel: one atlas texel per pixel. */
static ygfx_buf big_text;
static int big_count;

static void t_glyph_outline(float x, float y, const yscr_frame* f) {
    static ygfx_stim s[3];
    if (!f) {
        s[0] = text_run(big_text, big_count, 8, YGFX_CENTER, x, y - 42, 136, 56, AMBER);
        s[0].stroke = 4; s[0].stroke_align = YGFX_STROKE_OUTSIDE;   /* the outline, under the fill */
        s[1] = text_run(big_text, big_count, 8, YGFX_CENTER, x, y - 42, 136, 56, LIGHT);
        s[2] = text_run(big_text, big_count, 8, YGFX_CENTER, x, y + 46, 136, 56, TEAL);
        s[2].offset = 2;                                        /* 2 px bolder on every side */
        return;
    }
    put_n(s, 3);
}

static void t_glyph_tilt(float x, float y, const yscr_frame* f) {
    static ygfx_stim s;
    if (!f) {
        s = text_run(big_text, big_count, 8, YGFX_CENTER, x, y, 136, 56, LIGHT);
        bind_stim(&s, YGFX_P_ORI, CH_TILT);
        return;
    }
    put(&s);
}

/* --- page 6: v0.4: the program cache, video, instances ------------------------ */

/* The point the hit test looks at: the mouse when it is over the tile, else
 * a point that circles the field, so --sim and the screenshots show it. */
static float mouse_x = -1, mouse_y = -1;

static ygfx_inst field_el[400];

static void t_inst_field(float x, float y, const yscr_frame* f) {
    static ygfx_stim field, ring;
    float hx, hy, ex, ey;
    int k;
    if (!f) {
        ygfx_gabor_desc d;
        ygfx_instances_desc id;
        ygfx_shape_desc sd;
        int i;
        /* a sunflower: 400 points evenly in a disc of radius 88 px */
        for (i = 0; i < 400; i++) {
            float r = 88.0f * sqrtf(((float)i + 0.5f) / 400.0f), a = (float)i * 2.39996323f;
            memset(&field_el[i], 0, sizeof field_el[i]);
            field_el[i].x = r * cosf(a); field_el[i].y = r * sinf(a);
            field_el[i].ori = (float)(i * 47 % 180);
        }
        memset(&d, 0, sizeof d);
        d.place = YGFX_TOP_LEFT; d.x = x; d.y = y;
        d.sigma = 2.2f; d.sf = 1 / 5.0f; d.contrast = 0.35f;
        field = ygfx_gabor(&d);
        memset(&id, 0, sizeof id);
        id.inst = field_el; id.n = 400; id.fields = YGFX_I_XY | YGFX_I_ORI;
        field = ygfx_instances(&gfx, &field, &id);   /* the instanced program is made here */
        bind_stim(&field, YGFX_P_ORI, CH_ROT);      /* the template's ori turns the whole field */
        sd = shape_desc(YGFX_CIRCLE, x, y, 22, 0, LIGHT);
        sd.stroke = 1.5f;
        ring = ygfx_shape(&sd);
        ring.visible = 0;
        return;
    }
    put(&field);
    /* the element under the point, outlined */
    if (mouse_x >= x - 140 && mouse_x < x + 140 && mouse_y >= y - 100 && mouse_y < y + 100) { hx = mouse_x; hy = mouse_y; }
    else {
        double t = page_seconds(f) * 0.25;
        hx = x + (float)(55.0 * cos(6.28318530718 * t)); hy = y + (float)(55.0 * sin(6.28318530718 * t));
    }
    k = ygfx_hit_index(&gfx, &field, hx, hy);
    ring.visible = k >= 0 ? 1.0f : 0.0f;
    if (k >= 0) {
        ygfx_inst_resolve(&gfx, &field, k, &ex, &ey);
        ring.x = ex; ring.y = ey;
    }
    put(&ring);
}

static const float pal6[3 * 6] = { 0.55f, 0.10f, 0.07f, 0.10f, 0.42f, 0.12f, 0.08f, 0.16f, 0.60f,
                                   0.60f, 0.32f, 0.04f, 0.05f, 0.38f, 0.40f, 0.32f, 0.10f, 0.50f };
static ygfx_inst disc_el[300];

static void t_inst_palette(float x, float y, const yscr_frame* f) {
    static ygfx_stim s;
    if (!f) {
        ygfx_shape_desc d = shape_desc(YGFX_CIRCLE, x, y, 9, 0, GRAY);
        ygfx_instances_desc id;
        int i;
        uint32_t seed = 7;
        ygfx_inst_grid(disc_el, 20, 15, 13, 12);
        for (i = 0; i < 300; i++) {
            seed = seed * 1664525u + 1013904223u;
            disc_el[i].color = (float)(seed >> 8) / 16777216.0f * 5.0f;   /* fractions mix neighbors */
            disc_el[i].scale = 0.6f + 0.8f * (float)((seed >> 4) & 255) / 255.0f;
        }
        memset(&id, 0, sizeof id);
        id.inst = disc_el; id.n = 300; id.fields = YGFX_I_XY | YGFX_I_COLOR | YGFX_I_SCALE;
        id.palette = pal6; id.n_palette = 6;
        s = ygfx_shape(&d);
        s = ygfx_instances(&gfx, &s, &id);
        return;
    }
    put(&s);
}

/* Color bars over a gray ramp, as R'G'B' codes: 64 x 48, two-texel bars,
 * so 4:2:0 chroma stays inside a bar. Low contrast: codes 60 to 180. */
#define VID_W 64
#define VID_H 48
static uint8_t vid_rgba[VID_W * VID_H * 4];
static uint8_t vid_y[VID_W * VID_H], vid_uv[VID_W * VID_H / 2];
static ygfx_tex vid_nv12, vid_rgb;

static int video_setup(void) {
    static const uint8_t bars[8][3] = { { 180, 180, 180 }, { 180, 180, 60 }, { 60, 180, 180 }, { 60, 180, 60 },
                                        { 180, 60, 180 }, { 180, 60, 60 }, { 60, 60, 180 }, { 60, 60, 60 } };
    const double kr = 0.2126, kb = 0.0722, kg = 1.0 - kr - kb;
    ygfx_texture_desc td;
    ygfx_planes pl;
    int x, y;
    for (y = 0; y < VID_H; y++)
        for (x = 0; x < VID_W; x++) {
            uint8_t* p = vid_rgba + (y * VID_W + x) * 4;
            if (y < 32) { p[0] = bars[x / 8][0]; p[1] = bars[x / 8][1]; p[2] = bars[x / 8][2]; }
            else p[0] = p[1] = p[2] = (uint8_t)(60 + 120 * x / (VID_W - 1));
            p[3] = 255;
        }
    /* BT.709, limited range; chroma co-sited with even columns (LEFT) and
     * between rows: the mean of the two rows it sits between */
    for (y = 0; y < VID_H; y++)
        for (x = 0; x < VID_W; x++) {
            const uint8_t* p = vid_rgba + (y * VID_W + x) * 4;
            double Y = kr * p[0] / 255.0 + kg * p[1] / 255.0 + kb * p[2] / 255.0;
            vid_y[y * VID_W + x] = (uint8_t)floor(16.0 + 219.0 * Y + 0.5);
        }
    for (y = 0; y < VID_H / 2; y++)
        for (x = 0; x < VID_W / 2; x++) {
            double cb = 0, cr = 0;
            int r;
            for (r = 0; r < 2; r++) {
                const uint8_t* p = vid_rgba + ((2 * y + r) * VID_W + 2 * x) * 4;
                double Y = kr * p[0] / 255.0 + kg * p[1] / 255.0 + kb * p[2] / 255.0;
                cb += 0.5 * (p[2] / 255.0 - Y) / (2.0 * (1.0 - kb));
                cr += 0.5 * (p[0] / 255.0 - Y) / (2.0 * (1.0 - kr));
            }
            vid_uv[y * VID_W + 2 * x] = (uint8_t)floor(128.0 + 224.0 * cb + 0.5);
            vid_uv[y * VID_W + 2 * x + 1] = (uint8_t)floor(128.0 + 224.0 * cr + 0.5);
        }
    memset(&pl, 0, sizeof pl);
    pl.data[0] = vid_y; pl.data[1] = vid_uv;
    memset(&td, 0, sizeof td);
    td.w = VID_W; td.h = VID_H; td.format = YGFX_NV12; td.planes = &pl;
    td.enc.matrix = YGFX_MATRIX_BT709; td.enc.range = YGFX_RANGE_LIMITED;
    td.enc.transfer = YGFX_TRC_DEVICE;    /* the codes, as device values */
    td.enc.primaries = YGFX_PRIM_DEVICE;  /* stated: no gamut conversion */
    td.enc.siting = YGFX_SITING_LEFT;
    vid_nv12 = ygfx_texture(&gfx, &td);
    /* the same codes as RGBA8, through the same video program */
    memset(&td, 0, sizeof td);
    td.w = VID_W; td.h = VID_H; td.format = YGFX_RGBA8; td.data = vid_rgba;
    td.enc.matrix = YGFX_MATRIX_RGB; td.enc.range = YGFX_RANGE_FULL;
    td.enc.transfer = YGFX_TRC_DEVICE; td.enc.primaries = YGFX_PRIM_DEVICE;
    vid_rgb = ygfx_texture(&gfx, &td);
    return vid_nv12.id && vid_rgb.id ? 0 : -1;
}

static void t_nv12(float x, float y, const yscr_frame* f) {
    static ygfx_stim s[2];
    if (!f) {
        ygfx_image_desc d;
        int k;
        for (k = 0; k < 2; k++) {
            memset(&d, 0, sizeof d);
            d.place = YGFX_TOP_LEFT; d.tex = k ? vid_rgb : vid_nv12;
            d.x = x + (k ? 68.0f : -68.0f); d.y = y; d.w = 2 * VID_W; d.h = 2 * VID_H;
            s[k] = ygfx_image(&gfx, &d);
        }
        return;
    }
    put_n(s, 2);
}

/* A soft disc: straight alpha (rgb the color, alpha the coverage) and the
 * same texels premultiplied; drawn linear at 16x they must look the same. */
static void t_alpha(float x, float y, const yscr_frame* f) {
    static ygfx_stim s[2];
    if (!f) {
        static uint8_t straight[8 * 8 * 4], pre[8 * 8 * 4];
        ygfx_texture_desc td;
        ygfx_image_desc d;
        ygfx_tex t[2];
        int i, k;
        for (i = 0; i < 64; i++) {
            float u = (float)(i % 8) - 3.5f, v = (float)(i / 8) - 3.5f, a = 1.0f - sqrtf(u * u + v * v) / 4.0f;
            a = a < 0 ? 0 : a;
            for (k = 0; k < 3; k++) {
                straight[i * 4 + k] = (uint8_t)(TEAL[k] * 255 + 0.5f);
                pre[i * 4 + k] = (uint8_t)(TEAL[k] * a * 255 + 0.5f);
            }
            straight[i * 4 + 3] = pre[i * 4 + 3] = (uint8_t)(a * 255 + 0.5f);
        }
        memset(&td, 0, sizeof td);
        td.w = td.h = 8; td.format = YGFX_RGBA8;
        td.data = straight; t[0] = ygfx_texture(&gfx, &td);
        td.data = pre; t[1] = ygfx_texture(&gfx, &td);
        for (k = 0; k < 2; k++) {
            memset(&d, 0, sizeof d);
            d.place = YGFX_TOP_LEFT; d.tex = t[k]; d.linear = true;
            d.x = x + (k ? 66.0f : -66.0f); d.y = y; d.w = d.h = 128;
            d.premultiplied = k == 1;   /* say which the texels are; never guessed */
            s[k] = ygfx_image(&gfx, &d);
        }
        return;
    }
    put_n(s, 2);
}

/* Three runs from one glyph buffer, each from its own first record. */
static void t_glyph_first(float x, float y, const yscr_frame* f) {
    static ygfx_stim s[3];
    if (!f) {
        static const char* const words[3] = { "CACHE", "VIDEO", "INSTANCES" };
        static const float* const colors[3] = { AMBER, TEAL, LIGHT };
        static ygfx_glyph g[24];
        ygfx_buf b;
        int first[3], count[3], n = 0, k;
        for (k = 0; k < 3; k++) {
            first[k] = n;
            count[k] = layout(g + n, 24 - n, words[k], 0, 0, 4);
            n += count[k];
        }
        b = ygfx_buffer(&gfx, sizeof g);
        ygfx_buffer_update(&gfx, b, 0, g, sizeof g);
        for (k = 0; k < 3; k++) {
            float w = (float)(strlen(words[k]) * 24 - 4);
            s[k] = text_run(b, count[k], 4, YGFX_CENTER, x, y + (float)(k - 1) * 44, w, 28, colors[k]);
            s[k].first = (uint32_t)first[k];
        }
        return;
    }
    put_n(s, 3);
}

/* What the program cache did at open, as text made at setup. */
static ygfx_stim cache_text;
static int cache_text_setup(float x, float y, int cache_on) {
    static ygfx_glyph g[160];
    ygfx_programs ps;
    char t[200];
    ygfx_buf b;
    int n;
    ygfx_program_stats(&gfx, &ps);
    snprintf(t, sizeof t, "%s\nFROM THE CACHE: %u\nCOMPILED:       %u\nREJECTED:       %u\nOPEN:      %5.0f MS",
             cache_on ? "PROGRAM CACHE ON" : "PROGRAM CACHE OFF", ps.loaded, ps.compiled, ps.rejected, (double)ps.open_ns * 1e-6);
    n = layout(g, 160, t, 0, 0, 2.0f);   /* the labels' size */
    b = ygfx_buffer(&gfx, sizeof g);
    if (!b.id || ygfx_buffer_update(&gfx, b, 0, g, sizeof g) < 0) return -1;
    cache_text = text_run(b, n, 2.0f, YGFX_CENTER, x, y, 214, 90, LIGHT);
    return 0;
}

static int cache_on;
static void t_cache(float x, float y, const yscr_frame* f) {
    if (!f) { if (cache_text_setup(x, y, cache_on) < 0) refused++; return; }
    put(&cache_text);
}

static void t_grating_annulus(float x, float y, const yscr_frame* f) {
    static ygfx_stim g;
    if (!f) {
        ygfx_grating_desc d;
        memset(&d, 0, sizeof d);
        d.place = YGFX_TOP_LEFT; d.x = x; d.y = y; d.w = 180;
        d.sf = 1 / 14.0f; d.ori = 60; d.contrast = 0.3f;
        d.aperture = YGFX_ANNULUS; d.shape_p[0] = 40;   /* the inner radius, in the desc */
        d.edge = YGFX_EDGE_COSINE; d.edge_width = 6;
        g = ygfx_grating(&d);
        return;
    }
    put(&g);
}

/* --- page 7: v0.6: curve runs, the blur pass --------------------------------- */

/* The 5 x 7 font as a curve set: each lit pixel a unit square, the squares
 * of a letter merged by ysp/outline.h (overlaps removed), so every glyph is
 * resolved and drawn by exact area. Glyph ids are the characters from ' ';
 * units are font pixels, y down, the pen on the baseline. */
static ygfx_cset font_set;
static yol_ctx ol_cx;

static int font_cset_setup(void) {
    static int done;
    yol_cset s;
    yol_cset_desc od;
    yol_path p;
    ygfx_cset_desc d;
    int g, r, c, rc = 0;
    if (done) return font_set.id ? 0 : -1;
    done = 1;
    if (!yol_open(&ol_cx, NULL)) return -1;
    memset(&od, 0, sizeof od);
    od.n_glyphs = FONT_N; od.backward = true;   /* backward lists: the rays are faster */
    if (!yol_cset_open(&s, &ol_cx, &od)) return -1;
    yol_path_init(&p, &ol_cx);
    for (g = 1; g < FONT_N && rc == 0; g++) {
        yol_path_clear(&p);
        for (r = 0; r < 7; r++)
            for (c = 0; c < 5; c++)
                if (font5x7[g][r] & (0x10 >> c)) yol_rect(&p, c, r - 7, 1, 1, 0, 0);
        if (yol_path_end(&p) != YOL_OK || yol_cset_add(&s, (uint32_t)g, &p) != YOL_OK) rc = -1;
    }
    yol_path_free(&p);
    if (rc == 0) {
        memset(&d, 0, sizeof d);
        d.texels = s.texels; d.n_texels = s.n_texels; d.words = s.words; d.n_words = s.n_words;
        font_set = ygfx_cset_make(&gfx, &d);
    }
    yol_cset_close(&s);
    return font_set.id ? 0 : -1;
}

/* Items for text in the font set at px screen pixels per font pixel: pen
 * positions (in px: items are in the run's units) 6 font pixels apart. */
static int font_items(ygfx_citem* it, int cap, const char* s, float px) {
    int n = 0;
    float pen = 0;
    for (; *s && n < cap; s++, pen += 6 * px) {
        int c = (unsigned char)*s;
        if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
        if (c <= FONT_FIRST || c >= FONT_FIRST + FONT_N) continue;
        memset(&it[n], 0, sizeof it[n]);
        it[n].x = pen; it[n].glyph = (float)(c - FONT_FIRST); it[n].gate = 1;
        n++;
    }
    return n;
}

static ygfx_crun_desc font_run_desc(ygfx_citem* it, int n, float px, float x, float y, const float* color) {
    ygfx_crun_desc d;
    memset(&d, 0, sizeof d);
    d.place = YGFX_TOP_LEFT; d.anchor = YGFX_CENTER; d.x = x; d.y = y;
    d.set = font_set; d.items = it; d.n = n; d.size = px;
    rgb(d.color, color);
    return d;
}

/* GSAP's gsap.from(chars, { y: 30, opacity: 0, stagger: 0.08 }): each
 * letter's y and gate on its own channel, keyed tracks that repeat every 4 s */
#define WORD_N 7
static ygfx_citem word_it[WORD_N];
static ytl_key word_y[WORD_N][4], word_g[WORD_N][4];

static void t_stagger(float x, float y, const yscr_frame* f) {
    static ygfx_stim word;
    if (!f) {
        ygfx_crun_desc d;
        int n, k;
        if (font_cset_setup() < 0) { refused++; return; }
        n = font_items(word_it, WORD_N, "STAGGER", 5);
        d = font_run_desc(word_it, n, 5, x, y, LIGHT);
        d.fields = YGFX_I_GATE;
        word = ygfx_crun(&gfx, &d);
        for (k = 0; k < n; k++) {
            bind_field(&word_it[k].y, CH_LETTER_Y + k);
            bind_field(&word_it[k].gate, CH_LETTER_G + k);
        }
        return;
    }
    put(&word);
}

static int stagger_tracks(void) {
    ytl_track tr;
    int k, rc = 0;
    memset(&tr, 0, sizeof tr);
    tr.n_keys = 4; tr.period_ns = 4 * S_NS;
    for (k = 0; k < WORD_N; k++) {
        int64_t t0 = (int64_t)k * 80000000;   /* 0.08 s apart */
        ytl_key* a = word_y[k];
        ytl_key* b = word_g[k];
        memset(a, 0, sizeof word_y[k]);
        memset(b, 0, sizeof word_g[k]);
        a[0].time_ns = t0;            a[0].value = 30; a[0].ease = YTL_EASE_QUAD_OUT;   /* px below */
        a[1].time_ns = t0 + 700000000; a[1].value = 0;
        a[2].time_ns = 3200000000;    a[2].value = 0;  a[2].ease = YTL_EASE_COSINE;
        a[3].time_ns = 3700000000;    a[3].value = 0;
        b[0].time_ns = t0;            b[0].value = 0;  b[0].ease = YTL_EASE_QUAD_OUT;
        b[1].time_ns = t0 + 700000000; b[1].value = 1;
        b[2].time_ns = 3200000000;    b[2].value = 1;  b[2].ease = YTL_EASE_COSINE;
        b[3].time_ns = 3700000000;    b[3].value = 0;
        tr.keys = a;
        rc |= ytl_set_track(&tl, CH_LETTER_Y + k, PAGE_BASE, &tr);
        tr.keys = b;
        rc |= ytl_set_track(&tl, CH_LETTER_G + k, PAGE_BASE, &tr);
    }
    return rc;
}

/* CSS's text-shadow: 0 0 Npx gold: the word drawn white into an R16F layer
 * once, blurred each frame with sigma on a tween, drawn tinted under the
 * sharp word. */
static ygfx_blur glow;

static void t_glow(float x, float y, const yscr_frame* f) {
    static ygfx_stim white, word, img;
    static ygfx_citem it[8];
    static const float zero[4] = { 0, 0, 0, 0 };
    if (!f) {
        ygfx_blur_desc bd;
        ygfx_crun_desc d;
        int n;
        if (font_cset_setup() < 0) { refused++; return; }
        n = font_items(it, 8, "GLOW", 9);
        memset(&bd, 0, sizeof bd);
        bd.w = 260; bd.h = 140; bd.format = YGFX_R16F; bd.sigma = 3;
        if (ygfx_blur_make(&gfx, &glow, &bd) != YGFX_OK) { refused++; return; }
        d = font_run_desc(it, n, 9, 0, 0, LIGHT);
        d.place = YGFX_CENTER;               /* at the layer's center */
        d.color[0] = d.color[1] = d.color[2] = 1;
        white = ygfx_crun(&gfx, &d);
        d = font_run_desc(it, n, 9, x, y, DARK);
        word = ygfx_crun(&gfx, &d);
        bind_field(&glow.sigma, CH_SIGMA);
        /* the layer keeps the sharp word: drawn once, in a setup pass */
        if (ygfx_begin_setup(&gfx) != YGFX_OK || ygfx_begin_target(&gfx, glow.layer, zero) != YGFX_OK) { refused++; return; }
        put(&white);
        ygfx_end_target(&gfx);
        if (ygfx_end_setup(&gfx) != YGFX_OK) refused++;
        return;
    }
    if (ygfx_blur_apply(&gfx, &glow) != YGFX_OK) refused++;
    img = glow.image;
    img.place = YGFX_TOP_LEFT; img.ax = img.ay = 0.5f; img.x = x; img.y = y;
    rgb(img.tint, AMBER);
    put(&img);
    put(&word);
}

/* Artwork: three layers of one set, one run, a palette color each; the
 * layer under the pointer (ygfx_hit_index: the set kept on the CPU) is
 * drawn again, light, over the others. */
static ygfx_cset art_set;
static yol_cset art_src;   /* .keep: the hit test reads these arrays until exit */
static ygfx_citem art_it[3], art_hl[1];
static const float art_pal[9] = { 0.05f, 0.38f, 0.40f, 0.60f, 0.32f, 0.04f, 0.32f, 0.10f, 0.50f };

static void t_artwork(float x, float y, const yscr_frame* f) {
    static ygfx_stim art, hl;
    float hx, hy;
    int k;
    if (!f) {
        yol_cset* s = &art_src;
        yol_cset_desc od;
        yol_path p;
        ygfx_cset_desc d;
        ygfx_crun_desc rd;
        if (font_cset_setup() < 0) { refused++; return; }   /* makes ol_cx */
        memset(&od, 0, sizeof od);
        od.n_glyphs = 3;
        if (!yol_cset_open(s, &ol_cx, &od)) { refused++; return; }
        yol_path_init(&p, &ol_cx);
        yol_ellipse(&p, 0, 0, 78, 50);                         /* layer 0: a body */
        yol_path_end(&p);
        yol_cset_add(s, 0, &p);
        yol_path_clear(&p);
        for (k = 0; k < 14; k++) {                               /* layer 1: a star of 7 */
            double a = 3.14159265358979 * k / 7.0, r = k % 2 ? 18 : 42;
            if (k == 0) yol_move(&p, r * sin(a), -r * cos(a)); else yol_line(&p, r * sin(a), -r * cos(a));
        }
        yol_close_path(&p);
        yol_path_end(&p);
        yol_cset_add(s, 1, &p);
        yol_path_clear(&p);
        yol_ellipse(&p, 0, 0, 11, 11);                         /* layer 2: an eye */
        yol_path_end(&p);
        yol_cset_add(s, 2, &p);
        yol_path_free(&p);
        memset(&d, 0, sizeof d);
        d.texels = s->texels; d.n_texels = s->n_texels; d.words = s->words; d.n_words = s->n_words; d.keep = true;
        art_set = ygfx_cset_make(&gfx, &d);
        if (!art_set.id) { refused++; return; }
        memset(art_it, 0, sizeof art_it);
        for (k = 0; k < 3; k++) { art_it[k].x = 90; art_it[k].y = 60; art_it[k].glyph = (float)k; art_it[k].color = (float)k; }
        memset(&rd, 0, sizeof rd);
        rd.place = YGFX_TOP_LEFT; rd.anchor = YGFX_CENTER; rd.x = x; rd.y = y; rd.w = 180; rd.h = 120;
        rd.set = art_set; rd.items = art_it; rd.n = 3; rd.size = 1; rd.fields = YGFX_I_COLOR;
        rd.palette = art_pal; rd.n_palette = 3;
        art = ygfx_crun(&gfx, &rd);
        art_hl[0] = art_it[0];
        rd.items = art_hl; rd.n = 1; rd.fields = 0; rd.palette = NULL; rd.n_palette = 0;
        rgb(rd.color, LIGHT); rd.opacity = 0.45f;
        hl = ygfx_crun(&gfx, &rd);
        return;
    }
    put(&art);
    if (mouse_x >= x - 140 && mouse_x < x + 140 && mouse_y >= y - 100 && mouse_y < y + 100) { hx = mouse_x; hy = mouse_y; }
    else {
        double t = page_seconds(f) * 0.2;
        hx = x + (float)(70.0 * cos(6.28318530718 * t)); hy = y + (float)(40.0 * sin(6.28318530718 * t));
    }
    k = ygfx_hit_index(&gfx, &art, hx, hy);
    if (k >= 0) { art_hl[0].glyph = art_it[k].glyph; put(&hl); }
}

/* A reading page: static text belongs in a target, rendered once in a
 * setup pass and composited at 1:1 on whole px (the same coverage, bit for
 * bit); a run is drawn directly only when it moves, turns, scales or
 * animates. */
static void t_cached(float x, float y, const yscr_frame* f) {
    static ygfx_tex page;
    static ygfx_stim text, img;
    static ygfx_citem it[160];
    static const float zero[4] = { 0, 0, 0, 0 };
    if (!f) {
        static const char* lines[4] = { "RENDERED AT SETUP", "INTO AN RGBA16F", "TARGET, COMPOSITED", "EACH FRAME AT 1:1" };
        ygfx_target_desc td;
        ygfx_image_desc id;
        ygfx_crun_desc d;
        int n = 0, l, k, m;
        if (font_cset_setup() < 0) { refused++; return; }
        for (l = 0; l < 4; l++) {
            m = font_items(it + n, 160 - n, lines[l], 2);
            for (k = n; k < n + m; k++) it[k].y = (float)(l * 20);
            n += m;
        }
        memset(&td, 0, sizeof td);
        td.w = 240; td.h = 120; td.format = YGFX_RGBA16F;
        page = ygfx_target(&gfx, &td);
        d = font_run_desc(it, n, 2, 0, 0, LIGHT);
        d.place = YGFX_CENTER;
        text = ygfx_crun(&gfx, &d);
        memset(&id, 0, sizeof id);
        id.tex = page; id.place = YGFX_TOP_LEFT; id.anchor = YGFX_CENTER;
        id.x = floorf(x); id.y = floorf(y);   /* whole px: the texels land on pixels */
        img = ygfx_image(&gfx, &id);
        /* rendered once, at setup: no frame needed */
        if (ygfx_begin_setup(&gfx) != YGFX_OK || ygfx_begin_target(&gfx, page, zero) != YGFX_OK) { refused++; return; }
        put(&text);
        ygfx_end_target(&gfx);
        if (ygfx_end_setup(&gfx) != YGFX_OK) refused++;
        return;
    }
    put(&img);
}

/* --- page 8: v0.7 to v0.10: noise, simplex, rays, dither, an add layer ------ */

/* A label made at setup: one glyph run of s with its top left at x, y. */
static ygfx_stim small_label(const char* s, float x, float y, const float* color) {
    static ygfx_glyph g[24];
    ygfx_stim r;
    ygfx_buf b;
    int n = layout(g, 24, s, floorf(x), floorf(y), 2.0f);
    b = ygfx_buffer(&gfx, (size_t)n * sizeof g[0]);
    if (!b.id || ygfx_buffer_update(&gfx, b, 0, g, (size_t)n * sizeof g[0]) < 0) {
        refused++;
        memset(&r, 0, sizeof r);
        return r;
    }
    return text_run(b, n, 2.0f, YGFX_TOP_LEFT, 0, 0, WIN_W, WIN_H, color);
}
/* The x at which s, 12 px a letter, is centered on cx. */
static float centered(const char* s, float cx) { return floorf(cx - 0.5f * (float)(strlen(s) * 12 - 2)); }

/* UNIFORM and GAUSSIAN at one seed and one SD (0.15), each with the
 * histogram of the checks drawn, in half-SD bins, from the CPU twin
 * (ygfx_noise_fill: the same values, bit for bit). The bars are static:
 * drawn once into a target, in a setup pass. */
#define NZ_CHECK 2
#define NZ_N     44            /* checks a side: 88 px */
#define NZ_BINS  12            /* -3 to +3 SD */
static void t_noise_hist(float x, float y, const yscr_frame* f) {
    static ygfx_stim s[2], bars, lab[2];
    if (!f) {
        static float v[NZ_N * NZ_N];
        static const ygfx_noise_dist dist[2] = { YGFX_UNIFORM, YGFX_GAUSSIAN };
        static const float sd[2] = { 0.57735027f, 1.0f };   /* the values' SD: (-1, 1) uniform; N(0, 1) */
        static const char* const names[2] = { "UNIFORM", "GAUSSIAN" };
        static const float zero[4] = { 0, 0, 0, 0 };
        ygfx_stim bar[2 * NZ_BINS];
        ygfx_noise_desc d;
        ygfx_target_desc td;
        ygfx_image_desc id;
        ygfx_tex t;
        int k, i, b, cnt[NZ_BINS], top;
        memset(&td, 0, sizeof td);
        td.w = 240; td.h = 64; td.format = YGFX_RGBA16F;
        t = ygfx_target(&gfx, &td);
        for (k = 0; k < 2; k++) {
            float cx = x + (k ? 66.0f : -66.0f);
            memset(&d, 0, sizeof d);
            d.place = YGFX_TOP_LEFT; d.anchor = YGFX_TOP_LEFT;
            d.x = floorf(cx) - 44; d.y = floorf(y) - 96;   /* whole px: checks on pixels */
            d.w = d.h = NZ_N * NZ_CHECK; d.check = NZ_CHECK; d.seed = 8; d.dist = dist[k];
            d.contrast = 0.15f / sd[k];                   /* GAUSSIAN: contrast is the SD */
            s[k] = ygfx_noise(&d);
            ygfx_noise_fill(v, NZ_N, NZ_N, 8, dist[k]);
            memset(cnt, 0, sizeof cnt);
            for (i = 0; i < NZ_N * NZ_N; i++) {
                b = (int)floorf((v[i] / sd[k] + 3.0f) * 2.0f);
                cnt[b < 0 ? 0 : b >= NZ_BINS ? NZ_BINS - 1 : b]++;
            }
            for (top = 1, b = 0; b < NZ_BINS; b++) top = cnt[b] > top ? cnt[b] : top;
            for (b = 0; b < NZ_BINS; b++) {   /* in the target's px: 7 px a bin, 60 px the tallest */
                float h = 60.0f * (float)cnt[b] / (float)top;
                bar[k * NZ_BINS + b] = shape(YGFX_RECT, 120.0f + (k ? 66.0f : -66.0f) - 42.0f + 7.0f * (float)b + 3.5f,
                                             62.0f - 0.5f * h, 6, h, INK);
            }
            lab[k] = small_label(names[k], centered(names[k], cx), floorf(y) - 6, INK);
        }
        memset(&id, 0, sizeof id);
        id.tex = t; id.place = YGFX_TOP_LEFT; id.anchor = YGFX_TOP_LEFT;
        id.x = floorf(x) - 120; id.y = floorf(y) + 14;
        bars = ygfx_image(&gfx, &id);
        if (ygfx_begin_setup(&gfx) != YGFX_OK || ygfx_begin_target(&gfx, t, zero) != YGFX_OK) { refused++; return; }
        put_n(bar, 2 * NZ_BINS);
        ygfx_end_target(&gfx);
        if (ygfx_end_setup(&gfx) != YGFX_OK) refused++;
        return;
    }
    put_n(s, 2);
    put(&bars);
    put_n(lab, 2);
}

/* SIMPLEX fBm at 1, 2, 4 and 8 octaves, a 24 px lattice, z on a track;
 * a tile of two by two cells.
 * contrast is 0.15 / SD with the SD of each octave count (NOISE), so all
 * four have the same SD. */
static void t_simplex(float x, float y, const yscr_frame* f) {
    static ygfx_stim s[4], lab[4];
    if (!f) {
        static const int oct[4] = { 1, 2, 4, 8 };
        static const float sd[4] = { 0.425f, 0.317f, 0.261f, 0.246f };
        static const char* const names[4] = { "1 OCTAVE: SD 0.425", "2 OCTAVES: SD 0.317", "4 OCTAVES: SD 0.261",
                                              "8 OCTAVES: SD 0.246" };
        ygfx_noise_desc d;
        int k;
        for (k = 0; k < 4; k++) {
            float cx = x + (k % 2 ? 140.0f : -140.0f), cy = y + (k / 2 ? 100.0f : -110.0f);
            memset(&d, 0, sizeof d);
            d.place = YGFX_TOP_LEFT; d.x = floorf(cx); d.y = floorf(cy); d.w = d.h = 190;
            d.dist = YGFX_SIMPLEX; d.scale = 24; d.octaves = oct[k]; d.seed = 5;
            d.contrast = 0.15f / sd[k];
            s[k] = ygfx_noise(&d);
            bind_stim(&s[k], YGFX_P_NOISE_Z, CH_NOISE_Z);
            lab[k] = small_label(names[k], centered(names[k], cx), floorf(cy) + 97, INK);
        }
        return;
    }
    put_n(s, 4);
    put_n(lab, 4);
}

/* One word turning by quarter turns, drawn by default (L) and with .rays
 * (R). A run is drawn by exact area unless it is turned 0.01 degrees or
 * more from a quarter turn; then by the rays, with the same bits (RUNS).
 * The label below says which, by that rule. */
static void t_turn(float x, float y, const yscr_frame* f) {
    static ygfx_stim run[2], lab[3];
    static ygfx_citem it[2][4];
    if (!f) {
        ygfx_crun_desc d;
        int n, k;
        if (font_cset_setup() < 0) { refused++; return; }
        for (k = 1; k >= 0; k--) {   /* .rays first: it makes the rays program, which a turned run needs to take it */
            n = font_items(it[k], 4, "TURN", 4);
            d = font_run_desc(it[k], n, 4, x + (k ? 70.0f : -70.0f), y - 16, LIGHT);
            d.rays = k == 1;
            run[k] = ygfx_crun(&gfx, &d);
            bind_stim(&run[k], YGFX_P_ORI, CH_QUARTER);
        }
        lab[0] = small_label("EXACT", centered("EXACT", x - 70), y + 56, INK);
        lab[1] = small_label("RAYS", centered("RAYS", x - 70), y + 56, INK);
        lab[2] = small_label("RAYS", centered("RAYS", x + 70), y + 56, INK);
        return;
    }
    {
        float q = fmodf(fabsf(run[0].ori), 90.0f);
        put_n(run, 2);
        put(&lab[q < 0.01f || q > 89.99f ? 0 : 1]);
        put(&lab[2]);
    }
}

/* The output stage's three roundings of one shallow ramp, from code 40 to
 * 44 (dark, where a code step is 5 % of the light, so the bands show): floor(d * 255 + t) with t 0.5 (none), the Bayer 8 x 8 threshold
 * (ordered) or one frame's hash (noise), the output shader's formulas on
 * the CPU. The dither is one per gfx (desc.dither; NONE here), so the three
 * strips cannot all be the live output stage: each strip is an RGBA32F
 * image of the scene values that the output stage, with no dither, rounds
 * to the codes computed. Codes, not light. On the right, the same codes 8
 * times as far from code 42 (the mean), for viewing. A tile of two cells. */
#define DZ_W 240
#define DZ_H 36
#define DZ_C 40                /* the first code */
static float dz_value(int code) {
    /* the middle of the scene values that write `code` (ycol_output_code is
     * the output stage's CLUT and rounding on the CPU), by bisection */
    double e[2];
    int k, i;
    for (k = 0; k < 2; k++) {
        double lo = 0, hi = 1;
        for (i = 0; i < 48; i++) {
            ycol_rgb c;
            uint32_t q[3] = { 0, 0, 0 };   /* gcc -O2 cannot see the call fill it */
            c.r = c.g = c.b = 0.5 * (lo + hi);
            ycol_output_code(&cal, c, 8, q);
            if ((int)q[0] >= code + k) hi = c.r; else lo = c.r;
        }
        e[k] = hi;
    }
    return (float)(0.5 * (e[0] + e[1]));
}
static int dz_code(int mode, float r, int px, int py) {
    float t = 0.5f;
    if (mode == 1) {
        int u = px & 7, w = py & 7, z = u ^ w;
        int b = ((z & 1) << 5) | ((u & 1) << 4) | ((z & 2) << 2) | ((u & 2) << 1) | ((z & 4) >> 1) | ((u & 4) >> 2);
        t = ((float)b + 0.5f) / 64.0f;
    } else if (mode == 2) {
        uint32_t h = ygfx_hash2(px, py, 0u ^ ygfx_hash(0u));   /* frame 0, desc.seed 0 */
        t = ((float)(h >> 8) + 0.5f) * (1.0f / 16777216.0f);
    }
    return (int)floorf(r + t);
}
static void t_dither(float x, float y, const yscr_frame* f) {
    static ygfx_stim strip[6], lab[4];
    if (!f) {
        static float px[6 * DZ_H * DZ_W * 4];
        static const char* const names[3] = { "NO DITHER", "ORDERED 8X8", "NOISE, 1 FRAME" };
        float val[2][5];
        ygfx_texture_desc td;
        ygfx_image_desc id;
        ygfx_tex t;
        int k, i, j, c, x0[2];
        x0[0] = (int)floorf(x) - 148 - DZ_W / 2;
        x0[1] = (int)floorf(x) + 148 - DZ_W / 2;
        for (c = 0; c < 5; c++) {
            val[0][c] = dz_value(DZ_C + c);
            val[1][c] = dz_value(DZ_C + 2 + 8 * (c - 2));   /* x8 about the mean code */
        }
        for (k = 0; k < 6; k++) {
            int m = k % 3, y0 = (int)floorf(y) - 76 + 60 * m, xs = x0[k / 3];
            for (j = 0; j < DZ_H; j++)
                for (i = 0; i < DZ_W; i++) {
                    float r = (float)DZ_C + 4.0f * ((float)i + 0.5f) / DZ_W;   /* the code before rounding */
                    int q = dz_code(m, r, x0[0] + i, y0 + j) - DZ_C;          /* the real strip's pixels */
                    float* p = &px[((size_t)(k * DZ_H + j) * DZ_W + (size_t)i) * 4];
                    p[0] = p[1] = p[2] = val[k / 3][q < 0 ? 0 : q > 4 ? 4 : q];
                    p[3] = 1;
                }
            if (k < 3) lab[k] = small_label(names[k], (float)xs, (float)y0 - 16, INK);
        }
        lab[3] = small_label("X8 CONTRAST, TO SEE", centered("X8 CONTRAST, TO SEE", floorf(x) + 148), floorf(y) - 92, INK);
        memset(&td, 0, sizeof td);
        td.w = DZ_W; td.h = 6 * DZ_H; td.format = YGFX_RGBA32F; td.data = px;
        t = ygfx_texture(&gfx, &td);
        for (k = 0; k < 6; k++) {
            memset(&id, 0, sizeof id);
            id.tex = t; id.place = YGFX_TOP_LEFT; id.anchor = YGFX_TOP_LEFT;
            id.x = (float)x0[k / 3]; id.y = floorf(y) - 76 + 60.0f * (float)(k % 3);   /* whole px, 1:1, nearest */
            id.src[0] = 0; id.src[1] = (float)(k * DZ_H); id.src[2] = DZ_W; id.src[3] = DZ_H;
            strip[k] = ygfx_image(&gfx, &id);
        }
        return;
    }
    put_n(strip, 6);
    put_n(lab, 4);
}

/* A contrast increment as a layer: a gabor drawn once into an RGBA16F
 * target in a setup pass, then added (YGFX_STIM_ADD) over a grating (R);
 * the same grating alone (L). A tile of two by two cells. */
static void t_add_layer(float x, float y, const yscr_frame* f) {
    static ygfx_stim gr[2], img, lab[2];
    if (!f) {
        static const float zero[4] = { 0, 0, 0, 0 };
        ygfx_grating_desc d;
        ygfx_gabor_desc g;
        ygfx_target_desc td;
        ygfx_image_desc id;
        ygfx_stim gab;
        ygfx_tex t;
        int k;
        memset(&d, 0, sizeof d);
        d.place = YGFX_TOP_LEFT; d.y = floorf(y) - 10; d.w = d.h = 240;
        d.sf = 1 / 40.0f; d.ori = 90; d.contrast = 0.15f;
        for (k = 0; k < 2; k++) { d.x = floorf(x) + (k ? 140.0f : -140.0f); gr[k] = ygfx_grating(&d); }
        memset(&td, 0, sizeof td);
        td.w = td.h = 240;                                   /* RGBA16F: signed increments */
        t = ygfx_target(&gfx, &td);
        memset(&g, 0, sizeof g);
        g.place = YGFX_TOP_LEFT; g.x = 120; g.y = 120; g.sigma = 26; g.sf = 1 / 24.0f; g.ori = 45; g.contrast = 0.35f;
        gab = ygfx_gabor(&g);
        memset(&id, 0, sizeof id);
        id.tex = t; id.place = YGFX_TOP_LEFT; id.x = floorf(x) + 140; id.y = floorf(y) - 10; id.add = true;
        img = ygfx_image(&gfx, &id);
        lab[0] = small_label("GRATING", centered("GRATING", x - 140), y + 120, INK);
        lab[1] = small_label("+ GABOR", centered("+ GABOR", x + 140), y + 120, INK);
        if (ygfx_begin_setup(&gfx) != YGFX_OK || ygfx_begin_target(&gfx, t, zero) != YGFX_OK) { refused++; return; }
        put(&gab);
        ygfx_end_target(&gfx);
        if (ygfx_end_setup(&gfx) != YGFX_OK) refused++;
        return;
    }
    put_n(gr, 2);
    put(&img);
    put_n(lab, 2);
}

/* =========================================================================== *
 * Pages and layout
 * =========================================================================== */

typedef struct gallery_tile { const char* label; tile_fn fn; } gallery_tile;
/* cells: per tile, its first grid cell, columns and rows; NULL = one cell
 * each, in order */
typedef struct gallery_page { const char* name; const gallery_tile* tiles; int n; const int (*cells)[3]; } gallery_page;
#define COUNT(a) (int)(sizeof(a) / sizeof((a)[0]))

/* Labels: at most 22 characters a line, two lines. */
static const gallery_tile page1[] = {
    { "GABOR, DRIFTING 1 HZ", t_gabor_drift },
    { "GABOR, CONTRAST TWEEN\n(YOYO, COSINE EASE)", t_gabor_tween },
    { "GABOR, ASPECT 2,\nORI ON A KEYED TRACK", t_gabor_track },
    { "16 GABORS IN ONE\nDRAW_N CALL", t_gabor_array },
    { "GRATING, CIRCLE,\nCOSINE EDGE 20 PX", t_grating_circle },
    { "SQUARE WAVE, RECT,\nGAUSSIAN EDGE SD 3", t_grating_square },
    { "PLAID: TWO GRATINGS\nADD IN LINEAR LIGHT", t_plaid },
    { "USER SHADER: RADIAL\nGRATING, PHASE IN P[1]", t_user_radial },
    { "NOISE, UNIFORM,\n4 PX CHECKS", t_noise },
    { "BINARY NOISE IN A\nRING (A STROKE)", t_noise_ring },
    { "300 DOTS, THE FIELD\nTURNED BY A TRACK", t_dots },
    { "RING DOTS (A STROKE),\nRECT FIELD", t_dots_ring },
};
static const gallery_tile page2[] = {
    { "CIRCLE, ANNULUS,\nCROSS, LINE (V0.2)", t_basic },
    { "RECT, ROUNDED RECT,\nRRECT WITH 4 RADII", t_rects },
    { "ELLIPSE, UNEVEN\nCAPSULE", t_ellipse_capsule },
    { "ARC AND PIE", t_arc_pie },
    { "NGON AND STAR,\nROUNDED CORNERS", t_ngon_star },
    { "CONCAVE POLYGON:\nPLAIN, FILLETED", t_polygon },
    { "STROKE ALIGN: INSIDE,\nCENTER, OUTSIDE", t_stroke_align },
    { "JOINS: MITER, ROUND,\nBEVEL (LIMIT 1.2)", t_joins },
    { "POLYLINE JOINS: MITER,\nBEVEL, ROUND", t_polyline_joins },
    { "CAPS: BUTT, ROUND,\nSQUARE", t_caps },
    { "QUADRATIC BEZIERS,\nROUND AND BUTT CAPS", t_qbezier },
    { "OFFSET +10, 0, -10 PX:\nDILATE, ERODE", t_offset },
};
static const gallery_tile page3[] = {
    { "DASHES, OFFSET ON A\nTRACK: MARCHING", t_dash_march },
    { "DASH_SNAP, ROUND CAPS;\nBUTT CAPS", t_dash_snap },
    { "TRIM END TWEEN, ORI\nTRACK: A SPINNER", t_trim_spin },
    { "TRIM DRAWS A POLYLINE\nAND A BEZIER", t_trim_path },
    { "UNION, SUBTRACT,\nINTERSECT, XOR", t_ops },
    { "SMOOTH UNION, A\nPRIMITIVE'S X BOUND", t_smooth },
    { "ONION: OF THE RESULT,\nOF EACH PRIMITIVE", t_onion },
    { "CSG GEAR, TURNING,\nWITH A DROP SHADOW", t_gear },
    { "DROP SHADOW, GLOW", t_drop_glow },
    { "INNER SHADOW, BANDS", t_inner_bands },
    { "SHADOW: F(D) (LEFT),\nEXACT_BLUR (RIGHT)", t_exact_blur },
};
static const gallery_tile page4[] = {
    { "RED TO GREEN: RGB,\nOKLAB, DKL POLAR", t_spaces },
    { "SIX STOPS: RGB (TOP),\nOKLAB (BOTTOM)", t_six_stops },
    { "RADIAL PAINT, REPEAT,\nOKLAB", t_radial },
    { "ANGULAR PAINT, START\nON A TRACK", t_angular },
    { "VERTEX COLORS:\nTRIANGLE, HEXAGON", t_vertex },
    { "LINEAR PAINT ON A\nSTROKE, OKLAB", t_paint_stroke },
    { "BLEND OVER,\nOPACITY 0.75", t_blend_over },
    { "BLEND ADD: LIGHTS", t_blend_add },
    { "BLEND MULTIPLY:\nFILTERS ON A CARD", t_blend_multiply },
    { "GABOR ALONG DKL L-M", t_dkl_gabor },
    { "GRATING, S-CONE\nISOLATING", t_s_cone },
};
static const gallery_tile page5[] = {
    { "SPRITE 8X8 AT 16X,\nNEAREST", t_sprite_nearest },
    { "THE SAME SPRITE,\nLINEAR", t_sprite_linear },
    { "ATLAS SRC RECTS,\nTINTS", t_atlas_tint },
    { "R32F IMAGE AS A\nMODULATION", t_image_mod },
    { "MASK_TEX FROM A\nBITMAP, OUTLINED", t_mask },
    { "GRATING IN A MASK\nAPERTURE", t_mask_grating },
    { "GROUP: ORI ON A TRACK,\nSCALE TWEEN", t_group },
    { "OPACITY: GROUP (LEFT),\nFLAT TARGET (RIGHT)", t_group_vs_target },
    { "TARGET DRAWN ONCE,\nTURNED AND SCALED", t_target_turn },
    { "GABORS IN A TARGET,\nADDED EACH FRAME", t_target_add },
    { "GLYPH RUNS: OUTLINE,\nBOLD BY OFFSET", t_glyph_outline },
    { "GLYPH RUN, ORI TWEEN", t_glyph_tilt },
};
static const gallery_tile page6[] = {
    { "400 GABORS, ONE DRAW;\nHIT_INDEX OUTLINED", t_inst_field },
    { "300 DISCS: PALETTE\nAND SCALE PER ELEMENT", t_inst_palette },
    { "NV12 BT.709 LIMITED\n(L), RGBA8 (R)", t_nv12 },
    { "STRAIGHT (L), PREMULT\n(R) ALPHA, LINEAR 16X", t_alpha },
    { "GLYPH RUNS FROM ONE\nBUFFER (FIRST)", t_glyph_first },
    { "PROGRAM CACHE: OPEN\nAND SETUP", t_cache },
    { "GRATING DESC SHAPE_P:\nANNULUS APERTURE", t_grating_annulus },
};
static const gallery_tile page7[] = {
    { "STAGGERED ENTRANCE:\nY AND GATE PER LETTER", t_stagger },
    { "GLOW: R16F BLUR, SIGMA\nON A TWEEN", t_glow },
    { "ARTWORK: 3 LAYERS, A\nPALETTE; HIT_INDEX", t_artwork },
    { "STATIC TEXT IN A TARGET,\nRENDERED AT SETUP", t_cached },
};
static const gallery_tile page8[] = {
    { "UNIFORM AND GAUSSIAN,\nSD 0.15; HISTOGRAMS", t_noise_hist },
    { "TURNING TEXT: DEFAULT\n(L), .RAYS FORCED (R)", t_turn },
    { "ROUNDING OF A RAMP OVER CODES 40-44\n(CODES, NOT LIGHT)", t_dither },
    { "SIMPLEX FBM AT 1, 2, 4 AND 8 OCTAVES,\nZ ON A TRACK; CONTRAST 0.15 / SD", t_simplex },
    { "GABOR IN A TARGET, ADDED\nOVER A GRATING (STIM_ADD)", t_add_layer },
};
static const int page8_cells[][3] = { { 0, 1, 1 }, { 1, 1, 1 }, { 2, 2, 1 }, { 4, 2, 2 }, { 6, 2, 2 } };
static const gallery_page pages[] = {
    { "GRATINGS, GABORS, NOISE, DOTS", page1, COUNT(page1), NULL },
    { "SHAPES, STROKES AND JOINS", page2, COUNT(page2), NULL },
    { "DASHES, TRIM, COMPOUNDS, EFFECTS", page3, COUNT(page3), NULL },
    { "PAINT, COLOR SPACES, BLEND MODES", page4, COUNT(page4), NULL },
    { "IMAGES, MASKS, GROUPS, TARGETS, TEXT", page5, COUNT(page5), NULL },
    { "V0.4: CACHE, VIDEO, INSTANCES", page6, COUNT(page6), NULL },
    { "V0.6: CURVE RUNS, BLUR", page7, COUNT(page7), NULL },
    { "V0.7 TO V0.10: NOISE, RAYS, DITHER", page8, COUNT(page8), page8_cells },
};
#define N_PAGES COUNT(pages)

#define GRID_COLS 4
#define GRID_ROWS 3
#define HEADER    34.0f
#define MARGIN    6.0f
#define LABEL_H   40.0f
#define LABEL_PX  2.0f   /* screen px per font pixel: 10 x 14 px letters */

static float scr_w, scr_h;
static ygfx_stim panels[COUNT(pages)][GRID_COLS * GRID_ROWS];
static ygfx_stim page_text[COUNT(pages)];
static ygfx_glyph glyph_tmp[1024];

static void tile_rect(int p, int i, float* x0, float* y0, float* w, float* h) {
    const int* c = pages[p].cells ? pages[p].cells[i] : NULL;
    int cell = c ? c[0] : i;
    *w = (scr_w - 2 * MARGIN) / GRID_COLS;
    *h = (scr_h - HEADER - MARGIN) / GRID_ROWS;
    *x0 = MARGIN + (float)(cell % GRID_COLS) * *w;
    *y0 = HEADER + (float)(cell / GRID_COLS) * *h;
    if (c) { *w *= (float)c[1]; *h *= (float)c[2]; }
}

/* The center of a tile's drawing area, below its label. */
static void tile_center(int p, int i, float* x, float* y) {
    float x0, y0, w, h;
    tile_rect(p, i, &x0, &y0, &w, &h);
    *x = x0 + 0.5f * w;
    *y = y0 + LABEL_H + 0.5f * (h - LABEL_H);
}

/* One glyph run per page: its title, the keys and every tile's label. */
static int page_text_setup(int p) {
    static const char* keys = "ARROWS, SPACE OR 1-8: PAGE   SHIFT+ESC: QUIT";
    char head[160];
    float x0, y0, w, h;
    ygfx_buf b;
    int n, i, cap = COUNT(glyph_tmp);
    snprintf(head, sizeof head, "%d/%d  %s", p + 1, N_PAGES, pages[p].name);
    n = layout(glyph_tmp, cap, head, MARGIN + 6, 11, LABEL_PX);
    n += layout(glyph_tmp + n, cap - n, keys, floorf(scr_w - MARGIN - 6 - (float)(strlen(keys) * 12 - 2)), 11, LABEL_PX);
    for (i = 0; i < pages[p].n; i++) {
        tile_rect(p, i, &x0, &y0, &w, &h);
        n += layout(glyph_tmp + n, cap - n, pages[p].tiles[i].label, floorf(x0 + 12), floorf(y0 + 10), LABEL_PX);
    }
    b = ygfx_buffer(&gfx, (size_t)n * sizeof(ygfx_glyph));
    if (!b.id || ygfx_buffer_update(&gfx, b, 0, glyph_tmp, (size_t)n * sizeof(ygfx_glyph)) < 0) return -1;
    page_text[p] = text_run(b, n, LABEL_PX, YGFX_TOP_LEFT, 0, 0, scr_w, scr_h, INK);
    return 0;
}

static int big_text_setup(void) {
    static ygfx_glyph g[3];
    big_count = layout(g, 3, "YSP", 0, 0, 8);
    big_text = ygfx_buffer(&gfx, sizeof g);
    if (!big_text.id) return -1;
    return ygfx_buffer_update(&gfx, big_text, 0, g, sizeof g);
}

/* sRGB's primaries and white at gamma 2.2, and Gaussian primary spectra
 * (610, 545 and 455 nm) for the cone and DKL math. Not a measurement. */
static int cal_setup(void) {
    static const float xy[4][2] = { { 0.64f, 0.33f }, { 0.30f, 0.60f }, { 0.15f, 0.06f }, { 0.3127f, 0.3290f } };
    static const float peak[3] = { 610, 545, 455 }, width[3] = { 20, 25, 14 };
    float spd[3][81];
    char err[200];
    int i, k;
    if (ycol_cal_nominal(&cal, xy, 80.0f, 2.2) < 0) return -1;
    for (k = 0; k < 3; k++)
        for (i = 0; i < 81; i++) {
            float u = (380.0f + 5.0f * (float)i - peak[k]) / width[k];
            spd[k][i] = 0.01f * expf(-0.5f * u * u);
        }
    if (ycol_cal_set_spectra(&cal, 380, 5, 81, spd[0], spd[1], spd[2], NULL) < 0) return -1;
    if (ycol_cal_derive(&cal, err, sizeof err) < 0) { fprintf(stderr, "gfx_gallery: calibration: %s\n", err); return -1; }
    ycol_cal_save(&cal, NULL, 0);   /* the CRC over the new spectra, which open() checks */
    return 0;
}

static void show_page(int p, const yscr_frame* f) {
    char title[160];
    const char* c;
    int i;
    page_t0 = f->onset;
    ytl_anchor(&tl, PAGE_BASE, f->onset, 0);   /* every animation restarts */
    /* SDL and printf may allocate: only here, on a page change */
    snprintf(title, sizeof title, "gfx_gallery %d/%d: %s", p + 1, N_PAGES, pages[p].name);
    if (yscr_window(&scr)) SDL_SetWindowTitle(yscr_window(&scr), title);
    printf("page %d/%d: %s\n", p + 1, N_PAGES, pages[p].name);
    for (i = 0; i < pages[p].n; i++) {
        printf("  %2d  ", i + 1);
        for (c = pages[p].tiles[i].label; *c; c++) putchar(*c == '\n' ? ' ' : *c);
        putchar('\n');
    }
    fflush(stdout);
}

int main(int argc, char** argv) {
    yscr_desc sd;
    ygfx_desc gd;
    yscr_frame f;
    const char* shots = NULL;
    static ygfx_file_cache pcache;
    static char cache_dir[512];
    const ygfx_cache* cache = NULL;
    int no_cache = 0;
    uint8_t* shot_px = NULL;
    int64_t frames = -1, on_page = 0;
    int i, p, cur = 0, next, topmost = 0, sim = 0, started = 0, quit = 0, pages_done = 0;
    char line[640];

    memset(&sd, 0, sizeof sd);
    sd.windowed = true; sd.window_w = WIN_W; sd.window_h = WIN_H;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--sim")) { sd.backend = YSCR_BACKEND_SIM; sim = 1; }
        else if (!strcmp(argv[i], "--composition")) sd.backend = YSCR_BACKEND_COMPOSITION;
        else if (!strcmp(argv[i], "--topmost")) topmost = 1;
        else if (!strcmp(argv[i], "--page") && i + 1 < argc) cur = atoi(argv[++i]) - 1;
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) { frames = atoll(argv[++i]); if (frames < 1) cur = -1; }
        else if (!strcmp(argv[i], "--shots") && i + 1 < argc) shots = argv[++i];
        else if (!strcmp(argv[i], "--cache") && i + 1 < argc) { cache = ygfx_file_cache_init(&pcache, argv[++i]); if (!cache) cur = -1; }
        else if (!strcmp(argv[i], "--no-cache")) no_cache = 1;
        else cur = -1;
        if (cur < 0 || cur >= N_PAGES) {
            fprintf(stderr, "usage: gfx_gallery [--sim] [--page 1..%d] [--frames N] [--composition] [--topmost] "
                            "[--shots PREFIX] [--cache DIR] [--no-cache]\n", N_PAGES);
            return 2;
        }
    }
    /* --sim runs on ysp/rt.h's virtual clock: the same records on any host */
    if (sim) yrt_virtual_begin(0);
    /* The per-user folder unless told otherwise, so that a second run loads
     * the programs instead of compiling them (PROGRAM CACHE). */
    if (no_cache) cache = NULL;
    else if (!cache && ygfx_default_cache_dir(cache_dir, sizeof cache_dir) == YGFX_OK)
        cache = ygfx_file_cache_init(&pcache, cache_dir);
    if (cal_setup() < 0) { fprintf(stderr, "gfx_gallery: the nominal calibration failed\n"); return 1; }
#if defined(_WIN32)
    if (topmost) {   /* open() settles (ysp/screen.h v0.5.0): one input event lets this
                     * program take the foreground during open, not after it */
        INPUT in0;
        memset(&in0, 0, sizeof in0);
        in0.type = INPUT_MOUSE;
        in0.mi.dwFlags = MOUSEEVENTF_MOVE;
        SendInput(1, &in0, sizeof in0);
    }
#endif
    memset(&gd, 0, sizeof gd);
    rgb(gd.background, BG);
    gd.cal = &cal;
    gd.width = WIN_W; gd.height = WIN_H;   /* the simulated display's size */
    gd.cache = cache;                      /* NULL: compile every program */
    cache_on = cache != NULL;
    sd.on_context = start_gfx;
    sd.on_context_ctx = &gd;
    if (!yscr_open(&scr, &sd)) { fprintf(stderr, "gfx_gallery: %s\n", yscr_error(&scr)); ygfx_close(&gfx); return 1; }
    gd.screen = &scr;
    if (!ygfx_open(&gfx, &gd)) { fprintf(stderr, "gfx_gallery: %s\n", ygfx_error(&gfx)); yscr_close(&scr); return 1; }
    yscr_describe(&scr, line, sizeof line);
    printf("%s\n", line);
    ygfx_describe(&gfx, line, sizeof line);
    printf("%s\n", line);
    {
        ygfx_programs ps;
        ygfx_program_stats(&gfx, &ps);
        printf("programs at open: %u from the cache, %u compiled, %u rejected, %u stored; open %.0f ms\n", ps.loaded, ps.compiled,
               ps.rejected, ps.stored, (double)ps.open_ns * 1e-6);
    }
#if defined(_WIN32)
    if (topmost && yscr_window(&scr)) take_foreground(yscr_window(&scr));
#else
    if (topmost) printf("--topmost acts on Win32 windows; ignored here\n");
#endif

    /* Setup: every resource and stimulus of every page, before the first
     * frame; nothing in the frame loop allocates. */
    ygfx_size(&gfx, &scr_w, &scr_h);
    if (timeline_setup() < 0 || font_setup() < 0 || big_text_setup() < 0 || sprite_setup() < 0 || heart_setup() < 0 ||
        video_setup() < 0) {
        fprintf(stderr, "gfx_gallery: setup: %s\n", ygfx_error(&gfx));
        ygfx_close(&gfx);
        yscr_close(&scr);
        return 1;
    }
    for (p = 0; p < N_PAGES; p++) {
        for (i = 0; i < pages[p].n; i++) {
            float x0, y0, w, h;
            tile_rect(p, i, &x0, &y0, &w, &h);
            panels[p][i] = shape(YGFX_RECT, x0 + 0.5f * w, y0 + 0.5f * h, w - 8, h - 8, PANEL);
            panels[p][i].shape_p[1] = 10;
        }
    }
    for (p = 0; p < N_PAGES; p++) {
        for (i = 0; i < pages[p].n; i++) {
            float x, y;
            tile_center(p, i, &x, &y);
            cur_tile = pages[p].tiles[i].label;
            pages[p].tiles[i].fn(x, y, NULL);
        }
        if (page_text_setup(p) < 0) { fprintf(stderr, "gfx_gallery: labels: %s\n", ygfx_error(&gfx)); refused++; }
    }
    if (shots) shot_px = (uint8_t*)malloc((size_t)scr_w * (size_t)scr_h * 4);
    /* Each program's first draw now, not on the page that first shows it:
     * in 1 of 3 headless Chrome runs the page change that first drew new
     * kinds took a 33 ms frame (docs/web.md, "Priming"). */
    if (ygfx_prime(&gfx) < 0) { fprintf(stderr, "gfx_gallery: prime: %s\n", ygfx_error(&gfx)); refused++; }
    gallery_ready();

    next = cur;
    while (yscr_begin(&scr, &f) == YSCR_OK) {
        ytl_frame tf;
        const gallery_page* pg;
        if (yscr_window(&scr)) {
            SDL_Event ev;
            while (yscr_poll(&scr, &ev, NULL)) {   /* yscr_begin() reports Shift+Esc and close itself */
                if (ev.type == SDL_EVENT_QUIT || ev.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) quit = 1;
                else if (ev.type == SDL_EVENT_MOUSE_MOTION) { mouse_x = ev.motion.x; mouse_y = ev.motion.y; }   /* px at density 1 */
                else if (ev.type == SDL_EVENT_FINGER_DOWN)   /* a tap: right half next, left half previous */
                    next = ev.tfinger.x >= 0.5f ? (cur + 1) % N_PAGES : (cur + N_PAGES - 1) % N_PAGES;
                else if (ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat) {
                    SDL_Keycode k = ev.key.key;
                    if (k == SDLK_RIGHT || k == SDLK_DOWN || k == SDLK_SPACE || k == SDLK_PAGEDOWN) next = (cur + 1) % N_PAGES;
                    else if (k == SDLK_LEFT || k == SDLK_UP || k == SDLK_PAGEUP) next = (cur + N_PAGES - 1) % N_PAGES;
                    else if (k >= SDLK_1 && k < SDLK_1 + (SDL_Keycode)N_PAGES) next = (int)(k - SDLK_1);
                }
            }
        }
        if (!started || next != cur) { cur = next; show_page(cur, &f); on_page = 0; started = 1; }
        pg = &pages[cur];
        tf.onset = f.onset; tf.period = f.period; tf.index = f.index;
        ytl_evaluate(&tl, &tf, NULL, 0);
        ygfx_apply(binds, n_binds, ytl_values(&tl));
        yscr_mark(&scr, YSCR_PHASE_EVALUATE);

        ygfx_begin(&gfx, &f);
        cur_tile = "panels";
        put_n(panels[cur], pg->n);
        for (i = 0; i < pg->n; i++) {
            float x, y;
            tile_center(cur, i, &x, &y);
            cur_tile = pg->tiles[i].label;
            pg->tiles[i].fn(x, y, &f);
        }
        cur_tile = "labels";
        put(&page_text[cur]);
        ygfx_end(&gfx);
        on_page++;
        if (shot_px && on_page == SHOT_FRAMES) {   /* the back buffer, before the flip */
            char path[512];
            snprintf(path, sizeof path, "%s-p%d.ppm", shots, cur + 1);
            if (ygfx_read_output(&gfx, 0, 0, (int)scr_w, (int)scr_h, shot_px) < 0 ||
                ygfx_image_write(path, shot_px, (int)scr_w, (int)scr_h, 0, NULL, NULL, 0) < 0)
                fprintf(stderr, "gfx_gallery: could not write %s\n", path);
            else
                printf("wrote %s\n", path);
        }
        yscr_flip(&scr);

        if (sim || (shots && on_page == SHOT_FRAMES)) {   /* each page once */
            if (++pages_done == N_PAGES) break;
            next = (cur + 1) % N_PAGES;
        }
        if (quit || (frames > 0 && f.index + 1 >= frames)) break;
    }
    if (ygfx_clipped(&gfx)) printf("%llu draws may have left the gamut\n", (unsigned long long)ygfx_clipped(&gfx));
    free(shot_px);
    ygfx_close(&gfx);
    if (art_src.words) yol_cset_close(&art_src);
    yol_close(&ol_cx);
    yscr_close(&scr);
    if (refused) { fprintf(stderr, "gfx_gallery: %ld refused draws or setup errors\n", refused); return 1; }
    return 0;
}
