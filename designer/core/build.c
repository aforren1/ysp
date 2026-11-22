/* build.c - Build: the pack tool on a thread of its own, and the check
 * pack's build through the entry cache (dsg.h, step 6; docs/designer.md,
 * "Build" and "Part B as built"). OWNED BY PART B.
 *
 * The thread is a yrt_pump, as the check thread's (worker.c): its messages
 * are only wake-ups, and the job lives in a slot, so a post while one waits
 * replaces it (newest wins). A Build that runs goes to its end unless
 * dsg_build_cancel() or dsg_builder_stop() cancels it: the pack tool reads
 * the cancel before each entry (ypt_options.cancel).
 *
 * Only the Build section of dsg.h is defined here as external symbols:
 * tests/designer/build_run_double.c stands in for this whole file in
 * test_designer_ui. The file stamps (a stat) and the absolute path are
 * here, not in platform.c, because dsg.h 0.4.0 is frozen and has neither. */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#if !defined(_WIN32) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE 1   /* struct stat's st_mtim and realpath() under -std=c11 */
#endif
#include "dsg.h"
#include "ysp/json.h"
#include "ysp/pack.h"
#include "ysp/pack_tool.h"
#include "ysp/rt.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

#if defined(YRT_NO_THREADS) || (defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__))
#define DSG_BUILD_NO_THREADS 1
#endif

/* The entry cache's bound, decision 17's knob. The cache must hold the
 * records of the pack it builds, or each build misses: with least recently
 * used trimming and a working set larger than the bound, every build
 * derives its media again (a 10 s 1080p video: 3.5 s in place of a 0.24 s
 * hit). The working set is about the pack's media, so the bound is twice
 * the pack's size (a changed source's old record sits beside the new one
 * until a trim), and never below a floor that keeps the records of earlier
 * versions (an option tried and undone) for a while. The trim runs after
 * the build, with the size of the pack it made: the tool's own trim
 * (ypt_options.cache_max) runs before the pack exists, so a first build
 * with more new media than the last pack's bound would remove its own
 * records, and the next build would derive them again. docs/designer.md,
 * "Part B as built", has the measurements. */
#define DSG_BUILD_CACHE_FLOOR ((int64_t)1 << 30)

/* --- files: a stamp and an absolute path ----------------------------------- */

typedef struct fstamp {
    int64_t size, mtime_ns;
} fstamp;

#if defined(_WIN32)
static wchar_t* widen(const char* s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t* w = n > 0 ? (wchar_t*)malloc((size_t)n * sizeof *w) : NULL;
    if (w && MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n) != n) {
        free(w);
        w = NULL;
    }
    return w;
}

/* false: missing (or no access); size is then -1. */
static bool file_stamp(const char* path, fstamp* out) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    wchar_t* w = widen(path);
    BOOL ok;
    uint64_t t;
    memset(&a, 0, sizeof a);
    ok = w ? GetFileAttributesExW(w, GetFileExInfoStandard, &a) : FALSE;
    free(w);
    memset(out, 0, sizeof *out);
    out->size = -1;
    if (!ok) return false;
    out->size = (int64_t)(((uint64_t)a.nFileSizeHigh << 32) | a.nFileSizeLow);
    t = ((uint64_t)a.ftLastWriteTime.dwHighDateTime << 32) | a.ftLastWriteTime.dwLowDateTime;
    /* 100 ns from 1601 to ns from 1970 */
    out->mtime_ns = ((int64_t)t - 116444736000000000LL) * 100;
    return true;
}

static int abs_folder(const char* folder, char* out, size_t cap) {
    wchar_t* w = widen(folder);
    wchar_t* full = NULL;
    DWORD n = w ? GetFullPathNameW(w, 0, NULL, NULL) : 0;
    int k = 0;
    if (n > 0) full = (wchar_t*)malloc((size_t)n * sizeof *full);
    if (full && GetFullPathNameW(w, n, full, NULL) < n)
        k = WideCharToMultiByte(CP_UTF8, 0, full, -1, out, cap > (size_t)INT_MAX ? INT_MAX : (int)cap, NULL, NULL);
    free(w);
    free(full);
    return k > 0 ? 0 : -1;
}
#else
static bool file_stamp(const char* path, fstamp* out) {
    struct stat st;
    memset(out, 0, sizeof *out);
    out->size = -1;
    if (stat(path, &st) != 0) return false;
    out->size = (int64_t)st.st_size;
#if defined(__APPLE__)
    out->mtime_ns = (int64_t)st.st_mtimespec.tv_sec * 1000000000LL + (int64_t)st.st_mtimespec.tv_nsec;
#else
    out->mtime_ns = (int64_t)st.st_mtim.tv_sec * 1000000000LL + (int64_t)st.st_mtim.tv_nsec;
#endif
    return true;
}

