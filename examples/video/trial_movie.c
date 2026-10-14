/* trial_movie.c - a reaction time to an event inside a movie: ysp/video.h
 * plays the movie on a ysp/timeline.h base with its soundtrack as a
 * ysp/audio.h stream, ysp/response.h takes the RT from the flip record of
 * the display frame that first showed the event's video frame, and
 * ysp/trials.h writes one row per trial.
 *
 * Mirrors PsychoPy's Builder demo Feature Demos/movies/ (play, pause and
 * seek, by keys here in place of the button and the slider), the Coder
 * demo stimuli/MovieStim.py (a movie with sound), and jsPsych's
 * video-keyboard-response. Written from their descriptions in
 * docs/rig_spec.md 15; no PsychoPy code (GPL-3) was copied.
 *
 * The movie is made by the program: 5 s at 30 fps, 160 x 90, shown 4x. A
 * dim gray field with a bar that crosses it once and the frame index as 8
 * blocks at the top left; from frame 60 (2 s) on, a bright disk in the
 * center. The soundtrack is silence with a 0.1 s, 1 kHz tone at -30 dBFS
 * from 2 s: the disk's sound. Each trial seeks to its start frame (0, 15
 * or 30, from the conditions: a foreperiod of 2, 1.5 or 1 s) and plays.
 * Press Space when the disk appears. The response window opens at the
 * disk's onset and lasts 1.5 s; presses before it are EARLY, presses
 * within 0.1 s of it anticipations. P pauses and resumes the movie; Left
 * and Right seek 1 s back and ahead. The trial ends at the response, the
 * deadline or the movie's end; the movie pauses, and a 0.8 s blank
 * follows.
 *
 * THE ONSET. The disk's onset is an ONSET event at t(60) on the movie's
 * timeline base: ysp/video.h fires it on the display frame that first
 * shows frame 60 (its THE DUE FRAME), and that frame's flip record is the
 * final onset. The event goes on the base when the trial's seek lands: put
 * on before, while the base still stands at the last trial's end, it is
 * already due and fires at once. A seek past frame 60 skips the event
 * (ytl_skip()); then the onset is the first display frame whose video
 * frame is 60 or later, from yvid_last(). onset_video_frame names the
 * frame; onset_via says which path (timeline or record). P and the arrows
 * never reach the response collector, so a control is not an EARLY press.
 *
 * The data file has one row per trial (ysp/trials.h's data writer): the
 * session's columns (index, ..., start_frame), then trial_index,
 * target_frame, onset_video_frame, onset_via, n_pauses, n_seeks,
 * frames_shown, frames_repeated, frames_dropped (yvid_info's counts over
 * the trial: display frames that showed a new video frame, that showed
 * one again, and video frames never shown), then the response's
 * (YRSP_DATA_COLS). Times in seconds; the screen, the sound device and
 * the movie go to FILE.meta.
 *
 * --sim: the simulated display, miniaudio's null device and a synthetic
 * participant over 6 trials: a press 0.30 to 0.45 s after the onset, a
 * pause of 0.5 s before the onset in trial 1, two seeks ahead in trial 2
 * (past frame 60 from any start) and one back in trial 4, no press in
 * trial 3. It checks every row (RT and key as scripted from the flip
 * record, no EARLY press, the pause and seek counts, the onset's video
 * frame and path, every video frame the trial passed shown or dropped) and
 * exits 1 on a mismatch. A dropped frame is counted, not failed: on a
 * loaded host the decode thread can be late (1 of 6 trials once, 6 frames).
 * 19 s on the Iris Xe laptop (MSVC).
 *
 * Usage: trial_movie [--sim] [--auto] [--fullscreen] [--no-sound] [--reps N] [--seed N] [--out FILE]
 *   --auto       the --sim participant and checks on the real display and
 *                sound device (a hand test that needs no hands)
 *   --reps N     repetitions of each start frame (default 3; --sim, --auto: 2)
 *   --no-sound   no sound device: the movie runs on its own clock
 * Exit code: 0 (also after Shift+Esc), 1 when something did not open or a
 * --sim check failed, 2 for a bad argument.
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#define YSP_AUDIO_IMPLEMENTATION
#include "ysp/audio.h"
#define YSP_VIDEO_IMPLEMENTATION
#include "ysp/video.h"
#define YSP_TRIALS_IMPLEMENTATION
#include "ysp/trials.h"
#define YSP_RESPONSE_IMPLEMENTATION
#include "ysp/response.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { DISK_ON, N_CHANNELS };
enum { MOVIE = 1 };

#define MOVIE_W     160
#define MOVIE_H     90
#define FPS         30
#define N_FRAMES    150
#define TARGET      60          /* the disk's first frame */
#define SCALE       4
#define RESPONSE_S  1.5
#define MIN_RT_S    0.1
#define ITI_S       0.8
#define SEEK_S      1.0
#define TONE_S      0.1
#define MAX_ROWS    256
#define SIM_TRIALS  6
#define MOVIE_PATH  "trial_movie.yspseq"

