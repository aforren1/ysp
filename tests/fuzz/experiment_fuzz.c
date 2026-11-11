/* experiment_fuzz.c - fuzz target for ysp/experiment.h. A definition comes
 * from another lab inside a pack, so its JSON is untrusted input to the
 * loader. The input is the definition; a host with five tables and a few
 * pack entries (tests/adapt/experiment_host.h) runs on most inputs and no
 * host on the rest, so `$name` reaches levels and the second pass runs. Every input must return without a sanitizer report. A
 * refusal must carry messages, each one line; a load must give a form that
 * dumps, canonical bytes that load again to the same form, the same bytes
 * and the same ID, and for a version 1 file migrated bytes (format 3.2) that
 * load as version 2 and give themselves back. Then the input runs again in a small arena with a host
 * that misbehaves (tests/adapt/experiment_host.h), for the report's bounds
 * and for no sanitizer report. A NOMEM in the first, large arena is a bug.
 *
 * libFuzzer, clang:
 *     clang -O1 -g -fsanitize=fuzzer,address,undefined -Iinclude tests/fuzz/experiment_fuzz.c -o experiment_fuzz -lm
 * Self-driven (no libFuzzer: gcc; token-level mutations of the seed files,
 * which keep most inputs valid JSON, so loads succeed as well as fail; on
 * this kernel ASan needs setarch -R):
 *     gcc -std=c11 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -DYSP_FUZZ_SELF \
 *         -Iinclude -Itests/adapt tests/fuzz/experiment_fuzz.c -o exp_self -lm
 *     setarch -R ./exp_self 1000000 1 tests/experiment/good/x.json ... (the seed files)
 *   (iterations, RNG seed, then the seed files; repeat a file to weigh it)
 * Replay of files: -DYSP_FUZZ_REPLAY in place of -DYSP_FUZZ_SELF, the files
 * as arguments.
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#define YSP_JSON_IMPLEMENTATION
#include "ysp/json.h"
/* An endless host costs a full work limit per input; 5,000,000 would make it
 * the whole run. */
#define YEXP_WORK_STEPS 200000L
#define YSP_EXPERIMENT_IMPLEMENTATION
#include "ysp/experiment.h"
#include "../adapt/experiment_dump.h"
#include "../adapt/experiment_host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARENA_BYTES (32u << 20)
static uint64_t g_mem[ARENA_BYTES / 8];
static uint64_t g_mem2[ARENA_BYTES / 8];
static uint64_t g_mem3[(1u << 20) / 8];
static yexp_report g_r, g_r2;
static long long g_loaded, g_refused, g_nomem, g_warned, g_migrated, g_loaded2;

static void bad(const char* what, const char* text, size_t n) {
    fprintf(stderr, "experiment_fuzz: %s\ninput (%lu bytes): %.*s\n", what, (unsigned long)n, (int)(n > 4000 ? 4000 : n), text);
    abort();
}

static void check_report(const yexp_report* r, const char* text, size_t n) {
    int i;
    if (r->n_diags > YEXP_MAX_DIAGS || r->n_diags < 0 || r->n_errors < 0 || r->n_warnings < 0) bad("report counts", text, n);
    for (i = 0; i < r->n_diags; i++) {
        const char* m = r->diags[i].msg;
        if (!m[0] || strchr(m, '\n') || strlen(m) >= YEXP_MSG_BYTES) bad("a diagnostic is not one line", text, n);
    }
}

