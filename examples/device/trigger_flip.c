/* trigger_flip.c - an output role bound to a ysp/screen.h trigger channel
 * (ysp/device.h, AT THE FLIP): the screen's deadline worker calls the
 * device layer at each planned vblank, and the device layer writes the
 * code and logs it. No second scheduler.
 *
 *   device_trigger_flip [--out FAMILY:KEY] [--frames N] [--every K] [--pulse S]
 *       Runs N frames (default 600) on ysp/screen.h's simulated display (a
 *       60 Hz vblank grid on the ysp_rt clock, no window) and triggers code
 *       1, 2, 3, ... at every K-th flip (default 10), each a pulse of S
 *       seconds (default 0.002). Without --out the output is an in-process
 *       trigger box that keeps its writes (no hardware). With --out it is a
 *       real one: FAMILY is triggerbox, biosemi, mmbts, mmbts-s, lines,
 *       line or xid, KEY its match key (device_out_latency --list).
 *       It prints, per trigger, the planned vblank (the deadline), the
 *       write's start and length, and the pulse's end; then p50, p99 and
 *       max of write start minus deadline. A windowed program changes only
 *       sd.backend: the binding is the same.
 *
 * The binding is three lines (see main):
 *     yscr_trigger_desc ch = ydev_trigger_channel(&out, 0);
 *     sd.triggers = &ch; sd.n_triggers = 1;
 *     yscr_trigger(&scr, 0, code);            // between begin and flip
 * ysp/screen.h comes before ysp/device.h, so ydev_trigger_fn exists.
 *
 * Exit code 0 when every trigger wrote its code; 1 otherwise; 2 for a
 * usage or setup error.
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#ifndef YSCR_NO_SDL
#define YSCR_NO_SDL               /* the simulated display needs no SDL */
#endif
#define YSP_SCREEN_IMPLEMENTATION
#include "ysp/screen.h"
#define YSP_DEVICE_IMPLEMENTATION
#include "ysp/device.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- the in-process trigger box: keeps each write and its time ------------ */

typedef struct fakebox { int64_t t[4096]; uint8_t b[4096]; int n; } fakebox;
static fakebox g_box;

static void* fb_open(void* user, const char* key, char* found, size_t cap) {
    (void)key;
    snprintf(found, cap, "in-process box");
    return user;
}
static int fb_read(void* conn, uint8_t* buf, int cap, int64_t timeout_ns) {
    (void)conn; (void)buf; (void)cap;
    (void)yrt_sleep_ns(timeout_ns);
    return 0;
}
static int fb_write(void* conn, const uint8_t* buf, int n) {
    fakebox* f = (fakebox*)conn;
    /* writes come under the device's output lock: one at a time */
    if (f->n < 4096 && n > 0) { f->t[f->n] = yrt_now_ns(); f->b[f->n++] = buf[n - 1]; }
    return n;
}
static void fb_close(void* conn) { (void)conn; }

static int cmp_i64(const void* a, const void* b) {
    int64_t x = *(const int64_t*)a, y = *(const int64_t*)b;
    return x < y ? -1 : (x > y);
}

static int family_of(const char* s, bool* latched) {
    *latched = strcmp(s, "mmbts-s") == 0;
    if (!strcmp(s, "triggerbox")) return YBOX_TRIGGERBOX;
    if (!strcmp(s, "biosemi")) return YBOX_BIOSEMI;
    if (!strcmp(s, "mmbts") || !strcmp(s, "mmbts-s")) return YBOX_MMBTS;
    if (!strcmp(s, "lines")) return YBOX_LINES;
    if (!strcmp(s, "line")) return YBOX_LINE;
    if (!strcmp(s, "xid")) return YBOX_XID;
    return 0;
}

static ydev_device g_out;
static yscr_screen g_scr;
static unsigned char g_mem[YRT_RING_BYTES(16384)];
static yrt_event g_rec[16384];
static int64_t g_lag[2048];

