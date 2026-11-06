/* screen_x11_probe.c - display timing on X11, for the GLX backend of
 * ysp/screen.h. docs/screen_x11_probe.md says how to build, run and read it.
 *
 * Linux only. Links libX11, libxcb, xcb-present, xcb-randr and libGL. EGL,
 * Vulkan, SDL3 and ANGLE are loaded at run time and skipped with a message
 * when they are missing; Vulkan also needs its headers at build time.
 *
 * MIT No Attribution, as the rest of ysp.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <xcb/xcb.h>
#include <xcb/present.h>
#include <xcb/randr.h>
#include <GL/gl.h>
#include <GL/glx.h>

#if defined(__has_include)
#  if __has_include(<vulkan/vulkan.h>) && !defined(PROBE_NO_VULKAN)
#    define PROBE_VULKAN 1
#  endif
#  if __has_include(<SDL3/SDL.h>)
#    define PROBE_SDL3_HEADERS 1
#  endif
#endif
#ifndef PROBE_VULKAN
#  define PROBE_VULKAN 0
#endif
#if PROBE_VULKAN
#  define VK_NO_PROTOTYPES
#  define VK_USE_PLATFORM_XLIB_KHR
#  include <vulkan/vulkan.h>
#endif

/* GL and GLX values that older headers may lack. */
#ifndef GL_VERTEX_SHADER
#  define GL_VERTEX_SHADER 0x8B31
#  define GL_FRAGMENT_SHADER 0x8B30
#  define GL_COMPILE_STATUS 0x8B81
#  define GL_LINK_STATUS 0x8B82
#endif
#ifndef GL_SHADING_LANGUAGE_VERSION
#  define GL_SHADING_LANGUAGE_VERSION 0x8B8C
#endif
typedef char probe_GLchar;
#define P_GLX_CONTEXT_MAJOR_VERSION_ARB 0x2091
#define P_GLX_CONTEXT_MINOR_VERSION_ARB 0x2092
#define P_GLX_CONTEXT_PROFILE_MASK_ARB 0x9126
#define P_GLX_CONTEXT_ES2_PROFILE_BIT_EXT 0x0004
#define P_GLX_BUFFER_SWAP_COMPLETE_INTEL_MASK 0x04000000
#define P_GLX_EXCHANGE_COMPLETE_INTEL 0x8180
#define P_GLX_COPY_COMPLETE_INTEL 0x8181
#define P_GLX_FLIP_COMPLETE_INTEL 0x8182
#define P_GLX_BufferSwapComplete 1

/* GLX_INTEL_swap_event's event, as libGL converts it. */
typedef struct {
    int type;
    unsigned long serial;
    Bool send_event;
    Display* display;
    GLXDrawable drawable;
    int event_type;
    int64_t ust, msc, sbc;
} probe_swap_complete;

/* EGL is loaded at run time, so it needs no headers: the types and values
 * below are the Khronos ABI. */
typedef int32_t p_EGLint;
typedef unsigned p_EGLBoolean;
typedef unsigned p_EGLenum;
typedef intptr_t p_EGLAttrib;
#define P_EGL_NONE 0x3038
#define P_EGL_ALPHA_SIZE 0x3021
#define P_EGL_BLUE_SIZE 0x3022
#define P_EGL_GREEN_SIZE 0x3023
#define P_EGL_RED_SIZE 0x3024
#define P_EGL_CONFIG_ID 0x3028
#define P_EGL_NATIVE_VISUAL_ID 0x302E
#define P_EGL_SURFACE_TYPE 0x3033
#define P_EGL_RENDERABLE_TYPE 0x3040
#define P_EGL_VENDOR 0x3053
#define P_EGL_VERSION 0x3054
#define P_EGL_EXTENSIONS 0x3055
#define P_EGL_CLIENT_APIS 0x308D
#define P_EGL_CONTEXT_MAJOR_VERSION 0x3098
#define P_EGL_CONTEXT_MINOR_VERSION 0x30FB
#define P_EGL_OPENGL_ES_API 0x30A0
#define P_EGL_WINDOW_BIT 0x0004
#define P_EGL_OPENGL_ES3_BIT 0x0040
#define P_EGL_PLATFORM_X11_KHR 0x31D5
#define P_EGL_PLATFORM_X11_SCREEN_KHR 0x31D6
#define P_EGL_PLATFORM_ANGLE_ANGLE 0x3202
#define P_EGL_PLATFORM_ANGLE_TYPE_ANGLE 0x3203
#define P_EGL_PLATFORM_ANGLE_TYPE_DEFAULT_ANGLE 0x3206
#define P_EGL_PLATFORM_ANGLE_TYPE_OPENGL_ANGLE 0x320D
#define P_EGL_PLATFORM_ANGLE_TYPE_VULKAN_ANGLE 0x3450
#define P_EGL_PLATFORM_ANGLE_NATIVE_PLATFORM_TYPE_ANGLE 0x348F

/* SDL3 is loaded at run time too. Values from SDL 3.2's headers; checked
 * against them when they are installed. */
#define P_SDL_INIT_VIDEO 0x00000020u
#define P_SDL_GL_CONTEXT_MAJOR_VERSION 17
#define P_SDL_GL_CONTEXT_MINOR_VERSION 18
#define P_SDL_GL_CONTEXT_PROFILE_MASK 20
#define P_SDL_GL_CONTEXT_PROFILE_ES 0x0004
#define P_SDL_WINDOWPOS_CENTERED_DISPLAY(x) ((int64_t)(0x2FFF0000u | (x)))
#if defined(PROBE_SDL3_HEADERS)
#  include <SDL3/SDL_init.h>
#  include <SDL3/SDL_video.h>
_Static_assert(SDL_INIT_VIDEO == P_SDL_INIT_VIDEO, "SDL_INIT_VIDEO");
_Static_assert(SDL_GL_CONTEXT_MAJOR_VERSION == P_SDL_GL_CONTEXT_MAJOR_VERSION, "SDL_GLAttr");
_Static_assert(SDL_GL_CONTEXT_MINOR_VERSION == P_SDL_GL_CONTEXT_MINOR_VERSION, "SDL_GLAttr");
_Static_assert(SDL_GL_CONTEXT_PROFILE_MASK == P_SDL_GL_CONTEXT_PROFILE_MASK, "SDL_GLAttr");
_Static_assert(SDL_GL_CONTEXT_PROFILE_ES == P_SDL_GL_CONTEXT_PROFILE_ES, "SDL_GLProfile");
#endif

typedef void (*vfn)(void);

/* ------------------------------------------------------------------ */
/* Options and output                                                   */

typedef struct {
    int frames, vblanks, clock_samples;
    int screen;          /* -1: every screen */
    int both_only, no_both;
    int load, gpu_iters, late_every;
    int wm, quick, no_rt, glx_compat, timeout_s;
    int skip_env, skip_egl, skip_sdl, skip_vk;
    double sleep_margin_ms;
    const char* out;
    const char* angle_dir;
} options;

static options O = {600, 300, 200, -1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1800, 0, 0, 0, 0, 1.0, NULL, NULL};

static FILE *g_sum, *g_swaps, *g_vbl, *g_clk, *g_ext;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile sig_atomic_t g_stop;
static char g_base_name[256];
static int g_nscreens;

static void say(const char* fmt, ...) {
    va_list a;
    pthread_mutex_lock(&g_lock);
    va_start(a, fmt);
    vfprintf(stdout, fmt, a);
    va_end(a);
    if (g_sum) {
        va_start(a, fmt);
        vfprintf(g_sum, fmt, a);
        va_end(a);
        fflush(g_sum);
    }
    fflush(stdout);
    pthread_mutex_unlock(&g_lock);
}

static void ext_dump(const char* what, const char* list) {
    if (!g_ext) return;
    pthread_mutex_lock(&g_lock);
    fprintf(g_ext, "== %s\n", what);
    if (list) {
        const char* p = list;
        while (*p) {
            while (*p == ' ') p++;
            const char* e = p;
            while (*e && *e != ' ') e++;
            if (e > p) fprintf(g_ext, "%.*s\n", (int)(e - p), p);
            p = e;
        }
    }
    fprintf(g_ext, "\n");
    fflush(g_ext);
    pthread_mutex_unlock(&g_lock);
}

static int has_ext(const char* list, const char* name) {
    if (!list || !name) return 0;
    size_t n = strlen(name);
    const char* p = list;
    while ((p = strstr(p, name)) != NULL) {
        if ((p == list || p[-1] == ' ') && (p[n] == ' ' || p[n] == 0)) return 1;
        p += n;
    }
    return 0;
}

static const char* yesno(int v) { return v ? "yes" : "no"; }

/* ------------------------------------------------------------------ */
/* Time                                                                 */

static int64_t clk(clockid_t id) {
    struct timespec t;
    clock_gettime(id, &t);
    return (int64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}
static int64_t now_ns(void) { return clk(CLOCK_MONOTONIC); }

static void sleep_until(int64_t t) {
    struct timespec ts;
    ts.tv_sec = t / 1000000000;
    ts.tv_nsec = t % 1000000000;
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR && !g_stop) {}
}

static void spin_for(int64_t d) {
    int64_t e = now_ns() + d;
    while (now_ns() < e) {}
}

/* ------------------------------------------------------------------ */
/* Statistics                                                           */

typedef struct { int n; double p50, p99, min, max, mean; } dist;

static int cmpd(const void* a, const void* b) {
    double x = *(const double*)a, y = *(const double*)b;
    return x < y ? -1 : x > y;
}

static double qsorted(const double* v, int n, double p) {
    double i = p * (n - 1);
    int k = (int)i;
    double f = i - k;
    return k + 1 < n ? v[k] * (1 - f) + v[k + 1] * f : v[k];
}

static dist dist_of(double* v, int n) {
    dist d;
    d.n = n;
    if (n <= 0) {
        d.p50 = d.p99 = d.min = d.max = d.mean = NAN;
        return d;
    }
    qsort(v, (size_t)n, sizeof *v, cmpd);
    double s = 0;
    for (int i = 0; i < n; i++) s += v[i];
    d.min = v[0];
    d.max = v[n - 1];
    d.p50 = qsorted(v, n, 0.5);
    d.p99 = qsorted(v, n, 0.99);
    d.mean = s / n;
    return d;
}

/* Least squares y = a + b x. */
static int fit_line(const double* x, const double* y, int n, double* a, double* b) {
    if (n < 2) return 0;
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (int i = 0; i < n; i++) {
        sx += x[i];
        sy += y[i];
    }
    double mx = sx / n, my = sy / n;
    for (int i = 0; i < n; i++) {
        sxx += (x[i] - mx) * (x[i] - mx);
        sxy += (x[i] - mx) * (y[i] - my);
    }
    if (sxx <= 0) return 0;
    *b = sxy / sxx;
    *a = my - *b * mx;
    return 1;
}

/* ------------------------------------------------------------------ */
/* X helpers                                                            */

static __thread int t_xerr;
static int xerr_handler(Display* d, XErrorEvent* e) {
    (void)d;
    t_xerr = e->error_code ? e->error_code : 1;
    return 0;
}

static Atom A(Display* d, const char* n) { return XInternAtom(d, n, False); }

static void prop_card32(Display* d, Window w, const char* n, unsigned long v) {
    XChangeProperty(d, w, A(d, n), XA_CARDINAL, 32, PropModeReplace, (unsigned char*)&v, 1);
}

static int prop_long(Display* d, Window w, const char* n, long* v) {
    Atom type;
    int fmt;
    unsigned long nitems, after;
    unsigned char* data = NULL;
    Atom a = XInternAtom(d, n, True);
    if (a == None) return 0;
    if (XGetWindowProperty(d, w, a, 0, 1, False, AnyPropertyType, &type, &fmt, &nitems, &after, &data) != Success
        || !data || nitems < 1) {
        if (data) XFree(data);
        return 0;
    }
    *v = fmt == 32 ? *(long*)data : fmt == 16 ? *(short*)data : *(char*)data;
    XFree(data);
    return 1;
}

static int prop_text(Display* d, Window w, const char* n, char* out, int cap) {
    Atom type;
    int fmt;
    unsigned long nitems, after;
    unsigned char* data = NULL;
    out[0] = 0;
    Atom a = XInternAtom(d, n, True);
    if (a == None) return 0;
    if (XGetWindowProperty(d, w, a, 0, 256, False, AnyPropertyType, &type, &fmt, &nitems, &after, &data) != Success
        || !data || fmt != 8) {
        if (data) XFree(data);
        return 0;
    }
    snprintf(out, (size_t)cap, "%.*s", (int)nitems, (char*)data);
    XFree(data);
    return 1;
}

static void base_display_name(const char* s, char* out, size_t cap) {
    snprintf(out, cap, "%s", s);
    char* colon = strrchr(out, ':');
    if (colon) {
        char* dot = strchr(colon, '.');
        if (dot) *dot = 0;
    }
}

/* ------------------------------------------------------------------ */
/* RandR                                                                */

typedef struct {
    int active, x, y, w, h, gamma;
    uint32_t id, dot, ht, vt, flags;
    double hz;
    char outs[96], mode_name[32];
} crtc_t;
typedef struct { int n; crtc_t c[16]; } scr_t;
static scr_t g_scr[16];

static double mode_hz(const xcb_randr_mode_info_t* m) {
    if (!m->htotal || !m->vtotal) return 0;
    double v = m->vtotal;
    if (m->mode_flags & XCB_RANDR_MODE_FLAG_DOUBLE_SCAN) v *= 2;
    if (m->mode_flags & XCB_RANDR_MODE_FLAG_INTERLACE) v /= 2;
    return (double)m->dot_clock / ((double)m->htotal * v);
}

static char* atom_name(xcb_connection_t* xc, xcb_atom_t a, char* buf, int cap) {
    xcb_get_atom_name_reply_t* r = xcb_get_atom_name_reply(xc, xcb_get_atom_name(xc, a), NULL);
    if (!r) {
        snprintf(buf, (size_t)cap, "atom%u", a);
        return buf;
    }
    int n = xcb_get_atom_name_name_length(r);
    if (n > cap - 1) n = cap - 1;
    memcpy(buf, xcb_get_atom_name_name(r), (size_t)n);
    buf[n] = 0;
    free(r);
    return buf;
}

static void edid_describe(const uint8_t* e, int n, char* out, int cap) {
    if (n < 128 || e[0] != 0 || e[1] != 0xFF) {
        snprintf(out, (size_t)cap, "%d bytes", n);
        return;
    }
    char mfg[4] = {(char)('A' - 1 + ((e[8] >> 2) & 31)), (char)('A' - 1 + (((e[8] & 3) << 3) | (e[9] >> 5))),
                   (char)('A' - 1 + (e[9] & 31)), 0};
    unsigned prod = (unsigned)(e[10] | (e[11] << 8));
    char name[14] = "";
    for (int d = 54; d + 18 <= 126; d += 18)
        if (e[d] == 0 && e[d + 1] == 0 && e[d + 3] == 0xFC) {
            int k;
            for (k = 0; k < 13 && e[d + 5 + k] != 0x0A; k++) name[k] = (char)e[d + 5 + k];
            name[k] = 0;
        }
    snprintf(out, (size_t)cap, "%d bytes, maker %s, product 0x%04x, name \"%s\"", n, mfg, prod, name);
}

static void print_output_props(xcb_connection_t* xc, xcb_randr_output_t o) {
    xcb_randr_list_output_properties_reply_t* l =
        xcb_randr_list_output_properties_reply(xc, xcb_randr_list_output_properties(xc, o), NULL);
    if (!l) return;
    xcb_atom_t* atoms = xcb_randr_list_output_properties_atoms(l);
    int na = xcb_randr_list_output_properties_atoms_length(l);
    for (int i = 0; i < na; i++) {
        char name[128], val[512], tn[64], qs[256];
        atom_name(xc, atoms[i], name, sizeof name);
        val[0] = qs[0] = 0;
        xcb_randr_get_output_property_reply_t* r = xcb_randr_get_output_property_reply(
            xc, xcb_randr_get_output_property(xc, o, atoms[i], XCB_ATOM_ANY, 0, 128, 0, 0), NULL);
        if (r) {
            const uint8_t* data = xcb_randr_get_output_property_data(r);
            int ni = (int)r->num_items, fmt = r->format;
            if (!strcmp(name, "EDID")) {
                edid_describe(data, ni, val, sizeof val);
            } else if (r->type == XCB_ATOM_INTEGER || r->type == XCB_ATOM_CARDINAL) {
                int len = 0;
                for (int k = 0; k < ni && k < 8; k++) {
                    long v = fmt == 32 ? (r->type == XCB_ATOM_INTEGER ? (long)((const int32_t*)data)[k]
                                                                       : (long)((const uint32_t*)data)[k])
                           : fmt == 16 ? (long)((const int16_t*)data)[k]
                                       : (long)((const int8_t*)data)[k];
                    len += snprintf(val + len, sizeof val - (size_t)len, "%s%ld", k ? " " : "", v);
                }
                if (ni > 8) snprintf(val + len, sizeof val - (size_t)len, " ... (%d items)", ni);
            } else if (r->type == XCB_ATOM_ATOM && fmt == 32) {
                int len = 0;
                for (int k = 0; k < ni && k < 4; k++)
                    len += snprintf(val + len, sizeof val - (size_t)len, "%s%s", k ? " " : "",
                                    atom_name(xc, ((const uint32_t*)data)[k], tn, sizeof tn));
            } else if (r->type == XCB_ATOM_STRING) {
                snprintf(val, sizeof val, "\"%.*s\"", ni, (const char*)data);
            } else {
                snprintf(val, sizeof val, "%d items, format %d, type %s", ni, fmt,
                         atom_name(xc, r->type, tn, sizeof tn));
            }
            xcb_randr_query_output_property_reply_t* q =
                xcb_randr_query_output_property_reply(xc, xcb_randr_query_output_property(xc, o, atoms[i]), NULL);
            if (q) {
                int32_t* vv = xcb_randr_query_output_property_valid_values(q);
                int nv = xcb_randr_query_output_property_valid_values_length(q);
                if (q->range && nv == 2) snprintf(qs, sizeof qs, " (range %d to %d)", vv[0], vv[1]);
                else if (nv > 0) {
                    int len = snprintf(qs, sizeof qs, " (allowed:");
                    for (int k = 0; k < nv && k < 8 && len < (int)sizeof qs - 40; k++) {
                        if (r->type == XCB_ATOM_ATOM)
                            len += snprintf(qs + len, sizeof qs - (size_t)len, " %s",
                                            atom_name(xc, (xcb_atom_t)vv[k], tn, sizeof tn));
                        else
                            len += snprintf(qs + len, sizeof qs - (size_t)len, " %d", vv[k]);
                    }
                    snprintf(qs + len, sizeof qs - (size_t)len, ")");
                }
                free(q);
            }
            free(r);
        }
        say("      %s = %s%s\n", name, val, qs);
    }
    free(l);
}