static void run_one(const char* text, size_t n) {
    const yexp_host* host = n % 16 ? &g_host : NULL;
    yjs_arena a, a2;
    yexp_def* d = NULL;
    yexp_def* d2 = NULL;
    int rc;
    yjs_arena_init(&a, g_mem, sizeof g_mem);
    rc = yexp_load(&a, text, n, host, &d, &g_r);
    check_report(&g_r, text, n);
    /* an input of at most 64 KB fits this arena: a NOMEM here is a false one */
    if (rc == YEXP_ERR_NOMEM) bad("out of memory in a large arena", text, n);
    if (rc == YEXP_ERR_REFUSED) {
        g_refused++;
        if (d || g_r.n_errors < 1 || g_r.n_diags < 1) bad("a refusal without an error", text, n);
        return;
    }
    if (rc != YEXP_OK || !d || g_r.n_errors != 0) bad("a load with errors", text, n);
    g_loaded++;
    g_warned += g_r.n_warnings;
    {
        char* dump1 = xd_dump(d);
        char* dump2;
        const char* b1;
        const char* b2;
        size_t n1, n2;
        uint8_t id1[32], id2[32];
        if (yexp_canonical(d, &a, &b1, &n1, id1) != YEXP_OK || !n1 || b1[n1 - 1] != '\n') bad("canonical bytes", text, n);
        yjs_arena_init(&a2, g_mem2, sizeof g_mem2);
        rc = yexp_load(&a2, b1, n1, host, &d2, &g_r2);
        if (rc == YEXP_ERR_NOMEM) bad("out of memory reloading in a large arena", text, n);
        if (rc != YEXP_OK) {
            fprintf(stderr, "reload failed: %s\ncanonical: %.2000s\n", g_r2.n_diags ? g_r2.diags[0].msg : "", b1);
            bad("the canonical bytes did not load", text, n);
        }
        dump2 = xd_dump(d2);
        if (strcmp(dump1, dump2) != 0) {
            size_t q = 0;
            while (dump1[q] && dump1[q] == dump2[q]) q++;
            fprintf(stderr, "first difference at byte %lu: '%.80s' against '%.80s'\n", (unsigned long)q, dump1 + q, dump2 + q);
            bad("the canonical bytes gave another form", text, n);
        }
        if (yexp_canonical(d2, &a2, &b2, &n2, id2) != YEXP_OK) bad("canonical of canonical", text, n);
        if (n1 != n2 || memcmp(b1, b2, n1) != 0 || memcmp(id1, id2, 32) != 0) bad("the canonical bytes moved", text, n);
        free(dump1);
        free(dump2);
        if (d->format_version == 1) {
            /* format 3.2: a version 1 file's migration loads as version 2 and
             * gives itself back */
            const char* bm;
            const char* bm2;
            size_t nm, nm2;
            yexp_def* d3 = NULL;
            if (yexp_migrated(d, &a, &bm, &nm, NULL) != YEXP_OK) bad("migrated bytes", text, n);
            yjs_arena_init(&a2, g_mem2, sizeof g_mem2);
            rc = yexp_load(&a2, bm, nm, host, &d3, &g_r2);
            if (rc != YEXP_OK) {
                fprintf(stderr, "migrated reload failed: %s\nmigrated: %.2000s\n", g_r2.n_diags ? g_r2.diags[0].msg : "", bm);
                bad("the migrated bytes did not load as version 2", text, n);
            }
            if (yexp_canonical(d3, &a2, &bm2, &nm2, NULL) != YEXP_OK || nm != nm2 || memcmp(bm, bm2, nm) != 0) bad("the migrated bytes moved", text, n);
            g_migrated++;
        } else {
            g_loaded2++;
        }
    }
}

/* The same input again, in a small arena and with a host that misbehaves:
 * the size and the mode come from the input, so a failure replays. */
