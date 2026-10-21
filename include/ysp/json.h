/* ysp/json.h - v0.2.0 - public domain single-header strict JSON reader and canonical writer
 *
 *   One JSON reader for every ysp program that reads JSON at run time or in
 *   a tool: the rig profile (ysp/rigfile.h), the pack tool's source
 *   descriptions and manifests (pack/pack_tool.c), and later the
 *   experiment definition if its format is JSON. A strict RFC 8259 reader
 *   that refuses what a lenient one guesses at (comments, trailing commas,
 *   duplicate keys, invalid UTF-8, lone surrogates), with a depth and a
 *   size limit, an error with its line and column, numbers kept as their
 *   text with exact int64, fixed-point and correctly rounded double
 *   accessors, and a writer with one canonical form (keys in bytewise
 *   order, a fixed layout) so that the same value gives the same bytes and
 *   the same hash on every compiler.
 *
 *   Pure computation: no OS calls, no threads, no locale. The values live
 *   in a yjs_arena: the caller's memory only, or blocks from malloc up to a
 *   stated limit. Needs only the C standard library (memcpy, malloc,
 *   qsort, snprintf, ldexp). C99 is the floor: it builds as C99, C11 and
 *   C++17.
 *
 *   ---------------------------------------------------------------------
 *   USAGE
 *   ---------------------------------------------------------------------
 *       #define YSP_JSON_IMPLEMENTATION
 *       #include "ysp/json.h"
 *
 *       yjs_arena a;
 *       yjs_value* root;
 *       yjs_error e;
 *       int64_t n;
 *       yjs_arena_init_heap(&a, 0);                   // or yjs_arena_init(&a, buf, cap)
 *       if (yjs_parse(&a, text, len, NULL, &root, &e) != YJS_OK)
 *           printf("line %u, column %u: %s\n", e.line, e.column, e.msg);
 *       else if (yjs_int64(yjs_get(root, "count"), &n) == YJS_OK)
 *           ...
 *       yjs_arena_free(&a);                           // every value at once
 *
 *   ---------------------------------------------------------------------
 *   THE READER (yjs_parse)
 *   ---------------------------------------------------------------------
 *   RFC 8259 and nothing more: one value, optionally after a UTF-8 byte
 *   order mark, whitespace (space, tab, LF, CR) around tokens. Refused,
 *   each with its own message: a comment, a trailing comma, a single
 *   quote, NaN or Infinity, a leading zero or '+', a control character in
 *   a string, an unknown escape, a lone or reversed surrogate, invalid
 *   UTF-8 (overlong forms, surrogates, above U+10FFFF), \u0000 (so every
 *   string is also a C string), a duplicate key in one object, text after
 *   the value, a nonzero number whose first significant digit is past
 *   10^999999999 or below 10^-999999999 (a limit RFC 8259 section 9
 *   allows; it keeps a canonical exponent to 9 digits, and a double needs
 *   3), nesting deeper than opts.max_depth (64), input longer than
 *   opts.max_bytes (64 MiB; never more than 4 GiB - 1, since positions are
 *   32-bit). The result is YJS_OK or a YJS_ERR_* code and, in the error,
 *   the byte offset, the line and the column (both from 1; the column
 *   counts bytes) and a message. A failed parse leaves its allocations in
 *   the arena; free or reset the arena.
 *   Cost: one pass over the text; each string and number is copied into
 *   the arena once, NUL-terminated. About 32 bytes per value, 8 per array
 *   item, 24 per object member, plus the text of strings and numbers. The
 *   parser recurses once per level of nesting, so max_depth bounds its
 *   stack; the children of open containers wait on a scratch stack (from
 *   the heap, or from the top of a fixed arena).
 *
 *   ---------------------------------------------------------------------
 *   VALUES
 *   ---------------------------------------------------------------------
 *   A yjs_value is 32 bytes: type, number flags, n (bytes of a string or
 *   number, items of an array, members of an object), pos (its byte offset
 *   in the parsed text, 0 for a built value), and u: the NUL-terminated
 *   text, the item pointers, or the members.
 *   OBJECTS are kept sorted by key, bytewise (memcmp, a shorter key before
 *   its extensions): the reader sorts each object once when it closes,
 *   which also finds duplicates in O(n log n), and the builder inserts in
 *   order. So lookups are binary searches and the writer needs no sort.
 *   The order of the source text is not kept; pos and each member's
 *   key_pos still point into it for messages.
 *   NUMBERS keep their text exactly as written; the writer respells it
 *   (CANONICAL NUMBERS). flags has YJS_NUM_INT
 *   when the text has no fraction and no exponent, and YJS_NUM_I64 when it
 *   is also within int64, with the value in i. yjs_int64() takes integer
 *   text only ("1.0" and "1e3" are YJS_ERR_TYPE); yjs_double() converts
 *   any number, correctly rounded, ties to even, with no locale (the
 *   conversion of ysp/table.h's ytb_parse_number(): at most 19
 *   significant digits, else YJS_ERR_RANGE, as is a value that rounds
 *   past the largest double or a nonzero value that rounds to 0);
 *   yjs_fixed(v, d) gives value x 10^d as an exact int64, rounded to the
 *   nearest, ties to even, for durations in seconds read as nanoseconds
 *   (d = 9) without a binary fraction in between.
 *   STRINGS are valid UTF-8 without NUL, decoded, NUL-terminated.
 *
 *   ---------------------------------------------------------------------
 *   THE BUILDER
 *   ---------------------------------------------------------------------
 *   yjs_new() (null, booleans, empty containers), yjs_new_int(),
 *   yjs_new_fixed() (exact decimal text of v / 10^d, trailing zeros
 *   dropped: 1500000 with d = 9 is "0.0015"), yjs_new_double() (the
 *   double's canonical text, yjs_format_double(); NULL for NaN and
 *   infinity), yjs_new_number() (text checked against the grammar),
 *   yjs_new_string() (checked UTF-8, no NUL), yjs_push(), yjs_set()
 *   (replaces the value of an existing key), yjs_copy() (deep). Each
 *   returns NULL or a YJS_ERR_* on a refusal or a full arena. Values are
 *   pointers, so one value may sit in two containers; a value must not
 *   contain itself (the writer stops at depth 1024 with YJS_ERR_LIMIT).
 *   A container that grows gets a new array in the arena; the old one
 *   stays there until the arena is freed.
 *
 *   ---------------------------------------------------------------------
 *   THE WRITER (canonical form)
 *   ---------------------------------------------------------------------
 *   yjs_write() calls fn(ctx, bytes, n) with the text; yjs_write_mem()
 *   fills a buffer and returns the length it needs. YJS_WRITE_PRETTY, the
 *   canonical form (the pack manifest's, docs/pack.md 7, and the rig
 *   profile's): keys in bytewise order at every level, two spaces of
 *   indent, one member or item per line, ": " after a key, an array whose
 *   items are all scalars on one line with ", " between them, [] and {}
 *   when empty, LF only, no newline after the value. YJS_WRITE_COMPACT:
 *   the same order with no whitespace. Strings: '"' and '\' escaped with
 *   a backslash, other bytes below 0x20 as \u00XX (lowercase hex), all
 *   else as raw UTF-8. Numbers: each in its canonical spelling.
 *
 *   ---------------------------------------------------------------------
 *   CANONICAL NUMBERS
 *   ---------------------------------------------------------------------
 *   One spelling for each value, so that the same value gives the same
 *   bytes and hash. The writer writes a number's exact decimal value,
 *   never a rounded one: "1.50", "1.5e0", "15E-1" and "0.15e+1" are all
 *   written 1.5, and "-0", "-0.0" and "0e7" are 0. The spelling of a
 *   value D x 10^(E - k + 1) (D its k significant digits, no leading or
 *   trailing zero; E the exponent of the first digit):
 *     zero                   0 (no sign, no point)
 *     YJS_PLAIN_MIN <= E <= YJS_PLAIN_MAX (-18 and 20):
 *                            plain decimal, a '-' if negative, a '.' only
 *                            before fraction digits, "0." before a value
 *                            below 1: 100, 1.5, 0.0015, 0.000000001,
 *                            100000000000000000000
 *     else                   the first digit, '.' and the others if any,
 *                            'e', a '+' or '-', E without leading zeros:
 *                            1e+21, 1.5e-19, -5e-324
 *   So every int64 integer is plain decimal (as before), every
 *   yjs_new_fixed() text is unchanged (|value| >= 10^-18), and a double
 *   is written as JavaScript's String(x) writes it except from 10^-18 to
 *   below 10^-6, where JavaScript uses an exponent. Integer text
 *   stays integer text, and every accessor (yjs_int64() of integer text,
 *   yjs_double(), yjs_fixed()) reads the canonical text as it read the
 *   source. A value with more digits than a double holds keeps them: the
 *   writer does not round through a double.
 *   yjs_format_double(x) gives a double's canonical text: the shortest
 *   decimal that reads back to x (the nearest such, ties to even), so
 *   yjs_double() of it is x again. -0.0 is written 0. The digits come from
 *   Ryu (Ulf Adams, PLDI 2018) with small tables (780 bytes) and integer
 *   arithmetic only: no printf, no strtod, no FPU mode, so every compiler
 *   and platform gives the same bytes. The tables and a reference are in
 *   tests/adapt/json_dtoa_ref.py.
 *
 *   ---------------------------------------------------------------------
 *   THE ARENA
 *   ---------------------------------------------------------------------
 *   yjs_arena_init(a, buf, cap): the caller's memory and nothing else;
 *   a parse that does not fit is YJS_ERR_NOMEM, and arena.need tells how
 *   many bytes the failed request wanted beyond what was there.
 *   yjs_arena_init_heap(a, limit): blocks from YJS_MALLOC, 4 KB then
 *   doubling up to 1 MB (or one block as large as a request), at most
 *   limit bytes in all (0 = 256 MiB). yjs_arena_free() frees every block;
 *   yjs_arena_reset() keeps the first and empties it. One arena belongs
 *   to one thread at a time; values may be read from any thread once
 *   nothing writes.
 *
 *   ---------------------------------------------------------------------
 *   CHANGELOG
 *   ---------------------------------------------------------------------
 *   v0.2.0 - canonical numbers (docs/experiment_format.md, gap G7): the
 *          writer writes every number's exact value in one spelling
 *          (CANONICAL NUMBERS), not its source text; yjs_format_double()
 *          and yjs_new_double() give the shortest decimal that reads back
 *          (Ryu, integer only), not snprintf's. The reader refuses a
 *          number whose first digit is past 10^+-999999999. Changed bytes:
 *          a parsed number that was not already canonical ("1.50", "1E5",
 *          "-0.0"), and yjs_new_double() text (1e300 was "1e300", now
 *          "1e+300"; -5e-324 was "-4.94065645841247e-324"). yjs_new_int()
 *          and yjs_new_fixed() text and every int64 integer are unchanged.
 *          The shortest-double code is the RYU SECTION, under the Boost
 *          Software License 1.0 (the rest stays MIT-0).
 *   v0.1.1 - documentation only: checked against the repository
 *          conventions (README.md, Conventions), which it already met (the
 *          arena is init and _free, a parse writes its message into the
 *          caller's yjs_error, every failure is a negative YJS_ERR_*, the
 *          USAGE block builds as C99 and C++17). The CHANGELOG moved to the
 *          end of this comment. No code changed.
 *   v0.1.0 - first version: the reader and the writer of the pack tool
 *          (docs/pack.md 3) made a header, with numbers as text, the
 *          arena, sorted objects, the builder and the accessors.
 *
 *   STATUS: v0.2.0 (2026-10-21): 10 million doubles give the reference's
 *   text byte for byte on MSVC 19.44, MinGW-w64 gcc 16.1, WSL2 gcc 11.4
 *   (also -ffp-contract=fast) and emcc under node; about 69 ns a double.
 *   v0.1.1 (2026-10-10, MSVC 19.44 and MinGW-w64 gcc: the test
 *   passes unchanged); v0.1.0, 2026-10-09. docs/json.md has the measurements and the
 *   test counts.
 *
 *   ---------------------------------------------------------------------
 *   LICENSE: public domain / MIT-0, except the RYU SECTION (the shortest
 *   double, yjs_format_double()), which is under the Boost Software
 *   License 1.0; both texts are at the end of the file.
 */
#ifndef YSP_JSON_H_INCLUDED
#define YSP_JSON_H_INCLUDED

#define YJS_VERSION_MAJOR 0
#define YJS_VERSION_MINOR 2
#define YJS_VERSION_PATCH 0
#define YJS_VERSION_STRING "0.2.0"

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef YJS_API
#define YJS_API extern
#endif

