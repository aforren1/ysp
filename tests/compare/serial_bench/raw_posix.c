/* raw_posix.c - the Linux floor for serial_bench: the same echo round trip and
 * the same stream as serial_bench.m and serial_bench_py, with open, termios,
 * read, write and poll only. No ysp code. The counterpart of raw_win32.c: it
 * bounds what any API can do on this port and driver, so the API numbers can
 * be read as "floor + API cost".
 *
 * Needs the echo board (firmware/ysp_echo) on the port.
 *
 *   raw_posix /dev/ttyACM0 [out_dir] [n_rt] [flags]
 *
 * flags: "rt" skips the streams; "rev" runs the modes in reverse order.
 *
 * Build:
 *   cc -O2 -Wall -o raw_posix raw_posix.c -lm
 *
 * Modes, each with its own open:
 *   block  a blocking descriptor, raw termios with VMIN = 1 and VTIME = 0:
 *          read() returns as soon as one byte is there, in the kernel. A
 *          timeout comes from poll() before the read.
 *   poll   O_NONBLOCK, poll() for POLLIN, then read() of what is buffered.
 *          ysp/serial.h waits this way.
 *
 * Output: the metric names and statistics of raw_win32.c and serial_bench's
 * summary.txt (median, p99 at rank ceil(0.99 n), max, mean, in
 * microseconds), on stdout and in <out_dir>/raw_summary.txt, plus raw CSVs.
 * The clock is CLOCK_MONOTONIC, the clock of ysp/rt.h on Linux. The process
 * runs at normal priority.
 *
 * Public domain / MIT-0, as the rest of ysp. */
#define _DEFAULT_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define RT_LEN 35
#define ST_LEN 26
#define STREAM_HZ 1000
#define STREAM_S 10
#define N_PROBE 200

static FILE* g_sum;

static double now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e6 + (double)ts.tv_nsec / 1e3;
}

static void spin_until(double t) { while (now_us() < t) { } }

static void sleep_ms(int ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) { }
}

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
    if (g_sum) fprintf(g_sum, "%s\n", line);
    free(s);
}

static void report1(const char* api, const char* part, const char* metric, double v, const char* note) {
    report(api, part, metric, &v, 1, note);
}

/* --- the port ----------------------------------------------------------- */

typedef struct {
    int fd;
    int nonblock;       /* poll mode */
    const char* name;
} port;

static void port_open(port* p, const char* dev, const char* mode) {
    struct termios t;
    memset(p, 0, sizeof *p);
    p->name = mode;
    p->nonblock = strcmp(mode, "poll") == 0;
    p->fd = open(dev, O_RDWR | O_NOCTTY | O_CLOEXEC | (p->nonblock ? O_NONBLOCK : 0));
    if (p->fd < 0) die("open");
    if (tcgetattr(p->fd, &t) != 0) die("tcgetattr");
    cfmakeraw(&t);
    t.c_cflag |= CLOCAL | CREAD | HUPCL;
    t.c_cflag &= ~(tcflag_t)CRTSCTS;
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    cfsetispeed(&t, B115200);
    cfsetospeed(&t, B115200);
    if (tcsetattr(p->fd, TCSANOW, &t) != 0) die("tcsetattr");
    tcflush(p->fd, TCIOFLUSH);
}

static void port_close(port* p) { close(p->fd); }

static void port_write(port* p, const void* buf, int len) {
    const char* b = (const char*)buf;
    while (len > 0) {
        ssize_t w = write(p->fd, b, (size_t)len);
        if (w < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN) {
                struct pollfd pf = { p->fd, POLLOUT, 0 };
                poll(&pf, 1, 1000);
                continue;
            }
            die("write");
        }
        b += w;
        len -= (int)w;
    }
}

/* Whatever is there, waiting up to timeout_ms for the first byte. Returns
 * the byte count, 0 on timeout. */
