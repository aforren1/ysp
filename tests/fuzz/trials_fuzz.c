/* trials_fuzz.c - fuzz target for ysp/trials.h's rules text, open()
 * and a run. The first input byte is the participant number, the rest is
 * rules text applied with ytr_rules() against a fixed table; when the
 * rules parse, ytr_open() runs with a small repair budget, then up to 300
 * trials with re-queues, every jitter's draw (v0.2.1), ytr_format_rules()
 * and ytr_format_meta(), and a save and load of the session. Every input must return without a
 * sanitizer report; a refusal is a correct answer.
 *
 * libFuzzer, MSVC 2022 x64 (from the repository root, in a vcvars shell):
 *     cl /nologo /O1 /Zi /fsanitize=address /fsanitize=fuzzer /Iinclude tests\fuzz\trials_fuzz.c
 *     trials_fuzz.exe -max_total_time=1200 -timeout=10 corpus_trials
 * libFuzzer, clang:
 *     clang -O1 -g -fsanitize=fuzzer,address,undefined -Iinclude tests/fuzz/trials_fuzz.c -o trials_fuzz -lm
 * Seeds (no corpus is committed; the target writes its own):
 *     cc -DYSP_FUZZ_SEEDS -Iinclude tests/fuzz/trials_fuzz.c -o seeds -lm && ./seeds corpus_trials
 * Replay of a corpus without libFuzzer (gcc or clang under UBSan), every
 * file of the directory as an argument:
 *     gcc -std=c11 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all
 *         -DYSP_FUZZ_REPLAY -Iinclude tests/fuzz/trials_fuzz.c -o replay -lm
 *     ./replay corpus_trials/<files>
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#define YSP_TRIALS_IMPLEMENTATION
#include "ysp/trials.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t g_table_arena[(1u << 16) / 8];
static uint64_t g_rules_arena[(1u << 17) / 8];
static unsigned char g_snap[1 << 18];
static char g_buf[8192];
static ytr_trials g_t, g_u;
static ytb_table g_fixed;
static volatile unsigned g_sink;

static const char g_csv[] =
    "target,contrast,word,list,n,pair\n"
    "a,0.25,cat,1,2,p1\na,0.5,dog,1,1,p1\nb,0.25,\"new york\",2,2,\nb,0.5,ox,2,1,\n"
    "catch,0,emu,1,1,p2\ncatch,0,yak,2,1,p2\n";

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);
int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    ytr_desc d;
    ytr_rules_desc rd;
    ytr_trial_info ti;
    char err[256];
    uint64_t seed = 5, seed2;
    int i, len;
    if (!g_fixed.base) {
        ytb_csv_desc cd;
        memset(&cd, 0, sizeof(cd));
        cd.text = g_csv;
        cd.len = sizeof(g_csv) - 1;
        cd.arena = g_table_arena;
        cd.arena_size = sizeof(g_table_arena);
        if (!ytb_open_csv(&g_fixed, &cd)) abort();
    }
    if (size < 1) return 0;
    memset(&d, 0, sizeof(d));
    memset(&rd, 0, sizeof(rd));
    rd.text = (const char*)data + 1;
    rd.len = size - 1;
    rd.table = &g_fixed;
    rd.participant = data[0];
    rd.arena = g_rules_arena;
    rd.arena_size = sizeof(g_rules_arena);
    if (ytr_rules(&d, &rd, err, sizeof(err)) != 0) return 0;
    d.rng = ytr_splitmix;
    d.rng_ctx = &seed;
    if (d.max_swaps == 0 || d.max_swaps > 2000) d.max_swaps = 2000;   /* keep an input fast */
    if (!ytr_open(&g_t, &d)) return 0;
    g_sink += (unsigned)ytr_format_rules(&g_t, g_buf, sizeof(g_buf));
    g_sink += (unsigned)ytr_format_meta(&g_t, g_buf, sizeof(g_buf));
    for (i = 0; i < 300 && ytr_next(&g_t, &ti) >= 0; i++) {
        int j;
        for (j = 0; j < 4; j++) g_sink += (unsigned)ytr_jitter(&g_t, ti.index, j).ns;
        if (i == 40) {
            /* A save and a load mid-run, then the loaded copy continues. */
            len = ytr_save(&g_t, g_snap, sizeof(g_snap));
            seed2 = seed;
            d.rng_ctx = &seed2;
            if (len > 0 && ytr_open_saved(&g_u, &d, g_snap, (size_t)len))
                g_sink += (unsigned)ytr_next(&g_u, NULL);
            d.rng_ctx = &seed;
        }
        if (i % 7 == 3 && ytr_requeue(&g_t) == 0) continue;
        if (i % 11 == 5) ytr_mark_break(&g_t);
        ytr_update(&g_t, i % 3, NULL);
        if (ytr_format_row(&g_t, i, g_buf, sizeof(g_buf)) > 0) g_sink += (unsigned)g_buf[0];
    }
    return 0;
}

