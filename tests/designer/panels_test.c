/* panels_test.c - the panels' fields, objects, writes and renames with no
 * window (designer/core/schema.h; step 4, part C; docs/designer.md, "The
 * panels"). OWNED BY PART C.
 *
 * Usage: test_designer_panels WORK
 *   1. The schema has format 2's rows. The mirror of ysp/experiment.h's
 *      internal enums holds where the rows show it: every key that ends in
 *      _s is a duration in seconds, every row with enum values is an enum,
 *      the required flag is yexp_row_required().
 *   2. A key that a header's parameter table has takes its unit, range and
 *      text: stimuli (gfx, rdk), methods (stair, quest, aep), responses and
 *      choices (response), controls (sig, through the op it lowers to).
 *   3. The object at a path, for each object kind and each kind of stimulus,
 *      control, method, step, exit and variable; the chain up to the
 *      template; the key the path names.
 *   4. Every key that yexp_defaults(2) has for a section and kind shows for
 *      an object of that section and kind.
 *   5. Each type writes the canonical value (a duration typed 0.50 is 0.5),
 *      in one undo step; a text that is not the type writes nothing; Reset
 *      removes the key.
 *   6. A value out of a header's range is written, and the check reports it.
 *   7. The candidates of a name key: states and end, roles with lanes,
 *      controls by what they give.
 *   8. Renames: a state, a stimulus, a top-level control, a signal role, a
 *      template and a variable, with their references, in one undo step; the
 *      check stays clean; undo gives the bytes back; the chart's listener
 *      hears a state's rename. A reference the name types do not find is
 *      the check's to report.
 * Exit 0 on pass, 1 on a failure.
 *
 * test_designer_panels WORK --bench times the schema's work: a frame's (the
 * chain of objects at a path, each object's fields and their values), the
 * candidates of a name key, and renames, on the bimanual definition and on
 * copies of its templates up to about 1 MB (as bench_designer_doc makes
 * them). Microseconds, median and p99. Not a test: it checks nothing.
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "schema.h"
#include "doc.h"
#include "dsg.h"
#include "player.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef YSP_SOURCE_DIR
#define YSP_SOURCE_DIR "."
#endif

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const char* work = ".";

static const char* text0(dsg_doc* d) { return dsg_doc_bytes(d, DSG_BUF_DEF, NULL); }
static bool has(dsg_doc* d, const char* s) { return strstr(text0(d), s) != NULL; }

/* An object of every section and kind (format part 3). It need not load:
 * the schema reads the tree, not the checked form. */
static const char k_all[] =
    "{\"format\": \"ysp-experiment\", \"version\": 2, \"name\": \"all\",\n"
    " \"display\": {\"units\": \"px\"},\n"
    " \"session\": {\"fields\": {\"participant\": {\"type\": \"integer\", \"required\": true}}},\n"
    " \"web\": {\"allowed\": false},\n"
    " \"devices\": {\"resp\": {\"direction\": \"in\", \"family\": [\"keyboard\"]}},\n"
    " \"roles\": {\"hand\": {\"type\": \"position2d\", \"units\": \"display_px\"}},\n"
    " \"stimuli\": {\"s0\": {\"kind\": \"shape\"}, \"s1\": {\"kind\": \"grating\"}, \"s2\": {\"kind\": \"gabor\"},\n"
    "   \"s3\": {\"kind\": \"noise\"}, \"s4\": {\"kind\": \"image\"}, \"s5\": {\"kind\": \"text\"}, \"s6\": {\"kind\": \"artwork\"},\n"
    "   \"s7\": {\"kind\": \"rdk\"}, \"s8\": {\"kind\": \"sound\"}, \"s9\": {\"kind\": \"video\"}},\n"
    " \"methods\": {\"m0\": {\"type\": \"stair\"}, \"m1\": {\"type\": \"quest\"}, \"m2\": {\"type\": \"aep\"}},\n"
    " \"controls\": {\"c0\": {\"control\": \"cursor\"}, \"c1\": {\"control\": \"region\"}, \"c2\": {\"control\": \"distance\"},\n"
    "   \"c3\": {\"control\": \"movement\"}, \"c4\": {\"control\": \"peak\"}, \"c5\": {\"control\": \"moving_target\"},\n"
    "   \"c6\": {\"control\": \"trace\"}, \"c7\": {\"control\": \"signal_lost\"}},\n"
    " \"trials\": {\n"
    "   \"keys\": {\"events\": [{\"on\": \"s0\"}, {\"off\": \"s0\"}, {\"wait_s\": 1}, {\"at_s\": 1}, {\"set\": \"s0\", \"props\": {}},\n"
    "              {\"to\": \"s0\", \"props\": {}}, {\"then\": \"s0\", \"props\": {}}, {\"trigger\": 1, \"role\": \"resp\"},\n"
    "              {\"mark\": \"m\"}, {\"if\": \"$v\", \"steps\": [{\"wait_s\": 1}]}, {\"unless\": \"$v\", \"steps\": []},\n"
    "              {\"until\": \"c1\"}, {\"recenter\": \"hand\", \"to\": [0, 0]}, {\"flag\": \"f\", \"value\": true}],\n"
    "            \"response\": {\"role\": \"resp\", \"from\": \"s0\", \"choices\": [\"a\", {\"key\": \"b\", \"name\": \"B\"}],\n"
    "                         \"trigger\": {\"role\": \"resp\", \"code\": 1}},\n"
    "            \"columns\": {\"col\": \"$v\"}},\n"
    "   \"chart\": {\"flags\": [\"f\"], \"controls\": {\"mv\": {\"control\": \"movement\"}},\n"
    "             \"states\": [{\"name\": \"a\", \"steps\": [{\"wait_s\": 1}], \"exits\": [{\"to\": \"end\"}],\n"
    "                         \"max_visits\": 2, \"over\": {\"to\": \"end\"}}],\n"
    "             \"exits\": [{\"in\": [\"a\"], \"on\": \"c1\", \"to\": \"end\"}],\n"
    "             \"response\": {\"choices\": [{\"signal\": \"mv\"}], \"from\": \"s0\"}}},\n"
    " \"timeline\": [{\"name\": \"n\", \"loop\": {\"until_accuracy\": 0.8, \"max\": 3},\n"
    "               \"data\": {\"d\": 1},\n"
    "               \"vars\": {\"u\": {\"uniform\": [0, 1]}, \"w\": {\"walk2d\": {\"step\": 1, \"box\": [[0, 0], [2, 2]]}},\n"
    "                        \"c\": {\"choice\": [1, 2]}, \"v\": true},\n"
    "               \"timeline\": [{\"name\": \"leaf\", \"trial\": \"keys\"}]}]}\n";

static yjs_arena g_arena;

static const yjs_value* parse(const char* t) {
    yjs_value* v = NULL;
    yjs_error e;
    yjs_arena_reset(&g_arena);
    if (yjs_parse(&g_arena, t, strlen(t), NULL, &v, &e) != YJS_OK) {
        printf("  parse: %u:%u %s\n", e.line, e.column, e.msg);
        return NULL;
    }
    return v;
}

static char* read_all(const char* path, size_t* n) {
    char* d = NULL;
    char err[512];
    if (dsg_read_file(path, &d, n, err, sizeof err)) printf("  %s\n", err);
    return d;
}

/* --- 1. the schema ------------------------------------------------------------ */

