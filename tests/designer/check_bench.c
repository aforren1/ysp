/* check_bench.c - what the designer's check costs per edit
 * (designer/core/check.c; docs/designer.md, "The check pack", "Evidence:
 * the check's cost"). Not a test: it prints times.
 *
 * Usage: bench_designer_check WORK [BUILDS] [CHECKS] [PROJECT]
 *   WORK: a folder for the projects' copies; the check packs are written
 *   under it, so its filesystem is the one measured.
 *   Projects: tests/player/{stroop,oddball,rdk,bimanual,visuomotor,afc2}
 *   (stroop with YSP_DESIGNER_TEST_FONT), and two generated ones (below).
 *   For each, BUILDS rounds (default 30) of the parts that build a pack and
 *   CHECKS rounds (default 200) of the others:
 *     first     dsg_check() with no pack: the loader, the build, pl_check()
 *     build     check.c's build(): the source written, the description,
 *               ypt_build_text(), the pack read back, texts_key()
 *     ypt       check_description() and ypt_build_text() alone; the tool
 *               writes the pack as it builds, so its write is inside
 *     wsrc      dsg_write_file() of the definition (build()'s first write)
 *     wpack     dsg_write_file() of the pack's bytes to a file beside it:
 *               the write that ypt_build_text() does, done alone
 *     read      dsg_read_file() of the pack (build()'s read-back)
 *     textskey  texts_key(), which every check with a pack runs
 *     same      dsg_check() of a text whose texts are unchanged (a space
 *               added or removed): no build
 *     changed   dsg_check() of a text with one text stimulus's text changed
 *               (a keystroke in a text literal): one build
 * Each line: median, p95 and max in microseconds.
 *
 * The generated projects ("many", "many+words") are the worst case: 200
 * text stimuli, 50 each in Latin, Hebrew, Arabic and Devanagari, each in
 * its script's Noto Sans; 10,000 table rows that the timeline runs. The
 * player refuses a node's table above YTR_MAX_CONDITIONS (1,024) rows, so
 * the rows are 10 tables of 1,000, one per node. In "many+words" one more
 * stimulus shows the tables' word column, so the pack has 10,000 more
 * texts. The fonts come from the folder of
 * YSP_DESIGNER_TEST_FONT; without them the generated projects are skipped.
 *
 * check.c is compiled into this program, not taken from ysp_designer_core,
 * to reach its static build() and texts_key(): the linker then takes no
 * check.o from the library, since nothing else there calls dsg_check().
 */
#define _POSIX_C_SOURCE 200809L
#include "check.c"

#include <dirent.h>
#include <time.h>

#ifndef YSP_SOURCE_DIR
#define YSP_SOURCE_DIR "."
#endif
#ifndef YSP_DESIGNER_TEST_FONT
#define YSP_DESIGNER_TEST_FONT ""
#endif

static double now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e6 + (double)ts.tv_nsec / 1e3;
}

static int cmp_d(const void* a, const void* b) {
    double x = *(const double*)a, y = *(const double*)b;
    return x < y ? -1 : x > y;
}

static void report(const char* what, const char* name, double* t, int n) {
    int p95 = (n * 95 + 99) / 100 - 1;
    if (n <= 0) return;
    qsort(t, (size_t)n, sizeof *t, cmp_d);
    printf("  %-9s %-12s n %4d  median %10.1f  p95 %10.1f  max %10.1f us\n", what, name, n, t[n / 2], t[p95 < 0 ? 0 : p95], t[n - 1]);
    fflush(stdout);
}

static void loadavg(const char* when) {
    char b[128] = "";
    FILE* f = fopen("/proc/loadavg", "rb");
    if (f) {
        size_t k = fread(b, 1, sizeof b - 1, f);
        b[k] = 0;
        fclose(f);
    }
    printf("load average %s: %s", when, b[0] ? b : "?\n");
}

/* --- the projects -------------------------------------------------------- */

