/* Compile check: ysp/response.h as a C translation unit with the
 * implementation. CMake and CI build this as C11 with warnings as errors;
 * response.cpp builds the same source as C++17. The header has no
 * threads. One press in a window returns 0 to prove it links and runs. */
#define YSP_RESPONSE_IMPLEMENTATION
#include "ysp/response.h"

#include <string.h>

/* The lifecycle and the changed entry points, by type. */
typedef struct rsp_api {
    bool (*open)(yrsp_collector*, const yrsp_desc*);
    void (*close)(yrsp_collector*);
    const char* (*error)(const yrsp_collector*);
    void (*arm)(yrsp_collector*, int64_t);
    int (*update)(yrsp_collector*, int64_t);
} rsp_api;

static const rsp_api g_api = { yrsp_open, yrsp_close, yrsp_error, yrsp_arm, yrsp_update };

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
    g_api.close(&c);
    if (g_api.update(&c, 0) != YRSP_ERR_STATE) return 1;
    return r.t_response == in.t && r.rt_ns == YRT_MS(200) && r.rt_key_duration_ns == YRSP_NO_TIME
               && r.stamp_lo_ns == 0 && o.residual_ns == 0 && o.uncertainty_ns == 0 ? 0 : 1;
}