#if defined(YSP_FUZZ_SEEDS)
int main(int argc, char** argv) {
    static const char* const rules[] = {
        "order constrained\nreps 4\nmax_run target 1\n",
        "order constrained\nreps 2\nfollowed_by target a b\nmax_run target 2\n",
        "order constrained\nreps 1\nchunk pair\n",
        "order constrained\nreps 3\nbalance list\nmax_run target 2\n",
        "order constrained\nreps 2\nbalance target no_repeat no_leadin\n",
        "order full_random\nreps 2\ngroups list blocked balanced_latin\nwarmup 1\n",
        "order constrained\nreps 2\ngroups target alternate random\nmax_run word 1\n",
        "order with_replacement\ndraws 20 weights=n\nsubset 3\n",
        "where list=@participant\nweight n\norder random\npractice 2 from target=catch\n",
        "order balanced_latin\n",
        "list 0 1 2\nlist 5 5\norder list\n",
        "constrain target maxrep=1 mindist=2\norder constrained\nreps 3\n",
        "# comment\nmin_gap word=\"new york\" 2\norder constrained\nreps 4\nblock_size 6\nwarmup 1\nrequeue_gap 2\n",
        "order constrained\nreps 3\npreceded_by target b a\nmax_in_window @row=4 3 1\nfirst_not contrast=0\n",
        "order constrained\ncond_reps 1 2 0 3 1 1\nno_transition target a b\nspan_blocks\nblock_size 4\n",
        "order full_random\nreps 2\njitter iti uniform 0.8 1.2 rate=60000/1001\njitter fp exponential 0.5 2 0.4\n",
        "order random\nreps 2\njitter soa choice 0.1 0.2 0.4 rate=60\njitter w exponential contrast 1 0.2 rate=120\n",
        "order sequential\nreps 1\npractice 2\njitter a uniform contrast n\nrequeue_gap 1\n",
    };
    size_t i;
    char path[1024];
    if (argc < 2) { fprintf(stderr, "usage: seeds DIR\n"); return 1; }
    for (i = 0; i < sizeof(rules) / sizeof(rules[0]); i++) {
        FILE* f;
        unsigned char p = (unsigned char)i;
        snprintf(path, sizeof(path), "%s/rules%02u", argv[1], (unsigned)i);
        f = fopen(path, "wb");
        if (!f) { perror(path); return 1; }
        fwrite(&p, 1, 1, f);
        fwrite(rules[i], 1, strlen(rules[i]), f);
        fclose(f);
    }
    printf("trials_fuzz: wrote %u seeds to %s\n", (unsigned)i, argv[1]);
    return 0;
}
#elif defined(YSP_FUZZ_REPLAY)
int main(int argc, char** argv) {
    static uint8_t buf[1 << 20];
    int i, done = 0;
    for (i = 1; i < argc; i++) {
        FILE* f = fopen(argv[i], "rb");
        size_t n;
        if (!f) continue;
        n = fread(buf, 1, sizeof(buf), f);
        fclose(f);
        LLVMFuzzerTestOneInput(buf, n);
        done++;
    }
    printf("trials_fuzz: replayed %d inputs\n", done);
    return 0;
}
#endif
