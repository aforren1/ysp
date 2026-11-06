/* Time, output, statistics, the vblank grid, CVDisplayLink, the EGL and GL
 * loaders and window handling for the macOS display-timing probe. */
#import <Cocoa/Cocoa.h>
#import <CoreVideo/CoreVideo.h>
#include <dlfcn.h>
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <mach/mach_time.h>
#include "probe.h"

probe_opts g_opt;
FILE*      g_sum;
egl_api    E;
gl_api     G;

/* ---- time ---------------------------------------------------------------- */
static mach_timebase_info_data_t tb;

int64_t mach_ticks_to_ns(uint64_t t) {
    if (tb.denom == 0) mach_timebase_info(&tb);
    /* split so that a large tick count cannot overflow the multiply */
    return (int64_t)((t / tb.denom) * tb.numer + (t % tb.denom) * tb.numer / tb.denom);
}
int64_t mach_ns_now(void) { return mach_ticks_to_ns(mach_absolute_time()); }
int64_t mach_cont_ns_now(void) { return mach_ticks_to_ns(mach_continuous_time()); }
int64_t secs_to_ns(double s) { return (int64_t)llround(s * 1e9); }
double  ns_to_secs(int64_t ns) { return (double)ns * 1e-9; }
int64_t sdl_ticks_now(void) { return (int64_t)SDL_GetTicksNS(); }
int64_t clock_ns(int id) {
    struct timespec ts;
    clock_gettime((clockid_t)id, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

corr correlate(clock_fn other, int tries) {
    corr best = { 0, 0, INT64_MAX };
    for (int i = 0; i < tries; i++) {
        int64_t a = mach_ns_now();
        int64_t o = other();
        int64_t b = mach_ns_now();
        if (b - a < best.width) { best.width = b - a; best.mach = a + (b - a) / 2; best.other = o; }
    }
    return best;
}

/* ---- output -------------------------------------------------------------- */
void P(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    if (g_sum) {
        va_start(ap, fmt);
        vfprintf(g_sum, fmt, ap);
        va_end(ap);
        fflush(g_sum);
    }
    fflush(stdout);
}

void out_init(const char* section) {
    char path[1024];
    mkdir(g_opt.out, 0755);
    snprintf(path, sizeof path, "%s/summary.txt", g_opt.out);
    g_sum = fopen(path, "ab");
    time_t now = time(NULL);
    char when[64];
    strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", localtime(&now));
    P("\n==== %s%s%s  %s\n", section, g_opt.tag ? " " : "", g_opt.tag ? g_opt.tag : "", when);
}

const char* tagged(const char* base) {
    static char buf[256];
    if (g_opt.tag && g_opt.tag[0]) snprintf(buf, sizeof buf, "%s_%s", base, g_opt.tag);
    else snprintf(buf, sizeof buf, "%s", base);
    return buf;
}

FILE* open_csv(const char* name, const char* header) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s.csv", g_opt.out, name);
    FILE* f = fopen(path, "wb");
    if (f && header) fprintf(f, "%s\n", header);
    return f;
}

/* ---- statistics ---------------------------------------------------------- */
static int cmp_d(const void* a, const void* b) {
    double x = *(const double*)a, y = *(const double*)b;
    return x < y ? -1 : x > y;
}

stats stats_of(const double* v, int n) {
    stats s = { 0 };
    if (n <= 0) return s;
    double* c = malloc(sizeof(double) * (size_t)n);
    memcpy(c, v, sizeof(double) * (size_t)n);
    qsort(c, (size_t)n, sizeof(double), cmp_d);
    double sum = 0, sq = 0;
    for (int i = 0; i < n; i++) sum += c[i];
    s.mean = sum / n;
    for (int i = 0; i < n; i++) sq += (c[i] - s.mean) * (c[i] - s.mean);
    s.sd = n > 1 ? sqrt(sq / (n - 1)) : 0;
    s.n = n;
    s.min = c[0];
    s.max = c[n - 1];
    s.p01 = c[(int)floor(0.01 * (n - 1) + 0.5)];
    s.p50 = c[(int)floor(0.50 * (n - 1) + 0.5)];
    s.p99 = c[(int)floor(0.99 * (n - 1) + 0.5)];
    free(c);
    return s;
}

void P_stats(const char* name, const double* v, int n, const char* unit) {
    stats s = stats_of(v, n);
    if (n == 0) { P("  %-34s n 0\n", name); return; }
    P("  %-34s n %5d  min %9.3f  p01 %9.3f  p50 %9.3f  p99 %9.3f  max %9.3f  sd %8.3f %s\n",
      name, s.n, s.min, s.p01, s.p50, s.p99, s.max, s.sd, unit);
}

/* ---- the vblank grid ------------------------------------------------------
 * Least squares of time against vblank index. The index of each time comes
 * from the nominal period first, then from the fitted one, so a missed
 * callback (a gap of two periods) keeps its place on the grid. */
grid grid_fit(const int64_t* t, int n, double nominal_ns) {
    grid g = { 0 };
    if (n < 8 || nominal_ns <= 0) return g;
    double base = (double)t[0], period = nominal_ns, a = 0;
    for (int pass = 0; pass < 3; pass++) {
        double sx = 0, sy = 0, sxx = 0, sxy = 0;
        for (int i = 0; i < n; i++) {
            double y = (double)t[i] - base;
            double x = floor((y - a) / period + 0.5);
            sx += x; sy += y; sxx += x * x; sxy += x * y;
        }
        double den = n * sxx - sx * sx;
        if (den == 0) return g;
        period = (n * sxy - sx * sy) / den;
        a = (sy - period * sx) / n;
    }
    g.ok = 1;
    g.n = n;
    g.t0 = base + a;
    g.period = period;
    double* r = malloc(sizeof(double) * (size_t)n);
    int64_t last = 0;
    for (int i = 0; i < n; i++) {
        r[i] = fabs(grid_residual_us(&g, t[i]));
        int64_t k = grid_round(&g, t[i]);
        if (i > 0 && k - last > 1) g.missed += (int)(k - last - 1);
        last = k;
    }
    stats s = stats_of(r, n);
    g.res_p50 = s.p50; g.res_p99 = s.p99; g.res_max = s.max;
    free(r);
    return g;
}
double  grid_index(const grid* g, int64_t t) { return ((double)t - g->t0) / g->period; }
int64_t grid_round(const grid* g, int64_t t) { return (int64_t)floor(grid_index(g, t) + 0.5); }
double  grid_residual_us(const grid* g, int64_t t) {
    double k = floor(grid_index(g, t) + 0.5);
    return ((double)t - (g->t0 + k * g->period)) / 1000.0;
}

/* ---- CVDisplayLink -------------------------------------------------------- */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
static CVReturn cv_cb(CVDisplayLinkRef link, const CVTimeStamp* now, const CVTimeStamp* out,
                      CVOptionFlags fi, CVOptionFlags* fo, void* ctx) {
    (void)link; (void)fi; (void)fo;
    cv_link* c = (cv_link*)ctx;
    int64_t arrive = mach_ns_now();
    int i = c->n;
    if (i < c->cap) {
        cv_rec* r = &c->rec[i];
        r->now_host = mach_ticks_to_ns(now->hostTime);
        r->out_host = mach_ticks_to_ns(out->hostTime);
        r->video_time = now->videoTime;
        r->video_scale = now->videoTimeScale;
        r->video_period = now->videoRefreshPeriod;
        r->rate_scalar = now->rateScalar;
        r->arrive = arrive;
        __atomic_store_n(&c->n, i + 1, __ATOMIC_RELEASE);
    }
    return kCVReturnSuccess;
}

bool cv_start(cv_link* c, uint32_t display, int cap) {
    memset(c, 0, sizeof *c);
    c->rec = calloc((size_t)cap, sizeof(cv_rec));
    c->cap = cap;
    CVDisplayLinkRef l = NULL;
    if (CVDisplayLinkCreateWithCGDisplay(display, &l) != kCVReturnSuccess) return false;
    CVDisplayLinkSetOutputCallback(l, cv_cb, c);
    CVTime nom = CVDisplayLinkGetNominalOutputVideoRefreshPeriod(l);
    if (nom.timeScale) c->nominal_ns = 1e9 * (double)nom.timeValue / (double)nom.timeScale;
    c->link = l;
    if (CVDisplayLinkStart(l) != kCVReturnSuccess) return false;
    c->actual_ns = 1e9 * CVDisplayLinkGetActualOutputVideoRefreshPeriod(l);
    return true;
}

void cv_stop(cv_link* c) {
    if (!c->link) return;
    c->actual_ns = 1e9 * CVDisplayLinkGetActualOutputVideoRefreshPeriod((CVDisplayLinkRef)c->link);
    CVDisplayLinkStop((CVDisplayLinkRef)c->link);
    CVDisplayLinkRelease((CVDisplayLinkRef)c->link);
    c->link = NULL;
}
#pragma clang diagnostic pop

bool cv_latest(const cv_link* c, int64_t* vblank, double* period) {
    int n = __atomic_load_n(&c->n, __ATOMIC_ACQUIRE);
    if (n < 2) return false;
    /* A line through the last up to 120 callbacks: a single inNow can sit
     * hundreds of microseconds off the grid, so a target planned from it
     * alone would carry that error. */
    int k = n > 120 ? n - 120 : 0;
    double nom = c->nominal_ns > 0 ? c->nominal_ns : 16666666.0;
    int64_t t[120];
    int m = 0;
    for (int i = k; i < n; i++) t[m++] = c->rec[i].now_host;
    grid g = grid_fit(t, m, nom);
    if (!g.ok) {
        *vblank = c->rec[n - 1].now_host;
        *period = nom;
        return true;
    }
    *vblank = (int64_t)(g.t0 + (double)grid_round(&g, c->rec[n - 1].now_host) * g.period);
    *period = g.period;
    return true;
}

grid cv_grid(const cv_link* c, int64_t from, int64_t to) {
    int n = __atomic_load_n(&c->n, __ATOMIC_ACQUIRE);
    int64_t* t = malloc(sizeof(int64_t) * (size_t)(n + 1));
    int m = 0;
    for (int i = 0; i < n; i++)
        if (c->rec[i].now_host >= from && (to == 0 || c->rec[i].now_host <= to)) t[m++] = c->rec[i].now_host;
    grid g = grid_fit(t, m, c->nominal_ns);
    free(t);
    return g;
}

void cv_report(const cv_link* c, const char* name) {
    int n = c->n;
    grid g = cv_grid(c, 0, 0);
    P("  CVDisplayLink: %d callbacks, nominal %.6f ms, actual %.6f ms, fit %.6f ms (%.4f Hz), "
      "missed %d, |residual| p50 %.2f p99 %.2f max %.2f us\n",
      n, c->nominal_ns / 1e6, c->actual_ns / 1e6, g.period / 1e6, g.ok ? 1e9 / g.period : 0.0,
      g.missed, g.res_p50, g.res_p99, g.res_max);
    if (n < 2) return;
    double* d = malloc(sizeof(double) * (size_t)n * 3);
    double* e = d + n;
    double* f = d + 2 * n;
    for (int i = 0; i < n; i++) {
        d[i] = (double)(c->rec[i].arrive - c->rec[i].now_host) / 1000.0;
        e[i] = (double)(c->rec[i].out_host - c->rec[i].now_host) / 1e6;
        f[i] = (double)(c->rec[i].arrive - c->rec[i].out_host) / 1e6;
    }
    P_stats("CV callback minus inNow", d, n, "us");
    P_stats("CV inOutputTime minus inNow", e, n, "ms");
    P_stats("CV callback minus inOutputTime", f, n, "ms");
    if (name) {
        FILE* csv = open_csv(name, "i,now_host_ns,out_host_ns,video_time,video_scale,video_period,rate_scalar,arrive_ns");
        for (int i = 0; csv && i < n; i++) {
            const cv_rec* r = &c->rec[i];
            fprintf(csv, "%d,%lld,%lld,%lld,%d,%lld,%.9f,%lld\n", i, (long long)r->now_host,
                    (long long)r->out_host, (long long)r->video_time, r->video_scale,
                    (long long)r->video_period, r->rate_scalar, (long long)r->arrive);
        }
        if (csv) fclose(csv);
    }
    free(d);
}

/* ---- EGL ------------------------------------------------------------------ */
bool has_ext(const char* list, const char* name) {
    if (!list) return false;
    size_t n = strlen(name);
    for (const char* p = list; (p = strstr(p, name)) != NULL; p += n)
        if ((p == list || p[-1] == ' ') && (p[n] == ' ' || p[n] == 0)) return true;
    return false;
}

void* egl_proc(const char* name) {
    void* p = E.GetProcAddress ? (void*)E.GetProcAddress(name) : NULL;
    if (!p && E.lib) p = dlsym(E.lib, name);
    return p;
}

bool egl_load(const char* dir) {
    if (E.lib) return true;
    char path[1024];
    snprintf(path, sizeof path, "%s/libEGL.dylib", dir ? dir : ".");
    E.lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!E.lib) { P("  dlopen %s: %s\n", path, dlerror()); return false; }
    E.GetProcAddress = (PFNEGLGETPROCADDRESSPROC)dlsym(E.lib, "eglGetProcAddress");
#define L(field, name) E.field = (__typeof__(E.field))egl_proc(name)
    L(GetPlatformDisplayEXT, "eglGetPlatformDisplayEXT");
    L(Initialize, "eglInitialize");
    L(Terminate, "eglTerminate");
    L(QueryString, "eglQueryString");
    L(GetConfigs, "eglGetConfigs");
    L(ChooseConfig, "eglChooseConfig");
    L(GetConfigAttrib, "eglGetConfigAttrib");
    L(CreateContext, "eglCreateContext");
    L(DestroyContext, "eglDestroyContext");
    L(CreatePbufferSurface, "eglCreatePbufferSurface");
    L(CreatePbufferFromClientBuffer, "eglCreatePbufferFromClientBuffer");
    L(DestroySurface, "eglDestroySurface");
    L(MakeCurrent, "eglMakeCurrent");
    L(SwapBuffers, "eglSwapBuffers");
    L(SwapInterval, "eglSwapInterval");
    L(SurfaceAttrib, "eglSurfaceAttrib");
    L(QuerySurface, "eglQuerySurface");
    L(GetError, "eglGetError");
    L(CreateSyncKHR, "eglCreateSyncKHR");
    L(DestroySyncKHR, "eglDestroySyncKHR");
    L(ClientWaitSyncKHR, "eglClientWaitSyncKHR");
    L(QueryDisplayAttribEXT, "eglQueryDisplayAttribEXT");
    L(QueryDeviceAttribEXT, "eglQueryDeviceAttribEXT");
    L(QueryDeviceStringEXT, "eglQueryDeviceStringEXT");
#undef L
    return E.Initialize && E.GetPlatformDisplayEXT && E.MakeCurrent;
}

