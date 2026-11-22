/* build_test.c - Build (designer/core/build.c; docs/designer.md, "Plan of
 * step 6", Tests row B, and "Part B as built"):
 *
 *   1. Each tests/player project's Build has the pack ID of `ypak build` of
 *      the same description (the real ypak, as a process), and its stamp
 *      names the description, the definition and each source, absolute.
 *   2. A table with a bad cell: a DSG_FROM_BUILD error whose message the
 *      tables view maps to the cell (dsg_csv_place()).
 *   3. A definition error has its line, column and offset; a warning is an
 *      issue of severity warning with its place.
 *   4. Build builds the files: after a save, the pack's definition is the
 *      file's bytes.
 *   5. Newest wins: posts while the thread is held give one build, the
 *      last. A cancel drops a waiting Build.
 *   6. Cancel of a Build that runs (POSIX: a stand-in ffmpeg on the PATH
 *      holds the build in its step): DSG_BUILD_CANCELED, no pack, no
 *      temporary file.
 *   7. Stale: by revision, by a source's time (same size), by its size, and
 *      when the pack is gone.
 *   8. The check pack: the entry cache in DSG_CACHE_DIR; a build skipped
 *      only when its inputs are the last build's, and every pack it leaves
 *      equal to a fresh build of the same inputs (stroop's glyph runs too);
 *      through dsg_check() after a forget.
 *   9. Two builds at once, a Build and a check pack of one project sharing
 *      one cache (stroop: glyph runs); run under ThreadSanitizer too.
 *
 * Usage: test_designer_build WORK YPAK. stroop needs YSP_DESIGNER_TEST_FONT
 * (parts with it are skipped without one). */
#if !defined(_WIN32) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE 1   /* setenv(), chmod() under -std=c11 */
#endif
#include "dsg.h"
#include "csv.h"
#include "ysp/pack.h"
#include "ysp/pack_tool.h"
#include "ysp/rt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/utime.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#ifndef YSP_SOURCE_DIR
#define YSP_SOURCE_DIR "."
#endif
#ifndef YSP_DESIGNER_TEST_FONT
#define YSP_DESIGNER_TEST_FONT ""
#endif

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static char work[DSG_PATH_MAX];
static const char* ypak;
static const int64_t kWait = 120000000000LL;   /* a sanitizer build is slow */

/* --- files and projects --------------------------------------------------------- */

static int read_all(const char* path, char** d, size_t* n) {
    char err[512];
    if (dsg_read_file(path, d, n, err, sizeof err)) return -1;
    return 0;
}

/* A file's modification time in seconds from 1970 (whole seconds: Windows'
 * _utime64() takes no less). */
static int set_mtime_s(const char* path, int64_t s) {
#if defined(_WIN32)
    struct __utimbuf64 t;
    t.actime = (__time64_t)s;
    t.modtime = (__time64_t)s;
    return _utime64(path, &t);
#else
    struct timespec t[2];
    t[0].tv_sec = (time_t)s;
    t[0].tv_nsec = 0;
    t[1] = t[0];
    return utimensat(AT_FDCWD, path, t, 0);
#endif
}

/* Writes a file as a person would have long before the test: each write an
 * hour or two ago, one second after the last, so every write has a time of
 * its own and none is within the check pack's racy window
 * (DSG_BUILD_RACY_NS, build.c). */
static int64_t g_aged;
static int write_now(const char* path, const void* d, size_t n) {
    char err[512];
    if (dsg_write_file(path, d, n, err, sizeof err)) {
        printf("  %s\n", err);
        return -1;
    }
    return 0;
}
static int write_all(const char* path, const void* d, size_t n) {
    if (write_now(path, d, n)) return -1;
    if (!g_aged) g_aged = (int64_t)time(NULL) - 7200;
    return set_mtime_s(path, ++g_aged);
}

static int copy_file(const char* from, const char* to) {
    char* d;
    size_t n;
    int rc;
    if (read_all(from, &d, &n)) {
        printf("  cannot read %s\n", from);
        return -1;
    }
    rc = write_all(to, d, n);
    free(d);
    return rc;
}

static bool exists(const char* path) { return dsg_is_file(path); }

/* tests/player/SRC's files into WORK/NAME (stroop gets the test font). */
static int project(const char* src, const char* name, dsg_project* p, char* dir, size_t cap) {
    static const struct { const char* proj; const char* files[6]; } list[] = {
        { "afc2", { "pack.json", "experiment.json", NULL } },
        { "bimanual", { "pack.json", "experiment.json", NULL } },
        { "oddball", { "pack.json", "experiment.json", "oddball.csv", "standard.png", "target.png", NULL } },
        { "rdk", { "pack.json", "experiment.json", "conditions.csv", NULL } },
        { "stroop", { "pack.json", "stroop.json", "stroop.csv", NULL } },
        { "visuomotor", { "pack.json", "experiment.json", "targets.csv", NULL } },
    };
    char s[DSG_PATH_MAX], a[DSG_PATH_MAX], b[DSG_PATH_MAX], err[512];
    size_t i;
    int k;
    snprintf(s, sizeof s, "%s/tests/player/%s", YSP_SOURCE_DIR, src);
    if (dsg_join(dir, cap, work, name) || dsg_make_dir(dir, err, sizeof err)) return -1;
    for (i = 0; i < sizeof list / sizeof list[0]; i++) {
        if (strcmp(list[i].proj, src) != 0) continue;
        for (k = 0; list[i].files[k]; k++)
            if (dsg_join(a, sizeof a, s, list[i].files[k]) || dsg_join(b, sizeof b, dir, list[i].files[k]) || copy_file(a, b)) return -1;
    }
    if (strcmp(src, "stroop") == 0) {
        if (dsg_join(a, sizeof a, dir, "fonts") || dsg_make_dir(a, err, sizeof err) || dsg_join(b, sizeof b, dir, "fonts/NotoSans-Regular.ttf") ||
            copy_file(YSP_DESIGNER_TEST_FONT, b))
            return -1;
    }
    if (p && dsg_project_open(p, dir, err, sizeof err)) {
        printf("  %s\n", err);
        return -1;
    }
    return 0;
}

