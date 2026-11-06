/* trial_timed_response.c - the timed-response (forced-response) paradigm of
 * Hardwick, Forrence, Krakauer and Haith 2019 (Nature Human Behaviour 3:1252,
 * "Time-dependent competition between goal-directed and habitual response
 * preparation") as a small experiment: ysp/audio.h, ysp/screen.h,
 * ysp/gfx.h, ysp/timeline.h, ysp/trials.h, ysp/response.h, ysp/outline.h
 * and ysp/rt.h.
 *
 * Four fingers rest on J, K, L and ; (physical keys). Four symbols (letters
 * of the Phoenician alphabet, as in the paper) map to the four keys; four
 * boxes at the bottom of the screen stand for the keys, left to right.
 * Mapping A: symbol i is key i. Mapping B swaps the keys of symbols 1 and 2
 * (the remapped symbols); 0 and 3 are the consistently mapped ones.
 * Stimuli come in 20-trial sub-blocks, each symbol 5 times, never three
 * times in a row (ysp/trials.h: blocked groups, max_run 2). Three blocks:
 *   forced      100 trials. Four 1000 Hz tones 0.4 s apart, the first 0.5 s
 *               after the trial starts. Respond together with the fourth
 *               tone. The symbol appears at a preparation time (PT) before
 *               the fourth tone, drawn uniformly from 0 to 1.2 s. The
 *               pressed key's box turns green (correct) or red; "too early"
 *               or "too late" shows when the press is more than 0.1 s from
 *               the fourth tone (or there is none 0.8 s after it). 1 s of
 *               feedback, 0.4 s blank. No penalty for errors.
 *   training    100 reaction-time trials. The symbol and a 750 Hz tone
 *               together. Correct: a 1320 Hz tone, the next trial 0.3 s
 *               later. Wrong: a buzzer, then 1 s in which keys do nothing,
 *               then respond again to the same symbol, until correct. The
 *               block's time goes to stdout.
 *   criterion   No time limit. Box feedback for 0.5 s. Ends when each symbol
 *               has had 5 correct responses in a row (at least 20 trials;
 *               at most 400).
 *
 * One clock. The four tones and the symbol are events on one trial
 * timeline (ysp/timeline.h). The tones go to yau_play_at() at their exact
 * times through the timeline's audio look-ahead (ytl_peek() of
 * [last end, now + yau_lead_ns() + two frames) each frame); the symbol
 * lands on the frame nearest its time. The measured PT is the response's
 * RT from the symbol's flip onset (its flip record): the collector's
 * onset is the symbol, from the schedule's time until it lands, then the
 * landing's plan, then the record. A forced response may come before the
 * symbol: the window's minimum RT is negative (ysp/response.h v0.8), so
 * such a press is a response with a negative PT. Each forced trial sets
 * its window with yrsp_set_window(): from the first tone to 0.8 s after
 * the fourth, as RTs from the symbol (to within the symbol's residual,
 * half a frame at most). The timing error is the key time minus the
 * fourth tone's onset (its yau_onset record: the device-clock fit's
 * time). The row keeps the plan, both measurements, and each source's
 * residual and tier.
 *
 * Data (--out): one row per trial: block, block_type, mapping, trial,
 * sub_block, stimulus (0..3), remapped, correct_key, response, correct,
 * habitual (B, remapped symbol: the response is mapping A's key),
 * first_response and n_errors (training), pt_planned_s, pt_s (forced),
 * symbol_rt_s (training, criterion: key minus the symbol's flip onset; in
 * training the final, correct response's), timing_error_s, too_early,
 * too_late, stim_residual_s (flip onset minus the symbol's target),
 * stim_tier, tone_residual_s (the last tone's onset minus its target),
 * tone_tier (the worst of the trial's tones), tone_flags (YAU_ONSET_*, OR
 * of them), rsp_tier, rsp_flags, n_early. Seconds; settings in FILE.meta.
 * After a forced block: p(correct) in 100 ms sliding windows of measured
 * PT (centers 0.05 to 1.15 s in 0.05 s steps, the paper's Fig. 2f); under
 * mapping B for the consistent symbols, and for the remapped ones also
 * p(old-mapping response). Shift+Esc stops; the rows so far are kept.
 *
 * --sim: the simulated display and the null audio device on the virtual
 * clock, and blocks criterion A, training A (20 trials), forced A, criterion
 * B, forced B. The scripted participant presses at the fourth tone plus a
 * known jitter (up to 60 ms, and 150 ms early or 160 ms late on two trials
 * in ten). Its choice depends on the PT it saw (press minus the symbol's
 * onset): a guess below 0.40 s, the correct key from 0.40 s; under B, on a
 * remapped symbol, mapping A's key from 0.40 s and the correct key from
 * 0.65 s. Criterion and training responses are scripted errors and
 * correct keys timed from the symbol's flip record. It checks the order
 * (5 of each symbol per sub-block, no run of 3), each tone within half a
 * sample of its plan, the timing error against the jitter, the measured
 * PT against the plan plus the jitter (within half a frame), the curves
 * against the PTs the script saw and its step, the training rows and the
 * start tone on the symbol's flip, and the criterion's stopping trial; it
 * exits 1 on a mismatch. About 0.1 s: the virtual clock does not wait.
 * --auto runs the same participant on a real window and sound device and
 * prints the worst residuals instead of checking them.
 *
 * Usage: trial_timed_response [--sim | --auto] [--fullscreen] [--pulse]
 *        [--blocks LIST] [--mapping A|B] [--trials N] [--seed N] [--out FILE] [--font PATH]
 *        [--text-font PATH]
 *   --blocks LIST  comma-separated forced, training, criterion, each with
 *                  :A or :B for its mapping (default --mapping): the paper's
 *                  assessment is criterion:A,criterion:B,forced:B
 *   --trials N     trials of a forced or training block, a multiple of 20
 *   --pulse        Linux: PulseAudio, not ALSA exclusive (AUTO), which
 *                  suspends the desktop's sound while it runs
 *   --font PATH    the symbols' font (Noto Sans Phoenician or Segoe UI
 *                  Historic); a 5 x 7 bitmap without it, as on a CI runner
 * Exit code: 0 (also after Shift+Esc), 1 when something did not open or a
 * --sim check failed, 2 for a bad argument.
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#define YSP_GFX_IMPLEMENTATION
#include "ysp/gfx.h"
#define YSP_AUDIO_IMPLEMENTATION
#include "ysp/audio.h"
#define YSP_OUTLINE_IMPLEMENTATION
#include "ysp/outline.h"
#define YSP_TIMELINE_IMPLEMENTATION
#include "ysp/timeline.h"
#define YSP_TRIALS_IMPLEMENTATION
#include "ysp/trials.h"
#define YSP_RESPONSE_IMPLEMENTATION
#include "ysp/response.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { STIM_ON, FB_ON, EARLY_ON, LATE_ON, N_CHANNELS };
enum { TRIAL = 1 };
enum { END_OF_TRIAL = 1, TONE_MARK = 10 };           /* TONE_MARK + k: tone k; user: the sound */
enum { FORCED, TRAINING, CRITERION };
enum { SND_COUNT, SND_START, SND_GOOD, SND_BUZZ, N_SND };
static const char* const type_names[] = { "forced", "training", "criterion" };

#define N_STIM          4
#define SUB_TRIALS      20
#define N_TONES         4
#define LEAD_S          0.5     /* trial start to the first tone */
#define TONE_GAP_S      0.4
#define PT_MAX_S        1.2     /* the first tone to the fourth */
#define LATE_LIMIT_S    0.8     /* the window ends this long after the fourth tone */
#define TIMING_TOL_S    0.1
#define FEEDBACK_S      1.0
#define ITI_S           0.4
#define TRAIN_DELAY_S   0.3
#define PENALTY_S       1.0
#define CRIT_PRE_S      0.5
#define CRIT_FEEDBACK_S 0.5
#define CRIT_STREAK     5
#define CRIT_MAX_SUBS   20
#define REMAP_1         1       /* mapping B swaps these two symbols' keys */
#define REMAP_2         2
#define MAX_BLOCK       (CRIT_MAX_SUBS * SUB_TRIALS)
#define N_WIN           23      /* window centers 0.05 .. 1.15 s */
#define SYMBOL_PX       140.0f
#define TEXT_PX         44.0f
#define BOX_PX          90.0f
#define BOX_GAP         130.0f
#define BOX_Y           220.0f

