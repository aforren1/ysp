/* check_test.c - pl_check(): the player's load with no side effects, as the
 * designer runs it on each change (docs/player.md, "Reference: pl_check()").
 *
 * Usage: test_player_check NAME=PACK... [--time N]
 *   NAME is one of tests/player/compare.py's examples (rdk, stroop, afc2,
 *   oddball, visuomotor, bimanual) and PACK its built pack;
 *   tests/player/check_test.py builds them. rdk and oddball are required.
 *   With no NAME=PACK and YPAK set (the mutation runner,
 *   tests/mutate/player_lower.toml), it builds them under a temporary folder.
 * --time N: N calls of pl_check() on each pack, the median and the maximum
 *   wall time (an order of magnitude, not a timing claim).
 *
 * Built with PL_CHECK_COUNT (GNU ld's --wrap of malloc, calloc, realloc and
 * free), it also counts the heap blocks that stay live over repeated calls.
 * Built with PL_CHECK_MUT, the player's sources are compiled in. */
#if !defined(_WIN32) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE 1   /* mkdtemp(), setenv(): before any system header */
#endif
#if defined(PL_CHECK_MUT)
#include "impl.c"
#include "eval.c"
#include "host.c"
#include "lower.c"
#include "run.c"
#include "v2.c"
#else
#include "player.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if !defined(_WIN32)
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#else
#include <direct.h>
#include <process.h>
#define getpid _getpid
#include <io.h>
#endif

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#if defined(PL_CHECK_COUNT)
/* Live heap blocks: every call that gives a block, less every free() of one.
 * realloc() of a block to a new size keeps the count. */
static long long live;
void* __real_malloc(size_t n);
void* __real_calloc(size_t a, size_t b);
void* __real_realloc(void* q, size_t n);
void  __real_free(void* q);
void* __wrap_malloc(size_t n) { void* r = __real_malloc(n); if (r) live++; return r; }
void* __wrap_calloc(size_t a, size_t b) { void* r = __real_calloc(a, b); if (r) live++; return r; }
void* __wrap_realloc(void* q, size_t n) {
    void* r = __real_realloc(q, n);
    if (!q && r) live++;
    else if (q && !n) live--;
    return r;
}
void __wrap_free(void* q) { if (q) live--; __real_free(q); }
#endif

typedef struct pk { const char* name; void* data; size_t size; } pk;
static pk packs[8];
static int n_packs;
static pl_player P;
static ycol_cal nominal;          /* pl_check_desc.cal, made once */
static int given_cal;

static pk* pack_of(const char* name) {
    int i;
    for (i = 0; i < n_packs; i++) if (strcmp(packs[i].name, name) == 0) return &packs[i];
    return NULL;
}

static int read_file(const char* path, void** data, size_t* size) {
    FILE* fp = fopen(path, "rb");
    long n;
    if (!fp) return -1;
    fseek(fp, 0, SEEK_END);
    n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    *data = malloc((size_t)n + 1);   /* malloc's alignment: the pack wants 8 */
    if (!*data || fread(*data, 1, (size_t)n, fp) != (size_t)n) { fclose(fp); return -1; }
    fclose(fp);
    *size = (size_t)n;
    return 0;
}

/* The definition entry of a pack, as text the cases edit. */
static char* def_of(const pk* k, size_t* n) {
    ypak_pack pp;
    ypak_desc d;
    ypak_entry e;
    const void* data;
    char* out = NULL;
    memset(&d, 0, sizeof d);
    d.data = k->data;
    d.size = k->size;
    if (!ypak_open(&pp, &d)) return NULL;
    if (ypak_first_of(&pp, YPAK_KIND_EXPERIMENT, &e) == 0 && (data = ypak_data(&pp, &e)) != NULL) {
        out = (char*)malloc((size_t)e.size + 1);
        memcpy(out, data, (size_t)e.size);
        out[e.size] = 0;
        *n = (size_t)e.size;
    }
    ypak_close(&pp);
    return out;
}

/* text with the first `from` replaced by `to`; *line is the line it is on. */
static char* edit(const char* text, const char* from, const char* to, unsigned* line) {
    const char* at = strstr(text, from);
    size_t a, nf = strlen(from), nt = strlen(to), n = strlen(text);
    char* out;
    const char* c;
    if (!at) { printf("edit: no '%s'\n", from); return NULL; }
    a = (size_t)(at - text);
    out = (char*)malloc(n - nf + nt + 1);
    memcpy(out, text, a);
    memcpy(out + a, to, nt);
    memcpy(out + a + nt, at + nf, n - a - nf + 1);
    if (line) {
        *line = 1;
        for (c = text; c < at; c++) if (*c == '\n') (*line)++;
    }
    return out;
}

static const yrig_profile* check_rig;   /* pl_check_desc.rig of run_check() */

static int run_check(const pk* k, const char* def, pl_check_result* r) {
    pl_check_desc d;
    memset(&d, 0, sizeof d);
    d.rig = check_rig;
    d.pack.data = k->data;
    d.pack.size = k->size;
    d.def_text = def;
    d.def_len = def ? strlen(def) : 0;
    d.seed = 1;
    d.cal = given_cal ? &nominal : NULL;
    return pl_check(&P, &d, r);
}

static const char* const need_names[] = { "?", "field", "calibration", "view", "display_tier", "device", "role_tier", "signal", "sim_gain" };

/* The needs as "kind:name:value" lines, sorted, in buf. */
static void needs_text(const pl_check_result* r, char* buf, size_t cap) {
    char lines[64][128];
    int i, j, n = r->n_needs < 64 ? r->n_needs : 64;
    size_t used = 0;
    for (i = 0; i < n; i++) {
        const pl_need* q = &r->needs[i];
        snprintf(lines[i], sizeof lines[i], "%s:%s:%d", q->kind > 0 && q->kind <= 8 ? need_names[q->kind] : "?", q->name ? q->name : "", q->value);
    }
    for (i = 1; i < n; i++)
        for (j = i; j > 0 && strcmp(lines[j - 1], lines[j]) > 0; j--) {
            char t[128];
            memcpy(t, lines[j], sizeof t);
            memcpy(lines[j], lines[j - 1], sizeof t);
            memcpy(lines[j - 1], t, sizeof t);
        }
    buf[0] = 0;
    for (i = 0; i < n && used < cap; i++) used += (size_t)snprintf(buf + used, cap - used, "%s%s", i ? " " : "", lines[i]);
}

