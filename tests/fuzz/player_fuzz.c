/* player_fuzz.c - fuzz target over the player: load, lower and a --sim run
 * of a definition. A definition is untrusted input from another lab's pack,
 * so a mutated one must return an error or run to its end with no sanitizer
 * report.
 *
 * Self-driven (no libFuzzer here). The seeds are every definition that
 * tests/fuzz/player_corpus.py generates (one per key the format defines), and
 * the experiment entries of packs. Mutations swap tokens, flip bytes, insert
 * a key from ysp/experiment.h's table at an object's start, and delete a
 * key. Each input runs with a budget of virtual seconds and, on most inputs,
 * no cap on trials.
 *
 * Usage: player_fuzz N SEED WORKDIR RIG PACK... DIR:PACK...
 *   N iterations (0: run each seed once, print its result), SEED of the
 *   mutations, WORKDIR a scratch folder for the data sets, RIG a rig
 *   profile (player_make_rig), PACK a built pack whose experiment is a seed,
 *   DIR:PACK a folder of *.json seeds that run against PACK's tables.
 */
#include "player.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_SEEDS 4096
#define MAX_TEXT (1 << 20)

typedef struct seed_t { char* text; size_t n; const char* pack; const char* name; } seed_t;

static seed_t seeds[MAX_SEEDS];
static int n_seeds;
static uint64_t rng;
static char mutated[MAX_TEXT];