static void randr_scan(const char* dname, int print) {
    xcb_connection_t* xc = xcb_connect(dname, NULL);
    if (!xc || xcb_connection_has_error(xc)) {
        say("RandR: cannot connect to %s\n", dname);
        if (xc) xcb_disconnect(xc);
        return;
    }
    const xcb_query_extension_reply_t* ext = xcb_get_extension_data(xc, &xcb_randr_id);
    if (!ext || !ext->present) {
        say("RandR: not present on the server\n");
        xcb_disconnect(xc);
        return;
    }
    xcb_randr_query_version_reply_t* rv = xcb_randr_query_version_reply(xc, xcb_randr_query_version(xc, 1, 6), NULL);
    unsigned vmaj = rv ? rv->major_version : 0, vmin = rv ? rv->minor_version : 0;
    free(rv);
    if (print) say("RandR %u.%u\n", vmaj, vmin);
    xcb_screen_iterator_t it = xcb_setup_roots_iterator(xcb_get_setup(xc));
    for (int s = 0; it.rem && s < 16; xcb_screen_next(&it), s++) {
        xcb_window_t root = it.data->root;
        scr_t* sc = &g_scr[s];
        sc->n = 0;
        xcb_randr_get_screen_resources_current_reply_t* res = xcb_randr_get_screen_resources_current_reply(
            xc, xcb_randr_get_screen_resources_current(xc, root), NULL);
        if (!res) continue;
        xcb_randr_crtc_t* crtcs = xcb_randr_get_screen_resources_current_crtcs(res);
        int ncrtc = xcb_randr_get_screen_resources_current_crtcs_length(res);
        xcb_randr_output_t* outs = xcb_randr_get_screen_resources_current_outputs(res);
        int nout = xcb_randr_get_screen_resources_current_outputs_length(res);
        xcb_randr_mode_info_t* modes = xcb_randr_get_screen_resources_current_modes(res);
        int nmode = xcb_randr_get_screen_resources_current_modes_length(res);
        uint8_t* names = xcb_randr_get_screen_resources_current_names(res);
        int* name_off = calloc((size_t)nmode + 1, sizeof(int));
        for (int m = 0, off = 0; m < nmode; m++) {
            name_off[m] = off;
            off += modes[m].name_len;
        }
        char (*oname)[64] = calloc((size_t)nout + 1, sizeof *oname);
        xcb_randr_get_output_info_reply_t** oinfo = calloc((size_t)nout + 1, sizeof *oinfo);
        for (int o = 0; o < nout; o++) {
            oinfo[o] = xcb_randr_get_output_info_reply(
                xc, xcb_randr_get_output_info(xc, outs[o], res->config_timestamp), NULL);
            if (oinfo[o])
                snprintf(oname[o], 64, "%.*s", xcb_randr_get_output_info_name_length(oinfo[o]),
                         (const char*)xcb_randr_get_output_info_name(oinfo[o]));
        }
        if (print) say("\nScreen %d (root 0x%x): %d CRTCs, %d outputs, %d modes\n", s, root, ncrtc, nout, nmode);
        for (int i = 0; i < ncrtc && sc->n < 16; i++) {
            xcb_randr_get_crtc_info_reply_t* ci =
                xcb_randr_get_crtc_info_reply(xc, xcb_randr_get_crtc_info(xc, crtcs[i], res->config_timestamp), NULL);
            if (!ci) continue;
            crtc_t* c = &sc->c[sc->n++];
            memset(c, 0, sizeof *c);
            c->id = crtcs[i];
            c->active = ci->mode != 0;
            c->x = ci->x;
            c->y = ci->y;
            c->w = ci->width;
            c->h = ci->height;
            for (int m = 0; m < nmode; m++)
                if (modes[m].id == ci->mode) {
                    c->dot = modes[m].dot_clock;
                    c->ht = modes[m].htotal;
                    c->vt = modes[m].vtotal;
                    c->flags = modes[m].mode_flags;
                    c->hz = mode_hz(&modes[m]);
                    snprintf(c->mode_name, sizeof c->mode_name, "%.*s", modes[m].name_len, (const char*)names + name_off[m]);
                }
            xcb_randr_output_t* co = xcb_randr_get_crtc_info_outputs(ci);
            int nco = xcb_randr_get_crtc_info_outputs_length(ci);
            int len = 0;
            for (int k = 0; k < nco; k++)
                for (int o = 0; o < nout; o++)
                    if (outs[o] == co[k])
                        len += snprintf(c->outs + len, sizeof c->outs - (size_t)len, "%s%s", len ? "," : "", oname[o]);
            xcb_randr_get_crtc_gamma_size_reply_t* g =
                xcb_randr_get_crtc_gamma_size_reply(xc, xcb_randr_get_crtc_gamma_size(xc, crtcs[i]), NULL);
            c->gamma = g ? g->size : -1;
            free(g);
            if (print) {
                if (c->active)
                    say("  CRTC %u: %dx%d+%d+%d on %s, mode \"%s\", dot clock %u Hz, htotal %u, vtotal %u, flags 0x%x,"
                        " refresh %.6f Hz (period %.3f us), rotation %u, gamma ramp %d entries\n",
                        c->id, c->w, c->h, c->x, c->y, c->outs[0] ? c->outs : "-", c->mode_name, c->dot, c->ht, c->vt,
                        c->flags, c->hz, c->hz > 0 ? 1e6 / c->hz : 0.0, ci->rotation, c->gamma);
                else
                    say("  CRTC %u: off, gamma ramp %d entries\n", c->id, c->gamma);
            }
            free(ci);
        }
        if (print) {
            for (int o = 0; o < nout; o++) {
                if (!oinfo[o]) continue;
                const char* cs = oinfo[o]->connection == 0 ? "connected" : oinfo[o]->connection == 1 ? "disconnected" : "unknown";
                say("  output %s: %s, CRTC %u, %u x %u mm\n", oname[o], cs, oinfo[o]->crtc, oinfo[o]->mm_width,
                    oinfo[o]->mm_height);
                if (oinfo[o]->connection == 0) print_output_props(xc, outs[o]);
            }
            if (vmaj > 1 || (vmaj == 1 && vmin >= 4)) {
                xcb_randr_get_providers_reply_t* pr =
                    xcb_randr_get_providers_reply(xc, xcb_randr_get_providers(xc, root), NULL);
                if (pr) {
                    xcb_randr_provider_t* p = xcb_randr_get_providers_providers(pr);
                    int np = xcb_randr_get_providers_providers_length(pr);
                    for (int k = 0; k < np; k++) {
                        xcb_randr_get_provider_info_reply_t* pi = xcb_randr_get_provider_info_reply(
                            xc, xcb_randr_get_provider_info(xc, p[k], res->config_timestamp), NULL);
                        if (!pi) continue;
                        say("  provider %u: \"%.*s\", capabilities 0x%x (the name tells the DDX: \"modesetting\" or the GPU's name for amdgpu)\n",
                            p[k], xcb_randr_get_provider_info_name_length(pi),
                            (const char*)xcb_randr_get_provider_info_name(pi), pi->capabilities);
                        free(pi);
                    }
                    free(pr);
                }
            }
        }
        for (int o = 0; o < nout; o++) free(oinfo[o]);
        free(oinfo);
        free(oname);
        free(name_off);
        free(res);
    }
    xcb_disconnect(xc);
}

/* The mode period of the CRTC that holds most of a rectangle. */
static double rect_period_ns(int scr, int x, int y, int w, int h, const char** outs) {
    if (scr < 0 || scr >= 16) return 0;
    const scr_t* sc = &g_scr[scr];
    long best = -1;
    double per = 0;
    for (int i = 0; i < sc->n; i++) {
        const crtc_t* c = &sc->c[i];
        if (!c->active || c->hz <= 0) continue;
        long ix = (long)(x + w < c->x + c->w ? x + w : c->x + c->w) - (x > c->x ? x : c->x);
        long iy = (long)(y + h < c->y + c->h ? y + h : c->y + c->h) - (y > c->y ? y : c->y);
        long area = ix > 0 && iy > 0 ? ix * iy : 0;
        if (area > best) {
            best = area;
            per = 1e9 / c->hz;
            if (outs) *outs = c->outs;
        }
    }
    return per;
}

static int active_crtcs(int scr) {
    int n = 0;
    if (scr < 0 || scr >= 16) return 0;
    for (int i = 0; i < g_scr[scr].n; i++) n += g_scr[scr].c[i].active;
    return n;
}

/* ------------------------------------------------------------------ */
/* GL entry points, loaded per context so EGL and ANGLE get their own.  */

typedef struct {
    void (*Viewport)(GLint, GLint, GLsizei, GLsizei);
    void (*ClearColor)(GLfloat, GLfloat, GLfloat, GLfloat);
    void (*Clear)(GLbitfield);
    void (*Enable)(GLenum);
    void (*Disable)(GLenum);
    void (*Scissor)(GLint, GLint, GLsizei, GLsizei);
    void (*Finish)(void);
    const GLubyte* (*GetString)(GLenum);
    void (*GetIntegerv)(GLenum, GLint*);
    GLuint (*CreateShader)(GLenum);
    void (*ShaderSource)(GLuint, GLsizei, const probe_GLchar* const*, const GLint*);
    void (*CompileShader)(GLuint);
    void (*GetShaderiv)(GLuint, GLenum, GLint*);
    void (*GetShaderInfoLog)(GLuint, GLsizei, GLsizei*, probe_GLchar*);
    GLuint (*CreateProgram)(void);
    void (*AttachShader)(GLuint, GLuint);
    void (*LinkProgram)(GLuint);
    void (*GetProgramiv)(GLuint, GLenum, GLint*);
    void (*UseProgram)(GLuint);
    GLint (*GetUniformLocation)(GLuint, const probe_GLchar*);
    void (*Uniform1i)(GLint, GLint);
    void (*Uniform1f)(GLint, GLfloat);
    void (*DrawArrays)(GLenum, GLint, GLsizei);
} glfns;

static const struct { const char* name; size_t off; } k_glnames[] = {
    {"glViewport", offsetof(glfns, Viewport)},
    {"glClearColor", offsetof(glfns, ClearColor)},
    {"glClear", offsetof(glfns, Clear)},
    {"glEnable", offsetof(glfns, Enable)},
    {"glDisable", offsetof(glfns, Disable)},
    {"glScissor", offsetof(glfns, Scissor)},
    {"glFinish", offsetof(glfns, Finish)},
    {"glGetString", offsetof(glfns, GetString)},
    {"glGetIntegerv", offsetof(glfns, GetIntegerv)},
    {"glCreateShader", offsetof(glfns, CreateShader)},
    {"glShaderSource", offsetof(glfns, ShaderSource)},
    {"glCompileShader", offsetof(glfns, CompileShader)},
    {"glGetShaderiv", offsetof(glfns, GetShaderiv)},
    {"glGetShaderInfoLog", offsetof(glfns, GetShaderInfoLog)},
    {"glCreateProgram", offsetof(glfns, CreateProgram)},
    {"glAttachShader", offsetof(glfns, AttachShader)},
    {"glLinkProgram", offsetof(glfns, LinkProgram)},
    {"glGetProgramiv", offsetof(glfns, GetProgramiv)},
    {"glUseProgram", offsetof(glfns, UseProgram)},
    {"glGetUniformLocation", offsetof(glfns, GetUniformLocation)},
    {"glUniform1i", offsetof(glfns, Uniform1i)},
    {"glUniform1f", offsetof(glfns, Uniform1f)},
    {"glDrawArrays", offsetof(glfns, DrawArrays)},
};

/* ------------------------------------------------------------------ */
/* GLX extension entry points                                           */

static Bool (*pGetSyncValuesOML)(Display*, GLXDrawable, int64_t*, int64_t*, int64_t*);
static Bool (*pGetMscRateOML)(Display*, GLXDrawable, int32_t*, int32_t*);
static int64_t (*pSwapBuffersMscOML)(Display*, GLXDrawable, int64_t, int64_t, int64_t);
static Bool (*pWaitForSbcOML)(Display*, GLXDrawable, int64_t, int64_t*, int64_t*, int64_t*);
static void (*pSwapIntervalEXT)(Display*, GLXDrawable, int);
static int (*pSwapIntervalMESA)(unsigned);
static int (*pSwapIntervalSGI)(int);
static GLXContext (*pCreateContextAttribsARB)(Display*, GLXFBConfig, GLXContext, Bool, const int*);

#define LOADFN(dst, src) do { vfn f_ = (src); memcpy(&(dst), &f_, sizeof(dst)); } while (0)

static void glx_load_entry_points(void) {
    static int done;
    if (done) return;
    done = 1;
#define G(dst, name) LOADFN(dst, glXGetProcAddressARB((const GLubyte*)name))
    G(pGetSyncValuesOML, "glXGetSyncValuesOML");
    G(pGetMscRateOML, "glXGetMscRateOML");
    G(pSwapBuffersMscOML, "glXSwapBuffersMscOML");
    G(pWaitForSbcOML, "glXWaitForSbcOML");
    G(pSwapIntervalEXT, "glXSwapIntervalEXT");
    G(pSwapIntervalMESA, "glXSwapIntervalMESA");
    G(pSwapIntervalSGI, "glXSwapIntervalSGI");
    G(pCreateContextAttribsARB, "glXCreateContextAttribsARB");
#undef G
}

/* ------------------------------------------------------------------ */
/* EGL, loaded at run time                                              */

typedef struct {
    void *h, *gles;
    char path[512];
    vfn (*GetProcAddress)(const char*);
    void* (*GetPlatformDisplay)(p_EGLenum, void*, const p_EGLAttrib*);
    void* (*GetPlatformDisplayEXT)(p_EGLenum, void*, const p_EGLint*);
    p_EGLBoolean (*Initialize)(void*, p_EGLint*, p_EGLint*);
    p_EGLBoolean (*Terminate)(void*);
    const char* (*QueryString)(void*, p_EGLint);
    p_EGLBoolean (*ChooseConfig)(void*, const p_EGLint*, void**, p_EGLint, p_EGLint*);
    p_EGLBoolean (*GetConfigAttrib)(void*, void*, p_EGLint, p_EGLint*);
    void* (*CreateWindowSurface)(void*, void*, unsigned long, const p_EGLint*);
    p_EGLBoolean (*BindAPI)(p_EGLenum);
    void* (*CreateContext)(void*, void*, void*, const p_EGLint*);
    p_EGLBoolean (*MakeCurrent)(void*, void*, void*, void*);
    p_EGLBoolean (*SwapBuffers)(void*, void*);
    p_EGLBoolean (*SwapInterval)(void*, p_EGLint);
    p_EGLint (*GetError)(void);
    p_EGLBoolean (*DestroySurface)(void*, void*);
    p_EGLBoolean (*DestroyContext)(void*, void*);
    p_EGLBoolean (*GetSyncValuesCHROMIUM)(void*, void*, uint64_t*, uint64_t*, uint64_t*);
    p_EGLBoolean (*GetMscRateANGLE)(void*, void*, int32_t*, int32_t*);
    const char* (*GetDisplayDriverName)(void*);
} egl_lib;

static int egl_load(egl_lib* E, const char* egl_path, const char* gles_path) {
    memset(E, 0, sizeof *E);
    snprintf(E->path, sizeof E->path, "%s", egl_path);
    if (gles_path) E->gles = dlopen(gles_path, RTLD_NOW | RTLD_LOCAL);
    E->h = dlopen(egl_path, RTLD_NOW | RTLD_LOCAL);
    if (!E->h) {
        say("  %s: not loadable (%s)\n", egl_path, dlerror());
        return 0;
    }
#define S(dst, name) do { void* p_ = dlsym(E->h, name); memcpy(&(dst), &p_, sizeof(dst)); } while (0)
    S(E->GetProcAddress, "eglGetProcAddress");
    S(E->GetPlatformDisplay, "eglGetPlatformDisplay");
    S(E->Initialize, "eglInitialize");
    S(E->Terminate, "eglTerminate");
    S(E->QueryString, "eglQueryString");
    S(E->ChooseConfig, "eglChooseConfig");
    S(E->GetConfigAttrib, "eglGetConfigAttrib");
    S(E->CreateWindowSurface, "eglCreateWindowSurface");
    S(E->BindAPI, "eglBindAPI");
    S(E->CreateContext, "eglCreateContext");
    S(E->MakeCurrent, "eglMakeCurrent");
    S(E->SwapBuffers, "eglSwapBuffers");
    S(E->SwapInterval, "eglSwapInterval");
    S(E->GetError, "eglGetError");
    S(E->DestroySurface, "eglDestroySurface");
    S(E->DestroyContext, "eglDestroyContext");
#undef S
    if (!E->GetProcAddress || !E->Initialize || !E->CreateWindowSurface || !E->SwapBuffers) {
        say("  %s: core EGL entry points missing\n", egl_path);
        return 0;
    }
    LOADFN(E->GetPlatformDisplayEXT, E->GetProcAddress("eglGetPlatformDisplayEXT"));
    if (!E->GetPlatformDisplay) LOADFN(E->GetPlatformDisplay, E->GetProcAddress("eglGetPlatformDisplay"));
    LOADFN(E->GetSyncValuesCHROMIUM, E->GetProcAddress("eglGetSyncValuesCHROMIUM"));
    LOADFN(E->GetMscRateANGLE, E->GetProcAddress("eglGetMscRateANGLE"));
    LOADFN(E->GetDisplayDriverName, E->GetProcAddress("eglGetDisplayDriverName"));
    return 1;
}

