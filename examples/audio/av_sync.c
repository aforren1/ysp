/* audio_av_sync.c - a flash and a click planned for the same time: what
 * the records say about sound minus light, and, with a line board, what
 * the light and the sound do.
 *
 * Mirrors Psychtoolbox-3's PsychPortAudioTimingTest.m (sound onset against
 * a flip, checked with a photodiode and a microphone on an oscilloscope)
 * and AudioFeedbackLatencyTest.m without capture. Read for coverage only;
 * no code is copied.
 *
 *     audio_av_sync [--sim] [--n N] [--lead S] [--db DB] [--patch PX]
 *                   [--board KEY [--sound-board KEY] [--light-ch C] [--sound-ch C]]
 *                   [--csv FILE] [--fullscreen]
 *
 * Each trial plans a flash (the screen's corner patch, black to white for
 * 2 frames; WARNING: about 1.5 flashes a second) on frame j, K frames ahead
 * (K = --lead, default 0.1 s, in frames), and a 1 ms click with
 * yau_play_at() at frame j's predicted onset. The flip record of frame j
 * gives the light's onset as ysp/screen.h estimates it; the click's record
 * gives the sound's as ysp/audio.h's device-clock fit puts it. Both are
 * software numbers: "audio - flip" is what the two headers believe.
 *
 * HARDWARE MODE (--board; written, NEVER RUN: no board was attached). A
 * board with firmware/ysp_line/ (docs/device.md) stamps the photodiode on
 * the patch and the sound on its own microsecond clock; ysp/device.h maps
 * the stamps to the ysp_rt clock and sends them over ysp/screen.h's input
 * bridge. Then "sound - light" is the true offset, and "light - flip" and
 * "sound - audio" are the two headers' onset offsets (desc.onset_offset_ns
 * of each). The sound reaches a digital input through a comparator (a
 * sound-sensor module with a digital output on the line out, or the
 * board's analog mode with the signal biased to mid-scale). Two ways:
 *   one board, two inputs   --board KEY, light on channel --light-ch (1),
 *                           sound on --sound-ch (2). firmware/ysp_line/
 *                           has ONE input today: this needs a second
 *                           input pin in the firmware (not done here).
 *   two boards              --board KEY for the light, --sound-board KEY
 *                           for the sound (each its own YSP_CHANNEL; both
 *                           1 by default). Each clock is fitted on its
 *                           own, so the offset carries both fits' errors
 *                           (the unc_us of each event, printed).
 * --db may then go up to -6 dBFS, for the comparator; without a board it
 * stays at -30 or lower.
 *
 * --sim: the simulated display and the null device on ysp/rt.h's virtual
 * clock, and synthetic board edges (the light 5 ms after each flip onset,
 * the sound 12 ms after each click's onset, with a ringing edge 0.3 ms
 * after it) fed to the same matcher. It checks its own results and exits
 * 1 on a mismatch. Under a second: the virtual clock does not wait.
 *
 * CSV (--csv): one row per trial; planned in seconds on the ysp_rt clock,
 * every other time in seconds after planned.
 * Exit: 0, 1 when something did not open, a board was not running after
 * the warm-up, a trial had no edge or a --sim check failed, 2 usage.
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#define YSP_AUDIO_IMPLEMENTATION
#include "ysp/audio.h"
#define YSP_DEVICE_IMPLEMENTATION
#include "ysp/device.h"
#define YSP_SCREEN_IMPLEMENTATION
#include "ysp/screen.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_TRIALS   200
#define MAX_EDGES    4096
#define FLASH_FRAMES 2
#define SIM_LIGHT_NS 5000000
#define SIM_SOUND_NS 12000000

typedef struct trial {
    int64_t   frame, plan;
    yau_id    id;
    yau_onset a;
    int       a_rc, have_flip, fed;
    yscr_record flip;
    int64_t   light, sound;      /* 0 = no edge */
    uint16_t  light_unc, sound_unc;
} trial;
typedef struct edge { int64_t t; uint32_t device, control; uint16_t unc; } edge;

static yscr_screen scr;
static yau_audio au;
static ydev_device dev_light, dev_sound;
static trial trials[MAX_TRIALS];
static edge edges[MAX_EDGES];
static int n_edges;

