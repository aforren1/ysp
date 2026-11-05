/* gfx_text.c - Slug text from real fonts: ysp/outline.h builds the curve
 * sets at setup, ysp/gfx.h draws them.
 *
 * Six pages in a 1200 x 760 window:
 *   1  a word that grows from 6 to 600 px per em while it turns, with its
 *      size, angle and the program that draws it (exact area on quarter
 *      turns, rays between), over a ladder of small static sizes;
 *   2  the same text by exact area and by .rays, side by side, a 13x
 *      nearest-neighbor magnifier of each, the documented errors and the
 *      errors measured here at setup against yol_raster()'s exact
 *      coverage; a subpixel drift;
 *   3  outlines and faux bold from yol_stroke(), a hard shadow (the run
 *      drawn twice), a glow and a blurred-letter stimulus from the blur
 *      pass (sigma on a tween or on keys);
 *   4  a page of CJK glyphs (Microsoft YaHei) from a curve set of the glyphs
 *      it uses, or of the whole font with --whole-font,
 *      drawn directly or composited from a target filled at setup, with
 *      each mode's CPU and GPU time per frame;
 *   5  per-letter animation: a staggered entrance, a wave and an exit
 *      built with ytl_seq (GSAP's stagger), and a wave of tweens;
 *   6  an SVG made in this program, read by yol_svg() into layers of one
 *      curve set, drawn as one run; a click picks the layer under the
 *      pointer (ygfx_hit_index).
 *
 * LAYOUT IS BY ADVANCES ONLY (cmap and hmtx): no shaping, no kerning, no
 * bidi, no line breaking. That is enough for Latin labels and a grid of
 * CJK glyphs, and wrong for most scripts, so Arabic, Devanagari and the
 * like are left out here. Real text goes through Skribidi in the pack
 * tool and the player (rig_spec 5.2), which hand glyph ids and positions
 * to ysp/outline.h and ysp/gfx.h.
 *
 * Fonts: Segoe UI and Microsoft YaHei from C:/Windows/Fonts, or the files
 * given with --font and --cjk. A font that does not open is named on its
 * page and replaced by the 5 x 7 bitmap font of gfx_gallery.c made into
 * curves (capitals only), so every page runs without font files, as on a
 * CI runner. Every curve set, target and blur is made at setup and static
 * text is rendered into targets in setup passes; the frame loop allocates
 * nothing. Colors are linear light through a nominal calibration (sRGB
 * primaries, gamma 2.2): NOT a measurement of this display. Low to
 * moderate contrast and slow motion, because the display may be somebody's
 * working screen.
 *
 * Usage: gfx_text [--sim] [--page N] [--frames N] [--composition] [--topmost]
 *                 [--shots PREFIX] [--cache DIR] [--no-cache] [--font PATH]
 *                 [--cjk PATH] [--whole-font]
 *   Right, Down, Space, Page Down: next page; Left, Up, Page Up: previous;
 *   1 to 6: that page; Shift+Esc or closing the window: quit. Page keys:
 *   2: D drift on or off, the pointer moves the magnifier; 3: - and = set
 *   sigma, T back to the tween; 4: C cycles alternate, direct, cached;
 *   6: click picks a layer.
 *   --sim          the simulated display and the null backend, for CI:
 *                  draws each page once and exits
 *   --page N       start on page N (1 to 6)
 *   --frames N     stop after N frames
 *   --composition  the composition swapchain (Windows)
 *   --topmost      keep the window on top and take the foreground (Windows)
 *   --shots PREFIX show each page for 120 frames, read the output back, write
 *                  PREFIX-pN.ppm, then quit
 *   --cache DIR    keep compiled programs in DIR (ygfx_file_cache_init:
 *                  it writes files there); the default is the per-user
 *                  folder of ygfx_default_cache_dir(), when there is one
 *   --no-cache     compile every program; read and write no cache file
 *   --font PATH    the Latin font (default C:/Windows/Fonts/segoeui.ttf)
 *   --cjk PATH     the CJK font, face 0 (default C:/Windows/Fonts/msyh.ttc)
 *   --whole-font   page 4's set holds every glyph of the CJK font (YaHei:
 *                  30209, seconds and about 98 MB), not only the page's
 * Exit code: 0; 1 when the screen or the gfx did not open, or a draw or a
 * setup step was refused; 2 for a bad argument. A missing font is not an
 * error. On Windows set YSCR_ANGLE_DIR to ANGLE's directory.
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#define YSP_GFX_IMPLEMENTATION
#include "ysp/gfx.h"
#define YSP_TIMELINE_IMPLEMENTATION
#include "ysp/timeline.h"
#define YSP_OUTLINE_IMPLEMENTATION
#include "ysp/outline.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
    #define GLCALL __stdcall
#else
    #define GLCALL
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
static void take_foreground_on_window(void* w) { take_foreground((SDL_Window*)w); }
#endif

#define WIN_W 1200
#define WIN_H 760
#define S_NS YRT_S(1)
#define MS_NS (S_NS / 1000)
#define SHOT_FRAMES 120
#define COUNT(a) (int)(sizeof(a) / sizeof((a)[0]))

static yscr_screen scr;
/* SDL calls on the window go through yscr_window_call(): with the window
 * thread (ysp/screen.h, WINDOW THREAD) another thread owns the window. */
static void title_on_window(void* title) { SDL_SetWindowTitle(yscr_window(&scr), (const char*)title); }
static ygfx_gfx gfx;
static ycol_cal cal;
static ytl_timeline tl;
static ytl_event tl_events[4];     /* tracks and tweens only: no events */
static ytl_key tl_keys[4096];      /* the key arena of page 5's sequence */
static yol_ctx ol;
static void (GLCALL *glFinish_)(void);   /* page 4's GPU time; NULL on SIM */

/* Linear device RGB. The background is near mid-gray on the display. */
static const float BG[3]     = { 0.18f, 0.18f, 0.18f };
static const float DARK[3]   = { 0.05f, 0.05f, 0.05f };
static const float INK[3]    = { 0.55f, 0.55f, 0.55f };
static const float LIGHT[3]  = { 0.62f, 0.62f, 0.62f };
static const float AMBER[3]  = { 0.60f, 0.32f, 0.04f };
static const float TEAL[3]   = { 0.05f, 0.38f, 0.40f };
static const float WHITE[3]  = { 1.0f, 1.0f, 1.0f };

/* The label palette (YGFX_I_COLOR): an item's color is its index here. */
enum { C_INK, C_LIGHT, C_DIM, C_AMBER, C_TEAL, C_RED, N_COLORS };
static const float label_pal[3 * N_COLORS] = {
    0.55f, 0.55f, 0.55f,  0.66f, 0.66f, 0.66f,  0.34f, 0.34f, 0.34f,
    0.60f, 0.32f, 0.04f,  0.08f, 0.42f, 0.44f,  0.55f, 0.12f, 0.08f,
};

static long refused;
static const char* cur_what = "";
static const char* reported_what;

/* Every draw is checked: a refused draw names its page once, and the exit
 * code says so, which makes the --sim run in CI a check of every desc
 * against the header's rules. */
static void put(const ygfx_stim* s) {
    int rc = ygfx_draw(&gfx, s);
    if (rc < 0) {
        refused++;
        if (reported_what != cur_what) {
            fprintf(stderr, "gfx_text: %s: %s (%s)\n", cur_what, ygfx_strerror(rc), ygfx_error(&gfx));
            reported_what = cur_what;
        }
    }
}
static void fail(const char* what) {
    refused++;
    fprintf(stderr, "gfx_text: %s: %s\n", what, ygfx_error(&gfx));
}
static void rgb(float* dst, const float* src) { dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; }

/* --- fonts ------------------------------------------------------------------ */

/* The 5 x 7 font of gfx_gallery.c, ' ' to '_', rows top first, bit 4 the left
 * column: the fallback when a font file does not open. */
#define BM_FIRST ' '
#define BM_N     64
static const uint8_t font5x7[BM_N][7] = {
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }, { 0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04 },
    { 0x0A, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x00 }, { 0x0A, 0x0A, 0x1F, 0x0A, 0x1F, 0x0A, 0x0A },
    { 0x04, 0x0F, 0x14, 0x0E, 0x05, 0x1E, 0x04 }, { 0x18, 0x19, 0x02, 0x04, 0x08, 0x13, 0x03 },
    { 0x0C, 0x12, 0x14, 0x08, 0x15, 0x12, 0x0D }, { 0x0C, 0x04, 0x08, 0x00, 0x00, 0x00, 0x00 },
    { 0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02 }, { 0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08 },
    { 0x00, 0x04, 0x15, 0x0E, 0x15, 0x04, 0x00 }, { 0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00 },
    { 0x00, 0x00, 0x00, 0x00, 0x0C, 0x04, 0x08 }, { 0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00 },
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C }, { 0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x00 },
    { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E }, { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E },
    { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F }, { 0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E },
    { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 }, { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E },
    { 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E }, { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 },
    { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E }, { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C },
    { 0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00 }, { 0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x04, 0x08 },
    { 0x02, 0x04, 0x08, 0x10, 0x08, 0x04, 0x02 }, { 0x00, 0x00, 0x1F, 0x00, 0x1F, 0x00, 0x00 },
    { 0x08, 0x04, 0x02, 0x01, 0x02, 0x04, 0x08 }, { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04 },
    { 0x0E, 0x11, 0x17, 0x15, 0x17, 0x10, 0x0E }, { 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 },
    { 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E }, { 0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E },
    { 0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C }, { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F },
    { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10 }, { 0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F },
    { 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 }, { 0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E },
    { 0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C }, { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 },
    { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F }, { 0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11 },
    { 0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11 }, { 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E },
    { 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10 }, { 0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D },
    { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 }, { 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E },
    { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 }, { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E },
    { 0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04 }, { 0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A },
    { 0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11 }, { 0x11, 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04 },
    { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F }, { 0x0E, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0E },
    { 0x00, 0x10, 0x08, 0x04, 0x02, 0x01, 0x00 }, { 0x0E, 0x02, 0x02, 0x02, 0x02, 0x02, 0x0E },
    { 0x04, 0x0A, 0x11, 0x00, 0x00, 0x00, 0x00 }, { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F },
};

/* A font file, or the bitmap font when the file does not open. The bitmap
 * font's units are em with a font pixel of 0.1 em, so the same px per em
 * works for both. */
typedef struct tfont {
    const char* name;       /* for the page                                   */
    char        path[400];
    char        why[240];   /* why the file is not used; "" when it is        */
    uint8_t*    bytes;
    yol_font  f;
    int         real;       /* 1: the file, 0: the bitmap font                */
    uint32_t    G;          /* the glyph table's size                         */
    double      asc, dsc;   /* em, both positive                              */
    ygfx_cset set;
    uint32_t    n_built;    /* glyphs in the set                              */
    uint32_t    n_texels, n_words;   /* its arrays: 16 and 4 bytes each       */
    double      build_ms;
} tfont;

static tfont ui, cjk;       /* the Latin font, and the CJK page's             */
static ygfx_cset fx_set;  /* page 3: ui's strokes at gid, bolds at G + gid  */