static void* egl_platform_display(egl_lib* E, p_EGLenum plat, void* native, const p_EGLAttrib* a) {
    if (E->GetPlatformDisplay) {
        void* d = E->GetPlatformDisplay(plat, native, a);
        if (d) return d;
    }
    if (E->GetPlatformDisplayEXT) {
        p_EGLint ai[32];
        int i = 0;
        for (; a && a[i] != P_EGL_NONE && i < 30; i++) ai[i] = (p_EGLint)a[i];
        ai[i] = P_EGL_NONE;
        return E->GetPlatformDisplayEXT(plat, native, ai);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* The context: one window, one GL context, one Present observer.       */

enum { API_GLX = 1, API_EGL = 2 };
enum { RECT_FULL, RECT_WINDOW, RECT_CRTC0 };

typedef struct {
    uint32_t serial;
    uint8_t kind, mode;
    uint64_t ust, msc;
    int64_t arr, arr_raw, arr_real, arr_boot;
} pev;

typedef struct {
    char label[64], dpy_name[300];
    int screen, api, rect_mode;
    Display* dpy;
    Window win;
    Colormap cmap;
    int x, y, w, h;
    Visual* vis;
    int depth;
    GLXFBConfig fbc;
    GLXContext gctx;
    GLXWindow gwin;
    int glx_es, swap_ctl;      /* swap_ctl: 1 EXT, 2 MESA, 3 SGI */
    egl_lib* E;
    void *ed, *es, *ec, *ecfg;
    glfns gl;
    int has_sync;               /* OML on GLX, CHROMIUM on EGL */
    int has_intel, intel_ev;
    xcb_connection_t* xc;
    xcb_special_event_t* se;
    uint32_t eid;
    int has_present;
    pev* pix;
    int npix, pix_cap;
    int64_t pix_total, pix_skip;
    int64_t mode_count[4];
    pev* nm;
    int nnm, nm_cap;
    uint32_t nm_serial;
    int64_t sbc, run_sbc0;
    double period_mode, period_fit;
    const char* outs;
    int64_t last_ust, last_msc;
    int last_ok;
    GLuint prog;
    GLint u_n, u_t;
    int egl_rate, is_angle;
    int tainted;                /* the driver crashed under guarded(); never touch this context again */
} ctx;

static double ctx_period(const ctx* c) {
    return c->period_fit > 0 ? c->period_fit : c->period_mode > 0 ? c->period_mode : 1e9 / 60.0;
}

static void ctx_zero(ctx* c, int scr, const char* tag) {
    memset(c, 0, sizeof *c);
    c->screen = scr;
    snprintf(c->dpy_name, sizeof c->dpy_name, "%s.%d", g_base_name, scr);
    snprintf(c->label, sizeof c->label, "s%d/%s", scr, tag);
    c->pix_cap = O.frames + 1024;
    c->pix = calloc((size_t)c->pix_cap, sizeof(pev));
    c->nm_cap = O.vblanks + 256;
    c->nm = calloc((size_t)c->nm_cap, sizeof(pev));
}

static vfn gl_lookup(ctx* c, const char* name) {
    vfn f = NULL;
    if (c->api == API_GLX) return glXGetProcAddressARB((const GLubyte*)name);
    if (c->E->gles) {
        void* p = dlsym(c->E->gles, name);
        memcpy(&f, &p, sizeof f);
    }
    if (!f) f = c->E->GetProcAddress(name);
    return f;
}

static int gl_load(ctx* c) {
    int missing = 0;
    for (size_t i = 0; i < sizeof k_glnames / sizeof k_glnames[0]; i++) {
        vfn f = gl_lookup(c, k_glnames[i].name);
        if (!f) missing++;
        memcpy((char*)&c->gl + k_glnames[i].off, &f, sizeof f);
    }
    return missing;
}

static void hide_cursor(Display* d, Window w) {
    static char z[8];
    Pixmap p = XCreateBitmapFromData(d, w, z, 8, 8);
    XColor col;
    memset(&col, 0, sizeof col);
    Cursor cur = XCreatePixmapCursor(d, p, p, &col, &col, 0, 0);
    XDefineCursor(d, w, cur);
    XFreeCursor(d, cur);
    XFreePixmap(d, p);
}

static void pick_rect(ctx* c, int mode) {
    int scr = DefaultScreen(c->dpy);
    c->rect_mode = mode;
    c->x = 0;
    c->y = 0;
    c->w = DisplayWidth(c->dpy, scr);
    c->h = DisplayHeight(c->dpy, scr);
    if (mode == RECT_WINDOW) {
        c->x = 64;
        c->y = 64;
        c->w = 640;
        c->h = 480;
    } else if (mode == RECT_CRTC0) {
        for (int i = 0; i < g_scr[c->screen].n; i++)
            if (g_scr[c->screen].c[i].active) {
                const crtc_t* k = &g_scr[c->screen].c[i];
                c->x = k->x;
                c->y = k->y;
                c->w = k->w;
                c->h = k->h;
                break;
            }
    }
    c->outs = "";
    c->period_mode = rect_period_ns(c->screen, c->x, c->y, c->w, c->h, &c->outs);
    c->period_fit = 0;
}

static Window make_window(ctx* c, Visual* vis, int depth) {
    Display* d = c->dpy;
    int scr = DefaultScreen(d);
    int full = c->rect_mode != RECT_WINDOW;
    XSetWindowAttributes a;
    memset(&a, 0, sizeof a);
    c->cmap = a.colormap = XCreateColormap(d, RootWindow(d, scr), vis, AllocNone);
    a.event_mask = StructureNotifyMask | ExposureMask;
    a.override_redirect = full && !O.wm;
    unsigned long mask = CWColormap | CWBackPixel | CWBorderPixel | CWEventMask | CWOverrideRedirect;
    Window w = XCreateWindow(d, RootWindow(d, scr), c->x, c->y, (unsigned)c->w, (unsigned)c->h, 0, depth, InputOutput,
                             vis, mask, &a);
    XStoreName(d, w, "ysp x11 probe");
    if (full) {
        /* Asks a compositing manager, if one runs, to unredirect the window,
         * so the server can flip it. */
        prop_card32(d, w, "_NET_WM_BYPASS_COMPOSITOR", 1);
        if (O.wm) {
            Atom st = A(d, "_NET_WM_STATE_FULLSCREEN");
            XChangeProperty(d, w, A(d, "_NET_WM_STATE"), XA_ATOM, 32, PropModeReplace, (unsigned char*)&st, 1);
        }
    }
    hide_cursor(d, w);
    XMapRaised(d, w);
    int64_t until = now_ns() + 2000000000;
    XEvent ev;
    while (!XCheckTypedWindowEvent(d, w, MapNotify, &ev) && now_ns() < until) usleep(2000);
    if (full && O.wm) usleep(500000);
    XSync(d, False);
    c->vis = vis;
    c->depth = depth;
    return w;
}

/* --- Present observer: a second connection that selects the window's
 * PresentCompleteNotify events. The GL driver presents on its own
 * connection; Present sends each selecting client its own copy. */

static int po_select(ctx* c) {
    if (c->se) {
        xcb_unregister_for_special_event(c->xc, c->se);
        c->se = NULL;
    }
    c->eid = xcb_generate_id(c->xc);
    c->se = xcb_register_for_special_xge(c->xc, &xcb_present_id, c->eid, NULL);
    xcb_generic_error_t* err = xcb_request_check(
        c->xc, xcb_present_select_input_checked(c->xc, c->eid, (xcb_window_t)c->win, XCB_PRESENT_EVENT_MASK_COMPLETE_NOTIFY));
    if (err) {
        free(err);
        c->has_present = 0;
        return 0;
    }
    c->has_present = 1;
    return 1;
}

static int po_open(ctx* c) {
    c->has_present = 0;
    c->xc = xcb_connect(c->dpy_name, NULL);
    if (!c->xc || xcb_connection_has_error(c->xc)) return 0;
    const xcb_query_extension_reply_t* e = xcb_get_extension_data(c->xc, &xcb_present_id);
    if (!e || !e->present) return 0;
    return po_select(c);
}

static void po_take(ctx* c, xcb_generic_event_t* ev) {
    int64_t t = now_ns(), traw = clk(CLOCK_MONOTONIC_RAW), treal = clk(CLOCK_REALTIME), tboot = clk(CLOCK_BOOTTIME);
    const xcb_present_generic_event_t* ge = (const xcb_present_generic_event_t*)ev;
    if (ge->evtype == XCB_PRESENT_COMPLETE_NOTIFY) {
        const xcb_present_complete_notify_event_t* ce = (const xcb_present_complete_notify_event_t*)ev;
        pev p;
        p.serial = ce->serial;
        p.kind = ce->kind;
        p.mode = ce->mode;
        p.ust = ce->ust;
        p.msc = ce->msc;
        p.arr = t;
        p.arr_raw = traw;
        p.arr_real = treal;
        p.arr_boot = tboot;
        if (ce->kind == XCB_PRESENT_COMPLETE_KIND_PIXMAP) {
            if (c->pix_skip > 0) c->pix_skip--;
            else if (c->npix < c->pix_cap) c->pix[c->npix++] = p;
            c->pix_total++;
            if (ce->mode < 4) c->mode_count[ce->mode]++;
        } else {
            c->nm[c->nnm % c->nm_cap] = p;
            c->nnm++;
        }
    }
    free(ev);
}

static int po_pump(ctx* c, int timeout_ms) {
    if (!c->has_present) return 0;
    int got = 0;
    xcb_generic_event_t* ev;
    while ((ev = xcb_poll_for_special_event(c->xc, c->se)) != NULL) {
        po_take(c, ev);
        got++;
    }
    if (got || timeout_ms <= 0 || xcb_connection_has_error(c->xc)) return got;
    struct pollfd pf = {xcb_get_file_descriptor(c->xc), POLLIN, 0};
    if (poll(&pf, 1, timeout_ms) <= 0) return 0;
    /* Makes libxcb read the socket, which sorts special events into their
     * own queue; this connection selects no ordinary events. */
    free(xcb_poll_for_event(c->xc));
    while ((ev = xcb_poll_for_special_event(c->xc, c->se)) != NULL) {
        po_take(c, ev);
        got++;
    }
    return got;
}

static int po_wait_pix(ctx* c, int idx, int64_t deadline) {
    while (c->npix <= idx && !g_stop) {
        int64_t left = deadline - now_ns();
        if (left <= 0) break;
        int ms = (int)(left / 1000000) + 1;
        po_pump(c, ms > 50 ? 50 : ms);
    }
    return c->npix > idx;
}

static int po_find_nm(ctx* c, uint32_t serial, pev* out) {
    int j0 = c->nnm > c->nm_cap ? c->nnm - c->nm_cap : 0;
    for (int j = c->nnm - 1; j >= j0; j--)
        if (c->nm[j % c->nm_cap].serial == serial) {
            *out = c->nm[j % c->nm_cap];
            return 1;
        }
    return 0;
}

static int po_wait_nm(ctx* c, uint32_t serial, int64_t deadline, pev* out) {
    for (;;) {
        po_pump(c, 0);
        if (po_find_nm(c, serial, out)) return 1;
        int64_t left = deadline - now_ns();
        if (left <= 0 || g_stop) return 0;
        int ms = (int)(left / 1000000) + 1;
        po_pump(c, ms > 50 ? 50 : ms);
        if (po_find_nm(c, serial, out)) return 1;
    }
}

static uint32_t po_notify(ctx* c, uint64_t target) {
    uint32_t s = 0x40000000u | (++c->nm_serial & 0x3fffffffu);
    xcb_present_notify_msc(c->xc, (xcb_window_t)c->win, s, target, 0, 0);
    xcb_flush(c->xc);
    return s;
}

/* --- drawing */

static void heavy_init(ctx* c, int es) {
    if (O.gpu_iters <= 0) return;
    glfns* g = &c->gl;
    if (!g->CreateShader || !g->DrawArrays) {
        say("  GPU load: shader entry points missing, load off\n");
        return;
    }
    const char* hdr = es ? "#version 300 es\nprecision highp float;\nprecision highp int;\n" : "#version 130\n";
    const char* vs =
        "void main() {\n"
        "  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
        "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
        "}\n";
    /* A long dependent chain per pixel: fill-rate and ALU load the
     * compiler cannot remove, at a near-constant dark gray. */
    const char* fs =
        "uniform int n;\n"
        "uniform float t;\n"
        "out vec4 o;\n"
        "void main() {\n"
        "  vec2 q = gl_FragCoord.xy * 0.001;\n"
        "  float a = t;\n"
        "  for (int i = 0; i < n; i++) a = sin(a + q.x) * cos(a + q.y) + 0.5;\n"
        "  o = vec4(vec3(0.15 + 0.001 * fract(a)), 1.0);\n"
        "}\n";
    GLuint sh[2];
    const char* body[2] = {vs, fs};
    GLenum kind[2] = {GL_VERTEX_SHADER, GL_FRAGMENT_SHADER};
    for (int i = 0; i < 2; i++) {
        const probe_GLchar* src[2] = {hdr, body[i]};
        sh[i] = g->CreateShader(kind[i]);
        g->ShaderSource(sh[i], 2, src, NULL);
        g->CompileShader(sh[i]);
        GLint ok = 0;
        g->GetShaderiv(sh[i], GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[512] = "";
            g->GetShaderInfoLog(sh[i], sizeof log, NULL, log);
            say("  GPU load: shader %d did not compile: %s\n", i, log);
            return;
        }
    }
    GLuint p = g->CreateProgram();
    g->AttachShader(p, sh[0]);
    g->AttachShader(p, sh[1]);
    g->LinkProgram(p);
    GLint ok = 0;
    g->GetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        say("  GPU load: program did not link\n");
        return;
    }
    c->prog = p;
    c->u_n = g->GetUniformLocation(p, "n");
    c->u_t = g->GetUniformLocation(p, "t");
    say("  GPU load: on, %d iterations per pixel\n", O.gpu_iters);
}

/* A dark gray screen with a 64 x 64 patch at the top left that changes
 * between black and white every frame, for a photodiode. No full-field
 * flicker. */
static void draw(ctx* c, int frame) {
    glfns* g = &c->gl;
    g->Viewport(0, 0, c->w, c->h);
    g->ClearColor(0.15f, 0.15f, 0.15f, 1.0f);
    g->Clear(GL_COLOR_BUFFER_BIT);
    if (c->prog) {
        g->UseProgram(c->prog);
        g->Uniform1i(c->u_n, O.gpu_iters);
        g->Uniform1f(c->u_t, (float)frame * 0.01f);
        g->DrawArrays(GL_TRIANGLES, 0, 3);
        g->UseProgram(0);
    }
    g->Enable(GL_SCISSOR_TEST);
    g->Scissor(0, c->h - 64, 64, 64);
    float v = (frame & 1) ? 1.0f : 0.0f;
    g->ClearColor(v, v, v, 1.0f);
    g->Clear(GL_COLOR_BUFFER_BIT);
    g->Disable(GL_SCISSOR_TEST);
}

static void ctx_swap(ctx* c) {
    if (c->api == API_GLX) glXSwapBuffers(c->dpy, c->gwin);
    else c->E->SwapBuffers(c->ed, c->es);
}

static void ctx_interval(ctx* c, int n) {
    if (c->api == API_EGL) {
        if (c->E->SwapInterval) c->E->SwapInterval(c->ed, n);
        return;
    }
    if (c->swap_ctl == 1) pSwapIntervalEXT(c->dpy, c->gwin, n);
    else if (c->swap_ctl == 2) pSwapIntervalMESA((unsigned)n);
    else if (c->swap_ctl == 3 && n > 0) pSwapIntervalSGI(n);
}

static int ctx_sync_values(ctx* c, int64_t* u, int64_t* m, int64_t* s) {
    if (!c->has_sync) return 0;
    if (c->api == API_GLX) return pGetSyncValuesOML(c->dpy, c->gwin, u, m, s) ? 1 : 0;
    uint64_t a = 0, b = 0, d = 0;
    if (!c->E->GetSyncValuesCHROMIUM(c->ed, c->es, &a, &b, &d)) return 0;
    *u = (int64_t)a;
    *m = (int64_t)b;
    *s = (int64_t)d;
    return 1;
}

static void try_rt(const char* who) {
    if (O.no_rt) return;
    struct sched_param sp;
    memset(&sp, 0, sizeof sp);
    sp.sched_priority = 10;
    int e = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    say("  %s: SCHED_FIFO priority 10: %s\n", who, e ? strerror(e) : "on");
}

/* An optional driver query that crashes must not end the run: the first
 * call of each such query runs under a fault handler. Mesa 23.2's
 * eglGetMscRateANGLE crashed this way on its software X11 path. After a
 * crash the driver's and Xlib's locks may still be held, so the caller
 * abandons that context (ctx.tainted) instead of closing it. */
static sigjmp_buf g_jmp;
static volatile sig_atomic_t g_guard;

static void on_fault(int s) {
    if (g_guard) siglongjmp(g_jmp, s);
    signal(s, SIG_DFL);
    raise(s);
}

typedef int (*guarded_fn)(void* arg);

static int guarded(guarded_fn f, void* arg, const char* what) {
    struct sigaction sa, o1, o2, o3;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_fault;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &o1);
    sigaction(SIGBUS, &sa, &o2);
    sigaction(SIGFPE, &sa, &o3);
    volatile int r = -1;
    int sig = sigsetjmp(g_jmp, 1);
    if (sig == 0) {
        g_guard = 1;
        r = f(arg);
    } else {
        say("  %s: CRASHED in the driver (signal %d); not called again\n", what, sig);
    }
    g_guard = 0;
    sigaction(SIGSEGV, &o1, NULL);
    sigaction(SIGBUS, &o2, NULL);
    sigaction(SIGFPE, &o3, NULL);
    return r;
}

/* ------------------------------------------------------------------ */
/* GLX context                                                          */

static int glx_window(ctx* c) {
    XVisualInfo* vi = glXGetVisualFromFBConfig(c->dpy, c->fbc);
    if (!vi) return 0;
    c->win = make_window(c, vi->visual, vi->depth);
    XFree(vi);
    c->gwin = glXCreateWindow(c->dpy, c->fbc, c->win, NULL);
    return c->gwin != 0;
}

