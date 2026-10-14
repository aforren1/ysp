/* audio_first_sound.c - the first sound after a silence, against a repeat
 * (docs/rig_spec.md 13): does the output path wake late or quiet?
 *
 *     audio_first_sound [--sim [--sim-gate-ms G]] [--device OUT] [--in IN]
 *                       [--exclusive] [--gaps S,S,...] [--db DB] [--edge-db DB]
 *                       [--channel C] [--volume V] [--unmute] [--in-volume V]
 *                       [--in-unmute] [--csv FILE]
 *
 *   --gaps S,...   seconds of digital silence before each first sound
 *                  (default 1,10,60)
 *   --db DB        the burst's peak, dBFS (default -20; at most -6)
 *   --sim          no hardware: the simulated loop of audio_loopback_latency,
 *                  20 ms long, with gaps of 0.3,1,2 s unless --gaps; with
 *                  --sim-gate-ms G its sink mutes the first G ms after 250 ms
 *                  of silence. Exit 1 unless the measured difference is the
 *                  gate within 1.5 ms (0 without one). CI runs both
 *   the rest as audio_loopback_latency
 *
 * For each gap: the gap's silence, then sound A, then 200 ms later sound
 * B, the repeat (150 ms of silence between them). Each is a 1 kHz burst of 50 ms with 2 ms raised-cosine
 * ramps. Both go through the output (yau_play_at, 200 ms ahead) and come
 * back through the input; an edge detector finds each one's onset, and its
 * level is the RMS of the 40 ms after the onset. The program prints, per
 * gap: A's and B's latency (input fit's edge time - output fit's onset),
 * A minus B, and A's level minus B's. A path that gates on digital silence
 * shows a later onset and a lower level for A.
 *
 * Exit: 0 when every sound was found (and --sim met its check), 1 when a
 * device did not open, a sound was missing or --sim failed, 2 usage.
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#endif

#define YSP_AUDIO_IMPLEMENTATION
#include "ysp/audio.h"
#include "loop_common.h"

static yau_audio g_au;
static yau_capture g_cap;
static lp_rec g_rec;

#define MAX_GAPS 16

static void pump(void) {
    (void)yau_update(&g_au);
    lp_drain(&g_cap, &g_rec);
}
static void wait_until(int64_t t) {
    while (yrt_now_ns() < t) {
        int64_t s = yrt_now_ns() + 10000000;
        yrt_sleep_until(s < t ? s : t, 0);
        pump();
    }
}

int main(int argc, char** argv) {
    yau_desc d;
    yau_capture_desc cd;
    yau_tone_desc td;
    yau_buf burst;
    double gaps[MAX_GAPS], db = -20, edge_db = 1, gate_ms = 0, volume = -1, in_volume = -1;
    int ngaps = 0, i, sim = 0, channel = 0, unmute = 0, in_unmute = 0, rc = 0, ok = 1;
    const char *out = NULL, *in = NULL, *csvp = NULL;
    FILE* csv = NULL;
    char line[640];
    int64_t onset[2 * MAX_GAPS];
    yau_id ids[2 * MAX_GAPS];
    double noise_db;
    char gapbuf[256] = "";
#if defined(_WIN32)
    lp_ep eo, ei;
    memset(&eo, 0, sizeof eo);
    memset(&ei, 0, sizeof ei);
#endif
    memset(&d, 0, sizeof d);
    memset(&cd, 0, sizeof cd);
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--sim")) sim = 1;
        else if (!strcmp(argv[i], "--sim-gate-ms") && i + 1 < argc) gate_ms = atof(argv[++i]);
        else if (!strcmp(argv[i], "--device") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "--in") && i + 1 < argc) in = argv[++i];
        else if (!strcmp(argv[i], "--exclusive")) d.exclusive = true;
        else if (!strcmp(argv[i], "--gaps") && i + 1 < argc) snprintf(gapbuf, sizeof gapbuf, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--db") && i + 1 < argc) db = atof(argv[++i]);
        else if (!strcmp(argv[i], "--edge-db") && i + 1 < argc) edge_db = atof(argv[++i]);
        else if (!strcmp(argv[i], "--channel") && i + 1 < argc) channel = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--volume") && i + 1 < argc) volume = atof(argv[++i]);
        else if (!strcmp(argv[i], "--unmute")) unmute = 1;
        else if (!strcmp(argv[i], "--in-volume") && i + 1 < argc) in_volume = atof(argv[++i]);
        else if (!strcmp(argv[i], "--in-unmute")) in_unmute = 1;
        else if (!strcmp(argv[i], "--csv") && i + 1 < argc) csvp = argv[++i];
        else { fprintf(stderr, "audio_first_sound: unknown argument %s (see the top of the file)\n", argv[i]); return 2; }
    }
    /* --sim runs on ysp/rt.h's virtual clock: the same records on any host */
    if (sim) yrt_virtual_begin(0);
    if (!gapbuf[0]) snprintf(gapbuf, sizeof gapbuf, "%s", sim ? "0.3,1,2" : "1,10,60");
    {
        char* p = gapbuf;
        while (*p && ngaps < MAX_GAPS) {
            gaps[ngaps] = strtod(p, &p);
            if (gaps[ngaps] < 0.1 || gaps[ngaps] > 600) { fprintf(stderr, "--gaps: 0.1 to 600 s each\n"); return 2; }
            ngaps++;
            if (*p == ',') p++;
            else if (*p) { fprintf(stderr, "--gaps: seconds separated by commas\n"); return 2; }
        }
    }
    if (db > -6 || volume > 0.5 || in_volume > 1 || channel < 0 || gate_ms < 0 || gate_ms > 40 || (edge_db >= 0 && edge_db != 1)) {
        fprintf(stderr, "audio_first_sound: bad option (db <= -6, volume <= 0.5, sim-gate-ms 0..40, edge-db < 0)\n");
        return 2;
    }
    d.device = out;
    d.arena_bytes = 1 << 20;
    cd.device = in;
    cd.frames = 4 * 48000;
    cd.format.sample = YAU_S16;   /* any input of 16 bits or more */
    if (sim) {
        if (lp_sim_init(&g_lp, 20, gate_ms, 250) < 0) return 1;
        d.backend = YAU_BACKEND_CUSTOM; d.dev = &g_lp_out; d.dev_ctx = &g_lp;
        cd.backend = YAU_BACKEND_CUSTOM; cd.dev = &g_lp_in; cd.dev_ctx = &g_lp;
    }
