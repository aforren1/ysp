/* gfx_layout.c - text layout through pack/layout (Skribidi, pinned and
 * patched) drawn as ysp/gfx.h curve runs over ysp/outline.h curve sets.
 *
 * Four pages in a 1200 x 760 window:
 *   1  wrapped paragraphs in Latin, Arabic, Hebrew, Devanagari, Thai,
 *      Chinese and Japanese, with the Windows fonts; [ and ] or a drag of
 *      the handle change the width; each block shows its line count and
 *      the time of its last layout;
 *   2  mixed directions: the two strings that hit Skribidi's run-merge bug
 *      (docs/layout_probe.md 2.3) and the editor's caret string, each glyph
 *      colored by its logical position (amber first, teal last), with the
 *      order they must show and what unpatched Skribidi showed;
 *   3  Japanese phrase breaking (BudouX, lang "ja") beside UAX 14 breaking
 *      (lang "en"), and Thai (lang "th" against "en"), at one width;
 *   4  an editable text box on Skribidi's editor: caret moves in bidi text,
 *      selection (Shift+arrows, drag, double and triple click), undo and
 *      redo, IME composition. It turns on text input with
 *      yscr_text_input(): typed input is untimed while it is on.
 *
 * Fonts: Segoe UI, Arial, Nirmala UI, Leelawadee UI, Microsoft YaHei and
 * Yu Gothic from C:/Windows/Fonts, or the folder given with --fonts. One
 * fallback list serves every page (the first font that has a script's
 * codepoint), and a second one puts Yu Gothic first for Japanese. A missing
 * font is named on stdout and on every page; with no font at all the pages
 * draw nothing and say so on stdout, as on a CI runner. Curve sets hold the
 * glyphs the layouts use, built on demand at setup and when text changes,
 * uploaded between frames with ygfx_cset_add(). Static text is laid out
 * once; the frame loop draws items and allocates nothing until text
 * changes. Colors are linear light through a nominal calibration (sRGB
 * primaries, gamma 2.2): NOT a measurement of this display. Moderate
 * contrast, no motion.
 *
 * Usage: gfx_layout [--sim] [--page N] [--frames N] [--composition]
 *                   [--shots PREFIX] [--cache DIR] [--no-cache] [--fonts DIR]
 *   Right, Down, Page Down: next page; Left, Up, Page Up: previous; 1 to 4
 *   or F1 to F4: that page (on page 4, where keys edit, use F1 to F4 or
 *   Page Up and Page Down);
 *   Shift+Esc or closing the window: quit. Pages 1 and 3: [ and ]: width;
 *   page 1: drag the handle too. Page 4: type; arrows, Home, End, Shift to
 *   select, Ctrl+Z, Ctrl+Y, Ctrl+A, the mouse.
 *   --sim          the simulated display and the null backend, for CI:
 *                  draws each page once (page 4 runs a scripted edit) and
 *                  exits
 *   --page N       start on page N (1 to 4)
 *   --frames N     stop after N frames
 *   --composition  the composition swapchain (Windows)
 *   --shots PREFIX show each page for 120 frames, read the output back,
 *                  write PREFIX-pN.ppm, then quit
 *   --cache DIR    keep compiled programs in DIR (ygfx_file_cache_init:
 *                  it writes files there); the default is the per-user
 *                  folder of ygfx_default_cache_dir(), when there is one
 *   --no-cache     compile every program; read and write no cache file
 *   --fonts DIR    the font folder (default C:/Windows/Fonts)
 * Exit code: 0; 1 when the screen, the gfx or the layout did not open, or a
 * draw or setup step was refused; 2 for a bad argument. A missing font is
 * not an error. On Windows set YSCR_ANGLE_DIR to ANGLE's directory.
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#define YSP_GFX_IMPLEMENTATION
#include "ysp/gfx.h"
#define YSP_OUTLINE_IMPLEMENTATION
#include "ysp/outline.h"
#include "ysp/layout.h"
#include "skribidi/skb_editor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIN_W 1200
#define WIN_H 760
#define SHOT_FRAMES 120
#define COUNT(a) (int)(sizeof(a) / sizeof((a)[0]))

static yscr_screen scr;
static ygfx_gfx gfx;
static ycol_cal cal;
static yol_ctx ol;
static int sim;
static int shot_select;   /* --shots: page 4 shows a selection across the bidi runs */

/* Linear device RGB. */
static const float BG[3]    = { 0.18f, 0.18f, 0.18f };
static const float PANEL[3] = { 0.13f, 0.13f, 0.13f };
static const float SEL[3]   = { 0.07f, 0.20f, 0.22f };
static const float CARET[3] = { 0.60f, 0.32f, 0.04f };
static const float GRIP[3]  = { 0.30f, 0.30f, 0.30f };
static const float WHITE[3] = { 1.0f, 1.0f, 1.0f };

/* The text palette (YGFX_I_COLOR): an item's color is its index. */
enum { C_INK, C_LIGHT, C_DIM, C_AMBER, C_TEAL, C_RED, N_COLORS };
static const float text_pal[3 * N_COLORS] = {
    0.55f, 0.55f, 0.55f,  0.66f, 0.66f, 0.66f,  0.34f, 0.34f, 0.34f,
    0.60f, 0.32f, 0.04f,  0.08f, 0.42f, 0.44f,  0.55f, 0.12f, 0.08f,
};
/* Page 2's ramp: a fraction mixes two neighbors in linear light. */
static const float ramp_pal[3 * 2] = { 0.62f, 0.34f, 0.05f,  0.06f, 0.44f, 0.48f };

static long refused;
static const char* cur_what = "";
static const char* reported_what;

static void put(const ygfx_stim* s) {
    int rc = ygfx_draw(&gfx, s);
    if (rc < 0) {
        refused++;
        if (reported_what != cur_what) {
            fprintf(stderr, "gfx_layout: %s: %s (%s)\n", cur_what, ygfx_strerror(rc), ygfx_error(&gfx));
            reported_what = cur_what;
        }
    }
}
static void fail(const char* what, const char* why) {
    refused++;
    fprintf(stderr, "gfx_layout: %s: %s\n", what, why);
}
static void rgb(float* dst, const float* src) { dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; }

static ygfx_stim rect(float x0, float y0, float w, float h, const float* color) {
    ygfx_shape_desc d;
    memset(&d, 0, sizeof d);
    d.place = YGFX_TOP_LEFT; d.anchor = YGFX_TOP_LEFT;
    d.shape = YGFX_RECT; d.x = x0; d.y = y0; d.w = w > 0.5f ? w : 0.5f; d.h = h > 0.5f ? h : 0.5f;
    d.edge = YGFX_EDGE_COSINE; d.edge_width = 1.0f;
    rgb(d.color, color);
    return ygfx_shape(&d);
}

/* --- fonts and libraries ---------------------------------------------------- */

enum { F_SEGOE, F_ARIAL, F_NIRMALA, F_LEELAWADEE, F_YAHEI, F_YUGOTHIC, N_FILES };
typedef struct font_file {
    const char* name;
    const char* file;
    uint8_t*    bytes;
    size_t      n;
    char        why[700];   /* "" when it opened */
} font_file;
static font_file files[N_FILES] = {
    { "Segoe UI", "segoeui.ttf", NULL, 0, "" },
    { "Arial", "arial.ttf", NULL, 0, "" },
    { "Nirmala UI", "Nirmala.ttc", NULL, 0, "" },
    { "Leelawadee UI", "LeelawUI.ttf", NULL, 0, "" },
    { "Microsoft YaHei", "msyh.ttc", NULL, 0, "" },
    { "Yu Gothic", "YuGothR.ttc", NULL, 0, "" },
};

/* A layout library and, per font, its curve set on the GPU. */
#define MAX_LIB_FONTS 8
typedef struct xlib {
    ylay_lib* L;
    int         n;
    int         file_of[MAX_LIB_FONTS];
    ygfx_cset set[MAX_LIB_FONTS];
} xlib;
static xlib X_main, X_ja;
static int set_gen;   /* bumps when a set is made again: runs refer to its id */