static void to_bridge(void* ctx, const yin_event* e) { (void)ctx; (void)yscr_push_input(e); }

/* Every rising edge of a board, from the bridge or from --sim. */
static void on_input(const yin_event* e) {
    if (e->type != YIN_PRESS || (e->kind != YIN_KIND_SYNC && e->kind != YIN_KIND_BOX) || n_edges >= MAX_EDGES) return;
    edges[n_edges].t = e->t;
    edges[n_edges].device = e->device;
    edges[n_edges].control = e->control;
    edges[n_edges].unc = e->unc_us;
    n_edges++;
}

/* The first edge of (device, control) in [from, to), and its uncertainty. */
static int64_t first_edge(uint32_t device, uint32_t control, int64_t from, int64_t to, uint16_t* unc) {
    int64_t best = 0;
    int i;
    for (i = 0; i < n_edges; i++)
        if (edges[i].device == device && edges[i].control == control && edges[i].t >= from && edges[i].t < to &&
            (!best || edges[i].t < best)) { best = edges[i].t; *unc = edges[i].unc; }
    return best;
}

static int cmp_d(const void* a, const void* b) {
    double x = *(const double*)a, y = *(const double*)b;
    return x < y ? -1 : (x > y);
}

/* mean, sd, min, median, max of v[0..n) in ms; sorts v */
static void stats(const char* what, double* v, int n) {
    double m = 0, s = 0;
    int i;
    if (n <= 0) { printf("%-34s no values\n", what); return; }
    for (i = 0; i < n; i++) m += v[i];
    m /= n;
    for (i = 0; i < n; i++) s += (v[i] - m) * (v[i] - m);
    qsort(v, (size_t)n, sizeof v[0], cmp_d);
    printf("%-34s n %3d  mean %+8.3f  sd %6.3f  min %+8.3f  median %+8.3f  max %+8.3f ms\n", what, n, m,
           sqrt(s / (n > 1 ? n - 1 : 1)), v[0], v[n / 2], v[n - 1]);
}

static int start_board(ydev_device* dev, const char* role, const char* key, uint32_t device) {
    ydev_desc dd;
    memset(&dd, 0, sizeof dd);
    dd.role = role;
    dd.family = YBOX_LINE;
    dd.key = key;
    dd.device = device;
    dd.kind = YIN_KIND_SYNC;
    dd.sink = to_bridge;
    if (ydev_open(dev, &dd)) return 1;
    fprintf(stderr, "audio_av_sync: %s board: %s\n", role, ydev_error(dev));
    return 0;
}

