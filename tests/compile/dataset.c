/* Compile check: ysp/dataset.h as a C translation unit with the
 * implementation enabled. CMake and CI build this as C11 with warnings as
 * errors; dataset.cpp builds the same source as C++17. The header has no
 * threads, so there is no YDS_NO_THREADS variant. It runs and returns 0 to
 * prove it links, and makes no file: the calls below fail before the
 * folder. The table names every call by type, so a signature change fails
 * here and not in a user's build. ysp/screen.h, ysp/input.h,
 * ysp/timeline.h and ysp/rt.h come first so the adapters compile too
 * (declarations only: nothing of theirs is linked). */
#include "ysp/rt.h"
#include "ysp/input.h"
#include "ysp/timeline.h"
#include "ysp/screen.h"
#define YSP_DATASET_IMPLEMENTATION
#include "ysp/dataset.h"

#include <string.h>

typedef struct dataset_api {
    bool (*open)(yds_dataset*, const yds_desc*);
    const char* (*error)(const yds_dataset*);
    int (*write_trial)(yds_dataset*, const void*);
    int (*write_frame)(yds_dataset*, const yds_frame*);
    int (*write_event)(yds_dataset*, const yds_event*);
    int (*write_clock)(yds_dataset*, const yds_clock*);
    int (*stream)(yds_dataset*, const char*, const ytr_col*, int);
    int (*write_stream)(yds_dataset*, int, const void*);
    ytr_data* (*trials_writer)(yds_dataset*);
    ytr_data* (*stream_writer)(yds_dataset*, int);
    int (*session_set)(yds_dataset*, const char*, const yjs_value*);
    int (*session_push)(yds_dataset*, const char*, const yjs_value*);
    const yjs_value* (*session)(const yds_dataset*);
    int64_t (*rows)(const yds_dataset*, int);
    int (*finish)(yds_dataset*);
    void (*close)(yds_dataset*);
    void (*abandon)(yds_dataset*);
    int (*zip)(yds_dataset*, const char*, ytr_write_fn, void*);
} dataset_api;

static const dataset_api g_api = {
    yds_open, yds_error, yds_write_trial, yds_write_frame, yds_write_event, yds_write_clock,
    yds_stream, yds_write_stream, yds_trials_writer, yds_stream_writer, yds_session_set,
    yds_session_push, yds_session, yds_rows, yds_finish, yds_close, yds_abandon, yds_zip
};

typedef struct check_row {
    int     key;
    int64_t rt_ns;
} check_row;

static const ytr_col check_cols[] = {
    YTR_COL(check_row, key, YTR_COL_INT),
    YTR_COL(check_row, rt_ns, YTR_COL_TIME_NS),
    YTR_COL_TEXT("participant", "P01"),
};

int main(void) {
    static yds_dataset ds;
    yds_desc d;
    yds_frame f;
    yds_event e;
    yds_clock c;
    yscr_record r;
    yin_event in;
    ytl_event te;
    yrt_fit_info fi;
    memset(&d, 0, sizeof d);
    d.trial_cols = check_cols;
    d.n_trial_cols = (int)(sizeof(check_cols) / sizeof(check_cols[0]));
    if (g_api.open(&ds, &d)) return 1;                  /* no dir */
    if (g_api.error(&ds)[0] == '\0' || ds.error_code != YDS_ERR_ARG) return 2;
    if (g_api.write_trial(&ds, NULL) != YDS_ERR_CLOSED) return 3;
    if (g_api.zip(&ds, "x.zip", NULL, NULL) != YDS_ERR_ORDER) return 4;
    if (g_api.trials_writer(&ds) || g_api.stream_writer(&ds, 0) || g_api.session(&ds)) return 5;
    if (g_api.rows(&ds, YDS_TABLE_FRAMES) != 0) return 6;
    g_api.close(&ds);
    g_api.close(&ds);
    g_api.abandon(&ds);
    g_api.abandon(&ds);
    (void)g_api.write_frame; (void)g_api.write_event; (void)g_api.write_clock; (void)g_api.stream;
    (void)g_api.write_stream; (void)g_api.session_set; (void)g_api.session_push; (void)g_api.finish;

    memset(&r, 0, sizeof r);
    r.index = 7;
    r.onset = 1000;
    r.phase_ns[YSCR_PHASE_GPU] = YSCR_PHASE_UNKNOWN;
    yds_frame_from_screen(&f, &r, 3);
    if (f.frame != 7 || f.trial_index != 3 || f.gpu_ns != YTR_MISSING_NS || f.draw_ns != 0) return 7;
    memset(&in, 0, sizeof in);
    in.kind = (uint8_t)YIN_KIND_MOUSE;
    yds_event_from_input(&e, &in, "resp", 0);
    if (strcmp(e.kind, "mouse") != 0 || e.ticks != YTR_MISSING_I64) return 8;
    memset(&te, 0, sizeof te);
    te.kind = (uint8_t)YTL_ONSET;
    te.frame = -1;
    yds_event_from_timeline(&e, &te, "gabor", 0);
    if (strcmp(e.kind, "onset") != 0 || e.frame != YTR_MISSING_I64) return 9;
    memset(&fi, 0, sizeof fi);
    yds_clock_from_fit(&c, &fi, "photo");
    if (strcmp(c.kind, "fit") != 0) return 10;
    return 0;
}
