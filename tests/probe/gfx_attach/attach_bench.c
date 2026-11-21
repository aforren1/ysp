/* attach_bench.c - what a session's GL work costs the designer's UI thread
 * (ysp/gfx.h v0.23.0, DEFERRED OPEN and SHARED PROGRAMS; docs/gfx.md,
 * "v0.23.0"; docs/designer.md, "The GL context"). Not a test: it prints
 * times. Linux only: EGL's device platform, headless, no window.
 *
 * Usage: bench_gfx_attach WORK PROJECT ROUNDS
 *   PROJECT: a tests/player project (stroop, oddball, rdk, bimanual,
 *   visuomotor, afc2), "many" or "many+words" (check_bench.c's generated
 *   worst cases), or "images" (BENCH_IMAGES: a folder with experiment.json,
 *   pack.json and its PNGs).
 * Environment:
 *   BENCH_GPU     a substring of GL_RENDERER: the EGL device to use
 *   BENCH_MODE    "direct": ygfx_open() and ygfx_prime() on this thread, as
 *                 an attach without a deferred open would; "defer" (the
 *                 default): desc.defer, the load recorded, then
 *                 ygfx_attach() with BENCH_BUDGET_MS (4) a call
 *   BENCH_SHARE   1: desc.share is a gfx opened first on the context, as
 *                 the designer's main.cpp opens one
 *   BENCH_CALLS   set: print each attach call's time
 *
 * The designer's check (dsg_check() -> pl_check()) loads each project; its
 * null gfx is swapped for this one (--wrap of pl_gfx_open_null), at 1920 x
 * 1080 with the nominal calibration and desc.output_texture. The attach
 * runs before pl_close() (--wrap). One line a round; then medians.
 */
#define main check_bench_main_unused
#include "check_bench.c"
#undef main
#include <dlfcn.h>

typedef yscr_proc (*ab_gpa_fn)(const char*);
static ab_gpa_fn ab_gpa;
static yscr_proc ab_proc(void* ctx, const char* name) { (void)ctx; return ab_gpa(name); }
static void (*ab_finish)(void);
static char ab_renderer[200];

static void* ab_sym(void* lib, const char* n) {
    void* p = dlsym(lib, n);
    if (!p && ab_gpa) { yscr_proc f = ab_gpa(n); memcpy(&p, &f, sizeof p); }
    return p;
}

/* A pbuffer context on the first EGL device whose renderer has `want`. */
static int ab_egl(const char* want, int w, int h) {
    void* lib = dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
    unsigned (*query_devices)(int32_t, void**, int32_t*);
    void* (*gpd)(unsigned, void*, const int32_t*);
    unsigned (*init)(void*, int32_t*, int32_t*);
    unsigned (*bind_api)(unsigned);
    unsigned (*choose)(void*, const int32_t*, void**, int32_t, int32_t*);
    void* (*mkctx)(void*, void*, void*, const int32_t*);
    void* (*mkpb)(void*, void*, const int32_t*);
    unsigned (*cur)(void*, void*, void*, void*);
    const unsigned char* (*str)(unsigned);
    void* devs[16];
    int32_t nd = 0, i;
    void* p;
    if (!lib) return -1;
    p = dlsym(lib, "eglGetProcAddress");
    memcpy(&ab_gpa, &p, sizeof p);
    p = ab_sym(lib, "eglQueryDevicesEXT"); memcpy(&query_devices, &p, sizeof p);
    p = ab_sym(lib, "eglGetPlatformDisplayEXT"); memcpy(&gpd, &p, sizeof p);
    p = ab_sym(lib, "eglInitialize"); memcpy(&init, &p, sizeof p);
    p = ab_sym(lib, "eglBindAPI"); memcpy(&bind_api, &p, sizeof p);
    p = ab_sym(lib, "eglChooseConfig"); memcpy(&choose, &p, sizeof p);
    p = ab_sym(lib, "eglCreateContext"); memcpy(&mkctx, &p, sizeof p);
    p = ab_sym(lib, "eglCreatePbufferSurface"); memcpy(&mkpb, &p, sizeof p);
    p = ab_sym(lib, "eglMakeCurrent"); memcpy(&cur, &p, sizeof p);
    if (!query_devices || !gpd || !query_devices(16, devs, &nd)) return -1;
    for (i = 0; i < nd; i++) {
        int32_t a[13], n = 0, mj, mn, pa[5];
        void *dpy, *cfg = NULL, *ctx, *pb;
        dpy = gpd(0x313F, devs[i], NULL);   /* EGL_PLATFORM_DEVICE_EXT */
        if (!dpy || !init(dpy, &mj, &mn)) continue;
        bind_api(0x30A0);
        a[0] = 0x3024; a[1] = 8; a[2] = 0x3023; a[3] = 8; a[4] = 0x3022; a[5] = 8; a[6] = 0x3021; a[7] = 8;
        a[8] = 0x3033; a[9] = 0x0001; a[10] = 0x3040; a[11] = 0x0040; a[12] = 0x3038;
        if (!choose(dpy, a, &cfg, 1, &n) || n < 1) continue;
        a[0] = 0x3098; a[1] = 3; a[2] = 0x30FB; a[3] = 0; a[4] = 0x3038;
        ctx = mkctx(dpy, cfg, NULL, a);
        pa[0] = 0x3057; pa[1] = w; pa[2] = 0x3056; pa[3] = h; pa[4] = 0x3038;
        pb = ctx ? mkpb(dpy, cfg, pa) : NULL;
        if (!pb || !cur(dpy, pb, pb, ctx)) continue;
        p = ab_sym(lib, "glGetString"); memcpy(&str, &p, sizeof p);
        p = ab_sym(lib, "glFinish"); memcpy(&ab_finish, &p, sizeof p);
        snprintf(ab_renderer, sizeof ab_renderer, "%s", (const char*)str(0x1F01));
        if (!want || strstr(ab_renderer, want)) return 0;
        cur(dpy, NULL, NULL, NULL);
    }
    return -1;
}

