/* audio_voice_rt.c - record a spoken response after a beep: the voice
 * onset time on the ysp_rt clock, and each trial's sound in a WAV file.
 * The pattern of PsychoPy's microphone component and PTB's
 * SimpleVoiceTriggerDemo, on the rig clock.
 *
 *     audio_voice_rt [--sim] [--device OUT] [--in IN] [--trials N]
 *                    [--on-db DB] [--channel C] [--save PREFIX]
 *
 *   --sim          no hardware: the simulated loop with a participant who
 *                  says a word 400 to 600 ms after each beep; exit 1 unless
 *                  each RT is the word's time plus the loop's 10 ms within
 *                  8 ms (CI runs this)
 *   --device OUT, --in IN   the output and input whose names contain these
 *   --trials N     trials (default 5)
 *   --on-db DB     the voice key's start level, dBFS RMS (default -40)
 *   --channel C    the input channel (default 0)
 *   --save PREFIX  PREFIX_N.wav per trial, float, all input channels, from
 *                  100 ms before the beep to the end of the response
 *
 * A trial: a 1 kHz beep of 100 ms, 300 ms after the trial starts, by
 * yau_play_at(); its onset is the output fit's time (the record). The frame
 * thread is reader 0: each 10 ms it reads the new input, keeps it in the
 * trial's buffer and feeds the voice key from the beep's frame on. The RT
 * is yau_capture_time_at(the voice key's onset frame) minus the beep's
 * onset; the trial ends 300 ms after the voice key's offset, or 2.5 s after
 * the beep. Then, between trials, the WAV file is written. The RT holds the
 * loop's own latency (output and input pipelines), which
 * audio_loopback_latency measures: subtract its median, or set the input's
 * desc.offset_ns to minus it.
 *
 * Exit: 0 when every trial has a response (and --sim met its check), 1
 * when a device did not open, a trial had no response or --sim failed,
 * 2 usage.
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
static yau_vkey g_vk;
static yau_wavw g_wav;
#define TRIAL_FRAMES (48000 * 4)
static float g_trial[TRIAL_FRAMES * 8];

int main(int argc, char** argv) {
    yau_desc d;
    yau_capture_desc cd;
    yau_tone_desc td;
    yau_vkey_desc vd;
    yau_buf beep;
    char line[640];
    int i, trials = 5, sim = 0, channel = 0, rc = 0;
    double on_db = -40;
    const char *out = NULL, *in = NULL, *save = NULL;
    memset(&d, 0, sizeof d);
    memset(&cd, 0, sizeof cd);
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--sim")) sim = 1;
        else if (!strcmp(argv[i], "--device") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "--in") && i + 1 < argc) in = argv[++i];
        else if (!strcmp(argv[i], "--trials") && i + 1 < argc) trials = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--on-db") && i + 1 < argc) on_db = atof(argv[++i]);
        else if (!strcmp(argv[i], "--channel") && i + 1 < argc) channel = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--save") && i + 1 < argc) save = argv[++i];
        else { fprintf(stderr, "audio_voice_rt: unknown argument %s (see the top of the file)\n", argv[i]); return 2; }
    }
    /* --sim runs on ysp/rt.h's virtual clock: the same records on any host */
    if (sim) yrt_virtual_begin(0);
    if (trials < 1 || trials > 64 || on_db >= 0 || channel < 0) { fprintf(stderr, "audio_voice_rt: --trials 1..64, --on-db < 0\n"); return 2; }
    d.device = out;
    d.arena_bytes = 1 << 20;
    cd.device = in;
    cd.format.sample = YAU_S16;   /* any input of 16 bits or more */
    if (sim) {
        if (lp_sim_init(&g_lp, 20, 0, 0) < 0) return 1;
        for (i = 0; i < trials; i++) g_lp.rt_ms[i] = 400.0 + (double)((i * 37) % 200);
        g_lp.nrt = trials;
        d.backend = YAU_BACKEND_CUSTOM; d.dev = &g_lp_out; d.dev_ctx = &g_lp;
        cd.backend = YAU_BACKEND_CUSTOM; cd.dev = &g_lp_in; cd.dev_ctx = &g_lp;
    }
    if (!yau_capture_open(&g_cap, &cd)) { fprintf(stderr, "%s\n", yau_capture_error(&g_cap)); return 1; }
    if (!yau_open(&g_au, &d)) { fprintf(stderr, "%s\n", yau_error(&g_au)); yau_capture_close(&g_cap); return 1; }
    if (channel >= g_cap.dcaps.channels || g_cap.dcaps.channels > 8) { fprintf(stderr, "--channel: the input has %u\n", (unsigned)g_cap.dcaps.channels); rc = 2; goto done; }
    {
        int64_t until = yrt_now_ns() + YRT_S(3);
        yau_capture_info ci;
        do {
            (void)yau_update(&g_au);
            (void)yau_capture_read(&g_cap, 0, g_trial, TRIAL_FRAMES, NULL);   /* the warm-up: dropped */
            yau_capture_get_info(&g_cap, &ci);
            if (ci.fit_ready && g_au.fit_ft.ready) break;
            yrt_sleep_until(yrt_now_ns() + 10000000, 0);
        } while (yrt_now_ns() < until);
        if (!ci.fit_ready || !g_au.fit_ft.ready) { fprintf(stderr, "the fits did not warm up in 3 s\n"); rc = 1; goto done; }
    }
    yau_describe(&g_au, line, sizeof line);
    puts(line);
    yau_capture_describe(&g_cap, line, sizeof line);
    puts(line);
    memset(&td, 0, sizeof td);
    td.hz = 1000; td.dur_ns = YRT_MS(100); td.peak = yau_db(-20); td.ramp_ns = YRT_MS(5);
    beep = yau_tone(&g_au, &td);
    memset(&vd, 0, sizeof vd);
    vd.on_db = (float)on_db;
    vd.channel = (uint16_t)channel;
    if (!beep.frames || yau_vkey_init(&g_vk, &vd, g_cap.dcaps.rate) < 0) { fprintf(stderr, "beep or voice key refused\n"); rc = 1; goto done; }
    for (i = 0; i < trials; i++) {
        int C = g_cap.dcaps.channels, have_on = 0, have_off = 0;
        int64_t t_beep = yrt_now_ns() + YRT_MS(300), beep_frame, listen, first, n = 0, end_t = 0, rt = -1;
        yau_onset r;
        yau_id id = yau_play_at(&g_au, beep, t_beep);
        double on_frame = 0;
        if (id < 0) { fprintf(stderr, "%s\n", yau_error(&g_au)); rc = 1; break; }
        memset(&r, 0, sizeof r);
        /* the input from 100 ms before the beep on goes in the trial's
         * buffer; the voice key listens from 200 ms after it, because on
         * speakers the beep itself comes back through the microphone */
        (void)yau_capture_read(&g_cap, 0, g_trial, TRIAL_FRAMES, NULL);
        beep_frame = yau_capture_frame_at(&g_cap, t_beep);
        first = beep_frame - (int64_t)g_cap.dcaps.rate / 10;
        listen = beep_frame + (int64_t)g_cap.dcaps.rate / 5;
        for (;;) {
            int64_t got, f, k;
            yau_vkey_event ev[4];
            yrt_sleep_until(yrt_now_ns() + 10000000, 0);
            (void)yau_update(&g_au);
            while (n < TRIAL_FRAMES && (got = yau_capture_read(&g_cap, 0, g_trial + n * C, TRIAL_FRAMES - n, &f)) > 0) {
                if (f + got <= first) continue;   /* before the trial's window */
                if (n == 0 && f < first) {                      /* trim to the window */
                    memmove(g_trial, g_trial + (first - f) * C, (size_t)((got - (first - f)) * C) * sizeof(float));
                    got -= first - f;
                    f = first;
                }
                k = f >= listen ? 0 : listen - f;
                if (k < got) {
                    int m = yau_vkey_feed(&g_vk, g_trial + (n + k) * C, got - k, (uint16_t)C, f + k, ev, 4), j;
                    for (j = 0; j < m; j++) {
                        if (ev[j].kind == YAU_VKEY_ON && !have_on) { have_on = 1; on_frame = ev[j].frame; }
                        else if (ev[j].kind == YAU_VKEY_OFF && have_on) { have_off = 1; end_t = yrt_now_ns() + YRT_MS(300); }
                    }
                }
                n += got;
            }
            if (have_off && yrt_now_ns() >= end_t) break;
            if (yrt_now_ns() > t_beep + YRT_MS(2500)) break;
        }
        if (yau_wait(&g_au, id, YRT_S(1), &r) != YAU_OK) { fprintf(stderr, "no record for the beep\n"); rc = 1; break; }
        if (have_on) rt = yau_capture_time_at(&g_cap, on_frame) - r.onset;
        printf("trial %d: beep tier %d; RT %s%.1f ms%s", i + 1, r.tier, rt < 0 ? "none " : "",
               rt < 0 ? 0.0 : (double)rt / 1e6, have_off ? "" : " (no offset)");
        if (sim && i < g_lp.nrt) {
            double want = g_lp.rt_ms[i] + (g_lp.delay - 480) / 48.0;
            printf("; the participant spoke %.0f ms after the beep, the loop adds %.0f", g_lp.rt_ms[i], want - g_lp.rt_ms[i]);
            if (rt < 0 || fabs((double)rt / 1e6 - want) > 8.0) { printf(": FAIL"); rc = 1; }
        }
        printf("\n");
        if (rt < 0) rc = 1;
        if (save && n > 0) {
            char path[512];
            yau_wavw_desc wd;
            memset(&wd, 0, sizeof wd);
            snprintf(path, sizeof path, "%s_%d.wav", save, i + 1);
            wd.path = path; wd.rate = g_cap.dcaps.rate; wd.channels = (uint16_t)C;
            if (!yau_wavw_open(&g_wav, &wd) || yau_wavw_write(&g_wav, g_trial, n) != n) fprintf(stderr, "%s\n", yau_wavw_error(&g_wav));
            else printf("  %s: %lld frames from capture frame %lld (beep at frame %lld)\n", path, (long long)n,
                        (long long)first, (long long)beep_frame);
            yau_wavw_close(&g_wav);
        }
        (void)yau_vkey_init(&g_vk, &vd, g_cap.dcaps.rate);
        yrt_sleep_until(yrt_now_ns() + YRT_MS(300), 0);
    }
    {
        yau_capture_info ci;
        yau_capture_get_info(&g_cap, &ci);
        printf("input: dropped %lld frames, lost %lld, xruns %u, tier %d\n", (long long)ci.dropped, (long long)ci.lost,
               (unsigned)ci.xruns, (int)ci.tier);
    }
done:
    yau_close(&g_au);
    yau_capture_close(&g_cap);
    return rc;
}