static int tf_open(tfont* t, const char* path, const char* name) {
    FILE* fp;
    long n;
    char err[200];
    memset(t, 0, sizeof *t);
    t->name = name;
    snprintf(t->path, sizeof t->path, "%s", path);
    t->asc = 0.8; t->dsc = 0.2; t->G = BM_N;
    fp = fopen(path, "rb");
    if (!fp) { snprintf(t->why, sizeof t->why, "%s: %s was not found", name, path); return 0; }
    fseek(fp, 0, SEEK_END); n = ftell(fp); fseek(fp, 0, SEEK_SET);
    t->bytes = n > 0 ? (uint8_t*)malloc((size_t)n) : NULL;
    if (!t->bytes || fread(t->bytes, 1, (size_t)n, fp) != (size_t)n) {
        fclose(fp); free(t->bytes); t->bytes = NULL;
        snprintf(t->why, sizeof t->why, "%s: %s could not be read", name, path);
        return 0;
    }
    fclose(fp);
    if (yol_font_view(&t->f, t->bytes, (size_t)n, 0, err, sizeof err) < 0) {
        free(t->bytes); t->bytes = NULL;
        snprintf(t->why, sizeof t->why, "%s: %s", path, err);
        return 0;
    }
    t->real = 1;
    t->G = (uint32_t)t->f.n_glyphs;
    t->asc = (double)t->f.ascender / t->f.units_per_em;
    t->dsc = -(double)t->f.descender / t->f.units_per_em;
    return 1;
}

/* cmap; the bitmap font has capitals and ASCII punctuation only. */
static uint32_t tf_gid(const tfont* t, uint32_t c) {
    if (t->real) return yol_font_glyph_index(&t->f, c);
    if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
    return c >= BM_FIRST && c < BM_FIRST + BM_N ? c - BM_FIRST : (uint32_t)('?' - BM_FIRST);
}

/* hmtx, in em */
static double tf_adv(const tfont* t, uint32_t gid) {
    double a = 0.5, lsb;
    if (!t->real) return 0.6;
    if (yol_font_hmetrics(&t->f, gid, &a, &lsb) < 0) a = 0.5;
    return a;
}

/* Appends glyph gid at size path units per em with the pen at x, y. */
static int tf_path(const tfont* t, uint32_t gid, yol_path* p, double size, double x, double y) {
    int r, c;
    if (t->real) {
        yol_glyph_desc gd;
        memset(&gd, 0, sizeof gd);
        gd.size = size; gd.x = x; gd.y = y;
        return yol_font_glyph(&ol, &t->f, gid, p, &gd);
    }
    for (r = 0; r < 7; r++)
        for (c = 0; c < 5; c++)
            if (gid < BM_N && (font5x7[gid][r] & (0x10 >> c)))
                yol_rect(p, x + 0.1 * size * c, y + 0.1 * size * (r - 7), 0.1 * size, 0.1 * size, 0, 0);
    return yol_path_end(p);
}

/* A curve set of t's glyphs gids[0 .. n - 1] (n 0: the whole font),
 * resolved, with backward lists (the rays are faster with them). */
static int tf_build(tfont* t, const uint32_t* gids, int n) {
    yol_cset s;
    yol_cset_desc od;
    ygfx_cset_desc d;
    int64_t t0 = yrt_real_ns();
    int rc = 0, i;
    memset(&od, 0, sizeof od);
    od.n_glyphs = t->G; od.backward = true;
    if (!yol_cset_open(&s, &ol, &od)) return -1;
    if (t->real) {
        rc = yol_cset_add_font(&s, &t->f, gids, (uint32_t)n, 0);
        t->n_built = n ? (uint32_t)n : t->G;
    } else {
        yol_path p;
        yol_path_init(&p, &ol);
        for (i = 1; i < BM_N && rc == 0; i++) {
            yol_path_clear(&p);
            if (tf_path(t, (uint32_t)i, &p, 1, 0, 0) != YOL_OK || yol_cset_add(&s, (uint32_t)i, &p) != YOL_OK) rc = -1;
        }
        yol_path_free(&p);
        t->n_built = BM_N - 1;
    }
    if (rc < 0) { fprintf(stderr, "gfx_text: %s: %s\n", t->name, yol_error(&ol)); yol_cset_close(&s); return -1; }
    memset(&d, 0, sizeof d);
    d.texels = s.texels; d.n_texels = s.n_texels; d.words = s.words; d.n_words = s.n_words;
    t->set = ygfx_cset_make(&gfx, &d);
    t->build_ms = (double)(yrt_real_ns() - t0) * 1e-6;
    t->n_texels = s.n_texels; t->n_words = s.n_words;
    printf("%s: %u glyphs into a curve set in %.0f ms (%u texels, %u words)\n", t->real ? t->name : "the bitmap font",
           t->n_built, t->build_ms, s.n_texels, s.n_words);
    yol_cset_close(&s);
    return t->set.id ? 0 : -1;
}

/* Items for a line of ASCII text with the pen at x, y (px, the baseline)
 * and px per em: advances only (see the header comment). Spaces advance
 * and add no item. Returns the count written. */
static int layout(const tfont* t, ygfx_citem* it, int cap, const char* s, double x, double y, double px, int color) {
    double pen = x;
    int n = 0;
    for (; *s; s++) {
        uint32_t c = (unsigned char)*s, g;
        if (c == '\n') { pen = x; y += 1.25 * px; continue; }
        g = tf_gid(t, c);
        if (c != ' ' && n < cap) {
            memset(&it[n], 0, sizeof it[n]);
            it[n].x = (float)pen; it[n].y = (float)y; it[n].glyph = (float)g;
            it[n].gate = 1; it[n].scale = 1; it[n].contrast = 1; it[n].color = (float)color;
            n++;
        }
        pen += tf_adv(t, g) * px;
    }
    return n;
}

static double text_w(const tfont* t, const char* s, double px) {
    double w = 0, best = 0;
    for (; *s; s++) {
        if (*s == '\n') { w = 0; continue; }
        w += tf_adv(t, tf_gid(t, (unsigned char)*s)) * px;
        if (w > best) best = w;
    }
    return best;
}

/* A run whose box is the window and whose items are screen px: the same
 * coordinates in a window-sized target. */
static ygfx_crun_desc screen_desc(ygfx_cset set, const ygfx_citem* it, int n, double px, const float* color) {
    ygfx_crun_desc d;
    memset(&d, 0, sizeof d);
    d.place = YGFX_TOP_LEFT; d.anchor = YGFX_TOP_LEFT; d.w = WIN_W; d.h = WIN_H;
    d.set = set; d.items = it; d.n = n; d.size = (float)px;
    rgb(d.color, color);
    return d;
}

/* A word whose ink is centered on the screen point x, y (the items' extent
 * is the box: CENTER centers the ink). */
static ygfx_stim word_run(ygfx_cset set, const ygfx_citem* it, int n, double px, float x, float y, const float* color) {
    ygfx_crun_desc d;
    memset(&d, 0, sizeof d);
    d.place = YGFX_TOP_LEFT; d.anchor = YGFX_CENTER; d.x = x; d.y = y;
    d.set = set; d.items = it; d.n = n; d.size = (float)px;
    rgb(d.color, color);
    return ygfx_crun(&gfx, &d);
}

static ygfx_stim rect(float x0, float y0, float w, float h, const float* color, float radius) {
    ygfx_shape_desc d;
    memset(&d, 0, sizeof d);
    d.place = YGFX_TOP_LEFT; d.anchor = YGFX_TOP_LEFT;
    d.shape = YGFX_RRECT; d.x = x0; d.y = y0; d.w = w; d.h = h;
    d.shape_p[0] = d.shape_p[1] = d.shape_p[2] = d.shape_p[3] = radius;
    d.edge = YGFX_EDGE_COSINE; d.edge_width = 1.0f;
    rgb(d.color, color);
    return ygfx_shape(&d);
}

/* --- labels: static text in a window-sized target, dynamic text per frame - */

/* Three sizes, one run each, items colored from label_pal. Static labels
 * are rendered into lab_tex in a setup pass when a page is shown and
 * composited at 1:1 on whole px each frame: the run's coverage, bit for
 * bit, for 0.5 ms of a full screen instead of drawing every glyph. */
enum { TS_SMALL, TS_BODY, TS_TITLE, N_TS };
static const float ts_px[N_TS] = { 14, 16, 24 };
#define LAB_CAP 2400
#define DYN_CAP 300
static ygfx_citem lab_it[N_TS][LAB_CAP], dyn_it[N_TS][DYN_CAP];
static int lab_n[N_TS], dyn_n[N_TS];
static ygfx_stim lab_run[N_TS], dyn_run[N_TS], lab_img;
static ygfx_tex lab_tex;
static const float zero4[4] = { 0, 0, 0, 0 };

static void lab(int ts, double x, double y, int color, const char* s) {
    lab_n[ts] += layout(&ui, lab_it[ts] + lab_n[ts], LAB_CAP - lab_n[ts], s, x, y, ts_px[ts], color);
}
static void lab_right(int ts, double x, double y, int color, const char* s) { lab(ts, x - text_w(&ui, s, ts_px[ts]), y, color, s); }
static void dyn(int ts, double x, double y, int color, const char* s) {
    dyn_n[ts] += layout(&ui, dyn_it[ts] + dyn_n[ts], DYN_CAP - dyn_n[ts], s, x, y, ts_px[ts], color);
}

/* A line of dynamic text, built without printf, which may allocate: the
 * frame loop allocates nothing. */
static char dbuf[256];
static size_t dlen;
static void d_reset(void) { dlen = 0; dbuf[0] = 0; }
static void d_str(const char* s) { while (*s && dlen + 1 < sizeof dbuf) dbuf[dlen++] = *s++; dbuf[dlen] = 0; }
static void d_num(double v, int dec) {
    char tmp[40];
    int n = 0, i, neg = v < 0;
    long long q;
    double sc = 1;
    if (v != v) { d_str("nan"); return; }
    for (i = 0; i < dec; i++) sc *= 10;
    if (neg) v = -v;
    if (v > 1e12) v = 1e12;
    q = (long long)(v * sc + 0.5);
    if (neg && q) d_str("-");
    for (i = 0; i < dec; i++) { tmp[n++] = (char)('0' + q % 10); q /= 10; }
    if (dec) tmp[n++] = '.';
    do { tmp[n++] = (char)('0' + q % 10); q /= 10; } while (q && n < 38);
    while (n > 0 && dlen + 1 < sizeof dbuf) dbuf[dlen++] = tmp[--n];
    dbuf[dlen] = 0;
}

static int labels_setup(void) {
    ygfx_target_desc td;
    ygfx_image_desc id;
    ygfx_crun_desc d;
    int k;
    memset(&td, 0, sizeof td);
    td.w = WIN_W; td.h = WIN_H; td.format = YGFX_RGBA16F;
    lab_tex = ygfx_target(&gfx, &td);
    if (!lab_tex.id) return -1;
    memset(&id, 0, sizeof id);
    id.tex = lab_tex; id.place = YGFX_TOP_LEFT; id.anchor = YGFX_TOP_LEFT;
    lab_img = ygfx_image(&gfx, &id);
    for (k = 0; k < N_TS; k++) {
        /* made with every item, so ygfx_crun() makes the items' buffers;
         * count is set before each draw */
        d = screen_desc(ui.set, lab_it[k], LAB_CAP, ts_px[k], WHITE);
        d.fields = YGFX_I_COLOR; d.palette = label_pal; d.n_palette = N_COLORS;
        lab_run[k] = ygfx_crun(&gfx, &d);
        d.items = dyn_it[k]; d.n = DYN_CAP;
        dyn_run[k] = ygfx_crun(&gfx, &d);
    }
    return 0;
}

/* --- timeline channels and bindings ------------------------------------------ */

#define LET_N 13      /* "psychophysics" */
#define WAVE_N 16
enum { CH_GROW, CH_TURN, CH_DRIFT_X, CH_DRIFT_Y, CH_SIGMA,
       CH_LY, CH_LG = CH_LY + LET_N, CH_LS = CH_LG + LET_N, CH_WY = CH_LS + LET_N, CH_WC = CH_WY + WAVE_N,
       N_CH = CH_WC + WAVE_N };