static int xlib_open(xlib* X, const int* order, int n) {
    int i;
    memset(X, 0, sizeof *X);
    if (ylay_create(&X->L, &(ylay_desc){ .ol = &ol }) != YLAY_OK) return -1;
    for (i = 0; i < n; i++) {
        font_file* F = &files[order[i]];
        if (!F->bytes) continue;
        if (ylay_add_font(X->L, F->bytes, F->n, 0, F->file) < 0) {
            snprintf(F->why, sizeof F->why, "%s", ylay_error(X->L));
            continue;
        }
        X->file_of[X->n++] = order[i];
    }
    return 0;
}

/* Uploads the glyphs the layouts added since the last call: the first time
 * a set is made with room to grow; past its room it is made again, bigger,
 * and every run is rebuilt (set_gen). Between frames only. */
static void xlib_sync(xlib* X) {
    int i;
    for (i = 0; i < X->n; i++) {
        const uint32_t* g;
        int n = ylay_take_new_glyphs(X->L, i, &g), rc;
        ylay_font_info fi;
        ygfx_cset_desc d;
        if (n <= 0) continue;
        ylay_font(X->L, i, &fi);
        memset(&d, 0, sizeof d);
        d.texels = fi.set->texels; d.n_texels = fi.set->n_texels;
        d.words = fi.set->words; d.n_words = fi.set->n_words;
        if (X->set[i].id) {
            rc = ygfx_cset_add(&gfx, X->set[i], &d, g, n);
            if (rc == YGFX_ERR_FULL) { ygfx_cset_free(&gfx, X->set[i]); X->set[i].id = 0; }
            else if (rc < 0) fail("curve set", ygfx_error(&gfx));
        }
        if (!X->set[i].id) {
            /* room for typed text: about 1000 Latin or 100 CJK glyphs */
            d.cap_texels = d.n_texels + d.n_texels / 2 + 16384;
            d.cap_words = (d.n_words + d.n_words / 2 + 65536 + 3) & ~3u;
            X->set[i] = ygfx_cset_make(&gfx, &d);
            if (!X->set[i].id) fail("curve set", ygfx_error(&gfx));
            set_gen++;
        }
    }
}

/* --- text blocks ----------------------------------------------------------- */

#define TB_RUNS 16
typedef struct tb {
    xlib*        X;
    ylay_block b;
    ygfx_stim  st[TB_RUNS];
    int          n_st, gen;
    float        x, y;
    const float* pal;
    int          n_pal;
} tb;

static void tb_build(tb* t) {
    uint32_t r;
    t->n_st = 0;
    t->gen = set_gen;
    for (r = 0; r < t->b.n_runs && t->n_st < TB_RUNS; r++) {
        const ylay_run* run = &t->b.runs[r];
        ygfx_crun_desc d;
        if (!run->n) continue;
        memset(&d, 0, sizeof d);
        d.place = YGFX_TOP_LEFT; d.anchor = YGFX_TOP_LEFT;
        d.x = t->x; d.y = t->y;
        /* every run of the block shares its box, so the items' origin is the
         * block's top-left */
        d.w = t->b.w > 1 ? t->b.w : 1; d.h = t->b.h > 1 ? t->b.h : 1;
        d.set = t->X->set[run->font];
        d.items = t->b.items + run->first; d.n = (int)run->n; d.size = run->size;
        d.fields = YGFX_I_COLOR; d.palette = t->pal ? t->pal : text_pal; d.n_palette = t->pal ? t->n_pal : N_COLORS;
        rgb(d.color, WHITE);
        t->st[t->n_st++] = ygfx_crun(&gfx, &d);
    }
}

/* Lays text out into t at x, y (the block's top-left, screen px). */
static int tb_set(tb* t, xlib* X, const char* text, const ylay_style* st, float x, float y) {
    t->X = X; t->x = x; t->y = y;
    if (!X->n) { t->n_st = 0; return -1; }
    if (ylay_layout(X->L, text, -1, st, NULL, 0, &t->b) != YLAY_OK) {
        fail("layout", ylay_error(X->L));
        t->n_st = 0;
        return -1;
    }
    xlib_sync(X);
    tb_build(t);
    return 0;
}

static void tb_draw(tb* t) {
    int i;
    if (t->gen != set_gen) tb_build(t);
    for (i = 0; i < t->n_st; i++) put(&t->st[i]);
}

/* A label: one line in the UI fonts, its baseline near y. */
static void label(tb* t, float x, float y, float px, int color, const char* s) {
    ylay_style st;
    memset(&st, 0, sizeof st);
    st.size = px; st.color = (float)color; st.dir = YLAY_DIR_LTR;
    tb_set(t, &X_main, s, &st, x, y - px);
}
/* The same, ending at x_right: right-aligned in a box of width w. */
static void label_right(tb* t, float x_right, float y, float px, int color, const char* s) {
    ylay_style st;
    const float w = 700;
    memset(&st, 0, sizeof st);
    st.size = px; st.color = (float)color; st.dir = YLAY_DIR_LTR; st.width = w; st.align = YLAY_ALIGN_END;
    tb_set(t, &X_main, s, &st, x_right - w, y - px);
}

/* --- page frame: title, keys, notes ------------------------------------------ */

#define N_PAGES 4
static const char* const page_names[N_PAGES] = {
    "Wrapped paragraphs in seven scripts",
    "Mixed directions: the run-merge bug strings",
    "Japanese phrases (BudouX) against UAX 14",
    "An editable text box (Skribidi's editor)",
};
static const char* const page_keys[N_PAGES] = {
    "[ and ] or drag the handle: width",
    "",
    "[ and ]: width",
    "Type; arrows, Home, End; Shift: select; Ctrl+Z, Ctrl+Y, Ctrl+A; the mouse",
};
static tb frame_tb[6];
static char stamp_line[1200], stamp_short[200];

static void frame_static(int p) {
    char s[1000];
    int i, missing = 0;
    snprintf(s, sizeof s, "%d/%d   %s", p + 1, N_PAGES, page_names[p]);
    label(&frame_tb[0], 24, 40, 22, C_LIGHT, s);
    label_right(&frame_tb[1], WIN_W - 24, 26, 13, p == 3 ? C_AMBER : C_DIM,
                p == 3 ? "F1-F4 or Page Up/Down: page (keys edit here)     Shift+Esc: quit"
                       : "Arrows, 1-4, F1-F4 or Page Up/Down: page     Shift+Esc: quit");
    label_right(&frame_tb[2], WIN_W - 24, 44, 13, C_AMBER, page_keys[p]);
    s[0] = 0;
    for (i = 0; i < N_FILES; i++)
        if (files[i].why[0]) {
            size_t k = strlen(s);
            snprintf(s + k, sizeof s - k, "%s%s", missing++ ? "; " : "Missing: ", files[i].why);
        }
    label(&frame_tb[3], 24, WIN_H - 30, 12, missing ? C_RED : C_DIM,
          missing ? s : "Fonts: Segoe UI, Arial, Nirmala UI, Leelawadee UI, Microsoft YaHei, Yu Gothic (C:/Windows/Fonts); curve sets of the glyphs used.");
    label(&frame_tb[4], 24, WIN_H - 12, 12, C_DIM, stamp_short);
}

/* --- page 1: paragraphs -------------------------------------------------------- */

