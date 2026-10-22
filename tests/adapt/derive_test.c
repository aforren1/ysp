/* derive_test.c - self-checking test of the texts the pack tool takes from
 * an experiment definition (pack/derive.c, ypt_derive_texts; gap G9 of
 * docs/experiment_format.md). Every form that gives texts, every form that
 * is refused (each by its message), the order and uniqueness of the keys,
 * the canonical numbers, and the bounds on nesting and work; and the
 * lookups' keys against their levels (ypt_check_lookups): every source of
 * levels, the scope of a leaf, each refusal and warning by its message, and
 * the bounds. No layout and
 * no pack: tables are made here with ysp/table.h. No framework: it returns
 * 0 when every check passed and 1 after printing each failure.
 *
 * It compiles pack/derive.c into itself, so the mutation runner can mutate
 * that file (tests/mutate/derive.toml):
 *
 *     gcc -std=c11 -O1 -Wall -Wextra -Iinclude -Ipack tests/adapt/derive_test.c -lm
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#define YSP_JSON_IMPLEMENTATION
#include "ysp/json.h"
#define YSP_TABLE_IMPLEMENTATION
#include "ysp/table.h"
#define YSP_PACK_IMPLEMENTATION
#define YPAK_NO_FILE
#include "ysp/pack.h"
#include "derive.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail, g_checks;
#define CHECK(c, ...) do { g_checks++; if (!(c)) { g_fail++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* --- tables, by entry name ------------------------------------------------- */

typedef struct tab { const char* name; ytb_table t; uint64_t arena[2048]; } tab;
static tab g_tabs[8];
static int g_ntabs, g_calls;

static void make_table(const char* name, const char* csv) {
    tab* t = &g_tabs[g_ntabs++];
    ytb_csv_desc d;
    memset(&d, 0, sizeof d);
    d.text = csv;
    d.len = strlen(csv);
    d.arena = t->arena;
    d.arena_size = sizeof t->arena;
    t->name = name;
    CHECK(ytb_open_csv(&t->t, &d), "table %s: %s", name, ytb_error(&t->t));
}

static const void* table_of(void* ctx, const char* name, size_t* n) {
    int i;
    (void)ctx;
    g_calls++;
    for (i = 0; i < g_ntabs; i++)
        if (!strcmp(g_tabs[i].name, name)) { *n = g_tabs[i].t.size; return g_tabs[i].t.base; }
    return NULL;
}

static int derive(const char* def, ypt_texts* out, char* err, size_t cap) {
    return ypt_derive_texts(def, strlen(def), table_of, NULL, out, err, cap);
}

/* A definition with one text stimulus `s` and these nodes. */
static void def_with(char* out, size_t cap, const char* stim_keys, const char* nodes) {
    snprintf(out, cap,
             "{\"format\": \"ysp-experiment\", \"version\": 1, \"session\": {\"fields\": {\"group\": {\"type\": \"string\"}}},"
             " \"stimuli\": {\"s\": {\"kind\": \"text\", \"font\": \"f.ttf\", %s}, \"dot\": {\"kind\": \"shape\"}},"
             " \"trials\": {\"t\": {}}, \"timeline\": [%s]}", stim_keys, nodes);
}

/* The texts as "key|size|width|line_height|align|dir|lang" lines. */
static void texts_line(const ypt_texts* t, char* out, size_t cap) {
    size_t at = 0;
    int i;
    out[0] = 0;
    for (i = 0; i < t->n_texts && at < cap; i++) {
        const ypt_text* x = &t->texts[i];
        at += (size_t)snprintf(out + at, cap - at, "%s%s/%s|%s|%s|%s|%s|%s|%s", i ? "\n" : "", x->stimulus, x->text, x->size, x->width,
                               x->line_height, x->align, x->dir, x->lang);
    }
}

static void expect_texts(const char* label, const char* stim_keys, const char* nodes, const char* want, const char* tables) {
    char def[20000], got[4096], err[512], tl[256];
    ypt_texts t;
    int rc, i;
    def_with(def, sizeof def, stim_keys, nodes);
    rc = derive(def, &t, err, sizeof err);
    CHECK(rc == 0 && t.definition == 1, "%s: rc %d, %s", label, rc, err);
    if (rc) return;
    texts_line(&t, got, sizeof got);
    CHECK(!strcmp(got, want), "%s:\n%s\nwanted\n%s", label, got, want);
    tl[0] = 0;
    for (i = 0; i < t.n_tables; i++) { strcat(tl, i ? "," : ""); strcat(tl, t.tables[i]); }
    CHECK(!strcmp(tl, tables), "%s: tables '%s', wanted '%s'", label, tl, tables);
    for (i = 0; i < t.n_texts; i++) {
        const ypt_text* x = &t.texts[i];
        char k2[1024];
        size_t tn = strlen(x->text);
        int64_t kn = ypak_runs_key(&x->style, x->text, tn, k2, sizeof k2);
        CHECK(kn > 0 && !strcmp(k2, x->key) && x->text == x->key + kn - (int64_t)tn && !strcmp(x->font, "f.ttf") && x->n_stimuli == 1 &&
              !strcmp(x->stimuli[0], x->stimulus), "%s: text %d: the key is ypak_runs_key() of its style and text", label, i);
        if (i) CHECK(strcmp(t.texts[i - 1].key, x->key) < 0, "%s: keys strictly ascending at %d", label, i);
    }
    ypt_texts_free(&t);
}

static void expect_refused(const char* label, const char* stim_keys, const char* nodes, const char* says) {
    char def[20000], err[512];
    ypt_texts t;
    int rc;
    def_with(def, sizeof def, stim_keys, nodes);
    rc = derive(def, &t, err, sizeof err);
    CHECK(rc == YPAK_ERR_FORMAT && strstr(err, says) != NULL && t.n_texts == 0 && t.mem_ == NULL, "%s: rc %d, '%s' (wanted '%s')", label, rc,
          err, says);
    if (rc == 0) ypt_texts_free(&t);
}

#define LEAF "{\"name\": \"main\", \"trial\": \"t\"}"
#define TV(name) "{\"name\": \"main\", \"trial\": \"t\", \"timeline_variables\": \"" name "\"}"

/* --- the Stroop definition of docs/experiment_format.md 2.14 ----------------- */

