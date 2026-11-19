/* core_test.c - the designer's core with no window (designer/core/dsg.h;
 * docs/designer.md, "Order of work", step 3).
 *
 * Usage: test_designer_core WORK
 *   WORK is a folder the test may write: it copies projects from
 *   tests/player/ into it and builds their check packs there.
 *
 *   1. Places: line and column to offsets and codepoints; the place of each
 *      object kind and need kind a check can name, with the fallbacks.
 *   2. The redraw rule: an event draws DSG_SETTLE_FRAMES frames and no more;
 *      queued input, an animation and pending work draw; idle blocks.
 *   3. Open: the text, the version, a version 1 file migrated, the
 *      folder or the description as the path, refusals of a bad project.
 *   4. Save: canonical version 2 bytes when the loader reads the text, the
 *      text as typed when it does not; the dirty state after each.
 *   5. View state: written and read back; a bad view file is noted and the
 *      project still opens.
 *   6. Create: a new project opens and checks clean; a second create refuses.
 *   7. The check: a clean project builds its check pack once; a loader error,
 *      a player refusal and a need each link to their key's place; a text
 *      that does not parse is checked without a pack, then builds one.
 *   8. The check pack follows the texts (stroop, with a font): a changed
 *      text rebuilds it and checks clean; another change does not rebuild.
 * Exit 0 on pass, 1 on a failure.
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "dsg.h"
#include "player.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef YSP_SOURCE_DIR
#define YSP_SOURCE_DIR "."
#endif
#ifndef YSP_DESIGNER_TEST_FONT
#define YSP_DESIGNER_TEST_FONT ""
#endif

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const char* work;

/* --- helpers ------------------------------------------------------------- */

static void path2(char* out, size_t cap, const char* a, const char* b) {
    if (dsg_join(out, cap, a, b)) out[0] = 0;
}

static int copy_file(const char* from, const char* to) {
    char* d;
    size_t n;
    char err[512];
    if (dsg_read_file(from, &d, &n, err, sizeof err)) { printf("  %s\n", err); return -1; }
    if (dsg_write_file(to, d, n, err, sizeof err)) { printf("  %s\n", err); free(d); return -1; }
    free(d);
    return 0;
}

/* A fresh copy of tests/player/NAME under WORK/NAME, its designer folder's
 * known files removed. */
static int project_copy(const char* name, const char* const* files, char* out, size_t cap) {
    char src[DSG_PATH_MAX], a[DSG_PATH_MAX], b[DSG_PATH_MAX], err[512];
    int i;
    snprintf(src, sizeof src, "%s/tests/player/%s", YSP_SOURCE_DIR, name);
    path2(out, cap, work, name);
    if (dsg_make_dir(out, err, sizeof err)) { printf("  %s\n", err); return -1; }
    for (i = 0; files[i]; i++) {
        path2(a, sizeof a, src, files[i]);
        path2(b, sizeof b, out, files[i]);
        if (copy_file(a, b)) return -1;
    }
    path2(b, sizeof b, out, DSG_VIEW_FILE);
    remove(b);
    path2(b, sizeof b, out, DSG_CHECK_PACK);
    remove(b);
    path2(b, sizeof b, out, DSG_CHECK_SOURCE);
    remove(b);
    return 0;
}

static char* read_all(const char* path, size_t* n) {
    char* d = NULL;
    char err[512];
    if (dsg_read_file(path, &d, n, err, sizeof err)) return NULL;
    return d;
}

/* text with the first `from` replaced by `to`; malloc'd. */
static char* replaced(const char* text, const char* from, const char* to, size_t* n) {
    const char* at = strstr(text, from);
    size_t a, lf = strlen(from), lt = strlen(to), lt0 = strlen(text);
    char* out;
    if (!at) return NULL;
    a = (size_t)(at - text);
    out = (char*)malloc(lt0 - lf + lt + 1);
    memcpy(out, text, a);
    memcpy(out + a, to, lt);
    memcpy(out + a + lt, at + lf, lt0 - a - lf + 1);
    *n = lt0 - lf + lt;
    return out;
}

