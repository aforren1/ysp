/* trial_eeg_triggers.c - event codes for an EEG recording: a stimulus code
 * at the stimulus's flip, a response code at the key event, and an end
 * code at the trial's last flip, all through one trigger output
 * (ysp/device.h), each write's record beside the flip and response records
 * in the data row.
 *
 * It does what PsychoPy's Builder demos Hardware/EEG_parallel_component/,
 * EEG_serial_component/ and EEG_serial_code/ and the Coder demos
 * hardware/parallelPortOutput.py and timing/callOnFlip.py do. PsychoPy is
 * GPL-3: their descriptions were read (docs/rig_spec.md 15), no code was
 * copied.
 *
 * Each trial: a fixation cross for 0.5 s, then a white disc 0.2 s, 200 px
 * left or right of it. Press the Left or Right arrow for the side, within
 * 1.5 s of the disc's onset; presses before 0.1 s are anticipations. 0.6 s
 * after the response (or the deadline) the trial ends. The codes:
 *   stimulus  11 left, 12 right   at the disc's flip (a trigger channel)
 *   response  21 left, 22 right   at the key event, from the frame loop
 *   end       99                  at the flip of the trial's last frame
 * An output with fewer lines (ybox_code_max() below 99, as DTR and RTS)
 * gets 1, 2 and 3; the row has the side and the key either way.
 *
 * The output is the rig profile's role "trig" (ysp/rigfile.h,
 * docs/device.md: rigfile_profile --bind trig FAMILY KEY) when the profile
 * has one, else an in-process TriggerBox that keeps its bytes (no
 * hardware). --sim always uses the in-process box unless --rig names a
 * profile. Each code is a pulse of --pulse s (default 0.005), or the
 * width the rig fixes.
 *
 * Why the paths differ: the stimulus and end codes go to the screen's
 * trigger worker, which writes them at the planned vblank (AT THE FLIP in
 * ysp/device.h); a key event is only known when the frame loop polls it,
 * so its code goes out from there, up to a frame after the key's stamp.
 * The row says how late each write was, so an analysis can shift the
 * epochs: the code's own time is the write's start, which over USB is
 * where the code reaches the device (docs/serial.md, Pulses), plus the
 * output's write-to-edge latency from the rig profile's loopback.
 *
 * The data file has one row per trial (ysp/trials.h's data writer): the
 * session's columns (index, ..., side, key_answer), then trial_index,
 * correct, stim_code, stim_write_s (write start minus the disc's flip
 * onset), stim_late_s (write start minus the trigger's deadline, the
 * worker's wake), stim_dur_s (the write's length), resp_code, resp_write_s
 * (write start minus the key's stamp), resp_dur_s, end_code, end_write_s
 * (minus the last flip's onset), end_dur_s, writes_failed, then the
 * response's (YRSP_DATA_COLS).
 * Times in seconds; empty where no write was made. FILE.meta has the
 * screen, the output, the codes and the rig profile's hash.
 *
 * --sim: the simulated display and a scripted participant (RT 0.35 to
 * 0.45 s, a wrong key on trial 2, no response on trial 4), on ysp/rt.h's
 * virtual clock, so the output is the same on every run and host. The
 * in-process box runs in manual mode (ydev_poll() each frame ends the
 * pulses; no device thread). The screen's trigger worker is a real thread
 * that the virtual clock does not pace (measured: it wrote 117 to 500 ms
 * after the deadline, a different amount each run), so under --sim a
 * virtual timer makes its call, ydev_out_trigger() at the deadline;
 * device_trigger_flip runs the worker itself, on the real clock. It
 * checks each row (key and RT exact, onsets from flip records), that each
 * flip code's deadline is its flip's onset and its write starts there,
 * that the response code's write starts on the frame that took the key
 * (7 to 9 ns after the key's stamp, off the frame grid by rounding), and
 * that the in-process box got exactly the bytes the script implies.
 * Exits 1 on a mismatch. Under a second.
 *
 * Usage: trial_eeg_triggers [--sim] [--fullscreen] [--reps N] [--seed N]
 *          [--pulse S] [--rig NAME | --no-rig] [--out FILE]
 *   --reps N   repetitions of each side (default 20; --sim: 3)
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
/* after ysp/screen.h: ysp/device.h then gives the trigger channel */
#define YSP_RIGFILE_IMPLEMENTATION
#include "ysp/rigfile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { FIX_ON, DISC_ON, N_CHANNELS };
enum { TRIAL = 1 };
enum { END_OF_TRIAL = 1 };
enum { W_STIM, W_RESP, W_END, N_WRITES };

