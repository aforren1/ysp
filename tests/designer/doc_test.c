/* doc_test.c - the designer's document (designer/core/doc.h) and the view
 * file's parts, with no window (docs/designer.md, "Plan of step 4").
 *
 * Usage: test_designer_doc WORK
 *   1. Paths: escapes, split, within, get, the value at a text offset and the
 *      offset of a value, against a parsed definition.
 *   2. Buffers: bytes, revisions, saved and dirty.
 *   3. Splices: undo and redo, labels, the place of the last change, nested
 *      transactions as one step, cancel, a failed splice.
 *   4. Typing merges: characters typed in a run are one step, a Backspace of
 *      them joins it, typing elsewhere starts a step.
 *   5. Structured edits: set, the typed setters, remove, insert, move and
 *      rename in one transaction are one step; the bytes are the canonical
 *      layout; refused while the text does not parse, which leaves the last
 *      tree to show as stale.
 *   6. The canonical layout is yexp_canonical()'s: a no-op edit of a
 *      definition in its canonical bytes changes nothing and makes no step.
 *   7. Selection: kept before and after a step; undo and redo put it back.
 *   8. Renames: listeners hear the commit, the undo (swapped) and the redo.
 *   9. An issue's place to its value: a loader diagnostic's line and column
 *      name the key it refuses.
 *  10. View parts: set, get, reserved keys, and a round trip through the
 *      view file, with a part this designer does not know kept.
 *  11. Limits and errors: a bad path, an index past the end, undo inside a
 *      transaction.
 * Exit 0 on pass, 1 on a failure.
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "doc.h"
#include "dsg.h"
#include "player.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef YSP_SOURCE_DIR
#define YSP_SOURCE_DIR "."
#endif

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const char* text0(dsg_doc* d) { return dsg_doc_bytes(d, DSG_BUF_DEF, NULL); }

static bool has(dsg_doc* d, const char* s) { return strstr(text0(d), s) != NULL; }

static char* read_all(const char* path, size_t* n) {
    char* t = NULL;
    char err[512];
    if (dsg_read_file(path, &t, n, err, sizeof err)) printf("  %s\n", err);
    return t;
}

static const char k_def[] =
    "{\"format\": \"ysp-experiment\", \"version\": 2,\n"
    " \"stimuli\": {\"fix\": {\"kind\": \"shape\", \"shape\": \"cross\", \"w\": 20, \"h\": 20},\n"
    "             \"a/b~c\": {\"kind\": \"shape\", \"shape\": \"circle\", \"w\": 5, \"h\": 5}},\n"
    " \"trials\": {\"t\": {\"states\": [{\"name\": \"show\", \"steps\": [{\"on\": \"fix\"}],\n"
    "                                  \"exits\": [{\"after_s\": 0.5, \"to\": \"end\"}]}]}},\n"
    " \"timeline\": [{\"name\": \"main\", \"trial\": \"t\"}]}";

/* --- 1. paths ------------------------------------------------------------------- */