typedef enum yjs_type {
    YJS_NULL   = 0,
    YJS_FALSE  = 1,
    YJS_TRUE   = 2,
    YJS_NUMBER = 3,
    YJS_STRING = 4,
    YJS_ARRAY  = 5,
    YJS_OBJECT = 6
} yjs_type;

/* yjs_value.flags of a number */
#define YJS_NUM_INT 0x01u   /* no fraction and no exponent                 */
#define YJS_NUM_I64 0x02u   /* and within int64: the value is in i         */

/* Results */
#define YJS_OK             0
#define YJS_ERR_SYNTAX    -1   /* not JSON                                  */
#define YJS_ERR_UTF8      -2   /* invalid UTF-8, a lone surrogate, NUL      */
#define YJS_ERR_DUPLICATE -3   /* a key twice in one object                 */
#define YJS_ERR_LIMIT     -4   /* deeper than max_depth, longer than max_bytes */
#define YJS_ERR_NOMEM     -5   /* the arena is full                         */
#define YJS_ERR_TYPE      -6   /* an accessor: not that type (or missing)   */
#define YJS_ERR_RANGE     -7   /* an accessor: does not fit                 */
#define YJS_ERR_ARG       -8   /* NULL or a bad argument                    */
#define YJS_ERR_WRITE     -9   /* the writer's fn returned nonzero          */

#define YJS_DEFAULT_DEPTH 64
#define YJS_MAX_DEPTH     1024
#define YJS_DEFAULT_BYTES ((size_t)64 << 20)

typedef struct yjs_value  yjs_value;
typedef struct yjs_member yjs_member;

struct yjs_value {
    uint8_t      type;       /* yjs_type                                    */
    uint8_t      flags;      /* YJS_NUM_*                                   */
    uint16_t     reserved_;
    uint32_t     n;          /* STRING, NUMBER: bytes (no NUL); ARRAY: items;
                              * OBJECT: members                             */
    uint32_t     cap;        /* ARRAY, OBJECT: slots allocated              */
    uint32_t     pos;        /* byte offset in the parsed text; 0 if built  */
    union {
        const char*  s;      /* STRING, NUMBER: NUL-terminated              */
        yjs_value**  items;  /* ARRAY                                       */
        yjs_member*  members;/* OBJECT, sorted by key                       */
    } u;
    int64_t      i;          /* NUMBER with YJS_NUM_I64                     */
};

struct yjs_member {
    const char*  key;        /* NUL-terminated                              */
    uint32_t     key_len;
    uint32_t     key_pos;    /* byte offset of the key's quote; 0 if built  */
    yjs_value*   value;
};

typedef struct yjs_block yjs_block;

typedef struct yjs_arena {
    unsigned char* p;        /* the current block's bytes                   */
    size_t       cap;        /* its size                                    */
    size_t       used;       /* taken from the bottom                       */
    size_t       top;        /* taken from the top (the parser's scratch)   */
    yjs_block*   heap;       /* heap blocks, newest first                   */
    size_t       heap_bytes; /* allocated from the heap                     */
    size_t       heap_limit; /* 0: no heap (the caller's buffer only)       */
    size_t       total;      /* bytes handed out, all blocks                */
    size_t       need;       /* after YJS_ERR_NOMEM: the failed request     */
    unsigned char* first;    /* the caller's buffer, for reset              */
    size_t       first_cap;
} yjs_arena;

typedef struct yjs_opts {
    uint32_t     max_depth;  /* 0 = 64; at most 1024                        */
    size_t       max_bytes;  /* 0 = 64 MiB                                  */
} yjs_opts;

typedef struct yjs_error {
    int          code;       /* YJS_OK or a YJS_ERR_*                       */
    uint32_t     offset;     /* byte offset of the fault                    */
    uint32_t     line, column;
    char         msg[96];
} yjs_error;

/* fn returns 0 to go on, nonzero to stop (YJS_ERR_WRITE). */
typedef int (*yjs_write_fn)(void* ctx, const char* bytes, size_t n);

#define YJS_WRITE_PRETTY  0   /* the canonical form                        */
#define YJS_WRITE_COMPACT 1   /* the same order, no whitespace             */

/* Canonical numbers: plain decimal when the first significant digit's
 * exponent E is in [YJS_PLAIN_MIN, YJS_PLAIN_MAX], else d.ddde+E / d.ddde-E. */
#define YJS_PLAIN_MIN   (-18)
#define YJS_PLAIN_MAX   20
#define YJS_DOUBLE_TEXT 40    /* bytes that always hold yjs_format_double()'s
                               * text (at most 37) and its NUL              */

YJS_API const char* yjs_version(void);

/* --- the arena -------------------------------------------------------------- */
YJS_API void   yjs_arena_init(yjs_arena* a, void* buf, size_t cap);
YJS_API void   yjs_arena_init_heap(yjs_arena* a, size_t limit);
YJS_API void   yjs_arena_reset(yjs_arena* a);
YJS_API void   yjs_arena_free(yjs_arena* a);
/* n bytes, 8-aligned, zeroed; NULL when full. */
YJS_API void*  yjs_alloc(yjs_arena* a, size_t n);

/* --- the reader ------------------------------------------------------------- */
YJS_API int    yjs_parse(yjs_arena* a, const char* text, size_t n, const yjs_opts* opts,
                         yjs_value** out, yjs_error* err);
/* The line and column (from 1, bytes) of offset pos in text. */
YJS_API void   yjs_line_col(const char* text, size_t n, size_t pos, uint32_t* line, uint32_t* column);

/* --- access ------------------------------------------------------------------ */
/* NULL when v is not an object, or has no such key. */
YJS_API yjs_value*  yjs_get(const yjs_value* obj, const char* key);
YJS_API yjs_value*  yjs_get_n(const yjs_value* obj, const char* key, size_t n);
YJS_API yjs_member* yjs_member_n(const yjs_value* obj, const char* key, size_t n);
/* NULL when v is not an array or i is past its end. */
YJS_API yjs_value*  yjs_at(const yjs_value* arr, size_t i);
/* The text of a string, or NULL; n may be NULL. */
YJS_API const char* yjs_string(const yjs_value* v, size_t* n);
/* YJS_OK, YJS_ERR_TYPE (NULL, not a number or not integer text) or
 * YJS_ERR_RANGE. */
YJS_API int    yjs_int64(const yjs_value* v, int64_t* out);
YJS_API int    yjs_double(const yjs_value* v, double* out);
YJS_API int    yjs_fixed(const yjs_value* v, int decimals, int64_t* out);
/* YJS_OK or YJS_ERR_TYPE. */
YJS_API int    yjs_bool(const yjs_value* v, bool* out);
/* The number grammar on raw text (s need not end in NUL); decimals 0..18. */
YJS_API int    yjs_parse_fixed(const char* s, size_t n, int decimals, int64_t* out);
YJS_API int    yjs_parse_double(const char* s, size_t n, double* out);
/* The canonical text of a finite double: the shortest decimal that reads
 * back to it (the nearest such, ties to even), spelled as the writer spells
 * numbers; -0.0 is "0". The length (no NUL); the text and a NUL when it fits
 * in cap (YJS_DOUBLE_TEXT always does); YJS_ERR_RANGE for NaN or infinity. */
YJS_API int    yjs_format_double(double x, char* buf, size_t cap);
/* Whether s[0..n) is one JSON number; *flags gets YJS_NUM_INT. */
YJS_API bool   yjs_number_ok(const char* s, size_t n, unsigned* flags);
/* Whether s[0..n) is valid UTF-8 without NUL. */
YJS_API bool   yjs_utf8_ok(const char* s, size_t n);

/* --- the builder ------------------------------------------------------------- */
YJS_API yjs_value* yjs_new(yjs_arena* a, int type);
YJS_API yjs_value* yjs_new_int(yjs_arena* a, int64_t v);
YJS_API yjs_value* yjs_new_fixed(yjs_arena* a, int64_t v, int decimals);
YJS_API yjs_value* yjs_new_double(yjs_arena* a, double v);
YJS_API yjs_value* yjs_new_number(yjs_arena* a, const char* text, size_t n);
YJS_API yjs_value* yjs_new_string(yjs_arena* a, const char* s, size_t n);
YJS_API yjs_value* yjs_new_stringz(yjs_arena* a, const char* s);
YJS_API int        yjs_push(yjs_arena* a, yjs_value* arr, yjs_value* v);
YJS_API int        yjs_set(yjs_arena* a, yjs_value* obj, const char* key, yjs_value* v);
YJS_API int        yjs_set_n(yjs_arena* a, yjs_value* obj, const char* key, size_t n, yjs_value* v);
YJS_API yjs_value* yjs_copy(yjs_arena* a, const yjs_value* v);