static const dsg_issue* find_issue(const dsg_report* r, int from) {
    int i;
    for (i = 0; i < r->n_issues; i++)
        if (r->issues[i].from == from) return &r->issues[i];
    return NULL;
}

static const dsg_issue* find_need(const dsg_report* r, int kind) {
    int i;
    for (i = 0; i < r->n_issues; i++)
        if (r->issues[i].from == DSG_FROM_RUN && r->issues[i].object == kind) return &r->issues[i];
    return NULL;
}

static void dump(const dsg_report* r) {
    int i;
    for (i = 0; i < r->n_issues; i++)
        printf("    [%d/%d] %u:%u %s\n", r->issues[i].severity, r->issues[i].from, r->issues[i].place.line, r->issues[i].place.column,
               r->issues[i].message);
}

/* The text at an issue's place starts with `want`. */
static bool at_text(const char* text, const dsg_place* p, const char* want) {
    return p->line > 0 && strncmp(text + p->offset, want, strlen(want)) == 0;
}

/* --- 1. places --------------------------------------------------------------- */

static void test_places(void) {
    static const char t[] =
        "{\n"
        "  \"format\": \"ysp-experiment\", \"version\": 2,\n"
        "  \"session\": { \"fields\": { \"age\": { \"type\": \"integer\" } } },\n"
        "  \"display\": { \"calibration\": \"required\", \"units\": \"deg\" },\n"
        "  \"devices\": { \"box\": { \"direction\": \"out\" } },\n"
        "  \"roles\": { \"hand\": { \"type\": \"position2d_relative\" } },\n"
        "  \"stimuli\": { \"fix\": {}, \"\xD9\x85\xD8\xB1\": {} },\n"
        "  \"methods\": { \"st\": {} },\n"
        "  \"trials\": { \"t\": {} },\n"
        "  \"timeline\": [ { \"name\": \"a\", \"timeline\": [ { \"trial\": \"t\" }, { \"name\": \"b\", \"trial\": \"t\" } ] } ]\n"
        "}\n";
    size_t n = sizeof t - 1;
    dsg_place p;
    uint32_t off;
    static const char u[] = "a\xC3\xA9z\n\xD9\x85x";   /* a, e-acute, z, LF, meem, x */

    /* line and column (bytes) to offsets; codepoints for an editor */
    CHECK(dsg_offset_of(t, n, 1, 1) == 0, "1:1 is offset 0");
    off = dsg_offset_of(t, n, 2, 3);
    CHECK(strncmp(t + off, "\"format\"", 8) == 0, "2:3 is the format key, got '%.8s'", t + off);
    CHECK(dsg_offset_of(t, n, 2, 999) == (uint32_t)(strchr(t + 2, '\n') - t), "a column past the line's end stops at its LF");
    CHECK(dsg_offset_of(t, n, 999, 1) == n, "a line past the end is the end");
    CHECK(dsg_codepoints_before(u, sizeof u - 1, 4) == 3, "a, e-acute (2 bytes), z: 3 codepoints in 4 bytes");
    CHECK(dsg_codepoints_before(u, sizeof u - 1, 8) == 6, "and meem: 6 codepoints in 8 bytes");
    CHECK(dsg_byte_of_codepoint(u, sizeof u - 1, 3) == 4 && dsg_byte_of_codepoint(u, sizeof u - 1, 6) == 8 &&
          dsg_byte_of_codepoint(u, sizeof u - 1, 99) == sizeof u - 1, "codepoints back to bytes");

    CHECK(dsg_locate_object(t, n, PL_OBJ_STIMULUS, "fix", &p) && at_text(t, &p, "\"fix\"") && p.line == 7,
          "a stimulus is its key (line %u)", p.line);
    CHECK(dsg_locate_object(t, n, PL_OBJ_STIMULUS, "\xD9\x85\xD8\xB1", &p) && at_text(t, &p, "\"\xD9\x85"), "a stimulus with an Arabic name");
    CHECK(dsg_locate_object(t, n, PL_OBJ_STIMULUS, "gone", &p) && at_text(t, &p, "\"stimuli\""),
          "a stimulus not in the text falls back to the set's key");
    CHECK(dsg_locate_object(t, n, PL_OBJ_TEMPLATE, "t", &p) && at_text(t, &p, "\"t\"") && p.line == 9, "a template");
    CHECK(dsg_locate_object(t, n, PL_OBJ_METHOD, "st", &p) && at_text(t, &p, "\"st\""), "a method");
    CHECK(dsg_locate_object(t, n, PL_OBJ_SIGNAL_ROLE, "hand", &p) && at_text(t, &p, "\"hand\""), "a signal role");
    CHECK(dsg_locate_object(t, n, PL_OBJ_FIELD, "age", &p) && at_text(t, &p, "\"age\""), "a session field");
    CHECK(dsg_locate_object(t, n, PL_OBJ_FIELD, "participant", &p) && at_text(t, &p, "\"fields\""),
          "a default field falls back to fields");
    CHECK(dsg_locate_object(t, n, PL_OBJ_NODE, "a/b", &p) && p.line == 10 && at_text(t, &p, "\"name\": \"b\""),
          "a node's path, two deep, is its name key: line %u col %u", p.line, p.column);
    CHECK(dsg_locate_object(t, n, PL_OBJ_NODE, "a/", &p) && p.line == 10 && at_text(t, &p, "{ \"trial\""),
          "an unnamed node is its object");
    CHECK(dsg_locate_object(t, n, PL_OBJ_NODE, "a/zz", &p) && at_text(t, &p, "\"name\": \"a\""),
          "a path whose last part is not there falls back to its parent");
    CHECK(!dsg_locate_object(t, n, PL_OBJ_NONE, NULL, &p) && p.line == 0, "PL_OBJ_NONE has no place");
    CHECK(!dsg_locate_object("{ \"stimuli\": ", 13, PL_OBJ_STIMULUS, "fix", &p) && p.line == 0, "a text that does not parse has no place");

    CHECK(dsg_locate_need(t, n, PL_NEED_CALIBRATION, NULL, &p) && at_text(t, &p, "\"calibration\""), "calibration need");
    CHECK(dsg_locate_need(t, n, PL_NEED_VIEW, NULL, &p) && at_text(t, &p, "\"units\""), "view need");
    CHECK(dsg_locate_need(t, n, PL_NEED_DISPLAY_TIER, NULL, &p) && at_text(t, &p, "\"display\""), "tier need falls back to display");
    CHECK(dsg_locate_need(t, n, PL_NEED_DEVICE, "box", &p) && at_text(t, &p, "\"box\""), "device need");
    CHECK(dsg_locate_need(t, n, PL_NEED_SIM_GAIN, "hand", &p) && at_text(t, &p, "\"hand\""), "sim gain need");
    CHECK(dsg_locate_need(t, n, PL_NEED_FIELD, "age", &p) && at_text(t, &p, "\"age\""), "field need");
}

