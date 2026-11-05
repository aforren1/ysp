/* Shared declarations of the macOS display-timing probe.
 * docs/screen_macos_probe.md explains what each section measures. */
#ifndef YSP_SCREEN_MACOS_PROBE_H
#define YSP_SCREEN_MACOS_PROBE_H

#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_egl.h>

/* ---- options ----------------------------------------------------------- */
typedef struct probe_opts {
    const char* out;          /* output folder                                */
    const char* angle;        /* folder with libEGL.dylib and libGLESv2.dylib */
    const char* kind;         /* window, borderless, space, fullscreen, exclusive */
    const char* runs;         /* comma list of sub-runs, NULL = all          */
    const char* tag;          /* file name tag for this condition             */
    const char* backend;      /* ANGLE back end: default, metal, gl           */
    double      hz;           /* exclusive mode refresh, 0 = the desktop's    */
    double      seconds;      /* links: length                                */
    int         frames;       /* frames per run                               */
    int         reps;         /* presents per quantization cell               */
    int         drawables;    /* maximumDrawableCount 2 or 3                  */
    int         sync_off;     /* displaySyncEnabled = NO                      */
    int         interval;     /* EGL swap interval                            */
    int         bits;         /* 8, 10 or 16 (half float)                     */
    int         heavy;        /* fragment shader iterations, 0 = off          */
    int         keys;         /* input: key presses                           */
    int         hud;          /* MTL_HUD_ENABLED was set by the run script    */
    int         allow_locked; /* open windows on a locked screen (smoke runs) */
} probe_opts;

extern probe_opts g_opt;

/* ---- time ---------------------------------------------------------------
 * Every time the probe records is mach_absolute_time() scaled to ns by
 * mach_timebase_info ("mach ns"), the base ysp/rt.h v0.10.0 uses on macOS.
 * Apple's CFTimeInterval times (presentedTime, CACurrentMediaTime,
 * GPUStartTime, CADisplayLink, NSEvent.timestamp) are taken as seconds of
 * that clock: mach ns = s * 1e9. CVTimeStamp.hostTime is in mach ticks.
 * The env section checks each of these against mach_absolute_time instead
 * of assuming it. */
int64_t mach_ns_now(void);
int64_t mach_ticks_to_ns(uint64_t ticks);
int64_t secs_to_ns(double s);           /* CFTimeInterval to mach ns       */
double  ns_to_secs(int64_t ns);

typedef struct corr {
    int64_t other;    /* the other clock, ns                                 */
    int64_t mach;     /* mach ns at the middle of the tightest bracket      */
    int64_t width;    /* the bracket                                         */
} corr;
typedef int64_t (*clock_fn)(void);
corr    correlate(clock_fn other, int tries);
int64_t mach_cont_ns_now(void);
int64_t sdl_ticks_now(void);
int64_t clock_ns(int id);              /* clock_gettime(id)                 */

