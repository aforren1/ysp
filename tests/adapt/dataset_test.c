/* dataset_test.c - self-checking test for ysp/dataset.h. No framework: it
 * returns 0 when every check passed and 1 after printing each failure.
 *
 *     gcc -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Werror -Iinclude \
 *         -o dataset_test tests/adapt/dataset_test.c -lm
 *
 * It writes under ./dataset_test_work (the working folder) and removes it
 * at the start and at the end. Checks:
 *   - a small synthetic session against golden bytes (dataset_golden.h):
 *     session.json at the open and at the finish, every CSV, the zip's
 *     digest; every CSV read back with ysp/table.h; the zip parsed entry
 *     by entry (names, sizes, CRC-32, offsets), to a file and to memory;
 *   - a crash: a child process (this program with --crash-child) writes
 *     rows and leaves with _Exit(), no finish; every file reads, session.json
 *     says "closed": false; the same while a data set is open in-process;
 *   - refusals at the open (each before the folder exists), at the writes,
 *     the streams and the session keys, each by code and message;
 *   - the atomic rewrite: a blocked temporary file, and on Windows a
 *     session.json held without FILE_SHARE_DELETE: the old file stays;
 *   - a write failure recorded in write_errors; YTR_FLUSH_SYNC; a UTF-8
 *     folder name; a trailing separator.
 * With --golden it prints the files of the synthetic session as C strings
 * (to update dataset_golden.h after a deliberate change; review the diff).
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "ysp/rt.h"
#include "ysp/input.h"
#include "ysp/timeline.h"
#include "ysp/screen.h"
#define YSP_DATASET_IMPLEMENTATION
#include "ysp/dataset.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dirent.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "dataset_golden.h"

/* ---------------------------------------------------------------- harness */

static int g_failures = 0;
static int g_checks = 0;

static void fail_s(int line, const char* what, const char* got, const char* want) {
    fprintf(stderr, "dataset_test: FAIL at line %d: %s\n  got:  [%s]\n  want: [%s]\n", line, what,
            got ? got : "(null)", want ? want : "(null)");
    g_failures++;
}

