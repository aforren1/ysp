/* Section present: CAMetalLayer present timing in one window kind.
 * Runs: asap (one in flight, random phase), pipe (no wait), quant
 * (presentAtTime between two vblanks), ahead (targets seconds ahead), late
 * (held frames, some committed after their vblank), sleep (hold by sleeping,
 * then present at once), queue (several presents with targets in flight),
 * mindur (afterMinimumDuration). */
#import <Cocoa/Cocoa.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "mtl.h"

static bool want(const char* run, bool by_default) {
    if (!g_opt.runs) return by_default;
    const char* p = g_opt.runs;
    size_t n = strlen(run);
    while ((p = strstr(p, run)) != NULL) {
        if ((p == g_opt.runs || p[-1] == ',') && (p[n] == 0 || p[n] == ',')) return true;
        p += n;
    }
    return false;
}

static void sleep_until(int64_t t) {
    for (;;) {
        int64_t d = t - mach_ns_now();
        if (d <= 0) return;
        if (d > 300000) {
            struct timespec ts = { 0, (long)(d - 250000) };
            nanosleep(&ts, NULL);
        }
    }
}

static void wait_presented(Presenter* p, frame_rec* r) {
    int64_t end = mach_ns_now() + 1500000000;
    while (!__atomic_load_n(&r->complete, __ATOMIC_ACQUIRE) && mach_ns_now() < end)
        dispatch_semaphore_wait(p.presentedSem, dispatch_time(DISPATCH_TIME_NOW, 5000000));
}

/* The newest vblank at or before now, and the period, from CVDisplayLink. */
static int64_t last_vblank(Presenter* p, double* period) {
    int64_t v = 0;
    double per = 16666666.7;
    if (!cv_latest(p.cv, &v, &per)) v = mach_ns_now();
    int64_t now = mach_ns_now();
    double k = floor((double)(now - v) / per);
    *period = per;
    return v + (int64_t)(k * per);
}

static uint32_t rng = 12345;
static double frand(void) { rng = rng * 1664525u + 1013904223u; return (double)(rng >> 8) / 16777216.0; }

typedef struct run_ctx { Presenter* p; frame_rec* r; int n; int64_t t0, t1; } run_ctx;

static void finish(const char* name, run_ctx* c, FILE* csv) {
    [c->p drain];
    c->t1 = mach_ns_now();
    grid g = cv_grid(c->p.cv, c->t0 - 100000000, c->t1);
    P(" grid for %s: %d vblanks, period %.4f us, |residual| p99 %.2f us, missed %d\n", name, g.n, g.period / 1e3,
      g.res_p99, g.missed);
    analyze_run(name, c->r, c->n, &g);
    write_frames(csv, name, c->r, c->n, &g);
}

static void quant_table(frame_rec* r, int n, const grid* g, const int* leads, int nl, const double* fr, int nf) {
    double ph;
    grid gp = presented_grid(g, r, n, &ph);
    P("  presentAtTime(V + frac x period), shown vblank minus V: counts of -1 / 0 / +1 / +2 or more / not shown\n");
    P("  %-6s", "lead");
    for (int j = 0; j < nf; j++) P(" %17.2f", fr[j]);
    P("\n");
    for (int i = 0; i < nl; i++) {
        P("  %-6d", leads[i]);
        for (int j = 0; j < nf; j++) {
            int c[5] = { 0 };
            for (int k = 0; k < n; k++) {
                frame_rec* f = &r[k];
                if (f->lead != leads[i] || fabs(f->frac - fr[j]) > 1e-6) continue;
                if (!f->presented || !g->ok) { c[4]++; continue; }
                long long d = grid_round(&gp, f->presented) - grid_round(g, f->planned);
                c[d <= -1 ? 0 : d == 0 ? 1 : d == 1 ? 2 : 3]++;
            }
            char cell[32];
            snprintf(cell, sizeof cell, "%d/%d/%d/%d/%d", c[0], c[1], c[2], c[3], c[4]);
            P(" %17s", cell);
        }
        P("\n");
    }
}