static void run_hostile(const char* text, size_t n) {
    uint32_t h = 2166136261u;
    size_t i, cap;
    yjs_arena a;
    yexp_def* d = NULL;
    int rc;
    for (i = 0; i < n; i++) h = (h ^ (unsigned char)text[i]) * 16777619u;
    cap = 8 + (size_t)((h >> 8) % (sizeof g_mem3 - 8));
    if (h & 1u) cap = 8 + cap % 300000;   /* the sizes where a load runs out */
    g_bh_mode = (int)(h % BH_MODES);
    g_bh_calls = 0;
    yjs_arena_init(&a, g_mem3, cap);
    rc = yexp_load(&a, text, n, &g_bad_host, &d, &g_r2);
    check_report(&g_r2, text, n);
    if (rc != YEXP_OK && rc != YEXP_ERR_REFUSED && rc != YEXP_ERR_NOMEM) bad("a result that is no result", text, n);
    if (rc != YEXP_OK && d) bad("a form after a refusal", text, n);
    if (rc == YEXP_ERR_NOMEM) g_nomem++;
    if (rc == YEXP_OK) {
        const char* b;
        size_t bn;
        uint8_t id[32];
        char* s = xd_dump(d);
        rc = yexp_canonical(d, &a, &b, &bn, id);
        if (rc != YEXP_OK && rc != YEXP_ERR_NOMEM) bad("canonical in a small arena", text, n);
        free(s);
    }
    g_bh_mode = 0;
}

int LLVMFuzzerTestOneInput(const unsigned char* data, size_t size) {
    run_one((const char*)data, size);
    run_hostile((const char*)data, size);
    return 0;
}

