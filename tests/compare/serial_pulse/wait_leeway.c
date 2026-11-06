/* wait_leeway.c - how late a timed wait wakes on macOS, by wait kind and by
 * the thread it runs on.
 *
 * pulse_bench on macOS (docs/serial.md, macOS with the Teensy 4.0 echo
 * board) found host-timed widths a quarter to a half of their sleep too long
 * whenever the caller's own thread waited: Darwin coalesces the timers of
 * unprivileged threads. This program measures the waits alone, without a
 * port, so the fix in ysp/rt.h (v0.10.1) can be chosen and checked.
 *
 * Waits, each the coarse part only (no spin), absolute deadlines on
 * yrt_now_ns()'s clock:
 *   nanosleep         the relative nanosleep() loop of ysp/rt.h 0.10.0
 *   kevent_critical   a one-shot EVFILT_TIMER, NOTE_CRITICAL, absolute in
 *                     mach time (ysp/rt.h 0.10.1)
 *   kevent_leeway0    the same timer with NOTE_LEEWAY and a leeway of 0
 *                     (kevent64), not flagged critical
 *   latency_tier0     nanosleep() after THREAD_LATENCY_QOS_POLICY tier 0 on
 *                     the thread (restored afterwards)
 *   yrt_spin0         yrt_sleep_until(t, 0) of the header this is built with
 *   yrt_spin_default  yrt_sleep_until(t, YRT_DEFAULT_SPIN_NS): what a caller
 *                     of the header gets
 * Threads:
 *   main              the process's main thread; with WAIT_SDL, the main
 *                     thread of a Cocoa app with an SDL window open, as
 *                     ysp/screen.h's presenter runs
 *   ordinary          a new pthread at the defaults
 *   time_constraint   a new pthread after yrt_thread_elevate() (Mach
 *                     THREAD_TIME_CONSTRAINT_POLICY without privilege)
 * With WAIT_SDL the window stays open, and the main thread pumps its events,
 * while the other two threads measure.
 *
 * Output: one line per thread, wait and length: min, median, p90, p99 and
 * max lateness in microseconds, and the count over 1 ms; then every wait
 * more than 1 ms late, with its index, so an outlier can be placed.
 *
 * Build and run (macOS only), from this folder:
 *   cc -O2 -Wall -I../../../include -o wait_leeway wait_leeway.c -lpthread
 *   ./wait_leeway [n]                    (n waits per cell, default 200)
 * With an SDL window on the main thread (SDL3 built by tools/vendor_vis.py):
 *   cc -O2 -Wall -DWAIT_SDL -I../../../include -I<sdl3>/include -o wait_leeway_sdl \
 *      wait_leeway.c <sdl3>/lib/libSDL3.a <the frameworks of tools/vendor_vis.py SDL_MAC_LINK>
 *
 * Public domain / MIT-0, as the rest of ysp. */
#define YSP_RT_IMPLEMENTATION
#include "ysp/rt.h"
#if !defined(__APPLE__)
#error "wait_leeway.c measures a macOS behavior"
#endif
#include <errno.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event.h>
#include <time.h>
#include <unistd.h>
#if defined(WAIT_SDL)
#include <SDL3/SDL.h>
#endif

enum { W_NANOSLEEP, W_CRITICAL, W_LEEWAY0, W_TIER0, W_YRT0, W_YRT_SPIN, W_COUNT };
static const char* const wait_name[W_COUNT] = {
    "nanosleep", "kevent_critical", "kevent_leeway0", "latency_tier0", "yrt_spin0", "yrt_spin_default"
};
static const int ms[] = { 1, 2, 5, 10 };
enum { NW = 4, NMAX = 2000 };

static int g_n = 200;
static mach_timebase_info_data_t tb;
static volatile int g_done;

static int cmp(const void* a, const void* b) {
    int64_t x = *(const int64_t*)a, y = *(const int64_t*)b;
    return (x > y) - (x < y);
}

/* yrt_now_ns() is mach_absolute_time() scaled by the timebase, so a deadline
 * on that clock converts back to mach units with the inverse ratio. */
static uint64_t to_mach(int64_t ns) {
    return (uint64_t)ns / tb.numer * tb.denom + (uint64_t)ns % tb.numer * tb.denom / tb.numer;
}

static void set_tier0(int on) {
    thread_latency_qos_policy_data_t p;
    p.thread_latency_qos_tier = on ? LATENCY_QOS_TIER_0 : LATENCY_QOS_TIER_UNSPECIFIED;
    mach_port_t t = mach_thread_self();
    thread_policy_set(t, THREAD_LATENCY_QOS_POLICY, (thread_policy_t)&p, THREAD_LATENCY_QOS_POLICY_COUNT);
    mach_port_deallocate(mach_task_self(), t);
}

