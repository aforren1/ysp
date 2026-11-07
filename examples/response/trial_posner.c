/* trial_posner.c - spatial cueing (Posner 1980) as a small experiment:
 * ysp/trials.h, ysp/timeline.h, ysp/gfx.h, ysp/outline.h and
 * ysp/response.h.
 *
 * A fixation cross and two outlined boxes, 8 deg left and right, on
 * mid-gray. Each trial:
 *   fixation   1.0 to 1.5 s (uniform)
 *   cue        0.1 s: the cued box brightens (exogenous block) or an arrow
 *              replaces the cross (endogenous block)
 *   target     a dark disc in a box, 0.1, 0.3 or 0.7 s after the cue's
 *              onset, until the response or 1.5 s
 *   ITI        1 s
 * 80 % of target trials are valid, 20 % invalid; catch trials (10 % on
 * top) have no target. Space detects the target; with --localize, Left or
 * Right gives its side. A press before the target, or on a catch trial,
 * shows "too early" for 0.5 s.
 *
 * Sizes are in deg through --ppd (pixels per degree). Without it the
 * geometry is nominal (36.8 px/deg), flagged NOMINAL in FILE.meta:
 * ysp/screen.h does not report the screen's size.
 *
 * Data, one row per trial (ysp/trials.h's data writer): the session's
 * columns (index, block, ..., cue, validity, soa_s, cue_side, fix_s), then
 * target_side, catch_trial, early, correct, soa_measured_s (the target's
 * flip onset minus the cue's), cue_tier, then YRSP_DATA_COLS (response,
 * rt_s from the target's flip onset, ...). Settings in FILE.meta. At the
 * end: the validity effect (invalid minus valid median RT) per cue type
 * and SOA, and the false-alarm rate. Shift+Esc stops; the rows so far are
 * kept.
 *
 * Usage: trial_posner [--sim | --auto] [--localize] [--cue exo|endo|both] [--reps N]
 *          [--trials N] [--ppd PX] [--fullscreen] [--seed N] [--out FILE] [--font PATH]
 *   --sim       the simulated display and the virtual clock, with a
 *               scripted participant; checks every row and effect
 *   --auto      the same participant in a window
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

/* CATCH draws nothing: a catch trial's "target" event, so its window and
 * deadline run from that frame's flip record as a target's would */
enum { CUE_BOX, ARROW, TARGET, CATCH, FEEDBACK, N_CHANNELS };
enum { TRIAL = 1 };
enum { END_OF_TRIAL = 1 };
#define N_ROWS (2 * 3 * 3 * 2)

#ifdef _WIN32
#define DEFAULT_FONT "C:/Windows/Fonts/arial.ttf"
#else
#define DEFAULT_FONT "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
#endif

/* The program's columns, after the session's; then YRSP_DATA_COLS. */
typedef struct row { const char* target_side; bool catch_trial, early, correct; int cue_tier; } row;
static const ytr_col cols[] = {
    YTR_COL(row, target_side, YTR_COL_STRING), YTR_COL(row, catch_trial, YTR_COL_BOOL), YTR_COL(row, early, YTR_COL_BOOL),
    YTR_COL(row, correct, YTR_COL_BOOL), YTR_COL_SET("soa_measured_s", YTR_COL_TIME_NS), YTR_COL(row, cue_tier, YTR_COL_INT),
    YRSP_DATA_COLS,
};

/* A trial's condition, as indices into DESIGN; tside is the target's side */
typedef struct condition { int cue, valid, is_catch, soa, side, tside; } condition;

/* The scripted participant and the self-checks of --sim and --auto, at the
 * end of the file. Without sim_begin() each call does nothing. */
typedef struct tally tally;
static void sim_begin(bool virtual_clock, bool localize, const char* out_path);
static void sim_trial_start(int64_t now, int trial, const condition* c);
static void sim_target_seen(int64_t onset, int trial, const condition* c);
static void sim_feed(yrsp_collector* c, int64_t now);
static int sim_check_end(int n_rows, int want_rows, const tally* t);

static yscr_screen scr;
static ygfx_gfx gfx;
static yol_ctx ol;
static ytl_timeline tl;
static ytl_event storage[64];
static ytr_trials trials;
static ytr_data data;
static yrsp_collector rsp;
static uint64_t table_arena[2048];
static ytb_table tab;
static char conditions_csv[2048];
static int cond_reps[N_ROWS];

static ygfx_stim fix, boxes[2], cue_box, arrow, target, text;
static ygfx_citem text_item;
static float arrow_v[14];
static const ygfx_bind binds[] = {
    { .stim = &cue_box, .param = YGFX_P_VISIBLE, .channel = CUE_BOX },
    { .stim = &arrow, .param = YGFX_P_VISIBLE, .channel = ARROW },
    { .stim = &target, .param = YGFX_P_VISIBLE, .channel = TARGET },
    { .stim = &text, .param = YGFX_P_VISIBLE, .channel = FEEDBACK },
};