static const char conditions_csv[] = "start_frame\n0\n15\n30\n";
static const yrsp_choice keys[] = { { .key = "space" } };

typedef struct row {
    int trial_index, target_frame, onset_video_frame, n_pauses, n_seeks, frames_shown, frames_repeated, frames_dropped;
    const char* onset_via;
} row;
static const ytr_col cols[] = {
    YTR_COL(row, trial_index, YTR_COL_INT), YTR_COL(row, target_frame, YTR_COL_INT),
    YTR_COL(row, onset_video_frame, YTR_COL_INT), YTR_COL(row, onset_via, YTR_COL_STRING),
    YTR_COL(row, n_pauses, YTR_COL_INT), YTR_COL(row, n_seeks, YTR_COL_INT),
    YTR_COL(row, frames_shown, YTR_COL_INT), YTR_COL(row, frames_repeated, YTR_COL_INT),
    YTR_COL(row, frames_dropped, YTR_COL_INT), YRSP_DATA_COLS,
};

static yscr_screen scr;
static ygfx_gfx gfx;
static yau_audio au;
static yvid_movie mv;
static ytl_timeline tl;
static ytl_event storage[64];
static ytr_trials trials;
static ytr_data data;
static yrsp_collector rsp;
static uint64_t table_arena[1024];
static ytb_table tab;

/* The trial's control state, changed by keys. */
static int playing, n_pauses, n_seeks, seek_to = -1, last_vframe = -1;

/* --sim: the participant's script, per trial index. Controls are times
 * from the trial's start; the press is a time from the onset's flip.
 * seek_dir x n_seeks presses of an arrow, 0.3 s apart from seek_s: two
 * Rights pass frame 60 from any start frame, so the onset comes from the
 * record (THE ONSET). */
typedef struct sim_trial { double press_s, pause_s, resume_s, seek_s; int seek_dir, n_seeks; } sim_trial;
static const sim_trial script[SIM_TRIALS] = {
    { 0.35, 0, 0, 0, 0, 0 }, { 0.40, 0.4, 0.9, 0, 0, 0 }, { 0.30, 0, 0, 0.3, 1, 2 },
    { -1, 0, 0, 0, 0, 0 },   { 0.45, 0, 0, 0.5, -1, 1 },  { 0.30, 0, 0, 0, 0, 0 },
};
static yrsp_input sim_ev[8];
static int n_sim_ev, sim_next;

static int fail(const char* why) {
    fprintf(stderr, "trial_movie: %s\n", why);
    return 1;
}

static yrsp_input key_event(int64_t t, const char* key, int press) {
    yrsp_input e;
    memset(&e, 0, sizeof e);
    e.t = t;
    e.kind = YRSP_KIND_KEYBOARD;
    e.type = press ? YRSP_PRESS : YRSP_RELEASE;
    e.control = (uint32_t)yrsp_scancode(key);
    e.stamp = YRSP_TIER_SIM;
    e.value = press ? 1.0f : 0.0f;
    return e;
}

/* Two key events (press, release 0.06 s later), kept in time order. */
static void sim_key(int64_t t, const char* key) {
    int i;
    for (i = 0; i < 2 && n_sim_ev < 8; i++) {
        yrsp_input e = key_event(t + (i ? YRT_MS(60) : 0), key, !i);
        int k = n_sim_ev++;
        while (k > sim_next && sim_ev[k - 1].t > e.t) { sim_ev[k] = sim_ev[k - 1]; k--; }
        sim_ev[k] = e;
    }
}

