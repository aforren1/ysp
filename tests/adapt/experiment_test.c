/* experiment_test.c - tests of ysp/experiment.h, the experiment definition
 * loader. No framework: it returns 0 when every check passed and 1 after
 * printing each failure.
 *
 * What it covers:
 *   - the corpus tests/experiment/good: the worked examples of the format
 *     and examples/pack/stroop/, each compared with its golden dump; a
 *     round trip through the canonical bytes (the same form, the same ID,
 *     bytes canonical twice);
 *   - tests/experiment/bad: one file per refusal, compared with its .msg
 *     file: the first error's "line:column message";
 *   - every key of the format's table at its default: written out, it gives
 *     the same ID as left out, section by section;
 *   - the numbers (D2's open point), the host interface and the skip bits,
 *     the warnings, the arena (a full arena never crashes), the SHA-256
 *     against vectors, the table against itself.
 *
 *     gcc -std=c11 -Wall -Wextra -Iinclude tests/adapt/experiment_test.c -o t -lm
 *     YEXP_CORPUS=tests/experiment ./t
 * Set YEXP_WRITE_GOLDEN=1 to rewrite the golden files of good/ (then read the
 * diff: a golden file is the format's checked form).
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#define YSP_JSON_IMPLEMENTATION
#include "ysp/json.h"
#define YSP_EXPERIMENT_IMPLEMENTATION
#include "ysp/experiment.h"

#include "experiment_dump.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#else
#include <dirent.h>
#endif

static int g_failures, g_checks;

static void fail_(int line, const char* what, const char* detail) {
    g_failures++;
    fprintf(stderr, "experiment_test: FAIL line %d: %s%s%s\n", line, what, detail ? ": " : "", detail ? detail : "");
}
#define CHECK(c, what) do { g_checks++; if (!(c)) fail_(__LINE__, (what), NULL); } while (0)
#define CHECK2(c, what, d) do { g_checks++; if (!(c)) fail_(__LINE__, (what), (d)); } while (0)

static const char* g_corpus = "tests/experiment";

static char* read_file(const char* path, size_t* n) {
    FILE* f = fopen(path, "rb");
    char* p;
    long sz;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    p = (char*)malloc((size_t)sz + 1);
    if (!p) abort();
    if (fread(p, 1, (size_t)sz, f) != (size_t)sz) abort();
    fclose(f);
    p[sz] = 0;
    if (n) *n = (size_t)sz;
    return p;
}

#include "experiment_host.h"

/* --- corpus files ------------------------------------------------------- */

typedef struct name_list {
    char** v;
    int    n, cap;
} name_list;

static void list_add(name_list* l, const char* s) {
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 64;
        l->v = (char**)realloc(l->v, (size_t)l->cap * sizeof(char*));
        if (!l->v) abort();
    }
    l->v[l->n] = (char*)malloc(strlen(s) + 1);
    if (!l->v[l->n]) abort();
    strcpy(l->v[l->n], s);
    l->n++;
}

static int str_cmp(const void* a, const void* b) { return strcmp(*(char* const*)a, *(char* const*)b); }

/* Files of a folder with a given suffix, sorted. */
static void list_dir(const char* dir, const char* suffix, name_list* out) {
    size_t sl = strlen(suffix);
#ifdef _WIN32
    char pat[512];
    struct _finddata_t fd;
    intptr_t h;
    snprintf(pat, sizeof pat, "%s\\*%s", dir, suffix);
    h = _findfirst(pat, &fd);
    if (h != -1) {
        do { list_add(out, fd.name); } while (_findnext(h, &fd) == 0);
        _findclose(h);
    }
#else
    DIR* d = opendir(dir);
    struct dirent* e;
    if (!d) return;
    while ((e = readdir(d)) != NULL) {
        size_t n = strlen(e->d_name);
        if (n > sl && !strcmp(e->d_name + n - sl, suffix)) list_add(out, e->d_name);
    }
    closedir(d);
#endif
    if (out->n) qsort(out->v, (size_t)out->n, sizeof(char*), str_cmp);
    (void)sl;
}

static int g_tot_loaded;

