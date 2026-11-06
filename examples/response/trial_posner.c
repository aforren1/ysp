/* trial_posner.c - spatial cueing (Posner 1980) as a small experiment:
 * ysp/trials.h, ysp/timeline.h, ysp/gfx.h, ysp/outline.h and
 * ysp/response.h.
 *
 * The display: a fixation cross at the center and two outlined boxes, 8 deg
 * left and right of it, on mid-gray. Each trial: the cross and the boxes for
 * 1.0 to 1.5 s (uniform), then a cue for 0.1 s, then the target (a dark
 * disc in one box) at a cue-target SOA of 0.1, 0.3 or 0.7 s from the cue's
 * onset. The target stays until the response or 1.5 s. An ITI of 1 s
 * follows. Two cue types, one per block: exogenous (the cued box's outline
 * brightens and thickens) and endogenous (an arrow replaces the cross and
 * points to the cued box). 80 % of target trials are valid (the target in
 * the cued box), 20 % invalid; catch trials, 10 % on top of the target
 * trials, have a cue and no target. Space detects the target (simple RT);
 * with --localize, Left or Right gives its side. A press before the target
 * (EARLY in the collector) stops the trial; it and a press on a catch
 * trial show "too early" for 0.5 s. No other feedback.
 *
 * The design is the table below (DESIGN): every level and weight. The
 * conditions table is built from it, the weights become ysp/trials.h's
 * cond_reps, the cue type is a BLOCKED group in random order, and no two
 * catch trials run back to back. The fixation duration is the session's
 * jitter fix_s. The cue and the target are events on the trial's timeline
 * base; RT runs from the target's flip record, and the measured SOA is the
 * target's flip onset minus the cue's. A catch trial's "target" is an
 * event on a channel that draws nothing, so its response window and
 * deadline run from that frame's flip record as a target's would.
 *
 * Sizes are in deg through --ppd (pixels per degree). Without it the
 * geometry is nominal (570 mm away, 0.27 mm a pixel: 36.8 px/deg), flagged
 * NOMINAL in FILE.meta; ysp/screen.h does not report the screen's size.
 *
 * The data file has one row per trial (ysp/trials.h's data writer): the
 * session's columns (index, block, ..., outcome, cue, validity, soa_s (the
 * planned SOA), cue_side, fix_s), then target_side (empty on a catch
 * trial), catch_trial, early, correct, soa_measured_s, cue_tier, then the
 * response's (YRSP_DATA_COLS: response, rt_s, onset_tier for the target,
 * ...). Times in seconds; the settings go to FILE.meta. At the end: the
 * validity effect (invalid minus valid median RT, correct target trials)
 * per cue type and SOA, and the catch trials' false-alarm rate. Shift+Esc
 * stops the session; the rows so far are kept.
 *
 * --sim runs the design once on the simulated display, on the virtual
 * clock, with a scripted participant: RT 0.300 s valid and 0.340 s
 * invalid, except exogenous at 0.7 s (0.330 valid, 0.300 invalid:
 * inhibition of return) and endogenous at 0.1 s (0.320 and 0.330); one
 * early press during a fixation, one false alarm 0.25 s into the first
 * catch trial. It checks the printed effects and the false-alarm rate
 * against the script, reads its data file back and checks every row, and
 * exits 1 on a mismatch. --auto runs the same participant in a window (the
 * SOAs then are only reported).
 *
 * Usage: trial_posner [--sim | --auto] [--localize] [--cue exo|endo|both] [--reps N]
 *          [--trials N] [--ppd PX] [--fullscreen] [--seed N] [--out FILE] [--font PATH]
 *   --reps N    repetitions of the weighted design per block (default 2;
 *               --sim 1): 66 trials per block per repetition
 *   --trials N  stop after N trials (a short demonstration)
 * Exit code: 0 (also after Shift+Esc), 1 when something did not open or a
 * --sim check failed, 2 for a bad argument.
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#define YTB_STDIO   /* ytb_open_csv_file(): --sim reads its data file back */
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

/* DESIGN: the defaults, all in one place. Times in s, sizes in deg. The
 * weights multiply per row; validity 8 : 2 : 1 is 80 % valid among the
 * target trials plus catch trials at 10 % of their number. */
static const char* const CUES[2] = { "exo", "endo" };
static const int W_CUE[2] = { 1, 1 };
static const char* const VALIDITY[3] = { "valid", "invalid", "catch" };
static const int W_VALIDITY[3] = { 8, 2, 1 };
static const double SOA_S[3] = { 0.1, 0.3, 0.7 };
static const int W_SOA[3] = { 1, 1, 1 };
static const char* const SIDES[2] = { "left", "right" };
static const int W_SIDE[2] = { 1, 1 };
#define FIX_LO_S      1.0
#define FIX_HI_S      1.5
#define CUE_S         0.1
#define DEADLINE_S    1.5
#define FEEDBACK_S    0.5
#define ITI_S         1.0
#define ECC_DEG       8.0     /* box centers from the fixation */
#define BOX_DEG       1.5
#define BOX_LINE_DEG  0.06
#define CUE_LINE_DEG  0.2
#define TARGET_DEG    0.8
#define FIX_DEG       0.5
#define ARROW_DEG     1.2
#define TEXT_DEG      0.8     /* per em */
#define NOMINAL_PPD   (570.0 * 0.017455064928217585 / 0.27)   /* 570 mm, 0.27 mm a pixel */

