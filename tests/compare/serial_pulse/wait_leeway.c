/* wait_leeway.c - how late a wait on a normal thread wakes on macOS.
 *
 * pulse_bench on macOS (docs/serial.md, macOS with the Teensy 4.0 echo
 * board) found host-timed widths 25 to 50 % of the width too long whenever
 * the caller's own thread waited. This program measures the wait alone,
 * without a port: 100 waits each of 1, 2, 5 and 10 ms, on the main thread
 * at its default scheduling, and prints the wake minus the deadline.
 *
 *   yrt_sleep_until, spin 0          ysp/rt.h's relative nanosleep() loop
 *   yrt_sleep_until, spin 200 us     the same with YRT_DEFAULT_SPIN_NS, the
 *                                    wait of yser_pulse() and of pulse_bench
 *   mach_wait_until                  the Mach absolute wait
 *   kevent NOTE_CRITICAL             an EVFILT_TIMER, absolute in mach time,
 *                                    flagged critical (minimal coalescing)
 *   nanosleep, USER_INTERACTIVE      after pthread_set_qos_class_self_np()
 *
 * Build and run (macOS only):
 *   cc -O2 -Wall -I../../../include -o wait_leeway wait_leeway.c -lpthread
 *   ./wait_leeway
 *
 * Public domain / MIT-0, as the rest of ysp. */
#define YSP_RT_IMPLEMENTATION
#include "ysp/rt.h"
#if !defined(__APPLE__)
#error "wait_leeway.c measures a macOS behavior"
#endif
#include <mach/mach_time.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/event.h>

enum { K_SLEEP0, K_SLEEP_SPIN, K_MACH, K_KEVENT, K_COUNT };

static int kq = -1;
static mach_timebase_info_data_t tb;

static int cmp(const void* a, const void* b) {
    int64_t x = *(const int64_t*)a, y = *(const int64_t*)b;
    return (x > y) - (x < y);
}

/* yrt_now_ns() is mach_absolute_time() scaled by the timebase, so a deadline
 * on that clock converts back to mach units with the inverse ratio. */
static uint64_t to_mach(int64_t ns) { return (uint64_t)ns / tb.numer * tb.denom + (uint64_t)ns % tb.numer * tb.denom / tb.numer; }

static void wait_kind(int kind, int64_t until) {
    switch (kind) {
    case K_SLEEP0: (void)yrt_sleep_until(until, 0); break;
    case K_SLEEP_SPIN: (void)yrt_sleep_until(until, YRT_DEFAULT_SPIN_NS); break;
    case K_MACH: mach_wait_until(to_mach(until)); break;
    default: {
        struct kevent ev, out;
        EV_SET(&ev, 1, EVFILT_TIMER, EV_ADD | EV_ONESHOT, NOTE_CRITICAL | NOTE_ABSOLUTE | NOTE_MACHTIME,
               (int64_t)to_mach(until), NULL);
        (void)kevent(kq, &ev, 1, &out, 1, NULL);
    }
    }
}

static void run(const char* tag, int kind) {
    static const int ms[] = { 1, 2, 5, 10 };
    for (int w = 0; w < 4; w++) {
        enum { N = 100 };
        int64_t late[N];
        for (int i = 0; i < N; i++) {
            int64_t t = yrt_now_ns() + (int64_t)ms[w] * 1000000;
            wait_kind(kind, t);
            late[i] = yrt_now_ns() - t;
            yrt_spin_until(yrt_now_ns() + 300000);   /* not in phase with anything */
        }
        qsort(late, N, sizeof late[0], cmp);
        printf("%-30s %2d ms: late min %7.1f, median %7.1f, p90 %7.1f, max %7.1f us\n", tag, ms[w],
               late[0] / 1e3, late[N / 2] / 1e3, late[N * 9 / 10] / 1e3, late[N - 1] / 1e3);
        fflush(stdout);
    }
}

int main(void) {
    qos_class_t q;
    int rel;
    mach_timebase_info(&tb);
    kq = kqueue();
    if (kq < 0) { perror("kqueue"); return 1; }
    pthread_get_qos_class_np(pthread_self(), &q, &rel);
    printf("main thread QoS class 0x%x, timebase %u/%u\n", (unsigned)q, tb.numer, tb.denom);
    run("yrt_sleep_until, spin 0", K_SLEEP0);
    run("yrt_sleep_until, spin 200 us", K_SLEEP_SPIN);
    run("mach_wait_until", K_MACH);
    run("kevent NOTE_CRITICAL", K_KEVENT);
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    run("nanosleep, USER_INTERACTIVE", K_SLEEP0);
    return 0;
}
