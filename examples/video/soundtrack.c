/* video_soundtrack.c - a movie with its WAV on ysp/audio.h's clock, on the
 * real display and sound device: docs/video.md's "Soundtrack" measurement
 * as a program. Each display frame: yau_update(), yvid_follow_audio(),
 * yvid_update(), the draw at 1:1, the flip. It prints every number that
 * section's tables report, from the records of the movie, the sound and the
 * flips: the times are ysp/screen.h's flip onsets and ysp/audio.h's fit, so
 * this checks the lock between two clocks, not light against sound.
 *
 * Usage: video_soundtrack --file CLIP --wav WAV [--seconds S] [--script]
 *                         [--fullscreen] [--window WxH] [--audio auto|alsa|pulse|null]
 *                         [--db G] [--elevate] [--hw auto|sw|vaapi|dxva] [--device PATH] [--threads N] [--csv PATH]
 *   --seconds   stop after S seconds (default: the movie's end)
 *   --script    at 20 s a seek to the movie's middle, at 36 s a pause, at
 *               38 s a resume: the run of docs/video.md
 *   --audio     the backend (default auto: on Linux ALSA exclusive, which
 *               suspends PulseAudio while it runs)
 *   --db        the soundtrack's gain (default -20)
 *   --rate      the project rate in Hz (default 48000): the WAV's and the
 *               device's, with 16-bit samples: PulseAudio here runs at
 *               44100 Hz, s16
 *   --elevate   yrt_thread_elevate() before the screen opens
 *   --hw        the decoder (desc.hw_decode) of an MP4 file on FFmpeg
 *   --threads   the decoder's threads (desc.decode_threads) on FFmpeg
 *   --csv       the FRAME records: display, frame, decision, why, onset_ns,
 *               residual_ns, flags, ahead (frames ready after this one)
 * Shift+Esc ends it. Exit code: 0; 1 when something did not open or a frame
 * update failed; 2 for a bad argument. It needs a display and a sound
 * device, so it is not a ctest.
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#define YSP_AUDIO_IMPLEMENTATION
#include "ysp/audio.h"
#define YSP_VIDEO_IMPLEMENTATION
#include "ysp/video.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static yscr_screen scr;
static ygfx_gfx gfx;
static yvid_movie mv;
static yau_audio au;
static unsigned char vring_mem[YRT_RING_BYTES(1 << 16)];
static unsigned char aring_mem[YRT_RING_BYTES(1 << 12)];
static yrt_ring vring, aring;

static int64_t* res;      /* each shown frame on the audio clock: residual */
static int64_t n_res, cap_res;
static int64_t n_shown, n_repeated, n_late, n_est, drops_by[YVID_WHY_COUNT], reps_by[YVID_WHY_COUNT];
static int64_t n_sound_start, n_sound_stop, n_gaps, gap_frames, n_xruns, n_ends;
static double onset_lo = 1e18, onset_hi = -1e18;
static FILE* csv;

static int cmp_i64(const void* a, const void* b) {
    int64_t x = *(const int64_t*)a, y = *(const int64_t*)b;
    return x < y ? -1 : x > y;
}

