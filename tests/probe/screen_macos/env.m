/* Section env: the machine, the clocks, the displays and their modes, Metal,
 * and what ANGLE's EGL offers. Opens no window. */
#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#import <CoreVideo/CoreVideo.h>
#include <math.h>
#include <string.h>
#include <sys/sysctl.h>
#include <time.h>
#include <mach/mach_time.h>
#include "probe.h"

static int64_t r_camt(void) { return secs_to_ns(CACurrentMediaTime()); }
static int64_t r_uptime(void) { return secs_to_ns([NSProcessInfo processInfo].systemUptime); }
static int64_t r_uptime_raw(void) { return clock_ns(CLOCK_UPTIME_RAW); }
static int64_t r_mono(void) { return clock_ns(CLOCK_MONOTONIC); }
static int64_t r_mono_raw(void) { return clock_ns(CLOCK_MONOTONIC_RAW); }
static int64_t r_cgevent(void) {
    CGEventRef e = CGEventCreate(NULL);
    int64_t t = (int64_t)CGEventGetTimestamp(e);
    CFRelease(e);
    return t;
}
static int64_t r_cgevent_ticks(void) { return mach_ticks_to_ns((uint64_t)r_cgevent()); }

static void clock_step(const char* name, clock_fn f) {
    int64_t a = f(), b, step = INT64_MAX;
    int changes = 0;
    int64_t end = mach_ns_now() + 20000000;
    while (changes < 2000 && mach_ns_now() < end) {
        b = f();
        if (b != a) { if (b - a < step) step = b - a; a = b; changes++; }
    }
    P("  %-34s smallest step %lld ns\n", name, step == INT64_MAX ? -1LL : (long long)step);
}

/* Offset of another clock from mach ns, its bracket, and the change over
 * 1 s: the agreement the backend would rely on. */
static void clock_agree(const char* name, clock_fn f) {
    corr a = correlate(f, 64);
    struct timespec ts = { 1, 0 };
    nanosleep(&ts, NULL);
    corr b = correlate(f, 64);
    double drift_ppm = 1e6 * ((double)((b.other - a.other) - (b.mach - a.mach)) / (double)(b.mach - a.mach));
    P("  %-34s offset (other - mach) %+20.3f us, bracket %lld ns, rate %+9.3f ppm over 1 s\n", name,
      (double)(a.other - a.mach) / 1000.0, (long long)a.width, drift_ppm);
}