static int copy_file(const char* from, const char* to) {
    char* d = NULL;
    size_t n = 0;
    char err[512];
    if (dsg_read_file(from, &d, &n, err, sizeof err)) { printf("  %s\n", err); return -1; }
    if (dsg_write_file(to, d, n, err, sizeof err)) { printf("  %s\n", err); free(d); return -1; }
    free(d);
    return 0;
}

/* Every file of tests/player/NAME into WORK/NAME. */
static int copy_project(const char* work, const char* name, char* out, size_t cap) {
    char src[DSG_PATH_MAX], a[DSG_PATH_MAX], b[DSG_PATH_MAX], err[512];
    DIR* dir;
    struct dirent* e;
    snprintf(src, sizeof src, "%s/tests/player/%s", YSP_SOURCE_DIR, name);
    if (dsg_join(out, cap, work, name) || dsg_make_dir(out, err, sizeof err)) return -1;
    dir = opendir(src);
    if (!dir) return -1;
    while ((e = readdir(dir)) != NULL) {
        if (dsg_join(a, sizeof a, src, e->d_name) || !dsg_is_file(a)) continue;
        if (dsg_join(b, sizeof b, out, e->d_name) || copy_file(a, b)) { closedir(dir); return -1; }
    }
    closedir(dir);
    return 0;
}

/* The folder of YSP_DESIGNER_TEST_FONT, with a slash. */
static void font_dir(char* out, size_t cap) {
    const char* s = strrchr(YSP_DESIGNER_TEST_FONT, '/');
    size_t k = s ? (size_t)(s - YSP_DESIGNER_TEST_FONT) + 1 : 0;
    if (k >= cap) k = 0;
    memcpy(out, YSP_DESIGNER_TEST_FONT, k);
    out[k] = 0;
}

static int copy_font(const char* folder, const char* file) {
    char from[DSG_PATH_MAX], to[DSG_PATH_MAX], err[512];
    font_dir(from, sizeof from);
    snprintf(from + strlen(from), sizeof from - strlen(from), "%s", file);
    if (!dsg_is_file(from)) { printf("  no font %s\n", from); return -1; }
    if (dsg_join(to, sizeof to, folder, "fonts") || dsg_make_dir(to, err, sizeof err)) return -1;
    snprintf(to + strlen(to), sizeof to - strlen(to), "/%s", file);
    return copy_file(from, to);
}

/* --- the generated worst case -------------------------------------------- */

typedef struct buf { char* p; size_t n, c; } buf;

static void put(buf* b, const char* fmt, ...) {
    va_list ap;
    int k;
    for (;;) {
        va_start(ap, fmt);
        k = vsnprintf(b->p ? b->p + b->n : NULL, b->p ? b->c - b->n : 0, fmt, ap);
        va_end(ap);
        if (k < 0) return;
        if (b->p && b->n + (size_t)k < b->c) break;
        {
            size_t nc = b->c ? b->c * 2 : 65536;
            char* np;
            while (nc < b->n + (size_t)k + 1) nc *= 2;
            np = (char*)realloc(b->p, nc);
            if (!np) return;
            b->p = np;
            b->c = nc;
        }
    }
    b->n += (size_t)k;
}

static void put_cp(buf* b, uint32_t cp) {
    if (cp < 0x80) put(b, "%c", (int)cp);
    else if (cp < 0x800) put(b, "%c%c", (int)(0xC0 | (cp >> 6)), (int)(0x80 | (cp & 0x3F)));
    else put(b, "%c%c%c", (int)(0xE0 | (cp >> 12)), (int)(0x80 | ((cp >> 6) & 0x3F)), (int)(0x80 | (cp & 0x3F)));
}

static uint32_t rng_state = 0x9E3779B9u;
static uint32_t rnd(uint32_t n) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state % n;
}