/* One good file: golden, host and no host, round trip, canonical twice. */
static void check_good(const char* name) {
    char path[512], gold[512];
    size_t n, gn = 0;
    char* text;
    char* g;
    static yexp_report r, r2, r3;
    yjs_arena a, a2;
    yexp_def* d = NULL;
    yexp_def* d2 = NULL;
    char* dump1;
    char* dump2;
    const char* bytes = NULL;
    const char* bytes2 = NULL;
    size_t bn = 0, bn2 = 0;
    uint8_t id[32], id2[32];
    int rc;
    snprintf(path, sizeof path, "%s/good/%s", g_corpus, name);
    text = read_file(path, &n);
    CHECK2(text != NULL, "read good file", path);
    if (!text) return;
    yjs_arena_init_heap(&a, 0);
    rc = yexp_load(&a, text, n, &g_host, &d, &r);
    CHECK2(rc == YEXP_OK, "a good file loads with the host", r.n_diags ? r.diags[0].msg : name);
    if (rc != YEXP_OK) { free(text); yjs_arena_free(&a); return; }
    g_tot_loaded++;
    CHECK2(r.n_warnings == 0, "no warnings in the corpus", r.n_diags ? r.diags[0].msg : name);
    CHECK(r.skipped == 0, "the host ran every check");
    CHECK(r.run == (YEXP_CHECK_ENTRIES | YEXP_CHECK_COLUMNS | YEXP_CHECK_LEVELS | YEXP_CHECK_ROLES | YEXP_CHECK_PARAMS | YEXP_CHECK_RULES | YEXP_CHECK_OPS), "every check recorded as run");
    dump1 = xd_dump(d);
    /* the golden dump */
    snprintf(gold, sizeof gold, "%s/good/%s", g_corpus, name);
    { char* dot = strrchr(gold, '.'); if (dot) strcpy(dot, ".golden"); }
    if (getenv("YEXP_WRITE_GOLDEN")) {
        FILE* f = fopen(gold, "wb");
        if (!f) abort();
        fwrite(dump1, 1, strlen(dump1), f);
        fclose(f);
    }
    g = read_file(gold, &gn);
    CHECK2(g != NULL, "golden file", gold);
    if (g) {
        if (strcmp(g, dump1) != 0) {
            size_t i = 0;
            while (g[i] && dump1[i] && g[i] == dump1[i]) i++;
            fail_(__LINE__, "dump differs from the golden file; first difference at", &dump1[i > 40 ? i - 40 : 0]);
        }
        g_checks++;
        free(g);
    }
    /* no host: the same form, every host check skipped */
    {
        yjs_arena a3;
        yexp_def* d3 = NULL;
        char* dump3;
        yjs_arena_init_heap(&a3, 0);
        rc = yexp_load(&a3, text, n, NULL, &d3, &r3);
        CHECK2(rc == YEXP_OK, "a good file loads with no host", r3.n_diags ? r3.diags[0].msg : name);
        if (rc == YEXP_OK) {
            CHECK(r3.run == 0 && r3.skipped == (YEXP_CHECK_ENTRIES | YEXP_CHECK_COLUMNS | YEXP_CHECK_LEVELS | YEXP_CHECK_ROLES | YEXP_CHECK_PARAMS | YEXP_CHECK_RULES | YEXP_CHECK_OPS), "skip bits with no host");
            dump3 = xd_dump(d3);
            CHECK(!strcmp(dump3, dump1), "the form does not depend on the host");
            free(dump3);
        }
        yjs_arena_free(&a3);
    }
    /* round trip through the canonical bytes */
    rc = yexp_canonical(d, &a, &bytes, &bn, id);
    CHECK(rc == YEXP_OK, "canonical bytes");
    if (rc == YEXP_OK) {
        CHECK(bn > 0 && bytes[bn - 1] == '\n' && (bn < 2 || bytes[bn - 2] != '\n'), "one LF after the value");
        yjs_arena_init_heap(&a2, 0);
        rc = yexp_load(&a2, bytes, bn, &g_host, &d2, &r2);
        CHECK2(rc == YEXP_OK, "the canonical bytes load", r2.n_diags ? r2.diags[0].msg : name);
        if (rc == YEXP_OK) {
            dump2 = xd_dump(d2);
            if (strcmp(dump1, dump2) != 0) {
                size_t i = 0;
                while (dump1[i] && dump2[i] && dump1[i] == dump2[i]) i++;
                fail_(__LINE__, "the canonical bytes give another form; first difference at", &dump2[i > 60 ? i - 60 : 0]);
            }
            g_checks++;
            free(dump2);
            rc = yexp_canonical(d2, &a2, &bytes2, &bn2, id2);
            CHECK(rc == YEXP_OK, "canonical bytes of the canonical bytes");
            CHECK(bn == bn2 && !memcmp(bytes, bytes2, bn), "canonical bytes are stable");
            CHECK(!memcmp(id, id2, 32), "the ID is stable");
        }
        yjs_arena_free(&a2);
    }
    free(dump1);
    free(text);
    yjs_arena_free(&a);
}