#define FIX_S       0.5
#define STIM_S      0.2
#define RESPONSE_S  1.5
#define MIN_RT_S    0.1
#define ITI_S       0.6
#define MAX_ROWS    512

static const char conditions_csv[] = "side,key_answer\nleft,left\nright,right\n";
static const yrsp_choice keys[] = { { .key = "left" }, { .key = "right" } };

/* --- the in-process TriggerBox: every byte and its time ------------------ */
typedef struct fakebox { int64_t t[4096]; uint8_t b[4096]; int n; } fakebox;
static fakebox g_box;
static void* fb_open(void* user, const char* key, char* found, size_t cap) {
    (void)key;
    snprintf(found, cap, "in-process box");
    return user;
}
static int fb_write(void* conn, const uint8_t* buf, int n) {
    fakebox* f = (fakebox*)conn;
    /* under the device's output lock: one write at a time */
    if (f->n < 4096 && n > 0) { f->t[f->n] = yrt_now_ns(); f->b[f->n++] = buf[n - 1]; }
    return n;
}
static void fb_close(void* conn) { (void)conn; }
static int64_t virtual_now(void* ctx) { (void)ctx; return yrt_now_ns(); }

/* One write's record (YDEV_REC_OUT): start, end, deadline, code. */
typedef struct wrec { int64_t t0, t1, deadline; uint32_t code; int have, failed; } wrec;

