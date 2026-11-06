/* raw_posix.c - the POSIX floor for serial_bench, the sibling of raw_win32.c:
 * the same echo round trip and the same stream as serial_bench.m and
 * serial_bench_py, with open(), termios, write() and read() only, plus one
 * mode that makes the same exchanges through ysp/serial.h, so the header and
 * the floor are measured by one program, one method and one clock. No
 * MATLAB.
 *
 * Needs the echo board (firmware/ysp_echo) on the port.
 *
 *   raw_posix /dev/cu.usbmodemNNN [out_dir] [n_rt] [flags]   (macOS)
 *   raw_posix /dev/ttyACM0 [out_dir] [n_rt] [flags]          (Linux)
 *
 * flags: "rt" skips the streams; "rev" runs the modes in reverse order (the
 * first mode of a run measured slower on Windows, docs/serial.md, Audit).
 *
 * Build (macOS or Linux):
 *   cc -O2 -Wall -I../../../include -o raw_posix raw_posix.c -lpthread -lm
 *
 * Modes, each with its own open:
 *   block   a blocking descriptor, raw termios with VMIN = 0 and VTIME = 10:
 *           read() returns as soon as one byte is there, or after 1 s, in
 *           the kernel. write() blocks until the driver takes the bytes.
 *           The plain termios way, as raw_win32's "sync".
 *   select  O_NONBLOCK, select() for readable, then read() of what is
 *           there; write(), with select() for writable on EAGAIN. This is
 *           what ysp/serial.h does on macOS (POSIX WAITS in the header).
 *   poll    O_NONBLOCK, poll(POLLIN), then read(); not on macOS. This is
 *           what ysp/serial.h does on Linux.
 *   kqueue  O_NONBLOCK, kevent(EVFILT_READ), then read(); macOS and the
 *           BSDs only. Apple's poll(2) documents that it does not support
 *           devices; kqueue is the other wait the kernel offers.
 *   ysp     yser_write() and yser_read(YSER_READ_ANY) on a yser_port with
 *           the defaults. Not a floor: the header under test, on the
 *           floor's method.
 *
 * The 2026-10-12 Linux runs used an earlier, Linux-only version of this
 * file: its block mode had VMIN = 1, VTIME = 0 and a poll() for the
 * timeout, and it had no select or ysp mode (docs/serial.md, Results on
 * Linux).
 *
 * Output: the metric names and statistics of raw_win32.c and serial_bench's
 * summary.txt (median, p99 at rank ceil(0.99 n), max, mean, in
 * microseconds), on stdout and in <out_dir>/raw_summary.txt, plus raw CSVs.
 * The clock is yrt_now_ns() (mach_absolute_time() on macOS, CLOCK_MONOTONIC
 * on Linux) for every mode. The one-way split uses the clock fit of
 * sb_fit.m: the best exchange of each 200 ms, symmetric delays assumed.
 *
 * The process runs at normal priority, as MATLAB does in the bench.
 *
 * Public domain / MIT-0, as the rest of ysp. */
#define YSP_SERIAL_IMPLEMENTATION
#include "ysp/serial.h"   /* first: it sets _DEFAULT_SOURCE for glibc */
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>
#if defined(__APPLE__) || defined(__FreeBSD__)
#include <sys/event.h>
#define RAW_HAVE_KQUEUE 1
#endif
/* Apple's poll(2) does not support devices, so the mode is left out there. */
#if !defined(__APPLE__)
#include <poll.h>
#define RAW_HAVE_POLL 1
#endif

#define RT_LEN 35
#define ST_LEN 26
#define STREAM_HZ 1000
#define STREAM_S 10
#define N_PROBE 200

static FILE* g_sum;

static double now_us(void) { return (double)yrt_now_ns() / 1e3; }

static void spin_until(double t) { while (now_us() < t) { } }

static void sleep_ms(int ms) { (void)yrt_sleep_ns((int64_t)ms * 1000000); }

static void die(const char* what) {
    fprintf(stderr, "raw_posix: %s failed: %s\n", what, strerror(errno));
    exit(1);
}

/* --- statistics, same definitions as sb_stats.m ------------------------- */

static int cmp_d(const void* a, const void* b) {
    double x = *(const double*)a, y = *(const double*)b;
    return (x > y) - (x < y);
}

