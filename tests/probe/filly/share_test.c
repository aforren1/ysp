/* Driver-level answers to docs/filly_probe.md question 1, without Filament.
 *
 * (a) Can a desktop GL 4.1 core context share objects with an ES 3.0
 *     context? GLX (ysp/screen.h's GLX_OML makes its ES context through
 *     GLX_EXT_create_context_es2_profile) and EGL (tests/adapt/gfx_headless.h).
 * (c) Can a texture cross from a GL 4.1 core context to an ES 3.0 context
 *     that shares nothing with it, through an EGLImage on one EGLDisplay, or
 *     through a dma-buf (EGL_MESA_image_dma_buf_export, then
 *     EGL_EXT_image_dma_buf_import)?
 *
 * Each route renders a known color into the texture on the GL 4.1 side,
 * orders it with the route's sync, and reads it back on the ES side.
 *
 * Build: cc -O2 share_test.c -o share_test -lGL -lEGL -lX11
 */
#define _GNU_SOURCE
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/glx.h>
#include <GL/glcorearb.h>
#include <X11/Xlib.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef GLX_CONTEXT_ES2_PROFILE_BIT_EXT
#define GLX_CONTEXT_ES2_PROFILE_BIT_EXT 0x00000004
#endif

/* GL/gl.h defines GL_VERSION_1_1, so glcorearb.h skips these three types. */
typedef void (*PFNGLGENTEXTURESPROC)(GLsizei, GLuint*);
typedef void (*PFNGLBINDTEXTUREPROC)(GLenum, GLuint);
typedef GLboolean (*PFNGLISTEXTUREPROC)(GLuint);

typedef GLXContext (*create_attribs_fn)(Display*, GLXFBConfig, GLXContext, Bool, const int*);

/* GL entry points, loaded per API through glXGetProcAddress or eglGetProcAddress.
 * Mesa returns the same dispatch stubs for both, so one set serves every context. */
static PFNGLGENTEXTURESPROC p_glGenTextures;
static PFNGLBINDTEXTUREPROC p_glBindTexture;
static PFNGLTEXSTORAGE2DPROC p_glTexStorage2D;
static PFNGLGENFRAMEBUFFERSPROC p_glGenFramebuffers;
static PFNGLBINDFRAMEBUFFERPROC p_glBindFramebuffer;
static PFNGLFRAMEBUFFERTEXTURE2DPROC p_glFramebufferTexture2D;
static PFNGLCHECKFRAMEBUFFERSTATUSPROC p_glCheckFramebufferStatus;
static PFNGLCLEARCOLORPROC p_glClearColor;
static PFNGLCLEARPROC p_glClear;
static PFNGLREADPIXELSPROC p_glReadPixels;
static PFNGLFENCESYNCPROC p_glFenceSync;
static PFNGLWAITSYNCPROC p_glWaitSync;
static PFNGLCLIENTWAITSYNCPROC p_glClientWaitSync;
static PFNGLDELETESYNCPROC p_glDeleteSync;
static PFNGLFLUSHPROC p_glFlush;
static PFNGLFINISHPROC p_glFinish;
static PFNGLGETERRORPROC p_glGetError;
static PFNGLGETSTRINGPROC p_glGetString;
static PFNGLISTEXTUREPROC p_glIsTexture;
static PFNGLVIEWPORTPROC p_glViewport;
typedef void (*image_target_fn)(GLenum, void*);
static image_target_fn p_glEGLImageTargetTexture2DOES;

typedef void* (*gpa_fn)(const char*);
static void load_gl(gpa_fn gpa) {
#define L(n) p_##n = (void*)gpa(#n)
    L(glGenTextures); L(glBindTexture); L(glTexStorage2D); L(glGenFramebuffers);
    L(glBindFramebuffer); L(glFramebufferTexture2D); L(glCheckFramebufferStatus);
    L(glClearColor); L(glClear); L(glReadPixels); L(glFenceSync); L(glWaitSync);
    L(glClientWaitSync); L(glDeleteSync); L(glFlush); L(glFinish); L(glGetError);
    L(glGetString); L(glIsTexture); L(glViewport); L(glEGLImageTargetTexture2DOES);
#undef L
}
static void* glx_gpa(const char* n) { return (void*)glXGetProcAddressARB((const GLubyte*)n); }
static void* egl_gpa(const char* n) { return (void*)eglGetProcAddress(n); }

static int x_error;
static int on_x_error(Display* d, XErrorEvent* e) {
    char text[128];
    XGetErrorText(d, e->error_code, text, sizeof text);
    fprintf(stderr, "  X error: %s (code %d, request %d.%d)\n", text, e->error_code, e->request_code, e->minor_code);
    x_error = e->error_code;
    return 0;
}

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