/* --- 2. redraw ------------------------------------------------------------- */

static void test_redraw(void) {
    dsg_redraw r;
    int frames = 0;
    memset(&r, 0, sizeof r);
    CHECK(!dsg_redraw_due(&r, 0, false) && dsg_redraw_wait_ms(&r, 0, false) == -1, "idle: no frame, block");
    dsg_redraw_event(&r);
    while (dsg_redraw_due(&r, 0, false) && frames < 100) { dsg_redraw_drawn(&r); frames++; }
    CHECK(frames == DSG_SETTLE_FRAMES, "one event draws %d frames, drew %d", DSG_SETTLE_FRAMES, frames);
    CHECK(dsg_redraw_due(&r, 4, false) && dsg_redraw_wait_ms(&r, 4, false) == 0, "queued input draws and polls");
    CHECK(dsg_redraw_due(&r, 0, true), "an animation draws");
    r.work = true;
    CHECK(dsg_redraw_due(&r, 0, false), "pending work draws");
    r.work = false;
    CHECK(!dsg_redraw_due(&r, 0, false), "and then idle again");
}

/* --- 3 to 5. open, save, view ---------------------------------------------- */

static const char* const rdk_files[] = { "pack.json", "experiment.json", "conditions.csv", NULL };
static const char* const vm_files[] = { "pack.json", "experiment.json", "targets.csv", NULL };
static const char* const afc2_files[] = { "pack.json", "experiment.json", NULL };

