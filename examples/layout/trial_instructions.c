/* trial_instructions.c - instruction pages: paragraphs laid out once by
 * pack/layout (Skribidi) over ysp/outline.h curve sets, rendered into one
 * ysp/gfx.h target per page in a setup pass, then shown as images and
 * turned by keys. Each page view's onset and viewing time come from flip
 * records, the key's RT from ysp/response.h.
 *
 * Mirrors jsPsych's instructions plugin (pages, key_forward and
 * key_backward, the view history with each page's viewing time) and
 * PsychoPy's Coder demos basic/hello_world.py and
 * stimuli/text/textbox_simple.py (text on screen). Written from their
 * descriptions in docs/rig_spec.md 14 and 15; no PsychoPy code (GPL-3)
 * was copied.
 *
 * Four pages of two or three paragraphs, 26 px text in a column of 900 px
 * (less in a narrower window), each with a footer ("Page 2 of 4. Left:
 * back. Right: next."). Right turns forward, Left back (not on the first
 * page); Right on the last page ends the instructions. Every page is laid
 * out and rendered at setup; the frame loop draws one image a frame, at
 * whole pixels. ysp/gfx.h's TARGETS section states that a page drawn so
 * has the runs' coverage bit for bit, at 0.5 ms a frame against 7.5 ms
 * for the runs (its measurement, 1920 x 1200).
 *
 * THE TIMES. The key turns the page on the first frame that begins after
 * it; that frame's flip record is the new view's onset and ends the view
 * before it (the last view ends at a blank frame's flip). viewing_time_s is
 * the difference of two flip records; rt_s is the key from the view's
 * onset. The key that turned the page is usually still down when the next
 * view opens: rsp_flags then has HELD_AT_OPEN (256).
 *
 * Font: Arial (Windows) or DejaVu Sans (Linux), or --font. Without one,
 * --sim runs and checks the views with nothing drawn (as on a CI runner);
 * a window run stops, since a reader would see no text.
 *
 * Data (--out, ysp/trials.h's writer): one row per view: view_index,
 * page_index, onset_s (from the first view's onset), viewing_time_s,
 * end_tier (the tier of the flip that ended it), then the response's
 * columns (YRSP_DATA_COLS: response, rt_s, onset_src, onset_tier, ...).
 * FILE.meta has the screen, the layout stamp (versions and font hashes)
 * and the page sizes.
 *
 * --sim: the simulated display on ysp/rt.h's virtual clock and a scripted
 * reader who turns 0, 1, 2, back to 1, 2, 3 and out, with a Left on the
 * first page that must change nothing. It checks each view (page, RT as
 * scripted from the flip record, viewing time from two flip records, the
 * page turned within two frames of the key, with no frame held by the
 * host) and exits 1 on a mismatch. 3 idle runs and one beside a CPU burner
 * per core wrote the same bytes, the same on MSVC, MinGW gcc and WSL gcc.
 * Under a second; the layout time it prints is real (yrt_real_ns()).
 *
 * Usage: trial_instructions [--sim] [--auto] [--fullscreen] [--font PATH] [--out FILE]
 *   --auto  the --sim reader and checks in a window (four frames for the
 *           page turn there: a compositor can hold a flip)
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
#define YSP_TRIALS_IMPLEMENTATION
#include "ysp/trials.h"
#define YSP_RESPONSE_IMPLEMENTATION
#include "ysp/response.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N_PAGES    4
#define MAX_PARAS  3
#define TEXT_PX    26
#define FOOT_PX    18
#define COLUMN_MAX 900       /* px; narrower in a smaller window */
#define MARGIN_PX  40
#define PARA_GAP   18
#define MAX_RUNS   4
#define MAX_VIEWS  256

#ifdef _WIN32
#define DEFAULT_FONT "C:/Windows/Fonts/arial.ttf"
#else
#define DEFAULT_FONT "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
#endif

