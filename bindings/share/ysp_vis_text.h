/* ysp_vis_text.h - the host-independent text part of ysp-vis (Python ysp.gfx, MEX ysp_gfx)
 *
 * Not a library header. ysp_vis_core.h includes it after ysp_vis_gfx.h.
 * Text is laid out by pack/layout (ylay_: Skribidi with HarfBuzz, the
 * pinned and patched layout of docs/layout.md, the same as the pack tool
 * and the player use), and drawn as ysp/gfx.h curve runs over ysp/outline.h
 * curve sets built from the same font bytes.
 *
 * FONTS. A yv_font is one fallback list: a ylay_lib with its own yol_ctx,
 * the bytes of each file (owned here), and per file a curve set on the
 * gfx that grows as layouts use new glyphs. A text's runs use the sets of
 * its font. A font named by path in a text (font="arial.ttf") is made once
 * per gfx and kept (implicit); the gfx frees those last.
 *
 * TEXTS. A text is a yv_stim of vkind YV_K_TEXT: st->s is its first run
 * (one font at one size), placed as every stimulus (place, anchor, x, y,
 * ori, opacity, color, group), so lists, bulk sets, timeline bindings and
 * the attribute table work on it unchanged. A text with more runs (a
 * fallback font, a span with another size) draws them as copies of st->s
 * with that run's set, items and size: every run shares the block's box, so
 * the copies stay in register whatever the transform.
 *
 * UNITS. Sizes, widths and positions are the gfx's units (px or deg):
 * size is units per em. Character indices are codepoints here; a host
 * converts its own (Python str indices are codepoints; MATLAB's char is
 * UTF-16, Octave's UTF-8 bytes).
 *
 * MEMORY. Making fonts and texts, and laying out (a change of text, size,
 * width, ...), allocate: setup, between frames. Drawing a text allocates
 * nothing; a layout of the same text again grows nothing in the glue
 * (Skribidi allocates inside each layout: docs/layout.md, Cost).
 *
 * Public domain / MIT-0, as the headers.
 */
#ifndef YSP_VIS_TEXT_H
#define YSP_VIS_TEXT_H

#ifndef YSP_VIS_GFX_H
#error "include ysp_vis_core.h, which includes this file"
#endif

#if defined(_WIN32)
#include <wchar.h>
#endif

#define YV_FONT_MAX   16      /* files in one font's fallback list          */
#define YV_TEXT_MAX   (1 << 20)   /* bytes of one text                      */

typedef struct yv_font {
    yv_obj     o;              /* YV_O_FONT                                   */
    yol_ctx    ol;
    int        ol_open;
    ylay_lib*  L;
    int        n;              /* files added                                  */
    uint8_t*   bytes[YV_FONT_MAX];
    size_t     nbytes[YV_FONT_MAX];
    ygfx_cset  set[YV_FONT_MAX];   /* on the gfx; 0 until a layout uses it    */
    int        implicit;       /* made for a text's font= paths: the gfx frees it */
    char*      key;            /* implicit: the paths and faces, joined        */
} yv_font;

typedef struct yv_text {
    yv_font*   font;           /* users counted                                */
    char*      utf8;
    uint32_t   n_bytes, cap_bytes;
    uint32_t*  cp_off;         /* byte offset of codepoint i, n_cp + 1 entries */
    uint32_t   n_cp, cap_cp;
    ylay_style style;
    char       lang[48];
    ylay_span* spans;          /* byte offsets, for ylay                       */
    double*    span_v;         /* the host's: start, end (codepoints), r, g, b, size per span */
    int        n_spans, cap_spans;
    ylay_block b;
    double     layout_s, build_s, upload_s;   /* the last layout's parts        */
    uint32_t   n_new;          /* glyphs it built                              */
    uint64_t   layouts;
} yv_text;

/* ---- files ---------------------------------------------------------------- */

/* The whole file at a UTF-8 path, on the heap. 0, or ERR_IO with err. */
static int yv_read_file(const char* path, uint8_t** out, size_t* n, char* err, size_t cap) {
    FILE* fp = NULL;
    long len;
    uint8_t* b;
#if defined(_WIN32)
    {
        wchar_t w[1024];
        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, w, 1024) > 0) fp = _wfopen(w, L"rb");
    }
#else
    fp = fopen(path, "rb");
#endif
    if (!fp) return yv_fail(err, cap, YGFX_ERR_IO, "font: %s does not open", path);
    if (fseek(fp, 0, SEEK_END) != 0 || (len = ftell(fp)) <= 0 || fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return yv_fail(err, cap, YGFX_ERR_IO, "font: %s is empty or unreadable", path);
    }
    b = (uint8_t*)malloc((size_t)len);
    if (!b) { fclose(fp); return yv_fail(err, cap, YGFX_ERR_FULL, "out of memory"); }
    if (fread(b, 1, (size_t)len, fp) != (size_t)len) {
        fclose(fp);
        free(b);
        return yv_fail(err, cap, YGFX_ERR_IO, "font: %s could not be read", path);
    }
    fclose(fp);
    *out = b;
    *n = (size_t)len;
    return 0;
}

/* The system's sans font, as ygfx_text_make() takes it on Windows: Segoe
 * UI, then Arial. "" when neither is there. */
static const char* yv_default_font_path(char* buf, size_t cap) {
#if defined(_WIN32)
    static const char* names[] = { "segoeui.ttf", "arial.ttf" };
    char win[512];
    DWORD n = GetEnvironmentVariableA("WINDIR", win, (DWORD)sizeof win);
    int i;
    if (n == 0 || n >= sizeof win) snprintf(win, sizeof win, "C:\\Windows");
    for (i = 0; i < 2; i++) {
        FILE* fp;
        snprintf(buf, cap, "%s\\Fonts\\%s", win, names[i]);
        fp = fopen(buf, "rb");
        if (fp) { fclose(fp); return buf; }
    }
#endif
    if (cap) buf[0] = 0;
    return buf;
}

/* ---- fonts ---------------------------------------------------------------- */

/* One file of a fallback list: a path, or bytes the host gives (copied). */
typedef struct yv_font_src {
    const char* path;
    const void* bytes;
    size_t      n;
    int         face;
    const char* name;          /* bytes: the name the stamp gives; NULL: "bytes" */
} yv_font_src;

static void yv_font_free(yv_font* f) {
    int i;
    if (!f) return;
    if (f->o.g && f->o.g->open)
        for (i = 0; i < f->n; i++)
            if (f->set[i].id) ygfx_cset_free(&f->o.g->g, f->set[i]);
    if (f->L) ylay_destroy(f->L);
    if (f->ol_open) yol_close(&f->ol);
    for (i = 0; i < f->n; i++) free(f->bytes[i]);
    free(f->key);
    yv_unlink(&f->o);
    free(f);
}

static const char* yv_base_name(const char* p) {
    const char* b = p;
    for (; *p; p++) if (*p == '/' || *p == '\\') b = p + 1;
    return b;
}