enum { CUE_BOX, ARROW, TARGET, CATCH, FEEDBACK, N_CHANNELS };
enum { TRIAL = 1 };
enum { END_OF_TRIAL = 1 };
#define N_ROWS (2 * 3 * 3 * 2)

#ifdef _WIN32
#define DEFAULT_FONT "C:/Windows/Fonts/arial.ttf"
#else
#define DEFAULT_FONT "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
#endif

/* The scripted participant of --sim and --auto. */
static double sim_rt(int cue, int valid, int soa) {
    if (cue == 0 && soa == 2) return valid ? 0.330 : 0.300;   /* inhibition of return */
    if (cue == 1 && soa == 0) return valid ? 0.320 : 0.330;   /* an arrow takes time to use */
    return valid ? 0.300 : 0.340;
}
#define SIM_EARLY_S 0.6   /* from the trial's start: inside the shortest fixation */
#define SIM_FA_S    0.25  /* from the catch trial's empty frame */

/* The program's columns, after the session's; then YRSP_DATA_COLS. */
typedef struct row { const char* target_side; bool catch_trial, early, correct; int cue_tier; } row;
static const ytr_col cols[] = {
    YTR_COL(row, target_side, YTR_COL_STRING), YTR_COL(row, catch_trial, YTR_COL_BOOL), YTR_COL(row, early, YTR_COL_BOOL),
    YTR_COL(row, correct, YTR_COL_BOOL), YTR_COL_SET("soa_measured_s", YTR_COL_TIME_NS), YTR_COL(row, cue_tier, YTR_COL_INT),
    YRSP_DATA_COLS,
};

/* The letters of "too early" as 5 x 7 capitals (gfx_gallery.c's), in the
 * order of BM_LETTERS, for when the font does not open. */
static const char BM_LETTERS[] = "toearly";
static const uint8_t bm[7][7] = {
    { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 }, { 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E },
    { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F }, { 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 },
    { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 }, { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F },
    { 0x11, 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04 },
};

static yscr_screen scr;
static ygfx_gfx gfx;
static yol_ctx ol;
static ytl_timeline tl;
static ytl_event storage[64];
static ytr_trials trials;
static ytr_data data;
static yrsp_collector rsp;
static uint64_t table_arena[2048], back_arena[1 << 16];
static ytb_table tab, back;
static char conditions_csv[2048];
static int cond_reps[N_ROWS];
static yrsp_input sim_ev[4];
static int n_sim_ev, sim_next;
typedef struct kept { int8_t cue, soa, valid; int64_t rt; } kept;   /* a correct target trial's RT */
static kept kept_rt[YTR_MAX_TRIALS];
static int64_t cell[YTR_MAX_TRIALS];

static ygfx_stim fix, boxes[2], cue_box, arrow, target, text;
static ygfx_citem text_item;
static float arrow_v[14];
static const ygfx_bind binds[] = {
    { .stim = &cue_box, .param = YGFX_P_VISIBLE, .channel = CUE_BOX },
    { .stim = &arrow, .param = YGFX_P_VISIBLE, .channel = ARROW },
    { .stim = &target, .param = YGFX_P_VISIBLE, .channel = TARGET },
    { .stim = &text, .param = YGFX_P_VISIBLE, .channel = FEEDBACK },
};

static int fail(const char* why) {
    fprintf(stderr, "trial_posner: %s\n", why);
    return 1;
}

/* "too early" as one path, glyph 0 of a curve set: from the font when it
 * opens (returns 1), else from the bitmap capitals (0); -1 on a failure. */
