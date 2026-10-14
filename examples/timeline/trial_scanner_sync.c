/* trial_scanner_sync.c - an fMRI run: trials on a time base anchored at a
 * scanner's volume pulse (non-slip timing), the pulses as ysp/input.h
 * SYNC events from a key, a line board or an emulator thread, and each
 * stimulus onset in the data as an offset from its volume's pulse.
 *
 * It does what PsychoPy's Builder demo Hardware/fMRI/ and the Coder demo
 * hardware/fMRI_launchScan.py do. PsychoPy is GPL-3: their descriptions
 * were read (docs/rig_spec.md 15), no code was copied.
 *
 * The run: a gray ring while it waits for the scanner. The base of the
 * run starts at the pulse after --skip dummy volumes (time 0 at that
 * pulse's own stamp, not at the frame that saw it). Trial k's disc
 * appears at (1 + k x VOLS) x TR, plus 0 or TR/2 (a factor): every onset
 * is planned from the one anchor, so a slow frame or a late response
 * never moves a later trial. The disc is left or right of the cross for
 * 0.5 s; press 1 (left) or 2 (right) within 1 s, as a scanner's button
 * box sends them.
 *
 * Pulses (--sync):
 *   key      a key (--sync-key, default 5, the key most scanner
 *            interfaces send) becomes a YIN_KIND_SYNC event; it is never
 *            a response. The key's stamp is the pulse's time (SDL's raw
 *            keyboard path on Windows: ysp/screen.h INPUT has its error).
 *   board    the rig profile's role "scanner" (ysp/rigfile.h; a line
 *            board with firmware/ysp_line/ on the scanner's TTL output):
 *            its SYNC events, stamped on the board's clock fit.
 *   emulate  a ysp/rt.h worker thread makes a pulse every TR, each
 *            stamped with its deadline: a run without a scanner. Under
 *            --sim it is a virtual timer on the frame loop's thread
 *            instead: a real thread is not paced by the virtual clock.
 * Every source pushes its pulses into one wait-free ysp/rt.h ring; the
 * frame loop drains it. Default: board when the profile has the role,
 * else key.
 *
 * The data file has one row per trial (ysp/trials.h's data writer): the
 * session's columns (index, ..., side, offset_tr, key_answer), then
 * trial_index, correct, volume (the last pulse at or before the onset,
 * 0 = the anchor's), planned (the onset's base time, s from the anchor),
 * onset (the disc's flip onset, s from the anchor), slip (onset minus
 * planned: the landing on the frame grid and any drop), dropped (the
 * vblanks the disc's flip missed), from_pulse (the onset minus its
 * volume's pulse), then the response's (YRSP_DATA_COLS).
 * Onsets land on the first frame at or after their planned time
 * (YTL_LEAD_NONE), not on the nearest one: an onset planned at a pulse
 * (offset_tr 0) on the nearest frame can come before that pulse and then
 * count in the volume before (the emulator on the simulated display: 4.6
 * ms early, from_pulse near TR). The cost is up to one frame of slip.
 * from_pulse drifts when the scanner's clock and this PC's differ: the
 * base runs on the PC's clock. A scanner clock slower than the PC's puts
 * pulse m up to m x TR x the difference after its plan, so over a long run
 * an onset can still precede its pulse by that much; the pulses file shows
 * the drift. --pulses FILE gets every pulse (volume, t in s from the
 * anchor, the interval); the end of the run prints the mean TR the PC
 * measured and its difference from --tr in ppm.
 *
 * --sim: the simulated display, scripted pulses (TR 1 s, 2 dummies, the
 * scanner's clock 100 ppm fast against the PC's, so from_pulse grows by
 * 0.1 ms a volume) and a scripted participant (a wrong key on trial 1, no
 * response on trial 3). It checks every pulse interval and the anchor
 * (exact); that each disc's flip was planned for the vblank where the
 * timeline landed it, never before its planned time; that no onset comes
 * before its pulse, each in its planned volume, from_pulse minus the
 * offset in [0, one frame); volume and from_pulse against the pulses the
 * script implies (exact); and each response (key and RT exact). --sim
 * --sync emulate runs the emulator instead of the script, on the same
 * checks. --sim runs on ysp/rt.h's virtual clock, so its output is the
 * same on every run and host. Exits 1 on a mismatch. Under a second.
 *
 * Usage: trial_scanner_sync [--sim] [--fullscreen] [--tr S] [--vols N]
 *          [--skip N] [--reps N] [--sync key|board|emulate] [--sync-key K]
 *          [--rig NAME] [--seed N] [--out FILE] [--pulses FILE]
 *   --tr S     the repetition time (default 2; --sim: 1)
 *   --vols N   volumes per trial (default 2)
 *   --skip N   dummy volumes before the anchor (default 0; --sim: 2)
 *   --reps N   repetitions of each of the 4 conditions (default 4; --sim: 1)
 * Exit code: 0 (also after Shift+Esc), 1 when something did not open or a
 * --sim check failed, 2 for a bad argument.
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#define YSP_GFX_IMPLEMENTATION
#include "ysp/gfx.h"
#define YSP_TIMELINE_IMPLEMENTATION
#include "ysp/timeline.h"
#define YSP_TRIALS_IMPLEMENTATION
#include "ysp/trials.h"
#define YSP_RESPONSE_IMPLEMENTATION
#include "ysp/response.h"
#define YSP_RIGFILE_IMPLEMENTATION
#include "ysp/rigfile.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { WAIT_ON, FIX_ON, DISC_ON, N_CHANNELS };
enum { RUN = 1 };
enum { END_OF_TRIAL = 1 };
enum { SYNC_KEY = 1, SYNC_BOARD, SYNC_EMULATE, SYNC_SCRIPT };

#define STIM_S      0.5
#define RESPONSE_S  1.0
#define END_S       1.1     /* the row is written this long after the onset */
#define MIN_RT_S    0.1
#define SIM_DRIFT   -100e-6 /* --sim: the scanner's clock against the PC's */
#define MAX_PULSES  8192
#define MAX_ROWS    512