const char* egl_err_name(EGLint e) {
    switch (e) {
    case EGL_SUCCESS: return "EGL_SUCCESS";
    case EGL_NOT_INITIALIZED: return "EGL_NOT_INITIALIZED";
    case EGL_BAD_ACCESS: return "EGL_BAD_ACCESS";
    case EGL_BAD_ALLOC: return "EGL_BAD_ALLOC";
    case EGL_BAD_ATTRIBUTE: return "EGL_BAD_ATTRIBUTE";
    case EGL_BAD_CONFIG: return "EGL_BAD_CONFIG";
    case EGL_BAD_CONTEXT: return "EGL_BAD_CONTEXT";
    case EGL_BAD_CURRENT_SURFACE: return "EGL_BAD_CURRENT_SURFACE";
    case EGL_BAD_DISPLAY: return "EGL_BAD_DISPLAY";
    case EGL_BAD_MATCH: return "EGL_BAD_MATCH";
    case EGL_BAD_NATIVE_PIXMAP: return "EGL_BAD_NATIVE_PIXMAP";
    case EGL_BAD_NATIVE_WINDOW: return "EGL_BAD_NATIVE_WINDOW";
    case EGL_BAD_PARAMETER: return "EGL_BAD_PARAMETER";
    case EGL_BAD_SURFACE: return "EGL_BAD_SURFACE";
    case EGL_CONTEXT_LOST: return "EGL_CONTEXT_LOST";
    default: return "EGL_?";
    }
}

