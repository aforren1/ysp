/* raw_win32.c - the Win32 floor for serial_bench: the same echo round trip and
 * the same stream as serial_bench.m, with CreateFile, WriteFile and ReadFile
 * only. No ysp code, no MATLAB. It bounds what any API can do on this port and
 * driver, so the MATLAB numbers can be read as "floor + API cost".
 *
 * Needs the echo board (firmware/ysp_echo) on the port.
 *
 *   raw_win32 COM3 [out_dir] [n_rt] [flags]
 *
 * flags: "rt" skips the streams; "rev" runs the modes in reverse order. The
 * first mode of a run measured 40 to 80 us slower than the same calls later
 * in the run (this laptop), so two runs in opposite orders bound that.
 *
 * Build (either):
 *   gcc -O2 -Wall -o raw_win32.exe raw_win32.c
 *   cl /O2 /W3 raw_win32.c
 *
 * Modes, each with its own CreateFile:
 *   sync  non-overlapped handle. Read timeouts ReadIntervalTimeout = MAXDWORD,
 *         ReadTotalTimeoutMultiplier = MAXDWORD, ReadTotalTimeoutConstant =
 *         timeout: ReadFile returns as soon as one byte is there, in the
 *         kernel, with no sleep loop. IOPort opens its port this way
 *         (non-overlapped); it differs in how it waits for the first byte.
 *   sync_txmask  sync, plus SetCommMask(EV_TXEMPTY), which IOPort's open
 *         sets for its blocking writes. It shows what the mask alone costs.
 *   sync_ptb  every driver call IOPort makes, in C: the EV_TXEMPTY mask, its
 *         per-byte COMMTIMEOUTS set before each read, ClearCommError after
 *         each read and write, a read of the exact count (35 or 26 bytes).
 *         It leaves out IOPort's sleep loop for the first byte, as
 *         BlockingBackgroundRead=1 does, and MATLAB.
 *   ovlFILE_FLAG_OVERLAPPED, the same timeouts, ReadFile and WriteFile
 *         completed with GetOverlappedResult. ysp/serial.h does this.
 *   wce   FILE_FLAG_OVERLAPPED, SetCommMask(EV_RXCHAR); WaitCommEvent, then a
 *         ReadFile that returns what is buffered (ReadIntervalTimeout =
 *         MAXDWORD, the rest 0).
 *
 * Output: the same metric names and statistics as serial_bench's summary.txt
 * (median, p99 at rank ceil(0.99 n), max, mean, in microseconds), on stdout
 * and in <out_dir>/raw_summary.txt, plus raw CSVs. The clock is
 * QueryPerformanceCounter. The one-way split uses the same clock fit as
 * sb_fit.m: the best exchange of each 200 ms, symmetric delays assumed.
 *
 * The process runs at normal priority, as MATLAB does in the bench.
 *
 * Public domain / MIT-0, as the rest of ysp. */
#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define RT_LEN 35
#define ST_LEN 26
#define STREAM_HZ 1000
#define STREAM_S 10
#define N_PROBE 200

static double g_qpf;
static FILE* g_sum;

static double now_us(void) {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1e6 / g_qpf;
}

static void spin_until(double t) { while (now_us() < t) { } }

static void die(const char* what) {
    fprintf(stderr, "raw_win32: %s failed: error %lu\n", what, (unsigned long)GetLastError());
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
    HANDLE h;
    int ovl;            /* FILE_FLAG_OVERLAPPED */
    int wce;            /* WaitCommEvent mode */
    int ptb;            /* IOPort's driver calls, without its first-byte sleep */
    HANDLE ev_rd, ev_wr;
    const char* name;
} port;

/* The timeouts IOPort sets before each blocking ReadFile
 * (PsychSerialWindowsGlue.c:1272-1274) with SendTimeout=1.0 and
 * ReceiveTimeout=1.0 (lines 622-623, 604): per-byte, so ReadFile returns when
 * the requested count is in or a 1 s gap passes. */
static void set_ptb_timeouts(port* p) {
    COMMTIMEOUTS t;
    memset(&t, 0, sizeof t);
    t.ReadIntervalTimeout = 1000;
    t.ReadTotalTimeoutMultiplier = 1000;
    t.WriteTotalTimeoutMultiplier = 1000;
    if (!SetCommTimeouts(p->h, &t)) die("SetCommTimeouts");
}

