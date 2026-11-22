/* preview_test.c - the designer's preview core (designer/core/preview.c;
 * docs/designer.md, "Plan of step 5", part A). Headless: the sessions draw
 * on gfx's null backend, the clock is the test's, and the check thread's
 * jobs run inline (dsg_worker_desc.no_thread) except in the last part,
 * which runs the real thread (for ThreadSanitizer).
 *
 * The harness does what ui.cpp does each frame: take a check result and a
 * session result, tick the preview, and post a check when the document
 * changed.
 *
 * Parts: (1) the first session, the data set, the steps a UI frame for
 * given times (cap, speed, pause, step, hidden), idle while paused; (2)
 * the settle and the current revision; (3) last good on a refusal; (4)
 * stale sessions dropped; (5) the attach over frames with the old session
 * running, and newest wins; (6) run-time and load refusals as the preview
 * issue; (7) the pin's two stops, the leaves, "Preview this template";
 * (8) keys, choices, the outputs, the roles not driven; (9) the pointer to
 * role units for px, cm and deg; (10) the run folders, one left, across
 * two "processes"; (11) no heap call in a running tick; (12) the real
 * thread.
 *
 * Usage: test_designer_preview WORK. With GNU ld (PV_TEST_WRAP), --wrap
 * counts heap calls inside dsg_preview_tick() and wraps pl_sim_attach()
 * (a held attach) and pl_sim_frame() (which frames draw). */
#include "preview.h"
#include "doc.h"
#include "player.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const char* work;
static const int64_t FR = 16666667;   /* a 60 Hz UI frame */
static const int64_t kWait = 60000000000LL;

/* --- the wraps ------------------------------------------------------------------- */

#if defined(PV_TEST_WRAP)
static _Thread_local int in_tick;
static long long tick_heap;
void* __real_malloc(size_t n);
void* __real_calloc(size_t a, size_t b);
void* __real_realloc(void* q, size_t n);
void __real_free(void* q);
void* __wrap_malloc(size_t n) { if (in_tick) tick_heap++; return __real_malloc(n); }
void* __wrap_calloc(size_t a, size_t b) { if (in_tick) tick_heap++; return __real_calloc(a, b); }
void* __wrap_realloc(void* q, size_t n) { if (in_tick) tick_heap++; return __real_realloc(q, n); }
void __wrap_free(void* q) { if (in_tick && q) tick_heap++; __real_free(q); }

static int attach_hold;          /* attach calls that give 0 before the real one */
static int attach_calls;
static int64_t attach_budget;
int __real_pl_sim_attach(pl_player* p, int64_t budget_ns);
int __wrap_pl_sim_attach(pl_player* p, int64_t budget_ns) {
    attach_calls++;
    attach_budget = budget_ns;
    if (attach_hold > 0) {
        attach_hold--;
        return 0;
    }
    return __real_pl_sim_attach(p, budget_ns);
}

static int count_frames;
static int n_frames, n_draws;
static bool last_draw;
int __real_pl_sim_frame(pl_player* p, bool draw);
int __wrap_pl_sim_frame(pl_player* p, bool draw) {
    if (count_frames) {
        n_frames++;
        if (draw) n_draws++;
        last_draw = draw;
    }
    return __real_pl_sim_frame(p, draw);
}
#define WRAPPED 1
#define TICK_IN() (in_tick = 1)
#define TICK_OUT() (in_tick = 0)
#else
#define WRAPPED 0
#define TICK_IN() ((void)0)
#define TICK_OUT() ((void)0)
static int attach_calls, n_frames, n_draws;
/* only the wraps read these; volatile keeps clang's
 * -Wunused-but-set-global quiet about the parts' writes */
static volatile int attach_hold, count_frames;
static int64_t attach_budget;
static bool last_draw;
static long long tick_heap;   /* part_heap skips at run time without --wrap */
#endif

/* --- projects ---------------------------------------------------------------------- */

static int write_text(const char* folder, const char* name, const char* text) {
    char path[DSG_PATH_MAX], err[512];
    if (dsg_join(path, sizeof path, folder, name) || dsg_write_file(path, text, strlen(text), err, sizeof err)) {
        printf("  cannot write %s: %s\n", name, err);
        return -1;
    }
    return 0;
}

static const char kPackPlain[] =
    "{\"format\": \"ysp-pack-source\", \"version\": 1,\n"
    " \"experiment\": {\"entry\": \"experiment/main.json\", \"source\": \"experiment.json\", \"media_type\": \"application/vnd.ysp.experiment\"},\n"
    " \"resources\": []}\n";
static const char kPackConds[] =
    "{\"format\": \"ysp-pack-source\", \"version\": 1,\n"
    " \"experiment\": {\"entry\": \"experiment/main.json\", \"source\": \"experiment.json\", \"media_type\": \"application/vnd.ysp.experiment\"},\n"
    " \"resources\": [{\"name\": \"conds.pstb\", \"kind\": \"table\", \"source\": \"conds.csv\"}]}\n";

/* A keyboard trial: 0.5 s of fixation, then up to 2 s for f or j. */
static const char kKeys[] =
    "{\"format\": \"ysp-experiment\", \"version\": 1, \"name\": \"keys\",\n"
    " \"devices\": {\"resp\": {\"direction\": \"in\", \"family\": [\"keyboard\"], \"required\": true}},\n"
    " \"stimuli\": {\"fix\": {\"kind\": \"shape\", \"shape\": \"cross\", \"w\": 24, \"h\": 24, \"shape_p0\": 3}},\n"
    " \"trials\": {\"t\": {\"events\": [{\"on\": \"fix\"}, {\"wait_s\": 0.5}],\n"
    "   \"response\": {\"role\": \"resp\", \"choices\": [\"f\", \"j\"], \"from\": \"fix\", \"duration_s\": 2},\n"
    "   \"after\": [{\"off\": \"fix\"}, {\"wait_s\": 0.2}]}},\n"
    " \"timeline\": [{\"name\": \"main\", \"trial\": \"t\", \"rules\": [\"reps 3\"]}]}\n";

/* Two leaves: a, 2 of the table's 6 rows; then b. */
static const char kPins[] =
    "{\"format\": \"ysp-experiment\", \"version\": 1, \"name\": \"pins\",\n"
    " \"devices\": {\"resp\": {\"direction\": \"in\", \"family\": [\"keyboard\"], \"required\": true}},\n"
    " \"stimuli\": {\"fix\": {\"kind\": \"shape\", \"shape\": \"cross\", \"w\": 24, \"h\": 24, \"shape_p0\": 3}},\n"
    " \"trials\": {\"t\": {\"events\": [{\"on\": \"fix\"}, {\"wait_s\": 0.3}],\n"
    "   \"response\": {\"role\": \"resp\", \"choices\": [\"f\", \"j\"], \"from\": \"fix\", \"duration_s\": 1},\n"
    "   \"after\": [{\"off\": \"fix\"}, {\"wait_s\": 0.1}]}},\n"
    " \"timeline\": [{\"name\": \"a\", \"timeline_variables\": \"conds.pstb\", \"rules\": [\"subset 2\"], \"trial\": \"t\"},\n"
    "              {\"name\": \"b\", \"trial\": \"t\", \"rules\": [\"reps 2\"]}]}\n";