static void test_open_save(void) {
    char dir[DSG_PATH_MAX], p2[DSG_PATH_MAX], err[1024];
    dsg_project p;
    dsg_saved s;
    char* disk;
    size_t n, m;
    char* t;

    /* a version 2 file opens as it is */
    if (project_copy("visuomotor", vm_files, dir, sizeof dir)) { CHECK(0, "copy visuomotor"); return; }
    CHECK(dsg_project_open(&p, dir, err, sizeof err) == 0, "open visuomotor: %s", err);
    disk = read_all(p.def_path, &n);
    CHECK(disk && p.text_len == n && memcmp(p.text, disk, n) == 0, "a version 2 file's text is its bytes");
    CHECK(p.version == 2 && !p.migrated && !dsg_project_dirty(&p, p.text, p.text_len) && p.note[0] == 0,
          "version 2, not migrated, clean, no note ('%s')", p.note);
    CHECK(strcmp(p.def_source, "experiment.json") == 0, "the description's source");

    /* save: the canonical bytes, then clean; the file is what was written */
    t = replaced(p.text, "\"version\": 2", "\"version\":    2", &m);
    CHECK(t && dsg_project_dirty(&p, t, m), "an edit is dirty");
    CHECK(dsg_project_save(&p, t, m, &s, err, sizeof err) == 0, "save: %s", err);
    free(disk);
    disk = read_all(p.def_path, &n);
    CHECK(s.canonical && disk && n == s.n && memcmp(disk, s.bytes, n) == 0, "saved bytes are on disk");
    CHECK(disk && n > 2 && disk[n - 1] == '\n' && disk[n - 2] != '\n' && strstr(disk, "\n  \"version\": 2") != NULL &&
          strstr(disk, "\"format\": \"ysp-experiment\"") != NULL, "canonical: pretty, one LF at the end");
    CHECK(!dsg_project_dirty(&p, s.bytes, s.n) && dsg_project_dirty(&p, t, m), "the saved bytes are clean; the typed text differs");
    free(t);
    dsg_saved_free(&s);

    /* a text the loader refuses is written as typed */
    t = replaced(p.text, "\"format\"", "\"format\" oops", &m);
    CHECK(dsg_project_save(&p, t, m, &s, err, sizeof err) == 0 && !s.canonical, "a refused text saves, not canonical");
    free(disk);
    disk = read_all(p.def_path, &n);
    CHECK(disk && n == m && memcmp(disk, t, m) == 0 && !dsg_project_dirty(&p, t, m), "as typed, and clean");
    free(t);
    free(disk);
    dsg_saved_free(&s);
    dsg_project_close(&p);

    /* the description as the path; a version 1 file migrated */
    if (project_copy("rdk", rdk_files, dir, sizeof dir)) { CHECK(0, "copy rdk"); return; }
    path2(p2, sizeof p2, dir, "pack.json");
    CHECK(dsg_project_open(&p, p2, err, sizeof err) == 0 && strcmp(p.folder, dir) == 0, "open by its description: %s", err);
    CHECK(p.version == 1 && p.migrated && strstr(p.text, "\"version\": 2") && !strstr(p.saved, "\"version\": 2"),
          "version 1 is shown as version 2");
    CHECK(dsg_project_dirty(&p, p.text, p.text_len) && strstr(p.note, "version 1") != NULL, "so it is dirty, and noted: '%s'", p.note);
    CHECK(dsg_project_save(&p, p.text, p.text_len, &s, err, sizeof err) == 0 && s.canonical && s.n == p.text_len &&
          memcmp(s.bytes, p.text, s.n) == 0, "save writes the migrated bytes");
    dsg_saved_free(&s);
    dsg_project_close(&p);
    CHECK(dsg_project_open(&p, dir, err, sizeof err) == 0 && p.version == 2 && !p.migrated, "and it reopens as version 2");
    dsg_project_close(&p);

    /* refusals */
    path2(p2, sizeof p2, dir, "nothing-here");
    CHECK(dsg_project_open(&p, p2, err, sizeof err) != 0 && strstr(err, "no such"), "a missing path: %s", err);
    path2(p2, sizeof p2, dir, "conditions.csv");
    CHECK(dsg_project_open(&p, p2, err, sizeof err) != 0 && strstr(err, "line 1"), "a description that is not JSON: %s", err);
    path2(p2, sizeof p2, work, "no_project");
    dsg_make_dir(p2, err, sizeof err);
    CHECK(dsg_project_open(&p, p2, err, sizeof err) != 0 && strstr(err, "pack.json"), "a folder with no description: %s", err);
}