/* The options */
static struct {
    bool sim, autorun, loc, fullscreen;
    bool use_cue[2];
    int reps, max_trials;
    double ppd;
    uint64_t seed;
    const char *out_path, *font_path;
    char view_note[160];
} opt = { .use_cue = { 1, 1 }, .seed = 20261106, .out_path = "trial_posner.csv", .font_path = DEFAULT_FONT };

/* Where the run is: the trial, its records, and the summary's tallies */
static ytr_trial_info ti;
static condition cond;
static int cue_col, val_col, soa_col, side_col, meas_col, font_real, n_total, rows, responding, early;
static int64_t cue_frame = -1, target_frame = -1, cue_t;
static uint8_t cue_tier;
typedef struct kept { int8_t cue, soa, valid; int64_t rt; } kept;   /* a correct target trial's RT */
struct tally {
    kept rt[YTR_MAX_TRIALS];
    int n_kept, n_catch[2], n_fa[2], n_early;
    int64_t effect[2][3];   /* invalid minus valid median RT per cue and SOA */
};
static tally stats;
static int64_t cell[YTR_MAX_TRIALS];

static bool fail(const char* why) {
    fprintf(stderr, "trial_posner: %s\n", why);
    return false;
}

static int parse_options(int argc, char** argv) {
    const char* cue_arg = "both";
    int i;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--sim")) opt.sim = 1;
        else if (!strcmp(argv[i], "--auto")) opt.autorun = 1;
        else if (!strcmp(argv[i], "--localize")) opt.loc = 1;
        else if (!strcmp(argv[i], "--fullscreen")) opt.fullscreen = 1;
        else if (!strcmp(argv[i], "--cue") && i + 1 < argc) cue_arg = argv[++i];
        else if (!strcmp(argv[i], "--reps") && i + 1 < argc) opt.reps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--trials") && i + 1 < argc) opt.max_trials = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--ppd") && i + 1 < argc) opt.ppd = atof(argv[++i]);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) opt.seed = (uint64_t)strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) opt.out_path = argv[++i];
        else if (!strcmp(argv[i], "--font") && i + 1 < argc) opt.font_path = argv[++i];
        else break;
    }
    if (!strcmp(cue_arg, "exo")) opt.use_cue[1] = 0;
    else if (!strcmp(cue_arg, "endo")) opt.use_cue[0] = 0;
    else if (strcmp(cue_arg, "both")) i = 0;
    if (i < argc || (opt.sim && opt.autorun) || opt.max_trials < 0 || (opt.ppd != 0 && !(opt.ppd >= 1 && opt.ppd <= 1000))) {
        fprintf(stderr, "usage: trial_posner [--sim | --auto] [--localize] [--cue exo|endo|both] [--reps N] [--trials N]\n"
                        "                    [--ppd PX] [--fullscreen] [--seed N] [--out FILE] [--font PATH]\n");
        return 2;
    }
    if (opt.reps == 0) opt.reps = opt.sim ? 1 : 2;
    if (opt.reps < 1 || opt.reps > 20) { fprintf(stderr, "trial_posner: --reps takes 1..20\n"); return 2; }
    if (opt.ppd > 0) snprintf(opt.view_note, sizeof opt.view_note, "%g px/deg (--ppd)", opt.ppd);
    else {
        opt.ppd = NOMINAL_PPD;
        snprintf(opt.view_note, sizeof opt.view_note,
                 "NOMINAL (570 mm away, 0.27 mm a pixel, %.2f px/deg): sizes in deg are not visual angles", opt.ppd);
    }
    return 0;
}

static ygfx_stim shape(ygfx_shape_kind kind, float w, float p0, float stroke, float gray) {
    return ygfx_shape(&(ygfx_shape_desc){ .shape = kind, .w = w, .h = w, .shape_p = { p0 }, .stroke = stroke,
                                          .edge = YGFX_EDGE_COSINE, .edge_width = 1, .color = { gray, gray, gray } });
}

/* The cross, the boxes, the cue's box, the target and the arrow, in px
 * from deg */