EGLDisplay angle_display(const char* backend) {
    EGLAttrib type = 0;
    if (backend && strcmp(backend, "metal") == 0) type = PROBE_EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE;
    else if (backend && strcmp(backend, "gl") == 0) type = PROBE_EGL_PLATFORM_ANGLE_TYPE_OPENGL_ANGLE;
    EGLint attrs[3] = { PROBE_EGL_PLATFORM_ANGLE_TYPE_ANGLE, (EGLint)type, EGL_NONE };
    if (!type) attrs[0] = EGL_NONE;
    EGLDisplay d = E.GetPlatformDisplayEXT(PROBE_EGL_PLATFORM_ANGLE_ANGLE, NULL, attrs);
    if (d == EGL_NO_DISPLAY) { P("  eglGetPlatformDisplayEXT(%s): %s\n", backend, egl_err_name(E.GetError())); return d; }
    EGLint maj = 0, min = 0;
    if (!E.Initialize(d, &maj, &min)) {
        P("  eglInitialize(%s): %s\n", backend, egl_err_name(E.GetError()));
        return EGL_NO_DISPLAY;
    }
    return d;
}

void egl_print_config(EGLDisplay d, EGLConfig c, const char* prefix) {
    EGLint r = 0, g = 0, b = 0, a = 0, dep = 0, st = 0, surf = 0, rend = 0, id = 0, ct = 0,
           bind = 0, cav = 0, samples = 0;
    E.GetConfigAttrib(d, c, EGL_CONFIG_ID, &id);
    E.GetConfigAttrib(d, c, EGL_RED_SIZE, &r);
    E.GetConfigAttrib(d, c, EGL_GREEN_SIZE, &g);
    E.GetConfigAttrib(d, c, EGL_BLUE_SIZE, &b);
    E.GetConfigAttrib(d, c, EGL_ALPHA_SIZE, &a);
    E.GetConfigAttrib(d, c, EGL_DEPTH_SIZE, &dep);
    E.GetConfigAttrib(d, c, EGL_STENCIL_SIZE, &st);
    E.GetConfigAttrib(d, c, EGL_SURFACE_TYPE, &surf);
    E.GetConfigAttrib(d, c, EGL_RENDERABLE_TYPE, &rend);
    E.GetConfigAttrib(d, c, EGL_CONFIG_CAVEAT, &cav);
    E.GetConfigAttrib(d, c, EGL_SAMPLES, &samples);
    if (!E.GetConfigAttrib(d, c, EGL_COLOR_COMPONENT_TYPE_EXT, &ct)) { ct = 0; E.GetError(); }
    if (!E.GetConfigAttrib(d, c, PROBE_EGL_BIND_TO_TEXTURE_TARGET_ANGLE, &bind)) { bind = 0; E.GetError(); }
    P("%sconfig %3d  RGBA %2d/%2d/%2d/%2d %s  depth %2d stencil %d samples %d  surface 0x%03x%s%s  "
      "ES3 %d  caveat 0x%x  bind_target 0x%x\n",
      prefix, id, r, g, b, a, ct == EGL_COLOR_COMPONENT_TYPE_FLOAT_EXT ? "float" : "fixed", dep, st,
      samples, surf, (surf & EGL_WINDOW_BIT) ? " window" : "", (surf & EGL_PBUFFER_BIT) ? " pbuffer" : "",
      (rend & 0x40) != 0, cav, bind);
}

