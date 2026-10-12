/* pulse_bench.c - trigger pulses on the serial path, timed by the board.
 *
 * Sends byte pulses (code, wait, 0) and DTR and RTS line pulses to the echo
 * board (firmware/ysp_echo, version 2) and records when the board saw each
 * edge. The board stamps each received byte ("b" mode) and each change of
 * DTR or RTS ("l" mode) with micros(); "e" probes between blocks of pulses
 * give the clock fit that maps those stamps to the host clock.
 * pulse_stats.py turns the CSV files into onset latency, width error, missed
 * pulses and call cost.
 *
 *   pulse_bench COM3 out_dir [n] [rev] [modes=a,b,...] [widths=1,2,5,10]
 *
 * n is the pulse count per cell (default 200). "rev" runs the cells in the
 * reverse order: a median depends on the open and on the position in the
 * run (docs/serial.md, Audit), so two runs in opposite orders bound that.
 *
 * Modes, each cell with its own open:
 *   ysp_pulse        yser_pulse(p, 0x55, 0x00, width)
 *   ysp_pulse_async  yser_pulse_async(p, 0x55, 0x00, width)
 *   ysp_dtr          yser_set_dtr(1), yrt_sleep_until(t + width), yser_set_dtr(0)
 *   ysp_rts          the same with yser_set_rts. ysp/serial.h has no line
 *                    pulse call, so the bench writes this one out.
 *   raw_byte         non-overlapped CreateFile; WriteFile of 0x55, the same
 *                    wait, WriteFile of 0x00
 *   raw_dtr, raw_rts EscapeCommFunction(SETDTR or SETRTS), the same wait,
 *                    CLRDTR or CLRRTS
 *   ysp_bytes_t0     yser_write_byte(0x55), yrt_sleep_until(call start +
 *                    width), yser_write_byte(0x00): the width timed from the
 *                    call start instead of the onset call's return
 *   ysp_dtr_t0       the same for yser_set_dtr
 * The raw modes stamp with QueryPerformanceCounter, converted with
 * yrt_ticks_to_ns (bit-identical to yrt_now_ns). Every wait is
 * yrt_sleep_until with YRT_DEFAULT_SPIN_NS, the wait yser_pulse uses, so the
 * modes differ in the transport calls only. The width starts after the onset
 * call returns, as in yser_pulse, except in the two _t0 modes.
 *
 * Pulses come in blocks of 20, 50 to 150 ms apart (uniform), with 10 "e"
 * probes before each block and after the last. Byte cells keep DTR and RTS
 * asserted (the yser_open default); line cells drive both low first, so a
 * pulse is a low-high-low on one line.
 *
 * Output in out_dir (appended, so two runs can share a folder):
 *   probes.csv  cell,mode,width_us,t0_ns,t2_ns,rx_us,tx_us
 *   pulses.csv  cell,mode,width_us,block,i,t_start_ns,t_ret_ns,t_off_ns,t_off_ret_ns,rc
 *               (t_off_* only where the bench makes the trailing call; -1 else)
 *   board.csv   cell,mode,width_us,block,kind,t_us,v
 *               (kind B: byte v; L: v = dtr + 2 rts; D: count; G: loop gap us)
 *   cells.csv   cell,mode,width_us,run,async_policy,open_ms
 *
 * Build (Visual C++, from a Developer Command Prompt):
 *   cl /O2 /W3 /I..\..\..\include pulse_bench.c
 * or MinGW-w64: gcc -O2 -Wall -I../../../include -o pulse_bench.exe pulse_bench.c
 *
 * Windows only, because the raw floor is Win32. The process runs at normal
 * priority, as a MATLAB or Python experiment does.
 *
 * Public domain / MIT-0, as the rest of ysp. */
#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define YSP_SERIAL_IMPLEMENTATION
#include "ysp/serial.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BLOCK 20
#define N_PROBE 10
#define ON_CODE 0x55
#define OFF_CODE 0x00

enum { M_YSP_PULSE, M_YSP_ASYNC, M_YSP_DTR, M_YSP_RTS, M_RAW_BYTE, M_RAW_DTR, M_RAW_RTS,
       M_YSP_BYTES_T0, M_YSP_DTR_T0, M_COUNT };