/* dir/name's bytes, replaced: the first `from` by `to`. */
static int edit_file(const char* dir, const char* name, const char* from, const char* to) {
    char path[DSG_PATH_MAX];
    char* d;
    size_t n;
    char* at;
    char* out;
    size_t m;
    int rc;
    if (dsg_join(path, sizeof path, dir, name) || read_all(path, &d, &n)) return -1;
    at = strstr(d, from);
    if (!at) {
        printf("  '%s' not in %s\n", from, path);
        free(d);
        return -1;
    }
    m = n - strlen(from) + strlen(to);
    out = (char*)malloc(m + 1);
    memcpy(out, d, (size_t)(at - d));
    memcpy(out + (at - d), to, strlen(to));
    memcpy(out + (at - d) + strlen(to), at + strlen(from), n - (size_t)(at - d) - strlen(from));
    rc = write_all(path, out, m);
    free(out);
    free(d);
    return rc;
}

static void pack_id_of(const char* path, char id[72]) {
    ypak_pack pk;
    ypak_desc d;
    memset(&pk, 0, sizeof pk);
    memset(&d, 0, sizeof d);
    d.path = path;
    id[0] = 0;
    if (!ypak_open(&pk, &d)) return;
    ypak_id(&pk, id);
    ypak_close(&pk);
}

/* An entry's bytes (malloc), or NULL. */
static char* pack_entry(const char* path, const char* name, size_t* n) {
    ypak_pack pk;
    ypak_desc d;
    ypak_entry e;
    char* out = NULL;
    memset(&pk, 0, sizeof pk);
    memset(&e, 0, sizeof e);
    memset(&d, 0, sizeof d);
    d.path = path;
    *n = 0;
    if (!ypak_open(&pk, &d)) return NULL;
    if (ypak_find(&pk, name, &e) == 0) {
        const void* b = ypak_data(&pk, &e);
        if (b) {
            out = (char*)malloc((size_t)e.size + 1);
            memcpy(out, b, (size_t)e.size);
            out[e.size] = 0;
            *n = (size_t)e.size;
        }
    }
    ypak_close(&pk);
    return out;
}

/* `ypak build DESC -o OUT`, the real tool as a process. 0 when it wrote OUT. */
static int ypak_build(const char* desc, const char* out) {
    static int k;
    char log[DSG_PATH_MAX], err[512];
    const char* argv[6];
    dsg_proc_desc d;
    dsg_proc* p = NULL;
    int status = -1, i;
    char name[32];
    snprintf(name, sizeof name, "ypak-%d.log", k++);
    if (dsg_join(log, sizeof log, work, name)) return -1;
    remove(log);
    argv[0] = ypak;
    argv[1] = "build";
    argv[2] = desc;
    argv[3] = "-o";
    argv[4] = out;
    argv[5] = NULL;
    memset(&d, 0, sizeof d);
    d.argv = argv;
    d.out_path = log;
    if (dsg_proc_start(&p, &d, err, sizeof err)) {
        printf("  ypak: %s\n", err);
        return -1;
    }
    for (i = 0; i < 120000 && !dsg_proc_poll(p, &status); i++) yrt_sleep_ns(1000000);
    dsg_proc_free(p);
    return status == 0 ? 0 : -1;
}

/* One Build, waited for and taken. NULL when none came. */
static const dsg_build_result* build_now(dsg_builder* b, const dsg_project* p, uint64_t rev) {
    char out[DSG_PATH_MAX], err[512];
    const dsg_build_result* r = NULL;
    if (dsg_build_path(p, out, sizeof out) || dsg_build_post(b, p, out, rev, err, sizeof err)) {
        printf("  post: %s\n", err);
        return NULL;
    }
    if (dsg_build_wait(b, kWait) != 0 || !dsg_build_take(b, &r)) return NULL;
    return r;
}

static void print_issues(const dsg_build_result* r) {
    int i;
    for (i = 0; r && i < r->report.n_issues; i++)
        printf("    issue %d: sev %d from %d at %u:%u (%u): %s\n", i, r->report.issues[i].severity, r->report.issues[i].from,
               r->report.issues[i].place.line, r->report.issues[i].place.column, r->report.issues[i].place.offset,
               r->report.issues[i].message);
}

static bool is_absolute(const char* p) {
#if defined(_WIN32)
    return (p[0] && p[1] == ':') || (p[0] == '\\' && p[1] == '\\');
#else
    return p[0] == '/';
#endif
}

static volatile int32_t wakes;
static volatile int32_t gate;   /* 1: the build thread waits in its wake */
static void on_wake(void* ctx) {
    (void)ctx;
    dsg_atomic_add(&wakes, 1);
    while (dsg_atomic_load(&gate)) yrt_sleep_ns(1000000);   /* the test's gate, not the builder's */
}

static void wait_wakes(int32_t k) {
    int i;
    for (i = 0; i < 120000 && dsg_atomic_load(&wakes) < k; i++) yrt_sleep_ns(1000000);
}

/* --- 1. pack IDs ----------------------------------------------------------------- */