static void test_schema(void) {
    int n, i, nr = 0, bad_dur = 0, bad_enum = 0, bad_req = 0, bad_flags = 0, bad_cover = 0;
    const dsg_field* f = dsg_schema_fields(&n);
    const yexp_row* rows = yexp_defaults(2, &nr);
    CHECK(f && n >= nr && nr > 100, "format 2's rows (%d fields, %d rows)", n, nr);
    CHECK(dsg_schema_section("stimulus") >= 0 && dsg_schema_section("exit") >= 0 && dsg_schema_section("no such") == -1,
          "sections by name");
    for (i = 0; f && i < n; i++) {
        size_t kl = strlen(f[i].key);
        const bool dur_key = kl > 2 && strcmp(f[i].key + kl - 2, "_s") == 0;
        if (dur_key != (f[i].type == DSG_T_DUR)) { bad_dur++; printf("  duration? %s (type %d)\n", f[i].key, f[i].type); }
        if (f[i].type == DSG_T_DUR && strcmp(f[i].unit, "s") != 0) bad_dur++;
        if ((f[i].values != NULL) != (f[i].type == DSG_T_ENUM || f[i].type == DSG_T_ENUMS)) {
            bad_enum++;
            printf("  enum? %s (type %d)\n", f[i].key, f[i].type);
        }
        if (f[i].required != ((f[i].flags & DSG_R_REQ) != 0)) bad_req++;
        /* optional with no default: the row has no default text */
        if (((f[i].flags & (DSG_R_NONE | DSG_R_REQ)) != 0) != (f[i].def == NULL)) {
            bad_flags++;
            printf("  flags? %s %s (flags %u)\n", yexp_section_name(f[i].section), f[i].key, f[i].flags);
        }
    }
    /* every row's kinds are covered once by the fields (a split keeps them all) */
    for (i = 0; rows && i < nr; i++) {
        unsigned cover = 0;
        int k;
        for (k = 0; f && k < n; k++)
            if (f[k].section == rows[i].section && strcmp(f[k].key, rows[i].key) == 0 && f[k].type == rows[i].type &&
                (f[k].kinds & rows[i].kinds)) {
                if (cover & f[k].kinds) bad_cover++;
                cover |= f[k].kinds;
            }
        if (cover != rows[i].kinds) { bad_cover++; printf("  cover? %s %s\n", yexp_section_name(rows[i].section), rows[i].key); }
    }
    CHECK(bad_dur == 0, "every _s key is a duration in seconds, and only those (%d not)", bad_dur);
    CHECK(bad_enum == 0, "every row with values is an enum (%d not)", bad_enum);
    CHECK(bad_req == 0, "the required flag is yexp_row_required() (%d not)", bad_req);
    CHECK(bad_flags == 0, "a row is required or optional with no default exactly when it has no default (%d not)", bad_flags);
    CHECK(bad_cover == 0, "the fields cover each row's kinds once (%d not)", bad_cover);
}

/* --- 2. the headers' tables --------------------------------------------------- */

static bool header_has(int section, unsigned kind_bit, const char* key) {
    int n = 0, i;
    if (section == dsg_schema_section("stimulus")) {
        const ygfx_param* g = ygfx_params(&n);
        if (strcmp(key, "color") == 0) key = "color_r";
        for (i = 0; i < n; i++) if (strcmp(g[i].name, key) == 0) return true;
        g = ygfx_enum_params(&n);
        for (i = 0; i < n; i++) if (strcmp(g[i].name, key) == 0) return true;
        if (kind_bit & (1u << YEXP_RDK)) {
            const yrdk_param* r = yrdk_params(&n);
            for (i = 0; i < n; i++) if (strcmp(r[i].name, key) == 0) return true;
        }
        return false;
    }
    if (section == dsg_schema_section("stair method")) {
        const yst_param* r = yst_params(&n);
        for (i = 0; i < n; i++) if (strcmp(r[i].name, key) == 0) return true;
    }
    if (section == dsg_schema_section("quest method")) {
        const yqst_param* r = yqst_params(&n);
        for (i = 0; i < n; i++) if (strcmp(r[i].name, key) == 0) return true;
    }
    if (section == dsg_schema_section("aep method")) {
        const yaep_param* r = yaep_params(&n);
        for (i = 0; i < n; i++) if (strcmp(r[i].name, key) == 0) return true;
    }
    return false;
}

static const dsg_field* field(const char* section, unsigned kind_bit, const char* key) {
    return dsg_schema_field(dsg_schema_section(section), kind_bit, key);
}

static void test_headers(void) {
    static const char* const secs[] = { "stimulus", "stair method", "quest method", "aep method" };
    int s, missing = 0, joined = 0;
    const dsg_field* f;
    for (s = 0; s < 4; s++) {
        const int sn = dsg_schema_section(secs[s]);
        int b;
        for (b = 0; b < 16; b++) {
            const unsigned kb = s == 0 ? 1u << b : 0xffffu;
            const dsg_field* sel[256];
            int k = dsg_schema_select(sn, kb, sel, 256), i;
            for (i = 0; i < k && i < 256; i++) {
                if (!header_has(sn, kb, sel[i]->key)) continue;
                joined++;
                if (strcmp(sel[i]->from, "format") == 0 || !sel[i]->unit) {
                    missing++;
                    printf("  no join: %s %s (kind bit %x)\n", secs[s], sel[i]->key, kb);
                }
            }
            if (s != 0) break;
        }
    }
    CHECK(joined > 150 && missing == 0, "each stimulus and method key a header's table has takes its row (%d joined, %d not)", joined,
          missing);
    f = field("stimulus", 1u << YEXP_GRATING, "contrast");
    CHECK(f && strcmp(f->from, "ysp/gfx.h") == 0 && f->min == -1000 && f->max == 1000, "a grating's contrast has gfx's range");
    f = field("stimulus", 1u << YEXP_RDK, "coherence");
    CHECK(f && strcmp(f->from, "ysp/rdk.h") == 0 && f->min == 0 && f->max == 1, "an rdk's coherence has rdk's range");
    f = field("stimulus", 1u << YEXP_SHAPE, "w");
    CHECK(f && strcmp(f->unit, "u") == 0 && f->doc[0], "a shape's w is in gfx's unit, with gfx's text");
    f = field("stair method", 0xffff, "target_p");
    CHECK(f && strcmp(f->from, "ysp/stair.h") == 0 && f->max == 1, "a stair's target_p from stair.h");
    f = field("quest method", 0xffff, "stop_entropy");
    CHECK(f && strcmp(f->unit, "bit") == 0, "quest's stop_entropy is in bits");
    f = field("aep method", 0xffff, "hyperparams.noise_sd");
    CHECK(f && strcmp(f->from, "ysp/aep.h") == 0 && f->doc[0], "aep's dotted keys join");
    /* responses: a duration from a field in ns is in seconds */
    f = field("response", 0xffff, "duration_s");
    CHECK(f && strcmp(f->unit, "s") == 0 && strstr(f->from, "response.h") && fabs(f->max - 1e6) < 1e-3, "a response's duration_s: s, from ns");
    f = field("response with signal choices", 0xffff, "minimum_valid_rt_s");
    CHECK(f && strstr(f->from, "response.h") && f->doc[0], "a signal-choice response's minimum_valid_rt_s");
    f = field("choice", 0xffff, "key");
    CHECK(f && strstr(f->from, "response.h"), "a choice's key: choices.key");
    /* controls: the op each key lowers to (3.7), per kind */
    f = field("control", 1u << YEXP_REGION, "radius");
    CHECK(f && strcmp(f->from, "ysp/sig.h") == 0 && f->doc[0], "a region's radius: detect's level");
    f = field("control", 1u << YEXP_REGION, "stay_s");
    CHECK(f && strcmp(f->unit, "s") == 0 && strcmp(f->from, "ysp/sig.h") == 0, "a region's stay_s: detect's hold");
    f = field("control", 1u << YEXP_PEAK, "lag_s");
    CHECK(f && f->min > 0 && f->max == 10, "a peak's lag_s: speed's lag, 1e-6 to 10 s");
    f = field("control", 1u << YEXP_TRACE, "rate");
    {
        const dsg_field* g = field("control", 1u << YEXP_REGION, "rate");
        CHECK(f && g && f != g && strstr(f->doc, "") && f->doc != g->doc, "rate splits: a trace's is record's, a region's is detect's");
    }
    f = field("control", 1u << YEXP_SIGNAL_LOST, "after_s");
    CHECK(f && strcmp(f->from, "ysp/sig.h") == 0, "signal_lost's after_s: silent's after");
    f = field("control", 1u << YEXP_MOVEMENT, "onset");
    CHECK(f && strstr(f->from, "response.h"), "a movement's onset: a choice's level");
    /* every duration has a unit, every key has a unit string */
    {
        int n, i, bad = 0;
        const dsg_field* all = dsg_schema_fields(&n);
        for (i = 0; all && i < n; i++)
            if (!all[i].unit || !all[i].doc || !all[i].from || (all[i].type == DSG_T_DUR && strcmp(all[i].unit, "s") != 0)) bad++;
        CHECK(bad == 0, "every field has a unit, a text and a source (%d not)", bad);
    }
}

