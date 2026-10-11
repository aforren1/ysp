/* audio_schedule.c - sounds on a schedule at stated times on the ysp_rt
 * clock, each with its onset record.
 *
 * Mirrors Psychtoolbox-3's PsychPortAudio schedule demos, read for
 * coverage only (no code copied): BasicSoundScheduleDemo.m (a playlist of
 * preloaded buffers, added to while it plays), SimpleSoundScheduleDemo.m
 * (beeps at set times after a trigger), BasicAMAndMixScheduleDemo.m (voices
 * mixed, a volume per voice) and BasicSoundChannelHoppingDemo.m (a sound
 * on one channel, then the other).
 *
 *     audio_schedule [--null | --sim] [--device NAME] [--exclusive]
 *                    [--period N] [--queue Q] [--db DB] [--probe-n N]
 *                    [--csv FILE]
 *
 *   --null, --sim  miniaudio's null device (no sound hardware), with a
 *                  self-check that exits 1 on a mismatch (CI runs this)
 *   --device NAME  the playback device whose name contains NAME
 *   --exclusive    WASAPI exclusive mode (every other program goes silent)
 *   --period N     frames per callback asked for (desc.period)
 *   --queue Q      WASAPI shared periods queued ahead of the engine
 *                  (desc.queue: 1 or 2; default 2)
 *   --db DB        peak level in dBFS (default -40; above -30 is refused:
 *                  this is a demo, not a stimulus)
 *   --probe-n N    sounds per lead in the lead probe (default 10; --null 3)
 *   --csv FILE     one row per sound, times in seconds
 *
 * Four parts, all planned from one trigger time T0 (the program's start
 * plus 0.5 s; SimpleSoundScheduleDemo.m waits for a key instead):
 *   schedule  tones and clicks at stated times, on the left, the right or
 *             both channels, one inside a tone (the mix). Each is handed
 *             over 0.25 s before its time, so the playlist grows while it
 *             plays.
 *   repeat    one buffer played 4 times back to back by .loops (one
 *             record, sample-exact), then started 4 times at stated times
 *             (4 records).
 *   stop      a 2 s tone, -12 dB from +0.4 s (yau_gain_at), stopped at +1 s
 *             with a 20 ms fade (yau_stop_at).
 *   probe     silent 10 ms buffers handed over at shorter and shorter leads;
 *             the shortest lead with no LATE record is this machine's
 *             shortest safe lead, printed beside yau_lead_ns().
 * Each sound prints its planned time, the fit's time (the onset), the
 * residual, the start frame, the lead the path used and its tier. Every
 * onset is the device-clock fit's time, a model value (ysp/audio.h,
 * CONFIRMATION): the spread here is against the plan, not at the jack.
 *
 * Exit: 0, 1 when the device did not open, a record is missing or a
 * --null check failed, 2 usage.
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#define YSP_AUDIO_IMPLEMENTATION
#include "ysp/audio.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define T0_LEAD_S   0.5
#define HANDOVER_S  0.25   /* the schedule's own lead: well over the 40 ms
                            * measured as enough with the default queue
                            * (docs/audio.md) */
#define MAX_ITEMS   256
#define N_LEADS     19

enum { TONE, CLICK };
enum { CH_BOTH = 0, CH_LEFT = 1, CH_RIGHT = 2 };   /* output masks */

typedef struct entry { double at; int kind; double hz; uint64_t mask; const char* name; } entry;
static const entry schedule[] = {
    { 0.00, TONE,   500, CH_BOTH,  "tone 500 Hz, both" },
    { 0.25, CLICK,    0, CH_LEFT,  "click, left" },
    { 0.50, CLICK,    0, CH_RIGHT, "click, right" },
    { 0.75, TONE,   750, CH_LEFT,  "tone 750 Hz, left" },
    { 1.00, TONE,  1000, CH_RIGHT, "tone 1000 Hz, right" },
    { 1.25, CLICK,    0, CH_BOTH,  "click, both" },
    { 1.50, TONE,  1500, CH_BOTH,  "tone 1500 Hz, both" },
    { 1.55, CLICK,    0, CH_BOTH,  "click inside the tone" },
};
#define N_SCHEDULE ((int)(sizeof schedule / sizeof schedule[0]))
#define REPEAT_LOOP_S  2.0
#define REPEAT_START_S 2.5      /* then every 0.2 s, 4 times */
#define STOP_START_S   3.5
#define STOP_GAIN_S    0.4      /* after the start */
#define STOP_AT_S      1.0
static const double leads_ms[N_LEADS] = { 200, 150, 125, 100, 75, 60, 55, 50, 45, 40, 35, 30, 25, 20, 15, 12, 10, 8, 5 };

