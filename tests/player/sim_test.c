/* sim_test.c - the stepped session (player/sim.c, docs/player.md "Reference:
 * the stepped session"), against ysp_player --sim run in this process on
 * the virtual clock.
 *
 * Usage: test_player_sim NAME=PACK... [--keep DIR]
 *   NAME is one of rdk, stroop, afc2, oddball, sound, visuomotor, bimanual
 *   (tests/player/sim_test.py builds them; rdk and oddball are required).
 *   --keep DIR: the data sets go to DIR and stay. With no NAME=PACK and
 *   YPAK set (the mutation runner, tests/mutate/player_sim.toml), it builds
 *   them under a temporary folder.
 *
 * The cases, docs/designer.md "Tests", P4:
 *   (1) each pack at 800 x 600 with the scripted participant gives
 *       --sim's trials.csv, events.csv, frames.csv and streams byte for
 *       byte; visuomotor and bimanual with their samples through
 *       pl_sim_sample(); a sound that the response runs from within the
 *       tolerance measured here
 *   (2) no allocation in pl_sim_frame() (PL_SIM_COUNT: GNU ld's --wrap)
 *   (3) load and fast forward on one thread, attach and frames on another;
 *       no clock change, no elevated thread
 *   (4) two sessions stepped in turn while a third thread runs pl_check()
 *   (5) a key, a choice, a sample that ends a reach
 *   (6) pl_sim_where_now()'s states follow --sim's state rows (bimanual)
 *   (7) a refusal at run time names its object
 *   (8) live(): the caller's trials and the scripted ones
 *   (9) a stopped session's session.json says so
 *   (10) PL_SIM_GL (Linux): on a GL ES context (EGL, llvmpipe, no window),
 *       a session loaded on one thread with gl_caps and share and attached
 *       on the context's thread draws the frames of a direct gfx, read
 *       through pl_sim_texture() (stroop and oddball)
 *   (13) 0.19.0, a text's align (ysp/experiment.h 0.7.0): stroop's word,
 *       with no align, is centered on the display (its ink, on a GL
 *       context); stroop_start's, "align": "start", starts at the box's
 *       left edge; stroop_old (tests/player/align_packs.py: the pack that
 *       ypak 0.12.0 built from stroop) loads with ysp/experiment.h 0.6.0's
 *       defaults and draws and records what stroop_start does
 *   (12) 0.18.0, a desc's rig profile: the needs and refusals of pl_check()
 *       with the same profile and calibration; the profile's signal
 *       adapters, against --sim --rig; what a relative role's sample is
 *   and the display, rig and calibration a desc gives, the check's needs,
 *   and on_code against events.csv's out rows
 */
#if !defined(_WIN32) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE 1   /* mkdtemp(), setenv(): before any system header */
#endif
#if defined(PL_SIM_MUT) && defined(PL_SIM_GL)
/* one translation unit: GNU ld's --wrap cannot reach lower.c's call of
 * pl_gfx_open_null(), so the names do what the wrap does in the build */
#define pl_gfx_open_null __real_pl_gfx_open_null
#include "impl.c"
#undef pl_gfx_open_null
bool __wrap_pl_gfx_open_null(ygfx_gfx* g, ygfx_desc* d);
#define pl_gfx_open_null __wrap_pl_gfx_open_null
#include "eval.c"
#include "host.c"
#include "lower.c"
#include "run.c"
#include "v2.c"
#include "sim.c"
#undef pl_gfx_open_null
#elif defined(PL_SIM_MUT)
#include "impl.c"
#include "eval.c"
#include "host.c"
#include "lower.c"
#include "run.c"
#include "v2.c"
#include "sim.c"
#else
#include "player.h"
#endif

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <dirent.h>
#include <pthread.h>
#include <sched.h>
#include <sys/stat.h>
#include <unistd.h>
#else
#include <direct.h>
#include <io.h>
#include <process.h>
#include <windows.h>
#define getpid _getpid
#endif

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#if defined(PL_SIM_COUNT)
/* (2): heap calls on this thread while pl_sim_frame() runs */
static _Thread_local int in_frame;
static long long frame_allocs;
void* __real_malloc(size_t n);
void* __real_calloc(size_t a, size_t b);
void* __real_realloc(void* q, size_t n);
void* __wrap_malloc(size_t n) { if (in_frame) frame_allocs++; return __real_malloc(n); }
void* __wrap_calloc(size_t a, size_t b) { if (in_frame) frame_allocs++; return __real_calloc(a, b); }
void* __wrap_realloc(void* q, size_t n) { if (in_frame) frame_allocs++; return __real_realloc(q, n); }
static long long n_frames_stepped;
#define FRAME_IN() (in_frame = 1)
#define FRAME_OUT() (in_frame = 0, n_frames_stepped++)
#else
#define FRAME_IN() ((void)0)
#define FRAME_OUT() ((void)0)
#endif

static int sim_frame(pl_player* p, bool draw) {
    int rc;
    FRAME_IN();
    rc = pl_sim_frame(p, draw);
    FRAME_OUT();
    return rc;
}

typedef struct pk { const char* name; char path[1100]; void* data; size_t size; } pk;
static pk packs[12];
static int n_packs;
static char work[1100];
static ycol_cal nominal;

static pk* pack_of(const char* name) {
    int i;
    for (i = 0; i < n_packs; i++) if (strcmp(packs[i].name, name) == 0) return &packs[i];
    return NULL;
}

/* --- files ------------------------------------------------------------------ */

static int read_file(const char* path, void** data, size_t* size) {
    FILE* fp = fopen(path, "rb");
    long n;
    if (!fp) return -1;
    fseek(fp, 0, SEEK_END);
    n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    *data = malloc((size_t)n + 1);   /* malloc's alignment: the pack wants 8 */
    if (!*data || fread(*data, 1, (size_t)n, fp) != (size_t)n) { fclose(fp); return -1; }
    ((char*)*data)[n] = 0;
    fclose(fp);
    *size = (size_t)n;
    return 0;
}

static int make_dir(const char* path) {
#if !defined(_WIN32)
    return mkdir(path, 0700);
#else
    return _mkdir(path);
#endif
}

/* The names in a folder, as "a\nb\n" (unsorted), or "" when it is missing. */
static void list_dir(const char* path, char* out, size_t cap) {
    size_t used = 0;
    out[0] = 0;
#if !defined(_WIN32)
    {
        DIR* d = opendir(path);
        struct dirent* e;
        if (!d) return;
        while ((e = readdir(d)) != NULL)
            if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..") && used < cap) used += (size_t)snprintf(out + used, cap - used, "%s\n", e->d_name);
        closedir(d);
    }
#else
    {
        char pat[1200];
        struct _finddata_t f;
        intptr_t h;
        snprintf(pat, sizeof pat, "%s\\*", path);
        h = _findfirst(pat, &f);
        if (h == -1) return;
        do {
            if (strcmp(f.name, ".") && strcmp(f.name, "..") && used < cap) used += (size_t)snprintf(out + used, cap - used, "%s\n", f.name);
        } while (_findnext(h, &f) == 0);
        _findclose(h);
    }
#endif
}

static void rm_tree(const char* path) {
    char names[8192], *n, *e, sub[1400];
    list_dir(path, names, sizeof names);
    for (n = names; *n; n = e + 1) {
        e = strchr(n, '\n');
        *e = 0;
        snprintf(sub, sizeof sub, "%.1100s/%.250s", path, n);
        if (remove(sub) != 0) rm_tree(sub);
    }
#if !defined(_WIN32)
    rmdir(path);
#else
    _rmdir(path);
#endif
}

/* Two files' bytes equal; the first differing line goes to stdout. */
static int same_file(const char* a, const char* b, const char* what) {
    void *x = NULL, *y = NULL;
    size_t nx = 0, ny = 0;
    int ok;
    if (read_file(a, &x, &nx) != 0) { printf("  %s: no %s\n", what, a); free(x); return 0; }
    if (read_file(b, &y, &ny) != 0) { printf("  %s: no %s\n", what, b); free(x); free(y); return 0; }
    ok = nx == ny && memcmp(x, y, nx) == 0;
    if (!ok) {
        const char *p = (const char*)x, *q = (const char*)y, *l0 = p;
        size_t i = 0, line = 1;
        while (i < nx && i < ny && p[i] == q[i]) { if (p[i] == '\n') { line++; l0 = p + i + 1; } i++; }
        printf("  %s differs at line %zu (%zu and %zu bytes):\n    --sim:   %.200s\n    stepped: %.200s\n", what, line, nx, ny, l0,
               q + (l0 - p));
    }
    free(x);
    free(y);
    return ok;
}

/* Every file of the data set but session.json, and every stream. */
static int same_data(const char* ref, const char* got, const char* name) {
    static const char* const files[] = { "trials.csv", "events.csv", "frames.csv", "clock.csv" };
    char a[1400], b[1400], what[200], names[8192], names2[8192], *n, *e;
    size_t i;
    int ok = 1;
    for (i = 0; i < sizeof files / sizeof files[0]; i++) {
        snprintf(a, sizeof a, "%s/%s", ref, files[i]);
        snprintf(b, sizeof b, "%s/%s", got, files[i]);
        snprintf(what, sizeof what, "%s %s", name, files[i]);
        if (!same_file(a, b, what)) ok = 0;
    }
    snprintf(a, sizeof a, "%s/streams", ref);
    snprintf(b, sizeof b, "%s/streams", got);
    list_dir(a, names, sizeof names);
    list_dir(b, names2, sizeof names2);
    if (strlen(names) != strlen(names2)) { printf("  %s: streams '%s' and '%s'\n", name, names, names2); ok = 0; }
    for (n = names; *n; n = e + 1) {
        e = strchr(n, '\n');
        *e = 0;
        /* a stream's name is short; the precision keeps gcc's
         * -Wformat-truncation (Debug, -Werror) quiet about names[]'s size */
        snprintf(a, sizeof a, "%.1100s/streams/%.200s", ref, n);
        snprintf(b, sizeof b, "%.1100s/streams/%.200s", got, n);
        snprintf(what, sizeof what, "%.60s streams/%.100s", name, n);
        if (!same_file(a, b, what)) ok = 0;
    }
    return ok;
}

/* A file's text holds s. */
static int file_has(const char* path, const char* s) {
    void* x = NULL;
    size_t n = 0;
    int ok;
    if (read_file(path, &x, &n) != 0) return 0;
    ok = strstr((const char*)x, s) != NULL;
    free(x);
    return ok;
}

/* --- definitions ---------------------------------------------------------------- */

/* The definition entry of a pack, as text. */
static char* def_of(const pk* k) {
    ypak_pack pp;
    ypak_desc d;
    ypak_entry e;
    const void* data;
    char* out = NULL;
    memset(&d, 0, sizeof d);
    d.data = k->data;
    d.size = k->size;
    if (!ypak_open(&pp, &d)) return NULL;
    if (ypak_first_of(&pp, YPAK_KIND_EXPERIMENT, &e) == 0 && (data = ypak_data(&pp, &e)) != NULL) {
        out = (char*)malloc((size_t)e.size + 1);
        memcpy(out, data, (size_t)e.size);
        out[e.size] = 0;
    }
    ypak_close(&pp);
    return out;
}

/* text with every `from` replaced by `to` (text is freed); NULL when
 * `from` is missing */
static char* edit_all(char* text, const char* from, const char* to) {
    size_t nf = strlen(from), nt = strlen(to), n = 0;
    const char* c;
    char* out, *w;
    if (!text) return NULL;
    for (c = strstr(text, from); c; c = strstr(c + nf, from)) n++;
    if (!n) { printf("  edit: no '%s'\n", from); free(text); return NULL; }
    out = (char*)malloc(strlen(text) + n * (nt + 1) + 1);
    w = out;
    for (c = text; *c;) {
        if (strncmp(c, from, nf) == 0) { memcpy(w, to, nt); w += nt; c += nf; }
        else *w++ = *c++;
    }
    *w = 0;
    free(text);
    return out;
}

/* bimanual's reach exit as the pack holds it (ypak writes the definition
 * canonical), and the same with a timed exit after it */
#define ARRIVED "\"on\": \"arrived\",\n              \"to\": \"end\"\n            }"
#define ARRIVED_OR(s) ARRIVED ", { \"after_s\": " #s ", \"to\": \"end\", \"outcome\": \"timeout\" }"

/* --- samples ------------------------------------------------------------------ */

/* visuomotor's hand, a relative role at gain 1 (a px a count): from the
 * start, out 300 px in one of 8 directions in 150 ms, back after 0.8 s,
 * home 0.7 s; reports every 2 ms */
static int write_samples_vm(const char* path, int n_s) {
    static const int dir[8][2] = { { 4, 0 }, { 3, 3 }, { 0, 4 }, { -3, 3 }, { -4, 0 }, { -3, -3 }, { 0, -4 }, { 3, -3 } };
    FILE* fp = fopen(path, "wb");
    long k, n = (long)n_s * 500;
    if (!fp) return -1;
    fputs("t_s,role,x,y\n", fp);
    for (k = 0; k < n; k++) {
        long c = k / 900, j = k % 900;   /* a 1.8 s cycle */
        int dx = 0, dy = 0;
        if (j >= 350 && j < 425) { dx = dir[c % 8][0]; dy = dir[c % 8][1]; }
        else if (j >= 825) { dx = -dir[c % 8][0]; dy = -dir[c % 8][1]; }
        fprintf(fp, "%.6f,hand,%d,%d\n", 0.002 * (double)k, dx, dy);
    }
    fclose(fp);
    return 0;
}

/* bimanual's hands, two relative roles at gain 1 (a cm a count): small
 * circles about the start, so `ready` and `at_start` hold; 250 reports a
 * second each */
static int write_samples_bi(const char* path, int n_s) {
    FILE* fp = fopen(path, "wb");
    long k, n = (long)n_s * 250;
    if (!fp) return -1;
    fputs("t_s,role,x,y\n", fp);
    for (k = 0; k < n; k++) {
        double a = 2.0 * 3.141592653589793 * 0.4 * 0.004 * (double)k, da = 2.0 * 3.141592653589793 * 0.4 * 0.004;
        double dx = 0.5 * (cos(a + da) - cos(a)), dy = 0.5 * (sin(a + da) - sin(a));
        fprintf(fp, "%.6f,left,%g,%g\n", 0.004 * (double)k, dx, dy);
        fprintf(fp, "%.6f,right,%g,%g\n", 0.004 * (double)k + 0.002, -dx, dy);
    }
    fclose(fp);
    return 0;
}

