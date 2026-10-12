/* open_cost.c - what yser_open() and yser_set_rts() cost, for one build of
 * ysp/serial.h: n opens and closes of the port, then n RTS calls on one
 * open port (alternating levels, 20 ms apart), each timed with yrt_now_ns().
 *   open_cost COM3 [n]
 * Prints p50, p90, p99 and max in microseconds, the driver and rts_via_dtr
 * where the build has them. */
#define _CRT_SECURE_NO_WARNINGS
#define YSP_SERIAL_IMPLEMENTATION
#include "ysp/serial.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int cmp(const void* a, const void* b) {
    int64_t x = *(const int64_t*)a, y = *(const int64_t*)b;
    return x < y ? -1 : x > y;
}

static void show(const char* what, int64_t* v, int n) {
    qsort(v, (size_t)n, sizeof v[0], cmp);
    printf("%-12s n %4d  p50 %8.1f  p90 %8.1f  p99 %8.1f  max %8.1f us\n", what, n, (double)v[n / 2] / 1e3,
           (double)v[(n * 9) / 10] / 1e3, (double)v[(n * 99) / 100] / 1e3, (double)v[n - 1] / 1e3);
}

int main(int argc, char** argv) {
    const char* dev = argc > 1 ? argv[1] : "COM3";
    int n = argc > 2 ? atoi(argv[2]) : 100;
    static int64_t o[2000], c[2000], r[2000];
    yser_port p;
    yser_desc d;
    if (n > 2000) n = 2000;
    memset(&d, 0, sizeof d);
    d.device = dev;
    printf("ysp/serial.h %s\n", YSER_VERSION_STRING);
    for (int i = 0; i < n; i++) {
        memset(&p, 0, sizeof p);
        int64_t t0 = yrt_now_ns();
        if (!yser_open(&p, &d)) { fprintf(stderr, "open: %s\n", yser_error(&p)); return 1; }
        int64_t t1 = yrt_now_ns();
        yser_close(&p);
        int64_t t2 = yrt_now_ns();
        o[i] = t1 - t0;
        c[i] = t2 - t1;
        yrt_sleep_ns(YRT_MS(20));
    }
    show("open", o, n);
    show("close", c, n);
    memset(&p, 0, sizeof p);
    if (!yser_open(&p, &d)) return 1;
#if YSER_VERSION_MINOR > 6 || (YSER_VERSION_MINOR == 6 && YSER_VERSION_PATCH >= 1)
    printf("driver \"%s\", rts_via_dtr %d\n", p.driver, (int)p.rts_via_dtr);
#endif
    for (int i = 0; i < n; i++) {
        int64_t t0 = yrt_now_ns();
        if (yser_set_rts(&p, (i & 1) == 0) < 0) { fprintf(stderr, "set_rts failed\n"); return 1; }
        r[i] = yrt_now_ns() - t0;
        yrt_sleep_ns(YRT_MS(20));
    }
    show("set_rts", r, n);
    yser_close(&p);
    return 0;
}
