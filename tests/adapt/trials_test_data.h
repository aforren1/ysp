/* trials_test_data.h - the v0.4.0 part of trials_test.c (included by it,
 * after trials_test_v02.h, whose table helpers it uses): the data writer.
 * Every file written here is read back with ytb_open_csv() and compared cell by
 * cell with what went in, so "the file reads back" is checked, not assumed.
 */

#include <locale.h>
#include <wchar.h>

/* An in-memory sink: the bytes, and how many calls brought them. */
static char g_sink[1 << 20];
static size_t g_sink_len;
static int g_sink_calls;
static int g_sink_fail_after = -1;  /* calls that succeed before one fails;
                                     * -1 = none fails */

/* One call fails, the later ones succeed: a transient fault, so a writer
 * that went on after it would leave its bytes in the sink. */
static bool sink_write(void* ctx, const void* bytes, size_t n) {
    (void)ctx;
    if (g_sink_fail_after >= 0 && g_sink_calls == g_sink_fail_after) {
        g_sink_fail_after = -1;
        return false;
    }
    g_sink_calls++;
    if (g_sink_len + n > sizeof(g_sink)) return false;
    memcpy(g_sink + g_sink_len, bytes, n);
    g_sink_len += n;
    return true;
}

static void sink_reset(void) {
    g_sink_len = 0;
    g_sink_calls = 0;
    g_sink_fail_after = -1;
}

static ytr_data g_w;
static uint64_t g_back_arena64[(1u << 22) / 8];
static ytb_table g_back;

/* Parse the sink (or any bytes) back; every column STRING when asked, so
 * a text that looks like a number keeps its text. */
static bool read_back(const char* text, size_t len, bool all_string) {
    ytb_csv_desc cd;
    memset(&cd, 0, sizeof(cd));
    cd.text = text;
    cd.len = len;
    cd.allow_empty = true;
    cd.arena = g_back_arena64;
    cd.arena_size = sizeof(g_back_arena64);
    if (all_string) {
        cd.types[0].name = "s";
        cd.types[0].type = YTB_STRING;
        cd.n_types = 1;
    }
    if (!ytb_open_csv(&g_back, &cd)) {
        fprintf(stderr, "trials_test: read back: %s\n", ytb_error(&g_back));
        return false;
    }
    return true;
}

typedef struct data_row {
    int         key;
    bool        correct;
    double      level;
    double      fixed;
    int64_t     count;
    int64_t     rt_ns;
    int64_t     onset_ns;
    const char* label;
    char        code[8];
} data_row;

