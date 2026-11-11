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

static int g_tot_loaded, g_tot_migrated;

/* A good file's warnings: none, or the lines of its .warn file in order
 * ("line:column: message"), as format 3.18 says for the visuomotor lookups. */
static void check_warnings(const char* name, const yexp_report* r) {
    char path[512], got[8192];
    char* want;
    size_t at = 0, wn = 0;
    int i;
    snprintf(path, sizeof path, "%s/good/%s", g_corpus, name);
    { char* dot = strrchr(path, '.'); if (dot) strcpy(dot, ".warn"); }
    got[0] = 0;
    for (i = 0; i < r->n_diags; i++)
        if (r->diags[i].severity == YEXP_WARNING)
            at += (size_t)snprintf(got + at, sizeof got - at, "%u:%u: %s\n", r->diags[i].line, r->diags[i].column, r->diags[i].msg);
    if (getenv("YEXP_WRITE_GOLDEN") && r->n_warnings) {
        FILE* f = fopen(path, "wb");
        if (!f) abort();
        fwrite(got, 1, strlen(got), f);
        fclose(f);
    }
    want = read_file(path, &wn);
    if (!want) {
        CHECK2(r->n_warnings == 0, "no warnings in the corpus", r->n_diags ? r->diags[0].msg : name);
        return;
    }
    CHECK2(!strcmp(want, got), "the warnings are the .warn file's", got);
    free(want);
}

/* Two canonical trees: a version 1 file's and its migration (format 3.2):
 * the same but "version" 2 and no stimulus `visible`. */
static int migrated_same(const yjs_value* v1, const yjs_value* v2, int depth, int in_stimuli) {
    size_t i, k;
    if (v1->type != v2->type) return 0;
    switch (v1->type) {
    case YJS_OBJECT:
        for (i = 0, k = 0; i < v1->n; i++) {
            const yjs_member* m = &v1->u.members[i];
            if (depth == 2 && in_stimuli && !strcmp(m->key, "visible")) continue;
            if (k >= v2->n || strcmp(m->key, v2->u.members[k].key)) return 0;
            if (depth == 0 && !strcmp(m->key, "version")) {
                if (!(m->value->flags & YJS_NUM_I64) || m->value->i != 1 || !(v2->u.members[k].value->flags & YJS_NUM_I64) || v2->u.members[k].value->i != 2) return 0;
            } else if (!migrated_same(m->value, v2->u.members[k].value, depth + 1, depth == 0 ? !strcmp(m->key, "stimuli") : in_stimuli)) {
                return 0;
            }
            k++;
        }
        return k == v2->n;
    case YJS_ARRAY:
        if (v1->n != v2->n) return 0;
        for (i = 0; i < v1->n; i++)
            if (!migrated_same(v1->u.items[i], v2->u.items[i], depth + 1, 0)) return 0;
        return 1;
    case YJS_NUMBER: case YJS_STRING:
        return v1->n == v2->n && !memcmp(v1->u.s, v2->u.s, v1->n);
    default:
        return 1;
    }
}

/* Format 3.2: a version 1 file's migrated bytes are its bytes with "version"
 * 2 and no `visible`; they load as version 2, give themselves back, and the
 * stored ID stays the version 1 bytes' (D12). */
static void check_migrated(const char* name, const yexp_def* d, yjs_arena* a) {
    const char* b1;
    const char* bm;
    const char* bm2;
    size_t n1, nm, nm2;
    uint8_t id1[32], idm[32], idm2[32];
    yjs_arena a2, a3;
    yjs_value* j1;
    yjs_value* jm;
    yjs_error je;
    yexp_def* d2 = NULL;
    static yexp_report r;
    CHECK2(yexp_canonical(d, a, &b1, &n1, id1) == YEXP_OK, "canonical", name);
    CHECK2(yexp_migrated(d, a, &bm, &nm, idm) == YEXP_OK, "migrated bytes", name);
    CHECK2(memcmp(id1, idm, 32) != 0, "the migrated bytes have their own SHA-256", name);
    yjs_arena_init_heap(&a3, 0);
    if (yjs_parse(&a3, b1, n1, NULL, &j1, &je) == YJS_OK && yjs_parse(&a3, bm, nm, NULL, &jm, &je) == YJS_OK)
        CHECK2(migrated_same(j1, jm, 0, 0), "migrated: version 2 and no visible, the rest the same", name);
    else
        CHECK2(0, "migrated bytes parse", name);
    yjs_arena_free(&a3);
    yjs_arena_init_heap(&a2, 0);
    if (yexp_load(&a2, bm, nm, &g_host, &d2, &r) != YEXP_OK) {
        fail_(__LINE__, "the migrated bytes load as version 2", r.n_diags ? r.diags[0].msg : name);
    } else {
        CHECK2(d2->format_version == 2, "the migrated file is version 2", name);
        CHECK2(yexp_canonical(d2, &a2, &bm2, &nm2, idm2) == YEXP_OK && nm2 == nm && !memcmp(bm, bm2, nm), "the migrated bytes are version 2's canonical bytes", name);
        CHECK2(!memcmp(idm, idm2, 32), "and their ID", name);
        g_tot_migrated++;
    }
    yjs_arena_free(&a2);
}

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
    check_warnings(name, &r);
    CHECK(r.skipped == 0, "the host ran every check");
    CHECK(r.run == (YEXP_CHECK_ENTRIES | YEXP_CHECK_COLUMNS | YEXP_CHECK_LEVELS | YEXP_CHECK_ROLES | YEXP_CHECK_PARAMS | YEXP_CHECK_RULES | YEXP_CHECK_OPS |
                    (d->n_sigroles ? YEXP_CHECK_SIGROLES : 0u)), "every check recorded as run");
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
            CHECK(r3.run == 0 && r3.skipped == (YEXP_CHECK_ENTRIES | YEXP_CHECK_COLUMNS | YEXP_CHECK_LEVELS | YEXP_CHECK_ROLES | YEXP_CHECK_PARAMS | YEXP_CHECK_RULES | YEXP_CHECK_OPS |
                                                (d3->n_sigroles ? YEXP_CHECK_SIGROLES : 0u)), "skip bits with no host");
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
    if (d->format_version == 1) check_migrated(name, d, &a);
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
        snprintf(buf, cap, "%s\"devices\":{\"r\":{\"direction\":\"in\",\"family\":[\"k\"]}},\"stimuli\":{\"s\":{\"kind\":\"shape\"}},\"trials\":{\"t\":{\"events\":[{\"on\":\"s\"}],\"response\":{\"role\":\"r\",\"from\":\"s\",@K@ \"note\":\"n\"}}},\"timeline\":[{\"name\":\"m\",\"trial\":\"t\"}]}", head);
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

static void id_roundtrip(const char* doc, uint8_t id[32]);
static void one_spelling(const char* const* docs, int n, const char* what);

/* Version 2's sections (format 3.17), and version 1's under version 2. */
static const char* section_base2(int sect, int kind, char* buf, size_t cap) {
    static const char* const ckinds[] = { "cursor", "region", "distance", "movement", "peak", "moving_target", "trace", "signal_lost" };
    static const char* const creq[] = {
        "\"role\":\"h\",", "\"of\":\"h\",\"radius\":1,", "\"of\":\"h\",", "\"of\":\"h\",\"onset\":1,", "\"of\":\"h\",",
        "\"x\":{\"amplitudes\":[1],\"freqs_hz\":[1],\"phases\":[0]},\"y\":{\"amplitudes\":[1],\"freqs_hz\":[1],\"phases\":[0]},",
        "\"of\":[\"h\"],", "\"of\":\"h\",\"after_s\":1,"
    };
    const char* name = yexp_section_name(sect);
    const char* head = "{\"format\":\"ysp-experiment\",\"version\":2,\"roles\":{\"h\":{\"type\":\"position2d\",\"units\":\"display_px\"}},\"stimuli\":{\"s\":{\"kind\":\"shape\"}},";
    const char* tl = "\"timeline\":[{\"name\":\"m\",\"trial\":\"t\"}]}";
    if (!strcmp(name, "signal role"))
        snprintf(buf, cap, "{\"format\":\"ysp-experiment\",\"version\":2,\"roles\":{\"h\":{\"type\":\"position2d\",\"units\":\"display_px\",@K@ \"note\":\"n\"}},\"trials\":{\"t\":{}},%s", tl);
    else if (!strcmp(name, "control"))
        snprintf(buf, cap, "%s\"trials\":{\"t\":{\"controls\":{\"k\":{\"control\":\"%s\",%s@K@ \"note\":\"n\"}%s},"
                           "\"states\":[{\"name\":\"a\",\"steps\":[{\"on\":\"s\"}]}]%s}},%s",
                 head, ckinds[kind], creq[kind], kind == 3 ? "" : "", kind == 3 ? ",\"response\":{\"choices\":[{\"signal\":\"k\"}],\"from\":\"s\"}" : "", tl);
    else if (!strcmp(name, "state"))
        snprintf(buf, cap, "%s\"trials\":{\"t\":{\"states\":[{\"name\":\"a\",@K@ \"note\":\"n\"}]}},%s", head, tl);
    else if (!strcmp(name, "over"))
        snprintf(buf, cap, "%s\"trials\":{\"t\":{\"states\":[{\"name\":\"a\",\"max_visits\":2,\"over\":{\"to\":\"end\",@K@ \"note\":\"n\"}}]}},%s", head, tl);
    else if (!strcmp(name, "exit"))
        snprintf(buf, cap, "%s\"trials\":{\"t\":{\"states\":[{\"name\":\"a\",\"exits\":[{\"after_s\":1,\"to\":\"end\",@K@ \"note\":\"n\"}]}]}},%s", head, tl);
    else if (!strcmp(name, "response with signal choices"))
        snprintf(buf, cap, "%s\"trials\":{\"t\":{\"controls\":{\"mv\":{\"control\":\"movement\",\"of\":\"h\",\"onset\":1}},"
                           "\"states\":[{\"name\":\"a\",\"steps\":[{\"on\":\"s\"}]}],\"response\":{\"choices\":[{\"signal\":\"mv\"}],\"from\":\"s\",@K@ \"note\":\"n\"}}},%s", head, tl);
    else if (!strcmp(name, "uniform"))
        snprintf(buf, cap, "%s\"trials\":{\"t\":{}},\"timeline\":[{\"name\":\"m\",\"trial\":\"t\",\"vars\":{\"u\":{\"uniform\":[0,1],@K@ \"note\":\"n\"}}}]}", head);
    else if (!strcmp(name, "walk2d"))
        snprintf(buf, cap, "%s\"trials\":{\"t\":{}},\"timeline\":[{\"name\":\"m\",\"trial\":\"t\",\"vars\":{\"w\":{\"walk2d\":{\"step\":1,\"box\":[[-2,-2],[2,2]],@K@ \"note\":\"n\"}}}}]}", head);
    else if (!strcmp(name, "template"))
        snprintf(buf, cap, "%s\"trials\":{\"t\":{@K@ \"note\":\"n\"}},%s", head, tl);
    else if (!strcmp(name, "file"))
        snprintf(buf, cap, "{\"format\":\"ysp-experiment\",\"version\":2,@K@ \"trials\":{\"t\":{}},%s", tl);
    else
        return NULL;
    return buf;
}

