/* trial_images.c - the pack path end to end: textures and the conditions
 * table read from a pack that ypak built, images drawn by ysp/gfx.h, blocks
 * in random order from ysp/trials.h groups, responses through
 * ysp/response.h.
 *
 * Mirrors PsychoPy's Builder demos Design Templates/randomisedBlocks/
 * (image blocks in random order, images random within a block),
 * Experiments/navon/ (global and local letters), mentalRotation/ (turned
 * letters, same or mirrored) and the image part of Coder
 * stimuli/face_jpg.py (an image as a grating's mask). Written from their
 * descriptions in docs/rig_spec.md 15; no PsychoPy code (GPL-3) was
 * copied.
 *
 * The pack: the build runs trial_images_assets (PNGs made in code, and
 * examples/pack/trial_images.csv), then `ypak build`. Nothing binary is in
 * the repository. Its conditions table names the pack entry of each
 * row's image, so the table and the textures travel together.
 *
 * Two blocks, in random order (groups on the block column):
 *   navon     a large H or S made of small H or S; F if the large letter
 *             is H, J if it is S (Navon 1977)
 *   rotation  R or its mirror image turned 0, 60, 120 or 180 degrees; F if
 *             it is a normal R, J if mirrored (Shepard and Metzler 1971)
 * Each block opens with its cue for 1.5 s: a grating seen through the
 * pack's graded mask image. The task for the block is in the window's
 * title and on stdout. Each trial: a fixation cross for 0.5 s, the image
 * until a response or 2.5 s, a blank inter-trial interval of 0.5 s.
 *
 * The graded mask (docs/rig_spec.md 15.6): MASK_TEX takes a distance
 * shape, so a graded image goes through a target: the grating drawn into
 * an RGBA16F target as increments, the mask image over it with MULTIPLY
 * (rgb = dst x mask), and the target added to the scene. At setup the
 * program draws the grating alone into a second target, reads both back,
 * and prints the largest |masked - grating x mask| over the largest
 * |grating|. Not on the simulated display: its null backend draws nothing.
 *
 * The data file has one row per trial (ysp/trials.h's data writer):
 * trial_index, block, stimulus, ori, kind, answer, correct (jsPsych-style
 * names first), then the response's (YRSP_DATA_COLS: response, rt_s,
 * onset_src, onset_tier, the residuals, onset_frame, rsp_tier, rsp_flags,
 * n_anticipations, n_early). No session columns: the table's block column
 * (navon, rotation) has the name of the session's block number. Times in
 * seconds; the settings and the pack's id go to FILE.meta. Shift+Esc
 * stops the session; the rows so far are kept.
 *
 * --sim runs the 12 conditions once on the simulated display with a
 * synthetic participant: 60 ms slower on incongruent Navon figures, 2 ms
 * per degree of turn and 0.1 s more for a mirror image, one wrong key and
 * one timeout. It checks the rows, that each block ran whole, and the RTs
 * (the script runs from the image's planned onset, the RT from its flip
 * record), and exits 1 on a mismatch.
 * Under a second: the virtual clock does not wait.
 *
 * Usage: trial_images [--sim] [--fullscreen] [--reps N] [--seed N] [--out FILE] [--pack PATH]
 *   --reps N    repetitions of each row (default 2; --sim: 1)
 *   --pack PATH the pack (default: the one the build made)
 * Exit code: 0 (also after Shift+Esc), 1 when something did not open or a
 * --sim check failed, 2 for a bad argument.
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#define YSP_GFX_IMPLEMENTATION
#include "ysp/gfx.h"
#define YSP_PACK_IMPLEMENTATION
#include "ysp/pack.h"
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

#ifndef TRIAL_IMAGES_PACK
#define TRIAL_IMAGES_PACK "trial_images.ysppak"
#endif

enum { CUE_ON, FIX_ON, IMAGE_ON, N_CHANNELS };
enum { TRIAL = 1 };
enum { END_OF_TRIAL = 1 };

#define CUE_S      1.5
#define FIXATION_S 0.5
#define RESPONSE_S 2.5
#define MIN_RT_S   0.1
#define ITI_S      0.5
#define MASK_NAME  "img/mask.ysptex"
#define MASK_W     128

static const yrsp_choice keys[] = { { .key = "f", .name = "f" }, { .key = "j", .name = "j" } };

/* The program's columns; yrsp_data_set() fills YRSP_DATA_COLS. */
typedef struct row { int trial_index; const char *block, *stimulus, *kind, *answer; double ori; bool correct; } row;
static const ytr_col cols[] = {
    YTR_COL(row, trial_index, YTR_COL_INT), YTR_COL(row, block, YTR_COL_STRING),
    YTR_COL(row, stimulus, YTR_COL_STRING), YTR_COL(row, ori, YTR_COL_DOUBLE), YTR_COL(row, kind, YTR_COL_STRING),
    YTR_COL(row, answer, YTR_COL_STRING), YTR_COL(row, correct, YTR_COL_BOOL), YRSP_DATA_COLS,
};