/* --- 3. the object at a path -------------------------------------------------- */

static void expect(const yjs_value* root, const char* path, int obj, const char* section, unsigned kind_bit, const char* focus) {
    dsg_obj o;
    int rc = dsg_schema_object(root, path, &o);
    const int s = section ? dsg_schema_section(section) : -1;
    CHECK(rc == 0 && o.obj == obj && o.section == s && o.kind_bit == kind_bit && strcmp(o.focus, focus) == 0,
          "%s: %s %s bit %x focus '%s' (got %s, section %d, bit %x, focus '%s')", path, dsg_schema_obj_name(obj),
          section ? section : "-", kind_bit, focus, rc ? "nothing" : dsg_schema_obj_name(o.obj), rc ? -2 : o.section,
          rc ? 0 : o.kind_bit, rc ? "" : o.focus);
}

static void test_objects(void) {
    static const char* const stim_kinds[] = { "shape", "grating", "gabor", "noise", "image", "text", "artwork", "rdk", "sound", "video" };
    static const char* const ops[] = { "on", "off", "wait_s", "at_s", "set", "to", "then", "trigger", "mark", "if", "unless",
                                       "until", "recenter", "flag" };
    const yjs_value* root = parse(k_all);
    dsg_obj c[8];
    char p[64];
    int i, n;
    if (!root) { CHECK(0, "the test definition parses"); return; }
    expect(root, "", DSG_OBJ_FILE, "file", 0xffff, "");
    expect(root, "/version", DSG_OBJ_FILE, "file", 0xffff, "version");
    expect(root, "/display/units", DSG_OBJ_DISPLAY, "display", 0xffff, "units");
    expect(root, "/display/min_tier", DSG_OBJ_DISPLAY, "display", 0xffff, "min_tier");
    expect(root, "/session", DSG_OBJ_SESSION, "session", 0xffff, "");
    expect(root, "/session/fields/participant/type", DSG_OBJ_SFIELD, "session field", 0xffff, "type");
    expect(root, "/web", DSG_OBJ_WEB, "web", 0xffff, "");
    expect(root, "/devices/resp/family/0", DSG_OBJ_EVROLE, "role", 0xffff, "family");
    expect(root, "/roles/hand", DSG_OBJ_SIGROLE, "signal role", 0xffff, "");
    expect(root, "/stimuli", DSG_OBJ_FILE, "file", 0xffff, "stimuli");
    for (i = 0; i < 10; i++) {
        snprintf(p, sizeof p, "/stimuli/s%d", i);
        expect(root, p, DSG_OBJ_STIMULUS, "stimulus", 1u << i, "");
        CHECK(dsg_schema_object(root, p, c) == 0 && strcmp(c[0].kind, stim_kinds[i]) == 0 && strcmp(c[0].set, "/stimuli") == 0 &&
                  c[0].name[0] == 's',
              "%s is a %s, member of /stimuli", p, stim_kinds[i]);
    }
    expect(root, "/stimuli/s1/contrast", DSG_OBJ_STIMULUS, "stimulus", 1u << YEXP_GRATING, "contrast");
    expect(root, "/methods/m0", DSG_OBJ_METHOD, "stair method", 0xffff, "");
    expect(root, "/methods/m1/pf", DSG_OBJ_METHOD, "quest method", 0xffff, "pf");
    expect(root, "/methods/m2", DSG_OBJ_METHOD, "aep method", 0xffff, "");
    for (i = 0; i < 8; i++) {
        snprintf(p, sizeof p, "/controls/c%d", i);
        expect(root, p, DSG_OBJ_CONTROL, "control", 1u << i, "");
    }
    expect(root, "/trials/keys", DSG_OBJ_TEMPLATE, "template", 1u, "");
    expect(root, "/trials/chart", DSG_OBJ_TEMPLATE, "template", 2u, "");
    for (i = 0; i < 14; i++) {
        snprintf(p, sizeof p, "/trials/keys/events/%d", i);
        expect(root, p, DSG_OBJ_STEP, "step", 1u << i, "");
        CHECK(dsg_schema_object(root, p, c) == 0 && strcmp(c[0].kind, ops[i]) == 0 && strcmp(c[0].tmpl, "keys") == 0,
              "%s is a %s step in template keys", p, ops[i]);
    }
    expect(root, "/trials/keys/events/9/steps/0/wait_s", DSG_OBJ_STEP, "step", 1u << YEXP_WAIT, "wait_s");
    expect(root, "/trials/keys/response", DSG_OBJ_RESPONSE, "response", 0xffff, "");
    expect(root, "/trials/keys/response/trigger/code", DSG_OBJ_RTRIGGER, "response trigger", 0xffff, "code");
    expect(root, "/trials/keys/response/choices/0", DSG_OBJ_CHOICE, "choice", 0xffff, "");
    expect(root, "/trials/keys/response/choices/1/name", DSG_OBJ_CHOICE, "choice", 0xffff, "name");
    expect(root, "/trials/keys/columns/col", DSG_OBJ_COLUMN, NULL, 0xffff, "");
    expect(root, "/trials/chart/response", DSG_OBJ_RESPONSE, "response with signal choices", 0xffff, "");
    expect(root, "/trials/chart/response/choices/0", DSG_OBJ_CHOICE, NULL, 0xffff, "");
    expect(root, "/trials/chart/controls/mv", DSG_OBJ_CONTROL, "control", 1u << YEXP_MOVEMENT, "");
    expect(root, "/trials/chart/states/0", DSG_OBJ_STATE, "state", 0xffff, "");
    expect(root, "/trials/chart/states/0/max_visits", DSG_OBJ_STATE, "state", 0xffff, "max_visits");
    expect(root, "/trials/chart/states/0/over/to", DSG_OBJ_OVER, "over", 0xffff, "to");
    expect(root, "/trials/chart/states/0/exits/0/to", DSG_OBJ_EXIT, "exit", 1u, "to");
    expect(root, "/trials/chart/states/0/steps/0", DSG_OBJ_STEP, "step", 1u << YEXP_WAIT, "");
    expect(root, "/trials/chart/exits/0/in/0", DSG_OBJ_SHARED_EXIT, "exit", 2u, "in");
    expect(root, "/timeline/0", DSG_OBJ_NODE, "node", 0xffff, "");
    expect(root, "/timeline/0/loop/max", DSG_OBJ_LOOP, "loop", 0xffff, "max");
    expect(root, "/timeline/0/timeline/0/trial", DSG_OBJ_NODE, "node", 0xffff, "trial");
    expect(root, "/timeline/0/vars/u", DSG_OBJ_VAR, "uniform", 0xffff, "");
    expect(root, "/timeline/0/vars/w/walk2d/step", DSG_OBJ_VAR, "walk2d", 0xffff, "step");
    expect(root, "/timeline/0/vars/c", DSG_OBJ_VAR, NULL, 0xffff, "");
    expect(root, "/timeline/0/vars/v", DSG_OBJ_VAR, NULL, 0xffff, "");
    expect(root, "/timeline/0/data/d", DSG_OBJ_DATA, NULL, 0xffff, "");
    CHECK(dsg_schema_object(root, "/timeline/0/vars/w", c) == 0 && strcmp(c[0].keys, "/timeline/0/vars/w/walk2d") == 0 &&
              strcmp(c[0].name, "w") == 0,
          "a walk2d variable's keys are under walk2d ('%s')", c[0].keys);
    CHECK(dsg_schema_object(root, "/timeline/0/timeline/0", c) == 0 && strcmp(c[0].name, "leaf") == 0 && !c[0].set[0] &&
              dsg_schema_renamable(&c[0]),
          "a node's name is its name key");
    /* the chain: the exit, its state, its template */
    n = dsg_schema_chain(root, "/trials/chart/states/0/exits/0/to", c, 8);
    CHECK(n == 3 && c[0].obj == DSG_OBJ_EXIT && c[1].obj == DSG_OBJ_STATE && c[2].obj == DSG_OBJ_TEMPLATE &&
              strcmp(c[2].name, "chart") == 0 && strcmp(c[1].name, "a") == 0,
          "the chain of an exit (%d)", n);
    n = dsg_schema_chain(root, "/trials/keys/events/9/steps/0", c, 8);
    CHECK(n == 2 && c[0].obj == DSG_OBJ_STEP && c[1].obj == DSG_OBJ_TEMPLATE, "a step in an if step: the template above (%d)", n);
    n = dsg_schema_chain(root, "/stimuli/s1/sf", c, 8);
    CHECK(n == 1 && c[0].obj == DSG_OBJ_STIMULUS, "a stimulus alone (%d)", n);
    n = dsg_schema_chain(root, "/trials/keys/response/trigger", c, 8);
    CHECK(n == 3 && c[1].obj == DSG_OBJ_RESPONSE && c[2].obj == DSG_OBJ_TEMPLATE, "a trigger, its response, its template (%d)", n);
}