static int glx_ctx_open(ctx* c, int rect_mode, int print) {
    c->api = API_GLX;
    c->dpy = XOpenDisplay(c->dpy_name);
    if (!c->dpy) {
        say("  cannot open display %s\n", c->dpy_name);
        return 0;
    }
    int scr = DefaultScreen(c->dpy);
    glx_load_entry_points();
    int emaj = 0, emin = 0, errb = 0, evb = 0;
    if (!glXQueryExtension(c->dpy, &errb, &evb) || !glXQueryVersion(c->dpy, &emaj, &emin)) {
        say("  GLX: not available on %s\n", c->dpy_name);
        return 0;
    }
    const char* exts = glXQueryExtensionsString(c->dpy, scr);
    if (print) {
        say("GLX %d.%d on %s\n", emaj, emin, c->dpy_name);
        say("  server: vendor \"%s\", version \"%s\"\n", glXQueryServerString(c->dpy, scr, GLX_VENDOR),
            glXQueryServerString(c->dpy, scr, GLX_VERSION));
        say("  client: vendor \"%s\", version \"%s\"\n", glXGetClientString(c->dpy, GLX_VENDOR),
            glXGetClientString(c->dpy, GLX_VERSION));
        char what[400];
        snprintf(what, sizeof what, "GLX extensions, %s (usable)", c->dpy_name);
        ext_dump(what, exts);
        snprintf(what, sizeof what, "GLX server extensions, %s", c->dpy_name);
        ext_dump(what, glXQueryServerString(c->dpy, scr, GLX_EXTENSIONS));
        static const char* want[] = {"GLX_OML_sync_control", "GLX_SGI_video_sync", "GLX_INTEL_swap_event",
                                     "GLX_EXT_swap_control", "GLX_MESA_swap_control", "GLX_SGI_swap_control",
                                     "GLX_EXT_swap_control_tear", "GLX_ARB_create_context",
                                     "GLX_EXT_create_context_es2_profile", "GLX_EXT_create_context_es_profile",
                                     "GLX_MESA_query_renderer", "GLX_EXT_buffer_age", "GLX_ARB_framebuffer_sRGB",
                                     "GLX_EXT_framebuffer_sRGB", "GLX_ARB_fbconfig_float"};
        for (size_t i = 0; i < sizeof want / sizeof want[0]; i++) say("  %-36s %s\n", want[i], yesno(has_ext(exts, want[i])));
        int n = 0;
        GLXFBConfig* all = glXGetFBConfigs(c->dpy, scr, &n);
        int n10 = 0;
        char list[512] = "";
        int len = 0;
        for (int i = 0; i < n; i++) {
            int r = 0, g = 0, b = 0, a = 0, dt = 0, rt = 0, db = 0, vid = 0, id = 0;
            glXGetFBConfigAttrib(c->dpy, all[i], GLX_RED_SIZE, &r);
            glXGetFBConfigAttrib(c->dpy, all[i], GLX_GREEN_SIZE, &g);
            glXGetFBConfigAttrib(c->dpy, all[i], GLX_BLUE_SIZE, &b);
            glXGetFBConfigAttrib(c->dpy, all[i], GLX_ALPHA_SIZE, &a);
            glXGetFBConfigAttrib(c->dpy, all[i], GLX_DRAWABLE_TYPE, &dt);
            glXGetFBConfigAttrib(c->dpy, all[i], GLX_RENDER_TYPE, &rt);
            glXGetFBConfigAttrib(c->dpy, all[i], GLX_DOUBLEBUFFER, &db);
            glXGetFBConfigAttrib(c->dpy, all[i], GLX_VISUAL_ID, &vid);
            glXGetFBConfigAttrib(c->dpy, all[i], GLX_FBCONFIG_ID, &id);
            if (r == 10 && g == 10 && b == 10 && (dt & GLX_WINDOW_BIT) && (rt & GLX_RGBA_BIT) && db) {
                n10++;
                if (len < (int)sizeof list - 48)
                    len += snprintf(list + len, sizeof list - (size_t)len, " 0x%x(visual 0x%x, alpha %d)", id, vid, a);
            }
        }
        if (all) XFree(all);
        say("  FBConfigs: %d, of which 10 bpc double-buffered window RGBA: %d%s%s\n", n, n10, n10 ? ":" : "", list);
    }
    c->has_sync = has_ext(exts, "GLX_OML_sync_control") && pGetSyncValuesOML && pSwapBuffersMscOML && pWaitForSbcOML;
    c->has_intel = has_ext(exts, "GLX_INTEL_swap_event");
    c->intel_ev = evb + P_GLX_BufferSwapComplete;
    c->swap_ctl = has_ext(exts, "GLX_EXT_swap_control") && pSwapIntervalEXT ? 1
                : has_ext(exts, "GLX_MESA_swap_control") && pSwapIntervalMESA ? 2
                : has_ext(exts, "GLX_SGI_swap_control") && pSwapIntervalSGI ? 3 : 0;

    int attr[] = {GLX_X_RENDERABLE, True, GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT, GLX_RENDER_TYPE, GLX_RGBA_BIT,
                  GLX_X_VISUAL_TYPE, GLX_TRUE_COLOR, GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8,
                  GLX_DOUBLEBUFFER, True, None};
    int nc = 0;
    GLXFBConfig* cf = glXChooseFBConfig(c->dpy, scr, attr, &nc);
    if (!cf || nc == 0) {
        say("  GLX: no 8 bpc double-buffered window FBConfig\n");
        return 0;
    }
    c->fbc = cf[0];
    for (int i = 0; i < nc; i++) {
        int r = 0;
        XVisualInfo* vi = glXGetVisualFromFBConfig(c->dpy, cf[i]);
        glXGetFBConfigAttrib(c->dpy, cf[i], GLX_RED_SIZE, &r);
        int ok = vi && r == 8 && vi->depth == 24;
        if (vi) XFree(vi);
        if (ok) {
            c->fbc = cf[i];
            break;
        }
    }
    XFree(cf);
    pick_rect(c, rect_mode);
    if (!glx_window(c)) {
        say("  GLX: window creation failed\n");
        return 0;
    }
    c->gctx = NULL;
    c->glx_es = 0;
    if (!O.glx_compat && has_ext(exts, "GLX_EXT_create_context_es2_profile") && pCreateContextAttribsARB) {
        int ca[] = {P_GLX_CONTEXT_MAJOR_VERSION_ARB, 3, P_GLX_CONTEXT_MINOR_VERSION_ARB, 0,
                    P_GLX_CONTEXT_PROFILE_MASK_ARB, P_GLX_CONTEXT_ES2_PROFILE_BIT_EXT, None};
        t_xerr = 0;
        c->gctx = pCreateContextAttribsARB(c->dpy, c->fbc, NULL, True, ca);
        XSync(c->dpy, False);
        if (c->gctx && !t_xerr) c->glx_es = 1;
        else {
            if (print) say("  GLX ES 3.0 context: failed (X error %d)\n", t_xerr);
            c->gctx = NULL;
        }
    }
    if (!c->gctx) c->gctx = glXCreateNewContext(c->dpy, c->fbc, GLX_RGBA_TYPE, NULL, True);
    if (!c->gctx || !glXMakeContextCurrent(c->dpy, c->gwin, c->gwin, c->gctx)) {
        say("  GLX: context creation failed\n");
        return 0;
    }
    int missing = gl_load(c);
    if (missing && !c->gl.Clear) {
        say("  GLX: GL entry points missing\n");
        return 0;
    }
    if (print) {
        say("  context: %s, direct %s\n", c->glx_es ? "GL ES 3.0 through GLX_EXT_create_context_es2_profile" : "desktop GL (compatibility)",
            yesno(glXIsDirect(c->dpy, c->gctx)));
        say("  GL_VENDOR \"%s\"\n  GL_RENDERER \"%s\"\n  GL_VERSION \"%s\"\n  GL_SHADING_LANGUAGE_VERSION \"%s\"\n",
            c->gl.GetString(GL_VENDOR), c->gl.GetString(GL_RENDERER), c->gl.GetString(GL_VERSION),
            c->gl.GetString(GL_SHADING_LANGUAGE_VERSION));
        say("  swap interval control: %s\n", c->swap_ctl == 1 ? "GLX_EXT_swap_control" : c->swap_ctl == 2 ? "GLX_MESA_swap_control"
                                             : c->swap_ctl == 3 ? "GLX_SGI_swap_control" : "none");
    }
    ctx_interval(c, 1);
    if (c->has_intel) glXSelectEvent(c->dpy, c->gwin, P_GLX_BUFFER_SWAP_COMPLETE_INTEL_MASK);
    heavy_init(c, c->glx_es);
    if (!po_open(c)) say("  Present: cannot select PresentCompleteNotify on the window; Present columns stay empty\n");
    return 1;
}

/* Moves the context to a new window (a smaller one, or one output). */
static int glx_retarget(ctx* c, int rect_mode) {
    glXMakeContextCurrent(c->dpy, None, None, NULL);
    glXDestroyWindow(c->dpy, c->gwin);
    XDestroyWindow(c->dpy, c->win);
    XSync(c->dpy, False);
    pick_rect(c, rect_mode);
    if (!glx_window(c)) return 0;
    if (!glXMakeContextCurrent(c->dpy, c->gwin, c->gwin, c->gctx)) return 0;
    ctx_interval(c, 1);
    if (c->has_intel) glXSelectEvent(c->dpy, c->gwin, P_GLX_BUFFER_SWAP_COMPLETE_INTEL_MASK);
    c->sbc = 0;
    c->pix_total = 0;
    c->pix_skip = 0;
    c->npix = 0;
    if (c->xc) po_select(c);
    return 1;
}

static void ctx_close(ctx* c) {
    if (c->tainted) {
        say("  %s: abandoned after the driver crash; its window stays until the probe exits\n", c->label);
        memset(c, 0, sizeof *c);
        return;
    }
    if (c->api == API_GLX && c->dpy) {
        glXMakeContextCurrent(c->dpy, None, None, NULL);
        if (c->gctx) glXDestroyContext(c->dpy, c->gctx);
        if (c->gwin) glXDestroyWindow(c->dpy, c->gwin);
    } else if (c->api == API_EGL && c->E && c->ed) {
        c->E->MakeCurrent(c->ed, NULL, NULL, NULL);
        if (c->es) c->E->DestroySurface(c->ed, c->es);
        if (c->ec) c->E->DestroyContext(c->ed, c->ec);
        c->E->Terminate(c->ed);
    }
    if (c->dpy) {
        if (c->win) XDestroyWindow(c->dpy, c->win);
        XSync(c->dpy, False);
        XCloseDisplay(c->dpy);
    }
    if (c->xc) xcb_disconnect(c->xc);
    free(c->pix);
    free(c->nm);
    memset(c, 0, sizeof *c);
}

/* ------------------------------------------------------------------ */
/* EGL context                                                          */

static int egl_ctx_open(ctx* c, egl_lib* E, int angle_type, int rect_mode) {
    c->api = API_EGL;
    c->E = E;
    c->dpy = XOpenDisplay(c->dpy_name);
    if (!c->dpy) {
        say("  cannot open display %s\n", c->dpy_name);
        return 0;
    }
    int scr = DefaultScreen(c->dpy);
    const char* cext = E->QueryString ? E->QueryString(NULL, P_EGL_EXTENSIONS) : NULL;
    ext_dump("EGL client extensions", cext);
    static const char* wantc[] = {"EGL_EXT_platform_base", "EGL_KHR_platform_x11", "EGL_EXT_platform_x11",
                                  "EGL_MESA_platform_xcb", "EGL_ANGLE_platform_angle", "EGL_ANGLE_platform_angle_vulkan",
                                  "EGL_ANGLE_platform_angle_opengl", "EGL_EXT_device_query"};
    for (size_t i = 0; i < sizeof wantc / sizeof wantc[0]; i++)
        say("  client %-36s %s\n", wantc[i], yesno(has_ext(cext, wantc[i])));
    if (angle_type) {
        p_EGLAttrib a[] = {P_EGL_PLATFORM_ANGLE_TYPE_ANGLE, angle_type, P_EGL_PLATFORM_ANGLE_NATIVE_PLATFORM_TYPE_ANGLE,
                           P_EGL_PLATFORM_X11_KHR, P_EGL_NONE};
        c->ed = egl_platform_display(E, P_EGL_PLATFORM_ANGLE_ANGLE, c->dpy, a);
    } else {
        p_EGLAttrib a[] = {P_EGL_PLATFORM_X11_SCREEN_KHR, scr, P_EGL_NONE};
        c->ed = egl_platform_display(E, P_EGL_PLATFORM_X11_KHR, c->dpy, a);
        if (!c->ed) {
            p_EGLAttrib a2[] = {P_EGL_NONE};
            c->ed = egl_platform_display(E, P_EGL_PLATFORM_X11_KHR, c->dpy, a2);
            if (c->ed) say("  EGL_PLATFORM_X11_SCREEN_KHR refused; display made without it\n");
        }
    }
    if (!c->ed) {
        say("  eglGetPlatformDisplay: failed (error 0x%x)\n", E->GetError ? E->GetError() : 0);
        return 0;
    }
    p_EGLint maj = 0, min = 0;
    if (!E->Initialize(c->ed, &maj, &min)) {
        say("  eglInitialize: failed (error 0x%x)\n", E->GetError ? E->GetError() : 0);
        c->ed = NULL;
        return 0;
    }
    const char* dext = E->QueryString(c->ed, P_EGL_EXTENSIONS);
    say("  EGL %d.%d, vendor \"%s\", version \"%s\", client APIs \"%s\"\n", maj, min, E->QueryString(c->ed, P_EGL_VENDOR),
        E->QueryString(c->ed, P_EGL_VERSION), E->QueryString(c->ed, P_EGL_CLIENT_APIS));
    if (E->GetDisplayDriverName && has_ext(dext, "EGL_MESA_query_driver"))
        say("  driver (EGL_MESA_query_driver): %s\n", E->GetDisplayDriverName(c->ed));
    ext_dump("EGL display extensions", dext);
    static const char* want[] = {"EGL_CHROMIUM_sync_control", "EGL_ANGLE_sync_control_rate",
                                 "EGL_ANDROID_get_frame_timestamps", "EGL_ANDROID_presentation_time",
                                 "EGL_EXT_present_opaque", "EGL_KHR_swap_buffers_with_damage", "EGL_EXT_buffer_age",
                                 "EGL_KHR_partial_update", "EGL_KHR_gl_colorspace", "EGL_EXT_gl_colorspace_bt2020_pq",
                                 "EGL_EXT_gl_colorspace_scrgb_linear", "EGL_EXT_gl_colorspace_display_p3",
                                 "EGL_EXT_pixel_format_float", "EGL_EXT_surface_SMPTE2086_metadata",
                                 "EGL_MESA_query_driver", "EGL_KHR_create_context", "EGL_KHR_no_config_context"};
    for (size_t i = 0; i < sizeof want / sizeof want[0]; i++) say("  %-36s %s\n", want[i], yesno(has_ext(dext, want[i])));

    p_EGLint ca[] = {P_EGL_SURFACE_TYPE, P_EGL_WINDOW_BIT, P_EGL_RENDERABLE_TYPE, P_EGL_OPENGL_ES3_BIT,
                     P_EGL_RED_SIZE, 10, P_EGL_GREEN_SIZE, 10, P_EGL_BLUE_SIZE, 10, P_EGL_NONE};
    void* cfgs[64];
    p_EGLint n = 0, n10 = 0;
    E->ChooseConfig(c->ed, ca, cfgs, 64, &n);
    for (int i = 0; i < n; i++) {
        p_EGLint r = 0;
        E->GetConfigAttrib(c->ed, cfgs[i], P_EGL_RED_SIZE, &r);
        n10 += r == 10;
    }
    say("  configs with 10 bpc, ES 3.0, window: %d\n", n10);
    ca[5] = ca[7] = ca[9] = 8;
    n = 0;
    if (!E->ChooseConfig(c->ed, ca, cfgs, 64, &n) || n == 0) {
        say("  eglChooseConfig: no ES 3.0 window config (error 0x%x)\n", E->GetError());
        return 0;
    }
    c->ecfg = cfgs[0];
    for (int i = 0; i < n; i++) {
        p_EGLint r = 0, g = 0, b = 0, vid = 0;
        E->GetConfigAttrib(c->ed, cfgs[i], P_EGL_RED_SIZE, &r);
        E->GetConfigAttrib(c->ed, cfgs[i], P_EGL_GREEN_SIZE, &g);
        E->GetConfigAttrib(c->ed, cfgs[i], P_EGL_BLUE_SIZE, &b);
        E->GetConfigAttrib(c->ed, cfgs[i], P_EGL_NATIVE_VISUAL_ID, &vid);
        if (r == 8 && g == 8 && b == 8 && vid) {
            c->ecfg = cfgs[i];
            break;
        }
    }
    p_EGLint vid = 0, cid = 0;
    E->GetConfigAttrib(c->ed, c->ecfg, P_EGL_NATIVE_VISUAL_ID, &vid);
    E->GetConfigAttrib(c->ed, c->ecfg, P_EGL_CONFIG_ID, &cid);
    Visual* vis = DefaultVisual(c->dpy, scr);
    int depth = DefaultDepth(c->dpy, scr);
    if (vid) {
        XVisualInfo tmpl;
        int nv = 0;
        memset(&tmpl, 0, sizeof tmpl);
        tmpl.visualid = (VisualID)vid;
        tmpl.screen = scr;
        XVisualInfo* vi = XGetVisualInfo(c->dpy, VisualIDMask | VisualScreenMask, &tmpl, &nv);
        if (vi && nv > 0) {
            vis = vi[0].visual;
            depth = vi[0].depth;
        }
        if (vi) XFree(vi);
    }
    say("  config 0x%x, native visual 0x%x, depth %d\n", cid, vid, depth);
    pick_rect(c, rect_mode);
    c->win = make_window(c, vis, depth);
    c->es = E->CreateWindowSurface(c->ed, c->ecfg, (unsigned long)c->win, NULL);
    if (!c->es) {
        say("  eglCreateWindowSurface: failed (error 0x%x)\n", E->GetError());
        return 0;
    }
    E->BindAPI(P_EGL_OPENGL_ES_API);
    p_EGLint cx[] = {P_EGL_CONTEXT_MAJOR_VERSION, 3, P_EGL_CONTEXT_MINOR_VERSION, 0, P_EGL_NONE};
    c->ec = E->CreateContext(c->ed, c->ecfg, NULL, cx);
    if (!c->ec || !E->MakeCurrent(c->ed, c->es, c->es, c->ec)) {
        say("  ES 3.0 context: failed (error 0x%x)\n", E->GetError());
        return 0;
    }
    if (gl_load(c) && !c->gl.Clear) {
        say("  GL ES entry points missing\n");
        return 0;
    }
    say("  GL_VENDOR \"%s\"\n  GL_RENDERER \"%s\"\n  GL_VERSION \"%s\"\n", c->gl.GetString(GL_VENDOR),
        c->gl.GetString(GL_RENDERER), c->gl.GetString(GL_VERSION));
    ctx_interval(c, 1);
    c->has_sync = has_ext(dext, "EGL_CHROMIUM_sync_control") && E->GetSyncValuesCHROMIUM;
    c->egl_rate = has_ext(dext, "EGL_ANGLE_sync_control_rate") && E->GetMscRateANGLE;
    heavy_init(c, 1);
    if (!po_open(c)) say("  Present: cannot select PresentCompleteNotify on the window; Present columns stay empty\n");
    return 1;
}

/* ------------------------------------------------------------------ */
/* Clock and vblank tests                                               */

static const char* k_clock_names[4] = {"CLOCK_MONOTONIC", "CLOCK_MONOTONIC_RAW", "CLOCK_REALTIME", "CLOCK_BOOTTIME"};

/* A UST is a vblank time on some clock in some unit. For each clock and
 * unit, "after minus UST" must lie between 0 and about a period (the
 * vblank was the last one before the read) on nearly every sample. */
static void ust_report(const char* what, int n, const int64_t* ust, int64_t* const after[4], double per) {
    if (n <= 0) return;
    double* d = malloc(sizeof(double) * (size_t)n);
    int verdict = -1, vunit = 0;
    say("  %s: read time minus UST, for each clock and unit (expected: -2 ms to about one period, %.3f ms):\n", what, per / 1e6);
    for (int k = 0; k < 4; k++)
        for (int u = 0; u < 2; u++) {
            double scale = u == 0 ? 1000.0 : 1.0;
            int ok = 0;
            for (int i = 0; i < n; i++) {
                double v = (double)after[k][i] - (double)ust[i] * scale;
                d[i] = v;
                /* A UST is the end of its vblank, the start of scanout;
                 * events and reads come from the vblank's start, so they
                 * can precede it by the vertical blanking time (0.44 ms
                 * p50 on the first rig). */
                if (v >= -2e6 && v <= 2.0 * per + 2e6) ok++;
            }
            dist ds = dist_of(d, n);
            say("    %-20s %s: %4d of %d in range, p50 %.3f ms, min %.3f ms, max %.3f ms\n", k_clock_names[k],
                u == 0 ? "us" : "ns", ok, n, ds.p50 / 1e6, ds.min / 1e6, ds.max / 1e6);
            if (verdict < 0 && ok >= n * 95 / 100) {
                verdict = k;
                vunit = u;
            }
        }
    if (verdict >= 0)
        say("  => UST is %s in %s (first clock that fits; MONOTONIC and BOOTTIME agree unless the machine slept)\n",
            k_clock_names[verdict], vunit == 0 ? "microseconds" : "nanoseconds");
    else
        say("  => UST fits none of the clocks above\n");
    free(d);
}