/* ---- output -------------------------------------------------------------- */
extern FILE* g_sum;                     /* summary.txt, appended            */
void  P(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
FILE* open_csv(const char* name, const char* header);
void  out_init(const char* section);
const char* tagged(const char* base);  /* base + "_" + tag                  */

/* ---- statistics ---------------------------------------------------------- */
typedef struct stats { int n; double p01, p50, p99, min, max, mean, sd; } stats;
stats stats_of(const double* v, int n); /* sorts a copy                      */
void  P_stats(const char* name, const double* v, int n, const char* unit);

typedef struct grid {
    int    ok, n, missed;
    double t0;        /* mach ns of index 0                                  */
    double period;    /* ns                                                  */
    double res_p50, res_p99, res_max; /* |residual| us                       */
} grid;
grid    grid_fit(const int64_t* t, int n, double nominal_ns);
double  grid_index(const grid* g, int64_t t);      /* fractional index       */
int64_t grid_round(const grid* g, int64_t t);      /* nearest index          */
double  grid_residual_us(const grid* g, int64_t t);

/* ---- vblank sources ------------------------------------------------------ */
typedef struct cv_rec {
    int64_t now_host;     /* inNow.hostTime, mach ns                         */
    int64_t out_host;     /* inOutputTime.hostTime                           */
    int64_t video_time;   /* inNow.videoTime                                 */
    int32_t video_scale;
    int64_t video_period; /* inNow.videoRefreshPeriod                        */
    double  rate_scalar;
    int64_t arrive;       /* mach ns at the callback                         */
} cv_rec;
typedef struct cv_link {
    void*   link;         /* CVDisplayLinkRef                                */
    cv_rec* rec;
    int     cap;
    volatile int n;
    double  nominal_ns, actual_ns;
} cv_link;
bool    cv_start(cv_link* c, uint32_t display, int cap);
void    cv_stop(cv_link* c);
/* The newest vblank and the period, for planning targets while running. */
bool    cv_latest(const cv_link* c, int64_t* vblank, double* period);
grid    cv_grid(const cv_link* c, int64_t from, int64_t to);
void    cv_report(const cv_link* c, const char* name);

/* ---- EGL and GL, loaded at run time ------------------------------------- */
#define PROBE_EGL_PLATFORM_ANGLE_ANGLE              0x3202
#define PROBE_EGL_PLATFORM_ANGLE_TYPE_ANGLE         0x3203
#define PROBE_EGL_PLATFORM_ANGLE_TYPE_OPENGL_ANGLE  0x320D
#define PROBE_EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE   0x3489
#define PROBE_EGL_PLATFORM_ANGLE_DEVICE_ID_HIGH     0x34D6
#define PROBE_EGL_PLATFORM_ANGLE_DEVICE_ID_LOW      0x34D7
#define PROBE_EGL_METAL_DEVICE_ANGLE                0x34A6
#define PROBE_EGL_METAL_TEXTURE_ANGLE               0x34A7
#define PROBE_EGL_IOSURFACE_ANGLE                   0x3454
#define PROBE_EGL_IOSURFACE_PLANE_ANGLE             0x345A
#define PROBE_EGL_TEXTURE_RECTANGLE_ANGLE           0x345B
#define PROBE_EGL_TEXTURE_TYPE_ANGLE                0x345C
#define PROBE_EGL_TEXTURE_INTERNAL_FORMAT_ANGLE     0x345D
#define PROBE_EGL_IOSURFACE_USAGE_HINT_ANGLE        0x348A
#define PROBE_EGL_BIND_TO_TEXTURE_TARGET_ANGLE      0x348D
#define PROBE_EGL_SYNC_METAL_SHARED_EVENT_ANGLE     0x34D8
#define PROBE_EGL_SYNC_METAL_SHARED_EVENT_OBJECT    0x34D9
#define PROBE_EGL_SYNC_METAL_SHARED_EVENT_VALUE_LO  0x34DA
#define PROBE_EGL_SYNC_METAL_SHARED_EVENT_VALUE_HI  0x34DB

typedef struct egl_api {
    void* lib;
    PFNEGLGETPROCADDRESSPROC         GetProcAddress;
    PFNEGLGETPLATFORMDISPLAYEXTPROC  GetPlatformDisplayEXT;
    PFNEGLINITIALIZEPROC             Initialize;
    PFNEGLTERMINATEPROC              Terminate;
    PFNEGLQUERYSTRINGPROC            QueryString;
    PFNEGLGETCONFIGSPROC             GetConfigs;
    PFNEGLCHOOSECONFIGPROC           ChooseConfig;
    PFNEGLGETCONFIGATTRIBPROC        GetConfigAttrib;
    PFNEGLCREATECONTEXTPROC          CreateContext;
    PFNEGLDESTROYCONTEXTPROC         DestroyContext;
    PFNEGLCREATEPBUFFERSURFACEPROC   CreatePbufferSurface;
    PFNEGLCREATEPBUFFERFROMCLIENTBUFFERPROC CreatePbufferFromClientBuffer;
    PFNEGLDESTROYSURFACEPROC         DestroySurface;
    PFNEGLMAKECURRENTPROC            MakeCurrent;
    PFNEGLSWAPBUFFERSPROC            SwapBuffers;
    PFNEGLSWAPINTERVALPROC           SwapInterval;
    PFNEGLSURFACEATTRIBPROC          SurfaceAttrib;
    PFNEGLQUERYSURFACEPROC           QuerySurface;
    PFNEGLGETERRORPROC               GetError;
    PFNEGLCREATESYNCKHRPROC          CreateSyncKHR;
    PFNEGLDESTROYSYNCKHRPROC         DestroySyncKHR;
    PFNEGLCLIENTWAITSYNCKHRPROC      ClientWaitSyncKHR;
    PFNEGLQUERYDISPLAYATTRIBEXTPROC  QueryDisplayAttribEXT;
    PFNEGLQUERYDEVICEATTRIBEXTPROC   QueryDeviceAttribEXT;
    PFNEGLQUERYDEVICESTRINGEXTPROC   QueryDeviceStringEXT;
} egl_api;
extern egl_api E;
bool egl_load(const char* dir);          /* dlopen ANGLE's libEGL             */
void* egl_proc(const char* name);
const char* egl_err_name(EGLint e);
bool has_ext(const char* list, const char* name);
EGLDisplay angle_display(const char* backend); /* initialized, or NO_DISPLAY */
void egl_print_config(EGLDisplay d, EGLConfig c, const char* prefix);

typedef struct gl_api {
    void         (*ClearColor)(float, float, float, float);
    void         (*Clear)(unsigned);
    void         (*Enable)(unsigned);
    void         (*Disable)(unsigned);
    void         (*Scissor)(int, int, int, int);
    void         (*Viewport)(int, int, int, int);
    void         (*Flush)(void);
    void         (*Finish)(void);
    const unsigned char* (*GetString)(unsigned);
    void         (*GetIntegerv)(unsigned, int*);
    void         (*ReadPixels)(int, int, int, int, unsigned, unsigned, void*);
    unsigned     (*GetError)(void);
    unsigned     (*CreateShader)(unsigned);
    void         (*ShaderSource)(unsigned, int, const char* const*, const int*);
    void         (*CompileShader)(unsigned);
    void         (*GetShaderiv)(unsigned, unsigned, int*);
    void         (*GetShaderInfoLog)(unsigned, int, int*, char*);
    unsigned     (*CreateProgram)(void);
    void         (*AttachShader)(unsigned, unsigned);
    void         (*LinkProgram)(unsigned);
    void         (*GetProgramiv)(unsigned, unsigned, int*);
    void         (*UseProgram)(unsigned);
    int          (*GetUniformLocation)(unsigned, const char*);
    void         (*Uniform1f)(int, float);
    void         (*Uniform1i)(int, int);
    void         (*DrawArrays)(unsigned, int, int);
    void         (*GenVertexArrays)(int, unsigned*);
    void         (*BindVertexArray)(unsigned);
} gl_api;
extern gl_api G;
bool gl_load(void* (*get)(const char*));
/* A full-screen triangle; the shader is a ramp in x (mode 0) or a loop of
 * `iters` dependent operations (mode 1, GPU load). */
unsigned gl_program(int mode);
void gl_draw_fullscreen(unsigned prog, int iters, float level);
void gl_print_strings(const char* prefix);

#define PGL_COLOR_BUFFER_BIT        0x4000
#define PGL_SCISSOR_TEST            0x0C11
#define PGL_VENDOR                  0x1F00
#define PGL_RENDERER                0x1F01
#define PGL_VERSION                 0x1F02
#define PGL_EXTENSIONS              0x1F03
#define PGL_SHADING_LANGUAGE_VERSION 0x8B8C
#define PGL_RGBA                    0x1908
#define PGL_UNSIGNED_BYTE           0x1401
#define PGL_FLOAT                   0x1406
#define PGL_HALF_FLOAT              0x140B
#define PGL_UNSIGNED_INT_2_10_10_10_REV 0x8368
#define PGL_IMPL_READ_FORMAT        0x8B9B
#define PGL_IMPL_READ_TYPE          0x8B9A
#define PGL_TRIANGLES               0x0004
#define PGL_RED_BITS                0x0D52

/* ---- windows ------------------------------------------------------------- */
/* Opens an SDL window of g_opt.kind; with `gl` it carries SDL_WINDOW_OPENGL,
 * else SDL_WINDOW_METAL. Waits for fullscreen transitions to settle and
 * prints what it got. */
SDL_Window* open_window(bool gl, const char* title);
void        close_window(SDL_Window* w);
void        pump_for(double seconds);
void        print_window(SDL_Window* w);
bool        quit_requested(void);
/* The console session's screen lock; a locked screen occludes every window,
 * so no frame reaches the panel. */
bool        session_locked(void);
void        print_session(void);

/* ---- sections ------------------------------------------------------------ */
int sec_env(void);
int sec_egl(void);       /* 1a: SDL3's EGL path                             */
int sec_share(void);     /* 1b: header-owned CAMetalLayer and ANGLE         */
int sec_links(void);     /* 2: timing sources                               */
int sec_present(void);   /* 3 and 4: present timing per window kind         */
int sec_bits(void);      /* 5: 10-bit and half float                        */
int sec_input(void);     /* 6: key timestamps                               */
int sec_link(void);      /* CAMetalDisplayLink's pacing (macOS 14+)         */

#endif
