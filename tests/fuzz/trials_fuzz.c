/* trials_fuzz.c - fuzz target for ysp/trials.h's rules text, open()
 * and a run. The first input byte is the participant number, the rest is
 * rules text applied with ytr_rules() against a fixed table; when the
 * rules parse, ytr_open() runs with a small repair budget, then up to 300
 * trials with re-queues, every jitter's draw (v0.2.1), every variable and
 * the trial seed (v0.6; the seed is the first byte, the display's rate
 * 60000/1001 for rate=display), every walk's point (v0.8), ytr_format_rules()
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
 * Self-driven (no libFuzzer: gcc; mutations of the seeds and random
 * bytes, N iterations, seed S; on the Linux desktop ASan needs setarch -R):
 *     gcc -std=c11 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all
 *         -DYSP_FUZZ_SELF -Iinclude tests/fuzz/trials_fuzz.c -o trials_self -lm
 *     setarch -R ./trials_self N S
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
static long g_opened, g_walks;   /* self-driven counts: sessions opened, walk points read */

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
    d.seed = data[0];                    /* v0.6: the trial streams */
    d.display_rate_num = 60000;          /* for rate=display */
    d.display_rate_den = 1001;
    if (d.max_swaps == 0 || d.max_swaps > 2000) d.max_swaps = 2000;   /* keep an input fast */
    if (!ytr_open(&g_t, &d)) return 0;
    g_opened++;
    g_sink += (unsigned)ytr_format_rules(&g_t, g_buf, sizeof(g_buf));
    g_sink += (unsigned)ytr_format_meta(&g_t, g_buf, sizeof(g_buf));
    for (i = 0; i < 300 && ytr_next(&g_t, &ti) >= 0; i++) {
        int j;
        for (j = 0; j < 4; j++) g_sink += (unsigned)ytr_jitter(&g_t, ti.index, j).ns;
        for (j = 0; j < 4; j++) {
            const char* v = ytr_var_text(&g_t, ti.index, j);
            g_sink += (unsigned)ytr_var(&g_t, ti.index, j) + (v ? (unsigned)v[0] : 0u);
        }
        g_sink += (unsigned)ytr_trial_seed(&g_t, ti.index);
        for (j = 0; j < 4; j++) {
            double w[5];
            if (ytr_var_walk(&g_t, ti.index, j, w) == 0) {
                g_sink += (unsigned)(w[0] > w[2]) + (unsigned)(w[4] > 0);
                g_walks++;
            }
        }
        if (i == 60 && ti.index > 2) {
            /* an earlier trial's point: walked again from trial 0 */
            double w[5];
            for (j = 0; j < 4; j++) if (ytr_var_walk(&g_t, ti.index - 2, j, w) == 0) g_sink += (unsigned)(w[1] > 0);
        }
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

#if defined(YSP_FUZZ_SELF)
static const char* const* fz_rules(size_t* n);
#endif
#if defined(YSP_FUZZ_SELF)
static uint64_t g_rng = 20261111u;
static uint64_t fz_mix(void) {
    uint64_t z = (g_rng += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}
int main(int argc, char** argv) {
    static uint8_t buf[1 << 12];
    static const char* const words[] = { "walk2d", "choice", "var", "-0", "1e308", "nan", "0", "-10", "10", "\n",
                                         " ", "\"", "=", "reps", "order", "jitter", "1e-300", "2" };
    long iters = argc > 1 ? atol(argv[1]) : 100000, it;
    size_t nr, n, k;
    const char* const* rules = fz_rules(&nr);
    if (argc > 2) g_rng = (uint64_t)strtoull(argv[2], NULL, 10);
    g_walks = 0;
    for (it = 0; it < iters; it++) {
        uint64_t m = fz_mix();
        buf[0] = (uint8_t)fz_mix();
        if (m % 8 == 0) {
            n = 1 + (size_t)(fz_mix() % 512);
            for (k = 1; k < n; k++) buf[k] = (uint8_t)fz_mix();
        } else {
            /* a seed with words and bytes changed, inserted or cut */
            /* the three walk seeds (the last) half the time: v0.8's code */
            const char* r0 = (m >> 8) % 2 ? rules[nr - 1 - (m >> 16) % 3] : rules[(m >> 16) % nr];
            int edits = 1 + (int)(fz_mix() % 6), e;
            n = 1 + strlen(r0);
            memcpy(buf + 1, r0, n - 1);
            for (e = 0; e < edits; e++) {
                size_t at = 1 + (size_t)(fz_mix() % (n > 1 ? n - 1 : 1));
                switch (fz_mix() % 5) {
                case 0: buf[at] = (uint8_t)fz_mix(); break;
                case 1: {
                    const char* w = words[fz_mix() % (sizeof(words) / sizeof(words[0]))];
                    size_t wl = strlen(w);
                    if (n + wl < sizeof(buf)) { memmove(buf + at + wl, buf + at, n - at); memcpy(buf + at, w, wl); n += wl; }
                    break;
                }
                case 2: if (n + 1 < sizeof(buf)) { memmove(buf + at + 1, buf + at, n - at); buf[at] = (uint8_t)(32 + fz_mix() % 95); n++; } break;
                case 3: {
                    /* a digit changed: numbers near the box's limits */
                    size_t q;
                    for (q = at; q < n && !(buf[q] >= '0' && buf[q] <= '9'); q++) {}
                    if (q < n) buf[q] = (uint8_t)('0' + fz_mix() % 10);
                    break;
                }
                default: if (n > 2 && at < n) { memmove(buf + at, buf + at + 1, n - at - 1); n--; } break;
                }
            }
        }
        LLVMFuzzerTestOneInput(buf, n);
    }
    printf("trials_fuzz: %ld inputs, %ld opened, %ld walk points read\n", iters, g_opened, g_walks);
    return 0;
}
static const char* const* fz_rules(size_t* n) {
#else
#if defined(YSP_FUZZ_SEEDS)
int main(int argc, char** argv) {
#endif
#endif
#if defined(YSP_FUZZ_SEEDS) || defined(YSP_FUZZ_SELF)
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
        "order random\nweight n\nreps 3\nvar side choice left right\njitter soa uniform 0.8 1.2 rate=display\n",
        "order full_random\nreps 2\nvar a choice \"x y\" \"c,d\" \"q\"\"t\" e\nvar b choice 1 2 3 1\n",
        "order random\nreps 3\nvar aim walk2d 12 -10 10 -10 10 0 0\n",
        "order random\nreps 2\nvar side choice l r\nvar aim walk2d 1.5 -2 2 -1 1 0.5 0\nvar b walk2d 3 0 10 0 0 5 0\n",
        "order sequential\nreps 4\nvar w walk2d 0.1 0 1 0 1 1 1\njitter iti uniform 0.5 1\n",
    };
#if defined(YSP_FUZZ_SELF)
    *n = sizeof(rules) / sizeof(rules[0]);
    return rules;
}
#else
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
#endif
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
