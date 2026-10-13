/* audio_loopback_latency.c - output-to-input latency through a loop: a
 * cable from line out to line in, or a speaker heard by a microphone.
 *
 *     audio_loopback_latency [--sim [--sim-delay-ms D]] [--device OUT] [--in IN]
 *                            [--exclusive] [--in-exclusive] [--duplex] [--paths]
 *                            [--clicks N] [--every-ms MS] [--db DB] [--edge-db DB]
 *                            [--channel C] [--volume V] [--unmute]
 *                            [--in-volume V] [--in-unmute] [--distance-cm CM] [--burst-ms MS]
 *                            [--csv FILE]
 *
 *   --sim          no hardware: an output and an input on one timer thread,
 *                  the input hearing the output --sim-delay-ms later (default
 *                  20). Exit 1 if the median is not the delay minus a period
 *                  within 1 ms (CI runs this)
 *   --device OUT, --in IN   the output and input whose names contain these;
 *                  default the OS's
 *   --exclusive, --in-exclusive   WASAPI exclusive mode for either side
 *   --duplex       one device plays and records (desc.capture, DUPLEX)
 *   --paths        run twice: shared, then exclusive output (the same input)
 *   --clicks N     clicks per path (default 50), --every-ms apart (default 400)
 *   --db DB        the click's level, dBFS (default -20; at most -6)
 *   --edge-db DB   the input level that is an edge (default: the noise
 *                  floor measured in the first 0.5 s plus 25 dB)
 *   --channel C    the input channel to listen on (default 0)
 *   --volume V, --unmute   Windows: the output endpoint's volume scalar and
 *                  mute for the run (restored after; V at most 0.5)
 *   --in-volume V, --in-unmute   the same for the input endpoint
 *   --burst-ms MS  a 2 kHz tone burst of MS ms (0.5 ms ramps) instead of the
 *                  click: more energy for small speakers
 *   --distance-cm CM  printed with the result: speaker to microphone through
 *                  air (343 m/s: 2.9 us a mm)
 *   --csv FILE     one row per click
 *
 * A click is 12 frames (0.25 ms) at --db, one every --every-ms, each played
 * by yau_play_at() 200 ms ahead. Its onset is the output fit's time for its
 * first frame (the record). The input is read into memory; an edge
 * detector finds each click; the edge's time is the input fit's time for
 * its interpolated frame (yau_capture_time_at). Latency = edge time - onset.
 * The two fits are separate clocks, each from its own device's reports or
 * callbacks: the program prints each fit, its source, drift and spread,
 * then the latency per click: min, p1, p50, p99, max, sd. Neither fit is an
 * observation of the sound; the latency is the loop's sum: output pipeline,
 * DAC, cable or air, ADC, input pipeline, less what each fit absorbs. On
 * one device (--duplex, one converter clock) the drift of the two fits is
 * the same and the latency should not move over the run.
 *
 * Exit: 0 when every click was found (and --sim met its check), 1 when a
 * device did not open, clicks were missing, or --sim failed, 2 usage.
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

typedef struct opt {
    int sim, exclusive, in_exclusive, duplex, clicks, channel, unmute, in_unmute;
    double sim_delay_ms, every_ms, db, edge_db, volume, in_volume, distance_cm, burst_ms;
    const char *out, *in, *csv;
} opt;

/* Both fits warm, or 3 s: a CUSTOM device's open does not wait. */
static int wait_fits(int64_t until) {
    for (;;) {
        yau_capture_info in;
        (void)yau_update(&g_au);
        (void)yau_capture_update(&g_cap);
        lp_drain(&g_cap, &g_rec);
        yau_capture_get_info(&g_cap, &in);
        if (in.fit_ready && g_au.fit_ft.ready) return 0;
        if (yrt_now_ns() > until) return -1;
        yrt_sleep_until(yrt_now_ns() + 10000000, 0);
    }
}

static void print_fits(void) {
    yau_caps pc;
    yau_capture_info ci;
    (void)yau_capture_update(&g_cap);
    yau_get_caps(&g_au, &pc);
    yau_capture_get_info(&g_cap, &ci);
    printf("output fit: %s, drift %+.2f ppm, spread %.0f ns, %u points, worst tier %d, xruns %u\n",
           pc.pos_source == YAU_POS_DEVICE ? "position reports" : "callback times", pc.drift_ppm, pc.fit_spread_ns,
           (unsigned)pc.fit_points, (int)pc.worst_tier, (unsigned)pc.xruns);
    printf("input fit:  %s, drift %+.2f ppm, spread %.0f ns, %u points, tier %d, xruns %u, lost %lld, dropped %lld\n",
           ci.pos_source == YAU_POS_DEVICE ? "position reports" : "callback times", ci.drift_ppm, ci.fit_spread_ns,
           (unsigned)ci.fit_points, (int)ci.tier, (unsigned)ci.xruns, (long long)ci.lost, (long long)ci.dropped);
    printf("the two clocks differ by %+.2f ppm\n", ci.drift_ppm - pc.drift_ppm);
}