static int abs_folder(const char* folder, char* out, size_t cap) {
    char* r = realpath(folder, NULL);
    int k;
    if (!r) return -1;
    k = snprintf(out, cap, "%s", r);
    free(r);
    return k < 0 || (size_t)k >= cap ? -1 : 0;
}
#endif

/* The last '/' or '\\' of path; NULL: none. */
static const char* last_slash(const char* path) {
    const char* s1 = strrchr(path, '/');
    const char* s2 = strrchr(path, '\\');
    return s1 > s2 ? s1 : s2;
}

/* The folder that holds path ("." when it has none). */
static void parent_of(const char* path, char* out, size_t cap) {
    const char* s = last_slash(path);
    if (!s) snprintf(out, cap, ".");
    else if (s == path) snprintf(out, cap, "/");
    else snprintf(out, cap, "%.*s", (int)(s - path), path);
}

/* --- the sources that a description names --------------------------------- */

/* Calls fn for the experiment's source and each resource's "source" and
 * "from" (a curveset's font), in the description's order. -1 when the
 * description does not parse, or fn's -1. */
typedef int (*source_fn)(void* ctx, const char* rel);
static int each_source(const char* desc, size_t n, source_fn fn, void* ctx) {
    yjs_arena a;
    yjs_value* root = NULL;
    yjs_error e;
    const yjs_value* v;
    size_t i;
    int rc = 0;
    yjs_arena_init_heap(&a, 0);
    if (yjs_parse(&a, desc, n, NULL, &root, &e) != YJS_OK || !root || root->type != YJS_OBJECT) {
        yjs_arena_free(&a);
        return -1;
    }
    v = yjs_get(root, "experiment");
    v = v && v->type == YJS_OBJECT ? yjs_get(v, "source") : NULL;
    if (v && v->type == YJS_STRING) rc = fn(ctx, v->u.s);
    v = yjs_get(root, "resources");
    for (i = 0; rc == 0 && v && v->type == YJS_ARRAY && i < v->n; i++) {
        const yjs_value* r = yjs_at(v, i);
        const yjs_value* s = r && r->type == YJS_OBJECT ? yjs_get(r, "source") : NULL;
        const yjs_value* f = r && r->type == YJS_OBJECT ? yjs_get(r, "from") : NULL;
        if (s && s->type == YJS_STRING) rc = fn(ctx, s->u.s);
        if (rc == 0 && f && f->type == YJS_STRING) rc = fn(ctx, f->u.s);
    }
    yjs_arena_free(&a);
    return rc;
}

/* A resource's source by the name that a message gives it: its "name" as
 * written, or its source. false: no such resource. */
static bool resource_source(const char* desc, size_t n, const char* name, char* source, size_t cap) {
    yjs_arena a;
    yjs_value* root = NULL;
    yjs_error e;
    const yjs_value* v;
    size_t i;
    bool found = false;
    if (!desc) return false;
    yjs_arena_init_heap(&a, 0);
    if (yjs_parse(&a, desc, n, NULL, &root, &e) == YJS_OK && root && root->type == YJS_OBJECT) {
        v = yjs_get(root, "resources");
        for (i = 0; !found && v && v->type == YJS_ARRAY && i < v->n; i++) {
            const yjs_value* r = yjs_at(v, i);
            const yjs_value* s = r && r->type == YJS_OBJECT ? yjs_get(r, "source") : NULL;
            const yjs_value* nm = r && r->type == YJS_OBJECT ? yjs_get(r, "name") : NULL;
            if (!s || s->type != YJS_STRING) continue;
            if ((nm && nm->type == YJS_STRING && strcmp(nm->u.s, name) == 0) || strcmp(s->u.s, name) == 0) {
                snprintf(source, cap, "%s", s->u.s);
                found = true;
            }
        }
    }
    yjs_arena_free(&a);
    return found;
}

/* --- a result's storage ------------------------------------------------------- */

static int grow(void** p, size_t* cap, size_t need, size_t elem) {
    size_t nc;
    void* np;
    if (need <= *cap) return 0;
    nc = *cap ? *cap : 16;
    while (nc < need) nc *= 2;
    np = realloc(*p, nc * elem);
    if (!np) return -1;
    *p = np;
    *cap = nc;
    return 0;
}

/* One result and the storage it points into, kept between builds, so a
 * build allocates only when a buffer grows. Strings go in by offset and
 * become pointers at the end (the buffer moves while it grows). */