/* A --samples file read as v2.c reads it, for pl_sim_sample() */
typedef struct smp { int64_t t; char role[32]; float x, y; } smp;
static smp* read_samples(const char* path, long* n_out) {
    FILE* fp = fopen(path, "r");
    char line[256];
    smp* s;
    long n = 0, cap = 1 << 16;
    if (!fp) return NULL;
    s = (smp*)malloc(sizeof(smp) * (size_t)cap);
    while (s && fgets(line, sizeof line, fp)) {
        double t, x, y;
        char role[32];
        if (strncmp(line, "t_s", 3) == 0) continue;
        if (sscanf(line, "%lf,%31[^,],%lf,%lf", &t, role, &x, &y) != 4) continue;
        if (n == cap) {
            smp* q = (smp*)realloc(s, sizeof(smp) * (size_t)(cap *= 2));
            if (!q) { free(s); s = NULL; break; }
            s = q;
        }
        s[n].t = (int64_t)llround(t * 1e9);
        snprintf(s[n].role, sizeof s[n].role, "%s", role);
        s[n].x = (float)x;
        s[n].y = (float)y;
        n++;
    }
    fclose(fp);
    *n_out = n;
    return s;
}

/* --- the two runs ------------------------------------------------------------ */

typedef struct run_opts {
    const char* def;          /* NULL: the pack's */
    uint64_t seed;
    bool rig;                 /* --rig (the view and nominal calibration) for
                               * --sim; the built-in rig's stand-ins for the
                               * stepped session */
    const char* samples;      /* --samples, with --sim-gain ROLE=1 */
    const char* gain_roles[2];
    const char* participant;
    const char* rig_file;     /* --sim --rig FILE (0.18.0, case_rig)        */
    const yrig_profile* step_rig;   /* pl_sim_desc.rig, the same profile  */
} run_opts;

static char rig_path[1200];

/* ysp_player --sim, in this process on the virtual clock, ended after */
static int ref_run(const pk* k, const run_opts* o, const char* out) {
    pl_player* p = (pl_player*)calloc(1, sizeof *p);
    pl_args a;
    int rc, i;
    memset(&a, 0, sizeof a);
    a.pack_path = k->path;
    a.out_dir = out;
    a.sim = true;
    a.have_seed = true;
    a.seed = o->seed;
    a.n_fields = 1;
    a.fields[0].name = "participant";
    a.fields[0].value = "1";
    a.participant = o->participant;
    a.def_text = o->def;
    a.def_len = o->def ? strlen(o->def) : 0;
    if (o->rig) a.rig_path = rig_path;
    if (o->rig_file) a.rig_path = o->rig_file;
    a.samples_path = o->samples;
    for (i = 0; i < 2; i++)
        if (o->gain_roles[i]) {
            a.gains[a.n_gains].name = o->gain_roles[i];
            a.gains[a.n_gains].value = "1";
            a.n_gains++;
        }
    if (yrt_virtual_begin(0) != 0) { printf("  no virtual clock\n"); free(p); return -1; }
    rc = pl_load(p, &a) < 0 || pl_run(p) < 0 ? -1 : 0;
    if (rc < 0) printf("  --sim %s: %s\n", k->name, p->error);
    pl_close(p);
    yrt_virtual_end();
    free(p);
    return rc;
}

static void sim_desc(pl_sim_desc* d, const pk* k, const run_opts* o, const char* out) {
    memset(d, 0, sizeof *d);
    d->pack.data = k->data;
    d->pack.size = k->size;
    d->def_text = o->def;
    d->def_len = o->def ? strlen(o->def) : 0;
    d->seed = o->seed;
    d->cal = o->rig || o->step_rig ? &nominal : NULL;   /* the rig's calibration, which --rig's file holds */
    d->rig = o->step_rig;
    d->n_fields = 1;
    d->fields[0].name = "participant";
    d->fields[0].value = "1";
    d->out_dir = out;
    d->participant = o->participant;
    d->width = 800;
    d->height = 600;
}

/* The samples due by the next frame's onset, from k on: t0 is frame 0's onset */
static int feed_samples(pl_player* p, const smp* s, long n, long* k) {
    int64_t next = pl_sim_next_onset(p), t0 = p->sim.t0;
    if (p->sim.stage != 1) return 0;   /* the trials are over: --sim feeds no more either */
    while (*k < n && t0 + s[*k].t <= next) {
        const yexp_sigrole* r = yexp_sigrole_get(p->def, s[*k].role);
        if (!r || pl_sim_sample(p, t0 + s[*k].t, (int)(r - p->def->sigroles), s[*k].x, s[*k].y) != 0) return -1;
        (*k)++;
    }
    return 0;
}

/* The stepped session to its end, drawing every third frame on the null
 * gfx; *states (optional) gets where_now's state after each frame. */
typedef struct state_log { int64_t frame; int trial; char state[32]; } state_log;
static int step_run(const pk* k, const run_opts* o, const char* out, state_log* log, int log_cap, int* n_log) {
    pl_player* p = (pl_player*)calloc(1, sizeof *p);
    pl_sim_desc d;
    pl_check_result r;
    smp* s = NULL;
    long n_s = 0, ks = 0;
    int rc = 0;
    if (o->samples && !(s = read_samples(o->samples, &n_s))) { printf("  cannot read %s\n", o->samples); free(p); return -1; }
    sim_desc(&d, k, o, out);
    if (pl_sim_load(p, &d, &r) != PL_CHECK_OK) { printf("  stepped %s: load: %s\n", k->name, r.message); rc = -1; }
    if (rc == 0 && pl_sim_attach(p, 0) != 1) { printf("  stepped %s: attach: %s\n", k->name, p->error); rc = -1; }
    while (rc == 0) {
        int f;
        if (s && feed_samples(p, s, n_s, &ks) < 0) { printf("  stepped %s: a sample refused at %ld\n", k->name, ks); rc = -1; break; }
        f = sim_frame(p, p->sim.frame % 3 == 0);
        if (f < 0) { printf("  stepped %s: frame %lld: %s\n", k->name, (long long)p->sim.frame, p->error); rc = -1; break; }
        if (log && *n_log < log_cap) {
            pl_sim_where w;
            pl_sim_where_now(p, &w);
            log[*n_log].frame = p->sim.frame - 1;
            log[*n_log].trial = w.trial;
            snprintf(log[*n_log].state, sizeof log[*n_log].state, "%s", w.state ? w.state : "");
            (*n_log)++;
        }
        if (f == 1) break;
    }
    pl_close(p);
    free(s);
    free(p);
    return rc;
}

/* --- (1) and (2): each pack against --sim ---------------------------------------- */

static void path_in(char* buf, size_t cap, const char* name) { snprintf(buf, cap, "%.900s/%.150s", work, name); }

static int compare_pack(const char* label, const pk* k, const run_opts* o) {
    char a[1200], b[1200], l1[200], l2[200];
    int ok;
    snprintf(l1, sizeof l1, "%.150s_sim", label);
    snprintf(l2, sizeof l2, "%.150s_step", label);
    path_in(a, sizeof a, l1);
    path_in(b, sizeof b, l2);
    if (ref_run(k, o, a) < 0) { CHECK(0, "%s: --sim failed", label); return 0; }
    if (step_run(k, o, b, NULL, 0, NULL) < 0) { CHECK(0, "%s: the stepped session failed", label); return 0; }
    ok = same_data(a, b, label);
    CHECK(ok, "%s: the stepped session's data differ from --sim's", label);
    if (ok) printf("  %s: the same trials.csv, events.csv, frames.csv, clock.csv and streams as --sim\n", label);
    return ok;
}

/* The cells of two CSV texts, line by line: equal, or numbers within tol,
 * or (shift) times on the grid (1000 s and later: YRT_VIRTUAL_T0_NS) that
 * differ by shift within tol; the largest difference past the shift in
 * *max_d, the cells not equal in *n_diff */
static int csv_close(const char* x, const char* y, double shift, double tol, double* max_d, int* n_diff) {
    *max_d = 0;
    *n_diff = 0;
    while (*x && *y) {
        size_t a = strcspn(x, ",\n"), b = strcspn(y, ",\n");
        if (a != b || strncmp(x, y, a) != 0) {
            char* e1, *e2;
            double u = strtod(x, &e1), v = strtod(y, &e2), dd;
            if (e1 != x + a || e2 != y + b || e1 == x) {
                printf("    cells '%.*s' and '%.*s'\n", (int)a, x, (int)b, y);
                return 0;
            }
            dd = fabs(u - v);
            if (u >= 1000.0 && v >= 1000.0 && fabs(u - v - shift) < dd) dd = fabs(u - v - shift);
            if (dd > tol) { printf("    cells '%.*s' and '%.*s'\n", (int)a, x, (int)b, y); return 0; }
            if (dd > *max_d) *max_d = dd;
            if (fabs(u - v) > 0 && dd != fabs(u - v - shift)) (*n_diff)++;
        }
        if (x[a] != y[b]) return 0;
        x += a + (x[a] ? 1 : 0);
        y += b + (y[b] ? 1 : 0);
    }
    return *x == *y;
}

/* frames.csv's first onset (target_s of frame 0), or 0 */
static double first_onset(const char* dir) {
    char path[1300];
    void* x = NULL;
    size_t n;
    double t = 0;
    const char* l;
    snprintf(path, sizeof path, "%s/frames.csv", dir);
    if (read_file(path, &x, &n) != 0) return 0;
    l = strchr((const char*)x, '\n');
    if (l && (l = strchr(l + 1, ',')) && (l = strchr(l + 1, ','))) t = strtod(l + 1, NULL);
    free(x);
    return t;
}

/* A data set with a sound: --sim's null audio device opens on the virtual
 * clock and moves it, so --sim's frame 0 comes later than the grid's by
 * that time; every other cell is equal, or within tol (a sound's onset). */
static int same_data_shifted(const char* ref, const char* got, const char* name, double tol, double* shift_out, double* max_out, int* n_out) {
    static const char* const files[] = { "trials.csv", "events.csv", "frames.csv" };
    double shift = first_onset(ref) - first_onset(got), max_d = 0;
    size_t i;
    int ok = 1, n_all = 0;
    for (i = 0; i < sizeof files / sizeof files[0]; i++) {
        char a[1300], b[1300];
        void *x = NULL, *y = NULL;
        size_t nx = 0, ny = 0;
        double m = 0;
        int nd = 0;
        snprintf(a, sizeof a, "%s/%s", ref, files[i]);
        snprintf(b, sizeof b, "%s/%s", got, files[i]);
        if (read_file(a, &x, &nx) != 0 || read_file(b, &y, &ny) != 0 || !csv_close((const char*)x, (const char*)y, shift, tol, &m, &nd)) {
            printf("  %s %s: differs past the shift of %.9f s and %.1f us\n", name, files[i], shift, tol * 1e6);
            ok = 0;
        }
        if (m > max_d) max_d = m;
        n_all += nd;
        free(x);
        free(y);
    }
    *shift_out = shift;
    *max_out = max_d;
    *n_out = n_all;
    return ok;
}

static void case_equal(void) {
    static const struct { const char* name; bool rig; } P[] = {
        { "rdk", false }, { "stroop", false }, { "afc2", true }, { "oddball", true },
    };
    size_t i;
    for (i = 0; i < sizeof P / sizeof P[0]; i++) {
        const pk* k = pack_of(P[i].name);
        run_opts o;
        if (!k) { printf("  %s: no pack, skipped\n", P[i].name); continue; }
        memset(&o, 0, sizeof o);
        o.seed = 7;
        o.rig = P[i].rig;
        compare_pack(P[i].name, k, &o);
    }
    /* version 2 with samples, shortened by edits that keep every part of
     * the definition: fewer repetitions; bimanual's tracking 3 s, a reach
     * that ends after 2 s */
    {
        const pk* k = pack_of("visuomotor");
        if (k) {
            run_opts o;
            char sp[1200];
            memset(&o, 0, sizeof o);
            o.seed = 3;
            o.def = edit_all(edit_all(def_of(k), "\"reps 5\"", "\"reps 1\""), "\"reps 10\"", "\"reps 1\"");
            path_in(sp, sizeof sp, "visuomotor_samples.csv");
            o.samples = sp;
            o.gain_roles[0] = "hand";
            if (o.def && write_samples_vm(sp, 120) == 0) compare_pack("visuomotor", k, &o);
            else CHECK(0, "visuomotor: no definition or samples");
            free((void*)o.def);
        } else printf("  visuomotor: no pack, skipped\n");
    }
    {
        const pk* k = pack_of("bimanual");
        if (k) {
            run_opts o;
            char sp[1200];
            memset(&o, 0, sizeof o);
            o.seed = 3;
            o.rig = true;
            o.def = edit_all(edit_all(edit_all(def_of(k), "\"reps 30\"", "\"reps 2\""), "\"wait_s\": 46", "\"wait_s\": 3"),
                             ARRIVED, ARRIVED_OR(2));
            path_in(sp, sizeof sp, "bimanual_samples.csv");
            o.samples = sp;
            o.gain_roles[0] = "left";
            o.gain_roles[1] = "right";
            if (o.def && write_samples_bi(sp, 120) == 0) compare_pack("bimanual", k, &o);
            else CHECK(0, "bimanual: no definition or samples");
            free((void*)o.def);
        } else printf("  bimanual: no pack, skipped\n");
    }
    /* sound: the stepped session's sound keeps its plan and runs no audio
     * device; --sim's null device moves the virtual clock at its open (so
     * --sim's grid starts later by a fixed time) and starts each sound on
     * one of its samples, so a time counted from a sound can differ by
     * less than a sample period (48 kHz). Once with the response from the
     * dots, once from the tone. */
    {
        const pk* k = pack_of("sound");
        int v;
        for (v = 0; k && v < 2; v++) {
            run_opts o;
            char a[1200], b[1200];
            double shift = 0, max_d = 0;
            int n_diff = 0;
            memset(&o, 0, sizeof o);
            o.seed = 7;
            if (v == 1) o.def = edit_all(def_of(k), "\"from\": \"dots\"", "\"from\": \"tone\"");
            path_in(a, sizeof a, v ? "sound_from_sim" : "sound_sim");
            path_in(b, sizeof b, v ? "sound_from_step" : "sound_step");
            if ((v == 0 || o.def) && ref_run(k, &o, a) == 0 && step_run(k, &o, b, NULL, 0, NULL) == 0) {
                CHECK(same_data_shifted(a, b, v ? "sound from" : "sound", 1.0 / 48000.0, &shift, &max_d, &n_diff),
                      "%s: the data differ from --sim's by more than the grid's shift and a sample period", v ? "sound from the tone" : "sound");
                printf("  %s: --sim's data, its grid %.9f s later; %d cells more than that apart, by at most %.1f us (a sample period: 20.8 us)\n",
                       v ? "sound, the response from the tone" : "sound", shift, n_diff, max_d * 1e6);
            } else CHECK(0, "%s: a run failed", v ? "sound from the tone" : "sound");
            free((void*)o.def);
        }
        if (!k) printf("  sound: no pack, skipped\n");
    }
}