static void test_pack_ids(dsg_builder* b) {
    static const char* const names[] = { "afc2", "bimanual", "oddball", "rdk", "stroop", "visuomotor" };
    size_t i;
    printf("1. each project's Build has ypak's pack ID\n");
    for (i = 0; i < sizeof names / sizeof names[0]; i++) {
        char dir[DSG_PATH_MAX], other[DSG_PATH_MAX], id[72], want[DSG_PATH_MAX], name[64];
        dsg_project p;
        const dsg_build_result* r;
        int s, n_res = 0;
        bool abs_ok = true, sized = true;
        memset(&p, 0, sizeof p);
        if (strcmp(names[i], "stroop") == 0 && !dsg_is_file(YSP_DESIGNER_TEST_FONT)) {
            printf("  stroop skipped: no font at '%s'\n", YSP_DESIGNER_TEST_FONT);
            continue;
        }
        snprintf(name, sizeof name, "ids_%s", names[i]);
        if (project(names[i], name, &p, dir, sizeof dir)) { CHECK(0, "copy %s", names[i]); continue; }
        r = build_now(b, &p, 7);
        CHECK(r && r->status == DSG_BUILD_OK && r->report.n_errors == 0, "%s: built (%d, %d errors)", names[i], r ? r->status : -1,
              r ? r->report.n_errors : -1);
        if (!r || r->status != DSG_BUILD_OK) { print_issues(r); dsg_project_close(&p); continue; }
        dsg_build_path(&p, want, sizeof want);
        CHECK(strcmp(r->path, want) == 0 && r->revision == 7, "%s: the path %s and the revision", names[i], r->path);
        dsg_join(other, sizeof other, dir, "ypak.ysppak");
        CHECK(ypak_build(p.desc_path, other) == 0, "%s: ypak build", names[i]);
        pack_id_of(other, id);
        CHECK(id[0] && strcmp(id, r->pack_id) == 0, "%s: the pack ID is ypak's: %s against %s", names[i], r->pack_id, id);
        {
            char* d;
            size_t n;
            if (read_all(r->path, &d, &n) == 0) {
                CHECK((int64_t)n == r->size, "%s: the size %lld is the file's", names[i], (long long)r->size);
                free(d);
            }
        }
        /* the stamp: the description, the definition, each source */
        {
            const char* q = p.desc;
            while ((q = strstr(q, "\"source\"")) != NULL) {
                n_res++;
                q += 8;
            }
            for (q = p.desc; (q = strstr(q, "\"from\"")) != NULL; q += 6) n_res++;
        }
        for (s = 0; s < r->n_sources; s++) {
            if (!is_absolute(r->sources[s].path)) abs_ok = false;
            if (r->sources[s].size <= 0) sized = false;
        }
        CHECK(r->n_sources == 1 + n_res && abs_ok && sized, "%s: the stamp has %d sources (want %d), absolute and there", names[i],
              r->n_sources, 1 + n_res);
        CHECK(r->n_sources > 1 && strstr(r->sources[0].path, "pack.json") && strstr(r->sources[1].path, p.def_source),
              "%s: the description first, then the definition", names[i]);
        CHECK(r->ms > 0 && r->done_unix_s > 1700000000, "%s: the time %.2f ms and the end", names[i], r->ms);
        if (strcmp(names[i], "oddball") == 0) {
            /* its two textures went into the project's entry cache */
            char cache[DSG_PATH_MAX], e2[256];
            int64_t kept = -1;
            dsg_join(cache, sizeof cache, dir, DSG_CACHE_DIR);
            CHECK(ypt_cache_trim(cache, INT64_MAX, &kept, e2, sizeof e2) == 0 && kept > 0, "oddball: the Build's records are in %s (%lld bytes)",
                  DSG_CACHE_DIR, (long long)kept);
#if defined(__linux__)
            {
                /* The bound: an old record of 1.5 GiB (sparse), past the
                 * 1 GiB floor, goes at the next Build that writes a record;
                 * the Build's own stay. */
                char fake[DSG_PATH_MAX], png[DSG_PATH_MAX];
                FILE* f;
                int64_t kept2 = -1;
                dsg_join(fake, sizeof fake, cache, "0000000000000000000000000000000000000000000000000000000000000000.yspce");
                f = fopen(fake, "wb");
                if (f) {
                    fseeko(f, (off_t)3 << 29, SEEK_SET);
                    fputc(0, f);
                    fclose(f);
                }
                set_mtime_s(fake, (int64_t)time(NULL) - 100000);
                /* another texture: a record is written */
                dsg_join(png, sizeof png, dir, "standard.png");
                {
                    char* d = NULL;
                    size_t n = 0;
                    char t2[DSG_PATH_MAX];
                    dsg_join(t2, sizeof t2, dir, "target.png");
                    if (read_all(t2, &d, &n) == 0) {
                        write_all(png, d, n);
                        free(d);
                    }
                }
                r = build_now(b, &p, 8);
                CHECK(r && r->status == DSG_BUILD_OK && !exists(fake), "oddball: an old record past the bound went");
                CHECK(ypt_cache_trim(cache, INT64_MAX, &kept2, e2, sizeof e2) == 0 && kept2 > 0 && kept2 < ((int64_t)1 << 30),
                      "oddball: the Build's records stay (%lld bytes)", (long long)kept2);
            }
#endif
        }
        if (strcmp(names[i], "rdk") == 0) {
            /* a folder named "." has its real name */
            dsg_project q = p;
            char got[DSG_PATH_MAX];
            if (snprintf(q.folder, sizeof q.folder, "%s/.", dir) >= (int)sizeof q.folder) q.folder[0] = 0;
            CHECK(dsg_build_path(&q, got, sizeof got) == 0 && strstr(got, "/./build/ids_rdk.ysppak") != NULL, "a folder '.': %s", got);
        }
        CHECK(r && !dsg_build_stale(&p, r, r->revision), "%s: up to date just after", names[i]);
        dsg_project_close(&p);
    }
}

/* --- 2. and 3. issues -------------------------------------------------------------- */

static void test_issues(dsg_builder* b) {
    char dir[DSG_PATH_MAX], path[DSG_PATH_MAX];
    dsg_project p;
    const dsg_build_result* r;
    int i;
    memset(&p, 0, sizeof p);
    printf("2. a table's bad cell\n");
    if (project("rdk", "bad_cell", &p, dir, sizeof dir) == 0) {
        /* a row with one field too many: ysp/table.h names its line and row */
        if (edit_file(dir, "conditions.csv", "0.032,180,left", "0.032,180,left,extra")) CHECK(0, "edit the table");
        r = build_now(b, &p, 3);
        CHECK(r && r->status == DSG_BUILD_FAILED && r->report.n_errors == 1, "a bad cell fails the Build (%d)", r ? r->status : -1);
        print_issues(r);
        if (r && r->report.n_issues > 0) {
            const dsg_issue* is = &r->report.issues[0];
            char entry[256], cname[128];
            int row = -9, col = -9;
            const bool placed = dsg_csv_place(is->message, entry, sizeof entry, &row, &col, cname, sizeof cname);
            CHECK(is->severity == DSG_SEV_ERROR && is->from == DSG_FROM_BUILD, "an error from Build");
            CHECK(placed && strcmp(entry, "conditions.pstb") == 0 && row == 1, "the tables view's place: entry '%s', row %d, col %d",
                  placed ? entry : "", row, col);
            CHECK(is->place.line == 0, "no place in the definition");
            CHECK(strstr(is->message, "conditions.csv") != NULL, "the message names the source: %s", is->message);
            CHECK(strncmp(is->message, "ypak: ", 6) != 0, "without the tool's program name");
        }
        CHECK(r && dsg_build_stale(&p, r, 3), "a failed Build is stale");
        dsg_project_close(&p);
    } else {
        CHECK(0, "copy rdk");
    }

    printf("3. a definition's error and warning\n");
    if (project("rdk", "bad_def", &p, dir, sizeof dir) == 0) {
        char* d = NULL;
        size_t n = 0;
        if (edit_file(dir, "experiment.json", "\"$coherence\"", "\"$no_such_column\"")) CHECK(0, "edit the definition");
        r = build_now(b, &p, 4);
        CHECK(r && r->status == DSG_BUILD_FAILED && r->report.n_errors >= 1, "a definition error fails the Build");
        print_issues(r);
        dsg_join(path, sizeof path, dir, "experiment.json");
        if (r && r->report.n_issues > 0 && read_all(path, &d, &n) == 0) {
            const dsg_issue* is = &r->report.issues[0];
            const char* at = strstr(d, "\"$no_such_column\"");
            uint32_t line = 0, col = 0;
            dsg_line_col(d, n, (uint32_t)(at - d), &line, &col);
            CHECK(is->from == DSG_FROM_BUILD && is->severity == DSG_SEV_ERROR && is->place.line == line,
                  "its line %u is the column's %u", is->place.line, line);
            CHECK(is->place.offset == dsg_offset_of(d, n, is->place.line, is->place.column) && is->place.offset > 0 &&
                      is->place.offset <= (uint32_t)n,
                  "its offset %u is its line and column's", is->place.offset);
            free(d);
        }
        dsg_project_close(&p);
    } else {
        CHECK(0, "copy rdk");
    }
    if (project("rdk", "warn_def", &p, dir, sizeof dir) == 0) {
        char* d = NULL;
        size_t n = 0;
        if (edit_file(dir, "experiment.json", "{ \"wait_s\": 0.8 }", "{ \"wait_s\": 120 }")) CHECK(0, "edit the definition");
        r = build_now(b, &p, 5);
        CHECK(r && r->status == DSG_BUILD_OK && r->report.n_errors == 0 && r->report.n_issues >= 1, "a warning does not fail it (%d)",
              r ? r->status : -1);
        print_issues(r);
        dsg_join(path, sizeof path, dir, "experiment.json");
        for (i = 0; r && i < r->report.n_issues; i++) {
            const dsg_issue* is = &r->report.issues[i];
            CHECK(is->severity == DSG_SEV_WARNING && is->from == DSG_FROM_BUILD, "issue %d: a warning from Build", i);
            CHECK(strncmp(is->message, "warning: ", 9) != 0, "the severity says it, not the text: %s", is->message);
            if (i == 0 && read_all(path, &d, &n) == 0) {
                const char* at = strstr(d, "\"wait_s\": 120");
                uint32_t line = 0, col = 0;
                dsg_line_col(d, n, (uint32_t)(at - d), &line, &col);
                CHECK(is->place.line == line && is->place.offset == dsg_offset_of(d, n, is->place.line, is->place.column),
                      "the warning's place %u:%u (line %u)", is->place.line, is->place.column, line);
                free(d);
            }
        }
        dsg_project_close(&p);
    } else {
        CHECK(0, "copy rdk");
    }
}