typedef struct row {
    int trial_index, stim_code, resp_code, end_code, writes_failed;
    bool correct;
    int64_t stim_write_ns, stim_late_ns, stim_dur_ns, resp_write_ns, resp_dur_ns, end_write_ns, end_dur_ns;
} row;
static const ytr_col cols[] = {
    YTR_COL(row, trial_index, YTR_COL_INT), YTR_COL(row, correct, YTR_COL_BOOL),
    YTR_COL(row, stim_code, YTR_COL_INT), YTR_COL_D(row, stim_write_ns, YTR_COL_TIME_NS, 6),
    YTR_COL_D(row, stim_late_ns, YTR_COL_TIME_NS, 6), YTR_COL_D(row, stim_dur_ns, YTR_COL_TIME_NS, 6),
    YTR_COL(row, resp_code, YTR_COL_INT), YTR_COL_D(row, resp_write_ns, YTR_COL_TIME_NS, 6),
    YTR_COL_D(row, resp_dur_ns, YTR_COL_TIME_NS, 6), YTR_COL(row, end_code, YTR_COL_INT),
    YTR_COL_D(row, end_write_ns, YTR_COL_TIME_NS, 6), YTR_COL_D(row, end_dur_ns, YTR_COL_TIME_NS, 6),
    YTR_COL(row, writes_failed, YTR_COL_INT), YRSP_DATA_COLS,
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
static ydev_device trig;
static yrt_ring ring;
static unsigned char ring_mem[YRT_RING_BYTES(4096)];
static yrt_event recs[256];
static ygfx_stim fix, disc;
static const ygfx_bind binds[] = {
    { .stim = &fix,  .param = YGFX_P_VISIBLE, .channel = FIX_ON  },
    { .stim = &disc, .param = YGFX_P_VISIBLE, .channel = DISC_ON },
};

/* --sim: a flip code on a virtual timer at its deadline, the call the
 * screen's trigger worker makes (ydev_trigger_fn()). The worker is a real
 * thread, which the virtual clock does not pace: it wrote 117 to 500 ms
 * of virtual time after the deadline, a different amount each run. */
static uint32_t g_vcode[YRT_VIRTUAL_TIMERS];
static int g_vnext;
static int64_t code_timer(void* ctx, int64_t now) {
    (void)ydev_out_trigger(&trig, *(const uint32_t*)ctx, now);
    return 0;   /* once */
}
static int flip_code(int sim, uint32_t code, int64_t deadline) {
    if (!sim) return yscr_trigger(&scr, 0, code);
    g_vcode[g_vnext] = code;
    if (yrt_virtual_timer(code_timer, &g_vcode[g_vnext], deadline) < 0) return -1;
    g_vnext = (g_vnext + 1) % YRT_VIRTUAL_TIMERS;
    return 0;
}

static int fail(const char* why) {
    fprintf(stderr, "trial_eeg_triggers: %s\n", why);
    return 1;
}

/* --sim: who presses what. which: 1 the correct key, 2 the other, 0 none. */
static int sim_which(int trial) { return trial == 2 ? 2 : trial == 4 ? 0 : 1; }
static double sim_rt(int trial) { return 0.35 + 0.05 * (trial % 3); }

int main(int argc, char** argv) {
    static wrec sim_w[MAX_ROWS][N_WRITES];
    static yrsp_result sim_res[MAX_ROWS];
    static int sim_side[MAX_ROWS], sim_key[MAX_ROWS];
    static int64_t sim_stim_on[MAX_ROWS], sim_end_on[MAX_ROWS], sim_fed[MAX_ROWS];
    static int64_t lag[3][MAX_ROWS];
    yscr_desc sd = { 0 };
    yscr_trigger_desc ch;
    ydev_desc od = { 0 };
    yscr_caps caps;
    yscr_frame f;
    yrsp_source source;
    ytl_event fired[64];
    ytr_trial_info ti = { 0 };
    yrsp_result res = { 0 };
    yrsp_input sim_ev[2];
    wrec w[N_WRITES];
    row r = { 0 };
    char line[1024], meta[3072], meta_path[1100], err[256], hash[65] = "none";
    const char *out_path = "trial_eeg_triggers.csv", *rig_name = NULL, *out_note = "in-process TriggerBox";
    uint64_t seed = 20261013;
    uint32_t code[N_WRITES][2];
    double pulse_s = 0.005;
    int i, n, sim = 0, reps = 0, fullscreen = 0, no_rig = 0, aborted = 0, failed = 0, rows = 0, sim_bad = 0;
    int in_trial = 0, responding = 0, ending = 0, side = 0, side_col, n_sim_ev = 0, sim_next = 0, family, use_box = 1;
    int64_t sim_frame = -1, stim_frame = -1, end_frame = -1, end_onset = 0, end_since = 0;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--sim")) sim = 1;
        else if (!strcmp(argv[i], "--fullscreen")) fullscreen = 1;
        else if (!strcmp(argv[i], "--reps") && i + 1 < argc) reps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) seed = (uint64_t)strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--pulse") && i + 1 < argc) pulse_s = atof(argv[++i]);
        else if (!strcmp(argv[i], "--rig") && i + 1 < argc) rig_name = argv[++i];
        else if (!strcmp(argv[i], "--no-rig")) no_rig = 1;
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out_path = argv[++i];
        else {
            fprintf(stderr, "usage: trial_eeg_triggers [--sim] [--fullscreen] [--reps N] [--seed N] [--pulse S] "
                            "[--rig NAME | --no-rig] [--out FILE]\n");
            return 2;
        }
    }
    /* --sim runs on ysp/rt.h's virtual clock: the same records on any host */
    if (sim) yrt_virtual_begin(0);
    if (reps == 0) reps = sim ? 3 : 20;
    if (reps < 1 || reps > MAX_ROWS / 2 || !(pulse_s > 0.0 && pulse_s <= 0.1) || (rig_name && no_rig)) {
        fprintf(stderr, "trial_eeg_triggers: --reps takes 1..256, --pulse 0 to 0.1 s; --rig or --no-rig\n");
        return 2;
    }

    if (!yrt_ring_open(&ring, &(yrt_ring_desc){ .memory = ring_mem, .bytes = sizeof ring_mem })) return fail("ring");

    /* the output: the rig's "trig" role, or the in-process box */
    od.role = "trig";
    od.ring = &ring;
    od.pulse_ns = YRT_S(pulse_s);
    if (!no_rig && (!sim || rig_name)) {
        int rc = yrig_load(&prof, rig_name, err, sizeof err);
        const yrig_role* role = rc == YRIG_OK ? yrig_find(&prof, "trig") : NULL;
        if (rc != YRIG_OK && (rc != YRIG_ERR_MISSING || rig_name)) return fail(err);
        if (role) {
            for (i = 0; i < prof.n_notes; i++) printf("rig profile: %s\n", prof.notes[i]);
            if (ybox_fixed_pulse_ns(role->family_id) > 0 && !role->latched) od.pulse_ns = 0;   /* the box's own width */
            od.device = 1;
            if (!yrig_open_device(&trig, &prof, "trig", &od)) return fail(ydev_error(&trig));
            yrig_hash(&prof, hash);
            use_box = 0;
            out_note = "the rig profile's role trig";
        } else printf("trial_eeg_triggers: %s; the in-process box\n",
                      rc == YRIG_OK ? "the rig profile has no role trig" : "no rig profile");
    }
    if (use_box) {
        od.family = YBOX_TRIGGERBOX;
        od.key = "sim:box";
        od.device = 1;
        od.transport.open = fb_open;
        od.transport.write = fb_write;
        od.transport.close = fb_close;
        od.transport.user = &g_box;
        if (sim) {
            /* no reader thread and no trailing-edge worker: ydev_poll() in the
             * frame loop ends each pulse on the virtual clock */
            od.manual = true;
            od.now = virtual_now;
        }
        if (!ydev_open(&trig, &od)) return fail(ydev_error(&trig));
    }
    if (ydev_wait_running(&trig, YRT_S(3)) < 0) { fail(ydev_error(&trig)); ydev_close(&trig); return 1; }
    family = trig.d.family;
    for (i = 0; i < 2; i++) {   /* [write][side or key] */
        int wide = ybox_code_max(family) >= 99;
        code[W_STIM][i] = wide ? 11u + (uint32_t)i : 1u;
        code[W_RESP][i] = wide ? 21u + (uint32_t)i : 2u;
        code[W_END][i] = wide ? 99u : 3u;
    }

    /* the display, with the output as its trigger channel 0 */
    ch = ydev_trigger_channel(&trig, 0);
    sd.backend = sim ? YSCR_BACKEND_SIM : 0;
    sd.windowed = !fullscreen;
    if (!sim) {
        sd.triggers = &ch;
        sd.n_triggers = 1;
    }
    if (!ygfx_open_screen(&gfx, &scr, &sd, &(ygfx_desc){ .background = { 0.5f, 0.5f, 0.5f } })) {
        fail(ygfx_error(&gfx));
        ydev_close(&trig);
        return 1;
    }
    yscr_get_caps(&scr, &caps);
    fix = ygfx_shape(&(ygfx_shape_desc){ .shape = YGFX_CROSS, .w = 16, .h = 16, .shape_p = { 3 },
                                         .edge = YGFX_EDGE_COSINE, .edge_width = 1, .color = { 0.1f, 0.1f, 0.1f } });
    disc = ygfx_shape(&(ygfx_shape_desc){ .shape = YGFX_CIRCLE, .w = 80, .h = 80, .edge = YGFX_EDGE_COSINE,
                                          .edge_width = 1, .color = { 1, 1, 1 } });

    if (!ytb_open_csv(&tab, &(ytb_csv_desc){ .text = conditions_csv, .len = sizeof conditions_csv - 1,
                                             .arena = table_arena, .arena_size = sizeof table_arena }))
        return fail(ytb_error(&tab));
    side_col = ytb_col(&tab, "side");
    if (!ytr_open(&trials, &(ytr_desc){ .table = &tab, .reps = reps, .order = YTR_ORDER_RANDOM, .rng = ytr_splitmix,
                                        .rng_ctx = &seed }))
        return fail(ytr_error(&trials));
    if (!ytl_open(&tl, &(ytl_desc){ .events = storage, .event_capacity = 64, .n_channels = N_CHANNELS }))
        return fail(ytl_error(&tl));
    source = yrsp_sdl_source(&(yrsp_sdl_ctx){ .raw_keyboard = caps.raw_keyboard }, YRSP_KIND_KEYBOARD);
    if (!yrsp_open(&rsp, &(yrsp_desc){ .choices = keys, .n_choices = 2, .duration_ns = YRT_S(RESPONSE_S),
                                       .min_rt_ns = YRT_S(MIN_RT_S), .sources = &source, .n_sources = 1 }))
        return fail(yrsp_error(&rsp));

    yscr_describe(&scr, line, sizeof line);
    printf("%s\noutput: %s, %s at %s, codes %u/%u %u/%u %u, pulse %.4f s\n", line, out_note, ybox_family_name(family),
           trig.d.key, code[W_STIM][0], code[W_STIM][1], code[W_RESP][0], code[W_RESP][1], code[W_END][0],
           YRT_TO_S(trig.d.pulse_ns));
    snprintf(meta, sizeof meta, "screen=%s\noutput=%s; family %s; key %s; pulse_s %.6f (0: a set, or the box's own "
             "width)\ncodes=stimulus %u left %u right; response %u left %u right; end %u\nrig_profile_sha256=%s\n"
             "ysp_device=%s\nysp_response=%s\nkeyboard=%s\n", line, out_note, ybox_family_name(family), trig.d.key,
             YRT_TO_S(trig.d.pulse_ns), code[W_STIM][0], code[W_STIM][1], code[W_RESP][0], code[W_RESP][1],
             code[W_END][0], hash, ydev_version(), yrsp_version(), sim ? "synthetic (tier SIM)" : source.note);
    snprintf(meta_path, sizeof meta_path, "%s.meta", out_path);
    if (!ytr_data_open(&data, &(ytr_data_desc){ .path = out_path, .trials = &trials, .cols = cols,
            .n_cols = (int)(sizeof cols / sizeof cols[0]), .meta_path = meta_path, .meta = meta }))
        return fail(ytr_data_error(&data));

    while (!aborted) {
        int rc = yscr_begin(&scr, &f);
        if (trig.d.manual) (void)ydev_poll(&trig, 0);   /* trailing edges that are due */
        if (rc == YSCR_QUIT) { aborted = 1; break; }
        if (rc != YSCR_OK) { failed = fail(yscr_error(&scr)); break; }

        /* the output's records: each leading write of this trial's codes */
        n = yrt_ring_drain(&ring, recs, 256);
        for (i = 0; i < n; i++) {
            const yrt_event* e = &recs[i];
            int k, flip;
            if (e->source != YRT_SRC_DEVICE || e->kind != YDEV_REC_OUT || (e->u.u16[18] & YDEV_OUT_TRAILING)) continue;
            flip = (e->u.u16[18] & YDEV_OUT_FLIP) != 0;
            k = !flip ? W_RESP : ending ? W_END : W_STIM;
            if (!in_trial || w[k].have) continue;
            w[k].t0 = e->u.i64[0];
            w[k].t1 = e->u.i64[1];
            w[k].deadline = flip ? e->u.i64[3] : 0;
            w[k].code = e->u.u32[8];
            w[k].failed = (e->u.u16[18] & YDEV_OUT_FAILED) != 0;
            w[k].have = 1;
        }
        for (i = 0; i < f.n_done; i++)
            if (f.done[i].index == end_frame) end_onset = yrsp_onset_flip(&f.done[i]).t_ns;

        /* the trial is over once its end code's record and its last flip are in */
        if (ending && ((w[W_END].have && end_onset) || f.onset - end_since > YRT_S(0.5))) {
            const int64_t miss = YTR_MISSING_NS;
            r.trial_index = ti.index;
            r.correct = (res.flags & YRSP_R_RESPONDED) && res.response == side;
            r.stim_code = w[W_STIM].have ? (int)w[W_STIM].code : YTR_MISSING_INT;
            r.stim_write_ns = w[W_STIM].have && res.t_onset ? w[W_STIM].t0 - res.t_onset : miss;
            r.stim_late_ns = w[W_STIM].have ? w[W_STIM].t0 - w[W_STIM].deadline : miss;
            r.stim_dur_ns = w[W_STIM].have ? w[W_STIM].t1 - w[W_STIM].t0 : miss;
            r.resp_code = w[W_RESP].have ? (int)w[W_RESP].code : YTR_MISSING_INT;
            r.resp_write_ns = w[W_RESP].have ? w[W_RESP].t0 - res.t_response : miss;
            r.resp_dur_ns = w[W_RESP].have ? w[W_RESP].t1 - w[W_RESP].t0 : miss;
            r.end_code = w[W_END].have ? (int)w[W_END].code : YTR_MISSING_INT;
            r.end_write_ns = w[W_END].have && end_onset ? w[W_END].t0 - end_onset : miss;
            r.end_dur_ns = w[W_END].have ? w[W_END].t1 - w[W_END].t0 : miss;
            r.writes_failed = w[W_STIM].failed + w[W_RESP].failed + w[W_END].failed;
            ytr_update(&trials, (res.flags & YRSP_R_RESPONDED) ? r.correct : YTR_INVALID, NULL);
            if (yrsp_data_set(&data, &res) < 0 || ytr_data_write(&data, &r) < 0) failed = fail(ytr_data_error(&data));
            printf("trial %2d %-5s stim %3d +%.3f ms", ti.index, side ? "right" : "left", r.stim_code,
                   w[W_STIM].have ? YRT_TO_S(r.stim_write_ns) * 1e3 : -1.0);
            if (w[W_RESP].have) printf("  resp %3d +%.3f ms (rt %.3f s)", r.resp_code, YRT_TO_S(r.resp_write_ns) * 1e3,
                                       YRT_TO_S(res.rt_ns));
            else printf("  no response               ");
            printf("  end %3d +%.3f ms\n", r.end_code, w[W_END].have ? YRT_TO_S(r.end_write_ns) * 1e3 : -1.0);
            if (ti.index < MAX_ROWS) {
                memcpy(sim_w[ti.index], w, sizeof w);
                sim_res[ti.index] = res;
                sim_side[ti.index] = side;
                sim_end_on[ti.index] = end_onset;
            }
            rows++;
            in_trial = ending = 0;
        }

        if (!in_trial) {
            /* a new trial: fixation now, the disc 0.5 s later for 0.2 s */
            ytl_seq q;
            if (ytr_next(&trials, &ti) < 0 || ti.index >= MAX_ROWS) break;
            side = !strcmp(ytb_text(&tab, ti.condition, side_col), "right");
            disc.x = side ? 200.0f : -200.0f;
            ytl_clear(&tl, TRIAL);
            ytl_anchor(&tl, TRIAL, f.onset, 0);
            q = ytl_seq_on(&tl, TRIAL);
            ytl_on(&q, FIX_ON);
            ytl_wait(&q, YRT_S(FIX_S));
            ytl_on(&q, DISC_ON);
            ytl_wait(&q, YRT_S(STIM_S));
            ytl_off(&q, DISC_ON);
            if (q.err) { failed = fail(ytl_strerror(q.err)); break; }
            yrsp_arm(&rsp, f.onset);
            memset(w, 0, sizeof w);
            stim_frame = end_frame = -1;
            end_onset = 0;
            in_trial = responding = 1;
        }

        /* --sim: the key from the disc's flip record, fed once the clock
         * has reached it */
        for (i = 0; sim && sim_frame >= 0 && i < f.n_done; i++)
            if (f.done[i].index == sim_frame && yrsp_onset_flip(&f.done[i]).final) {
                sim_stim_on[ti.index] = f.done[i].onset;
                sim_ev[0].t = f.done[i].onset + YRT_S(sim_rt(ti.index));
                sim_ev[1].t = sim_ev[0].t + YRT_MS(80);
                n_sim_ev = sim_key[ti.index] >= 0 ? 2 : 0;
                sim_next = 0;
                sim_frame = -1;
            }
        yrsp_feed_screen(&scr, &rsp, 1);
        for (; sim && sim_next < n_sim_ev && sim_ev[sim_next].t <= yrt_now_ns(); sim_next++) {
            if (sim_next == 0) sim_fed[ti.index] = yrt_now_ns();
            yrsp_feed(&rsp, &sim_ev[sim_next]);
        }

        /* the response or the deadline: the response's code now, the end 0.6 s on */
        if (yrsp_update_frame(&rsp, &f) == YRSP_ENDED && responding) {
            ytl_seq q = ytl_seq_on(&tl, TRIAL);
            int64_t bt = 0, stim_end = YRT_S(FIX_S + STIM_S);
            yrsp_finish(&rsp, &res);   /* the key now; the final result at the end */
            if ((res.flags & YRSP_R_RESPONDED) && res.response >= 0)
                (void)ydev_out_mark(&trig, code[W_RESP][res.response], NULL);   /* its record has the result */
            ytl_base_time(&tl, TRIAL, f.onset, &bt);
            ytl_at(&q, bt > stim_end ? bt : stim_end);
            ytl_wait(&q, YRT_S(ITI_S));
            ytl_off(&q, FIX_ON);
            ytl_mark(&q, END_OF_TRIAL, 0);
            if (q.err) { failed = fail(ytl_strerror(q.err)); break; }
            responding = 0;
        }

        n = ytl_evaluate(&tl, &(ytl_frame){ f.onset, f.period, f.index }, fired, 64);
        for (i = 0; i < n; i++) {
            if (fired[i].kind == YTL_ONSET && fired[i].target == DISC_ON) {
                yrsp_set_landing(&rsp, &fired[i], 1, DISC_ON);
                if (flip_code(sim, code[W_STIM][side], f.onset) < 0) failed = fail("the stimulus code");
                stim_frame = fired[i].frame;
                if (sim && ti.index < MAX_ROWS) {
                    int which = sim_which(ti.index);
                    sim_key[ti.index] = which == 0 ? -1 : which == 1 ? side : 1 - side;
                    for (; sim_next < n_sim_ev; sim_next++) yrsp_feed(&rsp, &sim_ev[sim_next]);   /* a late release */
                    memset(sim_ev, 0, sizeof sim_ev);
                    if (which) {
                        sim_ev[0].kind = YRSP_KIND_KEYBOARD;
                        sim_ev[0].type = YRSP_PRESS;
                        sim_ev[0].control = (uint32_t)yrsp_scancode(sim_key[ti.index] ? "right" : "left");
                        sim_ev[0].stamp = YRSP_TIER_SIM;
                        sim_ev[0].value = 1;
                        sim_ev[1] = sim_ev[0];
                        sim_ev[1].type = YRSP_RELEASE;
                        sim_ev[1].value = 0;
                    }
                    n_sim_ev = sim_next = 0;
                    sim_frame = stim_frame;
                }
            }
            if (fired[i].kind == YTL_MARK && fired[i].code == END_OF_TRIAL) {
                if (flip_code(sim, code[W_END][0], f.onset) < 0) failed = fail("the end code");
                yrsp_finish(&rsp, &res);   /* with the release and the final onset */
                end_frame = fired[i].frame;
                end_since = f.onset;
                ending = 1;
            }
        }
        ygfx_apply(binds, (int)(sizeof binds / sizeof binds[0]), ytl_values(&tl));
        yscr_mark(&scr, YSCR_PHASE_EVALUATE);
        ygfx_begin(&gfx, &f);
        ygfx_draw(&gfx, &fix);
        ygfx_draw(&gfx, &disc);
        ygfx_end(&gfx);
        yscr_flip(&scr);
    }
    if (ytr_data_finish(&data) < 0) failed = fail(ytr_data_error(&data));
    ygfx_close(&gfx);
    yscr_close(&scr);
    ydev_close(&trig);   /* writes a pending trailing edge, then the box's close byte */
    printf("%d trials written to %s%s\n", rows, out_path, aborted ? " (stopped with Shift+Esc)" : "");

    if (sim && !failed && !aborted && use_box) {
        /* the bytes the box must have: each code, then its pulse's 0 */
        uint8_t want[MAX_ROWS * 6 + 2];
        int nw = 0, nl[3] = { 0, 0, 0 };
        want[nw++] = 0;   /* the 0 a TriggerBox gets when it opens */
        for (i = 0; i < rows; i++) {
            const yrsp_result* q = &sim_res[i];
            const wrec* x = sim_w[i];
            int resp_want = sim_key[i] >= 0, bad = 0;
            if (resp_want != ((q->flags & YRSP_R_RESPONDED) != 0) || (resp_want && q->response != sim_key[i]) ||
                (resp_want && q->rt_ns != YRT_S(sim_rt(i))) || q->onset_tier != YSCR_TIER_SIM ||
                q->onset_src != YRSP_ONSET_FLIP || q->t_onset != sim_stim_on[i])
                bad = 1;
            /* the flip codes: deadline at the flip's onset, written at it (the
             * virtual clock stands still during a write) */
            if (!x[W_STIM].have || x[W_STIM].failed || x[W_STIM].code != code[W_STIM][sim_side[i]] ||
                x[W_STIM].deadline != q->t_onset || x[W_STIM].t0 != x[W_STIM].deadline || x[W_STIM].t1 != x[W_STIM].t0)
                bad = 1;
            if (!x[W_END].have || x[W_END].failed || x[W_END].code != code[W_END][0] || x[W_END].deadline != sim_end_on[i] ||
                x[W_END].t0 != x[W_END].deadline || x[W_END].t1 != x[W_END].t0)
                bad = 1;
            /* the response code: from the loop, on the frame that took the key */
            if (resp_want != x[W_RESP].have || (resp_want && (x[W_RESP].failed || x[W_RESP].code != code[W_RESP][sim_key[i]] ||
                x[W_RESP].t0 != sim_fed[i] || x[W_RESP].t0 < q->t_response)))
                bad = 1;
            if (bad) {
                fprintf(stderr, "trial_eeg_triggers: --sim: trial %d: response %d (want %d) rt %.9f; stim %d code %u "
                                "deadline - onset %lld ns, write - deadline %lld ns; resp %d code %u write - key %lld ns; "
                                "end %d code %u deadline - onset %lld ns, write - deadline %lld ns\n",
                        i, q->response, sim_key[i], YRT_TO_S(q->rt_ns), x[W_STIM].have, x[W_STIM].code,
                        (long long)(x[W_STIM].deadline - q->t_onset), (long long)(x[W_STIM].t0 - x[W_STIM].deadline),
                        x[W_RESP].have, x[W_RESP].code, (long long)(x[W_RESP].t0 - q->t_response), x[W_END].have,
                        x[W_END].code, (long long)(x[W_END].deadline - sim_end_on[i]),
                        (long long)(x[W_END].t0 - x[W_END].deadline));
                sim_bad = 1;
            }
            lag[0][nl[0]++] = x[W_STIM].t0 - x[W_STIM].deadline;
            if (x[W_RESP].have) lag[1][nl[1]++] = x[W_RESP].t0 - q->t_response;
            lag[2][nl[2]++] = x[W_END].t0 - x[W_END].deadline;
            want[nw++] = (uint8_t)code[W_STIM][sim_side[i]];
            want[nw++] = 0;
            if (resp_want) { want[nw++] = (uint8_t)code[W_RESP][sim_key[i]]; want[nw++] = 0; }
            want[nw++] = (uint8_t)code[W_END][0];
            want[nw++] = 0;
        }
        want[nw++] = 0xFF;   /* the TriggerBox's close byte */
        if (g_box.n != nw || memcmp(g_box.b, want, (size_t)nw)) {
            fprintf(stderr, "trial_eeg_triggers: --sim: the box got %d bytes, the script implies %d\n", g_box.n, nw);
            sim_bad = 1;
        }
        if (rows != 2 * reps) { fprintf(stderr, "trial_eeg_triggers: --sim: %d rows, not %d\n", rows, 2 * reps); sim_bad = 1; }
        for (i = 0; i < 3; i++) {
            int64_t lo = INT64_MAX, hi = INT64_MIN;
            int k;
            for (k = 0; k < nl[i]; k++) { if (lag[i][k] < lo) lo = lag[i][k]; if (lag[i][k] > hi) hi = lag[i][k]; }
            if (nl[i]) printf("--sim: %-8s write start minus %-8s %lld to %lld ns (%d writes)\n",
                              i == 0 ? "stimulus" : i == 1 ? "response" : "end", i == 1 ? "key" : "deadline",
                              (long long)lo, (long long)hi, nl[i]);
        }
        if (!sim_bad) printf("--sim: every trial and every byte came out as scripted\n");
        failed = sim_bad;
    }
    return failed;
}
