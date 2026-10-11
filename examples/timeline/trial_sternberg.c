/* trial_sternberg.c - a memory-scanning trial (Sternberg 1966) as a small
 * experiment: ysp/timeline.h sequences as op tables, digits from
 * ysp/outline.h curve sets drawn by ysp/gfx.h, ysp/trials.h and
 * ysp/response.h.
 *
 * Mirrors PsychoPy's Builder demo Experiments/sternberg/ (a memory set,
 * then a probe: present or absent) and jsPsych's animation plugin (a timed
 * sequence of frames). Written from their descriptions in docs/rig_spec.md
 * 15; no PsychoPy code (GPL-3) was copied.
 *
 * Each trial: a fixation cross for 0.5 s, then the memory set: 2, 4 or 6
 * digits, one at a time at the center, each for 0.3 s with 0.2 s of blank
 * after it (a stimulus onset asynchrony of 0.5 s); a retention interval of
 * 1 s after the last digit; then the probe digit in amber. F if the probe
 * was in the set, J if not, within 2 s of the probe; responses before
 * 0.1 s are anticipations, and presses before the probe are EARLY. A blank
 * inter-trial interval of 0.6 s follows the response.
 *
 * The trial is one op table (ytl_run()), built per trial and checked as a
 * whole before any event goes in: a table that does not fit adds nothing.
 * Each digit's onset comes from its flip record, so the row has the SOAs
 * as shown (soa_2 .. soa_6: a digit's onset minus the one before it) and
 * probe_soa (the probe's onset minus the last digit's, planned 1.3 s). RT
 * runs from the probe's flip onset.
 *
 * Digits: Arial (Windows) or DejaVu Sans (Linux), or --font; a 5 x 7
 * bitmap font made into curves when the file does not open, as on a CI
 * runner. One curve set of 10 glyphs, made at setup.
 *
 * The data file has one row per trial: trial_index, set_size, memory_set,
 * probe, probe_present, answer, response, rt, correct (jsPsych-style names
 * first); then soa_2 .. soa_6, probe_soa, items_tier (the worst tier of
 * the digits' onsets), onset_tier, onset_src, rsp_tier, rsp_flags,
 * n_early, n_anticipations. Times in seconds. Shift+Esc stops the session;
 * the rows so far are kept.
 *
 * --sim runs the 6 conditions once on the simulated display with a
 * synthetic participant whose RT grows by 38 ms per digit, 50 ms more for
 * an absent probe: a press during the memory set and then a response, a
 * wrong key, and a timeout among correct responses. It checks its own
 * results and exits 1 on a mismatch. About 27 s.
 *
 * Usage: trial_sternberg [--sim] [--fullscreen] [--reps N] [--seed N] [--out FILE] [--font PATH]
 *   --reps N   repetitions of each of the 6 conditions (default 4; --sim: 1)
 * Exit code: 0 (also after Shift+Esc), 1 when something did not open or a
 * --sim check failed, 2 for a bad argument.
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#define YSP_GFX_IMPLEMENTATION
#include "ysp/gfx.h"
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

#define MAX_SET 6
enum { FIX_ON, ITEM0, PROBE_ON = ITEM0 + MAX_SET, N_CHANNELS };
enum { TRIAL = 1 };
enum { END_OF_TRIAL = 1 };

#define FIX_S       0.5
#define ITEM_S      0.3
#define GAP_S       0.2
#define RETENTION_S 1.0
#define RESPONSE_S  2.0
#define MIN_RT_S    0.1
#define ITI_S       0.6
#define DIGIT_PX    96.0f

#ifdef _WIN32
#define DEFAULT_FONT "C:/Windows/Fonts/arial.ttf"
#else
#define DEFAULT_FONT "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
#endif

static const char conditions_csv[] =
    "set_size,probe,answer\n"
    "2,present,f\n"
    "2,absent,j\n"
    "4,present,f\n"
    "4,absent,j\n"
    "6,present,f\n"
    "6,absent,j\n";

static const yrsp_choice keys[] = { { .key = "f", .name = "present" }, { .key = "j", .name = "absent" } };

/* The --sim participant. early > 0: a press at that many s from the
 * trial's start (during the memory set); else a response at sim_rt() from
 * the probe's onset. which: 1 the correct key, 2 the other one. */
