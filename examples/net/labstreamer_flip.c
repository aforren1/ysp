/* labstreamer_flip.c - the Neurobehavioral Systems LabStreamer as the rig's
 * reference instrument for display onsets: the hand test of ysp/net.h.
 *
 *   net_labstreamer_flip [--frames N] [--mark flip|record] [--marks NAME]
 *                        [--data KEY] [--lat KEY | --no-lat] [--channel C]
 *                        [--level L] [--hyst H] [--csv FILE]
 *                        [--backend dxgi|composition] [--lib PATH]
 *
 *   A window. A 64-pixel patch in the top-left corner runs black and white
 *   runs of 6 to 20 frames (up to 5 Hz flicker; do not run it where someone
 *   who is sensitive to flicker can see the screen). Tape the LabStreamer's
 *   phototransistor over the patch. At each change ysp sends a marker,
 *   "white" or "black", on its own LSL stream (--marks, ysp-flips):
 *     --mark flip    (default) from ysp/screen.h's trigger channel at the
 *                    planned vblank, stamped with that deadline
 *     --mark record  after the flip record, stamped with its measured onset
 *   Select that stream as the LabStreamer's trigger stream in its web
 *   client (docs/net.md, "LabStreamer hand test"). Two inlets read the
 *   LabStreamer back:
 *     --data KEY   its Data stream (default lsl:Data::, 10 kHz, channel 0
 *                  is input A): ysp's own threshold crossings of channel C
 *                  (--channel 0, --level 1.0, --hyst 0.2), mapped to the
 *                  ysp_rt clock through LSL's clock offsets
 *     --lat KEY    its Latencies stream (default lsl:Latencies::): one JSON
 *                  string per matched event, its latency in seconds
 *   The stream names are the manual's section names, not checked against a
 *   device: `net_stream_view --list` shows the real ones.
 *   At the end it prints, per change of the patch and as distributions:
 *     onset - stamp       the flip record's onset minus the marker's stamp
 *                         (0 in record mode)
 *     edge - onset        ysp's Data-stream edge minus the flip record
 *     edge - stamp        ysp's edge minus the marker's stamp: what the
 *                         LabStreamer should report
 *     latency             the LabStreamer's own number
 *     edge-stamp - latency  the cross-check: two clock paths, one quantity
 *   --csv writes one row per change. Shift+Esc ends the run.
 *
 * Nothing here has run against a LabStreamer or a real liblsl: no device
 * and no liblsl were on the development machine (docs/net.md, "Not done").
 *
 * Exit code 0 when every change got an edge (and a latency, unless
 * --no-lat); 1 otherwise; 2 for a usage or setup error.
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#define YSP_NET_IMPLEMENTATION
#include "ysp/net.h"
#define YSP_SCREEN_IMPLEMENTATION
#include "ysp/screen.h"
#include "ysp/net.h"        /* again, after ysp/screen.h: the trigger channel */
#define YSP_JSON_IMPLEMENTATION
#include "ysp/json.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_FRAMES 36000
#define MAX_EDGES  40000
#define MAX_LAT    8192

typedef struct edge { int64_t t; int level; int used; } edge;
typedef struct latmsg { char text[256]; } latmsg;
typedef struct mark { int64_t target; int64_t lsl_ns; uint32_t code; } mark;

static yscr_record   g_rec[MAX_FRAMES];
static int           g_have[MAX_FRAMES];
static unsigned char g_state[MAX_FRAMES];
static edge          g_edge[MAX_EDGES];
static int           g_ne;
static latmsg        g_lat[MAX_LAT];
static int           g_nlat;
static SDL_Mutex*    g_lat_mx;
static mark          g_mark[MAX_FRAMES];
static int           g_nmark;
static ynet_lsl      g_lib;
static ynet_outlet   g_marks;
static ynet_inlet    g_data, g_latin;

static const char* const g_names[2] = { "black", "white" };

