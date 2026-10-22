/* response_lag.c - the lag of a response trigger: a response box's press
 * to the start of the trigger code's write, through the frame loop and
 * through a ysp/device.h response rule on the box's reader thread
 * (docs/experiment_format.md, gap G8; ysp/device.h, RESPONSE RULES).
 *
 *   response_lag [--n N] [--pace sim|window] [--no-rule] [--csv FILE]
 *       A 320 x 200 window, for ysp/screen.h's input bridge. An in-process
 *       response box (the ysp line protocol on a fake transport, read by a
 *       ysp/device.h reader thread) presses button 1 every 80 to 180 ms,
 *       N times (default 600). Each press takes two paths to two
 *       in-process TriggerBoxes:
 *         frame loop  the bridge, the frame loop's yrsp_feed_screen() and
 *                     yrsp_update_frame(); at YRSP_ENDED, code 21 with
 *                     ydev_out_pulse(). The path of trial_eeg_triggers.c
 *         rule        the box's desc.rule, armed from the collector
 *                     (yrsp_rule_arm(), yrsp_rule_window()): code 31,
 *                     written on the reader thread
 *       --pace sim (default) runs the frame loop on ysp/screen.h's
 *       simulated 60 Hz display, the rate of a rig; --pace window on the
 *       window's own flips (a window in the background can be throttled).
 *       --no-rule leaves the rule out. It prints, per path, the press's
 *       stamp to the write's start: n, min, p5, p50, p95, p99, max and mean.
 *   response_lag --sim [--n N]
 *       No window, for CI: the simulated display, and the box in manual
 *       mode (ydev_poll() on the frame thread, which is then also the
 *       box's reader). Checks that each press gives one write on each path,
 *       with its code, the RULE flag and the press's t in the record, and
 *       no row with YRSP_R_SENT_MISMATCH.
 *
 * Why a fake box: the lag under test is the host's, from the event's stamp
 * on the reader thread to the write call. A real box adds its own USB
 * delivery before the stamp and the write's own time after it, the same on
 * both paths; docs/serial.md and docs/device.md measured those on a
 * Teensy 4.0.
 *
 * Exit code 0 when every press got its writes (and, with --sim, every
 * check passed); 1 otherwise; 2 for a usage or setup error.
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#define YSP_SCREEN_IMPLEMENTATION
#include "ysp/screen.h"
#define YSP_DEVICE_IMPLEMENTATION
#include "ysp/device.h"
#define YSP_RESPONSE_IMPLEMENTATION
#include "ysp/response.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_N      20000
#define MAX_FRAMES 200000
#define CODE_LOOP  21u
#define CODE_RULE  31u

/* --- the in-process response box: line protocol presses ------------------ */

typedef struct fakebtn {
    int64_t  next;             /* the next line's time                        */
    int      down;             /* the next line is a release                  */
    int      n, n_max;         /* presses sent, and how many to send          */
    uint64_t rng;
    int64_t  t_emit[MAX_N];    /* each press's bytes became readable          */
} fakebtn;
static fakebtn g_btn;

static uint32_t xorshift(uint64_t* s) {
    *s ^= *s << 13; *s ^= *s >> 7; *s ^= *s << 17;
    return (uint32_t)(*s >> 32);
}

static void* btn_open(void* user, const char* key, char* found, size_t cap) {
    fakebtn* b = (fakebtn*)user;
    (void)key;
    snprintf(found, cap, "in-process response box");
    b->next = yrt_now_ns() + 500000000;
    return user;
}
/* One line per call, when it is due: a press, or its release 30 ms later. */
static int btn_read(void* conn, uint8_t* buf, int cap, int64_t timeout_ns) {
    fakebtn* b = (fakebtn*)conn;
    int64_t now = yrt_now_ns(), t;
    if ((b->n >= b->n_max && !b->down) || b->next - now > timeout_ns) {
        if (timeout_ns > 0) (void)yrt_sleep_ns(timeout_ns);
        return 0;
    }
    (void)yrt_sleep_until(b->next, 200000);
    t = yrt_now_ns();
    if (!b->down) {
        b->t_emit[b->n++] = t;
        b->down = 1;
        b->next = t + 30000000;
    } else {
        b->down = 0;
        b->next = t + 50000000 + (int64_t)(xorshift(&b->rng) % 100000u) * 1000;   /* 80 to 180 ms apart */
    }
    return snprintf((char*)buf, (size_t)cap, "E %lld 1 %d\n", (long long)(t / 1000), b->down);
}
static int btn_write(void* conn, const uint8_t* buf, int n) { (void)conn; (void)buf; return n; }
static void btn_close(void* conn) { (void)conn; }

