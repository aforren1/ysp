/* ysp/table.h - v0.3.0 - public domain single-header typed tables
 *
 *   A table of rows and named, typed columns, as one read-only block of
 *   bytes: conditions files, trial lists, event lists. A CSV parser with
 *   stated bounds and a number syntax that no locale can change writes the
 *   block into the caller's memory; the pack stores the same bytes, and a
 *   view checks them and reads them in place. ysp/trials.h takes a table
 *   as its conditions (desc.table); the pack tool and ysp/timeline.h read
 *   tables through this header too.
 *
 *   Written in the single-header style of the stb / sokol libraries. Pure
 *   computation: no heap, no OS calls, no threads, and no file I/O unless
 *   YTB_STDIO is defined. Needs only the C standard library (snprintf,
 *   ldexp). Targets every platform the compiler does; C99 is the floor: it
 *   builds as C99, C11 and C++17, and in the C dialect MSVC compiles by
 *   default.
 *
 *   ---------------------------------------------------------------------
 *   USAGE
 *   ---------------------------------------------------------------------
 *   Do this:
 *
 *       #define YSP_TABLE_IMPLEMENTATION
 *
 *   in *one* C or C++ file before including this header to create the
 *   implementation. Every other file just includes the header normally.
 *   ysp/trials.h's implementation includes this one's (as ysp/gfx.h does
 *   ysp/color.h's), so a program that defines YSP_TRIALS_IMPLEMENTATION
 *   does not define YSP_TABLE_IMPLEMENTATION in another file.
 *
 *   A conditions file:
 *
 *       target,contrast,word
 *       a,0.25,cat
 *       a,0.5,"dog, big"
 *       b,0.25,ox
 *
 *       static uint64_t arena[1 << 17];          // 1 MB, 8-byte aligned
 *       ytb_table tab;
 *       ytb_csv_desc cd = { 0 };
 *       cd.text = csv;
 *       cd.len = csv_len;
 *       cd.arena = arena;
 *       cd.arena_size = sizeof arena;
 *       if (!ytb_open_csv(&tab, &cd)) { fputs(ytb_error(&tab), stderr); return 1; }
 *       // "ysp_table: line 13, row 12, column 'contrast' (2): '0,5' is not
 *       //  a number; the decimal separator is '.'"
 *
 *       int contrast = ytb_col(&tab, "contrast");    // once, not per trial
 *       double c = ytb_num(&tab, row, contrast);      // 0.25
 *       const char* w = ytb_text(&tab, row, ytb_col(&tab, "word"));
 *       ytb_close(&tab);                   // forgets the view; the arena is yours
 *
 *   The block is position-independent, so it can be saved and mapped back:
 *
 *       fwrite(tab.base, 1, tab.size, f);              // the pack tool
 *       ...
 *       ytb_table t2;
 *       if (!ytb_open_view(&t2, bytes, len)) fputs(ytb_error(&t2), stderr);
 *
 *   ---------------------------------------------------------------------
 *   MODEL
 *   ---------------------------------------------------------------------
 *   A table has n_rows rows (data rows; the header line is not one) and
 *   n_cols columns, each with a name and a type:
 *     YTB_INTEGER   int32
 *     YTB_NUMBER    double
 *     YTB_STRING    UTF-8 text
 *   Every column is also categorical: its LEVELS are its distinct values,
 *   numbered from 0 in order of first appearance. A cell stores its level
 *   (16 bits); a level stores its text as first written and its value.
 *   Numeric cells are equal when their values are: "0.5" and "0.50" are one
 *   level, whose text is "0.5" if that came first, and -0 and 0 are one
 *   level. A data file written from the table therefore shows numbers as
 *   the conditions file wrote them, not as printf would.
 *   Lookups: ytb_col() by name, ytb_level() / ytb_text() /
 *   ytb_num() / ytb_int() by row and column, ytb_level_text() and
 *   ytb_level_num() by level, ytb_find() for the level of a value text
 *   (bytes for a STRING column, value for a numeric one, so find(contrast,
 *   "0.50") finds the level written "0.5"), ytb_n_levels(),
 *   ytb_level_bytes() for a hot loop (n_rows little-endian uint16 level
 *   numbers), ytb_hash() for the block's content hash (a log line can
 *   name the exact conditions file with it). A bad row, column or level
 *   gives -1, NULL, NaN or INT32_MIN, as each declaration says.
 *
 *   ---------------------------------------------------------------------
 *   CSV DIALECT
 *   ---------------------------------------------------------------------
 *   RFC 4180, with these rules stated:
 *     encoding    UTF-8; a leading byte order mark is skipped; any invalid
 *                 UTF-8 (overlong forms, surrogates, above U+10FFFF) or a
 *                 NUL byte is an error. A Latin-1 export from a
 *                 spreadsheet fails here instead of corrupting text.
 *     records     end with CRLF or LF; the last may have no line end; a CR
 *                 outside quotes that is not followed by LF is an error.
 *     delimiter   ',' (desc.delimiter 0), or ';' or a tab.
 *     header      the first record. Each name is a letter or '_', then
 *                 letters, digits or '_', at most YTB_MAX_NAME (63)
 *                 bytes, unique (case-sensitive); PsychoPy asks the same
 *                 of a conditions file.
 *     fields      not trimmed: spaces are data. A field that starts with a
 *                 quote is quoted: inside, "" is a quote, and delimiters,
 *                 CR and LF are data; after the closing quote only a
 *                 delimiter or a record end may follow. A quote inside an
 *                 unquoted field is an error.
 *     shape       every record has the header's number of fields. A record
 *                 whose fields are all empty (spreadsheets write ",,,,"
 *                 rows; an empty line is one) is skipped and counted in
 *                 n_skipped.
 *     empty cell  the level "" of a STRING column. In an INTEGER or NUMBER
 *                 column it is an error, unless desc.allow_empty, which
 *                 makes it NaN (one level, text "") and the column NUMBER.
 *     not taken   comments, a type row, decimal commas, thousands
 *                 separators, hex, inf, nan, n/a, spaces around numbers,
 *                 other encodings.
 *   BOUNDS, each an error that names the line, row and column:
 *     YTB_MAX_COLUMNS (256) columns, YTB_MAX_ROWS (32767) data rows
 *     (row and level numbers are 16-bit), YTB_MAX_FIELD (4096) bytes in
 *     a field after unquoting, 2 GiB of input.
 *   TYPES. A column named in desc.types[] has that type and each of its
 *   cells must parse as it. Any other column is INTEGER when every
 *   non-empty cell is an integer in int32, else NUMBER when every non-empty
 *   cell is a number (one out of range or with too many digits counts, so
 *   that cell is reported rather than the column turning into text), else
 *   STRING; a column with no non-empty cell is STRING. Declare STRING for
 *   codes such as "007" that must keep their text: inferred, "007" is the
 *   integer 7 (its level text stays "007").
 *
 *   ---------------------------------------------------------------------
 *   NUMBERS
 *   ---------------------------------------------------------------------
 *   The syntax, and nothing else:
 *       integer = [+|-] digit {digit}                       (fits int32)
 *       number  = [+|-] ( digits [ "." {digit} ] | "." digits )
 *                 [ (e|E) [+|-] digits ]
 *   At most YTB_MAX_DIGITS (19) significant digits, counted from the
 *   first nonzero digit to the last nonzero one (pandas and spreadsheets
 *   write at most 17). More is YTB_NUM_DIGITS. A value whose magnitude
 *   rounds above the largest double, or a nonzero value that rounds to 0,
 *   is YTB_NUM_RANGE; subnormals are kept. The conversion is correctly
 *   rounded, ties to even, and reads no locale: Clinger's fast path when
 *   the digits fit 2^53 and the exponent is within 22, else exact big
 *   integers (M x 5^E rounded to 53 bits; or M 2^s divided by 5^-E by
 *   restoring division, the remainder as the sticky bit, subnormals rounded
 *   at their own bit). ytb_parse_number() and ytb_parse_int() are the
 *   parser on its own.
 *
 *   ---------------------------------------------------------------------
 *   THE BLOCK (format 1)
 *   ---------------------------------------------------------------------
 *   Little-endian, every section 8-byte aligned, every offset from the
 *   block's start, so the bytes are valid wherever they are mapped:
 *     header   48 bytes: "PSTB", u32 format (1), u32 n_rows, u32 n_cols,
 *              u32 n_skipped, u32 flags (0), u64 size, u64 hash, u64
 *              pool_off (where the text pool starts).
 *     columns  n_cols x 32 bytes: u32 name_off, u32 name_len, u32 type,
 *              u32 n_levels, u64 rows_off, u64 levels_off.
 *     rows     per column, n_rows u16 level numbers at rows_off.
 *     levels   per column, n_levels x 16 bytes at levels_off: u32
 *              text_off, u32 text_len, f64 value (the number for INTEGER
 *              and NUMBER, NaN for an empty cell, 0 for STRING).
 *     pool     from pool_off to size: UTF-8 strings, each followed by a
 *              NUL, so a text is a C string in place; zero padding to 8.
 *   The hash: over the block's little-endian 64-bit words, with word 4 (the
 *   hash itself) read as 0, four lanes over words i, i+1, i+2, i+3 (lane
 *   k starts at size + k; each word w: h = (h ^ w) * 0x9E3779B97F4A7C15,
 *   h ^= h >> 29), the words past the last full group of four into lane 0;
 *   then h0 ^ h1 * 0xC2B2AE3D27D4EB4F ^ h2 * 0x165667B19E3779F9 ^
 *   h3 * 0xD6E8FEB86659FD93, folded (x ^= x >> 32; x *= 0xD6E8FEB86659FD93;
 *   x ^= x >> 32). Not cryptographic: it finds corruption, not forgery.
 *   The parse writes the columns, then every column's rows, then every
 *   column's levels, then the pool, in that order, so the same input gives
 *   the same bytes on every compiler and platform.
 *   ytb_open_view() refuses a block (with a message) unless: the magic,
 *   format and flags are right; size fits the bytes given and the hash
 *   matches; the counts are in bounds; every offset and length is inside
 *   the block and aligned; the pool is valid UTF-8 (NULs between texts);
 *   every name and text lies in the pool and ends with a NUL (a text is
 *   its bytes up to its first NUL);
 *   every name is a valid, unique name; every row's level is below its
 *   column's n_levels and n_levels is at most n_rows; every INTEGER value is
 *   integral in int32. O(cells + text). A
 *   big-endian host refuses to parse or view: the block is little-endian
 *   by definition and no supported platform is big-endian.
 *
 *   ---------------------------------------------------------------------
 *   ERRORS
 *   ---------------------------------------------------------------------
 *   ytb_open_csv() and ytb_open_view() clear ytb_error() on entry, and
 *   return false and fill it on a failure, and
 *   for a parse err_line, err_row and err_col (1-based; 0 when not
 *   applicable, row 0 being the header): "ysp_table: line L, row R, column
 *   'name' (C): what". The line is the one the record starts on, which a
 *   quoted line break makes differ from row + 1. On a failure the view is
 *   empty (base NULL, no rows). The lookups return -1, NULL, NaN or
 *   INT32_MIN on a bad argument and never write the message.
 *   ytb_parse_number() and ytb_parse_int() return YTB_NUM_OK (0) or a
 *   negative YTB_NUM_* reason.
 *
 *   ---------------------------------------------------------------------
 *   MEMORY AND COST
 *   ---------------------------------------------------------------------
 *   Nothing allocates. ytb_open_csv() writes the block at the start of
 *   desc.arena (8-byte aligned) and uses the space after it as scratch: an
 *   8-byte entry per cell (at the arena's top, written while scanning), the
 *   unquoted text, and a hash table per column in turn. tab.need is set
 *   once the input is scanned (with arena NULL the call only sets it): about
 *   24 bytes per cell plus three times the input. The finished block is
 *   tab.size bytes, 2 bytes per cell plus 16 per level plus the text; the
 *   arena space after it is free again. Two passes over the input (scan,
 *   then write a column at a time).
 *
 *   ---------------------------------------------------------------------
 *   FILE LOADER (YTB_STDIO)
 *   ---------------------------------------------------------------------
 *   With YTB_STDIO defined before the implementation, ytb_open_csv_file()
 *   reads a file into the top of desc.arena and parses it with the rest.
 *   The path is UTF-8 on every platform (on Windows it is converted to
 *   UTF-16 for _wfopen, so a path with accents works). Off by default: the
 *   header otherwise touches no file.
 *
 *   ---------------------------------------------------------------------
 *   BUILDING
 *   ---------------------------------------------------------------------
 *   Nothing to link but the math library. Define YTB_API to override the
 *   default `extern` linkage.
 *
 *   ---------------------------------------------------------------------
 *   CHANGELOG
 *   ---------------------------------------------------------------------
 *   v0.3.0 - YTB_MAX_COLUMNS is 256 (was 64): a trial row with many
 *          recorded stimuli is wider than 64 columns (docs/
 *          experiment_format.md, gap G10), and ysp/trials.h's writer keeps
 *          to this bound so its files read back. The block format, the
 *          parser and the hash are v0.2.0's; a block of up to 64 columns is
 *          the same bytes. ytb_csv_desc (types[]) grew from 1 KB to 4 KB
 *          and the parse's stack by 3 KB.
 *   v0.2.0 - the repository conventions (README.md, Conventions). Breaks
 *          the API: ytb_csv() is ytb_open_csv(), ytb_csv_file() is
 *          ytb_open_csv_file(), ytb_view() is ytb_open_view(), and
 *          ytb_close() is new (it zeroes the handle; the block is the
 *          caller's). The YTB_NUM_* reasons are negative (-1, -2, -3), so
 *          every call that can fail returns 0 or a negative code. The USAGE
 *          block zeroes the desc and sets fields, so it builds as C99 and
 *          C++17. The parser and the block format are v0.1.0's.
 *   v0.1.0 - first version, split out of the ysp/trials.h v0.2 design so
 *          the pack tool and ysp/timeline.h can read the same tables: the
 *          CSV parser, the block format (1), the view and its checks, the
 *          number parser, the lookups, the opt-in file loader.
 *
 *   STATUS: v0.3.0 on 2026-10-21: 256 columns parse, view and read back
 *   by name, 257 are refused (tests/adapt/table_test.c); mutants b-01 and
 *   b-02 at the bound. The pinned block hash is unchanged.
 *   STATUS: v0.2.0 on 2026-10-10: built and tested with MSVC 19.44 /W4 /WX
 *   (C and C++17) and MinGW-w64 gcc -Werror; the test adds ytb_close() on a
 *   zeroed handle, after a failed open and twice. The rest is v0.1.0's:
 *   v0.1.0, 2026-10-07. Built and run on Windows 11 with MinGW-w64
 *   gcc 16.1 as C11, C99 and C++17 under -Wall -Wextra -Wpedantic -Wshadow
 *   -Werror and with MSVC 19.44 under /W4 /WX, in its default C dialect
 *   and as /std:c++17; on Linux (WSL2) with gcc 11.4 as C11 and C++17
 *   under -fsanitize=address,undefined -fno-sanitize-recover=all with no
 *   diagnostic. tests/adapt/table_test.c checks ytb_parse_number()
 *   against strtod() in the C locale bit for bit: 10,000,084 strings
 *   (exact halfway points and their neighbors, round trips of random
 *   doubles and subnormals, the range ends, the 19-digit limit) with 0
 *   disagreements on gcc (mingw-w64 strtod) and on MSVC (UCRT), 200,084 on
 *   Linux (glibc); again after switching to a decimal-comma locale
 *   (Windows). Every dialect rule and bound of CSV with its line, row and
 *   column; the block's hash pinned across compilers; every single-bit
 *   flip and truncation of a block refused; structural faults behind a
 *   valid hash refused by name. The Python binding of ysp/trials.h
 *   compares the parser with Python's csv module (40 random files) and
 *   float() (5000 values): all equal. Fuzzed (tests/fuzz/table_fuzz.c)
 *   under MSVC libFuzzer with ASan: 5,906,077 inputs in 1351 s, no crash;
 *   the 958 corpus files replayed on Linux under ASan and UBSan with no
 *   report. Mutations: 25 in tests/mutate/table.toml, 24 killed, 1
 *   equivalent. Costs measured (Iris Xe laptop, AC, measurement lock,
 *   examples/trials/bench.c, 21 rounds, medians, gcc 16.1 -O2 / MSVC 19.44
 *   /O2) on 10,000 rows x 8 columns: parse 181 / 188 MB/s for words,
 *   170 / 97 MB/s for %.17g numbers, 200 / 218 MB/s with 30 % quoted
 *   fields; ytb_view() of the 2 to 3 MB blocks 0.26 to 0.38 / 0.21 to
 *   0.31 ms. docs/table.md has the tables.
 *   What is NOT done: no run on macOS or a big-endian host (parse and view
 *   refuse there); the hash detects corruption, not forgery.
 *
 *   ---------------------------------------------------------------------
 *   LICENSE: public domain / MIT-0, see end of file.
 */
