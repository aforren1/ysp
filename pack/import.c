/* import.c - the pack tool's readers of source formats that are not its own
 * (pack/ysp/pack_tool.h): .xlsx workbooks into CSV text for ysp/table.h,
 * JPEG files into texels, and the ICC profile check that PNG and JPEG
 * share. Every input is a file from another lab, so every reader treats
 * its bytes as hostile: each offset and length is checked before use, each
 * decompressed size is bounded before it is allocated, and nothing is
 * guessed (a value the reader does not understand is refused by name).
 * tests/fuzz/import_fuzz.c fuzzes all three. */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "ysp/pack_tool.h"
#include "ysp/pack.h"
#include "ysp/table.h"
#include "lodepng.h"

#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/* stb_image.h is compiled once, in pack/stb_image.c, as JPEG only. */
unsigned char* stbi_load_from_memory(const unsigned char* buffer, int len, int* x, int* y, int* channels_in_file, int desired_channels);
const char* stbi_failure_reason(void);
void stbi_image_free(void* retval_from_stbi_load);

static int ferr(char* err, size_t cap, int code, const char* fmt, ...) {
    va_list ap;
    if (!err || !cap) return code;
    va_start(ap, fmt);
    vsnprintf(err, cap, fmt, ap);
    va_end(ap);
    return code;
}