/* The --sim participant; trial 2 presses the other key, trial 7 none. */
#define SIM_TRIALS 12
static double sim_rt(int rotation, double ori, int odd) {
    return rotation ? 0.50 + 0.002 * ori + (odd ? 0.10 : 0.0) : 0.50 + (odd ? 0.06 : 0.0);
}
static yrsp_input sim_ev[2];
static int n_sim_ev, sim_next;

static yscr_screen scr;
static ygfx_gfx gfx;
static ypak_pack pk;
static ytl_timeline tl;
static ytl_event storage[64];
static ytr_trials trials;
static ytr_data data;
static yrsp_collector rsp;
static ytb_table tab;
static ygfx_stim fix, cur, cue;
static int have_cue;
static float rd_u[MASK_W * MASK_W * 4], rd_m[MASK_W * MASK_W * 4];

static int fail(const char* why) {
    fprintf(stderr, "trial_images: %s\n", why);
    return 1;
}

/* The pack's textures, each made once. */
typedef struct img { const char* name; ygfx_tex tex; int w, h, format; const uint8_t* data; } img;
static img imgs[16];
static int n_imgs;

static int load_image(const char* name) {
    ypak_entry e;
    ypak_texture t;
    ygfx_texture_desc d;
    const void* p;
    char err[200];
    int i;
    for (i = 0; i < n_imgs; i++) if (!strcmp(imgs[i].name, name)) return i;
    if (n_imgs == 16) { fprintf(stderr, "trial_images: more than 16 images\n"); return -1; }
    if (ypak_find(&pk, name, &e) != 0 || !(p = ypak_data(&pk, &e))) { fprintf(stderr, "trial_images: %s\n", ypak_error(&pk)); return -1; }
    if (ypak_texture_view(p, e.size, &t, err, sizeof err) != 0) { fprintf(stderr, "trial_images: %s: %s\n", name, err); return -1; }
    if (t.compression != YPAK_TEX_RAW) { fprintf(stderr, "trial_images: %s: this example takes raw textures\n", name); return -1; }
    memset(&d, 0, sizeof d);
    d.w = (int32_t)t.w; d.h = (int32_t)t.h; d.format = (ygfx_format)t.format; d.data = t.data;
    memcpy(&d.enc, t.enc, sizeof d.enc);   /* stored as ygfx_texture() takes it (docs/pack.md 4.9) */
    imgs[n_imgs].tex = ygfx_texture(&gfx, &d);
    if (!imgs[n_imgs].tex.id) { fprintf(stderr, "trial_images: %s: %s\n", name, ygfx_error(&gfx)); return -1; }
    imgs[n_imgs].name = name; imgs[n_imgs].w = d.w; imgs[n_imgs].h = d.h;
    imgs[n_imgs].format = (int)t.format; imgs[n_imgs].data = t.data;
    return n_imgs++;
}

/* The graded mask: a grating into target U alone and into target M with
 * the mask over it by MULTIPLY; M becomes the cue. Returns 0 or -1. */