/* ---- GL -------------------------------------------------------------------- */
bool gl_load(void* (*get)(const char*)) {
#define L(field, name) G.field = (__typeof__(G.field))get(name)
    L(ClearColor, "glClearColor");
    L(Clear, "glClear");
    L(Enable, "glEnable");
    L(Disable, "glDisable");
    L(Scissor, "glScissor");
    L(Viewport, "glViewport");
    L(Flush, "glFlush");
    L(Finish, "glFinish");
    L(GetString, "glGetString");
    L(GetIntegerv, "glGetIntegerv");
    L(ReadPixels, "glReadPixels");
    L(GetError, "glGetError");
    L(CreateShader, "glCreateShader");
    L(ShaderSource, "glShaderSource");
    L(CompileShader, "glCompileShader");
    L(GetShaderiv, "glGetShaderiv");
    L(GetShaderInfoLog, "glGetShaderInfoLog");
    L(CreateProgram, "glCreateProgram");
    L(AttachShader, "glAttachShader");
    L(LinkProgram, "glLinkProgram");
    L(GetProgramiv, "glGetProgramiv");
    L(UseProgram, "glUseProgram");
    L(GetUniformLocation, "glGetUniformLocation");
    L(Uniform1f, "glUniform1f");
    L(Uniform1i, "glUniform1i");
    L(DrawArrays, "glDrawArrays");
    L(GenVertexArrays, "glGenVertexArrays");
    L(BindVertexArray, "glBindVertexArray");
#undef L
    return G.Clear && G.GetString && G.ReadPixels && G.CreateShader && G.DrawArrays && G.GenVertexArrays;
}