static int first_sync_call(void* arg) {
    int64_t u, m, s;
    return ctx_sync_values((ctx*)arg, &u, &m, &s);
}

static int msc_rate_call(void* arg) {
    ctx* c = arg;
    int32_t num = 0, den = 0;
    int ok = c->api == API_GLX ? (int)pGetMscRateOML(c->dpy, c->gwin, &num, &den)
                               : (int)c->E->GetMscRateANGLE(c->ed, c->es, &num, &den);
    if (ok && den)
        say("  %s: %d/%d = %.6f Hz; mode: %.6f Hz\n", c->api == API_GLX ? "glXGetMscRateOML" : "eglGetMscRateANGLE", num, den,
            (double)num / den, c->period_mode > 0 ? 1e9 / c->period_mode : 0.0);
    else
        say("  %s: failed\n", c->api == API_GLX ? "glXGetMscRateOML" : "eglGetMscRateANGLE");
    return ok;
}

static void clock_test(ctx* c) {
    if (!c->has_sync) {
        say("  sync values: not available (%s)\n", c->api == API_GLX ? "no GLX_OML_sync_control" : "no EGL_CHROMIUM_sync_control");
        return;
    }
    if (guarded(first_sync_call, c, c->api == API_GLX ? "glXGetSyncValuesOML" : "eglGetSyncValuesCHROMIUM") < 0) {
        c->has_sync = 0;
        c->tainted = 1;
        return;
    }
    int n = O.clock_samples;
    int64_t* ust = calloc((size_t)n, sizeof(int64_t));
    int64_t* msc = calloc((size_t)n, sizeof(int64_t));
    int64_t* t[4];
    for (int k = 0; k < 4; k++) t[k] = calloc((size_t)n, sizeof(int64_t));
    int m = 0;
    for (int i = 0; i < n && !g_stop; i++) {
        int64_t b = now_ns(), u = 0, ms = 0, s = 0;
        int ok = ctx_sync_values(c, &u, &ms, &s);
        int64_t a0 = now_ns(), a1 = clk(CLOCK_MONOTONIC_RAW), a2 = clk(CLOCK_REALTIME), a3 = clk(CLOCK_BOOTTIME);
        if (ok) {
            ust[m] = u;
            msc[m] = ms;
            t[0][m] = a0;
            t[1][m] = a1;
            t[2][m] = a2;
            t[3][m] = a3;
            pthread_mutex_lock(&g_lock);
            fprintf(g_clk, "%s,%s,%d,%" PRId64 ",%" PRId64 ",%" PRId64 ",%" PRId64 ",%" PRId64 ",%" PRId64 ",%" PRId64 ",%" PRId64 "\n",
                    c->label, c->api == API_GLX ? "glXGetSyncValuesOML" : "eglGetSyncValuesCHROMIUM", i, u, ms, s, b, a0, a1,
                    a2, a3);
            pthread_mutex_unlock(&g_lock);
            m++;
        }
        usleep(3300 + (i % 7) * 500);
    }
    say("  sync values: %d of %d reads succeeded; call time is in clock.csv\n", m, n);
    ust_report("sync-values UST", m, ust, t, ctx_period(c));
    if (m >= 2) {
        double dm = (double)(msc[m - 1] - msc[0]), du = (double)(ust[m - 1] - ust[0]) * 1000.0;
        if (dm > 0) say("  MSC rate from these reads: %.6f Hz (UST as us)\n", dm / du * 1e9);
    }
    if (c->api == API_GLX && pGetMscRateOML && guarded(msc_rate_call, c, "glXGetMscRateOML") < 0) c->tainted = 1;
    free(ust);
    free(msc);
    for (int k = 0; k < 4; k++) free(t[k]);
}

static int anchor(ctx* c) {
    double per = ctx_period(c);
    if (c->sbc > 0) {
        if (c->api == API_GLX && c->has_sync) {
            int64_t u, m, s;
            pWaitForSbcOML(c->dpy, c->gwin, c->sbc, &u, &m, &s);
        }
        if (c->has_present) {
            int64_t deadline = now_ns() + (int64_t)(8 * per);
            while (c->pix_total + c->pix_skip < c->sbc && now_ns() < deadline && !g_stop) po_pump(c, 20);
        } else {
            usleep((useconds_t)(4 * per / 1000));
        }
    }
    /* Events of swaps still in flight belong to no row; drop them. */
    c->pix_skip = c->sbc > c->pix_total ? c->sbc - c->pix_total : 0;
    c->npix = 0;
    c->run_sbc0 = c->sbc;
    if (c->has_present) {
        uint32_t s = po_notify(c, 0);
        pev e;
        if (po_wait_nm(c, s, now_ns() + 300000000, &e)) {
            c->last_msc = (int64_t)e.msc;
            c->last_ust = (int64_t)e.ust * 1000;
            c->last_ok = 1;
            return 1;
        }
    }
    int64_t u, m, s;
    if (ctx_sync_values(c, &u, &m, &s)) {
        c->last_msc = m;
        c->last_ust = u * 1000;
        c->last_ok = 1;
        return 1;
    }
    c->last_msc = 0;
    c->last_ust = now_ns();
    c->last_ok = 0;
    return 0;
}

static void vblank_test(ctx* c) {
    if (!c->has_present) {
        say("  PresentNotifyMSC: no Present, skipped\n");
        return;
    }
    anchor(c);
    int M = O.vblanks;
    if (M > c->nm_cap - 16) M = c->nm_cap - 16;
    c->nnm = 0;
    uint32_t* ser = calloc((size_t)M, sizeof(uint32_t));
    int64_t base = c->last_msc;
    double per = ctx_period(c);
    /* In batches: on the first rig (amdgpu DDX) the server completed every
     * NotifyMSC beyond 128 outstanding at once, with the current MSC. */
    for (int j0 = 0; j0 < M && !g_stop; j0 += 64) {
        int j1 = j0 + 64 < M ? j0 + 64 : M;
        for (int j = j0; j < j1; j++) {
            ser[j] = 0x40000000u | (++c->nm_serial & 0x3fffffffu);
            xcb_present_notify_msc(c->xc, (xcb_window_t)c->win, ser[j], (uint64_t)(base + 2 + j), 0, 0);
        }
        xcb_flush(c->xc);
        int64_t deadline = now_ns() + (int64_t)((j1 - j0 + 30) * per);
        while (c->nnm < j1 && now_ns() < deadline && !g_stop) po_pump(c, 50);
    }
    int n = c->nnm < M ? c->nnm : M;
    double *x = malloc(sizeof(double) * (size_t)n + 8), *y = malloc(sizeof(double) * (size_t)n + 8),
           *iv = malloc(sizeof(double) * (size_t)n + 8), *ar = malloc(sizeof(double) * (size_t)n + 8);
    int64_t* ust = malloc(sizeof(int64_t) * (size_t)n + 8);
    int64_t* t[4];
    for (int k = 0; k < 4; k++) t[k] = malloc(sizeof(int64_t) * (size_t)n + 8);
    int on = 0, niv = 0;
    for (int i = 0; i < n; i++) {
        const pev* e = &c->nm[i];
        int64_t target = base + 2 + (int64_t)(e->serial - ser[0]);
        on += (int64_t)e->msc == target;
        x[i] = (double)((int64_t)e->msc - (int64_t)c->nm[0].msc);
        y[i] = (double)((int64_t)e->ust - (int64_t)c->nm[0].ust) * 1000.0;
        ar[i] = (double)e->arr - (double)e->ust * 1000.0;
        ust[i] = (int64_t)e->ust;
        t[0][i] = e->arr;
        t[1][i] = e->arr_raw;
        t[2][i] = e->arr_real;
        t[3][i] = e->arr_boot;
        if (i > 0 && e->msc > c->nm[i - 1].msc)
            iv[niv++] = (double)((int64_t)e->ust - (int64_t)c->nm[i - 1].ust) * 1000.0 / (double)(e->msc - c->nm[i - 1].msc);
        pthread_mutex_lock(&g_lock);
        fprintf(g_vbl, "%s,%u,%" PRId64 ",%" PRIu64 ",%" PRIu64 ",%" PRId64 ",%" PRId64 ",%" PRId64 ",%" PRId64 "\n", c->label,
                e->serial, target, e->msc, e->ust, e->arr, e->arr_raw, e->arr_real, e->arr_boot);
        pthread_mutex_unlock(&g_lock);
    }
    say("  PresentNotifyMSC: %d of %d events, %d on the MSC asked for\n", n, M, on);
    double a = 0, b = 0;
    if (fit_line(x, y, n, &a, &b)) {
        double* res = malloc(sizeof(double) * (size_t)n + 8);
        for (int i = 0; i < n; i++) res[i] = fabs(y[i] - (a + b * x[i])) / 1e3;
        dist dr = dist_of(res, n);
        c->period_fit = b;
        say("  vblank period from UST fit: %.3f us (%.6f Hz); mode: %.3f us; difference %.1f ppm\n", b / 1e3, 1e9 / b,
            c->period_mode / 1e3, c->period_mode > 0 ? (b - c->period_mode) / c->period_mode * 1e6 : 0.0);
        say("  UST distance from that grid: p50 %.2f us, p99 %.2f us, max %.2f us\n", dr.p50, dr.p99, dr.max);
        free(res);
    }
    dist di = dist_of(iv, niv);
    say("  UST interval per vblank: min %.3f us, p50 %.3f us, max %.3f us\n", di.min / 1e3, di.p50 / 1e3, di.max / 1e3);
    dist da = dist_of(ar, n);
    say("  event received minus UST (CLOCK_MONOTONIC, UST as us): p50 %.1f us, p99 %.1f us, max %.1f us\n", da.p50 / 1e3,
        da.p99 / 1e3, da.max / 1e3);
    ust_report("PresentNotifyMSC UST", n, ust, t, ctx_period(c));
    free(ser);
    free(x);
    free(y);
    free(iv);
    free(ar);
    free(ust);
    for (int k = 0; k < 4; k++) free(t[k]);
}

/* ------------------------------------------------------------------ */
/* Swap runs                                                            */

enum { M_OML, M_SWAP_WAIT, M_SWAP_PIPE, M_SLEEP, M_VBLWAIT };
static const char* k_mname[] = {"oml", "swap_wait", "swap_pipe", "sleep", "vblwait"};

typedef struct {
    const char* name;
    int method, k, frames, late_every;
} spec;

typedef struct {
    int frame, late_inj;
    int64_t target, msc_prev, ust_prev;
    int64_t t_wake, t_call, t_ret, t_done;
    int64_t sbc, sbc_ret;
    int s_kind;                 /* 1: the swap's own values (OML wait); 2: current values (CHROMIUM) */
    int64_t s_ust, s_msc, s_sbc;
    int p_ok;
    uint32_t p_serial;
    int p_mode;
    uint64_t p_ust, p_msc;
    int64_t p_arr;
    int i_ok, i_type;
    int64_t i_ust, i_msc, i_sbc, i_arr;
} row;

typedef struct {
    int n, shown, mode[5], on, late, early, steps[4], late_inj, late_inj_late, serial_eq, serial_n, mode_changes;
    int intel_n, intel_flip, intel_copy, intel_exch;
    dist call, lat, known_s, known_p, grid, slack_on, slack_late;
    double period_fit, sp_maxdiff_us;
} rstats;

typedef struct {
    char label[128];
    spec s;
    row* rows;
    int n, ran;
    rstats st;
} runres;

static void intel_drain(ctx* c, runres* rr, int cap) {
    while (XPending(c->dpy)) {
        XEvent ev;
        XNextEvent(c->dpy, &ev);
        if (c->has_intel && ev.type == c->intel_ev) {
            const probe_swap_complete* e = (const probe_swap_complete*)&ev;
            int64_t idx = e->sbc - c->run_sbc0 - 1;
            if (rr && idx >= 0 && idx < cap) {
                row* r = &rr->rows[idx];
                r->i_ok = 1;
                r->i_type = e->event_type;
                r->i_ust = e->ust;
                r->i_msc = e->msc;
                r->i_sbc = e->sbc;
                r->i_arr = now_ns();
            }
        }
    }
}

static int row_onset(const row* r, int64_t* ns, int64_t* msc) {
    if (r->s_kind == 1) {
        *ns = r->s_ust * 1000;
        *msc = r->s_msc;
        return 1;
    }
    if (r->p_ok && r->p_mode != XCB_PRESENT_COMPLETE_MODE_SKIP) {
        *ns = (int64_t)r->p_ust * 1000;
        *msc = (int64_t)r->p_msc;
        return 1;
    }
    return 0;
}

static void run_stats(ctx* c, runres* rr) {
    rstats* st = &rr->st;
    memset(st, 0, sizeof *st);
    int n = rr->n;
    st->n = n;
    double *call = calloc((size_t)n + 1, sizeof(double)), *lat = calloc((size_t)n + 1, sizeof(double)),
           *ks = calloc((size_t)n + 1, sizeof(double)), *kp = calloc((size_t)n + 1, sizeof(double)),
           *gx = calloc((size_t)n + 1, sizeof(double)), *gy = calloc((size_t)n + 1, sizeof(double)),
           *so = calloc((size_t)n + 1, sizeof(double)), *sl = calloc((size_t)n + 1, sizeof(double));
    int ncall = 0, nlat = 0, nks = 0, nkp = 0, ng = 0, nso = 0, nsl = 0;
    double per = ctx_period(c);
    int64_t prev_msc = -1;
    int prev_mode = -1;
    int64_t o0 = 0, m0 = 0;
    for (int i = 0; i < n; i++) {
        const row* r = &rr->rows[i];
        call[ncall++] = (double)(r->t_ret - r->t_call) / 1e3;
        st->mode[r->p_ok ? (r->p_mode < 4 ? r->p_mode : 4) : 4]++;
        if (r->p_ok) {
            if (prev_mode >= 0 && r->p_mode != prev_mode) st->mode_changes++;
            prev_mode = r->p_mode;
            st->serial_n++;
            st->serial_eq += (int64_t)r->p_serial == (r->sbc & 0xffffffff);
        }
        if (r->i_ok) {
            st->intel_n++;
            st->intel_flip += r->i_type == P_GLX_FLIP_COMPLETE_INTEL;
            st->intel_copy += r->i_type == P_GLX_COPY_COMPLETE_INTEL;
            st->intel_exch += r->i_type == P_GLX_EXCHANGE_COMPLETE_INTEL;
        }
        st->late_inj += r->late_inj;
        int64_t on_ns, msc;
        if (!row_onset(r, &on_ns, &msc)) continue;
        st->shown++;
        if (!ng) {
            o0 = on_ns;
            m0 = msc;
        }
        gx[ng] = (double)(msc - m0);
        gy[ng] = (double)(on_ns - o0);
        ng++;
        lat[nlat++] = (double)(on_ns - r->t_ret) / 1e6;
        if (r->s_kind == 1 && r->t_done) ks[nks++] = (double)(r->t_done - on_ns) / 1e3;
        if (r->p_ok) kp[nkp++] = (double)(r->p_arr - (int64_t)r->p_ust * 1000) / 1e3;
        if (r->s_kind == 1 && r->p_ok) {
            double d = fabs((double)r->s_ust - (double)r->p_ust);
            if (d > st->sp_maxdiff_us) st->sp_maxdiff_us = d;
        }
        if (prev_msc >= 0) {
            int64_t d = msc - prev_msc;
            st->steps[d <= 0 ? 0 : d >= 3 ? 3 : (int)d]++;
        }
        prev_msc = msc;
        if (r->target > 0) {
            int64_t d = msc - r->target;
            double planned = (double)r->ust_prev + (double)(r->target - r->msc_prev) * per;
            double slack = (planned - (double)r->t_ret) / 1e6;
            if (d == 0) {
                st->on++;
                so[nso++] = slack;
            } else if (d > 0) {
                st->late++;
                sl[nsl++] = slack;
                st->late_inj_late += r->late_inj;
            } else {
                st->early++;
            }
        }
    }
    double a = 0, b = 0;
    if (fit_line(gx, gy, ng, &a, &b)) {
        st->period_fit = b;
        for (int i = 0; i < ng; i++) gy[i] = fabs(gy[i] - (a + b * gx[i])) / 1e3;
        st->grid = dist_of(gy, ng);
    } else {
        st->grid = dist_of(gy, 0);
    }
    st->call = dist_of(call, ncall);
    st->lat = dist_of(lat, nlat);
    st->known_s = dist_of(ks, nks);
    st->known_p = dist_of(kp, nkp);
    st->slack_on = dist_of(so, nso);
    st->slack_late = dist_of(sl, nsl);
    free(call);
    free(lat);
    free(ks);
    free(kp);
    free(gx);
    free(gy);
    free(so);
    free(sl);
}

static void csv_rows(ctx* c, runres* rr) {
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < rr->n; i++) {
        const row* r = &rr->rows[i];
        fprintf(g_swaps, "%s,%s,%d,%s,%d,%d,%d,%d,", rr->label, c->api == API_GLX ? "glx" : "egl", c->screen,
                k_mname[rr->s.method], rr->s.k, r->frame, r->late_inj, c->prog ? O.gpu_iters : 0);
        if (r->target > 0) fprintf(g_swaps, "%" PRId64, r->target);
        fprintf(g_swaps, ",%" PRId64 ",%" PRId64 ",", r->msc_prev, r->ust_prev);
        if (r->t_wake) fprintf(g_swaps, "%" PRId64, r->t_wake);
        fprintf(g_swaps, ",%" PRId64 ",%" PRId64 ",", r->t_call, r->t_ret);
        if (r->t_done) fprintf(g_swaps, "%" PRId64, r->t_done);
        fprintf(g_swaps, ",%" PRId64 ",", r->sbc);
        if (r->s_kind)
            fprintf(g_swaps, "%d,%" PRId64 ",%" PRId64 ",%" PRId64 ",", r->s_kind, r->s_ust, r->s_msc, r->s_sbc);
        else
            fprintf(g_swaps, ",,,,");
        if (r->p_ok)
            fprintf(g_swaps, "%u,%d,%" PRIu64 ",%" PRIu64 ",%" PRId64 ",", r->p_serial, r->p_mode, r->p_ust, r->p_msc, r->p_arr);
        else
            fprintf(g_swaps, ",,,,,");
        if (r->i_ok)
            fprintf(g_swaps, "0x%x,%" PRId64 ",%" PRId64 ",%" PRId64 ",%" PRId64 "\n", r->i_type, r->i_ust, r->i_msc, r->i_sbc,
                    r->i_arr);
        else
            fprintf(g_swaps, ",,,,\n");
    }
    fflush(g_swaps);
    pthread_mutex_unlock(&g_lock);
}

static void settle(ctx* c, int n) {
    for (int i = 0; i < n && !g_stop; i++) {
        draw(c, i);
        ctx_swap(c);
        c->sbc++;
        po_pump(c, 0);
    }
}