typedef struct build_out {
    dsg_build_result  pub;
    dsg_issue*        issues;
    size_t            c_issues;
    size_t*           msg_off;
    size_t            c_msg;
    dsg_build_source* src;
    size_t            c_src;
    size_t*           src_off;
    size_t            c_src_off;
    char*             strs;
    size_t            n_strs, c_strs;
    char              path[DSG_PATH_MAX];
} build_out;

static size_t put_str(build_out* o, const char* s) {
    size_t n = strlen(s) + 1, at;
    if (grow((void**)&o->strs, &o->c_strs, o->n_strs + n, 1)) return SIZE_MAX;
    at = o->n_strs;
    memcpy(o->strs + at, s, n);
    o->n_strs += n;
    return at;
}

static void out_reset(build_out* o) {
    memset(&o->pub, 0, sizeof o->pub);
    o->pub.report.status = DSG_CHECK_NONE;
    o->pub.path = o->path;
    o->n_strs = 0;
}

static void out_free(build_out* o) {
    free(o->issues);
    free(o->msg_off);
    free(o->src);
    free(o->src_off);
    free(o->strs);
}

/* --- the issues: the pack tool's lines, with their places ------------------- */

/* The definition's place that a message gives as "line N, column M", as
 * the check's place_in_message() (check.c) reads it; the source
 * description's lines and the manifest's are not the definition's. A
 * table's place ("line L, row R, column 'NAME' (C)") stays in the message,
 * where the tables view reads it (dsg_csv_place()). */
static void message_place(const char* msg, const char* def, size_t n, dsg_place* out) {
    const char* s = msg;
    unsigned l = 0, col = 0;
    memset(out, 0, sizeof *out);
    if (strncmp(msg, "the source description", 22) == 0 || strncmp(msg, "the manifest", 12) == 0) return;
    while ((s = strstr(s, "line ")) != NULL && sscanf(s, "line %u, column %u", &l, &col) != 2) s += 5;
    if (s && l > 0) {
        out->line = l;
        out->column = col;
        out->offset = def ? dsg_offset_of(def, n, l, col) : 0;
    }
}

typedef struct issue_ctx {
    build_out*  o;
    const char* def_path;     /* read at the first message with a line   */
    char*       def;
    size_t      def_n;
    bool        def_read;
    const char* desc;
    size_t      desc_n;
} issue_ctx;

static void add_issue(issue_ctx* x, int sev, const char* msg) {
    build_out* o = x->o;
    const size_t k = (size_t)(unsigned)o->pub.report.n_issues;
    dsg_issue* is;
    char text[2048];
    if (grow((void**)&o->issues, &o->c_issues, k + 1, sizeof *o->issues) ||
        grow((void**)&o->msg_off, &o->c_msg, k + 1, sizeof *o->msg_off))
        return;
    /* the tool's lines start with its program's name, which says nothing
     * in the designer: the Issues panel says "build" */
    if (strncmp(msg, "ypak: ", 6) == 0) msg += 6;
    snprintf(text, sizeof text, "%s", msg);
    /* "'NAME': ..." of a resource gets its source, since the designer has no
     * editor for a font, an image or a video ("Build") */
    if (text[0] == '\'') {
        const char* q = strchr(text + 1, '\'');
        char name[256], src[512];
        if (q && (size_t)(q - text - 1) < sizeof name) {
            memcpy(name, text + 1, (size_t)(q - text - 1));
            name[q - text - 1] = 0;
            if (resource_source(x->desc, x->desc_n, name, src, sizeof src) && strcmp(src, name) != 0 && !strstr(text, src)) {
                size_t m = strlen(text);
                snprintf(text + m, sizeof text - m, " (source '%s')", src);
            }
        }
    }
    if (strstr(msg, "line ") && !x->def_read) {
        char err[256];
        x->def_read = true;
        if (dsg_read_file(x->def_path, &x->def, &x->def_n, err, sizeof err)) x->def = NULL;
    }
    is = &o->issues[k];
    memset(is, 0, sizeof *is);
    is->severity = sev;
    is->from = DSG_FROM_BUILD;
    message_place(msg, x->def, x->def_n, &is->place);
    o->msg_off[k] = put_str(o, text);
    if (o->msg_off[k] == SIZE_MAX) return;
    o->pub.report.n_issues++;
    if (sev == DSG_SEV_ERROR) o->pub.report.n_errors++;
}

/* The warnings that the tool wrote to f, one a line ("warning: ..."). */
static void add_warnings(issue_ctx* x, FILE* f) {
    char line[2048];
    if (!f) return;
    rewind(f);
    while (fgets(line, sizeof line, f)) {
        size_t m = strlen(line);
        const char* s = line;
        if (m == sizeof line - 1 && line[m - 1] != '\n') {
            /* longer than the buffer: the rest of the line is dropped */
            int c;
            while ((c = fgetc(f)) != EOF && c != '\n') {
            }
        }
        while (m > 0 && (line[m - 1] == '\n' || line[m - 1] == '\r')) line[--m] = 0;
        if (m == 0) continue;
        if (strncmp(s, "warning: ", 9) == 0) s += 9;
        add_issue(x, DSG_SEV_WARNING, s);
    }
}

