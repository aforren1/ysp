/* device_pulse.c - ysp/device.h's host-timed pulses, timed by the echo board.
 *
 * The device-layer half of pulse_bench.c: the same board (firmware/ysp_echo
 * version 2), protocol, blocks and CSV files, so pulse_stats.py reads both.
 * Each pulse is ydev_out_pulse() on an output instance whose transport is a
 * yser_port this program owns, so the program can read the board's reports
 * on the same port (the reader role) while the instance writes (the writer
 * role). The instance's own worker writes each trailing edge.
 *
 *   device_pulse COM3 out_dir [n] [rev] [modes=a,b] [widths=1,5] [hold=us]
 *
 * Modes (the prefix is the header version, so two builds can share a
 * folder: dev030_ for v0.3.0, dev031_ for v0.3.1):
 *   devXXX_byte  YBOX_TRIGGERBOX, ydev_out_pulse(dev, 0x55, width): a byte
 *                pulse, the path of the TriggerBox and the MMBT-S at S
 *   devXXX_dtr   YBOX_LINES, code 1: DTR
 *   devXXX_rts   YBOX_LINES, code 2: RTS
 * hold sets desc.min_hold_ns in microseconds (v0.3.1; 0 = the default).
 * t_off_ns is the trailing write's start from ydev_out_last().
 *
 * Build (from a Developer Command Prompt; run under the timing guard):
 *   cl /O2 /W4 /I..\..\..\include device_pulse.c
 * Windows only, as pulse_bench.c.
 *
 * Public domain / MIT-0, as the rest of ysp. */
#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define YSP_DEVICE_IMPLEMENTATION
#include "ysp/device.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BLOCK 20
#define N_PROBE 10

enum { M_BYTE, M_DTR, M_RTS, M_COUNT };
static const char* const mode_suffix[M_COUNT] = { "byte", "dtr", "rts" };
static char mode_name[M_COUNT][32];

static yser_port g_sp;
static ydev_device g_dev;
static int64_t g_hold_ns = 0;

static void die(const char* what) {
    fprintf(stderr, "device_pulse: %s failed (os error %lu)\n", what, (unsigned long)GetLastError());
    exit(1);
}

/* The transport: the program's open port. */
static void* t_open(void* user, const char* key, char* found, size_t cap) {
    (void)user;
    snprintf(found, cap, "%s", key);
    return g_sp.is_open ? &g_sp : NULL;
}
static int t_write(void* conn, const uint8_t* buf, int n) { return yser_write((yser_port*)conn, buf, n); }
static void t_close(void* conn) { (void)conn; }
static int t_lines(void* conn, uint32_t code, uint32_t changed) {
    yser_port* p = (yser_port*)conn;
    if ((changed & 1u) && yser_set_dtr(p, (code & 1u) != 0) < 0) return -1;
    if ((changed & 2u) && yser_set_rts(p, (code & 2u) != 0) < 0) return -1;
    return 0;
}

static void cmd(const char* s) {
    if (yser_write(&g_sp, s, (int)strlen(s)) != (int)strlen(s)) die("command write");
}

static int read_exact(unsigned char* buf, int n, int64_t timeout_ns) {
    int got = 0;
    int64_t end = yrt_now_ns() + timeout_ns;
    while (got < n && yrt_now_ns() < end) {
        int r = yser_read(&g_sp, buf + got, n - got, YRT_MS(100), YSER_READ_ANY);
        if (r < 0) return r;
        got += r;
    }
    return got;
}

static int u10(const unsigned char* s, unsigned long* out) {
    unsigned long v = 0;
    for (int i = 0; i < 10; i++) {
        if (s[i] < '0' || s[i] > '9') return 0;
        v = v * 10u + (unsigned long)(s[i] - '0');
    }
    *out = v;
    return 1;
}

static unsigned long g_seq = 0;