static void run_corpus(void) {
    char dir[512];
    name_list good = { NULL, 0, 0 }, bad = { NULL, 0, 0 };
    int i;
    snprintf(dir, sizeof dir, "%s/good", g_corpus);
    list_dir(dir, ".json", &good);
    CHECK(good.n >= 5, "the good corpus exists");
    for (i = 0; i < good.n; i++) { check_good(good.v[i]); free(good.v[i]); }
    free(good.v);
    snprintf(dir, sizeof dir, "%s/bad", g_corpus);
    list_dir(dir, ".json", &bad);
    CHECK(bad.n >= 60, "the bad corpus exists");
    for (i = 0; i < bad.n; i++) {
        char path[512], msgp[512];
        size_t n = 0, mn = 0;
        char* text;
        char* want;
        static yexp_report r;
        yjs_arena a;
        yexp_def* d = NULL;
        int rc;
        snprintf(path, sizeof path, "%s/bad/%s", g_corpus, bad.v[i]);
        snprintf(msgp, sizeof msgp, "%s", path);
        { char* dot = strrchr(msgp, '.'); strcpy(dot, ".msg"); }
        text = read_file(path, &n);
        if (text && getenv("YEXP_WRITE_GOLDEN")) {
            char line[512];
            yjs_arena_init_heap(&a, 0);
            line[0] = 0;
            if (yexp_load(&a, text, n, &g_host, &d, &r) != YEXP_OK && r.n_diags >= 1)
                snprintf(line, sizeof line, "%u:%u: %s\n", r.diags[0].line, r.diags[0].column, r.diags[0].msg);
            else
                snprintf(line, sizeof line, "NOT REFUSED\n");
            {
                FILE* f = fopen(msgp, "wb");
                if (!f) abort();
                fwrite(line, 1, strlen(line), f);
                fclose(f);
            }
            yjs_arena_free(&a);
        }
        want = read_file(msgp, &mn);
        CHECK2(text && want, "bad file and its .msg", path);
        if (text && want) {
            char got[512];
            while (mn && (want[mn - 1] == '\n' || want[mn - 1] == '\r')) want[--mn] = 0;
            yjs_arena_init_heap(&a, 0);
            rc = yexp_load(&a, text, n, &g_host, &d, &r);
            CHECK2(rc == YEXP_ERR_REFUSED, "a bad file is refused", bad.v[i]);
            CHECK2(d == NULL, "no form for a refused file", bad.v[i]);
            CHECK2(r.n_errors >= 1 && r.n_diags >= 1 && r.diags[0].severity == YEXP_ERROR, "a refusal has an error", bad.v[i]);
            if (r.n_diags >= 1) {
                snprintf(got, sizeof got, "%u:%u: %s", r.diags[0].line, r.diags[0].column, r.diags[0].msg);
                if (strcmp(got, want) != 0) {
                    char d2[900];
                    snprintf(d2, sizeof d2, "%s:\n  want: %s\n  got:  %s", bad.v[i], want, got);
                    fail_(__LINE__, "message", d2);
                }
                g_checks++;
            }
            yjs_arena_free(&a);
        }
        free(text);
        free(want);
        free(bad.v[i]);
    }
    free(bad.v);
}

/* --- defaults: written out or left out, one ID -------------------------- */

static void id_of(const char* text, size_t n, uint8_t id[32]) {
    static yexp_report r;
    yjs_arena a;
    yexp_def* d = NULL;
    const char* b;
    size_t bn;
    int rc;
    yjs_arena_init_heap(&a, 0);
    rc = yexp_load(&a, text, n, NULL, &d, &r);
    if (rc != YEXP_OK) {
        fprintf(stderr, "  %s\n", r.n_diags ? r.diags[0].msg : "");
        fail_(__LINE__, "the document does not load", text);
    } else {
        yexp_canonical(d, &a, &b, &bn, id);
    }
    yjs_arena_free(&a);
}