#ifdef _WIN32
#define SYMBOL_FONT "C:/Windows/Fonts/seguihis.ttf"
#define TEXT_FONT   "C:/Windows/Fonts/arial.ttf"
#else
#define SYMBOL_FONT "/usr/share/fonts/truetype/noto/NotoSansPhoenician-Regular.ttf"
#define TEXT_FONT   "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
#endif

static const yrsp_choice keys[N_STIM] = { { .key = "j" }, { .key = "k" }, { .key = "l" }, { .key = ";" } };
static const char* const key_names[N_STIM] = { "j", "k", "l", ";" };
/* alf, gaml, het, shin */
static const uint32_t symbol_cp[N_STIM] = { 0x10900, 0x10902, 0x10907, 0x10914 };
static const char letters[] = "aelorty";               /* glyph ids N_STIM + i */
/* 5 x 7 bitmaps, rows top first, bit 4 the left column: the symbols, then
 * the letters, for a machine without the fonts. */
static const uint8_t bitmap[N_STIM + 7][7] = {
    { 0x04, 0x0E, 0x15, 0x04, 0x04, 0x04, 0x04 }, { 0x1F, 0x01, 0x01, 0x1F, 0x10, 0x10, 0x1F },
    { 0x11, 0x1B, 0x15, 0x11, 0x11, 0x11, 0x11 }, { 0x0E, 0x11, 0x15, 0x15, 0x15, 0x11, 0x0E },
    { 0x00, 0x00, 0x0E, 0x01, 0x0F, 0x11, 0x0F }, { 0x00, 0x00, 0x0E, 0x11, 0x1F, 0x10, 0x0E },
    { 0x0C, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E }, { 0x00, 0x00, 0x0E, 0x11, 0x11, 0x11, 0x0E },
    { 0x00, 0x00, 0x16, 0x19, 0x10, 0x10, 0x10 }, { 0x08, 0x08, 0x1C, 0x08, 0x08, 0x09, 0x06 },
    { 0x00, 0x00, 0x11, 0x11, 0x0F, 0x01, 0x0E },
};
static const float GREEN[3] = { 0.1f, 0.7f, 0.2f }, RED[3] = { 0.8f, 0.15f, 0.1f }, INK[3] = { 0.85f, 0.85f, 0.85f };

/* The scripted participant (--sim, --auto). */
#define SIM_T_A      0.40       /* a response prepared by mapping A from this PT */
#define SIM_T_B      0.65       /* by mapping B, on a remapped symbol */
#define SIM_RT       0.45       /* training and criterion RT */
static double sim_jitter(int i) { return i % 10 == 3 ? -0.150 : i % 10 == 7 ? 0.160 : 0.012 * ((i * 7) % 11 - 5); }

typedef struct block { int type, mapping_b, trials; } block;
typedef struct row {
    int block, trial, sub_block, stimulus, habitual, n_errors, stim_tier, tone_tier, tone_flags;
    bool remapped, correct, too_early, too_late;
    const char *block_type, *mapping, *correct_key, *first_response;
    int64_t pt_planned_ns, pt_ns, symbol_rt_ns, timing_error_ns, stim_residual_ns, tone_residual_ns;
} row;
static const ytr_col cols[] = {
    YTR_COL(row, block, YTR_COL_INT), YTR_COL(row, block_type, YTR_COL_STRING), YTR_COL(row, mapping, YTR_COL_STRING),
    YTR_COL(row, trial, YTR_COL_INT), YTR_COL(row, sub_block, YTR_COL_INT), YTR_COL(row, stimulus, YTR_COL_INT),
    YTR_COL(row, remapped, YTR_COL_BOOL), YTR_COL(row, correct_key, YTR_COL_STRING), YTR_COL_SET("response", YTR_COL_STRING),
    YTR_COL(row, correct, YTR_COL_BOOL), YTR_COL(row, habitual, YTR_COL_INT), YTR_COL(row, first_response, YTR_COL_STRING),
    YTR_COL(row, n_errors, YTR_COL_INT), YTR_COL(row, pt_planned_ns, YTR_COL_TIME_NS), YTR_COL(row, pt_ns, YTR_COL_TIME_NS),
    YTR_COL(row, symbol_rt_ns, YTR_COL_TIME_NS), YTR_COL(row, timing_error_ns, YTR_COL_TIME_NS),
    YTR_COL(row, too_early, YTR_COL_BOOL), YTR_COL(row, too_late, YTR_COL_BOOL),
    YTR_COL(row, stim_residual_ns, YTR_COL_TIME_NS), YTR_COL(row, stim_tier, YTR_COL_INT),
    YTR_COL(row, tone_residual_ns, YTR_COL_TIME_NS), YTR_COL(row, tone_tier, YTR_COL_INT),
    YTR_COL(row, tone_flags, YTR_COL_INT), YTR_COL_SET("rsp_tier", YTR_COL_INT), YTR_COL_SET("rsp_flags", YTR_COL_INT),
    YTR_COL_SET("n_early", YTR_COL_INT),
};

static yscr_screen scr;
static ygfx_gfx gfx;
static yau_audio au;
static yol_ctx ol;
static ygfx_cset glyphs;
static double advance[N_STIM + 7];      /* em */
static ytl_timeline tl;
static ytl_event storage[64];
static ytr_trials trials;
static ytr_data data;
/* One collector: a forced trial sets its own window (yrsp_set_window()),
 * the others have the desc's (no deadline). */
static yrsp_collector col, *rsp = &col;
static yau_buf snd[N_SND];
/* 7250: an order with a run across a sub-block boundary unless the rule
 * spans them, so --sim checks constraints_span_blocks */
static uint64_t seed = 7250, sim_rng = 0x5eed;

static ygfx_stim symbol, boxes[N_STIM], fb, word_early, word_late;
static ygfx_citem symbol_item, early_items[9], late_items[8];
static const ygfx_bind binds[] = {
    { .stim = &symbol, .param = YGFX_P_VISIBLE, .channel = STIM_ON },
    { .stim = &fb, .param = YGFX_P_VISIBLE, .channel = FB_ON },
    { .stim = &word_early, .param = YGFX_P_VISIBLE, .channel = EARLY_ON },
    { .stim = &word_late, .param = YGFX_P_VISIBLE, .channel = LATE_ON },
};

static block blocks[16];
static int n_blocks, cur = -1, block_open, in_trial, sim, scripted, failed, bad, rows;
static int64_t block_t0, frame_period;

/* The trial in progress. */
static struct {
    ytr_trial_info ti;
    int type, mapping_b, stim, key, responding, row_due, penalty, n_errors, first, n_tones, stim_seen;
    int64_t anchor, stim_target, stim_frame, stim_land, tone_t[N_TONES], penalty_end, pt_planned;
    yau_id tone_id[N_TONES];
    int tone_rc[N_TONES];
    yau_onset tone_rec[N_TONES];
    yscr_record stim_rec;
    yrsp_result res;
} tr;
static int64_t peek_from;               /* the audio look-ahead's next window start */

/* One block's rows, for the analysis and the --sim checks. */
typedef struct logged {
    int stim, choice, correct, n_errors, too_early, too_late, sim_choice;
    int64_t pt, rt, err, pt_planned, stim_res, tone_res_max, tone_gap_err;
    double sim_pt;                      /* the PT the participant saw; NAN: none */
} logged;
static logged lg[MAX_BLOCK];
static int n_lg, streak[N_STIM], criterion_met;