static const char* const pages[N_PAGES][MAX_PARAS] = {
    { "Welcome. This session takes about twenty minutes, with a short break in the middle.",
      "In each trial a cloud of dots appears in the middle of the screen. Some of the dots move together, to the "
      "left or to the right; the others move at random.",
      "Read each page at your own pace. Press Right to go on and Left to go back." },
    { "Your task is to say which way the dots that move together are going.",
      "Press the Left arrow key for left and the Right arrow key for right. Answer as quickly as you can, but try "
      "to be right: a fast guess is not what we need.",
      NULL },
    { "Keep your eyes on the small cross in the middle of the screen while the dots are shown. Do not follow the "
      "dots with your eyes.",
      "Some trials are hard on purpose. When you cannot tell, make your best guess. Nobody gets all of them right.",
      "If you need to stop, tell the experimenter. Do not press keys other than the two arrows." },
    { "There are four blocks of trials. Each block starts with this reminder: Left arrow for left, Right arrow for "
      "right, eyes on the cross.",
      "When you are ready, press Right to start the first block.",
      NULL },
};

static const yrsp_choice keys_first[] = { { .key = "right", .name = "next" } };
static const yrsp_choice keys_both[] = { { .key = "left", .name = "back" }, { .key = "right", .name = "next" } };

typedef struct row {
    int view_index, page_index, end_tier;
    int64_t onset_ns, viewing_time_ns;
} row;
static const ytr_col cols[] = {
    YTR_COL(row, view_index, YTR_COL_INT), YTR_COL(row, page_index, YTR_COL_INT),
    YTR_COL(row, onset_ns, YTR_COL_TIME_NS), YTR_COL(row, viewing_time_ns, YTR_COL_TIME_NS),
    YTR_COL(row, end_tier, YTR_COL_INT), YRSP_DATA_COLS,
};

static yscr_screen scr;
static ygfx_gfx gfx;
static yol_ctx ol;
static ylay_lib* L;
static ygfx_cset sets[MAX_RUNS];
static ytr_data data;
static yrsp_collector rsp[2];     /* the first page (Right only), the others */

/* A page: its paragraphs and footer as blocks, its target and the image of it. */
typedef struct page {
    ylay_block b[MAX_PARAS + 1];
    float y[MAX_PARAS + 1];
    int n;
    ygfx_tex t;
    ygfx_stim img;
} page;
static page pg[N_PAGES];
static int column = COLUMN_MAX;   /* the text's width, px */

/* --sim: the reader. Each view's key, s from the view's onset; a Left on
 * the first page before its Right, which must change nothing. */
typedef struct sim_view { int page; const char* key; double t; } sim_view;
static const sim_view script[] = {
    { 0, "right", 2.0 }, { 1, "right", 1.5 }, { 2, "left", 1.0 }, { 1, "right", 0.8 }, { 2, "right", 1.2 }, { 3, "right", 1.0 },
};
#define N_SCRIPT ((int)(sizeof script / sizeof script[0]))
#define SIM_FIRST_LEFT_S 0.5

/* --sim: a press of `key` at t and its release 0.08 s later, kept in time
 * order after the events already fed (from `next`). The release goes to
 * whichever view is open then, as SDL's would. Returns the new count. */
static int sim_tap(yrsp_input* ev, int n, int next, int64_t t, const char* key) {
    int i;
    for (i = 0; i < 2 && n < 4 * N_SCRIPT; i++) {
        yrsp_input e;
        int k = n++;
        memset(&e, 0, sizeof e);
        e.t = t + (i ? YRT_MS(80) : 0);
        e.kind = YRSP_KIND_KEYBOARD;
        e.type = i ? YRSP_RELEASE : YRSP_PRESS;
        e.control = (uint32_t)yrsp_scancode(key);
        e.stamp = YRSP_TIER_SIM;
        e.value = i ? 0.0f : 1.0f;
        while (k > next && ev[k - 1].t > e.t) { ev[k] = ev[k - 1]; k--; }
        ev[k] = e;
    }
    return n;
}