/* --- (3) two threads ------------------------------------------------------------- */

#if !defined(_WIN32)
typedef pthread_t th_t;
static int th_start(th_t* t, void* (*fn)(void*), void* arg) { return pthread_create(t, NULL, fn, arg); }
static void th_join(th_t t) { pthread_join(t, NULL); }
/* pthread_getschedparam(), not sched_getscheduler(): macOS has no
 * sched_getscheduler() */
static int normal_policy(void) {
    struct sched_param sp;
    int policy = -1;
    return pthread_getschedparam(pthread_self(), &policy, &sp) == 0 && policy == SCHED_OTHER;
}
#define TH_RET void*
#define TH_OK NULL
#else
typedef HANDLE th_t;
typedef struct th_tramp { void* (*fn)(void*); void* arg; } th_tramp;
static unsigned __stdcall th_run(void* a) { th_tramp* t = (th_tramp*)a; t->fn(t->arg); free(t); return 0; }
static int th_start(th_t* t, void* (*fn)(void*), void* arg) {
    th_tramp* tr = (th_tramp*)malloc(sizeof *tr);
    tr->fn = fn;
    tr->arg = arg;
    *t = (HANDLE)_beginthreadex(NULL, 0, th_run, tr, 0, NULL);
    return *t ? 0 : -1;
}
static void th_join(th_t t) { WaitForSingleObject(t, INFINITE); CloseHandle(t); }
static int normal_policy(void) { return GetThreadPriority(GetCurrentThread()) <= THREAD_PRIORITY_NORMAL; }
#define TH_RET void*
#define TH_OK NULL
#endif

typedef struct two_thread { pl_player* p; const pk* k; const char* out; int ok_load, ok_steps, virt, elevated, frames_ff; } two_thread;

static TH_RET thread_load(void* arg) {
    two_thread* t = (two_thread*)arg;
    pl_sim_desc d;
    pl_check_result r;
    run_opts o;
    int i;
    memset(&o, 0, sizeof o);
    o.seed = 7;
    sim_desc(&d, t->k, &o, t->out);
    t->ok_load = pl_sim_load(t->p, &d, &r) == PL_CHECK_OK;
    /* the fast forward: no draw, no GL */
    for (i = 0; t->ok_load && i < t->frames_ff; i++)
        if (sim_frame(t->p, false) != 0) { t->ok_load = 0; break; }
    if (yrt_is_virtual()) t->virt = 1;
    if (!normal_policy()) t->elevated = 1;
    return TH_OK;
}

static TH_RET thread_step(void* arg) {
    two_thread* t = (two_thread*)arg;
    int rc = 0;
    t->ok_steps = pl_sim_attach(t->p, 0) == 1;
    while (t->ok_steps && (rc = sim_frame(t->p, true)) == 0) { }
    if (rc < 0) t->ok_steps = 0;
    if (yrt_is_virtual()) t->virt = 1;
    if (!normal_policy()) t->elevated = 1;
    return TH_OK;
}

static void case_threads(void) {
    const pk* k = pack_of("rdk");
    two_thread t;
    th_t a, b;
    char ref[1200], got[1200];
    run_opts o;
    memset(&t, 0, sizeof t);
    memset(&o, 0, sizeof o);
    o.seed = 7;
    path_in(ref, sizeof ref, "threads_sim");
    path_in(got, sizeof got, "threads_step");
    t.p = (pl_player*)calloc(1, sizeof *t.p);
    t.k = k;
    t.out = got;
    t.frames_ff = 600;   /* 10 s, past the first trials */
    CHECK(!yrt_is_virtual(), "threads: the clock is virtual before the case");
    if (th_start(&a, thread_load, &t) != 0) { CHECK(0, "threads: no thread"); free(t.p); return; }
    th_join(a);
    CHECK(t.ok_load, "threads: the load or the fast forward failed: %s", t.p->error);
    CHECK(t.p->writer_open, "threads: the data set's writer is not running: a stepped session writes in place");
    CHECK(t.p->gfx.frames == 0, "threads: the fast forward drew %llu frames", (unsigned long long)t.p->gfx.frames);
    if (th_start(&b, thread_step, &t) != 0) { CHECK(0, "threads: no thread"); pl_close(t.p); free(t.p); return; }
    th_join(b);
    CHECK(t.ok_steps, "threads: attach or the frames failed: %s", t.p->error);
    CHECK(t.p->gfx.frames > 0, "threads: the drawn frames drew nothing");
    CHECK(!t.virt && !yrt_is_virtual(), "threads: the clock became virtual");
    CHECK(!t.elevated, "threads: a stepping thread left normal scheduling");
    pl_close(t.p);
    free(t.p);
    CHECK(ref_run(k, &o, ref) == 0 && same_data(ref, got, "threads"), "threads: the data differ from --sim's");
    printf("  threads: load and 600 frames on one thread, attach and the rest on another: --sim's data; no virtual clock, no elevation\n");
}

/* --- (4) two sessions in turn, pl_check() on a third thread ------------------------ */

/* the checker's stop flag: one int that two threads read and write */
#if defined(_MSC_VER)
static int ld_int(volatile long* x) { return (int)InterlockedCompareExchange(x, 0, 0); }
static void st_int(volatile long* x, int v) { (void)InterlockedExchange(x, (long)v); }
typedef volatile long flag_t;
#else
static int ld_int(int* x) { return __atomic_load_n(x, __ATOMIC_ACQUIRE); }
static void st_int(int* x, int v) { __atomic_store_n(x, v, __ATOMIC_RELEASE); }
typedef int flag_t;
#endif
typedef struct checker { const pk* k; flag_t stop; int n, bad; } checker;
static TH_RET thread_check(void* arg) {
    checker* c = (checker*)arg;
    pl_player* p = (pl_player*)calloc(1, sizeof *p);
    while (!ld_int(&c->stop)) {
        pl_check_desc d;
        pl_check_result r;
        memset(&d, 0, sizeof d);
        d.pack.data = c->k->data;
        d.pack.size = c->k->size;
        d.seed = 1;
        d.cal = &nominal;
        if (pl_check(p, &d, &r) != PL_CHECK_OK) c->bad++;
        pl_close(p);
        c->n++;
    }
    free(p);
    return TH_OK;
}

static void case_two_sessions(void) {
    const pk* k1 = pack_of("rdk");
    const pk* k2 = pack_of("oddball");
    const pk* kc = pack_of("afc2") ? pack_of("afc2") : k2;
    pl_player* p1 = (pl_player*)calloc(1, sizeof *p1);
    pl_player* p2 = (pl_player*)calloc(1, sizeof *p2);
    pl_sim_desc d1, d2;
    pl_check_result r1, r2;
    run_opts o1, o2;
    char a1[1200], b1[1200], a2[1200], b2[1200];
    checker c;
    th_t t;
    int f1 = 0, f2 = 0, ok1, ok2;
    memset(&o1, 0, sizeof o1);
    memset(&o2, 0, sizeof o2);
    memset(&c, 0, sizeof c);
    o1.seed = 11;
    o2.seed = 12;
    o2.rig = true;
    path_in(a1, sizeof a1, "two1_sim");
    path_in(b1, sizeof b1, "two1_step");
    path_in(a2, sizeof a2, "two2_sim");
    path_in(b2, sizeof b2, "two2_step");
    c.k = kc;
    if (th_start(&t, thread_check, &c) != 0) { CHECK(0, "two sessions: no thread"); return; }
    sim_desc(&d1, k1, &o1, b1);
    sim_desc(&d2, k2, &o2, b2);
    /* both loads, so both results are set for the message (MSVC C4701) */
    ok1 = pl_sim_load(p1, &d1, &r1) == PL_CHECK_OK;
    ok2 = pl_sim_load(p2, &d2, &r2) == PL_CHECK_OK;
    CHECK(ok1 && ok2, "two sessions: a load failed: %s %s", r1.message, r2.message);
    if (r1.status == PL_CHECK_OK && r2.status == PL_CHECK_OK) {
        (void)pl_sim_attach(p1, 0);
        (void)pl_sim_attach(p2, 0);
        while (f1 == 0 || f2 == 0) {
            if (f1 == 0) f1 = sim_frame(p1, true);
            if (f2 == 0) f2 = sim_frame(p2, false);
        }
    }
    pl_close(p1);
    pl_close(p2);
    st_int(&c.stop, 1);
    th_join(t);
    CHECK(f1 == 1 && f2 == 1, "two sessions: frames gave %d and %d", f1, f2);
    CHECK(c.n > 0 && c.bad == 0, "two sessions: %d checks, %d not OK", c.n, c.bad);
    CHECK(ref_run(k1, &o1, a1) == 0 && same_data(a1, b1, "two sessions, rdk"), "two sessions: rdk's data differ from its --sim");
    CHECK(ref_run(k2, &o2, a2) == 0 && same_data(a2, b2, "two sessions, oddball"), "two sessions: oddball's data differ from its --sim");
    printf("  two sessions in turn, %d checks of %s on a third thread: each as its --sim alone\n", c.n, kc->name);
    free(p1);
    free(p2);
}

/* --- (5) input ------------------------------------------------------------------ */

/* Steps to the frame whose next onset is at least `ms` after the response
 * phase began; returns the response's onset (the `from` stimulus's flip),
 * or -1 */
static int step_to_response(pl_player* p, int64_t ms) {
    pl_sim_where w;
    int64_t start = -1;
    int guard;
    for (guard = 0; guard < 100000; guard++) {
        pl_sim_where_now(p, &w);
        if (w.phase == PL_SIM_RESPONSE && start < 0) start = pl_sim_next_onset(p);
        if (start >= 0 && pl_sim_next_onset(p) - start >= ms * 1000000) return 0;
        if (sim_frame(p, false) != 0) return -1;
    }
    return -1;
}

static bool live_all(void* ctx, const pl_sim_where* w) { (void)ctx; (void)w; return true; }

static int step_trial_end(pl_player* p, int trials) {
    int guard;
    for (guard = 0; guard < 100000 && p->trials_done < trials; guard++)
        if (sim_frame(p, false) != 0) return -1;
    return p->trials_done >= trials ? 0 : -1;
}

/* The value of a column in trials.csv's row `row` (0 the first), or "" */
static void cell_of(const char* dir, int row, const char* col, char* out, size_t cap) {
    char path[1300], *t = NULL, *line, *hdr;
    size_t n;
    int ci = -1, i;
    void* x = NULL;
    out[0] = 0;
    snprintf(path, sizeof path, "%s/trials.csv", dir);
    if (read_file(path, &x, &n) != 0) return;
    t = (char*)x;
    hdr = t;
    line = strchr(t, '\n');
    if (!line) { free(x); return; }
    *line = 0;
    {
        char* c = hdr;
        for (i = 0; c; i++) {
            char* e = strchr(c, ',');
            size_t L = e ? (size_t)(e - c) : strlen(c);
            if (L == strlen(col) && strncmp(c, col, L) == 0) { ci = i; break; }
            c = e ? e + 1 : NULL;
        }
    }
    line++;
    for (i = 0; i < row && line; i++) { line = strchr(line, '\n'); if (line) line++; }
    if (ci >= 0 && line && *line) {
        char* c = line;
        for (i = 0; i < ci && c; i++) { c = strchr(c, ','); if (c) c++; }
        if (c) {
            size_t L = strcspn(c, ",\n");
            snprintf(out, cap, "%.*s", (int)(L < cap ? L : cap - 1), c);
        }
    }
    free(x);
}

static int64_t s_to_ns(const char* s) { return s[0] ? (int64_t)llround(strtod(s, NULL) * 1e9) : -1; }