/* --- the writer -------------------------------------------------------------- */
YJS_API int    yjs_write(const yjs_value* v, int flags, yjs_write_fn fn, void* ctx);
/* The length of the text (no NUL); the text and a NUL when it fits in cap. */
YJS_API size_t yjs_write_mem(const yjs_value* v, int flags, char* buf, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* YSP_JSON_H_INCLUDED */

/* ======================================================================= *
 *                            IMPLEMENTATION                               *
 * ======================================================================= */
#ifdef YSP_JSON_IMPLEMENTATION
#ifndef YSP_JSON_IMPLEMENTATION_GUARD
#define YSP_JSON_IMPLEMENTATION_GUARD

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

#ifndef YJS_MALLOC
#define YJS_MALLOC(n)     malloc(n)
#define YJS_REALLOC(p, n) realloc(p, n)
#define YJS_FREE(p)       free(p)
#endif

#ifdef __cplusplus
extern "C" {
#endif

struct yjs_block {
    yjs_block* next;
    size_t     cap;
    /* the bytes follow, 16-aligned */
};
#define YJS__BLOCK_HDR ((sizeof(yjs_block) + 15u) & ~(size_t)15u)

YJS_API const char* yjs_version(void) { return YJS_VERSION_STRING; }

/* ======================================================================= *
 *  ARENA
 * ======================================================================= */

YJS_API void yjs_arena_init(yjs_arena* a, void* buf, size_t cap) {
    uintptr_t b = (uintptr_t)buf, al = (b + 7u) & ~(uintptr_t)7u;
    if (!a) return;
    memset(a, 0, sizeof *a);
    /* both ends 8-aligned: values from the bottom, the parser's slots from
     * the top */
    if (!buf || cap < (size_t)(al - b) + 8u) return;
    a->p = (unsigned char*)al;
    a->cap = (cap - (size_t)(al - b)) & ~(size_t)7u;
    a->first = a->p;
    a->first_cap = a->cap;
}

YJS_API void yjs_arena_init_heap(yjs_arena* a, size_t limit) {
    if (!a) return;
    memset(a, 0, sizeof *a);
    a->heap_limit = limit ? limit : ((size_t)256 << 20);
}

YJS_API void yjs_arena_free(yjs_arena* a) {
    yjs_block* b;
    size_t lim;
    if (!a) return;
    b = a->heap;
    while (b) {
        yjs_block* nx = b->next;
        YJS_FREE(b);
        b = nx;
    }
    lim = a->heap_limit;
    {
        unsigned char* f = a->first;
        size_t fc = a->first_cap;
        memset(a, 0, sizeof *a);
        a->p = f;
        a->cap = fc;
        a->first = f;
        a->first_cap = fc;
        a->heap_limit = lim;
    }
}

YJS_API void yjs_arena_reset(yjs_arena* a) {
    yjs_block* keep;
    if (!a) return;
    if (a->first) { yjs_arena_free(a); return; }
    /* a heap arena keeps its oldest block, the smallest */
    keep = a->heap;
    while (keep && keep->next) {
        yjs_block* nx = keep->next;
        a->heap = nx;
        a->heap_bytes -= YJS__BLOCK_HDR + keep->cap;
        YJS_FREE(keep);
        keep = nx;
    }
    a->heap = keep;
    a->p = keep ? (unsigned char*)keep + YJS__BLOCK_HDR : NULL;
    a->cap = keep ? keep->cap : 0;
    a->used = a->top = a->total = a->need = 0;
}

/* A new heap block that holds at least n bytes. */
static bool yjs__grow(yjs_arena* a, size_t n) {
    size_t want;
    yjs_block* b;
    a->need = n;
    if (!a->heap_limit || a->top || n > a->heap_limit || a->heap_bytes > a->heap_limit) return false;
    want = a->cap ? a->cap * 2 : 4096;
    if (want > ((size_t)1 << 20)) want = (size_t)1 << 20;
    if (want < n) want = (n + 4095u) & ~(size_t)4095u;
    if (YJS__BLOCK_HDR + want > a->heap_limit - a->heap_bytes) {
        want = (n + 7u) & ~(size_t)7u;     /* the last block: just the request */
        if (YJS__BLOCK_HDR + want > a->heap_limit - a->heap_bytes) return false;
    }
    b = (yjs_block*)YJS_MALLOC(YJS__BLOCK_HDR + want);
    if (!b) return false;
    a->need = 0;
    b->next = a->heap;
    b->cap = want;
    a->heap = b;
    a->heap_bytes += YJS__BLOCK_HDR + want;
    a->p = (unsigned char*)b + YJS__BLOCK_HDR;
    a->cap = want;
    a->used = 0;
    return true;
}

static void* yjs__take(yjs_arena* a, size_t n, size_t align) {
    size_t at;
    if (!a) return NULL;
    at = (a->used + (align - 1u)) & ~(align - 1u);
    if (!a->p || at > a->cap - a->top || n > a->cap - a->top - at) {
        if (!yjs__grow(a, n + align)) return NULL;
        at = (a->used + (align - 1u)) & ~(align - 1u);
    }
    a->used = at + n;
    a->total += n;
    return a->p + at;
}

YJS_API void* yjs_alloc(yjs_arena* a, size_t n) {
    void* p = yjs__take(a, n ? n : 1, 8);
    if (p) memset(p, 0, n ? n : 1);
    return p;
}

static char* yjs__dup(yjs_arena* a, const char* s, size_t n) {
    char* d = (char*)yjs__take(a, n + 1, 1);
    if (!d) return NULL;
    if (n) memcpy(d, s, n);
    d[n] = '\0';
    return d;
}

/* ======================================================================= *
 *  UTF-8, NUMBERS
 * ======================================================================= */

/* The length of the valid UTF-8 sequence at s (n bytes left), or 0. */
static size_t yjs__utf8_seq(const unsigned char* s, size_t n) {
    unsigned c = s[0];
    unsigned lo = 0x80, hi = 0xBF;
    size_t k, m;
    if (c < 0x80) return c ? 1 : 0;
    if (c < 0xC2 || c > 0xF4) return 0;
    m = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : 2;
    if (m > n) return 0;
    if (c == 0xE0) lo = 0xA0;            /* overlong */
    else if (c == 0xED) hi = 0x9F;       /* surrogates */
    else if (c == 0xF0) lo = 0x90;       /* overlong */
    else if (c == 0xF4) hi = 0x8F;       /* above U+10FFFF */
    if (s[1] < lo || s[1] > hi) return 0;
    for (k = 2; k < m; k++)
        if ((s[k] & 0xC0u) != 0x80u) return 0;
    return m;
}

YJS_API bool yjs_utf8_ok(const char* s, size_t n) {
    const unsigned char* p = (const unsigned char*)s;
    size_t i = 0;
    if (!s && n) return false;
    while (i < n) {
        size_t m;
        if (p[i] >= 0x01 && p[i] < 0x80) { i++; continue; }
        m = yjs__utf8_seq(p + i, n - i);
        if (!m) return false;
        i += m;
    }
    return true;
}

/* The JSON number at s[0..n): its length (0 = no number) and flags. */
static size_t yjs__number_len(const unsigned char* s, size_t n, unsigned* flags, const char** why) {
    size_t i = 0, ni = 0, lead = 0;
    unsigned f = YJS_NUM_INT;
    bool nonzero = false;
    int64_t x = 0;
    *why = "not a number";
    if (i < n && s[i] == '-') i++;
    if (i >= n || s[i] < '0' || s[i] > '9') { *why = "a number has no digits"; return 0; }
    if (s[i] == '0') {
        i++;
        if (i < n && s[i] >= '0' && s[i] <= '9') { *why = "a leading zero"; return 0; }
    } else {
        size_t i0 = i;
        nonzero = true;
        while (i < n && s[i] >= '0' && s[i] <= '9') i++;
        ni = i - i0;
    }
    if (i < n && s[i] == '.') {
        i++;
        f = 0;
        if (i >= n || s[i] < '0' || s[i] > '9') { *why = "no digit after '.'"; return 0; }
        for (; i < n && s[i] >= '0' && s[i] <= '9'; i++) {
            if (nonzero) continue;
            if (s[i] == '0') lead++;
            else nonzero = true;
        }
    }
    if (i < n && (s[i] == 'e' || s[i] == 'E')) {
        bool eneg = false;
        i++;
        f = 0;
        if (i < n && (s[i] == '+' || s[i] == '-')) eneg = s[i++] == '-';
        if (i >= n || s[i] < '0' || s[i] > '9') { *why = "no digit in an exponent"; return 0; }
        for (; i < n && s[i] >= '0' && s[i] <= '9'; i++) if (x < 100000000000) x = x * 10 + (s[i] - '0');
        if (eneg) x = -x;
    }
    /* A limit RFC 8259 (section 9) allows: the first significant digit's
     * exponent is within +-999999999, so the canonical writer's exponent has
     * at most 9 digits and reads back. No double needs more than 3. */
    if (nonzero) {
        int64_t e = (ni ? (int64_t)ni - 1 : -1 - (int64_t)lead) + x;
        if (e > 999999999 || e < -999999999) { *why = "an exponent past 999999999"; return 0; }
    }
    *flags = f;
    return i;
}

YJS_API bool yjs_number_ok(const char* s, size_t n, unsigned* flags) {
    unsigned f = 0;
    const char* why;
    bool ok = s && n && yjs__number_len((const unsigned char*)s, n, &f, &why) == n;
    if (flags) *flags = ok ? f : 0;
    return ok;
}

/* Integer text to int64: false when it does not fit. */
static bool yjs__int_of(const char* s, size_t n, int64_t* out) {
    size_t i = 0;
    bool neg = false;
    uint64_t m = 0, lim;
    if (i < n && s[i] == '-') { neg = true; i++; }
    lim = neg ? (uint64_t)INT64_MAX + 1u : (uint64_t)INT64_MAX;
    for (; i < n; i++) {
        unsigned d = (unsigned)(s[i] - '0');
        if (m > (lim - d) / 10u) return false;
        m = m * 10u + d;
    }
    if (neg) *out = m == (uint64_t)INT64_MAX + 1u ? INT64_MIN : -(int64_t)m;
    else *out = (int64_t)m;
    return true;
}

YJS_API int yjs_parse_fixed(const char* s, size_t n, int decimals, int64_t* out) {
    unsigned f;
    const char* why;
    size_t i = 0, ndig = 0, nfrac = 0, k;
    bool neg = false, sticky = false;
    int64_t ex = 0, shift, kept;
    uint64_t acc = 0;
    int rd = 0;
    const unsigned char* p = (const unsigned char*)s;
    if (!s || !out || decimals < 0 || decimals > 18) return YJS_ERR_ARG;
    if (yjs__number_len(p, n, &f, &why) != n || n == 0) return YJS_ERR_TYPE;
    if (p[i] == '-') { neg = true; i++; }
    /* digits and the exponent */
    {
        size_t j;
        bool frac = false;
        for (j = i; j < n && p[j] != 'e' && p[j] != 'E'; j++) {
            if (p[j] == '.') { frac = true; continue; }
            ndig++;
            if (frac) nfrac++;
        }
        if (j < n) {
            bool eneg = false;
            j++;
            if (p[j] == '+' || p[j] == '-') eneg = p[j++] == '-';
            for (; j < n; j++) if (ex < 100000) ex = ex * 10 + (int64_t)(p[j] - '0');
            if (eneg) ex = -ex;
        }
    }
    shift = ex - (int64_t)nfrac + decimals;        /* value x 10^d = D x 10^shift */
    kept = shift >= 0 ? (int64_t)ndig : (int64_t)ndig + shift;
    k = 0;
    for (; i < n && p[i] != 'e' && p[i] != 'E'; i++) {
        unsigned d;
        if (p[i] == '.') continue;
        d = (unsigned)(p[i] - '0');
        if ((int64_t)k < kept) {
            if (acc > (UINT64_MAX - d) / 10u) return YJS_ERR_RANGE;
            acc = acc * 10u + d;
        } else if ((int64_t)k == kept) {
            rd = (int)d;
        } else if (d) {
            sticky = true;
        }
        k++;
    }
    if (shift > 0 && acc) {
        int64_t t;
        for (t = 0; t < shift; t++) {
            if (acc > UINT64_MAX / 10u) return YJS_ERR_RANGE;
            acc *= 10u;
        }
    }
    if (rd > 5 || (rd == 5 && (sticky || (acc & 1u)))) {
        if (acc == UINT64_MAX) return YJS_ERR_RANGE;
        acc++;
    }
    if (neg) {
        if (acc > (uint64_t)INT64_MAX + 1u) return YJS_ERR_RANGE;
        *out = acc == (uint64_t)INT64_MAX + 1u ? INT64_MIN : -(int64_t)acc;
    } else {
        if (acc > (uint64_t)INT64_MAX) return YJS_ERR_RANGE;
        *out = (int64_t)acc;
    }
    return YJS_OK;
}

/* --- decimal to double: ysp/table.h's ytb_parse_number() conversion ------------- */

#define YJS__MAX_DIGITS 19
#define YJS__BIGW 40
typedef struct yjs__big {
    uint32_t w[YJS__BIGW];
    int      n;
} yjs__big;

static void yjs__big_set(yjs__big* b, uint64_t v) {
    b->w[0] = (uint32_t)v;
    b->w[1] = (uint32_t)(v >> 32);
    b->n = b->w[1] ? 2 : (b->w[0] ? 1 : 0);
}
static void yjs__big_mul(yjs__big* b, uint32_t m) {
    uint64_t carry = 0;
    int i;
    for (i = 0; i < b->n; i++) {
        uint64_t t = (uint64_t)b->w[i] * m + carry;
        b->w[i] = (uint32_t)t;
        carry = t >> 32;
    }
    if (carry && b->n < YJS__BIGW) b->w[b->n++] = (uint32_t)carry;
}
static void yjs__big_pow5(yjs__big* b, int q) {
    yjs__big_set(b, 1);
    while (q >= 13) { yjs__big_mul(b, 1220703125u); q -= 13; }
    while (q > 0) { yjs__big_mul(b, 5u); q--; }
}
static void yjs__big_shl(yjs__big* b, int s) {
    int ws = s / 32, bs = s % 32, i, n = b->n;
    if (n == 0 || s <= 0) return;
    if (n + ws + 1 > YJS__BIGW) return;
    b->w[n + ws] = 0;
    for (i = n - 1; i >= 0; i--) {
        uint32_t v = b->w[i];
        if (bs) {
            b->w[i + ws + 1] |= v >> (32 - bs);
            b->w[i + ws] = v << bs;
        } else {
            b->w[i + ws] = v;
        }
    }
    for (i = 0; i < ws; i++) b->w[i] = 0;
    b->n = n + ws + 1;
    while (b->n > 0 && b->w[b->n - 1] == 0) b->n--;
}
static int yjs__bits64(uint64_t v) {
    int n = 0;
    while (v) { n++; v >>= 1; }
    return n;
}
static int yjs__bits128(uint64_t hi, uint64_t lo) {
    return hi ? 64 + yjs__bits64(hi) : yjs__bits64(lo);
}
static int yjs__big_bits(const yjs__big* b) {
    if (b->n == 0) return 0;
    return (b->n - 1) * 32 + yjs__bits64(b->w[b->n - 1]);
}
static int yjs__big_bit(const yjs__big* b, int i) {
    int w = i / 32;
    if (i < 0 || w >= b->n) return 0;
    return (int)((b->w[w] >> (i % 32)) & 1u);
}
static bool yjs__big_any_below(const yjs__big* b, int i) {
    int w, k;
    if (i <= 0) return false;
    w = i / 32;
    for (k = 0; k < w && k < b->n; k++) if (b->w[k]) return true;
    if (w < b->n && (i % 32) && (b->w[w] & ((1u << (i % 32)) - 1u))) return true;
    return false;
}
static uint64_t yjs__big_get64(const yjs__big* b, int i) {
    uint64_t v = 0;
    int k;
    for (k = 0; k < 64; k++) v |= (uint64_t)yjs__big_bit(b, i + k) << k;
    return v;
}
static int yjs__big_cmp(const yjs__big* a, const yjs__big* b) {
    int i;
    if (a->n != b->n) return a->n < b->n ? -1 : 1;
    for (i = a->n - 1; i >= 0; i--)
        if (a->w[i] != b->w[i]) return a->w[i] < b->w[i] ? -1 : 1;
    return 0;
}
static void yjs__big_sub(yjs__big* a, const yjs__big* b) {
    int64_t borrow = 0;
    int i;
    for (i = 0; i < a->n; i++) {
        int64_t t = (int64_t)a->w[i] - (i < b->n ? (int64_t)b->w[i] : 0) - borrow;
        borrow = t < 0;
        a->w[i] = (uint32_t)(t + (borrow ? ((int64_t)1 << 32) : 0));
    }
    while (a->n > 0 && a->w[a->n - 1] == 0) a->n--;
}
static void yjs__big_dbl(yjs__big* r, int bit) {
    uint32_t carry = (uint32_t)bit;
    int i;
    for (i = 0; i < r->n; i++) {
        uint32_t v = r->w[i];
        r->w[i] = (v << 1) | carry;
        carry = v >> 31;
    }
    if (carry && r->n < YJS__BIGW) r->w[r->n++] = carry;
}
static uint64_t yjs__round(uint64_t mant, int half, bool sticky) {
    if (half && (sticky || (mant & 1u))) mant++;
    return mant;
}

#if defined(__SIZEOF_INT128__)
__extension__ typedef unsigned __int128 yjs__u128;
#define YJS__HAVE_U128 1
#endif

static void yjs__mul64(uint64_t a, uint64_t b, uint64_t* hi, uint64_t* lo) {
#ifdef YJS__HAVE_U128
    yjs__u128 p = (yjs__u128)a * b;
    *hi = (uint64_t)(p >> 64);
    *lo = (uint64_t)p;
#else
    uint64_t a0 = a & 0xffffffffu, a1 = a >> 32, b0 = b & 0xffffffffu, b1 = b >> 32;
    uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
    uint64_t mid = (p00 >> 32) + (p01 & 0xffffffffu) + (p10 & 0xffffffffu);
    *lo = (p00 & 0xffffffffu) | (mid << 32);
    *hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
#endif
}
static uint64_t yjs__div128(uint64_t hi, uint64_t lo, uint64_t d, bool* rem) {
#ifdef YJS__HAVE_U128
    yjs__u128 n = ((yjs__u128)hi << 64) | lo;
    *rem = (n % d) != 0;
    return (uint64_t)(n / d);
#else
    uint64_t r = hi, q = 0;
    int i;
    for (i = 63; i >= 0; i--) {
        r = (r << 1) | ((lo >> i) & 1u);
        q <<= 1;
        if (r >= d) {
            r -= d;
            q |= 1u;
        }
    }
    *rem = r != 0;
    return q;
#endif
}

static const double yjs__p10[23] = {
    1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11,
    1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22
};

/* M x 10^E, M in (0, 10^19), correctly rounded, ties to even. */
static int yjs__convert(uint64_t m, int e, double* out) {
    yjs__big n, d, r;
    uint64_t mant, q;
    int b, shift, half, i, s, bq, u, scale;
    bool sticky;
    double v;

    if (e > 310) return YJS_ERR_RANGE;
    if (e < -345) return YJS_ERR_RANGE;
    if (m <= ((uint64_t)1 << 53) && e >= -22 && e <= 22) {
        v = (double)m;
        *out = e >= 0 ? v * yjs__p10[e] : v / yjs__p10[-e];
        return YJS_OK;
    }
    if (e >= -27 && e <= 27) {
        uint64_t p5 = 1, hi, lo;
        int k;
        for (k = 0; k < (e < 0 ? -e : e); k++) p5 *= 5u;
        if (e >= 0) {
            yjs__mul64(m, p5, &hi, &lo);
            b = yjs__bits128(hi, lo);
            if (b <= 53) {
                mant = lo;
                shift = 0;
            } else {
                shift = b - 53;
                if (shift >= 64) {
                    mant = hi >> (shift - 64);
                    half = (int)((shift - 65 >= 0) ? ((hi >> (shift - 65)) & 1u) : (lo >> 63));
                    sticky = (shift - 65 > 0 && (hi & ((((uint64_t)1) << (shift - 65)) - 1u))) ||
                             (shift - 65 >= 0 ? lo != 0 : (lo & ~((uint64_t)1 << 63)) != 0);
                } else {
                    mant = (lo >> shift) | (shift ? hi << (64 - shift) : 0);
                    half = (int)((lo >> (shift - 1)) & 1u);
                    sticky = shift >= 2 && (lo & ((((uint64_t)1) << (shift - 1)) - 1u)) != 0;
                }
                mant = yjs__round(mant, half, sticky);
            }
            *out = ldexp((double)mant, e + shift);
            return YJS_OK;
        } else {
            int bd = yjs__bits64(p5);
            s = 56 - yjs__bits64(m) + bd;
            if (s >= 64) { hi = m << (s - 64); lo = 0; }
            else if (s > 0) { hi = m >> (64 - s); lo = m << s; }
            else if (s == 0) { hi = 0; lo = m; }
            else { hi = 0; lo = m; p5 <<= -s; }
            q = yjs__div128(hi, lo, p5, &sticky);
            bq = yjs__bits64(q);
            scale = s - e;
            u = bq - 1 - scale - 52;
            if (u < -1074) u = -1074;
            shift = u + scale;
            mant = q >> shift;
            half = (int)((q >> (shift - 1)) & 1u);
            if (shift >= 2 && (q & ((((uint64_t)1) << (shift - 1)) - 1u))) sticky = true;
            mant = yjs__round(mant, half, sticky);
            *out = ldexp((double)mant, u);
            return YJS_OK;
        }
    }
    if (e >= 0) {
        yjs__big_set(&n, m);
        for (i = e; i >= 13; i -= 13) yjs__big_mul(&n, 1220703125u);
        for (; i > 0; i--) yjs__big_mul(&n, 5u);
        b = yjs__big_bits(&n);
        if (b <= 53) {
            mant = yjs__big_get64(&n, 0);
            shift = 0;
        } else {
            shift = b - 53;
            mant = yjs__big_get64(&n, shift);
            half = yjs__big_bit(&n, shift - 1);
            sticky = yjs__big_any_below(&n, shift - 1);
            mant = yjs__round(mant, half, sticky);
        }
        v = ldexp((double)mant, e + shift);
        if (!(v < HUGE_VAL)) return YJS_ERR_RANGE;
        *out = v;
        return YJS_OK;
    }
    yjs__big_pow5(&d, -e);
    s = 56 - yjs__bits64(m) + yjs__big_bits(&d);
    yjs__big_set(&n, m);
    if (s >= 0) yjs__big_shl(&n, s);
    else yjs__big_shl(&d, -s);
    r.n = 0;
    {
        int nb = yjs__big_bits(&n), k;
        for (k = nb - 1; k >= 58; k--) yjs__big_dbl(&r, yjs__big_bit(&n, k));
        while (r.n > 0 && r.w[r.n - 1] == 0) r.n--;
    }
    q = 0;
    for (i = 57; i >= 0; i--) {
        yjs__big_dbl(&r, yjs__big_bit(&n, i));
        if (yjs__big_cmp(&r, &d) >= 0) {
            yjs__big_sub(&r, &d);
            q |= (uint64_t)1 << i;
        }
    }
    sticky = r.n != 0;
    bq = yjs__bits64(q);
    scale = s - e;
    u = bq - 1 - scale - 52;
    if (u < -1074) u = -1074;
    shift = u + scale;
    if (shift >= 65) {
        mant = 0; half = 0; sticky = true;
    } else {
        mant = shift >= 64 ? 0 : q >> shift;
        half = (int)((q >> (shift - 1)) & 1u);
        if (shift >= 2 && (q & ((((uint64_t)1) << (shift - 1)) - 1u))) sticky = true;
    }
    mant = yjs__round(mant, half, sticky);
    v = ldexp((double)mant, u);
    if (v == 0.0) return YJS_ERR_RANGE;
    *out = v;
    return YJS_OK;
}

YJS_API int yjs_parse_double(const char* s, size_t n, double* out) {
    unsigned f;
    const char* why;
    size_t i = 0;
    bool neg = false, frac = false;
    uint64_t m = 0;
    int nd = 0, z = 0, e = 0, ex = 0, rc;
    double v;
    if (!s || !out) return YJS_ERR_ARG;
    if (n == 0 || yjs__number_len((const unsigned char*)s, n, &f, &why) != n) return YJS_ERR_TYPE;
    if (s[i] == '-') { neg = true; i++; }
    for (; i < n; i++) {
        char c = s[i];
        if (c == '.') { frac = true; continue; }
        if (c < '0' || c > '9') break;
        if (frac) e--;
        if (c == '0') {
            if (m != 0) z++;             /* a zero that may still be trailing */
            continue;
        }
        for (; z > 0; z--) {             /* zeros between nonzero digits count */
            if (++nd > YJS__MAX_DIGITS) return YJS_ERR_RANGE;
            m *= 10u;
        }
        if (++nd > YJS__MAX_DIGITS) return YJS_ERR_RANGE;
        m = m * 10u + (uint64_t)(c - '0');
    }
    if (i < n) {
        bool eneg = false;
        i++;
        if (s[i] == '+' || s[i] == '-') eneg = s[i++] == '-';
        for (; i < n; i++) if (ex < 100000) ex = ex * 10 + (s[i] - '0');
        if (eneg) ex = -ex;
    }
    if (m == 0) {
        *out = neg ? -0.0 : 0.0;
        return YJS_OK;
    }
    rc = yjs__convert(m, e + z + ex, &v);
    if (rc != YJS_OK) return rc;
    *out = neg ? -v : v;
    return YJS_OK;
}

/* --- the canonical spelling of a decimal ---------------------------------------- */

typedef void (*yjs__emit_fn)(void* ctx, const char* s, size_t n);

/* Decimal digits of m, written backwards into the end of buf; the start. */
static char* yjs__utoa(uint64_t m, char* end) {
    do { *--end = (char)('0' + (int)(m % 10u)); m /= 10u; } while (m);
    return end;
}

/* D x 10^(E - k + 1), D the k digits d1 d2 (two pieces, so that a number's
 * text is read in place around its '.'), no leading or trailing zero. */
static void yjs__spell(yjs__emit_fn emit, void* ctx, bool neg, const char* d1, size_t n1, const char* d2, size_t n2,
                       int64_t E) {
    static const char zeros[] = "00000000000000000000";
    size_t k = n1 + n2, at = 1;
    bool sci = E < YJS_PLAIN_MIN || E > YJS_PLAIN_MAX;
    if (neg) emit(ctx, "-", 1);
    if (!sci) {
        if (E >= (int64_t)k - 1) {
            emit(ctx, d1, n1);
            emit(ctx, d2, n2);
            emit(ctx, zeros, (size_t)(E - (int64_t)k + 1));
            return;
        }
        if (E < 0) {
            emit(ctx, "0.", 2);
            emit(ctx, zeros, (size_t)(-E - 1));
            emit(ctx, d1, n1);
            emit(ctx, d2, n2);
            return;
        }
        at = (size_t)E + 1;              /* digits before the point, fewer than k */
    }
    /* `at` digits, then the point when more follow */
    if (at <= n1) {
        emit(ctx, d1, at);
        if (k > at) emit(ctx, ".", 1);
        emit(ctx, d1 + at, n1 - at);
        emit(ctx, d2, n2);
    } else {
        emit(ctx, d1, n1);
        emit(ctx, d2, at - n1);
        emit(ctx, ".", 1);
        emit(ctx, d2 + (at - n1), n2 - (at - n1));
    }
    if (sci) {
        char buf[24];
        char* e = buf + sizeof buf;
        char* s = yjs__utoa(E < 0 ? (uint64_t)0 - (uint64_t)E : (uint64_t)E, e);
        *--s = E < 0 ? '-' : '+';
        *--s = 'e';
        emit(ctx, s, (size_t)(e - s));
    }
}

/* The canonical text of a JSON number's text: its exact value, respelled.
 * The grammar bounds E to +-999999999, so the exponent text reads back. */
static void yjs__canon_number(yjs__emit_fn emit, void* ctx, const char* s, size_t n) {
    size_t i = 0, ip, ni, nf = 0, fp = 0, a, b, nc;
    int64_t x = 0;
    bool neg = false;
    if (i < n && s[i] == '-') { neg = true; i++; }
    ip = i;
    while (i < n && s[i] >= '0' && s[i] <= '9') i++;
    ni = i - ip;
    if (i < n && s[i] == '.') {
        fp = ++i;
        while (i < n && s[i] >= '0' && s[i] <= '9') i++;
        nf = i - fp;
    }
    if (i < n) {                         /* 'e' or 'E' */
        bool eneg = false;
        i++;
        if (s[i] == '+' || s[i] == '-') eneg = s[i++] == '-';
        for (; i < n; i++) if (x < 100000000000) x = x * 10 + (s[i] - '0');   /* past it only for a zero */
        if (eneg) x = -x;
    }
    /* the digits C = integer part, then fraction; C[t] is s[ip + t] or s[fp + t - ni] */
    nc = ni + nf;
    for (a = 0; a < nc && (a < ni ? s[ip + a] : s[fp + a - ni]) == '0'; a++) {}
    if (a == nc) { emit(ctx, "0", 1); return; }
    for (b = nc; (b - 1 < ni ? s[ip + b - 1] : s[fp + b - 1 - ni]) == '0'; b--) {}
    /* significant digits C[a..b), the first at 10^(ni - 1 - a) x 10^x */
    if (b <= ni) yjs__spell(emit, ctx, neg, s + ip + a, b - a, NULL, 0, (int64_t)ni - 1 - (int64_t)a + x);
    else if (a >= ni) yjs__spell(emit, ctx, neg, s + fp + (a - ni), b - a, NULL, 0, (int64_t)ni - 1 - (int64_t)a + x);
    else yjs__spell(emit, ctx, neg, s + ip + a, ni - a, s + fp, b - ni, (int64_t)ni - 1 - (int64_t)a + x);
}

/* --- RYU SECTION: from here to END RYU SECTION ------------------------------------
 * This section may derive from Ryu, Copyright 2018 Ulf Adams, and is used
 * under the Boost Software License 1.0, whose text is at the end of this
 * file. Keep this notice and that text with any copy of this section. */

/* --- double to the shortest decimal that reads back to it ------------------------
 * Ryu (Ulf Adams, "Ryu: fast float-to-string conversion", PLDI 2018) in the
 * variant with small tables: a 125-bit multiplier for 5^i or 5^-i is one of
 * 13 stored ones times a power of 5 below 5^26, plus a correction of 0 to
 * 2. Integer arithmetic only, so the digits do not depend on the compiler,
 * the FPU or the C library. The tables and their check are
 * tests/adapt/json_dtoa_ref.py's; json_test.c compares every multiplier. */

/* tests/adapt/json_dtoa_ref.py tables: i <= 325, q <= 290 */
static const uint64_t yjs__p5[26] = {
    1u, 5u, 25u, 125u,
    625u, 3125u, 15625u, 78125u,
    390625u, 1953125u, 9765625u, 48828125u,
    244140625u, 1220703125u, 6103515625u, 30517578125u,
    152587890625u, 762939453125u, 3814697265625u, 19073486328125u,
    95367431640625u, 476837158203125u, 2384185791015625u, 11920928955078125u,
    59604644775390625u, 298023223876953125u,
};
static const uint64_t yjs__p5_split[13][2] = {
    { 0x0000000000000000u, 0x1000000000000000u },
    { 0x0000000000000000u, 0x14adf4b7320334b9u },
    { 0x0e549208b31adb10u, 0x1aba4714957d300du },
    { 0x6dc6ad264d8f0866u, 0x1145b7e285bf98f5u },
    { 0xeb1dbd923d8596cau, 0x1652efdc6018a1fcu },
    { 0xb4c1b80b22ae923cu, 0x1cda62055b2d9d83u },
    { 0x5bb28b4e8f7e4c30u, 0x12a5568b9f52f416u },
    { 0xf08aed437682d4fbu, 0x1819651531f9e78fu },
    { 0xb4ee134ad99bf150u, 0x1f25c186a6f04c28u },
    { 0x16499ecb70c25f03u, 0x1420eb449c8842e6u },
    { 0x85a56ead360865b0u, 0x1a03fde214caf085u },
    { 0x093db1d57999890bu, 0x10cfeb353a97dad8u },
    { 0xcf38bb735e3f36acu, 0x15baaf44fa52673eu },
};
static const uint64_t yjs__p5_inv[13][2] = {
    { 0x0000000000000000u, 0x2000000000000000u },
    { 0x52a6c95fc0655033u, 0x18c240c4aecb13bbu },
    { 0x7ca8d50071dfc805u, 0x1327fc58da0f6ff5u },
    { 0x6520247d3556476du, 0x1da48ce468e7c702u },
    { 0x6139cdd76802e6e8u, 0x16ef5b40c2fc7779u },
    { 0xf951a7ff43de8c78u, 0x11bebdf578b2f391u },
    { 0x7be8bee8d6e957e7u, 0x1b758d848fac54b0u },
    { 0x8bd3f9e999a423e9u, 0x153eda614071a3b7u },
    { 0x0848f973cb3ee3cdu, 0x10701bd527b4978cu },
    { 0x153285ebb9efbfa1u, 0x196fbb9bb44db44du },
    { 0xadeee7f86c07b695u, 0x13ae3591f5b4d936u },
    { 0x4d686a4eaf182221u, 0x1e74404f3daada91u },
    { 0x98c0a106e09ebd9eu, 0x17900ea4fda7c257u },
};
static const uint8_t yjs__p5_corr[82] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 64, 149, 89, 105, 89, 85, 85, 84, 85, 21, 85, 85, 86,
    4, 5, 21, 65, 16, 84, 85, 64, 69, 81, 85, 68, 64, 69, 80, 68,
    80, 85, 85, 69, 0, 64, 0, 64, 64, 4, 68, 150, 101, 85, 86, 85,
    69, 64, 69, 84, 81, 65, 21, 64, 85, 145, 85, 85, 85, 85, 64, 81,
    5, 1,
};
static const uint8_t yjs__p5_inv_corr[73] = {
    84, 69, 84, 84, 69, 85, 5, 4, 0, 16, 4, 16, 20, 4, 64, 0,
    0, 0, 1, 64, 85, 85, 21, 65, 84, 4, 0, 0, 68, 0, 1, 0,
    0, 0, 0, 64, 65, 0, 0, 68, 80, 68, 69, 80, 84, 0, 85, 85,
    84, 85, 101, 81, 0, 64, 0, 64, 1, 0, 0, 1, 0, 5, 1, 0,
    17, 84, 81, 81, 84, 85, 85, 5, 0,
};

/* bits of 5^e, 0 <= e <= 3528 */
static uint32_t yjs__pow5bits(uint32_t e) { return ((e * 1217359u) >> 19) + 1u; }

/* (m x (hi:lo)) >> s, 1 <= s <= 63, kept to 128 bits */
static void yjs__mulshr(uint64_t m, const uint64_t* v, uint32_t s, uint64_t* lo, uint64_t* hi) {
    uint64_t h0, l0, h2, l2, rl, rh;
    yjs__mul64(m, v[0], &h0, &l0);
    yjs__mul64(m, v[1], &h2, &l2);
    rl = (l0 >> s) | (h0 << (64 - s));
    rh = h0 >> s;
    *lo = rl + (l2 << (64 - s));
    *hi = rh + ((h2 << (64 - s)) | (l2 >> s)) + (*lo < rl);
}

static void yjs__add_small(uint64_t* lo, uint64_t* hi, uint64_t c) {
    *lo += c;
    *hi += *lo < c;
}

/* 5^i to 125 bits, i <= 325 */
static void yjs__pow5(uint32_t i, uint64_t* lo, uint64_t* hi) {
    uint32_t b = i / 26u, o = i - b * 26u;
    if (o == 0) { *lo = yjs__p5_split[b][0]; *hi = yjs__p5_split[b][1]; return; }
    yjs__mulshr(yjs__p5[o], yjs__p5_split[b], yjs__pow5bits(i) - yjs__pow5bits(b * 26u), lo, hi);
    yjs__add_small(lo, hi, (uint64_t)((yjs__p5_corr[i >> 2] >> ((i & 3u) * 2u)) & 3u));
}

/* floor(2^(bits of 5^q - 1 + 125) / 5^q) + 1, q <= 290 */
static void yjs__pow5_inv(uint32_t q, uint64_t* lo, uint64_t* hi) {
    uint32_t b = (q + 25u) / 26u, o = b * 26u - q;
    if (o == 0) {
        *lo = yjs__p5_inv[b][0];
        *hi = yjs__p5_inv[b][1];
        yjs__add_small(lo, hi, 1u);
        return;
    }
    yjs__mulshr(yjs__p5[o], yjs__p5_inv[b], yjs__pow5bits(b * 26u) - yjs__pow5bits(q), lo, hi);
    yjs__add_small(lo, hi, 1u + (uint64_t)((yjs__p5_inv_corr[q >> 2] >> ((q & 3u) * 2u)) & 3u));
}

/* (m x (hi:lo)) >> j, 118 <= j <= 125: the result fits 64 bits */
static uint64_t yjs__mulshift(uint64_t m, uint64_t lo, uint64_t hi, uint32_t j) {
    uint64_t h0, l0, h2, l2, sl, sh;
    uint32_t s = j - 64u;
    yjs__mul64(m, lo, &h0, &l0);
    yjs__mul64(m, hi, &h2, &l2);
    sl = l2 + h0;
    sh = h2 + (sl < h0);
    return (sh << (64 - s)) | (sl >> s);
}

static uint32_t yjs__pow5_factor(uint64_t v) {
    uint32_t k = 0;
    while (v && v % 5u == 0) { v /= 5u; k++; }
    return k;
}

/* A finite nonzero double's bits to D x 10^e, D the shortest digits that read
 * back to the same double, the nearest such, ties to even. */
static void yjs__ryu(uint64_t bits, uint64_t* digits, int32_t* exp10) {
    uint64_t ieee_m = bits & (((uint64_t)1 << 52) - 1u), m2, mv, vr, vp, vm, lo, hi, out;
    uint32_t ieee_e = (uint32_t)((bits >> 52) & 0x7FFu), mmshift, q;
    int32_t e2, e10, removed = 0;
    bool even, vm_tz = false, vr_tz = false;
    unsigned last = 0;
    if (ieee_e == 0) { e2 = 1 - 1023 - 52 - 2; m2 = ieee_m; }
    else { e2 = (int32_t)ieee_e - 1023 - 52 - 2; m2 = ((uint64_t)1 << 52) | ieee_m; }
    even = (m2 & 1u) == 0;
    mv = 4u * m2;
    /* the gap below a power of two is half the gap above it */
    mmshift = ieee_m != 0 || ieee_e <= 1;
    if (e2 >= 0) {
        q = (((uint32_t)e2 * 78913u) >> 18) - (e2 > 3);
        e10 = (int32_t)q;
        yjs__pow5_inv(q, &lo, &hi);
        {
            uint32_t j = (uint32_t)(-e2 + (int32_t)q + 125 + (int32_t)yjs__pow5bits(q) - 1);
            vr = yjs__mulshift(mv, lo, hi, j);
            vp = yjs__mulshift(mv + 2u, lo, hi, j);
            vm = yjs__mulshift(mv - 1u - mmshift, lo, hi, j);
        }
        if (q <= 21) {
            /* at most one of mv - 2, mv, mv + 2 is a multiple of 5 */
            if (mv % 5u == 0) vr_tz = yjs__pow5_factor(mv) >= q;
            else if (even) vm_tz = yjs__pow5_factor(mv - 1u - mmshift) >= q;
            else vp -= yjs__pow5_factor(mv + 2u) >= q;
        }
    } else {
        uint32_t i;
        q = (((uint32_t)-e2 * 732923u) >> 20) - (-e2 > 1);
        e10 = (int32_t)q + e2;
        i = (uint32_t)(-e2 - (int32_t)q);
        yjs__pow5(i, &lo, &hi);
        {
            uint32_t j = (uint32_t)((int32_t)q - ((int32_t)yjs__pow5bits(i) - 125));
            vr = yjs__mulshift(mv, lo, hi, j);
            vp = yjs__mulshift(mv + 2u, lo, hi, j);
            vm = yjs__mulshift(mv - 1u - mmshift, lo, hi, j);
        }
        if (q <= 1) {
            /* mv has two trailing zero bits, mv + 2 one */
            vr_tz = true;
            if (even) vm_tz = mmshift == 1;
            else vp--;
        } else if (q < 63) {
            vr_tz = (mv & (((uint64_t)1 << q) - 1u)) == 0;
        }
    }
    /* drop digits while the interval (vm, vp) still holds a shorter decimal */
    while (vp / 10u > vm / 10u) {
        vm_tz = vm_tz && vm % 10u == 0;
        vr_tz = vr_tz && last == 0;
        last = (unsigned)(vr % 10u);
        vr /= 10u; vp /= 10u; vm /= 10u;
        removed++;
    }
    if (vm_tz) {
        while (vm % 10u == 0) {
            vr_tz = vr_tz && last == 0;
            last = (unsigned)(vr % 10u);
            vr /= 10u; vp /= 10u; vm /= 10u;
            removed++;
        }
    }
    /* an exact tie rounds to even */
    if (vr_tz && last == 5 && vr % 2u == 0) last = 4;
    out = vr + ((vr == vm && (!even || !vm_tz)) || last >= 5);
    *digits = out;
    *exp10 = e10 + removed;
}

typedef struct yjs__sbuf { char* p; size_t n; } yjs__sbuf;
static void yjs__sbuf_emit(void* ctx, const char* s, size_t n) {
    yjs__sbuf* b = (yjs__sbuf*)ctx;
    if (!n) return;                      /* s may be NULL then */
    memcpy(b->p + b->n, s, n);
    b->n += n;
}

YJS_API int yjs_format_double(double x, char* buf, size_t cap) {
    char t[YJS_DOUBLE_TEXT];
    char d[24];
    yjs__sbuf sb;
    uint64_t bits, digits;
    int32_t e;
    memcpy(&bits, &x, sizeof bits);
    if (((bits >> 52) & 0x7FFu) == 0x7FFu) return YJS_ERR_RANGE;
    sb.p = t;
    sb.n = 0;
    if ((bits << 1) == 0) {
        t[sb.n++] = '0';                 /* -0.0 too */
    } else {
        char* end = d + sizeof d;
        char* s;
        yjs__ryu(bits, &digits, &e);
        s = yjs__utoa(digits, end);
        yjs__spell(yjs__sbuf_emit, &sb, (bits >> 63) != 0, s, (size_t)(end - s), NULL, 0,
                   (int64_t)e + (int64_t)(end - s) - 1);
    }
    if (buf && sb.n < cap) {
        memcpy(buf, t, sb.n);
        buf[sb.n] = '\0';
    }
    return (int)sb.n;
}
/* --- END RYU SECTION (Boost Software License 1.0) ------------------------------- */

/* ======================================================================= *
 *  READER
 * ======================================================================= */

typedef struct yjs__p {
    yjs_arena*           a;
    const unsigned char* s;
    size_t               n, i;
    uint32_t             depth, max_depth;
    yjs_member*          stk;       /* the heap stack, or the arena's top   */
    size_t               sn, scap;
    int                  down;      /* 1: slots below a->p + a->cap          */
    yjs_error*           err;
} yjs__p;

static int yjs__fail(yjs__p* p, int code, size_t at, const char* msg) {
    if (p->err && p->err->code == YJS_OK) {
        uint32_t line, col;
        if (at > p->n) at = p->n;
        yjs_line_col((const char*)p->s, p->n, at, &line, &col);
        p->err->code = code;
        p->err->offset = (uint32_t)at;
        p->err->line = line;
        p->err->column = col;
        snprintf(p->err->msg, sizeof p->err->msg, "%s", msg);
    }
    return code;
}

static int yjs__nomem(yjs__p* p) { return yjs__fail(p, YJS_ERR_NOMEM, p->i, "out of memory (the arena is full)"); }

YJS_API void yjs_line_col(const char* text, size_t n, size_t pos, uint32_t* line, uint32_t* column) {
    size_t k, l = 1, c = 1;
    if (!text) n = 0;
    for (k = 0; k < pos && k < n; k++) {
        if (text[k] == '\n') { l++; c = 1; }
        else c++;
    }
    if (line) *line = (uint32_t)l;
    if (column) *column = (uint32_t)c;
}

static yjs_member* yjs__slot(yjs__p* p, size_t k) {
    return p->down ? (yjs_member*)(void*)(p->a->p + p->a->cap) - (k + 1) : p->stk + k;
}

static int yjs__push(yjs__p* p, const yjs_member* m) {
    if (p->down) {
        yjs_arena* a = p->a;
        size_t need = (p->sn + 1) * sizeof(yjs_member);
        /* the top region grows down to a multiple of 8 below the block's end */
        if (need > a->cap || a->cap - need < a->used) { a->need = sizeof(yjs_member); return yjs__nomem(p); }
        a->top = need;
    } else if (p->sn == p->scap) {
        size_t nc = p->scap ? p->scap * 2 : 64;
        yjs_member* ns = (yjs_member*)YJS_REALLOC(p->stk, nc * sizeof(yjs_member));
        if (!ns) return yjs__nomem(p);
        p->stk = ns;
        p->scap = nc;
    }
    *yjs__slot(p, p->sn) = *m;
    p->sn++;
    return YJS_OK;
}

static void yjs__ws(yjs__p* p) {
    while (p->i < p->n) {
        unsigned char c = p->s[p->i];
        if (c != ' ' && c != '\n' && c != '\r' && c != '\t') break;
        p->i++;
    }
}

static int yjs__hex4(const unsigned char* s, uint32_t* out) {
    uint32_t v = 0;
    int k;
    for (k = 0; k < 4; k++) {
        unsigned c = s[k];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= c - '0';
        else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
        else return 0;
    }
    *out = v;
    return 1;
}

static size_t yjs__put_utf8(char* d, uint32_t cp) {
    if (cp < 0x80) { d[0] = (char)cp; return 1; }
    if (cp < 0x800) { d[0] = (char)(0xC0 | (cp >> 6)); d[1] = (char)(0x80 | (cp & 63)); return 2; }
    if (cp < 0x10000) {
        d[0] = (char)(0xE0 | (cp >> 12)); d[1] = (char)(0x80 | ((cp >> 6) & 63)); d[2] = (char)(0x80 | (cp & 63));
        return 3;
    }
    d[0] = (char)(0xF0 | (cp >> 18)); d[1] = (char)(0x80 | ((cp >> 12) & 63));
    d[2] = (char)(0x80 | ((cp >> 6) & 63)); d[3] = (char)(0x80 | (cp & 63));
    return 4;
}

/* The string whose quote is at p->i: checked in one pass, then copied or
 * decoded into the arena. */
static int yjs__string(yjs__p* p, const char** out, uint32_t* out_n) {
    const unsigned char* s = p->s;
    size_t i = p->i + 1, start = i, dlen = 0;
    bool esc = false;
    char* d;
    for (;;) {
        unsigned char c;
        if (i >= p->n) return yjs__fail(p, YJS_ERR_SYNTAX, p->i, "a string does not end");
        c = s[i];
        if (c == '"') break;
        if (c < 0x20) return yjs__fail(p, YJS_ERR_SYNTAX, i, "a control character in a string (escape it)");
        if (c == '\\') {
            esc = true;
            if (i + 1 >= p->n) return yjs__fail(p, YJS_ERR_SYNTAX, i, "a string does not end");
            switch (s[i + 1]) {
            case '"': case '\\': case '/': case 'b': case 'f': case 'n': case 'r': case 't':
                i += 2; dlen++; continue;
            case 'u': {
                uint32_t cp, lo;
                if (i + 6 > p->n || !yjs__hex4(s + i + 2, &cp)) return yjs__fail(p, YJS_ERR_SYNTAX, i, "a bad \\u escape");
                if (cp >= 0xDC00 && cp <= 0xDFFF) return yjs__fail(p, YJS_ERR_UTF8, i, "a lone surrogate");
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    if (i + 12 > p->n || s[i + 6] != '\\' || s[i + 7] != 'u' || !yjs__hex4(s + i + 8, &lo) || lo < 0xDC00 || lo > 0xDFFF)
                        return yjs__fail(p, YJS_ERR_UTF8, i, "a lone surrogate");
                    i += 12;
                    dlen += 4;
                    continue;
                }
                if (cp == 0) return yjs__fail(p, YJS_ERR_UTF8, i, "\\u0000 in a string");
                i += 6;
                dlen += cp < 0x80 ? 1 : cp < 0x800 ? 2 : 3;
                continue;
            }
            default:
                return yjs__fail(p, YJS_ERR_SYNTAX, i, "an unknown escape");
            }
        }
        if (c < 0x80) { i++; dlen++; continue; }
        {
            size_t m = yjs__utf8_seq(s + i, p->n - i);
            if (!m) return yjs__fail(p, YJS_ERR_UTF8, i, "invalid UTF-8");
            i += m;
            dlen += m;
        }
    }
    if (dlen > 0xFFFFFFFEu) return yjs__fail(p, YJS_ERR_LIMIT, p->i, "a string longer than 4 GiB");
    d = (char*)yjs__take(p->a, dlen + 1, 1);
    if (!d) return yjs__nomem(p);
    if (!esc) {
        memcpy(d, s + start, dlen);
    } else {
        size_t j = start, o = 0;
        while (j < i) {
            unsigned char c = s[j];
            if (c != '\\') { d[o++] = (char)c; j++; continue; }
            switch (s[j + 1]) {
            case 'b': d[o++] = '\b'; j += 2; break;
            case 'f': d[o++] = '\f'; j += 2; break;
            case 'n': d[o++] = '\n'; j += 2; break;
            case 'r': d[o++] = '\r'; j += 2; break;
            case 't': d[o++] = '\t'; j += 2; break;
            case 'u': {
                uint32_t cp = 0, lo = 0;   /* the check pass proved both escapes */
                (void)yjs__hex4(s + j + 2, &cp);
                j += 6;
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    (void)yjs__hex4(s + j + 2, &lo);
                    j += 6;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                o += yjs__put_utf8(d + o, cp);
            } break;
            default: d[o++] = (char)s[j + 1]; j += 2; break;   /* " \ / */
            }
        }
    }
    d[dlen] = '\0';
    *out = d;
    *out_n = (uint32_t)dlen;
    p->i = i + 1;
    return YJS_OK;
}