#ifndef YSP_TABLE_H_INCLUDED
#define YSP_TABLE_H_INCLUDED

#define YTB_VERSION_MAJOR 0
#define YTB_VERSION_MINOR 3
#define YTB_VERSION_PATCH 0
#define YTB_VERSION_STRING "0.3.0"

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef YTB_API
#define YTB_API extern
#endif

#define YTB_MAX_COLUMNS 256     /* v0.3.0: was 64 (a trial row is wider) */
#define YTB_MAX_ROWS    32767   /* row and level numbers are 16-bit       */
#define YTB_MAX_FIELD   4096    /* bytes in one field after unquoting     */
#define YTB_MAX_NAME    63      /* bytes in a column name                 */
#define YTB_MAX_DIGITS  19      /* significant digits in a number         */
#define YTB_FORMAT      1       /* the block format this header writes    */
#define YTB_HEADER_SIZE 48

typedef enum ytb_type {
    YTB_AUTO = 0,     /* in a declaration: infer the type                */
    YTB_INTEGER,      /* int32                                           */
    YTB_NUMBER,       /* double                                          */
    YTB_STRING        /* UTF-8 text                                      */
} ytb_type;

typedef struct ytb_decl {
    const char* name;
    ytb_type  type;
} ytb_decl;

typedef struct ytb_csv_desc {
    const void* text;           /* the file's bytes: UTF-8, BOM allowed     */
    size_t      len;
    char        delimiter;      /* 0 = ','; also ';' or '\t'                */
    bool        allow_empty;    /* an empty cell of a numeric column is NaN
                                 * (the column is NUMBER) instead of an error */
    ytb_decl  types[YTB_MAX_COLUMNS];   /* declared types, by name      */
    int         n_types;
    void*       arena;          /* 8-byte aligned; NULL: only compute need  */
    size_t      arena_size;
} ytb_csv_desc;

/* A read-only view of a table block. The block lives in the caller's arena
 * or in a pack and must outlive the view and every handle that uses it. */
typedef struct ytb_table {
    const unsigned char* base;  /* the block (FORMAT)                       */
    size_t      size;
    int         n_rows;
    int         n_cols;
    int         n_skipped;      /* CSV records with every field empty       */
    size_t      need;           /* arena bytes ytb_open_csv() needs            */
    int         err_line;       /* where a parse failed, 1-based; 0 = n/a   */
    int         err_row;        /* data row, 1-based; 0 = the header or n/a */
    int         err_col;        /* column, 1-based; 0 = n/a                 */
    char        error[256];
} ytb_table;

/* Parse CSV into a block at the start of desc->arena and view it. Returns
 * false with ytb_error() set; `need` is set either way once the input has
 * been scanned. With desc->arena NULL it only sets `need` (and returns
 * false with the message "ysp_table: no arena (need N bytes)"). */
YTB_API bool ytb_open_csv(ytb_table* tab, const ytb_csv_desc* desc);

/* Check a block (from a pack, or saved from a parse) and view it in place. */
YTB_API bool ytb_open_view(ytb_table* tab, const void* bytes, size_t len);

/* Forget the view: zero the handle. The block belongs to the caller (an arena
 * or a pack), so nothing is released; every open has a close. Safe on a
 * zeroed handle, after a failed open and twice. */
YTB_API void ytb_close(ytb_table* tab);

YTB_API const char* ytb_error(const ytb_table* tab);
YTB_API const char* ytb_version(void);
YTB_API const char* ytb_type_name(ytb_type type);

/* Lookups. A bad table, row, column or level gives -1, NULL, NaN or
 * INT32_MIN as stated. */
YTB_API int         ytb_col(const ytb_table* tab, const char* name);       /* -1 */
YTB_API const char* ytb_col_name(const ytb_table* tab, int col);           /* NULL */
YTB_API ytb_type  ytb_col_type(const ytb_table* tab, int col);           /* AUTO */
YTB_API int         ytb_n_levels(const ytb_table* tab, int col);           /* -1 */
YTB_API int         ytb_level(const ytb_table* tab, int row, int col);     /* -1 */
YTB_API int         ytb_find(const ytb_table* tab, int col, const char* value); /* -1 */
YTB_API const char* ytb_text(const ytb_table* tab, int row, int col);      /* NULL */
YTB_API const char* ytb_level_text(const ytb_table* tab, int col, int level); /* NULL */
YTB_API double      ytb_num(const ytb_table* tab, int row, int col);       /* NaN */
YTB_API double      ytb_level_num(const ytb_table* tab, int col, int level); /* NaN */
YTB_API int32_t     ytb_int(const ytb_table* tab, int row, int col);       /* INT32_MIN */
YTB_API uint64_t    ytb_hash(const ytb_table* tab);                        /* 0 */