static void test_stroop(void) {
    static const char def[] =
        "{\"format\": \"ysp-experiment\", \"version\": 1, \"name\": \"stroop\",\n"
        " \"stimuli\": {\n"
        "  \"fix\": {\"kind\": \"shape\", \"shape\": \"cross\", \"w\": 24},\n"
        "  \"word\": {\"kind\": \"text\", \"font\": \"fonts/NotoSans-Regular.ttf\", \"size\": 72, \"width\": 1000,\n"
        "           \"text\": {\"by\": \"$word\", \"map\": {\"red\": \"RED\", \"green\": \"GREEN\", \"blue\": \"BLUE\"}},\n"
        "           \"color\": {\"by\": \"$ink\", \"map\": {\"red\": [0.7, 0.04, 0.03], \"green\": [0.04, 0.42, 0.05], \"blue\": [0.07, 0.15, 0.85]}}},\n"
        "  \"banner\": {\"kind\": \"text\", \"font\": \"fonts/NotoSans-Regular.ttf\", \"size\": 30, \"width\": 1000, \"y\": -30,\n"
        "             \"text\": {\"by\": \"$practice\", \"map\": {\"true\": \"Practice. Name the ink color, not the word.\",\n"
        "                                                   \"false\": \"Test. No feedback from here on.\"}}},\n"
        "  \"keys\": {\"kind\": \"text\", \"font\": \"fonts/NotoSans-Regular.ttf\", \"size\": 22, \"width\": 1000, \"text\": \"R red     G green     B blue\"},\n"
        "  \"feedback\": {\"kind\": \"text\", \"font\": \"fonts/NotoSans-Regular.ttf\", \"size\": 32, \"width\": 1000,\n"
        "               \"text\": {\"by\": \"$outcome\", \"map\": {\"correct\": \"Correct\", \"wrong\": \"Wrong\", \"timeout\": \"Too slow\"}}}},\n"
        " \"trials\": {\"stroop\": {\"columns\": {\"feedback\": {\"by\": \"$practice\", \"map\": {\"true\": \"$outcome\", \"false\": \"\"}}}}},\n"
        " \"timeline\": [{\"name\": \"main\", \"timeline_variables\": \"stroop.pstb\", \"trial\": \"stroop\",\n"
        "               \"rules\": [\"order constrained\", \"weight weight\"]}]}\n";
#define NOTO "fonts/NotoSans-Regular.ttf#0|"
    static const char* const want[9][2] = {
        { NOTO "22|1000|0|start|auto||R red     G green     B blue", "22" },
        { NOTO "30|1000|0|start|auto||Practice. Name the ink color, not the word.", "30" },
        { NOTO "30|1000|0|start|auto||Test. No feedback from here on.", "30" },
        { NOTO "32|1000|0|start|auto||Correct", "32" }, { NOTO "32|1000|0|start|auto||Too slow", "32" },
        { NOTO "32|1000|0|start|auto||Wrong", "32" }, { NOTO "72|1000|0|start|auto||BLUE", "72" },
        { NOTO "72|1000|0|start|auto||GREEN", "72" }, { NOTO "72|1000|0|start|auto||RED", "72" } };
#undef NOTO
    ypt_texts t;
    char err[512];
    int rc = derive(def, &t, err, sizeof err), i;
    CHECK(rc == 0 && t.definition && t.n_texts == 9 && t.n_tables == 0, "stroop: rc %d, %d texts, %d tables: %s", rc, t.n_texts, t.n_tables, err);
    for (i = 0; rc == 0 && i < 9 && i < t.n_texts; i++)
        CHECK(!strcmp(t.texts[i].key, want[i][0]) && !strcmp(t.texts[i].size, want[i][1]) && !strcmp(t.texts[i].width, "1000") &&
              !strcmp(t.texts[i].line_height, "0") && !strcmp(t.texts[i].align, "start") && !strcmp(t.texts[i].dir, "auto") &&
              !t.texts[i].lang[0] && !strcmp(t.texts[i].font, "fonts/NotoSans-Regular.ttf"),
              "stroop text %d: %s size %s", i, t.texts[i].key, t.texts[i].size);
    CHECK(g_calls == 0, "stroop: no table read for lookups (%d calls)", g_calls);
    if (rc == 0) ypt_texts_free(&t);
    ypt_texts_free(&t);   /* twice: safe */
}

/* --- one block per style and text --------------------------------------------- */

/* Two stimuli of one style that show one text share its block; a stimulus
 * of another size, another font or another lang gets its own. 72.0 and 72
 * are one size. */
static void test_shared(void) {
    static const char def[] =
        "{\"format\": \"ysp-experiment\", \"version\": 1, \"stimuli\": {\n"
        " \"b\": {\"kind\": \"text\", \"font\": \"f.ttf\", \"size\": 72.0, \"width\": 1000, \"text\": \"Hi\"},\n"
        " \"a\": {\"kind\": \"text\", \"font\": \"f.ttf\", \"size\": 72, \"width\": 1e3, \"x\": 5, \"color\": [1, 0, 0],"
        " \"text\": {\"by\": \"$q\", \"map\": {\"1\": \"Hi\", \"2\": \"Bye\"}}},\n"
        " \"c\": {\"kind\": \"text\", \"font\": \"f.ttf\", \"size\": 36, \"width\": 1000, \"text\": \"Hi\"},\n"
        " \"d\": {\"kind\": \"text\", \"font\": \"g.ttf\", \"size\": 72, \"width\": 1000, \"text\": \"Hi\"},\n"
        " \"e\": {\"kind\": \"text\", \"font\": \"f.ttf\", \"size\": 72, \"width\": 1000, \"lang\": \"en\", \"text\": \"Hi\"}}}";
    static const char* const want[5] = { "f.ttf#0|36|1000|0|start|auto||Hi", "f.ttf#0|72|1000|0|start|auto|en|Hi",
                                         "f.ttf#0|72|1000|0|start|auto||Bye", "f.ttf#0|72|1000|0|start|auto||Hi",
                                         "g.ttf#0|72|1000|0|start|auto||Hi" };
    static const char* const stim[5] = { "c", "e", "a", "a,b", "d" };
    ypt_texts t;
    char err[512];
    int rc = derive(def, &t, err, sizeof err), i;
    CHECK(rc == 0 && t.n_texts == 5, "shared: rc %d, %d texts: %s", rc, t.n_texts, err);
    for (i = 0; rc == 0 && i < 5 && i < t.n_texts; i++) {
        char names[64] = "";
        int k;
        for (k = 0; k < t.texts[i].n_stimuli; k++) { strcat(names, k ? "," : ""); strcat(names, t.texts[i].stimuli[k]); }
        CHECK(!strcmp(t.texts[i].key, want[i]) && !strcmp(names, stim[i]) && !strcmp(t.texts[i].stimulus, t.texts[i].stimuli[0]),
              "shared text %d: '%s' of %s", i, t.texts[i].key, names);
    }
    if (rc == 0) ypt_texts_free(&t);
}

/* --- the forms that give texts ------------------------------------------------ */