static int key_of(int s, int mapping_b) {
    if (mapping_b && s == REMAP_1) return REMAP_2;
    if (mapping_b && s == REMAP_2) return REMAP_1;
    return s;
}
static int is_remapped(int s, int mapping_b) { return mapping_b && (s == REMAP_1 || s == REMAP_2); }
static int sim_guess(void) { return (int)(ytr_splitmix(&sim_rng) * N_STIM); }

/* --- glyphs ------------------------------------------------------------- */

static uint8_t* read_file(const char* path, long* n) {
    uint8_t* bytes = NULL;
    FILE* fp = fopen(path, "rb");
    *n = 0;
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END); *n = ftell(fp); fseek(fp, 0, SEEK_SET);
    if (*n > 0 && (bytes = (uint8_t*)malloc((size_t)*n)) && fread(bytes, 1, (size_t)*n, fp) != (size_t)*n) { free(bytes); bytes = NULL; }
    fclose(fp);
    return bytes;
}

/* Glyph g in em at pen 0, 0, y down: from the font when it has it, else
 * the bitmap. Returns 1 for the font, 0 for the bitmap, -1 on a failure. */
static int add_glyph(yol_cset* s, yol_path* p, const yol_font* font, uint32_t cp, int g) {
    uint32_t gid = font ? yol_font_glyph_index(font, cp) : 0;
    int r, c, rc;
    double lsb;
    yol_path_clear(p);
    if (gid) {
        yol_glyph_desc gd;
        memset(&gd, 0, sizeof gd);
        rc = yol_font_glyph(&ol, font, gid, p, &gd);
        if (yol_font_hmetrics(font, gid, &advance[g], &lsb) < 0) advance[g] = 0.6;
    } else {
        for (r = 0; r < 7; r++)
            for (c = 0; c < 5; c++)
                if (bitmap[g][r] & (0x10 >> c)) yol_rect(p, 0.1 * c, 0.1 * (r - 7), 0.1, 0.1, 0, 0);
        rc = yol_path_end(p);
        advance[g] = 0.6;
    }
    if (rc == YOL_OK) rc = yol_cset_add(s, (uint32_t)g, p);
    return rc != YOL_OK ? -1 : gid != 0;
}

/* One curve set: the 4 symbols, then the letters of the feedback words. */
static int make_glyphs(const char* sym_path, const char* text_path, char* what, size_t cap) {
    yol_font fs, ft;
    yol_cset s;
    yol_cset_desc od;
    yol_path p;
    ygfx_cset_desc d;
    long ns, nt;
    char why[256];
    uint8_t *bs = read_file(sym_path, &ns), *bt = read_file(text_path, &nt);
    int i, k, real_s = 0, real_t = 0, ok_s = bs && yol_font_view(&fs, bs, (size_t)ns, 0, why, sizeof why) == YOL_OK;
    int ok_t = bt && yol_font_view(&ft, bt, (size_t)nt, 0, why, sizeof why) == YOL_OK;
    memset(&od, 0, sizeof od);
    od.n_glyphs = N_STIM + 7;
    if (!yol_cset_open(&s, &ol, &od)) { free(bs); free(bt); return -1; }
    yol_path_init(&p, &ol);
    for (i = 0; i < N_STIM + 7; i++) {
        k = i < N_STIM ? add_glyph(&s, &p, ok_s ? &fs : NULL, symbol_cp[i], i)
                       : add_glyph(&s, &p, ok_t ? &ft : NULL, (uint32_t)letters[i - N_STIM], i);
        if (k < 0) break;
        if (i < N_STIM) real_s += k; else real_t += k;
    }
    memset(&d, 0, sizeof d);
    d.texels = s.texels; d.n_texels = s.n_texels; d.words = s.words; d.n_words = s.n_words;
    if (i == N_STIM + 7) glyphs = ygfx_cset_make(&gfx, &d);
    yol_path_free(&p);
    yol_cset_close(&s);
    free(bs);
    free(bt);   /* the set holds the curves */
    snprintf(what, cap, "symbols %s, text %s", real_s == N_STIM ? sym_path : real_s ? "mixed" : "5 x 7 bitmap",
             real_t == 7 ? text_path : real_t ? "mixed" : "5 x 7 bitmap");
    return i == N_STIM + 7 && glyphs.id ? 0 : -1;
}

/* A word as a curve run, its ink centered at x, y. */
static ygfx_stim word(ygfx_citem* it, const char* s, float y) {
    ygfx_crun_desc d;
    double pen = 0;
    int n = 0;
    for (; *s; s++) {
        const char* at = strchr(letters, *s);
        int g = at ? N_STIM + (int)(at - letters) : -1;
        if (g >= 0) {
            memset(&it[n], 0, sizeof it[n]);
            it[n].x = (float)pen; it[n].glyph = (float)g; it[n].scale = 1; it[n].gate = 1;
            n++;
        }
        pen += (g >= 0 ? advance[g] : 0.3) * TEXT_PX;
    }
    memset(&d, 0, sizeof d);
    d.set = glyphs; d.items = it; d.n = n; d.size = TEXT_PX; d.y = y;
    memcpy(d.color, INK, sizeof d.color);
    return ygfx_crun(&gfx, &d);
}

static ygfx_stim rect(float x, float y, float w, float stroke, const float* color) {
    ygfx_shape_desc d;
    memset(&d, 0, sizeof d);
    d.shape = YGFX_RECT; d.x = x; d.y = y; d.w = d.h = w; d.stroke = stroke;
    memcpy(d.color, color, sizeof d.color);
    return ygfx_shape(&d);
}
static float box_x(int k) { return ((float)k - 1.5f) * BOX_GAP; }

/* --- the scripted participant -------------------------------------------- */

typedef struct sim_press { int64_t t; int key, state; } sim_press;   /* key -1: chosen when due */
static sim_press sp[4];
static int n_sp;

static void sim_add(int64_t t, int key) {
    if (n_sp < 4) { sp[n_sp].t = t; sp[n_sp].key = key; sp[n_sp].state = 0; n_sp++; }
}

/* A new trial: a key still down stays queued for its release; the
 * collector would take the next press of that key for a held one. */
static void sim_reset(void) {
    int i, n = 0;
    for (i = 0; i < n_sp; i++)
        if (sp[i].state == 1) sp[n++] = sp[i];
    n_sp = n;
}

/* The forced choice at press time p: from the PT seen, the symbol's flip
 * record or, before it, its landing; no symbol yet: a guess. */
static int sim_forced_choice(int64_t p) {
    logged* l = &lg[n_lg];
    int64_t on = tr.stim_seen ? tr.stim_rec.onset : tr.stim_frame >= 0 ? tr.stim_land : 0;
    double pt = on && on <= p ? (double)(p - on) / 1e9 : NAN;
    int rem = is_remapped(tr.stim, tr.mapping_b);
    l->sim_pt = pt;
    if (pt >= (rem ? SIM_T_B : SIM_T_A)) l->sim_choice = tr.key;
    else if (rem && pt >= SIM_T_A) l->sim_choice = key_of(tr.stim, 0);
    else l->sim_choice = sim_guess();
    return l->sim_choice;
}

static void sim_feed(int64_t now) {
    int i;
    for (i = 0; i < n_sp; i++) {
        yrsp_input e;
        memset(&e, 0, sizeof e);
        e.stamp = YRSP_TIER_SIM;
        if (sp[i].state == 0 && sp[i].t <= now) {
            if (sp[i].key < 0) sp[i].key = sim_forced_choice(sp[i].t);
            e.t = sp[i].t; e.type = YRSP_PRESS; e.value = 1;
            e.control = (uint32_t)yrsp_scancode(key_names[sp[i].key]);
            yrsp_feed(rsp, &e);
            sp[i].state = 1;
        }
        if (sp[i].state == 1 && sp[i].t + YRT_MS(80) <= now) {
            e.t = sp[i].t + YRT_MS(80); e.type = YRSP_RELEASE;
            e.control = (uint32_t)yrsp_scancode(key_names[sp[i].key]);
            yrsp_feed(rsp, &e);
            sp[i].state = 2;
        }
    }
}