static int make_text(const char* path, float px_per_em) {
    static const char msg[] = "too early";
    yol_cset s;
    yol_cset_desc od;
    yol_path p;
    yol_font font;
    ygfx_cset set;
    ygfx_cset_desc d;
    ygfx_crun_desc rd;
    uint8_t* bytes = NULL;
    long n = 0;
    double pen = 0, adv, lsb;
    int real = 0, rc = YOL_OK, i, r, c;
    FILE* fp = fopen(path, "rb");
    if (fp) {
        fseek(fp, 0, SEEK_END); n = ftell(fp); fseek(fp, 0, SEEK_SET);
        bytes = n > 0 ? (uint8_t*)malloc((size_t)n) : NULL;
        if (bytes && fread(bytes, 1, (size_t)n, fp) != (size_t)n) { free(bytes); bytes = NULL; }
        fclose(fp);
    }
    if (bytes && yol_font_view(&font, bytes, (size_t)n, 0, NULL, 0) == YOL_OK) real = 1;
    memset(&od, 0, sizeof od);
    od.n_glyphs = 1;
    if (!yol_cset_open(&s, &ol, &od)) { free(bytes); return -1; }
    yol_path_init(&p, &ol);
    for (i = 0; msg[i] && rc == YOL_OK; i++) {
        if (real) {
            yol_glyph_desc gd;
            uint32_t g = yol_font_glyph_index(&font, (uint32_t)msg[i]);
            memset(&gd, 0, sizeof gd);
            gd.x = pen;
            rc = yol_font_glyph(&ol, &font, g, &p, &gd);
            if (yol_font_hmetrics(&font, g, &adv, &lsb) == YOL_OK) pen += adv;
        } else {
            const char* at = strchr(BM_LETTERS, msg[i]);
            for (r = 0; at && r < 7; r++)
                for (c = 0; c < 5; c++)
                    if (bm[at - BM_LETTERS][r] & (0x10 >> c)) yol_rect(&p, pen + 0.1 * c, 0.1 * (r - 7), 0.1, 0.1, 0, 0);
            pen += 0.6;
        }
    }
    if (rc == YOL_OK) rc = yol_path_end(&p);
    if (rc == YOL_OK) rc = yol_cset_add(&s, 0, &p);
    memset(&d, 0, sizeof d);
    d.texels = s.texels; d.n_texels = s.n_texels; d.words = s.words; d.n_words = s.n_words;
    set.id = 0;
    if (rc == YOL_OK) set = ygfx_cset_make(&gfx, &d);
    yol_path_free(&p);
    yol_cset_close(&s);
    free(bytes);
    if (rc != YOL_OK || !set.id) return -1;
    memset(&text_item, 0, sizeof text_item);
    text_item.scale = 1; text_item.gate = 1;
    memset(&rd, 0, sizeof rd);
    rd.set = set; rd.items = &text_item; rd.n = 1; rd.size = px_per_em;
    rd.y = -2.0f * px_per_em;   /* above the fixation */
    rd.color[0] = rd.color[1] = rd.color[2] = 0.05f;
    text = ygfx_crun(&gfx, &rd);
    return real;
}

static ygfx_stim shape(ygfx_shape_kind kind, float w, float p0, float stroke, float gray) {
    ygfx_shape_desc d;
    memset(&d, 0, sizeof d);
    d.shape = kind;
    d.w = d.h = w;
    d.shape_p[0] = p0;
    d.stroke = stroke;
    d.edge = YGFX_EDGE_COSINE;
    d.edge_width = 1;
    d.color[0] = d.color[1] = d.color[2] = gray;
    return ygfx_shape(&d);
}

/* The trial's end from base time bt: `off` hidden, "too early" for a
 * while when asked, then the ITI and the end mark. */
static int end_trial(int64_t bt, int off, int feedback) {
    ytl_seq q = ytl_seq_on(&tl, TRIAL);
    ytl_at(&q, bt);
    if (off >= 0) ytl_off(&q, off);
    if (feedback) {
        ytl_on(&q, FEEDBACK);
        ytl_wait(&q, YRT_S(FEEDBACK_S));
        ytl_off(&q, FEEDBACK);
    }
    ytl_wait(&q, YRT_S(ITI_S));
    ytl_mark(&q, END_OF_TRIAL, 0);
    return q.err;
}

/* a press of `key` at t, held 0.08 s */
static void sim_add(int64_t t, const char* key) {
    sim_ev[0] = (yrsp_input){ .t = t, .type = YRSP_PRESS, .control = (uint32_t)yrsp_scancode(key), .stamp = YRSP_TIER_SIM, .value = 1 };
    sim_ev[1] = sim_ev[0];
    sim_ev[1].type = YRSP_RELEASE;
    sim_ev[1].value = 0;
    sim_ev[1].t = t + YRT_MS(80);
    n_sim_ev = 2;
    sim_next = 0;
}

static int cmp_i64(const void* a, const void* b) {
    int64_t x = *(const int64_t*)a, y = *(const int64_t*)b;
    return x < y ? -1 : x > y;
}
/* the median RT of a cell, YRSP_NO_TIME when it is empty */
static int64_t median(int n_kept, int cue, int soa, int valid, int* n_out) {
    int i, n = 0;
    for (i = 0; i < n_kept; i++)
        if (kept_rt[i].cue == cue && kept_rt[i].soa == soa && kept_rt[i].valid == valid) cell[n++] = kept_rt[i].rt;
    *n_out = n;
    if (!n) return YRSP_NO_TIME;
    qsort(cell, (size_t)n, sizeof cell[0], cmp_i64);
    return n % 2 ? cell[n / 2] : (cell[n / 2 - 1] + cell[n / 2]) / 2;
}