/* Each example loads, and lists what a real run of it needs. */
static void case_examples(void) {
    static const struct { const char* name; const char* needs; } want[] = {
        { "rdk", "field:participant:0" },
        { "stroop", "field:participant:0" },
        { "afc2", "calibration::0 display_tier::2 field:participant:0 view::1" },
        { "oddball", "device:trig:1 display_tier::1 field:participant:0 role_tier:resp:2 view::1" },
        { "visuomotor", "field:participant:0 signal:hand:1 sim_gain:hand:0" },
        { "bimanual", "field:participant:0 signal:left:1 signal:right:1 sim_gain:left:0 sim_gain:right:0 view::2" },
    };
    size_t i;
    for (i = 0; i < sizeof want / sizeof want[0]; i++) {
        pk* k = pack_of(want[i].name);
        pl_check_result r;
        char got[2048];
        int rc;
        memset(&r, 0, sizeof r);
        if (!k) { printf("  %s: no pack, skipped\n", want[i].name); continue; }
        rc = run_check(k, NULL, &r);
        CHECK(rc == PL_CHECK_OK && r.status == PL_CHECK_OK, "%s: status %d: %s", want[i].name, r.status, r.message);
        CHECK(r.report && r.report->n_errors == 0, "%s: the report has errors", want[i].name);
        needs_text(&r, got, sizeof got);
        CHECK(strcmp(got, want[i].needs) == 0, "%s: needs '%s', want '%s'", want[i].name, got, want[i].needs);
        CHECK(!P.scr_open && !P.ds_open && !P.writer_open && !P.ring_open, "%s: a screen, a data set, a writer or a device ring opened", want[i].name);
        /* no rig: the loader's checks against one are skipped, and say so */
        CHECK(r.report && (P.def->n_roles == 0 || (r.report->skipped & YEXP_CHECK_ROLES)) &&
              (P.def->n_sigroles == 0 || (r.report->skipped & YEXP_CHECK_SIGROLES)), "%s: skipped 0x%x", want[i].name, r.report ? r.report->skipped : 0u);
        printf("  %s: ok, needs %s\n", want[i].name, got);
        pl_close(&P);
    }
}

/* The threads of the process (Linux), or -1. */
static int threads(void) {
#if defined(__linux__)
    DIR* d = opendir("/proc/self/task");
    struct dirent* e;
    int n = 0;
    if (!d) return -1;
    while ((e = readdir(d)) != NULL) if (e->d_name[0] != '.') n++;
    closedir(d);
    return n;
#else
    return -1;
#endif
}

/* A sound stimulus: its entry loads through an audio device that never
 * runs, so there is no audio thread. */
static void case_sound(void) {
    pk* k = pack_of("sound");
    pl_check_result r;
    int t0 = threads();
    memset(&r, 0, sizeof r);
    if (!k) { printf("  sound: no pack, skipped\n"); return; }
    CHECK(run_check(k, NULL, &r) == PL_CHECK_OK, "sound: status %d: %s", r.status, r.message);
    {
        const pl_stim* t = r.status == PL_CHECK_OK ? pl_stim_by_name(&P, "tone") : NULL;
        CHECK(P.audio_open && t && t->sound.frames && t->sound.n == 4800, "sound: the entry did not load");
    }
    CHECK(threads() == t0, "sound: %d threads, %d before", threads(), t0);
    printf("  sound: ok, %d threads during the check\n", threads());
    pl_close(&P);
}

/* The same pack through a range reader: entries read whole, the same
 * answer. */
static int64_t mem_read(void* ctx, int64_t off, void* buf, int64_t n) {
    const pk* k = (const pk*)ctx;
    if (off < 0 || off > (int64_t)k->size) return -1;
    if (n > (int64_t)k->size - off) n = (int64_t)k->size - off;
    memcpy(buf, (const char*)k->data + off, (size_t)n);
    return n;
}

static void case_reader(const char* name) {
    pk* k = pack_of(name);
    pl_check_desc d;
    pl_check_result r;
    ypak_reader rd;
    char a[2048], b[2048], id[65];
    memset(&r, 0, sizeof r);
    if (!k) return;
    (void)run_check(k, NULL, &r);
    needs_text(&r, a, sizeof a);
    memcpy(id, P.experiment_id, sizeof id);
    pl_close(&P);
    memset(&d, 0, sizeof d);
    rd.read = mem_read;
    rd.size = (int64_t)k->size;
    d.pack.reader = &rd;
    d.pack.reader_ctx = k;
    d.seed = 1;
    CHECK(pl_check(&P, &d, &r) == PL_CHECK_OK, "%s through a reader: %s", name, r.message);
    needs_text(&r, b, sizeof b);
    CHECK(strcmp(a, b) == 0 && strcmp(id, P.experiment_id) == 0, "%s through a reader: needs '%s' and '%s', ids %s and %s", name, a, b, id, P.experiment_id);
    pl_close(&P);
}

/* Every yexp error, with its line and column. */
static void case_definition(void) {
    pk* k = pack_of("rdk");
    size_t n;
    char* def, *e1, *e2;
    unsigned l1 = 0, l2 = 0;  /* MSVC C4701: set only when e1 is */
    pl_check_result r;
    int i, at1 = 0, at2 = 0;
    memset(&r, 0, sizeof r);
    if (!k || !(def = def_of(k, &n))) { CHECK(0, "the rdk pack"); return; }
    e1 = edit(def, "\"shape\": \"cross\"", "\"shape\": \"crossx\"", &l1);
    e2 = e1 ? edit(e1, "\"dot_size\": 4", "\"dot_sizes\": 4", &l2) : NULL;
    if (!e2) { CHECK(0, "the edits"); free(def); free(e1); return; }
    CHECK(run_check(k, e2, &r) == PL_CHECK_DEFINITION, "two errors: status %d: %s", r.status, r.message);
    CHECK(r.report && r.report->n_errors == 2, "two errors: %d", r.report ? r.report->n_errors : -1);
    for (i = 0; r.report && i < r.report->n_diags; i++) {
        const yexp_diag* g = &r.report->diags[i];
        if (g->severity != YEXP_ERROR) continue;
        if (g->line == l1 && g->column > 0 && strstr(g->msg, "crossx")) at1 = 1;
        if (g->line == l2 && g->column > 0 && strstr(g->msg, "dot_sizes")) at2 = 1;
        printf("  diag %u:%u: %s\n", g->line, g->column, g->msg);
    }
    CHECK(at1 && at2, "the diags at lines %u and %u", l1, l2);
    CHECK(r.object == PL_OBJ_NONE && r.n_needs == 0, "a definition error names no object and no needs");
    pl_close(&P);
    free(def); free(e1); free(e2);
}