static void report(const char* api, const char* part, const char* metric,
                   const double* v, int n, const char* note) {
    double* s = (double*)malloc((size_t)(n > 0 ? n : 1) * sizeof(double));
    int m = 0;
    double sum = 0;
    for (int i = 0; i < n; i++) if (!isnan(v[i])) { s[m++] = v[i]; sum += v[i]; }
    char line[512];
    if (m == 0) {
        snprintf(line, sizeof line, "%-10s %-7s %-34s %6d %10s %10s %10s %10s  %s",
                 api, part, metric, 0, "NaN", "NaN", "NaN", "NaN", note);
    } else {
        qsort(s, (size_t)m, sizeof(double), cmp_d);
        double med = (m % 2) ? s[m / 2] : 0.5 * (s[m / 2 - 1] + s[m / 2]);
        int k = (int)ceil(0.99 * m);
        if (k < 1) k = 1;
        snprintf(line, sizeof line, "%-10s %-7s %-34s %6d %10.4g %10.4g %10.4g %10.4g  %s",
                 api, part, metric, m, med, s[k - 1], s[m - 1], sum / m, note);
    }
    puts(line);
    fflush(stdout);
    if (g_sum) fprintf(g_sum, "%s\n", line);
    free(s);
}

static void report1(const char* api, const char* part, const char* metric, double v, const char* note) {
    report(api, part, metric, &v, 1, note);
}

/* --- the port ----------------------------------------------------------- */

enum { M_BLOCK, M_SELECT, M_POLL, M_KQUEUE, M_YSP, M_COUNT };
static const char* const mode_name[M_COUNT] = { "block", "select", "poll", "kqueue", "ysp" };

typedef struct {
    int mode;
    int fd;
    int kq;
    yser_port sp;
} port;

