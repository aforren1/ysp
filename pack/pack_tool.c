/* pack_tool.c - the pack tool as a library (pack/ysp/pack_tool.h).
 * docs/pack.md is the format; this file builds, verifies and rewrites packs. */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
/* fseeko, ftello and mkdir under -std=c11 (as ysp/rt.h does). */
#if (defined(__linux__) || defined(__EMSCRIPTEN__)) && !defined(_DEFAULT_SOURCE) &&     !defined(_GNU_SOURCE) && !defined(_POSIX_C_SOURCE) && !defined(_XOPEN_SOURCE)
#define _DEFAULT_SOURCE 1
#endif
#ifndef YSCR_NO_SDL
#define YSCR_NO_SDL
#endif
#ifndef YAU_NO_MINIAUDIO
#define YAU_NO_MINIAUDIO
#endif
#include "ysp/pack_tool.h"
#include "ysp/pack.h"
#include "ysp/json.h"
#include "ysp/table.h"
#include "ysp/outline.h"
#include "ysp/gfx.h"
#include "ysp/color.h"
#include "ysp/audio.h"
#include "ysp/layout.h"
#include "lodepng.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <direct.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#ifndef YPT_LODEPNG
#define YPT_LODEPNG "unknown"
#endif

/* --- context: errors and every allocation, freed at the end ----------------- */

typedef struct C {
    char*  err;
    size_t cap;
    void** blk;
    size_t nb, cb;
    FILE*  log;
    const char* base;
    yjs_arena ja;      /* the JSON values (jar())                          */
    int    jfail;      /* a JSON builder failed: no manifest from this call */
} C;

static int cfail(C* c, int code, const char* fmt, ...) {
    va_list ap;
    int n;
    if (!c->err || !c->cap) return code;
    if (c->err[0]) return code;   /* the first failure is the cause */
    n = snprintf(c->err, c->cap, "ypak: ");
    va_start(ap, fmt);
    vsnprintf(c->err + n, c->cap - (size_t)n, fmt, ap);
    va_end(ap);
    return code;
}

static void* calloc_c(C* c, size_t n) {
    void* p;
    if (c->nb == c->cb) {
        size_t cb = c->cb ? 2 * c->cb : 256;
        void** b = (void**)realloc(c->blk, cb * sizeof(void*));
        if (!b) return NULL;
        c->blk = b;
        c->cb = cb;
    }
    p = calloc(1, n ? n : 1);
    if (p) c->blk[c->nb++] = p;
    return p;
}

static void cfree_all(C* c) {
    size_t i;
    for (i = 0; i < c->nb; i++) free(c->blk[i]);
    free(c->blk);
    c->blk = NULL;
    c->nb = c->cb = 0;
    yjs_arena_free(&c->ja);
}

static char* cdup(C* c, const char* s, size_t n) {
    char* d = (char*)calloc_c(c, n + 1);
    if (d && n) memcpy(d, s, n);
    return d;
}

/* --- a growable byte buffer (malloc, freed by its owner) --------------------- */

typedef struct sb { char* p; size_t n, cap; int bad; } sb;

static void sb_put(sb* b, const void* s, size_t n) {
    if (b->bad) return;
    if (b->n + n + 1 > b->cap) {
        size_t c = b->cap ? b->cap : 4096;
        char* q;
        while (c < b->n + n + 1) c *= 2;
        q = (char*)realloc(b->p, c);
        if (!q) { b->bad = 1; return; }
        b->p = q;
        b->cap = c;
    }
    if (n) memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}
static void sb_puts(sb* b, const char* s) { sb_put(b, s, strlen(s)); }

/* --- files --------------------------------------------------------------------- */

static FILE* ufopen(const char* path, const char* mode) {
#if defined(_WIN32)
    wchar_t wp[2048], wm[8];
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, wp, 2048)) return NULL;
    if (!MultiByteToWideChar(CP_UTF8, 0, mode, -1, wm, 8)) return NULL;
    return _wfopen(wp, wm);
#else
    return fopen(path, mode);
#endif
}

static int uremove(const char* path) {
#if defined(_WIN32)
    wchar_t wp[2048];
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, wp, 2048)) return -1;
    return _wremove(wp);
#else
    return remove(path);
#endif
}

static int urename(const char* from, const char* to) {
#if defined(_WIN32)
    wchar_t a[2048], b[2048];
    if (!MultiByteToWideChar(CP_UTF8, 0, from, -1, a, 2048) || !MultiByteToWideChar(CP_UTF8, 0, to, -1, b, 2048)) return -1;
    return MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING) ? 0 : -1;
#else
    return rename(from, to);
#endif
}

static int umkdir(const char* path) {
#if defined(_WIN32)
    wchar_t wp[2048];
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, wp, 2048)) return -1;
    return _wmkdir(wp) == 0 || errno == EEXIST ? 0 : -1;
#else
    return mkdir(path, 0777) == 0 || errno == EEXIST ? 0 : -1;
#endif
}

static int uexists(const char* path) {
    FILE* f = ufopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

/* A whole file into memory owned by c. */
static uint8_t* read_file(C* c, const char* path, int64_t* n) {
    FILE* f = ufopen(path, "rb");
    uint8_t* d;
    int64_t sz;
    if (!f) { cfail(c, YPAK_ERR_IO, "cannot open '%s'", path); return NULL; }
#if defined(_WIN32)
    _fseeki64(f, 0, SEEK_END);
    sz = _ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);
#else
    fseeko(f, 0, SEEK_END);
    sz = (int64_t)ftello(f);
    fseeko(f, 0, SEEK_SET);
#endif
    if (sz < 0 || (uint64_t)sz > (uint64_t)SIZE_MAX / 2) { fclose(f); cfail(c, YPAK_ERR_IO, "cannot size '%s'", path); return NULL; }
    d = (uint8_t*)calloc_c(c, (size_t)sz + 16);
    if (!d) { fclose(f); cfail(c, YPAK_ERR_NOMEM, "out of memory reading '%s'", path); return NULL; }
    {
        int64_t at = 0;   /* in pieces, as sk_put() writes */
        while (at < sz) {
            size_t k = sz - at > ((int64_t)16 << 20) ? ((size_t)16 << 20) : (size_t)(sz - at);
            if (fread(d + at, 1, k, f) != k) { fclose(f); cfail(c, YPAK_ERR_IO, "cannot read '%s'", path); return NULL; }
            at += (int64_t)k;
        }
    }
    fclose(f);
    *n = sz;
    return d;
}

static void hex(const uint8_t* b, int n, char* out) {
    static const char h[] = "0123456789abcdef";
    int i;
    for (i = 0; i < n; i++) { out[2 * i] = h[b[i] >> 4]; out[2 * i + 1] = h[b[i] & 15]; }
    out[2 * n] = 0;
}

/* --- names -------------------------------------------------------------------- */

int ypt_name_ok(const char* name, size_t n) {
    static const char* const dev[] = { "con", "prn", "aux", "nul", "com", "lpt" };
    size_t i = 0;
    if (!name || !ypak_name_ok(name, n)) return 0;
    while (i < n) {
        size_t j = i, k, base;
        while (j < n && name[j] != '/') j++;
        if (j - i > 255 || name[i] == '.' || name[j - 1] == '.') return 0;
        for (k = i; k < j; k++) {
            char ch = name[k];
            if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' || ch == '-'))
                return 0;
        }
        /* a Windows device name, with or without an extension */
        base = i;
        while (base < j && name[base] != '.') base++;
        for (k = 0; k < 6; k++) {
            size_t len = base - i, m;
            int same = 1;
            if (len != 3 && !(len == 4 && k >= 4 && name[i + 3] >= '1' && name[i + 3] <= '9')) continue;
            for (m = 0; m < 3; m++) {
                char a = name[i + m];
                if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
                if (a != dev[k][m]) same = 0;
            }
            if (same && (len == 3 ? k < 4 : 1)) return 0;
        }
        i = j + 1;
    }
    return 1;
}

static int name_icmp(const char* a, size_t na, const char* b, size_t nb) {
    size_t i;
    if (na != nb) return 1;
    for (i = 0; i < na; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y) return 1;
    }
    return 0;
}

/* --- JSON: ysp/json.h, with the pack's rule on numbers ----------------------------- */

/* ysp/json.h is the one strict reader and canonical writer of the
 * repository (the rig profile reads it too). The pack adds one rule
 * (docs/pack.md 3): numbers are integers, so a real value stays a string
 * with the decimal text the author wrote. */
typedef yjs_value jv;
enum { J_NULL = YJS_NULL, J_FALSE = YJS_FALSE, J_TRUE = YJS_TRUE, J_INT = YJS_NUMBER, J_STR = YJS_STRING, J_ARR = YJS_ARRAY, J_OBJ = YJS_OBJECT };

/* Every value of a call lives in one heap arena, freed by cfree_all(). */
static yjs_arena* jar(C* c) {
    if (!c->ja.heap_limit) yjs_arena_init_heap(&c->ja, ~(size_t)0 / 2);
    return &c->ja;
}
/* A builder that fails marks the call, so a manifest is never written
 * without a value it should have had. */
static jv* jok(C* c, jv* v) {
    if (!v) { c->jfail = 1; cfail(c, YPAK_ERR_NOMEM, "out of memory in JSON"); }
    return v;
}
static jv* jnew(C* c, int t) { return jok(c, yjs_new(jar(c), t)); }
static jv* jstr(C* c, const char* s, size_t n) {
    jv* v = yjs_new_string(jar(c), s, n);
    if (!v) { c->jfail = 1; cfail(c, YPAK_ERR_FORMAT, "a text that is not UTF-8: '%.*s'", (int)(n < 60 ? n : 60), s); }
    return v;
}
static jv* jstrz(C* c, const char* s) { return jstr(c, s, strlen(s)); }
static jv* jint(C* c, int64_t i) { return jok(c, yjs_new_int(jar(c), i)); }
static jv* jbool(C* c, int b) { return jnew(c, b ? J_TRUE : J_FALSE); }
static void jpush(C* c, jv* a, jv* v) {
    if (!a || !v || yjs_push(jar(c), a, v) != YJS_OK) c->jfail = 1;
}
static jv* jget(const jv* o, const char* key) { return yjs_get(o, key); }
static void jset(C* c, jv* o, const char* key, jv* v) {
    if (!o || !v || yjs_set(jar(c), o, key, v) != YJS_OK) c->jfail = 1;
}
static jv* jcopy(C* c, const jv* v) { return v ? jok(c, yjs_copy(jar(c), v)) : NULL; }
/* The count, the i-th item of an array or member value of an object (in
 * key order), and the i-th key. */
static int jn(const jv* v) { return v->type == J_ARR || v->type == J_OBJ ? (int)v->n : 0; }
static jv* jat(const jv* v, int i) { return v->type == YJS_OBJECT ? v->u.members[i].value : v->u.items[i]; }
static const char* jkey(const jv* o, int i) { return o->u.members[i].key; }

/* The first number in the text (by position) that breaks the pack's rule. */
static void jints(const jv* v, const jv** first) {
    int i;
    if (v->type == J_INT) {
        size_t digits = v->n - (v->u.s[0] == '-');
        if ((!(v->flags & YJS_NUM_INT) || digits > 18) && (!*first || v->pos < (*first)->pos)) *first = v;
    } else if (v->type == J_ARR || v->type == J_OBJ) {
        for (i = 0; i < jn(v); i++) jints(jat(v, i), first);
    }
}

static jv* jparse(C* c, const char* text, size_t n, const char* what) {
    yjs_value* v;
    yjs_error e;
    const jv* bad = NULL;
    if (yjs_parse(jar(c), text, n, NULL, &v, &e) != YJS_OK) {
        cfail(c, e.code == YJS_ERR_NOMEM ? YPAK_ERR_NOMEM : YPAK_ERR_FORMAT, "%s: line %u, column %u: %s", what, (unsigned)e.line,
              (unsigned)e.column, e.msg);
        return NULL;
    }
    jints(v, &bad);
    if (bad) {
        uint32_t line, col;
        yjs_line_col(text, n, bad->pos, &line, &col);
        cfail(c, YPAK_ERR_FORMAT, "%s: line %u, column %u: %s", what, (unsigned)line, (unsigned)col,
              !(bad->flags & YJS_NUM_INT) ? "numbers are integers here; write a real value as a string (\"0.5\") so its decimal text is kept"
                                          : "an integer of more than 18 digits");
        return NULL;
    }
    return v;
}

static int jw_sb(void* ctx, const char* s, size_t n) {
    sb_put((sb*)ctx, s, n);
    return ((sb*)ctx)->bad;
}
/* The canonical form (docs/pack.md 7): ysp/json.h's YJS_WRITE_PRETTY. */
static void jw(C* c, sb* b, const jv* v) {
    if (c->jfail || yjs_write(v, YJS_WRITE_PRETTY, jw_sb, b) != YJS_OK) b->bad = 1;
}

/* Accessors with messages that name the resource. */
static int keys_ok(C* c, const jv* o, const char* const* allowed, const char* where) {
    int i;
    if (!o || o->type != J_OBJ) return cfail(c, YPAK_ERR_FORMAT, "%s: not an object", where);
    for (i = 0; i < jn(o); i++) {
        const yjs_member* m = &o->u.members[i];
        int k, ok = 0;
        for (k = 0; allowed[k]; k++)
            if (strlen(allowed[k]) == m->key_len && memcmp(allowed[k], m->key, m->key_len) == 0) ok = 1;
        if (!ok) return cfail(c, YPAK_ERR_FORMAT, "%s: unknown key '%.*s'", where, (int)m->key_len, m->key);
    }
    return 0;
}
static const char* str_of(const jv* o, const char* key, const char* def) {
    jv* v = jget(o, key);
    return v && v->type == J_STR ? v->u.s : def;
}
static int want_type(C* c, const jv* o, const char* key, int t, const char* where) {
    jv* v = jget(o, key);
    static const char* const tn[] = { "null", "a boolean", "a boolean", "an integer", "a string", "an array", "an object" };
    if (!v) return 0;
    if (v->type == t || (t == J_TRUE && (v->type == J_FALSE || v->type == J_TRUE))) return 0;
    return cfail(c, YPAK_ERR_FORMAT, "%s: '%s' must be %s", where, key, tn[t]);
}
/* A real value given as a decimal string, parsed by ysp/table.h. */
static int real_of(C* c, const jv* o, const char* key, const char* def, double* out, const char* where) {
    const char* s = str_of(o, key, def);
    int rc;
    if (want_type(c, o, key, J_STR, where)) return YPAK_ERR_FORMAT;
    rc = ytb_parse_number(s, strlen(s), out);
    if (rc != YTB_NUM_OK) return cfail(c, YPAK_ERR_FORMAT, "%s: '%s' is \"%s\", not a decimal number", where, key, s);
    return 0;
}

/* --- entries ------------------------------------------------------------------ */

typedef struct E {
    char*    name;  size_t nl;
    uint32_t kind, flags;
    const uint8_t* data; int64_t size;
    jv*      res;      /* the resource from the description                       */
    jv*      deriv;    /* derivation: op, by, the parameters with defaults         */
    jv*      sources;  /* [{path, sha256}]                                         */
    jv*      from;     /* entry names this one is made from, or NULL               */
    /* computed */
    uint64_t hash; uint32_t crc, chunk;
    uint64_t* ch; int64_t kc;
    uint8_t  sha[32];
    int64_t  hrel, drel;
    /* fonts: the face count; glyphs that glyph runs use, per face (a bitmap) */
    int      faces;
    uint8_t* used[16]; uint32_t n_glyphs[16];
} E;

typedef struct B {     /* a build */
    C*       c;
    E**      e;  int n, cap;
    jv*      desc;
    jv*      audio;
    int      rate, channels;
    int      priv;
    char     src_dir[1024];
    yol_ctx  ol;
    int      ol_ok;
} B;

