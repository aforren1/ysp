/* derive_test.c - self-checking test of the texts the pack tool takes from
 * an experiment definition (pack/derive.c, ypt_derive_texts; gap G9 of
 * docs/experiment_format.md). Every form that gives texts, every form that
 * is refused (each by its message), the order and uniqueness of the keys,
 * the canonical numbers, and the bounds on nesting and work. No layout and
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
static tab g_tabs[3];
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

int main(void) {
    test_stroop();
    test_shared();
    test_forms();
    test_opaque();
    test_refusals();
    printf("derive_test: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