enum { LATN, HEBR, ARAB, DEVA, N_SCRIPTS };
static const char* const script_font[N_SCRIPTS] = { "NotoSans-Regular.ttf", "NotoSansHebrew-Regular.ttf", "NotoSansArabic-Regular.ttf",
                                                    "NotoSansDevanagari-Regular.ttf" };

/* A made-up word of 3 to 8 letters: only the shaping work matters. */
static void put_word(buf* b, int script) {
    int k, len = 3 + (int)rnd(6);
    for (k = 0; k < len; k++) {
        switch (script) {
        case LATN: put_cp(b, 'a' + rnd(26)); break;
        case HEBR: put_cp(b, 0x05D0 + rnd(27)); break;
        case ARAB: {
            uint32_t r = rnd(29);
            put_cp(b, r < 19 ? 0x0628 + r : 0x0641 + (r - 19));
            break;
        }
        default:
            put_cp(b, 0x0915 + rnd(37));
            if (rnd(2)) put_cp(b, 0x093E + rnd(15));
            break;
        }
    }
}

/* The definition, pack.json and the tables of a generated project. */
static int make_worst(const char* work, const char* name, bool words, char* out, size_t cap) {
    char path[DSG_PATH_MAX], err[512];
    buf d = { 0 }, desc = { 0 }, csv = { 0 };
    int i, k, s, w, rc = -1;
    rng_state = 0x9E3779B9u;
    if (dsg_join(out, cap, work, name) || dsg_make_dir(out, err, sizeof err)) return -1;
    for (s = 0; s < N_SCRIPTS; s++)
        if (copy_font(out, script_font[s])) return -1;

    put(&d, "{\n  \"format\": \"ysp-experiment\", \"version\": 1, \"name\": \"%s\",\n", name);
    put(&d, "  \"devices\": { \"resp\": { \"direction\": \"in\", \"family\": [\"keyboard\"], \"required\": true } },\n");
    put(&d, "  \"stimuli\": {\n");
    for (i = 0; i < 200; i++) {
        s = i % N_SCRIPTS;
        put(&d, "    \"s%03d\": { \"kind\": \"text\", \"font\": \"fonts/%s\", \"size\": %d, \"width\": 1000, \"y\": %d, \"text\": \"", i,
            script_font[s], 20 + (i % 5) * 4, -400 + (i % 40) * 20);
        /* the bench's keystroke edits this word: "Edit" */
        if (i == 0) put(&d, "Edit ");
        for (w = 0; w < 6 + (int)rnd(7); w++) {
            if (w) put(&d, " ");
            put_word(&d, s);
        }
        put(&d, "\" },\n");
    }
    if (words) put(&d, "    \"word\": { \"kind\": \"text\", \"font\": \"fonts/%s\", \"size\": 48, \"width\": 1000, \"text\": \"$word\" },\n", script_font[0]);
    put(&d, "    \"fix\": { \"kind\": \"shape\", \"shape\": \"cross\", \"w\": 24, \"h\": 24, \"shape_p0\": 3, \"color\": [0.6, 0.6, 0.6] }\n  },\n");
    put(&d, "  \"trials\": {\n    \"t\": {\n      \"events\": [{ \"on\": \"fix\" }, { \"wait_s\": 0.5 }, { \"on\": [");
    for (i = 0; i < 200; i++) put(&d, "%s\"s%03d\"", i ? ", " : "", i);
    if (words) put(&d, ", \"word\"");
    put(&d, "] }],\n      \"response\": { \"role\": \"resp\", \"from\": \"s000\", \"duration_s\": 2,\n");
    put(&d, "                    \"choices\": [{ \"key\": \"f\", \"name\": \"a\" }, { \"key\": \"j\", \"name\": \"b\" }] },\n");
    put(&d, "      \"after\": [{ \"off\": [\"fix\"");
    for (i = 0; i < 200; i++) put(&d, ", \"s%03d\"", i);
    if (words) put(&d, ", \"word\"");
    put(&d, "] }, { \"wait_s\": 0.2 }]\n    }\n  },\n");
    put(&d, "  \"timeline\": [");
    for (i = 0; i < 10; i++)
        put(&d, "%s\n    { \"name\": \"b%d\", \"timeline_variables\": \"words%d.pstb\", \"trial\": \"t\" }", i ? "," : "", i, i);
    put(&d, "\n  ]\n}\n");

    put(&desc, "{\n  \"format\": \"ysp-pack-source\", \"version\": 1,\n");
    put(&desc, "  \"experiment\": { \"entry\": \"experiment/main.json\", \"source\": \"experiment.json\",\n");
    put(&desc, "                  \"media_type\": \"application/vnd.ysp.experiment\" },\n  \"resources\": [\n");
    for (i = 0; i < 10; i++)
        put(&desc, "%s    { \"name\": \"words%d.pstb\", \"kind\": \"table\", \"source\": \"words%d.csv\" }", i ? ",\n" : "", i, i);
    for (s = 0; s < N_SCRIPTS; s++) {
        put(&desc, ",\n    { \"kind\": \"font\", \"source\": \"fonts/%s\", \"license\": \"OFL-1.1\" }", script_font[s]);
        put(&desc, ",\n    { \"kind\": \"curveset\", \"from\": \"fonts/%s\", \"glyphs\": \"used\" }", script_font[s]);
    }
    /* the word column mixes the scripts: the other fonts are its fallbacks */
    if (words)
        put(&desc, ",\n    { \"kind\": \"glyphruns\", \"name\": \"experiment/main.ysprun\", \"fonts\": [\"fonts/%s\", \"fonts/%s\", \"fonts/%s\"] }",
            script_font[HEBR], script_font[ARAB], script_font[DEVA]);
    put(&desc, "\n  ]\n}\n");

    if (!d.p || !desc.p) goto done;
    if (dsg_join(path, sizeof path, out, "experiment.json") || dsg_write_file(path, d.p, d.n, err, sizeof err)) goto done;
    for (k = 0; k < 10; k++) {
        char nm[32];
        csv.n = 0;
        put(&csv, "word,cond\n");
        for (i = 0; i < 1000; i++) {
            put_word(&csv, i % N_SCRIPTS);
            put(&csv, "%d,%d\n", k * 1000 + i, i % 4);
        }
        snprintf(nm, sizeof nm, "words%d.csv", k);
        if (!csv.p || dsg_join(path, sizeof path, out, nm) || dsg_write_file(path, csv.p, csv.n, err, sizeof err)) goto done;
    }
    if (dsg_join(path, sizeof path, out, "pack.json") || dsg_write_file(path, desc.p, desc.n, err, sizeof err)) goto done;
    rc = 0;
done:
    free(d.p);
    free(csv.p);
    free(desc.p);
    return rc;
}

