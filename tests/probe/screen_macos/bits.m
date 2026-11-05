/* Section bits (question 5): 10-bit and half-float rendering through ANGLE's
 * Metal back end, and CAMetalLayer pixel formats. A 1024-pixel ramp is drawn
 * and read back; the count of distinct levels shows the depth that survived.
 * The panel's own depth needs a photometer, not this. */
#import <Cocoa/Cocoa.h>
#import <IOSurface/IOSurface.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "mtl.h"

#define RW 1024

static int distinct_u32(const uint32_t* v, int n) {
    int c = 0;
    for (int i = 0; i < n; i++) if (i == 0 || v[i] != v[i - 1]) c++;
    return c;
}

/* Reads row 0 of the current GL surface in the implementation's format. */
static int gl_ramp_levels(char* how, size_t cap) {
    int fmt = 0, type = 0;
    G.GetIntegerv(PGL_IMPL_READ_FORMAT, &fmt);
    G.GetIntegerv(PGL_IMPL_READ_TYPE, &type);
    uint32_t red[RW];
    if (type == PGL_UNSIGNED_INT_2_10_10_10_REV) {
        static uint32_t px[RW];
        G.ReadPixels(0, 0, RW, 1, (unsigned)fmt, (unsigned)type, px);
        for (int i = 0; i < RW; i++) red[i] = px[i] & 0x3ff;
    } else {
        /* RGBA float is readable from float buffers; others fall back to bytes */
        static float pf[RW * 4];
        static uint8_t pb[RW * 4];
        G.GetError();
        G.ReadPixels(0, 0, RW, 1, PGL_RGBA, PGL_FLOAT, pf);
        if (G.GetError() == 0) {
            for (int i = 0; i < RW; i++) red[i] = (uint32_t)lrintf(pf[i * 4] * 65535.0f);
            type = PGL_FLOAT;
            fmt = PGL_RGBA;
        } else {
            G.ReadPixels(0, 0, RW, 1, PGL_RGBA, PGL_UNSIGNED_BYTE, pb);
            for (int i = 0; i < RW; i++) red[i] = pb[i * 4];
            type = PGL_UNSIGNED_BYTE;
            fmt = PGL_RGBA;
        }
    }
    snprintf(how, cap, "read as format 0x%x type 0x%x", fmt, type);
    return distinct_u32(red, RW);
}

static void draw_ramp(int w, int h) {
    static unsigned prog;
    if (!prog) prog = gl_program(0);
    G.Viewport(0, 0, w, h);
    G.UseProgram(prog);
    int lw = G.GetUniformLocation(prog, "u_width");
    G.Uniform1f(lw, (float)RW);
    G.DrawArrays(PGL_TRIANGLES, 0, 3);
}