/* The player's refusals after the load, with the object they name. */
static void case_refusals(void) {
    pk* k = pack_of("rdk");
    size_t n;
    char* def, *e;
    pl_check_result r;
    memset(&r, 0, sizeof r);
    if (!k || !(def = def_of(k, &n))) { CHECK(0, "the rdk pack"); return; }
    /* a node: one name, two values */
    e = edit(def, "\"trial\": \"rdk\"\n", "\"trial\": \"rdk\", \"vars\": { \"x\": 1 }, \"data\": { \"x\": 2 }\n", NULL);
    CHECK(e && run_check(k, e, &r) == PL_CHECK_REFUSED, "vars and data: status %d: %s", r.status, r.message);
    CHECK(r.object == PL_OBJ_NODE && r.object_name && strcmp(r.object_name, "main") == 0 && strstr(r.message, "both a 'vars' name"),
          "vars and data: object %d '%s': %s", r.object, r.object_name ? r.object_name : "", r.message);
    CHECK(r.report && r.report->n_errors == 0, "a refusal after the load has no yexp error");
    printf("  refused (%d %s): %s\n", r.object, r.object_name ? r.object_name : "", r.message);
    pl_close(&P);
    free(e);
    /* a string participant: the load ended the process (0.13.2, pl_fields_init
     * read p->leaves before they were set); now it loads, and refuses
     * '@participant' in a node's rules by the node */
    e = edit(def, "\"devices\":", "\"session\": { \"fields\": { \"participant\": { \"type\": \"string\", \"required\": true } } },\n  \"devices\":", NULL);
    CHECK(e && run_check(k, e, &r) == PL_CHECK_OK, "a string participant: status %d: %s", r.status, r.message ? r.message : "");
    pl_close(&P);
    {
        char* e2 = e ? edit(e, "\"reps 5\"]", "\"reps 5\", \"where coherence=@participant\"]", NULL) : NULL;
        CHECK(e2 && run_check(k, e2, &r) != PL_CHECK_OK && r.message && strstr(r.message, "@participant"),
              "a string participant and @participant: status %d: %s", r.status, r.message ? r.message : "");
        if (r.status == PL_CHECK_REFUSED)
            CHECK(r.object == PL_OBJ_NODE && r.object_name && strcmp(r.object_name, "main") == 0, "@participant: the node, object %d", r.object);
        printf("  string participant and @participant (%d): %s\n", r.status, r.message ? r.message : "");
        pl_close(&P);
        free(e2);
    }
    free(e);
    /* a stimulus: an rdk's count must be a literal */
    e = edit(def, "\"count\": 200", "\"count\": \"$coherent_direction\"", NULL);
    CHECK(e && run_check(k, e, &r) == PL_CHECK_REFUSED, "rdk count: status %d: %s", r.status, r.message);
    CHECK(r.object == PL_OBJ_STIMULUS && r.object_name && strcmp(r.object_name, "dots") == 0, "rdk count: object %d '%s': %s", r.object,
          r.object_name ? r.object_name : "", r.message);
    printf("  refused (%d %s): %s\n", r.object, r.object_name ? r.object_name : "", r.message);
    pl_close(&P);
    free(e);
    /* a template: more choices than ysp/response.h takes */
    {
        char choices[400] = "\"choices\": [";
        int c;
        for (c = 0; c < YRSP_MAX_CHOICES + 1; c++) {
            char one[16];
            snprintf(one, sizeof one, "%s\"%c\"", c ? ", " : "", c < 26 ? 'a' + c : '0' + (c - 26));
            strcat(choices, one);
        }
        strcat(choices, "]");
        e = edit(def, "\"choices\": [\"left\", \"right\"]", choices, NULL);
        CHECK(e && run_check(k, e, &r) == PL_CHECK_REFUSED, "%d choices: status %d: %s", YRSP_MAX_CHOICES + 1, r.status, r.message);
        CHECK(r.object == PL_OBJ_TEMPLATE && r.object_name && strcmp(r.object_name, "rdk") == 0 && strstr(r.message, "choices is more than"),
              "%d choices: object %d '%s': %s", YRSP_MAX_CHOICES + 1, r.object, r.object_name ? r.object_name : "", r.message);
        printf("  refused (%d %s): %s\n", r.object, r.object_name ? r.object_name : "", r.message);
        pl_close(&P);
        free(e);
    }
    free(def);
}

/* One case of case_run_only(): the edits on a pack's definition, and the
 * refusal a real run gives (NULL: none). */
typedef struct ro_case {
    const char* name, *pack;
    const char* from[3], *to[3];
    int status, object;
    const char* object_name, *message;
} ro_case;

/* Refusals of a real run that come only from the definition, the pack and
 * the check's stand-ins: pl_check() gives each, with the message the
 * command line prints (0.14.0 said OK to each; "rule, keyboard" is the
 * control). The pack's definition is ypak's canonical text: the edits
 * match it. */