typedef struct item {
    const char* part;
    char        name[32];
    double      at;             /* planned, s after T0 */
    yau_id      id;
    yau_onset   r;
    int         rc;
    int64_t     lead_now;       /* yau_lead_ns() at the handover (probe) */
} item;

static yau_audio au;
static item items[MAX_ITEMS];
static int n_items;
static int64_t t0;

static item* add(const char* part, const char* name, double at) {
    item* it = &items[n_items++];
    memset(it, 0, sizeof *it);
    it->part = part;
    snprintf(it->name, sizeof it->name, "%s", name);
    it->at = at;
    it->rc = YAU_PENDING;
    return it;
}

static int64_t at_ns(double s) { return t0 + YRT_S(s); }

static void wait_until(double s) { yrt_sleep_until((int64_t)at_ns(s), YRT_DEFAULT_SPIN_NS); }

/* The final record: the onset, and the end frame once the sound ended. */
static void collect(item* it, int need_end) {
    int k;
    if (it->id <= 0) { it->rc = (int)it->id; return; }
    it->rc = yau_wait(&au, it->id, YRT_S(1), &it->r);
    for (k = 0; need_end && it->rc == YAU_OK && it->r.end_frame == 0 && k < 200; k++) {
        yrt_sleep_ns(10000000);
        it->rc = yau_result(&au, it->id, &it->r);
    }
}

static void print_item(const item* it) {
    if (it->rc != YAU_OK) { printf("  %-24s no record: %s\n", it->name, yau_strerror(it->rc)); return; }
    printf("  %-24s plan %+9.6f s  onset %+9.6f s  residual %+7.1f us  frame %8lld  lead %6.2f ms  tier %d  "
           "flags 0x%03x\n", it->name, it->at, (double)(it->r.onset - t0) / 1e9, (double)it->r.residual / 1e3,
           (long long)it->r.start_frame, (double)(it->r.onset - it->r.rendered_at) / 1e6, it->r.tier,
           (unsigned)it->r.flags);
}

static int cmp_i64(const void* a, const void* b) {
    int64_t x = *(const int64_t*)a, y = *(const int64_t*)b;
    return x < y ? -1 : (x > y);
}

