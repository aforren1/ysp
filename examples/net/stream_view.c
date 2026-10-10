/* stream_view.c - any LSL stream on the ysp_rt clock: rate, gaps and the
 * clock fit, once a second.
 *
 *   net_stream_view --list [--wait SECONDS] [--lib PATH]
 *       Lists the streams on the network with their match keys (ysp/net.h,
 *       MATCH KEYS), format, channels and rate.
 *   net_stream_view --key KEY [--seconds S] [--edge CH:LEVEL[:HYST]]
 *                   [--lib PATH]
 *       Opens an inlet for KEY on its own reader thread and prints, each
 *       second: the state, the samples that second (the measured rate),
 *       gaps and samples missing, ring drops, liblsl's newest offset and
 *       round trip, the remote fit (ppm, points, spread), the events, and
 *       the newest sample (numeric streams) or the last marker's text and
 *       its time on the ysp_rt clock minus the time it arrived. With
 *       --edge, threshold crossings of a channel are events too. At the end:
 *       the statistics. Unplug the sender, or stop it, to see LOST, the
 *       reconnect and the gap.
 *
 * Exit code 0 when the stream ran; 1 when it never opened; 2 for a usage
 * error or no liblsl.
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#define YSP_NET_IMPLEMENTATION
#include "ysp/net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The sinks run on the reader thread; the main loop reads these. A
 * printing example, so a lock-free handoff of the last one is enough. */
static volatile uint32_t g_events;
static yin_event g_last;
static char      g_text[64];
static int64_t   g_arrived;
static ynet_lsl  g_lib;

static void on_event(void* ctx, const yin_event* e) {
    (void)ctx;
    g_last = *e;
    g_arrived = yrt_now_ns();
    g_events++;
}
static void on_text(void* ctx, const yin_event* e, const char* t) {
    (void)ctx; (void)e;
    snprintf(g_text, sizeof g_text, "%s", t);
}

static const char* fmt_name(int f) { return f == YNET_FLOAT32 ? "float32" : f == YNET_STRING ? "string" : f == YNET_INT32 ? "int32" : "other"; }

static int list(double wait) {
    static ynet_found f[64];
    int n = ynet_resolve_all(&g_lib, f, 64, wait), i;
    if (n < 0) { fprintf(stderr, "resolve failed: %s\n", ynet_strerror(n)); return 2; }
    for (i = 0; i < n; i++)
        printf("%-60s %-8s %3d ch %9.1f Hz\n", f[i].key, fmt_name(f[i].format), f[i].channels, f[i].rate_hz);
    if (n == 0) printf("no streams in %.1f s\n", wait);
    return 0;
}

