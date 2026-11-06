/* Section input (question 6): F19 key presses posted with CGEventPost at
 * known mach times, then read three ways: the NSEvent timestamp (a local
 * monitor sees each event before SDL), SDL's event timestamp moved onto mach
 * time through a correlation of SDL_GetTicksNS, and the arrival time in the
 * program. Posting needs the Accessibility or "post event" permission; the
 * probe checks it and does not ask for it. */
#import <Cocoa/Cocoa.h>
#import <ApplicationServices/ApplicationServices.h>
#include <math.h>
#include <stdlib.h>
#include <unistd.h>
#include "probe.h"

#define KEY_F19 0x50

typedef struct key_rec {
    int64_t post_a, post_b, cg_ts;      /* mach ns around CGEventPost; the event's own stamp */
    int64_t ns_ts, ns_arrive;           /* NSEvent timestamp, monitor call            */
    int64_t sdl_ts, sdl_arrive;         /* SDL timestamp (SDL ns), SDL_WaitEvent return */
} key_rec;

static key_rec* kr;
static volatile int n_posted;
static int n_ns, n_sdl;

static void sleep_until_ns(int64_t t) {
    while (mach_ns_now() < t) usleep(200);
}

static void run_pass(const char* name, bool to_pid, int keys) {
    memset(kr, 0, sizeof(key_rec) * (size_t)keys);
    n_posted = 0; n_ns = 0; n_sdl = 0;
    corr c = correlate(sdl_ticks_now, 64);
    __block volatile int done = 0;
    pid_t me = getpid();
    NSThread* poster = [[NSThread alloc] initWithBlock:^{
        uint32_t rng = 777;
        int64_t t = mach_ns_now() + 500000000;
        for (int i = 0; i < keys; i++) {
            rng = rng * 1664525u + 1013904223u;
            t += 40000000 + (int64_t)(rng >> 8) % 10000000; /* 40 to 50 ms apart, random phase */
            sleep_until_ns(t);
            CGEventRef down = CGEventCreateKeyboardEvent(NULL, KEY_F19, true);
            CGEventRef up = CGEventCreateKeyboardEvent(NULL, KEY_F19, false);
            kr[i].cg_ts = (int64_t)CGEventGetTimestamp(down);
            kr[i].post_a = mach_ns_now();
            if (to_pid) CGEventPostToPid(me, down); else CGEventPost(kCGHIDEventTap, down);
            kr[i].post_b = mach_ns_now();
            __atomic_store_n(&n_posted, i + 1, __ATOMIC_RELEASE);
            usleep(15000);
            if (to_pid) CGEventPostToPid(me, up); else CGEventPost(kCGHIDEventTap, up);
            CFRelease(down);
            CFRelease(up);
        }
        usleep(300000);
        done = 1;
    }];
    [poster start];
    while (!done) {
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, 5)) {
            int64_t a = mach_ns_now();
            if (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_F19 && !e.key.repeat && n_sdl < keys) {
                kr[n_sdl].sdl_ts = (int64_t)e.key.timestamp;
                kr[n_sdl].sdl_arrive = a;
                n_sdl++;
            }
            if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE) break;
        }
    }
    corr c2 = correlate(sdl_ticks_now, 64);
    int posted = n_posted;
    P(" pass %s: posted %d, NSEvent monitor saw %d, SDL saw %d; SDL tick correlation bracket %lld ns, "
      "SDL - mach rate over the pass %+.3f ppm\n",
      name, posted, n_ns, n_sdl, (long long)c.width,
      1e6 * ((double)((c2.other - c.other) - (c2.mach - c.mach)) / (double)(c2.mach - c.mach)));
    int m = n_ns < posted ? n_ns : posted;
    int ms = n_sdl < posted ? n_sdl : posted;
    if (m == 0 && ms == 0) { P("  no key reached the program\n"); return; }
    double *a = calloc((size_t)keys, sizeof(double)), *b = calloc((size_t)keys, sizeof(double)),
           *d = calloc((size_t)keys, sizeof(double)), *e2 = calloc((size_t)keys, sizeof(double)),
           *f = calloc((size_t)keys, sizeof(double)), *g = calloc((size_t)keys, sizeof(double)),
           *h = calloc((size_t)keys, sizeof(double));
    for (int i = 0; i < m; i++) {
        a[i] = (double)(kr[i].ns_ts - kr[i].post_a) / 1000.0;
        b[i] = (double)(kr[i].ns_arrive - kr[i].post_a) / 1000.0;
        h[i] = (double)(kr[i].cg_ts - kr[i].post_a) / 1000.0;
    }
    for (int i = 0; i < ms; i++) {
        int64_t sdl_mach = kr[i].sdl_ts + (c.mach - c.other);
        d[i] = (double)(sdl_mach - kr[i].post_a) / 1000.0;
        e2[i] = (double)(kr[i].sdl_arrive - kr[i].post_a) / 1000.0;
        if (i < m) f[i] = (double)(sdl_mach - kr[i].ns_ts) / 1000.0;
    }
    for (int i = 0; i < posted; i++) g[i] = (double)(kr[i].post_b - kr[i].post_a) / 1000.0;
    P_stats("CGEventPost call", g, posted, "us");
    P_stats("CGEventGetTimestamp minus post (as ns)", h, m, "us");
    P_stats("NSEvent.timestamp minus post", a, m, "us");
    P_stats("NSEvent monitor call minus post", b, m, "us");
    P_stats("SDL timestamp (to mach) minus post", d, ms, "us");
    P_stats("SDL event read minus post", e2, ms, "us");
    P_stats("SDL timestamp minus NSEvent.timestamp", f, ms < m ? ms : m, "us");
    /* granularity: the sub-millisecond parts of the stamps */
    int sub_ns = 0, sub_sdl = 0;
    for (int i = 0; i < m; i++) if (kr[i].ns_ts % 1000000) sub_ns++;
    for (int i = 0; i < ms; i++) if (kr[i].sdl_ts % 1000000) sub_sdl++;
    P("  stamps with a sub-millisecond part: NSEvent %d of %d, SDL %d of %d\n", sub_ns, m, sub_sdl, ms);
    FILE* csv = open_csv(tagged(to_pid ? "input_pid" : "input_hid"),
                         "i,post_a,post_b,cg_ts,ns_ts,ns_arrive,sdl_ts,sdl_arrive,sdl_mach_offset");
    for (int i = 0; csv && i < posted; i++)
        fprintf(csv, "%d,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld\n", i, (long long)kr[i].post_a, (long long)kr[i].post_b,
                (long long)kr[i].cg_ts, (long long)kr[i].ns_ts, (long long)kr[i].ns_arrive, (long long)kr[i].sdl_ts,
                (long long)kr[i].sdl_arrive, (long long)(c.mach - c.other));
    if (csv) fclose(csv);
    free(a); free(b); free(d); free(e2); free(f); free(g); free(h);
}