static void drain(void) {
    yrt_event ev[256];
    int n, i;
    while ((n = yrt_ring_drain(&vring, ev, 256)) > 0) {
        for (i = 0; i < n; i++) {
            const yrt_event* e = &ev[i];
            if (e->kind == YVID_EV_FRAME) {
                unsigned dec = YVID_EV_DECISION_OF(e->u.u16[14]), why = YVID_EV_WHY_OF(e->u.u16[14]);
                int64_t residual = e->t_ns - e->u.i64[1];
                if (dec == YVID_SHOWN) {
                    n_shown++;
                    if (e->u.u32[6] & YVID_F_LATE) n_late++;
                    if (e->u.u32[6] & YVID_F_ESTIMATED) n_est++;
                    if ((e->u.u32[6] & YVID_F_AUDIO_CLOCK) && why == YVID_WHY_DUE) {
                        if (n_res == cap_res) {
                            int64_t* nr;
                            cap_res = cap_res ? cap_res * 2 : 8192;
                            nr = (int64_t*)realloc(res, (size_t)cap_res * sizeof *res);
                            if (!nr) { fprintf(stderr, "video_soundtrack: out of memory\n"); exit(1); }
                            res = nr;
                        }
                        res[n_res++] = residual;
                    }
                } else if (dec == YVID_REPEATED) {
                    n_repeated++;
                    if (why < YVID_WHY_COUNT) reps_by[why]++;
                }
                if (csv) fprintf(csv, "%u,%lld,%u,%u,%lld,%lld,%u,%u\n", (unsigned)e->u.u32[9], (long long)e->u.i64[0], dec, why,
                                 (long long)e->t_ns, (long long)residual, (unsigned)e->u.u32[6], (unsigned)e->u.u16[15]);
            } else if (e->kind == YVID_EV_DROP) {
                unsigned why = (unsigned)e->u.u16[14];
                if (why < YVID_WHY_COUNT) drops_by[why] += e->u.i64[1];
            } else if (e->kind == YVID_EV_SOUND) {
                if (e->u.u32[8] == 1) n_sound_start++;
                else n_sound_stop++;
            }
        }
    }
    while ((n = yrt_ring_drain(&aring, ev, 256)) > 0) {
        for (i = 0; i < n; i++) {
            const yrt_event* e = &ev[i];
            if (e->kind == YAU_EV_ONSET) {
                double r = (double)(e->t_ns - e->u.i64[0]);
                if (r < onset_lo) onset_lo = r;
                if (r > onset_hi) onset_hi = r;
                printf("video_soundtrack: sound ONSET, %.1f us from its target, tier %u\n", r / 1e3, YAU_EV_TIER_OF(e->u.u16[19]));
            } else if (e->kind == YAU_EV_GAP) {
                n_gaps++;
                gap_frames += e->u.i64[1];
            } else if (e->kind == YAU_EV_XRUN) {
                n_xruns++;
            } else if (e->kind == YAU_EV_END) {
                n_ends++;
            }
        }
    }
}

