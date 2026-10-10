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
 * The data file has one row per trial: trial_index, block, stimulus, ori,
 * kind, answer, response, rt, correct (jsPsych-style names first); then
 * onset_tier, onset_src, rsp_tier, rsp_flags, n_anticipations. Times in
 * seconds. Shift+Esc stops the session; the rows so far are kept.
 *
 * --sim runs the 12 conditions once on the simulated display with a
 * synthetic participant: 60 ms slower on incongruent Navon figures, 2 ms
 * per degree of turn and 0.1 s more for a mirror image, one wrong key and
 * one timeout. It checks the rows, that each block ran whole, and the RTs,
 * and exits 1 on a mismatch. About 22 s.
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

/* The --sim participant; trial 2 presses the other key, trial 7 none. */
#define SIM_TRIALS 12
static double sim_rt(int rotation, double ori, int odd) {
    return rotation ? 0.50 + 0.002 * ori + (odd ? 0.10 : 0.0) : 0.50 + (odd ? 0.06 : 0.0);
}

static yscr_screen scr;
static ygfx_gfx gfx;
static ypak_pack pk;
static ytl_timeline tl;
static ytl_event storage[64];
static ytr_trials trials;
static yrsp_collector rsp;
static ytb_table tab;
static ygfx_stim fix, cur, cue;
static int have_cue;
static float rd_u[MASK_W * MASK_W * 4], rd_m[MASK_W * MASK_W * 4];

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

static void sec(char* out, size_t cap, double s) {
    if (s != s) out[0] = 0;
    else snprintf(out, cap, "%.9f", s);
}