/* The Data inlet's edges: through the bridge to the frame loop. */
static void to_bridge(void* ctx, const yin_event* e) { (void)ctx; (void)yscr_push_input(e); }

/* The Latencies inlet's text: kept for the end, under a lock (the reader's thread). */
static void on_lat(void* ctx, const yin_event* e, const char* text) {
    (void)ctx; (void)e;
    SDL_LockMutex(g_lat_mx);
    if (g_nlat < MAX_LAT) snprintf(g_lat[g_nlat++].text, sizeof g_lat[0].text, "%s", text);
    SDL_UnlockMutex(g_lat_mx);
}

static int cmp_d(const void* a, const void* b) {
    double x = *(const double*)a, y = *(const double*)b;
    return x < y ? -1 : (x > y);
}

static void dist(const char* what, double* v, int n) {
    double mean = 0, sd = 0;
    int i;
    if (n <= 0) { printf("  %-22s n 0\n", what); return; }
    for (i = 0; i < n; i++) mean += v[i];
    mean /= n;
    for (i = 0; i < n; i++) sd += (v[i] - mean) * (v[i] - mean);
    sd = sqrt(sd / (n > 1 ? n - 1 : 1));
    qsort(v, (size_t)n, sizeof v[0], cmp_d);
    printf("  %-22s n %4d  mean %8.1f  sd %6.1f  min %8.1f  p1 %8.1f  p50 %8.1f  p99 %8.1f  max %8.1f  us\n", what, n,
           mean, sd, v[0], v[n / 100], v[n / 2], v[n * 99 / 100], v[n - 1]);
}

static void take_input(yscr_screen* scr) {
    SDL_Event ev;
    int64_t t;
    yin_event in;
    while (yscr_poll(scr, &ev, &t))
        if (yscr_event_input(scr, &ev, &in) && in.kind == YIN_KIND_SYNC && in.device == 2 && g_ne < MAX_EDGES &&
            (in.type == YIN_PRESS || in.type == YIN_RELEASE)) {
            g_edge[g_ne].t = in.t;
            g_edge[g_ne].level = in.type == YIN_PRESS;
            g_edge[g_ne].used = 0;
            g_ne++;
        }
}

static void take_records(yrt_ring* ring, yrt_event* evs, int cap) {
    int n, k;
    while ((n = yrt_ring_drain(ring, evs, cap)) > 0)
        for (k = 0; k < n; k++) {
            const yrt_event* e = &evs[k];
            if (e->source == YRT_SRC_SCREEN && e->kind == YSCR_EV_FLIP && e->u.u32[9] < MAX_FRAMES) {
                uint32_t idx = e->u.u32[9];
                g_rec[idx].onset = (int64_t)e->t_ns;
                g_rec[idx].dropped = e->u.u16[4];
                g_rec[idx].flags = (uint16_t)YSCR_EV_FLAGS_OF(e->u.u16[5]);
                g_have[idx] = 1;
            } else if (e->source == YRT_SRC_NET && e->kind == YNET_REC_OUT && e->aux == 1 && g_nmark < MAX_FRAMES &&
                       !(e->u.u16[18] & YNET_OUT_FAILED)) {
                g_mark[g_nmark].target = e->u.i64[3];
                g_mark[g_nmark].lsl_ns = e->u.i64[2];
                g_mark[g_nmark].code = e->u.u32[8];
                g_nmark++;
            }
        }
}

/* One latency message: {"event_time": s, "trigger_string": "...", "channel":
 * n, "latency": s, "uncertainty": s} (the manual's fields). */