/* Training and criterion presses, from the symbol's flip onset. */
static void sim_queue(int64_t on) {
    int i = tr.ti.index, wrong = (tr.key + 1) % N_STIM;
    if (tr.type == TRAINING && (i == 1 || i == 4)) {
        sim_add(on + YRT_S(0.40), wrong);
        sim_add(on + YRT_S(0.90), tr.key);          /* in the penalty: nothing */
        sim_add(on + YRT_S(1.90), tr.key);
    } else if (tr.type == TRAINING) {
        sim_add(on + YRT_S(SIM_RT), tr.key);
    } else {
        /* criterion: two slips under A; under B mapping A's key on a
         * remapped symbol's first two showings */
        int shown = 0, k, choice = tr.key;
        for (k = 0; k < n_lg; k++) shown += lg[k].stim == tr.stim;
        if (!tr.mapping_b && (i == 2 || i == 7)) choice = wrong;
        if (is_remapped(tr.stim, tr.mapping_b) && shown < 2) choice = key_of(tr.stim, 0);
        sim_add(on + YRT_S(SIM_RT), choice);
    }
}

/* --- sound -------------------------------------------------------------- */

static void start_tone(int k, int64_t t, int sound) {
    tr.tone_id[k] = yau_play_at(&au, snd[sound], t);
    tr.tone_rc[k] = tr.tone_id[k] > 0 ? YAU_PENDING : (int)tr.tone_id[k];
    if (tr.tone_id[k] <= 0) { fprintf(stderr, "trial_timed_response: play: %s\n", yau_strerror((int)tr.tone_id[k])); failed = 1; }
}

/* The audio look-ahead: each tone in [last end, now + the device's lead +
 * two frames) goes to yau_play_at() at its RT time, so it is scheduled
 * before it is due and only once. */
static void schedule_tones(int64_t period) {
    ytl_event pk[16];
    int64_t lead = yau_lead_ns(&au), to;
    int i, n;
    to = yrt_now_ns() + (lead > 0 ? lead : 0) + 2 * period + YRT_MS(5);
    if (to <= peek_from) return;
    n = ytl_peek(&tl, TRIAL, peek_from, to, pk, 16);
    peek_from = to;
    for (i = 0; i < n && i < 16; i++)
        if (pk[i].kind == YTL_MARK && pk[i].code >= TONE_MARK && pk[i].code < TONE_MARK + N_TONES && !tr.tone_id[pk[i].code - TONE_MARK])
            start_tone(pk[i].code - TONE_MARK, pk[i].onset, (int)pk[i].user);
}

static void play_now(int sound) {
    yau_play_desc d;
    yau_id id;
    memset(&d, 0, sizeof d);
    d.buf = snd[sound];
    if ((id = yau_play(&au, &d)) <= 0) fprintf(stderr, "trial_timed_response: play: %s\n", yau_strerror((int)id));
}

/* --- blocks and trials -------------------------------------------------- */

/* Sub-blocks of 20 (each symbol 5 times), blocked, no symbol three times
 * running, across the sub-blocks too. */
static int open_block(const block* b) {
    static ytr_desc d;
    memset(&d, 0, sizeof d);
    d.factors[0] = ytr_factor("stimulus", N_STIM);
    d.factors[1] = ytr_factor("sub_block", b->type == CRITERION ? CRIT_MAX_SUBS : b->trials / SUB_TRIALS);
    d.n_factors = 2;
    d.reps = SUB_TRIALS / N_STIM;
    d.order = YTR_ORDER_CONSTRAINED;
    d.constraints[0] = ytr_max_run(0, YTR_ANY_LEVEL, 2);
    d.n_constraints = 1;
    d.constraints_span_blocks = true;
    d.groups.mode = YTR_GROUPS_BLOCKED;
    d.groups.factor = 1;
    if (b->type == FORCED) { d.jitters[0] = ytr_uniform("pt_s", 0, YRT_S(PT_MAX_S)); d.n_jitters = 1; }
    d.rng = ytr_splitmix;
    d.rng_ctx = &seed;
    ytr_close(&trials);
    if (!ytr_open(&trials, &d)) { fprintf(stderr, "trial_timed_response: %s\n", ytr_error(&trials)); return 0; }
    n_lg = criterion_met = 0;
    memset(streak, 0, sizeof streak);
    return 1;
}

static void end_block(void);

/* The next trial's timeline, window and script; 0 when every block is done. */
static int start_trial(const yscr_frame* f) {
    ytl_seq q;
    int k;
    for (;;) {
        if (!block_open) {
            if (cur + 1 >= n_blocks || !open_block(&blocks[cur + 1])) return 0;
            cur++;
            block_open = 1;
            block_t0 = f->onset;
        }
        if (!criterion_met && n_lg < MAX_BLOCK && ytr_next(&trials, &tr.ti) >= 0) break;
        end_block();
        block_open = 0;
    }
    tr.type = blocks[cur].type;
    tr.mapping_b = blocks[cur].mapping_b;
    tr.stim = ytr_level(&trials, tr.ti.condition, 0);
    tr.key = key_of(tr.stim, tr.mapping_b);
    tr.responding = 1;
    tr.row_due = tr.penalty = tr.n_errors = tr.n_tones = tr.stim_seen = 0;
    tr.first = -1;
    tr.stim_frame = -1;
    tr.anchor = f->onset;
    memset(tr.tone_id, 0, sizeof tr.tone_id);
    for (k = 0; k < N_TONES; k++) tr.tone_rc[k] = YAU_OK;
    lg[n_lg].sim_pt = NAN;
    lg[n_lg].sim_choice = -1;
    sim_reset();
    symbol_item.glyph = (float)tr.stim;
    ytl_clear(&tl, TRIAL);
    ytl_anchor(&tl, TRIAL, f->onset, 0);
    peek_from = f->onset;
    q = ytl_seq_on(&tl, TRIAL);
    yrsp_arm(rsp, f->onset);
    if (tr.type == FORCED) {
        /* four tones on the trial's base; the symbol PT before the fourth */
        yrsp_onset plan;
        tr.pt_planned = ytr_jitter(&trials, tr.ti.index, 0).ns;
        tr.n_tones = N_TONES;
        for (k = 0; k < N_TONES; k++) {
            tr.tone_t[k] = f->onset + YRT_S(LEAD_S) + k * YRT_S(TONE_GAP_S);
            ytl_at(&q, YRT_S(LEAD_S) + k * YRT_S(TONE_GAP_S));
            ytl_mark(&q, TONE_MARK + k, SND_COUNT);
        }
        tr.stim_target = tr.tone_t[N_TONES - 1] - tr.pt_planned;
        ytl_at(&q, tr.stim_target - f->onset);
        ytl_on(&q, STIM_ON);
        /* RT from the symbol, from the first tone (a negative minimum) to
         * 0.8 s after the fourth; the symbol's schedule is the onset until
         * it lands, so a press before it is a response when it comes */
        yrsp_set_window(rsp, tr.tone_t[0] - tr.stim_target, tr.tone_t[N_TONES - 1] + YRT_S(LATE_LIMIT_S) - tr.stim_target);
        memset(&plan, 0, sizeof plan);
        plan.t_ns = tr.stim_target;
        plan.frame = -1;
        plan.src = YRSP_ONSET_PLAN;
        yrsp_set_onset(rsp, &plan);
        if (scripted) sim_add(tr.tone_t[N_TONES - 1] + YRT_S(sim_jitter(tr.ti.index)), -1);
    } else {
        /* training: the symbol with a tone, after the 0.3 s delay;
         * criterion: the symbol after 0.5 s */
        double pre = tr.type == TRAINING ? TRAIN_DELAY_S : CRIT_PRE_S;
        tr.pt_planned = YTR_MISSING_NS;
        tr.stim_target = f->onset + YRT_S(pre);
        ytl_at(&q, YRT_S(pre));
        ytl_on(&q, STIM_ON);
        if (tr.type == TRAINING) {
            tr.n_tones = 1;
            tr.tone_t[0] = tr.stim_target;
            ytl_mark(&q, TONE_MARK, SND_START);
        }
    }
    {
        ygfx_crun_desc d;
        memset(&d, 0, sizeof d);
        d.set = glyphs; d.items = &symbol_item; d.n = 1; d.size = SYMBOL_PX;
        memcpy(d.color, INK, sizeof d.color);
        symbol = ygfx_crun(&gfx, &d);
    }
    if (q.err) { fprintf(stderr, "trial_timed_response: timeline error %d\n", q.err); failed = 1; }
    return 1;
}