typedef struct para { const char* name; const char* lang; int ja; const char* text; } para;
static const para paras[] = {
    { "Latin", "en", 0,
      "Participants sat 57 cm from the display and pressed a key as soon as the grating appeared. Each block began with "
      "ten practice trials, and the next block started only after a short rest. Contrast, spatial frequency and "
      "orientation varied from trial to trial." },
    { "Arabic", "ar", 0,
      "\xd8\xac\xd9\x84\xd8\xb3 \xd8\xa7\xd9\x84\xd9\x85\xd8\xb4\xd8\xa7\xd8\xb1\xd9\x83\xd9\x88\xd9\x86 \xd8\xb9\xd9\x84\xd9\x89 "
      "\xd8\xa8\xd8\xb9\xd8\xaf 57 \xd8\xb3\xd9\x85 \xd9\x85\xd9\x86 \xd8\xa7\xd9\x84\xd8\xb4\xd8\xa7\xd8\xb4\xd8\xa9\xd8\x8c "
      "\xd9\x88\xd8\xb6\xd8\xba\xd8\xb7\xd9\x88\xd8\xa7 \xd8\xb9\xd9\x84\xd9\x89 \xd9\x85\xd9\x81\xd8\xaa\xd8\xa7\xd8\xad "
      "\xd8\xb9\xd9\x86\xd8\xaf \xd8\xb8\xd9\x87\xd9\x88\xd8\xb1 \xd8\xa7\xd9\x84\xd8\xb4\xd8\xa8\xd9\x83\xd8\xa9. "
      "The trial ended after 2.5 seconds. \xd8\xa8\xd8\xaf\xd8\xa3\xd8\xaa \xd9\x83\xd9\x84 \xd9\x85\xd8\xac\xd9\x85\xd9\x88\xd8\xb9\xd8\xa9 "
      "\xd8\xa8\xd8\xb9\xd8\xb4\xd8\xb1 \xd9\x85\xd8\xad\xd8\xa7\xd9\x88\xd9\x84\xd8\xa7\xd8\xaa \xd9\x84\xd9\x84\xd8\xaa\xd8\xaf\xd8\xb1\xd9\x8a\xd8\xa8\xd8\x8c "
      "\xd8\xab\xd9\x85 \xd8\xa7\xd8\xb3\xd8\xaa\xd8\xb1\xd8\xa7\xd8\xad\xd8\xa9 \xd9\x82\xd8\xb5\xd9\x8a\xd8\xb1\xd8\xa9." },
    { "Hebrew", "he", 0,
      "\xd7\x94\xd7\x9e\xd7\xa9\xd7\xaa\xd7\xaa\xd7\xa4\xd7\x99\xd7\x9d \xd7\x99\xd7\xa9\xd7\x91\xd7\x95 57 \xd7\xa1\xd7\xb4\xd7\x9e "
      "\xd7\x9e\xd7\x94\xd7\x9e\xd7\xa1\xd7\x9a \xd7\x95\xd7\x9c\xd7\x97\xd7\xa6\xd7\x95 \xd7\xa2\xd7\x9c \xd7\x9e\xd7\xa7\xd7\xa9 "
      "\xd7\x9b\xd7\xa9\xd7\x94\xd7\xa1\xd7\xa8\xd7\x99\xd7\x92 \xd7\x94\xd7\x95\xd7\xa4\xd7\x99\xd7\xa2. The trial ended after 2.5 seconds. "
      "\xd7\x9b\xd7\x9c \xd7\x91\xd7\x9c\xd7\x95\xd7\xa7 \xd7\x94\xd7\xaa\xd7\x97\xd7\x99\xd7\x9c \xd7\x91\xd7\xa2\xd7\xa9\xd7\xa8\xd7\x94 "
      "\xd7\xa0\xd7\x99\xd7\xa1\xd7\x95\xd7\x99\xd7\x99 \xd7\x90\xd7\x99\xd7\x9e\xd7\x95\xd7\x9f \xd7\x95\xd7\x90\xd7\x97\xd7\xa8\xd7\x99\xd7\x95 "
      "\xd7\x94\xd7\xa4\xd7\xa1\xd7\xa7\xd7\x94 \xd7\xa7\xd7\xa6\xd7\xa8\xd7\x94." },
    { "Devanagari", "hi", 0,
      "\xe0\xa4\xaa\xe0\xa5\x8d\xe0\xa4\xb0\xe0\xa4\xa4\xe0\xa4\xbf\xe0\xa4\xad\xe0\xa4\xbe\xe0\xa4\x97\xe0\xa5\x80 "
      "\xe0\xa4\xb8\xe0\xa5\x8d\xe0\xa4\x95\xe0\xa5\x8d\xe0\xa4\xb0\xe0\xa5\x80\xe0\xa4\xa8 \xe0\xa4\xb8\xe0\xa5\x87 57 "
      "\xe0\xa4\xb8\xe0\xa5\x87\xe0\xa4\x82\xe0\xa4\x9f\xe0\xa5\x80\xe0\xa4\xae\xe0\xa5\x80\xe0\xa4\x9f\xe0\xa4\xb0 "
      "\xe0\xa4\xa6\xe0\xa5\x82\xe0\xa4\xb0 \xe0\xa4\xac\xe0\xa5\x88\xe0\xa4\xa0\xe0\xa5\x87 \xe0\xa4\x94\xe0\xa4\xb0 "
      "\xe0\xa4\x9c\xe0\xa4\xbe\xe0\xa4\xb2\xe0\xa5\x80 \xe0\xa4\xa6\xe0\xa4\xbf\xe0\xa4\x96\xe0\xa4\xa4\xe0\xa5\x87 "
      "\xe0\xa4\xb9\xe0\xa5\x80 \xe0\xa4\x8f\xe0\xa4\x95 \xe0\xa4\x95\xe0\xa5\x81\xe0\xa4\x82\xe0\xa4\x9c\xe0\xa5\x80 "
      "\xe0\xa4\xa6\xe0\xa4\xac\xe0\xa4\xbe\xe0\xa4\x88\xe0\xa5\xa4 \xe0\xa4\xb9\xe0\xa4\xb0 \xe0\xa4\x96\xe0\xa4\x82\xe0\xa4\xa1 "
      "\xe0\xa4\xa6\xe0\xa4\xb8 \xe0\xa4\x85\xe0\xa4\xad\xe0\xa5\x8d\xe0\xa4\xaf\xe0\xa4\xbe\xe0\xa4\xb8 "
      "\xe0\xa4\xaa\xe0\xa4\xb0\xe0\xa5\x80\xe0\xa4\x95\xe0\xa5\x8d\xe0\xa4\xb7\xe0\xa4\xa3\xe0\xa5\x8b\xe0\xa4\x82 "
      "\xe0\xa4\xb8\xe0\xa5\x87 \xe0\xa4\xb6\xe0\xa5\x81\xe0\xa4\xb0\xe0\xa5\x82 \xe0\xa4\xb9\xe0\xa5\x81\xe0\xa4\x86\xe0\xa5\xa4" },
    { "Thai", "th", 0,
      "\xe0\xb8\x9c\xe0\xb8\xb9\xe0\xb9\x89\xe0\xb9\x80\xe0\xb8\x82\xe0\xb9\x89\xe0\xb8\xb2\xe0\xb8\xa3\xe0\xb9\x88\xe0\xb8\xa7\xe0\xb8\xa1"
      "\xe0\xb8\x99\xe0\xb8\xb1\xe0\xb9\x88\xe0\xb8\x87\xe0\xb8\xab\xe0\xb9\x88\xe0\xb8\xb2\xe0\xb8\x87\xe0\xb8\x88\xe0\xb8\xb2\xe0\xb8\x81"
      "\xe0\xb8\x88\xe0\xb8\xad\xe0\xb8\xa0\xe0\xb8\xb2\xe0\xb8\x9e\xe0\xb8\xab\xe0\xb9\x89\xe0\xb8\xb2\xe0\xb8\xaa\xe0\xb8\xb4\xe0\xb8\x9a"
      "\xe0\xb9\x80\xe0\xb8\x88\xe0\xb9\x87\xe0\xb8\x94\xe0\xb9\x80\xe0\xb8\x8b\xe0\xb8\x99\xe0\xb8\x95\xe0\xb8\xb4\xe0\xb9\x80\xe0\xb8\xa1"
      "\xe0\xb8\x95\xe0\xb8\xa3\xe0\xb9\x81\xe0\xb8\xa5\xe0\xb8\xb0\xe0\xb8\x81\xe0\xb8\x94\xe0\xb8\x9b\xe0\xb8\xb8\xe0\xb9\x88\xe0\xb8\xa1"
      "\xe0\xb8\x97\xe0\xb8\xb1\xe0\xb8\x99\xe0\xb8\x97\xe0\xb8\xb5\xe0\xb8\x97\xe0\xb8\xb5\xe0\xb9\x88\xe0\xb9\x80\xe0\xb8\xab\xe0\xb9\x87"
      "\xe0\xb8\x99\xe0\xb8\xa5\xe0\xb8\xb2\xe0\xb8\xa2\xe0\xb9\x80\xe0\xb8\xaa\xe0\xb9\x89\xe0\xb8\x99\xe0\xb8\x9b\xe0\xb8\xa3\xe0\xb8\xb2"
      "\xe0\xb8\x81\xe0\xb8\x8f\xe0\xb8\x82\xe0\xb8\xb6\xe0\xb9\x89\xe0\xb8\x99 \xe0\xb9\x81\xe0\xb8\x95\xe0\xb9\x88\xe0\xb8\xa5\xe0\xb8\xb0"
      "\xe0\xb8\x8a\xe0\xb9\x88\xe0\xb8\xa7\xe0\xb8\x87\xe0\xb9\x80\xe0\xb8\xa3\xe0\xb8\xb4\xe0\xb9\x88\xe0\xb8\xa1\xe0\xb8\x94\xe0\xb9\x89"
      "\xe0\xb8\xa7\xe0\xb8\xa2\xe0\xb8\x81\xe0\xb8\xb2\xe0\xb8\xa3\xe0\xb8\x9d\xe0\xb8\xb6\xe0\xb8\x81\xe0\xb8\xaa\xe0\xb8\xb4\xe0\xb8\x9a"
      "\xe0\xb8\x84\xe0\xb8\xa3\xe0\xb8\xb1\xe0\xb9\x89\xe0\xb8\x87\xe0\xb9\x81\xe0\xb8\xa5\xe0\xb8\xb0\xe0\xb8\x9e\xe0\xb8\xb1\xe0\xb8\x81"
      "\xe0\xb8\xaa\xe0\xb8\xb1\xe0\xb9\x89\xe0\xb8\x99\xe0\xb9\x86" },
    { "Chinese", "zh-Hans", 0,
      "\xe5\x8f\x82\xe4\xb8\x8e\xe8\x80\x85\xe5\x9d\x90\xe5\x9c\xa8\xe8\xb7\x9d\xe7\xa6\xbb\xe5\xb1\x8f\xe5\xb9\x95\xe4\xba\x94\xe5\x8d\x81"
      "\xe4\xb8\x83\xe5\x8e\x98\xe7\xb1\xb3\xe7\x9a\x84\xe4\xbd\x8d\xe7\xbd\xae\xef\xbc\x8c\xe7\x9c\x8b\xe5\x88\xb0\xe5\x85\x89\xe6\xa0\x85"
      "\xe5\x87\xba\xe7\x8e\xb0\xe5\x90\x8e\xe7\xab\x8b\xe5\x8d\xb3\xe6\x8c\x89\xe9\x94\xae\xe3\x80\x82\xe6\xaf\x8f\xe4\xb8\xaa\xe5\x8c\xba"
      "\xe7\xbb\x84\xe4\xbb\xa5\xe5\x8d\x81\xe6\xac\xa1\xe7\xbb\x83\xe4\xb9\xa0\xe8\xaf\x95\xe6\xac\xa1\xe5\xbc\x80\xe5\xa7\x8b\xef\xbc\x8c"
      "\xe7\x9f\xad\xe6\x9a\x82\xe4\xbc\x91\xe6\x81\xaf\xe4\xb9\x8b\xe5\x90\x8e\xe6\x89\x8d\xe5\xbc\x80\xe5\xa7\x8b\xe4\xb8\x8b\xe4\xb8\x80"
      "\xe4\xb8\xaa\xe5\x8c\xba\xe7\xbb\x84\xe3\x80\x82" },
    { "Japanese", "ja", 1,
      "\xe5\x8f\x82\xe5\x8a\xa0\xe8\x80\x85\xe3\x81\xaf\xe7\x94\xbb\xe9\x9d\xa2\xe3\x81\x8b\xe3\x82\x89\xe4\xba\x94\xe5\x8d\x81\xe4\xb8\x83"
      "\xe3\x82\xbb\xe3\x83\xb3\xe3\x83\x81\xe9\x9b\xa2\xe3\x82\x8c\xe3\x81\xa6\xe5\xba\xa7\xe3\x82\x8a\xe3\x80\x81\xe7\xb8\x9e\xe6\xa8\xa1"
      "\xe6\xa7\x98\xe3\x81\x8c\xe8\xa6\x8b\xe3\x81\x88\xe3\x81\x9f\xe3\x82\x89\xe3\x81\x99\xe3\x81\x90\xe3\x81\xab\xe3\x82\xad\xe3\x83\xbc"
      "\xe3\x82\x92\xe6\x8a\xbc\xe3\x81\x97\xe3\x81\xbe\xe3\x81\x97\xe3\x81\x9f\xe3\x80\x82\xe5\x90\x84\xe3\x83\x96\xe3\x83\xad\xe3\x83\x83"
      "\xe3\x82\xaf\xe3\x81\xaf\xe5\x8d\x81\xe5\x9b\x9e\xe3\x81\xae\xe7\xb7\xb4\xe7\xbf\x92\xe8\xa9\xa6\xe8\xa1\x8c\xe3\x81\x8b\xe3\x82\x89"
      "\xe5\xa7\x8b\xe3\x81\xbe\xe3\x82\x8a\xe3\x80\x81\xe7\x9f\xad\xe3\x81\x84\xe4\xbc\x91\xe6\x86\xa9\xe3\x81\xae\xe5\xbe\x8c\xe3\x81\xab"
      "\xe6\xac\xa1\xe3\x81\xae\xe3\x83\x96\xe3\x83\xad\xe3\x83\x83\xe3\x82\xaf\xe3\x81\x8c\xe5\xa7\x8b\xe3\x81\xbe\xe3\x82\x8a\xe3\x81\xbe"
      "\xe3\x81\x97\xe3\x81\x9f\xe3\x80\x82" },
};
#define N_PARAS COUNT(paras)
#define P_THAI 4
#define P_JAPANESE 6
#define P1_PX 15.0f
#define P1_TOP 92.0f
static tb p1_body[N_PARAS], p1_info[N_PARAS], p1_total;
static float p1_width = 470;
static int p1_drag;
static ygfx_stim p1_handle, p1_panel[2];