static void test_data_formats(void) {
    static const ytr_col cols[] = {
        YTR_COL(data_row, key, YTR_COL_INT),
        YTR_COL(data_row, correct, YTR_COL_BOOL),
        YTR_COL(data_row, level, YTR_COL_DOUBLE),
        YTR_COL_D(data_row, fixed, YTR_COL_DOUBLE, 3),
        YTR_COL(data_row, count, YTR_COL_INT64),
        YTR_COL(data_row, rt_ns, YTR_COL_TIME_NS),
        YTR_COL_D(data_row, onset_ns, YTR_COL_TIME_NS, 3),
        YTR_COL(data_row, label, YTR_COL_STRING),
        YTR_COL(data_row, code, YTR_COL_CHARS),
        YTR_COL_TEXT("participant", "P,07"),
        YTR_COL_SET("note", YTR_COL_STRING),
    };
    ytr_data_desc dd;
    data_row r;
    const char* want;

    memset(&dd, 0, sizeof(dd));
    dd.write = sink_write;
    dd.cols = cols;
    dd.n_cols = (int)(sizeof(cols) / sizeof(cols[0]));
    sink_reset();
    CHECK(ytr_data_open(&g_w, &dd));
    CHECK_S(ytr_data_error(&g_w), "");
    CHECK_I(g_sink_calls, 1);
    CHECK_I(ytr_data_col(&g_w, "note"), 10);
    CHECK_I(ytr_data_col(&g_w, "nope"), -1);
    CHECK_I(ytr_data_col(&g_w, "rt_ns"), 5);  /* declared name */
    CHECK_I(ytr_data_col(&g_w, "rt_s"), 5);   /* written name */
    CHECK_I(ytr_data_col(&g_w, "count_s"), -1);  /* INT64: no rename */

    memset(&r, 0, sizeof(r));
    r.key = 42;
    r.correct = true;
    r.level = 0.1;
    r.fixed = 2.0 / 3.0;
    r.count = INT64_MAX;
    r.rt_ns = 1184516667;
    r.onset_ns = -1234567890;        /* rounds half away from zero */
    r.label = "a \"b\", c";
    memcpy(r.code, "ABCDEFGH", 8);   /* full, no NUL: all 8 bytes */
    CHECK_I(ytr_data_set_string(&g_w, 10, "line\nbreak"), 0);
    CHECK_I(ytr_data_write(&g_w, &r), 0);
    CHECK_I(g_sink_calls, 2);        /* one call per row */

    r.key = YTR_MISSING_INT;
    r.correct = false;
    r.level = NAN;
    r.fixed = -0.0004;               /* rounds to -0.000 */
    r.count = YTR_MISSING_I64;
    r.rt_ns = YTR_MISSING_NS;
    r.onset_ns = -400000;            /* -0.0004 s: no sign on 0.000 */
    r.label = NULL;
    memcpy(r.code, "x\0yz", 4);
    CHECK_I(ytr_data_write(&g_w, &r), 0);   /* "note" was not set again */

    r.key = -7;
    r.level = 1e-300;
    r.fixed = 1e6;
    r.count = INT64_MIN + 1;
    r.rt_ns = INT64_MAX;
    r.onset_ns = 500000;             /* 0.0005 s: up to 0.001 */
    r.label = "";
    r.code[0] = '\0';
    CHECK_I(ytr_data_write(&g_w, &r), 0);
    CHECK_I((long)ytr_data_rows(&g_w), 3);
    CHECK_I(ytr_data_finish(&g_w), 0);
    CHECK_I(ytr_data_finish(&g_w), YTR_ERR_CLOSED);
    CHECK_HAS(ytr_data_error(&g_w), "ytr_data_finish: the writer is not open");
    ytr_data_close(&g_w);                   /* after finish, and twice: nothing */
    ytr_data_close(&g_w);
    CHECK_I(ytr_data_write(&g_w, &r), YTR_ERR_CLOSED);
    CHECK_HAS(ytr_data_error(&g_w), "ytr_data_write: the writer is not open");

    g_sink[g_sink_len] = '\0';
    want = "key,correct,level,fixed,count,rt_s,onset_s,label,code,participant,note\n"
           "42,1,0.1,0.667,9223372036854775807,1.184516667,-1.235,\"a \"\"b\"\", c\",ABCDEFGH,"
           "\"P,07\",\"line\nbreak\"\n"
           ",0,,-0.000,,,0.000,,x,\"P,07\",\n"
           "-7,0,1e-300,1000000.000,-9223372036854775807,"
           "9223372036.854775807,0.001,,,\"P,07\",\n";
    CHECK_S(g_sink, want);

    /* Back through ysp/table.h: the numbers exact, the texts as written. */
    CHECK(read_back(g_sink, g_sink_len, false));
    if (g_back.base) {
        CHECK_I(g_back.n_rows, 3);
        CHECK_I(g_back.n_cols, 11);
        CHECK(ytb_num(&g_back, 0, ytb_col(&g_back, "level")) == 0.1);
        CHECK(ytb_num(&g_back, 2, ytb_col(&g_back, "level")) == 1e-300);
        CHECK(is_nan(ytb_num(&g_back, 1, ytb_col(&g_back, "level"))));
        CHECK(ytb_num(&g_back, 0, ytb_col(&g_back, "rt_s")) == 1.184516667);
        CHECK_S(ytb_text(&g_back, 0, ytb_col(&g_back, "label")), "a \"b\", c");
        CHECK_S(ytb_text(&g_back, 0, ytb_col(&g_back, "participant")), "P,07");
        CHECK_S(ytb_text(&g_back, 0, ytb_col(&g_back, "note")), "line\nbreak");
        CHECK_S(ytb_text(&g_back, 1, ytb_col(&g_back, "note")), "");
        CHECK_S(ytb_text(&g_back, 0, ytb_col(&g_back, "count")), "9223372036854775807");
    }
}

/* Time columns: every decimals setting against a reference built from the
 * digits, for values around the rounding points. */
static void test_data_time(void) {
    static const int64_t vals[] = { 0, 1, -1, 4, 5, -5, 499999999, 500000000, 999999999,
                                    1000000000, -1000000000, 1499999999, 1500000000,
                                    -1500000000, INT64_MAX, INT64_MIN + 1 };
    ytr_col col = YTR_COL_SET("t", YTR_COL_TIME_NS);
    ytr_data_desc dd;
    int dec, v, bad = 0;
    for (dec = 1; dec <= 9; dec++) {
        char ref[64];
        memset(&dd, 0, sizeof(dd));
        dd.write = sink_write;
        col.decimals = dec;
        dd.cols = &col;
        dd.n_cols = 1;
        for (v = 0; v < (int)(sizeof(vals) / sizeof(vals[0])); v++) {
            /* Reference: the exact 9-digit decimal, rounded by hand. */
            uint64_t u = vals[v] < 0 ? (uint64_t)0 - (uint64_t)vals[v] : (uint64_t)vals[v];
            uint64_t whole = u / 1000000000u, frac = u % 1000000000u, p = 1, q;
            int k;
            const char* line;
            for (k = dec; k < 9; k++) p *= 10u;
            q = frac / p + ((frac % p) * 2u >= p && p > 1 ? 1u : 0u);
            {
                uint64_t top = 1;
                for (k = 0; k < dec; k++) top *= 10u;
                if (q == top) { whole++; q = 0; }
                snprintf(ref, sizeof(ref), "%s%llu.%0*llu",
                         (vals[v] < 0 && (whole || q)) ? "-" : "", (unsigned long long)whole,
                         dec, (unsigned long long)q);
            }
            sink_reset();
            CHECK(ytr_data_open(&g_w, &dd));
            CHECK_I(ytr_data_set_time_ns(&g_w, 0, vals[v]), 0);
            CHECK_I(ytr_data_write(&g_w, NULL), 0);
            ytr_data_close(&g_w);
            g_sink[g_sink_len] = '\0';
            line = strchr(g_sink, '\n');
            if (!line || strncmp(line + 1, ref, strlen(ref)) != 0 ||
                line[1 + strlen(ref)] != '\n') {
                if (bad++ < 5) fail_s(__LINE__, "time column", line ? line + 1 : "", ref);
            }
        }
    }
}