enum { PAGE_BASE = 1 };

static ygfx_bind binds[128];
static int n_binds;
static int64_t page_t0;

static void bind_group(ygfx_group* g, int param, int ch) {
    memset(&binds[n_binds], 0, sizeof binds[0]);
    binds[n_binds].group = g; binds[n_binds].param = (uint16_t)param; binds[n_binds].channel = (uint16_t)ch;
    n_binds++;
}
static void bind_field(float* field, int ch) {
    memset(&binds[n_binds], 0, sizeof binds[0]);
    binds[n_binds].field = field; binds[n_binds].channel = (uint16_t)ch;
    n_binds++;
}

/* A tween that runs between two values forever from base time start, so
 * that re-anchoring the page base at 0 replays it. */
static int yoyo(int ch, float from, float to, double seconds, int ease, int64_t start) {
    ytl_tween_desc d;
    memset(&d, 0, sizeof d);
    d.from = from; d.from_set = true; d.to = to;
    d.duration_ns = ytl_ns(seconds);
    d.start_ns = start; d.start_set = true;
    d.ease = ease; d.cycles = YTL_FOREVER; d.yoyo = true;
    return ytl_tween(&tl, ch, PAGE_BASE, &d);
}

static int timeline_setup(void) {
    static float initial[N_CH];
    ytl_desc td;
    int k;
    initial[CH_GROW] = 0.06f; initial[CH_SIGMA] = 3;
    for (k = 0; k < LET_N; k++) { initial[CH_LY + k] = 50; initial[CH_LG + k] = 0; initial[CH_LS + k] = 0.6f; }
    memset(&td, 0, sizeof td);
    td.events = tl_events; td.event_capacity = COUNT(tl_events); td.n_channels = N_CH; td.initial = initial;
    td.keys = tl_keys; td.key_capacity = COUNT(tl_keys);
    if (!ytl_open(&tl, &td)) { fprintf(stderr, "gfx_text: %s\n", ytl_error(&tl)); return -1; }
    return 0;
}

static double page_seconds(const yscr_frame* f) { return (double)(f->onset - page_t0) * 1e-9; }

/* =========================================================================== *
 * Pages. Each is one function: PG_SETUP makes its stimuli once, PG_STATIC
 * adds its labels and draws its static runs into the label target (in the
 * setup pass of a page change), PG_FRAME draws a frame.
 * =========================================================================== */

enum { PG_SETUP, PG_STATIC, PG_FRAME };
static float mouse_x = -1, mouse_y = -1, click_x = -1, click_y = -1;

/* --- page 1: scale and rotate ------------------------------------------------- */

static ygfx_group grow_g;
static ygfx_stim grow_word, ladder[12];
static ygfx_citem grow_it[8], ladder_it[12][8];
static const float ladder_px[12] = { 6, 7, 8, 9, 10, 11, 12, 14, 16, 18, 20, 24 };

/* The turn rests 1 s on each quarter turn, where the run takes the exact
 * area, and moves 3 s between them, where it takes the rays (v0.8). */
static const ytl_key turn_keys[9] = {
    { 0, 0, YTL_EASE_COSINE, 0, 0 },          { 3 * S_NS, 90, YTL_EASE_LINEAR, 0, 0 },
    { 4 * S_NS, 90, YTL_EASE_COSINE, 0, 0 },  { 7 * S_NS, 180, YTL_EASE_LINEAR, 0, 0 },
    { 8 * S_NS, 180, YTL_EASE_COSINE, 0, 0 }, { 11 * S_NS, 270, YTL_EASE_LINEAR, 0, 0 },
    { 12 * S_NS, 270, YTL_EASE_COSINE, 0, 0 },{ 15 * S_NS, 360, YTL_EASE_LINEAR, 0, 0 },
    { 16 * S_NS, 360, YTL_EASE_LINEAR, 0, 0 },
};

static void page_scale(int mode, const yscr_frame* f) {
    int k;
    if (mode == PG_SETUP) {
        ygfx_group_desc gd;
        ygfx_crun_desc d;
        ytl_track tr;
        int n;
        /* The run is made at 100 px per em and the group scales it: a group's
         * scale multiplies what is in units, the size and the pen positions
         * alike, so one channel grows the whole word about its center. */
        memset(&gd, 0, sizeof gd);
        gd.place = YGFX_CENTER; gd.y = 20; gd.scale = 0.06f;
        grow_g = ygfx_group_make(&gd);
        n = layout(&ui, grow_it, 8, "Slug", 0, 0, 100, 0);
        memset(&d, 0, sizeof d);
        d.place = YGFX_CENTER; d.anchor = YGFX_CENTER; d.set = ui.set; d.items = grow_it; d.n = n; d.size = 100;
        d.group = &grow_g;
        rgb(d.color, LIGHT);
        grow_word = ygfx_crun(&gfx, &d);
        bind_group(&grow_g, YGFX_G_SCALE, CH_GROW);
        bind_group(&grow_g, YGFX_G_ORI, CH_TURN);
        /* a geometric sweep, 6 to 600 px per em and back (YTL_EASE_LOG) */
        if (yoyo(CH_GROW, 0.06f, 6.0f, 7.0, YTL_EASE_LOG, 0) < 0) fail("page 1: the size tween");
        memset(&tr, 0, sizeof tr);
        tr.keys = turn_keys; tr.n_keys = COUNT(turn_keys); tr.period_ns = 16 * S_NS;
        if (ytl_set_track(&tl, CH_TURN, PAGE_BASE, &tr) < 0) fail("page 1: the turn track");
        /* the ladder: small sizes, static, so they go into the label target */
        {
            double x = 40;
            for (k = 0; k < COUNT(ladder_px); k++) {
                n = layout(&ui, ladder_it[k], 8, "Slug", x, WIN_H - 92, ladder_px[k], 0);
                d = screen_desc(ui.set, ladder_it[k], n, ladder_px[k], INK);
                ladder[k] = ygfx_crun(&gfx, &d);
                x += text_w(&ui, "Slug", ladder_px[k]) + 34;
            }
        }
        return;
    }
    if (mode == PG_STATIC) {
        double x = 40;
        char s[16];
        lab(TS_BODY, 40, 100, C_INK, "One run at 100 px per em in a group whose scale and ori are timeline channels:");
        lab(TS_BODY, 40, 122, C_INK, "the size sweeps 6 to 600 px per em geometrically (YTL_EASE_LOG), the turn rests on each quarter turn.");
        lab(TS_SMALL, 40, WIN_H - 122, C_DIM, "Static, unturned, exact area, rendered once into the label target:");
        for (k = 0; k < COUNT(ladder_px); k++) {
            put(&ladder[k]);
            snprintf(s, sizeof s, "%g", (double)ladder_px[k]);
            lab(TS_SMALL, x, WIN_H - 70, C_DIM, s);
            x += text_w(&ui, "Slug", ladder_px[k]) + 34;
        }
        return;
    }
    put(&grow_word);
    {
        double deg = fmod((double)grow_g.ori, 360.0), q = fmod(fabs(deg), 90.0);
        if (deg < 0) deg += 360;
        d_reset();
        d_str("size "); d_num(100.0 * grow_g.scale, 1); d_str(" px per em      angle "); d_num(deg, 1);
        d_str(" deg      program: ");
        /* the header's rule (CURVE RUNS): 0.01 degrees from a quarter turn */
        d_str(q <= 0.01 || q >= 89.99 ? "exact area (a quarter turn)" : "rays (turned)");
        dyn(TS_BODY, 40, 160, C_AMBER, dbuf);
    }
    (void)f;
}

/* --- page 2: exact area and rays ---------------------------------------------- */

#define CMP_W 1140
#define CMP_H 152
#define CMP_HALF 570
#define MAG_W 40
#define MAG_H 24
#define MAG_Z 13
#define CMP_X 30
#define CMP_Y 80
#define MAG_Y 252
static const float cmp_px[4] = { 8, 12, 24, 48 };
static const float cmp_base[4] = { 18, 40, 78, 136 };
static const char* const cmp_text[4] = {
    "Hamburgefonstiv 0123456789 &@ (exact) [rays] {area} quick brown fox",
    "Hamburgefonstiv 0123456789 &@ quick brown fox",
    "Hamburgefonstiv 0123",
    "Rag&8 Wy",
};
static ygfx_citem cmp_it[2][4][80];
static ygfx_stim cmp_run[2][4], cmp_img, mag_img[2], mag_mark[2], cmp_panel, mag_panel[2];
static ygfx_group drift_g;
static ygfx_tex cmp_tex;
static int cmp_f32, drift_on = 1, cmp_measured;
static double cmp_err[4][4];   /* per size: exact-area max, rays mean, rays max, edge pixels */

/* Both runs drawn into the target at setup, read back, and compared with
 * yol_raster()'s exact coverage in double. Each glyph is rasterized
 * alone and composited as the GPU blends draws (a + b (1 - a)): where two
 * neighbors share a pixel, the union's area is not what OVER gives. */
static void cmp_measure(void) {
    float* px = (float*)malloc((size_t)CMP_W * CMP_H * 4 * sizeof(float));
    double* ref = (double*)malloc((size_t)CMP_HALF * CMP_H * sizeof(double));
    double* one = (double*)malloc((size_t)CMP_HALF * CMP_H * sizeof(double));
    yol_path p;
    int r, k, x, y;
    if (!px || !ref || !one) { free(px); free(ref); free(one); return; }
    if (ygfx_begin_setup(&gfx) != YGFX_OK || ygfx_begin_target(&gfx, cmp_tex, zero4) != YGFX_OK) {
        fail("page 2: setup pass"); free(px); free(ref); free(one); return;
    }
    for (k = 0; k < 2; k++) for (r = 0; r < 4; r++) put(&cmp_run[k][r]);
    ygfx_end_target(&gfx);
    if (ygfx_end_setup(&gfx) != YGFX_OK) fail("page 2: setup pass");
    if (ygfx_read_target(&gfx, cmp_tex, 0, 0, CMP_W, CMP_H, px) != YGFX_OK) {   /* SIM: no pixels */
        free(px); free(ref); free(one);
        return;
    }
    yol_path_init(&p, &ol);
    for (r = 0; r < 4; r++) {
        int y0 = (int)(cmp_base[r] - 1.05 * cmp_px[r]) - 1, y1 = (int)(cmp_base[r] + 0.32 * cmp_px[r]) + 2, n = 0, bad = 0;
        double emax = 0, rsum = 0, rmax = 0;
        if (y0 < 0) y0 = 0;
        if (y1 > CMP_H) y1 = CMP_H;
        memset(ref, 0, (size_t)CMP_HALF * CMP_H * sizeof(double));
        for (k = 0; k < 80 && cmp_it[0][r][k].gate == 1 && !bad; k++) {
            yol_raster_desc rd;
            yol_box b;
            int bw, bh;
            yol_path_clear(&p);
            tf_path(&ui, (uint32_t)cmp_it[0][r][k].glyph, &p, cmp_px[r], cmp_it[0][r][k].x, cmp_it[0][r][k].y);
            if (!p.n_contours) continue;
            memset(&rd, 0, sizeof rd);
            rd.scale = 1; rd.format = YOL_ALPHA_F64;
            if (yol_raster(&ol, &p, &rd, &b) < 0) { bad = 1; break; }
            if (b.x0 < 0) b.x0 = 0;
            if (b.y0 < 0) b.y0 = 0;
            if (b.x1 > CMP_HALF) b.x1 = CMP_HALF;
            if (b.y1 > CMP_H) b.y1 = CMP_H;
            bw = b.x1 - b.x0; bh = b.y1 - b.y0;
            if (bw <= 0 || bh <= 0) continue;
            rd.x = -b.x0; rd.y = -b.y0; rd.out = one; rd.w = bw; rd.h = bh; rd.stride = bw * (int)sizeof(double);
            if (yol_raster(&ol, &p, &rd, NULL) < 0) { bad = 1; break; }
            for (y = 0; y < bh; y++)
                for (x = 0; x < bw; x++) {
                    double* d = &ref[(size_t)(y + b.y0) * CMP_HALF + (size_t)(x + b.x0)];
                    *d += one[(size_t)y * bw + x] * (1 - *d);
                }
        }
        if (bad) { fprintf(stderr, "gfx_text: page 2: %s\n", yol_error(&ol)); refused++; continue; }
        for (y = y0; y < y1; y++)
            for (x = 0; x < CMP_HALF; x++) {
                double c = ref[(size_t)y * CMP_HALF + x];
                double a = px[((size_t)y * CMP_W + x) * 4 + 3], e = px[((size_t)y * CMP_W + x + CMP_HALF) * 4 + 3];
                if (fabs(a - c) > emax) emax = fabs(a - c);
                if (fabs(e - c) > rmax) rmax = fabs(e - c);
                if (c > 0 && c < 1) { rsum += fabs(e - c); n++; }
            }
        cmp_err[r][0] = emax; cmp_err[r][1] = n ? rsum / n : 0; cmp_err[r][2] = rmax; cmp_err[r][3] = n;
        printf("page 2, %g px per em: exact area max %.2g, rays mean %.3f max %.3f over %d edge pixels\n", (double)cmp_px[r], emax,
               cmp_err[r][1], rmax, n);
    }
    yol_path_free(&p);
    cmp_measured = 1;
    free(px); free(ref); free(one);
}

