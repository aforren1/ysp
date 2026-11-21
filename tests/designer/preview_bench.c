/* preview_bench.c - what the preview costs (designer/core/preview.c;
 * docs/designer.md, "Plan of step 5", "Redraw and cost"). Not a test: it
 * prints times. Headless: gfx's null backend, so the GL side (the attach's
 * uploads and compiles, the drawn frame's GPU work) is not here; part D
 * measures it on a context.
 *
 * Usage: bench_designer_preview WORK [FRAMES] [PROJECT]
 *   WORK: a folder for the projects' copies and their data sets.
 *   FRAMES: UI frames timed per case (default 1000).
 *   Projects: bimanual and stroop (tests/player; stroop needs
 *   YSP_DESIGNER_TEST_FONT), and "many", the generated worst case of
 *   bench_designer_check: 200 text stimuli in four scripts on at once, ten
 *   tables of 1,000 rows. Each line: median, p99 and max in microseconds.
 *     load     the session op's load on the check thread (pl_sim_load(),
 *              the data set's folder and writer), 20 times
 *     load_pin the same with a pin to the last leaf: the fast forward
 *     close    the session op's close (pl_close(): the end's frames, the
 *              writer's last rows, session.json; the old runs' removal)
 *     adopt    the UI frame that takes a session and attaches it
 *     tick60   dsg_preview_tick() at a 60 Hz UI with a 60 Hz display: one
 *              step a frame, drawn
 *     tick500  the same with a 500 Hz display: 8 or 9 steps, the last drawn
 *     step     pl_sim_frame(draw false), inside the ticks above
 *     draw     pl_sim_frame(draw true), inside the ticks above
 * The steps are timed through GNU ld's --wrap of pl_sim_frame().
 *
 * The scripted participant answers no trial (the preview's trials are the
 * person's), so a trial with a response runs to its deadline. */
#define _POSIX_C_SOURCE 200809L
#include "preview.h"
#include "doc.h"
#include "player.h"

#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef YSP_SOURCE_DIR
#define YSP_SOURCE_DIR "."
#endif
#ifndef YSP_DESIGNER_TEST_FONT
#define YSP_DESIGNER_TEST_FONT ""
#endif

static double now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e6 + (double)ts.tv_nsec / 1e3;
}

static int cmp_d(const void* a, const void* b) {
    double x = *(const double*)a, y = *(const double*)b;
    return x < y ? -1 : x > y;
}

static void report(const char* what, const char* name, double* t, int n) {
    int p99 = (n * 99 + 99) / 100 - 1;
    if (n <= 0) return;
    qsort(t, (size_t)n, sizeof *t, cmp_d);
    printf("  %-9s %-9s n %5d  median %9.1f  p99 %9.1f  max %9.1f us\n", what, name, n, t[n / 2], t[p99 < 0 ? 0 : p99], t[n - 1]);
    fflush(stdout);
}

static void loadavg(const char* when) {
    char b[128] = "";
    FILE* f = fopen("/proc/loadavg", "rb");
    if (f) {
        size_t k = fread(b, 1, sizeof b - 1, f);
        b[k] = 0;
        fclose(f);
    }
    printf("load average %s: %s", when, b[0] ? b : "?\n");
}

/* --- pl_sim_frame() timed ------------------------------------------------------------ */

static double* g_step;   /* draw false */
static double* g_draw;   /* draw true */
static int g_n_step, g_n_draw, g_cap;
static int g_timing;
int __real_pl_sim_frame(pl_player* p, bool draw);
int __wrap_pl_sim_frame(pl_player* p, bool draw) {
    double t0;
    int rc;
    if (!g_timing) return __real_pl_sim_frame(p, draw);
    t0 = now_us();
    rc = __real_pl_sim_frame(p, draw);
    if (draw && g_n_draw < g_cap) g_draw[g_n_draw++] = now_us() - t0;
    else if (!draw && g_n_step < g_cap) g_step[g_n_step++] = now_us() - t0;
    return rc;
}

/* --- the projects ------------------------------------------------------------------------ */

static int copy_file(const char* from, const char* to) {
    char* d = NULL;
    size_t n = 0;
    char err[512];
    if (dsg_read_file(from, &d, &n, err, sizeof err)) { printf("  %s\n", err); return -1; }
    if (dsg_write_file(to, d, n, err, sizeof err)) { printf("  %s\n", err); free(d); return -1; }
    free(d);
    return 0;
}