/* --- 4. every key of a section shows -------------------------------------------- */

static void test_cover(void) {
    static const char* const paths[] = {
        "", "/display", "/session", "/session/fields/participant", "/web", "/devices/resp", "/roles/hand",
        "/stimuli/s0", "/stimuli/s1", "/stimuli/s2", "/stimuli/s3", "/stimuli/s4", "/stimuli/s5", "/stimuli/s6",
        "/stimuli/s7", "/stimuli/s8", "/stimuli/s9", "/methods/m0", "/methods/m1", "/methods/m2",
        "/controls/c0", "/controls/c1", "/controls/c2", "/controls/c3", "/controls/c4", "/controls/c5", "/controls/c6",
        "/controls/c7", "/trials/keys", "/trials/chart", "/trials/keys/response", "/trials/keys/response/trigger",
        "/trials/keys/response/choices/1", "/trials/chart/response", "/trials/chart/states/0", "/trials/chart/states/0/over",
        "/trials/chart/states/0/exits/0", "/trials/chart/exits/0", "/timeline/0", "/timeline/0/loop", "/timeline/0/vars/u",
        "/timeline/0/vars/w", "/trials/keys/events/0", "/trials/keys/events/1", "/trials/keys/events/2",
        "/trials/keys/events/3", "/trials/keys/events/4", "/trials/keys/events/5", "/trials/keys/events/6",
        "/trials/keys/events/7", "/trials/keys/events/8", "/trials/keys/events/9", "/trials/keys/events/10",
        "/trials/keys/events/11", "/trials/keys/events/12", "/trials/keys/events/13",
    };
    const yjs_value* root = parse(k_all);
    int nr = 0, i, j, objs = 0, missing = 0, shown = 0;
    const yexp_row* rows = yexp_defaults(2, &nr);
    unsigned seen[64][16];
    memset(seen, 0, sizeof seen);
    for (i = 0; root && i < (int)(sizeof paths / sizeof paths[0]); i++) {
        dsg_obj o;
        const dsg_field* sel[256];
        int k;
        if (dsg_schema_object(root, paths[i], &o) || o.section < 0) { CHECK(0, "%s is an object with keys", paths[i]); continue; }
        k = dsg_schema_select(o.section, o.kind_bit, sel, 256);
        objs++;
        for (j = 0; j < nr; j++) {
            int q, found = 0;
            if (rows[j].section != o.section || !(rows[j].kinds & o.kind_bit) || (rows[j].flags & DSG_R_STEP)) continue;
            for (q = 0; q < k && q < 256; q++) found += strcmp(sel[q]->key, rows[j].key) == 0 && sel[q]->type == rows[j].type;
            if (found != 1) { missing++; printf("  %s: '%s' shows %d times\n", paths[i], rows[j].key, found); }
            shown++;
        }
        if (o.section < 64) {
            int b;
            for (b = 0; b < 16; b++)
                if (o.kind_bit & (1u << b)) seen[o.section][b] = 1;
        }
    }
    CHECK(objs == (int)(sizeof paths / sizeof paths[0]) && missing == 0 && shown > 600,
          "every key of each section and kind shows once for its object (%d objects, %d keys, %d not)", objs, shown, missing);
    /* and every section and kind of the table had an object above */
    {
        int unseen = 0;
        for (j = 0; j < nr; j++) {
            int b, any = 0;
            for (b = 0; b < 16; b++)
                if ((rows[j].kinds & (1u << b)) && rows[j].section < 64 && seen[rows[j].section][b]) any = 1;
            if (!any && !(rows[j].flags & DSG_R_STEP)) { unseen++; printf("  no object for %s %s\n", yexp_section_name(rows[j].section), rows[j].key); }
        }
        CHECK(unseen == 0, "the test has an object for every section and kind (%d rows not)", unseen);
    }
    /* a stimulus's visible is a step's property (D41): not in its panel */
    {
        const dsg_field* sel[256];
        int k = dsg_schema_select(dsg_schema_section("stimulus"), 1u, sel, 256), q, vis = 0;
        for (q = 0; q < k && q < 256; q++) vis += strcmp(sel[q]->key, "visible") == 0;
        CHECK(vis == 0, "visible is not a stimulus key in version 2");
    }
}

/* --- 5. writes ------------------------------------------------------------------- */

static const char k_small[] =
    "{\"format\": \"ysp-experiment\", \"version\": 2,\n"
    " \"devices\": {\"resp\": {\"direction\": \"in\", \"family\": [\"keyboard\"]}},\n"
    " \"stimuli\": {\"g\": {\"kind\": \"grating\", \"sf\": 2}, \"t\": {\"kind\": \"text\", \"font\": \"f.ttf\", \"text\": \"hi\"}},\n"
    " \"trials\": {\"t\": {\"events\": [{\"on\": \"g\"}, {\"wait_s\": 1}],\n"
    "                    \"response\": {\"role\": \"resp\", \"from\": \"g\", \"choices\": [\"left\", \"right\"]}}},\n"
    " \"timeline\": [{\"name\": \"main\", \"trial\": \"t\"}]}\n";

