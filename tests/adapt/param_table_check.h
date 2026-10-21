/* param_table_check.h - the checks that the parameter tables of ysp/stair.h,
 * ysp/quest.h and ysp/aep.h share (yst_params(), yqst_params(),
 * yaep_params()). The three tables have one shape; a test copies each row
 * into a ptc_row and gives the list of desc fields it expects, by offset, so
 * a row is checked against a real field and not against its own text.
 *
 * Included by tests/adapt/stair_test.c, quest_test.c and aep_test.c after
 * their header. The caller supplies ptc_fail(line, message).
 */
#ifndef YSP_PARAM_TABLE_CHECK_H
#define YSP_PARAM_TABLE_CHECK_H

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

#endif
