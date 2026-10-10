/* table_test.c - self-checking test for ysp/table.h. No framework: it
 * returns 0 when every check passed and 1 after printing each failure.
 *
 *     gcc -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Werror -Iinclude \
 *         -o table_test tests/adapt/table_test.c -lm
 *
 * The number parser is checked against strtod() in the C locale on a random
 * corpus (YTB_TEST_NUMBERS sets its size; default 300000) built to hit
 * the hard cases: exact halfway points, their neighbors, subnormals, the
 * ends of the range and the 19-digit limit. Then again after switching
 * LC_NUMERIC to a decimal-comma locale, to show the parse does not move.
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#define YSP_TABLE_IMPLEMENTATION
#define YTB_STDIO
#include "ysp/table.h"

#include <float.h>
#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The hash of check_view()'s block (see there). */
#define YTB_TEST_BLOCK_HASH 0x3e6e7f4ea49fc613ULL

/* ---------------------------------------------------------------- harness */

static int g_failures = 0;
static int g_checks = 0;

static void fail_s(int line, const char* what, const char* got, const char* want) {
    fprintf(stderr, "table_test: FAIL at line %d: %s\n  got:  [%s]\n  want: [%s]\n", line, what,
            got ? got : "(null)", want ? want : "(null)");
    g_failures++;
}

