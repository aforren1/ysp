/* The probe's CAMetalLayer presenter and the per-run analysis. */
#import <Cocoa/Cocoa.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "mtl.h"

void run_worker(void (^work)(void)) {
    __block volatile int done = 0;
    NSThread* t = [[NSThread alloc] initWithBlock:^{
        @autoreleasepool { work(); }
        __atomic_store_n(&done, 1, __ATOMIC_RELEASE);
    }];
    t.qualityOfService = NSQualityOfServiceUserInteractive;
    [t start];
    while (!__atomic_load_n(&done, __ATOMIC_ACQUIRE)) pump_for(0.01);
}

static id<MTLTexture> solid(id<MTLDevice> dev, MTLPixelFormat fmt, float v) {
    MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt width:64 height:64 mipmapped:NO];
    d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    d.storageMode = MTLStorageModePrivate;
    id<MTLTexture> t = [dev newTextureWithDescriptor:d];
    id<MTLCommandQueue> q = [dev newCommandQueue];
    id<MTLCommandBuffer> cb = [q commandBuffer];
    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = t;
    rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(v, v, v, 1);
    [[cb renderCommandEncoderWithDescriptor:rp] endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    return t;
}

@implementation Presenter

- (instancetype)initWithDevice:(id<MTLDevice>)device format:(MTLPixelFormat)format title:(const char*)title {
    if (!(self = [super init])) return nil;
    _device = device ? device : MTLCreateSystemDefaultDevice();
    _window = open_window(false, title);
    if (!_window) return nil;
    _view = SDL_Metal_CreateView(_window);
    _layer = (__bridge CAMetalLayer*)SDL_Metal_GetLayer(_view);
    _layer.device = _device;
    _layer.pixelFormat = format;
    /* the corner patch is a blit into the drawable */
    _layer.framebufferOnly = NO;
    _layer.maximumDrawableCount = (NSUInteger)g_opt.drawables;
    _layer.displaySyncEnabled = g_opt.sync_off ? NO : YES;
    _queue = [_device newCommandQueue];
    _patchOn = solid(_device, format, 1.0f);
    _patchOff = solid(_device, format, 0.0f);
    _presentedSem = dispatch_semaphore_create(0);
    _cv = calloc(1, sizeof(cv_link));
    NSScreen* s = [(__bridge NSWindow*)SDL_GetPointerProperty(SDL_GetWindowProperties(_window),
                                                              SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, NULL) screen];
    uint32_t did = [s.deviceDescription[@"NSScreenNumber"] unsignedIntValue];
    if (!cv_start(_cv, did ? did : CGMainDisplayID(), 1 << 18)) P("  CVDisplayLink failed to start\n");
    [self printLayer];
    return self;
}

- (void)printLayer {
    CAMetalLayer* l = _layer;
    CGColorSpaceRef cs = l.colorspace;
    CFStringRef csn = cs ? CGColorSpaceCopyName(cs) : NULL;
    P("  CAMetalLayer: device \"%s\", pixelFormat %lu, drawableSize %.0fx%.0f, contentsScale %.1f, "
      "maximumDrawableCount %lu, displaySyncEnabled %d, presentsWithTransaction %d, framebufferOnly %d, "
      "wantsEDR %d, colorspace %s\n",
      l.device.name.UTF8String, (unsigned long)l.pixelFormat, l.drawableSize.width, l.drawableSize.height,
      l.contentsScale, (unsigned long)l.maximumDrawableCount, l.displaySyncEnabled, l.presentsWithTransaction,
      l.framebufferOnly, l.wantsExtendedDynamicRangeContent, csn ? CFStringGetCStringPtr(csn, kCFStringEncodingUTF8) : "(none)");
    if (csn) CFRelease(csn);
}

- (void)close {
    [self drain];
    if (_cv) { cv_stop(_cv); }
    if (_view) SDL_Metal_DestroyView(_view);
    close_window(_window);
    _window = NULL;
}