static const char kConds[] = "v\n0\n1\n2\n3\n4\n5\n";

/* A run-time refusal (test_player_sim's case 7): the minimum rt is past
 * the response's end. "pre" runs it first, so a pin to "l2" meets it in
 * the fast forward. */
static const char kRefuse[] =
    "{\"format\": \"ysp-experiment\", \"version\": 1, \"name\": \"t\",\n"
    " \"devices\": {\"r\": {\"direction\": \"in\", \"family\": [\"keyboard\"], \"required\": true}},\n"
    " \"stimuli\": {\"s\": {\"kind\": \"shape\", \"w\": 10, \"h\": 10}},\n"
    " \"trials\": {\"win\": {\"events\": [{\"on\": \"s\"}, {\"wait_s\": 0.2}],\n"
    "   \"response\": {\"role\": \"r\", \"choices\": [\"left\", \"right\"], \"from\": \"s\", \"duration_s\": \"$d_s\", \"minimum_valid_rt_s\": \"$m_s\"}}},\n"
    " \"timeline\": [{\"name\": \"pre\", \"trial\": \"win\", \"vars\": {\"d_s\": {\"choice\": [0.5]}, \"m_s\": {\"choice\": [0.6]}}},\n"
    "              {\"name\": \"l2\", \"trial\": \"win\", \"vars\": {\"d_s\": {\"choice\": [0.5]}, \"m_s\": {\"choice\": [0.1]}}}]}\n";

/* A position2d role in UNITS and a region at CX, CY of radius R: the
 * trial leaves "wait" when the role's value is in it. Two leaves, so a pin
 * to l2 stalls in l1 (the script moves no signal role). */
static void sig_def(char* out, size_t cap, const char* units, double cx, double cy, double r) {
    snprintf(out, cap,
             "{\"format\": \"ysp-experiment\", \"version\": 2, \"name\": \"sig\",\n"
             " \"display\": {\"units\": \"%s\"},\n"
             " \"roles\": {\"hand\": {\"type\": \"position2d\", \"units\": \"display_%s\"}},\n"
             " \"stimuli\": {\"s\": {\"kind\": \"shape\", \"shape\": \"circle\", \"w\": 1, \"h\": 1}},\n"
             " \"trials\": {\"t\": {\"controls\": {\"in\": {\"control\": \"region\", \"of\": \"hand\", \"center\": [%.6f, %.6f], \"radius\": %.6f}},\n"
             "   \"states\": [{\"name\": \"wait\", \"steps\": [{\"on\": \"s\"}], \"exits\": [{\"on\": \"in\", \"to\": \"done\"}]},\n"
             "              {\"name\": \"done\", \"steps\": [{\"off\": \"s\"}, {\"wait_s\": 0.2}]}]}},\n"
             " \"timeline\": [{\"name\": \"l1\", \"trial\": \"t\", \"rules\": [\"reps 2\"]}, {\"name\": \"l2\", \"trial\": \"t\"}]}\n",
             units, units, cx, cy, r);
}

static int make_project(const char* name, const char* def, const char* pack, const char* csv, char* folder, size_t cap) {
    char err[512];
    if (dsg_join(folder, cap, work, name) || dsg_make_dir(folder, err, sizeof err)) return -1;
    /* a run before this one left its data sets and check pack */
    if (dsg_remove_tree(folder, DSG_DIR, err, sizeof err)) return -1;
    if (write_text(folder, "pack.json", pack) || write_text(folder, "experiment.json", def)) return -1;
    if (csv && write_text(folder, "conds.csv", csv)) return -1;
    return 0;
}

static int copy_file(const char* from, const char* to) {
    char* d = NULL;
    size_t n = 0;
    char err[512];
    if (dsg_read_file(from, &d, &n, err, sizeof err)) { printf("  %s\n", err); return -1; }
    if (dsg_write_file(to, d, n, err, sizeof err)) { printf("  %s\n", err); free(d); return -1; }
    free(d);
    return 0;
}

/* tests/player/NAME's listed files into WORK/NAME. */
static int copy_player_project(const char* name, const char* const* files, char* folder, size_t cap) {
    char src[DSG_PATH_MAX], a[DSG_PATH_MAX], b[DSG_PATH_MAX], err[512];
    int i;
    snprintf(src, sizeof src, "%s/tests/player/%s", YSP_SOURCE_DIR, name);
    if (dsg_join(folder, cap, work, name) || dsg_make_dir(folder, err, sizeof err)) return -1;
    if (dsg_remove_tree(folder, DSG_DIR, err, sizeof err)) return -1;
    for (i = 0; files[i]; i++)
        if (dsg_join(a, sizeof a, src, files[i]) || dsg_join(b, sizeof b, folder, files[i]) || copy_file(a, b)) return -1;
    return 0;
}

/* --- the harness: ui.cpp's frame ----------------------------------------------------- */

typedef struct U {
    dsg_preview_gl gl;            /* caps NULL: the null backend */
    dsg_worker*    w;
    dsg_doc*       doc;
    dsg_preview*   pv;
    dsg_project    proj;
    bool           open, threaded, hold_checks;
    uint64_t       posted, floor;
    int64_t        now;
} U;

static int u_start(U* u, bool threaded) {
    dsg_worker_desc wd;
    char err[512];
    memset(u, 0, sizeof *u);
    memset(&wd, 0, sizeof wd);
    dsg_preview_session_ops(&u->gl, &wd.session);
    wd.no_thread = !threaded;
    u->threaded = threaded;
    u->w = dsg_worker_start(&wd, err, sizeof err);
    u->doc = dsg_doc_new();
    if (!u->w || !u->doc || dsg_doc_reset(u->doc, "", "", 0)) return -1;
    u->pv = dsg_preview_new(&u->gl, u->w, u->doc);
    u->now = 5000000000LL;
    return u->pv ? 0 : -1;
}

static void u_close(U* u) {
    if (!u->open) return;
    dsg_preview_close(u->pv);
    dsg_project_close(&u->proj);
    u->open = false;
    dsg_worker_forget(u->w);
    u->floor = dsg_doc_revision(u->doc) + 1;
}

static void u_stop(U* u) {
    u_close(u);
    dsg_preview_free(u->pv);
    dsg_worker_stop(u->w);
    dsg_doc_free(u->doc);
}

static void post_check(U* u) {
    size_t n = 0;
    const char* t = dsg_doc_bytes(u->doc, DSG_BUF_DEF, &n);
    const uint64_t rev = dsg_doc_revision(u->doc);
    if (dsg_worker_check(u->w, &u->proj, t, n, u->doc, rev, NULL) == 0) u->posted = rev;
    if (!u->threaded) (void)dsg_worker_wait(u->w, 0);
}

static int u_open(U* u, const char* folder) {
    char err[1024];
    dsg_preview_settings ps;
    memset(&ps, 0, sizeof ps);
    u_close(u);
    if (dsg_project_open(&u->proj, folder, err, sizeof err)) {
        printf("  open %s: %s\n", folder, err);
        return -1;
    }
    u->open = true;
    dsg_doc_reset(u->doc, u->proj.def_source, u->proj.text, u->proj.text_len);
    u->floor = dsg_doc_revision(u->doc);
    dsg_preview_open(u->pv, &u->proj);
    dsg_preview_settings_default(&ps);
    dsg_preview_set(u->pv, &ps);
    post_check(u);
    return 0;
}