static void make_stimuli(double ppd) {
    /* an arrow pointing right, from its center; ori 180 points it left */
    static const float v[14] = { -0.5f, -0.1f, 0.08f, -0.1f, 0.08f, -0.3f, 0.5f, 0, 0.08f, 0.3f, 0.08f, 0.1f, -0.5f, 0.1f };
    int i;
    fix = shape(YGFX_CROSS, (float)(FIX_DEG * ppd), (float)(0.08 * ppd), 0, 0.1f);
    for (i = 0; i < 2; i++) {
        boxes[i] = shape(YGFX_RECT, (float)(BOX_DEG * ppd), 0, (float)(BOX_LINE_DEG * ppd), 0.25f);
        boxes[i].x = (float)((i ? 1 : -1) * ECC_DEG * ppd);
    }
    cue_box = shape(YGFX_RECT, (float)(BOX_DEG * ppd), 0, (float)(CUE_LINE_DEG * ppd), 1.0f);
    target = shape(YGFX_CIRCLE, (float)(TARGET_DEG * ppd), 0, 0, 0.05f);
    for (i = 0; i < 14; i++) arrow_v[i] = (float)(v[i] * ARROW_DEG * ppd);
    arrow = ygfx_shape(&(ygfx_shape_desc){ .shape = YGFX_POLYGON, .shape_p = { 7 }, .vertices = arrow_v,
                                           .edge = YGFX_EDGE_COSINE, .edge_width = 1, .color = { 0.1f, 0.1f, 0.1f } });
}

/* The letters of "too early" as 5 x 7 capitals (gfx_gallery.c's), in the
 * order of BM_LETTERS, for when the font does not open. */
static const char BM_LETTERS[] = "toearly";
static const uint8_t bm[7][7] = {
    { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 }, { 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E },
    { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F }, { 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 },
    { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 }, { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F },
    { 0x11, 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04 },
};

/* A whole file in memory, NULL when it does not open */
static uint8_t* read_file(const char* path, long* n) {
    uint8_t* bytes = NULL;
    FILE* fp = fopen(path, "rb");
    *n = 0;
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END); *n = ftell(fp); fseek(fp, 0, SEEK_SET);
    bytes = *n > 0 ? (uint8_t*)malloc((size_t)*n) : NULL;
    if (bytes && fread(bytes, 1, (size_t)*n, fp) != (size_t)*n) { free(bytes); bytes = NULL; }
    fclose(fp);
    return bytes;
}

/* "too early" as one path, glyph 0 of a curve set: from the font when it
 * opens (returns 1), else from the bitmap capitals (0); -1 on a failure. */