static void fill_present(ctx* c, row* r) {
    int64_t idx = r->sbc - c->run_sbc0 - 1;
    if (idx < 0 || idx >= c->npix) return;
    const pev* e = &c->pix[idx];
    r->p_ok = 1;
    r->p_serial = e->serial;
    r->p_mode = e->mode;
    r->p_ust = e->ust;
    r->p_msc = e->msc;
    r->p_arr = e->arr;
}

static void run_test(ctx* c, const spec* s, runres* rr) {
    memset(rr, 0, sizeof *rr);
    rr->s = *s;
    snprintf(rr->label, sizeof rr->label, "%s/%s", c->label, s->name);
    int need_present = s->method == M_SWAP_PIPE || s->method == M_VBLWAIT || !(c->api == API_GLX && c->has_sync);
    if (s->method == M_OML && !(c->api == API_GLX && c->has_sync)) return;
    if (need_present && !c->has_present) return;
    int n = s->frames;
    if (n > c->pix_cap - 16) n = c->pix_cap - 16;
    rr->rows = calloc((size_t)n + 1, sizeof(row));
    /* A whole probe run is longer than the default 10-minute screen saver
     * timeout, and a blanked screen would stop the vblanks being measured. */
    XResetScreenSaver(c->dpy);
    XFlush(c->dpy);
    settle(c, 10);
    anchor(c);
    double per = ctx_period(c);
    for (int i = 0; i < n && !g_stop; i++) {
        row* r = &rr->rows[i];
        r->frame = i;
        draw(c, i);
        if (s->late_every > 0 && i % s->late_every == s->late_every - 1) {
            spin_for((int64_t)(1.25 * per));
            r->late_inj = 1;
        }
        r->msc_prev = c->last_msc;
        r->ust_prev = c->last_ust;
        int64_t target = 0;
        if (s->method == M_OML || s->method == M_SLEEP || s->method == M_VBLWAIT) target = c->last_msc + s->k;
        else if (s->method == M_SWAP_WAIT) target = c->last_msc + 1;
        r->target = target;
        if (s->method == M_SLEEP) {
            int64_t tw = c->last_ust + (int64_t)((double)(target - 1 - c->last_msc) * per) + (int64_t)(O.sleep_margin_ms * 1e6);
            r->t_wake = tw;
            sleep_until(tw);
        } else if (s->method == M_VBLWAIT) {
            uint32_t ser = po_notify(c, (uint64_t)(target - 1));
            pev e;
            if (po_wait_nm(c, ser, now_ns() + (int64_t)((s->k + 3) * per), &e)) r->t_wake = e.arr;
        }
        r->t_call = now_ns();
        if (s->method == M_OML) r->sbc_ret = pSwapBuffersMscOML(c->dpy, c->gwin, target, 0, 0);
        else ctx_swap(c);
        r->t_ret = now_ns();
        c->sbc++;
        r->sbc = c->sbc;
        if (s->method != M_SWAP_PIPE) {
            if (c->api == API_GLX && c->has_sync) {
                int64_t u = 0, m = 0, sb = 0;
                int64_t want = r->sbc_ret > 0 ? r->sbc_ret : r->sbc;
                if (pWaitForSbcOML(c->dpy, c->gwin, want, &u, &m, &sb)) {
                    r->s_kind = 1;
                    r->s_ust = u;
                    r->s_msc = m;
                    r->s_sbc = sb;
                }
                r->t_done = now_ns();
                po_wait_pix(c, (int)(r->sbc - c->run_sbc0 - 1), now_ns() + 3000000);
            } else {
                po_wait_pix(c, (int)(r->sbc - c->run_sbc0 - 1), now_ns() + (int64_t)(6 * per));
                r->t_done = now_ns();
                if (c->has_sync) {
                    int64_t u = 0, m = 0, sb = 0;
                    if (ctx_sync_values(c, &u, &m, &sb)) {
                        r->s_kind = 2;
                        r->s_ust = u;
                        r->s_msc = m;
                        r->s_sbc = sb;
                    }
                }
            }
            fill_present(c, r);
            if (c->has_intel) intel_drain(c, rr, n);
            int64_t on_ns, msc;
            if (row_onset(r, &on_ns, &msc)) {
                c->last_msc = msc;
                c->last_ust = on_ns;
            } else {
                int64_t m = target > 0 ? target : c->last_msc + 1;
                c->last_ust += (int64_t)((double)(m - c->last_msc) * per);
                c->last_msc = m;
            }
        } else {
            po_pump(c, 0);
            if (c->has_intel) intel_drain(c, rr, n);
        }
        rr->n = i + 1;
    }
    if (s->method == M_SWAP_PIPE && rr->n > 0) {
        po_wait_pix(c, rr->n - 1, now_ns() + (int64_t)(10 * per));
        for (int i = 0; i < rr->n; i++) {
            fill_present(c, &rr->rows[i]);
            rr->rows[i].t_done = rr->rows[i].p_arr;
        }
    }
    if (c->has_intel) {
        usleep((useconds_t)(2 * per / 1000));
        intel_drain(c, rr, n);
    }
    rr->ran = 1;
    run_stats(c, rr);
    csv_rows(c, rr);
}

static void print_run_header(void) {
    say("\n| run | swaps | shown | Present mode flip/copy/subopt/skip/none | mode changes | on target | late | early "
        "| MSC step 0/1/2/3+ | swap call us p50/p99/max | onset minus return ms p50/p99/max "
        "| known after onset us, sync p50/p99; Present p50/p99 | grid residual us p99/max "
        "| slack ms: min on time; max late |\n");
    say("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|\n");
}

static void print_run(const runres* rr) {
    if (!rr->ran) {
        say("| %s | skipped (%s) |||||||||||||\n", rr->label,
            rr->s.method == M_OML ? "no OML sync control" : "needs Present events");
        return;
    }
    const rstats* st = &rr->st;
    int targeted = rr->s.method != M_SWAP_PIPE;
    char on[32] = "-", late[48] = "-", early[32] = "-";
    if (targeted) {
        snprintf(on, sizeof on, "%d", st->on);
        if (st->late_inj)
            snprintf(late, sizeof late, "%d (%d of %d injected)", st->late, st->late_inj_late, st->late_inj);
        else
            snprintf(late, sizeof late, "%d", st->late);
        snprintf(early, sizeof early, "%d", st->early);
    }
    say("| %s | %d | %d | %d/%d/%d/%d/%d | %d | %s | %s | %s | %d/%d/%d/%d | %.0f/%.0f/%.0f | %.2f/%.2f/%.2f "
        "| %.0f/%.0f; %.0f/%.0f | %.1f/%.1f | %.2f; %.2f |\n",
        rr->label, st->n, st->shown, st->mode[1], st->mode[0], st->mode[3], st->mode[2], st->mode[4], st->mode_changes, on,
        late, early, st->steps[0], st->steps[1], st->steps[2], st->steps[3], st->call.p50, st->call.p99, st->call.max,
        st->lat.p50, st->lat.p99, st->lat.max, st->known_s.p50, st->known_s.p99, st->known_p.p50, st->known_p.p99,
        st->grid.p99, st->grid.max, st->slack_on.min, st->slack_late.max);
}

static void print_run_notes(const runres* rr) {
    if (!rr->ran) return;
    const rstats* st = &rr->st;
    say("  %s: Present serial equals the swap count on %d of %d", rr->label, st->serial_eq, st->serial_n);
    if (st->sp_maxdiff_us > 0 || rr->s.method == M_OML) say("; OML UST minus Present UST max %.0f us", st->sp_maxdiff_us);
    if (st->intel_n)
        say("; INTEL_swap_event %d (flip %d, copy %d, exchange %d)", st->intel_n, st->intel_flip, st->intel_copy, st->intel_exch);
    if (st->period_fit > 0) say("; period from this run %.3f us", st->period_fit / 1e3);
    say("\n");
}

/* ------------------------------------------------------------------ */
/* Suites                                                               */

static int make_specs(spec* out, int api_glx_sync) {
    int N = O.frames, k = 0;
    if (O.quick) {
        if (api_glx_sync) out[k++] = (spec){"oml_k1", M_OML, 1, N, 0};
        out[k++] = (spec){"swap_wait", M_SWAP_WAIT, 1, N, 0};
        out[k++] = (spec){"swap_pipe", M_SWAP_PIPE, 1, N, 0};
        return k;
    }
    if (api_glx_sync) {
        out[k++] = (spec){"oml_k1", M_OML, 1, N, 0};
        out[k++] = (spec){"oml_k2", M_OML, 2, N / 2, 0};
        out[k++] = (spec){"oml_k3", M_OML, 3, N / 3, 0};
    }
    out[k++] = (spec){"swap_wait", M_SWAP_WAIT, 1, N, 0};
    out[k++] = (spec){"swap_pipe", M_SWAP_PIPE, 1, N, 0};
    out[k++] = (spec){"sleep_k2", M_SLEEP, 2, N / 2, 0};
    out[k++] = (spec){"vblwait_k2", M_VBLWAIT, 2, N / 2, 0};
    if (api_glx_sync) out[k++] = (spec){"oml_k1_late", M_OML, 1, N, O.late_every > 0 ? O.late_every : 30};
    else out[k++] = (spec){"swap_wait_late", M_SWAP_WAIT, 1, N, O.late_every > 0 ? O.late_every : 30};
    return k;
}

static void report_window_state(ctx* c) {
    long v = 0;
    if (prop_long(c->dpy, c->win, "_VARIABLE_REFRESH", &v))
        say("  _VARIABLE_REFRESH on the window (set by Mesa when its adaptive_sync option is on): %ld\n", v);
    else
        say("  _VARIABLE_REFRESH on the window: not set\n");
    say("  window %dx%d+%d+%d, %s, CRTC outputs %s, mode period %.3f us\n", c->w, c->h, c->x, c->y,
        c->rect_mode == RECT_WINDOW ? "a normal window" : O.wm ? "fullscreen through the window manager" : "override-redirect",
        c->outs && c->outs[0] ? c->outs : "-", c->period_mode / 1e3);
    say("  Present events for this context's swaps: %s\n",
        c->pix_total > 0 ? "yes (the driver presents through DRI3/Present)" : "none seen (DRI2, or no Present)");
}

static void run_list(ctx* c, const spec* sp, int ns) {
    runres* rr = calloc((size_t)ns, sizeof(runres));
    print_run_header();
    for (int i = 0; i < ns && !g_stop; i++) {
        run_test(c, &sp[i], &rr[i]);
        print_run(&rr[i]);
    }
    for (int i = 0; i < ns; i++) print_run_notes(&rr[i]);
    for (int i = 0; i < ns; i++) free(rr[i].rows);
    free(rr);
}

static void glx_suite(int scr) {
    say("\n## 2-4. GLX on screen %d: sync control, clocks, swaps, Present\n\n", scr);
    ctx c;
    ctx_zero(&c, scr, "glx");
    if (!glx_ctx_open(&c, RECT_FULL, 1)) {
        ctx_close(&c);
        return;
    }
    try_rt("timing thread");
    settle(&c, 60);
    anchor(&c);
    report_window_state(&c);
    say("\n### Clocks\n\n");
    clock_test(&c);
    if (c.tainted) {
        ctx_close(&c);
        return;
    }
    vblank_test(&c);
    say("\n### Swaps, fullscreen\n");
    spec sp[16];
    int ns = make_specs(sp, c.has_sync);
    run_list(&c, sp, ns);
    if (!g_stop) {
        say("\n### Controls: a 640 x 480 window (expected: copy), and one output of several\n");
        if (glx_retarget(&c, RECT_WINDOW)) {
            spec ctl[2] = {{"window_swap_wait", M_SWAP_WAIT, 1, O.frames / 2, 0}, {"window_oml_k1", M_OML, 1, O.frames / 2, 0}};
            report_window_state(&c);
            run_list(&c, ctl, c.has_sync ? 2 : 1);
        }
        if (active_crtcs(scr) >= 2 && !g_stop && glx_retarget(&c, RECT_CRTC0)) {
            spec ctl[1] = {{"output0_swap_wait", M_SWAP_WAIT, 1, O.frames / 2, 0}};
            if (c.has_sync) ctl[0] = (spec){"output0_oml_k1", M_OML, 1, O.frames / 2, 0};
            report_window_state(&c);
            run_list(&c, ctl, 1);
        }
    }
    ctx_close(&c);
}

static void egl_suite(int scr, const char* egl_path, const char* gles_path, int angle_type, const char* tag) {
    egl_lib* E = calloc(1, sizeof *E);
    say("\n## 5. EGL on X11, screen %d: %s (%s)\n\n", scr, tag, egl_path);
    if (!egl_load(E, egl_path, gles_path)) {
        free(E);
        return;
    }
    ctx c;
    ctx_zero(&c, scr, tag);
    c.is_angle = angle_type != 0;
    if (!egl_ctx_open(&c, E, angle_type, RECT_FULL)) {
        ctx_close(&c);
        return;
    }
    try_rt("timing thread");
    settle(&c, 60);
    anchor(&c);
    report_window_state(&c);
    clock_test(&c);
    if (c.tainted) {
        ctx_close(&c);
        return;
    }
    spec sp[16];
    int ns = make_specs(sp, 0);
    run_list(&c, sp, ns);
    /* Last, so a driver that crashes here costs no measurement. Mesa's
     * copy is not called: it crashed on Mesa 23.2 and left locks held. */
    if (c.egl_rate && c.is_angle && !c.tainted && guarded(msc_rate_call, &c, "eglGetMscRateANGLE") < 0) c.tainted = 1;
    else if (c.egl_rate && !c.is_angle) say("  eglGetMscRateANGLE: advertised; not called on Mesa\n");
    ctx_close(&c);
    /* The driver library stays loaded: unloading some drivers crashes. */
}

static void angle_suite(int scr) {
    char egl[600], gles[600];
    snprintf(egl, sizeof egl, "%s/libEGL.so", O.angle_dir);
    snprintf(gles, sizeof gles, "%s/libGLESv2.so", O.angle_dir);
    struct { int type; const char* name; } types[] = {{P_EGL_PLATFORM_ANGLE_TYPE_VULKAN_ANGLE, "angle-vulkan"},
                                                      {P_EGL_PLATFORM_ANGLE_TYPE_OPENGL_ANGLE, "angle-gl"}};
    for (int i = 0; i < 2 && !g_stop; i++) egl_suite(scr, egl, gles, types[i].type, types[i].name);
}

/* ------------------------------------------------------------------ */
/* Two screens at once                                                  */

enum { R_IDLE, R_TIMED, R_UNTIMED };
static const int k_roles[4][2] = {{R_TIMED, R_IDLE}, {R_IDLE, R_TIMED}, {R_TIMED, R_TIMED}, {R_TIMED, R_UNTIMED}};
static const char* k_phase_names[4] = {"A: screen 0 alone", "B: screen 1 alone", "C: both timed at once",
                                       "D: screen 0 timed, screen 1 untimed (swap interval 0, as fast as it goes)"};
static pthread_barrier_t g_bar;
static int g_phase_left[4];
static int g_phase_stop[4];
static int g_both_ok[2];

typedef struct {
    int idx, scr;
    ctx c;
    runres res[4];
    int64_t untimed_swaps[4], untimed_modes[4][4];
    double untimed_secs[4];
} both_t;

static void* both_thread(void* arg) {
    both_t* b = arg;
    ctx_zero(&b->c, b->scr, "glx-both");
    g_both_ok[b->idx] = glx_ctx_open(&b->c, RECT_FULL, 0);
    if (g_both_ok[b->idx]) {
        try_rt(b->idx ? "screen 1 thread" : "screen 0 thread");
        settle(&b->c, 60);
        anchor(&b->c);
    }
    pthread_barrier_wait(&g_bar);
    int all = g_both_ok[0] && g_both_ok[1];
    for (int p = 0; p < 4 && all; p++) {
        pthread_barrier_wait(&g_bar);
        int role = k_roles[p][b->idx];
        if (role == R_TIMED && !g_stop) {
            spec s = b->c.has_sync ? (spec){"oml_k1", M_OML, 1, O.frames, 0} : (spec){"swap_wait", M_SWAP_WAIT, 1, O.frames, 0};
            run_test(&b->c, &s, &b->res[p]);
            snprintf(b->res[p].label, sizeof b->res[p].label, "s%d/glx/phase%c/%s", b->scr, 'A' + p, b->c.has_sync ? "oml_k1" : "swap_wait");
            if (__atomic_sub_fetch(&g_phase_left[p], 1, __ATOMIC_SEQ_CST) == 0) __atomic_store_n(&g_phase_stop[p], 1, __ATOMIC_SEQ_CST);
        } else if (role == R_UNTIMED) {
            int64_t m0[4];
            memcpy(m0, b->c.mode_count, sizeof m0);
            ctx_interval(&b->c, 0);
            int64_t t0 = now_ns(), k = 0;
            while (!__atomic_load_n(&g_phase_stop[p], __ATOMIC_SEQ_CST) && !g_stop) {
                draw(&b->c, (int)k++);
                ctx_swap(&b->c);
                b->c.sbc++;
                po_pump(&b->c, 0);
                intel_drain(&b->c, NULL, 0);
            }
            b->untimed_secs[p] = (double)(now_ns() - t0) / 1e9;
            ctx_interval(&b->c, 1);
            b->untimed_swaps[p] = k;
            for (int q = 0; q < 4; q++) b->untimed_modes[p][q] = b->c.mode_count[q] - m0[q];
        }
        pthread_barrier_wait(&g_bar);
    }
    return NULL;
}

static void cross_screen(const runres* a, const runres* b) {
    int64_t* tb = calloc((size_t)b->n + 1, sizeof(int64_t));
    int nb = 0;
    for (int j = 0; j < b->n; j++) {
        int64_t t, m;
        if (row_onset(&b->rows[j], &t, &m)) tb[nb++] = t;
    }
    double *d = calloc((size_t)a->n + 1, sizeof(double)), *tx = calloc((size_t)a->n + 1, sizeof(double)),
           *ty = calloc((size_t)a->n + 1, sizeof(double));
    int n = 0, j = 0;
    int64_t t0 = 0;
    for (int i = 0; i < a->n && nb > 0; i++) {
        int64_t ta, ma;
        if (!row_onset(&a->rows[i], &ta, &ma)) continue;
        if (!n) t0 = ta;
        while (j + 1 < nb && llabs(tb[j + 1] - ta) <= llabs(tb[j] - ta)) j++;
        d[n] = (double)(tb[j] - ta) / 1e3;
        tx[n] = (double)(ta - t0) / 1e9;
        ty[n] = d[n];
        n++;
    }
    double ia = 0, ib = 0;
    int fit = fit_line(tx, ty, n, &ia, &ib);
    dist dd = dist_of(d, n);
    say("  screen 1 onset minus nearest screen 0 onset (phase C): %d pairs, p50 %.1f us, min %.1f us, max %.1f us", n, dd.p50,
        dd.min, dd.max);
    if (fit) say(", drift %.3f us per second (%.3f ppm)", ib, ib);
    say("\n  (CRTCs that are not locked drift and wrap around the period; a steady value means they run in step)\n");
    free(tb);
    free(d);
    free(tx);
    free(ty);
}