int main(int argc, char** argv) {
    yau_desc d;
    yau_caps caps;
    yau_buf tones[4], click, rep, longtone, silent;
    yau_play_desc pd;
    item *loop_it = NULL, *stop_it = NULL, *starts[4] = { NULL, NULL, NULL, NULL };
    int64_t res[MAX_ITEMS], leads_seen[MAX_ITEMS];
    char line[512];
    const char* csv_path = NULL;
    double db = -40, frame_us;
    int i, k, sim = 0, probe_n = 0, missing = 0, bad = 0, n_res = 0, n_seen = 0, safe = -1;
    int late_at[N_LEADS];
    static const double hz[4] = { 500, 750, 1000, 1500 };

    memset(&d, 0, sizeof d);
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--null") || !strcmp(argv[i], "--sim")) sim = 1;
        else if (!strcmp(argv[i], "--device") && i + 1 < argc) d.device = argv[++i];
        else if (!strcmp(argv[i], "--exclusive")) d.exclusive = true;
        else if (!strcmp(argv[i], "--period") && i + 1 < argc) d.period = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--queue") && i + 1 < argc) d.queue = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--db") && i + 1 < argc) db = atof(argv[++i]);
        else if (!strcmp(argv[i], "--probe-n") && i + 1 < argc) probe_n = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--csv") && i + 1 < argc) csv_path = argv[++i];
        else {
            fprintf(stderr, "usage: audio_schedule [--null | --sim] [--device NAME] [--exclusive] [--period N] [--queue Q]\n"
                            "                      [--db DB] [--probe-n N] [--csv FILE]\n");
            return 2;
        }
    }
    if (probe_n == 0) probe_n = sim ? 3 : 10;
    if (db > -30 || probe_n < 1 || probe_n > 12) { fprintf(stderr, "audio_schedule: --db at most -30, --probe-n 1..12\n"); return 2; }
    if (sim) d.backend = YAU_BACKEND_NULL;
    d.arena_bytes = 2 << 20;
    if (!yau_open(&au, &d)) { fprintf(stderr, "audio_schedule: %s\n", yau_error(&au)); return 1; }
    yau_describe(&au, line, sizeof line);
    puts(line);
    yau_get_caps(&au, &caps);
    frame_us = 1e6 / (double)caps.rate;

    /* every buffer before the schedule starts: synthesis allocates from the
     * arena, which must not happen while the frame thread keeps time */
    {
        yau_tone_desc td;
        yau_click_desc cd;
        memset(&td, 0, sizeof td);
        td.dur_ns = YRT_MS(100);
        td.peak = yau_db((float)db);
        td.ramp_ns = YRT_MS(5);
        for (i = 0; i < 4; i++) { td.hz = hz[i]; tones[i] = yau_tone(&au, &td); }
        td.hz = 1200; td.dur_ns = YRT_MS(60);
        rep = yau_tone(&au, &td);
        td.hz = 400; td.dur_ns = YRT_S(2);
        longtone = yau_tone(&au, &td);
        memset(&cd, 0, sizeof cd);
        cd.dur_ns = YRT_MS(1);
        cd.peak = yau_db((float)db);
        click = yau_click(&au, &cd);
        silent = yau_alloc(&au, (int64_t)caps.rate / 100, 1);   /* 10 ms of zeros */
        if (!tones[0].frames || !tones[1].frames || !tones[2].frames || !tones[3].frames || !rep.frames ||
            !longtone.frames || !click.frames || !silent.frames) {
            fprintf(stderr, "audio_schedule: %s\n", yau_error(&au));
            yau_close(&au);
            return 1;
        }
    }

    t0 = yrt_now_ns() + YRT_S(T0_LEAD_S);
    printf("schedule from T0 (the trigger) = %.6f s on the ysp_rt clock; a frame is %.1f us\n", (double)t0 / 1e9, frame_us);

    /* schedule: each sound handed over HANDOVER_S before its time */
    for (i = 0; i < N_SCHEDULE; i++) {
        const entry* e = &schedule[i];
        item* it = add("schedule", e->name, e->at);
        wait_until(e->at - HANDOVER_S);
        memset(&pd, 0, sizeof pd);
        pd.buf = e->kind == CLICK ? click : tones[e->hz == 500 ? 0 : e->hz == 750 ? 1 : e->hz == 1000 ? 2 : 3];
        pd.at_ns = at_ns(e->at);
        pd.channels = e->mask;
        it->id = yau_play(&au, &pd);
    }
    /* repeat: 4 passes of one buffer as one play, then 4 starts of it */
    wait_until(REPEAT_LOOP_S - HANDOVER_S);
    loop_it = add("repeat", "60 ms tone, 4 passes", REPEAT_LOOP_S);
    memset(&pd, 0, sizeof pd);
    pd.buf = rep;
    pd.at_ns = at_ns(REPEAT_LOOP_S);
    pd.loops = 3;
    loop_it->id = yau_play(&au, &pd);
    for (k = 0; k < 4; k++) {
        char nm[32];
        double at = REPEAT_START_S + 0.2 * k;
        snprintf(nm, sizeof nm, "60 ms tone, start %d", k + 1);
        wait_until(at - HANDOVER_S);
        starts[k] = add("repeat", nm, at);
        starts[k]->id = yau_play_at(&au, rep, at_ns(at));
    }
    /* stop: a long tone, a gain change while it plays, a stop at a time */
    wait_until(STOP_START_S - HANDOVER_S);
    stop_it = add("stop", "2 s tone, stopped at 1 s", STOP_START_S);
    stop_it->id = yau_play_at(&au, longtone, at_ns(STOP_START_S));
    if (stop_it->id > 0) {
        if (yau_gain_at(&au, stop_it->id, at_ns(STOP_START_S + STOP_GAIN_S), -12.0f, YRT_MS(100)) < 0 ||
            yau_stop_at(&au, stop_it->id, at_ns(STOP_START_S + STOP_AT_S), YRT_MS(20)) < 0)
        {
            fprintf(stderr, "audio_schedule: gain or stop refused\n");
            bad = 1;
        }
    }
    wait_until(STOP_START_S + STOP_AT_S + 0.1);
    for (i = 0; i < n_items; i++) collect(&items[i], &items[i] == loop_it || &items[i] == stop_it);

    /* probe: the shortest lead with no LATE record, 10 ms buffers of
     * silence so the probe makes no sound; the lead is from now, at the
     * handover, which falls at a new phase of the 10 ms callback each time */
    for (k = 0; k < N_LEADS; k++) {
        int first = n_items;
        late_at[k] = 0;
        for (i = 0; i < probe_n; i++) {
            char nm[32];
            int64_t now = yrt_now_ns(), t = now + YRT_MS(leads_ms[k]);
            item* it;
            snprintf(nm, sizeof nm, "lead %.0f ms", leads_ms[k]);
            it = add("probe", nm, (double)(t - t0) / 1e9);
            it->lead_now = yau_lead_ns(&au);
            it->id = yau_play_at(&au, silent, t);
            yrt_sleep_until((int64_t)(t + YRT_MS(23) + (int64_t)i * 1700000), YRT_DEFAULT_SPIN_NS);
        }
        for (i = first; i < n_items; i++) {
            collect(&items[i], 0);
            if (items[i].rc == YAU_OK && (items[i].r.flags & YAU_ONSET_LATE)) late_at[k]++;
            if (items[i].lead_now > 0 && n_seen < MAX_ITEMS) leads_seen[n_seen++] = items[i].lead_now;
        }
    }

    /* the report */
    printf("sound                     planned and fit times are seconds after T0\n");
    for (i = 0; i < n_items; i++) {
        item* it = &items[i];
        if (it->rc != YAU_OK) { missing++; print_item(it); continue; }
        if (strcmp(it->part, "probe")) {
            print_item(it);
            res[n_res++] = it->r.residual;
        }
    }
    if (n_res > 0) {
        int64_t lo, hi, worst_iv = 0;
        double sum = 0;
        qsort(res, (size_t)n_res, sizeof res[0], cmp_i64);
        lo = res[0];
        hi = res[n_res - 1];
        for (i = 0; i < n_res; i++) sum += (double)llabs(res[i]);
        for (i = 1; i < N_SCHEDULE; i++) {
            const item *a = &items[i - 1], *b = &items[i];
            int64_t iv;
            if (a->rc != YAU_OK || b->rc != YAU_OK) continue;
            iv = (b->r.onset - a->r.onset) - (YRT_S(b->at) - YRT_S(a->at));
            if (llabs(iv) > llabs(worst_iv)) worst_iv = iv;
        }
        printf("residual (fit onset - plan), %d sounds: min %+.1f us, max %+.1f us, spread %.1f us, mean |r| %.1f us "
               "(rounding to the nearest frame alone allows +-%.1f us)\n", n_res, (double)lo / 1e3, (double)hi / 1e3,
               (double)(hi - lo) / 1e3, sum / n_res / 1e3, frame_us / 2);
        printf("schedule intervals, onset distance minus planned distance: worst %+.1f us\n", (double)worst_iv / 1e3);
    }
    if (loop_it && loop_it->rc == YAU_OK)
        printf("repeat by loops: %lld frames played, 4 x %lld planned\n",
               (long long)(loop_it->r.end_frame - loop_it->r.start_frame), (long long)rep.n);
    for (k = 1; k < 4; k++)
        if (starts[k] && starts[k - 1] && starts[k]->rc == YAU_OK && starts[k - 1]->rc == YAU_OK)
            printf("repeat start %d - start %d: %lld frames (0.2 s is %.1f)\n", k + 1, k,
                   (long long)(starts[k]->r.start_frame - starts[k - 1]->r.start_frame), 0.2 * caps.rate);
    if (stop_it && stop_it->rc == YAU_OK)
        printf("stop: ended at frame %lld, %+.1f us from its time (the fit's frame for it: %lld), flags 0x%03x\n",
               (long long)stop_it->r.end_frame,
               (double)(yau_time_of(&au, stop_it->r.end_frame) - at_ns(STOP_START_S + STOP_AT_S)) / 1e3,
               (long long)yau_frame_at(&au, at_ns(STOP_START_S + STOP_AT_S)), (unsigned)stop_it->r.flags);
    printf("lead probe, %d sounds per lead:", probe_n);
    for (k = 0; k < N_LEADS; k++) printf(" %.0f ms %d late%s", leads_ms[k], late_at[k], k + 1 < N_LEADS ? "," : "\n");
    for (k = 0; k < N_LEADS && late_at[k] == 0; k++) safe = k;
    if (n_seen > 0) {
        qsort(leads_seen, (size_t)n_seen, sizeof leads_seen[0], cmp_i64);
        printf("yau_lead_ns() at the handovers: min %.2f, median %.2f, max %.2f ms\n", (double)leads_seen[0] / 1e6,
               (double)leads_seen[n_seen / 2] / 1e6, (double)leads_seen[n_seen - 1] / 1e6);
    }
    if (safe >= 0) printf("shortest safe lead on this machine: %.0f ms (no LATE record at it or at any longer lead)\n", leads_ms[safe]);
    else printf("shortest safe lead: none of the leads probed; even %.0f ms gave a LATE record\n", leads_ms[0]);

    if (csv_path) {
        FILE* f = fopen(csv_path, "w");
        if (!f) { fprintf(stderr, "audio_schedule: cannot write %s\n", csv_path); bad = 1; }
        else {
            fprintf(f, "# %s\n# T0 %.9f s on the ysp_rt clock; times below are seconds after T0\n", line, (double)t0 / 1e9);
            fprintf(f, "part,name,planned,onset,residual,start_frame,end_frame,lead,tier,flags\n");
            for (i = 0; i < n_items; i++) {
                const item* it = &items[i];
                if (it->rc != YAU_OK) { fprintf(f, "%s,%s,%.9f,,,,,,,\n", it->part, it->name, it->at); continue; }
                fprintf(f, "%s,%s,%.9f,%.9f,%.9f,%lld,%lld,%.9f,%d,%u\n", it->part, it->name, it->at,
                        (double)(it->r.onset - t0) / 1e9, (double)it->r.residual / 1e9, (long long)it->r.start_frame,
                        (long long)it->r.end_frame, (double)(it->r.onset - it->r.rendered_at) / 1e9, it->r.tier,
                        (unsigned)it->r.flags);
            }
            fclose(f);
        }
    }
    yau_describe(&au, line, sizeof line);
    puts(line);

    if (sim) {
        /* The null device's fit is of callback times only, so a plan can
         * move with a refit; the bars are what that allows, not tier 2's */
        const int64_t res_bar = YRT_MS(1);
        for (i = 0; i < n_items; i++) {
            const item* it = &items[i];
            int ok = it->rc == YAU_OK && it->r.tier == YAU_TIER_SIM;
            if (ok && strcmp(it->part, "probe"))
                ok = !(it->r.flags & (YAU_ONSET_LATE | YAU_ONSET_CANCELED | YAU_ONSET_XRUN)) && llabs(it->r.residual) <= res_bar;
            if (ok && it == stop_it) ok = (it->r.flags & YAU_ONSET_STOPPED) && it->r.end_frame > 0 &&
                llabs(yau_time_of(&au, it->r.end_frame) - at_ns(STOP_START_S + STOP_AT_S)) <= res_bar;
            if (ok && it == loop_it) ok = it->r.end_frame - it->r.start_frame == 4 * rep.n;
            if (!ok) { fprintf(stderr, "audio_schedule: --null: %s %s: rc %d flags 0x%03x residual %lld ns\n", it->part,
                               it->name, it->rc, (unsigned)it->r.flags, (long long)it->r.residual); bad = 1; }
        }
        /* Each start can move by res_bar with its own refit, so their
         * distance can move by twice that (CI, Ubuntu: 63 frames) */
        for (k = 1; k < 4; k++)
            if (starts[k]->rc == YAU_OK && starts[k - 1]->rc == YAU_OK &&
                llabs((starts[k]->r.start_frame - starts[k - 1]->r.start_frame) - (int64_t)(0.2 * caps.rate)) >
                    2 * (int64_t)caps.rate / 1000) {
                fprintf(stderr, "audio_schedule: --null: start %d is off its planned distance\n", k + 1);
                bad = 1;
            }
        if (late_at[3] != 0) { fprintf(stderr, "audio_schedule: --null: a 100 ms lead was LATE\n"); bad = 1; }
        if (!bad && !missing) printf("--null: every sound came out as planned\n");
    }
    yau_close(&au);
    return (missing || bad) ? 1 : 0;
}
