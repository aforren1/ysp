/* dataset_session.c - a session's data set with ysp/dataset.h (v0.1).
 *
 * A simulated 2AFC session with no display and no device: 20 trials of a
 * fixation, a stimulus and a key, on a 60 Hz clock that the program counts
 * itself. It writes what the player writes (docs/experiment_format.md
 * 2.10): session.json, one trial row per trial, one frame row per flip,
 * the stimulus onsets and the keys in events.csv, a clock pair and a fit
 * in clock.csv, and the dot field's steps in streams/dots.csv. Then it
 * finishes the data set, zips it (the web form), and reads trials.csv back
 * with ysp/table.h to check every row.
 *
 * Nothing here touches hardware and nothing is a timing measurement.
 *
 * Build (from the repository root):
 *     cc -O2 -Iinclude -o dataset_session examples/dataset/session.c -lm
 *     cl /O2 /Iinclude examples\dataset\session.c
 *
 * Usage: dataset_session [--out DIR] [--no-zip]
 *   DIR must not exist (default dataset_session_out); the zip is DIR.zip.
 * Exit code: 0; 1 if the data set could not be written (DIR exists, say)
 * or the read-back found a mismatch; 2 for a usage error.
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "ysp/screen.h"   /* yscr_record, for yds_frame_from_screen() */
#define YTB_STDIO         /* ytb_open_csv_file(), for the read-back */
#define YSP_DATASET_IMPLEMENTATION
#include "ysp/dataset.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FRAME_NS 16666667LL
#define N_TRIALS 20

typedef struct trial_row {
    int         trial_index;
    const char* side;
    const char* response;
    bool        correct;
    int64_t     rt_ns;
    double      contrast;
} trial_row;

static const ytr_col trial_cols[] = {
    YTR_COL_TEXT("participant", "P01"),
    YTR_COL(trial_row, trial_index, YTR_COL_INT),
    YTR_COL(trial_row, side, YTR_COL_STRING),
    YTR_COL(trial_row, contrast, YTR_COL_DOUBLE),
    YTR_COL(trial_row, response, YTR_COL_STRING),
    YTR_COL(trial_row, correct, YTR_COL_BOOL),
    YTR_COL(trial_row, rt_ns, YTR_COL_TIME_NS),
};

typedef struct step_row {
    int64_t t_ns;
    int     trial_index;
    int     update;
} step_row;

static const ytr_col step_cols[] = {
    YTR_COL(step_row, t_ns, YTR_COL_TIME_NS),
    YTR_COL(step_row, trial_index, YTR_COL_INT),
    YTR_COL(step_row, update, YTR_COL_INT),
};

static uint64_t g_rng = 20261021u;
static uint32_t rnd(void) {
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17;
    return (uint32_t)(g_rng >> 32);
}