static int yjs__key_cmp(const char* a, size_t an, const char* b, size_t bn) {
    int c = memcmp(a, b, an < bn ? an : bn);
    if (c) return c;
    return an < bn ? -1 : an > bn;
}

static int yjs__member_cmp(const void* x, const void* y) {
    const yjs_member* a = (const yjs_member*)x;
    const yjs_member* b = (const yjs_member*)y;
    return yjs__key_cmp(a->key, a->key_len, b->key, b->key_len);
}

static int yjs__value(yjs__p* p, yjs_value** out);

static int yjs__container(yjs__p* p, yjs_value* v, int obj) {
    size_t base = p->sn, k, cnt;
    unsigned char close = obj ? '}' : ']';
    int rc;
    if (++p->depth > p->max_depth) return yjs__fail(p, YJS_ERR_LIMIT, p->i, "nested deeper than the limit");
    p->i++;
    yjs__ws(p);
    if (p->i < p->n && p->s[p->i] == close) {
        p->i++;
        p->depth--;
        return YJS_OK;
    }
    for (;;) {
        yjs_member m;
        memset(&m, 0, sizeof m);
        yjs__ws(p);
        if (obj) {
            if (p->i >= p->n || p->s[p->i] != '"') return yjs__fail(p, YJS_ERR_SYNTAX, p->i, "a key (a string) is expected");
            m.key_pos = (uint32_t)p->i;
            if ((rc = yjs__string(p, &m.key, &m.key_len)) != YJS_OK) return rc;
            yjs__ws(p);
            if (p->i >= p->n || p->s[p->i] != ':') return yjs__fail(p, YJS_ERR_SYNTAX, p->i, "':' is expected");
            p->i++;
        }
        if ((rc = yjs__value(p, &m.value)) != YJS_OK) return rc;
        if ((rc = yjs__push(p, &m)) != YJS_OK) return rc;
        yjs__ws(p);
        if (p->i < p->n && p->s[p->i] == ',') { p->i++; continue; }
        if (p->i < p->n && p->s[p->i] == close) { p->i++; break; }
        return yjs__fail(p, YJS_ERR_SYNTAX, p->i, obj ? "',' or '}' is expected" : "',' or ']' is expected");
    }
    cnt = p->sn - base;
    if (cnt > 0xFFFFFFFEu) return yjs__fail(p, YJS_ERR_LIMIT, v->pos, "a container with more than 4 G items");
    if (obj) {
        yjs_member* ms = (yjs_member*)yjs__take(p->a, cnt * sizeof(yjs_member), 8);
        if (!ms) return yjs__nomem(p);
        for (k = 0; k < cnt; k++) ms[k] = *yjs__slot(p, base + k);
        /* small objects: insertion sort; else qsort */
        if (cnt <= 16) {
            size_t a2, b2;
            for (a2 = 1; a2 < cnt; a2++) {
                yjs_member t = ms[a2];
                for (b2 = a2; b2 > 0 && yjs__member_cmp(&ms[b2 - 1], &t) > 0; b2--) ms[b2] = ms[b2 - 1];
                ms[b2] = t;
            }
        } else {
            qsort(ms, cnt, sizeof(yjs_member), yjs__member_cmp);
        }
        for (k = 1; k < cnt; k++)
            if (yjs__member_cmp(&ms[k - 1], &ms[k]) == 0) {
                uint32_t at = ms[k].key_pos > ms[k - 1].key_pos ? ms[k].key_pos : ms[k - 1].key_pos;
                char msg[96];
                snprintf(msg, sizeof msg, "a duplicate key \"%.48s\"", ms[k].key);
                return yjs__fail(p, YJS_ERR_DUPLICATE, at, msg);
            }
        v->u.members = ms;
    } else {
        yjs_value** it = (yjs_value**)yjs__take(p->a, cnt * sizeof(yjs_value*), 8);
        if (!it) return yjs__nomem(p);
        for (k = 0; k < cnt; k++) it[k] = yjs__slot(p, base + k)->value;
        v->u.items = it;
    }
    v->n = v->cap = (uint32_t)cnt;
    p->sn = base;
    if (p->down) p->a->top = base * sizeof(yjs_member);
    p->depth--;
    return YJS_OK;
}

