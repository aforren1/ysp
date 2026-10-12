/* serial_loopback.c - ysp/serial.h against a real port pair.
 *
 * Not a compile check and not run by ctest: it needs two device names that
 * are wired to each other, which CI does not have. Build it with
 * -DYSP_BUILD_LOOPBACK=ON.
 *
 *     serial_loopback <write_device> <read_device>
 *
 * Give it either
 *   - a virtual pair:      socat -d -d pty,raw,echo=0 pty,raw,echo=0
 *                          (or tests/loopback/pty_pair.py without socat)
 *                          then the two /dev/pts/N names it prints
 *                          (com0com on Windows: CNCA0 CNCB0), or
 *   - a physical loopback: the same port name twice, with TX tied to RX on
 *                          the connector.
 *
 * Exit code 0 if every check passed, 1 otherwise, 2 for a usage or setup
 * error. Run it under ThreadSanitizer too (-fsanitize=thread): the last phase
 * puts a reader thread and a writer thread on one handle, which is the whole
 * threading promise of the library.
 */
#define YSP_SERIAL_IMPLEMENTATION
#include "ysp/serial.h"   /* first include: it sets _DEFAULT_SOURCE for glibc,
                           * and it pulls in ysp/rt.h, which must be beside it */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
    #include <windows.h>
#else
    #include <pthread.h>
    #include <stdatomic.h>
    #include <time.h>
#endif

/* A short wait that is meant to expire, and the slack a scheduler adds to it.
 * Tight enough to catch a timeout that is ignored or halved, loose enough for
 * a loaded machine and for the 15.6 ms Windows tick. */
#define LB_TIMEOUT_MS 100
#define LB_SLACK_LO    95
#define LB_SLACK_HI   160

/* The checks below are written in microseconds on the library's clock. */
static uint64_t lb_now_us(void) { return (uint64_t)yrt_now_ns() / 1000u; }

/* --- platform glue (threads, a stop flag, a coarse sleep) ---------------- */

#if defined(_WIN32)
typedef HANDLE  lb_thread;
static volatile LONG lb_stop_flag;
static void lb_stop_set(void)  { InterlockedExchange(&lb_stop_flag, 1); }
static int  lb_stop_get(void)  { return (int)InterlockedCompareExchange(&lb_stop_flag, 0, 0); }
static void lb_sleep_ms(unsigned ms) { Sleep(ms); }
#else
typedef pthread_t lb_thread;
static atomic_int lb_stop_flag;
static void lb_stop_set(void)  { atomic_store(&lb_stop_flag, 1); }
static int  lb_stop_get(void)  { return atomic_load(&lb_stop_flag); }
static void lb_sleep_ms(unsigned ms) {
    struct timespec ts;
    ts.tv_sec  = ms / 1000u;
    ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
    while (nanosleep(&ts, &ts) != 0) { }
}
#endif

static void lb_body_blocked_read(void);
static void lb_body_hammer_read(void);
static void lb_body_hammer_write(void);

#if defined(_WIN32)
static DWORD WINAPI lb_entry_blocked_read(LPVOID a) { (void)a; lb_body_blocked_read(); return 0; }
static DWORD WINAPI lb_entry_hammer_read(LPVOID a)  { (void)a; lb_body_hammer_read();  return 0; }
static DWORD WINAPI lb_entry_hammer_write(LPVOID a) { (void)a; lb_body_hammer_write(); return 0; }
static lb_thread lb_start(LPTHREAD_START_ROUTINE fn) {
    lb_thread t = CreateThread(NULL, 0, fn, NULL, 0, NULL);
    if (!t) { fprintf(stderr, "CreateThread failed\n"); exit(2); }
    return t;
}
static void lb_join(lb_thread t) { WaitForSingleObject(t, INFINITE); CloseHandle(t); }
#define LB_START(name) lb_start(lb_entry_##name)
#else
static void* lb_entry_blocked_read(void* a) { (void)a; lb_body_blocked_read(); return NULL; }
static void* lb_entry_hammer_read(void* a)  { (void)a; lb_body_hammer_read();  return NULL; }
static void* lb_entry_hammer_write(void* a) { (void)a; lb_body_hammer_write(); return NULL; }
static lb_thread lb_start(void* (*fn)(void*)) {
    lb_thread t;
    if (pthread_create(&t, NULL, fn, NULL) != 0) { fprintf(stderr, "pthread_create failed\n"); exit(2); }
    return t;
}
static void lb_join(lb_thread t) { pthread_join(t, NULL); }
#define LB_START(name) lb_start(lb_entry_##name)
#endif