static int make_text(const char* path, float px_per_em) {
    static const char msg[] = "too early";
    yol_cset s;
    yol_path p;
    yol_font font;
    ygfx_cset set = { 0 };
    long n;
    double pen = 0, adv, lsb;
    int real = 0, rc = YOL_OK, i, r, c;
    uint8_t* bytes = read_file(path, &n);
    if (bytes && yol_font_view(&font, bytes, (size_t)n, 0, NULL, 0) == YOL_OK) real = 1;
    if (!yol_cset_open(&s, &ol, &(yol_cset_desc){ .n_glyphs = 1 })) { free(bytes); return -1; }
    yol_path_init(&p, &ol);
    for (i = 0; msg[i] && rc == YOL_OK; i++) {
        if (real) {
            uint32_t g = yol_font_glyph_index(&font, (uint32_t)msg[i]);
            rc = yol_font_glyph(&ol, &font, g, &p, &(yol_glyph_desc){ .x = pen });
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
    if (rc == YOL_OK)
        set = ygfx_cset_make(&gfx, &(ygfx_cset_desc){ .texels = s.texels, .n_texels = s.n_texels, .words = s.words,
                                                       .n_words = s.n_words });
    yol_path_free(&p);
    yol_cset_close(&s);
    free(bytes);
    if (rc != YOL_OK || !set.id) return -1;
    text_item = (ygfx_citem){ .scale = 1, .gate = 1 };
    text = ygfx_crun(&gfx, &(ygfx_crun_desc){ .set = set, .items = &text_item, .n = 1, .size = px_per_em,
                                               .y = -2.0f * px_per_em,   /* above the fixation */
                                               .color = { 0.05f, 0.05f, 0.05f } });
    return real;
}

/* The conditions table from DESIGN, its weights as cond_reps, and the
 * session: the cue type a BLOCKED group in random order, no two catch
 * trials back to back, the fixation's duration the jitter fix_s. */
static bool build_design(void) {
    int i, n = snprintf(conditions_csv, sizeof conditions_csv, "cue,validity,soa_s,cue_side\n");
    for (i = 0; i < N_ROWS; i++) {
        int c = i / 18, v = i / 6 % 3, s = i / 2 % 3, d = i % 2;
        n += snprintf(conditions_csv + n, sizeof conditions_csv - (size_t)n, "%s,%s,%g,%s\n", CUES[c], VALIDITY[v], SOA_S[s], SIDES[d]);
        cond_reps[i] = opt.reps * opt.use_cue[c] * W_CUE[c] * W_VALIDITY[v] * W_SOA[s] * W_SIDE[d];
        n_total += cond_reps[i];
    }
    if (!ytb_open_csv(&tab, &(ytb_csv_desc){ .text = conditions_csv, .len = (size_t)n, .arena = table_arena,
                                             .arena_size = sizeof table_arena }))
        return fail(ytb_error(&tab));
    cue_col = ytb_col(&tab, "cue");
    val_col = ytb_col(&tab, "validity");
    soa_col = ytb_col(&tab, "soa_s");
    side_col = ytb_col(&tab, "cue_side");
    return ytr_open(&trials, &(ytr_desc){
               .table = &tab, .cond_reps = cond_reps, .order = YTR_ORDER_CONSTRAINED,
               .constraints = { ytr_max_run(val_col, ytb_find(&tab, val_col, "catch"), 1) }, .n_constraints = 1,
               .groups = { .mode = YTR_GROUPS_BLOCKED, .factor = cue_col, .order = YTR_GROUP_ORDER_RANDOM },
               .jitters = { ytr_uniform("fix_s", YRT_S(FIX_LO_S), YRT_S(FIX_HI_S)) }, .n_jitters = 1,
               .rng = ytr_splitmix, .rng_ctx = &opt.seed }) ||
           fail(ytr_error(&trials));
}

/* The display, the stimuli, the text, the design, the timeline and the
 * collector: Space (or Left and Right), a deadline of 1.5 s from the
 * target's onset */
static bool open_task(void) {
    static const yrsp_choice detect[] = { { .key = "space" } }, localize[] = { { .key = "left" }, { .key = "right" } };
    if (!ygfx_open_screen(&gfx, &scr, &(yscr_desc){ .backend = opt.sim ? YSCR_BACKEND_SIM : 0, .windowed = !opt.fullscreen,
                                                    .window_w = 1024, .window_h = 768 },
                          &(ygfx_desc){ .background = { 0.5f, 0.5f, 0.5f } }))
        return fail(ygfx_error(&gfx));
    make_stimuli(opt.ppd);
    if (!yol_open(&ol, NULL)) return fail("outline context");
    font_real = make_text(opt.font_path, (float)(TEXT_DEG * opt.ppd));
    if (font_real < 0) { fprintf(stderr, "trial_posner: text: %s %s\n", yol_error(&ol), ygfx_error(&gfx)); return false; }
    if (!font_real) printf("%s did not open: the 5 x 7 bitmap capitals instead\n", opt.font_path);
    ygfx_prime(&gfx);   /* the text's set made the curve program after the open's prime (docs/gfx.md) */
    if (!build_design()) return false;
    if (!ytl_open(&tl, &(ytl_desc){ .events = storage, .event_capacity = 64, .n_channels = N_CHANNELS }))
        return fail(ytl_error(&tl));
    if (!yrsp_open(&rsp, &(yrsp_desc){ .choices = opt.loc ? localize : detect, .n_choices = opt.loc ? 2 : 1,
                                       .duration_ns = YRT_S(DEADLINE_S) }))
        return fail(yrsp_error(&rsp));
    return true;
}

/* The data file, and the settings beside it in FILE.meta */
static bool open_data(uint64_t seed0) {
    char line[1024], meta[2048], meta_path[1100];
    yscr_describe(&scr, line, sizeof line);
    snprintf(meta, sizeof meta, "screen=%s\nview=%s\nysp_response=%s\nresponse=%s\nkeyboard=%s\ntext=%s\nseed=%llu\n", line,
             opt.view_note, yrsp_version(), opt.loc ? "localize (left, right)" : "detect (space)",
             opt.sim || opt.autorun ? "synthetic (tier SIM)" : "SDL through yscr_event_input",
             font_real ? opt.font_path : "5 x 7 bitmap", (unsigned long long)seed0);
    snprintf(meta_path, sizeof meta_path, "%s.meta", opt.out_path);
    if (!ytr_data_open(&data, &(ytr_data_desc){ .path = opt.out_path, .trials = &trials, .cols = cols,
            .n_cols = (int)(sizeof cols / sizeof cols[0]), .meta_path = meta_path, .meta = meta }))
        return fail(ytr_data_error(&data));
    meas_col = ytr_data_col(&data, "soa_measured_s");
    printf("%s\n%d trials, %s; sizes: %s\n", line, opt.max_trials && opt.max_trials < n_total ? opt.max_trials : n_total,
           opt.loc ? "Left or Right for the target's side" : "space for the target", opt.view_note);
    return true;
}

/* The next trial's condition, places and timeline: the cue after the
 * fixation, the target (or CATCH) at the SOA from the cue's onset */
static bool start_trial(const yscr_frame* f) {
    ytl_seq q;
    cond.cue = !strcmp(ytb_text(&tab, ti.condition, cue_col), "endo");
    cond.valid = !strcmp(ytb_text(&tab, ti.condition, val_col), "valid");
    cond.is_catch = !strcmp(ytb_text(&tab, ti.condition, val_col), "catch");
    for (cond.soa = 0; cond.soa < 2 && YRT_S(SOA_S[cond.soa]) != YRT_S(ytb_num(&tab, ti.condition, soa_col)); cond.soa++) {}
    cond.side = !strcmp(ytb_text(&tab, ti.condition, side_col), "right");
    cond.tside = cond.valid || cond.is_catch ? cond.side : !cond.side;
    if (ti.first_in_block) printf("block %d: %s cues\n", ti.block, CUES[cond.cue]);
    cue_box.x = boxes[cond.side].x;
    arrow.ori = cond.side ? 0.0f : 180.0f;
    target.x = boxes[cond.tside].x;
    ytl_clear(&tl, TRIAL);
    ytl_anchor(&tl, TRIAL, f->onset, 0);
    q = ytl_seq_on(&tl, TRIAL);
    ytl_wait(&q, ytr_jitter(&trials, ti.index, 0).ns);
    ytl_on(&q, cond.cue ? ARROW : CUE_BOX);
    ytl_wait(&q, YRT_S(CUE_S));
    ytl_off(&q, cond.cue ? ARROW : CUE_BOX);
    ytl_wait(&q, YRT_S(SOA_S[cond.soa]) - YRT_S(CUE_S));
    ytl_on(&q, cond.is_catch ? CATCH : TARGET);
    if (q.err) return fail(ytl_strerror(q.err));
    yrsp_arm(&rsp, f->onset);   /* presses from the fixation on are logged; before the target, EARLY */
    cue_frame = target_frame = -1;
    cue_t = 0;
    responding = 1;
    early = 0;
    sim_trial_start(f->onset, ti.index, &cond);
    return true;
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

/* The ITI's end: the result, with the target's final onset, as a row */
static bool write_row(void) {
    yrsp_result res;
    row r;
    int responded;
    int64_t meas = YTR_MISSING_NS;
    bool ok = true;
    yrsp_finish(&rsp, &res);
    responded = (res.flags & YRSP_R_RESPONDED) != 0;
    r.target_side = cond.is_catch ? NULL : SIDES[cond.tside];
    r.catch_trial = cond.is_catch;
    r.early = early;
    r.correct = early ? 0 : cond.is_catch ? !responded
              : responded && (!opt.loc || !strcmp(res.response_name, SIDES[cond.tside]));
    r.cue_tier = cue_t ? cue_tier : YTR_MISSING_INT;
    if (!early && cue_t && res.t_onset && !(res.flags & YRSP_R_ONSET_PLAN)) meas = res.t_onset - cue_t;
    ytr_update(&trials, early ? YTR_INVALID : r.correct, NULL);
    ytr_data_set_time_ns(&data, meas_col, meas);
    if (yrsp_data_set(&data, &res) < 0 || ytr_data_write(&data, &r) < 0) ok = fail(ytr_data_error(&data));
    printf("trial %3d %-4s %-7s %s soa %.1f s: ", ti.index, CUES[cond.cue],
           VALIDITY[cond.valid ? 0 : cond.is_catch ? 2 : 1], SIDES[cond.side], SOA_S[cond.soa]);
    if (early) printf("too early\n");
    else if (cond.is_catch) printf("%s\n", responded ? "false alarm" : "no press, correct");
    else if (!responded) printf("too slow\n");
    else printf("rt %.3f s%s, soa shown %.4f s\n", YRT_TO_S(res.rt_ns), r.correct ? "" : ", wrong side", YRT_TO_S(meas));
    if (!early && !cond.is_catch && r.correct && stats.n_kept < YTR_MAX_TRIALS)
        stats.rt[stats.n_kept++] = (kept){ (int8_t)cond.cue, (int8_t)cond.soa, (int8_t)cond.valid, res.rt_ns };
    if (!early && cond.is_catch) { stats.n_catch[cond.cue]++; stats.n_fa[cond.cue] += responded; }
    stats.n_early += early;
    rows++;
    return ok;
}

static int cmp_i64(const void* a, const void* b) {
    int64_t x = *(const int64_t*)a, y = *(const int64_t*)b;
    return x < y ? -1 : x > y;
}
/* the median RT of a cell, YRSP_NO_TIME when it is empty */
static int64_t median(int cue, int soa, int valid, int* n_out) {
    int i, n = 0;
    for (i = 0; i < stats.n_kept; i++)
        if (stats.rt[i].cue == cue && stats.rt[i].soa == soa && stats.rt[i].valid == valid) cell[n++] = stats.rt[i].rt;
    *n_out = n;
    if (!n) return YRSP_NO_TIME;
    qsort(cell, (size_t)n, sizeof cell[0], cmp_i64);
    return n % 2 ? cell[n / 2] : (cell[n / 2 - 1] + cell[n / 2]) / 2;
}

/* The validity effect per cue type and SOA, and the false alarms */
static void print_summary(void) {
    int i, k;
    printf("validity effect, invalid minus valid median RT (correct target trials):\n");
    for (k = 0; k < 2; k++)
        for (i = 0; i < 3; i++) {
            int nv, ni;
            int64_t mv = median(k, i, 1, &nv), mi = median(k, i, 0, &ni);
            stats.effect[k][i] = nv && ni ? mi - mv : YRSP_NO_TIME;
            if (!nv && !ni) continue;
            printf("  %-4s soa %.1f s: valid %.3f s (%d), invalid %.3f s (%d), effect ", CUES[k], SOA_S[i],
                   nv ? YRT_TO_S(mv) : NAN, nv, ni ? YRT_TO_S(mi) : NAN, ni);
            if (nv && ni) printf("%+.1f ms\n", 1e3 * YRT_TO_S(stats.effect[k][i]));
            else printf("-\n");
        }
    for (k = 0; k < 2; k++)
        if (stats.n_catch[k])
            printf("catch trials, %-4s: %d false alarms of %d (%.3f)\n", CUES[k], stats.n_fa[k], stats.n_catch[k],
                   (double)stats.n_fa[k] / stats.n_catch[k]);
}

int main(int argc, char** argv) {
    char end[1100];
    uint64_t seed0;
    int i, n, rc, in_trial = 0, aborted = 0, failed = 0;

    if ((rc = parse_options(argc, argv)) != 0) return rc;
    if (opt.sim) yrt_virtual_begin(0);   /* ysp/rt.h's virtual clock: the same records on any host */
    if (opt.sim || opt.autorun) sim_begin(opt.sim, opt.loc, opt.out_path);
    seed0 = opt.seed;   /* for FILE.meta: the session advances opt.seed */
    if (!open_task() || !open_data(seed0)) return 1;

    for (;;) {
        yscr_frame f;
        ytl_event fired[64];   /* the timeline's capacity: all that can fire */
        yrsp_result res;
        int64_t bt = 0;
        int landed;
        rc = yscr_begin(&scr, &f);
        if (rc == YSCR_QUIT) { aborted = 1; break; }
        if (rc != YSCR_OK) { fail(yscr_error(&scr)); failed = 1; break; }
        /* the cue's and the target's flip records: their onsets, final */
        for (i = 0; i < f.n_done; i++) {
            yrsp_onset o = yrsp_onset_flip(&f.done[i]);
            if (!o.final) continue;
            if (f.done[i].index == cue_frame) { cue_t = o.t_ns; cue_tier = o.tier; }
            if (f.done[i].index == target_frame && responding) sim_target_seen(o.t_ns, ti.index, &cond);
        }
        if (!in_trial) {
            if ((opt.max_trials && rows >= opt.max_trials) || ytr_next(&trials, &ti) < 0) break;
            if (!start_trial(&f)) { failed = 1; break; }
            in_trial = 1;
        }

        yrsp_feed_screen(&scr, &rsp, 1);
        sim_feed(&rsp, f.onset);
        /* A press of a choice before the target's onset: the trial stops
         * here with "too early". Checked before this frame's events land,
         * so a press polled now is before any onset this frame brings. */
        if (responding) {
            const yrsp_entry* e = yrsp_entries(&rsp, &n);
            for (i = 0; i < n && !early; i++) early = (e[i].flags & YRSP_E_EARLY) && !(e[i].flags & YRSP_E_NOT_CHOICE);
            if (early) {
                ytl_base_time(&tl, TRIAL, f.onset, &bt);
                ytl_clear(&tl, TRIAL);   /* the cue and the target that have not come yet go */
                if ((rc = end_trial(bt, -1, 1)) != 0) { fail(ytl_strerror(rc)); failed = 1; break; }
                responding = 0;
                target_frame = -1;
            }
        }
        /* the target's flip record when it arrives; a response or the deadline ends it */
        if (yrsp_update_frame(&rsp, &f) == YRSP_ENDED && responding) {
            yrsp_finish(&rsp, &res);   /* again at the end: a late record still lands */
            ytl_base_time(&tl, TRIAL, f.onset, &bt);
            rc = end_trial(bt, cond.is_catch ? CATCH : TARGET, cond.is_catch && (res.flags & YRSP_R_RESPONDED));   /* a false alarm */
            if (rc != 0) { fail(ytl_strerror(rc)); failed = 1; break; }
            responding = 0;
        }

        n = ytl_evaluate(&tl, &(ytl_frame){ f.onset, f.period, f.index }, fired, 64);
        for (i = 0; i < n; i++)   /* the cue landed: its record comes later */
            if (fired[i].kind == YTL_ONSET && (fired[i].target == CUE_BOX || fired[i].target == ARROW)) cue_frame = fired[i].frame;
        landed = yrsp_set_landing(&rsp, fired, n, cond.is_catch ? CATCH : TARGET);   /* its planned onset, for now */
        if (landed >= 0) target_frame = fired[landed].frame;
        for (i = 0; i < n; i++)
            if (fired[i].kind == YTL_MARK && fired[i].code == END_OF_TRIAL) {
                if (!write_row()) failed = 1;
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

    /* the describe line again: its counters (key times, raw mice) are 0 at open */
    memcpy(end, "screen_end=", 11);
    yscr_describe(&scr, end + 11, sizeof end - 11);
    if ((rc = ytr_data_meta(&data, end)) < 0 && rc != YTR_ERR_CLOSED) fprintf(stderr, "%s\n", ytr_data_error(&data));
    if (ytr_data_finish(&data) < 0) { fail(ytr_data_error(&data)); failed = 1; }
    ygfx_close(&gfx);
    yscr_close(&scr);
    yol_close(&ol);
    printf("%d trials written to %s%s; %d too early\n", rows, opt.out_path, aborted ? " (stopped with Shift+Esc)" : "",
           stats.n_early);
    print_summary();
    if (failed || aborted) return failed;
    return sim_check_end(rows, opt.max_trials && opt.max_trials < n_total ? opt.max_trials : n_total, &stats);
}

/* ---- The scripted participant and the self-checks (--sim, --auto) -------
 *
 * RT 0.300 s valid and 0.340 s invalid, except exogenous at 0.7 s (0.330
 * valid, 0.300 invalid: inhibition of return) and endogenous at 0.1 s
 * (0.320 and 0.330); one early press during a fixation, one false alarm
 * 0.25 s into the first catch trial. It answers from the target's flip
 * record, as RT is measured, so it also runs in a window (--auto). The
 * checks: the printed effects and the false-alarm rate against the
 * script, then every row of the data file, read back. */
#define SIM_EARLY_S 0.6   /* from the trial's start: inside the shortest fixation */
#define SIM_FA_S    0.25  /* from the catch trial's empty frame */

static bool sim_on, sim_virtual, sim_loc;
static const char* sim_out_path;
static yrsp_input sim_ev[4];
static int n_sim_ev, sim_next, early_trial = -1, fa_trial = -1;
static uint64_t back_arena[1 << 16];
static ytb_table back;

static double sim_rt(int cue, int valid, int soa) {
    if (cue == 0 && soa == 2) return valid ? 0.330 : 0.300;   /* inhibition of return */
    if (cue == 1 && soa == 0) return valid ? 0.320 : 0.330;   /* an arrow takes time to use */
    return valid ? 0.300 : 0.340;
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

static void sim_begin(bool virtual_clock, bool localize, const char* out_path) {
    sim_on = true;
    sim_virtual = virtual_clock;
    sim_loc = localize;
    sim_out_path = out_path;
}

/* the early press: the first target trial after trial 0 */
static void sim_trial_start(int64_t now, int trial, const condition* c) {
    n_sim_ev = sim_next = 0;
    if (sim_on && early_trial < 0 && !c->is_catch && trial >= 1) {
        early_trial = trial;
        sim_add(now + YRT_S(SIM_EARLY_S), sim_loc ? SIDES[c->tside] : "space");
    }
}

static void sim_target_seen(int64_t onset, int trial, const condition* c) {
    if (!sim_on) return;
    if (c->is_catch && fa_trial < 0) { fa_trial = trial; sim_add(onset + YRT_S(SIM_FA_S), sim_loc ? "left" : "space"); }
    else if (!c->is_catch) sim_add(onset + YRT_S(sim_rt(c->cue, c->valid, c->soa)), sim_loc ? SIDES[c->tside] : "space");
}

static void sim_feed(yrsp_collector* c, int64_t now) {
    while (sim_next < n_sim_ev && sim_ev[sim_next].t <= now) yrsp_feed(c, &sim_ev[sim_next++]);
}

/* every row of the file, read back, against the script; returns the
 * number of bad rows, the largest SOA error in *soa_err */
static int sim_check_rows(const char* who, double* soa_err) {
    int i, nb = 0, idx_c, cue_c, val_c, soa_c, rt_c, cor_c, early_c, catch_c, meas_c, tier_c, ctier_c, rsp_c, tside_c, flags_c;
    idx_c = ytb_col(&back, "index"); cue_c = ytb_col(&back, "cue"); val_c = ytb_col(&back, "validity");
    soa_c = ytb_col(&back, "soa_s"); rt_c = ytb_col(&back, "rt_s"); cor_c = ytb_col(&back, "correct");
    early_c = ytb_col(&back, "early"); catch_c = ytb_col(&back, "catch_trial"); meas_c = ytb_col(&back, "soa_measured_s");
    tier_c = ytb_col(&back, "onset_tier"); ctier_c = ytb_col(&back, "cue_tier"); rsp_c = ytb_col(&back, "response");
    tside_c = ytb_col(&back, "target_side"); flags_c = ytb_col(&back, "rsp_flags");
    for (i = 0; i < back.n_rows; i++) {
        int idx = ytb_int(&back, i, idx_c), c = !strcmp(ytb_text(&back, i, cue_c), "endo");
        int v = !strcmp(ytb_text(&back, i, val_c), "valid"), ca = (int)ytb_num(&back, i, catch_c);
        double plan = ytb_num(&back, i, soa_c), rt = ytb_num(&back, i, rt_c), meas = ytb_num(&back, i, meas_c), want;
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
        ok = ytb_num(&back, i, early_c) == 0 && ca == !strcmp(ytb_text(&back, i, val_c), "catch") &&
             (want == want ? fabs(rt - want) < 0.5e-9 : rt != rt) && cor == (ca ? idx != fa_trial : 1) &&
             !(fl & YRSP_R_TIMEOUT) == (want == want) &&   /* the deadline ended every window without a press */
             (!sim_virtual || (ytb_num(&back, i, tier_c) == YSCR_TIER_SIM && ytb_num(&back, i, ctier_c) == YSCR_TIER_SIM));
        if (sim_loc && !ca) ok = ok && !strcmp(ytb_text(&back, i, rsp_c), ytb_text(&back, i, tside_c));
        if (!(fabs(meas - plan) <= *soa_err)) *soa_err = meas == meas ? fabs(meas - plan) : INFINITY;   /* NaN: missing */
        if (!ok) {
            fprintf(stderr, "trial_posner: %s: row %d (trial %d %s %s soa %.1f): rt %.9f correct %d\n", who, i, idx,
                    CUES[c], ytb_text(&back, i, val_c), plan, rt, cor);
            nb++;
        }
    }
    return nb;
}

static int sim_check_end(int n_rows, int want_rows, const tally* t) {
    const char* who = sim_virtual ? "--sim" : "--auto";
    /* the script's effects to the nanosecond: every RT of a cell is the
     * same. One early press and one false alarm, when their trials ran. */
    int bad = n_rows != want_rows, nb, i, k, n_fa = t->n_fa[0] + t->n_fa[1];
    double soa_err = 0;
    if (!sim_on) return 0;
    if (bad) fprintf(stderr, "trial_posner: %s: %d rows, not %d\n", who, n_rows, want_rows);
    if (n_fa != (fa_trial >= 0) || t->n_early != (early_trial >= 0)) {
        fprintf(stderr, "trial_posner: %s: %d false alarms, %d too early (script: %d, %d)\n", who, n_fa, t->n_early,
                fa_trial >= 0, early_trial >= 0);
        bad = 1;
    }
    for (k = 0; k < 2; k++)
        for (i = 0; i < 3; i++)
            if (t->effect[k][i] != YRSP_NO_TIME && t->effect[k][i] != YRT_S(sim_rt(k, 0, i)) - YRT_S(sim_rt(k, 1, i))) {
                fprintf(stderr, "trial_posner: %s: %s soa %.1f: effect %.9f s, script %.3f s\n", who, CUES[k], SOA_S[i],
                        YRT_TO_S(t->effect[k][i]), sim_rt(k, 0, i) - sim_rt(k, 1, i));
                bad = 1;
            }
    if (!ytb_open_csv_file(&back, sim_out_path, &(ytb_csv_desc){ .allow_empty = true, .arena = back_arena,
                                                                 .arena_size = sizeof back_arena })) {
        fail(ytb_error(&back));
        return 1;
    }
    nb = sim_check_rows(who, &soa_err);
    /* on the virtual clock no frame slips: the SOAs are the plan's on the
     * frame grid (within 1 us of a 16666667 ns period's multiples) */
    if (sim_virtual && soa_err > 1e-6) { fprintf(stderr, "trial_posner: %s: an SOA %.9f s off its plan\n", who, soa_err); nb++; }
    if (!sim_virtual) printf("%s: the largest SOA error %.6f s\n", who, soa_err);
    if (back.n_rows != n_rows) { fprintf(stderr, "trial_posner: %s: %d rows read back, %d written\n", who, back.n_rows, n_rows); nb++; }
    if (bad || nb) return 1;
    printf("%s: every row, effect and false alarm came out as scripted\n", who);
    return 0;
}