typedef struct sim_press { int trial; double early; int which; } sim_press;
static const sim_press sim_script[] = {
    { 0, 0, 1 },
    { 1, 0.9, 1 },   /* during the memory set: EARLY */
    { 1, 0, 1 },
    { 2, 0, 2 },     /* the wrong key */
    { 3, 0, 1 },
    /* trial 4: no press, a timeout */
    { 5, 0, 1 },
};
#define SIM_TRIALS 6
static double sim_rt(int set_size, int present) { return 0.40 + 0.038 * set_size + (present ? 0.0 : 0.05); }

/* The 5 x 7 digits of gfx_gallery.c's bitmap font, rows top first, bit 4
 * the left column. */
static const uint8_t digits5x7[10][7] = {
    { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E }, { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E },
    { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F }, { 0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E },
    { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 }, { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E },
    { 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E }, { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 },
    { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E }, { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C },
};

static const float INK[3] = { 0.8f, 0.8f, 0.8f }, AMBER[3] = { 0.75f, 0.42f, 0.05f };

static yscr_screen scr;
static ygfx_gfx gfx;
static yol_ctx ol;
static ygfx_cset digits;
static ytl_timeline tl;
static ytl_event storage[128];
static ytr_trials trials;
static yrsp_collector rsp;
static uint64_t table_arena[2048];
static ytb_table tab;

static ygfx_stim fix, item[MAX_SET], probe;
static ygfx_citem item_glyph[MAX_SET], probe_glyph;
static const ygfx_bind binds[] = {
    { .stim = &fix, .param = YGFX_P_VISIBLE, .channel = FIX_ON },
    { .stim = &item[0], .param = YGFX_P_VISIBLE, .channel = ITEM0 + 0 },
    { .stim = &item[1], .param = YGFX_P_VISIBLE, .channel = ITEM0 + 1 },
    { .stim = &item[2], .param = YGFX_P_VISIBLE, .channel = ITEM0 + 2 },
    { .stim = &item[3], .param = YGFX_P_VISIBLE, .channel = ITEM0 + 3 },
    { .stim = &item[4], .param = YGFX_P_VISIBLE, .channel = ITEM0 + 4 },
    { .stim = &item[5], .param = YGFX_P_VISIBLE, .channel = ITEM0 + 5 },
    { .stim = &probe, .param = YGFX_P_VISIBLE, .channel = PROBE_ON },
};

/* Ten glyphs, ids 0 to 9, in em with the pen at 0, 0 and y down: from the
 * font file when it opens, else from the bitmap. Returns 1 for the file, 0
 * for the bitmap (why says why), -1 on a failure. */