static void page_exact(int mode, const yscr_frame* f) {
    int k, r;
    if (mode == PG_SETUP) {
        ygfx_group_desc gd;
        ygfx_target_desc td;
        ygfx_image_desc id;
        ygfx_shape_desc sd;
        memset(&gd, 0, sizeof gd);
        gd.place = YGFX_TOP_LEFT;
        drift_g = ygfx_group_make(&gd);
        for (k = 0; k < 2; k++)
            for (r = 0; r < 4; r++) {
                ygfx_crun_desc d;
                int n = layout(&ui, cmp_it[k][r], 79, cmp_text[r], 8 + k * CMP_HALF, cmp_base[r], cmp_px[r], 0);
                while (n > 0 && cmp_it[k][r][n - 1].x + 0.8 * cmp_px[r] > (k + 1) * CMP_HALF - 4) n--;   /* stay in the half */
                cmp_it[k][r][n].gate = 0;   /* the end, for the measurement */
                memset(&d, 0, sizeof d);
                d.place = YGFX_TOP_LEFT; d.anchor = YGFX_TOP_LEFT; d.w = CMP_W; d.h = CMP_H;
                d.set = ui.set; d.items = cmp_it[k][r]; d.n = n; d.size = cmp_px[r]; d.group = &drift_g;
                d.rays = k == 1;
                rgb(d.color, WHITE);
                cmp_run[k][r] = ygfx_crun(&gfx, &d);
            }
        bind_group(&drift_g, YGFX_G_X, CH_DRIFT_X);
        bind_group(&drift_g, YGFX_G_Y, CH_DRIFT_Y);
        if (yoyo(CH_DRIFT_X, -0.5f, 0.5f, 2.3, YTL_EASE_COSINE, 0) < 0 || yoyo(CH_DRIFT_Y, -0.5f, 0.5f, 3.7, YTL_EASE_COSINE, 0) < 0)
            fail("page 2: the drift tweens");
        /* RGBA32F keeps the coverage to f32 for the measurement; RGBA16F
         * where the renderer cannot blend into it */
        memset(&td, 0, sizeof td);
        td.w = CMP_W; td.h = CMP_H; td.format = YGFX_RGBA32F;
        cmp_tex = ygfx_target(&gfx, &td);
        cmp_f32 = cmp_tex.id != 0;
        if (!cmp_tex.id) { td.format = YGFX_RGBA16F; cmp_tex = ygfx_target(&gfx, &td); }
        if (!cmp_tex.id) { fail("page 2: target"); return; }
        memset(&id, 0, sizeof id);
        id.tex = cmp_tex; id.place = YGFX_TOP_LEFT; id.anchor = YGFX_TOP_LEFT; id.x = CMP_X; id.y = CMP_Y;
        rgb(id.tint, LIGHT); id.tint[3] = 1;
        cmp_img = ygfx_image(&gfx, &id);
        for (k = 0; k < 2; k++) {
            id.x = (float)(CMP_X + k * CMP_HALF); id.y = MAG_Y;
            id.src[0] = 30; id.src[1] = 50; id.src[2] = MAG_W; id.src[3] = MAG_H;
            id.w = MAG_W * MAG_Z; id.h = MAG_H * MAG_Z;
            mag_img[k] = ygfx_image(&gfx, &id);   /* nearest: one block per pixel */
            mag_panel[k] = rect((float)(CMP_X + k * CMP_HALF), MAG_Y, MAG_W * MAG_Z, MAG_H * MAG_Z, DARK, 0);
            memset(&sd, 0, sizeof sd);
            sd.place = YGFX_TOP_LEFT; sd.anchor = YGFX_TOP_LEFT; sd.shape = YGFX_RECT; sd.w = MAG_W + 2; sd.h = MAG_H + 2;
            sd.stroke = 1; sd.stroke_align = YGFX_STROKE_INSIDE; sd.edge = YGFX_EDGE_COSINE; sd.edge_width = 1;
            rgb(sd.color, AMBER);
            mag_mark[k] = ygfx_shape(&sd);
        }
        cmp_panel = rect(CMP_X - 6, CMP_Y - 6, CMP_W + 12, CMP_H + 12, DARK, 6);
        cmp_measure();
        return;
    }
    if (mode == PG_STATIC) {
        char s[200];
        lab(TS_BODY, CMP_X, 72, C_LIGHT, "exact area (the default: resolved glyphs, not turned)");
        lab(TS_BODY, CMP_X + CMP_HALF, 72, C_LIGHT, "crun_desc.rays: two rays from each pixel center");
        lab(TS_SMALL, CMP_X, MAG_Y + MAG_H * MAG_Z + 18, C_DIM, "13x, nearest: one block per pixel of the target above (8, 12, 24, 48 px per em)");
        if (cmp_measured) {
            snprintf(s, sizeof s, "Measured here at setup against yol_raster(), exact in double (%s, no drift):",
                     cmp_f32 ? "RGBA32F" : "RGBA16F: to about 1e-3");
            lab(TS_SMALL, CMP_X, 616, C_INK, s);
            for (r = 0; r < 4; r++) {
                snprintf(s, sizeof s, "%2g px per em:   exact area max %.1e     rays: mean %.3f, max %.2f (%.0f edge pixels)",
                         (double)cmp_px[r], cmp_err[r][0], cmp_err[r][1], cmp_err[r][2], cmp_err[r][3]);
                lab(TS_SMALL, CMP_X + 16, 634 + 17 * r, C_INK, s);
            }
        } else {
            lab(TS_SMALL, CMP_X, 616, C_INK, "Not measured: no pixels to read back (the simulated display draws nothing).");
        }
        /* the documented rows come from the test's synthetic glyphs, so a
         * different number on a real font here is not a regression */
        lab(TS_SMALL, 720, 616, C_DIM, "Documented in ysp/gfx.h, on the test's synthetic");
        lab(TS_SMALL, 736, 634, C_DIM, "glyphs across three renderers, not on this font:");
        lab(TS_SMALL, 736, 651, C_DIM, "exact area within 1.8e-5; rays mean edge error 0.029,");
        lab(TS_SMALL, 736, 668, C_DIM, "0.019, 0.009, 0.004 at 8, 12, 24, 48 px per em; up");
        lab(TS_SMALL, 736, 685, C_DIM, "to 0.56 at corners, jumps of up to 0.44 where a ray");
        lab(TS_SMALL, 736, 702, C_DIM, "passes a corner. A real font's mean may differ.");
        return;
    }
    if (!drift_on) drift_g.x = drift_g.y = 0;
    if (ygfx_begin_target(&gfx, cmp_tex, zero4) == YGFX_OK) {
        for (k = 0; k < 2; k++) for (r = 0; r < 4; r++) put(&cmp_run[k][r]);
        ygfx_end_target(&gfx);
    } else {
        fail("page 2: target pass");
    }
    put(&cmp_panel);
    put(&cmp_img);
    {   /* the lens: under the pointer on either half, else a fixed spot on the 24 px row */
        float cx = 46, cy = 66;
        if (mouse_x >= CMP_X && mouse_x < CMP_X + CMP_W && mouse_y >= CMP_Y && mouse_y < CMP_Y + CMP_H) {
            cx = fmodf(mouse_x - CMP_X, (float)CMP_HALF); cy = mouse_y - CMP_Y;
        }
        cx = floorf(cx - MAG_W / 2); cy = floorf(cy - MAG_H / 2);
        if (cx < 0) cx = 0;
        if (cx > CMP_HALF - MAG_W) cx = CMP_HALF - MAG_W;
        if (cy < 0) cy = 0;
        if (cy > CMP_H - MAG_H) cy = CMP_H - MAG_H;
        for (k = 0; k < 2; k++) {
            put(&mag_panel[k]);
            mag_img[k].src[0] = cx + (float)(k * CMP_HALF); mag_img[k].src[1] = cy;
            put(&mag_img[k]);
            mag_mark[k].x = CMP_X + mag_img[k].src[0] - 1; mag_mark[k].y = CMP_Y + cy - 1;
            put(&mag_mark[k]);
        }
    }
    d_reset();
    d_str(drift_on ? "drift on: the group moves both runs by up to half a pixel, x " : "drift off (D): x ");
    d_num(drift_g.x, 2); d_str(" px, y "); d_num(drift_g.y, 2); d_str(" px");
    dyn(TS_SMALL, CMP_X + CMP_HALF, MAG_Y + MAG_H * MAG_Z + 18, C_DIM, dbuf);
    (void)f;
}

/* --- page 3: outlines, bold, shadow, glow, blur -------------------------------- */

static const char* const W_OUT = "Outline";
static const char* const W_BOLD = "Weight";
static ygfx_citem fx_it[6][16];
static ygfx_stim fx_out, fx_fill, fx_out2, fx_reg, fx_bold, fx_shadow, fx_word, fx_glow_word, fx_white, bl_white;
static ygfx_blur glow, blurl;
static float sigma_manual;   /* > 0: the keys set sigma, not the tween */

/* Strokes and bolds of the glyphs of W_OUT and W_BOLD, at ids gid and G +
 * gid of one set. Each glyph is resolved first, so the stroke follows its
 * outline and not overlapping contours (or the bitmap font's squares). */