static int run_path(const opt* o, int exclusive, FILE* csv) {
    yau_desc d;
    yau_capture_desc cd;
    yau_click_desc kd;
    yau_buf click;
    char line[640];
    int i, found = 0, rc = 0, nclick = o->clicks;
    int64_t* onset = (int64_t*)calloc((size_t)nclick, sizeof(int64_t));
    yau_id* ids = (yau_id*)calloc((size_t)nclick, sizeof(yau_id));
    double* lat = (double*)calloc((size_t)nclick, sizeof(double));
    double* edges = (double*)calloc((size_t)nclick * 4 + 16, sizeof(double));
    int64_t t0;
    double noise_db, edge_db;
    yau_edge ed;
    yau_edge_desc ekd;
    int nedge = 0;
    if (!onset || !ids || !lat || !edges) return 1;
    memset(&d, 0, sizeof d);
    memset(&cd, 0, sizeof cd);
    d.device = o->out;
    d.exclusive = exclusive != 0;
    d.arena_bytes = 1 << 16;
    cd.device = o->in;
    cd.exclusive = o->in_exclusive != 0;
    cd.frames = 4 * 48000;
    cd.format.sample = YAU_S16;   /* any input of 16 bits or more: a microphone in exclusive mode gives 16 */
    if (o->sim) {
        if (lp_sim_init(&g_lp, o->sim_delay_ms, 0, 0) < 0) return 1;
        d.backend = YAU_BACKEND_CUSTOM; d.dev = &g_lp_out; d.dev_ctx = &g_lp;
        cd.backend = YAU_BACKEND_CUSTOM; cd.dev = &g_lp_in; cd.dev_ctx = &g_lp;
    }
    if (o->duplex) { d.capture = &g_cap; d.capture_desc = &cd; }
    else if (!yau_capture_open(&g_cap, &cd)) { fprintf(stderr, "%s\n", yau_capture_error(&g_cap)); return 1; }
    if (!yau_open(&g_au, &d)) {
        fprintf(stderr, "%s\n", yau_error(&g_au));
        yau_capture_close(&g_cap);
        return 1;
    }
    g_rec.ch = o->channel;
    if (g_rec.ch >= g_cap.dcaps.channels) { fprintf(stderr, "--channel %d: the input has %u\n", g_rec.ch, (unsigned)g_cap.dcaps.channels); rc = 2; goto done; }
    g_rec.cap = (int64_t)(((double)nclick * o->every_ms / 1000.0 + 6.0) * 48000.0);
    g_rec.x = (float*)calloc((size_t)g_rec.cap, sizeof(float));
    g_rec.n = 0;
    if (!g_rec.x) { rc = 1; goto done; }
    if (wait_fits(yrt_now_ns() + YRT_S(3)) < 0) { fprintf(stderr, "the fits did not warm up in 3 s\n"); rc = 1; goto done; }
    yau_describe(&g_au, line, sizeof line);
    puts(line);
    yau_capture_describe(&g_cap, line, sizeof line);
    puts(line);
    /* the noise floor, 0.5 s of nothing */
    {
        int64_t from = g_rec.n, end = yrt_now_ns() + YRT_MS(500);
        while (yrt_now_ns() < end) { yrt_sleep_until(yrt_now_ns() + 10000000, 0); lp_drain(&g_cap, &g_rec); }
        noise_db = lp_rms_db(&g_rec, from, g_rec.n);
    }
    edge_db = o->edge_db < 0 ? o->edge_db : noise_db + 25;
    if (edge_db > -6) edge_db = -6;
    printf("input noise floor %.1f dBFS rms; edge level %.1f dBFS\n", noise_db, edge_db);
    memset(&kd, 0, sizeof kd);
    kd.dur_ns = YRT_US(250);
    kd.peak = yau_db((float)o->db);
    if (o->burst_ms > 0) {
        /* a 2 kHz burst with 0.5 ms ramps: more energy for small speakers */
        yau_tone_desc td;
        memset(&td, 0, sizeof td);
        td.hz = 2000; td.dur_ns = (int64_t)(o->burst_ms * 1e6); td.peak = kd.peak; td.ramp_ns = YRT_US(500);
        click = yau_tone(&g_au, &td);
    } else {
        click = yau_click(&g_au, &kd);
    }
    if (!click.frames) { fprintf(stderr, "%s\n", yau_error(&g_au)); rc = 1; goto done; }
    t0 = yrt_now_ns() + YRT_MS(300);
    for (i = 0; i < nclick; i++) {
        int64_t t = t0 + (int64_t)((double)i * o->every_ms * 1e6);
        yrt_sleep_until(t - YRT_MS(200), 0);
        ids[i] = yau_play_at(&g_au, click, t);
        if (ids[i] < 0) { fprintf(stderr, "%s\n", yau_error(&g_au)); rc = 1; goto done; }
        (void)yau_update(&g_au);
        lp_drain(&g_cap, &g_rec);
    }
    {
        int64_t end = t0 + (int64_t)((double)nclick * o->every_ms * 1e6) + YRT_MS(600);
        while (yrt_now_ns() < end) { yrt_sleep_until(yrt_now_ns() + 10000000, 0); lp_drain(&g_cap, &g_rec); (void)yau_update(&g_au); }
    }
    for (i = 0; i < nclick; i++) {
        yau_onset r;
        memset(&r, 0, sizeof r);
        if (yau_wait(&g_au, ids[i], YRT_S(1), &r) == YAU_OK && r.start_frame >= 0) onset[i] = r.onset;
        else onset[i] = -1;
        if (i == 0) printf("first click: tier %d, flags 0x%03x\n", r.tier, (unsigned)r.flags);
    }
    print_fits();
    {
        /* the loudest the input got during the clicks: an acoustic run that
         * finds no edge shows here whether anything came back */
        double pk = 0;
        int64_t j, from = yau_capture_frame_at(&g_cap, t0) - 4800;
        for (j = from < 0 ? 0 : from; j < g_rec.n; j++) if (fabs((double)g_rec.x[j]) > pk) pk = fabs((double)g_rec.x[j]);
        printf("input peak during the clicks: %.1f dBFS\n", pk > 0 ? 20 * log10(pk) : -999.0);
    }
    /* the edges, then each click's: the first edge at or after its onset
     * minus 2 ms and before the next click */
    memset(&ekd, 0, sizeof ekd);
    ekd.level_db = (float)edge_db;
    ekd.dead_ns = (int64_t)(o->every_ms * 0.5e6);
    if (yau_edge_init(&ed, &ekd, g_cap.dcaps.rate) == 0)
        nedge = yau_edge_feed(&ed, g_rec.x, g_rec.n, 1, 0, edges, nclick * 4 + 16);
    {
        int k = 0, nl = 0;
        double s1 = 0, s2 = 0;
        for (i = 0; i < nclick; i++) {
            int64_t te = -1;
            double ef = -1;
            if (onset[i] < 0) continue;
            for (; k < nedge; k++) {
                int64_t t = yau_capture_time_at(&g_cap, edges[k]);
                if (t < onset[i] - YRT_MS(2)) continue;
                if (t < onset[i] + (int64_t)(o->every_ms * 1e6) - YRT_MS(2)) { te = t; ef = edges[k]; k++; }
                break;
            }
            if (csv) fprintf(csv, "%s,%d,%lld,%.4f,%lld,%.1f\n", exclusive ? "exclusive" : "shared", i, (long long)onset[i],
                             ef, (long long)te, te >= 0 ? (double)(te - onset[i]) / 1e3 : -1.0);
            if (te < 0) continue;
            found++;
            lat[nl] = (double)(te - onset[i]) / 1e6;
            s1 += lat[nl]; s2 += lat[nl] * lat[nl];
            nl++;
        }
        printf("path: output %s, input %s%s; %d clicks, %d edges, %d found\n", exclusive ? "exclusive" : "shared",
               o->in_exclusive ? "exclusive" : "shared", o->duplex ? ", one duplex device" : "", nclick, nedge, found);
        if (nl > 0) {
            double mean = s1 / nl, sd = sqrt(s2 / nl - mean * mean > 0 ? s2 / nl - mean * mean : 0);
            qsort(lat, (size_t)nl, sizeof lat[0], lp_cmp_d);
            printf("latency (input fit's edge time - output fit's onset), ms: min %.3f p1 %.3f p50 %.3f p99 %.3f max %.3f sd %.3f\n",
                   lat[0], lp_pct(lat, nl, 0.01), lp_pct(lat, nl, 0.5), lp_pct(lat, nl, 0.99), lat[nl - 1], sd);
            if (o->distance_cm > 0)
                printf("air: %.1f cm at 343 m/s is %.3f ms of it\n", o->distance_cm, o->distance_cm / 34.3);
            if (o->sim) {
                double want = o->sim_delay_ms - 10.0;
                printf("sim: the loop's delay minus a period is %.3f ms\n", want);
                if (fabs(lp_pct(lat, nl, 0.5) - want) > 1.0) { printf("sim: FAIL\n"); rc = 1; }
            }
        }
        if (found < nclick) rc = rc ? rc : 1;
    }
done:
    if (o->duplex) yau_close(&g_au);
    else { yau_close(&g_au); yau_capture_close(&g_cap); }
    free(g_rec.x); g_rec.x = NULL;
    free(onset); free(ids); free(lat); free(edges);
    return rc;
}