static int copy_project(const char* work, const char* name, char* out, size_t cap) {
    char src[DSG_PATH_MAX], a[DSG_PATH_MAX + 256], b[DSG_PATH_MAX + 256], err[512];
    DIR* dir;
    struct dirent* e;
    snprintf(src, sizeof src, "%s/tests/player/%s", YSP_SOURCE_DIR, name);
    if (dsg_join(out, cap, work, name) || dsg_make_dir(out, err, sizeof err)) return -1;
    dir = opendir(src);
    if (!dir) return -1;
    while ((e = readdir(dir)) != NULL) {
        if (dsg_join(a, sizeof a, src, e->d_name) || !dsg_is_file(a)) continue;
        if (dsg_join(b, sizeof b, out, e->d_name) || copy_file(a, b)) { closedir(dir); return -1; }
    }
    closedir(dir);
    return 0;
}

static void font_dir(char* out, size_t cap) {
    const char* s = strrchr(YSP_DESIGNER_TEST_FONT, '/');
    size_t k = s ? (size_t)(s - YSP_DESIGNER_TEST_FONT) + 1 : 0;
    if (k >= cap) k = 0;
    memcpy(out, YSP_DESIGNER_TEST_FONT, k);
    out[k] = 0;
}

static int copy_font(const char* folder, const char* file) {
    char from[DSG_PATH_MAX], to[DSG_PATH_MAX], err[512];
    font_dir(from, sizeof from);
    snprintf(from + strlen(from), sizeof from - strlen(from), "%s", file);
    if (!dsg_is_file(from)) { printf("  no font %s\n", from); return -1; }
    if (dsg_join(to, sizeof to, folder, "fonts") || dsg_make_dir(to, err, sizeof err)) return -1;
    snprintf(to + strlen(to), sizeof to - strlen(to), "/%s", file);
    return copy_file(from, to);
}

/* The generated worst case, as bench_designer_check's make_worst() writes
 * it without the word column ("many"). */
typedef struct buf { char* p; size_t n, c; } buf;

static void put(buf* b, const char* fmt, ...) {
    va_list ap;
    int k;
    for (;;) {
        va_start(ap, fmt);
        k = vsnprintf(b->p ? b->p + b->n : NULL, b->p ? b->c - b->n : 0, fmt, ap);
        va_end(ap);
        if (k < 0) return;
        if (b->p && b->n + (size_t)k < b->c) break;
        {
            size_t nc = b->c ? b->c * 2 : 65536;
            char* np;
            while (nc < b->n + (size_t)k + 1) nc *= 2;
            np = (char*)realloc(b->p, nc);
            if (!np) return;
            b->p = np;
            b->c = nc;
        }
    }
    b->n += (size_t)k;
}

static void put_cp(buf* b, uint32_t cp) {
    if (cp < 0x80) put(b, "%c", (int)cp);
    else if (cp < 0x800) put(b, "%c%c", (int)(0xC0 | (cp >> 6)), (int)(0x80 | (cp & 0x3F)));
    else put(b, "%c%c%c", (int)(0xE0 | (cp >> 12)), (int)(0x80 | ((cp >> 6) & 0x3F)), (int)(0x80 | (cp & 0x3F)));
}

static uint32_t rng_state = 0x9E3779B9u;
static uint32_t rnd(uint32_t n) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state % n;
}

enum { LATN, HEBR, ARAB, DEVA, N_SCRIPTS };
static const char* const script_font[N_SCRIPTS] = { "NotoSans-Regular.ttf", "NotoSansHebrew-Regular.ttf", "NotoSansArabic-Regular.ttf",
                                                    "NotoSansDevanagari-Regular.ttf" };

static void put_word(buf* b, int script) {
    int k, len = 3 + (int)rnd(6);
    for (k = 0; k < len; k++) {
        switch (script) {
        case LATN: put_cp(b, 'a' + rnd(26)); break;
        case HEBR: put_cp(b, 0x05D0 + rnd(27)); break;
        case ARAB: {
            uint32_t r = rnd(29);
            put_cp(b, r < 19 ? 0x0628 + r : 0x0641 + (r - 19));
            break;
        }
        default:
            put_cp(b, 0x0915 + rnd(37));
            if (rnd(2)) put_cp(b, 0x093E + rnd(15));
            break;
        }
    }
}

