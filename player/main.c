/* player/main.c - the ysp_player command line.
 *
 * Usage: ysp_player PACK --out DIR [--sim] [--seed N] [--field NAME=VALUE]...
 *                   [--rig FILE|NAME] [--cal FILE] [--windowed] [--allow-below-tier ROLE[,ROLE]]
 *                   [--participant SPEC] [--rt S] [--max-trials N]
 *                   [--measure] [--trace-channels FILE] [--rdk-buffer N]
 *                   [--sim-hz HZ] [--test-sample-late ROLE=S]... [--test-sample-bound ROLE=S]...
 *                   [--test-feed-each] [--test-live-cap N]
 *
 * Without --sim the session runs for real: a window on the display (full
 * screen; --windowed for development), the keyboard and the rig profile's
 * devices (--rig, else the user's profile.json when there is one), and the
 * real clock. Shift+Esc stops it. A role whose tier (from the rig profile's
 * checked loopbacks) is worse than its min_tier refuses the start, unless
 * --allow-below-tier names it (each role's check stays for the others),
 * which session.json records.
 * --sim: the simulated display, the virtual clock, the null audio device
 * and a scripted participant. SPEC is "correct" (the default: each
 * trial's key_answer after --rt seconds), "silent", "p=0.8" (correct with
 * that probability), or "script:FILE" (a CSV of
 * trial_index,response,rt_s,hold_s). DIR must not exist.
 * --rdk-buffer sets an RDK's step buffer (tests: a small one flushes often).
 * Format version 2 (--sim): --samples FILE is the scripted participant of
 * the signal roles, a CSV of t_s,role,x,y[,arr_s] (each a raw mouse's counts, t_s
 * from the first frame's onset; arr_s when the report reaches the player,
 * t_s when left out); --sim-gain ROLE=UNITS is each role's
 * adapter scale per count (the rig profile has no adapter block, G18);
 * --test-draws FILE writes every leaf's trials and variables without
 * running a frame (tests: the C programs take the player's draws).
 * --sim-hz HZ: the simulated display's rate (default 60), its period
 * rounded to whole ns. --test-sample-late ROLE=S: that role's samples reach
 * the player S seconds after their t_s (tests: a late device report); the
 * player waits for it (its lateness bound, docs/player.md "Explanation:
 * late input"). --test-sample-bound ROLE=S: the bound the player waits for
 * in place of the late value (tests: a report later than its bound).
 * --test-live-cap N (a real run): one poll holds N raw mouse samples, not
 * 8,192 (tests: a full buffer stops the session).
 * --test-feed-each: each sample in its own ysig_feed(), its hits taken
 * before the next sample, in place of one ysig_feed_n() a poll (tests:
 * format 3.11 rule 4's check that both give the same results).
 * --profile (debug; a build with YSP_TRACE_RING): every zone of the frame
 * loop to DIR/profile.csv; tools/profile_summary.py summarizes it.
 * --hang-at-trial N stops the frame loop in trial N (tests: the panic
 * watchdog's case, tests/player/real_test.py). --test-lose-code N: the
 * screen never writes the Nth code at a flip, as a canceled trigger
 * (tests: the device records' matching). --test-plan-early S: a response's
 * plan onset S seconds before the timeline's, as a landing frame shown late
 * gives (tests: the wait for the flip record).
 *
 * --version prints "ysp_player VERSION" (PL_VERSION_STRING) and exits 0:
 * the designer runs only the player of its own version (designer.md,
 * "Plan of step 6", decision 5).
 *
 * Exit code: 0; 1 when the pack, the definition or the rig is refused or
 * the run failed (the message goes to stderr); 2 for a bad argument; 3
 * when the operator stopped the session (Shift+Esc, the window's close; in
 * --sim, SIGTERM): the data set is closed, and session.json says so
 * ("aborted").
 */
#if !defined(_WIN32) && !defined(_DEFAULT_SOURCE) && !defined(_GNU_SOURCE) && !defined(_POSIX_C_SOURCE) && !defined(_XOPEN_SOURCE)
#define _DEFAULT_SOURCE 1   /* sigaction() under -std=c11; before any system header */
#endif
#include "player.h"

#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static pl_player g_p;

/* --sim's SIGTERM: the designer's Stop (designer.md, "Stop"). Only the flag:
 * the frame loop reads it (pl_run()) and stops as an operator's Shift+Esc
 * does, so the finished trials keep their rows and session.json says
 * "aborted". A real run does not install it: SDL makes SIGTERM a quit
 * event there. Windows sends no SIGTERM to another process: there the
 * designer's Stop has no window to close in --sim, and only Kill (no
 * "aborted", the data set not closed) ends it. */
static void pl_on_term(int sig) {
    (void)sig;
    pl_stop_requested = 1;
}

