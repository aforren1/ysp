/* Section link: frames driven by CAMetalDisplayLink,
 * Apple's pacing model, against presentDrawable:atTime: (section present).
 *
 * The question (docs/screen_macos_probe.md, 4.8): does the link schedule
 * deeper than our depth 2, and does preferredFrameLatency change that?
 * Each frame is drawn the way ysp/screen.h's METAL backend draws: ANGLE into
 * the drawable's IOSurface through a pbuffer kept per IOSurface, ended with
 * an EGL_ANGLE_metal_shared_event_sync signal that the probe's command
 * buffer waits on, then the center pixel read back and the drawable
 * presented. Runs, each at preferredFrameLatency 1, 2 and the default:
 *   free         draw in every update; present the update's drawable.
 *   target_wait  ask for a vblank 3 updates ahead by drawing only in the
 *                update whose targetPresentationTimestamp is that vblank.
 *   target_at    draw in the first update, and present its drawable with
 *                presentDrawable:atTime: that vblank (if the link takes it).
 * The link calls back on a thread of the probe's own, whose run loop it is
 * added to; ANGLE's context is current there. */
#import <Cocoa/Cocoa.h>
#import <IOSurface/IOSurface.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "mtl.h"

#define LGL_BGRA_EXT 0x80E1
#define LEGL_TEXTURE_FORMAT 0x3080
#define LEGL_TEXTURE_TARGET 0x3081
#define LEGL_TEXTURE_RGBA 0x305E

enum { LINK_FREE = 0, LINK_TARGET_WAIT = 1, LINK_TARGET_AT = 2 };
static const char* const link_run_name[3] = { "free", "target_wait", "target_at" };

typedef struct link_rec {
    int64_t upd_arrive, upd_target, upd_present;  /* mach ns                     */
    int64_t t_commit, planned;                    /* planned: the vblank asked  */
    volatile int64_t presented, pres_arrive;
    volatile int complete;
    uint32_t readback, expect;
} link_rec;

typedef EGLSync (*lk_create_sync_fn)(EGLDisplay, EGLenum, const EGLAttrib*);
typedef EGLBoolean (*lk_destroy_sync_fn)(EGLDisplay, EGLSync);

/* What one run needs on the link's thread. */
typedef struct link_run {
    Presenter* p;
    EGLDisplay dpy;
    EGLConfig cfg;
    EGLContext ctx;
    EGLint bind_target;
    unsigned prog;
    int kind, n, max, heavy, w, h, trial, reps, ahead;
    int64_t want;          /* target runs: the vblank asked, 0 = choose at the next update */
    int64_t period;        /* from the updates' targetPresentationTimestamp steps */
    int64_t last_present;
    link_rec* r;
    id<MTLBuffer> rb;
    id<MTLSharedEvent> ev;
    uint64_t ev_val;
    void* key[8];
    EGLSurface pb[8];
    int npb;
    lk_create_sync_fn create_sync;
    lk_destroy_sync_fn destroy_sync;
    volatile int done;
    int skipped, refused_at;
} link_run;

static uint8_t lk_level(int i) { return (uint8_t)(40 + (i * 7) % 170); }

static EGLSurface lk_pbuffer(link_run* L, IOSurfaceRef ios) {
    int k;
    for (k = 0; k < L->npb; k++) if (L->key[k] == ios) return L->pb[k];
    if (L->npb == 8) return EGL_NO_SURFACE;
    EGLint pa[] = { EGL_WIDTH, L->w, EGL_HEIGHT, L->h, PROBE_EGL_IOSURFACE_PLANE_ANGLE, 0,
                    LEGL_TEXTURE_TARGET, L->bind_target, PROBE_EGL_TEXTURE_INTERNAL_FORMAT_ANGLE, LGL_BGRA_EXT,
                    LEGL_TEXTURE_FORMAT, LEGL_TEXTURE_RGBA, PROBE_EGL_TEXTURE_TYPE_ANGLE, PGL_UNSIGNED_BYTE, EGL_NONE };
    EGLSurface s = E.CreatePbufferFromClientBuffer(L->dpy, PROBE_EGL_IOSURFACE_ANGLE, ios, L->cfg, pa);
    if (s == EGL_NO_SURFACE) return s;
    L->key[L->npb] = ios;
    L->pb[L->npb++] = s;
    return s;
}

/* One frame: ANGLE draws into the update's drawable, the shared event
 * orders it before the probe's command buffer, which reads the center
 * pixel back and presents (at `at` when not 0). */
