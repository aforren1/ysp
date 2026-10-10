/* photodiode_check.c - the MCU photodiode on ysp/device.h: the first real
 * device on the device layer, and its hand test.
 *
 *   photodiode_check --list
 *       Lists the serial ports with their match keys (ysp/device.h, MATCH
 *       KEYS). No device needed.
 *   photodiode_check --key KEY [--family line|xid|photo] --monitor SECONDS
 *       No window: prints each event of the device at KEY with its mapped
 *       time, the read time it came with, and the fit, then the device's
 *       statistics. Works for any family: an XID box's presses, a line
 *       board's edges. The hand test of a device before a window is in it.
 *   photodiode_check --key KEY [--frames N] [--csv FILE] [--invert]
 *                    [--backend dxgi|composition]
 *       A window. A 64-pixel patch in the top-left corner runs a
 *       pseudo-random sequence of black and white runs of 1 to 5 frames
 *       (WARNING: up to 30 Hz flicker; do not run it where someone who is
 *       sensitive to flicker can see the screen). The photodiode board on
 *       that corner sends its edges in the ysp line protocol (firmware/
 *       ysp_line/); the device layer maps them to the ysp_rt clock on its
 *       reader thread and pushes them through ysp/screen.h's input bridge
 *       (yscr_push_input), and this loop reads them with every other input
 *       (yscr_poll, yscr_event_input). Each change of the patch is matched
 *       to the next edge of its polarity; the program prints edge minus
 *       flip onset (mean, SD, p1, p50, p99), the edges missing and extra,
 *       the device's fit and its statistics. --invert when the board reads
 *       light as 0. Shift+Esc ends the run.
 *
 * The key of a Teensy in serial mode is "serial:16C0:0483:<serial>:" (any
 * Teensy: "serial:16C0:0483::"); of a Raspberry Pi Pico with the
 * earlephilhower core, "serial:2E8A:000A::". --list shows them.
 *
 * What this checks on hardware, and nothing here has yet: the board's
 * firmware and protocol, identify, the timer queries and their bracket
 * widths on native USB, the fit on real USB delivery, the bridge under a
 * window, and the display onset against light (the number that
 * desc.onset_offset_ns of ysp/screen.h is for).
 *
 * Exit code 0 when the run ends with every expected edge found and none
 * extra (window mode) or the device ran (monitor mode); 1 otherwise; 2 for
 * a usage or setup error.
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#define YSP_DEVICE_IMPLEMENTATION
#include "ysp/device.h"
#define YSP_SCREEN_IMPLEMENTATION
#include "ysp/screen.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_FRAMES 36000
#define MAX_EDGES  40000

typedef struct edge { int64_t t; int level; int used; } edge;

static yscr_record   g_rec[MAX_FRAMES];
static int           g_have[MAX_FRAMES];
static unsigned char g_state[MAX_FRAMES];
static edge          g_edge[MAX_EDGES];
static int           g_ne;
static ydev_device   g_dev;

static int cmp_d(const void* a, const void* b) {
    double x = *(const double*)a, y = *(const double*)b;
    return x < y ? -1 : (x > y);
}

/* The sink: the bridge. Any thread may push; the frame loop reads. */
static void to_bridge(void* ctx, const yin_event* e) {
    (void)ctx;
    (void)yscr_push_input(e);
}

/* Monitor mode prints on the reader thread: slow, but it is a hand test. */
static void to_console(void* ctx, const yin_event* e) {
    int64_t* t0 = (int64_t*)ctx;
    printf("  event %s control %u value %.3f  t %+.6f s  ticks %u  unc %u us\n",
           e->type == YIN_PRESS ? "press  " : e->type == YIN_RELEASE ? "release" : "sample ", (unsigned)e->control,
           (double)e->value, (double)(e->t - *t0) / 1e9, (unsigned)e->ticks, (unsigned)e->unc_us);
    fflush(stdout);
}

static void print_stats(void) {
    ydev_stats st;
    yin_source src = ydev_source(&g_dev);
    ydev_get_stats(&g_dev, &st);
    printf("device: %s, port %s, ident \"%s\", opens %u\n", ydev_state_name(st.state), st.found, st.ident,
           (unsigned)st.opens);
    printf("  %llu reads, %llu bytes, %llu events, %llu pairs, %llu probes, %llu answers, %llu refused, "
           "%llu garbage bytes, %llu clamped\n", (unsigned long long)st.reads, (unsigned long long)st.bytes,
           (unsigned long long)st.events, (unsigned long long)st.pairs, (unsigned long long)st.probes,
           (unsigned long long)st.answers, (unsigned long long)st.refused, (unsigned long long)st.garbage,
           (unsigned long long)st.clamped);
    if (st.fit.ready)
        printf("  fit: %u points over %.1f s, %+.2f ppm%s, spread (p99) %.1f us, bracket width %.1f us, "
               "epoch %u\n", st.fit.points, (double)st.fit.span_ns / 1e9, st.fit.ppm,
               st.fit.slope ? "" : " (nominal)", (double)st.fit.spread_ns / 1e3, (double)st.fit.width_ns / 1e3,
               st.fit.epoch);
    printf("  source: tier %u, %s\n", (unsigned)src.tier, src.note);
}