static const char* const mode_name[M_COUNT] = {
    "ysp_pulse", "ysp_pulse_async", "ysp_dtr", "ysp_rts", "raw_byte", "raw_dtr", "raw_rts",
    "ysp_bytes_t0", "ysp_dtr_t0"
};
static int is_raw(int m) { return m >= M_RAW_BYTE && m <= M_RAW_RTS; }
static int is_line(int m) {
    return m == M_YSP_DTR || m == M_YSP_RTS || m == M_RAW_DTR || m == M_RAW_RTS || m == M_YSP_DTR_T0;
}
static int is_rts(int m) { return m == M_YSP_RTS || m == M_RAW_RTS; }

typedef struct {
    int raw;
    yser_port sp;
    HANDLE h;
} port;

static void die(const char* what, int code) {
    fprintf(stderr, "pulse_bench: %s failed (%d, os error %lu)\n", what, code,
            (unsigned long)GetLastError());
    exit(1);
}

static int64_t qpc_ns(void) {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return yrt_ticks_to_ns((int64_t)c.QuadPart);
}

static int64_t pnow(const port* p) { return p->raw ? qpc_ns() : yrt_now_ns(); }

static void port_open(port* p, const char* dev, int raw) {
    memset(p, 0, sizeof *p);
    p->raw = raw;
    if (!raw) {
        yser_desc d = { 0 };
        d.device = dev;
        if (!yser_open(&p->sp, &d)) {
            fprintf(stderr, "pulse_bench: open %s: %s\n", dev, yser_error(&p->sp));
            exit(1);
        }
        return;
    }
    char path[64];
    snprintf(path, sizeof path, "\\\\.\\%s", dev);
    p->h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL, NULL);
    if (p->h == INVALID_HANDLE_VALUE) die("CreateFile", 0);
    DCB d;
    memset(&d, 0, sizeof d);
    d.DCBlength = sizeof d;
    if (!GetCommState(p->h, &d)) die("GetCommState", 0);
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
    if (!SetCommState(p->h, &d)) die("SetCommState", 0);
    /* ReadFile returns as soon as one byte is there, or after 100 ms. */
    COMMTIMEOUTS t;
    memset(&t, 0, sizeof t);
    t.ReadIntervalTimeout = MAXDWORD;
    t.ReadTotalTimeoutMultiplier = MAXDWORD;
    t.ReadTotalTimeoutConstant = 100;
    t.WriteTotalTimeoutConstant = 1000;
    if (!SetCommTimeouts(p->h, &t)) die("SetCommTimeouts", 0);
    SetupComm(p->h, 65536, 4096);
    PurgeComm(p->h, PURGE_RXCLEAR | PURGE_TXCLEAR);
}

static void port_close(port* p) {
    if (p->raw) CloseHandle(p->h);
    else yser_close(&p->sp);
}

static int port_write(port* p, const void* buf, int n) {
    if (!p->raw) return yser_write(&p->sp, buf, n);
    DWORD done = 0;
    if (!WriteFile(p->h, buf, (DWORD)n, &done, NULL)) return -1;
    return (int)done;
}

/* Whatever arrived, waiting up to 100 ms for the first byte. */
static int port_read(port* p, unsigned char* buf, int cap) {
    if (!p->raw) return yser_read(&p->sp, buf, cap, YRT_MS(100), YSER_READ_ANY);
    DWORD done = 0;
    if (!ReadFile(p->h, buf, (DWORD)cap, &done, NULL)) return -1;
    return (int)done;
}

static int port_read_exact(port* p, unsigned char* buf, int n, int64_t timeout_ns) {
    int got = 0;
    int64_t end = pnow(p) + timeout_ns;
    while (got < n && pnow(p) < end) {
        int r = port_read(p, buf + got, n - got);
        if (r < 0) return r;
        got += r;
    }
    return got;
}

static void port_purge(port* p) {
    if (p->raw) PurgeComm(p->h, PURGE_RXCLEAR);
    else yser_purge(&p->sp, YSER_PURGE_RX);
}

static int port_line(port* p, int rts, int on) {
    if (!p->raw) return rts ? yser_set_rts(&p->sp, on != 0) : yser_set_dtr(&p->sp, on != 0);
    DWORD fn = rts ? (on ? SETRTS : CLRRTS) : (on ? SETDTR : CLRDTR);
    return EscapeCommFunction(p->h, fn) ? 0 : -1;
}

static int port_byte(port* p, unsigned char v) { return port_write(p, &v, 1); }