/* A low-contrast frame (the display may be somebody's working screen). */
static void make_frame(uint8_t* rgba, int i) {
    int x, y, bar = i * MOVIE_W / N_FRAMES;
    for (y = 0; y < MOVIE_H; y++)
        for (x = 0; x < MOVIE_W; x++) {
            uint8_t* p = rgba + ((size_t)y * MOVIE_W + x) * 4;
            int dx = x - MOVIE_W / 2, dy = y - MOVIE_H / 2, v = 100 + x / 8, blk = x / 8;
            if (y < 6 && blk < 8) v = ((i >> blk) & 1) ? 150 : 90;
            else if (x >= bar && x < bar + 3) v = 140;
            if (i >= TARGET && dx * dx + dy * dy <= 14 * 14) v = 220;
            p[0] = p[1] = p[2] = (uint8_t)v;
            p[3] = 255;
        }
}

static int write_movie(void) {
    static uint8_t rgba[MOVIE_W * MOVIE_H * 4];
    yvid_seq sq;
    yvid_seq_desc d;
    int i, rc = 0;
    memset(&sq, 0, sizeof sq);
    memset(&d, 0, sizeof d);
    d.path = MOVIE_PATH; d.w = MOVIE_W; d.h = MOVIE_H; d.fps_num = FPS; d.fps_den = 1;
    d.format = YVID_FMT_RGBA8; d.compression = YVID_SEQ_QOI; d.max_frames = N_FRAMES;
    if (!yvid_seq_open(&sq, &d)) return fail(yvid_seq_error(&sq));
    for (i = 0; i < N_FRAMES && rc == 0; i++) {
        const void* planes[3] = { rgba, NULL, NULL };
        const int32_t strides[3] = { MOVIE_W * 4, 0, 0 };
        make_frame(rgba, i);
        if (yvid_seq_write(&sq, planes, strides, NULL) != YVID_OK) rc = 1;
    }
    if (yvid_seq_finish(&sq) != YVID_OK) rc = 1;
    if (rc) fprintf(stderr, "trial_movie: %s\n", yvid_seq_error(&sq));
    yvid_seq_close(&sq);
    return rc;
}

static void le(uint8_t* p, uint32_t v, int n) { int k; for (k = 0; k < n; k++) p[k] = (uint8_t)(v >> (8 * k)); }

/* The soundtrack as a float WAV in memory, at the device's rate and
 * channels: its length must be the movie's in samples (SOUNDTRACK). */
static uint8_t* make_wav(uint32_t rate, uint16_t ch, size_t* n) {
    int64_t frames = (int64_t)N_FRAMES * rate / FPS, t0 = (int64_t)TARGET * rate / FPS, tn = (int64_t)(TONE_S * rate), i;
    int64_t ramp = rate / 200;   /* 5 ms raised-cosine edges: no click */
    size_t data_n = (size_t)frames * ch * 4;
    uint8_t* w = (uint8_t*)calloc(1, 44 + data_n);
    int c;
    if (!w) return NULL;
    memcpy(w, "RIFF", 4); le(w + 4, (uint32_t)(36 + data_n), 4); memcpy(w + 8, "WAVEfmt ", 8);
    le(w + 16, 16, 4); le(w + 20, 3, 2); le(w + 22, ch, 2); le(w + 24, rate, 4);
    le(w + 28, rate * ch * 4, 4); le(w + 32, (uint32_t)ch * 4, 2); le(w + 34, 32, 2);
    memcpy(w + 36, "data", 4); le(w + 40, (uint32_t)data_n, 4);
    for (i = 0; i < tn; i++) {
        double g = i < ramp ? 0.5 - 0.5 * cos(3.14159265358979 * (double)i / (double)ramp)
                 : i > tn - ramp ? 0.5 - 0.5 * cos(3.14159265358979 * (double)(tn - i) / (double)ramp) : 1.0;
        float v = (float)(0.0316 * g * sin(2 * 3.14159265358979 * 1000.0 * (double)i / rate));
        for (c = 0; c < ch; c++) memcpy(w + 44 + ((size_t)(t0 + i) * ch + c) * 4, &v, 4);
    }
    *n = 44 + data_n;
    return w;
}

/* An input: P and the arrows control the movie from this frame on; every
 * other event goes to the response collector, so a control is never an
 * EARLY press. */