static int mask_setup(const img* m) {
    static const float zero[4] = { 0, 0, 0, 0 };
    ygfx_target_desc td;
    ygfx_grating_desc gd;
    ygfx_image_desc id;
    ygfx_stim g, over;
    ygfx_tex U, M;
    double worst = 0, peak = 0;
    int i, c;
    if (m->format != YPAK_TEX_RGBA8 || m->w != MASK_W || m->h != MASK_W) { fprintf(stderr, "trial_images: the mask is not RGBA8 %dx%d\n", MASK_W, MASK_W); return -1; }
    memset(&td, 0, sizeof td);
    td.w = td.h = MASK_W;   /* RGBA16F: increments can be negative */
    U = ygfx_target(&gfx, &td);
    M = ygfx_target(&gfx, &td);
    memset(&gd, 0, sizeof gd);
    gd.w = gd.h = MASK_W; gd.sf = 1.0f / 16; gd.ori = 30;
    g = ygfx_grating(&gd);
    memset(&id, 0, sizeof id);
    id.tex = m->tex;
    over = ygfx_image(&gfx, &id);
    over.blend = YGFX_BLEND_MODE_MULTIPLY;
    if (!U.id || !M.id || ygfx_begin_setup(&gfx) < 0 || ygfx_begin_target(&gfx, U, zero) < 0 || ygfx_draw(&gfx, &g) < 0 ||
        ygfx_end_target(&gfx) < 0 || ygfx_begin_target(&gfx, M, zero) < 0 || ygfx_draw(&gfx, &g) < 0 ||
        ygfx_draw(&gfx, &over) < 0 || ygfx_end_target(&gfx) < 0 || ygfx_end_setup(&gfx) < 0 ||
        ygfx_read_target(&gfx, U, 0, 0, MASK_W, MASK_W, rd_u) < 0 || ygfx_read_target(&gfx, M, 0, 0, MASK_W, MASK_W, rd_m) < 0) {
        fprintf(stderr, "trial_images: mask: %s\n", ygfx_error(&gfx));
        return -1;
    }
    for (i = 0; i < MASK_W * MASK_W; i++)
        for (c = 0; c < 3; c++) {
            double want = (double)rd_u[4 * i + c] * m->data[4 * i + c] / 255.0, d = fabs(rd_m[4 * i + c] - want);
            if (d > worst) worst = d;
            if (fabs(rd_u[4 * i + c]) > peak) peak = fabs(rd_u[4 * i + c]);
        }
    printf("graded mask by target and MULTIPLY: largest |masked - grating x mask| %.3g, %.3g of the grating's peak %.3g "
           "(RGBA16F keeps 11 bits: 4.9e-4 of a value)\n", worst, peak > 0 ? worst / peak : NAN, peak);
    memset(&id, 0, sizeof id);
    id.tex = M; id.w = id.h = 2 * MASK_W; id.add = true; id.linear = true;
    cue = ygfx_image(&gfx, &id);
    have_cue = 1;
    return 0;
}