static void case_input(void) {
    const pk* k = pack_of("rdk");
    pl_player* p = (pl_player*)calloc(1, sizeof *p);
    pl_sim_desc d;
    pl_check_result r;
    run_opts o;
    char out[1200], cell[256], cell2[256];
    int64_t t_key = 0, t_choice = 0;
    pl_sim_where w;
    memset(&o, 0, sizeof o);
    o.seed = 7;
    path_in(out, sizeof out, "input");
    sim_desc(&d, k, &o, out);
    d.live = live_all;
    CHECK(pl_sim_load(p, &d, &r) == PL_CHECK_OK, "input: load: %s", r.message);
    if (r.status == PL_CHECK_OK) {
        /* trial 0: the left key 700 ms into the response phase (the dots
         * show at 500 ms), 5 ms before the next frame */
        CHECK(step_to_response(p, 700) == 0, "input: no response phase");
        t_key = pl_sim_next_onset(p) - 5000000;
        CHECK(pl_sim_key(p, pl_sim_next_onset(p) + 1, yrsp_scancode("left"), true) < 0, "input: a key after the next onset was taken");
        CHECK(pl_sim_key(p, pl_sim_next_onset(p) - p->sim.period, yrsp_scancode("left"), true) < 0, "input: a key at the last onset was taken");
        CHECK(pl_sim_key(p, t_key, yrsp_scancode("left"), true) == 0, "input: the key refused");
        CHECK(sim_frame(p, false) == 0, "input: the frame after the key");
        CHECK(pl_sim_key(p, pl_sim_next_onset(p) - 1, yrsp_scancode("left"), false) == 0, "input: the release refused");
        CHECK(step_trial_end(p, 1) == 0, "input: trial 0 did not end");
        pl_sim_where_now(p, &w);
        CHECK(w.response && strcmp(w.response, "left") == 0, "input: where_now's response '%s'", w.response ? w.response : "(none)");
        /* trial 1: the second choice ("right") 800 ms in */
        CHECK(step_to_response(p, 800) == 0, "input: no second response phase");
        t_choice = pl_sim_next_onset(p) - 1;
        CHECK(pl_sim_choice(p, t_choice, 2, true) < 0, "input: a third choice of two was taken");
        CHECK(pl_sim_choice(p, t_choice, 1, true) == 0, "input: the choice refused");
        CHECK(step_trial_end(p, 2) == 0, "input: trial 1 did not end");
        pl_sim_where_now(p, &w);
        CHECK(w.response && strcmp(w.response, "right") == 0, "input: the choice gave '%s'", w.response ? w.response : "(none)");
    }
    pl_close(p);
    /* rt = t - onset, from the data set */
    cell_of(out, 0, "response", cell, sizeof cell);
    CHECK(strcmp(cell, "left") == 0, "input: trial 0's response '%s'", cell);
    cell_of(out, 0, "rt_s", cell, sizeof cell);
    cell_of(out, 0, "dots_onset_s", cell2, sizeof cell2);
    CHECK(s_to_ns(cell) >= 0 && s_to_ns(cell) == t_key - s_to_ns(cell2), "input: rt %s s, want t - onset = %lld ns", cell, (long long)(t_key - s_to_ns(cell2)));
    cell_of(out, 1, "response", cell, sizeof cell);
    CHECK(strcmp(cell, "right") == 0, "input: trial 1's response '%s'", cell);
    cell_of(out, 1, "rt_s", cell, sizeof cell);
    cell_of(out, 1, "dots_onset_s", cell2, sizeof cell2);
    CHECK(s_to_ns(cell) == t_choice - s_to_ns(cell2), "input: the choice's rt %s s, want %lld ns", cell, (long long)(t_choice - s_to_ns(cell2)));
    free(p);

    /* a sample that ends a reach: a position2d role in px; out past 100 px
     * ends the trial, one at 50 px does not */
    {
        static const char reach[] =
            "{\"format\": \"ysp-experiment\", \"version\": 2, \"name\": \"reach\","
            " \"roles\": {\"hand\": {\"type\": \"position2d\", \"units\": \"display_px\"}},"
            " \"stimuli\": {\"dot\": {\"kind\": \"shape\", \"w\": 10, \"h\": 10}},"
            " \"trials\": {\"t\": {\"controls\": {\"out\": {\"control\": \"region\", \"of\": \"hand\", \"radius\": 100, \"outside\": true}},"
            "   \"states\": [{\"name\": \"hold\", \"steps\": [{\"on\": \"dot\"}, {\"wait_s\": 0.2}]},"
            "                {\"name\": \"reach\", \"exits\": [{\"on\": \"out\", \"to\": \"end\", \"outcome\": \"out\"}]}],"
            "   \"columns\": {\"out_x\": {\"hit\": \"out.x\"}, \"out_s\": {\"time\": \"out\", \"from\": \"dot\"}}}},"
            " \"timeline\": [{\"name\": \"l\", \"trial\": \"t\", \"rules\": [\"reps 1\"]}]}";
        pl_player* q = (pl_player*)calloc(1, sizeof *q);
        char rout[1200];
        int64_t t_out = 0, onset0 = 0;
        int i;
        o.def = reach;
        path_in(rout, sizeof rout, "reach");
        sim_desc(&d, k, &o, rout);
        CHECK(pl_sim_load(q, &d, &r) == PL_CHECK_OK, "reach: load: %s", r.message);
        if (r.status == PL_CHECK_OK) {
            onset0 = pl_sim_next_onset(q);
            for (i = 0; i < 30; i++) {   /* 0.5 s: in reach */
                CHECK(pl_sim_sample(q, pl_sim_next_onset(q) - 1, 0, 50.0f, 0.0f) == 0, "reach: a sample refused");
                CHECK(sim_frame(q, false) == 0, "reach: frame %d", i);
            }
            pl_sim_where_now(q, &w);
            CHECK(w.state && strcmp(w.state, "reach") == 0 && w.phase != PL_SIM_BETWEEN, "reach: at 50 px the state is '%s'", w.state ? w.state : "(none)");
            CHECK(pl_sim_sample(q, 0, 7, 0, 0) < 0, "reach: a sample of no role was taken");
            t_out = pl_sim_next_onset(q) - 3000000;
            CHECK(pl_sim_sample(q, t_out, 0, 150.0f, 0.0f) == 0, "reach: the sample refused");
            CHECK(pl_sim_sample(q, t_out - 1, 0, 0.0f, 0.0f) < 0, "reach: a sample earlier than the last was taken");
            CHECK(step_trial_end(q, 1) == 0, "reach: the trial did not end");
            pl_sim_where_now(q, &w);
            CHECK(w.outcome && strcmp(w.outcome, "out") == 0, "reach: outcome '%s'", w.outcome ? w.outcome : "(none)");
        }
        pl_close(q);
        free(q);
        cell_of(rout, 0, "out_x", cell, sizeof cell);
        CHECK(strtod(cell, NULL) == 150.0, "reach: out_x '%s'", cell);
        cell_of(rout, 0, "out_s", cell, sizeof cell);
        CHECK(s_to_ns(cell) == t_out - onset0, "reach: out_s '%s', want %lld ns after the dot", cell, (long long)(t_out - onset0));
        cell_of(rout, 0, "outcome", cell, sizeof cell);
        CHECK(strcmp(cell, "out") == 0, "reach: trials.csv's outcome '%s'", cell);
        printf("  input: a key and a choice give their responses and rt = t - onset; a sample past 100 px ends the reach\n");
    }
}

/* --- (6) the states, (7) a refusal, (8) live(), (9) a stop ------------------------ */

static void case_states(void) {
    const pk* k = pack_of("bimanual");
    static state_log log[20000];
    int n_log = 0, i, n_rows = 0, bad = 0, n_seen = 0;
    run_opts o;
    char a[1200], b[1200], sp[1200], path[1300];
    void* x = NULL;
    size_t nx;
    if (!k) { printf("  states: no bimanual pack, skipped\n"); return; }
    memset(&o, 0, sizeof o);
    o.seed = 5;
    o.rig = true;
    o.def = edit_all(edit_all(edit_all(def_of(k), "\"reps 30\"", "\"reps 1\""), "\"wait_s\": 46", "\"wait_s\": 2"),
                     ARRIVED, ARRIVED_OR(1));
    path_in(sp, sizeof sp, "states_samples.csv");
    path_in(a, sizeof a, "states_sim");
    path_in(b, sizeof b, "states_step");
    o.samples = sp;
    o.gain_roles[0] = "left";
    o.gain_roles[1] = "right";
    if (!o.def || write_samples_bi(sp, 60) != 0 || ref_run(k, &o, a) != 0 || step_run(k, &o, b, log, 20000, &n_log) != 0) {
        CHECK(0, "states: a run failed");
        free((void*)o.def);
        return;
    }
    /* each frame's state is the one whose row (trial, entry frame) is the
     * last at or before it, among rows of a state that lasted a frame */
    snprintf(path, sizeof path, "%s/events.csv", a);
    if (read_file(path, &x, &nx) == 0) {
        char* line = strchr((char*)x, '\n');
        static struct { int trial; int64_t frame; char text[32]; } rows[4096];
        while (line && *++line) {
            char f[20][64];
            int nf = 0;
            char* c = line, *e = strchr(line, '\n');
            if (e) *e = 0;
            while (nf < 20) {
                size_t L = strcspn(c, ",");
                snprintf(f[nf++], 64, "%.*s", (int)(L < 63 ? L : 63), c);
                if (!c[L]) break;
                c += L + 1;
            }
            if (nf >= 20 && strcmp(f[2], "state") == 0 && n_rows < 4096) {
                rows[n_rows].trial = atoi(f[1]);
                rows[n_rows].frame = strtoll(f[5], NULL, 10);
                snprintf(rows[n_rows].text, 32, "%.31s", f[19]);
                n_rows++;
            }
            line = e;
        }
        for (i = 0; i < n_log; i++) {
            const char* want = "";
            int j;
            if (!log[i].state[0]) continue;   /* between trials, or ended in its frame */
            n_seen++;
            for (j = 0; j < n_rows; j++)
                if (rows[j].trial == log[i].trial && rows[j].frame <= log[i].frame) want = rows[j].text;
            if (strcmp(want, log[i].state) != 0) {
                if (bad < 5) printf("  states: frame %lld trial %d: where_now '%s', --sim's rows '%s'\n", (long long)log[i].frame, log[i].trial, log[i].state, want);
                bad++;
            }
        }
        free(x);
    }
    CHECK(n_rows > 20 && bad == 0 && n_seen > n_log / 2, "states: %d state rows, %d frames whose state differs, %d of %d frames in a state", n_rows, bad, n_seen, n_log);
    CHECK(same_data(a, b, "states"), "states: the data differ from --sim's");
    printf("  states: %d frames' states follow --sim's %d state rows\n", n_log, n_rows);
    free((void*)o.def);
}

static void case_refusal(void) {
    const pk* k = pack_of("rdk");
    static const char def[] =
        "{\"format\": \"ysp-experiment\", \"version\": 1, \"name\": \"t\","
        " \"devices\": {\"r\": {\"direction\": \"in\", \"family\": [\"keyboard\"], \"required\": true}},"
        " \"stimuli\": {\"s\": {\"kind\": \"shape\", \"w\": 10, \"h\": 10}},"
        " \"trials\": {\"win\": {\"events\": [{\"on\": \"s\"}, {\"wait_s\": 0.2}],"
        "   \"response\": {\"role\": \"r\", \"choices\": [\"left\", \"right\"], \"from\": \"s\", \"duration_s\": \"$d_s\", \"minimum_valid_rt_s\": \"$m_s\"}}},"
        " \"timeline\": [{\"name\": \"l\", \"trial\": \"win\", \"vars\": {\"d_s\": {\"choice\": [0.5]}, \"m_s\": {\"choice\": [0.6]}}}]}";
    pl_player* p = (pl_player*)calloc(1, sizeof *p);
    pl_sim_desc d;
    pl_check_result r;
    run_opts o;
    char out[1200];
    int f = 0, i;
    memset(&o, 0, sizeof o);
    o.seed = 1;
    o.def = def;
    path_in(out, sizeof out, "refusal");
    sim_desc(&d, k, &o, out);
    CHECK(pl_sim_load(p, &d, &r) == PL_CHECK_OK, "refusal: the load refused: %s", r.message);
    for (i = 0; i < 100 && f == 0; i++) f = sim_frame(p, false);
    CHECK(f < 0 && strstr(p->error, "leaves no time") && p->fail_obj == PL_OBJ_TEMPLATE && p->fail_name && strcmp(p->fail_name, "win") == 0,
          "refusal: frame gave %d, '%s', object %d '%s'", f, p->error, p->fail_obj, p->fail_name ? p->fail_name : "");
    CHECK(sim_frame(p, false) < 0, "refusal: a frame after the refusal went on");
    pl_close(p);
    free(p);
    printf("  refusal: at run time, named template 'win'\n");
}

typedef struct live_log { int n, bad, last; } live_log;
static bool live_odd(void* ctx, const pl_sim_where* w) {
    live_log* l = (live_log*)ctx;
    if (w->trial != l->last + 1 || !w->leaf || strcmp(w->leaf, "main") || !w->tmpl || strcmp(w->tmpl, "rdk") || w->condition < 0) l->bad++;
    l->last = w->trial;
    l->n++;
    return (w->trial & 1) != 0;
}

static void case_live(void) {
    const pk* k = pack_of("rdk");
    pl_player* p = (pl_player*)calloc(1, sizeof *p);
    pl_sim_desc d;
    pl_check_result r;
    run_opts o;
    live_log l;
    char out[1200], cell[256], name[64];
    int f = 0, i, bad = 0;
    memset(&o, 0, sizeof o);
    memset(&l, 0, sizeof l);
    l.last = -1;
    o.seed = 7;
    path_in(out, sizeof out, "live");
    sim_desc(&d, k, &o, out);
    d.live = live_odd;
    d.ctx = &l;
    CHECK(pl_sim_load(p, &d, &r) == PL_CHECK_OK, "live: load: %s", r.message);
    while (r.status == PL_CHECK_OK && (f = sim_frame(p, false)) == 0) { }
    CHECK(f == 1, "live: the session did not end: %s", p->error);
    pl_close(p);
    free(p);
    CHECK(l.n == 40 && l.bad == 0, "live: asked %d times, %d with a wrong trial, leaf, template or row", l.n, l.bad);
    for (i = 0; i < 40; i++) {
        char key[64];
        cell_of(out, i, "response", cell, sizeof cell);
        cell_of(out, i, "key_answer", key, sizeof key);
        /* the caller's trials (odd) had no input: no response; the scripted
         * participant answers the others correctly */
        if ((i & 1) ? cell[0] != 0 : strcmp(cell, key) != 0) {
            if (bad < 3) printf("  live: trial %d response '%s', key_answer '%s'\n", i, cell, key);
            bad++;
        }
    }
    snprintf(name, sizeof name, "%d of 40", 40 - bad);
    CHECK(bad == 0, "live: %d trials wrong", bad);
    printf("  live: asked at each of 40 trials' starts; the caller's 20 silent, the script's 20 answered (%s)\n", name);
}

static void case_stop(void) {
    const pk* k = pack_of("rdk");
    pl_player* p = (pl_player*)calloc(1, sizeof *p);
    pl_sim_desc d;
    pl_check_result r;
    run_opts o;
    char out[1200], path[1300], cell[64], want[64];
    int i, done = 0;
    memset(&o, 0, sizeof o);
    o.seed = 7;
    path_in(out, sizeof out, "stop");
    sim_desc(&d, k, &o, out);
    CHECK(pl_sim_load(p, &d, &r) == PL_CHECK_OK, "stop: load: %s", r.message);
    for (i = 0; i < 500 && r.status == PL_CHECK_OK; i++) CHECK(sim_frame(p, i % 2 == 0 && pl_sim_attach(p, 0) == 1) == 0, "stop: frame %d", i);
    done = p->trials_done;
    pl_sim_stop(p);
    CHECK(pl_sim_key(p, pl_sim_next_onset(p) - 1, yrsp_scancode("left"), true) < 0, "stop: a key after the stop was taken");
    pl_sim_detach(p);
    pl_close(p);   /* the end's frames and session.json here */
    free(p);
    snprintf(path, sizeof path, "%s/session.json", out);
    snprintf(want, sizeof want, "\"trials_written\": %d", done);
    CHECK(file_has(path, "\"aborted\"") && file_has(path, "\"request\"") && file_has(path, want), "stop: session.json has no aborted, request and %s", want);
    cell_of(out, done - 1, "trial_index", cell, sizeof cell);
    CHECK(done > 0 && atoi(cell) == done - 1, "stop: trials.csv's last row is trial '%s', want %d", cell, done - 1);
    cell_of(out, done, "trial_index", cell, sizeof cell);
    CHECK(cell[0] == 0, "stop: a row past the stop: trial '%s'", cell);
    printf("  stop: after 500 frames, %d trials written; session.json says aborted, request\n", done);
}

