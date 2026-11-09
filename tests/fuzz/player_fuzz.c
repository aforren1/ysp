/* player_fuzz.c - fuzz target over the player: load, lower and a short --sim
 * run of a definition. A definition is untrusted input from another lab's
 * pack, so a mutated one must return an error or run to its end with no
 * sanitizer report.
 *
 * Self-driven (no libFuzzer here). Token-level mutations of the definitions
 * under tests/player keep many inputs loading, so the run path is reached.
 * Build (needs the pack files from `ypak build` of each folder under
 * tests/player):
 *     cmake --build BUILD --target player_fuzz
 *     setarch -R BUILD/player/player_fuzz N SEED WORKDIR RIG FILES.ysppak...
 * with N iterations, SEED for the mutations, WORKDIR a scratch folder that
 * holds the data sets, a rig profile (make_rig; the 2AFC and oddball packs
 * need a view and a calibration), and the packs. Each pack gives
 * its experiment entry as the seed text; runs reuse the pack's tables.
 */
#include "player.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_SEEDS 16
#define MAX_TEXT (1 << 20)

typedef struct seed_t { char* text; size_t n; const char* pack; } seed_t;

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

static void one(const seed_t* s, const char* text, size_t n, const char* out) {
    pl_args a;
    memset(&a, 0, sizeof a);
    a.pack_path = s->pack;
    a.out_dir = out;
    a.sim = true;
    a.have_seed = true;
    a.seed = rnd();
    a.max_trials = 6;
    a.max_seconds = 120;
    a.participant = "correct";
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
    for (k = 5; k < argc && n_seeds < MAX_SEEDS; k++) {
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
        n_seeds++;
        ypak_close(&pk);
    }
    yrt_virtual_begin(0);
    /* a data set folder inside the caller's scratch folder; its name is fixed
     * so that the delete below cannot reach anything else */
    snprintf(out, sizeof out, "%s/ds", argv[3]);
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
                memcpy(mutated, scratch, n);
                n = (rnd() % 5 == 0) ? mutate_byte(scratch, mutated, n) : mutate(scratch, mutated, n);
            }
            one(s, scratch, n, out);
        }
        if (i && i % 2000 == 0) fprintf(stderr, "player_fuzz: %lld iterations, %lld loaded, %lld ran to the end\n", i, n_loaded, n_ok);
    }
    printf("player_fuzz: %lld iterations, %lld loaded, %lld ran to the end\n", iters, n_loaded, n_ok);
    for (k = 0; k < n_seeds; k++) free(seeds[k].text);
    return 0;
}
