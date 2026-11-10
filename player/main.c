/* player/main.c - the ysp_player command line.
 *
 * Usage: ysp_player PACK --out DIR --sim [--seed N] [--field NAME=VALUE]...
 *                   [--rig FILE] [--cal FILE] [--participant SPEC] [--rt S]
 *                   [--max-trials N] [--measure] [--trace-channels FILE]
 *                   [--rdk-buffer N]
 *
 * --sim is required in this version: the simulated display, the virtual
 * clock, the null audio device and a scripted participant. SPEC is
 * "correct" (the default: each trial's key_answer after --rt seconds),
 * "silent", "p=0.8" (correct with that probability), or "script:FILE" (a
 * CSV of trial_index,response,rt_s,hold_s). DIR must not exist.
 * --rdk-buffer sets an RDK's step buffer (tests: a small one flushes often).
 *
 * Exit code: 0, 1 when the pack or the definition is refused or the run
 * failed (the message goes to stderr), 2 for a bad argument.
 */
#include "player.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static pl_player g_p;

static int usage(void) {
    fprintf(stderr, "usage: ysp_player PACK --out DIR --sim [--seed N] [--field NAME=VALUE]... [--rig FILE] [--cal FILE]\n"
                    "                  [--participant SPEC] [--rt S] [--max-trials N] [--max-seconds N]\n"
                    "  SPEC: correct, silent, p=0.8 or script:FILE\n");
    return 2;
}

int main(int argc, char** argv) {
    pl_args a;
    int i, rc;
    memset(&a, 0, sizeof a);
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
    if (!a.sim) { fprintf(stderr, "ysp_player: this version runs only with --sim\n"); return 2; }
    /* --sim runs on the virtual clock from before anything reads it, so the
     * records are the same on every host and every run */
    yrt_virtual_begin(0);
    if (pl_load(&g_p, &a) < 0) { fprintf(stderr, "ysp_player: %s\n", g_p.error); pl_close(&g_p); return 1; }
    rc = pl_run(&g_p);
    if (rc < 0) { fprintf(stderr, "ysp_player: %s\n", g_p.error); pl_close(&g_p); return 1; }
    if (a.measure) {
        printf("ysp_player: frame work over %lld frames: max %.1f us; trial start max %.1f us; %lld frames over 16 ms\n", (long long)g_p.work_n,
               (double)g_p.work_max_ns / 1e3, (double)g_p.work_trial_max_ns / 1e3, (long long)g_p.work_over_16ms);
    }
    printf("ysp_player: %d trials, %s, seed %llu, experiment %s\n", g_p.trials_done, a.out_dir, (unsigned long long)g_p.seed, g_p.experiment_id);
    pl_close(&g_p);
    return 0;
}
