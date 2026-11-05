/* param_table_check.h - the checks that the parameter tables of the ysp
 * headers share. Every table has one row shape (name, type, min, max, def,
 * unit, doc, values); a test copies each row into a ptc_row and gives the
 * list of fields it expects, by offset, so a row is checked against a real
 * field and not against its own text.
 *
 * The adaptive headers (yst_params(), yqst_params(), yaep_params()) use
 * ptc_field and ptc_check_row(), whose enums run 0, 1, 2, ... The rig
 * headers (audio, screen, color, outline, video, rdk, gfx's desc) use
 * ptc_slot and ptc_check_slots(), which take any field width. Both check
 * an enum row's values against the test's own names of the C enum.
 * ptc_values_parse() is the rule for the values of every table, and
 * tests/adapt/param_tables_test.c applies it to all of them.
 *
 * Included by the tests after their header. The caller supplies
 * ptc_fail(line, message).
 */
#ifndef YSP_PARAM_TABLE_CHECK_H
#define YSP_PARAM_TABLE_CHECK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

typedef struct ptc_row {
    const char *name, *type;
    double      min, max, def;
    const char *unit, *doc, *values;
} ptc_row;

#define PTC_ROW(r, p) do { (r).name = (p).name; (r).type = (p).type; (r).min = (p).min; \
    (r).max = (p).max; (r).def = (p).def; (r).unit = (p).unit; (r).doc = (p).doc;     \
    (r).values = (p).values; } while (0)

/* A desc field the test knows. kind: 'd' double, 'i' int, 'u' unsigned, 'b'
 * bool, 'e' a C enum, and 'D', 'I', 'E' arrays of double, int and enum.
 * n_enum is the enum's value count, from its last constant, and values the
 * row's text as the test writes it from the C enum's constants. */
typedef struct ptc_field {
    const char* name;
    size_t      off;
    char        kind;
    int         n_enum;
    const char* values;
} ptc_field;

static void ptc_fail(int line, const char* what);

/* Inline so that a test which does not call a helper gets no unused-function
 * warning (gcc -Wunused-function, MSVC C4505); MSVC's C before C11 spells
 * it __inline. */
#if defined(_MSC_VER) && !defined(__cplusplus)
#define PTC_INLINE static __inline
#else
#define PTC_INLINE static inline
#endif

PTC_INLINE const char* ptc_type_of(char kind) {
    switch (kind) {
    case 'd': return "f64";
    case 'i': return "i32";
    case 'u': return "u32";
    case 'b': return "bool";
    case 'e': return "enum";
    case 'D': return "f64[]";
    case 'I': return "i32[]";
    case 'E': return "enum[]";
    default:  return "?";
    }
}

PTC_INLINE int ptc_find(const ptc_field* f, int nf, const char* name) {
    int i;
    for (i = 0; i < nf; i++)
        if (name && strcmp(f[i].name, name) == 0) return i;
    return -1;
}

PTC_INLINE int ptc_required(const ptc_row* r) {
    return r->doc && strncmp(r->doc, "required", 8) == 0;
}

/* "name=0,name=1,...": names in [a-z0-9_], values 0, 1, 2, ... in order.
 * Returns the count, or -1 when the text breaks the rule. */