static void both_phases(void) {
    say("\n## 7. Two X screens at once: screens 0 and 1, one connection and one thread each\n\n");
    if (g_nscreens < 2) {
        say("Only %d X screen: skipped.\n", g_nscreens);
        return;
    }
    both_t b[2];
    memset(b, 0, sizeof b);
    for (int p = 0; p < 4; p++) {
        g_phase_left[p] = (k_roles[p][0] == R_TIMED) + (k_roles[p][1] == R_TIMED);
        g_phase_stop[p] = 0;
    }
    pthread_barrier_init(&g_bar, NULL, 2);
    pthread_t th[2];
    for (int i = 0; i < 2; i++) {
        b[i].idx = i;
        b[i].scr = i;
        pthread_create(&th[i], NULL, both_thread, &b[i]);
    }
    for (int i = 0; i < 2; i++) pthread_join(th[i], NULL);
    pthread_barrier_destroy(&g_bar);
    if (!g_both_ok[0] || !g_both_ok[1]) {
        say("Could not open GLX on both screens (screen 0 %s, screen 1 %s).\n", g_both_ok[0] ? "ok" : "failed",
            g_both_ok[1] ? "ok" : "failed");
    } else {
        for (int p = 0; p < 4; p++) {
            say("\n### Phase %s\n", k_phase_names[p]);
            print_run_header();
            for (int i = 0; i < 2; i++)
                if (k_roles[p][i] == R_TIMED) print_run(&b[i].res[p]);
            for (int i = 0; i < 2; i++)
                if (k_roles[p][i] == R_TIMED) print_run_notes(&b[i].res[p]);
            for (int i = 0; i < 2; i++)
                if (k_roles[p][i] == R_UNTIMED) {
                    const int64_t* m = b[i].untimed_modes[p];
                    say("  screen %d untimed: %" PRId64 " swaps in %.1f s (%.0f per second); Present mode flip %" PRId64 ", copy %" PRId64
                        ", subopt %" PRId64 ", skip %" PRId64 "\n",
                        i, b[i].untimed_swaps[p], b[i].untimed_secs[p],
                        b[i].untimed_secs[p] > 0 ? (double)b[i].untimed_swaps[p] / b[i].untimed_secs[p] : 0.0, m[1], m[0], m[3], m[2]);
                }
            if (p == 2) cross_screen(&b[0].res[2], &b[1].res[2]);
        }
    }
    for (int i = 0; i < 2; i++) {
        for (int p = 0; p < 4; p++) free(b[i].res[p].rows);
        ctx_close(&b[i].c);
    }
}

/* ------------------------------------------------------------------ */
/* Environment                                                          */

static void env_section(Display* d) {
    say("\n## 1. Environment\n\n");
    int vr = VendorRelease(d);
    say("X server: vendor \"%s\", release %d", ServerVendor(d), vr);
    if (strstr(ServerVendor(d), "X.Org")) say(" (X.Org %d.%d.%d)", vr / 10000000, (vr / 100000) % 100, (vr / 1000) % 100);
    say(", protocol %d.%d\n", ProtocolVersion(d), ProtocolRevision(d));
    say("display \"%s\", %d screens, default screen %d\n", DisplayString(d), ScreenCount(d), DefaultScreen(d));
    static const char* envs[] = {"DISPLAY", "XDG_SESSION_TYPE", "XDG_CURRENT_DESKTOP", "WAYLAND_DISPLAY", "vblank_mode",
                                 "adaptive_sync", "LIBGL_DRI3_DISABLE", "LIBGL_ALWAYS_SOFTWARE", "MESA_LOADER_DRIVER_OVERRIDE",
                                 "__GLX_VENDOR_LIBRARY_NAME", "__EGL_VENDOR_LIBRARY_FILENAMES", "YSP_ANGLE_DIR"};
    for (size_t i = 0; i < sizeof envs / sizeof envs[0]; i++) {
        const char* v = getenv(envs[i]);
        if (v) say("env %s=%s\n", envs[i], v);
    }
    int ne = 0;
    char** el = XListExtensions(d, &ne);
    char all[4096] = "";
    int len = 0;
    for (int i = 0; i < ne && len < (int)sizeof all - 64; i++)
        len += snprintf(all + len, sizeof all - (size_t)len, "%s%s", i ? " " : "", el[i]);
    if (el) XFreeExtensionList(el);
    say("server extensions: %s\n", all);
    static const char* flag[] = {"Present", "DRI3", "DRI2", "Composite", "RANDR", "GLX", "XFIXES", "SYNC",
                                 "XInputExtension", "NV-GLX", "MIT-SHM"};
    for (size_t i = 0; i < sizeof flag / sizeof flag[0]; i++) {
        int op, ev, er;
        say("  %-16s %s\n", flag[i], yesno(XQueryExtension(d, flag[i], &op, &ev, &er)));
    }
    xcb_connection_t* xc = xcb_connect(DisplayString(d), NULL);
    if (xc && !xcb_connection_has_error(xc)) {
        const xcb_query_extension_reply_t* e = xcb_get_extension_data(xc, &xcb_present_id);
        if (e && e->present) {
            xcb_present_query_version_reply_t* pv = xcb_present_query_version_reply(xc, xcb_present_query_version(xc, 1, 4), NULL);
            if (pv) say("Present %u.%u\n", pv->major_version, pv->minor_version);
            free(pv);
        }
    }
    if (xc) xcb_disconnect(xc);
    for (int s = 0; s < ScreenCount(d); s++) {
        Window root = RootWindow(d, s);
        XVisualInfo tmpl;
        int n30 = 0, nall = 0;
        memset(&tmpl, 0, sizeof tmpl);
        tmpl.screen = s;
        tmpl.depth = 30;
        tmpl.class = TrueColor;
        XVisualInfo* vi = XGetVisualInfo(d, VisualScreenMask | VisualDepthMask | VisualClassMask, &tmpl, &n30);
        if (vi) XFree(vi);
        vi = XGetVisualInfo(d, VisualScreenMask, &tmpl, &nall);
        if (vi) XFree(vi);
        char cm[32];
        snprintf(cm, sizeof cm, "_NET_WM_CM_S%d", s);
        Window owner = XGetSelectionOwner(d, A(d, cm));
        char wm[256] = "";
        long chk = 0;
        if (prop_long(d, root, "_NET_SUPPORTING_WM_CHECK", &chk) && chk) {
            t_xerr = 0;
            if (!prop_text(d, (Window)chk, "_NET_WM_NAME", wm, sizeof wm)) prop_text(d, (Window)chk, "WM_NAME", wm, sizeof wm);
            XSync(d, False);
        }
        say("\nX screen %d: %dx%d px, %dx%d mm, root depth %d, %d visuals, %d of them 30-bit TrueColor\n", s,
            DisplayWidth(d, s), DisplayHeight(d, s), DisplayWidthMM(d, s), DisplayHeightMM(d, s), DefaultDepth(d, s), nall, n30);
        say("  compositing manager (_NET_WM_CM_S%d owner): %s", s, owner ? "yes" : "none");
        if (owner) say(", window 0x%lx", (unsigned long)owner);
        say("\n  window manager: %s\n", wm[0] ? wm : chk ? "(unnamed)" : "none found");
    }
    say("\n");
    randr_scan(DisplayString(d), 1);
}

/* ------------------------------------------------------------------ */
/* Vulkan                                                               */

#if PROBE_VULKAN
static const char* vk_mode_name(VkPresentModeKHR m) {
    switch ((int)m) {
    case 0: return "IMMEDIATE";
    case 1: return "MAILBOX";
    case 2: return "FIFO";
    case 3: return "FIFO_RELAXED";
    case 1000111000: return "SHARED_DEMAND_REFRESH";
    case 1000111001: return "SHARED_CONTINUOUS_REFRESH";
    case 1000361000: return "FIFO_LATEST_READY";
    default: return "other";
    }
}

static void vk_section(Display* d) {
    say("\n## 6. Vulkan\n\n");
    void* h = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        say("libvulkan.so.1 not loadable (%s): skipped\n", dlerror());
        return;
    }
    PFN_vkGetInstanceProcAddr gipa;
    void* sym = dlsym(h, "vkGetInstanceProcAddr");
    memcpy(&gipa, &sym, sizeof gipa);
    if (!gipa) {
        say("vkGetInstanceProcAddr missing: skipped\n");
        return;
    }
#define VKI(inst, name) PFN_##name name = (PFN_##name)gipa(inst, #name)
    VKI(NULL, vkEnumerateInstanceExtensionProperties);
    VKI(NULL, vkCreateInstance);
    uint32_t ni = 0;
    vkEnumerateInstanceExtensionProperties(NULL, &ni, NULL);
    VkExtensionProperties* ie = calloc(ni + 1, sizeof *ie);
    vkEnumerateInstanceExtensionProperties(NULL, &ni, ie);
    char* buf = calloc(ni + 1, 128);
    for (uint32_t i = 0; i < ni; i++) {
        strcat(buf, ie[i].extensionName);
        strcat(buf, " ");
    }
    ext_dump("Vulkan instance extensions", buf);
    static const char* want_i[] = {"VK_KHR_surface", "VK_KHR_xlib_surface", "VK_KHR_xcb_surface", "VK_KHR_display",
                                   "VK_EXT_acquire_xlib_display", "VK_EXT_direct_mode_display", "VK_EXT_display_surface_counter",
                                   "VK_KHR_get_surface_capabilities2", "VK_EXT_surface_maintenance1",
                                   "VK_KHR_get_display_properties2", "VK_KHR_get_physical_device_properties2"};
    const char* en[16];
    uint32_t nen = 0;
    for (size_t i = 0; i < sizeof want_i / sizeof want_i[0]; i++) {
        int have = has_ext(buf, want_i[i]);
        say("  instance %-40s %s\n", want_i[i], yesno(have));
        if (have && strcmp(want_i[i], "VK_KHR_xcb_surface")) en[nen++] = want_i[i];
    }
    VkApplicationInfo app;
    memset(&app, 0, sizeof app);
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "ysp x11 probe";
    app.apiVersion = VK_MAKE_VERSION(1, 1, 0);
    VkInstanceCreateInfo ci;
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = nen;
    ci.ppEnabledExtensionNames = en;
    VkInstance inst;
    VkResult vr = vkCreateInstance(&ci, NULL, &inst);
    if (vr != VK_SUCCESS) {
        say("vkCreateInstance: %d\n", (int)vr);
        free(ie);
        free(buf);
        return;
    }
    VKI(inst, vkEnumeratePhysicalDevices);
    VKI(inst, vkGetPhysicalDeviceProperties);
    VKI(inst, vkGetPhysicalDeviceProperties2);
    VKI(inst, vkEnumerateDeviceExtensionProperties);
    VKI(inst, vkDestroyInstance);
    VKI(inst, vkCreateXlibSurfaceKHR);
    VKI(inst, vkDestroySurfaceKHR);
    VKI(inst, vkGetPhysicalDeviceSurfacePresentModesKHR);
    VKI(inst, vkGetPhysicalDeviceSurfaceFormatsKHR);
    VKI(inst, vkGetPhysicalDeviceSurfaceSupportKHR);
    VKI(inst, vkGetPhysicalDeviceQueueFamilyProperties);
    VKI(inst, vkGetPhysicalDeviceDisplayPropertiesKHR);
#undef VKI
    VkSurfaceKHR surf = VK_NULL_HANDLE;
    Window w = 0;
    if (vkCreateXlibSurfaceKHR && has_ext(buf, "VK_KHR_xlib_surface")) {
        w = XCreateSimpleWindow(d, RootWindow(d, DefaultScreen(d)), 0, 0, 64, 64, 0, 0, 0);
        XSync(d, False);
        VkXlibSurfaceCreateInfoKHR sci;
        memset(&sci, 0, sizeof sci);
        sci.sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR;
        sci.dpy = d;
        sci.window = w;
        if (vkCreateXlibSurfaceKHR(inst, &sci, NULL, &surf) != VK_SUCCESS) surf = VK_NULL_HANDLE;
    }
    uint32_t np = 0;
    vkEnumeratePhysicalDevices(inst, &np, NULL);
    VkPhysicalDevice* pd = calloc(np + 1, sizeof *pd);
    vkEnumeratePhysicalDevices(inst, &np, pd);
    for (uint32_t i = 0; i < np; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(pd[i], &p);
        say("\ndevice %u: \"%s\", type %d, Vulkan %u.%u.%u, vendor 0x%04x, device 0x%04x\n", i, p.deviceName, (int)p.deviceType,
            VK_VERSION_MAJOR(p.apiVersion), VK_VERSION_MINOR(p.apiVersion), VK_VERSION_PATCH(p.apiVersion), p.vendorID,
            p.deviceID);
#ifdef VK_API_VERSION_1_2
        if (vkGetPhysicalDeviceProperties2 && p.apiVersion >= VK_MAKE_VERSION(1, 2, 0)) {
            VkPhysicalDeviceDriverProperties dp;
            memset(&dp, 0, sizeof dp);
            dp.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
            VkPhysicalDeviceProperties2 p2;
            memset(&p2, 0, sizeof p2);
            p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
            p2.pNext = &dp;
            vkGetPhysicalDeviceProperties2(pd[i], &p2);
            say("  driver \"%s\", \"%s\", id %d\n", dp.driverName, dp.driverInfo, (int)dp.driverID);
        }
#endif
        uint32_t nd = 0;
        vkEnumerateDeviceExtensionProperties(pd[i], NULL, &nd, NULL);
        VkExtensionProperties* de = calloc(nd + 1, sizeof *de);
        vkEnumerateDeviceExtensionProperties(pd[i], NULL, &nd, de);
        char* db = calloc(nd + 1, 128);
        for (uint32_t k = 0; k < nd; k++) {
            strcat(db, de[k].extensionName);
            strcat(db, " ");
        }
        char what[300];
        snprintf(what, sizeof what, "Vulkan device extensions, %s", p.deviceName);
        ext_dump(what, db);
        static const char* want_d[] = {"VK_KHR_swapchain", "VK_KHR_present_id", "VK_KHR_present_wait", "VK_KHR_present_id2",
                                       "VK_KHR_present_wait2", "VK_GOOGLE_display_timing", "VK_EXT_present_timing",
                                       "VK_EXT_display_control", "VK_EXT_swapchain_maintenance1", "VK_KHR_display_swapchain",
                                       "VK_EXT_hdr_metadata", "VK_AMD_display_native_hdr", "VK_KHR_calibrated_timestamps",
                                       "VK_EXT_calibrated_timestamps"};
        for (size_t k = 0; k < sizeof want_d / sizeof want_d[0]; k++) say("  %-36s %s\n", want_d[k], yesno(has_ext(db, want_d[k])));
        if (surf) {
            uint32_t nq = 0;
            VkBool32 any = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(pd[i], &nq, NULL);
            for (uint32_t q = 0; q < nq && !any; q++) vkGetPhysicalDeviceSurfaceSupportKHR(pd[i], q, surf, &any);
            say("  presents to an X11 window on this screen: %s\n", yesno(any));
            if (any) {
                uint32_t nm = 0;
                vkGetPhysicalDeviceSurfacePresentModesKHR(pd[i], surf, &nm, NULL);
                VkPresentModeKHR* pm = calloc(nm + 1, sizeof *pm);
                vkGetPhysicalDeviceSurfacePresentModesKHR(pd[i], surf, &nm, pm);
                say("  present modes:");
                for (uint32_t k = 0; k < nm; k++) say(" %s", vk_mode_name(pm[k]));
                say("\n");
                free(pm);
                uint32_t nf = 0;
                vkGetPhysicalDeviceSurfaceFormatsKHR(pd[i], surf, &nf, NULL);
                VkSurfaceFormatKHR* sf = calloc(nf + 1, sizeof *sf);
                vkGetPhysicalDeviceSurfaceFormatsKHR(pd[i], surf, &nf, sf);
                say("  surface formats: %u; 10 bpc:", nf);
                int n10 = 0;
                for (uint32_t k = 0; k < nf; k++)
                    if (sf[k].format == VK_FORMAT_A2R10G10B10_UNORM_PACK32 || sf[k].format == VK_FORMAT_A2B10G10R10_UNORM_PACK32) {
                        say(" %s/colorspace %d", sf[k].format == VK_FORMAT_A2R10G10B10_UNORM_PACK32 ? "A2R10G10B10" : "A2B10G10R10",
                            (int)sf[k].colorSpace);
                        n10++;
                    }
                say("%s\n", n10 ? "" : " none");
                free(sf);
            }
        }
        if (vkGetPhysicalDeviceDisplayPropertiesKHR) {
            uint32_t ndp = 0;
            VkResult r = vkGetPhysicalDeviceDisplayPropertiesKHR(pd[i], &ndp, NULL);
            say("  VK_KHR_display displays: %u (result %d)\n", ndp, (int)r);
            if (ndp && r == VK_SUCCESS) {
                VkDisplayPropertiesKHR* dpp = calloc(ndp + 1, sizeof *dpp);
                vkGetPhysicalDeviceDisplayPropertiesKHR(pd[i], &ndp, dpp);
                for (uint32_t k = 0; k < ndp; k++)
                    say("    \"%s\" %ux%u\n", dpp[k].displayName ? dpp[k].displayName : "?", dpp[k].physicalResolution.width,
                        dpp[k].physicalResolution.height);
                free(dpp);
            }
        }
        free(de);
        free(db);
    }
    if (surf) vkDestroySurfaceKHR(inst, surf, NULL);
    if (w) XDestroyWindow(d, w);
    vkDestroyInstance(inst, NULL);
    free(pd);
    free(ie);
    free(buf);
}
#else
static void vk_section(Display* d) {
    (void)d;
    say("\n## 6. Vulkan\n\nNot compiled: <vulkan/vulkan.h> was missing at build time. Install the Vulkan headers and build again.\n");
}
#endif

/* ------------------------------------------------------------------ */
/* SDL3, in a child process per variant                                 */

typedef struct SDL_Window_ SDL_Window_;
typedef struct { int x, y, w, h; } sdl_rect;