/* --- 4. builds the files -------------------------------------------------------------- */

/* A description that does not parse: its line and column are not the
 * definition's. */
static void test_bad_desc(dsg_builder* b) {
    char dir[DSG_PATH_MAX];
    dsg_project p;
    const dsg_build_result* r;
    memset(&p, 0, sizeof p);
    if (project("rdk", "bad_desc", &p, dir, sizeof dir)) { CHECK(0, "copy rdk"); return; }
    if (edit_file(dir, "pack.json", "\"resources\": [", "\"resources\": [,")) CHECK(0, "edit the description");
    r = build_now(b, &p, 6);
    print_issues(r);
    CHECK(r && r->status == DSG_BUILD_FAILED && r->report.n_errors == 1 && strstr(r->report.issues[0].message, "line ") &&
              r->report.issues[0].place.line == 0,
          "the description's error has no place in the definition");
    CHECK(r && r->n_sources == 1 && strstr(r->sources[0].path, "pack.json"), "the stamp: the description alone (%d)", r ? r->n_sources : -1);
    dsg_project_close(&p);
}

static void test_saved(dsg_builder* b) {
    char dir[DSG_PATH_MAX], err[512];
    dsg_project p;
    memset(&p, 0, sizeof p);
    dsg_saved sv;
    const dsg_build_result* r;
    char* text;
    char* at;
    printf("4. Build builds the saved files\n");
    if (project("rdk", "saved", &p, dir, sizeof dir)) { CHECK(0, "copy rdk"); return; }
    text = (char*)malloc(p.text_len + 1);
    memcpy(text, p.text, p.text_len + 1);
    at = strstr(text, "reps 5");
    CHECK(at != NULL, "the text has 'reps 5'");
    if (at) at[5] = '3';
    memset(&sv, 0, sizeof sv);
    CHECK(dsg_project_save(&p, text, p.text_len, &sv, err, sizeof err) == 0, "saved: %s", err);
    r = build_now(b, &p, 9);
    CHECK(r && r->status == DSG_BUILD_OK, "built after the save");
    if (r && r->status == DSG_BUILD_OK) {
        size_t n = 0, fn = 0;
        char* e = pack_entry(r->path, "experiment/main.json", &n);
        char* f = NULL;
        read_all(p.def_path, &f, &fn);
        CHECK(e && f && strstr(e, "reps 3") && !strstr(e, "reps 5"), "the pack has the edit");
        CHECK(e && f && (n == fn || (n == fn + 1 && e[fn] == '\n')) && memcmp(e, f, fn) == 0,
              "the pack's definition is the saved file's bytes (%zu and %zu)", n, fn);
        free(e);
        free(f);
    }
    dsg_saved_free(&sv);
    free(text);
    dsg_project_close(&p);
}

/* --- 5. newest wins; a cancel drops a waiting Build ----------------------------------- */