static int list_ports(void) {
    yser_port_info p[32];
    int n = yser_list_ports(p, 32), i;
    char key[160];
    if (n < 0) { fprintf(stderr, "listing failed: %s\n", yser_strerror(n)); return 2; }
    for (i = 0; i < n; i++) {
        if (ydev_port_key(&p[i], key, sizeof key) < 0) key[0] = '\0';
        printf("%-10s %-48s %s\n", p[i].name, key, p[i].description);
    }
    if (n == 0) printf("no serial ports\n");
    return 0;
}

static int monitor(ydev_desc* d, double secs) {
    static int64_t t0;
    int64_t end;
    int last = -1;
    t0 = yrt_now_ns();
    d->sink = to_console;
    d->sink_ctx = &t0;
    if (!ydev_start(&g_dev, d)) { fprintf(stderr, "%s\n", ydev_error(&g_dev)); return 2; }
    end = t0 + (int64_t)(secs * 1e9);
    while (yrt_now_ns() < end) {
        int s = ydev_state(&g_dev);
        if (s != last) { printf("state %s at %+.3f s\n", ydev_state_name(s), (double)(yrt_now_ns() - t0) / 1e9); last = s; }
        (void)yrt_sleep_ns(10000000);
    }
    ydev_stop(&g_dev);
    print_stats();
    return g_dev.snap.opens > 0 ? 0 : 1;
}