/* base with @K@ replaced by extra (a JSON member with a trailing comma). */
static char* sub(const char* base, const char* extra) {
    const char* at = strstr(base, "@K@");
    size_t pre, el = strlen(extra);
    char* out;
    if (!at) abort();
    pre = (size_t)(at - base);
    out = (char*)malloc(strlen(base) + el + 2);
    if (!out) abort();
    memcpy(out, base, pre);
    memcpy(out + pre, extra, el);
    strcpy(out + pre + el, at + 3);
    return out;
}

/* Templates that hold every section's object, with the extra key placed. */
static const char* section_base(int sect, int kind, char* buf, size_t cap) {
    static const char* const stim_required[] = {
        "", "", "", "", "\"texture\": \"tex/a.ysptex\",", "\"font\": \"fonts/NotoSans-Regular.ttf\", \"size\": 10, \"text\": \"x\",",
        "\"artwork\": \"art/a.ysp\",", "", "\"audio\": \"snd/a.wav\",", "\"video\": \"vid/a.mp4\","
    };
    static const char* const kinds[] = { "shape", "grating", "gabor", "noise", "image", "text", "artwork", "rdk", "sound", "video" };
    const char* head = "{\"format\":\"ysp-experiment\",\"version\":1,";
    const char* tail_t = "\"trials\":{\"t\":{}},\"timeline\":[{\"name\":\"m\",\"trial\":\"t\"}]}";
    if (strcmp(yexp_section_name(sect), "stimulus") == 0)
        snprintf(buf, cap, "%s\"stimuli\":{\"s\":{\"kind\":\"%s\",%s @K@ \"note\":\"n\"}},%s", head, kinds[kind], stim_required[kind], tail_t);
    else if (strcmp(yexp_section_name(sect), "display") == 0)
        snprintf(buf, cap, "%s\"display\":{@K@ \"note\":\"n\"},%s", head, tail_t);
    else if (strcmp(yexp_section_name(sect), "role") == 0)
        snprintf(buf, cap, "%s\"devices\":{\"r\":{\"direction\":\"in\",\"family\":[\"keyboard\"],@K@ \"note\":\"n\"}},%s", head, tail_t);
    else if (strcmp(yexp_section_name(sect), "stair method") == 0)
        snprintf(buf, cap, "%s\"methods\":{\"m\":{\"type\":\"stair\",\"steps\":[0.1],@K@ \"note\":\"n\"}},%s", head, tail_t);
    else if (strcmp(yexp_section_name(sect), "quest method") == 0)
        snprintf(buf, cap, "%s\"methods\":{\"m\":{\"type\":\"quest\",\"stim\":[1],\"param\":[1],@K@ \"note\":\"n\"}},%s", head, tail_t);
    else if (strcmp(yexp_section_name(sect), "aep method") == 0)
        snprintf(buf, cap, "%s\"methods\":{\"m\":{\"type\":\"aep\",\"lo\":[0],\"hi\":[1],@K@ \"note\":\"n\"}},%s", head, tail_t);
    else if (strcmp(yexp_section_name(sect), "template") == 0)
        snprintf(buf, cap, "%s\"trials\":{\"t\":{@K@ \"note\":\"n\"}},\"timeline\":[{\"name\":\"m\",\"trial\":\"t\"}]}", head);
    else if (strcmp(yexp_section_name(sect), "response") == 0)
        snprintf(buf, cap, "%s\"devices\":{\"r\":{\"direction\":\"in\",\"family\":[\"k\"]}},\"stimuli\":{\"s\":{\"kind\":\"shape\"}},\"trials\":{\"t\":{\"response\":{\"role\":\"r\",\"from\":\"s\",@K@ \"note\":\"n\"}}},\"timeline\":[{\"name\":\"m\",\"trial\":\"t\"}]}", head);
    else if (strcmp(yexp_section_name(sect), "web") == 0)
        snprintf(buf, cap, "%s\"web\":{@K@ \"note\":\"n\"},%s", head, tail_t);
    else if (strcmp(yexp_section_name(sect), "file") == 0)
        snprintf(buf, cap, "{\"format\":\"ysp-experiment\",\"version\":1,@K@ %s", tail_t);
    else if (strcmp(yexp_section_name(sect), "node") == 0)
        snprintf(buf, cap, "%s\"trials\":{\"t\":{}},\"timeline\":[{\"name\":\"m\",\"trial\":\"t\",@K@ \"note\":\"n\"}]}", head);
    else
        return NULL;
    return buf;
}