static void test_newest(void) {
    char dir[DSG_PATH_MAX], out[DSG_PATH_MAX], err[512];
    dsg_project p;
    memset(&p, 0, sizeof p);
    dsg_builder_desc bd;
    dsg_builder* b;
    const dsg_build_result* r = NULL;
    printf("5. newest wins; a cancel drops a waiting Build\n");
    if (project("rdk", "newest", &p, dir, sizeof dir)) { CHECK(0, "copy rdk"); return; }
    dsg_build_path(&p, out, sizeof out);
    memset(&bd, 0, sizeof bd);
    bd.wake = on_wake;
    b = dsg_builder_start(&bd, err, sizeof err);
    CHECK(b != NULL, "a builder: %s", err);
    if (!b) { dsg_project_close(&p); return; }
    dsg_atomic_add(&wakes, -dsg_atomic_load(&wakes));
    dsg_atomic_add(&gate, 1);
    CHECK(!dsg_build_busy(b), "idle: not busy");
    CHECK(dsg_build_post(b, &p, out, 1, err, sizeof err) == 0 && dsg_build_busy(b), "posted: busy");
    wait_wakes(1);
    /* the thread waits in the first wake: the next posts pile up */
    CHECK(dsg_build_take(b, &r) && r && r->revision == 1 && r->status == DSG_BUILD_OK, "the first result, taken in its wake");
    CHECK(dsg_build_post(b, &p, out, 2, err, sizeof err) == 0 && dsg_build_post(b, &p, out, 3, err, sizeof err) == 0 &&
              dsg_build_busy(b),
          "two more posts wait");
    dsg_atomic_add(&gate, -1);
    CHECK(dsg_build_wait(b, kWait) == 0, "waited");
    CHECK(dsg_build_take(b, &r) && r && r->revision == 3 && r->status == DSG_BUILD_OK, "the newest ran: revision %llu",
          r ? (unsigned long long)r->revision : 0ull);
    CHECK(dsg_atomic_load(&wakes) == 2, "two builds, not three: %d wakes", dsg_atomic_load(&wakes));
    CHECK(!dsg_build_take(b, &r) && !dsg_build_busy(b), "nothing more; idle");

    /* a cancel drops the waiting one */
    dsg_atomic_add(&gate, 1);
    CHECK(dsg_build_post(b, &p, out, 4, err, sizeof err) == 0, "post 4");
    wait_wakes(3);
    CHECK(dsg_build_post(b, &p, out, 5, err, sizeof err) == 0 && dsg_build_busy(b), "post 5 waits");
    dsg_build_cancel(b);
    CHECK(!dsg_build_busy(b), "the cancel dropped it: not busy");
    dsg_atomic_add(&gate, -1);
    CHECK(dsg_build_wait(b, kWait) == 0, "waited");
    CHECK(dsg_build_take(b, &r) && r && r->revision == 4, "only 4's result");
    CHECK(!dsg_build_take(b, &r) && dsg_atomic_load(&wakes) == 3, "5 never ran: %d wakes", dsg_atomic_load(&wakes));
    /* a post after a cancel runs */
    CHECK(dsg_build_post(b, &p, out, 6, err, sizeof err) == 0 && dsg_build_wait(b, kWait) == 0 && dsg_build_take(b, &r) && r &&
              r->revision == 6 && r->status == DSG_BUILD_OK,
          "a post after the cancel builds");
    dsg_builder_stop(b);
    dsg_project_close(&p);
}

/* --- 6. cancel of a running Build ----------------------------------------------------------- */

#if !defined(_WIN32)
static void test_cancel_running(void) {
    static const char fake[] =
        "#!/bin/sh\n"
        "case \"$*\" in *-version*) echo 'ffmpeg version 0.0-ysp-test-stand-in'; echo 'configuration: none'; exit 0;; esac\n"
        ": > \"$YSP_TEST_FAKE_DIR/started\"\n"
        "i=0\n"
        "while [ ! -f \"$YSP_TEST_FAKE_DIR/release\" ] && [ $i -lt 12000 ]; do sleep 0.01; i=$((i+1)); done\n"
        "exit 1\n";
    static const char desc[] =
        "{\"format\": \"ysp-pack-source\", \"version\": 1,\n"
        " \"experiment\": {\"entry\": \"experiment/main.json\", \"source\": \"experiment.json\", \"media_type\": \"application/vnd.ysp.experiment\"},\n"
        " \"resources\": [\n"
        "  {\"name\": \"conditions.pstb\", \"kind\": \"table\", \"source\": \"conditions.csv\"},\n"
        "  {\"kind\": \"video\", \"name\": \"video/clip.mpg\", \"source\": \"raw.gray\",\n"
        "   \"ffmpeg_input\": [\"-f\", \"rawvideo\", \"-pix_fmt\", \"gray\", \"-s\", \"64x48\", \"-framerate\", \"30\"],\n"
        "   \"ffmpeg\": [\"-c:v\", \"mpeg1video\", \"-q:v\", \"3\", \"-g\", \"10\", \"-bf\", \"0\", \"-pix_fmt\", \"yuv420p\", \"-f\", \"mpeg\"]},\n"
        "  {\"name\": \"more.pstb\", \"kind\": \"table\", \"source\": \"conditions.csv\"}\n"
        " ]}\n";
    char dir[DSG_PATH_MAX], fdir[DSG_PATH_MAX], path[DSG_PATH_MAX], out[DSG_PATH_MAX], tmp[DSG_PATH_MAX], err[512], env[2 * DSG_PATH_MAX];
    static uint8_t raw[64 * 48 * 3];
    const char* old_path = getenv("PATH");
    dsg_project p;
    memset(&p, 0, sizeof p);
    dsg_builder* b;
    const dsg_build_result* r = NULL;
    int i;
    printf("6. a cancel stops a running Build after its ffmpeg step\n");
    if (project("rdk", "cancel", NULL, dir, sizeof dir)) { CHECK(0, "copy rdk"); return; }
    dsg_join(path, sizeof path, dir, "pack.json");
    write_all(path, desc, sizeof desc - 1);
    dsg_join(path, sizeof path, dir, "raw.gray");
    write_all(path, raw, sizeof raw);
    dsg_join(fdir, sizeof fdir, work, "fake_ffmpeg");
    dsg_make_dir(fdir, err, sizeof err);
    dsg_join(path, sizeof path, fdir, "ffmpeg");
    write_all(path, fake, sizeof fake - 1);
    chmod(path, 0755);
    dsg_join(path, sizeof path, fdir, "started");
    remove(path);
    dsg_join(path, sizeof path, fdir, "release");
    remove(path);
    snprintf(env, sizeof env, "%s:%s", fdir, old_path ? old_path : "/usr/bin:/bin");
    setenv("PATH", env, 1);
    setenv("YSP_TEST_FAKE_DIR", fdir, 1);
    if (dsg_project_open(&p, dir, err, sizeof err)) { CHECK(0, "open: %s", err); return; }
    b = dsg_builder_start(NULL, err, sizeof err);
    dsg_build_path(&p, out, sizeof out);
    if (snprintf(tmp, sizeof tmp, "%s.tmp", out) >= (int)sizeof tmp) tmp[0] = 0;
    CHECK(b && dsg_build_post(b, &p, out, 11, err, sizeof err) == 0, "posted");
    dsg_join(path, sizeof path, fdir, "started");
    for (i = 0; i < 120000 && !exists(path); i++) yrt_sleep_ns(1000000);
    CHECK(exists(path) && dsg_build_busy(b), "the build is in its ffmpeg step");
    dsg_build_cancel(b);
    CHECK(dsg_build_busy(b), "a cancel does not end the step: still busy");
    dsg_join(path, sizeof path, fdir, "release");
    write_all(path, "1", 1);
    CHECK(dsg_build_wait(b, kWait) == 0 && dsg_build_take(b, &r) && r, "a result");
    CHECK(r && r->status == DSG_BUILD_CANCELED && r->report.n_errors == 0, "canceled, no error (status %d)", r ? r->status : -1);
    print_issues(r);
    CHECK(!exists(out) && !exists(tmp), "no pack and no temporary file");
    CHECK(r && dsg_build_stale(&p, r, 11), "a canceled Build is stale");
    dsg_builder_stop(b);
    dsg_project_close(&p);
    setenv("PATH", old_path ? old_path : "/usr/bin:/bin", 1);
}
#endif