/* Feedback from this frame on: the box, the words, then everything off. */
static void feedback(const yscr_frame* f, int choice, int early, int late, double dur) {
    int64_t bt = 0;
    ytl_seq q = ytl_seq_on(&tl, TRIAL);
    ytl_base_time(&tl, TRIAL, f->onset, &bt);
    ytl_at(&q, bt);
    if (choice >= 0) {
        fb.x = box_x(choice);
        memcpy(fb.color, choice == tr.key ? GREEN : RED, sizeof fb.color);
        ytl_on(&q, FB_ON);
    }
    if (early) ytl_on(&q, EARLY_ON);
    if (late) ytl_on(&q, LATE_ON);
    ytl_wait(&q, YRT_S(dur));
    ytl_off(&q, STIM_ON); ytl_off(&q, FB_ON); ytl_off(&q, EARLY_ON); ytl_off(&q, LATE_ON);
    ytl_wait(&q, YRT_S(ITI_S));
    ytl_mark(&q, END_OF_TRIAL, 0);
    if (q.err) { fprintf(stderr, "trial_timed_response: timeline error %d\n", q.err); failed = 1; }
}

/* The window ended: a response, or the forced deadline. */
static void on_response(const yscr_frame* f) {
    int choice;
    yrsp_finish(rsp, &tr.res);
    choice = (tr.res.flags & YRSP_R_RESPONDED) ? tr.res.response : -1;
    if (tr.type == FORCED) {
        /* the screen's verdict from the plan: the record may not be in */
        int64_t err = tr.res.t_response - tr.tone_t[N_TONES - 1];
        feedback(f, choice, choice >= 0 && err < -YRT_S(TIMING_TOL_S), choice < 0 || err > YRT_S(TIMING_TOL_S), FEEDBACK_S);
    } else if (tr.type == CRITERION) {
        feedback(f, choice, 0, 0, CRIT_FEEDBACK_S);
    } else if (choice == tr.key) {
        int64_t bt = 0;
        ytl_seq q = ytl_seq_on(&tl, TRIAL);
        if (tr.first < 0) tr.first = choice;
        play_now(SND_GOOD);
        ytl_base_time(&tl, TRIAL, f->onset, &bt);
        ytl_at(&q, bt);
        ytl_off(&q, STIM_ON);
        ytl_mark(&q, END_OF_TRIAL, 0);   /* the next trial's 0.3 s is the delay */
    } else {
        if (tr.first < 0) tr.first = choice;
        tr.n_errors++;
        play_now(SND_BUZZ);
        tr.penalty = 1;
        tr.penalty_end = tr.res.t_response + YRT_S(PENALTY_S);
    }
}

static void write_row(void) {
    row r;
    logged* l = &lg[n_lg];
    int k, choice, tier = 0, flags = 0;
    int64_t t_key, res_max = 0;
    yrsp_finish(rsp, &tr.res);           /* again: the final onset, the releases */
    choice = (tr.res.flags & YRSP_R_RESPONDED) ? tr.res.response : -1;
    t_key = choice >= 0 ? tr.res.t_response : 0;
    memset(&r, 0, sizeof r);
    r.block = cur;
    r.block_type = type_names[tr.type];
    r.mapping = tr.mapping_b ? "B" : "A";
    r.trial = tr.ti.index;
    r.sub_block = ytr_level(&trials, tr.ti.condition, 1);
    r.stimulus = tr.stim;
    r.remapped = is_remapped(tr.stim, tr.mapping_b);
    r.correct_key = key_names[tr.key];
    r.correct = tr.type == TRAINING ? tr.n_errors == 0 : choice == tr.key;
    r.habitual = r.remapped && choice >= 0 ? choice == key_of(tr.stim, 0) : YTR_MISSING_INT;
    r.first_response = tr.type == TRAINING && tr.first >= 0 ? key_names[tr.first] : NULL;
    r.n_errors = tr.type == TRAINING ? tr.n_errors : YTR_MISSING_INT;
    r.pt_planned_ns = tr.pt_planned;
    r.pt_ns = tr.type == FORCED && t_key ? tr.res.rt_ns : YTR_MISSING_NS;
    r.symbol_rt_ns = tr.type != FORCED && t_key ? tr.res.rt_ns : YTR_MISSING_NS;
    r.timing_error_ns = tr.type == FORCED && t_key ? t_key - tr.tone_rec[N_TONES - 1].onset : YTR_MISSING_NS;
    r.too_early = r.timing_error_ns != YTR_MISSING_NS && r.timing_error_ns < -YRT_S(TIMING_TOL_S);
    r.too_late = tr.type == FORCED && (!t_key || r.timing_error_ns > YRT_S(TIMING_TOL_S));
    r.stim_residual_ns = tr.stim_rec.onset - tr.stim_target;
    r.stim_tier = tr.stim_rec.tier;
    for (k = 0; k < tr.n_tones; k++) {
        int64_t d = tr.tone_rec[k].onset - tr.tone_rec[k].target;
        if (tr.tone_rec[k].tier > tier) tier = tr.tone_rec[k].tier;
        flags |= tr.tone_rec[k].flags;
        if (llabs(d) > llabs(res_max)) res_max = d;
    }
    r.tone_residual_ns = tr.n_tones ? tr.tone_rec[tr.n_tones - 1].onset - tr.tone_rec[tr.n_tones - 1].target : YTR_MISSING_NS;
    r.tone_tier = tr.n_tones ? tier : YTR_MISSING_INT;
    r.tone_flags = tr.n_tones ? flags : YTR_MISSING_INT;
    if (yrsp_data_set(&data, &tr.res) < 0 || ytr_data_write(&data, &r) < 0) {
        fprintf(stderr, "trial_timed_response: %s\n", ytr_data_error(&data));
        failed = 1;
    }
    ytr_update(&trials, choice >= 0 ? r.correct : YTR_INVALID, NULL);

    l->stim = tr.stim; l->choice = choice; l->correct = r.correct;
    l->n_errors = tr.n_errors; l->too_early = r.too_early; l->too_late = r.too_late;
    l->pt = r.pt_ns; l->rt = r.symbol_rt_ns; l->err = r.timing_error_ns; l->pt_planned = tr.pt_planned;
    l->stim_res = r.stim_residual_ns; l->tone_res_max = res_max;
    /* forced: the tones' spacing against 1.2 s; training: the start tone
     * against the symbol's flip, both planned for one time */
    l->tone_gap_err = tr.n_tones == N_TONES ? tr.tone_rec[3].onset - tr.tone_rec[0].onset - 3 * YRT_S(TONE_GAP_S)
                    : tr.n_tones ? tr.tone_rec[0].onset - tr.stim_rec.onset : 0;
    n_lg++;
    rows++;
    if (tr.type == CRITERION && choice >= 0) {
        int all = 1;
        streak[tr.stim] = r.correct ? streak[tr.stim] + 1 : 0;
        for (k = 0; k < N_STIM; k++) all &= streak[k] >= CRIT_STREAK;
        criterion_met = all;
    }
    if (!sim)
        printf("%-9s %c %3d  symbol %d key %s  %-4s %-7s", r.block_type, *r.mapping, r.trial, tr.stim, r.correct_key,
               choice >= 0 ? key_names[choice] : "-", r.correct ? "correct" : "wrong");
    if (!sim && tr.type == FORCED)
        printf("  pt %.3f (planned %.3f) err %+.3f s%s\n", r.pt_ns == YTR_MISSING_NS ? NAN : YRT_TO_S(r.pt_ns),
               YRT_TO_S(tr.pt_planned), r.timing_error_ns == YTR_MISSING_NS ? NAN : YRT_TO_S(r.timing_error_ns),
               r.too_early ? " too early" : r.too_late ? " too late" : "");
    else if (!sim)
        printf("  rt %.3f s%s\n", r.symbol_rt_ns == YTR_MISSING_NS ? NAN : YRT_TO_S(r.symbol_rt_ns),
               tr.n_errors ? "  (after an error)" : "");
}