/* The threads of the process (Linux), or -1. */
static int threads(void) {
#if defined(__linux__)
    char names[8192];
    int n = 0;
    const char* c;
    list_dir("/proc/self/task", names, sizeof names);
    for (c = names; *c; c++) if (*c == '\n') n++;
    return n;
#else
    return -1;
#endif
}

/* The needs as sorted "kind:name:value" text */
static void needs_text(const pl_check_result* r, char* buf, size_t cap) {
    char lines[64][128];
    int i, j, n = r->n_needs < 64 ? r->n_needs : 64;
    size_t used = 0;
    for (i = 0; i < n; i++) snprintf(lines[i], sizeof lines[i], "%d:%s:%d", r->needs[i].kind, r->needs[i].name ? r->needs[i].name : "", r->needs[i].value);
    for (i = 1; i < n; i++)
        for (j = i; j > 0 && strcmp(lines[j - 1], lines[j]) > 0; j--) {
            char t[128];
            memcpy(t, lines[j], sizeof t);
            memcpy(lines[j], lines[j - 1], sizeof t);
            memcpy(lines[j - 1], t, sizeof t);
        }
    buf[0] = 0;
    for (i = 0; i < n && used < cap; i++) used += (size_t)snprintf(buf + used, cap - used, "%s%s", i ? " " : "", lines[i]);
}

/* The built-in rig and the desc's display, rig and calibration; the
 * check's needs; the order of attach and a drawn frame */
static void case_display(void) {
    const char* names[] = { "rdk", "stroop", "afc2", "oddball", "visuomotor", "bimanual", "sound" };
    const pk* k = pack_of("oddball");
    pl_player* p = (pl_player*)calloc(1, sizeof *p);
    pl_player* q = (pl_player*)calloc(1, sizeof *q);
    pl_sim_desc d;
    pl_check_result r, rc;
    run_opts o;
    char out[1200], a[2048], b[2048];
    size_t i;
    yrig_profile rig;
    memset(&o, 0, sizeof o);
    o.seed = 3;
    /* the built-in rig: 1920 x 1080 at 60 Hz, 1920 px over 518.4 mm */
    path_in(out, sizeof out, "display1");
    sim_desc(&d, k, &o, out);
    d.width = d.height = 0;
    CHECK(pl_sim_load(p, &d, &r) == PL_CHECK_OK, "display: load: %s", r.message);
    CHECK(p->gfx.w == 1920 && p->gfx.h == 1080 && p->caps.period_ns == 16666667, "display: %dx%d, %lld ns", (int)p->gfx.w, (int)p->gfx.h, (long long)p->caps.period_ns);
    CHECK(fabs(p->view.width_mm - 518.4f) < 1e-3f && fabs(p->view.distance_mm - 570.0f) < 1e-3f, "display: the view %g mm wide at %g mm", (double)p->view.width_mm,
          (double)p->view.distance_mm);
    CHECK(!p->have_cal, "display: a calibration with none given and none required");
    CHECK(pl_sim_frame(p, true) < 0 && strstr(p->error, "pl_sim_attach"), "display: a drawn frame before attach: '%s'", p->error);
    pl_close(p);
    /* the desc's rig (1024 px over 320 mm) and calibration, at 120 Hz */
    memset(&rig, 0, sizeof rig);
    yrig_init(&rig, "sim-test");
    rig.view.set = true;
    rig.view.distance_mm = 600.0;
    rig.view.width_mm = 320.0;
    rig.view.width_px = 1024;
    path_in(out, sizeof out, "display2");
    sim_desc(&d, k, &o, out);
    d.width = 1024;
    d.height = 768;
    d.period_ns = 8333333;
    d.rig = &rig;
    d.cal = &nominal;
    CHECK(pl_sim_load(p, &d, &r) == PL_CHECK_OK, "display: load with a rig: %s", r.message);
    CHECK(fabs(p->view.width_mm - 320.0f) < 1e-3f && fabs(p->view.distance_mm - 600.0f) < 1e-3f && p->have_cal && p->have_rig, "display: the rig's view %g mm at %g mm, cal %d",
          (double)p->view.width_mm, (double)p->view.distance_mm, (int)p->have_cal);
    {
        int64_t t0 = pl_sim_next_onset(p);
        CHECK(t0 == YRT_VIRTUAL_T0_NS + 7 * 8333333LL, "display: frame 0 at %lld", (long long)t0);
        CHECK(pl_sim_frame(p, false) == 0 && pl_sim_next_onset(p) - t0 == 8333333, "display: a frame of %lld ns", (long long)(pl_sim_next_onset(p) - t0));
    }
    pl_close(p);
    /* a sound: no audio thread, only the data set's writer */
    if (pack_of("sound")) {
        int t0 = threads();
        path_in(out, sizeof out, "display3");
        sim_desc(&d, pack_of("sound"), &o, out);
        CHECK(pl_sim_load(p, &d, &r) == PL_CHECK_OK, "display: sound: %s", r.message);
        CHECK(threads() == (t0 < 0 ? -1 : t0 + 1), "display: a sound's session has %d threads, %d before", threads(), t0);
        pl_close(p);
    }
    /* a session's needs are the check's, with no fields given */
    for (i = 0; i < sizeof names / sizeof names[0]; i++) {
        const pk* kk = pack_of(names[i]);
        pl_check_desc cd;
        char dir[200];
        if (!kk) continue;
        snprintf(dir, sizeof dir, "needs_%s", names[i]);
        path_in(out, sizeof out, dir);
        sim_desc(&d, kk, &o, out);
        d.n_fields = 0;
        memset(&cd, 0, sizeof cd);
        cd.pack.data = kk->data;
        cd.pack.size = kk->size;
        cd.seed = 3;
        CHECK(pl_sim_load(p, &d, &r) == PL_CHECK_OK, "needs %s: load: %s", names[i], r.message);
        CHECK(pl_check(q, &cd, &rc) == PL_CHECK_OK, "needs %s: check: %s", names[i], rc.message);
        needs_text(&r, a, sizeof a);
        needs_text(&rc, b, sizeof b);
        CHECK(strcmp(a, b) == 0 && a[0], "needs %s: the session's '%s', the check's '%s'", names[i], a, b);
        pl_close(p);
        pl_close(q);
    }
    free(p);
    free(q);
    printf("  display: the built-in rig, a rig and a calibration from the desc, 120 Hz; the needs of the check; no drawn frame before attach\n");
}

/* --- (12) a profile (0.18.0): the check's needs, the profile's adapters --- */

static yrig_profile rig_dev, rig_room, rig_changed;
static char room_path[1200];

static int write_file(const char* path, const void* data, size_t n) {
    FILE* fp = fopen(path, "wb");
    if (!fp) return -1;
    if (fwrite(data, 1, n, fp) != n) { fclose(fp); return -1; }
    return fclose(fp) == 0 ? 0 : -1;
}

static void hex_of(const void* data, size_t n, char hex[65]) {
    static const char H[] = "0123456789abcdef";
    uint8_t dg[32];
    int i;
    yrig_sha256(data, n, dg);
    for (i = 0; i < 32; i++) { hex[i * 2] = H[dg[i] >> 4]; hex[i * 2 + 1] = H[dg[i] & 15]; }
    hex[64] = 0;
}

/* In work (make_rig()'s folder, with its display.yspcal): dev binds
 * oddball's resp (xid, a checked bounds loopback: tier 1) and trig, with
 * make_rig's view; room binds no role, has a view of 1024 px over 320 mm
 * (32 px a cm) and the signals of hand, left and right; changed is room
 * with a calibration whose file failed its check. room is also a file,
 * for --sim --rig. */
static int rig_profiles(void) {
    static const char BOUNDS[] = "# a test fixture, not a measurement\nformat ysp-in-bounds 1\ndate 2026-11-22T00:00:00Z\nin_family xid\n"
                                 "n 200\nmissed 0\nlo_s -0.000100000\nmedian_s 0.000050000\nhi_s 0.000400000\n";
    static char text[4096], cal[sizeof(ycol_cal) + 16];
    char path[1300], err[300], bsha[65], csha[65], disp[256];
    FILE* fp;
    size_t n;
    path_in(path, sizeof path, "display.yspcal");
    if (!(fp = fopen(path, "rb"))) return -1;
    n = fread(cal, 1, sizeof cal, fp);
    fclose(fp);
    hex_of(cal, n, csha);
    hex_of(BOUNDS, strlen(BOUNDS), bsha);
    path_in(path, sizeof path, "loopback");
    if (make_dir(path) != 0) return -1;
    snprintf(path, sizeof path, "%.900s/loopback/%s.txt", work, bsha);
    if (write_file(path, BOUNDS, strlen(BOUNDS)) != 0) return -1;
    snprintf(disp, sizeof disp, "\"display\": {\"onset_offset_s\": 0, \"calibration\": {\"file\": \"display.yspcal\", \"sha256\": \"%s\"}}", csha);
    snprintf(text, sizeof text,
             "{\"format\": \"ysp-rig 1\", \"rig\": \"dev\", \"written\": \"2026-11-22T00:00:00Z\", \"roles\": {"
             "\"resp\": {\"family\": \"xid\", \"key\": \"port:/dev/null\", \"bounds\": {\"n\": 200, \"lo_s\": -0.0001, \"hi_s\": 0.0004, "
             "\"date\": \"2026-11-22T00:00:00Z\", \"sha256\": \"%s\"}}, \"trig\": {\"family\": \"triggerbox\", \"key\": \"port:/dev/null\"}}, %s, "
             "\"view\": {\"distance_mm\": 570, \"width_mm\": 518.4, \"width_px\": 1920}}",
             bsha, disp);
    if (yrig_parse(&rig_dev, text, strlen(text), work, err, sizeof err) != YRIG_OK) { printf("  dev: %s\n", err); return -1; }
    snprintf(text, sizeof text,
             "{\"format\": \"ysp-rig 1\", \"rig\": \"room\", \"written\": \"2026-11-22T00:00:00Z\", \"roles\": {}, %s, "
             "\"view\": {\"distance_mm\": 570, \"width_mm\": 320, \"width_px\": 1024}, \"signals\": {"
             "\"hand\": {\"adapter\": \"raw_mouse\", \"units\": \"display_px\", \"per_count\": [2, -1.5], \"device\": \"click\"}, "
             "\"left\": {\"adapter\": \"raw_mouse\", \"units\": \"display_px\", \"per_count\": 16, \"device\": \"click\"}, "
             "\"right\": {\"adapter\": \"raw_mouse\", \"units\": \"display_px\", \"per_count\": 16, \"device\": \"click\"}}}\n",
             disp);
    if (yrig_parse(&rig_room, text, strlen(text), work, err, sizeof err) != YRIG_OK) { printf("  room: %s\n", err); return -1; }
    path_in(room_path, sizeof room_path, "room.json");
    if (write_file(room_path, text, strlen(text)) != 0) return -1;
    memcpy(&rig_changed, &rig_room, sizeof rig_changed);
    rig_changed.display.check = YRIG_CHECK_CHANGED;   /* as yrig_load() gives for an edited file */
    return rig_dev.n_notes == 0 && rig_room.n_notes == 0 && rig_dev.role[0].bounds.check == YRIG_CHECK_OK && rig_room.display.check == YRIG_CHECK_OK ? 0 : -1;
}

/* status, message and needs */
static void result_text(const pl_check_result* r, char* buf, size_t cap) {
    char needs[1024];
    needs_text(r, needs, sizeof needs);
    snprintf(buf, cap, "%d|%.400s|%s", r->status, r->message ? r->message : "", needs);
}