static int fx_setup(void) {
    yol_cset s;
    yol_cset_desc od;
    yol_path g, o;
    ygfx_cset_desc d;
    static unsigned char seen[65536];
    const char* words[2] = { W_OUT, W_BOLD };
    int w, rc = 0;
    int64_t t0 = yrt_real_ns();
    memset(&od, 0, sizeof od);
    od.n_glyphs = 2 * ui.G; od.backward = true;
    if (ui.G > 32768 || !yol_cset_open(&s, &ol, &od)) return -1;
    yol_path_init(&g, &ol); yol_path_init(&o, &ol);
    for (w = 0; w < 2 && rc == 0; w++) {
        const char* c;
        for (c = words[w]; *c && rc == 0; c++) {
            uint32_t gid = tf_gid(&ui, (unsigned char)*c);
            yol_stroke_desc sd;
            if (seen[gid] & (1 << w)) continue;
            seen[gid] |= (unsigned char)(1 << w);
            yol_path_clear(&g);
            if (tf_path(&ui, gid, &g, 1, 0, 0) < 0 || yol_resolve(&ol, &g, &g) < 0) { rc = -1; break; }
            memset(&sd, 0, sizeof sd);
            /* outlined text: CSS -webkit-text-stroke: 0.035em; faux bold:
             * the fill and a 0.045 em stroke, 0.0225 em out on each side */
            sd.width = w == 0 ? 0.035 : 0.045;
            sd.mode = w == 0 ? YOL_STROKE : YOL_BOLD;
            yol_path_clear(&o);
            if (yol_stroke(&ol, &g, &o, &sd) < 0 || yol_cset_add(&s, w == 0 ? gid : ui.G + gid, &o) < 0) rc = -1;
        }
    }
    yol_path_free(&g); yol_path_free(&o);
    if (rc < 0) { fprintf(stderr, "gfx_text: page 3 strokes: %s\n", yol_error(&ol)); yol_cset_close(&s); return -1; }
    memset(&d, 0, sizeof d);
    d.texels = s.texels; d.n_texels = s.n_texels; d.words = s.words; d.n_words = s.n_words;
    fx_set = ygfx_cset_make(&gfx, &d);
    printf("page 3: strokes and bolds of %d glyphs in %.0f ms\n", (int)(strlen(W_OUT) + strlen(W_BOLD)),
           (double)(yrt_real_ns() - t0) * 1e-6);
    yol_cset_close(&s);
    return fx_set.id ? 0 : -1;
}

static void page_effects(int mode, const yscr_frame* f) {
    const float L = 300, R = 900, Y1 = 150, Y2 = 280, Y3 = 420, Y4 = 560, PX = 72;
    if (mode == PG_SETUP) {
        ygfx_blur_desc bd;
        ygfx_crun_desc d;
        int n, k;
        if (fx_setup() < 0) { fail("page 3: the stroke set"); return; }
        n = layout(&ui, fx_it[0], 16, W_OUT, 0, 0, PX, 0);
        fx_fill = word_run(ui.set, fx_it[0], n, PX, R, Y1, TEAL);
        for (k = 0; k < n; k++) fx_it[1][k] = fx_it[0][k];   /* the same pen positions, stroke glyphs */
        fx_out = word_run(fx_set, fx_it[1], n, PX, L, Y1, LIGHT);
        fx_out2 = word_run(fx_set, fx_it[1], n, PX, R, Y1, LIGHT);
        n = layout(&ui, fx_it[2], 16, W_BOLD, 0, 0, PX, 0);
        fx_reg = word_run(ui.set, fx_it[2], n, PX, L, Y2, LIGHT);
        for (k = 0; k < n; k++) { fx_it[3][k] = fx_it[2][k]; fx_it[3][k].glyph += (float)ui.G; }
        fx_bold = word_run(fx_set, fx_it[3], n, PX, R, Y2, LIGHT);
        n = layout(&ui, fx_it[4], 16, "Shadow", 0, 0, PX, 0);
        fx_shadow = word_run(ui.set, fx_it[4], n, PX, L + 4, Y3 + 4, DARK);
        fx_word = word_run(ui.set, fx_it[4], n, PX, L, Y3, LIGHT);
        n = layout(&ui, fx_it[5], 16, "Glow", 0, 0, PX, 0);
        fx_glow_word = word_run(ui.set, fx_it[5], n, PX, L, Y4, DARK);
        /* the glow: the word white in an R16F layer, blurred once, at setup */
        memset(&bd, 0, sizeof bd);
        bd.w = 320; bd.h = 150; bd.format = YGFX_R16F; bd.sigma = 6;
        if (ygfx_blur_make(&gfx, &glow, &bd) != YGFX_OK) { fail("page 3: glow"); return; }
        memset(&d, 0, sizeof d);
        d.place = YGFX_CENTER; d.anchor = YGFX_CENTER; d.set = ui.set; d.items = fx_it[5]; d.n = n; d.size = PX;
        rgb(d.color, WHITE);
        fx_white = ygfx_crun(&gfx, &d);
        /* the blurred letters: Sloan letters drawn once, blurred each frame */
        {
            static ygfx_citem sl[8];
            int m = layout(&ui, sl, 8, "DKNRZ", 0, 0, 96, 0);
            bd.w = 520; bd.h = 200; bd.sigma = 3;
            if (ygfx_blur_make(&gfx, &blurl, &bd) != YGFX_OK) { fail("page 3: blur"); return; }
            d.items = sl; d.n = m; d.size = 96;
            bl_white = ygfx_crun(&gfx, &d);
        }
        if (ygfx_begin_setup(&gfx) != YGFX_OK) { fail("page 3: setup pass"); return; }
        if (ygfx_begin_target(&gfx, glow.layer, zero4) == YGFX_OK) { put(&fx_white); ygfx_end_target(&gfx); }
        if (ygfx_blur_apply(&gfx, &glow) != YGFX_OK) fail("page 3: glow blur");
        if (ygfx_begin_target(&gfx, blurl.layer, zero4) == YGFX_OK) { put(&bl_white); ygfx_end_target(&gfx); }
        if (ygfx_end_setup(&gfx) != YGFX_OK) fail("page 3: setup pass");
        glow.image.place = YGFX_TOP_LEFT; glow.image.ax = glow.image.ay = 0.5f; glow.image.x = L; glow.image.y = Y4;
        rgb(glow.image.tint, AMBER);
        blurl.image.place = YGFX_TOP_LEFT; blurl.image.ax = blurl.image.ay = 0.5f; blurl.image.x = R; blurl.image.y = 490;
        rgb(blurl.image.tint, LIGHT);
        bind_field(&blurl.sigma, CH_SIGMA);
        if (yoyo(CH_SIGMA, 0.5f, 10.0f, 3.0, YTL_EASE_COSINE, 0) < 0) fail("page 3: the sigma tween");
        return;
    }
    if (mode == PG_STATIC) {
        lab(TS_SMALL, L - 250, Y1 + 50, C_DIM, "yol_stroke(YOL_STROKE, 0.035 em): the band as a fill");
        lab(TS_SMALL, R - 250, Y1 + 50, C_DIM, "the same band over the fill: an outlined letter");
        lab(TS_SMALL, L - 250, Y2 + 50, C_DIM, "regular");
        lab(TS_SMALL, R - 250, Y2 + 50, C_DIM, "yol_stroke(YOL_BOLD, 0.045 em): faux bold");
        lab(TS_SMALL, L - 250, Y3 + 50, C_DIM, "a hard shadow: the run drawn twice, 4 px apart");
        lab(TS_SMALL, L - 250, Y4 + 62, C_DIM, "a glow: an R16F layer blurred once at setup, sigma 6 px");
        lab(TS_SMALL, R - 250, 620, C_DIM, "a blurred-letter stimulus: Sloan letters drawn once into");
        lab(TS_SMALL, R - 250, 637, C_DIM, "an R16F layer, blurred each frame (Gaussian, linear light)");
        lab(TS_SMALL, 40, 690, C_DIM, "Curve runs refuse edges, strokes and fx: outlines and bold are fills from ysp/outline.h, soft edges come from the blur pass.");
        return;
    }
    put(&fx_fill); put(&fx_out); put(&fx_out2);
    put(&fx_reg); put(&fx_bold);
    put(&fx_shadow); put(&fx_word);
    put(&glow.image); put(&fx_glow_word);
    if (sigma_manual > 0) blurl.sigma = sigma_manual;
    if (ygfx_blur_apply(&gfx, &blurl) != YGFX_OK) fail("page 3: blur");
    put(&blurl.image);
    d_reset();
    d_str("sigma "); d_num(blurl.sigma, 2); d_str(sigma_manual > 0 ? " px (keys - and =; T: the tween)" : " px (a tween; keys - and = set it)");
    dyn(TS_BODY, R - 250, 380, C_AMBER, dbuf);
    (void)f;
}

/* --- page 4: a page of CJK, cached and direct ----------------------------------- */

#define CJK_PX 16
#define CJK_LH 21
#define CJK_X0 24
#define CJK_Y0 112
#define CJK_X1 1176
#define CJK_Y1 620
static ygfx_citem cjk_items[4000];
static int cjk_n;
static ygfx_buf cjk_buf;
static ygfx_stim cjk_run, cjk_shifted, cjk_img;
static ygfx_tex cjk_tex;
static int cjk_mode;          /* 0 alternate, 1 direct, 2 cached */
static int cjk_drawn_cached;  /* what this frame drew */
static double cjk_ms[2][2][64];   /* [direct, cached][cpu, gpu] ring */
static int cjk_cnt[2];
static double cjk_sum[2][2];
static long cjk_total[2];
static double cjk_setup_ms[2];   /* the text alone, timed at setup */
static int cjk_setup_n;

static int cjk_whole;         /* --whole-font: every glyph of the font in the set */

/* The page's items by cmap and hmtx alone, so the set can be built from
 * the glyphs they name: CJK from U+4E00, or capitals of the Latin set. */
static void cjk_layout(const tfont* t, int han) {
    double x = CJK_X0, y = CJK_Y0 + CJK_PX;
    uint32_t cp = han ? 0x4E00 : 'A';
    long tries = 0;
    cjk_n = 0;
    while (cjk_n < COUNT(cjk_items) && tries++ < 100000) {   /* a font with few of them ends too */
        uint32_t g = tf_gid(t, cp);
        double a;
        if (han) { if (++cp > 0x9FA5) cp = 0x4E00; }
        else if (++cp > 'Z') cp = 'A';
        if (!g) continue;
        a = tf_adv(t, g) * CJK_PX;
        if (x + a > CJK_X1) { x = CJK_X0; y += CJK_LH; }
        if (y > CJK_Y1) break;
        memset(&cjk_items[cjk_n], 0, sizeof cjk_items[0]);
        cjk_items[cjk_n].x = (float)x; cjk_items[cjk_n].y = (float)y; cjk_items[cjk_n].glyph = (float)g;
        cjk_n++;
        x += a;
    }
}