void gl_print_strings(const char* prefix) {
    P("%sGL_VENDOR   %s\n", prefix, (const char*)G.GetString(PGL_VENDOR));
    P("%sGL_RENDERER %s\n", prefix, (const char*)G.GetString(PGL_RENDERER));
    P("%sGL_VERSION  %s\n", prefix, (const char*)G.GetString(PGL_VERSION));
    P("%sGLSL        %s\n", prefix, (const char*)G.GetString(PGL_SHADING_LANGUAGE_VERSION));
}

static unsigned gl_shader(unsigned type, const char* src) {
    unsigned s = G.CreateShader(type);
    G.ShaderSource(s, 1, &src, NULL);
    G.CompileShader(s);
    int ok = 0;
    G.GetShaderiv(s, 0x8B81 /* COMPILE_STATUS */, &ok);
    if (!ok) {
        char log[1024];
        G.GetShaderInfoLog(s, sizeof log, NULL, log);
        P("  shader: %s\n", log);
    }
    return s;
}

unsigned gl_program(int mode) {
    static const char* vs =
        "#version 300 es\n"
        "void main() {\n"
        "  vec2 p = vec2(float((gl_VertexID & 1) << 2) - 1.0, float((gl_VertexID & 2) << 1) - 1.0);\n"
        "  gl_Position = vec4(p, 0.0, 1.0);\n"
        "}\n";
    static const char* fs_ramp =
        "#version 300 es\n"
        "precision highp float;\n"
        "uniform float u_width;\n"
        "out vec4 o;\n"
        "void main() { float v = floor(gl_FragCoord.x) / (u_width - 1.0); o = vec4(v, v, v, 1.0); }\n";
    static const char* fs_load =
        "#version 300 es\n"
        "precision highp float;\n"
        "uniform int u_iters;\n"
        "uniform float u_level;\n"
        "out vec4 o;\n"
        "void main() {\n"
        "  float x = gl_FragCoord.x * 1e-4;\n"
        "  for (int i = 0; i < u_iters; i++) x = fract(sin(x) * 43758.5453 + 0.1);\n"
        "  o = vec4(vec3(u_level + x * 1e-6), 1.0);\n"
        "}\n";
    unsigned p = G.CreateProgram();
    G.AttachShader(p, gl_shader(0x8B31 /* VERTEX */, vs));
    G.AttachShader(p, gl_shader(0x8B30 /* FRAGMENT */, mode == 0 ? fs_ramp : fs_load));
    G.LinkProgram(p);
    int ok = 0;
    G.GetProgramiv(p, 0x8B82 /* LINK_STATUS */, &ok);
    if (!ok) P("  program link failed\n");
    static unsigned vao;
    if (!vao) G.GenVertexArrays(1, &vao);
    G.BindVertexArray(vao);
    return p;
}