static void lk_frame(link_run* L, id<CAMetalDrawable> d, link_rec* f, int index, int64_t at) {
    IOSurfaceRef ios = d.texture.iosurface;
    EGLSurface s = ios ? lk_pbuffer(L, ios) : EGL_NO_SURFACE;
    uint8_t lv = lk_level(index);
    float v = lv / 255.0f;
    uint64_t val = 0;
    if (s == EGL_NO_SURFACE) { f->complete = 1; return; }
    E.MakeCurrent(L->dpy, s, s, L->ctx);
    G.Viewport(0, 0, L->w, L->h);
    G.ClearColor(v, v, v, 1);
    G.Clear(PGL_COLOR_BUFFER_BIT);
    if (L->prog) gl_draw_fullscreen(L->prog, L->heavy, v);
    if (L->create_sync) {
        val = ++L->ev_val;
        EGLAttrib sa[] = { PROBE_EGL_SYNC_METAL_SHARED_EVENT_OBJECT, (EGLAttrib)(__bridge void*)L->ev,
                           PROBE_EGL_SYNC_METAL_SHARED_EVENT_VALUE_LO, (EGLAttrib)(val & 0xffffffffu),
                           PROBE_EGL_SYNC_METAL_SHARED_EVENT_VALUE_HI, (EGLAttrib)(val >> 32), EGL_NONE };
        EGLSync es = L->create_sync(L->dpy, PROBE_EGL_SYNC_METAL_SHARED_EVENT_ANGLE, sa);
        G.Flush();
        if (es != EGL_NO_SYNC) L->destroy_sync(L->dpy, es);
        else { val = 0; G.Finish(); }
    } else {
        G.Finish();
    }
    E.MakeCurrent(L->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, L->ctx);
    id<MTLCommandBuffer> cb = [L->p.queue commandBuffer];
    if (val) [cb encodeWaitForEvent:L->ev value:val];
    id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
    [b copyFromTexture:d.texture sourceSlice:0 sourceLevel:0
          sourceOrigin:MTLOriginMake((NSUInteger)L->w / 2, (NSUInteger)L->h / 2, 0) sourceSize:MTLSizeMake(1, 1, 1)
              toBuffer:L->rb destinationOffset:(NSUInteger)index * 4 destinationBytesPerRow:4 destinationBytesPerImage:4];
    [b endEncoding];
    f->expect = lv;
    [d addPresentedHandler:^(id<MTLDrawable> dd) {
        f->pres_arrive = mach_ns_now();
        f->presented = dd.presentedTime > 0 ? secs_to_ns(dd.presentedTime) : 0;
        __atomic_store_n(&f->complete, 1, __ATOMIC_RELEASE);
    }];
    if (at) [cb presentDrawable:d atTime:ns_to_secs(at)];
    else [cb presentDrawable:d];
    [cb commit];
    f->t_commit = mach_ns_now();
}

@interface LinkDelegate : NSObject <CAMetalDisplayLinkDelegate>
@property(nonatomic) link_run* run;
@end

@implementation LinkDelegate
- (void)metalDisplayLink:(CAMetalDisplayLink*)link needsUpdate:(CAMetalDisplayLinkUpdate*)update {
    link_run* L = _run;
    int64_t arrive = mach_ns_now();
    int64_t tgt = secs_to_ns(update.targetTimestamp), pres = secs_to_ns(update.targetPresentationTimestamp);
    if (L->done || L->n >= L->max) { L->done = 1; return; }
    if (L->last_present && pres > L->last_present) {
        int64_t d = pres - L->last_present;
        if (!L->period || d < L->period * 3 / 2) L->period = L->period ? (L->period * 7 + d) / 8 : d;
    }
    L->last_present = pres;
    if (L->kind != LINK_FREE) {
        if (!L->period || L->want == -1) return; /* the period first; a trial's frame not shown yet */
        if (!L->want) {                         /* a trial: the vblank `ahead` updates on */
            L->want = pres + L->ahead * L->period;
            if (L->kind == LINK_TARGET_WAIT) return;
        } else if (L->kind == LINK_TARGET_WAIT) {
            if (llabs(pres - L->want) > L->period / 2) {
                if (pres > L->want) { L->want = 0; L->skipped++; }   /* passed it: a new trial */
                return;
            }
        } else {
            return;                             /* target_at: one frame a trial, made already */
        }
    }
    link_rec* f = &L->r[L->n];
    f->upd_arrive = arrive;
    f->upd_target = tgt;
    f->upd_present = pres;
    f->planned = L->kind == LINK_FREE ? 0 : L->want;
    @try {
        lk_frame(L, update.drawable, f, L->n, L->kind == LINK_TARGET_AT ? L->want : 0);
    } @catch (NSException* e) {
        L->refused_at++;
        f->complete = 1;
    }
    L->n++;
    if (L->kind != LINK_FREE) {
        /* the next trial starts after this frame is shown */
        L->want = -1;
    }
    (void)link;
}
@end

