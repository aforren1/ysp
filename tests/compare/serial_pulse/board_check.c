/* board_check.c - functional checks of ysp/serial.h and ysp/device.h against
 * the echo board (firmware/ysp_echo, version 2). No timing: run it first, on
 * any machine state, to know that the port, the board and every output path
 * work before a timed run.
 *
 *   board_check <device>          e.g. /dev/cu.usbmodem75925601, COM3
 *
 * Checks, each printed as pass or FAIL:
 *   - the port is listed by yser_list_ports(), and yser_find_ports() with
 *     the name as filter finds it
 *   - yser_open(); "v" answers "V ysp-echo 2 ...", "i" answers
 *     "I ysp-line 1 ..."
 *   - 500 "e <seq>" exchanges: every answer has the right length and seq,
 *     and the board's two stamps are in order
 *   - yser_set_dtr() and yser_set_rts(): each change is reported by the
 *     board ("l 1") with both bits right; yser_get_lines() succeeds
 *   - yser_write_byte(), yser_pulse() and yser_pulse_async(): the board
 *     stamps the bytes in order ("b" mode)
 *   - exclusive: a second exclusive open of the port is refused
 *   - ysp/device.h with its own serial transport (key "port:<device>"):
 *     a TriggerBox's ydev_out_set() and ydev_out_pulse() reach the board as
 *     bytes, and a LINES device's codes reach it as DTR and RTS. The
 *     transport's read is left out, so this program can read the board's
 *     reports on a second handle of the same port; open, write, lines and
 *     close are the header's own.
 * Information only: the driver name, a nonstandard rate (250000).
 *
 * Exit code 0 if every check passed, 1 otherwise, 2 for a usage error.
 *
 * Build: cc -O2 -Wall -I../../../include -o board_check board_check.c -lpthread
 * (Windows: cl /O2 /W4 /I..\..\..\include board_check.c; a COM port is
 * exclusive there, so the device part's second handle fails with a note).
 *
 * Public domain / MIT-0, as the rest of ysp. */
#define _CRT_SECURE_NO_WARNINGS
#define YSP_DEVICE_IMPLEMENTATION
#include "ysp/device.h"   /* first on POSIX: ysp/serial.h sets _DEFAULT_SOURCE */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail;

static void check(int ok, const char* what, const char* fmt, ...) {
    va_list ap;
    printf("%-5s %-44s ", ok ? "pass" : "FAIL", what);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
    if (!ok) g_fail++;
}

static void info(const char* what, const char* fmt, ...) {
    va_list ap;
    printf("%-5s %-44s ", "info", what);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}

static void sleep_ms(int ms) { (void)yrt_sleep_ns((int64_t)ms * 1000000); }

static int say(yser_port* p, const char* s) { return yser_write(p, s, (int)strlen(s)); }

/* One line ending in '\n', without it; 0 on timeout. */
static int read_line(yser_port* p, char* out, int cap, int64_t timeout_ns) {
    int n = 0;
    int64_t end = yrt_now_ns() + timeout_ns;
    while (yrt_now_ns() < end) {
        uint8_t c;
        int r = yser_read(p, &c, 1, end - yrt_now_ns(), YSER_READ_ANY);
        if (r < 0) return r;
        if (r == 0) continue;
        if (c == '\n') {
            if (n > 0 && out[n - 1] == '\r') n--;
            out[n] = '\0';
            return n;
        }
        if (n < cap - 1) out[n++] = (char)c;
    }
    out[n] = '\0';
    return 0;
}

/* The leading newline ends any partial line: a TriggerBox's open writes a
 * 0x00, which the board would otherwise take as the start of the next
 * command, and ignore that command. */
static void quiet(yser_port* p) {
    say(p, "\nx\nl 0\n");
    sleep_ms(50);
    yser_purge(p, YSER_PURGE_RX);
}

/* Reads "L <t> <dtr> <rts>" lines until one shows the wanted state. */
static int wait_lines(yser_port* p, int dtr, int rts) {
    char l[64];
    for (;;) {
        int n = read_line(p, l, sizeof l, YRT_MS(300));
        if (n <= 0) return 0;
        if (n == 16 && l[0] == 'L' && l[13] - '0' == dtr && l[15] - '0' == rts) return 1;
    }
}