/* --- 7. stale ------------------------------------------------------------------------- */

static void test_stale(dsg_builder* b) {
    char dir[DSG_PATH_MAX], path[DSG_PATH_MAX];
    dsg_project p;
    const dsg_build_result* r;
    char* d = NULL;
    size_t n = 0;
    memset(&p, 0, sizeof p);
    printf("7. stale\n");
    if (project("rdk", "stale", &p, dir, sizeof dir)) { CHECK(0, "copy rdk"); return; }
    CHECK(dsg_build_stale(&p, NULL, 1), "no result: stale");
    r = build_now(b, &p, 20);
    CHECK(r && r->status == DSG_BUILD_OK, "built");
    if (!r || r->status != DSG_BUILD_OK) { dsg_project_close(&p); return; }
    CHECK(!dsg_build_stale(&p, r, 20), "up to date");
    CHECK(dsg_build_stale(&p, r, 21), "another revision: stale");
    /* the same bytes again, later: the time differs, the size does not */
    dsg_join(path, sizeof path, dir, "conditions.csv");
    read_all(path, &d, &n);
    yrt_sleep_ns(30000000);
    write_all(path, d, n);
    CHECK(dsg_build_stale(&p, r, 20), "a source's time: stale");
    r = build_now(b, &p, 20);
    CHECK(r && !dsg_build_stale(&p, r, 20), "built again: up to date");
    if (edit_file(dir, "conditions.csv", "0.032,0,right", "0.0320,0,right")) CHECK(0, "edit");
    CHECK(r && dsg_build_stale(&p, r, 20), "a source's size: stale");
    r = build_now(b, &p, 20);
    CHECK(r && !dsg_build_stale(&p, r, 20), "built again: up to date");
    if (r) remove(r->path);
    CHECK(r && dsg_build_stale(&p, r, 20), "the pack gone: stale");
    r = build_now(b, &p, 20);
    CHECK(r && !dsg_build_stale(&p, r, 20), "built again: up to date");
    dsg_join(path, sizeof path, dir, "pack.json");
    yrt_sleep_ns(30000000);
    if (copy_file(path, path)) CHECK(0, "touch");
    CHECK(r && dsg_build_stale(&p, r, 20), "the description's time: stale");
    free(d);
    dsg_project_close(&p);
}

/* --- 8. the check pack ------------------------------------------------------------------- */

/* The file's identity: a build writes a new file and renames it over the
 * pack, so a pack built again is another file while the old one still
 * holds its number (a time could be the old one's: one step of the file
 * system's clock is 1 to 4 ms here). -1: no file. */