static int write_at(dsg_doc* d, const char* path, const char* key, const char* text, unsigned how) {
    dsg_obj o;
    const dsg_field* f;
    char err[256] = "";
    int rc;
    if (dsg_schema_object(dsg_doc_root(d), path, &o)) return -2;
    f = dsg_schema_field(o.section, o.kind_bit, key);
    if (!f) return -3;
    rc = dsg_schema_write(d, &o, f, text, how, err, sizeof err);
    if (rc) printf("  (write %s %s '%s': %s)\n", path, key, text, err);
    return rc;
}

static int reset_at(dsg_doc* d, const char* path, const char* key) {
    dsg_obj o;
    const dsg_field* f;
    char err[256] = "";
    if (dsg_schema_object(dsg_doc_root(d), path, &o)) return -2;
    f = dsg_schema_field(o.section, o.kind_bit, key);
    return f ? dsg_schema_reset(d, &o, f, err, sizeof err) : -3;
}

static void test_writes(void) {
    dsg_doc* d = dsg_doc_new();
    int depth;
    const dsg_field* f;
    char buf[256];
    dsg_obj o;
    if (!d || dsg_doc_reset(d, "experiment.json", k_small, strlen(k_small))) { CHECK(0, "a document"); return; }
    depth = dsg_doc_undo_depth(d);
    CHECK(write_at(d, "/trials/t/events/1", "wait_s", "0.50", 0) == 0 && has(d, "\"wait_s\": 0.5\n"),
          "a duration typed 0.50 is 0.5");
    CHECK(dsg_doc_undo_depth(d) == depth + 1, "one undo step");
    CHECK(write_at(d, "/trials/t/events/1", "wait_s", "1.0000000004", 0) == 0 && has(d, "\"wait_s\": 1\n"),
          "a duration is read to the ns (1.0000000004 is 1)");
    CHECK(write_at(d, "/trials/t/events/1", "wait_s", " 0.25 ", 0) == 0 && has(d, "\"wait_s\": 0.25\n"), "spaces around a number go");
    CHECK(write_at(d, "/stimuli/g", "contrast", "0.50", 0) == 0 && has(d, "\"contrast\": 0.5,"), "a number typed 0.50 is 0.5");
    CHECK(write_at(d, "/stimuli/g", "sf", "5e-1", 0) == 0 && has(d, "\"sf\": 0.5\n"), "5e-1 is 0.5");
    CHECK(write_at(d, "/stimuli/g", "layer", "3", 0) == 0 && has(d, "\"layer\": 3,"), "an integer");
    CHECK(write_at(d, "/stimuli/g", "aperture", "circle", 0) == 0 && has(d, "\"aperture\": \"circle\""), "an enum's name");
    CHECK(write_at(d, "/stimuli/t", "color", "[1, 0.50, 0]", 0) == 0 && has(d, "0.5,\n") && has(d, "\"color\": ["), "a list as JSON");
    CHECK(strstr(text0(d), "0.50") == NULL, "no number keeps its typed form");
    CHECK(write_at(d, "/stimuli/g", "ori", "$angle", 0) == 0 && has(d, "\"ori\": \"$angle\""), "a variable where a number goes");
    CHECK(write_at(d, "/stimuli/g", "x", "{\"by\": \"$side\", \"map\": {\"left\": -5, \"right\": 5}}", DSG_WRITE_JSON) == 0 &&
              has(d, "\"by\": \"$side\""),
          "a lookup as JSON");
    CHECK(write_at(d, "/stimuli/t", "text", " \xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D {x} ", 0) == 0 &&
              has(d, "\"text\": \" \xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D {x} \""),
          "user text as typed: its spaces and a brace kept");
    CHECK(write_at(d, "/stimuli/t", "text", "\"quoted\"", DSG_WRITE_JSON) == 0 && has(d, "\"text\": \"quoted\""),
          "a JSON string in JSON mode");
    CHECK(write_at(d, "/trials/t/response", "response_ends_trial", "false", 0) == 0 && has(d, "\"response_ends_trial\": false"),
          "a bool");
    CHECK(write_at(d, "/trials/t/response", "key_answer", "1", 0) == 0 && has(d, "\"key_answer\": 1"), "a value: a number");
    CHECK(write_at(d, "/trials/t/response", "key_answer", "left", 0) == 0 && has(d, "\"key_answer\": \"left\""), "a value: text");
    CHECK(write_at(d, "/trials/t/response/choices/0", "name", "\xD7\xA9\xD7\x9E\xD7\x90\xD7\x9C", 0) == 0 &&
              has(d, "\"key\": \"left\"") && has(d, "\"name\": \"\xD7\xA9\xD7\x9E\xD7\x90\xD7\x9C\""),
          "a string choice becomes {key, name}");
    CHECK(write_at(d, "/", "name", "small", 0) == 0 || write_at(d, "", "name", "small", 0) == 0, "the file's name");
    CHECK(has(d, "\"name\": \"small\""), "the file's name is written");
    depth = dsg_doc_undo_depth(d);
    CHECK(write_at(d, "/stimuli/g", "contrast", "abc", 0) == -1 && dsg_doc_undo_depth(d) == depth, "not a number: no step");
    CHECK(write_at(d, "/trials/t/events/1", "wait_s", "1 s", 0) == -1 && dsg_doc_undo_depth(d) == depth, "not seconds: no step");
    CHECK(write_at(d, "/stimuli/g", "layer", "x", 0) == -1, "not an integer");
    CHECK(write_at(d, "/stimuli/t", "color", "[1,", 0) == -1 && dsg_doc_undo_depth(d) == depth, "not JSON: no step");
    /* Reset */
    CHECK(reset_at(d, "/stimuli/g", "contrast") == 0 && !has(d, "\"contrast\"") && dsg_doc_undo_depth(d) == depth + 1,
          "Reset removes the key in one step");
    CHECK(reset_at(d, "/stimuli/g", "contrast") == 0 && dsg_doc_undo_depth(d) == depth + 1, "Reset of an absent key: no step");
    dsg_doc_undo(d);
    CHECK(has(d, "\"contrast\": 0.5"), "undo gives the key back");
    /* what a field shows */
    dsg_schema_object(dsg_doc_root(d), "/stimuli/g", &o);
    f = dsg_schema_field(o.section, o.kind_bit, "edge");
    dsg_schema_value_text(f, NULL, buf, sizeof buf);
    CHECK(f && strcmp(buf, "hard") == 0, "an absent enum shows its default's name ('%s')", buf);
    f = dsg_schema_field(o.section, o.kind_bit, "ax");
    dsg_schema_value_text(f, NULL, buf, sizeof buf);
    CHECK(f && strcmp(buf, "0.5") == 0, "an absent number shows its default ('%s')", buf);
    f = dsg_schema_field(o.section, o.kind_bit, "ori");
    CHECK(f && !dsg_schema_fits(f, dsg_path_get(dsg_doc_root(d), "/stimuli/g/ori")), "a variable does not fit a number widget");
    f = dsg_schema_field(o.section, o.kind_bit, "x");
    dsg_schema_value_text(f, dsg_path_get(dsg_doc_root(d), "/stimuli/g/x"), buf, sizeof buf);
    CHECK(strcmp(buf, "{\"by\":\"$side\",\"map\":{\"left\":-5,\"right\":5}}") == 0, "a lookup shows as compact JSON ('%s')", buf);
    dsg_schema_value_text(f, dsg_path_get(dsg_doc_root(d), "/stimuli/g/x"), buf, 12);
    CHECK(strlen(buf) == 11 && strcmp(buf + 8, "...") == 0, "a long value is cut with ... ('%s')", buf);
    dsg_doc_free(d);
}