int sec_input(void) {
    P("Permissions: AXIsProcessTrusted %d, CGPreflightPostEventAccess %d, CGPreflightListenEventAccess %d\n",
      AXIsProcessTrusted(), CGPreflightPostEventAccess(), CGPreflightListenEventAccess());
    SDL_Window* w = open_window(false, "ysp macOS probe: input");
    if (!w) return 1;
    int keys = g_opt.keys;
    kr = calloc((size_t)keys, sizeof(key_rec));
    id mon = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown handler:^NSEvent*(NSEvent* e) {
        if (e.keyCode == KEY_F19 && !e.ARepeat && n_ns < keys) {
            kr[n_ns].ns_arrive = mach_ns_now();
            kr[n_ns].ns_ts = secs_to_ns(e.timestamp);
            n_ns++;
        }
        return e;
    }];
    P("  key window %d, app active %d\n",
      ((__bridge NSWindow*)SDL_GetPointerProperty(SDL_GetWindowProperties(w), SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, NULL)).keyWindow,
      NSApp.active);
    print_session();
    /* A HID-level key goes to whatever has the keyboard focus: with the
     * screen locked that is the login window, with another app active it is
     * that app. Post there only when this program is active. */
    if (!session_locked() && NSApp.active) run_pass("CGEventPost(kCGHIDEventTap)", false, keys);
    else P(" pass CGEventPost(kCGHIDEventTap): skipped, the program is not the active app or the screen is locked\n");
    run_pass("CGEventPostToPid(self)", true, keys);
    [NSEvent removeMonitor:mon];
    free(kr);
    close_window(w);
    return 0;
}