/* --- analysis and checks ------------------------------------------------- */

/* p(kind) in the 100 ms window of measured PT centered on c, over the rows
 * that responded and pass the filter (0 all, 1 consistent, 2 remapped);
 * kind 0 correct, 1 habitual. NAN when the window is empty. */
static double window_p(int filter, int kind, double c, int* n, int from_script) {
    int i, hit = 0;
    *n = 0;
    for (i = 0; i < n_lg; i++) {
        const logged* l = &lg[i];
        int rem = is_remapped(l->stim, blocks[cur].mapping_b), ch = from_script ? l->sim_choice : l->choice;
        double pt = from_script && !isnan(l->sim_pt) ? l->sim_pt : (double)l->pt / 1e9;
        if (ch < 0 || l->pt == YTR_MISSING_NS || (filter == 1 && rem) || (filter == 2 && !rem)) continue;
        if (pt < c - 0.05 || pt >= c + 0.05) continue;
        (*n)++;
        hit += kind ? ch == key_of(l->stim, 0) : ch == key_of(l->stim, blocks[cur].mapping_b);
    }
    return *n ? (double)hit / *n : NAN;
}

static int check(int ok, const char* what, int i) {
    if (!ok) { fprintf(stderr, "trial_timed_response: --sim: block %d trial %d: %s\n", cur, i, what); bad = 1; }
    return ok;
}

/* The forced block's curves, and with a script, the checks against it. */
static void forced_report(void) {
    int w, n0, n1, n2, ns, b = blocks[cur].mapping_b;
    printf("block %d forced %c: p(response) in 100 ms windows of measured PT\n", cur, b ? 'B' : 'A');
    printf(b ? "  pt_s    n consistent p_correct    n remapped p_correct p_habitual\n" : "  pt_s    n p_correct\n");
    for (w = 0; w < N_WIN; w++) {
        double c = 0.05 * (w + 1), lo = c - 0.05;
        double p0 = window_p(b ? 1 : 0, 0, c, &n0, 0), p1 = window_p(2, 0, c, &n1, 0), p2 = window_p(2, 1, c, &n2, 0);
        if (b) printf("  %.2f  %3d %22.2f  %3d %18.2f %10.2f\n", c, n0, p0, n1, p1, p2);
        else printf("  %.2f  %3d %11.2f\n", c, n0, p0);
        if (!sim) continue;
        /* the curve the script implies, from the PTs it saw */
        check(window_p(b ? 1 : 0, 0, c, &ns, 1) == p0 || (isnan(p0) && ns == 0), "curve differs from the script's", w);
        if (b) check((window_p(2, 0, c, &ns, 1) == p1 || (isnan(p1) && !ns)) && (window_p(2, 1, c, &ns, 1) == p2 || (isnan(p2) && !ns)),
                     "remapped curves differ from the script's", w);
        if (lo >= SIM_T_A - 1e-9) check(n0 == 0 || p0 == 1.0, "p(correct) below 1 past the step", w);
        if (b && lo >= SIM_T_B - 1e-9) check(n1 == 0 || p1 == 1.0, "remapped p(correct) below 1 past T_B", w);
        if (b && lo >= SIM_T_A - 1e-9 && c + 0.05 <= SIM_T_B + 1e-9) check(n2 == 0 || p2 == 1.0, "p(habitual) below 1 in [T_A, T_B)", w);
    }
}

static void end_block(void) {
    int i, j, s, run = 0, guess = 0, hits = 0;
    double sec = (double)(yrt_now_ns() - block_t0) / 1e9;
    const block* b = &blocks[cur];
    int64_t worst_stim = 0, worst_tone = 0, worst_gap = 0, worst_err = 0;
    if (b->type == FORCED && n_lg) forced_report();
    if (b->type == TRAINING) printf("block %d training %c: %d trials in %.1f s\n", cur, b->mapping_b ? 'B' : 'A', n_lg, sec);
    if (b->type == CRITERION)
        printf("block %d criterion %c: %s after %d trials\n", cur, b->mapping_b ? 'B' : 'A', criterion_met ? "met" : "NOT met", n_lg);
    for (i = 0; i < n_lg; i++) {
        const logged* l = &lg[i];
        if (llabs(l->stim_res) > llabs(worst_stim)) worst_stim = l->stim_res;
        if (llabs(l->tone_res_max) > llabs(worst_tone)) worst_tone = l->tone_res_max;
        if (llabs(l->tone_gap_err) > llabs(worst_gap)) worst_gap = l->tone_gap_err;
        if (scripted && b->type == FORCED && l->choice >= 0 &&
            llabs(l->err - YRT_S(sim_jitter(i))) > llabs(worst_err)) worst_err = l->err - YRT_S(sim_jitter(i));
    }
    printf("  worst: symbol flip minus target %+.3f ms", (double)worst_stim / 1e6);
    if (b->type == FORCED)
        printf(", tone onset minus target %+.4f ms, tone 4 minus tone 1 minus 1.2 s %+.4f ms", (double)worst_tone / 1e6,
               (double)worst_gap / 1e6);
    if (b->type == TRAINING)
        printf(", tone onset minus target %+.4f ms, start tone minus symbol flip %+.4f ms", (double)worst_tone / 1e6,
               (double)worst_gap / 1e6);
    if (scripted && b->type == FORCED) printf(", timing error minus the script's jitter %+.4f ms", (double)worst_err / 1e6);
    printf("\n");
    if (!sim) return;

    /* the order: whole sub-blocks of 5 each, no symbol three times running */
    for (i = 0; i + SUB_TRIALS <= n_lg; i += SUB_TRIALS)
        for (s = 0; s < N_STIM; s++) {
            int c = 0;
            for (j = i; j < i + SUB_TRIALS; j++) c += lg[j].stim == s;
            check(c == SUB_TRIALS / N_STIM, "a sub-block without 5 of each symbol", i);
        }
    for (i = 0; i < n_lg; i++) {
        run = i && lg[i].stim == lg[i - 1].stim ? run + 1 : 1;
        check(run <= 2, "a symbol three times running", i);
    }
    if (b->type == FORCED) {
        check(n_lg == b->trials, "rows missing", n_lg);
        for (i = 0; i < n_lg; i++) {
            const logged* l = &lg[i];
            double jit = sim_jitter(i);
            /* the fit on the virtual clock is exact: each onset within half a
             * sample (48 kHz) of its plan; the symbol on the nearest frame */
            check(l->choice >= 0 && l->choice == l->sim_choice, "the response is not the script's", i);
            check(llabs(l->tone_res_max) <= 1000000000 / 48000 / 2 && llabs(l->tone_gap_err) <= 1000000000 / 48000, "a tone off its plan", i);
            check(llabs(l->err - YRT_S(jit)) <= 1000000000 / 48000 / 2, "timing error is not the script's jitter", i);
            check(l->too_early == (jit < -TIMING_TOL_S) && l->too_late == (jit > TIMING_TOL_S), "too early / too late", i);
            check(llabs(l->stim_res) <= frame_period / 2, "the symbol is not on the frame nearest its time", i);
            check(llabs(l->pt - l->pt_planned - YRT_S(jit)) <= frame_period / 2 + 1000000000 / 48000,
                  "measured PT is off the plan plus the jitter by more than half a frame", i);
            check(isnan(l->sim_pt) ? l->pt <= 0 : l->pt == (int64_t)llround(l->sim_pt * 1e9), "measured PT differs from the PT seen", i);
            if (!isnan(l->sim_pt) && l->sim_pt < SIM_T_A) { guess++; hits += l->correct; }
        }
        /* a quarter of the guesses hit, within 4 binomial SDs: 4 keys */
        check(guess >= 10 && fabs(hits - guess / 4.0) <= 4 * sqrt(guess * 3.0 / 16), "guess rate far from 1/4", guess);
    } else if (b->type == TRAINING) {
        check(n_lg == b->trials, "rows missing", n_lg);
        for (i = 0; i < n_lg; i++) {
            int err = i == 1 || i == 4;
            check(lg[i].n_errors == err && lg[i].correct == !err && lg[i].choice == key_of(lg[i].stim, b->mapping_b) &&
                  lg[i].rt == YRT_S(err ? 1.90 : SIM_RT), "training row", i);
            check(llabs(lg[i].tone_gap_err) <= 1000000000 / 48000 / 2 + llabs(lg[i].stim_res), "start tone off the symbol's flip", i);
        }
    } else {
        /* the stopping rule again: the first n at which each symbol's last
         * five showings were correct */
        int n, stop = -1;
        for (n = 1; n <= n_lg && stop < 0; n++) {
            int ok = 1;
            for (s = 0; s < N_STIM; s++) {
                int seen = 0;
                for (j = n - 1; j >= 0 && seen < CRIT_STREAK; j--)
                    if (lg[j].stim == s) { if (!lg[j].correct) break; seen++; }
                ok &= seen == CRIT_STREAK;
            }
            if (ok) stop = n;
        }
        check(criterion_met && stop == n_lg && n_lg >= SUB_TRIALS, "criterion stopped at the wrong trial", n_lg);
    }
}