static void probes(FILE* f, int cell, int mode, long wus, int n) {
    unsigned char c[16], r[64];
    for (int i = 0; i < n; i++) {
        unsigned long seq = ++g_seq, a, rx, tx;
        snprintf((char*)c, sizeof c, "e %010lu\n", seq);
        yrt_sleep_until(yrt_now_ns() + (int64_t)((double)rand() / RAND_MAX * 1e6), 0);
        int64_t t0 = yrt_now_ns();
        if (yser_write(&g_sp, c, 13) != 13) die("probe write");
        int got = read_exact(r, 35, YRT_MS(500));
        int64_t t2 = yrt_now_ns();
        if (got != 35 || r[0] != 'R' || r[34] != '\n' || !u10(r + 2, &a) || a != seq ||
            !u10(r + 13, &rx) || !u10(r + 24, &tx)) {
            fprintf(stderr, "device_pulse: probe %lu failed (%d bytes)\n", seq, got);
            Sleep(20);
            yser_purge(&g_sp, YSER_PURGE_RX);
            continue;
        }
        fprintf(f, "%d,%s,%ld,%lld,%lld,%lu,%lu\n", cell, mode_name[mode], wus, (long long)t0, (long long)t2, rx, tx);
    }
}

static void read_reports(FILE* f, int cell, int mode, long wus, int block, int want, int bytemode) {
    static unsigned char buf[1 << 16];
    int n = 0, got = 0, done = 0;
    int64_t quiet = yrt_now_ns() + YRT_MS(bytemode ? 1500 : 300);
    while (!done && yrt_now_ns() < quiet) {
        int r = yser_read(&g_sp, buf + n, (int)sizeof buf - n - 1, YRT_MS(100), YSER_READ_ANY);
        if (r < 0) die("report read");
        if (r > 0) { n += r; quiet = yrt_now_ns() + YRT_MS(300); }
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
                fprintf(stderr, "device_pulse: unexpected line in cell %d: %.*s", cell, len, (const char*)l);
            }
            s = i + 1;
        }
        memmove(buf, buf + s, (size_t)(n - s));
        n -= s;
        if (!bytemode && got >= want) done = 1;
    }
    if (!done) fprintf(stderr, "device_pulse: cell %d block %d: %d of %d reports\n", cell, block, got, want);
}

static void query_gap(FILE* f, int cell, int mode, long wus, int block) {
    unsigned char r[32];
    unsigned long t, v;
    cmd("g\n");
    int got = read_exact(r, 24, YRT_MS(500));
    if (got == 24 && r[0] == 'G' && u10(r + 2, &t) && u10(r + 13, &v))
        fprintf(f, "%d,%s,%ld,%d,G,%lu,%lu\n", cell, mode_name[mode], wus, block, t, v);
    else
        fprintf(stderr, "device_pulse: cell %d: no G line\n", cell);
}

typedef struct { FILE *probes, *pulses, *board, *cells; } outs;

