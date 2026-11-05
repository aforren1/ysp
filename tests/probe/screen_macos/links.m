/* Section links: the vblank sources against each other. CVDisplayLink (its
 * own thread), CADisplayLink from the window's NSView (main run loop, macOS
 * 14 and later) and MTLDrawable.presentedTime of frames presented as soon as
 * possible, one in flight, for g_opt.seconds. */
#import <Cocoa/Cocoa.h>
#include <math.h>
#include <stdlib.h>
#include "mtl.h"

typedef struct ca_rec { int64_t ts, target, arrive; double duration; } ca_rec;
static ca_rec* ca_r;
static int ca_n, ca_cap;

@interface LinkTarget : NSObject
- (void)tick:(CADisplayLink*)l;
@end
@implementation LinkTarget
- (void)tick:(CADisplayLink*)l {
    int64_t a = mach_ns_now();
    if (ca_n < ca_cap) {
        ca_r[ca_n].ts = secs_to_ns(l.timestamp);
        ca_r[ca_n].target = secs_to_ns(l.targetTimestamp);
        ca_r[ca_n].duration = l.duration;
        ca_r[ca_n].arrive = a;
        ca_n++;
    }
}
@end

static int64_t nearest_cv(const cv_link* c, int64_t t, int* hint) {
    int n = c->n, i = *hint;
    while (i + 1 < n && c->rec[i + 1].now_host <= t) i++;
    *hint = i;
    int64_t a = c->rec[i].now_host;
    int64_t b = i + 1 < n ? c->rec[i + 1].now_host : a;
    return llabs(t - a) <= llabs(b - t) ? a : b;
}

int sec_links(void) {
    Presenter* p = [[Presenter alloc] initWithDevice:nil format:MTLPixelFormatBGRA8Unorm title:"ysp macOS probe: links"];
    if (!p) return 1;
    ca_cap = (int)(g_opt.seconds * 400) + 1000;
    ca_r = calloc((size_t)ca_cap, sizeof(ca_rec));
    LinkTarget* lt = [LinkTarget new];
    NSView* view = (__bridge NSView*)p.view;
    CADisplayLink* dl = nil;
    if (@available(macOS 14.0, *)) {
        dl = [view displayLinkWithTarget:lt selector:@selector(tick:)];
        [dl addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];
    } else {
        P("  CADisplayLink needs macOS 14\n");
    }
    int nf = (int)(g_opt.seconds * 250) + 100;
    frame_rec* r = frames_new(nf);
    __block int n = 0;
    int64_t t0 = mach_ns_now();
    int64_t end = t0 + secs_to_ns(g_opt.seconds);
    run_worker(^{
        while (mach_ns_now() < end && n < nf && !quit_requested()) {
            frame_rec* f = &r[n];
            f->frame = n++;
            [p frame:f level:0.2f mindur:0];
            int64_t lim = mach_ns_now() + 1000000000;
            while (!__atomic_load_n(&f->complete, __ATOMIC_ACQUIRE) && mach_ns_now() < lim)
                dispatch_semaphore_wait(p.presentedSem, dispatch_time(DISPATCH_TIME_NOW, 5000000));
        }
        [p drain];
    });
    [dl invalidate];
    int64_t t1 = mach_ns_now();

    P("Sources over %.1f s\n", (double)(t1 - t0) / 1e9);
    cv_report(p.cv, tagged("links_cv"));
    grid gcv = cv_grid(p.cv, t0, t1);

    /* CADisplayLink */
    if (ca_n > 8) {
        int64_t* ts = malloc(sizeof(int64_t) * (size_t)ca_n);
        double *arr = malloc(sizeof(double) * (size_t)ca_n * 4), *tgt = arr + ca_n, *dcv = arr + 2 * ca_n,
               *dur = arr + 3 * ca_n;
        int hint = 0;
        for (int i = 0; i < ca_n; i++) {
            ts[i] = ca_r[i].ts;
            arr[i] = (double)(ca_r[i].arrive - ca_r[i].ts) / 1000.0;
            tgt[i] = (double)(ca_r[i].target - ca_r[i].ts) / 1e6;
            dcv[i] = (double)(ca_r[i].ts - nearest_cv(p.cv, ca_r[i].ts, &hint)) / 1000.0;
            dur[i] = ca_r[i].duration * 1e3;
        }
        grid g = grid_fit(ts, ca_n, gcv.ok ? gcv.period : 16666666.7);
        P("  CADisplayLink (NSView): %d callbacks, fit %.6f ms (%.4f Hz), missed %d, |residual| p50 %.2f p99 %.2f max %.2f us\n",
          ca_n, g.period / 1e6, 1e9 / g.period, g.missed, g.res_p50, g.res_p99, g.res_max);
        P_stats("CA callback minus timestamp", arr, ca_n, "us");
        P_stats("CA targetTimestamp minus timestamp", tgt, ca_n, "ms");
        P_stats("CA duration", dur, ca_n, "ms");
        P_stats("CA timestamp minus nearest CV inNow", dcv, ca_n, "us");
        if (gcv.ok && g.ok)
            P("  grid phase: CA t0 minus CV grid, %.3f us; period difference %.3f ppm\n",
              grid_residual_us(&gcv, (int64_t)g.t0), 1e6 * (g.period - gcv.period) / gcv.period);
        FILE* csv = open_csv(tagged("links_ca"), "i,timestamp_ns,target_ns,duration_s,arrive_ns");
        for (int i = 0; csv && i < ca_n; i++)
            fprintf(csv, "%d,%lld,%lld,%.9f,%lld\n", i, (long long)ca_r[i].ts, (long long)ca_r[i].target,
                    ca_r[i].duration, (long long)ca_r[i].arrive);
        if (csv) fclose(csv);
        free(ts);
        free(arr);
    } else {
        P("  CADisplayLink: %d callbacks\n", ca_n);
    }

    /* presentedTime of the frames */
    analyze_run("links_asap", r, n, &gcv);
    {
        int m = 0;
        int64_t* pt = malloc(sizeof(int64_t) * (size_t)(n + 1));
        for (int i = 0; i < n; i++) if (r[i].presented) pt[m++] = r[i].presented;
        grid g = grid_fit(pt, m, gcv.ok ? gcv.period : 16666666.7);
        P("  presentedTime alone: %d times, fit %.6f ms, missed %d, |residual| p50 %.2f p99 %.2f max %.2f us; "
          "phase against the CV grid %.3f us\n",
          m, g.period / 1e6, g.missed, g.res_p50, g.res_p99, g.res_max, gcv.ok && g.ok ? grid_residual_us(&gcv, (int64_t)g.t0) : 0.0);
        free(pt);
    }
    FILE* csv = open_csv(tagged("links_frames"), frames_header());
    write_frames(csv, "links_asap", r, n, &gcv);
    if (csv) fclose(csv);
    free(ca_r);
    [p close];
    return 0;
}