static int yjs__value(yjs__p* p, yjs_value** out) {
    yjs_value* v;
    unsigned char c;
    yjs__ws(p);
    if (p->i >= p->n) return yjs__fail(p, YJS_ERR_SYNTAX, p->i, "a value is missing");
    v = (yjs_value*)yjs__take(p->a, sizeof(yjs_value), 8);
    if (!v) return yjs__nomem(p);
    memset(v, 0, sizeof *v);
    v->pos = (uint32_t)p->i;
    *out = v;
    c = p->s[p->i];
    switch (c) {
    case '{': v->type = YJS_OBJECT; return yjs__container(p, v, 1);
    case '[': v->type = YJS_ARRAY;  return yjs__container(p, v, 0);
    case '"': v->type = YJS_STRING; return yjs__string(p, &v->u.s, &v->n);
    case 't':
        if (p->n - p->i >= 4 && memcmp(p->s + p->i, "true", 4) == 0) { v->type = YJS_TRUE; p->i += 4; break; }
        return yjs__fail(p, YJS_ERR_SYNTAX, p->i, "not a JSON value");
    case 'f':
        if (p->n - p->i >= 5 && memcmp(p->s + p->i, "false", 5) == 0) { v->type = YJS_FALSE; p->i += 5; break; }
        return yjs__fail(p, YJS_ERR_SYNTAX, p->i, "not a JSON value");
    case 'n':
        if (p->n - p->i >= 4 && memcmp(p->s + p->i, "null", 4) == 0) { v->type = YJS_NULL; p->i += 4; break; }
        return yjs__fail(p, YJS_ERR_SYNTAX, p->i, "not a JSON value");
    default:
        if (c == '-' || (c >= '0' && c <= '9')) {
            unsigned f = 0;
            const char* why;
            size_t len = yjs__number_len(p->s + p->i, p->n - p->i, &f, &why);
            if (!len) return yjs__fail(p, YJS_ERR_SYNTAX, p->i, why);
            v->type = YJS_NUMBER;
            v->flags = (uint8_t)f;
            v->n = (uint32_t)len;
            v->u.s = yjs__dup(p->a, (const char*)p->s + p->i, len);
            if (!v->u.s) return yjs__nomem(p);
            if ((f & YJS_NUM_INT) && yjs__int_of(v->u.s, len, &v->i)) v->flags |= YJS_NUM_I64;
            p->i += len;
            break;
        }
        return yjs__fail(p, YJS_ERR_SYNTAX, p->i, c == '/' ? "a comment (JSON has none)" : "not a JSON value");
    }
    /* a literal or a number must end at a delimiter: "truex", "12a" */
    if (p->i < p->n) {
        unsigned char e = p->s[p->i];
        if (e != ',' && e != ']' && e != '}' && e != ' ' && e != '\t' && e != '\n' && e != '\r')
            return yjs__fail(p, YJS_ERR_SYNTAX, p->i, "a value runs into other text");
    }
    return YJS_OK;
}