static void port_open(port* p, const char* dev, int mode) {
    memset(p, 0, sizeof *p);
    p->mode = mode;
    p->fd = p->kq = -1;
    if (mode == M_YSP) {
        yser_desc d;
        memset(&d, 0, sizeof d);
        d.device = dev;
        if (!yser_open(&p->sp, &d)) {
            fprintf(stderr, "raw_posix: open %s: %s\n", dev, yser_error(&p->sp));
            exit(1);
        }
        yser_purge(&p->sp, YSER_PURGE_RX | YSER_PURGE_TX);
        return;
    }
    p->fd = open(dev, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (p->fd < 0) die("open");
    if (mode == M_BLOCK && fcntl(p->fd, F_SETFL, 0) != 0) die("fcntl");
    struct termios t;
    if (tcgetattr(p->fd, &t) != 0) die("tcgetattr");
    cfmakeraw(&t);
    t.c_cflag |= (tcflag_t)(CLOCAL | CREAD);
    t.c_cflag &= (tcflag_t)~(CRTSCTS | HUPCL);
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = mode == M_BLOCK ? 10 : 0;
    cfsetispeed(&t, B115200);
    cfsetospeed(&t, B115200);
    if (tcsetattr(p->fd, TCSANOW, &t) != 0) die("tcsetattr");
    int lines = TIOCM_DTR | TIOCM_RTS;
    ioctl(p->fd, TIOCMBIS, &lines);
#if defined(RAW_HAVE_KQUEUE)
    if (mode == M_KQUEUE) {
        struct kevent ev;
        p->kq = kqueue();
        if (p->kq < 0) die("kqueue");
        EV_SET(&ev, p->fd, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, NULL);
        if (kevent(p->kq, &ev, 1, NULL, 0, NULL) != 0) die("kevent(EV_ADD)");
    }
#endif
    tcflush(p->fd, TCIOFLUSH);
}

static void port_close(port* p) {
    if (p->mode == M_YSP) { yser_close(&p->sp); return; }
    if (p->kq >= 0) close(p->kq);
    close(p->fd);
}

static void port_purge(port* p) {
    if (p->mode == M_YSP) yser_purge(&p->sp, YSER_PURGE_RX);
    else tcflush(p->fd, TCIFLUSH);
}

static void port_write(port* p, const void* buf, int len) {
    if (p->mode == M_YSP) {
        if (yser_write(&p->sp, buf, len) != len) die("yser_write");
        return;
    }
    int done = 0;
    while (done < len) {
        ssize_t w = write(p->fd, (const char*)buf + done, (size_t)(len - done));
        if (w > 0) { done += (int)w; continue; }
        if (w < 0 && errno == EINTR) continue;
        if (w < 0 && errno == EAGAIN) {
            fd_set s;
            FD_ZERO(&s);
            FD_SET(p->fd, &s);
            select(p->fd + 1, NULL, &s, NULL, NULL);
            continue;
        }
        die("write");
    }
}

/* Whatever is there, waiting up to timeout_ms for the first byte. Returns the
 * byte count, 0 on timeout. */
static int port_read(port* p, unsigned char* buf, int cap, int timeout_ms) {
    if (p->mode == M_YSP) {
        int r = yser_read(&p->sp, buf, cap, YRT_MS(timeout_ms), YSER_READ_ANY);
        if (r < 0) die("yser_read");
        return r;
    }
    if (p->mode == M_BLOCK) {
        ssize_t r = read(p->fd, buf, (size_t)cap);   /* VTIME bounds the wait */
        if (r < 0 && errno != EINTR) die("read");
        return r < 0 ? 0 : (int)r;
    }
    for (;;) {
        ssize_t r = read(p->fd, buf, (size_t)cap);
        if (r > 0) return (int)r;
        if (r < 0 && errno != EAGAIN && errno != EINTR) die("read");
        if (p->mode == M_SELECT) {
            fd_set s;
            struct timeval tv;
            FD_ZERO(&s);
            FD_SET(p->fd, &s);
            tv.tv_sec = timeout_ms / 1000;
            tv.tv_usec = (timeout_ms % 1000) * 1000;
            int k = select(p->fd + 1, &s, NULL, NULL, &tv);
            if (k == 0) return 0;
            if (k < 0 && errno != EINTR) die("select");
        }
#if defined(RAW_HAVE_POLL)
        else if (p->mode == M_POLL) {
            struct pollfd pf = { p->fd, POLLIN, 0 };
            int k = poll(&pf, 1, timeout_ms);
            if (k == 0) return 0;
            if (k < 0 && errno != EINTR) die("poll");
        }
#endif
#if defined(RAW_HAVE_KQUEUE)
        else {
            struct kevent ev;
            struct timespec ts;
            ts.tv_sec = timeout_ms / 1000;
            ts.tv_nsec = (long)(timeout_ms % 1000) * 1000000L;
            int k = kevent(p->kq, NULL, 0, &ev, 1, &ts);
            if (k == 0) return 0;
            if (k < 0 && errno != EINTR) die("kevent");
        }
#endif
    }
}

/* Read exactly n bytes or give up after timeout_ms in total. */
static int port_read_all(port* p, unsigned char* buf, int n, double timeout_ms) {
    int got = 0;
    double end = now_us() + timeout_ms * 1000.0;
    while (got < n) {
        double left = (end - now_us()) / 1000.0;
        if (left <= 0) break;
        int r = port_read(p, buf + got, n - got, (int)(left + 1));
        if (r == 0) break;
        got += r;
    }
    return got;
}

static void port_flush_board(port* p) {
    port_write(p, "x\n", 2);
    sleep_ms(50);
    port_purge(p);
}

/* The bytes-available query, the call behind serialport's NumBytesAvailable
 * and ysp_serial('available'). */
static int port_available(port* p) {
    if (p->mode == M_YSP) return yser_available(&p->sp);
    int n = 0;
    ioctl(p->fd, FIONREAD, &n);
    return n;
}

/* --- parsing ------------------------------------------------------------ */

static double digits10(const unsigned char* s) {
    double v = 0;
    for (int i = 0; i < 10; i++) {
        if (s[i] < '0' || s[i] > '9') return NAN;
        v = v * 10 + (s[i] - '0');
    }
    return v;
}

static int parse_reply(const unsigned char* r, int n, double seq, double* rx, double* tx) {
    if (n != RT_LEN || r[0] != 'R' || r[34] != '\n') return 0;
    if (digits10(r + 2) != seq) return 0;
    *rx = digits10(r + 13);
    *tx = digits10(r + 24);
    return !isnan(*rx) && !isnan(*tx);
}

static double unwrap(double v, double ref) {
    double d = fmod(v - ref, 4294967296.0);
    if (d < 0) d += 4294967296.0;
    return ref + d;
}

/* --- the clock fit, as sb_fit.m and raw_win32.c ------------------------- */

typedef struct { int ok, npts; double x0, y0, slope, unc, span_s; } fit;

static fit fit_make(const double* t0, const double* t2, const double* rx, const double* tx, int n) {
    fit F;
    memset(&F, 0, sizeof F);
    F.slope = 1;
    if (n < 2) return F;
    double* x = (double*)malloc((size_t)n * sizeof(double));
    double* y = (double*)malloc((size_t)n * sizeof(double));
    double minpath = 1e300;
    int m = 0;
    int i = 0;
    while (i < n) {
        double b = floor((t0[i] - t0[0]) / 200e3);
        int best = i;
        double bp = (t2[i] - t0[i]) - (tx[i] - rx[i]);
        int j = i + 1;
        while (j < n && floor((t0[j] - t0[0]) / 200e3) == b) {
            double pj = (t2[j] - t0[j]) - (tx[j] - rx[j]);
            if (pj < bp) { bp = pj; best = j; }
            j++;
        }
        x[m] = (rx[best] + tx[best]) / 2;
        y[m] = (t0[best] + t2[best]) / 2;
        if (bp < minpath) minpath = bp;
        m++;
        i = j;
    }
    F.npts = m;
    F.x0 = x[m - 1];
    F.span_s = (y[m - 1] - y[0]) / 1e6;
    if (F.span_s >= 2 && m >= 3) {
        double sx = 0, sy = 0, sxx = 0, sxy = 0;
        for (int k = 0; k < m; k++) {
            double xx = x[k] - F.x0;
            sx += xx; sy += y[k]; sxx += xx * xx; sxy += xx * y[k];
        }
        F.slope = (m * sxy - sx * sy) / (m * sxx - sx * sx);
        F.y0 = (sy - F.slope * sx) / m;
    } else {
        double* d = (double*)malloc((size_t)m * sizeof(double));
        for (int k = 0; k < m; k++) d[k] = y[k] - (x[k] - F.x0);
        qsort(d, (size_t)m, sizeof(double), cmp_d);
        F.y0 = (m % 2) ? d[m / 2] : 0.5 * (d[m / 2 - 1] + d[m / 2]);
        free(d);
    }
    F.unc = minpath / 2;
    F.ok = 1;
    free(x);
    free(y);
    return F;
}

static double fit_map(const fit* F, double dev) { return F->y0 + F->slope * (dev - F->x0); }

/* --- the round trip ----------------------------------------------------- */

typedef struct { double *t0, *t1, *t2, *rx, *tx; int n; int failed; } rtrun;

static void rt_alloc(rtrun* X, int n) {
    X->t0 = (double*)calloc((size_t)n, sizeof(double));
    X->t1 = (double*)calloc((size_t)n, sizeof(double));
    X->t2 = (double*)calloc((size_t)n, sizeof(double));
    X->rx = (double*)calloc((size_t)n, sizeof(double));
    X->tx = (double*)calloc((size_t)n, sizeof(double));
    X->n = 0;
    X->failed = 0;
}

static void rt_free(rtrun* X) { free(X->t0); free(X->t1); free(X->t2); free(X->rx); free(X->tx); }

/* gap_lo_us to gap_hi_us: the spacing of the exchanges, uniform at random.
 * The stream's probes use 2 to 3 ms: at 0 to 1 ms all 200 fell in one 200 ms
 * bucket of the fit, which then had 2 points and no slope (docs/serial.md,
 * Stream fits of the raw floors). */
static void rt_run(port* p, rtrun* X, int n, double seq0,
                   double gap_lo_us, double gap_hi_us, double* avail) {
    unsigned char cmd[16], r[64];
    double ref = NAN;
    port_flush_board(p);
    for (int i = 0; i < n; i++) {
        double seq = seq0 + i + 1;
        snprintf((char*)cmd, sizeof cmd, "e %010.0f\n", seq);
        spin_until(now_us() + gap_lo_us + (double)rand() / RAND_MAX * (gap_hi_us - gap_lo_us)); /* leave the USB frame phase */
        double t0 = now_us();
        port_write(p, cmd, 13);
        double t1 = now_us();
        int got = port_read_all(p, r, RT_LEN, 1000);
        double t2 = now_us();
        double rx, tx;
        if (!parse_reply(r, got, seq, &rx, &tx)) {
            X->failed++;
            sleep_ms(20);
            port_purge(p);
            continue;
        }
        if (isnan(ref)) ref = rx;
        int k = X->n++;
        X->t0[k] = t0; X->t1[k] = t1; X->t2[k] = t2;
        X->rx[k] = unwrap(rx, ref);
        X->tx[k] = unwrap(tx, ref);
        /* the bytes-available query, after the exchange so it does not
         * delay it */
        if (avail) {
            double a = now_us();
            (void)port_available(p);
            avail[k] = now_us() - a;
        }
    }
}

static void rt_report(const char* api, const char* tag, rtrun* X) {
    int n = X->n;
    double* v = (double*)malloc((size_t)(n > 0 ? n : 1) * sizeof(double));
    char m[64], note[160];
#define ROW(name, expr, nt) do { for (int i = 0; i < n; i++) v[i] = (expr); \
        snprintf(m, sizeof m, "%s_%s", tag, name); report(api, "rt", m, v, n, nt); } while (0)
    snprintf(m, sizeof m, "%s_failed", tag);
    report1(api, "rt", m, X->failed, "exchanges with a missing or wrong answer (count)");
    ROW("rtt", X->t2[i] - X->t0[i], "write call start to read call return");
    ROW("write_call", X->t1[i] - X->t0[i], "write call duration inside the exchange");
    ROW("write_ret_to_read_ret", X->t2[i] - X->t1[i], "write call return to read call return");
    fit F = fit_make(X->t0, X->t2, X->rx, X->tx, n);
    if (F.ok) {
        snprintf(note, sizeof note, "write call start to board read (+-%.0f us from the fit)", F.unc);
        ROW("host_to_board", fit_map(&F, X->rx[i]) - X->t0[i], note);
        snprintf(note, sizeof note, "write call return to board read (+-%.0f us)", F.unc);
        ROW("write_ret_to_board", fit_map(&F, X->rx[i]) - X->t1[i], note);
        snprintf(note, sizeof note, "board send stamp to read call return (+-%.0f us)", F.unc);
        ROW("board_to_host", X->t2[i] - fit_map(&F, X->tx[i]), note);
        snprintf(m, sizeof m, "%s_fit_unc", tag);
        report1(api, "rt", m, F.unc, "half the best path time: bound of the one-way split");
    }
#undef ROW
    free(v);
}