static void run_defaults(void) {
    int n = 0, i, tested = 0;
    const yexp_row* rows = yexp_defaults(1, &n);
    CHECK(rows && n > 300, "the table");
    CHECK(yexp_defaults(3, &n) == NULL && n == 0, "no table for a version this header does not read");
    CHECK(yexp_defaults(0, &n) == NULL && n == 0, "no table for version 0");
    n = 0;
    rows = yexp_defaults(1, &n);
    for (i = 0; i < n; i++) {
        const yexp_row* r = &rows[i];
        char buf[2048], extra[256];
        int kind, last_kind;
        if (!r->def) continue;
        /* a node's key for one kind of node, a stimulus key per kind */
        if (!strcmp(yexp_section_name(r->section), "node") && !strcmp(r->key, "repetitions")) continue;
        /* its default depends on the template: run_review() */
        if (!strcmp(yexp_section_name(r->section), "template") && !strcmp(r->key, "outcome")) continue;
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
    /* version 2: its own sections, and version 1's rows read under it */
    rows = yexp_defaults(2, &n);
    CHECK(rows && n > 400, "the version 2 table");
    tested = 0;
    for (i = 0; i < n; i++) {
        const yexp_row* r = &rows[i];
        char buf[2048], extra[256];
        int kind, last_kind;
        const char* sn = yexp_section_name(r->section);
        if (!r->def) continue;
        if (!strcmp(sn, "node") && !strcmp(r->key, "repetitions")) continue;
        if (!strcmp(sn, "template") && !strcmp(r->key, "outcome")) continue;
        /* events and after: a template with states refuses them written */
        last_kind = !strcmp(sn, "stimulus") ? 9 : !strcmp(sn, "control") ? 7 : 0;
        for (kind = 0; kind <= last_kind; kind++) {
            const char* base;
            char* a;
            char* b;
            uint8_t ida[32], idb[32];
            if ((!strcmp(sn, "stimulus") || !strcmp(sn, "control")) && !(r->kinds & (1u << kind))) continue;
            if (!strcmp(sn, "stimulus") && (r->flags & 0x10u)) continue;   /* visible: a step's property in version 2 */
            base = section_base2(r->section, kind, buf, sizeof buf);
            if (!base) {
                char* q;
                base = section_base(r->section, kind, buf, sizeof buf);
                if (!base) break;
                q = strstr(buf, "\"version\":1");
                if (q) q[10] = '2';
            }
            snprintf(extra, sizeof extra, "\"%s\": %s,", r->key, r->def);
            a = sub(base, extra);
            b = sub(base, "");
            id_of(a, strlen(a), ida);
            id_of(b, strlen(b), idb);
            if (memcmp(ida, idb, 32) != 0) {
                char d2[400];
                snprintf(d2, sizeof d2, "version 2 section %s key %s kind %d", sn, r->key, kind);
                fail_(__LINE__, "a key at its default changes the ID", d2);
            }
            g_checks++;
            tested++;
            free(a);
            free(b);
        }
    }
    CHECK(tested > 450, "version 2 defaults exercised");
    printf("  defaults, version 2: %d keys written out against left out\n", tested);
    {
        /* the template's outcome: 1 with states and no key_answer (3.17) */
#define OC2(x) "{\"format\":\"ysp-experiment\",\"version\":2,\"trials\":{\"t\":{" x "\"states\":[{\"name\":\"a\"}]}},\"timeline\":[{\"name\":\"m\",\"trial\":\"t\"}]}"
        static const char* const a[] = { OC2(""), OC2("\"outcome\":1,"), OC2("\"outcome\":1.0,") };
        uint8_t x[32], y[32];
        one_spelling(a, 3, "1 is the outcome's default with states");
        id_roundtrip(OC2(""), x);
        id_roundtrip(OC2("\"outcome\":\"correct\","), y);
        CHECK(memcmp(x, y, 32) != 0, "\"correct\" with states and no key_answer is not the default");
    }
}

/* --- the defaults table against the format page (3.17, D41 item 5) ----- */

/* One object name of 3.17 as a section and a kinds mask. */
static int page_object(const char* obj, const char** sect, unsigned* kinds) {
    static const struct { const char* obj; const char* sect; unsigned kinds; } M[] = {
        { "file", "file", 0xffffu }, { "signal role", "signal role", 0xffffu }, { "template", "template", 0xffffu },
        { "state", "state", 0xffffu }, { "over", "over", 0xffffu }, { "exit", "exit", 0x1u }, { "shared exit", "exit", 0x2u },
        { "control", "control", 0xffu }, { "cursor", "control", 0x01u }, { "region", "control", 0x02u }, { "distance", "control", 0x04u },
        { "movement", "control", 0x08u }, { "peak", "control", 0x10u }, { "moving_target", "control", 0x20u }, { "trace", "control", 0x40u },
        { "signal_lost", "control", 0x80u }, { "response with signal choices", "response with signal choices", 0xffffu },
        { "step until", "step", 0x0800u }, { "step recenter", "step", 0x1000u }, { "step flag", "step", 0x2000u },
        { "uniform", "uniform", 0xffffu }, { "walk2d", "walk2d", 0xffffu },
    };
    size_t i;
    for (i = 0; i < sizeof M / sizeof M[0]; i++)
        if (!strcmp(M[i].obj, obj)) { *sect = M[i].sect; *kinds = M[i].kinds; return 1; }
    return 0;
}

static void trim(char* s) {
    size_t n = strlen(s), a = 0;
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = 0;
    while (s[a] == ' ') a++;
    memmove(s, s + a, n - a + 1);
}

static void strip_ticks(char* s) {
    char* o = s;
    for (; *s; s++) if (*s != '`') *o++ = *s;
    *o = 0;
}

/* Splits at ", " outside backticks, brackets and parentheses. */
static int split_list(char* s, char** out, int cap) {
    int n = 0, depth = 0, tick = 0;
    char* start = s;
    for (; *s; s++) {
        if (*s == '`') tick = !tick;
        else if (!tick && (*s == '(' || *s == '[' || *s == '{')) depth++;
        else if (!tick && (*s == ')' || *s == ']' || *s == '}')) depth--;
        else if (!tick && !depth && *s == ',' && s[1] == ' ') {
            *s = 0;
            if (n < cap) out[n++] = start;
            start = s + 2;
            s++;
        }
    }
    if (n < cap) out[n++] = start;
    return n;
}

/* A default cell: 1 required, 2 none, 3 a JSON value in json[]. */
static int page_default(const char* cell, char* json, size_t cap) {
    const char* t = strchr(cell, '`');
    if (!strcmp(cell, "required")) return 1;
    if (!strcmp(cell, "exactly one required")) return 2;
    if (!t || !strncmp(cell, "none", 4)) return 2;   /* "none", "none (...)", "the trial's first frame", ... */
    {
        const char* e = strchr(t + 1, '`');
        size_t n = e ? (size_t)(e - t - 1) : 0;
        if (!e || n + 1 > cap) return 0;
        memcpy(json, t + 1, n);
        json[n] = 0;
    }
    return 3;
}

static int json_equal_text(const char* a, const char* b) {
    yjs_arena ar;
    yjs_value* x;
    yjs_value* y;
    yjs_error e;
    char ba[512], bb[512];
    int ok = 0;
    yjs_arena_init_heap(&ar, 0);
    if (yjs_parse(&ar, a, strlen(a), NULL, &x, &e) == YJS_OK && yjs_parse(&ar, b, strlen(b), NULL, &y, &e) == YJS_OK) {
        size_t na = yjs_write_mem(x, YJS_WRITE_COMPACT, ba, sizeof ba), nb = yjs_write_mem(y, YJS_WRITE_COMPACT, bb, sizeof bb);
        ok = na == nb && na < sizeof ba && !memcmp(ba, bb, na);
    }
    yjs_arena_free(&ar);
    return ok;
}

static const yexp_row* find_row(const yexp_row* rows, int n, const char* sect, unsigned kinds, const char* key) {
    int i;
    for (i = 0; i < n; i++)
        if (!strcmp(yexp_section_name(rows[i].section), sect) && (rows[i].kinds & kinds) && !strcmp(rows[i].key, key)) return &rows[i];
    return NULL;
}

static void run_defaults_page(void) {
    char path[600];
    char* doc;
    char* p;
    char* end;
    int n2 = 0, n1 = 0, i, checked = 0, k;
    const yexp_row* rows2 = yexp_defaults(2, &n2);
    const yexp_row* rows1 = yexp_defaults(1, &n1);
    uint8_t* seen = (uint8_t*)calloc((size_t)n2 + 1, 1);
    /* rows of version 2 that 3.17 does not list, each with its reason */
    static const struct { const char* sect; unsigned kinds; const char* key; } allow[] = {
        { "uniform", 0xffffu, "uniform" },                                  /* the form's own key, [lo, hi] (3.14) */
        { "step", 0x0800u, "until" }, { "step", 0x1000u, "recenter" }, { "step", 0x2000u, "flag" },   /* a step's verb */
        { "response with signal choices", 0xffffu, "duration_s" },          /* "as 2.6" (3.13) */
        { "response with signal choices", 0xffffu, "minimum_valid_rt_s" },
        { "response with signal choices", 0xffffu, "max_responses" },
        { "template", 0x0001u, "outcome" },                                 /* version 1's default, kept (3.17 head) */
        { "over", 0xffffu, "outcome" },   /* 3.11: "outcome none"; 3.17 lists only over's to (finding F15) */
    };
    snprintf(path, sizeof path, "%s/../../docs/experiment_format.md", g_corpus);
    doc = read_file(path, NULL);
    CHECK2(doc != NULL, "the format page", path);
    if (!doc || !seen) { free(doc); free(seen); return; }
    p = strstr(doc, "### 3.17 Defaults");
    end = p ? strstr(p, "### 3.18") : NULL;
    CHECK(p && end, "3.17 in the page");
    if (!p || !end) { free(doc); free(seen); return; }
    *end = 0;
    p = strstr(p, "|---|");
    while (p && (p = strchr(p, '\n')) != NULL) {
        char line[512], *cells[4], *keys[8], *defs[8];
        char* nl;
        int nc = 0, nk, nd, q;
        const char* sect;
        unsigned kinds;
        p++;
        if (*p != '|') break;
        nl = strchr(p, '\n');
        snprintf(line, sizeof line, "%.*s", (int)(nl ? nl - p : (long)strlen(p)), p);
        {
            char* c = line + 1;
            char* bar;
            while (nc < 4 && (bar = strchr(c, '|')) != NULL) { *bar = 0; cells[nc++] = c; c = bar + 1; }
        }
        if (nc != 3) { CHECK2(0, "a 3.17 row has three cells", line); continue; }
        for (q = 0; q < 3; q++) trim(cells[q]);
        strip_ticks(cells[0]);
        if (!page_object(cells[0], &sect, &kinds)) { CHECK2(0, "a 3.17 object the test knows", cells[0]); continue; }
        nd = split_list(cells[2], defs, 8);
        {
            /* keys: "a", "b", "x with y", "outcome with states and no key_answer" */
            char* tmp[8];
            int nt = split_list(cells[1], tmp, 8);
            nk = 0;
            for (q = 0; q < nt; q++) {
                char* w;
                strip_ticks(tmp[q]);
                trim(tmp[q]);
                if (strstr(tmp[q], " with states")) {
                    *strstr(tmp[q], " with states") = 0;
                    kinds = 0x2u;   /* the template's second outcome row */
                    keys[nk++] = tmp[q];
                } else if ((w = strstr(tmp[q], " with ")) != NULL) {
                    *w = 0;
                    keys[nk++] = tmp[q];
                    keys[nk++] = w + 6;
                } else {
                    keys[nk++] = tmp[q];
                }
            }
        }
        for (q = 0; q < nk; q++) {
            char json[200];
            const char* d = defs[nd == 1 ? 0 : q < nd ? q : nd - 1];
            int want;
            const yexp_row* r = find_row(rows2, n2, sect, kinds, keys[q]);
            char what[200];
            snprintf(what, sizeof what, "%.80s %.80s", cells[0], keys[q]);
            if (nd != 1 && nd != nk && strcmp(cells[2], "exactly one required")) CHECK2(0, "one default per key, or one for all", what);
            CHECK2(r != NULL, "3.17's row is in yexp_defaults(2)", what);
            if (!r) continue;
            seen[r - rows2] = 1;
            want = page_default(d, json, sizeof json);
            if (want == 1) CHECK2(yexp_row_required(r), "required in the page and in the table", what);
            else if (want == 2) CHECK2(!yexp_row_required(r) && !r->def, "no default in the page and in the table", what);
            else CHECK2(want == 3 && r->def && json_equal_text(json, r->def), "the page's default is the table's", what);
            checked++;
        }
    }
    /* every row version 2 adds is in 3.17, or in the list above */
    for (i = 0; i < n2; i++) {
        const yexp_row* r = &rows2[i];
        const yexp_row* r1 = find_row(rows1, n1, yexp_section_name(r->section), r->kinds, r->key);
        bool listed = seen[i] != 0;
        if (r1 && r1->kinds == r->kinds) {
            /* version 1's row: the same in version 2, but for part 3's changes */
            bool same = r1->type == r->type && ((r1->def == NULL) == (r->def == NULL)) && (!r->def || !strcmp(r1->def, r->def)) &&
                        r1->flags == r->flags && ((r1->values == NULL) == (r->values == NULL)) && (!r->values || !strcmp(r1->values, r->values));
            if (!same) {
                bool known = (!strcmp(r->key, "visible") && r->flags == (r1->flags | 0x10u)) ||
                             (!strcmp(r->key, "units") && !strcmp(r->values, "px=0,deg=1,cm=2"));
                CHECK2(known, "a version 1 row changed only as part 3 says", r->key);
            }
            continue;
        }
        for (k = 0; k < (int)(sizeof allow / sizeof allow[0]) && !listed; k++)
            if (!strcmp(allow[k].sect, yexp_section_name(r->section)) && allow[k].kinds == r->kinds && !strcmp(allow[k].key, r->key)) listed = true;
        CHECK2(listed, "a version 2 row is in 3.17", r->key);
    }
    /* and the other way: version 1's rows are all in version 2 */
    for (i = 0; i < n1; i++) {
        const yexp_row* r1 = &rows1[i];
        bool found = find_row(rows2, n2, yexp_section_name(r1->section), r1->kinds, r1->key) != NULL;
        CHECK2(found, "a version 1 row is in version 2", r1->key);
    }
    CHECK(checked >= 70, "3.17's rows read");
    printf("  defaults table: %d keys of 3.17 against yexp_defaults(2)\n", checked);
    free(seen);
    free(doc);
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
        /* a 63-byte name loads; 64 bytes is the corpus file name_64_bytes */
        uint8_t x[32];
        const char* doc = "{\"format\":\"ysp-experiment\",\"version\":1,\"stimuli\":{\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\":{\"kind\":\"shape\"}},\"trials\":{},\"timeline\":[]}";
        id_of(doc, strlen(doc), x);
    }
    {
        /* -0 is 0: one spelling, one value (D2) */
        static yexp_report r;
        yjs_arena ar;
        yexp_def* d = NULL;
        const char* doc = "{\"format\":\"ysp-experiment\",\"version\":1,\"display\":{\"background\":[-0,-0.0,0.5]},\"trials\":{},\"timeline\":[]}";
        yjs_arena_init_heap(&ar, 0);
        CHECK(yexp_load(&ar, doc, strlen(doc), NULL, &d, &r) == YEXP_OK, "-0 loads");
        if (d) CHECK(1.0 / d->background[0] > 0 && 1.0 / d->background[1] > 0, "the form holds 0, not -0");
        yjs_arena_free(&ar);
    }
    {
        /* a boolean's true without rules that make it: no warning, no refusal (pack.md 16) */
        static yexp_report r;
        yjs_arena ar;
        yexp_def* d = NULL;
        const char* doc =
            "{\"format\":\"ysp-experiment\",\"version\":1,\"stimuli\":{\"s\":{\"kind\":\"shape\",\"x\":{\"by\":\"$practice\",\"map\":{\"false\":1}}}},"
            "\"trials\":{\"t\":{\"events\":[{\"on\":\"s\"}]}},\"timeline\":[{\"name\":\"m\",\"trial\":\"t\"}]}";
        yjs_arena_init_heap(&ar, 0);
        CHECK(yexp_load(&ar, doc, strlen(doc), &g_host, &d, &r) == YEXP_OK && r.n_warnings == 0, "$practice has only 'false' when the rules cannot make it true");
        yjs_arena_free(&ar);
    }
    {
        /* a by whose levels are not all known is not checked, and warns about nothing */
        static yexp_report r;
        yjs_arena ar;
        yexp_def* d = NULL;
        const char* doc =
            "{\"format\":\"ysp-experiment\",\"version\":1,\"stimuli\":{\"s\":{\"kind\":\"shape\",\"x\":{\"by\":\"$participant\",\"map\":{\"3\":1}}}},"
            "\"trials\":{\"t\":{\"events\":[{\"on\":\"s\"}]}},\"timeline\":[{\"name\":\"m\",\"trial\":\"t\"}]}";
        yjs_arena_init_heap(&ar, 0);
        CHECK(yexp_load(&ar, doc, strlen(doc), &g_host, &d, &r) == YEXP_OK && r.n_warnings == 0, "a session field's levels are not known at load");
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

/* --- the review's list -------------------------------------------------- */

#define XH "{\"format\":\"ysp-experiment\",\"version\":1,"
#define XT "\"timeline\":[{\"name\":\"m\",\"trial\":\"t\"}]}"

static int load_rc(const char* doc, const yexp_host* h, yexp_report* r) {
    yjs_arena a;
    yexp_def* d = NULL;
    int rc;
    yjs_arena_init_heap(&a, 0);
    rc = yexp_load(&a, doc, strlen(doc), h, &d, r);
    yjs_arena_free(&a);
    return rc;
}

/* A column "f" of 0 and 1 whose buffer never gets a NUL. */
static int flag_column(void* ctx, const char* table, int i, yexp_column* o) {
    (void)ctx; (void)table;
    if (i == 0) { o->name = "f"; o->type = YEXP_COL_INTEGER; return 1; }
    return 0;
}
static int flag_level(void* ctx, const char* table, int col, int l, char* b, size_t cap) {
    (void)ctx; (void)table; (void)col;
    if (l > 1) return -1;
    memset(b, 'z', cap);
    b[0] = l ? '1' : '0';
    return 1;
}

/* The ID, and that the canonical bytes give back the same form and ID. */
static void id_roundtrip(const char* doc, uint8_t id[32]) {
    static yexp_report r, r2;
    yjs_arena a, a2;
    yexp_def* d = NULL;
    yexp_def* d2 = NULL;
    const char* b;
    const char* b2;
    size_t n, n2;
    uint8_t id2[32];
    char* s1;
    char* s2;
    memset(id, 0, 32);
    yjs_arena_init_heap(&a, 0);
    yjs_arena_init_heap(&a2, 0);
    if (yexp_load(&a, doc, strlen(doc), NULL, &d, &r) != YEXP_OK) {
        fail_(__LINE__, "does not load", r.n_diags ? r.diags[0].msg : doc);
    } else {
        g_checks++;
        yexp_canonical(d, &a, &b, &n, id);
        if (yexp_load(&a2, b, n, NULL, &d2, &r2) != YEXP_OK) {
            fail_(__LINE__, "the canonical bytes do not load", b);
        } else {
            s1 = xd_dump(d);
            s2 = xd_dump(d2);
            CHECK2(!strcmp(s1, s2), "the canonical bytes give the same form", doc);
            yexp_canonical(d2, &a2, &b2, &n2, id2);
            CHECK2(n == n2 && !memcmp(id, id2, 32), "the canonical bytes are stable", doc);
            free(s1);
            free(s2);
        }
    }
    yjs_arena_free(&a);
    yjs_arena_free(&a2);
}

static void one_spelling(const char* const* docs, int n, const char* what) {
    uint8_t a[32], b[32];
    int i;
    id_roundtrip(docs[0], a);
    for (i = 1; i < n; i++) {
        id_roundtrip(docs[i], b);
        CHECK2(!memcmp(a, b, 32), what, docs[i]);
    }
}

#define W_DOC(x) XH "\"stimuli\":{\"s\":{\"kind\":\"shape\",\"w\":" x "}},\"trials\":{\"t\":{}}," XT

static void run_review(void) {
    static yexp_report r;
    /* every value form, every wrong type of note */
    {
        static const char* const forms[] = {
            "{\"michelson\":0.5,\"note\":%s}", "{\"by\":\"$block\",\"map\":{\"1\":0.5},\"note\":%s}",
            "{\"gamut\":\"s.contrast\",\"note\":%s}"
        };
        static const char* const notes[] = { "true", "false", "null", "5", "[1,2,3]", "{\"a\":1}" };
        size_t f, k;
        for (f = 0; f < sizeof forms / sizeof forms[0]; f++)
            for (k = 0; k < sizeof notes / sizeof notes[0]; k++) {
                char v[160], doc[512];
                snprintf(v, sizeof v, forms[f], notes[k]);
                snprintf(doc, sizeof doc, XH "\"stimuli\":{\"s\":{\"kind\":\"gabor\",\"contrast\":%s}},\"trials\":{\"t\":{}}," XT, v);
                CHECK2(load_rc(doc, NULL, &r) == YEXP_ERR_REFUSED && r.n_diags && strstr(r.diags[0].msg, "'note' is a string"), "a non-string note on a value form", doc);
            }
        for (k = 0; k < sizeof notes / sizeof notes[0]; k++) {
            char doc[512];
            snprintf(doc, sizeof doc, XH "\"methods\":{\"q\":{\"type\":\"quest\",\"stim\":[{\"linspace\":[0,1,3],\"note\":%s}],\"param\":[1]}},\"trials\":{\"t\":{}}," XT, notes[k]);
            CHECK2(load_rc(doc, NULL, &r) == YEXP_ERR_REFUSED && r.n_diags && strstr(r.diags[0].msg, "'note' is a string"), "a non-string note on a linspace", doc);
            snprintf(doc, sizeof doc, XH "\"trials\":{\"t\":{}},\"timeline\":[{\"name\":\"m\",\"trial\":\"t\",\"vars\":{\"a\":{\"choice\":[1],\"note\":%s}}}]}", notes[k]);
            CHECK2(load_rc(doc, NULL, &r) == YEXP_ERR_REFUSED && r.n_diags && strstr(r.diags[0].msg, "'note' is a string"), "a non-string note on a choice", doc);
        }
    }
    /* one value, one form and ID */
    {
        static const char* const a[] = { W_DOC("9007199254740993"), W_DOC("9007199254740993.0"), W_DOC("9.007199254740993e15"), W_DOC("9007199254740993e0") };
        static const char* const b[] = { W_DOC("9007199254740992"), W_DOC("9007199254740992.0"), W_DOC("9.007199254740992E15") };
        static const char* const c[] = { W_DOC("9223372036854775807"), W_DOC("9223372036854775807.000") };
        static const char* const d[] = { W_DOC("-9223372036854775808"), W_DOC("-9223372036854775808.0") };
        static const char* const e[] = { W_DOC("100"), W_DOC("1e2"), W_DOC("100.0"), W_DOC("1.0E+2") };
        uint8_t x[32];
        one_spelling(a, 4, "one integer past 2^53, four spellings");
        one_spelling(b, 3, "2^53, three spellings");
        one_spelling(c, 2, "int64's largest");
        one_spelling(d, 2, "int64's smallest");
        one_spelling(e, 4, "a small integer");
        {
            /* a value past int64 whose double is int64's smallest is that integer */
            static const char* const f[] = { W_DOC("-9223372036854775808"), W_DOC("-9.22337203685477581e18") };
            one_spelling(f, 2, "a whole double at int64's low end");
        }
        /* a real whose double is whole, and one past int64: stable forms */
        id_roundtrip(W_DOC("9007199254740993.5"), x);
        id_roundtrip(W_DOC("-9.22337203685477581e18"), x);
        id_roundtrip(W_DOC("9223372036854775808"), x);
        id_roundtrip(W_DOC("1e300"), x);
    }
    /* one meaning, one ID */
    {
        static const char* const on[] = {
            XH "\"stimuli\":{\"s\":{\"kind\":\"shape\"}},\"trials\":{\"t\":{\"events\":[{\"on\":\"s\"}]}}," XT,
            XH "\"stimuli\":{\"s\":{\"kind\":\"shape\"}},\"trials\":{\"t\":{\"events\":[{\"on\":[\"s\"]}]}}," XT
        };
#define CH_DOC(x) XH "\"devices\":{\"r\":{\"direction\":\"in\",\"family\":[\"k\"]}},\"stimuli\":{\"s\":{\"kind\":\"shape\"}},\"trials\":{\"t\":{\"events\":[{\"on\":\"s\"}],\"response\":{\"role\":\"r\",\"from\":\"s\",\"choices\":[" x "]}}}," XT
        static const char* const ch[] = { CH_DOC("\"r\""), CH_DOC("{\"key\":\"r\"}") };
        static const char* const dl[] = { CH_DOC("\"$$r\""), CH_DOC("{\"key\":\"$$r\"}") };
        uint8_t x[32], y[32];
        one_spelling(on, 2, "one target, two spellings");
        one_spelling(ch, 2, "one choice, two spellings");
        one_spelling(dl, 2, "$$ in both choice forms");
        id_roundtrip(CH_DOC("{\"key\":\"r\",\"note\":\"kept\"}"), x);
        id_roundtrip(CH_DOC("\"r\""), y);
        CHECK(memcmp(x, y, 32) != 0, "a choice's note stays in the ID");
    }
    /* a michelson inside a lookup defines $contrast_shown */
    {
        const char* doc = XH "\"devices\":{\"r\":{\"direction\":\"in\",\"family\":[\"k\"]}},\"stimuli\":{\"g\":{\"kind\":\"gabor\",\"contrast\":{\"by\":\"$rep\",\"map\":{\"0\":{\"michelson\":0.5}}}}},"
            "\"trials\":{\"t\":{\"events\":[{\"on\":\"g\"}],\"columns\":{\"c\":\"$contrast_shown\"},\"response\":{\"role\":\"r\",\"from\":\"g\"}}}," XT;
        CHECK2(load_rc(doc, NULL, &r) == YEXP_OK, "a michelson in a lookup defines $contrast_shown", r.n_diags ? r.diags[0].msg : "");
    }
    /* a chain of vars that ends is no loop */
    {
        char doc[4096];
        int at = 0, i;
        at += snprintf(doc + at, sizeof doc - (size_t)at, XH "\"stimuli\":{\"s\":{\"kind\":\"shape\",\"w\":\"$a0\"}},\"trials\":{\"t\":{\"events\":[{\"on\":\"s\"}]}},\"timeline\":[{\"name\":\"m\",\"trial\":\"t\",\"vars\":{");
        for (i = 0; i < 20; i++) at += snprintf(doc + at, sizeof doc - (size_t)at, "\"a%d\":\"$a%d\",", i, i + 1);
        at += snprintf(doc + at, sizeof doc - (size_t)at, "\"a20\":1}}]}");
        CHECK2(load_rc(doc, NULL, &r) == YEXP_OK, "a chain of 20 vars loads", r.n_diags ? r.diags[0].msg : "");
    }
    /* 65 jitters and rule vars: no silent cap */
    {
        char doc[8192];
        int at = 0, i;
        at += snprintf(doc + at, sizeof doc - (size_t)at, XH "\"trials\":{\"t\":{\"events\":[{\"wait_s\":\"$j64_s\"}],\"columns\":{\"c\":\"$v64\"}}},\"timeline\":[{\"name\":\"m\",\"trial\":\"t\",\"rules\":[");
        for (i = 0; i < 65; i++) at += snprintf(doc + at, sizeof doc - (size_t)at, "\"jitter j%d_s uniform 0 1\",\"var v%d choice a b\",", i, i);
        at += snprintf(doc + at, sizeof doc - (size_t)at, "\"reps 1\"]}]}");
        CHECK2(load_rc(doc, NULL, &r) == YEXP_OK, "65 jitters and rule vars", r.n_diags ? r.diags[0].msg : "");
    }
    /* what no node reaches is named in a warning, and not refused */
    {
        const char* doc = XH "\"stimuli\":{\"s\":{\"kind\":\"shape\",\"w\":\"$nosuch\"}},\"trials\":{\"t\":{},\"u\":{\"events\":[{\"wait_s\":\"$nosuch_s\"}]}}," XT;
        CHECK(load_rc(doc, NULL, &r) == YEXP_OK, "unused parts are not refused");
        CHECK(r.n_warnings == 2 && r.n_diags == 2, "two warnings");
        CHECK(strstr(r.diags[0].msg, "template 'u'") && strstr(r.diags[0].msg, "not checked"), "the template by name");
        CHECK(strstr(r.diags[1].msg, "stimulus 's'") && strstr(r.diags[1].msg, "not checked"), "the stimulus by name");
    }
    /* valid long lists are linear, so the work limit does not refuse them */
    {
        size_t cap = 4u << 20, at = 0;
        char* doc = (char*)malloc(cap);
        int i, k;
        if (!doc) abort();
        at += (size_t)snprintf(doc + at, cap - at, XH "\"stimuli\":{");
        for (i = 0; i < 1024; i++) at += (size_t)snprintf(doc + at, cap - at, "%s\"s%d\":{\"kind\":\"shape\"}", i ? "," : "", i);
        at += (size_t)snprintf(doc + at, cap - at, "},\"trials\":{\"t\":{\"events\":[");
        for (k = 0; k < 100; k++) {
            at += (size_t)snprintf(doc + at, cap - at, "%s{\"on\":[", k ? "," : "");
            for (i = 0; i < 1024; i++) at += (size_t)snprintf(doc + at, cap - at, "%s\"s%d\"", i ? "," : "", i);
            at += (size_t)snprintf(doc + at, cap - at, "]}");
        }
        at += (size_t)snprintf(doc + at, cap - at, "]}}," XT);
        CHECK2(load_rc(doc, NULL, &r) == YEXP_OK, "lists at the stated limits are in the work budget", r.n_diags ? r.diags[0].msg : "");
        free(doc);
    }
    /* many templates in a small arena: the tables are sized on demand */
    {
        size_t cap = 1u << 20, at = 0;
        char* doc = (char*)malloc(cap);
        static uint64_t mem[(3u << 20) / 8];
        yjs_arena a;
        yexp_def* d = NULL;
        int i;
        if (!doc) abort();
        at += (size_t)snprintf(doc + at, cap - at, XH "\"trials\":{");
        for (i = 0; i < 1024; i++) at += (size_t)snprintf(doc + at, cap - at, "%s\"t%04d\":{\"events\":[]}", i ? "," : "", i);
        at += (size_t)snprintf(doc + at, cap - at, "},\"timeline\":[{\"name\":\"m\",\"trial\":\"t0000\"}]}");
        yjs_arena_init(&a, mem, sizeof mem);
        CHECK(yexp_load(&a, doc, at, NULL, &d, &r) == YEXP_OK, "1024 templates fit 3 MB");
        free(doc);
    }
    /* a host that misbehaves */
    {
        const char* doc =
            XH "\"devices\":{\"r\":{\"direction\":\"in\",\"family\":[\"k\"]}},\"stimuli\":{\"s\":{\"kind\":\"shape\",\"w\":1,\"x\":{\"by\":\"$c\",\"map\":{\"a\":1}}}},"
            "\"trials\":{\"t\":{\"events\":[{\"on\":\"s\"}]}},\"timeline\":[{\"name\":\"m\",\"trial\":\"t\",\"timeline_variables\":\"tt\"}]}";
        int m;
        for (m = 0; m < BH_MODES; m++) {
            int rc;
            g_bh_mode = m;
            g_bh_calls = 0;
            rc = load_rc(doc, &g_bad_host, &r);
            CHECK(rc == YEXP_OK || rc == YEXP_ERR_REFUSED, "a bad host: a plain result");
            if (m == BH_COL_ENDLESS || m == BH_COL_ENDLESS_LATE) CHECK(rc == YEXP_ERR_REFUSED && r.n_diags >= 1 && strstr(r.diags[0].msg, "steps to check"), "an endless column() hits the work limit");
            if (m == BH_LVL_LONG) {
                /* the 300-byte level is read, so its absence from the map is seen */
                CHECK(rc == YEXP_ERR_REFUSED && r.n_diags >= 1 && strstr(r.diags[0].msg, "no key for"), "a 300-byte level is checked");
            }
            if (m == BH_LVL_LONG_CHANGES || m == BH_LVL_HUGE || m == BH_LVL_MANY)
                CHECK(rc == YEXP_OK && (r.skipped & YEXP_CHECK_LEVELS) && !(r.run & YEXP_CHECK_LEVELS), "levels not read are recorded as skipped");
            if (m == BH_MSG_NO_NUL) {
                CHECK(rc == YEXP_ERR_REFUSED && r.n_diags >= 1, "a message with no NUL");
                /* 255 of the host's 256 bytes, after the prefix: the NUL was forced */
                {
                    const char* x = NULL;
                    int q;
                    for (q = 0; q < r.n_diags && !x; q++) x = strstr(r.diags[q].msg, "xxxxxxxx");
                    CHECK(x && strlen(x) == 255, "and it stays in bounds");
                }
            }
        }
        g_bh_mode = 0;
    }
    /* a member named "note" that is no string is a member, not a note */
    {
        const char* doc = XH "\"stimuli\":{\"note\":{\"kind\":\"shape\"}},\"trials\":{\"t\":{\"events\":[{\"on\":\"note\"}]}},\"timeline\":[{\"name\":\"m\",\"trial\":\"t\",\"vars\":{\"note\":5}}]}";
        CHECK2(load_rc(doc, NULL, &r) == YEXP_OK, "a member named note", r.n_diags ? r.diags[0].msg : "");
    }
    /* a long vars chain still has its lookup checked */
    {
        char doc[8192];
        int at = 0, i;
        at += snprintf(doc + at, sizeof doc - (size_t)at, XH "\"stimuli\":{\"s\":{\"kind\":\"shape\",\"w\":{\"by\":\"$a0\",\"map\":{\"a\":1}}}},\"trials\":{\"t\":{\"events\":[{\"on\":\"s\"}]}},\"timeline\":[{\"name\":\"m\",\"trial\":\"t\",\"vars\":{");
        for (i = 0; i < 50; i++) at += snprintf(doc + at, sizeof doc - (size_t)at, "\"a%d\":\"$a%d\",", i, i + 1);
        at += snprintf(doc + at, sizeof doc - (size_t)at, "\"a50\":{\"choice\":[\"a\",\"b\"]}}}]}");
        CHECK2(load_rc(doc, NULL, &r) == YEXP_ERR_REFUSED && r.n_diags && strstr(r.diags[0].msg, "no key for"), "a lookup at the end of 50 links is checked", r.n_diags ? r.diags[0].msg : "");
    }
    /* a column of 0 and 1, read by its length: the host need not write a NUL */
    {
        yexp_host h;
        const char* doc = XH "\"trials\":{\"t\":{}},\"timeline\":[{\"name\":\"c\",\"timeline_variables\":\"t\",\"timeline\":[{\"name\":\"m\",\"trial\":\"t\",\"if\":\"$f\"}]}]}";
        memset(&h, 0, sizeof h);
        h.column = flag_column;
        h.level = flag_level;
        CHECK2(load_rc(doc, &h, &r) == YEXP_OK, "0 and 1 with no NUL in the buffer", r.n_diags ? r.diags[0].msg : "");
    }
    /* an integer session field can stand for a boolean; a string field cannot */
    {
        const char* node = XH "\"trials\":{\"t\":{}},\"timeline\":[{\"name\":\"m\",\"trial\":\"t\",\"if\":\"$participant\"}]}";
        const char* node2 = XH "\"trials\":{\"t\":{}},\"timeline\":[{\"name\":\"m\",\"trial\":\"t\",\"unless\":\"$participant\"}]}";
        const char* step = XH "\"trials\":{\"t\":{\"events\":[{\"if\":\"$participant\",\"steps\":[]},{\"unless\":\"$participant\",\"steps\":[]}]}}," XT;
        CHECK(load_rc(node, NULL, &r) == YEXP_OK, "an integer field in a node's if");
        CHECK(load_rc(node2, NULL, &r) == YEXP_OK, "an integer field in a node's unless");
        CHECK(load_rc(step, NULL, &r) == YEXP_OK, "an integer field in a step's if and unless");
    }
    /* a stagger on one stimulus has no effect, so it is dropped */
    {
#define ON_DOC(x) XH "\"stimuli\":{\"s\":{\"kind\":\"shape\"}},\"trials\":{\"t\":{\"events\":[" x "]}}," XT
        static const char* const a[] = { ON_DOC("{\"on\":\"s\"}"), ON_DOC("{\"on\":[\"s\"],\"stagger_s\":0.1}"), ON_DOC("{\"on\":\"s\",\"stagger_s\":0.1}") };
        one_spelling(a, 3, "a stagger on one stimulus");
    }
    /* one refused element is one error: references to it are not dangling */
    {
        static const char* const docs[] = {
            XH "\"stimuli\":{\"s\":{\"kind\":\"shape\",\"w\":{\"michelson\":1,\"note\":5}}},\"trials\":{\"t\":{\"events\":[{\"on\":\"s\"},{\"set\":\"s\",\"props\":{\"x\":1}}],\"record\":[\"s\"]}}," XT,
            XH "\"stimuli\":{\"s\":{\"kind\":\"blob\"}},\"trials\":{\"t\":{\"events\":[{\"on\":\"s\"},{\"to\":\"s\",\"props\":{\"bogus\":1}}]}}," XT,
            XH "\"trials\":{\"t\":{\"bogus\":1}}," XT,
            XH "\"devices\":{\"o\":{\"direction\":\"sideways\",\"family\":[\"k\"]}},\"trials\":{\"t\":{\"events\":[{\"trigger\":1,\"role\":\"o\"}]}}," XT,
            XH "\"devices\":{\"o\":1},\"trials\":{\"t\":{\"events\":[{\"trigger\":1,\"role\":\"o\"}]}}," XT,
            XH "\"methods\":{\"q\":{\"type\":\"nope\"}},\"trials\":{\"t\":{}},\"timeline\":[{\"name\":\"m\",\"trial\":\"t\",\"tracks\":[\"q\"]}]}",
            XH "\"stimuli\":[1],\"trials\":{\"t\":{\"events\":[{\"on\":\"s\"}],\"record\":[\"s\"]}}," XT,
            XH "\"devices\":[1],\"trials\":{\"t\":{\"events\":[{\"trigger\":1,\"role\":\"o\"}]}}," XT,
            XH "\"methods\":[1],\"trials\":{\"t\":{}},\"timeline\":[{\"name\":\"m\",\"trial\":\"t\",\"tracks\":[\"q\"]}]}",
            XH "\"trials\":[1]," XT,
            XH "\"timeline\":[{\"name\":\"m\",\"trial\":\"t\"}]}",
        };
        size_t k;
        for (k = 0; k < sizeof docs / sizeof docs[0]; k++) {
            CHECK2(load_rc(docs[k], NULL, &r) == YEXP_ERR_REFUSED, "refused", docs[k]);
            CHECK2(r.n_errors == 1, "one cause, one error", r.n_diags > 1 ? r.diags[1].msg : docs[k]);
        }
        /* a from whose events failed is not also "never shown" */
        {
            const char* d = XH "\"devices\":{\"r\":{\"direction\":\"in\",\"family\":[\"k\"]}},\"stimuli\":{\"s\":{\"kind\":\"shape\"}},\"trials\":{\"t\":{\"events\":[{\"bogus\":1}],\"response\":{\"role\":\"r\",\"from\":\"s\"}}}," XT;
            CHECK(load_rc(d, NULL, &r) == YEXP_ERR_REFUSED && r.n_errors == 1, "a from after a bad event");
        }
    }
    /* a container with no leaf is named in a warning */
    {
        const char* doc = XH "\"trials\":{\"t\":{}},\"timeline\":[{\"name\":\"m\",\"trial\":\"t\"},{\"name\":\"c\",\"vars\":{\"x\":\"$nosuch\"},\"timeline\":[{\"name\":\"d\",\"timeline\":[]}]}]}";
        CHECK(load_rc(doc, NULL, &r) == YEXP_OK && r.n_warnings == 1, "one warning for a branch with no leaf");
        CHECK(r.n_diags == 1 && strstr(r.diags[0].msg, "node 'c'") && strstr(r.diags[0].msg, "not checked"), "by the outermost name");
    }
    /* a list of buttons and choices at the limits */
    {
        size_t cap = 2u << 20, at = 0;
        char* doc = (char*)malloc(cap);
        int i;
        if (!doc) abort();
        at += (size_t)snprintf(doc + at, cap - at, XH "\"devices\":{\"r\":{\"direction\":\"in\",\"family\":[\"k\"],\"buttons\":{");
        for (i = 0; i < 20000; i++) at += (size_t)snprintf(doc + at, cap - at, "%s\"%d\":\"b%d\"", i ? "," : "", i, i);
        at += (size_t)snprintf(doc + at, cap - at, "}}},\"stimuli\":{\"s\":{\"kind\":\"shape\"}},\"trials\":{\"t\":{\"events\":[{\"on\":\"s\"}],\"response\":{\"role\":\"r\",\"from\":\"s\",\"choices\":[");
        for (i = 0; i < 4000; i++) at += (size_t)snprintf(doc + at, cap - at, "%s\"b%d\"", i ? "," : "", i);
        at += (size_t)snprintf(doc + at, cap - at, "]}}}," XT);
        CHECK2(load_rc(doc, NULL, &r) == YEXP_OK, "4000 choices against 20000 buttons", r.n_diags ? r.diags[0].msg : "");
        free(doc);
    }
    /* outcome's default: "correct" with a response, empty without */
    {
#define O_RESP(x) XH "\"devices\":{\"r\":{\"direction\":\"in\",\"family\":[\"k\"]}},\"stimuli\":{\"s\":{\"kind\":\"shape\"}},\"trials\":{\"t\":{" x "\"events\":[{\"on\":\"s\"}],\"response\":{\"role\":\"r\",\"from\":\"s\"}}}," XT
#define O_NONE(x) XH "\"trials\":{\"t\":{" x "\"events\":[]}}," XT
        static const char* const a[] = { O_RESP(""), O_RESP("\"outcome\":\"correct\",") };
        static const char* const b[] = { O_NONE(""), O_NONE("\"outcome\":\"\",") };
        uint8_t x[32], y[32];
        yjs_arena ar;
        yexp_def* d = NULL;
        one_spelling(a, 2, "\"correct\" is the default with a response");
        one_spelling(b, 2, "empty is the default with no response");
        id_roundtrip(O_NONE(""), x);
        id_roundtrip(O_NONE("\"outcome\":\"correct\","), y);
        CHECK(memcmp(x, y, 32) != 0, "\"correct\" with no response is not the default");
        id_roundtrip(O_RESP("\"outcome\":\"\","), y);
        id_roundtrip(O_RESP(""), x);
        CHECK(memcmp(x, y, 32) != 0, "empty with a response is not the default");
        yjs_arena_init_heap(&ar, 0);
        CHECK(yexp_load(&ar, O_NONE(""), strlen(O_NONE("")), NULL, &d, &r) == YEXP_OK && d->templates[0].outcome && d->templates[0].outcome->kind == YEXP_V_STRING && !d->templates[0].outcome->str[0], "the form holds the empty outcome");
        yjs_arena_free(&ar);
    }
    /* a method_value with no leaf with tracks: warned, not in the form, not in the ID */
    {
#define MV_DOC(x, tr) XH "\"devices\":{\"r\":{\"direction\":\"in\",\"family\":[\"keyboard\"]}},\"stimuli\":{\"s\":{\"kind\":\"shape\"}},\"methods\":{\"q\":{\"type\":\"stair\",\"steps\":[1]}},\"trials\":{\"t\":{" x "\"events\":[{\"on\":\"s\"}],\"response\":{\"role\":\"r\",\"from\":\"s\",\"key_answer\":\"a\"}}},\"timeline\":[{\"name\":\"m\",\"trial\":\"t\"" tr "}]}"
#define TRK ",\"tracks\":[\"q\"]"
        static const char* const a[] = { MV_DOC("", ""), MV_DOC("\"method_value\":\"$nosuch\",", ""), MV_DOC("\"method_value\":3,", "") };
        uint8_t x[32], y[32];
        yjs_arena ar;
        yexp_def* d = NULL;
        one_spelling(a, 3, "a method_value no track reads is not in the ID");
        CHECK(load_rc(MV_DOC("\"method_value\":3,", ""), NULL, &r) == YEXP_OK && r.n_warnings == 1 && strstr(r.diags[0].msg, "ignored"), "and it is warned");
        CHECK(load_rc(MV_DOC("", ""), NULL, &r) == YEXP_OK && r.n_warnings == 0, "no warning when none is written");
        yjs_arena_init_heap(&ar, 0);
        CHECK(yexp_load(&ar, MV_DOC("\"method_value\":3,", ""), strlen(MV_DOC("\"method_value\":3,", "")), NULL, &d, &r) == YEXP_OK && !d->templates[0].method_value, "not in the form");
        yjs_arena_free(&ar);
        yjs_arena_init_heap(&ar, 0);
        CHECK(yexp_load(&ar, MV_DOC("", TRK), strlen(MV_DOC("", TRK)), NULL, &d, &r) == YEXP_OK && d->templates[0].method_value && d->templates[0].method_value->kind == YEXP_V_VAR, "with tracks: the default $level");
        yjs_arena_free(&ar);
        id_roundtrip(MV_DOC("\"method_value\":3,", TRK), x);
        id_roundtrip(MV_DOC("", TRK), y);
        CHECK(memcmp(x, y, 32) != 0, "with tracks a written method_value is in the ID");
        CHECK(load_rc(MV_DOC("\"method_value\":\"$nosuch\",", TRK), NULL, &r) == YEXP_ERR_REFUSED, "with tracks it is checked");
        {
            /* two leaves, one with tracks: kept, and checked on that leaf only */
            const char* two = XH "\"devices\":{\"r\":{\"direction\":\"in\",\"family\":[\"keyboard\"]}},\"stimuli\":{\"s\":{\"kind\":\"shape\"}},\"methods\":{\"q\":{\"type\":\"stair\",\"steps\":[1]}},\"trials\":{\"t\":{\"method_value\":\"$level\",\"events\":[{\"on\":\"s\"}],\"response\":{\"role\":\"r\",\"from\":\"s\",\"key_answer\":\"a\"}}},\"timeline\":[{\"name\":\"a\",\"trial\":\"t\"},{\"name\":\"b\",\"trial\":\"t\",\"tracks\":[\"q\"]}]}";
            CHECK2(load_rc(two, NULL, &r) == YEXP_OK && r.n_warnings == 0, "a template shared by a tracked and an untracked leaf", r.n_diags ? r.diags[0].msg : "");
        }
    }
    /* a rule var used as a number: numeric text is fine, other text is not */
    {
#define RV_DOC(c) XH "\"stimuli\":{\"s\":{\"kind\":\"shape\",\"w\":\"$x\"}},\"trials\":{\"t\":{\"events\":[{\"on\":\"s\"}]}},\"timeline\":[{\"name\":\"m\",\"trial\":\"t\",\"rules\":[\"var x choice " c "\"]}]}"
        CHECK2(load_rc(RV_DOC("1 2.5"), NULL, &r) == YEXP_OK, "a rule var of numeric text is a number", r.n_diags ? r.diags[0].msg : "");
        CHECK(load_rc(RV_DOC("1 two"), NULL, &r) == YEXP_ERR_REFUSED, "a rule var of other text is no number");
    }
    /* a level written with no NUL and the sane host's rule var levels */
    {
        const char* doc = XH "\"stimuli\":{\"s\":{\"kind\":\"shape\",\"w\":{\"by\":\"$x\",\"map\":{\"a\":1,\"b\":2}}}},\"trials\":{\"t\":{\"events\":[{\"on\":\"s\"}]}},"
                          "\"timeline\":[{\"name\":\"m\",\"trial\":\"t\",\"rules\":[\"var x choice a \\\"b\\\"\"]}]}";
        CHECK2(load_rc(doc, &g_host, &r) == YEXP_OK && r.n_warnings == 0, "a rule var's levels, one of them quoted", r.n_diags ? r.diags[0].msg : "");
    }
}

/* --- version 2: the host, the lookups, the spellings --------------------- */

#define V2H "{\"format\":\"ysp-experiment\",\"version\":2,"

static void run_v2(void) {
    static yexp_report r;
    yjs_arena a;
    yexp_def* d = NULL;
    /* sigrole(): run when the file has signal roles, recorded either way */
    {
        const char* doc = V2H "\"roles\":{\"not_on_rig\":{\"type\":\"scalar\",\"units\":\"device\"}},\"trials\":{\"t\":{}},\"timeline\":[{\"name\":\"m\",\"trial\":\"t\"}]}";
        const char* none = V2H "\"trials\":{\"t\":{}},\"timeline\":[{\"name\":\"m\",\"trial\":\"t\"}]}";
        yexp_host h;
        CHECK(load_rc(doc, &g_host, &r) == YEXP_ERR_REFUSED && r.n_diags && strstr(r.diags[0].msg, "no adapter"), "sigrole() refuses a role the rig lacks");
        CHECK((r.run & YEXP_CHECK_SIGROLES) != 0, "and it is recorded as run");
        memset(&h, 0, sizeof h);
        h.entry = st_entry;
        CHECK(load_rc(doc, &h, &r) == YEXP_OK && (r.skipped & YEXP_CHECK_SIGROLES) && !(r.run & YEXP_CHECK_SIGROLES), "no sigrole(): skipped (G18)");
        CHECK(load_rc(none, &g_host, &r) == YEXP_OK && !(r.run & YEXP_CHECK_SIGROLES) && !(r.skipped & YEXP_CHECK_SIGROLES), "no signal roles: neither bit");
    }
    /* the lookups by name, and a version 2 file's migrated bytes are its own */
    {
        char path[512];
        size_t n = 0;
        char* text;
        snprintf(path, sizeof path, "%s/good/v2_bimanual.json", g_corpus);
        text = read_file(path, &n);
        CHECK(text != NULL, "the bimanual example");
        if (text) {
            yjs_arena_init_heap(&a, 0);
            if (yexp_load(&a, text, n, &g_host, &d, &r) == YEXP_OK) {
                const char* b1;
                const char* b2;
                size_t n1, n2;
                uint8_t i1[32], i2[32];
                const yexp_template* reach = &d->templates[0];
                CHECK(yexp_sigrole_get(d, "left") && yexp_sigrole_get(d, "left")->lanes == 2, "yexp_sigrole_get()");
                CHECK(!yexp_sigrole_get(d, "dot") && !yexp_sigrole_get(NULL, "left"), "yexp_sigrole_get() of nothing");
                CHECK(yexp_control_get(d, NULL, "cursor") && yexp_control_get(d, NULL, "cursor")->shared, "a top-level control");
                CHECK(yexp_control_get(d, reach, "arrived") && !yexp_control_get(d, NULL, "arrived"), "a template's control, only through it");
                CHECK(yexp_control_get(d, reach, "cursor") == yexp_control_get(d, NULL, "cursor"), "a template sees the top-level controls");
                CHECK(reach->uses_top && reach->uses_top[0] == 1, "the reach template names the cursor");
                CHECK(yexp_canonical(d, &a, &b1, &n1, i1) == YEXP_OK && yexp_migrated(d, &a, &b2, &n2, i2) == YEXP_OK && n1 == n2 && !memcmp(b1, b2, n1) && !memcmp(i1, i2, 32),
                      "a version 2 file migrates to itself");
                CHECK(yexp_migrated(NULL, &a, &b2, &n2, i2) == YEXP_ERR_ARG, "yexp_migrated() of nothing");
            } else {
                CHECK2(0, "the bimanual example loads", r.n_diags ? r.diags[0].msg : "");
            }
            yjs_arena_free(&a);
            free(text);
        }
    }
    /* one meaning, one ID: a step's single target, a transform at its default, a key's default */
    {
#define ST_DOC(x) V2H "\"roles\":{\"h\":{\"type\":\"position2d\",\"units\":\"display_px\"}},\"stimuli\":{\"s\":{\"kind\":\"shape\"}},\"trials\":{\"t\":{\"controls\":{\"c\":{\"control\":\"cursor\",\"role\":\"h\"" x "}},\"states\":[{\"name\":\"a\",\"steps\":[{\"on\":\"s\"}]}]}},\"timeline\":[{\"name\":\"m\",\"trial\":\"t\"}]}"
        static const char* const tr[] = { ST_DOC(""), ST_DOC(",\"transform\":\"identity\""), ST_DOC(",\"about\":[0,0]"), ST_DOC(",\"about\":[0.0,-0]") };
        static const char* const rot[] = { ST_DOC(",\"transform\":{\"rotation_deg\":30}"), ST_DOC(",\"transform\":{\"rotation_deg\":30.0}"), ST_DOC(",\"transform\":{\"rotation_deg\":3e1}") };
        uint8_t x[32], y[32];
        one_spelling(tr, 4, "a cursor's keys at their defaults");
        one_spelling(rot, 3, "a rotation's spellings");
        id_roundtrip(ST_DOC(""), x);
        id_roundtrip(ST_DOC(",\"transform\":{\"rotation_deg\":0}"), y);
        CHECK(memcmp(x, y, 32) != 0, "a rotation of 0 is not \"identity\": another node");
    }
    /* several errors in one pass: three refused names in one template */
    {
        const char* doc = V2H "\"roles\":{\"h\":{\"type\":\"position2d\",\"units\":\"display_px\"}},\"stimuli\":{\"s\":{\"kind\":\"shape\"}},"
                          "\"trials\":{\"t\":{\"controls\":{\"a\":{\"control\":\"region\",\"of\":\"h\",\"radius\":0},\"b\":{\"control\":\"peak\",\"of\":\"zz\"}},"
                          "\"states\":[{\"name\":\"x\",\"exits\":[{\"after_s\":0,\"to\":\"y\"}]}]}},\"timeline\":[{\"name\":\"m\",\"trial\":\"t\"}]}";
        CHECK(load_rc(doc, NULL, &r) == YEXP_ERR_REFUSED && r.n_errors == 4, "four errors collected in one pass");
    }
    /* a version 1 file meets version 2's keys as a version message */
    {
        const char* doc = XH "\"roles\":{},\"trials\":{\"t\":{}}," XT;
        CHECK(load_rc(doc, NULL, &r) == YEXP_ERR_REFUSED && r.n_diags && strstr(r.diags[0].msg, "unknown key 'roles'"), "version 1 does not know roles");
    }
    /* many states in a chain: the cycle search stays linear */
    {
        size_t cap = 1u << 20, at = 0;
        char* doc = (char*)malloc(cap);
        int i;
        if (!doc) abort();
        at += (size_t)snprintf(doc + at, cap - at, V2H "\"trials\":{\"t\":{\"states\":[");
        for (i = 0; i < 4096; i++) at += (size_t)snprintf(doc + at, cap - at, "%s{\"name\":\"s%d\",\"exits\":[{\"to\":\"%s%d\"}]}", i ? "," : "", i, i + 1 < 4096 ? "s" : "end", i + 1 < 4096 ? i + 1 : 0);
        /* the last state's exit goes to "end0", which no state has */
        at += (size_t)snprintf(doc + at, cap - at, "]}},\"timeline\":[{\"name\":\"m\",\"trial\":\"t\"}]}");
        CHECK(load_rc(doc, NULL, &r) == YEXP_ERR_REFUSED && r.n_errors == 1 && strstr(r.diags[0].msg, "end0"), "4096 states: one dangling name, in the budget");
        free(doc);
    }
}

/* The review's shapes at size: each loads in the work budget, so the
 * closure of top-level controls, a shared exit's states and the outcomes
 * of requeue_outcomes cost what the file holds, not its square. */
static void run_v2_linear(void) {
    static yexp_report r;
    size_t cap = 8u << 20, at = 0;
    char* doc = (char*)malloc(cap);
    int i, k;
    if (!doc) abort();
    /* 1024 chained top-level peaks; 16 templates name the last */
    at += (size_t)snprintf(doc + at, cap - at, V2H "\"roles\":{\"h\":{\"type\":\"position2d\",\"units\":\"display_px\"}},\"stimuli\":{\"s\":{\"kind\":\"shape\"}},\"controls\":{");
    for (i = 0; i < 1024; i++) at += (size_t)snprintf(doc + at, cap - at, "%s\"c%04d\":{\"control\":\"peak\",\"of\":\"%s%04d\"}", i ? "," : "", i, i ? "c" : "h", i ? i - 1 : 0);
    /* "h0000" is no name: the first reads the role */
    at = (size_t)(strstr(doc, "\"h0000\"") - doc);
    memcpy(doc + at, "\"h\"    ", 7);
    at = strlen(doc);
    at += (size_t)snprintf(doc + at, cap - at, "},\"trials\":{");
    for (k = 0; k < 16; k++) at += (size_t)snprintf(doc + at, cap - at, "%s\"t%02d\":{\"states\":[{\"name\":\"a\",\"steps\":[{\"on\":\"s\"},{\"set\":\"s\",\"props\":{\"x\":{\"signal\":\"c1023\"}}}]}]}", k ? "," : "", k);
    at += (size_t)snprintf(doc + at, cap - at, "},\"timeline\":[");
    for (k = 0; k < 16; k++) at += (size_t)snprintf(doc + at, cap - at, "%s{\"name\":\"n%02d\",\"trial\":\"t%02d\"}", k ? "," : "", k, k);
    at += (size_t)snprintf(doc + at, cap - at, "]}");
    CHECK2(load_rc(doc, NULL, &r) == YEXP_OK, "1024 chained top-level controls, 16 templates", r.n_diags ? r.diags[0].msg : "");
    /* 4096 states, 64 shared exits each listing half of them */
    at = 0;
    at += (size_t)snprintf(doc + at, cap - at, V2H "\"roles\":{\"h\":{\"type\":\"position2d\",\"units\":\"display_px\"}},\"stimuli\":{\"s\":{\"kind\":\"shape\"}},"
                           "\"trials\":{\"t\":{\"controls\":{\"r\":{\"control\":\"region\",\"of\":\"h\",\"radius\":1}},\"states\":[");
    for (i = 0; i < 4096; i++) at += (size_t)snprintf(doc + at, cap - at, "%s{\"name\":\"s%04d\"%s}", i ? "," : "", i, i ? "" : ",\"steps\":[{\"on\":\"s\"}]");
    at += (size_t)snprintf(doc + at, cap - at, "],\"exits\":[");
    for (k = 0; k < 64; k++) {
        at += (size_t)snprintf(doc + at, cap - at, "%s{\"on\":\"r\",\"to\":\"end\",\"in\":[", k ? "," : "");
        for (i = 0; i < 2048; i++) at += (size_t)snprintf(doc + at, cap - at, "%s\"s%04d\"", i ? "," : "", i);
        at += (size_t)snprintf(doc + at, cap - at, "]}");
    }
    at += (size_t)snprintf(doc + at, cap - at, "]}},\"timeline\":[{\"name\":\"m\",\"trial\":\"t\"}]}");
    CHECK2(load_rc(doc, NULL, &r) == YEXP_OK, "4096 states, 64 shared exits of 2048 states", r.n_diags ? r.diags[0].msg : "");
    /* 4096 exits with outcomes and 4096 requeue_outcomes, each given */
    at = 0;
    at += (size_t)snprintf(doc + at, cap - at, V2H "\"roles\":{\"h\":{\"type\":\"position2d\",\"units\":\"display_px\"}},\"stimuli\":{\"s\":{\"kind\":\"shape\"}},"
                           "\"trials\":{\"t\":{\"controls\":{\"r\":{\"control\":\"region\",\"of\":\"h\",\"radius\":1}},\"states\":[{\"name\":\"a\",\"steps\":[{\"on\":\"s\"}],\"exits\":[");
    for (i = 0; i < 4096; i++) at += (size_t)snprintf(doc + at, cap - at, "%s{\"on\":\"r\",\"to\":\"end\",\"outcome\":\"o%04d\"}", i ? "," : "", i);
    at += (size_t)snprintf(doc + at, cap - at, "]}],\"requeue_outcomes\":[");
    for (i = 0; i < 4096; i++) at += (size_t)snprintf(doc + at, cap - at, "%s\"o%04d\"", i ? "," : "", 4095 - i);
    at += (size_t)snprintf(doc + at, cap - at, "]}},\"timeline\":[{\"name\":\"m\",\"trial\":\"t\"}]}");
    CHECK2(load_rc(doc, NULL, &r) == YEXP_OK && r.n_warnings == 0, "4096 requeue_outcomes against 4096 exits", r.n_diags ? r.diags[0].msg : "");
    free(doc);
}

/* --- arena -------------------------------------------------------------- */

static void run_arena(void) {
    /* every arena size from nothing up: a full arena never crashes */
    size_t n = 0;
    char path[512];
    char* text;
    static uint64_t mem[1 << 17];
    static yexp_report r;
    size_t cap;
    int ok = 0, nomem = 0;
    int file;
    for (file = 0; file < 2; file++) {
    ok = nomem = 0;
    snprintf(path, sizeof path, "%s/good/%s", g_corpus, file ? "v2_bimanual.json" : "2afc.json");
    text = read_file(path, &n);
    CHECK(text != NULL, "the 2afc and bimanual examples");
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
}

/* --- SHA-256 ------------------------------------------------------------ */

static void hex(const uint8_t* b, char* out) {
    int i;
    for (i = 0; i < 32; i++) snprintf(out + 2 * i, 3, "%02x", b[i]);
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
    run_review();
    run_host();
    run_v2();
    run_v2_linear();
    run_arena();
    run_defaults();
    run_defaults_page();
    run_corpus();
    if (g_failures) {
        fprintf(stderr, "experiment_test: %d failure(s) in %d checks\n", g_failures, g_checks);
        return 1;
    }
    printf("experiment_test: %d checks, %d files loaded, %d migrated: all passed\n", g_checks, g_tot_loaded, g_tot_migrated);
    return 0;
}