/* --- the bench ----------------------------------------------------------- */

/* text with `needle`'s last byte replaced by c, or with a space after the
 * first '{' when needle is NULL. malloc'd. */
static char* edited(const char* text, size_t n, const char* needle, char c, size_t* out_n) {
    char* t = (char*)malloc(n + 2);
    const char* at;
    if (!t) return NULL;
    if (!needle) {
        at = memchr(text, '{', n);
        if (!at) { free(t); return NULL; }
        memcpy(t, text, (size_t)(at - text) + 1);
        t[at - text + 1] = ' ';
        memcpy(t + (at - text) + 2, at + 1, n - (size_t)(at - text) - 1);
        *out_n = n + 1;
    } else {
        at = strstr(text, needle);
        if (!at) { free(t); return NULL; }
        memcpy(t, text, n);
        t[(at - text) + strlen(needle) - 1] = c;
        *out_n = n;
    }
    t[*out_n] = 0;
    return t;
}

static void bench_project(const char* name, const char* folder, const char* needle, int builds, int checks) {
    dsg_project p;
    dsg_checker* c = dsg_checker_new();
    dsg_report r;
    double* t = (double*)malloc((size_t)(builds > checks ? builds : checks) * sizeof *t);
    char err[1024], pack[DSG_PATH_MAX], copy[DSG_PATH_MAX];
    uint8_t tkey[32], key[32];
    int i, rebuilt = 0, n_texts = -1;
    memset(&p, 0, sizeof p);
    if (!c || !t) goto out;
    if (dsg_project_open(&p, folder, err, sizeof err)) { printf("%s: cannot open: %s\n", name, err); goto out; }
    if (dsg_join(pack, sizeof pack, p.folder, DSG_CHECK_PACK) || dsg_join(copy, sizeof copy, p.folder, DSG_DIR "/check/bench_copy.ysppak"))
        goto out;
    tables_key(NULL, tkey);

    /* one check to see that the project is good, and its pack's size */
    if (dsg_check(c, &p, p.text, p.text_len, NULL, &r) || !r.have_pack) {
        printf("%s: no check pack (status %d)\n", name, r.status);
        for (i = 0; i < r.n_issues; i++) printf("  %s\n", r.issues[i].message);
        goto out;
    }
    {
        ypt_texts tx;
        tables_ctx tc;
        ypak_desc pd;
        memset(&tx, 0, sizeof tx);
        memset(&tc, 0, sizeof tc);
        memset(&pd, 0, sizeof pd);
        pd.data = c->pack;
        pd.size = c->pack_size;
        tc.open = ypak_open(&tc.pk, &pd);
        if (ypt_derive_texts(p.text, p.text_len, table_bytes, &tc, &tx, err, sizeof err) == 0) {
            n_texts = tx.n_texts;
            ypt_texts_free(&tx);
        }
        if (tc.open) ypak_close(&tc.pk);
    }
    printf("%s: definition %zu B, pack %zu B, %d texts, check status %d, %d issues\n", name, p.text_len, c->pack_size, n_texts, r.status,
           r.n_issues);
    for (i = 0; i < r.n_issues; i++)
        if (r.issues[i].severity == DSG_SEV_ERROR) printf("  error: %s\n", r.issues[i].message);
    fflush(stdout);

    for (i = 0; i < builds; i++) {
        double t0;
        dsg_checker_forget(c);
        t0 = now_us();
        dsg_check(c, &p, p.text, p.text_len, NULL, &r);
        t[i] = now_us() - t0;
        rebuilt += r.built;
    }
    report("first", name, t, builds);

    for (i = 0; i < builds; i++) {
        double t0 = now_us();
        int rc = build(c, &p, p.text, p.text_len, NULL, tkey, err, sizeof err);
        t[i] = now_us() - t0;
        if (rc) { printf("  build failed: %s\n", err); break; }
    }
    report("build", name, t, i);

    for (i = 0; i < builds; i++) {
        size_t dl = 0;
        char* desc;
        ypt_options o;
        double t0;
        int rc;
        memset(&o, 0, sizeof o);
        t0 = now_us();
        desc = check_description(&p, NULL, &dl, err, sizeof err);
        rc = desc ? ypt_build_text(desc, dl, p.folder, pack, &o, err, sizeof err) : -1;
        t[i] = now_us() - t0;
        free(desc);
        if (rc) { printf("  ypt_build_text failed: %s\n", err); break; }
    }
    report("ypt", name, t, i);

    for (i = 0; i < checks; i++) {
        char src[DSG_PATH_MAX];
        double t0;
        dsg_join(src, sizeof src, p.folder, DSG_CHECK_SOURCE);
        t0 = now_us();
        dsg_write_file(src, p.text, p.text_len, err, sizeof err);
        t[i] = now_us() - t0;
    }
    report("wsrc", name, t, checks);

    for (i = 0; i < checks; i++) {
        double t0 = now_us();
        dsg_write_file(copy, c->pack, c->pack_size, err, sizeof err);
        t[i] = now_us() - t0;
    }
    report("wpack", name, t, checks);
    remove(copy);

    for (i = 0; i < checks; i++) {
        char* b = NULL;
        size_t bn = 0;
        double t0 = now_us();
        dsg_read_file(pack, &b, &bn, err, sizeof err);
        t[i] = now_us() - t0;
        free(b);
    }
    report("read", name, t, checks);

    for (i = 0; i < checks; i++) {
        double t0 = now_us();
        texts_key(c, p.text, p.text_len, key, err, sizeof err);
        t[i] = now_us() - t0;
    }
    report("textskey", name, t, checks);

    {
        size_t sn = 0;
        char* spaced = edited(p.text, p.text_len, NULL, 0, &sn);
        int built = 0;
        for (i = 0; spaced && i < checks; i++) {
            double t0 = now_us();
            if (i & 1) dsg_check(c, &p, spaced, sn, NULL, &r);
            else dsg_check(c, &p, p.text, p.text_len, NULL, &r);
            t[i] = now_us() - t0;
            built += r.built;
        }
        if (spaced) report("same", name, t, checks);
        if (built) printf("  same: %d checks built a pack\n", built);
        free(spaced);
    }

    if (needle && strstr(p.text, needle)) {
        int built = 0;
        for (i = 0; i < builds; i++) {
            size_t en = 0;
            char* e = edited(p.text, p.text_len, needle, (char)('a' + i % 26), &en);
            double t0;
            if (!e) break;
            t0 = now_us();
            dsg_check(c, &p, e, en, NULL, &r);
            t[i] = now_us() - t0;
            built += r.built;
            free(e);
        }
        report("changed", name, t, i);
        if (built != i) printf("  changed: %d of %d checks built a pack\n", built, i);
    } else {
        printf("  changed   %-12s n/a: no text stimulus\n", name);
    }
    if (rebuilt != builds) printf("  first: %d of %d checks built a pack\n", rebuilt, builds);

out:
    dsg_project_close(&p);
    dsg_checker_free(c);
    free(t);
}