/* Doubles: random bit patterns (finite) and a few fixed values written
 * shortest, read back bit for bit through ysp/table.h's parser. */
static void test_data_doubles(void) {
    ytr_col col = YTR_COL_SET("x", YTR_COL_DOUBLE);
    ytr_data_desc dd;
    static double xs[2000];
    int i, n = 0, bad = 0, longest = 0;
    uint64_t s = 99;
    xs[n++] = 0.0;
    xs[n++] = 1.0;
    xs[n++] = -2.5;
    xs[n++] = 1.0 / 3.0;
    xs[n++] = 5e-324;                 /* the least subnormal */
    xs[n++] = 1.7976931348623157e308;
    while (n < (int)(sizeof(xs) / sizeof(xs[0]))) {
        uint64_t b;
        double x;
        s += 0x9E3779B97F4A7C15ULL;
        b = s;
        b = (b ^ (b >> 30)) * 0xBF58476D1CE4E5B9ULL;
        b = (b ^ (b >> 27)) * 0x94D049BB133111EBULL;
        b ^= b >> 31;
        memcpy(&x, &b, sizeof(x));
        if (x != x || x - x != 0.0) continue;
        xs[n++] = x;
    }
    memset(&dd, 0, sizeof(dd));
    dd.write = sink_write;
    dd.cols = &col;
    dd.n_cols = 1;
    sink_reset();
    CHECK(ytr_data_open(&g_w, &dd));
    for (i = 0; i < n; i++) {
        ytr_data_set_double(&g_w, 0, xs[i]);
        CHECK_I(ytr_data_write(&g_w, NULL), 0);
    }
    CHECK_I(ytr_data_finish(&g_w), 0);
    {
        /* The text of each row: the fewest digits, at most 17. */
        const char* p = strchr(g_sink, '\n') + 1;
        for (i = 0; i < n && p < g_sink + g_sink_len; i++) {
            const char* e = strchr(p, '\n');
            int digits = 0;
            const char* q;
            bool lead = true;
            for (q = p; q < e && *q != 'e'; q++) {
                if (*q >= '1' && *q <= '9') lead = false;
                if (!lead && *q >= '0' && *q <= '9') digits++;
            }
            if (digits > longest) longest = digits;
            p = e + 1;
        }
    }
    CHECK(longest <= 17);  /* significant digits */
    CHECK(read_back(g_sink, g_sink_len, false));
    if (g_back.base) {
        CHECK_I(g_back.n_rows, n);
        for (i = 0; i < n && i < g_back.n_rows; i++) {
            double y = ytb_num(&g_back, i, 0);
            if (memcmp(&y, &xs[i], sizeof(y)) != 0 && !(y == 0.0 && xs[i] == 0.0)) {
                if (bad++ < 5) fprintf(stderr, "trials_test: double %.17g read back as %.17g\n",
                                       xs[i], y);
            }
        }
        CHECK_I(bad, 0);
    }
    /* A comma locale must not reach the file: the writer puts '.' back.
     * Where no such locale is installed this checks the C locale only. */
    {
        char loc[64] = "none installed";
        const char* got = setlocale(LC_NUMERIC, "German");
        if (!got) got = setlocale(LC_NUMERIC, "de_DE.UTF-8");
        if (got) snprintf(loc, sizeof(loc), "%s", got);
        sink_reset();
        CHECK(ytr_data_open(&g_w, &dd));
        ytr_data_set_double(&g_w, 0, 0.5);
        ytr_data_write(&g_w, NULL);
        ytr_data_set_double(&g_w, 0, 1e-7);
        ytr_data_write(&g_w, NULL);
        ytr_data_close(&g_w);
        col.decimals = 2;
        CHECK(ytr_data_open(&g_w, &dd));
        ytr_data_set_double(&g_w, 0, 0.25);
        ytr_data_write(&g_w, NULL);
        ytr_data_close(&g_w);
        setlocale(LC_NUMERIC, "C");
        g_sink[g_sink_len] = '\0';
        CHECK_S(g_sink, "x\n0.5\n1e-07\nx\n0.25\n");
        printf("  data writer, doubles: %d read back bit for bit, at most %d significant digits; "
               "under a comma locale (%s) the point stays '.'\n", n, longest, loc);
    }
}