static void run_cell(const char* dev, int mode, int64_t width_ns, int n, int cell, int run, outs* o) {
    long wus = (long)(width_ns / 1000);
    int line = mode != M_BYTE;
    uint32_t code = mode == M_BYTE ? 0x55u : mode == M_DTR ? 1u : 2u;
    int64_t t_opened = yrt_now_ns();
    yser_desc sd;
    memset(&sd, 0, sizeof sd);
    sd.device = dev;
    sd.write_timeout_ns = YRT_MS(100);
    sd.dtr_low_on_open = sd.rts_low_on_open = line;
    if (!yser_open(&g_sp, &sd)) { fprintf(stderr, "device_pulse: open %s: %s\n", dev, yser_error(&g_sp)); exit(1); }
    ydev_desc d;
    memset(&d, 0, sizeof d);
    d.role = "trig";
    d.family = line ? YBOX_LINES : YBOX_TRIGGERBOX;
    d.key = "port:bench";
    d.device = 1;
    d.transport.open = t_open;
    d.transport.write = t_write;
    d.transport.close = t_close;
    d.transport.lines = t_lines;
#if YDEV_VERSION_MINOR > 3 || (YDEV_VERSION_MINOR == 3 && YDEV_VERSION_PATCH >= 1)
    d.min_hold_ns = g_hold_ns;
#endif
    if (!ydev_open(&g_dev, &d)) { fprintf(stderr, "device_pulse: ydev_open: %s\n", ydev_error(&g_dev)); exit(1); }
    if (ydev_wait_running(&g_dev, YRT_S(3)) != YDEV_RUNNING) {
        fprintf(stderr, "device_pulse: not running: %s\n", ydev_error(&g_dev));
        exit(1);
    }
    double open_ms = (double)(yrt_now_ns() - t_opened) / 1e6;
    double hold_us = 0;
#if YDEV_VERSION_MINOR > 3 || (YDEV_VERSION_MINOR == 3 && YDEV_VERSION_PATCH >= 1)
    hold_us = (double)g_dev.d.min_hold_ns / 1e3;
#endif
    fprintf(o->cells, "%d,%s,%ld,%d,%s,%.3f,%.3f\n", cell, mode_name[mode], wus, run,
            yrt_policy_name(yrt_worker_policy(&g_dev.worker)), open_ms, hold_us);

    cmd("x\nl 0\n");
    Sleep(50);
    yser_purge(&g_sp, YSER_PURGE_RX);
    if (line) {
        unsigned char r[32];
        /* the instance's open wrote 0 to both lines */
        Sleep(20);
        cmd("l 1\n");
        int got = read_exact(r, 17, YRT_MS(500));
        if (got != 17 || r[0] != 'L' || r[13] != '0' || r[15] != '0') {
            fprintf(stderr, "device_pulse: cell %d: line report did not start (%d bytes)\n", cell, got);
            exit(1);
        }
    }
    query_gap(o->board, cell, mode, wus, -1);

    int nblocks = (n + BLOCK - 1) / BLOCK;
    int i = 0;
    for (int b = 0; b < nblocks; b++) {
        int bn = (n - i < BLOCK) ? n - i : BLOCK;
        probes(o->probes, cell, mode, wus, N_PROBE);
        if (!line) {
            char c[32];
            snprintf(c, sizeof c, "b %d 1000\n", 2 * bn);
            cmd(c);
            Sleep(20);
        }
        int64_t next = yrt_now_ns() + YRT_MS(50);
        for (int k = 0; k < bn; k++, i++) {
            ydev_out_info lead, trail;
            yrt_sleep_until(next, YRT_DEFAULT_SPIN_NS);
            int64_t t_start = yrt_now_ns();
            int rc = ydev_out_pulse(&g_dev, code, width_ns);
            int64_t t_ret = yrt_now_ns();
            ydev_out_last(&g_dev, &lead);
            /* the worker's trailing write, well after its deadline */
            yrt_sleep_until(t_start + width_ns + YRT_MS(5), 0);
            ydev_out_last(&g_dev, &trail);
            int64_t t_off = -1, t_off_ret = -1;
            if ((trail.flags & YDEV_OUT_TRAILING) && trail.seq == lead.seq) {
                t_off = trail.t_before;
                t_off_ret = trail.t_after;
            }
            fprintf(o->pulses, "%d,%s,%ld,%d,%d,%lld,%lld,%lld,%lld,%d\n", cell, mode_name[mode], wus, b, i,
                    (long long)t_start, (long long)t_ret, (long long)t_off, (long long)t_off_ret, rc);
            next = t_start + width_ns + YRT_MS(50) + (int64_t)((double)rand() / RAND_MAX * 100e6);
        }
        yrt_sleep_until(yrt_now_ns() + width_ns + YRT_MS(30), YRT_DEFAULT_SPIN_NS);
        read_reports(o->board, cell, mode, wus, b, 2 * bn, !line);
        query_gap(o->board, cell, mode, wus, b);
    }
    probes(o->probes, cell, mode, wus, N_PROBE);
    if (line) cmd("l 0\n");
    ydev_close(&g_dev);
    yser_close(&g_sp);
    fflush(o->probes); fflush(o->pulses); fflush(o->board); fflush(o->cells);
}