/* --- 6. out of a header's range: written, and the check reports it --------------- */

static int copy_file(const char* from, const char* to) {
    char* d;
    size_t n;
    char err[512];
    if (dsg_read_file(from, &d, &n, err, sizeof err)) { printf("  %s\n", err); return -1; }
    if (dsg_write_file(to, d, n, err, sizeof err)) { printf("  %s\n", err); free(d); return -1; }
    free(d);
    return 0;
}

static int project_copy(const char* name, const char* const* files, char* out, size_t cap) {
    char src[DSG_PATH_MAX], a[DSG_PATH_MAX], b[DSG_PATH_MAX], err[512];
    int i;
    snprintf(src, sizeof src, "%s/tests/player/%s", YSP_SOURCE_DIR, name);
    if (dsg_join(out, cap, work, name) || dsg_make_dir(out, err, sizeof err)) return -1;
    for (i = 0; files[i]; i++) {
        if (dsg_join(a, sizeof a, src, files[i]) || dsg_join(b, sizeof b, out, files[i]) || copy_file(a, b)) return -1;
    }
    if (dsg_join(b, sizeof b, out, DSG_VIEW_FILE) == 0) remove(b);
    return 0;
}

static const dsg_issue* issue_with(const dsg_report* r, const char* a, const char* b) {
    int i;
    for (i = 0; i < r->n_issues; i++)
        if (r->issues[i].severity == DSG_SEV_ERROR && strstr(r->issues[i].message, a) && (!b || strstr(r->issues[i].message, b)))
            return &r->issues[i];
    return NULL;
}

static void dump(const dsg_report* r) {
    int i;
    for (i = 0; i < r->n_issues; i++) printf("    [%d] %s\n", r->issues[i].severity, r->issues[i].message);
}

static int open_doc(const char* name, const char* const* files, dsg_project* p, dsg_doc* d) {
    char dir[DSG_PATH_MAX], err[1024];
    if (project_copy(name, files, dir, sizeof dir)) { CHECK(0, "copy %s", name); return -1; }
    if (dsg_project_open(p, dir, err, sizeof err)) { CHECK(0, "open %s: %s", name, err); return -1; }
    if (dsg_doc_reset(d, p->def_source, p->text, p->text_len)) { CHECK(0, "the document of %s", name); dsg_project_close(p); return -1; }
    return 0;
}

static bool check_clean(dsg_checker* c, dsg_project* p, dsg_doc* d, const char* what) {
    dsg_report r;
    size_t n;
    const char* t = dsg_doc_bytes(d, DSG_BUF_DEF, &n);
    bool ok = dsg_check(c, p, t, n, &r) == 0 && r.status == DSG_CHECK_OK && r.n_errors == 0;
    CHECK(ok, "%s: the check is clean (status %d, %d errors)", what, r.status, r.n_errors);
    if (!ok) dump(&r);
    return ok;
}

static void test_range(dsg_checker* c) {
    static const char* const files[] = { "pack.json", "experiment.json", "conditions.csv", NULL };
    dsg_project p;
    dsg_doc* d = dsg_doc_new();
    dsg_report r;
    size_t n;
    const char* t;
    const dsg_issue* is;
    if (!d || open_doc("rdk", files, &p, d)) { dsg_doc_free(d); return; }
    check_clean(c, &p, d, "rdk");
    CHECK(write_at(d, "/stimuli/dots", "coherence", "1.5", 0) == 0 && has(d, "\"coherence\": 1.5"),
          "a coherence above rdk's 1 is written");
    t = dsg_doc_bytes(d, DSG_BUF_DEF, &n);
    CHECK(dsg_check(c, &p, t, n, &r) == 0 && r.n_errors > 0, "and the check refuses it (%d errors)", r.n_errors);
    is = issue_with(&r, "coherence", "rdk.h");
    {
        char at[DSG_SEL_PATH] = "";
        const yjs_value* root = dsg_doc_root(d);
        if (is && is->place.line && root) dsg_path_at(root, is->place.offset, at, sizeof at);
        CHECK(is && dsg_path_within(at, "/stimuli/dots/coherence"), "the check names coherence and rdk.h's range at its key ('%s')", at);
    }
    if (!is) dump(&r);
    CHECK(write_at(d, "/stimuli/dots", "dot_size", "-4", 0) == 0 && has(d, "\"dot_size\": -4"), "a dot_size below gfx's 0 is written");
    reset_at(d, "/stimuli/dots", "coherence");
    t = dsg_doc_bytes(d, DSG_BUF_DEF, &n);
    CHECK(dsg_check(c, &p, t, n, &r) == 0 && issue_with(&r, "dot_size", "gfx.h"), "the check names dot_size and gfx.h's range");
    dsg_project_close(&p);
    dsg_doc_free(d);
}

/* --- 7. candidates ---------------------------------------------------------------- */

static bool listed(const char* buf, int n, const char* name) {
    int i;
    const char* s = buf;
    for (i = 0; i < n; i++, s += strlen(s) + 1)
        if (strcmp(s, name) == 0) return true;
    return false;
}

static void test_candidates(void) {
    const yjs_value* root;
    char* text;
    size_t n;
    char buf[2048], path[DSG_PATH_MAX];
    dsg_obj o;
    int k;
    snprintf(path, sizeof path, "%s/tests/player/bimanual/experiment.json", YSP_SOURCE_DIR);
    text = read_all(path, &n);
    root = text ? parse(text) : NULL;
    if (!root) { CHECK(0, "bimanual parses"); free(text); return; }
    dsg_schema_object(root, "/trials/reach/states/2/exits/0", &o);
    k = dsg_schema_candidates(root, &o, dsg_schema_field(o.section, o.kind_bit, "to"), buf, sizeof buf);
    CHECK(k == 4 && listed(buf, k, "begin") && listed(buf, k, "hold_start") && listed(buf, k, "go") && listed(buf, k, "end"),
          "an exit's to: the template's states and end (%d)", k);
    k = dsg_schema_candidates(root, &o, dsg_schema_field(o.section, o.kind_bit, "on"), buf, sizeof buf);
    CHECK(listed(buf, k, "at_start") && listed(buf, k, "arrived") && listed(buf, k, "response") && !listed(buf, k, "top_speed") &&
              !listed(buf, k, "cursor") && !listed(buf, k, "moved"),
          "an exit's on: controls with hits and response (%d)", k);
    dsg_schema_object(root, "/controls/cursor", &o);
    k = dsg_schema_candidates(root, &o, dsg_schema_field(o.section, o.kind_bit, "x"), buf, sizeof buf);
    CHECK(listed(buf, k, "left.x") && listed(buf, k, "right.y") && listed(buf, k, "left"), "a cursor's x: roles with lanes (%d)", k);
    dsg_schema_object(root, "/trials/reach/controls/path", &o);
    k = dsg_schema_candidates(root, &o, dsg_schema_field(o.section, o.kind_bit, "to"), buf, sizeof buf);
    CHECK(listed(buf, k, "arrived") && listed(buf, k, "go") && listed(buf, k, "reach_target") && listed(buf, k, "end") &&
              listed(buf, k, "response"),
          "a trace's to: stimuli, hits, states, response and end (%d)", k);
    dsg_schema_object(root, "/timeline/0/timeline/1", &o);
    k = dsg_schema_candidates(root, &o, dsg_schema_field(o.section, o.kind_bit, "trial"), buf, sizeof buf);
    CHECK(k == 2 && listed(buf, k, "track") && listed(buf, k, "reach"), "a node's trial: the templates (%d)", k);
    k = dsg_schema_candidates(root, &o, dsg_schema_field(o.section, o.kind_bit, "trial"), buf, 8);
    CHECK(k == 2 && strcmp(buf, "reach") == 0, "a short buffer keeps the names that fit and counts all");
    free(text);
}

