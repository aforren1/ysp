/* screen_loopback.c - ysp/screen.h's flip records against a photodiode.
 *
 * Not a compile check and not run by ctest: it needs a photodiode on the
 * display, which CI does not have. Build it with -DYSP_BUILD_LOOPBACK=ON.
 *
 *     screen_loopback <serial_device> [frames] [out.csv] [dxgi|composition]
 *
 * WARNING: the patch in the top-left corner alternates between black and
 * white at up to 30 Hz for the length of the run (default 3600 frames, one
 * minute at 60 Hz). Do not run it where someone who is sensitive to flicker
 * can see the screen.
 *
 * Hardware: a photodiode taped over the top-left corner, into a comparator,
 * into a microcontroller that writes 6-byte frames on USB serial:
 *   0xA5, level (0 dark, 1 light), microseconds (uint32, little-endian)
 *       at each change of the comparator's output, stamped in the interrupt;
 *   0x5A, 0, microseconds
 *       every 100 ms, a sync frame.
 * A Teensy or an Arduino with a comparator on an interrupt pin, in about
 * twenty lines:
 *
 *     volatile uint32_t t; volatile uint8_t lvl, seen;
 *     void edge() { t = micros(); lvl = digitalRead(2); seen = 1; }
 *     void setup() { Serial.begin(115200); pinMode(2, INPUT);
 *                    attachInterrupt(digitalPinToInterrupt(2), edge, CHANGE); }
 *     void send(uint8_t tag, uint8_t v, uint32_t us) {
 *         uint8_t b[6] = { tag, v, (uint8_t)us, (uint8_t)(us >> 8),
 *                          (uint8_t)(us >> 16), (uint8_t)(us >> 24) };
 *         Serial.write(b, 6); }
 *     uint32_t last;
 *     void loop() {
 *         if (seen) { noInterrupts(); uint32_t u = t; uint8_t v = lvl; seen = 0;
 *                     interrupts(); send(0xA5, v, u); }
 *         if (micros() - last >= 100000) { last = micros(); send(0x5A, 0, last); }
 *     }
 *
 * The device's microseconds are put on the ysp_rt clock by ysp/rt.h's
 * device clock fit (yrt_fit, LATE mode: an arrival is late, never
 * early), fed with the sync frames' (device time, arrival) pairs; the fit
 * after the run maps every edge, so an edge is timed by the device's
 * interrupt, not by USB arrival. The fit unwraps micros() (it wraps every
 * 71.6 minutes of the board's uptime, which a run can cross) and reports
 * a restart of the board's clock; a run with one is refused, because the
 * edges before it would be mapped by the new clock. Until 2026-10-08 this
 * program fitted a line through the least-late pair of the first and of
 * the last quarter of the run; it still prints how far that line is from
 * the fit, for the first hardware runs.
 *
 * The patch runs a pseudo-random sequence of black and white runs of 1 to 5
 * frames, so a missed or a doubled frame shows as an edge out of place.
 * Each change of the patch is an expected edge at that frame's flip onset.
 * For each, the program finds the photodiode edge of the same polarity
 * that follows it within one period plus 30 ms, and reports edge minus
 * onset: the delay from the vblank the OS reports to light at the corner.
 * That mean is what desc.onset_offset_ns is for.
 *
 * Output: the describe line; per edge (to out.csv when given): frame,
 * polarity, flip onset, photodiode time, difference, path, flags; then the
 * mean, SD, p1, p50, p99 of the difference, edges missing, edges extra, and
 * the flips the records call late or dropped.
 *
 * Exit code 0 if every expected edge was found and no edge was extra, 1
 * otherwise, 2 for a usage or setup error. Shift+Esc ends the run.
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS   /* fopen(); fopen_s() is not portable */
#endif
#define YSP_SERIAL_IMPLEMENTATION
#include "ysp/serial.h"
#define YSP_SCREEN_IMPLEMENTATION
#include "ysp/screen.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_FRAMES 36000
#define MAX_EDGES  40000
#define MAX_SYNCS  20000

typedef struct edge { int64_t t; int level; int used; } edge;
typedef struct sync { int64_t arrival; uint32_t dev_us; } sync_t;
static yrt_fit g_fit;   /* 40 KB: static */