/* The pointers last; errors first (dsg_report's order). */
static void out_finish(build_out* o) {
    int i, j, sev;
    const int n = o->pub.report.n_issues;
    for (i = 0; i < n; i++) o->issues[i].message = o->strs + o->msg_off[i];
    for (i = 0; i < o->pub.n_sources; i++) o->src[i].path = o->strs + o->src_off[i];
    /* a stable sort by severity, in place: n is a handful */
    for (sev = DSG_SEV_ERROR, j = 0; sev <= DSG_SEV_NOTE; sev++)
        for (i = j; i < n; i++)
            if (o->issues[i].severity == sev) {
                dsg_issue t = o->issues[i];
                memmove(&o->issues[j + 1], &o->issues[j], (size_t)(i - j) * sizeof t);
                o->issues[j++] = t;
            }
    o->pub.report.issues = n > 0 ? o->issues : NULL;
    o->pub.sources = o->pub.n_sources > 0 ? o->src : NULL;
}

/* --- the stamp ------------------------------------------------------------------ */

typedef struct stamp_ctx {
    build_out*  o;
    const char* base;    /* the sources' folder, absolute */
} stamp_ctx;

static int stamp_one(void* ctx, const char* rel) {
    stamp_ctx* s = (stamp_ctx*)ctx;
    build_out* o = s->o;
    const size_t k = (size_t)(unsigned)o->pub.n_sources;
    char path[DSG_PATH_MAX];
    fstamp st = { 0, 0 };
    if (dsg_join(path, sizeof path, s->base, rel)) return 0;   /* too long: the build says so */
    if (grow((void**)&o->src, &o->c_src, k + 1, sizeof *o->src) || grow((void**)&o->src_off, &o->c_src_off, k + 1, sizeof *o->src_off))
        return -1;
    file_stamp(path, &st);
    o->src[k].path = NULL;
    o->src[k].size = st.size;
    o->src[k].mtime_ns = st.size < 0 ? 0 : st.mtime_ns;
    o->src_off[k] = put_str(o, path);
    if (o->src_off[k] == SIZE_MAX) return -1;
    o->pub.n_sources++;
    return 0;
}

/* The description itself, then each source it names, from the
 * description's folder (ypt_build()'s sources' folder). */
static void take_stamp(build_out* o, const char* desc_path, const char* desc, size_t n) {
    char dir[DSG_PATH_MAX], base[DSG_PATH_MAX];
    const char* sl = last_slash(desc_path);
    stamp_ctx s;
    parent_of(desc_path, dir, sizeof dir);
    if (abs_folder(dir, base, sizeof base)) snprintf(base, sizeof base, "%s", dir);
    s.o = o;
    s.base = base;
    if (stamp_one(&s, sl ? sl + 1 : desc_path) == 0 && desc) each_source(desc, n, stamp_one, &s);
}

/* --- the builder ------------------------------------------------------------------ */

typedef struct build_job {
    uint64_t revision;
    int32_t  gen;                       /* the post's: a cancel covers it when
                                         * cancel_gen has reached it         */
    char     folder[DSG_PATH_MAX];
    char     desc_path[DSG_PATH_MAX];
    char     def_path[DSG_PATH_MAX];
    char     out_path[DSG_PATH_MAX];
    struct dsg_builder* b;
} build_job;

struct dsg_builder {
    dsg_builder_desc desc;
#if !defined(DSG_BUILD_NO_THREADS)
    yrt_pump         pump;
#endif
    bool             threaded;
    uint32_t         last_seq;           /* the poster's last wake           */
    volatile int32_t cancel_gen;         /* atomic: the build thread reads it */

    /* --- under the pump's publish lock --- */
    int32_t          post_gen;
    bool             have_wait;
    bool             running;
    bool             have_out;
    bool             stopping;
    build_job        wait, run;
    build_out*       out_work;           /* the thread fills it              */
    build_out*       out_ready;
    build_out*       out_ui;             /* the UI's, until its next take    */
    build_out        outs[3];
};

#if !defined(DSG_BUILD_NO_THREADS)
static void lock(dsg_builder* b) {
    if (b->threaded) yrt_pump_lock(&b->pump);
}
static void unlock(dsg_builder* b) {
    if (b->threaded) yrt_pump_unlock(&b->pump);
}
#else
static void lock(dsg_builder* b) { (void)b; }
static void unlock(dsg_builder* b) { (void)b; }
#endif