static void set_timeouts(port* p, DWORD first_byte_ms) {
    COMMTIMEOUTS t;
    if (p->ptb) { set_ptb_timeouts(p); return; }
    memset(&t, 0, sizeof t);
    t.ReadIntervalTimeout = MAXDWORD;
    if (!p->wce) {
        t.ReadTotalTimeoutMultiplier = MAXDWORD;
        t.ReadTotalTimeoutConstant = first_byte_ms;
    }
    t.WriteTotalTimeoutConstant = 1000;
    if (!SetCommTimeouts(p->h, &t)) die("SetCommTimeouts");
}

static void port_open(port* p, const char* dev, const char* mode) {
    char path[64];
    memset(p, 0, sizeof *p);
    p->name = mode;
    p->ovl = strncmp(mode, "sync", 4) != 0;    p->wce = strcmp(mode, "wce") == 0;
    p->ptb = strcmp(mode, "sync_ptb") == 0;
    snprintf(path, sizeof path, "\\\\.\\%s", dev);
    p->h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                       p->ovl ? FILE_FLAG_OVERLAPPED : FILE_ATTRIBUTE_NORMAL, NULL);
    if (p->h == INVALID_HANDLE_VALUE) die("CreateFile");
    DCB d;
    memset(&d, 0, sizeof d);
    d.DCBlength = sizeof d;
    if (!GetCommState(p->h, &d)) die("GetCommState");
    d.BaudRate = 115200;
    d.ByteSize = 8;
    d.Parity = NOPARITY;
    d.StopBits = ONESTOPBIT;
    d.fBinary = TRUE;
    d.fParity = FALSE;
    d.fDtrControl = DTR_CONTROL_ENABLE;
    d.fRtsControl = RTS_CONTROL_ENABLE;
    d.fOutxCtsFlow = d.fOutxDsrFlow = d.fOutX = d.fInX = FALSE;
    d.fNull = FALSE;
    d.fAbortOnError = FALSE;
    if (!SetCommState(p->h, &d)) die("SetCommState");
    set_timeouts(p, 1000);
    if (p->wce && !SetCommMask(p->h, EV_RXCHAR)) die("SetCommMask");
    /* IOPort's open sets this mask for its blocking writes, whatever the
     * write mode (PsychSerialWindowsGlue.c:453). */
    if ((strcmp(mode, "sync_txmask") == 0 || p->ptb) && !SetCommMask(p->h, EV_TXEMPTY)) die("SetCommMask");
    p->ev_rd = CreateEventA(NULL, TRUE, FALSE, NULL);
    p->ev_wr = CreateEventA(NULL, TRUE, FALSE, NULL);
    PurgeComm(p->h, PURGE_RXCLEAR | PURGE_TXCLEAR);
}

static void port_close(port* p) {
    CloseHandle(p->h);
    CloseHandle(p->ev_rd);
    CloseHandle(p->ev_wr);
}

static void port_write(port* p, const void* buf, int len) {
    DWORD done = 0;
    if (!p->ovl) {
        if (!WriteFile(p->h, buf, (DWORD)len, &done, NULL)) die("WriteFile");
        if (p->ptb) { /* PsychIOOSCheckError after every write (line 1177) */
            COMSTAT st;
            DWORD err;
            ClearCommError(p->h, &err, &st);
        }
        return;
    }
    OVERLAPPED o;
    memset(&o, 0, sizeof o);
    o.hEvent = p->ev_wr;
    ResetEvent(p->ev_wr);
    if (!WriteFile(p->h, buf, (DWORD)len, &done, &o)) {
        if (GetLastError() != ERROR_IO_PENDING) die("WriteFile");
        if (!GetOverlappedResult(p->h, &o, &done, TRUE)) die("GetOverlappedResult(write)");
    }
}

/* Whatever is there, waiting for the first byte (sync, ovl: in the read; wce:
 * in WaitCommEvent). Returns the byte count, 0 on timeout. */
