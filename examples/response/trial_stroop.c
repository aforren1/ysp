/* trial_stroop.c - the Stroop task (Stroop 1935) as a small experiment:
 * color words in colored ink, laid out by pack/layout (Skribidi) over
 * ysp/outline.h curve sets and drawn as ysp/gfx.h curve runs; conditions
 * from a CSV through ysp/table.h, the order from ysp/trials.h's rules
 * text, responses through ysp/response.h.
 *
 * Mirrors PsychoPy's Builder demos Experiments/stroop/ (colored color
 * words, a key per ink color) and stroopExtended/ (practice with feedback,
 * then a test), and the task part of Hardware/EGI_netstation/. Written from
 * their descriptions in docs/rig_spec.md 15; no PsychoPy code (GPL-3) was
 * copied.
 *
 * Each trial: a fixation cross for 0.5 s, then RED, GREEN or BLUE in red,
 * green or blue ink until a response or 2 s. Name the ink: R red, G green,
 * B blue (physical keys). Responses before 0.1 s are anticipations. In the
 * practice block "Correct", "Wrong" or "Too slow" follows for 0.6 s; the
 * test block has no feedback. A banner opens each block for 1.5 s. A
 * blank inter-trial interval of 0.4 s ends each trial.
 *
 * The conditions are 9 rows (word x ink) with a weight column: the 3
 * congruent rows count twice, so half of the test trials are congruent.
 * The rules text sets the rest: a constrained order with at most 3
 * congruent trials in a row, and 4 practice trials drawn from all rows.
 * Words and messages are laid out once at setup; the frame loop draws
 * runs and allocates nothing. Colors are linear device RGB, uncalibrated.
 *
 * Font: Arial (Windows) or DejaVu Sans (Linux), or --font. Without one,
 * --sim runs and checks the trials with nothing drawn (as on a CI runner);
 * a window run stops, since a participant would see no words.
 *
 * The data file has one row per trial: trial_index, block, stimulus, ink,
 * congruent, answer, response, rt, correct, feedback (jsPsych-style names
 * first); then onset_tier, onset_src, rsp_tier, rsp_flags,
 * n_anticipations. Times in seconds. Shift+Esc stops the session; the rows
 * so far are kept.
 *
 * --sim runs 4 practice and 12 test trials on the simulated display with a
 * synthetic participant who is 0.1 s slower on incongruent trials: a word
 * read in place of the ink, a timeout and a Stroop error among correct
 * responses. It checks the rows, their classification, the feedback and
 * the order's rule, and exits 1 on a mismatch. About 30 s.
 *
 * Usage: trial_stroop [--sim] [--fullscreen] [--reps N] [--seed N] [--out FILE]
 *                     [--font PATH] [--conditions FILE]
 *   --reps N           test trials are weight x N per row (default 3; --sim: 1)
 *   --conditions FILE  a CSV with the columns word, ink, congruent, answer, weight
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
#include "ysp/layout.h"
#define YSP_TIMELINE_IMPLEMENTATION
#include "ysp/timeline.h"
#define YTB_STDIO
#define YSP_TRIALS_IMPLEMENTATION
#include "ysp/trials.h"
#define YSP_RESPONSE_IMPLEMENTATION
#include "ysp/response.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { BANNER_PRACTICE, BANNER_TEST, FIX_ON, WORD_ON, FB_CORRECT, FB_WRONG, FB_SLOW, N_CHANNELS };
enum { TRIAL = 1 };
enum { END_OF_TRIAL = 1 };

#define BANNER_S   1.5
#define FIXATION_S 0.5
#define RESPONSE_S 2.0
#define MIN_RT_S   0.1
#define FEEDBACK_S 0.6
#define ITI_S      0.4
#define N_PRACTICE 4

#ifdef _WIN32
#define DEFAULT_FONT "C:/Windows/Fonts/arial.ttf"
#else
#define DEFAULT_FONT "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
#endif

static const char conditions_csv[] =
    "word,ink,congruent,answer,weight\n"
    "red,red,1,r,2\n"
    "red,green,0,g,1\n"
    "red,blue,0,b,1\n"
    "green,red,0,r,1\n"
    "green,green,1,g,2\n"
    "green,blue,0,b,1\n"
    "blue,red,0,r,1\n"
    "blue,green,0,g,1\n"
    "blue,blue,1,b,2\n";

static const char rules[] =
    "order constrained\n"
    "max_run congruent=1 3\n"
    "practice 4\n";

static const yrsp_choice keys[] = { { .key = "r", .name = "red" }, { .key = "g", .name = "green" }, { .key = "b", .name = "blue" } };
#define N_INKS 3
static const char* const ink_names[N_INKS] = { "red", "green", "blue" };
static const float ink_rgb[N_INKS][3] = { { 0.70f, 0.04f, 0.03f }, { 0.04f, 0.42f, 0.05f }, { 0.07f, 0.15f, 0.85f } };
static const float GRAY[3] = { 0.75f, 0.75f, 0.75f };

/* The --sim participant: 0.45 s on congruent trials, 0.55 s on
 * incongruent ones. Trial 1 reads the word (or presses the next key when
 * word and ink agree), trial 3 does not answer, test trial 10 makes the
 * same error, test trial 13 times out. */