static int make_digits(const char* path, char* why, size_t cap) {
    yol_cset s;
    yol_cset_desc od;
    yol_path p;
    yol_font font;
    ygfx_cset_desc d;
    uint8_t* bytes = NULL;
    long n = 0;
    int real = 0, rc = 0, i, r, c;
    FILE* fp = fopen(path, "rb");
    why[0] = 0;
    if (fp) {
        fseek(fp, 0, SEEK_END); n = ftell(fp); fseek(fp, 0, SEEK_SET);
        bytes = n > 0 ? (uint8_t*)malloc((size_t)n) : NULL;
        if (bytes && fread(bytes, 1, (size_t)n, fp) != (size_t)n) { free(bytes); bytes = NULL; }
        fclose(fp);
    }
    if (bytes && yol_font_view(&font, bytes, (size_t)n, 0, why, cap) == YOL_OK) real = 1;
    else if (!why[0]) snprintf(why, cap, "%s could not be read", path);
    memset(&od, 0, sizeof od);
    od.n_glyphs = 10;
    if (!yol_cset_open(&s, &ol, &od)) { free(bytes); return -1; }
    yol_path_init(&p, &ol);
    for (i = 0; i < 10 && rc == YOL_OK; i++) {
        yol_path_clear(&p);
        if (real) {
            yol_glyph_desc gd;
            memset(&gd, 0, sizeof gd);
            rc = yol_font_glyph(&ol, &font, yol_font_glyph_index(&font, (uint32_t)('0' + i)), &p, &gd);
        } else {
            for (r = 0; r < 7; r++)
                for (c = 0; c < 5; c++)
                    if (digits5x7[i][r] & (0x10 >> c)) yol_rect(&p, 0.1 * c, 0.1 * (r - 7), 0.1, 0.1, 0, 0);
            rc = yol_path_end(&p);
        }
        if (rc == YOL_OK) rc = yol_cset_add(&s, (uint32_t)i, &p);
    }
    memset(&d, 0, sizeof d);
    d.texels = s.texels; d.n_texels = s.n_texels; d.words = s.words; d.n_words = s.n_words;
    if (rc == YOL_OK) digits = ygfx_cset_make(&gfx, &d);
    yol_path_free(&p);
    yol_cset_close(&s);
    free(bytes);   /* the set holds the curves; the font is not read again */
    return rc == YOL_OK && digits.id ? real : -1;
}

/* One digit as a curve run of one item. w, h 0: the box is the glyph's
 * ink, centered on the screen, so 1 and 8 sit on the same center. */
static ygfx_stim digit_run(ygfx_citem* it, int digit, const float* color) {
    ygfx_crun_desc d;
    memset(it, 0, sizeof *it);
    it->glyph = (float)digit; it->scale = 1; it->gate = 1;
    memset(&d, 0, sizeof d);
    d.set = digits; d.items = it; d.n = 1; d.size = DIGIT_PX;
    d.color[0] = color[0]; d.color[1] = color[1]; d.color[2] = color[2];
    return ygfx_crun(&gfx, &d);
}

static void sec(char* out, size_t cap, double s) {
    if (s != s) out[0] = 0;
    else snprintf(out, cap, "%.9f", s);
}