static void test_forms(void) {
    make_table("words.pstb", "word,n,x\nred,1,0.5\ngreen,2,0.25\nblue,1,0.5\nred,3,1\n");
    make_table("more.pstb", "word,prize\nviolet,$5\nred,$7\n");
    expect_texts("literal", "\"size\": 10, \"text\": \"Hello\"", LEAF, "s/Hello|10|0|0|start|auto|", "");
    expect_texts("escaped dollar", "\"size\": 10, \"text\": \"$$5 prize\"", LEAF, "s/$5 prize|10|0|0|start|auto|", "");
    expect_texts("empty literal", "\"size\": 10, \"text\": \"\"", LEAF, "s/|10|0|0|start|auto|", "");
    expect_texts("column levels", "\"size\": 10, \"text\": \"$word\"", TV("words.pstb"),
                 "s/blue|10|0|0|start|auto|\ns/green|10|0|0|start|auto|\ns/red|10|0|0|start|auto|", "words.pstb");
    expect_texts("column of two tables, a container's",
                 "\"size\": 10, \"text\": \"$word\"",
                 "{\"name\": \"a\", \"timeline_variables\": \"words.pstb\", \"timeline\": [" TV("more.pstb") "]}",
                 "s/blue|10|0|0|start|auto|\ns/green|10|0|0|start|auto|\ns/red|10|0|0|start|auto|\ns/violet|10|0|0|start|auto|",
                 "more.pstb,words.pstb");
    expect_texts("column in the second table only", "\"size\": 10, \"text\": \"$prize\"",
                 "{\"name\": \"a\", \"timeline_variables\": \"words.pstb\", \"timeline\": [" TV("more.pstb") "]}",
                 "s/$5|10|0|0|start|auto|\ns/$7|10|0|0|start|auto|", "more.pstb");
    expect_texts("a table cell is text, not a variable", "\"size\": 10, \"text\": \"$prize\"", TV("more.pstb"),
                 "s/$5|10|0|0|start|auto|\ns/$7|10|0|0|start|auto|", "more.pstb");
    expect_texts("vars value and choice",
                 "\"size\": 10, \"text\": {\"by\": \"$side\", \"map\": {\"left\": \"$msg\", \"right\": \"$side\"}}",
                 "{\"name\": \"main\", \"trial\": \"t\", \"vars\": {\"msg\": \"Go\", \"side\": {\"choice\": [\"left\", \"right\"]}}}",
                 "s/Go|10|0|0|start|auto|\ns/left|10|0|0|start|auto|\ns/right|10|0|0|start|auto|", "");
    expect_texts("vars choice of a column",
                 "\"size\": 10, \"text\": \"$pick\"",
                 "{\"name\": \"main\", \"trial\": \"t\", \"timeline_variables\": \"more.pstb\", \"vars\": {\"pick\": {\"choice\": [\"$word\", \"none\"]}}}",
                 "s/none|10|0|0|start|auto|\ns/red|10|0|0|start|auto|\ns/violet|10|0|0|start|auto|", "more.pstb");
    expect_texts("nested lookups and duplicates",
                 "\"size\": 10, \"text\": {\"by\": \"$a\", \"map\": {\"x\": \"Same\", \"y\": {\"by\": \"$b\", \"map\": {\"1\": \"Same\", \"2\": \"Other\"}}}}",
                 LEAF, "s/Other|10|0|0|start|auto|\ns/Same|10|0|0|start|auto|", "");
    expect_texts("layout keys, canonical numbers",
                 "\"size\": 2.50, \"width\": 1e3, \"line_height\": 1.20, \"align\": \"center\", \"dir\": \"rtl\", \"lang\": \"he\", \"text\": \"x\"",
                 LEAF, "s/x|2.5|1000|1.2|center|rtl|he", "");
    {
        /* the key itself: every layout key in its field */
        char def[1024], err[256];
        ypt_texts t;
        def_with(def, sizeof def, "\"size\": 2.50, \"width\": 1e3, \"line_height\": 1.20, \"align\": \"center\", \"dir\": \"rtl\", "
                                  "\"lang\": \"he\", \"text\": \"x|y\"", LEAF);
        CHECK(derive(def, &t, err, sizeof err) == 0 && t.n_texts == 1 && !strcmp(t.texts[0].key, "f.ttf#0|2.5|1000|1.2|center|rtl|he|x|y"),
              "the key of every layout key: '%s'", t.n_texts ? t.texts[0].key : err);
        ypt_texts_free(&t);
    }
    /* the double's shortest decimal, not the source's exact one: the player
     * reads a double and spells it with yjs_format_double() */
    expect_texts("a double's spelling", "\"size\": 0.1000000000000000055, \"text\": \"x\"", LEAF, "s/x|0.1|0|0|start|auto|", "");
    expect_texts("size in degrees", "\"size\": 0.8, \"width\": 0, \"text\": \"x\"", LEAF, "s/x|0.8|0|0|start|auto|", "");
    expect_texts("UTF-8 and control bytes in a text", "\"size\": 10, \"text\": \"\\u05e9\\u05dc\\u05d5\\u05dd\\nline\"", LEAF,
                 "s/\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d\nline|10|0|0|start|auto|", "");
    /* a table that no text needs is not read, so a missing one is the loader's */
    g_calls = 0;
    expect_texts("unused table not read", "\"size\": 10, \"text\": \"x\"", TV("nothing.pstb"), "s/x|10|0|0|start|auto|", "");
    CHECK(g_calls == 0, "an unused table was read (%d calls)", g_calls);
    /* one view per table, however many lookups */
    g_calls = 0;
    expect_texts("one open per table", "\"size\": 10, \"text\": {\"by\": \"$q\", \"map\": {\"a\": \"$word\", \"b\": \"$word\", \"c\": \"$word\"}}",
                 TV("words.pstb"), "s/blue|10|0|0|start|auto|\ns/green|10|0|0|start|auto|\ns/red|10|0|0|start|auto|", "words.pstb");
    CHECK(g_calls == 1, "table opened %d times for three lookups", g_calls);
}

/* --- not a definition, other versions ------------------------------------------ */

static void test_opaque(void) {
    static const char* const opaque[] = { "{\"trials\": \"conditions.pstb\", \"note\": \"opaque to the pack\"}", "not json",
                                          "{\"format\": \"other\", \"version\": 1}", "[1, 2]", "", "{\"format\": 5}" };
    ypt_texts t;
    char err[256];
    int i, rc;
    for (i = 0; i < 6; i++) {
        rc = ypt_derive_texts(opaque[i], strlen(opaque[i]), table_of, NULL, &t, err, sizeof err);
        CHECK(rc == 0 && t.definition == 0 && t.n_texts == 0 && !err[0], "opaque %d: rc %d, def %d", i, rc, t.definition);
        ypt_texts_free(&t);
    }
    rc = derive("{\"format\": \"ysp-experiment\", \"version\": 2}", &t, err, sizeof err);
    CHECK(rc == YPAK_ERR_FORMAT && strstr(err, "version is 2; this ypak lays out the texts of version 1"), "version 2: %s", err);
    rc = derive("{\"format\": \"ysp-experiment\", \"version\": 1,\n \"stimuli\": {},}", &t, err, sizeof err);
    CHECK(rc == YPAK_ERR_FORMAT && strstr(err, "names \"ysp-experiment\" but is not JSON: line 2, column") && !t.mem_, "a broken definition: %s", err);
    rc = derive("{\"format\": \"ysp-experiment\"}", &t, err, sizeof err);
    CHECK(rc == YPAK_ERR_FORMAT && strstr(err, "version is missing"), "no version: %s", err);
    rc = derive("{\"format\": \"ysp-experiment\", \"version\": 1.0}", &t, err, sizeof err);
    CHECK(rc == YPAK_ERR_FORMAT && strstr(err, "version is 1.0"), "version 1.0: %s", err);
    rc = derive("{\"format\": \"ysp-experiment\", \"version\": 1, \"stimuli\": {\"a\": {\"kind\": \"shape\"}, \"b\": 5}}", &t, err, sizeof err);
    CHECK(rc == 0 && t.definition == 1 && t.n_texts == 0, "no text stimuli: rc %d, %d texts", rc, t.n_texts);
    ypt_texts_free(&t);
    CHECK(ypt_derive_texts(NULL, 0, table_of, NULL, &t, err, sizeof err) == YPAK_ERR_ARG, "NULL definition");
    CHECK(ypt_derive_texts("{}", 2, NULL, NULL, &t, err, sizeof err) == YPAK_ERR_ARG, "NULL table function");
    CHECK(ypt_derive_texts("{}", 2, table_of, NULL, NULL, err, sizeof err) == YPAK_ERR_ARG, "NULL out");
}