static int port_read(port* p, unsigned char* buf, int cap, DWORD timeout_ms) {
    DWORD done = 0;
    if (!p->ovl) {
        if (p->ptb) set_ptb_timeouts(p);  /* before every read (line 1276) */
        if (!ReadFile(p->h, buf, (DWORD)cap, &done, NULL)) die("ReadFile");
        if (p->ptb) { /* and the error check after it (line 1382) */
            COMSTAT st;
            DWORD err;
            ClearCommError(p->h, &err, &st);
        }
        return (int)done;
    }
    OVERLAPPED o;
    if (p->wce) {
        double end = now_us() + timeout_ms * 1000.0;
        for (;;) {
            /* Bytes already buffered: no event will come for them. An
             * EV_RXCHAR latched by bytes an earlier read took ends the wait
             * at once, so the loop asks again rather than trusting it. */
            COMSTAT st;
            DWORD err;
            if (!ClearCommError(p->h, &err, &st)) die("ClearCommError");
            if (st.cbInQue > 0) break;
            double left = (end - now_us()) / 1000.0;
            if (left <= 0) return 0;
            DWORD mask = 0;
            memset(&o, 0, sizeof o);
            o.hEvent = p->ev_rd;
            ResetEvent(p->ev_rd);
            if (!WaitCommEvent(p->h, &mask, &o)) {
                if (GetLastError() != ERROR_IO_PENDING) die("WaitCommEvent");
                if (WaitForSingleObject(p->ev_rd, (DWORD)left + 1) != WAIT_OBJECT_0) {
                    SetCommMask(p->h, EV_RXCHAR); /* ends the pending wait */
                    GetOverlappedResult(p->h, &o, &done, TRUE);
                    return 0;
                }
                if (!GetOverlappedResult(p->h, &o, &done, TRUE)) die("GetOverlappedResult(wce)");
            }
        }
    }
    memset(&o, 0, sizeof o);
    o.hEvent = p->ev_rd;
    ResetEvent(p->ev_rd);
    if (!ReadFile(p->h, buf, (DWORD)cap, &done, &o)) {
        if (GetLastError() != ERROR_IO_PENDING) die("ReadFile");
        if (!GetOverlappedResult(p->h, &o, &done, TRUE)) die("GetOverlappedResult(read)");
    }
    return (int)done;
}

/* Read exactly n bytes or give up after timeout_ms in total. */
static int port_read_all(port* p, unsigned char* buf, int n, double timeout_ms) {
    int got = 0;
    double end = now_us() + timeout_ms * 1000.0;
    while (got < n) {
        double left = (end - now_us()) / 1000.0;
        if (left <= 0) break;
        int r = port_read(p, buf + got, n - got, (DWORD)(left + 1));
        if (r == 0) break;
        got += r;
    }
    return got;
}