static void case_rig(void) {
    static const char* const names[] = { "rdk", "stroop", "afc2", "oddball", "visuomotor", "bimanual", "sound" };
    const yrig_profile* rigs[3] = { &rig_dev, &rig_room, &rig_changed };
    pl_player* p = (pl_player*)calloc(1, sizeof *p);
    pl_player* q = (pl_player*)calloc(1, sizeof *q);
    pl_sim_desc d;
    pl_check_desc cd;
    pl_check_result r, rc;
    run_opts o;
    char out[1200], a[2048], b[2048], cell[256];
    size_t i;
    int j, c, n_same = 0;
    if (rig_profiles() != 0) { CHECK(0, "rig: the test profiles"); free(p); free(q); return; }
    memset(&o, 0, sizeof o);
    o.seed = 3;
    /* (a) a session's needs and refusals are the check's with the same
     * profile and calibration; a few by value */
    for (i = 0; i < sizeof names / sizeof names[0]; i++) {
        const pk* kk = pack_of(names[i]);
        if (!kk) continue;
        for (j = 0; j < 3; j++)
            for (c = 0; c < 2; c++) {
                char dir[200];
                snprintf(dir, sizeof dir, "rigneeds_%s_%d_%d", names[i], j, c);
                path_in(out, sizeof out, dir);
                sim_desc(&d, kk, &o, out);
                d.n_fields = 0;
                d.rig = rigs[j];
                d.cal = c ? &nominal : NULL;
                memset(&cd, 0, sizeof cd);
                cd.pack.data = kk->data;
                cd.pack.size = kk->size;
                cd.seed = 3;
                cd.rig = rigs[j];
                cd.cal = d.cal;
                (void)pl_sim_load(p, &d, &r);
                (void)pl_check(q, &cd, &rc);
                result_text(&r, a, sizeof a);
                result_text(&rc, b, sizeof b);
                CHECK(strcmp(a, b) == 0, "rig: %s with profile %d, cal %d: the session's '%s', the check's '%s'", names[i], j, c, a, b);
                n_same += strcmp(a, b) == 0;
                if (c == 1 && ((j == 0 && !strcmp(names[i], "oddball")) || (j == 1 && (!strcmp(names[i], "afc2") || !strcmp(names[i], "visuomotor") ||
                                                                                   !strcmp(names[i], "bimanual"))) ||
                               (j == 2 && !strcmp(names[i], "afc2")))) {
                    static const struct { const char* name; int rig; const char* want; } W[] = {
                        { "oddball", 0, "1:participant:0 4::1" }, { "afc2", 1, "1:participant:0 4::2" }, { "visuomotor", 1, "1:participant:0" },
                        { "bimanual", 1, "1:participant:0" }, { "afc2", 2, "1:participant:0 2::0 4::2" },
                    };
                    size_t w;
                    needs_text(&r, a, sizeof a);
                    for (w = 0; w < sizeof W / sizeof W[0]; w++)
                        if (W[w].rig == j && !strcmp(W[w].name, names[i]))
                            CHECK(r.status == PL_CHECK_OK && strcmp(a, W[w].want) == 0, "rig: %s with profile %d: status %d, needs '%s', want '%s'", names[i], j,
                                  r.status, a, W[w].want);
                }
                if (c == 0 && j == 1 && !strcmp(names[i], "afc2")) {
                    needs_text(&r, a, sizeof a);
                    CHECK(strcmp(a, "1:participant:0 2::0 4::2") == 0, "rig: afc2 with room and no calibration: needs '%s'", a);
                }
                pl_close(p);
                pl_close(q);
            }
    }
    /* (b) the profile's adapters: the session equals --sim --rig with no
     * --sim-gain, sample for sample (hand 2 and -1.5 px a count; left and
     * right 16 px a count, 0.5 cm through the view) */
    {
        const pk* k = pack_of("visuomotor");
        if (k) {
            char sp[1200];
            memset(&o, 0, sizeof o);
            o.seed = 3;
            o.def = edit_all(edit_all(def_of(k), "\"reps 5\"", "\"reps 1\""), "\"reps 10\"", "\"reps 1\"");
            path_in(sp, sizeof sp, "visuomotor_rig_samples.csv");
            o.samples = sp;
            o.rig_file = room_path;
            o.step_rig = &rig_room;
            if (o.def && write_samples_vm(sp, 120) == 0) compare_pack("visuomotor_rig", k, &o);
            else CHECK(0, "rig: visuomotor: no definition or samples");
            free((void*)o.def);
        }
        k = pack_of("bimanual");
        if (k) {
            char sp[1200];
            memset(&o, 0, sizeof o);
            o.seed = 3;
            o.def = edit_all(edit_all(edit_all(def_of(k), "\"reps 30\"", "\"reps 2\""), "\"wait_s\": 46", "\"wait_s\": 3"), ARRIVED, ARRIVED_OR(2));
            path_in(sp, sizeof sp, "bimanual_rig_samples.csv");
            o.samples = sp;
            o.rig_file = room_path;
            o.step_rig = &rig_room;
            if (o.def && write_samples_bi(sp, 120) == 0) compare_pack("bimanual_rig", k, &o);
            else CHECK(0, "rig: bimanual: no definition or samples");
            free((void*)o.def);
        }
    }
    /* (c) what a sample is: a relative role's counts, at the profile's
     * 2 px a count; with no profile, 1 px a count */
    {
        static const char reach[] =
            "{\"format\": \"ysp-experiment\", \"version\": 2, \"name\": \"reach\","
            " \"roles\": {\"hand\": {\"type\": \"position2d_relative\", \"units\": \"display_px\"}},"
            " \"stimuli\": {\"dot\": {\"kind\": \"shape\", \"w\": 10, \"h\": 10}},"
            " \"trials\": {\"t\": {\"controls\": {\"out\": {\"control\": \"region\", \"of\": \"hand\", \"radius\": 100, \"outside\": true}},"
            "   \"states\": [{\"name\": \"hold\", \"steps\": [{\"on\": \"dot\"}, {\"wait_s\": 0.2}]},"
            "                {\"name\": \"reach\", \"exits\": [{\"on\": \"out\", \"to\": \"end\", \"outcome\": \"out\"}, {\"after_s\": 2, \"to\": \"end\", \"outcome\": \"late\"}]}],"
            "   \"columns\": {\"out_x\": {\"hit\": \"out.x\"}}}},"
            " \"timeline\": [{\"name\": \"l\", \"trial\": \"t\", \"rules\": [\"reps 1\"]}]}";
        const pk* k = pack_of("rdk");
        int v, f;
        for (v = 0; v < 2; v++) {
            pl_sim_where w;
            memset(&o, 0, sizeof o);
            o.seed = 3;
            o.def = reach;
            path_in(out, sizeof out, v ? "rig_reach_none" : "rig_reach");
            sim_desc(&d, k, &o, out);
            d.rig = v ? NULL : &rig_room;
            d.cal = v ? NULL : &nominal;
            CHECK(pl_sim_load(p, &d, &r) == PL_CHECK_OK, "rig: reach %d: load: %s", v, r.message);
            if (r.status != PL_CHECK_OK) { pl_close(p); continue; }
            for (f = 0; f < 20; f++) CHECK(sim_frame(p, false) == 0, "rig: reach %d: frame %d", v, f);   /* into reach */
            CHECK(pl_sim_sample(p, pl_sim_next_onset(p) - 1, 0, 30.0f, 0.0f) == 0, "rig: reach %d: a sample refused", v);
            CHECK(sim_frame(p, false) == 0, "rig: reach %d: a frame", v);
            CHECK(pl_sim_sample(p, pl_sim_next_onset(p) - 1, 0, 30.0f, 0.0f) == 0, "rig: reach %d: a sample refused", v);
            CHECK(step_trial_end(p, 1) == 0, "rig: reach %d: the trial did not end", v);
            pl_sim_where_now(p, &w);
            pl_close(p);
            cell_of(out, 0, "outcome", cell, sizeof cell);
            CHECK(strcmp(cell, v ? "late" : "out") == 0, "rig: reach %d: outcome '%s', want '%s' (60 counts at %s px a count)", v, cell, v ? "late" : "out", v ? "1" : "2");
            if (!v) {
                cell_of(out, 0, "out_x", cell, sizeof cell);
                CHECK(strtod(cell, NULL) == 120.0, "rig: reach: out_x '%s', want 120 (60 counts at 2 px)", cell);
            }
        }
    }
    free(p);
    free(q);
    printf("  rig: %d sessions' needs and refusals equal the check's with one profile; the profile's adapters equal --sim --rig's; a count is 2 px with the profile, 1 without\n",
           n_same);
}

/* on_code: each code the session sends, as events.csv's out rows */
typedef struct code_log { int n; char rows[512][96]; } code_log;
static void on_code(void* ctx, const char* role, uint32_t code, int64_t t_ns) {
    code_log* l = (code_log*)ctx;
    if (l->n < 512) snprintf(l->rows[l->n], 96, "%lld.%09lld,%s,%u", (long long)(t_ns / 1000000000), (long long)(t_ns % 1000000000), role, (unsigned)code);
    l->n++;
}
static int cmp_rows(const void* x, const void* y) { return strcmp((const char*)x, (const char*)y); }

static void case_codes(void) {
    const pk* k = pack_of("oddball");
    pl_player* p = (pl_player*)calloc(1, sizeof *p);
    static code_log got, want;
    pl_sim_desc d;
    pl_check_result r;
    run_opts o;
    char out[1200], path[1300];
    void* x = NULL;
    size_t n;
    int f = 0, i, same;
    memset(&o, 0, sizeof o);
    got.n = want.n = 0;
    o.seed = 3;
    o.rig = true;
    path_in(out, sizeof out, "codes");
    sim_desc(&d, k, &o, out);
    d.on_code = on_code;
    d.ctx = &got;
    CHECK(pl_sim_load(p, &d, &r) == PL_CHECK_OK, "codes: load: %s", r.message);
    while (r.status == PL_CHECK_OK && (f = sim_frame(p, false)) == 0) { }
    pl_close(p);
    free(p);
    snprintf(path, sizeof path, "%s/events.csv", out);
    if (read_file(path, &x, &n) == 0) {
        char* line = (char*)x;
        while ((line = strchr(line, '\n')) != NULL && *++line) {
            char t[40], kind[16], role[32];
            int code = 0;
            /* t_s,trial_index,kind,role,stimulus,frame,planned_s,residual_s,code */
            if (sscanf(line, "%39[^,],%*[^,],%15[^,],%31[^,],", t, kind, role) == 3 && strcmp(kind, "out") == 0) {
                const char* c = line;
                for (i = 0; i < 8 && c; i++) { c = strchr(c, ','); if (c) c++; }
                if (c) code = atoi(c);
                if (want.n < 512) snprintf(want.rows[want.n], 96, "%s,%s,%d", t, role, code);
                want.n++;
            }
        }
        free(x);
    }
    qsort(got.rows, (size_t)(got.n < 512 ? got.n : 512), sizeof got.rows[0], cmp_rows);
    qsort(want.rows, (size_t)(want.n < 512 ? want.n : 512), sizeof want.rows[0], cmp_rows);
    same = got.n == want.n;
    for (i = 0; same && i < got.n && i < 512; i++) same = strcmp(got.rows[i], want.rows[i]) == 0;
    CHECK(f == 1 && want.n > 0 && same, "codes: on_code gave %d codes, events.csv has %d out rows (first: '%s' and '%s')", got.n, want.n, got.n ? got.rows[0] : "",
          want.n ? want.rows[0] : "");
    printf("  codes: on_code gave the %d codes of events.csv's out rows, at their times\n", got.n);
}

/* --- the packs and the rig ---------------------------------------------------------- */

/* tests/player/make_rig.c's profile: 570 mm, 1920 px over 518.4 mm, the
 * nominal calibration; the built-in rig's stand-ins are the same */
static int make_rig(const char* dir) {
    static const float srgb_xy[4][2] = { { 0.64f, 0.33f }, { 0.30f, 0.60f }, { 0.15f, 0.06f }, { 0.3127f, 0.3290f } };
    static ycol_cal cal;
    static yrig_profile prof;
    static unsigned char bytes[sizeof(ycol_cal)];
    char path[1300], err[300];
    FILE* fp;
    int n, i;
    if (ycol_cal_nominal(&cal, srgb_xy, 100.0f, 2.2) < 0) return -1;
    n = ycol_cal_save(&cal, bytes, sizeof bytes);
    if (n < 0) return -1;
    snprintf(path, sizeof path, "%s/display.yspcal", dir);
    if (!(fp = fopen(path, "wb"))) return -1;
    fwrite(bytes, 1, (size_t)n, fp);
    fclose(fp);
    yrig_init(&prof, "player-test");
    prof.view.set = true;
    prof.view.distance_mm = 570.0;
    prof.view.width_mm = 0.27 * 1920.0;
    prof.view.width_px = 1920;
    prof.display.set = true;
    snprintf(prof.display.calibration, sizeof prof.display.calibration, "display.yspcal");
    {
        static const char H[] = "0123456789abcdef";
        uint8_t dg[32];
        yrig_sha256(bytes, (size_t)n, dg);
        for (i = 0; i < 32; i++) { prof.display.calibration_sha256[i * 2] = H[dg[i] >> 4]; prof.display.calibration_sha256[i * 2 + 1] = H[dg[i] & 15]; }
        prof.display.calibration_sha256[64] = 0;
    }
    snprintf(rig_path, sizeof rig_path, "%s/profile.json", dir);
    return yrig_save_file(&prof, rig_path, err, sizeof err) == YRIG_OK ? 0 : -1;
}

#if defined(PL_SIM_MUT) && !defined(_WIN32)
/* The mutation runner: build the packs with $YPAK from $YPL_REPO, as
 * check_test.c does. */
static int build_packs(const char* dir) {
    static const char* const names[] = { "rdk", "afc2", "oddball", "visuomotor", "bimanual", "stroop", "sound" };
    const char* ypak = getenv("YPAK");
    const char* repo = getenv("YPL_REPO") ? getenv("YPL_REPO") : ".";
    const char* font = getenv("YPL_FONT") ? getenv("YPL_FONT") : "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf";
    static char cmd[16384];
    size_t i;
    if (!ypak) return -1;
    for (i = 0; i < sizeof names / sizeof names[0]; i++) {
        pk* k = &packs[n_packs];
        snprintf(k->path, sizeof k->path, "%s/%s.ysppak", dir, names[i]);
        if (strcmp(names[i], "sound") == 0) {
            /* the rdk example with a sound first (sim_test.py's) */
            snprintf(cmd, sizeof cmd,
                     "cp -r '%s/tests/player/rdk' '%s/sound_src' && cd '%s/sound_src' && python3 -c \"import json,struct;"
                     "s=json.load(open('pack.json'));s['audio']={'rate':48000,'channels':2};"
                     "s['resources'].append({'name':'tone.wav','kind':'audio','source':'tone.wav'});json.dump(s,open('pack.json','w'));"
                     "d=json.load(open('experiment.json'));d['stimuli']['tone']={'kind':'sound','audio':'tone.wav'};"
                     "d['trials']['rdk']['events'].insert(0,{'on':'tone'});json.dump(d,open('experiment.json','w'));"
                     "b=bytes(4800*4);open('tone.wav','wb').write(b'RIFF'+struct.pack('<I',36+len(b))+b'WAVEfmt '+struct.pack('<IHHIIHH',16,1,2,48000,192000,4,16)+b'data'+struct.pack('<I',len(b))+b)\""
                     " && '%s' build pack.json -o '%s' >/dev/null",
                     repo, dir, dir, ypak, k->path);
        } else if (strcmp(names[i], "stroop") == 0) {
            if (access(font, R_OK) != 0) continue;
            snprintf(cmd, sizeof cmd, "cp -r '%s/tests/player/stroop' '%s/stroop_src' && mkdir -p '%s/stroop_src/fonts' && cp '%s' '%s/stroop_src/fonts/NotoSans-Regular.ttf' && "
                     "cd '%s/stroop_src' && '%s' build pack.json -o '%s' >/dev/null",
                     repo, dir, dir, font, dir, dir, ypak, k->path);
        } else {
            snprintf(cmd, sizeof cmd, "cd '%s/tests/player/%s' && '%s' build pack.json -o '%s' >/dev/null", repo, names[i], ypak, k->path);
        }
        if (system(cmd) != 0) { printf("ypak failed: %s\n", cmd); return -1; }
        k->name = names[i];
        if (read_file(k->path, &k->data, &k->size) != 0) return -1;
        n_packs++;
        if (strcmp(names[i], "stroop") == 0) {
            /* the align case's packs (sim_test.py's) */
            static const char* const extra[2] = { "stroop_start", "stroop_old" };
            int j;
            snprintf(cmd, sizeof cmd, "python3 '%s/tests/player/align_packs.py' '%s' '%s/stroop_src' '%s' '%s' >/dev/null", repo, ypak, dir, k->path, dir);
            if (system(cmd) != 0) { printf("align_packs.py failed: %s\n", cmd); return -1; }
            for (j = 0; j < 2; j++) {
                pk* x = &packs[n_packs];
                snprintf(x->path, sizeof x->path, "%s/%s.ysppak", dir, extra[j]);
                x->name = extra[j];
                if (read_file(x->path, &x->data, &x->size) != 0) return -1;
                n_packs++;
            }
        }
    }
    return 0;
}
#endif