int main(int argc, char** argv) {
    yscr_desc sd;
    ygfx_desc gd;
    ygfx_shape_desc fd;
    ytl_desc td;
    ytb_csv_desc cd;
    ytr_desc rd;
    yrsp_desc pd;
    yscr_frame f;
    ytl_event fired[16];
    ytl_op ops[3 + 4 * MAX_SET + 1];
    ytr_trial_info ti = { 0 };
    yrsp_result res, sim_res[SIM_TRIALS];
    yrsp_input sim_ev[8];
    double sim_soa[SIM_TRIALS][MAX_SET + 1];
    int sim_k[SIM_TRIALS], sim_present[SIM_TRIALS], sim_correct[SIM_TRIALS];
    int64_t item_frame[MAX_SET], item_t[MAX_SET], probe_frame = -1, t_start = 0;
    uint8_t item_tier[MAX_SET];
    int set[10], k = 0, present = 0, probe_digit = 0, n_ops;
    FILE* out;
    char line[1024], meta[2048], why[300];
    const char* out_path = "trial_sternberg.csv";
    const char* font_path = DEFAULT_FONT;
    uint64_t seed = 20261009, digit_rng;
    int i, n, sim = 0, reps = 0, fullscreen = 0, aborted = 0, failed = 0, rows = 0, font_real;
    int in_trial = 0, responding = 0, n_sim_ev = 0, sim_next = 0, size_col, probe_col, answer_col;
    double sum_k = 0, sum_rt = 0, sum_kk = 0, sum_krt = 0;
    int n_fit = 0;
    char answer[8] = "";

    memset(&sd, 0, sizeof sd);
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--sim")) sim = 1;
        else if (!strcmp(argv[i], "--fullscreen")) fullscreen = 1;
        else if (!strcmp(argv[i], "--reps") && i + 1 < argc) reps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) seed = (uint64_t)strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out_path = argv[++i];
        else if (!strcmp(argv[i], "--font") && i + 1 < argc) font_path = argv[++i];
        else {
            fprintf(stderr, "usage: trial_sternberg [--sim] [--fullscreen] [--reps N] [--seed N] [--out FILE] [--font PATH]\n");
            return 2;
        }
    }
    if (reps == 0) reps = sim ? 1 : 4;
    if (reps < 1 || reps > 100) { fprintf(stderr, "trial_sternberg: --reps takes 1..100\n"); return 2; }
    digit_rng = seed ^ 0x9E3779B97F4A7C15ull;   /* the digits' own stream: the order's draws stay ytr's */
    if (sim) sd.backend = YSCR_BACKEND_SIM;
    sd.windowed = !fullscreen;

    if (!yscr_open(&scr, &sd)) { fprintf(stderr, "trial_sternberg: %s\n", yscr_error(&scr)); return 1; }
    memset(&gd, 0, sizeof gd);
    gd.screen = &scr;
    gd.background[0] = gd.background[1] = gd.background[2] = 0.2f;
    if (!ygfx_open(&gfx, &gd)) { fprintf(stderr, "trial_sternberg: %s\n", ygfx_error(&gfx)); return 1; }
    if (!yol_open(&ol, NULL)) { fprintf(stderr, "trial_sternberg: outline context\n"); return 1; }
    font_real = make_digits(font_path, why, sizeof why);
    if (font_real < 0) { fprintf(stderr, "trial_sternberg: digits: %s %s\n", yol_error(&ol), ygfx_error(&gfx)); return 1; }
    if (!font_real) printf("%s: the 5 x 7 bitmap digits instead\n", why);
    memset(&fd, 0, sizeof fd);
    fd.shape = YGFX_CROSS; fd.w = fd.h = 24; fd.shape_p[0] = 3;
    fd.edge = YGFX_EDGE_COSINE; fd.edge_width = 1;
    fd.color[0] = fd.color[1] = fd.color[2] = 0.6f;
    fix = ygfx_shape(&fd);

    memset(&cd, 0, sizeof cd);
    cd.text = conditions_csv;
    cd.len = sizeof conditions_csv - 1;
    cd.arena = table_arena;
    cd.arena_size = sizeof table_arena;
    if (!ytb_open_csv(&tab, &cd)) { fprintf(stderr, "trial_sternberg: %s\n", ytb_error(&tab)); return 1; }
    size_col = ytb_col(&tab, "set_size");
    probe_col = ytb_col(&tab, "probe");
    answer_col = ytb_col(&tab, "answer");
    memset(&rd, 0, sizeof rd);
    rd.table = &tab;
    rd.reps = reps;
    rd.order = YTR_ORDER_RANDOM;
    rd.rng = ytr_splitmix;
    rd.rng_ctx = &seed;
    if (!ytr_open(&trials, &rd)) { fprintf(stderr, "trial_sternberg: %s\n", ytr_error(&trials)); return 1; }

    memset(&td, 0, sizeof td);
    td.events = storage;
    td.event_capacity = 128;
    td.n_channels = N_CHANNELS;
    if (!ytl_open(&tl, &td)) { fprintf(stderr, "trial_sternberg: %s\n", ytl_error(&tl)); return 1; }

    memset(&pd, 0, sizeof pd);
    pd.choices = keys;
    pd.n_choices = 2;
    pd.duration_ns = YRT_S(RESPONSE_S);
    pd.min_rt_ns = YRT_S(MIN_RT_S);
    if (!yrsp_open(&rsp, &pd)) { fprintf(stderr, "trial_sternberg: %s\n", yrsp_error(&rsp)); return 1; }

    out = fopen(out_path, "w");
    if (!out) { fprintf(stderr, "trial_sternberg: cannot write %s\n", out_path); return 1; }
    ytr_format_meta(&trials, meta, sizeof meta);
    meta[strcspn(meta, "\r\n")] = 0;
    yscr_describe(&scr, line, sizeof line);
    fprintf(out, "# %s\n# %s\n# ysp_response %s; keyboard: %s; digits: %s; seed %llu\n", meta, line, yrsp_version(),
            sim ? "synthetic (tier SIM)" : "SDL through yscr_event_input", font_real ? font_path : "5 x 7 bitmap",
            (unsigned long long)seed);
    fprintf(out, "trial_index,set_size,memory_set,probe,probe_present,answer,response,rt,correct,"
                 "soa_2,soa_3,soa_4,soa_5,soa_6,probe_soa,items_tier,onset_tier,onset_src,rsp_tier,rsp_flags,"
                 "n_early,n_anticipations\n");
    printf("%s\n", line);
    probe = digit_run(&probe_glyph, 0, AMBER);

    ygfx_prime(&gfx);   /* each kind's first draw now, not on a trial's first frames (docs/gfx.md) */
    for (;;) {
        SDL_Event ev;
        int64_t bt = 0;
        yrsp_input in;
        int rc = yscr_begin(&scr, &f);
        if (rc == YSCR_QUIT) { aborted = 1; break; }
        if (rc != YSCR_OK) { fprintf(stderr, "trial_sternberg: %s\n", yscr_error(&scr)); failed = 1; break; }

        /* the digits' and the probe's flip records: their onsets, final */
        for (i = 0; i < f.n_done; i++) {
            int j;
            for (j = 0; j < k; j++)
                if (f.done[i].index == item_frame[j]) {
                    yrsp_onset o = yrsp_onset_flip(&f.done[i]);
                    item_t[j] = o.t_ns;
                    item_tier[j] = o.tier;
                }
            if (f.done[i].index == probe_frame) {
                yrsp_onset o = yrsp_onset_flip(&f.done[i]);
                yrsp_set_onset(&rsp, &o);
            }
        }

        if (!in_trial) {
            ytl_seq q;
            const char* a;
            int pool[10];
            if (ytr_next(&trials, &ti) < 0) break;
            k = (int)ytb_num(&tab, ti.condition, size_col);
            present = !strcmp(ytb_text(&tab, ti.condition, probe_col), "present");
            a = ytb_text(&tab, ti.condition, answer_col);
            snprintf(answer, sizeof answer, "%s", a ? a : "");
            if (k < 1 || k > MAX_SET) { fprintf(stderr, "trial_sternberg: set_size %d is not 1..%d\n", k, MAX_SET); failed = 1; break; }
            /* k digits without replacement (a partial shuffle); an absent
             * probe from the 10 - k left */
            for (i = 0; i < 10; i++) pool[i] = i;
            for (i = 0; i < k; i++) {
                int j = i + (int)(ytr_splitmix(&digit_rng) * (10 - i)), tmp = pool[i];
                pool[i] = pool[j]; pool[j] = tmp;
                set[i] = pool[i];
            }
            probe_digit = present ? set[(int)(ytr_splitmix(&digit_rng) * k)]
                                  : pool[k + (int)(ytr_splitmix(&digit_rng) * (10 - k))];
            for (i = 0; i < MAX_SET; i++) {
                item[i] = digit_run(&item_glyph[i], i < k ? set[i] : 0, INK);
                item_frame[i] = -1;
                item_t[i] = 0;
                item_tier[i] = 0;
            }
            probe = digit_run(&probe_glyph, probe_digit, AMBER);
            probe_frame = -1;

            /* The whole trial as one table, checked before anything goes in. */
            n_ops = 0;
            ops[n_ops++] = (ytl_op)YTL_ON(FIX_ON);
            ops[n_ops++] = (ytl_op)YTL_WAIT(YRT_S(FIX_S));
            ops[n_ops++] = (ytl_op)YTL_OFF(FIX_ON);
            for (i = 0; i < k; i++) {
                ops[n_ops++] = (ytl_op)YTL_ON(ITEM0 + i);
                ops[n_ops++] = (ytl_op)YTL_WAIT(YRT_S(ITEM_S));
                ops[n_ops++] = (ytl_op)YTL_OFF(ITEM0 + i);
                ops[n_ops++] = (ytl_op)YTL_WAIT(i + 1 < k ? YRT_S(GAP_S) : YRT_S(RETENTION_S));
            }
            ops[n_ops++] = (ytl_op)YTL_ON(PROBE_ON);
            ytl_clear(&tl, TRIAL);
            ytl_anchor(&tl, TRIAL, f.onset, 0);
            q = ytl_seq_on(&tl, TRIAL);
            if (ytl_run(&q, ops, n_ops) != 0) {
                fprintf(stderr, "trial_sternberg: op %d of the trial's table: error %d\n", q.err_call, q.err);
                failed = 1;
                break;
            }
            yrsp_arm(&rsp, f.onset);
            t_start = f.onset;
            in_trial = responding = 1;
            n_sim_ev = sim_next = 0;
            for (i = 0; sim && i < (int)(sizeof sim_script / sizeof sim_script[0]); i++) {
                const sim_press* p = &sim_script[i];
                yrsp_input* e = &sim_ev[n_sim_ev];
                if (p->trial != ti.index || p->early <= 0 || n_sim_ev + 2 > 8) continue;
                memset(e, 0, sizeof *e);
                e->t = t_start + YRT_S(p->early);
                e->type = YRSP_PRESS;
                e->control = (uint32_t)yrsp_scancode(answer);
                e->stamp = YRSP_TIER_SIM;
                e->value = 1;
                sim_ev[n_sim_ev + 1] = *e;
                sim_ev[n_sim_ev + 1].type = YRSP_RELEASE;
                sim_ev[n_sim_ev + 1].value = 0;
                sim_ev[n_sim_ev + 1].t = e->t + YRT_MS(80);
                n_sim_ev += 2;
            }
        }

        while (yscr_poll(&scr, &ev, NULL))
            if (yscr_event_input(&scr, &ev, &in)) yrsp_feed(&rsp, &in);
        while (sim && sim_next < n_sim_ev && sim_ev[sim_next].t <= f.onset) yrsp_feed(&rsp, &sim_ev[sim_next++]);

        if (responding && yrsp_update(&rsp, f.onset) == YRSP_ENDED) {
            ytl_seq q = ytl_seq_on(&tl, TRIAL);
            ytl_base_time(&tl, TRIAL, f.onset, &bt);
            ytl_at(&q, bt);
            ytl_off(&q, PROBE_ON);
            ytl_wait(&q, YRT_S(ITI_S));
            ytl_mark(&q, END_OF_TRIAL, 0);
            if (q.err) { fprintf(stderr, "trial_sternberg: timeline error %d\n", q.err); failed = 1; break; }
            responding = 0;
        }

        n = ytl_evaluate(&tl, &(ytl_frame){ f.onset, f.period, f.index }, fired, 16);
        for (i = 0; i < n && i < 16; i++) {
            if (fired[i].kind == YTL_ONSET && fired[i].target >= ITEM0 && fired[i].target < ITEM0 + MAX_SET)
                item_frame[fired[i].target - ITEM0] = fired[i].frame;
            if (fired[i].kind == YTL_ONSET && fired[i].target == PROBE_ON) {
                yrsp_onset o = yrsp_onset_landing(&fired[i]);
                yrsp_set_onset(&rsp, &o);
                probe_frame = fired[i].frame;
                if (sim) {   /* the participant answers from the probe's onset */
                    int j;
                    for (j = 0; j < (int)(sizeof sim_script / sizeof sim_script[0]); j++) {
                        const sim_press* p = &sim_script[j];
                        yrsp_input* e = &sim_ev[n_sim_ev];
                        if (p->trial != ti.index || p->early > 0 || n_sim_ev + 2 > 8) continue;
                        memset(e, 0, sizeof *e);
                        e->t = o.t_ns + YRT_S(sim_rt(k, present));
                        e->type = YRSP_PRESS;
                        e->control = (uint32_t)yrsp_scancode(p->which == 1 ? answer : !strcmp(answer, "f") ? "j" : "f");
                        e->stamp = YRSP_TIER_SIM;
                        e->value = 1;
                        sim_ev[n_sim_ev + 1] = *e;
                        sim_ev[n_sim_ev + 1].type = YRSP_RELEASE;
                        sim_ev[n_sim_ev + 1].value = 0;
                        sim_ev[n_sim_ev + 1].t = e->t + YRT_MS(80);
                        n_sim_ev += 2;
                    }
                }
            }
            if (fired[i].kind == YTL_MARK && fired[i].code == END_OF_TRIAL) {
                char rt[32], soa[MAX_SET + 1][32], mem[2 * MAX_SET + 1];
                int correct, j, worst = 0;
                yrsp_finish(&rsp, &res);
                correct = (res.flags & YRSP_R_RESPONDED) && !strcmp(keys[res.response].key, answer);
                sec(rt, sizeof rt, res.rt_ns == YRSP_NO_TIME ? NAN : YRT_TO_S(res.rt_ns));
                /* k was checked against MAX_SET when the trial began; the
                 * clamp lets gcc see that mem cannot overflow */
                int nk = k < 0 ? 0 : k > MAX_SET ? MAX_SET : k;
                for (j = 0; j < nk; j++) {
                    mem[2 * j] = (char)('0' + set[j]);
                    mem[2 * j + 1] = ' ';
                    if (item_tier[j] > worst) worst = item_tier[j];
                }
                mem[nk ? 2 * nk - 1 : 0] = 0;
                for (j = 1; j < MAX_SET; j++)   /* soa[j]: digit j's onset minus digit j-1's */
                    sec(soa[j], sizeof soa[j], j < k && item_t[j] && item_t[j - 1] ? (double)(item_t[j] - item_t[j - 1]) / 1e9 : NAN);
                sec(soa[0], sizeof soa[0], res.t_onset && item_t[k - 1] ? (double)(res.t_onset - item_t[k - 1]) / 1e9 : NAN);
                fprintf(out, "%d,%d,%s,%d,%d,%s,%s,%s,%d,%s,%s,%s,%s,%s,%s,%d,%u,%u,%u,%u,%d,%d\n", ti.index, k, mem,
                        probe_digit, present, answer, res.response_name ? res.response_name : "", rt, correct, soa[1],
                        soa[2], soa[3], soa[4], soa[5], soa[0], worst, (unsigned)res.onset_tier, (unsigned)res.onset_src,
                        (unsigned)res.stamp_tier, (unsigned)res.flags, res.n_early, res.n_anticipations);
                fflush(out);
                printf("trial %2d set %-11s probe %d rt %s response %s %s; soa %s %s %s %s %s probe %s\n", ti.index, mem,
                       probe_digit, rt[0] ? rt : "-", res.response_name ? res.response_name : "-",
                       !(res.flags & YRSP_R_RESPONDED) ? "(too slow)" : correct ? "correct" : "wrong", soa[1], soa[2],
                       soa[3], soa[4], soa[5], soa[0]);
                if (correct) { sum_k += k; sum_rt += YRT_TO_S(res.rt_ns); sum_kk += (double)k * k; sum_krt += k * res.rt_ns; n_fit++; }
                ytr_update(&trials, (res.flags & YRSP_R_RESPONDED) ? correct : YTR_INVALID, NULL);
                if (ti.index < SIM_TRIALS) {
                    sim_res[ti.index] = res;
                    sim_k[ti.index] = k;
                    sim_present[ti.index] = present;
                    sim_correct[ti.index] = correct;
                    for (j = 0; j <= MAX_SET; j++) sim_soa[ti.index][j] = soa[j][0] ? atof(soa[j]) : NAN;
                }
                rows++;
                in_trial = 0;
            }
        }
        ygfx_apply(binds, (int)(sizeof binds / sizeof binds[0]), ytl_values(&tl));
        yscr_mark(&scr, YSCR_PHASE_EVALUATE);
        ygfx_begin(&gfx, &f);
        ygfx_draw(&gfx, &fix);
        for (i = 0; i < MAX_SET; i++) ygfx_draw(&gfx, &item[i]);
        ygfx_draw(&gfx, &probe);
        ygfx_end(&gfx);
        yscr_flip(&scr);
    }
    fclose(out);
    ygfx_close(&gfx);
    yscr_close(&scr);
    yol_close(&ol);
    printf("%d trials written to %s%s\n", rows, out_path, aborted ? " (stopped with Shift+Esc)" : "");
    if (n_fit > 1 && sum_kk * n_fit != sum_k * sum_k)
        printf("RT over set size, correct trials, present and absent pooled: slope %.1f ms per digit (%d trials)\n",
               1e3 * (n_fit * sum_krt - sum_k * sum_rt) / (n_fit * sum_kk - sum_k * sum_k), n_fit);

    if (sim && !failed && reps != 1) printf("--sim: the self-check needs the default --reps\n");
    else if (sim && !failed) {
        int bad = rows != SIM_TRIALS;
        double period = (double)f.period / 1e9;
        if (bad) fprintf(stderr, "trial_sternberg: --sim: %d rows, not %d\n", rows, SIM_TRIALS);
        for (i = 0; i < rows && i < SIM_TRIALS; i++) {
            const yrsp_result* r = &sim_res[i];
            int ok = 1, j;
            /* The simulated display runs on the host's clock: while a
             * loaded host's frame loop is behind, begin() plans later
             * vblanks, as on a real display, and a digit lands on a later
             * frame. An SOA then differs by whole periods: 2 and 3 under 12
             * CPU-bound processes (measured 2026-10-10; one period was
             * allowed before, and 2 runs in 10 failed). */
            for (j = 0; j <= MAX_SET; j++) {
                double want = j == 0 ? ITEM_S + RETENTION_S : ITEM_S + GAP_S, d = fabs(sim_soa[i][j] - want);
                if (j > 0 && j >= sim_k[i]) ok = ok && sim_soa[i][j] != sim_soa[i][j];   /* empty past the set */
                else ok = ok && fabs(d - period * floor(d / period + 0.5)) < 1e-6;
            }
            if (i == 4) ok = ok && !(r->flags & YRSP_R_RESPONDED) && (r->flags & YRSP_R_TIMEOUT);
            else ok = ok && (r->flags & YRSP_R_RESPONDED) && fabs(YRT_TO_S(r->rt_ns) - sim_rt(sim_k[i], sim_present[i])) < 1e-6;
            ok = ok && r->n_early == (i == 1);
            ok = ok && sim_correct[i] == (i != 2 && i != 4);
            ok = ok && r->onset_tier == YSCR_TIER_SIM && r->onset_src == YRSP_ONSET_FLIP;
            if (!ok) {
                fprintf(stderr, "trial_sternberg: --sim: trial %d: set %d rt %.9f flags 0x%x n_early %d soa %.9f %.9f\n", i,
                        sim_k[i], YRT_TO_S(r->rt_ns), (unsigned)r->flags, r->n_early, sim_soa[i][1], sim_soa[i][0]);
                bad = 1;
            }
        }
        if (bad) return 1;
        printf("--sim: every trial came out as scripted\n");
    }
    return failed;
}