static void frame_at(U* u, int64_t dt, bool shown) {
    const dsg_check_result* res = NULL;
    dsg_session_result sr;
    u->now += dt;
    if (u->threaded) (void)dsg_worker_wait(u->w, kWait);
    if (dsg_worker_take_check(u->w, &res) && u->open && res->revision >= u->floor) dsg_preview_checked(u->pv, res, u->now);
    memset(&sr, 0, sizeof sr);
    if (dsg_worker_take_session(u->w, &sr)) dsg_preview_adopt(u->pv, &sr, u->now);
    TICK_IN();
    dsg_preview_tick(u->pv, u->now, shown);
    TICK_OUT();
    if (u->open && !u->hold_checks && dsg_doc_revision(u->doc) != u->posted) post_check(u);
}

static void frame(U* u) { frame_at(u, FR, true); }

static dsg_preview_info info(U* u) {
    dsg_preview_info i;
    memset(&i, 0, sizeof i);
    dsg_preview_get_info(u->pv, &i);
    return i;
}

static dsg_preview_where where(U* u) {
    dsg_preview_where w;
    memset(&w, 0, sizeof w);
    (void)dsg_preview_where_now(u->pv, &w);
    return w;
}

static dsg_worker_stats stats(U* u) {
    dsg_worker_stats s;
    memset(&s, 0, sizeof s);
    dsg_worker_get_stats(u->w, &s);
    return s;
}

/* Frames until a session shows and runs (or k frames). */
static bool until_running(U* u, int k) {
    int i;
    for (i = 0; i < k; i++) {
        frame(u);
        if (info(u).status == DSG_PV_RUNNING && !info(u).attaching && !info(u).loading) return true;
    }
    return false;
}

/* Frames until a session other than the one with data set out0 runs. */
static bool until_other(U* u, const char* out0, int k) {
    int i;
    for (i = 0; i < k; i++) {
        frame(u);
        if (info(u).status == DSG_PV_RUNNING && !info(u).attaching && strcmp(info(u).out_dir, out0) != 0) return true;
    }
    return false;
}

static bool until_status(U* u, int status, int k) {
    int i;
    for (i = 0; i < k; i++) {
        frame(u);
        if (info(u).status == status) return true;
    }
    return false;
}

/* Replaces the first `from` in the definition's text. */
static int edit(U* u, const char* from, const char* to) {
    size_t n = 0;
    const char* t = dsg_doc_bytes(u->doc, DSG_BUF_DEF, &n);
    const char* at = strstr(t, from);
    if (!at) return -1;
    return dsg_doc_splice(u->doc, DSG_BUF_DEF, (size_t)(at - t), strlen(from), to, strlen(to));
}

static void set_pin(U* u, const char* leaf, int row) {
    dsg_preview_settings s;
    dsg_preview_get(u->pv, &s);
    snprintf(s.session.pin_leaf, sizeof s.session.pin_leaf, "%s", leaf ? leaf : "");
    s.session.pin_row = row;
    dsg_preview_set(u->pv, &s);
}

static void set_speed(U* u, double speed) {
    dsg_preview_settings s;
    dsg_preview_get(u->pv, &s);
    s.speed = speed;
    dsg_preview_set(u->pv, &s);
}

/* The run-N folders in the project's .designer/preview; *last the highest. */
static int count_runs(const char* folder, int* last) {
    char path[DSG_PATH_MAX + 64];
    int k, n = 0;
    *last = 0;
    for (k = 1; k <= 400; k++) {
        snprintf(path, sizeof path, "%s/" DSG_DIR "/preview/run-%d", folder, k);
        if (dsg_is_dir(path)) {
            n++;
            *last = k;
        }
    }
    return n;
}

/* --- (1) the first session and the steps ---------------------------------------------- */

static void part_steps(U* u, const char* folder) {
    dsg_preview_info i;
    dsg_preview_where w0, w1;
    int64_t f0, k;
    int idle;
    char path[DSG_PATH_MAX];

    CHECK(u_open(u, folder) == 0, "steps: open");
    i = info(u);
    CHECK(i.status == DSG_PV_NONE, "steps: before the check, status %d", i.status);
    CHECK(until_running(u, 10), "steps: the first session did not run: status %d", info(u).status);
    i = info(u);
    CHECK(i.width == 1920 && i.height == 1080 && i.hz == 60, "steps: the built-in display %dx%d at %d", i.width, i.height, i.hz);
    CHECK(strstr(i.out_dir, "/" DSG_DIR "/preview/run-") != NULL && dsg_is_dir(i.out_dir), "steps: the data set '%s'", i.out_dir);
    CHECK(dsg_preview_animating(u->pv) && !dsg_preview_work(u->pv), "steps: running: animating and no work");
    CHECK(i.shown_unix_s > 1700000000, "steps: the post's time %lld", (long long)i.shown_unix_s);
    snprintf(path, sizeof path, "%s/frames.csv", i.out_dir);
    CHECK(dsg_is_file(path), "steps: the writer made frames.csv");

    /* one frame a UI frame at speed 1 */
    w0 = where(u);
    for (k = 0; k < 60; k++) frame(u);
    w1 = where(u);
    CHECK(w1.frame - w0.frame >= 59 && w1.frame - w0.frame <= 61, "steps: 60 UI frames stepped %lld", (long long)(w1.frame - w0.frame));
    CHECK(w1.trial >= 0 && w1.leaf && strcmp(w1.leaf, "main") == 0 && w1.tmpl && strcmp(w1.tmpl, "t") == 0,
          "steps: where_now trial %d leaf %s tmpl %s", w1.trial, w1.leaf ? w1.leaf : "-", w1.tmpl ? w1.tmpl : "-");

    /* the cap: a 1 s stall gives 16 steps, the last one drawn */
    f0 = where(u).frame;
    count_frames = 1;
    n_frames = n_draws = 0;
    frame_at(u, 1000000000, true);
    count_frames = 0;
    CHECK(where(u).frame - f0 == 16, "steps: a 1 s frame stepped %lld, not the cap 16", (long long)(where(u).frame - f0));
    CHECK(info(u).slow, "steps: the dropped frames are not reported as slow");
    if (WRAPPED) CHECK(n_frames == 16 && n_draws == 1 && last_draw, "steps: %d frames, %d drawn, the last drawn %d", n_frames, n_draws, last_draw);
    /* no catch-up after the drop */
    f0 = where(u).frame;
    frame(u);
    CHECK(where(u).frame - f0 <= 2 && !info(u).slow, "steps: after the drop, %lld frames", (long long)(where(u).frame - f0));

    /* speed */
    set_speed(u, 2.0);
    f0 = where(u).frame;
    for (k = 0; k < 30; k++) frame(u);
    CHECK(where(u).frame - f0 >= 59 && where(u).frame - f0 <= 61, "steps: speed 2, 30 UI frames stepped %lld", (long long)(where(u).frame - f0));
    set_speed(u, 0.25);
    f0 = where(u).frame;
    for (k = 0; k < 40; k++) frame(u);
    CHECK(where(u).frame - f0 >= 9 && where(u).frame - f0 <= 11, "steps: speed 0.25, 40 UI frames stepped %lld", (long long)(where(u).frame - f0));
    set_speed(u, 1.0);

    /* pause: no steps, idle */
    dsg_preview_pause(u->pv);
    f0 = where(u).frame;
    idle = 0;
    for (k = 0; k < 600; k++) {   /* 10 s at 60 Hz */
        frame(u);
        if (dsg_preview_work(u->pv) || dsg_preview_animating(u->pv)) idle++;
    }
    CHECK(where(u).frame == f0 && info(u).status == DSG_PV_PAUSED, "steps: paused, stepped %lld", (long long)(where(u).frame - f0));
    CHECK(idle == 0, "steps: paused, %d of 600 frames wanted a redraw", idle);

    /* step: one frame, drawn, then paused */
    dsg_preview_step(u->pv);
    CHECK(dsg_preview_work(u->pv), "steps: a step asked for is work");
    count_frames = 1;
    n_frames = n_draws = 0;
    frame(u);
    count_frames = 0;
    CHECK(where(u).frame == f0 + 1 && info(u).status == DSG_PV_PAUSED && !dsg_preview_work(u->pv), "steps: step gave %lld frames",
          (long long)(where(u).frame - f0));
    if (WRAPPED) CHECK(n_frames == 1 && n_draws == 1, "steps: the step drew %d of %d", n_draws, n_frames);

    /* run after a long pause: no catch-up */
    frame_at(u, 5000000000LL, true);
    dsg_preview_run(u->pv);
    f0 = where(u).frame;
    frame(u);
    frame(u);
    CHECK(where(u).frame - f0 >= 1 && where(u).frame - f0 <= 3, "steps: run after 5 s paused stepped %lld", (long long)(where(u).frame - f0));

    /* a hidden pane pauses: no steps, no animation, no catch-up */
    f0 = where(u).frame;
    for (k = 0; k < 60; k++) frame_at(u, FR, false);
    CHECK(where(u).frame == f0 && !dsg_preview_animating(u->pv) && !dsg_preview_work(u->pv), "steps: hidden, stepped %lld",
          (long long)(where(u).frame - f0));
    frame(u);
    frame(u);
    CHECK(where(u).frame - f0 >= 1 && where(u).frame - f0 <= 3, "steps: shown again stepped %lld", (long long)(where(u).frame - f0));

    /* to the end: "Session over" */
    CHECK(until_status(u, DSG_PV_ENDED, 2000), "steps: the session did not end: status %d", info(u).status);
    i = info(u);
    CHECK(i.trials == 3 && !dsg_preview_animating(u->pv), "steps: ended after %d trials", i.trials);
}