int main(int argc, char** argv) {
    static yds_dataset ds;
    static ytb_table back;
    static uint64_t back_arena[1 << 14];
    const char* out = "dataset_session_out";
    bool zip = true;
    char zip_path[1024];
    yds_desc d;
    yjs_arena a;
    yjs_value *s, *fields;
    int i, f, dots, bad = 0;
    int64_t t = 1000000000LL, frame = 0;
    trial_row rows[N_TRIALS];

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) out = argv[++i];
        else if (strcmp(argv[i], "--no-zip") == 0) zip = false;
        else {
            fprintf(stderr, "usage: dataset_session [--out DIR] [--no-zip]\n");
            return 2;
        }
    }

    /* The player's keys of session.json: the writer adds its own. */
    yjs_arena_init_heap(&a, 0);
    s = yjs_new(&a, YJS_OBJECT);
    fields = yjs_new(&a, YJS_OBJECT);
    yjs_set(&a, fields, "participant", yjs_new_stringz(&a, "P01"));
    yjs_set(&a, s, "fields", fields);
    yjs_set(&a, s, "experiment", yjs_new_stringz(&a, "simulated 2AFC, no pack"));
    yjs_set(&a, s, "seed", yjs_new_int(&a, (int64_t)g_rng));
    yjs_set(&a, s, "refresh_hz", yjs_new_double(&a, 1e9 / (double)FRAME_NS));

    memset(&d, 0, sizeof d);
    d.dir = out;
    d.session = s;
    d.trial_cols = trial_cols;
    d.n_trial_cols = (int)(sizeof trial_cols / sizeof trial_cols[0]);
    if (!yds_open(&ds, &d)) {
        fprintf(stderr, "%s\n", yds_error(&ds));
        yjs_arena_free(&a);
        return 1;
    }
    yjs_arena_free(&a);   /* the data set has its copy */
    dots = yds_stream(&ds, "dots", step_cols, 3);
    if (dots < 0) fprintf(stderr, "%s\n", yds_error(&ds));

    {
        yds_clock c;
        yds_clock_init(&c);
        c.role = "photo";
        c.kind = "pair";
        c.t_ns = t;
        c.ticks = 0;
        c.width_ns = 40000;
        yds_write_clock(&ds, &c);
    }

    for (i = 0; i < N_TRIALS; i++) {
        trial_row* r = &rows[i];
        int64_t onset = 0;
        r->trial_index = i;
        r->side = (rnd() & 1u) ? "right" : "left";
        r->contrast = 0.02 * (1 + i % 5);
        /* 30 frames of fixation, then the stimulus for 12 frames. */
        for (f = 0; f < 42; f++, frame++) {
            yscr_record rec;
            yds_frame fr;
            memset(&rec, 0, sizeof rec);
            rec.index = frame;
            rec.target = t + frame * FRAME_NS;
            rec.planned = rec.target;
            rec.onset = rec.target + 30000 + (int64_t)(rnd() % 4000u);
            rec.residual = rec.onset - rec.target;
            rec.path = 1;
            rec.tier = 2;
            rec.phase_ns[YSCR_PHASE_DRAW] = 200000u + rnd() % 50000u;
            rec.phase_ns[YSCR_PHASE_GPU] = YSCR_PHASE_UNKNOWN;
            yds_frame_from_screen(&fr, &rec, i);
            if (yds_write_frame(&ds, &fr) < 0) bad = 1;
            if (f == 30) {
                yds_event e;
                yds_event_init(&e);
                onset = rec.onset;
                e.t_ns = onset;
                e.trial_index = i;
                e.kind = "onset";
                e.stimulus = "gabor";
                e.frame = frame;
                e.planned_ns = rec.target;
                e.residual_ns = rec.residual;
                if (yds_write_event(&ds, &e) < 0) bad = 1;
            }
            if (f >= 30 && dots >= 0) {
                step_row st;
                st.t_ns = rec.onset;
                st.trial_index = i;
                st.update = f - 30;
                if (yds_write_stream(&ds, dots, &st) < 0) bad = 1;
            }
        }
        /* The simulated observer: more often correct at higher contrast. */
        r->correct = (int)(rnd() % 100u) < 55 + i % 5 * 10;
        r->response = r->correct ? r->side : (strcmp(r->side, "left") == 0 ? "right" : "left");
        r->rt_ns = i == 7 ? YTR_MISSING_NS : 300000000LL + (int64_t)(rnd() % 200000000u);
        if (i == 7) r->response = NULL;   /* a missed response: empty fields */
        if (r->response) {
            yds_event e;
            yds_event_init(&e);
            e.t_ns = onset + r->rt_ns;
            e.trial_index = i;
            e.kind = "response";
            e.role = "resp";
            e.text = r->response;
            if (yds_write_event(&ds, &e) < 0) bad = 1;
        }
        if (yds_write_trial(&ds, r) < 0) {
            fprintf(stderr, "%s\n", yds_error(&ds));
            bad = 1;
        }
        t += 60 * FRAME_NS;   /* the inter-trial interval */
    }
    {
        yds_clock c;
        yds_clock_init(&c);
        c.role = "photo";
        c.kind = "fit";
        c.t_ns = 1000000000LL;
        c.ticks = 0;
        c.ns_per_tick = 1000.0;
        c.ppm = 0.0;
        c.points = 2;
        c.epoch = 0;
        yds_write_clock(&ds, &c);
    }

    if (yds_finish(&ds) < 0) {
        fprintf(stderr, "%s\n", yds_error(&ds));
        bad = 1;
    }
    if (zip) {
        snprintf(zip_path, sizeof zip_path, "%s.zip", out);
        if (yds_zip(&ds, zip_path, NULL, NULL) < 0) {
            fprintf(stderr, "%s\n", yds_error(&ds));
            bad = 1;
        }
    }
    yds_close(&ds);

    /* The read-back: every trial row as written. */
    {
        ytb_csv_desc cd;
        char path[1100];
        memset(&cd, 0, sizeof cd);
        cd.allow_empty = true;
        cd.arena = back_arena;
        cd.arena_size = sizeof back_arena;
        snprintf(path, sizeof path, "%s/trials.csv", out);
        if (!ytb_open_csv_file(&back, path, &cd)) {
            fprintf(stderr, "read-back: %s\n", ytb_error(&back));
            return 1;
        }
        if (back.n_rows != N_TRIALS) bad = 1;
        for (i = 0; i < back.n_rows && i < N_TRIALS; i++) {
            double rt = ytb_num(&back, i, ytb_col(&back, "rt_s"));
            const char* resp = ytb_text(&back, i, ytb_col(&back, "response"));
            if (rows[i].rt_ns == YTR_MISSING_NS ? !isnan(rt) : fabs(rt - (double)rows[i].rt_ns * 1e-9) > 1e-12) bad = 1;
            if (strcmp(resp ? resp : "", rows[i].response ? rows[i].response : "") != 0) bad = 1;
        }
    }
    printf("%s: %d trials, %lld frames%s; read back %s\n", out, N_TRIALS, (long long)frame,
           zip ? ", zipped" : "", bad ? "WITH MISMATCHES" : "as written");
    return bad ? 1 : 0;
}