static const char conditions_csv[] =
    "side,offset_tr,key_answer\nleft,0,1\nright,0,2\nleft,0.5,1\nright,0.5,2\n";
static const yrsp_choice keys[] = { { .key = "1", .name = "left" }, { .key = "2", .name = "right" } };

typedef struct row {
    int trial_index, volume, dropped;
    bool correct;
    int64_t planned_ns, onset_ns, slip_ns, from_pulse_ns;
} row;
static const ytr_col cols[] = {
    YTR_COL(row, trial_index, YTR_COL_INT), YTR_COL(row, correct, YTR_COL_BOOL), YTR_COL(row, volume, YTR_COL_INT),
    YTR_COL(row, dropped, YTR_COL_INT),
    YTR_COL_D(row, planned_ns, YTR_COL_TIME_NS, 6), YTR_COL_D(row, onset_ns, YTR_COL_TIME_NS, 6),
    YTR_COL_D(row, slip_ns, YTR_COL_TIME_NS, 6), YTR_COL_D(row, from_pulse_ns, YTR_COL_TIME_NS, 6), YRSP_DATA_COLS,
};

static yscr_screen scr;
static ygfx_gfx gfx;
static ytl_timeline tl;
static ytl_event storage[64];
static ytr_trials trials;
static ytr_data data;
static yrsp_collector rsp;
static uint64_t table_arena[1024];
static ytb_table tab;
static yrig_profile prof;
static ydev_device board;
static yrt_worker emu;
static yrt_ring pulse_ring;
static unsigned char pulse_mem[YRT_RING_BYTES(1024)];
static yrt_event drained[1024];
static int64_t pulse_t[MAX_PULSES];
static int n_pulses;
static int64_t emu_tr;
static ygfx_stim waitring, fix, disc;
static const ygfx_bind binds[] = {
    { .stim = &waitring, .param = YGFX_P_VISIBLE, .channel = WAIT_ON },
    { .stim = &fix,      .param = YGFX_P_VISIBLE, .channel = FIX_ON  },
    { .stim = &disc,     .param = YGFX_P_VISIBLE, .channel = DISC_ON },
};

static int fail(const char* why) {
    fprintf(stderr, "trial_scanner_sync: %s\n", why);
    return 1;
}