static int fail(const char* why) {
    fprintf(stderr, "trial_instructions: %s\n", why);
    return 1;
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

static int lay(ylay_block* b, const char* s, float px) {
    ylay_style st;
    memset(&st, 0, sizeof st);
    st.size = px; st.width = (float)column; st.line_height = 1.25f; st.align = YLAY_ALIGN_START; st.dir = YLAY_DIR_LTR; st.lang = "en";
    return ylay_layout(L, s, -1, &st, NULL, 0, b) == YLAY_OK ? 0 : -1;
}

/* A block's runs into the bound target, its top-left at (MARGIN, y). */
static int draw_block(const ylay_block* b, float y, const float* color) {
    uint32_t r;
    int rc = 0;
    for (r = 0; r < b->n_runs; r++) {
        const ylay_run* run = &b->runs[r];
        ygfx_crun_desc d;
        ygfx_stim s;
        if (!run->n || run->font >= MAX_RUNS) continue;
        memset(&d, 0, sizeof d);
        d.set = sets[run->font]; d.items = b->items + run->first; d.n = (int)run->n; d.size = run->size;
        d.place = YGFX_TOP_LEFT; d.anchor = YGFX_TOP_LEFT;
        d.x = MARGIN_PX; d.y = y; d.w = b->w > 1 ? b->w : 1; d.h = b->h > 1 ? b->h : 1;
        d.color[0] = color[0]; d.color[1] = color[1]; d.color[2] = color[2];
        s = ygfx_crun(&gfx, &d);
        if (ygfx_draw(&gfx, &s) < 0) rc = -1;
    }
    return rc;
}

/* Every page laid out, its glyph sets on the GPU, each page drawn into its
 * own target in one setup pass. Returns the target height, or < 0. */
static int setup_pages(void) {
    static const float TEXT[3] = { 0.80f, 0.80f, 0.80f }, FOOT[3] = { 0.45f, 0.45f, 0.45f };
    static const float zero[4] = { 0, 0, 0, 0 };
    float h_max = 0, sw, sh;
    int p, k, i, nf, th;
    ygfx_size(&gfx, &sw, &sh);
    if (sw > 0 && (int)sw - 2 * MARGIN_PX - 40 < column) column = (int)sw - 2 * MARGIN_PX - 40;
    if (column < 300) column = 300;
    for (p = 0; p < N_PAGES; p++) {
        char foot[96];
        float y = MARGIN_PX;
        for (k = 0; k < MAX_PARAS && pages[p][k]; k++) {
            if (lay(&pg[p].b[k], pages[p][k], TEXT_PX) < 0) return -1;
            pg[p].y[k] = y;
            y += pg[p].b[k].h + PARA_GAP;
        }
        snprintf(foot, sizeof foot, "Page %d of %d.   %sRight: %s.", p + 1, N_PAGES, p ? "Left: back.   " : "",
                 p == N_PAGES - 1 ? "start" : "next");
        if (lay(&pg[p].b[k], foot, FOOT_PX) < 0) return -1;
        pg[p].y[k] = y + PARA_GAP;
        pg[p].n = k + 1;
        if (pg[p].y[k] + pg[p].b[k].h > h_max) h_max = pg[p].y[k] + pg[p].b[k].h;
    }
    nf = ylay_font_count(L);
    for (i = 0; i < nf && i < MAX_RUNS; i++) {
        ylay_font_info fi;
        ygfx_cset_desc d;
        ylay_font(L, i, &fi);
        memset(&d, 0, sizeof d);
        d.texels = fi.set->texels; d.n_texels = fi.set->n_texels; d.words = fi.set->words; d.n_words = fi.set->n_words;
        if (d.n_texels && !(sets[i] = ygfx_cset_make(&gfx, &d)).id) return -1;
    }
    th = (int)ceilf(h_max) + MARGIN_PX;
    for (p = 0; p < N_PAGES; p++) {
        ygfx_image_desc id;
        pg[p].t = ygfx_target(&gfx, &(ygfx_target_desc){ .w = column + 2 * MARGIN_PX, .h = th, .format = YGFX_RGBA16F });
        if (!pg[p].t.id) return -1;
        memset(&id, 0, sizeof id);
        id.tex = pg[p].t; id.place = YGFX_TOP_LEFT; id.anchor = YGFX_TOP_LEFT;
        id.x = floorf(0.5f * (sw - (float)(column + 2 * MARGIN_PX)));   /* whole px: texels land on pixels */
        id.y = floorf(0.5f * (sh - (float)th));
        pg[p].img = ygfx_image(&gfx, &id);
    }
    if (ygfx_begin_setup(&gfx) < 0) return -1;
    for (p = 0; p < N_PAGES; p++) {
        if (ygfx_begin_target(&gfx, pg[p].t, zero) < 0) return -1;
        for (k = 0; k < pg[p].n; k++)
            if (draw_block(&pg[p].b[k], pg[p].y[k], k == pg[p].n - 1 ? FOOT : TEXT) < 0) return -1;
        if (ygfx_end_target(&gfx) < 0) return -1;
    }
    return ygfx_end_setup(&gfx) < 0 ? -1 : th;
}

int main(int argc, char** argv) {
    static row views[MAX_VIEWS];
    static yrsp_result res[MAX_VIEWS];
    static int64_t view_frame[MAX_VIEWS];   /* the view's first frame */
    static int view_tier[MAX_VIEWS];        /* its flip's tier */
    static int view_stall[MAX_VIEWS];       /* the host held its first frame: a vblank skipped or dropped */
    int end_stall = 0;
    yscr_frame f;
    yscr_caps caps;
    yrsp_source source;
    yrsp_desc pd;
    static yrsp_input sim_ev[4 * N_SCRIPT];
    char line[1024], stamp[2048], meta[4096], meta_path[1100];
    const char *out_path = "trial_instructions.csv", *font_path = DEFAULT_FONT;
    uint8_t* font = NULL;
    size_t font_n = 0;
    int i, sim = 0, scripted = 0, fullscreen = 0, aborted = 0, failed = 0, have_font = 0, th = 0, sim_bad = 0;
    int n_views = 0, n_written = 0, page_now = 0, ended = 0, n_sim_ev = 0, sim_next = 0, end_tier = 0;
    int64_t end_frame = -1, end_onset = 0, first_onset = 0, prev_vblank = -1;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--sim")) sim = scripted = 1;
        else if (!strcmp(argv[i], "--auto")) scripted = 1;
        else if (!strcmp(argv[i], "--fullscreen")) fullscreen = 1;
        else if (!strcmp(argv[i], "--font") && i + 1 < argc) font_path = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out_path = argv[++i];
        else {
            fprintf(stderr, "usage: trial_instructions [--sim] [--auto] [--fullscreen] [--font PATH] [--out FILE]\n");
            return 2;
        }
    }

    /* --sim runs on ysp/rt.h's virtual clock: the same records on any host */
    if (sim) yrt_virtual_begin(0);
    if (!ygfx_open_screen(&gfx, &scr, &(yscr_desc){ .backend = sim ? YSCR_BACKEND_SIM : 0, .windowed = !fullscreen },
                          &(ygfx_desc){ .background = { 0.08f, 0.08f, 0.08f } }))
        return fail(ygfx_error(&gfx));
    yscr_get_caps(&scr, &caps);
    if (!yol_open(&ol, NULL) || ylay_create(&L, &(ylay_desc){ .ol = &ol }) != YLAY_OK) return fail("the layout library did not open");
    font = read_file(font_path, &font_n);
    if (font && ylay_add_font(L, font, font_n, 0, font_path) >= 0) {
        int64_t t0 = yrt_real_ns();
        if ((th = setup_pages()) < 0) {
            fprintf(stderr, "trial_instructions: pages: %s %s\n", ylay_error(L), ygfx_error(&gfx));
            return 1;
        }
        printf("%d pages laid out and rendered into %d x %d targets in %.1f ms\n", N_PAGES, column + 2 * MARGIN_PX, th,
               (double)(yrt_real_ns() - t0) / 1e6);
        ylay_stamp(L, NULL, stamp, sizeof stamp);
        have_font = 1;
    } else {
        printf("trial_instructions: %s: %s; no text is drawn\n", font_path, font ? ylay_error(L) : "not found");
        if (!sim) return fail("a window run needs a font (--font PATH)");
        snprintf(stamp, sizeof stamp, "none (no font)");
    }
    ygfx_prime(&gfx);

    /* the page keys: Right on the first page, Left and Right after it */
    source = yrsp_sdl_source(&(yrsp_sdl_ctx){ .raw_keyboard = caps.raw_keyboard }, YRSP_KIND_KEYBOARD);
    memset(&pd, 0, sizeof pd);
    pd.sources = &source;
    pd.n_sources = 1;
    pd.choices = keys_first;
    pd.n_choices = 1;
    if (!yrsp_open(&rsp[0], &pd)) return fail(yrsp_error(&rsp[0]));
    pd.choices = keys_both;
    pd.n_choices = 2;
    if (!yrsp_open(&rsp[1], &pd)) return fail(yrsp_error(&rsp[1]));

    yscr_describe(&scr, line, sizeof line);
    printf("%s\n", line);
    snprintf(meta, sizeof meta, "screen=%s\nlayout=%s\npages=%d, %d px text in a %d px column, targets %d x %d RGBA16F\n"
             "ysp_response=%s\nkeyboard=%s\n", line, stamp, N_PAGES, TEXT_PX, column, column + 2 * MARGIN_PX, th,
             yrsp_version(), scripted ? "scripted (--sim, --auto)" : source.note);
    snprintf(meta_path, sizeof meta_path, "%s.meta", out_path);
    if (!ytr_data_open(&data, &(ytr_data_desc){ .path = out_path, .cols = cols, .n_cols = (int)(sizeof cols / sizeof cols[0]),
                                                .meta_path = meta_path, .meta = meta }))
        return fail(ytr_data_error(&data));

    while (!aborted) {
        yrsp_collector* c = &rsp[page_now > 0];
        int st, k;
        int rc = yscr_begin(&scr, &f);
        if (rc == YSCR_QUIT) { aborted = 1; break; }
        if (rc != YSCR_OK) { failed = fail(yscr_error(&scr)); break; }

        /* the flip records: a view's onset is its first frame's; the end
         * frame's record closes the last view */
        for (i = 0; i < f.n_done; i++) {
            const yscr_record* q = &f.done[i];
            if (!q->onset || (q->flags & YSCR_FLIP_PENDING)) continue;
            for (k = n_written; k < n_views; k++)
                if (!views[k].onset_ns && view_frame[k] == q->index) {
                    views[k].onset_ns = q->onset;
                    view_tier[k] = q->tier;
                    view_stall[k] |= q->dropped > 0;
                    if (scripted && k == n_views - 1 && k < N_SCRIPT && script[k].page == page_now) {
                        /* the reader's key from the view's onset; on the first
                         * page a Left before it, which is not a choice there */
                        if (k == 0) n_sim_ev = sim_tap(sim_ev, n_sim_ev, sim_next, q->onset + YRT_S(SIM_FIRST_LEFT_S), "left");
                        n_sim_ev = sim_tap(sim_ev, n_sim_ev, sim_next, q->onset + YRT_S(script[k].t), script[k].key);
                    }
                }
            if (ended && q->index == end_frame) { end_onset = q->onset; end_tier = q->tier; end_stall |= q->dropped > 0; }
        }
        /* a view is complete when its onset and the next one's (or the end's) are in */
        while (n_written < n_views && views[n_written].onset_ns) {
            row* w = &views[n_written];
            row out;
            int64_t next = n_written + 1 < n_views ? views[n_written + 1].onset_ns : ended ? end_onset : 0;
            if (!next) break;
            if (!first_onset) first_onset = w->onset_ns;
            w->viewing_time_ns = next - w->onset_ns;
            w->end_tier = n_written + 1 < n_views ? view_tier[n_written + 1] : end_tier;
            out = *w;
            out.onset_ns = w->onset_ns - first_onset;
            if (yrsp_data_set(&data, &res[n_written]) < 0 || ytr_data_write(&data, &out) < 0) failed = fail(ytr_data_error(&data));
            printf("view %d page %d: shown %.3f s, %s after %.3f s\n", w->view_index, w->page_index, YRT_TO_S(w->viewing_time_ns),
                   res[n_written].response_name ? res[n_written].response_name : "-", YRT_TO_S(res[n_written].rt_ns));
            n_written++;
        }
        if (ended && n_written == n_views) break;

        /* the key; it turns the page on this frame */
        if (!ended) {
            yrsp_feed_screen(&scr, c, 1);
            for (; scripted && sim_next < n_sim_ev && sim_ev[sim_next].t <= f.onset; sim_next++) yrsp_feed(c, &sim_ev[sim_next]);
            st = n_views ? yrsp_update_frame(c, &f) : YRSP_ENDED;
            if (st == YRSP_ENDED) {
                if (n_views) {
                    yrsp_result* r = &res[n_views - 1];
                    yrsp_finish(c, r);
                    if (r->response_name && !strcmp(r->response_name, "back")) page_now--;
                    else if (page_now == N_PAGES - 1) { ended = 1; end_frame = f.index; }
                    else page_now++;
                }
                if (!ended && n_views < MAX_VIEWS) {
                    /* a new view: its plan is this frame, its flip record the onset */
                    yrsp_onset o;
                    c = &rsp[page_now > 0];
                    yrsp_arm(c, f.onset);
                    memset(&o, 0, sizeof o);
                    o.t_ns = f.onset;
                    o.frame = f.index;
                    o.src = YRSP_ONSET_PLAN;
                    yrsp_set_onset(c, &o);
                    memset(&views[n_views], 0, sizeof views[n_views]);
                    views[n_views].view_index = n_views;
                    views[n_views].page_index = page_now;
                    view_frame[n_views] = f.index;
                    view_stall[n_views] = prev_vblank >= 0 && f.vblank - prev_vblank > 1;
                    n_views++;
                } else if (!ended) { ended = 1; end_frame = f.index; }
                if (ended) end_stall = prev_vblank >= 0 && f.vblank - prev_vblank > 1;
            }
        }

        yscr_mark(&scr, YSCR_PHASE_EVALUATE);
        ygfx_begin(&gfx, &f);
        if (have_font && !ended) ygfx_draw(&gfx, &pg[page_now].img);
        ygfx_end(&gfx);
        yscr_flip(&scr);
        prev_vblank = f.vblank;
    }
    if (ytr_data_finish(&data) < 0) failed = fail(ytr_data_error(&data));
    ygfx_close(&gfx);
    yscr_close(&scr);
    for (i = 0; i < N_PAGES; i++) {
        int k;
        for (k = 0; k < MAX_PARAS + 1; k++) ylay_block_free(&pg[i].b[k]);
    }
    ylay_destroy(L);
    yol_close(&ol);
    free(font);
    printf("%d views written to %s%s\n", n_written, out_path, aborted ? " (stopped with Shift+Esc)" : "");

    if (scripted && !failed && !aborted) {
        /* the key to the next page's flip: two periods on the simulated
         * grid, four in a window that a compositor runs */
        int64_t lag_max = (sim ? 2 : 4) * (caps.period_ns > 0 ? caps.period_ns : YRT_S(1.0 / 60)) + YRT_US(100);
        int n_late = 0;
        if (n_written != N_SCRIPT) { fprintf(stderr, "trial_instructions: --sim: %d views, not %d\n", n_written, N_SCRIPT); sim_bad = 1; }
        for (i = 0; i < n_written && i < N_SCRIPT; i++) {
            const row* w = &views[i];
            const yrsp_result* r = &res[i];
            int64_t lag = w->viewing_time_ns - r->rt_ns;   /* key to the next page's flip */
            /* in a window (--auto) not checked where the host held the turning frame (a vblank
             * skipped) or its flip (dropped); counted */
            int stall = i + 1 < n_written ? view_stall[i + 1] : end_stall;
            int ok = w->page_index == script[i].page && (r->flags & YRSP_R_RESPONDED) && r->rt_ns == YRT_S(script[i].t) &&
                     !strcmp(r->response_name, !strcmp(script[i].key, "left") ? "back" : "next") &&
                     r->onset_src == YRSP_ONSET_FLIP && (sim ? r->onset_tier == YSCR_TIER_SIM && w->end_tier == YSCR_TIER_SIM :
                     r->onset_tier > 0 && w->end_tier > 0) && lag >= 0 && ((!sim && stall) || lag <= lag_max);
            n_late += stall;   /* none on the virtual clock: there it fails */
            if (i == 0) ok = ok && r->n_anticipations + r->n_early == 0;   /* the Left on page 1 was not a response */
            if (!ok) {
                fprintf(stderr, "trial_instructions: --sim: view %d: page %d (want %d) rt %.9f (want %.3f) %s, shown %.9f, "
                                "onset src %d tier %d, the turn %.6f s after the key\n", i, w->page_index, script[i].page,
                        YRT_TO_S(r->rt_ns), script[i].t, r->response_name ? r->response_name : "-",
                        YRT_TO_S(w->viewing_time_ns), r->onset_src, r->onset_tier, YRT_TO_S(lag));
                sim_bad = 1;
            }
        }
        if (sim_bad) failed = 1;
        else printf("--sim: every view came out as scripted (%d pages turned late by the host)\n", n_late);
    }
    return failed;
}