static void pl_term_handler(void) {
#if defined(_WIN32)
    (void)signal(SIGTERM, pl_on_term);
#else
    /* sigaction: the handler stays for a second SIGTERM (signal()'s may
     * reset to the default and end the process mid-write) */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = pl_on_term;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    (void)sigaction(SIGTERM, &sa, NULL);
#endif
}

static int usage(void) {
    fprintf(stderr, "usage: ysp_player PACK --out DIR [--sim] [--seed N] [--field NAME=VALUE]... [--rig FILE|NAME] [--cal FILE]\n"
                    "                  [--windowed] [--allow-below-tier ROLE[,ROLE]] [--max-trials N] [--measure]\n"
                    "       --sim only: [--participant SPEC] [--rt S] [--max-seconds N]\n"
                    "                   [--samples FILE] [--sim-gain ROLE=UNITS]... [--test-draws FILE] [--sim-hz HZ]\n"
                    "       debug: [--profile] (a YSP_TRACE_RING build)\n"
                    "       ysp_player --version\n"
                    "  SPEC: correct, silent, p=0.8 or script:FILE\n");
    return 2;
}

static int run_main(pl_args* ap, int argc, char** argv);

/* The field names are the one allocation of the arguments: freed on every
 * path out, early returns included, so a sanitizer's leak check stays quiet. */
int main(int argc, char** argv) {
    pl_args a;
    int i, rc;
    memset(&a, 0, sizeof a);
    rc = run_main(&a, argc, argv);
    for (i = 0; i < a.n_fields; i++) free((void*)a.fields[i].name);
    for (i = 0; i < a.n_gains; i++) free((void*)a.gains[i].name);
    for (i = 0; i < a.n_lates; i++) free((void*)a.lates[i].name);
    for (i = 0; i < a.n_bounds; i++) free((void*)a.bounds[i].name);
    return rc;
}