static void p1_layout(void) {
    char s[200];
    float col_x[2] = { 24, 0 }, y[2] = { P1_TOP, P1_TOP };
    double total_us = 0;
    int i;
    col_x[1] = col_x[0] + p1_width + 48;
    for (i = 0; i < N_PARAS; i++) {
        ylay_style st;
        int c = i < 4 ? 0 : 1;
        memset(&st, 0, sizeof st);
        st.size = P1_PX; st.width = p1_width; st.lang = paras[i].lang; st.color = C_INK;
        tb_set(&p1_body[i], paras[i].ja ? &X_ja : &X_main, paras[i].text, &st, col_x[c], y[c] + 18);
        total_us += p1_body[i].b.layout_us;
        snprintf(s, sizeof s, "%s (%s): %u lines, %u glyphs, layout %.0f us", paras[i].name, paras[i].lang,
                 p1_body[i].b.n_lines, p1_body[i].b.n_items, p1_body[i].b.layout_us);
        label(&p1_info[i], col_x[c], y[c] + 12, 12, p1_body[i].b.n_missing ? C_RED : C_DIM, s);
        y[c] += 18 + (p1_body[i].b.h > 0 ? p1_body[i].b.h : P1_PX) + 14;
    }
    snprintf(s, sizeof s, "Width %.0f px; all seven laid out in %.2f ms (Skribidi and the glue; the first layout also builds glyphs).",
             p1_width, total_us * 1e-3);
    label(&p1_total, 24, 74, 13, C_AMBER, s);
    p1_panel[0] = rect(col_x[0] - 4, P1_TOP, p1_width + 8, y[0] - P1_TOP - 6, PANEL);
    p1_panel[1] = rect(col_x[1] - 4, P1_TOP, p1_width + 8, y[1] - P1_TOP - 6, PANEL);
    p1_handle = rect(col_x[0] + p1_width + 2, P1_TOP, 6, y[0] - P1_TOP - 6, p1_drag ? CARET : GRIP);
}

static void p1_draw(void) {
    int i;
    put(&p1_panel[0]); put(&p1_panel[1]); put(&p1_handle);
    for (i = 0; i < N_PARAS; i++) { tb_draw(&p1_body[i]); tb_draw(&p1_info[i]); }
    tb_draw(&p1_total);
}

/* --- page 2: mixed directions -------------------------------------------------- */