void gl_draw_fullscreen(unsigned prog, int iters, float level) {
    G.UseProgram(prog);
    int li = G.GetUniformLocation(prog, "u_iters");
    int ll = G.GetUniformLocation(prog, "u_level");
    if (li >= 0) G.Uniform1i(li, iters);
    if (ll >= 0) G.Uniform1f(ll, level);
    G.DrawArrays(PGL_TRIANGLES, 0, 3);
}

/* ---- windows --------------------------------------------------------------- */
static bool g_quit;
bool quit_requested(void) { return g_quit; }

bool session_locked(void) {
    CFDictionaryRef d = CGSessionCopyCurrentDictionary();
    if (!d) return false;
    CFBooleanRef v = (CFBooleanRef)CFDictionaryGetValue(d, CFSTR("CGSSessionScreenIsLocked"));
    bool locked = v && CFGetTypeID(v) == CFBooleanGetTypeID() && CFBooleanGetValue(v);
    CFRelease(d);
    return locked;
}

void print_session(void) {
    P("  session: screen locked %d, main display asleep %d, app active %d\n", session_locked(),
      CGDisplayIsAsleep(CGMainDisplayID()), NSApp.active);
    if (session_locked()) P("  WARNING: the screen is locked; no frame reaches the panel\n");
}