static void list_cg_modes(CGDirectDisplayID d) {
    CFStringRef k[] = { kCGDisplayShowDuplicateLowResolutionModes };
    CFTypeRef v[] = { kCFBooleanTrue };
    CFDictionaryRef opt = CFDictionaryCreate(NULL, (const void**)k, (const void**)v, 1,
                                             &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFArrayRef ms = CGDisplayCopyAllDisplayModes(d, opt);
    CGDisplayModeRef cur = CGDisplayCopyDisplayMode(d);
    P("  current mode: %zux%zu points, %zux%zu pixels, %.3f Hz, io id %d\n", CGDisplayModeGetWidth(cur),
      CGDisplayModeGetHeight(cur), CGDisplayModeGetPixelWidth(cur), CGDisplayModeGetPixelHeight(cur),
      CGDisplayModeGetRefreshRate(cur), CGDisplayModeGetIODisplayModeID(cur));
    /* Refresh rates offered at the current mode's size: the ones a fullscreen
     * run can switch to without scaling. */
    P("  modes at this size (CGDisplayCopyAllDisplayModes):");
    for (CFIndex i = 0; i < CFArrayGetCount(ms); i++) {
        CGDisplayModeRef m = (CGDisplayModeRef)CFArrayGetValueAtIndex(ms, i);
        if (CGDisplayModeGetPixelWidth(m) == CGDisplayModeGetPixelWidth(cur) &&
            CGDisplayModeGetPixelHeight(m) == CGDisplayModeGetPixelHeight(cur) &&
            CGDisplayModeGetWidth(m) == CGDisplayModeGetWidth(cur))
            P(" %.3f%s", CGDisplayModeGetRefreshRate(m), CGDisplayModeIsUsableForDesktopGUI(m) ? "" : "(not GUI)");
    }
    P("\n  all modes: %ld (full list in env_modes.csv)\n", (long)CFArrayGetCount(ms));
    FILE* f = open_csv("env_modes", "display,points_w,points_h,pixels_w,pixels_h,hz,io_flags,usable_gui,io_id");
    for (CFIndex i = 0; f && i < CFArrayGetCount(ms); i++) {
        CGDisplayModeRef m = (CGDisplayModeRef)CFArrayGetValueAtIndex(ms, i);
        fprintf(f, "%u,%zu,%zu,%zu,%zu,%.6f,0x%x,%d,%d\n", d, CGDisplayModeGetWidth(m), CGDisplayModeGetHeight(m),
                CGDisplayModeGetPixelWidth(m), CGDisplayModeGetPixelHeight(m), CGDisplayModeGetRefreshRate(m),
                CGDisplayModeGetIOFlags(m), CGDisplayModeIsUsableForDesktopGUI(m), CGDisplayModeGetIODisplayModeID(m));
    }
    if (f) fclose(f);
    CGDisplayModeRelease(cur);
    CFRelease(ms);
    CFRelease(opt);
}

static void env_displays(void) {
    P("Displays (CoreGraphics, AppKit, CoreVideo)\n");
    CGDirectDisplayID ids[16];
    uint32_t n = 0;
    CGError cge = CGGetActiveDisplayList(16, ids, &n);
    P(" CGGetActiveDisplayList: error %d, %u displays; CGMainDisplayID %u\n", (int)cge, n, CGMainDisplayID());
    if (n == 0) { ids[0] = CGMainDisplayID(); n = 1; }
    for (uint32_t i = 0; i < n; i++) {
        CGDirectDisplayID d = ids[i];
        CGSize mm = CGDisplayScreenSize(d);
        P(" display %u: main %d, builtin %d, vendor 0x%x model 0x%x, %.0f x %.0f mm, CGDisplayBounds %.0fx%.0f\n",
          d, CGDisplayIsMain(d), CGDisplayIsBuiltin(d), CGDisplayVendorNumber(d), CGDisplayModelNumber(d),
          mm.width, mm.height, CGDisplayBounds(d).size.width, CGDisplayBounds(d).size.height);
        list_cg_modes(d);
        CVDisplayLinkRef l = NULL;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        if (CVDisplayLinkCreateWithCGDisplay(d, &l) == kCVReturnSuccess) {
            CVTime t = CVDisplayLinkGetNominalOutputVideoRefreshPeriod(l);
            P("  CVDisplayLink nominal period %lld/%d s = %.6f ms\n", (long long)t.timeValue, t.timeScale,
              t.timeScale ? 1e3 * (double)t.timeValue / t.timeScale : 0.0);
            CVDisplayLinkRelease(l);
        }
#pragma clang diagnostic pop
    }
    for (NSScreen* s in [NSScreen screens]) {
        NSNumber* num = s.deviceDescription[@"NSScreenNumber"];
        P(" NSScreen %u \"%s\": frame %.0fx%.0f at %.0f,%.0f, scale %.1f, depth %ld bits/sample, "
          "colorSpace \"%s\"\n",
          num.unsignedIntValue, s.localizedName.UTF8String, s.frame.size.width, s.frame.size.height,
          s.frame.origin.x, s.frame.origin.y, s.backingScaleFactor, (long)NSBitsPerSampleFromDepth(s.depth),
          s.colorSpace.localizedName.UTF8String);
        P("  maximumFramesPerSecond %ld, minimumRefreshInterval %.6f ms, maximumRefreshInterval %.6f ms, "
          "displayUpdateGranularity %.6f ms\n",
          (long)s.maximumFramesPerSecond, 1e3 * s.minimumRefreshInterval, 1e3 * s.maximumRefreshInterval,
          1e3 * s.displayUpdateGranularity);
        P("  EDR: now %.3f, potential %.3f, reference %.3f\n", s.maximumExtendedDynamicRangeColorComponentValue,
          s.maximumPotentialExtendedDynamicRangeColorComponentValue,
          s.maximumReferenceExtendedDynamicRangeColorComponentValue);
    }
    P("Displays (SDL3)\n");
    int nd = 0;
    SDL_DisplayID* ds = SDL_GetDisplays(&nd);
    for (int i = 0; i < nd; i++) {
        const SDL_DisplayMode* dm = SDL_GetDesktopDisplayMode(ds[i]);
        P(" SDL display %u \"%s\": desktop %dx%d @ %.3f Hz (%d/%d) density %.2f, content scale %.2f\n", ds[i],
          SDL_GetDisplayName(ds[i]), dm->w, dm->h, dm->refresh_rate, dm->refresh_rate_numerator,
          dm->refresh_rate_denominator, dm->pixel_density, SDL_GetDisplayContentScale(ds[i]));
        int nm = 0;
        SDL_DisplayMode** ms = SDL_GetFullscreenDisplayModes(ds[i], &nm);
        P("  fullscreen modes: %d:", nm);
        for (int k = 0; k < nm; k++)
            P(" %dx%d@%.2f%s", ms[k]->w, ms[k]->h, ms[k]->refresh_rate, ms[k]->pixel_density > 1 ? "x2" : "");
        P("\n");
        SDL_free(ms);
    }
    SDL_free(ds);
}

static void env_metal(void) {
    P("Metal\n");
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    for (id<MTLDevice> d in MTLCopyAllDevices())
        P(" device \"%s\" registryID 0x%llx, unifiedMemory %d, default %d\n",
          d.name.UTF8String, d.registryID, d.hasUnifiedMemory, d == dev);
}

static void env_angle_backend(const char* backend) {
    P(" ANGLE backend request \"%s\"\n", backend);
    EGLDisplay d = angle_display(backend);
    if (d == EGL_NO_DISPLAY) return;
    P("  EGL_VENDOR  %s\n  EGL_VERSION %s\n", E.QueryString(d, EGL_VENDOR), E.QueryString(d, EGL_VERSION));
    const char* ext = E.QueryString(d, EGL_EXTENSIONS);
    P("  display extensions: %s\n", ext);
    static const char* want[] = {
        "EGL_ANGLE_sync_control_rate", "EGL_CHROMIUM_sync_control", "EGL_ANDROID_get_frame_timestamps",
        "EGL_ANDROID_presentation_time", "EGL_ANGLE_timestamp_surface_attribute", "EGL_EXT_buffer_age",
        "EGL_ANGLE_metal_texture_client_buffer", "EGL_ANGLE_iosurface_client_buffer",
        "EGL_ANGLE_metal_shared_event_sync", "EGL_ANGLE_metal_commands_scheduled_sync",
        "EGL_ANGLE_wait_until_work_scheduled", "EGL_ANGLE_metal_create_context_ownership_identity",
        "EGL_ANGLE_device_metal", "EGL_EXT_device_query", "EGL_KHR_fence_sync", "EGL_KHR_wait_sync",
        "EGL_ANGLE_global_fence_sync", "EGL_EXT_pixel_format_float", "EGL_KHR_gl_colorspace",
        "EGL_EXT_gl_colorspace_display_p3", "EGL_EXT_gl_colorspace_scrgb_linear",
        "EGL_ANGLE_create_surface_swap_interval", "EGL_KHR_surfaceless_context", NULL };
    P("  of interest:");
    for (int i = 0; want[i]; i++) P(" %s=%d", want[i] + 4, has_ext(ext, want[i]));
    P("\n");
    EGLint n = 0;
    E.GetConfigs(d, NULL, 0, &n);
    EGLConfig cs[256];
    if (n > 256) n = 256;
    E.GetConfigs(d, cs, n, &n);
    P("  %d configs\n", n);
    for (int i = 0; i < n; i++) egl_print_config(d, cs[i], "   ");
    /* the Metal device ANGLE runs on */
    if (E.QueryDisplayAttribEXT && E.QueryDeviceAttribEXT) {
        EGLAttrib dev = 0, mtl = 0;
        if (E.QueryDisplayAttribEXT(d, EGL_DEVICE_EXT, &dev) &&
            E.QueryDeviceAttribEXT((EGLDeviceEXT)dev, PROBE_EGL_METAL_DEVICE_ANGLE, &mtl) && mtl) {
            id<MTLDevice> m = (__bridge id<MTLDevice>)(void*)mtl;
            P("  EGL_METAL_DEVICE_ANGLE: \"%s\" registryID 0x%llx, same object as MTLCreateSystemDefaultDevice: %d\n",
              m.name.UTF8String, m.registryID, m == MTLCreateSystemDefaultDevice());
            const char* dext = E.QueryDeviceStringEXT ? E.QueryDeviceStringEXT((EGLDeviceEXT)dev, EGL_EXTENSIONS) : NULL;
            P("  device extensions: %s\n", dext ? dext : "(none)");
        } else {
            P("  EGL_METAL_DEVICE_ANGLE query failed: %s\n", egl_err_name(E.GetError()));
        }
    }
    /* GL strings need a context; a 16 x 16 pbuffer is enough */
    EGLint ca[] = { EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                    EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, 0x40, EGL_NONE };
    EGLConfig cfg;
    EGLint nc = 0;
    if (E.ChooseConfig(d, ca, &cfg, 1, &nc) && nc == 1) {
        EGLint pa[] = { EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE };
        EGLint xa[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 0, EGL_NONE };
        EGLSurface s = E.CreatePbufferSurface(d, cfg, pa);
        EGLContext c = E.CreateContext(d, cfg, EGL_NO_CONTEXT, xa);
        if (s != EGL_NO_SURFACE && c != EGL_NO_CONTEXT && E.MakeCurrent(d, s, s, c)) {
            gl_load(egl_proc);
            gl_print_strings("  ");
            const char* gext = (const char*)G.GetString(PGL_EXTENSIONS);
            P("  GL extensions of interest: EXT_color_buffer_float=%d EXT_color_buffer_half_float=%d "
              "EXT_disjoint_timer_query=%d ANGLE_metal_texture? n/a\n",
              has_ext(gext, "GL_EXT_color_buffer_float"), has_ext(gext, "GL_EXT_color_buffer_half_float"),
              has_ext(gext, "GL_EXT_disjoint_timer_query"));
            E.MakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        } else {
            P("  context: %s\n", egl_err_name(E.GetError()));
        }
        if (c != EGL_NO_CONTEXT) E.DestroyContext(d, c);
        if (s != EGL_NO_SURFACE) E.DestroySurface(d, s);
    }
    E.Terminate(d);
}

/* Can ANGLE take the probe's own MTLDevice? EGL_ANGLE_device_creation names
 * a device type; this tries the Metal one. */
static void env_angle_device_creation(void) {
    typedef void* (*create_fn)(EGLint, void*, const EGLAttrib*);
    typedef EGLBoolean (*release_fn)(void*);
    create_fn create = (create_fn)egl_proc("eglCreateDeviceANGLE");
    release_fn release = (release_fn)egl_proc("eglReleaseDeviceANGLE");
    const char* cext = E.QueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
    P(" EGL_ANGLE_device_creation in client extensions: %d, eglCreateDeviceANGLE %s\n",
      has_ext(cext, "EGL_ANGLE_device_creation"), create ? "found" : "absent");
    if (!create) return;
    id<MTLDevice> mine = MTLCreateSystemDefaultDevice();
    void* dev = create(PROBE_EGL_METAL_DEVICE_ANGLE, (__bridge void*)mine, NULL);
    EGLint err = E.GetError();
    P("  eglCreateDeviceANGLE(EGL_METAL_DEVICE_ANGLE, the default MTLDevice): %s (%s)\n",
      dev ? "a device" : "EGL_NO_DEVICE", egl_err_name(err));
    if (dev) {
        EGLDisplay d = E.GetPlatformDisplayEXT(EGL_PLATFORM_DEVICE_EXT, dev, NULL);
        EGLint maj = 0, min = 0;
        bool ok = d != EGL_NO_DISPLAY && E.Initialize(d, &maj, &min);
        P("  eglGetPlatformDisplayEXT(EGL_PLATFORM_DEVICE_EXT) and eglInitialize: %s (%s)\n", ok ? "ok" : "failed",
          egl_err_name(E.GetError()));
        if (ok) E.Terminate(d);
        if (release) release(dev);
    }
    /* The other route: pick the device by registry id on the ANGLE platform. */
    uint64_t rid = mine.registryID;
    EGLint attrs[] = { PROBE_EGL_PLATFORM_ANGLE_TYPE_ANGLE, PROBE_EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE,
                       PROBE_EGL_PLATFORM_ANGLE_DEVICE_ID_HIGH, (EGLint)(rid >> 32),
                       PROBE_EGL_PLATFORM_ANGLE_DEVICE_ID_LOW, (EGLint)(rid & 0xffffffffu), EGL_NONE };
    EGLDisplay d = E.GetPlatformDisplayEXT(PROBE_EGL_PLATFORM_ANGLE_ANGLE, NULL, attrs);
    EGLint maj = 0, min = 0;
    bool ok = d != EGL_NO_DISPLAY && E.Initialize(d, &maj, &min);
    P("  platform display with DEVICE_ID_HIGH/LOW = registryID: %s (%s)\n", ok ? "ok" : "failed",
      egl_err_name(E.GetError()));
    if (ok) E.Terminate(d);
}

int sec_env(void) {
    char model[128] = "", cpu[128] = "";
    size_t len = sizeof model;
    sysctlbyname("hw.model", model, &len, NULL, 0);
    len = sizeof cpu;
    sysctlbyname("machdep.cpu.brand_string", cpu, &len, NULL, 0);
    P("Machine: %s, %s, %s\n", model, cpu, [NSProcessInfo processInfo].operatingSystemVersionString.UTF8String);
    P("SDL %d.%d.%d (%s)\n", SDL_VERSIONNUM_MAJOR(SDL_GetVersion()), SDL_VERSIONNUM_MINOR(SDL_GetVersion()),
      SDL_VERSIONNUM_MICRO(SDL_GetVersion()), SDL_GetRevision());

    mach_timebase_info_data_t t;
    mach_timebase_info(&t);
    P("Clocks. Reference: mach_absolute_time() * %u / %u (mach ns), the base of ysp/rt.h v0.10.0 on macOS\n",
      t.numer, t.denom);
    clock_step("mach_absolute_time (ns)", mach_ns_now);
    clock_step("CACurrentMediaTime (ns)", r_camt);
    clock_step("NSProcessInfo.systemUptime (ns)", r_uptime);
    clock_step("CLOCK_UPTIME_RAW", r_uptime_raw);
    clock_step("CLOCK_MONOTONIC (rt.h before 0.10)", r_mono);
    clock_step("CLOCK_MONOTONIC_RAW", r_mono_raw);
    clock_step("SDL_GetTicksNS", sdl_ticks_now);
    clock_step("CGEventGetTimestamp (raw)", r_cgevent);
    clock_agree("CACurrentMediaTime", r_camt);
    clock_agree("NSProcessInfo.systemUptime", r_uptime);
    clock_agree("CLOCK_UPTIME_RAW", r_uptime_raw);
    clock_agree("mach_continuous_time", mach_cont_ns_now);
    clock_agree("CLOCK_MONOTONIC", r_mono);
    clock_agree("CLOCK_MONOTONIC_RAW", r_mono_raw);
    clock_agree("SDL_GetTicksNS", sdl_ticks_now);
    clock_agree("CGEventGetTimestamp as ns", r_cgevent);
    clock_agree("CGEventGetTimestamp as mach ticks", r_cgevent_ticks);
    P("  (an offset near 0 and a rate near 0 ppm mean the same clock; SDL's ticks start at SDL_Init)\n");

    print_session();
    env_displays();
    env_metal();
    P("ANGLE from %s\n", g_opt.angle);
    if (!egl_load(g_opt.angle)) return 1;
    P(" client extensions: %s\n", E.QueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS));
    env_angle_backend("default");
    env_angle_backend("metal");
    env_angle_backend("gl");
    env_angle_device_creation();
    return 0;
}