/* --- refusals ----------------------------------------------------------------- */

static void test_refusals(void) {
    char def[4096], nodes[16384];
    int i, at;
    expect_refused("built-in", "\"size\": 10, \"text\": \"$outcome\"", LEAF, "'$outcome' is a built-in variable");
    expect_refused("built-in in a map", "\"size\": 10, \"text\": {\"by\": \"$x\", \"map\": {\"a\": \"$trial_index\"}}", LEAF,
                   "'$trial_index' is a built-in variable");
    expect_refused("rt", "\"size\": 10, \"text\": \"$rt_s\"", LEAF, "{\"by\": \"$rt_s\", \"map\": {...}}");
    expect_refused("participant", "\"size\": 10, \"text\": \"$participant\"", LEAF, "'$participant' is a session field");
    expect_refused("session field", "\"size\": 10, \"text\": \"$group\"", LEAF, "'$group' is a session field");
    expect_refused("unknown", "\"size\": 10, \"text\": \"$soa_s\"", LEAF, "'$soa_s' names no table column, 'vars' entry or built-in");
    expect_refused("not a column of the table", "\"size\": 10, \"text\": \"$nope\"", TV("words.pstb"), "'$nope' names no table column");
    expect_refused("integer column", "\"size\": 10, \"text\": \"$n\"", TV("words.pstb"), "'$n' is a column of 'words.pstb' of type integer");
    expect_refused("number column", "\"size\": 10, \"text\": \"$x\"", TV("words.pstb"), "of type number; a text comes from a string column");
    expect_refused("missing table", "\"size\": 10, \"text\": \"$word\"", TV("nothing.pstb"),
                   "node 'main': 'timeline_variables' is 'nothing.pstb', which is not a table entry");
    expect_refused("timeline_variables not a string", "\"size\": 10, \"text\": \"x\"", "{\"name\": \"m\", \"timeline_variables\": 3}",
                   "node 'm': 'timeline_variables' names a table entry");
    expect_refused("script", "\"size\": 10, \"text\": {\"lua\": \"return 'hi'\"}", LEAF, "text from a script");
    expect_refused("number text", "\"size\": 10, \"text\": 5", LEAF, "a text is a string, a \"$variable\" or a lookup");
    expect_refused("bool text", "\"size\": 10, \"text\": true", LEAF, "this value is none of them");
    expect_refused("array text", "\"size\": 10, \"text\": [\"a\"]", LEAF, "this value is none of them");
    expect_refused("map without by", "\"size\": 10, \"text\": {\"map\": {\"a\": \"b\"}}", LEAF, "this value is none of them");
    expect_refused("map not an object", "\"size\": 10, \"text\": {\"by\": \"$a\", \"map\": [\"b\"]}", LEAF, "this value is none of them");
    expect_refused("vars number", "\"size\": 10, \"text\": \"$v\"", "{\"name\": \"m\", \"vars\": {\"v\": 3}}", "this value is none of them");
    expect_refused("no size", "\"text\": \"x\"", LEAF, "a text needs 'font' (a FONT entry of the pack), 'size' and 'text'");
    expect_refused("no text", "\"size\": 10", LEAF, "needs 'font'");
    expect_refused("size as text", "\"size\": \"72\", \"text\": \"x\"", LEAF, "'size' must be a number: glyph runs are laid out at pack build");
    expect_refused("size variable", "\"size\": \"$size\", \"text\": \"x\"", LEAF, "'size' must be a number");
    expect_refused("size lookup", "\"size\": {\"by\": \"$a\", \"map\": {\"x\": 3}}, \"text\": \"x\"", LEAF, "'size' must be a number");
    expect_refused("size 0", "\"size\": 0, \"text\": \"x\"", LEAF, "'size' is 0; it is above 0");
    expect_refused("size negative", "\"size\": -3, \"text\": \"x\"", LEAF, "'size' is -3; it is above 0");
    expect_refused("width negative", "\"size\": 3, \"width\": -1, \"text\": \"x\"", LEAF, "'width' is -1; it is at least 0");
    expect_refused("width huge", "\"size\": 3, \"width\": 1e10, \"text\": \"x\"", LEAF, "and at most 1e9");
    expect_refused("line height", "\"size\": 3, \"line_height\": -0.5, \"text\": \"x\"", LEAF, "'line_height' is -0.5");
    expect_refused("align", "\"size\": 3, \"align\": \"middle\", \"text\": \"x\"", LEAF, "'align' is \"middle\"; it is start, end or center");
    expect_refused("dir", "\"size\": 3, \"dir\": \"ttb\", \"text\": \"x\"", LEAF, "'dir' is \"ttb\"; it is auto, ltr or rtl");
    expect_refused("lang variable", "\"size\": 3, \"lang\": \"$lang\", \"text\": \"x\"", LEAF, "'lang' is a variable");
    expect_refused("lang number", "\"size\": 3, \"lang\": 5, \"text\": \"x\"", LEAF, "'lang' must be a string");
    expect_refused("lang not BCP 47", "\"size\": 3, \"lang\": \"en_US\", \"text\": \"x\"", LEAF,
                   "'lang' is \"en_US\"; a BCP 47 tag has letters, digits and '-' only");
    expect_refused("loop", "\"size\": 3, \"text\": \"$a\"", "{\"name\": \"m\", \"vars\": {\"a\": \"$b\", \"b\": \"$a\"}}",
                   "variables nest more than 8 deep");
    /* the font variable: its own message */
    def_with(def, sizeof def, "\"size\": 3, \"text\": \"x\"", LEAF);
    {
        char d2[4096], err[256];
        ypt_texts t;
        char* f = strstr(def, "\"f.ttf\"");
        snprintf(d2, sizeof d2, "%.*s\"$font\"%s", (int)(f - def), def, f + 7);
        CHECK(derive(d2, &t, err, sizeof err) == YPAK_ERR_FORMAT && strstr(err, "'font' is a variable"), "font variable: %s", err);
        snprintf(d2, sizeof d2, "%.*s\"\"%s", (int)(f - def), def, f + 7);
        CHECK(derive(d2, &t, err, sizeof err) == YPAK_ERR_FORMAT && strstr(err, "'font' is empty"), "empty font: %s", err);
        snprintf(d2, sizeof d2, "%.*s\"a#b.ttf\"%s", (int)(f - def), def, f + 7);
        CHECK(derive(d2, &t, err, sizeof err) == YPAK_ERR_FORMAT && strstr(err, "'font' is \"a#b.ttf\", which is not a pack entry name"),
              "a font name with '#': %s", err);
    }
    /* the work bound: v0 to v6 each defined in 9 nodes as the next, v7 a
     * text: 9^7 lookups without the bound */
    at = 0;
    for (i = 0; i < 9 * 8 && at < (int)sizeof nodes - 100; i++) {
        int v = i % 8;
        if (v < 7) at += snprintf(nodes + at, sizeof nodes - (size_t)at, "%s{\"name\": \"n%d\", \"vars\": {\"v%d\": \"$v%d\"}}", i ? "," : "", i, v, v + 1);
        else at += snprintf(nodes + at, sizeof nodes - (size_t)at, ",{\"name\": \"n%d\", \"vars\": {\"v7\": \"end\"}}", i);
    }
    expect_refused("work bound", "\"size\": 3, \"text\": \"$v0\"", nodes, "steps to resolve");
    /* 256 nodes with a table, then one more */
    at = 0;
    for (i = 0; i < 257; i++) at += snprintf(nodes + at, sizeof nodes - (size_t)at, "%s{\"timeline_variables\": \"words.pstb\"}", i ? "," : "");
    expect_refused("257 tables", "\"size\": 3, \"text\": \"x\"", nodes, "more than 256 nodes name a table");
    at = 0;
    for (i = 0; i < 257; i++) at += snprintf(nodes + at, sizeof nodes - (size_t)at, "%s{\"vars\": {}}", i ? "," : "");
    expect_refused("257 vars", "\"size\": 3, \"text\": \"x\"", nodes, "more than 256 nodes have 'vars'");
}