static void rt_csv(const char* dir, const char* api, const char* tag, rtrun* X) {
    if (!dir) return;
    char path[1024];
    snprintf(path, sizeof path, "%s/%s_rt_%s.csv", dir, api, tag);
    FILE* f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "t0_us,t1_us,t2_us,board_rx_us,board_tx_us\n");
    for (int i = 0; i < X->n; i++)
        fprintf(f, "%.3f,%.3f,%.3f,%.0f,%.0f\n", X->t0[i], X->t1[i], X->t2[i], X->rx[i], X->tx[i]);
    fclose(f);
}

/* --- the stream --------------------------------------------------------- */

static double cpu_s(void) {
    struct rusage u;
    getrusage(RUSAGE_SELF, &u);
    return (double)u.ru_utime.tv_sec + (double)u.ru_utime.tv_usec * 1e-6 +
           (double)u.ru_stime.tv_sec + (double)u.ru_stime.tv_usec * 1e-6;
}

static void stream_run(port* p, const char* api, const char* dir) {
    const int nlines = STREAM_HZ * STREAM_S;
    const int cap = nlines + 100;
    double* lk = (double*)calloc((size_t)cap, sizeof(double));     /* k */
    double* lt = (double*)calloc((size_t)cap, sizeof(double));     /* board t */
    double* lh = (double*)calloc((size_t)cap, sizeof(double));     /* host stamp */
    int* lr = (int*)calloc((size_t)cap, sizeof(int));              /* read index */
    int* per = (int*)calloc((size_t)cap * 4, sizeof(int));         /* lines per read */
    unsigned char line[64], buf[4096];
    int nl = 0, bad = 0, ll = 0, nreads = 0;
    long got = 0;

    rtrun P;
    rt_alloc(&P, 2 * N_PROBE);
    rt_run(p, &P, N_PROBE, 1e6, 2000, 3000, NULL);
    char cmd[32];
    int clen = snprintf(cmd, sizeof cmd, "s %d %d\n", STREAM_HZ, nlines);
    double c0 = cpu_s(), w0 = now_us();
    port_write(p, cmd, clen);
    double end = w0 + (STREAM_S + 2) * 1e6;
    while (got < (long)nlines * ST_LEN && now_us() < end) {
        int r = port_read(p, buf, (int)sizeof buf, 200);
        if (r <= 0) continue;
        double t = now_us();
        int ri = nreads++;
        got += r;
        for (int i = 0; i < r; i++) {
            if (ll < (int)sizeof line) line[ll] = buf[i];
            ll++;
            if (buf[i] != '\n') continue;
            if (ll == ST_LEN && line[0] == 'A' && nl < cap) {
                lt[nl] = digits10(line + 2);
                lk[nl] = digits10(line + 15);
                lh[nl] = t;
                lr[nl] = ri;
                if (ri < cap * 4) per[ri]++;
                nl++;
            } else {
                bad++;
            }
            ll = 0;
        }
    }
    double w1 = now_us(), c1 = cpu_s();
    port_flush_board(p);
    rtrun P2;
    rt_alloc(&P2, N_PROBE);
    rt_run(p, &P2, N_PROBE, 2e6, 2000, 3000, NULL);
    for (int i = 0; i < P2.n; i++) {
        P.t0[P.n] = P2.t0[i]; P.t2[P.n] = P2.t2[i];
        /* both blocks unwrap against their own first stamp; within a run the
         * board's micros() does not wrap (71.6 minutes), so they agree */
        P.rx[P.n] = P2.rx[i]; P.tx[P.n] = P2.tx[i];
        P.n++;
    }
    rt_free(&P2);

    /* missed lines: distinct k */
    char* seen = (char*)calloc((size_t)nlines + 2, 1);
    int nu = 0, dup = 0;
    for (int i = 0; i < nl; i++) {
        int k = (int)lk[i];
        if (k < 1 || k > nlines) continue;
        if (seen[k]) dup++; else { seen[k] = 1; nu++; }
    }
    static const char* const how[M_COUNT] = {
        "read() on a blocking descriptor, VMIN 0, VTIME 10",
        "select(), then read() of what is there",
        "poll(POLLIN), then read() of what is there",
        "kevent(EVFILT_READ), then read() of what is there",
        "yser_read(YSER_READ_ANY)"
    };
    char note[200], m[64];
    snprintf(note, sizeof note, "of %d lines (count); %s", nlines, how[p->mode]);
    report1(api, "stream", "line_lines_missed", nlines - nu, note);
    report1(api, "stream", "line_lines_duplicated", dup, "count");
    report1(api, "stream", "line_bad_lines", bad, "lines of the wrong length or kind (count)");
    {
        int nper = 0, coal = 0, tot = 0;
        double* pv = (double*)malloc((size_t)(nreads > 0 ? nreads : 1) * sizeof(double));
        for (int i = 0; i < nreads && i < cap * 4; i++) {
            if (per[i] <= 0) continue;
            pv[nper++] = per[i];
            tot += per[i];
            if (per[i] > 1) coal += per[i];
        }
        report(api, "stream", "line_lines_per_read", pv, nper, "lines completed by one read");
        report1(api, "stream", "line_coalesced_pct", tot ? 100.0 * coal / tot : NAN,
                "percent of lines that arrived in a read with another line");
        free(pv);
    }
    snprintf(note, sizeof note, "process CPU time / wall time over %.1f s, percent of one core", (w1 - w0) / 1e6);
    report1(api, "stream", "line_cpu_pct", 100.0 * (c1 - c0) / ((w1 - w0) / 1e6), note);

    fit F = fit_make(P.t0, P.t2, P.rx, P.tx, P.n);
    if (F.ok && nl > 0) {
        double ref = P.rx[0];
        double* v = (double*)malloc((size_t)nl * sizeof(double));
        for (int i = 0; i < nl; i++) v[i] = lh[i] - fit_map(&F, unwrap(lt[i], ref));
        snprintf(note, sizeof note, "host time after the read returned minus mapped board send stamp (+-%.0f us)", F.unc);
        report(api, "stream", "line_lat_host", v, nl, note);
        report1(api, "stream", "line_fit_unc", F.unc, "half the best probe path time");
        snprintf(m, sizeof m, "%d points over %.1f s", F.npts, F.span_s);
        report1(api, "stream", "line_fit_ppm", (1 / F.slope - 1) * 1e6, m);
        if (dir) {
            char path[1024];
            snprintf(path, sizeof path, "%s/%s_stream_line.csv", dir, api);
            FILE* f = fopen(path, "wb");
            if (f) {
                fprintf(f, "k,board_t_us,read_index,host_us,lat_us\n");
                for (int i = 0; i < nl; i++)
                    fprintf(f, "%.0f,%.0f,%d,%.3f,%.3f\n", lk[i], lt[i], lr[i], lh[i], v[i]);
                fclose(f);
            }
        }
        free(v);
    }
    free(seen); free(lk); free(lt); free(lh); free(lr); free(per);
    rt_free(&P);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: raw_posix /dev/cu.usbmodemNNN|/dev/ttyACMn [out_dir] [n_rt] [rt,rev]\n");
        return 2;
    }
    const char* dev = argv[1];
    const char* dir = argc >= 3 ? argv[2] : NULL;
    int n_rt = argc >= 4 ? atoi(argv[3]) : 1000;
    int rt_only = argc >= 5 && strstr(argv[4], "rt") != NULL;
    int rev = argc >= 5 && strstr(argv[4], "rev") != NULL;
    if (dir) {
        mkdir(dir, 0777);
        char path[1024];
        snprintf(path, sizeof path, "%s/raw_summary.txt", dir);
        g_sum = fopen(path, "wb");
    }
    srand(12345);
    char hdr[256];
    snprintf(hdr, sizeof hdr, "%-10s %-7s %-34s %6s %10s %10s %10s %10s  %s",
             "api", "part", "metric", "n", "median", "p99", "max", "mean", "note");
    puts("Times in microseconds unless the note says count or percent.");
    puts(hdr);
    if (g_sum) fprintf(g_sum, "Times in microseconds unless the note says count or percent.\n%s\n", hdr);

    /* clock cost, as the bench's clock part */
    {
        int n = 100000;
        double* d = (double*)malloc((size_t)n * sizeof(double));
        for (int i = 0; i < n; i++) { double a = now_us(); d[i] = now_us() - a; }
        report("raw", "clock", "clock_pair", d, n, "two back-to-back yrt_now_ns() reads");
        free(d);
    }

    int modes[M_COUNT], nm = 0;
    for (int m = 0; m < M_COUNT; m++) {
#if !defined(RAW_HAVE_KQUEUE)
        if (m == M_KQUEUE) continue;
#endif
#if !defined(RAW_HAVE_POLL)
        if (m == M_POLL) continue;
#endif
        modes[nm++] = m;
    }
    for (int mj = 0; mj < nm; mj++) {
        int mi = modes[rev ? nm - 1 - mj : mj];
        port p;
        port_open(&p, dev, mi);
        char api[16];
        snprintf(api, sizeof api, mi == M_YSP ? "ysp_c" : "raw_%s", mode_name[mi]);
        rtrun X;
        rt_alloc(&X, n_rt);
        double* av = (double*)calloc((size_t)n_rt, sizeof(double));
        rt_run(&p, &X, n_rt, 0, 0, 1000, av);
        rt_report(api, "block", &X);
        rt_csv(dir, api, "block", &X);
        report(api, "calls", "bytes_available", av, X.n,
               mi == M_YSP ? "yser_available()" : "ioctl(FIONREAD), the call behind a bytes-available query");
        free(av);
        rt_free(&X);
        if (!rt_only) stream_run(&p, api, dir);
        port_close(&p);
    }
    if (g_sum) fclose(g_sum);
    return 0;
}