/* A fallback list of n files. NULL with err. */
static yv_font* yv_font_make(yv_gfx* g, const yv_font_src* src, int n, const char* key, int* code, char* err, size_t cap) {
    yv_font* f;
    ylay_desc ld;
    int i, rc;
    *code = YGFX_ERR_ARG;
    if (!g->open) { *code = yv_fail(err, cap, YGFX_ERR_CLOSED, "ysp_gfx: the gfx (or its screen) is closed"); return NULL; }
    if (n < 1 || n > YV_FONT_MAX) { yv_fail(err, cap, YGFX_ERR_ARG, "font: 1 to %d files in the fallback list", YV_FONT_MAX); return NULL; }
    f = (yv_font*)calloc(1, sizeof *f);
    if (!f) { *code = yv_fail(err, cap, YGFX_ERR_FULL, "out of memory"); return NULL; }
    yv_link(g, &f->o, YV_O_FONT);
    if (!yol_open(&f->ol, NULL)) { *code = yv_fail(err, cap, YGFX_ERR_FULL, "font: the outline context did not open"); goto fail; }
    f->ol_open = 1;
    memset(&ld, 0, sizeof ld);
    ld.ol = &f->ol;
    if (ylay_create(&f->L, &ld) != YLAY_OK) { *code = yv_fail(err, cap, YGFX_ERR_FULL, "font: the layout library did not open"); goto fail; }
    for (i = 0; i < n; i++) {
        char name[96];
        if (src[i].face < 0) { yv_fail(err, cap, YGFX_ERR_ARG, "font: face is 0 or more"); goto fail; }
        if (src[i].path) {
            rc = yv_read_file(src[i].path, &f->bytes[i], &f->nbytes[i], err, cap);
            if (rc < 0) { *code = rc; goto fail; }
            snprintf(name, sizeof name, "%s", yv_base_name(src[i].path));
        } else {
            if (!src[i].bytes || src[i].n < 12) { yv_fail(err, cap, YGFX_ERR_ARG, "font: the bytes of a font file"); goto fail; }
            f->bytes[i] = (uint8_t*)malloc(src[i].n);
            if (!f->bytes[i]) { *code = yv_fail(err, cap, YGFX_ERR_FULL, "out of memory"); goto fail; }
            memcpy(f->bytes[i], src[i].bytes, src[i].n);
            f->nbytes[i] = src[i].n;
            snprintf(name, sizeof name, "%s", src[i].name && src[i].name[0] ? src[i].name : "bytes");
        }
        f->n = i + 1;   /* the bytes are f's from here (freed by yv_font_free) */
        rc = ylay_add_font(f->L, f->bytes[i], f->nbytes[i], src[i].face, name);
        if (rc < 0) {
            *code = yv_fail(err, cap, YGFX_ERR_FORMAT, "font: %s: %s", src[i].path ? src[i].path : name, ylay_error(f->L));
            goto fail;
        }
    }
    if (key) {
        f->implicit = 1;
        f->key = (char*)malloc(strlen(key) + 1);
        if (!f->key) { *code = yv_fail(err, cap, YGFX_ERR_FULL, "out of memory"); goto fail; }
        memcpy(f->key, key, strlen(key) + 1);
    }
    return f;
fail:
    yv_font_free(f);
    return NULL;
}

/* The gfx closed: its GL objects went with it (yv_gfx_close). */
static void yv_font_lost(yv_obj* o) {
    yv_font* f = (yv_font*)o;
    int i;
    for (i = 0; i < YV_FONT_MAX; i++) f->set[i].id = 0;
}

/* A font made for a key before (implicit), or NULL. */
static yv_font* yv_font_find(yv_gfx* g, const char* key) {
    yv_obj* o;
    for (o = g->objs; o; o = o->next)
        if (o->type == YV_O_FONT && ((yv_font*)o)->implicit && !strcmp(((yv_font*)o)->key, key)) return (yv_font*)o;
    return NULL;
}

/* The gfx's default font: the system's sans font, made once. */
static yv_font* yv_font_default(yv_gfx* g, int* code, char* err, size_t cap) {
    yv_font* f = yv_font_find(g, "<default>");
    yv_font_src s;
    char path[600];
    if (f) return f;
    if (!yv_default_font_path(path, sizeof path)[0]) {
        *code = yv_fail(err, cap, YGFX_ERR_IO, "font: no system font (Segoe UI, Arial) was found: give font= a file");
        return NULL;
    }
    memset(&s, 0, sizeof s);
    s.path = path;
    return yv_font_make(g, &s, 1, "<default>", code, err, cap);
}

/* Paths joined with their faces: an implicit font's key. */
static int yv_font_key(const yv_font_src* src, int n, char* key, size_t cap) {
    size_t k = 0;
    int i;
    key[0] = 0;
    for (i = 0; i < n; i++) {
        int w;
        if (!src[i].path) return -1;
        w = snprintf(key + k, cap - k, "%s%s#%d", i ? "|" : "", src[i].path, src[i].face);
        if (w < 0 || (size_t)w >= cap - k) return -1;
        k += (size_t)w;
    }
    return 0;
}

/* The glyphs the layouts built since the last call go to the gfx: into a
 * set with room, or a new bigger set (the texts take the new id at their
 * next draw). Between frames: an upload. */
static int yv_font_sync(yv_font* f, char* err, size_t cap) {
    yv_gfx* g = f->o.g;
    int i;
    for (i = 0; i < f->n; i++) {
        const uint32_t* gl;
        int k = ylay_take_new_glyphs(f->L, i, &gl), rc;
        ylay_font_info fi;
        ygfx_cset_desc d;
        int first = 0;
        if (k <= 0) continue;
        ylay_font(f->L, i, &fi);
        memset(&d, 0, sizeof d);
        d.texels = fi.set->texels; d.n_texels = fi.set->n_texels;
        d.words = fi.set->words; d.n_words = fi.set->n_words;
        if (f->set[i].id) {
            rc = ygfx_cset_add(&g->g, f->set[i], &d, gl, k);
            if (rc == YGFX_ERR_FULL) { ygfx_cset_free(&g->g, f->set[i]); f->set[i].id = 0; }
            else if (rc < 0) return yv_gfx_err(g, rc, err, cap);
        }
        if (!f->set[i].id) {
            /* room for text typed or changed later: about 1000 Latin or 100
             * CJK glyphs (examples/layout/gfx_layout.c's) */
            d.cap_texels = d.n_texels + d.n_texels / 2 + 16384;
            d.cap_words = (d.n_words + d.n_words / 2 + 65536 + 3) & ~3u;
            first = !g->text_ready;
            f->set[i] = ygfx_cset_make(&g->g, &d);
            if (!f->set[i].id) return yv_gfx_err(g, YGFX_ERR_FULL, err, cap);
            g->text_ready = 1;
        }
        /* The first set made the text program: prime it, so the first
         * frame that draws text does not pay its first draw (TEXT, PRIME).
         * ygfx_prime() reads a pixel back; between a flip and its
         * completion that read blocked 19 s to more than 5 min (measured,
         * docs/gfx.md "Bindings", text), so the flips complete first. */
        if (first && g->prime) {
            if (g->scr && !g->scr->in_frame) yscr_wait_flip(&g->scr->s, NULL);   /* ORDER before any flip: nothing to wait for */
            if (ygfx_prime(&g->g) < 0) return yv_gfx_err(g, YGFX_ERR_GL, err, cap);
        }
    }
    return 0;
}

/* ---- texts ---------------------------------------------------------------- */