/* --- main --------------------------------------------------------------- */

static int parse_blocks(const char* s, int mapping_b, int n_trials) {
    char buf[256], *tok;
    snprintf(buf, sizeof buf, "%s", s);
    for (tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
        char* colon = strchr(tok, ':');
        block* b;
        if (n_blocks == 16) return 0;
        b = &blocks[n_blocks];
        b->mapping_b = mapping_b;
        b->trials = n_trials;
        if (colon) {
            *colon = 0;
            if (strcmp(colon + 1, "A") && strcmp(colon + 1, "B")) return 0;
            b->mapping_b = colon[1] == 'B';
        }
        for (b->type = 0; b->type < 3 && strcmp(tok, type_names[b->type]); b->type++) {}
        if (b->type == 3) return 0;
        n_blocks++;
    }
    return n_blocks > 0;
}

static yau_buf tone(double hz, double dur, float db) {
    yau_tone_desc t;
    memset(&t, 0, sizeof t);
    t.hz = hz; t.dur_ns = YRT_S(dur); t.peak = yau_db(db); t.ramp_ns = YRT_MS(5);
    return yau_tone(&au, &t);
}

int main(int argc, char** argv) {
    yau_desc ad;
    yscr_frame f;
    ytl_event fired[64];
    yrsp_desc pd;
    char line[1024], aline[512], meta[4096], meta_path[1100], what[600];
    const char *out_path = "trial_timed_response.csv", *blocks_arg = "forced", *font_path = SYMBOL_FONT, *text_font = TEXT_FONT;
    int i, k, n, fullscreen = 0, aborted = 0, mapping_b = 0, n_trials = 100, pulse = 0;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--sim")) sim = scripted = 1;
        else if (!strcmp(argv[i], "--auto")) scripted = 1;
        else if (!strcmp(argv[i], "--pulse")) pulse = 1;
        else if (!strcmp(argv[i], "--fullscreen")) fullscreen = 1;
        else if (!strcmp(argv[i], "--blocks") && i + 1 < argc) blocks_arg = argv[++i];
        else if (!strcmp(argv[i], "--mapping") && i + 1 < argc && (!strcmp(argv[i + 1], "A") || !strcmp(argv[i + 1], "B")))
            mapping_b = argv[++i][0] == 'B';
        else if (!strcmp(argv[i], "--trials") && i + 1 < argc) n_trials = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) seed = (uint64_t)strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out_path = argv[++i];
        else if (!strcmp(argv[i], "--font") && i + 1 < argc) font_path = argv[++i];
        else if (!strcmp(argv[i], "--text-font") && i + 1 < argc) text_font = argv[++i];
        else {
            fprintf(stderr, "usage: trial_timed_response [--sim | --auto] [--fullscreen] [--pulse] [--blocks LIST] [--mapping A|B] "
                            "[--trials N] [--seed N] [--out FILE] [--font PATH] [--text-font PATH]\n");
            return 2;
        }
    }
    if (n_trials < SUB_TRIALS || n_trials > MAX_BLOCK || n_trials % SUB_TRIALS) {
        fprintf(stderr, "trial_timed_response: --trials takes a multiple of 20 up to %d\n", MAX_BLOCK);
        return 2;
    }
    if (sim) {
        static const block script[] = { { CRITERION, 0, 0 }, { TRAINING, 0, 20 }, { FORCED, 0, 100 },
                                        { CRITERION, 1, 0 }, { FORCED, 1, 100 } };
        memcpy(blocks, script, sizeof script);
        n_blocks = 5;
    } else if (!parse_blocks(blocks_arg, mapping_b, n_trials)) {
        fprintf(stderr, "trial_timed_response: --blocks takes forced, training, criterion, each with :A or :B\n");
        return 2;
    }
    /* --sim runs on ysp/rt.h's virtual clock: the same records on any host */
    if (sim) yrt_virtual_begin(0);

    if (!ygfx_open_screen(&gfx, &scr, &(yscr_desc){ .backend = sim ? YSCR_BACKEND_SIM : 0, .windowed = !fullscreen },
                          &(ygfx_desc){ .background = { 0.1f, 0.1f, 0.1f } })) {
        fprintf(stderr, "trial_timed_response: %s\n", ygfx_error(&gfx));
        return 1;
    }
    if (!yol_open(&ol, NULL) || make_glyphs(font_path, text_font, what, sizeof what) < 0) {
        fprintf(stderr, "trial_timed_response: glyphs: %s %s\n", yol_error(&ol), ygfx_error(&gfx));
        return 1;
    }
    for (k = 0; k < N_STIM; k++) boxes[k] = rect(box_x(k), BOX_Y, BOX_PX, 4, INK);
    fb = rect(0, BOX_Y, BOX_PX - 12, 0, GREEN);
    word_early = word(early_items, "too early", -150);
    word_late = word(late_items, "too late", -150);
    symbol_item.scale = symbol_item.gate = 1;

    memset(&ad, 0, sizeof ad);
    ad.backend = sim ? YAU_BACKEND_NULL : pulse ? YAU_BACKEND_PULSE : YAU_BACKEND_AUTO;
    if (pulse) ad.format.sample = YAU_S16;   /* a desktop sink is often 16-bit; open() converts nothing */
    ad.arena_bytes = 1 << 20;
    if (!yau_open(&au, &ad)) { fprintf(stderr, "trial_timed_response: %s\n", yau_error(&au)); return 1; }
    snd[SND_COUNT] = tone(1000, 0.1, -30);
    snd[SND_START] = tone(750, 0.1, -30);
    snd[SND_GOOD] = tone(1320, 0.15, -30);
    snd[SND_BUZZ] = yau_alloc(&au, YRT_MS(300) * 48000 / YRT_S(1), 1);
    for (k = 0; k < (int)snd[SND_BUZZ].n; k++)   /* a 150 Hz square wave: a buzzer */
        snd[SND_BUZZ].frames[k] = (k * 150 / 24000) % 2 ? -yau_db(-36) : yau_db(-36);
    for (k = 0; k < N_SND; k++)
        if (!snd[k].frames) { fprintf(stderr, "trial_timed_response: %s\n", yau_error(&au)); return 1; }

    if (!ytl_open(&tl, &(ytl_desc){ .events = storage, .event_capacity = 64, .n_channels = N_CHANNELS })) {
        fprintf(stderr, "trial_timed_response: %s\n", ytl_error(&tl));
        return 1;
    }
    /* from the symbol, no deadline; a forced trial sets its own window */
    memset(&pd, 0, sizeof pd);
    pd.choices = keys;
    pd.n_choices = N_STIM;
    if (!yrsp_open(rsp, &pd)) { fprintf(stderr, "trial_timed_response: %s\n", yrsp_error(rsp)); return 1; }

    yscr_describe(&scr, line, sizeof line);
    yau_describe(&au, aline, sizeof aline);
    snprintf(meta, sizeof meta, "screen=%s\naudio=%s\nysp_response=%s\nkeyboard=%s\nglyphs=%s\nkeys=j k l ;\n"
             "mapping_b=swaps the keys of symbols %d and %d\nblocks=%s\nseed=%llu\n", line, aline, yrsp_version(),
             scripted ? "scripted participant (tier SIM)" : "SDL through yscr_event_input", what, REMAP_1, REMAP_2,
             sim ? "criterion:A,training:A(20),forced:A,criterion:B,forced:B" : blocks_arg, (unsigned long long)seed);
    snprintf(meta_path, sizeof meta_path, "%s.meta", out_path);
    if (!ytr_data_open(&data, &(ytr_data_desc){ .path = out_path, .cols = cols, .n_cols = (int)(sizeof cols / sizeof cols[0]),
                                                .meta_path = meta_path, .meta = meta })) {
        fprintf(stderr, "trial_timed_response: %s\n", ytr_data_error(&data));
        return 1;
    }
    printf("%s\n%s\n%s\n", line, aline, what);
    memset(&tr, 0, sizeof tr);

    for (;;) {
        int rc = yscr_begin(&scr, &f);
        if (rc == YSCR_QUIT) { aborted = 1; break; }
        if (rc != YSCR_OK) { fprintf(stderr, "trial_timed_response: %s\n", yscr_error(&scr)); failed = 1; break; }
        frame_period = f.period;
        if (!in_trial && !start_trial(&f)) break;
        in_trial = 1;
        if (sim && f.onset - tr.anchor > YRT_S(30)) {   /* a script that never ends a trial */
            fprintf(stderr, "trial_timed_response: --sim: block %d trial %d did not end\n", cur, tr.ti.index);
            failed = 1;
            break;
        }

        /* the symbol's flip record; the script's presses from it */
        for (i = 0; tr.stim_frame >= 0 && !tr.stim_seen && i < f.n_done; i++)
            if (f.done[i].index == tr.stim_frame && yrsp_onset_flip(&f.done[i]).final) {
                tr.stim_rec = f.done[i];
                tr.stim_seen = 1;
                if (scripted && tr.type != FORCED) sim_queue(tr.stim_rec.onset);
            }
        /* the tones' records */
        for (k = 0; k < tr.n_tones; k++)
            if (tr.tone_id[k] > 0 && tr.tone_rc[k] == YAU_PENDING) {
                tr.tone_rc[k] = yau_result(&au, tr.tone_id[k], &tr.tone_rec[k]);
                if (tr.tone_rc[k] != YAU_OK && tr.tone_rc[k] != YAU_PENDING) {
                    fprintf(stderr, "trial_timed_response: tone record: %s\n", yau_strerror(tr.tone_rc[k]));
                    failed = 1;
                }
            }
        schedule_tones(f.period);
        if (failed) break;

        /* training: the penalty is over, a new window on the same onset */
        if (tr.penalty && f.onset >= tr.penalty_end) {
            yrsp_onset o = yrsp_onset_flip(&tr.stim_rec);
            yrsp_arm(rsp, tr.penalty_end);
            yrsp_set_onset(rsp, &o);
            tr.penalty = 0;
            tr.responding = 1;
        }
        yrsp_feed_screen(&scr, rsp, 1);
        if (scripted) sim_feed(f.onset);
        if (yrsp_update_frame(rsp, &f) == YRSP_ENDED && tr.responding && !tr.penalty) {
            tr.responding = 0;
            on_response(&f);
        }

        n = ytl_evaluate(&tl, &(ytl_frame){ f.onset, f.period, f.index }, fired, 64);
        for (i = 0; i < n; i++) {
            const ytl_event* e = &fired[i];
            if (e->kind == YTL_ONSET && e->target == STIM_ON && tr.stim_frame < 0) {
                tr.stim_frame = e->frame;
                tr.stim_land = e->onset;
                yrsp_set_landing(rsp, e, 1, STIM_ON);
            }
            /* a tone the look-ahead missed: late, and its record says so */
            if (e->kind == YTL_MARK && e->code >= TONE_MARK && e->code < TONE_MARK + N_TONES && !tr.tone_id[e->code - TONE_MARK])
                start_tone(e->code - TONE_MARK, e->onset - e->residual, (int)e->user);
            if (e->kind == YTL_MARK && e->code == END_OF_TRIAL) tr.row_due = 1;
        }
        /* the trial is over and its records are in: the row */
        if (tr.row_due) {
            int pending = !tr.stim_seen;
            for (k = 0; k < tr.n_tones; k++) pending |= tr.tone_rc[k] == YAU_PENDING;
            if (!pending) { write_row(); in_trial = 0; }
        }
        ygfx_apply(binds, (int)(sizeof binds / sizeof binds[0]), ytl_values(&tl));
        yscr_mark(&scr, YSCR_PHASE_EVALUATE);
        ygfx_begin(&gfx, &f);
        for (k = 0; k < N_STIM; k++) ygfx_draw(&gfx, &boxes[k]);
        ygfx_draw(&gfx, &fb);
        ygfx_draw(&gfx, &symbol);
        ygfx_draw(&gfx, &word_early);
        ygfx_draw(&gfx, &word_late);
        ygfx_end(&gfx);
        yscr_flip(&scr);
    }
    {   /* the describe line again: its counters (key times, raw mice) are 0 at open */
        char end[1100];
        int mrc;
        memcpy(end, "screen_end=", 11);
        yscr_describe(&scr, end + 11, sizeof end - 11);
        if ((mrc = ytr_data_meta(&data, end)) < 0 && mrc != YTR_ERR_CLOSED) fprintf(stderr, "%s\n", ytr_data_error(&data));
    }
    if (ytr_data_finish(&data) < 0) { fprintf(stderr, "trial_timed_response: %s\n", ytr_data_error(&data)); failed = 1; }
    yau_describe(&au, aline, sizeof aline);
    printf("%s\n", aline);
    yau_close(&au);
    ygfx_close(&gfx);
    yscr_close(&scr);
    yol_close(&ol);
    printf("%d trials written to %s%s\n", rows, out_path, aborted ? " (stopped with Shift+Esc)" : "");
    if (sim && !failed && !bad && cur == n_blocks - 1) printf("--sim: every block came out as scripted\n");
    else if (sim && !failed) { fprintf(stderr, "trial_timed_response: --sim: checks failed\n"); return 1; }
    return failed;
}