/* --- 8. renames -------------------------------------------------------------------- */

typedef struct heard { int n; char last[256]; } heard;
static void on_rename(void* ctx, const char* scope, const char* from, const char* to) {
    heard* h = (heard*)ctx;
    h->n++;
    snprintf(h->last, sizeof h->last, "%s:%s>%s", scope, from, to);
}

static int rename_at(dsg_doc* d, const char* path, const char* to, int* changed) {
    dsg_obj o;
    char err[256] = "";
    int rc;
    if (dsg_schema_object(dsg_doc_root(d), path, &o)) return -2;
    rc = dsg_schema_rename(d, &o, to, changed, err, sizeof err);
    if (rc) printf("  (rename %s: %s)\n", path, err);
    return rc;
}

static int count(const char* t, const char* s) {
    int k = 0;
    const char* p = t;
    while ((p = strstr(p, s)) != NULL) { k++; p += strlen(s); }
    return k;
}

static void test_renames(dsg_checker* c) {
    static const char* const files[] = { "pack.json", "experiment.json", NULL };
    dsg_project p;
    dsg_doc* d = dsg_doc_new();
    heard h;
    int changed = 0, depth;
    char* before;
    size_t n;
    dsg_report r;
    memset(&h, 0, sizeof h);
    if (!d || open_doc("bimanual", files, &p, d)) { dsg_doc_free(d); return; }
    /* the canonical layout first, so undo compares whole bytes */
    dsg_def_set_string(d, "/name", "bimanual_tracking");
    if (!check_clean(c, &p, d, "bimanual")) { dsg_project_close(&p); dsg_doc_free(d); return; }
    before = (char*)malloc(strlen(text0(d)) + 1);
    if (!before) return;
    strcpy(before, text0(d));
    dsg_doc_listen(d, on_rename, &h);
    depth = dsg_doc_undo_depth(d);

    /* a state: its exits' to, a moving target's from, a trace's from */
    CHECK(rename_at(d, "/trials/track/states/2", "track_phase", &changed) == 0 && changed == 3, "rename state move (%d refs)", changed);
    CHECK(!has(d, "\"move\"") && count(text0(d), "\"track_phase\"") == 4, "the state and its 3 references");
    CHECK(dsg_doc_undo_depth(d) == depth + 1, "one undo step");
    CHECK(h.n == 1 && strcmp(h.last, "/trials/track/states:move>track_phase") == 0, "the chart hears it: '%s'", h.last);
    check_clean(c, &p, d, "after the state's rename");

    /* a stimulus: steps' on lists, a set step, a control's stimulus, a response's from, a control's from */
    CHECK(rename_at(d, "/stimuli/reach_target", "goal_disk", &changed) == 0 && changed >= 6, "rename stimulus reach_target (%d refs)",
          changed);
    CHECK(!has(d, "reach_target"), "no reach_target is left");
    check_clean(c, &p, d, "after the stimulus's rename");

    /* a top-level control: of, a trace's of list */
    CHECK(rename_at(d, "/controls/cursor", "pointer", &changed) == 0 && changed >= 7, "rename control cursor (%d refs)", changed);
    CHECK(!has(d, "\"cursor\"") || has(d, "\"control\": \"cursor\""), "every reference to cursor changed");
    CHECK(count(text0(d), "\"cursor\"") == 1, "only the control's kind is still cursor");
    check_clean(c, &p, d, "after the control's rename");

    /* a template control named in forms: when, signal, a column's time */
    CHECK(rename_at(d, "/trials/reach/controls/arrived", "landed", &changed) == 0 && changed >= 4,
          "rename control arrived (%d refs: exit on, when, column time, trace to)", changed);
    CHECK(!has(d, "\"arrived\""), "no arrived is left");
    check_clean(c, &p, d, "after the template control's rename");

    /* a signal role: lanes kept, a trace's of */
    CHECK(rename_at(d, "/roles/left", "hand_l", &changed) == 0 && changed == 2, "rename role left (%d refs)", changed);
    CHECK(has(d, "\"hand_l.x\"") && !has(d, "\"left") , "left.x is hand_l.x");
    check_clean(c, &p, d, "after the role's rename");

    /* a template: the nodes' trial */
    CHECK(rename_at(d, "/trials/reach", "reaching", &changed) == 0 && changed == 4, "rename template reach (%d nodes)", changed);
    check_clean(c, &p, d, "after the template's rename");

    /* a variable defined once: "$phase_x" in a moving target's phases */
    CHECK(rename_at(d, "/timeline/0/vars/phase_x", "phases_x", &changed) == 0 && changed == 1, "rename variable phase_x (%d refs)",
          changed);
    CHECK(has(d, "\"$phases_x\"") && !has(d, "phase_x"), "$phase_x is $phases_x");
    check_clean(c, &p, d, "after the variable's rename");

    /* a node: only its name */
    CHECK(rename_at(d, "/timeline/0/timeline/0", "first_track", &changed) == 0 && changed == 0 && has(d, "\"first_track\""),
          "rename a node");

    /* undo: every step back to the same bytes */
    {
        int k, steps = dsg_doc_undo_depth(d) - depth;
        for (k = 0; k < steps; k++) dsg_doc_undo(d);
    }
    CHECK(strcmp(text0(d), before) == 0, "undo gives the bytes back");
    CHECK(strcmp(h.last, "/trials/track/states:track_phase>move") == 0, "and the chart hears the undo: '%s'", h.last);

    /* a reference the name types cannot find: walk2d's $aim_x; the check names it */
    CHECK(rename_at(d, "/timeline/0/vars/aim", "target", &changed) == 0 && changed == 0, "rename walk2d variable aim (%d refs)", changed);
    {
        const char* t = dsg_doc_bytes(d, DSG_BUF_DEF, &n);
        CHECK(dsg_check(c, &p, t, n, &r) == 0 && r.n_errors > 0 && issue_with(&r, "aim", NULL),
              "the check reports the $aim_x it missed (%d errors)", r.n_errors);
    }
    dsg_doc_undo(d);
    /* a variable that other nodes define too: its references are theirs as well */
    CHECK(rename_at(d, "/timeline/0/timeline/0/vars/perturb", "perturb0", &changed) == 0 && changed == 0 && has(d, "\"$perturb\""),
          "rename a variable that 9 other nodes define: no reference changes (%d)", changed);
    dsg_doc_undo(d);
    /* refused */
    CHECK(rename_at(d, "/stimuli/dot", "track_target", &changed) == -1 && strcmp(text0(d), before) == 0,
          "a name the set has: refused, no change");
    CHECK(rename_at(d, "/stimuli/dot", "", &changed) == -1, "an empty name: refused");
    CHECK(rename_at(d, "/trials/reach/states/0/exits/0", "x", &changed) == -1, "an exit has no name");
    free(before);
    dsg_project_close(&p);
    dsg_doc_free(d);

    /* a state's name is its template's: the other template's state of that
     * name and its exits stay */
    {
        static const char two[] =
            "{\"format\": \"ysp-experiment\", \"version\": 2,\n"
            " \"trials\": {\"t1\": {\"states\": [{\"name\": \"a\", \"exits\": [{\"after_s\": 1, \"to\": \"a\"}]}]},\n"
            "            \"t2\": {\"states\": [{\"name\": \"a\", \"exits\": [{\"after_s\": 1, \"to\": \"a\"}]}]}},\n"
            " \"timeline\": [{\"name\": \"n\", \"trial\": \"t1\"}]}\n";
        dsg_doc* d2 = dsg_doc_new();
        if (d2 && dsg_doc_reset(d2, "e.json", two, strlen(two)) == 0) {
            CHECK(rename_at(d2, "/trials/t2/states/0", "b", &changed) == 0 && changed == 1, "rename t2's state a (%d refs)", changed);
            CHECK(count(text0(d2), "\"a\"") == 2 && count(text0(d2), "\"b\"") == 2, "t1's a and its exit stay");
        }
        dsg_doc_free(d2);
    }
}