static void on_input(const yrsp_input* in, int controls, const yscr_frame* f) {
    int64_t mt, to;
    int key = in->kind == YRSP_KIND_KEYBOARD ? (int)in->control : -1;
    if (key != yrsp_scancode("p") && key != yrsp_scancode("left") && key != yrsp_scancode("right")) {
        yrsp_feed(&rsp, in);
        return;
    }
    if (!controls || in->type != YRSP_PRESS || (in->flags & YRSP_IN_REPEAT)) return;
    if (in->control == (uint32_t)yrsp_scancode("p")) {
        if (playing ? yvid_pause_at(&mv, YVID_ASAP) > 0 : yvid_play_at(&mv, YVID_ASAP) > 0) {
            playing = !playing;
            n_pauses += !playing;
        }
    } else if (in->control == (uint32_t)yrsp_scancode("left") || in->control == (uint32_t)yrsp_scancode("right")) {
        mt = yvid_movie_time(&mv, f->onset);
        to = mt + (in->control == (uint32_t)yrsp_scancode("right") ? YRT_S(SEEK_S) : -YRT_S(SEEK_S));
        if (to < 0) to = 0;
        if (to > yvid_frame_time(FPS, 1, N_FRAMES - 1)) to = yvid_frame_time(FPS, 1, N_FRAMES - 1);
        if (yvid_seek(&mv, to, playing ? YVID_ASAP : YVID_STAY_PAUSED) > 0) {
            n_seeks++;
            seek_to = (int)yvid_due(FPS, 1, to, 0);
        }
    }
}