static int parse_lat(const char* text, double* event_time, char* trig, size_t cap, double* latency, double* unc) {
    static unsigned char arena_mem[16384];
    yjs_arena a;
    yjs_value* root;
    yjs_error err;
    size_t n = 0;
    const char* s;
    yjs_arena_init(&a, arena_mem, sizeof arena_mem);
    if (yjs_parse(&a, text, strlen(text), NULL, &root, &err) != YJS_OK) return 0;
    if (yjs_double(yjs_get(root, "event_time"), event_time) != YJS_OK) return 0;
    if (yjs_double(yjs_get(root, "latency"), latency) != YJS_OK) return 0;
    if (yjs_double(yjs_get(root, "uncertainty"), unc) != YJS_OK) *unc = -1;
    s = yjs_string(yjs_get(root, "trigger_string"), &n);
    snprintf(trig, cap, "%.*s", s ? (int)n : 0, s ? s : "");
    return 1;
}

int main(int argc, char** argv) {
    static yscr_screen scr;
    static unsigned char ring_mem[YRT_RING_BYTES(8192)];
    static yrt_event evs[8192];
    static double v_os[MAX_FRAMES], v_eo[MAX_FRAMES], v_es[MAX_FRAMES], v_lat[MAX_FRAMES], v_x[MAX_FRAMES];
    static int lat_used[MAX_LAT];
    yrt_ring ring;
    yrt_ring_desc rd;
    yscr_desc sd;
    yscr_frame f;
    yscr_trigger_desc ch;
    ynet_outlet_desc od;
    ynet_inlet_desc id;
    ynet_stats st;
    FILE* csv = NULL;
    char line[512], marks_key[96];
    const char* data_key = "lsl:Data::";
    const char* lat_key = "lsl:Latencies::";
    const char* marks_name = "ysp-flips";
    const char* backend = NULL;
    const char* path = NULL;
    int frames = 1200, i, run = 0, level = 0, record_mode = 0, use_lat = 1, channel = 0;
    int n_os = 0, n_eo = 0, n_es = 0, n_lat = 0, n_x = 0, changes = 0, missing_edge = 0, missing_lat = 0;
    int by_time = 0;
    float thr = 1.0f, hyst = 0.2f;
    uint32_t rng = 2463534242u;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--mark") && i + 1 < argc) {
            const char* m = argv[++i];
            if (!strcmp(m, "record")) record_mode = 1;
            else if (strcmp(m, "flip")) { fprintf(stderr, "--mark flip or record\n"); return 2; }
        }
        else if (!strcmp(argv[i], "--marks") && i + 1 < argc) marks_name = argv[++i];
        else if (!strcmp(argv[i], "--data") && i + 1 < argc) data_key = argv[++i];
        else if (!strcmp(argv[i], "--lat") && i + 1 < argc) lat_key = argv[++i];
        else if (!strcmp(argv[i], "--no-lat")) use_lat = 0;
        else if (!strcmp(argv[i], "--channel") && i + 1 < argc) channel = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--level") && i + 1 < argc) thr = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--hyst") && i + 1 < argc) hyst = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--csv") && i + 1 < argc) {
            if (!(csv = fopen(argv[++i], "w"))) { fprintf(stderr, "cannot write %s\n", argv[i]); return 2; }
        }
        else if (!strcmp(argv[i], "--backend") && i + 1 < argc) backend = argv[++i];
        else if (!strcmp(argv[i], "--lib") && i + 1 < argc) path = argv[++i];
        else {
            fprintf(stderr, "usage: net_labstreamer_flip [--frames N] [--mark flip|record] [--marks NAME] [--data KEY] "
                            "[--lat KEY | --no-lat] [--channel C] [--level L] [--hyst H] [--csv FILE] "
                            "[--backend dxgi|composition] [--lib PATH]\n");
            return 2;
        }
    }
    if (frames < 120 || frames > MAX_FRAMES) { fprintf(stderr, "frames must be 120..%d\n", MAX_FRAMES); return 2; }
    if (channel < 0 || channel >= YNET_MAX_CHANNELS || hyst < 0) { fprintf(stderr, "channel 0..63, hyst >= 0\n"); return 2; }
    if (!ynet_lsl_open(&g_lib, path)) { fprintf(stderr, "%s\n", ynet_lsl_error(&g_lib)); return 2; }
    printf("liblsl %d.%d from %s\n", (int)g_lib.version / 100, (int)g_lib.version % 100, g_lib.path);

    rd.memory = ring_mem;
    rd.bytes = sizeof ring_mem;
    memset(&ring, 0, sizeof ring);
    if (!yrt_ring_open(&ring, &rd)) { fprintf(stderr, "%s\n", yrt_ring_error(&ring)); return 2; }

    /* the marker outlet first: the screen's trigger channel points at it */
    snprintf(marks_key, sizeof marks_key, "lsl:%s:Markers:ysp-labstreamer-flip", marks_name);
    memset(&od, 0, sizeof od);
    od.lib = &g_lib;
    od.role = "marks";
    od.key = marks_key;
    od.device = 1;
    od.format = YNET_STRING;
    od.code_names = g_names;
    od.n_code_names = 2;
    od.ring = &ring;
    if (!ynet_outlet_open(&g_marks, &od)) { fprintf(stderr, "%s\n", ynet_outlet_error(&g_marks)); return 2; }

    memset(&sd, 0, sizeof sd);
    sd.ring = &ring;
    sd.patch.on = true;
    sd.patch.size = 64;
    sd.patch.corner = YSCR_TOP_LEFT;
    if (!record_mode) {
        ch = ynet_trigger_channel(&g_marks, 0);
        sd.triggers = &ch;
        sd.n_triggers = 1;
    }
    if (backend) {
        if (!strcmp(backend, "composition")) sd.backend = YSCR_BACKEND_COMPOSITION;
        else if (!strcmp(backend, "dxgi")) sd.backend = YSCR_BACKEND_DXGI_FLIP;
        else { fprintf(stderr, "backend: dxgi or composition\n"); return 2; }
    }
    if (!yscr_open(&scr, &sd)) { fprintf(stderr, "screen: %s\n", yscr_error(&scr)); return 2; }
    yscr_describe(&scr, line, sizeof line);
    printf("%s\n", line);
    g_lat_mx = SDL_CreateMutex();

    memset(&id, 0, sizeof id);
    id.lib = &g_lib;
    id.role = "ref";
    id.key = data_key;
    id.device = 2;
    id.format = YNET_FLOAT32;
    id.edges[0].channel = channel;
    id.edges[0].level = thr;
    id.edges[0].hysteresis = hyst;
    id.n_edges = 1;
    id.sink = to_bridge;
    id.ring = &ring;
    if (!ynet_inlet_open(&g_data, &id)) { fprintf(stderr, "%s\n", ynet_inlet_error(&g_data)); yscr_close(&scr); return 2; }
    if (use_lat) {
        memset(&id, 0, sizeof id);
        id.lib = &g_lib;
        id.role = "lat";
        id.key = lat_key;
        id.device = 3;
        id.format = YNET_STRING;
        id.text_sink = on_lat;
        id.ring = &ring;
        if (!ynet_inlet_open(&g_latin, &id)) { fprintf(stderr, "%s\n", ynet_inlet_error(&g_latin)); yscr_close(&scr); return 2; }
    }
    (void)yrt_thread_elevate(NULL);

    /* gray until the inlets run and the LabStreamer has subscribed (up to 10 s) */
    {
        int64_t end = yrt_now_ns() + 10000000000LL;
        while (yrt_now_ns() < end) {
            if (yscr_begin(&scr, &f) != YSCR_OK) break;
            yscr_set_patch(&scr, 0.5f);
            yscr_flip(&scr);
            take_input(&scr);
            take_records(&ring, evs, 8192);
            if (ynet_inlet_state(&g_data) == YNET_RUNNING && (!use_lat || ynet_inlet_state(&g_latin) == YNET_RUNNING) &&
                ynet_outlet_consumers(&g_marks))
                break;
        }
        if (ynet_inlet_state(&g_data) != YNET_RUNNING || (use_lat && ynet_inlet_state(&g_latin) != YNET_RUNNING)) {
            fprintf(stderr, "after 10 s: %s is %s, %s is %s (net_stream_view --list shows the streams)\n", data_key,
                    ynet_state_name(ynet_inlet_state(&g_data)), use_lat ? lat_key : "--no-lat",
                    use_lat ? ynet_state_name(ynet_inlet_state(&g_latin)) : "-");
            if (use_lat) ynet_inlet_close(&g_latin);
            ynet_inlet_close(&g_data);
            yscr_close(&scr);
            ynet_outlet_close(&g_marks);
            return 2;
        }
        if (!ynet_outlet_consumers(&g_marks))
            printf("warning: nothing subscribes to %s yet: select it as the LabStreamer's trigger stream\n", marks_key);
        g_ne = 0;
        g_nmark = 0;
        memset(g_have, 0, sizeof g_have);
        SDL_LockMutex(g_lat_mx);
        g_nlat = 0;
        SDL_UnlockMutex(g_lat_mx);
    }

    for (i = 0; i < frames; i++) {
        int changed = 0;
        if (yscr_begin(&scr, &f) != YSCR_OK) break;
        if (run == 0) {                       /* a new run of 6 to 20 frames */
            rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
            run = 6 + (int)(rng % 15);
            level = !level;
            changed = 1;
        }
        run--;
        if (f.index < MAX_FRAMES) g_state[f.index] = (unsigned char)level;
        yscr_set_patch(&scr, level ? 1.0f : 0.0f);
        if (changed && !record_mode) (void)yscr_trigger(&scr, 0, (uint32_t)level);
        yscr_flip(&scr);
        take_input(&scr);
        take_records(&ring, evs, 8192);
        {
            /* record mode: a marker for each change whose record came in */
            static int64_t sent_upto = 1;
            int64_t k;
            if (record_mode)
                for (k = sent_upto; k < (int64_t)f.index && k < MAX_FRAMES; k++) {
                    if (!g_have[k]) break;
                    if (g_state[k] != g_state[k - 1])
                        (void)ynet_out_mark_at(&g_marks, g_state[k], NULL, g_rec[k].onset);
                    sent_upto = k + 1;
                }
        }
    }
    frames = i;
    {   /* the light, the Data stream and the LabStreamer's answers lag: 1.5 s more */
        yscr_record r;
        int64_t end = yrt_now_ns() + 1500000000;
        yscr_wait_flip(&scr, &r);
        while (yrt_now_ns() < end) {
            take_input(&scr);
            take_records(&ring, evs, 8192);
            (void)yrt_sleep_ns(5000000);
        }
    }
    ynet_inlet_stats(&g_data, &st);
    if (use_lat) ynet_inlet_close(&g_latin);
    ynet_inlet_close(&g_data);
    yscr_close(&scr);
    take_records(&ring, evs, 8192);
    ynet_outlet_close(&g_marks);

    printf("Data stream: %s, %llu samples, %.1f Hz, gaps %llu (%llu missing), offset %+.6f s, rtt %.1f us, "
           "fit %+.2f ppm over %u points\n", ynet_state_name(st.state), (unsigned long long)st.samples, st.measured_hz,
           (unsigned long long)st.gaps, (unsigned long long)st.missing, (double)st.lsl_offset_ns / 1e9, (double)st.lsl_rtt_ns / 1e3, st.remote.ppm,
           (unsigned)st.remote.points);

    if (csv) fprintf(csv, "frame,level,onset_ns,stamp_ns,marker_lsl_s,edge_ns,edge_minus_onset_us,edge_minus_stamp_us,"
                          "latency_us,cross_us\n");
    {
        int m = 0, latk = 0;
        for (i = 1; i < frames && i < MAX_FRAMES; i++) {
            int k, best = -1, li = -1;
            int64_t onset, stamp, window;
            double lsl_s = 0, lat_us = NAN, es = NAN;
            if (!g_have[i] || g_state[i] == g_state[i - 1]) continue;
            changes++;
            onset = g_rec[i].onset;
            stamp = record_mode ? onset : 0;
            /* this change's marker: the one stamped nearest the onset */
            for (k = m; k < g_nmark; k++)
                if (llabs(g_mark[k].target - onset) < scr.caps.period_ns / 2) { stamp = g_mark[k].target; lsl_s = (double)g_mark[k].lsl_ns / 1e9; m = k + 1; break; }
            if (stamp) v_os[n_os++] = (double)(onset - stamp) / 1e3;
            window = scr.caps.period_ns + 30000000;
            for (k = 0; k < g_ne; k++)
                if (!g_edge[k].used && g_edge[k].level == g_state[i] && g_edge[k].t >= onset - 2000000 &&
                    g_edge[k].t <= onset + window) { best = k; break; }
            if (best >= 0) {
                g_edge[best].used = 1;
                v_eo[n_eo++] = (double)(g_edge[best].t - onset) / 1e3;
                if (stamp) { es = (double)(g_edge[best].t - stamp) / 1e3; v_es[n_es++] = es; }
            } else missing_edge++;
            if (use_lat) {
                /* by event_time when it is the marker's LSL stamp; else in order of the trigger string */
                SDL_LockMutex(g_lat_mx);
                for (k = 0; k < g_nlat && li < 0; k++) {
                    double et, lat, unc;
                    char trig[64];
                    if (lat_used[k] || !parse_lat(g_lat[k].text, &et, trig, sizeof trig, &lat, &unc)) continue;
                    if (lsl_s != 0 && fabs(et - lsl_s) < 0.0005) { li = k; by_time++; }
                }
                for (k = latk; k < g_nlat && li < 0; k++) {
                    double et, lat, unc;
                    char trig[64];
                    if (lat_used[k] || !parse_lat(g_lat[k].text, &et, trig, sizeof trig, &lat, &unc)) continue;
                    if (strstr(trig, g_names[g_state[i]])) { li = k; latk = k + 1; }
                }
                if (li >= 0) {
                    double et, lat, unc;
                    char trig[64];
                    lat_used[li] = 1;
                    (void)parse_lat(g_lat[li].text, &et, trig, sizeof trig, &lat, &unc);
                    lat_us = lat * 1e6;
                    v_lat[n_lat++] = lat_us;
                    if (!isnan(es)) v_x[n_x++] = es - lat_us;
                } else missing_lat++;
                SDL_UnlockMutex(g_lat_mx);
            }
            if (csv)
                fprintf(csv, "%d,%d,%lld,%lld,%.6f,%lld,%.1f,%.1f,%.1f,%.1f\n", i, g_state[i], (long long)onset,
                        (long long)stamp, lsl_s, best >= 0 ? (long long)g_edge[best].t : 0LL,
                        best >= 0 ? (double)(g_edge[best].t - onset) / 1e3 : NAN, es, lat_us,
                        (!isnan(es) && !isnan(lat_us)) ? es - lat_us : NAN);
        }
    }
    if (csv) fclose(csv);
    printf("changes %d; ysp edges missing %d, extra %d; latencies %d received, missing %d (%d matched by event_time)\n",
           changes, missing_edge, g_ne - n_eo, g_nlat, use_lat ? missing_lat : 0, by_time);
    dist("onset - stamp", v_os, n_os);
    dist("edge - onset", v_eo, n_eo);
    dist("edge - stamp", v_es, n_es);
    if (use_lat) {
        dist("LabStreamer latency", v_lat, n_lat);
        dist("edge-stamp - latency", v_x, n_x);
    }
    if (g_lat_mx) SDL_DestroyMutex(g_lat_mx);
    ynet_lsl_close(&g_lib);
    return (changes > 0 && missing_edge == 0 && (!use_lat || missing_lat == 0)) ? 0 : 1;
}