#if defined(_WIN32)
    if (!sim) {
        if (lp_ep_open(&eo, 0, out) == 0) { lp_ep_set(&eo, unmute, (float)volume); lp_ep_print(&eo, "output"); }
        if (lp_ep_open(&ei, 1, in) == 0) { lp_ep_set(&ei, in_unmute, (float)in_volume); lp_ep_print(&ei, "input"); }
    }
#else
    if (volume >= 0 || unmute || in_volume >= 0 || in_unmute) printf("volume and mute: Windows only; set them by hand\n");
#endif
    if (!yau_capture_open(&g_cap, &cd)) { fprintf(stderr, "%s\n", yau_capture_error(&g_cap)); rc = 1; goto restore; }
    if (!yau_open(&g_au, &d)) { fprintf(stderr, "%s\n", yau_error(&g_au)); yau_capture_close(&g_cap); rc = 1; goto restore; }
    g_rec.ch = channel < g_cap.dcaps.channels ? channel : 0;
    {
        double total = 2.0;
        for (i = 0; i < ngaps; i++) total += gaps[i] + 1.0;
        g_rec.cap = (int64_t)((total + 4.0) * 48000.0);
        g_rec.x = (float*)calloc((size_t)g_rec.cap, sizeof(float));
        if (!g_rec.x) { rc = 1; goto done; }
    }
    {
        int64_t until = yrt_now_ns() + YRT_S(3);
        for (;;) {
            yau_capture_info ci;
            pump();
            (void)yau_capture_update(&g_cap);
            yau_capture_get_info(&g_cap, &ci);
            if (ci.fit_ready && g_au.fit_ft.ready) break;
            if (yrt_now_ns() > until) { fprintf(stderr, "the fits did not warm up in 3 s\n"); rc = 1; goto done; }
            yrt_sleep_until(yrt_now_ns() + 10000000, 0);
        }
    }
    yau_describe(&g_au, line, sizeof line);
    puts(line);
    yau_capture_describe(&g_cap, line, sizeof line);
    puts(line);
    {
        int64_t from = g_rec.n;
        wait_until(yrt_now_ns() + YRT_MS(500));
        noise_db = lp_rms_db(&g_rec, from, g_rec.n);
    }
    if (edge_db >= 0) edge_db = noise_db + 25 > -6 ? -6 : noise_db + 25;
    printf("input noise floor %.1f dBFS rms; edge level %.1f dBFS\n", noise_db, edge_db);
    memset(&td, 0, sizeof td);
    td.hz = 1000; td.dur_ns = YRT_MS(50); td.peak = yau_db((float)db); td.ramp_ns = YRT_MS(2);
    burst = yau_tone(&g_au, &td);
    if (!burst.frames) { fprintf(stderr, "%s\n", yau_error(&g_au)); rc = 1; goto done; }
    /* gap, A, 500 ms, B, for each gap; the output is all zeros between */
    {
        int64_t t = yrt_now_ns();
        for (i = 0; i < ngaps; i++) {
            int64_t ta = t + (int64_t)(gaps[i] * 1e9), tb = ta + YRT_MS(200);
            printf("gap %.1f s ...\n", gaps[i]);
            wait_until(ta - YRT_MS(200));
            ids[2 * i] = yau_play_at(&g_au, burst, ta);
            wait_until(tb - YRT_MS(200));
            ids[2 * i + 1] = yau_play_at(&g_au, burst, tb);
            t = tb + YRT_MS(50);
        }
        wait_until(t + YRT_MS(600));
    }
    for (i = 0; i < 2 * ngaps; i++) {
        yau_onset r;
        memset(&r, 0, sizeof r);
        onset[i] = ids[i] > 0 && yau_wait(&g_au, ids[i], YRT_S(1), &r) == YAU_OK && r.start_frame >= 0 ? r.onset : -1;
    }
    (void)yau_capture_update(&g_cap);
    {
        yau_edge ed;
        yau_edge_desc ek;
        static double edges[256];
        int ne, k = 0;
        memset(&ek, 0, sizeof ek);
        ek.level_db = (float)edge_db;
        ek.dead_ns = YRT_MS(100);
        (void)yau_edge_init(&ed, &ek, g_cap.dcaps.rate);
        ne = yau_edge_feed(&ed, g_rec.x, g_rec.n, 1, 0, edges, 256);
        if (csvp && (csv = fopen(csvp, "w")) != NULL) fprintf(csv, "gap_s,sound,onset_ns,edge_ns,latency_us,level_db\n");
        printf("%8s  %10s  %10s  %10s  %10s  %10s  %10s\n", "gap s", "A lat ms", "B lat ms", "A-B ms", "A dBFS", "B dBFS", "A-B dB");
        for (i = 0; i < ngaps; i++) {
            double latv[2] = { -1, -1 }, lev[2] = { -999, -999 };
            int s;
            for (s = 0; s < 2; s++) {
                int64_t o = onset[2 * i + s], te = -1;
                double ef = -1;
                if (o < 0) continue;
                for (; k < ne; k++) {
                    int64_t tt = yau_capture_time_at(&g_cap, edges[k]);
                    if (tt < o - YRT_MS(2)) continue;
                    if (tt < o + YRT_MS(150)) { te = tt; ef = edges[k]; k++; }
                    break;
                }
                if (te < 0) continue;
                latv[s] = (double)(te - o) / 1e6;
                lev[s] = lp_rms_db(&g_rec, (int64_t)ef, (int64_t)ef + 40 * 48);
                if (csv) fprintf(csv, "%.3f,%c,%lld,%lld,%.1f,%.2f\n", gaps[i], s ? 'B' : 'A', (long long)o, (long long)te,
                                 latv[s] * 1e3, lev[s]);
            }
            if (latv[0] < 0 || latv[1] < 0) { printf("%8.1f  a sound was not found\n", gaps[i]); ok = 0; continue; }
            printf("%8.1f  %10.3f  %10.3f  %10.3f  %10.2f  %10.2f  %10.2f\n", gaps[i], latv[0], latv[1], latv[0] - latv[1],
                   lev[0], lev[1], lev[0] - lev[1]);
            if (sim && gaps[i] >= 0.3) {
                double want = gate_ms;
                /* the gate mutes the start of the burst: its onset moves by
                 * the gate, less the ramp's own time to the edge level */
                if (fabs((latv[0] - latv[1]) - want) > 1.5 + (want > 0 ? 2.0 : 0.0)) {
                    printf("sim: FAIL (A-B %.3f ms, the gate %.1f ms)\n", latv[0] - latv[1], want);
                    ok = 0;
                }
            }
        }
        if (csv) fclose(csv);
    }
    {
        yau_capture_info ci;
        yau_caps pc;
        yau_get_caps(&g_au, &pc);
        yau_capture_get_info(&g_cap, &ci);
        printf("output fit: drift %+.2f ppm, spread %.0f ns, xruns %u; input fit: drift %+.2f ppm, spread %.0f ns, xruns %u, dropped %lld\n",
               pc.drift_ppm, pc.fit_spread_ns, (unsigned)pc.xruns, ci.drift_ppm, ci.fit_spread_ns, (unsigned)ci.xruns,
               (long long)ci.dropped);
    }
    if (!ok) rc = 1;
done:
    yau_close(&g_au);
    yau_capture_close(&g_cap);
    free(g_rec.x);
restore:
#if defined(_WIN32)
    lp_ep_restore(&eo);
    lp_ep_restore(&ei);
#endif
    return rc;
}