static void page_cjk(int mode, const yscr_frame* f) {
    if (mode == PG_SETUP) {
        const tfont* t = cjk.set.id ? &cjk : &ui;
        ygfx_crun_desc d;
        ygfx_target_desc td;
        ygfx_image_desc id;
        if (!cjk.set.id) cjk_layout(&ui, 0);   /* else fonts_setup() laid it out */
        /* a page in a buffer, uploaded once: no copy per frame */
        cjk_buf = ygfx_buffer(&gfx, (size_t)cjk_n * sizeof(ygfx_citem));
        if (!cjk_buf.id || ygfx_buffer_update(&gfx, cjk_buf, 0, cjk_items, (size_t)cjk_n * sizeof(ygfx_citem)) < 0) { fail("page 4: buffer"); return; }
        d = screen_desc(t->set, NULL, cjk_n, CJK_PX, INK);
        d.buf = cjk_buf;
        cjk_run = ygfx_crun(&gfx, &d);
        /* the same run moved so the page's top-left is the target's */
        d.x = -CJK_X0; d.y = -(CJK_Y0 - 6);
        cjk_shifted = ygfx_crun(&gfx, &d);
        memset(&td, 0, sizeof td);
        td.w = CJK_X1 - CJK_X0 + 4; td.h = CJK_Y1 - CJK_Y0 + 12; td.format = YGFX_RGBA16F;
        cjk_tex = ygfx_target(&gfx, &td);
        if (!cjk_tex.id) { fail("page 4: target"); return; }
        memset(&id, 0, sizeof id);
        id.tex = cjk_tex; id.place = YGFX_TOP_LEFT; id.anchor = YGFX_TOP_LEFT; id.x = CJK_X0; id.y = CJK_Y0 - 6;   /* whole px */
        cjk_img = ygfx_image(&gfx, &id);
        if (ygfx_begin_setup(&gfx) != YGFX_OK || ygfx_begin_target(&gfx, cjk_tex, zero4) != YGFX_OK) { fail("page 4: setup pass"); return; }
        put(&cjk_shifted);
        ygfx_end_target(&gfx);
        if (ygfx_end_setup(&gfx) != YGFX_OK) fail("page 4: setup pass");
        return;
    }
    if (mode == PG_STATIC) {
        char s[720];
        lab(TS_SMALL, 40, 70, C_DIM, "Cached: rendered once at setup into an RGBA16F target, composited at 1:1 on whole px: the same pixels, bit for bit.");
        lab(TS_SMALL, 40, 88, C_DIM, "Below: whole frames on this page, glFinish() before begin() and after end(); CPU is begin() to end(), GPU begin() to idle.");
        if (cjk_setup_n)
            snprintf(s, sizeof s, "The text alone, at setup (a setup pass between two glFinish calls, %d of each, interleaved): direct %.2f ms, cached %.2f ms.",
                     cjk_setup_n, cjk_setup_ms[0], cjk_setup_ms[1]);
        else
            snprintf(s, sizeof s, "The text alone: not timed (no GL on the simulated display).");
        lab(TS_SMALL, 40, 666, C_INK, s);
        if (cjk.set.id)
            snprintf(s, sizeof s, "%s, %s: %u glyphs in the curve set (%d on the page at %d px per em), built at setup in %.2f s, %.1f MB.",
                     cjk.name, cjk_whole ? "the whole font (--whole-font)" : "the glyphs this page uses (--whole-font: all of them)",
                     cjk.n_built, cjk_n, CJK_PX, cjk.build_ms * 1e-3, (16.0 * cjk.n_texels + 4.0 * cjk.n_words) / 1048576.0);
        else
            snprintf(s, sizeof s, "%s. The page shows %d glyphs of the Latin set at %d px per em instead.", cjk.why, cjk_n, CJK_PX);
        lab(TS_SMALL, 40, 706, cjk.set.id ? C_INK : C_RED, s);
        return;
    }
    {
        int m = cjk_mode ? cjk_mode - 1 : (int)((f->index / 30) & 1);
        cjk_drawn_cached = m;
        put(m ? &cjk_img : &cjk_run);
    }
    {
        static const char* const names[3] = { "alternating every 30 frames (C)", "direct (C)", "cached (C)" };
        int k;
        d_reset();
        d_str("mode: "); d_str(names[cjk_mode]); d_str("     now: "); d_str(cjk_drawn_cached ? "cached" : "direct");
        dyn(TS_BODY, 40, 686, C_AMBER, dbuf);
        for (k = 0; k < 2; k++) {
            double c = 0, g = 0;
            int i, n = cjk_cnt[k] < 64 ? cjk_cnt[k] : 64;
            for (i = 0; i < n; i++) { c += cjk_ms[k][0][i]; g += cjk_ms[k][1][i]; }
            d_reset();
            d_str(k ? "cached: " : "direct: ");
            if (!glFinish_) d_str("no GPU time on the simulated display");
            else if (!n) d_str("not drawn yet");
            else { d_str("CPU "); d_num(c / n, 2); d_str(" ms, GPU "); d_num(g / n, 2); d_str(" ms (last "); d_num(n, 0); d_str(" frames)"); }
            dyn(TS_BODY, k ? 620 : 40, 646, C_LIGHT, dbuf);
        }
    }
}

static void cjk_record(int cached, double cpu_ms, double gpu_ms) {
    int i = cjk_cnt[cached] % 64;
    cjk_ms[cached][0][i] = cpu_ms; cjk_ms[cached][1][i] = gpu_ms;
    cjk_cnt[cached]++;
    cjk_sum[cached][0] += cpu_ms; cjk_sum[cached][1] += gpu_ms; cjk_total[cached]++;
}

/* The text alone, without the frame's output stage or the present: each
 * mode drawn into the label target in a setup pass, between two glFinish
 * calls, interleaved. At setup, before the label target holds a page. */
static void cjk_bench(void) {
    double sum[2] = { 0, 0 };
    int rep, m;
    if (!glFinish_ || !cjk_n || !cjk_tex.id) return;
    for (rep = 0; rep < 24; rep++)
        for (m = 0; m < 2; m++) {
            int64_t t0;
            glFinish_();
            t0 = yrt_real_ns();
            if (ygfx_begin_setup(&gfx) != YGFX_OK || ygfx_begin_target(&gfx, lab_tex, zero4) != YGFX_OK) { fail("page 4: timing"); return; }
            put(m ? &cjk_img : &cjk_run);
            ygfx_end_target(&gfx);
            ygfx_end_setup(&gfx);
            glFinish_();
            if (rep >= 4) sum[m] += (double)(yrt_real_ns() - t0) * 1e-6;   /* the first ones warm up */
        }
    cjk_setup_n = 20;
    for (m = 0; m < 2; m++) cjk_setup_ms[m] = sum[m] / cjk_setup_n;
    printf("page 4, setup passes: direct %.3f ms, cached %.3f ms per pass (mean of %d each, interleaved)\n", cjk_setup_ms[0],
           cjk_setup_ms[1], cjk_setup_n);
}

/* --- page 5: per-letter animation -------------------------------------------- */

static ygfx_citem let_it[LET_N], wave_it[WAVE_N];
static ygfx_stim let_run, wave_run;
static const float wave_pal[6] = { 0.62f, 0.62f, 0.62f, 0.60f, 0.32f, 0.04f };

static void page_letters(int mode, const yscr_frame* f) {
    int k;
    if (mode == PG_SETUP) {
        ygfx_crun_desc d;
        ytl_seq q;
        ytl_tween_desc t;
        int n = layout(&ui, let_it, LET_N, "psychophysics", 0, 0, 96, 0), m;
        memset(&d, 0, sizeof d);
        d.place = YGFX_TOP_LEFT; d.anchor = YGFX_CENTER; d.x = 600; d.y = 300;
        d.set = ui.set; d.items = let_it; d.n = n; d.size = 96; d.fields = YGFX_I_GATE | YGFX_I_SCALE;
        rgb(d.color, LIGHT);
        let_run = ygfx_crun(&gfx, &d);
        for (k = 0; k < n; k++) {
            bind_field(&let_it[k].y, CH_LY + k);
            bind_field(&let_it[k].gate, CH_LG + k);
            bind_field(&let_it[k].scale, CH_LS + k);
        }
        /* GSAP: gsap.timeline()
         *   .from(chars, { y: 50, opacity: 0, scale: 0.6, duration: 0.6, ease: "power1.out", stagger: 0.06 })
         *   .to(chars, { y: -18, duration: 0.4, ease: "sine.inOut", yoyo: true, repeat: 5, stagger: 0.08 }, 1.6)
         *   .to(chars, { y: -40, opacity: 0, duration: 0.5, ease: "power1.in", stagger: 0.04 }, 5.6)
         * Each letter's field is a channel; a sequence lowers each one's
         * tweens to one keyed track. */
        q = ytl_seq_on(&tl, PAGE_BASE);
        for (k = 0; k < n; k++) {
            ytl_at(&q, (int64_t)k * 60 * MS_NS);
            memset(&t, 0, sizeof t);
            t.from = 50; t.from_set = true; t.to = 0; t.duration_ns = 600 * MS_NS; t.ease = YTL_EASE_QUAD_OUT;
            ytl_to(&q, CH_LY + k, &t);
            t.from = 0.6f; t.to = 1;
            ytl_to(&q, CH_LS + k, &t);
            t.from = 0; t.to = 1; t.duration_ns = 400 * MS_NS;
            ytl_to(&q, CH_LG + k, &t);
            ytl_at(&q, 1600 * MS_NS + (int64_t)k * 80 * MS_NS);
            memset(&t, 0, sizeof t);
            t.to = -18; t.duration_ns = 400 * MS_NS; t.ease = YTL_EASE_COSINE; t.yoyo = true; t.cycles = 3;
            ytl_to(&q, CH_LY + k, &t);
            ytl_at(&q, 5600 * MS_NS + (int64_t)k * 40 * MS_NS);
            memset(&t, 0, sizeof t);
            t.to = -40; t.duration_ns = 500 * MS_NS; t.ease = YTL_EASE_QUAD_IN;
            ytl_to(&q, CH_LY + k, &t);
            t.to = 0;
            ytl_to(&q, CH_LG + k, &t);
        }
        if (q.err < 0) { fprintf(stderr, "gfx_text: page 5: the sequence's call %d: %s\n", q.err_call, ytl_strerror(q.err)); refused++; }
        /* a wave that never ends: a yoyo tween per letter, each started 70 ms
         * after the one before; the color is a palette position */
        m = layout(&ui, wave_it, WAVE_N, "a wave, forever", 0, 0, 56, 0);
        memset(&d, 0, sizeof d);
        d.place = YGFX_TOP_LEFT; d.anchor = YGFX_CENTER; d.x = 600; d.y = 540;
        d.set = ui.set; d.items = wave_it; d.n = m; d.size = 56; d.fields = YGFX_I_COLOR;
        d.palette = wave_pal; d.n_palette = 2;
        wave_run = ygfx_crun(&gfx, &d);
        for (k = 0; k < m; k++) {
            bind_field(&wave_it[k].y, CH_WY + k);
            bind_field(&wave_it[k].color, CH_WC + k);
            if (yoyo(CH_WY + k, 0, -14, 0.6, YTL_EASE_COSINE, (int64_t)k * 70 * MS_NS) < 0 ||
                yoyo(CH_WC + k, 0, 1, 1.2, YTL_EASE_COSINE, (int64_t)k * 70 * MS_NS) < 0) fail("page 5: the wave tweens");
        }
        return;
    }
    if (mode == PG_STATIC) {
        lab(TS_SMALL, 40, 100, C_INK, "Each letter's y, gate and scale are timeline channels bound with ygfx_bind.field to the run's items:");
        lab(TS_SMALL, 40, 118, C_INK, "a staggered entrance, a wave and an exit built with ytl_seq (GSAP's stagger), replayed every 7.5 s by an anchor.");
        lab(TS_SMALL, 40, 420, C_INK, "Below: a yoyo tween per letter on y and on the palette position, started 70 ms apart.");
        lab(TS_SMALL, 40, 680, C_DIM, "Items in .items are copied at each draw (17 us of CPU for 40 glyphs, documented), so a run that animates per letter costs little.");
        return;
    }
    put(&let_run);
    put(&wave_run);
    d_reset();
    d_str("t = "); d_num(page_seconds(f), 1); d_str(" s");
    dyn(TS_SMALL, 40, 150, C_AMBER, dbuf);
}

/* --- page 6: SVG artwork ------------------------------------------------------ */