YJS_API int yjs_parse(yjs_arena* a, const char* text, size_t n, const yjs_opts* opts,
                      yjs_value** out, yjs_error* err) {
    yjs__p p;
    yjs_error dummy;
    int rc;
    size_t max_bytes = opts && opts->max_bytes ? opts->max_bytes : YJS_DEFAULT_BYTES;
    if (!err) err = &dummy;
    memset(err, 0, sizeof *err);
    if (out) *out = NULL;
    memset(&p, 0, sizeof p);
    p.err = err;
    p.s = (const unsigned char*)text;
    p.n = n;
    if (!a || !out || (!text && n)) return yjs__fail(&p, YJS_ERR_ARG, 0, "a NULL argument");
    p.a = a;
    p.max_depth = opts && opts->max_depth ? opts->max_depth : YJS_DEFAULT_DEPTH;
    if (p.max_depth > YJS_MAX_DEPTH) p.max_depth = YJS_MAX_DEPTH;
    if (n > max_bytes || n > 0xFFFFFFFEu) return yjs__fail(&p, YJS_ERR_LIMIT, 0, "longer than the size limit");
    /* A fixed arena keeps the scratch stack at its top end; a heap arena
     * keeps it on the heap, since its blocks change under it. */
    p.down = a->heap_limit == 0;
    a->top = 0;
    if (n >= 3 && p.s[0] == 0xEF && p.s[1] == 0xBB && p.s[2] == 0xBF) p.i = 3;
    rc = yjs__value(&p, out);
    if (rc == YJS_OK) {
        yjs__ws(&p);
        if (p.i != n) rc = yjs__fail(&p, YJS_ERR_SYNTAX, p.i, "text after the value");
    }
    if (p.down) a->top = 0;
    else YJS_FREE(p.stk);
    if (rc != YJS_OK) *out = NULL;
    return rc;
}