static yscr_record g_rec[MAX_FRAMES];
static int g_have[MAX_FRAMES];
static unsigned char g_state[MAX_FRAMES];
static edge g_edge[MAX_EDGES];
static int g_ne;
static uint32_t g_edge_dev[MAX_EDGES];
static sync_t g_sync[MAX_SYNCS];
static int g_ns;
static unsigned char g_buf[4096];
static int g_nbuf;

/* Parse what arrived; stamp it now. Device time wraps every 71 minutes,
 * longer than a run. */
static void read_device(yser_port* p) {
    int n, i;
    int64_t now;
    n = yser_read(p, g_buf + g_nbuf, (int)sizeof g_buf - g_nbuf, 0, YSER_READ_ANY);
    if (n <= 0) return;
    now = yrt_now_ns();
    g_nbuf += n;
    i = 0;
    while (g_nbuf - i >= 6) {
        unsigned char* b = g_buf + i;
        uint32_t us;
        if (b[0] != 0xA5 && b[0] != 0x5A) { i++; continue; }
        us = (uint32_t)b[2] | ((uint32_t)b[3] << 8) | ((uint32_t)b[4] << 16) | ((uint32_t)b[5] << 24);
        if (b[0] == 0xA5 && g_ne < MAX_EDGES) {
            g_edge[g_ne].level = b[1] != 0;
            g_edge[g_ne].used = 0;
            g_edge_dev[g_ne] = us;
            g_ne++;
        } else if (b[0] == 0x5A && g_ns < MAX_SYNCS) {
            g_sync[g_ns].arrival = now;
            g_sync[g_ns].dev_us = us;
            g_ns++;
        }
        i += 6;
    }
    memmove(g_buf, g_buf + i, (size_t)(g_nbuf - i));
    g_nbuf -= i;
}

static int cmp_d(const void* a, const void* b) {
    double x = *(const double*)a, y = *(const double*)b;
    return x < y ? -1 : (x > y);
}