static void test_view(void) {
    char dir[DSG_PATH_MAX], vf[DSG_PATH_MAX], err[1024];
    dsg_view v, w;
    dsg_project p;
    static const char ini[] = "[Window][Issues]\nPos=10,20\nSize=300,200\n";
    if (project_copy("afc2", afc2_files, dir, sizeof dir)) { CHECK(0, "copy afc2"); return; }
    CHECK(dsg_view_read(dir, &v, err, sizeof err) == 0 && v.caret_line == 0 && !v.imgui_ini, "no view file: an empty view");
    v.caret_line = 12;
    v.caret_column = 7;
    v.scroll_y = 340.5f;
    v.imgui_ini = (char*)malloc(sizeof ini);
    memcpy(v.imgui_ini, ini, sizeof ini);
    v.imgui_ini_len = sizeof ini - 1;
    CHECK(dsg_view_write(dir, &v, err, sizeof err) == 0, "write the view: %s", err);
    CHECK(dsg_view_read(dir, &w, err, sizeof err) == 0 && w.caret_line == 12 && w.caret_column == 7 && w.scroll_y == 340.5f &&
          w.imgui_ini_len == v.imgui_ini_len && memcmp(w.imgui_ini, ini, v.imgui_ini_len) == 0, "it reads back");
    dsg_view_free(&w);
    CHECK(dsg_project_open(&p, dir, err, sizeof err) == 0 && p.view.caret_line == 12 && p.note[0] != 0,
          "open reads it (note: '%s')", p.note);
    {
        /* the definition is not touched by the view (format D13) */
        char* disk;
        size_t n;
        char src[DSG_PATH_MAX];
        snprintf(src, sizeof src, "%s/tests/player/afc2/experiment.json", YSP_SOURCE_DIR);
        disk = read_all(src, &n);
        CHECK(disk && n == p.saved_len && memcmp(disk, p.saved, n) == 0, "the definition's bytes are unchanged");
        free(disk);
    }
    dsg_project_close(&p);
    dsg_view_free(&v);

    path2(vf, sizeof vf, dir, DSG_VIEW_FILE);
    dsg_write_file(vf, "{ \"format\": ", 12, err, sizeof err);
    CHECK(dsg_view_read(dir, &w, err, sizeof err) != 0 && w.caret_line == 0, "a bad view file: %s", err);
    CHECK(dsg_project_open(&p, dir, err, sizeof err) == 0 && strstr(p.note, "view") != NULL, "the project still opens: '%s'", p.note);
    dsg_project_close(&p);
}

/* --- 6, 7. create and check -------------------------------------------- */

static void test_create(dsg_checker* c) {
    char dir[DSG_PATH_MAX], f[DSG_PATH_MAX], err[1024];
    dsg_project p;
    dsg_report r;
    path2(dir, sizeof dir, work, "new_project");
    path2(f, sizeof f, dir, "pack.json");
    remove(f);
    path2(f, sizeof f, dir, "experiment.json");
    remove(f);
    path2(f, sizeof f, dir, DSG_CHECK_PACK);
    remove(f);
    CHECK(dsg_project_create(dir, err, sizeof err) == 0, "create: %s", err);
    CHECK(dsg_project_create(dir, err, sizeof err) != 0 && strstr(err, "already"), "a second create refuses: %s", err);
    CHECK(dsg_project_open(&p, dir, err, sizeof err) == 0 && p.version == 2 && !dsg_project_dirty(&p, p.text, p.text_len),
          "the new project opens, version 2, clean: %s", err);
    CHECK(dsg_check(c, &p, p.text, p.text_len, &r) == 0 && r.status == PL_CHECK_OK && r.n_errors == 0 && r.built,
          "and checks clean (status %d)", r.status);
    if (r.n_errors) dump(&r);
    dsg_project_close(&p);
}