/* --- lookups: a key for every level (ypt_check_lookups) ------------------------ */

static char g_warn[4096];
static int g_nwarn;

static void warn_of(void* ctx, const char* line) {
    size_t at = strlen(g_warn);
    (void)ctx;
    g_nwarn++;
    snprintf(g_warn + at, sizeof g_warn - at, "%s%s", at ? "\n" : "", line);
}

/* A definition of these stimuli, templates and nodes, checked. */
static int lookups(const char* stimuli, const char* trials, const char* timeline, char* err, size_t cap) {
    char def[20000];
    snprintf(def, sizeof def,
             "{\"format\": \"ysp-experiment\", \"version\": 1, \"session\": {\"fields\": {\"group\": {\"type\": \"string\"}}},\n"
             " \"stimuli\": {%s},\n \"trials\": {%s},\n \"timeline\": [%s]}\n", stimuli, trials, timeline);
    g_warn[0] = 0;
    g_nwarn = 0;
    return ypt_check_lookups(def, strlen(def), table_of, NULL, warn_of, NULL, err, cap);
}

static void lk_ok(const char* label, const char* stimuli, const char* trials, const char* timeline, const char* warn) {
    char err[1024];
    int rc = lookups(stimuli, trials, timeline, err, sizeof err);
    CHECK(rc == 0 && !err[0], "%s: rc %d, '%s'", label, rc, err);
    CHECK(!strcmp(g_warn, warn), "%s: warnings\n'%s'\nwanted\n'%s'", label, g_warn, warn);
}

static void lk_refused(const char* label, const char* stimuli, const char* trials, const char* timeline, const char* says) {
    char err[1024];
    int rc = lookups(stimuli, trials, timeline, err, sizeof err);
    CHECK(rc == YPAK_ERR_FORMAT && strstr(err, says) != NULL, "%s: rc %d, '%s' (wanted '%s')", label, rc, err, says);
}

/* One stimulus "s" with this property, shown by template "t". */
#define LK_STIM(prop) "\"s\": {\"kind\": \"shape\", \"x\": " prop "}"
#define LK_T "\"t\": {\"events\": [{\"on\": \"s\"}]}"
#define LK_LEAF(extra) "{\"name\": \"main\", \"trial\": \"t\"" extra "}"

