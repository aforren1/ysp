/* device_loopback.c - ysp/device.h on a real serial port pair.
 *
 * Not a compile check and not run by ctest: it needs two serial ports wired
 * to each other. Build it with -DYSP_BUILD_LOOPBACK=ON.
 *
 *     device_loopback <device_port> <board_port> [socat_pid]
 *
 * A virtual pair works: on Linux
 *     socat -d -d pty,raw,echo=0 pty,raw,echo=0 &
 * prints two /dev/pts/N names (com0com gives a pair on Windows). A thread
 * plays a ysp line protocol board on <board_port>: its clock is the host's
 * microseconds plus an offset, it sends a sync every 100 ms and an edge
 * every 50 ms, and it answers "i" and "q <seq>". ysp/device.h opens
 * "port:<device_port>" with the serial transport, on its own reader thread.
 * The program checks that the device identifies and runs, that every edge
 * arrives with its time mapped through the fit to within 2 ms of the time
 * the board stamped it, and that timer queries are answered. With
 * socat_pid it then kills socat, which hangs up the port, and checks that
 * the device goes LOST within 2 s. Before that it writes 20 output codes
 * ("o"); the board answers each with "O <t> <code>" stamped when it read
 * the command, and the program checks that each report, mapped through the
 * fit, lies within 5 ms after the time before its write.
 *
 * Exit code 0 if every check passed, 1 otherwise, 2 for a usage error.
 */
#define YSP_DEVICE_IMPLEMENTATION
#include "ysp/device.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <signal.h>
#include <sys/types.h>
#endif

static yser_port   g_board;
static ydev_device g_dev;
static volatile int g_board_stop;
static int64_t     g_offset_us = 123456789;   /* the board's clock minus ours */
static int64_t     g_edges[4096];               /* host ns of each edge sent */
static volatile int g_n_edges;
static yin_event   g_ev[4096];
static int         g_nev;
static unsigned char g_ring_mem[YRT_RING_BYTES(16384)];
static yrt_ring    g_ring;
static yrt_event   g_rec[16384];

static uint32_t board_us(void) { return (uint32_t)(yrt_now_ns() / 1000 + g_offset_us); }

static void board_send(const char* s) { (void)yser_write(&g_board, s, (int)strlen(s)); }

#if defined(_WIN32)
static DWORD WINAPI board_thread(LPVOID arg)
#else
static void* board_thread(void* arg)
#endif
{
    char line[64], out[96];
    int nline = 0, level = 0;
    int64_t next_sync = yrt_now_ns(), next_edge = next_sync + 30000000;
    (void)arg;
    while (!g_board_stop) {
        uint8_t b[64];
        int n = yser_read(&g_board, b, (int)sizeof b, 2, YSER_READ_ANY), i;
        int64_t now = yrt_now_ns();
        for (i = 0; i < n; i++) {
            uint32_t t = board_us();
            if (b[i] == '\n') {
                line[nline] = '\0';
                if (line[0] == 'q') { snprintf(out, sizeof out, "Q %s %lu\n", line + 2, (unsigned long)t); board_send(out); }
                else if (line[0] == 'i') board_send("I ysp-line 1 loopback test\n");
                else if (line[0] == 'o') { snprintf(out, sizeof out, "O %lu %s\n", (unsigned long)t, line + 2); board_send(out); }
                nline = 0;
            } else if (nline < 63) {
                line[nline++] = (char)b[i];
            }
        }
        if (n < 0) break;
        if (now >= next_sync) {
            snprintf(out, sizeof out, "S %lu\n", (unsigned long)board_us());
            board_send(out);
            next_sync += 100000000;
        }
        if (now >= next_edge && g_n_edges < 4096) {
            int64_t th = yrt_now_ns();
            uint32_t t = (uint32_t)(th / 1000 + g_offset_us);
            level ^= 1;
            snprintf(out, sizeof out, "E %lu 1 %d\n", (unsigned long)t, level);
            board_send(out);
            g_edges[g_n_edges] = (th / 1000) * 1000;   /* the board's microsecond, on our clock */
            g_n_edges = g_n_edges + 1;
            next_edge += 50000000;
        }
    }
#if defined(_WIN32)
    return 0;
#else
    return NULL;
#endif
}

static void sink(void* ctx, const yin_event* e) { (void)ctx; if (g_nev < 4096) g_ev[g_nev++] = *e; }