/* The column's level per row as n_rows little-endian uint16 values, for a
 * caller's hot loop (level of row r = p[2r] | p[2r+1] << 8). NULL on a bad
 * column. */
YTB_API const unsigned char* ytb_level_bytes(const ytb_table* tab, int col);

/* The number syntax (NUMBERS), on its own. Return YTB_NUM_OK and the value
 * in *out, or the reason, which is negative. */
#define YTB_NUM_OK     0
#define YTB_NUM_SYNTAX (-1)  /* not the syntax                            */
#define YTB_NUM_DIGITS (-2)  /* more than YTB_MAX_DIGITS significant digits */
#define YTB_NUM_RANGE  (-3)  /* overflows a double (int32 for an integer),
                              * or a nonzero value rounds to 0              */
YTB_API int ytb_parse_number(const char* s, size_t n, double* out);
YTB_API int ytb_parse_int(const char* s, size_t n, int32_t* out);

#ifdef YTB_STDIO
/* Read the file at `path` (UTF-8, also on Windows) into the top of
 * desc->arena and parse it with the rest of the arena; desc->text and
 * desc->len are ignored. */
YTB_API bool ytb_open_csv_file(ytb_table* tab, const char* path, const ytb_csv_desc* desc);
#endif

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* YSP_TABLE_H_INCLUDED */

/* ======================================================================= *
 *                             IMPLEMENTATION                              *
 * ======================================================================= */
#ifdef YSP_TABLE_IMPLEMENTATION
#ifndef YSP_TABLE_IMPLEMENTATION_GUARD
#define YSP_TABLE_IMPLEMENTATION_GUARD

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#ifdef NAN
    #define YTB__NAN ((double)NAN)
#else
    #define YTB__NAN (HUGE_VAL - HUGE_VAL)
#endif

YTB_API const char* ytb_version(void) { return YTB_VERSION_STRING; }

YTB_API const char* ytb_type_name(ytb_type type) {
    switch (type) {
    case YTB_INTEGER: return "integer";
    case YTB_NUMBER:  return "number";
    case YTB_STRING:  return "string";
    default:            return "auto";
    }
}

/* --- little-endian fields ---------------------------------------------- */

/* Plain loads: a big-endian host refuses every parse and view, so a block
 * is only ever read on a little-endian one, where these are the
 * little-endian reads (MSVC did not fuse the byte-wise form into one load;
 * the view was 2x slower, docs/table.md). */
static uint32_t ytb__rd32(const unsigned char* p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}
static uint64_t ytb__rd64(const unsigned char* p) {
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
}
static void ytb__wr32(unsigned char* p, uint32_t v) {
    p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}
static void ytb__wr64(unsigned char* p, uint64_t v) {
    ytb__wr32(p, (uint32_t)v);
    ytb__wr32(p + 4, (uint32_t)(v >> 32));
}
static double ytb__rdf64(const unsigned char* p) {
    uint64_t u = ytb__rd64(p);
    double d;
    memcpy(&d, &u, sizeof(d));
    return d;
}
static void ytb__wrf64(unsigned char* p, double d) {
    uint64_t u;
    memcpy(&u, &d, sizeof(u));
    ytb__wr64(p, u);
}

/* The block is little-endian by definition; a double's bytes are read as a
 * uint64, which is only the IEEE pattern when the host stores both the same
 * way, so a big-endian host refuses rather than mis-reading. */
static bool ytb__little(void) {
    uint32_t one = 1;
    unsigned char b;
    memcpy(&b, &one, 1);
    return b == 1;
}

/* The block's content hash, over its little-endian 64-bit words (size is
 * a multiple of 8) with the hash field (word 4) read as zero, so every
 * header bit is covered too: h = (h ^ w) * K, h ^= h >> 29, per word, from
 * h = size, then a final avalanche. A word at a time: byte-wise FNV-1a cost
 * 2 ms on a 2 MB block, more than the rest of the view (measured,
 * docs/table.md). */
static uint64_t ytb__block_hash(const unsigned char* b, size_t size) {
    /* Four lanes over words i, i+1, i+2, i+3, so the multiplies overlap;
     * a 48-byte header and 8-byte alignment leave size a multiple of 8,
     * not of 32, so the tail runs lane 0. */
    uint64_t h[4], w;
    size_t i = 0;
    int k;
    for (k = 0; k < 4; k++) h[k] = (uint64_t)size + (uint64_t)k;
    for (; i + 32 <= size; i += 32)
        for (k = 0; k < 4; k++) {
            size_t at = i + 8u * (size_t)k;
            w = (at == 32) ? 0u : ytb__rd64(b + at);
            h[k] = (h[k] ^ w) * 0x9E3779B97F4A7C15ULL;
            h[k] ^= h[k] >> 29;
        }
    for (; i + 8 <= size; i += 8) {
        w = (i == 32) ? 0u : ytb__rd64(b + i);
        h[0] = (h[0] ^ w) * 0x9E3779B97F4A7C15ULL;
        h[0] ^= h[0] >> 29;
    }
    w = h[0] ^ (h[1] * 0xC2B2AE3D27D4EB4FULL) ^ (h[2] * 0x165667B19E3779F9ULL) ^ (h[3] * 0xD6E8FEB86659FD93ULL);
    w ^= w >> 32;
    w *= 0xD6E8FEB86659FD93ULL;
    w ^= w >> 32;
    return w;
}

/* --- numbers ----------------------------------------------------------- */

/* A big unsigned integer, little-endian 32-bit words. 40 words hold the
 * largest operand the conversion builds (about 930 bits: M << s for a
 * denominator 5^345). */
#define YTB__BIGW 40
typedef struct ytb__big {
    uint32_t w[YTB__BIGW];
    int      n;     /* words in use; w[n-1] != 0 unless n == 0 */
} ytb__big;

static void ytb__big_set(ytb__big* b, uint64_t v) {
    b->w[0] = (uint32_t)v;
    b->w[1] = (uint32_t)(v >> 32);
    b->n = b->w[1] ? 2 : (b->w[0] ? 1 : 0);
}

static void ytb__big_mul(ytb__big* b, uint32_t m) {
    uint64_t carry = 0;
    int i;
    for (i = 0; i < b->n; i++) {
        uint64_t t = (uint64_t)b->w[i] * m + carry;
        b->w[i] = (uint32_t)t;
        carry = t >> 32;
    }
    if (carry && b->n < YTB__BIGW) b->w[b->n++] = (uint32_t)carry;
}

static void ytb__big_pow5(ytb__big* b, int q) {
    ytb__big_set(b, 1);
    while (q >= 13) { ytb__big_mul(b, 1220703125u); q -= 13; }   /* 5^13 */
    while (q > 0) { ytb__big_mul(b, 5u); q--; }
}