/* Random strings with commas, quotes, line breaks, UTF-8 and invalid bytes,
 * through the writer and back: each cell equal to the input, with every
 * invalid byte as U+FFFD. */
static void test_data_quoting(void) {
    static const char* const pieces[] = { ",", "\"", "\n", "\r", "\r\n", " ", "a", "Z", "0", "-1.5",
                                          "\xC3\xA9", "\xE2\x82\xAC", "\xF0\x9F\x98\x80",
                                          "\xFF", "\xC3", "\xED\xA0\x80", "\xE0\x80\xAF", "#",
                                          "\t", "''" };
    static char strs[400][96];
    static char want[400][300];
    ytr_col cols[2];
    ytr_data_desc dd;
    int i, n = 400, bad = 0;
    uint64_t s = 7;
    cols[0] = (ytr_col)YTR_COL_SET("id", YTR_COL_INT);
    cols[1] = (ytr_col)YTR_COL_SET("s", YTR_COL_STRING);
    for (i = 0; i < n; i++) {
        int k, np = (int)(ytr_splitmix(&s) * 12.0);
        size_t a = 0, b = 0;
        for (k = 0; k < np; k++) {
            int pi = (int)(ytr_splitmix(&s) * (double)(sizeof(pieces) / sizeof(pieces[0])));
            const char* pc = pieces[pi];
            size_t pl = strlen(pc), m;
            memcpy(strs[i] + a, pc, pl);
            a += pl;
            /* The expected text: invalid bytes one U+FFFD each. */
            for (m = 0; m < pl;) {
                size_t v = ytr__u8((const unsigned char*)pc + m, pl - m);
                if (pi >= 13 && pi <= 16) v = 0;  /* the four invalid pieces */
                if (v == 0) { memcpy(want[i] + b, "\xEF\xBF\xBD", 3); b += 3; m++; }
                else { memcpy(want[i] + b, pc + m, v); b += v; m += v; }
            }
        }
        strs[i][a] = '\0';
        want[i][b] = '\0';
    }
    memset(&dd, 0, sizeof(dd));
    dd.write = sink_write;
    dd.cols = cols;
    dd.n_cols = 2;
    sink_reset();
    CHECK(ytr_data_open(&g_w, &dd));
    for (i = 0; i < n; i++) {
        ytr_data_set_int(&g_w, 0, i);
        ytr_data_set_string(&g_w, 1, strs[i]);
        CHECK_I(ytr_data_write(&g_w, NULL), 0);
    }
    CHECK_I(ytr_data_finish(&g_w), 0);
    CHECK(read_back(g_sink, g_sink_len, true));
    if (g_back.base) {
        CHECK_I(g_back.n_rows, n);
        for (i = 0; i < n && i < g_back.n_rows; i++) {
            const char* got = ytb_text(&g_back, i, 1);
            CHECK_I(ytb_int(&g_back, i, 0), i);
            if (!got || strcmp(got, want[i]) != 0) {
                if (bad++ < 5) fail_s(__LINE__, "string read back", got ? got : "(null)", want[i]);
            }
        }
    }
}

/* A session's columns first, then the caller's: the session's part equals
 * ytr_format_row() without its newline. */