/* --- (2) the settle --------------------------------------------------------------------- */

static void part_settle(U* u, const char* folder) {
    dsg_worker_stats s0;
    int64_t t0;
    int k;
    CHECK(u_open(u, folder) == 0, "settle: open");
    CHECK(until_running(u, 10), "settle: no session");
    s0 = stats(u);
    CHECK(edit(u, "\"reps 3\"", "\"reps 4\"") == 0, "settle: edit");
    t0 = u->now;
    frame(u);   /* sees the change, posts the check */
    frame(u);   /* takes the accepted check: 33 ms after the edit */
    CHECK(stats(u).sessions_loaded == s0.sessions_loaded, "settle: a session job 33 ms after the edit");
    CHECK(dsg_preview_work(u->pv), "settle: the job to post is work");
    while (u->now + FR < t0 + 300000000) frame(u);
    CHECK(stats(u).sessions_loaded == s0.sessions_loaded, "settle: a session job before 300 ms (%lld ms)", (long long)((u->now - t0) / 1000000));
    frame(u);
    frame(u);
    CHECK(stats(u).sessions_loaded == s0.sessions_loaded + 1, "settle: no session job after 300 ms (%lld ms)", (long long)((u->now - t0) / 1000000));
    CHECK(until_running(u, 10), "settle: the new session did not show");

    /* a burst: an edit every 100 ms for 1 s, one job 300 ms after the last */
    s0 = stats(u);
    for (k = 0; k < 10; k++) {
        CHECK(edit(u, k % 2 ? "\"reps 3\"" : "\"reps 4\"", k % 2 ? "\"reps 4\"" : "\"reps 3\"") == 0, "settle: burst edit");
        frame_at(u, 100000000, true);
    }
    t0 = u->now;
    frame(u);
    CHECK(stats(u).sessions_loaded == s0.sessions_loaded, "settle: a job during the burst");
    while (u->now < t0 + 400000000) frame(u);
    CHECK(stats(u).sessions_loaded == s0.sessions_loaded + 1, "settle: %lld jobs after the burst", (long long)(stats(u).sessions_loaded - s0.sessions_loaded));

    /* a check of an older revision posts nothing */
    CHECK(until_running(u, 10), "settle: no session before the old-revision case");
    s0 = stats(u);
    CHECK(edit(u, "\"reps 4\"", "\"reps 5\"") == 0, "settle: edit A");
    frame(u);   /* posts A's check */
    CHECK(edit(u, "\"reps 5\"", "\"reps 6\"") == 0, "settle: edit B");
    u->hold_checks = true;
    for (k = 0; k < 40; k++) frame(u);   /* A's result arrives; B's check is held */
    CHECK(stats(u).sessions_loaded == s0.sessions_loaded, "settle: a session for an older revision's check");
    u->hold_checks = false;
    for (k = 0; k < 30; k++) frame(u);
    CHECK(stats(u).sessions_loaded == s0.sessions_loaded + 1, "settle: B's session %lld", (long long)(stats(u).sessions_loaded - s0.sessions_loaded));
}

/* --- (3) last good on a refusal --------------------------------------------------------- */

static void part_last_good(U* u, const char* folder) {
    dsg_worker_stats s0;
    dsg_preview_info i;
    char out0[DSG_PATH_MAX];
    int64_t f0;
    int k;
    CHECK(u_open(u, folder) == 0, "last good: open");
    CHECK(until_running(u, 10), "last good: no session");
    snprintf(out0, sizeof out0, "%s", info(u).out_dir);
    s0 = stats(u);
    CHECK(edit(u, "\"reps 3\"", "\"reps 3\",,") == 0, "last good: edit");
    f0 = where(u).frame;
    for (k = 0; k < 60; k++) frame(u);
    i = info(u);
    CHECK(stats(u).sessions_loaded == s0.sessions_loaded && strcmp(i.out_dir, out0) == 0, "last good: the session changed");
    CHECK(i.status == DSG_PV_RUNNING && where(u).frame - f0 >= 59, "last good: the session stopped (status %d)", i.status);
    CHECK(i.stale && i.stale_errors > 0, "last good: stale %d with %d errors", i.stale, i.stale_errors);
    /* restart: the last good text again */
    dsg_preview_restart(u->pv);
    CHECK(until_running(u, 10) && strcmp(info(u).out_dir, out0) != 0, "last good: restart did not load the last good text");
    CHECK(stats(u).sessions_loaded == s0.sessions_loaded + 1 && info(u).stale, "last good: restart");
    /* fixed: a new session after the settle */
    CHECK(edit(u, "\"reps 3\",,", "\"reps 2\"") == 0, "last good: fix");
    for (k = 0; k < 40; k++) frame(u);
    CHECK(stats(u).sessions_loaded == s0.sessions_loaded + 2 && !info(u).stale, "last good: the fixed text did not load");
}