static void run_defaults(void) {
    int n = 0, i, tested = 0;
    const yexp_row* rows = yexp_defaults(1, &n);
    CHECK(rows && n > 300, "the table");
    CHECK(yexp_defaults(2, &n) == NULL, "no table for a version this header does not read");
    n = 0;
    rows = yexp_defaults(1, &n);
    for (i = 0; i < n; i++) {
        const yexp_row* r = &rows[i];
        char buf[2048], extra[256];
        int kind, last_kind;
        if (!r->def) continue;
        /* a node's key for one kind of node, a stimulus key per kind */
        if (!strcmp(yexp_section_name(r->section), "node") && !strcmp(r->key, "repetitions")) continue;
        if (!strcmp(yexp_section_name(r->section), "stimulus")) last_kind = 9;
        else last_kind = 0;
        for (kind = 0; kind <= last_kind; kind++) {
            const char* base;
            char* a;
            char* b;
            uint8_t ida[32], idb[32];
            if (!strcmp(yexp_section_name(r->section), "stimulus") && !(r->kinds & (1u << kind))) continue;
            base = section_base(r->section, kind, buf, sizeof buf);
            if (!base) break;
            /* composites written at their default too: {} and [] */
            snprintf(extra, sizeof extra, "\"%s\": %s,", r->key, r->def);
            a = sub(base, extra);
            b = sub(base, "");
            id_of(a, strlen(a), ida);
            id_of(b, strlen(b), idb);
            if (memcmp(ida, idb, 32) != 0) {
                char d2[400];
                snprintf(d2, sizeof d2, "section %s key %s kind %d", yexp_section_name(r->section), r->key, kind);
                fail_(__LINE__, "a key at its default changes the ID", d2);
            }
            g_checks++;
            tested++;
            free(a);
            free(b);
        }
    }
    CHECK(tested > 400, "defaults exercised");
    printf("  defaults: %d keys written out against left out\n", tested);
}

/* --- numbers (D2) ------------------------------------------------------- */

static void one_id(const char* wait, uint8_t id[32]) {
    char doc[512];
    snprintf(doc, sizeof doc, "{\"format\":\"ysp-experiment\",\"version\":1,\"stimuli\":{\"s\":{\"kind\":\"gabor\",\"contrast\":%s}},"
                               "\"trials\":{\"t\":{}},\"timeline\":[{\"name\":\"m\",\"trial\":\"t\"}]}", wait);
    id_of(doc, strlen(doc), id);
}

static void wait_id(const char* wait, uint8_t id[32]) {
    char doc[512];
    snprintf(doc, sizeof doc, "{\"format\":\"ysp-experiment\",\"version\":1,"
                               "\"trials\":{\"t\":{\"events\":[{\"wait_s\":%s}]}},\"timeline\":[{\"name\":\"m\",\"trial\":\"t\"}]}", wait);
    id_of(doc, strlen(doc), id);
}