static int make_worst(const char* work, char* out, size_t cap) {
    char path[DSG_PATH_MAX], err[512];
    buf d = { 0 }, desc = { 0 }, csv = { 0 };
    int i, k, s, w, rc = -1;
    rng_state = 0x9E3779B9u;
    if (dsg_join(out, cap, work, "many") || dsg_make_dir(out, err, sizeof err)) return -1;
    for (s = 0; s < N_SCRIPTS; s++)
        if (copy_font(out, script_font[s])) return -1;
    put(&d, "{\n  \"format\": \"ysp-experiment\", \"version\": 1, \"name\": \"many\",\n");
    put(&d, "  \"devices\": { \"resp\": { \"direction\": \"in\", \"family\": [\"keyboard\"], \"required\": true } },\n");
    put(&d, "  \"stimuli\": {\n");
    for (i = 0; i < 200; i++) {
        s = i % N_SCRIPTS;
        put(&d, "    \"s%03d\": { \"kind\": \"text\", \"font\": \"fonts/%s\", \"size\": %d, \"width\": 1000, \"y\": %d, \"text\": \"", i,
            script_font[s], 20 + (i % 5) * 4, -400 + (i % 40) * 20);
        if (i == 0) put(&d, "Edit ");
        for (w = 0; w < 6 + (int)rnd(7); w++) {
            if (w) put(&d, " ");
            put_word(&d, s);
        }
        put(&d, "\" },\n");
    }
    put(&d, "    \"fix\": { \"kind\": \"shape\", \"shape\": \"cross\", \"w\": 24, \"h\": 24, \"shape_p0\": 3, \"color\": [0.6, 0.6, 0.6] }\n  },\n");
    put(&d, "  \"trials\": {\n    \"t\": {\n      \"events\": [{ \"on\": \"fix\" }, { \"wait_s\": 0.5 }, { \"on\": [");
    for (i = 0; i < 200; i++) put(&d, "%s\"s%03d\"", i ? ", " : "", i);
    put(&d, "] }],\n      \"response\": { \"role\": \"resp\", \"from\": \"s000\", \"duration_s\": 2,\n");
    put(&d, "                    \"choices\": [{ \"key\": \"f\", \"name\": \"a\" }, { \"key\": \"j\", \"name\": \"b\" }] },\n");
    put(&d, "      \"after\": [{ \"off\": [\"fix\"");
    for (i = 0; i < 200; i++) put(&d, ", \"s%03d\"", i);
    put(&d, "] }, { \"wait_s\": 0.2 }]\n    }\n  },\n");
    put(&d, "  \"timeline\": [");
    for (i = 0; i < 10; i++)
        put(&d, "%s\n    { \"name\": \"b%d\", \"timeline_variables\": \"words%d.pstb\", \"trial\": \"t\" }", i ? "," : "", i, i);
    put(&d, "\n  ]\n}\n");
    put(&desc, "{\n  \"format\": \"ysp-pack-source\", \"version\": 1,\n");
    put(&desc, "  \"experiment\": { \"entry\": \"experiment/main.json\", \"source\": \"experiment.json\",\n");
    put(&desc, "                  \"media_type\": \"application/vnd.ysp.experiment\" },\n  \"resources\": [\n");
    for (i = 0; i < 10; i++)
        put(&desc, "%s    { \"name\": \"words%d.pstb\", \"kind\": \"table\", \"source\": \"words%d.csv\" }", i ? ",\n" : "", i, i);
    for (s = 0; s < N_SCRIPTS; s++) {
        put(&desc, ",\n    { \"kind\": \"font\", \"source\": \"fonts/%s\", \"license\": \"OFL-1.1\" }", script_font[s]);
        put(&desc, ",\n    { \"kind\": \"curveset\", \"from\": \"fonts/%s\", \"glyphs\": \"used\" }", script_font[s]);
    }
    put(&desc, "\n  ]\n}\n");
    if (!d.p || !desc.p) goto done;
    if (dsg_join(path, sizeof path, out, "experiment.json") || dsg_write_file(path, d.p, d.n, err, sizeof err)) goto done;
    for (k = 0; k < 10; k++) {
        char nm[32];
        csv.n = 0;
        put(&csv, "word,cond\n");
        for (i = 0; i < 1000; i++) {
            put_word(&csv, i % N_SCRIPTS);
            put(&csv, "%d,%d\n", k * 1000 + i, i % 4);
        }
        snprintf(nm, sizeof nm, "words%d.csv", k);
        if (!csv.p || dsg_join(path, sizeof path, out, nm) || dsg_write_file(path, csv.p, csv.n, err, sizeof err)) goto done;
    }
    if (dsg_join(path, sizeof path, out, "pack.json") || dsg_write_file(path, desc.p, desc.n, err, sizeof err)) goto done;
    rc = 0;
done:
    free(d.p);
    free(csv.p);
    free(desc.p);
    return rc;
}