/* ypt_options.cancel, on the build thread. */
static int job_canceled(void* ctx) {
    const build_job* j = (const build_job*)ctx;
    const uint32_t c = (uint32_t)dsg_atomic_load(&j->b->cancel_gen);
    return (int32_t)(c - (uint32_t)j->gen) >= 0;
}

static int64_t cache_bound(const char* pack_path) {
    fstamp st = { 0, 0 };
    int64_t twice;
    if (!file_stamp(pack_path, &st) || st.size <= 0) return DSG_BUILD_CACHE_FLOOR;
    twice = st.size > INT64_MAX / 2 ? INT64_MAX : 2 * st.size;
    return twice > DSG_BUILD_CACHE_FLOOR ? twice : DSG_BUILD_CACHE_FLOOR;
}

/* After each build, the least recently used records go down to the bound
 * of the pack at pack_path (the new one; after a failed build, the one
 * before it). A trim lists the folder: about 0.9 us a record (82 us for
 * 100), beside a build that hashes every source. Whether this build wrote a
 * record cannot be told cheaply: the folder's time moves in steps of 1 to
 * 4 ms here, and a build of a small project takes less. */
static void cache_trim_after(const char* cache, const char* pack_path) {
    char err[256];
    (void)ypt_cache_trim(cache, cache_bound(pack_path), NULL, err, sizeof err);
}

/* The tool's warnings: C's tmpfile(), else a file in the project's DSG_DIR
 * (a system whose tmpfile() has no folder it may write). */
static FILE* warn_file(const char* folder, char* path, size_t cap) {
    FILE* f = tmpfile();
    path[0] = 0;
    if (f) return f;
    if (dsg_join(path, cap, folder, DSG_DIR "/build-warnings.txt")) {
        path[0] = 0;
        return NULL;
    }
    f = fopen(path, "w+b");
    if (!f) path[0] = 0;
    return f;
}

static void run_build(build_job* j, build_out* o) {
    const int64_t t0 = yrt_now_ns();
    char err[2048], cache[DSG_PATH_MAX], out_dir[DSG_PATH_MAX], wpath[DSG_PATH_MAX];
    char* desc = NULL;
    size_t desc_n = 0;
    ypt_options opt;
    issue_ctx x;
    FILE* wf;
    int rc;
    out_reset(o);
    o->pub.revision = j->revision;
    snprintf(o->path, sizeof o->path, "%s", j->out_path);
    memset(&x, 0, sizeof x);
    x.o = o;
    x.def_path = j->def_path;
    err[0] = 0;
    /* the stamp before the build: a source that changes during it makes
     * the result stale, never the other way */
    if (dsg_read_file(j->desc_path, &desc, &desc_n, err, sizeof err)) desc = NULL;
    x.desc = desc;
    x.desc_n = desc_n;
    take_stamp(o, j->desc_path, desc, desc_n);
    if (!desc) {
        o->pub.status = DSG_BUILD_FAILED;
        add_issue(&x, DSG_SEV_ERROR, err);
        goto done;
    }
    parent_of(j->out_path, out_dir, sizeof out_dir);
    if (dsg_join(cache, sizeof cache, j->folder, DSG_CACHE_DIR) || dsg_make_dir(out_dir, err, sizeof err)) {
        o->pub.status = DSG_BUILD_FAILED;
        add_issue(&x, DSG_SEV_ERROR, err[0] ? err : "the project's path is too long");
        goto done;
    }
    wf = warn_file(j->folder, wpath, sizeof wpath);
    memset(&opt, 0, sizeof opt);
    opt.warn = wf;
    opt.cancel = job_canceled;
    opt.cancel_ctx = j;
    opt.cache = cache;
    rc = ypt_build(j->desc_path, j->out_path, &opt, err, sizeof err);
    cache_trim_after(cache, j->out_path);
    add_warnings(&x, wf);
    if (wf) fclose(wf);
    if (wpath[0]) remove(wpath);
    if (rc == YPT_ERR_CANCELED) {
        o->pub.status = DSG_BUILD_CANCELED;
    } else if (rc != 0) {
        o->pub.status = DSG_BUILD_FAILED;
        add_issue(&x, DSG_SEV_ERROR, err[0] ? err : "the pack tool failed and gave no reason");
    } else {
        ypak_pack pk;
        ypak_desc d;
        fstamp st = { 0, 0 };
        memset(&pk, 0, sizeof pk);
        memset(&d, 0, sizeof d);
        d.path = j->out_path;
        d.verify = YPAK_VERIFY_OPEN;
        if (!ypak_open(&pk, &d)) {
            o->pub.status = DSG_BUILD_FAILED;
            snprintf(err, sizeof err, "%.1500s: the pack just built does not open: %s", j->out_path, ypak_error(&pk));
            add_issue(&x, DSG_SEV_ERROR, err);
        } else {
            ypak_id(&pk, o->pub.pack_id);
            ypak_close(&pk);
            o->pub.status = DSG_BUILD_OK;
            o->pub.size = file_stamp(j->out_path, &st) ? st.size : 0;
        }
    }
done:
    free(x.def);
    free(desc);
    o->pub.ms = (double)(yrt_now_ns() - t0) / 1e6;
    o->pub.done_unix_s = (int64_t)time(NULL);
    out_finish(o);
}

