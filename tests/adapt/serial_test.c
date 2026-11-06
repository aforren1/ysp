/* serial_test.c - ysp/serial.h on a pseudo-terminal, no hardware.
 *
 * A pseudo-terminal's slave is a tty that yser_open() configures as it
 * does a port; this test holds the master. It checks bytes both ways, a
 * read that times out, desc.exclusive (a second exclusive open of the same
 * slave refused, and allowed again after the close) and a hangup (the
 * master closed: YSER_ERR_DISCONNECTED). Modem lines, IOSSIOSPEED and
 * timing need a real port: tests/loopback/serial_loopback.c.
 *
 * Windows has no pseudo-terminal; there the test prints a note and passes.
 * Exit code 0 when every check passed, 1 otherwise.
 */
#if defined(__linux__)
    /* posix_openpt() and ptsname() are XSI; _DEFAULT_SOURCE keeps what
     * ysp/serial.h needs (CRTSCTS). Not on macOS, where a feature macro
     * hides the Darwin calls that ysp/rt.h uses. */
    #define _DEFAULT_SOURCE 1
    #define _XOPEN_SOURCE 700
#endif
#define YSP_SERIAL_IMPLEMENTATION
#include "ysp/serial.h"

#include <stdio.h>
#include <string.h>

static int g_checks, g_failures;
#define CHECK(cond, what) do { g_checks++; if (!(cond)) { g_failures++; \
    printf("serial_test: FAIL line %d: %s\n", __LINE__, (what)); } } while (0)

#if defined(_WIN32)
int main(void) {
    printf("serial_test: no pseudo-terminal on Windows; nothing to run\n");
    return 0;
}
#else
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

/* A master and its slave's name; -1 when the OS gives none. */
static int open_pty(char* name, size_t cap) {
    int m = posix_openpt(O_RDWR | O_NOCTTY);
    const char* s;
    if (m < 0) return -1;
    if (grantpt(m) != 0 || unlockpt(m) != 0 || (s = ptsname(m)) == NULL || strlen(s) >= cap) {
        close(m);
        return -1;
    }
    strcpy(name, s);
    return m;
}

static void test_bytes(int m, const char* name) {
    yser_port p;
    yser_desc d;
    char b[16];
    int n, k;
    int64_t t0;
    memset(&p, 0, sizeof p);
    memset(&d, 0, sizeof d);
    d.device = name;
    CHECK(yser_open(&p, &d), yser_error(&p));
    if (!yser_is_open(&p)) return;
    CHECK(yser_write(&p, "abc", 3) == 3, "write 3 bytes");
    for (n = 0, k = 0; n < 3 && k < 100; k++) {
        int r = (int)read(m, b + n, sizeof b - (size_t)n);
        if (r > 0) n += r; else usleep(1000);
    }
    CHECK(n == 3 && memcmp(b, "abc", 3) == 0, "the master reads them");
    CHECK(write(m, "xyz", 3) == 3, "the master writes 3 bytes");
    n = yser_read(&p, b, 3, YRT_MS(1000), YSER_READ_ALL);
    CHECK(n == 3 && memcmp(b, "xyz", 3) == 0, "READ_ALL reads them");
    t0 = yrt_now_ns();
    n = yser_read(&p, b, sizeof b, YRT_MS(50), YSER_READ_ANY);
    CHECK(n == 0 && yrt_now_ns() - t0 >= YRT_MS(45), "an idle read times out with 0");
    yser_close(&p);
}

/* v0.6.3: on macOS TIOCEXCL let a second open of a pseudo-terminal
 * through; the flock() refuses it. Linux refuses it by TIOCEXCL, except
 * for root (CAP_SYS_ADMIN), which may open anything. */
static void test_exclusive(const char* name) {
    yser_port p, q;
    yser_desc d;
    bool second;
    memset(&p, 0, sizeof p);
    memset(&q, 0, sizeof q);
    memset(&d, 0, sizeof d);
    d.device = name;
    d.exclusive = true;
    CHECK(yser_open(&p, &d), yser_error(&p));
    second = yser_open(&q, &d);
#if defined(__APPLE__)
    CHECK(!second, "a second exclusive open is refused");
    CHECK(strstr(yser_error(&q), "exclusive") != NULL, "the message says why");
#else
    if (geteuid() != 0) CHECK(!second, "a second exclusive open is refused");
#endif
    if (second) yser_close(&q);
    yser_close(&p);
    memset(&q, 0, sizeof q);
    CHECK(yser_open(&q, &d), "an exclusive open after the close");
    yser_close(&q);
}

static void test_hangup(int m, const char* name) {
    yser_port p;
    yser_desc d;
    char b[4];
    int r;
    memset(&p, 0, sizeof p);
    memset(&d, 0, sizeof d);
    d.device = name;
    CHECK(yser_open(&p, &d), yser_error(&p));
    if (!yser_is_open(&p)) { close(m); return; }
    close(m);
    r = yser_read(&p, b, sizeof b, YRT_MS(1000), YSER_READ_ANY);
    CHECK(r == YSER_ERR_DISCONNECTED, "a read after the master closed: disconnected");
    yser_close(&p);
}

int main(void) {
    char name[256];
    int m = open_pty(name, sizeof name);
    if (m < 0) {
        printf("serial_test: no pseudo-terminal here; nothing to run\n");
        return 0;
    }
    test_bytes(m, name);
    test_exclusive(name);
    test_hangup(m, name);
    printf("serial_test: %d checks, %d failed (%s)\n", g_checks, g_failures, name);
    return g_failures ? 1 : 0;
}
#endif