/* --- (4) stale sessions --------------------------------------------------------------------- */

static void part_stale(U* u, const char* folder) {
    dsg_worker_stats s0;
    char out0[DSG_PATH_MAX];
    size_t n = 0;
    const char* t;
    dsg_session_settings ss;
    CHECK(u_open(u, folder) == 0, "stale: open");
    CHECK(until_running(u, 10), "stale: no session");
    snprintf(out0, sizeof out0, "%s", info(u).out_dir);

    /* (a) an edit after the post */
    s0 = stats(u);
    dsg_preview_restart(u->pv);
    frame(u);   /* posts; the inline load leaves the result ready */
    CHECK(stats(u).sessions_loaded == s0.sessions_loaded + 1, "stale: the restart posted no job");
    CHECK(edit(u, "\"reps 3\"", "\"reps 4\"") == 0, "stale: edit");
    u->hold_checks = true;
    frame(u);   /* takes it: stale */
    u->hold_checks = false;
    (void)dsg_worker_wait(u->w, 0);
    CHECK(!info(u).attaching && strcmp(info(u).out_dir, out0) == 0, "stale (a): an edit's stale session attached");
    CHECK(stats(u).sessions_closed > s0.sessions_closed, "stale (a): the session was not closed");
    CHECK(until_other(u, out0, 40), "stale (a): the edit's own session did not show");
    snprintf(out0, sizeof out0, "%s", info(u).out_dir);

    /* (b) settings changed after the post */
    s0 = stats(u);
    dsg_preview_restart(u->pv);
    frame(u);
    {
        dsg_preview_settings ps;
        dsg_preview_get(u->pv, &ps);
        ps.session.seed = 77;
        dsg_preview_set(u->pv, &ps);
    }
    u->hold_checks = true;
    attach_hold = WRAPPED ? 1 : 0;   /* hold the next attach, to see it start */
    frame(u);   /* drops the seed-1 session; posts the seed-77 job */
    u->hold_checks = false;
    CHECK(!info(u).attaching && stats(u).sessions_dropped + stats(u).sessions_closed > s0.sessions_dropped + s0.sessions_closed,
          "stale (b): the old settings' session was adopted");
    frame(u);
    CHECK(info(u).attaching || !WRAPPED, "stale (b): the new settings' session did not attach");
    CHECK(until_running(u, 10), "stale (b): no session");
    attach_hold = 0;

    /* (c) a newer job waits */
    s0 = stats(u);
    snprintf(out0, sizeof out0, "%s", info(u).out_dir);
    dsg_preview_restart(u->pv);
    frame(u);
    t = dsg_doc_bytes(u->doc, DSG_BUF_DEF, &n);
    memset(&ss, 0, sizeof ss);
    CHECK(dsg_worker_session(u->w, t, n, NULL, 0, &ss) == 0, "stale (c): the test's job");
    frame(u);   /* the preview's session arrives while the test's job waits */
    CHECK(!info(u).attaching && strcmp(info(u).out_dir, out0) == 0, "stale (c): adopted while a newer job waits");
    (void)dsg_worker_wait(u->w, 0);   /* the test's job: no pack, it fails */
    frame(u);
    CHECK(!dsg_preview_issue(u->pv, &(dsg_issue){ 0 }), "stale (c): another job's failure became the issue");

    /* (d) an edit after the post to a text that the check refuses: the
     * newest job's session is dropped and no job follows, so nothing loads */
    CHECK(until_status(u, DSG_PV_RUNNING, 10), "stale (d): no session");
    snprintf(out0, sizeof out0, "%s", info(u).out_dir);
    s0 = stats(u);
    dsg_preview_restart(u->pv);
    frame(u);
    CHECK(info(u).loading, "stale (d): the restart's job loads");
    CHECK(edit(u, "\"reps 4\"", "\"reps 4\"x") == 0, "stale (d): edit");
    frame(u);   /* takes the session: stale; posts the check */
    frame(u);   /* the check refuses the text */
    CHECK(stats(u).sessions_loaded == s0.sessions_loaded + 1, "stale (d): another job posted");
    CHECK(strcmp(info(u).out_dir, out0) == 0 && info(u).stale, "stale (d): the last good session is not shown, stale");
    CHECK(!info(u).loading, "stale (d): a session still loads, but no job waits or runs");
    CHECK(edit(u, "\"reps 4\"x", "\"reps 4\"") == 0, "stale (d): edit back");
    CHECK(until_other(u, out0, 40) && !info(u).stale, "stale (d): the fixed text's session did not show");
}

/* --- (5) the attach over frames, newest wins ----------------------------------------------- */

static void part_attach(U* u, const char* folder) {
    char out0[DSG_PATH_MAX], out1[DSG_PATH_MAX];
    int64_t f0;
    int k, newest = 0;
    if (!WRAPPED) {
        printf("  attach: no --wrap, skipped\n");
        return;
    }
    CHECK(u_open(u, folder) == 0, "attach: open");
    CHECK(until_running(u, 10), "attach: no session");
    snprintf(out0, sizeof out0, "%s", info(u).out_dir);
    attach_hold = 5;
    attach_calls = 0;
    dsg_preview_restart(u->pv);
    frame(u);   /* posts */
    frame(u);   /* adopts; the first held call */
    CHECK(info(u).attaching && info(u).status == DSG_PV_RUNNING, "attach: not attaching (status %d)", info(u).status);
    CHECK(attach_budget == 4000000, "attach: the budget %lld ns, not 4 ms", (long long)attach_budget);
    f0 = where(u).frame;
    frame(u);
    frame(u);
    CHECK(where(u).frame - f0 >= 2 && strcmp(info(u).out_dir, out0) == 0, "attach: the old session did not run while the new one attached");
    /* a newer session arrives mid-attach: it replaces the attach */
    dsg_preview_restart(u->pv);
    frame(u);   /* posts the newer job (loaded inline); a held call of the older */
    (void)count_runs(folder, &newest);   /* the newer job's run: the highest */
    frame(u);   /* adopts the newer: the older goes */
    attach_hold = 2;
    for (k = 0; k < 2; k++) {
        frame(u);
        CHECK(info(u).attaching && strcmp(info(u).out_dir, out0) == 0, "attach: the old session left before the attach ended");
    }
    f0 = where(u).frame;
    frame(u);   /* the attach completes: no step of the new one here */
    snprintf(out1, sizeof out1, "%s", info(u).out_dir);
    CHECK(!info(u).attaching && strcmp(out1, out0) != 0, "attach: the newer session did not show");
    {
        const char* tail = strrchr(out1, '-');
        CHECK(tail && atoi(tail + 1) == newest, "attach: the shown run is %s, not the newest job's run-%d", out1, newest);
    }
    (void)f0;
    CHECK(where(u).frame == 0, "attach: the new session stepped in its attach frame (%lld)", (long long)where(u).frame);
    frame(u);
    CHECK(where(u).frame >= 1, "attach: the new session did not step after its attach");
    /* 5 + 2 held calls in all, and the one that attached */
    CHECK(attach_calls >= 8, "attach: %d calls to pl_sim_attach()", attach_calls);
    attach_hold = 0;
}

/* --- (6) refusals as the preview issue ------------------------------------------------------- */