/* Every job that waits; one at a time, the newest. */
static void run_jobs(dsg_builder* b) {
    for (;;) {
        build_out* o;
        lock(b);
        if (b->stopping || !b->have_wait) {
            unlock(b);
            return;
        }
        b->run = b->wait;
        b->have_wait = false;
        b->running = true;
        o = b->out_work;
        unlock(b);
        run_build(&b->run, o);
        lock(b);
        b->out_work = b->out_ready;
        b->out_ready = o;
        b->have_out = true;
        b->running = false;
        unlock(b);
        if (b->desc.wake) b->desc.wake(b->desc.ctx);
    }
}

#if !defined(DSG_BUILD_NO_THREADS)
static void on_msg(void* ctx, const void* msg, uint32_t seq) {
    (void)msg;
    (void)seq;
    run_jobs((dsg_builder*)ctx);
}
#endif

dsg_builder* dsg_builder_start(const dsg_builder_desc* d, char* err, size_t cap) {
    dsg_builder* b = (dsg_builder*)calloc(1, sizeof *b);
    int i;
    if (!b) {
        snprintf(err, cap, "out of memory for the builder");
        return NULL;
    }
    if (d) b->desc = *d;
    for (i = 0; i < 3; i++) out_reset(&b->outs[i]);
    b->out_work = &b->outs[0];
    b->out_ready = &b->outs[1];
    b->out_ui = &b->outs[2];
    b->wait.b = b;
    b->run.b = b;
#if !defined(DSG_BUILD_NO_THREADS)
    if (!b->desc.no_thread) {
        yrt_pump_desc pd;
        memset(&pd, 0, sizeof pd);
        pd.msg_size = 1;
        pd.capacity = 16;
        pd.on_msg = on_msg;
        pd.ctx = b;
        /* a stop drops the waiting wake-ups: its job is dropped too */
        pd.drop_on_stop = true;
        if (!yrt_pump_open(&b->pump, &pd)) {
            snprintf(err, cap, "the build thread: %s", yrt_pump_error(&b->pump));
            free(b);
            return NULL;
        }
        b->threaded = true;
    }
#endif
    return b;
}

void dsg_builder_stop(dsg_builder* b) {
    int i;
    if (!b) return;
    dsg_build_cancel(b);
    lock(b);
    b->stopping = true;
    unlock(b);
#if !defined(DSG_BUILD_NO_THREADS)
    if (b->threaded) yrt_pump_close(&b->pump);
#endif
    b->threaded = false;
    for (i = 0; i < 3; i++) out_free(&b->outs[i]);
    free(b);
}

/* Decision 1: PROJECT/build/NAME.ysppak, NAME the folder's last name; for
 * a folder given as "." or "..", the real folder's name. */
int dsg_build_path(const dsg_project* p, char* out, size_t cap) {
    const char* f = p->folder;
    const char* base = f;
    char abs[DSG_PATH_MAX];
    size_t n = strlen(f), m;
    const char* name;
    int k;
    while (n > 1 && (f[n - 1] == '/' || f[n - 1] == '\\')) n--;
    name = f + n;
    while (name > f && name[-1] != '/' && name[-1] != '\\') name--;
    m = (size_t)(f + n - name);
    if (m == 0 || (m == 1 && name[0] == '.') || (m == 2 && name[0] == '.' && name[1] == '.')) {
        size_t a;
        if (abs_folder(f, abs, sizeof abs)) return -1;
        a = strlen(abs);
        while (a > 1 && (abs[a - 1] == '/' || abs[a - 1] == '\\')) a--;
        base = abs + a;
        while (base > abs && base[-1] != '/' && base[-1] != '\\') base--;
        m = (size_t)(abs + a - base);
        if (m == 0) return -1;
        name = base;
    }
    k = snprintf(out, cap, "%.*s/%s/%.*s.ysppak", (int)n, f, DSG_BUILD_DIR, (int)m, name);
    return k < 0 || (size_t)k >= cap ? -1 : 0;
}