static void cmd(port* p, const char* s) {
    if (port_write(p, s, (int)strlen(s)) != (int)strlen(s)) die("command write", 0);
}

/* --- parsing ------------------------------------------------------------ */

static int u10(const unsigned char* s, unsigned long* out) {
    unsigned long v = 0;
    for (int i = 0; i < 10; i++) {
        if (s[i] < '0' || s[i] > '9') return 0;
        v = v * 10u + (unsigned long)(s[i] - '0');
    }
    *out = v;
    return 1;
}

/* --- probes ------------------------------------------------------------- */

static unsigned long g_seq = 0;

static void probes(port* p, FILE* f, int cell, int mode, long wus, int n) {
    unsigned char c[16], r[64];
    for (int i = 0; i < n; i++) {
        unsigned long seq = ++g_seq, a, rx, tx;
        snprintf((char*)c, sizeof c, "e %010lu\n", seq);
        /* leave the USB frame phase */
        yrt_sleep_until(yrt_now_ns() + (int64_t)((double)rand() / RAND_MAX * 1e6), 0);
        int64_t t0 = pnow(p);
        if (port_write(p, c, 13) != 13) die("probe write", 0);
        int got = port_read_exact(p, r, 35, YRT_MS(500));
        int64_t t2 = pnow(p);
        if (got != 35 || r[0] != 'R' || r[34] != '\n' || !u10(r + 2, &a) || a != seq ||
            !u10(r + 13, &rx) || !u10(r + 24, &tx)) {
            fprintf(stderr, "pulse_bench: probe %lu failed (%d bytes)\n", seq, got);
            Sleep(20);
            port_purge(p);
            continue;
        }
        fprintf(f, "%d,%s,%ld,%lld,%lld,%lu,%lu\n", cell, mode_name[mode], wus,
                (long long)t0, (long long)t2, rx, tx);
    }
}

/* --- the board's reports after a block ----------------------------------- */

/* Reads lines until `want` B or L lines (and a D line in byte mode) came, or
 * until 300 ms passed without a byte. Writes each line to board.csv. */
static void read_reports(port* p, FILE* f, int cell, int mode, long wus, int block, int want, int bytemode) {
    static unsigned char buf[1 << 16];
    int n = 0, got = 0, done = 0;
    int64_t quiet = pnow(p) + YRT_MS(bytemode ? 1500 : 300);
    while (!done && pnow(p) < quiet) {
        int r = port_read(p, buf + n, (int)sizeof buf - n - 1);
        if (r < 0) die("report read", r);
        if (r > 0) { n += r; quiet = pnow(p) + YRT_MS(300); }
        /* parse complete lines */
        int s = 0;
        for (int i = 0; i < n; i++) {
            if (buf[i] != '\n') continue;
            unsigned char* l = buf + s;
            int len = i - s + 1;
            unsigned long t, v;
            if ((l[0] == 'B' || l[0] == 'L') && len == 17 && u10(l + 2, &t)) {
                if (l[0] == 'B') v = (unsigned long)((l[13] - '0') * 100 + (l[14] - '0') * 10 + (l[15] - '0'));
                else v = (unsigned long)((l[13] - '0') + 2 * (l[15] - '0'));
                fprintf(f, "%d,%s,%ld,%d,%c,%lu,%lu\n", cell, mode_name[mode], wus, block, l[0], t, v);
                got++;
            } else if ((l[0] == 'D' || l[0] == 'G') && len == 24 && u10(l + 2, &t) && u10(l + 13, &v)) {
                fprintf(f, "%d,%s,%ld,%d,%c,%lu,%lu\n", cell, mode_name[mode], wus, block, l[0], t, v);
                if (l[0] == 'D') done = 1;
            } else {
                fprintf(stderr, "pulse_bench: unexpected line in cell %d: %.*s", cell, len, (const char*)l);
            }
            s = i + 1;
        }
        memmove(buf, buf + s, (size_t)(n - s));
        n -= s;
        if (!bytemode && got >= want) done = 1;
    }
    if (!done) fprintf(stderr, "pulse_bench: cell %d block %d: %d of %d reports\n", cell, block, got, want);
}