/* --- the bench --------------------------------------------------------------------------- */

typedef struct U {
    dsg_preview_gl gl;
    dsg_session_ops ops;
    dsg_worker*    w;
    dsg_doc*       doc;
    dsg_preview*   pv;
    dsg_project    proj;
    uint64_t       posted;
    int64_t        now;
} U;

static const int64_t FR = 16666667;

static void post_check(U* u) {
    size_t n = 0;
    const char* t = dsg_doc_bytes(u->doc, DSG_BUF_DEF, &n);
    const uint64_t rev = dsg_doc_revision(u->doc);
    if (dsg_worker_check(u->w, &u->proj, t, n, u->doc, rev) == 0) u->posted = rev;
    (void)dsg_worker_wait(u->w, 0);
}

/* ui.cpp's frame; *t_tick the tick's time */
static void frame(U* u, double* t_tick) {
    const dsg_check_result* res = NULL;
    dsg_session_result sr;
    double t0;
    u->now += FR;
    if (dsg_worker_take_check(u->w, &res)) dsg_preview_checked(u->pv, res, u->now);
    memset(&sr, 0, sizeof sr);
    t0 = now_us();
    if (dsg_worker_take_session(u->w, &sr)) dsg_preview_adopt(u->pv, &sr, u->now);
    dsg_preview_tick(u->pv, u->now, true);
    if (t_tick) *t_tick = now_us() - t0;
    if (dsg_doc_revision(u->doc) != u->posted) post_check(u);
}

static bool running(U* u) {
    dsg_preview_info i;
    dsg_preview_get_info(u->pv, &i);
    return i.status == DSG_PV_RUNNING && !i.attaching && !i.loading;
}

/* The session op's load and close, called as the worker calls them. */
static void bench_load(U* u, const char* name, const dsg_check_result* cr, const char* pin, int n) {
    double* tl = (double*)calloc((size_t)n, sizeof *tl);
    double* tc = (double*)calloc((size_t)n, sizeof *tc);
    size_t len = 0;
    const char* text = dsg_doc_bytes(u->doc, DSG_BUF_DEF, &len);
    char err[512], label[32];
    int i, k = 0;
    for (i = 0; i < n && tl && tc; i++) {
        dsg_session* s = (dsg_session*)calloc(1, sizeof *s);
        double t0;
        int rc;
        if (!s) break;
        s->revision = cr->revision;
        s->pack = dsg_pack_retain(cr->pack);
        s->settings.seed = 1;
        s->settings.pin_row = -1;
        snprintf(s->settings.participant, sizeof s->settings.participant, "correct");
        s->settings.rt_s = 0.3;
        if (pin) snprintf(s->settings.pin_leaf, sizeof s->settings.pin_leaf, "%s", pin);
        if (dsg_join(s->settings.out_dir, sizeof s->settings.out_dir, u->proj.folder, DSG_DIR "/preview") != 0) s->settings.out_dir[0] = 0;
        t0 = now_us();
        rc = u->ops.load(u->ops.ctx, s, text, len, err, sizeof err);
        tl[k] = now_us() - t0;
        if (rc != 0) printf("  %s: load: %s\n", name, err);
        t0 = now_us();
        u->ops.close(u->ops.ctx, s);
        tc[k++] = now_us() - t0;
        dsg_pack_release(s->pack);
        free(s);
    }
    snprintf(label, sizeof label, "%s", pin ? "load_pin" : "load");
    report(label, name, tl, k);
    if (!pin) report("close", name, tc, k);
    free(tl);
    free(tc);
}