static void pump_once(void) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_EVENT_QUIT) g_quit = true;
        if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE) g_quit = true;
    }
}

void pump_for(double seconds) {
    int64_t end = mach_ns_now() + secs_to_ns(seconds);
    do {
        pump_once();
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.005, true);
    } while (mach_ns_now() < end && !g_quit);
}

static NSWindow* nswin(SDL_Window* w) {
    return (__bridge NSWindow*)SDL_GetPointerProperty(SDL_GetWindowProperties(w),
                                                      SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, NULL);
}

void print_window(SDL_Window* w) {
    int x = 0, y = 0, ww = 0, wh = 0, pw = 0, ph = 0;
    SDL_GetWindowPosition(w, &x, &y);
    SDL_GetWindowSize(w, &ww, &wh);
    SDL_GetWindowSizeInPixels(w, &pw, &ph);
    SDL_WindowFlags f = SDL_GetWindowFlags(w);
    NSWindow* nw = nswin(w);
    const SDL_DisplayMode* fm = SDL_GetWindowFullscreenMode(w);
    const SDL_DisplayMode* cm = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(w));
    print_session();
    P("  window: kind %s, at %d,%d, %dx%d points, %dx%d pixels, fullscreen %d, borderless %d, "
      "NSWindow level %ld, styleMask 0x%lx, fullscreen space %d, occlusion visible %d\n",
      g_opt.kind, x, y, ww, wh, pw, ph, (f & SDL_WINDOW_FULLSCREEN) != 0, (f & SDL_WINDOW_BORDERLESS) != 0,
      (long)nw.level, (unsigned long)nw.styleMask, (nw.styleMask & NSWindowStyleMaskFullScreen) != 0,
      (nw.occlusionState & NSWindowOcclusionStateVisible) != 0);
    if (cm) P("  display mode now: %dx%d @ %.3f Hz (%d/%d), density %.2f; window's fullscreen mode %s\n",
              cm->w, cm->h, cm->refresh_rate, cm->refresh_rate_numerator, cm->refresh_rate_denominator,
              cm->pixel_density, fm ? "set" : "desktop");
}