static void lk_report(link_run* L, const char* name, double latency, FILE* csv) {
    int i, n = L->n, shown = 0, notshown = 0, bad = 0, steps[5] = { 0 }, on = 0, early = 0, late = 0;
    double *lat = calloc((size_t)n + 1, sizeof(double)), *prom = calloc((size_t)n + 1, sizeof(double)),
           *u2l = calloc((size_t)n + 1, sizeof(double)), *arr = calloc((size_t)n + 1, sizeof(double)),
           *upd = calloc((size_t)n + 1, sizeof(double));
    int nl = 0, np = 0, nu = 0, na = 0, nd = 0;
    int64_t prev = 0, per = L->period > 0 ? L->period : 16680375;
    const uint8_t* px = (const uint8_t*)L->rb.contents;
    for (i = 0; i < n; i++) {
        link_rec* f = &L->r[i];
        f->readback = px[i * 4 + 2];
        if (abs((int)f->readback - (int)f->expect) > 1) bad++;
        upd[nd++] = (double)(f->upd_present - f->upd_arrive) / 1e6;
        if (!f->presented) { notshown++; continue; }
        shown++;
        lat[nl++] = (double)(f->presented - f->t_commit) / 1e6;
        prom[np++] = (double)(f->presented - f->upd_present) / 1e3;
        u2l[nu++] = (double)(f->presented - f->upd_arrive) / (double)per;
        arr[na++] = (double)(f->pres_arrive - f->presented) / 1e3;
        if (prev) {
            long long s = llround((double)(f->presented - prev) / (double)per);
            steps[s < 0 ? 0 : s > 4 ? 4 : s]++;
        }
        prev = f->presented;
        if (f->planned > 0) {
            long long d = llround((double)(f->presented - f->planned) / (double)per);
            if (d == 0) on++; else if (d < 0) early++; else late++;
        }
    }
    P(" run %s, preferredFrameLatency %s: %d frames, %d shown, %d not shown, readback mismatches %d, "
      "skipped trials %d, presentDrawable:atTime: refused %d; period from the updates %.3f us\n",
      name, latency > 0 ? (latency == 1 ? "1" : "2") : "default", n, shown, notshown, bad, L->skipped, L->refused_at,
      (double)per / 1e3);
    P("  vblank steps between shown frames (presentedTime): 0:%d 1:%d 2:%d 3:%d 4+:%d\n", steps[0], steps[1], steps[2],
      steps[3], steps[4]);
    if (on + early + late) P("  against the vblank asked: on %d, early %d, late %d\n", on, early, late);
    P_stats("presented minus commit", lat, nl, "ms");
    P_stats("presented minus targetPresentationTimestamp", prom, np, "us");
    P_stats("update to presented (periods)", u2l, nu, "");
    P_stats("targetPresentationTimestamp minus update", upd, nd, "ms");
    P_stats("presented handler minus presentedTime", arr, na, "us");
    for (i = 0; csv && i < n; i++) {
        link_rec* f = &L->r[i];
        fprintf(csv, "%s,%.1f,%d,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%u,%u\n", name, latency, i, (long long)f->upd_arrive,
                (long long)f->upd_target, (long long)f->upd_present, (long long)f->t_commit, (long long)f->planned,
                (long long)f->presented, (long long)f->pres_arrive, f->readback, f->expect);
    }
    free(lat); free(prom); free(u2l); free(arr); free(upd);
}