int main(int argc, char** argv) {
    static yscr_screen scr;
    static unsigned char ring_mem[YRT_RING_BYTES(4096)];
    static yrt_event evs[4096];
    static double diff[MAX_FRAMES];
    yrt_ring ring;
    yrt_ring_desc rd;
    yscr_desc sd;
    yscr_frame f;
    ydev_desc dd;
    FILE* csv = NULL;
    char line[512];
    const char* key = NULL;
    const char* backend = NULL;
    double mon = 0;
    int frames = 3600, i, run = 0, level = 0, nd = 0, missing = 0, extra = 0, late = 0, dropped = 0, invert = 0;
    int family = YBOX_LINE;
    uint32_t rng = 2463534242u;
    double mean = 0, sd2 = 0;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--list")) return list_ports();
        else if (!strcmp(argv[i], "--key") && i + 1 < argc) key = argv[++i];
        else if (!strcmp(argv[i], "--monitor") && i + 1 < argc) mon = atof(argv[++i]);
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--csv") && i + 1 < argc) {
            if (!(csv = fopen(argv[++i], "w"))) { fprintf(stderr, "cannot write %s\n", argv[i]); return 2; }
        }
        else if (!strcmp(argv[i], "--backend") && i + 1 < argc) backend = argv[++i];
        else if (!strcmp(argv[i], "--invert")) invert = 1;
        else if (!strcmp(argv[i], "--family") && i + 1 < argc) {
            const char* fam = argv[++i];
            family = !strcmp(fam, "xid") ? YBOX_XID : !strcmp(fam, "photo") ? YBOX_PHOTO : !strcmp(fam, "line") ? YBOX_LINE : 0;
            if (!family) { fprintf(stderr, "family: line, xid or photo\n"); return 2; }
        } else {
            fprintf(stderr, "usage: photodiode_check --list | --key KEY [--family line|xid|photo] "
                            "[--monitor SECONDS | --frames N --csv FILE --invert --backend dxgi|composition]\n");
            return 2;
        }
    }
    if (!key) { fprintf(stderr, "--key is required (--list shows the keys)\n"); return 2; }
    if (frames < 60 || frames > MAX_FRAMES) { fprintf(stderr, "frames must be 60..%d\n", MAX_FRAMES); return 2; }

    memset(&dd, 0, sizeof dd);
    dd.role = "photo";
    dd.family = family;
    dd.key = key;
    dd.device = 1;
    dd.kind = family == YBOX_XID ? YIN_KIND_BOX : YIN_KIND_SYNC;
    if (mon > 0) return monitor(&dd, mon);

    rd.memory = ring_mem;
    rd.bytes = sizeof ring_mem;
    memset(&ring, 0, sizeof ring);
    if (!yrt_ring_open(&ring, &rd)) { fprintf(stderr, "%s\n", yrt_ring_error(&ring)); return 2; }
    memset(&sd, 0, sizeof sd);
    sd.ring = &ring;
    sd.patch.on = true;
    sd.patch.size = 64;
    sd.patch.corner = YSCR_TOP_LEFT;
    if (backend) {
        if (!strcmp(backend, "composition")) sd.backend = YSCR_BACKEND_COMPOSITION;
        else if (!strcmp(backend, "dxgi")) sd.backend = YSCR_BACKEND_DXGI_FLIP;
        else { fprintf(stderr, "backend: dxgi or composition\n"); return 2; }
    }
    /* the screen first: the bridge exists once a screen with SDL is open */
    if (!yscr_open(&scr, &sd)) { fprintf(stderr, "screen: %s\n", yscr_error(&scr)); return 2; }
    yscr_describe(&scr, line, sizeof line);
    printf("%s\n", line);
    dd.sink = to_bridge;
    if (!ydev_start(&g_dev, &dd)) { fprintf(stderr, "%s\n", ydev_error(&g_dev)); yscr_close(&scr); return 2; }
    (void)yrt_thread_elevate(NULL);

    /* 3 s of gray while the device opens and its fit gets a slope */
    {
        int64_t end = yrt_now_ns() + 3000000000LL;
        while (yrt_now_ns() < end) {
            SDL_Event ev;
            int64_t t;
            if (yscr_begin(&scr, &f) != YSCR_OK) break;
            yscr_set_patch(&scr, 0.5f);
            yscr_flip(&scr);
            while (yscr_poll(&scr, &ev, &t)) { }
        }
        if (ydev_state(&g_dev) != YDEV_RUNNING) {
            fprintf(stderr, "the device at %s is %s after 3 s\n", key, ydev_state_name(ydev_state(&g_dev)));
            ydev_stop(&g_dev);
            print_stats();
            yscr_close(&scr);
            return 2;
        }
        while (yrt_ring_drain(&ring, evs, 4096) > 0) { }
    }

    for (i = 0; i < frames; i++) {
        SDL_Event ev;
        int64_t t;
        yin_event in;
        int k, n;
        if (yscr_begin(&scr, &f) != YSCR_OK) break;
        if (run == 0) {                       /* a new run of 1 to 5 frames */
            rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
            run = 1 + (int)(rng % 5);
            level = !level;
        }
        run--;
        if (f.index < MAX_FRAMES) g_state[f.index] = (unsigned char)level;
        yscr_set_patch(&scr, level ? 1.0f : 0.0f);
        yscr_flip(&scr);
        while (yscr_poll(&scr, &ev, &t))
            if (yscr_event_input(&scr, &ev, &in) && in.kind == YIN_KIND_SYNC && in.device == 1 && g_ne < MAX_EDGES &&
                (in.type == YIN_PRESS || in.type == YIN_RELEASE)) {
                g_edge[g_ne].t = in.t;
                g_edge[g_ne].level = (in.type == YIN_PRESS) ^ invert;
                g_edge[g_ne].used = 0;
                g_ne++;
            }
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
    frames = i;
    {   /* the last record, and the light that lags: 300 ms more of input */
        yscr_record r;
        int64_t end = yrt_now_ns() + 300000000;
        int k, n;
        yscr_wait_flip(&scr, &r);
        while (yrt_now_ns() < end) {
            SDL_Event ev;
            int64_t t;
            yin_event in;
            while (yscr_poll(&scr, &ev, &t))
                if (yscr_event_input(&scr, &ev, &in) && in.kind == YIN_KIND_SYNC && in.device == 1 && g_ne < MAX_EDGES &&
                    (in.type == YIN_PRESS || in.type == YIN_RELEASE)) {
                    g_edge[g_ne].t = in.t;
                    g_edge[g_ne].level = (in.type == YIN_PRESS) ^ invert;
                    g_edge[g_ne].used = 0;
                    g_ne++;
                }
            (void)yrt_sleep_ns(5000000);
        }
        ydev_stop(&g_dev);
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
    print_stats();

    if (csv) fprintf(csv, "frame,level,onset_ns,photodiode_ns,diff_us,path,flags\n");
    for (i = 1; i < frames && i < MAX_FRAMES; i++) {
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
    {   /* edges inside the run only: the warm-up's and the closing ones are not the sequence's */
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
