/* parallel_test.c - self-checking test for ysp/parallel.h without a port.
 * It returns 0 when every check passed and 1 after printing each failure.
 *
 *     gcc -std=c11 -Wall -Wextra -Werror -Iinclude -o parallel_test \
 *         tests/adapt/parallel_test.c -lm -lpthread              (Linux)
 *
 * The trailing-edge deadline (v0.4.1) on every platform. On Windows a
 * handle opened by hand, with Out32 and Inp32 replaced by functions that
 * keep each write, runs ypar_pulse() and ypar_pulse_async() with no driver:
 * the test includes the implementation and fills the handle as
 * ypar_open() would. Elsewhere a write goes to ppdev or to an I/O port,
 * which a test cannot fake.
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#define YSP_PARALLEL_IMPLEMENTATION
#include "ysp/parallel.h"

#include <stdio.h>
#include <string.h>

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond) do { g_checks++; if (!(cond)) { \
    fprintf(stderr, "parallel_test: FAIL at line %d: %s\n", __LINE__, #cond); g_failures++; } } while (0)
#define CHECK_I(got, want) do { long long g_ = (long long)(got), w_ = (long long)(want); g_checks++; \
    if (g_ != w_) { fprintf(stderr, "parallel_test: FAIL at line %d: %s (got %lld, want %lld)\n", \
                            __LINE__, #got, g_, w_); g_failures++; } } while (0)

/* "Forever": INT64_MAX, and a width whose sum with any clock reading
 * still passes INT64_MAX. */
static const int64_t g_forever[2] = { INT64_MAX, INT64_MAX - YRT_MS(1) };

/* v0.4.1: the deadline is t + width, saturated. The plain sum of a width
 * near INT64_MAX overflowed into the past, and the 0 came at once. */
static void test_off_at(void) {
    int i;
    CHECK_I(ypar__off_at(YRT_S(1000), YRT_MS(2)), YRT_S(1000) + YRT_MS(2));
    CHECK_I(ypar__off_at(YRT_S(1000), 0), YRT_S(1000));
    for (i = 0; i < 2; i++) CHECK(ypar__off_at(YRT_S(1000), g_forever[i]) == INT64_MAX);
    CHECK(ypar__off_at(INT64_MAX - 10, 10) == INT64_MAX);
    CHECK(ypar__off_at(INT64_MAX - 10, 9) == INT64_MAX - 1);
    CHECK(ypar__off_at(0, INT64_MAX) == INT64_MAX);
    printf("  the trailing edge's deadline saturates: ok\n");
}

#if defined(_WIN32)

/* The port: each data write, in order. The async worker writes too. */
static volatile LONG g_nw;
static uint8_t g_w[4096];

static void __stdcall fake_out(short addr, short val) {
    LONG k;
    if ((uint16_t)addr != 0x378) return;   /* data register only */
    k = InterlockedIncrement(&g_nw) - 1;
    if (k < 4096) g_w[k] = (uint8_t)val;
}
static short __stdcall fake_inp(short addr) { (void)addr; return 0; }

static int nw(void) { return (int)InterlockedCompareExchange(&g_nw, 0, 0); }
static int last(void) { int n = nw(); return n > 0 && n <= 4096 ? g_w[n - 1] : -1; }

/* What ypar_open() leaves on a port: open, the DLL's entry points. */
static void fake_open(ypar_port* p) {
    memset(p, 0, sizeof *p);
    p->backend = YPAR_BACKEND_INPOUT;
    p->base_addr = 0x378;
    p->out_fn = (void (*)(void))fake_out;
    p->inp_fn = (void (*)(void))fake_inp;
    CHECK(ypar__open_done(p));   /* open, and the async worker started */
    CHECK(p->async != NULL);
}

static void test_pulses(void) {
    ypar_port p;
    int i, n;
    int64_t t0;
    fake_open(&p);
    /* the stand-in works: a 2 ms pulse on the worker ends with its 0 */
    g_nw = 0;
    CHECK_I(ypar_pulse_async(&p, 0x0F, YRT_MS(2)), 0);
    t0 = yrt_now_ns();
    while (nw() < 2 && yrt_now_ns() - t0 < YRT_S(5)) (void)yrt_sleep_ns(YRT_MS(1));
    CHECK_I(nw(), 2);
    CHECK(g_w[0] == 0x0F && g_w[1] == 0);
    /* async, forever: the value holds until the next write */
    for (i = 0; i < 2; i++) {
        g_nw = 0;
        CHECK_I(ypar_pulse_async(&p, 0x55, g_forever[i]), 0);
        (void)yrt_sleep_ns(YRT_MS(50));
        CHECK_I(nw(), 1);
        CHECK_I(last(), 0x55);
        CHECK_I(ypar_write_data(&p, 0), 0);
        CHECK_I(last(), 0);
    }
    /* blocking, forever, on the virtual clock: the wait runs to the
     * clock's end instead of returning at once */
    for (i = 0; i < 2; i++) {
        g_nw = 0;
        CHECK_I(yrt_virtual_begin(YRT_S(1000)), 0);
        CHECK_I(ypar_pulse(&p, 0x33, g_forever[i]), 0);
        CHECK(yrt_now_ns() == INT64_MAX);
        yrt_virtual_end();
        n = nw();
        CHECK(n == 2 && g_w[0] == 0x33 && g_w[1] == 0);
    }
    ypar_close(&p);
    CHECK(!ypar_is_open(&p));
    printf("  pulses on a stand-in port: a width near INT64_MAX holds the value: ok\n");
}
#endif

int main(void) {
    printf("ysp_parallel %s\n", YPAR_VERSION_STRING);
    test_off_at();
#if defined(_WIN32)
    test_pulses();
#else
    printf("  pulses: Windows only (a ppdev or I/O port write cannot be faked)\n");
#endif
    if (g_failures) {
        fprintf(stderr, "parallel_test: %d of %d checks FAILED\n", g_failures, g_checks);
        return 1;
    }
    printf("parallel_test: all %d checks passed\n", g_checks);
    return 0;
}