static void test_paths(void) {
    char p[DSG_SEL_PATH], parent[DSG_SEL_PATH], last[64];
    yjs_arena a;
    yjs_value* root = NULL;
    yjs_error e;
    uint32_t off = 0;
    const char* at;

    CHECK(dsg_path_key(p, sizeof p, "", "stimuli") == 0 && strcmp(p, "/stimuli") == 0, "a key: '%s'", p);
    CHECK(dsg_path_key(p, sizeof p, "/stimuli", "a/b~c") == 0 && strcmp(p, "/stimuli/a~1b~0c") == 0, "escapes: '%s'", p);
    CHECK(dsg_path_index(p, sizeof p, "/timeline", 3) == 0 && strcmp(p, "/timeline/3") == 0, "an index: '%s'", p);
    CHECK(dsg_path_key(p, 8, "/stimuli", "fix") == -1, "too long for the buffer");
    CHECK(dsg_path_split("/stimuli/a~1b~0c", parent, sizeof parent, last, sizeof last) == 0 && strcmp(parent, "/stimuli") == 0 &&
              strcmp(last, "a/b~c") == 0, "split: '%s' '%s'", parent, last);
    CHECK(dsg_path_split("", parent, sizeof parent, last, sizeof last) == -1, "the root has no parent");
    CHECK(dsg_path_within("/a/b", "/a") && dsg_path_within("/a", "/a") && !dsg_path_within("/ab", "/a") && dsg_path_within("/a", ""),
          "within");

    yjs_arena_init_heap(&a, 0);
    CHECK(yjs_parse(&a, k_def, sizeof k_def - 1, NULL, &root, &e) == YJS_OK, "the test definition parses");
    CHECK(dsg_path_get(root, "") == root, "'' is the root");
    CHECK(dsg_path_get(root, "/stimuli/a~1b~0c/w") && dsg_path_get(root, "/stimuli/a~1b~0c/w")->i == 5, "an escaped key resolves");
    CHECK(dsg_path_get(root, "/trials/t/states/0/exits/0/to") != NULL, "array items resolve");
    CHECK(!dsg_path_get(root, "/trials/t/states/1") && !dsg_path_get(root, "/trials/t/states/01") &&
              !dsg_path_get(root, "/stimuli/fix~2"), "a missing item, a leading zero and a bad escape do not");

    /* the value at a place: on a key, inside a value, between members */
    at = strstr(k_def, "\"after_s\"");
    CHECK(dsg_path_at(root, (uint32_t)(at - k_def), p, sizeof p) == 0 && strcmp(p, "/trials/t/states/0/exits/0/after_s") == 0,
          "a key's place: '%s'", p);
    at = strstr(k_def, "0.5");
    CHECK(dsg_path_at(root, (uint32_t)(at - k_def), p, sizeof p) == 0 && strcmp(p, "/trials/t/states/0/exits/0/after_s") == 0,
          "inside its value: '%s'", p);
    at = strstr(k_def, "\"circle\"");
    CHECK(dsg_path_at(root, (uint32_t)(at - k_def), p, sizeof p) == 0 && strcmp(p, "/stimuli/a~1b~0c/shape") == 0, "escaped: '%s'", p);
    CHECK(dsg_path_at(root, 0, p, sizeof p) == 0 && strcmp(p, "") == 0, "the start is the root: '%s'", p);
    CHECK(dsg_path_offset(root, "/stimuli/fix", &off) && off == (uint32_t)(strstr(k_def, "\"fix\"") - k_def), "a member's key offset");
    CHECK(dsg_path_offset(root, "/timeline/0", &off) && off == (uint32_t)(strstr(k_def, "{\"name\": \"main\"") - k_def),
          "an item's offset");
    CHECK(!dsg_path_offset(root, "/nothing", &off), "no offset for a missing value");
    yjs_arena_free(&a);
}

/* --- 2, 3. buffers, splices, undo ------------------------------------------------- */