#define ART_MAX 16
#define ART_SC 1.5f
#define ART_X 40
#define ART_Y 80
static yol_cset art_src;   /* .keep: the hit test reads these arrays until exit */
static ygfx_cset art_set;
static ygfx_citem art_it[ART_MAX], art_hl[1];
static ygfx_stim art_run, art_hl_run, art_panel;
static float art_pal[3 * ART_MAX];
static int art_n, art_pick = -1;
static uint32_t art_rgba[ART_MAX];
static int art_src_kind[ART_MAX], art_elem[ART_MAX];
static double art_vb[4];
static const char* const art_names[] = {
    "rect, the sky", "circle, the sun", "path (Q, T), far hills", "path (C, S), near hills", "rect, the house",
    "polygon, the roof", "rect (rx), the door", "rect, the trunk", "ellipse, the canopy", "path (arcs, evenodd), a ring",
    "polyline, the fence", "line, the ground",
};

static double srgb_to_linear(double v) { return v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4); }

static void page_svg(int mode, const yscr_frame* f) {
    int k;
    if (mode == PG_SETUP) {
        static const char svg[] =
            "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 480 300\">\n"
            "  <rect x=\"0\" y=\"0\" width=\"480\" height=\"300\" fill=\"#9db4cc\"/>\n"
            "  <circle cx=\"390\" cy=\"72\" r=\"34\" fill=\"#e2b45a\" stroke=\"#b8893a\" stroke-width=\"6\"/>\n"
            "  <path d=\"M0 210 Q 70 120 150 175 T 300 160 Q 380 110 480 190 L480 300 L0 300 Z\" fill=\"#7d9a78\"/>\n"
            "  <path d=\"M0 240 C 90 200 160 260 260 230 S 420 200 480 235 L480 300 L0 300 Z\" fill=\"#5a7653\"/>\n"
            "  <g transform=\"translate(118 150)\">\n"
            "    <rect x=\"0\" y=\"40\" width=\"96\" height=\"70\" fill=\"#d2c3a6\" stroke=\"#6b5a45\" stroke-width=\"4\" stroke-linejoin=\"round\"/>\n"
            "    <polygon points=\"-10,44 48,0 106,44\" fill=\"#94574a\"/>\n"
            "    <rect x=\"38\" y=\"70\" width=\"22\" height=\"40\" rx=\"4\" fill=\"#6b5a45\"/>\n"
            "  </g>\n"
            "  <g transform=\"translate(330 150) rotate(-5)\">\n"
            "    <rect x=\"-6\" y=\"40\" width=\"12\" height=\"62\" fill=\"#6b5039\"/>\n"
            "    <ellipse cx=\"0\" cy=\"22\" rx=\"38\" ry=\"44\" fill=\"#3f6b45\" fill-opacity=\"0.9\"/>\n"
            "  </g>\n"
            "  <path d=\"M 70 268 a 22 22 0 1 0 44 0 a 22 22 0 1 0 -44 0 Z M 82 268 a 10 10 0 1 0 20 0 a 10 10 0 1 0 -20 0 Z\"\n"
            "        fill=\"#c2b394\" fill-rule=\"evenodd\"/>\n"
            "  <polyline points=\"230,262 250,248 270,262 290,248 310,262 330,248\" fill=\"none\" stroke=\"#7a6a55\"\n"
            "            stroke-width=\"4\" stroke-linecap=\"round\" stroke-linejoin=\"round\"/>\n"
            "  <line x1=\"20\" y1=\"294\" x2=\"460\" y2=\"294\" stroke=\"#46563f\" stroke-width=\"3\"/>\n"
            "</svg>\n";
        static yol_svg_layer layers[ART_MAX];
        yol_svg_desc sd;
        yol_cset_desc od;
        ygfx_cset_desc cd;
        ygfx_crun_desc d;
        yol_path o;
        char err[200];
        memset(&sd, 0, sizeof sd);
        sd.layers = layers; sd.max_layers = ART_MAX;
        art_n = yol_svg(&ol, svg, sizeof svg - 1, &sd, art_vb, err, sizeof err);
        if (art_n <= 0) { fprintf(stderr, "gfx_text: page 6: %s\n", err); refused++; art_n = 0; return; }
        /* fills and strokes at ids 0 .. n - 1, their outlines (the pick's
         * highlight) at n .. 2n - 1 */
        memset(&od, 0, sizeof od);
        od.n_glyphs = (uint32_t)(2 * art_n); od.backward = true;
        if (!yol_cset_open(&art_src, &ol, &od)) { fail("page 6: set"); return; }
        yol_path_init(&o, &ol);
        for (k = 0; k < art_n; k++) {
            yol_stroke_desc st;
            memset(&st, 0, sizeof st);
            st.width = 4; st.join = YOL_JOIN_ROUND;
            yol_path_clear(&o);
            if (yol_cset_add(&art_src, (uint32_t)k, &layers[k].path) < 0 || yol_stroke(&ol, &layers[k].path, &o, &st) < 0 ||
                yol_cset_add(&art_src, (uint32_t)(art_n + k), &o) < 0) {
                fprintf(stderr, "gfx_text: page 6: layer %d: %s\n", k, yol_error(&ol)); refused++;
            }
            art_rgba[k] = layers[k].rgba; art_src_kind[k] = layers[k].source; art_elem[k] = layers[k].element;
            art_pal[3 * k] = (float)srgb_to_linear((layers[k].rgba >> 24 & 255) / 255.0);
            art_pal[3 * k + 1] = (float)srgb_to_linear((layers[k].rgba >> 16 & 255) / 255.0);
            art_pal[3 * k + 2] = (float)srgb_to_linear((layers[k].rgba >> 8 & 255) / 255.0);
            memset(&art_it[k], 0, sizeof art_it[k]);
            art_it[k].glyph = (float)k; art_it[k].color = (float)k; art_it[k].contrast = (float)(layers[k].rgba & 255) / 255.0f;
            yol_path_free(&layers[k].path);
        }
        yol_path_free(&o);
        memset(&cd, 0, sizeof cd);
        cd.texels = art_src.texels; cd.n_texels = art_src.n_texels; cd.words = art_src.words; cd.n_words = art_src.n_words; cd.keep = true;
        art_set = ygfx_cset_make(&gfx, &cd);
        if (!art_set.id) { fail("page 6: set"); return; }
        memset(&d, 0, sizeof d);
        d.place = YGFX_TOP_LEFT; d.anchor = YGFX_TOP_LEFT; d.x = ART_X; d.y = ART_Y;
        d.w = (float)art_vb[2] * ART_SC; d.h = (float)art_vb[3] * ART_SC;
        d.set = art_set; d.items = art_it; d.n = art_n; d.size = ART_SC;
        d.fields = YGFX_I_COLOR | YGFX_I_CONTRAST; d.palette = art_pal; d.n_palette = art_n;
        art_run = ygfx_crun(&gfx, &d);
        memset(art_hl, 0, sizeof art_hl);
        d.items = art_hl; d.n = 1; d.fields = 0; d.palette = NULL; d.n_palette = 0;
        d.color[0] = d.color[1] = d.color[2] = 0.80f;   /* light, against every layer's color */
        art_hl_run = ygfx_crun(&gfx, &d);
        art_panel = rect(ART_X - 6, ART_Y - 6, (float)art_vb[2] * ART_SC + 12, (float)art_vb[3] * ART_SC + 12, DARK, 6);
        return;
    }
    if (mode == PG_STATIC) {
        char s[160];
        lab(TS_SMALL, 40, 64, C_INK, "An SVG made in this program, read by yol_svg() into solid layers (a fill, then its stroke), each one glyph of one curve set: one run.");
        lab(TS_BODY, 790, 100, C_LIGHT, "layer   source   color");
        for (k = 0; k < art_n; k++) {
            const char* nm = art_elem[k] >= 0 && art_elem[k] < COUNT(art_names) ? art_names[art_elem[k]] : "?";
            snprintf(s, sizeof s, "%2d   %s   #%06x  %s", k, art_src_kind[k] == YOL_SVG_STROKE ? "stroke" : "fill", (unsigned)(art_rgba[k] >> 8), nm);
            lab(TS_SMALL, 800, 124 + 19 * k, C_INK, s);
        }
        lab(TS_SMALL, 40, ART_Y + 470, C_DIM, "Click a shape: ygfx_hit_index() gives the topmost layer under the pointer, by the winding");
        lab(TS_SMALL, 40, ART_Y + 488, C_DIM, "from the set kept on the CPU (.keep); its outline (yol_stroke, 4 units) is drawn over it.");
        return;
    }
    if (!art_n) return;
    if (click_x >= 0) {
        art_pick = ygfx_hit_index(&gfx, &art_run, click_x, click_y);
        click_x = click_y = -1;
    }
    put(&art_panel);
    put(&art_run);
    if (art_pick >= 0) {
        art_hl[0].glyph = (float)(art_n + art_pick);
        put(&art_hl_run);
        d_reset();
        d_str("picked: layer "); d_num(art_pick, 0);
        dyn(TS_BODY, 790, 124 + 19 * art_n + 20, C_AMBER, dbuf);
        dyn(TS_SMALL, 780, 124 + 19 * art_pick, C_AMBER, ">");
    }
    (void)f;
}

/* =========================================================================== *
 * Pages and the loop
 * =========================================================================== */

typedef void (*page_fn)(int mode, const yscr_frame* f);
typedef struct text_page { const char* name; const char* keys; page_fn fn; double loop_s; } text_page;
static const text_page pages[] = {
    { "Scale and rotate", "", page_scale, 0 },
    { "Exact area and rays", "D: drift; the pointer moves the lens", page_exact, 0 },
    { "Outlines, bold, shadow, glow, blur", "- and =: sigma; T: the tween", page_effects, 0 },
    { "A page of CJK: direct and cached", "C: alternate, direct, cached", page_cjk, 0 },
    { "Per-letter animation", "", page_letters, 7.5 },
    { "SVG artwork as curve layers", "click: pick a layer", page_svg, 0 },
};
#define N_PAGES COUNT(pages)

/* The page's labels into lab_tex, between frames: a setup pass. */
static void page_static(int p) {
    char s[720];
    int k;
    for (k = 0; k < N_TS; k++) lab_n[k] = 0;
    if (ygfx_begin_setup(&gfx) != YGFX_OK || ygfx_begin_target(&gfx, lab_tex, zero4) != YGFX_OK) { fail("labels: setup pass"); return; }
    cur_what = pages[p].name;
    snprintf(s, sizeof s, "%d/%d   %s", p + 1, N_PAGES, pages[p].name);
    lab(TS_TITLE, 24, 36, C_LIGHT, s);
    lab_right(TS_SMALL, WIN_W - 24, 22, C_DIM, "Arrows, Space or 1-6: page     Shift+Esc: quit");
    if (pages[p].keys[0]) lab_right(TS_SMALL, WIN_W - 24, 40, C_AMBER, pages[p].keys);
    pages[p].fn(PG_STATIC, NULL);
    if (ui.real) snprintf(s, sizeof s, "Font: %s (ASCII glyphs, resolved, backward lists).", ui.path);
    else snprintf(s, sizeof s, "%s: the 5 x 7 bitmap font made into curves instead.", ui.why);
    lab(TS_SMALL, 24, WIN_H - 30, ui.real ? C_DIM : C_RED, s);
    lab(TS_SMALL, 24, WIN_H - 12, C_DIM,
        "Layout by advances only (cmap, hmtx): no shaping, kerning, bidi or line breaking. Real text goes through Skribidi in the pack tool and the player.");
    for (k = 0; k < N_TS; k++) {
        if (!lab_n[k]) continue;
        lab_run[k].count = (uint32_t)lab_n[k];
        put(&lab_run[k]);
    }
    ygfx_end_target(&gfx);
    if (ygfx_end_setup(&gfx) != YGFX_OK) fail("labels: setup pass");
}

