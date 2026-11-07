/* Compile check: ysp/sig.h as a C translation unit with the implementation
 * enabled. CMake and CI build this as C11 with warnings as errors; sig.cpp
 * builds the same source as C++17. The header has no threads, so there is
 * no YSIG_NO_THREADS variant. It runs and returns 0 to prove it links. The
 * table names the API by type, so a signature change fails here and not in
 * a user's build. */
#define YSP_SIG_IMPLEMENTATION
#include "ysp/sig.h"

#include <string.h>

typedef struct sig_api {
    const char* (*version)(void);
    bool (*check)(const ysig_desc*, char*, size_t);
    size_t (*arena_size)(const ysig_desc*);
    bool (*open)(ysig_graph*, const ysig_desc*);
    const char* (*error)(const ysig_graph*);
    void (*reset)(ysig_graph*, int64_t);
    bool (*set)(ysig_graph*, int, int, double);
    bool (*set_data)(ysig_graph*, int, int, double);
    int (*feed)(ysig_graph*, const yin_event*);
    int (*frame)(ysig_graph*, const ysig_frame_in*);
    bool (*arm)(ysig_graph*, int, int64_t);
    bool (*disarm)(ysig_graph*, int);
    int (*detected)(ysig_graph*, ysig_hit*, int);
    int (*derived)(ysig_graph*, yin_event*, int);
    const ysig_value* (*value_of)(const ysig_graph*, int);
    int (*value_at)(const ysig_graph*, int, int64_t, ysig_value*);
    int (*adapter_node)(const ysig_graph*, int, int);
    void (*get_stats)(const ysig_graph*, ysig_stats*);
    int (*replay)(ysig_graph*, const ysig_entry*, int);
    const ysig_param* (*op_params)(int, int*);
    const char* (*op_name)(int);
} sig_api;

static const sig_api g_sig_api = {
    ysig_version, ysig_check, ysig_arena_size, ysig_open, ysig_error, ysig_reset, ysig_set, ysig_set_data,
    ysig_feed, ysig_frame, ysig_arm, ysig_disarm, ysig_detected, ysig_derived, ysig_value_of, ysig_value_at,
    ysig_adapter_node, ysig_get_stats, ysig_replay, ysig_op_params, ysig_op_name
};

static double g_arena[4096];

int main(void) {
    static ysig_graph g;
    ysig_node n[3];
    float ch[2];
    yin_event e;
    ysig_stats st;
    int np = 0;
    /* The zero-init rule (README, Conventions): { 0 } in C, {} in C++. */
#ifdef __cplusplus
    ysig_desc d = {};
    ysig_frame_in f = {};
#else
    ysig_desc d = { 0 };
    ysig_frame_in f = { 0 };
#endif
    n[0] = ysig_node_of(YSIG_OP_SOURCE, -1, -1);
    n[0].p[YSIG_SOURCE_KIND] = YIN_KIND_MOUSE;   /* the p[] names are int constants in C and C++ */
    n[0].p[YSIG_SOURCE_CONTROL] = YIN_AXIS_DELTA;
    n[0].lanes = 2;
    n[1] = ysig_node_of(YSIG_OP_INTEGRATE, 0, -1);
    n[1].p[YSIG_INTEGRATE_GAIN_X] = 1;
    n[2] = ysig_node_of(YSIG_OP_BIND, 1, -1);
    n[2].p[YSIG_BIND_CHANNEL] = 0;
    d.nodes = n;
    d.n_nodes = 3;
    d.channels = ch;
    d.n_channels = 2;
    d.arena = g_arena;
    d.arena_size = g_sig_api.arena_size(&d);
    if (!d.arena_size || d.arena_size > sizeof g_arena || !g_sig_api.check(&d, NULL, 0)) return 1;
    if (!g_sig_api.open(&g, &d)) return 2;
    g_sig_api.reset(&g, 0);
    memset(&e, 0, sizeof e);
    e.t = 1000;
    e.kind = YIN_KIND_MOUSE;
    e.type = YIN_SAMPLE;
    e.control = YIN_AXIS_DELTA;
    e.x = 3;
    if (g_sig_api.feed(&g, &e) != 1) return 3;
    f.onset_ns = 2000;
    f.poll_ns = 2000;   /* input is known up to here */
    f.has_poll = true;
    if (g_sig_api.frame(&g, &f) != 0 || ch[0] != 3.0f) return 4;
    g_sig_api.get_stats(&g, &st);
    if (st.taken != 1 || !g_sig_api.op_params(YSIG_OP_INTEGRATE, &np) || np != 16) return 5;
    if (strcmp(g_sig_api.version(), YSIG_VERSION_STRING) || strcmp(g_sig_api.op_name(YSIG_OP_BIND), "bind")) return 6;
    if (g_sig_api.value_of(&g, 1)->v[0] != 3 || g_sig_api.value_at(&g, 1, 0, NULL) != -1) return 7;
    return g_sig_api.error(&g)[0] == '\0' ? 0 : 8;
}
