/* Section egl (question 1a): SDL3's own EGL path on Cocoa against ANGLE.
 * SDL makes the window layer-backed and gives ANGLE the view's CALayer;
 * ANGLE then owns a CAMetalLayer. The probe finds that layer, reports what
 * ANGLE set on it, and, to see when ANGLE's frames reach the panel, swizzles
 * -[CAMetalLayer nextDrawable] in this process so that each drawable ANGLE
 * takes gets a presented handler. That swizzle is a measurement tool only. */
#import <Cocoa/Cocoa.h>
#import <objc/runtime.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "mtl.h"

typedef struct egl_frame {
    int64_t t_call, t_ret, t_next;      /* swap call, return, ANGLE's nextDrawable */
    int64_t presented, pres_arrive;
    uint64_t ust, msc, sbc;
    int sync_ok, has_drawable;
    volatile int complete;
} egl_frame;

static egl_frame* ef;
static int ef_cap;
static volatile int ef_cur = -1;        /* frame being drawn                    */
static IMP orig_next;
static __unsafe_unretained CAMetalLayer* angle_layer;

static id swz_next(id self, SEL cmd) {
    int64_t t = mach_ns_now();
    id<CAMetalDrawable> d = ((id (*)(id, SEL))orig_next)(self, cmd);
    int i = ef_cur;
    if (d && i >= 0 && i < ef_cap) {
        if (!angle_layer) angle_layer = self;
        egl_frame* f = &ef[i];
        f->t_next = t;
        f->has_drawable = 1;
        [d addPresentedHandler:^(id<MTLDrawable> dd) {
            f->pres_arrive = mach_ns_now();
            f->presented = dd.presentedTime > 0 ? secs_to_ns(dd.presentedTime) : 0;
            __atomic_store_n(&f->complete, 1, __ATOMIC_RELEASE);
        }];
    }
    return d;
}

static void find_metal_layers(CALayer* l, int depth) {
    if (!l) return;
    P("  %*slayer %s%s\n", depth * 2, "", class_getName([l class]),
      [l isKindOfClass:[CAMetalLayer class]] ? " (CAMetalLayer)" : "");
    if ([l isKindOfClass:[CAMetalLayer class]]) {
        CAMetalLayer* m = (CAMetalLayer*)l;
        P("  %*s  pixelFormat %lu, drawableSize %.0fx%.0f, maximumDrawableCount %lu, displaySyncEnabled %d, "
          "presentsWithTransaction %d, framebufferOnly %d, allowsNextDrawableTimeout %d, device \"%s\"\n",
          depth * 2, "", (unsigned long)m.pixelFormat, m.drawableSize.width, m.drawableSize.height,
          (unsigned long)m.maximumDrawableCount, m.displaySyncEnabled, m.presentsWithTransaction, m.framebufferOnly,
          m.allowsNextDrawableTimeout, m.device.name.UTF8String);
    }
    for (CALayer* s in l.sublayers) find_metal_layers(s, depth + 1);
}

static SDL_EGLAttrib plat_attrs[3];
/* SDL frees the array it gets, so hand it a copy from SDL_malloc. */
static SDL_EGLAttrib* SDLCALL plat_cb(void* ud) {
    (void)ud;
    SDL_EGLAttrib* a = (SDL_EGLAttrib*)SDL_malloc(sizeof plat_attrs);
    memcpy(a, plat_attrs, sizeof plat_attrs);
    return a;
}

typedef EGLBoolean (*sync_values_fn)(EGLDisplay, EGLSurface, EGLuint64KHR*, EGLuint64KHR*, EGLuint64KHR*);