static int64_t file_id(const char* path) {
#if defined(_WIN32)
    BY_HANDLE_FILE_INFORMATION fi;
    HANDLE h = CreateFileA(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    BOOL ok;
    if (h == INVALID_HANDLE_VALUE) return -1;
    memset(&fi, 0, sizeof fi);
    ok = GetFileInformationByHandle(h, &fi);
    CloseHandle(h);
    return ok ? (int64_t)(((uint64_t)fi.nFileIndexHigh << 32) | fi.nFileIndexLow) : -1;
#else
    struct stat st;
    return stat(path, &st) == 0 ? (int64_t)st.st_ino : -1;
#endif
}

/* check.c's check description: the experiment's source at DSG_CHECK_SOURCE. */
static char* check_desc(const dsg_project* p, size_t* n) {
    const char* key = "\"source\": \"";
    const char* at = strstr(p->desc, key);
    const char* end;
    char* out;
    size_t m;
    if (!at) return NULL;
    at += strlen(key);
    end = strchr(at, '"');
    m = (size_t)(at - p->desc) + strlen(DSG_CHECK_SOURCE) + strlen(end);
    out = (char*)malloc(m + 1);
    memcpy(out, p->desc, (size_t)(at - p->desc));
    memcpy(out + (at - p->desc), DSG_CHECK_SOURCE, strlen(DSG_CHECK_SOURCE));
    memcpy(out + (at - p->desc) + strlen(DSG_CHECK_SOURCE), end, strlen(end) + 1);
    *n = m;
    return out;
}

/* A check pack of text: the designer's call, then a fresh build of the same
 * inputs with no cache and no skip, which it must equal. *rebuilt: the pack
 * file changed. */
/* A description in place of check_desc()'s, for one call; NULL: none. */
static const char* g_desc_over;
static bool check_pack_is_fresh(const dsg_project* p, const char* text, size_t tn, bool* rebuilt) {
    char src[DSG_PATH_MAX], pack[DSG_PATH_MAX], fresh[DSG_PATH_MAX], err[1024];
    char* desc;
    size_t dn = 0, an = 0, bn = 0;
    char* a = NULL;
    char* bb = NULL;
    int64_t t0;
    ypt_options o;
    bool same;
    dsg_join(src, sizeof src, p->folder, DSG_CHECK_SOURCE);
    dsg_join(pack, sizeof pack, p->folder, DSG_CHECK_PACK);
    dsg_join(fresh, sizeof fresh, p->folder, DSG_DIR "/fresh.ysppak");
    dsg_join(fresh, sizeof fresh, p->folder, DSG_DIR);
    dsg_make_dir(fresh, err, sizeof err);
    dsg_join(fresh, sizeof fresh, p->folder, DSG_DIR "/check");
    dsg_make_dir(fresh, err, sizeof err);
    dsg_join(fresh, sizeof fresh, p->folder, DSG_DIR "/fresh.ysppak");
    /* as check.c writes it before each build, its time says nothing: the
     * same time for every text */
    write_now(src, text, tn);
    set_mtime_s(src, 1000000000);
    if (g_desc_over) {
        dn = strlen(g_desc_over);
        desc = (char*)malloc(dn + 1);
        memcpy(desc, g_desc_over, dn + 1);
    } else {
        desc = check_desc(p, &dn);
    }
    t0 = file_id(pack);
    if (!desc || dsg_build_check_pack(p, desc, dn, pack, err, sizeof err) != 0) {
        printf("  check pack: %s\n", err);
        free(desc);
        return false;
    }
    *rebuilt = file_id(pack) != t0;
    memset(&o, 0, sizeof o);
    if (ypt_build_text(desc, dn, p->folder, fresh, &o, err, sizeof err) != 0) {
        printf("  fresh: %s\n", err);
        free(desc);
        return false;
    }
    free(desc);
    read_all(pack, &a, &an);
    read_all(fresh, &bb, &bn);
    same = a && bb && an == bn && memcmp(a, bb, an) == 0;
    free(a);
    free(bb);
    return same;
}

static void test_check_pack(void) {
    char dir[DSG_PATH_MAX], path[DSG_PATH_MAX], err[1024];
    dsg_project p;
    bool rebuilt = false;
    memset(&p, 0, sizeof p);
    printf("8. the check pack: the entry cache and the skip\n");
    if (project("oddball", "cp_oddball", &p, dir, sizeof dir) == 0) {
        size_t dn = 0;
        char* desc = check_desc(&p, &dn);
        char pack[DSG_PATH_MAX];
        dsg_join(path, sizeof path, dir, DSG_DIR "/check");
        dsg_join(err, sizeof err, dir, DSG_DIR);
        dsg_make_dir(err, err, sizeof err);
        dsg_make_dir(path, err, sizeof err);
        dsg_join(path, sizeof path, dir, DSG_CHECK_SOURCE);
        write_all(path, p.text, p.text_len);
        dsg_join(pack, sizeof pack, dir, DSG_CHECK_PACK);
        CHECK(desc && dsg_build_check_pack(&p, desc, dn, pack, err, sizeof err) == 0, "oddball's check pack: %s", err);
        dsg_join(path, sizeof path, dir, DSG_CACHE_DIR);
        CHECK(dsg_is_dir(path), "the entry cache is at %s", DSG_CACHE_DIR);
        {
            int64_t kept = -1;
            CHECK(ypt_cache_trim(path, INT64_MAX, &kept, err, sizeof err) == 0 && kept > 0, "it holds oddball's two textures: %lld bytes",
                  (long long)kept);
        }
        free(desc);
        dsg_project_close(&p);
    } else {
        CHECK(0, "copy oddball");
    }

    if (project("rdk", "cp_rdk", &p, dir, sizeof dir) == 0) {
        char* other = (char*)malloc(p.text_len + 1);
        char* at;
        memcpy(other, p.text, p.text_len + 1);
        at = strstr(other, "reps 5");
        if (at) at[5] = '4';
        CHECK(check_pack_is_fresh(&p, p.text, p.text_len, &rebuilt) && rebuilt, "the first: built, fresh");
        CHECK(check_pack_is_fresh(&p, p.text, p.text_len, &rebuilt) && !rebuilt, "the same inputs: skipped, fresh");
        CHECK(check_pack_is_fresh(&p, other, p.text_len, &rebuilt) && rebuilt, "another text: built, fresh");
        CHECK(check_pack_is_fresh(&p, p.text, p.text_len, &rebuilt) && rebuilt, "the first text again: built, fresh");
        CHECK(check_pack_is_fresh(&p, p.text, p.text_len, &rebuilt) && !rebuilt, "again: skipped");
        {
            /* another description, the same sources: an option of the table that
             * its derivation records */
            size_t dn = 0;
            char* d0 = check_desc(&p, &dn);
            char* d1 = (char*)malloc(dn + 64);
            const char* at2 = d0 ? strstr(d0, "\"kind\": \"table\"") : NULL;
            if (at2) {
                memcpy(d1, d0, (size_t)(at2 - d0));
                strcpy(d1 + (at2 - d0), "\"allow_empty\": true, ");
                strcat(d1, at2);
                g_desc_over = d1;
            }
            CHECK(at2 && check_pack_is_fresh(&p, p.text, p.text_len, &rebuilt) && rebuilt, "another description: built, fresh");
            g_desc_over = NULL;
            CHECK(check_pack_is_fresh(&p, p.text, p.text_len, &rebuilt) && rebuilt, "the first again: built, fresh");
            free(d0);
            free(d1);
        }
        dsg_join(path, sizeof path, dir, "conditions.csv");
        yrt_sleep_ns(30000000);
        copy_file(path, path);
        CHECK(check_pack_is_fresh(&p, p.text, p.text_len, &rebuilt) && rebuilt, "a source's time: built, fresh");
        if (edit_file(dir, "conditions.csv", "0.032,0,right", "0.064,0,right")) CHECK(0, "edit");
        CHECK(check_pack_is_fresh(&p, p.text, p.text_len, &rebuilt) && rebuilt, "a source's bytes and time: built, fresh");
        dsg_join(path, sizeof path, dir, DSG_CHECK_PACK);
        remove(path);
        CHECK(check_pack_is_fresh(&p, p.text, p.text_len, &rebuilt) && rebuilt, "the pack gone: built, fresh");
        /* another pack in its place, of the same size */
        {
            char* d = NULL;
            size_t n = 0;
            if (read_all(path, &d, &n) == 0) {
                d[n / 2] = (char)(d[n / 2] ^ 1);
                yrt_sleep_ns(30000000);
                write_all(path, d, n);
                free(d);
            }
        }
        CHECK(check_pack_is_fresh(&p, p.text, p.text_len, &rebuilt) && rebuilt, "another pack in its place: built, fresh");
#if !defined(_WIN32)
        {
            /* another size at the same time */
            char* d = NULL;
            size_t n = 0;
            struct stat st;
            struct timespec t[2];
            dsg_join(path, sizeof path, dir, DSG_CHECK_PACK);
            stat(path, &st);
#if defined(__APPLE__)
            t[0] = st.st_mtimespec;
#else
            t[0] = st.st_mtim;
#endif
            t[1] = t[0];
            if (read_all(path, &d, &n) == 0) {
                write_now(path, d, n - 1);
                free(d);
            }
            utimensat(AT_FDCWD, path, t, 0);
        }
        CHECK(check_pack_is_fresh(&p, p.text, p.text_len, &rebuilt) && rebuilt, "a pack of another size at its time: built, fresh");
#endif
        free(other);
        dsg_project_close(&p);
    } else {
        CHECK(0, "copy rdk");
    }

    if (!dsg_is_file(YSP_DESIGNER_TEST_FONT)) {
        printf("  stroop skipped: no font at '%s'\n", YSP_DESIGNER_TEST_FONT);
    } else if (project("stroop", "cp_stroop", &p, dir, sizeof dir) == 0) {
        /* glyph runs: a text with other words, then the first again */
        char* other = (char*)malloc(p.text_len + 1);
        char* at;
        memcpy(other, p.text, p.text_len + 1);
        at = strstr(other, "\"Correct\"");
        CHECK(at != NULL, "stroop has its feedback text");
        if (at) at[2] = 'a';
        CHECK(check_pack_is_fresh(&p, p.text, p.text_len, &rebuilt) && rebuilt, "stroop: built, fresh");
        CHECK(check_pack_is_fresh(&p, other, p.text_len, &rebuilt) && rebuilt, "another text's glyph runs: built, fresh");
        CHECK(check_pack_is_fresh(&p, p.text, p.text_len, &rebuilt) && rebuilt, "the first again: built, fresh");
        CHECK(check_pack_is_fresh(&p, p.text, p.text_len, &rebuilt) && !rebuilt, "the same: skipped, fresh");
        dsg_join(path, sizeof path, dir, "fonts/NotoSans-Regular.ttf");
        yrt_sleep_ns(30000000);
        copy_file(path, path);
        CHECK(check_pack_is_fresh(&p, p.text, p.text_len, &rebuilt) && rebuilt, "the font's time: built, fresh");
        free(other);
        dsg_project_close(&p);
    } else {
        CHECK(0, "copy stroop");
    }

#if !defined(_WIN32)
    /* racy: a source written again in the clock step of the last build, at
     * the same size, keeps its time; no key may cover it */
    if (project("rdk", "cp_racy", &p, dir, sizeof dir) == 0) {
        char* d = NULL;
        size_t n = 0;
        struct stat st;
        char* at;
        dsg_join(path, sizeof path, dir, "conditions.csv");
        read_all(path, &d, &n);
        write_now(path, d, n);
        CHECK(check_pack_is_fresh(&p, p.text, p.text_len, &rebuilt) && rebuilt, "a source written now: built, fresh");
        stat(path, &st);
        at = d ? strstr(d, "0.032,0,right") : NULL;
        if (at) at[3] = '6';
        write_now(path, d, n);
        {
            struct timespec t[2];
#if defined(__APPLE__)
            t[0] = st.st_mtimespec;
#else
            t[0] = st.st_mtim;
#endif
            t[1] = t[0];
            utimensat(AT_FDCWD, path, t, 0);
        }
        CHECK(at && check_pack_is_fresh(&p, p.text, p.text_len, &rebuilt) && rebuilt,
              "other bytes of the same size at the same time: built, fresh");
        free(d);
        dsg_project_close(&p);
    } else {
        CHECK(0, "copy rdk");
    }
#endif

    /* through the check: a forget (a project open again) costs no build */
    if (project("rdk", "cp_check", &p, dir, sizeof dir) == 0) {
        dsg_checker* c = dsg_checker_new();
        dsg_report r;
        int64_t t0;
        memset(&r, 0, sizeof r);
        dsg_join(path, sizeof path, dir, DSG_CHECK_PACK);
        CHECK(c && dsg_check(c, &p, p.text, p.text_len, NULL, &r) == 0 && r.built && r.n_errors == 0, "the first check builds");
        t0 = file_id(path);
        dsg_checker_forget(c);
        CHECK(dsg_check(c, &p, p.text, p.text_len, NULL, &r) == 0 && r.built && r.have_pack && r.n_errors == 0,
              "after a forget: a pack, no error");
        CHECK(file_id(path) == t0, "and the pack file was kept, not built again");
        dsg_checker_free(c);
        dsg_project_close(&p);
    } else {
        CHECK(0, "copy rdk");
    }
}

/* --- 9. two builds at once ----------------------------------------------------------------- */

static void test_two_at_once(void) {
    const char* proj = dsg_is_file(YSP_DESIGNER_TEST_FONT) ? "stroop" : "oddball";
    char dir[DSG_PATH_MAX], out[DSG_PATH_MAX], other[DSG_PATH_MAX], err[1024], id[72];
    dsg_project p;
    memset(&p, 0, sizeof p);
    dsg_builder* b;
    int k;
    printf("9. a Build and a check pack at once (%s)\n", proj);
    if (project(proj, "two", &p, dir, sizeof dir)) { CHECK(0, "copy %s", proj); return; }
    b = dsg_builder_start(NULL, err, sizeof err);
    dsg_build_path(&p, out, sizeof out);
    dsg_join(other, sizeof other, dir, "ypak.ysppak");
    CHECK(ypak_build(p.desc_path, other) == 0, "ypak build");
    pack_id_of(other, id);
    for (k = 0; k < 4 && b; k++) {
        const dsg_build_result* r = NULL;
        bool rebuilt = false;
        char path[DSG_PATH_MAX];
        /* a cold cache at each round, so both derive */
        dsg_join(path, sizeof path, dir, DSG_DIR);
        dsg_remove_tree(dir, DSG_DIR, err, sizeof err);
        CHECK(dsg_build_post(b, &p, out, (uint64_t)k, err, sizeof err) == 0, "posted");
        CHECK(check_pack_is_fresh(&p, p.text, p.text_len, &rebuilt), "round %d: the check pack is fresh", k);
        CHECK(dsg_build_wait(b, kWait) == 0 && dsg_build_take(b, &r) && r && r->status == DSG_BUILD_OK && strcmp(r->pack_id, id) == 0,
              "round %d: the Build has ypak's ID", k);
    }
    dsg_builder_stop(b);
    dsg_project_close(&p);
}

int main(int argc, char** argv) {
    char err[512];
    dsg_builder* b;
    if (argc < 3) {
        fprintf(stderr, "usage: test_designer_build WORK YPAK\n");
        return 2;
    }
    snprintf(work, sizeof work, "%s", argv[1]);
    ypak = argv[2];
    dsg_remove_tree(work, "b", err, sizeof err);
    {
        char w[DSG_PATH_MAX];
        dsg_make_dir(work, err, sizeof err);
        dsg_join(w, sizeof w, work, "b");
        dsg_make_dir(w, err, sizeof err);
        snprintf(work, sizeof work, "%s", w);
    }
    b = dsg_builder_start(NULL, err, sizeof err);
    CHECK(b != NULL, "the builder: %s", err);
    if (b) {
        test_pack_ids(b);
        test_issues(b);
        test_bad_desc(b);
        test_saved(b);
        test_stale(b);
        dsg_builder_stop(b);
    }
    test_newest();
#if !defined(_WIN32)
    test_cancel_running();
#else
    printf("6. skipped on Windows (the stand-in ffmpeg is a shell script)\n");
#endif
    test_check_pack();
    test_two_at_once();
    printf("test_designer_build: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