static void test_check(dsg_checker* c) {
    char dir[DSG_PATH_MAX], err[1024];
    dsg_project p;
    dsg_report r;
    const dsg_issue* is;
    char* t;
    size_t m;

    if (project_copy("rdk", rdk_files, dir, sizeof dir)) { CHECK(0, "copy rdk"); return; }
    if (dsg_project_open(&p, dir, err, sizeof err)) { CHECK(0, "open rdk: %s", err); return; }

    /* clean: one build, then checks against it */
    CHECK(dsg_check(c, &p, p.text, p.text_len, &r) == 0 && r.status == PL_CHECK_OK && r.n_errors == 0 && r.built && r.have_pack,
          "rdk checks clean and builds its check pack (status %d, %d errors)", r.status, r.n_errors);
    if (r.n_errors) dump(&r);
    is = find_need(&r, PL_NEED_FIELD);
    CHECK(is && is->severity == DSG_SEV_NEED && is->name && strcmp(is->name, "participant") == 0 && is->place.line == 0,
          "the default participant field is a need with no place in the text");
    CHECK(dsg_check(c, &p, p.text, p.text_len, &r) == 0 && r.status == PL_CHECK_OK && !r.built, "a second check does not build");

    /* tier 1: the loader's error, at its key */
    t = replaced(p.text, "\"dot_size\"", "\"dot_sizes\"", &m);
    CHECK(dsg_check(c, &p, t, m, &r) == 0 && r.status == PL_CHECK_DEFINITION && r.n_errors > 0, "an unknown key: status %d", r.status);
    is = find_issue(&r, DSG_FROM_LOADER);
    CHECK(is && is->severity == DSG_SEV_ERROR && at_text(t, &is->place, "\"dot_sizes\""), "the loader's error is at the key (%u:%u)",
          is ? is->place.line : 0, is ? is->place.column : 0);
    if (!is || !at_text(t, &is->place, "\"dot_sizes\"")) dump(&r);
    free(t);

    /* tier 2: the player's refusal, at the stimulus it names */
    t = replaced(p.text, "\"count\": 200", "\"count\": \"$coherence\"", &m);
    CHECK(t != NULL, "the rdk count");
    if (t) {
        CHECK(dsg_check(c, &p, t, m, &r) == 0 && r.status == PL_CHECK_REFUSED, "an rdk count that varies: status %d", r.status);
        is = find_issue(&r, DSG_FROM_PLAYER);
        CHECK(is && is->object == PL_OBJ_STIMULUS && is->name && strcmp(is->name, "dots") == 0 && at_text(t, &is->place, "\"dots\""),
              "the refusal names stimulus 'dots' and is at its key");
        if (!is || is->object != PL_OBJ_STIMULUS) dump(&r);
        free(t);
    }
    /* tier 2: a node, at its name */
    t = replaced(p.text, "\"name\": \"main\"", "\"name\": \"main\", \"vars\": { \"x\": 1 }, \"data\": { \"x\": 2 }", &m);
    CHECK(t && dsg_check(c, &p, t, m, &r) == 0 && r.status == PL_CHECK_REFUSED, "x in a node's vars and data: status %d", r.status);
    is = find_issue(&r, DSG_FROM_PLAYER);
    CHECK(is && is->object == PL_OBJ_NODE && is->name && strcmp(is->name, "main") == 0 && t && at_text(t, &is->place, "\"name\": \"main\""),
          "the refusal names node 'main' and is at its name");
    if (!is || is->object != PL_OBJ_NODE) dump(&r);
    free(t);

    /* a text that does not parse, with no check pack yet: the loader alone */
    dsg_checker_forget(c);
    t = replaced(p.text, "\"stimuli\": {", "\"stimuli\": {,", &m);
    CHECK(dsg_check(c, &p, t, m, &r) == 0 && r.status == -1 && !r.have_pack && r.n_errors == 1, "no pack: status %d, %d errors",
          r.status, r.n_errors);
    is = find_issue(&r, DSG_FROM_LOADER);
    CHECK(is && is->place.line > 1 && t[is->place.offset] == ',', "the parse error's place is the comma (%u:%u)",
          is ? is->place.line : 0, is ? is->place.column : 0);
    CHECK(find_issue(&r, DSG_FROM_DESIGNER) != NULL, "and a note that the pack's checks wait");
    free(t);
    CHECK(dsg_check(c, &p, p.text, p.text_len, &r) == 0 && r.status == PL_CHECK_OK && r.built, "fixed, it builds and checks");
    dsg_project_close(&p);

    /* tier 3: a required calibration, at its key */
    if (project_copy("afc2", afc2_files, dir, sizeof dir)) { CHECK(0, "copy afc2"); return; }
    if (dsg_project_open(&p, dir, err, sizeof err)) { CHECK(0, "open afc2: %s", err); return; }
    CHECK(dsg_check(c, &p, p.text, p.text_len, &r) == 0 && r.status == PL_CHECK_OK && r.built, "afc2 checks: status %d", r.status);
    if (r.status != PL_CHECK_OK) dump(&r);
    is = find_need(&r, PL_NEED_CALIBRATION);
    CHECK(is && at_text(p.text, &is->place, "\"calibration\""), "the calibration need is at its key");
    dsg_project_close(&p);
}

