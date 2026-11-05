/* Section share (question 1b): a CAMetalLayer the probe owns, on ANGLE's own
 * MTLDevice, with ANGLE drawing each frame and the probe presenting it.
 * Targets:
 *   drawable_ios    ANGLE draws into the drawable itself: a pbuffer
 *                   (EGL_ANGLE_iosurface_client_buffer) on the IOSurface
 *                   behind the drawable's texture, cached per IOSurface;
 *   drawable_image  the same through an EGLImage of the drawable's
 *                   MTLTexture (EGL_ANGLE_metal_texture_client_buffer) bound
 *                   to a GL framebuffer, cached per texture;
 *   iosurface       ANGLE draws into its own IOSurface; the probe blits it
 *                   into the drawable.
 * Syncs between ANGLE's command queue and the probe's: none, glFinish, an
 * MTLSharedEvent that ANGLE signals (EGL_ANGLE_metal_shared_event_sync) and
 * the probe's command buffer waits on, or eglWaitUntilWorkScheduledANGLE.
 * Each frame has its own gray level; the probe's command buffer reads the
 * drawable's center pixel before it presents, so a frame read before ANGLE
 * finished shows as a mismatch. */
#import <Cocoa/Cocoa.h>
#import <IOSurface/IOSurface.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "mtl.h"

#define PEGL_TEXTURE_FORMAT 0x3080
#define PEGL_TEXTURE_TARGET 0x3081
#define PEGL_TEXTURE_RGBA   0x305E
#define PGL_BGRA_EXT        0x80E1
#define PGL_TEXTURE_2D      0x0DE1
#define PGL_FRAMEBUFFER     0x8D40
#define PGL_COLOR_ATTACHMENT0 0x8CE0
#define PGL_FRAMEBUFFER_COMPLETE 0x8CD5

typedef EGLSync (*create_sync_fn)(EGLDisplay, EGLenum, const EGLAttrib*);
typedef EGLBoolean (*destroy_sync_fn)(EGLDisplay, EGLSync);
typedef EGLBoolean (*wait_scheduled_fn)(EGLDisplay);
typedef EGLImage (*create_image_fn)(EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, const EGLAttrib*);
typedef EGLBoolean (*destroy_image_fn)(EGLDisplay, EGLImage);

static struct {
    void (*GenTextures)(int, unsigned*);
    void (*DeleteTextures)(int, const unsigned*);
    void (*BindTexture)(unsigned, unsigned);
    void (*EGLImageTargetTexture2DOES)(unsigned, void*);
    void (*GenFramebuffers)(int, unsigned*);
    void (*DeleteFramebuffers)(int, const unsigned*);
    void (*BindFramebuffer)(unsigned, unsigned);
    void (*FramebufferTexture2D)(unsigned, unsigned, unsigned, unsigned, int);
    unsigned (*CheckFramebufferStatus)(unsigned);
} X;

enum { T_DRAWABLE_IOS, T_DRAWABLE_IMAGE, T_IOSURFACE, T_COUNT };
enum { S_NONE, S_FINISH, S_EVENT, S_SCHEDULED };
static const char* tname[] = { "drawable_ios", "drawable_image", "iosurface" };
static const char* sname[] = { "none", "finish", "event", "scheduled" };

static uint8_t level_byte(int i) { return (uint8_t)(40 + (i * 7) % 170); }

typedef struct target_cache {
    void*      key[8];
    EGLSurface surf[8];
    EGLImage   img[8];
    unsigned   tex[8], fbo[8];
    int        n, made, failed;
    double     create_us;
} target_cache;

static EGLDisplay dpy;
static EGLConfig cfg;
static EGLint bind_target;

static EGLSurface ios_pbuffer(IOSurfaceRef ios, int w, int h) {
    EGLint pa[] = { EGL_WIDTH, w, EGL_HEIGHT, h, PROBE_EGL_IOSURFACE_PLANE_ANGLE, 0,
                    PEGL_TEXTURE_TARGET, bind_target, PROBE_EGL_TEXTURE_INTERNAL_FORMAT_ANGLE, PGL_BGRA_EXT,
                    PEGL_TEXTURE_FORMAT, PEGL_TEXTURE_RGBA, PROBE_EGL_TEXTURE_TYPE_ANGLE, PGL_UNSIGNED_BYTE, EGL_NONE };
    return E.CreatePbufferFromClientBuffer(dpy, PROBE_EGL_IOSURFACE_ANGLE, ios, cfg, pa);
}