static int run_main(pl_args* ap, int argc, char** argv) {
#define a (*ap)
    int i, rc;
    a.max_seconds = 172800;
    for (i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--version")) {
            printf("ysp_player %s\n", PL_VERSION_STRING);
            return 0;
        }
    for (i = 1; i < argc; i++) {
        const char* s = argv[i];
        const char* v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(s, "--sim")) a.sim = true;
        else if (!strcmp(s, "--seed") && v) { a.seed = strtoull(v, NULL, 10); a.have_seed = true; i++; }
        else if (!strcmp(s, "--out") && v) { a.out_dir = v; i++; }
        else if (!strcmp(s, "--rig") && v) { a.rig_path = v; i++; }
        else if (!strcmp(s, "--cal") && v) { a.cal_path = v; i++; }
        else if (!strcmp(s, "--participant") && v) { a.participant = v; i++; }
        else if (!strcmp(s, "--rt") && v) { a.rt_s = atof(v); i++; }
        else if (!strcmp(s, "--max-trials") && v) { a.max_trials = atoi(v); i++; }
        else if (!strcmp(s, "--max-seconds") && v) { a.max_seconds = atoll(v); i++; }
        else if (!strcmp(s, "--measure")) a.measure = true;
        else if (!strcmp(s, "--windowed")) a.windowed = true;
        else if (!strcmp(s, "--allow-below-tier") && v) {
            /* a flag here is a forgotten value (it would swallow that flag);
             * an empty name names no role: both are typos, not overrides */
            size_t n = strlen(v);
            if (v[0] == '-' || n == 0 || v[0] == ',' || v[n - 1] == ',' || strstr(v, ",,")) {
                fprintf(stderr, "ysp_player: --allow-below-tier '%s': give role names, ROLE[,ROLE], with no empty entry\n", v);
                return 2;
            }
            a.allow_below = v;
            i++;
        }
        else if (!strcmp(s, "--hang-at-trial") && v) { a.hang_trial = atoi(v) + 1; i++; }
        else if (!strcmp(s, "--test-lose-code") && v) { a.lose_code = atoi(v); i++; }
        else if (!strcmp(s, "--test-plan-early") && v) { a.plan_early_ns = (int64_t)llround(atof(v) * 1e9); i++; }
        else if (!strcmp(s, "--test-record-late") && v) { a.record_late_ns = (int64_t)llround(atof(v) * 1e9); i++; }
        else if (!strcmp(s, "--test-record-delay") && v) { a.record_delay = atoi(v); i++; }
        else if (!strcmp(s, "--test-live-cap") && v) { a.live_cap = atoi(v); i++; }
        else if (!strcmp(s, "--test-feed-each")) a.feed_each = true;
        else if (!strcmp(s, "--trace-channels") && v) { a.trace_path = v; i++; }
        else if (!strcmp(s, "--rdk-buffer") && v) { a.rdk_buffer = atoi(v); i++; }
        else if (!strcmp(s, "--samples") && v) { a.samples_path = v; i++; }
        else if (!strcmp(s, "--test-draws") && v) { a.draws_path = v; i++; }
        else if (!strcmp(s, "--profile")) a.profile = true;
        else if (!strcmp(s, "--sim-hz") && v) {
            /* whole ns, as the default 1/60 s is: 60 gives screen.h's own
             * 16,666,667, so a 60 Hz run keeps its bytes */
            double hz = atof(v);
            if (!(hz >= 1.0 && hz <= 10000.0)) {
                fprintf(stderr, "ysp_player: --sim-hz '%s': give 1 to 10000 frames a second\n", v);
                return 2;
            }
            a.sim_period_ns = (int64_t)llround(1e9 / hz);
            i++;
        }
        else if (!strcmp(s, "--test-sample-bound") && v) {
            /* ROLE=S: the lateness bound the player waits for, not the
             * late value: a report later than its bound (N1) */
            const char* eq = strchr(v, '=');
            double b = eq ? atof(eq + 1) : -1.0;
            if (!eq || eq == v || !(b >= 0.0 && b <= 10.0) || a.n_bounds == 16) {
                fprintf(stderr, "ysp_player: --test-sample-bound '%s': give ROLE=S, 0 to 10 s\n", v);
                return 2;
            }
            {
                char* name = (char*)malloc((size_t)(eq - v) + 1);
                if (!name) return 1;
                memcpy(name, v, (size_t)(eq - v));
                name[eq - v] = 0;
                a.bounds[a.n_bounds].name = name;
                a.bounds[a.n_bounds].value = eq + 1;
                a.n_bounds++;
            }
            i++;
        }
        else if (!strcmp(s, "--test-sample-late") && v) {
            /* ROLE=S: a device whose reports come S seconds after their
             * stamp (transport, a tablet's driver); the stamp stays */
            const char* eq = strchr(v, '=');
            double late = eq ? atof(eq + 1) : -1.0;
            if (!eq || eq == v || !(late >= 0.0 && late <= 10.0) || a.n_lates == 16) {
                fprintf(stderr, "ysp_player: --test-sample-late '%s': give ROLE=S, 0 to 10 s\n", v);
                return 2;
            }
            {
                char* name = (char*)malloc((size_t)(eq - v) + 1);
                if (!name) return 1;
                memcpy(name, v, (size_t)(eq - v));
                name[eq - v] = 0;
                a.lates[a.n_lates].name = name;
                a.lates[a.n_lates].value = eq + 1;
                a.n_lates++;
            }
            i++;
        }
        else if (!strcmp(s, "--sim-gain") && v) {
            /* ROLE=UNITS: the --sim adapter's scale per count (G18 has no
             * rig block for it, 3.19 item 3) */
            const char* eq = strchr(v, '=');
            if (!eq || eq == v || !(atof(eq + 1) > 0) || a.n_gains == 16) {
                fprintf(stderr, "ysp_player: --sim-gain '%s': give ROLE=UNITS, units per count above 0\n", v);
                return 2;
            }
            {
                char* name = (char*)malloc((size_t)(eq - v) + 1);
                if (!name) return 1;
                memcpy(name, v, (size_t)(eq - v));
                name[eq - v] = 0;
                a.gains[a.n_gains].name = name;
                a.gains[a.n_gains].value = eq + 1;
                a.n_gains++;
            }
            i++;
        }
        else if (!strcmp(s, "--field") && v) {
            const char* eq = strchr(v, '=');
            if (!eq || a.n_fields == PL_MAX_FIELDS) return usage();
            {
                char* name = (char*)malloc((size_t)(eq - v) + 1);
                if (!name) return 1;
                memcpy(name, v, (size_t)(eq - v));
                name[eq - v] = 0;
                a.fields[a.n_fields].name = name;
                a.fields[a.n_fields].value = eq + 1;
                a.n_fields++;
            }
            i++;
        } else if (s[0] == '-') return usage();
        else if (!a.pack_path) a.pack_path = s;
        else return usage();
    }
    if (!a.pack_path || !a.out_dir) return usage();
    if (!a.sim && (a.participant || a.rt_s != 0.0)) {
        fprintf(stderr, "ysp_player: --participant and --rt are for --sim: a real run's participant is real\n");
        return 2;
    }
    if (!a.sim && (a.record_late_ns || a.record_delay)) {
        fprintf(stderr, "ysp_player: --test-record-late and --test-record-delay are for --sim\n");
        return 2;
    }
    if (a.live_cap && (a.sim || a.live_cap < 1 || a.live_cap > 8192)) {
        fprintf(stderr, "ysp_player: --test-live-cap is for a real run, 1 to 8192 samples\n");
        return 2;
    }
    if (a.record_delay < 0 || a.record_delay > 8 || a.record_late_ns < 0) {
        fprintf(stderr, "ysp_player: --test-record-delay takes 0 to 8 frames, --test-record-late a time of 0 or more\n");
        return 2;
    }
    if (!a.sim && (a.samples_path || a.n_gains || a.draws_path)) {
        fprintf(stderr, "ysp_player: --samples, --sim-gain and --test-draws are for --sim\n");
        return 2;
    }
    if (!a.sim && (a.sim_period_ns || a.n_lates || a.n_bounds || a.feed_each)) {
        fprintf(stderr, "ysp_player: --sim-hz, --test-sample-late, --test-sample-bound and --test-feed-each are for --sim\n");
        return 2;
    }