/* --- 8. the check pack follows the texts ------------------------------------- */

static void test_texts(dsg_checker* c) {
    static const char* const files[] = { "pack.json", "stroop.json", "stroop.csv", NULL };
    char dir[DSG_PATH_MAX], fonts[DSG_PATH_MAX], f[DSG_PATH_MAX], err[1024];
    dsg_project p;
    dsg_report r;
    char *t, *t2;
    size_t m, m2;
    if (!dsg_is_file(YSP_DESIGNER_TEST_FONT)) {
        printf("8. skipped: no font at '%s' (YSP_DESIGNER_TEST_FONT)\n", YSP_DESIGNER_TEST_FONT);
        return;
    }
    if (project_copy("stroop", files, dir, sizeof dir)) { CHECK(0, "copy stroop"); return; }
    path2(fonts, sizeof fonts, dir, "fonts");
    dsg_make_dir(fonts, err, sizeof err);
    path2(f, sizeof f, fonts, "NotoSans-Regular.ttf");
    if (copy_file(YSP_DESIGNER_TEST_FONT, f)) { CHECK(0, "copy the font"); return; }
    if (dsg_project_open(&p, dir, err, sizeof err)) { CHECK(0, "open stroop: %s", err); return; }
    CHECK(dsg_check(c, &p, p.text, p.text_len, &r) == 0 && r.status == PL_CHECK_OK && r.built, "stroop checks: status %d", r.status);
    if (r.status != PL_CHECK_OK) dump(&r);

    t = replaced(p.text, "R red", "K red", &m);
    CHECK(t && dsg_check(c, &p, t, m, &r) == 0 && r.built && r.status == PL_CHECK_OK,
          "a new text rebuilds the check pack and checks clean (built %d, status %d)", r.built, r.status);
    if (r.status != PL_CHECK_OK) dump(&r);
    t2 = t ? replaced(t, "\"wait_s\": 0.4", "\"wait_s\": 0.45", &m2) : NULL;
    CHECK(t2 && dsg_check(c, &p, t2, m2, &r) == 0 && !r.built && r.status == PL_CHECK_OK, "a change to no text does not rebuild");
    free(t);
    free(t2);
    dsg_project_close(&p);
}

int main(int argc, char** argv) {
    char err[512];
    dsg_checker* c;
    if (argc < 2) {
        printf("usage: test_designer_core WORK\n");
        return 2;
    }
    work = argv[1];
    if (dsg_make_dir(work, err, sizeof err)) { printf("%s\n", err); return 1; }
    test_places();
    test_redraw();
    test_open_save();
    test_view();
    c = dsg_checker_new();
    CHECK(c != NULL, "a checker");
    if (c) {
        test_create(c);
        test_check(c);
        test_texts(c);
        dsg_checker_free(c);
    }
    printf("%s: %d checks, %d failed\n", fails ? "FAIL" : "PASS", checks, fails);
    return fails ? 1 : 0;
}