int sec_present(void) {
    Presenter* p = [[Presenter alloc] initWithDevice:nil format:MTLPixelFormatBGRA8Unorm title:"ysp macOS probe: present"];
    if (!p) return 1;
    pump_for(1.0); /* CVDisplayLink fills its first second */
    FILE* csv = open_csv(tagged("present"), frames_header());
    const int N = g_opt.frames;
    __block int depth_seen = 1;

    run_worker(^{
        double per;
        /* asap: one frame in flight; a random wait of up to a period before each
         * frame spreads the commit over the whole refresh, so the depth shows
         * its dependence on the phase. */
        if (want("asap", true) && !quit_requested()) {
            run_ctx c = { p, calloc((size_t)N, sizeof(frame_rec)), N, mach_ns_now(), 0 };
            for (int i = 0; i < N && !quit_requested(); i++) {
                last_vblank(p, &per);
                sleep_until(mach_ns_now() + (int64_t)(frand() * per));
                c.r[i].frame = i;
                [p frame:&c.r[i] level:0.25f mindur:0];
                wait_presented(p, &c.r[i]);
            }
            finish("asap", &c, csv);
            /* the depth most asap frames showed, for the sleep run */
            grid g = cv_grid(p.cv, c.t0, c.t1);
            int h[8] = { 0 };
            for (int i = 0; i < N; i++)
                if (c.r[i].presented && g.ok) {
                    long long d = grid_round(&g, c.r[i].presented) - (long long)floor(grid_index(&g, c.r[i].t_commit));
                    if (d >= 0 && d < 8) h[d]++;
                }
            for (int d = 1; d < 8; d++) if (h[d] > h[depth_seen]) depth_seen = d;
            free(c.r);
        }
        /* pipe: no wait but nextDrawable's; the queue the layer allows */
        if (want("pipe", true) && !quit_requested()) {
            run_ctx c = { p, calloc((size_t)N, sizeof(frame_rec)), N, mach_ns_now(), 0 };
            for (int i = 0; i < N && !quit_requested(); i++) {
                c.r[i].frame = i;
                [p frame:&c.r[i] level:0.3f mindur:0];
            }
            finish("pipe", &c, csv);
            free(c.r);
        }
        /* quant: target V + frac x period with V `lead` vblanks after the last one */
        if (want("quant", true) && !quit_requested()) {
            static const int leads[] = { 1, 2, 3, 5 };
            static const double fr[] = { -0.5, -0.45, -0.25, 0.0, 0.25, 0.45, 0.5 };
            int n = 4 * 7 * g_opt.reps, k = 0;
            run_ctx c = { p, calloc((size_t)n, sizeof(frame_rec)), n, mach_ns_now(), 0 };
            for (int rep = 0; rep < g_opt.reps && !quit_requested(); rep++)
                for (int li = 0; li < 4; li++)
                    for (int fi = 0; fi < 7; fi++) {
                        frame_rec* f = &c.r[k];
                        int64_t v = last_vblank(p, &per);
                        f->frame = k++;
                        f->mode = PM_AT;
                        f->lead = leads[li];
                        f->frac = fr[fi];
                        f->planned = v + (int64_t)(leads[li] * per);
                        f->target = f->planned + (int64_t)(fr[fi] * per);
                        [p frame:f level:0.35f mindur:0];
                        wait_presented(p, f);
                    }
            c.n = k;
            finish("quant", &c, csv);
            grid g = cv_grid(p.cv, c.t0 - 100000000, c.t1);
            quant_table(c.r, c.n, &g, leads, 4, fr, 7);
            free(c.r);
        }
        /* ahead: how far ahead a target holds */
        if (want("ahead", false) && !quit_requested()) {
            static const double ahead[] = { 0.1, 0.25, 0.5, 1.0, 2.0, 5.0 };
            int n = 6 * 3, k = 0;
            run_ctx c = { p, calloc((size_t)n, sizeof(frame_rec)), n, mach_ns_now(), 0 };
            for (int rep = 0; rep < 3; rep++)
                for (int i = 0; i < 6 && !quit_requested(); i++) {
                    frame_rec* f = &c.r[k];
                    int64_t v = last_vblank(p, &per);
                    int lead = (int)floor(ahead[i] * 1e9 / per + 0.5);
                    f->frame = k++;
                    f->mode = PM_AT;
                    f->lead = lead;
                    f->planned = v + (int64_t)(lead * per);
                    f->target = f->planned;
                    [p frame:f level:0.4f mindur:0];
                    wait_presented(p, f);
                }
            c.n = k;
            {
                grid g = cv_grid(p.cv, c.t0 - 100000000, 0);
                double ph;
                grid gp = presented_grid(&g, c.r, c.n, &ph);
                for (int i = 0; i < c.n; i++) {
                    frame_rec* f = &c.r[i];
                    P("  ahead %d vblanks: shown %+lld vblanks from the target, %.3f s after the commit\n", f->lead,
                      f->presented && g.ok ? (long long)(grid_round(&gp, f->presented) - grid_round(&g, f->planned)) : 0LL,
                      f->presented ? (double)(f->presented - f->t_commit) / 1e9 : -1.0);
                }
            }
            finish("ahead", &c, csv);
            free(c.r);
        }
        /* late: frames held 2 vblanks; every 10th is committed 0.3 period after
         * its target vblank */
        if (want("late", true) && !quit_requested()) {
            run_ctx c = { p, calloc((size_t)N, sizeof(frame_rec)), N, mach_ns_now(), 0 };
            for (int i = 0; i < N && !quit_requested(); i++) {
                frame_rec* f = &c.r[i];
                int64_t v = last_vblank(p, &per);
                f->frame = i;
                f->mode = PM_AT;
                f->lead = 2;
                f->planned = v + (int64_t)(2 * per);
                f->target = f->planned;
                if (i % 10 == 9) { f->late_inj = 1; sleep_until(f->planned + (int64_t)(0.3 * per)); }
                [p frame:f level:0.45f mindur:0];
                wait_presented(p, f);
            }
            finish("late", &c, csv);
            free(c.r);
        }
        /* sleep: hold by sleeping until 1 ms after vblank (V - depth), then present at once */
        if (want("sleep", true) && !quit_requested()) {
            run_ctx c = { p, calloc((size_t)N, sizeof(frame_rec)), N, mach_ns_now(), 0 };
            P(" sleep run uses depth %d (from asap, else 1)\n", depth_seen);
            for (int i = 0; i < N && !quit_requested(); i++) {
                frame_rec* f = &c.r[i];
                int64_t v = last_vblank(p, &per);
                f->frame = i;
                f->lead = 3;
                f->planned = v + (int64_t)(3 * per);
                sleep_until(f->planned - (int64_t)(depth_seen * per) + 1000000);
                [p frame:f level:0.5f mindur:0];
                wait_presented(p, f);
            }
            finish("sleep", &c, csv);
            free(c.r);
        }
        /* queue: drawables - 1 presents in flight, each for its own vblank */
        if (want("queue", true) && !quit_requested()) {
            int K = g_opt.drawables - 1 > 0 ? g_opt.drawables - 1 : 1;
            int n = K * g_opt.reps, k = 0;
            run_ctx c = { p, calloc((size_t)n, sizeof(frame_rec)), n, mach_ns_now(), 0 };
            for (int rep = 0; rep < g_opt.reps && !quit_requested(); rep++) {
                int64_t v = last_vblank(p, &per);
                for (int j = 0; j < K; j++) {
                    frame_rec* f = &c.r[k];
                    f->frame = k++;
                    f->mode = PM_AT;
                    f->lead = 2 + j;
                    f->planned = v + (int64_t)((2 + j) * per);
                    f->target = f->planned;
                    [p frame:f level:0.55f mindur:0];
                }
                for (int j = k - K; j < k; j++) wait_presented(p, &c.r[j]);
            }
            c.n = k;
            finish("queue", &c, csv);
            free(c.r);
        }
        /* mindur: each frame at least 2 periods after the one before */
        if (want("mindur", true) && !quit_requested()) {
            run_ctx c = { p, calloc((size_t)N, sizeof(frame_rec)), N, mach_ns_now(), 0 };
            last_vblank(p, &per);
            for (int i = 0; i < N && !quit_requested(); i++) {
                c.r[i].frame = i;
                c.r[i].mode = PM_MINDUR;
                [p frame:&c.r[i] level:0.6f mindur:2.0 * per / 1e9];
            }
            finish("mindur", &c, csv);
            free(c.r);
        }
    });
    if (csv) fclose(csv);
    cv_report(p.cv, tagged("present_cv"));
    print_window(p.window);
    [p close];
    return 0;
}