/* Every pulse source ends here: a SYNC event's stamp into the ring. Any
 * thread; the ring is wait-free. */
static void pulse_in(const yin_event* e, int source) {
    yrt_event r;
    if (e->kind != YIN_KIND_SYNC || e->type != YIN_PRESS) return;
    memset(&r, 0, sizeof r);
    r.t_ns = e->t;
    r.source = (uint16_t)YRT_SRC_USER;
    r.kind = 1;
    r.aux = (uint32_t)source;
    (void)yrt_ring_push(&pulse_ring, &r);
}
static void board_sink(void* ctx, const yin_event* e) { (void)ctx; pulse_in(e, SYNC_BOARD); }

/* --sim's emulator: the same pulses as a virtual timer. */
static int64_t emu_timer(void* ctx, int64_t now) {
    yin_event e;
    (void)ctx;
    memset(&e, 0, sizeof e);
    e.t = now;
    e.kind = YIN_KIND_SYNC;
    e.type = YIN_PRESS;
    pulse_in(&e, SYNC_EMULATE);
    return now + emu_tr;
}

/* The emulator: a pulse at each deadline, stamped with it, then the next. */
static void emu_job(void* ctx, const yrt_job_info* info) {
    yin_event e;
    (void)ctx;
    if (info->flushed) return;   /* the close: no pulse */
    memset(&e, 0, sizeof e);
    e.t = info->deadline_ns;
    e.kind = YIN_KIND_SYNC;
    e.type = YIN_PRESS;
    pulse_in(&e, SYNC_EMULATE);
    (void)yrt_worker_submit(&emu, info->deadline_ns + emu_tr, emu_job, NULL);
}

/* The last pulse at or before t, or -1. */
static int volume_at(int64_t t) {
    int i = n_pulses - 1;
    while (i >= 0 && pulse_t[i] > t) i--;
    return i;
}

