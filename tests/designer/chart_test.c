/* chart_test.c - the state chart's model, commands and view state with no
 * window (designer/core/chart.h; step 4, part A; docs/designer.md, "The
 * chart"). OWNED BY PART A.
 *
 * Usage: test_designer_chart WORK
 *   WORK is a folder the test may write: it copies projects into it and
 *   builds their check packs there.
 *
 *   1. The model of each test project with states (bimanual, visuomotor)
 *      and of v2_retry (shared exits, `over`, a state with no exits in the
 *      middle): states, the end, and edges of each kind with their targets
 *      and labels. An events template is read-only, split at `until`.
 *   2. Places: a selection's or an issue's path to its state or edge.
 *   3. Commands: add, connect, delete a state (its exits, shared exits and
 *      `in` entries, `over`), delete an exit, rename (every `to`, `in`,
 *      `over.to`, a control's `from`), set first. Each is one undo step,
 *      undoes to the same bytes, redoes to the same bytes, and leaves the
 *      player's check clean where the definition allows. Refusals make no
 *      step.
 *   4. View state: a rename moves the position, undo moves it back, redo
 *      again; a template's rename moves its entry; the "chart" part
 *      round-trips through the view file; the definition's bytes never
 *      hold a position.
 * Exit 0 on pass, 1 on a failure.
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "chart.h"
#include "dsg.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef YSP_SOURCE_DIR
#define YSP_SOURCE_DIR "."
#endif

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const char* work;
static dsg_checker* checker;

/* --- helpers ----------------------------------------------------------------- */

static int copy_file(const char* from, const char* to) {
    char* d = NULL;
    size_t n = 0;
    char err[512];
    if (dsg_read_file(from, &d, &n, err, sizeof err)) { printf("  %s\n", err); return -1; }
    if (dsg_write_file(to, d, n, err, sizeof err)) { printf("  %s\n", err); free(d); return -1; }
    free(d);
    return 0;
}

/* WORK/copy with pack.json from tests/player/NAME and experiment.json from
 * def (relative to the source tree), opened as a project. */
static int project(const char* name, const char* def, const char* copy, dsg_project* p) {
    char src[DSG_PATH_MAX], dir[DSG_PATH_MAX], a[DSG_PATH_MAX], b[DSG_PATH_MAX], err[512];
    snprintf(src, sizeof src, "%s/tests/player/%s", YSP_SOURCE_DIR, name);
    if (dsg_join(dir, sizeof dir, work, copy) || dsg_make_dir(dir, err, sizeof err)) return -1;
    if (dsg_join(a, sizeof a, src, "pack.json") || dsg_join(b, sizeof b, dir, "pack.json") || copy_file(a, b)) return -1;
    snprintf(a, sizeof a, "%s/%s", YSP_SOURCE_DIR, def);
    if (dsg_join(b, sizeof b, dir, "experiment.json") || copy_file(a, b)) return -1;
    if (strcmp(name, "visuomotor") == 0 &&
        (dsg_join(a, sizeof a, src, "targets.csv") || dsg_join(b, sizeof b, dir, "targets.csv") || copy_file(a, b)))
        return -1;
    if (dsg_join(b, sizeof b, dir, DSG_VIEW_FILE) == 0) remove(b);
    if (dsg_project_open(p, dir, err, sizeof err)) { printf("  open %s: %s\n", dir, err); return -1; }
    return 0;
}

/* The player's check of the document's definition: its errors, or -1 when
 * it did not run. Errors are shown when one is not expected. */
static int check_errors(dsg_project* p, dsg_doc* d, bool show) {
    dsg_report r;
    size_t n = 0;
    const char* t = dsg_doc_bytes(d, DSG_BUF_DEF, &n);
    int i;
    memset(&r, 0, sizeof r);
    if (dsg_check(checker, p, t, n, NULL, &r)) return -1;
    for (i = 0; i < r.n_issues && i < 3; i++)
        if (show && r.issues[i].severity == DSG_SEV_ERROR) printf("  check: %s\n", r.issues[i].message);
    return r.status == DSG_CHECK_OK ? r.n_errors : (r.n_errors ? r.n_errors : 1);
}

static int errors(dsg_project* p, dsg_doc* d) { return check_errors(p, d, true); }

typedef struct snap {
    char*  b;
    size_t n;
} snap;

static snap take(dsg_doc* d) {
    snap s = { NULL, 0 };
    const char* t = dsg_doc_bytes(d, DSG_BUF_DEF, &s.n);
    s.b = (char*)malloc(s.n + 1);
    if (s.b) memcpy(s.b, t, s.n + 1);
    return s;
}

static bool same(dsg_doc* d, const snap* s) {
    size_t n = 0;
    const char* t = dsg_doc_bytes(d, DSG_BUF_DEF, &n);
    return s->b && n == s->n && memcmp(t, s->b, n) == 0;
}

/* A command made one step; undo gives the bytes before, redo the bytes
 * after; the document ends at the bytes after. */