int main(int argc, char** argv) {
    const char* out_arg = NULL;
    int frames = 600, every = 10, i, n, k, ntrig = 0, nwrote = 0, nlag = 0;
    double pulse_s = 0.002;
    bool latched = false;
    char key[256], fam[32];
    yrt_ring ring;
    yrt_ring_desc rd;
    ydev_desc od;
    yscr_desc sd;
    yscr_frame f;
    yscr_trigger_desc ch;
    uint32_t code = 0;

    for (i = 1; i < argc; i++) {
        const char* v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(argv[i], "--out") && v) { out_arg = v; i++; }
        else if (!strcmp(argv[i], "--frames") && v) { frames = atoi(v); i++; }
        else if (!strcmp(argv[i], "--every") && v) { every = atoi(v); i++; }
        else if (!strcmp(argv[i], "--pulse") && v) { pulse_s = atof(v); i++; }
        else {
            fprintf(stderr, "usage: device_trigger_flip [--out FAMILY:KEY] [--frames N] [--every K] [--pulse S]\n");
            return 2;
        }
    }
    if (frames < 1 || frames > 100000 || every < 1 || pulse_s < 0.0 || pulse_s > 0.1) return 2;

    memset(&rd, 0, sizeof rd);
    rd.memory = g_mem;
    rd.bytes = sizeof g_mem;
    memset(&ring, 0, sizeof ring);
    if (!yrt_ring_open(&ring, &rd)) return 2;

    /* the output role */
    memset(&od, 0, sizeof od);
    od.role = "trig";
    od.device = 1;
    od.ring = &ring;
    od.pulse_ns = (int64_t)(pulse_s * 1e9 + 0.5);
    if (out_arg) {
        const char* c = strchr(out_arg, ':');
        if (!c || (size_t)(c - out_arg) >= sizeof fam) return 2;
        memcpy(fam, out_arg, (size_t)(c - out_arg));
        fam[c - out_arg] = '\0';
        od.family = family_of(fam, &latched);
        od.latched = latched;
        snprintf(key, sizeof key, "%s", c + 1);
        od.key = key;
        if (!od.family) { fprintf(stderr, "unknown family %s\n", fam); return 2; }
        if (ybox_fixed_pulse_ns(od.family) > 0 && !latched) od.pulse_ns = 0;   /* the box's own 8 ms */
    } else {
        od.family = YBOX_TRIGGERBOX;
        od.key = "sim:box";
        od.transport.open = fb_open;
        od.transport.read = fb_read;
        od.transport.write = fb_write;
        od.transport.close = fb_close;
        od.transport.user = &g_box;
    }
    if (!ydev_open(&g_out, &od)) { fprintf(stderr, "%s\n", ydev_error(&g_out)); return 2; }
    if (ydev_wait_running(&g_out, YRT_S(2)) < 0) {
        fprintf(stderr, "the output at %s: %s\n", od.key, ydev_error(&g_out));
        ydev_close(&g_out);
        return 2;
    }

    /* the screen, with the output as its trigger channel 0 */
    ch = ydev_trigger_channel(&g_out, 0);
    memset(&sd, 0, sizeof sd);
    sd.backend = YSCR_BACKEND_SIM;
    sd.ring = &ring;
    sd.triggers = &ch;
    sd.n_triggers = 1;
    if (!yscr_open(&g_scr, &sd)) { fprintf(stderr, "screen: %s\n", yscr_error(&g_scr)); ydev_close(&g_out); return 2; }
    {
        char line[512];
        yscr_describe(&g_scr, line, sizeof line);
        printf("%s\n", line);
    }
    for (i = 0; i < frames; i++) {
        if (yscr_begin(&g_scr, &f) != YSCR_OK) break;
        if (i % every == every - 1) {
            code = code % 255 + 1;
            if (yscr_trigger(&g_scr, 0, code) == YSCR_OK) ntrig++;
        }
        if (yscr_flip(&g_scr) != YSCR_OK) break;
    }
    (void)yscr_wait_flip(&g_scr, NULL);
    (void)yrt_sleep_ns(50000000);       /* the last pulse's end */
    yscr_close(&g_scr);
    ydev_close(&g_out);

    /* the device's records of the writes the screen's worker made */
    n = yrt_ring_drain(&ring, g_rec, 16384);
    printf("  deadline (s)   write start - deadline (us)   write (us)   code   pulse end - write start (us)\n");
    for (k = 0; k < n; k++) {
        const yrt_event* r = &g_rec[k];
        if (r->source != YRT_SRC_DEVICE || r->kind != YDEV_REC_OUT || !(r->u.u16[18] & YDEV_OUT_FLIP)) continue;
        if (r->u.u16[18] & YDEV_OUT_FAILED) { printf("  write failed, code %u\n", (unsigned)r->u.u32[8]); continue; }
        nwrote++;
        if (nlag < 2048) g_lag[nlag++] = r->u.i64[0] - r->u.i64[3];
        {
            /* its trailing edge, if the host times it */
            int j;
            double end_us = -1.0;
            for (j = k + 1; j < n; j++) {
                const yrt_event* q = &g_rec[j];
                if (q->source == YRT_SRC_DEVICE && q->kind == YDEV_REC_OUT && (q->u.u16[18] & YDEV_OUT_TRAILING) &&
                    q->u.u16[19] == r->u.u16[19]) { end_us = (double)q->u.i64[2] / 1e3; break; }
            }
            if (nwrote <= 10 || nwrote == ntrig)
                printf("  %12.6f   %10.1f   %10.2f   %4u   %s%.1f\n", (double)r->u.i64[3] / 1e9,
                       (double)(r->u.i64[0] - r->u.i64[3]) / 1e3, (double)(r->u.i64[1] - r->u.i64[0]) / 1e3,
                       (unsigned)r->u.u32[8], end_us < 0 ? "device-timed " : "", end_us < 0 ? (double)r->u.i64[2] / 1e3 : end_us);
            else if (nwrote == 11)
                printf("  ...\n");
        }
    }
    qsort(g_lag, (size_t)nlag, sizeof g_lag[0], cmp_i64);
    if (nlag)
        printf("trigger_flip: %d triggers, %d written; write start minus deadline p50 %.1f us, p99 %.1f, max %.1f\n", ntrig,
               nwrote, (double)g_lag[nlag / 2] / 1e3, (double)g_lag[(nlag * 99) / 100] / 1e3, (double)g_lag[nlag - 1] / 1e3);
    if (!out_arg) printf("trigger_flip: the in-process box got %d writes\n", g_box.n);
    return ntrig > 0 && nwrote == ntrig ? 0 : 1;
}