int main(int argc, char** argv) {
    static yrsp_result sim_res[MAX_ROWS];
    static row sim_row[MAX_ROWS];
    static int sim_key[MAX_ROWS];
    static int64_t sim_plan[MAX_ROWS], sim_vblank[MAX_ROWS], sim_resid[MAX_ROWS], sim_off[MAX_ROWS];
    yscr_caps caps;
    yscr_frame f;
    yrsp_source source;
    ytl_event fired[64];
    ytr_trial_info ti = { 0 };
    yrsp_result res = { 0 };
    yrsp_input sim_ev[2];
    row r = { 0 };
    SDL_Event ev;
    yin_event in;
    FILE* pf = NULL;
    char line[1024], meta[3072], meta_path[1100], err[256];
    const char *out_path = "trial_scanner_sync.csv", *pulses_path = NULL, *rig_name = NULL, *sync_key = "5";
    const char* sync_names[] = { "", "key", "board", "emulate", "script" };
    uint64_t seed = 20261013;
    double tr_s = 0, frac = 0;
    int i, n, sim = 0, fullscreen = 0, vols = 2, skip = -1, reps = 0, sync = 0, aborted = 0, failed = 0, rows = 0;
    int sim_bad = 0, in_trial = 0, side = 0, side_col, frac_col, sync_sc, k_trial = 0, anchored = 0;
    int n_sim_ev = 0, sim_next = 0, n_script = 0;
    int64_t tr, trial_ns, anchor = 0, planned = 0, onset = 0, sim_frame = -1, script_t0 = 0, script_tr = 0;
    int64_t disc_frame = -1, disc_planned = 0;   /* the disc's flip: the vblank it was planned for */
    int disc_dropped = 0;

    for (i = 1; i < argc; i++) {
        const char* v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(argv[i], "--sim")) sim = 1;
        else if (!strcmp(argv[i], "--fullscreen")) fullscreen = 1;
        else if (!strcmp(argv[i], "--tr") && v) { tr_s = atof(v); i++; }
        else if (!strcmp(argv[i], "--vols") && v) { vols = atoi(v); i++; }
        else if (!strcmp(argv[i], "--skip") && v) { skip = atoi(v); i++; }
        else if (!strcmp(argv[i], "--reps") && v) { reps = atoi(v); i++; }
        else if (!strcmp(argv[i], "--sync") && v) {
            sync = !strcmp(v, "key") ? SYNC_KEY : !strcmp(v, "board") ? SYNC_BOARD : !strcmp(v, "emulate") ? SYNC_EMULATE : -1;
            i++;
        } else if (!strcmp(argv[i], "--sync-key") && v) { sync_key = v; i++; }
        else if (!strcmp(argv[i], "--rig") && v) { rig_name = v; i++; }
        else if (!strcmp(argv[i], "--seed") && v) { seed = (uint64_t)strtoull(v, NULL, 10); i++; }
        else if (!strcmp(argv[i], "--out") && v) { out_path = v; i++; }
        else if (!strcmp(argv[i], "--pulses") && v) { pulses_path = v; i++; }
        else { sync = -1; break; }
    }
    /* --sim runs on ysp/rt.h's virtual clock: the same records on any host */
    if (sim && sync >= 0) yrt_virtual_begin(0);
    if (tr_s == 0) tr_s = sim ? 1.0 : 2.0;
    if (skip < 0) skip = sim ? 2 : 0;
    if (reps == 0) reps = sim ? 1 : 4;
    tr = YRT_S(tr_s);
    trial_ns = vols * tr;
    sync_sc = yrsp_scancode(sync_key);
    /* the next trial is added at the row before it, so it must still be ahead */
    if (sync < 0 || !(tr_s >= 0.2 && tr_s <= 30) || vols < 1 || vols > 64 || skip > 100 || reps < 1 ||
        4 * reps > MAX_ROWS || sync_sc < 0 || trial_ns - tr / 2 - YRT_S(END_S) < YRT_MS(100) || (sim && sync == SYNC_BOARD)) {
        fprintf(stderr, "usage: trial_scanner_sync [--sim] [--fullscreen] [--tr S] [--vols N] [--skip N] [--reps N] "
                        "[--sync key|board|emulate] [--sync-key K] [--rig NAME] [--seed N] [--out FILE] [--pulses FILE]\n"
                        "  a trial (vols x tr) must last at least tr/2 + %.1f s; --sim takes key (scripted) or emulate\n",
                END_S + 0.1);
        return 2;
    }
    if (!yrt_ring_open(&pulse_ring, &(yrt_ring_desc){ .memory = pulse_mem, .bytes = sizeof pulse_mem })) return fail("ring");

    /* the pulse source */
    if (sim && sync != SYNC_EMULATE) sync = SYNC_SCRIPT;
    if (!sim && sync != SYNC_KEY && sync != SYNC_EMULATE) {
        int rc = yrig_load(&prof, rig_name, err, sizeof err);
        if (rc != YRIG_OK && (rc != YRIG_ERR_MISSING || rig_name || sync == SYNC_BOARD)) return fail(err);
        if (rc == YRIG_OK && yrig_find(&prof, "scanner")) {
            ydev_desc base = { 0 };
            base.sink = board_sink;
            base.kind = YIN_KIND_SYNC;
            base.device = 1;
            if (!yrig_open_device(&board, &prof, "scanner", &base)) return fail(ydev_error(&board));
            if (ydev_wait_running(&board, YRT_S(3)) < 0) { fail(ydev_error(&board)); ydev_close(&board); return 1; }
            sync = SYNC_BOARD;
        } else if (sync == SYNC_BOARD) return fail("the rig profile has no role scanner");
        else sync = SYNC_KEY;
    }

    if (!ygfx_open_screen(&gfx, &scr, &(yscr_desc){ .backend = sim ? YSCR_BACKEND_SIM : 0, .windowed = !fullscreen },
                          &(ygfx_desc){ .background = { 0.5f, 0.5f, 0.5f } })) {
        fail(ygfx_error(&gfx));
        ydev_close(&board);
        return 1;
    }
    yscr_get_caps(&scr, &caps);
    waitring = ygfx_shape(&(ygfx_shape_desc){ .shape = YGFX_ANNULUS, .w = 60, .h = 60, .shape_p = { 24 },
                                              .edge = YGFX_EDGE_COSINE, .edge_width = 1, .color = { 0.35f, 0.35f, 0.35f } });
    fix = ygfx_shape(&(ygfx_shape_desc){ .shape = YGFX_CROSS, .w = 16, .h = 16, .shape_p = { 3 },
                                         .edge = YGFX_EDGE_COSINE, .edge_width = 1, .color = { 0.1f, 0.1f, 0.1f } });
    disc = ygfx_shape(&(ygfx_shape_desc){ .shape = YGFX_CIRCLE, .w = 80, .h = 80, .edge = YGFX_EDGE_COSINE,
                                          .edge_width = 1, .color = { 1, 1, 1 } });

    if (!ytb_open_csv(&tab, &(ytb_csv_desc){ .text = conditions_csv, .len = sizeof conditions_csv - 1,
                                             .arena = table_arena, .arena_size = sizeof table_arena }))
        return fail(ytb_error(&tab));
    side_col = ytb_col(&tab, "side");
    frac_col = ytb_col(&tab, "offset_tr");
    if (!ytr_open(&trials, &(ytr_desc){ .table = &tab, .reps = reps, .order = YTR_ORDER_RANDOM, .rng = ytr_splitmix,
                                        .rng_ctx = &seed }))
        return fail(ytr_error(&trials));
    if (!ytl_open(&tl, &(ytl_desc){ .events = storage, .event_capacity = 64, .n_channels = N_CHANNELS,
                                    .lead = YTL_LEAD_NONE,   /* pulse-locked: never before the plan */
                                    .initial = (const float[N_CHANNELS]){ 1, 0, 0 } }))
        return fail(ytl_error(&tl));
    source = yrsp_sdl_source(&(yrsp_sdl_ctx){ .raw_keyboard = caps.raw_keyboard }, YRSP_KIND_KEYBOARD);
    if (!yrsp_open(&rsp, &(yrsp_desc){ .choices = keys, .n_choices = 2, .duration_ns = YRT_S(RESPONSE_S),
                                       .min_rt_ns = YRT_S(MIN_RT_S), .sources = &source, .n_sources = 1 }))
        return fail(yrsp_error(&rsp));

    yscr_describe(&scr, line, sizeof line);
    printf("%s\npulses: %s%s%s, TR %.6f s, %d dummies, %d volumes a trial\n", line, sync_names[sync],
           sync == SYNC_KEY ? " " : "", sync == SYNC_KEY ? sync_key : "", tr_s, skip, vols);
    snprintf(meta, sizeof meta, "screen=%s\nsync=%s%s%s; tr_s %.9f; skip %d; vols %d; the base anchored at the "
             "pulse after the dummies; trial k at (1 + k vols) tr + offset_tr tr\nysp_response=%s\nkeyboard=%s\n",
             line, sync_names[sync], sync == SYNC_KEY ? " " : "", sync == SYNC_KEY ? sync_key : "", tr_s, skip, vols,
             yrsp_version(), sim ? "synthetic (tier SIM)" : source.note);
    snprintf(meta_path, sizeof meta_path, "%s.meta", out_path);
    if (!ytr_data_open(&data, &(ytr_data_desc){ .path = out_path, .trials = &trials, .cols = cols,
            .n_cols = (int)(sizeof cols / sizeof cols[0]), .meta_path = meta_path, .meta = meta }))
        return fail(ytr_data_error(&data));

    if (sync == SYNC_EMULATE) {
        emu_tr = tr;
        if (sim) {
            if (yrt_virtual_timer(emu_timer, NULL, yrt_now_ns() + YRT_S(0.5)) < 0) return fail("emulator");
        } else {
            if (!yrt_worker_open(&emu, NULL)) return fail(yrt_worker_error(&emu));
            if (yrt_worker_submit(&emu, yrt_now_ns() + YRT_S(0.5), emu_job, NULL) < 0) return fail("emulator");
        }
    }
    printf("waiting for the scanner%s\n", sync == SYNC_KEY ? " (or press the sync key)" : "");

    while (!aborted) {
        int rc = yscr_begin(&scr, &f);
        if (rc == YSCR_QUIT) { aborted = 1; break; }
        if (rc != YSCR_OK) { failed = fail(yscr_error(&scr)); break; }
        if (sync == SYNC_SCRIPT) {
            /* the scanner's pulses, once the clock has reached them; its
             * clock runs 100 ppm fast (SIM_DRIFT) */
            if (!script_t0) { script_t0 = f.onset + YRT_S(0.5); script_tr = tr + (int64_t)llround(tr_s * SIM_DRIFT * 1e9); }
            while (script_t0 + n_script * script_tr <= yrt_now_ns()) {
                yin_event e = { 0 };
                e.t = script_t0 + script_tr * n_script++;
                e.kind = YIN_KIND_SYNC;
                e.type = YIN_PRESS;
                pulse_in(&e, SYNC_SCRIPT);
            }
        }

        /* input: the sync key is a pulse, never a response */
        while (yscr_poll(&scr, &ev, NULL))
            if (yscr_event_input(&scr, &ev, &in)) {
                if (sync == SYNC_KEY && in.kind == YIN_KIND_KEYBOARD && in.control == (uint32_t)sync_sc) {
                    if (in.type == YIN_PRESS && !(in.flags & YIN_REPEAT)) { in.kind = YIN_KIND_SYNC; pulse_in(&in, SYNC_KEY); }
                } else yrsp_feed(&rsp, &in);
            }
        n = yrt_ring_drain(&pulse_ring, drained, 1024);
        for (i = 0; i < n; i++)
            if (drained[i].source == YRT_SRC_USER && n_pulses < MAX_PULSES) pulse_t[n_pulses++] = drained[i].t_ns;

        /* the anchor: the pulse after the dummies, at its own stamp */
        if (!anchored && n_pulses > skip) {
            anchor = pulse_t[skip];
            ytl_anchor(&tl, RUN, anchor, 0);
            anchored = 1;
            printf("anchored at volume %d (pulse %d)\n", 0, skip + 1);
        }

        if (anchored && !in_trial) {
            /* the next trial, at its place on the run's base */
            ytl_seq q;
            if (ytr_next(&trials, &ti) < 0 || ti.index >= MAX_ROWS) break;
            side = !strcmp(ytb_text(&tab, ti.condition, side_col), "right");
            frac = ytb_num(&tab, ti.condition, frac_col);
            disc.x = side ? 200.0f : -200.0f;
            planned = tr + k_trial * trial_ns + (int64_t)llround(frac * (double)tr);
            q = ytl_seq_on(&tl, RUN);
            if (k_trial == 0) { ytl_off(&q, WAIT_ON); ytl_on(&q, FIX_ON); }
            ytl_at(&q, planned);
            ytl_on(&q, DISC_ON);
            ytl_wait(&q, YRT_S(STIM_S));
            ytl_off(&q, DISC_ON);
            ytl_at(&q, planned + YRT_S(END_S));
            ytl_mark(&q, END_OF_TRIAL, 0);
            if (q.err) { failed = fail(ytl_strerror(q.err)); break; }
            yrsp_arm(&rsp, f.onset);
            onset = 0;
            in_trial = 1;
            k_trial++;
        }

        for (i = 0; i < f.n_done; i++)
            if (f.done[i].index == disc_frame) { disc_planned = f.done[i].planned; disc_dropped = (int)f.done[i].dropped; }

        /* --sim: the key from the disc's flip record */
        for (i = 0; sim && sim_frame >= 0 && i < f.n_done; i++)
            if (f.done[i].index == sim_frame && yrsp_onset_flip(&f.done[i]).final) {
                sim_ev[0].t = f.done[i].onset + YRT_S(0.4 + 0.05 * (ti.index % 3));
                sim_ev[1].t = sim_ev[0].t + YRT_MS(80);
                n_sim_ev = sim_key[ti.index] >= 0 ? 2 : 0;
                sim_next = 0;
                sim_frame = -1;
            }
        for (; sim && sim_next < n_sim_ev && sim_ev[sim_next].t <= yrt_now_ns(); sim_next++)
            yrsp_feed(&rsp, &sim_ev[sim_next]);
        yrsp_update_frame(&rsp, &f);   /* the disc's flip record makes RT final */

        n = ytl_evaluate(&tl, &(ytl_frame){ f.onset, f.period, f.index }, fired, 64);
        for (i = 0; i < n; i++) {
            if (fired[i].kind == YTL_ONSET && fired[i].target == DISC_ON) {
                yrsp_set_landing(&rsp, &fired[i], 1, DISC_ON);
                disc_frame = fired[i].frame;
                if (ti.index < MAX_ROWS) sim_resid[ti.index] = fired[i].residual;
                if (sim && ti.index < MAX_ROWS) {
                    int which = ti.index == 1 ? 2 : ti.index == 3 ? 0 : 1;   /* a wrong key, a timeout */
                    sim_key[ti.index] = which == 0 ? -1 : which == 1 ? side : 1 - side;
                    for (; sim_next < n_sim_ev; sim_next++) yrsp_feed(&rsp, &sim_ev[sim_next]);
                    memset(sim_ev, 0, sizeof sim_ev);
                    sim_ev[0].kind = YRSP_KIND_KEYBOARD;
                    sim_ev[0].type = YRSP_PRESS;
                    sim_ev[0].control = (uint32_t)yrsp_scancode(sim_key[ti.index] == 1 ? "2" : "1");
                    sim_ev[0].stamp = YRSP_TIER_SIM;
                    sim_ev[0].value = 1;
                    sim_ev[1] = sim_ev[0];
                    sim_ev[1].type = YRSP_RELEASE;
                    sim_ev[1].value = 0;
                    n_sim_ev = sim_next = 0;
                    sim_frame = fired[i].frame;
                }
            }
            if (fired[i].kind == YTL_MARK && fired[i].code == END_OF_TRIAL) {
                /* the onset's record and every pulse before it are in by now */
                yrsp_finish(&rsp, &res);
                onset = res.t_onset;
                r.trial_index = ti.index;
                r.correct = (res.flags & YRSP_R_RESPONDED) && res.response == side;
                r.volume = volume_at(onset) - skip;
                r.dropped = disc_dropped;
                r.planned_ns = planned;
                r.onset_ns = onset - anchor;
                r.slip_ns = onset - anchor - planned;
                r.from_pulse_ns = volume_at(onset) >= 0 ? onset - pulse_t[volume_at(onset)] : YTR_MISSING_NS;
                ytr_update(&trials, (res.flags & YRSP_R_RESPONDED) ? r.correct : YTR_INVALID, NULL);
                if (yrsp_data_set(&data, &res) < 0 || ytr_data_write(&data, &r) < 0) failed = fail(ytr_data_error(&data));
                printf("trial %2d %-5s planned %8.3f s  onset %+.3f ms  volume %3d  +%.4f s from its pulse  %s\n",
                       ti.index, side ? "right" : "left", YRT_TO_S(planned), YRT_TO_S(r.slip_ns) * 1e3, r.volume,
                       YRT_TO_S(r.from_pulse_ns), !(res.flags & YRSP_R_RESPONDED) ? "no response"
                                                  : r.correct ? "correct" : "wrong");
                if (ti.index < MAX_ROWS) {
                    sim_res[ti.index] = res;
                    sim_row[ti.index] = r;
                    sim_plan[ti.index] = planned;
                    sim_off[ti.index] = (int64_t)llround(frac * (double)tr);
                    sim_vblank[ti.index] = disc_planned;
                }
                rows++;
                in_trial = 0;
            }
        }
        ygfx_apply(binds, (int)(sizeof binds / sizeof binds[0]), ytl_values(&tl));
        yscr_mark(&scr, YSCR_PHASE_EVALUATE);
        ygfx_begin(&gfx, &f);
        ygfx_draw(&gfx, &waitring);
        ygfx_draw(&gfx, &fix);
        ygfx_draw(&gfx, &disc);
        ygfx_end(&gfx);
        yscr_flip(&scr);
    }
    if (sync == SYNC_EMULATE && !sim) yrt_worker_close(&emu);
    ydev_close(&board);
    if (ytr_data_finish(&data) < 0) failed = fail(ytr_data_error(&data));
    ygfx_close(&gfx);
    yscr_close(&scr);
    printf("%d trials written to %s%s\n", rows, out_path, aborted ? " (stopped with Shift+Esc)" : "");

    /* the pulses: the file, and the TR this PC's clock measured */
    if (pulses_path) {
        if (!(pf = fopen(pulses_path, "w"))) failed = fail("cannot write the pulses file");
        else {
            fprintf(pf, "volume,t_s,interval_s\n");
            for (i = 0; i < n_pulses; i++) {
                fprintf(pf, "%d,%.9f,", i - skip, YRT_TO_S(pulse_t[i] - anchor));
                if (i) fprintf(pf, "%.9f", YRT_TO_S(pulse_t[i] - pulse_t[i - 1]));
                fputc('\n', pf);
            }
            fclose(pf);
        }
    }
    if (n_pulses > skip + 1) {
        double mean = (double)(pulse_t[n_pulses - 1] - pulse_t[skip]) / (n_pulses - 1 - skip);
        printf("%d pulses; mean TR %.9f s on this PC's clock, %+.1f ppm against --tr\n", n_pulses, mean / 1e9,
               (mean / (double)tr - 1.0) * 1e6);
    }

    if (sim && !failed && !aborted) {
        int64_t want_tr = sync == SYNC_SCRIPT ? script_tr : tr, period = YRT_S(1) / 60;   /* the simulated display's */
        for (i = 1; i < n_pulses; i++)
            if (pulse_t[i] - pulse_t[i - 1] != want_tr) {
                fprintf(stderr, "trial_scanner_sync: --sim: pulse %d came %lld ns after the one before, not %lld\n", i,
                        (long long)(pulse_t[i] - pulse_t[i - 1]), (long long)want_tr);
                sim_bad = 1;
            }
        if ((sync == SYNC_SCRIPT && pulse_t[0] != script_t0) || n_pulses <= skip || anchor != pulse_t[skip]) {
            fprintf(stderr, "trial_scanner_sync: --sim: anchored at %lld, not at the pulse after the dummies\n", (long long)anchor);
            sim_bad = 1;
        }
        for (i = 0; i < rows; i++) {
            const yrsp_result* q = &sim_res[i];
            const row* x = &sim_row[i];
            /* the volume from the pulses' first time and the interval alone */
            int resp_want = sim_key[i] >= 0, vol = (int)((q->t_onset - pulse_t[0]) / want_tr);
            int64_t pulse_onset = q->t_onset - (pulse_t[0] + vol * want_tr);
            if (resp_want != ((q->flags & YRSP_R_RESPONDED) != 0) || (resp_want && q->response != sim_key[i]) ||
                (resp_want && llabs(q->rt_ns - YRT_S(0.4 + 0.05 * (i % 3))) > 1000) ||
                q->onset_tier != YSCR_TIER_SIM || q->onset_src != YRSP_ONSET_FLIP ||
                sim_resid[i] < 0 || sim_resid[i] >= period || sim_vblank[i] - anchor - x->planned_ns != sim_resid[i] || x->onset_ns != q->t_onset - anchor || x->planned_ns != sim_plan[i] ||
                x->volume != vol - skip || x->from_pulse_ns != pulse_onset || x->volume < 1 ||
                /* never before its pulse, in its planned volume, within a frame of the offset */
                x->volume != (int)((sim_plan[i] - sim_off[i]) / tr) || x->from_pulse_ns - sim_off[i] < 0 ||
                x->from_pulse_ns - sim_off[i] >= period) {
                fprintf(stderr, "trial_scanner_sync: --sim: trial %d: response %d (want %d) rt %.9f; planned %.9f "
                                "onset %.9f slip %.9f volume %d (want %d) from_pulse %.9f\n", i, q->response,
                        sim_key[i], YRT_TO_S(q->rt_ns), YRT_TO_S(x->planned_ns), YRT_TO_S(x->onset_ns),
                        YRT_TO_S(x->slip_ns), x->volume, vol - skip, YRT_TO_S(x->from_pulse_ns));
                sim_bad = 1;
            }
        }
        if (rows != 4 * reps) { fprintf(stderr, "trial_scanner_sync: --sim: %d rows, not %d\n", rows, 4 * reps); sim_bad = 1; }
        if (!sim_bad) printf("--sim: the anchor, every pulse and every trial came out as scripted\n");
        failed = sim_bad;
    }
    return failed;
}