#define AR_PHRASE "\xd9\x85\xd9\x8e\xd8\xb1\xd9\x92\xd8\xad\xd9\x8e\xd8\xa8\xd9\x8b\xd8\xa7 \xd8\xa8\xd9\x90\xd8\xa7\xd9\x84\xd9\x92\xd8\xb9\xd9\x8e\xd8\xa7\xd9\x84\xd9\x8e\xd9\x85\xd9\x90"
typedef struct bidi_case { const char* text; int dir; const char* expect; const char* unpatched; } bidi_case;
static const bidi_case bidi_cases[] = {
    { "The word " AR_PHRASE " 123 means hello world.", YLAY_DIR_LTR,
      "LTR paragraph. Must read: \"The word\", then 123, then the Arabic phrase (right to left), then \"means hello world.\"",
      "Unpatched Skribidi put the Arabic phrase after \"world.\": the number's run merged with the LTR run after it." },
    { "\xd9\x85\xd8\xb9 English \xd9\x88 123", YLAY_DIR_RTL,
      "RTL paragraph. Must read from the right: the first Arabic word, \"English\", the one-letter word waw, then 123 at the left end.",
      "Unpatched Skribidi dropped the waw: a space run after \"English\" merged with it, and the merged run lost its glyph." },
    { "abc \xd9\x85\xd8\xb1\xd8\xad\xd8\xa8\xd8\xa7 123 def", YLAY_DIR_LTR,
      "LTR paragraph, the editor's caret string. Must read: abc, 123, the Arabic word (right to left), def.",
      "Unpatched Skribidi put the Arabic word after \"def\"." },
};
#define N_BIDI COUNT(bidi_cases)
static tb p2_line[N_BIDI], p2_expect[N_BIDI], p2_unp[N_BIDI], p2_note;
static ygfx_stim p2_panel[N_BIDI];

static void p2_setup(void) {
    int i;
    for (i = 0; i < N_BIDI; i++) {
        ylay_style st;
        float y = 96 + 190.0f * (float)i;
        uint32_t k, ncp = 0;
        memset(&st, 0, sizeof st);
        st.size = 44; st.dir = bidi_cases[i].dir; st.width = 1100;
        p2_line[i].pal = ramp_pal; p2_line[i].n_pal = 2;
        tb_set(&p2_line[i], &X_main, bidi_cases[i].text, &st, 50, y + 10);
        /* color by logical position: the ramp from the first codepoint to the
         * last; the runs read the items at each draw */
        for (k = 0; k < p2_line[i].b.n_items; k++) if (p2_line[i].b.cluster[k] + 1 > ncp) ncp = p2_line[i].b.cluster[k] + 1;
        for (k = 0; k < p2_line[i].b.n_items; k++)
            p2_line[i].b.items[k].color = ncp > 1 ? (float)p2_line[i].b.cluster[k] / (float)(ncp - 1) : 0.0f;
        label(&p2_expect[i], 50, y + 108, 15, C_LIGHT, bidi_cases[i].expect);
        label(&p2_unp[i], 50, y + 132, 13, C_DIM, bidi_cases[i].unpatched);
        p2_panel[i] = rect(40, y, 1120, 82, PANEL);
    }
    label(&p2_note, 24, 74, 13, C_AMBER,
          "Each glyph is colored by its place in the text, amber first and teal last: within a run the color steps one way, and runs keep their order.");
}

static void p2_draw(void) {
    int i;
    for (i = 0; i < N_BIDI; i++) { put(&p2_panel[i]); tb_draw(&p2_line[i]); tb_draw(&p2_expect[i]); tb_draw(&p2_unp[i]); }
    tb_draw(&p2_note);
}

/* --- page 3: phrase breaking ---------------------------------------------------- */

static float p3_width = 300;
static tb p3_body[4], p3_info[4], p3_note;
static ygfx_stim p3_panel[4];

static void p3_layout(void) {
    static const char* const langs[4] = { "ja", "en", "th", "en" };
    static const char* const what[4] = { "lang ja: BudouX phrases", "lang en: UAX 14, any two ideographs",
                                         "lang th: BudouX's Thai model", "lang en: spaces only, lines run past the box" };
    char s[200];
    int i;
    for (i = 0; i < 4; i++) {
        ylay_style st;
        float x = 40 + (float)(i % 2) * 560, y = 110 + (float)(i / 2) * 300;
        memset(&st, 0, sizeof st);
        st.size = 20; st.width = p3_width; st.lang = langs[i]; st.color = C_INK;
        tb_set(&p3_body[i], i < 2 ? &X_ja : &X_main, paras[i < 2 ? P_JAPANESE : P_THAI].text, &st, x, y);
        snprintf(s, sizeof s, "%s: %u lines", what[i], p3_body[i].b.n_lines);
        label(&p3_info[i], x, y - 10, 13, i % 2 ? C_DIM : C_TEAL, s);
        p3_panel[i] = rect(x - 4, y - 2, p3_width + 8, 260, PANEL);
    }
    snprintf(s, sizeof s, "Width %.0f px. The same text, Skribidi, two languages: BudouX keeps phrases whole where UAX 14 may break.", p3_width);
    label(&p3_note, 24, 74, 13, C_AMBER, s);
}

static void p3_draw(void) {
    int i;
    for (i = 0; i < 4; i++) { put(&p3_panel[i]); tb_draw(&p3_body[i]); tb_draw(&p3_info[i]); }
    tb_draw(&p3_note);
}

/* --- page 4: the editor ---------------------------------------------------------- */

#define ED_X 60.0f
#define ED_Y 150.0f
#define ED_W 1080.0f
#define ED_PX 30.0f
#define MAX_SEL 64
static skb_editor_t* editor;
static tb ed_text, ed_info, ed_note[2];
static ygfx_stim ed_panel, ed_caret, ed_sel[MAX_SEL];
static int ed_n_sel, ed_has_caret, ed_dragging, ed_composing;
static double ed_last_us;
static int64_t ed_edits;

static skb_rect2_t sel_raw[MAX_SEL];
static int sel_n_raw;

static void sel_rect(skb_rect2_t r, void* ctx) {
    (void)ctx;
    if (sel_n_raw < MAX_SEL) sel_raw[sel_n_raw++] = r;
}

static int sel_cmp(const void* a, const void* b) {
    const skb_rect2_t* A = (const skb_rect2_t*)a;
    const skb_rect2_t* B = (const skb_rect2_t*)b;
    if (A->y != B->y) return A->y < B->y ? -1 : 1;
    return A->x < B->x ? -1 : A->x > B->x;
}

/* Skribidi gives a selection one rectangle per run, and gives some twice.
 * Drawn as they come, their soft edges meet inside the band: two half-
 * covered pixels there let the panel show through as a thin line. The
 * rectangles of one line that touch or overlap become one band. */
static void sel_bands(void) {
    int i, m = 0;
    qsort(sel_raw, (size_t)sel_n_raw, sizeof sel_raw[0], sel_cmp);
    for (i = 0; i < sel_n_raw; i++) {
        skb_rect2_t* r = &sel_raw[i];
        if (m && sel_raw[m - 1].y == r->y && r->x <= sel_raw[m - 1].x + sel_raw[m - 1].width + 0.5f) {
            float end = r->x + r->width;
            if (end > sel_raw[m - 1].x + sel_raw[m - 1].width) sel_raw[m - 1].width = end - sel_raw[m - 1].x;
        } else {
            sel_raw[m++] = *r;
        }
    }
    for (i = 0; i < m; i++) ed_sel[ed_n_sel++] = rect(ED_X + sel_raw[i].x, ED_Y + sel_raw[i].y, sel_raw[i].width, sel_raw[i].height, SEL);
}

/* Lays the editor's paragraphs into ed_text, the selection and the caret:
 * after every change. t0: when the change began (yrt ns), for its cost;
 * 0 for none. */