int main(int argc, char** argv) {
    static yser_port port;
    static yscr_screen scr;
    static unsigned char ring_mem[YRT_RING_BYTES(4096)];
    static yrt_event evs[4096];
    static double diff[MAX_FRAMES];
    yrt_ring ring;
    yrt_ring_desc rd;
    yser_desc sd;
    yscr_desc d;
    yscr_frame f;
    FILE* csv = NULL;
    char line[512];
    int frames = 3600, i, run = 0, level = 0, nd = 0, missing = 0, extra = 0, late = 0, dropped = 0;
    uint32_t rng = 2463534242u;
    double off0, off1, t_mid0, t_mid1, mean = 0, sd2 = 0, old_lo = 1e300, old_hi = -1e300;
    yrt_fit_desc fd;
    yrt_fit_info fi;

    if (argc < 2) {
        fprintf(stderr, "usage: screen_loopback <serial_device> [frames] [out.csv] [dxgi|composition]\n");
        return 2;
    }
    if (argc > 2) frames = atoi(argv[2]);
    if (frames < 60 || frames > MAX_FRAMES) { fprintf(stderr, "frames must be 60..%d\n", MAX_FRAMES); return 2; }
    if (argc > 3 && !(csv = fopen(argv[3], "w"))) { fprintf(stderr, "cannot write %s\n", argv[3]); return 2; }

    memset(&sd, 0, sizeof sd);
    sd.device = argv[1];
    sd.baud = 115200;
    sd.low_latency = true;
    if (!yser_open(&port, &sd)) { fprintf(stderr, "serial: %s\n", yser_error(&port)); return 2; }

    rd.memory = ring_mem;
    rd.bytes = sizeof ring_mem;
    memset(&ring, 0, sizeof ring);
    if (!yrt_ring_open(&ring, &rd)) { fprintf(stderr, "%s\n", yrt_ring_error(&ring)); return 2; }
    yrt_thread_elevate(NULL);
    memset(&d, 0, sizeof d);
    d.ring = &ring;
    d.patch.on = true;
    d.patch.size = 64;
    d.patch.corner = YSCR_TOP_LEFT;
    if (argc > 4) {
        if (!strcmp(argv[4], "composition")) d.backend = YSCR_BACKEND_COMPOSITION;
        else if (!strcmp(argv[4], "dxgi")) d.backend = YSCR_BACKEND_DXGI_FLIP;
        else { fprintf(stderr, "backend: dxgi or composition\n"); return 2; }
    }
    if (!yscr_open(&scr, &d)) { fprintf(stderr, "screen: %s\n", yscr_error(&scr)); yser_close(&port); return 2; }
    yscr_describe(&scr, line, sizeof line);
    printf("%s\n", line);

    for (i = 0; i < frames; i++) {
        int k, n;
        if (yscr_begin(&scr, &f) != YSCR_OK) break;
        if (run == 0) {                       /* a new run of 1 to 5 frames */
            rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
            run = 1 + (int)(rng % 5);
            level = !level;
        }
        run--;
        g_state[f.index] = (unsigned char)level;
        yscr_set_patch(&scr, level ? 1.0f : 0.0f);
        yscr_flip(&scr);
        read_device(&port);
        while ((n = yrt_ring_drain(&ring, evs, 4096)) > 0)
            for (k = 0; k < n; k++) {
                const yrt_event* e = &evs[k];
                uint32_t idx = e->u.u32[9];
                if (e->source != YRT_SRC_SCREEN || e->kind != YSCR_EV_FLIP || idx >= MAX_FRAMES) continue;
                g_rec[idx].onset = (int64_t)e->t_ns;
                g_rec[idx].dropped = e->u.u16[4];
                g_rec[idx].path = (uint8_t)YSCR_EV_PATH_OF(e->u.u16[5]);
                g_rec[idx].flags = (uint16_t)YSCR_EV_FLAGS_OF(e->u.u16[5]);
                g_have[idx] = 1;
            }
    }
    {
        yscr_record r;
        int k, n;
        yscr_wait_flip(&scr, &r);
        yscr_close(&scr);
        while ((n = yrt_ring_drain(&ring, evs, 4096)) > 0)
            for (k = 0; k < n; k++) {
                const yrt_event* e = &evs[k];
                uint32_t idx = e->u.u32[9];
                if (e->source != YRT_SRC_SCREEN || e->kind != YSCR_EV_FLIP || idx >= MAX_FRAMES) continue;
                g_rec[idx].onset = (int64_t)e->t_ns;
                g_rec[idx].dropped = e->u.u16[4];
                g_rec[idx].path = (uint8_t)YSCR_EV_PATH_OF(e->u.u16[5]);
                g_rec[idx].flags = (uint16_t)YSCR_EV_FLAGS_OF(e->u.u16[5]);
                g_have[idx] = 1;
            }
    }
    {   /* the light lags; collect for 300 ms more */
        int64_t end = yrt_now_ns() + 300000000;
        while (yrt_now_ns() < end) { read_device(&port); yrt_sleep_ns(5000000); }
    }
    yser_close(&port);
    frames = i;

    if (g_ns < 4) { fprintf(stderr, "fewer than 4 sync frames from the device: check its sketch\n"); return 2; }
    {   /* device clock to ysp_rt: the fit, over every sync frame */
        int k;
        memset(&fd, 0, sizeof fd);
        fd.mode = YRT_FIT_LATE;
        fd.ns_per_tick = 1000.0;   /* micros() */
        fd.tick_bits = 32;
        if (!yrt_fit_open(&g_fit, &fd)) { fprintf(stderr, "fit: %s\n", yrt_fit_error(&g_fit)); return 2; }
        for (k = 0; k < g_ns; k++) (void)yrt_fit_add(&g_fit, g_sync[k].dev_us, g_sync[k].arrival, 0);
        yrt_fit_get(&g_fit, &fi);
        printf("device clock fit: %u pairs, %u points over %.1f s, %+.2f ppm, spread (p99 above) %.1f us, "
               "rejected %u, epoch %u%s\n", fi.pairs, fi.points, (double)fi.span_ns / 1e9, fi.ppm,
               (double)fi.spread_ns / 1e3, fi.rejected, fi.epoch, fi.slope ? "" : ", slope nominal");
        if (fi.epoch > 0) {
            fprintf(stderr, "the device's clock restarted during the run (a board reset?): run again\n");
            return 2;
        }
        for (k = 0; k < g_ne; k++) g_edge[k].t = yrt_fit_map(&g_fit, g_edge_dev[k]);
    }
    {   /* the line of v0.3: the least-late pair at each end of the run */
        int q = g_ns / 4, k;
        off0 = 1e300; off1 = 1e300;
        t_mid0 = 0; t_mid1 = 0;
        for (k = 0; k < q; k++) {
            double o = (double)g_sync[k].arrival - (double)g_sync[k].dev_us * 1000.0;
            if (o < off0) { off0 = o; t_mid0 = (double)g_sync[k].dev_us * 1000.0; }
        }
        for (k = g_ns - q; k < g_ns; k++) {
            double o = (double)g_sync[k].arrival - (double)g_sync[k].dev_us * 1000.0;
            if (o < off1) { off1 = o; t_mid1 = (double)g_sync[k].dev_us * 1000.0; }
        }
        for (k = 0; k < g_ne; k++) {
            double dv = (double)g_edge_dev[k] * 1000.0;
            double off = t_mid1 > t_mid0 ? off0 + (off1 - off0) * (dv - t_mid0) / (t_mid1 - t_mid0) : off0;
            double dd = (dv + off) - (double)g_edge[k].t;
            if (dd < old_lo) old_lo = dd;
            if (dd > old_hi) old_hi = dd;
        }
        if (g_ne > 0)
            printf("the two-quarter line of v0.3 minus the fit, over the edges: %+.1f .. %+.1f us%s\n",
                   old_lo / 1e3, old_hi / 1e3, fi.ticks0 >= (1ull << 32) ? " (micros() wrapped: the old line is wrong)" : "");
    }
    if (csv) fprintf(csv, "frame,level,onset_ns,photodiode_ns,diff_us,path,flags\n");
    for (i = 1; i < frames; i++) {
        int k, best = -1;
        int64_t onset, window;
        if (!g_have[i]) continue;
        if (g_rec[i].flags & YSCR_FLIP_LATE_TARGET) late++;
        if (g_rec[i].dropped) dropped++;
        if (g_state[i] == g_state[i - 1]) continue;
        onset = g_rec[i].onset;
        window = scr.caps.period_ns + 30000000;
        for (k = 0; k < g_ne; k++)
            if (!g_edge[k].used && g_edge[k].level == g_state[i] && g_edge[k].t >= onset - 2000000 &&
                g_edge[k].t <= onset + window) { best = k; break; }
        if (best < 0) { missing++; continue; }
        g_edge[best].used = 1;
        diff[nd++] = (double)(g_edge[best].t - onset) / 1000.0;
        if (csv) fprintf(csv, "%d,%d,%lld,%lld,%.1f,%u,%u\n", i, g_state[i], (long long)onset,
                         (long long)g_edge[best].t, diff[nd - 1], g_rec[i].path, g_rec[i].flags);
    }
    {   /* edges inside the run only: frame 0's edge and the closing ones are not the sequence's */
        int first = 1, last = frames - 1;
        while (first < frames && !g_have[first]) first++;
        while (last > 0 && !g_have[last]) last--;
        for (i = 0; i < g_ne; i++)
            if (!g_edge[i].used && first <= last && g_edge[i].t > g_rec[first].onset - 2000000 &&
                g_edge[i].t < g_rec[last].onset + scr.caps.period_ns) extra++;
    }
    if (csv) fclose(csv);
    if (nd > 0) {
        for (i = 0; i < nd; i++) mean += diff[i];
        mean /= nd;
        for (i = 0; i < nd; i++) sd2 += (diff[i] - mean) * (diff[i] - mean);
        qsort(diff, (size_t)nd, sizeof diff[0], cmp_d);
        printf("photodiode edge - flip onset, us: n %d mean %.1f sd %.1f p1 %.1f p50 %.1f p99 %.1f\n", nd, mean,
               sqrt(sd2 / (nd > 1 ? nd - 1 : 1)), diff[nd / 100], diff[nd / 2], diff[nd * 99 / 100]);
    }
    printf("frames %d, edges expected %d, missing %d, extra %d; records: late targets %d, dropped %d\n",
           frames, nd + missing, missing, extra, late, dropped);
    return (missing == 0 && extra == 0 && nd > 0) ? 0 : 1;
}