static void one_step(dsg_doc* d, int depth0, const snap* before, const char* what) {
    snap after = take(d);
    CHECK(dsg_doc_undo_depth(d) == depth0 + 1, "%s: one undo step (%d, was %d)", what, dsg_doc_undo_depth(d), depth0);
    CHECK(dsg_doc_undo(d) == 0 && same(d, before), "%s: undo gives the same bytes", what);
    CHECK(dsg_doc_redo(d) == 0 && same(d, &after), "%s: redo gives the same bytes again", what);
    free(after.b);
}

static const dsg_chart_edge* edge(const dsg_chart* c, int from, int kind, int k) {
    int i;
    for (i = 0; i < c->n_edges; i++)
        if (c->edges[i].from == from && c->edges[i].kind == kind && k-- == 0) return &c->edges[i];
    return NULL;
}

static int count_kind(const dsg_chart* c, int kind) {
    int i, n = 0;
    for (i = 0; i < c->n_edges; i++) n += c->edges[i].kind == kind;
    return n;
}

static bool has_text(dsg_doc* d, const char* s) {
    size_t n = 0;
    return strstr(dsg_doc_bytes(d, DSG_BUF_DEF, &n), s) != NULL;
}

/* --- 1. the model -------------------------------------------------------------- */

static void test_model(void) {
    dsg_project p;
    dsg_doc* d = dsg_doc_new();
    dsg_chart c;
    const dsg_chart_edge* e;
    memset(&c, 0, sizeof c);
    if (project("bimanual", "tests/player/bimanual/experiment.json", "model_bimanual", &p)) { CHECK(0, "copy bimanual"); return; }
    dsg_doc_reset(d, "experiment.json", p.text, p.text_len);

    CHECK(dsg_chart_build(dsg_doc_root(d), "nope", &c) == -1 && c.n_states == 0, "no such template");
    CHECK(dsg_chart_build(dsg_doc_root(d), "track", &c) == 0 && c.has_states && c.n_states == 3, "track: 3 states (%d)", c.n_states);
    CHECK(strcmp(c.tmpl, "track") == 0 && strcmp(c.states[0].name, "iti") == 0 && strcmp(c.states[2].name, "move") == 0,
          "track: its name and its states in order");
    CHECK(c.states[0].n_steps == 3 && c.states[0].n_exits == 0 && c.states[1].n_exits == 1, "track: steps and exits counted");
    CHECK(c.n_edges == 3, "track: 3 edges (%d)", c.n_edges);
    e = edge(&c, 0, DSG_EDGE_NEXT, 0);
    CHECK(e && e->to == 1 && strcmp(e->label, "steps end") == 0 && e->to_name[0] == 0, "iti, no exits: next listed state (rule 10)");
    e = edge(&c, 1, DSG_EDGE_EXIT, 0);
    CHECK(e && e->to == 2 && e->index == 0 && strcmp(e->label, "on ready") == 0 && strcmp(e->to_name, "move") == 0,
          "wait: on ready to move ('%s')", e ? e->label : "");
    e = edge(&c, 2, DSG_EDGE_NEXT, 0);
    CHECK(e && e->to == 3, "move, last with no exits: the end (n_states)");

    CHECK(dsg_chart_build(dsg_doc_root(d), "reach", &c) == 0 && c.n_states == 3 && c.n_edges == 4, "reach: 3 states, 4 edges (%d)",
          c.n_edges);
    e = edge(&c, 0, DSG_EDGE_EXIT, 0);
    CHECK(e && e->to == 1 && strcmp(e->label, "instant, if $first_in_block") == 0, "begin: instant if ('%s')", e ? e->label : "");
    e = edge(&c, 0, DSG_EDGE_EXIT, 1);
    CHECK(e && e->to == 2 && e->index == 1 && strcmp(e->label, "instant") == 0, "begin: instant to go");
    e = edge(&c, 2, DSG_EDGE_EXIT, 0);
    CHECK(e && e->to == 3 && strcmp(e->to_name, "end") == 0, "go: to the end");
    CHECK(count_kind(&c, DSG_EDGE_NEXT) == 0, "reach: every state has exits, so no next edge");
    CHECK(dsg_chart_find(&c, "go") == 2 && dsg_chart_find(&c, "end") == -1, "find by name");
    dsg_project_close(&p);

    if (project("visuomotor", "tests/player/visuomotor/experiment.json", "model_visuomotor", &p)) { CHECK(0, "copy visuomotor"); return; }
    dsg_doc_reset(d, "experiment.json", p.text, p.text_len);
    CHECK(dsg_chart_build(dsg_doc_root(d), "reach", &c) == 0 && c.n_states == 3 && c.n_edges == 3, "visuomotor: 3 states, 3 edges");
    e = edge(&c, 1, DSG_EDGE_EXIT, 0);
    CHECK(e && e->to == 2 && strcmp(e->label, "on past_target") == 0, "go: on past_target to show_end");
    e = edge(&c, 2, DSG_EDGE_NEXT, 0);
    CHECK(e && e->to == 3, "show_end: the end");
    dsg_project_close(&p);

    if (project("bimanual", "tests/experiment/good/v2_retry.json", "model_retry", &p)) { CHECK(0, "copy retry"); return; }
    dsg_doc_reset(d, "experiment.json", p.text, p.text_len);
    CHECK(dsg_chart_build(dsg_doc_root(d), "trial", &c) == 0 && c.n_states == 8, "retry: 8 states");
    CHECK(count_kind(&c, DSG_EDGE_SHARED) == 6, "retry: a shared edge per `in` state, 2 + 4 (%d)", count_kind(&c, DSG_EDGE_SHARED));
    e = edge(&c, 1, DSG_EDGE_SHARED, 0);
    CHECK(e && e->to == 5 && e->index == 0 && e->in_index == 0 && strcmp(e->label, "on broke") == 0, "cue: shared on broke");
    e = edge(&c, 3, DSG_EDGE_SHARED, 0);
    CHECK(e == NULL, "pause is in no `in`");
    e = edge(&c, 4, DSG_EDGE_SHARED, 0);
    CHECK(e && e->to == 8 && e->index == 1 && e->in_index == 3 && strcmp(e->label, "on lost, outcome lost") == 0,
          "go: shared on lost to the end ('%s')", e ? e->label : "");
    e = edge(&c, 0, DSG_EDGE_OVER, 0);
    CHECK(e && e->to == 8 && strcmp(e->label, "over 3 visits, outcome too_many_breaks") == 0 && c.states[0].max_visits == 3,
          "fixate: over to the end");
    e = edge(&c, 0, DSG_EDGE_EXIT, 1);
    CHECK(e && strcmp(e->label, "after 5 s, outcome timeout") == 0, "an after_s exit's label ('%s')", e ? e->label : "");
    e = edge(&c, 3, DSG_EDGE_NEXT, 0);
    CHECK(e && e->to == 4, "pause: next listed (go)");
    CHECK(count_kind(&c, DSG_EDGE_NEXT) == 1, "only pause has a next edge");
    {
        /* cue to delay (its exit) and delay to... cue has a shared edge to
         * note_break, and fixate two edges to the end: the second bows */
        const dsg_chart_edge* a = edge(&c, 0, DSG_EDGE_EXIT, 1);
        const dsg_chart_edge* b = edge(&c, 0, DSG_EDGE_OVER, 0);
        CHECK(a && b && a->slot == 0 && b->slot > 0, "two edges between one pair bow apart (slots %d, %d)", a ? a->slot : -1,
              b ? b->slot : -1);
    }
    dsg_project_close(&p);

    /* an events template: its parts split at until, read-only */
    {
        static const char def[] =
            "{\"trials\": {\"e\": {\"events\": [{\"on\": \"a\"}, {\"wait_s\": 1}, {\"until\": \"c\", \"timeout_s\": 2}, {\"off\": \"a\"}]},"
            " \"none\": {\"events\": []}, \"neither\": {}}}";
        char path[DSG_SEL_PATH];
        dsg_doc_reset(d, "experiment.json", def, sizeof def - 1);
        CHECK(dsg_chart_build(dsg_doc_root(d), "e", &c) == 0 && !c.has_states && c.n_states == 3, "events: 3 parts (%d)", c.n_states);
        CHECK(strcmp(c.states[0].name, "steps 1 to 2") == 0 && strcmp(c.states[1].name, "until c") == 0 &&
              strcmp(c.states[2].name, "step 4") == 0, "the parts' names");
        CHECK(c.n_edges == 4 && edge(&c, 1, DSG_EDGE_EXIT, 0) && edge(&c, 1, DSG_EDGE_EXIT, 0)->to == 2 &&
              edge(&c, 1, DSG_EDGE_EXIT, 1) && edge(&c, 1, DSG_EDGE_EXIT, 1)->to == 3 && edge(&c, 2, DSG_EDGE_NEXT, 0)->to == 3,
              "until: on c to the next part, its timeout to the end");
        CHECK(dsg_chart_state_path(&c, 1, path, sizeof path) == 0 && strcmp(path, "/trials/e/events/2") == 0, "a part's path is its step");
        CHECK(dsg_chart_add_state(d, "e", "x") == -1 && strstr(dsg_chart_error(), "events") && dsg_doc_undo_depth(d) == 0,
              "commands refuse an events template: %s", dsg_chart_error());
        CHECK(dsg_chart_build(dsg_doc_root(d), "none", &c) == 0 && c.n_states == 1 && c.n_edges == 1, "no events: one empty part");
        CHECK(dsg_chart_build(dsg_doc_root(d), "neither", &c) == -1, "neither states nor events: no chart");
    }
    dsg_chart_free(&c);
    dsg_doc_free(d);
}