int main(int argc, char** argv) {
    static const char* const players[] = { "stroop", "oddball", "rdk", "bimanual", "visuomotor", "afc2" };
    const char* work;
    const char* only;
    char folder[DSG_PATH_MAX], err[512];
    int builds, checks, k;
    if (argc < 2) {
        printf("usage: bench_designer_check WORK [BUILDS] [CHECKS] [PROJECT]\n");
        return 2;
    }
    work = argv[1];
    builds = argc > 2 ? atoi(argv[2]) : 30;
    checks = argc > 3 ? atoi(argv[3]) : 200;
    only = argc > 4 ? argv[4] : NULL;
    if (builds < 1) builds = 1;
    if (checks < 1) checks = 1;
    if (dsg_make_dir(work, err, sizeof err)) { printf("%s\n", err); return 1; }
    printf("bench_designer_check: %d builds, %d checks, work %s\n", builds, checks, work);
    loadavg("before");
    for (k = 0; k < 6; k++) {
        const char* nm = players[k];
        if (only && strcmp(only, nm) != 0) continue;
        if (copy_project(work, nm, folder, sizeof folder)) { printf("%s: cannot copy\n", nm); continue; }
        if (k == 0 && copy_font(folder, "NotoSans-Regular.ttf")) { printf("stroop: skipped, no font\n"); continue; }
        bench_project(nm, folder, k == 0 ? "R red" : NULL, builds, checks);
    }
    if (!only || strcmp(only, "many") == 0) {
        if (make_worst(work, "many", false, folder, sizeof folder) == 0) bench_project("many", folder, "Edit", builds, checks);
        else printf("many: skipped\n");
    }
    if (!only || strcmp(only, "many+words") == 0) {
        if (make_worst(work, "many_words", true, folder, sizeof folder) == 0) bench_project("many+words", folder, "Edit", builds, checks);
        else printf("many+words: skipped\n");
    }
    loadavg("after");
    return 0;
}