int sec_link(void) {
    if (!egl_load(g_opt.angle)) return 1;
    EGLDisplay dpy = angle_display("metal");
    if (dpy == EGL_NO_DISPLAY) return 1;
    EGLAttrib edev = 0, emtl = 0;
    if (!E.QueryDisplayAttribEXT(dpy, EGL_DEVICE_EXT, &edev) ||
        !E.QueryDeviceAttribEXT((EGLDeviceEXT)edev, PROBE_EGL_METAL_DEVICE_ANGLE, &emtl) || !emtl) {
        P("RESULT link: no ANGLE Metal device\n");
        return 1;
    }
    id<MTLDevice> dev = (__bridge id<MTLDevice>)(void*)emtl;
    EGLint ca[] = { EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                    EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, 0x40, EGL_NONE };
    EGLConfig cfg;
    EGLint nc = 0;
    if (!E.ChooseConfig(dpy, ca, &cfg, 1, &nc) || nc < 1) return 1;
    EGLint xa[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 0, EGL_NONE };
    EGLContext ctx = E.CreateContext(dpy, cfg, EGL_NO_CONTEXT, xa);
    EGLint bind = 0x305F;
    E.GetConfigAttrib(dpy, cfg, PROBE_EGL_BIND_TO_TEXTURE_TARGET_ANGLE, &bind);

    Presenter* p = [[Presenter alloc] initWithDevice:dev format:MTLPixelFormatBGRA8Unorm title:"ysp macOS probe: link"];
    if (!p) return 1;
    pump_for(1.0);
    FILE* csv = open_csv(tagged("link"), "run,latency,frame,upd_arrive,upd_target,upd_present,t_commit,planned,presented,"
                                         "pres_arrive,readback,expect");
    static const double lats[3] = { 1, 2, 0 };   /* 0: the link's own default */
    run_worker(^{
        E.MakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx);
        gl_load(egl_proc);
        unsigned prog = g_opt.heavy ? gl_program(1) : 0;
        for (int k = 0; k < 3 && !quit_requested(); k++) {
            for (int kind = 0; kind < 3 && !quit_requested(); kind++) {
                char want[16];
                snprintf(want, sizeof want, "%s", link_run_name[kind]);
                if (g_opt.runs && !strstr(g_opt.runs, want)) continue;
                link_run L = { 0 };
                L.p = p; L.dpy = dpy; L.cfg = cfg; L.ctx = ctx; L.bind_target = bind; L.prog = prog;
                L.kind = kind;
                L.max = kind == LINK_FREE ? g_opt.frames : g_opt.reps;
                L.heavy = g_opt.heavy;
                L.ahead = 3;
                L.w = (int)p.layer.drawableSize.width;
                L.h = (int)p.layer.drawableSize.height;
                L.r = (link_rec*)calloc((size_t)L.max + 1, sizeof(link_rec));   /* never freed: late handlers */
                L.rb = [dev newBufferWithLength:(NSUInteger)(L.max + 1) * 4 options:MTLResourceStorageModeShared];
                L.ev = [dev newSharedEvent];
                L.create_sync = (lk_create_sync_fn)egl_proc("eglCreateSync");
                L.destroy_sync = (lk_destroy_sync_fn)egl_proc("eglDestroySync");
                CAMetalDisplayLink* link = [[CAMetalDisplayLink alloc] initWithMetalLayer:p.layer];
                LinkDelegate* del = [LinkDelegate new];
                del.run = &L;
                link.delegate = del;
                float def = link.preferredFrameLatency;
                if (lats[k] > 0) link.preferredFrameLatency = (float)lats[k];
                P("  link: preferredFrameLatency %.1f (default %.1f)\n", link.preferredFrameLatency, def);
                [link addToRunLoop:[NSRunLoop currentRunLoop] forMode:NSDefaultRunLoopMode];
                int64_t end = mach_ns_now() + secs_to_ns(kind == LINK_FREE ? 2.0 + g_opt.frames / 30.0
                                                                          : 2.0 + g_opt.reps * 0.2);
                while (!L.done && mach_ns_now() < end && !quit_requested()) {
                    if (L.want == -1) {   /* target runs: wait for the frame, then a new trial */
                        link_rec* f = &L.r[L.n - 1];
                        if (__atomic_load_n(&f->complete, __ATOMIC_ACQUIRE) ||
                            mach_ns_now() - f->t_commit > 500000000)
                            L.want = 0;
                    }
                    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.002, true);
                }
                [link invalidate];
                /* the last handlers */
                int64_t lim = mach_ns_now() + 1000000000;
                while (L.n > 0 && !__atomic_load_n(&L.r[L.n - 1].complete, __ATOMIC_ACQUIRE) && mach_ns_now() < lim)
                    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.005, true);
                char name[48];
                snprintf(name, sizeof name, "%s", link_run_name[kind]);
                lk_report(&L, name, lats[k], csv);
                E.MakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx);
                for (int j = 0; j < L.npb; j++) E.DestroySurface(dpy, L.pb[j]);
            }
        }
        E.MakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    });
    if (csv) fclose(csv);
    cv_report(p.cv, tagged("link_cv"));
    print_window(p.window);
    [p close];
    E.DestroyContext(dpy, ctx);
    E.Terminate(dpy);
    return 0;
}