static void wait_until(int kind, int kq, int64_t until) {
    switch (kind) {
    case W_NANOSLEEP:
    case W_TIER0:
        for (;;) {
            int64_t now = yrt_now_ns();
            struct timespec rel;
            if (now >= until) return;
            rel.tv_sec = (time_t)((until - now) / 1000000000);
            rel.tv_nsec = (long)((until - now) % 1000000000);
            if (nanosleep(&rel, NULL) == 0 || errno != EINTR) return;
        }
    case W_CRITICAL: {
        struct kevent ev, out;
        EV_SET(&ev, 1, EVFILT_TIMER, EV_ADD | EV_ONESHOT, NOTE_CRITICAL | NOTE_ABSOLUTE | NOTE_MACHTIME,
               (int64_t)to_mach(until), NULL);
        while (kevent(kq, &ev, 1, &out, 1, NULL) < 0 && errno == EINTR) { }
        return;
    }
    case W_LEEWAY0: {
        struct kevent64_s ev, out;
        EV_SET64(&ev, 2, EVFILT_TIMER, EV_ADD | EV_ONESHOT, NOTE_LEEWAY | NOTE_ABSOLUTE | NOTE_MACHTIME,
                 (int64_t)to_mach(until), 0, 0, 0);
        int r;
        while ((r = kevent64(kq, &ev, 1, &out, 1, 0, NULL)) < 0 && errno == EINTR) { }
        if (r < 0 || (out.flags & EV_ERROR)) {
            fprintf(stderr, "kevent64: %s\n", strerror(r < 0 ? errno : (int)out.data));
            exit(1);
        }
        return;
    }
    case W_YRT0: (void)yrt_sleep_until(until, 0); return;
    default: (void)yrt_sleep_until(until, YRT_DEFAULT_SPIN_NS); return;
    }
}

static void pump(void) {
#if defined(WAIT_SDL)
    if (pthread_main_np()) SDL_PumpEvents();
#endif
}

static void run_thread(const char* tag) {
    static int64_t late[NMAX];
    int kq = kqueue(), kq64 = kqueue();   /* a kqueue keeps the event format of its first call */
    if (kq < 0 || kq64 < 0) { perror("kqueue"); exit(1); }
    for (int k = 0; k < W_COUNT; k++) {
        if (k == W_TIER0) set_tier0(1);
        for (int w = 0; w < NW; w++) {
            int over = 0;
            for (int i = 0; i < g_n; i++) {
                int64_t t = yrt_now_ns() + (int64_t)ms[w] * 1000000;
                wait_until(k, k == W_LEEWAY0 ? kq64 : kq, t);
                late[i] = yrt_now_ns() - t;
                if (late[i] > 1000000) {
                    over++;
                    printf("  outlier %-15s %-16s %2d ms wait %3d: %.1f us late\n", tag, wait_name[k], ms[w], i,
                           late[i] / 1e3);
                }
                pump();
                yrt_spin_until(yrt_now_ns() + 300000);   /* out of phase with anything periodic */
            }
            qsort(late, (size_t)g_n, sizeof late[0], cmp);
            printf("%-15s %-16s %2d ms: min %8.1f  med %8.1f  p90 %8.1f  p99 %8.1f  max %8.1f  >1ms %d\n", tag,
                   wait_name[k], ms[w], late[0] / 1e3, late[g_n / 2] / 1e3, late[g_n * 9 / 10] / 1e3,
                   late[(g_n * 99 + 99) / 100 - 1] / 1e3, late[g_n - 1] / 1e3, over);
            fflush(stdout);
        }
        if (k == W_TIER0) set_tier0(0);
    }
    close(kq); close(kq64);
}

static void* ordinary_main(void* arg) {
    (void)arg;
    run_thread("ordinary");
    g_done = 1;
    return NULL;
}

static void* tc_main(void* arg) {
    yrt_sched_deadline s;
    (void)arg;
    memset(&s, 0, sizeof s);
    yrt_policy p = yrt_thread_elevate(&s);
    printf("time_constraint thread: policy %s\n", yrt_policy_name(p));
    run_thread("time_constraint");
    g_done = 1;
    return NULL;
}

static void run_on(void* (*fn)(void*)) {
    pthread_t th;
    g_done = 0;
    pthread_create(&th, NULL, fn, NULL);
    while (!g_done) {
#if defined(WAIT_SDL)
        SDL_Event e;
        (void)SDL_WaitEventTimeout(&e, 20);
#else
        usleep(20000);
#endif
    }
    pthread_join(th, NULL);
}

int main(int argc, char** argv) {
    if (argc > 1) g_n = atoi(argv[1]);
    if (g_n < 10 || g_n > NMAX) g_n = 200;
    mach_timebase_info(&tb);
    printf("ysp/rt.h %d.%d.%d, %d waits per cell, timebase %u/%u, lateness in us\n", YRT_VERSION_MAJOR,
           YRT_VERSION_MINOR, YRT_VERSION_PATCH, g_n, tb.numer, tb.denom);
#if defined(WAIT_SDL)
    if (!SDL_Init(SDL_INIT_VIDEO)) { fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1; }
    SDL_Window* win = SDL_CreateWindow("wait_leeway", 320, 200, 0);
    if (!win) { fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); return 1; }
    SDL_RaiseWindow(win);
    for (int i = 0; i < 50; i++) { SDL_PumpEvents(); usleep(10000); }
    run_thread("main_sdl");
#else
    run_thread("main");
#endif
    run_on(ordinary_main);
    run_on(tc_main);
#if defined(WAIT_SDL)
    SDL_DestroyWindow(win);
    SDL_Quit();
#endif
    return 0;
}