static void test_buffers(dsg_doc* d) {
    size_t n;
    int t, buf = -1;
    size_t off = 0, len = 0;
    uint64_t r0, r1;
    CHECK(dsg_doc_reset(d, "experiment.json", "abc", 3) == 0 && dsg_doc_buffer_count(d) == 1, "reset");
    t = dsg_doc_add_buffer(d, DSG_KIND_TABLE, "conditions.csv", "x,y\n1,2\n", 8);
    CHECK(t == 1 && dsg_doc_find_buffer(d, "conditions.csv") == 1 && dsg_doc_buffer_kind(d, 1) == DSG_KIND_TABLE, "a table buffer");
    CHECK(dsg_doc_add_buffer(d, DSG_KIND_TABLE, "conditions.csv", "", 0) == -1, "a name twice is refused");
    CHECK(!dsg_doc_dirty(d, 0) && !dsg_doc_dirty(d, 1), "clean at load");
    r0 = dsg_doc_rev(d, 1);
    CHECK(dsg_doc_splice(d, 1, 6, 1, "42", 2) == 0 && strcmp(dsg_doc_bytes(d, 1, &n), "x,y\n1,42\n") == 0 && n == 9, "a splice");
    r1 = dsg_doc_rev(d, 1);
    CHECK(r1 != r0 && dsg_doc_dirty(d, 1) && !dsg_doc_dirty(d, 0), "the revision moved; dirty");
    CHECK(dsg_doc_last_change(d, &buf, &off, &len) && buf == 1 && off == 6 && len == 2, "the last change's place");
    CHECK(dsg_doc_can_undo(d) && strcmp(dsg_doc_undo_label(d), "Edit") == 0, "undo offered");
    CHECK(dsg_doc_undo(d) == 0 && strcmp(dsg_doc_bytes(d, 1, NULL), "x,y\n1,2\n") == 0 && !dsg_doc_dirty(d, 1), "undo, clean again");
    CHECK(dsg_doc_last_change(d, &buf, &off, &len) && buf == 1 && off == 6 && len == 1, "undo's place");
    CHECK(dsg_doc_rev(d, 1) != r1 && dsg_doc_rev(d, 1) != r0, "undo is a new revision");
    CHECK(dsg_doc_redo(d) == 0 && strcmp(dsg_doc_bytes(d, 1, NULL), "x,y\n1,42\n") == 0, "redo");
    dsg_doc_mark_saved(d, 1);
    CHECK(!dsg_doc_dirty(d, 1), "saved");

    /* nested transactions over two buffers: one step */
    dsg_doc_begin(d, "Two files");
    dsg_doc_begin(d, "inner");
    CHECK(dsg_doc_splice(d, 0, 0, 0, "<", 1) == 0, "inner splice");
    CHECK(dsg_doc_commit(d) == 0 && dsg_doc_in_transaction(d), "the inner commit keeps the transaction");
    CHECK(dsg_doc_splice(d, 1, 0, 1, "z", 1) == 0, "outer splice");
    CHECK(!dsg_doc_can_undo(d), "no undo inside a transaction");
    CHECK(dsg_doc_undo(d) == -1, "undo refuses inside a transaction");
    CHECK(dsg_doc_commit(d) == 0 && !dsg_doc_in_transaction(d), "the outer commit");
    CHECK(strcmp(text0(d), "<abc") == 0 && dsg_doc_bytes(d, 1, NULL)[0] == 'z', "both changed");
    CHECK(strcmp(dsg_doc_undo_label(d), "Two files") == 0, "the outer label names the step");
    CHECK(dsg_doc_undo(d) == 0 && strcmp(text0(d), "abc") == 0 && dsg_doc_bytes(d, 1, NULL)[0] == 'x', "one undo takes both back");
    CHECK(dsg_doc_can_redo(d), "redo offered");

    /* a new step drops the redo; cancel restores */
    CHECK(dsg_doc_splice(d, 0, 3, 0, "d", 1) == 0 && !dsg_doc_can_redo(d), "a new step drops the redo");
    dsg_doc_begin(d, "Cancelled");
    dsg_doc_splice(d, 0, 0, 4, "", 0);
    dsg_doc_splice(d, 1, 0, 0, "q", 1);
    dsg_doc_cancel(d);
    CHECK(strcmp(text0(d), "abcd") == 0 && dsg_doc_bytes(d, 1, NULL)[0] == 'x' && strcmp(dsg_doc_undo_label(d), "Edit") == 0,
          "cancel puts both back and makes no step");
    CHECK(dsg_doc_splice(d, 0, 9, 0, "!", 1) == -1 && strstr(dsg_doc_error(d), "past the end"), "a splice past the end: '%s'",
          dsg_doc_error(d));
    CHECK(dsg_doc_splice(d, 7, 0, 0, "!", 1) == -1, "no such buffer");
    CHECK(dsg_doc_replace(d, 0, "abcd", 4, 0) == 0 && strcmp(dsg_doc_undo_label(d), "Edit") == 0 && dsg_doc_undo_depth(d) == 2,
          "a replace that changes nothing makes no step");
}

/* --- 4. typing ------------------------------------------------------------------------ */

static void type_text(dsg_doc* d, const char* now) { dsg_doc_replace(d, DSG_BUF_DEF, now, strlen(now), DSG_EDIT_MERGE); }