static E* add_entry(B* b) {
    E* e;
    if (b->n == b->cap) {
        int nc = b->cap ? 2 * b->cap : 64;
        E** ne = (E**)calloc_c(b->c, (size_t)nc * sizeof(E*));
        if (!ne) return NULL;
        if (b->n) memcpy(ne, b->e, (size_t)b->n * sizeof(E*));
        b->e = ne;
        b->cap = nc;
    }
    e = (E*)calloc_c(b->c, sizeof(E));
    if (e) b->e[b->n++] = e;
    return e;
}

static E* find_entry(B* b, const char* name) {
    int i;
    for (i = 0; i < b->n; i++)
        if (b->e[i]->nl == strlen(name) && memcmp(b->e[i]->name, name, b->e[i]->nl) == 0) return b->e[i];
    return NULL;
}

/* A source file named by the description: a relative path in the reader's
 * name rule (no '..', no absolute path, no drive). */
static uint8_t* load_source(B* b, E* e, const char* rel, int64_t* n) {
    char path[2048];
    uint8_t* d;
    uint8_t sha[32];
    char hx[65];
    jv* s;
    if (!ypak_name_ok(rel, strlen(rel)) || strchr(rel, ':')) {
        cfail(b->c, YPAK_ERR_FORMAT, "'%s': source '%s' is not a relative path inside the sources' folder", e->name, rel);
        return NULL;
    }
    snprintf(path, sizeof path, "%s%s%s", b->src_dir, b->src_dir[0] ? "/" : "", rel);
    d = read_file(b->c, path, n);
    if (!d) return NULL;
    ypak_sha256(d, (size_t)*n, sha);
    hex(sha, 32, hx);
    s = jnew(b->c, J_OBJ);
    jset(b->c, s, "path", jstrz(b->c, rel));
    jset(b->c, s, "sha256", jstrz(b->c, hx));
    jpush(b->c, e->sources, s);
    return d;
}

static const char* ext_of(uint32_t kind) {
    switch (kind) {
    case YPAK_KIND_TABLE: return ".pstb";
    case YPAK_KIND_CALIBRATION: return ".yspcal";
    case YPAK_KIND_CURVESET: return ".yspcset";
    case YPAK_KIND_ARTWORK: return ".yspart";
    case YPAK_KIND_SHADER: return ".yspshd";
    case YPAK_KIND_TEXTURE: return ".ysptex";
    default: return NULL;
    }
}

/* The source path with the kind's extension. */
static char* default_name(B* b, const char* path, uint32_t kind) {
    const char* ext = ext_of(kind);
    const char* dot = strrchr(path, '.');
    const char* sl = strrchr(path, '/');
    size_t stem = (dot && (!sl || dot > sl)) ? (size_t)(dot - path) : strlen(path);
    char* r;
    if (!ext) return cdup(b->c, path, strlen(path));
    r = (char*)calloc_c(b->c, stem + strlen(ext) + 1);
    if (!r) return NULL;
    memcpy(r, path, stem);
    memcpy(r + stem, ext, strlen(ext));
    return r;
}