static void run_numbers(void) {
    uint8_t a[32], b[32];
    /* one real, two texts past its 17th digit: one ID (D2's open point) */
    one_id("0.1", a);
    one_id("0.1000000000000000055", b);
    CHECK(!memcmp(a, b, 32), "two texts of one double give one ID");
    one_id("0.5", a);
    one_id("5e-1", b);
    CHECK(!memcmp(a, b, 32), "spellings of a real");
    one_id("0.50", b);
    CHECK(!memcmp(a, b, 32), "trailing zeros");
    one_id("0.5", a);
    one_id("0.5000000000000001", b);
    CHECK(memcmp(a, b, 32) != 0, "two different doubles, two IDs");
    wait_id("0.25", a);
    wait_id("0.250000000", b);
    CHECK(!memcmp(a, b, 32), "a duration's spelling");
    wait_id("0.25", a);
    wait_id("0.250000001", b);
    CHECK(memcmp(a, b, 32) != 0, "a duration's nanoseconds differ");
    wait_id("1.0000000001", a);
    wait_id("1.00000000013", b);
    /* below a nanosecond: the exact decimal of the nanoseconds */
    CHECK(!memcmp(a, b, 32), "a duration rounds to nanoseconds");
    {
        static yexp_report r;
        yjs_arena ar;
        yexp_def* d = NULL;
        const char* doc = "{\"format\":\"ysp-experiment\",\"version\":1,\"stimuli\":{\"s\":{\"kind\":\"gabor\",\"contrast\":0.5,\"seed_never\":null}},\"trials\":{},\"timeline\":[]}";
        /* an unknown key is refused; the rest checks the failure path frees nothing */
        yjs_arena_init_heap(&ar, 0);
        CHECK(yexp_load(&ar, doc, strlen(doc), NULL, &d, &r) == YEXP_ERR_REFUSED, "an unknown key is refused");
        CHECK(d == NULL, "no form for a refusal");
        yjs_arena_free(&ar);
    }
    {
        /* integer text stays exact past 2^53 */
        static yexp_report r;
        yjs_arena ar;
        yexp_def* d = NULL;
        const char* doc = "{\"format\":\"ysp-experiment\",\"version\":1,\"stimuli\":{\"s\":{\"kind\":\"shape\",\"w\":9007199254740993}},\"trials\":{},\"timeline\":[]}";
        const char* bytes;
        size_t bn;
        yjs_arena_init_heap(&ar, 0);
        CHECK(yexp_load(&ar, doc, strlen(doc), NULL, &d, &r) == YEXP_OK, "a large integer loads");
        if (d) {
            yexp_canonical(d, &ar, &bytes, &bn, NULL);
            CHECK(strstr(bytes, "9007199254740993") != NULL, "integer text stays exact past 2^53");
        }
        yjs_arena_free(&ar);
    }
    {
        /* more than 19 significant digits cannot be read as a double */
        static yexp_report r;
        yjs_arena ar;
        yexp_def* d = NULL;
        const char* doc = "{\"format\":\"ysp-experiment\",\"version\":1,\"stimuli\":{\"s\":{\"kind\":\"shape\",\"w\":1.23456789012345678901}},\"trials\":{},\"timeline\":[]}";
        yjs_arena_init_heap(&ar, 0);
        CHECK(yexp_load(&ar, doc, strlen(doc), NULL, &d, &r) == YEXP_ERR_REFUSED, "too many digits refused");
        CHECK(r.n_diags == 1 && strstr(r.diags[0].msg, "19 significant") != NULL, "named so");
        yjs_arena_free(&ar);
    }
    {
        /* a canonical file that holds "note" keeps it: a note is part of the hash (D1) */
        uint8_t x[32], y[32];
        const char* with = "{\"format\":\"ysp-experiment\",\"version\":1,\"note\":\"hello\",\"trials\":{},\"timeline\":[]}";
        const char* without = "{\"format\":\"ysp-experiment\",\"version\":1,\"trials\":{},\"timeline\":[]}";
        id_of(with, strlen(with), x);
        id_of(without, strlen(without), y);
        CHECK(memcmp(x, y, 32) != 0, "a note is part of the hash");
    }
}

/* --- the host ----------------------------------------------------------- */