static void test_data_session(void) {
    static double levels[YTR_MAX_TRIALS];
    typedef struct srow { int64_t rt_ns; double level; } srow;
    static const ytr_col cols[] = { YTR_COL(srow, rt_ns, YTR_COL_TIME_NS),
                                    YTR_COL(srow, level, YTR_COL_DOUBLE) };
    static const char* csv = "ori,contrast\nleft,0.1\nright,0.2\n\"up, high\",0.4\n";
    ytr_data_desc dd;
    ytr_desc d;
    ytr_trial_info ti = ti_zero;
    srow r = { 0, 0.0 };
    char line[512];
    int i, rows = 0, rc;
    const char* p;

    if (!csv_table(&g_tab, csv)) { g_failures++; return; }
    zero_desc(&d);
    d.table = &g_tab;
    d.reps = 3;
    d.order = YTR_ORDER_FULL_RANDOM;
    d.n_practice = 2;
    d.rng = ytr_splitmix;
    d.rng_ctx = &g_rng_state;
    d.jitters[0] = ytr_uniform("iti", YRT_MS(500), YRT_S(1));
    d.n_jitters = 1;
    d.records = levels;
    d.record_size = sizeof(double);
    g_rng_state = 5;
    CHECK(ytr_open(&g_t, &d));

    memset(&dd, 0, sizeof(dd));
    dd.write = sink_write;
    dd.trials = &g_t;
    dd.cols = cols;
    dd.n_cols = 2;
    sink_reset();
    CHECK(ytr_data_open(&g_w, &dd));
    g_sink[g_sink_len] = '\0';
    CHECK_S(g_sink, "index,block,rep,condition,track,practice,warmup,requeued,after_break,outcome,"
                    "ori,contrast,iti,rt_s,level\n");
    CHECK_I(ytr_data_write(&g_w, &r), YTR_ERR_ORDER);   /* nothing completed */
    CHECK_HAS(ytr_data_error(&g_w), "a trial is pending, or its row is written already");
    i = 0;
    while (ytr_next(&g_t, &ti) >= 0) {
        CHECK_I(ytr_data_write(&g_w, &r), YTR_ERR_ORDER);  /* pending, not done */
        if (i == 4) {
            ytr_requeue(&g_t);      /* a re-queued trial is a row too */
        } else {
            ytr_update(&g_t, i % 2, NULL);
        }
        if (i == 6) { i++; continue; }  /* a trial with no row: allowed */
        r.rt_ns = 300000000 + (int64_t)i * 1000;
        r.level = ytb_num(&g_tab, ti.condition, 1);
        rc = ytr_data_write(&g_w, &r);
        CHECK_I(rc, 0);
        CHECK_I(ytr_data_write(&g_w, &r), YTR_ERR_ORDER);  /* the same trial twice */
        rows++;
        i++;
    }
    CHECK_I(ytr_data_finish(&g_w), 0);
    CHECK_I(rows, ytr_n_run(&g_t) - 1);

    /* Each row: the session's line, then the caller's fields. */
    p = strchr(g_sink, '\n') + 1;
    for (i = 0; i < ytr_n_run(&g_t); i++) {
        size_t n;
        char tail[128];
        if (i == 6) continue;
        n = (size_t)ytr_format_row(&g_t, i, line, sizeof(line)) - 1u;
        if (strncmp(p, line, n) != 0) { fail_s(__LINE__, "session columns", p, line); break; }
        snprintf(tail, sizeof(tail), ",0.%09lld,", (long long)(300000000 + (int64_t)i * 1000));
        if (strncmp(p + n, tail, strlen(tail)) != 0) { fail_s(__LINE__, "caller columns", p + n, tail); break; }
        p = strchr(p + n + strlen(tail), '\n') + 1;
    }
    CHECK(read_back(g_sink, g_sink_len, false));
    if (g_back.base) {
        CHECK_I(g_back.n_rows, rows);
        CHECK_I(g_back.n_cols, 15);
        CHECK_S(ytb_col_name(&g_back, 12), "iti");
        /* The quoted table text came back whole. */
        CHECK(ytb_find(&g_back, ytb_col(&g_back, "ori"), "up, high") >= 0);
    }

    /* A writer opened mid-session writes only what completes after it. */
    sink_reset();
    CHECK(ytr_data_open(&g_w, &dd));
    CHECK_I(ytr_data_write(&g_w, &r), YTR_ERR_ORDER);
    ytr_data_close(&g_w);
}