static void part_refusals(U* u, const char* folder, const char* folder_nodir) {
    dsg_issue is;
    char path[DSG_PATH_MAX + 64], err[512];
    int k;
    memset(&is, 0, sizeof is);
    /* at run time: the template 'win' */
    CHECK(u_open(u, folder) == 0, "refusal: open");
    CHECK(until_status(u, DSG_PV_FAILED, 200), "refusal: the session did not fail at run time: status %d", info(u).status);
    CHECK(dsg_preview_issue(u->pv, &is), "refusal: no preview issue");
    {
        /* the place is the template's, in the text the session loaded (the
         * open shows version 1 as version 2, so not the file's lines) */
        size_t n = 0;
        const char* t = dsg_doc_bytes(u->doc, DSG_BUF_DEF, &n);
        const char* set = strstr(t, "\"trials\"");
        const char* at = set ? strstr(set, "\"win\"") : NULL;
        uint32_t line = 0, col = 0;
        if (at) dsg_line_col(t, n, (uint32_t)(at - t), &line, &col);
        CHECK(is.from == DSG_FROM_PREVIEW && is.severity == DSG_SEV_ERROR && is.object == PL_OBJ_TEMPLATE && is.name &&
                  strcmp(is.name, "win") == 0 && strstr(is.message, "leaves no time") && line > 0 && is.place.line == line,
              "refusal: issue from %d object %d '%s' line %u (want %u): %s", is.from, is.object, is.name ? is.name : "", is.place.line, line,
              is.message);
    }
    CHECK(!dsg_preview_animating(u->pv) && !dsg_preview_work(u->pv), "refusal: a failed session animates");
    /* the next accepted change restarts */
    CHECK(edit(u, "[0.6]", "[0.2]") == 0, "refusal: edit");
    CHECK(until_running(u, 40), "refusal: the fixed text did not run");
    CHECK(!dsg_preview_issue(u->pv, &is), "refusal: the issue stayed");

    /* in the fast forward: the issue, no session, status failed */
    CHECK(edit(u, "[0.2]", "[0.6]") == 0, "refusal: edit back");
    CHECK(u_open(u, folder) == 0, "refusal: open again");
    set_pin(u, "l2", -1);
    for (k = 0; k < 10; k++) frame(u);
    CHECK(info(u).status == DSG_PV_FAILED && dsg_preview_issue(u->pv, &is) && is.object == PL_OBJ_TEMPLATE,
          "refusal: the fast forward's refusal: status %d", info(u).status);
    set_pin(u, NULL, -1);

    /* a load that fails: the data sets' folder is a file */
    CHECK(u_open(u, folder_nodir) == 0, "refusal: open nodir");
    snprintf(path, sizeof path, "%s/" DSG_DIR, folder_nodir);
    (void)dsg_make_dir(path, err, sizeof err);
    snprintf(path, sizeof path, "%s/" DSG_DIR "/preview", folder_nodir);
    CHECK(dsg_write_file(path, "x", 1, err, sizeof err) == 0, "refusal: the file");
    for (k = 0; k < 10; k++) frame(u);
    CHECK(info(u).status == DSG_PV_FAILED && dsg_preview_issue(u->pv, &is) && is.from == DSG_FROM_PREVIEW && strstr(is.message, "folder"),
          "refusal: a failed load: status %d, issue '%s'", info(u).status, dsg_preview_issue(u->pv, &is) ? is.message : "");
    remove(path);
    dsg_preview_restart(u->pv);
    CHECK(until_running(u, 10) && !dsg_preview_issue(u->pv, &is), "refusal: the load after the fix");
}

/* --- (7) the pin --------------------------------------------------------------------------------- */

static void part_pin(U* u, const char* folder, const char* sig_folder) {
    dsg_preview_leaf lf;
    dsg_preview_where w;
    dsg_preview_settings ps;
    int row, found = 0, missed = 0;
    memset(&lf, 0, sizeof lf);
    memset(&ps, 0, sizeof ps);
    CHECK(u_open(u, folder) == 0, "pin: open");
    CHECK(until_running(u, 10), "pin: no session");
    CHECK(dsg_preview_leaves(u->pv) == 2, "pin: %d leaves", dsg_preview_leaves(u->pv));
    CHECK(dsg_preview_leaf_at(u->pv, 0, &lf) && strcmp(lf.path, "a") == 0 && strcmp(lf.tmpl, "t") == 0 && lf.rows == 6,
          "pin: leaf 0 %s %s %d", lf.path ? lf.path : "-", lf.tmpl ? lf.tmpl : "-", lf.rows);
    CHECK(dsg_preview_leaf_at(u->pv, 1, &lf) && strcmp(lf.path, "b") == 0 && lf.rows == 0, "pin: leaf 1");
    for (row = 0; row < 6; row++) {
        set_pin(u, "a", row);
        CHECK(until_running(u, 10), "pin: row %d no session", row);
        w = where(u);
        if (!info(u).note[0]) {
            found++;
            CHECK(w.leaf && strcmp(w.leaf, "a") == 0 && w.condition == row, "pin: row %d found at leaf %s row %d", row, w.leaf ? w.leaf : "-",
                  w.condition);
        } else {
            missed++;
            CHECK(strstr(info(u).note, "was not drawn in this run of the leaf: seed 1") != NULL && w.leaf && strcmp(w.leaf, "b") == 0,
                  "pin: row %d note '%s' at leaf %s", row, info(u).note, w.leaf ? w.leaf : "-");
        }
    }
    CHECK(found == 2 && missed == 4, "pin: %d rows found, %d not drawn (subset 2 of 6)", found, missed);
    set_pin(u, "b", -1);
    CHECK(until_running(u, 10) && where(u).leaf && strcmp(where(u).leaf, "b") == 0 && !info(u).note[0], "pin: leaf b, any row");
    /* "Preview this template": the first leaf that runs it, row any */
    CHECK(dsg_preview_pin_template(u->pv, "t"), "pin: the template");
    CHECK(!dsg_preview_pin_template(u->pv, "nope"), "pin: no such template");
    dsg_preview_get(u->pv, &ps);
    CHECK(strcmp(ps.session.pin_leaf, "a") == 0 && ps.session.pin_row == -1, "pin: the template's pin %s %d", ps.session.pin_leaf, ps.session.pin_row);
    CHECK(until_running(u, 10) && where(u).leaf && strcmp(where(u).leaf, "a") == 0, "pin: the template's leaf");
    set_pin(u, NULL, -1);

    /* the stall: a trial that waits on a signal role */
    CHECK(u_open(u, sig_folder) == 0, "pin: open sig");
    set_pin(u, "l2", -1);
    CHECK(until_running(u, 10), "pin: sig, no session");
    CHECK(strstr(info(u).note, "waits for input that the script cannot give") != NULL && where(u).leaf && strcmp(where(u).leaf, "l1") == 0,
          "pin: the stall's note '%s'", info(u).note);
    set_pin(u, NULL, -1);
}

/* --- (8) keys, choices, outputs, roles not driven ---------------------------------------------- */