/* Reads the B lines of "b" mode until D; writes the byte values to v. */
static int read_bytes(yser_port* p, int* v, int cap) {
    char l[64];
    int n = 0;
    for (;;) {
        int k = read_line(p, l, sizeof l, YRT_MS(1500));
        if (k <= 0) return -1;
        if (l[0] == 'B' && k == 16 && n < cap) v[n++] = atoi(l + 13);
        if (l[0] == 'D') return n;
    }
}

static int same(const int* a, const int* b, int n) {
    for (int i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}

static void fmt_bytes(char* s, size_t cap, const int* v, int n) {
    size_t o = 0;
    s[0] = '\0';
    for (int i = 0; i < n && o + 5 < cap; i++) o += (size_t)snprintf(s + o, cap - o, "%s%02X", i ? " " : "", v[i]);
}

static void check_device(const char* dev) {
    char key[300], s[128];
    snprintf(key, sizeof key, "port:%s", dev);
    yser_port rd;
    yser_desc rdd;
    memset(&rd, 0, sizeof rd);
    memset(&rdd, 0, sizeof rdd);
    rdd.device = dev;
    rdd.dtr_low_on_open = rdd.rts_low_on_open = true;
    if (!yser_open(&rd, &rdd)) {
        info("ysp/device.h on its own transport", "no second handle: %s", yser_error(&rd));
        return;
    }
    quiet(&rd);

    /* TriggerBox: a byte path */
    {
        static ydev_device d;
        ydev_desc dd;
        memset(&d, 0, sizeof d);
        memset(&dd, 0, sizeof dd);
        dd.role = "trig";
        dd.family = YBOX_TRIGGERBOX;
        dd.key = key;
        dd.device = 1;
        if (!ydev_open(&d, &dd)) { check(0, "ydev_open TRIGGERBOX", "%s", ydev_error(&d)); goto lines; }
        /* the open itself: the serial transport, then without its read */
        ydev_close(&d);
        memset(&d, 0, sizeof d);
        dd.transport = ydev_serial_transport(&d);
        dd.transport.read = NULL;
        dd.transport.interrupt = NULL;
        if (!ydev_open(&d, &dd) || ydev_wait_running(&d, YRT_S(3)) != YDEV_RUNNING) {
            check(0, "ydev_open TRIGGERBOX", "%s", ydev_error(&d));
            goto lines;
        }
        sleep_ms(50);
        yser_purge(&rd, YSER_PURGE_RX);
        say(&rd, "\nb 5 1000\n");
        sleep_ms(30);
        int r1 = ydev_out_set(&d, 0x55);
        int r2 = ydev_out_set(&d, 0);
        int r3 = ydev_out_pulse(&d, 0x2A, YRT_MS(5));
        sleep_ms(30);
        int r4 = ydev_out_mark(&d, 0x11, NULL);
        int v[16], want[5] = { 0x55, 0x00, 0x2A, 0x00, 0x11 };
        int n = read_bytes(&rd, v, 16);
        fmt_bytes(s, sizeof s, v, n > 0 ? n : 0);
        check(r1 == YDEV_OK && r2 == YDEV_OK && r3 == YDEV_OK && r4 == YDEV_OK && n >= 4 && same(v, want, 4),
              "TriggerBox: set, pulse reach the board", "rc %d %d %d, board saw %s", r1, r2, r3, s);
        info("TriggerBox: mark", "rc %d (%s)", r4, n == 5 && v[4] == 0x11 ? "board saw 11" : "see above");
        ydev_close(&d);
    }
lines:
    /* LINES: DTR is bit 0, RTS bit 1 */
    {
        static ydev_device d;
        ydev_desc dd;
        memset(&d, 0, sizeof d);
        memset(&dd, 0, sizeof dd);
        dd.role = "lines";
        dd.family = YBOX_LINES;
        dd.key = key;
        dd.device = 1;
        dd.transport = ydev_serial_transport(&d);
        dd.transport.read = NULL;
        dd.transport.interrupt = NULL;
        if (!ydev_open(&d, &dd) || ydev_wait_running(&d, YRT_S(3)) != YDEV_RUNNING) {
            check(0, "ydev_open LINES", "%s", ydev_error(&d));
            yser_close(&rd);
            return;
        }
        sleep_ms(50);
        yser_purge(&rd, YSER_PURGE_RX);
        say(&rd, "\nl 1\n");
        int ok = wait_lines(&rd, 0, 0);
        check(ok, "LINES: both low after the open", "%s", ok ? "L 0 0" : "no L 0 0 report");
        static const uint32_t codes[] = { 1, 0, 2, 0, 3, 0 };
        int seen = 0;
        for (int i = 0; i < 6; i++) {
            int rc = ydev_out_set(&d, codes[i]);
            if (rc == YDEV_OK && wait_lines(&rd, (int)(codes[i] & 1), (int)(codes[i] >> 1))) seen++;
        }
        check(seen == 6, "LINES: codes 1 0 2 0 3 0 as DTR, RTS", "%d of 6 reported", seen);
        int rc = ydev_out_pulse(&d, 2, YRT_MS(5));
        int on = rc == YDEV_OK && wait_lines(&rd, 0, 1);
        int off = on && wait_lines(&rd, 0, 0);
        check(on && off, "LINES: pulse on RTS", "rc %d, on %d, off %d", rc, on, off);
        say(&rd, "l 0\n");
        ydev_close(&d);
    }
    yser_close(&rd);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: board_check <device>\n");
        return 2;
    }
    const char* dev = argv[1];
    char l[128];

    /* enumeration */
    {
        yser_port_info ports[32];
        int n = yser_list_ports(ports, 32), hit = -1;
        for (int i = 0; i < n && i < 32; i++) if (strcmp(ports[i].name, dev) == 0) hit = i;
        check(hit >= 0, "yser_list_ports lists the port", "%d ports", n);
        if (hit >= 0)
            info("its info", "vid %04x pid %04x serial \"%s\" location \"%s\" driver \"%s\"",
                 ports[hit].vid, ports[hit].pid, ports[hit].serial_number, ports[hit].location, ports[hit].driver);
        yser_port_filter f;
        memset(&f, 0, sizeof f);
        f.name = dev;
        n = yser_find_ports(&f, ports, 32);
        check(n == 1 && strcmp(ports[0].name, dev) == 0, "yser_find_ports by name", "%d found", n);
    }

    yser_port p;
    yser_desc d;
    memset(&p, 0, sizeof p);
    memset(&d, 0, sizeof d);
    d.device = dev;
    int64_t t0 = yrt_now_ns();
    if (!yser_open(&p, &d)) {
        check(0, "yser_open", "%s", yser_error(&p));
        return 1;
    }
    check(1, "yser_open", "driver \"%s\", rts_via_dtr %d, worker %s, open %.1f ms (not a timing result)",
          p.driver, (int)p.rts_via_dtr, yrt_policy_name(p.async_policy), (double)(yrt_now_ns() - t0) / 1e6);
    quiet(&p);

    say(&p, "v\n");
    int n = read_line(&p, l, sizeof l, YRT_MS(500));
    check(n > 0 && strncmp(l, "V ysp-echo 2 ", 13) == 0, "the board is ysp_echo version 2", "\"%s\"", l);
    say(&p, "i\n");
    n = read_line(&p, l, sizeof l, YRT_MS(500));
    check(n > 0 && strncmp(l, "I ysp-line 1 ", 13) == 0, "\"i\" answers the line protocol", "\"%s\"", l);

    /* echoes */
    {
        int good = 0, order = 0;
        for (int i = 1; i <= 500; i++) {
            char c[24], want[16];
            snprintf(c, sizeof c, "e %d\n", i);
            say(&p, c);
            uint8_t r[35];
            int got = yser_read(&p, r, 35, YRT_MS(500), YSER_READ_ALL);
            snprintf(want, sizeof want, "R %010d ", i);
            if (got == 35 && r[34] == '\n' && memcmp(r, want, 13) == 0) {
                good++;
                if (strtoul((const char*)r + 24, NULL, 10) >= strtoul((const char*)r + 13, NULL, 10)) order++;
            } else {
                sleep_ms(20);
                yser_purge(&p, YSER_PURGE_RX);
            }
        }
        check(good == 500 && order == 500, "500 echoes, length, seq, stamps in order", "%d good, %d in order", good, order);
    }

    /* DTR and RTS, as the board sees them and as the driver reports them */
    {
        int seen = 0, lines;
        static const int seq[][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 }, { 0, 0 } };
        yser_set_dtr(&p, false);
        yser_set_rts(&p, false);
        sleep_ms(20);
        say(&p, "l 1\n");
        (void)wait_lines(&p, 0, 0);
        for (int i = 1; i < 5; i++) {
            int rd = yser_set_dtr(&p, seq[i][0] != 0);
            int rr = yser_set_rts(&p, seq[i][1] != 0);
            if (rd == 0 && rr == 0 && wait_lines(&p, seq[i][0], seq[i][1])) seen++;
        }
        say(&p, "l 0\n");
        check(seen == 4, "DTR and RTS changes reach the board", "%d of 4 states reported", seen);
        lines = yser_get_lines(&p);
        check(lines >= 0, "yser_get_lines on the driver", "%d (%s)", lines,
              lines >= 0 ? "the input lines' mask" : yser_strerror(lines));
        yser_set_dtr(&p, true);
        yser_set_rts(&p, true);
        quiet(&p);
    }

    /* bytes and pulses */
    {
        int v[16], want[7] = { 0xA5, 0x55, 0x00, 0x66, 0x01, 0x77, 0x02 };
        say(&p, "b 7 1000\n");
        sleep_ms(30);
        int r1 = yser_write_byte(&p, 0xA5);
        int r2 = yser_pulse(&p, 0x55, 0x00, YRT_MS(2));
        int r3 = yser_pulse_async(&p, 0x66, 0x01, YRT_MS(2));
        sleep_ms(20);
        int r4 = yser_pulse_async(&p, 0x77, 0x02, 0);
        int k = read_bytes(&p, v, 16);
        char s[128];
        fmt_bytes(s, sizeof s, v, k > 0 ? k : 0);
        check(r1 == 1 && r2 == 2 && r3 == 1 && r4 == 1 && k == 7 && same(v, want, 7),
              "write_byte, pulse, pulse_async reach the board", "rc %d %d %d %d, board saw %s", r1, r2, r3, r4, s);
    }

    /* exclusive */
    {
        yser_port a, b;
        yser_desc e;
        memset(&a, 0, sizeof a);
        memset(&b, 0, sizeof b);
        memset(&e, 0, sizeof e);
        e.device = dev;
        e.exclusive = true;
        quiet(&p);
        yser_close(&p);
        int oa = yser_open(&a, &e);
        int ob = yser_open(&b, &e);
        check(oa && !ob, "a second exclusive open is refused", "first %d, second %d: %s", oa, ob,
              ob ? "(opened)" : yser_error(&b));
        if (ob) yser_close(&b);
        yser_desc plain;
        memset(&plain, 0, sizeof plain);
        plain.device = dev;
        int oc = yser_open(&b, &plain);
        info("a plain open beside an exclusive one", "%s", oc ? "opened (TIOCEXCL let it through)" : yser_error(&b));
        if (oc) yser_close(&b);
        if (oa) yser_close(&a);
    }

    /* a nonstandard rate: USB CDC ignores the rate, the driver may not */
    {
        yser_port a;
        yser_desc e;
        memset(&a, 0, sizeof a);
        memset(&e, 0, sizeof e);
        e.device = dev;
        e.baud = 250000;
        if (yser_open(&a, &e)) {
            say(&a, "x\nv\n");
            n = read_line(&a, l, sizeof l, YRT_MS(500));
            info("open at 250000 baud", "opened; \"v\" answers \"%s\"", l);
            yser_close(&a);
        } else {
            info("open at 250000 baud", "%s", yser_error(&a));
        }
    }

    check_device(dev);
    printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "all checks passed", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