static void test_data_refusals(void) {
    typedef struct brow { int i; double x; char c[4]; } brow;
    ytr_data_desc dd;
    ytr_col c[3];
    ytr_desc d;
    int i;

    memset(&dd, 0, sizeof(dd));
    {
        static ytr_data z;                  /* zeroed: close does nothing */
        ytr_data_close(&z);
        ytr_data_close(NULL);
        CHECK_I(ytr_data_finish(NULL), YTR_ERR_ARG);
    }
    CHECK(!ytr_data_open(NULL, &dd));
    CHECK(!ytr_data_open(&g_w, NULL));
    CHECK_HAS(ytr_data_error(&g_w), "null desc");
    CHECK(!ytr_data_open(&g_w, &dd));
    CHECK_HAS(ytr_data_error(&g_w), "no path and no write function");
    dd.write = sink_write;
    CHECK(!ytr_data_open(&g_w, &dd));
    CHECK_HAS(ytr_data_error(&g_w), "no columns");
    ytr_data_close(&g_w);                   /* after a failed open */
    CHECK_HAS(ytr_data_error(&g_w), "no columns");

    dd.cols = c;
    dd.n_cols = 1;
    c[0] = (ytr_col)YTR_COL_SET("2bad", YTR_COL_INT);
    CHECK(!ytr_data_open(&g_w, &dd));
    CHECK_HAS(ytr_data_error(&g_w), "'2bad' is not a name");
    c[0] = (ytr_col)YTR_COL_SET("a b", YTR_COL_INT);
    CHECK(!ytr_data_open(&g_w, &dd));
    c[0] = (ytr_col)YTR_COL_SET("ok", YTR_COL_INT);
    c[0].type = (ytr_col_type)0;
    CHECK(!ytr_data_open(&g_w, &dd));
    CHECK_HAS(ytr_data_error(&g_w), "not a ytr_col_type");
    c[0] = (ytr_col)YTR_COL(brow, i, YTR_COL_DOUBLE);   /* an int field as DOUBLE */
    CHECK(!ytr_data_open(&g_w, &dd));
    CHECK_HAS(ytr_data_error(&g_w), "'i' is DOUBLE but its field is 4 bytes");
    c[0] = (ytr_col)YTR_COL(brow, x, YTR_COL_TIME_NS);
    c[0].decimals = 10;
    CHECK(!ytr_data_open(&g_w, &dd));
    CHECK_HAS(ytr_data_error(&g_w), "decimals 10");
    c[0] = (ytr_col)YTR_COL(brow, x, YTR_COL_DOUBLE);
    c[0].decimals = 18;
    CHECK(!ytr_data_open(&g_w, &dd));
    c[0] = (ytr_col)YTR_COL(brow, i, YTR_COL_INT);
    c[0].decimals = 1;
    CHECK(!ytr_data_open(&g_w, &dd));
    c[0] = (ytr_col)YTR_COL(brow, c, YTR_COL_CHARS);
    c[0].size = 0;
    CHECK(!ytr_data_open(&g_w, &dd));
    CHECK_HAS(ytr_data_error(&g_w), "CHARS needs the array's size");
    c[0] = (ytr_col)YTR_COL_TEXT("p", NULL);
    CHECK(!ytr_data_open(&g_w, &dd));
    CHECK_HAS(ytr_data_error(&g_w), "CONST without text");
    c[0] = (ytr_col)YTR_COL_SET("a", YTR_COL_INT);
    c[1] = (ytr_col)YTR_COL_SET("a", YTR_COL_DOUBLE);
    dd.n_cols = 2;
    CHECK(!ytr_data_open(&g_w, &dd));
    CHECK_HAS(ytr_data_error(&g_w), "two columns are named 'a'");
    c[0] = (ytr_col)YTR_COL_SET("t_s", YTR_COL_TIME_NS);
    c[1] = (ytr_col)YTR_COL_SET("t_ns", YTR_COL_TIME_NS);
    CHECK(!ytr_data_open(&g_w, &dd));
    CHECK_HAS(ytr_data_error(&g_w), "two columns are named 't_s'");
    dd.n_cols = YTR_DATA_MAX_COLS + 1;
    CHECK(!ytr_data_open(&g_w, &dd));
    dd.n_cols = 1;
    dd.flush = (ytr_flush)7;
    CHECK(!ytr_data_open(&g_w, &dd));
    CHECK_HAS(ytr_data_error(&g_w), "flush 7");
    dd.flush = YTR_FLUSH_ROW;

    /* A session whose names would not read back, or collide. */
    zero_desc(&d);
    d.factors[0] = ytr_factor("a,\"b\"", 2);
    d.n_factors = 1;
    d.reps = 1;
    CHECK(ytr_open(&g_t, &d));
    dd.trials = &g_t;
    CHECK(!ytr_data_open(&g_w, &dd));
    CHECK_HAS(ytr_data_error(&g_w), "the session's column");
    d.factors[0] = ytr_factor("ori", 2);
    CHECK(ytr_open(&g_t, &d));
    c[0] = (ytr_col)YTR_COL_SET("outcome", YTR_COL_INT);
    CHECK(!ytr_data_open(&g_w, &dd));
    CHECK_HAS(ytr_data_error(&g_w), "two columns are named 'outcome'");
    g_t.open = false;
    CHECK(!ytr_data_open(&g_w, &dd));
    CHECK_HAS(ytr_data_error(&g_w), "the session is not open");
    dd.trials = NULL;

    /* The setters: the type and the column must match a SET column. */
    c[0] = (ytr_col)YTR_COL(brow, i, YTR_COL_INT);
    c[1] = (ytr_col)YTR_COL_SET("x", YTR_COL_DOUBLE);
    dd.n_cols = 2;
    sink_reset();
    CHECK(ytr_data_open(&g_w, &dd));
    CHECK_I(ytr_data_set_int(&g_w, 0, 1), YTR_ERR_ARG);     /* bound to the struct */
    CHECK_HAS(ytr_data_error(&g_w), "ytr_data_set_int: column 'i' is not a YTR_COL_SET column");
    CHECK_I(ytr_data_set_int(&g_w, 1, 1), YTR_ERR_ARG);     /* a DOUBLE column */
    CHECK_I(ytr_data_set_double(&g_w, 2, 1.0), YTR_ERR_ARG);
    CHECK_HAS(ytr_data_error(&g_w), "ytr_data_set_double: column 2 is not in desc.cols");
    CHECK_I(ytr_data_set_double(&g_w, -1, 1.0), YTR_ERR_ARG);
    CHECK_I(ytr_data_set_bool(&g_w, 1, true), YTR_ERR_ARG);
    CHECK_I(ytr_data_set_int64(&g_w, 1, 1), YTR_ERR_ARG);
    CHECK_I(ytr_data_set_time_ns(&g_w, 1, 1), YTR_ERR_ARG);
    CHECK_I(ytr_data_set_string(&g_w, 1, "s"), YTR_ERR_ARG);
    CHECK_I(ytr_data_set_double(&g_w, 1, 2.5), 0);
    CHECK_I(ytr_data_write(&g_w, NULL), YTR_ERR_ARG);       /* a bound column, no row */
    CHECK_HAS(ytr_data_error(&g_w), "ytr_data_write: invalid argument");
    CHECK_I(ytr_data_write(NULL, NULL), YTR_ERR_ARG);
    ytr_data_close(&g_w);
    CHECK_I(ytr_data_set_double(&g_w, 1, 2.5), YTR_ERR_CLOSED);
    CHECK_HAS(ytr_data_error(&g_w), "ytr_data_set_double: the writer is not open");
    CHECK_I(ytr_data_col(&g_w, "x"), -1);
    CHECK_I((long)ytr_data_rows(&g_w), 0);

    /* 64 columns open; 65 with a session do not. */
    {
        static char nm[YTR_DATA_MAX_COLS][8];
        static ytr_col many[YTR_DATA_MAX_COLS];
        for (i = 0; i < YTR_DATA_MAX_COLS; i++) {
            snprintf(nm[i], sizeof(nm[i]), "c%d", i);
            many[i] = (ytr_col)YTR_COL_SET(nm[i], YTR_COL_INT);
        }
        dd.cols = many;
        dd.n_cols = YTR_DATA_MAX_COLS;
        CHECK(ytr_data_open(&g_w, &dd));
        ytr_data_close(&g_w);
        d.factors[0] = ytr_factor("ori", 2);
        CHECK(ytr_open(&g_t, &d));
        dd.trials = &g_t;
        CHECK(!ytr_data_open(&g_w, &dd));
        CHECK_HAS(ytr_data_error(&g_w), "more than 64 columns");
    }
}