int main(int argc, char** argv) {
    static const yrsp_choice detect[] = { { .key = "space" } }, localize[] = { { .key = "left" }, { .key = "right" } };
    yscr_frame f = { 0 };
    ytl_event fired[64];                   /* the timeline's capacity: all that can fire */
    ytr_trial_info ti = { 0 };
    ytr_desc td;
    ytb_csv_desc cd;
    yrsp_result res;
    row r = { 0 };
    char line[1024], meta[2048], meta_path[1100], view_note[160];
    const char *out_path = "trial_posner.csv", *font_path = DEFAULT_FONT, *cue_arg = "both";
    uint64_t seed = 20261106, seed0;
    double ppd = 0;
    int i, k, n, sim = 0, autop = 0, loc = 0, reps = 0, max_trials = 0, fullscreen = 0, aborted = 0, failed = 0, rows = 0;
    int in_trial = 0, responding = 0, early = 0, n_kept = 0, font_real, use_cue[2] = { 1, 1 };
    int cue = 0, valid = 0, is_catch = 0, soa = 0, side = 0, tside = 0, early_trial = -1, fa_trial = -1;
    int n_total = 0, cue_col, val_col, soa_col, side_col, meas_col, n_catch[2] = { 0, 0 }, n_fa[2] = { 0, 0 }, n_early = 0;
    int64_t cue_frame = -1, target_frame = -1, cue_t = 0, effect[2][3];
    uint8_t cue_tier = 0;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--sim")) sim = 1;
        else if (!strcmp(argv[i], "--auto")) autop = 1;
        else if (!strcmp(argv[i], "--localize")) loc = 1;
        else if (!strcmp(argv[i], "--fullscreen")) fullscreen = 1;
        else if (!strcmp(argv[i], "--cue") && i + 1 < argc) cue_arg = argv[++i];
        else if (!strcmp(argv[i], "--reps") && i + 1 < argc) reps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--trials") && i + 1 < argc) max_trials = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--ppd") && i + 1 < argc) ppd = atof(argv[++i]);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) seed = (uint64_t)strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out_path = argv[++i];
        else if (!strcmp(argv[i], "--font") && i + 1 < argc) font_path = argv[++i];
        else break;
    }
    if (!strcmp(cue_arg, "exo")) use_cue[1] = 0;
    else if (!strcmp(cue_arg, "endo")) use_cue[0] = 0;
    else if (strcmp(cue_arg, "both")) i = 0;
    if (i < argc || (sim && autop) || max_trials < 0 || (ppd != 0 && !(ppd >= 1 && ppd <= 1000))) {
        fprintf(stderr, "usage: trial_posner [--sim | --auto] [--localize] [--cue exo|endo|both] [--reps N] [--trials N]\n"
                        "                    [--ppd PX] [--fullscreen] [--seed N] [--out FILE] [--font PATH]\n");
        return 2;
    }
    /* --sim runs on ysp/rt.h's virtual clock: the same records on any host */
    if (sim) yrt_virtual_begin(0);
    if (reps == 0) reps = sim ? 1 : 2;
    if (reps < 1 || reps > 20) { fprintf(stderr, "trial_posner: --reps takes 1..20\n"); return 2; }
    if (ppd > 0) snprintf(view_note, sizeof view_note, "%g px/deg (--ppd)", ppd);
    else {
        ppd = NOMINAL_PPD;
        snprintf(view_note, sizeof view_note, "NOMINAL (570 mm away, 0.27 mm a pixel, %.2f px/deg): sizes in deg are not visual angles", ppd);
    }
    seed0 = seed;

    /* the display, the stimuli in px from deg, and the text */
    if (!ygfx_open_screen(&gfx, &scr, &(yscr_desc){ .backend = sim ? YSCR_BACKEND_SIM : 0, .windowed = !fullscreen,
                                                    .window_w = 1024, .window_h = 768 },
                          &(ygfx_desc){ .background = { 0.5f, 0.5f, 0.5f } }))
        return fail(ygfx_error(&gfx));
    fix = shape(YGFX_CROSS, (float)(FIX_DEG * ppd), (float)(0.08 * ppd), 0, 0.1f);
    for (i = 0; i < 2; i++) {
        boxes[i] = shape(YGFX_RECT, (float)(BOX_DEG * ppd), 0, (float)(BOX_LINE_DEG * ppd), 0.25f);
        boxes[i].x = (float)((i ? 1 : -1) * ECC_DEG * ppd);
    }
    cue_box = shape(YGFX_RECT, (float)(BOX_DEG * ppd), 0, (float)(CUE_LINE_DEG * ppd), 1.0f);
    target = shape(YGFX_CIRCLE, (float)(TARGET_DEG * ppd), 0, 0, 0.05f);
    {   /* an arrow pointing right, from its center; ori 180 points it left */
        static const float v[14] = { -0.5f, -0.1f, 0.08f, -0.1f, 0.08f, -0.3f, 0.5f, 0, 0.08f, 0.3f, 0.08f, 0.1f, -0.5f, 0.1f };
        ygfx_shape_desc d;
        for (i = 0; i < 14; i++) arrow_v[i] = (float)(v[i] * ARROW_DEG * ppd);
        memset(&d, 0, sizeof d);
        d.shape = YGFX_POLYGON;
        d.shape_p[0] = 7;
        d.vertices = arrow_v;
        d.edge = YGFX_EDGE_COSINE;
        d.edge_width = 1;
        d.color[0] = d.color[1] = d.color[2] = 0.1f;
        arrow = ygfx_shape(&d);
    }
    if (!yol_open(&ol, NULL)) return fail("outline context");
    font_real = make_text(font_path, (float)(TEXT_DEG * ppd));
    if (font_real < 0) { fprintf(stderr, "trial_posner: text: %s %s\n", yol_error(&ol), ygfx_error(&gfx)); return 1; }
    if (!font_real) printf("%s did not open: the 5 x 7 bitmap capitals instead\n", font_path);
    ygfx_prime(&gfx);   /* the text's set made the curve program after the open's prime (docs/gfx.md) */

    /* the conditions table from DESIGN, and its weights as cond_reps */
    n = snprintf(conditions_csv, sizeof conditions_csv, "cue,validity,soa_s,cue_side\n");
    for (i = 0; i < N_ROWS; i++) {
        int c = i / 18, v = i / 6 % 3, s = i / 2 % 3, d = i % 2;
        n += snprintf(conditions_csv + n, sizeof conditions_csv - (size_t)n, "%s,%s,%g,%s\n", CUES[c], VALIDITY[v], SOA_S[s], SIDES[d]);
        cond_reps[i] = reps * use_cue[c] * W_CUE[c] * W_VALIDITY[v] * W_SOA[s] * W_SIDE[d];
        n_total += cond_reps[i];
    }
    memset(&cd, 0, sizeof cd);
    cd.text = conditions_csv;
    cd.len = (size_t)n;
    cd.arena = table_arena;
    cd.arena_size = sizeof table_arena;
    if (!ytb_open_csv(&tab, &cd)) return fail(ytb_error(&tab));
    cue_col = ytb_col(&tab, "cue");
    val_col = ytb_col(&tab, "validity");
    soa_col = ytb_col(&tab, "soa_s");
    side_col = ytb_col(&tab, "cue_side");
    memset(&td, 0, sizeof td);
    td.table = &tab;
    td.cond_reps = cond_reps;
    td.order = YTR_ORDER_CONSTRAINED;
    td.constraints[0] = ytr_max_run(val_col, ytb_find(&tab, val_col, "catch"), 1);
    td.n_constraints = 1;
    td.groups.mode = YTR_GROUPS_BLOCKED;
    td.groups.factor = cue_col;
    td.groups.order = YTR_GROUP_ORDER_RANDOM;
    td.jitters[0] = ytr_uniform("fix_s", YRT_S(FIX_LO_S), YRT_S(FIX_HI_S));
    td.n_jitters = 1;
    td.rng = ytr_splitmix;
    td.rng_ctx = &seed;
    if (!ytr_open(&trials, &td)) return fail(ytr_error(&trials));
    if (!ytl_open(&tl, &(ytl_desc){ .events = storage, .event_capacity = 64, .n_channels = N_CHANNELS }))
        return fail(ytl_error(&tl));
    if (!yrsp_open(&rsp, &(yrsp_desc){ .choices = loc ? localize : detect, .n_choices = loc ? 2 : 1,
                                       .duration_ns = YRT_S(DEADLINE_S) }))
        return fail(yrsp_error(&rsp));

    /* the data file, and the settings beside it */
    yscr_describe(&scr, line, sizeof line);
    snprintf(meta, sizeof meta, "screen=%s\nview=%s\nysp_response=%s\nresponse=%s\nkeyboard=%s\ntext=%s\nseed=%llu\n", line,
             view_note, yrsp_version(), loc ? "localize (left, right)" : "detect (space)",
             sim || autop ? "synthetic (tier SIM)" : "SDL through yscr_event_input", font_real ? font_path : "5 x 7 bitmap",
             (unsigned long long)seed0);
    snprintf(meta_path, sizeof meta_path, "%s.meta", out_path);
    if (!ytr_data_open(&data, &(ytr_data_desc){ .path = out_path, .trials = &trials, .cols = cols,
            .n_cols = (int)(sizeof cols / sizeof cols[0]), .meta_path = meta_path, .meta = meta }))
        return fail(ytr_data_error(&data));
    meas_col = ytr_data_col(&data, "soa_measured_s");
    printf("%s\n%d trials, %s; sizes: %s\n", line, max_trials && max_trials < n_total ? max_trials : n_total,
           loc ? "Left or Right for the target's side" : "space for the target", view_note);

    for (;;) {
        int64_t bt = 0;
        int rc = yscr_begin(&scr, &f), landed;
        if (rc == YSCR_QUIT) { aborted = 1; break; }
        if (rc != YSCR_OK) { failed = fail(yscr_error(&scr)); break; }
        /* the cue's and the target's flip records: their onsets, final */
        for (i = 0; i < f.n_done; i++) {
            yrsp_onset o = yrsp_onset_flip(&f.done[i]);
            if (!o.final) continue;
            if (f.done[i].index == cue_frame) { cue_t = o.t_ns; cue_tier = o.tier; }
            if (f.done[i].index == target_frame && (sim || autop) && responding) {
                /* the participant answers from the record, as RT is measured */
                if (is_catch && fa_trial < 0) { fa_trial = ti.index; sim_add(o.t_ns + YRT_S(SIM_FA_S), loc ? "left" : "space"); }
                else if (!is_catch) sim_add(o.t_ns + YRT_S(sim_rt(cue, valid, soa)), loc ? SIDES[tside] : "space");
            }
        }

        if (!in_trial) {
            ytl_seq q;
            if (max_trials && rows >= max_trials) break;
            if (ytr_next(&trials, &ti) < 0) break;
            cue = !strcmp(ytb_text(&tab, ti.condition, cue_col), "endo");
            valid = !strcmp(ytb_text(&tab, ti.condition, val_col), "valid");
            is_catch = !strcmp(ytb_text(&tab, ti.condition, val_col), "catch");
            for (soa = 0; soa < 2 && YRT_S(SOA_S[soa]) != YRT_S(ytb_num(&tab, ti.condition, soa_col)); soa++) {}
            side = !strcmp(ytb_text(&tab, ti.condition, side_col), "right");
            tside = valid || is_catch ? side : !side;
            if (ti.first_in_block) printf("block %d: %s cues\n", ti.block, CUES[cue]);
            cue_box.x = boxes[side].x;
            arrow.ori = side ? 0.0f : 180.0f;
            target.x = boxes[tside].x;
            /* the cue after the fixation, the target (or the empty frame) at the SOA from the cue's onset */
            ytl_clear(&tl, TRIAL);
            ytl_anchor(&tl, TRIAL, f.onset, 0);
            q = ytl_seq_on(&tl, TRIAL);
            ytl_wait(&q, ytr_jitter(&trials, ti.index, 0).ns);
            ytl_on(&q, cue ? ARROW : CUE_BOX);
            ytl_wait(&q, YRT_S(CUE_S));
            ytl_off(&q, cue ? ARROW : CUE_BOX);
            ytl_wait(&q, YRT_S(SOA_S[soa]) - YRT_S(CUE_S));
            ytl_on(&q, is_catch ? CATCH : TARGET);
            if (q.err) { failed = fail(ytl_strerror(q.err)); break; }
            yrsp_arm(&rsp, f.onset);   /* presses from the fixation on are logged; before the target, EARLY */
            cue_frame = target_frame = -1;
            cue_t = 0;
            in_trial = responding = 1;
            early = 0;
            n_sim_ev = sim_next = 0;
            if ((sim || autop) && early_trial < 0 && !is_catch && ti.index >= 1) {
                early_trial = ti.index;
                sim_add(f.onset + YRT_S(SIM_EARLY_S), loc ? SIDES[tside] : "space");
            }
        }

        yrsp_feed_screen(&scr, &rsp, 1);
        while (sim_next < n_sim_ev && sim_ev[sim_next].t <= f.onset) yrsp_feed(&rsp, &sim_ev[sim_next++]);

        /* A press of a choice before the target's onset: the trial stops
         * here with "too early". Checked before this frame's events land,
         * so a press polled now is before any onset this frame brings. */
        if (responding) {
            const yrsp_entry* e = yrsp_entries(&rsp, &n);
            for (i = 0; i < n && !early; i++) early = (e[i].flags & YRSP_E_EARLY) && !(e[i].flags & YRSP_E_NOT_CHOICE);
            if (early) {
                ytl_base_time(&tl, TRIAL, f.onset, &bt);
                ytl_clear(&tl, TRIAL);   /* the cue and the target that have not come yet go */
                if ((rc = end_trial(bt, -1, 1)) != 0) { failed = fail(ytl_strerror(rc)); break; }
                responding = 0;
                target_frame = -1;
            }
        }
        /* the target's flip record when it arrives; a response or the deadline ends it */
        if (yrsp_update_frame(&rsp, &f) == YRSP_ENDED && responding) {
            yrsp_finish(&rsp, &res);   /* again at the end: a late record still lands */
            ytl_base_time(&tl, TRIAL, f.onset, &bt);
            rc = end_trial(bt, is_catch ? CATCH : TARGET, is_catch && (res.flags & YRSP_R_RESPONDED));   /* a false alarm */
            if (rc != 0) { failed = fail(ytl_strerror(rc)); break; }
            responding = 0;
        }

        n = ytl_evaluate(&tl, &(ytl_frame){ f.onset, f.period, f.index }, fired, 64);
        for (i = 0; i < n; i++)   /* the cue landed: its record comes later */
            if (fired[i].kind == YTL_ONSET && (fired[i].target == CUE_BOX || fired[i].target == ARROW)) cue_frame = fired[i].frame;
        landed = yrsp_set_landing(&rsp, fired, n, is_catch ? CATCH : TARGET);   /* its planned onset, for now */
        if (landed >= 0) target_frame = fired[landed].frame;
        for (i = 0; i < n; i++)
            if (fired[i].kind == YTL_MARK && fired[i].code == END_OF_TRIAL) {
                int responded;
                int64_t meas = YTR_MISSING_NS;
                yrsp_finish(&rsp, &res);
                responded = (res.flags & YRSP_R_RESPONDED) != 0;
                r.target_side = is_catch ? NULL : SIDES[tside];
                r.catch_trial = is_catch;
                r.early = early;
                r.correct = early ? 0 : is_catch ? !responded
                          : responded && (!loc || !strcmp(res.response_name, SIDES[tside]));
                r.cue_tier = cue_t ? cue_tier : YTR_MISSING_INT;
                if (!early && cue_t && res.t_onset && !(res.flags & YRSP_R_ONSET_PLAN)) meas = res.t_onset - cue_t;
                ytr_update(&trials, early ? YTR_INVALID : r.correct, NULL);
                ytr_data_set_time_ns(&data, meas_col, meas);
                if (yrsp_data_set(&data, &res) < 0 || ytr_data_write(&data, &r) < 0) failed = fail(ytr_data_error(&data));
                printf("trial %3d %-4s %-7s %s soa %.1f s: ", ti.index, CUES[cue], VALIDITY[valid ? 0 : is_catch ? 2 : 1],
                       SIDES[side], SOA_S[soa]);
                if (early) printf("too early\n");
                else if (is_catch) printf("%s\n", responded ? "false alarm" : "no press, correct");
                else if (!responded) printf("too slow\n");
                else printf("rt %.3f s%s, soa shown %.4f s\n", YRT_TO_S(res.rt_ns), r.correct ? "" : ", wrong side", YRT_TO_S(meas));
                if (!early && !is_catch && r.correct && n_kept < YTR_MAX_TRIALS)
                    kept_rt[n_kept++] = (kept){ (int8_t)cue, (int8_t)soa, (int8_t)valid, res.rt_ns };
                if (!early && is_catch) { n_catch[cue]++; n_fa[cue] += responded; }
                n_early += early;
                rows++;
                in_trial = 0;
            }
        ygfx_apply(binds, (int)(sizeof binds / sizeof binds[0]), ytl_values(&tl));
        fix.visible = arrow.visible >= 0.5f ? 0.0f : 1.0f;   /* the arrow replaces the cross */
        yscr_mark(&scr, YSCR_PHASE_EVALUATE);
        ygfx_begin(&gfx, &f);
        ygfx_draw_n(&gfx, boxes, 2);
        ygfx_draw(&gfx, &cue_box);
        ygfx_draw(&gfx, &fix);
        ygfx_draw(&gfx, &arrow);
        ygfx_draw(&gfx, &target);
        ygfx_draw(&gfx, &text);
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
    if (ytr_data_finish(&data) < 0) failed = fail(ytr_data_error(&data));
    ygfx_close(&gfx);
    yscr_close(&scr);
    yol_close(&ol);
    printf("%d trials written to %s%s; %d too early\n", rows, out_path, aborted ? " (stopped with Shift+Esc)" : "", n_early);

    /* the validity effect per cue type and SOA, and the false alarms */
    printf("validity effect, invalid minus valid median RT (correct target trials):\n");
    for (k = 0; k < 2; k++)
        for (i = 0; i < 3; i++) {
            int nv, ni;
            int64_t mv = median(n_kept, k, i, 1, &nv), mi = median(n_kept, k, i, 0, &ni);
            effect[k][i] = nv && ni ? mi - mv : YRSP_NO_TIME;
            if (!nv && !ni) continue;
            printf("  %-4s soa %.1f s: valid %.3f s (%d), invalid %.3f s (%d), effect ", CUES[k], SOA_S[i],
                   nv ? YRT_TO_S(mv) : NAN, nv, ni ? YRT_TO_S(mi) : NAN, ni);
            if (nv && ni) printf("%+.1f ms\n", 1e3 * YRT_TO_S(effect[k][i]));
            else printf("-\n");
        }
    for (k = 0; k < 2; k++)
        if (n_catch[k])
            printf("catch trials, %-4s: %d false alarms of %d (%.3f)\n", CUES[k], n_fa[k], n_catch[k],
                   (double)n_fa[k] / n_catch[k]);

    if ((sim || autop) && !failed && !aborted) {
        const char* who = sim ? "--sim" : "--auto";
        /* the script's effects to the nanosecond: every RT of a cell is the
         * same. One early press and one false alarm, when their trials ran. */
        int want_rows = max_trials && max_trials < n_total ? max_trials : n_total, bad = rows != want_rows, nb, rt_c, cor_c, early_c, catch_c, meas_c, tier_c, ctier_c, idx_c, rsp_c, tside_c, flags_c;
        double soa_err = 0;
        if (bad) fprintf(stderr, "trial_posner: %s: %d rows, not %d\n", who, rows, want_rows);
        k = n_fa[0] + n_fa[1];
        if (k != (fa_trial >= 0) || n_early != (early_trial >= 0)) {
            fprintf(stderr, "trial_posner: %s: %d false alarms, %d too early (script: %d, %d)\n", who, k, n_early,
                    fa_trial >= 0, early_trial >= 0);
            bad = 1;
        }
        for (k = 0; k < 2; k++)
            for (i = 0; i < 3; i++)
                if (effect[k][i] != YRSP_NO_TIME && effect[k][i] != YRT_S(sim_rt(k, 0, i)) - YRT_S(sim_rt(k, 1, i))) {
                    fprintf(stderr, "trial_posner: %s: %s soa %.1f: effect %.9f s, script %.3f s\n", who, CUES[k], SOA_S[i],
                            YRT_TO_S(effect[k][i]), sim_rt(k, 0, i) - sim_rt(k, 1, i));
                    bad = 1;
                }
        /* every row of the file, read back, against the script */
        memset(&cd, 0, sizeof cd);
        cd.allow_empty = true;
        cd.arena = back_arena;
        cd.arena_size = sizeof back_arena;
        if (!ytb_open_csv_file(&back, out_path, &cd)) return fail(ytb_error(&back));
        idx_c = ytb_col(&back, "index"); cue_col = ytb_col(&back, "cue"); val_col = ytb_col(&back, "validity");
        soa_col = ytb_col(&back, "soa_s"); rt_c = ytb_col(&back, "rt_s"); cor_c = ytb_col(&back, "correct");
        early_c = ytb_col(&back, "early"); catch_c = ytb_col(&back, "catch_trial"); meas_c = ytb_col(&back, "soa_measured_s");
        tier_c = ytb_col(&back, "onset_tier"); ctier_c = ytb_col(&back, "cue_tier"); rsp_c = ytb_col(&back, "response");
        tside_c = ytb_col(&back, "target_side"); flags_c = ytb_col(&back, "rsp_flags");
        for (nb = i = 0; i < back.n_rows; i++) {
            int idx = ytb_int(&back, i, idx_c), c = !strcmp(ytb_text(&back, i, cue_col), "endo");
            int v = !strcmp(ytb_text(&back, i, val_col), "valid"), ca = (int)ytb_num(&back, i, catch_c);
            double plan = ytb_num(&back, i, soa_col), rt = ytb_num(&back, i, rt_c), meas = ytb_num(&back, i, meas_c), want;
            int s = plan < 0.2 ? 0 : plan < 0.5 ? 1 : 2, ok, cor = (int)ytb_num(&back, i, cor_c);
            int fl = (int)ytb_num(&back, i, flags_c);
            /* the early trial ended before the target: no onset, no deadline
             * ran, so no TIMEOUT (ysp/response.h v0.8) */
            if (idx == early_trial) {
                ok = ytb_num(&back, i, early_c) == 1 && cor == 0 && rt != rt && meas != meas && fl == YRSP_R_NO_ONSET;
                if (!ok) fprintf(stderr, "trial_posner: %s: the early trial's row (rsp_flags %d)\n", who, fl);
                nb += !ok;
                continue;
            }
            want = ca ? (idx == fa_trial ? SIM_FA_S : NAN) : sim_rt(c, v, s);
            ok = ytb_num(&back, i, early_c) == 0 && ca == !strcmp(ytb_text(&back, i, val_col), "catch") &&
                 (want == want ? fabs(rt - want) < 0.5e-9 : rt != rt) && cor == (ca ? idx != fa_trial : 1) &&
                 !(fl & YRSP_R_TIMEOUT) == (want == want) &&   /* the deadline ended every window without a press */
                 (!sim || (ytb_num(&back, i, tier_c) == YSCR_TIER_SIM && ytb_num(&back, i, ctier_c) == YSCR_TIER_SIM));
            if (loc && !ca) ok = ok && !strcmp(ytb_text(&back, i, rsp_c), ytb_text(&back, i, tside_c));
            if (!(fabs(meas - plan) <= soa_err)) soa_err = meas == meas ? fabs(meas - plan) : INFINITY;   /* NaN: missing */
            if (!ok) {
                fprintf(stderr, "trial_posner: %s: row %d (trial %d %s %s soa %.1f): rt %.9f correct %d\n", who, i, idx,
                        CUES[c], ytb_text(&back, i, val_col), plan, rt, cor);
                nb++;
            }
        }
        /* on the virtual clock no frame slips: the SOAs are the plan's on the
         * frame grid (within 1 us of a 16666667 ns period's multiples) */
        if (sim && soa_err > 1e-6) { fprintf(stderr, "trial_posner: %s: an SOA %.9f s off its plan\n", who, soa_err); nb++; }
        if (!sim) printf("%s: the largest SOA error %.6f s\n", who, soa_err);
        if (back.n_rows != rows) { fprintf(stderr, "trial_posner: %s: %d rows read back, %d written\n", who, back.n_rows, rows); nb++; }
        if (bad || nb) return 1;
        printf("%s: every row, effect and false alarm came out as scripted\n", who);
    }
    return failed;
}