int main(int argc, char** argv) {
    static ynet_inlet in;
    static unsigned char mem[YNET_STREAM_BYTES(1 << 14, YNET_MAX_CHANNELS)];   /* 1.6 s at 10 kHz */
    static ynet_stream ring;
    static float x[YNET_MAX_CHANNELS * 4096];
    ynet_inlet_desc d;
    ynet_stats st, prev;
    const char* key = NULL;
    const char* path = NULL;
    double seconds = 30, wait = 2.0;
    int do_list = 0, i, sec, last_state = -1, edge = 0, channels_set = 0, rc;
    float level = 0, hyst = 0;
    int edge_ch = 0;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--list")) do_list = 1;
        else if (!strcmp(argv[i], "--wait") && i + 1 < argc) wait = atof(argv[++i]);
        else if (!strcmp(argv[i], "--key") && i + 1 < argc) key = argv[++i];
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "--lib") && i + 1 < argc) path = argv[++i];
        else if (!strcmp(argv[i], "--edge") && i + 1 < argc) {
            const char* s = argv[++i];
            edge = sscanf(s, "%d:%f:%f", &edge_ch, &level, &hyst) >= 2;
            if (!edge) { fprintf(stderr, "--edge CH:LEVEL[:HYST]\n"); return 2; }
        } else {
            fprintf(stderr, "usage: net_stream_view --list [--wait S] | --key KEY [--seconds S] [--edge CH:LEVEL[:HYST]]"
                            " [--lib PATH]\n");
            return 2;
        }
    }
    if ((rc = ynet_lsl_load(&g_lib, path)) != YNET_OK) { fprintf(stderr, "%s\n", g_lib.error); return 2; }
    printf("liblsl %d.%d from %s\n", (int)g_lib.version / 100, (int)g_lib.version % 100, g_lib.path);
    if (do_list) { rc = list(wait); ynet_lsl_unload(&g_lib); return rc; }
    if (!key) { fprintf(stderr, "--key or --list\n"); ynet_lsl_unload(&g_lib); return 2; }
    memset(&d, 0, sizeof d);
    d.lib = &g_lib;
    d.role = "view";
    d.key = key;
    d.device = 1;
    d.sink = on_event;
    d.text_sink = on_text;
    if (!ynet_stream_init(&ring, mem, sizeof mem, YNET_MAX_CHANNELS)) return 2;
    d.stream = &ring;   /* 64 channels wide: holds any stream */
    if (edge) {
        d.edges[0].channel = edge_ch;
        d.edges[0].level = level;
        d.edges[0].hysteresis = hyst;
        d.n_edges = 1;
    }
    if (!ynet_inlet_start(&in, &d)) { fprintf(stderr, "%s\n", ynet_inlet_error(&in)); ynet_lsl_unload(&g_lib); return 2; }
    memset(&prev, 0, sizeof prev);
    for (sec = 0; sec < (int)seconds; sec++) {
        int64_t t_new = 0;
        double l_new = 0;
        (void)yrt_sleep_ns(1000000000);
        ynet_inlet_stats(&in, &st);
        if (st.state != last_state) {
            printf("state %s (opens %u)%s%s\n", ynet_state_name(st.state), (unsigned)st.opens,
                   st.state == YNET_FAILED ? ": " : "", st.state == YNET_FAILED ? ynet_inlet_error(&in) : "");
            if (st.state == YNET_RUNNING)
                printf("  stream %s:%s:%s:%s, %s, %d ch, %.1f Hz, uid %s\n", st.found.name, st.found.type,
                       st.found.source_id, st.found.hostname, fmt_name(st.format), st.channels, st.rate_hz, st.uid);
            last_state = st.state;
        }
        if (!channels_set && st.channels > 0) channels_set = 1;
        while (ynet_stream_read(&ring, NULL, NULL, x, 4096) > 0) { }   /* a writer would keep these */
        printf("%3d s: %6llu samples (%.1f Hz), gaps %llu (%llu missing), drops %llu, clamped %llu; offset %+.6f s, "
               "rtt %.1f us; fit %+.2f ppm, %u points, spread %.1f us%s; events %u\n",
               sec + 1, (unsigned long long)(st.samples - prev.samples), st.measured_hz, (unsigned long long)st.gaps,
               (unsigned long long)st.missing, (unsigned long long)st.dropped, (unsigned long long)st.clamped,
               st.offset_s, st.rtt_s * 1e6, st.remote.ppm, (unsigned)st.remote.points,
               (double)st.remote.spread_ns / 1e3, st.remote.slope ? "" : " (nominal)", (unsigned)g_events);
        if (st.format == YNET_FLOAT32 && ynet_stream_newest(&ring, &t_new, &l_new, x)) {
            int c;
            printf("       newest: lsl %.6f s, ysp_rt %.6f s, arrived %.1f ms later; x =", l_new, (double)t_new / 1e9,
                   (double)(st.last_rx_ns - t_new) / 1e6);
            for (c = 0; c < st.channels && c < 8; c++) printf(" %.4g", (double)x[c]);
            printf("%s\n", st.channels > 8 ? " ..." : "");
        } else if (g_events) {
            yin_event e = g_last;
            printf("       last event: code %u \"%s\", ysp_rt %.6f s, %.1f us before it arrived, unc %u us\n",
                   (unsigned)e.code, g_text, (double)e.t / 1e9, (double)(g_arrived - e.t) / 1e3, (unsigned)e.unc_us);
        }
        prev = st;
    }
    ynet_inlet_stop(&in);
    ynet_inlet_stats(&in, &st);
    printf("total: %llu samples, %llu events, %llu chunks, %llu gaps, %llu missing, %llu dropped, %llu clamped, "
           "%llu backward; %llu corrections (%llu refused), %llu resolves (%llu ambiguous); reader busy %.1f ns per sample\n",
           (unsigned long long)st.samples, (unsigned long long)st.events, (unsigned long long)st.chunks,
           (unsigned long long)st.gaps, (unsigned long long)st.missing, (unsigned long long)st.dropped,
           (unsigned long long)st.clamped, (unsigned long long)st.backward, (unsigned long long)st.corrections,
           (unsigned long long)st.refused, (unsigned long long)st.resolves, (unsigned long long)st.ambiguous,
           st.samples ? (double)st.busy_ns / (double)st.samples : 0.0);
    printf("local fit: %+.3f ppm, bracket %.1f us; source: %s\n", st.local.ppm, (double)st.local.width_ns / 1e3,
           ynet_inlet_source(&in).note);
    ynet_lsl_unload(&g_lib);
    return st.opens > 0 ? 0 : 1;
}