int main(int argc, char** argv) {
    ypak_entry e;
    yscr_frame f;
    ytl_event fired[64];                   /* the timeline's capacity: all that can fire */
    ytr_trial_info ti = { 0 };
    yrsp_result res, sim_res[SIM_TRIALS];
    row r = { 0 };
    double sim_want[SIM_TRIALS];
    int64_t sim_plan[SIM_TRIALS] = { 0 };   /* where the script's times start */
    int sim_block[SIM_TRIALS], sim_correct[SIM_TRIALS], row_img[YTR_MAX_CONDITIONS];
    char line[1024], tmeta[1024], meta[3072], meta_path[1100], id[72];
    const char *out_path = "trial_images.csv", *pack_path = TRIAL_IMAGES_PACK;
    const void* p;
    uint64_t seed = 20261009;
    int i, n, landed, sim = 0, reps = 0, fullscreen = 0, aborted = 0, failed = 0, rows = 0;
    int in_trial = 0, responding = 0, block_col, image_col, ori_col, kind_col, answer_col, rotation = 0, odd = 0;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--sim")) sim = 1;
        else if (!strcmp(argv[i], "--fullscreen")) fullscreen = 1;
        else if (!strcmp(argv[i], "--reps") && i + 1 < argc) reps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) seed = (uint64_t)strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out_path = argv[++i];
        else if (!strcmp(argv[i], "--pack") && i + 1 < argc) pack_path = argv[++i];
        else {
            fprintf(stderr, "usage: trial_images [--sim] [--fullscreen] [--reps N] [--seed N] [--out FILE] [--pack PATH]\n");
            return 2;
        }
    }
    /* --sim runs on ysp/rt.h's virtual clock: the same records on any host */
    if (sim) yrt_virtual_begin(0);
    if (reps == 0) reps = sim ? 1 : 2;
    if (reps < 1 || reps > 50) { fprintf(stderr, "trial_images: --reps takes 1..50\n"); return 2; }

    /* The pack: mapped, each entry verified when first used. */
    if (!ypak_open(&pk, &(ypak_desc){ .path = pack_path })) { fprintf(stderr, "trial_images: %s: %s\n", pack_path, ypak_error(&pk)); return 1; }
    if (ypak_find(&pk, "conditions.pstb", &e) != 0 || !(p = ypak_data(&pk, &e)) || !ytb_open_view(&tab, p, (size_t)e.size)) {
        fprintf(stderr, "trial_images: conditions.pstb: %s%s\n", ypak_error(&pk), ytb_error(&tab));
        return 1;
    }
    block_col = ytb_col(&tab, "block"); image_col = ytb_col(&tab, "image"); ori_col = ytb_col(&tab, "ori");
    kind_col = ytb_col(&tab, "kind"); answer_col = ytb_col(&tab, "answer");
    if (block_col < 0 || image_col < 0 || ori_col < 0 || kind_col < 0 || answer_col < 0)
        return fail("the table needs block, image, ori, kind and answer");
    /* images random inside a block, a block column level per block, the blocks in random order */
    if (!ytr_open(&trials, &(ytr_desc){ .table = &tab, .reps = reps, .order = YTR_ORDER_FULL_RANDOM,
            .groups = { .mode = YTR_GROUPS_BLOCKED, .factor = block_col, .order = YTR_GROUP_ORDER_RANDOM },
            .rng = ytr_splitmix, .rng_ctx = &seed }))
        return fail(ytr_error(&trials));

    /* the display and the graphics, opened together and primed */
    if (!ygfx_open_screen(&gfx, &scr, &(yscr_desc){ .backend = sim ? YSCR_BACKEND_SIM : 0, .windowed = !fullscreen },
                          &(ygfx_desc){ .background = { 0.3f, 0.3f, 0.3f } }))
        return fail(ygfx_error(&gfx));
    /* Every texture is made before the first frame. */
    for (i = 0; i < tab.n_rows && i < YTR_MAX_CONDITIONS; i++)
        if ((row_img[i] = load_image(ytb_text(&tab, i, image_col))) < 0) return 1;
    if ((i = load_image(MASK_NAME)) < 0) return 1;
    if (sim) printf("graded mask: not checked on the simulated display (its null backend draws nothing)\n");
    else if (mask_setup(&imgs[i]) < 0) return 1;
    fix = ygfx_shape(&(ygfx_shape_desc){ .shape = YGFX_CROSS, .w = 24, .h = 24, .shape_p = { 3 },
                                         .edge = YGFX_EDGE_COSINE, .edge_width = 1 });

    if (!ytl_open(&tl, &(ytl_desc){ .events = storage, .event_capacity = 64, .n_channels = N_CHANNELS }))
        return fail(ytl_error(&tl));
    if (!yrsp_open(&rsp, &(yrsp_desc){ .choices = keys, .n_choices = 2, .duration_ns = YRT_S(RESPONSE_S),
                                       .min_rt_ns = YRT_S(MIN_RT_S) }))
        return fail(yrsp_error(&rsp));

    /* the data file, and the settings beside it: the session's line too,
     * which the writer adds only with the session's columns */
    ypak_id(&pk, id);
    yscr_describe(&scr, line, sizeof line);
    ytr_format_meta(&trials, tmeta, sizeof tmeta);
    snprintf(meta, sizeof meta, "%spack=%s (%s)\nscreen=%s\nysp_response=%s\nkeyboard=%s\n", tmeta, id, pack_path, line,
             yrsp_version(), sim ? "synthetic (tier SIM)" : "SDL through yscr_event_input");
    snprintf(meta_path, sizeof meta_path, "%s.meta", out_path);
    if (!ytr_data_open(&data, &(ytr_data_desc){ .path = out_path, .cols = cols, .n_cols = (int)(sizeof cols / sizeof cols[0]),
                                                .meta_path = meta_path, .meta = meta }))
        return fail(ytr_data_error(&data));
    printf("pack %s\n", id);

    while (!aborted) {
        const float* v;
        int rc = yscr_begin(&scr, &f);
        if (rc == YSCR_QUIT) { aborted = 1; break; }
        if (rc != YSCR_OK) { failed = fail(yscr_error(&scr)); break; }

        if (!in_trial) {
            /* a new trial: the block's cue first, then fixation, then the image */
            ytl_seq q;
            const img* im;
            if (ytr_next(&trials, &ti) < 0) break;
            r.block = ytb_text(&tab, ti.condition, block_col);
            r.stimulus = ytb_text(&tab, ti.condition, image_col);
            r.kind = ytb_text(&tab, ti.condition, kind_col);
            r.answer = ytb_text(&tab, ti.condition, answer_col);
            r.ori = ytb_num(&tab, ti.condition, ori_col);
            rotation = !strcmp(r.block, "rotation");
            odd = !strcmp(r.kind, rotation ? "mirror" : "incongruent");
            /* a 1-channel image is coverage: light letters on the gray;
             * turned images are read with linear filtering */
            im = &imgs[row_img[ti.condition]];
            cur = ygfx_image(&gfx, &(ygfx_image_desc){ .tex = im->tex, .w = (float)(im->w * (rotation ? 2 : 3)),
                .h = (float)(im->h * (rotation ? 2 : 3)), .ori = (float)r.ori, .linear = r.ori != 0,
                .coverage = im->format == YPAK_TEX_R8, .tint = { 0.9f, 0.9f, 0.9f, 1 } });
            ytl_clear(&tl, TRIAL);
            ytl_anchor(&tl, TRIAL, f.onset, 0);
            q = ytl_seq_on(&tl, TRIAL);
            if (ti.first_in_block) {
                const char* task = rotation ? "Rotation: F if the R is normal, J if it is mirrored"
                                            : "Navon: F if the large letter is H, J if it is S";
                printf("%s\n", task);   /* SDL and printf may allocate: only here, between blocks */
                if (yscr_window(&scr)) SDL_SetWindowTitle(yscr_window(&scr), task);
                ytl_on(&q, CUE_ON);
                ytl_wait(&q, YRT_S(CUE_S));
                ytl_off(&q, CUE_ON);
            }
            ytl_on(&q, FIX_ON);
            ytl_wait(&q, YRT_S(FIXATION_S));
            ytl_off(&q, FIX_ON);
            ytl_on(&q, IMAGE_ON);
            if (q.err) { failed = fail(ytl_strerror(q.err)); break; }
            yrsp_arm(&rsp, f.onset);
            in_trial = responding = 1;
            n_sim_ev = sim_next = 0;
        }

        yrsp_feed_screen(&scr, &rsp, 1);
        while (sim && sim_next < n_sim_ev && sim_ev[sim_next].t <= f.onset) yrsp_feed(&rsp, &sim_ev[sim_next++]);

        /* the image frame's flip record, when it arrives, is the final
         * onset; a response or the deadline ends the window: hide the
         * image on this frame */
        if (yrsp_update_frame(&rsp, &f) == YRSP_ENDED && responding) {
            ytl_seq q = ytl_seq_on(&tl, TRIAL);
            int64_t bt = 0;
            ytl_base_time(&tl, TRIAL, f.onset, &bt);
            ytl_at(&q, bt);
            ytl_off(&q, IMAGE_ON);
            ytl_wait(&q, YRT_S(ITI_S));
            ytl_mark(&q, END_OF_TRIAL, 0);
            if (q.err) { failed = fail(ytl_strerror(q.err)); break; }
            responding = 0;
        }

        n = ytl_evaluate(&tl, &(ytl_frame){ f.onset, f.period, f.index }, fired, 64);
        /* the image landed on this frame: its planned onset, for now */
        landed = yrsp_set_landing(&rsp, fired, n, IMAGE_ON);
        if (landed >= 0 && sim && ti.index != 7) {   /* the participant answers from the plan */
            if (ti.index < SIM_TRIALS) sim_plan[ti.index] = fired[landed].onset;
            sim_ev[0] = (yrsp_input){ .t = fired[landed].onset + YRT_S(sim_rt(rotation, r.ori, odd)), .type = YRSP_PRESS,
                .control = (uint32_t)yrsp_scancode(ti.index == 2 ? (!strcmp(r.answer, "f") ? "j" : "f") : r.answer),
                .stamp = YRSP_TIER_SIM, .value = 1 };
            sim_ev[1] = sim_ev[0];
            sim_ev[1].type = YRSP_RELEASE;
            sim_ev[1].value = 0;
            sim_ev[1].t = sim_ev[0].t + YRT_MS(80);
            n_sim_ev = 2;
        }
        for (i = 0; i < n; i++)
            if (fired[i].kind == YTL_MARK && fired[i].code == END_OF_TRIAL) {
                /* the trial is over, and the record of its image is in */
                yrsp_finish(&rsp, &res);
                r.trial_index = ti.index;
                r.correct = (res.flags & YRSP_R_RESPONDED) && !strcmp(keys[res.response].key, r.answer);
                ytr_update(&trials, (res.flags & YRSP_R_RESPONDED) ? r.correct : YTR_INVALID, NULL);
                if (yrsp_data_set(&data, &res) < 0 || ytr_data_write(&data, &r) < 0) failed = fail(ytr_data_error(&data));
                if (!(res.flags & YRSP_R_RESPONDED)) printf("trial %2d %-8s %-28s %3g too slow\n", ti.index, r.block, r.stimulus, r.ori);
                else printf("trial %2d %-8s %-28s %3g rt %.3f s, %s %s\n", ti.index, r.block, r.stimulus, r.ori,
                            YRT_TO_S(res.rt_ns), res.response_name, r.correct ? "correct" : "wrong");
                if (ti.index < SIM_TRIALS) {
                    sim_res[ti.index] = res;
                    sim_want[ti.index] = sim_rt(rotation, r.ori, odd);
                    sim_block[ti.index] = rotation;
                    sim_correct[ti.index] = r.correct;
                }
                rows++;
                in_trial = 0;
            }
        v = ytl_values(&tl);
        yscr_mark(&scr, YSCR_PHASE_EVALUATE);
        ygfx_begin(&gfx, &f);
        if (v[CUE_ON] > 0.5f && have_cue) ygfx_draw(&gfx, &cue);
        if (v[FIX_ON] > 0.5f) ygfx_draw(&gfx, &fix);
        if (v[IMAGE_ON] > 0.5f) ygfx_draw(&gfx, &cur);
        ygfx_end(&gfx);
        yscr_flip(&scr);
    }
    if (ytr_data_finish(&data) < 0) failed = fail(ytr_data_error(&data));
    ygfx_close(&gfx);
    yscr_close(&scr);
    ypak_close(&pk);
    printf("%d trials written to %s%s\n", rows, out_path, aborted ? " (stopped with Shift+Esc)" : "");

    if (sim && !failed && reps != 1) printf("--sim: the self-check needs the default --reps\n");
    else if (sim && !failed) {
        int bad = rows != SIM_TRIALS, changes = 0;
        if (bad) fprintf(stderr, "trial_images: --sim: %d rows, not %d\n", rows, SIM_TRIALS);
        for (i = 0; i < rows && i < SIM_TRIALS; i++) {
            const yrsp_result* q = &sim_res[i];
            int ok = 1;
            if (i > 0 && sim_block[i] != sim_block[i - 1]) changes++;
            if (i == 7) ok = !(q->flags & YRSP_R_RESPONDED) && (q->flags & YRSP_R_TIMEOUT);
            else ok = (q->flags & YRSP_R_RESPONDED) && /* script from the plan, RT from the flip record */
                      llabs(q->rt_ns + (q->t_onset - sim_plan[i]) - YRT_S(sim_want[i])) <= 1000;
            ok = ok && sim_correct[i] == (i != 2 && i != 7);
            ok = ok && q->onset_tier == YSCR_TIER_SIM && q->onset_src == YRSP_ONSET_FLIP;
            if (!ok) {
                fprintf(stderr, "trial_images: --sim: trial %d: rt %.9f (want %.9f) flags 0x%x correct %d\n", i, YRT_TO_S(q->rt_ns),
                        sim_want[i], (unsigned)q->flags, sim_correct[i]);
                bad = 1;
            }
        }
        if (changes != 1) { fprintf(stderr, "trial_images: --sim: the blocks changed %d times, not once\n", changes); bad = 1; }
        if (bad) return 1;
        printf("--sim: every trial came out as scripted\n");
    }
    return failed;
}