static const SDL_DisplayMode* find_mode(SDL_DisplayID d, double hz) {
    int n = 0;
    SDL_DisplayMode** modes = SDL_GetFullscreenDisplayModes(d, &n);
    const SDL_DisplayMode* desk = SDL_GetDesktopDisplayMode(d);
    static SDL_DisplayMode pick;
    bool found = false;
    for (int i = 0; i < n; i++) {
        const SDL_DisplayMode* m = modes[i];
        if (m->w == desk->w && m->h == desk->h && fabs(m->refresh_rate - hz) < 0.5 &&
            m->pixel_density == desk->pixel_density) { pick = *m; found = true; break; }
    }
    SDL_free(modes);
    return found ? &pick : NULL;
}

SDL_Window* open_window(bool gl, const char* title) {
    const char* kind = g_opt.kind ? g_opt.kind : "window";
    if (session_locked() && !g_opt.allow_locked) {
        print_session();
        P("RESULT refused: the screen is locked, so no frame would reach the panel (--allow-locked runs anyway)\n");
        exit(3);
    }
    SDL_DisplayID disp = SDL_GetPrimaryDisplay();
    const SDL_DisplayMode* desk = SDL_GetDesktopDisplayMode(disp);
    SDL_WindowFlags flags = gl ? SDL_WINDOW_OPENGL : SDL_WINDOW_METAL;
    int w = 800, h = 600;
    if (strcmp(kind, "borderless") == 0) { flags |= SDL_WINDOW_BORDERLESS; w = desk->w; h = desk->h; }
    SDL_SetHint(SDL_HINT_VIDEO_MAC_FULLSCREEN_SPACES, strcmp(kind, "space") == 0 ? "1" : "0");
    SDL_Window* win = SDL_CreateWindow(title, w, h, flags | SDL_WINDOW_HIDDEN);
    if (!win) { P("  SDL_CreateWindow: %s\n", SDL_GetError()); return NULL; }
    if (strcmp(kind, "borderless") == 0) {
        SDL_SetWindowPosition(win, 0, 0);
        /* above the menu bar and the Dock, as a display-sized stimulus window needs */
        nswin(win).level = NSMainMenuWindowLevel + 1;
    } else if (strcmp(kind, "window") == 0) {
        /* A program started from a terminal does not get the foreground on
         * macOS 14 and later, so a normal-level window can sit behind the
         * active app, or in another space when that app is fullscreen, and
         * then its drawables are never presented. */
        nswin(win).level = NSFloatingWindowLevel;
    }
    if (strcmp(kind, "space") != 0)
        nswin(win).collectionBehavior |= NSWindowCollectionBehaviorCanJoinAllSpaces |
                                         NSWindowCollectionBehaviorFullScreenAuxiliary;
    SDL_ShowWindow(win);
    SDL_RaiseWindow(win);
    [NSApp activateIgnoringOtherApps:YES];
    if (strcmp(kind, "space") == 0 || strcmp(kind, "fullscreen") == 0) {
        SDL_SetWindowFullscreenMode(win, NULL);
        SDL_SetWindowFullscreen(win, true);
    } else if (strcmp(kind, "exclusive") == 0) {
        double hz = g_opt.hz > 0 ? g_opt.hz : desk->refresh_rate;
        const SDL_DisplayMode* m = find_mode(disp, hz);
        if (!m) P("  no %dx%d mode at %.1f Hz; using the desktop mode\n", desk->w, desk->h, hz);
        if (!SDL_SetWindowFullscreenMode(win, m)) P("  SDL_SetWindowFullscreenMode: %s\n", SDL_GetError());
        SDL_SetWindowFullscreen(win, true);
    }
    SDL_SyncWindow(win);
    /* A space animates for about a second; a mode change blanks the panel for
     * longer. Settle before anything is timed. */
    pump_for(strcmp(kind, "window") == 0 ? 0.5 : 3.0);
    print_window(win);
    return win;
}

void close_window(SDL_Window* w) {
    if (!w) return;
    if (SDL_GetWindowFlags(w) & SDL_WINDOW_FULLSCREEN) {
        SDL_SetWindowFullscreen(w, false);
        SDL_SyncWindow(w);
        pump_for(1.5);
    }
    SDL_DestroyWindow(w);
    pump_for(0.2);
}