static uint32_t rle16(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static uint32_t rle32(const uint8_t* p) { return rle16(p) | (rle16(p + 2) << 16); }
static uint32_t rbe16(const uint8_t* p) { return ((uint32_t)p[0] << 8) | p[1]; }
static uint32_t rbe32(const uint8_t* p) { return (rbe16(p) << 16) | rbe16(p + 2); }

/* --- a growable byte buffer ----------------------------------------------------- */

typedef struct buf { char* p; size_t n, cap, limit; int bad; } buf;

static void buf_put(buf* b, const void* s, size_t n) {
    if (b->bad) return;
    if (b->n + n + 1 > b->cap) {
        size_t c = b->cap ? b->cap : 4096;
        char* q;
        if (b->n + n + 1 > b->limit) { b->bad = 1; return; }
        while (c < b->n + n + 1) c *= 2;
        if (c > b->limit) c = b->limit;
        q = (char*)realloc(b->p, c);
        if (!q) { b->bad = 1; return; }
        b->p = q;
        b->cap = c;
    }
    if (n) memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}
static void buf_putc(buf* b, char ch) { buf_put(b, &ch, 1); }

/* ======================================================================== */
/* xlsx                                                                      */
/* ======================================================================== */

/* Bounds. A conditions file is kilobytes; these stop a hostile file (a zip
 * bomb, a sheet of a million styled rows) before it costs memory, and each
 * is named in its message. */
#define XL_MAX_PART   ((size_t)64 << 20)    /* one decompressed part          */
#define XL_MAX_ENTRIES 4096u                /* zip entries                    */
#define XL_MAX_CELLS  (((size_t)YTB_MAX_ROWS + 1) * (size_t)YTB_MAX_COLUMNS)
#define XL_MAX_ROW    1048576               /* Excel's own limits             */
#define XL_MAX_COL    16384

typedef struct zent { const uint8_t* name; uint32_t nl, method, flags, crc; uint32_t csize, usize, lho; } zent;
typedef struct zip { const uint8_t* d; size_t n; zent* e; uint32_t ne; } zip;

static int zip_open(zip* z, const uint8_t* d, size_t n, char* err, size_t cap) {
    size_t i, eocd = 0, lo = n > 65557 ? n - 65557 : 0, at;
    uint32_t ne, cds, cdo, k;
    int found = 0;
    memset(z, 0, sizeof *z);
    z->d = d;
    z->n = n;
    if (n >= 8 && rbe32(d) == 0xD0CF11E0u)
        return ferr(err, cap, YPAK_ERR_FORMAT, "an OLE compound file, not a zip: a password-protected workbook or an old .xls; save it as .xlsx without a password");
    if (n < 22) return ferr(err, cap, YPAK_ERR_FORMAT, "not a zip file (%zu bytes)", n);
    for (i = n - 22 + 1; i-- > lo;) {
        if (rle32(d + i) == 0x06054B50u && i + 22 + rle16(d + i + 20) == n) { eocd = i; found = 1; break; }
    }
    if (!found) return ferr(err, cap, YPAK_ERR_FORMAT, "not a zip file: no end of central directory record");
    if (rle16(d + eocd + 4) || rle16(d + eocd + 6) || rle16(d + eocd + 8) != rle16(d + eocd + 10))
        return ferr(err, cap, YPAK_ERR_FORMAT, "a zip that spans several disks");
    ne = rle16(d + eocd + 10);
    cds = rle32(d + eocd + 12);
    cdo = rle32(d + eocd + 16);
    if (ne == 0xFFFFu || cds == 0xFFFFFFFFu || cdo == 0xFFFFFFFFu)
        return ferr(err, cap, YPAK_ERR_FORMAT, "a zip64 workbook; a conditions file is far smaller than 4 GB");
    if (ne == 0 || ne > XL_MAX_ENTRIES) return ferr(err, cap, YPAK_ERR_FORMAT, "%u zip entries (1 to %u)", ne, XL_MAX_ENTRIES);
    if ((uint64_t)cdo + cds > eocd) return ferr(err, cap, YPAK_ERR_FORMAT, "the zip's central directory lies outside the file");
    z->e = (zent*)calloc(ne, sizeof(zent));
    if (!z->e) return ferr(err, cap, YPAK_ERR_NOMEM, "out of memory");
    at = cdo;
    for (k = 0; k < ne; k++) {
        zent* e = &z->e[k];
        uint32_t el, cl;
        if (at + 46 > (size_t)cdo + cds || rle32(d + at) != 0x02014B50u)
            return ferr(err, cap, YPAK_ERR_FORMAT, "zip central record %u is damaged", k);
        e->flags = rle16(d + at + 8);
        e->method = rle16(d + at + 10);
        e->crc = rle32(d + at + 16);
        e->csize = rle32(d + at + 20);
        e->usize = rle32(d + at + 24);
        e->nl = rle16(d + at + 28);
        el = rle16(d + at + 30);
        cl = rle16(d + at + 32);
        e->lho = rle32(d + at + 42);
        if (at + 46 + (size_t)e->nl + el + cl > (size_t)cdo + cds) return ferr(err, cap, YPAK_ERR_FORMAT, "zip central record %u is damaged", k);
        e->name = d + at + 46;
        at += 46 + (size_t)e->nl + el + cl;
    }
    z->ne = ne;
    return 0;
}

static void zip_close(zip* z) { free(z->e); memset(z, 0, sizeof *z); }

/* OPC part names compare without regard to ASCII case (ECMA-376 part 2). */
static int name_ieq(const uint8_t* a, size_t na, const char* b) {
    size_t i;
    if (na != strlen(b)) return 0;
    for (i = 0; i < na; i++) {
        int x = a[i], y = (unsigned char)b[i];
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
    }
    return 1;
}

/* A part's bytes, decompressed and checked by its CRC-32: malloc'd, NUL
 * after the end. 1 when the part does not exist, 0 on success, < 0 on a
 * failure with the message. */
static int zip_part(const zip* z, const char* name, char** out, size_t* outn, char* err, size_t cap) {
    uint32_t k;
    *out = NULL;
    *outn = 0;
    for (k = 0; k < z->ne; k++) {
        const zent* e = &z->e[k];
        size_t lh = e->lho, data;
        unsigned char* u = NULL;
        size_t un = 0;
        if (!name_ieq(e->name, e->nl, name)) continue;
        if (e->flags & 1u) return ferr(err, cap, YPAK_ERR_FORMAT, "zip entry '%s' is encrypted", name);
        if (e->usize > XL_MAX_PART) return ferr(err, cap, YPAK_ERR_FORMAT, "'%s' is %u bytes; the limit is %zu MiB", name, e->usize, XL_MAX_PART >> 20);
        if (lh + 30 > z->n || rle32(z->d + lh) != 0x04034B50u) return ferr(err, cap, YPAK_ERR_FORMAT, "zip entry '%s': no local header", name);
        data = lh + 30 + rle16(z->d + lh + 26) + rle16(z->d + lh + 28);
        if (data > z->n || e->csize > z->n - data) return ferr(err, cap, YPAK_ERR_FORMAT, "zip entry '%s' runs past the end of the file", name);
        if (e->method == 0) {
            if (e->csize != e->usize) return ferr(err, cap, YPAK_ERR_FORMAT, "zip entry '%s': stored with two sizes", name);
            u = (unsigned char*)malloc((size_t)e->usize + 1);
            if (!u) return ferr(err, cap, YPAK_ERR_NOMEM, "out of memory");
            memcpy(u, z->d + data, e->usize);
            un = e->usize;
        } else if (e->method == 8) {
            LodePNGDecompressSettings s;
            unsigned rc;
            lodepng_decompress_settings_init(&s);
            s.max_output_size = (size_t)e->usize + 1;
            rc = lodepng_inflate(&u, &un, z->d + data, e->csize, &s);
            if (rc) { free(u); return ferr(err, cap, YPAK_ERR_FORMAT, "zip entry '%s': deflate data: %s", name, lodepng_error_text(rc)); }
            if (un != e->usize) { free(u); return ferr(err, cap, YPAK_ERR_FORMAT, "zip entry '%s' inflates to %zu bytes; its record says %u", name, un, e->usize); }
            {
                unsigned char* v = (unsigned char*)realloc(u, un + 1);
                if (!v) { free(u); return ferr(err, cap, YPAK_ERR_NOMEM, "out of memory"); }
                u = v;
            }
        } else {
            return ferr(err, cap, YPAK_ERR_FORMAT, "zip entry '%s' uses compression method %u (stored and deflate only)", name, e->method);
        }
        u[un] = 0;
        if (ypak_crc32(0, u, un) != e->crc) { free(u); return ferr(err, cap, YPAK_ERR_FORMAT, "zip entry '%s' fails its CRC-32: the file is damaged", name); }
        *out = (char*)u;
        *outn = un;
        return 0;
    }
    return 1;
}

/* --- XML: the subset OOXML parts use ------------------------------------------- */

enum { XT_END, XT_OPEN, XT_CLOSE, XT_EMPTY, XT_TEXT };
typedef struct xr { const char* p; const char* e; } xr;
typedef struct xt { int t, cdata; const char* name; size_t nl; const char* attr; size_t al; const char* text; size_t tl; } xt;

/* The next token. Declarations, processing instructions and comments are
 * skipped; a DOCTYPE is refused (no part has one, and its entities are an
 * attack); CDATA is text taken as is. */
static int x_next(xr* x, xt* t, char* err, size_t cap) {
    for (;;) {
        const char* p = x->p;
        memset(t, 0, sizeof *t);
        if (p >= x->e) { t->t = XT_END; return 0; }
        if (*p != '<') {
            const char* q = (const char*)memchr(p, '<', (size_t)(x->e - p));
            if (!q) q = x->e;
            t->t = XT_TEXT;
            t->text = p;
            t->tl = (size_t)(q - p);
            x->p = q;
            return 0;
        }
        if (x->e - p >= 2 && p[1] == '?') {
            const char* q = p + 2;
            while (q + 1 < x->e && !(q[0] == '?' && q[1] == '>')) q++;
            if (q + 1 >= x->e) return ferr(err, cap, YPAK_ERR_FORMAT, "XML: a processing instruction without its end");
            x->p = q + 2;
            continue;
        }
        if (x->e - p >= 4 && !memcmp(p, "<!--", 4)) {
            const char* q = p + 4;
            while (q + 2 < x->e && !(q[0] == '-' && q[1] == '-' && q[2] == '>')) q++;
            if (q + 2 >= x->e) return ferr(err, cap, YPAK_ERR_FORMAT, "XML: a comment without its end");
            x->p = q + 3;
            continue;
        }
        if (x->e - p >= 9 && !memcmp(p, "<![CDATA[", 9)) {
            const char* q = p + 9;
            while (q + 2 < x->e && !(q[0] == ']' && q[1] == ']' && q[2] == '>')) q++;
            if (q + 2 >= x->e) return ferr(err, cap, YPAK_ERR_FORMAT, "XML: a CDATA section without its end");
            t->t = XT_TEXT;
            t->cdata = 1;
            t->text = p + 9;
            t->tl = (size_t)(q - (p + 9));
            x->p = q + 3;
            return 0;
        }
        if (x->e - p >= 2 && p[1] == '!') return ferr(err, cap, YPAK_ERR_FORMAT, "XML: a DOCTYPE or declaration; workbook parts have none");
        {
            const char* q = p + 1;
            int close = 0;
            char quote = 0;
            if (q < x->e && *q == '/') { close = 1; q++; }
            t->name = q;
            while (q < x->e && *q != '>' && *q != '/' && *q != ' ' && *q != '\t' && *q != '\r' && *q != '\n') q++;
            t->nl = (size_t)(q - t->name);
            if (!t->nl) return ferr(err, cap, YPAK_ERR_FORMAT, "XML: a tag without a name");
            t->attr = q;
            while (q < x->e && (quote || *q != '>')) {
                if (quote) { if (*q == quote) quote = 0; }
                else if (*q == '"' || *q == '\'') quote = *q;
                else if (*q == '<') return ferr(err, cap, YPAK_ERR_FORMAT, "XML: '<' inside a tag");
                q++;
            }
            if (q >= x->e) return ferr(err, cap, YPAK_ERR_FORMAT, "XML: a tag without its '>'");
            t->al = (size_t)(q - t->attr);
            if (close) t->t = XT_CLOSE;
            else if (t->al && t->attr[t->al - 1] == '/') { t->t = XT_EMPTY; t->al--; }
            else t->t = XT_OPEN;
            x->p = q + 1;
            return 0;
        }
    }
}

/* The local name (after any prefix) equals s. */
static int x_is(const char* name, size_t nl, const char* s) {
    const char* c = (const char*)memchr(name, ':', nl);
    size_t sl = strlen(s);
    if (c) { nl -= (size_t)(c + 1 - name); name = c + 1; }
    return nl == sl && !memcmp(name, s, sl);
}

/* An attribute's raw value by local name; prefixed: 1 for a namespaced
 * attribute only (r:id), 0 for an unprefixed one only. 1 when found. */
static int x_attr(const xt* t, const char* local, int prefixed, const char** v, size_t* vn) {
    const char* p = t->attr;
    const char* e = t->attr + t->al;
    while (p < e) {
        const char* n0;
        size_t nl;
        char q;
        while (p < e && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
        if (p >= e) break;
        n0 = p;
        while (p < e && *p != '=' && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') p++;
        nl = (size_t)(p - n0);
        while (p < e && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
        if (p >= e || *p != '=') return 0;
        p++;
        while (p < e && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
        if (p >= e || (*p != '"' && *p != '\'')) return 0;
        q = *p++;
        {
            const char* v0 = p;
            while (p < e && *p != q) p++;
            if (p >= e) return 0;
            if (x_is(n0, nl, local) && (memchr(n0, ':', nl) != NULL) == (prefixed != 0)) { *v = v0; *vn = (size_t)(p - v0); return 1; }
            p++;
        }
    }
    return 0;
}

static void put_utf8(buf* b, uint32_t c) {
    char u[4];
    if (c < 0x80) { u[0] = (char)c; buf_put(b, u, 1); }
    else if (c < 0x800) { u[0] = (char)(0xC0 | (c >> 6)); u[1] = (char)(0x80 | (c & 63)); buf_put(b, u, 2); }
    else if (c < 0x10000) { u[0] = (char)(0xE0 | (c >> 12)); u[1] = (char)(0x80 | ((c >> 6) & 63)); u[2] = (char)(0x80 | (c & 63)); buf_put(b, u, 3); }
    else { u[0] = (char)(0xF0 | (c >> 18)); u[1] = (char)(0x80 | ((c >> 12) & 63)); u[2] = (char)(0x80 | ((c >> 6) & 63)); u[3] = (char)(0x80 | (c & 63)); buf_put(b, u, 4); }
}

static int hexv(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

/* XML text with its five entities and character references decoded, onto
 * b. ooxml: also the _xHHHH_ escapes of ECMA-376 ST_Xstring (cell text). */
static int x_text(buf* b, const char* s, size_t n, int cdata, int ooxml, char* err, size_t cap) {
    size_t i = 0;
    while (i < n) {
        char ch = s[i];
        if (ch == '&' && !cdata) {
            const char* semi = (const char*)memchr(s + i, ';', n - i < 12 ? n - i : 12);
            size_t k = semi ? (size_t)(semi - (s + i)) : 0;
            uint32_t c = 0;
            if (!semi) return ferr(err, cap, YPAK_ERR_FORMAT, "XML: '&' without ';'");
            if (k == 3 && !memcmp(s + i + 1, "lt", 2)) c = '<';
            else if (k == 3 && !memcmp(s + i + 1, "gt", 2)) c = '>';
            else if (k == 4 && !memcmp(s + i + 1, "amp", 3)) c = '&';
            else if (k == 5 && !memcmp(s + i + 1, "quot", 4)) c = '"';
            else if (k == 5 && !memcmp(s + i + 1, "apos", 4)) c = '\'';
            else if (k >= 3 && s[i + 1] == '#') {
                size_t j = 2;
                int base = 10;
                if (s[i + 2] == 'x') { base = 16; j = 3; }
                if (j >= k) return ferr(err, cap, YPAK_ERR_FORMAT, "XML: an empty character reference");
                for (; j < k; j++) {
                    int v = base == 16 ? hexv(s[i + j]) : (s[i + j] >= '0' && s[i + j] <= '9' ? s[i + j] - '0' : -1);
                    if (v < 0) return ferr(err, cap, YPAK_ERR_FORMAT, "XML: a bad character reference");
                    c = c * (uint32_t)base + (uint32_t)v;
                    if (c > 0x10FFFF) return ferr(err, cap, YPAK_ERR_FORMAT, "XML: a character reference above U+10FFFF");
                }
                if (c == 0 || (c >= 0xD800 && c <= 0xDFFF)) return ferr(err, cap, YPAK_ERR_FORMAT, "XML: a character reference to U+%04X", c);
            } else {
                return ferr(err, cap, YPAK_ERR_FORMAT, "XML: the entity '&%.*s;' is not one of XML's five", (int)(k - 1), s + i + 1);
            }
            put_utf8(b, c);
            i += k + 1;
            continue;
        }
        if (ooxml && ch == '_' && n - i >= 7 && s[i + 1] == 'x' && s[i + 6] == '_' &&
            hexv(s[i + 2]) >= 0 && hexv(s[i + 3]) >= 0 && hexv(s[i + 4]) >= 0 && hexv(s[i + 5]) >= 0) {
            uint32_t c = (uint32_t)(hexv(s[i + 2]) << 12 | hexv(s[i + 3]) << 8 | hexv(s[i + 4]) << 4 | hexv(s[i + 5]));
            if (c == 0 || (c >= 0xD800 && c <= 0xDFFF)) return ferr(err, cap, YPAK_ERR_FORMAT, "a cell text escape to U+%04X", c);
            put_utf8(b, c);
            i += 7;
            continue;
        }
        buf_putc(b, ch);
        i++;
    }
    return b->bad ? ferr(err, cap, YPAK_ERR_NOMEM, "out of memory or past the size limit") : 0;
}

/* --- relationships and paths ----------------------------------------------------- */

/* dir (ends with '/', or empty) joined with a relative target, '.' and '..'
 * resolved; an absolute target ("/xl/...") from the root. 0 or < 0. */
static int join_path(const char* dir, const char* t, size_t tn, char* out, size_t cap) {
    char tmp[1024];
    size_t n = 0, i = 0, o = 0;
    if (tn && t[0] == '/') { t++; tn--; dir = ""; }
    n = (size_t)snprintf(tmp, sizeof tmp, "%s%.*s", dir, (int)tn, t);
    if (n >= sizeof tmp || n >= cap) return -1;
    while (i < n) {
        size_t j = i;
        while (j < n && tmp[j] != '/') j++;
        if (j - i == 1 && tmp[i] == '.') { /* skip */ }
        else if (j - i == 2 && tmp[i] == '.' && tmp[i + 1] == '.') {
            if (!o) return -1;
            o--;
            while (o && out[o - 1] != '/') o--;
        } else if (j > i) {
            memcpy(out + o, tmp + i, j - i);
            o += j - i;
            if (j < n) out[o++] = '/';
        }
        i = j + 1;
    }
    if (o && out[o - 1] == '/') o--;
    out[o] = 0;
    return o ? 0 : -1;
}

typedef struct rel { char id[64]; char target[512]; int type; } rel;
enum { REL_OTHER, REL_DOC, REL_SHEET, REL_STRINGS, REL_STYLES };

/* Relationships of a part: Id, the target resolved against dir, the type.
 * External targets are skipped. Returns the count or < 0. */
static int read_rels(const char* xml, size_t n, const char* dir, rel* r, int max, char* err, size_t cap) {
    xr x;
    xt t;
    int k = 0, rc;
    x.p = xml;
    x.e = xml + n;
    while ((rc = x_next(&x, &t, err, cap)) == 0 && t.t != XT_END) {
        const char *id, *tg, *ty, *mode;
        size_t idn, tgn, tyn, mn;
        if ((t.t != XT_OPEN && t.t != XT_EMPTY) || !x_is(t.name, t.nl, "Relationship")) continue;
        if (!x_attr(&t, "Id", 0, &id, &idn) || !x_attr(&t, "Target", 0, &tg, &tgn) || !x_attr(&t, "Type", 0, &ty, &tyn)) continue;
        if (x_attr(&t, "TargetMode", 0, &mode, &mn) && mn == 8 && !memcmp(mode, "External", 8)) continue;
        if (k >= max) return ferr(err, cap, YPAK_ERR_FORMAT, "more than %d relationships in one part", max);
        if (idn >= sizeof r[k].id) continue;
        memcpy(r[k].id, id, idn);
        r[k].id[idn] = 0;
        {
            buf b;
            memset(&b, 0, sizeof b);
            b.limit = 1024;
            if (x_text(&b, tg, tgn, 0, 0, err, cap) < 0 || !b.p || join_path(dir, b.p, b.n, r[k].target, sizeof r[k].target) < 0) {
                free(b.p);
                continue;
            }
            free(b.p);
        }
#define ENDS(s) (tyn >= sizeof(s) - 1 && !memcmp(ty + tyn - (sizeof(s) - 1), s, sizeof(s) - 1))
        r[k].type = ENDS("/officeDocument") ? REL_DOC : ENDS("/worksheet") ? REL_SHEET : ENDS("/sharedStrings") ? REL_STRINGS
                  : ENDS("/styles") ? REL_STYLES : REL_OTHER;
#undef ENDS
        k++;
    }
    return rc < 0 ? rc : k;
}

static void dir_of_part(const char* part, char* dir, size_t cap) {
    const char* s = strrchr(part, '/');
    size_t n = s ? (size_t)(s - part) + 1 : 0;
    if (n >= cap) n = 0;
    memcpy(dir, part, n);
    dir[n] = 0;
}

/* --- styles: which cell formats are dates or times ------------------------------- */

/* A number format code shows a date or time when it has d, m, y, h or s
 * outside quoted text, escapes and [color] or [condition] brackets
 * ([h], [mm] and [ss] are elapsed times, so they count). */
static int code_is_date(const char* s, size_t n) {
    size_t i = 0;
    while (i < n) {
        char c = s[i];
        if (c == '"') { i++; while (i < n && s[i] != '"') i++; i++; continue; }
        if (c == '\\' || c == '_' || c == '*') { i += 2; continue; }
        if (c == '[') {
            size_t j = i + 1;
            int time = j < n;
            while (j < n && s[j] != ']') { char d = (char)(s[j] | 32); if (d != 'h' && d != 'm' && d != 's') time = 0; j++; }
            if (time && j > i + 1) return 1;
            i = j + 1;
            continue;
        }
        c = (char)(c | 32);
        if (c == 'd' || c == 'm' || c == 'y' || c == 'h' || c == 's') return 1;
        i++;
    }
    return 0;
}

static int builtin_is_date(uint32_t id) {
    return (id >= 14 && id <= 22) || (id >= 27 && id <= 36) || (id >= 45 && id <= 47) || (id >= 50 && id <= 58);
}

static uint32_t parse_u32(const char* s, size_t n, int* ok) {
    uint64_t v = 0;
    size_t i;
    *ok = n > 0 && n <= 9;
    for (i = 0; i < n && *ok; i++) {
        if (s[i] < '0' || s[i] > '9') *ok = 0;
        else v = v * 10 + (uint64_t)(s[i] - '0');
    }
    return (uint32_t)v;
}

/* is_date[k] for cell format (style) k; n_styles set. */
static int read_styles(const char* xml, size_t n, uint8_t** is_date, uint32_t* n_styles, char* err, size_t cap) {
    xr x;
    xt t;
    int rc, in_xfs = 0;
    uint32_t nf = 0, cf = 0, k;
    uint32_t* fid = NULL;
    uint8_t* fdate = NULL;
    uint8_t* out = NULL;
    uint32_t no = 0, co = 0;
    x.p = xml;
    x.e = xml + n;
    while ((rc = x_next(&x, &t, err, cap)) == 0 && t.t != XT_END) {
        if (t.t == XT_CLOSE && x_is(t.name, t.nl, "cellXfs")) in_xfs = 0;
        if (t.t != XT_OPEN && t.t != XT_EMPTY) continue;
        if (x_is(t.name, t.nl, "numFmt")) {
            const char *a, *b;
            size_t an, bn;
            int ok;
            if (x_attr(&t, "numFmtId", 0, &a, &an) && x_attr(&t, "formatCode", 0, &b, &bn)) {
                uint32_t id = parse_u32(a, an, &ok);
                buf code;
                if (!ok) continue;
                if (nf == cf) {
                    uint32_t nc = cf ? 2 * cf : 32;
                    uint32_t* f2;
                    uint8_t* d2;
                    if (nc > 65536) { rc = ferr(err, cap, YPAK_ERR_FORMAT, "styles: more than 65536 number formats"); break; }
                    f2 = (uint32_t*)realloc(fid, nc * sizeof(uint32_t));
                    if (f2) fid = f2;
                    d2 = (uint8_t*)realloc(fdate, nc);
                    if (d2) fdate = d2;
                    if (!f2 || !d2) { rc = ferr(err, cap, YPAK_ERR_NOMEM, "out of memory"); break; }
                    cf = nc;
                }
                memset(&code, 0, sizeof code);
                code.limit = 4096;
                if (x_text(&code, b, bn, 0, 0, err, cap) < 0) { free(code.p); rc = YPAK_ERR_FORMAT; break; }
                fid[nf] = id;
                fdate[nf] = (uint8_t)code_is_date(code.p ? code.p : "", code.n);
                nf++;
                free(code.p);
            }
        } else if (x_is(t.name, t.nl, "cellXfs") && t.t == XT_OPEN) {
            in_xfs = 1;
        } else if (in_xfs && x_is(t.name, t.nl, "xf")) {
            const char* a;
            size_t an;
            uint32_t id = 0;
            int ok = 1, d = 0;
            if (x_attr(&t, "numFmtId", 0, &a, &an)) id = parse_u32(a, an, &ok);
            if (!ok) id = 0;
            d = builtin_is_date(id);
            for (k = 0; k < nf; k++) if (fid[k] == id) d = fdate[k];
            if (no == co) {
                uint32_t nc = co ? 2 * co : 64;
                uint8_t* o2;
                if (nc > 1u << 20) { rc = ferr(err, cap, YPAK_ERR_FORMAT, "styles: too many cell formats"); break; }
                o2 = (uint8_t*)realloc(out, nc);
                if (!o2) { rc = ferr(err, cap, YPAK_ERR_NOMEM, "out of memory"); break; }
                out = o2;
                co = nc;
            }
            out[no++] = (uint8_t)d;
        }
    }
    free(fid);
    free(fdate);
    if (rc < 0) { free(out); return rc; }
    *is_date = out;
    *n_styles = no;
    return 0;
}

/* --- shared strings -------------------------------------------------------------- */

typedef struct sst { buf text; uint32_t* off; uint32_t* len; uint32_t n, cap; } sst;

static int read_sst(const char* xml, size_t n, sst* s, char* err, size_t cap) {
    xr x;
    xt t;
    int rc, in_si = 0, in_t = 0, in_rph = 0;
    size_t start = 0;
    x.p = xml;
    x.e = xml + n;
    s->text.limit = (size_t)1 << 30;
    while ((rc = x_next(&x, &t, err, cap)) == 0 && t.t != XT_END) {
        if (t.t == XT_OPEN && x_is(t.name, t.nl, "si")) { in_si = 1; start = s->text.n; }
        else if (t.t == XT_EMPTY && x_is(t.name, t.nl, "si")) { in_si = 1; start = s->text.n; t.t = XT_CLOSE; }
        if (!in_si) continue;
        if (t.t == XT_OPEN && x_is(t.name, t.nl, "rPh")) in_rph = 1;
        else if (t.t == XT_CLOSE && x_is(t.name, t.nl, "rPh")) in_rph = 0;
        else if (t.t == XT_OPEN && x_is(t.name, t.nl, "t")) in_t = 1;
        else if (t.t == XT_CLOSE && x_is(t.name, t.nl, "t")) in_t = 0;
        else if (t.t == XT_TEXT && in_t && !in_rph) { if ((rc = x_text(&s->text, t.text, t.tl, t.cdata, 1, err, cap)) < 0) break; }
        else if (t.t == XT_CLOSE && x_is(t.name, t.nl, "si")) {
            if (s->n == s->cap) {
                uint32_t nc = s->cap ? 2 * s->cap : 256;
                uint32_t* o2;
                uint32_t* l2;
                if (nc > (1u << 24)) { rc = ferr(err, cap, YPAK_ERR_FORMAT, "more than 16 million shared strings"); break; }
                o2 = (uint32_t*)realloc(s->off, nc * sizeof(uint32_t));
                if (o2) s->off = o2;
                l2 = (uint32_t*)realloc(s->len, nc * sizeof(uint32_t));
                if (l2) s->len = l2;
                if (!o2 || !l2) { rc = ferr(err, cap, YPAK_ERR_NOMEM, "out of memory"); break; }
                s->cap = nc;
            }
            s->off[s->n] = (uint32_t)start;
            s->len[s->n] = (uint32_t)(s->text.n - start);
            s->n++;
            in_si = in_t = in_rph = 0;
        }
    }
    if (rc == 0 && s->text.bad) rc = ferr(err, cap, YPAK_ERR_NOMEM, "shared strings: out of memory");
    return rc;
}

/* --- the sheet ----------------------------------------------------------------- */

enum { CV_NUMBER, CV_TEXT, CV_BOOL };
typedef struct xcell { uint32_t row, col, off, len; int kind; } xcell;

typedef struct xsheet {
    xcell*   c;
    size_t   n, cap;
    buf      text;     /* every cell's value, back to back */
    int      formulas;
} xsheet;

static void cell_ref(uint32_t row, uint32_t col, char* out) {
    char letters[4];
    int k = 0, i;
    uint32_t c = col + 1;
    while (c && k < 3) { letters[k++] = (char)('A' + (c - 1) % 26); c = (c - 1) / 26; }
    for (i = 0; i < k; i++) out[i] = letters[k - 1 - i];
    snprintf(out + k, 12, "%u", row + 1);
}

/* "B12" into 0-based column and row. */
static int parse_ref(const char* s, size_t n, uint32_t* row, uint32_t* col) {
    size_t i = 0;
    uint32_t c = 0, r = 0;
    while (i < n && i < 3 && s[i] >= 'A' && s[i] <= 'Z') { c = c * 26 + (uint32_t)(s[i] - 'A' + 1); i++; }
    if (i == 0 || i == n) return -1;
    for (; i < n; i++) {
        if (s[i] < '0' || s[i] > '9' || r > XL_MAX_ROW) return -1;
        r = r * 10 + (uint32_t)(s[i] - '0');
    }
    if (c < 1 || c > XL_MAX_COL || r < 1 || r > XL_MAX_ROW) return -1;
    *col = c - 1;
    *row = r - 1;
    return 0;
}

static int add_cell(xsheet* sh, uint32_t row, uint32_t col, int kind, const char* v, size_t vn, char* err, size_t cap) {
    if (sh->n == sh->cap) {
        size_t nc = sh->cap ? 2 * sh->cap : 1024;
        xcell* c2;
        if (sh->n >= XL_MAX_CELLS) return ferr(err, cap, YPAK_ERR_FORMAT, "more than %zu cells with values; a table holds %d rows of %d columns",
                                              XL_MAX_CELLS, YTB_MAX_ROWS, YTB_MAX_COLUMNS);
        c2 = (xcell*)realloc(sh->c, nc * sizeof(xcell));
        if (!c2) return ferr(err, cap, YPAK_ERR_NOMEM, "out of memory");
        sh->c = c2;
        sh->cap = nc;
    }
    if (vn > YTB_MAX_FIELD) {
        char ref[16];
        cell_ref(row, col, ref);
        return ferr(err, cap, YPAK_ERR_FORMAT, "cell %s holds %zu bytes; a table field holds at most %d", ref, vn, YTB_MAX_FIELD);
    }
    sh->c[sh->n].row = row;
    sh->c[sh->n].col = col;
    sh->c[sh->n].off = (uint32_t)sh->text.n;
    sh->c[sh->n].len = (uint32_t)vn;
    sh->c[sh->n].kind = kind;
    buf_put(&sh->text, v, vn);
    if (sh->text.bad) return ferr(err, cap, YPAK_ERR_NOMEM, "out of memory");
    sh->n++;
    return 0;
}

static int read_sheet(const char* xml, size_t n, const sst* ss, const uint8_t* is_date, uint32_t n_styles, xsheet* sh,
                      char* err, size_t cap) {
    xr x;
    xt t;
    int rc, in_data = 0, in_c = 0, in_v = 0, in_is = 0, in_t = 0, in_rph = 0, has_f = 0, has_v = 0;
    uint32_t row = 0, crow = 0, ccol = 0, style = 0;
    int64_t last_row = -1, last_col = -1;
    char ctype[16];
    buf v;
    memset(&v, 0, sizeof v);
    v.limit = (size_t)1 << 26;
    sh->text.limit = (size_t)1 << 30;
    ctype[0] = 0;
    x.p = xml;
    x.e = xml + n;
    while ((rc = x_next(&x, &t, err, cap)) == 0 && t.t != XT_END) {
        if (!in_data) {
            if (t.t == XT_OPEN && x_is(t.name, t.nl, "sheetData")) in_data = 1;
            continue;
        }
        if (t.t == XT_CLOSE && x_is(t.name, t.nl, "sheetData")) break;
        if ((t.t == XT_OPEN || t.t == XT_EMPTY) && x_is(t.name, t.nl, "row")) {
            const char* a;
            size_t an;
            if (x_attr(&t, "r", 0, &a, &an)) {
                int ok;
                uint32_t r = parse_u32(a, an, &ok);
                if (!ok || r < 1 || r > XL_MAX_ROW || (int64_t)r - 1 <= last_row) { rc = ferr(err, cap, YPAK_ERR_FORMAT, "sheet: row number '%.*s' out of order or out of range", (int)(an < 20 ? an : 20), a); break; }
                row = r - 1;
            } else {
                row = (uint32_t)(last_row + 1);
            }
            last_row = row;
            last_col = -1;
            continue;
        }
        if ((t.t == XT_OPEN || t.t == XT_EMPTY) && x_is(t.name, t.nl, "c")) {
            const char* a;
            size_t an;
            if (last_row < 0) { rc = ferr(err, cap, YPAK_ERR_FORMAT, "sheet: a cell outside a row"); break; }
            crow = row;
            if (x_attr(&t, "r", 0, &a, &an)) {
                if (parse_ref(a, an, &crow, &ccol) < 0 || crow != row || (int64_t)ccol <= last_col) {
                    rc = ferr(err, cap, YPAK_ERR_FORMAT, "sheet: cell reference '%.*s' out of order or out of range", (int)(an < 20 ? an : 20), a);
                    break;
                }
            } else {
                ccol = (uint32_t)(last_col + 1);
                if (ccol >= XL_MAX_COL) { rc = ferr(err, cap, YPAK_ERR_FORMAT, "sheet: more than %d columns", XL_MAX_COL); break; }
            }
            last_col = ccol;
            ctype[0] = 0;
            if (x_attr(&t, "t", 0, &a, &an) && an < sizeof ctype) { memcpy(ctype, a, an); ctype[an] = 0; }
            else if (x_attr(&t, "t", 0, &a, &an)) { rc = ferr(err, cap, YPAK_ERR_FORMAT, "sheet: an unknown cell type"); break; }
            style = 0;
            if (x_attr(&t, "s", 0, &a, &an)) {
                int ok;
                style = parse_u32(a, an, &ok);
                if (!ok) style = 0;
            }
            has_f = has_v = 0;
            v.n = 0;
            if (v.p) v.p[0] = 0;
            if (t.t == XT_EMPTY) continue;   /* a styled cell with no value */
            in_c = 1;
            continue;
        }
        if (!in_c) continue;
        if (t.t == XT_OPEN || t.t == XT_EMPTY) {
            if (x_is(t.name, t.nl, "f")) has_f = 1;
            else if (x_is(t.name, t.nl, "v") && t.t == XT_OPEN) { in_v = 1; has_v = 1; }
            else if (x_is(t.name, t.nl, "v")) has_v = 1;
            else if (x_is(t.name, t.nl, "is")) { in_is = 1; has_v = 1; }
            else if (x_is(t.name, t.nl, "rPh") && t.t == XT_OPEN) in_rph = 1;
            else if (x_is(t.name, t.nl, "t") && t.t == XT_OPEN && in_is) in_t = 1;
            continue;
        }
        if (t.t == XT_TEXT) {
            if ((in_v && !in_is) || (in_is && in_t && !in_rph)) {
                rc = x_text(&v, t.text, t.tl, t.cdata, in_is, err, cap);
                if (rc < 0) break;
            }
            continue;
        }
        /* XT_CLOSE */
        if (x_is(t.name, t.nl, "v")) in_v = 0;
        else if (x_is(t.name, t.nl, "t")) in_t = 0;
        else if (x_is(t.name, t.nl, "rPh")) in_rph = 0;
        else if (x_is(t.name, t.nl, "is")) in_is = 0;
        else if (x_is(t.name, t.nl, "c")) {
            char ref[16];
            const char* val = v.p ? v.p : "";
            size_t vn = v.n;
            in_c = in_v = in_is = in_t = in_rph = 0;
            cell_ref(crow, ccol, ref);
            if (!has_v) {
                if (has_f) { rc = ferr(err, cap, YPAK_ERR_FORMAT, "cell %s has a formula and no saved value; open the workbook in Excel and save it", ref); break; }
                continue;
            }
            if (has_f) sh->formulas++;
            if (!ctype[0] || !strcmp(ctype, "n")) {
                double d;
                if (!vn) continue;
                if (ytb_parse_number(val, vn, &d) != YTB_NUM_OK) { rc = ferr(err, cap, YPAK_ERR_FORMAT, "cell %s: '%.*s' is not a number", ref, (int)(vn < 40 ? vn : 40), val); break; }
                if (is_date && style < n_styles && is_date[style]) {
                    rc = ferr(err, cap, YPAK_ERR_FORMAT, "cell %s is a date or a time (its cell format); a table holds numbers and text: format the column as Number or Text", ref);
                    break;
                }
                rc = add_cell(sh, crow, ccol, CV_NUMBER, val, vn, err, cap);
            } else if (!strcmp(ctype, "s")) {
                int ok;
                uint32_t k = parse_u32(val, vn, &ok);
                if (!ok || !ss || k >= ss->n) { rc = ferr(err, cap, YPAK_ERR_FORMAT, "cell %s names shared string '%.*s', which does not exist", ref, (int)(vn < 20 ? vn : 20), val); break; }
                if (!ss->len[k]) continue;
                rc = add_cell(sh, crow, ccol, CV_TEXT, ss->text.p + ss->off[k], ss->len[k], err, cap);
            } else if (!strcmp(ctype, "str") || !strcmp(ctype, "inlineStr")) {
                if (!vn) continue;
                rc = add_cell(sh, crow, ccol, CV_TEXT, val, vn, err, cap);
            } else if (!strcmp(ctype, "b")) {
                if (vn != 1 || (val[0] != '0' && val[0] != '1')) { rc = ferr(err, cap, YPAK_ERR_FORMAT, "cell %s: a boolean that is not 0 or 1", ref); break; }
                rc = add_cell(sh, crow, ccol, CV_BOOL, val[0] == '1' ? "TRUE" : "FALSE", val[0] == '1' ? 4 : 5, err, cap);
            } else if (!strcmp(ctype, "e")) {
                rc = ferr(err, cap, YPAK_ERR_FORMAT, "cell %s holds the error %.*s; fix it in the workbook", ref, (int)(vn < 20 ? vn : 20), val);
                break;
            } else if (!strcmp(ctype, "d")) {
                rc = ferr(err, cap, YPAK_ERR_FORMAT, "cell %s is a date; a table holds numbers and text", ref);
                break;
            } else {
                rc = ferr(err, cap, YPAK_ERR_FORMAT, "cell %s has the unknown type '%s'", ref, ctype);
                break;
            }
            if (rc < 0) break;
        }
    }
    free(v.p);
    if (rc == 0 && !in_data) rc = ferr(err, cap, YPAK_ERR_FORMAT, "the sheet has no sheetData");
    return rc;
}

/* --- the grid as CSV ------------------------------------------------------------ */

static void csv_field(buf* b, const char* s, size_t n) {
    size_t i;
    int q = 0;
    for (i = 0; i < n; i++) if (s[i] == ',' || s[i] == '"' || s[i] == '\r' || s[i] == '\n') q = 1;
    if (!q) { buf_put(b, s, n); return; }
    buf_putc(b, '"');
    for (i = 0; i < n; i++) { if (s[i] == '"') buf_putc(b, '"'); buf_putc(b, s[i]); }
    buf_putc(b, '"');
}

static int cell_cmp(const void* a, const void* b) {
    const xcell* x = (const xcell*)a;
    const xcell* y = (const xcell*)b;
    if (x->row != y->row) return x->row < y->row ? -1 : 1;
    return x->col < y->col ? -1 : x->col > y->col;
}

static int sheet_csv(xsheet* sh, ypt_sheet* out, char* err, size_t cap) {
    uint32_t cols[YTB_MAX_COLUMNS];
    int nc = 0, k;
    uint8_t any_text[YTB_MAX_COLUMNS], any_other[YTB_MAX_COLUMNS];
    size_t i = 0;
    uint32_t r, last;
    buf b;
    if (!sh->n) return ferr(err, cap, YPAK_ERR_FORMAT, "the sheet is empty");
    /* in order already when the XML is; sorted for any other order */
    qsort(sh->c, sh->n, sizeof(xcell), cell_cmp);
    if (sh->c[0].row != 0) return ferr(err, cap, YPAK_ERR_FORMAT, "row 1 is empty; the first row names the columns");
    for (; i < sh->n && sh->c[i].row == 0; i++) {
        if (nc == YTB_MAX_COLUMNS) return ferr(err, cap, YPAK_ERR_FORMAT, "more than %d named columns in row 1", YTB_MAX_COLUMNS);
        if (sh->c[i].kind != CV_TEXT) {
            char ref[16];
            cell_ref(0, sh->c[i].col, ref);
            return ferr(err, cap, YPAK_ERR_FORMAT, "cell %s in row 1 is not text; row 1 names the columns", ref);
        }
        cols[nc++] = sh->c[i].col;
    }
    memset(any_text, 0, sizeof any_text);
    memset(any_other, 0, sizeof any_other);
    memset(&b, 0, sizeof b);
    b.limit = (size_t)1 << 31;
    for (k = 0; k < nc; k++) {
        if (k) buf_putc(&b, ',');
        csv_field(&b, sh->text.p + sh->c[k].off, sh->c[k].len);
    }
    buf_putc(&b, '\n');
    last = sh->c[sh->n - 1].row;
    if (last > (uint32_t)YTB_MAX_ROWS) {
        free(b.p);
        return ferr(err, cap, YPAK_ERR_FORMAT, "values down to row %u; a table holds %d data rows", last + 1, YTB_MAX_ROWS);
    }
    for (r = 1; r <= last; r++) {
        for (k = 0; k < nc; k++) {
            if (k) buf_putc(&b, ',');
            while (i < sh->n && sh->c[i].row == r && sh->c[i].col < cols[k]) {
                char ref[16];
                cell_ref(r, sh->c[i].col, ref);
                free(b.p);
                return ferr(err, cap, YPAK_ERR_FORMAT, "cell %s has a value, but its column has no name in row 1", ref);
            }
            if (i < sh->n && sh->c[i].row == r && sh->c[i].col == cols[k]) {
                csv_field(&b, sh->text.p + sh->c[i].off, sh->c[i].len);
                if (sh->c[i].kind == CV_TEXT) any_text[k] = 1;
                else any_other[k] = 1;
                i++;
            }
        }
        if (i < sh->n && sh->c[i].row == r) {
            char ref[16];
            cell_ref(r, sh->c[i].col, ref);
            free(b.p);
            return ferr(err, cap, YPAK_ERR_FORMAT, "cell %s has a value, but its column has no name in row 1", ref);
        }
        buf_putc(&b, '\n');
    }
    if (b.bad) { free(b.p); return ferr(err, cap, YPAK_ERR_NOMEM, "out of memory"); }
    out->csv = b.p;
    out->csv_len = b.n;
    out->n_cols = nc;
    out->rows = (int)last;
    out->text_cols = 0;
    for (k = 0; k < nc; k++) if (any_text[k] && !any_other[k]) out->text_cols |= (uint64_t)1 << k;
    return 0;
}

void ypt_sheet_free(ypt_sheet* s) {
    if (!s) return;
    free(s->csv);
    memset(s, 0, sizeof *s);
}

int ypt_xlsx_read(const void* data, size_t n, const char* want, ypt_sheet* out, char* err, size_t cap) {
    zip z;
    char* part = NULL;
    size_t pn = 0;
    rel* rels = NULL;
    int nr = 0, k, rc;
    char doc[512], dir[512], rp[600], sheet_part[512], strings_part[512], styles_part[512];
    sst ss;
    xsheet sh;
    uint8_t* is_date = NULL;
    uint32_t n_styles = 0;
    if (err && cap) err[0] = 0;
    memset(out, 0, sizeof *out);
    memset(&ss, 0, sizeof ss);
    memset(&sh, 0, sizeof sh);
    rc = zip_open(&z, (const uint8_t*)data, n, err, cap);
    if (rc < 0) { zip_close(&z); return rc; }
    rels = (rel*)calloc(256, sizeof(rel));
    if (!rels) { zip_close(&z); return ferr(err, cap, YPAK_ERR_NOMEM, "out of memory"); }
    /* the package's relationships name the workbook part */
    snprintf(doc, sizeof doc, "xl/workbook.xml");
    rc = zip_part(&z, "_rels/.rels", &part, &pn, err, cap);
    if (rc < 0) goto done;
    if (rc == 0) {
        nr = read_rels(part, pn, "", rels, 256, err, cap);
        free(part);
        part = NULL;
        if (nr < 0) { rc = nr; goto done; }
        for (k = 0; k < nr; k++) if (rels[k].type == REL_DOC) { snprintf(doc, sizeof doc, "%s", rels[k].target); break; }
    }
    dir_of_part(doc, dir, sizeof dir);
    snprintf(rp, sizeof rp, "%s_rels/%s.rels", dir, doc + strlen(dir));
    rc = zip_part(&z, rp, &part, &pn, err, cap);
    if (rc != 0) { if (rc > 0) rc = ferr(err, cap, YPAK_ERR_FORMAT, "not a workbook: no '%s'", rp); goto done; }
    nr = read_rels(part, pn, dir, rels, 256, err, cap);
    free(part);
    part = NULL;
    if (nr < 0) { rc = nr; goto done; }
    strings_part[0] = styles_part[0] = sheet_part[0] = 0;
    for (k = 0; k < nr; k++) {
        if (rels[k].type == REL_STRINGS) snprintf(strings_part, sizeof strings_part, "%s", rels[k].target);
        if (rels[k].type == REL_STYLES) snprintf(styles_part, sizeof styles_part, "%s", rels[k].target);
    }
    /* the sheets, in the workbook's order */
    rc = zip_part(&z, doc, &part, &pn, err, cap);
    if (rc != 0) { if (rc > 0) rc = ferr(err, cap, YPAK_ERR_FORMAT, "not a workbook: no '%s' (an .xlsx saved by Excel, LibreOffice or openpyxl has it)", doc); goto done; }
    {
        xr x;
        xt t;
        int found = 0, count = 0;
        char names[1024];
        size_t nn = 0;
        names[0] = 0;
        x.p = part;
        x.e = part + pn;
        while ((rc = x_next(&x, &t, err, cap)) == 0 && t.t != XT_END && !found) {
            const char *nm, *id;
            size_t nml, idl;
            buf nb;
            if ((t.t != XT_OPEN && t.t != XT_EMPTY) || !x_is(t.name, t.nl, "sheet")) continue;
            if (!x_attr(&t, "name", 0, &nm, &nml) || !x_attr(&t, "id", 1, &id, &idl)) continue;
            memset(&nb, 0, sizeof nb);
            nb.limit = 1024;
            if ((rc = x_text(&nb, nm, nml, 0, 0, err, cap)) < 0) { free(nb.p); break; }
            count++;
            if (nn + nb.n + 4 < sizeof names) nn += (size_t)snprintf(names + nn, sizeof names - nn, "%s'%s'", nn ? ", " : "", nb.p ? nb.p : "");
            if (!want || (nb.p && !strcmp(nb.p, want))) {
                for (k = 0; k < nr; k++)
                    if (rels[k].type == REL_SHEET && strlen(rels[k].id) == idl && !memcmp(rels[k].id, id, idl)) {
                        snprintf(sheet_part, sizeof sheet_part, "%s", rels[k].target);
                        snprintf(out->sheet, sizeof out->sheet, "%s", nb.p ? nb.p : "");
                        found = 1;
                    }
                if (!found && !want) {
                    free(nb.p);
                    rc = ferr(err, cap, YPAK_ERR_FORMAT, "the first sheet is not a worksheet (a chart sheet?); name one with \"sheet\"");
                    break;
                }
            }
            free(nb.p);
        }
        free(part);
        part = NULL;
        if (rc < 0) goto done;
        if (!found) {
            rc = want ? ferr(err, cap, YPAK_ERR_FORMAT, "no worksheet named '%s'; the workbook has %s", want, count ? names : "none")
                      : ferr(err, cap, YPAK_ERR_FORMAT, "the workbook lists no sheet");
            goto done;
        }
    }
    if (strings_part[0]) {
        rc = zip_part(&z, strings_part, &part, &pn, err, cap);
        if (rc < 0) goto done;
        if (rc == 0) {
            rc = read_sst(part, pn, &ss, err, cap);
            free(part);
            part = NULL;
            if (rc < 0) goto done;
        }
    }
    if (styles_part[0]) {
        rc = zip_part(&z, styles_part, &part, &pn, err, cap);
        if (rc < 0) goto done;
        if (rc == 0) {
            rc = read_styles(part, pn, &is_date, &n_styles, err, cap);
            free(part);
            part = NULL;
            if (rc < 0) goto done;
        }
    }
    rc = zip_part(&z, sheet_part, &part, &pn, err, cap);
    if (rc != 0) { if (rc > 0) rc = ferr(err, cap, YPAK_ERR_FORMAT, "the sheet's part '%s' is missing", sheet_part); goto done; }
    rc = read_sheet(part, pn, &ss, is_date, n_styles, &sh, err, cap);
    free(part);
    part = NULL;
    if (rc < 0) goto done;
    rc = sheet_csv(&sh, out, err, cap);
    out->formulas = sh.formulas;
done:
    free(part);
    free(rels);
    free(ss.text.p);
    free(ss.off);
    free(ss.len);
    free(sh.c);
    free(sh.text.p);
    free(is_date);
    zip_close(&z);
    if (rc < 0) {
        char sheet_name[256];
        snprintf(sheet_name, sizeof sheet_name, "%s", out->sheet);
        ypt_sheet_free(out);
        snprintf(out->sheet, sizeof out->sheet, "%s", sheet_name);
    }
    return rc < 0 ? rc : 0;
}

/* ======================================================================== */
/* ICC profiles                                                              */
/* ======================================================================== */

static double s15(const uint8_t* p) { return (double)(int32_t)rbe32(p) / 65536.0; }

static double srgb_eotf(double x) { return x <= 0.04045 ? x / 12.92 : pow((x + 0.055) / 1.055, 2.4); }

/* A TRC tag (curv or para) at 256 codes against sRGB's; the largest
 * difference, or -1 for a tag this check does not read. */
static double trc_err(const uint8_t* t, size_t n) {
    double worst = 0;
    int k;
    if (n < 12) return -1;
    if (!memcmp(t, "curv", 4)) {
        uint32_t cnt = rbe32(t + 8);
        if (cnt < 2 || (size_t)cnt > (n - 12) / 2) return -1;   /* identity or a pure gamma: not sRGB */
        for (k = 0; k < 256; k++) {
            double x = k / 255.0, p = x * (cnt - 1), y;
            uint32_t j = (uint32_t)p;
            if (j > cnt - 2) j = cnt - 2;
            y = (rbe16(t + 12 + 2 * j) + (rbe16(t + 14 + 2 * j) - (double)rbe16(t + 12 + 2 * j)) * (p - j)) / 65535.0;
            if (fabs(y - srgb_eotf(x)) > worst) worst = fabs(y - srgb_eotf(x));
        }
        return worst;
    }
    if (!memcmp(t, "para", 4)) {
        uint32_t fn = rbe16(t + 8);
        static const int np[5] = { 1, 3, 4, 5, 7 };
        double g, a = 1, b = 0, c = 0, d = 0, e = 0, f = 0;
        if (fn > 4 || n < 12 + 4 * (size_t)np[fn]) return -1;
        g = s15(t + 12);
        if (fn >= 1) { a = s15(t + 16); b = s15(t + 20); }
        if (fn == 2) { c = s15(t + 24); }
        if (fn >= 3) { c = s15(t + 24); d = s15(t + 28); }
        if (fn == 4) { e = s15(t + 32); f = s15(t + 36); }
        for (k = 0; k < 256; k++) {
            double x = k / 255.0, y;
            if (fn == 0) y = pow(x, g);
            else if (fn == 1) y = x >= -b / a ? pow(a * x + b, g) : 0;
            else if (fn == 2) y = x >= -b / a ? pow(a * x + b, g) + c : c;
            else if (fn == 3) y = x >= d ? pow(a * x + b, g) : c * x;
            else y = x >= d ? pow(a * x + b, g) + e : c * x + f;
            if (fabs(y - srgb_eotf(x)) > worst) worst = fabs(y - srgb_eotf(x));
        }
        return worst;
    }
    return -1;
}

int ypt_icc_srgb(const void* data, size_t n, char* why, size_t cap) {
    /* sRGB's colorants adapted to D50 (the ICC's connection space), as
     * every sRGB profile writes them */
    static const double want[3][3] = { { 0.4361, 0.2225, 0.0139 }, { 0.3851, 0.7169, 0.0971 }, { 0.1431, 0.0606, 0.7141 } };
    static const char* const xyz[3] = { "rXYZ", "gXYZ", "bXYZ" };
    static const char* const trc[3] = { "rTRC", "gTRC", "bTRC" };
    const uint8_t* p = (const uint8_t*)data;
    uint32_t size, nt, i;
    int k, have = 0;
    if (why && cap) why[0] = 0;
    if (n < 132) return ferr(why, cap, YPAK_ERR_FORMAT, "an ICC profile of %zu bytes", n);
    size = rbe32(p);
    if (size > n || size < 132 || memcmp(p + 36, "acsp", 4)) return ferr(why, cap, YPAK_ERR_FORMAT, "a damaged ICC profile");
    if (memcmp(p + 16, "RGB ", 4)) return ferr(why, cap, 0, "an ICC profile for %.4s, not RGB", (const char*)p + 16);
    nt = rbe32(p + 128);
    if (nt > (size - 132) / 12) return ferr(why, cap, YPAK_ERR_FORMAT, "a damaged ICC profile (tag count)");
    for (k = 0; k < 6; k++) {
        const char* sig = k < 3 ? xyz[k] : trc[k - 3];
        int found = 0;
        for (i = 0; i < nt; i++) {
            const uint8_t* r = p + 132 + 12 * i;
            uint32_t off = rbe32(r + 4), sz = rbe32(r + 8);
            if (memcmp(r, sig, 4)) continue;
            if (off > size || sz > size - off) return ferr(why, cap, YPAK_ERR_FORMAT, "a damaged ICC profile (tag %s)", sig);
            found = 1;
            if (k < 3) {
                int m;
                if (sz < 20 || memcmp(p + off, "XYZ ", 4)) return ferr(why, cap, 0, "an ICC profile whose %s is not an XYZ value", sig);
                for (m = 0; m < 3; m++)
                    if (fabs(s15(p + off + 8 + 4 * m) - want[k][m]) > 0.002)
                        return ferr(why, cap, 0, "an ICC profile whose primaries are not sRGB's (%s %.4f %.4f %.4f)", sig, s15(p + off + 8),
                                    s15(p + off + 12), s15(p + off + 16));
            } else {
                double e = trc_err(p + off, sz);
                if (e < 0) return ferr(why, cap, 0, "an ICC profile whose %s is not sRGB's curve (an identity, a pure gamma, or a form this check does not read)", sig);
                if (e > 0.5 / 255.0) return ferr(why, cap, 0, "an ICC profile whose %s is %.2f codes of 255 from sRGB's curve", sig, e * 255.0);
            }
            break;
        }
        if (!found) return ferr(why, cap, 0, "an ICC profile without %s (not a matrix and curves RGB profile)", sig);
        have++;
    }
    return have == 6;
}

/* ======================================================================== */
/* JPEG                                                                      */
/* ======================================================================== */

void ypt_jpeg_free(ypt_jpeg* j) {
    if (!j) return;
    if (j->px) stbi_image_free(j->px);
    memset(j, 0, sizeof *j);
}

/* EXIF IFD0's orientation (0x0112), or 0 when absent. < 0 when damaged. */
static int exif_orientation(const uint8_t* t, size_t n) {
    int le;
    uint32_t off, cnt, k;
    if (n < 8) return -1;
    if (!memcmp(t, "II*\0", 4)) le = 1;
    else if (!memcmp(t, "MM\0*", 4)) le = 0;
    else return -1;
    off = le ? rle32(t + 4) : rbe32(t + 4);
    if (off > n || n - off < 2) return -1;
    cnt = le ? rle16(t + off) : rbe16(t + off);
    if ((size_t)cnt > (n - off - 2) / 12) return -1;
    for (k = 0; k < cnt; k++) {
        const uint8_t* e = t + off + 2 + 12 * k;
        uint32_t tag = le ? rle16(e) : rbe16(e), type = le ? rle16(e + 2) : rbe16(e + 2);
        if (tag != 0x0112) continue;
        if (type != 3) return -1;
        return (int)(le ? rle16(e + 8) : rbe16(e + 8));
    }
    return 0;
}

int ypt_jpeg_read(const void* data, size_t n, int decode, ypt_jpeg* out, char* err, size_t cap) {
    const uint8_t* d = (const uint8_t*)data;
    size_t at = 2;
    uint8_t* icc = NULL;
    size_t icc_n = 0;
    int icc_parts = 0, icc_seen = 0, sof = 0, rc = 0, w = 0, h = 0, comp = 0;
    uint8_t icc_have[256];
    if (err && cap) err[0] = 0;
    memset(out, 0, sizeof *out);
    memset(icc_have, 0, sizeof icc_have);
    if (n < 4 || d[0] != 0xFF || d[1] != 0xD8) return ferr(err, cap, YPAK_ERR_FORMAT, "not a JPEG file (no SOI marker)");
    if (n > (size_t)0x7FFFFFFF) return ferr(err, cap, YPAK_ERR_FORMAT, "a JPEG file of 2 GiB or more");
    for (;;) {
        uint32_t m, len;
        const uint8_t* s;
        if (at + 4 > n) { rc = ferr(err, cap, YPAK_ERR_FORMAT, "the JPEG ends before its image data"); break; }
        if (d[at] != 0xFF) { rc = ferr(err, cap, YPAK_ERR_FORMAT, "JPEG: no marker at byte %zu", at); break; }
        while (at + 1 < n && d[at + 1] == 0xFF) at++;   /* fill bytes */
        if (at + 4 > n) { rc = ferr(err, cap, YPAK_ERR_FORMAT, "the JPEG ends before its image data"); break; }
        m = d[at + 1];
        if (m == 0xD8 || m == 0xD9 || (m >= 0xD0 && m <= 0xD7) || m == 0x01) { rc = ferr(err, cap, YPAK_ERR_FORMAT, "JPEG: marker 0x%02X before the image data", m); break; }
        len = rbe16(d + at + 2);
        if (len < 2 || at + 2 + len > n) { rc = ferr(err, cap, YPAK_ERR_FORMAT, "JPEG: segment 0x%02X runs past the end", m); break; }
        s = d + at + 4;
        len -= 2;
        if (m == 0xDA) {
            size_t k;
            int eoi = 0;
            if (!sof) { rc = ferr(err, cap, YPAK_ERR_FORMAT, "JPEG: image data before the frame header"); break; }
            /* stb_image pads a cut stream with zeros and decodes it, so a
             * file cut short is found here: in entropy data 0xFF is always
             * stuffed, so FF D9 is the end marker */
            for (k = at + 2; k + 1 < n && !eoi; k++) eoi = d[k] == 0xFF && d[k + 1] == 0xD9;
            if (!eoi) rc = ferr(err, cap, YPAK_ERR_FORMAT, "the JPEG has no end marker (EOI): the file is cut short");
            break;
        }
        if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
            if (sof) { rc = ferr(err, cap, YPAK_ERR_FORMAT, "JPEG: two frame headers"); break; }
            if (m != 0xC0 && m != 0xC1 && m != 0xC2) {
                rc = ferr(err, cap, YPAK_ERR_FORMAT, "JPEG: SOF%u (%s); baseline, extended sequential and progressive Huffman JPEGs are read: "
                          "save it again as an ordinary JPEG", m - 0xC0, m == 0xC3 ? "lossless" : (m & 8) ? "arithmetic coding" : "hierarchical");
                break;
            }
            if (len < 6) { rc = ferr(err, cap, YPAK_ERR_FORMAT, "JPEG: a short frame header"); break; }
            if (s[0] != 8) { rc = ferr(err, cap, YPAK_ERR_FORMAT, "JPEG: %u-bit samples; 8-bit only", s[0]); break; }
            h = (int)rbe16(s + 1);
            w = (int)rbe16(s + 3);
            comp = s[5];
            if (!w || !h) { rc = ferr(err, cap, YPAK_ERR_FORMAT, "JPEG: no size in the frame header (a DNL marker)"); break; }
            if (comp == 4) { rc = ferr(err, cap, YPAK_ERR_FORMAT, "a CMYK JPEG; ysp converts no color: export it as RGB"); break; }
            if (comp != 1 && comp != 3) { rc = ferr(err, cap, YPAK_ERR_FORMAT, "JPEG: %d components (1 or 3)", comp); break; }
            if (len < 6 + 3 * (uint32_t)comp) { rc = ferr(err, cap, YPAK_ERR_FORMAT, "JPEG: a short frame header"); break; }
            if (comp == 1) snprintf(out->sampling, sizeof out->sampling, "gray");
            else snprintf(out->sampling, sizeof out->sampling, "%dx%d,%dx%d,%dx%d", s[7] >> 4, s[7] & 15, s[10] >> 4, s[10] & 15, s[13] >> 4, s[13] & 15);
            out->progressive = m == 0xC2;
            sof = 1;
        } else if (m == 0xE1 && len >= 6 && !memcmp(s, "Exif\0\0", 6)) {
            int o = exif_orientation(s + 6, len - 6);
            if (o < 0) { rc = ferr(err, cap, YPAK_ERR_FORMAT, "JPEG: a damaged EXIF block"); break; }
            if (o > 1) {
                rc = ferr(err, cap, YPAK_ERR_FORMAT, "the JPEG's EXIF orientation is %d: the picture is stored turned or mirrored, and programs "
                          "disagree on whether to apply it. Save it with the pixels turned and orientation 1", o);
                break;
            }
        } else if (m == 0xE2 && len >= 14 && !memcmp(s, "ICC_PROFILE\0", 12)) {
            uint32_t seq = s[12], cnt = s[13];
            uint8_t* grow;
            if (!seq || !cnt || seq > cnt || (icc_parts && (uint32_t)icc_parts != cnt) || icc_have[seq]) { rc = ferr(err, cap, YPAK_ERR_FORMAT, "JPEG: damaged ICC profile chunks"); break; }
            icc_parts = (int)cnt;
            icc_have[seq] = 1;
            icc_seen++;
            /* chunks are concatenated in sequence order; writers emit them in order */
            if (seq != (uint32_t)icc_seen) { rc = ferr(err, cap, YPAK_ERR_FORMAT, "JPEG: ICC profile chunks out of order"); break; }
            grow = (uint8_t*)realloc(icc, icc_n + len - 14 + 1);
            if (!grow) { rc = ferr(err, cap, YPAK_ERR_NOMEM, "out of memory"); break; }
            icc = grow;
            memcpy(icc + icc_n, s + 14, len - 14);
            icc_n += len - 14;
        }
        at += 2 + 2 + len;
    }
    if (rc == 0 && icc_parts && icc_seen != icc_parts) rc = ferr(err, cap, YPAK_ERR_FORMAT, "JPEG: %d of %d ICC profile chunks", icc_seen, icc_parts);
    if (rc == 0 && icc) {
        char why[200];
        int s = ypt_icc_srgb(icc, icc_n, why, sizeof why);
        if (s <= 0) rc = ferr(err, cap, YPAK_ERR_FORMAT, "the JPEG has %s; ysp converts no color: export it without a profile, or as sRGB", why);
        else out->icc_srgb = 1;
    }
    free(icc);
    if (rc < 0) return rc;
    if (w > 65536 || h > 65536) return ferr(err, cap, YPAK_ERR_FORMAT, "JPEG: %d x %d; a texture is at most 65536 on a side", w, h);
    out->w = w;
    out->h = h;
    out->components = comp;
    out->channels = comp == 1 ? 1 : 4;
    if (!decode) return 0;
    {
        int x = 0, y = 0, c = 0;
        out->px = stbi_load_from_memory(d, (int)n, &x, &y, &c, out->channels);
        if (!out->px) return ferr(err, cap, YPAK_ERR_FORMAT, "JPEG decode: %s", stbi_failure_reason() ? stbi_failure_reason() : "failed");
        if (x != w || y != h || c != comp) { ypt_jpeg_free(out); return ferr(err, cap, YPAK_ERR_FORMAT, "JPEG decode: the decoder read another image than the header says"); }
    }
    return 0;
}