static int metal_ramp_levels(id<MTLDevice> dev, id<MTLTexture> t) {
    id<MTLCommandQueue> q = [dev newCommandQueue];
    size_t bpp = t.pixelFormat == MTLPixelFormatRGBA16Float ? 8 : 4;
    id<MTLBuffer> b = [dev newBufferWithLength:RW * bpp options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
    [e copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(RW, 1, 1)
              toBuffer:b destinationOffset:0 destinationBytesPerRow:RW * bpp destinationBytesPerImage:RW * bpp];
    [e endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    uint32_t red[RW];
    for (int i = 0; i < RW; i++) {
        if (bpp == 8) {
            const __fp16* h = (const __fp16*)b.contents;
            red[i] = (uint32_t)lrintf((float)h[i * 4] * 65535.0f);
        } else {
            uint32_t v = ((const uint32_t*)b.contents)[i];
            red[i] = t.pixelFormat == MTLPixelFormatBGR10A2Unorm ? (v >> 20) & 0x3ff
                   : t.pixelFormat == MTLPixelFormatRGB10A2Unorm ? v & 0x3ff : (v >> 16) & 0xff;
        }
    }
    return distinct_u32(red, RW);
}

static struct {
    void (*GenTextures)(int, unsigned*);
    void (*BindTexture)(unsigned, unsigned);
    void (*TexImage2D)(unsigned, int, int, int, int, int, unsigned, unsigned, const void*);
    void (*EGLImageTargetTexture2DOES)(unsigned, void*);
    void (*GenFramebuffers)(int, unsigned*);
    void (*BindFramebuffer)(unsigned, unsigned);
    void (*FramebufferTexture2D)(unsigned, unsigned, unsigned, unsigned, int);
    unsigned (*CheckFramebufferStatus)(unsigned);
} Y;

#define BGL_TEXTURE_2D 0x0DE1
#define BGL_FRAMEBUFFER 0x8D40
#define BGL_COLOR_ATTACHMENT0 0x8CE0
#define BGL_FRAMEBUFFER_COMPLETE 0x8CD5
#define BGL_RGB10_A2 0x8059
#define BGL_RGBA16F 0x881A
#define BGL_RGBA8 0x8058

typedef EGLImage (*create_image_fn)(EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, const EGLAttrib*);

/* Binds a new framebuffer on texture `tex` and reports its status. */
static unsigned fbo_on(unsigned tex) {
    unsigned fbo = 0;
    Y.GenFramebuffers(1, &fbo);
    Y.BindFramebuffer(BGL_FRAMEBUFFER, fbo);
    Y.FramebufferTexture2D(BGL_FRAMEBUFFER, BGL_COLOR_ATTACHMENT0, BGL_TEXTURE_2D, tex, 0);
    return Y.CheckFramebufferStatus(BGL_FRAMEBUFFER);
}

int sec_bits(void) {
    if (!egl_load(g_opt.angle)) return 1;
    EGLDisplay dpy = angle_display("metal");
    if (dpy == EGL_NO_DISPLAY) return 1;
    EGLAttrib edev = 0, emtl = 0;
    E.QueryDisplayAttribEXT(dpy, EGL_DEVICE_EXT, &edev);
    E.QueryDeviceAttribEXT((EGLDeviceEXT)edev, PROBE_EGL_METAL_DEVICE_ANGLE, &emtl);
    id<MTLDevice> dev = (__bridge id<MTLDevice>)(void*)emtl;

    /* 1. EGL configurations: a window surface or pbuffer needs one */
    static const struct { const char* name; int r, a, fl; } reqs[] = {
        { "RGB10_A2", 10, 2, 0 }, { "RGBA16F", 16, 16, 1 } };
    for (int k = 0; k < 2; k++) {
        EGLint ca[] = { EGL_RED_SIZE, reqs[k].r, EGL_GREEN_SIZE, reqs[k].r, EGL_BLUE_SIZE, reqs[k].r,
                        EGL_ALPHA_SIZE, reqs[k].a, EGL_COLOR_COMPONENT_TYPE_EXT,
                        reqs[k].fl ? EGL_COLOR_COMPONENT_TYPE_FLOAT_EXT : EGL_COLOR_COMPONENT_TYPE_FIXED_EXT, EGL_NONE };
        EGLConfig cs[16];
        EGLint nc = 0, exact = 0;
        E.ChooseConfig(dpy, ca, cs, 16, &nc);
        for (int i = 0; i < nc; i++) {
            EGLint r = 0;
            E.GetConfigAttrib(dpy, cs[i], EGL_RED_SIZE, &r);
            exact += r == reqs[k].r;
        }
        P("RESULT bits config %s: eglChooseConfig gives %d, %d with exactly %d red bits\n", reqs[k].name, nc, exact, reqs[k].r);
    }

    /* 2. Render targets that need no config: a GL texture, an EGLImage of an
     * MTLTexture, an IOSurface pbuffer. A context on the RGBA8 config. */
    EGLint c8[] = { EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                    EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, 0x40, EGL_NONE };
    EGLConfig cfg;
    EGLint nc = 0;
    E.ChooseConfig(dpy, c8, &cfg, 1, &nc);
    EGLint xa[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 0, EGL_NONE };
    EGLContext ctx = E.CreateContext(dpy, cfg, EGL_NO_CONTEXT, xa);
    E.MakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx);
    gl_load(egl_proc);
#define L(f, n) Y.f = (__typeof__(Y.f))egl_proc(n)
    L(GenTextures, "glGenTextures"); L(BindTexture, "glBindTexture"); L(TexImage2D, "glTexImage2D");
    L(EGLImageTargetTexture2DOES, "glEGLImageTargetTexture2DOES"); L(GenFramebuffers, "glGenFramebuffers");
    L(BindFramebuffer, "glBindFramebuffer"); L(FramebufferTexture2D, "glFramebufferTexture2D");
    L(CheckFramebufferStatus, "glCheckFramebufferStatus");
#undef L
    create_image_fn create_image = (create_image_fn)egl_proc("eglCreateImage");
    P("Ramps of %d steps; distinct red levels read back (8 bits gives 256, 10 bits 1024)\n", RW);

    static const struct { const char* name; unsigned ifmt, fmt, type; } gt[] = {
        { "GL texture RGBA8", BGL_RGBA8, PGL_RGBA, PGL_UNSIGNED_BYTE },
        { "GL texture RGB10_A2", BGL_RGB10_A2, PGL_RGBA, PGL_UNSIGNED_INT_2_10_10_10_REV },
        { "GL texture RGBA16F", BGL_RGBA16F, PGL_RGBA, PGL_HALF_FLOAT } };
    for (int k = 0; k < 3; k++) {
        unsigned tex = 0;
        Y.GenTextures(1, &tex);
        Y.BindTexture(BGL_TEXTURE_2D, tex);
        Y.TexImage2D(BGL_TEXTURE_2D, 0, (int)gt[k].ifmt, RW, 4, 0, gt[k].fmt, gt[k].type, NULL);
        unsigned st = fbo_on(tex);
        if (st != BGL_FRAMEBUFFER_COMPLETE) { P("  RESULT bits %s: framebuffer status 0x%x\n", gt[k].name, st); continue; }
        draw_ramp(RW, 4);
        char how[96];
        int lv = gl_ramp_levels(how, sizeof how);
        P("  RESULT bits %s: %d distinct levels, %s\n", gt[k].name, lv, how);
    }

    static const struct { const char* name; MTLPixelFormat f; } mt[] = {
        { "BGRA8Unorm", MTLPixelFormatBGRA8Unorm }, { "RGB10A2Unorm", MTLPixelFormatRGB10A2Unorm },
        { "BGR10A2Unorm", MTLPixelFormatBGR10A2Unorm }, { "RGBA16Float", MTLPixelFormatRGBA16Float } };
    for (int k = 0; k < 4 && create_image; k++) {
        MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:mt[k].f width:RW height:4 mipmapped:NO];
        d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        id<MTLTexture> t = [dev newTextureWithDescriptor:d];
        EGLAttrib ia[] = { EGL_NONE };
        EGLImage img = create_image(dpy, EGL_NO_CONTEXT, PROBE_EGL_METAL_TEXTURE_ANGLE, (__bridge EGLClientBuffer)t, ia);
        if (img == EGL_NO_IMAGE) {
            P("  RESULT bits EGLImage of MTLTexture %s: eglCreateImage %s\n", mt[k].name, egl_err_name(E.GetError()));
            continue;
        }
        unsigned tex = 0;
        Y.GenTextures(1, &tex);
        Y.BindTexture(BGL_TEXTURE_2D, tex);
        Y.EGLImageTargetTexture2DOES(BGL_TEXTURE_2D, img);
        unsigned st = fbo_on(tex);
        if (st != BGL_FRAMEBUFFER_COMPLETE) {
            P("  RESULT bits EGLImage of MTLTexture %s: framebuffer status 0x%x\n", mt[k].name, st);
            continue;
        }
        draw_ramp(RW, 4);
        G.Finish();
        P("  RESULT bits EGLImage of MTLTexture %s: %d distinct levels (Metal readback)\n", mt[k].name,
          metal_ramp_levels(dev, t));
    }
    Y.BindFramebuffer(BGL_FRAMEBUFFER, 0);

    static const struct { const char* name; uint32_t cv; int bpe; unsigned ifmt, type; MTLPixelFormat f; } io[] = {
        { "IOSurface 'BGRA'", 'BGRA', 4, 0x80E1 /* BGRA_EXT */, PGL_UNSIGNED_BYTE, MTLPixelFormatBGRA8Unorm },
        { "IOSurface 'l10r' RGB10_A2", 'l10r', 4, BGL_RGB10_A2, PGL_UNSIGNED_INT_2_10_10_10_REV, MTLPixelFormatBGR10A2Unorm },
        { "IOSurface 'RGhA' RGBA half", 'RGhA', 8, PGL_RGBA, PGL_HALF_FLOAT, MTLPixelFormatRGBA16Float } };
    EGLint bind = 0x305F;
    E.GetConfigAttrib(dpy, cfg, PROBE_EGL_BIND_TO_TEXTURE_TARGET_ANGLE, &bind);
    for (int k = 0; k < 3; k++) {
        NSDictionary* props = @{ (id)kIOSurfaceWidth: @(RW), (id)kIOSurfaceHeight: @4,
                                 (id)kIOSurfaceBytesPerElement: @(io[k].bpe), (id)kIOSurfacePixelFormat: @(io[k].cv) };
        IOSurfaceRef s = IOSurfaceCreate((__bridge CFDictionaryRef)props);
        EGLint pa[] = { EGL_WIDTH, RW, EGL_HEIGHT, 4, PROBE_EGL_IOSURFACE_PLANE_ANGLE, 0, 0x3081 /* TEXTURE_TARGET */, bind,
                        PROBE_EGL_TEXTURE_INTERNAL_FORMAT_ANGLE, (EGLint)io[k].ifmt, 0x3080 /* TEXTURE_FORMAT */,
                        0x305E /* TEXTURE_RGBA */, PROBE_EGL_TEXTURE_TYPE_ANGLE, (EGLint)io[k].type, EGL_NONE };
        EGLSurface ps = s ? E.CreatePbufferFromClientBuffer(dpy, PROBE_EGL_IOSURFACE_ANGLE, s, cfg, pa) : EGL_NO_SURFACE;
        if (ps == EGL_NO_SURFACE || !E.MakeCurrent(dpy, ps, ps, ctx)) {
            P("  RESULT bits %s pbuffer: %s\n", io[k].name, egl_err_name(E.GetError()));
            if (s) CFRelease(s);
            continue;
        }
        draw_ramp(RW, 4);
        G.Finish();
        MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:io[k].f width:RW height:4 mipmapped:NO];
        d.usage = MTLTextureUsageShaderRead;
        id<MTLTexture> t = [dev newTextureWithDescriptor:d iosurface:s plane:0];
        P("  RESULT bits %s pbuffer: %d distinct levels (Metal readback)\n", io[k].name, t ? metal_ramp_levels(dev, t) : -1);
        E.MakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx);
        E.DestroySurface(dpy, ps);
        CFRelease(s);
    }
    E.MakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    E.DestroyContext(dpy, ctx);
    E.Terminate(dpy);


    /* CAMetalLayer formats: does the layer take the format, and do frames show? */
    static const struct { const char* name; MTLPixelFormat f; int edr; } lf[] = {
        { "BGRA8Unorm", MTLPixelFormatBGRA8Unorm, 0 },
        { "BGR10A2Unorm", MTLPixelFormatBGR10A2Unorm, 0 },
        { "RGB10A2Unorm", MTLPixelFormatRGB10A2Unorm, 0 },
        { "RGBA16Float", MTLPixelFormatRGBA16Float, 1 },
        { "BGR10_XR", MTLPixelFormatBGR10_XR, 1 },
    };
    P("CAMetalLayer pixel formats (30 frames each)\n");
    if (session_locked() && !g_opt.allow_locked) {
        P("  skipped: the screen is locked\n");
        return 0;
    }
    for (size_t k = 0; k < sizeof lf / sizeof lf[0]; k++) {
        Presenter* p = nil;
        @try {
            p = [[Presenter alloc] initWithDevice:nil format:lf[k].f title:"ysp macOS probe: bits"];
        } @catch (NSException* ex) {
            P("  RESULT layer %s: exception %s\n", lf[k].name, ex.reason.UTF8String);
            continue;
        }
        if (!p) continue;
        if (lf[k].edr) {
            p.layer.wantsExtendedDynamicRangeContent = YES;
            CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceExtendedLinearSRGB);
            p.layer.colorspace = cs;
            CGColorSpaceRelease(cs);
        }
        frame_rec* r = calloc(30, sizeof(frame_rec));
        run_worker(^{
            for (int i = 0; i < 30; i++) { r[i].frame = i; [p frame:&r[i] level:0.5f mindur:0]; }
            [p drain];
        });
        int shown = 0;
        for (int i = 0; i < 30; i++) shown += r[i].presented != 0;
        P("  RESULT layer %s: pixelFormat after set %lu (asked %lu), %d of 30 frames shown\n", lf[k].name,
          (unsigned long)p.layer.pixelFormat, (unsigned long)lf[k].f, shown);
        [p printLayer];
        free(r);
        [p close];
    }
    for (NSScreen* s in [NSScreen screens])
        P("  NSScreen \"%s\": %ld bits per sample, EDR potential %.2f\n", s.localizedName.UTF8String,
          (long)NSBitsPerSampleFromDepth(s.depth), s.maximumPotentialExtendedDynamicRangeColorComponentValue);
    return 0;
}