static void query_gap(port* p, FILE* f, int cell, int mode, long wus, int block) {
    unsigned char r[32];
    unsigned long t, v;
    cmd(p, "g\n");
    int got = port_read_exact(p, r, 24, YRT_MS(500));
    if (got == 24 && r[0] == 'G' && u10(r + 2, &t) && u10(r + 13, &v))
        fprintf(f, "%d,%s,%ld,%d,G,%lu,%lu\n", cell, mode_name[mode], wus, block, t, v);
    else
        fprintf(stderr, "pulse_bench: cell %d: no G line\n", cell);
}

/* --- one cell ------------------------------------------------------------ */

typedef struct { FILE *probes, *pulses, *board, *cells; } outs;

static void run_cell(const char* dev, int mode, int64_t width_ns, int n, int cell, int run, outs* o) {
    port p;
    long wus = (long)(width_ns / 1000);
    int64_t t_open = qpc_ns();
    port_open(&p, dev, is_raw(mode));
    double open_ms = (double)(qpc_ns() - t_open) / 1e6;
    fprintf(o->cells, "%d,%s,%ld,%d,%s,%.3f\n", cell, mode_name[mode], wus, run,
            is_raw(mode) ? "none" : yrt_policy_name(p.sp.async_policy), open_ms);

    /* a known state: no stream, no line reports, no byte mode left over */
    cmd(&p, "x\nl 0\n");
    Sleep(50);
    port_purge(&p);
    if (is_line(mode)) {
        unsigned char r[32];
        /* RTS first: Windows' usbser.sys sends no request for an RTS change
         * alone, only the next DTR change carries it (measured; see
         * docs/serial.md, Pulses). */
        if (port_line(&p, 1, 0) < 0 || port_line(&p, 0, 0) < 0) die("lines low", 0);
        Sleep(20);
        cmd(&p, "l 1\n");
        int got = port_read_exact(&p, r, 17, YRT_MS(500));
        if (got != 17 || r[0] != 'L' || r[13] != '0' || r[15] != '0') {
            fprintf(stderr, "pulse_bench: cell %d: line report did not start (%d bytes)\n", cell, got);
            exit(1);
        }
    }
    query_gap(&p, o->board, cell, mode, wus, -1); /* resets the board's gap */

    int nblocks = (n + BLOCK - 1) / BLOCK;
    int i = 0;
    for (int b = 0; b < nblocks; b++) {
        int bn = (n - i < BLOCK) ? n - i : BLOCK;
        probes(&p, o->probes, cell, mode, wus, N_PROBE);
        if (!is_line(mode)) {
            char c[32];
            snprintf(c, sizeof c, "b %d 1000\n", 2 * bn);
            cmd(&p, c);
            Sleep(20);
        }
        int64_t next = pnow(&p) + YRT_MS(50);
        for (int k = 0; k < bn; k++, i++) {
            yrt_sleep_until(next, YRT_DEFAULT_SPIN_NS);
            int64_t t_start = pnow(&p), t_ret, t_off = -1, t_off_ret = -1;
            int rc;
            switch (mode) {
            case M_YSP_PULSE:
                rc = yser_pulse(&p.sp, ON_CODE, OFF_CODE, width_ns);
                t_ret = pnow(&p);
                break;
            case M_YSP_ASYNC:
                rc = yser_pulse_async(&p.sp, ON_CODE, OFF_CODE, width_ns);
                t_ret = pnow(&p);
                break;
            case M_RAW_BYTE:
            case M_YSP_BYTES_T0:
                rc = port_byte(&p, ON_CODE);
                t_ret = pnow(&p);
                yrt_sleep_until((mode == M_YSP_BYTES_T0 ? t_start : t_ret) + width_ns, YRT_DEFAULT_SPIN_NS);
                t_off = pnow(&p);
                rc = rc * 10 + port_byte(&p, OFF_CODE);
                t_off_ret = pnow(&p);
                break;
            default: /* the line modes */
                rc = port_line(&p, is_rts(mode), 1);
                t_ret = pnow(&p);
                yrt_sleep_until((mode == M_YSP_DTR_T0 ? t_start : t_ret) + width_ns, YRT_DEFAULT_SPIN_NS);
                t_off = pnow(&p);
                rc = rc * 10 + port_line(&p, is_rts(mode), 0);
                t_off_ret = pnow(&p);
                break;
            }
            fprintf(o->pulses, "%d,%s,%ld,%d,%d,%lld,%lld,%lld,%lld,%d\n", cell, mode_name[mode], wus,
                    b, i, (long long)t_start, (long long)t_ret, (long long)t_off, (long long)t_off_ret, rc);
            next = t_start + width_ns + YRT_MS(50) + (int64_t)((double)rand() / RAND_MAX * 100e6);
        }
        /* the trailing edge of the last pulse, then the reports */
        yrt_sleep_until(pnow(&p) + width_ns + YRT_MS(30), YRT_DEFAULT_SPIN_NS);
        read_reports(&p, o->board, cell, mode, wus, b, 2 * bn, !is_line(mode));
        query_gap(&p, o->board, cell, mode, wus, b);
    }
    probes(&p, o->probes, cell, mode, wus, N_PROBE);
    if (is_line(mode)) cmd(&p, "l 0\n");
    port_close(&p);
    fflush(o->probes); fflush(o->pulses); fflush(o->board); fflush(o->cells);
}