int main(int argc, char** argv) {
    static yrsp_result sim_res[MAX_ROWS];
    static row sim_row[MAX_ROWS];
    static int sim_seek_to[MAX_ROWS], sim_span[MAX_ROWS];
    yscr_frame f;
    yscr_caps caps;
    yau_caps ac;
    yrsp_source source;
    ytl_event fired[64];                   /* the timeline's capacity: all that can fire */
    ytr_trial_info ti = { 0 };
    yrsp_result res;
    yvid_info info0 = { 0 }, info1;
    yvid_record vr;
    ygfx_stim film;
    row r = { 0 };
    char line[1024], aline[1024], vline[1024], meta[4096], meta_path[1100];
    const char* out_path = "trial_movie.csv";
    uint8_t* wav = NULL;
    size_t wav_n = 0;
    uint64_t seed = 20261013;
    int i, n, sim = 0, scripted = 0, reps = 0, fullscreen = 0, sound = 1, aborted = 0, failed = 0, rows = 0, sim_bad = 0;
    int in_trial = 0, responding = 0, onset_set = 0, shown = 0, start_col, start = 0, vrc = YVID_OK;
    int64_t trial_t0 = 0, end_t = 0, onset_display = -1, sim_wait = -1;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--sim")) sim = scripted = 1;
        else if (!strcmp(argv[i], "--auto")) scripted = 1;
        else if (!strcmp(argv[i], "--fullscreen")) fullscreen = 1;
        else if (!strcmp(argv[i], "--no-sound")) sound = 0;
        else if (!strcmp(argv[i], "--reps") && i + 1 < argc) reps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) seed = (uint64_t)strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out_path = argv[++i];
        else {
            fprintf(stderr, "usage: trial_movie [--sim] [--auto] [--fullscreen] [--no-sound] [--reps N] [--seed N] [--out FILE]\n");
            return 2;
        }
    }
    if (reps == 0) reps = scripted ? 2 : 3;
    if (reps < 1 || reps > MAX_ROWS / 3) { fprintf(stderr, "trial_movie: --reps takes 1..%d\n", MAX_ROWS / 3); return 2; }
    if (write_movie()) return 1;

    /* the display, the sound device, the movie on its timeline base, its soundtrack */
    if (!ygfx_open_screen(&gfx, &scr, &(yscr_desc){ .backend = sim ? YSCR_BACKEND_SIM : 0, .windowed = !fullscreen },
                          &(ygfx_desc){ .background = { 0.35f, 0.35f, 0.35f } }))
        return fail(ygfx_error(&gfx));
    yscr_get_caps(&scr, &caps);
    if (sound && !yau_open(&au, &(yau_desc){ .backend = sim ? YAU_BACKEND_NULL : YAU_BACKEND_AUTO }))
        return fail(yau_error(&au));
    if (!ytl_open(&tl, &(ytl_desc){ .events = storage, .event_capacity = 64, .n_channels = N_CHANNELS }))
        return fail(ytl_error(&tl));
    if (!yvid_open(&mv, &gfx, &(yvid_desc){ .path = MOVIE_PATH, .timeline = &tl, .base = MOVIE })) return fail(yvid_error(&mv));
    if (sound) {
        yau_get_caps(&au, &ac);
        if (!(wav = make_wav(ac.rate, ac.channels, &wav_n))) return fail("no memory for the soundtrack");
        if (yvid_soundtrack(&mv, &au, &(yvid_soundtrack_desc){ .data = wav, .size = wav_n, .ramp_ns = YRT_MS(5) }) < 0)
            return fail(yvid_error(&mv));
    }
    film = yvid_stim(&mv, &(yvid_stim_desc){ .w = MOVIE_W * SCALE, .h = MOVIE_H * SCALE, .linear = true });

    /* the conditions, in a random order */
    if (!ytb_open_csv(&tab, &(ytb_csv_desc){ .text = conditions_csv, .len = sizeof conditions_csv - 1,
                                             .arena = table_arena, .arena_size = sizeof table_arena }))
        return fail(ytb_error(&tab));
    start_col = ytb_col(&tab, "start_frame");
    if (!ytr_open(&trials, &(ytr_desc){ .table = &tab, .reps = reps, .order = YTR_ORDER_RANDOM, .rng = ytr_splitmix,
                                        .rng_ctx = &seed }))
        return fail(ytr_error(&trials));

    /* Space, within 1.5 s of the disk's onset, none before 0.1 s */
    source = yrsp_sdl_source(&(yrsp_sdl_ctx){ .raw_keyboard = caps.raw_keyboard }, YRSP_KIND_KEYBOARD);
    if (!yrsp_open(&rsp, &(yrsp_desc){ .choices = keys, .n_choices = 1, .duration_ns = YRT_S(RESPONSE_S),
                                       .min_rt_ns = YRT_S(MIN_RT_S), .sources = &source, .n_sources = 1 }))
        return fail(yrsp_error(&rsp));

    yscr_describe(&scr, line, sizeof line);
    if (sound) yau_describe(&au, aline, sizeof aline);
    else snprintf(aline, sizeof aline, "none (--no-sound)");
    yvid_describe(&mv, vline, sizeof vline);
    printf("%s\n%s\n%s\n", line, aline, vline);
    snprintf(meta, sizeof meta, "screen=%s\naudio=%s\nmovie=%s\nmovie_content=%d x %d at %d fps, %d frames, disk from "
             "frame %d, a %.2f s 1 kHz tone at -30 dBFS from it\nysp_response=%s\nkeyboard=%s\n", line, aline, vline,
             MOVIE_W, MOVIE_H, FPS, N_FRAMES, TARGET, TONE_S, yrsp_version(), scripted ? "scripted (--sim, --auto)" : source.note);
    snprintf(meta_path, sizeof meta_path, "%s.meta", out_path);
    if (!ytr_data_open(&data, &(ytr_data_desc){ .path = out_path, .trials = &trials, .cols = cols,
            .n_cols = (int)(sizeof cols / sizeof cols[0]), .meta_path = meta_path, .meta = meta }))
        return fail(ytr_data_error(&data));

    while (!aborted) {
        SDL_Event ev;
        yrsp_input in;
        int st, landed, have_vr, k;
        int rc = yscr_begin(&scr, &f);
        if (rc == YSCR_QUIT) { aborted = 1; break; }
        if (rc != YSCR_OK) { failed = fail(yscr_error(&scr)); break; }

        if (!in_trial && f.onset >= end_t) {
            /* a new trial: a seek to the start frame that plays once its
             * frames are in; the disk's event goes on the base when it lands */
            if (ytr_next(&trials, &ti) < 0 || ti.index >= MAX_ROWS) break;
            start = ytb_int(&tab, ti.condition, start_col);
            ytl_clear(&tl, MOVIE);
            if (yvid_seek_frame(&mv, start, YVID_ASAP) < 0) { failed = fail(yvid_error(&mv)); break; }
            yvid_get_info(&mv, &info0);
            yrsp_arm(&rsp, f.onset);
            playing = in_trial = responding = 1;
            onset_set = shown = n_pauses = n_seeks = 0;
            seek_to = last_vframe = -1;
            onset_display = sim_wait = -1;
            r.onset_video_frame = YTR_MISSING_INT;
            r.onset_via = "none";
            trial_t0 = f.onset;
            n_sim_ev = sim_next = 0;
            if (scripted && ti.index < SIM_TRIALS) {
                const sim_trial* s = &script[ti.index];
                if (s->pause_s > 0) { sim_key(trial_t0 + YRT_S(s->pause_s), "p"); sim_key(trial_t0 + YRT_S(s->resume_s), "p"); }
                for (k = 0; k < s->n_seeks; k++) sim_key(trial_t0 + YRT_S(s->seek_s + 0.3 * k), s->seek_dir > 0 ? "right" : "left");
            }
        }

        /* --sim: the press from the onset frame's flip record */
        for (i = 0; scripted && sim_wait >= 0 && i < f.n_done; i++)
            if (f.done[i].index == sim_wait && yrsp_onset_flip(&f.done[i]).final) {
                if (ti.index < SIM_TRIALS && script[ti.index].press_s > 0)
                    sim_key(f.done[i].onset + YRT_S(script[ti.index].press_s), "space");
                sim_wait = -1;
            }
        while (yscr_poll(&scr, &ev, NULL))
            if (yscr_event_input(&scr, &ev, &in)) on_input(&in, responding, &f);
        for (; scripted && sim_next < n_sim_ev && sim_ev[sim_next].t <= f.onset; sim_next++) on_input(&sim_ev[sim_next], responding, &f);
        st = yrsp_update_frame(&rsp, &f);

        /* the movie: its clock from the sound device's, then the frame due */
        if (sound) {
            yau_update(&au);
            if (yvid_follow_audio(&mv, &au, &f) < 0) { failed = fail(yvid_error(&mv)); break; }
        }
        vrc = yvid_update(&mv, &f);
        if (vrc < 0) { failed = fail(yvid_error(&mv)); break; }
        have_vr = yvid_last(&mv, &vr) == YVID_OK && vr.display == f.index;
        if (in_trial && have_vr && !shown) {
            /* the seek landed: the base is at the start frame now, so the
             * disk's event goes on it here. Added before, while the base
             * still stood at the last trial's end, it would fire at once */
            ytl_seq q = ytl_seq_on(&tl, MOVIE);
            ytl_at(&q, yvid_frame_time(FPS, 1, TARGET));
            ytl_on(&q, DISK_ON);
            if (q.err) { failed = fail(ytl_strerror(q.err)); break; }
            shown = 1;
        }
        n = ytl_evaluate(&tl, &(ytl_frame){ f.onset, f.period, f.index }, fired, 64);
        if (responding && !onset_set && (landed = yrsp_set_landing(&rsp, fired, n, DISK_ON)) >= 0) {
            onset_set = 1;
            onset_display = fired[landed].frame;
            r.onset_via = "timeline";
        }
        if (in_trial && have_vr) {
            last_vframe = (int)vr.frame;
            if (responding && vr.frame >= TARGET && r.onset_video_frame == YTR_MISSING_INT) {
                r.onset_video_frame = (int)vr.frame;
                if (!onset_set) {   /* a seek skipped the event: the record's frame is the onset */
                    yrsp_onset o;
                    memset(&o, 0, sizeof o);
                    o.t_ns = vr.predicted;
                    o.frame = f.index;
                    o.src = YRSP_ONSET_PLAN;
                    yrsp_set_onset(&rsp, &o);
                    onset_set = 1;
                    onset_display = f.index;
                    r.onset_via = "record";
                } else if (onset_display != f.index) {
                    fprintf(stderr, "trial_movie: trial %d: the event fired on display frame %lld, frame %d showed on %lld\n",
                            ti.index, (long long)onset_display, TARGET, (long long)f.index);
                    sim_bad = 1;
                }
                sim_wait = onset_display;
            }
        }

        /* the response, the deadline or the movie's end: pause, then a blank */
        if (responding && (st == YRSP_ENDED || vrc == YVID_ENDED)) {
            if (playing && yvid_pause_at(&mv, YVID_ASAP) < 0) { failed = fail(yvid_error(&mv)); break; }
            playing = responding = 0;
            end_t = f.onset + YRT_S(ITI_S);
        }
        if (in_trial && !responding && f.onset >= end_t) {
            /* the onset's flip record is in: the row */
            yvid_get_info(&mv, &info1);
            yrsp_finish(&rsp, &res);
            r.trial_index = ti.index;
            r.target_frame = TARGET;
            r.n_pauses = n_pauses;
            r.n_seeks = n_seeks;
            r.frames_shown = (int)(info1.shown - info0.shown);
            r.frames_repeated = (int)(info1.repeated - info0.repeated);
            r.frames_dropped = (int)(info1.dropped - info0.dropped);
            ytr_update(&trials, (res.flags & YRSP_R_RESPONDED) ? 1 : YTR_INVALID, NULL);
            if (yrsp_data_set(&data, &res) < 0 || ytr_data_write(&data, &r) < 0) failed = fail(ytr_data_error(&data));
            printf("trial %2d start %2d onset frame %d (%s) ", ti.index, start, r.onset_video_frame, r.onset_via);
            if (res.flags & YRSP_R_RESPONDED) printf("rt %.3f s", YRT_TO_S(res.rt_ns));
            else printf("no response");
            printf("; %d pauses, %d seeks; frames shown %d, repeated %d, dropped %d\n", n_pauses, n_seeks, r.frames_shown,
                   r.frames_repeated, r.frames_dropped);
            sim_res[ti.index] = res;
            sim_row[ti.index] = r;
            sim_seek_to[ti.index] = seek_to;
            sim_span[ti.index] = last_vframe - start + 1;   /* video frames the trial passed */
            rows++;
            in_trial = 0;
        }

        yscr_mark(&scr, YSCR_PHASE_EVALUATE);
        ygfx_begin(&gfx, &f);
        if (responding && shown) ygfx_draw(&gfx, &film);
        ygfx_end(&gfx);
        yscr_flip(&scr);
    }
    if (ytr_data_finish(&data) < 0) failed = fail(ytr_data_error(&data));
    yvid_describe(&mv, vline, sizeof vline);
    printf("%s\n", vline);
    if (sound) { yau_describe(&au, aline, sizeof aline); printf("%s\n", aline); }
    yvid_close(&mv);
    if (sound) yau_close(&au);
    ygfx_close(&gfx);
    yscr_close(&scr);
    free(wav);
    remove(MOVIE_PATH);
    printf("%d trials written to %s%s\n", rows, out_path, aborted ? " (stopped with Shift+Esc)" : "");

    if (scripted && !failed && !aborted && reps == 2) {
        int n_dropped = 0;
        for (i = 0; i < rows && i < SIM_TRIALS; i++) {
            const yrsp_result* q = &sim_res[i];
            const row* w = &sim_row[i];
            int want_frame = sim_seek_to[i] > TARGET ? sim_seek_to[i] : TARGET, ok = 1;
            if (script[i].press_s > 0)   /* RT from the flip record, as scripted */
                ok = (q->flags & YRSP_R_RESPONDED) && q->response == 0 && q->rt_ns == YRT_S(script[i].press_s);
            else
                ok = !(q->flags & YRSP_R_RESPONDED) && (q->flags & YRSP_R_TIMEOUT);
            ok = ok && q->onset_src == YRSP_ONSET_FLIP && (sim ? q->onset_tier == YSCR_TIER_SIM : q->onset_tier > 0) && q->n_early == 0;
            ok = ok && w->n_pauses == (script[i].pause_s > 0) && w->n_seeks == script[i].n_seeks;
            ok = ok && w->onset_video_frame == want_frame && !strcmp(w->onset_via, want_frame > TARGET ? "record" : "timeline");
            /* every video frame the trial passed was shown or dropped; a
             * drop is the host's (a late decode on a loaded machine), so it
             * is counted, not failed */
            ok = ok && w->frames_shown > 0 && (sim_seek_to[i] >= 0 || w->frames_shown + w->frames_dropped == sim_span[i]);
            n_dropped += w->frames_dropped;
            if (!ok) {
                fprintf(stderr, "trial_movie: --sim: trial %d: rt %.9f flags 0x%x onset src %d tier %d pauses %d seeks %d "
                                "onset frame %d (want %d) via %s, shown %d dropped %d\n", i, YRT_TO_S(q->rt_ns), (unsigned)q->flags,
                        q->onset_src, q->onset_tier, w->n_pauses, w->n_seeks, w->onset_video_frame, want_frame, w->onset_via,
                        w->frames_shown, w->frames_dropped);
                sim_bad = 1;
            }
        }
        if (rows != SIM_TRIALS) { fprintf(stderr, "trial_movie: --sim: %d rows, not %d\n", rows, SIM_TRIALS); sim_bad = 1; }
        if (sim_bad) failed = 1;
        else printf("--sim: every trial came out as scripted (%d video frames dropped by the host)\n", n_dropped);
    } else if (scripted && !failed && !aborted) printf("--sim: the self-check needs --reps 2\n");
    return failed;
}