/* --- 2. places ------------------------------------------------------------------ */

static void test_places(void) {
    static const char def[] =
        "{\"trials\": {\"a/b\": {\"exits\": [{\"on\": \"x\", \"to\": \"end\", \"in\": [\"s\", \"u\"]}],"
        " \"states\": [{\"name\": \"s\", \"exits\": [{\"to\": \"t\"}]}, {\"name\": \"t\", \"max_visits\": 2, \"over\": {\"to\": \"s\"}},"
        " {\"name\": \"u\"}]}}}";
    dsg_doc* d = dsg_doc_new();
    dsg_chart c;
    dsg_chart_place pl = { -1, -1, -1 };
    char path[DSG_SEL_PATH];
    int i;
    memset(&c, 0, sizeof c);
    dsg_doc_reset(d, "experiment.json", def, sizeof def - 1);
    CHECK(dsg_chart_build(dsg_doc_root(d), "a/b", &c) == 0 && c.n_edges == 5, "a template with '/' in its name: 5 edges (%d)", c.n_edges);
    CHECK(edge(&c, 1, DSG_EDGE_NEXT, 0) && !edge(&c, 2, DSG_EDGE_NEXT, 0) && count_kind(&c, DSG_EDGE_NEXT) == 1,
          "no exits of its own but in a shared exit's `in`: no next edge (the player's rule 10)");
    CHECK(dsg_chart_state_path(&c, 1, path, sizeof path) == 0 && strcmp(path, "/trials/a~1b/states/1") == 0, "a state's path, escaped");
    CHECK(dsg_chart_locate(&c, "/trials/a~1b/states/1/max_visits", &pl) && pl.state == 1 && pl.kind == -1, "a state's key: the state");
    CHECK(dsg_chart_locate(&c, "/trials/a~1b/states/0/exits/0/to", &pl) && pl.state == 0 && pl.kind == DSG_EDGE_EXIT && pl.index == 0,
          "an exit's key: the exit");
    CHECK(dsg_chart_locate(&c, "/trials/a~1b/states/1/over", &pl) && pl.kind == DSG_EDGE_OVER, "over");
    CHECK(dsg_chart_locate(&c, "/trials/a~1b/exits/0/in/0", &pl) && pl.state == -1 && pl.kind == DSG_EDGE_SHARED && pl.index == 0,
          "a shared exit's `in`: the shared exit");
    CHECK(!dsg_chart_locate(&c, "/trials/a~1b/states/7", &pl) && !dsg_chart_locate(&c, "/trials/a~1b/response", &pl) &&
          !dsg_chart_locate(&c, "/trials/a~1b/statesx/0", &pl), "outside the states and the shared exits: nowhere");
    for (i = 0; i < c.n_edges; i++) {
        const dsg_chart_edge* e = &c.edges[i];
        dsg_chart_place q = { -1, -1, -1 };
        if (e->kind == DSG_EDGE_NEXT) {
            CHECK(dsg_chart_edge_path(&c, e, path, sizeof path) == 0 && path[0] == 0, "a next edge has no path");
            continue;
        }
        CHECK(dsg_chart_edge_path(&c, e, path, sizeof path) == 0 && dsg_chart_locate(&c, path, &q) && dsg_chart_place_has_edge(&q, e),
              "edge %d's path '%s' locates to it", i, path);
    }
    dsg_chart_free(&c);
    dsg_doc_free(d);
}