static void show_page(int p, const yscr_frame* f) {
    char title[160];
    page_t0 = f->onset;
    ytl_anchor(&tl, PAGE_BASE, f->onset, 0);   /* every animation restarts */
    click_x = click_y = -1;                       /* a click belongs to its page */
    /* SDL and printf may allocate: only here, on a page change */
    snprintf(title, sizeof title, "gfx_text %d/%d: %s", p + 1, N_PAGES, pages[p].name);
    if (yscr_window(&scr)) yscr_window_call(&scr, title_on_window, title);
    printf("page %d/%d: %s\n", p + 1, N_PAGES, pages[p].name);
    fflush(stdout);
    page_static(p);
    if (p == 5 && art_n && art_pick < 0)   /* a pick to show before any click: the sun */
        art_pick = ygfx_hit_index(&gfx, &art_run, ART_X + 390 * ART_SC, ART_Y + 72 * ART_SC);
}

/* sRGB's primaries and white at gamma 2.2. Not a measurement. */
static int cal_setup(void) {
    static const float xy[4][2] = { { 0.64f, 0.33f }, { 0.30f, 0.60f }, { 0.15f, 0.06f }, { 0.3127f, 0.3290f } };
    if (ycol_cal_nominal(&cal, xy, 80.0f, 2.2) < 0) return -1;
    ycol_cal_save(&cal, NULL, 0);   /* seals the CRC, which open() checks */
    return 0;
}

#define DEFAULT_FONT "C:/Windows/Fonts/segoeui.ttf"
#define DEFAULT_CJK  "C:/Windows/Fonts/msyh.ttc"

static int fonts_setup(const char* font_path, const char* cjk_path) {
    static uint32_t list[128];
    int n = 0, c, k;
    tf_open(&ui, font_path, strcmp(font_path, DEFAULT_FONT) ? "The --font font" : "Segoe UI");
    if (!ui.real) printf("%s: the bitmap font instead\n", ui.why);
    for (c = 32; c < 127; c++) {
        uint32_t g = tf_gid(&ui, (uint32_t)c);
        for (k = 0; k < n && list[k] != g; k++) {}
        if (k == n) list[n++] = g;   /* .notdef (0) too: the box for a missing character */
    }
    if (tf_build(&ui, list, n) < 0) return -1;
    if (tf_open(&cjk, cjk_path, strcmp(cjk_path, DEFAULT_CJK) ? "The --cjk font" : "Microsoft YaHei") && !tf_gid(&cjk, 0x4E00)) {
        snprintf(cjk.why, sizeof cjk.why, "%.200s has no glyph for U+4E00", cjk.path);
        cjk.real = 0;
    }
    if (cjk.real) {
        /* the page's glyphs only, unless --whole-font: 1800 glyphs build in
         * a fraction of the whole font's seconds and ~98 MB */
        static uint32_t used[COUNT(cjk_items)];
        static unsigned char seen[65536];
        int m = 0, i;
        cjk_layout(&cjk, 1);
        for (i = 0; i < cjk_n; i++) {
            uint32_t g = (uint32_t)cjk_items[i].glyph;
            if (g < 65536 && !seen[g]) { seen[g] = 1; used[m++] = g; }
        }
        if (tf_build(&cjk, cjk_whole ? NULL : used, cjk_whole ? 0 : m) < 0) {
            snprintf(cjk.why, sizeof cjk.why, "%s: the curve set failed", cjk.name);
            cjk.set.id = 0;
        }
    } else {
        printf("%s: page 4 shows the Latin set instead\n", cjk.why);
    }
    return 0;
}

int main(int argc, char** argv) {
    yscr_desc sd;
    ygfx_desc gd;
    yscr_frame f;
    const char* shots = NULL;
    const char* font_path = DEFAULT_FONT;
    const char* cjk_path = DEFAULT_CJK;
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
        else if (!strcmp(argv[i], "--font") && i + 1 < argc) font_path = argv[++i];
        else if (!strcmp(argv[i], "--cjk") && i + 1 < argc) cjk_path = argv[++i];
        else if (!strcmp(argv[i], "--whole-font")) cjk_whole = 1;
        else cur = -1;
        if (cur < 0 || cur >= N_PAGES) {
            fprintf(stderr, "usage: gfx_text [--sim] [--page 1..%d] [--frames N] [--composition] [--topmost] [--shots PREFIX] "
                            "[--cache DIR] [--no-cache] [--font PATH] [--cjk PATH] [--whole-font]\n", N_PAGES);
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
    if (cal_setup() < 0) { fprintf(stderr, "gfx_text: the nominal calibration failed\n"); return 1; }
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
    if (!yscr_open(&scr, &sd)) { fprintf(stderr, "gfx_text: %s\n", yscr_error(&scr)); return 1; }
    memset(&gd, 0, sizeof gd);
    gd.screen = &scr;
    rgb(gd.background, BG);
    gd.cal = &cal;
    gd.width = WIN_W; gd.height = WIN_H;   /* the simulated display's size */
    gd.cache = cache;
    if (!ygfx_open(&gfx, &gd)) { fprintf(stderr, "gfx_text: %s\n", ygfx_error(&gfx)); yscr_close(&scr); return 1; }
    yscr_describe(&scr, line, sizeof line);
    printf("%s\n", line);
    ygfx_describe(&gfx, line, sizeof line);
    printf("%s\n", line);
    glFinish_ = (void (GLCALL*)(void))yscr_gl_proc(&scr, "glFinish");
#if defined(_WIN32)
    if (topmost && yscr_window(&scr)) yscr_window_call(&scr, take_foreground_on_window, yscr_window(&scr));
#else
    if (topmost) printf("--topmost acts on Win32 windows; ignored here\n");
#endif

    /* Setup: every curve set, target, blur and stimulus of every page,
     * before the first frame; nothing in the frame loop allocates. */
    if (!yol_open(&ol, NULL) || timeline_setup() < 0 || fonts_setup(font_path, cjk_path) < 0 || labels_setup() < 0) {
        fprintf(stderr, "gfx_text: setup: %s\n", ygfx_error(&gfx));
        ygfx_close(&gfx);
        yscr_close(&scr);
        return 1;
    }
    for (p = 0; p < N_PAGES; p++) {
        cur_what = pages[p].name;
        pages[p].fn(PG_SETUP, NULL);
    }
    cjk_bench();
    {
        ygfx_programs ps;
        ygfx_program_stats(&gfx, &ps);
        printf("programs: %u from the cache, %u compiled; open %.0f ms\n", ps.loaded, ps.compiled, (double)ps.open_ns * 1e-6);
    }
    if (shots) shot_px = (uint8_t*)malloc((size_t)WIN_W * WIN_H * 4);

    next = cur;
    while (yscr_begin(&scr, &f) == YSCR_OK) {
        ytl_frame tf;
        int timing, k;
        int64_t t0 = 0, t1 = 0, t2 = 0;
        if (yscr_window(&scr)) {
            SDL_Event ev;
            while (yscr_poll(&scr, &ev, NULL)) {   /* yscr_begin() reports Shift+Esc and close itself */
                if (ev.type == SDL_EVENT_QUIT || ev.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) quit = 1;
                else if (ev.type == SDL_EVENT_MOUSE_MOTION) { mouse_x = ev.motion.x; mouse_y = ev.motion.y; }   /* px at density 1 */
                else if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN) { click_x = ev.button.x; click_y = ev.button.y; }
                else if (ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat) {
                    SDL_Keycode key = ev.key.key;
                    if (key == SDLK_RIGHT || key == SDLK_DOWN || key == SDLK_SPACE || key == SDLK_PAGEDOWN) next = (cur + 1) % N_PAGES;
                    else if (key == SDLK_LEFT || key == SDLK_UP || key == SDLK_PAGEUP) next = (cur + N_PAGES - 1) % N_PAGES;
                    else if (key >= SDLK_1 && key < SDLK_1 + (SDL_Keycode)N_PAGES) next = (int)(key - SDLK_1);
                    else if (cur == 1 && key == SDLK_D) drift_on = !drift_on;
                    else if (cur == 2 && key == SDLK_MINUS) sigma_manual = (sigma_manual > 0 ? sigma_manual : blurl.sigma) / 1.25f;
                    else if (cur == 2 && key == SDLK_EQUALS) sigma_manual = (sigma_manual > 0 ? sigma_manual : blurl.sigma) * 1.25f;
                    else if (cur == 2 && key == SDLK_T) sigma_manual = 0;
                    else if (cur == 3 && key == SDLK_C) cjk_mode = (cjk_mode + 1) % 3;
                }
            }
            if (sigma_manual > 0 && sigma_manual < 0.35f) sigma_manual = 0.35f;   /* above the box's own SD */
            if (sigma_manual > 30) sigma_manual = 30;
        }
        if (!started || next != cur) { cur = next; show_page(cur, &f); on_page = 0; started = 1; }
        if (pages[cur].loop_s > 0 && page_seconds(&f) >= pages[cur].loop_s) {
            page_t0 = f.onset;
            ytl_anchor(&tl, PAGE_BASE, f.onset, 0);   /* rewinds: the sequence replays exactly */
        }
        tf.onset = f.onset; tf.period = f.period; tf.index = f.index;
        ytl_evaluate(&tl, &tf, NULL, 0);
        ygfx_apply(binds, n_binds, ytl_values(&tl));
        yscr_mark(&scr, YSCR_PHASE_EVALUATE);

        timing = cur == 3 && glFinish_ != NULL;
        if (timing) { glFinish_(); t0 = yrt_real_ns(); }
        ygfx_begin(&gfx, &f);
        for (k = 0; k < N_TS; k++) dyn_n[k] = 0;
        cur_what = pages[cur].name;
        pages[cur].fn(PG_FRAME, &f);
        put(&lab_img);
        for (k = 0; k < N_TS; k++) {
            if (!dyn_n[k]) continue;
            dyn_run[k].count = (uint32_t)dyn_n[k];
            put(&dyn_run[k]);
        }
        ygfx_end(&gfx);
        if (timing) {
            t1 = yrt_real_ns();
            glFinish_();
            t2 = yrt_real_ns();
            if (f.index % 30 > 2 || cjk_mode) cjk_record(cjk_drawn_cached, (double)(t1 - t0) * 1e-6, (double)(t2 - t0) * 1e-6);
        }
        on_page++;
        if (shot_px && on_page == SHOT_FRAMES) {   /* the back buffer, before the flip */
            char path[512];
            snprintf(path, sizeof path, "%s-p%d.ppm", shots, cur + 1);
            if (ygfx_read_output(&gfx, 0, 0, WIN_W, WIN_H, shot_px) < 0 || ygfx_image_write(path, shot_px, WIN_W, WIN_H, 0, NULL, NULL, 0) < 0)
                fprintf(stderr, "gfx_text: could not write %s\n", path);
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
    for (i = 0; i < 2; i++)
        if (cjk_total[i])
            printf("page 4, %s: CPU %.3f ms, GPU %.3f ms per frame (mean of %ld frames, glFinish-bracketed)\n", i ? "cached" : "direct",
                   cjk_sum[i][0] / (double)cjk_total[i], cjk_sum[i][1] / (double)cjk_total[i], cjk_total[i]);
    if (ygfx_clipped(&gfx)) printf("%llu draws may have left the gamut\n", (unsigned long long)ygfx_clipped(&gfx));
    free(shot_px);
    ygfx_close(&gfx);
    if (art_src.words) yol_cset_close(&art_src);
    yol_close(&ol);
    free(ui.bytes); free(cjk.bytes);
    yscr_close(&scr);
    if (refused) { fprintf(stderr, "gfx_text: %ld refused draws or setup errors\n", refused); return 1; }
    return 0;
}