static uint64_t rnd(void) {
    uint64_t z = (rng += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

static size_t tok_start(const char* s, size_t n, size_t at) {
    while (at < n && !(s[at] == '"' || (s[at] >= '0' && s[at] <= '9'))) at++;
    return at;
}

static size_t tok_end(const char* s, size_t n, size_t at) {
    size_t i = at;
    if (at >= n) return at;
    if (s[at] == '"') {
        for (i = at + 1; i < n && s[i] != '"'; i++) if (s[i] == '\\') i++;
        return i < n ? i + 1 : n;
    }
    while (i < n && ((s[i] >= '0' && s[i] <= '9') || s[i] == '.' || s[i] == '-' || s[i] == 'e')) i++;
    return i;
}

/* Replace token a with a copy of token b from the same text. */
static size_t mutate(char* dst, const char* src, size_t n) {
    size_t a = tok_start(src, n, rnd() % n);
    size_t b = tok_start(src, n, rnd() % n);
    size_t ae = tok_end(src, n, a), be = tok_end(src, n, b);
    size_t len = a + (be - b) + (n - ae);
    if (a >= n || b >= n || len >= MAX_TEXT) { memcpy(dst, src, n); return n; }
    memcpy(dst, src, a);
    memcpy(dst + a, src + b, be - b);
    memcpy(dst + a + (be - b), src + ae, n - ae);
    return len;
}

static size_t mutate_byte(char* dst, const char* src, size_t n) {
    size_t a = rnd() % n;
    memcpy(dst, src, n);
    dst[a] = (char)rnd();
    return n;
}

static void rmdir_recursive(const char* path) {
    DIR* d = opendir(path);
    if (!d) { remove(path); return; }
    for (struct dirent* e = readdir(d); e; e = readdir(d)) {
        char sub[512];
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(sub, sizeof sub, "%s/%s", path, e->d_name);
        rmdir_recursive(sub);
    }
    closedir(d);
    rmdir(path);
}

static pl_player p;

static const char* rig_path;
static long long n_loaded, n_run, n_ok;
static int g_print;
static const char* dict_keys[2048];
static int n_dict;
static char dict_buf[1 << 20];

/* the format's keys and defaults: "key":value text */
static void dict_build(void) {
    int n, i;
    size_t used = 0;
    const yexp_row* r = yexp_defaults(1, &n);
    for (i = 0; i < n && n_dict < 2048; i++) {
        int k;
        if (!r[i].def) continue;
        k = snprintf(dict_buf + used, sizeof dict_buf - used, "\"%s\":%s,", r[i].key, r[i].def);
        if (k <= 0 || used + (size_t)k + 1 >= sizeof dict_buf) break;
        dict_keys[n_dict++] = dict_buf + used;
        used += (size_t)k + 1;
    }
}

/* Insert a dictionary "key":value after a random '{'. */
static size_t mutate_insert(char* dst, const char* src, size_t n) {
    size_t a = rnd() % n, i;
    const char* ins = dict_keys[rnd() % (uint64_t)n_dict];
    size_t len = strlen(ins);
    for (i = a; i < n; i++) if (src[i] == '{') break;
    if (i >= n || n + len + 1 >= MAX_TEXT || !n_dict) { memcpy(dst, src, n); return n; }
    memcpy(dst, src, i + 1);
    memcpy(dst + i + 1, ins, len);
    memcpy(dst + i + 1 + len, src + i + 1, n - i - 1);
    return n + len;
}

/* Delete a "key":value, pair at a random comma. */
static size_t mutate_delete(char* dst, const char* src, size_t n) {
    size_t a = rnd() % n, i, e, depth = 0;
    for (i = a; i < n; i++) if (src[i] == '"') break;
    if (i >= n) { memcpy(dst, src, n); return n; }
    for (e = i; e < n; e++) {
        if (src[e] == '{' || src[e] == '[') depth++;
        else if (src[e] == '}' || src[e] == ']') { if (depth == 0) break; depth--; }
        else if (src[e] == ',' && depth == 0) { e++; break; }
    }
    memcpy(dst, src, i);
    memcpy(dst + i, src + e, n - e);
    return n - (e - i);
}

static void one(const seed_t* s, const char* text, size_t n, const char* out) {
    pl_args a;
    memset(&a, 0, sizeof a);
    a.pack_path = s->pack;
    a.out_dir = out;
    a.sim = true;
    a.have_seed = true;
    a.seed = rnd();
    /* no cap on trials on most inputs; only the virtual-time budget ends a run */
    a.max_trials = rnd() % 4 == 0 ? 64 : 0;
    a.max_seconds = 300;
    {
        static const char* const parts[] = { "correct", "silent", "p=0.5" };
        a.participant = parts[rnd() % 3];
    }
    a.rt_s = 0.1;
    a.def_text = text;
    a.def_len = n;
    a.rig_path = rig_path;
    a.n_fields = 1;
    a.fields[0].name = "participant";
    a.fields[0].value = "1";
    rmdir_recursive(out);
    if (pl_load(&p, &a) == 0) {
        n_loaded++;
        n_run++;
        if (pl_run(&p) == 0) n_ok++;
        else if (g_print) printf("  RUN FAILED: %.200s\n", p.error);
    } else if (g_print) {
        printf("  refused: %.200s\n", p.error);
    }
    pl_close(&p);
    rmdir_recursive(out);
}

int main(int argc, char** argv) {
    long long iters, i;
    int k;
    char out[128];
    if (argc < 6) { fprintf(stderr, "usage: player_fuzz N SEED WORKDIR RIG PACK...\n"); return 2; }
    iters = atoll(argv[1]);
    rng = strtoull(argv[2], NULL, 10);
    rig_path = argv[4];
    for (k = 5; k < argc; k++) {
        const char* colon = strchr(argv[k], ':');
        if (colon) {
            /* DIR:PACK */
            char dir[1024], path[2048];
            DIR* dd;
            struct dirent* de;
            snprintf(dir, sizeof dir, "%.*s", (int)(colon - argv[k]), argv[k]);
            if (!(dd = opendir(dir))) { fprintf(stderr, "cannot read %s\n", dir); return 2; }
            while ((de = readdir(dd)) && n_seeds < MAX_SEEDS) {
                size_t L = strlen(de->d_name);
                FILE* fp;
                char* c;
                long sz;
                if (L < 5 || strcmp(de->d_name + L - 5, ".json")) continue;
                snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
                fp = fopen(path, "rb");
                if (!fp) continue;
                fseek(fp, 0, SEEK_END); sz = ftell(fp); fseek(fp, 0, SEEK_SET);
                c = (char*)malloc((size_t)sz);
                if (fread(c, 1, (size_t)sz, fp) != (size_t)sz) { fclose(fp); free(c); continue; }
                fclose(fp);
                seeds[n_seeds].text = c;
                seeds[n_seeds].n = (size_t)sz;
                seeds[n_seeds].pack = colon + 1;
                { char* nm = (char*)malloc(strlen(de->d_name) + 1); strcpy(nm, de->d_name); seeds[n_seeds].name = nm; }
                n_seeds++;
            }
            closedir(dd);
            continue;
        }
        {
        static ypak_pack pk;
        ypak_desc d;
        ypak_entry e;
        const void* data;
        char* c;
        memset(&d, 0, sizeof d);
        d.path = argv[k];
        if (!ypak_open(&pk, &d)) { fprintf(stderr, "%s: %s\n", argv[k], ypak_error(&pk)); return 2; }
        if (ypak_first_of(&pk, YPAK_KIND_EXPERIMENT, &e) != 0 || !(data = ypak_data(&pk, &e))) { fprintf(stderr, "%s: no experiment\n", argv[k]); return 2; }
        c = (char*)malloc((size_t)e.size);
        memcpy(c, data, (size_t)e.size);
        seeds[n_seeds].text = c;
        seeds[n_seeds].n = (size_t)e.size;
        seeds[n_seeds].pack = argv[k];
        seeds[n_seeds].name = argv[k];
        n_seeds++;
        ypak_close(&pk);
        }
    }
    dict_build();
    yrt_virtual_begin(0);
    /* a data set folder inside the caller's scratch folder; its name is fixed
     * so that the delete below cannot reach anything else */
    snprintf(out, sizeof out, "%s/ds", argv[3]);
    if (iters == 0) {
        g_print = 1;
        for (i = 0; i < n_seeds; i++) {
            printf("%s\n", seeds[i].name);
            one(&seeds[i], seeds[i].text, seeds[i].n, out);
        }
        printf("player_fuzz: %d seeds, %lld loaded, %lld ran to the end\n", n_seeds, n_loaded, n_ok);
        return 0;
    }
    for (i = 0; i < iters; i++) {
        const seed_t* s = &seeds[rnd() % (uint64_t)n_seeds];
        int m = (int)(rnd() % 8);
        size_t n = 0;
        memcpy(mutated, s->text, s->n);
        n = s->n;
        if (m == 0) {
            one(s, mutated, n, out);   /* the seed unchanged */
        } else {
            int j, count = 1 + (int)(rnd() % 3);
            static char scratch[MAX_TEXT];
            memcpy(scratch, s->text, s->n);
            n = s->n;
            for (j = 0; j < count; j++) {
                int r = (int)(rnd() % 10);
                memcpy(mutated, scratch, n);
                if (r < 5) n = mutate(scratch, mutated, n);
                else if (r < 7) n = mutate_insert(scratch, mutated, n);
                else if (r < 9) n = mutate_delete(scratch, mutated, n);
                else n = mutate_byte(scratch, mutated, n);
            }
            one(s, scratch, n, out);
        }
        if (i && i % 2000 == 0) fprintf(stderr, "player_fuzz: %lld iterations, %lld loaded, %lld ran to the end\n", i, n_loaded, n_ok);
    }
    printf("player_fuzz: %lld iterations, %lld loaded, %lld ran to the end\n", iters, n_loaded, n_ok);
    for (k = 0; k < n_seeds; k++) free(seeds[k].text);
    return 0;
}