#if defined(YSP_FUZZ_SELF)
static uint64_t g_rng = 0x5167u;
static uint64_t gen(void) {
    uint64_t z = (g_rng += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

#define CAP 65536
typedef struct seedf { char* p; size_t n; } seedf;
static seedf g_seeds[4096];
static int g_nseeds;

/* Words that reach the format's branches: every key and enum value of the
 * table, the built-ins, value forms, names the seeds define. */
static const char* g_words[4096];
static int g_nwords;

static void word_add(const char* s) { if (g_nwords < 4096) g_words[g_nwords++] = s; }

static void words_init(void) {
    static char store[65536];
    size_t at = 0;
    static const char* const extra[] = {
        "$practice", "$warmup", "$first_in_block", "$after_break", "$trial_index", "$block", "$rep", "$condition",
        "$trial_seed", "$method", "$level", "$estimate", "$response", "$rt_s", "$responded", "$correct", "$outcome",
        "$contrast_shown", "$cond", "$n", "$soa_s", "$participant", "$word", "$ink", "$side", "$$lit", "$", "$bad name",
        "fix", "gabor", "stair", "quest", "resp", "trig", "main", "t", "s", "by", "map", "linspace", "gamut", "michelson",
        "choice", "lua", "script", "until", "states", "exits", "flags", "flag", "recenter", "signal", "when", "note",
        "participant", "true", "false", "correct", "wrong", "timeout", "left", "right", "a", "b", "t.pstb", "stroop.pstb",
        "gabor.contrast", "fix.w", "s.nope", "nope.x", "stair", "in", "out", "both", "keyboard", "no_response", "jitter soa_s uniform 0.1 0.2",
        "var x choice a b", "practice 2", "reps 2", "order random",
        /* version 2 */
        "hand", "hand.x", "hand.y", "hand.z", "cur", "cur.x", "near", "near.x", "cursor", "cursor.x", "goal", "end", "response", "move",
        "$late", "$aim_x", "$aim_from_y", "$aim_deg", "$aim", "$phase_x", "$perturb", "$rotation_deg", "$feedback", "identity",
        "rotation_deg", "mirror_deg", "uniform", "walk2d", "hit", "time", "above", "then", "else", "in", "over", "click_in",
        "assign_roles", "signals", "top_speed", "past_target", "at_start", "moved", "arrived", "a", "b", "go", "back", NULL
    };
    int n = 0, i, ver;
    const yexp_row* rows;
    for (i = 0; extra[i]; i++) word_add(extra[i]);
    for (ver = 1; ver <= 2; ver++) {
    rows = yexp_defaults(ver, &n);
    for (i = 0; i < n; i++) {
        word_add(rows[i].key);
        if (rows[i].values) {
            const char* p = rows[i].values;
            while (p && *p && at < sizeof store - 64) {
                const char* eq = strchr(p, '=');
                const char* end;
                size_t len;
                if (!eq) break;
                end = strchr(eq, ',');
                len = (size_t)(eq - p);
                memcpy(store + at, p, len);
                store[at + len] = 0;
                word_add(store + at);
                at += len + 1;
                p = end ? end + 1 : NULL;
            }
        }
    }
    }
}

static const char* const g_numbers[] = {
    "0", "1", "-1", "2", "0.5", "0.25", "0.1", "0.1000000000000000055", "1.5e0", "1e2", "1e400", "1e-400", "-0", "-0.0",
    "9007199254740993", "12345678901234567890", "1.23456789012345678901", "60.5", "99999999999", "1e30", "3", "7", "1024", "4097",
    "0.000000001", "0.0000000001", "1e-10", "63", "64", "255", "256",
    /* integer-valued reals at and above 2^53, each spelled more than one way */
    "9007199254740992.0", "9007199254740993.0", "9.007199254740993e15", "9007199254740993.5", "1.8014398509481984e16",
    "9223372036854775807.0", "9223372036854775808", "9223372036854775808.0", "-9223372036854775808.0", "-9.22337203685477581e18",
    "-1", "-0.5", "-60.5", "3000000000", "2147483648"
};

typedef struct tok { size_t at, len; char kind; } tok;

/* Tokens of a JSON text: strings ('s'), numbers ('n'), keywords ('w'), and
 * the punctuation ('p'). Invalid text gives fewer tokens: mutations then
 * go by bytes. */
static int tokenize(const char* s, size_t n, tok* out, int cap) {
    size_t i = 0;
    int k = 0;
    while (i < n && k < cap) {
        char ch = s[i];
        if (ch == ' ' || ch == '\n' || ch == '\t' || ch == '\r') { i++; continue; }
        if (ch == '"') {
            size_t j = i + 1;
            while (j < n && s[j] != '"') { if (s[j] == '\\') j++; j++; }
            if (j >= n) return k;
            out[k].at = i; out[k].len = j + 1 - i; out[k].kind = 's'; k++;
            i = j + 1;
        } else if (ch == '-' || (ch >= '0' && ch <= '9')) {
            size_t j = i + 1;
            while (j < n && ((s[j] >= '0' && s[j] <= '9') || strchr(".eE+-", s[j]))) j++;
            out[k].at = i; out[k].len = j - i; out[k].kind = 'n'; k++;
            i = j;
        } else if (ch == 't' || ch == 'f' || ch == 'n') {
            size_t j = i;
            while (j < n && s[j] >= 'a' && s[j] <= 'z') j++;
            out[k].at = i; out[k].len = j - i; out[k].kind = 'w'; k++;
            i = j;
        } else {
            out[k].at = i; out[k].len = 1; out[k].kind = 'p'; k++;
            i++;
        }
    }
    return k;
}

static size_t splice(char* buf, size_t n, size_t at, size_t del, const char* ins, size_t insl) {
    if (at + del > n || n - del + insl >= CAP) return n;
    memmove(buf + at + insl, buf + at + del, n - at - del);
    if (insl) memcpy(buf + at, ins, insl);
    return n - del + insl;
}

/* The end of the JSON value that starts at token t: one past its last token. */
static int value_end(const tok* tk, const char* s, int nt, int t) {
    int depth = 0, i;
    for (i = t; i < nt; i++) {
        if (tk[i].kind == 'p') {
            char ch = s[tk[i].at];
            if (ch == '{' || ch == '[') depth++;
            else if (ch == '}' || ch == ']') { if (depth == 0) return i; depth--; if (depth == 0) return i + 1; }
            else if ((ch == ',') && depth == 0) return i;
        } else if (depth == 0) return i + 1;
    }
    return nt;
}

static size_t mutate(char* buf, size_t n) {
    static tok tk[16384];
    int edits = 1 + (int)(gen() % 3), e;
    for (e = 0; e < edits; e++) {
        int nt;
        uint64_t m = gen() % 100;
        if (n < 1) break;
        nt = tokenize(buf, n, tk, 16384);
        if (nt < 4) m = 99;   /* too few tokens to edit: bytes */
        if (m < 30) {
            /* a string becomes a word of the format */
            int t = (int)(gen() % (uint64_t)nt), tries = 0;
            char q[300];
            while (tk[t].kind != 's' && tries++ < 8) t = (int)(gen() % (uint64_t)nt);
            if (tk[t].kind == 's' && g_nwords) {
                const char* w = g_words[gen() % (uint64_t)g_nwords];
                size_t l = (size_t)snprintf(q, sizeof q, "\"%s\"", w);
                n = splice(buf, n, tk[t].at, tk[t].len, q, l);
            }
        } else if (m < 45) {
            /* a number becomes a tricky number */
            int t = (int)(gen() % (uint64_t)nt), tries = 0;
            while (tk[t].kind != 'n' && tries++ < 8) t = (int)(gen() % (uint64_t)nt);
            if (tk[t].kind == 'n') {
                const char* w = g_numbers[gen() % (sizeof g_numbers / sizeof g_numbers[0])];
                n = splice(buf, n, tk[t].at, tk[t].len, w, strlen(w));
            }
        } else if (m < 60) {
            /* delete a member "key": value, with its comma */
            int t = (int)(gen() % (uint64_t)nt), tries = 0;
            while (!(tk[t].kind == 's' && t + 1 < nt && tk[t + 1].kind == 'p' && buf[tk[t + 1].at] == ':') && tries++ < 16)
                t = (int)(gen() % (uint64_t)nt);
            if (tk[t].kind == 's' && t + 1 < nt && buf[tk[t + 1].at] == ':') {
                int end = value_end(tk, buf, nt, t + 2);
                size_t a = tk[t].at, b = end < nt ? tk[end].at : n;
                if (end < nt && buf[tk[end].at] == ',') b = tk[end].at + 1;
                n = splice(buf, n, a, b - a, "", 0);
            }
        } else if (m < 75) {
            /* insert a member after an opening brace */
            int t = (int)(gen() % (uint64_t)nt), tries = 0;
            while (!(tk[t].kind == 'p' && buf[tk[t].at] == '{') && tries++ < 16) t = (int)(gen() % (uint64_t)nt);
            if (tk[t].kind == 'p' && buf[tk[t].at] == '{') {
                char q[600];
                const char* w = g_nwords ? g_words[gen() % (uint64_t)g_nwords] : "note";
                const char* v;
                size_t l;
                switch (gen() % 6) {
                case 0: v = "1"; break;
                case 1: v = "\"$cond\""; break;
                case 2: v = "true"; break;
                case 3: v = "[]"; break;
                case 4: v = "{}"; break;
                default: v = "\"x\""; break;
                }
                l = (size_t)snprintf(q, sizeof q, "\"%s\": %s,", w, v);
                n = splice(buf, n, tk[t].at + 1, 0, q, l);
            }
        } else if (m < 85) {
            /* copy a value over another */
            int a = (int)(gen() % (uint64_t)nt), b = (int)(gen() % (uint64_t)nt);
            if (tk[a].kind != 'p' && tk[b].kind != 'p' && tk[a].len < 200) {
                char tmp[256];
                memcpy(tmp, buf + tk[a].at, tk[a].len);
                n = splice(buf, n, tk[b].at, tk[b].len, tmp, tk[a].len);
            }
        } else if (m < 95) {
            /* graft a member's text from a seed: a hole-free way to reach
             * combinations no single seed has */
            const seedf* sf = &g_seeds[gen() % (uint64_t)g_nseeds];
            static tok t2[16384];
            int nt2 = tokenize(sf->p, sf->n, t2, 16384);
            int t = nt2 ? (int)(gen() % (uint64_t)nt2) : 0, tries = 0;
            while (nt2 && !(t2[t].kind == 's' && t + 1 < nt2 && t2[t + 1].kind == 'p' && sf->p[t2[t + 1].at] == ':') && tries++ < 16)
                t = (int)(gen() % (uint64_t)nt2);
            if (nt2 && t2[t].kind == 's' && t + 1 < nt2 && sf->p[t2[t + 1].at] == ':') {
                int end = value_end(t2, sf->p, nt2, t + 2);
                size_t a = t2[t].at, b = end < nt2 ? t2[end].at : sf->n;
                /* into a random object */
                int at = (int)(gen() % (uint64_t)nt), tries2 = 0;
                while (!(tk[at].kind == 'p' && buf[tk[at].at] == '{') && tries2++ < 16) at = (int)(gen() % (uint64_t)nt);
                if (tk[at].kind == 'p' && buf[tk[at].at] == '{' && b - a < 4000) {
                    char tmp[4100];
                    memcpy(tmp, sf->p + a, b - a);
                    tmp[b - a] = ',';
                    n = splice(buf, n, tk[at].at + 1, 0, tmp, b - a + 1);
                }
            }
        } else {
            /* a byte */
            size_t at = (size_t)(gen() % n);
            switch (gen() % 3) {
            case 0: buf[at] = (char)gen(); break;
            case 1: n = splice(buf, n, at, 1, "", 0); break;
            default: { char ch = (char)gen(); n = splice(buf, n, at, 0, &ch, 1); break; }
            }
        }
        if (n < 1) break;
    }
    return n;
}

static char* read_all(const char* path, size_t* out) {
    FILE* f = fopen(path, "rb");
    long sz;
    char* p;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    p = (char*)malloc((size_t)sz + 1);
    if (!p || fread(p, 1, (size_t)sz, f) != (size_t)sz) abort();
    fclose(f);
    *out = (size_t)sz;
    return p;
}

int main(int argc, char** argv) {
    static char buf[CAP];
    long iters, it;
    int i;
    if (argc < 4) { fprintf(stderr, "usage: self ITERS SEED seedfile...\n"); return 2; }
    iters = atol(argv[1]);
    g_rng = (uint64_t)strtoull(argv[2], NULL, 10);
    words_init();
    for (i = 3; i < argc; i++) {
        size_t n;
        char* p = read_all(argv[i], &n);
        if (p && n && g_nseeds < 4096) { g_seeds[g_nseeds].p = p; g_seeds[g_nseeds].n = n; g_nseeds++; }
    }
    if (!g_nseeds) { fprintf(stderr, "no seeds\n"); return 2; }
    for (i = 0; i < g_nseeds; i++) { run_one(g_seeds[i].p, g_seeds[i].n); run_hostile(g_seeds[i].p, g_seeds[i].n); }
    for (it = 0; it < iters; it++) {
        const seedf* sf = &g_seeds[gen() % (uint64_t)g_nseeds];
        size_t n = sf->n < CAP ? sf->n : CAP - 1;
        memcpy(buf, sf->p, n);
        n = mutate(buf, n);
        run_one(buf, n);
        run_hostile(buf, n);
    }
    printf("experiment_fuzz: %ld inputs: %lld loaded (%lld version 1, migrated; %lld version 2), %lld refused, %lld out of memory, %lld warnings\n",
           iters + g_nseeds, g_loaded, g_migrated, g_loaded2, g_refused, g_nomem, g_warned);
    return 0;
}
#elif defined(YSP_FUZZ_REPLAY)
static char* read_all(const char* path, size_t* out) {
    FILE* f = fopen(path, "rb");
    long sz;
    char* p;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    p = (char*)malloc((size_t)sz + 1);
    if (!p || fread(p, 1, (size_t)sz, f) != (size_t)sz) abort();
    fclose(f);
    *out = (size_t)sz;
    return p;
}
int main(int argc, char** argv) {
    int i;
    for (i = 1; i < argc; i++) {
        size_t n = 0;
        char* p = read_all(argv[i], &n);
        if (!p) { fprintf(stderr, "cannot read %s\n", argv[i]); return 1; }
        run_one(p, n);
        run_hostile(p, n);
        free(p);
    }
    printf("replayed %d files: %lld loaded, %lld refused\n", argc - 1, g_loaded, g_refused);
    return 0;
}
#endif