static double ab_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

/* The round's numbers, ms. */
typedef struct ab_round { double open, check, attach, call_max, finish; int calls; } ab_round;
static ab_round ab_now;
static int ab_on, ab_defer = 1, ab_share;
static double ab_budget = 4.0;
static ycol_cal ab_cal;
static ygfx_gfx ab_first;
static ygfx_gl_caps ab_caps;
static ygfx_gfx* ab_g;

bool __real_pl_gfx_open_null(ygfx_gfx* g, ygfx_desc* d);
bool __wrap_pl_gfx_open_null(ygfx_gfx* g, ygfx_desc* d) {
    double t0;
    bool ok;
    if (!ab_on) return __real_pl_gfx_open_null(g, d);
    d->screen = NULL;
    d->backend = NULL;
    d->backend_ctx = NULL;
    d->gl_proc = ab_defer ? NULL : ab_proc;
    d->gl_ctx = NULL;
    d->width = 1920;
    d->height = 1080;
    d->cal = &ab_cal;
    d->output_texture = true;
    d->defer = ab_defer != 0;
    d->gl_caps = ab_defer ? &ab_caps : NULL;
    d->share = ab_share ? &ab_first : NULL;
    t0 = ab_ms();
    ok = ygfx_open(g, d);
    if (ok && !ab_defer) (void)ygfx_prime(g);
    ab_now.open = ab_ms() - t0;
    if (!ok) printf("open: %s\n", ygfx_error(g));
    ab_g = ok ? g : NULL;
    return ok;
}

void __real_pl_close(pl_player* p);
void __wrap_pl_close(pl_player* p) {
    if (ab_on && ab_g && ab_defer) {
        /* the UI thread's frames: one attach call each */
        int rc;
        do {
            double t0 = ab_ms(), t;
            rc = ygfx_attach(ab_g, ab_proc, NULL, (int64_t)(ab_budget * 1e6));
            t = ab_ms() - t0;
            ab_now.attach += t;
            ab_now.calls++;
            if (t > ab_now.call_max) ab_now.call_max = t;
            if (getenv("BENCH_CALLS")) printf("    call %d: %.2f ms\n", ab_now.calls, t);
        } while (rc == 0);
        if (rc < 0) printf("attach: %s\n", ygfx_error(ab_g));
    }
    if (ab_on) {
        double t0 = ab_ms();
        ab_finish();
        ab_now.finish = ab_ms() - t0;
    }
    ab_g = NULL;
    __real_pl_close(p);
}

static int ab_cmp(const void* a, const void* b) {
    double x = *(const double*)a, y = *(const double*)b;
    return x < y ? -1 : x > y;
}
static double ab_med(const ab_round* r, int n, size_t off) {
    double v[256];
    int i;
    for (i = 0; i < n; i++) memcpy(&v[i], (const char*)&r[i] + off, sizeof(double));
    qsort(v, (size_t)n, sizeof *v, ab_cmp);
    return v[n / 2];
}

