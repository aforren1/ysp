/* probe_c.c - the ysp-av probe's frame loop in C: the reference that the
 * Python, MATLAB and Octave loops are compared with (docs/av_probe.md).
 *
 * Usage: probe_c [--windowed] [--n N] [--seconds S] [--csv PATH]
 *                [--idle S] [--abort] [--panic] [--audio null|default]
 *                [--angle-dir DIR] [--preload DLL] [--modules]
 *                [--latency key|click [--lat-frames] [--lat-n N]] [--dxgi]
 * YSCR_WINDOW_THREAD=1 in the environment gives the window a thread of its
 * own (docs/winthread_probe.md).
 * Each frame does what the host loops do: begin, evaluate, then per
 * stimulus one parameter set and one draw, end, flip_at(predicted onset).
 * Exit code: 0; 1 when open failed; 2 for a bad argument.
 */
#include "probe_core.h"

static void print_modules(const char* when) {
    static char buf[16384];
    int n = pv_modules(buf, sizeof buf, 0);
    printf("modules %s (%d loaded):\n%s", when, n, buf);
}

static int run_frames(int64_t frames, int64_t until_ns) {
    int rc = YSCR_OK;
    int64_t k;
    for (k = 0; frames < 0 || k < frames; k++) {
        if (until_ns && yrt_now_ns() >= until_ns) break;
        rc = pv_frame_c(1);
        if (rc != YSCR_OK) break;
    }
    return rc;
}

int main(int argc, char** argv) {
    pv_opts o;
    int i, n = 1, abort_test = 0, modules = 0, lat = -1, lat_frames = 0, lat_n = 200;
    double seconds = 5, idle = 0;
    const char *csv = NULL, *audio = NULL, *preload = NULL;
    static char buf[16384];
    int rc;

    memset(&o, 0, sizeof o);
    o.w = 1280; o.h = 720;
    o.foreground = YSCR_FOREGROUND_FORCE;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--windowed")) o.windowed = 1;
        else if (!strcmp(argv[i], "--n") && i + 1 < argc) n = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "--csv") && i + 1 < argc) csv = argv[++i];
        else if (!strcmp(argv[i], "--idle") && i + 1 < argc) idle = atof(argv[++i]);
        else if (!strcmp(argv[i], "--abort")) abort_test = 1;
        else if (!strcmp(argv[i], "--panic")) { abort_test = 3; o.panic = 1; }
        else if (!strcmp(argv[i], "--audio") && i + 1 < argc) audio = argv[++i];
        else if (!strcmp(argv[i], "--angle-dir") && i + 1 < argc) o.angle_dir = argv[++i];
        else if (!strcmp(argv[i], "--preload") && i + 1 < argc) preload = argv[++i];
        else if (!strcmp(argv[i], "--modules")) modules = 1;
        else if (!strcmp(argv[i], "--latency") && i + 1 < argc) lat = !strcmp(argv[++i], "click");
        else if (!strcmp(argv[i], "--lat-frames")) lat_frames = 1;
        else if (!strcmp(argv[i], "--dxgi")) o.backend = YSCR_BACKEND_DXGI_FLIP;
        else if (!strcmp(argv[i], "--lat-n") && i + 1 < argc) lat_n = atoi(argv[++i]);
        else { fprintf(stderr, "probe_c: bad argument %s\n", argv[i]); return 2; }
    }
    yrt_thread_elevate(NULL);
    if (preload) printf("preload %s: %p\n", preload, pv_preload(preload));
    if (modules) print_modules("before open");
    if (!pv_open(&o)) { fprintf(stderr, "probe_c: %s\n", pv_last_error()); return 1; }
    if (pv_setup(n) < 0) { fprintf(stderr, "probe_c: setup failed: %s\n", ygfx_error(&pv->gfx)); pv_close(); pv_free(); return 1; }
    pv_describe(buf, sizeof buf);
    printf("%s\n", buf);
    if (audio) {
        rc = pv_audio_open(!strcmp(audio, "null"), buf, sizeof buf);
        printf("audio %s: %s: %s\n", audio, rc == 0 ? "open" : "FAILED", buf);
    }
    if (modules) print_modules("after open");

    if (abort_test) {
        /* frames until begin() reports the abort; panic: hang instead */
        run_frames(60, 0);
        pv_keys(abort_test, 500, 300);
        if (abort_test == 3) {
            printf("hanging the frame thread for 10 s; 3 presses follow\n");
            fflush(stdout);
            Sleep(10000);
            pv_wt_state(buf, sizeof buf);
            printf("panic did not end the process: %s\n", buf);
            rc = run_frames(-1, yrt_now_ns() + YRT_S(2));
            printf("after the panic: rc=%d abort=0x%x presses=%d\n", rc, pv->f.abort, pv->f.abort_presses);
        } else {
            rc = run_frames(-1, yrt_now_ns() + YRT_S(5));
            printf("abort: rc=%d abort=0x%x presses=%d keys_sent=%ld skipped=%ld\n", rc, pv->f.abort,
                   pv->f.abort_presses, (long)pv->keys_sent, (long)pv->keys_skipped);
        }
    } else if (lat >= 0) {
        run_frames(60, 0);
        pv_latency(lat, lat_n, lat_frames, buf, sizeof buf);
        printf("%s", buf);
    } else if (idle > 0) {
        int64_t first;
        run_frames(120, 0);
        first = pv->n_frames;
        pv_watch_start();
        Sleep((DWORD)(idle * 1000));
        pv_watch_stop(buf, sizeof buf);
        printf("idle %.1f s, watcher:\n%s", idle, buf);
        pv_watch_start();
        run_frames(120, 0);
        pv_watch_stop(buf, sizeof buf);
        printf("after idle, watcher:\n%s", buf);
        printf("first frame after idle: index %lld\n", (long long)first);
    } else {
        rc = run_frames(-1, yrt_now_ns() + (int64_t)(seconds * 1e9));
        if (rc != YSCR_OK) printf("loop ended: rc=%d %s\n", rc, yscr_error(&pv->scr));
    }
    pv_close();
    if (csv) {
        char tag[256];
        snprintf(tag, sizeof tag, "host=C n=%d windowed=%d seconds=%.0f", n, o.windowed, seconds);
        printf("dumped %d frames to %s\n", pv_dump(csv, tag), csv);
    }
    printf("frames %lld records %lld lost %lld\n", (long long)pv->n_frames, (long long)pv->n_recs, (long long)pv->lost);
    pv_free();
    return 0;
}