/* A failing write: the row is refused, every later one too, and finish()
 * says so; nothing is written after the failure. */
static void test_data_io_error(void) {
    ytr_col c = YTR_COL_SET("v", YTR_COL_INT);
    ytr_data_desc dd;
    memset(&dd, 0, sizeof(dd));
    dd.write = sink_write;
    dd.cols = &c;
    dd.n_cols = 1;
    sink_reset();
    g_sink_fail_after = 0;
    CHECK(!ytr_data_open(&g_w, &dd));
    CHECK_HAS(ytr_data_error(&g_w), "cannot write the header");
    sink_reset();
    g_sink_fail_after = 3;              /* the header and two rows */
    CHECK(ytr_data_open(&g_w, &dd));
    ytr_data_set_int(&g_w, 0, 1);
    CHECK_I(ytr_data_write(&g_w, NULL), 0);
    CHECK_I(ytr_data_write(&g_w, NULL), 0);
    CHECK_I(ytr_data_write(&g_w, NULL), YTR_ERR_IO);
    CHECK_HAS(ytr_data_error(&g_w), "the write failed after 2 rows");
    CHECK_I(ytr_data_write(&g_w, NULL), YTR_ERR_IO);
    CHECK_I((long)ytr_data_rows(&g_w), 2);
    CHECK_I(ytr_data_finish(&g_w), YTR_ERR_IO);
    CHECK_HAS(ytr_data_error(&g_w), "the write failed after 2 rows");
    g_sink[g_sink_len] = '\0';
    CHECK_S(g_sink, "v\n1\n\n");
    CHECK_S(ytr_strerror(YTR_ERR_IO), "data file write failed");
}

/* A text longer than the row buffer goes out in pieces, whole. */
static void test_data_long_row(void) {
    static char big[3 * YTR_DATA_BUF];
    ytr_col c[2];
    ytr_data_desc dd;
    size_t k;
    c[0] = (ytr_col)YTR_COL_SET("a", YTR_COL_STRING);
    c[1] = (ytr_col)YTR_COL_SET("b", YTR_COL_INT);
    for (k = 0; k + 1 < sizeof(big); k++) big[k] = (char)('a' + (int)(k % 26));
    big[k] = '\0';
    memset(&dd, 0, sizeof(dd));
    dd.write = sink_write;
    dd.cols = c;
    dd.n_cols = 2;
    sink_reset();
    CHECK(ytr_data_open(&g_w, &dd));
    ytr_data_set_string(&g_w, 0, big);
    ytr_data_set_int(&g_w, 1, 9);
    CHECK_I(ytr_data_write(&g_w, NULL), 0);
    CHECK(g_sink_calls >= 4);
    CHECK_I(ytr_data_finish(&g_w), 0);
    CHECK_I((long)g_sink_len, (long)(4 + strlen(big) + 3));
    CHECK(memcmp(g_sink + 4, big, strlen(big)) == 0);
    CHECK(memcmp(g_sink + 4 + strlen(big), ",9\n", 3) == 0);
}