int main(int argc, char** argv) {
    static ab_round R[256];
    const char* work;
    const char* name;
    const char* mode = getenv("BENCH_MODE");
    int rounds, r;
    char folder[DSG_PATH_MAX], err[512];
    dsg_project p;
    dsg_checker* c;
    dsg_report rep;
    ygfx_desc fd;
    if (argc < 4) { printf("usage: bench_gfx_attach WORK PROJECT ROUNDS\n"); return 2; }
    work = argv[1];
    name = argv[2];
    rounds = atoi(argv[3]);
    if (rounds < 2 || rounds > 256) return 2;
    ab_defer = !(mode && strcmp(mode, "direct") == 0);
    ab_share = getenv("BENCH_SHARE") && getenv("BENCH_SHARE")[0] == '1';
    if (getenv("BENCH_BUDGET_MS")) ab_budget = atof(getenv("BENCH_BUDGET_MS"));
    if (ab_egl(getenv("BENCH_GPU"), 1920, 1080)) { printf("no EGL device for %s\n", getenv("BENCH_GPU") ? getenv("BENCH_GPU") : "(any)"); return 1; }
    if (pl_check_nominal_cal(&ab_cal)) return 1;
    /* the designer's first gfx: the caps and, with BENCH_SHARE, the programs */
    memset(&fd, 0, sizeof fd);
    fd.gl_proc = ab_proc;
    fd.width = 1920;
    fd.height = 1080;
    fd.output_texture = true;
    if (!ygfx_open(&ab_first, &fd) || ygfx_gl_caps_get(&ab_first, &ab_caps) != YGFX_OK) { printf("first gfx: %s\n", ygfx_error(&ab_first)); return 1; }
    if (dsg_make_dir(work, err, sizeof err)) { printf("%s\n", err); return 1; }
    if (strcmp(name, "many") == 0) { if (make_worst(work, "many", false, folder, sizeof folder)) return 1; }
    else if (strcmp(name, "many+words") == 0) { if (make_worst(work, "many_words", true, folder, sizeof folder)) return 1; }
    else if (strcmp(name, "images") == 0) {
        const char* src = getenv("BENCH_IMAGES");
        DIR* dir;
        struct dirent* e;
        char a[DSG_PATH_MAX], b[DSG_PATH_MAX];
        if (!src || dsg_join(folder, sizeof folder, work, "images") || dsg_make_dir(folder, err, sizeof err) || !(dir = opendir(src))) return 1;
        while ((e = readdir(dir)) != NULL) {
            if (dsg_join(a, sizeof a, src, e->d_name) || !dsg_is_file(a)) continue;
            if (dsg_join(b, sizeof b, folder, e->d_name) || copy_file(a, b)) { closedir(dir); return 1; }
        }
        closedir(dir);
    } else {
        if (copy_project(work, name, folder, sizeof folder)) { printf("%s: cannot copy\n", name); return 1; }
        if (strcmp(name, "stroop") == 0 && copy_font(folder, "NotoSans-Regular.ttf")) return 1;
    }
    memset(&p, 0, sizeof p);
    c = dsg_checker_new();
    if (!c || dsg_project_open(&p, folder, err, sizeof err)) { printf("%s: %s\n", name, err); return 1; }
    if (dsg_check(c, &p, p.text, p.text_len, NULL, &rep) || !rep.have_pack) { printf("%s: no pack\n", name); return 1; }
    printf("%s | %s | %s%s, budget %.1f ms\n", name, ab_renderer, ab_defer ? "defer" : "direct", ab_share ? ", shared programs" : "", ab_budget);
    ab_on = 1;
    for (r = 0; r < rounds; r++) {
        double t0;
        memset(&ab_now, 0, sizeof ab_now);
        t0 = ab_ms();
        dsg_check(c, &p, p.text, p.text_len, NULL, &rep);
        ab_now.check = ab_ms() - t0 - ab_now.attach - ab_now.finish;
        if (rep.status != 0) printf("round %d: status %d\n", r, rep.status);
        R[r] = ab_now;
        printf("  round %2d: open %7.2f, rest of the load %7.2f, attach %7.2f in %3d calls (max %6.2f), finish %6.2f\n", r, ab_now.open,
               ab_now.check - ab_now.open, ab_now.attach, ab_now.calls, ab_now.call_max, ab_now.finish);
    }
    {
        double cm[256];
        int i, nc;
        for (i = 1; i < rounds; i++) cm[i - 1] = (double)R[i].calls;
        nc = rounds - 1;
        qsort(cm, (size_t)nc, sizeof *cm, ab_cmp);
        printf("SUMMARY %s | %s | %s%s | rounds 2..%d (median): open %.2f, attach %.2f in %.0f calls, max call %.2f, finish %.2f\n",
               name, ab_renderer, ab_defer ? "defer" : "direct", ab_share ? "+share" : "", rounds, ab_med(R + 1, nc, offsetof(ab_round, open)),
               ab_med(R + 1, nc, offsetof(ab_round, attach)), cm[nc / 2], ab_med(R + 1, nc, offsetof(ab_round, call_max)),
               ab_med(R + 1, nc, offsetof(ab_round, finish)));
        {
            double w = 0.0;
            for (i = 1; i < rounds; i++) if (R[i].call_max > w) w = R[i].call_max;
            printf("  worst call of rounds 2..%d: %.2f ms; round 1: open %.2f, attach %.2f in %d calls (max %.2f)\n", rounds, w, R[0].open,
                   R[0].attach, R[0].calls, R[0].call_max);
        }
    }
    ab_on = 0;
    dsg_project_close(&p);
    dsg_checker_free(c);
    ygfx_close(&ab_first);
    return 0;
}