static void yv_text_free(yv_text* t) {
    if (!t) return;
    if (t->font) t->font->o.users--;
    ylay_block_free(&t->b);
    free(t->utf8);
    free(t->cp_off);
    free(t->spans);
    free(t->span_v);
    free(t);
}

/* Decodes UTF-8 enough to index it: the byte offset of every codepoint.
 * Refuses bytes that are not UTF-8 (the host converts its strings). */
static int yv_text_index(yv_text* t, char* err, size_t cap) {
    uint32_t i = 0, n = 0;
    const unsigned char* s = (const unsigned char*)t->utf8;
    if (t->n_bytes + 1 > t->cap_cp) {
        uint32_t* p = (uint32_t*)realloc(t->cp_off, sizeof(uint32_t) * (t->n_bytes + 1));
        if (!p) return yv_fail(err, cap, YGFX_ERR_FULL, "out of memory");
        t->cp_off = p;
        t->cap_cp = t->n_bytes + 1;
    }
    while (i < t->n_bytes) {
        unsigned c = s[i];
        uint32_t len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0, k;
        if (!len || i + len > t->n_bytes) return yv_fail(err, cap, YGFX_ERR_ARG, "text: not UTF-8 at byte %u", i);
        for (k = 1; k < len; k++)
            if ((s[i + k] & 0xC0) != 0x80) return yv_fail(err, cap, YGFX_ERR_ARG, "text: not UTF-8 at byte %u", i + k);
        t->cp_off[n++] = i;
        i += len;
    }
    t->cp_off[n] = t->n_bytes;
    t->n_cp = n;
    return 0;
}

static uint32_t yv_text_cp_at(const yv_text* t, uint32_t i) {
    const unsigned char* s = (const unsigned char*)t->utf8 + t->cp_off[i];
    if (s[0] < 0x80) return s[0];
    if ((s[0] >> 5) == 6) return (uint32_t)(s[0] & 0x1F) << 6 | (s[1] & 0x3F);
    if ((s[0] >> 4) == 14) return (uint32_t)(s[0] & 0x0F) << 12 | (uint32_t)(s[1] & 0x3F) << 6 | (s[2] & 0x3F);
    return (uint32_t)(s[0] & 0x07) << 18 | (uint32_t)(s[1] & 0x3F) << 12 | (uint32_t)(s[2] & 0x3F) << 6 | (s[3] & 0x3F);
}

/* Separates words (words()): Unicode White_Space, less the no-break spaces,
 * which join the words around them. */