static void run_host(void) {
    /* a host with only some functions: those run, the rest are recorded as skipped */
    static yexp_report r;
    yjs_arena a;
    yexp_def* d = NULL;
    yexp_host h;
    const char* doc =
        "{\"format\":\"ysp-experiment\",\"version\":1,\"stimuli\":{\"s\":{\"kind\":\"shape\"}},"
        "\"trials\":{\"t\":{\"events\":[{\"on\":\"s\"}]}},"
        "\"timeline\":[{\"name\":\"m\",\"trial\":\"t\",\"timeline_variables\":\"nope.pstb\"}]}";
    memset(&h, 0, sizeof h);
    h.entry = st_entry;
    yjs_arena_init_heap(&a, 0);
    CHECK(yexp_load(&a, doc, strlen(doc), &h, &d, &r) == YEXP_ERR_REFUSED, "entry() refuses a table that is not in the pack");
    CHECK(r.n_diags >= 1 && strstr(r.diags[0].msg, "not a table entry of this pack") != NULL, "named");
    CHECK((r.run & YEXP_CHECK_ENTRIES) != 0, "entries recorded as run");
    CHECK((r.skipped & YEXP_CHECK_COLUMNS) != 0, "columns recorded as skipped");
    CHECK((r.skipped & YEXP_CHECK_RULES) != 0, "rules recorded as skipped");
    yjs_arena_free(&a);
    /* no column(): a table in scope makes an unknown name its column */
    {
        const char* doc2 =
            "{\"format\":\"ysp-experiment\",\"version\":1,\"stimuli\":{\"s\":{\"kind\":\"shape\",\"x\":\"$cond\"}},"
            "\"trials\":{\"t\":{\"events\":[{\"on\":\"s\"}]}},"
            "\"timeline\":[{\"name\":\"m\",\"trial\":\"t\",\"timeline_variables\":\"base.pstb\"}]}";
        yjs_arena_init_heap(&a, 0);
        CHECK(yexp_load(&a, doc2, strlen(doc2), NULL, &d, &r) == YEXP_OK, "no column(): a name may be a column");
        yjs_arena_free(&a);
        /* with no table in scope the same name is refused */
        doc2 =
            "{\"format\":\"ysp-experiment\",\"version\":1,\"stimuli\":{\"s\":{\"kind\":\"shape\",\"x\":\"$cond\"}},"
            "\"trials\":{\"t\":{\"events\":[{\"on\":\"s\"}]}},\"timeline\":[{\"name\":\"m\",\"trial\":\"t\"}]}";
        yjs_arena_init_heap(&a, 0);
        CHECK(yexp_load(&a, doc2, strlen(doc2), NULL, &d, &r) == YEXP_ERR_REFUSED, "no table: a name is unknown");
        yjs_arena_free(&a);
    }
    /* rules() and check_ops() ran */
    {
        static yexp_report rr;
        yexp_def* d2 = NULL;
        const char* doc3 = "{\"format\":\"ysp-experiment\",\"version\":1,\"trials\":{\"t\":{}},\"timeline\":[{\"name\":\"m\",\"trial\":\"t\",\"rules\":[\"reps 2\"]}]}";
        g_rules_calls = g_ops_calls = 0;
        yjs_arena_init_heap(&a, 0);
        CHECK(yexp_load(&a, doc3, strlen(doc3), &g_host, &d2, &rr) == YEXP_OK, "host checks pass");
        CHECK(g_rules_calls == 1 && g_ops_calls == 1, "rules() per leaf and check_ops() per template");
        if (d2) CHECK(!strcmp(d2->timeline[0].rules, "reps 2"), "the rules text");
        yjs_arena_free(&a);
    }
    /* the warning for a lookup key no level reaches */
    {
        static yexp_report rr;
        yexp_def* d2 = NULL;
        const char* doc4 =
            "{\"format\":\"ysp-experiment\",\"version\":1,\"stimuli\":{\"s\":{\"kind\":\"shape\",\"x\":{\"by\":\"$cond\",\"map\":{\"a\":1,\"b\":2,\"extra\":3}}}},"
            "\"trials\":{\"t\":{\"events\":[{\"on\":\"s\"}]}},"
            "\"timeline\":[{\"name\":\"m\",\"trial\":\"t\",\"timeline_variables\":\"base.pstb\"}]}";
        yjs_arena_init_heap(&a, 0);
        CHECK(yexp_load(&a, doc4, strlen(doc4), &g_host, &d2, &rr) == YEXP_OK, "a key no level reaches is not refused");
        CHECK(rr.n_warnings == 1, "it warns");
        CHECK(rr.n_diags == 1 && rr.diags[0].severity == YEXP_WARNING && strstr(rr.diags[0].msg, "extra") != NULL, "naming the key");
        yjs_arena_free(&a);
    }
    /* a warning for a long duration, and no refusal */
    {
        static yexp_report rr;
        yexp_def* d2 = NULL;
        const char* doc5 = "{\"format\":\"ysp-experiment\",\"version\":1,\"trials\":{\"t\":{\"events\":[{\"wait_s\":90}]}},\"timeline\":[{\"name\":\"m\",\"trial\":\"t\"}]}";
        yjs_arena_init_heap(&a, 0);
        CHECK(yexp_load(&a, doc5, strlen(doc5), NULL, &d2, &rr) == YEXP_OK, "a long duration loads");
        CHECK(rr.n_warnings == 1 && strstr(rr.diags[0].msg, "above 60") != NULL, "warned");
        yjs_arena_free(&a);
    }
    /* several errors are collected in one pass */
    {
        static yexp_report rr;
        yexp_def* d2 = NULL;
        const char* doc6 =
            "{\"format\":\"ysp-experiment\",\"version\":1,\"bad_a\":1,\"bad_b\":2,"
            "\"stimuli\":{\"s\":{\"kind\":\"shape\",\"bogus\":1}},\"trials\":{},\"timeline\":[]}";
        yjs_arena_init_heap(&a, 0);
        CHECK(yexp_load(&a, doc6, strlen(doc6), NULL, &d2, &rr) == YEXP_ERR_REFUSED, "refused");
        CHECK(rr.n_errors == 3, "three errors collected");
        yjs_arena_free(&a);
    }
    /* more errors than the report holds are counted, not kept */
    {
        static yexp_report rr;
        yexp_def* d2 = NULL;
        char doc7[8192];
        int at = 0, i;
        at += snprintf(doc7 + at, sizeof doc7 - (size_t)at, "{\"format\":\"ysp-experiment\",\"version\":1,");
        for (i = 0; i < 50; i++) at += snprintf(doc7 + at, sizeof doc7 - (size_t)at, "\"unknown_%d\":1,", i);
        at += snprintf(doc7 + at, sizeof doc7 - (size_t)at, "\"trials\":{},\"timeline\":[]}");
        yjs_arena_init_heap(&a, 0);
        CHECK(yexp_load(&a, doc7, (size_t)at, NULL, &d2, &rr) == YEXP_ERR_REFUSED, "refused");
        CHECK(rr.n_errors == 50 && rr.n_diags == YEXP_MAX_DIAGS, "counted past the report's capacity");
        yjs_arena_free(&a);
    }
}