/* --- the in-process TriggerBoxes -------------------------------------------- */

static void* box_open(void* user, const char* key, char* found, size_t cap) {
    (void)key;
    snprintf(found, cap, "in-process trigger box");
    return user;
}
static int box_read(void* conn, uint8_t* buf, int cap, int64_t timeout_ns) {
    (void)conn; (void)buf; (void)cap;
    (void)yrt_sleep_ns(timeout_ns);
    return 0;
}
static int box_write(void* conn, const uint8_t* buf, int n) { (void)conn; (void)buf; return n; }
static void box_close(void* conn) { (void)conn; }

static ydev_transport fake(void* (*op)(void*, const char*, char*, size_t), int (*rd)(void*, uint8_t*, int, int64_t),
                           int (*wr)(void*, const uint8_t*, int), void (*cl)(void*), void* user) {
    ydev_transport t;
    memset(&t, 0, sizeof t);
    t.open = op; t.read = rd; t.write = wr; t.close = cl; t.user = user;
    return t;
}

static yrsp_collector g_rsp;

static void to_bridge(void* ctx, const yin_event* e) {
    (void)ctx;
    (void)yscr_push_input(e);
}
/* --sim: the box is polled on the frame thread, so its events go straight
 * to the collector */
static void to_collector(void* ctx, const yin_event* e) {
    (void)ctx;
    (void)yrsp_feed(&g_rsp, e);
}

static int cmp_i64(const void* a, const void* b) {
    int64_t x = *(const int64_t*)a, y = *(const int64_t*)b;
    return x < y ? -1 : (x > y);
}

static void summary(const char* what, int64_t* v, int n) {
    double mean = 0.0;
    int i;
    if (n <= 0) { printf("%-34s none\n", what); return; }
    qsort(v, (size_t)n, sizeof v[0], cmp_i64);
    for (i = 0; i < n; i++) mean += (double)v[i];
    mean /= n;
    printf("%-34s n %d  min %.1f  p5 %.1f  p50 %.1f  p95 %.1f  p99 %.1f  max %.1f  mean %.1f us\n", what, n,
           (double)v[0] / 1e3, (double)v[(n * 5) / 100] / 1e3, (double)v[n / 2] / 1e3, (double)v[(n * 95) / 100] / 1e3,
           (double)v[(n * 99) / 100] / 1e3, (double)v[n - 1] / 1e3, mean / 1e3);
}

static ydev_device    g_in, g_out, g_rout;
static ydev_rule      g_rule;
static yscr_screen    g_scr, g_sim;
static int64_t        g_loop[MAX_N], g_rlag[MAX_N], g_stamp[MAX_N], g_t[MAX_N], g_begin[MAX_N];
static int64_t        g_iter[MAX_FRAMES];
static unsigned char  g_ring_mem[YRT_RING_BYTES(8192)];
static yrt_event      g_rec[8192];

static int open_box(ydev_device* d, const char* role, uint32_t device, yrt_ring* ring) {
    ydev_desc od;
    memset(&od, 0, sizeof od);
    od.role = role;
    od.family = YBOX_TRIGGERBOX;
    od.key = "sim:box";
    od.device = device;
    od.ring = ring;
    od.pulse_ns = YRT_MS(5);
    od.transport = fake(box_open, box_read, box_write, box_close, d);   /* open returns it: non-NULL */
    if (!ydev_open(d, &od) || ydev_wait_running(d, YRT_S(2)) < 0) {
        fprintf(stderr, "%s: %s\n", role, ydev_error(d));
        return 0;
    }
    return 1;
}