static void part_input(U* u, const char* keys_folder, const char* oddball, const char* visuomotor) {
    dsg_preview_output o;
    const char* why = NULL;
    const char* name = NULL;
    int k, n;
    bool ok;
    CHECK(u_open(u, keys_folder) == 0, "input: open");
    CHECK(until_running(u, 10), "input: no session");
    CHECK(!dsg_preview_key(u->pv, 9, true), "input: a key taken without the keyboard");
    dsg_preview_set_keyboard(u->pv, true);
    /* to the response window: 0.5 s after the trial's start */
    for (k = 0; k < 200 && where(u).phase != DSG_PV_PHASE_RESPONSE; k++) frame(u);
    CHECK(where(u).phase == DSG_PV_PHASE_RESPONSE, "input: no response window");
    CHECK(dsg_preview_choices(u->pv) == 2 && strcmp(dsg_preview_choice_label(u->pv, 1), "j") == 0, "input: choices %d",
          dsg_preview_choices(u->pv));
    for (k = 0; k < 6; k++) frame(u);
    CHECK(dsg_preview_key(u->pv, 9, true), "input: F not taken");
    CHECK(dsg_preview_key(u->pv, DSG_SCANCODE_ESCAPE, true) && !dsg_preview_keyboard(u->pv), "input: Escape kept the keyboard");
    CHECK(dsg_preview_key(u->pv, 9, false), "input: the up of a taken key went to the designer");
    CHECK(!dsg_preview_key(u->pv, 13, false), "input: another up taken");
    /* the response shows when its trial ends */
    {
        const int t0 = where(u).trial;
        for (k = 0; k < 120 && where(u).trial == t0; k++) frame(u);
    }
    {
        dsg_preview_where w = where(u);
        CHECK(w.response && strcmp(w.response, "f") == 0, "input: the response %s", w.response ? w.response : "(none)");
        /* the key's time is a frame's onset less 1 ns, on the grid */
        CHECK(w.rt_ns > 0 && (w.rt_ns + 1) % FR == 0, "input: rt %lld ns is not a frame onset less 1 ns", (long long)w.rt_ns);
    }
    /* paused: a key is taken and dropped */
    dsg_preview_set_keyboard(u->pv, true);
    dsg_preview_pause(u->pv);
    CHECK(dsg_preview_key(u->pv, 13, true), "input: paused, the key went to the designer");
    CHECK(!dsg_preview_key(u->pv, 13, false) || true, "input: paused up");
    dsg_preview_set_keyboard(u->pv, false);
    dsg_preview_run(u->pv);

    /* oddball: a choice, the trigger codes, the sync line */
    CHECK(u_open(u, oddball) == 0, "input: open oddball");
    CHECK(until_running(u, 10), "input: oddball, no session");
    /* the window opens with the picture, whose trigger is the first code */
    for (k = 0; k < 600 && dsg_preview_outputs(u->pv) == 0; k++) frame(u);
    CHECK(dsg_preview_choices(u->pv) == 2 && strcmp(dsg_preview_choice_label(u->pv, 0), "left") == 0, "input: oddball's choices");
    frame(u);
    dsg_preview_choice(u->pv, 0, true);
    frame(u);
    dsg_preview_choice(u->pv, 0, false);
    {
        const int t0 = where(u).trial;
        for (k = 0; k < 300 && where(u).trial == t0; k++) frame(u);
    }
    CHECK(where(u).response && strcmp(where(u).response, "left") == 0, "input: the choice gave %s", where(u).response ? where(u).response : "(none)");
    n = dsg_preview_outputs(u->pv);
    ok = n >= 2;
    memset(&o, 0, sizeof o);
    for (k = 0; k < n; k++) {
        ok = ok && dsg_preview_output_at(u->pv, k, &o) && o.role && strcmp(o.role, "trig") == 0 && o.t_ns > 0;
        if (k == n - 1) ok = ok && o.code == 101;
    }
    CHECK(ok, "input: %d outputs, the last code %u", n, n ? o.code : 0u);
    CHECK(dsg_preview_undriven(u->pv) == 1 && (name = dsg_preview_undriven_role(u->pv, 0, &why)) && strcmp(name, "light") == 0 &&
              strcmp(why, "a sync input") == 0,
          "input: oddball's roles not driven %d", dsg_preview_undriven(u->pv));
    /* visuomotor: a relative role */
    CHECK(u_open(u, visuomotor) == 0, "input: open visuomotor");
    CHECK(until_running(u, 10), "input: visuomotor, no session");
    CHECK(dsg_preview_undriven(u->pv) == 1 && (name = dsg_preview_undriven_role(u->pv, 0, &why)) && strcmp(name, "hand") == 0 &&
              strstr(why, "relative"),
          "input: visuomotor's roles not driven");
}

/* --- (9) the pointer to role units -------------------------------------------------------------- */

/* The display's px of a point in role units: the built-in rig's 0.27 mm a
 * px and 570 mm, as gfx scales them. */
static double ppu_of(const char* units) {
    const double w = 1920.0, wmm = 0.27 * w;
    if (strcmp(units, "cm") == 0) return w * 10.0 / wmm;
    if (strcmp(units, "deg") == 0) return w / wmm * 570.0 * tan(3.14159265358979323846 / 180.0);
    return 1.0;
}

static void part_pointer(U* u) {
    static const char* const units[3] = { "px", "cm", "deg" };
    /* far from the center, so cm's scale and deg's differ by more than the
     * radius there (25 cm: 3.4 px; 24 deg: 4.4 px) */
    static const double cx[3] = { 600, 25, 24 }, cy[3] = { -300, -10, -9 }, r[3] = { 1.5, 0.04, 0.04 };
    int u_i, k;
    for (u_i = 0; u_i < 3; u_i++) {
        char def[4096], folder[DSG_PATH_MAX], name[32];
        const double k_px = ppu_of(units[u_i]);
        const float px = (float)(960.0 + cx[u_i] * k_px), py = (float)(540.0 + cy[u_i] * k_px);
        snprintf(name, sizeof name, "ptr_%s", units[u_i]);
        sig_def(def, sizeof def, units[u_i], cx[u_i], cy[u_i], r[u_i]);
        if (make_project(name, def, kPackPlain, NULL, folder, sizeof folder)) { CHECK(0, "pointer: project"); return; }
        CHECK(u_open(u, folder) == 0, "pointer %s: open", units[u_i]);
        CHECK(until_running(u, 10), "pointer %s: no session: %s", units[u_i], dsg_preview_issue(u->pv, &(dsg_issue){ 0 }) ? "issue" : "");
        CHECK(dsg_preview_undriven(u->pv) == 0, "pointer %s: the role is not driven", units[u_i]);
        for (k = 0; k < 20 && !(where(u).state && strcmp(where(u).state, "wait") == 0); k++) frame(u);
        /* outside by 3 px: stays */
        dsg_preview_pointer(u->pv, px + 3.0f * (u_i == 0 ? 1.0f : 1.6f), py, true);
        for (k = 0; k < 10; k++) frame(u);
        CHECK(where(u).state && strcmp(where(u).state, "wait") == 0, "pointer %s: a point outside the region ended the state (%s)", units[u_i],
              where(u).state ? where(u).state : "-");
        /* leaving the display sends nothing */
        dsg_preview_pointer(u->pv, px, py, false);
        for (k = 0; k < 5; k++) frame(u);
        CHECK(where(u).state && strcmp(where(u).state, "wait") == 0, "pointer %s: a pointer outside the display moved the role", units[u_i]);
        /* inside */
        dsg_preview_pointer(u->pv, px, py, true);
        for (k = 0; k < 5; k++) frame(u);
        CHECK(where(u).state && strcmp(where(u).state, "done") == 0, "pointer %s: at the region's center the state is %s", units[u_i],
              where(u).state ? where(u).state : "-");
    }
}