static FILE* open_csv(const char* dir, const char* name, const char* header) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE* f = fopen(path, "rb");
    int fresh = f == NULL;
    if (f) fclose(f);
    f = fopen(path, "ab");
    if (!f) { fprintf(stderr, "pulse_bench: cannot write %s\n", path); exit(1); }
    if (fresh) fprintf(f, "%s\n", header);
    return f;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: pulse_bench COM3 out_dir [n] [rev] [modes=a,b] [widths=1,2,5,10]\n");
        return 2;
    }
    const char* dev = argv[1];
    const char* dir = argv[2];
    int n = 200, rev = 0, run = 1;
    int use[M_COUNT];
    long widths[8] = { 1, 2, 5, 10 };
    int nw = 4;
    for (int m = 0; m < M_COUNT; m++) use[m] = 1;
    for (int a = 3; a < argc; a++) {
        if (strcmp(argv[a], "rev") == 0) { rev = 1; run = 2; }
        else if (strncmp(argv[a], "modes=", 6) == 0) {
            for (int m = 0; m < M_COUNT; m++) use[m] = 0;
            char tmp[256];
            snprintf(tmp, sizeof tmp, "%s", argv[a] + 6);
            for (char* t = strtok(tmp, ","); t; t = strtok(NULL, ",")) {
                int found = 0;
                for (int m = 0; m < M_COUNT; m++) if (strcmp(t, mode_name[m]) == 0) { use[m] = 1; found = 1; }
                if (!found) { fprintf(stderr, "pulse_bench: unknown mode %s\n", t); return 2; }
            }
        } else if (strncmp(argv[a], "widths=", 7) == 0) {
            char tmp[256];
            nw = 0;
            snprintf(tmp, sizeof tmp, "%s", argv[a] + 7);
            for (char* t = strtok(tmp, ","); t && nw < 8; t = strtok(NULL, ",")) widths[nw++] = atol(t);
        } else n = atoi(argv[a]);
    }
    CreateDirectoryA(dir, NULL);
    outs o;
    o.probes = open_csv(dir, "probes.csv", "cell,mode,width_us,t0_ns,t2_ns,rx_us,tx_us");
    o.pulses = open_csv(dir, "pulses.csv", "cell,mode,width_us,block,i,t_start_ns,t_ret_ns,t_off_ns,t_off_ret_ns,rc");
    o.board = open_csv(dir, "board.csv", "cell,mode,width_us,block,kind,t_us,v");
    o.cells = open_csv(dir, "cells.csv", "cell,mode,width_us,run,async_policy,open_ms");
    srand(rev ? 2 : 1);

    /* the cells in order: for each width, each mode */
    int list[64][2], nc = 0;
    for (int w = 0; w < nw; w++)
        for (int m = 0; m < M_COUNT; m++)
            if (use[m]) { list[nc][0] = m; list[nc][1] = w; nc++; }
    int base = run * 1000;
    for (int k = 0; k < nc; k++) {
        int c = rev ? nc - 1 - k : k;
        int m = list[c][0];
        int64_t w = YRT_MS(widths[list[c][1]]);
        fprintf(stderr, "pulse_bench: cell %d/%d %s %ld ms\n", k + 1, nc, mode_name[m], widths[list[c][1]]);
        run_cell(dev, m, w, n, base + c, run, &o);
    }
    fclose(o.probes); fclose(o.pulses); fclose(o.board); fclose(o.cells);
    return 0;
}