#define TRIG_MAP "\"code\": {\n            \"by\": \"$response\",\n            \"map\": {\n              \"left\": 101,\n              \"right\": 102\n            }\n          }"
static void case_run_only(void) {
    static const ro_case cases[] = {
        /* a response trigger on a role a rig can bind to a device (xid):
         * the rule's code is fixed when the device opens (pl_rule_make()) */
        { "rule", "oddball", { TRIG_MAP }, { "\"code\": \"$code\"" }, PL_CHECK_REFUSED, PL_OBJ_TEMPLATE, "oddball",
          "template 'oddball': the response trigger's code on the device role 'resp' must be a number, or a lookup by $response of numbers: "
          "a rule's codes are set when the device opens" },
        /* the keyboard only: no rig binds it, the frame loop writes the code */
        { "rule, keyboard", "oddball", { TRIG_MAP, "\"family\": [\"xid\", \"keyboard\"]" }, { "\"code\": \"$code\"", "\"family\": [\"keyboard\"]" },
          PL_CHECK_OK, PL_OBJ_NONE, NULL, NULL },
        /* keyboard first, so --sim takes keys; a device bound to it takes
         * buttons, and the role has none (pl_tmpl_open()) */
        { "buttons", "oddball",
          { "\"family\": [\"xid\", \"keyboard\"]", "\"buttons\": {\n        \"1\": \"left\",\n        \"2\": \"right\"\n      },",
            ",\n        \"trigger\": {\n          " TRIG_MAP ",\n          \"role\": \"trig\"\n        }" },
          { "\"family\": [\"keyboard\", \"xid\"]", "", "" }, PL_CHECK_REFUSED, PL_OBJ_TEMPLATE, "oddball",
          "template 'oddball': choice 'left' is not one of role 'resp''s buttons" },
        /* streams/<name>.csv: two rdk stimuli whose names differ in case */
        { "rdk streams", "rdk", { "\"dots\": {" },
          { "\"Dots\": { \"kind\": \"rdk\", \"w\": 100, \"count\": 20, \"speed\": 200, \"dot_size\": 4 },\n    \"dots\": {" },
          PL_CHECK_REFUSED, PL_OBJ_STIMULUS, "dots", "the stream 'dots': ysp_dataset: yds_stream: 'dots' is taken by 'Dots' (names differ in more than case)" },
        /* two traces whose names differ in case */
        { "trace streams", "visuomotor", { "\"path\": {" },
          { "\"Path\": { \"control\": \"trace\", \"of\": [\"hand\"] },\n        \"path\": {" },
          PL_CHECK_REFUSED, PL_OBJ_TEMPLATE, "reach", "the stream 'path': ysp_dataset: yds_stream: 'path' is taken by 'Path' (names differ in more than case)" },
        /* a trace's columns: hand_x is the role's lane and a control */
        { "trace columns", "visuomotor", { "\"of\": [\"hand\", \"cursor\"]", "\"aid\": {" },
          { "\"of\": [\"hand\", \"hand_x\"]", "\"hand_x\": { \"control\": \"distance\", \"of\": \"hand\" },\n        \"aid\": {" }, PL_CHECK_REFUSED,
          PL_OBJ_TEMPLATE, "reach", "the stream 'path': ysp_dataset: yds_stream: ysp_trials: ytr_data_open: two columns are named 'hand_x'" },
    };
    size_t c;
    for (c = 0; c < sizeof cases / sizeof cases[0]; c++) {
        const ro_case* k = &cases[c];
        pk* pkk = pack_of(k->pack);
        size_t n;
        char* def, *e = NULL;
        int j;
        pl_check_result r;
        memset(&r, 0, sizeof r);
        if (!pkk) { printf("  %s: no pack, skipped\n", k->name); continue; }
        if (!(def = def_of(pkk, &n))) { CHECK(0, "%s: the pack's definition", k->name); continue; }
        for (j = 0; j < 3 && k->from[j]; j++) {
            char* e2 = edit(e ? e : def, k->from[j], k->to[j], NULL);
            free(e);
            e = e2;
            if (!e) break;
        }
        CHECK(e != NULL, "%s: the edits", k->name);
        if (!e) { free(def); continue; }
        (void)run_check(pkk, e, &r);
        CHECK(r.status == k->status, "%s: status %d, want %d: %s", k->name, r.status, k->status, r.message ? r.message : "");
        CHECK(r.object == k->object && (k->object_name ? r.object_name && strcmp(r.object_name, k->object_name) == 0 : !r.object_name),
              "%s: object %d '%s'", k->name, r.object, r.object_name ? r.object_name : "");
        if (k->message) CHECK(r.message && strcmp(r.message, k->message) == 0, "%s: message '%s'", k->name, r.message ? r.message : "");
        printf("  %s (%d %d %s): %s\n", k->name, r.status, r.object, r.object_name ? r.object_name : "", r.message ? r.message : "");
        pl_close(&P);
        free(e);
        free(def);
    }
    {
        /* 17 rdk stimuli: one stream more than ysp/dataset.h holds */
        pk* pkk = pack_of("rdk");
        size_t n;
        char* def = pkk ? def_of(pkk, &n) : NULL, *e;
        char more[2048] = "";
        pl_check_result r;
        int j;
        memset(&r, 0, sizeof r);
        for (j = 0; j < YDS_MAX_STREAMS; j++) {
            char one[128];
            snprintf(one, sizeof one, "\"r%02d\": { \"kind\": \"rdk\", \"w\": 100, \"count\": 20, \"speed\": 200, \"dot_size\": 4 },\n    ", j);
            strcat(more, one);
        }
        strcat(more, "\"dots\": {");
        e = def ? edit(def, "\"dots\": {", more, NULL) : NULL;
        CHECK(e && run_check(pkk, e, &r) == PL_CHECK_REFUSED && strstr(r.message, "yds_stream: 16 streams already (YDS_MAX_STREAMS)"),
              "17 rdk streams: status %d: %s", r.status, r.message ? r.message : "");
        printf("  17 rdk streams (%d): %s\n", r.status, r.message ? r.message : "");
        if (e) pl_close(&P);
        free(e);
        free(def);
    }
}

/* Entries a stimulus or a node names: missing, or of another kind. */
static void case_entries(void) {
    pk* k = pack_of("oddball");
    size_t n;
    char* def, *e;
    unsigned line;
    pl_check_result r;
    int i, found;
    memset(&r, 0, sizeof r);
    if (!k || !(def = def_of(k, &n))) { CHECK(0, "the oddball pack"); return; }
    {
        static const struct { const char* from, *to, *word; } cases[] = {
            { "\"texture\": \"$image\"", "\"texture\": \"nosuch.ysptex\"", "nosuch.ysptex" },
            { "\"texture\": \"$image\"", "\"texture\": \"oddball.pstb\"", "oddball.pstb" },
            { "\"timeline_variables\": \"oddball.pstb\"", "\"timeline_variables\": \"nosuch.pstb\"", "nosuch.pstb" },
        };
        size_t c;
        for (c = 0; c < sizeof cases / sizeof cases[0]; c++) {
            e = edit(def, cases[c].from, cases[c].to, &line);
            CHECK(e && run_check(k, e, &r) == PL_CHECK_DEFINITION, "%s: status %d: %s", cases[c].word, r.status, r.message);
            found = 0;
            /* at the key, or at the node that names a table */
            for (i = 0; r.report && i < r.report->n_diags; i++)
                if (r.report->diags[i].line > 0 && r.report->diags[i].line <= line && r.report->diags[i].column > 0 &&
                    strstr(r.report->diags[i].msg, cases[c].word)) {
                    found = 1;
                    printf("  diag %u:%u: %s\n", r.report->diags[i].line, r.report->diags[i].column, r.report->diags[i].msg);
                }
            CHECK(found, "%s: no diag at line %u: %s", cases[c].word, line, r.message);
            pl_close(&P);
            free(e);
        }
    }
    /* a texture through a node's variable: the loader follows its levels to
     * the pack */
    {
        char* e1 = edit(def, "\"texture\": \"$image\"", "\"texture\": \"$img2\"", NULL);
        e = e1 ? edit(e1, "\"trial\": \"oddball\"\n", "\"trial\": \"oddball\", \"vars\": { \"img2\": \"nosuch.ysptex\" }\n", NULL) : NULL;
        free(e1);
    }
    CHECK(e && run_check(k, e, &r) == PL_CHECK_DEFINITION, "a variable's missing texture: status %d: %s", r.status, r.message);
    found = 0;
    for (i = 0; r.report && i < r.report->n_diags; i++)
        if (r.report->diags[i].line > 0 && strstr(r.report->diags[i].msg, "nosuch.ysptex")) found = 1;
    CHECK(found, "a variable's missing texture: %s", r.message);
    pl_close(&P);
    free(e);
    free(def);
}

/* afc2 needs a calibration: one given in d.cal, or the one made on the
 * call, give one answer. */
