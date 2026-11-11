/* player/main.c - the ysp_player command line.
 *
 * Usage: ysp_player PACK --out DIR [--sim] [--seed N] [--field NAME=VALUE]...
 *                   [--rig FILE|NAME] [--cal FILE] [--windowed] [--allow-below-tier ROLE[,ROLE]]
 *                   [--participant SPEC] [--rt S] [--max-trials N]
 *                   [--measure] [--trace-channels FILE] [--rdk-buffer N]
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
 * --hang-at-trial N stops the frame loop in trial N (tests: the panic
 * watchdog's case, tests/player/real_test.py). --test-lose-code N: the
 * screen never writes the Nth code at a flip, as a canceled trigger
 * (tests: the device records' matching). --test-plan-early S: a response's
 * plan onset S seconds before the timeline's, as a landing frame shown late
 * gives (tests: the wait for the flip record).
 *
 * Exit code: 0; 1 when the pack, the definition or the rig is refused or
 * the run failed (the message goes to stderr); 2 for a bad argument; 3
 * when the operator stopped the session (Shift+Esc, the window's close):
 * the data set is closed, and session.json says so ("aborted").
 */
#include "player.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static pl_player g_p;

static int usage(void) {
    fprintf(stderr, "usage: ysp_player PACK --out DIR [--sim] [--seed N] [--field NAME=VALUE]... [--rig FILE|NAME] [--cal FILE]\n"
                    "                  [--windowed] [--allow-below-tier ROLE[,ROLE]] [--max-trials N] [--measure]\n"
                    "       --sim only: [--participant SPEC] [--rt S] [--max-seconds N]\n"
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
    return rc;
}

static int run_main(pl_args* ap, int argc, char** argv) {
#define a (*ap)
    int i, rc;
    a.max_seconds = 172800;
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
        else if (!strcmp(s, "--trace-channels") && v) { a.trace_path = v; i++; }
        else if (!strcmp(s, "--rdk-buffer") && v) { a.rdk_buffer = atoi(v); i++; }
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
    if (a.sim && (a.windowed || a.hang_trial || a.lose_code)) { fprintf(stderr, "ysp_player: --windowed, --hang-at-trial and --test-lose-code are for a real run\n"); return 2; }
#if defined(YSCR_NO_SDL)
    if (!a.sim) { fprintf(stderr, "ysp_player: built without SDL (YSCR_NO_SDL): --sim only\n"); return 2; }
#endif
    /* --sim runs on the virtual clock from before anything reads it, so the
     * records are the same on every host and every run */
    if (a.sim) yrt_virtual_begin(0);
    if (pl_load(&g_p, &a) < 0) {
        fprintf(stderr, "ysp_player: %s\n", g_p.error);
        rc = g_p.bad_arg ? 2 : 1;
        pl_close(&g_p);
        return rc;
    }
    rc = pl_run(&g_p);
    if (rc < 0) { fprintf(stderr, "ysp_player: %s\n", g_p.error); pl_close(&g_p); return 1; }
    if (g_p.aborted) {
        printf("ysp_player: stopped by the operator; %d trials written to %s (trial %d has no row)\n", g_p.trials_done, a.out_dir, g_p.abort_trial);
        pl_close(&g_p);
        return 3;
    }
    if (a.measure) {
        printf("ysp_player: frame work over %lld frames: max %.1f us (frame %lld); trial start max %.1f us; %lld frames over 16 ms\n",
               (long long)g_p.work_n, (double)g_p.work_max_ns / 1e3, (long long)g_p.work_max_frame, (double)g_p.work_trial_max_ns / 1e3,
               (long long)g_p.work_over_16ms);
        if (!a.sim)
            printf("ysp_player: the input (SDL's pump and the collectors) max %.1f us (frame %lld)\n", (double)g_p.work_input_max_ns / 1e3,
                   (long long)g_p.work_input_max_frame);
    }
    printf("ysp_player: %d trials, %s, seed %llu, experiment %s\n", g_p.trials_done, a.out_dir, (unsigned long long)g_p.seed, g_p.experiment_id);
    pl_close(&g_p);
    return 0;
#undef a
}