PTC_INLINE int ptc_enum_count(const char* v) {
    int k = 0;
    if (!v || !*v) return -1;
    for (;;) {
        const char* p = v;
        long n = 0;
        int digits = 0;
        while ((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_') p++;
        if (p == v || *p != '=') return -1;
        p++;
        while (*p >= '0' && *p <= '9') { n = n * 10 + (*p - '0'); p++; digits++; }
        if (digits == 0 || n != k) return -1;
        k++;
        if (*p == '\0') return k;
        if (*p != ',') return -1;
        v = p + 1;
    }
}

/* Structure: a real field, the type of that field, text present, the range
 * ordered, the default inside it unless the field is required, and an
 * enum's values listing every value of the C enum. Returns the field's
 * index or -1. */
PTC_INLINE int ptc_check_row(const ptc_row* r, const ptc_field* f, int nf) {
    char msg[256];
    int i = ptc_find(f, nf, r->name);
    if (i < 0) {
        snprintf(msg, sizeof msg, "row %s names no desc field", r->name ? r->name : "(null)");
        ptc_fail(__LINE__, msg);
        return -1;
    }
    if (!r->type || strcmp(r->type, ptc_type_of(f[i].kind)) != 0) {
        snprintf(msg, sizeof msg, "row %s: type %s, the field is %s", r->name,
                 r->type ? r->type : "(null)", ptc_type_of(f[i].kind));
        ptc_fail(__LINE__, msg);
    }
    if (!r->unit || !r->doc || !r->doc[0]) {
        snprintf(msg, sizeof msg, "row %s: unit or doc missing", r->name);
        ptc_fail(__LINE__, msg);
    }
    if (!(r->min <= r->max) || (!ptc_required(r) && !(r->min <= r->def && r->def <= r->max))) {
        snprintf(msg, sizeof msg, "row %s: min %g, def %g, max %g", r->name, r->min, r->def, r->max);
        ptc_fail(__LINE__, msg);
    }
    if (f[i].kind == 'e' || f[i].kind == 'E') {
        int k = ptc_enum_count(r->values);
        if (k != f[i].n_enum || r->min != 0.0 || r->max != (double)(f[i].n_enum - 1) ||
            !f[i].values || strcmp(r->values, f[i].values) != 0) {
            snprintf(msg, sizeof msg, "row %s: values \"%s\" list %d of %d, range %g..%g",
                     r->name, r->values ? r->values : "(null)", k, f[i].n_enum, r->min, r->max);
            ptc_fail(__LINE__, msg);
        }
    } else if (r->values) {
        snprintf(msg, sizeof msg, "row %s: values set on a %s", r->name, r->type);
        ptc_fail(__LINE__, msg);
    }
    return i;
}

/* Every field once: no row twice, and no field the test lists without a row. */
PTC_INLINE void ptc_check_cover(const ptc_row* rows, int n, const ptc_field* f, int nf) {
    char msg[160];
    int i, j, seen;
    for (i = 0; i < n; i++)
        for (j = 0; j < i; j++)
            if (rows[i].name && rows[j].name && strcmp(rows[i].name, rows[j].name) == 0) {
                snprintf(msg, sizeof msg, "row %s twice", rows[i].name);
                ptc_fail(__LINE__, msg);
            }
    for (j = 0; j < nf; j++) {
        seen = 0;
        for (i = 0; i < n; i++)
            if (rows[i].name && strcmp(rows[i].name, f[j].name) == 0) seen++;
        if (seen != 1) {
            snprintf(msg, sizeof msg, "field %s has %d rows", f[j].name, seen);
            ptc_fail(__LINE__, msg);
        }
    }
}

/* A scalar field as a double, and back. */
PTC_INLINE double ptc_get(const void* base, const ptc_field* f) {
    const unsigned char* p = (const unsigned char*)base + f->off;
    double d;
    int i;
    unsigned u;
    bool b;
    switch (f->kind) {
    case 'd': memcpy(&d, p, sizeof d); return d;
    case 'i': case 'e': memcpy(&i, p, sizeof i); return (double)i;
    case 'u': memcpy(&u, p, sizeof u); return (double)u;
    case 'b': memcpy(&b, p, sizeof b); return b ? 1.0 : 0.0;
    default: return 0.0;
    }
}

PTC_INLINE void ptc_set(void* base, const ptc_field* f, double v) {
    unsigned char* p = (unsigned char*)base + f->off;
    int i = (int)v;
    unsigned u = (unsigned)v;
    bool b = v != 0.0;
    switch (f->kind) {
    case 'd': memcpy(p, &v, sizeof v); break;
    case 'i': case 'e': memcpy(p, &i, sizeof i); break;
    case 'u': memcpy(p, &u, sizeof u); break;
    case 'b': memcpy(p, &b, sizeof b); break;
    default: break;
    }
}

PTC_INLINE int ptc_scalar(const ptc_field* f) {
    return f->kind == 'd' || f->kind == 'i' || f->kind == 'u' || f->kind == 'b' || f->kind == 'e';
}

/* --- the values text, for every table ------------------------------------ */

/* "name=value,name=value,...": a name is [a-z][a-z0-9_]* (never a number,
 * so a loader can tell a name from a value), no name twice, the values
 * integers in increasing order. v gets up to cap values. Returns the
 * count, or -1 and *why when the text breaks the rule. */
PTC_INLINE int ptc_values_parse(const char* text, long* v, int cap, const char** why) {
    const char* s = text;
    int k = 0;
    long prev = 0;
    *why = "";
    if (!s || !*s) { *why = "empty"; return -1; }
    for (;;) {
        const char* name = s;
        const char* q;
        size_t len;
        long n = 0;
        int neg = 0, digits = 0;
        if (!(*s >= 'a' && *s <= 'z')) { *why = "a name starts with a lower-case letter"; return -1; }
        while ((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '_') s++;
        len = (size_t)(s - name);
        if (*s != '=') { *why = "a name is [a-z][a-z0-9_]* and then '='"; return -1; }
        s++;
        if (*s == '-') { neg = 1; s++; }
        while (*s >= '0' && *s <= '9') { n = n * 10 + (*s - '0'); s++; digits++; }
        if (!digits || digits > 9) { *why = "a value is an integer"; return -1; }
        if (neg) n = -n;
        if (k > 0 && n <= prev) { *why = "the values are not increasing"; return -1; }
        /* no name twice: the earlier pairs are well formed, so walk them */
        for (q = text; q < name;) {
            const char* e = q;
            while (*e != '=') e++;
            if ((size_t)(e - q) == len && memcmp(q, name, len) == 0) { *why = "a name twice"; return -1; }
            while (*e != ',') e++;
            q = e + 1;
        }
        if (k < cap) v[k] = n;
        prev = n;
        k++;
        if (*s == '\0') return k;
        if (*s != ',') { *why = "pairs are joined by ','"; return -1; }
        s++;
    }
}

/* The text is name_of's enum: every value 0..63 that name_of names is
 * listed under that name, and nothing else is. name_of is a switch on the
 * C enum without a default, so gcc and clang -Wall -Werror stop the build
 * when the enum gains a value. */
PTC_INLINE int ptc_values_are(const char* text, const char* (*name_of)(int)) {
    long v[64];
    const char* why;
    const char* s = text;
    int k = ptc_values_parse(text, v, 64, &why), i, want = 0;
    if (k < 0 || k > 64) return 0;
    for (i = 0; i < 64; i++) want += name_of(i) != NULL;
    if (want != k) return 0;
    for (i = 0; i < k; i++) {
        const char* eq = strchr(s, '=');
        const char* nm = v[i] >= 0 && v[i] < 64 ? name_of((int)v[i]) : NULL;
        if (!nm || strlen(nm) != (size_t)(eq - s) || memcmp(nm, s, (size_t)(eq - s)) != 0) return 0;
        s = strchr(eq, ',');
        s = s ? s + 1 : eq;
    }
    return 1;
}

/* --- rows of any field width (the rig headers) --------------------------- */

/* A field the test knows: the row name, the field's place in its struct,
 * and for an enum the test's names of the C enum's values. */
typedef struct ptc_slot {
    const char* name;
    size_t      off, size;
    const char* (*name_of)(int);
} ptc_slot;

/* Whether a field of size bytes holds the type; an enum is a C enum or an
 * integer field that holds one: 1, 2 or 4 bytes. */
PTC_INLINE int ptc_type_fits(const char* type, size_t size) {
    if (!type) return 0;
    if (!strcmp(type, "bool")) return size == sizeof(bool);
    if (!strcmp(type, "u8") || !strcmp(type, "i8")) return size == 1;
    if (!strcmp(type, "u16") || !strcmp(type, "i16")) return size == 2;
    if (!strcmp(type, "u32") || !strcmp(type, "i32") || !strcmp(type, "f32")) return size == 4;
    if (!strcmp(type, "u64") || !strcmp(type, "i64") || !strcmp(type, "f64")) return size == 8;
    if (!strcmp(type, "enum")) return size == 1 || size == 2 || size == 4;
    if (!strcmp(type, "str")) return size == sizeof(const char*);
    return 0;
}

/* Every row names one field the test lists, of the row's type, with its
 * text and an ordered range around the default; it has values exactly when
 * it is an enum, and then they are the C enum's, with min and max the
 * first and last value. Every field has one row. */
PTC_INLINE void ptc_check_slots(const ptc_row* rows, int n, const ptc_slot* f, int nf) {
    char msg[256];
    int i, j, seen;
    for (i = 0; i < n; i++) {
        const ptc_row* r = &rows[i];
        const ptc_slot* s = NULL;
        int is_enum = r->type && strcmp(r->type, "enum") == 0;
        for (j = 0; j < nf; j++)
            if (r->name && strcmp(f[j].name, r->name) == 0) s = &f[j];
        if (!s) {
            snprintf(msg, sizeof msg, "row %s names no field", r->name ? r->name : "(null)");
            ptc_fail(__LINE__, msg);
            continue;
        }
        if (!ptc_type_fits(r->type, s->size)) {
            snprintf(msg, sizeof msg, "row %s: type %s, the field has %d bytes", r->name,
                     r->type ? r->type : "(null)", (int)s->size);
            ptc_fail(__LINE__, msg);
        }
        if (!r->unit || !r->doc || !r->doc[0]) {
            snprintf(msg, sizeof msg, "row %s: unit or doc missing", r->name);
            ptc_fail(__LINE__, msg);
        }
        if (!(r->min <= r->def && r->def <= r->max)) {
            snprintf(msg, sizeof msg, "row %s: min %g, def %g, max %g", r->name, r->min, r->def, r->max);
            ptc_fail(__LINE__, msg);
        }
        if (is_enum != (s->name_of != NULL)) {
            snprintf(msg, sizeof msg, "row %s: type %s, but the test %s the field an enum", r->name,
                     r->type ? r->type : "(null)", s->name_of ? "calls" : "does not call");
            ptc_fail(__LINE__, msg);
        } else if (is_enum) {
            long v[64];
            const char* why;
            int k = ptc_values_parse(r->values, v, 64, &why);
            if (k < 1 || k > 64 || !ptc_values_are(r->values, s->name_of) || r->min != (double)v[0] ||
                r->max != (double)v[k - 1]) {
                snprintf(msg, sizeof msg, "row %s: values \"%s\" are not its enum's, or min %g and max %g are not their ends",
                         r->name, r->values ? r->values : "(null)", r->min, r->max);
                ptc_fail(__LINE__, msg);
            }
        } else if (r->values) {
            snprintf(msg, sizeof msg, "row %s: values set on a %s", r->name, r->type ? r->type : "(null)");
            ptc_fail(__LINE__, msg);
        }
    }
    for (j = 0; j < nf; j++) {
        seen = 0;
        for (i = 0; i < n; i++)
            if (rows[i].name && strcmp(rows[i].name, f[j].name) == 0) seen++;
        if (seen != 1) {
            snprintf(msg, sizeof msg, "field %s has %d rows", f[j].name, seen);
            ptc_fail(__LINE__, msg);
        }
    }
}

/* A field as a double, read by the row's type: what open made of a zero
 * field. */
PTC_INLINE double ptc_read(const void* base, const ptc_slot* s, const char* type) {
    const unsigned char* b = (const unsigned char*)base + s->off;
    if (!strcmp(type, "f32")) { float x; memcpy(&x, b, 4); return x; }
    if (!strcmp(type, "f64")) { double x; memcpy(&x, b, 8); return x; }
    if (!strcmp(type, "bool")) { bool x; memcpy(&x, b, sizeof x); return x ? 1.0 : 0.0; }
    if (s->size == 1) return *b;
    if (s->size == 2) { unsigned short x; memcpy(&x, b, 2); return x; }
    if (s->size == 4) {
        if (!strcmp(type, "u32")) { unsigned int x; memcpy(&x, b, 4); return x; }
        { int x; memcpy(&x, b, 4); return x; }
    }
    if (!strcmp(type, "u64")) { unsigned long long x; memcpy(&x, b, 8); return (double)x; }
    { long long x; memcpy(&x, b, 8); return (double)x; }
}

#endif