static void case_given_cal(void) {
    pk* k = pack_of("afc2");
    pl_check_result r;
    char a[2048], b[2048];
    memset(&r, 0, sizeof r);
    if (!k) return;
    given_cal = 0;
    CHECK(run_check(k, NULL, &r) == PL_CHECK_OK && P.have_cal, "afc2, the calibration made: %s", r.message);
    needs_text(&r, a, sizeof a);
    CHECK(memcmp(&P.cal, &nominal, sizeof nominal) == 0, "afc2: the calibration made is not pl_check_nominal_cal()'s");
    pl_close(&P);
    given_cal = 1;
    CHECK(run_check(k, NULL, &r) == PL_CHECK_OK && P.have_cal, "afc2, the calibration given: %s", r.message);
    needs_text(&r, b, sizeof b);
    CHECK(strcmp(a, b) == 0 && memcmp(&P.cal, &nominal, sizeof nominal) == 0, "afc2: the given calibration gives '%s', the made one '%s'", b, a);
    pl_close(&P);
    given_cal = 0;
}

/* --- 0.18.0: pl_check_desc.rig ------------------------------------------- */

static char rig_dir[1024];   /* made before the cases' folders; removed at the end */

static int write_bytes(const char* path, const void* data, size_t n) {
    FILE* fp = fopen(path, "wb");
    if (!fp) return -1;
    if (fwrite(data, 1, n, fp) != n) { fclose(fp); return -1; }
    return fclose(fp) == 0 ? 0 : -1;
}

static void sha_hex(const void* data, size_t n, char hex[65]) {
    static const char H[] = "0123456789abcdef";
    uint8_t d[32];
    int i;
    yrig_sha256(data, n, d);
    for (i = 0; i < 32; i++) { hex[i * 2] = H[d[i] >> 4]; hex[i * 2 + 1] = H[d[i] & 15]; }
    hex[64] = 0;
}

static const char BOUNDS[] = "# a test fixture, not a measurement\nformat ysp-in-bounds 1\ndate 2026-11-22T00:00:00Z\nin_family xid\n"
                             "n 200\nmissed 0\nlo_s -0.000100000\nmedian_s 0.000050000\nhi_s 0.000400000\n";
static char bounds_sha[65], cal_sha[65];
static char rig_path_bounds[1200], rig_path_cal[1200], rig_path_lb[1200];

/* The rig folder of player_make_rig (its view and its nominal calibration),
 * with a bounds loopback for an xid role, so that role is tier 1. */
static int rig_files(void) {
    static unsigned char bytes[sizeof(ycol_cal)];
    int n = ycol_cal_save(&nominal, bytes, sizeof bytes);
    if (n < 0) return -1;
    snprintf(rig_path_cal, sizeof rig_path_cal, "%s/display.yspcal", rig_dir);
    snprintf(rig_path_lb, sizeof rig_path_lb, "%s/loopback", rig_dir);
    sha_hex(BOUNDS, strlen(BOUNDS), bounds_sha);
    sha_hex(bytes, (size_t)n, cal_sha);
    snprintf(rig_path_bounds, sizeof rig_path_bounds, "%s/loopback/%s.txt", rig_dir, bounds_sha);
#if !defined(_WIN32)
    if (mkdir(rig_path_lb, 0700) != 0) return -1;
#else
    if (_mkdir(rig_path_lb) != 0) return -1;
#endif
    if (write_bytes(rig_path_cal, bytes, (size_t)n) != 0 || write_bytes(rig_path_bounds, BOUNDS, strlen(BOUNDS)) != 0) return -1;
    return 0;
}

static void rig_files_remove(void) {
    remove(rig_path_bounds);
    remove(rig_path_cal);
#if !defined(_WIN32)
    rmdir(rig_path_lb);
    rmdir(rig_dir);
#else
    _rmdir(rig_path_lb);
    _rmdir(rig_dir);
#endif
}

/* A profile from its parts, checked against the rig folder's files. */
static int rig_make(yrig_profile* out, const char* roles, const char* display, const char* view, const char* signals) {
    char text[4096], err[300];
    int rc;
    snprintf(text, sizeof text, "{\"format\": \"ysp-rig 1\", \"rig\": \"check\", \"written\": \"2026-11-22T00:00:00Z\", \"roles\": {%s}%s%s%s%s%s%s}",
             roles, display ? ", \"display\": " : "", display ? display : "", view ? ", \"view\": " : "", view ? view : "",
             signals ? ", \"signals\": " : "", signals ? signals : "");
    rc = yrig_parse(out, text, strlen(text), rig_dir, err, sizeof err);
    if (rc != YRIG_OK) printf("  the profile: %s\n%s\n", err, text);
    return rc;
}

/* status, errors, message and needs: what the designer shows of a check */
static void check_text(const pl_check_result* r, char* buf, size_t cap) {
    char needs[1024];
    needs_text(r, needs, sizeof needs);
    snprintf(buf, cap, "%d|%d|%d|%s|%s", r->status, r->report ? r->report->n_errors : -1, r->report ? r->report->n_warnings : -1,
             r->message ? r->message : "", needs);
}

/* The first error diagnostic's text, or "". */
static const char* first_error(const pl_check_result* r) {
    int i;
    for (i = 0; r->report && i < r->report->n_diags; i++)
        if (r->report->diags[i].severity == YEXP_ERROR) return r->report->diags[i].msg;
    return "";
}