int main(int argc, char** argv) {
    opt o;
    int i, rc, paths = 0;
    FILE* csv = NULL;
#if defined(_WIN32)
    lp_ep eo, ei;
    memset(&eo, 0, sizeof eo);
    memset(&ei, 0, sizeof ei);
#endif
    memset(&o, 0, sizeof o);
    o.clicks = 50; o.every_ms = 400; o.db = -20; o.sim_delay_ms = 20; o.volume = -1; o.in_volume = -1; o.edge_db = 1;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--sim")) o.sim = 1;
        else if (!strcmp(argv[i], "--sim-delay-ms") && i + 1 < argc) o.sim_delay_ms = atof(argv[++i]);
        else if (!strcmp(argv[i], "--device") && i + 1 < argc) o.out = argv[++i];
        else if (!strcmp(argv[i], "--in") && i + 1 < argc) o.in = argv[++i];
        else if (!strcmp(argv[i], "--exclusive")) o.exclusive = 1;
        else if (!strcmp(argv[i], "--in-exclusive")) o.in_exclusive = 1;
        else if (!strcmp(argv[i], "--duplex")) o.duplex = 1;
        else if (!strcmp(argv[i], "--paths")) paths = 1;
        else if (!strcmp(argv[i], "--clicks") && i + 1 < argc) o.clicks = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--every-ms") && i + 1 < argc) o.every_ms = atof(argv[++i]);
        else if (!strcmp(argv[i], "--db") && i + 1 < argc) o.db = atof(argv[++i]);
        else if (!strcmp(argv[i], "--edge-db") && i + 1 < argc) o.edge_db = atof(argv[++i]);
        else if (!strcmp(argv[i], "--channel") && i + 1 < argc) o.channel = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--volume") && i + 1 < argc) o.volume = atof(argv[++i]);
        else if (!strcmp(argv[i], "--unmute")) o.unmute = 1;
        else if (!strcmp(argv[i], "--in-volume") && i + 1 < argc) o.in_volume = atof(argv[++i]);
        else if (!strcmp(argv[i], "--in-unmute")) o.in_unmute = 1;
        else if (!strcmp(argv[i], "--distance-cm") && i + 1 < argc) o.distance_cm = atof(argv[++i]);
        else if (!strcmp(argv[i], "--burst-ms") && i + 1 < argc) o.burst_ms = atof(argv[++i]);
        else if (!strcmp(argv[i], "--csv") && i + 1 < argc) o.csv = argv[++i];
        else { fprintf(stderr, "audio_loopback_latency: unknown argument %s (see the top of the file)\n", argv[i]); return 2; }
    }
    if (o.clicks < 2 || o.clicks > 2000 || o.every_ms < 50 || o.every_ms > 5000 || o.db > -6 || o.volume > 0.5
        || o.in_volume > 1 || o.channel < 0 || o.burst_ms < 0 || o.burst_ms > 100 || o.sim_delay_ms < 10 || o.sim_delay_ms > 500 || (o.edge_db >= 0 && o.edge_db != 1)
        || (o.duplex && (o.in_exclusive != o.exclusive || paths))) {
        fprintf(stderr, "audio_loopback_latency: bad option (clicks 2..2000, every-ms 50..5000, db <= -6, volume <= 0.5, "
                        "edge-db < 0; --duplex shares one mode and has no --paths)\n");
        return 2;
    }
    if (o.csv && (csv = fopen(o.csv, "w")) != NULL) fprintf(csv, "path,click,onset_ns,edge_frame,edge_ns,latency_us\n");
#if defined(_WIN32)
    if (!o.sim) {
        if (lp_ep_open(&eo, 0, o.out) == 0) { lp_ep_set(&eo, o.unmute, (float)o.volume); lp_ep_print(&eo, "output"); }
        if (lp_ep_open(&ei, 1, o.in) == 0) { lp_ep_set(&ei, o.in_unmute, (float)o.in_volume); lp_ep_print(&ei, "input"); }
    }
#else
    if (o.volume >= 0 || o.unmute || o.in_volume >= 0 || o.in_unmute) printf("volume and mute: Windows only; set them by hand\n");
#endif
    rc = run_path(&o, o.exclusive, csv);
    if (paths && rc != 2) {
        int rc2 = run_path(&o, !o.exclusive, csv);
        if (rc2 > rc) rc = rc2;
    }
#if defined(_WIN32)
    lp_ep_restore(&eo);
    lp_ep_restore(&ei);
#endif
    if (csv) fclose(csv);
    return rc;
}