/* --- checks -------------------------------------------------------------- */

static int lb_failures;

static void lb_check(int ok, const char* what, const char* fmt, ...) {
    va_list ap;
    printf("%-5s %-34s ", ok ? "pass" : "FAIL", what);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
    if (!ok) lb_failures++;
}

static void lb_info(const char* what, const char* fmt, ...) {
    va_list ap;
    printf("%-5s %-34s ", "info", what);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}

/* --- shared state for the thread bodies ---------------------------------- */

static yser_port* lb_w;          /* handle bytes are written to */
static yser_port* lb_r;          /* handle bytes are read from  */
static int        lb_read_rc;
static uint64_t   lb_read_done_us;
static long       lb_hammer_written, lb_hammer_read, lb_hammer_short, lb_hammer_err;
static long       lb_hammer_pulsed;

static void lb_body_blocked_read(void) {
    uint8_t buf[8];
    lb_read_rc      = yser_read(lb_r, buf, 1, YSER_TIMEOUT_INFINITE, YSER_READ_ANY);
    lb_read_done_us = lb_now_us();
}

/* The threading contract under test: these two run at the same time on ONE
 * handle (lb_w), one in the reader role and one in the writer role. */
static void lb_body_hammer_read(void) {
    uint8_t buf[64];
    while (!lb_stop_get()) {
        int n = yser_read(lb_w, buf, (int)sizeof(buf), YRT_MS(20), YSER_READ_ANY);
        if (n > 0) lb_hammer_read += n;
        else if (n < 0 && n != YSER_ERR_INTERRUPTED) { lb_hammer_err++; break; }
    }
}

static void lb_body_hammer_write(void) {
    long i = 0;
    while (!lb_stop_get()) {
        /* Every eighth write is an async pulse, so the worker thread is
         * writing its trailing byte into the same stream while this thread
         * keeps writing and the reader keeps reading: three threads, one
         * handle, which is the arrangement the lock exists for. */
        int n = (++i % 8 == 0) ? yser_pulse_async(lb_w, 0xA5, 0x00, YRT_US(300))
                               : yser_write_byte(lb_w, 0xA5);
        if (n == 1) { lb_hammer_written++; if (i % 8 == 0) lb_hammer_pulsed++; }
        else if (n == 0) lb_hammer_short++;   /* write timeout: the peer is not draining */
        else if (n < 0) { lb_hammer_err++; break; }
    }
}

/* --- helpers ------------------------------------------------------------- */

/* Read both ends until the transport goes quiet, then purge. A purge alone is
 * not enough after a phase that stuffed the pipe: it empties the local driver
 * buffer, but a relay like socat can still hold tens of kilobytes that would
 * arrive in the middle of the next check and make a blocking read return
 * data. */
static void lb_quiesce(bool same) {
    uint8_t sink[1024];
    uint64_t deadline = lb_now_us() + 5000000ull;
    uint64_t last_byte_us = lb_now_us();
    while (lb_now_us() < deadline) {
        int got = 0, k;
        k = yser_read(lb_w, sink, (int)sizeof(sink), 0, YSER_READ_ANY);
        if (k > 0) got += k;
        if (!same) {
            k = yser_read(lb_r, sink, (int)sizeof(sink), 0, YSER_READ_ANY);
            if (k > 0) got += k;
        }
        if (got > 0) { last_byte_us = lb_now_us(); continue; }
        if (lb_now_us() - last_byte_us > 200000ull) break;
        lb_sleep_ms(5);
    }
    yser_purge(lb_w, YSER_PURGE_RX | YSER_PURGE_TX);
    if (!same) yser_purge(lb_r, YSER_PURGE_RX | YSER_PURGE_TX);
}