static void case_rig(void) {
    static yrig_profile full, room, part, wrong, changed, units;
    static yrig_profile keep;
    char roles[1024], display[256], signals[1024];
    pl_check_result r;
    char a[2048], b[2048];
    size_t n, i;
    memset(&r, 0, sizeof r);
    if (rig_files() != 0) { CHECK(0, "the rig folder %s", rig_dir); return; }
    snprintf(roles, sizeof roles,
             "\"resp\": {\"family\": \"xid\", \"key\": \"port:/dev/null\", \"bounds\": {\"n\": 200, \"lo_s\": -0.0001, \"hi_s\": 0.0004, "
             "\"date\": \"2026-11-22T00:00:00Z\", \"sha256\": \"%s\"}}, \"trig\": {\"family\": \"triggerbox\", \"key\": \"port:/dev/null\"}",
             bounds_sha);
    snprintf(display, sizeof display, "{\"onset_offset_s\": 0, \"calibration\": {\"file\": \"display.yspcal\", \"sha256\": \"%s\"}}", cal_sha);
    snprintf(signals, sizeof signals,
             "{\"hand\": {\"adapter\": \"raw_mouse\", \"units\": \"display_px\", \"per_count\": 1, \"device\": \"click\"}, "
             "\"left\": {\"adapter\": \"raw_mouse\", \"units\": \"display_cm\", \"per_count\": 0.01, \"device\": \"click\"}, "
             "\"right\": {\"adapter\": \"raw_mouse\", \"units\": \"display_cm\", \"per_count\": 0.01, \"device\": \"click\"}}");
#define VIEW "{\"distance_mm\": 570, \"width_mm\": 518.4, \"width_px\": 1920}"
    /* room: full's display, view and signals, no role: afc2, rdk, stroop and
     * sound name a keyboard role 'resp', which full binds to an xid */
    if (rig_make(&full, roles, display, VIEW, signals) != YRIG_OK || rig_make(&room, "", display, VIEW, signals) != YRIG_OK ||
        rig_make(&part, "\"resp\": {\"family\": \"xid\", \"key\": \"port:/dev/null\"}", NULL, NULL, NULL) != YRIG_OK ||
        rig_make(&wrong, "\"resp\": {\"family\": \"line\", \"key\": \"port:/dev/null\"}", NULL, NULL, NULL) != YRIG_OK ||
        rig_make(&units, "", NULL, VIEW,
                 "{\"left\": {\"adapter\": \"raw_mouse\", \"units\": \"workspace_cm\", \"per_count\": 0.01, \"device\": \"click\"}, "
                 "\"right\": {\"adapter\": \"raw_mouse\", \"units\": \"display_cm\", \"per_count\": 0.01, \"device\": \"click\"}}") != YRIG_OK) {
        CHECK(0, "the test profiles");
        rig_files_remove();
        return;
    }
#undef VIEW
    CHECK(full.n_notes == 0 && full.display.check == YRIG_CHECK_OK && full.role[0].bounds.check == YRIG_CHECK_OK,
          "the full profile's checks: %d notes, calibration %d, bounds %d", full.n_notes, full.display.check, full.role[0].bounds.check);
    memcpy(&changed, &room, sizeof changed);
    changed.display.check = YRIG_CHECK_CHANGED;   /* as yrig_load() gives for an edited calibration file */
    memcpy(&keep, &part, sizeof keep);
    given_cal = 1;
    {
        /* each need the profile meets is gone; the others stay */
        static const struct { const char* pack; const yrig_profile* rig; int cal; const char* needs; } want[] = {
            /* resp bound with a checked bounds loopback (tier 1), trig bound, the view */
            { "oddball", &full, 1, "display_tier::1 field:participant:0" },
            /* resp bound with no loopback (UNKNOWN), trig unbound, no view */
            { "oddball", &part, 1, "device:trig:1 display_tier::1 field:participant:0 role_tier:resp:2 view::1" },
            /* the profile's calibration, given; its view */
            { "afc2", &room, 1, "display_tier::2 field:participant:0" },
            /* the calibration not given: the check reads no file, so the need stays */
            { "afc2", &room, 0, "calibration::0 display_tier::2 field:participant:0" },
            /* its file failed the check: a real start's calibration is not the profile's */
            { "afc2", &changed, 1, "calibration::0 display_tier::2 field:participant:0" },
            /* the signals: hand in display_px; left and right in display_cm, through the view */
            { "visuomotor", &full, 1, "field:participant:0" },
            { "bimanual", &full, 1, "field:participant:0" },
            /* no signals in the profile: the needs stay */
            { "bimanual", &part, 1, "field:participant:0 signal:left:1 signal:right:1 sim_gain:left:0 sim_gain:right:0 view::2" },
        };
        for (i = 0; i < sizeof want / sizeof want[0]; i++) {
            pk* k = pack_of(want[i].pack);
            if (!k) { printf("  %s: no pack, skipped\n", want[i].pack); continue; }
            check_rig = want[i].rig;
            given_cal = want[i].cal;
            (void)run_check(k, NULL, &r);
            needs_text(&r, a, sizeof a);
            CHECK(r.status == PL_CHECK_OK && strcmp(a, want[i].needs) == 0, "%s with profile %d: status %d '%s', needs '%s', want '%s'", want[i].pack, (int)i,
                  r.status, r.message ? r.message : "", a, want[i].needs);
            /* the loader's checks against a rig ran */
            CHECK(r.report && !(r.report->skipped & (YEXP_CHECK_ROLES | YEXP_CHECK_SIGROLES)), "%s with profile %d: skipped 0x%x", want[i].pack, (int)i,
                  r.report ? r.report->skipped : 0u);
            {
                /* a bound role's device never opens: the check opens nothing */
                int j, devs = 0;
                for (j = 0; P.roles && P.def && j < P.def->n_roles; j++) devs += P.roles[j].dev != NULL;
                CHECK(!P.scr_open && !P.ds_open && !P.writer_open && !P.ring_open && devs == 0, "%s with profile %d: a screen, a data set, a writer, a ring or %d devices opened",
                      want[i].pack, (int)i, devs);
            }
            if (want[i].cal && strcmp(want[i].pack, "afc2") == 0)
                CHECK(P.have_cal && memcmp(&P.cal, &nominal, sizeof nominal) == 0, "%s with profile %d: the calibration is not the given one", want[i].pack, (int)i);
            printf("  %s with profile %d: needs %s\n", want[i].pack, (int)i, a);
            pl_close(&P);
        }
    }
    /* the copy, not the caller's profile, takes the stand-in view */
    CHECK(memcmp(&keep, &part, sizeof keep) == 0, "pl_check() wrote into pl_check_desc.rig");
    {
        /* a min_tier of 1: the loopback's tier 1 meets it too */
        pk* k = pack_of("oddball");
        char* def = k ? def_of(k, &n) : NULL;
        char* e = def ? edit(def, "\"min_tier\": 2", "\"min_tier\": 1", NULL) : NULL;
        check_rig = &full;
        CHECK(e && run_check(k, e, &r) == PL_CHECK_OK, "oddball, min_tier 1: status %d: %s", r.status, r.message ? r.message : "");
        needs_text(&r, a, sizeof a);
        CHECK(strcmp(a, "display_tier::1 field:participant:0") == 0, "oddball, min_tier 1, with the profile: needs '%s'", a);
        if (e) pl_close(&P);
        free(e);
        free(def);
    }
    {
        /* the start's refusal: the rig binds resp to a family it does not list */
        pk* k = pack_of("oddball");
        check_rig = &wrong;
        (void)run_check(k, NULL, &r);
        CHECK(r.status == PL_CHECK_DEFINITION && strstr(first_error(&r), "'resp'") && strstr(first_error(&r), "the rig binds it to family 'line', which the role does not list"),
              "oddball, resp bound to line: status %d: '%s' '%s'", r.status, first_error(&r), r.message ? r.message : "");
        printf("  oddball, resp bound to line (%d): %s\n", r.status, first_error(&r));
        pl_close(&P);
        /* a signal's units that do not convert to the role's */
        k = pack_of("bimanual");
        check_rig = &units;
        (void)run_check(k, NULL, &r);
        CHECK(r.status == PL_CHECK_DEFINITION && strstr(first_error(&r), "'left'") && strstr(first_error(&r), "do not convert"),
              "bimanual, left in workspace_cm: status %d: '%s' '%s'", r.status, first_error(&r), r.message ? r.message : "");
        printf("  bimanual, left in workspace_cm (%d): %s\n", r.status, first_error(&r));
        pl_close(&P);
    }
    {
        /* a definition that needs nothing of the profile: the same result */
        static const char* const names[] = { "rdk", "stroop", "sound" };
        for (i = 0; i < sizeof names / sizeof names[0]; i++) {
            pk* k = pack_of(names[i]);
            if (!k) continue;
            check_rig = NULL;
            (void)run_check(k, NULL, &r);
            check_text(&r, a, sizeof a);
            pl_close(&P);
            check_rig = &room;
            (void)run_check(k, NULL, &r);
            check_text(&r, b, sizeof b);
            pl_close(&P);
            CHECK(strcmp(a, b) == 0, "%s: '%s' with no profile, '%s' with one", names[i], a, b);
        }
    }
    check_rig = NULL;
    given_cal = 0;
    rig_files_remove();
}