int main(int argc, char** argv) {
    yscr_desc sd;
    ygfx_desc gd;
    ygfx_shape_desc fd;
    ytl_desc td;
    ytr_desc rd;
    yrsp_desc pd;
    ypak_desc kd;
    ypak_entry e;
    yscr_frame f;
    ytl_event fired[16];
    ytr_trial_info ti = { 0 };
    yrsp_result res, sim_res[SIM_TRIALS];
    yrsp_input sim_ev[2];
    double sim_want[SIM_TRIALS];
    int sim_block[SIM_TRIALS], sim_correct[SIM_TRIALS], row_img[YTR_MAX_CONDITIONS];
    FILE* out;
    char line[1024], meta[2048], id[72];
    const char* out_path = "trial_images.csv";
    const char* pack_path = TRIAL_IMAGES_PACK;
    const void* p;
    uint64_t seed = 20261009;
    int64_t image_frame = -1;
    int i, n, sim = 0, reps = 0, fullscreen = 0, aborted = 0, failed = 0, rows = 0;
    int in_trial = 0, responding = 0, n_sim_ev = 0, sim_next = 0, block_col, image_col, ori_col, kind_col, answer_col;
    int rotation = 0, odd = 0;
    double ori = 0;
    char answer[8] = "";

    memset(&sd, 0, sizeof sd);
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
    if (reps == 0) reps = sim ? 1 : 2;
    if (reps < 1 || reps > 50) { fprintf(stderr, "trial_images: --reps takes 1..50\n"); return 2; }

    /* The pack: mapped, each entry verified when first used. */
    memset(&kd, 0, sizeof kd);
    kd.path = pack_path;
    if (!ypak_open(&pk, &kd)) { fprintf(stderr, "trial_images: %s: %s\n", pack_path, ypak_error(&pk)); return 1; }
    if (ypak_find(&pk, "conditions.pstb", &e) != 0 || !(p = ypak_data(&pk, &e)) || !ytb_open_view(&tab, p, (size_t)e.size)) {
        fprintf(stderr, "trial_images: conditions.pstb: %s%s\n", ypak_error(&pk), ytb_error(&tab));
        return 1;
    }
    block_col = ytb_col(&tab, "block"); image_col = ytb_col(&tab, "image"); ori_col = ytb_col(&tab, "ori");
    kind_col = ytb_col(&tab, "kind"); answer_col = ytb_col(&tab, "answer");
    if (block_col < 0 || image_col < 0 || ori_col < 0 || kind_col < 0 || answer_col < 0) {
        fprintf(stderr, "trial_images: the table needs block, image, ori, kind and answer\n");
        return 1;
    }
    memset(&rd, 0, sizeof rd);
    rd.table = &tab;
    rd.reps = reps;
    rd.order = YTR_ORDER_FULL_RANDOM;       /* inside a block */
    rd.groups.mode = YTR_GROUPS_BLOCKED;    /* a block column level per block */
    rd.groups.factor = block_col;
    rd.groups.order = YTR_GROUP_ORDER_RANDOM;
    rd.rng = ytr_splitmix;
    rd.rng_ctx = &seed;
    if (!ytr_open(&trials, &rd)) { fprintf(stderr, "trial_images: %s\n", ytr_error(&trials)); return 1; }

    if (sim) sd.backend = YSCR_BACKEND_SIM;
    sd.windowed = !fullscreen;
    if (!yscr_open(&scr, &sd)) { fprintf(stderr, "trial_images: %s\n", yscr_error(&scr)); return 1; }
    memset(&gd, 0, sizeof gd);
    gd.screen = &scr;
    gd.background[0] = gd.background[1] = gd.background[2] = 0.3f;
    if (!ygfx_open(&gfx, &gd)) { fprintf(stderr, "trial_images: %s\n", ygfx_error(&gfx)); return 1; }
    /* Every texture is made before the first frame. */
    for (i = 0; i < tab.n_rows && i < YTR_MAX_CONDITIONS; i++)
        if ((row_img[i] = load_image(ytb_text(&tab, i, image_col))) < 0) return 1;
    if ((i = load_image(MASK_NAME)) < 0) return 1;
    if (sim) printf("graded mask: not checked on the simulated display (its null backend draws nothing)\n");
    else if (mask_setup(&imgs[i]) < 0) return 1;
    memset(&fd, 0, sizeof fd);
    fd.shape = YGFX_CROSS; fd.w = fd.h = 24; fd.shape_p[0] = 3;
    fd.edge = YGFX_EDGE_COSINE; fd.edge_width = 1;
    fix = ygfx_shape(&fd);

    memset(&td, 0, sizeof td);
    td.events = storage;
    td.event_capacity = 64;
    td.n_channels = N_CHANNELS;
    if (!ytl_open(&tl, &td)) { fprintf(stderr, "trial_images: %s\n", ytl_error(&tl)); return 1; }
    memset(&pd, 0, sizeof pd);
    pd.choices = keys;
    pd.n_choices = 2;
    pd.duration = RESPONSE_S;
    pd.minimum_valid_rt = MIN_RT_S;
    if (!yrsp_init(&rsp, &pd)) { fprintf(stderr, "trial_images: %s\n", yrsp_error(&rsp)); return 1; }

    out = fopen(out_path, "w");
    if (!out) { fprintf(stderr, "trial_images: cannot write %s\n", out_path); return 1; }
    ytr_format_meta(&trials, meta, sizeof meta);
    meta[strcspn(meta, "\r\n")] = 0;
    ypak_id(&pk, id);
    fprintf(out, "# %s\n# pack %s (%s)\n", meta, id, pack_path);
    yscr_describe(&scr, line, sizeof line);
    fprintf(out, "# %s\n# ysp_response %s; keyboard: %s\n", line, yrsp_version(),
            sim ? "synthetic (tier SIM)" : "SDL through yscr_event_input");
    fprintf(out, "trial_index,block,stimulus,ori,kind,answer,response,rt,correct,onset_tier,onset_src,rsp_tier,rsp_flags,"
                 "n_anticipations\n");
    printf("pack %s\n", id);

    for (;;) {
        SDL_Event ev;
        int64_t bt = 0;
        yrsp_input in;
        const float* v;
        int rc = yscr_begin(&scr, &f);
        if (rc == YSCR_QUIT) { aborted = 1; break; }
        if (rc != YSCR_OK) { fprintf(stderr, "trial_images: %s\n", yscr_error(&scr)); failed = 1; break; }

        for (i = 0; i < f.n_done; i++)   /* the image's flip record: its onset, final */
            if (f.done[i].index == image_frame) {
                yrsp_onset o = yrsp_onset_flip(&f.done[i]);
                yrsp_set_onset(&rsp, &o);
            }

        if (!in_trial) {
            ytl_seq q;
            ygfx_image_desc id_ = { 0 };
            const img* im;
            const char* a;
            if (ytr_next(&trials, &ti) < 0) break;
            rotation = !strcmp(ytb_text(&tab, ti.condition, block_col), "rotation");
            ori = ytb_num(&tab, ti.condition, ori_col);
            odd = !strcmp(ytb_text(&tab, ti.condition, kind_col), rotation ? "mirror" : "incongruent");
            a = ytb_text(&tab, ti.condition, answer_col);
            snprintf(answer, sizeof answer, "%s", a ? a : "");
            im = &imgs[row_img[ti.condition]];
            /* a 1-channel image is coverage: light letters on the gray;
             * turned images are read with linear filtering */
            id_.tex = im->tex; id_.w = (float)(im->w * (rotation ? 2 : 3)); id_.h = (float)(im->h * (rotation ? 2 : 3));
            id_.ori = (float)ori; id_.linear = ori != 0; id_.coverage = im->format == YPAK_TEX_R8;
            id_.tint[0] = id_.tint[1] = id_.tint[2] = 0.9f; id_.tint[3] = 1;
            cur = ygfx_image(&gfx, &id_);
            image_frame = -1;
            ytl_clear(&tl, TRIAL);
            ytl_anchor(&tl, TRIAL, f.onset, 0);
            q = ytl_seq_on(&tl, TRIAL);
            if (ti.first_in_block) {
                const char* task = rotation ? "Rotation: F if the R is normal, J if it is mirrored"
                                            : "Navon: F if the large letter is H, J if it is S";
                printf("%s\n", task);   /* SDL and printf may allocate: only here, between blocks */
                if (yscr_window(&scr)) SDL_SetWindowTitle(yscr_window(&scr), task);
                ytl_on(&q, CUE_ON);
                ytl_wait(&q, YTL_S(CUE_S));
                ytl_off(&q, CUE_ON);
            }
            ytl_on(&q, FIX_ON);
            ytl_wait(&q, YTL_S(FIXATION_S));
            ytl_off(&q, FIX_ON);
            ytl_on(&q, IMAGE_ON);
            if (q.err) { fprintf(stderr, "trial_images: timeline error %d\n", q.err); failed = 1; break; }
            yrsp_arm(&rsp, f.onset);
            in_trial = responding = 1;
            n_sim_ev = sim_next = 0;
        }

        while (yscr_poll(&scr, &ev, NULL))
            if (yscr_event_input(&scr, &ev, &in)) yrsp_feed(&rsp, &in);
        while (sim && sim_next < n_sim_ev && sim_ev[sim_next].t <= f.onset) yrsp_feed(&rsp, &sim_ev[sim_next++]);

        if (responding && yrsp_update(&rsp, f.onset) == YRSP_ENDED) {
            ytl_seq q = ytl_seq_on(&tl, TRIAL);
            ytl_base_time(&tl, TRIAL, f.onset, &bt);
            ytl_at(&q, bt);
            ytl_off(&q, IMAGE_ON);
            ytl_wait(&q, YTL_S(ITI_S));
            ytl_mark(&q, END_OF_TRIAL, 0);
            if (q.err) { fprintf(stderr, "trial_images: timeline error %d\n", q.err); failed = 1; break; }
            responding = 0;
        }

        n = ytl_evaluate(&tl, &(ytl_frame){ f.onset, f.period, f.index }, fired, 16);
        for (i = 0; i < n && i < 16; i++) {
            if (fired[i].kind == YTL_ONSET && fired[i].target == IMAGE_ON) {
                yrsp_onset o = yrsp_onset_landing(&fired[i]);
                yrsp_set_onset(&rsp, &o);
                image_frame = fired[i].frame;
                if (sim && ti.index != 7) {   /* the participant answers from the image's onset */
                    memset(sim_ev, 0, sizeof sim_ev);
                    sim_ev[0].t = o.t + YTL_S(sim_rt(rotation, ori, odd));
                    sim_ev[0].type = YRSP_PRESS;
                    sim_ev[0].control = (uint32_t)yrsp_scancode(ti.index == 2 ? (!strcmp(answer, "f") ? "j" : "f") : answer);
                    sim_ev[0].stamp = YRSP_TIER_SIM;
                    sim_ev[0].value = 1;
                    sim_ev[1] = sim_ev[0];
                    sim_ev[1].type = YRSP_RELEASE;
                    sim_ev[1].value = 0;
                    sim_ev[1].t = sim_ev[0].t + YTL_MS(80);
                    n_sim_ev = 2;
                }
            }
            if (fired[i].kind == YTL_MARK && fired[i].code == END_OF_TRIAL) {
                char rt[32];
                int correct;
                yrsp_finish(&rsp, &res);
                correct = (res.flags & YRSP_R_RESPONDED) && !strcmp(keys[res.response].key, answer);
                sec(rt, sizeof rt, res.rt);
                fprintf(out, "%d,%s,%s,%g,%s,%s,%s,%s,%d,%u,%u,%u,%u,%d\n", ti.index, ytb_text(&tab, ti.condition, block_col),
                        ytb_text(&tab, ti.condition, image_col), ori, ytb_text(&tab, ti.condition, kind_col), answer,
                        res.response_name ? res.response_name : "", rt, correct, (unsigned)res.onset_tier,
                        (unsigned)res.onset_src, (unsigned)res.stamp_tier, (unsigned)res.flags, res.n_anticipations);
                fflush(out);
                printf("trial %2d %-8s %-28s %3g rt %s response %s %s\n", ti.index, ytb_text(&tab, ti.condition, block_col),
                       ytb_text(&tab, ti.condition, image_col), ori, rt[0] ? rt : "-",
                       res.response_name ? res.response_name : "-",
                       !(res.flags & YRSP_R_RESPONDED) ? "(too slow)" : correct ? "correct" : "wrong");
                ytr_update(&trials, (res.flags & YRSP_R_RESPONDED) ? correct : YTR_INVALID, NULL);
                if (ti.index < SIM_TRIALS) {
                    sim_res[ti.index] = res;
                    sim_want[ti.index] = sim_rt(rotation, ori, odd);
                    sim_block[ti.index] = rotation;
                    sim_correct[ti.index] = correct;
                }
                rows++;
                in_trial = 0;
            }
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
    fclose(out);
    ygfx_close(&gfx);
    yscr_close(&scr);
    ypak_close(&pk);
    printf("%d trials written to %s%s\n", rows, out_path, aborted ? " (stopped with Shift+Esc)" : "");

    if (sim && !failed && reps != 1) printf("--sim: the self-check needs the default --reps\n");
    else if (sim && !failed) {
        int bad = rows != SIM_TRIALS, changes = 0;
        if (bad) fprintf(stderr, "trial_images: --sim: %d rows, not %d\n", rows, SIM_TRIALS);
        for (i = 0; i < rows && i < SIM_TRIALS; i++) {
            const yrsp_result* r = &sim_res[i];
            int ok = 1;
            if (i > 0 && sim_block[i] != sim_block[i - 1]) changes++;
            if (i == 7) ok = !(r->flags & YRSP_R_RESPONDED) && (r->flags & YRSP_R_TIMEOUT);
            else ok = (r->flags & YRSP_R_RESPONDED) && fabs(r->rt - sim_want[i]) < 1e-6;
            ok = ok && sim_correct[i] == (i != 2 && i != 7);
            ok = ok && r->onset_tier == YSCR_TIER_SIM && r->onset_src == YRSP_ONSET_FLIP;
            if (!ok) {
                fprintf(stderr, "trial_images: --sim: trial %d: rt %.9f (want %.9f) flags 0x%x correct %d\n", i, r->rt,
                        sim_want[i], (unsigned)r->flags, sim_correct[i]);
                bad = 1;
            }
        }
        if (changes != 1) { fprintf(stderr, "trial_images: --sim: the blocks changed %d times, not once\n", changes); bad = 1; }
        if (bad) return 1;
        printf("--sim: every trial came out as scripted\n");
    }
    return failed;
}