static void ed_refresh(int64_t t0) {
    int i, n = skb_editor_get_paragraph_count(editor);
    skb_text_range_t selr = skb_editor_get_current_selection(editor);
    char s[200];
    ed_text.X = &X_main; ed_text.x = 0; ed_text.y = 0;
    for (i = 0; i < n; i++) {
        skb_vec2_t off = skb_editor_get_paragraph_offset(editor, i);
        if (ylay_from_skb(X_main.L, skb_editor_get_paragraph_layout(editor, i), ED_X + off.x, ED_Y + off.y, C_LIGHT, i > 0, &ed_text.b) != YLAY_OK)
            fail("editor", ylay_error(X_main.L));
    }
    xlib_sync(&X_main);
    tb_build(&ed_text);
    ed_n_sel = 0;
    ed_has_caret = 0;
    sel_n_raw = 0;
    if (skb_editor_get_text_range_count(editor, selr) > 0) {
        skb_editor_iterate_text_range_bounds(editor, selr, sel_rect, NULL);
        if (shot_select) {
            int k;
            printf("page 4: the selection is %d rectangles from Skribidi:", sel_n_raw);
            for (k = 0; k < sel_n_raw; k++) printf(" %.2f-%.2f", sel_raw[k].x, sel_raw[k].x + sel_raw[k].width);
        }
        sel_bands();
        if (shot_select) printf(", %d bands drawn\n", ed_n_sel);
    }
    else {
        skb_caret_info_t c = skb_editor_get_caret_info_at(editor, SKB_CURRENT_SELECTION_END);
        float h = -c.ascender + c.descender;
        ed_caret = rect(ED_X + c.x - 1, ED_Y + c.y + c.ascender, 2, h, CARET);
        ed_has_caret = 1;
        /* the input method's window goes by the caret */
        if (yscr_text_input(&scr, true, (int)(ED_X + c.x), (int)(ED_Y + c.y + c.ascender), 2, (int)h) < 0)
            fail("text input", yscr_error(&scr));
    }
    if (t0) {
        ed_last_us = (double)(yrt_now_ns() - t0) * 1e-3;
        ed_edits++;
    }
    if (ed_edits)
        snprintf(s, sizeof s, "Paragraphs %d, glyphs %u. Last change: %.0f us (Skribidi's edit and layout, the glue, new glyphs, their upload)%s",
                 n, ed_text.b.n_items, ed_last_us, ed_composing ? "; composing" : "");
    else
        snprintf(s, sizeof s, "Paragraphs %d, glyphs %u. Each change shows its cost here.", n, ed_text.b.n_items);
    label(&ed_info, 24, 74, 13, C_AMBER, s);
}

static int ed_setup(void) {
    static const char* initial =
        "abc \xd9\x85\xd8\xb1\xd8\xad\xd8\xa8\xd8\xa7 123 def. Type here: the caret moves through the bidi runs in logical order. "
        "\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d 2026 \xe5\x8f\x82\xe5\x8a\xa0\xe8\x80\x85";
    skb_attribute_t layout_attrs[2], para_attrs[1];
    skb_editor_params_t p;
    if (!X_main.n) return 0;
    layout_attrs[0] = skb_attribute_make_text_wrap(SKB_WRAP_WORD_CHAR);
    layout_attrs[1] = skb_attribute_make_line_height(SKB_LINE_HEIGHT_METRICS_RELATIVE, 1.3f);
    para_attrs[0] = skb_attribute_make_font_size(ED_PX);
    memset(&p, 0, sizeof p);
    p.font_collection = ylay_skb_fonts(X_main.L);
    p.attribute_collection = ylay_skb_attributes(X_main.L);
    p.editor_width = ED_W;
    p.editor_height = -1.0f;
    p.layout_attributes = (skb_attribute_set_t){ layout_attrs, 2, 0, NULL };
    p.paragraph_attributes = (skb_attribute_set_t){ para_attrs, 1, 0, NULL };
    editor = skb_editor_create(&p);
    if (!editor) return -1;
    skb_editor_set_text_utf8(editor, ylay_skb_temp(X_main.L), initial, -1);
    ed_text.pal = text_pal; ed_text.n_pal = N_COLORS;
    ed_panel = rect(ED_X - 12, ED_Y - 12, ED_W + 24, 420, PANEL);
    label(&ed_note[0], 24, WIN_H - 66, 14, C_AMBER,
          "Typed input is untimed: while this page has text input on, keys come through the message path, not the raw timed path.");
    label(&ed_note[1], 24, WIN_H - 48, 12, C_DIM,
          "yscr_text_input() turns it on for this page only; every flip meanwhile carries YSCR_FLIP_TEXT_INPUT and the ring gets a record.");
    return 0;
}

static uint32_t sdl_mods(SDL_Keymod m) {
    uint32_t r = 0;
    if (m & SDL_KMOD_SHIFT) r |= SKB_MOD_SHIFT;
    if (m & SDL_KMOD_CTRL) r |= SKB_MOD_CONTROL;
    if (m & SDL_KMOD_ALT) r |= SKB_MOD_ALT;
    return r;
}

/* UTF-8 to UTF-32 for the composition; returns the count. */
static int to_utf32(const char* s, uint32_t* out, int cap) {
    const unsigned char* p = (const unsigned char*)s;
    int n = 0;
    while (*p && n < cap) {
        uint32_t c = *p++;
        if (c >= 0xf0 && p[0] && p[1] && p[2]) { c = (c & 7) << 18 | (uint32_t)(p[0] & 0x3f) << 12 | (uint32_t)(p[1] & 0x3f) << 6 | (p[2] & 0x3f); p += 3; }
        else if (c >= 0xe0 && p[0] && p[1]) { c = (c & 15) << 12 | (uint32_t)(p[0] & 0x3f) << 6 | (p[1] & 0x3f); p += 2; }
        else if (c >= 0xc0 && p[0]) { c = (c & 31) << 6 | (p[0] & 0x3f); p += 1; }
        out[n++] = c;
    }
    return n;
}

/* One key for the editor; 1 when it changed something. */
static int ed_key(SDL_Keycode key, SDL_Keymod mod) {
    skb_temp_alloc_t* tmp = ylay_skb_temp(X_main.L);
    uint32_t m = sdl_mods(mod);
    skb_editor_key_t k = SKB_KEY_NONE;
    if (mod & SDL_KMOD_CTRL) {
        if (key == SDLK_Z && (mod & SDL_KMOD_SHIFT)) { skb_editor_redo(editor, tmp); return 1; }
        if (key == SDLK_Z) { skb_editor_undo(editor, tmp); return 1; }
        if (key == SDLK_Y) { skb_editor_redo(editor, tmp); return 1; }
        if (key == SDLK_A) { skb_editor_select_all(editor); return 1; }
    }
    switch (key) {
    case SDLK_LEFT: k = SKB_KEY_LEFT; break;
    case SDLK_RIGHT: k = SKB_KEY_RIGHT; break;
    case SDLK_UP: k = SKB_KEY_UP; break;
    case SDLK_DOWN: k = SKB_KEY_DOWN; break;
    case SDLK_HOME: k = SKB_KEY_HOME; break;
    case SDLK_END: k = SKB_KEY_END; break;
    case SDLK_BACKSPACE: k = SKB_KEY_BACKSPACE; break;
    case SDLK_DELETE: k = SKB_KEY_DELETE; break;
    case SDLK_RETURN: case SDLK_KP_ENTER: k = SKB_KEY_ENTER; break;
    default: return 0;
    }
    skb_editor_process_key_pressed(editor, tmp, k, m);
    return 1;
}

static void ed_text_input(const char* utf8) {
    skb_temp_alloc_t* tmp = ylay_skb_temp(X_main.L);
    if (ed_composing) {
        uint32_t u[256];
        int n = to_utf32(utf8, u, 256);
        skb_editor_commit_composition_utf32(editor, tmp, u, n);
        ed_composing = 0;
    } else {
        skb_editor_insert_text_utf8(editor, tmp, SKB_CURRENT_SELECTION, utf8, -1);
    }
}

static void ed_editing(const char* utf8, int start) {
    skb_temp_alloc_t* tmp = ylay_skb_temp(X_main.L);
    uint32_t u[256];
    int n = to_utf32(utf8, u, 256);
    if (n == 0) { if (ed_composing) skb_editor_clear_composition(editor, tmp); ed_composing = 0; return; }
    skb_editor_set_composition_utf32(editor, tmp, u, n, start >= 0 ? start : n);
    ed_composing = 1;
}

static void p4_draw(void) {
    int i;
    put(&ed_panel);
    for (i = 0; i < ed_n_sel; i++) put(&ed_sel[i]);
    tb_draw(&ed_text);
    if (ed_has_caret) put(&ed_caret);
    tb_draw(&ed_info);
    tb_draw(&ed_note[0]); tb_draw(&ed_note[1]);
}

/* Page 4 in --sim: a scripted edit through every editor path but the IME
 * window: typing, a composition and its commit, moves, a selection, undo. */