static void test_typing(dsg_doc* d) {
    int depth;
    dsg_doc_reset(d, "e.json", "{}", 2);
    type_text(d, "{a}");
    type_text(d, "{ab}");
    type_text(d, "{abc}");
    depth = dsg_doc_undo_depth(d);
    CHECK(depth == 1 && strcmp(dsg_doc_undo_label(d), "Typing") == 0, "three keys, one step (%d)", depth);
    type_text(d, "{ab}");   /* Backspace of what was typed */
    CHECK(dsg_doc_undo_depth(d) == 1, "a Backspace of typed text joins it");
    type_text(d, "x{ab}");  /* elsewhere */
    CHECK(dsg_doc_undo_depth(d) == 2, "typing elsewhere is a step");
    dsg_doc_undo(d);
    CHECK(strcmp(text0(d), "{ab}") == 0, "undo takes the elsewhere text: '%s'", text0(d));
    dsg_doc_undo(d);
    CHECK(strcmp(text0(d), "{}") == 0, "undo takes the run: '%s'", text0(d));
    dsg_doc_redo(d);
    type_text(d, "{abz}");
    CHECK(dsg_doc_undo_depth(d) == 2, "after a redo, typing does not join the redone step");
    dsg_doc_break_merge(d);
    type_text(d, "{abzz}");
    CHECK(dsg_doc_undo_depth(d) == 3, "a broken merge starts a step");
    /* UTF-8: a change inside a character takes the whole character */
    dsg_doc_reset(d, "e.json", "\"\xC3\xA9\"", 4);
    type_text(d, "\"\xC3\xA8\"");
    {
        int buf = -1;
        size_t off = 0, len = 0;
        CHECK(dsg_doc_last_change(d, &buf, &off, &len) && off == 1 && len == 2, "the splice starts at the character (%zu, %zu)", off, len);
    }
}

/* --- 5, 6. structured edits ------------------------------------------------------------- */