int sec_egl(void) {
    if (!egl_load(g_opt.angle)) return 1;
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    int bits = g_opt.bits;
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, bits);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, bits);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, bits);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, bits == 10 ? 2 : bits);
    if (bits == 16) SDL_GL_SetAttribute(SDL_GL_FLOATBUFFERS, 1);
    if (strcmp(g_opt.backend, "default") != 0) {
        SDL_GL_SetAttribute(SDL_GL_EGL_PLATFORM, PROBE_EGL_PLATFORM_ANGLE_ANGLE);
        plat_attrs[0] = PROBE_EGL_PLATFORM_ANGLE_TYPE_ANGLE;
        plat_attrs[1] = strcmp(g_opt.backend, "gl") == 0 ? PROBE_EGL_PLATFORM_ANGLE_TYPE_OPENGL_ANGLE
                                                         : PROBE_EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE;
        plat_attrs[2] = EGL_NONE;
        SDL_EGL_SetAttributeCallbacks(plat_cb, NULL, NULL, NULL);
    }
    Method m = class_getInstanceMethod([CAMetalLayer class], @selector(nextDrawable));
    orig_next = method_setImplementation(m, (IMP)swz_next);

    SDL_Window* w = open_window(true, "ysp macOS probe: SDL EGL");
    if (!w) {
        P("RESULT sdl_egl_window FAILED (%d bits per channel): %s\n", bits, SDL_GetError());
        return 1;
    }
    SDL_GLContext ctx = SDL_GL_CreateContext(w);
    if (!ctx) {
        P("RESULT sdl_egl_context FAILED: %s\n", SDL_GetError());
        close_window(w);
        return 1;
    }
    P("RESULT sdl_egl_context OK\n");
    EGLDisplay dpy = (EGLDisplay)SDL_EGL_GetCurrentDisplay();
    EGLConfig cfg = (EGLConfig)SDL_EGL_GetCurrentConfig();
    EGLSurface surf = (EGLSurface)SDL_EGL_GetWindowSurface(w);
    gl_load((void* (*)(const char*))SDL_GL_GetProcAddress);
    P("  EGL_VENDOR %s, EGL_VERSION %s\n", E.QueryString(dpy, EGL_VENDOR), E.QueryString(dpy, EGL_VERSION));
    const char* ext = E.QueryString(dpy, EGL_EXTENSIONS);
    P("  display extensions: %s\n", ext);
    egl_print_config(dpy, cfg, "  SDL's ");
    gl_print_strings("  ");
    int r = 0, g = 0, b = 0, a = 0;
    SDL_GL_GetAttribute(SDL_GL_RED_SIZE, &r);
    SDL_GL_GetAttribute(SDL_GL_GREEN_SIZE, &g);
    SDL_GL_GetAttribute(SDL_GL_BLUE_SIZE, &b);
    SDL_GL_GetAttribute(SDL_GL_ALPHA_SIZE, &a);
    P("  SDL_GL_GetAttribute RGBA %d/%d/%d/%d\n", r, g, b, a);
    EGLint rb = 0, sb = 0, sw = 0, sh = 0;
    E.QuerySurface(dpy, surf, EGL_RENDER_BUFFER, &rb);
    E.QuerySurface(dpy, surf, EGL_SWAP_BEHAVIOR, &sb);
    E.QuerySurface(dpy, surf, EGL_WIDTH, &sw);
    E.QuerySurface(dpy, surf, EGL_HEIGHT, &sh);
    P("  window surface %dx%d, render buffer 0x%x, swap behavior 0x%x\n", sw, sh, rb, sb);
    bool interval_ok = SDL_GL_SetSwapInterval(g_opt.interval);
    P("  SDL_GL_SetSwapInterval(%d): %s\n", g_opt.interval, interval_ok ? "ok" : SDL_GetError());

    /* the timing extensions, each tried once before the run */
    sync_values_fn sync_values = (sync_values_fn)egl_proc("eglGetSyncValuesCHROMIUM");
    PFNEGLGETMSCRATEANGLEPROC msc_rate = (PFNEGLGETMSCRATEANGLEPROC)egl_proc("eglGetMscRateANGLE");
    PFNEGLGETNEXTFRAMEIDANDROIDPROC next_id = (PFNEGLGETNEXTFRAMEIDANDROIDPROC)egl_proc("eglGetNextFrameIdANDROID");
    PFNEGLGETFRAMETIMESTAMPSANDROIDPROC frame_ts = (PFNEGLGETFRAMETIMESTAMPSANDROIDPROC)egl_proc("eglGetFrameTimestampsANDROID");
    PFNEGLGETFRAMETIMESTAMPSUPPORTEDANDROIDPROC ts_sup =
        (PFNEGLGETFRAMETIMESTAMPSUPPORTEDANDROIDPROC)egl_proc("eglGetFrameTimestampSupportedANDROID");
    PFNEGLPRESENTATIONTIMEANDROIDPROC pres_time = (PFNEGLPRESENTATIONTIMEANDROIDPROC)egl_proc("eglPresentationTimeANDROID");
    P("  advertised: CHROMIUM_sync_control %d, ANGLE_sync_control_rate %d, ANDROID_get_frame_timestamps %d, "
      "ANDROID_presentation_time %d, ANGLE_timestamp_surface_attribute %d\n",
      has_ext(ext, "EGL_CHROMIUM_sync_control"), has_ext(ext, "EGL_ANGLE_sync_control_rate"),
      has_ext(ext, "EGL_ANDROID_get_frame_timestamps"), has_ext(ext, "EGL_ANDROID_presentation_time"),
      has_ext(ext, "EGL_ANGLE_timestamp_surface_attribute"));
    bool use_sync = sync_values && has_ext(ext, "EGL_CHROMIUM_sync_control");
    if (use_sync) {
        EGLuint64KHR u = 0, mm = 0, s = 0;
        EGLBoolean ok = sync_values(dpy, surf, &u, &mm, &s);
        P("  eglGetSyncValuesCHROMIUM: %s, ust %llu msc %llu sbc %llu (%s)\n", ok ? "ok" : "failed",
          (unsigned long long)u, (unsigned long long)mm, (unsigned long long)s, egl_err_name(E.GetError()));
    }
    if (msc_rate && has_ext(ext, "EGL_ANGLE_sync_control_rate")) {
        EGLint num = 0, den = 0;
        EGLBoolean ok = msc_rate(dpy, surf, &num, &den);
        P("  eglGetMscRateANGLE: %s, %d/%d (%s)\n", ok ? "ok" : "failed", num, den, egl_err_name(E.GetError()));
    }
    bool use_fts = false;
    if (frame_ts && has_ext(ext, "EGL_ANDROID_get_frame_timestamps")) {
        EGLBoolean ok = E.SurfaceAttrib(dpy, surf, EGL_TIMESTAMPS_ANDROID, EGL_TRUE);
        P("  eglSurfaceAttrib(EGL_TIMESTAMPS_ANDROID): %s (%s); supported:", ok ? "ok" : "failed", egl_err_name(E.GetError()));
        static const EGLint names[] = { EGL_REQUESTED_PRESENT_TIME_ANDROID, EGL_RENDERING_COMPLETE_TIME_ANDROID,
                                        EGL_COMPOSITION_LATCH_TIME_ANDROID, EGL_DISPLAY_PRESENT_TIME_ANDROID };
        for (int i = 0; i < 4; i++) P(" 0x%x=%d", names[i], ts_sup ? ts_sup(dpy, surf, names[i]) : -1);
        P("\n");
        use_fts = ok;
    }
    P("  ANGLE's layers under the content view:\n");
    NSWindow* nw = (__bridge NSWindow*)SDL_GetPointerProperty(SDL_GetWindowProperties(w), SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, NULL);
    find_metal_layers(nw.contentView.layer, 1);

    /* the swap run */
    int N = g_opt.frames;
    ef_cap = N;
    ef = calloc((size_t)N, sizeof(egl_frame));
    EGLuint64KHR* fid = calloc((size_t)N, sizeof(EGLuint64KHR));
    cv_link cv;
    cv_start(&cv, CGMainDisplayID(), 1 << 16);
    pump_for(1.0);
    unsigned prog = g_opt.heavy ? gl_program(1) : 0;
    int64_t t0 = mach_ns_now();
    for (int i = 0; i < N && !quit_requested(); i++) {
        egl_frame* f = &ef[i];
        ef_cur = i;
        if (use_fts && next_id) next_id(dpy, surf, &fid[i]);
        G.Viewport(0, 0, sw, sh);
        G.ClearColor(0.25f, 0.25f, 0.25f, 1);
        G.Clear(PGL_COLOR_BUFFER_BIT);
        if (prog) gl_draw_fullscreen(prog, g_opt.heavy, 0.25f);
        G.Enable(PGL_SCISSOR_TEST);
        G.Scissor(0, sh - 64, 64, 64);
        G.ClearColor((i & 1) ? 1.f : 0.f, (i & 1) ? 1.f : 0.f, (i & 1) ? 1.f : 0.f, 1);
        G.Clear(PGL_COLOR_BUFFER_BIT);
        G.Disable(PGL_SCISSOR_TEST);
        f->t_call = mach_ns_now();
        SDL_GL_SwapWindow(w);
        f->t_ret = mach_ns_now();
        if (use_sync) {
            f->sync_ok = sync_values(dpy, surf, &f->ust, &f->msc, &f->sbc);
            if (!f->sync_ok) E.GetError();
        }
        if ((i & 7) == 0) {
            SDL_Event e;
            while (SDL_PollEvent(&e)) {}
        }
    }
    int n = ef_cur + 1;
    ef_cur = -1;
    pump_for(0.3);
    int64_t t1 = mach_ns_now();
    cv_stop(&cv);
    grid gr = cv_grid(&cv, t0 - 100000000, t1);
    cv_report(&cv, tagged("egl_cv"));

    double *call = calloc((size_t)n, sizeof(double)), *intv = calloc((size_t)n, sizeof(double)),
           *phase = calloc((size_t)n, sizeof(double)), *lat = calloc((size_t)n, sizeof(double)),
           *res = calloc((size_t)n, sizeof(double)), *arr = calloc((size_t)n, sizeof(double)),
           *ustd = calloc((size_t)n, sizeof(double));
    int nlat = 0, nres = 0, nust = 0, sync_fail = 0, shown = 0, notshown = 0, nodraw = 0, depth[6] = { 0 }, steps[5] = { 0 };
    long long prev = INT64_MIN;
    for (int i = 0; i < n; i++) {
        egl_frame* f = &ef[i];
        call[i] = (double)(f->t_ret - f->t_call) / 1000.0;
        if (i) intv[i - 1] = (double)(f->t_ret - ef[i - 1].t_ret) / 1e6;
        if (gr.ok) { double x = grid_index(&gr, f->t_ret); phase[i] = x - floor(x); }
        if (use_sync && !f->sync_ok) sync_fail++;
        if (use_sync && f->sync_ok) ustd[nust++] = (double)f->ust; /* unit is checked below */
        if (!f->has_drawable) { nodraw++; continue; }
        if (!f->complete) continue;
        if (!f->presented) { notshown++; continue; }
        shown++;
        lat[nlat++] = (double)(f->presented - f->t_call) / 1e6;
        arr[nres] = (double)(f->pres_arrive - f->presented) / 1000.0;
        if (gr.ok) {
            res[nres] = fabs(grid_residual_us(&gr, f->presented));
            long long vs = grid_round(&gr, f->presented), vc = (long long)floor(grid_index(&gr, f->t_call));
            long long d = vs - vc;
            depth[d < 0 ? 0 : d > 5 ? 5 : d]++;
            if (prev != INT64_MIN) { long long s = vs - prev; steps[s < 0 ? 0 : s > 4 ? 4 : s]++; }
            prev = vs;
        }
        nres++;
    }
    P("Swap run: %d frames in %.2f s (%.2f per s), swap interval %d, kind %s\n", n,
      (double)(ef[n - 1].t_ret - ef[0].t_call) / 1e9, n / ((double)(ef[n - 1].t_ret - ef[0].t_call) / 1e9),
      g_opt.interval, g_opt.kind);
    P_stats("eglSwapBuffers call", call, n, "us");
    P_stats("interval between returns", intv, n - 1, "ms");
    P_stats("return phase in the refresh (0..1)", phase, n, "");
    P("  ANGLE drawables seen through the swizzle: %d of %d frames; shown %d, presentedTime 0: %d\n", n - nodraw, n,
      shown, notshown);
    if (gr.ok) {
        P("  depth (shown vblank minus the vblank before the swap call): <=0:%d 1:%d 2:%d 3:%d 4:%d 5+:%d\n", depth[0],
          depth[1], depth[2], depth[3], depth[4], depth[5]);
        P("  vblank steps between shown frames: 0:%d 1:%d 2:%d 3:%d 4+:%d\n", steps[0], steps[1], steps[2], steps[3], steps[4]);
        P_stats("|presentedTime - CV grid|", res, nres, "us");
    }
    P_stats("presentedTime minus swap call", lat, nlat, "ms");
    P_stats("presented handler minus presentedTime", arr, nres, "us");
    if (use_sync) {
        P("  eglGetSyncValuesCHROMIUM failed on %d of %d frames\n", sync_fail, n);
        if (nust > 1) {
            /* is the UST mach ns, mach ticks or us? compare its span with mach ns */
            double span_ust = ustd[nust - 1] - ustd[0];
            double span = (double)(ef[n - 1].t_ret - ef[0].t_ret);
            P("  UST span / mach ns span over the run: %.6f; last UST %llu vs mach ns at read %lld; msc %llu sbc %llu\n",
              span_ust / span, (unsigned long long)ef[n - 1].ust, (long long)ef[n - 1].t_ret,
              (unsigned long long)ef[n - 1].msc, (unsigned long long)ef[n - 1].sbc);
        }
    }
    if (use_fts && frame_ts) {
        int got = 0;
        for (int i = 0; i < n; i++) {
            EGLint names[] = { EGL_DISPLAY_PRESENT_TIME_ANDROID };
            EGLnsecsANDROID v = 0;
            if (frame_ts(dpy, surf, fid[i], 1, names, &v) && v > 0) got++;
        }
        P("  eglGetFrameTimestampsANDROID DISPLAY_PRESENT_TIME valid on %d of %d frames (%s)\n", got, n,
          egl_err_name(E.GetError()));
    }
    FILE* csv = open_csv(tagged("egl_frames"), "frame,t_call,t_ret,t_next,presented,pres_arrive,sync_ok,ust,msc,sbc,v_call,v_shown");
    for (int i = 0; csv && i < n; i++) {
        egl_frame* f = &ef[i];
        fprintf(csv, "%d,%lld,%lld,%lld,%lld,%lld,%d,%llu,%llu,%llu,%lld,%lld\n", i, (long long)f->t_call,
                (long long)f->t_ret, (long long)f->t_next, (long long)f->presented, (long long)f->pres_arrive,
                f->sync_ok, (unsigned long long)f->ust, (unsigned long long)f->msc, (unsigned long long)f->sbc,
                gr.ok ? (long long)floor(grid_index(&gr, f->t_call)) : 0LL,
                gr.ok && f->presented ? (long long)grid_round(&gr, f->presented) : 0LL);
    }
    if (csv) fclose(csv);

    /* EGL_ANDROID_presentation_time: does a time hold a frame? */
    if (pres_time && has_ext(ext, "EGL_ANDROID_presentation_time") && gr.ok) {
        cv_start(&cv, CGMainDisplayID(), 1 << 14);
        pump_for(0.5);
        int on = 0, other = 0, failed = 0;
        for (int i = 0; i < 20 && !quit_requested(); i++) {
            int64_t v;
            double per;
            cv_latest(&cv, &v, &per);
            int64_t target = v + (int64_t)(4 * per);
            ef_cur = i;
            memset(&ef[i], 0, sizeof ef[i]);
            G.ClearColor(0.3f, 0.3f, 0.3f, 1);
            G.Clear(PGL_COLOR_BUFFER_BIT);
            if (!pres_time(dpy, surf, target)) failed++;
            SDL_GL_SwapWindow(w);
            ef_cur = -1;
            int64_t lim = mach_ns_now() + 500000000;
            while (!ef[i].complete && mach_ns_now() < lim) pump_for(0.002);
            if (ef[i].presented && llabs(ef[i].presented - target) < (int64_t)(per / 2)) on++;
            else other++;
        }
        cv_stop(&cv);
        P("  eglPresentationTimeANDROID(4 vblanks ahead): on target %d, elsewhere %d, call failed %d\n", on, other, failed);
    }
    P("  ANGLE's CAMetalLayer after the run:\n");
    find_metal_layers(nw.contentView.layer, 1);
    free(call); free(intv); free(phase); free(lat); free(res); free(arr); free(ustd); free(fid);
    SDL_GL_DestroyContext(ctx);
    close_window(w);
    return 0;
}