static void test_lookups(void) {
    /* the Stroop definition of experiment_format.md 2.14, with its table */
    static const char stroop_stim[] =
        "\"fix\": {\"kind\": \"shape\"},\n"
        "\"word\": {\"kind\": \"text\", \"font\": \"f.ttf\", \"size\": 72,\n"
        "  \"text\": {\"by\": \"$word\", \"map\": {\"red\": \"RED\", \"green\": \"GREEN\", \"blue\": \"BLUE\"}},\n"
        "  \"color\": {\"by\": \"$ink\", \"map\": {\"red\": [0.7, 0.04, 0.03], \"green\": [0.04, 0.42, 0.05], \"blue\": [0.07, 0.15, 0.85]}}},\n"
        "\"banner\": {\"kind\": \"text\", \"font\": \"f.ttf\", \"size\": 30,\n"
        "  \"text\": {\"by\": \"$practice\", \"map\": {\"true\": \"Practice.\", \"false\": \"Test.\"}}},\n"
        "\"keys\": {\"kind\": \"text\", \"font\": \"f.ttf\", \"size\": 22, \"text\": \"R red\"},\n"
        "\"feedback\": {\"kind\": \"text\", \"font\": \"f.ttf\", \"size\": 32,\n"
        "  \"text\": {\"by\": \"$outcome\", \"map\": {\"correct\": \"Correct\", \"wrong\": \"Wrong\", \"timeout\": \"Too slow\"}}}";
    static const char stroop_t[] =
        "\"stroop\": {\"events\": [{\"if\": \"$first_in_block\", \"steps\": [{\"on\": [\"banner\", \"keys\"]}, {\"wait_s\": 1.5},\n"
        "  {\"off\": [\"banner\", \"keys\"]}]}, {\"on\": \"fix\"}, {\"wait_s\": 0.5}, {\"off\": \"fix\"}, {\"on\": \"word\"}],\n"
        " \"response\": {\"role\": \"resp\", \"from\": \"word\", \"duration_s\": 2, \"choices\": [{\"key\": \"r\", \"name\": \"red\"},\n"
        "  {\"key\": \"g\", \"name\": \"green\"}, {\"key\": \"b\", \"name\": \"blue\"}], \"key_answer\": \"$answer\"},\n"
        " \"after\": [{\"off\": \"word\"}, {\"if\": \"$practice\", \"steps\": [{\"on\": \"feedback\"}, {\"wait_s\": 0.6}, {\"off\": \"feedback\"}]}],\n"
        " \"columns\": {\"feedback\": {\"by\": \"$practice\", \"map\": {\"true\": \"$outcome\", \"false\": \"\"}}}}";
    static const char stroop_node[] = "{\"name\": \"main\", \"timeline_variables\": \"stroop.pstb\", \"trial\": \"stroop\"}";
    char err[1024], buf[4096];
    int rc;
    make_table("stroop.pstb", "word,ink,congruent,answer,weight\nred,red,1,r,2\nred,green,0,g,1\nred,blue,0,b,1\ngreen,red,0,r,1\n"
                              "green,green,1,g,2\ngreen,blue,0,b,1\nblue,red,0,r,1\nblue,green,0,g,1\nblue,blue,1,b,2\n");
    lk_ok("Stroop", stroop_stim, stroop_t, stroop_node, "");

    /* a table column: each missing level named, sorted, in the leaf */
    lk_refused("column, one missing", LK_STIM("{\"by\": \"$word\", \"map\": {\"red\": 1, \"green\": 2}}"), LK_T,
               LK_LEAF(", \"timeline_variables\": \"stroop.pstb\""),
               "stimulus 's': 'x': the lookup's map has no key for \"blue\", which '$word' can take in node 'main'; a lookup needs a key for "
               "every level of its 'by'");
    lk_refused("column, two missing", LK_STIM("{\"by\": \"$word\", \"map\": {\"red\": 1}}"), LK_T,
               LK_LEAF(", \"timeline_variables\": \"stroop.pstb\""), "no key for \"blue\" and \"green\", which");
    lk_refused("a typo is a missing level", LK_STIM("{\"by\": \"$word\", \"map\": {\"red\": 1, \"gren\": 2, \"blue\": 3}}"), LK_T,
               LK_LEAF(", \"timeline_variables\": \"stroop.pstb\""), "no key for \"green\"");
    lk_ok("a key no level reaches", LK_STIM("{\"by\": \"$word\", \"map\": {\"red\": 1, \"green\": 2, \"blue\": 3, \"purple\": 4}}"), LK_T,
          LK_LEAF(", \"timeline_variables\": \"stroop.pstb\""),
          "stimulus 's': 'x': no level of '$word' reaches the map's key \"purple\"; a typo, or a level the trials no longer have?");
    lk_ok("two keys no level reaches", LK_STIM("{\"by\": \"$word\", \"map\": {\"red\": 1, \"green\": 2, \"blue\": 3, \"pink\": 4, \"aqua\": 5}}"),
          LK_T, LK_LEAF(", \"timeline_variables\": \"stroop.pstb\""),
          "stimulus 's': 'x': no level of '$word' reaches the map's keys \"aqua\" and \"pink\"; a typo, or a level the trials no longer have?");
    make_table("many.pstb", "k\na\nb\nc\nd\ne\nf\ng\nh\ni\nj\nk\n");
    lk_refused("more than 8 missing", LK_STIM("{\"by\": \"$k\", \"map\": {\"z\": 1}}"), LK_T, LK_LEAF(", \"timeline_variables\": \"many.pstb\""),
               "no key for \"a\", \"b\", \"c\", \"d\", \"e\", \"f\", \"g\", \"h\" and 3 more, which '$k'");
    /* numbers: the canonical text of the level's value */
    make_table("nums.pstb", "n,x\n1,0.50\n2,0.25\n007,1\n1,1.0\n");
    lk_ok("number and integer columns", LK_STIM("{\"by\": \"$x\", \"map\": {\"0.5\": 1, \"0.25\": 2, \"1\": 3}}"),
          "\"t\": {\"events\": [{\"on\": \"s\"}], \"columns\": {\"c\": {\"by\": \"$n\", \"map\": {\"1\": 1, \"2\": 2, \"7\": 3}}}}",
          LK_LEAF(", \"timeline_variables\": \"nums.pstb\""), "");
    lk_refused("a number key as written in the table", LK_STIM("{\"by\": \"$x\", \"map\": {\"0.50\": 1, \"0.25\": 2, \"1\": 3}}"), LK_T,
               LK_LEAF(", \"timeline_variables\": \"nums.pstb\""), "no key for \"0.5\"");
    lk_refused("an integer key with zeros", "", "\"t\": {\"columns\": {\"c\": {\"by\": \"$n\", \"map\": {\"1\": 1, \"2\": 2, \"007\": 3}}}}",
               LK_LEAF(", \"timeline_variables\": \"nums.pstb\""), "template 't': 'columns.c': the lookup's map has no key for \"7\"");

    /* vars: a choice, a value, a variable of a variable, a lookup */
    lk_refused("vars choice", LK_STIM("{\"by\": \"$side\", \"map\": {\"left\": -5}}"), LK_T,
               LK_LEAF(", \"vars\": {\"side\": {\"choice\": [\"left\", \"right\"]}}"), "no key for \"right\", which '$side' can take");
    lk_ok("vars choice, numbers and bools", LK_STIM("{\"by\": \"$v\", \"map\": {\"0.5\": 1, \"2\": 2, \"true\": 3, \"x\": 4}}"), LK_T,
          LK_LEAF(", \"vars\": {\"v\": {\"choice\": [0.50, 2.0, true, \"x\"]}}"), "");
    lk_refused("vars value", LK_STIM("{\"by\": \"$v\", \"map\": {\"Stop\": 1}}"), LK_T, LK_LEAF(", \"vars\": {\"v\": \"Go\"}"),
               "no key for \"Go\"");
    lk_ok("vars of vars, and $$", LK_STIM("{\"by\": \"$a\", \"map\": {\"$5\": 1, \"red\": 2, \"green\": 3, \"blue\": 4}}"), LK_T,
          LK_LEAF(", \"timeline_variables\": \"stroop.pstb\", \"vars\": {\"a\": {\"choice\": [\"$$5\", \"$b\"]}, \"b\": \"$word\"}"), "");
    lk_refused("a lookup var", LK_STIM("{\"by\": \"$a\", \"map\": {\"x\": 1}}"), LK_T,
               LK_LEAF(", \"vars\": {\"a\": {\"by\": \"$practice\", \"map\": {\"true\": \"x\", \"false\": \"y\"}}}"), "no key for \"y\"");
    lk_ok("vars named by and map are not a lookup", LK_STIM("1"), LK_T, LK_LEAF(", \"vars\": {\"by\": \"$practice\", \"map\": {\"a\": 1}}"), "");
    lk_ok("a key with a line break: one line", LK_STIM("{\"by\": \"$v\", \"map\": {\"x\": 1, \"a\\nb\": 2}}"), LK_T,
          LK_LEAF(", \"vars\": {\"v\": \"x\"}"),
          "stimulus 's': 'x': no level of '$v' reaches the map's key \"a?b\"; a typo, or a level the trials no longer have?");
    lk_refused("a level with a line break: one line", LK_STIM("{\"by\": \"$v\", \"map\": {\"x\": 1}}"), LK_T,
               LK_LEAF(", \"vars\": {\"v\": {\"choice\": [\"x\", \"a\\tb\"]}}"), "no key for \"a?b\", which");
    lk_refused("repeated levels named once", LK_STIM("{\"by\": \"$v\", \"map\": {}}"), LK_T,
               LK_LEAF(", \"vars\": {\"v\": {\"choice\": [\"b\", \"b\", \"a\", \"$w\"]}, \"w\": \"a\"}"),
               "the lookup's map has no key for \"a\" and \"b\", which '$v' can take");
    lk_ok("vars that name each other: the loader's to refuse", LK_STIM("{\"by\": \"$a\", \"map\": {\"x\": 1}}"), LK_T,
          LK_LEAF(", \"vars\": {\"a\": \"$b\", \"b\": \"$a\"}"), "");
    lk_ok("a literal by", LK_STIM("{\"by\": \"b\", \"map\": {\"a\": 1, \"b\": 2}}"), LK_T, LK_LEAF(""),
          "stimulus 's': 'x': no level of 'b' reaches the map's key \"a\"; a typo, or a level the trials no longer have?");

    /* built-ins with levels the definition fixes */
    lk_refused("practice", LK_STIM("{\"by\": \"$practice\", \"map\": {\"true\": 1}}"), LK_T, LK_LEAF(""), "no key for \"false\"");
    lk_refused("correct", LK_STIM("{\"by\": \"$correct\", \"map\": {\"false\": 1}}"), LK_T, LK_LEAF(""), "no key for \"true\"");
    lk_refused("outcome with a deadline", LK_STIM("{\"by\": \"$outcome\", \"map\": {\"correct\": 1, \"wrong\": 2}}"),
               "\"t\": {\"response\": {\"from\": \"s\", \"duration_s\": 1.5, \"choices\": [\"a\"]}}", LK_LEAF(""), "no key for \"timeout\"");
    lk_refused("outcome, a deadline from a variable", LK_STIM("{\"by\": \"$outcome\", \"map\": {\"correct\": 1, \"wrong\": 2}}"),
               "\"t\": {\"response\": {\"from\": \"s\", \"duration_s\": \"$d_s\", \"choices\": [\"a\"]}}", LK_LEAF(""), "no key for \"timeout\"");
    lk_ok("outcome without a deadline", LK_STIM("{\"by\": \"$outcome\", \"map\": {\"correct\": 1, \"wrong\": 2}}"),
          "\"t\": {\"response\": {\"from\": \"s\", \"choices\": [\"a\"]}}", LK_LEAF(""), "");
    lk_ok("a timeout key without a deadline", LK_STIM("{\"by\": \"$outcome\", \"map\": {\"correct\": 1, \"wrong\": 2, \"timeout\": 3}}"),
          "\"t\": {\"response\": {\"from\": \"s\", \"duration_s\": 0, \"choices\": [\"a\"]}}", LK_LEAF(""),
          "stimulus 's': 'x': no level of '$outcome' reaches the map's key \"timeout\"; a typo, or a level the trials no longer have?");
    lk_ok("outcome without a response: not known", LK_STIM("{\"by\": \"$outcome\", \"map\": {\"x\": 1}}"), LK_T, LK_LEAF(""), "");
    lk_refused("response: the choices' keys, not names", "",
               "\"t\": {\"response\": {\"choices\": [{\"key\": \"r\", \"name\": \"red\"}, \"left\"],\n"
               " \"trigger\": {\"role\": \"trig\", \"code\": {\"by\": \"$response\", \"map\": {\"red\": 101, \"left\": 102}}}}}",
               LK_LEAF(""), "template 't': 'response.trigger.code': the lookup's map has no key for \"r\", which '$response' can take");
    lk_ok("response", "", "\"t\": {\"response\": {\"choices\": [\"left\", \"right\"], \"trigger\": {\"role\": \"trig\", "
                          "\"code\": {\"by\": \"$response\", \"map\": {\"left\": 101, \"right\": 102}}}}}", LK_LEAF(""), "");
    lk_refused("method: the leaf's tracks", LK_STIM("{\"by\": \"$method\", \"map\": {\"stair\": 1}}"), LK_T,
               LK_LEAF(", \"tracks\": [\"stair\", \"quest\"]"), "no key for \"quest\"");
    lk_ok("not known at build: a session field, a jitter, a level",
          LK_STIM("[{\"by\": \"$group\", \"map\": {\"A\": 1}}, {\"by\": \"$soa_s\", \"map\": {}}, {\"by\": \"$level\", \"map\": {}},"
                  " {\"by\": \"$nothing\", \"map\": {}}, {\"by\": [1], \"map\": {}}]"), LK_T, LK_LEAF(""), "");

    /* scope: the leaf's tables, its template and the stimuli it names */
    make_table("colors.pstb", "word\ncyan\nmagenta\n");
    lk_ok("two leaves, one column name, two stimuli",
          "\"a\": {\"kind\": \"shape\", \"x\": {\"by\": \"$word\", \"map\": {\"red\": 1, \"green\": 2, \"blue\": 3}}},"
          " \"b\": {\"kind\": \"shape\", \"x\": {\"by\": \"$word\", \"map\": {\"cyan\": 1, \"magenta\": 2}}}",
          "\"ta\": {\"events\": [{\"on\": \"a\"}]}, \"tb\": {\"after\": [{\"if\": \"$practice\", \"steps\": [{\"to\": \"b\"}]}]}",
          "{\"name\": \"one\", \"timeline_variables\": \"stroop.pstb\", \"trial\": \"ta\"},"
          " {\"name\": \"two\", \"timeline_variables\": \"colors.pstb\", \"trial\": \"tb\"}", "");
    lk_refused("a stimulus of two leaves", "\"a\": {\"kind\": \"shape\", \"x\": {\"by\": \"$word\", \"map\": {\"red\": 1, \"green\": 2, \"blue\": 3}}}",
               "\"ta\": {\"record\": [\"a\"]}",
               "{\"name\": \"one\", \"timeline_variables\": \"stroop.pstb\", \"trial\": \"ta\"},"
               " {\"name\": \"blk\", \"timeline\": [{\"name\": \"two\", \"timeline_variables\": \"colors.pstb\", \"trial\": \"ta\"}]}",
               "stimulus 'a': 'x': the lookup's map has no key for \"cyan\" and \"magenta\", which '$word' can take in node 'blk/two'");
    lk_ok("the union of two leaves reaches every key",
          "\"a\": {\"kind\": \"shape\", \"x\": {\"by\": \"$v\", \"map\": {\"p\": 1, \"q\": 2}}}", "\"ta\": {\"response\": {\"from\": \"a\"}}",
          "{\"name\": \"one\", \"vars\": {\"v\": \"p\"}, \"trial\": \"ta\"}, {\"name\": \"two\", \"vars\": {\"v\": \"q\"}, \"trial\": \"ta\"}", "");
    lk_refused("a stimulus in an if's steps, in a list", LK_STIM("{\"by\": \"$practice\", \"map\": {\"true\": 1}}"),
               "\"t\": {\"events\": [{\"wait_s\": 1}, {\"if\": \"$warmup\", \"steps\": [{\"on\": [\"q\", \"s\"]}]}]}", LK_LEAF(""),
               "stimulus 's': 'x': the lookup's map has no key for \"false\"");
    lk_ok("the nearest vars", LK_STIM("{\"by\": \"$v\", \"map\": {\"q\": 1}}"), LK_T,
          "{\"name\": \"blk\", \"vars\": {\"v\": \"p\"}, \"timeline\": [" LK_LEAF(", \"vars\": {\"v\": \"q\"}") "]}", "");
    lk_ok("a stimulus no template names is not checked", LK_STIM("{\"by\": \"$practice\", \"map\": {}}"), "\"t\": {}", LK_LEAF(""), "");
    lk_ok("a container's table; the leaf's own comes first",
          LK_STIM("{\"by\": \"$word\", \"map\": {\"cyan\": 1, \"magenta\": 2}}"), LK_T,
          "{\"name\": \"blk\", \"timeline_variables\": \"stroop.pstb\", \"timeline\": [" LK_LEAF(", \"timeline_variables\": \"colors.pstb\"") "]}", "");
    lk_refused("a container's table is inherited", LK_STIM("{\"by\": \"$word\", \"map\": {\"cyan\": 1, \"magenta\": 2}}"), LK_T,
               "{\"name\": \"blk\", \"timeline_variables\": \"stroop.pstb\", \"timeline\": [" LK_LEAF("") "]}",
               "no key for \"blue\", \"green\" and \"red\", which '$word' can take in node 'blk/main'");
    lk_refused("a container's data", "", "\"t\": {}",
               "{\"name\": \"blk\", \"data\": {\"c\": {\"by\": \"$practice\", \"map\": {\"true\": 1}}}, \"timeline\": [" LK_LEAF("") "]}",
               "node 'blk': 'data.c': the lookup's map has no key for \"false\", which '$practice' can take in node 'blk/main'");
    lk_refused("a step's value", LK_STIM("0"), "\"t\": {\"events\": [{\"on\": \"s\"}, {\"set\": \"s\", \"props\": {\"x\": "
                                               "{\"by\": \"$practice\", \"map\": {\"false\": 1}}}}]}", LK_LEAF(""),
               "template 't': 'events[1].props.x': the lookup's map has no key for \"true\"");
    lk_refused("a lookup in a map value", LK_STIM("{\"by\": \"$practice\", \"map\": {\"true\": 1, \"false\": {\"by\": \"$correct\", \"map\": "
                                                  "{\"true\": 2}}}}"), LK_T, LK_LEAF(""),
               "stimulus 's': 'x.map.false': the lookup's map has no key for \"false\"");
    lk_refused("a missing table", LK_STIM("{\"by\": \"$word\", \"map\": {}}"), LK_T, LK_LEAF(", \"timeline_variables\": \"nothing.pstb\""),
               "node 'main': 'timeline_variables' is 'nothing.pstb', which is not a table entry of this pack");

    /* not a definition this check reads */
    {
        static const char* const other[] = { "not json", "{\"format\": \"other\"}", "{\"format\": \"ysp-experiment\", \"version\": 2}", "" };
        int i;
        for (i = 0; i < 4; i++) {
            rc = ypt_check_lookups(other[i], strlen(other[i]), table_of, NULL, warn_of, NULL, err, sizeof err);
            CHECK(rc == 0 && !err[0], "not checked %d: rc %d, %s", i, rc, err);
        }
        CHECK(ypt_check_lookups(NULL, 0, table_of, NULL, NULL, NULL, err, sizeof err) == YPAK_ERR_ARG, "NULL definition");
        CHECK(ypt_check_lookups("{}", 2, NULL, NULL, NULL, NULL, err, sizeof err) == YPAK_ERR_ARG, "NULL table function");
    }
    /* no warn function: the same refusals, no warnings computed */
    {
        static const char d[] = "{\"format\": \"ysp-experiment\", \"version\": 1, \"stimuli\": {\"s\": {\"x\": {\"by\": \"$practice\", "
                                "\"map\": {\"true\": 1, \"false\": 2, \"maybe\": 3}}}}, \"trials\": {\"t\": {\"record\": [\"s\"]}}, "
                                "\"timeline\": [{\"trial\": \"t\"}]}";
        rc = ypt_check_lookups(d, strlen(d), table_of, NULL, NULL, NULL, err, sizeof err);
        CHECK(rc == 0, "no warn function: rc %d, %s", rc, err);
    }
    /* the deepest tree the parser takes (JSON depth 64: 31 nodes), its
     * table at the top; then the work bound */
    {
        int i, at = snprintf(buf, sizeof buf, "{\"name\": \"a\", \"timeline_variables\": \"colors.pstb\", \"timeline\": [");
        for (i = 0; i < 29; i++) at += snprintf(buf + at, sizeof buf - (size_t)at, "{\"name\": \"a\", \"timeline\": [");
        at += snprintf(buf + at, sizeof buf - (size_t)at, "{\"name\": \"z\", \"trial\": \"t\"}");
        for (i = 0; i < 30; i++) at += snprintf(buf + at, sizeof buf - (size_t)at, "]}");
        lk_refused("31 nodes deep", LK_STIM("{\"by\": \"$word\", \"map\": {\"cyan\": 1}}"), LK_T, buf,
                   "no key for \"magenta\", which '$word' can take in node 'a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/a/z'");
        at = snprintf(buf, sizeof buf, ", \"vars\": {");
        for (i = 0; i < 7; i++)
            at += snprintf(buf + at, sizeof buf - (size_t)at, "\"v%d\": {\"choice\": [\"$v%d\", \"$v%d\", \"$v%d\", \"$v%d\", \"$v%d\", \"$v%d\", "
                           "\"$v%d\", \"$v%d\", \"$v%d\", \"$v%d\"]}, ", i, i + 1, i + 1, i + 1, i + 1, i + 1, i + 1, i + 1, i + 1, i + 1, i + 1);
        snprintf(buf + at, sizeof buf - (size_t)at, "\"v7\": \"end\"}}");
        {
            char node[8192];
            snprintf(node, sizeof node, "{\"name\": \"main\", \"trial\": \"t\"%s", buf);
            lk_refused("work bound", LK_STIM("{\"by\": \"$v0\", \"map\": {\"end\": 1}}"), LK_T, node, "steps to check");
        }
    }
}

int main(void) {
    test_stroop();
    test_shared();
    test_forms();
    test_opaque();
    test_refusals();
    test_lookups();
    printf("derive_test: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