/* --- arena -------------------------------------------------------------- */

static void run_arena(void) {
    /* every arena size from nothing up: a full arena never crashes */
    size_t n = 0;
    char path[512];
    char* text;
    static uint64_t mem[1 << 16];
    static yexp_report r;
    size_t cap;
    int ok = 0, nomem = 0;
    snprintf(path, sizeof path, "%s/good/2afc.json", g_corpus);
    text = read_file(path, &n);
    CHECK(text != NULL, "the 2afc example");
    if (!text) return;
    for (cap = 8; cap < sizeof mem; cap += cap / 8 + 1) {
        yjs_arena a;
        yexp_def* d = NULL;
        int rc;
        yjs_arena_init(&a, mem, cap);
        rc = yexp_load(&a, text, n, NULL, &d, &r);
        if (rc == YEXP_ERR_NOMEM) { CHECK(d == NULL, "no form when out of memory"); nomem++; }
        else if (rc == YEXP_OK) ok++;
        else CHECK2(0, "unexpected result in a small arena", r.n_diags ? r.diags[0].msg : "");
    }
    CHECK(ok > 0 && nomem > 0, "both outcomes seen");
    free(text);
}

/* --- SHA-256 ------------------------------------------------------------ */

static void hex(const uint8_t* b, char* out) {
    int i;
    for (i = 0; i < 32; i++) sprintf(out + 2 * i, "%02x", b[i]);
}

static void run_sha(void) {
    /* NIST vectors through the same code the experiment ID uses */
    static const struct { const char* in; const char* out; } v[] = {
        { "", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" },
        { "abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" },
        { "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1" },
    };
    size_t i;
    for (i = 0; i < sizeof v / sizeof v[0]; i++) {
        uint8_t h[32];
        char t[65];
        yexp__sha s;
        yexp__sha_init(&s);
        yexp__sha_update(&s, v[i].in, strlen(v[i].in));
        yexp__sha_final(&s, h);
        hex(h, t);
        CHECK2(!strcmp(t, v[i].out), "SHA-256 vector", v[i].in);
    }
    {
        /* a million 'a': the padding across block boundaries */
        uint8_t h[32];
        char t[65];
        yexp__sha s;
        int k;
        char block[1000];
        memset(block, 'a', sizeof block);
        yexp__sha_init(&s);
        for (k = 0; k < 1000; k++) yexp__sha_update(&s, block, sizeof block);
        yexp__sha_final(&s, h);
        hex(h, t);
        CHECK(!strcmp(t, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"), "SHA-256 of a million 'a'");
    }
}

/* --- the first bytes of the ID are stable: pin them ---------------------- */

int main(void) {
    const char* env = getenv("YEXP_CORPUS");
    if (env) g_corpus = env;
    CHECK(!strcmp(yexp_version(), YEXP_VERSION_STRING), "version string");
    run_sha();
    run_numbers();
    run_host();
    run_arena();
    run_defaults();
    run_corpus();
    if (g_failures) {
        fprintf(stderr, "experiment_test: %d failure(s) in %d checks\n", g_failures, g_checks);
        return 1;
    }
    printf("experiment_test: %d checks, %d files loaded: all passed\n", g_checks, g_tot_loaded);
    return 0;
}
