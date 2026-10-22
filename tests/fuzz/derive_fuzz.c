/* derive_fuzz.c - fuzz target for the texts the pack tool takes from an
 * experiment definition (pack/derive.c, ypt_derive_texts). A pack's
 * definition comes from another lab and is rebuilt with `ypak rebuild`, so
 * its JSON is untrusted input to the tool. The input is the definition;
 * two tables are fixed ("t.pstb": a string, an integer and a number column;
 * "u.pstb": one string column), so a `$name` can reach table levels.
 * Every input must return without a sanitizer report. A refusal must carry
 * a message and leave nothing to free; an accepted definition must give
 * keys in strictly ascending order, each ypak_runs_key() of its style and
 * text (the player's key), every field set, its stimuli sorted, and only
 * tables the callback knows.
 *
 * One translation unit: it includes pack/derive.c and the json and table
 * implementations, so no CMake target is needed.
 *
 * libFuzzer, MSVC 2022 x64 (from the repository root, in a vcvars shell):
 *     cl /nologo /O1 /Zi /fsanitize=address /fsanitize=fuzzer /Iinclude /Ipack tests\fuzz\derive_fuzz.c
 *     derive_fuzz.exe -max_total_time=600 -timeout=10 -max_len=8192 corpus_derive
 * libFuzzer, clang:
 *     clang -O1 -g -fsanitize=fuzzer,address,undefined -Iinclude -Ipack tests/fuzz/derive_fuzz.c -o derive_fuzz -lm
 * Replay of a corpus, or of definition files, without libFuzzer (gcc or
 * clang under UBSan); with no arguments it runs its built-in seeds:
 *     gcc -std=c11 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all
 *         -DYSP_FUZZ_REPLAY -Iinclude -Ipack tests/fuzz/derive_fuzz.c -o replay -lm
 *     ./replay corpus_derive/<files>
 * Seeds: examples/pack/stroop/stroop.json and the definitions that
 * tests/adapt/derive_test.c writes are good starting files.
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

static ytb_table g_t, g_u;
static uint64_t g_ta[2048], g_ua[1024];
static int g_ready;

static void tables(void) {
    static const char t[] = "word,n,x\nred,1,0.5\ngreen,2,0.25\nblue,1,0.5\n$x,3,1\n,4,2\n";
    static const char u[] = "word\nviolet\nred\n";
    ytb_csv_desc d;
    memset(&d, 0, sizeof d);
    d.text = t;
    d.len = sizeof t - 1;
    d.arena = g_ta;
    d.arena_size = sizeof g_ta;
    if (!ytb_open_csv(&g_t, &d)) abort();
    d.text = u;
    d.len = sizeof u - 1;
    d.arena = g_ua;
    d.arena_size = sizeof g_ua;
    if (!ytb_open_csv(&g_u, &d)) abort();
    g_ready = 1;
}

static const void* table_of(void* ctx, const char* name, size_t* n) {
    (void)ctx;
    if (!strcmp(name, "t.pstb")) { *n = g_t.size; return g_t.base; }
    if (!strcmp(name, "u.pstb")) { *n = g_u.size; return g_u.base; }
    return NULL;
}

static void one(const uint8_t* data, size_t size) {
    ypt_texts t;
    char err[512];
    int rc, i;
    if (!g_ready) tables();
    rc = ypt_derive_texts(data, size, table_of, NULL, &t, err, sizeof err);
    if (rc) {
        if (rc > 0 || !err[0] || t.mem_ || t.n_texts) abort();
        return;
    }
    if (err[0] || (!t.definition && (t.n_texts || t.n_tables))) abort();
    for (i = 0; i < t.n_texts; i++) {
        const ypt_text* x = &t.texts[i];
        static char kb[1 << 16];
        int64_t kn;
        int q;
        if (!x->key || !x->stimulus || !x->text || !x->font || !x->size || !x->width || !x->line_height || !x->align || !x->dir ||
            !x->lang || !x->font[0] || x->n_stimuli < 1 || strcmp(x->stimuli[0], x->stimulus))
            abort();
        for (q = 1; q < x->n_stimuli; q++)
            if (strcmp(x->stimuli[q - 1], x->stimuli[q]) >= 0) abort();
        kn = ypak_runs_key(&x->style, x->text, strlen(x->text), kb, sizeof kb);
        if (kn < 0 || (size_t)kn >= sizeof kb || strcmp(kb, x->key) || x->text != x->key + kn - (int64_t)strlen(x->text)) abort();
        if (i && strcmp(t.texts[i - 1].key, x->key) >= 0) abort();
    }
    for (i = 0; i < t.n_tables; i++)
        if (strcmp(t.tables[i], "t.pstb") && strcmp(t.tables[i], "u.pstb")) abort();
    ypt_texts_free(&t);
}

#if defined(YSP_FUZZ_REPLAY)
static const char* const seeds[] = {
    "{\"format\": \"ysp-experiment\", \"version\": 1, \"stimuli\": {\"s\": {\"kind\": \"text\", \"font\": \"f\", \"size\": 3, \"text\": "
    "{\"by\": \"$a\", \"map\": {\"x\": \"$word\", \"y\": \"$$1\", \"z\": \"$v\"}}}}, \"timeline\": [{\"timeline_variables\": \"t.pstb\", "
    "\"vars\": {\"v\": {\"choice\": [\"$word\", \"q\"]}}, \"timeline\": [{\"timeline_variables\": \"u.pstb\"}]}]}",
    "{\"format\": \"ysp-experiment\", \"version\": 1, \"stimuli\": {\"s\": {\"kind\": \"text\", \"font\": \"f\", \"size\": 3, \"text\": \"$n\"}}, "
    "\"timeline\": [{\"timeline_variables\": \"t.pstb\"}]}",
    "{\"format\": \"ysp-experiment\", \"version\": 1, \"stimuli\": {\"s\": {\"kind\": \"text\", \"font\": \"f\", \"size\": 3, \"text\": \"$a\"}}, "
    "\"timeline\": [{\"vars\": {\"a\": \"$b\", \"b\": \"$a\"}}]}",
    "{\"trials\": \"x\"}", "not json", "",
};

static uint8_t* load(const char* path, size_t* n) {
    FILE* f = fopen(path, "rb");
    uint8_t* d;
    long sz;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    d = (uint8_t*)malloc((size_t)sz + 1);
    if (!d || (sz > 0 && fread(d, 1, (size_t)sz, f) != (size_t)sz)) { fclose(f); free(d); return NULL; }
    fclose(f);
    *n = (size_t)sz;
    return d;
}

int main(int argc, char** argv) {
    int i;
    if (argc < 2) {
        for (i = 0; i < (int)(sizeof seeds / sizeof seeds[0]); i++) one((const uint8_t*)seeds[i], strlen(seeds[i]));
        printf("replayed %d built-in seeds\n", (int)(sizeof seeds / sizeof seeds[0]));
        return 0;
    }
    for (i = 1; i < argc; i++) {
        size_t n;
        uint8_t* d = load(argv[i], &n);
        if (!d) { fprintf(stderr, "cannot read %s\n", argv[i]); return 1; }
        one(d, n);
        free(d);
    }
    printf("replayed %d inputs\n", argc - 1);
    return 0;
}
#else
int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    one(data, size);
    return 0;
}
#endif