static void test_edits(dsg_doc* d) {
    const yjs_value* v;
    bool stale = true;
    size_t n;
    int depth;
    char* canon;

    dsg_doc_reset(d, "experiment.json", k_def, sizeof k_def - 1);
    CHECK(dsg_doc_root(d) != NULL, "the definition parses");
    dsg_doc_begin(d, "Several");
    CHECK(dsg_def_set_seconds_ns(d, "/trials/t/states/0/exits/0/after_s", 250000000) == 0, "a duration");
    CHECK(dsg_def_set_string(d, "/display/units", "deg") == 0, "a member under a missing object");
    CHECK(dsg_def_set_number_text(d, "/stimuli/fix/w", "20.50") == 0, "a number as typed");
    CHECK(dsg_def_set_bool(d, "/stimuli/fix/visible", true) == 0 && dsg_def_set_int(d, "/stimuli/fix/layer", 2) == 0, "bool, int");
    CHECK(dsg_def_insert(d, "/trials/t/states", -1, "{\"name\": \"wait\"}", strlen("{\"name\": \"wait\"}")) == 0, "append a state");
    CHECK(dsg_def_insert(d, "/trials/t/states", 0, "{\"name\": \"first\"}", strlen("{\"name\": \"first\"}")) == 0, "insert a state first");
    v = dsg_def_get(d, "/trials/t/states/2/name");
    CHECK(v && strcmp(v->u.s, "wait") == 0, "the transaction sees its own edits");
    CHECK(dsg_def_move(d, "/trials/t/states", 2, 0) == 0, "move");
    CHECK(dsg_def_remove(d, "/trials/t/states/1") == 0, "remove an item");
    CHECK(dsg_def_rename(d, "/stimuli", "a/b~c", "dot") == 0, "rename a member");
    CHECK(dsg_def_rename(d, "/stimuli", "dot", "fix") == -1, "rename to a key there is refused");
    CHECK(dsg_def_remove(d, "/stimuli/dot/h") == 0, "remove a member");
    CHECK(dsg_def_set(d, "/timeline/-", "{\"trial\": \"t\"}", strlen("{\"trial\": \"t\"}")) == 0, "set '-' appends");
    CHECK(dsg_def_set(d, "/name", "{bad", 4) == -1 && strstr(dsg_doc_error(d), "not JSON"), "bad JSON: '%s'", dsg_doc_error(d));
    CHECK(strcmp(text0(d), k_def) == 0, "nothing reaches the bytes before the commit");
    CHECK(dsg_doc_commit(d) == 0, "commit");
    CHECK(dsg_doc_undo_depth(d) == 1 && strcmp(dsg_doc_undo_label(d), "Several") == 0, "one step");
    CHECK(has(d, "\"after_s\": 0.25") && has(d, "\"units\": \"deg\"") && has(d, "\"w\": 20.5") && has(d, "\"layer\": 2") &&
              has(d, "\"visible\": true"), "the values, canonically spelled");
    CHECK(has(d, "\"dot\": {\n") && !has(d, "a/b~c") && !has(d, "\"first\""), "renamed and removed");
    v = dsg_path_get(dsg_doc_root(d), "/trials/t/states/0/name");
    CHECK(v && strcmp(v->u.s, "wait") == 0, "moved: '%s'", v ? v->u.s : "");
    CHECK(text0(d)[0] == '{' && text0(d)[1] == '\n' && text0(d)[strlen(text0(d)) - 1] == '\n' && text0(d)[strlen(text0(d)) - 2] == '}',
          "the canonical layout, one LF after the value");
    dsg_doc_undo(d);
    CHECK(strcmp(text0(d), k_def) == 0, "one undo gives the text as typed back");
    dsg_doc_redo(d);

    /* a single edit is its own step */
    depth = dsg_doc_undo_depth(d);
    CHECK(dsg_def_set_string(d, "/name", "demo") == 0 && dsg_doc_undo_depth(d) == depth + 1, "one edit, one step");
    CHECK(dsg_def_remove(d, "/nothing/here") == -1 && dsg_doc_undo_depth(d) == depth + 1, "a refused edit makes no step");

    /* a text that does not parse; a view called dsg_doc_root() in the frame before */
    CHECK(dsg_doc_root(d) != NULL, "the edited text parses");
    dsg_doc_replace(d, DSG_BUF_DEF, "{\"name\": ", 9, 0);
    CHECK(dsg_doc_root(d) == NULL && dsg_doc_parse_error(d) != NULL, "no tree, a parse error");
    v = dsg_doc_shown(d, &stale);
    CHECK(v && stale && dsg_path_get(v, "/name") && strcmp(dsg_path_get(v, "/name")->u.s, "demo") == 0, "the last tree, stale");
    CHECK(dsg_def_set_string(d, "/name", "x") == -1 && strstr(dsg_doc_error(d), "does not parse"), "edits refuse: '%s'",
          dsg_doc_error(d));
    dsg_doc_undo(d);
    CHECK(dsg_doc_root(d) != NULL, "it parses again after undo");

    /* 6: the layout is yexp_canonical()'s */
    canon = read_all(YSP_SOURCE_DIR "/tests/player/bimanual/experiment.json", &n);
    if (canon) {
        yjs_arena a;
        yexp_def* def = NULL;
        yexp_report* rep = (yexp_report*)calloc(1, sizeof *rep);
        const char* bytes;
        size_t bn;
        yjs_arena_init_heap(&a, 0);
        if (rep && yexp_load(&a, canon, n, NULL, &def, rep) == YEXP_OK && yexp_canonical(def, &a, &bytes, &bn, NULL) == YEXP_OK) {
            const yjs_value* nm;
            dsg_doc_reset(d, "experiment.json", bytes, bn);
            nm = dsg_path_get(dsg_doc_root(d), "/version");
            CHECK(nm != NULL, "the version");
            CHECK(dsg_def_set_int(d, "/version", nm ? nm->i : 2) == 0 && dsg_doc_undo_depth(d) == 0 && dsg_doc_rev(d, 0) != 0,
                  "a no-op edit of canonical bytes makes no step (%d)", dsg_doc_undo_depth(d));
            CHECK(dsg_doc_bytes(d, 0, &n) && n == bn && memcmp(text0(d), bytes, bn) == 0, "and the bytes are yexp_canonical()'s");
        } else {
            CHECK(0, "the bimanual definition loads");
        }
        yjs_arena_free(&a);
        free(rep);
        free(canon);
    }
}