/* Wait until the driver reports at least `want` bytes, or give up. Returns
 * what yser_available() last said. */
static int lb_wait_available(yser_port* p, int want, unsigned limit_ms) {
    uint64_t end = lb_now_us() + (uint64_t)limit_ms * 1000ull;
    int n = 0;
    for (;;) {
        n = yser_available(p);
        if (n < 0 || n >= want || lb_now_us() >= end) return n;
        lb_sleep_ms(2);
    }
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <write_device> <read_device>\n"
                        "       (two names from a socat pty pair, or the same "
                        "name twice for a wired loopback)\n", argv[0]);
        return 2;
    }
    bool same = (strcmp(argv[1], argv[2]) == 0);

    /* Zeroed, as the header requires: yser_open() refuses a handle whose
     * is_open flag is set, and an automatic variable's is set to whatever was
     * on the stack. */
    yser_port a, b;
    memset(&a, 0, sizeof a);
    memset(&b, 0, sizeof b);
    /* A short write timeout on the handle under test, so the "peer stops
     * draining" check below does not sit for the 1 s default. Writes into an
     * empty transmit buffer never reach it. */
    yser_desc da = { .device = argv[1], .baud = 115200, .write_timeout_ns = YRT_MS(LB_TIMEOUT_MS) };
    yser_desc db = { .device = argv[2], .baud = 115200 };
    if (!yser_open(&a, &da)) { fprintf(stderr, "open %s: %s\n", argv[1], yser_error(&a)); return 2; }
    if (!same && !yser_open(&b, &db)) {
        fprintf(stderr, "open %s: %s\n", argv[2], yser_error(&b));
        yser_close(&a);
        return 2;
    }
    lb_w = &a;
    lb_r = same ? &a : &b;
    printf("write on %s, read on %s (%s), async_policy=%d\n\n",
           a.device, lb_r->device, same ? "wired loopback" : "port pair",
           (int)a.async_policy);

    yser_purge(lb_w, YSER_PURGE_RX | YSER_PURGE_TX);
    if (!same) yser_purge(lb_r, YSER_PURGE_RX | YSER_PURGE_TX);

    /* 0. Reopening an open handle is refused, not honored: without the guard
     * it memsets the handle and loses the descriptor and the worker thread.
     * Checked first, because a failure here would leak for the rest of the
     * run. The handle must still be usable afterwards, which everything
     * below exercises. */
    lb_check(!yser_open(lb_w, &da), "open on an open handle is refused",
             "error: %s", yser_error(lb_w));
    lb_check(yser_is_open(lb_w), "the handle survives the refusal", "%s", "is_open");

    /* 1. N bytes out, exactly N back. */
    uint8_t out[64], in[128];
    for (int i = 0; i < (int)sizeof(out); i++) out[i] = (uint8_t)(i * 7 + 1);
    int n = yser_write(lb_w, out, (int)sizeof(out));
    lb_check(n == (int)sizeof(out), "write 64 bytes", "accepted %d", n);
    n = yser_read(lb_r, in, (int)sizeof(out), YRT_MS(1000), YSER_READ_ALL);
    lb_check(n == (int)sizeof(out), "READ_ALL 64 within 1000 ms", "got %d (%s)",
             n, n < 0 ? yser_strerror(n) : "ok");
    lb_check(n == (int)sizeof(out) && memcmp(in, out, sizeof(out)) == 0,
             "payload matches", "%s", "memcmp");

    /* 2. An idle port polls to 0, at once, and not to an error. */
    uint64_t t0 = lb_now_us();
    n = yser_read(lb_r, in, (int)sizeof(in), 0, YSER_READ_ANY);
    uint64_t poll_us = lb_now_us() - t0;
    lb_check(n == 0 && poll_us < 2000, "READ_ANY timeout 0 when idle",
             "got %d in %llu us (want 0 in < 2000)", n, (unsigned long long)poll_us);

    /* 3. And waits out a finite timeout instead of returning early or never. */
    t0 = lb_now_us();
    n = yser_read(lb_r, in, (int)sizeof(in), YRT_MS(LB_TIMEOUT_MS), YSER_READ_ANY);
    uint64_t idle_ms = (lb_now_us() - t0) / 1000ull;
    lb_check(n == 0 && idle_ms >= LB_SLACK_LO && idle_ms <= LB_SLACK_HI,
             "READ_ANY timeout 100 when idle", "got %d after %llu ms (want 0 in %d..%d)",
             n, (unsigned long long)idle_ms, LB_SLACK_LO, LB_SLACK_HI);

    /* 4. yser_available agrees with what arrives. */
    n = yser_write(lb_w, out, 16);
    yser_drain(lb_w);
    int avail = lb_wait_available(lb_r, 16, 500);
    lb_check(avail == 16, "yser_available after 16 bytes", "reports %d", avail);
    n = yser_read(lb_r, in, 16, YRT_MS(500), YSER_READ_ALL);
    lb_check(n == 16, "drain those 16", "got %d", n);

    /* 5. Purge empties the receive buffer. */
    yser_write(lb_w, out, 16);
    yser_drain(lb_w);
    lb_wait_available(lb_r, 16, 500);
    int purged = yser_purge(lb_r, YSER_PURGE_RX);
    avail = yser_available(lb_r);
    n = yser_read(lb_r, in, (int)sizeof(in), 0, YSER_READ_ANY);
    lb_check(purged == 0 && avail == 0 && n == 0, "purge RX empties the buffer",
             "purge=%d available=%d read=%d", purged, avail, n);

    /* 6. READ_ALL returns a short count when the deadline passes. */
    yser_write(lb_w, out, 8);
    yser_drain(lb_w);
    lb_wait_available(lb_r, 8, 500);
    t0 = lb_now_us();
    n = yser_read(lb_r, in, 32, YRT_MS(200), YSER_READ_ALL);
    uint64_t elapsed_ms = (lb_now_us() - t0) / 1000ull;
    lb_check(n == 8 && elapsed_ms >= 195 && elapsed_ms <= 260,
             "READ_ALL short count at deadline", "got %d after %llu ms (want 8 in 195..260)",
             n, (unsigned long long)elapsed_ms);

    /* 7. A write whose peer stops draining returns a short count when
     *    write_timeout_ns expires, and is not an error. Filling the transmit
     *    path means filling the peer's receive buffer too, so give up if the
     *    transport swallows more than a few megabytes. */
    {
        uint8_t block[256];
        memset(block, 0x5A, sizeof(block));
        long sent = 0;
        int short_count = -1;
        uint64_t write_ms = 0;
        for (long i = 0; i < 16384; i++) {   /* 4 MiB budget */
            uint64_t w0 = lb_now_us();
            int k = yser_write(lb_w, block, (int)sizeof(block));
            uint64_t w_ms = (lb_now_us() - w0) / 1000ull;
            if (k < 0) { short_count = k; write_ms = w_ms; break; }
            sent += k;
            if (k < (int)sizeof(block)) { short_count = k; write_ms = w_ms; break; }
        }
        if (short_count < 0) {
            lb_info("write timeout when peer is full",
                    "no short count after %ld bytes (%s): transport never blocks",
                    sent, short_count == -1 ? "budget spent" : yser_strerror(short_count));
        } else {
            lb_check(write_ms >= LB_SLACK_LO && write_ms <= LB_SLACK_HI,
                     "write timeout when peer is full",
                     "short count %d after %ld bytes, blocked %llu ms (want %d..%d)",
                     short_count, sent, (unsigned long long)write_ms, LB_SLACK_LO, LB_SLACK_HI);
        }
        lb_quiesce(same);
    }

    /* 8. yser_interrupt wakes a blocked reader. */
    lb_read_rc = 0x7FFF;
    lb_thread th = LB_START(blocked_read);
    lb_sleep_ms(100);                    /* let it reach the blocking wait */
    t0 = lb_now_us();
    int irq = yser_interrupt(lb_r);
    lb_join(th);
    /* The read can only finish after t0 when it really blocked; a read that
     * returned early (leftover bytes in the transport) would underflow. */
    uint64_t wake_us = lb_read_done_us > t0 ? lb_read_done_us - t0 : 0;
    lb_check(irq == 0 && lb_read_rc == YSER_ERR_INTERRUPTED && wake_us < 10000,
             "interrupt wakes a blocked read", "rc=%d after %llu us (want < 10000)",
             lb_read_rc, (unsigned long long)wake_us);

    /* 9. A zero filter is the same enumeration. */
    int all = yser_list_ports(NULL, 0);
    yser_port_filter zero;
    memset(&zero, 0, sizeof(zero));
    int filtered = yser_find_ports(&zero, NULL, 0);
    lb_check(all == filtered, "find_ports(zero) == list_ports", "%d == %d", all, filtered);

    /* 10. A non-blocking pulse puts both bytes on the wire, in order, about
     *     `usec` apart. The spacing is measured where it matters, at the
     *     reader: it is the worker's deadline plus one transport hop at each
     *     end, which is what a device would see. */
    {
        uint8_t pa[2] = { 0, 0 };
        int rc = yser_pulse_async(lb_w, 0xA1, 0xA2, YRT_US(2000));
        int n1 = yser_read(lb_r, pa, 1, YRT_MS(500), YSER_READ_ANY);
        uint64_t t_on = lb_now_us();
        int n2 = yser_read(lb_r, pa + 1, 1, YRT_MS(500), YSER_READ_ANY);
        uint64_t t_off = lb_now_us();
        uint64_t gap_us = t_off > t_on ? t_off - t_on : 0;
        lb_check(rc == 1 && n1 == 1 && n2 == 1 && pa[0] == 0xA1 && pa[1] == 0xA2,
                 "pulse_async: on then off", "rc=%d, bytes %02X %02X", rc, pa[0], pa[1]);
        lb_check(gap_us >= 1500 && gap_us <= 6000, "pulse_async 2 ms spacing",
                 "%llu us apart, async_policy=%d (want 1500..6000)",
                 (unsigned long long)gap_us, (int)lb_w->async_policy);
    }

    /* 11. A plain write while a pulse is pending cancels the trailing byte: a
     *     write means "hold this". */
    {
        uint8_t pb[8];
        int rc = yser_pulse_async(lb_w, 0xB1, 0xB2, YRT_US(20000));
        int wr = yser_write_byte(lb_w, 0x42);
        /* Asks for four bytes so a surviving 0xB2 would be counted, and waits
         * past the 20 ms width. */
        int got = yser_read(lb_r, pb, 4, YRT_MS(50), YSER_READ_ALL);
        lb_check(rc == 1 && wr == 1 && got == 2 && pb[0] == 0xB1 && pb[1] == 0x42,
                 "write cancels a pending off",
                 "rc=%d wr=%d, %d bytes: %02X %02X", rc, wr, got,
                 got > 0 ? pb[0] : 0, got > 1 ? pb[1] : 0);
    }

    /* 11b. The same cancel, raced against the worker's own dispatch. The width
     *      here is short enough that the trailing byte is sometimes already on
     *      its way to the writer mutex when the plain write takes it, which is
     *      the window the armed/deadline token exists for: yrt_worker_cancel
     *      cannot reach a job that has left ysp/rt.h's lock. Either order is
     *      allowed on the wire. What must never happen is a 0xB2 AFTER the
     *      0x42 that means "hold this". */
    {
        const int rounds = 30;
        int raced = 0, undone = 0, odd = 0;
        for (int i = 0; i < rounds; i++) {
            uint8_t px[8];
            /* Alternating widths, because the two sides of the race need
             * different ones: a zero width is due the moment it is submitted,
             * so the worker is already running the callback, while 200 us
             * usually lets the cancel land first. */
            int rc = yser_pulse_async(lb_w, 0xB1, 0xB2, (i % 2) ? YRT_US(200) : 0);
            int wr = yser_write_byte(lb_w, 0x42);
            int got = yser_read(lb_r, px, 4, YRT_MS(20), YSER_READ_ALL);
            if (rc != 1 || wr != 1 || got < 2 || px[0] != 0xB1) { odd++; continue; }
            if (px[got - 1] != 0x42) undone++;       /* the write was undone */
            else if (got == 3 && px[1] == 0xB2) raced++;  /* off won, legally */
            else if (got != 2) odd++;
        }
        lb_check(undone == 0 && odd == 0, "write cancels a dispatched off",
                 "%d rounds: %d undone, %d where the off won the race first, "
                 "%d unexpected", rounds, undone, raced, odd);
    }

    /* 12. A second pulse moves the one pending trailing edge: two onsets, one
     *     off, and it is the newer off byte. */
    {
        uint8_t pc[8];
        int r1 = yser_pulse_async(lb_w, 0xC1, 0xC2, YRT_US(30000));
        lb_sleep_ms(1);
        int r2 = yser_pulse_async(lb_w, 0xC3, 0xC4, YRT_US(30000));
        int got = yser_read(lb_r, pc, 5, YRT_MS(200), YSER_READ_ALL);
        lb_check(r1 == 1 && r2 == 1 && got == 3 &&
                 pc[0] == 0xC1 && pc[1] == 0xC3 && pc[2] == 0xC4,
                 "second pulse replaces the first",
                 "%d bytes: %02X %02X %02X", got,
                 got > 0 ? pc[0] : 0, got > 1 ? pc[1] : 0, got > 2 ? pc[2] : 0);
    }

    /* 12b. Back to back, the second onset is usually still queued when it is
     *      asked for (v0.6: the worker writes onsets). Both onsets arrive, in
     *      order, before the one off; a write right after them waits for the
     *      queue and cancels that off. The calls themselves do no I/O. */
    {
        uint8_t pc[8];
        uint64_t t_call = lb_now_us();
        int r1 = yser_pulse_async(lb_w, 0xC5, 0xC6, YRT_US(30000));
        int r2 = yser_pulse_async(lb_w, 0xC7, 0xC8, YRT_US(30000));
        uint64_t call_us = lb_now_us() - t_call;
        int got = yser_read(lb_r, pc, 5, YRT_MS(200), YSER_READ_ALL);
        lb_check(r1 == 1 && r2 == 1 && got == 3 &&
                 pc[0] == 0xC5 && pc[1] == 0xC7 && pc[2] == 0xC8,
                 "queued pulses keep their order",
                 "%d bytes: %02X %02X %02X, two calls in %llu us", got,
                 got > 0 ? pc[0] : 0, got > 1 ? pc[1] : 0, got > 2 ? pc[2] : 0,
                 (unsigned long long)call_us);
        int r3 = yser_pulse_async(lb_w, 0xC9, 0xCA, YRT_US(30000));
        int wr = yser_write_byte(lb_w, 0x43);
        got = yser_read(lb_r, pc, 4, YRT_MS(60), YSER_READ_ALL);
        lb_check(r3 == 1 && wr == 1 && got == 2 && pc[0] == 0xC9 && pc[1] == 0x43,
                 "a write does not overtake a queued onset",
                 "r3=%d wr=%d, %d bytes: %02X %02X", r3, wr, got,
                 got > 0 ? pc[0] : 0, got > 1 ? pc[1] : 0);
    }

    /* 13. yser_close with a pulse pending writes the trailing byte at once
     *     instead of waiting out the width, so a device is never left holding
     *     a trigger code. Needs a third handle on the write device, which a
     *     Windows COM port will refuse (it is exclusive). */
    {
        yser_port c;
        memset(&c, 0, sizeof c);
        yser_desc dc = { .device = argv[1], .baud = 115200 };
        if (!yser_open(&c, &dc)) {
            lb_info("close flushes a pending off", "no third handle: %s", yser_error(&c));
        } else if (c.async_policy == YSER_ASYNC_NONE) {
            lb_info("close flushes a pending off", "no worker: %s", yser_error(&c));
            yser_close(&c);
        } else {
            uint8_t pd[4];
            int rc = yser_pulse_async(&c, 0xD1, 0xD2, YRT_US(1000000));  /* 1 s width */
            lb_sleep_ms(20);
            uint64_t t_close = lb_now_us();
            yser_close(&c);
            int got = yser_read(lb_r, pd, 2, YRT_MS(500), YSER_READ_ALL);
            uint64_t after_us = lb_now_us() - t_close;
            lb_check(rc == 1 && got == 2 && pd[0] == 0xD1 && pd[1] == 0xD2 &&
                     after_us < 5000, "close flushes a pending off",
                     "rc=%d, %d bytes %02X %02X, %llu us after close (want < 5000)",
                     rc, got, got > 0 ? pd[0] : 0, got > 1 ? pd[1] : 0,
                     (unsigned long long)after_us);
        }
    }

    /* 14. One reader thread and one writer thread on one handle, which is what
     *     the error-code split exists for. Run this under ThreadSanitizer. */
    yser_purge(lb_w, YSER_PURGE_RX | YSER_PURGE_TX);
    if (!same) yser_purge(lb_r, YSER_PURGE_RX | YSER_PURGE_TX);
    lb_thread tw = LB_START(hammer_write);
    lb_thread tr = LB_START(hammer_read);
    uint64_t hammer_end = lb_now_us() + 500000ull;
    long fed = 0;
    while (lb_now_us() < hammer_end) {
        if (!same) {
            /* Feed the handle under test from the far end, and keep the far
             * end's own receive buffer from filling up. */
            uint8_t sink[256];
            if (yser_write_byte(lb_r, 0x5A) == 1) fed++;
            yser_read(lb_r, sink, (int)sizeof(sink), 0, YSER_READ_ANY);
        }
        lb_sleep_ms(1);
    }
    lb_stop_set();
    yser_interrupt(lb_w);
    lb_join(tw);
    lb_join(tr);
    lb_check(lb_hammer_err == 0 && lb_hammer_written > 0,
             "concurrent reader + writer",
             "wrote %ld (%ld async pulses), read %ld, fed %ld, short %ld, errors %ld",
             lb_hammer_written, lb_hammer_pulsed, lb_hammer_read, fed,
             lb_hammer_short, lb_hammer_err);

    /* 15. The wake that stopped the hammer reader may have landed after it had
     *     already left on the stop flag, and an unconsumed wake stays latched.
     *     One read consumes it: a pending wake beats buffered data, so after
     *     this call no latch can be left to surprise the check below. */
    {
        uint8_t sink[256];
        int drained = yser_read(lb_w, sink, (int)sizeof(sink), 0, YSER_READ_ANY);
        lb_check(drained >= 0 || drained == YSER_ERR_INTERRUPTED,
                 "interrupt latch consumed", "first read after the joins: %d (%s)",
                 drained, drained == YSER_ERR_INTERRUPTED ? "latch" :
                          drained < 0 ? yser_strerror(drained) : "no latch");
        lb_quiesce(same);
    }

    /* 16. The peer going away must not look like silence. Last, because it
     *     destroys the pair. What actually happens depends on the transport:
     *     a real USB unplug and a closed pty master both hang up, but a socat
     *     pty pair only hangs up once socat itself notices and exits. */
    if (!same) {
        yser_close(lb_r);
        lb_sleep_ms(200);
        n = yser_read(lb_w, in, (int)sizeof(in), YRT_MS(500), YSER_READ_ANY);
        if (n == YSER_ERR_DISCONNECTED)
            lb_check(1, "read after the peer closed", "%s", "YSER_ERR_DISCONNECTED");
        else if (n == 0)
            lb_info("read after the peer closed", "%s", "no hangup on this transport");
        else
            lb_check(0, "read after the peer closed", "got %d (%s)",
                     n, n < 0 ? yser_strerror(n) : "bytes from a closed peer");
    }

    yser_close(lb_w);
    printf("\n%s (%d failure%s)\n", lb_failures ? "FAILED" : "all checks passed",
           lb_failures, lb_failures == 1 ? "" : "s");
    return lb_failures ? 1 : 0;
}
