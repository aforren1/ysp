/* Compile check: ysp/response.h as a C translation unit with the
 * implementation. CMake and CI build this as C11 with warnings as errors;
 * response.cpp builds the same source as C++17. The header has no
 * threads. One press in a window returns 0 to prove it links and runs.
 * v0.4: the frame-loop and data helpers, with ysp/screen.h and
 * ysp/timeline.h for their structs (no implementation, no SDL) and
 * ysp/trials.h for the data writer. */
#include "ysp/screen.h"
#include "ysp/timeline.h"
#define YSP_TRIALS_IMPLEMENTATION
#include "ysp/trials.h"
#define YSP_RESPONSE_IMPLEMENTATION
#include "ysp/response.h"

static bool sink(void* ctx, const void* bytes, size_t n) { (void)bytes; *(size_t*)ctx += n; return true; }

#include <string.h>

/* The lifecycle and the changed entry points, by type. */
typedef struct rsp_api {
    bool (*open)(yrsp_collector*, const yrsp_desc*);
    void (*close)(yrsp_collector*);
    const char* (*error)(const yrsp_collector*);
    void (*arm)(yrsp_collector*, int64_t);
    int (*update)(yrsp_collector*, int64_t);
    const yrsp_param* (*params)(int*);   /* v0.6 */
    int (*set_sent)(yrsp_collector*, const yrsp_sent*, int, int);   /* v0.7 */
} rsp_api;

static const rsp_api g_api = { yrsp_open, yrsp_close, yrsp_error, yrsp_arm, yrsp_update, yrsp_params, yrsp_set_sent };

/* The unit macros are constant expressions. */
static const int64_t g_window[2] = { YRT_MS(100), YRT_S(1.5) };

int main(void) {
    static yrsp_collector c;
    yrsp_desc d;
    yrsp_onset o;
    yrsp_input in;
    yrsp_result r;
    memset(&d, 0, sizeof(d));
    memset(&o, 0, sizeof(o));
    memset(&in, 0, sizeof(in));
    d.min_rt_ns = g_window[0];
    d.duration_ns = g_window[1];
    d.max_hold_ns = YRT_S(2);
    if (!g_api.open(&c, &d) || g_api.error(&c)[0] != 0) return 1;
    g_api.arm(&c, 0);
    o.t_ns = 1000;
    o.final = 1;
    yrsp_set_onset(&c, &o);
    in.t = 1000 + YRT_MS(200);
    in.type = YRSP_PRESS;
    in.control = 9;
    if (yrsp_feed(&c, &in) != YRSP_ENDED) return 1;
    yrsp_finish(&c, &r);
    {   /* v0.4: a landing, its frame's record, the row */
        static ytr_data w;
        static const ytr_col cols[] = { YRSP_DATA_COLS };
        ytr_data_desc dd;
        ytl_event ev;
        yscr_record rec;
        yscr_frame fr;
        yrsp_result r2;
        size_t bytes = 0;
        memset(&ev, 0, sizeof(ev));
        memset(&rec, 0, sizeof(rec));
        memset(&fr, 0, sizeof(fr));
        memset(&dd, 0, sizeof(dd));
        yrsp_arm(&c, 0);
        ev.kind = YTL_ONSET; ev.target = 3; ev.frame = 9; ev.onset = 1000;
        if (yrsp_set_landing(&c, &ev, 1, 3) != 0 || yrsp_plan_frame(&c) != 9) return 1;
        rec.index = 9; rec.onset = 1200;
        fr.done = &rec; fr.n_done = 1; fr.onset = 2000;
        if (yrsp_update_frame(&c, &fr) != YRSP_OPEN || yrsp_plan_frame(&c) != -1) return 1;
        yrsp_finish(&c, &r2);
        dd.write = sink; dd.write_ctx = &bytes; dd.cols = cols; dd.n_cols = YRSP_N_DATA_COLS;
        if (!ytr_data_open(&w, &dd) || yrsp_data_set(&w, &r2) != YRSP_N_DATA_COLS || ytr_data_write(&w, NULL) != 0) return 1;
        ytr_data_close(&w);
        if (r2.t_onset != 1200 || bytes == 0) return 1;
    }
    g_api.close(&c);
    if (g_api.update(&c, 0) != YRSP_ERR_STATE) return 1;
    {   /* v0.6: the first row is the mode enum */
        int n = 0;
        const yrsp_param* p = g_api.params(&n);
        if (!p || n < 1 || strcmp(p[0].name, "mode") != 0 || strcmp(p[0].values, "all=0,list=1,none=2") != 0) return 1;
    }
    return r.t_response == in.t && r.rt_ns == YRT_MS(200) && r.rt_key_duration_ns == YRSP_NO_TIME
               && r.stamp_lo_ns == 0 && o.residual_ns == 0 && o.uncertainty_ns == 0 ? 0 : 1;
}