int main(int argc, char** argv) {
    yscr_desc sd;
    yau_desc ad;
    yau_click_desc cd;
    yau_buf click;
    yscr_frame f;
    yscr_caps caps;
    FILE* csv = NULL;
    char line[1024];
    const char *board = NULL, *sound_board = NULL;
    double lead_s = 0.1, db = -40;
    static double v_sw[MAX_TRIALS], v_light[MAX_TRIALS], v_sound[MAX_TRIALS], v_true[MAX_TRIALS];
    uint32_t rng = 2463534242u, light_dev = 1, sound_dev = 1, light_ch = 1, sound_ch = 0;
    int i, n = 0, sim = 0, fullscreen = 0, patch = 160, failed = 0, aborted = 0, planned = 0, K = 0;
    int n_sw = 0, n_hw = 0, n_light = 0, n_sound = 0, bad = 0, lit = 0;
    int64_t next_plan = 0, end_frame = -1, warm_end, period = 0;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--sim")) sim = 1;
        else if (!strcmp(argv[i], "--n") && i + 1 < argc) n = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--lead") && i + 1 < argc) lead_s = atof(argv[++i]);
        else if (!strcmp(argv[i], "--db") && i + 1 < argc) db = atof(argv[++i]);
        else if (!strcmp(argv[i], "--patch") && i + 1 < argc) patch = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--board") && i + 1 < argc) board = argv[++i];
        else if (!strcmp(argv[i], "--sound-board") && i + 1 < argc) sound_board = argv[++i];
        else if (!strcmp(argv[i], "--light-ch") && i + 1 < argc) light_ch = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--sound-ch") && i + 1 < argc) sound_ch = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--fullscreen")) fullscreen = 1;
        else if (!strcmp(argv[i], "--csv") && i + 1 < argc) {
            if (!(csv = fopen(argv[++i], "w"))) { fprintf(stderr, "audio_av_sync: cannot write %s\n", argv[i]); return 2; }
        } else {
            fprintf(stderr, "usage: audio_av_sync [--sim] [--n N] [--lead S] [--db DB] [--patch PX] [--board KEY "
                            "[--sound-board KEY] [--light-ch C] [--sound-ch C]] [--csv FILE] [--fullscreen]\n");
            return 2;
        }
    }
    /* --sim runs on ysp/rt.h's virtual clock: the same records on any host */
    if (sim) yrt_virtual_begin(0);
    if (n == 0) n = sim ? 12 : 30;
    if (sound_board) sound_dev = 2;
    if (!sound_ch) sound_ch = sound_board ? 1 : 2;
    if (n < 1 || n > MAX_TRIALS || lead_s < 0.02 || lead_s > 1 || patch < 16 || db > (board ? -6 : -30) ||
        (sim && board) || (sound_board && !board) || light_ch < 1 || light_ch > 255 || sound_ch > 255 ||
        (!sound_board && sound_ch == light_ch)) {
        fprintf(stderr, "audio_av_sync: --n 1..%d, --lead 0.02..1 s, --patch 16 px or more, --db at most -30 "
                        "(-6 with --board), no --board with --sim, two different channels on one board\n", MAX_TRIALS);
        return 2;
    }

    memset(&sd, 0, sizeof sd);
    if (sim) sd.backend = YSCR_BACKEND_SIM;
    sd.windowed = !fullscreen;
    sd.patch.on = true;
    sd.patch.size = patch;
    sd.patch.corner = YSCR_TOP_LEFT;
    /* the screen first: the bridge exists once a screen with SDL is open */
    if (!yscr_open(&scr, &sd)) { fprintf(stderr, "audio_av_sync: %s\n", yscr_error(&scr)); return 1; }
    yscr_get_caps(&scr, &caps);
    memset(&ad, 0, sizeof ad);
    if (sim) ad.backend = YAU_BACKEND_NULL;
    ad.arena_bytes = 1 << 20;
    if (!yau_open(&au, &ad)) { fprintf(stderr, "audio_av_sync: %s\n", yau_error(&au)); yscr_close(&scr); return 1; }
    memset(&cd, 0, sizeof cd);
    cd.dur_ns = YRT_MS(1);
    cd.peak = yau_db((float)db);
    click = yau_click(&au, &cd);
    if (!click.frames) { fprintf(stderr, "audio_av_sync: %s\n", yau_error(&au)); failed = 1; goto done; }
    if (board && (!start_board(&dev_light, "light", board, light_dev) ||
                  (sound_board && !start_board(&dev_sound, "sound", sound_board, sound_dev)))) { failed = 1; goto done; }
    yscr_describe(&scr, line, sizeof line);
    printf("%s\n", line);
    if (csv) fprintf(csv, "# %s\n", line);
    yau_describe(&au, line, sizeof line);
    printf("%s\n", line);
    if (csv) {
        fprintf(csv, "# %s\n# board: %s%s%s; light device %u channel %u, sound device %u channel %u\n", line,
                board ? board : (sim ? "synthetic (--sim)" : "none"), sound_board ? ", sound board " : "",
                sound_board ? sound_board : "", (unsigned)light_dev, (unsigned)light_ch, (unsigned)sound_dev,
                (unsigned)sound_ch);
        fprintf(csv, "trial,frame,planned,flip_onset,flip_tier,flip_dropped,audio_onset,audio_tier,audio_flags,"
                     "audio_minus_flip,light,sound,sound_minus_light\n");
    }
    (void)yrt_thread_elevate(NULL);

    /* black while the boards open and their fits get a slope */
    warm_end = yrt_now_ns() + (board ? YRT_S(3) : YRT_S(1));
    for (;;) {
        SDL_Event ev;
        yin_event in;
        int rc = yscr_begin(&scr, &f);
        if (rc == YSCR_QUIT) { aborted = 1; break; }
        if (rc != YSCR_OK) { fprintf(stderr, "audio_av_sync: %s\n", yscr_error(&scr)); failed = 1; break; }
        if (!K) {
            period = f.period > 0 ? f.period : YRT_S(1.0 / 60);
            K = (int)((YRT_S(lead_s) + period - 1) / period);
        }

        /* the flip records of the flash frames */
        for (i = 0; i < f.n_done; i++) {
            int k;
            for (k = 0; k < planned; k++)
                if (trials[k].frame == f.done[i].index) { trials[k].flip = f.done[i]; trials[k].have_flip = 1; }
        }

        if (board && !planned && f.onset >= warm_end &&
            (ydev_state(&dev_light) != YDEV_RUNNING || (sound_board && ydev_state(&dev_sound) != YDEV_RUNNING))) {
            fprintf(stderr, "audio_av_sync: after the warm-up the light board is %s%s%s\n",
                    ydev_state_name(ydev_state(&dev_light)), sound_board ? " and the sound board " : "",
                    sound_board ? ydev_state_name(ydev_state(&dev_sound)) : "");
            failed = 1;
            break;
        }
        /* the next trial: a flash K frames ahead, the click at its predicted onset */
        if (f.onset >= warm_end && planned < n && f.index >= next_plan) {
            trial* t = &trials[planned++];
            memset(t, 0, sizeof *t);
            t->frame = f.index + K;
            t->plan = f.onset + (int64_t)K * period;
            t->id = yau_play_at(&au, click, t->plan);
            t->a_rc = t->id > 0 ? YAU_PENDING : (int)t->id;
            rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
            next_plan = t->frame + FLASH_FRAMES + 30 + (int64_t)(rng % 15);   /* 0.53 to 0.78 s apart at 60 Hz */
            if (planned == n) end_frame = t->frame + FLASH_FRAMES + YRT_MS(400) / period;
        }
        for (i = 0, lit = 0; i < planned; i++)
            if (f.index >= trials[i].frame && f.index < trials[i].frame + FLASH_FRAMES) lit = 1;
        yscr_set_patch(&scr, lit ? 1.0f : 0.0f);
        yscr_flip(&scr);

        while (yscr_poll(&scr, &ev, NULL))
            if (yscr_event_input(&scr, &ev, &in)) on_input(&in);
        for (i = 0; i < planned; i++) {
            trial* t = &trials[i];
            if (t->a_rc == YAU_PENDING) t->a_rc = yau_result(&au, t->id, &t->a);
            if (sim && t->a_rc == YAU_OK && t->have_flip && !t->fed) {
                /* the board's edges, made up: the light lags its flip, the
                 * sound its click, and the click rings once */
                yin_event e;
                memset(&e, 0, sizeof e);
                e.kind = YIN_KIND_SYNC;
                e.type = YIN_PRESS;
                e.device = light_dev;
                e.control = light_ch;
                e.t = t->flip.onset + SIM_LIGHT_NS;
                on_input(&e);
                e.device = sound_dev;
                e.control = sound_ch;
                e.t = t->a.onset + SIM_SOUND_NS;
                on_input(&e);
                e.t += 300000;
                on_input(&e);
                t->fed = 1;
            }
        }
        if (end_frame >= 0 && f.index >= end_frame) break;
    }

    /* match and report */
    printf("trial  frame   flip - plan  audio - plan  audio - flip   light - flip  sound - audio  sound - light  (ms)\n");
    for (i = 0; i < planned; i++) {
        trial* t = &trials[i];
        char hw[96] = "";
        if (t->a_rc == YAU_PENDING) t->a_rc = yau_wait(&au, t->id, YRT_S(1), &t->a);
        t->light = t->sound = 0;
        if (t->have_flip && t->flip.onset) {
            int64_t from = t->plan - YRT_MS(20), to = t->plan + YRT_MS(150);
            t->light = first_edge(light_dev, light_ch, from, to, &t->light_unc);
            t->sound = first_edge(sound_dev, sound_ch, from, to, &t->sound_unc);
        }
        if (t->a_rc != YAU_OK || !t->have_flip || !t->flip.onset) {
            printf("%5d %6lld   record missing: audio %s, flip %s\n", i, (long long)t->frame, yau_strerror(t->a_rc),
                   t->have_flip ? "not shown" : "none");
            bad = 1;
            continue;
        }
        v_sw[n_sw++] = (double)(t->a.onset - t->flip.onset) / 1e6;
        if (t->light) v_light[n_light++] = (double)(t->light - t->flip.onset) / 1e6;
        if (t->sound) v_sound[n_sound++] = (double)(t->sound - t->a.onset) / 1e6;
        if (t->light && t->sound) {
            v_true[n_hw++] = (double)(t->sound - t->light) / 1e6;
            snprintf(hw, sizeof hw, "%+12.3f  %+12.3f  %+12.3f", (double)(t->light - t->flip.onset) / 1e6,
                     (double)(t->sound - t->a.onset) / 1e6, (double)(t->sound - t->light) / 1e6);
        } else if (board || sim) snprintf(hw, sizeof hw, "  edge missing: light %s, sound %s", t->light ? "ok" : "none",
                                         t->sound ? "ok" : "none");
        printf("%5d %6lld  %+12.3f  %+12.3f  %+12.3f  %s  tiers %u/%u  flags 0x%03x\n", i, (long long)t->frame,
               (double)(t->flip.onset - t->plan) / 1e6, (double)(t->a.onset - t->plan) / 1e6,
               (double)(t->a.onset - t->flip.onset) / 1e6, hw, (unsigned)t->flip.tier, (unsigned)t->a.tier,
               (unsigned)t->a.flags);
        if (csv) {
            fprintf(csv, "%d,%lld,%.9f,%.9f,%u,%u,%.9f,%u,%u,%.9f,", i, (long long)t->frame, (double)t->plan / 1e9,
                    (double)(t->flip.onset - t->plan) / 1e9, (unsigned)t->flip.tier, (unsigned)t->flip.dropped,
                    (double)(t->a.onset - t->plan) / 1e9, (unsigned)t->a.tier, (unsigned)t->a.flags,
                    (double)(t->a.onset - t->flip.onset) / 1e9);
            if (t->light) fprintf(csv, "%.9f", (double)(t->light - t->plan) / 1e9);
            fputc(',', csv);
            if (t->sound) fprintf(csv, "%.9f", (double)(t->sound - t->plan) / 1e9);
            fputc(',', csv);
            if (t->light && t->sound) fprintf(csv, "%.9f", (double)(t->sound - t->light) / 1e9);
            fputc('\n', csv);
        }
        if (sim) {
            /* On the virtual clock the null device's blocks keep their
             * times: the sound is on the flash within half a sample (the
             * default 48 kHz), where the plan meets the sample grid. */
            int ok = t->flip.tier == YSCR_TIER_SIM && t->a.tier == YAU_TIER_SIM &&
                     !(t->a.flags & (YAU_ONSET_LATE | YAU_ONSET_XRUN)) &&
                     llabs(t->a.onset - t->flip.onset) <= 1000000000 / 48000 / 2 &&
                     t->light == t->flip.onset + SIM_LIGHT_NS && t->sound == t->a.onset + SIM_SOUND_NS;
            if (!ok) { fprintf(stderr, "audio_av_sync: --sim: trial %d is off\n", i); bad = 1; }
        }
    }
    if (planned < n && !aborted) bad = 1;
    stats("audio - flip (the records)", v_sw, n_sw);
    if (board || sim) {
        stats("light - flip (screen offset)", v_light, n_light);
        stats("sound - audio (audio offset)", v_sound, n_sound);
        stats("sound - light (the true offset)", v_true, n_hw);
        printf("edges: %d in all; trials without a light edge %d, without a sound edge %d\n", n_edges, n_sw - n_light,
               n_sw - n_sound);
        if (board) printf("the means of light - flip and sound - audio are desc.onset_offset_ns of ysp/screen.h and "
                          "ysp/audio.h on this rig, if the boards' fits held\n");
        if (board && n_hw < n_sw) bad = 1;   /* an edge missing: check the wiring and the levels */
    }
    if (sim && !bad && n_hw == n_sw && n_sw == n) printf("--sim: every trial came out as planned\n");
    else if (sim) bad = 1;

done:
    if (board) { ydev_close(&dev_light); if (sound_board) ydev_close(&dev_sound); }
    if (csv) fclose(csv);
    yau_describe(&au, line, sizeof line);
    printf("%s\n", line);
    yau_close(&au);
    yscr_close(&scr);
    if (aborted) printf("stopped with Shift+Esc after %d trials\n", planned);
    return (failed || bad) ? 1 : 0;
}