/* Two calls give one answer (seeded), and a call leaves no clock behind:
 * the same answer on the virtual clock, and the real clock after. */
static void case_repeat_and_clock(void) {
    pk* k = pack_of("oddball");
    pl_check_result r;
    char a[2048], b[2048], ia[65], ib[65];
    int na, nb;
    memset(&r, 0, sizeof r);
    if (!k) return;
    CHECK(!yrt_is_virtual(), "the test starts on the real clock");
    (void)run_check(k, NULL, &r);
    needs_text(&r, a, sizeof a);
    memcpy(ia, P.experiment_id, sizeof ia);
    na = P.n_leaves;
    pl_close(&P);
    CHECK(!yrt_is_virtual(), "pl_check() leaves the real clock");
    (void)run_check(k, NULL, &r);
    needs_text(&r, b, sizeof b);
    CHECK(strcmp(a, b) == 0 && strcmp(ia, P.experiment_id) == 0 && na == P.n_leaves, "a second call gives another answer");
    pl_close(&P);
    yrt_virtual_begin(0);
    CHECK(run_check(k, NULL, &r) == PL_CHECK_OK, "on the virtual clock: %s", r.message);
    needs_text(&r, b, sizeof b);
    memcpy(ib, P.experiment_id, sizeof ib);
    nb = P.n_leaves;
    CHECK(strcmp(a, b) == 0 && strcmp(ia, ib) == 0 && na == nb, "the virtual clock gives another answer");
    pl_close(&P);
    yrt_virtual_end();
    CHECK(!yrt_is_virtual(), "the real clock again");
}

/* Nothing in a folder: the current one and the temporary one. */
static int empty_dir(const char* path) {
#if !defined(_WIN32)
    DIR* d = opendir(path);
    struct dirent* e;
    int n = 0;
    if (!d) return 0;
    while ((e = readdir(d)) != NULL)
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) { printf("  written: %s/%s\n", path, e->d_name); n++; }
    closedir(d);
    return n == 0;
#else
    char pat[1100];
    struct _finddata_t f;
    intptr_t h;
    int n = 0;
    snprintf(pat, sizeof pat, "%s\\*", path);
    h = _findfirst(pat, &f);
    if (h == -1) return 1;
    do { if (strcmp(f.name, ".") && strcmp(f.name, "..")) { printf("  written: %s\\%s\n", path, f.name); n++; } } while (_findnext(h, &f) == 0);
    _findclose(h);
    return n == 0;
#endif
}

static double now_s(void) { return 1e-9 * (double)yrt_now_ns(); }

static int cmp_d(const void* a, const void* b) {
    double x = *(const double*)a, y = *(const double*)b;
    return x < y ? -1 : x > y;
}

static void time_packs(int n) {
    int i, j;
    double* t = (double*)malloc(sizeof(double) * (size_t)n);
    for (i = 0; i < n_packs && t; i++) {
        pl_check_result r;
        memset(&r, 0, sizeof r);
        (void)run_check(&packs[i], NULL, &r);   /* warm */
        pl_close(&P);
        for (j = 0; j < n; j++) {
            double t0 = now_s();
            (void)run_check(&packs[i], NULL, &r);
            pl_close(&P);
            t[j] = now_s() - t0;
        }
        qsort(t, (size_t)n, sizeof *t, cmp_d);
        printf("time %s%s: %d calls, median %.3f ms, max %.3f ms\n", packs[i].name, given_cal ? " (d.cal given)" : "", n, t[n / 2] * 1e3, t[n - 1] * 1e3);
    }
    free(t);
}

#if defined(PL_CHECK_MUT) && !defined(_WIN32)
/* The mutation runner: build the packs with $YPAK from $YPL_REPO. */
static char workdir[512];
static int build_packs(void) {
    static const char* const names[] = { "rdk", "afc2", "oddball", "visuomotor", "bimanual", "stroop", "sound" };
    static char paths[7][700];
    const char* ypak = getenv("YPAK");
    const char* repo = getenv("YPL_REPO") ? getenv("YPL_REPO") : ".";
    const char* font = getenv("YPL_FONT") ? getenv("YPL_FONT") : "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf";
    const char* base = getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp";
    static char cmd[16384];
    size_t i;
    if (!ypak) return -1;
    snprintf(workdir, sizeof workdir, "%s/pl_check_XXXXXX", base);
    if (!mkdtemp(workdir)) return -1;
    for (i = 0; i < sizeof names / sizeof names[0]; i++) {
        snprintf(paths[i], sizeof paths[i], "%s/%s.ysppak", workdir, names[i]);
        if (strcmp(names[i], "sound") == 0) {
            /* a sound stimulus and its audio entry (check_test.py makes one
             * from the rdk example) */
            char dir[600], path[700];
            FILE* fp;
            int k;
            snprintf(dir, sizeof dir, "%s/sound_src", workdir);
            if (mkdir(dir, 0700) != 0) return -1;
            snprintf(path, sizeof path, "%s/pack.json", dir);
            if (!(fp = fopen(path, "wb"))) return -1;
            fputs("{\"format\":\"ysp-pack-source\",\"version\":1,\"audio\":{\"rate\":48000,\"channels\":2},"
                  "\"experiment\":{\"entry\":\"experiment/main.json\",\"source\":\"experiment.json\",\"media_type\":\"application/vnd.ysp.experiment\"},"
                  "\"resources\":[{\"name\":\"tone.wav\",\"kind\":\"audio\",\"source\":\"tone.wav\"}]}\n", fp);
            fclose(fp);
            snprintf(path, sizeof path, "%s/experiment.json", dir);
            if (!(fp = fopen(path, "wb"))) return -1;
            fputs("{\"format\":\"ysp-experiment\",\"version\":1,\"name\":\"sound\","
                  "\"devices\":{\"resp\":{\"direction\":\"in\",\"family\":[\"keyboard\"],\"required\":true}},"
                  "\"stimuli\":{\"fix\":{\"kind\":\"shape\",\"w\":16,\"h\":16},\"tone\":{\"kind\":\"sound\",\"audio\":\"tone.wav\"}},"
                  "\"trials\":{\"t\":{\"events\":[{\"on\":\"tone\"},{\"on\":\"fix\"}],"
                  "\"response\":{\"role\":\"resp\",\"choices\":[\"left\",\"right\"],\"from\":\"fix\",\"duration_s\":1}}},"
                  "\"timeline\":[{\"name\":\"main\",\"trial\":\"t\",\"rules\":[\"reps 2\"]}]}\n", fp);
            fclose(fp);
            snprintf(path, sizeof path, "%s/tone.wav", dir);
            if (!(fp = fopen(path, "wb"))) return -1;
            {
                /* 4800 frames of stereo 16-bit silence at 48 kHz */
                static const unsigned char hdr[44] = { 'R', 'I', 'F', 'F', 0x24, 0x4b, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 2, 0,
                                                       0x80, 0xbb, 0, 0, 0x00, 0xee, 0x02, 0, 4, 0, 16, 0, 'd', 'a', 't', 'a', 0x00, 0x4b, 0, 0 };
                fwrite(hdr, 1, sizeof hdr, fp);
                for (k = 0; k < 4800 * 4; k++) fputc(0, fp);
            }
            fclose(fp);
            snprintf(cmd, sizeof cmd, "cd '%s' && '%s' build pack.json -o '%s' >/dev/null", dir, ypak, paths[i]);
        } else if (strcmp(names[i], "stroop") == 0) {
            if (access(font, R_OK) != 0) continue;
            snprintf(cmd, sizeof cmd, "cp -r '%s/tests/player/stroop' '%s/stroop_src' && mkdir -p '%s/stroop_src/fonts' && cp '%s' '%s/stroop_src/fonts/NotoSans-Regular.ttf' && "
                     "cd '%s/stroop_src' && '%s' build pack.json -o '%s' >/dev/null",
                     repo, workdir, workdir, font, workdir, workdir, ypak, paths[i]);
        } else {
            snprintf(cmd, sizeof cmd, "cd '%s/tests/player/%s' && '%s' build pack.json -o '%s' >/dev/null", repo, names[i], ypak, paths[i]);
        }
        if (system(cmd) != 0) { printf("ypak failed: %s\n", cmd); return -1; }
        packs[n_packs].name = names[i];
        if (read_file(paths[i], &packs[n_packs].data, &packs[n_packs].size) != 0) return -1;
        n_packs++;
    }
    return 0;
}
#endif