static int count_lines(const char* text, size_t n) {
    size_t i;
    int k = 0;
    for (i = 0; i < n; i++) k += text[i] == '\n';
    return k;
}

/* Read a whole file; on Windows by its UTF-16 name. */
static size_t slurp(const char* path, const wchar_t* wpath, char* out, size_t cap) {
    FILE* f = NULL;
    size_t n;
#ifdef _WIN32
    (void)path;
    /* Shared, as a monitor would open it while the session writes. */
    f = _wfsopen(wpath, L"rb", _SH_DENYNO);
#else
    (void)wpath;
    f = fopen(path, "rb");
#endif
    if (!f) return 0;
    n = fread(out, 1, cap - 1, f);
    fclose(f);
    out[n] = '\0';
    return n;
}

/* A real file and its sidecar, with a UTF-8 name, in both flush modes. */
static void test_data_file(void) {
    static char text[1 << 16];
    static const char* const path = "trials_test_data_\xC3\xA9.csv";
    static const wchar_t* const wpath = L"trials_test_data_\x00e9" L".csv";
    static const char* const mpath = "trials_test_data_\xC3\xA9.meta";
    static const wchar_t* const wmpath = L"trials_test_data_\x00e9" L".meta";
    ytr_col c[2];
    ytr_data_desc dd;
    ytr_desc d;
    ytr_trial_info ti = ti_zero;
    size_t n;
    int mode;
    c[0] = (ytr_col)YTR_COL_SET("rt_ns", YTR_COL_TIME_NS);
    c[1] = (ytr_col)YTR_COL_TEXT("session", "s1");
    for (mode = 0; mode < 2; mode++) {
        int k = 0;
        zero_desc(&d);
        d.n_conditions = 2;
        d.reps = 2;
        CHECK(ytr_open(&g_t, &d));
        memset(&dd, 0, sizeof(dd));
        dd.path = path;
        dd.trials = &g_t;
        dd.cols = c;
        dd.n_cols = 2;
        dd.flush = mode ? YTR_FLUSH_SYNC : YTR_FLUSH_ROW;
        dd.meta_path = mpath;
        dd.meta = "participant=P07\nrig=test";
        CHECK(ytr_data_open(&g_w, &dd));
        while (ytr_next(&g_t, &ti) >= 0) {
            ytr_update(&g_t, 1, NULL);
            ytr_data_set_time_ns(&g_w, 0, 250000000 + k);
            CHECK_I(ytr_data_write(&g_w, NULL), 0);
            /* The row is in the file before close: a crash keeps it. */
            n = slurp(path, wpath, text, sizeof(text));
            CHECK_I(count_lines(text, n), k + 2);
            k++;
        }
        CHECK_I(ytr_data_finish(&g_w), 0);
        n = slurp(path, wpath, text, sizeof(text));
        CHECK(read_back(text, n, false));
        if (g_back.base) {
            CHECK_I(g_back.n_rows, 4);
            CHECK(ytb_num(&g_back, 3, ytb_col(&g_back, "rt_s")) == 0.250000003);
            CHECK_S(ytb_text(&g_back, 0, ytb_col(&g_back, "session")), "s1");
        }
        CHECK(strchr(text, '\r') == NULL);   /* LF only */
        n = slurp(mpath, wmpath, text, sizeof(text));
        {
            static const char* const head = "ysp_trials=" YTR_VERSION_STRING
                " data=trials_test_data_\xC3\xA9.csv\nparticipant=P07\nrig=test\nysp_trials=";
            static const char* const tail = "\nrows=4 closed=1\n";
            CHECK(strncmp(text, head, strlen(head)) == 0);
            CHECK(n > strlen(tail) && strcmp(text + n - strlen(tail), tail) == 0);
        }
    }
#ifdef _WIN32
    _wremove(wpath);
    _wremove(wmpath);
#else
    remove(path);
    remove(mpath);
#endif
    memset(&dd, 0, sizeof(dd));
    dd.path = "no_such_directory_here/x.csv";
    dd.cols = c;
    dd.n_cols = 1;
    CHECK(!ytr_data_open(&g_w, &dd));
    CHECK_HAS(ytr_data_error(&g_w), "cannot create 'no_such_directory_here/x.csv'");
}

static void test_data(void) {
    test_data_formats();
    test_data_time();
    test_data_doubles();
    test_data_quoting();
    test_data_session();
    test_data_refusals();
    test_data_io_error();
    test_data_long_row();
    test_data_file();
    printf("  data writer: every type and missing value, times at 1 to 9 decimals, 400 random "
           "strings (commas, quotes, CR, LF, UTF-8, invalid bytes) read back by ytb_open_csv; a "
           "session's rows equal format_row; 34 refusals; a failing write; a UTF-8 path and "
           "its sidecar; sizeof(ytr_data) = %lu\n", (unsigned long)sizeof(ytr_data));
}