/* ======================================================================= *
 *  ACCESS
 * ======================================================================= */

YJS_API yjs_member* yjs_member_n(const yjs_value* obj, const char* key, size_t n) {
    size_t lo = 0, hi;
    if (!obj || obj->type != YJS_OBJECT || !key) return NULL;
    hi = obj->n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const yjs_member* m = &obj->u.members[mid];
        int c = yjs__key_cmp(m->key, m->key_len, key, n);
        if (c == 0) return (yjs_member*)m;
        if (c < 0) lo = mid + 1;
        else hi = mid;
    }
    return NULL;
}

YJS_API yjs_value* yjs_get_n(const yjs_value* obj, const char* key, size_t n) {
    yjs_member* m = yjs_member_n(obj, key, n);
    return m ? m->value : NULL;
}

YJS_API yjs_value* yjs_get(const yjs_value* obj, const char* key) {
    return key ? yjs_get_n(obj, key, strlen(key)) : NULL;
}

YJS_API yjs_value* yjs_at(const yjs_value* arr, size_t i) {
    if (!arr || arr->type != YJS_ARRAY || i >= arr->n) return NULL;
    return arr->u.items[i];
}

YJS_API const char* yjs_string(const yjs_value* v, size_t* n) {
    if (!v || v->type != YJS_STRING) return NULL;
    if (n) *n = v->n;
    return v->u.s;
}

YJS_API int yjs_int64(const yjs_value* v, int64_t* out) {
    if (!out) return YJS_ERR_ARG;
    if (!v || v->type != YJS_NUMBER || !(v->flags & YJS_NUM_INT)) return YJS_ERR_TYPE;
    if (!(v->flags & YJS_NUM_I64)) return YJS_ERR_RANGE;
    *out = v->i;
    return YJS_OK;
}

YJS_API int yjs_double(const yjs_value* v, double* out) {
    if (!out) return YJS_ERR_ARG;
    if (!v || v->type != YJS_NUMBER) return YJS_ERR_TYPE;
    return yjs_parse_double(v->u.s, v->n, out);
}

YJS_API int yjs_fixed(const yjs_value* v, int decimals, int64_t* out) {
    if (!out) return YJS_ERR_ARG;
    if (!v || v->type != YJS_NUMBER) return YJS_ERR_TYPE;
    return yjs_parse_fixed(v->u.s, v->n, decimals, out);
}

YJS_API int yjs_bool(const yjs_value* v, bool* out) {
    if (!out) return YJS_ERR_ARG;
    if (!v || (v->type != YJS_TRUE && v->type != YJS_FALSE)) return YJS_ERR_TYPE;
    *out = v->type == YJS_TRUE;
    return YJS_OK;
}

/* ======================================================================= *
 *  BUILDER
 * ======================================================================= */

YJS_API yjs_value* yjs_new(yjs_arena* a, int type) {
    yjs_value* v;
    if (type != YJS_NULL && type != YJS_FALSE && type != YJS_TRUE && type != YJS_ARRAY && type != YJS_OBJECT) return NULL;
    v = (yjs_value*)yjs_alloc(a, sizeof(yjs_value));
    if (v) v->type = (uint8_t)type;
    return v;
}

static yjs_value* yjs__num_text(yjs_arena* a, const char* t, size_t n, unsigned f) {
    yjs_value* v = (yjs_value*)yjs_alloc(a, sizeof(yjs_value));
    if (!v) return NULL;
    v->type = YJS_NUMBER;
    v->flags = (uint8_t)f;
    v->n = (uint32_t)n;
    v->u.s = yjs__dup(a, t, n);
    if (!v->u.s) return NULL;
    if ((f & YJS_NUM_INT) && yjs__int_of(v->u.s, n, &v->i)) v->flags |= YJS_NUM_I64;
    return v;
}

YJS_API yjs_value* yjs_new_int(yjs_arena* a, int64_t x) {
    char buf[24];
    char* e = buf + sizeof buf;
    char* s;
    uint64_t m = x < 0 ? (uint64_t)0 - (uint64_t)x : (uint64_t)x;
    s = yjs__utoa(m, e);
    if (x < 0) *--s = '-';
    return yjs__num_text(a, s, (size_t)(e - s), YJS_NUM_INT);
}

YJS_API yjs_value* yjs_new_fixed(yjs_arena* a, int64_t x, int decimals) {
    char buf[48];
    char* e = buf + sizeof buf;
    char* s;
    uint64_t m = x < 0 ? (uint64_t)0 - (uint64_t)x : (uint64_t)x, p10 = 1, ip, fp;
    int k, nd;
    if (decimals < 0 || decimals > 18) return NULL;
    for (k = 0; k < decimals; k++) p10 *= 10u;
    ip = m / p10;
    fp = m % p10;
    if (fp == 0) return yjs_new_int(a, x < 0 ? -(int64_t)ip : (int64_t)ip);
    nd = decimals;
    while (fp % 10u == 0) { fp /= 10u; nd--; }
    s = e;
    for (k = 0; k < nd; k++) { *--s = (char)('0' + (int)(fp % 10u)); fp /= 10u; }
    *--s = '.';
    s = yjs__utoa(ip, s);
    if (x < 0) *--s = '-';
    return yjs__num_text(a, s, (size_t)(e - s), 0);
}

YJS_API yjs_value* yjs_new_double(yjs_arena* a, double x) {
    char t[YJS_DOUBLE_TEXT];
    unsigned f = 0;
    int n = yjs_format_double(x, t, sizeof t);
    if (n < 0 || !yjs_number_ok(t, (size_t)n, &f)) return NULL;
    return yjs__num_text(a, t, (size_t)n, f);
}

YJS_API yjs_value* yjs_new_number(yjs_arena* a, const char* text, size_t n) {
    unsigned f = 0;
    if (!text || !yjs_number_ok(text, n, &f)) return NULL;
    return yjs__num_text(a, text, n, f);
}

YJS_API yjs_value* yjs_new_string(yjs_arena* a, const char* s, size_t n) {
    yjs_value* v;
    if ((!s && n) || n > 0xFFFFFFFEu || !yjs_utf8_ok(s, n)) return NULL;
    v = (yjs_value*)yjs_alloc(a, sizeof(yjs_value));
    if (!v) return NULL;
    v->type = YJS_STRING;
    v->n = (uint32_t)n;
    v->u.s = yjs__dup(a, s ? s : "", n);
    return v->u.s ? v : NULL;
}

YJS_API yjs_value* yjs_new_stringz(yjs_arena* a, const char* s) {
    return s ? yjs_new_string(a, s, strlen(s)) : NULL;
}