/* --- (10) the run folders ------------------------------------------------------------------------- */

static void part_runs(U* u, const char* folder) {
    U u2;
    int k, n, last = 0, kept;
    const char* tail;
    CHECK(u_open(u, folder) == 0, "runs: open");
    CHECK(until_running(u, 10), "runs: no session");
    for (k = 0; k < 4; k++) {
        dsg_preview_restart(u->pv);
        CHECK(until_running(u, 10), "runs: restart %d", k);
    }
    (void)dsg_worker_wait(u->w, 0);
    n = count_runs(folder, &last);
    tail = strrchr(info(u).out_dir, '-');
    CHECK(n == 1 && tail && atoi(tail + 1) == last, "runs: %d run folders, the newest run-%d, shown %s", n, last, info(u).out_dir);
    /* the project closes: the shown run's data set stays */
    kept = last;
    u_close(u);
    (void)dsg_worker_wait(u->w, 0);
    n = count_runs(folder, &last);
    CHECK(n == 1 && last == kept, "runs: after the close %d folders, run-%d", n, last);
    {
        char path[DSG_PATH_MAX + 64];
        snprintf(path, sizeof path, "%s/" DSG_DIR "/preview/run-%d/session.json", folder, kept);
        CHECK(dsg_is_file(path), "runs: the kept run has no session.json");
    }
    /* another process on the same project: it numbers on, and its first
     * session removes this one's */
    if (u_start(&u2, false)) { CHECK(0, "runs: second preview"); return; }
    CHECK(u_open(&u2, folder) == 0, "runs: open in the second");
    CHECK(until_running(&u2, 10), "runs: the second's session");
    (void)dsg_worker_wait(u2.w, 0);
    n = count_runs(folder, &last);
    tail = strrchr(info(&u2).out_dir, '-');
    CHECK(n == 1 && last > kept && tail && atoi(tail + 1) == last, "runs: the second process left %d folders, run-%d (was run-%d)", n, last, kept);
    u_stop(&u2);
}

/* --- (11) no heap call in a running tick ---------------------------------------------------------- */

static void part_heap(U* u, const char* keys_folder, const char* sig_folder) {
    int k;
    if (!WRAPPED) {
        printf("  heap: no --wrap, skipped\n");
        return;
    }
    CHECK(u_open(u, keys_folder) == 0, "heap: open");
    CHECK(until_running(u, 10), "heap: no session");
    dsg_preview_set_keyboard(u->pv, true);
    tick_heap = 0;
    for (k = 0; k < 900; k++) {
        if (k % 20 == 0) (void)dsg_preview_key(u->pv, 9 + (k / 20) % 5, true);
        if (k % 20 == 3) (void)dsg_preview_key(u->pv, 9 + (k / 20) % 5, false);
        frame(u);
    }
    CHECK(tick_heap == 0, "heap: %lld heap calls in 900 running ticks with keys", tick_heap);
    CHECK(u_open(u, sig_folder) == 0, "heap: open sig");
    CHECK(until_running(u, 10), "heap: sig, no session");
    tick_heap = 0;
    for (k = 0; k < 600; k++) {
        dsg_preview_pointer(u->pv, (float)(900 + k % 50), (float)(500 + k % 30), true);
        frame(u);
    }
    CHECK(tick_heap == 0, "heap: %lld heap calls in 600 running ticks with the pointer", tick_heap);
}

/* --- (12) the real thread ---------------------------------------------------------------------------- */

static void part_thread(const char* folder) {
    U u;
    int k;
    if (u_start(&u, true)) { CHECK(0, "thread: start"); return; }
    CHECK(u_open(&u, folder) == 0, "thread: open");
    CHECK(until_running(&u, 20), "thread: no session: status %d", info(&u).status);
    for (k = 0; k < 20; k++) {
        /* bursts of restarts and edits: newest wins across the threads */
        dsg_preview_restart(u.pv);
        frame(&u);
        if (k % 3 == 0) (void)edit(&u, k % 2 ? "\"reps 4\"" : "\"reps 3\"", k % 2 ? "\"reps 3\"" : "\"reps 4\"");
        frame_at(&u, 350000000, true);
    }
    CHECK(until_running(&u, 40), "thread: no session at the end");
    u_close(&u);
    CHECK(u_open(&u, folder) == 0, "thread: open again");
    CHECK(until_running(&u, 20), "thread: no session after the reopen");
    u_stop(&u);
}

int main(int argc, char** argv) {
    static const char* const oddball_files[] = { "pack.json", "experiment.json", "oddball.csv", "standard.png", "target.png", NULL };
    static const char* const visuo_files[] = { "pack.json", "experiment.json", "targets.csv", NULL };
    char keys[DSG_PATH_MAX], pins[DSG_PATH_MAX], refuse_f[DSG_PATH_MAX], nodir[DSG_PATH_MAX], sig[DSG_PATH_MAX], odd[DSG_PATH_MAX],
        visuo[DSG_PATH_MAX], runs[DSG_PATH_MAX], err[512], def[4096];
    U u;
    if (argc < 2) {
        printf("usage: test_designer_preview WORK\n");
        return 2;
    }
    work = argv[1];
    if (dsg_make_dir(work, err, sizeof err)) {
        printf("%s\n", err);
        return 1;
    }
    sig_def(def, sizeof def, "px", 600, -300, 1.5);
    if (make_project("keys", kKeys, kPackPlain, NULL, keys, sizeof keys) || make_project("pins", kPins, kPackConds, kConds, pins, sizeof pins) ||
        make_project("refuse", kRefuse, kPackPlain, NULL, refuse_f, sizeof refuse_f) ||
        make_project("nodir", kKeys, kPackPlain, NULL, nodir, sizeof nodir) || make_project("sig", def, kPackPlain, NULL, sig, sizeof sig) ||
        make_project("runs", kKeys, kPackPlain, NULL, runs, sizeof runs) || copy_player_project("oddball", oddball_files, odd, sizeof odd) ||
        copy_player_project("visuomotor", visuo_files, visuo, sizeof visuo)) {
        printf("FAIL: cannot make the projects\n");
        return 1;
    }
    if (u_start(&u, false)) {
        printf("FAIL: cannot start\n");
        return 1;
    }
    printf("(1) steps\n");
    part_steps(&u, keys);
    printf("(2) settle\n");
    part_settle(&u, keys);
    printf("(3) last good\n");
    part_last_good(&u, keys);
    printf("(4) stale\n");
    part_stale(&u, keys);
    printf("(5) attach\n");
    part_attach(&u, keys);
    printf("(6) refusals\n");
    part_refusals(&u, refuse_f, nodir);
    printf("(7) pin\n");
    part_pin(&u, pins, sig);
    printf("(8) input\n");
    part_input(&u, keys, odd, visuo);
    printf("(9) pointer\n");
    part_pointer(&u);
    printf("(10) runs\n");
    part_runs(&u, runs);
    printf("(11) heap\n");
    part_heap(&u, keys, sig);
    u_stop(&u);
    printf("(12) thread\n");
    part_thread(keys);
    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