int sec_share(void) {
    if (!egl_load(g_opt.angle)) return 1;
    dpy = angle_display("metal");
    if (dpy == EGL_NO_DISPLAY) return 1;
    const char* ext = E.QueryString(dpy, EGL_EXTENSIONS);
    EGLAttrib edev = 0, emtl = 0;
    if (!E.QueryDisplayAttribEXT || !E.QueryDisplayAttribEXT(dpy, EGL_DEVICE_EXT, &edev) ||
        !E.QueryDeviceAttribEXT((EGLDeviceEXT)edev, PROBE_EGL_METAL_DEVICE_ANGLE, &emtl) || !emtl) {
        P("RESULT angle_metal_device FAILED: %s\n", egl_err_name(E.GetError()));
        return 1;
    }
    id<MTLDevice> dev = (__bridge id<MTLDevice>)(void*)emtl;
    P("RESULT angle_metal_device OK \"%s\" (same object as MTLCreateSystemDefaultDevice: %d)\n", dev.name.UTF8String,
      dev == MTLCreateSystemDefaultDevice());
    P("  metal_texture_client_buffer %d, iosurface_client_buffer %d, metal_shared_event_sync %d, "
      "wait_until_work_scheduled %d, KHR_image_base %d, surfaceless %d\n",
      has_ext(ext, "EGL_ANGLE_metal_texture_client_buffer"), has_ext(ext, "EGL_ANGLE_iosurface_client_buffer"),
      has_ext(ext, "EGL_ANGLE_metal_shared_event_sync"), has_ext(ext, "EGL_ANGLE_wait_until_work_scheduled"),
      has_ext(ext, "EGL_KHR_image_base"), has_ext(ext, "EGL_KHR_surfaceless_context"));

    EGLint ca[] = { EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                    EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, 0x40, EGL_NONE };
    EGLint nc = 0;
    if (!E.ChooseConfig(dpy, ca, &cfg, 1, &nc) || nc < 1) { P("  no RGBA8 pbuffer config\n"); return 1; }
    egl_print_config(dpy, cfg, "  using ");
    bind_target = 0x305F;
    E.GetConfigAttrib(dpy, cfg, PROBE_EGL_BIND_TO_TEXTURE_TARGET_ANGLE, &bind_target);
    EGLint xa[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 0, EGL_NONE };
    EGLContext ctx = E.CreateContext(dpy, cfg, EGL_NO_CONTEXT, xa);
    create_sync_fn create_sync = (create_sync_fn)egl_proc("eglCreateSync");
    destroy_sync_fn destroy_sync = (destroy_sync_fn)egl_proc("eglDestroySync");
    wait_scheduled_fn wait_sched = (wait_scheduled_fn)egl_proc("eglWaitUntilWorkScheduledANGLE");
    create_image_fn create_image = (create_image_fn)egl_proc("eglCreateImage");
    destroy_image_fn destroy_image = (destroy_image_fn)egl_proc("eglDestroyImage");

    Presenter* p = [[Presenter alloc] initWithDevice:dev format:MTLPixelFormatBGRA8Unorm title:"ysp macOS probe: share"];
    if (!p) return 1;
    pump_for(0.5);
    const int W = (int)p.layer.drawableSize.width, H = (int)p.layer.drawableSize.height;
    const int N = g_opt.frames;
    FILE* csv = open_csv(tagged("share"), frames_header());
    id<MTLSharedEvent> ev = [dev newSharedEvent];
    __block uint64_t ev_val = 0;

    run_worker(^{
        E.MakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx);
        gl_load(egl_proc);
#define L(f, n) X.f = (__typeof__(X.f))egl_proc(n)
        L(GenTextures, "glGenTextures"); L(DeleteTextures, "glDeleteTextures"); L(BindTexture, "glBindTexture");
        L(EGLImageTargetTexture2DOES, "glEGLImageTargetTexture2DOES"); L(GenFramebuffers, "glGenFramebuffers");
        L(DeleteFramebuffers, "glDeleteFramebuffers"); L(BindFramebuffer, "glBindFramebuffer");
        L(FramebufferTexture2D, "glFramebufferTexture2D"); L(CheckFramebufferStatus, "glCheckFramebufferStatus");
#undef L
        gl_print_strings("  ");
        P("  GL_OES_EGL_image %d\n", has_ext((const char*)G.GetString(PGL_EXTENSIONS), "GL_OES_EGL_image"));
        unsigned prog = g_opt.heavy ? gl_program(1) : 0;
        for (int tg = 0; tg < T_COUNT; tg++) {
            IOSurfaceRef ios = NULL;
            id<MTLTexture> own = nil;
            EGLSurface fixed = EGL_NO_SURFACE;
            if (tg == T_IOSURFACE) {
                NSDictionary* props = @{ (id)kIOSurfaceWidth: @(W), (id)kIOSurfaceHeight: @(H),
                                         (id)kIOSurfaceBytesPerElement: @4, (id)kIOSurfacePixelFormat: @((uint32_t)'BGRA') };
                ios = IOSurfaceCreate((__bridge CFDictionaryRef)props);
                MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                              width:(NSUInteger)W height:(NSUInteger)H mipmapped:NO];
                d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
                own = [dev newTextureWithDescriptor:d iosurface:ios plane:0];
                fixed = ios_pbuffer(ios, W, H);
                P(" target iosurface: pbuffer on the probe's IOSurface (bind target 0x%x): %s (%s)\n", bind_target,
                  fixed != EGL_NO_SURFACE ? "ok" : "failed", egl_err_name(E.GetError()));
                if (fixed == EGL_NO_SURFACE) { CFRelease(ios); continue; }
            }
            if (tg == T_DRAWABLE_IMAGE && !(create_image && X.EGLImageTargetTexture2DOES)) {
                P(" target drawable_image: eglCreateImage or glEGLImageTargetTexture2DOES missing\n");
                continue;
            }
            for (int sy = 0; sy < 4 && !quit_requested(); sy++) {
                if (sy == S_EVENT && !(create_sync && has_ext(ext, "EGL_ANGLE_metal_shared_event_sync"))) continue;
                if (sy == S_SCHEDULED && !wait_sched) continue;
                char name[64];
                snprintf(name, sizeof name, "%s_%s", tname[tg], sname[sy]);
                frame_rec* r = calloc((size_t)N, sizeof(frame_rec));
                double *mc = calloc((size_t)N, sizeof(double)), *dr = calloc((size_t)N, sizeof(double));
                id<MTLBuffer> rb = [dev newBufferWithLength:(NSUInteger)N * 4 options:MTLResourceStorageModeShared];
                target_cache tc;
                memset(&tc, 0, sizeof tc);
                int sync_fail = 0, n = 0;
                int64_t t0 = mach_ns_now();
                for (int i = 0; i < N && !quit_requested(); i++) {
                    frame_rec* f = &r[i];
                    f->frame = i;
                    f->t_req = mach_ns_now();
                    id<CAMetalDrawable> d = [p.layer nextDrawable];
                    f->t_got = mach_ns_now();
                    if (!d) break;
                    EGLSurface s = fixed;
                    unsigned fbo = 0;
                    if (tg != T_IOSURFACE) {
                        void* key = tg == T_DRAWABLE_IOS ? (void*)d.texture.iosurface : (__bridge void*)d.texture;
                        int k = 0;
                        while (k < tc.n && tc.key[k] != key) k++;
                        if (k == tc.n) {
                            if (tc.n == 8) { P("  %s: more than 8 distinct drawable textures\n", name); break; }
                            int64_t a = mach_ns_now();
                            if (tg == T_DRAWABLE_IOS) {
                                tc.surf[k] = key ? ios_pbuffer((IOSurfaceRef)key, W, H) : EGL_NO_SURFACE;
                                if (tc.surf[k] == EGL_NO_SURFACE) {
                                    P("  %s: drawable IOSurface %p, pbuffer failed: %s\n", name, key, egl_err_name(E.GetError()));
                                    break;
                                }
                            } else {
                                EGLAttrib ia[] = { EGL_NONE };
                                tc.img[k] = create_image(dpy, EGL_NO_CONTEXT, PROBE_EGL_METAL_TEXTURE_ANGLE, (EGLClientBuffer)key, ia);
                                if (tc.img[k] == EGL_NO_IMAGE) {
                                    P("  %s: eglCreateImage(EGL_METAL_TEXTURE_ANGLE) failed: %s\n", name, egl_err_name(E.GetError()));
                                    break;
                                }
                                E.MakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx);
                                X.GenTextures(1, &tc.tex[k]);
                                X.BindTexture(PGL_TEXTURE_2D, tc.tex[k]);
                                X.EGLImageTargetTexture2DOES(PGL_TEXTURE_2D, tc.img[k]);
                                X.GenFramebuffers(1, &tc.fbo[k]);
                                X.BindFramebuffer(PGL_FRAMEBUFFER, tc.fbo[k]);
                                X.FramebufferTexture2D(PGL_FRAMEBUFFER, PGL_COLOR_ATTACHMENT0, PGL_TEXTURE_2D, tc.tex[k], 0);
                                unsigned st = X.CheckFramebufferStatus(PGL_FRAMEBUFFER);
                                if (st != PGL_FRAMEBUFFER_COMPLETE) { P("  %s: framebuffer status 0x%x\n", name, st); break; }
                            }
                            tc.create_us += (double)(mach_ns_now() - a) / 1000.0;
                            tc.key[k] = key;
                            tc.n++;
                            tc.made++;
                        }
                        s = tc.surf[k];
                        fbo = tc.fbo[k];
                    }
                    int64_t a = mach_ns_now();
                    E.MakeCurrent(dpy, s, s, ctx);
                    if (tg == T_DRAWABLE_IMAGE) X.BindFramebuffer(PGL_FRAMEBUFFER, fbo);
                    mc[i] = (double)(mach_ns_now() - a) / 1000.0;
                    a = mach_ns_now();
                    uint8_t lv = level_byte(i);
                    float Lv = lv / 255.0f;
                    G.Viewport(0, 0, W, H);
                    G.ClearColor(Lv, Lv, Lv, 1);
                    G.Clear(PGL_COLOR_BUFFER_BIT);
                    if (prog) gl_draw_fullscreen(prog, g_opt.heavy, Lv);
                    uint64_t wait_val = 0;
                    if (sy == S_FINISH) G.Finish();
                    else if (sy == S_EVENT) {
                        wait_val = ++ev_val;
                        EGLAttrib sa[] = { PROBE_EGL_SYNC_METAL_SHARED_EVENT_OBJECT, (EGLAttrib)(__bridge void*)ev,
                                           PROBE_EGL_SYNC_METAL_SHARED_EVENT_VALUE_LO, (EGLAttrib)(wait_val & 0xffffffffu),
                                           PROBE_EGL_SYNC_METAL_SHARED_EVENT_VALUE_HI, (EGLAttrib)(wait_val >> 32), EGL_NONE };
                        EGLSync es = create_sync(dpy, PROBE_EGL_SYNC_METAL_SHARED_EVENT_ANGLE, sa);
                        if (es == EGL_NO_SYNC) {
                            if (!sync_fail) P("  %s: eglCreateSync(METAL_SHARED_EVENT) failed: %s\n", name, egl_err_name(E.GetError()));
                            sync_fail++;
                            wait_val = 0;
                        }
                        G.Flush();
                        if (es != EGL_NO_SYNC) destroy_sync(dpy, es);
                    } else {
                        G.Flush();
                        if (sy == S_SCHEDULED) wait_sched(dpy);
                    }
                    dr[i] = (double)(mach_ns_now() - a) / 1000.0;
                    id<MTLCommandBuffer> cb = [p.queue commandBuffer];
                    if (wait_val) [cb encodeWaitForEvent:ev value:wait_val];
                    id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
                    if (tg == T_IOSURFACE)
                        [b copyFromTexture:own sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                                sourceSize:MTLSizeMake((NSUInteger)W, (NSUInteger)H, 1) toTexture:d.texture destinationSlice:0
                          destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
                    [b copyFromTexture:d.texture sourceSlice:0 sourceLevel:0
                          sourceOrigin:MTLOriginMake((NSUInteger)W / 2, (NSUInteger)H / 2, 0) sourceSize:MTLSizeMake(1, 1, 1)
                              toBuffer:rb destinationOffset:(NSUInteger)i * 4 destinationBytesPerRow:4 destinationBytesPerImage:4];
                    [b endEncoding];
                    f->expect = lv;
                    [p commit:cb drawable:d rec:f mindur:0];
                    n++;
                }
                [p drain];
                int64_t t1 = mach_ns_now();
                E.MakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx);
                for (int k = 0; k < tc.n; k++) {
                    if (tc.surf[k]) E.DestroySurface(dpy, tc.surf[k]);
                    if (tc.fbo[k]) X.DeleteFramebuffers(1, &tc.fbo[k]);
                    if (tc.tex[k]) X.DeleteTextures(1, &tc.tex[k]);
                    if (tc.img[k]) destroy_image(dpy, tc.img[k]);
                }
                int bad = 0;
                const uint8_t* px = (const uint8_t*)rb.contents;
                for (int i = 0; i < n; i++) {
                    r[i].readback = px[i * 4 + 2]; /* BGRA: red */
                    if (abs((int)r[i].readback - (int)r[i].expect) > 1) bad++;
                }
                grid g = cv_grid(p.cv, t0 - 100000000, t1);
                P("RESULT share %s: %d frames, readback mismatches %d, distinct drawable surfaces %d (made in %.1f us each), "
                  "shared-event syncs failed %d\n",
                  name, n, bad, tc.n, tc.made ? tc.create_us / tc.made : 0.0, sync_fail);
                P_stats("eglMakeCurrent (+ bind)", mc, n, "us");
                P_stats("GL draw + sync", dr, n, "us");
                analyze_run(name, r, n, &g);
                write_frames(csv, name, r, n, &g);
                free(r); free(mc); free(dr);
            }
            if (fixed != EGL_NO_SURFACE) E.DestroySurface(dpy, fixed);
            if (ios) CFRelease(ios);
        }
        E.MakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    });
    if (csv) fclose(csv);
    [p close];
    E.DestroyContext(dpy, ctx);
    E.Terminate(dpy);
    return 0;
}