int main(int argc, char** argv) {
    const char* file = NULL;
    const char* wav = NULL;
    const char* csv_path = NULL;
    const char* device = NULL;
    double seconds = 0, db = -20;
    uint32_t rate = 0;
    int script = 0, fullscreen = 0, elevate = 0, ww = 640, wh = 360, i, threads = 0, failed = 0, step = 0, hw = 0;
    yau_backend backend = YAU_BACKEND_AUTO;
    yscr_frame f;
    ygfx_stim film;
    int64_t t_start = 0;
    char line[2048];
    int rc = YVID_OK;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--file") && i + 1 < argc) file = argv[++i];
        else if (!strcmp(argv[i], "--wav") && i + 1 < argc) wav = argv[++i];
        else if (!strcmp(argv[i], "--csv") && i + 1 < argc) csv_path = argv[++i];
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "--db") && i + 1 < argc) db = atof(argv[++i]);
        else if (!strcmp(argv[i], "--rate") && i + 1 < argc) rate = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--threads") && i + 1 < argc) threads = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--device") && i + 1 < argc) device = argv[++i];
        else if (!strcmp(argv[i], "--script")) script = 1;
        else if (!strcmp(argv[i], "--fullscreen")) fullscreen = 1;
        else if (!strcmp(argv[i], "--elevate")) elevate = 1;
        else if (!strcmp(argv[i], "--window") && i + 1 < argc && sscanf(argv[i + 1], "%dx%d", &ww, &wh) == 2) i++;
        else if (!strcmp(argv[i], "--hw") && i + 1 < argc) {
            const char* v = argv[++i];
            hw = !strcmp(v, "auto") ? YVID_HW_AUTO : !strcmp(v, "sw") ? YVID_HW_OFF : !strcmp(v, "vaapi") ? YVID_HW_VAAPI : !strcmp(v, "dxva") ? YVID_HW_DXVA : -1;
            if (hw < 0) { fprintf(stderr, "video_soundtrack: --hw auto, sw, vaapi or dxva\n"); return 2; }
        }
        else if (!strcmp(argv[i], "--audio") && i + 1 < argc) {
            const char* v = argv[++i];
            if (!strcmp(v, "auto")) backend = YAU_BACKEND_AUTO;
            else if (!strcmp(v, "alsa")) backend = YAU_BACKEND_ALSA;
            else if (!strcmp(v, "pulse")) backend = YAU_BACKEND_PULSE;
            else if (!strcmp(v, "null")) backend = YAU_BACKEND_NULL;
            else { fprintf(stderr, "video_soundtrack: --audio auto, alsa, pulse or null\n"); return 2; }
        }
        else {
            fprintf(stderr, "usage: video_soundtrack --file CLIP --wav WAV [--seconds S] [--script] [--fullscreen] [--window WxH]\n"
                            "                        [--audio auto|alsa|pulse|null] [--db G] [--rate HZ] [--elevate] [--hw auto|sw|vaapi|dxva] [--device PATH] [--threads N] [--csv PATH]\n");
            return 2;
        }
    }
    if (!file || !wav) { fprintf(stderr, "video_soundtrack: --file and --wav are required\n"); return 2; }
    if (elevate) {
        yrt_policy p = yrt_thread_elevate(NULL);
        printf("video_soundtrack: the frame thread's policy: %d\n", (int)p);
    }
    if (!yrt_ring_open(&vring, &(yrt_ring_desc){ .memory = vring_mem, .bytes = sizeof vring_mem }) ||
        !yrt_ring_open(&aring, &(yrt_ring_desc){ .memory = aring_mem, .bytes = sizeof aring_mem })) {
        fprintf(stderr, "video_soundtrack: the rings did not open\n");
        return 1;
    }
    if (!yscr_open(&scr, &(yscr_desc){ .windowed = !fullscreen, .window_w = ww, .window_h = wh })) {
        fprintf(stderr, "video_soundtrack: %s\n", yscr_error(&scr));
        return 1;
    }
    if (!ygfx_open(&gfx, &(ygfx_desc){ .screen = &scr, .background = { 0.45f, 0.45f, 0.45f } })) {
        fprintf(stderr, "video_soundtrack: %s\n", ygfx_error(&gfx));
        return 1;
    }
    if (!yau_open(&au, &(yau_desc){ .backend = backend, .ring = &aring, .format = { .rate = rate, .sample = (uint16_t)(rate ? YAU_S16 : 0) } })) {
        fprintf(stderr, "video_soundtrack: %s\n", yau_error(&au));
        return 1;
    }
    {
        int64_t t0 = yrt_real_ns();
        yvid_desc vd;
        memset(&vd, 0, sizeof vd);
        vd.path = file;
        vd.ring = &vring;
        vd.hw_decode = (yvid_hw)hw;
        vd.decode_threads = threads;
        vd.hw_device = device;
        if (!yvid_open(&mv, &gfx, &vd)) { fprintf(stderr, "video_soundtrack: %s\n", yvid_error(&mv)); return 1; }
        printf("video_soundtrack: yvid_open() took %.1f ms\n", (double)(yrt_real_ns() - t0) / 1e6);
    }
    if (yvid_soundtrack(&mv, &au, &(yvid_soundtrack_desc){ .path = wav, .db = (float)db, .ramp_ns = YRT_MS(5) }) < 0) {
        fprintf(stderr, "video_soundtrack: %s\n", yvid_error(&mv));
        return 1;
    }
    film = yvid_stim(&mv, &(yvid_stim_desc){ .opacity = 1.0f });
    yscr_describe(&scr, line, sizeof line);
    printf("%s\n", line);
    yau_describe(&au, line, sizeof line);
    printf("%s\n", line);
    if (csv_path) {
        csv = fopen(csv_path, "w");
        if (!csv) { fprintf(stderr, "video_soundtrack: cannot write %s\n", csv_path); return 1; }
        fprintf(csv, "display,frame,decision,why,onset_ns,residual_ns,flags,ahead\n");
    }
    if (yvid_play_at(&mv, YVID_ASAP) < 0) { fprintf(stderr, "video_soundtrack: %s\n", yvid_error(&mv)); return 1; }
    while (yscr_begin(&scr, &f) == YSCR_OK) {
        double el;
        if (!t_start) t_start = f.onset;
        el = (double)(f.onset - t_start) * 1e-9;
        yau_update(&au);
        if (script && step == 0 && el >= 20) { yvid_seek(&mv, mv.info.duration / 2, YVID_ASAP); step = 1; }
        if (script && step == 1 && el >= 36) { yvid_pause_at(&mv, YVID_ASAP); step = 2; }
        if (script && step == 2 && el >= 38) { yvid_play_at(&mv, YVID_ASAP); step = 3; }
        if (yvid_follow_audio(&mv, &au, &f) < 0) { fprintf(stderr, "video_soundtrack: %s\n", yvid_error(&mv)); failed = 1; break; }
        rc = yvid_update(&mv, &f);
        if (rc < 0) { fprintf(stderr, "video_soundtrack: %s\n", yvid_error(&mv)); failed = 1; break; }
        ygfx_begin(&gfx, &f);
        ygfx_draw(&gfx, &film);
        ygfx_end(&gfx);
        yscr_flip(&scr);
        drain();
        if (rc == YVID_ENDED) break;
        if (seconds > 0 && el >= seconds) break;
    }
    yscr_wait_flip(&scr, NULL);
    {
        yscr_frame g;
        if (!failed && yscr_begin(&scr, &g) == YSCR_OK) {
            int k;
            for (k = 0; k < g.n_done; k++) yvid_flip_done(&mv, &g.done[k]);
            ygfx_begin(&gfx, &g); ygfx_end(&gfx); yscr_flip(&scr);
        }
    }
    yvid_describe(&mv, line, sizeof line);
    printf("%s\n", line);
    yscr_describe(&scr, line, sizeof line);
    printf("%s\n", line);
    {
        yau_caps c;
        yau_get_caps(&au, &c);
        printf("video_soundtrack: device %+.1f ppm against the ysp_rt clock, fit spread %.1f us, device underruns %lld\n",
               c.drift_ppm, c.fit_spread_ns / 1e3, (long long)c.xruns);
    }
    yvid_close(&mv);
    yau_describe(&au, line, sizeof line);
    printf("%s\n", line);
    yau_close(&au);
    drain();
    if (n_res > 0) {
        qsort(res, (size_t)n_res, sizeof *res, cmp_i64);
        printf("video_soundtrack: flip onset minus the frame's time on the audio clock, %lld frames: p1 %+.3f, p50 %+.3f, p99 %+.3f, min %+.3f, max %+.3f ms\n",
               (long long)n_res, (double)res[n_res / 100] / 1e6, (double)res[n_res / 2] / 1e6, (double)res[n_res * 99 / 100] / 1e6,
               (double)res[0] / 1e6, (double)res[n_res - 1] / 1e6);
    }
    printf("video_soundtrack: shown %lld (late %lld, estimated %lld), repeated %lld (due %lld, decode late %lld, drift %lld), dropped: display late %lld, decode late %lld, drift %lld, cadence %lld\n",
           (long long)n_shown, (long long)n_late, (long long)n_est, (long long)n_repeated, (long long)reps_by[YVID_WHY_DUE],
           (long long)reps_by[YVID_WHY_DECODE_LATE], (long long)reps_by[YVID_WHY_DRIFT], (long long)drops_by[YVID_WHY_DISPLAY_LATE],
           (long long)drops_by[YVID_WHY_DECODE_LATE], (long long)drops_by[YVID_WHY_DRIFT], (long long)drops_by[YVID_WHY_CADENCE]);
    printf("video_soundtrack: sound starts %lld, stops %lld, onsets from their targets %.1f to %.1f us, gaps %lld (%lld frames), underrun records %lld, ends %lld\n",
           (long long)n_sound_start, (long long)n_sound_stop, onset_lo / 1e3, onset_hi / 1e3, (long long)n_gaps, (long long)gap_frames, (long long)n_xruns, (long long)n_ends);
    if (csv) fclose(csv);
    free(res);
    ygfx_close(&gfx);
    yscr_close(&scr);
    return failed ? 1 : 0;
}