int dsg_build_post(dsg_builder* b, const dsg_project* p, const char* out_path, uint64_t revision, char* err, size_t cap) {
    build_job* j;
    if (!b || !p || !p->folder[0] || !p->desc_path[0]) {
        snprintf(err, cap, "no project to build");
        return -1;
    }
    if (!out_path || !out_path[0] || strlen(out_path) >= sizeof b->wait.out_path) {
        snprintf(err, cap, "no path for the pack, or one too long");
        return -1;
    }
    lock(b);
    j = &b->wait;
    j->revision = revision;
    j->gen = ++b->post_gen;
    snprintf(j->folder, sizeof j->folder, "%s", p->folder);
    snprintf(j->desc_path, sizeof j->desc_path, "%s", p->desc_path);
    snprintf(j->def_path, sizeof j->def_path, "%s", p->def_path);
    snprintf(j->out_path, sizeof j->out_path, "%s", out_path);
    b->have_wait = true;
    unlock(b);
#if !defined(DSG_BUILD_NO_THREADS)
    if (b->threaded) {
        const int seq = yrt_pump_submit(&b->pump, "b");
        /* Full: a wake that has not started yet is in the ring, and it
         * takes this job too, so last_seq still covers it. */
        if (seq > 0) b->last_seq = (uint32_t)seq;
    }
#endif
    return 0;
}

void dsg_build_cancel(dsg_builder* b) {
    if (!b) return;
    lock(b);
    /* every job posted so far; the only writer, under the lock */
    dsg_atomic_add(&b->cancel_gen, (int32_t)((uint32_t)b->post_gen - (uint32_t)dsg_atomic_load(&b->cancel_gen)));
    b->have_wait = false;
    unlock(b);
}

bool dsg_build_busy(const dsg_builder* b) {
    dsg_builder* m = (dsg_builder*)(uintptr_t)b;   /* the lock, not the state */
    bool busy;
    if (!m) return false;
    lock(m);
    busy = m->have_wait || m->running;
    unlock(m);
    return busy;
}

bool dsg_build_take(dsg_builder* b, const dsg_build_result** out) {
    bool got = false;
    if (out) *out = NULL;
    if (!b) return false;
    lock(b);
    if (b->have_out) {
        build_out* t = b->out_ui;
        b->out_ui = b->out_ready;
        b->out_ready = t;
        b->have_out = false;
        got = true;
    }
    unlock(b);
    if (got && out) *out = &b->out_ui->pub;
    return got;
}

int dsg_build_wait(dsg_builder* b, int64_t timeout_ns) {
    if (!b) return 0;
    if (!b->threaded) {
        run_jobs(b);
        return 0;
    }
#if !defined(DSG_BUILD_NO_THREADS)
    if (b->last_seq == 0) return 0;
    return yrt_pump_wait(&b->pump, b->last_seq, timeout_ns) == YRT_OK ? 0 : -1;
#else
    (void)timeout_ns;
    return 0;
#endif
}

bool dsg_build_stale(const dsg_project* p, const dsg_build_result* r, uint64_t revision) {
    char want[DSG_PATH_MAX];
    fstamp st = { 0, 0 };
    int i;
    if (!r || r->status != DSG_BUILD_OK || r->revision != revision || !r->path) return true;
    /* a result of another project, or of another path */
    if (p && (dsg_build_path(p, want, sizeof want) || strcmp(want, r->path) != 0)) return true;
    /* the pack itself: gone, or another one in its place */
    if (!file_stamp(r->path, &st) || st.size != r->size) return true;
    for (i = 0; i < r->n_sources; i++) {
        const dsg_build_source* s = &r->sources[i];
        if (!file_stamp(s->path, &st)) {
            if (s->size >= 0) return true;
        } else if (st.size != s->size || st.mtime_ns != s->mtime_ns) {
            return true;
        }
    }
    return false;
}

/* --- the check pack ------------------------------------------------------------ */

/* The check pack's build is skipped when its inputs are the last build's:
 * the description's bytes, and for each source it names either its bytes
 * (the designer's own copies under DSG_DIR: the text under check and the
 * unsaved tables, which check.c writes again before each build, so their
 * time says nothing) or its size and modification time (the project's
 * files). The definition's bytes are in the key, so a pack kept never has
 * another text's glyph runs. The key and the pack's own size and time go
 * to OUT.key after a build; a pack that another build replaced, or one
 * deleted, is built again.
 *
 * A size and a time can miss a change: a file written again within one
 * step of the file system's clock, at the same size, keeps its time. So, as
 * git does for its index, no key is kept when a source's time is within
 * DSG_BUILD_RACY_NS of the pack's own (one clock, the file system's): a
 * later write then has a later time. 2 s is FAT's step, the coarsest. */