/* --- (10) a GL context: the deferred gfx against a direct one ------------------- */

#if defined(PL_SIM_GL)
/* A stepped session's gfx on a real GL ES 3.0 context (Mesa's llvmpipe,
 * EGL's surfaceless platform, no window): loaded on one thread with
 * desc.gl_caps and desc.share, attached and drawn on the context's thread,
 * read through pl_sim_texture()'s name. Its frames must equal those of the
 * same session whose gfx opened directly on the context, which the wrap of
 * pl_gfx_open_null() below gives: the player has no direct GL path of its
 * own for a stepped session. No EGL, no context: the case is skipped, as
 * test_gfx skips. */
#include <dlfcn.h>

typedef yscr_proc (*gl_gpa_fn)(const char*);
static struct {
    void* lib;
    gl_gpa_fn gpa;
    void *dpy, *ctx, *surf;
    unsigned (*make_current)(void*, void*, void*, void*);
    unsigned (*destroy)(void*, void*);
    unsigned (*destroy_ctx)(void*, void*);
    void (*gen_fb)(int32_t, uint32_t*);
    void (*bind_fb)(uint32_t, uint32_t);
    void (*fb_tex)(uint32_t, uint32_t, uint32_t, uint32_t, int32_t);
    uint32_t (*fb_status)(uint32_t);
    void (*read)(int32_t, int32_t, int32_t, int32_t, uint32_t, uint32_t, void*);
    void (*del_fb)(int32_t, const uint32_t*);
    void (*use_program)(uint32_t);
    void (*bind_vao)(uint32_t);
    void (*active_texture)(uint32_t);
    void (*bind_texture)(uint32_t, uint32_t);
    void (*viewport)(int32_t, int32_t, int32_t, int32_t);
    uint32_t fb;
    char renderer[160];
} gl;
static int gl_direct;   /* the wrap opens a direct GL gfx: the reference session */

static yscr_proc gl_proc(void* ctx, const char* name) {
    yscr_proc p = NULL;
    void* o;
    (void)ctx;
    if (gl.gpa) p = gl.gpa(name);
    if (!p && (o = dlsym(gl.lib, name)) != NULL) memcpy(&p, &o, sizeof p);
    return p;
}
static void* gl_sym(const char* name) {
    yscr_proc p = gl_proc(NULL, name);
    void* o;
    memcpy(&o, &p, sizeof o);
    return o;
}
#define GL_LOAD(dst, name) do { void* o_ = gl_sym(name); memcpy(&(dst), &o_, sizeof o_); } while (0)

static const char* gl_open(void) {
    void* (*gpd)(unsigned, void*, const int32_t*);
    unsigned (*init)(void*, int32_t*, int32_t*);
    unsigned (*bind_api)(unsigned);
    unsigned (*choose)(void*, const int32_t*, void**, int32_t, int32_t*);
    void* (*mkctx)(void*, void*, void*, const int32_t*);
    void* (*mkpb)(void*, void*, const int32_t*);
    const unsigned char* (*str)(uint32_t);
    void* cfg = NULL;
    void* o;
    int32_t a[13], n = 0, mj, mn;
    setenv("LIBGL_ALWAYS_SOFTWARE", "1", 0);   /* llvmpipe: no GPU, the same on every machine */
    if (!(gl.lib = dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL))) return "no libEGL.so.1";
    o = dlsym(gl.lib, "eglGetProcAddress");
    memcpy(&gl.gpa, &o, sizeof o);
    GL_LOAD(gpd, "eglGetPlatformDisplayEXT");
    GL_LOAD(init, "eglInitialize");
    GL_LOAD(bind_api, "eglBindAPI");
    GL_LOAD(choose, "eglChooseConfig");
    GL_LOAD(mkctx, "eglCreateContext");
    GL_LOAD(mkpb, "eglCreatePbufferSurface");
    GL_LOAD(gl.make_current, "eglMakeCurrent");
    GL_LOAD(gl.destroy, "eglDestroySurface");
    GL_LOAD(gl.destroy_ctx, "eglDestroyContext");
    if (!gpd || !init || !bind_api || !choose || !mkctx || !mkpb || !gl.make_current) return "EGL lacks EGL_EXT_platform_base";
    gl.dpy = gpd(0x31DD, NULL, NULL);   /* EGL_PLATFORM_SURFACELESS_MESA */
    if (!gl.dpy || !init(gl.dpy, &mj, &mn)) { gl.dpy = NULL; return "no surfaceless EGL display"; }
    bind_api(0x30A0);   /* EGL_OPENGL_ES_API */
    a[0] = 0x3024; a[1] = 8; a[2] = 0x3023; a[3] = 8; a[4] = 0x3022; a[5] = 8; a[6] = 0x3021; a[7] = 8;
    a[8] = 0x3033; a[9] = 0x0001; a[10] = 0x3040; a[11] = 0x0040; a[12] = 0x3038;   /* PBUFFER, ES3 */
    if (!choose(gl.dpy, a, &cfg, 1, &n) || n < 1) return "no RGBA8 ES 3.0 pbuffer config";
    a[0] = 0x3098; a[1] = 3; a[2] = 0x30FB; a[3] = 0; a[4] = 0x3038;
    gl.ctx = mkctx(gl.dpy, cfg, NULL, a);
    a[0] = 0x3057; a[1] = 16; a[2] = 0x3056; a[3] = 16; a[4] = 0x3038;
    gl.surf = gl.ctx ? mkpb(gl.dpy, cfg, a) : NULL;
    if (!gl.surf || !gl.make_current(gl.dpy, gl.surf, gl.surf, gl.ctx)) return "no ES 3.0 context";
    GL_LOAD(gl.gen_fb, "glGenFramebuffers");
    GL_LOAD(gl.bind_fb, "glBindFramebuffer");
    GL_LOAD(gl.fb_tex, "glFramebufferTexture2D");
    GL_LOAD(gl.fb_status, "glCheckFramebufferStatus");
    GL_LOAD(gl.read, "glReadPixels");
    GL_LOAD(gl.del_fb, "glDeleteFramebuffers");
    GL_LOAD(gl.use_program, "glUseProgram");
    GL_LOAD(gl.bind_vao, "glBindVertexArray");
    GL_LOAD(gl.active_texture, "glActiveTexture");
    GL_LOAD(gl.bind_texture, "glBindTexture");
    GL_LOAD(gl.viewport, "glViewport");
    GL_LOAD(str, "glGetString");
    if (!gl.gen_fb || !gl.bind_fb || !gl.fb_tex || !gl.fb_status || !gl.read || !gl.del_fb || !gl.use_program || !gl.bind_vao ||
        !gl.active_texture || !gl.bind_texture || !gl.viewport || !str)
        return "no GL ES 3.0 entry points";
    snprintf(gl.renderer, sizeof gl.renderer, "%s", (const char*)str(0x1F01));
    gl.gen_fb(1, &gl.fb);
    return NULL;
}

static void gl_close(void) {
    if (gl.fb) gl.del_fb(1, &gl.fb);
    gl.fb = 0;
    if (gl.dpy) {
        gl.make_current(gl.dpy, NULL, NULL, NULL);
        if (gl.surf) gl.destroy(gl.dpy, gl.surf);
        if (gl.ctx) gl.destroy_ctx(gl.dpy, gl.ctx);
    }
    /* No eglTerminate() and no dlclose(): Mesa's terminate unloads llvmpipe,
     * and LeakSanitizer then reports 112 bytes in 2 blocks from that unknown
     * module, which no suppression can name (gfx.md, "Sanitizers"). The
     * display stays until the process ends. */
    memset(&gl, 0, sizeof gl);
}

/* The texture's texels with our own GL calls, as the designer's ImGui
 * samples them; rows turned to top first, as ygfx_read_output() gives
 * them. Then a host's state, as ImGui's draw may leave it: the session's
 * next drawn frame must forget gfx's cached state (ygfx_reset_state()). */
static int gl_read_texture(uint32_t name, int w, int h, uint8_t* rgba, uint8_t* row) {
    int y;
    const size_t stride = (size_t)w * 4;
    gl.bind_fb(0x8D40, gl.fb);   /* GL_FRAMEBUFFER */
    gl.fb_tex(0x8D40, 0x8CE0, 0x0DE1, name, 0);   /* COLOR_ATTACHMENT0, TEXTURE_2D */
    if (gl.fb_status(0x8D40) != 0x8CD5) return -1;   /* FRAMEBUFFER_COMPLETE */
    gl.read(0, 0, w, h, 0x1908, 0x1401, rgba);   /* RGBA, UNSIGNED_BYTE */
    for (y = 0; y < h / 2; y++) {
        memcpy(row, rgba + (size_t)y * stride, stride);
        memcpy(rgba + (size_t)y * stride, rgba + (size_t)(h - 1 - y) * stride, stride);
        memcpy(rgba + (size_t)(h - 1 - y) * stride, row, stride);
    }
    gl.use_program(0);
    gl.bind_vao(0);
    gl.active_texture(0x84C0);   /* GL_TEXTURE0 */
    gl.bind_texture(0x0DE1, name);
    gl.viewport(0, 0, 16, 16);
    return 0;
}

/* the reference session: the null gfx's open becomes a direct one on the
 * current context, with the output texture, at the session's size */
bool __real_pl_gfx_open_null(ygfx_gfx* g, ygfx_desc* d);
bool __wrap_pl_gfx_open_null(ygfx_gfx* g, ygfx_desc* d);
bool __wrap_pl_gfx_open_null(ygfx_gfx* g, ygfx_desc* d) {
    if (!gl_direct) return __real_pl_gfx_open_null(g, d);
    d->screen = NULL;
    d->backend = NULL;
    d->backend_ctx = NULL;
    d->gl_proc = gl_proc;
    d->gl_ctx = NULL;
    d->output_texture = true;
    return ygfx_open(g, d);
}