/* --- the bench ---------------------------------------------------------------------- */

static double now_us(void) {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (double)ts.tv_sec * 1e6 + (double)ts.tv_nsec / 1e3;
}

static int cmp_d(const void* a, const void* b) {
    double x = *(const double*)a, y = *(const double*)b;
    return x < y ? -1 : x > y;
}

static void report(const char* what, double* t, int n) {
    qsort(t, (size_t)n, sizeof *t, cmp_d);
    printf("  %-52s median %9.1f  p99 %9.1f us\n", what, t[n / 2], t[(n * 99) / 100]);
}

/* bimanual with its templates copied under new names to about `target` bytes */
static char* scaled(const char* src, size_t n, size_t target, size_t* out_n) {
    yjs_arena a;
    yjs_value* root = NULL;
    yjs_error e;
    yjs_value* trials;
    char* out = NULL;
    size_t len = n;
    int k;
    yjs_arena_init_heap(&a, 0);
    if (yjs_parse(&a, src, n, NULL, &root, &e) != YJS_OK || !(trials = yjs_get(root, "trials"))) goto done;
    {
        uint32_t base = trials->n, i;
        for (k = 1; len < target && k < 100000; k++) {
            for (i = 0; i < base; i++) {
                char nm[160];
                snprintf(nm, sizeof nm, "%s_%d", trials->u.members[i].key, k);
                yjs_set(&a, trials, nm, yjs_copy(&a, trials->u.members[i].value));
            }
            len = yjs_write_mem(root, YJS_WRITE_PRETTY, NULL, 0);
        }
    }
    out = (char*)malloc(len + 2);
    if (out) {
        yjs_write_mem(root, YJS_WRITE_PRETTY, out, len + 1);
        out[len++] = '\n';
        out[len] = 0;
        *out_n = len;
    }
done:
    yjs_arena_free(&a);
    return out;
}

/* What the panels do each frame for a selection: the chain, each object's
 * fields, and each field's value. */
static void frame_work(const yjs_value* root, const char* path) {
    dsg_obj c[4];
    const dsg_field* sel[256];
    char kp[DSG_SEL_PATH];
    int n = dsg_schema_chain(root, path, c, 4), i, k, j;
    for (i = 0; i < n; i++) {
        k = dsg_schema_select(c[i].section, c[i].kind_bit, sel, 256);
        for (j = 0; j < k && j < 256; j++)
            if (dsg_schema_key_path(&c[i], sel[j]->key, kp, sizeof kp) == 0) (void)dsg_path_get(root, kp);
    }
}

static void bench_one(const char* name, const char* bytes, size_t n, int rounds) {
    dsg_doc* d = dsg_doc_new();
    double* t = (double*)malloc((size_t)rounds * sizeof *t);
    const yjs_value* root;
    char what[128], buf[4096];
    dsg_obj o;
    int i, changed = 0;
    if (!d || !t || dsg_doc_reset(d, "experiment.json", bytes, n)) return;
    dsg_def_set_string(d, "/name", "bench");   /* the canonical layout first */
    root = dsg_doc_root(d);
    printf("%s, %zu bytes\n", name, n);
    for (i = 0; i < rounds; i++) {
        double t0 = now_us();
        frame_work(root, "/trials/reach/states/2/exits/0/to");
        t[i] = now_us() - t0;
    }
    report("a frame's schema work: an exit, its state, its template", t, rounds);
    for (i = 0; i < rounds; i++) {
        double t0 = now_us();
        frame_work(root, "/stimuli/dot/w");
        t[i] = now_us() - t0;
    }
    report("a frame's schema work: a stimulus", t, rounds);
    dsg_schema_object(root, "/trials/reach/states/2/exits/0", &o);
    for (i = 0; i < rounds; i++) {
        double t0 = now_us();
        dsg_schema_candidates(root, &o, dsg_schema_field(o.section, o.kind_bit, "on"), buf, sizeof buf);
        t[i] = now_us() - t0;
    }
    report("the candidates of an exit's on (a combo opens)", t, rounds);
    for (i = 0; i < rounds; i++) {
        double t0;
        char err[256];
        dsg_schema_object(dsg_doc_root(d), (i & 1) ? "/stimuli/spot" : "/stimuli/dot", &o);
        t0 = now_us();
        dsg_schema_rename(d, &o, (i & 1) ? "dot" : "spot", &changed, err, sizeof err);
        t[i] = now_us() - t0;
    }
    snprintf(what, sizeof what, "rename a stimulus (%d references, whole file)", changed);
    report(what, t, rounds);
    for (i = 0; i < rounds; i++) {
        double t0;
        char err[256];
        dsg_schema_object(dsg_doc_root(d), "/trials/track/states/2", &o);
        t0 = now_us();
        dsg_schema_rename(d, &o, (i & 1) ? "move" : "track_phase", &changed, err, sizeof err);
        t[i] = now_us() - t0;
    }
    snprintf(what, sizeof what, "rename a state (%d references, its template)", changed);
    report(what, t, rounds);
    free(t);
    dsg_doc_free(d);
}

static int bench(void) {
    char path[DSG_PATH_MAX];
    size_t n = 0, m = 0;
    char* src;
    char* big;
    snprintf(path, sizeof path, "%s/tests/player/bimanual/experiment.json", YSP_SOURCE_DIR);
    src = read_all(path, &n);
    if (!src) return 1;
    bench_one("bimanual", src, n, 2000);
    big = scaled(src, n, 100000, &m);
    if (big) bench_one("bimanual, templates copied to 100 KB", big, m, 200);
    free(big);
    big = scaled(src, n, 1000000, &m);
    if (big) bench_one("bimanual, templates copied to 1 MB", big, m, 20);
    free(big);
    free(src);
    return 0;
}

int main(int argc, char** argv) {
    dsg_checker* c;
    char err[512];
    if (argc > 1) work = argv[1];
    if (argc > 2 && strcmp(argv[2], "--bench") == 0) {
        yjs_arena_init_heap(&g_arena, (size_t)64 << 20);
        return bench();
    }
    if (dsg_make_dir(work, err, sizeof err)) {
        printf("FAIL: the work folder: %s\n", err);
        return 1;
    }
    yjs_arena_init_heap(&g_arena, (size_t)64 << 20);
    c = dsg_checker_new();
    test_schema();
    test_headers();
    test_objects();
    test_cover();
    test_writes();
    test_candidates();
    if (c) {
        test_range(c);
        test_renames(c);
    } else {
        CHECK(0, "a checker");
    }
    dsg_checker_free(c);
    yjs_arena_free(&g_arena);
    printf("%s: %d checks, %d failed\n", fails ? "FAIL" : "PASS", checks, fails);
    return fails ? 1 : 0;
}