#define SKIP_TAG "ysp-designer check-pack inputs 1"
#define DSG_BUILD_RACY_NS 2000000000LL

typedef struct skip_ctx {
    ypak_sha256_state h;
    const char*       folder;
    int64_t           newest;    /* the newest time of a source by stamp */
} skip_ctx;

static int skip_one(void* ctx, const char* rel) {
    skip_ctx* s = (skip_ctx*)ctx;
    char path[DSG_PATH_MAX];
    fstamp st = { 0, 0 };
    int64_t v[2];
    ypak_sha256_update(&s->h, rel, strlen(rel) + 1);
    if (dsg_join(path, sizeof path, s->folder, rel)) return -1;
    if (strncmp(rel, DSG_DIR "/", sizeof DSG_DIR) == 0) {
        char* d = NULL;
        size_t n = 0;
        char err[256];
        uint8_t sum[32];
        if (dsg_read_file(path, &d, &n, err, sizeof err)) return -1;
        ypak_sha256(d, n, sum);
        free(d);
        ypak_sha256_update(&s->h, sum, sizeof sum);
        return 0;
    }
    file_stamp(path, &st);
    v[0] = st.size;
    v[1] = st.size < 0 ? 0 : st.mtime_ns;
    if (st.size >= 0 && st.mtime_ns > s->newest) s->newest = st.mtime_ns;
    ypak_sha256_update(&s->h, v, sizeof v);
    return 0;
}

/* 0, the key and the newest source time, or -1: build, and keep no key. */
static int skip_key(const dsg_project* p, const char* desc, size_t n, uint8_t key[32], int64_t* newest) {
    skip_ctx s;
    const uint64_t len = (uint64_t)n;
    s.folder = p->folder;
    s.newest = INT64_MIN;
    ypak_sha256_init(&s.h);
    ypak_sha256_update(&s.h, SKIP_TAG, sizeof SKIP_TAG);
    ypak_sha256_update(&s.h, YPT_VERSION_STRING, sizeof YPT_VERSION_STRING);
    ypak_sha256_update(&s.h, &len, sizeof len);
    ypak_sha256_update(&s.h, desc, n);
    if (each_source(desc, n, skip_one, &s) != 0) return -1;
    ypak_sha256_final(&s.h, key);
    *newest = s.newest;
    return 0;
}

/* OUT.key's one line: the key's hex digits, the pack's size and time. */
static void skip_line(const uint8_t key[32], const fstamp* st, char* out, size_t cap) {
    static const char hx[] = "0123456789abcdef";
    char h[65];
    int i;
    for (i = 0; i < 32; i++) {
        h[2 * i] = hx[key[i] >> 4];
        h[2 * i + 1] = hx[key[i] & 15];
    }
    h[64] = 0;
    snprintf(out, cap, "%s %lld %lld\n", h, (long long)st->size, (long long)st->mtime_ns);
}

int dsg_build_check_pack(const dsg_project* p, const char* desc, size_t desc_len, const char* out_path, char* err, size_t cap) {
    char cache[DSG_PATH_MAX], key_path[DSG_PATH_MAX], line[160];
    uint8_t key[32] = { 0 };
    int64_t newest = INT64_MIN;
    bool have_key;
    ypt_options o;
    fstamp st = { 0, 0 };
    int k, rc;
    k = snprintf(key_path, sizeof key_path, "%s.key", out_path);
    if (dsg_join(cache, sizeof cache, p->folder, DSG_CACHE_DIR) || k < 0 || (size_t)k >= sizeof key_path) {
        snprintf(err, cap, "the project's path is too long");
        return -1;
    }
    have_key = skip_key(p, desc, desc_len, key, &newest) == 0;
    if (have_key && file_stamp(out_path, &st)) {
        char* old = NULL;
        size_t n = 0;
        char e2[64];
        skip_line(key, &st, line, sizeof line);
        if (dsg_read_file(key_path, &old, &n, e2, sizeof e2) == 0) {
            const bool same = n == strlen(line) && memcmp(old, line, n) == 0;
            free(old);
            if (same) return 0;
        }
    }
    memset(&o, 0, sizeof o);
    o.cache = cache;
    rc = ypt_build_text(desc, desc_len, p->folder, out_path, &o, err, cap);
    cache_trim_after(cache, out_path);
    if (rc != 0) return -1;
    if (have_key && file_stamp(out_path, &st) && (newest == INT64_MIN || newest < st.mtime_ns - DSG_BUILD_RACY_NS)) {
        char e2[256];
        skip_line(key, &st, line, sizeof line);
        (void)dsg_write_file(key_path, line, strlen(line), e2, sizeof e2);
    } else {
        remove(key_path);
    }
    return 0;
}