static FILE* open_csv(const char* dir, const char* name, const char* header) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE* f = fopen(path, "rb");
    int fresh = f == NULL;
    if (f) fclose(f);
    f = fopen(path, "ab");
    if (!f) { fprintf(stderr, "device_pulse: cannot write %s\n", path); exit(1); }
    if (fresh) fprintf(f, "%s\n", header);
    return f;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: device_pulse COM3 out_dir [n] [rev] [modes=a,b] [widths=1,5] [hold=us]\n");
        return 2;
    }
    const char* dev = argv[1];
    const char* dir = argv[2];
    int n = 200, rev = 0, run = 1, use[M_COUNT] = { 1, 1, 1 };
    double widths[8] = { 1, 5 };
    int nw = 2;
    for (int m = 0; m < M_COUNT; m++)
        snprintf(mode_name[m], sizeof mode_name[m], "dev%d%d%d_%s", YDEV_VERSION_MAJOR, YDEV_VERSION_MINOR,
                 YDEV_VERSION_PATCH, mode_suffix[m]);
    for (int a = 3; a < argc; a++) {
        if (strcmp(argv[a], "rev") == 0) { rev = 1; run = 2; }
        else if (strncmp(argv[a], "hold=", 5) == 0) g_hold_ns = (int64_t)(atof(argv[a] + 5) * 1e3 + 0.5);
        else if (strncmp(argv[a], "modes=", 6) == 0) {
            char tmp[256];
            for (int m = 0; m < M_COUNT; m++) use[m] = 0;
            snprintf(tmp, sizeof tmp, "%s", argv[a] + 6);
            for (char* t = strtok(tmp, ","); t; t = strtok(NULL, ",")) {
                int found = 0;
                for (int m = 0; m < M_COUNT; m++)
                    if (strcmp(t, mode_suffix[m]) == 0 || strcmp(t, mode_name[m]) == 0) { use[m] = 1; found = 1; }
                if (!found) { fprintf(stderr, "device_pulse: unknown mode %s\n", t); return 2; }
            }
        } else if (strncmp(argv[a], "widths=", 7) == 0) {
            char tmp[256];
            nw = 0;
            snprintf(tmp, sizeof tmp, "%s", argv[a] + 7);
            for (char* t = strtok(tmp, ","); t && nw < 8; t = strtok(NULL, ",")) widths[nw++] = atof(t);
        } else n = atoi(argv[a]);
    }
    CreateDirectoryA(dir, NULL);
    outs o;
    o.probes = open_csv(dir, "probes.csv", "cell,mode,width_us,t0_ns,t2_ns,rx_us,tx_us");
    o.pulses = open_csv(dir, "pulses.csv", "cell,mode,width_us,block,i,t_start_ns,t_ret_ns,t_off_ns,t_off_ret_ns,rc");
    o.board = open_csv(dir, "board.csv", "cell,mode,width_us,block,kind,t_us,v");
    o.cells = open_csv(dir, "cells.csv", "cell,mode,width_us,run,async_policy,open_ms,min_hold_us");
    srand(rev ? 2 : 1);
    int list[64][2], nc = 0;
    for (int w = 0; w < nw; w++)
        for (int m = 0; m < M_COUNT; m++)
            if (use[m]) { list[nc][0] = m; list[nc][1] = w; nc++; }
    /* cell numbers by version, so two builds can share a folder */
    int base = run * 1000 + 100 * YDEV_VERSION_PATCH + 5000;
    for (int k = 0; k < nc; k++) {
        int c = rev ? nc - 1 - k : k;
        int m = list[c][0];
        int64_t w = (int64_t)(widths[list[c][1]] * 1e6 + 0.5);
        fprintf(stderr, "device_pulse: cell %d/%d %s %g ms\n", k + 1, nc, mode_name[m], widths[list[c][1]]);
        run_cell(dev, m, w, n, base + c, run, &o);
    }
    fclose(o.probes); fclose(o.pulses); fclose(o.board); fclose(o.cells);
    return 0;
}