static uint64_t fnv(const uint8_t* b, size_t n) {
    uint64_t h = 1469598103934665603ull;
    size_t i;
    for (i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}

enum { GL_FRAMES = 720, GL_W = 800, GL_H = 600 };
typedef struct gl_load { pl_player* p; pl_sim_desc d; int ok; char err[300]; } gl_load;

static TH_RET gl_thread_load(void* arg) {
    gl_load* t = (gl_load*)arg;
    pl_check_result r;
    t->ok = pl_sim_load(t->p, &t->d, &r) == PL_CHECK_OK;
    if (!t->ok) snprintf(t->err, sizeof t->err, "%s", r.message);
    return TH_OK;
}

/* One session for GL_FRAMES frames, each other one drawn and read; its
 * hashes in h[]. deferred: the load on another thread with gl_caps and
 * share, the attach budget_ns a call; else the direct gfx on this thread. */
static int gl_session(const pk* k, const run_opts* o, const char* out, bool deferred, const ygfx_gl_caps* caps, ygfx_gfx* share,
                      int64_t budget_ns, uint64_t* h, int* n_h, int* attach_calls, ygfx_programs* st_attach, ygfx_programs* st_end,
                      long* lit) {
    gl_load t;
    th_t th;
    ygfx_native nat;
    uint8_t* rgba = (uint8_t*)malloc((size_t)GL_W * GL_H * 4);
    uint8_t* row = (uint8_t*)malloc((size_t)GL_W * 4);
    int rc = 0, f, a;
    memset(&t, 0, sizeof t);
    t.p = (pl_player*)calloc(1, sizeof *t.p);
    sim_desc(&t.d, k, o, out);
    *n_h = 0;
    *attach_calls = 0;
    *lit = 0;
    if (deferred) {
        t.d.gl_caps = caps;
        t.d.gl_proc = gl_proc;
        t.d.share = share;
        if (th_start(&th, gl_thread_load, &t) != 0) { printf("  gl: no thread\n"); rc = -1; }
        else th_join(th);
    } else {
        gl_direct = 1;
        gl_thread_load(&t);
        gl_direct = 0;
    }
    if (rc == 0 && !t.ok) { printf("  gl %s: load: %s\n", k->name, t.err); rc = -1; }
    if (rc == 0 && deferred) {
        CHECK(!pl_sim_texture(t.p, &nat), "gl %s: a texture before the attach", k->name);
        CHECK(pl_sim_frame(t.p, true) < 0 && strstr(t.p->error, "before pl_sim_attach"), "gl %s: a drawn frame before the attach was not refused", k->name);
        /* the refusal fails the session: load it again */
        pl_close(t.p);
        memset(t.p, 0, sizeof *t.p);
        rm_tree(out);
        if (th_start(&th, gl_thread_load, &t) != 0) { printf("  gl: no thread\n"); rc = -1; }
        else th_join(th);
        if (rc == 0 && !t.ok) { printf("  gl %s: load: %s\n", k->name, t.err); rc = -1; }
    }
    while (rc == 0 && (a = pl_sim_attach(t.p, budget_ns)) != 1) {
        (*attach_calls)++;
        if (a < 0 || *attach_calls > 10000) { printf("  gl %s: attach: %s\n", k->name, t.p->error); rc = -1; }
    }
    if (rc == 0) {
        (*attach_calls)++;
        ygfx_program_stats(&t.p->gfx, st_attach);
    }
    for (f = 0; rc == 0 && f < GL_FRAMES; f++) {
        bool draw = f % 2 == 0;
        int r = sim_frame(t.p, draw);   /* counted for (2): --wrap reaches the player and gfx, not Mesa */
        if (r < 0) { printf("  gl %s: frame %d: %s\n", k->name, f, t.p->error); rc = -1; break; }
        if (draw) {
            size_t i;
            if (deferred) {
                if (!pl_sim_texture(t.p, &nat) || nat.gl_format != 0x8058 || gl_read_texture(nat.gl_name, GL_W, GL_H, rgba, row) != 0) {
                    printf("  gl %s: frame %d: no GL_RGBA8 texture to read (name %u, format 0x%x)\n", k->name, f, nat.gl_name, nat.gl_format);
                    rc = -1;
                    break;
                }
            } else if (ygfx_read_output(&t.p->gfx, 0, 0, GL_W, GL_H, rgba) != 0) {
                printf("  gl %s: frame %d: read: %s\n", k->name, f, ygfx_error(&t.p->gfx));
                rc = -1;
                break;
            }
            h[(*n_h)++] = fnv(rgba, (size_t)GL_W * GL_H * 4);
            /* the pixels unlike the top-left one, the background's */
            for (i = 1; i < (size_t)GL_W * GL_H; i++)
                if (memcmp(rgba + i * 4, rgba, 4) != 0) (*lit)++;
        }
        if (r == 1) break;
    }
    if (rc == 0) ygfx_program_stats(&t.p->gfx, st_end);
    pl_sim_stop(t.p);
    pl_sim_detach(t.p);   /* the GL thread: the session's GL objects go */
    pl_close(t.p);
    free(t.p);
    free(rgba);
    free(row);
    return rc;
}

static void case_gl(void) {
    static uint64_t h_ref[GL_FRAMES], h_def[GL_FRAMES];
    static const char* const names[2] = { "stroop", "oddball" };
    const char* why = gl_open();
    ygfx_gfx first;
    ygfx_desc fd;
    ygfx_gl_caps caps;
    int i;
    if (why) {
        printf("  gl: skipped: %s\n", why);
        gl_close();
        return;
    }
    /* the designer's first gfx on the window's context (main.cpp): the caps
     * and the shared programs */
    memset(&first, 0, sizeof first);
    memset(&fd, 0, sizeof fd);
    fd.gl_proc = gl_proc;
    fd.width = 64;
    fd.height = 64;
    if (!ygfx_open(&first, &fd) || ygfx_gl_caps_get(&first, &caps) != YGFX_OK) {
        CHECK(0, "gl: the first gfx on %s: %s", gl.renderer, ygfx_error(&first));
        ygfx_close(&first);
        gl_close();
        return;
    }
    for (i = 0; i < 2; i++) {
        const pk* k = pack_of(names[i]);
        char ref[1200], got[1200], lab[64];
        run_opts o;
        int n_ref = 0, n_def = 0, calls_ref = 0, calls_def = 0, j, n_same = 0, n_distinct = 1;
        ygfx_programs pa_ref, pe_ref, pa_def, pe_def;
        long lit_ref = 0, lit_def = 0;
        if (!k) { printf("  gl %s: no pack, skipped\n", names[i]); continue; }
        memset(&o, 0, sizeof o);
        o.seed = 11;
        o.rig = i == 1;   /* oddball through the nominal calibration's output stage */
        memset(&pa_ref, 0, sizeof pa_ref); memset(&pe_ref, 0, sizeof pe_ref);
        memset(&pa_def, 0, sizeof pa_def); memset(&pe_def, 0, sizeof pe_def);
        snprintf(lab, sizeof lab, "gl_%s_direct", names[i]);
        path_in(ref, sizeof ref, lab);
        snprintf(lab, sizeof lab, "gl_%s_deferred", names[i]);
        path_in(got, sizeof got, lab);
        if (gl_session(k, &o, ref, false, NULL, NULL, 0, h_ref, &n_ref, &calls_ref, &pa_ref, &pe_ref, &lit_ref) != 0 ||
            /* stroop one step a call (budget 1 ns), oddball all in one call (0) */
            gl_session(k, &o, got, true, &caps, &first, i == 0 ? 1 : 0, h_def, &n_def, &calls_def, &pa_def, &pe_def, &lit_def) != 0) {
            CHECK(0, "gl %s: a session failed", names[i]);
            continue;
        }
        for (j = 0; j < n_ref && j < n_def; j++) {
            if (h_ref[j] == h_def[j]) n_same++;
            if (j > 0 && h_ref[j] != h_ref[j - 1]) n_distinct++;
        }
        CHECK(n_ref == n_def && n_same == n_ref, "gl %s: %d of %d drawn frames equal the direct gfx's (%d drawn deferred)", names[i], n_same, n_ref, n_def);
        CHECK(n_distinct > 2 && lit_ref > 0, "gl %s: the frames show nothing to compare (%d changes, %ld pixels not the background)", names[i], n_distinct, lit_ref);
        CHECK(pa_def.shared > 0, "gl %s: the deferred gfx took no program from desc.share", names[i]);
        CHECK(pe_def.compiled + pe_def.loaded + pe_def.shared == pa_def.compiled + pa_def.loaded + pa_def.shared,
              "gl %s: the frames made programs after the attach (%u then %u): the text programs were not made at the load", names[i],
              pa_def.compiled + pa_def.loaded + pa_def.shared, pe_def.compiled + pe_def.loaded + pe_def.shared);
        CHECK(i != 0 || calls_def > 1, "gl %s: a budget of 1 ns attached in %d call", names[i], calls_def);
        CHECK(i != 1 || calls_def == 1, "gl %s: a budget of 0 (all) took %d calls", names[i], calls_def);
        CHECK(same_data(ref, got, lab), "gl %s: the data sets differ", names[i]);
        printf("  gl %s (%s): %d of %d drawn frames equal (%d changes, %ld pixels not the background); attach %d calls; programs: %u shared, %u compiled\n", names[i],
               gl.renderer, n_same, n_ref, n_distinct, lit_ref, calls_def, pa_def.shared, pa_def.compiled);
    }
    ygfx_close(&first);
    gl_close();
}

/* --- (13) a text's align: the ink on a GL context, an old pack ------------------- */

enum { AL_W = 1200, AL_H = 400, AL_FRAMES = 300, AL_WORD = 130 };

/* One session on the direct gfx at AL_W x AL_H for AL_FRAMES frames, every
 * other one drawn and hashed into h; at frame AL_WORD (the first trial's
 * word on, nothing else) the ink's columns, the pixels unlike the
 * top-left one, in *x0 and *x1. *writer gets the player's def_writer. */
static int al_session(const pk* k, const char* out, uint64_t* h, int* n_h, int* x0, int* x1, char* writer, size_t wcap) {
    gl_load t;
    uint8_t* rgba = (uint8_t*)malloc((size_t)AL_W * AL_H * 4);
    run_opts o;
    int rc = 0, f, a, calls = 0;
    memset(&t, 0, sizeof t);
    memset(&o, 0, sizeof o);
    o.seed = 11;
    t.p = (pl_player*)calloc(1, sizeof *t.p);
    sim_desc(&t.d, k, &o, out);
    t.d.width = AL_W;
    t.d.height = AL_H;
    *n_h = 0;
    *x0 = AL_W;
    *x1 = -1;
    gl_direct = 1;
    gl_thread_load(&t);
    gl_direct = 0;
    if (!t.ok) { printf("  align %s: load: %s\n", k->name, t.err); rc = -1; }
    if (rc == 0) snprintf(writer, wcap, "%s", t.p->def_writer);
    while (rc == 0 && (a = pl_sim_attach(t.p, 0)) != 1)
        if (a < 0 || ++calls > 10000) { printf("  align %s: attach: %s\n", k->name, t.p->error); rc = -1; }
    for (f = 0; rc == 0 && f < AL_FRAMES; f++) {
        bool draw = f % 2 == 0;
        int r = pl_sim_frame(t.p, draw);
        if (r < 0) { printf("  align %s: frame %d: %s\n", k->name, f, t.p->error); rc = -1; break; }
        if (draw) {
            if (!rgba || ygfx_read_output(&t.p->gfx, 0, 0, AL_W, AL_H, rgba) != 0) { printf("  align %s: read: %s\n", k->name, ygfx_error(&t.p->gfx)); rc = -1; break; }
            h[(*n_h)++] = fnv(rgba, (size_t)AL_W * AL_H * 4);
            if (f == AL_WORD) {
                size_t i;
                for (i = 1; i < (size_t)AL_W * AL_H; i++)
                    if (memcmp(rgba + i * 4, rgba, 4) != 0) {
                        int x = (int)(i % AL_W);
                        if (x < *x0) *x0 = x;
                        if (x > *x1) *x1 = x;
                    }
            }
        }
        if (r == 1) break;
    }
    pl_sim_stop(t.p);
    pl_sim_detach(t.p);
    pl_close(t.p);
    free(t.p);
    free(rgba);
    return rc;
}

static void align_ink(void) {
    static uint64_t h[3][AL_FRAMES];
    static const char* const names[3] = { "stroop", "stroop_start", "stroop_old" };
    int n_h[3] = { 0, 0, 0 }, x0[3] = { 0, 0, 0 }, x1[3] = { 0, 0, 0 }, i, j, same = 0;
    char writer[3][32];
    char dir[1200], lab[64];
    const char* why = gl_open();
    if (why) {
        printf("  align: ink skipped: %s\n", why);
        gl_close();
        return;
    }
    for (i = 0; i < 3; i++) {
        snprintf(lab, sizeof lab, "align_%s", names[i]);
        path_in(dir, sizeof dir, lab);
        writer[i][0] = 0;
        CHECK(al_session(pack_of(names[i]), dir, h[i], &n_h[i], &x0[i], &x1[i], writer[i], sizeof writer[i]) == 0, "align %s: the session failed",
              names[i]);
    }
    gl_close();
    /* the box is 1000 px at the display's center: 100 to 1100 */
    CHECK(x1[0] > x0[0] && abs(x0[0] + x1[0] + 1 - AL_W) <= 6, "align: stroop's word, no align, is not centered: ink %d to %d on %d px", x0[0], x1[0], AL_W);
    CHECK(x1[1] > x0[1] && x0[1] >= 100 && x0[1] <= 110 && x1[1] < AL_W / 2, "align: stroop_start's word does not start at the box's edge, 100: ink %d to %d",
          x0[1], x1[1]);
    CHECK(!strcmp(writer[0], YEXP_VERSION_STRING) && !strcmp(writer[2], "0.6.0"), "align: the definitions' writers '%s' and '%s'", writer[0], writer[2]);
    for (j = 0; j < n_h[1] && j < n_h[2]; j++)
        if (h[1][j] == h[2][j]) same++;
    CHECK(n_h[1] == AL_FRAMES / 2 && n_h[2] == n_h[1] && same == n_h[1], "align: %d of %d drawn frames of the old pack equal stroop_start's (%d drawn)", same,
          n_h[1], n_h[2]);
    printf("  align: ink of the word: no align %d to %d, start %d to %d, old pack %d to %d (%d px); old pack's frames %d of %d equal\n", x0[0], x1[0], x0[1],
           x1[1], x0[2], x1[2], AL_W, same, n_h[1]);
}
#else
static void case_gl(void) { printf("  gl: skipped: not built with PL_SIM_GL (Linux, GNU ld)\n"); }
static void align_ink(void) { printf("  align: ink skipped: not built with PL_SIM_GL (Linux, GNU ld)\n"); }
#endif

static void case_align(void) {
    static const char* const names[3] = { "stroop", "stroop_start", "stroop_old" };
    char a[1200], b[1200];
    run_opts o;
    int i;
    for (i = 0; i < 3; i++)
        if (!pack_of(names[i])) { printf("  align: no %s pack, skipped\n", names[i]); return; }
    /* the data: the old pack's --sim run is stroop_start's; the player
     * finds its "start" blocks */
    memset(&o, 0, sizeof o);
    o.seed = 11;
    path_in(a, sizeof a, "align_start_sim");
    path_in(b, sizeof b, "align_old_sim");
    CHECK(ref_run(pack_of("stroop_start"), &o, a) == 0 && ref_run(pack_of("stroop_old"), &o, b) == 0 && same_data(a, b, "align old"),
          "align: the old pack's --sim data are not those of the pack with start written");
    align_ink();
}

int main(int argc, char** argv) {
    int i;
    const char* keep = NULL;
    const char* base = getenv("TMPDIR") ? getenv("TMPDIR") : getenv("TEMP") ? getenv("TEMP") : "/tmp";
    for (i = 1; i < argc; i++) {
        const char* eq = strchr(argv[i], '=');
        if (strcmp(argv[i], "--keep") == 0 && i + 1 < argc) { keep = argv[++i]; continue; }
        if (!eq || n_packs == (int)(sizeof packs / sizeof packs[0])) { printf("usage: test_player_sim NAME=PACK... [--keep DIR]\n"); return 2; }
        packs[n_packs].name = argv[i];
        argv[i][eq - argv[i]] = 0;
        snprintf(packs[n_packs].path, sizeof packs[n_packs].path, "%s", eq + 1);
        if (read_file(eq + 1, &packs[n_packs].data, &packs[n_packs].size) != 0) { printf("cannot read %s\n", eq + 1); return 2; }
        n_packs++;
    }
    if (keep) snprintf(work, sizeof work, "%s", keep);
    else snprintf(work, sizeof work, "%s/pl_sim_%d", base, (int)getpid());
    if (make_dir(work) != 0) { printf("cannot make %s\n", work); return 2; }
#if defined(PL_SIM_MUT) && !defined(_WIN32)
    if (n_packs == 0 && build_packs(work) != 0) { printf("no packs: give NAME=PACK, or set YPAK\n"); return 2; }
#endif
    if (!pack_of("rdk") || !pack_of("oddball")) { printf("the rdk and oddball packs are required\n"); return 2; }
    if (pl_check_nominal_cal(&nominal) < 0 || make_rig(work) != 0) { printf("no rig profile in %s\n", work); return 2; }
    {
        const char* only = getenv("PL_SIM_CASES");   /* a mutant's cases, or all */
#define RUN(name, fn) do { if (!only || strstr(only, name)) fn(); } while (0)
        RUN("equal", case_equal);
        RUN("threads", case_threads);
        RUN("two", case_two_sessions);
        RUN("input", case_input);
        RUN("states", case_states);
        RUN("refusal", case_refusal);
        RUN("live", case_live);
        RUN("stop", case_stop);
        RUN("display", case_display);
        RUN("codes", case_codes);
        RUN("gl", case_gl);
        RUN("rig", case_rig);
        RUN("align", case_align);
#undef RUN
    }
#if defined(PL_SIM_COUNT)
    CHECK(frame_allocs == 0, "%lld heap calls inside pl_sim_frame() over %lld frames", frame_allocs, n_frames_stepped);
    printf("  heap calls inside pl_sim_frame(): %lld over %lld frames\n", frame_allocs, n_frames_stepped);
#endif
    if (!keep) rm_tree(work);
    for (i = 0; i < n_packs; i++) free(packs[i].data);
    printf("%d of %d checks passed\n", checks - fails, checks);
    return fails ? 1 : 0;
}