int main(int argc, char** argv) {
    const char* csv = NULL;
    int n = 600, i, got = 0, rgot = 0, rc, frames, pace_window = 0, sim = 0, use_rule = 1, bad = 0, mismatch = 0;
    yscr_screen* pace = &g_sim;
    ydev_desc id;
    yscr_desc sd;
    yscr_frame f;
    yrsp_desc rd;
    yrsp_onset on;
    yrt_ring ring;
    yrt_ring_desc ringd;
    int64_t end;
    char line[512];

    for (i = 1; i < argc; i++) {
        const char* v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(argv[i], "--n") && v) { n = atoi(v); i++; }
        else if (!strcmp(argv[i], "--csv") && v) { csv = v; i++; }
        else if (!strcmp(argv[i], "--sim")) sim = 1;
        else if (!strcmp(argv[i], "--no-rule")) use_rule = 0;
        else if (!strcmp(argv[i], "--pace") && v && (!strcmp(v, "sim") || !strcmp(v, "window"))) {
            pace_window = !strcmp(v, "window");
            i++;
        } else {
            fprintf(stderr, "usage: response_lag [--n N] [--pace sim|window] [--no-rule] [--csv FILE] | --sim [--n N]\n");
            return 2;
        }
    }
    if (n < 1 || n > MAX_N) { fprintf(stderr, "--n: 1 to %d\n", MAX_N); return 2; }
    if (sim && pace_window) { fprintf(stderr, "--sim has no window\n"); return 2; }
    if (sim && n == 600) n = 20;

    memset(&ringd, 0, sizeof ringd);
    ringd.memory = g_ring_mem;
    ringd.bytes = sizeof g_ring_mem;
    memset(&ring, 0, sizeof ring);
    if (!yrt_ring_open(&ring, &ringd)) return 2;

    if (!sim) {
        memset(&sd, 0, sizeof sd);
        sd.windowed = true;
        sd.window_w = 320;
        sd.window_h = 200;
        /* the window first: the bridge exists once a screen with SDL is open */
        if (!yscr_open(&g_scr, &sd)) { fprintf(stderr, "screen: %s\n", yscr_error(&g_scr)); return 2; }
        yscr_describe(&g_scr, line, sizeof line);
        printf("%s\n", line);
    }
    if (pace_window) {
        pace = &g_scr;
    } else {
        /* the frame loop on a 60 Hz grid; the window stays for the bridge */
        yscr_desc pd;
        memset(&pd, 0, sizeof pd);
        pd.backend = YSCR_BACKEND_SIM;
        if (!yscr_open(&g_sim, &pd)) { fprintf(stderr, "screen: %s\n", yscr_error(&g_sim)); return 2; }
        yscr_describe(&g_sim, line, sizeof line);
        printf("%s\n", line);
    }

    if (!open_box(&g_out, "trig", 2, &ring) || !open_box(&g_rout, "rtrig", 3, &ring)) return 2;

    /* the collector: any box press, each press a trial */
    memset(&rd, 0, sizeof rd);
    rd.kinds = YRSP_KIND_BIT(YRSP_KIND_BOX);
    if (!yrsp_open(&g_rsp, &rd)) { fprintf(stderr, "%s\n", yrsp_error(&g_rsp)); return 2; }
    if (use_rule) {
        ydev_rule_desc rl;
        ydev_rule_choice ch;
        memset(&ch, 0, sizeof ch);
        ch.kind = YIN_KIND_BOX;
        ch.code = CODE_RULE;
        memset(&rl, 0, sizeof rl);
        rl.out = &g_rout;
        rl.choices = &ch;
        rl.n_choices = 1;
        if (!ydev_rule_open(&g_rule, &rl)) { fprintf(stderr, "%s\n", ydev_rule_error(&g_rule)); return 2; }
    }

    g_btn.n_max = n;
    g_btn.rng = 0x9E3779B97F4A7C15ull;
    memset(&id, 0, sizeof id);
    id.role = "resp";
    id.family = YBOX_LINE;
    id.key = "sim:resp";
    id.device = 1;
    id.no_identify = true;
    id.probe_ns = -1;
    id.silent_ns = -1;
    id.manual = sim != 0;
    id.sink = sim ? to_collector : to_bridge;
    id.rule = use_rule ? &g_rule : NULL;
    id.transport = fake(btn_open, btn_read, btn_write, btn_close, &g_btn);
    if (!ydev_open(&g_in, &id) || ydev_wait_running(&g_in, YRT_S(2)) < 0) {
        fprintf(stderr, "input: %s\n", ydev_error(&g_in));
        return 2;
    }
    (void)yrt_thread_elevate(NULL);

    memset(&on, 0, sizeof on);
    on.final = 1;
    on.frame = -1;
    end = yrt_now_ns() + (int64_t)n * 200000000 + YRT_S(3);
    for (i = 0; got < n && yrt_now_ns() < end; i++) {
        int64_t tb;
        if (yscr_begin(pace, &f) != YSCR_OK) break;
        tb = yrt_now_ns();
        if (i < MAX_FRAMES) g_iter[i] = tb;
        if (i == 0) {
            yrsp_arm(&g_rsp, f.onset);
            on.t_ns = f.onset;
            (void)yrsp_set_onset(&g_rsp, &on);
            if (use_rule) yrsp_rule_arm(&g_rule, &g_rsp);
        }
        if (sim) (void)ydev_poll(&g_in, 0);
        else yrsp_feed_screen(&g_scr, &g_rsp, 1);
        if (yrsp_update_frame(&g_rsp, &f) == YRSP_ENDED) {
            yrsp_result res;
            ydev_out_info oi;
            rc = ydev_out_pulse(&g_out, CODE_LOOP, YRT_MS(5));
            ydev_out_last(&g_out, &oi);
            if (use_rule) (void)yrsp_rule_sent(&g_rule, &g_rsp);   /* CODES SENT: flag a disagreement */
            (void)yrsp_finish(&g_rsp, &res);
            if (res.flags & YRSP_R_SENT_MISMATCH) mismatch++;
            if (rc == YDEV_OK && res.t_response) {
                g_t[got] = res.t_response;
                g_loop[got] = oi.t_before - res.t_response;
                g_begin[got] = tb - res.t_response;
                got++;
            } else
                fprintf(stderr, "press %d: write %s\n", got, ydev_strerror(rc));
            if (use_rule) {
                /* the rule's write for the same press, made on the reader thread */
                ydev_rule_info ri;
                ydev_rule_get(&g_rule, &ri);
                if (ri.fired == 1 && ri.result == YDEV_OK && ri.t_event == res.t_response)
                    g_rlag[rgot++] = ri.t_before - ri.t_event;
                else
                    fprintf(stderr, "press %d: rule fired %d, result %s\n", got, ri.fired, ydev_strerror(ri.result));
            }
            yrsp_arm(&g_rsp, f.onset);
            on.t_ns = f.onset;
            (void)yrsp_set_onset(&g_rsp, &on);
            if (use_rule) yrsp_rule_arm(&g_rule, &g_rsp);
        }
        if (use_rule) yrsp_rule_window(&g_rule, &g_rsp);
        yscr_flip(pace);
    }
    frames = i;
    ydev_close(&g_in);
    ydev_rule_close(&g_rule);
    ydev_close(&g_out);
    ydev_close(&g_rout);
    if (pace == &g_sim) yscr_close(&g_sim);
    if (!sim) yscr_close(&g_scr);

    if (sim) {
        /* the records: one RULE write per press with its t, one frame-loop write */
        int nr = 0, nl = 0, k, m = yrt_ring_drain(&ring, g_rec, 8192);
        for (k = 0; k < m; k++) {
            const yrt_event* r = &g_rec[k];
            uint32_t fl;
            if (r->source != YRT_SRC_DEVICE || r->kind != YDEV_REC_OUT) continue;
            fl = r->u.u16[18];
            if (fl & YDEV_OUT_TRAILING) continue;
            if (r->aux == 3 && r->u.u32[8] == CODE_RULE) {
                if (!(fl & YDEV_OUT_RULE) || (fl & YDEV_OUT_FAILED) || nr >= got || r->u.i64[3] != g_t[nr]) bad++;
                nr++;
            } else if (r->aux == 2 && r->u.u32[8] == CODE_LOOP) {
                if (fl & YDEV_OUT_RULE) bad++;
                nl++;
            }
        }
        printf("response_lag --sim: %d presses, %d frame-loop writes, %d rule writes, %d bad records\n", g_btn.n, nl,
               nr, bad);
        if (nr != n || nl != n || rgot != n) bad++;
    }
    if (csv) {
        FILE* fp = fopen(csv, "wb");
        if (fp) {
            fprintf(fp, "press,stamp_minus_emit_ns,frame_loop_ns,rule_ns\n");
            for (i = 0; i < got; i++)
                fprintf(fp, "%d,%lld,%lld,%lld\n", i, (long long)(i < g_btn.n ? g_t[i] - g_btn.t_emit[i] : 0),
                        (long long)g_loop[i], (long long)(i < rgot ? g_rlag[i] : -1));
            fclose(fp);
        }
    }
    for (i = 0; i < got && i < g_btn.n; i++) g_stamp[i] = g_t[i] - g_btn.t_emit[i];
    printf("response_lag: %d presses, %d frame-loop writes, %d rule writes, %d frames\n", g_btn.n, got, rgot, frames);
    {
        int nf = frames < MAX_FRAMES ? frames : MAX_FRAMES, k;
        for (k = 1; k < nf; k++) g_iter[k - 1] = g_iter[k] - g_iter[k - 1];
        summary("begin() return to the next", g_iter, nf - 1);
    }
    summary("stamp minus bytes readable", g_stamp, got < g_btn.n ? got : g_btn.n);
    summary("frame loop: begin() minus stamp", g_begin, got);
    summary("frame loop: write minus stamp", g_loop, got);
    if (use_rule) summary("rule: write minus stamp", g_rlag, rgot);
    if (use_rule) printf("response_lag: %d rows with YRSP_R_SENT_MISMATCH\n", mismatch);
    return got == n && g_btn.n == n && (!use_rule || rgot == n) && !bad && !mismatch ? 0 : 1;
}