static void w16(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void w32(uint8_t* p, uint32_t v) { w16(p, v & 0xFFFF); w16(p + 2, v >> 16); }
static void w64(uint8_t* p, uint64_t v) { w32(p, (uint32_t)v); w32(p + 4, (uint32_t)(v >> 32)); }
static uint32_t r16(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static uint32_t r32(const uint8_t* p) { return r16(p) | (r16(p + 2) << 16); }

static jv* deriv_new(B* b, const char* op, const char* by) {
    jv* d = jnew(b->c, J_OBJ);
    jset(b->c, d, "op", jstrz(b->c, op));
    jset(b->c, d, "by", jstrz(b->c, by));
    return d;
}

/* --- kinds ------------------------------------------------------------------- */

static int build_copy(B* b, E* e, const char* by) {
    const char* src = str_of(e->res, "source", NULL);
    if (!src) return cfail(b->c, YPAK_ERR_FORMAT, "'%s': no 'source'", e->name);
    e->data = load_source(b, e, src, &e->size);
    if (!e->data) return YPAK_ERR_IO;
    e->deriv = deriv_new(b, "copy", by);
    return 0;
}

static int build_table(B* b, E* e) {
    static const char* const keys[] = { "name", "kind", "source", "license", "delimiter", "types", "allow_empty", NULL };
    const char* src = str_of(e->res, "source", NULL);
    const char* delim = str_of(e->res, "delimiter", ",");
    jv* types = jget(e->res, "types");
    jv* ae = jget(e->res, "allow_empty");
    ytb_csv_desc d;
    ytb_table t;
    int64_t n;
    const uint8_t* text;
    void* arena;
    int i;
    char ver[64];
    if (keys_ok(b->c, e->res, keys, e->name) || want_type(b->c, e->res, "types", J_OBJ, e->name) ||
        want_type(b->c, e->res, "allow_empty", J_TRUE, e->name) || want_type(b->c, e->res, "delimiter", J_STR, e->name))
        return YPAK_ERR_FORMAT;
    if (!src) return cfail(b->c, YPAK_ERR_FORMAT, "'%s': no 'source' (a CSV or TSV file)", e->name);
    text = load_source(b, e, src, &n);
    if (!text) return YPAK_ERR_IO;
    memset(&d, 0, sizeof d);
    d.text = text;
    d.len = (size_t)n;
    if (!strcmp(delim, ",")) d.delimiter = ',';
    else if (!strcmp(delim, ";")) d.delimiter = ';';
    else if (!strcmp(delim, "\t")) d.delimiter = '\t';
    else return cfail(b->c, YPAK_ERR_FORMAT, "'%s': delimiter must be \",\", \";\" or \"\\t\"", e->name);
    d.allow_empty = ae && ae->type == J_TRUE;
    if (types) {
        if (jn(types) > YTB_MAX_COLUMNS) return cfail(b->c, YPAK_ERR_FORMAT, "'%s': more than %d types", e->name, YTB_MAX_COLUMNS);
        for (i = 0; i < jn(types); i++) {
            const jv* tv = jat(types, i);
            d.types[i].name = jkey(types, i);
            if (tv->type != J_STR) return cfail(b->c, YPAK_ERR_FORMAT, "'%s': a type must be a string", e->name);
            if (!strcmp(tv->u.s, "integer")) d.types[i].type = YTB_INTEGER;
            else if (!strcmp(tv->u.s, "number")) d.types[i].type = YTB_NUMBER;
            else if (!strcmp(tv->u.s, "string")) d.types[i].type = YTB_STRING;
            else return cfail(b->c, YPAK_ERR_FORMAT, "'%s': type '%s' is not integer, number or string", e->name, tv->u.s);
        }
        d.n_types = jn(types);
    }
    memset(&t, 0, sizeof t);
    (void)ytb_open_csv(&t, &d);
    if (!t.need) return cfail(b->c, YPAK_ERR_FORMAT, "'%s': %s", e->name, ytb_error(&t));
    arena = calloc_c(b->c, t.need + 16);
    if (!arena) return cfail(b->c, YPAK_ERR_NOMEM, "'%s': out of memory", e->name);
    d.arena = arena;
    d.arena_size = t.need;
    if (!ytb_open_csv(&t, &d)) return cfail(b->c, YPAK_ERR_FORMAT, "'%s': %s", e->name, ytb_error(&t));
    e->data = t.base;
    e->size = (int64_t)t.size;
    snprintf(ver, sizeof ver, "ysp/table.h %s", YTB_VERSION_STRING);
    e->deriv = deriv_new(b, "csv", ver);
    jset(b->c, e->deriv, "delimiter", jstrz(b->c, delim));
    jset(b->c, e->deriv, "allow_empty", jbool(b->c, d.allow_empty));
    jset(b->c, e->deriv, "types", types ? jcopy(b->c, types) : jnew(b->c, J_OBJ));
    return 0;
}

/* Open question 1 of docs/pack.md (decided 2026-10-09): a
 * calibration in a pack is for preview and simulation; the player never
 * applies it on a rig, whose calibration comes from its rig profile. The
 * one place that decides what a CALIBRATION entry is. */
static const char* calibration_role(void) { return "preview"; }

static int build_calibration(B* b, E* e) {
    static const char* const keys[] = { "name", "kind", "source", "license", NULL };
    ycol_cal* cal;
    char msg[256], ver[64];
    int rc;
    if (keys_ok(b->c, e->res, keys, e->name)) return YPAK_ERR_FORMAT;
    rc = build_copy(b, e, "ysp/color.h");
    if (rc) return rc;
    cal = (ycol_cal*)calloc_c(b->c, sizeof(ycol_cal));
    if (!cal) return cfail(b->c, YPAK_ERR_NOMEM, "out of memory");
    if (ycol_cal_load(cal, e->data, (size_t)e->size, msg, sizeof msg) < 0)
        return cfail(b->c, YPAK_ERR_FORMAT, "'%s': %s", e->name, msg);
    snprintf(ver, sizeof ver, "ysp/color.h %s", YCOL_VERSION_STRING);
    e->deriv = deriv_new(b, "copy", ver);
    jset(b->c, e->deriv, "role", jstrz(b->c, calibration_role()));
    return 0;
}

/* OS/2 fsType of a face (TrueType, OpenType, or a face of a collection);
 * -1 when the font has no OS/2 table. */
static int font_fstype(const uint8_t* d, int64_t n, int face) {
    uint32_t off = 0, nt, i;
    if (n >= 12 && memcmp(d, "ttcf", 4) == 0) {
        uint32_t nf = ((uint32_t)d[8] << 24) | ((uint32_t)d[9] << 16) | ((uint32_t)d[10] << 8) | d[11];
        if ((uint32_t)face >= nf || 12 + 4 * (int64_t)nf > n) return -1;
        off = ((uint32_t)d[12 + 4 * face] << 24) | ((uint32_t)d[13 + 4 * face] << 16) | ((uint32_t)d[14 + 4 * face] << 8) | d[15 + 4 * face];
    }
    if ((int64_t)off + 12 > n) return -1;
    nt = ((uint32_t)d[off + 4] << 8) | d[off + 5];
    for (i = 0; i < nt; i++) {
        int64_t r = (int64_t)off + 12 + 16 * (int64_t)i;
        uint32_t to;
        if (r + 16 > n) return -1;
        if (memcmp(d + r, "OS/2", 4) != 0) continue;
        to = ((uint32_t)d[r + 8] << 24) | ((uint32_t)d[r + 9] << 16) | ((uint32_t)d[r + 10] << 8) | d[r + 11];
        if ((int64_t)to + 10 > n) return -1;
        return (int)(((uint32_t)d[to + 8] << 8) | d[to + 9]);
    }
    return -1;
}

/* Open question 7 of docs/pack.md (decided 2026-10-09): a font whose
 * OS/2 fsType says "restricted license embedding" (bits 0 to 3 equal 2) is
 * refused unless the source description marks the pack private. The one
 * check that decides it. */
static int font_license_ok(B* b, const E* e, int fstype) {
    if (fstype >= 0 && (fstype & 0xF) == 2 && !b->priv)
        return cfail(b->c, YPAK_ERR_FORMAT, "'%s': the font's license forbids embedding (OS/2 fsType 0x%04x, restricted); "
                     "packs move between labs. Use a font licensed for redistribution, or set \"private\": true in the "
                     "source description for a pack that stays in the lab", e->name, fstype);
    return 0;
}

static int build_font(B* b, E* e) {
    static const char* const keys[] = { "name", "kind", "source", "license", NULL };
    char msg[256], ver[64];
    int rc, faces, f, fst = -1;
    if (keys_ok(b->c, e->res, keys, e->name)) return YPAK_ERR_FORMAT;
    rc = build_copy(b, e, "ysp/outline.h");
    if (rc) return rc;
    faces = yol_font_faces(e->data, (size_t)e->size);
    if (faces < 1 || faces > 16) return cfail(b->c, YPAK_ERR_FORMAT, "'%s': not a font, or more than 16 faces", e->name);
    for (f = 0; f < faces; f++) {
        yol_font yf;
        int t;
        if (yol_font_view(&yf, e->data, (size_t)e->size, f, msg, sizeof msg) < 0)
            return cfail(b->c, YPAK_ERR_FORMAT, "'%s' face %d: %s", e->name, f, msg);
        e->n_glyphs[f] = (uint32_t)yf.n_glyphs;
        t = font_fstype(e->data, e->size, f);
        if (f == 0) fst = t;
        rc = font_license_ok(b, e, t);
        if (rc) return rc;
    }
    e->faces = faces;
    snprintf(ver, sizeof ver, "ysp/outline.h %s", YOL_VERSION_STRING);
    e->deriv = deriv_new(b, "copy", ver);
    jset(b->c, e->deriv, "faces", jint(b->c, faces));
    jset(b->c, e->deriv, "fs_type", jint(b->c, fst));
    return 0;
}

/* A curve set body (YSPCSET1) from an outline builder set. */
static uint8_t* cset_bytes(B* b, const yol_cset* s, uint32_t units, uint32_t flags, uint32_t face, int64_t* n) {
    int64_t total = 64 + 16 * (int64_t)s->n_texels + 4 * (int64_t)s->n_words;
    uint8_t* d = (uint8_t*)calloc_c(b->c, (size_t)total + 16);
    if (!d) return NULL;
    memcpy(d, "YSPCSET1", 8);
    w32(d + 8, 1); w32(d + 12, 64); w32(d + 16, units); w32(d + 20, s->words[2]); w32(d + 24, flags); w32(d + 28, face);
    w64(d + 32, s->n_texels); w64(d + 40, s->n_words); w64(d + 48, 64); w64(d + 56, 64 + 16 * (uint64_t)s->n_texels);
    memcpy(d + 64, s->texels, 16 * (size_t)s->n_texels);
    memcpy(d + 64 + 16 * (size_t)s->n_texels, s->words, 4 * (size_t)s->n_words);
    *n = total;
    return d;
}

static int check_cset(B* b, const E* e, const yol_cset* s) {
    ygfx_cset_desc cd;
    char msg[256];
    memset(&cd, 0, sizeof cd);
    cd.texels = s->texels;
    cd.n_texels = s->n_texels;
    cd.words = s->words;
    cd.n_words = s->n_words;
    if (ygfx_cset_check(&cd, msg, sizeof msg) < 0)
        return cfail(b->c, YPAK_ERR_FORMAT, "'%s': the curve set fails ygfx_cset_check: %s", e->name, msg);
    return 0;
}

static int build_curveset(B* b, E* e) {
    static const char* const keys[] = { "name", "kind", "from", "license", "face", "glyphs", "tol", NULL };
    const char* from = str_of(e->res, "from", NULL);
    const char* glyphs = str_of(e->res, "glyphs", "all");
    const char* tols = str_of(e->res, "tol", "1e-4");
    jv* fv = jget(e->res, "face");
    int face = fv && fv->type == J_INT ? (int)fv->i : 0, rc;
    double tol;
    E* font;
    yol_font yf;
    yol_cset set;
    yol_cset_desc sd;
    char msg[256], ver[64];
    uint32_t* list = NULL;
    uint32_t nl = 0, g;
    if (keys_ok(b->c, e->res, keys, e->name) || want_type(b->c, e->res, "face", J_INT, e->name) ||
        want_type(b->c, e->res, "from", J_STR, e->name) || want_type(b->c, e->res, "glyphs", J_STR, e->name))
        return YPAK_ERR_FORMAT;
    if (real_of(b->c, e->res, "tol", "1e-4", &tol, e->name)) return YPAK_ERR_FORMAT;
    if (!from) return cfail(b->c, YPAK_ERR_FORMAT, "'%s': no 'from' (the font entry)", e->name);
    font = find_entry(b, from);
    if (!font || font->kind != YPAK_KIND_FONT) return cfail(b->c, YPAK_ERR_FORMAT, "'%s': 'from' is '%s', not a font entry of this pack", e->name, from);
    if (face < 0 || face >= font->faces) return cfail(b->c, YPAK_ERR_FORMAT, "'%s': face %d; the font has %d", e->name, face, font->faces);
    if (strcmp(glyphs, "all") && strcmp(glyphs, "used"))
        return cfail(b->c, YPAK_ERR_FORMAT, "'%s': glyphs must be \"all\" or \"used\"", e->name);
    if (yol_font_view(&yf, font->data, (size_t)font->size, face, msg, sizeof msg) < 0)
        return cfail(b->c, YPAK_ERR_FORMAT, "'%s': %s", e->name, msg);
    if (!strcmp(glyphs, "used")) {
        list = (uint32_t*)calloc_c(b->c, sizeof(uint32_t) * ((size_t)yf.n_glyphs + 1));
        if (!list) return cfail(b->c, YPAK_ERR_NOMEM, "out of memory");
        list[nl++] = 0;   /* .notdef: what a missing glyph draws */
        if (font->used[face])
            for (g = 1; g < (uint32_t)yf.n_glyphs; g++)
                if (font->used[face][g >> 3] & (1u << (g & 7))) list[nl++] = g;
    }
    memset(&sd, 0, sizeof sd);
    sd.n_glyphs = (uint32_t)yf.n_glyphs;
    sd.backward = true;
    if (!yol_cset_open(&set, &b->ol, &sd)) {
        yol_cset_close(&set);
        return cfail(b->c, YPAK_ERR_FORMAT, "'%s': %s", e->name, yol_error(&b->ol));
    }
    rc = yol_cset_add_font(&set, &yf, list, list ? nl : 0, tol);
    if (rc < 0) {
        yol_cset_close(&set);
        return cfail(b->c, YPAK_ERR_FORMAT, "'%s': %s", e->name, yol_error(&b->ol));
    }
    rc = check_cset(b, e, &set);
    if (rc == 0) {
        e->data = cset_bytes(b, &set, YPAK_CSET_UNITS_EM, list ? 0u : YPAK_CSET_ALL, (uint32_t)face, &e->size);
        if (!e->data) rc = cfail(b->c, YPAK_ERR_NOMEM, "out of memory");
    }
    yol_cset_close(&set);
    if (rc) return rc;
    e->from = jnew(b->c, J_ARR);
    jpush(b->c, e->from, jstrz(b->c, from));
    snprintf(ver, sizeof ver, "ysp/outline.h %s", YOL_VERSION_STRING);
    e->deriv = deriv_new(b, "outline", ver);
    jset(b->c, e->deriv, "face", jint(b->c, face));
    jset(b->c, e->deriv, "glyphs", jstrz(b->c, glyphs));
    jset(b->c, e->deriv, "tol", jstrz(b->c, tols));
    jset(b->c, e->deriv, "resolve", jbool(b->c, 1));
    jset(b->c, e->deriv, "backward", jbool(b->c, 1));
    return 0;
}

static int build_artwork(B* b, E* e) {
    static const char* const keys[] = { "name", "kind", "source", "license", "tol", NULL };
    const char* src = str_of(e->res, "source", NULL);
    const char* tols = str_of(e->res, "tol", "0");
    double tol, vb[4];
    int64_t n, setn = 0;
    const uint8_t* text;
    yol_svg_layer* layers;
    yol_svg_desc sd;
    yol_cset set;
    yol_cset_desc cd;
    char msg[256], ver[64];
    int nl, cap = 256, i, rc;
    uint8_t* setb;
    if (keys_ok(b->c, e->res, keys, e->name) || real_of(b->c, e->res, "tol", "0", &tol, e->name)) return YPAK_ERR_FORMAT;
    if (!src) return cfail(b->c, YPAK_ERR_FORMAT, "'%s': no 'source' (an SVG file)", e->name);
    text = load_source(b, e, src, &n);
    if (!text) return YPAK_ERR_IO;
    for (;;) {
        layers = (yol_svg_layer*)calloc_c(b->c, sizeof(yol_svg_layer) * (size_t)cap);
        if (!layers) return cfail(b->c, YPAK_ERR_NOMEM, "out of memory");
        memset(&sd, 0, sizeof sd);
        sd.tol = tol;
        sd.layers = layers;
        sd.max_layers = cap;
        nl = yol_svg(&b->ol, (const char*)text, (size_t)n, &sd, vb, msg, sizeof msg);
        if (nl == YOL_ERR_FULL && cap < (1 << 20)) { cap *= 4; continue; }
        break;
    }
    if (nl < 0) return cfail(b->c, YPAK_ERR_FORMAT, "'%s': %s", e->name, msg);
    memset(&cd, 0, sizeof cd);
    cd.n_glyphs = (uint32_t)(nl ? nl : 1);
    cd.backward = true;
    rc = yol_cset_open(&set, &b->ol, &cd) ? 0 : -1;
    for (i = 0; rc >= 0 && i < nl; i++) rc = yol_cset_add(&set, (uint32_t)i, &layers[i].path);
    if (rc < 0) rc = cfail(b->c, YPAK_ERR_FORMAT, "'%s': %s", e->name, yol_error(&b->ol));
    else rc = check_cset(b, e, &set);
    setb = rc ? NULL : cset_bytes(b, &set, YPAK_CSET_UNITS_USER, 0, 0, &setn);
    yol_cset_close(&set);
    if (rc) { for (i = 0; i < nl; i++) yol_path_free(&layers[i].path); return rc; }
    {
        int64_t lo = 80, co = 80 + 16 * (int64_t)nl;
        uint8_t* d = (uint8_t*)calloc_c(b->c, (size_t)(co + setn) + 16);
        if (!d || !setb) return cfail(b->c, YPAK_ERR_NOMEM, "out of memory");
        memcpy(d, "YSPARTW1", 8);
        w32(d + 8, 1); w32(d + 12, 80); w32(d + 16, (uint32_t)nl); w32(d + 20, 0);
        for (i = 0; i < 4; i++) { uint64_t v; memcpy(&v, &vb[i], 8); w64(d + 24 + 8 * i, v); }
        w64(d + 56, (uint64_t)lo); w64(d + 64, (uint64_t)co); w64(d + 72, (uint64_t)setn);
        for (i = 0; i < nl; i++) {
            uint8_t* l = d + lo + 16 * i;
            w32(l, (uint32_t)i); w32(l + 4, layers[i].rgba); w32(l + 8, (uint32_t)layers[i].source); w32(l + 12, (uint32_t)layers[i].element);
            yol_path_free(&layers[i].path);
        }
        memcpy(d + co, setb, (size_t)setn);
        e->data = d;
        e->size = co + setn;
    }
    snprintf(ver, sizeof ver, "ysp/outline.h %s", YOL_VERSION_STRING);
    e->deriv = deriv_new(b, "svg", ver);
    jset(b->c, e->deriv, "tol", jstrz(b->c, tols));
    return 0;
}

/* A canonical WAV (RIFF, or RF64 past 4 GiB): fmt and data only. */
static uint8_t* wav_write(B* b, int tag, int bits, int valid, int ch, uint32_t rate, const uint8_t* pcm, int64_t bytes, int64_t* n) {
    int ext = valid != bits;
    int64_t fmt = ext ? 40 : 16, total = 12 + 8 + fmt + 8 + bytes;
    uint8_t* d = (uint8_t*)calloc_c(b->c, (size_t)total + 16);
    uint8_t* p;
    if (!d || total - 8 > 0xFFFFFFFFLL) return NULL;
    memcpy(d, "RIFF", 4); w32(d + 4, (uint32_t)(total - 8)); memcpy(d + 8, "WAVE", 4);
    p = d + 12;
    memcpy(p, "fmt ", 4); w32(p + 4, (uint32_t)fmt);
    w16(p + 8, ext ? 0xFFFEu : (uint32_t)tag); w16(p + 10, (uint32_t)ch); w32(p + 12, rate);
    w32(p + 16, rate * (uint32_t)ch * (uint32_t)(bits / 8)); w16(p + 20, (uint32_t)(ch * bits / 8)); w16(p + 22, (uint32_t)bits);
    if (ext) {
        static const uint8_t guid_tail[14] = { 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 };
        w16(p + 24, 22); w16(p + 26, (uint32_t)valid); w32(p + 28, 0); w16(p + 32, (uint32_t)tag); memcpy(p + 34, guid_tail, 14);
    }
    p += 8 + fmt;
    memcpy(p, "data", 4); w32(p + 4, (uint32_t)bytes);
    memcpy(p + 8, pcm, (size_t)bytes);
    *n = total;
    return d;
}

/* The forms ysp/audio.h refuses that convert exactly: unsigned 8-bit to
 * 16-bit, 32-bit integers whose low byte is 0 to 24 in 32, 64-bit floats
 * that are exact in 32-bit float. 0 and the new bytes, or a refusal. */
static int wav_convert(B* b, E* e, const uint8_t* d, int64_t n, const char** what) {
    int64_t pos = 12, data_off = -1, data_n = 0, i;
    int tag = 0, ch = 0, bits = 0;
    uint32_t rate = 0;
    if (n < 12 || memcmp(d, "RIFF", 4) || memcmp(d + 8, "WAVE", 4)) return 1;
    while (pos + 8 <= n) {
        uint32_t sz = r32(d + pos + 4);
        if (!memcmp(d + pos, "fmt ", 4) && sz >= 16 && pos + 8 + 16 <= n) {
            tag = (int)r16(d + pos + 8); ch = (int)r16(d + pos + 10); rate = r32(d + pos + 12); bits = (int)r16(d + pos + 22);
        } else if (!memcmp(d + pos, "data", 4)) {
            data_off = pos + 8;
            data_n = sz;
            break;
        }
        pos += 8 + (int64_t)sz + (sz & 1u);
    }
    if (data_off < 0 || data_off + data_n > n || ch <= 0) return 1;
    if (tag == 1 && bits == 8) {
        int16_t* o = (int16_t*)calloc_c(b->c, (size_t)data_n * 2 + 2);
        for (i = 0; o && i < data_n; i++) o[i] = (int16_t)(((int)d[data_off + i] - 128) * 256);
        e->data = o ? wav_write(b, 1, 16, 16, ch, rate, (const uint8_t*)o, data_n * 2, &e->size) : NULL;
        *what = "unsigned 8-bit to 16-bit";
        return e->data ? 0 : cfail(b->c, YPAK_ERR_NOMEM, "out of memory");
    }
    if (tag == 1 && bits == 32) {
        for (i = 0; i + 4 <= data_n; i += 4)
            if (d[data_off + i] != 0)
                return cfail(b->c, YPAK_ERR_FORMAT, "'%s': 32-bit integer samples use their low byte (sample %lld); "
                             "they are not exact in float: export 24-bit or float", e->name, (long long)(i / 4));
        e->data = wav_write(b, 1, 32, 24, ch, rate, d + data_off, data_n, &e->size);
        *what = "32-bit integer with a zero low byte to 24 in 32";
        return e->data ? 0 : cfail(b->c, YPAK_ERR_NOMEM, "out of memory");
    }
    if (tag == 3 && bits == 64) {
        float* o = (float*)calloc_c(b->c, (size_t)data_n / 2 + 4);
        for (i = 0; o && i + 8 <= data_n; i += 8) {
            double v;
            float f;
            memcpy(&v, d + data_off + i, 8);
            f = (float)v;
            if ((double)f != v || v != v)
                return cfail(b->c, YPAK_ERR_FORMAT, "'%s': 64-bit float sample %lld is not exact in 32-bit float; "
                             "export 32-bit float", e->name, (long long)(i / 8));
            o[i / 8] = f;
        }
        e->data = o ? wav_write(b, 3, 32, 32, ch, rate, (const uint8_t*)o, data_n / 2, &e->size) : NULL;
        *what = "64-bit float, every sample exact, to 32-bit float";
        return e->data ? 0 : cfail(b->c, YPAK_ERR_NOMEM, "out of memory");
    }
    return 1;
}

static int build_audio(B* b, E* e) {
    static const char* const keys[] = { "name", "kind", "source", "license", "stream", NULL };
    const char* src = str_of(e->res, "source", NULL);
    jv* st = jget(e->res, "stream");
    yau_wav_desc wd;
    yau_wav_info info;
    char msg[256], ver[64];
    const char* conv = NULL;
    int64_t n, k;
    const uint8_t* d;
    int rc;
    if (keys_ok(b->c, e->res, keys, e->name) || want_type(b->c, e->res, "stream", J_TRUE, e->name)) return YPAK_ERR_FORMAT;
    if (!src) return cfail(b->c, YPAK_ERR_FORMAT, "'%s': no 'source' (a WAV file)", e->name);
    if (!b->rate) return cfail(b->c, YPAK_ERR_FORMAT, "'%s': audio needs the project's \"audio\": {\"rate\", \"channels\"} in the description", e->name);
    d = load_source(b, e, src, &n);
    if (!d) return YPAK_ERR_IO;
    e->data = d;
    e->size = n;
    memset(&wd, 0, sizeof wd);
    wd.data = d;
    wd.size = (size_t)n;
    rc = yau_wav_probe(&wd, &info, msg, sizeof msg);
    if (rc < 0) {
        int cv = wav_convert(b, e, d, n, &conv);
        if (cv < 0) return cv;
        if (cv > 0) return cfail(b->c, YPAK_ERR_FORMAT, "'%s': %s", e->name, msg);
        wd.data = e->data;
        wd.size = (size_t)e->size;
        rc = yau_wav_probe(&wd, &info, msg, sizeof msg);
        if (rc < 0) return cfail(b->c, YPAK_ERR_FORMAT, "'%s': after converting: %s", e->name, msg);
    }
    if ((int)info.rate != b->rate)
        return cfail(b->c, YPAK_ERR_FORMAT, "'%s': %u Hz; the project rate is %d Hz. ysp never resamples at play, and this "
                     "tool does not resample yet: export the file at %d Hz", e->name, info.rate, b->rate, b->rate);
    if (info.channels != 1 && (int)info.channels != b->channels)
        return cfail(b->c, YPAK_ERR_FORMAT, "'%s': %u channels; the project has %d (1 is also accepted)", e->name, info.channels, b->channels);
    if (info.format == YAU_WAV_F32) {
        const uint8_t* s = (const uint8_t*)e->data + info.data_offset;
        for (k = 0; k < info.frames * info.channels; k++) {
            uint32_t u = r32(s + 4 * k);
            if ((u & 0x7F800000u) == 0x7F800000u)
                return cfail(b->c, YPAK_ERR_FORMAT, "'%s': float sample %lld is not a number (NaN or infinity)", e->name, (long long)k);
        }
    }
    if (st && st->type == J_TRUE) e->flags |= YPAK_F_STREAM;
    snprintf(ver, sizeof ver, "ysp/audio.h %s", YAU_VERSION_STRING);
    e->deriv = deriv_new(b, conv ? "convert" : "copy", ver);
    if (conv) jset(b->c, e->deriv, "convert", jstrz(b->c, conv));
    jset(b->c, e->deriv, "stream", jbool(b->c, (e->flags & YPAK_F_STREAM) != 0));
    return 0;
}

static int build_shader(B* b, E* e) {
    static const char* const keys[] = { "name", "kind", "source", "license", "mode", NULL };
    static const char* const modes[3] = { "modulation", "color", "add" };
    const char* src = str_of(e->res, "source", NULL);
    const char* mode = str_of(e->res, "mode", NULL);
    int64_t n, i;
    const uint8_t* body;
    int m = -1, len;
    char* wrapped;
    uint32_t params = 0, tex = 0, flags = 0;
    uint64_t wh;
    char ver[64];
    if (keys_ok(b->c, e->res, keys, e->name)) return YPAK_ERR_FORMAT;
    if (!src || !mode) return cfail(b->c, YPAK_ERR_FORMAT, "'%s': a shader needs 'source' and 'mode'", e->name);
    for (i = 0; i < 3; i++) if (!strcmp(mode, modes[i])) m = (int)i;
    if (m < 0) return cfail(b->c, YPAK_ERR_FORMAT, "'%s': mode must be modulation, color or add", e->name);
    body = load_source(b, e, src, &n);
    if (!body) return YPAK_ERR_IO;
    if (memchr(body, 0, (size_t)n)) return cfail(b->c, YPAK_ERR_FORMAT, "'%s': the shader text holds a NUL byte", e->name);
    len = ygfx_shader_wrap((const char*)body, (ygfx_shader_mode)m, NULL, 0);
    if (len > -1000) return cfail(b->c, YPAK_ERR_FORMAT, "'%s': ygfx_shader_wrap refused the body (%d)", e->name, len);
    len = -len - 1000;
    wrapped = (char*)calloc_c(b->c, (size_t)len + 1);
    if (!wrapped || ygfx_shader_wrap((const char*)body, (ygfx_shader_mode)m, wrapped, (size_t)len + 1) != len)
        return cfail(b->c, YPAK_ERR_FORMAT, "'%s': ygfx_shader_wrap failed", e->name);
    wh = ypak_xxh64(wrapped, (size_t)len, 0);
    /* reflection by tokens: ysp_param(k) with a literal k, the textures,
     * time and seed */
    for (i = 0; i < n; i++) {
        const char* s = (const char*)body + i;
        int64_t left = n - i;
        if (i > 0 && (((body[i - 1] | 32) >= 'a' && (body[i - 1] | 32) <= 'z') || body[i - 1] == '_' || (body[i - 1] >= '0' && body[i - 1] <= '9')))
            continue;
        if (left >= 10 && !memcmp(s, "ysp_param(", 10)) {
            int64_t j = i + 10, v = 0, nd = 0;
            while (j < n && body[j] == ' ') j++;
            while (j < n && body[j] >= '0' && body[j] <= '9' && nd < 3) { v = v * 10 + (body[j] - '0'); j++; nd++; }
            while (j < n && body[j] == ' ') j++;
            if (nd && j < n && body[j] == ')' && v < 32) params |= 1u << v;
            else params = 0xFFFFFFFFu;
        } else if (left >= 8 && !memcmp(s, "ysp_time", 8)) flags |= YPAK_SHADER_TIME;
        else if (left >= 8 && !memcmp(s, "ysp_seed", 8)) flags |= YPAK_SHADER_SEED;
        else if (left >= 8 && !memcmp(s, "ysp_tex", 7) && s[7] >= '0' && s[7] <= '2') tex |= 1u << (s[7] - '0');
        else if (left >= 9 && !memcmp(s, "ysp_utex0", 9)) tex |= 8u;
    }
    {
        int64_t no = 64 + n + 1, total = (no + (int64_t)e->nl + 1 + 15) & ~(int64_t)15;
        uint8_t* d = (uint8_t*)calloc_c(b->c, (size_t)total + 16);
        if (!d) return cfail(b->c, YPAK_ERR_NOMEM, "out of memory");
        memcpy(d, "YSPSHAD1", 8);
        w32(d + 8, 1); w32(d + 12, 64); w32(d + 16, (uint32_t)m); w32(d + 20, flags); w32(d + 24, params); w32(d + 28, tex);
        w32(d + 32, YGFX_SHADER_CONTRACT); w32(d + 36, 0); w64(d + 40, wh);
        w32(d + 48, 64); w32(d + 52, (uint32_t)n); w32(d + 56, (uint32_t)no); w32(d + 60, (uint32_t)e->nl);
        memcpy(d + 64, body, (size_t)n);
        memcpy(d + no, e->name, e->nl);
        e->data = d;
        e->size = total;
    }
    snprintf(ver, sizeof ver, "ysp/gfx.h %s", YGFX_VERSION_STRING);
    e->deriv = deriv_new(b, "shader", ver);
    jset(b->c, e->deriv, "mode", jstrz(b->c, mode));
    jset(b->c, e->deriv, "contract", jint(b->c, YGFX_SHADER_CONTRACT));
    jset(b->c, e->deriv, "validated", jbool(b->c, 0));
    return 0;
}

static int build_texture(B* b, E* e) {
    static const char* const keys[] = { "name", "kind", "source", "license", "compression", "encoding", "primaries", "format", NULL };
    const char* src = str_of(e->res, "source", NULL);
    const char* comp = str_of(e->res, "compression", "raw");
    const char* encs = str_of(e->res, "encoding", NULL);
    const char* fmts = str_of(e->res, "format", NULL);
    const char* prims = str_of(e->res, "primaries", NULL);
    LodePNGState st;
    unsigned w = 0, h = 0, err;
    unsigned char* px = NULL;
    const uint8_t* png;
    int64_t n;
    uint32_t fmt = 0, tb, trc = 0, prim = 0;
    uint8_t* raw;
    uint64_t raw_n;
    char ver[96];
    if (keys_ok(b->c, e->res, keys, e->name)) return YPAK_ERR_FORMAT;
    if (!src) return cfail(b->c, YPAK_ERR_FORMAT, "'%s': no 'source' (a PNG file)", e->name);
    png = load_source(b, e, src, &n);
    if (!png) return YPAK_ERR_IO;
    lodepng_state_init(&st);
    err = lodepng_inspect(&w, &h, &st, png, (size_t)n);
    if (err) { lodepng_state_cleanup(&st); return cfail(b->c, YPAK_ERR_FORMAT, "'%s': not a PNG (%s); phase 2 reads PNG only", e->name, lodepng_error_text(err)); }
    {
        LodePNGColorType ct = st.info_png.color.colortype;
        unsigned bd = st.info_png.color.bitdepth;
        const char* def;
        if (bd == 16 && ct == LCT_GREY) def = "r16ui";
        else if (bd == 16) def = NULL;
        else if (ct == LCT_GREY) def = "r8";
        else if (ct == LCT_GREY_ALPHA) def = "rg8";
        else def = "rgba8";
        if (!fmts) fmts = def;
        if (!fmts)
            return lodepng_state_cleanup(&st), cfail(b->c, YPAK_ERR_FORMAT, "'%s': a 16-bit color PNG; set \"format\": \"rgba32f\" "
                                                     "to store k / 65535 as float", e->name);
        if (!strcmp(fmts, "r8")) { fmt = YPAK_TEX_R8; st.info_raw.colortype = LCT_GREY; st.info_raw.bitdepth = 8; }
        else if (!strcmp(fmts, "rg8")) { fmt = YPAK_TEX_RG8; st.info_raw.colortype = LCT_GREY_ALPHA; st.info_raw.bitdepth = 8; }
        else if (!strcmp(fmts, "rgba8")) { fmt = YPAK_TEX_RGBA8; st.info_raw.colortype = LCT_RGBA; st.info_raw.bitdepth = 8; }
        else if (!strcmp(fmts, "r16ui")) { fmt = YPAK_TEX_R16UI; st.info_raw.colortype = LCT_GREY; st.info_raw.bitdepth = 16; }
        else if (!strcmp(fmts, "rgba32f")) { fmt = YPAK_TEX_RGBA32F; st.info_raw.colortype = LCT_RGBA; st.info_raw.bitdepth = 16; }
        else return lodepng_state_cleanup(&st), cfail(b->c, YPAK_ERR_FORMAT, "'%s': format '%s' is not r8, rg8, rgba8, r16ui or rgba32f", e->name, fmts);
        if (strcmp(fmts, def ? def : "") != 0) {
            /* asked for another form than the file's: only widening that is exact */
            int ok = (fmt == YPAK_TEX_RGBA8 && bd <= 8) || (fmt == YPAK_TEX_RGBA32F) || (fmt == YPAK_TEX_R8 && ct == LCT_GREY && bd < 8);
            if (!ok) return lodepng_state_cleanup(&st), cfail(b->c, YPAK_ERR_FORMAT, "'%s': format '%s' would drop or round values of this "
                                                              "PNG (color type %d, %u bits)", e->name, fmts, (int)ct, bd);
        }
        if (!encs) {
            if (bd == 16) return lodepng_state_cleanup(&st), cfail(b->c, YPAK_ERR_FORMAT, "'%s': a 16-bit PNG has no default encoding; set "
                                                                   "\"encoding\": \"linear\", \"srgb\" or \"device\"", e->name);
            encs = "srgb";
        }
    }
    if (!strcmp(encs, "srgb")) trc = 3;
    else if (!strcmp(encs, "linear")) trc = 4;
    else if (!strcmp(encs, "device")) trc = 1;
    else return lodepng_state_cleanup(&st), cfail(b->c, YPAK_ERR_FORMAT, "'%s': encoding must be srgb, linear or device", e->name);
    /* The stored bytes are what ygfx_texture() takes (docs/pack.md 4.9), so
     * every choice ysp/gfx.h would refuse at load is refused here, and
     * nothing is left for the player to guess: linear values carry no
     * encoding; gray has no primaries of its own (the display's white);
     * sRGB codes in RGB name their primaries, because an untagged PNG does
     * not say whether its RGB is BT.709's or the display's. */
    if (prims && strcmp(prims, "bt709") && strcmp(prims, "device"))
        return lodepng_state_cleanup(&st), cfail(b->c, YPAK_ERR_FORMAT, "'%s': primaries must be bt709 or device", e->name);
    if (fmt == YPAK_TEX_R16UI && trc != 4)
        return lodepng_state_cleanup(&st), cfail(b->c, YPAK_ERR_FORMAT, "'%s': r16ui holds linear values (ysp/gfx.h decodes no transfer on it): "
                                                 "set \"encoding\": \"linear\", or \"format\": \"rgba32f\" for %s codes", e->name, encs);
    if (trc == 4 || fmt == YPAK_TEX_R8 || fmt == YPAK_TEX_RG8 || trc == 1) {
        if (prims)
            return lodepng_state_cleanup(&st), cfail(b->c, YPAK_ERR_FORMAT, "'%s': no \"primaries\" here: %s", e->name,
                                                     trc == 4 ? "linear values carry no encoding (the display's own RGB)"
                                                     : trc == 1 ? "device codes are the display's own RGB"
                                                                : "a gray texture is the display's white at each level");
        prim = trc == 4 ? 0 : 1;   /* YGFX_PRIM_DEVICE */
    } else if (!prims) {
        return lodepng_state_cleanup(&st), cfail(b->c, YPAK_ERR_FORMAT, "'%s': sRGB codes in RGB need \"primaries\": \"bt709\" (converted "
                                                 "through the rig's calibration, which must have chromaticities) or \"device\" (the "
                                                 "transfer decoded, the RGB shown as the display's)", e->name);
    } else {
        prim = !strcmp(prims, "bt709") ? 2 : 1;   /* YGFX_PRIM_BT709, YGFX_PRIM_DEVICE */
    }
    if (strcmp(comp, "raw") && strcmp(comp, "qoi"))
        return lodepng_state_cleanup(&st), cfail(b->c, YPAK_ERR_FORMAT, "'%s': compression must be raw or qoi", e->name);
    if (!strcmp(comp, "qoi") && fmt != YPAK_TEX_RGBA8)
        return lodepng_state_cleanup(&st), cfail(b->c, YPAK_ERR_FORMAT, "'%s': QOI is for rgba8 only", e->name);
    err = lodepng_decode(&px, &w, &h, &st, png, (size_t)n);
    if (err) { lodepng_state_cleanup(&st); return cfail(b->c, YPAK_ERR_FORMAT, "'%s': PNG decode: %s", e->name, lodepng_error_text(err)); }
    /* principle 5: no color conversion, so a profile other than sRGB is refused */
    if (st.info_png.iccp_defined) { free(px); lodepng_state_cleanup(&st); return cfail(b->c, YPAK_ERR_FORMAT, "'%s': the PNG has an ICC profile (iCCP); ysp converts no color: export it without one", e->name); }
    if (st.info_png.gama_defined && st.info_png.gama_gamma != 45455) { free(px); lodepng_state_cleanup(&st); return cfail(b->c, YPAK_ERR_FORMAT, "'%s': the PNG's gAMA is %u/100000, not sRGB's 45455", e->name, st.info_png.gama_gamma); }
    if (st.info_png.chrm_defined && !(st.info_png.chrm_white_x == 31270 && st.info_png.chrm_white_y == 32900 && st.info_png.chrm_red_x == 64000 &&
                                      st.info_png.chrm_red_y == 33000 && st.info_png.chrm_green_x == 30000 && st.info_png.chrm_green_y == 60000 &&
                                      st.info_png.chrm_blue_x == 15000 && st.info_png.chrm_blue_y == 6000)) {
        free(px); lodepng_state_cleanup(&st);
        return cfail(b->c, YPAK_ERR_FORMAT, "'%s': the PNG's cHRM primaries are not sRGB's", e->name);
    }
    lodepng_state_cleanup(&st);
    tb = fmt == YPAK_TEX_R8 ? 1 : fmt == YPAK_TEX_RG8 ? 2 : fmt == YPAK_TEX_R16UI ? 2 : fmt == YPAK_TEX_RGBA8 ? 4 : 16;
    raw_n = (uint64_t)w * h * tb;
    raw = (uint8_t*)calloc_c(b->c, (size_t)raw_n + 16);
    if (!raw) { free(px); return cfail(b->c, YPAK_ERR_NOMEM, "out of memory"); }
    if (fmt == YPAK_TEX_R16UI) {
        uint64_t i;
        for (i = 0; i < (uint64_t)w * h; i++) { raw[2 * i] = px[2 * i + 1]; raw[2 * i + 1] = px[2 * i]; }   /* PNG is big-endian */
    } else if (fmt == YPAK_TEX_RGBA32F) {
        uint64_t i;
        for (i = 0; i < (uint64_t)w * h * 4; i++) {
            float f = (float)(((unsigned)px[2 * i] << 8) | px[2 * i + 1]) / 65535.0f;
            memcpy(raw + 4 * i, &f, 4);
        }
    } else {
        memcpy(raw, px, (size_t)raw_n);
    }
    free(px);
    {
        uint64_t stored = raw_n, total;
        uint8_t* q = NULL;
        uint8_t* d;
        if (!strcmp(comp, "qoi")) {
            size_t need = ypak_qoi_encode(raw, w, h, NULL, 0);
            q = (uint8_t*)calloc_c(b->c, need);
            if (!q) return cfail(b->c, YPAK_ERR_NOMEM, "out of memory");
            stored = ypak_qoi_encode(raw, w, h, q, need);
        }
        total = (64 + stored + 15) & ~(uint64_t)15;
        d = (uint8_t*)calloc_c(b->c, (size_t)total + 16);
        if (!d) return cfail(b->c, YPAK_ERR_NOMEM, "out of memory");
        memcpy(d, "YSPTEXR1", 8);
        w32(d + 8, 1); w32(d + 12, 64); w32(d + 16, w); w32(d + 20, h); w32(d + 24, fmt);
        if (trc != 4) { d[28] = 1; d[29] = 2; d[30] = (uint8_t)trc; d[31] = (uint8_t)prim; }   /* matrix RGB, full range; linear: all 0 */
        w32(d + 36, q ? YPAK_TEX_QOI : YPAK_TEX_RAW); w64(d + 40, stored); w64(d + 48, raw_n); w64(d + 56, ypak_xxh64(raw, (size_t)raw_n, 0));
        memcpy(d + 64, q ? q : raw, (size_t)stored);
        e->data = d;
        e->size = (int64_t)total;
    }
    snprintf(ver, sizeof ver, "lodepng %s", YPT_LODEPNG);
    e->deriv = deriv_new(b, "png", ver);
    jset(b->c, e->deriv, "compression", jstrz(b->c, comp));
    jset(b->c, e->deriv, "encoding", jstrz(b->c, encs));
    jset(b->c, e->deriv, "format", jstrz(b->c, fmts));
    if (prims) jset(b->c, e->deriv, "primaries", jstrz(b->c, prims));
    return 0;
}

typedef struct blk { const char* key; jv* v; } blk;
static int blk_cmp(const void* a, const void* b2) {
    const blk* x = (const blk*)a;
    const blk* y = (const blk*)b2;
    return strcmp(x->key, y->key);
}

static int build_glyphruns(B* b, E* e) {
    static const char* const keys[] = { "name", "kind", "license", "fonts", "blocks", NULL };
    static const char* const bkeys[] = { "key", "text", "size", "width", "line_height", "dir", "align", "lang", "color", NULL };
    jv* fonts = jget(e->res, "fonts");
    jv* blocks = jget(e->res, "blocks");
    ylay_lib* L = NULL;
    ylay_desc ld;
    ylay_block lb;
    E* fe[64];
    E* ce[64];
    int ff[64], nf, i, rc = 0;
    blk* bl;
    sb pool, runs, items, clus, lines, brec;
    uint32_t n_runs = 0, n_items = 0, n_lines = 0;
    char stamp[2048];
    if (keys_ok(b->c, e->res, keys, e->name) || want_type(b->c, e->res, "fonts", J_ARR, e->name) ||
        want_type(b->c, e->res, "blocks", J_ARR, e->name))
        return YPAK_ERR_FORMAT;
    if (!fonts || !jn(fonts) || jn(fonts) > 64) return cfail(b->c, YPAK_ERR_FORMAT, "'%s': 'fonts' lists 1 to 64 font entries", e->name);
    if (!blocks || !jn(blocks)) return cfail(b->c, YPAK_ERR_FORMAT, "'%s': 'blocks' is empty", e->name);
    nf = jn(fonts);
    e->from = jnew(b->c, J_ARR);
    for (i = 0; i < nf; i++) {
        const jv* f = jat(fonts, i);
        const char* fname = f->type == J_STR ? f->u.s : str_of(f, "font", NULL);
        jv* fv = f->type == J_OBJ ? jget(f, "face") : NULL;
        int face = fv && fv->type == J_INT ? (int)fv->i : 0, k;
        if (!fname) return cfail(b->c, YPAK_ERR_FORMAT, "'%s': a font is a name or {\"font\", \"face\"}", e->name);
        fe[i] = find_entry(b, fname);
        if (!fe[i] || fe[i]->kind != YPAK_KIND_FONT) return cfail(b->c, YPAK_ERR_FORMAT, "'%s': '%s' is not a font entry of this pack", e->name, fname);
        if (face < 0 || face >= fe[i]->faces) return cfail(b->c, YPAK_ERR_FORMAT, "'%s': '%s' has no face %d", e->name, fname, face);
        ff[i] = face;
        ce[i] = NULL;
        for (k = 0; k < b->n; k++) {
            E* q = b->e[k];
            jv* qf;
            if (q->kind != YPAK_KIND_CURVESET) continue;
            qf = jget(q->res, "face");
            if (!strcmp(str_of(q->res, "from", ""), fname) && (qf && qf->type == J_INT ? (int)qf->i : 0) == face) { ce[i] = q; break; }
        }
        if (!ce[i]) return cfail(b->c, YPAK_ERR_FORMAT, "'%s': no curveset entry is made from '%s' face %d; add one", e->name, fname, face);
        jpush(b->c, e->from, jstrz(b->c, fname));
    }
    memset(&ld, 0, sizeof ld);
    ld.ol = &b->ol;
    if (ylay_create(&L, &ld) < 0) return cfail(b->c, YPAK_ERR_NOMEM, "'%s': ylay_create failed", e->name);
    for (i = 0; i < nf; i++)
        if (ylay_add_font(L, fe[i]->data, (size_t)fe[i]->size, ff[i], fe[i]->name) < 0) {
            rc = cfail(b->c, YPAK_ERR_FORMAT, "'%s': %s", e->name, ylay_error(L));
            ylay_destroy(L);
            return rc;
        }
    bl = (blk*)calloc_c(b->c, sizeof(blk) * (size_t)jn(blocks));
    for (i = 0; bl && i < jn(blocks); i++) {
        if (keys_ok(b->c, jat(blocks, i), bkeys, e->name) || !str_of(jat(blocks, i), "key", NULL) || !str_of(jat(blocks, i), "text", NULL)) {
            ylay_destroy(L);
            return cfail(b->c, YPAK_ERR_FORMAT, "'%s': block %d needs 'key' and 'text'", e->name, i);
        }
        bl[i].key = str_of(jat(blocks, i), "key", "");
        bl[i].v = jat(blocks, i);
    }
    if (!bl) { ylay_destroy(L); return cfail(b->c, YPAK_ERR_NOMEM, "out of memory"); }
    qsort(bl, (size_t)jn(blocks), sizeof(blk), blk_cmp);
    for (i = 1; i < jn(blocks); i++)
        if (!strcmp(bl[i].key, bl[i - 1].key)) { ylay_destroy(L); return cfail(b->c, YPAK_ERR_FORMAT, "'%s': key '%s' twice", e->name, bl[i].key); }
    memset(&pool, 0, sizeof pool); memset(&runs, 0, sizeof runs); memset(&items, 0, sizeof items);
    memset(&clus, 0, sizeof clus); memset(&lines, 0, sizeof lines); memset(&brec, 0, sizeof brec);
    sb_put(&pool, "", 0);
    /* the fonts' names first */
    {
        uint8_t fr[16 * 64];
        for (i = 0; i < nf; i++) {
            w32(fr + 16 * i, (uint32_t)pool.n); w32(fr + 16 * i + 4, (uint32_t)ce[i]->nl);
            sb_put(&pool, ce[i]->name, ce[i]->nl); sb_put(&pool, "\0", 1);
            w32(fr + 16 * i + 8, (uint32_t)pool.n); w32(fr + 16 * i + 12, (uint32_t)fe[i]->nl);
            sb_put(&pool, fe[i]->name, fe[i]->nl); sb_put(&pool, "\0", 1);
        }
        sb_put(&brec, fr, (size_t)16 * (size_t)nf);   /* parked in brec's head; moved below */
    }
    memset(&lb, 0, sizeof lb);
    for (i = 0; rc == 0 && i < jn(blocks); i++) {
        const jv* v = bl[i].v;
        ylay_style s;
        double size, width, lh, color;
        const char* dir = str_of(v, "dir", "auto");
        const char* align = str_of(v, "align", "start");
        const char* lang = str_of(v, "lang", "");
        const char* text = str_of(v, "text", "");
        uint8_t r[96];
        uint32_t k;
        memset(&s, 0, sizeof s);
        if (real_of(b->c, v, "size", NULL, &size, e->name) || real_of(b->c, v, "width", "0", &width, e->name) ||
            real_of(b->c, v, "line_height", "0", &lh, e->name) || real_of(b->c, v, "color", "0", &color, e->name)) { rc = YPAK_ERR_FORMAT; break; }
        s.size = (float)size; s.width = (float)width; s.line_height = (float)lh; s.color = (float)color; s.lang = lang;
        s.dir = !strcmp(dir, "ltr") ? YLAY_DIR_LTR : !strcmp(dir, "rtl") ? YLAY_DIR_RTL : !strcmp(dir, "auto") ? YLAY_DIR_AUTO : -1;
        s.align = !strcmp(align, "start") ? YLAY_ALIGN_START : !strcmp(align, "end") ? YLAY_ALIGN_END : !strcmp(align, "center") ? YLAY_ALIGN_CENTER : -1;
        if (s.dir < 0 || s.align < 0) { rc = cfail(b->c, YPAK_ERR_FORMAT, "'%s' block '%s': dir is auto, ltr or rtl; align is start, end or center", e->name, bl[i].key); break; }
        if (ylay_layout(L, text, -1, &s, NULL, 0, &lb) < 0) { rc = cfail(b->c, YPAK_ERR_FORMAT, "'%s' block '%s': %s", e->name, bl[i].key, ylay_error(L)); break; }
        if (lb.n_missing) { rc = cfail(b->c, YPAK_ERR_FORMAT, "'%s' block '%s': %u characters no font maps; add a font that has them", e->name, bl[i].key, lb.n_missing); break; }
        memset(r, 0, sizeof r);
        w32(r, (uint32_t)pool.n); w32(r + 4, (uint32_t)strlen(bl[i].key)); sb_puts(&pool, bl[i].key); sb_put(&pool, "\0", 1);
        w32(r + 8, (uint32_t)pool.n); w32(r + 12, (uint32_t)strlen(text)); sb_puts(&pool, text); sb_put(&pool, "\0", 1);
        w32(r + 16, (uint32_t)pool.n); w32(r + 20, (uint32_t)strlen(lang)); sb_puts(&pool, lang); sb_put(&pool, "\0", 1);
        memcpy(r + 24, &s.size, 4); memcpy(r + 28, &s.width, 4); memcpy(r + 32, &s.line_height, 4); memcpy(r + 36, &s.color, 4);
        w32(r + 40, (uint32_t)s.dir); w32(r + 44, (uint32_t)s.align); w32(r + 48, (uint32_t)lb.dir); w32(r + 52, lb.n_missing);
        memcpy(r + 56, &lb.w, 4); memcpy(r + 60, &lb.h, 4);
        w32(r + 64, n_runs); w32(r + 68, lb.n_runs); w32(r + 72, n_items); w32(r + 76, lb.n_items); w32(r + 80, n_lines); w32(r + 84, lb.n_lines);
        sb_put(&brec, r, 96);
        for (k = 0; k < lb.n_runs; k++) {
            uint8_t q[16];
            const ylay_run* ru = &lb.runs[k];
            w32(q, (uint32_t)ru->font); memcpy(q + 4, &ru->size, 4); w32(q + 8, ru->first); w32(q + 12, ru->n);
            sb_put(&runs, q, 16);
        }
        for (k = 0; k < lb.n_items; k++) {
            const ygfx_citem* it = &lb.items[k];
            uint32_t g = (uint32_t)it->glyph, fi = 0, rr;
            uint8_t cl[4];
            sb_put(&items, it, 32);
            w32(cl, lb.cluster[k]);
            sb_put(&clus, cl, 4);
            for (rr = 0; rr < lb.n_runs; rr++)
                if (k >= lb.runs[rr].first && k < lb.runs[rr].first + lb.runs[rr].n) fi = (uint32_t)lb.runs[rr].font;
            if (fi < (uint32_t)nf) {
                E* f = fe[fi];
                int face = ff[fi];
                if (!f->used[face]) f->used[face] = (uint8_t*)calloc_c(b->c, (size_t)f->n_glyphs[face] / 8 + 1);
                if (f->used[face] && g < f->n_glyphs[face]) f->used[face][g >> 3] |= (uint8_t)(1u << (g & 7));
            }
        }
        for (k = 0; k < lb.n_lines; k++) {
            uint8_t q[32];
            const ylay_line* li = &lb.lines[k];
            memset(q, 0, 32);
            memcpy(q, &li->x, 4); memcpy(q + 4, &li->baseline, 4); memcpy(q + 8, &li->width, 4); memcpy(q + 12, &li->ascent, 4);
            memcpy(q + 16, &li->descent, 4); w32(q + 20, li->text_start); w32(q + 24, li->text_end);
            sb_put(&lines, q, 32);
        }
        n_runs += lb.n_runs;
        n_items += lb.n_items;
        n_lines += lb.n_lines;
    }
    ylay_stamp(L, NULL, stamp, sizeof stamp);
    ylay_block_free(&lb);
    ylay_destroy(L);
    if (rc == 0 && (pool.bad || runs.bad || items.bad || clus.bad || lines.bad || brec.bad)) rc = cfail(b->c, YPAK_ERR_NOMEM, "out of memory");
    if (rc == 0) {
#define A16(x) (((x) + 15u) & ~(uint64_t)15u)
        uint64_t fo = 112, bo = A16(fo + 16u * (uint64_t)nf), ro = A16(bo + 96u * (uint64_t)jn(blocks)), io = A16(ro + 16u * n_runs);
        uint64_t co = A16(io + 32u * (uint64_t)n_items), lo = A16(co + 4u * (uint64_t)n_items), po = A16(lo + 32u * (uint64_t)n_lines);
        uint64_t total = A16(po + pool.n);
        uint8_t* d = (uint8_t*)calloc_c(b->c, (size_t)total + 16);
        if (!d) rc = cfail(b->c, YPAK_ERR_NOMEM, "out of memory");
        else {
            memcpy(d, "YSPGRUN1", 8);
            w32(d + 8, 1); w32(d + 12, 112); w32(d + 16, (uint32_t)nf); w32(d + 20, (uint32_t)jn(blocks)); w32(d + 24, n_runs);
            w32(d + 28, n_items); w32(d + 32, n_lines);
            w64(d + 40, fo); w64(d + 48, bo); w64(d + 56, ro); w64(d + 64, io); w64(d + 72, co); w64(d + 80, lo); w64(d + 88, po);
            w64(d + 96, pool.n);
            memcpy(d + fo, brec.p, (size_t)16 * (size_t)nf);
            memcpy(d + bo, brec.p + 16 * nf, (size_t)96 * (size_t)jn(blocks));
            if (runs.n) memcpy(d + ro, runs.p, runs.n);
            if (items.n) memcpy(d + io, items.p, items.n);
            if (clus.n) memcpy(d + co, clus.p, clus.n);
            if (lines.n) memcpy(d + lo, lines.p, lines.n);
            memcpy(d + po, pool.p, pool.n);
            e->data = d;
            e->size = (int64_t)total;
        }
#undef A16
    }
    free(pool.p); free(runs.p); free(items.p); free(clus.p); free(lines.p); free(brec.p);
    if (rc) return rc;
    {
        /* the stamp without the font list: the fonts are this entry's 'from' */
        jv* bs = jnew(b->c, J_ARR);
        e->deriv = deriv_new(b, "layout", stamp);
        jset(b->c, e->deriv, "fonts", jcopy(b->c, fonts));
        for (i = 0; i < jn(blocks); i++) {
            const jv* v = bl[i].v;
            jv* o = jnew(b->c, J_OBJ);
            jset(b->c, o, "key", jstrz(b->c, bl[i].key));
            jset(b->c, o, "text", jstrz(b->c, str_of(v, "text", "")));
            jset(b->c, o, "size", jstrz(b->c, str_of(v, "size", "")));
            jset(b->c, o, "width", jstrz(b->c, str_of(v, "width", "0")));
            jset(b->c, o, "line_height", jstrz(b->c, str_of(v, "line_height", "0")));
            jset(b->c, o, "dir", jstrz(b->c, str_of(v, "dir", "auto")));
            jset(b->c, o, "align", jstrz(b->c, str_of(v, "align", "start")));
            jset(b->c, o, "lang", jstrz(b->c, str_of(v, "lang", "")));
            jset(b->c, o, "color", jstrz(b->c, str_of(v, "color", "0")));
            jpush(b->c, bs, o);
        }
        jset(b->c, e->deriv, "blocks", bs);
    }
    return 0;
}

/* --- the container ------------------------------------------------------------ */

typedef struct sink {
    FILE*          f;
    sb*            mem;
    const uint8_t* cmp;  int64_t cmp_n;   /* compare instead of write             */
    int64_t        pos, diff;             /* bytes so far; the first difference   */
    int            bad;
} sink;

static void sk_put(sink* s, const void* p, size_t n) {
    if (!n || s->bad) { s->pos += (int64_t)n; return; }
    if (s->f) {
        /* in pieces: one fwrite of 4.3 GB ran for minutes on MinGW's
         * msvcrt and wrote 5 MB (measured) */
        const uint8_t* q = (const uint8_t*)p;
        size_t left = n;
        while (left && !s->bad) {
            size_t k = left > ((size_t)16 << 20) ? ((size_t)16 << 20) : left;
            if (fwrite(q, 1, k, s->f) != k) s->bad = 1;
            q += k;
            left -= k;
        }
    } else if (s->mem) {
        sb_put(s->mem, p, n);
        if (s->mem->bad) s->bad = 1;
    } else if (s->cmp) {
        if (s->diff < 0) {
            int64_t k = s->cmp_n - s->pos < (int64_t)n ? s->cmp_n - s->pos : (int64_t)n;
            if (k < (int64_t)n) {
                /* past the end of the file compared with */
                int64_t j = k > 0 ? k : 0;
                if (k > 0 && memcmp(s->cmp + s->pos, p, (size_t)k) != 0) {
                    for (j = 0; j < k && s->cmp[s->pos + j] == ((const uint8_t*)p)[j]; j++) {}
                }
                s->diff = s->pos + j;
            } else if (memcmp(s->cmp + s->pos, p, n) != 0) {
                int64_t j;
                for (j = 0; s->cmp[s->pos + j] == ((const uint8_t*)p)[j]; j++) {}
                s->diff = s->pos + j;
            }
        }
    }
    s->pos += (int64_t)n;
}
static void sk_zeros(sink* s, int64_t n) {
    static const uint8_t z[4096] = { 0 };
    while (n > 0) { int64_t k = n > 4096 ? 4096 : n; sk_put(s, z, (size_t)k); n -= k; }
}

static int ent_cmp(const void* a, const void* b) {
    const E* x = *(const E* const*)a;
    const E* y = *(const E* const*)b;
    int c = memcmp(x->name, y->name, x->nl < y->nl ? x->nl : y->nl);
    if (c) return c;
    return x->nl < y->nl ? -1 : x->nl > y->nl;
}

static int hash_entry(C* c, E* e) {
    e->kc = ypak_chunk_count(e->size);
    e->ch = (uint64_t*)calloc_c(c, (size_t)e->kc * 8);
    if (!e->ch) return cfail(c, YPAK_ERR_NOMEM, "out of memory");
    e->hash = ypak_entry_hash(e->data, e->size, e->ch);
    e->crc = ypak_crc32(0, e->data, (size_t)e->size);
    ypak_sha256(e->data, (size_t)e->size, e->sha);
    return 0;
}

/* The chunk table of sorted entries (ysp/chunks excluded), as an entry. */
static E* make_chunks(C* c, E** ents, int n) {
    int64_t total = 0, k = 0;
    int i;
    uint8_t* d;
    E* ch;
    for (i = 0; i < n; i++)
        if (ents[i]->kc > 1) total += ents[i]->kc;
    d = (uint8_t*)calloc_c(c, (size_t)(32 + 8 * total));
    ch = (E*)calloc_c(c, sizeof(E));
    if (!d || !ch) { cfail(c, YPAK_ERR_NOMEM, "out of memory"); return NULL; }
    memcpy(d, "YSPCHNK1", 8);
    w32(d + 8, 1); w32(d + 12, YPAK_CHUNK_LOG2); w64(d + 16, (uint64_t)total);
    for (i = 0; i < n; i++) {
        E* e = ents[i];
        if (e->kc > 1) {
            int64_t j;
            e->chunk = (uint32_t)k;
            for (j = 0; j < e->kc; j++) w64(d + 32 + 8 * (k + j), e->ch[j]);
            k += e->kc;
        } else {
            e->chunk = YPAK_NO_CHUNK;
        }
    }
    ch->name = cdup(c, YPAK_CHUNKS, strlen(YPAK_CHUNKS));
    ch->nl = strlen(YPAK_CHUNKS);
    ch->kind = YPAK_KIND_CHUNKS;
    ch->data = d;
    ch->size = 32 + 8 * total;
    if (hash_entry(c, ch)) return NULL;
    ch->chunk = YPAK_NO_CHUNK;
    return ch;
}

/* Writes the pack: ents[0] is the manifest, the rest in name order with
 * ysp/chunks among them, every entry hashed and chunk-indexed. prefix: the
 * bytes before the zip start (already written by the caller); absolute:
 * offsets count from the file start. */
static int write_pack(C* c, sink* s, E** ents, int n, int64_t prefix, int absolute) {
    int64_t at = 0, base = absolute ? 0 : prefix, cd_start, cd_size;
    sb cd;
    int i;
    char cm[160];
    memset(&cd, 0, sizeof cd);
    for (i = 0; i < n; i++) {
        E* e = ents[i];
        uint8_t h[64 + YPAK_MAX_NAME];
        int z = e->size >= 0xFFFFFFFFLL;
        int64_t pad, hn;
        e->hrel = at;
        e->drel = (at + 30 + (int64_t)e->nl + (z ? 20 : 0) + 6 + YPAK_ALIGN - 1) & ~(int64_t)(YPAK_ALIGN - 1);
        pad = e->drel - (at + 30 + (int64_t)e->nl + (z ? 20 : 0) + 6);
        w32(h, 0x04034B50u); w16(h + 4, z ? 45 : 20); w16(h + 6, 0x0800u); w16(h + 8, 0); w16(h + 10, 0); w16(h + 12, 0x21);
        w32(h + 14, e->crc); w32(h + 18, z ? 0xFFFFFFFFu : (uint32_t)e->size); w32(h + 22, z ? 0xFFFFFFFFu : (uint32_t)e->size);
        w16(h + 26, (uint32_t)e->nl); w16(h + 28, (uint32_t)((z ? 20 : 0) + 6 + pad));
        memcpy(h + 30, e->name, e->nl);
        hn = 30 + (int64_t)e->nl;
        if (z) { w16(h + hn, 1); w16(h + hn + 2, 16); w64(h + hn + 4, (uint64_t)e->size); w64(h + hn + 12, (uint64_t)e->size); hn += 20; }
        w16(h + hn, 0xD935u); w16(h + hn + 2, (uint32_t)(2 + pad)); w16(h + hn + 4, YPAK_ALIGN);
        hn += 6;
        sk_put(s, h, (size_t)hn);
        sk_zeros(s, pad);
        sk_put(s, e->data, (size_t)e->size);
        at = e->drel + e->size;
    }
    cd_start = at;
    for (i = 0; i < n; i++) {
        E* e = ents[i];
        uint8_t r[46 + YPAK_MAX_NAME + 64];
        int64_t lho = e->hrel + prefix - base, rn = 46 + (int64_t)e->nl;
        int zs = e->size >= 0xFFFFFFFFLL, zo = lho >= 0xFFFFFFFFLL, z = zs || zo;
        w32(r, 0x02014B50u); w16(r + 4, z ? 0x032Du : 0x0314u); w16(r + 6, z ? 45 : 20); w16(r + 8, 0x0800u); w16(r + 10, 0);
        w16(r + 12, 0); w16(r + 14, 0x21); w32(r + 16, e->crc);
        w32(r + 20, zs ? 0xFFFFFFFFu : (uint32_t)e->size); w32(r + 24, zs ? 0xFFFFFFFFu : (uint32_t)e->size);
        w16(r + 28, (uint32_t)e->nl); w16(r + 32, 0); w16(r + 34, 0); w16(r + 36, 0); w32(r + 38, 0100644u << 16);
        w32(r + 42, zo ? 0xFFFFFFFFu : (uint32_t)lho);
        memcpy(r + 46, e->name, e->nl);
        if (z) {
            uint32_t want = (zs ? 16u : 0u) + (zo ? 8u : 0u);
            w16(r + rn, 1); w16(r + rn + 2, want); rn += 4;
            if (zs) { w64(r + rn, (uint64_t)e->size); w64(r + rn + 8, (uint64_t)e->size); rn += 16; }
            if (zo) { w64(r + rn, (uint64_t)lho); rn += 8; }
        }
        w16(r + rn, YPAK_EXT_ID); w16(r + rn + 2, YPAK_EXT_SIZE);
        r[rn + 4] = 1; r[rn + 5] = 0; w16(r + rn + 6, e->kind); w32(r + rn + 8, e->flags);
        w64(r + rn + 12, (uint64_t)e->drel); w64(r + rn + 20, e->hash); w32(r + rn + 28, e->chunk);
        rn += 32;
        w16(r + 30, (uint32_t)(rn - 46 - (int64_t)e->nl));
        sb_put(&cd, r, (size_t)rn);
    }
    if (cd.bad) { free(cd.p); return cfail(c, YPAK_ERR_NOMEM, "out of memory"); }
    cd_size = (int64_t)cd.n;
    sk_put(s, cd.p, cd.n);
    {
        uint8_t t[200];
        int64_t cd_off = cd_start + prefix - base;
        int need64 = n >= 0xFFFF || cd_off >= 0xFFFFFFFFLL;
        char sha[65];
        size_t k;
        if (need64) {
            int64_t rec = cd_start + cd_size + prefix - base;
            w32(t, 0x06064B50u); w64(t + 4, 44); w16(t + 12, 0x032D); w16(t + 14, 45); w32(t + 16, 0); w32(t + 20, 0);
            w64(t + 24, (uint64_t)n); w64(t + 32, (uint64_t)n); w64(t + 40, (uint64_t)cd_size); w64(t + 48, (uint64_t)cd_off);
            w32(t + 56, 0x07064B50u); w32(t + 60, 0); w64(t + 64, (uint64_t)rec); w32(t + 72, 1);
            sk_put(s, t, 76);
        }
        hex(ents[0]->sha, 32, sha);
        k = (size_t)snprintf(cm, sizeof cm, "ysp-pack/%d manifest=sha256:%s cd=xxh64:%016llx", YPAK_FORMAT, sha,
                             (unsigned long long)ypak_xxh64(cd.p, cd.n, 0));
        w32(t, 0x06054B50u); w16(t + 4, 0); w16(t + 6, 0);
        w16(t + 8, n >= 0xFFFF ? 0xFFFFu : (uint32_t)n); w16(t + 10, n >= 0xFFFF ? 0xFFFFu : (uint32_t)n);
        w32(t + 12, (uint32_t)cd_size); w32(t + 16, cd_off >= 0xFFFFFFFFLL ? 0xFFFFFFFFu : (uint32_t)cd_off);
        w16(t + 20, (uint32_t)k);
        memcpy(t + 22, cm, k);
        sk_put(s, t, 22 + k);
    }
    free(cd.p);
    if (s->bad) return cfail(c, YPAK_ERR_IO, "cannot write the pack");
    return 0;
}

/* --- build ------------------------------------------------------------------- */

static void lib_versions(C* c, jv* tool) {
    jv* l = jnew(c, J_OBJ);
    const ylay_versions* v = ylay_get_versions();
    char line[1024];
    jset(c, tool, "name", jstrz(c, "ypak"));
    jset(c, tool, "version", jstrz(c, YPT_VERSION_STRING));
    jset(c, l, "ysp/pack.h", jstrz(c, YPAK_VERSION_STRING));
    jset(c, l, "ysp/table.h", jstrz(c, YTB_VERSION_STRING));
    jset(c, l, "ysp/json.h", jstrz(c, YJS_VERSION_STRING));
    jset(c, l, "ysp/outline.h", jstrz(c, YOL_VERSION_STRING));
    jset(c, l, "ysp/gfx.h", jstrz(c, YGFX_VERSION_STRING));
    jset(c, l, "ysp/color.h", jstrz(c, YCOL_VERSION_STRING));
    jset(c, l, "ysp/audio.h", jstrz(c, YAU_VERSION_STRING));
    snprintf(line, sizeof line, "ylay %s skribidi %s %s harfbuzz %s sheenbidi %s libunibreak %s budouxc %s", v->ylay, v->skribidi,
             v->skribidi_patch, v->harfbuzz, v->sheenbidi, v->libunibreak, v->budouxc);
    jset(c, l, "layout", jstrz(c, line));
    jset(c, l, "lodepng", jstrz(c, YPT_LODEPNG));
    jset(c, tool, "libraries", l);
}

static jv* entry_obj(C* c, const E* e) {
    jv* o = jnew(c, J_OBJ);
    char hx[65];
    jset(c, o, "name", jstr(c, e->name, e->nl));
    jset(c, o, "kind", jstrz(c, ypak_kind_name(e->kind)));
    jset(c, o, "size", jint(c, e->size));
    hex(e->sha, 32, hx);
    jset(c, o, "sha256", jstrz(c, hx));
    snprintf(hx, sizeof hx, "%016llx", (unsigned long long)e->hash);
    jset(c, o, "xxh64", jstrz(c, hx));
    snprintf(hx, sizeof hx, "%08x", e->crc);
    jset(c, o, "crc32", jstrz(c, hx));
    if (e->kind != YPAK_KIND_CHUNKS) {
        jset(c, o, "sources", e->sources ? e->sources : jnew(c, J_ARR));
        if (e->from) jset(c, o, "from", e->from);
        if (e->res && str_of(e->res, "license", NULL)) jset(c, o, "license", jstrz(c, str_of(e->res, "license", "")));
        if (e->flags & YPAK_F_STREAM) jset(c, o, "stream", jbool(c, 1));
    }
    jset(c, o, "derivation", e->deriv ? e->deriv : jnew(c, J_OBJ));
    return o;
}

typedef int (*kind_fn)(B*, E*);

static int build_desc(C* c, jv* desc, const char* base_dir, sink* out) {
    static const char* const tkeys[] = { "format", "version", "experiment", "audio", "private", "resources", NULL };
    static const char* const ekeys[] = { "entry", "source", "media_type", NULL };
    static const char* const akeys[] = { "rate", "channels", "map", NULL };
    B b;
    jv* res;
    jv* ex;
    jv* au;
    jv* manifest;
    jv* earr;
    int i, pass, rc = 0;
    E* man;
    E* chunks;
    E** all;
    sb mt;
    memset(&b, 0, sizeof b);
    b.c = c;
    b.desc = desc;
    snprintf(b.src_dir, sizeof b.src_dir, "%s", base_dir ? base_dir : "");
    if (keys_ok(c, desc, tkeys, "the source description")) return YPAK_ERR_FORMAT;
    if (strcmp(str_of(desc, "format", ""), "ysp-pack-source") != 0 || !jget(desc, "version") || jget(desc, "version")->type != J_INT ||
        jget(desc, "version")->i != 1)
        return cfail(c, YPAK_ERR_FORMAT, "the source description needs \"format\": \"ysp-pack-source\" and \"version\": 1");
    res = jget(desc, "resources");
    if (!res || res->type != J_ARR) return cfail(c, YPAK_ERR_FORMAT, "the source description needs a 'resources' array");
    b.priv = jget(desc, "private") && jget(desc, "private")->type == J_TRUE;
    au = jget(desc, "audio");
    if (au) {
        jv* r = jget(au, "rate");
        jv* ch = jget(au, "channels");
        if (keys_ok(c, au, akeys, "audio") || !r || r->type != J_INT || !ch || ch->type != J_INT || r->i < 8000 || r->i > 768000 ||
            ch->i < 1 || ch->i > 64)
            return cfail(c, YPAK_ERR_FORMAT, "audio: needs integer 'rate' (8000 to 768000) and 'channels' (1 to 64)");
        b.rate = (int)r->i;
        b.channels = (int)ch->i;
        b.audio = au;
    }
    if (!yol_open(&b.ol, NULL)) return cfail(c, YPAK_ERR_NOMEM, "yol_open failed");
    b.ol_ok = 1;
    ex = jget(desc, "experiment");
    if (ex) {
        E* e;
        if (keys_ok(c, ex, ekeys, "experiment") || !str_of(ex, "entry", NULL) || !str_of(ex, "source", NULL)) {
            rc = cfail(c, YPAK_ERR_FORMAT, "experiment: needs 'entry' and 'source'");
            goto done;
        }
        e = add_entry(&b);
        e->res = ex;
        e->kind = YPAK_KIND_EXPERIMENT;
        e->name = cdup(c, str_of(ex, "entry", ""), strlen(str_of(ex, "entry", "")));
        e->nl = strlen(e->name);
    }
    for (i = 0; i < jn(res); i++) {
        jv* r = jat(res, i);
        const char* kind = str_of(r, "kind", NULL);
        const char* name = str_of(r, "name", NULL);
        E* e;
        uint32_t k = kind ? ypak_kind_id(kind) : 0;
        if (r->type != J_OBJ || !k || k == YPAK_KIND_MANIFEST || k == YPAK_KIND_CHUNKS || k == YPAK_KIND_EXPERIMENT || k >= YPAK_KIND_VIDEO) {
            rc = cfail(c, YPAK_ERR_FORMAT, "resource %d: kind '%s' is not one this tool builds (file, table, calibration, font, curveset, "
                       "glyphruns, artwork, audio, shader, texture; the experiment is the top-level \"experiment\")", i, kind ? kind : "");
            goto done;
        }
        e = add_entry(&b);
        e->res = r;
        e->kind = k;
        if (name) e->name = cdup(c, name, strlen(name));
        else if (str_of(r, "source", NULL)) e->name = default_name(&b, str_of(r, "source", ""), k);
        else if (k == YPAK_KIND_CURVESET && str_of(r, "from", NULL)) {
            char t[1100];
            jv* fv = jget(r, "face");
            const char* f = str_of(r, "from", "");
            const char* dot = strrchr(f, '.');
            size_t stem = dot ? (size_t)(dot - f) : strlen(f);
            if (fv && fv->type == J_INT && fv->i) snprintf(t, sizeof t, "%.*s-%d.yspcset", (int)stem, f, (int)fv->i);
            else snprintf(t, sizeof t, "%.*s.yspcset", (int)stem, f);
            e->name = cdup(c, t, strlen(t));
        } else {
            rc = cfail(c, YPAK_ERR_FORMAT, "resource %d (%s): needs a 'name'", i, kind);
            goto done;
        }
        e->nl = strlen(e->name);
    }
    /* names: the writer's rule, reserved, unique without regard to case */
    for (i = 0; i < b.n; i++) {
        int k;
        E* e = b.e[i];
        if (!ypt_name_ok(e->name, e->nl)) {
            rc = cfail(c, YPAK_ERR_FORMAT, "'%s': not a portable entry name (A-Z a-z 0-9 . _ - and /; no component starting or "
                       "ending with '.'; no device names)", e->name);
            goto done;
        }
        if (e->nl >= 4 && !memcmp(e->name, "ysp/", 4)) { rc = cfail(c, YPAK_ERR_FORMAT, "'%s': names under ysp/ are reserved", e->name); goto done; }
        for (k = 0; k < i; k++)
            if (!name_icmp(e->name, e->nl, b.e[k]->name, b.e[k]->nl)) {
                rc = cfail(c, YPAK_ERR_FORMAT, "'%s' and '%s': the same name without regard to case", b.e[k]->name, e->name);
                goto done;
            }
    }
    /* build in dependency order: fonts before glyph runs before curve sets */
    for (pass = 0; pass < 3 && rc == 0; pass++) {
        for (i = 0; i < b.n && rc == 0; i++) {
            E* e = b.e[i];
            uint32_t k = e->kind;
            if ((pass == 0) != (k != YPAK_KIND_GLYPHRUNS && k != YPAK_KIND_CURVESET)) continue;
            if (pass == 1 && k != YPAK_KIND_GLYPHRUNS) continue;
            if (pass == 2 && k != YPAK_KIND_CURVESET) continue;
            e->sources = jnew(c, J_ARR);
            switch (k) {
            case YPAK_KIND_EXPERIMENT: rc = build_copy(&b, e, "ypak " YPT_VERSION_STRING); break;
            case YPAK_KIND_FILE: {
                static const char* const keys[] = { "name", "kind", "source", "license", NULL };
                rc = keys_ok(c, e->res, keys, e->name);
                if (!rc) rc = build_copy(&b, e, "ypak " YPT_VERSION_STRING);
            } break;
            case YPAK_KIND_TABLE: rc = build_table(&b, e); break;
            case YPAK_KIND_CALIBRATION: rc = build_calibration(&b, e); break;
            case YPAK_KIND_FONT: rc = build_font(&b, e); break;
            case YPAK_KIND_CURVESET: rc = build_curveset(&b, e); break;
            case YPAK_KIND_GLYPHRUNS: rc = build_glyphruns(&b, e); break;
            case YPAK_KIND_ARTWORK: rc = build_artwork(&b, e); break;
            case YPAK_KIND_AUDIO: rc = build_audio(&b, e); break;
            case YPAK_KIND_SHADER: rc = build_shader(&b, e); break;
            case YPAK_KIND_TEXTURE: rc = build_texture(&b, e); break;
            default: rc = cfail(c, YPAK_ERR_FORMAT, "'%s': kind not built", e->name); break;
            }
            if (rc == 0) rc = hash_entry(c, e);
            if (rc == 0 && c->log) fprintf(c->log, "  %-11s %12lld  %s\n", ypak_kind_name(e->kind), (long long)e->size, e->name);
        }
    }
    if (rc) goto done;
    /* order, chunks, manifest */
    qsort(b.e, (size_t)b.n, sizeof(E*), ent_cmp);
    chunks = make_chunks(c, b.e, b.n);
    if (!chunks) { rc = YPAK_ERR_NOMEM; goto done; }
    all = (E**)calloc_c(c, sizeof(E*) * ((size_t)b.n + 2));
    man = (E*)calloc_c(c, sizeof(E));
    if (!all || !man) { rc = cfail(c, YPAK_ERR_NOMEM, "out of memory"); goto done; }
    all[0] = man;
    for (i = 0; i < b.n; i++) all[i + 1] = b.e[i];
    all[b.n + 1] = chunks;
    qsort(all + 1, (size_t)b.n + 1, sizeof(E*), ent_cmp);
    chunks->deriv = deriv_new(&b, "chunks", "ysp/pack.h " YPAK_VERSION_STRING);
    manifest = jnew(c, J_OBJ);
    jset(c, manifest, "format", jstrz(c, "ysp-pack"));
    jset(c, manifest, "version", jint(c, YPAK_FORMAT));
    {
        jv* tool = jnew(c, J_OBJ);
        lib_versions(c, tool);
        jset(c, manifest, "tool", tool);
    }
    if (b.audio) jset(c, manifest, "audio", jcopy(c, b.audio));
    if (b.priv) jset(c, manifest, "private", jbool(c, 1));
    if (ex) {
        jv* o = jnew(c, J_OBJ);
        jset(c, o, "entry", jstrz(c, str_of(ex, "entry", "")));
        if (str_of(ex, "media_type", NULL)) jset(c, o, "media_type", jstrz(c, str_of(ex, "media_type", "")));
        jset(c, manifest, "experiment", o);
    }
    earr = jnew(c, J_ARR);
    for (i = 1; i < b.n + 2; i++) jpush(c, earr, entry_obj(c, all[i]));
    jset(c, manifest, "entries", earr);
    memset(&mt, 0, sizeof mt);
    jw(c, &mt, manifest);
    sb_put(&mt, "\n", 1);
    if (mt.bad) { free(mt.p); rc = cfail(c, YPAK_ERR_NOMEM, "out of memory"); goto done; }
    man->name = cdup(c, YPAK_MANIFEST, strlen(YPAK_MANIFEST));
    man->nl = strlen(YPAK_MANIFEST);
    man->kind = YPAK_KIND_MANIFEST;
    man->data = (const uint8_t*)cdup(c, mt.p, mt.n);
    man->size = (int64_t)mt.n;
    free(mt.p);
    rc = hash_entry(c, man);
    man->chunk = YPAK_NO_CHUNK;
    if (rc == 0) rc = write_pack(c, out, all, b.n + 2, 0, 0);
done:
    if (b.ol_ok) yol_close(&b.ol);
    return rc;
}

static void dir_of(const char* path, char* out, size_t cap) {
    const char* s1 = strrchr(path, '/');
    const char* s2 = strrchr(path, '\\');
    const char* s = s1 > s2 ? s1 : s2;
    if (!s) { snprintf(out, cap, "."); return; }
    snprintf(out, cap, "%.*s", (int)(s - path), path);
}

static int finish_file(C* c, sink* s, const char* tmp, const char* out_path, int rc) {
    if (s->f) {
        if (fclose(s->f) != 0 && rc == 0) rc = cfail(c, YPAK_ERR_IO, "cannot write '%s'", tmp);
        s->f = NULL;
    }
    if (rc == 0 && urename(tmp, out_path) != 0) rc = cfail(c, YPAK_ERR_IO, "cannot rename '%s' to '%s'", tmp, out_path);
    if (rc) uremove(tmp);
    return rc;
}

int ypt_build_text(const char* json, size_t n, const char* base_dir, const char* out_path, const ypt_options* o, char* err, size_t cap) {
    C c;
    jv* desc;
    sink s;
    char tmp[2100];
    int rc;
    memset(&c, 0, sizeof c);
    c.err = err;
    c.cap = cap;
    if (err && cap) err[0] = 0;
    c.log = o ? o->log : NULL;
    desc = jparse(&c, json, n, "the source description");
    if (!desc) { cfree_all(&c); return YPAK_ERR_FORMAT; }
    memset(&s, 0, sizeof s);
    s.diff = -1;
    snprintf(tmp, sizeof tmp, "%s.tmp", out_path);
    s.f = ufopen(tmp, "wb");
    if (!s.f) { rc = cfail(&c, YPAK_ERR_IO, "cannot create '%s'", tmp); cfree_all(&c); return rc; }
    rc = build_desc(&c, desc, o && o->sources ? o->sources : base_dir, &s);
    rc = finish_file(&c, &s, tmp, out_path, rc);
    cfree_all(&c);
    return rc;
}

int ypt_build(const char* desc_path, const char* out_path, const ypt_options* o, char* err, size_t cap) {
    C c;
    int64_t n;
    uint8_t* t;
    char dir[2048];
    int rc;
    memset(&c, 0, sizeof c);
    c.err = err;
    c.cap = cap;
    if (err && cap) err[0] = 0;
    t = read_file(&c, desc_path, &n);
    if (!t) { cfree_all(&c); return YPAK_ERR_IO; }
    dir_of(desc_path, dir, sizeof dir);
    rc = ypt_build_text((const char*)t, (size_t)n, dir, out_path, o, err, cap);
    cfree_all(&c);
    return rc;
}

/* --- reading packs ------------------------------------------------------------- */

static int open_pack(C* c, ypak_pack* p, const char* path, ypak_verify v) {
    ypak_desc d;
    memset(&d, 0, sizeof d);
    d.path = path;
    d.verify = v;
    if (!ypak_open(p, &d)) return cfail(c, p->error_code, "%s: %s", path, ypak_error(p));
    return 0;
}

/* The manifest of an open pack, parsed. */
static jv* pack_manifest(C* c, ypak_pack* p) {
    ypak_entry e;
    const void* d;
    if (ypak_at(p, 0, &e)) { cfail(c, YPAK_ERR_FORMAT, "no manifest"); return NULL; }
    d = ypak_data(p, &e);
    if (!d) { cfail(c, YPAK_ERR_CORRUPT, "%s", ypak_error(p)); return NULL; }
    return jparse(c, (const char*)d, (size_t)e.size, "the manifest");
}

/* Entries of an open pack as E records, in pack order (entry 0 the
 * manifest), for writing the pack again. */
static E** pack_entries(C* c, ypak_pack* p) {
    E** all = (E**)calloc_c(c, sizeof(E*) * p->n);
    uint32_t i;
    if (!all) return NULL;
    for (i = 0; i < p->n; i++) {
        ypak_entry e;
        E* x = (E*)calloc_c(c, sizeof(E));
        if (!x) return NULL;
        ypak_at(p, i, &e);
        x->name = cdup(c, e.name, e.name_len);
        x->nl = e.name_len;
        x->kind = e.kind;
        x->flags = e.flags;
        x->data = (const uint8_t*)ypak_data(p, &e);
        x->size = e.size;
        if (!x->data && e.size) { cfail(c, YPAK_ERR_CORRUPT, "%s", ypak_error(p)); return NULL; }
        if (hash_entry(c, x)) return NULL;
        all[i] = x;
    }
    return all;
}

/* The chunk indices of all (pack order), with ysp/chunks skipped, as
 * make_chunks assigns them. */
static void assign_chunks(E** all, int n) {
    int i;
    uint32_t k = 0;
    for (i = 0; i < n; i++) {
        E* e = all[i];
        if (i > 0 && e->kind != YPAK_KIND_CHUNKS && e->kc > 1) { e->chunk = k; k += (uint32_t)e->kc; }
        else e->chunk = YPAK_NO_CHUNK;
    }
}

static const char* jhex_of(const jv* o, const char* key) { return str_of(o, key, ""); }

int ypt_verify(const char* pack_path, FILE* report, char* err, size_t cap) {
    C c;
    ypak_pack p;
    jv* m;
    jv* ents;
    E** all;
    int rc, i, found = 0;
    uint32_t k;
    memset(&c, 0, sizeof c);
    c.err = err;
    c.cap = cap;
    if (err && cap) err[0] = 0;
    rc = open_pack(&c, &p, pack_path, YPAK_VERIFY_OPEN);
    if (rc) { cfree_all(&c); return rc; }
    if (report) fprintf(report, "structure and every XXH64: ok (%u entries, %lld bytes hashed)\n", p.n, (long long)p.hashed_bytes);
    all = pack_entries(&c, &p);
    if (!all) { rc = YPAK_ERR_CORRUPT; goto out; }
    for (k = 0; k < p.n; k++) {
        ypak_entry e;
        ypak_at(&p, k, &e);
        if (all[k]->crc != e.crc32) { rc = cfail(&c, YPAK_ERR_CORRUPT, "'%.*s': CRC-32 %08x, the record says %08x", (int)e.name_len, e.name, all[k]->crc, e.crc32); goto out; }
    }
    if (report) fprintf(report, "CRC-32 of every entry: ok\n");
    m = pack_manifest(&c, &p);
    if (!m) { rc = YPAK_ERR_FORMAT; goto out; }
    if (strcmp(str_of(m, "format", ""), "ysp-pack") || !jget(m, "version") || jget(m, "version")->i != YPAK_FORMAT) {
        rc = cfail(&c, YPAK_ERR_FORMAT, "the manifest's format or version is not ysp-pack 1");
        goto out;
    }
    ents = jget(m, "entries");
    if (!ents || ents->type != J_ARR || (uint32_t)jn(ents) != p.n - 1) { rc = cfail(&c, YPAK_ERR_FORMAT, "the manifest lists %d entries; the pack has %u besides it", ents ? jn(ents) : 0, p.n - 1); goto out; }
    for (i = 0; i < jn(ents); i++) {
        const jv* o = jat(ents, i);
        const E* x = all[i + 1];
        char sha[65], xx[17], cr[9];
        hex(x->sha, 32, sha);
        snprintf(xx, sizeof xx, "%016llx", (unsigned long long)x->hash);
        snprintf(cr, sizeof cr, "%08x", x->crc);
        if (strlen(str_of(o, "name", "")) != x->nl || memcmp(str_of(o, "name", ""), x->name, x->nl) ||
            ypak_kind_id(str_of(o, "kind", "")) != x->kind || !jget(o, "size") || jget(o, "size")->i != x->size ||
            strcmp(jhex_of(o, "sha256"), sha) || strcmp(jhex_of(o, "xxh64"), xx) || strcmp(jhex_of(o, "crc32"), cr) ||
            ((jget(o, "stream") && jget(o, "stream")->type == J_TRUE) != ((x->flags & YPAK_F_STREAM) != 0))) {
            rc = cfail(&c, YPAK_ERR_CORRUPT, "manifest entry %d ('%s') does not match the pack's entry '%.*s'", i, str_of(o, "name", ""), (int)x->nl, x->name);
            goto out;
        }
        if (x->kind == YPAK_KIND_EXPERIMENT) found = 1;
    }
    if (found != (jget(m, "experiment") != NULL) || (found && strcmp(str_of(jget(m, "experiment"), "entry", ""), all[p.experiment]->name))) {
        rc = cfail(&c, YPAK_ERR_FORMAT, "the manifest's experiment does not name the pack's experiment entry");
        goto out;
    }
    if (report) fprintf(report, "SHA-256 of every entry against the manifest: ok\n");
    {
        /* the canonical container, from the entries */
        sink s;
        assign_chunks(all, (int)p.n);
        memset(&s, 0, sizeof s);
        s.diff = -1;
        s.cmp = p.base_ + p.zip_start;
        s.cmp_n = p.file_size - p.zip_start;
        rc = write_pack(&c, &s, all, (int)p.n, p.zip_start, p.base == 0 && p.zip_start > 0);
        if (rc == 0 && (s.diff >= 0 || s.pos != s.cmp_n)) {
            rc = cfail(&c, YPAK_ERR_FORMAT, "the container is not the canonical one: the first difference is at byte %lld",
                       (long long)(s.diff >= 0 ? s.diff + p.zip_start : (s.pos < s.cmp_n ? s.pos : s.cmp_n) + p.zip_start));
            goto out;
        }
        if (rc) goto out;
    }
    if (report) {
        char id[72];
        ypak_id(&p, id);
        fprintf(report, "canonical container: ok\npack ID %s\nverify: ok\n", id);
    }
out:
    ypak_close(&p);
    cfree_all(&c);
    return rc;
}

int ypt_list(const char* pack_path, FILE* out, char* err, size_t cap) {
    C c;
    ypak_pack p;
    uint32_t i;
    int rc;
    memset(&c, 0, sizeof c);
    c.err = err;
    c.cap = cap;
    if (err && cap) err[0] = 0;
    rc = open_pack(&c, &p, pack_path, YPAK_VERIFY_NONE);
    if (rc) return rc;
    for (i = 0; i < p.n; i++) {
        ypak_entry e;
        ypak_at(&p, i, &e);
        if (out) fprintf(out, "%-11s %14lld %14lld %s %.*s\n", ypak_kind_name(e.kind), (long long)e.size, (long long)e.data_off,
                         (e.flags & YPAK_F_STREAM) ? "stream" : "-     ", (int)e.name_len, e.name);
    }
    ypak_close(&p);
    return 0;
}

int ypt_info(const char* pack_path, FILE* out, char* err, size_t cap) {
    C c;
    ypak_pack p;
    jv* m;
    int rc;
    char line[512];
    memset(&c, 0, sizeof c);
    c.err = err;
    c.cap = cap;
    if (err && cap) err[0] = 0;
    rc = open_pack(&c, &p, pack_path, YPAK_VERIFY_USE);
    if (rc) return rc;
    m = pack_manifest(&c, &p);
    if (!m) { ypak_close(&p); cfree_all(&c); return YPAK_ERR_FORMAT; }
    ypak_describe(&p, line, sizeof line);
    if (out) {
        jv* t = jget(m, "tool");
        jv* a = jget(m, "audio");
        jv* x = jget(m, "experiment");
        fprintf(out, "%s\n", line);
        fprintf(out, "tool: %s %s\n", str_of(t, "name", "?"), str_of(t, "version", "?"));
        if (x) fprintf(out, "experiment: %s (%s)\n", str_of(x, "entry", ""), str_of(x, "media_type", "no media type"));
        else fprintf(out, "experiment: none (a pack of resources)\n");
        if (a) fprintf(out, "audio: %lld Hz, %lld channels\n", (long long)(jget(a, "rate") ? jget(a, "rate")->i : 0),
                       (long long)(jget(a, "channels") ? jget(a, "channels")->i : 0));
        if (jget(m, "private")) fprintf(out, "private: true (fonts restricted from embedding may be inside; do not share)\n");
    }
    ypak_close(&p);
    cfree_all(&c);
    return 0;
}

int ypt_cat(const char* pack_path, const char* name, FILE* out, char* err, size_t cap) {
    C c;
    ypak_pack p;
    ypak_entry e;
    const void* d;
    int rc;
    memset(&c, 0, sizeof c);
    c.err = err;
    c.cap = cap;
    if (err && cap) err[0] = 0;
    rc = open_pack(&c, &p, pack_path, YPAK_VERIFY_USE);
    if (rc) return rc;
    if (ypak_find(&p, name, &e)) { rc = cfail(&c, YPAK_ERR_NOT_FOUND, "%s", ypak_error(&p)); ypak_close(&p); return rc; }
    d = ypak_data(&p, &e);
    if (!d && e.size) { rc = cfail(&c, YPAK_ERR_CORRUPT, "%s", ypak_error(&p)); ypak_close(&p); return rc; }
    if (e.size && fwrite(d, 1, (size_t)e.size, out) != (size_t)e.size) rc = cfail(&c, YPAK_ERR_IO, "cannot write");
    ypak_close(&p);
    return rc;
}

int ypt_extract(const char* pack_path, const char* const* names, int n, const char* dir, int force, FILE* report, char* err, size_t cap) {
    C c;
    ypak_pack p;
    uint32_t i;
    int rc;
    memset(&c, 0, sizeof c);
    c.err = err;
    c.cap = cap;
    if (err && cap) err[0] = 0;
    rc = open_pack(&c, &p, pack_path, YPAK_VERIFY_USE);
    if (rc) return rc;
    for (i = 0; rc == 0 && i < p.n; i++) {
        ypak_entry e;
        char path[2400];
        const void* d;
        FILE* f;
        size_t k;
        int want = n == 0, j;
        ypak_at(&p, i, &e);
        for (j = 0; j < n; j++) if (strlen(names[j]) == e.name_len && !memcmp(names[j], e.name, e.name_len)) want = 1;
        if (!want) continue;
        if (!ypt_name_ok(e.name, e.name_len) && !(e.name_len >= 4 && !memcmp(e.name, "ysp/", 4))) {
            rc = cfail(&c, YPAK_ERR_FORMAT, "'%.*s': not a portable file name; use ypak cat", (int)e.name_len, e.name);
            break;
        }
        snprintf(path, sizeof path, "%s/%.*s", dir, (int)e.name_len, e.name);
        /* the folders on the way */
        for (k = strlen(dir) + 1; path[k]; k++)
            if (path[k] == '/') { path[k] = 0; umkdir(path); path[k] = '/'; }
        if (!force && uexists(path)) { rc = cfail(&c, YPAK_ERR_IO, "'%s' exists; use --force to replace it", path); break; }
        d = ypak_data(&p, &e);
        if (!d && e.size) { rc = cfail(&c, YPAK_ERR_CORRUPT, "%s", ypak_error(&p)); break; }
        f = ufopen(path, "wb");
        if (!f) { rc = cfail(&c, YPAK_ERR_IO, "cannot create '%s'", path); break; }
        if (e.size && fwrite(d, 1, (size_t)e.size, f) != (size_t)e.size) rc = cfail(&c, YPAK_ERR_IO, "cannot write '%s'", path);
        if (fclose(f) != 0 && rc == 0) rc = cfail(&c, YPAK_ERR_IO, "cannot write '%s'", path);
        if (rc == 0 && report) fprintf(report, "%s\n", path);
    }
    ypak_close(&p);
    return rc;
}

/* The manifest back into a source description. */
static jv* manifest_to_desc(C* c, const jv* m) {
    jv* d = jnew(c, J_OBJ);
    jv* res = jnew(c, J_ARR);
    jv* ents = jget(m, "entries");
    jv* ex = jget(m, "experiment");
    int i;
    jset(c, d, "format", jstrz(c, "ysp-pack-source"));
    jset(c, d, "version", jint(c, 1));
    if (jget(m, "audio")) jset(c, d, "audio", jcopy(c, jget(m, "audio")));
    if (jget(m, "private")) jset(c, d, "private", jbool(c, 1));
    for (i = 0; ents && i < jn(ents); i++) {
        const jv* o = jat(ents, i);
        const char* kind = str_of(o, "kind", "");
        jv* src = jget(o, "sources");
        jv* dv = jget(o, "derivation");
        jv* r;
        int k;
        if (!strcmp(kind, "chunks")) continue;
        if (!strcmp(kind, "experiment")) {
            jv* x = jnew(c, J_OBJ);
            jset(c, x, "entry", jstrz(c, str_of(o, "name", "")));
            if (src && jn(src)) jset(c, x, "source", jstrz(c, str_of(jat(src, 0), "path", "")));
            if (ex && str_of(ex, "media_type", NULL)) jset(c, x, "media_type", jstrz(c, str_of(ex, "media_type", "")));
            jset(c, d, "experiment", x);
            continue;
        }
        r = jnew(c, J_OBJ);
        jset(c, r, "name", jstrz(c, str_of(o, "name", "")));
        jset(c, r, "kind", jstrz(c, kind));
        if (src && jn(src)) jset(c, r, "source", jstrz(c, str_of(jat(src, 0), "path", "")));
        if (!strcmp(kind, "curveset") && jget(o, "from") && jn(jget(o, "from"))) jset(c, r, "from", jcopy(c, jat(jget(o, "from"), 0)));
        if (str_of(o, "license", NULL)) jset(c, r, "license", jstrz(c, str_of(o, "license", "")));
        for (k = 0; dv && k < jn(dv); k++) {
            const char* key = jkey(dv, k);
            if (!strcmp(key, "op") || !strcmp(key, "by")) continue;
            /* what the tool records but does not take back */
            if ((!strcmp(kind, "font") && (!strcmp(key, "faces") || !strcmp(key, "fs_type"))) ||
                (!strcmp(kind, "calibration") && !strcmp(key, "role")) ||
                (!strcmp(kind, "curveset") && (!strcmp(key, "resolve") || !strcmp(key, "backward"))) ||
                (!strcmp(kind, "shader") && (!strcmp(key, "contract") || !strcmp(key, "validated"))) ||
                (!strcmp(kind, "audio") && !strcmp(key, "convert")))
                continue;
            jset(c, r, key, jcopy(c, jat(dv, k)));
        }
        jpush(c, res, r);
    }
    jset(c, d, "resources", res);
    return d;
}

int ypt_rebuild(const char* pack_path, const char* sources_dir, const char* out_path, FILE* report, char* err, size_t cap) {
    C c;
    ypak_pack p, q;
    jv* m;
    jv* desc;
    sb text;
    char tmp[2100];
    int rc;
    memset(&c, 0, sizeof c);
    c.err = err;
    c.cap = cap;
    if (err && cap) err[0] = 0;
    rc = open_pack(&c, &p, pack_path, YPAK_VERIFY_USE);
    if (rc) { cfree_all(&c); return rc; }
    if (p.zip_start != 0) {
        ypak_close(&p);
        rc = cfail(&c, YPAK_ERR_ARG, "'%s' is appended to a program; rebuild the pack it was made from", pack_path);
        cfree_all(&c);
        return rc;
    }
    m = pack_manifest(&c, &p);
    if (!m) { ypak_close(&p); cfree_all(&c); return YPAK_ERR_FORMAT; }
    {
        const char* v = str_of(jget(m, "tool"), "version", "");
        if (strcmp(v, YPT_VERSION_STRING) && report)
            fprintf(report, "note: the pack was built by ypak %s; this is ypak %s, so differences can come from the tool\n", v, YPT_VERSION_STRING);
    }
    /* every source against the manifest's SHA-256 first: a changed source is
     * named, not found later as a difference in some entry */
    {
        jv* ents = jget(m, "entries");
        int i, k;
        for (i = 0; ents && i < jn(ents); i++) {
            jv* src = jget(jat(ents, i), "sources");
            for (k = 0; src && k < jn(src); k++) {
                const char* rel = str_of(jat(src, k), "path", "");
                char path[2048], hx[65];
                uint8_t sha[32];
                int64_t n;
                uint8_t* d;
                snprintf(path, sizeof path, "%s/%s", sources_dir, rel);
                d = read_file(&c, path, &n);
                if (!d) { ypak_close(&p); rc = c.err && c.err[0] ? YPAK_ERR_IO : cfail(&c, YPAK_ERR_IO, "cannot read '%s'", path); cfree_all(&c); return rc; }
                ypak_sha256(d, (size_t)n, sha);
                hex(sha, 32, hx);
                if (strcmp(hx, str_of(jat(src, k), "sha256", ""))) {
                    ypak_close(&p);
                    rc = cfail(&c, YPAK_ERR_CORRUPT, "source '%s' of '%s' is not the one the pack was built from (SHA-256 %s, the "
                               "manifest says %s)", rel, str_of(jat(ents, i), "name", ""), hx, str_of(jat(src, k), "sha256", ""));
                    if (report) fprintf(report, "%s\n", err);
                    cfree_all(&c);
                    return rc;
                }
            }
        }
        if (report) fprintf(report, "every source matches the manifest's SHA-256\n");
    }
    desc = manifest_to_desc(&c, m);
    memset(&text, 0, sizeof text);
    jw(&c, &text, desc);
    if (text.bad) { free(text.p); ypak_close(&p); cfree_all(&c); return YPAK_ERR_NOMEM; }
    if (out_path) snprintf(tmp, sizeof tmp, "%s", out_path);
    else snprintf(tmp, sizeof tmp, "%s.rebuild", pack_path);
    rc = ypt_build_text(text.p, text.n, sources_dir, tmp, NULL, err, cap);
    free(text.p);
    if (rc) { ypak_close(&p); cfree_all(&c); return rc; }
    rc = open_pack(&c, &q, tmp, YPAK_VERIFY_NONE);
    if (rc == 0) {
        uint32_t i;
        int64_t first = -1;
        const char* where = NULL;
        size_t wn = 0;
        if (q.file_size == p.file_size && memcmp(q.base_, p.base_, (size_t)p.file_size) == 0) {
            if (report) fprintf(report, "identical: %lld bytes\n", (long long)p.file_size);
        } else {
            for (i = 0; i < p.n && first < 0; i++) {
                ypak_entry a, b2;
                int64_t j;
                ypak_at(&p, i, &a);
                if (ypak_find_n(&q, a.name, a.name_len, &b2)) { where = a.name; wn = a.name_len; first = 0; break; }
                for (j = 0; j < a.size && j < b2.size; j++)
                    if (((const uint8_t*)a.data)[j] != ((const uint8_t*)b2.data)[j]) break;
                if (j < a.size || a.size != b2.size) { where = a.name; wn = a.name_len; first = j; }
            }
            if (where) rc = cfail(&c, YPAK_ERR_CORRUPT, "rebuilt pack differs: entry '%.*s' at byte %lld", (int)wn, where, (long long)first);
            else rc = cfail(&c, YPAK_ERR_CORRUPT, "rebuilt pack differs in its container (%lld against %lld bytes)", (long long)q.file_size, (long long)p.file_size);
            if (report) fprintf(report, "%s\n", err);
        }
        ypak_close(&q);
    }
    ypak_close(&p);
    if (!out_path) uremove(tmp);
    cfree_all(&c);
    return rc;
}

int ypt_append(const char* player_path, const char* pack_path, const char* out_path, char* err, size_t cap) {
    C c;
    ypak_pack p;
    E** all;
    uint8_t* player;
    int64_t pn, padded;
    sink s;
    char tmp[2100];
    int rc;
    memset(&c, 0, sizeof c);
    c.err = err;
    c.cap = cap;
    if (err && cap) err[0] = 0;
    player = read_file(&c, player_path, &pn);
    if (!player) { cfree_all(&c); return YPAK_ERR_IO; }
    rc = open_pack(&c, &p, pack_path, YPAK_VERIFY_OPEN);
    if (rc) { cfree_all(&c); return rc; }
    if (p.zip_start != 0) { ypak_close(&p); rc = cfail(&c, YPAK_ERR_ARG, "'%s' is already appended to a program", pack_path); cfree_all(&c); return rc; }
    all = pack_entries(&c, &p);
    if (!all) { ypak_close(&p); cfree_all(&c); return YPAK_ERR_CORRUPT; }
    assign_chunks(all, (int)p.n);
    padded = (pn + YPAK_ALIGN - 1) & ~(int64_t)(YPAK_ALIGN - 1);
    memset(&s, 0, sizeof s);
    s.diff = -1;
    snprintf(tmp, sizeof tmp, "%s.tmp", out_path);
    s.f = ufopen(tmp, "wb");
    if (!s.f) { ypak_close(&p); rc = cfail(&c, YPAK_ERR_IO, "cannot create '%s'", tmp); cfree_all(&c); return rc; }
    sk_put(&s, player, (size_t)pn);
    sk_zeros(&s, padded - pn);
    rc = write_pack(&c, &s, all, (int)p.n, padded, 1);
    ypak_close(&p);
    rc = finish_file(&c, &s, tmp, out_path, rc);
    cfree_all(&c);
    return rc;
}