int main(int argc, char** argv) {
    int i, timing = 0;
    for (i = 1; i < argc; i++) {
        const char* eq = strchr(argv[i], '=');
        if (strcmp(argv[i], "--time") == 0 && i + 1 < argc) { timing = atoi(argv[++i]); continue; }
        if (!eq || n_packs == 8) { printf("usage: test_player_check NAME=PACK... [--time N]\n"); return 2; }
        packs[n_packs].name = argv[i];
        argv[i][eq - argv[i]] = 0;
        if (read_file(eq + 1, &packs[n_packs].data, &packs[n_packs].size) != 0) { printf("cannot read %s\n", eq + 1); return 2; }
        n_packs++;
    }
#if defined(PL_CHECK_MUT) && !defined(_WIN32)
    if (n_packs == 0 && build_packs() != 0) { printf("no packs: give NAME=PACK, or set YPAK\n"); return 2; }
#endif
    if (!pack_of("rdk") || !pack_of("oddball")) { printf("the rdk and oddball packs are required\n"); return 2; }
    if (pl_check_nominal_cal(&nominal) < 0) { printf("pl_check_nominal_cal() failed\n"); return 2; }
    if (timing > 0) {
        time_packs(timing);
        given_cal = 1;
        time_packs(timing);
        return 0;
    }
    {
        /* the cases run in an empty folder, with TMPDIR another: a write to
         * either shows */
        char cwd_dir[1024], tmp_dir[1024], old[1024];
        const char* base = getenv("TMPDIR") ? getenv("TMPDIR") : getenv("TEMP") ? getenv("TEMP") : "/tmp";
        long long live0 = 0;
        /* the process id: the mutation runner runs several at once */
        snprintf(cwd_dir, sizeof cwd_dir, "%s/pl_check_cwd_%d_%d", base, (int)getpid(), (int)time(NULL));
        snprintf(tmp_dir, sizeof tmp_dir, "%s/pl_check_tmp_%d_%d", base, (int)getpid(), (int)time(NULL));
        snprintf(rig_dir, sizeof rig_dir, "%s/pl_check_rig_%d_%d", base, (int)getpid(), (int)time(NULL));
#if !defined(_WIN32)
        if (!getcwd(old, sizeof old) || mkdir(cwd_dir, 0700) != 0 || mkdir(tmp_dir, 0700) != 0 || mkdir(rig_dir, 0700) != 0 || chdir(cwd_dir) != 0) { printf("no work folders under %s\n", base); return 2; }
        setenv("TMPDIR", tmp_dir, 1);
#else
        if (!_getcwd(old, sizeof old) || _mkdir(cwd_dir) != 0 || _mkdir(tmp_dir) != 0 || _mkdir(rig_dir) != 0 || _chdir(cwd_dir) != 0) { printf("no work folders under %s\n", base); return 2; }
        _putenv_s("TMP", tmp_dir);
        _putenv_s("TEMP", tmp_dir);
#endif
        case_examples();
        case_sound();
        case_reader("oddball");
        case_definition();
        case_refusals();
        case_run_only();
        case_entries();
        case_repeat_and_clock();
        case_given_cal();
        case_rig();
#if defined(PL_CHECK_COUNT)
        /* every case's blocks, then 20 more rounds of the examples: the live
         * count must not grow */
        live0 = live;
        for (i = 0; i < 20; i++) {
            int j;
            for (j = 0; j < n_packs; j++) {
                pl_check_result r;
                memset(&r, 0, sizeof r);
                (void)run_check(&packs[j], NULL, &r);
                pl_close(&P);
            }
        }
        CHECK(live == live0, "the live heap blocks went from %lld to %lld over 20 rounds", live0, live);
        printf("  live heap blocks: %lld before and %lld after 20 rounds of %d packs\n", live0, live, n_packs);
#else
        (void)live0;
#endif
        CHECK(empty_dir(cwd_dir), "a file in the current folder");
        CHECK(empty_dir(tmp_dir), "a file in the temporary folder");
#if !defined(_WIN32)
        if (chdir(old) != 0) printf("cannot go back to %s\n", old);
        rmdir(cwd_dir);
        rmdir(tmp_dir);
#else
        _chdir(old);
        _rmdir(cwd_dir);
        _rmdir(tmp_dir);
#endif
    }
#if defined(PL_CHECK_MUT) && !defined(_WIN32)
    if (workdir[0]) {
        char cmd[600];
        snprintf(cmd, sizeof cmd, "rm -r '%s'", workdir);
        (void)!system(cmd);
    }
#endif
    for (i = 0; i < n_packs; i++) free(packs[i].data);
    printf("test_player_check: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