static void bench_project(const char* name, const char* folder, const char* pin, int frames) {
    U u;
    dsg_worker_desc wd;
    dsg_preview_settings ps;
    char err[1024];
    double* t = (double*)calloc((size_t)frames, sizeof *t);
    double tick = 0;
    int i, k;
    memset(&u, 0, sizeof u);
    memset(&wd, 0, sizeof wd);
    dsg_preview_session_ops(&u.gl, &wd.session);
    u.ops = wd.session;
    wd.no_thread = true;
    u.w = dsg_worker_start(&wd, err, sizeof err);
    u.doc = dsg_doc_new();
    if (!t || !u.w || !u.doc || dsg_doc_reset(u.doc, "", "", 0)) { printf("%s: cannot start\n", name); return; }
    u.pv = dsg_preview_new(&u.gl, u.w, u.doc);
    u.now = 1000000000;
    if (dsg_project_open(&u.proj, folder, err, sizeof err)) { printf("%s: cannot open: %s\n", name, err); return; }
    dsg_doc_reset(u.doc, u.proj.def_source, u.proj.text, u.proj.text_len);
    dsg_preview_open(u.pv, &u.proj);
    dsg_preview_settings_default(&ps);
    dsg_preview_set(u.pv, &ps);
    post_check(&u);
    {
        /* the session op alone, with the check's pack */
        const dsg_check_result* cr = NULL;
        if (!dsg_worker_take_check(u.w, &cr) || cr->report.status != DSG_CHECK_OK || !cr->pack) {
            printf("%s: the check did not accept the definition\n", name);
            return;
        }
        dsg_preview_checked(u.pv, cr, u.now);
        bench_load(&u, name, cr, NULL, 20);
        if (pin) bench_load(&u, name, cr, pin, 10);
    }
    /* the first session through the UI path: the frame that adopts and
     * attaches it */
    for (i = 0, k = 0; i < 20 && !running(&u); i++) {
        dsg_preview_info inf;
        frame(&u, &tick);
        dsg_preview_get_info(u.pv, &inf);
        if (inf.status == DSG_PV_RUNNING && k < frames) t[k++] = tick;
    }
    report("adopt", name, t, k);
    for (k = 0; k < 2; k++) {
        const int hz = k ? 500 : 60;
        if (hz != 60) {
            dsg_preview_get(u.pv, &ps);
            ps.session.hz = hz;
            dsg_preview_set(u.pv, &ps);
            for (i = 0; i < 20 && !running(&u); i++) frame(&u, NULL);
            frame(&u, NULL);
        }
        g_n_step = g_n_draw = 0;
        g_timing = 1;
        for (i = 0; i < frames; i++) frame(&u, &t[i]);
        g_timing = 0;
        report(hz == 60 ? "tick60" : "tick500", name, t, frames);
        report(hz == 60 ? "step60" : "step500", name, g_step, g_n_step);
        report(hz == 60 ? "draw60" : "draw500", name, g_draw, g_n_draw);
    }
    if (pin) {
        /* where the fast forward stopped */
        dsg_preview_info inf;
        dsg_preview_get(u.pv, &ps);
        snprintf(ps.session.pin_leaf, sizeof ps.session.pin_leaf, "%s", pin);
        dsg_preview_set(u.pv, &ps);
        frame(&u, NULL);
        for (i = 0; i < 20 && !running(&u); i++) frame(&u, NULL);
        dsg_preview_get_info(u.pv, &inf);
        {
            dsg_preview_where w;
            (void)dsg_preview_where_now(u.pv, &w);
            printf("  pin %s: %s (status %d, trial %d, leaf %s)\n", pin, inf.note[0] ? inf.note : "reached", inf.status, w.trial,
                   w.leaf ? w.leaf : "-");
        }
    }
    dsg_preview_close(u.pv);
    dsg_project_close(&u.proj);
    dsg_preview_free(u.pv);
    dsg_worker_stop(u.w);
    dsg_doc_free(u.doc);
    free(t);
}

int main(int argc, char** argv) {
    const char* work;
    const char* only;
    char folder[DSG_PATH_MAX], err[512];
    int frames;
    if (argc < 2) {
        printf("usage: bench_designer_preview WORK [FRAMES] [PROJECT]\n");
        return 2;
    }
    work = argv[1];
    frames = argc > 2 ? atoi(argv[2]) : 1000;
    only = argc > 3 ? argv[3] : NULL;
    if (frames < 10) frames = 10;
    if (dsg_make_dir(work, err, sizeof err)) { printf("%s\n", err); return 1; }
    g_cap = frames * 20;
    g_step = (double*)calloc((size_t)g_cap, sizeof *g_step);
    g_draw = (double*)calloc((size_t)g_cap, sizeof *g_draw);
    if (!g_step || !g_draw) return 1;
    loadavg("before");
    if ((!only || strcmp(only, "bimanual") == 0) && copy_project(work, "bimanual", folder, sizeof folder) == 0)
        bench_project("bimanual", folder, "blocks/b4_track", frames);
    if ((!only || strcmp(only, "stroop") == 0) && copy_project(work, "stroop", folder, sizeof folder) == 0 &&
        copy_font(folder, "NotoSans-Regular.ttf") == 0)
        bench_project("stroop", folder, NULL, frames);
    if ((!only || strcmp(only, "many") == 0) && make_worst(work, folder, sizeof folder) == 0) bench_project("many", folder, "b9", frames);
    loadavg("after");
    free(g_step);
    free(g_draw);
    return 0;
}