static void port_flush_board(port* p) {
    port_write(p, "x\n", 2);
    Sleep(50);
    PurgeComm(p->h, PURGE_RXCLEAR | PURGE_RXABORT);
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

static void rt_run(port* p, rtrun* X, int n, double seq0, double* cce, double* sct) {
    unsigned char cmd[16], r[64];
    double ref = NAN;
    port_flush_board(p);
    for (int i = 0; i < n; i++) {
        double seq = seq0 + i + 1;
        snprintf((char*)cmd, sizeof cmd, "e %010.0f\n", seq);
        spin_until(now_us() + (double)rand() / RAND_MAX * 1000.0); /* leave the USB frame phase */
        double t0 = now_us();
        port_write(p, cmd, 13);
        double t1 = now_us();
        int got = port_read_all(p, r, RT_LEN, 1000);
        double t2 = now_us();
        double rx, tx;
        if (!parse_reply(r, got, seq, &rx, &tx)) {
            X->failed++;
            Sleep(20);
            PurgeComm(p->h, PURGE_RXCLEAR);
            continue;
        }
        if (isnan(ref)) ref = rx;
        int k = X->n++;
        X->t0[k] = t0; X->t1[k] = t1; X->t2[k] = t2;
        X->rx[k] = unwrap(rx, ref);
        X->tx[k] = unwrap(tx, ref);
        /* Cost of the two calls IOPort adds around each read and write, on
         * this driver, measured after the exchange so they do not delay it. */
        if (cce) {
            COMSTAT st;
            DWORD err;
            double a = now_us();
            ClearCommError(p->h, &err, &st);
            cce[k] = now_us() - a;
        }
        if (sct) {
            COMMTIMEOUTS t;
            GetCommTimeouts(p->h, &t);
            double a = now_us();
            SetCommTimeouts(p->h, &t);
            sct[k] = now_us() - a;
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
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s\\%s_rt_%s.csv", dir, api, tag);
    FILE* f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "t0_us,t1_us,t2_us,board_rx_us,board_tx_us\n");
    for (int i = 0; i < X->n; i++)
        fprintf(f, "%.3f,%.3f,%.3f,%.0f,%.0f\n", X->t0[i], X->t1[i], X->t2[i], X->rx[i], X->tx[i]);
    fclose(f);
}

/* --- the stream --------------------------------------------------------- */

static double cpu_s(void) {
    FILETIME c, e, k, u;
    GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    ULARGE_INTEGER kk, uu;
    kk.LowPart = k.dwLowDateTime; kk.HighPart = k.dwHighDateTime;
    uu.LowPart = u.dwLowDateTime; uu.HighPart = u.dwHighDateTime;
    return (double)(kk.QuadPart + uu.QuadPart) * 1e-7;
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
    rt_run(p, &P, N_PROBE, 1e6, NULL, NULL);
    char cmd[32];
    int clen = snprintf(cmd, sizeof cmd, "s %d %d\n", STREAM_HZ, nlines);
    double c0 = cpu_s(), w0 = now_us();
    port_write(p, cmd, clen);
    double end = w0 + (STREAM_S + 2) * 1e6;
    while (got < (long)nlines * ST_LEN && now_us() < end) {
        /* sync_ptb reads one line per call, as IOPort's Read(h, 1, 26). */
        int r = port_read(p, buf, p->ptb ? ST_LEN : (int)sizeof buf, 200);
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
    rt_run(p, &P2, N_PROBE, 2e6, NULL, NULL);
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
             !p->ovl ? "ReadFile on a non-overlapped handle, first byte waits in the kernel"
             : p->wce ? "WaitCommEvent(EV_RXCHAR), then ReadFile of what is buffered"
                      : "overlapped ReadFile, GetOverlappedResult");
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
            char path[MAX_PATH];
            snprintf(path, sizeof path, "%s\\%s_stream_line.csv", dir, api);
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
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_qpf = (double)f.QuadPart;
    if (argc < 2) {
        fprintf(stderr, "usage: raw_win32 COMn [out_dir] [n_rt]\n");
        return 2;
    }
    const char* dev = argv[1];
    const char* dir = argc >= 3 ? argv[2] : NULL;
    int n_rt = argc >= 4 ? atoi(argv[3]) : 1000;
    int rt_only = argc >= 5 && strstr(argv[4], "rt") != NULL;
    int rev = argc >= 5 && strstr(argv[4], "rev") != NULL;
    if (dir) {
        CreateDirectoryA(dir, NULL);
        char path[MAX_PATH];
        snprintf(path, sizeof path, "%s\\raw_summary.txt", dir);
        g_sum = fopen(path, "wb");
    }
    srand(12345);
    /* What Psychtoolbox's clock does to the process on its first call
     * (PsychTimeGlue.c:609 and :394-402), to test whether that, rather than
     * IOPort's driver calls, changes the timing. */
    if (getenv("RAW_TIMER_1MS")) {
        typedef UINT (WINAPI *tbp_fn)(UINT);
        HMODULE w = LoadLibraryA("winmm.dll");
        tbp_fn tbp = w ? (tbp_fn)(void*)GetProcAddress(w, "timeBeginPeriod") : NULL;
        if (tbp) tbp(1);
        puts("note: timeBeginPeriod(1)");
    }
    if (getenv("RAW_AFFINITY_ALL")) {
        DWORD_PTR pm, sm;
        GetProcessAffinityMask(GetCurrentProcess(), &pm, &sm);
        SetThreadAffinityMask(GetCurrentThread(), pm);
        puts("note: SetThreadAffinityMask(process mask)");
    }
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
        report("raw", "clock", "clock_pair", d, n, "two back-to-back QueryPerformanceCounter reads");
        free(d);
    }

    static const char* modes[] = { "sync", "sync_txmask", "sync_ptb", "ovl", "wce" };
    for (int mj = 0; mj < 5; mj++) {
        int mi = rev ? 4 - mj : mj;
        port p;
        port_open(&p, dev, modes[mi]);
        char api[16];
        snprintf(api, sizeof api, "raw_%s", modes[mi]);
        rtrun X;
        rt_alloc(&X, n_rt);
        double* cce = (double*)calloc((size_t)n_rt, sizeof(double));
        double* sct = (double*)calloc((size_t)n_rt, sizeof(double));
        rt_run(&p, &X, n_rt, 0, cce, sct);
        rt_report(api, "block", &X);
        rt_csv(dir, api, "block", &X);
        report(api, "calls", "clear_comm_error", cce, X.n, "ClearCommError(), the call behind BytesAvailable");
        report(api, "calls", "set_comm_timeouts", sct, X.n, "SetCommTimeouts(), unchanged values");
        free(cce); free(sct);
        rt_free(&X);
        if (!rt_only) stream_run(&p, api, dir);
        port_close(&p);
    }
    if (g_sum) fclose(g_sum);
    return 0;
}