/* Room for one more slot of size sz in a container. */
static int yjs__room(yjs_arena* a, yjs_value* c, size_t sz) {
    uint32_t nc;
    void* nb;
    if (c->n < c->cap) return YJS_OK;
    if (c->cap >= 0x7FFFFFFFu) return YJS_ERR_LIMIT;
    nc = c->cap ? c->cap * 2u : 4u;
    nb = yjs__take(a, (size_t)nc * sz, 8);
    if (!nb) return YJS_ERR_NOMEM;
    if (c->n) memcpy(nb, c->type == YJS_ARRAY ? (void*)c->u.items : (void*)c->u.members, (size_t)c->n * sz);
    if (c->type == YJS_ARRAY) c->u.items = (yjs_value**)nb;
    else c->u.members = (yjs_member*)nb;
    c->cap = nc;
    return YJS_OK;
}

YJS_API int yjs_push(yjs_arena* a, yjs_value* arr, yjs_value* v) {
    int rc;
    if (!a || !arr || !v || arr->type != YJS_ARRAY) return YJS_ERR_ARG;
    if ((rc = yjs__room(a, arr, sizeof(yjs_value*))) != YJS_OK) return rc;
    arr->u.items[arr->n++] = v;
    return YJS_OK;
}

YJS_API int yjs_set_n(yjs_arena* a, yjs_value* obj, const char* key, size_t n, yjs_value* v) {
    size_t lo = 0, hi;
    int rc;
    yjs_member* m;
    if (!a || !obj || !v || !key || obj->type != YJS_OBJECT) return YJS_ERR_ARG;
    if (n > 0xFFFFFFFEu || !yjs_utf8_ok(key, n)) return YJS_ERR_UTF8;
    hi = obj->n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = yjs__key_cmp(obj->u.members[mid].key, obj->u.members[mid].key_len, key, n);
        if (c == 0) { obj->u.members[mid].value = v; return YJS_OK; }
        if (c < 0) lo = mid + 1;
        else hi = mid;
    }
    if ((rc = yjs__room(a, obj, sizeof(yjs_member))) != YJS_OK) return rc;
    m = obj->u.members;
    if (lo < obj->n) memmove(&m[lo + 1], &m[lo], (obj->n - lo) * sizeof(yjs_member));
    m[lo].key = yjs__dup(a, key, n);
    if (!m[lo].key) {
        memmove(&m[lo], &m[lo + 1], (obj->n - lo) * sizeof(yjs_member));
        return YJS_ERR_NOMEM;
    }
    m[lo].key_len = (uint32_t)n;
    m[lo].key_pos = 0;
    m[lo].value = v;
    obj->n++;
    return YJS_OK;
}

YJS_API int yjs_set(yjs_arena* a, yjs_value* obj, const char* key, yjs_value* v) {
    return key ? yjs_set_n(a, obj, key, strlen(key), v) : YJS_ERR_ARG;
}

static yjs_value* yjs__copy(yjs_arena* a, const yjs_value* v, int depth) {
    yjs_value* r;
    uint32_t k;
    if (!v || depth > YJS_MAX_DEPTH) return NULL;
    r = (yjs_value*)yjs_alloc(a, sizeof(yjs_value));
    if (!r) return NULL;
    *r = *v;
    if (v->type == YJS_STRING || v->type == YJS_NUMBER) {
        r->u.s = yjs__dup(a, v->u.s, v->n);
        return r->u.s ? r : NULL;
    }
    if (v->type == YJS_ARRAY) {
        r->u.items = v->n ? (yjs_value**)yjs__take(a, (size_t)v->n * sizeof(yjs_value*), 8) : NULL;
        if (v->n && !r->u.items) return NULL;
        r->cap = v->n;
        for (k = 0; k < v->n; k++)
            if (!(r->u.items[k] = yjs__copy(a, v->u.items[k], depth + 1))) return NULL;
    } else if (v->type == YJS_OBJECT) {
        r->u.members = v->n ? (yjs_member*)yjs__take(a, (size_t)v->n * sizeof(yjs_member), 8) : NULL;
        if (v->n && !r->u.members) return NULL;
        r->cap = v->n;
        for (k = 0; k < v->n; k++) {
            r->u.members[k] = v->u.members[k];
            r->u.members[k].key = yjs__dup(a, v->u.members[k].key, v->u.members[k].key_len);
            r->u.members[k].value = yjs__copy(a, v->u.members[k].value, depth + 1);
            if (!r->u.members[k].key || !r->u.members[k].value) return NULL;
        }
    }
    return r;
}

YJS_API yjs_value* yjs_copy(yjs_arena* a, const yjs_value* v) { return yjs__copy(a, v, 0); }

/* ======================================================================= *
 *  WRITER
 * ======================================================================= */

typedef struct yjs__w {
    yjs_write_fn fn;
    void*        ctx;
    int          flags;
    int          rc;
    char         buf[512];   /* batches small writes into fewer calls    */
    size_t       n;
} yjs__w;

static void yjs__flush(yjs__w* w) {
    if (w->n && w->rc == YJS_OK && w->fn(w->ctx, w->buf, w->n) != 0) w->rc = YJS_ERR_WRITE;
    w->n = 0;
}

static void yjs__out(yjs__w* w, const char* s, size_t n) {
    if (w->rc != YJS_OK) return;
    if (n > sizeof w->buf - w->n) {
        yjs__flush(w);
        if (n > sizeof w->buf) {
            if (w->rc == YJS_OK && w->fn(w->ctx, s, n) != 0) w->rc = YJS_ERR_WRITE;
            return;
        }
    }
    memcpy(w->buf + w->n, s, n);
    w->n += n;
}

static void yjs__w_emit(void* ctx, const char* s, size_t n) {
    if (n) yjs__out((yjs__w*)ctx, s, n);
}

static void yjs__indent(yjs__w* w, int n) {
    static const char sp[] = "                                                                ";
    while (n > 0) {
        int k = n > 64 ? 64 : n;
        yjs__out(w, sp, (size_t)k);
        n -= k;
    }
}

static void yjs__wstr(yjs__w* w, const char* s, size_t n) {
    static const char hx[] = "0123456789abcdef";
    size_t i, run = 0;
    yjs__out(w, "\"", 1);
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\' || c < 0x20) {
            yjs__out(w, s + run, i - run);
            if (c == '"') yjs__out(w, "\\\"", 2);
            else if (c == '\\') yjs__out(w, "\\\\", 2);
            else {
                char u[6];
                u[0] = '\\'; u[1] = 'u'; u[2] = '0'; u[3] = '0';
                u[4] = hx[c >> 4]; u[5] = hx[c & 15u];
                yjs__out(w, u, 6);
            }
            run = i + 1;
        }
    }
    yjs__out(w, s + run, n - run);
    yjs__out(w, "\"", 1);
}

static bool yjs__scalar(const yjs_value* v) { return v->type != YJS_ARRAY && v->type != YJS_OBJECT; }

static void yjs__wval(yjs__w* w, const yjs_value* v, int ind, int depth) {
    uint32_t k;
    bool pretty = !(w->flags & YJS_WRITE_COMPACT);
    if (w->rc != YJS_OK) return;
    if (!v || depth > YJS_MAX_DEPTH) { w->rc = v ? YJS_ERR_LIMIT : YJS_ERR_ARG; return; }
    switch (v->type) {
    case YJS_NULL:  yjs__out(w, "null", 4); break;
    case YJS_TRUE:  yjs__out(w, "true", 4); break;
    case YJS_FALSE: yjs__out(w, "false", 5); break;
    case YJS_NUMBER:
        if (v->flags & YJS_NUM_I64) {
            char buf[24];
            char* e = buf + sizeof buf;
            char* s = yjs__utoa(v->i < 0 ? (uint64_t)0 - (uint64_t)v->i : (uint64_t)v->i, e);
            if (v->i < 0) *--s = '-';
            yjs__out(w, s, (size_t)(e - s));
        } else {
            yjs__canon_number(yjs__w_emit, w, v->u.s, v->n);
        }
        break;
    case YJS_STRING: yjs__wstr(w, v->u.s, v->n); break;
    case YJS_ARRAY: {
        bool flat = true;
        if (!v->n) { yjs__out(w, "[]", 2); break; }
        for (k = 0; k < v->n; k++) if (!v->u.items[k] || !yjs__scalar(v->u.items[k])) flat = false;
        if (flat || !pretty) {
            yjs__out(w, "[", 1);
            for (k = 0; k < v->n; k++) {
                if (k) yjs__out(w, pretty ? ", " : ",", pretty ? 2 : 1);
                yjs__wval(w, v->u.items[k], ind, depth + 1);
            }
            yjs__out(w, "]", 1);
            break;
        }
        yjs__out(w, "[\n", 2);
        for (k = 0; k < v->n; k++) {
            yjs__indent(w, ind + 2);
            yjs__wval(w, v->u.items[k], ind + 2, depth + 1);
            yjs__out(w, k + 1 < v->n ? ",\n" : "\n", k + 1 < v->n ? 2 : 1);
        }
        yjs__indent(w, ind);
        yjs__out(w, "]", 1);
    } break;
    case YJS_OBJECT:
        if (!v->n) { yjs__out(w, "{}", 2); break; }
        yjs__out(w, pretty ? "{\n" : "{", pretty ? 2 : 1);
        for (k = 0; k < v->n; k++) {
            const yjs_member* m = &v->u.members[k];
            if (pretty) yjs__indent(w, ind + 2);
            yjs__wstr(w, m->key, m->key_len);
            yjs__out(w, pretty ? ": " : ":", pretty ? 2 : 1);
            yjs__wval(w, m->value, ind + 2, depth + 1);
            if (pretty) yjs__out(w, k + 1 < v->n ? ",\n" : "\n", k + 1 < v->n ? 2 : 1);
            else if (k + 1 < v->n) yjs__out(w, ",", 1);
        }
        if (pretty) yjs__indent(w, ind);
        yjs__out(w, "}", 1);
        break;
    default:
        w->rc = YJS_ERR_ARG;
        break;
    }
}

YJS_API int yjs_write(const yjs_value* v, int flags, yjs_write_fn fn, void* ctx) {
    yjs__w w;
    if (!v || !fn) return YJS_ERR_ARG;
    w.fn = fn;
    w.ctx = ctx;
    w.flags = flags;
    w.rc = YJS_OK;
    w.n = 0;
    yjs__wval(&w, v, 0, 0);
    yjs__flush(&w);
    return w.rc;
}

typedef struct yjs__memw { char* buf; size_t cap, n; } yjs__memw;

static int yjs__mem_fn(void* ctx, const char* s, size_t n) {
    yjs__memw* m = (yjs__memw*)ctx;
    if (m->n < m->cap) {
        size_t k = m->cap - m->n < n ? m->cap - m->n : n;
        memcpy(m->buf + m->n, s, k);
    }
    m->n += n;
    return 0;
}

YJS_API size_t yjs_write_mem(const yjs_value* v, int flags, char* buf, size_t cap) {
    yjs__memw m;
    m.buf = buf;
    m.cap = buf ? cap : 0;
    m.n = 0;
    if (yjs_write(v, flags, yjs__mem_fn, &m) != YJS_OK) return 0;
    if (buf && cap) buf[m.n < cap ? m.n : cap - 1] = '\0';
    return m.n;
}

#ifdef __cplusplus
}
#endif

#endif /* YSP_JSON_IMPLEMENTATION_GUARD */
#endif /* YSP_JSON_IMPLEMENTATION */

/* ------------------------------------------------------------------------
 * This software is available under the MIT-0 (MIT No Attribution) license.
 *
 * Copyright (c) 2026 ysp contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY.
 * ------------------------------------------------------------------------ */

/* ------------------------------------------------------------------------
 * The RYU SECTION above (Copyright 2018 Ulf Adams) is available under the
 * Boost Software License 1.0:
 *
 * Boost Software License - Version 1.0 - August 17th, 2003
 *
 * Permission is hereby granted, free of charge, to any person or organization
 * obtaining a copy of the software and accompanying documentation covered by
 * this license (the "Software") to use, reproduce, display, distribute,
 * execute, and transmit the Software, and to prepare derivative works of the
 * Software, and to permit third-parties to whom the Software is furnished to
 * do so, all subject to the following:
 *
 * The copyright notices in the Software and this entire statement, including
 * the above license grant, this restriction and the following disclaimer,
 * must be included in all copies of the Software, in whole or in part, and
 * all derivative works of the Software, unless such copies or derivative
 * works are solely in the form of machine-executable object code generated by
 * a source language processor.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO EVENT
 * SHALL THE COPYRIGHT HOLDERS OR ANYONE DISTRIBUTING THE SOFTWARE BE LIABLE
 * FOR ANY DAMAGES OR OTHER LIABILITY, WHETHER IN CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 * ------------------------------------------------------------------------ */