/* --- 3. commands, 4. view state --------------------------------------------------- */

static void test_commands(void) {
    dsg_project p;
    dsg_doc* d = dsg_doc_new();
    dsg_chart c;
    dsg_chart_view v;
    snap s0, s;
    int dep;
    float x = 0, y = 0;
    char err[512] = "";
    memset(&c, 0, sizeof c);
    dsg_chart_view_init(&v);
    if (project("bimanual", "tests/player/bimanual/experiment.json", "cmd_bimanual", &p)) { CHECK(0, "copy bimanual"); return; }
    dsg_doc_reset(d, "experiment.json", p.text, p.text_len);
    CHECK(dsg_doc_listen(d, dsg_chart_view_on_rename, &v) == 0, "listen");
    CHECK(errors(&p, d) == 0, "bimanual checks clean before any command");
    s0 = take(d);

    /* add */
    dep = dsg_doc_undo_depth(d);
    s = take(d);
    CHECK(dsg_chart_add_state(d, "track", "extra") == 0, "add: %s", dsg_chart_error());
    one_step(d, dep, &s, "add");
    CHECK(strcmp(dsg_doc_undo_label(d), "Add state") == 0, "the step's label");
    CHECK(strcmp(dsg_doc_selection(d)->path, "/trials/track/states/3") == 0, "add selects the new state");
    CHECK(dsg_chart_build(dsg_doc_root(d), "track", &c) == 0 && c.n_states == 4 && edge(&c, 2, DSG_EDGE_NEXT, 0)->to == 3 &&
          edge(&c, 3, DSG_EDGE_NEXT, 0)->to == 4, "move then goes to extra, extra to the end");
    CHECK(errors(&p, d) == 0, "clean after add");
    free(s.b);

    /* refusals make no step */
    dep = dsg_doc_undo_depth(d);
    s = take(d);
    CHECK(dsg_chart_add_state(d, "track", "end") == -1 && strstr(dsg_chart_error(), "reserved"), "add 'end': %s", dsg_chart_error());
    CHECK(dsg_chart_add_state(d, "track", "response") == -1, "add 'response'");
    CHECK(dsg_chart_add_state(d, "track", "iti") == -1 && strstr(dsg_chart_error(), "already"), "add a name a state has");
    CHECK(dsg_chart_add_state(d, "track", "") == -1, "add no name");
    CHECK(dsg_chart_add_state(d, "nope", "x") == -1 && strstr(dsg_chart_error(), "nope"), "add to no template: %s", dsg_chart_error());
    CHECK(dsg_chart_connect(d, "track", 9, "end") == -1 && dsg_chart_connect(d, "track", 0, "nowhere") == -1,
          "connect from a stale index, or to no state");
    CHECK(dsg_chart_delete_state(d, "track", 4) == -1 && dsg_chart_set_first(d, "track", -1) == -1 &&
          dsg_chart_rename_state(d, "track", 4, "z") == -1, "stale indices");
    CHECK(dsg_chart_rename_state(d, "track", 0, "wait") == -1, "rename to a name a state has");
    CHECK(dsg_chart_delete_exit(d, "/trials/track/states/0") == -1 && dsg_chart_delete_exit(d, "/trials/track/states/1/exits/5") == -1,
          "delete_exit of a state, or of no exit");
    CHECK(dsg_doc_undo_depth(d) == dep && same(d, &s), "no refusal made a step or changed a byte");
    free(s.b);

    /* connect */
    dep = dsg_doc_undo_depth(d);
    s = take(d);
    CHECK(dsg_chart_connect(d, "track", 3, "end") == 0, "connect extra to the end: %s", dsg_chart_error());
    one_step(d, dep, &s, "connect");
    CHECK(strcmp(dsg_doc_selection(d)->path, "/trials/track/states/3/exits/0") == 0, "connect selects the new exit");
    CHECK(dsg_chart_build(dsg_doc_root(d), "track", &c) == 0 && edge(&c, 3, DSG_EDGE_EXIT, 0) &&
          edge(&c, 3, DSG_EDGE_EXIT, 0)->to == 4 && !edge(&c, 3, DSG_EDGE_NEXT, 0), "extra's exit to the end replaces its next edge");
    CHECK(errors(&p, d) == 0, "clean after connect");
    free(s.b);
    dep = dsg_doc_undo_depth(d);
    s = take(d);
    CHECK(dsg_chart_connect(d, "track", 3, "iti") == 0, "a second exit, to iti");
    one_step(d, dep, &s, "connect to a state");
    CHECK(dsg_chart_build(dsg_doc_root(d), "track", &c) == 0 && edge(&c, 3, DSG_EDGE_EXIT, 1)->to == 0, "the exit goes to iti");
    /* iti, wait, move, extra, iti: ready holds at once, so the loader
     * refuses the cycle (F26); the chart makes it as asked */
    CHECK(check_errors(&p, d, false) > 0, "the check reports the cycle");
    free(s.b);

    /* delete an exit: the second, then the first with its array */
    dep = dsg_doc_undo_depth(d);
    s = take(d);
    CHECK(dsg_chart_delete_exit(d, "/trials/track/states/3/exits/1") == 0, "delete exit: %s", dsg_chart_error());
    one_step(d, dep, &s, "delete exit");
    CHECK(strcmp(dsg_doc_selection(d)->path, "/trials/track") == 0, "delete exit selects the template");
    free(s.b);
    CHECK(dsg_chart_delete_exit(d, "/trials/track/states/3/exits/0") == 0 && !has_text(d, "\"exits\": []"),
          "the last exit takes its empty array with it");
    CHECK(dsg_chart_build(dsg_doc_root(d), "track", &c) == 0 && edge(&c, 3, DSG_EDGE_NEXT, 0), "extra has a next edge again");

    /* rename, with positions */
    CHECK(dsg_chart_view_set_pos(&v, "track", "move", 300, 120) == 0 && dsg_chart_view_set_pos(&v, "track", "wait", 10, 20) == 0,
          "positions set");
    dep = dsg_doc_undo_depth(d);
    s = take(d);
    CHECK(dsg_chart_rename_state(d, "track", 2, "moving") == 0, "rename move: %s", dsg_chart_error());
    one_step(d, dep, &s, "rename");   /* undo, then redo: the listener ran three times */
    CHECK(has_text(d, "\"to\": \"moving\"") && has_text(d, "\"from\": \"moving\"") && !has_text(d, "\"move\""),
          "the exit's `to` and the controls' `from` follow");
    CHECK(strcmp(dsg_doc_selection(d)->path, "/trials/track/states/2") == 0, "rename selects the state");
    CHECK(errors(&p, d) == 0, "clean after rename");
    CHECK(dsg_chart_view_pos(&v, "track", "moving", &x, &y) && x == 300 && y == 120 && !dsg_chart_view_pos(&v, "track", "move", &x, &y),
          "the position follows the rename");
    CHECK(dsg_doc_undo(d) == 0 && dsg_chart_view_pos(&v, "track", "move", &x, &y) && x == 300 &&
          !dsg_chart_view_pos(&v, "track", "moving", &x, &y), "undo moves it back");
    CHECK(dsg_doc_redo(d) == 0 && dsg_chart_view_pos(&v, "track", "moving", &x, &y) && x == 300, "redo moves it again");
    {
        /* a stale position under the new name gives way to the renamed one */
        const int n0 = dsg_chart_view_template(&v, "track", false)->n_pos;
        dsg_chart_view_set_pos(&v, "track", "zz", 5, 5);
        dsg_chart_view_on_rename(&v, "/trials/track/states", "moving", "zz");
        CHECK(dsg_chart_view_pos(&v, "track", "zz", &x, &y) && x == 300 && dsg_chart_view_template(&v, "track", false)->n_pos == n0,
              "one entry, the renamed state's (%g)", x);
        dsg_chart_view_on_rename(&v, "/trials/track/states", "zz", "moving");
        dsg_chart_view_on_rename(&v, "/trials/other/states", "moving", "q");
        dsg_chart_view_on_rename(&v, "/stimuli", "moving", "q");
        CHECK(dsg_chart_view_pos(&v, "track", "moving", &x, &y), "another template's or set's rename moves nothing");
    }
    free(s.b);

    /* set first */
    dep = dsg_doc_undo_depth(d);
    s = take(d);
    CHECK(dsg_chart_set_first(d, "track", 1) == 0, "set first: %s", dsg_chart_error());
    one_step(d, dep, &s, "set first");
    CHECK(dsg_chart_build(dsg_doc_root(d), "track", &c) == 0 && strcmp(c.states[0].name, "wait") == 0 &&
          strcmp(c.states[1].name, "iti") == 0, "wait is first, iti second");
    CHECK(strcmp(dsg_doc_selection(d)->path, "/trials/track/states/0") == 0, "set first selects the state at its new place");
    CHECK(dsg_chart_set_first(d, "track", 0) == 0 && dsg_doc_undo_depth(d) == dep + 1, "the first again: no step");
    free(s.b);
    CHECK(dsg_chart_set_first(d, "track", 1) == 0 && dsg_chart_build(dsg_doc_root(d), "track", &c) == 0 &&
          strcmp(c.states[0].name, "iti") == 0, "iti first again");

    /* delete a state: hold_start, with begin's exit to it */
    dep = dsg_doc_undo_depth(d);
    s = take(d);
    CHECK(dsg_chart_delete_state(d, "reach", 1) == 0, "delete hold_start: %s", dsg_chart_error());
    one_step(d, dep, &s, "delete state");
    CHECK(dsg_chart_build(dsg_doc_root(d), "reach", &c) == 0 && c.n_states == 2 && c.states[0].n_exits == 1 &&
          strcmp(edge(&c, 0, DSG_EDGE_EXIT, 0)->to_name, "go") == 0, "begin keeps only its exit to go");
    CHECK(strcmp(dsg_doc_selection(d)->path, "/trials/reach") == 0, "delete selects the template");
    CHECK(errors(&p, d) == 0, "clean after delete");
    CHECK(dsg_doc_undo(d) == 0 && strcmp(dsg_doc_selection(d)->path, "/trials/track/states/0") == 0,
          "undo puts the selection from before back");
    free(s.b);

    /* the template's rename moves its view entry (another view's command) */
    CHECK(dsg_doc_undo_depth(d) > 0 && dsg_def_rename(d, "/trials", "track", "tracking") == 0, "rename the template");
    CHECK(dsg_chart_view_pos(&v, "tracking", "moving", &x, &y) && !dsg_chart_view_template(&v, "track", false),
          "the template's positions follow it");
    CHECK(dsg_doc_undo(d) == 0 && dsg_chart_view_pos(&v, "track", "moving", &x, &y), "and back with undo");

    /* a text that does not parse: every command refuses */
    {
        size_t n = 0;
        const char* t = dsg_doc_bytes(d, DSG_BUF_DEF, &n);
        char* bad = (char*)malloc(n + 2);
        memcpy(bad, t, n);
        bad[n] = '}';
        dep = dsg_doc_undo_depth(d);
        dsg_doc_replace(d, DSG_BUF_DEF, bad, n + 1, 0);
        CHECK(dsg_chart_add_state(d, "track", "q") == -1 && strstr(dsg_chart_error(), "parse"), "no parse, no command: %s",
              dsg_chart_error());
        CHECK(strcmp(dsg_doc_undo_label(d), "Add state") != 0 && !dsg_doc_can_redo(d), "the typing is the last step");
        dsg_doc_undo(d);
        free(bad);
    }

    /* the view state through the view file, and never in the definition */
    {
        dsg_view vf, back;
        dsg_chart_view w;
        size_t n = 0, m = 0;
        char* json;
        const char* part;
        dsg_chart_tview* tv = dsg_chart_view_template(&v, "track", true);
        memset(&vf, 0, sizeof vf);
        memset(&back, 0, sizeof back);
        tv->pan_x = -12.6f;
        tv->pan_y = 33.6f;
        tv->zoom = 1.25f;
        dsg_chart_view_set_pos(&v, "reach", DSG_CHART_END, 640, -80);
        dsg_chart_view_set_current(&v, "reach");
        json = dsg_chart_view_write(&v, &n);
        CHECK(json && dsg_view_part_set(&vf, "chart", json, n) == 0 && dsg_view_write(p.folder, &vf, err, sizeof err) == 0,
              "the part goes into the view file: %s", err);
        CHECK(dsg_view_read(p.folder, &back, err, sizeof err) == 0 && (part = dsg_view_part_get(&back, "chart", &m)) != NULL,
              "and comes back");
        dsg_chart_view_init(&w);
        CHECK(part && dsg_chart_view_read(&w, part, m) == 0, "the part reads");
        CHECK(w.current && strcmp(w.current, "reach") == 0, "the template shown");
        tv = dsg_chart_view_template(&w, "track", false);
        CHECK(tv && tv->pan_x == -13 && tv->pan_y == 34 && fabsf(tv->zoom - 1.25f) < 1e-6f, "pan in whole px, zoom (%g %g %g)",
              tv ? tv->pan_x : 0, tv ? tv->pan_y : 0, tv ? tv->zoom : 0);
        CHECK(dsg_chart_view_pos(&w, "track", "moving", &x, &y) && x == 300 && y == 120 && dsg_chart_view_pos(&w, "reach", "end", &x, &y) &&
              x == 640 && y == -80, "positions, the end's too");
        {
            size_t n2 = 0;
            char* again = dsg_chart_view_write(&w, &n2);
            CHECK(again && n2 == n && memcmp(again, json, n) == 0, "written again, the same bytes");
            free(again);
        }
        static const char odd[] = "{\"templates\": {\"t\": {\"pan\": \"x\", \"zoom\": -1, \"states\": {\"a\": [1], \"b\": [1, 2], \"c\": [1e9, 2], \"d\": [1, 2, 3]}}}}";
        CHECK(dsg_chart_view_read(&w, odd, sizeof odd - 1) == 0 &&
              dsg_chart_view_pos(&w, "t", "b", &x, &y) && !dsg_chart_view_pos(&w, "t", "a", &x, &y) && !dsg_chart_view_pos(&w, "t", "c", &x, &y) && !dsg_chart_view_pos(&w, "t", "d", &x, &y) &&
              dsg_chart_view_template(&w, "t", false)->zoom == 1, "entries of the wrong shape are skipped");
        CHECK(dsg_chart_view_read(&w, "{", 1) == -1, "a part that does not parse");
        dsg_chart_view_free(&w);
        free(json);
        dsg_view_free(&vf);
        dsg_view_free(&back);
    }
    CHECK(!has_text(d, "\"pan\"") && !has_text(d, "\"zoom\"") && !has_text(d, "\"template\"") && !has_text(d, "640"),
          "the definition's bytes hold no view state");
    {
        /* back to the start: every step undone gives the file's bytes */
        while (dsg_doc_can_undo(d)) dsg_doc_undo(d);
        CHECK(same(d, &s0), "all undone: the bytes opened");
    }
    free(s0.b);
    dsg_doc_unlisten(d, dsg_chart_view_on_rename, &v);
    dsg_chart_view_free(&v);
    dsg_chart_free(&c);
    dsg_doc_free(d);
    dsg_project_close(&p);
}