static void ed_script(void) {
    static const char* const typed[3] = { "x", "\xd8\xb3", "1" };
    int64_t t0;
    int i;
    for (i = 0; i < 3; i++) { t0 = yrt_now_ns(); ed_text_input(typed[i]); ed_refresh(t0); }
    t0 = yrt_now_ns(); ed_editing("\xe3\x81\x8b\xe3\x81\xaa", 2); ed_refresh(t0);
    t0 = yrt_now_ns(); ed_text_input("\xe4\xbb\xae\xe5\x90\x8d"); ed_refresh(t0);
    for (i = 0; i < 4; i++) { t0 = yrt_now_ns(); ed_key(SDLK_LEFT, SDL_KMOD_NONE); ed_refresh(t0); }
    t0 = yrt_now_ns(); ed_key(SDLK_RIGHT, SDL_KMOD_SHIFT); ed_refresh(t0);
    t0 = yrt_now_ns(); ed_key(SDLK_Z, SDL_KMOD_CTRL); ed_refresh(t0);
    printf("page 4 script: %lld changes, the last (an undo) %.0f us; %u glyphs\n", (long long)ed_edits, ed_last_us, ed_text.b.n_items);
}

/* --- setup and the loop --------------------------------------------------------- */

static int read_font(font_file* F, const char* dir) {
    char path[600];
    FILE* fp;
    long n;
    snprintf(path, sizeof path, "%s/%s", dir, F->file);
    fp = fopen(path, "rb");
    if (!fp) { snprintf(F->why, sizeof F->why, "%s (%s not found)", F->name, path); return 0; }
    fseek(fp, 0, SEEK_END); n = ftell(fp); fseek(fp, 0, SEEK_SET);
    F->bytes = n > 0 ? (uint8_t*)malloc((size_t)n) : NULL;
    if (!F->bytes || fread(F->bytes, 1, (size_t)n, fp) != (size_t)n) {
        free(F->bytes); F->bytes = NULL; fclose(fp);
        snprintf(F->why, sizeof F->why, "%s (%s could not be read)", F->name, path);
        return 0;
    }
    fclose(fp);
    F->n = (size_t)n;
    return 1;
}

/* sRGB's primaries and white at gamma 2.2. Not a measurement. */
static int cal_setup(void) {
    static const float xy[4][2] = { { 0.64f, 0.33f }, { 0.30f, 0.60f }, { 0.15f, 0.06f }, { 0.3127f, 0.3290f } };
    if (ycol_cal_nominal(&cal, xy, 80.0f, 2.2) < 0) return -1;
    ycol_cal_save(&cal, NULL, 0);   /* seals the CRC, which open() checks */
    return 0;
}

static void show_page(int p, int prev) {
    char title[160];
    /* SDL and printf may allocate: only here, on a page change */
    snprintf(title, sizeof title, "gfx_layout %d/%d: %s", p + 1, N_PAGES, page_names[p]);
    if (yscr_window(&scr)) SDL_SetWindowTitle(yscr_window(&scr), title);
    printf("page %d/%d: %s\n", p + 1, N_PAGES, page_names[p]);
    fflush(stdout);
    if (prev == 3) yscr_text_input(&scr, false, 0, 0, 0, 0);
    cur_what = page_names[p];
    frame_static(p);
    if (p == 3 && editor) {
        ed_refresh(0);   /* turns text input on at the caret */
        if (sim) ed_script();
        if (shot_select) {
            /* " مرحبا 123 " of "abc مرحبا 123 def.": one band on the screen,
             * four runs in Skribidi */
            skb_editor_select(editor, (skb_text_range_t){ { 3, SKB_AFFINITY_TRAILING }, { 14, SKB_AFFINITY_TRAILING } });
            ed_refresh(0);
        }
    }
}

