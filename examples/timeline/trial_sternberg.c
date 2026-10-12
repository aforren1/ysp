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
 * The data file has one row per trial (ysp/trials.h's data writer): the
 * session's columns (index, block, ..., set_size, probe_type, answer),
 * then trial_index, memory_set, probe (the digit), probe_present, correct,
 * items_tier (the worst tier of the digits' onsets), probe_soa, soa_2 ..
 * soa_6, then the response's (YRSP_DATA_COLS: response, rt, onset_src,
 * onset_tier, the residuals, onset_frame, rsp_tier, rsp_flags,
 * n_anticipations, n_early). Times in seconds; the settings go to
 * FILE.meta. Shift+Esc stops the session; the rows so far are kept.
 *
 * --sim runs the 6 conditions once on the simulated display with a
 * synthetic participant whose RT grows by 38 ms per digit, 50 ms more for
 * an absent probe: a press during the memory set and then a response, a
 * wrong key, and a timeout among correct responses. It checks its own
 * results (the script runs from the probe's planned onset, the RT from its
 * flip record) and exits 1 on a mismatch. About 27 s.
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
    "set_size,probe_type,answer\n"
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
static yrsp_input sim_ev[8];
static int n_sim_ev, sim_next;

/* The program's columns; ysp/trials.h writes the session's before them,
 * the soa columns are set per row, and yrsp_data_set() fills
 * YRSP_DATA_COLS. */
typedef struct row { int trial_index, probe, items_tier; bool probe_present, correct; char memory_set[2 * MAX_SET + 1]; } row;
static const ytr_col cols[] = {
    YTR_COL(row, trial_index, YTR_COL_INT), YTR_COL(row, memory_set, YTR_COL_CHARS), YTR_COL(row, probe, YTR_COL_INT),
    YTR_COL(row, probe_present, YTR_COL_BOOL), YTR_COL(row, correct, YTR_COL_BOOL), YTR_COL(row, items_tier, YTR_COL_INT),
    YTR_COL_SET("probe_soa", YTR_COL_TIME_NS), YTR_COL_SET("soa_2", YTR_COL_TIME_NS), YTR_COL_SET("soa_3", YTR_COL_TIME_NS),
    YTR_COL_SET("soa_4", YTR_COL_TIME_NS), YTR_COL_SET("soa_5", YTR_COL_TIME_NS), YTR_COL_SET("soa_6", YTR_COL_TIME_NS),
    YRSP_DATA_COLS,
};

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
static ytr_data data;
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

static int fail(const char* why) {
    fprintf(stderr, "trial_sternberg: %s\n", why);
    return 1;
}

/* --sim: a press of `key` at t, held 0.08 s. */
static void sim_add(int64_t t, const char* key) {
    if (n_sim_ev + 2 > 8) return;
    sim_ev[n_sim_ev] = (yrsp_input){ .t = t, .type = YRSP_PRESS, .control = (uint32_t)yrsp_scancode(key),
                                     .stamp = YRSP_TIER_SIM, .value = 1 };
    sim_ev[n_sim_ev + 1] = sim_ev[n_sim_ev];
    sim_ev[n_sim_ev + 1].type = YRSP_RELEASE;
    sim_ev[n_sim_ev + 1].value = 0;
    sim_ev[n_sim_ev + 1].t = t + YRT_MS(80);
    n_sim_ev += 2;
}

int main(int argc, char** argv) {
    yscr_frame f = { 0 };   /* its period is read after the loop */
    ytl_event fired[128];                  /* the timeline's capacity: all that can fire */
    ytl_op ops[3 + 4 * MAX_SET + 1];
    ytr_trial_info ti = { 0 };
    yrsp_result res, sim_res[SIM_TRIALS];
    row r = { 0 };
    double sim_soa[SIM_TRIALS][MAX_SET];
    int64_t sim_plan[SIM_TRIALS] = { 0 };   /* where the script's times start */
    int sim_k[SIM_TRIALS], sim_present[SIM_TRIALS], sim_correct[SIM_TRIALS];
    int64_t item_frame[MAX_SET], item_t[MAX_SET], soa[MAX_SET];
    uint8_t item_tier[MAX_SET];
    int set[10], k = 0, present = 0, n_ops;
    char line[1024], meta[2048], meta_path[1100], why[300];
    const char *out_path = "trial_sternberg.csv", *font_path = DEFAULT_FONT, *answer = "";
    uint64_t seed = 20261009, digit_rng, order_rng;
    int i, j, n, landed, sim = 0, reps = 0, fullscreen = 0, aborted = 0, failed = 0, rows = 0, font_real;
    int in_trial = 0, responding = 0, size_col, probe_col, answer_col, soa_col;
    double sum_k = 0, sum_rt = 0, sum_kk = 0, sum_krt = 0;
    int n_fit = 0;

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
    order_rng = seed;                           /* the order's; seed stays the argument, for the meta */

    /* the display and the graphics, opened together and primed */
    if (!ygfx_open_screen(&gfx, &scr, &(yscr_desc){ .backend = sim ? YSCR_BACKEND_SIM : 0, .windowed = !fullscreen },
                          &(ygfx_desc){ .background = { 0.2f, 0.2f, 0.2f } }))
        return fail(ygfx_error(&gfx));
    if (!yol_open(&ol, NULL)) return fail("outline context");
    font_real = make_digits(font_path, why, sizeof why);
    if (font_real < 0) { fprintf(stderr, "trial_sternberg: digits: %s %s\n", yol_error(&ol), ygfx_error(&gfx)); return 1; }
    if (!font_real) printf("%s: the 5 x 7 bitmap digits instead\n", why);
    fix = ygfx_shape(&(ygfx_shape_desc){ .shape = YGFX_CROSS, .w = 24, .h = 24, .shape_p = { 3 },
                                         .edge = YGFX_EDGE_COSINE, .edge_width = 1, .color = { 0.6f, 0.6f, 0.6f } });
    probe = digit_run(&probe_glyph, 0, AMBER);
    ygfx_prime(&gfx);   /* the digits' set made the curve program after the open's prime (docs/gfx.md) */

    /* the conditions, in a random order */
    if (!ytb_open_csv(&tab, &(ytb_csv_desc){ .text = conditions_csv, .len = sizeof conditions_csv - 1,
                                             .arena = table_arena, .arena_size = sizeof table_arena }))
        return fail(ytb_error(&tab));
    size_col = ytb_col(&tab, "set_size");
    probe_col = ytb_col(&tab, "probe_type");
    answer_col = ytb_col(&tab, "answer");
    if (!ytr_open(&trials, &(ytr_desc){ .table = &tab, .reps = reps, .order = YTR_ORDER_RANDOM, .rng = ytr_splitmix,
                                        .rng_ctx = &order_rng }))
        return fail(ytr_error(&trials));
    if (!ytl_open(&tl, &(ytl_desc){ .events = storage, .event_capacity = 128, .n_channels = N_CHANNELS }))
        return fail(ytl_error(&tl));
    if (!yrsp_open(&rsp, &(yrsp_desc){ .choices = keys, .n_choices = 2, .duration_ns = YRT_S(RESPONSE_S),
                                       .min_rt_ns = YRT_S(MIN_RT_S) }))
        return fail(yrsp_error(&rsp));

    /* the data file, and the settings beside it */
    yscr_describe(&scr, line, sizeof line);
    snprintf(meta, sizeof meta, "screen=%s\nysp_response=%s\nkeyboard=%s\ndigits=%s\nseed=%llu\n", line, yrsp_version(),
             sim ? "synthetic (tier SIM)" : "SDL through yscr_event_input", font_real ? font_path : "5 x 7 bitmap",
             (unsigned long long)seed);
    snprintf(meta_path, sizeof meta_path, "%s.meta", out_path);
    if (!ytr_data_open(&data, &(ytr_data_desc){ .path = out_path, .trials = &trials, .cols = cols,
            .n_cols = (int)(sizeof cols / sizeof cols[0]), .meta_path = meta_path, .meta = meta }))
        return fail(ytr_data_error(&data));
    soa_col = ytr_data_col(&data, "probe_soa");   /* then soa_2 .. soa_6 */
    printf("%s\n", line);

    while (!aborted) {
        int rc = yscr_begin(&scr, &f);
        if (rc == YSCR_QUIT) { aborted = 1; break; }
        if (rc != YSCR_OK) { failed = fail(yscr_error(&scr)); break; }
        for (i = 0; i < f.n_done; i++)   /* the digits' flip records: their onsets, final */
            for (j = 0; j < k; j++)
                if (f.done[i].index == item_frame[j]) {
                    yrsp_onset o = yrsp_onset_flip(&f.done[i]);
                    item_t[j] = o.t_ns;
                    item_tier[j] = o.tier;
                }

        if (!in_trial) {
            ytl_seq q;
            int pool[10];
            if (ytr_next(&trials, &ti) < 0) break;
            k = (int)ytb_num(&tab, ti.condition, size_col);
            present = !strcmp(ytb_text(&tab, ti.condition, probe_col), "present");
            answer = ytb_text(&tab, ti.condition, answer_col);
            if (k < 1 || k > MAX_SET) { fprintf(stderr, "trial_sternberg: set_size %d is not 1..%d\n", k, MAX_SET); failed = 1; break; }
            /* k digits without replacement (a partial shuffle); an absent
             * probe from the 10 - k left */
            for (i = 0; i < 10; i++) pool[i] = i;
            for (i = 0; i < k; i++) {
                int m = i + (int)(ytr_splitmix(&digit_rng) * (10 - i)), tmp = pool[i];
                pool[i] = pool[m]; pool[m] = tmp;
                set[i] = pool[i];
            }
            r.probe = present ? set[(int)(ytr_splitmix(&digit_rng) * k)] : pool[k + (int)(ytr_splitmix(&digit_rng) * (10 - k))];
            for (i = 0; i < MAX_SET; i++) {
                item[i] = digit_run(&item_glyph[i], i < k ? set[i] : 0, INK);
                item_frame[i] = -1;
                item_t[i] = 0;
                item_tier[i] = 0;
            }
            probe = digit_run(&probe_glyph, r.probe, AMBER);

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
            yrsp_arm(&rsp, f.onset);   /* presses from the fixation on are logged (EARLY) */
            in_trial = responding = 1;
            n_sim_ev = sim_next = 0;
            for (i = 0; sim && i < (int)(sizeof sim_script / sizeof sim_script[0]); i++)
                if (sim_script[i].trial == ti.index && sim_script[i].early > 0)
                    sim_add(f.onset + YRT_S(sim_script[i].early), answer);
        }

        yrsp_feed_screen(&scr, &rsp, 1);
        while (sim && sim_next < n_sim_ev && sim_ev[sim_next].t <= f.onset) yrsp_feed(&rsp, &sim_ev[sim_next++]);

        /* the probe frame's flip record, when it arrives, is the final
         * onset; a response or the deadline ends the window */
        if (yrsp_update_frame(&rsp, &f) == YRSP_ENDED && responding) {
            ytl_seq q = ytl_seq_on(&tl, TRIAL);
            int64_t bt = 0;
            ytl_base_time(&tl, TRIAL, f.onset, &bt);
            ytl_at(&q, bt);
            ytl_off(&q, PROBE_ON);
            ytl_wait(&q, YRT_S(ITI_S));
            ytl_mark(&q, END_OF_TRIAL, 0);
            if (q.err) { failed = fail(ytl_strerror(q.err)); break; }
            responding = 0;
        }

        n = ytl_evaluate(&tl, &(ytl_frame){ f.onset, f.period, f.index }, fired, 128);
        for (i = 0; i < n; i++)   /* a digit landed: its record comes later */
            if (fired[i].kind == YTL_ONSET && fired[i].target >= ITEM0 && fired[i].target < ITEM0 + MAX_SET)
                item_frame[fired[i].target - ITEM0] = fired[i].frame;
        /* the probe landed on this frame: its planned onset, for now */
        landed = yrsp_set_landing(&rsp, fired, n, PROBE_ON);
        if (landed >= 0 && sim && ti.index < SIM_TRIALS) {   /* the participant answers from the plan */
            sim_plan[ti.index] = fired[landed].onset;
            for (j = 0; j < (int)(sizeof sim_script / sizeof sim_script[0]); j++)
                if (sim_script[j].trial == ti.index && sim_script[j].early <= 0)
                    sim_add(sim_plan[ti.index] + YRT_S(sim_rt(k, present)),
                            sim_script[j].which == 1 ? answer : !strcmp(answer, "f") ? "j" : "f");
        }
        for (i = 0; i < n; i++)
            if (fired[i].kind == YTL_MARK && fired[i].code == END_OF_TRIAL) {
                /* the trial is over, and the records of its digits and its probe are in */
                int worst = 0, nk = k < 0 ? 0 : k > MAX_SET ? MAX_SET : k;   /* the clamp lets gcc see the bounds */
                yrsp_finish(&rsp, &res);
                r.trial_index = ti.index;
                r.probe_present = present;
                r.correct = (res.flags & YRSP_R_RESPONDED) && !strcmp(keys[res.response].key, answer);
                for (j = 0; j < nk; j++) {
                    r.memory_set[2 * j] = (char)('0' + set[j]);
                    r.memory_set[2 * j + 1] = ' ';
                    if (item_tier[j] > worst) worst = item_tier[j];
                }
                r.memory_set[nk ? 2 * nk - 1 : 0] = 0;
                r.items_tier = worst;
                /* soa[j]: digit j's onset minus digit j-1's; soa[0]: the probe's minus the last digit's */
                soa[0] = res.t_onset && item_t[nk - 1] ? res.t_onset - item_t[nk - 1] : YTR_MISSING_NS;
                for (j = 1; j < MAX_SET; j++)
                    soa[j] = j < nk && item_t[j] && item_t[j - 1] ? item_t[j] - item_t[j - 1] : YTR_MISSING_NS;
                ytr_update(&trials, (res.flags & YRSP_R_RESPONDED) ? r.correct : YTR_INVALID, NULL);
                for (j = 0; j < MAX_SET; j++) ytr_data_set_time_ns(&data, soa_col + j, soa[j]);
                if (yrsp_data_set(&data, &res) < 0 || ytr_data_write(&data, &r) < 0) failed = fail(ytr_data_error(&data));
                if (!(res.flags & YRSP_R_RESPONDED)) printf("trial %2d set %-11s probe %d too slow", ti.index, r.memory_set, r.probe);
                else printf("trial %2d set %-11s probe %d rt %.3f s, %s", ti.index, r.memory_set, r.probe,
                            YRT_TO_S(res.rt_ns), r.correct ? "correct" : "wrong");
                printf("; probe soa %.3f s\n", soa[0] == YTR_MISSING_NS ? NAN : YRT_TO_S(soa[0]));
                if (r.correct) { sum_k += k; sum_rt += YRT_TO_S(res.rt_ns); sum_kk += (double)k * k; sum_krt += k * YRT_TO_S(res.rt_ns); n_fit++; }
                if (ti.index < SIM_TRIALS) {
                    sim_res[ti.index] = res;
                    sim_k[ti.index] = k;
                    sim_present[ti.index] = present;
                    sim_correct[ti.index] = r.correct;
                    for (j = 0; j < MAX_SET; j++) sim_soa[ti.index][j] = soa[j] == YTR_MISSING_NS ? NAN : YRT_TO_S(soa[j]);
                }
                rows++;
                in_trial = 0;
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
    if (ytr_data_finish(&data) < 0) failed = fail(ytr_data_error(&data));
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
            const yrsp_result* q = &sim_res[i];
            int ok = 1;
            /* The simulated display runs on the host's clock: while a
             * loaded host's frame loop is behind, begin() plans later
             * vblanks, as on a real display, and a digit lands on a later
             * frame. An SOA then differs by whole periods: 2 and 3 under 12
             * CPU-bound processes (measured 2026-10-10; one period was
             * allowed before, and 2 runs in 10 failed). */
            for (j = 0; j < MAX_SET; j++) {
                double want = j == 0 ? ITEM_S + RETENTION_S : ITEM_S + GAP_S, d = fabs(sim_soa[i][j] - want);
                if (j > 0 && j >= sim_k[i]) ok = ok && sim_soa[i][j] != sim_soa[i][j];   /* empty past the set */
                else ok = ok && fabs(d - period * floor(d / period + 0.5)) < 1e-6;
            }
            if (i == 4) ok = ok && !(q->flags & YRSP_R_RESPONDED) && (q->flags & YRSP_R_TIMEOUT);
            else ok = ok && (q->flags & YRSP_R_RESPONDED) && /* script from the plan, RT from the flip record */
                      llabs(q->rt_ns + (q->t_onset - sim_plan[i]) - YRT_S(sim_rt(sim_k[i], sim_present[i]))) <= 1000;
            ok = ok && q->n_early == (i == 1);
            ok = ok && sim_correct[i] == (i != 2 && i != 4);
            ok = ok && q->onset_tier == YSCR_TIER_SIM && q->onset_src == YRSP_ONSET_FLIP;
            if (!ok) {
                fprintf(stderr, "trial_sternberg: --sim: trial %d: set %d rt %.9f flags 0x%x n_early %d soa %.9f %.9f\n", i,
                        sim_k[i], YRT_TO_S(q->rt_ns), (unsigned)q->flags, q->n_early, sim_soa[i][1], sim_soa[i][0]);
                bad = 1;
            }
        }
        if (bad) return 1;
        printf("--sim: every trial came out as scripted\n");
    }
    return failed;
}