static void ytb__big_shl(ytb__big* b, int s) {
    int ws = s / 32, bs = s % 32, i, n = b->n;
    if (n == 0 || s <= 0) return;
    if (n + ws + 1 > YTB__BIGW) return;   /* cannot happen in range; callers bound s */
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

static int ytb__bits64(uint64_t v) {
    int n = 0;
    while (v) { n++; v >>= 1; }
    return n;
}

static int ytb__bits128(uint64_t hi, uint64_t lo) {
    return hi ? 64 + ytb__bits64(hi) : ytb__bits64(lo);
}

static int ytb__big_bits(const ytb__big* b) {
    if (b->n == 0) return 0;
    return (b->n - 1) * 32 + ytb__bits64(b->w[b->n - 1]);
}

static int ytb__big_bit(const ytb__big* b, int i) {
    int w = i / 32;
    if (i < 0 || w >= b->n) return 0;
    return (int)((b->w[w] >> (i % 32)) & 1u);
}

/* Any bit below bit i set? */
static bool ytb__big_any_below(const ytb__big* b, int i) {
    int w, k;
    if (i <= 0) return false;
    w = i / 32;
    for (k = 0; k < w && k < b->n; k++) if (b->w[k]) return true;
    if (w < b->n && (i % 32) && (b->w[w] & ((1u << (i % 32)) - 1u))) return true;
    return false;
}

/* 64 bits of b starting at bit i. */
static uint64_t ytb__big_get64(const ytb__big* b, int i) {
    uint64_t v = 0;
    int k;
    for (k = 0; k < 64; k++) v |= (uint64_t)ytb__big_bit(b, i + k) << k;
    return v;
}

static int ytb__big_cmp(const ytb__big* a, const ytb__big* b) {
    int i;
    if (a->n != b->n) return a->n < b->n ? -1 : 1;
    for (i = a->n - 1; i >= 0; i--)
        if (a->w[i] != b->w[i]) return a->w[i] < b->w[i] ? -1 : 1;
    return 0;
}

static void ytb__big_sub(ytb__big* a, const ytb__big* b) {
    int64_t borrow = 0;
    int i;
    for (i = 0; i < a->n; i++) {
        int64_t t = (int64_t)a->w[i] - (i < b->n ? (int64_t)b->w[i] : 0) - borrow;
        borrow = t < 0;
        a->w[i] = (uint32_t)(t + (borrow ? ((int64_t)1 << 32) : 0));
    }
    while (a->n > 0 && a->w[a->n - 1] == 0) a->n--;
}

/* r = 2r + bit */
static void ytb__big_dbl(ytb__big* r, int bit) {
    uint32_t carry = (uint32_t)bit;
    int i;
    for (i = 0; i < r->n; i++) {
        uint32_t v = r->w[i];
        r->w[i] = (v << 1) | carry;
        carry = v >> 31;
    }
    if (carry && r->n < YTB__BIGW) r->w[r->n++] = carry;
}

/* Round mant (with the half bit and the sticky bits below it) to nearest,
 * ties to even. */
static uint64_t ytb__round(uint64_t mant, int half, bool sticky) {
    if (half && (sticky || (mant & 1u))) mant++;
    return mant;
}

/* 128-bit helpers for the middle path: |E| <= 27, so 5^|E| < 2^63 and every
 * operand fits two 64-bit words. Compiler 128-bit types where they exist,
 * else the same results from 32-bit pieces. */
#if defined(__SIZEOF_INT128__)
__extension__ typedef unsigned __int128 ytb__u128;
#define YTB__HAVE_U128 1
#endif

/* hi:lo = a * b */
static void ytb__mul64(uint64_t a, uint64_t b, uint64_t* hi, uint64_t* lo) {
#ifdef YTB__HAVE_U128
    ytb__u128 p = (ytb__u128)a * b;
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

/* floor(hi:lo / d) and whether a remainder is left, for a quotient below
 * 2^64 (hi < d). */
static uint64_t ytb__div128(uint64_t hi, uint64_t lo, uint64_t d, bool* rem) {
#ifdef YTB__HAVE_U128
    ytb__u128 n = ((ytb__u128)hi << 64) | lo;
    *rem = (n % d) != 0;
    return (uint64_t)(n / d);
#else
    /* Restoring division, a bit at a time; r < d < 2^63 keeps 2r + 1 in
     * 64 bits. */
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

static int ytb__bits128(uint64_t hi, uint64_t lo);

static const double ytb__p10[23] = {
    1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11,
    1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22
};

/* M x 10^E, M in (0, 10^19), correctly rounded, ties to even. */
static int ytb__convert(uint64_t m, int e, double* out) {
    ytb__big n, d, r;
    uint64_t mant, q;
    int b, shift, half, i, s, bq, u, scale;
    bool sticky;
    double v;

    if (e > 310) return YTB_NUM_RANGE;        /* >= 1e310 */
    if (e < -345) return YTB_NUM_RANGE;       /* < 1e-326, rounds to 0 */
    /* Clinger's fast path: both operands exact, one rounding. */
    if (m <= ((uint64_t)1 << 53) && e >= -22 && e <= 22) {
        v = (double)m;
        *out = e >= 0 ? v * ytb__p10[e] : v / ytb__p10[-e];
        return YTB_NUM_OK;
    }
    if (e >= -27 && e <= 27) {
        /* 5^|E| < 2^63: two-word arithmetic, exact, with the same rounding
         * as the big-integer path below. */
        uint64_t p5 = 1, hi, lo;
        int k;
        for (k = 0; k < (e < 0 ? -e : e); k++) p5 *= 5u;
        if (e >= 0) {
            ytb__mul64(m, p5, &hi, &lo);
            b = ytb__bits128(hi, lo);
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
                mant = ytb__round(mant, half, sticky);
            }
            v = ldexp((double)mant, e + shift);
            *out = v;
            return YTB_NUM_OK;
        } else {
            /* q = floor(M 2^s / 5^q), 2^55 < q < 2^57; the numerator has
             * 56 + bits(5^q) <= 119 bits. */
            int bd = ytb__bits64(p5);
            s = 56 - ytb__bits64(m) + bd;
            if (s >= 64) { hi = m << (s - 64); lo = 0; }
            else if (s > 0) { hi = m >> (64 - s); lo = m << s; }
            else if (s == 0) { hi = 0; lo = m; }
            else { hi = 0; lo = m; p5 <<= -s; }
            q = ytb__div128(hi, lo, p5, &sticky);
            bq = ytb__bits64(q);
            scale = s - e;
            u = bq - 1 - scale - 52;
            if (u < -1074) u = -1074;
            shift = u + scale;
            mant = q >> shift;
            half = (int)((q >> (shift - 1)) & 1u);
            if (shift >= 2 && (q & ((((uint64_t)1) << (shift - 1)) - 1u))) sticky = true;
            mant = ytb__round(mant, half, sticky);
            *out = ldexp((double)mant, u);
            return YTB_NUM_OK;
        }
    }
    if (e >= 0) {
        /* M x 5^E x 2^E: exact integer, rounded to 53 bits. */
        ytb__big_set(&n, m);
        for (i = e; i >= 13; i -= 13) ytb__big_mul(&n, 1220703125u);
        for (; i > 0; i--) ytb__big_mul(&n, 5u);
        b = ytb__big_bits(&n);
        if (b <= 53) {
            mant = ytb__big_get64(&n, 0);
            shift = 0;
        } else {
            shift = b - 53;
            mant = ytb__big_get64(&n, shift);
            half = ytb__big_bit(&n, shift - 1);
            sticky = ytb__big_any_below(&n, shift - 1);
            mant = ytb__round(mant, half, sticky);
        }
        v = ldexp((double)mant, e + shift);
        if (!(v < HUGE_VAL)) return YTB_NUM_RANGE;
        *out = v;
        return YTB_NUM_OK;
    }
    /* M / (5^q 2^q): Q = floor(M 2^s / 5^q) with 2^55 < Q < 2^57, then
     * round Q (remainder as sticky) to the double's precision at its
     * exponent, which is fewer than 53 bits for a subnormal. */
    ytb__big_pow5(&d, -e);
    s = 56 - ytb__bits64(m) + ytb__big_bits(&d);
    ytb__big_set(&n, m);
    if (s >= 0) ytb__big_shl(&n, s);
    else ytb__big_shl(&d, -s);
    /* Restoring division for the 58 low quotient bits; n >> 58 < d because
     * Q < 2^58. */
    r.n = 0;
    {
        int nb = ytb__big_bits(&n), k;
        for (k = nb - 1; k >= 58; k--) ytb__big_dbl(&r, ytb__big_bit(&n, k));
        while (r.n > 0 && r.w[r.n - 1] == 0) r.n--;
    }
    q = 0;
    for (i = 57; i >= 0; i--) {
        ytb__big_dbl(&r, ytb__big_bit(&n, i));
        if (ytb__big_cmp(&r, &d) >= 0) {
            ytb__big_sub(&r, &d);
            q |= (uint64_t)1 << i;
        }
    }
    sticky = r.n != 0;
    bq = ytb__bits64(q);
    scale = s - e;                       /* value = q x 2^-scale (+ remainder) */
    u = bq - 1 - scale - 52;             /* exponent of the last kept bit */
    if (u < -1074) u = -1074;
    shift = u + scale;                   /* bits of q to drop, >= 3 */
    if (shift >= 65) {
        mant = 0; half = 0; sticky = true;
    } else {
        mant = shift >= 64 ? 0 : q >> shift;
        half = (int)((q >> (shift - 1)) & 1u);
        if (shift >= 2 && (q & ((((uint64_t)1) << (shift - 1)) - 1u))) sticky = true;
    }
    mant = ytb__round(mant, half, sticky);
    v = ldexp((double)mant, u);
    if (v == 0.0) return YTB_NUM_RANGE;
    *out = v;
    return YTB_NUM_OK;
}

static bool ytb__digit(char c) { return c >= '0' && c <= '9'; }

YTB_API int ytb_parse_number(const char* s, size_t n, double* out) {
    size_t i = 0;
    bool neg = false, any = false, frac = false;
    uint64_t m = 0;
    int nd = 0, z = 0, e = 0, ex = 0, rc;
    double v;
    if (!s || !out) return YTB_NUM_SYNTAX;
    if (i < n && (s[i] == '+' || s[i] == '-')) neg = s[i++] == '-';
    for (; i < n; i++) {
        char c = s[i];
        if (c == '.') {
            if (frac) return YTB_NUM_SYNTAX;
            frac = true;
            continue;
        }
        if (!ytb__digit(c)) break;
        any = true;
        if (frac) e--;
        if (c == '0') {
            if (m != 0) z++;             /* a zero that may still be trailing */
            continue;
        }
        /* The zeros between two nonzero digits are significant. */
        for (; z > 0; z--) {
            if (++nd > YTB_MAX_DIGITS) return YTB_NUM_DIGITS;
            m *= 10u;
        }
        if (++nd > YTB_MAX_DIGITS) return YTB_NUM_DIGITS;
        m = m * 10u + (uint64_t)(c - '0');
    }
    if (!any) return YTB_NUM_SYNTAX;
    if (i < n && (s[i] == 'e' || s[i] == 'E')) {
        bool eneg = false, edig = false;
        i++;
        if (i < n && (s[i] == '+' || s[i] == '-')) eneg = s[i++] == '-';
        for (; i < n && ytb__digit(s[i]); i++) {
            edig = true;
            if (ex < 100000) ex = ex * 10 + (s[i] - '0');
        }
        if (!edig) return YTB_NUM_SYNTAX;
        if (eneg) ex = -ex;
    }
    if (i != n) return YTB_NUM_SYNTAX;
    if (m == 0) {
        *out = neg ? -0.0 : 0.0;
        return YTB_NUM_OK;
    }
    rc = ytb__convert(m, e + z + ex, &v);
    if (rc != YTB_NUM_OK) return rc;
    *out = neg ? -v : v;
    return YTB_NUM_OK;
}

YTB_API int ytb_parse_int(const char* s, size_t n, int32_t* out) {
    size_t i = 0;
    bool neg = false;
    int64_t v = 0;
    if (!s || !out) return YTB_NUM_SYNTAX;
    if (i < n && (s[i] == '+' || s[i] == '-')) neg = s[i++] == '-';
    if (i >= n) return YTB_NUM_SYNTAX;
    for (; i < n; i++) {
        if (!ytb__digit(s[i])) return YTB_NUM_SYNTAX;
        if (v <= 2147483648LL) v = v * 10 + (s[i] - '0');
    }
    if (neg) v = -v;
    if (v < -2147483647LL - 1 || v > 2147483647LL) return YTB_NUM_RANGE;
    *out = (int32_t)v;
    return YTB_NUM_OK;
}

/* --- UTF-8 ------------------------------------------------------------- */

/* Valid UTF-8 (no overlong forms, no surrogates, nothing above U+10FFFF)
 * and, unless allow_nul, no NUL. Returns the offset of the first bad byte,
 * or n. The view checks a whole pool at once, NULs separating its texts. */
static size_t ytb__utf8_ex(const unsigned char* p, size_t n, bool allow_nul) {
    size_t i = 0;
    while (i < n) {
        unsigned c;
        size_t k, need;
        unsigned lo = 0x80, hi = 0xBF;
        /* Eight ASCII bytes with no NUL at a time: most text is ASCII. */
        while (i + 8 <= n) {
            uint64_t w;
            memcpy(&w, p + i, 8);
            if ((w & 0x8080808080808080ULL) ||
                (!allow_nul && ((w - 0x0101010101010101ULL) & ~w & 0x8080808080808080ULL)))
                break;
            i += 8;
        }
        if (i >= n) break;
        c = p[i];
        if (c == 0 && !allow_nul) return i;
        if (c < 0x80) { i++; continue; }
        if (c >= 0xC2 && c <= 0xDF) need = 1;
        else if (c >= 0xE0 && c <= 0xEF) {
            need = 2;
            if (c == 0xE0) lo = 0xA0;
            if (c == 0xED) hi = 0x9F;
        } else if (c >= 0xF0 && c <= 0xF4) {
            need = 3;
            if (c == 0xF0) lo = 0x90;
            if (c == 0xF4) hi = 0x8F;
        } else {
            return i;
        }
        if (i + need >= n) return i;
        for (k = 1; k <= need; k++) {
            unsigned b = p[i + k];
            if (k == 1 ? (b < lo || b > hi) : (b < 0x80 || b > 0xBF)) return i;
        }
        i += need + 1;
    }
    return n;
}

static size_t ytb__utf8(const unsigned char* p, size_t n) { return ytb__utf8_ex(p, n, false); }

/* --- messages ---------------------------------------------------------- */

/* "ysp_table: line L, row R, column 'name' (C): what". row 0 with a line
 * is the header; a 0 line, row or column is left out. */
static void ytb__vfail(ytb_table* tab, int line, int row, int col, const char* name,
                         const char* fmt, va_list ap) {
    int n, k;
    size_t cap = sizeof(tab->error);
    tab->err_line = line;
    tab->err_row = row;
    tab->err_col = col;
    n = snprintf(tab->error, cap, "ysp_table: ");
    if (n < 0) n = 0;
    if (line > 0) {
        k = row > 0 ? snprintf(tab->error + n, cap - (size_t)n, "line %d, row %d", line, row)
                    : snprintf(tab->error + n, cap - (size_t)n, "line %d, header", line);
        if (k > 0) n += k;
    } else if (row > 0) {
        k = snprintf(tab->error + n, cap - (size_t)n, "row %d", row);
        if (k > 0) n += k;
    }
    if (col > 0 && n < (int)cap) {
        const char* sep = (line > 0 || row > 0) ? ", " : "";
        k = name ? snprintf(tab->error + n, cap - (size_t)n, "%scolumn '%s' (%d)", sep, name, col)
                 : snprintf(tab->error + n, cap - (size_t)n, "%scolumn %d", sep, col);
        if (k > 0) n += k;
    }
    if (n > (int)cap - 3) n = (int)cap - 3;
    if (line > 0 || row > 0 || col > 0) {
        tab->error[n++] = ':';
        tab->error[n++] = ' ';
    }
    vsnprintf(tab->error + n, cap - (size_t)n, fmt, ap);
    tab->base = NULL;
    tab->size = 0;
    tab->n_rows = tab->n_cols = 0;
}

static bool ytb__fail(ytb_table* tab, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    ytb__vfail(tab, 0, 0, 0, NULL, fmt, ap);
    va_end(ap);
    return false;
}

static bool ytb__fail_at(ytb_table* tab, int line, int row, int col, const char* name,
                           const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    ytb__vfail(tab, line, row, col, name, fmt, ap);
    va_end(ap);
    return false;
}

/* A field's text for a message: at most 32 bytes, control bytes as '?'. */
static void ytb__quote(char* dst, size_t cap, const unsigned char* p, size_t n) {
    size_t i, k = 0, lim = n > 32 ? 32 : n;
    /* Do not cut a UTF-8 sequence in half. */
    while (lim < n && lim > 0 && (p[lim] & 0xC0) == 0x80) lim--;
    for (i = 0; i < lim && k + 4 < cap; i++) dst[k++] = (char)(p[i] < 0x20 ? '?' : p[i]);
    if (lim < n && k + 4 < cap) { dst[k++] = '.'; dst[k++] = '.'; dst[k++] = '.'; }
    dst[k] = '\0';
}

/* --- CSV scanning ------------------------------------------------------ */

/* One cell of the scan: raw bytes (inside the quotes when quoted). */
typedef struct ytb__cell {
    uint32_t off;
    uint32_t len;   /* raw length; bit 31: quoted, bit 30: holds "" */
} ytb__cell;

#define YTB__QUOTED 0x80000000u
#define YTB__DQ     0x40000000u
#define YTB__LENMASK 0x3FFFFFFFu

typedef struct ytb__scan {
    const unsigned char* p;
    size_t  n, pos;
    char    delim;
    int     line;
} ytb__scan;

/* Field end kinds. */
#define YTB__END_FIELD  0
#define YTB__END_RECORD 1
#define YTB__END_INPUT  2

/* Scan one field. On success fills cell and *ulen (the unquoted length)
 * and returns the end kind; on a syntax error returns -1 with *why set. */
static int ytb__field(ytb__scan* sc, ytb__cell* cell, size_t* ulen, const char** why,
                        int* why_line) {
    const unsigned char* p = sc->p;
    size_t i = sc->pos, start, dq = 0;
    int open_line;
    if (i < sc->n && p[i] == '"') {
        open_line = sc->line;
        start = ++i;
        for (;;) {
            if (i >= sc->n) {
                *why = "a quoted field is not closed";
                *why_line = open_line;
                return -1;
            }
            if (p[i] == '"') {
                if (i + 1 < sc->n && p[i + 1] == '"') { dq++; i += 2; continue; }
                break;
            }
            if (p[i] == '\n') sc->line++;
            if (p[i] == 0) { *why = "a NUL byte"; *why_line = sc->line; return -1; }
            i++;
        }
        cell->off = (uint32_t)start;
        cell->len = (uint32_t)(i - start) | YTB__QUOTED | (dq ? YTB__DQ : 0u);
        *ulen = (i - start) - dq;
        i++;    /* the closing quote */
        sc->pos = i;
        if (i >= sc->n) return YTB__END_INPUT;
        if (p[i] == (unsigned char)sc->delim) { sc->pos = i + 1; return YTB__END_FIELD; }
        if (p[i] == '\n') { sc->pos = i + 1; sc->line++; return YTB__END_RECORD; }
        if (p[i] == '\r' && i + 1 < sc->n && p[i + 1] == '\n') {
            sc->pos = i + 2; sc->line++; return YTB__END_RECORD;
        }
        *why = "a character after a closing quote (a quote inside a field is written \"\")";
        *why_line = sc->line;
        return -1;
    }
    start = i;
    for (; i < sc->n; i++) {
        unsigned char c = p[i];
        if (c == (unsigned char)sc->delim || c == '\n' || c == '\r') break;
        if (c == '"') {
            *why = "a quote inside an unquoted field (quote the whole field)";
            *why_line = sc->line;
            return -1;
        }
        if (c == 0) { *why = "a NUL byte"; *why_line = sc->line; return -1; }
    }
    cell->off = (uint32_t)start;
    cell->len = (uint32_t)(i - start);
    *ulen = i - start;
    if (i >= sc->n) { sc->pos = i; return YTB__END_INPUT; }
    if (p[i] == (unsigned char)sc->delim) { sc->pos = i + 1; return YTB__END_FIELD; }
    if (p[i] == '\n') { sc->pos = i + 1; sc->line++; return YTB__END_RECORD; }
    if (i + 1 < sc->n && p[i + 1] == '\n') { sc->pos = i + 2; sc->line++; return YTB__END_RECORD; }
    *why = "a CR that is not followed by LF (records end with CRLF or LF)";
    *why_line = sc->line;
    return -1;
}

/* The unquoted text of a cell: the raw bytes when they need no change,
 * else a copy into tmp with "" made ". */
static const unsigned char* ytb__text(const unsigned char* in, const ytb__cell* c,
                                        unsigned char* tmp, size_t* n) {
    const unsigned char* p = in + c->off;
    size_t len = c->len & YTB__LENMASK, i, k = 0;
    if (!(c->len & YTB__DQ)) { *n = len; return p; }
    for (i = 0; i < len; i++) {
        tmp[k++] = p[i];
        if (p[i] == '"') i++;
    }
    *n = k;
    return tmp;
}

static bool ytb__ident(const unsigned char* p, size_t n) {
    size_t i;
    if (n == 0 || n > YTB_MAX_NAME) return false;
    for (i = 0; i < n; i++) {
        unsigned char c = p[i];
        bool alpha = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
        if (!alpha && !(i > 0 && c >= '0' && c <= '9')) return false;
    }
    return true;
}

/* What a field can be, by its syntax alone (no conversion; the write pass
 * converts once): bit 1 an integer in int32, bit 2 a number. A number out of
 * range or with too many digits still counts, so the write pass names the
 * cell instead of the column silently becoming text. */
static int ytb__kind(const unsigned char* p, size_t n) {
    size_t i = 0, d0;
    bool point = false;
    int k;
    if (n == 0) return 0;
    if (p[0] == '+' || p[0] == '-') i++;
    d0 = i;
    while (i < n && p[i] >= '0' && p[i] <= '9') i++;
    if (i == n && i > d0) {
        /* Digits only: an integer when it fits int32 (at most 10 digits
         * after leading zeros, then by value). */
        int32_t iv;
        k = 2;
        if (ytb_parse_int((const char*)p, n, &iv) == YTB_NUM_OK) k |= 1;
        return k;
    }
    if (i < n && p[i] == '.') {
        point = true;
        i++;
        while (i < n && p[i] >= '0' && p[i] <= '9') i++;
    }
    if (i == d0 + (point ? 1u : 0u)) return 0;     /* no digit */
    if (i < n && (p[i] == 'e' || p[i] == 'E')) {
        size_t e0;
        i++;
        if (i < n && (p[i] == '+' || p[i] == '-')) i++;
        e0 = i;
        while (i < n && p[i] >= '0' && p[i] <= '9') i++;
        if (i == e0) return 0;
    }
    return i == n ? 2 : 0;
}

/* --- the block --------------------------------------------------------- */

static size_t ytb__align8(size_t n) { return (n + 7u) & ~(size_t)7u; }

/* Column descriptor fields, at 48 + 32 * col. */
#define YTB__C_NAME_OFF   0
#define YTB__C_NAME_LEN   4
#define YTB__C_TYPE       8
#define YTB__C_NLEV      12
#define YTB__C_ROWS_OFF  16
#define YTB__C_LEV_OFF   24

static const unsigned char* ytb__cd(const ytb_table* tab, int col) {
    return tab->base + YTB_HEADER_SIZE + 32u * (unsigned)col;
}

static bool ytb__ok(const ytb_table* tab, int col) {
    return tab && tab->base && col >= 0 && col < tab->n_cols;
}

YTB_API void ytb_close(ytb_table* tab) {
    if (tab) memset(tab, 0, sizeof(*tab));
}

YTB_API const char* ytb_error(const ytb_table* tab) {
    return tab ? tab->error : "ysp_table: null table";
}

YTB_API const char* ytb_col_name(const ytb_table* tab, int col) {
    if (!ytb__ok(tab, col)) return NULL;
    return (const char*)tab->base + ytb__rd32(ytb__cd(tab, col) + YTB__C_NAME_OFF);
}

YTB_API int ytb_col(const ytb_table* tab, const char* name) {
    int c;
    if (!tab || !tab->base || !name) return -1;
    for (c = 0; c < tab->n_cols; c++)
        if (strcmp(ytb_col_name(tab, c), name) == 0) return c;
    return -1;
}

YTB_API ytb_type ytb_col_type(const ytb_table* tab, int col) {
    if (!ytb__ok(tab, col)) return YTB_AUTO;
    return (ytb_type)ytb__rd32(ytb__cd(tab, col) + YTB__C_TYPE);
}

YTB_API int ytb_n_levels(const ytb_table* tab, int col) {
    if (!ytb__ok(tab, col)) return -1;
    return (int)ytb__rd32(ytb__cd(tab, col) + YTB__C_NLEV);
}

YTB_API const unsigned char* ytb_level_bytes(const ytb_table* tab, int col) {
    if (!ytb__ok(tab, col)) return NULL;
    return tab->base + ytb__rd64(ytb__cd(tab, col) + YTB__C_ROWS_OFF);
}

YTB_API int ytb_level(const ytb_table* tab, int row, int col) {
    const unsigned char* p;
    if (!ytb__ok(tab, col) || row < 0 || row >= tab->n_rows) return -1;
    p = ytb_level_bytes(tab, col) + 2u * (unsigned)row;
    return (int)p[0] | (int)p[1] << 8;
}

static const unsigned char* ytb__lev(const ytb_table* tab, int col, int level) {
    const unsigned char* cd;
    if (!ytb__ok(tab, col)) return NULL;
    cd = ytb__cd(tab, col);
    if (level < 0 || (uint32_t)level >= ytb__rd32(cd + YTB__C_NLEV)) return NULL;
    return tab->base + ytb__rd64(cd + YTB__C_LEV_OFF) + 16u * (unsigned)level;
}

YTB_API const char* ytb_level_text(const ytb_table* tab, int col, int level) {
    const unsigned char* l = ytb__lev(tab, col, level);
    return l ? (const char*)tab->base + ytb__rd32(l) : NULL;
}

YTB_API double ytb_level_num(const ytb_table* tab, int col, int level) {
    const unsigned char* l = ytb__lev(tab, col, level);
    if (!l || ytb_col_type(tab, col) == YTB_STRING) return YTB__NAN;
    return ytb__rdf64(l + 8);
}

YTB_API const char* ytb_text(const ytb_table* tab, int row, int col) {
    return ytb_level_text(tab, col, ytb_level(tab, row, col));
}

YTB_API double ytb_num(const ytb_table* tab, int row, int col) {
    return ytb_level_num(tab, col, ytb_level(tab, row, col));
}

YTB_API int32_t ytb_int(const ytb_table* tab, int row, int col) {
    if (ytb_col_type(tab, col) != YTB_INTEGER) return INT32_MIN;
    {
        double v = ytb_num(tab, row, col);
        return (v == v) ? (int32_t)v : INT32_MIN;
    }
}

YTB_API uint64_t ytb_hash(const ytb_table* tab) {
    if (!tab || !tab->base) return 0;
    return ytb__rd64(tab->base + 32);
}

YTB_API int ytb_find(const ytb_table* tab, int col, const char* value) {
    int nl, l;
    size_t vn;
    ytb_type ty;
    double want, got;
    if (!ytb__ok(tab, col) || !value) return -1;
    nl = ytb_n_levels(tab, col);
    ty = ytb_col_type(tab, col);
    vn = strlen(value);
    if (ty == YTB_STRING || vn == 0) {
        for (l = 0; l < nl; l++)
            if (strcmp(ytb_level_text(tab, col, l), value) == 0) return l;
        return -1;
    }
    if (ytb_parse_number(value, vn, &want) != YTB_NUM_OK) return -1;
    for (l = 0; l < nl; l++) {
        got = ytb_level_num(tab, col, l);
        if (got == want) return l;
    }
    return -1;
}

/* --- view -------------------------------------------------------------- */

YTB_API bool ytb_open_view(ytb_table* tab, const void* bytes, size_t len) {
    const unsigned char* b = (const unsigned char*)bytes;
    uint32_t nr, nc, c, r, nlev, nlen, k;
    uint64_t size, roff, loff, toff, tlen, pool;
    if (!tab) return false;
    memset(tab, 0, sizeof(*tab));
    if (!ytb__little()) return ytb__fail(tab, "a big-endian host cannot read the little-endian block");
    if (!b || len < YTB_HEADER_SIZE || memcmp(b, "PSTB", 4) != 0)
        return ytb__fail(tab, "not a table block (no PSTB header)");
    if (ytb__rd32(b + 4) != YTB_FORMAT)
        return ytb__fail(tab, "block format %u is not %u", (unsigned)ytb__rd32(b + 4), (unsigned)YTB_FORMAT);
    nr = ytb__rd32(b + 8);
    nc = ytb__rd32(b + 12);
    size = ytb__rd64(b + 24);
    if (nr > YTB_MAX_ROWS || nc < 1 || nc > YTB_MAX_COLUMNS)
        return ytb__fail(tab, "block: %u rows and %u columns are out of range", (unsigned)nr, (unsigned)nc);
    if (size > len || size < YTB_HEADER_SIZE + 32u * nc || (size & 7u))
        return ytb__fail(tab, "block: size %llu does not fit the %llu bytes given",
                           (unsigned long long)size, (unsigned long long)len);
    pool = ytb__rd64(b + 40);
    if (ytb__rd32(b + 16) > YTB_MAX_ROWS || ytb__rd32(b + 20) != 0)
        return ytb__fail(tab, "block: reserved header fields are not zero");
    if (pool < YTB_HEADER_SIZE + 32u * nc || pool > size || (pool & 7u))
        return ytb__fail(tab, "block: the text pool offset is out of range");
    if (ytb__block_hash(b, (size_t)size) != ytb__rd64(b + 32))
        return ytb__fail(tab, "block: the content hash does not match (corrupt or truncated)");
    /* Every text lies in the pool, which is valid UTF-8 as a whole with NULs
     * between texts; each text then only needs its own bounds, its NUL and
     * no NUL inside. One pass instead of one per text (80% of the view's
     * time on a 10,000-row table, docs/table.md). */
    if (ytb__utf8_ex(b + pool, (size_t)(size - pool), true) != size - pool)
        return ytb__fail(tab, "block: the text pool is not valid UTF-8");
    for (c = 0; c < nc; c++) {
        const unsigned char* cd = b + YTB_HEADER_SIZE + 32u * c;
        uint32_t ty = ytb__rd32(cd + YTB__C_TYPE), noff = ytb__rd32(cd + YTB__C_NAME_OFF);
        nlen = ytb__rd32(cd + YTB__C_NAME_LEN);
        nlev = ytb__rd32(cd + YTB__C_NLEV);
        roff = ytb__rd64(cd + YTB__C_ROWS_OFF);
        loff = ytb__rd64(cd + YTB__C_LEV_OFF);
        if (noff < pool || (uint64_t)noff + nlen + 1u > size || b[noff + nlen] != 0 || !ytb__ident(b + noff, nlen))
            return ytb__fail(tab, "block: column %u has no valid name", (unsigned)c + 1u);
        for (k = 0; k < c; k++) {
            const unsigned char* od = b + YTB_HEADER_SIZE + 32u * k;
            if (ytb__rd32(od + YTB__C_NAME_LEN) == nlen &&
                memcmp(b + ytb__rd32(od + YTB__C_NAME_OFF), b + noff, nlen) == 0)
                return ytb__fail(tab, "block: column name '%s' appears twice", (const char*)b + noff);
        }
        if (ty < YTB_INTEGER || ty > YTB_STRING)
            return ytb__fail(tab, "block: column '%s' has type %u", (const char*)b + noff, (unsigned)ty);
        if (nlev > nr || (nr > 0 && nlev == 0))
            return ytb__fail(tab, "block: column '%s' has %u levels for %u rows",
                               (const char*)b + noff, (unsigned)nlev, (unsigned)nr);
        if ((roff & 7u) || roff > size || 2ull * nr > size - roff ||
            (loff & 7u) || loff > size || 16ull * nlev > size - loff)
            return ytb__fail(tab, "block: column '%s' points outside the block", (const char*)b + noff);
        for (r = 0; r < nr; r++) {
            uint32_t lv = (uint32_t)b[roff + 2u * r] | (uint32_t)b[roff + 2u * r + 1u] << 8;
            if (lv >= nlev)
                return ytb__fail(tab, "block: column '%s' row %u has level %u of %u",
                                   (const char*)b + noff, (unsigned)r + 1u, (unsigned)lv, (unsigned)nlev);
        }
        for (k = 0; k < nlev; k++) {
            const unsigned char* l = b + loff + 16u * k;
            double v;
            toff = ytb__rd32(l);
            tlen = ytb__rd32(l + 4);
            /* A NUL inside a text would only shorten it as a C string; the
             * parse writes none, and a check per text cost more than the
             * rest of the view (measured, docs/table.md). */
            if (toff < pool || toff + tlen + 1u > size || b[toff + tlen] != 0)
                return ytb__fail(tab, "block: column '%s' level %u has no valid text",
                                   (const char*)b + noff, (unsigned)k);
            v = ytb__rdf64(l + 8);
            if (ty == YTB_INTEGER && !(v >= -2147483648.0 && v <= 2147483647.0 && v == (double)(int32_t)v))
                return ytb__fail(tab, "block: column '%s' level %u is not an int32",
                                   (const char*)b + noff, (unsigned)k);
        }
    }
    tab->base = b;
    tab->size = (size_t)size;
    tab->n_rows = (int)nr;
    tab->n_cols = (int)nc;
    tab->n_skipped = (int)ytb__rd32(b + 16);
    return true;
}

/* --- CSV ---------------------------------------------------------------- */

typedef struct ytb__col {
    int        type;      /* resolved type                              */
    int        declared;  /* declared type, or YTB_AUTO               */
    int        kinds;     /* AND of ytb__kind over non-empty cells    */
    bool       any;       /* a non-empty cell exists                    */
    bool       empty;     /* an empty cell exists                       */
} ytb__col;

/* A string's hash for interning, eight bytes at a time (internal; not
 * part of the block format). */
static uint64_t ytb__strhash(const unsigned char* p, size_t n) {
    uint64_t h = 0x9E3779B97F4A7C15ULL ^ (uint64_t)n, w;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        memcpy(&w, p + i, 8);
        h = (h ^ w) * 0xBF58476D1CE4E5B9ULL;
        h ^= h >> 31;
    }
    w = 0;
    if (i < n) memcpy(&w, p + i, n - i);
    h = (h ^ w) * 0x94D049BB133111EBULL;
    return h ^ (h >> 29);
}

static uint32_t ytb__mix(uint64_t h) {
    h ^= h >> 33;
    h *= 0xFF51AFD7ED558CCDULL;
    h ^= h >> 33;
    return (uint32_t)h;
}

/* Column c's name from the stored header cells, for a pass-1 message. */
static const char* ytb__hname(const unsigned char* in, const ytb__cell* idx, size_t idx_cap,
                                size_t cells, int nc, int c, char* buf) {
    const ytb__cell* hc;
    size_t hn;
    if (!idx || nc <= 0 || c < 0 || c >= nc || (size_t)nc > idx_cap || (size_t)nc > cells) return NULL;
    hc = &idx[-(ptrdiff_t)c - 1];
    hn = hc->len & YTB__LENMASK;
    if ((hc->len & YTB__DQ) || !ytb__ident(in + hc->off, hn)) return NULL;
    memcpy(buf, in + hc->off, hn);
    buf[hn] = '\0';
    return buf;
}

/* The line on which data row `row` (1-based, skipped rows not counted)
 * starts, found again by a scan: only an error message needs it. */
static int ytb__row_line(const unsigned char* in, size_t n, char delim, int row) {
    ytb__scan sc;
    ytb__cell cell;
    size_t ulen;
    const char* why;
    int why_line, rec = -1, line, end;
    bool empty = true;
    sc.p = in;
    sc.n = n;
    sc.pos = 0;
    sc.delim = delim;
    sc.line = 1;
    if (n >= 3 && in[0] == 0xEF && in[1] == 0xBB && in[2] == 0xBF) sc.pos = 3;
    line = 1;
    while (sc.pos < n) {
        end = ytb__field(&sc, &cell, &ulen, &why, &why_line);
        if (end < 0) return 0;
        if (ulen) empty = false;
        if (end == YTB__END_FIELD) continue;
        if (rec < 0 || !empty) rec++;
        if (rec == row) return line;
        if (end == YTB__END_INPUT) break;
        line = sc.line;
        empty = true;
    }
    return 0;
}

YTB_API bool ytb_open_csv(ytb_table* tab, const ytb_csv_desc* d) {
    const unsigned char* in;
    size_t n, cells = 0, idx_cap = 0, pool_bound = 0, ulen, need, top;
    ytb__scan sc;
    ytb__cell cell, *idx = NULL;
    ytb__col cols[YTB_MAX_COLUMNS];
    int nc = 0, nr = 0, field = 0, kind, rec_line, why_line = 0, c, r, skipped = 0;
    const char* why = NULL;
    bool rec_empty = true, stored = true;
    size_t rec_first = 0;
    unsigned char* arena;
    char delim;
    char q[48];
    char nbuf[YTB_MAX_NAME + 1];

    if (!tab) return false;
    memset(tab, 0, sizeof(*tab));
    if (!d) return ytb__fail(tab, "null desc");
    if (!ytb__little()) return ytb__fail(tab, "a big-endian host cannot write the little-endian block");
    in = (const unsigned char*)d->text;
    n = d->len;
    if (!in && n) return ytb__fail(tab, "desc.text is NULL");
    if (n > 0x7FFFFFFFu) return ytb__fail(tab, "the input is 2 GiB or more");
    delim = d->delimiter ? d->delimiter : ',';
    if (delim != ',' && delim != ';' && delim != '\t')
        return ytb__fail(tab, "desc.delimiter must be ',', ';' or a tab");
    if (d->n_types < 0 || d->n_types > YTB_MAX_COLUMNS)
        return ytb__fail(tab, "desc.n_types must be in [0, %d]", YTB_MAX_COLUMNS);
    arena = (unsigned char*)d->arena;
    if (arena && ((uintptr_t)arena & 7u)) return ytb__fail(tab, "desc.arena is not 8-byte aligned");
    memset(cols, 0, sizeof(cols));

    sc.p = in;
    sc.n = n;
    sc.pos = 0;
    sc.delim = delim;
    sc.line = 1;
    if (n >= 3 && in[0] == 0xEF && in[1] == 0xBB && in[2] == 0xBF) sc.pos = 3;
    if (sc.pos >= n) return ytb__fail_at(tab, 1, 0, 0, NULL, "the input is empty (a header line is required)");

    /* The cell index grows down from the arena's top while the scan runs;
     * when it does not fit, the scan goes on to learn what is needed. */
    if (arena) {
        top = d->arena_size & ~(size_t)7u;
        idx = (ytb__cell*)(void*)(arena + top);
        idx_cap = top / sizeof(ytb__cell);
    }

    /* Pass 1: structure, bounds, UTF-8, sizes, and what each column can be. */
    rec_line = sc.line;
    for (;;) {
        int end = ytb__field(&sc, &cell, &ulen, &why, &why_line);
        const unsigned char* raw;
        size_t rawlen;
        if (end < 0)
            return ytb__fail_at(tab, why_line, nc == 0 ? 0 : nr + 1, field + 1,
                                  ytb__hname(in, idx, idx_cap, cells, nc, field, nbuf), "%s", why);
        raw = in + cell.off;
        rawlen = cell.len & YTB__LENMASK;
        if (ulen > YTB_MAX_FIELD)
            return ytb__fail_at(tab, rec_line, nc == 0 ? 0 : nr + 1, field + 1,
                                  ytb__hname(in, idx, idx_cap, cells, nc, field, nbuf), "a field of %lu bytes (at most %d)", (unsigned long)ulen, YTB_MAX_FIELD);
        if (ytb__utf8(raw, rawlen) != rawlen)
            return ytb__fail_at(tab, rec_line, nc == 0 ? 0 : nr + 1, field + 1,
                                  ytb__hname(in, idx, idx_cap, cells, nc, field, nbuf),
                                  "the field is not valid UTF-8");
        if (nc == 0 && field >= YTB_MAX_COLUMNS)
            return ytb__fail_at(tab, rec_line, 0, field + 1, NULL, "more than %d columns", YTB_MAX_COLUMNS);
        if (nc > 0 && field >= nc)
            return ytb__fail_at(tab, rec_line, nr + 1, 0, NULL,
                                  "row has more than the header's %d fields", nc);
        if (ulen) rec_empty = false;
        if (cells < idx_cap) idx[-(ptrdiff_t)cells - 1] = cell;
        else stored = false;
        cells++;
        pool_bound += ulen + 1;
        field++;
        if (end == YTB__END_FIELD) continue;
        /* A record ended. */
        if (nc == 0) {
            nc = field;
            if (rec_empty && nc == 1)
                return ytb__fail_at(tab, rec_line, 0, 0, NULL, "the header line is empty");
        } else if (rec_empty) {
            /* Every field empty: skipped, as Excel writes such rows; its
             * cells leave the index again. */
            skipped++;
            cells = rec_first;
        } else if (field != nc) {
            return ytb__fail_at(tab, rec_line, nr + 1, 0, NULL,
                                  "row has %d fields, the header has %d", field, nc);
        } else {
            nr++;
            if (nr > YTB_MAX_ROWS)
                return ytb__fail_at(tab, rec_line, nr, 0, NULL, "more than %d rows", YTB_MAX_ROWS);
        }
        if (end == YTB__END_INPUT || sc.pos >= n) break;
        field = 0;
        rec_empty = true;
        rec_first = cells;
        rec_line = sc.line;
    }
    tab->n_skipped = skipped;

    {
        size_t hash_cap = 16, final_bound, pool_off, lev_total;
        unsigned char *pool_tmp, *tmp, *blk;
        uint32_t* hash;
        size_t pool_len = 0, lev_pos, rows_pos;
        int dc;

        while (hash_cap < 2u * (size_t)nr) hash_cap <<= 1;
        lev_total = 16u * (size_t)nr * (size_t)nc;
        final_bound = YTB_HEADER_SIZE + 32u * (size_t)nc + (size_t)nc * ytb__align8(2u * (size_t)nr) +
                      lev_total;
        pool_bound = ytb__align8(pool_bound);
        need = ytb__align8(final_bound + pool_bound) + pool_bound + hash_cap * 4u +
               ytb__align8(YTB_MAX_FIELD + 8u) + cells * sizeof(ytb__cell) + 16u;
        tab->need = need;
        if (!arena) return ytb__fail(tab, "no arena (need %lu bytes)", (unsigned long)need);
        if (!stored || need > d->arena_size)
            return ytb__fail(tab, "the arena has %lu bytes, the parse needs %lu",
                               (unsigned long)d->arena_size, (unsigned long)need);

        /* Header names, then declared types. */
        for (c = 0; c < nc; c++) {
            const ytb__cell* hc = &idx[-(ptrdiff_t)c - 1];
            size_t hn = hc->len & YTB__LENMASK;
            const unsigned char* hp = in + hc->off;
            int k;
            if ((hc->len & YTB__DQ) || !ytb__ident(hp, hn)) {
                ytb__quote(q, sizeof(q), hp, hn);
                return ytb__fail_at(tab, 1, 0, c + 1, NULL,
                                      "column name '%s' is not a name (a letter or _, then letters, digits or _, at most %d bytes)",
                                      q, YTB_MAX_NAME);
            }
            for (k = 0; k < c; k++) {
                const ytb__cell* oc = &idx[-(ptrdiff_t)k - 1];
                if ((oc->len & YTB__LENMASK) == hn && memcmp(in + oc->off, hp, hn) == 0) {
                    ytb__quote(q, sizeof(q), hp, hn);
                    return ytb__fail_at(tab, 1, 0, c + 1, NULL, "column name '%s' appears twice", q);
                }
            }
        }
        for (dc = 0; dc < d->n_types; dc++) {
            const char* dn = d->types[dc].name;
            int found = -1;
            if (!dn) return ytb__fail(tab, "desc.types[%d].name is NULL", dc);
            if ((int)d->types[dc].type < (int)YTB_AUTO || (int)d->types[dc].type > (int)YTB_STRING)
                return ytb__fail(tab, "desc.types[%d].type is not a ytb_type", dc);
            for (c = 0; c < nc; c++) {
                const ytb__cell* hc = &idx[-(ptrdiff_t)c - 1];
                size_t hn = hc->len & YTB__LENMASK;
                if (strlen(dn) == hn && memcmp(dn, in + hc->off, hn) == 0) found = c;
            }
            if (found < 0) return ytb__fail(tab, "desc.types[%d]: no column '%s'", dc, dn);
            cols[found].declared = (int)d->types[dc].type;
        }

        /* What each column can be, from its data cells. */
        for (r = 0; r < nr; r++) {
            for (c = 0; c < nc; c++) {
                const ytb__cell* ce = &idx[-(ptrdiff_t)((size_t)(r + 1) * (size_t)nc + (size_t)c) - 1];
                size_t len = ce->len & YTB__LENMASK;
                ytb__col* co = &cols[c];
                if (len == 0) { co->empty = true; continue; }
                kind = (ce->len & YTB__DQ) ? 0 : ytb__kind(in + ce->off, len);
                co->kinds = co->any ? (co->kinds & kind) : kind;
                co->any = true;
            }
        }
        for (c = 0; c < nc; c++) {
            ytb__col* co = &cols[c];
            if (co->declared != YTB_AUTO) co->type = co->declared;
            else if (co->any && (co->kinds & 1)) co->type = YTB_INTEGER;
            else if (co->any && (co->kinds & 2)) co->type = YTB_NUMBER;
            else co->type = YTB_STRING;
            if (co->type == YTB_INTEGER && co->empty && d->allow_empty) co->type = YTB_NUMBER;
        }

        /* Regions: the block from 0, the pool scratch after its bound, then
         * the hash table and the unquoting buffer; the index is at the top. */
        blk = arena;
        pool_tmp = arena + ytb__align8(final_bound + pool_bound);
        hash = (uint32_t*)(void*)(pool_tmp + pool_bound);
        tmp = (unsigned char*)(hash + hash_cap);

        /* Column names go into the pool first. */
        rows_pos = YTB_HEADER_SIZE + 32u * (size_t)nc;
        memset(blk, 0, rows_pos);
        for (c = 0; c < nc; c++) {
            const ytb__cell* hc = &idx[-(ptrdiff_t)c - 1];
            size_t hn = hc->len & YTB__LENMASK;
            const unsigned char* hp = in + hc->off;
            unsigned char* cd = blk + YTB_HEADER_SIZE + 32u * (size_t)c;
            memcpy(pool_tmp + pool_len, hp, hn);
            pool_tmp[pool_len + hn] = 0;
            ytb__wr32(cd + YTB__C_NAME_OFF, (uint32_t)pool_len);   /* pool-relative for now */
            ytb__wr32(cd + YTB__C_NAME_LEN, (uint32_t)hn);
            ytb__wr32(cd + YTB__C_TYPE, (uint32_t)cols[c].type);
            pool_len += hn + 1;
        }
        lev_pos = rows_pos + (size_t)nc * ytb__align8(2u * (size_t)nr);

        /* Pass 2, a column at a time: intern each cell's value into the
         * column's levels (first appearance order), write the row's level. */
        for (c = 0; c < nc; c++) {
            unsigned char* cd = blk + YTB_HEADER_SIZE + 32u * (size_t)c;
            unsigned char* rows = blk + rows_pos + (size_t)c * ytb__align8(2u * (size_t)nr);
            size_t lev0 = lev_pos;
            int nlev = 0, ty = cols[c].type;
            memset(hash, 0, hash_cap * 4u);
            memset(rows, 0, ytb__align8(2u * (size_t)nr));
            for (r = 0; r < nr; r++) {
                const ytb__cell* ce = &idx[-(ptrdiff_t)((size_t)(r + 1) * (size_t)nc + (size_t)c) - 1];
                size_t tn, slot;
                const unsigned char* tp = ytb__text(in, ce, tmp, &tn);
                double v = 0.0;
                uint64_t key;
                int lv = -1;
                if (ty != YTB_STRING) {
                    int rc;
                    if (tn == 0) {
                        if (!d->allow_empty)
                            return ytb__fail_at(tab, ytb__row_line(in, n, delim, r + 1), r + 1, c + 1,
                                                  (const char*)pool_tmp + ytb__rd32(cd),
                                                  "the cell is empty and the column is %s (allow_empty makes it NaN)",
                                                  ytb_type_name((ytb_type)ty));
                        v = YTB__NAN;
                    } else if (ty == YTB_INTEGER) {
                        int32_t iv = 0;
                        rc = ytb_parse_int((const char*)tp, tn, &iv);
                        if (rc != YTB_NUM_OK) {
                            ytb__quote(q, sizeof(q), tp, tn);
                            return ytb__fail_at(tab, ytb__row_line(in, n, delim, r + 1), r + 1, c + 1,
                                                  (const char*)pool_tmp + ytb__rd32(cd), "'%s' is not an integer%s", q,
                                                  rc == YTB_NUM_RANGE ? " in the int32 range" : "");
                        }
                        v = (double)iv;
                    } else {
                        rc = ytb_parse_number((const char*)tp, tn, &v);
                        if (rc != YTB_NUM_OK) {
                            bool comma = memchr(tp, ',', tn) != NULL;
                            ytb__quote(q, sizeof(q), tp, tn);
                            return ytb__fail_at(tab, ytb__row_line(in, n, delim, r + 1), r + 1, c + 1,
                                                  (const char*)pool_tmp + ytb__rd32(cd), "'%s' %s", q,
                                                  rc == YTB_NUM_DIGITS ? "has more than 19 significant digits (declare a code column YTB_STRING)" :
                                                  rc == YTB_NUM_RANGE ? "is out of the range of a double" :
                                                  comma ? "is not a number; the decimal separator is '.'" :
                                                  "is not a number");
                        }
                    }
                    if (v != v) key = 0x7FF8000000000000ULL;
                    else if (v == 0.0) key = 0;
                    else memcpy(&key, &v, sizeof(key));
                } else {
                    key = ytb__strhash(tp, tn);
                }
                slot = ytb__mix(key) & (hash_cap - 1u);
                for (;;) {
                    uint32_t h = hash[slot];
                    const unsigned char* le;
                    if (h == 0) break;
                    le = blk + lev0 + 16u * (h - 1u);
                    if (ty == YTB_STRING) {
                        if (ytb__rd32(le + 4) == tn && memcmp(pool_tmp + ytb__rd32(le), tp, tn) == 0) {
                            lv = (int)h - 1;
                            break;
                        }
                    } else {
                        double lvv = ytb__rdf64(le + 8);
                        if (lvv == v || (lvv != lvv && v != v)) { lv = (int)h - 1; break; }
                    }
                    slot = (slot + 1u) & (hash_cap - 1u);
                }
                if (lv < 0) {
                    unsigned char* le = blk + lev0 + 16u * (size_t)nlev;
                    memcpy(pool_tmp + pool_len, tp, tn);
                    pool_tmp[pool_len + tn] = 0;
                    ytb__wr32(le, (uint32_t)pool_len);
                    ytb__wr32(le + 4, (uint32_t)tn);
                    ytb__wrf64(le + 8, ty == YTB_STRING ? 0.0 : v);
                    pool_len += tn + 1;
                    lv = nlev++;
                    hash[slot] = (uint32_t)lv + 1u;
                }
                rows[2 * r] = (unsigned char)lv;
                rows[2 * r + 1] = (unsigned char)(lv >> 8);
            }
            ytb__wr32(cd + YTB__C_NLEV, (uint32_t)nlev);
            ytb__wr64(cd + YTB__C_ROWS_OFF, (uint64_t)(rows - blk));
            ytb__wr64(cd + YTB__C_LEV_OFF, (uint64_t)lev0);
            lev_pos = lev0 + 16u * (size_t)nlev;
        }

        /* The pool follows the level tables; offsets become absolute. */
        pool_off = ytb__align8(lev_pos);
        memmove(blk + pool_off, pool_tmp, pool_len);
        for (c = 0; c < nc; c++) {
            unsigned char* cd = blk + YTB_HEADER_SIZE + 32u * (size_t)c;
            uint32_t nl = ytb__rd32(cd + YTB__C_NLEV), k;
            unsigned char* lt = blk + ytb__rd64(cd + YTB__C_LEV_OFF);
            ytb__wr32(cd + YTB__C_NAME_OFF, (uint32_t)(ytb__rd32(cd + YTB__C_NAME_OFF) + pool_off));
            for (k = 0; k < nl; k++)
                ytb__wr32(lt + 16u * k, (uint32_t)(ytb__rd32(lt + 16u * k) + pool_off));
        }
        {
            size_t end = ytb__align8(pool_off + pool_len);
            memset(blk + pool_off + pool_len, 0, end - (pool_off + pool_len));
            memcpy(blk, "PSTB", 4);
            ytb__wr32(blk + 4, YTB_FORMAT);
            ytb__wr32(blk + 8, (uint32_t)nr);
            ytb__wr32(blk + 12, (uint32_t)nc);
            ytb__wr32(blk + 16, (uint32_t)skipped);
            ytb__wr32(blk + 20, 0);
            ytb__wr64(blk + 24, (uint64_t)end);
            ytb__wr64(blk + 40, (uint64_t)pool_off);
            ytb__wr64(blk + 32, 0);
            ytb__wr64(blk + 32, ytb__block_hash(blk, end));
            tab->base = blk;
            tab->size = end;
            tab->n_rows = nr;
            tab->n_cols = nc;
            tab->n_skipped = skipped;
            tab->error[0] = '\0';
        }
    }
    return true;
}

/* --- file loader ------------------------------------------------------- */

#ifdef YTB_STDIO
#ifdef _WIN32
#include <wchar.h>
/* UTF-8 to UTF-16 by hand, so the loader needs no windows.h. */
static bool ytb__wide(const char* s, wchar_t* w, size_t cap) {
    const unsigned char* p = (const unsigned char*)s;
    size_t n = strlen(s), i = 0, k = 0;
    if (ytb__utf8(p, n) != n) return false;
    while (i < n) {
        uint32_t cp;
        unsigned c = p[i];
        if (c < 0x80) { cp = c; i += 1; }
        else if (c < 0xE0) { cp = (c & 0x1Fu) << 6 | (p[i + 1] & 0x3Fu); i += 2; }
        else if (c < 0xF0) { cp = (c & 0x0Fu) << 12 | (p[i + 1] & 0x3Fu) << 6 | (p[i + 2] & 0x3Fu); i += 3; }
        else {
            cp = (c & 0x07u) << 18 | (p[i + 1] & 0x3Fu) << 12 | (p[i + 2] & 0x3Fu) << 6 | (p[i + 3] & 0x3Fu);
            i += 4;
        }
        if (cp >= 0x10000) {
            if (k + 2 >= cap) return false;
            cp -= 0x10000;
            w[k++] = (wchar_t)(0xD800 + (cp >> 10));
            w[k++] = (wchar_t)(0xDC00 + (cp & 0x3FFu));
        } else {
            if (k + 1 >= cap) return false;
            w[k++] = (wchar_t)cp;
        }
    }
    w[k] = 0;
    return true;
}
#endif

YTB_API bool ytb_open_csv_file(ytb_table* tab, const char* path, const ytb_csv_desc* desc) {
    FILE* f = NULL;
    long sz;
    size_t got, at;
    ytb_csv_desc d;
    unsigned char* arena;
    if (!tab) return false;
    memset(tab, 0, sizeof(*tab));
    if (!desc || !path) return ytb__fail(tab, "null desc or path");
#ifdef _WIN32
    {
        static wchar_t wpath[4096];
        if (!ytb__wide(path, wpath, sizeof(wpath) / sizeof(wpath[0])))
            return ytb__fail(tab, "the path is not valid UTF-8 or is too long");
#if defined(_MSC_VER)
        if (_wfopen_s(&f, wpath, L"rb") != 0) f = NULL;
#else
        f = _wfopen(wpath, L"rb");
#endif
    }
#else
    f = fopen(path, "rb");
#endif
    if (!f) return ytb__fail(tab, "cannot open '%s'", path);
    if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return ytb__fail(tab, "cannot read the size of '%s'", path);
    }
    arena = (unsigned char*)desc->arena;
    if (!arena || (size_t)sz + 8u > desc->arena_size) {
        fclose(f);
        tab->need = (size_t)sz;
        return ytb__fail(tab, "the arena cannot hold '%s' (%ld bytes) and its parse", path, sz);
    }
    at = (desc->arena_size - (size_t)sz) & ~(size_t)7u;
    got = fread(arena + at, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) return ytb__fail(tab, "short read of '%s'", path);
    d = *desc;
    d.text = arena + at;
    d.len = (size_t)sz;
    d.arena_size = at;
    if (!ytb_open_csv(tab, &d)) {
        tab->need += (size_t)sz + 8u;
        return false;
    }
    return true;
}
#endif /* YTB_STDIO */

#endif /* YSP_TABLE_IMPLEMENTATION_GUARD */
#endif /* YSP_TABLE_IMPLEMENTATION */

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