int main(int argc, char** argv) {
    static const int main_order[] = { F_SEGOE, F_ARIAL, F_NIRMALA, F_LEELAWADEE, F_YAHEI };
    static const int ja_order[] = { F_SEGOE, F_YUGOTHIC };
    yscr_desc sd;
    ygfx_desc gd;
    yscr_frame f;
    const char* shots = NULL;
    const char* fonts_dir = "C:/Windows/Fonts";
    static ygfx_file_cache pcache;
    static char cache_dir[512];
    const ygfx_cache* cache = NULL;
    int no_cache = 0;
    uint8_t* shot_px = NULL;
    int64_t frames = -1, on_page = 0;
    int i, cur = 0, next, started = 0, quit = 0, pages_done = 0, n_fonts = 0, prev;
    static uint8_t key_down[SDL_SCANCODE_COUNT];
    char line[640];

    memset(&sd, 0, sizeof sd);
    sd.windowed = true; sd.window_w = WIN_W; sd.window_h = WIN_H;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--sim")) { sd.backend = YSCR_BACKEND_SIM; sim = 1; }
        else if (!strcmp(argv[i], "--composition")) sd.backend = YSCR_BACKEND_COMPOSITION;
        else if (!strcmp(argv[i], "--page") && i + 1 < argc) cur = atoi(argv[++i]) - 1;
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) { frames = atoll(argv[++i]); if (frames < 1) cur = -1; }
        else if (!strcmp(argv[i], "--shots") && i + 1 < argc) { shots = argv[++i]; shot_select = 1; }
        else if (!strcmp(argv[i], "--cache") && i + 1 < argc) { cache = ygfx_file_cache_init(&pcache, argv[++i]); if (!cache) cur = -1; }
        else if (!strcmp(argv[i], "--no-cache")) no_cache = 1;
        else if (!strcmp(argv[i], "--fonts") && i + 1 < argc) fonts_dir = argv[++i];
        else cur = -1;
        if (cur < 0 || cur >= N_PAGES) {
            fprintf(stderr, "usage: gfx_layout [--sim] [--page 1..%d] [--frames N] [--composition] [--shots PREFIX] "
                            "[--cache DIR] [--no-cache] [--fonts DIR]\n", N_PAGES);
            return 2;
        }
    }
    /* The per-user folder unless told otherwise, so that a second run loads
     * the programs instead of compiling them (PROGRAM CACHE). */
    if (no_cache) cache = NULL;
    else if (!cache && ygfx_default_cache_dir(cache_dir, sizeof cache_dir) == YGFX_OK)
        cache = ygfx_file_cache_init(&pcache, cache_dir);
    for (i = 0; i < N_FILES; i++) {
        n_fonts += read_font(&files[i], fonts_dir);
        if (files[i].why[0]) printf("missing font: %s\n", files[i].why);
    }
    if (cal_setup() < 0) { fprintf(stderr, "gfx_layout: the nominal calibration failed\n"); return 1; }
    if (!yscr_open(&scr, &sd)) { fprintf(stderr, "gfx_layout: %s\n", yscr_error(&scr)); return 1; }
    memset(&gd, 0, sizeof gd);
    gd.screen = &scr;
    rgb(gd.background, BG);
    gd.cal = &cal;
    gd.width = WIN_W; gd.height = WIN_H;   /* the simulated display's size */
    gd.cache = cache;
    if (!ygfx_open(&gfx, &gd)) { fprintf(stderr, "gfx_layout: %s\n", ygfx_error(&gfx)); yscr_close(&scr); return 1; }
    yscr_describe(&scr, line, sizeof line);
    printf("%s\n", line);
    ygfx_describe(&gfx, line, sizeof line);
    printf("%s\n", line);
    if (yol_init(&ol, NULL) != YOL_OK || xlib_open(&X_main, main_order, COUNT(main_order)) < 0 ||
        xlib_open(&X_ja, ja_order, COUNT(ja_order)) < 0) {
        fprintf(stderr, "gfx_layout: the layout library did not open\n");
        ygfx_close(&gfx); yscr_close(&scr);
        return 1;
    }
    for (i = 0; i < N_FILES; i++) if (files[i].why[0] && files[i].bytes) printf("refused font: %s\n", files[i].why);
    if (!n_fonts) printf("no font opened (%s): every page draws nothing and says so here\n", fonts_dir);
    ylay_stamp(X_main.L, NULL, stamp_line, sizeof stamp_line);
    printf("%s\n", stamp_line);
    {
        const ylay_versions* v = ylay_get_versions();
        snprintf(stamp_short, sizeof stamp_short, "Skribidi %.7s with patches %.19s..., HarfBuzz %s, ysp_layout %s (the whole stamp, with the font hashes, is on stdout)",
                 v->skribidi, v->skribidi_patch, v->harfbuzz, v->ylay);
    }

    /* Setup: every page's static text, laid out once; the glyphs they use
     * become the first curve sets. */
    {
        int64_t t0 = yrt_now_ns();
        p1_layout();
        p2_setup();
        p3_layout();
        if (ed_setup() < 0) { fprintf(stderr, "gfx_layout: the editor did not open\n"); return 1; }
        printf("setup: every page laid out, glyphs built and uploaded in %.0f ms\n", (double)(yrt_now_ns() - t0) * 1e-6);
    }
    if (n_fonts) {
        int k;
        for (k = 0; k < X_main.n; k++) {
            ylay_font_info fi;
            ylay_font(X_main.L, k, &fi);
            printf("  %-14s %5u glyphs built in %6.1f ms\n", fi.name, fi.n_built, fi.build_us * 1e-3);
        }
    }
    if (shots) shot_px = (uint8_t*)malloc((size_t)WIN_W * WIN_H * 4);

    next = cur;
    while (yscr_begin(&scr, &f) == YSCR_OK) {
        if (yscr_window(&scr)) {
            SDL_Event ev;
            while (yscr_poll(&scr, &ev, NULL)) {   /* yscr_begin() reports Shift+Esc and close itself */
                if (ev.type == SDL_EVENT_QUIT || ev.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) quit = 1;
                else if (ev.type == SDL_EVENT_KEY_UP) { if (ev.key.scancode < SDL_SCANCODE_COUNT) key_down[ev.key.scancode] = 0; }
                else if (ev.type == SDL_EVENT_KEY_DOWN) {
                    SDL_Keycode key = ev.key.key;
                    /* A key injected by virtual-key code arrives twice
                     * (ysp/screen.h INPUT): a press counts after a release */
                    if (!ev.key.repeat && ev.key.scancode < SDL_SCANCODE_COUNT) {
                        if (key_down[ev.key.scancode]) continue;
                        key_down[ev.key.scancode] = 1;
                    }
                    /* Page 4 takes letters, digits and arrows for editing, so
                     * F1 to F4 and Page Up/Down change pages everywhere;
                     * Page Up/Down need Fn on many laptops. */
                    if (key == SDLK_PAGEDOWN) { next = (cur + 1) % N_PAGES; continue; }
                    if (key == SDLK_PAGEUP) { next = (cur + N_PAGES - 1) % N_PAGES; continue; }
                    if (key >= SDLK_F1 && key < SDLK_F1 + (SDL_Keycode)N_PAGES) { next = (int)(key - SDLK_F1); continue; }
                    if (cur == 3 && editor) {
                        int64_t t0 = yrt_now_ns();
                        if (!ed_composing && ed_key(key, ev.key.mod)) ed_refresh(t0);
                        continue;
                    }
                    if (ev.key.repeat && key != SDLK_LEFTBRACKET && key != SDLK_RIGHTBRACKET) continue;
                    if (key == SDLK_RIGHT || key == SDLK_DOWN) next = (cur + 1) % N_PAGES;
                    else if (key == SDLK_LEFT || key == SDLK_UP) next = (cur + N_PAGES - 1) % N_PAGES;
                    else if (key >= SDLK_1 && key < SDLK_1 + (SDL_Keycode)N_PAGES) next = (int)(key - SDLK_1);
                    else if ((key == SDLK_LEFTBRACKET || key == SDLK_RIGHTBRACKET) && (cur == 0 || cur == 2)) {
                        float* w = cur == 0 ? &p1_width : &p3_width;
                        *w += key == SDLK_LEFTBRACKET ? -10.0f : 10.0f;
                        if (*w < 120) *w = 120;
                        if (*w > 540) *w = 540;
                        if (cur == 0) p1_layout(); else p3_layout();
                    }
                } else if (ev.type == SDL_EVENT_TEXT_INPUT && cur == 3 && editor) {
                    int64_t t0 = yrt_now_ns();
                    ed_text_input(ev.text.text);
                    ed_refresh(t0);
                } else if (ev.type == SDL_EVENT_TEXT_EDITING && cur == 3 && editor) {
                    int64_t t0 = yrt_now_ns();
                    ed_editing(ev.edit.text, ev.edit.start);
                    ed_refresh(t0);
                } else if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN && ev.button.button == SDL_BUTTON_LEFT) {
                    float hx = 24 + p1_width + 2;
                    if (cur == 0 && ev.button.x >= hx - 6 && ev.button.x <= hx + 14 && ev.button.y >= P1_TOP) { p1_drag = 1; p1_layout(); }
                    if (cur == 3 && editor) {
                        int64_t t0 = yrt_now_ns();
                        skb_editor_process_mouse_click(editor, ev.button.x - ED_X, ev.button.y - ED_Y, sdl_mods(SDL_GetModState()),
                                                       (double)ev.button.timestamp * 1e-9);
                        ed_dragging = 1;
                        ed_refresh(t0);
                    }
                } else if (ev.type == SDL_EVENT_MOUSE_BUTTON_UP && ev.button.button == SDL_BUTTON_LEFT) {
                    if (p1_drag) { p1_drag = 0; p1_layout(); }
                    ed_dragging = 0;
                } else if (ev.type == SDL_EVENT_MOUSE_MOTION) {
                    if (p1_drag && cur == 0) {
                        float w = ev.motion.x - 24 - 4;
                        if (w < 120) w = 120;
                        if (w > 540) w = 540;
                        if (w != p1_width) { p1_width = w; p1_layout(); }
                    }
                    if (ed_dragging && cur == 3 && editor) {
                        int64_t t0 = yrt_now_ns();
                        skb_editor_process_mouse_drag(editor, ev.motion.x - ED_X, ev.motion.y - ED_Y);
                        ed_refresh(t0);
                    }
                }
            }
        }
        if (!started || next != cur) { prev = started ? cur : -1; cur = next; show_page(cur, prev); on_page = 0; started = 1; }

        ygfx_begin(&gfx, &f);
        cur_what = page_names[cur];
        if (cur == 0) p1_draw();
        else if (cur == 1) p2_draw();
        else if (cur == 2) p3_draw();
        else p4_draw();
        for (i = 0; i < 5; i++) tb_draw(&frame_tb[i]);
        ygfx_end(&gfx);
        on_page++;
        if (shot_px && on_page == SHOT_FRAMES) {   /* the back buffer, before the flip */
            char path[512];
            snprintf(path, sizeof path, "%s-p%d.ppm", shots, cur + 1);
            if (ygfx_read_output(&gfx, 0, 0, WIN_W, WIN_H, shot_px) < 0 || ygfx_image_write(path, YGFX_IMAGE_PPM, shot_px, WIN_W, WIN_H, 0, NULL) < 0)
                fprintf(stderr, "gfx_layout: could not write %s\n", path);
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
    if (cur == 3) yscr_text_input(&scr, false, 0, 0, 0, 0);
    if (editor) skb_editor_destroy(editor);
    free(shot_px);
    for (i = 0; i < N_PARAS; i++) { ylay_block_free(&p1_body[i].b); ylay_block_free(&p1_info[i].b); }
    for (i = 0; i < N_BIDI; i++) { ylay_block_free(&p2_line[i].b); ylay_block_free(&p2_expect[i].b); ylay_block_free(&p2_unp[i].b); }
    for (i = 0; i < 4; i++) { ylay_block_free(&p3_body[i].b); ylay_block_free(&p3_info[i].b); }
    for (i = 0; i < 6; i++) ylay_block_free(&frame_tb[i].b);
    ylay_block_free(&p1_total.b); ylay_block_free(&p2_note.b); ylay_block_free(&p3_note.b);
    ylay_block_free(&ed_text.b); ylay_block_free(&ed_info.b); ylay_block_free(&ed_note[0].b); ylay_block_free(&ed_note[1].b);
    ygfx_close(&gfx);
    ylay_destroy(X_main.L);
    ylay_destroy(X_ja.L);
    yol_free(&ol);
    for (i = 0; i < N_FILES; i++) free(files[i].bytes);
    yscr_close(&scr);
    if (refused) { fprintf(stderr, "gfx_layout: %ld refused draws or setup errors\n", refused); return 1; }
    return 0;
}