- (void)frame:(frame_rec*)r level:(float)level mindur:(double)mindur {
    @autoreleasepool {
        r->t_req = mach_ns_now();
        id<CAMetalDrawable> d = [_layer nextDrawable];
        r->t_got = mach_ns_now();
        if (!d) { r->complete = 1; return; }
        r->drawable_id = (uint64_t)d.drawableID;
        id<MTLCommandBuffer> cb = [_queue commandBuffer];
        MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = d.texture;
        rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(level, level, level, 1);
        [[cb renderCommandEncoderWithDescriptor:rp] endEncoding];
        id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
        [b copyFromTexture:(r->frame & 1) ? _patchOn : _patchOff sourceSlice:0 sourceLevel:0
              sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(64, 64, 1)
                 toTexture:d.texture destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
        [b endEncoding];
        [self commit:cb drawable:d rec:r mindur:mindur];
    }
}

/* Shared by frame: and the share section, which encodes its own work. */
- (void)commit:(id<MTLCommandBuffer>)cb drawable:(id<CAMetalDrawable>)d rec:(frame_rec*)r mindur:(double)mindur {
    dispatch_semaphore_t sem = _presentedSem;
    __atomic_add_fetch(&_outstanding, 1, __ATOMIC_ACQ_REL);
    Presenter* me = self;
    [cb addScheduledHandler:^(id<MTLCommandBuffer> c) { r->sched = mach_ns_now(); }];
    [cb addCompletedHandler:^(id<MTLCommandBuffer> c) {
        r->done = mach_ns_now();
        r->gpu_start = secs_to_ns(c.GPUStartTime);
        r->gpu_end = secs_to_ns(c.GPUEndTime);
    }];
    [d addPresentedHandler:^(id<MTLDrawable> dd) {
        r->pres_arrive = mach_ns_now();
        r->presented = dd.presentedTime > 0 ? secs_to_ns(dd.presentedTime) : 0;
        __atomic_store_n(&r->complete, 1, __ATOMIC_RELEASE);
        __atomic_sub_fetch(&me->_outstanding, 1, __ATOMIC_ACQ_REL);
        dispatch_semaphore_signal(sem);
    }];
    if (r->mode == PM_AT) [cb presentDrawable:d atTime:ns_to_secs(r->target)];
    else if (r->mode == PM_MINDUR) [cb presentDrawable:d afterMinimumDuration:mindur];
    else [cb presentDrawable:d];
    double p;
    cv_latest(_cv, &r->live_vblank, &p);
    [cb commit];
    r->t_commit = mach_ns_now();
}

- (void)drain {
    int64_t end = mach_ns_now() + 1000000000;
    while (__atomic_load_n(&_outstanding, __ATOMIC_ACQUIRE) > 0 && mach_ns_now() < end)
        dispatch_semaphore_wait(_presentedSem, dispatch_time(DISPATCH_TIME_NOW, 20000000));
    /* drop stale signals */
    while (dispatch_semaphore_wait(_presentedSem, DISPATCH_TIME_NOW) == 0) {}
}
@end

/* ---- analysis --------------------------------------------------------------- */
const char* frames_header(void) {
    return "run,frame,mode,lead,frac,late_inj,t_req,t_got,t_commit,target,planned,live_vblank,sched,done,"
           "gpu_start,gpu_end,presented,pres_arrive,drawable_id,readback,expect,"
           "v_commit,v_planned,v_shown,resid_us";
}

/* presentedTime need not sit on CVDisplayLink's inNow: the CV grid moved by
 * the circular mean of the presented times' residuals. A shown frame's
 * vblank index is its index on this grid, so index k means "the frame shown
 * at the vblank that follows CV vblank k by `phase`". */
grid presented_grid(const grid* g, const frame_rec* r, int n, double* phase_us) {
    grid p = *g;
    *phase_us = 0;
    if (!g->ok) return p;
    double sx = 0, sy = 0;
    int m = 0;
    for (int i = 0; i < n; i++) {
        if (!r[i].presented) continue;
        double a = 2 * M_PI * grid_residual_us(g, r[i].presented) * 1000.0 / g->period;
        sx += cos(a); sy += sin(a); m++;
    }
    if (!m) return p;
    double ph = atan2(sy, sx) / (2 * M_PI) * g->period;
    p.t0 += ph;
    *phase_us = ph / 1000.0;
    return p;
}

void write_frames(FILE* csv, const char* name, const frame_rec* r, int n, const grid* g) {
    if (!csv) return;
    double phase;
    grid gp = presented_grid(g, r, n, &phase);
    for (int i = 0; i < n; i++) {
        const frame_rec* f = &r[i];
        double vc = g->ok ? floor(grid_index(g, f->t_commit)) : 0;
        long long vp = (g->ok && f->planned) ? (long long)grid_round(g, f->planned) : 0;
        long long vs = (g->ok && f->presented) ? (long long)grid_round(&gp, f->presented) : 0;
        double res = (g->ok && f->presented) ? grid_residual_us(g, f->presented) : 0;
        fprintf(csv, "%s,%d,%d,%d,%.3f,%d,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%llu,%u,%u,"
                     "%.0f,%lld,%lld,%.3f\n",
                name, f->frame, f->mode, f->lead, f->frac, f->late_inj, (long long)f->t_req, (long long)f->t_got,
                (long long)f->t_commit, (long long)f->target, (long long)f->planned, (long long)f->live_vblank,
                (long long)f->sched, (long long)f->done, (long long)f->gpu_start, (long long)f->gpu_end,
                (long long)f->presented, (long long)f->pres_arrive, (unsigned long long)f->drawable_id,
                f->readback, f->expect, vc, vp, vs, res);
    }
}

/* One summary block per run. The depth is the shown vblank minus the last
 * vblank before the commit returned, as screen.md defines it. */
void analyze_run(const char* name, frame_rec* r, int n, const grid* g) {
    int shown = 0, notshown = 0, incomplete = 0, depth[6] = { 0 }, step[5] = { 0 };
    int on = 0, early = 0, late = 0, late_inj_late = 0, late_inj = 0;
    double *lat = calloc((size_t)n, sizeof(double)), *res = calloc((size_t)n, sizeof(double)),
           *arr = calloc((size_t)n, sizeof(double)), *nd = calloc((size_t)n, sizeof(double)),
           *cm = calloc((size_t)n, sizeof(double)), *gpu = calloc((size_t)n, sizeof(double)),
           *sch = calloc((size_t)n, sizeof(double));
    int nl = 0, nr = 0, na = 0, nn = 0, nc = 0, ng = 0, ns = 0;
    long long prev_shown = INT64_MIN;
    double phase_us;
    grid gp = presented_grid(g, r, n, &phase_us);
    for (int i = 0; i < n; i++) {
        frame_rec* f = &r[i];
        nd[nn++] = (double)(f->t_got - f->t_req) / 1000.0;
        if (f->t_commit) cm[nc++] = (double)(f->t_commit - f->t_got) / 1000.0;
        if (f->gpu_end > 0 && f->gpu_start > 0) gpu[ng++] = (double)(f->gpu_end - f->gpu_start) / 1000.0;
        if (f->sched) sch[ns++] = (double)(f->sched - f->t_commit) / 1000.0;
        if (!f->complete) { incomplete++; continue; }
        if (!f->presented) { notshown++; continue; }
        shown++;
        lat[nl++] = (double)(f->presented - f->t_commit) / 1e6;
        arr[na++] = (double)(f->pres_arrive - f->presented) / 1000.0;
        if (!g->ok) continue;
        res[nr++] = fabs(grid_residual_us(&gp, f->presented));
        long long vs = grid_round(&gp, f->presented);
        long long vc = (long long)floor(grid_index(g, f->t_commit));
        if (f->mode == PM_ASAP) {
            long long dd = vs - vc;
            depth[dd < 0 ? 0 : dd > 5 ? 5 : dd]++;
        }
        if (f->planned) {
            long long vp = grid_round(g, f->planned);
            if (vs == vp) on++;
            else if (vs < vp) early++;
            else { late++; if (f->late_inj) late_inj_late++; }
        }
        if (f->late_inj) late_inj++;
        if (prev_shown != INT64_MIN) {
            long long s = vs - prev_shown;
            step[s < 0 ? 0 : s > 4 ? 4 : s]++;
        }
        prev_shown = vs;
    }
    P(" run %s: %d frames, %d shown, %d not shown (presentedTime 0), %d without a handler\n", name, n, shown,
      notshown, incomplete);
    if (g->ok) {
        P("  vblank steps between shown frames: 0:%d 1:%d 2:%d 3:%d 4+:%d\n", step[0], step[1], step[2], step[3], step[4]);
        if (depth[0] + depth[1] + depth[2] + depth[3] + depth[4] + depth[5])
            P("  depth (asap frames; shown vblank minus the vblank before the commit): <=0:%d 1:%d 2:%d 3:%d 4:%d 5+:%d\n",
              depth[0], depth[1], depth[2], depth[3], depth[4], depth[5]);
        if (on + early + late)
            P("  against the planned vblank: on %d, early %d, late %d (late among injected late frames: %d of %d)\n",
              on, early, late, late_inj_late, late_inj);
        P("  presentedTime phase after the CV inNow grid: %+.1f us (circular mean)\n", phase_us);
        P_stats("|presentedTime - (CV grid + phase)|", res, nr, "us");
    }
    P_stats("presented minus commit", lat, nl, "ms");
    P_stats("presented handler minus presentedTime", arr, na, "us");
    P_stats("nextDrawable wait", nd, nn, "us");
    P_stats("encode+commit", cm, nc, "us");
    P_stats("scheduled handler minus commit", sch, ns, "us");
    P_stats("GPU time (GPUEnd - GPUStart)", gpu, ng, "us");
    free(lat); free(res); free(arr); free(nd); free(cm); free(gpu); free(sch);
}