static int yv_is_space(uint32_t c) {
    return (c >= 0x09 && c <= 0x0D) || c == 0x20 || c == 0x85 || c == 0x1680 || (c >= 0x2000 && c <= 0x2006) ||
           (c >= 0x2008 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x205F || c == 0x3000;
}

static int yv_text_set_string(yv_text* t, const char* s, size_t n, char* err, size_t cap) {
    if (n > YV_TEXT_MAX) return yv_fail(err, cap, YGFX_ERR_ARG, "text: at most %d bytes", YV_TEXT_MAX);
    if (n + 1 > t->cap_bytes) {
        char* p = (char*)realloc(t->utf8, n + 1);
        if (!p) return yv_fail(err, cap, YGFX_ERR_FULL, "out of memory");
        t->utf8 = p;
        t->cap_bytes = (uint32_t)n + 1;
    }
    memcpy(t->utf8, s, n);
    t->utf8[n] = 0;
    t->n_bytes = (uint32_t)n;
    return yv_text_index(t, err, cap);
}

/* Spans as the host gives them: k rows of start, end (codepoints, end
 * excluded), r, g, b (NaN: the text's color) and size (0: the text's).
 * Colors go to the palette from entry 1 (entry 0 is the text's color). */
static int yv_text_set_spans(yv_stim* st, const double* v, int count, char* err, size_t cap) {
    yv_text* t = (yv_text*)st->text;
    int k, i, n_col = 0;
    if (count % 6) return yv_fail(err, cap, YGFX_ERR_ARG, "spans: rows of start, end, r, g, b, size");
    k = count / 6;
    if (k > t->cap_spans) {
        ylay_span* p = (ylay_span*)realloc(t->spans, sizeof(ylay_span) * (size_t)k);
        double* q;
        if (!p) return yv_fail(err, cap, YGFX_ERR_FULL, "out of memory");
        t->spans = p;
        q = (double*)realloc(t->span_v, sizeof(double) * 6 * (size_t)k);
        if (!q) return yv_fail(err, cap, YGFX_ERR_FULL, "out of memory");
        t->span_v = q;
        t->cap_spans = k;
    }
    for (i = 0; i < k; i++) {
        const double* r = v + 6 * i;
        int has_col = r[2] == r[2] || r[3] == r[3] || r[4] == r[4];
        if (r[0] != floor(r[0]) || r[1] != floor(r[1]) || r[0] < 0 || r[1] < r[0])
            return yv_fail(err, cap, YGFX_ERR_ARG, "spans: span %d: start and end are character indices, start <= end", i);
        if (i && r[0] < v[6 * (i - 1) + 1]) return yv_fail(err, cap, YGFX_ERR_ARG, "spans: sorted, not overlapping (span %d)", i);
        if (has_col && !(r[2] == r[2] && r[3] == r[3] && r[4] == r[4]))
            return yv_fail(err, cap, YGFX_ERR_ARG, "spans: span %d: a color is r, g, b (NaN for the text's)", i);
        if (!(r[5] == r[5]) || r[5] < 0) return yv_fail(err, cap, YGFX_ERR_ARG, "spans: span %d: size, 0 (the text's) or more", i);
        if (has_col) n_col++;
    }
    if (n_col > YGFX_MAX_PALETTE - 1) return yv_fail(err, cap, YGFX_ERR_ARG, "spans: at most %d colors", YGFX_MAX_PALETTE - 1);
    memcpy(t->span_v, v, sizeof(double) * (size_t)count);
    t->n_spans = k;
    return 0;
}

/* The spans for ylay: byte offsets (clamped to the text), palette positions. */
static int yv_text_spans_resolve(yv_stim* st, char* err, size_t cap) {
    yv_text* t = (yv_text*)st->text;
    int i, n_col = 0;
    for (i = 0; i < t->n_spans; i++) {
        const double* r = t->span_v + 6 * i;
        uint32_t a = (uint32_t)r[0], b = (uint32_t)r[1];
        if (a > t->n_cp || b > t->n_cp)
            return yv_fail(err, cap, YGFX_ERR_ARG, "spans: span %d (%u to %u) passes the text's %u characters", i, a, b, t->n_cp);
        t->spans[i].start = t->cp_off[a];
        t->spans[i].end = t->cp_off[b];
        t->spans[i].size = (float)r[5];
        t->spans[i].lang = NULL;
        if (r[2] == r[2]) {
            n_col++;
            t->spans[i].color = (float)n_col;
            st->palette[3 * n_col] = (float)r[2];
            st->palette[3 * n_col + 1] = (float)r[3];
            st->palette[3 * n_col + 2] = (float)r[4];
        } else {
            t->spans[i].color = -1.0f;
        }
    }
    if (n_col) {
        st->s.inst_fields |= YGFX_I_COLOR | YGFX_I_XY;
        st->s.palette = st->palette;
        st->s.n_palette = (uint32_t)n_col + 1;
    } else {
        st->s.inst_fields &= ~YGFX_I_COLOR;
        st->s.palette = NULL;
        st->s.n_palette = 0;
    }
    return 0;
}

/* Lays the text out again and makes its runs. Between frames. */
static int yv_text_layout(yv_stim* st, char* err, size_t cap) {
    yv_text* t = (yv_text*)st->text;
    yv_gfx* g = st->o.g;
    yv_font* f = t->font;
    double t0, t1, t2;
    float w, h;
    int rc;
    if (g->in_frame) return yv_fail(err, cap, YGFX_ERR_ORDER, "text: change it between frames (a layout uploads new glyphs)");
    if (!(t->style.size > 0)) return yv_fail(err, cap, YGFX_ERR_ARG, "text: size > 0 (units per em)");
    rc = yv_text_spans_resolve(st, err, cap);
    if (rc < 0) return rc;
    t->style.lang = t->lang[0] ? t->lang : NULL;
    t0 = yrt_real_ns() / 1e9;
    rc = ylay_layout(f->L, t->utf8 ? t->utf8 : "", (int)t->n_bytes, &t->style, t->spans, t->n_spans, &t->b);
    t1 = yrt_real_ns() / 1e9;
    if (rc < 0) return yv_fail(err, cap, rc == YLAY_ERR_NOMEM ? YGFX_ERR_FULL : YGFX_ERR_ARG, "text: %s", ylay_error(f->L));
    rc = yv_font_sync(f, err, cap);
    t2 = yrt_real_ns() / 1e9;
    if (rc < 0) return rc;
    t->layout_s = t->b.layout_us / 1e6;
    t->build_s = t->b.build_us / 1e6;
    t->upload_s = t2 - t1;
    t->n_new = t->b.n_new_glyphs;
    t->layouts++;
    (void)t0;
    /* The box: the laid-out block, or the wrap box when there is one, so
     * an anchor places the box the text was wrapped in. */
    w = t->b.w;
    h = t->b.h;
    if (t->style.width > 0 && t->style.width > w) w = t->style.width;
    st->s.w = w;
    st->s.h = h;
    if (t->b.n_runs > 0) {
        const ylay_run* r0 = &t->b.runs[0];
        st->s.set = f->set[r0->font];
        st->s.items = t->b.items + r0->first;
        st->s.count = r0->n;
        st->s.size = r0->size;
        /* ygfx_crun() makes the frame's item buffers the first time, and the
         * rays program for a turned or grouped run: a setup call that
         * returns a stimulus this one does not need. */
        if (t->b.n_items) {
            ygfx_crun_desc d;
            memset(&d, 0, sizeof d);
            d.set = st->s.set;
            d.items = st->s.items;
            d.n = (int)st->s.count;
            d.size = st->s.size;
            d.w = w > 0 ? w : 1;
            d.h = h > 0 ? h : 1;
            d.ori = st->s.ori;
            d.group = st->s.group;
            d.rays = (st->s.flags & YGFX_STIM_RAYS) != 0;
            d.aliased = (st->s.flags & YGFX_STIM_ALIASED) != 0;
            (void)ygfx_crun(&g->g, &d);
        }
    } else {
        st->s.items = NULL;
        st->s.count = 0;
    }
    return 0;
}

/* Draws every run of a text: run 0 is st->s itself, the rest copies of it
 * with their run's set, items and size. Allocates nothing. */
static int yv_text_draw(yv_gfx* g, yv_stim* st) {
    yv_text* t = (yv_text*)st->text;
    yv_font* f = t->font;
    uint32_t r;
    int rc;
    if (t->b.n_runs == 0) return 0;
    st->s.set = f->set[t->b.runs[0].font];   /* a set made again has a new id */
    if (st->s.palette) { st->palette[0] = st->s.color[0]; st->palette[1] = st->s.color[1]; st->palette[2] = st->s.color[2]; }
    rc = ygfx_draw(&g->g, &st->s);
    for (r = 1; rc >= 0 && r < t->b.n_runs; r++) {
        const ylay_run* run = &t->b.runs[r];
        ygfx_stim x;
        if (!run->n) continue;
        x = st->s;
        x.set = f->set[run->font];
        x.items = t->b.items + run->first;
        x.count = run->n;
        x.size = run->size;
        rc = ygfx_draw(&g->g, &x);
    }
    return rc;
}

/* The topmost glyph of a text under a screen pixel: its character index
 * (codepoint), or -1. By the glyph boxes (CURVE RUNS, ygfx_hit_index). */
static int yv_text_hit(const yv_stim* st, float px, float py) {
    const yv_text* t = (const yv_text*)st->text;
    const yv_font* f = t->font;
    int r;
    for (r = (int)t->b.n_runs - 1; r >= 0; r--) {
        const ylay_run* run = &t->b.runs[r];
        ygfx_stim x;
        int k;
        if (!run->n) continue;
        x = st->s;
        x.set = f->set[run->font];
        x.items = t->b.items + run->first;
        x.count = run->n;
        x.size = run->size;
        k = ygfx_hit_index(&st->o.g->g, &x, px, py);
        if (k >= 0) return (int)t->b.cluster[run->first + (uint32_t)k];
    }
    return -1;
}

/* The screen frame of a text's box: a point (units from the box's
 * top-left) to screen px. From ygfx_local(), which is affine: its inverse
 * from three points. */
typedef struct yv_frame { double mx, my, c, s, k, hw, hh; } yv_frame;

static void yv_text_frame(const yv_stim* st, yv_frame* fr) {
    const ygfx_gfx* g = &st->o.g->g;
    float a[2], b[2];
    double L = 1024.0, ax, ay;
    ygfx_local(g, &st->s, 0.0f, 0.0f, &a[0], &a[1]);
    ygfx_local(g, &st->s, (float)L, 0.0f, &b[0], &b[1]);
    /* local = R^T (p - m), R^T (1, 0) = (c, -s) */
    fr->c = ((double)b[0] - a[0]) / L;
    fr->s = -((double)b[1] - a[1]) / L;
    ax = a[0];
    ay = a[1];
    fr->mx = -(fr->c * ax - fr->s * ay);
    fr->my = -(fr->s * ax + fr->c * ay);
    fr->k = (double)g->ppu * (st->s.group && st->s.group->scale > 0.0f ? st->s.group->scale : 1.0f);
    fr->hw = 0.5 * st->s.w;
    fr->hh = 0.5 * st->s.h;
}

/* The axis-aligned screen bounds of the box (x0, y0)-(x1, y1) in units from
 * the text's top-left. */
static void yv_text_rect(const yv_frame* fr, double x0, double y0, double x1, double y1, double out[4]) {
    double xs[2] = { x0, x1 }, ys[2] = { y0, y1 };
    int i, j;
    out[0] = out[1] = 1e300;
    out[2] = out[3] = -1e300;
    for (i = 0; i < 2; i++)
        for (j = 0; j < 2; j++) {
            double lx = (xs[i] - fr->hw) * fr->k, ly = (ys[j] - fr->hh) * fr->k;
            double px = fr->mx + fr->c * lx - fr->s * ly, py = fr->my + fr->s * lx + fr->c * ly;
            if (px < out[0]) out[0] = px;
            if (py < out[1]) out[1] = py;
            if (px > out[2]) out[2] = px;
            if (py > out[3]) out[3] = py;
        }
}

/* The ink box of a glyph in em (the curve set's record), or 0 for none. */
static int yv_glyph_box(const yol_cset* s, uint32_t gid, float b[4]) {
    uint32_t o;
    if (!s || !s->words || 8 + gid >= s->n_words) return 0;
    o = s->words[8 + gid];
    if (!o || o + 4 > s->n_words) return 0;
    memcpy(b, &s->words[o], 4 * sizeof(float));
    return b[2] > b[0] && b[3] > b[1];
}

/* The line an item's cluster is in. */
static int yv_text_line_of(const yv_text* t, uint32_t cl) {
    uint32_t i;
    for (i = 0; i < t->b.n_lines; i++)
        if (cl >= t->b.lines[i].text_start && cl < t->b.lines[i].text_end) return (int)i;
    return t->b.n_lines ? (int)t->b.n_lines - 1 : -1;
}

/* Per glyph, in item order: its character index, its line, and its ink box
 * as axis-aligned screen px (x0, y0, x1, y1); a glyph without ink (a space)
 * gives its pen point twice. out: 6 doubles per item. */
static int yv_text_glyphs(const yv_stim* st, double* out) {
    const yv_text* t = (const yv_text*)st->text;
    yv_frame fr;
    uint32_t r, k;
    int n = 0;
    yv_text_frame(st, &fr);
    for (r = 0; r < t->b.n_runs; r++) {
        const ylay_run* run = &t->b.runs[r];
        ylay_font_info fi;
        ylay_font(t->font->L, run->font, &fi);
        for (k = run->first; k < run->first + run->n; k++) {
            const ygfx_citem* it = &t->b.items[k];
            float bx[4];
            double* o = out + 6 * n++;
            o[0] = t->b.cluster[k];
            o[1] = yv_text_line_of(t, t->b.cluster[k]);
            if (yv_glyph_box(fi.set, (uint32_t)it->glyph, bx))
                yv_text_rect(&fr, it->x + run->size * bx[0], it->y + run->size * bx[1], it->x + run->size * bx[2],
                             it->y + run->size * bx[3], o + 2);
            else
                yv_text_rect(&fr, it->x, it->y, it->x, it->y, o + 2);
        }
    }
    return n;
}

/* Lines: first and last + 1 character, and the line box (from its top,
 * baseline - ascent, to its bottom, baseline + descent, over its width) as
 * axis-aligned screen px. out: 6 doubles per line. */
static int yv_text_lines(const yv_stim* st, double* out) {
    const yv_text* t = (const yv_text*)st->text;
    yv_frame fr;
    uint32_t i;
    yv_text_frame(st, &fr);
    for (i = 0; i < t->b.n_lines; i++) {
        const ylay_line* l = &t->b.lines[i];
        double* o = out + 6 * i;
        o[0] = l->text_start;
        o[1] = l->text_end;
        yv_text_rect(&fr, l->x, l->baseline - l->ascent, l->x + l->width, l->baseline + l->descent, o + 2);
    }
    return (int)t->b.n_lines;
}

/* Words: runs of characters between white space (yv_is_space), each with
 * its first and last + 1 character, its line, and its box as axis-aligned
 * screen px: across, from its leftmost glyph's pen to the right edge of its
 * rightmost glyph's advance (the next glyph's pen on the line, or the
 * line's end), so kerning and marks are the layout's; down, its line's box.
 * A word broken across lines (it cannot be: lines break at spaces or, with
 * no space, inside a word too wide for the box) takes its first line.
 * out: 7 doubles per word; returns the count, or -1 for no memory. */
static int yv_text_words(const yv_stim* st, double* out, int cap_words) {
    const yv_text* t = (const yv_text*)st->text;
    yv_frame fr;
    uint32_t i, n = t->b.n_items;
    float* right;
    int nw = 0;
    if (n == 0) return 0;
    right = (float*)malloc(sizeof(float) * n);
    if (!right) return -1;
    /* each item's right edge: the next pen to its right on its line */
    for (i = 0; i < n; i++) {
        int li = yv_text_line_of(t, t->b.cluster[i]);
        const ylay_line* l = &t->b.lines[li];
        float x = t->b.items[i].x, best = l->x + l->width;
        uint32_t j;
        for (j = 0; j < n; j++) {
            float xj = t->b.items[j].x;
            if (xj > x + 1e-6f && xj < best && yv_text_line_of(t, t->b.cluster[j]) == li) best = xj;
        }
        right[i] = best;
    }
    yv_text_frame(st, &fr);
    i = 0;
    while (i < t->n_cp && nw < cap_words) {
        uint32_t a, b, k;
        double x0 = 1e300, x1 = -1e300;
        int li = -1;
        while (i < t->n_cp && yv_is_space(yv_text_cp_at(t, i))) i++;
        if (i >= t->n_cp) break;
        a = i;
        while (i < t->n_cp && !yv_is_space(yv_text_cp_at(t, i))) i++;
        b = i;
        for (k = 0; k < n; k++) {
            uint32_t cl = t->b.cluster[k];
            if (cl < a || cl >= b) continue;
            if (li < 0) li = yv_text_line_of(t, cl);
            if (t->b.items[k].x < x0) x0 = t->b.items[k].x;
            if (right[k] > x1) x1 = right[k];
        }
        if (li < 0) continue;   /* no glyph: nothing drawn */
        {
            const ylay_line* l = &t->b.lines[li];
            double* o = out + 7 * nw++;
            o[0] = a;
            o[1] = b;
            o[2] = li;
            yv_text_rect(&fr, x0, l->baseline - l->ascent, x1, l->baseline + l->descent, o + 3);
        }
    }
    free(right);
    return nw;
}

/* The number of words words() gives at most: the runs of non-space. */
static int yv_text_word_count(const yv_text* t) {
    uint32_t i;
    int n = 0, in = 0;
    for (i = 0; i < t->n_cp; i++) {
        int sp = yv_is_space(yv_text_cp_at(t, i));
        if (!sp && !in) n++;
        in = !sp;
    }
    return n;
}

/* ---- the text's own parameters by name --------------------------------------- */

static const char* const yv_text_keys[] = { "text", "font", "size", "width", "align", "line_height", "dir", "lang",
                                            "spans", NULL };

static int yv_text_key(const char* name) {
    int i;
    for (i = 0; yv_text_keys[i]; i++) if (!strcmp(name, yv_text_keys[i])) return 1;
    return 0;
}

/* A font from a host argument: an object (a Font of this gfx), or a path
 * (or "a|b|c" paths, a fallback list) made once and kept. */
static yv_font* yv_text_font_arg(yv_gfx* g, yv_kv* k, int* code, char* err, size_t cap) {
    *code = YGFX_ERR_ARG;
    if (k->obj) {
        if (k->obj->type != YV_O_FONT || k->obj->g != g) { yv_fail(err, cap, YGFX_ERR_ARG, "font: a Font of this gfx"); return NULL; }
        return (yv_font*)k->obj;
    }
    if (k->s) {
        yv_font_src src[YV_FONT_MAX];
        char key[4096], paths[4096];
        int n = 0;
        char* p;
        yv_font* f;
        if (!k->s[0] || strlen(k->s) >= sizeof paths) { yv_fail(err, cap, YGFX_ERR_ARG, "font: a path"); return NULL; }
        snprintf(paths, sizeof paths, "%s", k->s);
        memset(src, 0, sizeof src);
        for (p = paths; p && n < YV_FONT_MAX;) {
            char* bar = strchr(p, '|');
            if (bar) *bar = 0;
            src[n++].path = p;
            p = bar ? bar + 1 : NULL;
        }
        if (yv_font_key(src, n, key, sizeof key) < 0) { yv_fail(err, cap, YGFX_ERR_ARG, "font: the paths are too long"); return NULL; }
        f = yv_font_find(g, key);
        return f ? f : yv_font_make(g, src, n, key, code, err, cap);
    }
    yv_fail(err, cap, YGFX_ERR_ARG, "font: a Font, or a font file's path");
    return NULL;
}

static const char* const yv_text_align_values = "start=0,end=1,center=2";
static const char* const yv_text_dir_values = "auto=0,ltr=1,rtl=2";

/* Takes the text's own keys from kv into st (none applied to the layout
 * yet); *changed: whether a layout is due. */
static int yv_text_take(yv_stim* st, yv_kv* kv, int n, int* changed, char* err, size_t cap) {
    yv_text* t = (yv_text*)st->text;
    yv_kv* k;
    int e, code;
    double d;
    if ((k = yv_take(kv, n, "font"))) {
        yv_font* f = yv_text_font_arg(st->o.g, k, &code, err, cap);
        if (!f) return code;
        if (f != t->font) {
            if (t->font) t->font->o.users--;
            t->font = f;
            f->o.users++;
            *changed = 1;
        }
    }
    if ((k = yv_take(kv, n, "text"))) {
        if (!k->s) return yv_fail(err, cap, YGFX_ERR_ARG, "text: a string");
        if (yv_text_set_string(t, k->s, strlen(k->s), err, cap) < 0) return YGFX_ERR_ARG;
        *changed = 1;
    }
    if ((k = yv_take(kv, n, "size"))) {
        if (yv_num(k, &d, err, cap) < 0 || !(d > 0)) return yv_fail(err, cap, YGFX_ERR_ARG, "size: units per em, > 0");
        t->style.size = (float)d;
        *changed = 1;
    }
    if ((k = yv_take(kv, n, "width"))) {
        if (yv_num(k, &d, err, cap) < 0 || d < 0) return yv_fail(err, cap, YGFX_ERR_ARG, "width: the wrap width in units, 0 = no wrapping");
        t->style.width = (float)d;
        *changed = 1;
    }
    if ((k = yv_take(kv, n, "line_height"))) {
        if (yv_num(k, &d, err, cap) < 0 || d < 0) return yv_fail(err, cap, YGFX_ERR_ARG, "line_height: times the font's line, 0 = 1");
        t->style.line_height = (float)d;
        *changed = 1;
    }
    if ((k = yv_take(kv, n, "align"))) {
        if (!k->s || yv_enum_value(yv_text_align_values, k->s, &e) < 0) return yv_fail(err, cap, YGFX_ERR_ARG, "align: start, center or end");
        t->style.align = e == 0 ? YLAY_ALIGN_START : e == 1 ? YLAY_ALIGN_END : YLAY_ALIGN_CENTER;
        *changed = 1;
    }
    if ((k = yv_take(kv, n, "dir"))) {
        if (!k->s || yv_enum_value(yv_text_dir_values, k->s, &e) < 0) return yv_fail(err, cap, YGFX_ERR_ARG, "dir: auto, ltr or rtl");
        t->style.dir = e == 0 ? YLAY_DIR_AUTO : e == 1 ? YLAY_DIR_LTR : YLAY_DIR_RTL;
        *changed = 1;
    }
    if ((k = yv_take(kv, n, "lang"))) {
        if (!k->s || strlen(k->s) >= sizeof t->lang) return yv_fail(err, cap, YGFX_ERR_ARG, "lang: a BCP 47 tag (\"en\", \"ja\", \"ar\"), or \"\"");
        snprintf(t->lang, sizeof t->lang, "%s", k->s);
        *changed = 1;
    }
    if ((k = yv_take(kv, n, "spans"))) {
        if (k->s || k->obj) return yv_fail(err, cap, YGFX_ERR_ARG, "spans: rows of start, end, r, g, b, size");
        if (yv_text_set_spans(st, k->v, k->n, err, cap) < 0) return YGFX_ERR_ARG;
        *changed = 1;
    }
    return 0;
}

/* Sets a text's parameters: its own (a layout when any changed), then the
 * stimulus's (place, color, ...). */
static int yv_text_set(yv_stim* st, yv_kv* kv, int n, char* err, size_t cap) {
    yv_text* t = (yv_text*)st->text;
    int changed = 0, rc, i, own = 0;
    if (yv_live(&st->o, err, cap) < 0) return YGFX_ERR_CLOSED;
    for (i = 0; i < n; i++) if (!kv[i].used && yv_text_key(kv[i].name)) own = 1;
    if (own && st->o.g->in_frame) return yv_fail(err, cap, YGFX_ERR_ORDER, "text: change it between frames (a layout uploads new glyphs)");
    if (own) {
        /* A refused change leaves the text as it was: the parameters back,
         * and the layout of them again (a failed layout may have emptied
         * the block). */
        char* old = (char*)malloc(t->n_bytes + 1);
        double* ospans = t->n_spans ? (double*)malloc(sizeof(double) * 6 * (size_t)t->n_spans) : NULL;
        ylay_style ostyle = t->style;
        char olang[sizeof t->lang];
        yv_font* ofont = t->font;
        int on = t->n_spans;
        uint32_t onb = t->n_bytes;
        if (!old || (on && !ospans)) { free(old); free(ospans); return yv_fail(err, cap, YGFX_ERR_FULL, "out of memory"); }
        memcpy(old, t->utf8, onb + 1);
        if (on) memcpy(ospans, t->span_v, sizeof(double) * 6 * (size_t)on);
        memcpy(olang, t->lang, sizeof olang);
        rc = yv_text_take(st, kv, n, &changed, err, cap);
        if (rc >= 0 && changed) rc = yv_text_layout(st, err, cap);
        if (rc < 0) {
            char e2[8];
            if (t->font != ofont) { t->font->o.users--; ofont->o.users++; t->font = ofont; }
            t->style = ostyle;
            memcpy(t->lang, olang, sizeof olang);
            yv_text_set_string(t, old, onb, e2, sizeof e2);
            if (on) memcpy(t->span_v, ospans, sizeof(double) * 6 * (size_t)on);
            t->n_spans = on;
            yv_text_layout(st, e2, sizeof e2);
        }
        free(old);
        free(ospans);
        if (rc < 0) return rc;
    }
    return yv_set_rest(st, kv, n, err, cap);
}

/* A text: font (a Font, a path, or none for the system's sans font), text,
 * size (units per em; 0 = 32 px, or 1 deg in deg units), width, align
 * (center by default), line_height, dir, lang, spans, then any stimulus
 * parameter (place, anchor, x, y, ori, color, space, opacity, blend, rays,
 * aliased, group, visible, gate, ...). NULL with err. */
static yv_stim* yv_text_make(yv_gfx* g, yv_kv* kv, int n, int* code, char* err, size_t cap) {
    yv_stim* st;
    yv_text* t;
    yv_box b;
    yv_kv* k;
    ygfx_crun_desc d;
    int changed = 0, rc;
    *code = YGFX_ERR_ARG;
    if (!g->open) { *code = yv_fail(err, cap, YGFX_ERR_CLOSED, "ysp_gfx: the gfx (or its screen) is closed"); return NULL; }
    if (g->in_frame) { *code = yv_fail(err, cap, YGFX_ERR_ORDER, "text: make it between frames (a layout uploads glyphs)"); return NULL; }
    if (yv_box_take(kv, n, &b, err, cap) < 0) return NULL;
    if (b.w != 0 || b.h != 0) { yv_fail(err, cap, YGFX_ERR_ARG, "text: width (the wrap width), not w and h: the box is the layout's"); return NULL; }
    st = yv_stim_alloc(g, YV_K_TEXT);
    t = st ? (yv_text*)calloc(1, sizeof *t) : NULL;
    if (!t) { yv_stim_free(st); *code = yv_fail(err, cap, YGFX_ERR_FULL, "out of memory"); return NULL; }
    st->text = t;
    t->style.size = g->desc.units == YGFX_DEG ? 1.0f : 32.0f;
    t->style.align = YLAY_ALIGN_CENTER;
    if (yv_text_set_string(t, "", 0, err, cap) < 0) goto fail;
    /* the system's sans font when none is given */
    if (!(k = yv_take(kv, n, "font"))) {
        yv_font* f = yv_font_default(g, code, err, cap);
        if (!f) goto fail_code;
        t->font = f;
        f->o.users++;
    } else {
        k->used = 0;   /* yv_text_take() reads it */
    }
    rc = yv_text_take(st, kv, n, &changed, err, cap);
    if (rc < 0) { *code = rc; goto fail_code; }
    memset(&d, 0, sizeof d);
    d.place = (ygfx_align)b.place; d.anchor = (ygfx_align)b.anchor;
    d.x = b.x; d.y = b.y; d.ori = b.ori;
    d.w = 1; d.h = 1;   /* the layout's box replaces them */
    d.color[0] = d.color[1] = d.color[2] = 1.0f;   /* white until color= says */
    st->s = ygfx_crun(&g->g, &d);
    /* the stimulus's parameters before the layout: rays, aliased, ori and
     * group decide which programs ygfx_crun() makes for it */
    if (yv_set_rest(st, kv, n, err, cap) < 0) goto fail;
    rc = yv_text_layout(st, err, cap);
    if (rc < 0) { *code = rc; goto fail_code; }
    return st;
fail:
    *code = YGFX_ERR_ARG;
fail_code:
    yv_stim_free(st);
    return NULL;
}

/* The text's own parameter as numbers or a string (*s), for a host's get. */
static int yv_text_get(const yv_stim* st, const char* name, double* v, int* nv, const char** s, char* buf, size_t bcap) {
    const yv_text* t = (const yv_text*)st->text;
    *nv = 0;
    *s = NULL;
    if (!strcmp(name, "text")) { *s = t->utf8 ? t->utf8 : ""; return 0; }
    if (!strcmp(name, "size")) { v[0] = t->style.size; *nv = 1; return 0; }
    if (!strcmp(name, "width")) { v[0] = t->style.width; *nv = 1; return 0; }
    if (!strcmp(name, "line_height")) { v[0] = t->style.line_height; *nv = 1; return 0; }
    if (!strcmp(name, "align")) { *s = t->style.align == YLAY_ALIGN_END ? "end" : t->style.align == YLAY_ALIGN_CENTER ? "center" : "start"; return 0; }
    if (!strcmp(name, "dir")) { *s = t->style.dir == YLAY_DIR_LTR ? "ltr" : t->style.dir == YLAY_DIR_RTL ? "rtl" : "auto"; return 0; }
    if (!strcmp(name, "lang")) { *s = t->lang; return 0; }
    if (!strcmp(name, "box")) { v[0] = st->s.w; v[1] = st->s.h; *nv = 2; return 0; }
    if (!strcmp(name, "resolved_dir")) { *s = t->b.dir == YLAY_DIR_RTL ? "rtl" : "ltr"; return 0; }
    if (!strcmp(name, "n_lines")) { v[0] = t->b.n_lines; *nv = 1; return 0; }
    if (!strcmp(name, "n_glyphs")) { v[0] = t->b.n_items; *nv = 1; return 0; }
    if (!strcmp(name, "n_runs")) { v[0] = t->b.n_runs; *nv = 1; return 0; }
    if (!strcmp(name, "n_chars")) { v[0] = t->n_cp; *nv = 1; return 0; }
    if (!strcmp(name, "missing")) { v[0] = t->b.n_missing; *nv = 1; return 0; }
    if (!strcmp(name, "layout_time")) { v[0] = t->layout_s; *nv = 1; return 0; }
    if (!strcmp(name, "build_time")) { v[0] = t->build_s; *nv = 1; return 0; }
    if (!strcmp(name, "upload_time")) { v[0] = t->upload_s; *nv = 1; return 0; }
    if (!strcmp(name, "new_glyphs")) { v[0] = t->n_new; *nv = 1; return 0; }
    if (!strcmp(name, "stamp")) { ylay_stamp(t->font->L, &t->b, buf, bcap); *s = buf; return 0; }
    if (!strcmp(name, "spans")) return 1;   /* the host reads span_v */
    return -1;
}

/* A font's files, for a host's info: name, face, sha256 and metrics. */
static int yv_font_info(const yv_font* f, int i, ylay_font_info* out) {
    return ylay_font(f->L, i, out);
}

/* Frees the fonts the gfx still holds (the implicit ones, and any a host
 * left): the last step before the yv_gfx itself goes. */
static void yv_gfx_free_fonts(yv_gfx* g) {
    yv_obj* o = g->objs;
    while (o) {
        yv_obj* next = o->next;
        if (o->type == YV_O_FONT) yv_font_free((yv_font*)o);
        o = next;
    }
}

/* ---- the C path, for the pixel tests ------------------------------------------------ */

/* Two texts made with pack/layout and ysp/gfx.h directly, as a C program
 * makes them (examples/layout/gfx_layout.c, examples/response/trial_stroop.c):
 * a layout library per fallback list, a curve set per font from the
 * glyphs the layout built, a ygfx_crun() per run over the block's box. The
 * tests make the same texts through the binding and compare the scenes
 * read back (tests/test_window.py, test_mex_vis_window.m).
 *   0: "Hello, world" in Segoe UI at 40 px, centered, at (-120, -80) from
 *      the screen's center, color (0.9, 0.6, 0.2);
 *   1: "Ab <CJK> <Arabic> 12" with Segoe UI then Microsoft YaHei, 36 px,
 *      wrapped at 220 px, start-aligned, line height 1.2, the first two
 *      characters green by a span, the rest (0.8, 0.8, 0.8), turned 20
 *      degrees about its top-left at (60, 300) from the screen's top-left. */
#define YV_CTEXT_TEXT1 "Ab \xE4\xB8\xAD\xE6\x96\x87 \xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7 12"

typedef struct yv_ctext {
    yol_ctx    ol;
    ylay_lib*  L[2];
    uint8_t*   bytes[2];
    size_t     n[2];
    ygfx_cset  set[2][2];      /* per scene, per font of its list         */
    ylay_block b[2];
    ygfx_stim  st[2][8];
    int        ns[2];
    float      pal[6];
} yv_ctext;

static void yv_ctext_free(yv_gfx* g) {
    yv_ctext* c = (yv_ctext*)g->ctext;
    int i;
    if (!c) return;
    for (i = 0; i < 2; i++) {
        if (c->L[i]) ylay_destroy(c->L[i]);
        ylay_block_free(&c->b[i]);
        free(c->bytes[i]);
    }
    yol_close(&c->ol);
    free(c);
    g->ctext = NULL;   /* its curve sets went with the gfx (ygfx_close) */
}

static int yv_ctext_upload(yv_gfx* g, ylay_lib* L, ygfx_cset* sets, int nf) {
    int i;
    for (i = 0; i < nf; i++) {
        const uint32_t* gl;
        ylay_font_info fi;
        ygfx_cset_desc d;
        if (ylay_take_new_glyphs(L, i, &gl) <= 0) continue;
        ylay_font(L, i, &fi);
        memset(&d, 0, sizeof d);
        d.texels = fi.set->texels; d.n_texels = fi.set->n_texels;
        d.words = fi.set->words; d.n_words = fi.set->n_words;
        sets[i] = ygfx_cset_make(&g->g, &d);
        if (!sets[i].id) return -1;
    }
    return 0;
}

/* Lays out and uploads both texts; outside a frame. */
static int yv_ctext_setup(yv_gfx* g, char* err, size_t cap) {
    static const char* files[2] = { "segoeui.ttf", "msyh.ttc" };
    yv_ctext* c;
    char win[400], path[600];
    ylay_desc ld;
    ylay_style st;
    ylay_span sp;
    int i, r;
    if (!g->open) return yv_fail(err, cap, YGFX_ERR_CLOSED, "the gfx is closed");
    if (g->in_frame) return yv_fail(err, cap, YGFX_ERR_ORDER, "_c_text_setup: outside a frame");
    if (g->ctext) return 0;
    c = (yv_ctext*)calloc(1, sizeof *c);
    if (!c) return yv_fail(err, cap, YGFX_ERR_FULL, "out of memory");
    g->ctext = c;
    if (!yol_open(&c->ol, NULL)) goto fail;
#if defined(_WIN32)
    if (!GetEnvironmentVariableA("WINDIR", win, (DWORD)sizeof win)) snprintf(win, sizeof win, "C:\\Windows");
#else
    snprintf(win, sizeof win, "/usr/share");
#endif
    for (i = 0; i < 2; i++) {
        snprintf(path, sizeof path, "%s\\Fonts\\%s", win, files[i]);
        if (yv_read_file(path, &c->bytes[i], &c->n[i], err, cap) < 0) { yv_ctext_free(g); return YGFX_ERR_IO; }
    }
    memset(&ld, 0, sizeof ld);
    ld.ol = &c->ol;
    if (ylay_create(&c->L[0], &ld) != YLAY_OK || ylay_create(&c->L[1], &ld) != YLAY_OK) goto fail;
    if (ylay_add_font(c->L[0], c->bytes[0], c->n[0], 0, files[0]) < 0 || ylay_add_font(c->L[1], c->bytes[0], c->n[0], 0, files[0]) < 0 ||
        ylay_add_font(c->L[1], c->bytes[1], c->n[1], 0, files[1]) < 0)
        goto fail;
    /* scene 0 */
    memset(&st, 0, sizeof st);
    st.size = 40; st.align = YLAY_ALIGN_CENTER;
    if (ylay_layout(c->L[0], "Hello, world", -1, &st, NULL, 0, &c->b[0]) != YLAY_OK) goto fail;
    if (yv_ctext_upload(g, c->L[0], c->set[0], 1) < 0) goto fail;
    for (r = 0; r < (int)c->b[0].n_runs && r < 8; r++) {
        const ylay_run* run = &c->b[0].runs[r];
        ygfx_crun_desc d;
        memset(&d, 0, sizeof d);
        d.place = YGFX_CENTER; d.anchor = YGFX_CENTER; d.x = -120; d.y = -80;
        d.w = c->b[0].w; d.h = c->b[0].h;
        d.set = c->set[0][run->font]; d.items = c->b[0].items + run->first; d.n = (int)run->n; d.size = run->size;
        d.color[0] = 0.9f; d.color[1] = 0.6f; d.color[2] = 0.2f;
        c->st[0][c->ns[0]++] = ygfx_crun(&g->g, &d);
    }
    /* scene 1: the span's color is palette entry 1, the text's entry 0 */
    memset(&st, 0, sizeof st);
    st.size = 36; st.width = 220; st.align = YLAY_ALIGN_START; st.line_height = 1.2f; st.color = 0;
    memset(&sp, 0, sizeof sp);
    sp.start = 0; sp.end = 2; sp.color = 1;
    if (ylay_layout(c->L[1], YV_CTEXT_TEXT1, -1, &st, &sp, 1, &c->b[1]) != YLAY_OK) goto fail;
    if (yv_ctext_upload(g, c->L[1], c->set[1], 2) < 0) goto fail;
    c->pal[0] = c->pal[1] = c->pal[2] = 0.8f;
    c->pal[3] = 0.1f; c->pal[4] = 0.8f; c->pal[5] = 0.2f;
    for (r = 0; r < (int)c->b[1].n_runs && r < 8; r++) {
        const ylay_run* run = &c->b[1].runs[r];
        ygfx_crun_desc d;
        memset(&d, 0, sizeof d);
        d.place = YGFX_TOP_LEFT; d.anchor = YGFX_TOP_LEFT; d.x = 60; d.y = 300; d.ori = 20;
        d.w = c->b[1].w > 220 ? c->b[1].w : 220; d.h = c->b[1].h;   /* the wrap box */
        d.set = c->set[1][run->font]; d.items = c->b[1].items + run->first; d.n = (int)run->n; d.size = run->size;
        d.fields = YGFX_I_COLOR; d.palette = c->pal; d.n_palette = 2;
        d.color[0] = d.color[1] = d.color[2] = 0.8f;
        c->st[1][c->ns[1]++] = ygfx_crun(&g->g, &d);
    }
    return 0;
fail:
    yv_fail(err, cap, YGFX_ERR_ARG, "_c_text_setup: %s", c->L[1] ? ylay_error(c->L[1]) : c->L[0] ? ylay_error(c->L[0]) : ygfx_error(&g->g));
    yv_ctext_free(g);
    return YGFX_ERR_ARG;
}

/* Draws C scene k (0 or 1) in the current frame. */
static int yv_ctext_draw(yv_gfx* g, int k, char* err, size_t cap) {
    yv_ctext* c = (yv_ctext*)g->ctext;
    int i, rc;
    if (!c) return yv_fail(err, cap, YGFX_ERR_ORDER, "_c_text: _c_text_setup first");
    if (k < 0 || k > 1) return yv_fail(err, cap, YGFX_ERR_ARG, "_c_text: 0 or 1");
    for (i = 0; i < c->ns[k]; i++) {
        rc = ygfx_draw(&g->g, &c->st[k][i]);
        if (rc < 0) return yv_gfx_err(g, rc, err, cap);
    }
    return 0;
}

#endif /* YSP_VIS_TEXT_H */