/* --- 7, 8. selection and renames ------------------------------------------------------------- */

typedef struct heard_t { int n; char last[256]; } heard_t;
static void on_rename(void* ctx, const char* scope, const char* from, const char* to) {
    heard_t* h = (heard_t*)ctx;
    h->n++;
    snprintf(h->last, sizeof h->last, "%s:%s>%s", scope, from, to);
}

static void test_selection(dsg_doc* d) {
    dsg_sel s;
    heard_t h;
    uint64_t r;
    memset(&h, 0, sizeof h);
    dsg_doc_reset(d, "experiment.json", k_def, sizeof k_def - 1);
    CHECK(dsg_doc_selection(d)->buf == -1, "nothing selected at reset");
    dsg_sel_def(&s, "/stimuli/fix");
    dsg_doc_select(d, &s);
    r = dsg_doc_selection_rev(d);
    dsg_doc_select(d, &s);
    CHECK(dsg_doc_selection_rev(d) == r, "the same selection does not move the revision");
    CHECK(dsg_doc_listen(d, on_rename, &h) == 0, "listen");

    dsg_doc_begin(d, "Rename stimulus");
    dsg_def_rename(d, "/stimuli", "fix", "cross");
    dsg_def_set_string(d, "/trials/t/states/0/steps/0/on", "cross");
    dsg_sel_def(&s, "/stimuli/cross");
    dsg_doc_select(d, &s);
    CHECK(h.n == 0, "listeners wait for the commit");
    dsg_doc_commit(d);
    CHECK(h.n == 1 && strcmp(h.last, "/stimuli:fix>cross") == 0, "the commit: '%s'", h.last);
    CHECK(strcmp(dsg_doc_selection(d)->path, "/stimuli/cross") == 0, "the step's selection");
    dsg_doc_undo(d);
    CHECK(h.n == 2 && strcmp(h.last, "/stimuli:cross>fix") == 0, "the undo, swapped: '%s'", h.last);
    CHECK(strcmp(dsg_doc_selection(d)->path, "/stimuli/fix") == 0, "undo puts the selection back: '%s'", dsg_doc_selection(d)->path);
    dsg_doc_redo(d);
    CHECK(h.n == 3 && strcmp(h.last, "/stimuli:fix>cross") == 0, "the redo: '%s'", h.last);
    CHECK(strcmp(dsg_doc_selection(d)->path, "/stimuli/cross") == 0, "redo selects again");
    dsg_doc_begin(d, "Cancelled");
    dsg_doc_note_rename(d, "/x", "a", "b");
    dsg_doc_cancel(d);
    CHECK(h.n == 3, "a cancelled rename is not heard");
    CHECK(dsg_doc_note_rename(d, "/x", "a", "b") == -1, "a rename outside a transaction is refused");
    dsg_doc_unlisten(d, on_rename, &h);
    dsg_doc_undo(d);
    CHECK(h.n == 3, "unlisten");
}

/* --- 9. an issue's place to its value ------------------------------------------------------------ */

static void test_issue_path(void) {
    static const char bad[] =
        "{\"format\": \"ysp-experiment\", \"version\": 2,\n"
        " \"stimuli\": {\"fix\": {\"kind\": \"shape\", \"shape\": \"cross\", \"w\": 20, \"h\": 20, \"wdth\": 3}},\n"
        " \"trials\": {\"t\": {\"events\": [{\"on\": \"fix\"}, {\"wait_s\": 0.5}]}},\n"
        " \"timeline\": [{\"name\": \"main\", \"trial\": \"t\"}]}";
    yjs_arena a;
    yexp_def* def = NULL;
    yexp_report* rep = (yexp_report*)calloc(1, sizeof *rep);
    yjs_value* root = NULL;
    yjs_error e;
    char p[DSG_SEL_PATH] = "";
    int i;
    bool found = false;
    yjs_arena_init_heap(&a, 0);
    CHECK(rep && yexp_load(&a, bad, sizeof bad - 1, NULL, &def, rep) != YEXP_OK && rep->n_diags > 0, "the loader refuses wdth");
    yjs_parse(&a, bad, sizeof bad - 1, NULL, &root, &e);
    for (i = 0; rep && i < rep->n_diags; i++) {
        uint32_t off = dsg_offset_of(bad, sizeof bad - 1, rep->diags[i].line, rep->diags[i].column);
        if (rep->diags[i].line && dsg_path_at(root, off, p, sizeof p) == 0 && strcmp(p, "/stimuli/fix/wdth") == 0) found = true;
    }
    CHECK(found, "a diagnostic's place is the key it refuses ('%s', %d diagnostics)", p, rep ? rep->n_diags : 0);
    yjs_arena_free(&a);
    free(rep);
}