#if !defined(YRT_TRACE_RING)
    /* before the data set exists: a run that cannot profile writes nothing */
    if (a.profile) {
        fprintf(stderr, "ysp_player: --profile needs a build with YSP_TRACE_RING (ysp/rt.h's trace ring); this one has no zones\n");
        return 2;
    }
#endif
    if (a.sim && (a.windowed || a.hang_trial || a.lose_code)) { fprintf(stderr, "ysp_player: --windowed, --hang-at-trial and --test-lose-code are for a real run\n"); return 2; }
#if defined(YSCR_NO_SDL)
    if (!a.sim) { fprintf(stderr, "ysp_player: built without SDL (YSCR_NO_SDL): --sim only\n"); return 2; }
#endif
    /* --sim runs on the virtual clock from before anything reads it, so the
     * records are the same on every host and every run */
    if (a.sim) yrt_virtual_begin(0);
    /* before the load: a Stop during it ends the session at its first
     * frame, with no trial, rather than by the signal */
    if (a.sim && !a.draws_path) pl_term_handler();
    if (pl_load(&g_p, &a) < 0) {
        fprintf(stderr, "ysp_player: %s\n", g_p.error);
        rc = g_p.bad_arg ? 2 : 1;
        pl_close(&g_p);
        return rc;
    }
    rc = a.draws_path ? pl_v2_draws(&g_p) : pl_run(&g_p);
    if (rc < 0) { fprintf(stderr, "ysp_player: %s\n", g_p.error); pl_close(&g_p); return 1; }
    if (g_p.aborted) {
        /* abort_trial -1: the stop came between two trials */
        if (g_p.abort_trial >= 0)
            printf("ysp_player: stopped by the operator; %d trials written to %s (trial %d has no row)\n", g_p.trials_done, a.out_dir, g_p.abort_trial);
        else
            printf("ysp_player: stopped by the operator; %d trials written to %s (no trial was running)\n", g_p.trials_done, a.out_dir);
        pl_close(&g_p);
        return 3;
    }
    if (a.measure) {
        printf("ysp_player: frame work over %lld frames: max %.1f us (frame %lld); trial start max %.1f us; %lld frames over 16 ms\n",
               (long long)g_p.work_n, (double)g_p.work_max_ns / 1e3, (long long)g_p.work_max_frame, (double)g_p.work_trial_max_ns / 1e3,
               (long long)g_p.work_over_16ms);
        if (g_p.work_hist && g_p.work_n > 0) {
            /* a bin's upper edge: a percentile is never under-stated */
            static const double q[3] = { 0.5, 0.99, 0.999 };
            double us[3];
            int64_t acc = 0, need;
            int b = 0, k;
            for (k = 0; k < 3; k++) {
                need = (int64_t)ceil(q[k] * (double)g_p.work_n);
                while (b < PL_WORK_BINS - 1 && acc + g_p.work_hist[b] < need) acc += g_p.work_hist[b++];
                us[k] = (double)(b + 1) * PL_WORK_BIN_NS / 1e3;
            }
            printf("ysp_player: frame work p50 %.1f us, p99 %.1f us, p99.9 %.1f us; %lld frames over the period (%.3f ms)\n", us[0], us[1], us[2],
                   (long long)g_p.work_over_period, (double)g_p.caps.period_ns / 1e6);
        }
        if (a.sim)
            printf("ysp_player: --sim writes in place, not in the frame work above (a real run queues them to its writer thread): max %.1f us in a frame (frame %lld)\n",
                   (double)g_p.work_wr_max_ns / 1e3, (long long)g_p.work_wr_max_frame);
        if (!a.sim)
            printf("ysp_player: the input (SDL's pump and the collectors) max %.1f us (frame %lld)\n", (double)g_p.work_input_max_ns / 1e3,
                   (long long)g_p.work_input_max_frame);
    }
    printf("ysp_player: %d trials, %s, seed %llu, experiment %s\n", g_p.trials_done, a.out_dir, (unsigned long long)g_p.seed, g_p.experiment_id);
    pl_close(&g_p);
    return 0;
#undef a
}