int main(int argc, char** argv) {
    yser_desc sd;
    ydev_desc d;
    ydev_stats st;
    char key[300];
    int fails = 0, i, k, matched = 0;
    double worst = 0;
#if defined(_WIN32)
    HANDLE th;
#else
    pthread_t th;
#endif
    if (argc < 3) { fprintf(stderr, "usage: device_loopback <device_port> <board_port> [socat_pid]\n"); return 2; }
    memset(&sd, 0, sizeof sd);
    sd.device = argv[2];
    if (!yser_open(&g_board, &sd)) { fprintf(stderr, "board port: %s\n", yser_error(&g_board)); return 2; }
#if defined(_WIN32)
    th = CreateThread(NULL, 0, board_thread, NULL, 0, NULL);
#else
    pthread_create(&th, NULL, board_thread, NULL);
#endif
    snprintf(key, sizeof key, "port:%s", argv[1]);
    memset(&d, 0, sizeof d);
    d.role = "loopback";
    d.family = YBOX_LINE;
    d.key = key;
    d.device = 1;
    d.kind = YIN_KIND_SYNC;
    d.sink = sink;
    d.no_elevate = true;
    {
        yrt_ring_desc rd;
        memset(&rd, 0, sizeof rd);
        rd.memory = g_ring_mem;
        rd.bytes = sizeof g_ring_mem;
        if (!yrt_ring_open(&g_ring, &rd)) return 2;
        d.ring = &g_ring;
    }
    if (!ydev_start(&g_dev, &d)) { fprintf(stderr, "%s\n", ydev_error(&g_dev)); return 2; }
    (void)yrt_sleep_ns(6000000000ull);
    {
        /* outputs over the serial transport: 20 codes, each reported back */
        int64_t before[20];
        int got = 0, r, m;
        double worst_rep = 0;
        while (yrt_ring_drain(&g_ring, g_rec, 16384) > 0) { }
        for (i = 0; i < 20; i++) {
            ydev_out_info oi;
            if (ydev_out_set(&g_dev, (uint32_t)(1 + (i & 1))) != YDEV_OK) { puts("FAIL: ydev_out_set"); fails++; break; }
            ydev_out_last(&g_dev, &oi);
            before[i] = oi.t_before;
            (void)yrt_sleep_ns(20000000);
        }
        (void)yrt_sleep_ns(100000000);
        m = yrt_ring_drain(&g_ring, g_rec, 16384);
        for (r = 0; r < m && got < 20; r++) {
            const yrt_event* e = &g_rec[r];
            double dt;
            if (e->source != YRT_SRC_DEVICE || e->kind != YDEV_REC_OUT || !(e->u.u16[18] & YDEV_OUT_REPORTED)) continue;
            dt = (double)(e->u.i64[0] - before[got]);
            if (dt < -100000.0 || dt > 5e6 || e->u.u32[8] != (uint32_t)(1 + (got & 1))) { printf("FAIL: report %d: %.1f us\n", got, dt / 1e3); fails++; }
            if (dt > worst_rep) worst_rep = dt;
            got++;
        }
        printf("outputs: 20 written, %d reported, worst report minus write %.1f us\n", got, worst_rep / 1e3);
        if (got != 20) { puts("FAIL: reports"); fails++; }
    }
    ydev_get_stats(&g_dev, &st);
    printf("state %s, ident \"%s\", %llu events, %llu probes, %llu answers, fit %+.2f ppm, spread %.1f us, width %.1f us\n",
           ydev_state_name(st.state), st.ident, (unsigned long long)st.events, (unsigned long long)st.probes,
           (unsigned long long)st.answers, st.fit.ppm, (double)st.fit.spread_ns / 1e3, (double)st.fit.width_ns / 1e3);
    if (st.state != YDEV_RUNNING || strcmp(st.ident, "ysp-line 1 loopback test") != 0) { puts("FAIL: not running"); fails++; }
    if (st.probes < 30 || st.answers < st.probes * 9 / 10) { puts("FAIL: timer queries"); fails++; }
    if (argc > 3) {
#if !defined(_WIN32)
        int64_t t_kill = yrt_now_ns();
        kill((pid_t)atoi(argv[3]), SIGTERM);
        while (ydev_state(&g_dev) == YDEV_RUNNING && yrt_now_ns() - t_kill < 3000000000LL) (void)yrt_sleep_ns(10000000);
        printf("after the hang-up: %s in %.3f s\n", ydev_state_name(ydev_state(&g_dev)),
               (double)(yrt_now_ns() - t_kill) / 1e9);
        if (ydev_state(&g_dev) != YDEV_LOST || yrt_now_ns() - t_kill > 2000000000LL) { puts("FAIL: not lost"); fails++; }
#endif
    }
    g_board_stop = 1;
    ydev_stop(&g_dev);
#if defined(_WIN32)
    WaitForSingleObject(th, INFINITE);
#else
    pthread_join(th, NULL);
#endif
    yser_close(&g_board);
    /* every edge after the first second, matched by its tick */
    for (i = 0; i < g_nev; i++) {
        for (k = 0; k < g_n_edges; k++) {
            if ((uint32_t)(g_edges[k] / 1000 + g_offset_us) == g_ev[i].ticks) {
                double e = (double)(g_ev[i].t - g_edges[k]);
                if (g_edges[k] - g_edges[0] > 1000000000LL) {
                    if (e < 0) e = -e;
                    if (e > worst) worst = e;
                    matched++;
                }
                break;
            }
        }
    }
    printf("%d edges sent, %d events, %d matched after 1 s, worst |t - edge| %.1f us\n", g_n_edges, g_nev, matched,
           worst / 1e3);
    if (matched < 80 || worst > 2e6) { puts("FAIL: edges"); fails++; }
    printf("%s\n", fails ? "FAILED" : "all checks passed");
    return fails ? 1 : 0;
}