static void sdl_child(int force_egl, int display_index) {
    void* h = dlopen("libSDL3.so.0", RTLD_NOW | RTLD_LOCAL);
    if (!h) h = dlopen("libSDL3.so", RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        printf("libSDL3.so.0 not loadable (%s): skipped\n", dlerror());
        return;
    }
    bool (*SetHint)(const char*, const char*);
    bool (*Init)(uint32_t);
    void (*Quit)(void);
    const char* (*GetError)(void);
    int (*GetVersion)(void);
    const char* (*GetCurrentVideoDriver)(void);
    uint32_t* (*GetDisplays)(int*);
    const char* (*GetDisplayName)(uint32_t);
    bool (*GetDisplayBounds)(uint32_t, sdl_rect*);
    void (*Free)(void*);
    uint32_t (*CreateProperties)(void);
    bool (*SetNumberProperty)(uint32_t, const char*, int64_t);
    bool (*SetStringProperty)(uint32_t, const char*, const char*);
    bool (*SetBooleanProperty)(uint32_t, const char*, bool);
    void (*DestroyProperties)(uint32_t);
    SDL_Window_* (*CreateWindowWithProperties)(uint32_t);
    void (*DestroyWindow)(SDL_Window_*);
    bool (*ShowWindow)(SDL_Window_*);
    void (*PumpEvents)(void);
    uint32_t (*GetWindowProperties)(SDL_Window_*);
    int64_t (*GetNumberProperty)(uint32_t, const char*, int64_t);
    void* (*GetPointerProperty)(uint32_t, const char*, void*);
    uint32_t (*GetDisplayForWindow)(SDL_Window_*);
    bool (*GL_SetAttribute)(int, int);
    void* (*GL_CreateContext)(SDL_Window_*);
    bool (*GL_DestroyContext)(void*);
    vfn (*GL_GetProcAddress)(const char*);
#define S(dst, name) do { void* p_ = dlsym(h, name); memcpy(&(dst), &p_, sizeof(dst)); if (!p_) { printf("missing %s\n", name); return; } } while (0)
    S(SetHint, "SDL_SetHint");
    S(Init, "SDL_Init");
    S(Quit, "SDL_Quit");
    S(GetError, "SDL_GetError");
    S(GetVersion, "SDL_GetVersion");
    S(GetCurrentVideoDriver, "SDL_GetCurrentVideoDriver");
    S(GetDisplays, "SDL_GetDisplays");
    S(GetDisplayName, "SDL_GetDisplayName");
    S(GetDisplayBounds, "SDL_GetDisplayBounds");
    S(Free, "SDL_free");
    S(CreateProperties, "SDL_CreateProperties");
    S(SetNumberProperty, "SDL_SetNumberProperty");
    S(SetStringProperty, "SDL_SetStringProperty");
    S(SetBooleanProperty, "SDL_SetBooleanProperty");
    S(DestroyProperties, "SDL_DestroyProperties");
    S(CreateWindowWithProperties, "SDL_CreateWindowWithProperties");
    S(DestroyWindow, "SDL_DestroyWindow");
    S(ShowWindow, "SDL_ShowWindow");
    S(PumpEvents, "SDL_PumpEvents");
    S(GetWindowProperties, "SDL_GetWindowProperties");
    S(GetNumberProperty, "SDL_GetNumberProperty");
    S(GetPointerProperty, "SDL_GetPointerProperty");
    S(GetDisplayForWindow, "SDL_GetDisplayForWindow");
    S(GL_SetAttribute, "SDL_GL_SetAttribute");
    S(GL_CreateContext, "SDL_GL_CreateContext");
    S(GL_DestroyContext, "SDL_GL_DestroyContext");
    S(GL_GetProcAddress, "SDL_GL_GetProcAddress");
#undef S
    int v = GetVersion();
    printf("SDL %d.%d.%d, DISPLAY=%s\n", v / 1000000, v / 1000 % 1000, v % 1000, getenv("DISPLAY") ? getenv("DISPLAY") : "");
    SetHint("SDL_VIDEO_DRIVER", "x11");
    if (force_egl) SetHint("SDL_VIDEO_FORCE_EGL", "1");
    if (!Init(P_SDL_INIT_VIDEO)) {
        printf("RESULT: SDL_Init failed: %s\n", GetError());
        return;
    }
    printf("video driver %s\n", GetCurrentVideoDriver());
    int nd = 0;
    uint32_t* ids = GetDisplays(&nd);
    for (int i = 0; i < nd; i++) {
        sdl_rect r = {0, 0, 0, 0};
        GetDisplayBounds(ids[i], &r);
        printf("display index %d: id %u \"%s\" %dx%d+%d+%d\n", i, ids[i], GetDisplayName(ids[i]), r.w, r.h, r.x, r.y);
    }
    uint32_t id = ids && display_index < nd ? ids[display_index] : 0;
    if (ids) Free(ids);
    if (!id) {
        printf("RESULT: SDL lists no display with index %d\n", display_index);
        Quit();
        return;
    }
    if (force_egl) {
        GL_SetAttribute(P_SDL_GL_CONTEXT_PROFILE_MASK, P_SDL_GL_CONTEXT_PROFILE_ES);
        GL_SetAttribute(P_SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        GL_SetAttribute(P_SDL_GL_CONTEXT_MINOR_VERSION, 0);
    }
    uint32_t props = CreateProperties();
    SetStringProperty(props, "SDL.window.create.title", "ysp sdl probe");
    SetNumberProperty(props, "SDL.window.create.x", P_SDL_WINDOWPOS_CENTERED_DISPLAY(id));
    SetNumberProperty(props, "SDL.window.create.y", P_SDL_WINDOWPOS_CENTERED_DISPLAY(id));
    SetNumberProperty(props, "SDL.window.create.width", 320);
    SetNumberProperty(props, "SDL.window.create.height", 240);
    SetBooleanProperty(props, "SDL.window.create.opengl", true);
    SetBooleanProperty(props, "SDL.window.create.hidden", true);
    SDL_Window_* w = CreateWindowWithProperties(props);
    DestroyProperties(props);
    if (!w) {
        printf("RESULT: SDL_CreateWindowWithProperties failed: %s\n", GetError());
        Quit();
        return;
    }
    ShowWindow(w);
    for (int i = 0; i < 10; i++) {
        PumpEvents();
        usleep(20000);
    }
    uint32_t wp = GetWindowProperties(w);
    long long sdl_scr = (long long)GetNumberProperty(wp, "SDL.window.x11.screen", -1);
    unsigned long xw = (unsigned long)GetNumberProperty(wp, "SDL.window.x11.window", 0);
    Display* xd = GetPointerProperty(wp, "SDL.window.x11.display", NULL);
    int real = -1;
    if (xd && xw) {
        XWindowAttributes a;
        if (XGetWindowAttributes(xd, (Window)xw, &a)) real = XScreenNumberOfScreen(a.screen);
    }
    printf("window: SDL display id %u, SDL's x11 screen property %lld, the X server puts it on screen %d\n", GetDisplayForWindow(w),
           sdl_scr, real);
    void* gc = GL_CreateContext(w);
    char glr[300] = "";
    if (!gc) {
        printf("GL context: failed: %s\n", GetError());
    } else {
        const GLubyte* (*GetString)(GLenum);
        LOADFN(GetString, GL_GetProcAddress("glGetString"));
        if (GetString) snprintf(glr, sizeof glr, "%s | %s", (const char*)GetString(GL_RENDERER), (const char*)GetString(GL_VERSION));
        printf("GL context: ok, %s\n", glr);
        GL_DestroyContext(gc);
    }
    printf("RESULT: window on X screen %d, %s context %s\n", real, force_egl ? "EGL ES 3.0" : "GLX", gc ? "ok" : "FAILED");
    DestroyWindow(w);
    Quit();
}

static void sdl_variant(const char* label, const char* display, int force_egl, int display_index) {
    say("\n%s\n", label);
    int fd[2];
    if (pipe(fd)) {
        say("  pipe failed\n");
        return;
    }
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        close(fd[0]);
        dup2(fd[1], 1);
        dup2(fd[1], 2);
        if (display) setenv("DISPLAY", display, 1);
        setvbuf(stdout, NULL, _IOLBF, 0);
        sdl_child(force_egl, display_index);
        fflush(stdout);
        _exit(0);
    }
    close(fd[1]);
    char buf[8192];
    int len = 0;
    int64_t deadline = now_ns() + 20000000000LL;
    for (;;) {
        struct pollfd pf = {fd[0], POLLIN, 0};
        int64_t left = deadline - now_ns();
        if (left <= 0) {
            kill(pid, SIGKILL);
            say("  (killed after 20 s)\n");
            break;
        }
        if (poll(&pf, 1, (int)(left / 1000000) + 1) <= 0) continue;
        ssize_t r = read(fd[0], buf + len, sizeof buf - 1 - (size_t)len);
        if (r <= 0) break;
        len += (int)r;
        if (len >= (int)sizeof buf - 1) break;
    }
    close(fd[0]);
    buf[len] = 0;
    int st = 0;
    waitpid(pid, &st, 0);
    for (char* line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) say("  %s\n", line);
    if (WIFSIGNALED(st)) say("  child ended by signal %d\n", WTERMSIG(st));
}

static void sdl_section(void) {
    say("\n## 8. SDL3 on a non-default X screen\n");
    char d1[300];
    snprintf(d1, sizeof d1, "%s.1", g_base_name);
    sdl_variant("Variant 1: default DISPLAY, SDL display index 0, GLX", NULL, 0, 0);
    sdl_variant("Variant 2: default DISPLAY, SDL display index 0, EGL ES 3.0", NULL, 1, 0);
    if (g_nscreens < 2) {
        say("\nOnly one X screen: the non-default-screen variants need two.\n");
        return;
    }
    sdl_variant("Variant 3: DISPLAY=<display>.1, display index 0, GLX", d1, 0, 0);
    sdl_variant("Variant 4: DISPLAY=<display>.1, display index 0, EGL ES 3.0", d1, 1, 0);
    sdl_variant("Variant 5: default DISPLAY, display index 1 (a display on the other screen, if SDL lists it), GLX", NULL, 0, 1);
    sdl_variant("Variant 6: default DISPLAY, display index 1, EGL ES 3.0", NULL, 1, 1);
}

/* ------------------------------------------------------------------ */
/* Load and main                                                        */

static volatile int g_load_stop;
static void* load_fn(void* a) {
    (void)a;
    volatile uint64_t x = 1;
    while (!g_load_stop) x = x * 6364136223846793005ULL + 1442695040888963407ULL;
    return NULL;
}

static void on_signal(int s) {
    (void)s;
    g_stop = 1;
}

static void on_alarm(int s) {
    (void)s;
    static const char m[] = "\nscreen_x11_probe: time limit reached, exiting (windows close with the connection)\n";
    ssize_t r = write(2, m, sizeof m - 1);
    (void)r;
    _exit(3);
}

static void usage(void) {
    printf("usage: screen_x11_probe [options]\n"
           "  --out DIR         write summary.txt and the CSV files into DIR (default: ./ysp_x11_probe_<date>)\n"
           "  --frames N        swaps per run (default 600; runs that hold 2 or 3 vblanks use N/2 and N/3)\n"
           "  --vblanks N       PresentNotifyMSC events in the vblank test (default 300)\n"
           "  --screen N        run the GLX and EGL tests on X screen N only, no two-screen phases\n"
           "  --both            run only the two-screen phases (screens 0 and 1)\n"
           "  --no-both         skip the two-screen phases\n"
           "  --quick           fewer runs per API (oml_k1, swap_wait, swap_pipe)\n"
           "  --load N          N busy threads for the whole run\n"
           "  --gpu-heavy       a heavy fragment shader every frame (400 iterations per pixel)\n"
           "  --gpu-iters N     the same with N iterations\n"
           "  --late-every N    frame interval of the injected late frames (default 30)\n"
           "  --wm              fullscreen through the window manager (_NET_WM_STATE_FULLSCREEN), not override-redirect\n"
           "  --glx-compat      use a desktop GL context on GLX, not GL ES 3.0\n"
           "  --angle DIR       also run EGL through ANGLE's libEGL.so and libGLESv2.so in DIR (or set YSP_ANGLE_DIR)\n"
           "  --sleep-margin MS wake this long after the vblank before a held frame's swap (default 1.0)\n"
           "  --no-rt           do not ask for SCHED_FIFO\n"
           "  --skip-env --skip-egl --skip-sdl --skip-vk\n"
           "  --timeout S       give up after S seconds (default 1800)\n");
}

int main(int argc, char** argv) {
    XInitThreads();
    O.angle_dir = getenv("YSP_ANGLE_DIR");
    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        const char* v = i + 1 < argc ? argv[i + 1] : NULL;
#define ARG(name) (!strcmp(a, name) && v && (++i, 1))
        if (ARG("--out")) O.out = v;
        else if (ARG("--frames")) O.frames = atoi(v);
        else if (ARG("--vblanks")) O.vblanks = atoi(v);
        else if (ARG("--screen")) O.screen = atoi(v);
        else if (ARG("--load")) O.load = atoi(v);
        else if (ARG("--gpu-iters")) O.gpu_iters = atoi(v);
        else if (ARG("--late-every")) O.late_every = atoi(v);
        else if (ARG("--angle")) O.angle_dir = v;
        else if (ARG("--sleep-margin")) O.sleep_margin_ms = atof(v);
        else if (ARG("--timeout")) O.timeout_s = atoi(v);
        else if (!strcmp(a, "--both")) O.both_only = 1;
        else if (!strcmp(a, "--no-both")) O.no_both = 1;
        else if (!strcmp(a, "--quick")) O.quick = 1;
        else if (!strcmp(a, "--gpu-heavy")) O.gpu_iters = 400;
        else if (!strcmp(a, "--wm")) O.wm = 1;
        else if (!strcmp(a, "--glx-compat")) O.glx_compat = 1;
        else if (!strcmp(a, "--no-rt")) O.no_rt = 1;
        else if (!strcmp(a, "--skip-env")) O.skip_env = 1;
        else if (!strcmp(a, "--skip-egl")) O.skip_egl = 1;
        else if (!strcmp(a, "--skip-sdl")) O.skip_sdl = 1;
        else if (!strcmp(a, "--skip-vk")) O.skip_vk = 1;
        else {
            usage();
            return !strcmp(a, "--help") || !strcmp(a, "-h") ? 0 : 2;
        }
#undef ARG
    }
    if (O.frames < 30) O.frames = 30;
    if (O.vblanks < 30) O.vblanks = 30;

    char outdir[512];
    if (O.out) {
        snprintf(outdir, sizeof outdir, "%s", O.out);
    } else {
        time_t t = time(NULL);
        struct tm tm;
        localtime_r(&t, &tm);
        strftime(outdir, sizeof outdir, "ysp_x11_probe_%Y%m%d_%H%M%S", &tm);
    }
    mkdir(outdir, 0755);
    char path[1024];
#define OPEN(var, name, header) do { snprintf(path, sizeof path, "%s/%s", outdir, name); var = fopen(path, "w"); \
        if (!var) { fprintf(stderr, "cannot write %s\n", path); return 2; } const char* h_ = header; if (h_) fputs(h_, var); } while (0)
    OPEN(g_sum, "summary.txt", NULL);
    OPEN(g_swaps, "swaps.csv",
         "run,api,screen,method,k,frame,late_injected,gpu_iters,target_msc,msc_prev,ust_prev_ns,t_wake_ns,t_call_ns,t_ret_ns,"
         "t_done_ns,sbc,sync_kind,sync_ust,sync_msc,sync_sbc,present_serial,present_mode,present_ust,present_msc,"
         "present_arrival_ns,intel_type,intel_ust,intel_msc,intel_sbc,intel_arrival_ns\n");
    OPEN(g_vbl, "vblank.csv", "context,serial,target_msc,msc,ust,arrival_mono_ns,arrival_raw_ns,arrival_real_ns,arrival_boot_ns\n");
    OPEN(g_clk, "clock.csv", "context,source,i,ust,msc,sbc,mono_before_ns,mono_after_ns,raw_after_ns,real_after_ns,boot_after_ns\n");
    OPEN(g_ext, "extensions.txt", NULL);
#undef OPEN

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGALRM, on_alarm);
    alarm((unsigned)O.timeout_s);
    XSetErrorHandler(xerr_handler);

    struct utsname un;
    uname(&un);
    time_t t0 = time(NULL);
    char when[64];
    strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S %z", localtime(&t0));
    say("# ysp X11 display-timing probe\n\n");
    say("date %s\nkernel %s %s %s\ncommand", when, un.sysname, un.release, un.machine);
    for (int i = 0; i < argc; i++) say(" %s", argv[i]);
    say("\nbuilt with %s, Vulkan section %s\n",
#if defined(__clang__)
        "clang " __clang_version__,
#elif defined(__GNUC__)
        "gcc " __VERSION__,
#else
        "an unknown compiler",
#endif
        PROBE_VULKAN ? "compiled" : "not compiled");
    say("options: frames %d, vblanks %d, load %d threads, GPU iterations %d, %s, late every %d, sleep margin %.2f ms\n", O.frames,
        O.vblanks, O.load, O.gpu_iters, O.wm ? "fullscreen through the WM" : "override-redirect fullscreen",
        O.late_every > 0 ? O.late_every : 30, O.sleep_margin_ms);
    if (strstr(un.release, "microsoft") || strstr(un.release, "Microsoft") || access("/proc/sys/fs/binfmt_misc/WSLInterop", F_OK) == 0)
        say("\nWARNING: this is WSL (WSLg's XWayland or an Xvfb). No timing below says anything about a real display.\n");

    Display* d = XOpenDisplay(NULL);
    if (!d) {
        say("cannot open display \"%s\"\n", getenv("DISPLAY") ? getenv("DISPLAY") : "");
        return 2;
    }
    base_display_name(DisplayString(d), g_base_name, sizeof g_base_name);
    g_nscreens = ScreenCount(d);
    randr_scan(DisplayString(d), 0);
    if (!O.skip_env) env_section(d);
    if (!O.skip_vk && !g_stop) vk_section(d);
    if (!O.skip_sdl && !g_stop) sdl_section();

    pthread_t* lt = NULL;
    if (O.load > 0) {
        say("\n## 9. Load: %d busy threads from here on\n", O.load);
        lt = calloc((size_t)O.load, sizeof *lt);
        for (int i = 0; i < O.load; i++) pthread_create(&lt[i], NULL, load_fn, NULL);
    }
    int first = O.screen >= 0 ? O.screen : 0, last = O.screen >= 0 ? O.screen : g_nscreens - 1;
    if (last > 3) last = 3;
    if (!O.both_only)
        for (int s = first; s <= last && !g_stop; s++) {
            if (s >= g_nscreens) {
                say("\nscreen %d does not exist (%d screens)\n", s, g_nscreens);
                break;
            }
            glx_suite(s);
            if (!O.skip_egl && !g_stop) egl_suite(s, "libEGL.so.1", "libGLESv2.so.2", 0, "egl");
            if (O.angle_dir && !g_stop) angle_suite(s);
        }
    if (!g_stop && (O.both_only || (O.screen < 0 && !O.no_both && g_nscreens >= 2))) both_phases();
    if (lt) {
        g_load_stop = 1;
        for (int i = 0; i < O.load; i++) pthread_join(lt[i], NULL);
        free(lt);
    }
    say("\n%s. Files: %s/summary.txt, swaps.csv, vblank.csv, clock.csv, extensions.txt\n", g_stop ? "Stopped early" : "Done", outdir);
    XCloseDisplay(d);
    fclose(g_swaps);
    fclose(g_vbl);
    fclose(g_clk);
    fclose(g_ext);
    fclose(g_sum);
    return g_stop ? 1 : 0;
}