static int port_read(port* p, unsigned char* buf, int cap, int timeout_ms) {
    struct pollfd pf = { p->fd, POLLIN, 0 };
    ssize_t r;
    if (p->nonblock) {
        r = read(p->fd, buf, (size_t)cap);   /* bytes already buffered */
        if (r > 0) return (int)r;
        if (poll(&pf, 1, timeout_ms) <= 0) return 0;
        r = read(p->fd, buf, (size_t)cap);
        return r > 0 ? (int)r : 0;
    }
    /* VMIN = 1 alone would wait forever; poll() gives the timeout, and the
     * read then returns what is there */
    if (poll(&pf, 1, timeout_ms) <= 0) return 0;
    r = read(p->fd, buf, (size_t)cap);
    return r > 0 ? (int)r : 0;
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
    tcflush(p->fd, TCIFLUSH);
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

/* --- the clock fit, as sb_fit.m ----------------------------------------- */

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

/* gap_us: the spacing of the exchanges, 0 to gap_us at random. The stream's
 * probes use 2 to 3 ms: 0 to 1 ms puts 200 of them in one 200 ms bucket of
 * the fit, which then has 2 points and no slope (as serial_bench_py). */
static void rt_run(port* p, rtrun* X, int n, double seq0, double gap_lo_us, double gap_hi_us) {
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
            tcflush(p->fd, TCIFLUSH);
            continue;
        }
        if (isnan(ref)) ref = rx;
        int k = X->n++;
        X->t0[k] = t0; X->t1[k] = t1; X->t2[k] = t2;
        X->rx[k] = unwrap(rx, ref);
        X->tx[k] = unwrap(tx, ref);
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
    char path[512];
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
    struct timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
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
    rt_run(p, &P, N_PROBE, 1e6, 2000, 3000);
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
    rt_run(p, &P2, N_PROBE, 2e6, 2000, 3000);
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
    char note[200], m[64];
    snprintf(note, sizeof note, "of %d lines (count); %s", nlines,
             p->nonblock ? "poll(POLLIN), then read() of what is buffered"
                         : "blocking read(), VMIN 1, first byte waits in the kernel");
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

    /* map board stamps: unwrap the line stamps against the first probe's rx
     * reference (the probes were unwrapped against their own first rx) */
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
            char path[512];
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
        fprintf(stderr, "usage: raw_posix /dev/ttyACMn [out_dir] [n_rt] [rt,rev]\n");
        return 2;
    }
    const char* dev = argv[1];
    const char* dir = argc >= 3 ? argv[2] : NULL;
    int n_rt = argc >= 4 ? atoi(argv[3]) : 1000;
    int rt_only = argc >= 5 && strstr(argv[4], "rt") != NULL;
    int rev = argc >= 5 && strstr(argv[4], "rev") != NULL;
    if (dir) {
        mkdir(dir, 0777);
        char path[512];
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

    {
        int n = 100000;
        double* d = (double*)malloc((size_t)n * sizeof(double));
        for (int i = 0; i < n; i++) { double a = now_us(); d[i] = now_us() - a; }
        report("raw", "clock", "clock_pair", d, n, "two back-to-back clock_gettime(CLOCK_MONOTONIC) reads");
        free(d);
    }

    static const char* modes[] = { "block", "poll" };
    for (int mj = 0; mj < 2; mj++) {
        int mi = rev ? 1 - mj : mj;
        port p;
        port_open(&p, dev, modes[mi]);
        char api[16];
        snprintf(api, sizeof api, "raw_%s", modes[mi]);
        rtrun X;
        rt_alloc(&X, n_rt);
        rt_run(&p, &X, n_rt, 0, 0, 1000);
        rt_report(api, "block", &X);
        rt_csv(dir, api, "block", &X);
        rt_free(&X);
        if (!rt_only) stream_run(&p, api, dir);
        port_close(&p);
    }
    if (g_sum) fclose(g_sum);
    return 0;
}