/* Clears the texture to a known color in the current context. */
static void fill(GLuint tex, float r, float g, float b) {
    GLuint fb;
    p_glGenFramebuffers(1, &fb);
    p_glBindFramebuffer(GL_FRAMEBUFFER, fb);
    p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    p_glViewport(0, 0, 64, 64);
    p_glClearColor(r, g, b, 1.0f);
    p_glClear(GL_COLOR_BUFFER_BIT);
    p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

/* Reads texel (5, 5) in the current context. */
static int read_px(GLuint tex, unsigned char px[4]) {
    GLuint fb;
    p_glGenFramebuffers(1, &fb);
    p_glBindFramebuffer(GL_FRAMEBUFFER, fb);
    p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    GLenum st = p_glCheckFramebufferStatus(GL_FRAMEBUFFER);
    memset(px, 0, 4);
    if (st == GL_FRAMEBUFFER_COMPLETE) p_glReadPixels(5, 5, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return st == GL_FRAMEBUFFER_COMPLETE;
}

static int expect(const unsigned char px[4], int r, int g, int b) {
    int ok = abs(px[0] - r) <= 1 && abs(px[1] - g) <= 1 && abs(px[2] - b) <= 1;
    printf("  read %d %d %d %d, expected %d %d %d: %s\n", px[0], px[1], px[2], px[3], r, g, b, ok ? "PASS" : "FAIL");
    return ok;
}

/* --- (a) GLX ----------------------------------------------------------- */
static void glx_share(void) {
    printf("(a) GLX: ES 3.0 context, then a 4.1 core context that shares it\n");
    Display* d = XOpenDisplay(NULL);
    if (!d) { printf("  no X display\n"); return; }
    XSetErrorHandler(on_x_error);
    int fbattr[] = { GLX_DRAWABLE_TYPE, GLX_PBUFFER_BIT, GLX_RENDER_TYPE, GLX_RGBA_BIT,
                     GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8, GLX_ALPHA_SIZE, 8, None };
    int n = 0;
    GLXFBConfig* cfg = glXChooseFBConfig(d, DefaultScreen(d), fbattr, &n);
    if (!cfg || n == 0) { printf("  no FBConfig\n"); return; }
    create_attribs_fn create = (create_attribs_fn)glXGetProcAddressARB((const GLubyte*)"glXCreateContextAttribsARB");
    int es_attr[] = { GLX_CONTEXT_MAJOR_VERSION_ARB, 3, GLX_CONTEXT_MINOR_VERSION_ARB, 0,
                      GLX_CONTEXT_PROFILE_MASK_ARB, GLX_CONTEXT_ES2_PROFILE_BIT_EXT, None };
    int core_attr[] = { GLX_CONTEXT_MAJOR_VERSION_ARB, 4, GLX_CONTEXT_MINOR_VERSION_ARB, 1,
                        GLX_CONTEXT_PROFILE_MASK_ARB, GLX_CONTEXT_CORE_PROFILE_BIT_ARB,
                        GLX_CONTEXT_FLAGS_ARB, GLX_CONTEXT_FORWARD_COMPATIBLE_BIT_ARB, None };
    x_error = 0;
    GLXContext es = create(d, cfg[0], NULL, True, es_attr);
    XSync(d, False);
    printf("  ES 3.0 context: %s\n", es ? "made" : "refused");
    x_error = 0;
    GLXContext core = create(d, cfg[0], es, True, core_attr);
    XSync(d, False);
    printf("  4.1 core context sharing the ES context: %s (X error %d)\n", core ? "made" : "refused", x_error);
    x_error = 0;
    GLXContext core2 = create(d, cfg[0], NULL, True, core_attr);
    GLXContext es2 = core2 ? create(d, cfg[0], core2, True, es_attr) : NULL;
    XSync(d, False);
    printf("  reverse order, ES context sharing a core context: %s (X error %d)\n", es2 ? "made" : "refused", x_error);
    if (!es || !core) { XCloseDisplay(d); return; }

    int pb_attr[] = { GLX_PBUFFER_WIDTH, 64, GLX_PBUFFER_HEIGHT, 64, None };
    GLXPbuffer pb = glXCreatePbuffer(d, cfg[0], pb_attr);
    glXMakeContextCurrent(d, pb, pb, es);
    load_gl(glx_gpa);
    printf("  ES side: %s\n", p_glGetString(GL_VERSION));
    GLuint tex;
    p_glGenTextures(1, &tex);
    p_glBindTexture(GL_TEXTURE_2D, tex);
    p_glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 64, 64);
    p_glFlush();
    glXMakeContextCurrent(d, pb, pb, core);
    printf("  core side: %s; glIsTexture(ES name) = %d\n", p_glGetString(GL_VERSION), p_glIsTexture(tex));
    fill(tex, 0.25f, 0.5f, 0.75f);
    GLsync s = p_glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    p_glFlush();  /* a fence another context waits on must be flushed (GL 4.6 4.1.2) */
    glXMakeContextCurrent(d, pb, pb, es);
    p_glWaitSync(s, 0, GL_TIMEOUT_IGNORED);
    unsigned char px[4];
    read_px(tex, px);
    expect(px, 64, 128, 191);
    p_glDeleteSync(s);
    glXMakeContextCurrent(d, None, None, NULL);
    glXDestroyContext(d, core);
    glXDestroyContext(d, es);
    XCloseDisplay(d);
}

/* --- EGL ------------------------------------------------------------- */
static EGLDisplay egl_open(void) {
    PFNEGLGETPLATFORMDISPLAYEXTPROC gpd = (void*)eglGetProcAddress("eglGetPlatformDisplayEXT");
    EGLDisplay dpy = gpd(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
    EGLint maj, min;
    if (!eglInitialize(dpy, &maj, &min)) return EGL_NO_DISPLAY;
    return dpy;
}
static EGLConfig egl_config(EGLDisplay dpy, EGLint renderable) {
    EGLint attr[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, renderable,
                      EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE };
    EGLConfig c = NULL;
    EGLint n = 0;
    eglChooseConfig(dpy, attr, &c, 1, &n);
    return n ? c : NULL;
}
static EGLContext egl_es(EGLDisplay dpy, EGLContext share) {
    EGLint a[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 0, EGL_NONE };
    eglBindAPI(EGL_OPENGL_ES_API);
    return eglCreateContext(dpy, egl_config(dpy, EGL_OPENGL_ES3_BIT), share, a);
}
static EGLContext egl_core(EGLDisplay dpy, EGLContext share) {
    EGLint a[] = { EGL_CONTEXT_MAJOR_VERSION, 4, EGL_CONTEXT_MINOR_VERSION, 1,
                   EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE };
    eglBindAPI(EGL_OPENGL_API);
    return eglCreateContext(dpy, egl_config(dpy, EGL_OPENGL_BIT), share, a);
}
/* eglMakeCurrent binds a context of the API bound on this thread; switch the API first. */
static void egl_use(EGLDisplay dpy, EGLContext c, int es) {
    eglBindAPI(es ? EGL_OPENGL_ES_API : EGL_OPENGL_API);
    if (!eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, c)) printf("  eglMakeCurrent failed 0x%x\n", eglGetError());
}

static void egl_share(void) {
    printf("(a) EGL (surfaceless): ES 3.0 context, then a 4.1 core context that shares it\n");
    EGLDisplay dpy = egl_open();
    if (dpy == EGL_NO_DISPLAY) { printf("  no EGL display\n"); return; }
    EGLContext es = egl_es(dpy, EGL_NO_CONTEXT);
    printf("  ES 3.0 context: %s\n", es ? "made" : "refused");
    EGLContext core = egl_core(dpy, es);
    EGLint err = eglGetError();
    printf("  4.1 core context sharing the ES context: %s (eglGetError 0x%x)\n", core ? "made" : "refused", core ? EGL_SUCCESS : err);
    if (core) {
        egl_use(dpy, es, 1);
        load_gl(egl_gpa);
        GLuint tex;
        p_glGenTextures(1, &tex);
        p_glBindTexture(GL_TEXTURE_2D, tex);
        p_glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 64, 64);
        p_glFlush();
        egl_use(dpy, core, 0);
        fill(tex, 0.25f, 0.5f, 0.75f);
        GLsync s = p_glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        p_glFlush();
        egl_use(dpy, es, 1);
        p_glWaitSync(s, 0, GL_TIMEOUT_IGNORED);
        unsigned char px[4];
        read_px(tex, px);
        expect(px, 64, 128, 191);
        eglDestroyContext(dpy, core);
    }
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(dpy, es);
    eglTerminate(dpy);
}