#define SIM_TRIALS (N_PRACTICE + 12)
static int sim_wrong(int t) { return t == 1 || t == 10; }
static int sim_silent(int t) { return t == 3 || t == 13; }
static double sim_rt(int congruent) { return congruent ? 0.45 : 0.55; }

static yscr_screen scr;
static ygfx_gfx gfx;
static yol_ctx ol;
static ylay_lib* L;
static ygfx_cset sets[4];
static ytl_timeline tl;
static ytl_event storage[64];
static ytr_trials trials;
static yrsp_collector rsp;
static uint64_t table_arena[4096], rules_arena[2048];
static ytb_table tab;

/* A laid-out text: its block and one curve run per font run. */
#define TXT_RUNS 4
typedef struct txt { ylay_block b; ygfx_stim st[TXT_RUNS]; int n; float y; } txt;
static txt word_txt[N_INKS], banner[2], fb[3], keys_txt, cur;
static ygfx_stim fix;

static int lay(txt* t, const char* s, float px, float y) {
    ylay_style st;
    memset(&st, 0, sizeof st);
    st.size = px; st.width = 1000; st.align = YLAY_ALIGN_CENTER; st.dir = YLAY_DIR_LTR; st.lang = "en";
    if (ylay_layout(L, s, -1, &st, NULL, 0, &t->b) != YLAY_OK) return -1;
    t->y = y;
    return 0;
}

/* The runs, after every set is on the GPU. Each run shares the block's box,
 * centered at (0, y). */
static void runs(txt* t, const float* color) {
    uint32_t r;
    t->n = 0;
    for (r = 0; r < t->b.n_runs && t->n < TXT_RUNS; r++) {
        const ylay_run* run = &t->b.runs[r];
        ygfx_crun_desc d;
        if (!run->n || run->font >= 4) continue;
        memset(&d, 0, sizeof d);
        d.set = sets[run->font]; d.items = t->b.items + run->first; d.n = (int)run->n; d.size = run->size;
        d.y = t->y; d.w = t->b.w > 1 ? t->b.w : 1; d.h = t->b.h > 1 ? t->b.h : 1;
        d.color[0] = color[0]; d.color[1] = color[1]; d.color[2] = color[2];
        t->st[t->n++] = ygfx_crun(&gfx, &d);
    }
}

static void draw_txt(const txt* t) {
    int i;
    for (i = 0; i < t->n; i++) ygfx_draw(&gfx, &t->st[i]);
}