#define CHECK(cond) do { g_checks++; if (!(cond)) { \
    fprintf(stderr, "table_test: FAIL at line %d: %s\n", __LINE__, #cond); g_failures++; } } while (0)
#define CHECK_I(got, want) do { long long g_ = (long long)(got), w_ = (long long)(want); g_checks++; \
    if (g_ != w_) { fprintf(stderr, "table_test: FAIL at line %d: %s (got %lld, want %lld)\n", \
                            __LINE__, #got, g_, w_); g_failures++; } } while (0)
#define CHECK_S(got, want) do { const char* g_ = (got); const char* w_ = (want); g_checks++; \
    if (!g_ || !w_ || strcmp(g_, w_) != 0) fail_s(__LINE__, #got, g_, w_); } while (0)
#define CHECK_HAS(str, sub) do { const char* s_ = (str); g_checks++; \
    if (!s_ || !strstr(s_, (sub))) fail_s(__LINE__, "message lacks a substring", s_, (sub)); } while (0)

/* An 8-byte aligned arena; tables are views into it. */
static uint64_t g_arena64[(8u << 20) / 8];
static unsigned char* const g_arena = (unsigned char*)g_arena64;
static uint64_t g_arena2_64[(1u << 20) / 8];
static unsigned char* const g_arena2 = (unsigned char*)g_arena2_64;

static bool parse(ytb_table* t, const char* text, const ytb_csv_desc* base) {
    ytb_csv_desc d;
    if (base) d = *base;
    else memset(&d, 0, sizeof(d));
    d.text = text;
    d.len = strlen(text);
    d.arena = g_arena;
    d.arena_size = sizeof(g_arena64);
    return ytb_open_csv(t, &d);
}

/* -------------------------------------------------------------- numbers */

static uint64_t g_rng = 0x1234567ULL;
static uint64_t rnd64(void) {
    uint64_t z = (g_rng += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}
static int rnd(int m) { return (int)(rnd64() % (uint64_t)m); }

static bool same_bits(double a, double b) { return memcmp(&a, &b, sizeof(a)) == 0; }

/* What ytb_parse_number must give for s: strtod's value in the C locale,
 * or the reason it must refuse. strtod accepts things this syntax does not
 * (spaces, hex, inf), so the corpus only holds strings of the syntax. */
static int oracle(const char* s, double* out) {
    char* end;
    double v;
    const char* p = s;
    int nd = 0;
    bool seen = false;
    /* Significant digits, by the syntax's rule: from the first nonzero
     * digit to the last nonzero digit of the mantissa. */
    {
        const char* first = NULL, *last = NULL;
        for (p = s; *p && *p != 'e' && *p != 'E'; p++)
            if (*p >= '1' && *p <= '9') { if (!first) first = p; last = p; }
        if (first)
            for (p = first; p <= last; p++) if (*p >= '0' && *p <= '9') nd++;
        seen = first != NULL;
    }
    {
        /* The stated syntax, checked here independently of the parser. */
        const char* q = s;
        int dig = 0;
        if (*q == '+' || *q == '-') q++;
        while (*q >= '0' && *q <= '9') { q++; dig++; }
        if (*q == '.') { q++; while (*q >= '0' && *q <= '9') { q++; dig++; } }
        if (!dig) return YTB_NUM_SYNTAX;
        if (*q == 'e' || *q == 'E') {
            int ed = 0;
            q++;
            if (*q == '+' || *q == '-') q++;
            while (*q >= '0' && *q <= '9') { q++; ed++; }
            if (!ed) return YTB_NUM_SYNTAX;
        }
        if (*q) return YTB_NUM_SYNTAX;
    }
    if (nd > YTB_MAX_DIGITS) return YTB_NUM_DIGITS;
    v = strtod(s, &end);
    if (*end != '\0') return YTB_NUM_SYNTAX;
    if (v == HUGE_VAL || v == -HUGE_VAL) return YTB_NUM_RANGE;
    if (seen && v == 0.0) return YTB_NUM_RANGE;
    *out = v;
    return YTB_NUM_OK;
}

static int g_num_bad = 0;
static void num_case(const char* s) {
    double got = 0.0, want = 0.0;
    int rg = ytb_parse_number(s, strlen(s), &got);
    int rw = oracle(s, &want);
    g_checks++;
    if (rg != rw || (rg == YTB_NUM_OK && !same_bits(got, want))) {
        if (g_num_bad++ < 20)
            fprintf(stderr, "table_test: FAIL number [%s]: got rc %d %.17g, strtod rc %d %.17g\n",
                    s, rg, got, rw, want);
        g_failures++;
    }
}

/* A decimal string of the syntax: up to 19 significant digits, a point
 * somewhere or nowhere, an exponent or none. */
static void rand_decimal(char* buf) {
    int nd = 1 + rnd(19), pt = rnd(nd + 1), k = 0, i;
    if (rnd(4) == 0) buf[k++] = '-';
    else if (rnd(8) == 0) buf[k++] = '+';
    if (rnd(5) == 0) { buf[k++] = '0'; if (pt == 0) buf[k++] = '.'; }
    for (i = 0; i < nd; i++) {
        if (i == pt && pt > 0 && rnd(3)) buf[k++] = '.';
        buf[k++] = (char)('0' + (i == 0 ? 1 + rnd(9) : rnd(10)));
    }
    if (rnd(2)) {
        int e = rnd(700) - 360;
        k += sprintf(buf + k, "%c%d", rnd(2) ? 'e' : 'E', e);
    }
    buf[k] = '\0';
}

static void check_numbers(void) {
    static const char* const fixed[] = {
        "0", "-0", "+0", "0.0", "00.000", "1", "-1", "1.5", ".5", "5.", "1e10", "1E-5", "+3",
        "0.1", "0.2", "0.3", "0.30000000000000004", "123456789012345678", "1234567890123456789",
        "9007199254740992", "9007199254740993", "9007199254740994", "9007199254740995",
        "18014398509481985", "18014398509481987", "1.7976931348623157e308",
        "1.7976931348623158e308", "1.7976931348623159e308", "4.9406564584124654e-324",
        "2.4703282292062328e-324", "2.4703282292062327e-324", "2.2250738585072011e-308",
        "2.2250738585072012e-308", "2.2250738585072014e-308", "1e-400", "1e400", "1e308", "1e309",
        "1e-323", "1e-324", "1e-325", "0.000000000000000000000000000001", "1.0000000000000000000",
        "100000000000000000000000", "1e23", "8.533e-7", "123.456e-2", "7e22", "7e23", "9e15",
        "12345678901234567890", "0.12345678901234567890", "1.00000000000000000001",
        "1e100000000", "1e-100000000", "1.5e0300", "3.14159265358979323", "6.02214076e23",
        "", "+", "-", ".", "-.", "1.2.3", "1e", "1e+", "1e-", " 1", "1 ", "0x10", "inf", "nan",
        "1,5", "1_000", "e5", "--1", "+-1", "1e5.5", "1.e5", ".e5",
        /* Exact quotients with a half bit and set bits below it: the bits
         * below must round up what would otherwise read as a tie. */
        "9007199254740993.5", "4503599627370498.75", "4503599627370498.5", "4503599627370499.5",
    };
    char buf[96];
    size_t i;
    int n, extra = 300000, k;
    const char* env = getenv("YTB_TEST_NUMBERS");
    if (env) extra = atoi(env);
    for (i = 0; i < sizeof(fixed) / sizeof(fixed[0]); i++) num_case(fixed[i]);
    for (n = 0; n < extra; n++) {
        switch (n % 6) {
        case 0: case 1:
            rand_decimal(buf);
            break;
        case 2: {
            /* An exact halfway point between two doubles: (2a+1) 2^k with
             * a 53-bit a; then its neighbors. */
            uint64_t a = (rnd64() >> 11) | ((uint64_t)1 << 52);
            int sh = rnd(10), d = rnd(3) - 1;
            uint64_t h = ((2 * a + 1) << sh);
            if (h >= 10000000000000000000ULL) h >>= 1;
            sprintf(buf, "%llu", (unsigned long long)(h + (uint64_t)(int64_t)d));
            break;
        }
        case 3: {
            /* printf round trips of random doubles, including subnormals. */
            uint64_t u = rnd64();
            double v;
            if (rnd(4) == 0) u &= 0x000FFFFFFFFFFFFFULL;     /* subnormal */
            memcpy(&v, &u, sizeof(v));
            if (v != v || v == HUGE_VAL || v == -HUGE_VAL) v = 1.0;
            k = 13 + rnd(5);
            sprintf(buf, "%.*e", k, v);
            break;
        }
        case 4: {
            /* Near the subnormal and normal boundaries and DBL_MAX. */
            static const double edges[] = { 4.9406564584124654e-324, 2.2250738585072014e-308,
                                            1.7976931348623157e308, 1e-320, 1e-310 };
            double v = edges[rnd(5)];
            uint64_t u;
            memcpy(&u, &v, sizeof(u));
            u += (uint64_t)(rnd(5) - 2);
            memcpy(&v, &u, sizeof(v));
            if (v != v || v <= 0 || v == HUGE_VAL) v = 4.9406564584124654e-324;
            sprintf(buf, "%.*e", 14 + rnd(4), v);
            break;
        }
        default: {
            /* Many digits around 19 and long zero runs. */
            int nd = 17 + rnd(4), j, p = 0;
            for (j = 0; j < nd; j++) buf[p++] = (char)('0' + (j == 0 ? 1 + rnd(9) : (rnd(3) ? 0 : rnd(10))));
            p += sprintf(buf + p, "e%d", rnd(80) - 40);
            buf[p] = '\0';
            break;
        }
        }
        num_case(buf);
    }
    printf("numbers: %d fixed and %d random strings against strtod\n",
           (int)(sizeof(fixed) / sizeof(fixed[0])), extra);
}

static void check_locale(void) {
    static const char* const names[] = { "German", "de_DE.UTF-8", "de_DE", "fr_FR.UTF-8", "French" };
    size_t i;
    const char* got = NULL;
    double v = 0.0;
    for (i = 0; i < sizeof(names) / sizeof(names[0]) && !got; i++) got = setlocale(LC_ALL, names[i]);
    if (!got) {
        printf("locale: no decimal-comma locale on this system; the locale check did not run\n");
        return;
    }
    /* Proof the locale took: the C library now reads a comma. */
    {
        char* end;
        double c = strtod("0,5", &end);
        printf("locale: %s (strtod(\"0,5\") = %g)\n", got, c);
    }
    CHECK_I(ytb_parse_number("0.5", 3, &v), YTB_NUM_OK);
    CHECK(v == 0.5);
    CHECK_I(ytb_parse_number("0,5", 3, &v), YTB_NUM_SYNTAX);
    CHECK(ytb_parse_number("0,5", 3, &v) < 0);   /* v0.2: reasons are negative */
    CHECK_I(ytb_parse_number("1234.5678e-3", 12, &v), YTB_NUM_OK);
    CHECK(v == 1.2345678);
    {
        ytb_table t;
        CHECK(parse(&t, "a,b\n0.25,x\n1.5,y\n", NULL));
        CHECK(ytb_num(&t, 0, 0) == 0.25);
        CHECK(ytb_num(&t, 1, 0) == 1.5);
        CHECK_I(ytb_col_type(&t, 0), YTB_NUMBER);
    }
    setlocale(LC_ALL, "C");
}

static void check_ints(void) {
    int32_t v = 0;
    CHECK_I(ytb_parse_int("2147483647", 10, &v), YTB_NUM_OK); CHECK_I(v, 2147483647);
    CHECK_I(ytb_parse_int("-2147483648", 11, &v), YTB_NUM_OK); CHECK_I(v, -2147483647 - 1);
    CHECK_I(ytb_parse_int("2147483648", 10, &v), YTB_NUM_RANGE);
    CHECK_I(ytb_parse_int("-2147483649", 11, &v), YTB_NUM_RANGE);
    CHECK_I(ytb_parse_int("99999999999999999999999", 23, &v), YTB_NUM_RANGE);
    CHECK_I(ytb_parse_int("007", 3, &v), YTB_NUM_OK); CHECK_I(v, 7);
    CHECK_I(ytb_parse_int("-0", 2, &v), YTB_NUM_OK); CHECK_I(v, 0);
    CHECK_I(ytb_parse_int("+5", 2, &v), YTB_NUM_OK); CHECK_I(v, 5);
    CHECK_I(ytb_parse_int("", 0, &v), YTB_NUM_SYNTAX);
    CHECK_I(ytb_parse_int("-", 1, &v), YTB_NUM_SYNTAX);
    CHECK_I(ytb_parse_int("1.0", 3, &v), YTB_NUM_SYNTAX);
    CHECK_I(ytb_parse_int("1e3", 3, &v), YTB_NUM_SYNTAX);
    CHECK_I(ytb_parse_int(" 1", 2, &v), YTB_NUM_SYNTAX);
}

/* ------------------------------------------------------------------ CSV */

static void check_basic(void) {
    ytb_table t;
    const char* csv =
        "target,contrast,n,word\n"
        "a,0.50,1,cat\n"
        "b,0.25,2,\"dog, big\"\n"
        "a,0.5,3,\"say \"\"hi\"\"\"\n"
        "c,1e-1,1,\"two\nlines\"\n";
    CHECK(parse(&t, csv, NULL));
    CHECK_S(ytb_error(&t), "");
    CHECK_I(t.n_rows, 4);
    CHECK_I(t.n_cols, 4);
    CHECK_I(t.n_skipped, 0);
    CHECK_S(ytb_col_name(&t, 0), "target");
    CHECK_S(ytb_col_name(&t, 3), "word");
    CHECK_I(ytb_col(&t, "contrast"), 1);
    CHECK_I(ytb_col(&t, "nope"), -1);
    CHECK_I(ytb_col_type(&t, 0), YTB_STRING);
    CHECK_I(ytb_col_type(&t, 1), YTB_NUMBER);
    CHECK_I(ytb_col_type(&t, 2), YTB_INTEGER);
    CHECK_I(ytb_col_type(&t, 3), YTB_STRING);
    /* Levels: first appearance; numbers by value, text as first written. */
    CHECK_I(ytb_n_levels(&t, 0), 3);
    CHECK_I(ytb_level(&t, 2, 0), 0);
    CHECK_I(ytb_level(&t, 3, 0), 2);
    CHECK_I(ytb_n_levels(&t, 1), 3);
    CHECK_I(ytb_level(&t, 2, 1), 0);
    CHECK_S(ytb_text(&t, 2, 1), "0.50");
    CHECK(ytb_num(&t, 2, 1) == 0.5);
    CHECK(ytb_num(&t, 3, 1) == 0.1);
    CHECK_S(ytb_text(&t, 3, 1), "1e-1");
    CHECK_I(ytb_find(&t, 1, "0.5"), 0);
    CHECK_I(ytb_find(&t, 1, "0.500000"), 0);
    CHECK_I(ytb_find(&t, 1, "0.1"), 2);
    CHECK_I(ytb_find(&t, 1, "0.7"), -1);
    CHECK_I(ytb_find(&t, 0, "b"), 1);
    CHECK_I(ytb_find(&t, 0, "B"), -1);
    CHECK_I(ytb_int(&t, 1, 2), 2);
    CHECK_I(ytb_int(&t, 1, 1), INT32_MIN);
    CHECK_I(ytb_n_levels(&t, 2), 3);
    CHECK_S(ytb_text(&t, 1, 3), "dog, big");
    CHECK_S(ytb_text(&t, 2, 3), "say \"hi\"");
    CHECK_S(ytb_text(&t, 3, 3), "two\nlines");
    CHECK(isnan(ytb_num(&t, 0, 0)));
    CHECK_S(ytb_level_text(&t, 0, 2), "c");
    {
        const unsigned char* lb = ytb_level_bytes(&t, 0);
        CHECK(lb != NULL);
        if (lb) CHECK_I(lb[6] | lb[7] << 8, 2);
    }
    /* Bad arguments. */
    CHECK_I(ytb_level(&t, 4, 0), -1);
    CHECK_I(ytb_level(&t, 0, 4), -1);
    CHECK_I(ytb_level(&t, -1, 0), -1);
    CHECK(ytb_text(&t, 0, -1) == NULL);
    CHECK(ytb_level_text(&t, 0, 3) == NULL);
    CHECK(isnan(ytb_level_num(&t, 1, 9)));
    CHECK_I(ytb_n_levels(NULL, 0), -1);
    CHECK_I(ytb_col(NULL, "x"), -1);
    CHECK_I(ytb_hash(NULL), 0);
    CHECK_I(ytb_col_type(&t, 99), YTB_AUTO);
    CHECK(ytb_level_bytes(&t, 7) == NULL);
    CHECK_I(ytb_find(&t, 0, NULL), -1);
    /* The need is exact enough: the parse fits it and fails 8 bytes short. */
    {
        ytb_csv_desc d;
        ytb_table u;
        size_t need = t.need;
        memset(&d, 0, sizeof(d));
        d.text = csv;
        d.len = strlen(csv);
        CHECK(!ytb_open_csv(&u, &d));
        CHECK_I(u.need, need);
        CHECK_HAS(ytb_error(&u), "no arena");
        d.arena = g_arena2;
        d.arena_size = need;
        CHECK(ytb_open_csv(&u, &d));
        d.arena_size = need - 8;
        CHECK(!ytb_open_csv(&u, &d));
        CHECK_HAS(ytb_error(&u), "the parse needs");
        d.arena = g_arena2 + 4;
        d.arena_size = need;
        CHECK(!ytb_open_csv(&u, &d));
        CHECK_HAS(ytb_error(&u), "aligned");
    }
}

static void check_dialect(void) {
    ytb_table t;
    ytb_csv_desc d;
    /* BOM, CRLF, no final newline. */
    CHECK(parse(&t, "\xEF\xBB\xBF" "x,y\r\n1,a\r\n2,b", NULL));
    CHECK_S(ytb_col_name(&t, 0), "x");
    CHECK_I(t.n_rows, 2);
    CHECK_S(ytb_text(&t, 1, 1), "b");
    CHECK_I(t.n_skipped, 0);
    /* A CRLF counts one line, after a quoted field or not. */
    CHECK(!parse(&t, "x,y\r\n1,a\r\n\"2\",b\r\n3,\"c\"d\r\n", NULL));
    CHECK_I(t.err_line, 4);
    /* CR inside quotes is data; CRLF inside quotes too. */
    CHECK(parse(&t, "x\n\"a\r\nb\"\n\"c\rd\"\n", NULL));
    CHECK_S(ytb_text(&t, 0, 0), "a\r\nb");
    CHECK_S(ytb_text(&t, 1, 0), "c\rd");
    /* All-empty records are skipped and counted; a trailing blank line too. */
    CHECK(parse(&t, "x,y\n1,a\n,\n\n2,b\n,\n\n", NULL));
    CHECK_I(t.n_rows, 2);
    CHECK_I(t.n_skipped, 4);
    CHECK_I(ytb_int(&t, 1, 0), 2);
    /* Spaces are data: " 1" makes the column a string column. */
    CHECK(parse(&t, "x\n1\n 2\n", NULL));
    CHECK_I(ytb_col_type(&t, 0), YTB_STRING);
    CHECK_S(ytb_text(&t, 1, 0), " 2");
    /* A quoted number is a number; one holding "" is not. */
    CHECK(parse(&t, "x\n\"1.5\"\n2\n", NULL));
    CHECK_I(ytb_col_type(&t, 0), YTB_NUMBER);
    /* Integers and numbers in one column make it NUMBER. */
    CHECK(parse(&t, "x\n1\n2.5\n", NULL));
    CHECK_I(ytb_col_type(&t, 0), YTB_NUMBER);
    /* An empty column is a STRING column with the level "". */
    CHECK(parse(&t, "x,y\n,1\n,2\n", NULL));
    CHECK_I(ytb_col_type(&t, 0), YTB_STRING);
    CHECK_I(ytb_n_levels(&t, 0), 1);
    CHECK_S(ytb_text(&t, 0, 0), "");
    CHECK_I(ytb_find(&t, 0, ""), 0);
    /* -0 and 0 are one level. */
    CHECK(parse(&t, "x\n0.0\n-0\n1.25\n", NULL));
    CHECK_I(ytb_n_levels(&t, 0), 2);
    /* Header only: no rows. */
    CHECK(parse(&t, "a,b\n", NULL));
    CHECK_I(t.n_rows, 0);
    CHECK_I(t.n_cols, 2);
    CHECK_I(ytb_n_levels(&t, 0), 0);
    /* Delimiters. */
    memset(&d, 0, sizeof(d));
    d.delimiter = ';';
    CHECK(parse(&t, "a;b\n1,5;x\n2,5;y\n", &d));
    CHECK_I(ytb_col_type(&t, 0), YTB_STRING);
    CHECK_S(ytb_text(&t, 0, 0), "1,5");
    d.delimiter = '\t';
    CHECK(parse(&t, "a\tb\n1\t\"x\ty\"\n", &d));
    CHECK_S(ytb_text(&t, 0, 1), "x\ty");
    d.delimiter = '|';
    CHECK(!parse(&t, "a|b\n", &d));
    CHECK_HAS(ytb_error(&t), "delimiter");
    /* Declared types. */
    memset(&d, 0, sizeof(d));
    d.types[0].name = "code";
    d.types[0].type = YTB_STRING;
    d.types[1].name = "n";
    d.types[1].type = YTB_NUMBER;
    d.n_types = 2;
    CHECK(parse(&t, "code,n\n007,1\n7,2\n", &d));
    CHECK_I(ytb_col_type(&t, 0), YTB_STRING);
    CHECK_I(ytb_n_levels(&t, 0), 2);
    CHECK_S(ytb_text(&t, 0, 0), "007");
    CHECK_I(ytb_col_type(&t, 1), YTB_NUMBER);
    d.types[1].type = YTB_INTEGER;
    CHECK(!parse(&t, "code,n\n007,1\n7,2.5\n", &d));
    CHECK_HAS(ytb_error(&t), "line 3, row 2, column 'n' (2): '2.5' is not an integer");
    CHECK_I(t.err_line, 3);
    CHECK_I(t.err_row, 2);
    CHECK_I(t.err_col, 2);
    d.types[1].type = YTB_NUMBER;
    CHECK(!parse(&t, "code,n\n007,1\n7,\"0,5\"\n", &d));
    CHECK_HAS(ytb_error(&t), "'0,5' is not a number; the decimal separator is '.'");
    d.types[0].name = "nope";
    CHECK(!parse(&t, "code,n\n1,1\n", &d));
    CHECK_HAS(ytb_error(&t), "desc.types[0]: no column 'nope'");
    /* allow_empty. */
    CHECK(!parse(&t, "x,y\n1,a\n,b\n", NULL));
    CHECK_HAS(ytb_error(&t), "line 3, row 2, column 'x' (1): the cell is empty and the column is integer");
    memset(&d, 0, sizeof(d));
    d.allow_empty = true;
    CHECK(parse(&t, "x,y\n1,a\n,b\n3,c\n,d\n", &d));
    CHECK_I(ytb_col_type(&t, 0), YTB_NUMBER);
    CHECK(isnan(ytb_num(&t, 1, 0)));
    CHECK_I(ytb_n_levels(&t, 0), 3);
    CHECK_I(ytb_level(&t, 3, 0), ytb_level(&t, 1, 0));
    CHECK_S(ytb_text(&t, 1, 0), "");
    CHECK_I(ytb_find(&t, 0, ""), 1);
    /* Range and digit limits in a numeric column. */
    CHECK(!parse(&t, "x\n1\n1e999\n", NULL));
    CHECK_HAS(ytb_error(&t), "'1e999' is out of the range of a double");
    CHECK(!parse(&t, "x\n1\n12345678901234567890.5\n", NULL));
    CHECK_HAS(ytb_error(&t), "more than 19 significant digits");
    CHECK(!parse(&t, "x\n1\n99999999999\n", NULL) || ytb_col_type(&t, 0) == YTB_NUMBER);
    /* Integers past int32 make the column NUMBER, not an error. */
    CHECK(parse(&t, "x\n1\n99999999999\n", NULL));
    CHECK_I(ytb_col_type(&t, 0), YTB_NUMBER);
}

static void check_errors(void) {
    ytb_table t;
    static char big[200000];
    int i, k;
    struct { const char* csv; const char* msg; int line, row, col; } cases[] = {
        { "", "the input is empty", 1, 0, 0 },
        { "\xEF\xBB\xBF", "the input is empty", 1, 0, 0 },
        { "\n1\n", "the header line is empty", 1, 0, 0 },
        { "a,b\n1,\"x\n2,y\n", "a quoted field is not closed", 2, 1, 2 },
        { "a,b\n1,x\"y\n", "a quote inside an unquoted field", 2, 1, 2 },
        { "a,b\n1,\"xy\"z\n", "a character after a closing quote", 2, 1, 2 },
        { "a,b\n1,x\ry\n", "a CR that is not followed by LF", 2, 1, 2 },
        { "a,b\n1,x\r", "a CR that is not followed by LF", 2, 1, 2 },
        { "a,b\n1,2,3\n", "row has more than the header's 2 fields", 2, 1, 0 },
        { "a,b\n1\n", "row has 1 fields, the header has 2", 2, 1, 0 },
        { "a,b\n1,\xC0\x80\n", "not valid UTF-8", 2, 1, 2 },
        { "a,b\n1,\xED\xA0\x80\n", "not valid UTF-8", 2, 1, 2 },
        { "a,b\n1,\xF5\x80\x80\x80\n", "not valid UTF-8", 2, 1, 2 },
        { "a,b\n1,\xE2\x82\n", "not valid UTF-8", 2, 1, 2 },
        { "a,1b\n1,2\n", "column name '1b' is not a name", 1, 0, 2 },
        { "a,b c\n1,2\n", "column name 'b c' is not a name", 1, 0, 2 },
        { "a,,c\n1,2,3\n", "column name '' is not a name", 1, 0, 2 },
        { "a,\"b\"\"\"\n1,2\n", "is not a name", 1, 0, 2 },
        { "a,b,a\n1,2,3\n", "column name 'a' appears twice", 1, 0, 3 },
        { "a,b\n1,2\n3,4\n5,\"6\n7\",8\n", "row has more", 4, 3, 0 },
    };
    for (i = 0; i < (int)(sizeof(cases) / sizeof(cases[0])); i++) {
        g_checks++;
        if (parse(&t, cases[i].csv, NULL)) {
            fprintf(stderr, "table_test: FAIL: case %d parsed: %s\n", i, cases[i].msg);
            g_failures++;
            continue;
        }
        CHECK_HAS(ytb_error(&t), cases[i].msg);
        if (t.err_line != cases[i].line || t.err_row != cases[i].row || t.err_col != cases[i].col) {
            fprintf(stderr, "table_test: FAIL: case %d at line %d row %d col %d, want %d %d %d (%s)\n", i,
                    t.err_line, t.err_row, t.err_col, cases[i].line, cases[i].row, cases[i].col,
                    ytb_error(&t));
            g_failures++;
        }
        CHECK(t.base == NULL && t.n_rows == 0);
    }
    /* A NUL byte (strlen stops at it, so give the length). */
    {
        ytb_csv_desc d;
        static const char nul[] = "a\n1\0\n";
        memset(&d, 0, sizeof(d));
        d.text = nul;
        d.len = sizeof(nul) - 1;
        d.arena = g_arena;
        d.arena_size = sizeof(g_arena64);
        CHECK(!ytb_open_csv(&t, &d));
        CHECK_HAS(ytb_error(&t), "a NUL byte");
    }
    /* 64 columns fit, 65 do not; names are full length. */
    k = 0;
    for (i = 0; i < 65; i++) k += sprintf(big + k, "%sc%d", i ? "," : "", i);
    big[k++] = '\n';
    big[k] = '\0';
    CHECK(!parse(&t, big, NULL));
    CHECK_HAS(ytb_error(&t), "more than 64 columns");
    k = 0;
    for (i = 0; i < 64; i++) k += sprintf(big + k, "%sc%d", i ? "," : "", i);
    k += sprintf(big + k, "\n");
    for (i = 0; i < 64; i++) k += sprintf(big + k, "%s%d", i ? "," : "", i);
    k += sprintf(big + k, "\n");
    CHECK(parse(&t, big, NULL));
    CHECK_I(t.n_cols, 64);
    /* A name of 63 bytes is a name, 64 is not. */
    memset(big, 'n', 63);
    strcpy(big + 63, "\n1\n");
    CHECK(parse(&t, big, NULL));
    memset(big, 'n', 64);
    strcpy(big + 64, "\n1\n");
    CHECK(!parse(&t, big, NULL));
    CHECK_HAS(ytb_error(&t), "is not a name");
    /* A field of 4096 bytes fits, 4097 does not, quoted or not. */
    k = sprintf(big, "x\n");
    memset(big + k, 'q', 4096);
    strcpy(big + k + 4096, "\n");
    CHECK(parse(&t, big, NULL));
    k = sprintf(big, "x\n\"");
    memset(big + k, 'q', 4097);
    strcpy(big + k + 4097, "\"\n");
    CHECK(!parse(&t, big, NULL));
    CHECK_HAS(ytb_error(&t), "a field of 4097 bytes (at most 4096)");
    k = sprintf(big, "x\n\"");
    for (i = 0; i < 2048; i++) { big[k++] = '"'; big[k++] = '"'; }
    strcpy(big + k, "\"\n");
    CHECK(parse(&t, big, NULL));
    CHECK_I(strlen(ytb_text(&t, 0, 0)), 2048);
}

static void check_rows_bound(void) {
    static char buf[32769 * 8 + 64];
    ytb_table t;
    int r, k = sprintf(buf, "n,m\n");
    for (r = 0; r < YTB_MAX_ROWS; r++) k += sprintf(buf + k, "%d,%d\n", r, r % 7);
    CHECK(parse(&t, buf, NULL));
    CHECK_I(t.n_rows, YTB_MAX_ROWS);
    CHECK_I(ytb_n_levels(&t, 0), YTB_MAX_ROWS);
    CHECK_I(ytb_n_levels(&t, 1), 7);
    CHECK_I(ytb_int(&t, YTB_MAX_ROWS - 1, 0), YTB_MAX_ROWS - 1);
    CHECK_I(ytb_level(&t, YTB_MAX_ROWS - 1, 0), YTB_MAX_ROWS - 1);
    CHECK_I(ytb_level(&t, 300, 0), 300);
    {
        const unsigned char* lb = ytb_level_bytes(&t, 0);
        CHECK(lb && (lb[600] | lb[601] << 8) == 300);
    }
    sprintf(buf + k, "1,1\n");
    CHECK(!parse(&t, buf, NULL));
    CHECK_HAS(ytb_error(&t), "more than 32767 rows");
}

/* --------------------------------------------------------------- the block */

static void rehash(unsigned char* b) {
    uint64_t size = ytb__rd64(b + 24);
    ytb__wr64(b + 32, ytb__block_hash(b, (size_t)size));
}

static void check_view(void) {
    ytb_table t, v;
    static unsigned char copy[1 << 16];
    size_t i, n;
    int r, c, bit, refused = 0;
    const char* csv = "w,x,y\nab,1,0.5\ncd,2,0.25\nab,3,0.5\n\xC3\xA9t\xC3\xA9,4,1e3\n";
    CHECK(parse(&t, csv, NULL));
    n = t.size;
    CHECK(n % 8 == 0);
    if (n + 8 > sizeof(copy)) {
        CHECK(n + 8 <= sizeof(copy));
        return;
    }
    memcpy(copy, t.base, n);
    CHECK(ytb_open_view(&v, copy, n));
    CHECK_I(v.n_rows, t.n_rows);
    CHECK_I(v.n_cols, t.n_cols);
    CHECK_I(ytb_hash(&v), ytb_hash(&t));
    for (r = 0; r < t.n_rows; r++)
        for (c = 0; c < t.n_cols; c++) {
            CHECK_S(ytb_text(&v, r, c), ytb_text(&t, r, c));
            CHECK_I(ytb_level(&v, r, c), ytb_level(&t, r, c));
        }
    CHECK_S(ytb_text(&v, 3, 0), "\xC3\xA9t\xC3\xA9");
    /* v0.2: close forgets the view and is safe zeroed, after a failed open
     * and twice; a closed view answers like an empty one. */
    {
        ytb_table z;
        memset(&z, 0, sizeof(z));
        ytb_close(&z);
        ytb_close(NULL);
        CHECK(!ytb_open_view(&z, copy, 7));
        CHECK(ytb_error(&z)[0] != '\0');
        ytb_close(&z);
        ytb_close(&z);
        CHECK(z.base == NULL);
        CHECK_I(ytb_col(&z, "w"), -1);
        CHECK(ytb_text(&z, 0, 0) == NULL);
        CHECK(ytb_open_view(&z, copy, n));
        CHECK(ytb_error(&z)[0] == '\0');
        ytb_close(&z);
        CHECK_I(z.n_rows, 0);
    }
    /* The block's bytes are a function of the input alone: the same parse
     * gives the same hash on every compiler (this constant was printed by
     * the first gcc and MSVC runs, which agreed). */
    printf("block: %lu bytes, hash %016llx\n", (unsigned long)n, (unsigned long long)ytb_hash(&t));
    CHECK(ytb_hash(&t) == YTB_TEST_BLOCK_HASH);
    /* Every single-bit flip and every truncation is refused. */
    for (i = 0; i < n; i++) {
        for (bit = 0; bit < 8; bit++) {
            copy[i] ^= (unsigned char)(1u << bit);
            if (!ytb_open_view(&v, copy, n)) refused++;
            copy[i] ^= (unsigned char)(1u << bit);
        }
    }
    CHECK_I(refused, (long long)n * 8);
    for (i = 0; i < n; i++) CHECK(!ytb_open_view(&v, copy, i));
    CHECK(ytb_open_view(&v, copy, n + 8));   /* trailing bytes beyond size are fine */
    /* Structural checks behind a valid hash. */
    {
        unsigned char* cd = copy + YTB_HEADER_SIZE;
        uint64_t roff = ytb__rd64(cd + 16), loff = ytb__rd64(cd + 24);
        copy[roff] = 9;               /* row 0's level of column w: 9 of 3 */
        rehash(copy);
        CHECK(!ytb_open_view(&v, copy, n));
        CHECK_HAS(ytb_error(&v), "has level 9 of 3");
        copy[roff] = 3;               /* the first level out of range */
        rehash(copy);
        CHECK(!ytb_open_view(&v, copy, n));
        CHECK_HAS(ytb_error(&v), "has level 3 of 3");
        memcpy(copy, t.base, n);
        {
            /* Bad UTF-8 in the pool behind a valid hash. */
            unsigned char* l = copy + loff;
            copy[ytb__rd32(l)] = 0xFF;
            rehash(copy);
            CHECK(!ytb_open_view(&v, copy, n));
            CHECK_HAS(ytb_error(&v), "not valid UTF-8");
        }
        memcpy(copy, t.base, n);
        copy[ytb__rd32(cd)] = '9';  /* the name "w" becomes "9" */
        rehash(copy);
        CHECK(!ytb_open_view(&v, copy, n));
        CHECK_HAS(ytb_error(&v), "has no valid name");
        memcpy(copy, t.base, n);
        ytb__wr32(cd + 8, 7);       /* type 7 */
        rehash(copy);
        CHECK(!ytb_open_view(&v, copy, n));
        memcpy(copy, t.base, n);
        ytb__wr64(cd + 24, loff + 4);   /* misaligned levels */
        rehash(copy);
        CHECK(!ytb_open_view(&v, copy, n));
        memcpy(copy, t.base, n);
        ytb__wr32(copy + YTB_HEADER_SIZE + 32 + 12, 99);   /* x: 99 levels for 4 rows */
        rehash(copy);
        CHECK(!ytb_open_view(&v, copy, n));
        memcpy(copy, t.base, n);
        {
            /* An INTEGER level that is not an int32. */
            unsigned char* xd = copy + YTB_HEADER_SIZE + 32;
            ytb__wrf64(copy + ytb__rd64(xd + 24) + 8, 1.5);
            rehash(copy);
            CHECK(!ytb_open_view(&v, copy, n));
            CHECK_HAS(ytb_error(&v), "is not an int32");
        }
        memcpy(copy, t.base, n);
        {
            /* A level text whose NUL is gone. */
            unsigned char* l = copy + loff;
            copy[ytb__rd32(l) + ytb__rd32(l + 4)] = 'z';
            rehash(copy);
            CHECK(!ytb_open_view(&v, copy, n));
        }
        memcpy(copy, t.base, n);
        ytb__wr32(copy + 4, 2);
        CHECK(!ytb_open_view(&v, copy, n));
        CHECK_HAS(ytb_error(&v), "block format 2 is not 1");
    }
    CHECK(!ytb_open_view(&v, NULL, 64));
    CHECK(!ytb_open_view(&v, "PSTX", 4));
}

static void check_file(void) {
    static const char name[] = "ysp_table_test_t\xC3\xA1" "bla.csv";
    FILE* f;
    ytb_table t;
    ytb_csv_desc d;
#ifdef _WIN32
    static wchar_t wname[64];
    CHECK(ytb__wide(name, wname, 64));
#if defined(_MSC_VER)
    if (_wfopen_s(&f, wname, L"wb") != 0) f = NULL;
#else
    f = _wfopen(wname, L"wb");
#endif
#else
    f = fopen(name, "wb");
#endif
    CHECK(f != NULL);
    if (!f) return;
    fputs("a,b\n1,x\n2,y\n", f);
    fclose(f);
    memset(&d, 0, sizeof(d));
    d.arena = g_arena;
    d.arena_size = sizeof(g_arena64);
    CHECK(ytb_open_csv_file(&t, name, &d));
    CHECK_I(t.n_rows, 2);
    CHECK_S(ytb_text(&t, 1, 1), "y");
    d.arena_size = 64;
    CHECK(!ytb_open_csv_file(&t, name, &d));
    CHECK(!ytb_open_csv_file(&t, "no_such_file.csv", &d));
    CHECK_HAS(ytb_error(&t), "cannot open");
#ifdef _WIN32
    _wremove(wname);
#else
    remove(name);
#endif
}

int main(void) {
    printf("ysp_table %s\n", ytb_version());
    CHECK_S(ytb_version(), YTB_VERSION_STRING);
    check_numbers();
    check_ints();
    check_basic();
    check_dialect();
    check_errors();
    check_rows_bound();
    check_view();
    check_file();
    check_locale();
    if (g_failures) {
        fprintf(stderr, "table_test: %d of %d checks FAILED\n", g_failures, g_checks);
        return 1;
    }
    printf("table_test: all %d checks passed\n", g_checks);
    return 0;
}