/* --- (c) EGLImage and dma-buf ---------------------------------------- */
static void egl_image(int via_dmabuf) {
    printf("(c) %s: 4.1 core texture -> ES 3.0 context, no shared objects\n",
           via_dmabuf ? "dma-buf (EGL_MESA_image_dma_buf_export -> EGL_EXT_image_dma_buf_import)"
                      : "EGLImage on one EGLDisplay (EGL_KHR_gl_texture_2D_image)");
    EGLDisplay dpy = egl_open();
    PFNEGLCREATEIMAGEKHRPROC create_image = (void*)eglGetProcAddress("eglCreateImageKHR");
    PFNEGLDESTROYIMAGEKHRPROC destroy_image = (void*)eglGetProcAddress("eglDestroyImageKHR");
    PFNEGLEXPORTDMABUFIMAGEQUERYMESAPROC query = (void*)eglGetProcAddress("eglExportDMABUFImageQueryMESA");
    PFNEGLEXPORTDMABUFIMAGEMESAPROC export_ = (void*)eglGetProcAddress("eglExportDMABUFImageMESA");
    PFNEGLCREATESYNCKHRPROC create_sync = (void*)eglGetProcAddress("eglCreateSyncKHR");
    PFNEGLWAITSYNCKHRPROC wait_sync = (void*)eglGetProcAddress("eglWaitSyncKHR");
    PFNEGLDESTROYSYNCKHRPROC destroy_sync = (void*)eglGetProcAddress("eglDestroySyncKHR");
    /* For the dma-buf route the consumer is a second EGLDisplay, as a separate
     * library or process would have; Mesa gives a separate screen per display
     * only for a separate device fd, so this is the closest in-process test. */
    EGLDisplay dpy2 = dpy;
    EGLContext core = egl_core(dpy, EGL_NO_CONTEXT);
    EGLContext es = egl_es(dpy2, EGL_NO_CONTEXT);
    egl_use(dpy, core, 0);
    load_gl(egl_gpa);
    GLuint src;
    p_glGenTextures(1, &src);
    p_glBindTexture(GL_TEXTURE_2D, src);
    p_glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 64, 64);
    EGLImageKHR img = create_image(dpy, core, EGL_GL_TEXTURE_2D_KHR, (EGLClientBuffer)(uintptr_t)src, NULL);
    printf("  eglCreateImage from the core texture: %s (0x%x)\n", img ? "made" : "refused", img ? 0 : eglGetError());
    if (!img) return;
    EGLImageKHR es_img = img;
    int fd = -1;
    if (via_dmabuf) {
        int fourcc = 0, planes = 0;
        EGLuint64KHR mod = 0;
        EGLint stride = 0, offset = 0;
        if (!query(dpy, img, &fourcc, &planes, &mod) || !export_(dpy, img, &fd, &stride, &offset)) {
            printf("  export failed 0x%x\n", eglGetError());
            return;
        }
        printf("  exported: fourcc %.4s, %d plane(s), modifier 0x%llx, stride %d, offset %d, fd %d\n",
               (char*)&fourcc, planes, (unsigned long long)mod, stride, offset, fd);
        EGLint a[] = { EGL_WIDTH, 64, EGL_HEIGHT, 64, EGL_LINUX_DRM_FOURCC_EXT, fourcc,
                       EGL_DMA_BUF_PLANE0_FD_EXT, fd, EGL_DMA_BUF_PLANE0_OFFSET_EXT, offset,
                       EGL_DMA_BUF_PLANE0_PITCH_EXT, stride,
                       EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, (EGLint)(mod & 0xffffffff),
                       EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, (EGLint)(mod >> 32), EGL_NONE };
        es_img = create_image(dpy2, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, a);
        printf("  import as EGL_LINUX_DMA_BUF_EXT: %s (0x%x)\n", es_img ? "made" : "refused", es_img ? 0 : eglGetError());
        if (!es_img) return;
    }
    /* Producer: render, then an EGL fence; EGL syncs belong to the display, so
     * the consumer context can wait on it on the GPU (EGL_KHR_wait_sync). */
    double t0 = now_ms();
    fill(src, 0.25f, 0.5f, 0.75f);
    EGLSyncKHR sync = create_sync(dpy, EGL_SYNC_FENCE_KHR, NULL);
    p_glFlush();
    egl_use(dpy2, es, 1);
    GLuint dst;
    p_glGenTextures(1, &dst);
    p_glBindTexture(GL_TEXTURE_2D, dst);
    p_glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, es_img);
    printf("  glEGLImageTargetTexture2DOES on the ES side: GL error 0x%x\n", p_glGetError());
    wait_sync(dpy2, sync, 0);
    unsigned char px[4];
    read_px(dst, px);
    double t1 = now_ms();
    expect(px, 64, 128, 191);
    printf("  produce, sync, consume and read back: %.3f ms (one shot, includes the readback stall)\n", t1 - t0);
    destroy_sync(dpy, sync);
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (es_img != img) destroy_image(dpy2, es_img);
    destroy_image(dpy, img);
    if (fd >= 0) close(fd);
    eglDestroyContext(dpy, core);
    eglDestroyContext(dpy2, es);
    eglTerminate(dpy);
}

int main(void) {
    glx_share();
    egl_share();
    egl_image(0);
    egl_image(1);
    return 0;
}