static uint8_t* read_file(const char* path, size_t* n) {
    FILE* fp = fopen(path, "rb");
    uint8_t* b = NULL;
    long len;
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END); len = ftell(fp); fseek(fp, 0, SEEK_SET);
    if (len > 0 && (b = (uint8_t*)malloc((size_t)len)) != NULL && fread(b, 1, (size_t)len, fp) != (size_t)len) { free(b); b = NULL; }
    fclose(fp);
    *n = b ? (size_t)len : 0;
    return b;
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
    ytr_rules_desc rr;
    yrsp_desc pd;
    yscr_frame f;
    ytl_event fired[16];
    ytr_trial_info ti = { 0 };
    yrsp_result res, sim_res[SIM_TRIALS];
    yrsp_input sim_ev[4];
    int sim_cong[SIM_TRIALS], sim_correct[SIM_TRIALS], sim_fb[SIM_TRIALS], sim_prac[SIM_TRIALS], cond_reps[64];
    FILE* out;
    char line[1024], meta[2048], err[300], rules_out[512];
    const char* out_path = "trial_stroop.csv";
    const char* font_path = DEFAULT_FONT;
    const char* cond_path = NULL;
    uint8_t* font = NULL;
    size_t font_n = 0;
    uint64_t seed = 20261009;
    int64_t word_frame = -1;
    int i, n, sim = 0, reps = 0, fullscreen = 0, aborted = 0, failed = 0, rows = 0, have_font = 0;
    int in_trial = 0, responding = 0, n_sim_ev = 0, sim_next = 0, word_col, ink_col, cong_col, answer_col, weight_col;
    int ink = 0, word = 0, congruent = 0, feedback = -1;
    char answer[8] = "";

    memset(&sd, 0, sizeof sd);
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--sim")) sim = 1;
        else if (!strcmp(argv[i], "--fullscreen")) fullscreen = 1;
        else if (!strcmp(argv[i], "--reps") && i + 1 < argc) reps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) seed = (uint64_t)strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out_path = argv[++i];
        else if (!strcmp(argv[i], "--font") && i + 1 < argc) font_path = argv[++i];
        else if (!strcmp(argv[i], "--conditions") && i + 1 < argc) cond_path = argv[++i];
        else {
            fprintf(stderr, "usage: trial_stroop [--sim] [--fullscreen] [--reps N] [--seed N] [--out FILE] [--font PATH] [--conditions FILE]\n");
            return 2;
        }
    }
    if (reps == 0) reps = sim ? 1 : 3;
    if (reps < 1 || reps > 20) { fprintf(stderr, "trial_stroop: --reps takes 1..20\n"); return 2; }

    /* The conditions: rows, columns by name, and the weight as repetitions. */
    memset(&cd, 0, sizeof cd);
    cd.text = conditions_csv;
    cd.len = sizeof conditions_csv - 1;
    cd.arena = table_arena;
    cd.arena_size = sizeof table_arena;
    if (!(cond_path ? ytb_csv_file(&tab, cond_path, &cd) : ytb_csv(&tab, &cd))) {
        fprintf(stderr, "trial_stroop: %s\n", ytb_error(&tab));
        return 1;
    }
    word_col = ytb_col(&tab, "word"); ink_col = ytb_col(&tab, "ink"); cong_col = ytb_col(&tab, "congruent");
    answer_col = ytb_col(&tab, "answer"); weight_col = ytb_col(&tab, "weight");
    if (word_col < 0 || ink_col < 0 || cong_col < 0 || answer_col < 0 || weight_col < 0 || tab.n_rows > 64) {
        fprintf(stderr, "trial_stroop: the conditions need word, ink, congruent, answer and weight, at most 64 rows\n");
        return 1;
    }
    for (i = 0; i < tab.n_rows; i++) cond_reps[i] = ytb_int(&tab, i, weight_col) * reps;
    memset(&rd, 0, sizeof rd);
    rd.table = &tab;
    rd.rng = ytr_splitmix;
    rd.rng_ctx = &seed;
    memset(&rr, 0, sizeof rr);
    rr.text = rules; rr.len = sizeof rules - 1; rr.table = &tab;
    rr.arena = rules_arena; rr.arena_size = sizeof rules_arena;
    if (ytr_rules(&rd, &rr, err, sizeof err) != 0) { fprintf(stderr, "trial_stroop: %s\n", err); return 1; }
    rd.cond_reps = cond_reps;
    if (!ytr_open(&trials, &rd)) { fprintf(stderr, "trial_stroop: %s\n", ytr_error(&trials)); return 1; }

    if (sim) sd.backend = YSCR_BACKEND_SIM;
    sd.windowed = !fullscreen;
    if (!yscr_open(&scr, &sd)) { fprintf(stderr, "trial_stroop: %s\n", yscr_error(&scr)); return 1; }
    memset(&gd, 0, sizeof gd);
    gd.screen = &scr;
    gd.background[0] = gd.background[1] = gd.background[2] = 0.05f;
    if (!ygfx_open(&gfx, &gd)) { fprintf(stderr, "trial_stroop: %s\n", ygfx_error(&gfx)); return 1; }

    /* Every text is laid out here, then each font's set goes up once. */
    if (!yol_open(&ol, NULL) || ylay_create(&L, &(ylay_desc){ .ol = &ol }) != YLAY_OK) {
        fprintf(stderr, "trial_stroop: the layout library did not open\n");
        return 1;
    }
    font = read_file(font_path, &font_n);
    if (font && ylay_add_font(L, font, font_n, 0, font_path) >= 0) {
        static const char* const fb_text[3] = { "Correct", "Wrong", "Too slow" };
        int ok = 1, nf = ylay_font_count(L);
        for (i = 0; i < N_INKS; i++) {
            char up[16];
            int c;
            for (c = 0; ink_names[i][c] && c < 15; c++) up[c] = (char)toupper((unsigned char)ink_names[i][c]);
            up[c] = 0;
            ok &= lay(&word_txt[i], up, 72, 0) == 0;
        }
        for (i = 0; i < 3; i++) ok &= lay(&fb[i], fb_text[i], 32, 0) == 0;
        ok &= lay(&banner[0], "Practice. Name the ink color, not the word.", 30, -30) == 0;
        ok &= lay(&banner[1], "Test. No feedback from here on.", 30, -30) == 0;
        ok &= lay(&keys_txt, "R red     G green     B blue", 22, 30) == 0;
        for (i = 0; i < nf && i < 4 && ok; i++) {
            ylay_font_info fi;
            ygfx_cset_desc d;
            ylay_font(L, i, &fi);
            memset(&d, 0, sizeof d);
            d.texels = fi.set->texels; d.n_texels = fi.set->n_texels; d.words = fi.set->words; d.n_words = fi.set->n_words;
            if (d.n_texels) ok &= (sets[i] = ygfx_cset_make(&gfx, &d)).id != 0;
        }
        if (!ok) { fprintf(stderr, "trial_stroop: text: %s %s\n", ylay_error(L), ygfx_error(&gfx)); return 1; }
        for (i = 0; i < N_INKS; i++) runs(&word_txt[i], ink_rgb[i]);
        for (i = 0; i < 3; i++) runs(&fb[i], GRAY);
        runs(&banner[0], GRAY); runs(&banner[1], GRAY); runs(&keys_txt, GRAY);
        have_font = 1;
    } else {
        printf("trial_stroop: %s: %s; no text is drawn\n", font_path, font ? ylay_error(L) : "not found");
        if (!sim) { fprintf(stderr, "trial_stroop: a window run needs a font (--font PATH)\n"); return 1; }
    }
    memset(&fd, 0, sizeof fd);
    fd.shape = YGFX_CROSS; fd.w = fd.h = 24; fd.shape_p[0] = 3;
    fd.edge = YGFX_EDGE_COSINE; fd.edge_width = 1;
    fd.color[0] = fd.color[1] = fd.color[2] = 0.6f;
    fix = ygfx_shape(&fd);

    memset(&td, 0, sizeof td);
    td.events = storage;
    td.event_capacity = 64;
    td.n_channels = N_CHANNELS;
    if (!ytl_open(&tl, &td)) { fprintf(stderr, "trial_stroop: %s\n", ytl_error(&tl)); return 1; }
    memset(&pd, 0, sizeof pd);
    pd.choices = keys;
    pd.n_choices = 3;
    pd.duration_ns = YRT_S(RESPONSE_S);
    pd.min_rt_ns = YRT_S(MIN_RT_S);
    if (!yrsp_open(&rsp, &pd)) { fprintf(stderr, "trial_stroop: %s\n", yrsp_error(&rsp)); return 1; }

    out = fopen(out_path, "w");
    if (!out) { fprintf(stderr, "trial_stroop: cannot write %s\n", out_path); return 1; }
    ytr_format_meta(&trials, meta, sizeof meta);
    meta[strcspn(meta, "\r\n")] = 0;
    ytr_format_rules(&trials, rules_out, sizeof rules_out);
    for (i = 0; rules_out[i]; i++) if (rules_out[i] == '\n') rules_out[i] = ';';
    yscr_describe(&scr, line, sizeof line);
    fprintf(out, "# %s\n# rules: %s\n# %s\n# ysp_response %s; keyboard: %s\n", meta, rules_out, line, yrsp_version(),
            sim ? "synthetic (tier SIM)" : "SDL through yscr_event_input");
    if (have_font) { ylay_stamp(L, NULL, line, sizeof line); fprintf(out, "# layout: %s\n", line); }
    fprintf(out, "trial_index,block,stimulus,ink,congruent,answer,response,rt,correct,feedback,"
                 "onset_tier,onset_src,rsp_tier,rsp_flags,n_anticipations\n");

    for (;;) {
        SDL_Event ev;
        int64_t bt = 0;
        yrsp_input in;
        const float* v;
        int rc = yscr_begin(&scr, &f);
        if (rc == YSCR_QUIT) { aborted = 1; break; }
        if (rc != YSCR_OK) { fprintf(stderr, "trial_stroop: %s\n", yscr_error(&scr)); failed = 1; break; }

        for (i = 0; i < f.n_done; i++)   /* the word's flip record: its onset, final */
            if (f.done[i].index == word_frame) {
                yrsp_onset o = yrsp_onset_flip(&f.done[i]);
                yrsp_set_onset(&rsp, &o);
            }

        if (!in_trial) {
            ytl_seq q;
            const char* a = NULL;
            if (ytr_next(&trials, &ti) < 0) break;
            for (word = 0; word < N_INKS && strcmp(ytb_text(&tab, ti.condition, word_col), ink_names[word]); word++) {}
            for (ink = 0; ink < N_INKS && strcmp(ytb_text(&tab, ti.condition, ink_col), ink_names[ink]); ink++) {}
            if (word == N_INKS || ink == N_INKS) { fprintf(stderr, "trial_stroop: row %d: word and ink must be red, green or blue\n", ti.condition); failed = 1; break; }
            congruent = ytb_int(&tab, ti.condition, cong_col);
            a = ytb_text(&tab, ti.condition, answer_col);
            snprintf(answer, sizeof answer, "%s", a ? a : "");
            cur = word_txt[word];   /* the word's runs, in this trial's ink */
            for (i = 0; i < cur.n; i++) memcpy(cur.st[i].color, ink_rgb[ink], sizeof ink_rgb[ink]);
            feedback = -1;
            word_frame = -1;
            ytl_clear(&tl, TRIAL);
            ytl_anchor(&tl, TRIAL, f.onset, 0);
            q = ytl_seq_on(&tl, TRIAL);
            if (ti.first_in_block) {
                ytl_on(&q, ti.practice ? BANNER_PRACTICE : BANNER_TEST);
                ytl_wait(&q, YRT_S(BANNER_S));
                ytl_off(&q, ti.practice ? BANNER_PRACTICE : BANNER_TEST);
            }
            ytl_on(&q, FIX_ON);
            ytl_wait(&q, YRT_S(FIXATION_S));
            ytl_off(&q, FIX_ON);
            ytl_on(&q, WORD_ON);
            if (q.err) { fprintf(stderr, "trial_stroop: timeline error %d\n", q.err); failed = 1; break; }
            yrsp_arm(&rsp, f.onset);
            in_trial = responding = 1;
            n_sim_ev = sim_next = 0;
        }

        while (yscr_poll(&scr, &ev, NULL))
            if (yscr_event_input(&scr, &ev, &in)) yrsp_feed(&rsp, &in);
        while (sim && sim_next < n_sim_ev && sim_ev[sim_next].t <= f.onset) yrsp_feed(&rsp, &sim_ev[sim_next++]);

        if (responding && yrsp_update(&rsp, f.onset) == YRSP_ENDED) {
            ytl_seq q = ytl_seq_on(&tl, TRIAL);
            yrsp_result now;
            yrsp_finish(&rsp, &now);   /* the live result picks the feedback; the row takes the final one */
            ytl_base_time(&tl, TRIAL, f.onset, &bt);
            ytl_at(&q, bt);
            ytl_off(&q, WORD_ON);
            if (ti.practice) {
                feedback = !(now.flags & YRSP_R_RESPONDED) ? 2 : strcmp(keys[now.response].key, answer) ? 1 : 0;
                ytl_on(&q, FB_CORRECT + feedback);
                ytl_wait(&q, YRT_S(FEEDBACK_S));
                ytl_off(&q, FB_CORRECT + feedback);
            }
            ytl_wait(&q, YRT_S(ITI_S));
            ytl_mark(&q, END_OF_TRIAL, 0);
            if (q.err) { fprintf(stderr, "trial_stroop: timeline error %d\n", q.err); failed = 1; break; }
            responding = 0;
        }

        n = ytl_evaluate(&tl, &(ytl_frame){ f.onset, f.period, f.index }, fired, 16);
        for (i = 0; i < n && i < 16; i++) {
            if (fired[i].kind == YTL_ONSET && fired[i].target == WORD_ON) {
                yrsp_onset o = yrsp_onset_landing(&fired[i]);
                yrsp_set_onset(&rsp, &o);
                word_frame = fired[i].frame;
                if (sim && !sim_silent(ti.index)) {   /* the participant answers from the word's onset */
                    int k = sim_wrong(ti.index) ? (congruent ? (ink + 1) % N_INKS : word) : ink;
                    memset(sim_ev, 0, sizeof sim_ev);
                    sim_ev[0].t = o.t_ns + YRT_S(sim_rt(congruent));
                    sim_ev[0].type = YRSP_PRESS;
                    sim_ev[0].control = (uint32_t)yrsp_scancode(keys[k].key);
                    sim_ev[0].stamp = YRSP_TIER_SIM;
                    sim_ev[0].value = 1;
                    sim_ev[1] = sim_ev[0];
                    sim_ev[1].type = YRSP_RELEASE;
                    sim_ev[1].value = 0;
                    sim_ev[1].t = sim_ev[0].t + YRT_MS(80);
                    n_sim_ev = 2;
                }
            }
            if (fired[i].kind == YTL_MARK && fired[i].code == END_OF_TRIAL) {
                static const char* const fb_name[3] = { "correct", "wrong", "too_slow" };
                char rt[32];
                int correct;
                yrsp_finish(&rsp, &res);
                correct = (res.flags & YRSP_R_RESPONDED) && !strcmp(keys[res.response].key, answer);
                sec(rt, sizeof rt, res.rt_ns == YRSP_NO_TIME ? NAN : YRT_TO_S(res.rt_ns));
                fprintf(out, "%d,%s,%s,%s,%d,%s,%s,%s,%d,%s,%u,%u,%u,%u,%d\n", ti.index, ti.practice ? "practice" : "test",
                        ytb_text(&tab, ti.condition, word_col), ink_names[ink], congruent, answer,
                        res.response_name ? res.response_name : "", rt, correct, feedback >= 0 ? fb_name[feedback] : "",
                        (unsigned)res.onset_tier, (unsigned)res.onset_src, (unsigned)res.stamp_tier, (unsigned)res.flags,
                        res.n_anticipations);
                fflush(out);
                printf("trial %2d %-8s %-5s in %-5s rt %s response %-5s %s\n", ti.index, ti.practice ? "practice" : "test",
                       ink_names[word], ink_names[ink], rt[0] ? rt : "-", res.response_name ? res.response_name : "-",
                       !(res.flags & YRSP_R_RESPONDED) ? "(too slow)" : correct ? "correct" : "wrong");
                ytr_update(&trials, (res.flags & YRSP_R_RESPONDED) ? correct : YTR_INVALID, NULL);
                if (ti.index < SIM_TRIALS) {
                    sim_res[ti.index] = res;
                    sim_cong[ti.index] = congruent * 2 + (word == ink);   /* the column and the truth */
                    sim_correct[ti.index] = correct;
                    sim_fb[ti.index] = feedback;
                    sim_prac[ti.index] = ti.practice;
                }
                rows++;
                in_trial = 0;
            }
        }
        v = ytl_values(&tl);
        yscr_mark(&scr, YSCR_PHASE_EVALUATE);
        ygfx_begin(&gfx, &f);
        if (v[FIX_ON] > 0.5f) ygfx_draw(&gfx, &fix);
        if (v[WORD_ON] > 0.5f) draw_txt(&cur);
        for (i = 0; i < 2; i++)
            if (v[BANNER_PRACTICE + i] > 0.5f) { draw_txt(&banner[i]); draw_txt(&keys_txt); }
        for (i = 0; i < 3; i++)
            if (v[FB_CORRECT + i] > 0.5f) draw_txt(&fb[i]);
        ygfx_end(&gfx);
        yscr_flip(&scr);
    }
    fclose(out);
    ygfx_close(&gfx);
    yscr_close(&scr);
    ylay_destroy(L);
    yol_close(&ol);
    free(font);
    printf("%d trials written to %s%s\n", rows, out_path, aborted ? " (stopped with Shift+Esc)" : "");

    if (sim && !failed && (reps != 1 || cond_path)) printf("--sim: the self-check needs the default conditions and --reps\n");
    else if (sim && !failed) {
        int bad = rows != SIM_TRIALS, run = 0, n_rt[2] = { 0, 0 };
        double sum_rt[2] = { 0, 0 };
        if (bad) fprintf(stderr, "trial_stroop: --sim: %d rows, not %d\n", rows, SIM_TRIALS);
        for (i = 0; i < rows && i < SIM_TRIALS; i++) {
            const yrsp_result* r = &sim_res[i];
            int cong = sim_cong[i] >> 1, ok = (sim_cong[i] & 1) == cong && sim_prac[i] == (i < N_PRACTICE);
            if (sim_silent(i)) ok = ok && !(r->flags & YRSP_R_RESPONDED) && (r->flags & YRSP_R_TIMEOUT) && !sim_correct[i];
            else ok = ok && (r->flags & YRSP_R_RESPONDED) && fabs(YRT_TO_S(r->rt_ns) - sim_rt(cong)) < 1e-6 && sim_correct[i] == !sim_wrong(i);
            ok = ok && sim_fb[i] == (i >= N_PRACTICE ? -1 : sim_silent(i) ? 2 : sim_wrong(i) ? 1 : 0);
            ok = ok && r->onset_tier == YSCR_TIER_SIM && r->onset_src == YRSP_ONSET_FLIP;
            if (i >= N_PRACTICE) {   /* the rule: at most 3 congruent test trials in a row */
                run = cong ? run + 1 : 0;
                ok = ok && run <= 3;
                if (sim_correct[i]) { sum_rt[cong] += YRT_TO_S(r->rt_ns); n_rt[cong]++; }
            }
            if (!ok) {
                fprintf(stderr, "trial_stroop: --sim: trial %d: congruent %d rt %.9f flags 0x%x correct %d feedback %d\n", i,
                        cong, YRT_TO_S(r->rt_ns), (unsigned)r->flags, sim_correct[i], sim_fb[i]);
                bad = 1;
            }
        }
        if (n_rt[0] && n_rt[1]) {
            double effect = sum_rt[0] / n_rt[0] - sum_rt[1] / n_rt[1];
            printf("Stroop effect, correct test trials: %.3f s (%d incongruent, %d congruent)\n", effect, n_rt[0], n_rt[1]);
            if (fabs(effect - 0.1) > 1e-6) bad = 1;
        } else bad = 1;
        if (bad) return 1;
        printf("--sim: every trial came out as scripted\n");
    }
    return failed;
}