/* --- 10. view parts ------------------------------------------------------------------------------- */

static void test_view_parts(const char* work) {
    dsg_view v, r;
    size_t n;
    char err[512], path[DSG_PATH_MAX], folder[DSG_PATH_MAX];
    const char* j;
    memset(&v, 0, sizeof v);
    CHECK(dsg_view_part_set(&v, "chart", "{\"templates\": {\"t\": {\"states\": {\"show\": [10, 20.5]}}}}", 54) == 0, "set a part");
    CHECK(dsg_view_part_set(&v, "tables", "[1]", 3) == -1, "a part is an object");
    CHECK(dsg_view_part_set(&v, "caret", "{}", 2) == -1, "a reserved key is not a part");
    CHECK(dsg_view_part_set(&v, "future", "{\"x\": 1}", 8) == 0, "a part this designer does not read");
    j = dsg_view_part_get(&v, "chart", &n);
    CHECK(j && strcmp(j, "{\"templates\":{\"t\":{\"states\":{\"show\":[10,20.5]}}}}") == 0 && n == strlen(j), "compact: '%s'", j ? j : "");
    CHECK(dsg_view_part_get(&v, "none", &n) == NULL && n == 0, "a missing part");
    snprintf(v.main_view, sizeof v.main_view, "chart");
    snprintf(v.selection, sizeof v.selection, "/trials/t/states/0");
    v.caret_line = 3;
    v.caret_column = 1;
    if (dsg_join(folder, sizeof folder, work, "viewparts") || dsg_make_dir(folder, err, sizeof err)) {
        CHECK(0, "the work folder");
        return;
    }
    CHECK(dsg_view_write(folder, &v, err, sizeof err) == 0, "write: %s", err);
    CHECK(dsg_view_read(folder, &r, err, sizeof err) == 0, "read: %s", err);
    CHECK(strcmp(r.main_view, "chart") == 0 && strcmp(r.selection, "/trials/t/states/0") == 0 && r.caret_line == 3, "the known keys");
    j = dsg_view_part_get(&r, "chart", NULL);
    CHECK(j && strcmp(j, "{\"templates\":{\"t\":{\"states\":{\"show\":[10,20.5]}}}}") == 0, "the chart part back");
    CHECK(dsg_view_part_get(&r, "future", NULL) != NULL, "the unknown part is kept");
    CHECK(dsg_view_part_set(&r, "future", NULL, 0) == 0 && dsg_view_part_get(&r, "future", NULL) == NULL && r.n_parts == 1, "remove");
    dsg_view_free(&r);
    dsg_view_free(&v);
    dsg_join(path, sizeof path, folder, DSG_VIEW_FILE);
    remove(path);
}

int main(int argc, char** argv) {
    dsg_doc* d;
    char err[512];
    if (argc < 2) {
        printf("usage: test_designer_doc WORK\n");
        return 2;
    }
    if (dsg_make_dir(argv[1], err, sizeof err)) {
        printf("FAIL: the work folder: %s\n", err);
        return 1;
    }
    d = dsg_doc_new();
    if (!d) {
        printf("FAIL: out of memory\n");
        return 1;
    }
    test_paths();
    test_buffers(d);
    test_typing(d);
    test_edits(d);
    test_selection(d);
    test_issue_path();
    test_view_parts(argv[1]);
    dsg_doc_free(d);
    printf("%s: %d checks, %d failed\n", fails ? "FAIL" : "PASS", checks, fails);
    return fails ? 1 : 0;
}