#define CHECK(cond) do { g_checks++; if (!(cond)) { \
    fprintf(stderr, "dataset_test: FAIL at line %d: %s\n", __LINE__, #cond); g_failures++; } } while (0)
#define CHECK_I(got, want) do { long long g_ = (long long)(got), w_ = (long long)(want); g_checks++; \
    if (g_ != w_) { fprintf(stderr, "dataset_test: FAIL at line %d: %s (got %lld, want %lld)\n", \
                            __LINE__, #got, g_, w_); g_failures++; } } while (0)
#define CHECK_S(got, want) do { const char* g_ = (got); const char* w_ = (want); g_checks++; \
    if (!g_ || !w_ || strcmp(g_, w_) != 0) fail_s(__LINE__, #got, g_, w_); } while (0)
#define CHECK_HAS(str, sub) do { const char* s_ = (str); g_checks++; \
    if (!s_ || !strstr(s_, (sub))) fail_s(__LINE__, "message lacks a substring", s_, (sub)); } while (0)

#define WORK "dataset_test_work"

/* ------------------------------------------------------------ file helpers */

#if defined(_WIN32)
static bool wide(const char* s, wchar_t* w, int cap) {
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, w, cap) > 0;
}
#endif

static FILE* xopen(const char* path, const char* mode) {
#if defined(_WIN32)
    wchar_t w[1100], m[8];
    int i;
    if (!wide(path, w, 1100)) return NULL;
    for (i = 0; mode[i] && i < 7; i++) m[i] = (wchar_t)mode[i];
    m[i] = 0;
    return _wfopen(w, m);
#else
    return fopen(path, mode);
#endif
}

/* The whole file, NUL-terminated, in a static buffer per slot; NULL when
 * it cannot be read. */
static char g_files[4][1 << 18];
static size_t g_file_len[4];
static const char* read_file(int slot, const char* path) {
    FILE* f = xopen(path, "rb");
    size_t n;
    if (!f) return NULL;
    n = fread(g_files[slot], 1, sizeof g_files[slot] - 1, f);
    fclose(f);
    g_files[slot][n] = '\0';
    g_file_len[slot] = n;
    return g_files[slot];
}

static bool exists(const char* path) {
#if defined(_WIN32)
    wchar_t w[1100];
    return wide(path, w, 1100) && GetFileAttributesW(w) != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st;
    return stat(path, &st) == 0;
#endif
}

static void make_dir(const char* path) {
#if defined(_WIN32)
    wchar_t w[1100];
    if (wide(path, w, 1100)) CreateDirectoryW(w, NULL);
#else
    mkdir(path, 0777);
#endif
}

/* Removes a folder and everything in it. */
static void rm_tree(const char* path) {
#if defined(_WIN32)
    wchar_t w[1100], pat[1100];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    char sub[1100];
    if (!wide(path, w, 1100)) return;
    swprintf(pat, 1100, L"%ls\\*", w);
    h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            char nm[600];
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            if (WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, nm, sizeof nm, NULL, NULL) <= 0) continue;
            snprintf(sub, sizeof sub, "%s/%s", path, nm);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) rm_tree(sub);
            else {
                wchar_t ws[1100];
                if (wide(sub, ws, 1100)) DeleteFileW(ws);
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(w);
#else
    DIR* d = opendir(path);
    struct dirent* e;
    char sub[1100];
    if (d) {
        while ((e = readdir(d)) != NULL) {
            struct stat st;
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
            snprintf(sub, sizeof sub, "%s/%s", path, e->d_name);
            if (stat(sub, &st) == 0 && S_ISDIR(st.st_mode)) rm_tree(sub);
            else remove(sub);
        }
        closedir(d);
    }
    rmdir(path);
#endif
}

static uint64_t fnv1a(const void* p, size_t n) {
    const unsigned char* b = (const unsigned char*)p;
    uint64_t h = 0xcbf29ce484222325ULL;
    size_t i;
    for (i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001b3ULL; }
    return h;
}

static uint32_t crc32_ref(const void* p, size_t n) {
    const unsigned char* b = (const unsigned char*)p;
    uint32_t c = 0xFFFFFFFFu;
    size_t i;
    int k;
    for (i = 0; i < n; i++) {
        c ^= b[i];
        for (k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

/* A CSV through ysp/table.h, every numeric empty cell as NaN. */
static ytb_table g_tab;
static uint64_t g_tab_arena[(1 << 20) / 8];
static bool read_csv(const char* text, size_t n) {
    static ytb_csv_desc cd;
    memset(&cd, 0, sizeof cd);
    cd.text = text;
    cd.len = n;
    cd.allow_empty = true;
    cd.arena = g_tab_arena;
    cd.arena_size = sizeof g_tab_arena;
    if (!ytb_open_csv(&g_tab, &cd)) {
        fprintf(stderr, "dataset_test: ysp/table.h refused a file: %s\n", ytb_error(&g_tab));
        return false;
    }
    return true;
}

/* ------------------------------------------------------ the golden session */

typedef struct trow {
    int         trial_index;
    const char* response;
    bool        correct;
    int64_t     rt_ns;
    double      level;
} trow;

static const ytr_col g_trial_cols[] = {
    YTR_COL_TEXT("participant", "P07"),
    YTR_COL(trow, trial_index, YTR_COL_INT),
    YTR_COL(trow, response, YTR_COL_STRING),
    YTR_COL(trow, correct, YTR_COL_BOOL),
    YTR_COL(trow, rt_ns, YTR_COL_TIME_NS),
    YTR_COL(trow, level, YTR_COL_DOUBLE),
    YTR_COL_SET("note", YTR_COL_STRING),
};
#define N_TRIAL_COLS ((int)(sizeof g_trial_cols / sizeof g_trial_cols[0]))

typedef struct dots_row {
    int64_t t_ns;
    int64_t seed;
    int     updates;
} dots_row;

static const ytr_col g_dots_cols[] = {
    YTR_COL(dots_row, t_ns, YTR_COL_TIME_NS),
    YTR_COL(dots_row, seed, YTR_COL_INT64),
    YTR_COL(dots_row, updates, YTR_COL_INT),
};

static yjs_arena g_a;

/* The player's keys, as the test gives them. */
static yjs_value* session_value(bool with_start) {
    yjs_value *s = yjs_new(&g_a, YJS_OBJECT), *dev = yjs_new(&g_a, YJS_ARRAY), *d = yjs_new(&g_a, YJS_OBJECT);
    yjs_value* fields = yjs_new(&g_a, YJS_OBJECT);
    yjs_set(&g_a, s, "experiment", yjs_new_stringz(&g_a, "sha256:00ff"));
    yjs_set(&g_a, s, "seed", yjs_new_int(&g_a, 20261021));
    yjs_set(&g_a, s, "refresh_hz", yjs_new_double(&g_a, 59.94));
    yjs_set(&g_a, s, "onset_offset_s", yjs_new_fixed(&g_a, 1500000, 9));
    yjs_set(&g_a, fields, "participant", yjs_new_int(&g_a, 7));
    yjs_set(&g_a, s, "fields", fields);
    yjs_set(&g_a, d, "role", yjs_new_stringz(&g_a, "resp"));
    yjs_set(&g_a, d, "family", yjs_new_stringz(&g_a, "keyboard"));
    yjs_push(&g_a, dev, d);
    yjs_set(&g_a, s, "devices", dev);
    if (with_start) yjs_set(&g_a, s, "start_utc", yjs_new_stringz(&g_a, "2026-10-21T12:00:00Z"));
    return s;
}

static bool open_set(yds_dataset* ds, const char* dir, bool with_start, ytr_flush flush) {
    yds_desc d;
    memset(&d, 0, sizeof d);
    d.dir = dir;
    d.session = session_value(with_start);
    d.trial_cols = g_trial_cols;
    d.n_trial_cols = N_TRIAL_COLS;
    d.flush = flush;
    return yds_open(ds, &d);
}

/* The rows of the synthetic session. */
static void write_rows(yds_dataset* ds, int* dots_id, int n_trials) {
    trow t;
    yds_frame f;
    yds_event e;
    yds_clock c;
    yscr_record r;
    yin_event in;
    ytl_event te;
    yrt_fit_info fi;
    dots_row dr;
    int i;

    for (i = 0; i < n_trials; i++) {
        t.trial_index = i;
        t.response = i == 1 ? NULL : (i == 0 ? "left" : "right, \"quoted\"");
        t.correct = i != 1;
        t.rt_ns = i == 1 ? YTR_MISSING_NS : 412345678 + (int64_t)i * 1000;
        t.level = i == 2 ? 0.1 : 0.05 * (i + 1);
        if (i == 1) ytr_data_set_string(yds_trials_writer(ds), ytr_data_col(yds_trials_writer(ds), "note"), "no response");
        CHECK_I(yds_write_trial(ds, &t), 0);
    }

    memset(&r, 0, sizeof r);
    for (i = 0; i < 4; i++) {
        r.index = 100 + i;
        r.target = 5000000000LL + (int64_t)i * 16666667;
        r.planned = r.target + 120;
        r.onset = i == 3 ? 0 : r.planned + 35000;     /* frame 103 never shown */
        r.residual = r.onset - r.target;
        r.dropped = (uint32_t)(i == 2);
        r.path = 1;
        r.tier = (uint8_t)(i == 2 ? 3 : 2);
        r.flags = (uint16_t)(i == 2 ? 0x4 : 0);
        r.phase_ns[YSCR_PHASE_EVALUATE] = 12000;
        r.phase_ns[YSCR_PHASE_SCRIPT] = 0;
        r.phase_ns[YSCR_PHASE_DRAW] = 250000 + (uint32_t)i;
        r.phase_ns[YSCR_PHASE_UPLOAD] = 0;
        r.phase_ns[YSCR_PHASE_SWAP] = 900000;
        r.phase_ns[YSCR_PHASE_GPU] = YSCR_PHASE_UNKNOWN;
        yds_frame_from_screen(&f, &r, i < 2 ? 0 : 1);
        CHECK_I(yds_write_frame(ds, &f), 0);
    }

    memset(&te, 0, sizeof te);
    te.kind = (uint8_t)YTL_ONSET;
    te.target = 2;
    te.code = 0;
    te.frame = 101;
    te.onset = 5016701787LL;
    te.residual = 35000;
    te.flags = YTL_EV_FIRED;
    yds_event_from_timeline(&e, &te, "gabor", 0);
    e.planned_ns = 5016666787LL;
    CHECK_I(yds_write_event(ds, &e), 0);

    memset(&in, 0, sizeof in);
    in.t = 5420000000LL;
    in.kind = (uint8_t)YIN_KIND_KEYBOARD;
    in.type = (uint8_t)YIN_PRESS;
    in.control = 4;
    in.code = 97;
    in.stamp = 2;
    in.flags = (uint8_t)YIN_DEVTICKS;
    in.ticks = 123456u;
    in.value = 1.0f;
    yds_event_from_input(&e, &in, "resp", 0);
    CHECK_I(yds_write_event(ds, &e), 0);

    yds_event_init(&e);
    e.t_ns = 5420000000LL;
    e.trial_index = 0;
    e.kind = "response";
    e.role = "resp";
    e.text = "left";
    CHECK_I(yds_write_event(ds, &e), 0);

    yds_event_init(&e);
    e.t_ns = 5016670000LL;
    e.trial_index = 0;
    e.kind = "out";
    e.role = "trig";
    e.code = 12;
    e.deadline_ns = 5016666787LL;
    e.end_ns = 5016690000LL;
    e.residual_ns = e.t_ns - e.deadline_ns;
    e.flags = 0;
    CHECK_I(yds_write_event(ds, &e), 0);

    yds_event_init(&e);
    e.t_ns = 5500000000LL;
    e.kind = "visibility";
    e.text = "hidden, then \"visible\"\nagain";
    CHECK_I(yds_write_event(ds, &e), 0);

    yds_clock_init(&c);
    c.role = "photo";
    c.kind = "pair";
    c.t_ns = 4000000000LL;
    c.ticks = 1000;
    c.width_ns = 41000;
    CHECK_I(yds_write_clock(ds, &c), 0);
    memset(&fi, 0, sizeof fi);
    fi.t0_ns = 4000000000LL;
    fi.ticks0 = 1000;
    fi.ns_per_tick = 1000.0002;
    fi.ppm = 0.2;
    fi.spread_ns = 15000;
    fi.width_ns = 41000;
    fi.span_ns = 5000000000LL;
    fi.points = 25;
    fi.epoch = 0;
    yds_clock_from_fit(&c, &fi, "photo");
    CHECK_I(yds_write_clock(ds, &c), 0);

    *dots_id = yds_stream(ds, "dots", g_dots_cols, 3);
    CHECK_I(*dots_id, 0);
    for (i = 0; i < 3; i++) {
        dr.t_ns = 5016701787LL + (int64_t)i * 16666667;
        dr.seed = 77;
        dr.updates = i;
        CHECK_I(yds_write_stream(ds, *dots_id, &dr), 0);
    }
}

static void session_changes(yds_dataset* ds) {
    yjs_value* ch = yjs_new(&g_a, YJS_OBJECT);
    yjs_value* st = yjs_new(&g_a, YJS_OBJECT);
    yjs_set(&g_a, ch, "at_trial", yjs_new_int(&g_a, 2));
    yjs_set(&g_a, ch, "experiment", yjs_new_stringz(&g_a, "sha256:0100"));
    CHECK_I(yds_session_push(ds, "changes", ch), 0);
    yjs_set(&g_a, st, "fullscreen", yjs_new(&g_a, YJS_TRUE));
    CHECK_I(yds_session_set(ds, "self_test", st), 0);
}

/* ------------------------------------------------------------------ zip */

static uint16_t rd16(const unsigned char* p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const unsigned char* p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

typedef struct membuf { unsigned char* p; size_t n, cap; bool fail_after; size_t limit; } membuf;
static bool mem_write(void* ctx, const void* bytes, size_t n) {
    membuf* m = (membuf*)ctx;
    if (m->fail_after && m->n + n > m->limit) return false;
    if (m->n + n > m->cap) return false;
    memcpy(m->p + m->n, bytes, n);
    m->n += n;
    return true;
}

/* Every entry: local header, name, data; the central directory; the end
 * record. names[i] and texts[i] give what each entry must hold (text NULL
 * for a folder). */
static void check_zip(const unsigned char* z, size_t n, const char* const* names, const char* const* texts, int count) {
    const unsigned char* eocd;
    uint32_t cd_off, cd_size;
    size_t at;
    int i;
    CHECK(n >= 22);
    if (n < 22) return;
    eocd = z + n - 22;
    CHECK_I(rd32(eocd), 0x06054B50u);
    CHECK_I(rd16(eocd + 8), count);
    CHECK_I(rd16(eocd + 10), count);
    cd_size = rd32(eocd + 12);
    cd_off = rd32(eocd + 16);
    CHECK_I((size_t)cd_off + cd_size, n - 22);
    at = cd_off;
    for (i = 0; i < count && at + 46 <= n; i++) {
        const unsigned char* c = z + at;
        uint32_t nl = rd16(c + 28), lho = rd32(c + 42), crc = rd32(c + 16), sz = rd32(c + 20);
        size_t tl = texts[i] ? strlen(texts[i]) : 0;
        const unsigned char* l = z + lho;
        CHECK_I(rd32(c), 0x02014B50u);
        CHECK_I(rd16(c + 8), 0x0800);       /* UTF-8 names */
        CHECK_I(rd16(c + 10), 0);           /* stored */
        CHECK_I(rd16(c + 14), 0x21);        /* 1980-01-01 */
        CHECK_I(nl, strlen(names[i]));
        CHECK(memcmp(c + 46, names[i], nl) == 0);
        CHECK_I(sz, tl);
        CHECK_I(rd32(c + 24), tl);
        CHECK_I(crc, texts[i] ? crc32_ref(texts[i], tl) : 0);
        CHECK_I(rd32(c + 38) >> 16, texts[i] ? 0100644 : 040755);
        CHECK(lho + 30 + nl + sz <= cd_off);
        CHECK_I(rd32(l), 0x04034B50u);
        CHECK_I(rd32(l + 14), crc);
        CHECK_I(rd32(l + 18), sz);
        CHECK_I(rd16(l + 26), nl);
        CHECK_I(rd16(l + 28), 0);
        CHECK(memcmp(l + 30, names[i], nl) == 0);
        if (texts[i]) CHECK(memcmp(l + 30 + nl, texts[i], tl) == 0);
        if (i + 1 < count) {
            uint32_t next = rd32(c + 46 + nl + 42);
            CHECK_I(next, lho + 30 + nl + sz);   /* entries back to back */
        }
        at += 46 + nl;
    }
    CHECK_I(i, count);
    CHECK_I(at, (size_t)cd_off + cd_size);
}

/* ------------------------------------------------------------------ tests */

static yds_dataset g_ds, g_ds2;
static unsigned char g_zip[1 << 18], g_zip2[1 << 18];

static const char* path_in(char* buf, size_t cap, const char* dir, const char* name) {
    snprintf(buf, cap, "%s/%s", dir, name);
    return buf;
}

static void test_golden(bool print) {
    char p[512];
    int dots = -1, i;
    const char* s;
    static const char* const files[] = { "session.json", "trials.csv", "frames.csv", "events.csv",
                                         "clock.csv", "streams/dots.csv" };
    const char* const gold[] = { GOLD_SESSION_END, GOLD_TRIALS, GOLD_FRAMES, GOLD_EVENTS, GOLD_CLOCK, GOLD_DOTS };
    const char* const names[] = { "golden/session.json", "golden/trials.csv", "golden/frames.csv",
                                  "golden/events.csv", "golden/clock.csv", "golden/streams/",
                                  "golden/streams/dots.csv" };
    const char* texts[7];
    membuf m;
    yjs_value* root;
    yjs_error je;
    bool b = false;

    CHECK(open_set(&g_ds, WORK "/golden", true, YTR_FLUSH_ROW));
    s = read_file(0, path_in(p, sizeof p, WORK "/golden", "session.json"));
    if (print) printf("#define GOLD_SESSION_START \\\n%s\n", s ? s : "");
    CHECK_S(s, GOLD_SESSION_START);
    CHECK(exists(WORK "/golden/streams"));
    CHECK(!exists(WORK "/golden/session.json.tmp"));
    write_rows(&g_ds, &dots, 3);
    session_changes(&g_ds);
    CHECK_I(yds_rows(&g_ds, YDS_TABLE_TRIALS), 3);
    CHECK_I(yds_rows(&g_ds, YDS_TABLE_FRAMES), 4);
    CHECK_I(yds_rows(&g_ds, YDS_TABLE_EVENTS), 5);
    CHECK_I(yds_rows(&g_ds, YDS_TABLE_CLOCK), 2);
    CHECK_I(yds_rows(&g_ds, YDS_TABLE_STREAM + dots), 3);
    CHECK_I(yds_rows(&g_ds, YDS_TABLE_STREAM + 1), 0);
    CHECK(yds_session(&g_ds) != NULL);
    CHECK(yjs_get(yds_session(&g_ds), "changes") != NULL);
    CHECK_I(yds_zip(&g_ds, WORK "/early.zip", NULL, NULL), YDS_ERR_ORDER);
    CHECK_HAS(yds_error(&g_ds), "still open");
    CHECK_I(yds_finish(&g_ds), 0);
    CHECK_I(yds_finish(&g_ds), YDS_ERR_CLOSED);
    CHECK_I(yds_write_frame(&g_ds, NULL), YDS_ERR_CLOSED);
    CHECK(yds_session(&g_ds) == NULL);
    CHECK(!exists(WORK "/golden/session.json.tmp"));

    for (i = 0; i < 6; i++) {
        s = read_file(1, path_in(p, sizeof p, WORK "/golden", files[i]));
        if (print) printf("/* %s */\n%s\n", files[i], s ? s : "");
        CHECK_S(s, gold[i]);
        texts[i < 5 ? i : 6] = gold[i];
    }
    texts[5] = NULL;

    /* session.json: strict JSON, and its canonical bytes are the file. */
    s = read_file(0, path_in(p, sizeof p, WORK "/golden", "session.json"));
    yjs_arena_reset(&g_a);
    CHECK_I(yjs_parse(&g_a, s, strlen(s), NULL, &root, &je), YJS_OK);
    if (root) {
        char back[8192];
        size_t bn = yjs_write_mem(root, YJS_WRITE_PRETTY, back, sizeof back);
        CHECK(bn + 1 == strlen(s) && memcmp(back, s, bn) == 0 && s[bn] == '\n');
        CHECK_I(yjs_bool(yjs_get(root, "closed"), &b), YJS_OK);
        CHECK(b);
    }

    /* Every CSV back through ysp/table.h. */
    CHECK(read_csv(GOLD_TRIALS, strlen(GOLD_TRIALS)));
    CHECK_I(g_tab.n_rows, 3);
    CHECK_I(g_tab.n_cols, 7);
    CHECK(fabs(ytb_num(&g_tab, 0, ytb_col(&g_tab, "rt_s")) - 0.412345678) < 1e-12);
    CHECK(isnan(ytb_num(&g_tab, 1, ytb_col(&g_tab, "rt_s"))));
    CHECK_S(ytb_text(&g_tab, 2, ytb_col(&g_tab, "response")), "right, \"quoted\"");
    CHECK_S(ytb_text(&g_tab, 1, ytb_col(&g_tab, "note")), "no response");
    CHECK(ytb_num(&g_tab, 2, ytb_col(&g_tab, "level")) == 0.1);
    CHECK(read_csv(GOLD_FRAMES, strlen(GOLD_FRAMES)));
    CHECK_I(g_tab.n_rows, 4);
    CHECK_I(g_tab.n_cols, YDS_FRAME_COLS);
    CHECK(isnan(ytb_num(&g_tab, 3, ytb_col(&g_tab, "onset_s"))));
    CHECK(isnan(ytb_num(&g_tab, 0, ytb_col(&g_tab, "gpu_s"))));
    CHECK(fabs(ytb_num(&g_tab, 0, ytb_col(&g_tab, "draw_s")) - 0.00025) < 1e-15);
    CHECK_I(ytb_col(&g_tab, "frame"), 0);
    CHECK(read_csv(GOLD_EVENTS, strlen(GOLD_EVENTS)));
    CHECK_I(g_tab.n_rows, 5);
    CHECK_I(g_tab.n_cols, YDS_EVENT_COLS);
    CHECK_S(ytb_text(&g_tab, 4, ytb_col(&g_tab, "text")), "hidden, then \"visible\"\nagain");
    CHECK_S(ytb_text(&g_tab, 1, ytb_col(&g_tab, "kind")), "keyboard");
    CHECK(ytb_num(&g_tab, 1, ytb_col(&g_tab, "ticks")) == 123456.0);
    CHECK(read_csv(GOLD_CLOCK, strlen(GOLD_CLOCK)));
    CHECK_I(g_tab.n_rows, 2);
    CHECK_I(g_tab.n_cols, YDS_CLOCK_COLS);
    CHECK(ytb_num(&g_tab, 1, ytb_col(&g_tab, "ns_per_tick")) == 1000.0002);
    CHECK(read_csv(GOLD_DOTS, strlen(GOLD_DOTS)));
    CHECK_I(g_tab.n_rows, 3);

    /* The zip, to a file and to memory: the same bytes, each entry right,
     * and the golden digest (dates are fixed, so the bytes are). */
    CHECK_I(yds_zip(&g_ds, WORK "/golden.zip", NULL, NULL), 0);
    s = read_file(2, WORK "/golden.zip");
    CHECK(s != NULL);
    m.p = g_zip;
    m.n = 0;
    m.cap = sizeof g_zip;
    m.fail_after = false;
    m.limit = 0;
    CHECK_I(yds_zip(&g_ds, NULL, mem_write, &m), 0);
    CHECK_I(m.n, g_file_len[2]);
    CHECK(s && memcmp(g_zip, s, m.n) == 0);
    check_zip(g_zip, m.n, names, texts, 7);
    if (print) printf("#define GOLD_ZIP_FNV 0x%016llxULL\n#define GOLD_ZIP_SIZE %lu\n",
                      (unsigned long long)fnv1a(g_zip, m.n), (unsigned long)m.n);
    CHECK_I(m.n, GOLD_ZIP_SIZE);
    CHECK(fnv1a(g_zip, m.n) == GOLD_ZIP_FNV);
    /* A write function that fails: the zip says so. */
    m.n = 0;
    m.fail_after = true;
    m.limit = 100;
    CHECK_I(yds_zip(&g_ds, NULL, mem_write, &m), YDS_ERR_IO);
    CHECK_HAS(yds_error(&g_ds), "the write failed");
    CHECK_I(yds_zip(&g_ds, NULL, NULL, NULL), YDS_ERR_ARG);
    CHECK_I(yds_zip(&g_ds, WORK "/no_such_folder/x.zip", NULL, NULL), YDS_ERR_IO);
    CHECK_HAS(yds_error(&g_ds), "cannot create");
    yds_close(&g_ds);
    yds_close(&g_ds);
    /* After the close the folder is still the handle's to zip. */
    m.n = 0;
    m.fail_after = false;
    CHECK_I(yds_zip(&g_ds, NULL, mem_write, &m), 0);
    CHECK(fnv1a(g_zip, m.n) == GOLD_ZIP_FNV);
}

/* The child of the crash test: rows, a live change, then gone without a
 * finish. _Exit() runs no atexit handler and flushes no stdio buffer, as a
 * crash. */
static int crash_child(const char* dir) {
    int dots;
    yjs_arena_init_heap(&g_a, 0);
    if (!open_set(&g_ds, dir, true, YTR_FLUSH_ROW)) return 2;
    write_rows(&g_ds, &dots, 2);
    session_changes(&g_ds);
    _Exit(3);
}

/* What a data set cut short leaves: every file readable, session.json
 * strict JSON with "closed": false and no "rows", every row written. */
static void check_unfinished(const char* dir, int trials) {
    char p[512];
    const char* s;
    yjs_value* root;
    yjs_error je;
    bool b = true;
    s = read_file(0, path_in(p, sizeof p, dir, "session.json"));
    CHECK(s != NULL);
    if (!s) return;
    yjs_arena_reset(&g_a);
    CHECK_I(yjs_parse(&g_a, s, strlen(s), NULL, &root, &je), YJS_OK);
    CHECK_I(yjs_bool(yjs_get(root, "closed"), &b), YJS_OK);
    CHECK(!b);
    CHECK(yjs_get(root, "rows") == NULL);
    CHECK(yjs_get(root, "changes") != NULL);
    CHECK(yjs_get(root, "self_test") != NULL);
    CHECK(!exists(path_in(p, sizeof p, dir, "session.json.tmp")));
    s = read_file(1, path_in(p, sizeof p, dir, "trials.csv"));
    CHECK(s && read_csv(s, strlen(s)));
    CHECK_I(g_tab.n_rows, trials);
    s = read_file(1, path_in(p, sizeof p, dir, "frames.csv"));
    CHECK(s && read_csv(s, strlen(s)));
    CHECK_I(g_tab.n_rows, 4);
    s = read_file(1, path_in(p, sizeof p, dir, "events.csv"));
    CHECK(s && read_csv(s, strlen(s)));
    CHECK_I(g_tab.n_rows, 5);
    s = read_file(1, path_in(p, sizeof p, dir, "clock.csv"));
    CHECK(s && read_csv(s, strlen(s)));
    CHECK_I(g_tab.n_rows, 2);
    s = read_file(1, path_in(p, sizeof p, dir, "streams/dots.csv"));
    CHECK(s && read_csv(s, strlen(s)));
    CHECK_I(g_tab.n_rows, 3);
}

static void test_crash(const char* self) {
    int dots;
    /* In this process, while the data set is open: each row is in the
     * file already. */
    CHECK(open_set(&g_ds, WORK "/open", true, YTR_FLUSH_ROW));
    write_rows(&g_ds, &dots, 2);
    session_changes(&g_ds);
    check_unfinished(WORK "/open", 2);
    yds_close(&g_ds);   /* finishes it */
    {
        char p[512];
        const char* s = read_file(0, path_in(p, sizeof p, WORK "/open", "session.json"));
        CHECK_HAS(s, "\"closed\": true");
        CHECK_HAS(s, "\"trials\": 2");
    }
#if !defined(__EMSCRIPTEN__)
    {
        char cmd[2048];
        int rc;
#if defined(_WIN32)
        /* cmd.exe drops the outer quotes of a line that starts with one. */
        snprintf(cmd, sizeof cmd, "\"\"%s\" --crash-child %s/crash\"", self, WORK);
#else
        snprintf(cmd, sizeof cmd, "\"%s\" --crash-child %s/crash", self, WORK);
#endif
        rc = system(cmd);
#if !defined(_WIN32)
        if (rc != -1 && WIFEXITED(rc)) rc = WEXITSTATUS(rc);
#endif
        CHECK_I(rc, 3);
        check_unfinished(WORK "/crash", 2);
    }
#else
    (void)self;
#endif
}

static void test_refusals(void) {
    yds_desc d;
    char longdir[YDS_MAX_DIR + 8];
    static const ytr_col bad_cols[] = { YTR_COL_SET("rt", YTR_COL_TIME_NS) };
    yjs_value* s;
    int i, id;
    dots_row dr;

    memset(&d, 0, sizeof d);
    CHECK(!yds_open(&g_ds, NULL));
    CHECK_HAS(yds_error(&g_ds), "null desc");
    CHECK(!yds_open(&g_ds, &d));
    CHECK_HAS(yds_error(&g_ds), "no dir");
    d.dir = WORK "/r1";
    CHECK(!yds_open(&g_ds, &d));
    CHECK_HAS(yds_error(&g_ds), "no trial_cols");
    d.trial_cols = bad_cols;
    d.n_trial_cols = 1;
    CHECK(!yds_open(&g_ds, &d));
    CHECK_HAS(yds_error(&g_ds), "trial_cols");
    CHECK_HAS(yds_error(&g_ds), "rt_s");
    CHECK(!exists(WORK "/r1"));
    d.trial_cols = g_trial_cols;
    d.n_trial_cols = N_TRIAL_COLS;
    d.flush = (ytr_flush)7;
    CHECK(!yds_open(&g_ds, &d));
    CHECK_HAS(yds_error(&g_ds), "flush 7");
    d.flush = YTR_FLUSH_ROW;
    s = yjs_new(&g_a, YJS_ARRAY);
    d.session = s;
    CHECK(!yds_open(&g_ds, &d));
    CHECK_HAS(yds_error(&g_ds), "not an object");
    s = session_value(true);
    yjs_set(&g_a, s, "closed", yjs_new(&g_a, YJS_TRUE));
    d.session = s;
    CHECK(!yds_open(&g_ds, &d));
    CHECK_HAS(yds_error(&g_ds), "'closed'");
    s = session_value(true);
    yjs_set(&g_a, s, "format", yjs_new_stringz(&g_a, "x"));
    d.session = s;
    CHECK(!yds_open(&g_ds, &d));
    CHECK_HAS(yds_error(&g_ds), "'format'");
    CHECK(!exists(WORK "/r1"));
    memset(longdir, 'a', sizeof longdir - 1);
    longdir[sizeof longdir - 1] = '\0';
    d.session = NULL;
    d.dir = longdir;
    CHECK(!yds_open(&g_ds, &d));
    CHECK_HAS(yds_error(&g_ds), "longer than");
    d.dir = WORK "/no_parent/r2";
    CHECK(!yds_open(&g_ds, &d));
    CHECK_HAS(yds_error(&g_ds), "parent");
    CHECK(!exists(WORK "/no_parent"));
    d.dir = WORK "/bad\xff";
    CHECK(!yds_open(&g_ds, &d));
    CHECK_HAS(yds_error(&g_ds), "not UTF-8");

    /* A folder that is there: refused, untouched. */
    make_dir(WORK "/there");
    d.dir = WORK "/there";
    CHECK(!yds_open(&g_ds, &d));
    CHECK_HAS(yds_error(&g_ds), "exists");
    CHECK(!exists(WORK "/there/session.json"));
    CHECK(!exists(WORK "/there/streams"));
    yds_close(&g_ds);
    yds_close(&g_ds);

    /* No session given: the writer's keys and a start time. */
    d.dir = WORK "/plain";
    CHECK(yds_open(&g_ds, &d));
    d.dir = WORK "/plain2";
    CHECK(!yds_open(&g_ds, &d));                    /* still open: refused, kept */
    CHECK_HAS(yds_error(&g_ds), "close it first");
    CHECK(!exists(WORK "/plain2"));
    CHECK(yds_session(&g_ds) != NULL);
    {
        const yjs_value* r = yds_session(&g_ds);
        const char* st = yjs_string(yjs_get(r, "start_utc"), NULL);
        int64_t v = 0;
        CHECK(st && strlen(st) == 20 && st[4] == '-' && st[10] == 'T' && st[19] == 'Z');
        CHECK_S(yjs_string(yjs_get(r, "format"), NULL), YDS_FORMAT);
        CHECK_I(yjs_int64(yjs_get(r, "version"), &v), YJS_OK);
        CHECK_I(v, 1);
    }
    /* Session keys */
    s = yjs_new_int(&g_a, 1);
    CHECK_I(yds_session_set(&g_ds, "closed", s), YDS_ERR_ARG);
    CHECK_I(yds_session_set(&g_ds, "rows", s), YDS_ERR_ARG);
    CHECK_I(yds_session_push(&g_ds, "write_errors", s), YDS_ERR_ARG);
    CHECK_HAS(yds_error(&g_ds), "writer's key");
    CHECK_I(yds_session_set(&g_ds, NULL, s), YDS_ERR_ARG);
    CHECK_I(yds_session_set(&g_ds, "x", NULL), YDS_ERR_ARG);
    CHECK_I(yds_session_set(&g_ds, "n", s), 0);
    CHECK_I(yds_session_push(&g_ds, "n", s), YDS_ERR_ARG);
    CHECK_HAS(yds_error(&g_ds), "not an array");
    CHECK_I(yds_session_set(&g_ds, "start_utc", yjs_new_stringz(&g_a, "2026-01-01T00:00:00Z")), 0);
    CHECK_I(yds_session_push(&g_ds, "pushed", s), 0);
    CHECK_I(yds_session_push(&g_ds, "pushed", s), 0);
    CHECK_I(yjs_get(yds_session(&g_ds), "pushed")->n, 2);
    /* The adapters' missing values: no device ticks without YIN_DEVTICKS,
     * no frame, time or residual for an event that never landed. */
    {
        yin_event in;
        ytl_event te;
        yds_event e;
        memset(&in, 0, sizeof in);
        in.ticks = 99u;
        in.kind = (uint8_t)YIN_KIND_USER;
        yds_event_from_input(&e, &in, "r", 1);
        CHECK_I(e.ticks, YTR_MISSING_I64);
        CHECK_S(e.kind, "user");
        memset(&te, 0, sizeof te);
        te.frame = -1;
        te.onset = 5;
        te.residual = 6;
        te.kind = (uint8_t)YTL_KIND_USER;
        yds_event_from_timeline(&e, &te, "s", 1);
        CHECK_I(e.frame, YTR_MISSING_I64);
        CHECK_I(e.t_ns, YTR_MISSING_NS);
        CHECK_I(e.residual_ns, YTR_MISSING_NS);
        CHECK_S(e.kind, "user");
    }
    /* Streams */
    CHECK_I(yds_stream(&g_ds, "a-b", g_dots_cols, 3), YDS_ERR_ARG);
    CHECK_HAS(yds_error(&g_ds), "not a name");
    CHECK_I(yds_stream(&g_ds, "1x", g_dots_cols, 3), YDS_ERR_ARG);
    CHECK_I(yds_stream(&g_ds, "", g_dots_cols, 3), YDS_ERR_ARG);
    CHECK_I(yds_stream(&g_ds, NULL, g_dots_cols, 3), YDS_ERR_ARG);
    CHECK_I(yds_stream(&g_ds, "../up", g_dots_cols, 3), YDS_ERR_ARG);
    CHECK_I(yds_stream(&g_ds, "a234567890123456789012345678901234567890123456789012345678901234", g_dots_cols, 3),
            YDS_ERR_ARG);
    CHECK_I(yds_stream(&g_ds, "bad", bad_cols, 1), YDS_ERR_ARG);
    CHECK_HAS(yds_error(&g_ds), "rt_s");
    CHECK(!exists(WORK "/plain/streams/bad.csv"));
    CHECK_I(yds_stream(&g_ds, "dots", g_dots_cols, 3), 0);
    CHECK_I(yds_stream(&g_ds, "DOTS", g_dots_cols, 3), YDS_ERR_ARG);
    CHECK_HAS(yds_error(&g_ds), "more than case");
    for (i = 1; i < YDS_MAX_STREAMS; i++) {
        char nm[16];
        snprintf(nm, sizeof nm, "s%d", i);
        CHECK_I(yds_stream(&g_ds, nm, g_dots_cols, 3), i);
    }
    CHECK_I(yds_stream(&g_ds, "one_more", g_dots_cols, 3), YDS_ERR_FULL);
    CHECK(yds_stream_writer(&g_ds, YDS_MAX_STREAMS - 1) != NULL);
    CHECK(yds_stream_writer(&g_ds, YDS_MAX_STREAMS) == NULL);
    CHECK(yds_stream_writer(&g_ds, -1) == NULL);
    dr.t_ns = 1;
    dr.seed = 2;
    dr.updates = 3;
    CHECK_I(yds_write_stream(&g_ds, YDS_MAX_STREAMS, &dr), YDS_ERR_ARG);
    CHECK_I(yds_write_stream(&g_ds, -1, &dr), YDS_ERR_ARG);
    CHECK_HAS(yds_error(&g_ds), "no stream -1");
    id = YDS_MAX_STREAMS - 1;
    CHECK_I(yds_write_stream(&g_ds, id, &dr), 0);
    CHECK_I(yds_write_stream(&g_ds, id, NULL), YDS_ERR_ARG);
    CHECK_HAS(yds_error(&g_ds), "s15.csv");
    /* Rows */
    CHECK_I(yds_write_frame(&g_ds, NULL), YDS_ERR_ARG);
    CHECK_I(yds_write_event(&g_ds, NULL), YDS_ERR_ARG);
    CHECK_I(yds_write_clock(&g_ds, NULL), YDS_ERR_ARG);
    CHECK_I(yds_write_trial(&g_ds, NULL), YDS_ERR_ARG);
    CHECK_HAS(yds_error(&g_ds), "trials.csv");
    CHECK_I(yds_finish(&g_ds), 0);
    {
        char p[512];
        const char* t = read_file(0, path_in(p, sizeof p, WORK "/plain", "session.json"));
        CHECK_HAS(t, "\"start_utc\": \"2026-01-01T00:00:00Z\"");
        CHECK_HAS(t, "\"s15\": 1");
        CHECK_HAS(t, "\"dots\": 0");
        CHECK(t && !strstr(t, "write_errors"));
    }
    CHECK_I(yds_write_trial(&g_ds, NULL), YDS_ERR_CLOSED);
    CHECK_I(yds_stream(&g_ds, "late", g_dots_cols, 3), YDS_ERR_CLOSED);
    CHECK_I(yds_session_set(&g_ds, "late", s), YDS_ERR_CLOSED);
    CHECK_I(yds_rows(&g_ds, YDS_TABLE_TRIALS), 0);
    yds_close(&g_ds);

    /* A handle never opened */
    memset(&g_ds2, 0, sizeof g_ds2);
    yds_close(&g_ds2);
    CHECK_I(yds_finish(&g_ds2), YDS_ERR_CLOSED);
    CHECK_I(yds_zip(&g_ds2, WORK "/z.zip", NULL, NULL), YDS_ERR_ORDER);
    CHECK_HAS(yds_error(&g_ds2), "not finished");
    CHECK_I(yds_write_event(NULL, NULL), YDS_ERR_ARG);
    CHECK_I(yds_finish(NULL), YDS_ERR_ARG);
    yds_close(NULL);
    CHECK_S(yds_error(NULL), "");
    CHECK(!yds_open(NULL, &d));
    CHECK_S(yds_strerror(YDS_ERR_EXISTS), "the folder exists");
    CHECK_S(yds_strerror(-99), "unknown error");
    CHECK_S(yds_version(), YDS_VERSION_STRING);
}

/* The rewrite of session.json fails: the old file stays, whole. */
static void test_atomic(void) {
    char p[512], before[8192];
    const char* s;
    yjs_value* v = yjs_new_int(&g_a, 5);
    CHECK(open_set(&g_ds, WORK "/atomic", true, YTR_FLUSH_ROW));
    s = read_file(0, path_in(p, sizeof p, WORK "/atomic", "session.json"));
    before[0] = 0;
    if (s && strlen(s) < sizeof before) memcpy(before, s, strlen(s) + 1);
    /* A folder where the temporary file goes: its create fails. */
    make_dir(WORK "/atomic/session.json.tmp");
    CHECK_I(yds_session_set(&g_ds, "k", v), YDS_ERR_IO);
    CHECK_HAS(yds_error(&g_ds), "cannot create");
    CHECK_S(read_file(0, path_in(p, sizeof p, WORK "/atomic", "session.json")), before);
    rm_tree(WORK "/atomic/session.json.tmp");
#if defined(_WIN32)
    {
        /* Held open without FILE_SHARE_DELETE: MoveFileExW cannot replace
         * it. The retries end, the old file stays, the temporary goes. */
        wchar_t w[600];
        HANDLE h;
        LARGE_INTEGER t0, t1, fq;
        wide(path_in(p, sizeof p, WORK "/atomic", "session.json"), w, 600);
        h = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        CHECK(h != INVALID_HANDLE_VALUE);
        QueryPerformanceFrequency(&fq);
        QueryPerformanceCounter(&t0);
        CHECK_I(yds_session_set(&g_ds, "k", v), YDS_ERR_IO);
        QueryPerformanceCounter(&t1);
        CHECK_HAS(yds_error(&g_ds), "the old session.json stays");
        CHECK((t1.QuadPart - t0.QuadPart) * 1000 / fq.QuadPart >= 35);   /* 5 tries, 4 waits of 10 ms */
        CloseHandle(h);
        CHECK_S(read_file(0, path_in(p, sizeof p, WORK "/atomic", "session.json")), before);
        CHECK(!exists(WORK "/atomic/session.json.tmp"));
    }
#endif
    /* The value stayed in the handle: the next rewrite has it. */
    CHECK_I(yds_session_set(&g_ds, "k2", v), 0);
    s = read_file(0, path_in(p, sizeof p, WORK "/atomic", "session.json"));
    CHECK_HAS(s, "\"k\": 5");
    CHECK_HAS(s, "\"k2\": 5");
    CHECK_HAS(s, "\"closed\": false");

    /* A table whose write failed (as ysp/trials.h marks it): later rows
     * refused, the others go on, finish says so in write_errors. */
    {
        yds_frame f;
        yds_event e;
        yds_frame_init(&f);
        yds_event_init(&e);
        e.kind = "x";
        g_ds.frames.io_failed = true;
        snprintf(g_ds.frames.error, sizeof g_ds.frames.error, "ysp_trials: simulated failure");
        CHECK_I(yds_write_frame(&g_ds, &f), YDS_ERR_IO);
        CHECK_HAS(yds_error(&g_ds), "frames.csv");
        CHECK_HAS(yds_error(&g_ds), "simulated failure");
        CHECK_I(yds_write_event(&g_ds, &e), 0);
        CHECK_I(yds_finish(&g_ds), YDS_ERR_IO);
        CHECK_HAS(yds_error(&g_ds), "frames.csv");
        s = read_file(0, path_in(p, sizeof p, WORK "/atomic", "session.json"));
        CHECK_HAS(s, "\"closed\": true");
        CHECK_HAS(s, "\"write_errors\": [");
        CHECK_HAS(s, "frames.csv: ysp_trials: simulated failure");
        CHECK_HAS(s, "\"events\": 1");
    }
    yds_close(&g_ds);
}

/* YTR_FLUSH_SYNC, a UTF-8 folder, a trailing separator. */
static void test_variants(void) {
    char p[512];
    int dots;
    const char* s;
    membuf m;
    static const char* const files[] = { "trials.csv", "frames.csv", "events.csv", "clock.csv", "streams/dots.csv" };
    const char* const gold[] = { GOLD_TRIALS, GOLD_FRAMES, GOLD_EVENTS, GOLD_CLOCK, GOLD_DOTS };
    int i;
    CHECK(open_set(&g_ds, WORK "/sync", true, YTR_FLUSH_SYNC));
    write_rows(&g_ds, &dots, 3);
    CHECK_I(yds_finish(&g_ds), 0);
    yds_close(&g_ds);
    for (i = 0; i < 5; i++) CHECK_S(read_file(1, path_in(p, sizeof p, WORK "/sync", files[i])), gold[i]);

    CHECK(open_set(&g_ds, WORK "/d\xc3\xa9j\xc3\xa0_vu/", false, YTR_FLUSH_ROW));
    write_rows(&g_ds, &dots, 1);
    CHECK_I(yds_finish(&g_ds), 0);
    s = read_file(0, WORK "/d\xc3\xa9j\xc3\xa0_vu/session.json");
    CHECK_HAS(s, "\"closed\": true");
    CHECK_HAS(s, "\"start_utc\": \"");
    m.p = g_zip2;
    m.n = 0;
    m.cap = sizeof g_zip2;
    m.fail_after = false;
    m.limit = 0;
    CHECK_I(yds_zip(&g_ds, NULL, mem_write, &m), 0);
    /* The first entry's name: the folder's UTF-8 name, no trailing '/'. */
    CHECK(m.n > 30 && memcmp(g_zip2 + 30, "d\xc3\xa9j\xc3\xa0_vu/session.json", 22) == 0);
    yds_close(&g_ds);
}

/* ysp/trials.h v0.6's data-line columns in a player's trials.csv: the
 * variable's text and trial_seed in the trials writer's own spelling. */
typedef struct vrow {
    int         trial_index;
    const char* side;
    char        trial_seed[YDS_SEED_TEXT];
} vrow;

static const ytr_col g_vrow_cols[] = {
    YTR_COL(vrow, trial_index, YTR_COL_INT),
    YTR_COL(vrow, side, YTR_COL_STRING),
    YTR_COL(vrow, trial_seed, YTR_COL_CHARS),
};

/* Field k of a CSV line without quotes. */
static void field(const char* line, int k, char* out, size_t cap) {
    size_t n = 0;
    while (k > 0 && *line) if (*line++ == ',') k--;
    while (*line && *line != ',' && *line != '\n' && n + 1 < cap) out[n++] = *line++;
    out[n] = 0;
}

static ytr_trials g_tr;
static void test_trials_v06(void) {
    static const char* const sides[] = { "left", "right" };
    static uint64_t rng_seed = 99;
    ytr_desc d;
    ytr_trial_info ti;
    yds_desc dd;
    vrow rows[12];
    char head[4096], line[4096], f[64], p[512];
    int k_seed = -1, k_side = -1, k, n = 0, i;
    const char* s;
    char x[YDS_SEED_TEXT];
    yds_seed_text(0x00f3a9c4e2b7d158ULL, x);
    CHECK_S(x, "x00f3a9c4e2b7d158");
    yds_seed_text(UINT64_MAX, x);
    CHECK_S(x, "xffffffffffffffff");

    memset(&d, 0, sizeof d);
    d.factors[0] = ytr_factor("contrast", 3);
    d.n_factors = 1;
    d.reps = 4;
    d.order = YTR_ORDER_RANDOM;
    d.rng = ytr_splitmix;
    d.rng_ctx = &rng_seed;
    d.seed = 0x5eedULL;
    d.vars[0] = ytr_var_choice("side", sides, 2);
    d.n_vars = 1;
    CHECK(ytr_open(&g_tr, &d));
    CHECK(ytr_format_header(&g_tr, head, sizeof head) > 0);
    for (k = 0; k < 64; k++) {
        field(head, k, f, sizeof f);
        if (!f[0]) break;
        if (strcmp(f, "trial_seed") == 0) k_seed = k;
        if (strcmp(f, "side") == 0) k_side = k;
    }
    CHECK(k_seed >= 0 && k_side >= 0);

    memset(&dd, 0, sizeof dd);
    dd.dir = WORK "/v06";
    dd.trial_cols = g_vrow_cols;
    dd.n_trial_cols = 3;
    CHECK(yds_open(&g_ds, &dd));
    while (n < 12 && ytr_next(&g_tr, &ti) >= 0) {
        ytr_update(&g_tr, 1, NULL);
        rows[n].trial_index = ti.index;
        rows[n].side = ytr_var_text(&g_tr, ti.index, 0);
        yds_seed_text(ytr_trial_seed(&g_tr, ti.index), rows[n].trial_seed);
        CHECK_I(yds_write_trial(&g_ds, &rows[n]), 0);
        /* The same texts as the trials writer's own data line. */
        CHECK(ytr_format_row(&g_tr, ti.index, line, sizeof line) > 0);
        field(line, k_seed, f, sizeof f);
        CHECK_S(rows[n].trial_seed, f);
        field(line, k_side, f, sizeof f);
        CHECK_S(rows[n].side, f);
        n++;
    }
    CHECK_I(n, 12);
    CHECK_I(yds_finish(&g_ds), 0);
    yds_close(&g_ds);
    s = read_file(1, path_in(p, sizeof p, WORK "/v06", "trials.csv"));
    CHECK(s && read_csv(s, strlen(s)));
    CHECK_I(g_tab.n_rows, 12);
    CHECK_I(ytb_col_type(&g_tab, ytb_col(&g_tab, "trial_seed")), YTB_STRING);
    for (i = 0; i < n && i < g_tab.n_rows; i++) {
        CHECK_S(ytb_text(&g_tab, i, ytb_col(&g_tab, "trial_seed")), rows[i].trial_seed);
        CHECK_S(ytb_text(&g_tab, i, ytb_col(&g_tab, "side")), rows[i].side);
    }
    ytr_close(&g_tr);
}

int main(int argc, char** argv) {
    bool print = argc > 1 && strcmp(argv[1], "--golden") == 0;
    if (argc > 2 && strcmp(argv[1], "--crash-child") == 0) return crash_child(argv[2]);
    yjs_arena_init_heap(&g_a, 0);
    rm_tree(WORK);
    make_dir(WORK);
    test_golden(print);
    if (print) return 0;
    test_crash(argv[0]);
    test_refusals();
    test_atomic();
    test_variants();
    test_trials_v06();
    if (g_failures == 0) rm_tree(WORK);
    yjs_arena_free(&g_a);
    printf("dataset_test: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