/* Shared exits and over: v2_retry, checked with bimanual's pack. */
static void test_shared(void) {
    dsg_project p;
    dsg_doc* d = dsg_doc_new();
    dsg_chart c;
    snap s;
    int dep;
    memset(&c, 0, sizeof c);
    if (project("bimanual", "tests/experiment/good/v2_retry.json", "cmd_retry", &p)) { CHECK(0, "copy retry"); return; }
    dsg_doc_reset(d, "experiment.json", p.text, p.text_len);
    CHECK(errors(&p, d) == 0, "v2_retry checks clean");

    /* rename cue: fixate's exit, both shared exits' `in` */
    dep = dsg_doc_undo_depth(d);
    s = take(d);
    CHECK(dsg_chart_rename_state(d, "trial", 1, "cue2") == 0, "rename cue: %s", dsg_chart_error());
    one_step(d, dep, &s, "rename cue");
    CHECK(has_text(d, "\"to\": \"cue2\"") && has_text(d, "\"in\": [\"cue2\", \"delay\"]") &&
          has_text(d, "\"in\": [\"fixate\", \"cue2\", \"delay\", \"go\"]") && !has_text(d, "\"cue\""), "every reference follows");
    CHECK(errors(&p, d) == 0, "clean after the rename");
    free(s.b);

    /* rename fixate: over's target elsewhere; relay's, warn's, note_break's `to` */
    CHECK(dsg_chart_rename_state(d, "trial", 0, "fix_on") == 0 && !has_text(d, "\"to\": \"fixate\"") && errors(&p, d) == 0,
          "rename fixate: every `to` follows, clean");

    /* delete cue2: fixate's exit to it goes, and its `in` entries */
    dep = dsg_doc_undo_depth(d);
    s = take(d);
    CHECK(dsg_chart_delete_state(d, "trial", 1) == 0, "delete cue2: %s", dsg_chart_error());
    one_step(d, dep, &s, "delete with shared exits");
    CHECK(dsg_chart_build(dsg_doc_root(d), "trial", &c) == 0 && c.n_states == 7 && count_kind(&c, DSG_EDGE_SHARED) == 4 &&
          c.states[0].n_exits == 1, "fixate keeps its timeout; the shared exits lose cue2");
    CHECK(!has_text(d, "cue2"), "no reference is left");
    CHECK(errors(&p, d) == 0, "clean after the delete");
    free(s.b);

    /* delete delay: the broke exit's `in` empties, so it goes */
    CHECK(dsg_chart_delete_state(d, "trial", 1) == 0, "delete delay");
    CHECK(dsg_chart_build(dsg_doc_root(d), "trial", &c) == 0 && count_kind(&c, DSG_EDGE_SHARED) == 2 && !has_text(d, "\"on\": \"broke\""),
          "the shared exit with an empty `in` is gone");
    CHECK(errors(&p, d) == 0, "clean after the second delete");

    /* delete the over edge: max_visits goes with it */
    dep = dsg_doc_undo_depth(d);
    s = take(d);
    CHECK(dsg_chart_delete_exit(d, "/trials/trial/states/0/over") == 0, "delete over: %s", dsg_chart_error());
    one_step(d, dep, &s, "delete over");
    CHECK(!has_text(d, "\"over\"") && !has_text(d, "\"max_visits\""), "over and max_visits are gone");
    free(s.b);

    /* delete the last shared exit: the array goes */
    CHECK(dsg_chart_delete_exit(d, "/trials/trial/exits/0") == 0 && !dsg_def_get(d, "/trials/trial/exits"),
          "the last shared exit takes its array with it");

    /* delete the end's target of over: a state that over leads to */
    {
        static const char def[] =
            "{\"trials\": {\"t\": {\"states\": [{\"name\": \"a\", \"max_visits\": 2, \"over\": {\"to\": \"b\"}, \"exits\": [{\"to\": \"b\"}]},"
            " {\"name\": \"b\"}]}}}";
        dsg_doc_reset(d, "experiment.json", def, sizeof def - 1);
        CHECK(dsg_chart_rename_state(d, "t", 1, "c") == 0 && has_text(d, "\"over\": {\n") &&
              dsg_def_get(d, "/trials/t/states/0/over/to") && strcmp(dsg_def_get(d, "/trials/t/states/0/over/to")->u.s, "c") == 0,
              "a rename rewrites over.to");
        CHECK(dsg_chart_delete_state(d, "t", 1) == 0 && !has_text(d, "\"over\"") && !has_text(d, "\"max_visits\"") && !has_text(d, "\"exits\""),
              "a state's delete takes over, max_visits and the emptied exits that led to it");
    }
    dsg_chart_free(&c);
    dsg_doc_free(d);
    dsg_project_close(&p);
}

/* add_state on a template with neither states nor events makes `states`. */
static void test_first_state(void) {
    static const char def[] = "{\"trials\": {\"t\": {}}}";
    dsg_doc* d = dsg_doc_new();
    dsg_doc_reset(d, "experiment.json", def, sizeof def - 1);
    CHECK(dsg_chart_add_state(d, "t", "a") == 0 && has_text(d, "\"states\": [") && has_text(d, "\"name\": \"a\""),
          "the first state makes `states`: %s", dsg_chart_error());
    dsg_doc_free(d);
}

int main(int argc, char** argv) {
    char err[512];
    if (argc < 2) {
        printf("usage: test_designer_chart WORK\n");
        return 2;
    }
    work = argv[1];
    if (dsg_make_dir(work, err, sizeof err)) {
        printf("FAIL: %s\n", err);
        return 1;
    }
    checker = dsg_checker_new();
    if (!checker) return 1;
    test_model();
    test_places();
    test_commands();
    test_shared();
    test_first_state();
    dsg_checker_free(checker);
    printf("%s: %d checks, %d failed\n", fails ? "FAIL" : "PASS", checks, fails);
    return fails ? 1 : 0;
}
